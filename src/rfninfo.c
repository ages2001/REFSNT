/*
 * REFSNT - IRP_MJ_QUERY_INFORMATION
 */

#include "rfn.h"

/* The FCB keeps the NT attribute bits already */
ULONG
RfnNtAttributes (
    ULONG Attributes
    )
{
    ULONG Nt = Attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                             FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_ARCHIVE);

    if (Nt == 0) {
        Nt = FILE_ATTRIBUTE_NORMAL;
    }

    return Nt;
}

VOID
RfnFillBasicInfo (
    PRFN_FCB Fcb,
    PFILE_BASIC_INFORMATION Info
    )
{
    Info->CreationTime = Fcb->CreationTime;
    Info->LastAccessTime = Fcb->LastAccessTime;
    Info->LastWriteTime = Fcb->LastWriteTime;
    Info->ChangeTime = Fcb->ChangeTime;
    Info->FileAttributes = RfnNtAttributes(Fcb->Attributes);
}

VOID
RfnFillStandardInfo (
    PRFN_FCB Fcb,
    PFILE_STANDARD_INFORMATION Info
    )
{
    /* directories count their subdirectories' ".." too; NT wants 1 there */
    Info->NumberOfLinks = RfnIsDcb(Fcb) ? 1 : (Fcb->Inode.LinksCount ? Fcb->Inode.LinksCount : 1);
    Info->DeletePending = (BOOLEAN)((Fcb->FcbState & FCB_STATE_DELETE_PENDING) != 0);

    if (RfnIsDcb(Fcb)) {
        Info->AllocationSize.QuadPart = 0;
        Info->EndOfFile.QuadPart = 0;
        Info->Directory = TRUE;
    } else {
        Info->AllocationSize = Fcb->Header.AllocationSize;
        Info->EndOfFile = Fcb->Header.FileSize;
        Info->Directory = FALSE;
    }
}

VOID
RfnFillNetworkOpenInfo (
    PRFN_FCB Fcb,
    PFILE_NETWORK_OPEN_INFORMATION Info
    )
{
    FILE_STANDARD_INFORMATION Standard;

    RfnFillStandardInfo(Fcb, &Standard);

    Info->CreationTime = Fcb->CreationTime;
    Info->LastAccessTime = Fcb->LastAccessTime;
    Info->LastWriteTime = Fcb->LastWriteTime;
    Info->ChangeTime = Fcb->ChangeTime;
    Info->AllocationSize = Standard.AllocationSize;
    Info->EndOfFile = Standard.EndOfFile;
    Info->FileAttributes = RfnNtAttributes(Fcb->Attributes);
}

/*
 * Copies as much of "\dir\file" as fits in Room bytes and returns the
 * full length in bytes.
 */
static NTSTATUS
RfnCopyPath (
    PRFN_FCB Fcb,
    PWSTR Target,
    ULONG Room,
    PULONG FullLength,
    PULONG Copied
    )
{
    PRFN_FCB Walk;
    ULONG Length = 0;
    PWSTR Path;
    PWSTR Write;

    if (Fcb->ParentDcb == NULL) {
        Length = sizeof(WCHAR);
    } else {
        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Length += sizeof(WCHAR) + Walk->Name.Length;
        }
    }

    Path = (PWSTR)ExAllocatePoolWithTag(PagedPool, Length, RFN_TAG_NAME);
    if (Path == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Fcb->ParentDcb == NULL) {

        Path[0] = L'\\';

    } else {

        Write = (PWSTR)((PUCHAR)Path + Length);

        for (Walk = Fcb; Walk->ParentDcb != NULL; Walk = Walk->ParentDcb) {
            Write = (PWSTR)((PUCHAR)Write - Walk->Name.Length);
            RtlCopyMemory(Write, Walk->Name.Buffer, Walk->Name.Length);
            Write--;
            *Write = L'\\';
        }
    }

    *FullLength = Length;
    *Copied = (Length < Room) ? Length : Room;

    RtlCopyMemory(Target, Path, *Copied);
    ExFreePool(Path);

    return STATUS_SUCCESS;
}

static NTSTATUS
RfnQueryNameInfo (
    PRFN_FCB Fcb,
    PFILE_NAME_INFORMATION Info,
    ULONG Length,
    PULONG Used
    )
{
    ULONG Base = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName);
    ULONG FullLength;
    ULONG Copied;
    NTSTATUS Status;

    if (Length < Base) {
        return STATUS_INFO_LENGTH_MISMATCH;
    }

    Status = RfnCopyPath(Fcb, Info->FileName, Length - Base, &FullLength, &Copied);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    Info->FileNameLength = FullLength;
    *Used = Base + Copied;

    return (Copied < FullLength) ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
}

/* Handles opened on the volume itself */
static NTSTATUS
RfnQueryVolumeFileInfo (
    PRFN_IRP_CONTEXT Ctx,
    PRFN_FCB Vfcb,
    FILE_INFORMATION_CLASS Class,
    PVOID Buffer,
    ULONG Length
    )
{
    PRFN_VCB Vcb = Vfcb->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PFILE_STANDARD_INFORMATION Standard;
    PFILE_BASIC_INFORMATION Basic;

    switch (Class) {

    case FilePositionInformation:
        ((PFILE_POSITION_INFORMATION)Buffer)->CurrentByteOffset = FileObject->CurrentByteOffset;
        Ctx->Irp->IoStatus.Information = sizeof(FILE_POSITION_INFORMATION);
        return STATUS_SUCCESS;

    case FileStandardInformation:
        Standard = (PFILE_STANDARD_INFORMATION)Buffer;
        RtlZeroMemory(Standard, sizeof(FILE_STANDARD_INFORMATION));
        Standard->AllocationSize.QuadPart = Vcb->PartitionBytes;
        Standard->EndOfFile.QuadPart = Vcb->PartitionBytes;
        Standard->NumberOfLinks = 1;
        Ctx->Irp->IoStatus.Information = sizeof(FILE_STANDARD_INFORMATION);
        return STATUS_SUCCESS;

    case FileBasicInformation:
        Basic = (PFILE_BASIC_INFORMATION)Buffer;
        RtlZeroMemory(Basic, sizeof(FILE_BASIC_INFORMATION));
        Basic->FileAttributes = FILE_ATTRIBUTE_NORMAL;
        Ctx->Irp->IoStatus.Information = sizeof(FILE_BASIC_INFORMATION);
        return STATUS_SUCCESS;

    default:
        break;
    }

    UNREFERENCED_PARAMETER(Length);
    return STATUS_INVALID_PARAMETER;
}

NTSTATUS
RfnCommonQueryInformation (
    PRFN_IRP_CONTEXT Ctx
    )
{
    PIRP Irp = Ctx->Irp;
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    PRFN_FCB Fcb = (PRFN_FCB)FileObject->FsContext;
    PRFN_VCB Vcb = Ctx->Vcb;
    FILE_INFORMATION_CLASS Class = IrpSp->Parameters.QueryFile.FileInformationClass;
    ULONG Length = IrpSp->Parameters.QueryFile.Length;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    PFILE_ALL_INFORMATION All;
    PRFN_FILE_ATTRIBUTE_TAG_INFORMATION Tag;
    ULONG Used = 0;
    ULONG Base;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Fcb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (RfnIsVfcb(Fcb)) {
        return RfnQueryVolumeFileInfo(Ctx, Fcb, Class, Buffer, Length);
    }

    (VOID)ExAcquireResourceSharedLite(&Vcb->Resource, TRUE);
    (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);

    __try {

        Status = RfnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        switch ((ULONG)Class) {

        case FileBasicInformation:
            RfnFillBasicInfo(Fcb, (PFILE_BASIC_INFORMATION)Buffer);
            Used = sizeof(FILE_BASIC_INFORMATION);
            break;

        case FileStandardInformation:
            RfnFillStandardInfo(Fcb, (PFILE_STANDARD_INFORMATION)Buffer);
            Used = sizeof(FILE_STANDARD_INFORMATION);
            break;

        case FileInternalInformation:
            ((PFILE_INTERNAL_INFORMATION)Buffer)->IndexNumber.QuadPart = Fcb->Ino;
            Used = sizeof(FILE_INTERNAL_INFORMATION);
            break;

        case FileEaInformation:
            ((PFILE_EA_INFORMATION)Buffer)->EaSize = RfnNtEaSize(Vcb, Fcb);
            Used = sizeof(FILE_EA_INFORMATION);
            break;

        case FilePositionInformation:
            ((PFILE_POSITION_INFORMATION)Buffer)->CurrentByteOffset = FileObject->CurrentByteOffset;
            Used = sizeof(FILE_POSITION_INFORMATION);
            break;

        case FileNameInformation:
            Status = RfnQueryNameInfo(Fcb, (PFILE_NAME_INFORMATION)Buffer, Length, &Used);
            break;

        case FileAllInformation:
            All = (PFILE_ALL_INFORMATION)Buffer;
            Base = FIELD_OFFSET(FILE_ALL_INFORMATION, NameInformation);

            if (Length < Base + FIELD_OFFSET(FILE_NAME_INFORMATION, FileName)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }

            RtlZeroMemory(All, Base);
            RfnFillBasicInfo(Fcb, &All->BasicInformation);
            RfnFillStandardInfo(Fcb, &All->StandardInformation);
            All->InternalInformation.IndexNumber.QuadPart = Fcb->Ino;
            All->EaInformation.EaSize = RfnNtEaSize(Vcb, Fcb);
            All->PositionInformation.CurrentByteOffset = FileObject->CurrentByteOffset;
            All->AlignmentInformation.AlignmentRequirement =
                Ctx->DeviceObject->AlignmentRequirement;

            Status = RfnQueryNameInfo(Fcb, &All->NameInformation, Length - Base, &Used);
            Used += Base;
            break;

        case FileNetworkOpenInformation:
            RfnFillNetworkOpenInfo(Fcb, (PFILE_NETWORK_OPEN_INFORMATION)Buffer);
            Used = sizeof(FILE_NETWORK_OPEN_INFORMATION);
            break;

        case RFN_CLASS_ATTRIBUTE_TAG:
            if (Length < sizeof(RFN_FILE_ATTRIBUTE_TAG_INFORMATION)) {
                Status = STATUS_INFO_LENGTH_MISMATCH;
                break;
            }
            Tag = (PRFN_FILE_ATTRIBUTE_TAG_INFORMATION)Buffer;
            Tag->FileAttributes = RfnNtAttributes(Fcb->Attributes);
            Tag->ReparseTag = 0;
            Used = sizeof(RFN_FILE_ATTRIBUTE_TAG_INFORMATION);
            break;

        case FileAlternateNameInformation:
            /* ext has no short names */
            Status = STATUS_OBJECT_NAME_NOT_FOUND;
            break;

        default:
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        if (NT_SUCCESS(Status) || Status == STATUS_BUFFER_OVERFLOW) {
            Irp->IoStatus.Information = Used;
        }

    } __finally {

        RfnRelease(&Fcb->Resource);
        RfnRelease(&Vcb->Resource);
    }

    return Status;
}

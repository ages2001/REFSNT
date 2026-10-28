/*
 * REFSNT - fast I/O and cache manager callbacks
 */

#include "rfn.h"

static BOOLEAN
NTAPI
RfnFastIoNoWrite (
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    PVOID Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(FileOffset);
    UNREFERENCED_PARAMETER(Length);
    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(LockKey);
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);
    return FALSE;
}

UCHAR
RfnIsFastIoPossible (
    PRFN_FCB Fcb
    )
{
    if (Fcb->Vcb->VcbState & VCB_STATE_DISMOUNTED) {
        return FastIoIsNotPossible;
    }

    if (FsRtlAreThereCurrentFileLocks(&Fcb->FileLock)) {
        return FastIoIsQuestionable;
    }

    return FastIoIsPossible;
}

static BOOLEAN
RfnFastIoUsable (
    PRFN_FCB Fcb
    )
{
    if (Fcb == NULL || (!RfnIsFcb(Fcb) && !RfnIsDcb(Fcb))) {
        return FALSE;
    }

    if ((Fcb->Vcb->VcbState & VCB_STATE_DISMOUNTED) ||
        (Fcb->Vcb->Vpb->RealDevice->Flags & DO_VERIFY_VOLUME)) {

        return FALSE;
    }

    return TRUE;
}

static BOOLEAN
NTAPI
RfnFastIoCheckIfPossible (
    PFILE_OBJECT FileObject,
    PLARGE_INTEGER FileOffset,
    ULONG Length,
    BOOLEAN Wait,
    ULONG LockKey,
    BOOLEAN CheckForReadOperation,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PRFN_FCB Fcb = (PRFN_FCB)FileObject->FsContext;
    LARGE_INTEGER LargeLength;

    UNREFERENCED_PARAMETER(Wait);
    UNREFERENCED_PARAMETER(IoStatus);
    UNREFERENCED_PARAMETER(DeviceObject);

    if (!RfnFastIoUsable(Fcb) || !RfnIsFcb(Fcb)) {
        return FALSE;
    }

    LargeLength.QuadPart = Length;

    if (CheckForReadOperation) {
        return FsRtlFastCheckLockForRead(&Fcb->FileLock, FileOffset, &LargeLength, LockKey,
                                         FileObject, IoGetCurrentProcess());
    }

    if (Fcb->Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return FALSE;
    }

    return FsRtlFastCheckLockForWrite(&Fcb->FileLock, FileOffset, &LargeLength, LockKey,
                                      FileObject, IoGetCurrentProcess());
}

static BOOLEAN
NTAPI
RfnFastQueryBasicInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_BASIC_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PRFN_FCB Fcb = (PRFN_FCB)FileObject->FsContext;
    FILE_BASIC_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!RfnFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    RfnFillBasicInfo(Fcb, &Info);

    RfnRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_BASIC_INFORMATION);

    return TRUE;
}

static BOOLEAN
NTAPI
RfnFastQueryStandardInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_STANDARD_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PRFN_FCB Fcb = (PRFN_FCB)FileObject->FsContext;
    FILE_STANDARD_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!RfnFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    RfnFillStandardInfo(Fcb, &Info);

    RfnRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_STANDARD_INFORMATION);

    return TRUE;
}

static BOOLEAN
NTAPI
RfnFastQueryNetworkOpenInfo (
    PFILE_OBJECT FileObject,
    BOOLEAN Wait,
    PFILE_NETWORK_OPEN_INFORMATION Buffer,
    PIO_STATUS_BLOCK IoStatus,
    PDEVICE_OBJECT DeviceObject
    )
{
    PRFN_FCB Fcb = (PRFN_FCB)FileObject->FsContext;
    FILE_NETWORK_OPEN_INFORMATION Info;

    UNREFERENCED_PARAMETER(DeviceObject);

    if (!RfnFastIoUsable(Fcb)) {
        return FALSE;
    }

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    RfnFillNetworkOpenInfo(Fcb, &Info);

    RfnRelease(&Fcb->Resource);
    FsRtlExitFileSystem();

    *Buffer = Info;
    IoStatus->Status = STATUS_SUCCESS;
    IoStatus->Information = sizeof(FILE_NETWORK_OPEN_INFORMATION);

    return TRUE;
}

#ifdef RFN_NT31
/*
 * NT 3.1 calls the fast I/O routines without the DeviceObject parameter that
 * NT 3.5 added: these routines pop what it pushes (stdcall), or the caller's
 * stack - and the registers it restores from it - would be off by four bytes.
 */
static BOOLEAN NTAPI
RfnFastIoCheckIfPossible31 (PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length, BOOLEAN Wait,
                            ULONG LockKey, BOOLEAN CheckForReadOperation, PIO_STATUS_BLOCK IoStatus)
{
    return RfnFastIoCheckIfPossible(FileObject, FileOffset, Length, Wait, LockKey, CheckForReadOperation, IoStatus, NULL);
}

static BOOLEAN NTAPI
RfnFastIoNoWrite31 (PFILE_OBJECT FileObject, PLARGE_INTEGER FileOffset, ULONG Length, BOOLEAN Wait,
                    ULONG LockKey, PVOID Buffer, PIO_STATUS_BLOCK IoStatus)
{
    return RfnFastIoNoWrite(FileObject, FileOffset, Length, Wait, LockKey, Buffer, IoStatus, NULL);
}

static BOOLEAN NTAPI
RfnFastQueryBasicInfo31 (PFILE_OBJECT FileObject, BOOLEAN Wait, PFILE_BASIC_INFORMATION Buffer, PIO_STATUS_BLOCK IoStatus)
{
    return RfnFastQueryBasicInfo(FileObject, Wait, Buffer, IoStatus, NULL);
}

static BOOLEAN NTAPI
RfnFastQueryStandardInfo31 (PFILE_OBJECT FileObject, BOOLEAN Wait, PFILE_STANDARD_INFORMATION Buffer, PIO_STATUS_BLOCK IoStatus)
{
    return RfnFastQueryStandardInfo(FileObject, Wait, Buffer, IoStatus, NULL);
}
#endif

VOID
RfnInitializeFastIo (
    PFAST_IO_DISPATCH FastIo
    )
{
    RtlZeroMemory(FastIo, sizeof(FAST_IO_DISPATCH));

    FastIo->SizeOfFastIoDispatch = sizeof(FAST_IO_DISPATCH);
    FastIo->FastIoCheckIfPossible = RfnFastIoCheckIfPossible;
    FastIo->FastIoRead = FsRtlCopyRead;
    /* Writes take the IRP path: blocks are found for the data before it
       enters the cache (holes, inline data), so a full disk is reported to
       the writer and not to the lazy writer */
    FastIo->FastIoWrite = RfnFastIoNoWrite;
    FastIo->FastIoQueryBasicInfo = RfnFastQueryBasicInfo;
    FastIo->FastIoQueryStandardInfo = RfnFastQueryStandardInfo;
    FastIo->FastIoQueryNetworkOpenInfo = RfnFastQueryNetworkOpenInfo;
#ifdef RFN_NT31
    if (RfnNt31) {
        /* FsRtlCopyRead is NT 3.1's own: it already takes what NT 3.1 passes */
        FastIo->FastIoCheckIfPossible = (PFAST_IO_CHECK_IF_POSSIBLE)RfnFastIoCheckIfPossible31;
        FastIo->FastIoWrite = (PFAST_IO_WRITE)RfnFastIoNoWrite31;
        FastIo->FastIoQueryBasicInfo = (PFAST_IO_QUERY_BASIC_INFO)RfnFastQueryBasicInfo31;
        FastIo->FastIoQueryStandardInfo = (PFAST_IO_QUERY_STANDARD_INFO)RfnFastQueryStandardInfo31;
        FastIo->FastIoQueryNetworkOpenInfo = NULL;
    }
#endif
}

/* ------------------------------------------------------------------ */
/* Cache manager callbacks                                             */
/* ------------------------------------------------------------------ */

static BOOLEAN
NTAPI
RfnAcquireForLazyWrite (
    PVOID Context,
    BOOLEAN Wait
    )
{
    PRFN_FCB Fcb = (PRFN_FCB)Context;

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->PagingIoResource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    /* Paging writes from this thread must not move VDL */
    Fcb->LazyWriteThread = KeGetCurrentThread();

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    return TRUE;
}

static VOID
NTAPI
RfnReleaseFromLazyWrite (
    PVOID Context
    )
{
    PRFN_FCB Fcb = (PRFN_FCB)Context;

    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {
        IoSetTopLevelIrp(NULL);
    }

    Fcb->LazyWriteThread = NULL;

    RfnRelease(&Fcb->PagingIoResource);
    FsRtlExitFileSystem();
}

static BOOLEAN
NTAPI
RfnAcquireForReadAhead (
    PVOID Context,
    BOOLEAN Wait
    )
{
    PRFN_FCB Fcb = (PRFN_FCB)Context;

    FsRtlEnterFileSystem();

    if (!ExAcquireResourceSharedLite(&Fcb->Resource, Wait)) {
        FsRtlExitFileSystem();
        return FALSE;
    }

    if (IoGetTopLevelIrp() == NULL) {
        IoSetTopLevelIrp((PIRP)FSRTL_CACHE_TOP_LEVEL_IRP);
    }

    return TRUE;
}

static VOID
NTAPI
RfnReleaseFromReadAhead (
    PVOID Context
    )
{
    PRFN_FCB Fcb = (PRFN_FCB)Context;

    if (IoGetTopLevelIrp() == (PIRP)FSRTL_CACHE_TOP_LEVEL_IRP) {
        IoSetTopLevelIrp(NULL);
    }

    RfnRelease(&Fcb->Resource);
    FsRtlExitFileSystem();
}

VOID
RfnInitializeCacheCallbacks (
    VOID
    )
{
    RfnData.CacheManagerCallbacks.AcquireForLazyWrite = RfnAcquireForLazyWrite;
    RfnData.CacheManagerCallbacks.ReleaseFromLazyWrite = RfnReleaseFromLazyWrite;
    RfnData.CacheManagerCallbacks.AcquireForReadAhead = RfnAcquireForReadAhead;
    RfnData.CacheManagerCallbacks.ReleaseFromReadAhead = RfnReleaseFromReadAhead;

}

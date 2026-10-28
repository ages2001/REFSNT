/*
 * REFSNT - IRP_MJ_FILE_SYSTEM_CONTROL (mount, verify, lock/unlock,
 * dismount, queries, REFSNT's own controls) and IRP_MJ_PNP
 */

#include "rfn.h"
#include "rfnctl.h"

/* ------------------------------------------------------------------ */
/* Mount                                                               */
/* ------------------------------------------------------------------ */

/* Hot-plug devices (W2K) get the removable-media flush policy */
#define RFN_IOCTL_GET_HOTPLUG_INFO CTL_CODE(0x0000002d, 0x0305, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _RFN_HOTPLUG_INFO {
    ULONG       Size;
    BOOLEAN     MediaRemovable;
    BOOLEAN     MediaHotplug;
    BOOLEAN     DeviceHotplug;
    BOOLEAN     WriteCacheEnableOverride;
} RFN_HOTPLUG_INFO;

static VOID
RfnCheckDeviceState (
    PRFN_VCB Vcb,
    PDEVICE_OBJECT TargetDevice
    )
{
    RFN_HOTPLUG_INFO Hotplug;
    NTSTATUS Status;

    Status = RfnDeviceIoctl(TargetDevice, IOCTL_DISK_IS_WRITABLE, NULL, 0, NULL, 0, TRUE, NULL);

    if (Status == STATUS_MEDIA_WRITE_PROTECTED || !RfnWriteSupportEnabled()) {
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
    }

    /* a CD / DVD / BD drive is written only if it says it can be (a DVD-RAM, say): pressed and
       recordable discs, and drives that answer nothing, are mounted read-only */
    if (Vcb->Optical && !NT_SUCCESS(Status)) {
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
    }

    if (Vcb->VcbState & VCB_STATE_REMOVABLE) {
        Vcb->VcbState |= VCB_STATE_FLUSH_ON_CLOSE;
    }

    RtlZeroMemory(&Hotplug, sizeof(Hotplug));

    Status = RfnDeviceIoctl(TargetDevice, RFN_IOCTL_GET_HOTPLUG_INFO, NULL, 0,
                             &Hotplug, sizeof(Hotplug), TRUE, NULL);

    if (NT_SUCCESS(Status) && (Hotplug.MediaRemovable || Hotplug.DeviceHotplug)) {
        Vcb->VcbState |= VCB_STATE_FLUSH_ON_CLOSE;
    }
}

/* The label as NT shows it: the logical volume identifier (the first 32 characters) */
VOID
RfnLoadLabel (
    PRFN_VCB Vcb
    )
{
    UNICODE_STRING U;
    ULONG Bytes = 0;
    ULONG i;
    PVPB Vpb = Vcb->Vpb;

    while (Bytes < 255 && Vcb->Vol.VolumeName[Bytes] != 0) {
        Bytes++;
    }

    Vcb->LabelLength = 0;
    if (Bytes != 0 && NT_SUCCESS(RfnExtToUnicode((const UCHAR *)Vcb->Vol.VolumeName, Bytes, &U))) {
        Vcb->LabelLength = U.Length / sizeof(WCHAR);
        if (Vcb->LabelLength > RFN_MAX_LABEL) {
            Vcb->LabelLength = RFN_MAX_LABEL;
        }
        RtlCopyMemory(Vcb->Label, U.Buffer, Vcb->LabelLength * sizeof(WCHAR));
        ExFreePool(U.Buffer);
        /* a label is not a file name: no escapes for a trailing space or dot */
        for (i = 0; i < Vcb->LabelLength; i++) {
            if (Vcb->Label[i] >= 0xF001 && Vcb->Label[i] <= 0xF07F) Vcb->Label[i] -= 0xF000;
        }
    }

    Vpb->VolumeLabelLength = (USHORT)(Vcb->LabelLength * sizeof(WCHAR));
    for (i = 0; i < Vcb->LabelLength; i++) {
        Vpb->VolumeLabel[i] = Vcb->Label[i];
    }
}

/* A ReFS boot sector at sector 0 */
static BOOLEAN
RfnProbeRefs (
    PDEVICE_OBJECT Device,
    ULONG SectorSize,
    PUCHAR Buffer
    )
{
    if (!NT_SUCCESS(RfnReadSectors(Device, 0, SectorSize < 512 ? 512 : SectorSize, Buffer, TRUE))) {
        return FALSE;
    }
    return RfxIsRefs(Buffer);
}

#define RFN_PROBE_BYTES         (16 * 4096)

/* Is the ReFS volume this VCB had still on the disk? */
BOOLEAN
RfnSameVolume (
    PRFN_VCB Vcb
    )
{
    BOOLEAN Same;
    RfnEngineEnter(Vcb);
    Same = RfxSameVolume(&Vcb->Vol);
    RfnRelease(&Vcb->EngineResource);
    return Same;
}

/*
 * (Re)reads the engine's view of the volume: superblock, chunk map, roots,
 * block groups. Vcb->Resource exclusive; writing not started.
 */
NTSTATUS
RfnLoadVolume (
    PRFN_VCB Vcb
    )
{
    BOOLEAN ReadOnly = (BOOLEAN)((Vcb->VcbState & VCB_STATE_READ_ONLY) != 0);
    NTSTATUS Status;

    RfnEngineEnter(Vcb);

    RfxUnmountFree(&Vcb->Vol);
    RtlZeroMemory(&Vcb->Vol, sizeof(RFX_VOL));
    Vcb->Vol.HostContext = Vcb;
    Vcb->Vol.DeviceBytes = (ULONGLONG)Vcb->PartitionBytes;
    Vcb->Vol.DeviceSector = Vcb->SectorSize;
    Vcb->Vol.Optical = Vcb->Optical;
    Vcb->Writing = FALSE;

    Status = RfxMount(&Vcb->Vol, ReadOnly);

    if (NT_SUCCESS(Status) && Vcb->PartitionBytes != 0 &&
        (Vcb->Vol.BlocksCount << Vcb->Vol.BlockShift) > (ULONGLONG)Vcb->PartitionBytes) {
        RFN_DBG((RFN_PFX "file system (%I64u blocks) larger than the partition\n",
                 Vcb->Vol.BlocksCount));
        Status = STATUS_DISK_CORRUPT_ERROR;
    }

    if (NT_SUCCESS(Status)) {
        if (Vcb->Vol.ReadOnly) {
            Vcb->VcbState |= VCB_STATE_READ_ONLY;
        }
        Vcb->VolumeBytes = (LONGLONG)(Vcb->Vol.BlocksCount << Vcb->Vol.BlockShift);
        if (Vcb->PartitionBytes < Vcb->VolumeBytes) {
            Vcb->PartitionBytes = Vcb->VolumeBytes;
        }
        Vcb->SerialNumber = Vcb->Vol.SerialNumber;
        /* made bootable by refsboot? (the engine found the view and keeps its window) */
        Vcb->BootView = (BOOLEAN)(Vcb->Vol.Rfs.AvoidHi > Vcb->Vol.Rfs.AvoidLo);
    } else {
        RfxUnmountFree(&Vcb->Vol);
    }

    RfnRelease(&Vcb->EngineResource);

    if (Status == RFX_CORRUPT || Status == RFX_NOT_SUPPORTED || Status == RFX_UNRECOGNIZED) {
        Status = STATUS_UNRECOGNIZED_VOLUME;
    }
    return Status;
}

/* After a mount or a reload: in use, and what a crashed Linux left on the orphan list */
static VOID
RfnBeginUse (
    PRFN_VCB Vcb
    )
{
    NTSTATUS Status;

    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return;
    }
    Status = RfnStartWriting(Vcb);
    if (NT_SUCCESS(Status) && Vcb->Writing) {
        RfnEngineEnter(Vcb);
        Status = RfnEngineLeave(Vcb, RfxProcessOrphans(&Vcb->Vol));
        if (!NT_SUCCESS(Status)) {
            RFN_DBG((RFN_PFX "orphan list: %lX\n", Status));
        }
    }
}

/* ntddcdrm.h: CTL_CODE(FILE_DEVICE_CD_ROM, 0x13, METHOD_BUFFERED, FILE_READ_ACCESS) */
#define RFN_IOCTL_CDROM_GET_DRIVE_GEOMETRY  0x0002404CUL

/* ntddcdrm.h: IOCTL_CDROM_READ_TOC, CTL_CODE(FILE_DEVICE_CD_ROM, 0, METHOD_BUFFERED, FILE_READ_ACCESS) */
#define RFN_IOCTL_CDROM_READ_TOC            0x00024000UL

/* The bytes of a disc: the lead-out's address in the table of contents (MSF), or 0 */
static LONGLONG
RfnDiscBytes (
    PDEVICE_OBJECT Device,
    ULONG SectorSize
    )
{
    PUCHAR Toc;
    LONGLONG Bytes = 0;
    ULONG Got = 0;
    Toc = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, 804, RFN_TAG_BUFFER);
    if (Toc == NULL) {
        return 0;
    }
    RtlZeroMemory(Toc, 804);
    if (NT_SUCCESS(RfnDeviceIoctl(Device, RFN_IOCTL_CDROM_READ_TOC, NULL, 0, Toc, 804, TRUE, &Got)) && Got >= 12) {
        ULONG First = Toc[2], Last = Toc[3], Entry = 4 + 8 * (Last - First + 1);
        if (Last >= First && Last - First < 99 && Entry + 8 <= 804 && Toc[Entry + 2] == 0xAA) {
            ULONG Lba = ((ULONG)Toc[Entry + 5] * 60 + Toc[Entry + 6]) * 75 + Toc[Entry + 7];
            if (Lba > 150) {
                Bytes = (LONGLONG)(Lba - 150) * SectorSize;
            }
        }
    }
    ExFreePool(Toc);
    return Bytes;
}

static NTSTATUS
RfnMountVolume (
    PRFN_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PDEVICE_OBJECT TargetDevice = IrpSp->Parameters.MountVolume.DeviceObject;
    PVPB Vpb = IrpSp->Parameters.MountVolume.Vpb;
    PDEVICE_OBJECT RealDevice = Vpb->RealDevice;
    DISK_GEOMETRY Geometry;
    PARTITION_INFORMATION Partition;
    PDEVICE_OBJECT VolumeDevice = NULL;
    PRFN_VCB Vcb = NULL;
    PUCHAR Super = NULL;
    LONGLONG PartitionBytes;
    BOOLEAN ClearedVerify = FALSE;
    BOOLEAN Resources = FALSE;
    NTSTATUS Status;

    RtlZeroMemory(&Geometry, sizeof(Geometry));
    RFN_DBG((RFN_PFX "mount offered: device %p (type %lu)\n", TargetDevice, (ULONG)TargetDevice->DeviceType));
    Status = RfnDeviceIoctl(TargetDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
                             &Geometry, sizeof(Geometry), TRUE, NULL);
    if (!NT_SUCCESS(Status) && TargetDevice->DeviceType == FILE_DEVICE_CD_ROM) {
        /* CD / DVD / BD drives: their own geometry request (the size from the medium's capacity) */
        Status = RfnDeviceIoctl(TargetDevice, RFN_IOCTL_CDROM_GET_DRIVE_GEOMETRY, NULL, 0,
                                 &Geometry, sizeof(Geometry), TRUE, NULL);
    }
    if (!NT_SUCCESS(Status) || Geometry.BytesPerSector < 512 || Geometry.BytesPerSector > 4096 ||
        (Geometry.BytesPerSector & (Geometry.BytesPerSector - 1))) {
        RFN_DBG((RFN_PFX "  no geometry (%lX, %lu bytes a sector)\n", Status, Geometry.BytesPerSector));
        return STATUS_UNRECOGNIZED_VOLUME;
    }

    RtlZeroMemory(&Partition, sizeof(Partition));
    Status = RfnDeviceIoctl(TargetDevice, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0,
                             &Partition, sizeof(Partition), TRUE, NULL);
    PartitionBytes = NT_SUCCESS(Status) ? Partition.PartitionLength.QuadPart : 0;
    if (PartitionBytes == 0 && TargetDevice->DeviceType == FILE_DEVICE_CD_ROM) {
        /* a disc: up to the lead-out of its last session (the geometry rounds down to 4 MB) */
        PartitionBytes = RfnDiscBytes(TargetDevice, Geometry.BytesPerSector);
    }
    if (PartitionBytes == 0) {
        /* Floppies and superfloppies: the geometry gives the size */
        PartitionBytes = Geometry.Cylinders.QuadPart * Geometry.TracksPerCylinder *
                         Geometry.SectorsPerTrack * Geometry.BytesPerSector;
    }

    Super = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, RFN_PROBE_BYTES, RFN_TAG_BUFFER);
    if (Super == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    __try {

        /* Cheap test first: most volumes offered to us are not ReFS */
        if (!RfnProbeRefs(TargetDevice, Geometry.BytesPerSector, Super)) {
            RFN_DBG((RFN_PFX "  no ReFS boot sector\n"));
            Status = STATUS_UNRECOGNIZED_VOLUME;
            __leave;
        }

        Status = IoCreateDevice(RfnData.DriverObject,
                                sizeof(RFN_VCB),
                                NULL,
                                (TargetDevice->DeviceType == FILE_DEVICE_CD_ROM ||
                                 TargetDevice->DeviceType == FILE_DEVICE_DVD) ?
                                    FILE_DEVICE_CD_ROM_FILE_SYSTEM : FILE_DEVICE_DISK_FILE_SYSTEM,
                                0,
                                FALSE,
                                &VolumeDevice);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        VolumeDevice->StackSize = (CCHAR)(TargetDevice->StackSize + 1);

        if (TargetDevice->AlignmentRequirement > VolumeDevice->AlignmentRequirement) {
            VolumeDevice->AlignmentRequirement = TargetDevice->AlignmentRequirement;
        }

        VolumeDevice->SectorSize = (USHORT)Geometry.BytesPerSector;

#ifdef DO_DEVICE_INITIALIZING
        VolumeDevice->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

        Vcb = (PRFN_VCB)VolumeDevice->DeviceExtension;
        RtlZeroMemory(Vcb, sizeof(RFN_VCB));

        Vcb->NodeTypeCode = RFN_NTC_VCB;
        Vcb->NodeByteSize = (CSHORT)sizeof(RFN_VCB);
        ExInitializeResourceLite(&Vcb->Resource);
        ExInitializeResourceLite(&Vcb->EngineResource);
        Resources = TRUE;
        InitializeListHead(&Vcb->FcbList);
        InitializeListHead(&Vcb->DirNotifyList);
        InitializeListHead(&Vcb->VcbLinks);
        FsRtlNotifyInitializeSync(&Vcb->NotifySync);
        RfnMetaInitialize(Vcb);

        Vcb->Vpb = Vpb;
        Vcb->TargetDeviceObject = TargetDevice;
        Vcb->VolumeDeviceObject = VolumeDevice;
        Vcb->VerifyThread = KeGetCurrentThread();
        Vcb->PartitionBytes = PartitionBytes;
        Vcb->SectorSize = Geometry.BytesPerSector;
        for (Vcb->SectorShift = 9; (1UL << Vcb->SectorShift) < Vcb->SectorSize; Vcb->SectorShift++)
            ;
        Vcb->Optical = (BOOLEAN)(TargetDevice->DeviceType == FILE_DEVICE_CD_ROM ||
                                 TargetDevice->DeviceType == FILE_DEVICE_DVD);

        if (RealDevice->Characteristics & FILE_REMOVABLE_MEDIA) {
            Vcb->VcbState |= VCB_STATE_REMOVABLE;
        }

        RfnCheckDeviceState(Vcb, TargetDevice);

        if (RealDevice->Flags & DO_VERIFY_VOLUME) {
            RealDevice->Flags &= ~DO_VERIFY_VOLUME;
            ClearedVerify = TRUE;
        }

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Status = RfnLoadVolume(Vcb);
        RfnRelease(&Vcb->Resource);
        if (!NT_SUCCESS(Status)) {
            RFN_DBG((RFN_PFX "mount failed %lX\n", Status));
            if (Status == STATUS_DISK_CORRUPT_ERROR || Status == STATUS_FILE_CORRUPT_ERROR) {
                Status = STATUS_UNRECOGNIZED_VOLUME;
            }
            __leave;
        }

        Vcb->RootDcb = RfnCreateRootDcb(Vcb);
        Vcb->VolumeFcb = RfnCreateVolumeFcb(Vcb);
        if (Vcb->RootDcb == NULL || Vcb->VolumeFcb == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        RfnLoadLabel(Vcb);

        Vpb->SerialNumber = Vcb->SerialNumber;
        Vpb->DeviceObject = VolumeDevice;
        Vcb->VcbState |= VCB_STATE_MOUNTED;
        Vcb->VerifyThread = NULL;

        (VOID)ObReferenceObjectByPointer(TargetDevice, 0, NULL, KernelMode);

        (VOID)ExAcquireResourceExclusiveLite(&RfnData.Resource, TRUE);
        InsertTailList(&RfnData.VcbList, &Vcb->VcbLinks);
        RfnRelease(&RfnData.Resource);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        RfnBeginUse(Vcb);
        RfnRelease(&Vcb->Resource);

        /*
         * Claim the drive letter the registry binds to this volume, now, before
         * the session manager runs: the boot volume needs its letter (usually
         * C:) even on an unrecognized (Linux-type 83h) partition, which the I/O
         * manager would otherwise leave unlettered. No-op on NT 3.1 - 4.0 (no
         * MountedDevices key) and when the letter is already assigned.
         */
        RfnClaimRegistryLetter(Vcb, TargetDevice, Partition.StartingOffset.QuadPart);

        RFN_DBG((RFN_PFX "Mounted ReFS %u.%u %08lX: %I64u clusters of %lu%s%s\n",
                 (ULONG)Vcb->Vol.Rfs.Major, (ULONG)Vcb->Vol.Rfs.Minor,
                 Vcb->SerialNumber, Vcb->Vol.BlocksCount, Vcb->Vol.BlockSize,
                 Vcb->Vol.ReadOnlyForced ? ", read-only (features)" : "",
                 (Vcb->VcbState & VCB_STATE_READ_ONLY) ? ", read-only" : ""));

    } __finally {

        if (Super != NULL) {
            ExFreePool(Super);
        }

        if (!NT_SUCCESS(Status)) {

            if (Vcb != NULL) {

                Vpb->DeviceObject = NULL;

                if (ClearedVerify) {
                    RealDevice->Flags |= DO_VERIFY_VOLUME;
                }

                while (!IsListEmpty(&Vcb->FcbList)) {
                    RfnDeleteFcb(CONTAINING_RECORD(Vcb->FcbList.Flink, RFN_FCB, FcbLinks));
                }
                if (Vcb->VolumeFcb != NULL) {
                    RfnDeleteFcb(Vcb->VolumeFcb);
                }

                RfxUnmountFree(&Vcb->Vol);
                RfnBounceFree(Vcb);
                FsRtlNotifyUninitializeSync(&Vcb->NotifySync);
                if (Resources) {
                    ExDeleteResourceLite(&Vcb->EngineResource);
                    ExDeleteResourceLite(&Vcb->Resource);
                }
            }

            if (VolumeDevice != NULL) {
                IoDeleteDevice(VolumeDevice);
            }
        }
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* Verify                                                              */
/* ------------------------------------------------------------------ */

static NTSTATUS
RfnVerifyVolume (
    PRFN_IRP_CONTEXT Ctx
    )
{
    PIO_STACK_LOCATION IrpSp = Ctx->IrpSp;
    PVPB Vpb = IrpSp->Parameters.VerifyVolume.Vpb;
    PDEVICE_OBJECT VolumeDevice = IrpSp->Parameters.VerifyVolume.DeviceObject;
    PRFN_VCB Vcb;
    PUCHAR Super = NULL;
    BOOLEAN Delete = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    if (VolumeDevice == NULL || VolumeDevice->DriverObject != RfnData.DriverObject ||
        RfnIsFsDevice(VolumeDevice)) {

        return STATUS_WRONG_VOLUME;
    }

    Vcb = (PRFN_VCB)VolumeDevice->DeviceExtension;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->VcbState & VCB_STATE_DISMOUNTED) {
            Status = STATUS_WRONG_VOLUME;
            __leave;
        }

        if (!(Vpb->RealDevice->Flags & DO_VERIFY_VOLUME)) {
            __leave;
        }

        Vcb->VerifyThread = KeGetCurrentThread();

        if (RfnSameVolume(Vcb)) {

            /* Same volume; it may have been changed while away (another
               system wrote it): read it again */
            (VOID)RfnStopWriting(Vcb);
            RfnPurgeCachedFiles(Vcb);
            Status = RfnLoadVolume(Vcb);
            if (NT_SUCCESS(Status)) {
                RfnLoadLabel(Vcb);
                RfnBeginUse(Vcb);
            } else {
                Delete = RfnDismountVcb(Vcb);
                Status = STATUS_WRONG_VOLUME;
            }

        } else {

            RFN_DBG((RFN_PFX "Verify: volume %08lX is gone\n", Vcb->SerialNumber));

            Delete = RfnDismountVcb(Vcb);
            Status = STATUS_WRONG_VOLUME;
        }

        Vpb->RealDevice->Flags &= ~DO_VERIFY_VOLUME;

    } __finally {

        if (Super != NULL) {
            ExFreePool(Super);
        }

        Vcb->VerifyThread = NULL;
        RfnRelease(&Vcb->Resource);
    }

    if (Delete) {
        RfnDeleteVcb(Vcb);
        Ctx->Vcb = NULL;
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* User requests                                                       */
/* ------------------------------------------------------------------ */

static NTSTATUS
RfnGetVolumeOpen (
    PRFN_IRP_CONTEXT Ctx,
    PRFN_CCB *Ccb
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PRFN_FCB Fcb;

    if (FileObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    Fcb = (PRFN_FCB)FileObject->FsContext;
    *Ccb = (PRFN_CCB)FileObject->FsContext2;

    if (Fcb == NULL || *Ccb == NULL || !RfnIsVfcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static VOID
RfnNotifyVolume (
    PFILE_OBJECT FileObject,
    ULONG Event
    )
{
#ifndef RFN_NT4
    (VOID)FsRtlNotifyVolumeEvent(FileObject, Event);
#else
    UNREFERENCED_PARAMETER(FileObject);
    UNREFERENCED_PARAMETER(Event);
#endif
}

#ifndef FSRTL_VOLUME_DISMOUNT
#define FSRTL_VOLUME_DISMOUNT       1
#endif
#ifndef FSRTL_VOLUME_LOCK
#define FSRTL_VOLUME_LOCK           3
#endif
#ifndef FSRTL_VOLUME_LOCK_FAILED
#define FSRTL_VOLUME_LOCK_FAILED    4
#endif
#ifndef FSRTL_VOLUME_UNLOCK
#define FSRTL_VOLUME_UNLOCK         5
#endif

static NTSTATUS
RfnLockVolume (
    PRFN_IRP_CONTEXT Ctx
    )
{
    PRFN_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PRFN_CCB Ccb;
    KIRQL Irql;
    NTSTATUS Status;

    Status = RfnGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    RfnNotifyVolume(FileObject, FSRTL_VOLUME_LOCK);

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->VcbState & (VCB_STATE_LOCKED | VCB_STATE_PNP_LOCKED)) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        /* Whoever locks is about to work on the disk itself */
        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)RfnFlushVolume(Vcb, TRUE);
            RfnPurgeCachedFiles(Vcb);
        }

        /* Only the caller's handle may be open */
        if (Vcb->UncleanCount != 1) {
            Status = STATUS_ACCESS_DENIED;
            __leave;
        }

        Vcb->VcbState |= VCB_STATE_LOCKED;
        Vcb->LockFileObject = FileObject;

        IoAcquireVpbSpinLock(&Irql);
        Vcb->Vpb->Flags |= VPB_LOCKED;
        IoReleaseVpbSpinLock(Irql);

    } __finally {

        RfnRelease(&Vcb->Resource);
    }

    if (!NT_SUCCESS(Status)) {
        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        RfnResumeWriting(Vcb);
        RfnRelease(&Vcb->Resource);
        RfnNotifyVolume(FileObject, FSRTL_VOLUME_LOCK_FAILED);
    }

    return Status;
}

static NTSTATUS
RfnUnlockVolume (
    PRFN_IRP_CONTEXT Ctx
    )
{
    PRFN_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PRFN_CCB Ccb;
    NTSTATUS Status;

    Status = RfnGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        if (Vcb->LockFileObject != FileObject) {

            Status = STATUS_NOT_LOCKED;

        } else {

            RfnUnlockVcb(Vcb);

            /* Metadata may have been rewritten under us */
            if (Vcb->VcbState & VCB_STATE_DASD_WRITTEN) {
                (VOID)RfnDismountVcb(Vcb);
            } else {
                RfnResumeWriting(Vcb);
            }
        }

    } __finally {

        RfnRelease(&Vcb->Resource);
    }

    if (NT_SUCCESS(Status)) {
        RfnNotifyVolume(FileObject, FSRTL_VOLUME_UNLOCK);
    }

    return Status;
}

static NTSTATUS
RfnDismountVolume (
    PRFN_IRP_CONTEXT Ctx
    )
{
    PRFN_VCB Vcb = Ctx->Vcb;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PRFN_CCB Ccb;
    NTSTATUS Status;

    Status = RfnGetVolumeOpen(Ctx, &Ccb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    /*
     * Programs running from the volume (the system volume while autochk
     * runs, say) cannot be dismounted from under: refuse, as a lock would be.
     */
    {
        PLIST_ENTRY Entry;
        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
            PRFN_FCB Fcb = CONTAINING_RECORD(Entry, RFN_FCB, FcbLinks);
            if ((Fcb->FcbState & FCB_STATE_PAGING_FILE) ||
                (Fcb->SectionObjectPointers.ImageSectionObject != NULL &&
                 !MmFlushImageSection(&Fcb->SectionObjectPointers, MmFlushForWrite))) {
                RfnRelease(&Vcb->Resource);
                RFN_DBG((RFN_PFX "dismount refused: the volume is in use by running programs\n"));
                return STATUS_ACCESS_DENIED;
            }
        }
    }

    RfnNotifyVolume(FileObject, FSRTL_VOLUME_DISMOUNT);

    __try {

        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)RfnFlushVolume(Vcb, TRUE);
        }

        /* The caller's handle stays open, so the VCB cannot go away here */
        Ccb->Flags |= CCB_FLAG_DISMOUNTED_VOLUME;
        (VOID)RfnDismountVcb(Vcb);

    } __finally {

        RfnRelease(&Vcb->Resource);
    }

    return STATUS_SUCCESS;
}

static NTSTATUS
RfnIsVolumeDirty (
    PRFN_IRP_CONTEXT Ctx
    )
{
    PRFN_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    PULONG Result = (PULONG)Irp->AssociatedIrp.SystemBuffer;

    if (RfnXSp(Ctx->IrpSp)->Parameters.FileSystemControl.OutputBufferLength < sizeof(ULONG)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* the integrity descriptor was open when mounted (not closed cleanly) */
    *Result = Vcb->Vol.Dirty ? VOLUME_IS_DIRTY : 0;
    Irp->IoStatus.Information = sizeof(ULONG);

    return STATUS_SUCCESS;
}

static NTSTATUS
RfnMarkVolumeDirtyRequest (
    PRFN_IRP_CONTEXT Ctx
    )
{
    PRFN_VCB Vcb = Ctx->Vcb;
    NTSTATUS Status;

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = RfnVerifyWritable(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        /* remembered until the volume goes (an open integrity descriptor says the same) */
        Vcb->Vol.Dirty = TRUE;
        Status = STATUS_SUCCESS;

    } __finally {

        RfnRelease(&Vcb->Resource);
    }

    return Status;
}

/* REFSNT's own controls (extctl.h) */
static NTSTATUS
RfnPrivateRequest (
    PRFN_IRP_CONTEXT Ctx,
    ULONG Code
    )
{
    PRFN_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PRFN_FCB Fcb = (FileObject != NULL) ? (PRFN_FCB)FileObject->FsContext : NULL;
    ULONG InLength = RfnXSp(Ctx->IrpSp)->Parameters.FileSystemControl.InputBufferLength;
    ULONG OutLength = RfnXSp(Ctx->IrpSp)->Parameters.FileSystemControl.OutputBufferLength;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    BOOLEAN FcbAcquired = FALSE;
    NTSTATUS Status;

    if (Fcb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = RfnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status)) {
            __leave;
        }

        switch (Code) {

        case RFN_FSCTL_QUERY_VOLUME: {
            PRFN_VOLUME_INFO Info = (PRFN_VOLUME_INFO)Buffer;
            ULONG i;
            if (OutLength < sizeof(RFN_VOLUME_INFO)) {
                Status = STATUS_BUFFER_TOO_SMALL;
                __leave;
            }
            RtlZeroMemory(Info, sizeof(RFN_VOLUME_INFO));
            RfnEngineEnter(Vcb);
            Info->Version = 1;
            Info->BlockSize = Vcb->Vol.BlockSize;
            Info->BlocksCount = Vcb->Vol.BlocksCount;
            RfxCountFree(&Vcb->Vol);
            Info->FreeBlocks = Vcb->Vol.FreeBlocksCount;
            Info->DataBlocks = Vcb->Vol.DataBlocks;
            Info->Revision = ((ULONG)Vcb->Vol.Rfs.Major << 8) | Vcb->Vol.Rfs.Minor;
            Info->Maps = Vcb->Vol.Rfs.NConts;
            Info->Files = 0;
            Info->Directories = 0;
            Info->ReadOnly = (Vcb->VcbState & VCB_STATE_READ_ONLY) ? 1 : 0;
            Info->ReadOnlyForced = Vcb->Vol.ReadOnlyForced ? 1 : 0;
            Info->Commits = Vcb->Commits;
            RtlCopyMemory(Info->Uuid, Vcb->Vol.Uuid, 16);
            for (i = 0; i < sizeof(Info->Label) - 1 && Vcb->Vol.VolumeName[i]; i++) {
                Info->Label[i] = Vcb->Vol.VolumeName[i];
            }
            RfnRelease(&Vcb->EngineResource);
            Irp->IoStatus.Information = sizeof(RFN_VOLUME_INFO);
            break;
        }

        case RFN_FSCTL_COMMIT:
            if (!(Vcb->VcbState & VCB_STATE_READ_ONLY)) {
                Status = RfnFlushVolume(Vcb, FALSE);
            }
            break;

        case RFN_FSCTL_GET_POSIX: {
            PRFN_POSIX_INFO Out = (PRFN_POSIX_INFO)Buffer;
            PRFX_INODE I;
            if (RfnIsVfcb(Fcb)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            if (OutLength < sizeof(RFN_POSIX_INFO)) {
                Status = STATUS_BUFFER_TOO_SMALL;
                __leave;
            }
            (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;
            I = &Fcb->Inode;
            RtlZeroMemory(Out, sizeof(RFN_POSIX_INFO));
            Out->Valid = RFN_POSIX_MODE | RFN_POSIX_UID | RFN_POSIX_GID;
            Out->Mode = I->Mode;
            Out->Uid = (ULONG)I->UidLo | ((ULONG)I->UidHi << 16);
            Out->Gid = (ULONG)I->GidLo | ((ULONG)I->GidHi << 16);
            Out->Links = I->LinksCount;
            Out->Inode = Fcb->Ino;
            Out->Flags = I->Flags;
            Out->Generation = I->Generation;
            Irp->IoStatus.Information = sizeof(RFN_POSIX_INFO);
            break;
        }

        case RFN_FSCTL_SET_POSIX: {
            PRFN_POSIX_INFO In = (PRFN_POSIX_INFO)Buffer;
            RFX_INODE Before;
            if (RfnIsVfcb(Fcb)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            if (InLength < sizeof(RFN_POSIX_INFO)) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            Status = RfnVerifyWritable(Ctx, Vcb);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
            (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;
            if (Fcb->FcbState & FCB_STATE_DELETED) {
                Status = STATUS_FILE_DELETED;
                __leave;
            }
            if (Fcb->FcbState & FCB_STATE_INODE_DIRTY) {
                Status = RfnUpdateInode(Vcb, Fcb);
                if (!NT_SUCCESS(Status)) {
                    __leave;
                }
            }
            RfnEngineEnter(Vcb);
            Before = Fcb->Inode;
            if (In->Valid & RFN_POSIX_MODE) {
                Fcb->Inode.Mode = (USHORT)((Fcb->Inode.Mode & RFX_S_IFMT) | (In->Mode & 07777));
            }
            if (In->Valid & RFN_POSIX_UID) {
                Fcb->Inode.UidLo = (USHORT)In->Uid;
                Fcb->Inode.UidHi = (USHORT)(In->Uid >> 16);
            }
            if (In->Valid & RFN_POSIX_GID) {
                Fcb->Inode.GidLo = (USHORT)In->Gid;
                Fcb->Inode.GidHi = (USHORT)(In->Gid >> 16);
            }
            Fcb->Inode.Ctime = RfxHostNow();
            Status = RfxWriteInode(&Vcb->Vol, Fcb->Ino, &Fcb->Inode);
            if (!NT_SUCCESS(Status)) {
                Fcb->Inode = Before;
            }
            Status = RfnEngineLeave(Vcb, Status);
            if (NT_SUCCESS(Status)) {
                KeQuerySystemTime(&Fcb->ChangeTime);
                Fcb->Attributes = RfnComputeAttributes(Vcb, &Fcb->Inode, Fcb->RfxName, Fcb->RfxNameLength,
                                                       Fcb->DosAttributes);
                RfnNotifyChange(Vcb, Fcb, FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY,
                                FILE_ACTION_MODIFIED);
            }
            break;
        }

        case RFN_FSCTL_READ_LINK: {
            ULONG Length = 0;
            PCHAR Text;
            if (RfnIsVfcb(Fcb) || (Fcb->Inode.Mode & RFX_S_IFMT) != RFX_S_IFLNK) {
                Status = STATUS_NOT_A_REPARSE_POINT;
                __leave;
            }
            Text = (PCHAR)ExAllocatePoolWithTag(PagedPool, Vcb->Vol.BlockSize + 1, RFN_TAG_BUFFER);
            if (Text == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }
            (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;
            RfnEngineEnter(Vcb);
            Status = RfnEngineLeave(Vcb, RfxReadLink(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, Text,
                                                     Vcb->Vol.BlockSize + 1, &Length));
            if (NT_SUCCESS(Status)) {
                if (OutLength < Length) {
                    Status = STATUS_BUFFER_OVERFLOW;
                    Length = OutLength;
                }
                RtlCopyMemory(Buffer, Text, Length);
                Irp->IoStatus.Information = Length;
            }
            ExFreePool(Text);
            break;
        }

        case RFN_FSCTL_CREATE_LINK: {
            PRFN_LINK_REQUEST Req = (PRFN_LINK_REQUEST)Buffer;
            UNICODE_STRING Name;
            UCHAR Bytes[RFN_MAX_NAME + 1];
            ULONG Length;
            PCHAR Target;
            ULONG NewIno;
            RFX_INODE NewInode;
            RFX_ENTRY *Found;

            if (!RfnIsDcb(Fcb)) {
                Status = STATUS_NOT_A_DIRECTORY;
                __leave;
            }
            if (InLength < FIELD_OFFSET(RFN_LINK_REQUEST, Name) ||
                (ULONG)FIELD_OFFSET(RFN_LINK_REQUEST, Name) + Req->NameLength + Req->TargetLength > InLength ||
                Req->NameLength == 0 || (Req->NameLength & 1) || Req->TargetLength == 0 ||
                Req->TargetLength >= Vcb->Vol.BlockSize) {
                Status = STATUS_INVALID_PARAMETER;
                __leave;
            }
            Status = RfnVerifyWritable(Ctx, Vcb);
            if (!NT_SUCCESS(Status)) {
                __leave;
            }
            Name.Buffer = Req->Name;
            Name.Length = Name.MaximumLength = Req->NameLength;
            if (!RfnIsValidUnicodeName(&Name) ||
                !NT_SUCCESS(RfnUnicodeToExt(&Name, Bytes, &Length)) || Length == 0) {
                Status = STATUS_OBJECT_NAME_INVALID;
                __leave;
            }
            Bytes[Length] = 0;

            (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);
            FcbAcquired = TRUE;
            if (Fcb->FcbState & (FCB_STATE_DELETED | FCB_STATE_DELETE_PENDING)) {
                Status = STATUS_DELETE_PENDING;
                __leave;
            }

            Found = (RFX_ENTRY *)ExAllocatePoolWithTag(PagedPool, sizeof(RFX_ENTRY), RFN_TAG_BUFFER);
            if (Found == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }
            Status = RfnLookupName(Vcb, Fcb, &Name, RfnData.CaseSensitive, Found);
            ExFreePool(Found);
            if (NT_SUCCESS(Status)) {
                Status = STATUS_OBJECT_NAME_COLLISION;
                __leave;
            }
            if (Status != STATUS_OBJECT_NAME_NOT_FOUND) {
                __leave;
            }

            Target = (PCHAR)ExAllocatePoolWithTag(PagedPool, Req->TargetLength + 1, RFN_TAG_BUFFER);
            if (Target == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                __leave;
            }
            RtlCopyMemory(Target, (PUCHAR)Req->Name + Req->NameLength, Req->TargetLength);
            Target[Req->TargetLength] = 0;

            RfnEngineEnter(Vcb);
            Status = RfxCreate(&Vcb->Vol, Fcb->Ino, &Fcb->Inode, (const char *)Bytes, Length,
                               (USHORT)(RFX_S_IFLNK | 0777), Target, &NewIno, &NewInode);
            if (NT_SUCCESS(Status) && (RfnData.DefaultUid || RfnData.DefaultGid)) {
                NewInode.UidLo = (USHORT)RfnData.DefaultUid;
                NewInode.UidHi = (USHORT)(RfnData.DefaultUid >> 16);
                NewInode.GidLo = (USHORT)RfnData.DefaultGid;
                NewInode.GidHi = (USHORT)(RfnData.DefaultGid >> 16);
                Status = RfxWriteInode(&Vcb->Vol, NewIno, &NewInode);
            }
            Status = RfnEngineLeave(Vcb, Status);
            ExFreePool(Target);

            if (NT_SUCCESS(Status)) {
                RfnIndexAdd(Fcb, Bytes, Length, NewIno);
                if (NT_SUCCESS(RfnBuildFullName(Fcb))) {
                    UNICODE_STRING Full;
                    USHORT Prefix = (USHORT)(Fcb->FullName.Length + (Fcb->ParentDcb != NULL ? sizeof(WCHAR) : 0));
                    Full.MaximumLength = (USHORT)(Prefix + Name.Length);
                    Full.Buffer = (PWSTR)ExAllocatePoolWithTag(PagedPool, Full.MaximumLength, RFN_TAG_NAME);
                    if (Full.Buffer != NULL) {
                        RtlCopyMemory(Full.Buffer, Fcb->FullName.Buffer, Fcb->FullName.Length);
                        if (Fcb->ParentDcb != NULL) {
                            Full.Buffer[Fcb->FullName.Length / sizeof(WCHAR)] = L'\\';
                        }
                        RtlCopyMemory((PUCHAR)Full.Buffer + Prefix, Name.Buffer, Name.Length);
                        Full.Length = Full.MaximumLength;
                        FsRtlNotifyFullReportChange(Vcb->NotifySync, &Vcb->DirNotifyList, (PSTRING)&Full,
                                                    Prefix, NULL, NULL, FILE_NOTIFY_CHANGE_FILE_NAME,
                                                    FILE_ACTION_ADDED, NULL);
                        ExFreePool(Full.Buffer);
                    }
                }
            }
            break;
        }

        default:
            Status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }

    } __finally {

        if (FcbAcquired) {
            RfnRelease(&Fcb->Resource);
        }
        RfnRelease(&Vcb->Resource);
    }

    return Status;
}

NTSTATUS
RfnUserFsRequest (
    PRFN_IRP_CONTEXT Ctx
    )
{
    ULONG Code = RfnXSp(Ctx->IrpSp)->Parameters.FileSystemControl.FsControlCode;

    switch (Code) {

    case FSCTL_LOCK_VOLUME:
        return RfnLockVolume(Ctx);

    case FSCTL_UNLOCK_VOLUME:
        return RfnUnlockVolume(Ctx);

    case FSCTL_DISMOUNT_VOLUME:
        return RfnDismountVolume(Ctx);

    case FSCTL_IS_VOLUME_MOUNTED:
        return RfnVerifyVcb(Ctx, Ctx->Vcb);

    case FSCTL_IS_PATHNAME_VALID:
        return STATUS_SUCCESS;

    case FSCTL_IS_VOLUME_DIRTY:
        return RfnIsVolumeDirty(Ctx);

    case FSCTL_MARK_VOLUME_DIRTY:
        return RfnMarkVolumeDirtyRequest(Ctx);

    case RFN_FSCTL_QUERY_VOLUME:
    case RFN_FSCTL_GET_POSIX:
    case RFN_FSCTL_SET_POSIX:
    case RFN_FSCTL_READ_LINK:
    case RFN_FSCTL_CREATE_LINK:
    case RFN_FSCTL_COMMIT:
        return RfnPrivateRequest(Ctx, Code);

    case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    case FSCTL_REQUEST_BATCH_OPLOCK:
    case FSCTL_REQUEST_FILTER_OPLOCK:
        return STATUS_OPLOCK_NOT_GRANTED;

    case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    case FSCTL_OPLOCK_BREAK_NOTIFY:
    case FSCTL_OPLOCK_BREAK_ACK_NO_2:
        return STATUS_INVALID_OPLOCK_PROTOCOL;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS
RfnCommonFileSystemControl (
    PRFN_IRP_CONTEXT Ctx
    )
{
    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_MOUNT_VOLUME:
        if (Ctx->Vcb != NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        return RfnMountVolume(Ctx);

    case IRP_MN_VERIFY_VOLUME:
        return RfnVerifyVolume(Ctx);

    case IRP_MN_USER_FS_REQUEST:
#ifdef IRP_MN_KERNEL_CALL
    case IRP_MN_KERNEL_CALL:
#endif
        if (Ctx->Vcb == NULL) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        return RfnUserFsRequest(Ctx);
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

/* ------------------------------------------------------------------ */
/* Plug and Play (Windows 2000)                                        */
/* ------------------------------------------------------------------ */

#ifndef RFN_NT4

static NTSTATUS
NTAPI
RfnPnpCompletion (
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
    )
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);

    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Sends the IRP down and waits; the caller completes it */
static NTSTATUS
RfnForwardAndWait (
    PRFN_IRP_CONTEXT Ctx,
    PRFN_VCB Vcb
    )
{
    PIRP Irp = Ctx->Irp;
    KEVENT Event;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);

    RfnCopyStackToNext(Irp);
    IoSetCompletionRoutine(Irp, RfnPnpCompletion, &Event, TRUE, TRUE, TRUE);

    if (IoCallDriver(Vcb->TargetDeviceObject, Irp) == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
    }

    return Irp->IoStatus.Status;
}

NTSTATUS
RfnCommonPnp (
    PRFN_IRP_CONTEXT Ctx
    )
{
    PRFN_VCB Vcb = Ctx->Vcb;
    PIRP Irp = Ctx->Irp;
    BOOLEAN Delete = FALSE;
    NTSTATUS Status;

    switch (Ctx->IrpSp->MinorFunction) {

    case IRP_MN_QUERY_REMOVE_DEVICE:

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

        if (!(Vcb->VcbState & VCB_STATE_DISMOUNTED)) {
            (VOID)RfnFlushVolume(Vcb, TRUE);
            RfnPurgeCachedFiles(Vcb);
        }

        if (Vcb->UncleanCount != 0 || (Vcb->VcbState & VCB_STATE_LOCKED)) {
            RfnResumeWriting(Vcb);
            RfnRelease(&Vcb->Resource);
            return STATUS_ACCESS_DENIED;
        }

        Vcb->VcbState |= VCB_STATE_PNP_LOCKED;
        RfnRelease(&Vcb->Resource);

        Status = RfnForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

        if (NT_SUCCESS(Status)) {
            Delete = RfnDismountVcb(Vcb);
        } else {
            Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
            RfnResumeWriting(Vcb);
        }

        RfnRelease(&Vcb->Resource);
        break;

    case IRP_MN_REMOVE_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:

        Status = RfnForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        Vcb->VcbState |= VCB_STATE_REMOVED;
        Delete = RfnDismountVcb(Vcb);
        RfnRelease(&Vcb->Resource);
        break;

    case IRP_MN_CANCEL_REMOVE_DEVICE:

        Status = RfnForwardAndWait(Ctx, Vcb);

        (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);
        Vcb->VcbState &= ~VCB_STATE_PNP_LOCKED;
        RfnResumeWriting(Vcb);
        RfnRelease(&Vcb->Resource);
        break;

    default:

        if (Vcb->VcbState & VCB_STATE_REMOVED) {
            return STATUS_NO_SUCH_DEVICE;
        }

        RfnSkipStack(Irp);
        Ctx->Flags |= RFN_CTX_NO_COMPLETE;
        return IoCallDriver(Vcb->TargetDeviceObject, Irp);
    }

    if (Delete) {
        RfnDeleteVcb(Vcb);
        Ctx->Vcb = NULL;
    }

    return Status;
}

#else

NTSTATUS
RfnCommonPnp (
    PRFN_IRP_CONTEXT Ctx
    )
{
    UNREFERENCED_PARAMETER(Ctx);
    return STATUS_INVALID_DEVICE_REQUEST;
}

#endif

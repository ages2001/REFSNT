/*
 * REFSNT - IRP_MJ_FLUSH_BUFFERS, IRP_MJ_SHUTDOWN and the volume's in-use state
 *
 * While REFSNT may change a volume, the volume says so on the disk, as Linux
 * does: with a journal the needs_recovery flag is set (a crash leaves the
 * journal to replay), without one the clean state is cleared (fsck checks
 * it). RfxStartWriting does that at mount; RfxStopWriting - last commit,
 * journal empty, state restored - at lock, dismount and shutdown.
 * Flushing a file or the volume commits the journal.
 */

#include "rfn.h"

/* ------------------------------------------------------------------ */
/* In use                                                              */
/* ------------------------------------------------------------------ */

NTSTATUS
RfnStartWriting (
    PRFN_VCB Vcb
    )
{
    NTSTATUS Status;

    if (Vcb->Writing || Vcb->Vol.ReadOnly ||
        (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_SHUTDOWN | VCB_STATE_REMOVED))) {
        return STATUS_SUCCESS;
    }

    RfnEngineEnter(Vcb);
    Status = RfxStartWriting(&Vcb->Vol);
    if (NT_SUCCESS(Status)) {
        Vcb->Writing = TRUE;
    }
    Status = RfnEngineLeave(Vcb, Status);

    if (NT_SUCCESS(Status)) {
        RfnCommitTimerStart(Vcb);
    } else {
        RFN_DBG((RFN_PFX "cannot start writing (%lX): read-only\n", Status));
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
        Vcb->Vol.ReadOnly = TRUE;
    }
    return Status;
}

/* Everything on the disk and the volume clean; the engine not held */
NTSTATUS
RfnStopWriting (
    PRFN_VCB Vcb
    )
{
    NTSTATUS Status;

    if (!Vcb->Writing) {
        return STATUS_SUCCESS;
    }

    RfnCommitTimerStop(Vcb);

    if (Vcb->BootView) {
        RfnUpdateBootView(Vcb);
    }

    RfnEngineEnter(Vcb);
    Status = RfxStopWriting(&Vcb->Vol);
    Vcb->Writing = FALSE;
    Status = RfnEngineLeave(Vcb, Status);
    RFN_DBG((RFN_PFX "%08lX clean (%lX), %lu commits\n", Vcb->SerialNumber, Status, Vcb->Commits));

    if (!NT_SUCCESS(Status)) {
        RFN_DBG((RFN_PFX "stop writing failed %lX\n", Status));
    }
    return Status;
}

/* ------------------------------------------------------------------ */
/* The boot view (see rfsshim.c)                                       */
/* ------------------------------------------------------------------ */

static RFS_STATUS
RfnShimCommit (PVOID Ctx)
{
    return RfnCommit((PRFN_VCB)Ctx);
}

/* Files in use as paging files, or with changes not yet in their file entry, stay where they are */
static BOOLEAN
RfnShimCanMove (PVOID Ctx, ULONGLONG Home, ULONGLONG FileId)
{
    PRFN_VCB Vcb = (PRFN_VCB)Ctx;
    PLIST_ENTRY Entry;
    PRFN_FCB Fcb;
    ULONG Ino = RfxInoOfFile(&Vcb->Vol, Home, FileId);
    if (Ino == 0) {
        return TRUE;                    /* never opened */
    }
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, RFN_FCB, FcbLinks);
        if (Fcb->Ino == Ino &&
            (Fcb->FcbState & (FCB_STATE_PAGING_FILE | FCB_STATE_INODE_DIRTY) || Fcb->Runs != NULL)) {
            return FALSE;
        }
    }
    return TRUE;
}

/* A file's data moved: the FCBs take the new map */
static VOID
RfnShimMoved (PVOID Ctx, ULONGLONG Home, ULONGLONG FileId)
{
    PRFN_VCB Vcb = (PRFN_VCB)Ctx;
    PLIST_ENTRY Entry;
    PRFN_FCB Fcb;
    ULONG Ino = RfxInoOfFile(&Vcb->Vol, Home, FileId);
    if (Ino == 0) {
        return;
    }
    RfxForget(&Vcb->Vol, Ino);
    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, RFN_FCB, FcbLinks);
        if (Fcb->Ino == Ino && !(Fcb->FcbState & FCB_STATE_DELETED)) {
            (VOID)RfxReadInode(&Vcb->Vol, Ino, &Fcb->Inode);
            Fcb->MapRun = 0;
        }
    }
}

/*
 * Brings the FAT view NTLDR reads up to date with the files it shows:
 * a boot file that changed, grew, or was replaced shows its current blocks.
 * Called while the volume stops being written (shutdown, lock, dismount),
 * everything flushed; Vcb->Resource exclusive.
 */
VOID
RfnUpdateBootView (
    PRFN_VCB Vcb
    )
{
    RFS_SHIM_HOST Host;
    RFS_SHIM_RESULT Result;
    NTSTATUS Status;

    if (Vcb->Vol.ReadOnly || (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_DISMOUNTED))) {
        return;
    }
    RtlZeroMemory(&Host, sizeof(Host));
    Host.Vol = &Vcb->Vol.Rfs;
    Host.Ctx = Vcb;
    Host.Commit = RfnShimCommit;
    Host.CanMove = RfnShimCanMove;
    Host.Moved = RfnShimMoved;

    RfnEngineEnter(Vcb);
    Status = RfnCommit(Vcb);
    if (NT_SUCCESS(Status)) {
        RtlZeroMemory(&Result, sizeof(Result));
        Status = RfsShimUpdate(&Host, NULL, NULL, 0, &Result);
        RFN_DBG((RFN_PFX "boot view: %lX, %lu files, %lu moved in, %lu out%s%s\n", Status, Result.Files,
                 Result.MovedIn, Result.MovedOut, Result.Error ? ": " : "", Result.Error ? Result.Error : ""));
    }
    (VOID)RfnEngineLeave(Vcb, Status);
}

/* Back in use after a lock (or a refused one); Vcb->Resource exclusive */
VOID
RfnResumeWriting (
    PRFN_VCB Vcb
    )
{
    if (!(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_LOCKED | VCB_STATE_PNP_LOCKED))) {
        (VOID)RfnStartWriting(Vcb);
    }
}

/* ------------------------------------------------------------------ */
/* Flushing                                                            */
/* ------------------------------------------------------------------ */

/* Data and inode of one file; Vcb->Resource exclusive */
NTSTATUS
RfnFlushFile (
    PRFN_VCB Vcb,
    PRFN_FCB Fcb
    )
{
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status = STATUS_SUCCESS;

    (VOID)ExAcquireResourceExclusiveLite(&Fcb->Resource, TRUE);

    __try {

        if (Fcb->SectionObjectPointers.DataSectionObject != NULL) {

            CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, &Iosb);
            Status = Iosb.Status;

            /* Let a lazy write in progress finish */
            (VOID)ExAcquireResourceExclusiveLite(&Fcb->PagingIoResource, TRUE);
            RfnRelease(&Fcb->PagingIoResource);
        }

        /*
         * ext keeps no valid-data length on disk, so blocks allocated past
         * the last byte written must be zeroed before the larger size
         * reaches the inode, or stale data would show through on the next
         * mount (holes need nothing).
         */
        if (RfnIsFcb(Fcb) && !(Fcb->FcbState & FCB_STATE_DELETED) &&
            Fcb->Header.ValidDataLength.QuadPart < Fcb->Header.FileSize.QuadPart) {

            NTSTATUS Zero = RfnZeroDisk(Vcb, Fcb, Fcb->Header.ValidDataLength.QuadPart,
                                        Fcb->Header.FileSize.QuadPart);

            if (NT_SUCCESS(Zero)) {
                Fcb->Header.ValidDataLength = Fcb->Header.FileSize;
            } else if (NT_SUCCESS(Status)) {
                Status = Zero;
            }
        }

        if ((Fcb->FcbState & FCB_STATE_INODE_DIRTY) && !(Fcb->FcbState & FCB_STATE_DELETED)) {

            if (NT_SUCCESS(Status)) {
                Status = RfnUpdateInode(Vcb, Fcb);
            } else {
                (VOID)RfnUpdateInode(Vcb, Fcb);
            }
        }

    } __finally {

        RfnRelease(&Fcb->Resource);
    }

    return Status;
}

/* Metadata: a journal commit (or, without a journal, the cached blocks) */
static NTSTATUS
RfnFlushMetadata (
    PRFN_VCB Vcb
    )
{
    return RfnCommitNow(Vcb);
}

/*
 * Writes everything cached for the volume; Final also ends its use
 * (journal empty, clean state), for a lock, dismount or shutdown.
 * Vcb->Resource exclusive.
 */
NTSTATUS
RfnFlushVolume (
    PRFN_VCB Vcb,
    BOOLEAN Final
    )
{
    PLIST_ENTRY Entry;
    PRFN_FCB Fcb;
    BOOLEAN Restart;
    NTSTATUS Status = STATUS_SUCCESS;
    NTSTATUS Result;

    if (Vcb->VcbState & (VCB_STATE_READ_ONLY | VCB_STATE_DISMOUNTED)) {
        return STATUS_SUCCESS;
    }

    /* A flush can close file objects, so the list is walked with markers */
    do {

        Restart = FALSE;

        for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {

            Fcb = CONTAINING_RECORD(Entry, RFN_FCB, FcbLinks);

            if ((Fcb->FcbState & FCB_STATE_VISITED) || !RfnIsFcb(Fcb)) {
                continue;
            }

            Fcb->FcbState |= FCB_STATE_VISITED;
            Fcb->RefCount++;

            Result = RfnFlushFile(Vcb, Fcb);
            if (NT_SUCCESS(Status)) {
                Status = Result;
            }

            RfnDereferenceFcb(Vcb, Fcb);

            Restart = TRUE;
            break;
        }

    } while (Restart);

    for (Entry = Vcb->FcbList.Flink; Entry != &Vcb->FcbList; Entry = Entry->Flink) {
        Fcb = CONTAINING_RECORD(Entry, RFN_FCB, FcbLinks);
        Fcb->FcbState &= ~FCB_STATE_VISITED;
    }

    Result = RfnFlushMetadata(Vcb);
    if (NT_SUCCESS(Status)) {
        Status = Result;
    }

    if (Final) {
        if (Vcb->Writing) {
            Result = RfnStopWriting(Vcb);
            if (NT_SUCCESS(Status)) {
                Status = Result;
            }
        } else if (Vcb->BootView && (Vcb->VcbState & VCB_STATE_SHUTDOWN)) {
            /* the last shutdown call (after the registry's last flush): the view once more */
            RfnUpdateBootView(Vcb);
        }
    }

    /* Skipped during shutdown: the disk cache-flush waits on a disk stack that
     * may already be going down (the "Shutdown in progress" hang, disk light on).
     * Writes are write-through; the disk flushes its own cache at shutdown. */
    if (!RfnData.ShuttingDown) {
        (VOID)RfnFlushDevice(Vcb);
    }

    return Status;
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

NTSTATUS
RfnCommonFlushBuffers (
    PRFN_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PRFN_FCB Fcb = (PRFN_FCB)FileObject->FsContext;
    PRFN_VCB Vcb = Ctx->Vcb;
    NTSTATUS Status;

    if (Fcb == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

    __try {

        Status = RfnVerifyVcb(Ctx, Vcb);
        if (!NT_SUCCESS(Status) || (Vcb->VcbState & VCB_STATE_READ_ONLY)) {
            __leave;
        }

        if (RfnIsVfcb(Fcb)) {

            Status = RfnFlushVolume(Vcb, FALSE);

        } else {

            if (RfnIsFcb(Fcb)) {
                Status = RfnFlushFile(Vcb, Fcb);
            }

            if (NT_SUCCESS(Status)) {
                Status = RfnFlushMetadata(Vcb);
            } else {
                (VOID)RfnFlushMetadata(Vcb);
            }

            (VOID)RfnFlushDevice(Vcb);
        }

    } __finally {

        RfnRelease(&Vcb->Resource);
    }

    return Status;
}

/* Sent to the file system device: leave every volume clean */
NTSTATUS
RfnCommonShutdown (
    PRFN_IRP_CONTEXT Ctx
    )
{
    PLIST_ENTRY Entry;
    PRFN_VCB Vcb;

    UNREFERENCED_PARAMETER(Ctx);

    RFN_DBG((RFN_PFX "shutdown\n"));
    RfnData.ShuttingDown = TRUE;   /* skip disk cache-flush: hangs at shutdown */
    (VOID)ExAcquireResourceSharedLite(&RfnData.Resource, TRUE);

    __try {

        for (Entry = RfnData.VcbList.Flink; Entry != &RfnData.VcbList; Entry = Entry->Flink) {

            Vcb = CONTAINING_RECORD(Entry, RFN_VCB, VcbLinks);

            (VOID)ExAcquireResourceExclusiveLite(&Vcb->Resource, TRUE);

            if (Vcb->VcbState & VCB_STATE_MOUNTED) {
                /* From now on every change goes to the disk at once (RfnEngineLeave):
                   the journal is closed and the volume marked clean */
                Vcb->VcbState |= VCB_STATE_SHUTDOWN;
                (VOID)RfnFlushVolume(Vcb, TRUE);
            }

            RfnRelease(&Vcb->Resource);
        }

    } __finally {

        RfnRelease(&RfnData.Resource);
    }

    return STATUS_SUCCESS;
}

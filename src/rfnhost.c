/*
 * REFSNT - the engine's host in the driver: device reads and writes for
 * the engine (staged through nonpaged memory, any alignment), memory, the
 * clock, the engine lock and the commits.
 *
 * The engine keeps a write-back cache of metadata blocks and file entries; a
 * commit writes them, flushes the disk and closes the integrity descriptor
 * (the first change after it opens it again). Commits happen every
 * RFN_COMMIT_SECONDS, when more than RFN_COMMIT_DIRTY bytes are dirty, on a
 * flush request, and when the volume goes away.
 *
 * Everything here runs with Vcb->EngineResource held exclusive, except the
 * timer DPC (which only queues the commit work item).
 */

#include "rfn.h"

#define VCB_OF(Vol)         ((PRFN_VCB)(Vol)->HostContext)
#define ENGINE_UP(Vcb)      ((Vcb)->Vol.Mounted)
#define RFN_COMMIT_DIRTY    (8u << 20)
#define RFN_STAGE           (256u * 1024)

/* ------------------------------------------------------------------ */
/* Device I/O for the engine                                           */
/* ------------------------------------------------------------------ */

/*
 * The staging buffer: one per volume, allocated at the first I/O (the mount)
 * and kept, so that a commit under memory pressure does not fail for want
 * of nonpaged pool; smaller temporary ones if even that could not be had.
 */
static PUCHAR
RfnBounceGet (
    PRFN_VCB Vcb,
    PULONG Stage
    )
{
    ULONG Size;
    PUCHAR P;
    if (Vcb->Bounce == NULL) {
        Vcb->Bounce = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, RFN_STAGE + RFN_SECTOR_SIZE, RFN_TAG_BUFFER);
    }
    if (Vcb->Bounce != NULL) {
        *Stage = RFN_STAGE;
        return Vcb->Bounce;
    }
    for (Size = 64 * 1024; Size >= 4096; Size /= 4) {
        P = (PUCHAR)ExAllocatePoolWithTag(NonPagedPool, Size + RFN_SECTOR_SIZE, RFN_TAG_BUFFER);
        if (P != NULL) {
            *Stage = Size;
            return P;
        }
    }
    RFN_DBG((RFN_PFX "no staging buffer\n"));
    return NULL;
}

static VOID
RfnBounceDone (
    PRFN_VCB Vcb,
    PUCHAR P
    )
{
    if (P != Vcb->Bounce) {
        ExFreePool(P);
    }
}

VOID
RfnBounceFree (
    PRFN_VCB Vcb
    )
{
    if (Vcb->Bounce != NULL) {
        ExFreePool(Vcb->Bounce);
        Vcb->Bounce = NULL;
    }
}

/* Length bytes at byte Offset of the volume, any alignment */
RFS_STATUS
RfsHostRead (
    PRFS_VOL Vol,
    ULONGLONG Offset,
    ULONG Length,
    PVOID Buffer
    )
{
    PRFN_VCB Vcb = VCB_OF(Vol);
    PUCHAR Out = (PUCHAR)Buffer;
    PUCHAR Bounce;
    ULONG Stage;
    NTSTATUS Status = STATUS_SUCCESS;

    Bounce = RfnBounceGet(Vcb, &Stage);
    if (Bounce == NULL) {
        return RFS_NO_MEMORY;
    }
    while (Length != 0) {
        ULONGLONG First = Offset & ~(ULONGLONG)(RFN_SECTOR_SIZE - 1);
        ULONG Lead = (ULONG)(Offset - First);
        ULONG Take = Length;
        ULONG Span;
        if (Take > Stage - Lead) {
            Take = Stage - Lead;
        }
        Span = (Lead + Take + RFN_SECTOR_SIZE - 1) & ~(RFN_SECTOR_SIZE - 1);
        Status = RfnReadDisk(Vcb, First >> RFN_SECTOR_SHIFT, Span >> RFN_SECTOR_SHIFT, Bounce);
        if (!NT_SUCCESS(Status)) {
            break;
        }
        RtlCopyMemory(Out, Bounce + Lead, Take);
        Out += Take;
        Offset += Take;
        Length -= Take;
    }
    RfnBounceDone(Vcb, Bounce);
    return Status;
}

RFS_STATUS
RfsHostWrite (
    PRFS_VOL Vol,
    ULONGLONG Offset,
    ULONG Length,
    const VOID *Buffer
    )
{
    PRFN_VCB Vcb = VCB_OF(Vol);
    const UCHAR *In = (const UCHAR *)Buffer;
    PUCHAR Bounce;
    ULONG Stage;
    NTSTATUS Status = STATUS_SUCCESS;

    if (Vcb->VcbState & VCB_STATE_READ_ONLY) {
        return STATUS_MEDIA_WRITE_PROTECTED;
    }
    Bounce = RfnBounceGet(Vcb, &Stage);
    if (Bounce == NULL) {
        return RFS_NO_MEMORY;
    }
    while (Length != 0) {
        ULONGLONG First = Offset & ~(ULONGLONG)(RFN_SECTOR_SIZE - 1);
        ULONG Lead = (ULONG)(Offset - First);
        ULONG Take = Length;
        ULONG Span;
        if (Take > Stage - Lead) {
            Take = Stage - Lead;
        }
        Span = (Lead + Take + RFN_SECTOR_SIZE - 1) & ~(RFN_SECTOR_SIZE - 1);
        if (Lead != 0 || Span != Take) {
            /* partial sectors at the edges keep their other bytes */
            Status = RfnReadDisk(Vcb, First >> RFN_SECTOR_SHIFT, Span >> RFN_SECTOR_SHIFT, Bounce);
            if (!NT_SUCCESS(Status)) {
                break;
            }
        }
        RtlCopyMemory(Bounce + Lead, In, Take);
        Status = RfnWriteDisk(Vcb, First >> RFN_SECTOR_SHIFT, Span >> RFN_SECTOR_SHIFT, Bounce);
        if (!NT_SUCCESS(Status)) {
            break;
        }
        In += Take;
        Offset += Take;
        Length -= Take;
    }
    RfnBounceDone(Vcb, Bounce);
    return Status;
}

RFS_STATUS
RfsHostFlush (
    PRFS_VOL Vol
    )
{
    return RfnFlushDevice(VCB_OF(Vol));
}

/*
 * The engine's memory: paged (paging I/O of a paging file never enters the
 * engine). When the system is short of commit, paged pool can fail for a
 * while: then nonpaged pool, and failing that a few retries a moment apart,
 * since a commit that cannot get its buffers leaves the volume read-only.
 */
PVOID
RfsHostAlloc (
    ULONG Bytes
    )
{
    PVOID P;
    ULONG Try;
    if (Bytes == 0) {
        Bytes = 1;
    }
    for (Try = 0; ; Try++) {
        P = ExAllocatePoolWithTag(PagedPool, Bytes, RFN_TAG_ENGINE);
        if (P == NULL) {
            P = ExAllocatePoolWithTag(NonPagedPool, Bytes, RFN_TAG_ENGINE);
        }
        if (P != NULL || Try == 40) {       /* (always below DISPATCH_LEVEL: the engine runs under an ERESOURCE) */
            break;
        }
        {
            LARGE_INTEGER Wait;
            Wait.QuadPart = -250000;        /* 25 ms */
            (VOID)KeDelayExecutionThread(KernelMode, FALSE, &Wait);
        }
    }
    if (P == NULL) {
        RFN_DBG((RFN_PFX "engine allocation of %lu bytes failed\n", Bytes));
    } else if (Try != 0) {
        RFN_DBG((RFN_PFX "engine allocation of %lu bytes: %lu retries\n", Bytes, Try));
    }
    return P;
}

VOID
RfsHostFree (
    PVOID P
    )
{
    if (P != NULL) {
        ExFreePool(P);
    }
}

PVOID RfxHostAlloc(ULONG Bytes) { return RfsHostAlloc(Bytes); }
VOID  RfxHostFree(PVOID P) { RfsHostFree(P); }

ULONGLONG RfsHostNowNt(VOID) { LARGE_INTEGER Now; KeQuerySystemTime(&Now); return (ULONGLONG)Now.QuadPart; }

ULONGLONG
RfxHostNowNt (
    VOID
    )
{
    LARGE_INTEGER Now;
    KeQuerySystemTime(&Now);
    return (ULONGLONG)Now.QuadPart;
}

ULONG
RfxHostNow (
    VOID
    )
{
    LARGE_INTEGER Now;
    ULONG Seconds;

    KeQuerySystemTime(&Now);
    if (!RtlTimeToSecondsSince1970(&Now, &Seconds)) {
        Seconds = 0;
    }
    return Seconds;
}

/* ------------------------------------------------------------------ */
/* Engine calls and commits                                            */
/* ------------------------------------------------------------------ */

VOID RfnMetaInitialize(PRFN_VCB Vcb) { UNREFERENCED_PARAMETER(Vcb); }
VOID RfnMetaPurge(PRFN_VCB Vcb) { UNREFERENCED_PARAMETER(Vcb); }

VOID
RfnEngineEnter (
    PRFN_VCB Vcb
    )
{
    (VOID)ExAcquireResourceExclusiveLite(&Vcb->EngineResource, TRUE);
}

/* Writes the running transaction (engine held) */
NTSTATUS
RfnCommit (
    PRFN_VCB Vcb
    )
{
    NTSTATUS Status;

    if (!ENGINE_UP(Vcb) || !RfxTxnPending(&Vcb->Vol)) {
        return STATUS_SUCCESS;
    }
    Status = RfxTxnCommit(&Vcb->Vol);
    if (NT_SUCCESS(Status)) {
        Vcb->Commits++;
    } else {
        /* the transaction cannot be written: nothing more goes to the disk,
           which keeps its last consistent generation */
        RFN_DBG((RFN_PFX "commit failed %lX: the volume is read-only from now on\n", Status));
        Vcb->VcbState |= VCB_STATE_READ_ONLY;
        Vcb->Vol.ReadOnly = TRUE;
    }
    return Status;
}

NTSTATUS
RfnCommitNow (
    PRFN_VCB Vcb
    )
{
    RfnEngineEnter(Vcb);
    return RfnEngineLeave(Vcb, RfnCommit(Vcb));
}

/*
 * Ends an engine call: commits when the transaction has grown large (or the
 * system is going down). Returns Status, or the commit's error if the call
 * itself succeeded.
 */
NTSTATUS
RfnEngineLeave (
    PRFN_VCB Vcb,
    NTSTATUS Status
    )
{
    NTSTATUS Flush = STATUS_SUCCESS;

    if (ENGINE_UP(Vcb) && RfxTxnPending(&Vcb->Vol) &&
        (RfxDirtyBytes(&Vcb->Vol) > RFN_COMMIT_DIRTY || (Vcb->VcbState & VCB_STATE_SHUTDOWN))) {
        if (Vcb->VcbState & VCB_STATE_SHUTDOWN) {
            RFN_DBG((RFN_PFX "commit after shutdown\n"));
        }
        Flush = RfnCommit(Vcb);
    }

    RfnRelease(&Vcb->EngineResource);

    if (NT_SUCCESS(Status) && !NT_SUCCESS(Flush)) {
        Status = Flush;
    }
    return RfnStatus(Status);
}

/* ------------------------------------------------------------------ */
/* The periodic commit                                                 */
/* ------------------------------------------------------------------ */

static VOID
NTAPI
RfnCommitWorker (
    PVOID Context
    )
{
    PRFN_VCB Vcb = (PRFN_VCB)Context;

    if (!(Vcb->VcbState & (VCB_STATE_DISMOUNTED | VCB_STATE_IN_DISMOUNT)) && Vcb->Writing) {
        RfnEngineEnter(Vcb);
        (VOID)RfnCommit(Vcb);
        RfnRelease(&Vcb->EngineResource);
    }
    InterlockedExchange(&Vcb->CommitQueued, 0);
    if (Vcb->TimerSet) {
        LARGE_INTEGER Due;
        Due.QuadPart = -(LONGLONG)RFN_COMMIT_SECONDS * 10000000;
        KeSetTimer(&Vcb->CommitTimer, Due, &Vcb->CommitDpc);
    }
    KeSetEvent(&Vcb->CommitIdle, IO_NO_INCREMENT, FALSE);
}

static VOID
NTAPI
RfnCommitDpc (
    PKDPC Dpc,
    PVOID Context,
    PVOID Arg1,
    PVOID Arg2
    )
{
    PRFN_VCB Vcb = (PRFN_VCB)Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);

    if (!Vcb->TimerSet) {
        return;
    }
    if (InterlockedExchange(&Vcb->CommitQueued, 1) == 0) {
        (VOID)KeResetEvent(&Vcb->CommitIdle);
        ExQueueWorkItem(&Vcb->CommitItem, DelayedWorkQueue);
    }
}

VOID
RfnCommitTimerStart (
    PRFN_VCB Vcb
    )
{
    LARGE_INTEGER Due;

    if (Vcb->TimerSet || Vcb->Vol.ReadOnly) {
        return;
    }
    KeInitializeEvent(&Vcb->CommitIdle, NotificationEvent, TRUE);
    ExInitializeWorkItem(&Vcb->CommitItem, RfnCommitWorker, Vcb);
    KeInitializeDpc(&Vcb->CommitDpc, RfnCommitDpc, Vcb);
    KeInitializeTimer(&Vcb->CommitTimer);
    Due.QuadPart = -(LONGLONG)RFN_COMMIT_SECONDS * 10000000;
    Vcb->TimerSet = TRUE;
    KeSetTimer(&Vcb->CommitTimer, Due, &Vcb->CommitDpc);
}

VOID
RfnCommitTimerStop (
    PRFN_VCB Vcb
    )
{
    if (!Vcb->TimerSet) {
        return;
    }
    Vcb->TimerSet = FALSE;
    KeCancelTimer(&Vcb->CommitTimer);
    KeWaitForSingleObject(&Vcb->CommitIdle, Executive, KernelMode, FALSE, NULL);
    KeCancelTimer(&Vcb->CommitTimer);
}

/* ------------------------------------------------------------------ */
/* Status mapping                                                      */
/* ------------------------------------------------------------------ */

NTSTATUS
RfnStatus (
    RFX_STATUS S
    )
{
    switch (S) {
    case RFX_NO_SPACE:          return STATUS_EA_TOO_LARGE;
    case RFX_TOO_LARGE:         return STATUS_DISK_FULL;
    case RFX_NOT_A_DIRECTORY:   return STATUS_NOT_A_DIRECTORY;
    case RFX_NO_SUCH_EA:        return STATUS_NO_EAS_ON_FILE;
    case RFX_EA_CORRUPT:        return STATUS_EA_CORRUPT_ERROR;
    case RFX_READ_ONLY:         return STATUS_MEDIA_WRITE_PROTECTED;
    default:                    return (NTSTATUS)S;
    }
}

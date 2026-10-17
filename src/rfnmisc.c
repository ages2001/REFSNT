/*
 * REFSNT - IRP_MJ_DEVICE_CONTROL and IRP_MJ_LOCK_CONTROL
 */

#include "rfn.h"

/* Device requests on any of our handles go to the disk */
NTSTATUS
RfnCommonDeviceControl (
    PRFN_IRP_CONTEXT Ctx
    )
{
    PFILE_OBJECT FileObject = Ctx->IrpSp->FileObject;
    PRFN_CCB Ccb = (PRFN_CCB)FileObject->FsContext2;
    PRFN_VCB Vcb = Ctx->Vcb;

    if (FileObject->FsContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * NT 3.1's DeviceIoControl sends file system control codes as device
     * controls: they are ours (the parameter layouts are the same).
     */
    if ((Ctx->IrpSp->Parameters.DeviceIoControl.IoControlCode >> 16) == FILE_DEVICE_FILE_SYSTEM) {
        return RfnUserFsRequest(Ctx);
    }

    /* After a removal the target device may be gone */
    if ((Vcb->VcbState & VCB_STATE_DISMOUNTED) &&
        (Ccb == NULL || !(Ccb->Flags & CCB_FLAG_DISMOUNTED_VOLUME))) {

        return RFN_STATUS_DISMOUNTED;
    }

    RfnSkipStack(Ctx->Irp);
    Ctx->Flags |= RFN_CTX_NO_COMPLETE;

    return IoCallDriver(Vcb->TargetDeviceObject, Ctx->Irp);
}

NTSTATUS
RfnCommonLockControl (
    PRFN_IRP_CONTEXT Ctx
    )
{
    PRFN_FCB Fcb = (PRFN_FCB)Ctx->IrpSp->FileObject->FsContext;
    NTSTATUS Status;

    if (Fcb == NULL || !RfnIsFcb(Fcb)) {
        return STATUS_INVALID_PARAMETER;
    }

    Status = RfnVerifyVcb(Ctx, Fcb->Vcb);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    (VOID)ExAcquireResourceSharedLite(&Fcb->Resource, TRUE);

    __try {

        /* Completes the IRP itself */
        Status = FsRtlProcessFileLock(&Fcb->FileLock, Ctx->Irp, NULL);
        Ctx->Flags |= RFN_CTX_NO_COMPLETE;

        Fcb->Header.IsFastIoPossible = RfnIsFastIoPossible(Fcb);

    } __finally {

        RfnRelease(&Fcb->Resource);
    }

    return Status;
}

#ifdef RFN_DBGPORT
#include <stdarg.h>
/* A small printf to the debug port: %s %c %d %u %x %X %p, l and I64 sizes */
VOID
RfnDbgPrintf (
    const char *Format,
    ...
    )
{
    va_list Ap;
    const char *p;
    char Buf[24];
    int i, Long64, Width, Zero;

    va_start(Ap, Format);
    for (p = Format; *p; p++) {
        if (*p != '%') { RfnDbgCh((unsigned char)*p); continue; }
        p++;
        Long64 = 0;
        Width = 0;
        Zero = (*p == '0');
        while (*p >= '0' && *p <= '9') { Width = Width * 10 + (*p - '0'); p++; }
        if (*p == 'l') p++;
        if (p[0] == 'I' && p[1] == '6' && p[2] == '4') { Long64 = 1; p += 3; }
        switch (*p) {
        case 's': { const char *t = va_arg(Ap, const char *); RfnDbgStr(t ? t : "(null)"); break; }
        case 'c': RfnDbgCh((unsigned char)va_arg(Ap, int)); break;
        case 'd': case 'u': case 'x': case 'X': case 'p': {
            ULONGLONG v = Long64 ? va_arg(Ap, ULONGLONG) :
                          (*p == 'p') ? (ULONGLONG)(ULONG_PTR)va_arg(Ap, PVOID) : (ULONGLONG)va_arg(Ap, ULONG);
            int Base = (*p == 'd' || *p == 'u') ? 10 : 16;
            if (*p == 'd' && !Long64 && (LONG)v < 0) { RfnDbgCh('-'); v = (ULONGLONG)(-(LONG)v); }
            i = 0;
            do { Buf[i++] = "0123456789ABCDEF"[v % Base]; v /= Base; } while (v && i < 22);
            while (Width > i && Width-- > 0) RfnDbgCh(Zero ? '0' : ' ');
            while (i) RfnDbgCh((unsigned char)Buf[--i]);
            break;
        }
        case '%': RfnDbgCh('%'); break;
        default: RfnDbgCh('?'); break;
        }
    }
    va_end(Ap);
}
#endif

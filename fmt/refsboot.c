/*
 * REFSNT - refsboot: makes a ReFS volume start Windows NT (and undoes it).
 *
 *   refsboot E: [/S:dir] [/Q]
 *   refsboot \\.\PhysicalDriveN /P:n [/S:dir] [/Q]
 *   refsboot IMAGE [/P:n] [/S:dir] [/Q]
 *   refsboot ... /U        removes the boot loader and the boot view
 *
 * The volume must be ReFS 3.x with 512-byte sectors and hold NTLDR,
 * NTDETECT.COM and BOOT.INI in its root and the Windows directory (with
 * REFSNT installed to start at boot: refsinst /BOOT). refsboot creates the
 * boot area (REFSBOOT.SYS) near the start of the volume, moves the files the
 * loader reads within reach of the FAT16 view, writes the view (see
 * src/rfsshim.c) and the boot loader in sectors 0-1; the ReFS boot sector
 * moves to sector 2 (its copy at the end of the volume stays). On a disk
 * image it also marks the partition active.
 *
 * While sector 0 holds the loader, Windows 8 and later see the volume as RAW:
 * refsboot /U gives it back its ReFS boot sector.
 *
 * Run it again after changing NTLDR, the kernel, the HAL or a boot driver
 * from a system without REFSNT; REFSNT itself keeps the view current.
 */
#include "rfseng.h"
#include "rfsimg.h"
#include "rfsshim.h"
#include "refsbootbin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#include <sys/time.h>
#define _stricmp strcasecmp
#endif

static RFS_IMAGE Img;

RFS_STATUS RfsHostRead(PRFS_VOL V, ULONGLONG Off, ULONG Len, PVOID Buf) { (void)V; return RimgRead(&Img, Off, Buf, Len) ? RFS_OK : RFS_IO_ERROR; }
RFS_STATUS RfsHostWrite(PRFS_VOL V, ULONGLONG Off, ULONG Len, const VOID *Buf) { (void)V; return RimgWrite(&Img, Off, Buf, Len) ? RFS_OK : RFS_IO_ERROR; }
RFS_STATUS RfsHostFlush(PRFS_VOL V) { (void)V; return RFS_OK; }
PVOID RfsHostAlloc(ULONG Bytes) { return malloc(Bytes ? Bytes : 1); }
VOID RfsHostFree(PVOID P) { free(P); }
ULONGLONG
RfsHostNowNt(VOID)
{
#ifdef _WIN32
    FILETIME ft; SYSTEMTIME st;
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return 116444736000000000ULL + (ULONGLONG)tv.tv_sec * 10000000ULL + (ULONGLONG)tv.tv_usec * 10;
#endif
}

static int
Usage(void)
{
    printf("Makes a ReFS volume start Windows NT.\n\n"
           "refsboot drive: [/S:dir] [/Q] [/U]\n"
           "refsboot \\\\.\\PhysicalDriveN /P:n [/S:dir] [/Q] [/U]\n"
           "refsboot image-file [/P:n] [/S:dir] [/Q] [/U]\n\n"
           "  /S:dir   the Windows directory (default: from BOOT.INI)\n"
           "  /P:n     partition n of a disk or disk image\n"
           "  /T:xx    also set the partition type (hex)\n"
           "  /U       undo: the volume gets its ReFS boot sector back (Windows 8 and\n"
           "           later read it again) and no longer starts Windows NT\n"
           "  /Q       quiet\n\n"
           "The volume must be ReFS 3.x and hold NTLDR, NTDETECT.COM, BOOT.INI and\n"
           "Windows NT with REFSNT installed by refsinst /BOOT.\n");
    return 2;
}

int
main(int argc, char **argv)
{
    const char *Target = NULL, *SysRoot = NULL;
    int I, Slot = -1, Quiet = 0, Undo = 0, Rc = 1;
    unsigned Type = 0;
    RFS_VOL *V = NULL;
    RFS_SHIM_HOST H;
    RFS_SHIM_RESULT R;
    RFS_STATUS St;
    unsigned long Start, Spt, Heads;

    for (I = 1; I < argc; I++) {
        const char *A = argv[I];
        if ((A[0] == '/' || A[0] == '-') && A[1] && (!A[2] || A[2] == ':')) {
            char C = (char)((A[1] >= 'a' && A[1] <= 'z') ? A[1] - 32 : A[1]);
            if (C == 'Q' && !A[2]) Quiet = 1;
            else if (C == 'U' && !A[2]) Undo = 1;
            else if (C == 'S' && A[2] == ':') SysRoot = A + 3;
            else if (C == 'P' && A[2] == ':' && atoi(A + 3) >= 1 && atoi(A + 3) <= 128) Slot = atoi(A + 3) - 1;
            else if (C == 'T' && A[2] == ':') Type = (unsigned)strtoul(A + 3, NULL, 16);
            else return Usage();
        } else if (Target == NULL) {
            Target = A;
        } else {
            return Usage();
        }
    }
    if (Target == NULL) return Usage();
#ifndef _WIN32
    if (Target[0] && Target[1] == ':' && Target[2] == 0) { printf("Drives can be prepared on Windows only.\n"); return 1; }
#endif
    if (!RimgOpen(&Img, Target, Slot, 0)) {
        if (Slot >= 0) printf("Cannot open partition %d of %s.\n", Slot + 1, Target);
        else printf("Cannot open %s.\n", Target);
        return 1;
    }
    if (!RimgLock(&Img)) {
        printf("Cannot lock drive %s: it is in use (open files, a running program,\n"
               "the system or paging file). Close them and try again.\n", Target);
        RimgClose(&Img);
        return 1;
    }
    if (Img.Sector != 512) {
        printf("refsboot failed: the disk has %u-byte sectors; the boot loader needs 512.\n", Img.Sector);
        goto done;
    }
    if (!Quiet) printf(Undo ? "Removing the boot loader from %s...\n" : "Preparing %s to start Windows NT...\n", Target);
    V = (RFS_VOL *)calloc(1, sizeof(RFS_VOL));
    if (!V) { printf("Out of memory.\n"); goto done; }
    V->DeviceBytes = Img.Size;
    V->SectorSize = Img.Sector;
    St = RfsMount(V, FALSE);
    if (!RFS_SUCCESS(St)) {
        printf("refsboot failed: this is not a ReFS volume REFSNT reads (%08lX).\n", (unsigned long)St);
        goto done;
    }
    if (V->ReadOnly) {
        printf("refsboot failed: REFSNT cannot write this volume (%s).\n", V->ReadOnlyWhy ? V->ReadOnlyWhy : "read-only");
        RfsUnmount(V);
        goto done;
    }
    memset(&H, 0, sizeof(H));
    memset(&R, 0, sizeof(R));
    H.Vol = V;
    RimgGeometry(&Img, &Start, &Spt, &Heads);
    H.PartitionStart = Start;
    H.SectorsPerTrack = Spt;
    H.Heads = Heads;
    if (Undo) {
        St = RfsShimRemove(&H, &R);
    } else {
        St = RfsShimReserve(&H, &R);
        if (RFS_SUCCESS(St)) St = RfsShimUpdate(&H, SysRoot, RfsBootCode, sizeof(RfsBootCode), &R);
    }
    if (!RFS_SUCCESS(RfsCommit(V)) && RFS_SUCCESS(St)) { St = RFS_IO_ERROR; R.Error = "write error"; }
    RfsUnmount(V);
    if (!RFS_SUCCESS(St)) {
        printf("refsboot failed: %s%s%.11s%s\n", R.Error ? R.Error : "error",
               R.ErrorName[0] ? " (" : "", (const char *)R.ErrorName, R.ErrorName[0] ? ")" : "");
        goto done;
    }
    Rc = 0;
    if (Undo) {
        if (!Quiet) printf("Done. The volume is plain ReFS again.\n");
        goto done;
    }
    if (!Quiet) {
        if (R.MovedOut) printf("  %lu files moved further out to make room\n", (unsigned long)R.MovedOut);
        printf("  boot view: %lu files in %lu directories (Windows in \\%s), %lu moved near the start, %lu KB clusters\n",
               (unsigned long)R.Files, (unsigned long)R.Dirs, R.WinDir, (unsigned long)R.MovedIn,
               (unsigned long)(R.ClusterBytes >> 10));
    }
    if (Type) {
        int T = RimgSetType(&Img, (unsigned char)Type);
        if (T == 0) printf("Note: the partition type could not be set to %02Xh.\n", Type);
    }
    if (RimgSetActive(&Img) == 0) printf("Note: the partition could not be marked active.\n");
#ifdef _WIN32
    if (Img.IsDrive && !Quiet) printf("Mark the partition active (Disk Administrator) if it is to start the computer.\n");
#endif
    if (!Quiet) printf("Done. Windows 8 and later see the volume as RAW until refsboot /U.\n");
done:
    free(V);
    RimgUnlock(&Img);
    RimgClose(&Img);
    return Rc;
}

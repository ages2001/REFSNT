/*
 * REFSNT - refsfmt: formats a volume as ReFS.
 *
 *   refsfmt drive: [/FS:ReFS|ReFSv1|ReFSv2|ReFSv3] [/A:4096|64K] [/V:label] [/T:xx] [/Q] [/Y]
 *   refsfmt \\.\PhysicalDriveN /P:n [options]
 *   refsfmt image-file [/P:n] [options]
 *
 *   /FS:ReFSv3   ReFS 3.14 (Windows 10 1709 and later, Windows 11, Server 2019
 *                and later); the default, also /FS:ReFS
 *   /FS:ReFSv2   ReFS 2.0 (Windows Server 2016 Technical Preview / TH2)
 *   /FS:ReFSv1   ReFS 1.2 (Windows 8.1, Server 2012 R2), like Windows'
 *                own "format /FS:ReFSv1"
 *   /R:1|2|3     the same as /FS:ReFSv1 ... ReFSv3
 *   /A:size      cluster size: 4096 (the default) or 64K; ReFS 1.2 always
 *                uses 64K
 *   /V:label     volume label (up to 32 characters); /L:label too
 *   /T:xx        partition type (hex) to set; 07 by default
 *   /Q           quick (the only kind there is: the metadata is written, the
 *                data area is not)
 *   /Y           no confirmation
 *
 * ReFS 2.0 and 3.14 are made from templates of volumes Windows formatted
 * (fmt/rfsmkfs.c); ReFS 1.2 is built from scratch (fmt/rfsmk1.c). Every
 * volume is mounted again before refsfmt reports success. The same program
 * runs on Windows NT 3.1 - XP x64 and on Linux (images and block devices).
 */
#include "rfseng.h"
#include "rfsimg.h"
#include "rfsmkfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#include <sys/time.h>
#include <unistd.h>
#define _stricmp strcasecmp
#endif

static RFS_IMAGE Img;
static int Quiet;

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
    GetSystemTime(&st);                 /* (GetSystemTimeAsFileTime: not on NT 3.1) */
    SystemTimeToFileTime(&st, &ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return 116444736000000000ULL + (ULONGLONG)tv.tv_sec * 10000000ULL + (ULONGLONG)tv.tv_usec * 10;
#endif
}

/* serial numbers and the volume GUID: not cryptographic, just different each time */
static ULONGLONG RndState;
static ULONGLONG
Rnd(VOID)
{
    RndState ^= RndState << 13;
    RndState ^= RndState >> 7;
    RndState ^= RndState << 17;
    return RndState;
}

static VOID
Seed(VOID)
{
    RndState = RfsHostNowNt() ^ 0x9E3779B97F4A7C15ULL;
#ifdef _WIN32
    RndState ^= (ULONGLONG)GetTickCount() << 32;
    RndState ^= (ULONGLONG)GetCurrentProcessId() << 16;
#else
    RndState ^= (ULONGLONG)getpid() << 16;
#endif
    if (!RndState) RndState = 1;
    Rnd(); Rnd();
}

static void
Progress(PVOID Ctx, ULONG Pct)
{
    (void)Ctx;
    if (Quiet) return;
    printf("\r%3lu percent completed.", (unsigned long)Pct);
    if (Pct >= 100) printf("\n");
    fflush(stdout);
}

static int
Usage(void)
{
    printf("Formats a volume as ReFS.\n\n"
           "refsfmt drive: [/FS:ReFS|ReFSv1|ReFSv2|ReFSv3] [/A:4096|64K] [/V:label] [/T:xx] [/Q] [/Y]\n"
           "refsfmt \\\\.\\PhysicalDriveN /P:n [options]\n"
           "refsfmt image-file [/P:n] [options]\n\n"
           "  /FS:ReFSv3  ReFS 3.14 (Windows 10 1709 and later, 11, Server 2019 and later);\n"
           "              the default, also /FS:ReFS\n"
           "  /FS:ReFSv2  ReFS 2.0 (Windows Server 2016 TH2)\n"
           "  /FS:ReFSv1  ReFS 1.2 (Windows 8.1, Server 2012 R2)\n"
           "  /R:1|2|3    the same as /FS:ReFSv1, ReFSv2, ReFSv3\n"
           "  /A:size     cluster size, 4096 (default) or 64K; ReFS 1.2: 64K only\n"
           "  /V:label    volume label (also /L:label)\n"
           "  /P:n        partition n of a disk or disk image\n"
           "  /T:xx       partition type to set (hex, default 07)\n"
           "  /Q          quick format (refsfmt always formats quickly)\n"
           "  /Y          do not ask for confirmation\n");
    return 2;
}

static ULONG
ParseSize(const char *S)
{
    char *e;
    unsigned long v = strtoul(S, &e, 10);
    if (*e == 'K' || *e == 'k') { v *= 1024; e++; }
    return *e ? 0 : (ULONG)v;
}

int
main(int argc, char **argv)
{
    const char *Target = NULL, *Label = NULL;
    int I, Slot = -1, Yes = 0, Rc = 1, Version = 3;
    unsigned Type = 0x07;
    ULONG Cluster = 0;
    RFS_VOL *V = NULL;
    RFS_STATUS St;
    RFS_FMT_RESULT R;
    RFS_FMT_LIMITS L;

    for (I = 1; I < argc; I++) {
        const char *A = argv[I];
        if (A[0] == '/' || A[0] == '-') {
            const char *O = A + 1;
            if (!_stricmp(O, "Q")) ;
            else if (!_stricmp(O, "Y")) Yes = 1;
            else if (!_stricmp(O, "QUIET")) Quiet = 1;
            else if (!_stricmp(O, "FS:ReFS") || !_stricmp(O, "FS:ReFSv3")) Version = 3;
            else if (!_stricmp(O, "FS:ReFSv2")) Version = 2;
            else if (!_stricmp(O, "FS:ReFSv1")) Version = 1;
            else if ((O[0] == 'R' || O[0] == 'r') && O[1] == ':' && O[2] >= '1' && O[2] <= '3' && (!O[3] || O[3] == '.')) Version = O[2] - '0';
            else if ((O[0] == 'A' || O[0] == 'a') && O[1] == ':') { Cluster = ParseSize(O + 2); if (!Cluster) return Usage(); }
            else if ((O[0] == 'V' || O[0] == 'v' || O[0] == 'L' || O[0] == 'l') && O[1] == ':') Label = O + 2;
            else if ((O[0] == 'P' || O[0] == 'p') && O[1] == ':' && atoi(O + 2) >= 1 && atoi(O + 2) <= 128) Slot = atoi(O + 2) - 1;
            else if ((O[0] == 'T' || O[0] == 't') && O[1] == ':') Type = (unsigned)strtoul(O + 2, NULL, 16);
            else { printf("Unknown option %s\n\n", A); return Usage(); }
        } else if (Target == NULL) {
            Target = A;
        } else {
            return Usage();
        }
    }
    if (Target == NULL) return Usage();
    if (Label && strlen(Label) > 32) { printf("The label is too long (32 characters at most).\n"); return 1; }

    /* the version and cluster size */
    if (Version == 1) {
        if (Cluster && Cluster != 65536) { printf("ReFS 1.2 volumes have 64K clusters.\n"); return 1; }
        Cluster = 65536;
    } else {
        if (!Cluster) Cluster = 4096;
        if (!RfsFormatQuery((UCHAR)Version, Cluster, &L)) {
            printf("ReFS %s volumes can have 4096-byte or 64K clusters.\n", Version == 3 ? "3.14" : "2.0");
            return 1;
        }
    }
#ifndef _WIN32
    if (Target[0] && Target[1] == ':' && Target[2] == 0) { printf("Drives can be formatted on Windows only.\n"); return 1; }
#endif
    if (!RimgOpen(&Img, Target, Slot, 0)) {
        if (Slot >= 0) printf("Cannot open partition %d of %s.\n", Slot + 1, Target);
        else printf("Cannot open %s.\n", Target);
        return 1;
    }
    if (Img.Sector > 4096) { printf("The disk has %u-byte sectors; ReFS takes 512 to 4096.\n", Img.Sector); goto close; }
    if (Version != 1 && Img.Size < L.MinBytes) {
        printf("The volume is too small: ReFS %s with %lu-byte clusters needs at least %lu MB here.\n",
               Version == 3 ? "3.14" : "2.0", (unsigned long)Cluster, (unsigned long)(L.MinBytes >> 20));
        goto close;
    }
    if (!Yes) {
        int Ch, Ans = 0;
        if (Img.Slot >= 0) printf("Partition %d of %s (%lu MB)", Img.Slot + 1, Target, (unsigned long)(Img.Size >> 20));
        else printf("%s (%lu MB)", Target, (unsigned long)(Img.Size >> 20));
        printf(" will be formatted as ReFS %s. Everything on it is lost.\nProceed with format (Y/N)? ",
               Version == 3 ? "3.14" : Version == 2 ? "2.0" : "1.2");
        fflush(stdout);
        while ((Ch = getchar()) != -1 && Ch != '\n') if (!Ans && Ch != ' ' && Ch != '\r') Ans = Ch;
        if (Ans != 'y' && Ans != 'Y') { Rc = 1; goto close; }
    }
    if (!RimgLock(&Img)) {
        printf("Cannot lock drive %s: it is in use (open files, a running program,\n"
               "the system or paging file). Close them and try again.\n", Target);
        goto close;
    }
    V = (RFS_VOL *)calloc(1, sizeof(RFS_VOL));
    if (!V) { printf("Out of memory.\n"); goto unlock; }
    V->DeviceBytes = Img.Size;
    V->SectorSize = Img.Sector;
    Seed();
    if (!Quiet) printf("Formatting %lu MB as ReFS %s, %lu-byte clusters...\n", (unsigned long)(Img.Size >> 20),
                       Version == 3 ? "3.14" : Version == 2 ? "2.0" : "1.2", (unsigned long)Cluster);
    memset(&R, 0, sizeof(R));
    if (Version == 1) {
        St = RfsFormatV1(V, Label ? Label : "", Cluster);
        if (RFS_SUCCESS(St)) {
            /* read back */
            RfsZero(V, sizeof(*V));
            V->DeviceBytes = Img.Size;
            V->SectorSize = Img.Sector;
            St = RfsMount(V, FALSE);
            if (RFS_SUCCESS(St)) {
                R.Major = V->Major; R.Minor = V->Minor; R.ClusterSize = V->ClusterSize;
                R.Bytes = V->TotalClusters * 16384ULL;
                R.FreeClusters = V->FreeClusters;
                R.TotalClusters = R.Bytes / V->ClusterSize;
                if (V->ReadOnly) St = RFS_CORRUPT;
                RfsUnmount(V);
            }
        }
    } else {
        UCHAR Guid[16];
        ULONGLONG A = Rnd(), B = Rnd();
        RfsPut64(Guid, A);
        RfsPut64(Guid + 8, B);
        Guid[7] = (UCHAR)((Guid[7] & 0x0F) | 0x40);         /* a version 4 GUID */
        Guid[8] = (UCHAR)((Guid[8] & 0x3F) | 0x80);
        St = RfsFormatTemplate(V, (UCHAR)Version, Cluster, Label, Rnd(), Guid, Progress, NULL, &R);
    }
    if (!RFS_SUCCESS(St)) {
        if (St == RFS_DISK_FULL) printf("\nThe volume is too small for ReFS.\n");
        else if (St == RFS_IO_ERROR) printf("\nA write failed: the disk is damaged or protected.\n");
        else printf("\nThe format failed (%08lX).\n", (unsigned long)(ULONG)St);
        goto unlock;
    }
#ifdef _WIN32
    if (Img.Slot >= 0 || Img.IsDrive) {
#else
    if (Img.Slot >= 0) {
#endif
        int T = RimgSetType(&Img, (unsigned char)Type);
        if (T == 0) printf("Note: the partition type could not be set to %02Xh.\n", Type);
        else if (T == 1 && !Quiet) printf("Partition type set to %02Xh.\n", Type);
    }
    if (!Quiet) {
        printf("ReFS %u.%u, %lu-byte clusters%s%s%s.\n", R.Major, R.Minor, (unsigned long)R.ClusterSize,
               Label && *Label ? ", label \"" : "", Label && *Label ? Label : "", Label && *Label ? "\"" : "");
        printf("%12lu MB total volume size\n%12lu MB available\n", (unsigned long)(R.Bytes >> 20),
               (unsigned long)((R.FreeClusters * R.ClusterSize) >> 20));
    }
    Rc = 0;
unlock:
    free(V);
    RimgUnlock(&Img);
close:
    RimgClose(&Img);
    return Rc;
}

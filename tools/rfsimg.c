/*
 * REFSNT - rfsimg: a drive, a disk (one MBR partition of it) or an image
 * file for the tools (Windows NT 3.1 and later, Linux). Byte-addressed reads and writes, staged through a
 * sector-aligned buffer so a raw Windows device sees only whole sectors.
 */
#include "rfsimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winioctl.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#ifdef __linux__
#include <linux/fs.h>
#endif
#endif

#define BOUNCE  (1u << 20)

static int
RawRead(RFS_IMAGE *I, unsigned long long Off, void *Buf, unsigned Len)
{
#ifdef _WIN32
    LONG High = (LONG)(Off >> 32); DWORD Done;
    /* SetFilePointer, not ...Ex: NT 3.1 - 4.0 have only this one */
    if (SetFilePointer(I->Handle, (LONG)(ULONG)Off, &High, FILE_BEGIN) == 0xFFFFFFFF && GetLastError() != NO_ERROR) return 0;
    if (!ReadFile(I->Handle, Buf, Len, &Done, NULL)) return 0;
    if (Done < Len) memset((char *)Buf + Done, 0, Len - Done);
    return 1;
#else
    ssize_t R = pread(I->Fd, Buf, Len, (off_t)Off);
    if (R < 0) return 0;
    if ((unsigned)R < Len) memset((char *)Buf + R, 0, Len - (unsigned)R);
    return 1;
#endif
}

static int
RawWrite(RFS_IMAGE *I, unsigned long long Off, const void *Buf, unsigned Len)
{
#ifdef _WIN32
    LONG High = (LONG)(Off >> 32); DWORD Done;
    if (SetFilePointer(I->Handle, (LONG)(ULONG)Off, &High, FILE_BEGIN) == 0xFFFFFFFF && GetLastError() != NO_ERROR) return 0;
    return WriteFile(I->Handle, Buf, Len, &Done, NULL) && Done == Len;
#else
    return pwrite(I->Fd, Buf, Len, (off_t)Off) == (ssize_t)Len;
#endif
}

/* aligned I/O through the bounce buffer; Write may read-modify-write the edges */
static int
Xfer(RFS_IMAGE *I, unsigned long long Off, void *Buf, unsigned Len, int Write)
{
    unsigned char *B = (unsigned char *)Buf;
    if (I->Size && Off + Len > I->Size) return 0;
    Off += I->Offset;
    while (Len) {
        unsigned long long A = Off & ~(unsigned long long)(I->Sector - 1);
        unsigned Lead = (unsigned)(Off - A);
        unsigned Chunk = Len, Span;
        if (Chunk > BOUNCE - Lead) Chunk = BOUNCE - Lead;
        Span = (Lead + Chunk + I->Sector - 1) & ~(I->Sector - 1);
        if (!Write) {
            if (!RawRead(I, A, I->Bounce, Span)) return 0;
            memcpy(B, I->Bounce + Lead, Chunk);
        } else {
            if ((Lead || (Chunk & (I->Sector - 1))) && !RawRead(I, A, I->Bounce, Span)) return 0;
            memcpy(I->Bounce + Lead, B, Chunk);
            if (!RawWrite(I, A, I->Bounce, Span)) return 0;
        }
        Off += Chunk; B += Chunk; Len -= Chunk;
    }
    return 1;
}

int RimgRead(RFS_IMAGE *I, unsigned long long Off, void *Buf, unsigned Len) { return Xfer(I, Off, Buf, Len, 0); }
int RimgWrite(RFS_IMAGE *I, unsigned long long Off, const void *Buf, unsigned Len) { return !I->ReadOnly && Xfer(I, Off, (void *)Buf, Len, 1); }

static unsigned long
Get32(const unsigned char *P) { return P[0] | (P[1] << 8) | ((unsigned long)P[2] << 16) | ((unsigned long)P[3] << 24); }

/* a ReFS boot sector at byte Base of the target */
static int
HasRefs(RFS_IMAGE *I, unsigned long long Base)
{
    unsigned char B[1536];
    if (!RawRead(I, Base, B, 1536)) return 0;
    if (!memcmp(B + 3, "ReFS\0\0\0\0", 8) && !memcmp(B + 16, "FSRS", 4)) return 1;
    /* made bootable by refsboot: the loader in sectors 0-1, the ReFS boot sector in sector 2 */
    return !memcmp(B + 3, "REFSNTB1", 8) && !memcmp(B + 1024 + 3, "ReFS\0\0\0\0", 8) && !memcmp(B + 1024 + 16, "FSRS", 4);
}

static unsigned long long
Get64(const unsigned char *P) { return Get32(P) | ((unsigned long long)Get32(P + 4) << 32); }

/* a GPT: the first partition holding ReFS (Slot < 0) or partition Slot */
static int
FindGpt(RFS_IMAGE *I, int Slot)
{
    unsigned char H[512], E[128];
    unsigned ss, k;
    for (ss = 512; ss <= 4096; ss *= 8) {
        unsigned long long ent;
        unsigned cnt, sz;
        if (!RawRead(I, ss, H, 512) || memcmp(H, "EFI PART", 8)) continue;
        ent = Get64(H + 0x48); cnt = Get32(H + 0x50); sz = Get32(H + 0x54);
        if (sz < 128 || sz > 512 || cnt > 1024) return -2;
        for (k = 0; k < cnt; k++) {
            unsigned long long first, last;
            if (!RawRead(I, ent * ss + (unsigned long long)k * sz, E, 128)) return -2;
            first = Get64(E + 0x20); last = Get64(E + 0x28);
            if (!first) continue;
            if (Slot >= 0 ? (int)k != Slot : !HasRefs(I, first * ss)) continue;
            I->Offset = first * ss;
            I->Size = (last - first + 1) * ss;
            return (int)k;
        }
        return Slot >= 0 ? -2 : -3;
    }
    return -3;
}

/* a partition table (not a file system's boot sector) -> pick the partition */
static int
FindPartition(RFS_IMAGE *I, int Slot)
{
    unsigned char Mbr[512];
    int K;
    if (Slot < 0 && HasRefs(I, 0)) return -1;           /* a bare volume */
    if (!RawRead(I, 0, Mbr, 512) || Mbr[510] != 0x55 || Mbr[511] != 0xAA) return Slot >= 0 ? -2 : -1;
    for (K = 0; K < 4; K++) if (Mbr[446 + 16 * K + 4] == 0xEE) break;
    if (K < 4) {
        int g = FindGpt(I, Slot);
        if (g != -3) return g;
    }
    for (K = 0; K < 4; K++) {
        unsigned char *E = Mbr + 446 + 16 * K;
        unsigned long long Start = (unsigned long long)Get32(E + 8) * 512;
        if (Slot >= 0 && K != Slot) continue;
        if (E[4] == 0 || E[4] == 0x05 || E[4] == 0x0F || E[4] == 0xEE) { if (Slot >= 0) return -2; continue; }
        if (Slot < 0 && !HasRefs(I, Start)) continue;
        I->Offset = Start;
        I->Size = (unsigned long long)Get32(E + 12) * 512;
        return K;
    }
    if (Slot < 0) {
        /* no ReFS yet (formatting): the first partition */
        for (K = 0; K < 4; K++) {
            unsigned char *E = Mbr + 446 + 16 * K;
            if (E[4] == 0 || E[4] == 0x05 || E[4] == 0x0F || E[4] == 0xEE) continue;
            I->Offset = (unsigned long long)Get32(E + 8) * 512;
            I->Size = (unsigned long long)Get32(E + 12) * 512;
            return K;
        }
    }
    return Slot >= 0 ? -2 : -1;
}

int
RimgOpen(RFS_IMAGE *I, const char *Path, int Slot, int ReadOnly)
{
    memset(I, 0, sizeof(*I));
    I->Slot = -1;
    I->ReadOnly = ReadOnly;
    I->Sector = 512;
#ifdef _WIN32
    {
        char Name[300];
        DWORD N;
        I->IsDrive = (Path[0] && Path[1] == ':' && Path[2] == 0);
        if (I->IsDrive) sprintf(Name, "\\\\.\\%c:", Path[0]);
        else { strncpy(Name, Path, sizeof(Name) - 1); Name[sizeof(Name) - 1] = 0; }
        I->Handle = CreateFileA(Name, ReadOnly ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE),
                                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (I->Handle == INVALID_HANDLE_VALUE) return 0;
        I->Bounce = (unsigned char *)VirtualAlloc(NULL, BOUNCE, MEM_COMMIT, PAGE_READWRITE);
        if (!I->Bounce) { CloseHandle(I->Handle); return 0; }
        {
            DISK_GEOMETRY G;
            if (DeviceIoControl(I->Handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &G, sizeof(G), &N, NULL) &&
                G.BytesPerSector >= 512 && G.BytesPerSector <= 65536)
                I->Sector = G.BytesPerSector;
        }
        if (I->IsDrive) {
            PARTITION_INFORMATION Pi;
            if (DeviceIoControl(I->Handle, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0, &Pi, sizeof(Pi), &N, NULL))
                I->Size = (unsigned long long)Pi.PartitionLength.QuadPart;
            else {
                /* CD/DVD drives have no partition: the whole medium */
                DISK_GEOMETRY Dg;
                if (DeviceIoControl(I->Handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &Dg, sizeof(Dg), &N, NULL))
                    I->Size = (unsigned long long)Dg.Cylinders.QuadPart * Dg.TracksPerCylinder * Dg.SectorsPerTrack * Dg.BytesPerSector;
            }
            return I->Size != 0;
        } else {
            DISK_GEOMETRY Dg;
            DWORD High = 0, Low = GetFileSize(I->Handle, &High);
            unsigned long long Whole = ((unsigned long long)High << 32) | Low;
            if (Low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) Whole = 0;
            if (Whole == 0 && DeviceIoControl(I->Handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &Dg, sizeof(Dg), &N, NULL))
                Whole = (unsigned long long)Dg.Cylinders.QuadPart * Dg.TracksPerCylinder * Dg.SectorsPerTrack * Dg.BytesPerSector;
            I->Slot = FindPartition(I, Slot);
            if (I->Slot == -2) { RimgClose(I); return 0; }
            if (I->Slot < 0) I->Size = Whole;
            return I->Size != 0;
        }
    }
#else
    {
        struct stat St;
        I->Fd = open(Path, ReadOnly ? O_RDONLY : O_RDWR);
        if (I->Fd < 0) return 0;
        I->Bounce = (unsigned char *)malloc(BOUNCE);
        if (!I->Bounce) { close(I->Fd); return 0; }
        if (fstat(I->Fd, &St) == 0) I->Size = (unsigned long long)St.st_size;
#ifdef BLKGETSIZE64
        if (S_ISBLK(St.st_mode)) { unsigned long long B; if (ioctl(I->Fd, BLKGETSIZE64, &B) == 0) I->Size = B; }
#endif
        I->Slot = FindPartition(I, Slot);
        if (I->Slot == -2) { RimgClose(I); return 0; }
        return I->Size != 0;
    }
#endif
}

void
RimgClose(RFS_IMAGE *I)
{
#ifdef _WIN32
    if (I->Handle && I->Handle != INVALID_HANDLE_VALUE) CloseHandle(I->Handle);
    if (I->Bounce) VirtualFree(I->Bounce, 0, MEM_RELEASE);
    I->Handle = NULL;
#else
    if (I->Fd >= 0) close(I->Fd);
    free(I->Bounce);
    I->Fd = -1;
#endif
    I->Bounce = NULL;
}

int
RimgLock(RFS_IMAGE *I)
{
#ifdef _WIN32
    DWORD N;
    if (!I->IsDrive) return 1;
    if (!DeviceIoControl(I->Handle, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &N, NULL)) return 0;
    I->Locked = 1;
    DeviceIoControl(I->Handle, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &N, NULL);
#endif
    (void)I;
    return 1;
}

void
RimgUnlock(RFS_IMAGE *I)
{
#ifdef _WIN32
    DWORD N;
    if (!I->IsDrive || !I->Locked) return;
    I->Locked = 0;
    DeviceIoControl(I->Handle, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &N, NULL);
    DeviceIoControl(I->Handle, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &N, NULL);
#endif
    (void)I;
}

/* the partition type: through the volume (a drive) or in the MBR (a disk) */
int
RimgSetType(RFS_IMAGE *I, unsigned char Type)
{
    unsigned char Mbr[512];
#ifdef _WIN32
    if (I->IsDrive) {
        PARTITION_INFORMATION Pi; SET_PARTITION_INFORMATION Sp; DWORD N;
        if (!DeviceIoControl(I->Handle, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0, &Pi, sizeof(Pi), &N, NULL)) return 0;
        if (Pi.PartitionType == Type) return 2;
        Sp.PartitionType = Type;
        return DeviceIoControl(I->Handle, IOCTL_DISK_SET_PARTITION_INFO, &Sp, sizeof(Sp), NULL, 0, &N, NULL) ? 1 : 0;
    }
#endif
    if (I->Slot < 0) return 2;                  /* a bare file system: no table */
    if (!RawRead(I, 0, Mbr, 512)) return 0;
    if (Mbr[446 + 16 * I->Slot + 4] == Type) return 2;
    Mbr[446 + 16 * I->Slot + 4] = Type;
    return RawWrite(I, 0, Mbr, 512);
}

/* For a boot sector: where the partition starts (sectors) and the disk's geometry */
void
RimgGeometry(RFS_IMAGE *I, unsigned long *Start, unsigned long *Spt, unsigned long *Heads)
{
    *Start = (unsigned long)(I->Offset / 512);
    *Spt = 63;
    *Heads = 255;
#ifdef _WIN32
    if (I->IsDrive) {
        PARTITION_INFORMATION Pi; DISK_GEOMETRY G; DWORD N;
        if (DeviceIoControl(I->Handle, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0, &Pi, sizeof(Pi), &N, NULL))
            *Start = Pi.HiddenSectors;
        if (DeviceIoControl(I->Handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0, &G, sizeof(G), &N, NULL) &&
            G.SectorsPerTrack && G.TracksPerCylinder) {
            *Spt = G.SectorsPerTrack;
            *Heads = G.TracksPerCylinder;
        }
    }
#endif
}

/* A disk (image): the partition becomes the only active one */
int
RimgSetActive(RFS_IMAGE *I)
{
    unsigned char Mbr[512];
    int K;
#ifdef _WIN32
    if (I->IsDrive) return 2;
#endif
    if (I->Slot < 0) return 2;
    if (!RawRead(I, 0, Mbr, 512)) return 0;
    for (K = 0; K < 4; K++) Mbr[446 + 16 * K] = (unsigned char)(K == I->Slot ? 0x80 : 0);
    return RawWrite(I, 0, Mbr, 512);
}

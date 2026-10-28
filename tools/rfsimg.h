/* REFSNT - rfsimg: the device the tools work on (see rfsimg.c) */
#ifndef _RFSIMG_H_
#define _RFSIMG_H_

#ifdef _WIN32
#include <windows.h>
#endif

typedef struct {
#ifdef _WIN32
    HANDLE              Handle;
    int                 IsDrive;
    int                 Locked;
#else
    int                 Fd;
#endif
    unsigned long long  Offset;     /* bytes: start of the file system */
    unsigned long long  Size;       /* bytes */
    int                 Slot;       /* MBR slot of the partition, -1 = none */
    int                 ReadOnly;
    unsigned char      *Bounce;     /* sector-aligned staging buffer */
    unsigned            Sector;     /* the device sector size */
} RFS_IMAGE;

/* Path: "E:" (Windows), \\.\PhysicalDriveN, an image file or a device.
   Slot: MBR partition 0..3, or -1 for "the first partition holding a ReFS volume if
   the target holds a partition table, else the whole target". */
int  RimgOpen(RFS_IMAGE *I, const char *Path, int Slot, int ReadOnly);
void RimgClose(RFS_IMAGE *I);
int  RimgRead(RFS_IMAGE *I, unsigned long long Off, void *Buf, unsigned Len);
int  RimgWrite(RFS_IMAGE *I, unsigned long long Off, const void *Buf, unsigned Len);
/* Windows drives: lock + dismount before writing; set the partition type */
int  RimgLock(RFS_IMAGE *I);
void RimgUnlock(RFS_IMAGE *I);
int  RimgSetType(RFS_IMAGE *I, unsigned char Type);
/* boot: where the partition starts and the geometry; mark the partition active (disk images) */
void RimgGeometry(RFS_IMAGE *I, unsigned long *Start, unsigned long *Spt, unsigned long *Heads);
int  RimgSetActive(RFS_IMAGE *I);

#endif

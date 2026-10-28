/* REFSNT - the FAT16 boot view of a ReFS volume (see rfsshim.c) */
#ifndef _RFSSHIM_H_
#define _RFSSHIM_H_

#include "rfseng.h"

#define RFS_SHIM_OEM            "REFSNTB1"      /* bytes 3-10 of the loader's boot sector (sector 0) */
#define RFS_SHIM_WINDIR         0x3F0           /* the Windows directory, 16 bytes, at this offset of the loader (sector 1) */
#define RFS_SHIM_AREA_NAME      "REFSBOOT.SYS"  /* the boot area: a hidden system file in the root */

/* What the view is built through: the mounted, writable volume */
typedef struct _RFS_SHIM_HOST {
    PRFS_VOL    Vol;
    PVOID       Ctx;
    /* optional: a commit between files (default RfsCommit); may a file's data move; it moved */
    RFS_STATUS  (*Commit)(PVOID Ctx);
    BOOLEAN     (*CanMove)(PVOID Ctx, ULONGLONG Home, ULONGLONG FileId);
    VOID        (*Moved)(PVOID Ctx, ULONGLONG Home, ULONGLONG FileId);
    /* for a new boot sector: the BPB's geometry */
    ULONG       PartitionStart, SectorsPerTrack, Heads;
} RFS_SHIM_HOST, *PRFS_SHIM_HOST;

typedef struct _RFS_SHIM_RESULT {
    const char *Error;                  /* why it failed (NULL: it did not) */
    UCHAR       ErrorName[12];          /* the file it is about, if any */
    ULONG       Files, Dirs;
    ULONG       MovedIn;                /* boot files moved into reach */
    ULONG       MovedOut;               /* other files moved out of the way */
    ULONG       ClusterBytes;
    char        WinDir[16];
} RFS_SHIM_RESULT, *PRFS_SHIM_RESULT;

/* Creates the boot area file (REFSBOOT.SYS) low on the volume, if there is none */
RFS_STATUS RfsShimReserve(PRFS_SHIM_HOST Host, PRFS_SHIM_RESULT Result);

/*
 * Builds the boot view (Code: the boot loader, 1024 bytes,
 * written with its BPB) or brings it up to date (Code NULL). WinDir: the
 * Windows directory, or NULL: the one recorded in the boot sectors, else
 * BOOT.INI's default entry.
 */
RFS_STATUS RfsShimUpdate(PRFS_SHIM_HOST Host, const char *WinDir, const UCHAR *Code, ULONG CodeSize, PRFS_SHIM_RESULT Result);

/* Takes the view away: the ReFS boot sector back to sector 0, REFSBOOT.SYS removed */
RFS_STATUS RfsShimRemove(PRFS_SHIM_HOST Host, PRFS_SHIM_RESULT Result);

/* TRUE if the volume carries a view (from sector 0 and 1, 1024 bytes) */
BOOLEAN RfsShimPresent(const UCHAR *Sectors);

/* The window of a view: physical clusters [Lo, Hi) kept for the boot files (allocated last) */
BOOLEAN RfsShimWindow(PRFS_VOL Vol, const UCHAR *Sectors, PULONGLONG Lo, PULONGLONG Hi);

#endif

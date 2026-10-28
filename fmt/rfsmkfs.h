/*
 * REFSNT - the formatters: ReFS 1.2 (fmt/rfsmk1.c, made from scratch) and
 * ReFS 2.0 / 3.14 (fmt/rfsmkfs.c, from templates of Windows-formatted volumes).
 */
#ifndef _RFSMKFS_H_
#define _RFSMKFS_H_

typedef VOID (*RFS_FMT_PROGRESS)(PVOID Ctx, ULONG Percent);

typedef struct _RFS_FMT_LIMITS {
    UCHAR       Minor;              /* the version the template makes (3.14, 2.0) */
    ULONGLONG   ContainerBytes;     /* the volume is a whole number of these */
    ULONGLONG   MinBytes;           /* the smallest volume */
} RFS_FMT_LIMITS, *PRFS_FMT_LIMITS;

typedef struct _RFS_FMT_RESULT {
    UCHAR       Major, Minor;
    ULONG       ClusterSize;
    ULONGLONG   TotalClusters, FreeClusters, Bytes;
} RFS_FMT_RESULT, *PRFS_FMT_RESULT;

/* is there a template for ReFS Major (2, 3) with ClusterSize-byte clusters? */
BOOLEAN     RfsFormatQuery(UCHAR Major, ULONG ClusterSize, PRFS_FMT_LIMITS Limits);

/*
 * Formats the device (V: zeroed but for HostContext, SectorSize and
 * DeviceBytes; the host's RfsHostRead/Write reach it) as ReFS Major. V is
 * left unmounted. RFS_DISK_FULL: the device is too small.
 */
RFS_STATUS  RfsFormatTemplate(PRFS_VOL V, UCHAR Major, ULONG ClusterSize, const char *Label,
                              ULONGLONG Serial, const UCHAR *Guid, RFS_FMT_PROGRESS Progress, PVOID Ctx,
                              PRFS_FMT_RESULT Result);

#endif

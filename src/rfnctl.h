/*
 * REFSNT - private file system controls (driver and tools)
 *
 *   RFN_FSCTL_QUERY_VOLUME  any handle of the volume: RFN_VOLUME_INFO
 *   RFN_FSCTL_GET_POSIX     any file handle: RFN_POSIX_INFO (mode, owner,
 *                           links, inode number, flags)
 *   RFN_FSCTL_SET_POSIX     file handle opened with FILE_WRITE_ATTRIBUTES:
 *                           input RFN_POSIX_INFO, Valid says which fields
 *   RFN_FSCTL_READ_LINK     a symbolic link opened as itself (DELETE access
 *                           or FILE_OPEN_REPARSE_POINT): the target text
 *   RFN_FSCTL_CREATE_LINK   a directory handle: input RFN_LINK_REQUEST
 *                           makes a symbolic link Name -> Target in it
 *   RFN_FSCTL_COMMIT        any handle: commit the running transaction now
 */

#ifndef _RFNCTL_H_
#define _RFNCTL_H_

#define RFN_FSCTL_QUERY_VOLUME  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B0, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RFN_FSCTL_GET_POSIX     CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B1, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RFN_FSCTL_SET_POSIX     CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RFN_FSCTL_READ_LINK     CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B3, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RFN_FSCTL_CREATE_LINK   CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B4, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define RFN_FSCTL_COMMIT        CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 0x8B5, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _RFN_VOLUME_INFO {
    ULONG       Version;            /* 1 */
    ULONG       BlockSize;          /* the logical block size */
    ULONGLONG   BlocksCount;        /* the volume */
    ULONGLONG   FreeBlocks;         /* in the partition file data goes to */
    ULONG       Revision;           /* ReFS version, e.g. 0x030E */
    ULONG       Maps;               /* partition maps */
    ULONGLONG   DataBlocks;         /* the partition file data goes to */
    ULONG       Files;              /* as the integrity descriptor counts them */
    ULONG       Directories;
    ULONG       ReadOnly;           /* mounted read-only (asked or forced) */
    ULONG       ReadOnlyForced;     /* a feature or state REFSNT cannot write */
    ULONG       Commits;            /* transactions written */
    UCHAR       Uuid[16];
    char        Label[64];
} RFN_VOLUME_INFO, *PRFN_VOLUME_INFO;

#define RFN_POSIX_MODE          0x01
#define RFN_POSIX_UID           0x02
#define RFN_POSIX_GID           0x04

typedef struct _RFN_POSIX_INFO {
    ULONG       Valid;              /* SET: which fields to apply */
    ULONG       Mode;               /* type and permission bits (st_mode) */
    ULONG       Uid;
    ULONG       Gid;
    ULONG       Links;
    ULONG       Inode;
    ULONG       Flags;              /* ICB flags */
    ULONG       Generation;
} RFN_POSIX_INFO, *PRFN_POSIX_INFO;

typedef struct _RFN_LINK_REQUEST {
    USHORT      NameLength;         /* bytes of UTF-16 */
    USHORT      TargetLength;       /* bytes of the UTF-8 target that follows the name */
    WCHAR       Name[1];            /* then the target (UTF-8, not terminated) */
} RFN_LINK_REQUEST, *PRFN_LINK_REQUEST;

#endif

/*
 * REFSNT - the engine interface the driver layer uses (rfx.c implements it
 * on the ReFS engine).
 *
 * The driver layer was built around an inode-number, block-mapped file
 * system interface. rfx.c gives ReFS that shape:
 *
 *   - inode numbers: a directory is its object id (the root is RFX_ROOT_INO);
 *     a file gets a number of its own (top bit set) the first time its name
 *     is seen, kept for the mount: files have no object of their own, only
 *     a row in their directory (and a per-directory file id);
 *   - blocks are clusters; the data map is the file's extents, translated
 *     to physical clusters; space is given at the end of a file (zeroed where
 *     the caller does not write);
 *   - files whose data is kept in their record carry RFX_IF_INLINE_DATA:
 *     the first write moves it into clusters (RfxUninline);
 *   - DOS attributes are ReFS's own: read-only is the owner write bit,
 *     hidden/system/archive are offered as user.DOSATTRIB.
 */
#ifndef _RFX_H_
#define _RFX_H_

#include "rfseng.h"

typedef LONG RFX_STATUS;

#define RFX_OK                  ((RFX_STATUS)0x00000000L)
#define RFX_NO_MORE_ENTRIES     ((RFX_STATUS)0x8000001AL)
#define RFX_NOT_FOUND           ((RFX_STATUS)0xC0000034L)
#define RFX_EXISTS              ((RFX_STATUS)0xC0000035L)
#define RFX_DISK_FULL           ((RFX_STATUS)0xC000007FL)
#define RFX_NO_MEMORY           ((RFX_STATUS)0xC000009AL)
#define RFX_CORRUPT             ((RFX_STATUS)0xC0000032L)
#define RFX_FILE_CORRUPT        ((RFX_STATUS)0xC0000102L)
#define RFX_BAD_NAME            ((RFX_STATUS)0xC0000033L)
#define RFX_NOT_EMPTY           ((RFX_STATUS)0xC0000101L)
#define RFX_IO_ERROR            ((RFX_STATUS)0xC0000185L)
#define RFX_READ_ONLY           ((RFX_STATUS)0xC00000A2L)
#define RFX_UNRECOGNIZED        ((RFX_STATUS)0xC000014FL)
#define RFX_TOO_LARGE           ((RFX_STATUS)0xC0000904L)
#define RFX_NOT_SUPPORTED       ((RFX_STATUS)0xC00000BBL)
#define RFX_NOT_A_DIRECTORY     ((RFX_STATUS)0xC0000103L)
#define RFX_IS_A_DIRECTORY      ((RFX_STATUS)0xC00000BAL)
#define RFX_NAME_TOO_LONG       ((RFX_STATUS)0xC0000106L)
#define RFX_NO_SUCH_EA          ((RFX_STATUS)0xC0000052L)
#define RFX_EA_CORRUPT          ((RFX_STATUS)0x80000013L)
#define RFX_BUFFER_TOO_SMALL    ((RFX_STATUS)0xC0000023L)
#define RFX_ACCESS_DENIED       ((RFX_STATUS)0xC0000022L)
#define RFX_TOO_MANY_LINKS      ((RFX_STATUS)0xC0000265L)
#define RFX_NO_SPACE            ((RFX_STATUS)0xC0000907L)

#define RFX_MAX_NAME            255
#define RFX_ROOT_INO            2           /* the root directory */
#define RFX_VIRTUAL_INO         0x80000000UL /* files: numbered by a table */

/* the in-memory inode (fields as the driver layer knows them) */
typedef struct _RFX_INODE {
    USHORT      Mode;
    USHORT      UidLo;
    ULONG       SizeLo;
    ULONG       Atime;
    ULONG       Ctime;
    ULONG       Mtime;
    ULONG       Dtime;
    USHORT      GidLo;
    USHORT      LinksCount;
    ULONG       BlocksLo;               /* 512-byte sectors */
    ULONG       Flags;                  /* RFX_IF_* */
    ULONG       Version;
    ULONG       Block[15];
    ULONG       Generation;
    ULONG       FileAclLo;
    ULONG       SizeHi;
    ULONG       ObsoFaddr;
    USHORT      BlocksHi;
    USHORT      FileAclHi;
    USHORT      UidHi;
    USHORT      GidHi;
    USHORT      ChecksumLo;
    USHORT      Reserved;
    USHORT      ExtraIsize;
    USHORT      ChecksumHi;
    ULONG       CtimeExtra;             /* nanoseconds << 2 | epoch bits */
    ULONG       MtimeExtra;
    ULONG       AtimeExtra;
    ULONG       Crtime;
    ULONG       CrtimeExtra;
    ULONG       VersionHi;
    ULONG       ProjId;
    /* ReFS */
    ULONGLONG   UniqueId;
} RFX_INODE, *PRFX_INODE;

#define RFX_GOOD_OLD_INODE_SIZE 128

#define RFX_S_IFMT              0xF000
#define RFX_S_IFSOCK            0xC000
#define RFX_S_IFLNK             0xA000
#define RFX_S_IFREG             0x8000
#define RFX_S_IFBLK             0x6000
#define RFX_S_IFDIR             0x4000
#define RFX_S_IFCHR             0x2000
#define RFX_S_IFIFO             0x1000

#define RFX_IF_SYNC             0x00000008
#define RFX_IF_IMMUTABLE        0x00000010
#define RFX_IF_APPEND           0x00000020
#define RFX_IF_NODUMP           0x00000040
#define RFX_IF_NOATIME          0x00000080
#define RFX_IF_INLINE_DATA      0x10000000  /* read through the engine; rewritten on first write */
#define RFX_IF_NEEDS_PREP       0x20000000  /* (not used on ReFS) */
#define RFX_IF_SUBVOL_ROOT      0x40000000  /* (not used on ReFS) */

#define RFX_FT_UNKNOWN          0
#define RFX_FT_REG_FILE         1
#define RFX_FT_DIR              2
#define RFX_FT_SYMLINK          7

#define RFX_XATTR_INDEX_USER            1
#define RFX_XATTR_INDEX_POSIX_ACCESS    2
#define RFX_XATTR_INDEX_POSIX_DEFAULT   3
#define RFX_XATTR_INDEX_TRUSTED         4
#define RFX_XATTR_INDEX_SECURITY        6
#define RFX_XATTR_INDEX_SYSTEM          7

typedef struct _RFX_ENTRY {
    ULONG       Inode;
    UCHAR       FileType;
    USHORT      NameLength;
    char        Name[RFX_MAX_NAME + 1];
    ULONGLONG   Cookie;
} RFX_ENTRY, *PRFX_ENTRY;

/* a file the driver knows by a number of its own: where its name row is */
#define RFX_VINO_SHIFT  8
#define RFX_VINO_CHUNK  (1UL << RFX_VINO_SHIFT)

typedef struct _RFX_VINO {
    ULONGLONG   Parent;                 /* the directory holding its name */
    ULONGLONG   Home;                   /* the directory holding its record */
    ULONGLONG   FileId;
    UCHAR       KeyFlags;
    UCHAR       Gone;                   /* removed */
    USHORT      NameLen;
    USHORT     *Name;
} RFX_VINO;

/* an open engine file (the inode cache) */
typedef struct _RFX_NODE {
    struct _RFX_NODE *Next;             /* hash */
    struct _RFX_NODE *Prev2, *Next2;    /* LRU */
    ULONG       Ino;
    PRFS_FILE   F;
    BOOLEAN     Dirty;
} RFX_NODE, *PRFX_NODE;

#define RFX_NODE_HASH           256
#define RFX_NODE_MAX            512

/* where the last enumeration of a directory stopped */
typedef struct _RFX_ENUMPOS {
    ULONG       DirIno;
    ULONGLONG   Cookie;
    USHORT      NameLen;
    USHORT      Name[RFS_MAX_NAME + 1];
} RFX_ENUMPOS;

#define RFX_ENUM_SLOTS          8

typedef struct _RFX_VOL {
    PVOID       HostContext;            /* the VCB */
    RFS_VOL     Rfs;                    /* the ReFS volume */
    BOOLEAN     Mounted;
    /* set by the layer before RfxMount */
    ULONGLONG   DeviceBytes;
    ULONG       DeviceSector;
    BOOLEAN     Optical;

    ULONG       BlockSize;              /* the cluster size */
    ULONG       BlockShift;
    ULONG       SectorsPerBlock;
    ULONGLONG   BlocksCount;            /* the volume, in clusters */
    ULONGLONG   DataBlocks;             /* what the allocator covers */
    ULONGLONG   FreeBlocksCount;
    ULONG       InodesCount, FreeInodesCount;
    ULONG       InodeSize;
    ULONG       GroupCount;
    ULONG       NodeSize;
    char        VolumeName[256];        /* the label (UTF-8) */
    ULONG       SerialNumber;
    UCHAR       Uuid[16];
    BOOLEAN     ReadOnly;
    BOOLEAN     ReadOnlyForced;         /* the volume cannot be written by REFSNT */
    BOOLEAN     Dirty;
    BOOLEAN     DataCopies;             /* never */

    RFX_VINO  **VinoChunk;              /* RFX_VINO_CHUNK entries each (no large allocations) */
    ULONG       VinoCount, VinoCap;     /* entries, entries the chunks hold */
    ULONG       VinoChunks, VinoChunkCap;
    PULONG      VinoHash;               /* (Home, FileId) -> index + 1 */
    ULONG       VinoHashSize;
    PRFX_NODE   NodeHash[RFX_NODE_HASH];
    PRFX_NODE   LruHead, LruTail;
    ULONG       NodeCount;
    RFX_ENUMPOS Enum[RFX_ENUM_SLOTS];
    ULONG       EnumNext;
} RFX_VOL, *PRFX_VOL;

/* the host (rfnhost.c) */
PVOID       RfxHostAlloc(ULONG Bytes);
VOID        RfxHostFree(PVOID P);
ULONG       RfxHostNow(VOID);

/* volume */
BOOLEAN     RfxIsRefs(const UCHAR *Sector0);                       /* a ReFS boot sector */
BOOLEAN     RfxSameVolume(PRFX_VOL Vol);                            /* still the volume mounted? */
RFX_STATUS  RfxMount(PRFX_VOL Vol, BOOLEAN ReadOnly);
VOID        RfxUnmountFree(PRFX_VOL Vol);
RFX_STATUS  RfxStartWriting(PRFX_VOL Vol);
RFX_STATUS  RfxStopWriting(PRFX_VOL Vol);
RFX_STATUS  RfxTxnCommit(PRFX_VOL Vol);
BOOLEAN     RfxTxnPending(PRFX_VOL Vol);
ULONG       RfxDirtyBytes(PRFX_VOL Vol);
RFX_STATUS  RfxSetLabel(PRFX_VOL Vol, const char *Label);
RFX_STATUS  RfxProcessOrphans(PRFX_VOL Vol);
VOID        RfxCountFree(PRFX_VOL Vol);
/* operation boundaries: the engine's time stamp, and failure handling */
VOID        RfxOpBegin(PRFX_VOL Vol);
RFX_STATUS  RfxOpEnd(PRFX_VOL Vol, RFX_STATUS Status);

/* times */
VOID        RfxSetTime(ULONG *Field, ULONG *Extra, ULONGLONG Nt100ns);
ULONGLONG   RfxGetTime(ULONG Field, ULONG Extra);

/* inodes */
RFX_STATUS  RfxReadInode(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode);
RFX_STATUS  RfxWriteInode(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode);
ULONGLONG   RfxInodeSize(PRFX_VOL Vol, PRFX_INODE Inode);
VOID        RfxInodeSetSize(PRFX_VOL Vol, PRFX_INODE Inode, ULONGLONG Size);
ULONGLONG   RfxInodeBlocks(PRFX_VOL Vol, PRFX_INODE Inode);
ULONGLONG   RfxInodeNumber(PRFX_VOL Vol, ULONG Ino);        /* the unique ID */
ULONG       RfxInoOfFile(PRFX_VOL Vol, ULONGLONG Home, ULONGLONG FileId);   /* 0: never seen */
VOID        RfxForget(PRFX_VOL Vol, ULONG Ino);

/* directories */
RFX_STATUS  RfxLookup(PRFX_VOL Vol, ULONG DirIno, PRFX_INODE Dir, const char *Name, ULONG NameLen, PRFX_ENTRY Entry);
RFX_STATUS  RfxEnum(PRFX_VOL Vol, ULONG DirIno, PRFX_INODE Dir, ULONGLONG Cookie, PRFX_ENTRY Entry);
RFX_STATUS  RfxDirIsEmpty(PRFX_VOL Vol, ULONG DirIno, PRFX_INODE Dir, PBOOLEAN Empty);
RFX_STATUS  RfxGetParent(PRFX_VOL Vol, ULONG DirIno, PRFX_INODE Dir, PULONG Parent);
RFX_STATUS  RfxCreate(PRFX_VOL Vol, ULONG DirIno, PRFX_INODE Dir, const char *Name, ULONG NameLen,
                      USHORT Mode, const char *Target, PULONG NewIno, PRFX_INODE NewInode);
RFX_STATUS  RfxUnlink(PRFX_VOL Vol, ULONG DirIno, PRFX_INODE Dir, const char *Name, ULONG NameLen);
RFX_STATUS  RfxRemoveEntry(PRFX_VOL Vol, ULONG DirIno, PRFX_INODE Dir, const char *Name, ULONG NameLen);
RFX_STATUS  RfxLink(PRFX_VOL Vol, ULONG DirIno, PRFX_INODE Dir, const char *Name, ULONG NameLen,
                    ULONG TargetIno, PRFX_INODE Target);
RFX_STATUS  RfxRename(PRFX_VOL Vol, ULONG OldDir, PRFX_INODE OldDirInode, const char *OldName, ULONG OldLen,
                      ULONG NewDir, PRFX_INODE NewDirInode, const char *NewName, ULONG NewLen);
RFX_STATUS  RfxReadLink(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, char *Buf, ULONG BufSize, PULONG Len);

/* file data */
RFX_STATUS  RfxReadFile(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, ULONGLONG Offset, ULONG Length,
                        PVOID Buffer, PULONG Read);
RFX_STATUS  RfxMapFileBlock(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, ULONGLONG FileBlk,
                            PULONGLONG Disk, PULONG RunLen);
RFX_STATUS  RfxGetFileBlock(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, ULONGLONG FileBlk,
                            PULONGLONG Disk, PBOOLEAN Fresh);
/* gives [FirstBlk, FirstBlk+Count) disk space where it has none (prepared files) */
RFX_STATUS  RfxAllocateRange(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, ULONGLONG FirstBlk, ULONGLONG Count);
RFX_STATUS  RfxUninline(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode);
RFX_STATUS  RfxTruncateBlocks(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, ULONGLONG NewBlocks);
/* the device blocks of the other copies of a data block (DUP data) */
ULONG       RfxDataMirrors(PRFX_VOL Vol, ULONGLONG DiskBlock, PULONGLONG Other, ULONG Max);

/* extended attributes (NameIndex: the namespace) */
RFX_STATUS  RfxGetXattr(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, UCHAR NameIndex, const char *Name,
                        ULONG NameLen, PUCHAR Value, ULONG ValueSize, PULONG ValueLen);
RFX_STATUS  RfxSetXattr(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, UCHAR NameIndex, const char *Name,
                        ULONG NameLen, const UCHAR *Value, ULONG ValueLen);
RFX_STATUS  RfxListXattr(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode,
                         VOID (*Report)(PVOID Ctx, UCHAR NameIndex, const char *Name, ULONG NameLen,
                                        const UCHAR *Value, ULONG ValueLen), PVOID Ctx);

#endif

/*
 * REFSNT - the ReFS engine: everything that understands the on-disk format,
 * shared by the driver (through rfx.c), the tools and the Linux test harness.
 *
 * One thread at a time (the host serializes).
 *
 * Reading: pages come through a cache; a B+ tree is walked with a cursor.
 * Writing is copy-on-write, as ReFS does it: a change loads the nodes on its
 * path into memory ("nodes"), changes them there, and a commit writes every
 * changed node to a newly allocated page, bottom up, puts the new page
 * references into the parents (object table rows, checkpoint roots), frees
 * the old pages and writes the other checkpoint. Until the commit the
 * previous checkpoint describes the volume.
 */
#ifndef _RFSENG_H_
#define _RFSENG_H_

#ifdef RFS_KERNEL
#include "rfsnt.h"
#else
#include "rfsusr.h"
#endif
#include "refs.h"

typedef LONG RFS_STATUS;

#define RFS_OK                  ((RFS_STATUS)0x00000000L)
#define RFS_NO_MORE             ((RFS_STATUS)0x8000001AL)
#define RFS_NOT_FOUND           ((RFS_STATUS)0xC0000034L)
#define RFS_EXISTS              ((RFS_STATUS)0xC0000035L)
#define RFS_DISK_FULL           ((RFS_STATUS)0xC000007FL)
#define RFS_NO_MEMORY           ((RFS_STATUS)0xC000009AL)
#define RFS_CORRUPT             ((RFS_STATUS)0xC0000032L)
#define RFS_FILE_CORRUPT        ((RFS_STATUS)0xC0000102L)
#define RFS_BAD_NAME            ((RFS_STATUS)0xC0000033L)
#define RFS_NOT_EMPTY           ((RFS_STATUS)0xC0000101L)
#define RFS_IO_ERROR            ((RFS_STATUS)0xC0000185L)
#define RFS_READ_ONLY           ((RFS_STATUS)0xC00000A2L)
#define RFS_UNRECOGNIZED        ((RFS_STATUS)0xC000014FL)
#define RFS_NOT_SUPPORTED       ((RFS_STATUS)0xC00000BBL)
#define RFS_NOT_A_DIRECTORY     ((RFS_STATUS)0xC0000103L)
#define RFS_IS_A_DIRECTORY      ((RFS_STATUS)0xC00000BAL)
#define RFS_NAME_TOO_LONG       ((RFS_STATUS)0xC0000106L)
#define RFS_CHECKSUM            ((RFS_STATUS)0xC0000032L)   /* (reported as corruption) */
#define RFS_INVALID             ((RFS_STATUS)0xC000000DL)

#define RFS_SUCCESS(s)          ((LONG)(s) >= 0)

#define RFS_MAX_NAME            255         /* UTF-16 code units */

/* a page reference: up to four clusters (3.x) or one 16 KB block (1.x), and the page's checksum */
typedef struct _RFS_REF {
    ULONGLONG   Lcn[4];
    USHORT      Flags;
    UCHAR       CkType;
    UCHAR       CkLen;
    UCHAR       Ck[32];
} RFS_REF, *PRFS_REF;

/* one row of a node, as the engine works with it */
typedef struct _RFS_ROW {
    PUCHAR      Key;
    PUCHAR      Val;
    USHORT      KeyLen;
    USHORT      Flags;                  /* the row header's flags */
    ULONG       ValLen;
    USHORT      Marker;                 /* the upper half of the key-index entry */
    UCHAR       Owned;                  /* Key and Val are one allocation of the row's own */
    UCHAR       Overlap;                /* the value begins with the key (key offset == value offset) */
    struct _RFS_NODE *Child;            /* index rows: the child, once loaded */
} RFS_ROW, *PRFS_ROW;

/* a node in memory (decoded from a page, or from a row value for an embedded table) */
typedef struct _RFS_NODE {
    struct _RFS_TABLE *Table;
    UCHAR       Level;
    UCHAR       Flags;                  /* REFS_NF_* */
    BOOLEAN     Dirty;
    BOOLEAN     IsRoot;
    ULONG       NRows, CapRows;
    PRFS_ROW    Rows;
    PUCHAR      Buf;                    /* the bytes rows point into (owned) */
    ULONG       BufLen;
    BOOLEAN     HasRef;                 /* read from a page (Ref) */
    RFS_REF     Ref;
    PUCHAR      Desc;                   /* root: the index root descriptor and header data (owned) */
    ULONG       DescLen;
    UCHAR       PageHdr[0x50];          /* the page header as read (clocks, table id) */
    ULONG       NodeHdr[10];            /* the node header as read */
    /* writing */
    BOOLEAN     Placed;                 /* NewRef is this commit's location */
    RFS_REF     NewRef;
} RFS_NODE, *PRFS_NODE;

/* where a table's root lives */
#define RFS_TROOT_CHECKPOINT    1       /* checkpoint root Index */
#define RFS_TROOT_OBJECT        2       /* object table row Oid */
#define RFS_TROOT_EMBEDDED      3       /* a row value (in memory) */

typedef struct _RFS_TABLE {
    struct _RFS_TABLE *HashNext;
    struct _RFS_TABLE *LruPrev, *LruNext;
    struct _RFS_VOL *Vol;
    UCHAR       Where;                  /* RFS_TROOT_* */
    UCHAR       Rule;                   /* key comparison rule (from the schema) */
    BOOLEAN     Real;                   /* real (physical) cluster numbers */
    BOOLEAN     Dirty;
    ULONG       Index;                  /* checkpoint root */
    ULONGLONG   Oid;                    /* object table row; also the table id pages carry */
    ULONG       Schema;
    ULONG       Refs;
    RFS_REF     RootRef;                /* the root page (not for embedded tables) */
    PRFS_NODE   Root;
    ULONGLONG   TreeClock;              /* the root page's tree clock */
    UCHAR       Tier;                   /* the allocator its pages come from (RFS_TIER_*) */
    BOOLEAN     OnDirtyList;
    struct _RFS_TABLE *DirtyNext;
    struct _RFS_TABLE *Owner;           /* embedded tables: the table holding the row (page table id, clocks) */
} RFS_TABLE, *PRFS_TABLE;

/* allocators (3.x tiers) */
#define RFS_TIER_MEDIUM         0       /* root 1: data and most metadata */
#define RFS_TIER_CONTAINER      1       /* root 2: the pages of tables 1, 2, 6 and 11 */
#define RFS_TIER_SMALL          2       /* root 12: the container tables' pages (never changed here) */
#define RFS_TIER_NONE           3
#define RFS_NTIERS              3

/* what an allocation is for (container preference) */
#define RFS_FOR_META            0
#define RFS_FOR_DATA            1

/* a position in a table */
#define RFS_MAX_DEPTH           8
typedef struct _RFS_CURSOR {
    PRFS_TABLE  Table;
    ULONG       Depth;
    PRFS_NODE   Node[RFS_MAX_DEPTH];
    ULONG       Slot[RFS_MAX_DEPTH];
    BOOLEAN     Valid;
    BOOLEAN     HoldsRef;               /* the cursor keeps its table open (until RfsCursorClose) */
} RFS_CURSOR, *PRFS_CURSOR;

/* the page cache */
typedef struct _RFS_PAGE {
    struct _RFS_PAGE *HashNext;
    struct _RFS_PAGE *Prev, *Next;
    ULONGLONG   Addr;                   /* first physical cluster (3.x) or block (1.x) */
    ULONG       Refs;
    PUCHAR      Data;
} RFS_PAGE, *PRFS_PAGE;

#define RFS_PAGE_HASH           512
#define RFS_TABLE_HASH          256

typedef struct _RFS_CONTAINER {
    ULONGLONG   Phys;                   /* first physical cluster, or ~0: not mapped */
} RFS_CONTAINER;

/* a cluster run freed since the last commit: not to be reused before the next checkpoint is written */
typedef struct _RFS_RUN {
    ULONGLONG   Start;                  /* physical */
    ULONGLONG   Count;
} RFS_RUN, *PRFS_RUN;

typedef struct _RFS_VOL {
    PVOID       HostContext;
    ULONG       SectorSize;             /* the device's */
    ULONGLONG   DeviceBytes;

    /* format */
    UCHAR       Major, Minor;
    BOOLEAN     V1;
    BOOLEAN     V2;                     /* ReFS 2.x (Server 2016 TH2): containers, 64 MiB bands, 10 roots */
    ULONG       ClusterSize, ClusterShift;
    ULONG       PageSize, PageShift;
    ULONG       ClustersPerPage;        /* 3.x: 4 (4 KB clusters) or 1 */
    ULONG       HeaderSize;             /* page header: 80 or 48 */
    ULONG       RefSize;                /* page reference: 104, 48, 72 or 24 */
    UCHAR       CkType;                 /* page references' checksum type */
    UCHAR       SelfCkType;             /* the superblock's and checkpoints' self checksum */
    ULONGLONG   TotalClusters;          /* 3.x clusters, 1.x 16 KB blocks */
    ULONGLONG   Serial;
    ULONG       VolSig;
    UCHAR       Guid[16];
    ULONG       VbrFlags;

    /* containers (3.x) */
    ULONG       ClustersPerContainer;
    ULONG       ContainerShift;         /* VLCN >> shift = container id */
    PULONGLONG  ContPhys;               /* container id -> first physical cluster (~0: none) */
    ULONG       NConts;

    /* the checkpoint in use */
    PUCHAR      Chkp;                   /* its page */
    ULONGLONG   ChkpLcn[2];             /* the two slots */
    ULONG       ChkpSlot;               /* the slot Chkp came from */
    ULONGLONG   Clock;
    ULONGLONG   AllocClock;
    ULONG       ChkpFlags;
    ULONG       NRoots;
    RFS_REF     Roots[REFS3_NROOTS];
    ULONG       RootOff[REFS3_NROOTS];  /* where each root reference sits in the checkpoint */

    /* well-known tables */
    PRFS_TABLE  Object;                 /* object table */
    PRFS_TABLE  Schema;
    PUSHORT     Upcase;                 /* 65536 entries */
    UCHAR       Rules[0x200];           /* schema id >> 4 (0x100..0x1FF ids, 0xE000.. ids folded) -> rule */
    UCHAR       SysRules[0x20];         /* (schema - 0xE000) >> 4 -> rule */

    /* caches */
    PRFS_PAGE   PageHash[RFS_PAGE_HASH];
    PRFS_PAGE   PageHead, PageTail;
    ULONG       PageCount, PageMax;
    PRFS_TABLE  TableHash[RFS_TABLE_HASH];
    PRFS_TABLE  TableLruHead, TableLruTail;
    ULONG       TableCount, TableMax;

    /* state */
    BOOLEAN     ReadOnly;
    BOOLEAN     ReadOnlyForced;
    const char *ReadOnlyWhy;
    ULONG       ChecksumErrors;
    BOOLEAN     VerifyChecksums;
    char        Label[3 * 32 + 1];      /* UTF-8 */
    ULONGLONG   NextOid;
    PUCHAR      Scratch;                /* one page */

    /* writing */
    PRFS_TABLE  Global[REFS3_NROOTS];   /* checkpoint-root tables opened for changes */
    PRFS_TABLE  DirtyHead;              /* tables with changes to commit */
    PRFS_RUN    Quarantine;             /* runs freed since the last commit */
    ULONG       NQuar, CapQuar;
    PUCHAR      ContFlags;              /* container id -> class (RFS_CC_*) */
    PULONG      PhysToCont;             /* physical cluster / CPC -> container id (~0: none) */
    ULONG       NPhysConts;
    ULONGLONG   Hint[RFS_NTIERS][2];    /* where the last allocation ended (physical), per tier and purpose */
    BOOLEAN     AllocChanged;           /* the allocators changed since the last commit */
    BOOLEAN     Dirty;                  /* anything to commit */
    BOOLEAN     InCommit;
    UCHAR       AllocFormat[RFS_NTIERS];/* the format byte of each tier's bitmap rows */
    ULONGLONG   FreeClusters;           /* Medium tier: clusters not allocated (the summary) */
    ULONG       DataRunMax;             /* the longest extent written (clusters) */
    ULONGLONG   AvoidLo, AvoidHi;       /* physical clusters allocated last (the boot view's window) */
    BOOLEAN     Formatting;             /* refsfmt: the small allocator (container tables) may change */
    BOOLEAN     BootView;               /* refsboot made it bootable (sectors 0-1: the boot loader) */
    UCHAR       BootSectors[1024];      /* those sectors as mounted */
} RFS_VOL, *PRFS_VOL;

/* container classes */
#define RFS_CC_AVOID            0       /* system / other tiers */
#define RFS_CC_META             1
#define RFS_CC_DATA             2
#define RFS_CC_ANY              3       /* unused or unclassified */

/* ---- the host (driver: rfnhost.c; tools: their own) --------------------------- */
RFS_STATUS  RfsHostRead(PRFS_VOL Vol, ULONGLONG Offset, ULONG Length, PVOID Buffer);
RFS_STATUS  RfsHostWrite(PRFS_VOL Vol, ULONGLONG Offset, ULONG Length, const VOID *Buffer);
RFS_STATUS  RfsHostFlush(PRFS_VOL Vol);
PVOID       RfsHostAlloc(ULONG Bytes);
VOID        RfsHostFree(PVOID P);
ULONGLONG   RfsHostNowNt(VOID);

/* ---- support (rfssup.c) ------------------------------------------------------------ */
USHORT      RfsGet16(const VOID *P);
ULONG       RfsGet32(const VOID *P);
ULONGLONG   RfsGet64(const VOID *P);
VOID        RfsPut16(PVOID P, USHORT V);
VOID        RfsPut32(PVOID P, ULONG V);
VOID        RfsPut64(PVOID P, ULONGLONG V);
VOID        RfsZero(PVOID P, ULONG N);
VOID        RfsCopy(PVOID D, const VOID *S, ULONG N);
VOID        RfsMove(PVOID D, const VOID *S, ULONG N);
int         RfsCmp(const VOID *A, const VOID *B, ULONG N);
PVOID       RfsAllocZero(ULONG Bytes);
ULONG       RfsCrc32c(ULONG Crc, const VOID *Data, ULONG Length);       /* Crc: previous result (0 to start) */
ULONGLONG   RfsCrc64(ULONGLONG Crc, const VOID *Data, ULONG Length);    /* CRC-64/NVME, same chaining */
VOID        RfsSha256(const VOID *Data, ULONG Length, UCHAR Digest[32]);
/* the checksum of a buffer of the given type into Out (4, 8 or 32 bytes); returns its length */
ULONG       RfsChecksum(UCHAR Type, const VOID *Data, ULONG Length, PUCHAR Out);
USHORT      RfsVbrChecksum(const UCHAR *Vbr, ULONG Size);
ULONG       RfsUtf16ToUtf8(const USHORT *In, ULONG InLen, char *Out, ULONG OutMax);
ULONG       RfsUtf8ToUtf16(const char *In, ULONG InLen, PUSHORT Out, ULONG OutMax);    /* (ULONG)-1: bad */
VOID        RfsDefaultUpcase(PUSHORT Table);

/* ---- volume (rfsvol.c) ----------------------------------------------------------------- */
BOOLEAN     RfsProbe(const UCHAR *Sector0);
BOOLEAN     RfsProbeView(const UCHAR *Sectors);    /* sectors 0-2: a bootable volume's loader + boot sector */
RFS_STATUS  RfsMount(PRFS_VOL Vol, BOOLEAN ReadOnly);
VOID        RfsUnmount(PRFS_VOL Vol);
RFS_STATUS  RfsReadClusters(PRFS_VOL Vol, ULONGLONG Lcn, ULONG Count, PVOID Buf);    /* physical */
RFS_STATUS  RfsTranslate(PRFS_VOL Vol, ULONGLONG Vlcn, PULONGLONG Plcn, PULONG Run); /* virtual -> physical */
/* references */
VOID        RfsRefGet(PRFS_VOL Vol, const UCHAR *P, PRFS_REF Ref);
VOID        RfsRefPut(PRFS_VOL Vol, PUCHAR P, const RFS_REF *Ref);
BOOLEAN     RfsRefValid(PRFS_VOL Vol, const RFS_REF *Ref);
/* pages */
RFS_STATUS  RfsPageRead(PRFS_VOL Vol, const RFS_REF *Ref, BOOLEAN Real, PRFS_PAGE *Page);
VOID        RfsPagePut(PRFS_VOL Vol, PRFS_PAGE Page);
VOID        RfsPageDropAll(PRFS_VOL Vol);
VOID        RfsPageForget(PRFS_VOL Vol, ULONGLONG Addr);

/* ---- B+ trees (rfstree.c) --------------------------------------------------------------- */
UCHAR       RfsRuleOf(PRFS_VOL Vol, ULONG Schema);
BOOLEAN     RfsNodeBare(PRFS_TABLE Table, PRFS_NODE Node);
VOID        RfsRecordKey(UCHAR Rule, const RFS_ROW *Row, PUCHAR Key);
int         RfsKeyCompare(PRFS_VOL Vol, UCHAR Rule, const UCHAR *A, ULONG ALen, const UCHAR *B, ULONG BLen);
RFS_STATUS  RfsNodeDecode(PRFS_VOL Vol, PRFS_TABLE Table, const UCHAR *Buf, ULONG Len, ULONG HdrOff, BOOLEAN Root, PRFS_NODE *Node);
VOID        RfsNodeFree(PRFS_NODE Node);
RFS_STATUS  RfsLoadChild(PRFS_VOL Vol, PRFS_TABLE Table, PRFS_NODE Parent, ULONG Slot, PRFS_NODE *Child);
/* tables */
RFS_STATUS  RfsTableOpenRoot(PRFS_VOL Vol, ULONG RootIndex, PRFS_TABLE *Table);     /* checkpoint root */
RFS_STATUS  RfsTableOpenObject(PRFS_VOL Vol, ULONGLONG Oid, PRFS_TABLE *Table);     /* via the object table */
RFS_STATUS  RfsTableOpenEmbedded(PRFS_VOL Vol, ULONGLONG OwnerOid, const UCHAR *Val, ULONG Len, PRFS_TABLE *Table);
VOID        RfsTableClose(PRFS_VOL Vol, PRFS_TABLE Table);
/* cursors: Seek positions at the first row >= Key (Key NULL: the first row) */
RFS_STATUS  RfsSeek(PRFS_TABLE Table, const UCHAR *Key, ULONG KeyLen, PRFS_CURSOR Cur);
RFS_STATUS  RfsNext(PRFS_CURSOR Cur);                   /* to the next row; RFS_NO_MORE at the end */
PRFS_ROW    RfsCurRow(PRFS_CURSOR Cur);
VOID        RfsCursorClose(PRFS_CURSOR Cur);
RFS_STATUS  RfsLookup(PRFS_TABLE Table, const UCHAR *Key, ULONG KeyLen, PRFS_CURSOR Cur);   /* exact */

/* ---- files (rfsfile.c) --------------------------------------------------------------------- */
#define RFS_FT_FILE             1
#define RFS_FT_DIR              2

typedef struct _RFS_TIMES {
    ULONGLONG   Create, Write, Change, Access;
} RFS_TIMES;

/* a directory entry, as enumeration returns it */
typedef struct _RFS_DIRENT {
    USHORT      Name[RFS_MAX_NAME + 1];
    ULONG       NameLen;                /* code units */
    UCHAR       Type;                   /* RFS_FT_* */
    UCHAR       KeyFlags;               /* REFS_NAME_EMBEDDED / _INDEX */
    ULONGLONG   Oid;                    /* directories: their object */
    ULONGLONG   FileId;                 /* files: (Home, FileId) is their identity */
    ULONGLONG   Home;
    RFS_TIMES   Times;
    ULONG       Attributes;
    ULONGLONG   Size, Alloc;
} RFS_DIRENT, *PRFS_DIRENT;

/* an open file or directory */
typedef struct _RFS_EXTENT {
    ULONGLONG   Vcn;
    ULONGLONG   Lcn;                    /* virtual cluster (3.x) or 16 KB block (1.x); 0: hole */
    ULONG       Run;
    USHORT      Flags;
} RFS_EXTENT, *PRFS_EXTENT;

typedef struct _RFS_FILE {
    UCHAR       Type;
    ULONGLONG   Parent;                 /* the directory holding the name row */
    ULONGLONG   Oid;                    /* directories */
    ULONGLONG   Home, FileId;           /* files: the table holding the record, the record's id */
    RFS_TIMES   Times;
    ULONG       Attributes;
    ULONG       IFlags;
    ULONGLONG   SecurityId;
    ULONGLONG   Size, ValidSize, Alloc;
    BOOLEAN     Inline;                 /* the data is in the record */
    PUCHAR      InlineData;
    ULONG       InlineLen;
    BOOLEAN     Reparse;
    PRFS_EXTENT Ext;
    ULONG       NExt, CapExt;
    /* writing */
    USHORT      Name[RFS_MAX_NAME + 1]; /* the name row (in Parent) */
    ULONG       NameLen;
    UCHAR       KeyFlags;               /* REFS_NAME_EMBEDDED / _INDEX */
    BOOLEAN     Integrity;              /* integrity-stream extents: not written here */
    BOOLEAN     Changed;                /* the record needs saving */
    BOOLEAN     ExtChanged;             /* the extent list changed */
    ULONG       LinkCount;
} RFS_FILE, *PRFS_FILE;

RFS_STATUS  RfsDirNext(PRFS_VOL Vol, ULONGLONG DirOid, PRFS_CURSOR Cur, BOOLEAN First, PRFS_DIRENT Ent);
RFS_STATUS  RfsDirAfter(PRFS_VOL Vol, ULONGLONG DirOid, const USHORT *Name, ULONG NameLen, PRFS_DIRENT Ent);
RFS_STATUS  RfsDirLookup(PRFS_VOL Vol, ULONGLONG DirOid, const USHORT *Name, ULONG NameLen, PRFS_DIRENT Ent);
RFS_STATUS  RfsFileOpen(PRFS_VOL Vol, ULONGLONG DirOid, const RFS_DIRENT *Ent, PRFS_FILE *File);
RFS_STATUS  RfsFileOpenDir(PRFS_VOL Vol, ULONGLONG Oid, PRFS_FILE *File);
VOID        RfsFileClose(PRFS_FILE File);
RFS_STATUS  RfsFileRead(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Offset, ULONG Length, PVOID Buf, PULONG Got);
/* the physical cluster (3.x) / byte block of file cluster Vcn: Run clusters contiguous; *Plcn 0: zeros */
RFS_STATUS  RfsFileMap(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Vcn, PULONGLONG Plcn, PULONG Run);
int         RfsNameCompare(PRFS_VOL Vol, const USHORT *A, ULONG ALen, const USHORT *B, ULONG BLen);

/* ---- writing: trees (rfswrite.c) ------------------------------------------------------------------ */
/* keeps big-frame bodies out of thin dispatchers (NT 3.x kernel stacks are 12K) */
#ifdef __GNUC__
#define RFS_NOINLINE            __attribute__((noinline))
#else
#define RFS_NOINLINE
#endif
#define RFS_MAX_KEY             (4 + 2 * RFS_MAX_NAME + 8)
#define RFS_EMBEDDED_MAX        0x600   /* bytes an embedded table's root may take before it gets child pages */

RFS_STATUS  RfsRowOwn(PRFS_ROW Row);
ULONG       RfsNodeSize(PRFS_TABLE Table, PRFS_NODE Node);
RFS_STATUS  RfsEncodePage(PRFS_TABLE Table, PRFS_NODE Node, const RFS_REF *Where, ULONGLONG Clock, ULONGLONG TreeClock, PUCHAR Page, PRFS_REF Out);
RFS_STATUS  RfsTreeInsert(PRFS_TABLE Table, const UCHAR *Key, ULONG KeyLen, const UCHAR *Val, ULONG ValLen, USHORT Flags, BOOLEAN AllowEqual);
RFS_STATUS  RfsTreeInsertRecord(PRFS_TABLE Table, const UCHAR *Rec, ULONG Len);
RFS_STATUS  RfsTreeUpdateAt(PRFS_CURSOR Cur, const UCHAR *Val, ULONG ValLen, USHORT Flags);    /* closes Cur */
RFS_STATUS  RfsTreeUpdate(PRFS_TABLE Table, const UCHAR *Key, ULONG KeyLen, const UCHAR *Val, ULONG ValLen, USHORT Flags);
RFS_STATUS  RfsTreeModifyAt(PRFS_CURSOR Cur, PUCHAR *Val);     /* the row's value to change in place (same size); Cur stays open */
RFS_STATUS  RfsTreeDeleteAt(PRFS_CURSOR Cur);                  /* closes Cur */
RFS_STATUS  RfsTreeDelete(PRFS_TABLE Table, const UCHAR *Key, ULONG KeyLen);
RFS_STATUS  RfsTreeFreeAll(PRFS_TABLE Table);                  /* frees every page of the table (it is going away) */
VOID        RfsTreeFreeNode(PRFS_TABLE Table, PRFS_NODE Node);  /* the node's page (if any) is freed */
ULONG       RfsEmbeddedSize(PRFS_TABLE Table);
RFS_STATUS  RfsEmbeddedStore(PRFS_TABLE Table, PUCHAR *Val, PULONG Len);
RFS_STATUS  RfsWriteDirtyChildren(PRFS_TABLE Table, PRFS_NODE Node);
RFS_STATUS  RfsNewTable(PRFS_VOL Vol, UCHAR Where, ULONGLONG Oid, ULONG Schema, const UCHAR *Desc, ULONG DescLen, PRFS_TABLE *Table);

/* tables opened for writing (rfstree.c) */
RFS_STATUS  RfsSeekLeaf(PRFS_TABLE Table, const UCHAR *Key, ULONG KeyLen, PRFS_CURSOR Cur);   /* the leaf and slot Key belongs at */
VOID        RfsTableDirty(PRFS_TABLE Table);
RFS_STATUS  RfsGlobal(PRFS_VOL Vol, ULONG Root, PRFS_TABLE *Table);    /* not to be closed */
VOID        RfsTableForget(PRFS_VOL Vol, PRFS_TABLE Table);            /* a deleted object's table leaves the cache */
RFS_STATUS  RfsSeekLast(PRFS_TABLE Table, PRFS_CURSOR Cur);
RFS_STATUS  RfsTreeReplaceAt(PRFS_CURSOR Cur, const UCHAR *Key, ULONG KeyLen, const UCHAR *Val, ULONG ValLen, USHORT Flags);  /* same position */

/* ---- 1.x (rfsv1.c) --------------------------------------------------------------------------------- */
RFS_STATUS  RfsMountV1(PRFS_VOL Vol);
/* allocator root summaries (offsets past REFS_IR_DATA): 1.x and 2.x keep
 * {covered, free} at +0/+8; 3.x moved them to +8/+0x10 */
#define RFS_SUM_COVERED(V)  (((V)->V1 || (V)->V2) ? 0x00 : 0x08)
#define RFS_SUM_FREE(V)     (((V)->V1 || (V)->V2) ? 0x08 : 0x10)
/* the 1.x-style record layer (attribute rows {u64 size, u32 type, name},
 * embedded-table row flag 0x08, schemas 0x110/0x120): 1.x, and 2.x on top of
 * 3.x-style pages, containers and record headers */
/* references inside tables (index rows): 2.x keeps them at 0x30 bytes though its
 * checkpoint references are 0x68 */
#define RFS_TREEREF(V)      ((V)->V2 ? 0x30UL : (V)->RefSize)
#define RFS_RULE_V1ATTR     0x80    /* (internal) 1.x/2.x attribute rows: by type, then name */
#define RFS_V1REC(V)        ((V)->V1 || (V)->V2)
#define RFS_RF_EMB(V)       (RFS_V1REC(V) ? REFS1_RF_EMBEDDED : REFS_RF_EMBEDDED)

RFS_STATUS  RfsFormatV1(PRFS_VOL Vol, const char *Label, ULONG ClusterSize);
RFS_STATUS  RfsCreateV1(PRFS_VOL Vol, ULONGLONG DirOid, const USHORT *Name, ULONG Len, BOOLEAN Dir, ULONG Attr, PRFS_DIRENT Ent);
RFS_STATUS  RfsFileSaveV1(PRFS_VOL Vol, PRFS_FILE File);
RFS_STATUS  RfsRemoveV1(PRFS_VOL Vol, ULONGLONG DirOid, const USHORT *Name, ULONG Len);
RFS_STATUS  RfsFileWriteV1(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Off, ULONG Len, const VOID *Buf);
RFS_STATUS  RfsCommitV1(PRFS_VOL Vol);
ULONGLONG   RfsFreeBlocksV1(PRFS_VOL Vol);
RFS_STATUS  RfsAllocPageV1(PRFS_VOL Vol, PRFS_REF Ref);
RFS_STATUS  RfsFreePageV1(PRFS_VOL Vol, const RFS_REF *Ref);
RFS_STATUS  RfsAllocV1Data(PRFS_VOL Vol, ULONG Blocks, PULONGLONG Blk, PULONG Got);
RFS_STATUS  RfsFreeV1Data(PRFS_VOL Vol, ULONGLONG Blk, ULONG Blocks);
/* driver-layer file-data ops in 16 KiB block units (rfsv1.c) */
RFS_STATUS  RfsFileSetSizeV1(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Size, BOOLEAN KeepAlloc);
RFS_STATUS  RfsFileAllocToV1(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Blocks, ULONGLONG Keep, ULONGLONG KeepCount);
RFS_STATUS  RfsFileTruncToV1(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Blocks);
RFS_STATUS  RfsRenameV1(PRFS_VOL Vol, ULONGLONG FromDir, const USHORT *From, ULONG FromLen, ULONGLONG ToDir, const USHORT *To, ULONG ToLen, BOOLEAN Replace);

/* ---- writing: allocation (rfsalloc.c) -------------------------------------------------------------- */
RFS_STATUS  RfsAllocInit(PRFS_VOL Vol);
VOID        RfsAllocDone(PRFS_VOL Vol);
/* Count clusters in one run (Min..Count; Align: the first must be a multiple), physical; *Got the run length */
RFS_STATUS  RfsAlloc(PRFS_VOL Vol, UCHAR Tier, UCHAR For, ULONG Min, ULONG Count, ULONG Align, PULONGLONG Plcn, PULONG Got);
RFS_STATUS  RfsFree(PRFS_VOL Vol, UCHAR Tier, ULONGLONG Plcn, ULONGLONG Count);
RFS_STATUS  RfsAllocIn(PRFS_VOL Vol, ULONGLONG Lo, ULONGLONG Hi, ULONG Min, ULONG Count, ULONG Align, PULONGLONG Plcn, PULONG Got);
BOOLEAN     RfsClusterFree(PRFS_VOL Vol, ULONGLONG Plcn);
RFS_STATUS  RfsFreeData(PRFS_VOL Vol, ULONGLONG Vlcn, ULONGLONG Count);   /* virtual, refcounts honoured */
RFS_STATUS  RfsAllocPage(PRFS_VOL Vol, UCHAR Tier, BOOLEAN Real, PRFS_REF Ref);   /* a metadata page's clusters */
RFS_STATUS  RfsFreePage(PRFS_VOL Vol, UCHAR Tier, BOOLEAN Real, const RFS_REF *Ref);
RFS_STATUS  RfsToVirtual(PRFS_VOL Vol, ULONGLONG Plcn, PULONGLONG Vlcn);
VOID        RfsQuarantineClear(PRFS_VOL Vol);
ULONGLONG   RfsFreeCount(PRFS_VOL Vol);
UCHAR       RfsTierOf(PRFS_VOL Vol, PRFS_TABLE Table);

/* ---- writing: commit (rfscommit.c) --------------------------------------------------------------- */
RFS_STATUS  RfsCommit(PRFS_VOL Vol);
RFS_STATUS  RfsWritePage(PRFS_VOL Vol, const RFS_REF *Where, BOOLEAN Real, const UCHAR *Page);

/* ---- writing: files and directories (rfsops.c) ---------------------------------------------------- */
RFS_STATUS  RfsCreate(PRFS_VOL Vol, ULONGLONG DirOid, const USHORT *Name, ULONG NameLen, BOOLEAN Dir, ULONG Attributes, PRFS_DIRENT Ent);
RFS_STATUS  RfsRemove(PRFS_VOL Vol, ULONGLONG DirOid, const USHORT *Name, ULONG NameLen);
RFS_STATUS  RfsRename(PRFS_VOL Vol, ULONGLONG FromDir, const USHORT *From, ULONG FromLen, ULONGLONG ToDir, const USHORT *To, ULONG ToLen, BOOLEAN Replace);
RFS_STATUS  RfsFileWrite(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Offset, ULONG Length, const VOID *Buf);
RFS_STATUS  RfsFileSetSize(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Size, BOOLEAN KeepAlloc);
RFS_STATUS  RfsFileSetValid(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Valid);
RFS_STATUS  RfsFileSetInfo(PRFS_VOL Vol, PRFS_FILE File, const RFS_TIMES *Times, ULONG Attributes);
RFS_STATUS  RfsFileSave(PRFS_VOL Vol, PRFS_FILE File);         /* writes the file's record back */
RFS_STATUS  RfsSetLabel(PRFS_VOL Vol, const char *Label);
RFS_STATUS  RfsFileAllocTo(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Clusters, ULONGLONG Keep, ULONGLONG KeepCount);
RFS_STATUS  RfsFileTruncTo(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Clusters);
BOOLEAN     RfsFilePlaced(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Lo, ULONGLONG Hi);
RFS_STATUS  RfsFileRelocate(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Lo, ULONGLONG Hi, BOOLEAN Into, PULONG Moved);
RFS_STATUS  RfsFileAdopt(PRFS_VOL Vol, PRFS_FILE File, ULONGLONG Vlcn, ULONG Run);
RFS_STATUS  RfsDirParent(PRFS_VOL Vol, ULONGLONG Oid, PULONGLONG Parent, USHORT *Name, PULONG Len);

#endif

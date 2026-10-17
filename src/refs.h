/*
 * REFSNT - ReFS on-disk format (versions 1.x and 3.x)
 *
 * ReFS keeps all its metadata in "Minstore" B+ trees. A volume is anchored by
 * the boot sector (sector 0), a superblock and two alternating checkpoints;
 * the checkpoint holds the roots of the global tables (object table, schema
 * table, allocators, ...), and the object table holds the root of every
 * directory and system object. Files live inside their directory's tree.
 *
 *   1.x (Windows 8 / Server 2012, 8.1 / 2012 R2): 16 KB metadata pages
 *       addressed by 16 KB block numbers, a 48-byte page header, 24-byte page
 *       references (CRC64), six checkpoint roots, no containers.
 *   3.x (Windows 10 1709+, 11): pages of one cluster (64 KB clusters) or four
 *       clusters (4 KB clusters), an 80-byte page header with a signature,
 *       page references of four cluster numbers plus a checksum, thirteen
 *       checkpoint roots, and virtual cluster numbers translated through the
 *       container table (64 MB containers).
 *
 * All numbers are little-endian. Offsets are in bytes.
 */
#ifndef _REFS_H_
#define _REFS_H_

/* ---- boot sector (sector 0; a copy in the last sector) ---------------------------------------- */
#define REFS_VBR_FSNAME         0x03    /* "ReFS\0\0\0\0" */
#define REFS_VBR_FSRS           0x10    /* "FSRS" */
#define REFS_VBR_FSRS_SIZE      0x14    /* u16 0x200 */
#define REFS_VBR_CHECKSUM       0x16    /* u16 */
#define REFS_VBR_SECTORS        0x18    /* u64 */
#define REFS_VBR_BPS            0x20    /* u32 bytes a sector */
#define REFS_VBR_SPC            0x24    /* u32 sectors a cluster */
#define REFS_VBR_MAJOR          0x28    /* u8 */
#define REFS_VBR_MINOR          0x29    /* u8 */
#define REFS_VBR_CKSEL          0x2A    /* u16 (3.10+): 0 none, 2 CRC64, 4 SHA-256 */
#define REFS_VBR_FLAGS          0x2C    /* u32 */
#define REFS_VBR_SERIAL         0x38    /* u64 */
#define REFS_VBR_CONTAINER      0x40    /* u64 bytes a container (3.x; 0 on 1.x) */
#define REFS_VBR_GUID           0x48    /* 16 (3.10+) */

/* ---- common page header ---------------------------------------------------------------------- */
/* 3.x: 80 bytes */
#define REFS3_PH_SIGNATURE      0x00    /* "SUPB" "CHKP" "MSB+" */
#define REFS3_PH_VERSION        0x04    /* u32 2 */
#define REFS3_PH_VOLSIG         0x0C    /* u32 xor of the volume GUID's four dwords */
#define REFS3_PH_CLOCK          0x10    /* u64 virtual allocator clock */
#define REFS3_PH_TREECLOCK      0x18    /* u64 tree update clock */
#define REFS3_PH_LCN            0x20    /* 4 x u64 the page's own clusters */
#define REFS3_PH_TABLEHI        0x40    /* u64 */
#define REFS3_PH_TABLE          0x48    /* u64 table (object) id */
#define REFS3_PH_SIZE           0x50
/* 1.x: 48 bytes */
#define REFS1_PH_BLOCK          0x00    /* u64 the page's own block */
#define REFS1_PH_CLOCK          0x08    /* u64 */
#define REFS1_PH_TABLEHI        0x10    /* u64 */
#define REFS1_PH_TABLE          0x18    /* u64 */
#define REFS1_PH_ONE            0x20    /* u64 1 in the superblock and checkpoint */
#define REFS1_PH_SIZE           0x30

/* ---- page reference ----------------------------------------------------------------------------- */
/* 3.x: 4 x u64 clusters, u16 flags, u8 checksum type, u8 checksum offset (from +0x20 ... +0x28 = data),
   u32 checksum length, checksum. 104 bytes (3.4 - 3.9), 48 (CRC64), 72 (SHA-256) */
#define REFS3_REF_LCN           0x00
#define REFS3_REF_FLAGS         0x20
#define REFS3_REF_CKTYPE        0x22
#define REFS3_REF_CKOFF         0x23
#define REFS3_REF_CKLEN         0x24
#define REFS3_REF_CK            0x28
/* 1.x: u64 block, u16 flags, u8 checksum type, u8 checksum offset (from +0x08), u16 length, u16 0, checksum */
#define REFS1_REF_BLOCK         0x00
#define REFS1_REF_FLAGS         0x08
#define REFS1_REF_CKTYPE        0x0A
#define REFS1_REF_CKOFF         0x0B
#define REFS1_REF_CKLEN         0x0C
#define REFS1_REF_CK            0x10
#define REFS1_REF_SIZE          24

#define REFS_CK_NONE            0
#define REFS_CK_CRC32C          1
#define REFS_CK_CRC64           2
#define REFS_CK_SHA256          4

/* ---- superblock (3.x: cluster 30; 1.x: block 30) -------------------------------------------- */
#define REFS3_SB_GUID           0x50
#define REFS3_SB_GENERATION     0x68
#define REFS3_SB_CHKP_OFF       0x70
#define REFS3_SB_CHKP_COUNT     0x74
#define REFS3_SB_SELF_OFF       0x78
#define REFS3_SB_SELF_LEN       0x7C
#define REFS1_SB_GUID           0x30
#define REFS1_SB_GENERATION     0x48
#define REFS1_SB_CHKP_OFF       0x50
#define REFS1_SB_CHKP_COUNT     0x54
#define REFS1_SB_SELF_OFF       0x58
#define REFS1_SB_SELF_LEN       0x5C
#define REFS_SB_LOCATION        30

/* ---- checkpoint ------------------------------------------------------------------------------------ */
#define REFS3_CP_VERSION_ECHO   0x50
#define REFS3_CP_MAJOR          0x54
#define REFS3_CP_MINOR          0x56
#define REFS3_CP_SELF_OFF       0x58
#define REFS3_CP_REFSIZE        0x5C
#define REFS3_CP_CLOCK          0x60
#define REFS3_CP_ALLOCCLOCK     0x68
#define REFS3_CP_LSN            0x70
#define REFS3_CP_FLAGS          0x78
#define REFS3_CP_DATAEND        0x88
#define REFS3_CP_MAXROOTS       0x8C
#define REFS3_CP_NROOTS         0x90
#define REFS3_CP_ROOTS          0x94
#define REFS3_CPF_INDIRECT      0x0200
#define REFS3_CPF_CRC64         0x0400
#define REFS3_NROOTS            13
#define REFS2_NROOTS            10      /* ReFS 2.x: object..schema2 (0-9), no 10-12 */
#define REFS1_CP_MAJOR          0x34
#define REFS1_CP_MINOR          0x36
#define REFS1_CP_SELF_OFF       0x38
#define REFS1_CP_SELF_LEN       0x3C
#define REFS1_CP_CLOCK          0x40
#define REFS1_CP_DATAEND        0x48
#define REFS1_CP_MAXROOTS       0x4C
#define REFS1_CP_NROOTS         0x58
#define REFS1_CP_ROOTS          0x5C
#define REFS1_NROOTS            6

/* 3.x checkpoint roots */
#define REFS_ROOT_OBJECT        0
#define REFS_ROOT_MEDIUM        1
#define REFS_ROOT_CONTALLOC     2
#define REFS_ROOT_SCHEMA        3
#define REFS_ROOT_PARENTCHILD   4
#define REFS_ROOT_OBJECT2       5
#define REFS_ROOT_REFCOUNT      6
#define REFS_ROOT_CONTAINER     7
#define REFS_ROOT_CONTAINER2    8
#define REFS_ROOT_SCHEMA2       9
#define REFS_ROOT_CONTINDEX     10
#define REFS_ROOT_INTEGRITY     11
#define REFS_ROOT_SMALL         12

/* table ids (page header) */
#define REFS_TID_SCHEMA         0x01
#define REFS_TID_OBJECT         0x02
#define REFS_TID_PARENTCHILD    0x03
#define REFS_TID_OBJECT2        0x04
#define REFS_TID_REFCOUNT       0x05
#define REFS_TID_SCHEMA2        0x06
#define REFS_TID_CONTAINER      0x0B
#define REFS_TID_CONTAINER2     0x0C
#define REFS_TID_CONTINDEX      0x0E
#define REFS_TID_INTEGRITY      0x0F
#define REFS_TID_CONTALLOC      0x20
#define REFS_TID_MEDIUM         0x21
#define REFS_TID_SMALL          0x22
/* 1.x allocators */
#define REFS1_TID_SMALL         0x0C
#define REFS1_TID_MEDIUM        0x0D
#define REFS1_TID_LARGE         0x0E

/* 1.x checkpoint roots, placed into the logical slots this engine reuses for 3.x */
#define REFS1_ROOT_LARGE        REFS_ROOT_CONTALLOC     /* 2 */
#define REFS1_ROOT_MEDIUM       REFS_ROOT_MEDIUM        /* 1 */
#define REFS1_ROOT_SMALL        REFS_ROOT_SMALL         /* 12 */

/* 1.x directory entry key type (key+2) and row flag for an embedded table */
#define REFS1_ENT_FILE          1
#define REFS1_ENT_DIR           2
#define REFS1_RF_EMBEDDED       0x0008

/* 1.x file / stream record header-data offsets (value-relative) */
#define REFS1_FH_SECHASH        0x60
#define REFS1_FH_SECID          0x64
#define REFS1_FH_SIZE           0x68
#define REFS1_FH_ALLOC          0x70
#define REFS1_FH_DIROID         0x50    /* directory self record: its own OID */
#define REFS1_FH_NEXTID         0x90    /* directory self record: next file id */

/* 1.x extent row value: block units (16 KiB) */
#define REFS1_EXT_VCN           0x00    /* u64 first file block */
#define REFS1_EXT_COUNT         0x08    /* u64 blocks */
#define REFS1_EXT_LCN           0x10    /* u64 first volume block */
#define REFS1_EXT_FLAGS         0x18    /* u16 */
#define REFS1_EXT_CKTYPE        0x1A    /* u8 */
#define REFS1_EXT_CKOFF         0x1B    /* u8 */
#define REFS1_EXT_CKLEN         0x1C    /* u16 */
#define REFS1_EXT_CK            0x20    /* per-block checksums */

/* well-known objects */
#define REFS_OID_UPCASE         0x07
#define REFS_OID_UPCASE2        0x08
#define REFS_OID_LOGINFO        0x09
#define REFS_OID_LOGINFO2       0x0A
#define REFS_OID_TRASH          0x0D
#define REFS_OID_VOLINFO        0x500
#define REFS_OID_VOLINFO2       0x501
#define REFS_OID_FSMETA         0x520
#define REFS_OID_SECURITY       0x530
#define REFS_OID_REPARSE        0x540
#define REFS_OID_REPARSE2       0x541
#define REFS_OID_ROOT           0x600
#define REFS_OID_FIRST_USER     0x701

/* schemas */
#define REFS_SCHEMA_DIR         0x130   /* directory tables */
#define REFS_SCHEMA_SECURITY    0x140
#define REFS_SCHEMA_VOLINFO     0x150
#define REFS_SCHEMA_REPARSE     0x160
#define REFS_SCHEMA_FILE        0x1E0   /* a file's own table (embedded in its name row) */
#define REFS_SCHEMA_DIRSELF     0x1F0   /* a directory's own (0x10) row */
#define REFS_SCHEMA_STREAM      0x200   /* a stream's extent table */
#define REFS_SCHEMA_ALLOC       0xE010
#define REFS_SCHEMA_OBJECT      0xE030
#define REFS_SCHEMA_PARENTCHILD 0xE040
#define REFS_SCHEMA_SCHEMA      0xE060
#define REFS_SCHEMA_CONTAINER   0xE0C0

/* ---- B+ tree node ------------------------------------------------------------------------------------- */
/* index root descriptor (root pages at header end; embedded tables at the value start) */
#define REFS_IR_HDROFF          0x00    /* u32 offset to the node header */
#define REFS_IR_SIZE            0x04    /* u16 0x28 */
#define REFS_IR_KIND            0x06    /* u16 */
#define REFS_IR_ONE             0x08    /* u32 */
#define REFS_IR_SCHEMA          0x0C    /* u32 */
#define REFS_IR_SCHEMA2         0x10    /* u32 */
#define REFS_IR_TWO             0x14    /* u32 */
#define REFS_IR_LEAVES          0x18    /* u64 leaf pages of the table */
#define REFS_IR_ROWS            0x20    /* u64 rows of the table */
#define REFS_IR_DATA            0x28    /* the table's header data */
/* node header (0x28 bytes) */
#define REFS_NH_DATASTART       0x00
#define REFS_NH_DATAEND         0x04
#define REFS_NH_FREE            0x08
#define REFS_NH_LEVEL           0x0C
#define REFS_NH_FLAGS           0x0D
#define REFS_NH_KEYSTART        0x10
#define REFS_NH_COUNT           0x14
#define REFS_NH_KEYEND          0x20
#define REFS_NH_SIZE            0x28
#define REFS1_NH_KEYEND         0x18    /* 1.x: end of the offset array (3.x: 0x20) */
#define REFS1_NH_SIZE           0x20    /* 1.x node header (3.x: 0x28) */
#define REFS_NF_INDEX           0x01
#define REFS_NF_ROOT            0x02
#define REFS_NF_RANGE           0x04    /* range keys (allocators, extents) */
#define REFS_NF_BARE            0x08    /* leaf rows are bare records (no row header): extents */
/* row header */
#define REFS_RH_SIZE            0x00    /* u32 */
#define REFS_RH_KEYOFF          0x04
#define REFS_RH_KEYLEN          0x06
#define REFS_RH_FLAGS           0x08
#define REFS_RH_VALOFF          0x0A
#define REFS_RH_VALLEN          0x0C
#define REFS_RH_LENGTH          0x10
#define REFS_RF_EMBEDDED        0x0001  /* 3.x: the value is a table */
#define REFS_RF_LASTCHILD       0x0002  /* the index row with no key */
#define REFS1_RF_EMBEDDED       0x0008  /* 1.x */

/* ---- directory tables (schema 0x130) ------------------------------------------------------------- */
#define REFS_KEY_DIRSELF        0x0010
#define REFS_KEY_REVERSE        0x0020  /* u32 0x80000020 */
#define REFS_KEY_NAME           0x0030
#define REFS_KEY_BACKING        0x0040  /* u32 0x80000040: a split file's own table */
#define REFS_NAME_EMBEDDED      0x0001  /* the name row holds the file's table */
#define REFS_NAME_INDEX         0x0002  /* the name row points at a backing record (directories too) */
/* index-entry value (84 bytes; 72 before 3.10) */
#define REFS_IE_FILEID          0x00
#define REFS_IE_HOME            0x08
#define REFS_IE_CTIME           0x10
#define REFS_IE_MTIME           0x18
#define REFS_IE_CHTIME          0x20
#define REFS_IE_ATIME           0x28
#define REFS_IE_ALLOC           0x30
#define REFS_IE_SIZE            0x38
#define REFS_IE_ATTR            0x40
#define REFS_IE_LENGTH          84
#define REFS_IE_LENGTH_OLD      72
/* a file table's header data (value + 0x28) */
#define REFS_FH_CTIME           0x28
#define REFS_FH_MTIME           0x30
#define REFS_FH_CHTIME          0x38
#define REFS_FH_ATIME           0x40
#define REFS_FH_ATTR            0x48
#define REFS_FH_IFLAGS          0x4C
#define REFS_FH_SECID           0x50
#define REFS_FH_SIZE            0x58
#define REFS_FH_ALLOC           0x60
#define REFS_FH_USN             0x68
#define REFS_FH_JOURNAL         0x70
#define REFS_FH_NEXTID          0x80

#define REFS_ATTR_DIRECTORY     0x10000000UL    /* in directory entries */

/* attribute rows of a file table */
#define REFS_AT_MARKER_SINGLE   0x80000001UL
#define REFS_AT_MARKER_MULTI    0x80000002UL
#define REFS_AT_OBJLINK         0x39
#define REFS_AT_DATA            0x80
#define REFS_AT_I30             0x90
#define REFS_AT_NAMED           0xB0
#define REFS_AT_REPARSE         0xC0
#define REFS_AT_EAINFO          0xD0
#define REFS_AT_EA              0xE0
#define REFS_STREAM_META        0x08    /* sub-stream id of the stream-set record */
#define REFS_STREAM_LIVE        0x1000  /* sub-stream id of the live data stream */

/* extent record (24 bytes, + 4 a cluster with integrity checksums) */
#define REFS_EXT_VLCN           0x00
#define REFS_EXT_FLAGS          0x08    /* u16 */
#define REFS_EXT_RECSIZE        0x0A    /* u16 */
#define REFS_EXT_VCN            0x0C    /* u32 */
#define REFS_EXT_RUN            0x14    /* u32 */
#define REFS_EXT_LENGTH         24
#define REFS_EXTF_WRITTEN       0x0010
#define REFS_EXTF_SPARSE        0x0020
#define REFS_EXTF_DATA          0x0040
#define REFS_EXTF_CHECKSUMS     0x0080

/* ---- 1.x ------------------------------------------------------------------------------------------- */
#define REFS1_PAGE              16384
#define REFS1_BLOCK_SHIFT       14

#endif

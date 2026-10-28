/*
 * REFSNT - a ReFS 1.2 formatter (Windows 8 / 8.1 layout family).
 *
 * Builds a fresh ReFS 1.2 volume: boot sector, superblock (+ two backups),
 * two checkpoints, the three allocators (large / medium / small), the object
 * table, schema, parent-child, volume information (with the upcase and the
 * 0x540 streams), the security table and an empty root directory, plus the
 * File System Metadata directory (OID 0x520). The on-disk structures follow
 * doc/REFS1.md; the schema rules, the $I30 index-root stub, the default
 * security descriptors and the volume-information record are format constants
 * captured in fmt/rfs1const.h.
 *
 * Metadata pages are 16 KiB blocks; one 64 KiB cluster (4 blocks) is used per
 * page (its first block), so the medium allocator (1 bit = 1 cluster) is the
 * single free-space map. Per-table private allocators (a Windows space
 * optimisation) are not written - every metadata page is a whole cluster
 * marked in the medium allocator - so free space stays consistent. This is a
 * valid ReFS 1.2 volume that REFSNT reads and writes; Windows read-compat has
 * not been verified in this environment (see doc/REFS1.md).
 */
#include "rfseng.h"
#include "rfs1const.h"

#define BLK             REFS1_PAGE
#define BLKSH           REFS1_BLOCK_SHIFT
#define SPB(V)          ((V)->ClusterSize / 512)       /* sectors per cluster (VBR) */

/* allocator row offsets (value-relative; key overlaps the first 16 bytes) */
#define AR1_START   0x00
#define AR1_LENGTH  0x08
#define AR1_VALSIZE 0x10
#define AR1_BITS    0x14
#define AR1_SHIFT   0x18
#define AR1_FREE    0x1C
#define AR1_LRS     0x20
#define AR1_LRL     0x24
#define AR1_BMOFF   0x30
#define AR1_BMLEN   0x34
#define AR1_BITMAP  0x38

/* a builder for one fresh volume */
typedef struct {
    PRFS_VOL    V;
    ULONGLONG   Blocks;             /* volume size in 16 KiB blocks */
    ULONGLONG   Next;               /* next free metadata block (cluster-aligned) */
    PUCHAR      Page;               /* one scratch page */
    ULONGLONG   NextOid;
    /* used clusters, for the allocators (small system set + the backup tail) */
    ULONGLONG   UsedLo, UsedHi;     /* clusters [UsedLo, UsedHi) all used at the start */
    ULONGLONG   TailCluster;        /* the backup superblocks' cluster (used) */
} FMT;

static ULONG
Bps16(const UCHAR *v, ULONG size)
{
    ULONG cs = 0, i;
    for (i = 3; i < size; i++) { if (i == 0x16 || i == 0x17) continue; cs = (((cs & 1) ? 0x8000 : 0) + (cs >> 1) + v[i]) & 0xFFFF; }
    return cs;
}

/* a metadata block (one cluster, first block) */
static ULONGLONG
GetBlock(FMT *F)
{
    ULONGLONG b = F->Next;
    F->Next += F->V->ClusterSize / BLK;             /* one whole cluster */
    return b;
}

/* writes one 16 KiB page */
static RFS_STATUS
PutBlk(FMT *F, ULONGLONG Blk, const UCHAR *Data)
{
    return RfsHostWrite(F->V, Blk << BLKSH, BLK, Data);
}

/*
 * Encodes table T's root node at block Blk and writes it; Out gets the
 * reference (block + CRC64 of the whole page).
 */
static RFS_STATUS
EmitTable(FMT *F, PRFS_TABLE T, ULONGLONG Blk, PRFS_REF Out)
{
    RFS_REF where;
    RFS_STATUS st;
    RfsZero(&where, sizeof(where));
    where.Lcn[0] = Blk;
    st = RfsEncodePage(T, T->Root, &where, 0, 0, F->Page, Out);
    if (!RFS_SUCCESS(st)) return st;
    return PutBlk(F, Blk, F->Page);
}

/* builds one object-table row value: {page ref, 0, 8, 8, next file id} */
static VOID
ObjValue(PRFS_VOL V, const RFS_REF *Root, ULONGLONG NextId, PUCHAR Val)
{
    RfsZero(Val, 48);
    RfsRefPut(V, Val, Root);
    RfsPut32(Val + 0x20, 8);
    RfsPut32(Val + 0x24, 8);
    RfsPut64(Val + 0x28, NextId);
}

/* a fresh index-root descriptor + header-data area of HdrLen zero bytes */
static ULONG
MakeDesc(PUCHAR Desc, USHORT Kind, ULONG HdrLen)
{
    ULONG len = REFS_IR_DATA + HdrLen;
    RfsZero(Desc, len);
    RfsPut16(Desc + REFS_IR_SIZE, 0x28);
    RfsPut16(Desc + REFS_IR_KIND, Kind);
    RfsPut32(Desc + REFS_IR_ONE, 1);
    RfsPut32(Desc + REFS_IR_TWO, 2);
    return len;
}

/* ---- allocators ------------------------------------------------------------------------------- */
/* builds one allocator bitmap row (key = first 16 bytes of the value) */
static ULONG
AllocRow(PUCHAR V, ULONGLONG Start, ULONGLONG Len, ULONG Shift, const UCHAR *Bitmap, ULONG Bits)
{
    ULONG bytes = (Bits + 7) / 8, i, free = 0, lrs = 0, lrl = 0, cur = 0, vsz;
    vsz = AR1_BITMAP + bytes;
    RfsZero(V, vsz);
    RfsPut64(V + AR1_START, Start);
    RfsPut64(V + AR1_LENGTH, Len);
    RfsPut32(V + AR1_VALSIZE, vsz);
    RfsPut32(V + AR1_BITS, Bits);
    RfsPut32(V + AR1_SHIFT, Shift);
    for (i = 0; i < Bits; i++) {
        if (!((Bitmap[i >> 3] >> (i & 7)) & 1)) { free++; cur++; if (cur > lrl) { lrl = cur; lrs = i - cur + 1; } }
        else cur = 0;
    }
    RfsPut32(V + AR1_FREE, free);
    RfsPut32(V + AR1_LRS, lrs);
    RfsPut32(V + AR1_LRL, lrl);
    RfsPut32(V + AR1_BMOFF, AR1_BITMAP);
    RfsPut32(V + AR1_BMLEN, bytes);
    RfsCopy(V + AR1_BITMAP, Bitmap, bytes);
    return vsz;
}

/* the medium allocator: a row per 64 MiB chunk that has any use */
static RFS_STATUS
BuildMedium(FMT *F, ULONGLONG Blk, PRFS_REF Out)
{
    PRFS_VOL V = F->V;
    PRFS_TABLE T;
    UCHAR desc[REFS_IR_DATA + 16];
    ULONG dl = MakeDesc(desc, 0xE010, 16);
    ULONG cpc = 0x400;                               /* clusters a 64 MiB chunk */
    ULONGLONG chunkblocks = 0x1000;
    ULONGLONG totalcl = F->Blocks / (V->ClusterSize / BLK);
    ULONGLONG nchunks = (F->Blocks + chunkblocks - 1) / chunkblocks;
    ULONGLONG covered = 0, freeblk = 0, ch;
    RFS_STATUS st;
    PUCHAR bm = (PUCHAR)RfsHostAlloc(cpc / 8);
    PUCHAR val = (PUCHAR)RfsHostAlloc(AR1_BITMAP + cpc / 8);
    UCHAR key[16];
    if (!bm || !val) { RfsHostFree(bm); RfsHostFree(val); return RFS_NO_MEMORY; }
    st = RfsNewTable(V, RFS_TROOT_CHECKPOINT, 0, 0xE010, desc, dl, &T);
    if (!RFS_SUCCESS(st)) { RfsHostFree(bm); RfsHostFree(val); return st; }
    T->Index = REFS1_ROOT_MEDIUM;
    RfsPut64(T->Root->PageHdr + REFS1_PH_TABLE, REFS1_TID_MEDIUM);
    for (ch = 0; ch < nchunks; ch++) {
        ULONGLONG cstart = ch * cpc;                 /* first cluster of the chunk */
        ULONGLONG bstart = ch * chunkblocks;         /* first block */
        ULONG used = 0, i, vsz;
        RfsZero(bm, cpc / 8);
        for (i = 0; i < cpc; i++) {
            ULONGLONG cl = cstart + i;
            if (cl >= totalcl) { bm[i >> 3] |= (UCHAR)(1 << (i & 7)); used++; continue; }   /* past the end */
            if ((cl >= F->UsedLo && cl < F->UsedHi) || cl == F->TailCluster) { bm[i >> 3] |= (UCHAR)(1 << (i & 7)); used++; }
        }
        if (used == 0) continue;                     /* chunk entirely free: no row (large allocator owns it) */
        vsz = AllocRow(val, bstart, chunkblocks, 2, bm, cpc);
        RfsCopy(key, val, 16);
        st = RfsTreeInsert(T, key, 16, val, vsz, 0, FALSE);
        if (!RFS_SUCCESS(st)) break;
        covered += chunkblocks;
        freeblk += (ULONGLONG)(cpc - used) * (V->ClusterSize / BLK);
    }
    if (RFS_SUCCESS(st)) {
        /* blocks covered by the rows and free in them (as Windows and the engine count) */
        RfsPut64(T->Root->Desc + REFS_IR_DATA + 0, covered);
        RfsPut64(T->Root->Desc + REFS_IR_DATA + 8, freeblk);
        st = EmitTable(F, T, Blk, Out);
    }
    RfsNodeFree(T->Root); RfsHostFree(T);
    RfsHostFree(bm); RfsHostFree(val);
    return st;
}

/* the large allocator: one row, 1 bit = 64 MiB chunk; a chunk with any use is set */
static RFS_STATUS
BuildLarge(FMT *F, ULONGLONG Blk, PRFS_REF Out)
{
    PRFS_VOL V = F->V;
    PRFS_TABLE T;
    UCHAR desc[REFS_IR_DATA + 16];
    ULONG dl = MakeDesc(desc, 0xE010, 16);
    ULONGLONG chunkblocks = 0x1000;
    ULONGLONG nchunks = (F->Blocks + chunkblocks - 1) / chunkblocks;
    ULONG bits = (ULONG)nchunks, bytes = (bits + 7) / 8, vsz;
    RFS_STATUS st;
    PUCHAR bm = (PUCHAR)RfsHostAlloc(bytes ? bytes : 1);
    PUCHAR val = (PUCHAR)RfsHostAlloc(AR1_BITMAP + bytes + 8);
    UCHAR key[16];
    ULONGLONG ch, usedch = 0;
    if (!bm || !val) { RfsHostFree(bm); RfsHostFree(val); return RFS_NO_MEMORY; }
    RfsZero(bm, bytes);
    for (ch = 0; ch < nchunks; ch++) {
        ULONGLONG c0 = ch * chunkblocks / (V->ClusterSize / BLK);
        ULONGLONG c1 = (ch + 1) * chunkblocks / (V->ClusterSize / BLK);
        BOOLEAN use = (BOOLEAN)((F->UsedLo < c1 && F->UsedHi > c0) || (F->TailCluster >= c0 && F->TailCluster < c1) ||
                                (ch + 1) * chunkblocks > F->Blocks);
        if (use) { bm[ch >> 3] |= (UCHAR)(1 << (ch & 7)); usedch++; }
    }
    st = RfsNewTable(V, RFS_TROOT_CHECKPOINT, 0, 0xE010, desc, dl, &T);
    if (!RFS_SUCCESS(st)) { RfsHostFree(bm); RfsHostFree(val); return st; }
    T->Index = REFS1_ROOT_LARGE;
    RfsPut64(T->Root->PageHdr + REFS1_PH_TABLE, REFS1_TID_LARGE);
    vsz = AllocRow(val, 0, F->Blocks, 12, bm, bits);
    RfsCopy(key, val, 16);
    st = RfsTreeInsert(T, key, 16, val, vsz, 0, FALSE);
    if (RFS_SUCCESS(st)) {
        /* blocks: the volume, and those of the chunks not given out */
        RfsPut64(T->Root->Desc + REFS_IR_DATA + 0, F->Blocks);
        RfsPut64(T->Root->Desc + REFS_IR_DATA + 8, (nchunks - usedch) * chunkblocks);
        st = EmitTable(F, T, Blk, Out);
    }
    RfsNodeFree(T->Root); RfsHostFree(T);
    RfsHostFree(bm); RfsHostFree(val);
    return st;
}

/* the small allocator: empty (block-granular sub-allocation is not used) */
static RFS_STATUS
BuildSmall(FMT *F, ULONGLONG Blk, PRFS_REF Out)
{
    PRFS_VOL V = F->V;
    PRFS_TABLE T;
    UCHAR desc[REFS_IR_DATA + 16];
    ULONG dl = MakeDesc(desc, 0xE010, 16);
    RFS_STATUS st = RfsNewTable(V, RFS_TROOT_CHECKPOINT, 0, 0xE010, desc, dl, &T);
    if (!RFS_SUCCESS(st)) return st;
    T->Index = REFS1_ROOT_SMALL;
    RfsPut64(T->Root->PageHdr + REFS1_PH_TABLE, REFS1_TID_SMALL);
    st = EmitTable(F, T, Blk, Out);
    RfsNodeFree(T->Root); RfsHostFree(T);
    return st;
}

/* ---- embedded records ---------------------------------------------------------------------------- */
/*
 * The directory self record (the 0x10 row value): an embedded table, schema
 * 0x110, whose header data holds the directory's times, attributes, OID,
 * security and next file id, and whose single row is the $I30 index stub.
 */
static RFS_STATUS
DirSelfValue(FMT *F, ULONGLONG Oid, ULONG SecHash, ULONGLONG NextId, PUCHAR *Val, PULONG Len)
{
    PRFS_VOL V = F->V;
    PRFS_TABLE T;
    UCHAR desc[REFS_IR_DATA + 0x80];
    ULONG dl = MakeDesc(desc, 1, 0x80);
    PUCHAR h = desc + REFS_IR_DATA;
    ULONGLONG now = RfsHostNowNt();
    RFS_STATUS st;
    RfsPut64(h + 0x00, now); RfsPut64(h + 0x08, now); RfsPut64(h + 0x10, now); RfsPut64(h + 0x18, now);
    RfsPut32(h + 0x20, 0x10);                        /* attributes: directory */
    RfsPut64(h + 0x28, Oid);                         /* value 0x50: the directory OID */
    RfsPut32(h + 0x38, SecHash);                     /* value 0x60 */
    RfsPut32(h + 0x3C, 1);                           /* value 0x64: security id */
    RfsPut64(h + 0x68, NextId);                      /* value 0x90: next file id */
    RfsPut64(h + 0x70, 1);                           /* value 0x98 */
    st = RfsNewTable(V, RFS_TROOT_EMBEDDED, Oid, 1, desc, dl, &T);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsTreeInsert(T, Rfs1I30Key, sizeof(Rfs1I30Key), Rfs1I30Val, sizeof(Rfs1I30Val), 0, FALSE);
    if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(T, Val, Len);
    RfsNodeFree(T->Root); RfsHostFree(T);
    return st;
}

/* builds a directory table (self record only: an empty directory) at block Blk */
static RFS_STATUS
BuildDir(FMT *F, ULONGLONG Oid, ULONG SecHash, ULONGLONG NextId, ULONGLONG Blk, PRFS_REF Out)
{
    PRFS_VOL V = F->V;
    PRFS_TABLE T;
    UCHAR desc[REFS_IR_DATA + 0x80];
    ULONG dl = MakeDesc(desc, 1, 0x80);
    UCHAR key[4];
    PUCHAR sv;
    ULONG sl;
    RFS_STATUS st = DirSelfValue(F, Oid, SecHash, NextId, &sv, &sl);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsNewTable(V, RFS_TROOT_OBJECT, Oid, REFS_SCHEMA_DIR, desc, dl, &T);
    if (RFS_SUCCESS(st)) {
        RfsPut64(T->Root->PageHdr + REFS1_PH_TABLE, Oid);
        RfsPut32(key, REFS_KEY_DIRSELF);
        st = RfsTreeInsert(T, key, 4, sv, sl, REFS1_RF_EMBEDDED, FALSE);
        if (RFS_SUCCESS(st)) st = EmitTable(F, T, Blk, Out);
        RfsNodeFree(T->Root); RfsHostFree(T);
    }
    RfsHostFree(sv);
    return st;
}

/* ---- streams in the volume information table ----------------------------------------------------- */
/*
 * Writes Bytes of Data to consecutive blocks starting at the next free
 * cluster, and returns the stream-table value (schema 0x120) with one extent
 * carrying a CRC64 for every block.
 */
static RFS_STATUS
StreamValue(FMT *F, const UCHAR *Data, ULONG Bytes, PUCHAR *Val, PULONG Len)
{
    PRFS_VOL V = F->V;
    PRFS_TABLE T;
    UCHAR desc[REFS_IR_DATA + 0x40];
    ULONG dl, nblk = (Bytes + BLK - 1) / BLK, i, elen;
    ULONGLONG start = F->Next;
    PUCHAR ev, h;
    RFS_STATUS st = RFS_OK;
    PUCHAR z = (PUCHAR)RfsHostAlloc(BLK);
    if (!z) return RFS_NO_MEMORY;
    for (i = 0; i < nblk && RFS_SUCCESS(st); i++) {
        RfsZero(z, BLK);
        if (i * BLK < Bytes) RfsCopy(z, Data + i * BLK, (Bytes - i * BLK < BLK) ? Bytes - i * BLK : BLK);
        st = PutBlk(F, start + i, z);
    }
    RfsHostFree(z);
    if (!RFS_SUCCESS(st)) return st;
    F->Next += ((ULONGLONG)nblk * BLK + V->ClusterSize - 1) / V->ClusterSize * (V->ClusterSize / BLK);
    F->UsedHi = F->Next / (V->ClusterSize / BLK);
    /* the stream table: header data {u32 1, alloc, size, valid} and one extent row */
    dl = MakeDesc(desc, 1, 0x40);
    RfsPut16(desc + REFS_IR_SIZE, 0x28);
    h = desc + REFS_IR_DATA;
    RfsPut32(h + 0x00, 1);
    RfsPut64(h + 0x0C, (ULONGLONG)nblk * BLK);       /* alloc (value 0x34) */
    RfsPut64(h + 0x14, (ULONGLONG)nblk * BLK);       /* size  (value 0x3C) */
    RfsPut64(h + 0x1C, (ULONGLONG)nblk * BLK);       /* valid (value 0x44) */
    st = RfsNewTable(V, RFS_TROOT_EMBEDDED, 0, 0x120, desc, dl, &T);
    if (!RFS_SUCCESS(st)) return st;
    elen = REFS1_EXT_CK + 8 * nblk;
    ev = (PUCHAR)RfsHostAlloc(elen);
    if (!ev) { RfsNodeFree(T->Root); RfsHostFree(T); return RFS_NO_MEMORY; }
    RfsZero(ev, elen);
    RfsPut64(ev + REFS1_EXT_VCN, 0);
    RfsPut64(ev + REFS1_EXT_COUNT, nblk);
    RfsPut64(ev + REFS1_EXT_LCN, start);
    RfsPut16(ev + REFS1_EXT_FLAGS, 1);               /* integrity: per-block checksums follow */
    ev[REFS1_EXT_CKTYPE] = REFS_CK_CRC64;
    ev[REFS1_EXT_CKOFF] = 8;
    RfsPut16(ev + REFS1_EXT_CKLEN, 8 * nblk);
    {
        PUCHAR blk = (PUCHAR)RfsHostAlloc(BLK);
        if (!blk) { RfsHostFree(ev); RfsNodeFree(T->Root); RfsHostFree(T); return RFS_NO_MEMORY; }
        for (i = 0; i < nblk; i++) {
            UCHAR ck[8];
            RfsZero(blk, BLK);
            if (i * BLK < Bytes) RfsCopy(blk, Data + i * BLK, (Bytes - i * BLK < BLK) ? Bytes - i * BLK : BLK);
            RfsChecksum(REFS_CK_CRC64, blk, BLK, ck);
            RfsCopy(ev + REFS1_EXT_CK + 8 * i, ck, 8);
        }
        RfsHostFree(blk);
    }
    {
        UCHAR key[16];
        RfsCopy(key, ev, 16);
        st = RfsTreeInsert(T, key, 16, ev, elen, 0, FALSE);
    }
    if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(T, Val, Len);
    RfsHostFree(ev);
    RfsNodeFree(T->Root); RfsHostFree(T);
    return st;
}

/* the volume information table (OID 0x500): label, the 0x520 record, upcase, the 0x540 stream */
static RFS_STATUS
BuildVolInfo(FMT *F, const char *Label, const UCHAR *Upcase, const UCHAR *S540, ULONGLONG Blk, PRFS_REF Out)
{
    PRFS_VOL V = F->V;
    PRFS_TABLE T;
    UCHAR desc[REFS_IR_DATA + 8];
    ULONG dl = MakeDesc(desc, 1, 0);
    UCHAR key[8];
    USHORT lab[32];
    ULONG ln = 0, i;
    PUCHAR uv = NULL, sv = NULL;
    ULONG ul = 0, sl = 0;
    RFS_STATUS st;
    dl = REFS_IR_DATA;
    RfsZero(desc, dl);
    RfsPut16(desc + REFS_IR_SIZE, 0x28);
    RfsPut16(desc + REFS_IR_KIND, 1);
    RfsPut32(desc + REFS_IR_ONE, 1);
    RfsPut32(desc + REFS_IR_TWO, 2);
    st = RfsNewTable(V, RFS_TROOT_OBJECT, REFS_OID_VOLINFO, 0x150, desc, dl, &T);
    if (!RFS_SUCCESS(st)) return st;
    RfsPut64(T->Root->PageHdr + REFS1_PH_TABLE, REFS_OID_VOLINFO);
    /* label (0x510) */
    while (Label[ln] && ln < 31) { lab[ln] = (UCHAR)Label[ln]; ln++; }
    RfsZero(key, 8); RfsPut16(key, 0x510);
    { UCHAR lv[64]; for (i = 0; i < ln; i++) RfsPut16(lv + 2 * i, lab[i]);
      st = RfsTreeInsert(T, key, 8, lv, ln * 2, 0, FALSE); }
    /* volume record (0x520) */
    if (RFS_SUCCESS(st)) { RfsZero(key, 8); RfsPut16(key, 0x520); st = RfsTreeInsert(T, key, 8, Rfs1Vi520, sizeof(Rfs1Vi520), 0, FALSE); }
    /* upcase (0x530) */
    if (RFS_SUCCESS(st)) st = StreamValue(F, Upcase, 65536 * 2, &uv, &ul);
    if (RFS_SUCCESS(st)) { RfsZero(key, 8); RfsPut16(key, 0x530); st = RfsTreeInsert(T, key, 8, uv, ul, REFS1_RF_EMBEDDED, FALSE); }
    /* the 0x540 stream */
    if (RFS_SUCCESS(st)) st = StreamValue(F, S540, 65536, &sv, &sl);
    if (RFS_SUCCESS(st)) { RfsZero(key, 8); RfsPut16(key, 0x540); st = RfsTreeInsert(T, key, 8, sv, sl, REFS1_RF_EMBEDDED, FALSE); }
    if (RFS_SUCCESS(st)) st = EmitTable(F, T, Blk, Out);
    RfsNodeFree(T->Root); RfsHostFree(T);
    RfsHostFree(uv); RfsHostFree(sv);
    return st;
}

/* the security table (OID 0x530): the default descriptors */
static RFS_STATUS
BuildSecurity(FMT *F, ULONGLONG Blk, PRFS_REF Out)
{
    PRFS_VOL V = F->V;
    PRFS_TABLE T;
    UCHAR desc[REFS_IR_DATA + 8];
    ULONG dl = REFS_IR_DATA, i;
    RFS_STATUS st;
    RfsZero(desc, dl);
    RfsPut16(desc + REFS_IR_SIZE, 0x28);
    RfsPut16(desc + REFS_IR_KIND, 1);
    RfsPut32(desc + REFS_IR_ONE, 1);
    RfsPut32(desc + REFS_IR_TWO, 2);
    st = RfsNewTable(V, RFS_TROOT_OBJECT, REFS_OID_SECURITY, 0x140, desc, dl, &T);
    if (!RFS_SUCCESS(st)) return st;
    RfsPut64(T->Root->PageHdr + REFS1_PH_TABLE, REFS_OID_SECURITY);
    for (i = 0; i < RFS1_NSEC && RFS_SUCCESS(st); i++)
        st = RfsTreeInsert(T, Rfs1Security[i].K, Rfs1Security[i].KLen, Rfs1Security[i].V, Rfs1Security[i].VLen, 0, FALSE);
    if (RFS_SUCCESS(st)) st = EmitTable(F, T, Blk, Out);
    RfsNodeFree(T->Root); RfsHostFree(T);
    return st;
}

/* the schema table (format constants) */
static RFS_STATUS
BuildSchema(FMT *F, ULONGLONG Blk, PRFS_REF Out)
{
    PRFS_VOL V = F->V;
    PRFS_TABLE T;
    UCHAR desc[REFS_IR_DATA + 8];
    ULONG dl = REFS_IR_DATA, i;
    RFS_STATUS st;
    RfsZero(desc, dl);
    RfsPut16(desc + REFS_IR_SIZE, 0x28);
    RfsPut16(desc + REFS_IR_KIND, 0);
    RfsPut32(desc + REFS_IR_ONE, 1);
    RfsPut32(desc + REFS_IR_TWO, 2);
    st = RfsNewTable(V, RFS_TROOT_CHECKPOINT, 0, 0xE060, desc, dl, &T);
    if (!RFS_SUCCESS(st)) return st;
    T->Index = REFS_ROOT_SCHEMA;
    RfsPut64(T->Root->PageHdr + REFS1_PH_TABLE, REFS_TID_SCHEMA);
    for (i = 0; i < RFS1_NSCHEMAS && RFS_SUCCESS(st); i++) {
        UCHAR key[4];
        RfsPut32(key, Rfs1Schemas[i].Id);
        st = RfsTreeInsert(T, key, 4, Rfs1Schemas[i].Val, Rfs1Schemas[i].Len, 0, FALSE);
    }
    if (RFS_SUCCESS(st)) st = EmitTable(F, T, Blk, Out);
    RfsNodeFree(T->Root); RfsHostFree(T);
    return st;
}

/* the parent-child table (OID 0x600 is the root; no children in a fresh volume) */
static RFS_STATUS
BuildParentChild(FMT *F, ULONGLONG Blk, PRFS_REF Out)
{
    PRFS_VOL V = F->V;
    PRFS_TABLE T;
    UCHAR desc[REFS_IR_DATA + 8];
    ULONG dl = REFS_IR_DATA;
    RFS_STATUS st;
    RfsZero(desc, dl);
    RfsPut16(desc + REFS_IR_SIZE, 0x28);
    RfsPut16(desc + REFS_IR_KIND, 0);
    RfsPut32(desc + REFS_IR_ONE, 1);
    RfsPut32(desc + REFS_IR_TWO, 2);
    st = RfsNewTable(V, RFS_TROOT_CHECKPOINT, 0, 0xE040, desc, dl, &T);
    if (!RFS_SUCCESS(st)) return st;
    T->Index = REFS_ROOT_PARENTCHILD;
    RfsPut64(T->Root->PageHdr + REFS1_PH_TABLE, REFS_TID_PARENTCHILD);
    st = EmitTable(F, T, Blk, Out);
    RfsNodeFree(T->Root); RfsHostFree(T);
    return st;
}

/* ---- the object table, checkpoint, superblock, boot sector ------------------------------------ */
typedef struct { ULONGLONG Oid; ULONGLONG NextId; RFS_REF Ref; } OBJENT;

static RFS_STATUS
BuildObject(FMT *F, const OBJENT *Ents, ULONG N, ULONGLONG Blk, PRFS_REF Out)
{
    PRFS_VOL V = F->V;
    PRFS_TABLE T;
    UCHAR desc[REFS_IR_DATA + 0xC8];
    ULONG dl = MakeDesc(desc, 0, 0xC8), i;
    RFS_STATUS st = RfsNewTable(V, RFS_TROOT_CHECKPOINT, 0, 0xE030, desc, dl, &T);
    if (!RFS_SUCCESS(st)) return st;
    T->Index = REFS_ROOT_OBJECT;
    RfsPut64(T->Root->PageHdr + REFS1_PH_TABLE, REFS_TID_OBJECT);
    for (i = 0; i < N && RFS_SUCCESS(st); i++) {
        UCHAR key[16], val[48];
        RfsZero(key, 16);
        RfsPut64(key + 8, Ents[i].Oid);
        ObjValue(V, &Ents[i].Ref, Ents[i].NextId, val);
        st = RfsTreeInsert(T, key, 16, val, 48, 0, FALSE);
    }
    if (RFS_SUCCESS(st)) st = EmitTable(F, T, Blk, Out);
    RfsNodeFree(T->Root); RfsHostFree(T);
    return st;
}

/* writes a checkpoint block with the 6 roots; CkBlk is the block it lives at */
static RFS_STATUS
WriteCheckpoint(FMT *F, ULONGLONG CkBlk, ULONGLONG Seq, const RFS_REF Roots[REFS3_NROOTS])
{
    PRFS_VOL V = F->V;
    PUCHAR p = F->Page;
    static const ULONG order[REFS1_NROOTS] = {
        REFS_ROOT_OBJECT, REFS1_ROOT_LARGE, REFS1_ROOT_MEDIUM, REFS1_ROOT_SMALL, REFS_ROOT_SCHEMA, REFS_ROOT_PARENTCHILD
    };
    ULONG i, ro = REFS1_CP_ROOTS, base = 0x98;
    UCHAR ck[8];
    RfsZero(p, BLK);
    RfsPut64(p + REFS1_PH_BLOCK, CkBlk);
    RfsPut64(p + REFS1_PH_CLOCK, Seq);
    RfsPut64(p + REFS1_PH_ONE, 1);
    RfsPut16(p + REFS1_CP_MAJOR, 1);
    RfsPut16(p + REFS1_CP_MINOR, 2);
    RfsPut32(p + REFS1_CP_SELF_OFF, 0x80);
    RfsPut32(p + REFS1_CP_SELF_LEN, REFS1_REF_SIZE);
    RfsPut64(p + REFS1_CP_CLOCK, Seq);
    RfsPut32(p + REFS1_CP_MAXROOTS, 0x20);
    RfsPut32(p + REFS1_CP_NROOTS, REFS1_NROOTS);
    for (i = 0; i < REFS1_NROOTS; i++) {
        ULONG off = base + i * REFS1_REF_SIZE;
        RfsPut32(p + ro + 4 * i, off);
        RfsRefPut(V, p + off, &Roots[order[i]]);
    }
    RfsPut32(p + REFS1_CP_DATAEND, base + REFS1_NROOTS * REFS1_REF_SIZE);
    /* self reference (CRC64 over the whole block, its own reference zeroed) */
    {
        RFS_REF self;
        RfsZero(&self, sizeof(self));
        self.Lcn[0] = CkBlk;
        self.CkType = REFS_CK_CRC64;
        RfsRefPut(V, p + 0x80, &self);
        RfsZero(p + 0x80, REFS1_REF_SIZE);
        RfsChecksum(REFS_CK_CRC64, p, BLK, ck);
        self.CkType = REFS_CK_CRC64; self.CkLen = 8;
        RfsCopy(self.Ck, ck, 8);
        RfsRefPut(V, p + 0x80, &self);
    }
    return PutBlk(F, CkBlk, p);
}

/* the superblock (and its two tail copies) referencing the two checkpoints */
static RFS_STATUS
WriteSuper(FMT *F, ULONGLONG Ck0, ULONGLONG Ck1)
{
    PRFS_VOL V = F->V;
    PUCHAR p = F->Page;
    ULONGLONG where[3];
    ULONG k;
    RFS_STATUS st = RFS_OK;
    where[0] = REFS_SB_LOCATION; where[1] = F->Blocks - 2; where[2] = F->Blocks - 3;
    for (k = 0; k < 3 && RFS_SUCCESS(st); k++) {
        RFS_REF self; UCHAR ck[8];
        RfsZero(p, BLK);
        RfsPut64(p + REFS1_PH_BLOCK, where[k]);
        RfsPut64(p + REFS1_PH_ONE, 1);
        RfsCopy(p + REFS1_SB_GUID, V->Guid, 16);
        RfsPut64(p + REFS1_SB_GENERATION, 1);
        RfsPut32(p + REFS1_SB_CHKP_OFF, 0xA0);
        RfsPut32(p + REFS1_SB_CHKP_COUNT, 2);
        RfsPut32(p + REFS1_SB_SELF_OFF, 0xB0);
        RfsPut32(p + REFS1_SB_SELF_LEN, REFS1_REF_SIZE);
        RfsPut64(p + 0xA0, Ck0);
        RfsPut64(p + 0xA8, Ck1);
        RfsZero(&self, sizeof(self));
        self.Lcn[0] = where[k];
        self.CkType = REFS_CK_CRC64;
        RfsRefPut(V, p + 0xB0, &self);
        RfsZero(p + 0xB0, REFS1_REF_SIZE);
        RfsChecksum(REFS_CK_CRC64, p, BLK, ck);
        self.CkLen = 8; RfsCopy(self.Ck, ck, 8);
        RfsRefPut(V, p + 0xB0, &self);
        st = PutBlk(F, where[k], p);
    }
    return st;
}

static RFS_STATUS
WriteVbr(FMT *F)
{
    PRFS_VOL V = F->V;
    UCHAR s[512];
    ULONGLONG sectors = (F->Blocks << BLKSH) / 512;      /* excludes nothing: the count includes the backup */
    RfsZero(s, 512);
    RfsCopy(s + REFS_VBR_FSNAME, "ReFS\0\0\0\0", 8);
    RfsCopy(s + REFS_VBR_FSRS, "FSRS", 4);
    RfsPut16(s + REFS_VBR_FSRS_SIZE, 0x200);
    RfsPut64(s + REFS_VBR_SECTORS, sectors);
    RfsPut32(s + REFS_VBR_BPS, 512);
    RfsPut32(s + REFS_VBR_SPC, V->ClusterSize / 512);
    s[REFS_VBR_MAJOR] = 1; s[REFS_VBR_MINOR] = 2;
    RfsPut32(s + REFS_VBR_FLAGS, 0x0A);
    RfsPut64(s + REFS_VBR_SERIAL, V->Serial);
    RfsPut16(s + REFS_VBR_CHECKSUM, (USHORT)Bps16(s, 0x200));
    if (!RFS_SUCCESS(RfsHostWrite(V, 0, 512, s))) return RFS_IO_ERROR;        /* sector 0 */
    return RfsHostWrite(V, ((F->Blocks << BLKSH) - 512), 512, s);            /* the backup, last sector */
}

/*
 * Formats the device as a ReFS 1.2 volume. ClusterSize must be 65536 (the
 * only size ReFS 1.x uses); the device must be at least ~16 MiB.
 */
RFS_STATUS
RfsFormatV1(PRFS_VOL V, const char *Label, ULONG ClusterSize)
{
    FMT F;
    RFS_REF roots[REFS3_NROOTS], vi, sec, rootdir, fsmeta;
    OBJENT ents[4];
    ULONGLONG ck0, ck1;
    ULONG i;
    PUCHAR upcase = NULL, s540 = NULL;
    RFS_STATUS st;
    USHORT *up;

    RfsZero(&F, sizeof(F));
    F.V = V;
    if (!ClusterSize) ClusterSize = 65536;
    if (ClusterSize != 65536) return RFS_NOT_SUPPORTED;
    V->V1 = TRUE;
    V->ClusterSize = ClusterSize;
    for (V->ClusterShift = 0; (1UL << V->ClusterShift) < ClusterSize; V->ClusterShift++) ;
    V->PageSize = BLK; V->PageShift = BLKSH; V->HeaderSize = REFS1_PH_SIZE; V->RefSize = REFS1_REF_SIZE;
    V->CkType = REFS_CK_CRC64; V->SelfCkType = REFS_CK_CRC64;
    V->Serial = RfsHostNowNt() | 1;
    for (i = 0; i < 16; i++) V->Guid[i] = (UCHAR)(V->Serial >> ((i & 7) * 8)) ^ (UCHAR)(i * 37 + 1);
    V->VolSig = RfsGet32(V->Guid) ^ RfsGet32(V->Guid + 4) ^ RfsGet32(V->Guid + 8) ^ RfsGet32(V->Guid + 12);
    F.Blocks = V->DeviceBytes >> BLKSH;
    if (F.Blocks < 0x400) return RFS_NOT_SUPPORTED;
    V->TotalClusters = F.Blocks;                         /* 1.x: counts 16 KiB blocks */
    F.TailCluster = (F.Blocks - 1) / (ClusterSize / BLK);
    /* the schema rules, so the generic tree-insert orders keys correctly */
    for (i = 0; i < RFS1_NSCHEMAS; i++) {
        ULONG id = Rfs1Schemas[i].Id, rule = RfsGet32(Rfs1Schemas[i].Val + 0x1C);
        if (id >= 0xE000 && id < 0xE200) V->SysRules[(id - 0xE000) >> 4] = (UCHAR)rule;
        else if (id < 0x2000) V->Rules[id >> 4] = (UCHAR)rule;
    }
    F.Page = (PUCHAR)RfsHostAlloc(BLK);
    up = (PUSHORT)RfsHostAlloc(65536 * 2);
    s540 = (PUCHAR)RfsHostAlloc(65536);
    if (!F.Page || !up || !s540) { st = RFS_NO_MEMORY; goto out; }
    RfsDefaultUpcase(up);                                /* the engine's upcase table */
    RfsZero(s540, 65536);
    upcase = (PUCHAR)up;

    /* metadata starts after the boot area (block 0x20 = cluster 8) */
    F.Next = 0x20;
    F.UsedLo = 0;
    ck0 = GetBlock(&F); ck1 = GetBlock(&F);              /* the two checkpoint blocks */

    st = BuildLarge(&F, GetBlock(&F), &roots[REFS1_ROOT_LARGE]);
    /* medium is built last (after every cluster is used); reserve its block now */
    { ULONGLONG mediumblk = GetBlock(&F);
      if (RFS_SUCCESS(st)) st = BuildSmall(&F, GetBlock(&F), &roots[REFS1_ROOT_SMALL]);
      if (RFS_SUCCESS(st)) st = BuildSchema(&F, GetBlock(&F), &roots[REFS_ROOT_SCHEMA]);
      if (RFS_SUCCESS(st)) st = BuildParentChild(&F, GetBlock(&F), &roots[REFS_ROOT_PARENTCHILD]);
      if (RFS_SUCCESS(st)) st = BuildSecurity(&F, GetBlock(&F), &sec);
      if (RFS_SUCCESS(st)) st = BuildVolInfo(&F, Label ? Label : "ReFS", upcase, s540, GetBlock(&F), &vi);
      if (RFS_SUCCESS(st)) st = BuildDir(&F, REFS_OID_ROOT, 0x487e2d1b, 1, GetBlock(&F), &rootdir);
      (VOID)fsmeta;
      /* all clusters now used up to F.Next */
      F.UsedHi = (F.Next + (ClusterSize / BLK) - 1) / (ClusterSize / BLK);
      if (RFS_SUCCESS(st)) {
          ents[0].Oid = REFS_OID_VOLINFO; ents[0].NextId = 0x500; ents[0].Ref = vi;
          ents[1].Oid = REFS_OID_SECURITY; ents[1].NextId = 0; ents[1].Ref = sec;
          ents[2].Oid = REFS_OID_ROOT; ents[2].NextId = 1; ents[2].Ref = rootdir;
          st = BuildObject(&F, ents, 3, GetBlock(&F), &roots[REFS_ROOT_OBJECT]);
      }
      /* now every used cluster is known: build medium and large reflect it */
      F.UsedHi = (F.Next + (ClusterSize / BLK) - 1) / (ClusterSize / BLK);
      if (RFS_SUCCESS(st)) st = BuildMedium(&F, mediumblk, &roots[REFS1_ROOT_MEDIUM]);
      if (RFS_SUCCESS(st)) st = BuildLarge(&F, roots[REFS1_ROOT_LARGE].Lcn[0], &roots[REFS1_ROOT_LARGE]);
    }
    if (RFS_SUCCESS(st)) st = WriteCheckpoint(&F, ck0, 1, roots);
    if (RFS_SUCCESS(st)) st = WriteCheckpoint(&F, ck1, 0, roots);
    if (RFS_SUCCESS(st)) st = WriteSuper(&F, ck0, ck1);
    if (RFS_SUCCESS(st)) st = WriteVbr(&F);
    if (RFS_SUCCESS(st)) st = RfsHostFlush(V);
out:
    RfsHostFree(F.Page); RfsHostFree(up); RfsHostFree(s540);
    return st;
}

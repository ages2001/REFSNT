/*
 * REFSNT - ReFS 1.x (Windows 8 / Server 2012 / 8.1 / 2012 R2).
 *
 * The on-disk format is described in doc/REFS1.md (decoded from a real
 * Windows 8.1 ReFS 1.2 volume and refs.sys / uReFS.dll). 1.x differs from 3.x
 * in ways the shared engine already parameterises (16 KiB blocks and pages,
 * 48-byte page headers, 24-byte references, u32 row value lengths, the index
 * root at page+0x30); the rest - mounting, the different checkpoint, the three
 * global allocators with a private allocator per table, and the file/extent
 * record layout - is here.
 */
#include "rfseng.h"

#define BLK                     REFS1_PAGE              /* 16 KiB */
#define BLKSH                   REFS1_BLOCK_SHIFT       /* 14 */

/* ------------------------------------------------------------------ */
/* Mount (read)                                                        */
/* ------------------------------------------------------------------ */

/* a 16 KiB block's self-reference checksum (the whole block, its reference zeroed) */
static BOOLEAN
SelfOk1(PRFS_VOL V, PUCHAR Page, ULONG DescOff, ULONG DescLen)
{
    UCHAR save[24], ck[32];
    PUCHAR d;
    ULONG type, len, n;
    if (DescOff + DescLen > BLK || DescLen != REFS1_REF_SIZE) return FALSE;
    d = Page + DescOff;
    type = d[REFS1_REF_CKTYPE];
    if (type == REFS_CK_NONE) return TRUE;
    len = RfsGet16(d + REFS1_REF_CKLEN);
    if (len > 8) return FALSE;
    RfsCopy(save, d, DescLen);
    RfsZero(d, DescLen);
    n = RfsChecksum((UCHAR)type, Page, BLK, ck);
    RfsCopy(d, save, DescLen);
    V->SelfCkType = (UCHAR)type;
    return (BOOLEAN)(n == len && RfsCmp(ck, save + REFS1_REF_CK, n) == 0);
}

/* block Blk into Buf (one 16 KiB page) */
static RFS_STATUS
ReadBlk(PRFS_VOL V, ULONGLONG Blk, PUCHAR Buf)
{
    return RfsHostRead(V, Blk << BLKSH, BLK, Buf);
}

static RFS_STATUS
ReadSuper1(PRFS_VOL V, PUCHAR Buf)
{
    ULONGLONG where[3];
    ULONG i, off, cnt;
    where[0] = REFS_SB_LOCATION;             /* block 30 */
    where[1] = V->TotalClusters - 2;
    where[2] = V->TotalClusters - 3;
    for (i = 0; i < 3; i++) {
        if (!RFS_SUCCESS(ReadBlk(V, where[i], Buf))) continue;
        if (RfsGet64(Buf + REFS1_PH_BLOCK) != where[i]) continue;
        if (!SelfOk1(V, Buf, RfsGet32(Buf + REFS1_SB_SELF_OFF), RfsGet32(Buf + REFS1_SB_SELF_LEN))) continue;
        off = RfsGet32(Buf + REFS1_SB_CHKP_OFF);
        cnt = RfsGet32(Buf + REFS1_SB_CHKP_COUNT);
        if (cnt != 2 || off + 16 > BLK) continue;
        V->ChkpLcn[0] = RfsGet64(Buf + off);
        V->ChkpLcn[1] = RfsGet64(Buf + off + 8);
        RfsCopy(V->Guid, Buf + REFS1_SB_GUID, 16);
        V->VolSig = RfsGet32(V->Guid) ^ RfsGet32(V->Guid + 4) ^ RfsGet32(V->Guid + 8) ^ RfsGet32(V->Guid + 12);
        return RFS_OK;
    }
    return RFS_CORRUPT;
}

/* the table id a root page belongs to (page header +0x18) */
static ULONGLONG
RootTable(PRFS_VOL V, const RFS_REF *R)
{
    PRFS_PAGE P;
    ULONGLONG tid = 0;
    if (RFS_SUCCESS(RfsPageRead(V, R, TRUE, &P))) {
        tid = RfsGet64(P->Data + REFS1_PH_TABLE);
        RfsPagePut(V, P);
    }
    return tid;
}

static RFS_STATUS
ParseCheckpoint1(PRFS_VOL V, PUCHAR P)
{
    ULONG n, i, base;
    if (RfsGet16(P + REFS1_CP_MAJOR) != 1) return RFS_CORRUPT;
    n = RfsGet32(P + REFS1_CP_NROOTS);
    if (n != REFS1_NROOTS) return RFS_CORRUPT;
    V->RefSize = REFS1_REF_SIZE;
    V->Clock = RfsGet64(P + REFS1_CP_CLOCK);
    V->AllocClock = V->Clock;
    V->ChkpFlags = 0;
    V->NRoots = REFS3_NROOTS;                /* logical slots; v1 fills a few */
    base = REFS1_CP_ROOTS;
    for (i = 0; i < REFS3_NROOTS; i++) { RfsZero(&V->Roots[i], sizeof(RFS_REF)); V->RootOff[i] = 0; }
    /* the 6 v1 roots, in checkpoint order, classified by their page's table id */
    for (i = 0; i < n; i++) {
        ULONG off = RfsGet32(P + base + 4 * i);
        ULONG slot;
        RFS_REF ref;
        ULONGLONG tid;
        if (off + REFS1_REF_SIZE > BLK) return RFS_CORRUPT;
        RfsRefGet(V, P + off, &ref);
        tid = RootTable(V, &ref);
        switch (tid) {
        case REFS_TID_OBJECT:       slot = REFS_ROOT_OBJECT;      break;
        case REFS_TID_SCHEMA:       slot = REFS_ROOT_SCHEMA;      break;
        case REFS_TID_PARENTCHILD:  slot = REFS_ROOT_PARENTCHILD; break;
        case REFS1_TID_LARGE:       slot = REFS1_ROOT_LARGE;      break;
        case REFS1_TID_MEDIUM:      slot = REFS1_ROOT_MEDIUM;     break;
        case REFS1_TID_SMALL:       slot = REFS1_ROOT_SMALL;      break;
        default:                    continue;
        }
        V->Roots[slot] = ref;
        V->RootOff[slot] = off;
    }
    if (!RfsRefValid(V, &V->Roots[REFS_ROOT_OBJECT]) || !RfsRefValid(V, &V->Roots[REFS_ROOT_SCHEMA]))
        return RFS_CORRUPT;
    V->CkType = V->Roots[REFS_ROOT_OBJECT].CkType;
    return RFS_OK;
}

static RFS_STATUS
ReadCheckpoint1(PRFS_VOL V)
{
    PUCHAR buf[2];
    ULONG i, best = 2;
    ULONGLONG clk[2];
    RFS_STATUS st = RFS_CORRUPT;
    buf[0] = (PUCHAR)RfsHostAlloc(BLK);
    buf[1] = (PUCHAR)RfsHostAlloc(BLK);
    if (!buf[0] || !buf[1]) { RfsHostFree(buf[0]); RfsHostFree(buf[1]); return RFS_NO_MEMORY; }
    for (i = 0; i < 2; i++) {
        clk[i] = 0;
        if (!RFS_SUCCESS(ReadBlk(V, V->ChkpLcn[i], buf[i]))) continue;
        if (RfsGet64(buf[i] + REFS1_PH_BLOCK) != V->ChkpLcn[i]) continue;
        if (!SelfOk1(V, buf[i], RfsGet32(buf[i] + REFS1_CP_SELF_OFF), RfsGet32(buf[i] + REFS1_CP_SELF_LEN))) continue;
        clk[i] = RfsGet64(buf[i] + REFS1_CP_CLOCK) + 1;              /* +1: distinguish from "not read" */
        if (best == 2 || clk[i] > clk[best]) best = i;
    }
    if (best < 2) {
        st = ParseCheckpoint1(V, buf[best]);
        if (!RFS_SUCCESS(st) && clk[best ^ 1]) { best ^= 1; st = ParseCheckpoint1(V, buf[best]); }
    }
    if (RFS_SUCCESS(st)) { V->Chkp = buf[best]; V->ChkpSlot = best; RfsHostFree(buf[best ^ 1]); }
    else { RfsHostFree(buf[0]); RfsHostFree(buf[1]); }
    return st;
}

/* the schema table's rules (same row format as 3.x) */
static RFS_STATUS
LoadSchemas1(PRFS_VOL V)
{
    RFS_CURSOR c;
    RFS_STATUS st;
    st = RfsTableOpenRoot(V, REFS_ROOT_SCHEMA, &V->Schema);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsSeek(V->Schema, NULL, 0, &c);
    while (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        if (r->KeyLen >= 4 && r->ValLen >= 0x20) {
            ULONG id = RfsGet32(r->Key), rule = RfsGet32(r->Val + 0x1C);
            if (id >= 0xE000 && id < 0xE200) V->SysRules[(id - 0xE000) >> 4] = (UCHAR)rule;
            else if (id < 0x2000) V->Rules[id >> 4] = (UCHAR)rule;
        }
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    return st == RFS_NO_MORE ? RFS_OK : RFS_CORRUPT;
}

/* a stream row of the volume-information table (OID 0x500): its extent blocks into Buf */
static RFS_STATUS
ReadVolStream(PRFS_VOL V, PRFS_TABLE T, ULONG Row530, PUCHAR Buf, ULONG Max, PULONG Got)
{
    RFS_CURSOR c;
    RFS_STATUS st;
    UCHAR key[8];
    *Got = 0;
    RfsZero(key, 8);
    RfsPut16(key, (USHORT)Row530);
    st = RfsLookup(T, key, 8, &c);
    if (!RFS_SUCCESS(st)) return st;
    {
        PRFS_ROW r = RfsCurRow(&c);
        if ((r->Flags & REFS1_RF_EMBEDDED) && r->ValLen >= 0x30) {
            PRFS_TABLE S;
            st = RfsTableOpenEmbedded(V, T->Oid, r->Val, r->ValLen, &S);
            if (RFS_SUCCESS(st)) {
                RFS_CURSOR e;
                st = RfsSeek(S, NULL, 0, &e);
                while (RFS_SUCCESS(st)) {
                    PRFS_ROW x = RfsCurRow(&e);
                    if (x->ValLen >= REFS1_EXT_FLAGS) {
                        ULONGLONG vcn = RfsGet64(x->Val + REFS1_EXT_VCN);
                        ULONGLONG cnt = RfsGet64(x->Val + REFS1_EXT_COUNT);
                        ULONGLONG lcn = RfsGet64(x->Val + REFS1_EXT_LCN);
                        ULONGLONG b;
                        for (b = 0; b < cnt; b++) {
                            ULONG off = (ULONG)((vcn + b) << BLKSH);
                            if (off + BLK <= Max)
                                if (RFS_SUCCESS(ReadBlk(V, lcn + b, Buf + off)) && off + BLK > *Got) *Got = off + BLK;
                        }
                    }
                    st = RfsNext(&e);
                }
                RfsCursorClose(&e);
                RfsTableClose(V, S);
                if (st == RFS_NO_MORE) st = RFS_OK;
            }
        } else st = RFS_NOT_FOUND;
    }
    RfsCursorClose(&c);
    return st;
}

static RFS_STATUS
LoadUpcase1(PRFS_VOL V)
{
    PRFS_TABLE T;
    RFS_STATUS st;
    PUCHAR raw;
    ULONG got = 0, i;
    V->Upcase = (PUSHORT)RfsHostAlloc(65536 * 2);
    if (!V->Upcase) return RFS_NO_MEMORY;
    RfsDefaultUpcase(V->Upcase);                 /* fallback */
    st = RfsTableOpenObject(V, REFS_OID_VOLINFO, &T);
    if (!RFS_SUCCESS(st)) return RFS_OK;
    raw = (PUCHAR)RfsHostAlloc(65536 * 2);
    if (!raw) { RfsTableClose(V, T); return RFS_NO_MEMORY; }
    st = ReadVolStream(V, T, 0x530, raw, 65536 * 2, &got);
    RfsTableClose(V, T);
    if (RFS_SUCCESS(st) && got >= 65536 * 2)
        for (i = 0; i < 65536; i++) V->Upcase[i] = RfsGet16(raw + 2 * i);
    RfsHostFree(raw);
    return RFS_OK;
}

static VOID
LoadLabel1(PRFS_VOL V)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    UCHAR key[8];
    V->Label[0] = 0;
    if (!RFS_SUCCESS(RfsTableOpenObject(V, REFS_OID_VOLINFO, &T))) return;
    RfsZero(key, 8);
    RfsPut16(key, 0x510);
    if (RFS_SUCCESS(RfsLookup(T, key, 8, &c))) {
        PRFS_ROW r = RfsCurRow(&c);
        USHORT name[32];
        ULONG n = r->ValLen / 2, i;
        if (n > 32) n = 32;
        for (i = 0; i < n; i++) name[i] = RfsGet16(r->Val + 2 * i);
        while (n && name[n - 1] == 0) n--;
        RfsUtf16ToUtf8(name, n, V->Label, sizeof(V->Label) - 1);
        RfsCursorClose(&c);
    }
    RfsTableClose(V, T);
}

RFS_STATUS
RfsMountV1(PRFS_VOL V)
{
    RFS_STATUS st;
    PUCHAR sb;
    V->Scratch = (PUCHAR)RfsHostAlloc(BLK);
    sb = (PUCHAR)RfsHostAlloc(BLK);
    if (!sb || !V->Scratch) { RfsHostFree(sb); return RFS_NO_MEMORY; }
    st = ReadSuper1(V, sb);
    RfsHostFree(sb);
    if (RFS_SUCCESS(st)) st = ReadCheckpoint1(V);
    if (RFS_SUCCESS(st)) st = LoadSchemas1(V);
    if (RFS_SUCCESS(st)) st = RfsTableOpenRoot(V, REFS_ROOT_OBJECT, &V->Object);
    if (RFS_SUCCESS(st)) st = LoadUpcase1(V);
    if (RFS_SUCCESS(st)) LoadLabel1(V);
    return st;
}

/* ------------------------------------------------------------------ */
/* Allocation (medium allocator: 1 bit = 1 cluster = 4 blocks)          */
/* ------------------------------------------------------------------ */

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
#define CPB(V)      ((V)->ClusterSize / REFS1_PAGE)         /* blocks a cluster (4) */
/* file data units: 1.x addresses data in 16 KiB blocks (taken a whole 64 KiB
 * cluster at a time); 2.x in clusters, through containers as 3.x does */
#define DUSH(V)     ((V)->V2 ? (V)->ClusterShift : REFS1_BLOCK_SHIFT)
#define DUSZ(V)     (1UL << DUSH(V))
#define DUCPB(V)    ((V)->V2 ? 1 : CPB(V))

static VOID B1Set(PUCHAR B, ULONG i) { B[i >> 3] |= (UCHAR)(1 << (i & 7)); }
static VOID B1Clr(PUCHAR B, ULONG i) { B[i >> 3] &= (UCHAR)~(1 << (i & 7)); }
static BOOLEAN B1Get(const UCHAR *B, ULONG i) { return (BOOLEAN)((B[i >> 3] >> (i & 7)) & 1); }

/* recompute free count and the largest free run of a bitmap row */
static VOID
RowRecount(PUCHAR V)
{
    ULONG bits = RfsGet32(V + AR1_BITS), bo = RfsGet32(V + AR1_BMOFF), i;
    ULONG free = 0, lrs = 0, lrl = 0, cur = 0;
    const UCHAR *bm = V + bo;
    for (i = 0; i < bits; i++) {
        if (!B1Get(bm, i)) { free++; cur++; if (cur > lrl) { lrl = cur; lrs = i - cur + 1; } }
        else cur = 0;
    }
    RfsPut32(V + AR1_FREE, free);
    RfsPut32(V + AR1_LRS, lrs);
    RfsPut32(V + AR1_LRL, lrl);
}

/*
 * Free 16 KiB blocks, from the allocator rows themselves: each row's free
 * count (in bits) times its bit size. The medium allocator's rows cover the
 * chunks it was handed; the large allocator's clear bits are whole free
 * chunks. (The summaries in the roots' header data use different units on
 * volumes Windows made.)
 */
ULONGLONG
RfsFreeBlocksV1(PRFS_VOL V)
{
    static const ULONG roots[2] = { REFS1_ROOT_MEDIUM, REFS1_ROOT_LARGE };
    ULONGLONG blocks = 0;
    ULONG k;
    for (k = 0; k < 2; k++) {
        PRFS_TABLE T;
        RFS_CURSOR c;
        RFS_STATUS st;
        if (!RFS_SUCCESS(RfsTableOpenRoot(V, roots[k], &T))) continue;
        st = RfsSeek(T, NULL, 0, &c);
        while (RFS_SUCCESS(st) && st != RFS_NO_MORE) {
            PRFS_ROW r = RfsCurRow(&c);
            if (r->ValLen >= AR1_BITMAP && RfsGet32(r->Val + AR1_SHIFT) < 32)
                blocks += (ULONGLONG)RfsGet32(r->Val + AR1_FREE) << RfsGet32(r->Val + AR1_SHIFT);
            st = RfsNext(&c);
        }
        RfsCursorClose(&c);
        RfsTableClose(V, T);
    }
    return blocks;
}

/* the medium allocator's free-cluster summary */
static VOID
MedSummary(PRFS_TABLE T, LONGLONG Delta)
{
    PRFS_NODE r = T->Root;
    if (r->Desc && r->DescLen >= REFS_IR_DATA + 16) {
        RfsPut64(r->Desc + REFS_IR_DATA + 8, RfsGet64(r->Desc + REFS_IR_DATA + 8) + (ULONGLONG)Delta);
        r->Dirty = TRUE;
    }
}

/* is the cluster at block B quarantined (freed this transaction)? */
static BOOLEAN
Quarantined(PRFS_VOL V, ULONGLONG Blk)
{
    ULONG i;
    for (i = 0; i < V->NQuar; i++)
        if (Blk >= V->Quarantine[i].Start && Blk < V->Quarantine[i].Start + V->Quarantine[i].Count) return TRUE;
    return FALSE;
}

static RFS_STATUS
QuarAdd1(PRFS_VOL V, ULONGLONG Start, ULONGLONG Count)
{
    if (V->NQuar == V->CapQuar) {
        ULONG cap = V->CapQuar ? V->CapQuar * 2 : 64;
        PRFS_RUN n = (PRFS_RUN)RfsHostAlloc(cap * sizeof(RFS_RUN));
        if (!n) return RFS_NO_MEMORY;
        if (V->Quarantine) { RfsCopy(n, V->Quarantine, V->NQuar * sizeof(RFS_RUN)); RfsHostFree(V->Quarantine); }
        V->Quarantine = n; V->CapQuar = cap;
    }
    V->Quarantine[V->NQuar].Start = Start;
    V->Quarantine[V->NQuar].Count = Count;
    V->NQuar++;
    return RFS_OK;
}

/*
 * Allocate Count clusters (contiguous) from the medium allocator; *Blk gets
 * the first block (cluster * CPB). Quarantined clusters are skipped.
 */
static RFS_STATUS
MedAllocIn(PRFS_VOL V, ULONG Count, PULONGLONG Blk)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    RFS_STATUS st;
    ULONG cpb = CPB(V);
    *Blk = 0;
    st = RfsGlobal(V, REFS1_ROOT_MEDIUM, &T);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsSeek(T, NULL, 0, &c);
    while (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        if (r->ValLen >= AR1_BITMAP && RfsGet32(r->Val + AR1_FREE) >= Count) {
            ULONGLONG start = RfsGet64(r->Val + AR1_START);
            ULONG bits = RfsGet32(r->Val + AR1_BITS), bo = RfsGet32(r->Val + AR1_BMOFF);
            ULONG run = 0, i, first = 0;
            PUCHAR val;
            const UCHAR *bm0 = r->Val + bo;
            for (i = 0; i < bits; i++) {
                ULONGLONG blk = start + (ULONGLONG)i * cpb;
                if (!B1Get(bm0, i) && !Quarantined(V, blk)) {
                    if (run == 0) first = i;
                    if (++run == Count) break;
                } else run = 0;
            }
            if (run == Count) {
                st = RfsTreeModifyAt(&c, &val);
                if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st; }
                for (i = 0; i < Count; i++) B1Set(val + bo, first + i);
                RowRecount(val);
                MedSummary(T, -(LONGLONG)Count * cpb);       /* (the summary counts blocks) */
                V->AllocChanged = TRUE;
                if (V->FreeClusters >= Count) V->FreeClusters -= Count;
                *Blk = start + (ULONGLONG)first * cpb;
                RfsCursorClose(&c);
                return RFS_OK;
            }
        }
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    return RFS_DISK_FULL;
}

/*
 * The medium allocator takes one more chunk (1 bit of the large allocator,
 * 0x1000 blocks = 64 MiB) when none of its rows has the run asked for: the
 * chunk's bit is set in the large row and a medium row over it, all free, is
 * added - laid out like the medium rows Windows made (the first is the
 * template).
 */
static RFS_STATUS
MedGrow(PRFS_VOL V)
{
    PRFS_TABLE L, M;
    RFS_CURSOR c;
    PUCHAR val, nv = NULL;
    ULONGLONG lstart, chunk, cstart;
    ULONG lbits, lshift, lbo, i, nvl = 0, mshift;
    RFS_STATUS st;
    st = RfsGlobal(V, REFS1_ROOT_LARGE, &L);
    if (RFS_SUCCESS(st)) st = RfsGlobal(V, REFS1_ROOT_MEDIUM, &M);
    if (!RFS_SUCCESS(st)) return st;
    /* the template: the first medium row */
    st = RfsSeek(M, NULL, 0, &c);
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st == RFS_NO_MORE ? RFS_DISK_FULL : st; }
    {
        PRFS_ROW r = RfsCurRow(&c);
        if (r->ValLen < AR1_BITMAP || r->ValLen > 0x1000) { RfsCursorClose(&c); return RFS_DISK_FULL; }
        nvl = r->ValLen;
        nv = (PUCHAR)RfsHostAlloc(nvl);
        if (!nv) { RfsCursorClose(&c); return RFS_NO_MEMORY; }
        RfsCopy(nv, r->Val, nvl);
    }
    RfsCursorClose(&c);
    mshift = RfsGet32(nv + AR1_SHIFT);
    /* a free chunk in the large allocator (one row over the volume) */
    st = RfsSeek(L, NULL, 0, &c);
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); RfsHostFree(nv); return st == RFS_NO_MORE ? RFS_DISK_FULL : st; }
    {
        PRFS_ROW r = RfsCurRow(&c);
        if (r->ValLen < AR1_BITMAP) { RfsCursorClose(&c); RfsHostFree(nv); return RFS_DISK_FULL; }
        lstart = RfsGet64(r->Val + AR1_START);
        lbits = RfsGet32(r->Val + AR1_BITS);
        lshift = RfsGet32(r->Val + AR1_SHIFT);
        lbo = RfsGet32(r->Val + AR1_BMOFF);
        if (lbo + (lbits + 7) / 8 > r->ValLen || lshift >= 32 || lshift <= mshift) { RfsCursorClose(&c); RfsHostFree(nv); return RFS_DISK_FULL; }
        for (i = 0; i < lbits; i++) if (!B1Get(r->Val + lbo, i)) break;
        if (i == lbits) { RfsCursorClose(&c); RfsHostFree(nv); return RFS_DISK_FULL; }
        /* the chunk must fit the template's bitmap */
        if ((1UL << (lshift - mshift)) > (nvl - RfsGet32(nv + AR1_BMOFF)) * 8) { RfsCursorClose(&c); RfsHostFree(nv); return RFS_DISK_FULL; }
        chunk = i;
        st = RfsTreeModifyAt(&c, &val);
        if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); RfsHostFree(nv); return st; }
        B1Set(val + lbo, i);
        RowRecount(val);
    }
    RfsCursorClose(&c);
    MedSummary(L, -(LONGLONG)(1ULL << lshift));         /* large: blocks free */
    /* the new medium row */
    cstart = lstart + (chunk << lshift);
    RfsPut64(nv + AR1_START, cstart);
    RfsPut64(nv + AR1_LENGTH, 1ULL << lshift);
    RfsPut32(nv + AR1_BITS, 1UL << (lshift - mshift));
    RfsZero(nv + RfsGet32(nv + AR1_BMOFF), nvl - RfsGet32(nv + AR1_BMOFF));
    RowRecount(nv);
    st = RfsTreeInsert(M, nv, 16, nv, nvl, 0, FALSE);
    RfsHostFree(nv);
    if (!RFS_SUCCESS(st)) return st;
    {
        PRFS_NODE r = M->Root;      /* medium: blocks covered and free */
        if (r->Desc && r->DescLen >= REFS_IR_DATA + 16) {
            RfsPut64(r->Desc + REFS_IR_DATA, RfsGet64(r->Desc + REFS_IR_DATA) + (1ULL << lshift));
            r->Dirty = TRUE;
        }
        MedSummary(M, (LONGLONG)(1ULL << lshift));
    }
    V->AllocChanged = TRUE;
    return RFS_OK;
}

static RFS_STATUS
MedAlloc(PRFS_VOL V, ULONG Count, PULONGLONG Blk)
{
    RFS_STATUS st = MedAllocIn(V, Count, Blk);
    if (st == RFS_DISK_FULL) {
        /* a run longer than a row never fits one: the caller takes shorter ones */
        PRFS_TABLE M;
        RFS_CURSOR c;
        ULONG bits = 0;
        if (RFS_SUCCESS(RfsGlobal(V, REFS1_ROOT_MEDIUM, &M))) {
            if (RFS_SUCCESS(RfsSeek(M, NULL, 0, &c)) && RfsCurRow(&c)->ValLen >= AR1_BITMAP)
                bits = RfsGet32(RfsCurRow(&c)->Val + AR1_BITS);
            RfsCursorClose(&c);
        }
        if (Count <= bits && RFS_SUCCESS(MedGrow(V))) st = MedAllocIn(V, Count, Blk);
    }
    return st;
}

/* mark Count clusters free (quarantined now; the bits are cleared at commit) */
static RFS_STATUS
MedFree(PRFS_VOL V, ULONGLONG Blk, ULONG Count)
{
    return QuarAdd1(V, Blk, Count);
}

/* clear the bits of every quarantined run in the medium allocator */
static RFS_STATUS
MedApplyQuar(PRFS_VOL V)
{
    PRFS_TABLE T;
    ULONG q, cpb = CPB(V);
    RFS_STATUS st;
    if (!V->NQuar) return RFS_OK;
    st = RfsGlobal(V, REFS1_ROOT_MEDIUM, &T);
    if (!RFS_SUCCESS(st)) return st;
    for (q = 0; q < V->NQuar; q++) {
        ULONGLONG blk = V->Quarantine[q].Start, cnt = V->Quarantine[q].Count, i;
        RFS_CURSOR c;
        UCHAR key[16];
        RfsZero(key, 16);
        RfsPut64(key, blk);
        RfsPut64(key + 8, 1);
        st = RfsSeek(T, key, 16, &c);
        if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); continue; }
        {
            PRFS_ROW r = RfsCurRow(&c);
            ULONGLONG start = RfsGet64(r->Val + AR1_START);
            ULONG bo = RfsGet32(r->Val + AR1_BMOFF);
            PUCHAR val;
            st = RfsTreeModifyAt(&c, &val);
            if (RFS_SUCCESS(st)) {
                for (i = 0; i < cnt; i++) {
                    ULONG bit = (ULONG)((blk + i - start) / cpb);
                    B1Clr(val + bo, bit);
                }
                RowRecount(val);
                MedSummary(T, (LONGLONG)cnt);
                V->FreeClusters += cnt / cpb;
            }
            RfsCursorClose(&c);
        }
    }
    return RFS_OK;
}

RFS_STATUS
RfsAllocPageV1(PRFS_VOL V, PRFS_REF R)
{
    ULONGLONG blk;
    RFS_STATUS st = MedAlloc(V, 1, &blk);
    if (!RFS_SUCCESS(st)) return st;
    RfsZero(R, sizeof(*R));
    R->Lcn[0] = blk;
    R->CkType = REFS_CK_CRC64;
    return RFS_OK;
}

RFS_STATUS
RfsFreePageV1(PRFS_VOL V, const RFS_REF *R)
{
    return MedFree(V, R->Lcn[0], CPB(V));
}

/* clusters for file data: Count blocks rounded up to whole clusters, from medium */
RFS_STATUS
RfsAllocV1Data(PRFS_VOL V, ULONG Blocks, PULONGLONG Blk, PULONG Got)
{
    ULONG cpb = CPB(V), ncl = (Blocks + cpb - 1) / cpb;
    RFS_STATUS st;
    /* a run lies within one allocator row: when no row has ncl free in a
       row, take a shorter run (the caller asks again for the rest) */
    for (;;) {
        st = MedAlloc(V, ncl, Blk);
        if (st != RFS_DISK_FULL || ncl == 1) break;
        ncl = (ncl + 1) / 2;
    }
    if (!RFS_SUCCESS(st)) return st;
    *Got = ncl * cpb;
    return RFS_OK;
}

RFS_STATUS
RfsFreeV1Data(PRFS_VOL V, ULONGLONG Blk, ULONG Blocks)
{
    ULONG cpb = CPB(V);
    return MedFree(V, Blk & ~(ULONGLONG)(cpb - 1), ((Blocks + cpb - 1) / cpb) * cpb);
}

/* ------------------------------------------------------------------ */
/* Commit                                                              */
/* ------------------------------------------------------------------ */

static RFS_STATUS SetObjRootV1(PRFS_VOL V, ULONGLONG Oid, const RFS_REF *Ref, BOOLEAN TouchOnly);

/* new pages for the dirty nodes of T; *Placed counts them */
static RFS_STATUS
PlaceV1(PRFS_TABLE T, PRFS_NODE N, PULONG Placed)
{
    PRFS_VOL V = T->Vol;
    ULONG i;
    RFS_STATUS st;
    if (!N->Dirty) return RFS_OK;
    if (N->Level) {
        for (i = 0; i < N->NRows; i++)
            if (N->Rows[i].Child && N->Rows[i].Child->Dirty) {
                st = PlaceV1(T, N->Rows[i].Child, Placed);
                if (!RFS_SUCCESS(st)) return st;
            }
    }
    if (N->Placed) return RFS_OK;
    if (N->HasRef) { st = RfsFreePageV1(V, &N->Ref); if (!RFS_SUCCESS(st)) return st; N->HasRef = FALSE; }
    st = RfsAllocPageV1(V, &N->NewRef);
    if (!RFS_SUCCESS(st)) return st;
    N->Placed = TRUE;
    (*Placed)++;
    return RFS_OK;
}

static RFS_STATUS
EncodeV1(PRFS_TABLE T, PRFS_NODE N, ULONGLONG Clock, PRFS_REF Out)
{
    PRFS_VOL V = T->Vol;
    ULONG i;
    RFS_STATUS st;
    if (N->Level) {
        for (i = 0; i < N->NRows; i++) {
            PRFS_ROW r = &N->Rows[i];
            RFS_REF ref;
            if (!r->Child || !r->Child->Dirty) continue;
            st = EncodeV1(T, r->Child, Clock, &ref);
            if (!RFS_SUCCESS(st)) return st;
            st = RfsRowOwn(r);
            if (!RFS_SUCCESS(st)) return st;
            if (r->ValLen < REFS1_REF_SIZE) return RFS_CORRUPT;
            RfsRefPut(V, r->Val, &ref);
        }
    }
    if (!N->Placed) return RFS_CORRUPT;
    st = RfsEncodePage(T, N, &N->NewRef, Clock, 0, V->Scratch, Out);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsWritePage(V, &N->NewRef, FALSE, V->Scratch);
    if (!RFS_SUCCESS(st)) return st;
    RfsCopy(N->PageHdr, V->Scratch, V->HeaderSize);
    N->Ref = *Out; N->HasRef = TRUE; N->Placed = FALSE; N->Dirty = FALSE;
    return RFS_OK;
}

static RFS_STATUS
SetObjRootV1(PRFS_VOL V, ULONGLONG Oid, const RFS_REF *Ref, BOOLEAN TouchOnly)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    UCHAR key[16];
    PUCHAR val;
    RFS_STATUS st = RfsGlobal(V, REFS_ROOT_OBJECT, &T);
    if (!RFS_SUCCESS(st)) return st;
    RfsZero(key, 16);
    RfsPut64(key + 8, Oid);
    st = RfsLookup(T, key, 16, &c);
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st == RFS_NOT_FOUND ? RFS_CORRUPT : st; }
    st = RfsTreeModifyAt(&c, &val);
    if (RFS_SUCCESS(st) && !TouchOnly) RfsRefPut(V, val, Ref);
    RfsCursorClose(&c);
    return st;
}

/* the inactive checkpoint slot gets the new roots, clock + 1 and its self checksum */
static RFS_STATUS
WriteCheckpointV1(PRFS_VOL V, ULONGLONG Clock)
{
    PUCHAR p = (PUCHAR)RfsHostAlloc(REFS1_PAGE);
    ULONG slot = V->ChkpSlot ^ 1, i, so, sl;
    ULONGLONG lcn = V->ChkpLcn[slot];
    RFS_REF self;
    UCHAR ck[32];
    RFS_STATUS st;
    if (!p) return RFS_NO_MEMORY;
    RfsCopy(p, V->Chkp, REFS1_PAGE);
    RfsPut64(p + REFS1_PH_BLOCK, lcn);
    RfsPut64(p + REFS1_PH_CLOCK, Clock);
    RfsPut64(p + REFS1_CP_CLOCK, Clock);
    for (i = 0; i < REFS1_NROOTS; i++) {
        ULONG off = RfsGet32(p + REFS1_CP_ROOTS + 4 * i);
        ULONG slotidx;
        ULONGLONG tid;
        RFS_REF cur;
        if (off + REFS1_REF_SIZE > REFS1_PAGE) continue;
        RfsRefGet(V, p + off, &cur);
        /* which logical slot this checkpoint entry is: by its page's table id */
        {
            PRFS_PAGE pg;
            tid = 0;
            if (RFS_SUCCESS(RfsPageRead(V, &cur, TRUE, &pg))) { tid = RfsGet64(pg->Data + REFS1_PH_TABLE); RfsPagePut(V, pg); }
        }
        switch (tid) {
        case REFS_TID_OBJECT: slotidx = REFS_ROOT_OBJECT; break;
        case REFS_TID_SCHEMA: slotidx = REFS_ROOT_SCHEMA; break;
        case REFS_TID_PARENTCHILD: slotidx = REFS_ROOT_PARENTCHILD; break;
        case REFS1_TID_LARGE: slotidx = REFS1_ROOT_LARGE; break;
        case REFS1_TID_MEDIUM: slotidx = REFS1_ROOT_MEDIUM; break;
        case REFS1_TID_SMALL: slotidx = REFS1_ROOT_SMALL; break;
        default: continue;
        }
        RfsRefPut(V, p + off, &V->Roots[slotidx]);
    }
    so = RfsGet32(p + REFS1_CP_SELF_OFF);
    sl = RfsGet32(p + REFS1_CP_SELF_LEN);
    if (so + sl > REFS1_PAGE) { RfsHostFree(p); return RFS_CORRUPT; }
    RfsRefGet(V, p + so, &self);
    RfsZero(p + so, sl);
    self.Lcn[0] = lcn; self.Lcn[1] = self.Lcn[2] = self.Lcn[3] = 0;
    if (!self.CkType) self.CkType = REFS_CK_CRC64;
    self.CkLen = (UCHAR)RfsChecksum(self.CkType, p, REFS1_PAGE, ck);
    RfsCopy(self.Ck, ck, self.CkLen);
    RfsRefPut(V, p + so, &self);
    st = RfsHostWrite(V, lcn << REFS1_BLOCK_SHIFT, REFS1_PAGE, p);
    if (RFS_SUCCESS(st)) st = RfsHostFlush(V);
    if (!RFS_SUCCESS(st)) { RfsHostFree(p); return st; }
    RfsHostFree(V->Chkp);
    V->Chkp = p; V->ChkpSlot = slot; V->Clock = Clock;
    return RFS_OK;
}

static BOOLEAN
IsAllocTableV1(PRFS_TABLE T)
{
    return (BOOLEAN)(T->Where == RFS_TROOT_CHECKPOINT &&
                     (T->Index == REFS1_ROOT_MEDIUM || T->Index == REFS1_ROOT_LARGE || T->Index == REFS1_ROOT_SMALL));
}

RFS_STATUS
RfsCommitV1(PRFS_VOL V)
{
    PRFS_TABLE T;
    RFS_STATUS st = RFS_OK;
    ULONGLONG clock = V->Clock + 1;
    ULONG placed, rounds;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (!V->Dirty || !V->DirtyHead) { V->Dirty = FALSE; return RFS_OK; }
    V->InCommit = TRUE;

    /* clear the bits of clusters freed this transaction before placing new pages */
    st = MedApplyQuar(V);
    if (!RFS_SUCCESS(st)) goto out;
    V->NQuar = 0;

    /* object rows of changed object tables (touch: they get the table's new root) */
    for (T = V->DirtyHead; T; T = T->DirtyNext)
        if (T->Where == RFS_TROOT_OBJECT && T->Dirty) {
            st = SetObjRootV1(V, T->Oid, NULL, TRUE);
            if (!RFS_SUCCESS(st)) goto out;
        }
    /* place pages: ordinary tables, then allocators until nothing new is left */
    placed = 0;
    for (T = V->DirtyHead; T; T = T->DirtyNext)
        if (T->Dirty && !IsAllocTableV1(T)) { st = PlaceV1(T, T->Root, &placed); if (!RFS_SUCCESS(st)) goto out; }
    for (rounds = 0; rounds < 64; rounds++) {
        placed = 0;
        for (T = V->DirtyHead; T; T = T->DirtyNext)
            if (T->Dirty && IsAllocTableV1(T)) { st = PlaceV1(T, T->Root, &placed); if (!RFS_SUCCESS(st)) goto out; }
        for (T = V->DirtyHead; T; T = T->DirtyNext)
            if (T->Dirty && !IsAllocTableV1(T)) { st = PlaceV1(T, T->Root, &placed); if (!RFS_SUCCESS(st)) goto out; }
        if (!placed) break;
    }
    if (rounds == 64) { st = RFS_CORRUPT; goto out; }
    /* encode: object table (+ its rows), then the rest */
    for (T = V->DirtyHead; T; T = T->DirtyNext) {
        RFS_REF ref;
        if (!T->Dirty || T->Where != RFS_TROOT_OBJECT) continue;
        st = EncodeV1(T, T->Root, clock, &ref);
        if (!RFS_SUCCESS(st)) goto out;
        T->RootRef = ref;
        st = SetObjRootV1(V, T->Oid, &ref, FALSE);
        if (!RFS_SUCCESS(st)) goto out;
    }
    for (T = V->DirtyHead; T; T = T->DirtyNext) {
        RFS_REF ref;
        if (!T->Dirty || T->Where != RFS_TROOT_CHECKPOINT) continue;
        if (!T->Root->Dirty) { T->Dirty = FALSE; continue; }
        st = EncodeV1(T, T->Root, clock, &ref);
        if (!RFS_SUCCESS(st)) goto out;
        T->RootRef = ref;
        V->Roots[T->Index] = ref;
    }
    st = RfsHostFlush(V);
    if (RFS_SUCCESS(st)) st = WriteCheckpointV1(V, clock);
    if (!RFS_SUCCESS(st)) goto out;
    V->NQuar = 0;
    V->AllocChanged = FALSE;
    while ((T = V->DirtyHead) != NULL) {
        V->DirtyHead = T->DirtyNext; T->DirtyNext = NULL; T->OnDirtyList = FALSE; T->Dirty = FALSE;
        RfsTableClose(V, T);
    }
    V->Dirty = FALSE;
out:
    V->InCommit = FALSE;
    if (!RFS_SUCCESS(st)) { V->ReadOnly = TRUE; V->ReadOnlyWhy = "a commit failed"; }
    return st;
}

/* ------------------------------------------------------------------ */
/* Creating and removing files and directories (1.x record layout)     */
/* ------------------------------------------------------------------ */

#define V1_RF_EMBEDDED  0x0008
#define DEF_SECHASH     0x487e2d1bUL        /* the default "everyone/system/admins" descriptor's hash */

static RFS_STATUS
V1NewOid(PRFS_VOL V, PULONGLONG Oid)
{
    if (!V->NextOid) {
        RFS_CURSOR c;
        PRFS_TABLE T;
        RFS_STATUS st = RfsGlobal(V, REFS_ROOT_OBJECT, &T);
        if (!RFS_SUCCESS(st)) return st;
        st = RfsSeekLast(T, &c);
        if (!RFS_SUCCESS(st)) return st;
        V->NextOid = RfsGet64(RfsCurRow(&c)->Key + 8) + 1;
        RfsCursorClose(&c);
        if (V->NextOid < REFS_OID_FIRST_USER) V->NextOid = REFS_OID_FIRST_USER;
    }
    *Oid = V->NextOid++;
    return RFS_OK;
}

/* read & bump a directory's next-file-id counter (the object row's +0x28) */
static RFS_STATUS
V2NextId(PRFS_VOL V, ULONGLONG Dir, PULONGLONG Id)
{
    static const ULONG roots[2] = { REFS_ROOT_OBJECT, REFS_ROOT_OBJECT2 };
    ULONGLONG id = 0;
    ULONG k;
    for (k = 0; k < 2; k++) {
        PRFS_TABLE T;
        RFS_CURSOR c;
        UCHAR key[16];
        PUCHAR val;
        ULONG po;
        RFS_STATUS st = RfsGlobal(V, roots[k], &T);
        if (!RFS_SUCCESS(st)) return st;
        RfsZero(key, 16);
        RfsPut64(key + 8, Dir);
        st = RfsLookup(T, key, 16, &c);
        if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); if (k) continue; return st; }
        po = RfsCurRow(&c)->ValLen >= 0x18 ? RfsGet32(RfsCurRow(&c)->Val + 0x10) : 0;
        if (po < 0x18 || po + 8 > RfsCurRow(&c)->ValLen) { RfsCursorClose(&c); return RFS_CORRUPT; }
        if (k == 0) { id = RfsGet64(RfsCurRow(&c)->Val + po) + 1; if (id < 2) id = 2; }
        st = RfsTreeModifyAt(&c, &val);
        if (RFS_SUCCESS(st)) RfsPut64(val + po, id);
        RfsCursorClose(&c);
        if (!RFS_SUCCESS(st)) return st;
    }
    *Id = id;
    return RFS_OK;
}

static RFS_STATUS
V1NextId(PRFS_VOL V, ULONGLONG Dir, PULONGLONG Id)
{
    if (V->V2) return V2NextId(V, Dir, Id);
    {
    PRFS_TABLE T;
    RFS_CURSOR c;
    UCHAR key[16];
    PUCHAR val;
    RFS_STATUS st = RfsGlobal(V, REFS_ROOT_OBJECT, &T);
    if (!RFS_SUCCESS(st)) return st;
    RfsZero(key, 16);
    RfsPut64(key + 8, Dir);
    st = RfsLookup(T, key, 16, &c);
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st; }
    if (RfsCurRow(&c)->ValLen < 0x30) { RfsCursorClose(&c); return RFS_CORRUPT; }
    *Id = RfsGet64(RfsCurRow(&c)->Val + 0x28);
    if (*Id < 1) *Id = 1;
    st = RfsTreeModifyAt(&c, &val);
    if (RFS_SUCCESS(st)) RfsPut64(val + 0x28, *Id + 1);
    RfsCursorClose(&c);
    return st;
    }
}

/* insert (or delete) the object-table row for a new table (ref filled at commit) */
/* 2.x: a 3.x-style object row as Windows writes it - header, the table's
 * reference at +0x20 (filled at commit), the next-file-id trailer at +0xC8 */
#define V2_OBJROW       0xF0
#define V2_OBJTRAILER   0xC8
static RFS_STATUS
V2ObjRow(PRFS_VOL V, ULONGLONG Oid, ULONGLONG NextId, BOOLEAN Add)
{
    static const ULONG roots[2] = { REFS_ROOT_OBJECT, REFS_ROOT_OBJECT2 };
    UCHAR key[16], val[V2_OBJROW];
    ULONG k;
    RfsZero(key, 16);
    RfsPut64(key + 8, Oid);
    RfsZero(val, sizeof(val));
    RfsPut32(val, 2);
    RfsPut32(val + 0x08, 0x18);
    RfsPut32(val + 0x0C, 0x30);
    RfsPut32(val + 0x10, V2_OBJTRAILER);
    RfsPut32(val + 0x14, 8);
    RfsPut64(val + 0x18, RfsGet64(V->Chkp + 0x70));
    RfsPut64(val + V2_OBJTRAILER, NextId);
    for (k = 0; k < 2; k++) {
        PRFS_TABLE T;
        RFS_STATUS st = RfsGlobal(V, roots[k], &T);
        if (!RFS_SUCCESS(st)) { if (k) break; return st; }
        if (Add) st = RfsTreeInsert(T, key, 16, val, V2_OBJROW, 0, FALSE);
        else { st = RfsTreeDelete(T, key, 16); if (st == RFS_NOT_FOUND) st = RFS_OK; }
        if (!RFS_SUCCESS(st)) return st;
    }
    return RFS_OK;
}

static RFS_STATUS
V1ObjRow(PRFS_VOL V, ULONGLONG Oid, ULONGLONG NextId, BOOLEAN Add)
{
    PRFS_TABLE T;
    UCHAR key[16], val[48];
    RFS_STATUS st;
    if (V->V2) return V2ObjRow(V, Oid, NextId, Add);
    st = RfsGlobal(V, REFS_ROOT_OBJECT, &T);
    if (!RFS_SUCCESS(st)) return st;
    RfsZero(key, 16);
    RfsPut64(key + 8, Oid);
    if (Add) {
        RfsZero(val, 48);
        val[REFS1_REF_CKTYPE] = REFS_CK_CRC64;
        val[REFS1_REF_CKOFF] = 8;
        RfsPut16(val + REFS1_REF_CKLEN, 8);
        RfsPut32(val + 0x20, 8);
        RfsPut32(val + 0x24, 8);
        RfsPut64(val + 0x28, NextId);
        return RfsTreeInsert(T, key, 16, val, 48, 0, FALSE);
    }
    st = RfsTreeDelete(T, key, 16);
    return st == RFS_NOT_FOUND ? RFS_OK : st;
}

static RFS_STATUS
V1ParentChild(PRFS_VOL V, ULONGLONG Parent, ULONGLONG Child, BOOLEAN Add)
{
    PRFS_TABLE T;
    UCHAR k[32];
    RFS_STATUS st = RfsGlobal(V, REFS_ROOT_PARENTCHILD, &T);
    if (!RFS_SUCCESS(st)) return st;
    RfsZero(k, 32);
    RfsPut64(k + 8, Parent);
    RfsPut64(k + 24, Child);
    if (Add) return RfsTreeInsert(T, k, 32, k, 32, 0, FALSE);
    st = RfsTreeDelete(T, k, 32);
    return st == RFS_NOT_FOUND ? RFS_OK : st;
}

/*
 * The header data of a file or directory-self record. 1.x: attributes +0x20,
 * parent/own OID +0x28, file id +0x30, security hash +0x38, size +0x40,
 * allocation +0x48, (directories) next file id +0x68. 2.x keeps the 3.x
 * layout instead: attributes +0x20, security hash +0x28, size +0x30,
 * allocation +0x38, file id / next file id +0x58 (value offsets 0x48, 0x50,
 * 0x58, 0x60, 0x80 - REFS_FH_*).
 */
static VOID
RecHeader(PRFS_VOL V, PUCHAR h, const RFS_TIMES *Tm, ULONG Attr, ULONGLONG Oid, ULONGLONG Id,
          ULONG SecHash, ULONGLONG Size, ULONGLONG Alloc, BOOLEAN DirSelf)
{
    RfsPut64(h + 0x00, Tm->Create); RfsPut64(h + 0x08, Tm->Write);
    RfsPut64(h + 0x10, Tm->Change); RfsPut64(h + 0x18, Tm->Access);
    if (V->V2) {
        RfsPut32(h + 0x20, DirSelf ? (Attr & ~0x10UL) : Attr);
        RfsPut32(h + 0x28, SecHash);
        RfsPut32(h + 0x2C, 1);
        RfsPut64(h + 0x30, Size);
        RfsPut64(h + 0x38, Alloc);
        RfsPut64(h + 0x58, Id);
        RfsPut64(h + 0x70, 1);
        return;
    }
    RfsPut32(h + 0x20, Attr);
    RfsPut64(h + 0x28, Oid);
    if (DirSelf) RfsPut64(h + 0x68, Id); else RfsPut64(h + 0x30, Id);
    RfsPut32(h + 0x38, SecHash);
    RfsPut32(h + 0x3C, 1);
    RfsPut64(h + 0x40, Size);
    RfsPut64(h + 0x48, Alloc);
    RfsPut64(h + 0x70, 1);
}
#define V1_SCHEMA_DIRSELF(V)  ((V)->V2 ? 0x170UL : 0x110UL)

/*
 * A directory's link attribute (type 0x38) in its self record: {u32 0, u32
 * payload size, u16 0x0C, u16 1}, then the directory's 72-byte index entry
 * as its parent holds it (parent OID at +0; 2.x +8; its times; the directory
 * flag), u32 name length, u16 0, and its name key {0x30, 2, name}. The row
 * value is rounded up to 4 bytes past a 16-byte allowance.
 */
#define V1_AT_LINK          0x38
#define V1_LINK_MAX         (0x10 + 0x48 + 6 + 4 + 2 * RFS_MAX_NAME + 4)
static ULONG
V1LinkValue(PRFS_VOL V, ULONGLONG Parent, const RFS_TIMES *Tm, const USHORT *Name, ULONG Len, PUCHAR Out)
{
    ULONG pay = 0x48 + 4 + 2 + 4 + 2 * Len, vl = (pay + 0x10 + 3) & ~3UL, i;
    PUCHAR e = Out + 0x0C;
    RfsZero(Out, vl);
    RfsPut32(Out + 0x04, pay);
    RfsPut16(Out + 0x08, 0x0C);
    RfsPut16(Out + 0x0A, 1);
    RfsPut64(e + (V->V2 ? 8 : 0), Parent);
    RfsPut64(e + 0x10, Tm->Create); RfsPut64(e + 0x18, Tm->Write);
    RfsPut64(e + 0x20, Tm->Change); RfsPut64(e + 0x28, Tm->Access);
    RfsPut32(e + 0x40, REFS_ATTR_DIRECTORY);
    RfsPut32(Out + 0x54, Len);
    RfsPut16(Out + 0x5A, REFS_KEY_NAME);
    RfsPut16(Out + 0x5C, REFS1_ENT_DIR);
    for (i = 0; i < Len; i++) RfsPut16(Out + 0x5E + 2 * i, Name[i]);
    return vl;
}

static RFS_STATUS
V1InsertLink(PRFS_TABLE T, ULONGLONG Parent, const RFS_TIMES *Tm, const USHORT *Name, ULONG Len)
{
    UCHAR k[14];
    PUCHAR v = (PUCHAR)RfsHostAlloc(V1_LINK_MAX);
    ULONG vl;
    RFS_STATUS st;
    if (!v) return RFS_NO_MEMORY;
    vl = V1LinkValue(T->Vol, Parent, Tm, Name, Len, v);
    RfsZero(k, 14);
    RfsPut64(k, vl);
    RfsPut32(k + 8, V1_AT_LINK);
    st = RfsTreeInsert(T, k, 14, v, vl, 0, FALSE);
    RfsHostFree(v);
    return st;
}

/* the directory self record value (embedded table, schema 0x110 (2.x: 0x170): link + $I30 stub) */
static RFS_STATUS
V1DirSelf(PRFS_VOL V, ULONGLONG Oid, ULONGLONG NextId, ULONG Attr, const RFS_TIMES *Tm, PUCHAR *Val, PULONG Len,
          ULONGLONG Parent, const USHORT *Name, ULONG NameLen)
{
    PRFS_TABLE T;
    UCHAR desc[REFS_IR_DATA + 0x80];
    PUCHAR h = desc + REFS_IR_DATA;
    RFS_STATUS st;
    static const UCHAR i30k[20] = { 0x94,0,0,0,0,0,0,0, 0x90,0,0,0, 0x24,0, '$',0,'I',0,'3',0 };
    /* the $I30 index-root stub: a fixed NTFS-compatibility blob */
    static const UCHAR i30v[148] = {
        0,0,0,0, 0x80,0,0,0, 0x0c,0,0,0, 0x30,0,0,0, 0x40,0x02,0x01,0,
        0x16,0,0,0, 0x10,0,0,0, 0x70,0,0,0, 0x70,0,0,0 };
    RfsZero(desc, sizeof(desc));
    RfsPut32(desc, REFS_IR_DATA + 0x80);
    RfsPut16(desc + REFS_IR_SIZE, 0x28);
    RfsPut16(desc + REFS_IR_KIND, 1);
    RfsPut32(desc + REFS_IR_ONE, 1);
    RfsPut32(desc + REFS_IR_SCHEMA, V1_SCHEMA_DIRSELF(V));
    RfsPut32(desc + REFS_IR_SCHEMA2, V1_SCHEMA_DIRSELF(V));
    RfsPut32(desc + REFS_IR_TWO, 2);
    RecHeader(V, h, Tm, Attr, Oid, NextId, DEF_SECHASH, 0, 0, TRUE);
    st = RfsNewTable(V, RFS_TROOT_EMBEDDED, Oid, V1_SCHEMA_DIRSELF(V), desc, REFS_IR_DATA + 0x80, &T);
    if (!RFS_SUCCESS(st)) return st;
    st = V1InsertLink(T, Parent, Tm, Name, NameLen);
    if (RFS_SUCCESS(st)) st = RfsTreeInsert(T, i30k, sizeof(i30k), i30v, sizeof(i30v), 0, FALSE);
    if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(T, Val, Len);
    RfsNodeFree(T->Root); RfsHostFree(T);
    return st;
}

/* an empty file record value (embedded table, schema 0x110, header only) */
static RFS_STATUS
V1FileRec(PRFS_VOL V, ULONGLONG Dir, ULONGLONG FileId, ULONG Attr, const RFS_TIMES *Tm, PUCHAR *Val, PULONG Len)
{
    PRFS_TABLE T;
    UCHAR desc[REFS_IR_DATA + 0x80];
    PUCHAR h = desc + REFS_IR_DATA;
    RFS_STATUS st;
    RfsZero(desc, sizeof(desc));
    RfsPut32(desc, REFS_IR_DATA + 0x80);
    RfsPut16(desc + REFS_IR_SIZE, 0x28);
    RfsPut16(desc + REFS_IR_KIND, 1);
    RfsPut32(desc + REFS_IR_ONE, 1);
    RfsPut32(desc + REFS_IR_SCHEMA, 0x110);
    RfsPut32(desc + REFS_IR_SCHEMA2, 0x110);
    RfsPut32(desc + REFS_IR_TWO, 2);
    RecHeader(V, h, Tm, Attr, Dir, FileId, DEF_SECHASH, 0, 0, FALSE);
    st = RfsNewTable(V, RFS_TROOT_EMBEDDED, Dir, 0x110, desc, REFS_IR_DATA + 0x80, &T);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsEmbeddedStore(T, Val, Len);
    RfsNodeFree(T->Root); RfsHostFree(T);
    return st;
}

/* the 72-byte subdirectory entry value */
static VOID
V1SubdirEntry(PRFS_VOL V, ULONGLONG Oid, const RFS_TIMES *Tm, ULONG Attr, PUCHAR E)
{
    RfsZero(E, 72);
    RfsPut64(E + (V->V2 ? REFS_IE_HOME : 0x00), Oid);
    RfsPut64(E + 0x10, Tm->Create);
    RfsPut64(E + 0x18, Tm->Write);
    RfsPut64(E + 0x20, Tm->Change);
    RfsPut64(E + 0x28, Tm->Access);
    RfsPut32(E + 0x40, Attr | REFS_ATTR_DIRECTORY);
}

/* name key {0x30, type, name} */
static ULONG
V1NameKey(const USHORT *Name, ULONG Len, USHORT Type, PUCHAR Key)
{
    ULONG i;
    RfsPut16(Key, REFS_KEY_NAME);
    RfsPut16(Key + 2, Type);
    for (i = 0; i < Len; i++) RfsPut16(Key + 4 + 2 * i, Name[i]);
    return 4 + 2 * Len;
}

/* the reverse (id -> name) row key: 1.x {0x80000020, 0, dirOID, fileId}; 2.x as 3.x {0x80000020, 0, fileId, 0} */
static VOID
V1RevKey(PRFS_VOL V, ULONGLONG Dir, ULONGLONG Id, PUCHAR k)
{
    RfsZero(k, 24);
    RfsPut32(k, 0x80000020UL);
    if (V->V2) { RfsPut64(k + 8, Id); return; }
    RfsPut64(k + 8, Dir);
    RfsPut64(k + 16, Id);
}

/* the reverse row: value {0, u16 0x0C, u16 bytes, name} */
static RFS_STATUS
V1RevRow(PRFS_TABLE D, ULONGLONG Dir, ULONGLONG Id, const USHORT *Name, ULONG Len)
{
    UCHAR k[24];
    PUCHAR v = (PUCHAR)RfsHostAlloc(12 + 2 * RFS_MAX_NAME);
    ULONG i;
    RFS_STATUS st;
    if (!v) return RFS_NO_MEMORY;
    if (Len > RFS_MAX_NAME) Len = RFS_MAX_NAME;
    V1RevKey(D->Vol, Dir, Id, k);
    RfsZero(v, 12);
    RfsPut16(v + 8, 0x0C);
    RfsPut16(v + 10, (USHORT)(Len * 2));
    for (i = 0; i < Len; i++) RfsPut16(v + 12 + 2 * i, Name[i]);
    st = RfsTreeInsert(D, k, 24, v, 12 + 2 * Len, 0, FALSE);
    RfsHostFree(v);
    return st;
}

static RFS_STATUS
TouchDirV1(PRFS_VOL V, PRFS_TABLE D)
{
    RFS_CURSOR c;
    UCHAR key[4];
    PUCHAR val;
    RFS_STATUS st;
    ULONGLONG now = RfsHostNowNt();
    RfsPut32(key, REFS_KEY_DIRSELF);
    st = RfsLookup(D, key, 4, &c);
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st == RFS_NOT_FOUND ? RFS_OK : st; }
    st = RfsTreeModifyAt(&c, &val);
    if (RFS_SUCCESS(st) && RfsCurRow(&c)->ValLen >= REFS_IR_DATA + 0x20) {
        RfsPut64(val + REFS_IR_DATA + 0x08, now);    /* write time */
        RfsPut64(val + REFS_IR_DATA + 0x10, now);    /* metadata change time */
    }
    RfsCursorClose(&c);
    return st;
}

RFS_STATUS
RfsCreateV1(PRFS_VOL V, ULONGLONG DirOid, const USHORT *Name, ULONG Len, BOOLEAN Dir, ULONG Attr, PRFS_DIRENT Ent)
{
    PRFS_TABLE D;
    PUCHAR key;
    ULONG kl;
    RFS_STATUS st;
    ULONGLONG now = RfsHostNowNt();
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (Len == 0 || Len > RFS_MAX_NAME) return RFS_BAD_NAME;
    st = RfsTableOpenObject(V, DirOid, &D);
    if (!RFS_SUCCESS(st)) return st;
    /* the lookup result lands in Ent (rebuilt below): no second RFS_DIRENT on the stack */
    if (RFS_SUCCESS(RfsDirLookup(V, DirOid, Name, Len, Ent))) { RfsTableClose(V, D); return RFS_EXISTS; }
    key = (PUCHAR)RfsHostAlloc(RFS_MAX_KEY);
    if (!key) { RfsTableClose(V, D); return RFS_NO_MEMORY; }
    RfsZero(Ent, sizeof(*Ent));
    Ent->Times.Create = Ent->Times.Write = Ent->Times.Change = Ent->Times.Access = now;
    { ULONG i; for (i = 0; i < Len; i++) Ent->Name[i] = Name[i]; Ent->NameLen = Len; }
    if (!Dir) {
        ULONGLONG id = 0;
        PUCHAR val; ULONG vl;
        if (!Attr) Attr = 0x20;
        st = V1NextId(V, DirOid, &id);
        if (RFS_SUCCESS(st)) st = V1FileRec(V, DirOid, id, Attr & ~0x10UL, &Ent->Times, &val, &vl);
        if (RFS_SUCCESS(st)) {
            kl = V1NameKey(Name, Len, REFS1_ENT_FILE, key);
            st = RfsTreeInsert(D, key, kl, val, vl, V1_RF_EMBEDDED, FALSE);
            RfsHostFree(val);
        }
        if (RFS_SUCCESS(st)) st = V1RevRow(D, DirOid, id, Name, Len);
        Ent->Type = RFS_FT_FILE; Ent->KeyFlags = REFS1_ENT_FILE; Ent->Home = DirOid; Ent->FileId = id;
        Ent->Attributes = Attr & ~0x10UL;
    } else {
        ULONGLONG oid = 0;
        PRFS_TABLE N = NULL;
        PUCHAR rec = NULL; ULONG rl;
        UCHAR ev[72];
        st = V1NewOid(V, &oid);
        if (RFS_SUCCESS(st)) st = V1ObjRow(V, oid, 1, TRUE);
        if (RFS_SUCCESS(st)) st = V1DirSelf(V, oid, 1, 0x10, &Ent->Times, &rec, &rl, DirOid, Name, Len);
        if (RFS_SUCCESS(st)) {
            UCHAR desc[REFS_IR_DATA + 0x40];
            ULONG dl = V->V2 ? REFS_IR_DATA + 0x40 : REFS_IR_DATA;
            RfsZero(desc, sizeof(desc));
            RfsPut16(desc + REFS_IR_SIZE, 0x28);
            RfsPut16(desc + REFS_IR_KIND, 1);
            RfsPut32(desc + REFS_IR_ONE, 1);
            RfsPut32(desc + REFS_IR_TWO, 2);
            st = RfsNewTable(V, RFS_TROOT_OBJECT, oid, REFS_SCHEMA_DIR, desc, dl, &N);
            if (RFS_SUCCESS(st)) {
                UCHAR k4[4];
                if (!V->V2) RfsPut64(N->Root->PageHdr + REFS1_PH_TABLE, oid);
                RfsPut32(k4, REFS_KEY_DIRSELF);
                st = RfsTreeInsert(N, k4, 4, rec, rl, V1_RF_EMBEDDED, FALSE);
                RfsTableClose(V, N);
            }
        }
        RfsHostFree(rec);
        if (RFS_SUCCESS(st)) st = V1ParentChild(V, DirOid, oid, TRUE);
        if (RFS_SUCCESS(st)) {
            kl = V1NameKey(Name, Len, REFS1_ENT_DIR, key);
            V1SubdirEntry(V, oid, &Ent->Times, Attr & ~0x10UL, ev);
            st = RfsTreeInsert(D, key, kl, ev, 72, 0, FALSE);
        }
        Ent->Type = RFS_FT_DIR; Ent->KeyFlags = REFS1_ENT_DIR; Ent->Oid = oid; Ent->Home = DirOid;
        Ent->Attributes = (Attr & ~0x10UL) | 0x10;
    }
    if (RFS_SUCCESS(st)) st = TouchDirV1(V, D);
    RfsTableClose(V, D);
    RfsHostFree(key);
    return st;
}

/* ------------------------------------------------------------------ */
/* File data (write)                                                   */
/* ------------------------------------------------------------------ */

/* file data space: 1.x from its own Medium allocator (blocks); 2.x the 3.x
 * Medium allocator in clusters, recorded by virtual cluster number */
static RFS_STATUS
DataAlloc(PRFS_VOL V, ULONG Want, PULONGLONG Lcn, PULONG Got)
{
    if (V->V2) {
        ULONGLONG p;
        RFS_STATUS st = RfsAlloc(V, RFS_TIER_MEDIUM, RFS_FOR_DATA, 1, Want, 1, &p, Got);
        if (!RFS_SUCCESS(st)) return st;
        st = RfsToVirtual(V, p, Lcn);
        if (!RFS_SUCCESS(st)) (VOID)RfsFree(V, RFS_TIER_MEDIUM, p, *Got);
        return st;
    }
    return RfsAllocV1Data(V, Want, Lcn, Got);
}

static RFS_STATUS
DataFree(PRFS_VOL V, ULONGLONG Lcn, ULONG Count)
{
    return V->V2 ? RfsFreeData(V, Lcn, Count) : RfsFreeV1Data(V, Lcn, Count);
}

static RFS_STATUS
ExtAdd1(PRFS_FILE F, ULONGLONG Vcn, ULONGLONG Lcn, ULONG Run)
{
    if (F->NExt && F->Ext[F->NExt - 1].Vcn + F->Ext[F->NExt - 1].Run == Vcn &&
        F->Ext[F->NExt - 1].Lcn + F->Ext[F->NExt - 1].Run == Lcn) {
        F->Ext[F->NExt - 1].Run += Run;
        return RFS_OK;
    }
    if (F->NExt == F->CapExt) {
        ULONG cap = F->CapExt ? F->CapExt * 2 : 8;
        PRFS_EXTENT n = (PRFS_EXTENT)RfsHostAlloc(cap * sizeof(RFS_EXTENT));
        if (!n) return RFS_NO_MEMORY;
        if (F->Ext) { RfsCopy(n, F->Ext, F->NExt * sizeof(RFS_EXTENT)); RfsHostFree(F->Ext); }
        F->Ext = n; F->CapExt = cap;
    }
    F->Ext[F->NExt].Vcn = Vcn;
    F->Ext[F->NExt].Lcn = Lcn;
    F->Ext[F->NExt].Run = Run;
    F->Ext[F->NExt].Flags = 0;
    F->NExt++;
    return RFS_OK;
}

/* grow the file's allocation to cover at least NeedBlocks blocks */
static RFS_STATUS
GrowAlloc(PRFS_VOL V, PRFS_FILE F, ULONGLONG NeedBlocks)
{
    ULONGLONG have = F->Alloc >> DUSH(V);
    while (have < NeedBlocks) {
        ULONGLONG blk;
        ULONG got, want = (ULONG)(NeedBlocks - have);
        RFS_STATUS st;
        if (want > 0x4000) want = 0x4000;
        st = DataAlloc(V, want, &blk, &got);
        if (!RFS_SUCCESS(st)) return st;
        st = ExtAdd1(F, have, blk, got);
        if (!RFS_SUCCESS(st)) { DataFree(V, blk, got); return st; }
        have += got;
        F->Alloc = have << DUSH(V);
        F->ExtChanged = TRUE;
    }
    return RFS_OK;
}

/* blocks the extents cover (end of the last one), in 16 KiB block units */
static ULONGLONG
ExtEnd1(PRFS_FILE F)
{
    return F->NExt ? F->Ext[F->NExt - 1].Vcn + F->Ext[F->NExt - 1].Run : 0;
}

/* zero file blocks [From, To) on disk (holes within the valid range read zero) */
static RFS_STATUS
ZeroBlocksV1(PRFS_VOL V, PRFS_FILE F, ULONGLONG From, ULONGLONG To)
{
    UCHAR *z;
    RFS_STATUS st = RFS_OK;
    if (From >= To) return RFS_OK;
    z = (UCHAR *)RfsHostAlloc(DUSZ(V));
    if (!z) return RFS_NO_MEMORY;
    RfsZero(z, DUSZ(V));
    while (From < To) {
        ULONGLONG plcn;
        ULONG run, i;
        st = RfsFileMap(V, F, From, &plcn, &run);
        if (!RFS_SUCCESS(st)) break;
        if (!run) run = 1;
        if ((ULONGLONG)run > To - From) run = (ULONG)(To - From);
        if (!plcn) { From += run; continue; }       /* a hole: leave it */
        for (i = 0; i < run; i++) {
            st = RfsHostWrite(V, (plcn + i) << DUSH(V), DUSZ(V), z);
            if (!RFS_SUCCESS(st)) break;
        }
        if (!RFS_SUCCESS(st)) break;
        From += run;
    }
    RfsHostFree(z);
    return st;
}

/* free whole clusters past NeedBlocks (rounded up to a cluster) */
static RFS_STATUS
FreeTailV1(PRFS_VOL V, PRFS_FILE F, ULONGLONG NeedBlocks)
{
    ULONG cpb = DUCPB(V);
    ULONGLONG keepb = ((NeedBlocks + cpb - 1) / cpb) * cpb;
    RFS_STATUS st = RFS_OK;
    while (F->NExt) {
        PRFS_EXTENT e = &F->Ext[F->NExt - 1];
        if (e->Vcn >= keepb) {
            if (e->Lcn) { st = DataFree(V, e->Lcn, e->Run); if (!RFS_SUCCESS(st)) return st; }
            F->NExt--; F->ExtChanged = TRUE; continue;
        }
        if (e->Vcn + e->Run <= keepb) break;
        {
            ULONG keep = (ULONG)(keepb - e->Vcn);           /* a multiple of cpb */
            if (e->Lcn) { st = DataFree(V, e->Lcn + keep, e->Run - keep); if (!RFS_SUCCESS(st)) return st; }
            e->Run = keep; F->ExtChanged = TRUE;
        }
        break;
    }
    F->Alloc = ExtEnd1(F) << DUSH(V);
    return st;
}

/* grow/shrink F's allocation to hold Size bytes (driver layer) */
RFS_STATUS
RfsFileSetSizeV1(PRFS_VOL V, PRFS_FILE F, ULONGLONG Size, BOOLEAN KeepAlloc)
{
    ULONGLONG need = (Size + DUSZ(V) - 1) >> DUSH(V), have;
    RFS_STATUS st = RFS_OK;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (F->Type != RFS_FT_FILE) { F->Size = Size; F->Changed = TRUE; return RFS_OK; }
    have = ExtEnd1(F);
    if (need > have) {
        st = GrowAlloc(V, F, need);
        if (RFS_SUCCESS(st)) st = ZeroBlocksV1(V, F, have, ExtEnd1(F));
        if (!RFS_SUCCESS(st)) { F->Changed = TRUE; return st; }
    } else if (need < have && !KeepAlloc) {
        st = FreeTailV1(V, F, need);
        if (!RFS_SUCCESS(st)) return st;
    }
    F->Size = Size;
    if (F->ValidSize > Size) F->ValidSize = Size;
    F->Alloc = ExtEnd1(F) << DUSH(V);
    F->Changed = F->ExtChanged = TRUE;
    return RFS_OK;
}

/* allocate blocks [end, Blocks); zero the new ones except [Keep, Keep+KeepCount) */
RFS_STATUS
RfsFileAllocToV1(PRFS_VOL V, PRFS_FILE F, ULONGLONG Blocks, ULONGLONG Keep, ULONGLONG KeepCount)
{
    ULONGLONG have, nb, ka, kb;
    RFS_STATUS st;
    if (V->ReadOnly) return RFS_READ_ONLY;
    have = ExtEnd1(F);
    if (Blocks <= have) return RFS_OK;
    st = GrowAlloc(V, F, Blocks);
    if (!RFS_SUCCESS(st)) return st;
    F->Changed = TRUE;
    F->Alloc = ExtEnd1(F) << DUSH(V);
    nb = ExtEnd1(F);
    ka = Keep; kb = Keep + KeepCount;
    if (ka < have) ka = have;
    if (kb > nb) kb = nb;
    if (ka >= kb) return ZeroBlocksV1(V, F, have, nb);  /* keep range empty: zero all new */
    if (have < ka) { st = ZeroBlocksV1(V, F, have, ka); if (!RFS_SUCCESS(st)) return st; }
    if (kb < nb)   { st = ZeroBlocksV1(V, F, kb, nb);   if (!RFS_SUCCESS(st)) return st; }
    return RFS_OK;
}

/* free blocks from Blocks on (the size is the caller's) */
RFS_STATUS
RfsFileTruncToV1(PRFS_VOL V, PRFS_FILE F, ULONGLONG Blocks)
{
    if (V->ReadOnly) return RFS_READ_ONLY;
    return FreeTailV1(V, F, Blocks);
}

RFS_STATUS
RfsFileWriteV1(PRFS_VOL V, PRFS_FILE F, ULONGLONG Off, ULONG Len, const VOID *Buf)
{
    const UCHAR *in = (const UCHAR *)Buf;
    ULONG done = 0;
    RFS_STATUS st;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (F->Type != RFS_FT_FILE) return RFS_IS_A_DIRECTORY;
    if (F->Integrity) return RFS_NOT_SUPPORTED;      /* integrity streams: not written */
    if (!Len) return RFS_OK;
    st = GrowAlloc(V, F, (Off + Len + DUSZ(V) - 1) >> DUSH(V));
    if (!RFS_SUCCESS(st)) return st;
    while (done < Len) {
        ULONGLONG pos = Off + done, vcn = pos >> DUSH(V), plcn;
        ULONG within = (ULONG)(pos & (DUSZ(V) - 1)), run, n;
        st = RfsFileMap(V, F, vcn, &plcn, &run);
        if (!RFS_SUCCESS(st)) return st;
        if (!plcn) return RFS_CORRUPT;
        n = Len - done;
        if ((ULONGLONG)run * DUSZ(V) - within < n) n = (ULONG)((ULONGLONG)run * DUSZ(V) - within);
        if (within || (n & (DUSZ(V) - 1))) {
            /* partial block: read-modify-write one block at a time */
            UCHAR *blk = (UCHAR *)RfsHostAlloc(DUSZ(V));
            ULONG bn = n > DUSZ(V) - within ? DUSZ(V) - within : n;
            if (!blk) return RFS_NO_MEMORY;
            RfsZero(blk, DUSZ(V));
            if (pos < F->ValidSize) (VOID)RfsHostRead(V, plcn << DUSH(V), DUSZ(V), blk);
            RfsCopy(blk + within, in + done, bn);
            st = RfsHostWrite(V, plcn << DUSH(V), DUSZ(V), blk);
            RfsHostFree(blk);
            if (!RFS_SUCCESS(st)) return st;
            n = bn;
        } else {
            st = RfsHostWrite(V, plcn << DUSH(V), n, in + done);
            if (!RFS_SUCCESS(st)) return st;
        }
        done += n;
    }
    if (Off + Len > F->Size) { F->Size = Off + Len; F->ExtChanged = TRUE; }
    if (Off + Len > F->ValidSize) F->ValidSize = Off + Len;
    F->Changed = TRUE;
    return RFS_OK;
}

/* rebuild the file's embedded record (header + $DATA stream) and replace the name row */
static RFS_STATUS
FileSaveV1In(PRFS_VOL V, PRFS_FILE F, PUCHAR key)
{
    PRFS_TABLE D, S;
    RFS_CURSOR c;
    UCHAR desc[REFS_IR_DATA + 0x80], sk[16];
    PUCHAR h, sv = NULL, dv = NULL;
    ULONG kl, sl, dl, i;
    RFS_STATUS st;
    if (!F->Changed && !F->ExtChanged) return RFS_OK;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (F->Type != RFS_FT_FILE) return RFS_OK;
    /* build the file record header */
    RfsZero(desc, sizeof(desc));
    RfsPut32(desc, REFS_IR_DATA + 0x80);
    RfsPut16(desc + REFS_IR_SIZE, 0x28);
    RfsPut16(desc + REFS_IR_KIND, 1);
    RfsPut32(desc + REFS_IR_ONE, 1);
    RfsPut32(desc + REFS_IR_SCHEMA, 0x110);
    RfsPut32(desc + REFS_IR_SCHEMA2, 0x110);
    RfsPut32(desc + REFS_IR_TWO, 2);
    h = desc + REFS_IR_DATA;
    RecHeader(V, h, &F->Times, F->Attributes ? F->Attributes : 0x20, F->Home, F->FileId,
              (ULONG)F->SecurityId ? (ULONG)F->SecurityId : DEF_SECHASH, F->Size, F->Alloc, FALSE);
    st = RfsNewTable(V, RFS_TROOT_EMBEDDED, F->Home, 0x110, desc, REFS_IR_DATA + 0x80, &D);
    if (!RFS_SUCCESS(st)) return st;
    /* the record's other attribute rows stay as they are */
    {
        PRFS_TABLE P, O;
        RFS_CURSOR pc, oc;
        if (RFS_SUCCESS(RfsTableOpenObject(V, F->Home, &P))) {
            kl = V1NameKey(F->Name, F->NameLen, REFS1_ENT_FILE, key);
            if (RFS_SUCCESS(RfsLookup(P, key, kl, &pc))) {
                PRFS_ROW nr = RfsCurRow(&pc);
                if ((nr->Flags & REFS1_RF_EMBEDDED) && RFS_SUCCESS(RfsTableOpenEmbedded(V, F->Home, nr->Val, nr->ValLen, &O))) {
                    RFS_STATUS s2 = RfsSeek(O, NULL, 0, &oc);
                    while (RFS_SUCCESS(s2) && RFS_SUCCESS(st)) {
                        PRFS_ROW a = RfsCurRow(&oc);
                        BOOLEAN data = (BOOLEAN)(a->KeyLen >= 12 && a->KeyLen <= 14 && RfsGet32(a->Key + 8) == REFS_AT_DATA);
                        if (!data) st = RfsTreeInsert(D, a->Key, a->KeyLen, a->Val, a->ValLen, a->Flags, FALSE);
                        s2 = RfsNext(&oc);
                    }
                    RfsCursorClose(&oc);
                    RfsTableClose(V, O);
                }
            }
            RfsCursorClose(&pc);
            RfsTableClose(V, P);
        }
        if (!RFS_SUCCESS(st)) { RfsNodeFree(D->Root); RfsHostFree(D); return st; }
    }
    /* the $DATA stream (only if there is data) */
    if (F->NExt && F->Size) {
        UCHAR sdesc[REFS_IR_DATA + 0x40];
        PUCHAR sh;
        RfsZero(sdesc, sizeof(sdesc));
        RfsPut32(sdesc, REFS_IR_DATA + 0x40);
        RfsPut16(sdesc + REFS_IR_SIZE, 0x28);
        RfsPut16(sdesc + REFS_IR_KIND, 1);
        RfsPut32(sdesc + REFS_IR_ONE, 1);
        RfsPut32(sdesc + REFS_IR_SCHEMA, 0x120);
        RfsPut32(sdesc + REFS_IR_SCHEMA2, 0x120);
        RfsPut32(sdesc + REFS_IR_TWO, 2);
        sh = sdesc + REFS_IR_DATA;
        RfsPut32(sh + 0x00, 1);
        RfsPut64(sh + 0x0C, F->Alloc);
        RfsPut64(sh + 0x14, F->Size);
        RfsPut64(sh + 0x1C, F->ValidSize);
        st = RfsNewTable(V, RFS_TROOT_EMBEDDED, 0, 0x120, sdesc, REFS_IR_DATA + 0x40, &S);
        if (!RFS_SUCCESS(st)) { RfsNodeFree(D->Root); RfsHostFree(D); return st; }
        for (i = 0; i < F->NExt && RFS_SUCCESS(st); i++) {
            UCHAR ev[0x20], ek[16];
            RfsZero(ev, sizeof(ev));
            RfsPut64(ev + REFS1_EXT_VCN, F->Ext[i].Vcn);
            RfsPut64(ev + REFS1_EXT_COUNT, F->Ext[i].Run);
            RfsPut64(ev + REFS1_EXT_LCN, F->Ext[i].Lcn);
            RfsCopy(ek, ev, 16);
            st = RfsTreeInsert(S, ek, 16, ev, 0x20, 0, FALSE);
        }
        if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(S, &sv, &sl);
        RfsNodeFree(S->Root); RfsHostFree(S);
        if (RFS_SUCCESS(st)) {
            RfsZero(sk, 16);
            RfsPut64(sk, (ULONGLONG)sl);         /* key: the value size, then type 0x80 */
            RfsPut32(sk + 8, REFS_AT_DATA);
            st = RfsTreeInsert(D, sk, 14, sv, sl, V1_RF_EMBEDDED, FALSE);
        }
        RfsHostFree(sv);
    }
    if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(D, &dv, &dl);
    RfsNodeFree(D->Root); RfsHostFree(D);
    if (!RFS_SUCCESS(st)) return st;
    /* replace the file's name row in the parent */
    st = RfsTableOpenObject(V, F->Home, &D);
    if (RFS_SUCCESS(st)) {
        kl = V1NameKey(F->Name, F->NameLen, REFS1_ENT_FILE, key);
        st = RfsSeekLeaf(D, key, kl, &c);
        if (RFS_SUCCESS(st)) st = RfsTreeReplaceAt(&c, key, kl, dv, dl, V1_RF_EMBEDDED);
        else RfsCursorClose(&c);
        RfsTableClose(V, D);
    }
    RfsHostFree(dv);
    if (RFS_SUCCESS(st)) { F->Changed = F->ExtChanged = FALSE; }
    return st;
}

RFS_STATUS
RfsFileSaveV1(PRFS_VOL V, PRFS_FILE F)
{
    PUCHAR key;
    RFS_STATUS st;
    if (!F->Changed && !F->ExtChanged) return RFS_OK;
    key = (PUCHAR)RfsHostAlloc(RFS_MAX_KEY);     /* off the stack: NT 3.x kernel stacks are 12K */
    if (!key) return RFS_NO_MEMORY;
    st = FileSaveV1In(V, F, key);
    RfsHostFree(key);
    return st;
}

/* ------------------------------------------------------------------ */
/* Removing files and directories                                      */
/* ------------------------------------------------------------------ */

static RFS_STATUS
RemoveV1In(PRFS_VOL V, ULONGLONG DirOid, const USHORT *Name, ULONG Len, PRFS_DIRENT pe, PUCHAR key)
{
    PRFS_TABLE D;
    UCHAR rk[24];
    ULONG kl;
    RFS_STATUS st;
    if (V->ReadOnly) return RFS_READ_ONLY;
    st = RfsDirLookup(V, DirOid, Name, Len, pe);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsTableOpenObject(V, DirOid, &D);
    if (!RFS_SUCCESS(st)) return st;
    if (pe->Type == RFS_FT_DIR) {
        /* must be empty (only the self record) */
        PRFS_TABLE C;
        RFS_CURSOR cc;
        BOOLEAN empty = TRUE;
        st = RfsTableOpenObject(V, pe->Oid, &C);
        if (RFS_SUCCESS(st)) {
            st = RfsSeek(C, NULL, 0, &cc);
            while (RFS_SUCCESS(st)) {
                PRFS_ROW r = RfsCurRow(&cc);
                if (!(r->KeyLen == 4 && RfsGet16(r->Key) == REFS_KEY_DIRSELF)) { empty = FALSE; break; }
                st = RfsNext(&cc);
            }
            if (RFS_SUCCESS(st) || st == RFS_NO_MORE) RfsCursorClose(&cc);
            if (empty) {
                if (V->V2) (VOID)RfsTreeFreeAll(C);
                else if (C->Root->HasRef) (VOID)RfsFreePageV1(V, &C->Root->Ref);
                RfsTableForget(V, C);
            }
            RfsTableClose(V, C);
        }
        if (!empty) { RfsTableClose(V, D); return RFS_NOT_EMPTY; }
        st = V1ParentChild(V, DirOid, pe->Oid, FALSE);
        if (RFS_SUCCESS(st)) st = V1ObjRow(V, pe->Oid, 0, FALSE);
        if (RFS_SUCCESS(st)) {
            kl = V1NameKey(Name, Len, REFS1_ENT_DIR, key);
            st = RfsTreeDelete(D, key, kl);
        }
    } else {
        /* free the file's data clusters, then delete its rows */
        PRFS_FILE F;
        if (RFS_SUCCESS(RfsFileOpen(V, DirOid, pe, &F))) {
            ULONG i;
            for (i = 0; i < F->NExt; i++)
                if (F->Ext[i].Lcn && !(F->Ext[i].Flags & REFS_EXTF_SPARSE))
                    (VOID)DataFree(V, F->Ext[i].Lcn, F->Ext[i].Run);
            RfsFileClose(F);
        }
        kl = V1NameKey(Name, Len, REFS1_ENT_FILE, key);
        st = RfsTreeDelete(D, key, kl);
        if (RFS_SUCCESS(st)) {
            V1RevKey(V, DirOid, pe->FileId, rk);
            st = RfsTreeDelete(D, rk, 24);
            if (st == RFS_NOT_FOUND) st = RFS_OK;
        }
    }
    if (RFS_SUCCESS(st)) st = TouchDirV1(V, D);
    RfsTableClose(V, D);
    return st;
}

typedef struct { RFS_DIRENT E; UCHAR Key[RFS_MAX_KEY]; } V1_RMSCR;

RFS_STATUS
RfsRemoveV1(PRFS_VOL V, ULONGLONG DirOid, const USHORT *Name, ULONG Len)
{
    V1_RMSCR *x;
    RFS_STATUS st;
    if (V->ReadOnly) return RFS_READ_ONLY;
    x = (V1_RMSCR *)RfsHostAlloc(sizeof(*x));
    if (!x) return RFS_NO_MEMORY;
    st = RemoveV1In(V, DirOid, Name, Len, &x->E, x->Key);
    RfsHostFree(x);
    return st;
}

/* ------------------------------------------------------------------ */
/* Rename / move                                                       */
/* ------------------------------------------------------------------ */
/* file-record header fields, as offsets into the embedded row value */
#define V1FH_CHTIME  (REFS_IR_DATA + 0x10)
#define V1FH_HOME    (REFS_IR_DATA + 0x28)
#define V1FH_FILEID  (REFS_IR_DATA + 0x30)

/* copy a name row's value+flags out of table T (caller frees *Val) */
static RFS_STATUS
CopyNameRow(PRFS_TABLE T, const UCHAR *Key, ULONG Kl, PUCHAR *Val, PULONG Vl, PUSHORT Flags)
{
    RFS_CURSOR c;
    RFS_STATUS st = RfsLookup(T, Key, Kl, &c);
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st; }
    {
        PRFS_ROW r = RfsCurRow(&c);
        PUCHAR v = (PUCHAR)RfsHostAlloc(r->ValLen ? r->ValLen : 1);
        if (!v) { RfsCursorClose(&c); return RFS_NO_MEMORY; }
        RfsCopy(v, r->Val, r->ValLen);
        *Val = v; *Vl = r->ValLen; *Flags = r->Flags;
    }
    RfsCursorClose(&c);
    return RFS_OK;
}

/* a moved or renamed directory's link attribute: the new parent and name */
static RFS_STATUS
V1UpdateLink(PRFS_VOL V, ULONGLONG Oid, ULONGLONG Parent, const USHORT *Name, ULONG Len)
{
    PRFS_TABLE N, E;
    RFS_CURSOR c, c2;
    UCHAR k4[4];
    PUCHAR nv;
    ULONG nl;
    USHORT fl;
    RFS_TIMES tm;
    RFS_STATUS st = RfsTableOpenObject(V, Oid, &N);
    if (!RFS_SUCCESS(st)) return st;
    RfsPut32(k4, REFS_KEY_DIRSELF);
    st = RfsLookup(N, k4, 4, &c);
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); RfsTableClose(V, N); return st == RFS_NOT_FOUND ? RFS_OK : st; }
    fl = RfsCurRow(&c)->Flags;
    st = RfsTableOpenEmbedded(V, Oid, RfsCurRow(&c)->Val, RfsCurRow(&c)->ValLen, &E);
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); RfsTableClose(V, N); return st; }
    RfsZero(&tm, sizeof(tm));
    if (E->Root->Desc && E->Root->DescLen >= REFS_IR_DATA + 0x20) {
        const UCHAR *h = E->Root->Desc + REFS_IR_DATA;
        tm.Create = RfsGet64(h); tm.Write = RfsGet64(h + 8); tm.Change = RfsGet64(h + 0x10); tm.Access = RfsGet64(h + 0x18);
    }
    /* the old link row(s) go */
    for (;;) {
        st = RfsSeek(E, NULL, 0, &c2);
        while (RFS_SUCCESS(st) && !(RfsCurRow(&c2)->KeyLen >= 12 && RfsGet32(RfsCurRow(&c2)->Key + 8) == V1_AT_LINK)) st = RfsNext(&c2);
        if (!RFS_SUCCESS(st)) { RfsCursorClose(&c2); break; }
        st = RfsTreeDeleteAt(&c2);
        if (!RFS_SUCCESS(st)) break;
    }
    if (st == RFS_NO_MORE || st == RFS_NOT_FOUND) st = RFS_OK;
    if (RFS_SUCCESS(st)) st = V1InsertLink(E, Parent, &tm, Name, Len);
    if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(E, &nv, &nl);
    RfsTableClose(V, E);
    if (RFS_SUCCESS(st)) { st = RfsTreeUpdateAt(&c, nv, nl, fl); RfsHostFree(nv); }
    else RfsCursorClose(&c);
    RfsTableClose(V, N);
    return st;
}

typedef struct { RFS_DIRENT E, Te; UCHAR Fk[RFS_MAX_KEY], Tk[RFS_MAX_KEY]; } V1_RNSCR;

static RFS_STATUS
RenameV1In(V1_RNSCR *x, PRFS_VOL V, ULONGLONG FromDir, const USHORT *From, ULONG FromLen,
            ULONGLONG ToDir, const USHORT *To, ULONG ToLen, BOOLEAN Replace)
{
    PRFS_TABLE S = NULL, D = NULL;
    PUCHAR fk = x->Fk, tk = x->Tk;
    UCHAR rk[24];
    ULONG fkl, tkl;
    PUCHAR val = NULL;
    ULONG vl;
    USHORT flags;
    ULONGLONG now = RfsHostNowNt();
    BOOLEAN same = (BOOLEAN)(FromDir == ToDir);
    RFS_STATUS st;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (FromLen == 0 || FromLen > RFS_MAX_NAME || ToLen == 0 || ToLen > RFS_MAX_NAME) return RFS_BAD_NAME;
    st = RfsDirLookup(V, FromDir, From, FromLen, &x->E);
    if (!RFS_SUCCESS(st)) return st;
    /* destination already present? */
    st = RfsDirLookup(V, ToDir, To, ToLen, &x->Te);
    if (RFS_SUCCESS(st)) {
        BOOLEAN itself = (BOOLEAN)(same &&
            ((x->E.Type == RFS_FT_DIR && x->Te.Type == RFS_FT_DIR && x->E.Oid == x->Te.Oid) ||
             (x->E.Type == RFS_FT_FILE && x->Te.Type == RFS_FT_FILE && x->E.FileId == x->Te.FileId)));
        if (!itself) {
            if (x->Te.Type == RFS_FT_DIR) return RFS_IS_A_DIRECTORY;
            if (!Replace) return RFS_EXISTS;
            st = RfsRemoveV1(V, ToDir, To, ToLen);
            if (!RFS_SUCCESS(st)) return st;
        }
    } else if (st != RFS_NOT_FOUND) {
        return st;
    }
    st = RfsTableOpenObject(V, FromDir, &S);
    if (!RFS_SUCCESS(st)) return st;
    if (same) D = S;
    else { st = RfsTableOpenObject(V, ToDir, &D); if (!RFS_SUCCESS(st)) { RfsTableClose(V, S); return st; } }

    if (x->E.Type == RFS_FT_FILE) {
        ULONGLONG id = x->E.FileId, nid;
        fkl = V1NameKey(From, FromLen, REFS1_ENT_FILE, fk);
        st = CopyNameRow(S, fk, fkl, &val, &vl, &flags);
        if (!RFS_SUCCESS(st)) goto out;
        nid = id;
        /* drop the old name row and reverse row */
        st = RfsTreeDelete(S, fk, fkl);
        if (RFS_SUCCESS(st)) {
            V1RevKey(V, FromDir, id, rk);
            st = RfsTreeDelete(S, rk, 24);
            if (st == RFS_NOT_FOUND) st = RFS_OK;
        }
        if (RFS_SUCCESS(st) && !same) {
            st = V1NextId(V, ToDir, &nid);
            if (V->V2) {
                if (RFS_SUCCESS(st) && vl >= REFS_FH_NEXTID + 8) RfsPut64(val + REFS_FH_NEXTID, nid);
            } else {
                if (RFS_SUCCESS(st) && vl >= V1FH_HOME + 8)  RfsPut64(val + V1FH_HOME, ToDir);
                if (RFS_SUCCESS(st) && vl >= V1FH_FILEID + 8) RfsPut64(val + V1FH_FILEID, nid);
            }
        }
        if (RFS_SUCCESS(st) && vl >= V1FH_CHTIME + 8) RfsPut64(val + V1FH_CHTIME, now);
        if (RFS_SUCCESS(st)) {
            tkl = V1NameKey(To, ToLen, REFS1_ENT_FILE, tk);
            st = RfsTreeInsert(D, tk, tkl, val, vl, flags, FALSE);
        }
        if (RFS_SUCCESS(st)) st = V1RevRow(D, ToDir, nid, To, ToLen);
    } else {
        /* directory: move the 72-byte subdir entry, re-link parent/child */
        fkl = V1NameKey(From, FromLen, REFS1_ENT_DIR, fk);
        st = CopyNameRow(S, fk, fkl, &val, &vl, &flags);
        if (!RFS_SUCCESS(st)) goto out;
        st = RfsTreeDelete(S, fk, fkl);
        if (RFS_SUCCESS(st) && !same) {
            st = V1ParentChild(V, FromDir, x->E.Oid, FALSE);
            if (RFS_SUCCESS(st)) st = V1ParentChild(V, ToDir, x->E.Oid, TRUE);
        }
        if (RFS_SUCCESS(st)) {
            tkl = V1NameKey(To, ToLen, REFS1_ENT_DIR, tk);
            st = RfsTreeInsert(D, tk, tkl, val, vl, flags, FALSE);
        }
        if (RFS_SUCCESS(st)) st = V1UpdateLink(V, x->E.Oid, ToDir, To, ToLen);
    }
    if (RFS_SUCCESS(st)) st = TouchDirV1(V, S);
    if (RFS_SUCCESS(st) && !same) st = TouchDirV1(V, D);
out:
    if (val) RfsHostFree(val);
    if (!same && D) RfsTableClose(V, D);
    RfsTableClose(V, S);
    return st;
}

RFS_STATUS
RfsRenameV1(PRFS_VOL V, ULONGLONG FromDir, const USHORT *From, ULONG FromLen,
            ULONGLONG ToDir, const USHORT *To, ULONG ToLen, BOOLEAN Replace)
{
    V1_RNSCR *x;
    RFS_STATUS st;
    if (V->ReadOnly) return RFS_READ_ONLY;
    x = (V1_RNSCR *)RfsHostAlloc(sizeof(*x));
    if (!x) return RFS_NO_MEMORY;
    st = RenameV1In(x, V, FromDir, From, FromLen, ToDir, To, ToLen, Replace);
    RfsHostFree(x);
    return st;
}

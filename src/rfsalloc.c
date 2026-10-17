/*
 * REFSNT - cluster allocation (ReFS 3.x).
 *
 * Three allocator tables (schema 0xE010) share one row format:
 *   key {u64 start, u64 length} (physical clusters; the value starts with it)
 *   value: +0x10 u16 free clusters (saturates at 0xFFFF), +0x12 u16 flags
 *   (bit 0: a bitmap follows), +0x14 u8 bitmap offset (0x18 or 0), +0x15 u8
 *   format, +0x16 u16 allocated-count hint (0xFFFF: the whole range),
 *   +0x18 the bitmap, one bit per cluster (1: allocated).
 *   A row without a bitmap is uniform: free 0 = all allocated, else all free.
 * The root's header data keeps a summary: +0x08 clusters covered, +0x10
 * clusters not allocated.
 *
 *   Medium (root 1)     covers every container; data and most metadata.
 *                       Containers owned by the other tiers are "allocated" here.
 *   Container (root 2)  the pages of tables 1, 2, 6 and 11.
 *   Small (root 12)     the container tables' pages; never changed here.
 *
 * Rows are kept container sized: a uniform row spanning several containers
 * is cut into a bitmap row for the container being changed and uniform rows
 * for the rest.
 *
 * Clusters freed before a commit stay unusable (the quarantine) until the
 * checkpoint that no longer references them has been written: the previous
 * checkpoint must stay intact on disk.
 */
#include "rfseng.h"
#ifdef RFS_TRACE
#include <stdio.h>
#define TRACE(x) printf x
#else
#define TRACE(x)
#endif

#define AR_FREE         0x10
#define AR_FLAGS        0x12
#define AR_BMOFF        0x14
#define AR_FORMAT       0x15
#define AR_HINT         0x16
#define AR_BITMAP       0x18
#define ARF_BITMAP      0x0001

#define SUM_COVERED     0x08
#define SUM_FREE        0x10

static const ULONG TierRoot[RFS_NTIERS] = { REFS_ROOT_MEDIUM, REFS_ROOT_CONTALLOC, REFS_ROOT_SMALL };

/* the allocator a table's pages come from */
UCHAR
RfsTierOf(PRFS_VOL V, PRFS_TABLE T)
{
    (VOID)V;
    if (T->Where == RFS_TROOT_CHECKPOINT) {
        switch (T->Index) {
        case REFS_ROOT_MEDIUM: case REFS_ROOT_CONTALLOC: case REFS_ROOT_REFCOUNT: case REFS_ROOT_INTEGRITY:
            return RFS_TIER_CONTAINER;
        case REFS_ROOT_CONTAINER: case REFS_ROOT_CONTAINER2: case REFS_ROOT_SMALL:
            return RFS_TIER_SMALL;
        }
    }
    return RFS_TIER_MEDIUM;
}

/* ---- containers ------------------------------------------------------------------------------------ */
static ULONG
ContOfPhys(PRFS_VOL V, ULONGLONG Plcn)
{
    ULONGLONG i = Plcn / V->ClustersPerContainer;
    if (i >= V->NPhysConts) return 0xFFFFFFFFUL;
    return V->PhysToCont[i];
}

RFS_STATUS
RfsToVirtual(PRFS_VOL V, ULONGLONG Plcn, PULONGLONG Vlcn)
{
    ULONG c = ContOfPhys(V, Plcn);
    if (c == 0xFFFFFFFFUL || c >= V->NConts) return RFS_CORRUPT;
    *Vlcn = ((ULONGLONG)c << V->ContainerShift) + (Plcn - V->ContPhys[c]);
    return RFS_OK;
}

/* how welcome an allocation is in a container: 0 best .. 3 last resort, 4 never */
static ULONG
RankMedium(PRFS_VOL V, ULONGLONG Plcn, UCHAR For)
{
    ULONG c = ContOfPhys(V, Plcn), f;
    if (c == 0xFFFFFFFFUL || !V->ContFlags) return 4;
    /* 2.x container rows carry no tier/placement flags (just 0/1): every container is plain */
    f = V->V2 ? 0 : (V->ContFlags[c] | ((ULONG)V->ContFlags[c + V->NConts] << 8));
    if (f & 0x0200) return 4;                               /* another tier's */
    {
        ULONG r;
        if (For == RFS_FOR_META) {
            if ((f & 0x4001) == 0x4001) r = 0;
            else if (f == 0x4000 || f == 0) r = 1;
            else if (f & 0x2040) r = 2;
            else r = 3;
        } else {
            if (f & 0x0040) r = 0;
            else if (f & 0x2000 || f == 0x4000 || f == 0) r = 1;
            else if ((f & 0x4001) == 0x4001) r = 2;
            else return 4;
        }
        /* the boot view's window: last */
        if (Plcn >= V->AvoidLo && Plcn < V->AvoidHi && r < 3) r = 3;
        return r;
    }
}

/* placed allocations (the boot view): any container but another tier's */
static BOOLEAN
Placeable(PRFS_VOL V, ULONGLONG Plcn)
{
    ULONG c = ContOfPhys(V, Plcn);
    if (c == 0xFFFFFFFFUL || !V->ContFlags) return FALSE;
    return (BOOLEAN)!(V->ContFlags[c + V->NConts] & 0x02);
}

/* the other tiers' rows only cover their own containers */
#define Rank(V, P, For, Tier)   ((Tier) == RFS_TIER_MEDIUM ? RankMedium(V, P, For) : 0)

/* ---- the quarantine ---------------------------------------------------------------------------------- */
static RFS_STATUS
QuarAdd(PRFS_VOL V, ULONGLONG Start, ULONGLONG Count)
{
    if (V->NQuar && V->Quarantine[V->NQuar - 1].Start + V->Quarantine[V->NQuar - 1].Count == Start) {
        V->Quarantine[V->NQuar - 1].Count += Count;
        return RFS_OK;
    }
    if (V->NQuar == V->CapQuar) {
        ULONG cap = V->CapQuar ? V->CapQuar * 2 : 64;
        PRFS_RUN n = (PRFS_RUN)RfsHostAlloc(cap * sizeof(RFS_RUN));
        if (!n) return RFS_NO_MEMORY;
        if (V->Quarantine) { RfsCopy(n, V->Quarantine, V->NQuar * sizeof(RFS_RUN)); RfsHostFree(V->Quarantine); }
        V->Quarantine = n;
        V->CapQuar = cap;
    }
    V->Quarantine[V->NQuar].Start = Start;
    V->Quarantine[V->NQuar].Count = Count;
    V->NQuar++;
    return RFS_OK;
}

/* the end of a quarantined run overlapping [Start, Start+Count), or 0 */
static ULONGLONG
QuarHit(PRFS_VOL V, ULONGLONG Start, ULONGLONG Count)
{
    ULONG i;
    ULONGLONG end = 0;
    for (i = 0; i < V->NQuar; i++) {
        PRFS_RUN q = &V->Quarantine[i];
        if (q->Start < Start + Count && Start < q->Start + q->Count && q->Start + q->Count > end) end = q->Start + q->Count;
    }
    return end;
}

VOID
RfsQuarantineClear(PRFS_VOL V)
{
    V->NQuar = 0;
}

/* ---- rows ------------------------------------------------------------------------------------------- */
static BOOLEAN BitGet(const UCHAR *B, ULONG I) { return (BOOLEAN)((B[I >> 3] >> (I & 7)) & 1); }
static VOID BitSet(PUCHAR B, ULONG I) { B[I >> 3] |= (UCHAR)(1 << (I & 7)); }
static VOID BitClr(PUCHAR B, ULONG I) { B[I >> 3] &= (UCHAR)~(1 << (I & 7)); }

static ULONG
RowFree(const UCHAR *V, ULONG Len)
{
    ULONGLONG len = RfsGet64(V + 8);
    if (!(RfsGet16(V + AR_FLAGS) & ARF_BITMAP)) {
        if (RfsGet16(V + AR_FREE) == 0) return 0;
        return len > 0xFFFFFFFFUL ? 0xFFFFFFFFUL : (ULONG)len;
    }
    {
        ULONG n = 0, i, bo = V[AR_BMOFF] ? V[AR_BMOFF] : AR_BITMAP;
        if (bo + (len + 7) / 8 > Len) return 0;
        for (i = 0; i < (ULONG)len; i++) if (!BitGet(V + bo, i)) n++;
        return n;
    }
}

/* a row value: Bitmap (or uniform: Allocated) */
static ULONG
MakeRow(PRFS_VOL V, UCHAR Tier, PUCHAR Out, ULONGLONG Start, ULONGLONG Len, BOOLEAN Bitmap, BOOLEAN Allocated)
{
    ULONG n = AR_BITMAP;
    RfsZero(Out, AR_BITMAP);
    RfsPut64(Out, Start);
    RfsPut64(Out + 8, Len);
    Out[AR_FORMAT] = V->AllocFormat[Tier];
    if (Bitmap) {
        ULONG bytes = (ULONG)(((Len + 7) / 8 + 7) & ~7ULL);
        RfsZero(Out + AR_BITMAP, bytes);
        if (Allocated) {
            ULONG i;
            for (i = 0; i < (ULONG)Len; i++) BitSet(Out + AR_BITMAP, i);
        }
        RfsPut16(Out + AR_FREE, (USHORT)(Allocated ? 0 : (Len > 0xFFFF ? 0xFFFF : Len)));
        RfsPut16(Out + AR_FLAGS, ARF_BITMAP);
        Out[AR_BMOFF] = AR_BITMAP;
        RfsPut16(Out + AR_HINT, (USHORT)(Allocated ? 0xFFFF : 0));
        n += bytes;
    } else {
        RfsPut16(Out + AR_FREE, (USHORT)(Allocated ? 0 : (Len > 0xFFFF ? 0xFFFF : Len)));
        RfsPut16(Out + AR_FLAGS, 0x0002);
        RfsPut16(Out + AR_HINT, (USHORT)(Allocated ? 0xFFFF : 0));
    }
    return n;
}

static RFS_STATUS
InsertRow(PRFS_TABLE T, const UCHAR *Row, ULONG Len)
{
    return RfsTreeInsert(T, Row, 16, Row, Len, 0, FALSE);
}

/*
 * The uniform row at the cursor (spanning [S, S+L)) becomes a bitmap row
 * for the container holding Plcn, with uniform rows for the rest. The cursor
 * is closed.
 */
static RFS_STATUS
SplitUniform(PRFS_VOL V, UCHAR Tier, PRFS_CURSOR C, ULONGLONG Plcn)
{
    PRFS_TABLE T = C->Table;
    PRFS_ROW r = RfsCurRow(C);
    ULONGLONG s = RfsGet64(r->Val), l = RfsGet64(r->Val + 8), cpc = V->ClustersPerContainer;
    ULONGLONG cs = Plcn - (Plcn - s) % cpc, ce;
    BOOLEAN alloc = (BOOLEAN)(RfsGet16(r->Val + AR_FREE) == 0);
    PUCHAR buf;
    RFS_STATUS st;
    if (cs < s) cs = s;
    ce = cs + cpc;
    if (ce > s + l) ce = s + l;
    buf = (PUCHAR)RfsHostAlloc(AR_BITMAP + (ULONG)(cpc / 8) + 16);
    if (!buf) { RfsCursorClose(C); return RFS_NO_MEMORY; }
    st = RfsTreeDeleteAt(C);
    if (RFS_SUCCESS(st) && cs > s) st = InsertRow(T, buf, MakeRow(V, Tier, buf, s, cs - s, FALSE, alloc));
    if (RFS_SUCCESS(st)) st = InsertRow(T, buf, MakeRow(V, Tier, buf, cs, ce - cs, TRUE, alloc));
    if (RFS_SUCCESS(st) && ce < s + l) st = InsertRow(T, buf, MakeRow(V, Tier, buf, ce, s + l - ce, FALSE, alloc));
    RfsHostFree(buf);
    return st;
}

static VOID
Summary(PRFS_TABLE T, LONGLONG Delta)
{
    PRFS_NODE r = T->Root;
    if (!r->Desc || r->DescLen < REFS_IR_DATA + RFS_SUM_FREE(T->Vol) + 8) return;
    RfsPut64(r->Desc + REFS_IR_DATA + RFS_SUM_FREE(T->Vol), RfsGet64(r->Desc + REFS_IR_DATA + RFS_SUM_FREE(T->Vol)) + (ULONGLONG)Delta);
    r->Dirty = TRUE;
}

static VOID
SetCounts(PUCHAR Val, LONG Delta)
{
    ULONG f = RfsGet16(Val + AR_FREE);
    ULONGLONG len = RfsGet64(Val + 8);
    LONG n = (LONG)f + Delta;
    ULONG used;
    if (f == 0xFFFF && len > 0xFFFF) n = 0xFFFF;                /* saturated: leave it */
    if (n < 0) n = 0;
    if (n > 0xFFFF) n = 0xFFFF;
    RfsPut16(Val + AR_FREE, (USHORT)n);
    used = (ULONG)(len - (ULONG)n);
    RfsPut16(Val + AR_HINT, (USHORT)(n == 0 ? 0xFFFF : (used > 0xFFFE ? 0xFFFE : used)));
}

/* ---- allocating ------------------------------------------------------------------------------------- */
/*
 * In the bitmap row at the cursor, a free run of at least Min clusters
 * (up to Count) starting at a multiple of Align, at or after From, in a
 * container of rank Want: it is taken. *Got 0: none.
 */
#define WANT_ANY    0xFF        /* any container this tier may use (a placed allocation) */

static RFS_STATUS
TakeInRow(PRFS_VOL V, UCHAR Tier, UCHAR For, ULONG Want, PRFS_CURSOR C, ULONGLONG From, ULONGLONG Until,
          ULONG Min, ULONG Count, ULONG Align, PULONGLONG Plcn, PULONG Got)
{
    PRFS_TABLE T = C->Table;
    PRFS_ROW r = RfsCurRow(C);
    ULONGLONG s = RfsGet64(r->Val), l = RfsGet64(r->Val + 8);
    ULONG bo = r->Val[AR_BMOFF] ? r->Val[AR_BMOFF] : AR_BITMAP, i, n;
    PUCHAR val;
    RFS_STATUS st;
    *Got = 0;
    if (bo + (l + 7) / 8 > r->ValLen || l > 0x100000) return RFS_OK;
    i = From > s ? (ULONG)(From - s) : 0;
    if (Until < s + l) l = Until > s ? Until - s : 0;
    while (i < (ULONG)l) {
        ULONGLONG p = s + i, q;
        ULONG j;
        if (Align > 1 && p % Align) { i += (ULONG)(Align - p % Align); continue; }
        if (BitGet(r->Val + bo, i)) { i++; continue; }
        if (Want == WANT_ANY ? !Placeable(V, p) : Rank(V, p, For, Tier) != Want) {
            /* the rest of this container */
            ULONGLONG ce = p - (p - s) % V->ClustersPerContainer + V->ClustersPerContainer;
            i = (ULONG)(ce - s);
            continue;
        }
        for (j = i; j < (ULONG)l && j - i < Count && !BitGet(r->Val + bo, j); j++) ;
        n = j - i;
        if (n >= Min && (q = QuarHit(V, p, n)) != 0) {
            /* up to the quarantined part, or past it */
            ULONGLONG qs = q, k;
            for (k = 0; k < V->NQuar; k++) {
                PRFS_RUN qr = &V->Quarantine[k];
                if (qr->Start < p + n && p < qr->Start + qr->Count && qr->Start < qs) qs = qr->Start;
            }
            if (qs > p && qs - p >= Min) n = (ULONG)(qs - p);
            else { i = (ULONG)(q - s); continue; }
        }
        if (n < Min) { i = j + 1; continue; }
        if (Align > 1) n -= n % Align;
        if (n < Min || n == 0) { i = j + 1; continue; }
        /* take [i, i+n) */
        st = RfsTreeModifyAt(C, &val);
        if (!RFS_SUCCESS(st)) return st;
        for (j = i; j < i + n; j++) BitSet(val + bo, j);
        SetCounts(val, -(LONG)n);
        Summary(T, -(LONGLONG)n);
        *Plcn = p;
        *Got = n;
        return RFS_OK;
    }
    return RFS_OK;
}

RFS_STATUS
RfsAlloc(PRFS_VOL V, UCHAR Tier, UCHAR For, ULONG Min, ULONG Count, ULONG Align, PULONGLONG Plcn, PULONG Got)
{
    PRFS_TABLE T;
    RFS_STATUS st;
    ULONG want, pass;
    *Got = 0;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (Tier >= RFS_TIER_SMALL) return RFS_NOT_SUPPORTED;
    if (Min == 0) Min = 1;
    if (Count < Min) Count = Min;
    st = RfsGlobal(V, TierRoot[Tier], &T);
    if (!RFS_SUCCESS(st)) return st;
    for (want = 0; want < 4; want++) {
        for (pass = 0; pass < 2; pass++) {
            ULONGLONG from = pass == 0 ? V->Hint[Tier][For] : 0;
            ULONGLONG stop = pass == 0 ? ~0ULL : V->Hint[Tier][For];
            UCHAR key[16];
            RFS_CURSOR c;
            if (pass == 1 && from == stop) break;
            RfsPut64(key, from);
            RfsPut64(key + 8, 1);
            st = RfsSeek(T, key, 16, &c);
            while (RFS_SUCCESS(st)) {
                PRFS_ROW r = RfsCurRow(&c);
                ULONGLONG s, l;
                if (r->ValLen < AR_BITMAP) { st = RfsNext(&c); continue; }
                s = RfsGet64(r->Val);
                l = RfsGet64(r->Val + 8);
                if (s >= stop) break;
                if (RfsGet16(r->Val + AR_FLAGS) & ARF_BITMAP) {
                    if (RfsGet16(r->Val + AR_FREE) != 0) {
                        st = TakeInRow(V, Tier, For, want, &c, from, ~0ULL, Min, Count, Align, Plcn, Got);
                        if (!RFS_SUCCESS(st) || *Got) break;
                    }
                } else if (RfsGet16(r->Val + AR_FREE) != 0) {
                    /* uniform free: the first suitable container in it gets a bitmap row */
                    ULONGLONG p = s > from ? s : from, cpc = V->ClustersPerContainer;
                    BOOLEAN found = FALSE;
                    p -= (p - s) % cpc;
                    for (; p < s + l; p += cpc) {
                        if (Rank(V, p, For, Tier) == want && !(QuarHit(V, p, cpc) && QuarHit(V, p, cpc) >= p + cpc)) { found = TRUE; break; }
                    }
                    if (found) {
                        st = SplitUniform(V, Tier, &c, p);
                        if (!RFS_SUCCESS(st)) return st;
                        /* again, from the new row */
                        RfsPut64(key, p);
                        st = RfsSeek(T, key, 16, &c);
                        continue;
                    }
                }
                st = RfsNext(&c);
            }
            RfsCursorClose(&c);
            if (st != RFS_NO_MORE && !RFS_SUCCESS(st)) return st;
            if (*Got) {
                V->Hint[Tier][For] = *Plcn + *Got;
                V->AllocChanged = TRUE;
                if (Tier == RFS_TIER_MEDIUM && V->FreeClusters >= *Got) V->FreeClusters -= *Got;
                return RFS_OK;
            }
        }
    }
    TRACE(("alloc failed: tier %u for %u min %lu count %lu align %lu\n", Tier, For, (unsigned long)Min, (unsigned long)Count, (unsigned long)Align));
    return RFS_DISK_FULL;
}

/*
 * Clusters inside [Lo, Hi) (physical): a run of Min..Count starting at a
 * multiple of Align. For the boot view, which needs its files near the start.
 */
RFS_STATUS
RfsAllocIn(PRFS_VOL V, ULONGLONG Lo, ULONGLONG Hi, ULONG Min, ULONG Count, ULONG Align, PULONGLONG Plcn, PULONG Got)
{
    PRFS_TABLE T;
    RFS_STATUS st;
    UCHAR key[16];
    RFS_CURSOR c;
    *Got = 0;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (Min == 0) Min = 1;
    if (Count < Min) Count = Min;
    st = RfsGlobal(V, REFS_ROOT_MEDIUM, &T);
    if (!RFS_SUCCESS(st)) return st;
    RfsPut64(key, Lo);
    RfsPut64(key + 8, 1);
    st = RfsSeek(T, key, 16, &c);
    while (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        ULONGLONG s;
        if (r->ValLen < AR_BITMAP) { st = RfsNext(&c); continue; }
        s = RfsGet64(r->Val);
        if (s >= Hi) break;
        if (RfsGet16(r->Val + AR_FLAGS) & ARF_BITMAP) {
            if (RfsGet16(r->Val + AR_FREE) != 0) {
                st = TakeInRow(V, RFS_TIER_MEDIUM, RFS_FOR_DATA, WANT_ANY, &c, Lo, Hi, Min, Count, Align, Plcn, Got);
                if (!RFS_SUCCESS(st) || *Got) break;
            }
        } else if (RfsGet16(r->Val + AR_FREE) != 0) {
            ULONGLONG p = s > Lo ? s : Lo;
            if (Placeable(V, p)) {
                st = SplitUniform(V, RFS_TIER_MEDIUM, &c, p);
                if (!RFS_SUCCESS(st)) return st;
                RfsPut64(key, p);
                st = RfsSeek(T, key, 16, &c);
                continue;
            }
        }
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    if (st != RFS_NO_MORE && !RFS_SUCCESS(st)) return st;
    if (!*Got) return RFS_DISK_FULL;
    V->AllocChanged = TRUE;
    if (V->FreeClusters >= *Got) V->FreeClusters -= *Got;
    return RFS_OK;
}

/* is the physical cluster free (and usable) in the Medium allocator? */
BOOLEAN
RfsClusterFree(PRFS_VOL V, ULONGLONG Plcn)
{
    PRFS_TABLE T;
    UCHAR key[16];
    RFS_CURSOR c;
    BOOLEAN f = FALSE;
    if (!RFS_SUCCESS(RfsGlobal(V, REFS_ROOT_MEDIUM, &T))) return FALSE;
    RfsPut64(key, Plcn);
    RfsPut64(key + 8, 1);
    if (RFS_SUCCESS(RfsLookup(T, key, 16, &c))) {
        PRFS_ROW r = RfsCurRow(&c);
        ULONGLONG s = RfsGet64(r->Val);
        if (RfsGet16(r->Val + AR_FLAGS) & ARF_BITMAP) {
            ULONG bo = r->Val[AR_BMOFF] ? r->Val[AR_BMOFF] : AR_BITMAP;
            f = (BOOLEAN)!BitGet(r->Val + bo, (ULONG)(Plcn - s));
        } else {
            f = (BOOLEAN)(RfsGet16(r->Val + AR_FREE) != 0);
        }
        if (f && QuarHit(V, Plcn, 1)) f = FALSE;
    }
    RfsCursorClose(&c);
    return f;
}

/* ---- freeing ----------------------------------------------------------------------------------------- */
RFS_STATUS
RfsFree(PRFS_VOL V, UCHAR Tier, ULONGLONG Plcn, ULONGLONG Count)
{
    PRFS_TABLE T;
    RFS_STATUS st;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (Tier >= RFS_TIER_SMALL) return RFS_NOT_SUPPORTED;
    st = RfsGlobal(V, TierRoot[Tier], &T);
    if (!RFS_SUCCESS(st)) return st;
    while (Count) {
        UCHAR key[16];
        RFS_CURSOR c;
        PRFS_ROW r;
        ULONGLONG s, l, n;
        RfsPut64(key, Plcn);
        RfsPut64(key + 8, 1);
        st = RfsLookup(T, key, 16, &c);
        if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st == RFS_NOT_FOUND ? RFS_CORRUPT : st; }
        r = RfsCurRow(&c);
        s = RfsGet64(r->Val);
        l = RfsGet64(r->Val + 8);
        if (!(RfsGet16(r->Val + AR_FLAGS) & ARF_BITMAP)) {
            if (RfsGet16(r->Val + AR_FREE) != 0) { RfsCursorClose(&c); return RFS_CORRUPT; }    /* already free */
            st = SplitUniform(V, Tier, &c, Plcn);
            if (!RFS_SUCCESS(st)) return st;
            continue;
        }
        n = s + l - Plcn;
        if (n > Count) n = Count;
        {
            PUCHAR val;
            ULONG bo = r->Val[AR_BMOFF] ? r->Val[AR_BMOFF] : AR_BITMAP, i, first = (ULONG)(Plcn - s);
            if (bo + (l + 7) / 8 > r->ValLen) { RfsCursorClose(&c); return RFS_CORRUPT; }
            for (i = first; i < first + (ULONG)n; i++) {
                if (!BitGet(r->Val + bo, i)) { RfsCursorClose(&c); return RFS_CORRUPT; }     /* not allocated */
            }
            st = RfsTreeModifyAt(&c, &val);
            if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st; }
            for (i = first; i < first + (ULONG)n; i++) BitClr(val + bo, i);
            SetCounts(val, (LONG)n);
            Summary(T, (LONGLONG)n);
        }
        RfsCursorClose(&c);
        st = QuarAdd(V, Plcn, n);
        if (!RFS_SUCCESS(st)) return st;
        if (Tier == RFS_TIER_MEDIUM) V->FreeClusters += n;
        V->AllocChanged = TRUE;
        Plcn += n;
        Count -= n;
    }
    return RFS_OK;
}

/*
 * Data clusters (virtual): a cluster the block-refcount table counts
 * references for is shared (block clones, snapshots): its count drops, it
 * stays allocated.
 */
static RFS_STATUS
Unshare(PRFS_VOL V, ULONGLONG Vlcn, ULONGLONG Count, PULONGLONG Plain)
{
    PRFS_TABLE T;
    UCHAR key[16];
    RFS_CURSOR c;
    RFS_STATUS st;
    PRFS_ROW r;
    ULONGLONG s, l;
    *Plain = Count;
    st = RfsGlobal(V, REFS_ROOT_REFCOUNT, &T);
    if (!RFS_SUCCESS(st)) return RFS_OK;                    /* no table: nothing shared */
    if (!T->Root->NRows) return RFS_OK;
    RfsPut64(key, Vlcn);
    RfsPut64(key + 8, 1);
    st = RfsLookup(T, key, 16, &c);
    if (st == RFS_NOT_FOUND) {
        /* plain up to the next row */
        RfsCursorClose(&c);
        st = RfsSeek(T, key, 16, &c);
        if (RFS_SUCCESS(st)) {
            s = RfsGet64(RfsCurRow(&c)->Key);
            if (s > Vlcn && s - Vlcn < Count) *Plain = s - Vlcn;
        }
        RfsCursorClose(&c);
        return RFS_OK;
    }
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st; }
    r = RfsCurRow(&c);
    s = RfsGet64(r->Val);
    l = RfsGet64(r->Val + 8);
    if (r->ValLen < 0x1C + 2 * l || l > 0x10000) { RfsCursorClose(&c); return RFS_OK; }   /* compact form: treat as plain */
    {
        ULONG i = (ULONG)(Vlcn - s);
        USHORT e = RfsGet16(r->Val + 0x1C + 2 * i);
        ULONG n = 1;
        BOOLEAN shared = (BOOLEAN)((e & 0x3FFF) != 0);
        /* a run of equal sharing */
        while (n < Count && i + n < l && (((RfsGet16(r->Val + 0x1C + 2 * (i + n)) & 0x3FFF) != 0) == shared)) n++;
        if (!shared) { *Plain = n; RfsCursorClose(&c); return RFS_OK; }
        {
            PUCHAR val;
            ULONG k, total;
            st = RfsTreeModifyAt(&c, &val);
            if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st; }
            total = RfsGet32(val + 0x18);
            for (k = i; k < i + n; k++) {
                USHORT x = RfsGet16(val + 0x1C + 2 * k);
                RfsPut16(val + 0x1C + 2 * k, (USHORT)((x & 0xC000) | ((x & 0x3FFF) - 1)));
                if (total) total--;
            }
            RfsPut32(val + 0x18, total);
        }
        RfsCursorClose(&c);
        *Plain = 0;
        return (RFS_STATUS)n;                           /* (positive: that many shared clusters handled) */
    }
}

RFS_STATUS
RfsFreeData(PRFS_VOL V, ULONGLONG Vlcn, ULONGLONG Count)
{
    while (Count) {
        ULONGLONG plain, plcn;
        ULONG run;
        RFS_STATUS st = Unshare(V, Vlcn, Count, &plain);
        if (!RFS_SUCCESS(st)) return st;
        if (st > 0) { Vlcn += (ULONG)st; Count -= (ULONG)st; continue; }
        while (plain) {
            ULONGLONG n;
            st = RfsTranslate(V, Vlcn, &plcn, &run);
            if (!RFS_SUCCESS(st)) return st;
            n = run < plain ? run : plain;
            st = RfsFree(V, RFS_TIER_MEDIUM, plcn, n);
            if (!RFS_SUCCESS(st)) return st;
            Vlcn += n; Count -= n; plain -= n;
        }
    }
    return RFS_OK;
}

/* ---- pages ------------------------------------------------------------------------------------------- */
RFS_STATUS
RfsAllocPage(PRFS_VOL V, UCHAR Tier, BOOLEAN Real, PRFS_REF Ref)
{
    ULONGLONG p;
    ULONG got, i, n = V->ClustersPerPage;
    RFS_STATUS st = RfsAlloc(V, Tier, RFS_FOR_META, n, n, n, &p, &got);
    if (!RFS_SUCCESS(st)) return st;
    RfsZero(Ref, sizeof(*Ref));
    /* 2.x keeps the allocator tables in the self region and refers to them
     * physically (container 0 is identity), as Windows does */
    if (V->V2 && Tier == RFS_TIER_CONTAINER) Real = TRUE;
    for (i = 0; i < n; i++) {
        if (Real) Ref->Lcn[i] = p + i;
        else {
            st = RfsToVirtual(V, p + i, &Ref->Lcn[i]);
            if (!RFS_SUCCESS(st)) return st;
        }
    }
    return RFS_OK;
}

RFS_STATUS
RfsFreePage(PRFS_VOL V, UCHAR Tier, BOOLEAN Real, const RFS_REF *Ref)
{
    ULONG i;
    for (i = 0; i < V->ClustersPerPage; i++) {
        ULONGLONG p;
        ULONG run;
        RFS_STATUS st;
        if (Real) p = Ref->Lcn[i];
        else {
            st = RfsTranslate(V, Ref->Lcn[i], &p, &run);
            if (!RFS_SUCCESS(st)) return st;
        }
        st = RfsFree(V, Tier, p, 1);
        if (!RFS_SUCCESS(st)) return st;
    }
    return RFS_OK;
}

/* ---- setup ------------------------------------------------------------------------------------------- */
RFS_STATUS
RfsAllocInit(PRFS_VOL V)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    RFS_STATUS st;
    ULONG i, t;
    V->NPhysConts = (ULONG)(V->TotalClusters / V->ClustersPerContainer + 1);
    V->PhysToCont = (PULONG)RfsHostAlloc(V->NPhysConts * sizeof(ULONG));
    V->ContFlags = (PUCHAR)RfsAllocZero(2 * V->NConts + 2);
    if (!V->PhysToCont || !V->ContFlags) return RFS_NO_MEMORY;
    for (i = 0; i < V->NPhysConts; i++) V->PhysToCont[i] = 0xFFFFFFFFUL;
    for (i = 0; i < V->NConts; i++) {
        if (V->ContPhys[i] != ~0ULL && V->ContPhys[i] / V->ClustersPerContainer < V->NPhysConts)
            V->PhysToCont[V->ContPhys[i] / V->ClustersPerContainer] = i;
    }
    /* container flags (the low and high bytes kept apart) */
    st = RfsTableOpenRoot(V, REFS_ROOT_CONTAINER, &T);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsSeek(T, NULL, 0, &c);
    while (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        if (r->KeyLen >= 8 && r->ValLen >= 0x18) {
            ULONGLONG id = RfsGet64(r->Key);
            ULONG f = RfsGet32(r->Val + 0x14);
            if (id < V->NConts) {
                V->ContFlags[id] = (UCHAR)f;
                V->ContFlags[id + V->NConts] = (UCHAR)(f >> 8);
            }
        }
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    RfsTableClose(V, T);
    if (st != RFS_NO_MORE) return st;
    /* each tier's row format byte and the free count */
    for (t = 0; t < RFS_TIER_SMALL; t++) {
        V->AllocFormat[t] = (UCHAR)(t == RFS_TIER_MEDIUM && V->Minor >= 7 ? 2 : 1);
        st = RfsGlobal(V, TierRoot[t], &T);
        if (!RFS_SUCCESS(st)) return st;
        st = RfsSeek(T, NULL, 0, &c);
        while (RFS_SUCCESS(st)) {
            PRFS_ROW r = RfsCurRow(&c);
            if (r->ValLen >= AR_BITMAP && (RfsGet16(r->Val + AR_FLAGS) & ARF_BITMAP)) { V->AllocFormat[t] = r->Val[AR_FORMAT]; break; }
            st = RfsNext(&c);
        }
        RfsCursorClose(&c);
        if (t == RFS_TIER_MEDIUM && T->Root->Desc && T->Root->DescLen >= REFS_IR_DATA + RFS_SUM_FREE(V) + 8)
            V->FreeClusters = RfsGet64(T->Root->Desc + REFS_IR_DATA + RFS_SUM_FREE(V));
    }
    return RFS_OK;
}

VOID
RfsAllocDone(PRFS_VOL V)
{
    if (V->PhysToCont) RfsHostFree(V->PhysToCont);
    if (V->ContFlags) RfsHostFree(V->ContFlags);
    if (V->Quarantine) RfsHostFree(V->Quarantine);
    V->PhysToCont = NULL;
    V->ContFlags = NULL;
    V->Quarantine = NULL;
    V->NQuar = V->CapQuar = 0;
}

ULONGLONG
RfsFreeCount(PRFS_VOL V)
{
    return V->FreeClusters;
}

/* (a row's free count from its bitmap, for the checker) */
ULONG RfsAllocRowFree(const UCHAR *Val, ULONG Len) { return RowFree(Val, Len); }

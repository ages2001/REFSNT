/*
 * REFSNT - the ReFS 2.0 and 3.14 formatter, from templates.
 *
 * ReFS 2.x and 3.x have too much in them that only Windows' own format
 * defines (the log, the schema, the system tables, their header data) to be
 * made up here, so refsfmt starts from volumes Windows formatted: their
 * first K containers (tools/rfstmpl.c turned them into fmt/rfstmpl.h). What
 * depends on the size is then made for the new volume:
 *
 *   - the boot sector (sectors, serial number, GUID) and its copy in the
 *     last sector, the backup superblocks in the last container;
 *   - the container tables (both copies): the rows of the containers past
 *     the template's first K go, rows for the new volume's containers come
 *     in, cloned from the template's, the tail container (backup superblocks)
 *     moves to the new end;
 *   - the Medium allocator: its rows past the first K containers are
 *     replaced by one free range and the tail container's row; the summary
 *     (clusters covered, free, allocated) follows;
 *   - root 11 (3.x), a single row covering the volume;
 *   - the volume label.
 *
 * All of that goes through the engine (copy-on-write, as any change does):
 * two commits, so both checkpoints describe the new volume. The log is
 * zeroed except for the pages the template carries.
 */
#include "rfseng.h"
#include "rfsmkfs.h"
#include "rfstmpl.h"

ULONG RfsAllocRowFree(const UCHAR *Val, ULONG Len);

/* ---- the templates ------------------------------------------------------------------------ */
typedef struct {
    UCHAR       Major;
    ULONG       ClusterSize;
    const UCHAR *Data;
    ULONG       Size;
} TMPL;

static const TMPL Templates[] = {
    { 3, 4096,  RfsTmpl_3_4K,  sizeof(RfsTmpl_3_4K) },
    { 3, 65536, RfsTmpl_3_64K, sizeof(RfsTmpl_3_64K) },
    { 2, 4096,  RfsTmpl_2_4K,  sizeof(RfsTmpl_2_4K) },
    { 2, 65536, RfsTmpl_2_64K, sizeof(RfsTmpl_2_64K) },
};

typedef struct {
    UCHAR       Major, Minor;
    ULONG       ClusterSize, Cpc, K, OldN, NZero, NRuns, RawLen, CompLen;
    const UCHAR *Zero;              /* NZero x {u64 start, u64 count} */
    const UCHAR *Runs;              /* NRuns x {u32 start, u32 count} */
    const UCHAR *Comp;
} THDR;

static BOOLEAN
ParseTemplate(const UCHAR *P, ULONG Size, THDR *H)
{
    if (Size < 40 || RfsCmp(P, "RFT1", 4) != 0) return FALSE;
    H->Major = P[4];
    H->Minor = P[5];
    H->ClusterSize = RfsGet32(P + 8);
    H->Cpc = RfsGet32(P + 12);
    H->K = RfsGet32(P + 16);
    H->OldN = RfsGet32(P + 20);
    H->NZero = RfsGet32(P + 24);
    H->NRuns = RfsGet32(P + 28);
    H->RawLen = RfsGet32(P + 32);
    H->CompLen = RfsGet32(P + 36);
    H->Zero = P + 40;
    H->Runs = H->Zero + 16 * H->NZero;
    H->Comp = H->Runs + 8 * H->NRuns;
    return (BOOLEAN)(H->Comp + H->CompLen == P + Size && H->K >= 1 && H->OldN > H->K && H->Cpc);
}

static BOOLEAN
Expand(const UCHAR *In, ULONG InLen, PUCHAR Out, ULONG OutLen)
{
    ULONG i = 0, o = 0;
    while (i < InLen) {
        UCHAR c = In[i++];
        if (c < 0x80) {
            ULONG n = (ULONG)c + 1;
            if (i + n > InLen || o + n > OutLen) return FALSE;
            RfsCopy(Out + o, In + i, n);
            i += n; o += n;
        } else {
            ULONG n, d;
            if (c == 0xFF) {
                if (i + 2 > InLen) return FALSE;
                n = (ULONG)(In[i] | (In[i + 1] << 8)) + 130;
                i += 2;
            } else {
                n = (ULONG)c - 0x80 + 3;
            }
            if (i + 2 > InLen) return FALSE;
            d = (ULONG)(In[i] | (In[i + 1] << 8));
            i += 2;
            if (!d || d > o || o + n > OutLen) return FALSE;
            while (n--) { Out[o] = Out[o - d]; o++; }
        }
    }
    return (BOOLEAN)(o == OutLen);
}

BOOLEAN
RfsFormatQuery(UCHAR Major, ULONG ClusterSize, PRFS_FMT_LIMITS Limits)
{
    ULONG i;
    THDR h;
    for (i = 0; i < sizeof(Templates) / sizeof(Templates[0]); i++) {
        if (Templates[i].Major != Major || Templates[i].ClusterSize != ClusterSize) continue;
        if (!ParseTemplate(Templates[i].Data, Templates[i].Size, &h)) return FALSE;
        if (Limits) {
            Limits->Minor = h.Minor;
            Limits->ContainerBytes = (ULONGLONG)h.Cpc * h.ClusterSize;
            Limits->MinBytes = (ULONGLONG)(h.K + 1) * h.Cpc * h.ClusterSize;
        }
        return TRUE;
    }
    return FALSE;
}

/* ---- writing the start ------------------------------------------------------------------- */
#define ZCHUNK  (1UL << 20)

static RFS_STATUS
WriteZero(PRFS_VOL V, ULONGLONG Off, ULONGLONG Len, PUCHAR Z, RFS_FMT_PROGRESS Progress, PVOID Ctx)
{
    ULONGLONG done = 0;
    while (done < Len) {
        ULONG n = (Len - done) > ZCHUNK ? ZCHUNK : (ULONG)(Len - done);
        RFS_STATUS st = RfsHostWrite(V, Off + done, n, Z);
        if (!RFS_SUCCESS(st)) return st;
        done += n;
        if (Progress && !(done & ((16UL << 20) - 1))) Progress(Ctx, (ULONG)(done * 99 / Len));
    }
    return RFS_OK;
}

/* a self-checksummed page (superblock): its own cluster in the header and the descriptor */
static VOID
SelfSum(PUCHAR Page, ULONG Span, ULONGLONG Lcn)
{
    ULONG so = RfsGet32(Page + REFS3_SB_SELF_OFF), sl = RfsGet32(Page + REFS3_SB_SELF_LEN);
    UCHAR d[104], ck[32];
    ULONG n;
    RfsPut64(Page + REFS3_PH_LCN, Lcn);
    if (so + sl > Span || sl > sizeof(d) || sl < 0x28) return;
    RfsCopy(d, Page + so, sl);
    RfsPut64(d + REFS3_REF_LCN, Lcn);
    RfsZero(Page + so, sl);
    n = RfsChecksum(d[REFS3_REF_CKTYPE], Page, Span, ck);
    if (n && 0x28 + n <= sl) RfsCopy(d + REFS3_REF_CK, ck, n);
    RfsCopy(Page + so, d, sl);
}

/* ---- resizing, through the engine ---------------------------------------------------------- */
typedef struct {
    UCHAR       Key[16];
    UCHAR       Val[0x1000];         /* (a 4K-cluster allocator row: 0x818) */
    ULONG       Len;
    USHORT      Flags;
    BOOLEAN     Have;
} SAVED;

static VOID
Save(SAVED *S, PRFS_ROW R)
{
    ULONG n = R->ValLen > sizeof(S->Val) ? sizeof(S->Val) : R->ValLen;
    RfsZero(S->Key, 16);
    RfsCopy(S->Key, R->Key, R->KeyLen > 16 ? 16 : R->KeyLen);
    RfsCopy(S->Val, R->Val, n);
    S->Len = n;
    S->Flags = R->Flags;
    S->Have = TRUE;
}

/* the rows whose first u64 of the key is at or past From (16-byte keys) go */
static RFS_STATUS
DeleteFrom(PRFS_TABLE T, ULONGLONG From, BOOLEAN ById, ULONGLONG KC, BOOLEAN V2, ULONG Cpc)
{
    for (;;) {
        RFS_CURSOR c;
        RFS_STATUS st = RfsSeek(T, NULL, 0, &c);
        BOOLEAN found = FALSE;
        UCHAR key[16];
        while (RFS_SUCCESS(st) && st != RFS_NO_MORE) {
            PRFS_ROW r = RfsCurRow(&c);
            ULONGLONG x;
            if (ById) x = (V2 ? RfsGet64(r->Val + REFS2_CONT_PHYS) : RfsGet64(r->Val + r->ValLen - 16)) / Cpc;
            else x = RfsGet64(r->Key);
            if (x >= (ById ? KC : From) && r->KeyLen == 16) { RfsCopy(key, r->Key, 16); found = TRUE; break; }
            st = RfsNext(&c);
        }
        RfsCursorClose(&c);
        if (st != RFS_NO_MORE && !RFS_SUCCESS(st)) return st;
        if (!found) return RFS_OK;
        st = RfsTreeDelete(T, key, 16);
        if (!RFS_SUCCESS(st)) return st;
    }
}

static RFS_STATUS
ResizeContainerTable(PRFS_VOL V, ULONG Root, const THDR *H, ULONGLONG N)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    RFS_STATUS st;
    static SAVED tail, model;
    ULONGLONG cpc = H->Cpc, maxId = 0, delta, tailId, p;
    ULONG po;
    tail.Have = model.Have = FALSE;
    st = RfsGlobal(V, Root, &T);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsSeek(T, NULL, 0, &c);
    while (RFS_SUCCESS(st) && st != RFS_NO_MORE) {
        PRFS_ROW r = RfsCurRow(&c);
        ULONGLONG phys, id = RfsGet64(r->Key);
        if (r->KeyLen != 16 || r->ValLen < 0x38) { RfsCursorClose(&c); return RFS_CORRUPT; }
        phys = V->V2 ? RfsGet64(r->Val + REFS2_CONT_PHYS) : RfsGet64(r->Val + r->ValLen - 16);
        if (phys == (ULONGLONG)(H->OldN - 1) * cpc) Save(&tail, r);
        else if (phys == (ULONGLONG)(H->OldN - 2) * cpc) Save(&model, r);
        if (phys < (ULONGLONG)H->K * cpc && id > maxId) maxId = id;
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    if (st != RFS_NO_MORE && !RFS_SUCCESS(st)) return st;
    if (!tail.Have || !model.Have) return RFS_CORRUPT;
    po = V->V2 ? REFS2_CONT_PHYS : model.Len - 16;
    delta = RfsGet64(model.Key) - (H->OldN - 2);
    tailId = RfsGet64(tail.Key) == (H->OldN - 1) + delta ? (N - 1) + delta : RfsGet64(tail.Key);

    st = DeleteFrom(T, 0, TRUE, H->K, V->V2, H->Cpc);
    if (!RFS_SUCCESS(st)) return st;
    for (p = H->K; p + 1 < N; p++) {
        ULONGLONG id = p + delta;
        RfsPut64(model.Key, id);
        RfsPut64(model.Val, id);
        RfsPut64(model.Val + po, p * cpc);
        st = RfsTreeInsert(T, model.Val, 16, model.Val, model.Len, model.Flags, FALSE);
        if (!RFS_SUCCESS(st)) return st;
        if (id > maxId) maxId = id;
    }
    po = V->V2 ? REFS2_CONT_PHYS : tail.Len - 16;
    RfsPut64(tail.Key, tailId);
    RfsPut64(tail.Val, tailId);
    RfsPut64(tail.Val + po, (N - 1) * cpc);
    st = RfsTreeInsert(T, tail.Val, 16, tail.Val, tail.Len, tail.Flags, FALSE);
    if (!RFS_SUCCESS(st)) return st;
    if (tailId > maxId) maxId = tailId;
    /* the header data: the highest container id */
    if (T->Root->Desc && T->Root->DescLen >= REFS_IR_DATA + 8) RfsPut64(T->Root->Desc + REFS_IR_DATA, maxId);
    T->Root->Dirty = TRUE;
    RfsTableDirty(T);
    return RFS_OK;
}

static RFS_STATUS
ResizeMedium(PRFS_VOL V, const THDR *H, ULONGLONG N)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    RFS_STATUS st;
    static SAVED tail, one, many;
    ULONGLONG cpc = H->Cpc, kc = (ULONGLONG)H->K * cpc, freeN = 0;
    tail.Have = one.Have = many.Have = FALSE;
    st = RfsGlobal(V, REFS_ROOT_MEDIUM, &T);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsSeek(T, NULL, 0, &c);
    while (RFS_SUCCESS(st) && st != RFS_NO_MORE) {
        PRFS_ROW r = RfsCurRow(&c);
        ULONGLONG s = RfsGet64(r->Val), l = RfsGet64(r->Val + 8);
        BOOLEAN bitmap = (BOOLEAN)((RfsGet16(r->Val + 0x12) & 1) != 0);
        if (s < kc && s + l > kc) { RfsCursorClose(&c); return RFS_CORRUPT; }     /* (not in any template) */
        if (s == (ULONGLONG)(H->OldN - 1) * cpc) Save(&tail, r);
        else if (s >= kc && !bitmap && RfsGet16(r->Val + 0x10) != 0) {
            if (l == cpc) { if (!one.Have) Save(&one, r); }
            else if (!many.Have) Save(&many, r);
        }
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    if (st != RFS_NO_MORE && !RFS_SUCCESS(st)) return st;
    if (!tail.Have || (!one.Have && !many.Have)) return RFS_CORRUPT;
    st = DeleteFrom(T, kc, FALSE, 0, V->V2, H->Cpc);
    if (!RFS_SUCCESS(st)) return st;
    if (N - 1 > H->K) {
        ULONGLONG len = (N - 1 - H->K) * cpc;
        SAVED *m = (len > cpc && many.Have) || !one.Have ? &many : &one;
        RfsPut64(m->Val, kc);
        RfsPut64(m->Val + 8, len);
        RfsPut16(m->Val + 0x10, (USHORT)(len > 0xFFFF ? 0xFFFF : len));
        st = RfsTreeInsert(T, m->Val, 16, m->Val, m->Len, m->Flags, FALSE);
        if (!RFS_SUCCESS(st)) return st;
    }
    RfsPut64(tail.Val, (N - 1) * cpc);
    st = RfsTreeInsert(T, tail.Val, 16, tail.Val, tail.Len, tail.Flags, FALSE);
    if (!RFS_SUCCESS(st)) return st;
    /* the summary */
    st = RfsSeek(T, NULL, 0, &c);
    while (RFS_SUCCESS(st) && st != RFS_NO_MORE) {
        PRFS_ROW r = RfsCurRow(&c);
        freeN += RfsAllocRowFree(r->Val, r->ValLen);
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    if (st != RFS_NO_MORE && !RFS_SUCCESS(st)) return st;
    if (!T->Root->Desc || T->Root->DescLen < REFS_IR_DATA + RFS_SUM_FREE(V) + 0x18) return RFS_CORRUPT;
    RfsPut64(T->Root->Desc + REFS_IR_DATA + RFS_SUM_COVERED(V), N * cpc);
    RfsPut64(T->Root->Desc + REFS_IR_DATA + RFS_SUM_FREE(V), freeN);
    RfsPut64(T->Root->Desc + REFS_IR_DATA + RFS_SUM_FREE(V) + 0x10, N * cpc - freeN);
    V->FreeClusters = freeN;
    T->Root->Dirty = TRUE;
    RfsTableDirty(T);
    return RFS_OK;
}

/* root 11 (3.x): one row {0, the volume's clusters} */
static RFS_STATUS
ResizeRoot11(PRFS_VOL V, const THDR *H, ULONGLONG N)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    RFS_STATUS st;
    static SAVED row;
    ULONGLONG old = (ULONGLONG)H->OldN * H->Cpc;
    row.Have = FALSE;
    if (V->NRoots <= REFS_ROOT_INTEGRITY) return RFS_OK;
    st = RfsGlobal(V, REFS_ROOT_INTEGRITY, &T);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsSeek(T, NULL, 0, &c);
    while (RFS_SUCCESS(st) && st != RFS_NO_MORE) {
        PRFS_ROW r = RfsCurRow(&c);
        if (r->KeyLen == 16 && RfsGet64(r->Key) == 0 && RfsGet64(r->Key + 8) == old && r->ValLen >= 16) { Save(&row, r); break; }
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    if (!row.Have) return RFS_OK;
    st = RfsTreeDelete(T, row.Key, 16);
    if (!RFS_SUCCESS(st)) return st;
    RfsPut64(row.Val + 8, N * H->Cpc);
    return RfsTreeInsert(T, row.Val, 16, row.Val, row.Len, row.Flags, FALSE);
}

/* the in-memory container maps, from the (changed) container table */
static RFS_STATUS
RebuildMaps(PRFS_VOL V)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    RFS_STATUS st;
    ULONGLONG maxId = 0;
    ULONG i, n;
    st = RfsGlobal(V, REFS_ROOT_CONTAINER, &T);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsSeek(T, NULL, 0, &c);
    while (RFS_SUCCESS(st) && st != RFS_NO_MORE) {
        if (RfsGet64(RfsCurRow(&c)->Key) > maxId) maxId = RfsGet64(RfsCurRow(&c)->Key);
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    n = (ULONG)maxId + 1;
    if (V->ContPhys) RfsHostFree(V->ContPhys);
    if (V->PhysToCont) RfsHostFree(V->PhysToCont);
    if (V->ContFlags) RfsHostFree(V->ContFlags);
    V->NConts = n;
    V->NPhysConts = (ULONG)(V->TotalClusters / V->ClustersPerContainer + 1);
    V->ContPhys = (PULONGLONG)RfsHostAlloc(n * sizeof(ULONGLONG));
    V->PhysToCont = (PULONG)RfsHostAlloc(V->NPhysConts * sizeof(ULONG));
    V->ContFlags = (PUCHAR)RfsAllocZero(2 * n + 2);
    if (!V->ContPhys || !V->PhysToCont || !V->ContFlags) return RFS_NO_MEMORY;
    for (i = 0; i < n; i++) V->ContPhys[i] = V->V2 ? ((ULONGLONG)i << V->ContainerShift) : ~0ULL;
    for (i = 0; i < V->NPhysConts; i++) V->PhysToCont[i] = 0xFFFFFFFFUL;
    st = RfsSeek(T, NULL, 0, &c);
    while (RFS_SUCCESS(st) && st != RFS_NO_MORE) {
        PRFS_ROW r = RfsCurRow(&c);
        ULONG id = (ULONG)RfsGet64(r->Key), f = RfsGet32(r->Val + 0x14);
        V->ContPhys[id] = V->V2 ? RfsGet64(r->Val + REFS2_CONT_PHYS) : RfsGet64(r->Val + r->ValLen - 16);
        V->ContFlags[id] = (UCHAR)f;
        V->ContFlags[id + n] = (UCHAR)(f >> 8);
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    for (i = 0; i < n; i++) {
        if (V->ContPhys[i] != ~0ULL && V->ContPhys[i] / V->ClustersPerContainer < V->NPhysConts)
            V->PhysToCont[V->ContPhys[i] / V->ClustersPerContainer] = i;
    }
    return st == RFS_NO_MORE ? RFS_OK : st;
}

/* ---- the format ------------------------------------------------------------------------------ */
RFS_STATUS
RfsFormatTemplate(PRFS_VOL V, UCHAR Major, ULONG ClusterSize, const char *Label,
                  ULONGLONG Serial, const UCHAR *Guid, RFS_FMT_PROGRESS Progress, PVOID Ctx,
                  PRFS_FMT_RESULT Result)
{
    const TMPL *t = NULL;
    THDR h;
    ULONG i, bps, cs;
    ULONGLONG N, total, o;
    PUCHAR raw = NULL, z = NULL, page = NULL;
    RFS_STATUS st;
    const UCHAR *runs;
    ULONGLONG sb = 0, vbr = 0;          /* where clusters 30 and 0 are in raw (+1) */

    for (i = 0; i < sizeof(Templates) / sizeof(Templates[0]); i++)
        if (Templates[i].Major == Major && Templates[i].ClusterSize == ClusterSize) t = &Templates[i];
    if (!t || !ParseTemplate(t->Data, t->Size, &h)) return RFS_NOT_SUPPORTED;
    cs = h.ClusterSize;
    bps = V->SectorSize ? V->SectorSize : 512;
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || bps > cs) return RFS_NOT_SUPPORTED;
    N = V->DeviceBytes / cs / h.Cpc;
    if (N < (ULONGLONG)h.K + 1) return RFS_DISK_FULL;
    if (N > 0x7FFFFFF) N = 0x7FFFFFF;
    total = N * h.Cpc;

    raw = (PUCHAR)RfsHostAlloc(h.RawLen);
    z = (PUCHAR)RfsAllocZero(ZCHUNK);
    page = (PUCHAR)RfsHostAlloc(cs);
    st = RFS_NO_MEMORY;
    if (!raw || !z || !page) goto out;
    st = RFS_CORRUPT;
    if (!Expand(h.Comp, h.CompLen, raw, h.RawLen)) goto out;

    /* 1. the log, zeroed */
    if (Progress) Progress(Ctx, 0);
    for (i = 0; i < h.NZero; i++) {
        st = WriteZero(V, RfsGet64(h.Zero + 16 * i) * cs, RfsGet64(h.Zero + 16 * i + 8) * cs, z, Progress, Ctx);
        if (!RFS_SUCCESS(st)) goto out;
    }
    /* 2. the template's clusters */
    runs = h.Runs;
    for (i = 0, o = 0; i < h.NRuns; i++) {
        ULONGLONG s = RfsGet32(runs + 8 * i), n = RfsGet32(runs + 8 * i + 4), d = 0;
        if (s <= REFS_SB_LOCATION && REFS_SB_LOCATION < s + n) sb = o + (REFS_SB_LOCATION - s) * cs + 1;
        if (s == 0) vbr = o + 1;
        while (d < n * cs) {
            ULONG k = (n * cs - d) > ZCHUNK ? ZCHUNK : (ULONG)(n * cs - d);
            st = RfsHostWrite(V, s * cs + d, k, raw + o + d);
            if (!RFS_SUCCESS(st)) goto out;
            d += k;
        }
        o += n * cs;
    }
    st = RFS_CORRUPT;
    if (!sb || !vbr) goto out;

    /* 3. the boot sector, here and in the last sector */
    {
        PUCHAR b = raw + vbr - 1;
        ULONG size = RfsGet16(b + REFS_VBR_FSRS_SIZE);
        if (size < 0x48 || size > 512) goto out;
        RfsPut64(b + REFS_VBR_SECTORS, total * cs / bps);
        RfsPut32(b + REFS_VBR_BPS, bps);
        RfsPut32(b + REFS_VBR_SPC, cs / bps);
        RfsPut64(b + REFS_VBR_SERIAL, Serial);
        if (Guid && h.Major >= 3 && size >= REFS_VBR_GUID + 16) RfsCopy(b + REFS_VBR_GUID, Guid, 16);
        RfsPut16(b + REFS_VBR_CHECKSUM, RfsVbrChecksum(b, size));
        RfsZero(page, cs);
        RfsCopy(page, b, 512);
        st = RfsHostWrite(V, 0, bps, page);
        if (RFS_SUCCESS(st)) st = RfsHostWrite(V, (total - 4) * cs, cs, z);
        if (RFS_SUCCESS(st)) st = RfsHostWrite(V, (total - 1) * cs, cs, z);
        if (RFS_SUCCESS(st)) st = RfsHostWrite(V, total * cs - bps, bps, page);
        if (!RFS_SUCCESS(st)) goto out;
    }
    /* 4. the backup superblocks */
    for (i = 2; i <= 3; i++) {
        RfsCopy(page, raw + sb - 1, cs);
        SelfSum(page, cs, total - i);
        st = RfsHostWrite(V, (total - i) * cs, cs, page);
        if (!RFS_SUCCESS(st)) goto out;
    }
    st = RfsHostFlush(V);
    if (!RFS_SUCCESS(st)) goto out;
    RfsHostFree(raw); raw = NULL;

    /* 5. the size-dependent tables */
    V->Formatting = TRUE;
    st = RfsMount(V, FALSE);
    if (!RFS_SUCCESS(st)) goto out;
    if (V->ReadOnly || V->TotalClusters != total) { st = RFS_CORRUPT; goto unmount; }
    st = ResizeMedium(V, &h, N);
    if (RFS_SUCCESS(st)) st = ResizeContainerTable(V, REFS_ROOT_CONTAINER, &h, N);
    if (RFS_SUCCESS(st)) st = ResizeContainerTable(V, REFS_ROOT_CONTAINER2, &h, N);
    if (RFS_SUCCESS(st)) st = ResizeRoot11(V, &h, N);
    if (RFS_SUCCESS(st)) st = RebuildMaps(V);
    if (RFS_SUCCESS(st)) st = RfsSetLabel(V, Label ? Label : "");
    if (RFS_SUCCESS(st)) st = RfsCommit(V);
    /* again: the other checkpoint too */
    if (RFS_SUCCESS(st)) st = RfsSetLabel(V, Label ? Label : "");
    if (RFS_SUCCESS(st)) {
        PRFS_TABLE T;
        st = RfsGlobal(V, REFS_ROOT_MEDIUM, &T);
        if (RFS_SUCCESS(st)) { T->Root->Dirty = TRUE; RfsTableDirty(T); }
    }
    if (RFS_SUCCESS(st)) st = RfsCommit(V);
    if (RFS_SUCCESS(st)) st = RfsHostFlush(V);
unmount:
    RfsUnmount(V);
    V->Formatting = FALSE;
    if (!RFS_SUCCESS(st)) goto out;

    /* 6. read back */
    {
        PVOID host = V->HostContext;
        ULONG ss = V->SectorSize;
        ULONGLONG db = V->DeviceBytes;
        RfsZero(V, sizeof(*V));
        V->HostContext = host; V->SectorSize = ss; V->DeviceBytes = db;
        st = RfsMount(V, FALSE);
        if (!RFS_SUCCESS(st)) goto out;
        if (Result) {
            Result->Major = V->Major;
            Result->Minor = V->Minor;
            Result->ClusterSize = V->ClusterSize;
            Result->TotalClusters = V->TotalClusters;
            Result->FreeClusters = V->FreeClusters;
            Result->Bytes = V->TotalClusters * V->ClusterSize;
        }
        if (V->ReadOnly) st = RFS_CORRUPT;
        RfsUnmount(V);
    }
    if (Progress) Progress(Ctx, 100);
out:
    if (raw) RfsHostFree(raw);
    if (z) RfsHostFree(z);
    if (page) RfsHostFree(page);
    return st;
}

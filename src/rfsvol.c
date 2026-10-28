/*
 * REFSNT - the volume: boot sector, superblock, checkpoints, containers
 * (virtual -> physical clusters), the page cache, and mounting.
 */
#include "rfseng.h"
#include "rfsshim.h"

/* ---- references ---------------------------------------------------------------------------- */
VOID
RfsRefGet(PRFS_VOL V, const UCHAR *P, PRFS_REF R)
{
    ULONG i, n;
    RfsZero(R, sizeof(*R));
    if (V->V1) {
        R->Lcn[0] = RfsGet64(P + REFS1_REF_BLOCK);
        R->Flags = RfsGet16(P + REFS1_REF_FLAGS);
        R->CkType = P[REFS1_REF_CKTYPE];
        n = RfsGet16(P + REFS1_REF_CKLEN);
        if (n > 8) n = 8;
        R->CkLen = (UCHAR)n;
        RfsCopy(R->Ck, P + REFS1_REF_CK, n);
        return;
    }
    for (i = 0; i < 4; i++) R->Lcn[i] = RfsGet64(P + 8 * i);
    R->Flags = RfsGet16(P + REFS3_REF_FLAGS);
    R->CkType = P[REFS3_REF_CKTYPE];
    n = RfsGet32(P + REFS3_REF_CKLEN);
    if (n > 32 || 0x28 + n > V->RefSize) n = 0;
    R->CkLen = (UCHAR)n;
    RfsCopy(R->Ck, P + REFS3_REF_CK, n);
}

VOID
RfsRefPut(PRFS_VOL V, PUCHAR P, const RFS_REF *R)
{
    ULONG i;
    if (V->V1) {
        RfsZero(P, REFS1_REF_SIZE);
        RfsPut64(P + REFS1_REF_BLOCK, R->Lcn[0]);
        RfsPut16(P + REFS1_REF_FLAGS, R->Flags);
        P[REFS1_REF_CKTYPE] = R->CkType;
        P[REFS1_REF_CKOFF] = 8;
        RfsPut16(P + REFS1_REF_CKLEN, R->CkLen);
        RfsCopy(P + REFS1_REF_CK, R->Ck, R->CkLen);
        return;
    }
    RfsZero(P, RFS_TREEREF(V));                 /* (2.x checkpoint slots: the rest stays zero) */
    for (i = 0; i < 4; i++) RfsPut64(P + 8 * i, R->Lcn[i]);
    RfsPut16(P + REFS3_REF_FLAGS, R->Flags);
    P[REFS3_REF_CKTYPE] = R->CkType;
    P[REFS3_REF_CKOFF] = 8;
    RfsPut32(P + REFS3_REF_CKLEN, R->CkLen);
    RfsCopy(P + REFS3_REF_CK, R->Ck, R->CkLen);
}

BOOLEAN
RfsRefValid(PRFS_VOL V, const RFS_REF *R)
{
    (VOID)V;
    return (BOOLEAN)(R->Lcn[0] != 0 && R->Lcn[0] != ~(ULONGLONG)0);
}

/* ---- raw clusters ------------------------------------------------------------------------------ */
RFS_STATUS
RfsReadClusters(PRFS_VOL V, ULONGLONG Lcn, ULONG Count, PVOID Buf)
{
    return RfsHostRead(V, Lcn << V->ClusterShift, Count << V->ClusterShift, Buf);
}

/* virtual cluster -> physical; Run: clusters to the end of the container */
RFS_STATUS
RfsTranslate(PRFS_VOL V, ULONGLONG Vlcn, PULONGLONG Plcn, PULONG Run)
{
    ULONGLONG c, off;
    if (V->V1 || !V->ClustersPerContainer) { *Plcn = Vlcn; if (Run) *Run = 0x7FFFFFFF; return RFS_OK; }
    c = Vlcn >> V->ContainerShift;
    off = Vlcn & (V->ClustersPerContainer - 1);
    if (c >= V->NConts || V->ContPhys[c] == ~(ULONGLONG)0) return RFS_CORRUPT;
    *Plcn = V->ContPhys[c] + off;
    if (Run) *Run = (ULONG)(V->ClustersPerContainer - off);
    return RFS_OK;
}

/* ---- the page cache -------------------------------------------------------------------------- */
static ULONG PageHash(ULONGLONG A) { return (ULONG)((A * 0x9E3779B97F4A7C15ULL) >> 55) & (RFS_PAGE_HASH - 1); }

static VOID
PageUnlink(PRFS_VOL V, PRFS_PAGE P)
{
    if (P->Prev) P->Prev->Next = P->Next; else V->PageHead = P->Next;
    if (P->Next) P->Next->Prev = P->Prev; else V->PageTail = P->Prev;
    P->Prev = P->Next = NULL;
}

static VOID
PageFront(PRFS_VOL V, PRFS_PAGE P)
{
    P->Next = V->PageHead;
    P->Prev = NULL;
    if (V->PageHead) V->PageHead->Prev = P; else V->PageTail = P;
    V->PageHead = P;
}

static VOID
PageDestroy(PRFS_VOL V, PRFS_PAGE P)
{
    PRFS_PAGE *pp = &V->PageHash[PageHash(P->Addr)];
    while (*pp && *pp != P) pp = &(*pp)->HashNext;
    if (*pp) *pp = P->HashNext;
    PageUnlink(V, P);
    RfsHostFree(P->Data);
    RfsHostFree(P);
    V->PageCount--;
}

/* trims unreferenced pages beyond the limit */
static VOID
PageTrim(PRFS_VOL V)
{
    PRFS_PAGE P = V->PageTail;
    while (V->PageCount > V->PageMax && P) {
        PRFS_PAGE prev = P->Prev;
        if (P->Refs == 0) PageDestroy(V, P);
        P = prev;
    }
}

/* the physical address of a page: its first cluster, translated */
static RFS_STATUS
PageAddr(PRFS_VOL V, const RFS_REF *R, BOOLEAN Real, PULONGLONG Addr)
{
    if (V->V1 || Real) { *Addr = R->Lcn[0]; return RFS_OK; }
    return RfsTranslate(V, R->Lcn[0], Addr, NULL);
}

static RFS_STATUS
PageLoad(PRFS_VOL V, const RFS_REF *R, BOOLEAN Real, PUCHAR Buf)
{
    RFS_STATUS st;
    ULONG i;
    if (V->V1) return RfsHostRead(V, R->Lcn[0] << REFS1_BLOCK_SHIFT, REFS1_PAGE, Buf);
    for (i = 0; i < V->ClustersPerPage; ) {
        ULONGLONG p0, pn;
        ULONG n = 1;
        if (Real) p0 = R->Lcn[i];
        else { st = RfsTranslate(V, R->Lcn[i], &p0, NULL); if (!RFS_SUCCESS(st)) return st; }
        /* physically consecutive slots in one read */
        while (i + n < V->ClustersPerPage) {
            if (Real) pn = R->Lcn[i + n];
            else if (!RFS_SUCCESS(RfsTranslate(V, R->Lcn[i + n], &pn, NULL))) break;
            if (pn != p0 + n) break;
            n++;
        }
        st = RfsReadClusters(V, p0, n, Buf + (i << V->ClusterShift));
        if (!RFS_SUCCESS(st)) return st;
        i += n;
    }
    return RFS_OK;
}

/* checks a page against its reference (checksum) and against itself (signature, own address) */
static RFS_STATUS
PageCheck(PRFS_VOL V, const RFS_REF *R, const UCHAR *Data)
{
    UCHAR ck[32];
    if (V->V1) {
        if (RfsGet64(Data + REFS1_PH_BLOCK) != R->Lcn[0]) return RFS_CORRUPT;
    } else {
        if (RfsCmp(Data, "MSB+", 4) != 0) return RFS_CORRUPT;
        if (RfsGet64(Data + REFS3_PH_LCN) != R->Lcn[0]) return RFS_CORRUPT;
    }
    if (V->VerifyChecksums && R->CkType != REFS_CK_NONE && R->CkLen) {
        ULONG n = RfsChecksum(R->CkType, Data, V->PageSize, ck);
        if (n != R->CkLen || RfsCmp(ck, R->Ck, n) != 0) {
            /* a crc collision sentinel: 0xABBAFFFFABBAFFFE is stored as all ones */
            V->ChecksumErrors++;
            return RFS_CHECKSUM;
        }
    }
    return RFS_OK;
}

RFS_STATUS
RfsPageRead(PRFS_VOL V, const RFS_REF *R, BOOLEAN Real, PRFS_PAGE *Out)
{
    ULONGLONG addr;
    PRFS_PAGE P;
    RFS_STATUS st;
    ULONG h;
    *Out = NULL;
    if (!RfsRefValid(V, R)) return RFS_CORRUPT;
    st = PageAddr(V, R, Real, &addr);
    if (!RFS_SUCCESS(st)) return st;
    h = PageHash(addr);
    for (P = V->PageHash[h]; P; P = P->HashNext) {
        if (P->Addr == addr) {
            P->Refs++;
            PageUnlink(V, P);
            PageFront(V, P);
            *Out = P;
            return RFS_OK;
        }
    }
    P = (PRFS_PAGE)RfsAllocZero(sizeof(RFS_PAGE));
    if (!P) return RFS_NO_MEMORY;
    P->Data = (PUCHAR)RfsHostAlloc(V->PageSize);
    if (!P->Data) { RfsHostFree(P); return RFS_NO_MEMORY; }
    st = PageLoad(V, R, Real, P->Data);
    if (RFS_SUCCESS(st)) st = PageCheck(V, R, P->Data);
    if (!RFS_SUCCESS(st)) { RfsHostFree(P->Data); RfsHostFree(P); return st; }
    P->Addr = addr;
    P->Refs = 1;
    P->HashNext = V->PageHash[h];
    V->PageHash[h] = P;
    PageFront(V, P);
    V->PageCount++;
    PageTrim(V);
    *Out = P;
    return RFS_OK;
}

VOID
RfsPagePut(PRFS_VOL V, PRFS_PAGE P)
{
    if (!P) return;
    if (P->Refs) P->Refs--;
    PageTrim(V);
}

VOID
RfsPageDropAll(PRFS_VOL V)
{
    while (V->PageHead) PageDestroy(V, V->PageHead);
}

/* forgets the cached copy of the page at physical Addr (it is about to be rewritten) */
VOID
RfsPageForget(PRFS_VOL V, ULONGLONG Addr)
{
    PRFS_PAGE P;
    for (P = V->PageHash[PageHash(Addr)]; P; P = P->HashNext) {
        if (P->Addr == Addr) {
            if (P->Refs == 0) PageDestroy(V, P);
            else P->Addr = ~(ULONGLONG)0;          /* stale: never found again */
            return;
        }
    }
}

/* ---- boot sector ------------------------------------------------------------------------------- */
BOOLEAN
RfsProbe(const UCHAR *S0)
{
    return (BOOLEAN)(RfsCmp(S0 + REFS_VBR_FSNAME, "ReFS\0\0\0\0", 8) == 0 && RfsCmp(S0 + REFS_VBR_FSRS, "FSRS", 4) == 0);
}

/*
 * A volume made bootable by refsboot carries the boot loader (with a FAT
 * BPB) in sectors 0-1 and its ReFS boot sector in sector 2 (and at its end).
 */
BOOLEAN
RfsProbeView(const UCHAR *Sectors)
{
    return (BOOLEAN)(RfsCmp(Sectors + 3, "REFSNTB1", 8) == 0 && Sectors[510] == 0x55 && Sectors[511] == 0xAA &&
                     RfsProbe(Sectors + 1024));
}

static RFS_STATUS
ReadBoot(PRFS_VOL V)
{
    UCHAR s[1536], *b = s;
    RFS_STATUS st = RfsHostRead(V, 0, 1536, s);
    ULONG bps, spc, size;
    if (!RFS_SUCCESS(st)) return st;
    if (!RfsProbe(b)) {
        if (!RfsProbeView(s)) return RFS_UNRECOGNIZED;
        b = s + 1024;
        V->BootView = TRUE;
        RfsCopy(V->BootSectors, s, 1024);
    }
    size = RfsGet16(b + REFS_VBR_FSRS_SIZE);
    if (size < 0x48 || size > 512 || RfsVbrChecksum(b, size) != RfsGet16(b + REFS_VBR_CHECKSUM)) return RFS_UNRECOGNIZED;
    bps = RfsGet32(b + REFS_VBR_BPS);
    spc = RfsGet32(b + REFS_VBR_SPC);
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || !spc || (spc & (spc - 1))) return RFS_UNRECOGNIZED;
    V->ClusterSize = bps * spc;
    if (V->ClusterSize < 4096 || V->ClusterSize > 65536) return RFS_UNRECOGNIZED;
    for (V->ClusterShift = 0; (1UL << V->ClusterShift) < V->ClusterSize; V->ClusterShift++)
        ;
    V->Major = b[REFS_VBR_MAJOR];
    V->Minor = b[REFS_VBR_MINOR];
    V->VbrFlags = RfsGet32(b + REFS_VBR_FLAGS);
    V->Serial = RfsGet64(b + REFS_VBR_SERIAL);
    V->TotalClusters = (RfsGet64(b + REFS_VBR_SECTORS) * bps) >> V->ClusterShift;
    if (V->Major == 1) {
        V->V1 = TRUE;
        V->PageSize = REFS1_PAGE;
        V->HeaderSize = REFS1_PH_SIZE;
        V->RefSize = REFS1_REF_SIZE;
        V->TotalClusters = (RfsGet64(b + REFS_VBR_SECTORS) * bps) >> REFS1_BLOCK_SHIFT;
    } else if (V->Major == 2 || V->Major == 3) {
        ULONGLONG cb = RfsGet64(b + REFS_VBR_CONTAINER);
        V->PageSize = V->ClusterSize < 16384 ? 16384 : V->ClusterSize;
        V->ClustersPerPage = V->PageSize >> V->ClusterShift;
        V->HeaderSize = REFS3_PH_SIZE;
        /* ReFS 2.x (Server 2016 TH2) does not record the container size in the
         * VBR; it uses the standard 64 MiB band like 3.x. */
        if (!cb && V->Major == 2) { V->V2 = TRUE; cb = 64ULL << 20; }
        if (cb) {
            ULONG s = 0;
            V->ClustersPerContainer = (ULONG)(cb >> V->ClusterShift);
            if (V->ClustersPerContainer & (V->ClustersPerContainer - 1)) return RFS_UNRECOGNIZED;
            while ((1UL << s) <= V->ClustersPerContainer) s++;
            V->ContainerShift = s;         /* one more than log2: the boundary bit is never set */
        }
    } else {
        return RFS_UNRECOGNIZED;
    }
    for (V->PageShift = 0; (1UL << V->PageShift) < V->PageSize; V->PageShift++)
        ;
    return RFS_OK;
}

/* ---- superblock and checkpoints (3.x) -------------------------------------------------------- */
/* a self checksum: computed over the first cluster with the self descriptor zeroed */
static BOOLEAN
SelfOk(PRFS_VOL V, PUCHAR Page, ULONG DescOff, ULONG DescLen, ULONG Span)
{
    UCHAR save[104], ck[32];
    PUCHAR d;
    ULONG type, len, n;
    if (DescOff + DescLen > Span || DescLen > sizeof(save) || DescLen < 0x28) return FALSE;
    d = Page + DescOff;
    type = d[REFS3_REF_CKTYPE];
    len = RfsGet32(d + REFS3_REF_CKLEN);
    if (type == REFS_CK_NONE) return TRUE;
    if (len > 32 || 0x28 + len > DescLen) return FALSE;
    RfsCopy(save, d, DescLen);
    RfsZero(d, DescLen);
    n = RfsChecksum((UCHAR)type, Page, Span, ck);
    RfsCopy(d, save, DescLen);
    V->SelfCkType = (UCHAR)type;
    return (BOOLEAN)(n == len && RfsCmp(ck, save + REFS3_REF_CK, n) == 0);
}

static RFS_STATUS
ReadSuper3(PRFS_VOL V, PUCHAR Buf)
{
    ULONGLONG where[3];
    ULONG i;
    where[0] = REFS_SB_LOCATION;
    where[1] = V->TotalClusters - 2;
    where[2] = V->TotalClusters - 3;
    for (i = 0; i < 3; i++) {
        ULONG off, cnt;
        if (!RFS_SUCCESS(RfsReadClusters(V, where[i], 1, Buf))) continue;
        if (RfsCmp(Buf, "SUPB", 4) != 0) continue;
        if (!SelfOk(V, Buf, RfsGet32(Buf + REFS3_SB_SELF_OFF), RfsGet32(Buf + REFS3_SB_SELF_LEN), V->ClusterSize)) continue;
        off = RfsGet32(Buf + REFS3_SB_CHKP_OFF);
        cnt = RfsGet32(Buf + REFS3_SB_CHKP_COUNT);
        if (cnt != 2 || off + 16 > V->ClusterSize) continue;
        V->ChkpLcn[0] = RfsGet64(Buf + off);
        V->ChkpLcn[1] = RfsGet64(Buf + off + 8);
        RfsCopy(V->Guid, Buf + REFS3_SB_GUID, 16);
        V->VolSig = RfsGet32(V->Guid) ^ RfsGet32(V->Guid + 4) ^ RfsGet32(V->Guid + 8) ^ RfsGet32(V->Guid + 12);
        return RFS_OK;
    }
    return RFS_CORRUPT;
}

static RFS_STATUS
ParseCheckpoint3(PRFS_VOL V, PUCHAR P)
{
    ULONG n, i, base, refsize;
    if (RfsCmp(P, "CHKP", 4) != 0) return RFS_CORRUPT;
    refsize = RfsGet32(P + REFS3_CP_REFSIZE);
    if (refsize != 0x68 && refsize != 0x30 && refsize != 0x48) return RFS_CORRUPT;
    n = RfsGet32(P + REFS3_CP_NROOTS);
    /* ReFS 2.x has 10 roots (no container-index, integrity or small-allocator
     * roots); 3.x has 13. Accept either, index 0-9 the same. */
    if (n != REFS3_NROOTS && !(V->V2 && n == REFS2_NROOTS)) return RFS_CORRUPT;
    V->RefSize = refsize;
    V->ChkpFlags = RfsGet32(P + REFS3_CP_FLAGS);
    V->Clock = RfsGet64(P + REFS3_CP_CLOCK);
    V->AllocClock = RfsGet64(P + REFS3_CP_ALLOCCLOCK);
    V->NRoots = n;
    base = REFS3_CP_ROOTS;
    if (V->ChkpFlags & REFS3_CPF_INDIRECT) base = RfsGet32(P + REFS3_CP_ROOTS);
    for (i = 0; i < n; i++) {
        ULONG off = RfsGet32(P + base + 4 * i);
        if (off + refsize > V->ClusterSize) return RFS_CORRUPT;
        V->RootOff[i] = off;
        RfsRefGet(V, P + off, &V->Roots[i]);
    }
    V->CkType = V->Roots[REFS_ROOT_OBJECT].CkType;
    return RFS_OK;
}

static RFS_STATUS
ReadCheckpoint3(PRFS_VOL V)
{
    PUCHAR buf[2];
    ULONG i, best = 2;
    ULONGLONG clock[2];
    RFS_STATUS st = RFS_CORRUPT;
    buf[0] = (PUCHAR)RfsHostAlloc(V->PageSize);
    buf[1] = (PUCHAR)RfsHostAlloc(V->PageSize);
    if (!buf[0] || !buf[1]) { RfsHostFree(buf[0]); RfsHostFree(buf[1]); return RFS_NO_MEMORY; }
    for (i = 0; i < 2; i++) {
        clock[i] = 0;
        if (!RFS_SUCCESS(RfsReadClusters(V, V->ChkpLcn[i], V->ClustersPerPage, buf[i]))) continue;
        if (RfsCmp(buf[i], "CHKP", 4) != 0) continue;
        if (!SelfOk(V, buf[i], RfsGet32(buf[i] + REFS3_CP_SELF_OFF), RfsGet32(buf[i] + REFS3_CP_REFSIZE), V->ClusterSize)) continue;
        clock[i] = RfsGet64(buf[i] + REFS3_CP_CLOCK);
        if (best == 2 || clock[i] > clock[best]) best = i;
    }
    if (best < 2) {
        st = ParseCheckpoint3(V, buf[best]);
        if (!RFS_SUCCESS(st) && clock[best ^ 1]) {
            best ^= 1;
            st = ParseCheckpoint3(V, buf[best]);
        }
    }
    if (RFS_SUCCESS(st)) {
        V->Chkp = buf[best];
        V->ChkpSlot = best;
        RfsHostFree(buf[best ^ 1]);
    } else {
        RfsHostFree(buf[0]);
        RfsHostFree(buf[1]);
    }
    return st;
}

/* ---- containers --------------------------------------------------------------------------------- */
/*
 * ReFS 2.x container map. Same container table as 3.x (root 7 / its copy
 * root 8), but the band's physical cluster is at value offset +0x28 rather
 * than the last 16 bytes of the row. Containers not in the table (the self
 * region, container 0) are identity-mapped: virtual == physical.
 */
static RFS_STATUS
BuildContainersV2(PRFS_VOL V)
{
    PRFS_TABLE T = NULL;
    RFS_CURSOR c;
    RFS_STATUS st;
    ULONG idx[2] = { REFS_ROOT_CONTAINER, REFS_ROOT_CONTAINER2 }, k, i;
    for (k = 0; k < 2; k++) {
        ULONGLONG maxc = 0;
        st = RfsTableOpenRoot(V, idx[k], &T);
        if (!RFS_SUCCESS(st)) continue;
        st = RfsSeek(T, NULL, 0, &c);
        while (RFS_SUCCESS(st)) {
            PRFS_ROW r = RfsCurRow(&c);
            if (r->KeyLen >= 8 && RfsGet64(r->Key) > maxc) maxc = RfsGet64(r->Key);
            st = RfsNext(&c);
        }
        RfsCursorClose(&c);
        if (st != RFS_NO_MORE || maxc > 0x10000000) { RfsTableClose(V, T); continue; }
        if (maxc < V->TotalClusters / V->ClustersPerContainer) maxc = V->TotalClusters / V->ClustersPerContainer;
        V->NConts = (ULONG)maxc + 1;
        V->ContPhys = (PULONGLONG)RfsHostAlloc(V->NConts * sizeof(ULONGLONG));
        if (!V->ContPhys) { RfsTableClose(V, T); return RFS_NO_MEMORY; }
        for (i = 0; i < V->NConts; i++) V->ContPhys[i] = (ULONGLONG)i << V->ContainerShift;  /* identity default */
        st = RfsSeek(T, NULL, 0, &c);
        while (RFS_SUCCESS(st)) {
            PRFS_ROW r = RfsCurRow(&c);
            ULONGLONG id = (r->KeyLen >= 8) ? RfsGet64(r->Key) : ~(ULONGLONG)0;
            if (id < V->NConts && r->ValLen >= REFS2_CONT_PHYS + 8) V->ContPhys[id] = RfsGet64(r->Val + REFS2_CONT_PHYS);
            st = RfsNext(&c);
        }
        RfsCursorClose(&c);
        RfsTableClose(V, T);
        if (st == RFS_NO_MORE) return RFS_OK;
        RfsHostFree(V->ContPhys);
        V->ContPhys = NULL;
    }
    return RFS_CORRUPT;
}

static RFS_STATUS
LoadContainers(PRFS_VOL V)
{
    PRFS_TABLE T = NULL;
    RFS_CURSOR c;
    RFS_STATUS st;
    ULONG idx[2] = { REFS_ROOT_CONTAINER, REFS_ROOT_CONTAINER2 }, k;
    if (!V->ClustersPerContainer) return RFS_OK;
    if (V->V2) return BuildContainersV2(V);
    for (k = 0; k < 2; k++) {
        ULONGLONG maxc = 0, i;
        st = RfsTableOpenRoot(V, idx[k], &T);
        if (!RFS_SUCCESS(st)) continue;
        /* the largest container id first */
        st = RfsSeek(T, NULL, 0, &c);
        while (RFS_SUCCESS(st)) {
            PRFS_ROW r = RfsCurRow(&c);
            if (r->KeyLen >= 8 && RfsGet64(r->Key) > maxc) maxc = RfsGet64(r->Key);
            st = RfsNext(&c);
        }
        RfsCursorClose(&c);
        if (st != RFS_NO_MORE || maxc > 0x10000000) { RfsTableClose(V, T); continue; }
        V->NConts = (ULONG)maxc + 1;
        V->ContPhys = (PULONGLONG)RfsHostAlloc(V->NConts * sizeof(ULONGLONG));
        if (!V->ContPhys) { RfsTableClose(V, T); return RFS_NO_MEMORY; }
        for (i = 0; i < V->NConts; i++) V->ContPhys[i] = ~(ULONGLONG)0;
        st = RfsSeek(T, NULL, 0, &c);
        while (RFS_SUCCESS(st)) {
            PRFS_ROW r = RfsCurRow(&c);
            if (r->KeyLen >= 8 && r->ValLen >= 0x30) V->ContPhys[RfsGet64(r->Key)] = RfsGet64(r->Val + r->ValLen - 16);
            st = RfsNext(&c);
        }
        RfsCursorClose(&c);
        RfsTableClose(V, T);
        if (st == RFS_NO_MORE) return RFS_OK;
        RfsHostFree(V->ContPhys);
        V->ContPhys = NULL;
    }
    return RFS_CORRUPT;
}

/* ---- schemas: key comparison rules ---------------------------------------------------------------- */
static RFS_STATUS
LoadSchemas(PRFS_VOL V)
{
    RFS_CURSOR c;
    RFS_STATUS st;
    ULONG idx[2] = { REFS_ROOT_SCHEMA, REFS_ROOT_SCHEMA2 }, k;
    for (k = 0; k < 2; k++) {
        st = RfsTableOpenRoot(V, idx[k], &V->Schema);
        if (!RFS_SUCCESS(st)) continue;
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
        if (st == RFS_NO_MORE) return RFS_OK;
        RfsTableClose(V, V->Schema);
        V->Schema = NULL;
    }
    return RFS_CORRUPT;
}

UCHAR
RfsRuleOf(PRFS_VOL V, ULONG Schema)
{
    if (Schema >= 0xE000 && Schema < 0xE200) return V->SysRules[(Schema - 0xE000) >> 4];
    /* 1.x / 2.x file and directory-self records: attribute rows {u64 size, u32 type, name},
     * kept in (type, name) order though their schemas say rule 9 */
    if (RFS_V1REC(V) && (Schema == 0x110 || Schema == 0x170)) return RFS_RULE_V1ATTR;
    if (Schema < 0x2000) return V->Rules[Schema >> 4];
    return 0;
}

/* ---- the upcase table and the label ------------------------------------------------------------- */
static RFS_STATUS
LoadUpcase(PRFS_VOL V)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    RFS_STATUS st;
    ULONG pos = 0;
    PUCHAR raw;
    V->Upcase = (PUSHORT)RfsHostAlloc(65536 * 2);
    if (!V->Upcase) return RFS_NO_MEMORY;
    RfsDefaultUpcase(V->Upcase);
    if (V->V1) return RFS_OK;
    st = RfsTableOpenObject(V, REFS_OID_UPCASE, &T);
    if (!RFS_SUCCESS(st)) st = RfsTableOpenObject(V, REFS_OID_UPCASE2, &T);
    if (!RFS_SUCCESS(st)) return RFS_OK;                /* the built-in one */
    raw = (PUCHAR)RfsHostAlloc(4 + 65536 * 2);
    if (!raw) { RfsTableClose(V, T); return RFS_NO_MEMORY; }
    st = RfsSeek(T, NULL, 0, &c);
    while (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        if (r->KeyLen == 4 && RfsGet32(r->Key) >= 1) {
            ULONG n = r->ValLen;
            if (pos + n > 4 + 65536 * 2) n = 4 + 65536 * 2 - pos;
            RfsCopy(raw + pos, r->Val, n);
            pos += n;
        }
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    RfsTableClose(V, T);
    if (pos == 4 + 65536 * 2 && RfsGet32(raw) == 65536) RfsCopy(V->Upcase, raw + 4, 65536 * 2);
    RfsHostFree(raw);
    return RFS_OK;
}

static VOID
LoadLabel(PRFS_VOL V)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    UCHAR key[8];
    V->Label[0] = 0;
    if (!RFS_SUCCESS(RfsTableOpenObject(V, REFS_OID_VOLINFO, &T)) &&
        !RFS_SUCCESS(RfsTableOpenObject(V, REFS_OID_VOLINFO2, &T))) return;
    RfsZero(key, sizeof(key));
    RfsPut16(key, 0x510);
    if (RFS_SUCCESS(RfsLookup(T, key, 8, &c))) {
        PRFS_ROW r = RfsCurRow(&c);
        USHORT name[32];
        ULONG n = r->ValLen / 2, i;
        if (n > 32) n = 32;
        for (i = 0; i < n; i++) name[i] = RfsGet16(r->Val + 2 * i);
        while (n && name[n - 1] == 0) n--;
        RfsUtf16ToUtf8(name, n, V->Label, sizeof(V->Label));
    }
    RfsCursorClose(&c);
    RfsTableClose(V, T);
}

/* ---- mount ---------------------------------------------------------------------------------------- */
RFS_STATUS
RfsMount(PRFS_VOL V, BOOLEAN ReadOnly)
{
    RFS_STATUS st;
    PUCHAR sb;
    if (!V->PageMax) V->PageMax = 256;
    if (!V->TableMax) V->TableMax = 64;
    V->VerifyChecksums = TRUE;
    st = ReadBoot(V);
    if (!RFS_SUCCESS(st)) return st;
    if (V->V1) {
        st = RfsMountV1(V);
        if (!RFS_SUCCESS(st)) { RfsUnmount(V); return st; }
        V->ReadOnly = TRUE;
        if (ReadOnly || V->ReadOnlyForced) return RFS_OK;
        if (V->Minor != 2) V->ReadOnlyWhy = "only ReFS 1.2 is written here";
        else if (V->ChecksumErrors) V->ReadOnlyWhy = "checksum errors";
        if (V->ReadOnlyWhy) return RFS_OK;
        V->ReadOnly = FALSE;
        /* free clusters: the medium allocator's and the chunks the large one has not given out */
        V->FreeClusters = RfsFreeBlocksV1(V) / (V->ClusterSize / REFS1_PAGE);
        return RFS_OK;
    }
    sb = (PUCHAR)RfsHostAlloc(V->ClusterSize);
    V->Scratch = (PUCHAR)RfsHostAlloc(V->PageSize);
    if (!sb || !V->Scratch) { RfsHostFree(sb); RfsUnmount(V); return RFS_NO_MEMORY; }
    st = ReadSuper3(V, sb);
    RfsHostFree(sb);
    if (RFS_SUCCESS(st)) st = ReadCheckpoint3(V);
    if (RFS_SUCCESS(st)) st = LoadContainers(V);
    if (RFS_SUCCESS(st)) st = LoadSchemas(V);
    if (RFS_SUCCESS(st)) {
        st = RfsTableOpenRoot(V, REFS_ROOT_OBJECT, &V->Object);
        if (!RFS_SUCCESS(st)) st = RfsTableOpenRoot(V, REFS_ROOT_OBJECT2, &V->Object);
    }
    if (RFS_SUCCESS(st)) st = LoadUpcase(V);
    if (RFS_SUCCESS(st)) LoadLabel(V);
    if (!RFS_SUCCESS(st)) { RfsUnmount(V); return st; }
    V->ReadOnly = TRUE;
    if (ReadOnly || V->ReadOnlyForced) return RFS_OK;
    /* writing: the object table must be the primary copy, the format one this engine writes */
    if (V->Object->Index != REFS_ROOT_OBJECT) V->ReadOnlyWhy = "the object table is damaged";
    else if (V->Major != 3 && V->Major != 2) V->ReadOnlyWhy = "this ReFS version is read-only here";
    else if (!V->ClustersPerContainer) V->ReadOnlyWhy = "no containers";
    else if (V->ChecksumErrors) V->ReadOnlyWhy = "checksum errors";
    if (V->ReadOnlyWhy) return RFS_OK;
    V->ReadOnly = FALSE;                                /* (RfsGlobal below needs it) */
    st = RfsAllocInit(V);
    if (!RFS_SUCCESS(st)) {
        V->ReadOnly = TRUE;
        V->ReadOnlyWhy = "the allocators could not be read";
        RfsAllocDone(V);
    } else if (V->BootView) {
        /* made bootable: the boot files' window is allocated last */
        (VOID)RfsShimWindow(V, V->BootSectors, &V->AvoidLo, &V->AvoidHi);
    }
    return RFS_OK;
}

VOID
RfsUnmount(PRFS_VOL V)
{
    ULONG i;
    for (i = 0; i < REFS3_NROOTS; i++) {
        if (V->Global[i] && V->Global[i] != V->Object) RfsTableClose(V, V->Global[i]);
        V->Global[i] = NULL;
    }
    while (V->DirtyHead) {
        PRFS_TABLE t = V->DirtyHead;
        V->DirtyHead = t->DirtyNext;
        t->OnDirtyList = FALSE;
        t->Dirty = FALSE;
        RfsTableClose(V, t);
    }
    RfsAllocDone(V);
    if (V->Object) { RfsTableClose(V, V->Object); V->Object = NULL; }
    if (V->Schema) { RfsTableClose(V, V->Schema); V->Schema = NULL; }
    for (i = 0; i < RFS_TABLE_HASH; i++) {
        while (V->TableHash[i]) {
            PRFS_TABLE t = V->TableHash[i];
            V->TableHash[i] = t->HashNext;
            if (t->Root) RfsNodeFree(t->Root);
            RfsHostFree(t);
        }
    }
    V->TableLruHead = V->TableLruTail = NULL;
    V->TableCount = 0;
    RfsPageDropAll(V);
    if (V->Chkp) RfsHostFree(V->Chkp);
    if (V->ContPhys) RfsHostFree(V->ContPhys);
    if (V->Upcase) RfsHostFree(V->Upcase);
    if (V->Scratch) RfsHostFree(V->Scratch);
    V->Chkp = NULL; V->ContPhys = NULL; V->Upcase = NULL; V->Scratch = NULL;
}

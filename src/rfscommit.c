/*
 * REFSNT - the commit: changed B+ tree nodes go to new pages and a new
 * checkpoint makes them the volume.
 *
 *  0. every changed object table's row in the object tables (roots 0 and 5,
 *     kept identical) is marked changed: it will get the table's new root
 *  1. every changed node gets a new page (old pages are freed: quarantined
 *     until step 4 is done); the allocator tables change while this happens,
 *     so their own nodes are placed last, until nothing new is left
 *  2. nodes are encoded bottom up, each page's checksum going into its
 *     parent's reference; object tables first (their roots go into the
 *     object table rows), the global tables after them
 *  3. the pages are flushed
 *  4. the checkpoint slot not in use gets the new roots, clock + 1 and its
 *     self checksum, and is flushed: the volume is now the new one
 */
#include "rfseng.h"

/* writes a page (copy-on-write: a new location) */
RFS_STATUS
RfsWritePage(PRFS_VOL V, const RFS_REF *Where, BOOLEAN Real, const UCHAR *Page)
{
    ULONG i;
    RFS_STATUS st;
    if (V->V1) {
        RfsPageForget(V, Where->Lcn[0]);
        return RfsHostWrite(V, Where->Lcn[0] << REFS1_BLOCK_SHIFT, REFS1_PAGE, Page);
    }
    for (i = 0; i < V->ClustersPerPage; ) {
        ULONGLONG p0, pn;
        ULONG n = 1, k;
        if (Real) p0 = Where->Lcn[i];
        else { st = RfsTranslate(V, Where->Lcn[i], &p0, NULL); if (!RFS_SUCCESS(st)) return st; }
        while (i + n < V->ClustersPerPage) {
            if (Real) pn = Where->Lcn[i + n];
            else if (!RFS_SUCCESS(RfsTranslate(V, Where->Lcn[i + n], &pn, NULL))) break;
            if (pn != p0 + n) break;
            n++;
        }
        for (k = 0; k < n; k++) RfsPageForget(V, p0 + k);
        st = RfsHostWrite(V, p0 << V->ClusterShift, n << V->ClusterShift, Page + (i << V->ClusterShift));
        if (!RFS_SUCCESS(st)) return st;
        i += n;
    }
    return RFS_OK;
}

/* ---- placing ---------------------------------------------------------------------------------------- */
/* new pages for the dirty nodes of T not placed yet; *Placed counts them */
static RFS_STATUS
Place(PRFS_TABLE T, PRFS_NODE N, PULONG Placed)
{
    PRFS_VOL V = T->Vol;
    ULONG i;
    RFS_STATUS st;
    if (!N->Dirty) return RFS_OK;
    if (N->Level) {
        for (i = 0; i < N->NRows; i++) {
            if (N->Rows[i].Child && N->Rows[i].Child->Dirty) {
                st = Place(T, N->Rows[i].Child, Placed);
                if (!RFS_SUCCESS(st)) return st;
            }
        }
    }
    if (N->Placed) return RFS_OK;
    if (N->HasRef) {
        st = V->V1 ? RfsFreePageV1(V, &N->Ref) : RfsFreePage(V, T->Tier, T->Real, &N->Ref);
        if (!RFS_SUCCESS(st)) return st;
        N->HasRef = FALSE;
    }
    st = V->V1 ? RfsAllocPageV1(V, &N->NewRef) : RfsAllocPage(V, T->Tier, T->Real, &N->NewRef);
    if (!RFS_SUCCESS(st)) return st;
    N->Placed = TRUE;
    (*Placed)++;
    return RFS_OK;
}

/* ---- encoding ---------------------------------------------------------------------------------------- */
static RFS_STATUS
Encode(PRFS_TABLE T, PRFS_NODE N, ULONGLONG Clock, ULONGLONG TreeClock, PRFS_REF Out)
{
    PRFS_VOL V = T->Vol;
    ULONG i;
    RFS_STATUS st;
    if (N->Level) {
        for (i = 0; i < N->NRows; i++) {
            PRFS_ROW r = &N->Rows[i];
            RFS_REF ref;
            if (!r->Child || !r->Child->Dirty) continue;
            st = Encode(T, r->Child, Clock, TreeClock, &ref);
            if (!RFS_SUCCESS(st)) return st;
            st = RfsRowOwn(r);
            if (!RFS_SUCCESS(st)) return st;
            if (r->ValLen < RFS_TREEREF(V)) return RFS_CORRUPT;
            RfsRefPut(V, r->Val, &ref);
        }
    }
    if (!N->Placed) return RFS_CORRUPT;
    st = RfsEncodePage(T, N, &N->NewRef, Clock, TreeClock, V->Scratch, Out);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsWritePage(V, &N->NewRef, T->Real, V->Scratch);
    if (!RFS_SUCCESS(st)) return st;
    RfsCopy(N->PageHdr, V->Scratch, V->HeaderSize);
    N->Ref = *Out;
    N->HasRef = TRUE;
    N->Placed = FALSE;
    N->Dirty = FALSE;
    return RFS_OK;
}

/* the object table rows of Oid: the new root (in both copies) */
static RFS_STATUS
SetObjectRoot(PRFS_VOL V, ULONGLONG Oid, const RFS_REF *Ref, BOOLEAN TouchOnly)
{
    static const ULONG roots[2] = { REFS_ROOT_OBJECT, REFS_ROOT_OBJECT2 };
    ULONG k;
    for (k = 0; k < (ULONG)(V->V1 ? 1 : 2); k++) {
        PRFS_TABLE T;
        RFS_CURSOR c;
        UCHAR key[16];
        PUCHAR val;
        RFS_STATUS st = RfsGlobal(V, roots[k], &T);
        if (!RFS_SUCCESS(st)) return st;
        RfsZero(key, 16);
        RfsPut64(key + 8, Oid);
        st = RfsLookup(T, key, 16, &c);
        if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st == RFS_NOT_FOUND ? RFS_CORRUPT : st; }
        st = RfsTreeModifyAt(&c, &val);
        if (RFS_SUCCESS(st) && !TouchOnly) {
            PRFS_ROW r = RfsCurRow(&c);
            if (V->V1) RfsRefPut(V, val, Ref);
            else if (r->ValLen >= 0x20 + V->RefSize) RfsRefPut(V, val + 0x20, Ref);
            else st = RFS_CORRUPT;
        }
        RfsCursorClose(&c);
        if (!RFS_SUCCESS(st)) return st;
    }
    return RFS_OK;
}

/* the checkpoint: a copy of the current one with the new roots and clocks, to the other slot */
static RFS_STATUS
WriteCheckpoint3(PRFS_VOL V, ULONGLONG Clock)
{
    PUCHAR p = (PUCHAR)RfsHostAlloc(V->PageSize);
    ULONG slot = V->ChkpSlot ^ 1, i, so, sl;
    ULONGLONG lcn = V->ChkpLcn[slot];
    RFS_REF self;
    RFS_STATUS st;
    UCHAR ck[32];
    if (!p) return RFS_NO_MEMORY;
    RfsCopy(p, V->Chkp, V->PageSize);
    RfsPut64(p + REFS3_PH_CLOCK, Clock);
    RfsPut64(p + REFS3_PH_LCN, lcn);
    RfsPut64(p + REFS3_CP_CLOCK, Clock);
    if (V->AllocChanged) RfsPut64(p + REFS3_CP_ALLOCCLOCK, Clock);
    for (i = 0; i < V->NRoots; i++) RfsRefPut(V, p + V->RootOff[i], &V->Roots[i]);
    so = RfsGet32(p + REFS3_CP_SELF_OFF);
    sl = RfsGet32(p + REFS3_CP_REFSIZE);
    if (so + sl > V->ClusterSize) { RfsHostFree(p); return RFS_CORRUPT; }
    RfsRefGet(V, p + so, &self);
    RfsZero(p + so, sl);
    self.Lcn[0] = lcn;
    self.Lcn[1] = self.Lcn[2] = self.Lcn[3] = 0;
    if (!self.CkType) self.CkType = V->SelfCkType ? V->SelfCkType : REFS_CK_CRC32C;
    self.CkLen = (UCHAR)RfsChecksum(self.CkType, p, V->ClusterSize, ck);
    RfsCopy(self.Ck, ck, self.CkLen);
    RfsRefPut(V, p + so, &self);
    st = RfsHostWrite(V, lcn << V->ClusterShift, V->PageSize, p);
    if (RFS_SUCCESS(st)) st = RfsHostFlush(V);
    if (!RFS_SUCCESS(st)) { RfsHostFree(p); return st; }
    RfsHostFree(V->Chkp);
    V->Chkp = p;
    V->ChkpSlot = slot;
    V->Clock = Clock;
    if (V->AllocChanged) V->AllocClock = Clock;
    return RFS_OK;
}

static BOOLEAN
IsAllocTable(PRFS_TABLE T)
{
    return (BOOLEAN)(T->Tier == RFS_TIER_CONTAINER);
}

RFS_STATUS
RfsCommit(PRFS_VOL V)
{
    PRFS_TABLE T;
    RFS_STATUS st = RFS_OK;
    ULONGLONG clock = V->Clock + 1;
    ULONG placed, rounds;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (!V->Dirty || !V->DirtyHead) { V->Dirty = FALSE; return RFS_OK; }
    if (V->V1) return RfsCommitV1(V);
    V->InCommit = TRUE;

    /* 0. object rows of changed object tables */
    for (T = V->DirtyHead; T; T = T->DirtyNext) {
        if (T->Where == RFS_TROOT_OBJECT && T->Dirty) {
            st = SetObjectRoot(V, T->Oid, NULL, TRUE);
            if (!RFS_SUCCESS(st)) goto out;
        }
    }
    /* 1. pages: the ordinary tables, then the allocators until they stop changing */
    placed = 0;
    for (T = V->DirtyHead; T; T = T->DirtyNext) {
        if (T->Dirty && !IsAllocTable(T)) {
            st = Place(T, T->Root, &placed);
            if (!RFS_SUCCESS(st)) goto out;
        }
    }
    for (rounds = 0; rounds < 64; rounds++) {
        placed = 0;
        for (T = V->DirtyHead; T; T = T->DirtyNext) {
            if (T->Dirty && IsAllocTable(T)) {
                st = Place(T, T->Root, &placed);
                if (!RFS_SUCCESS(st)) goto out;
            }
        }
        /* an ordinary table changed by the allocators (none should) */
        for (T = V->DirtyHead; T; T = T->DirtyNext) {
            if (T->Dirty && !IsAllocTable(T)) {
                st = Place(T, T->Root, &placed);
                if (!RFS_SUCCESS(st)) goto out;
            }
        }
        if (!placed) break;
    }
    if (rounds == 64) { st = RFS_CORRUPT; goto out; }

    /* 2. encoding: object tables, their rows, the rest */
    for (T = V->DirtyHead; T; T = T->DirtyNext) {
        RFS_REF ref;
        if (!T->Dirty || T->Where != RFS_TROOT_OBJECT) continue;
        st = Encode(T, T->Root, clock, T->TreeClock + 1, &ref);
        if (!RFS_SUCCESS(st)) goto out;
        T->TreeClock++;
        T->RootRef = ref;
        st = SetObjectRoot(V, T->Oid, &ref, FALSE);
        if (!RFS_SUCCESS(st)) goto out;
    }
    for (T = V->DirtyHead; T; T = T->DirtyNext) {
        RFS_REF ref;
        if (!T->Dirty || T->Where != RFS_TROOT_CHECKPOINT) continue;
        if (!T->Root->Dirty) { T->Dirty = FALSE; continue; }
        st = Encode(T, T->Root, clock, T->TreeClock, &ref);
        if (!RFS_SUCCESS(st)) goto out;
        T->RootRef = ref;
        V->Roots[T->Index] = ref;
    }
    /* 3. 4. */
    st = RfsHostFlush(V);
    if (RFS_SUCCESS(st)) st = WriteCheckpoint3(V, clock);
    if (!RFS_SUCCESS(st)) goto out;

    /* done: the quarantine is over, the tables are clean */
    RfsQuarantineClear(V);
    V->AllocChanged = FALSE;
    while ((T = V->DirtyHead) != NULL) {
        V->DirtyHead = T->DirtyNext;
        T->DirtyNext = NULL;
        T->OnDirtyList = FALSE;
        T->Dirty = FALSE;
        RfsTableClose(V, T);
    }
    V->Dirty = FALSE;
out:
    V->InCommit = FALSE;
    if (!RFS_SUCCESS(st)) {
        /* the volume on disk is still the previous checkpoint; the changes in memory cannot be trusted */
        V->ReadOnly = TRUE;
        V->ReadOnlyWhy = "a commit failed";
    }
    return st;
}

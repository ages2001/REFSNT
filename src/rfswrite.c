/*
 * REFSNT - changing B+ trees (copy-on-write) and encoding nodes.
 *
 * A change loads the path from the root to the leaf, marks every node on it
 * dirty (each will be written to a new page at the commit, so each parent's
 * reference changes too), and changes the rows in memory. A node that grows
 * past its page is split in two (the new index row's key is the left half's
 * last key: an upper bound); a root that splits gets a level more. A node
 * that empties is removed from its parent; a root index left with one child
 * takes that child's place. The last row of every index node is kept
 * without a key (no upper bound), as ReFS 3.10 and later write them.
 *
 * Embedded tables (a file's table in its name row, a stream's extents in
 * the $DATA row) are changed the same way and then encoded back into the
 * value of the row that holds them (RfsEmbeddedStore).
 */
#include "rfseng.h"

#define ALIGN8(x)   (((x) + 7) & ~7UL)

/* ---- rows ------------------------------------------------------------------------------------- */
static RFS_STATUS
RowAssign(PRFS_ROW R, const UCHAR *Key, ULONG KeyLen, const UCHAR *Val, ULONG ValLen, USHORT Flags)
{
    PUCHAR b = (PUCHAR)RfsHostAlloc(KeyLen + ValLen + 1);
    if (!b) return RFS_NO_MEMORY;
    if (KeyLen) RfsCopy(b, Key, KeyLen);
    if (ValLen && Val) RfsCopy(b + KeyLen, Val, ValLen);
    else if (ValLen) RfsZero(b + KeyLen, ValLen);
    if (R->Owned) RfsHostFree(R->Key ? R->Key : R->Val);
    R->Key = KeyLen ? b : NULL;
    R->Val = b + KeyLen;
    if (!R->Key) R->Val = b;
    R->KeyLen = (USHORT)KeyLen;
    R->ValLen = ValLen;
    R->Flags = Flags;
    R->Owned = TRUE;
    R->Overlap = (UCHAR)(KeyLen && ValLen >= KeyLen && Val && RfsCmp(Key, Val, KeyLen) == 0);
    return RFS_OK;
}

/* the row's own copy of its bytes (before changing them in place) */
RFS_STATUS
RfsRowOwn(PRFS_ROW R)
{
    PUCHAR b;
    if (R->Owned) return RFS_OK;
    b = (PUCHAR)RfsHostAlloc(R->KeyLen + R->ValLen + 1);
    if (!b) return RFS_NO_MEMORY;
    if (R->KeyLen) RfsCopy(b, R->Key, R->KeyLen);
    RfsCopy(b + R->KeyLen, R->Val, R->ValLen);
    R->Key = R->KeyLen ? b : NULL;
    R->Val = R->KeyLen ? b + R->KeyLen : b;
    R->Owned = TRUE;
    return RFS_OK;
}

static RFS_STATUS
NodeGrowRows(PRFS_NODE N, ULONG Need)
{
    PRFS_ROW n;
    ULONG cap;
    if (Need <= N->CapRows) return RFS_OK;
    cap = N->CapRows ? N->CapRows * 2 : 8;
    while (cap < Need) cap *= 2;
    n = (PRFS_ROW)RfsAllocZero(cap * sizeof(RFS_ROW));
    if (!n) return RFS_NO_MEMORY;
    if (N->Rows) { RfsCopy(n, N->Rows, N->NRows * sizeof(RFS_ROW)); RfsHostFree(N->Rows); }
    N->Rows = n;
    N->CapRows = cap;
    return RFS_OK;
}

/* inserts an empty row at Slot */
static RFS_STATUS
NodeInsertSlot(PRFS_NODE N, ULONG Slot)
{
    RFS_STATUS st = NodeGrowRows(N, N->NRows + 1);
    if (!RFS_SUCCESS(st)) return st;
    RfsMove(&N->Rows[Slot + 1], &N->Rows[Slot], (N->NRows - Slot) * sizeof(RFS_ROW));
    RfsZero(&N->Rows[Slot], sizeof(RFS_ROW));
    N->Rows[Slot].Marker = 0xFFFF;
    N->NRows++;
    return RFS_OK;
}

static VOID
RowRelease(PRFS_ROW R)
{
    if (R->Child) RfsNodeFree(R->Child);
    if (R->Owned) RfsHostFree(R->Key ? R->Key : R->Val);
    R->Child = NULL;
    R->Owned = FALSE;
}

/* removes the row at Slot (the child it holds, if loaded, is released by the caller) */
static VOID
NodeRemoveSlot(PRFS_NODE N, ULONG Slot, BOOLEAN FreeChild)
{
    PRFS_ROW r = &N->Rows[Slot];
    if (!FreeChild) r->Child = NULL;
    RowRelease(r);
    RfsMove(&N->Rows[Slot], &N->Rows[Slot + 1], (N->NRows - Slot - 1) * sizeof(RFS_ROW));
    N->NRows--;
}

/* ---- sizes ------------------------------------------------------------------------------------- */
static BOOLEAN
IsBare(PRFS_TABLE T, PRFS_NODE N)
{
    return RfsNodeBare(T, N);
}

/* where a row's value goes: sharing the key's bytes when it begins with the key or is its tail */
static VOID
RowLayout(const RFS_ROW *R, PULONG ValOff, PULONG Size)
{
    if (R->KeyLen && R->ValLen >= R->KeyLen && RfsCmp(R->Key, R->Val, R->KeyLen) == 0) {
        *ValOff = REFS_RH_LENGTH;
        *Size = ALIGN8(REFS_RH_LENGTH + R->ValLen);
    } else if (R->ValLen && R->KeyLen >= R->ValLen && RfsCmp(R->Key + R->KeyLen - R->ValLen, R->Val, R->ValLen) == 0) {
        *ValOff = REFS_RH_LENGTH + R->KeyLen - R->ValLen;
        *Size = ALIGN8(REFS_RH_LENGTH + R->KeyLen);
    } else {
        *ValOff = ALIGN8(REFS_RH_LENGTH + R->KeyLen);
        *Size = ALIGN8(*ValOff + R->ValLen);
    }
}

static ULONG
RowSize(PRFS_TABLE T, PRFS_NODE N, PRFS_ROW R)
{
    ULONG vo, sz;
    if (IsBare(T, N)) return ALIGN8(R->ValLen);
    RowLayout(R, &vo, &sz);
    return sz;
}

/* the bytes the node takes: node header, rows, key index (descriptor not counted) */
ULONG
RfsNodeSize(PRFS_TABLE T, PRFS_NODE N)
{
    ULONG s = REFS_NH_SIZE + 4 * N->NRows, i;
    for (i = 0; i < N->NRows; i++) s += RowSize(T, N, &N->Rows[i]);
    return s;
}

/* the room a node has */
static ULONG
Capacity(PRFS_TABLE T, PRFS_NODE N)
{
    PRFS_VOL V = T->Vol;
    if (N->IsRoot && T->Where == RFS_TROOT_EMBEDDED) return RFS_EMBEDDED_MAX;
    return V->PageSize - V->HeaderSize - (N->IsRoot ? N->DescLen : 8);
}

/* ---- encoding ---------------------------------------------------------------------------------- */
/*
 * Writes node N at Out + At (descriptor or the 8-byte offset field there),
 * rows from the node header on, the key index at Out + End (pages: the page
 * end; embedded tables: right after the rows).
 */
static RFS_STATUS
Encode(PRFS_TABLE T, PRFS_NODE N, PUCHAR Out, ULONG At, ULONG End)
{
    PRFS_VOL V = T->Vol;
    ULONG hdr, pos, i, ks;
    BOOLEAN bare = IsBare(T, N);
    PUCHAR h;
    if (N->IsRoot && N->Desc) {
        RfsCopy(Out + At, N->Desc, N->DescLen);
        RfsPut32(Out + At, N->DescLen);
        hdr = At + N->DescLen;
    } else {
        RfsPut32(Out + At, 8);
        RfsPut32(Out + At + 4, 0);
        hdr = At + 8;
    }
    h = Out + hdr;
    RfsZero(h, REFS_NH_SIZE);
    pos = (V->V1 || V->V2) ? REFS1_NH_SIZE : REFS_NH_SIZE;   /* 1.x and 2.x: 0x20-byte node header */
    ks = End - hdr - 4 * N->NRows;
    if (hdr + pos > End || ks < pos) return RFS_CORRUPT;
    for (i = 0; i < N->NRows; i++) {
        PRFS_ROW r = &N->Rows[i];
        ULONG sz = RowSize(T, N, r);
        USHORT marker;
        if (pos + sz > ks) return RFS_CORRUPT;
        RfsZero(h + pos, sz);
        if (bare) {
            ULONG vcn = r->ValLen >= REFS_EXT_LENGTH ? RfsGet32(r->Val + REFS_EXT_VCN) : 0;
            RfsCopy(h + pos, r->Val, r->ValLen);
            /* records in a root leaf carry their file cluster; elsewhere no hint */
            marker = (USHORT)(N->IsRoot ? (vcn < 0xFFFF ? vcn : 0xFFFF) : 0xFFFF);
        } else {
            ULONG vo, sz2;
            RowLayout(r, &vo, &sz2);
            RfsPut32(h + pos + REFS_RH_SIZE, sz);
            RfsPut16(h + pos + REFS_RH_KEYOFF, REFS_RH_LENGTH);
            RfsPut16(h + pos + REFS_RH_KEYLEN, r->KeyLen);
            RfsPut16(h + pos + REFS_RH_FLAGS, r->Flags);
            RfsPut16(h + pos + REFS_RH_VALOFF, (USHORT)vo);
            if (V->V1) RfsPut32(h + pos + REFS_RH_VALLEN, r->ValLen);
            else RfsPut16(h + pos + REFS_RH_VALLEN, (USHORT)r->ValLen);
            RfsCopy(h + pos + vo, r->Val, r->ValLen);
            if (r->KeyLen) RfsCopy(h + pos + REFS_RH_LENGTH, r->Key, r->KeyLen);
            marker = 0xFFFF;
        }
        RfsPut32(h + ks + 4 * i, pos | ((ULONG)marker << 16));
        pos += sz;
    }
    RfsPut32(h + REFS_NH_DATASTART, (V->V1 || V->V2) ? REFS1_NH_SIZE : REFS_NH_SIZE);
    RfsPut32(h + REFS_NH_DATAEND, pos);
    RfsPut32(h + REFS_NH_FREE, ks - pos);
    h[REFS_NH_LEVEL] = N->Level;
    h[REFS_NH_FLAGS] = N->Flags;
    RfsPut32(h + REFS_NH_KEYSTART, ks);
    RfsPut32(h + REFS_NH_COUNT, N->NRows);
    if (V->V1 || V->V2) {
        RfsPut32(h + REFS1_NH_KEYEND, ks + 4 * N->NRows);
        RfsPut32(h + 0x1C, 0);
    } else {
        RfsPut32(h + 0x18, N->NodeHdr[6]);
        RfsPut32(h + 0x1C, N->NodeHdr[7]);
        RfsPut32(h + REFS_NH_KEYEND, ks + 4 * N->NRows);
    }
    return RFS_OK;
}

/* a page: header and node; Ref (the new location) gets the page's checksum */
RFS_STATUS
RfsEncodePage(PRFS_TABLE T, PRFS_NODE N, const RFS_REF *Where, ULONGLONG Clock, ULONGLONG TreeClock, PUCHAR Page, PRFS_REF Out)
{
    PRFS_VOL V = T->Vol;
    RFS_STATUS st;
    ULONG i;
    RfsZero(Page, V->PageSize);
    if (V->V1) {
        RfsCopy(Page, N->PageHdr, REFS1_PH_SIZE);
        RfsPut64(Page + REFS1_PH_BLOCK, Where->Lcn[0]);
        RfsPut64(Page + REFS1_PH_CLOCK, Clock);
        RfsPut64(Page + REFS1_PH_TABLEHI, 0);
        if (T->Oid) RfsPut64(Page + REFS1_PH_TABLE, T->Oid);
    } else {
        RfsCopy(Page, N->PageHdr, REFS3_PH_SIZE);
        RfsCopy(Page, "MSB+", 4);
        RfsPut32(Page + REFS3_PH_VERSION, 2);
        RfsPut32(Page + 8, 0);
        RfsPut32(Page + REFS3_PH_VOLSIG, V->VolSig);
        RfsPut64(Page + REFS3_PH_CLOCK, Clock);
        RfsPut64(Page + REFS3_PH_TREECLOCK, TreeClock);
        for (i = 0; i < 4; i++) RfsPut64(Page + REFS3_PH_LCN + 8 * i, i < V->ClustersPerPage ? Where->Lcn[i] : 0);
        RfsPut64(Page + REFS3_PH_TABLEHI, 0);
        if (T->Oid) RfsPut64(Page + REFS3_PH_TABLE, T->Oid);
    }
    st = Encode(T, N, Page, V->HeaderSize, V->PageSize);
    if (!RFS_SUCCESS(st)) return st;
    *Out = *Where;
    Out->Flags = 0;
    Out->CkType = V->CkType;
    Out->CkLen = (UCHAR)RfsChecksum(V->CkType, Page, V->PageSize, Out->Ck);
    return RFS_OK;
}

/* ---- dirty marking ------------------------------------------------------------------------------- */
static VOID
MarkPathDirty(PRFS_CURSOR C)
{
    ULONG d;
    for (d = 0; d <= C->Depth; d++) C->Node[d]->Dirty = TRUE;
    RfsTableDirty(C->Table);
}

/* descends to the leaf where Key belongs; Slot = its insertion position */
static RFS_STATUS
PathTo(PRFS_TABLE T, const UCHAR *Key, ULONG KeyLen, PRFS_CURSOR C)
{
    RFS_STATUS st = RfsSeekLeaf(T, Key, KeyLen, C);
    return st;
}

static VOID
DescAdd(PRFS_TABLE T, LONG Rows, LONG Leaves)
{
    PRFS_NODE r = T->Root;
    if (!r->Desc || r->DescLen < REFS_IR_DATA) return;
    if (Rows) RfsPut64(r->Desc + REFS_IR_ROWS, RfsGet64(r->Desc + REFS_IR_ROWS) + (LONGLONG)Rows);
    if (Leaves) RfsPut64(r->Desc + REFS_IR_LEAVES, RfsGet64(r->Desc + REFS_IR_LEAVES) + (LONGLONG)Leaves);
}

/* the separator for a node: its last key (range tables: {last unit, 1}) */
static RFS_STATUS
Separator(PRFS_TABLE T, PRFS_NODE N, PUCHAR Key, PULONG KeyLen)
{
    PRFS_ROW r = &N->Rows[N->NRows - 1];
    if (IsBare(T, N) || ((T->Rule == 3 || T->Rule == 0x0B) && r->KeyLen >= 16)) {
        UCHAR k[16];
        ULONGLONG s, l;
        if (IsBare(T, N)) RfsRecordKey(T->Rule, r, k);
        else RfsCopy(k, r->Key, 16);
        s = RfsGet64(k); l = RfsGet64(k + 8);
        if (N->Level && r->KeyLen >= 16) l = RfsGet64(r->Key + 8);
        RfsZero(Key, 16);
        RfsPut64(Key, s + (l ? l : 1) - 1);
        RfsPut64(Key + 8, 1);
        if (N->Level) RfsCopy(Key, r->Key, 16);
        *KeyLen = 16;
        return RFS_OK;
    }
    if (r->KeyLen > RFS_MAX_KEY) return RFS_CORRUPT;
    RfsCopy(Key, r->Key, r->KeyLen);
    *KeyLen = r->KeyLen;
    return RFS_OK;
}

/* a new node like Like (same level and flags, not a root) */
static PRFS_NODE
NewNode(PRFS_TABLE T, PRFS_NODE Like)
{
    PRFS_NODE n = (PRFS_NODE)RfsAllocZero(sizeof(RFS_NODE));
    if (!n) return NULL;
    n->Table = T;
    n->Level = Like->Level;
    n->Flags = (UCHAR)(Like->Flags & ~REFS_NF_ROOT);
    n->Dirty = TRUE;
    RfsCopy(n->PageHdr, Like->PageHdr, sizeof(n->PageHdr));
    return n;
}

/* moves rows [From, end) of N into M */
static RFS_STATUS
MoveRows(PRFS_NODE N, ULONG From, PRFS_NODE M)
{
    ULONG n = N->NRows - From, i;
    RFS_STATUS st = NodeGrowRows(M, M->NRows + n);
    if (!RFS_SUCCESS(st)) return st;
    for (i = From; i < N->NRows; i++) {
        PRFS_ROW r = &N->Rows[i];
        st = RfsRowOwn(r);      /* the bytes must outlive N's buffer */
        if (!RFS_SUCCESS(st)) return st;
        M->Rows[M->NRows++] = *r;
    }
    N->NRows = From;
    return RFS_OK;
}

/* an index row pointing at Child, with Key (Key NULL: the last child, no key) */
static RFS_STATUS
SetIndexRow(PRFS_TABLE T, PRFS_ROW R, const UCHAR *Key, ULONG KeyLen, PRFS_NODE Child)
{
    PRFS_VOL V = T->Vol;
    RFS_STATUS st = RowAssign(R, Key, Key ? KeyLen : 0, NULL, RFS_TREEREF(V), (USHORT)(Key ? 0 : REFS_RF_LASTCHILD));
    if (!RFS_SUCCESS(st)) return st;
    R->Overlap = FALSE;
    R->Child = Child;
    R->Marker = 0xFFFF;
    if (Child->HasRef) RfsRefPut(V, R->Val, &Child->Ref);
    return RFS_OK;
}

/* where to cut N: about half of its bytes on each side, at least one row each */
static ULONG
SplitPoint(PRFS_TABLE T, PRFS_NODE N)
{
    ULONG total = 0, acc = 0, i;
    for (i = 0; i < N->NRows; i++) total += RowSize(T, N, &N->Rows[i]) + 4;
    for (i = 0; i + 1 < N->NRows; i++) {
        acc += RowSize(T, N, &N->Rows[i]) + 4;
        if (acc * 2 >= total) return i + 1;
    }
    return N->NRows - 1;
}

/* the nodes on C's path that no longer fit are split, from the leaf up */
static RFS_STATUS
Rebalance(PRFS_TABLE T, PRFS_CURSOR C)
{
    LONG d;
    RFS_STATUS st;
    for (d = (LONG)C->Depth; d >= 0; d--) {
        PRFS_NODE N = C->Node[d], L, R;
        UCHAR key[RFS_MAX_KEY];
        ULONG kl, cut;
        if (RfsNodeSize(T, N) <= Capacity(T, N)) continue;
        if (N->NRows < 2) return RFS_NOT_SUPPORTED;       /* one row that does not fit a node */
        cut = SplitPoint(T, N);
        if (d == 0) {
            /* the root keeps its place (descriptor); its rows go to two new children */
            BOOLEAN leaf = (BOOLEAN)(N->Level == 0);
            L = NewNode(T, N);
            R = NewNode(T, N);
            if (!L || !R) { RfsNodeFree(L); RfsNodeFree(R); return RFS_NO_MEMORY; }
            st = MoveRows(N, cut, R);
            if (RFS_SUCCESS(st)) st = MoveRows(N, 0, L);
            if (RFS_SUCCESS(st)) st = Separator(T, L, key, &kl);
            if (!RFS_SUCCESS(st)) { RfsNodeFree(L); RfsNodeFree(R); return st; }
            N->Level++;
            N->Flags |= REFS_NF_INDEX;
            N->NodeHdr[6] = N->NodeHdr[7] = 0;
            st = NodeGrowRows(N, 2);
            if (!RFS_SUCCESS(st)) return st;
            RfsZero(N->Rows, 2 * sizeof(RFS_ROW));
            N->NRows = 2;
            st = SetIndexRow(T, &N->Rows[0], key, kl, L);
            if (RFS_SUCCESS(st)) st = SetIndexRow(T, &N->Rows[1], NULL, 0, R);
            if (!RFS_SUCCESS(st)) return st;
            if (leaf) DescAdd(T, 0, 2);
            /* the halves may still be too big (an embedded root's limit is below a page's) */
            C->Depth = 0;
            return RFS_OK;
        }
        /* N keeps the left half; R takes the right one and N's place in the parent */
        R = NewNode(T, N);
        if (!R) return RFS_NO_MEMORY;
        st = MoveRows(N, cut, R);
        if (RFS_SUCCESS(st)) st = Separator(T, N, key, &kl);
        if (!RFS_SUCCESS(st)) { RfsNodeFree(R); return st; }
        {
            PRFS_NODE P = C->Node[d - 1];
            ULONG s = C->Slot[d - 1];
            st = NodeInsertSlot(P, s);
            if (!RFS_SUCCESS(st)) { RfsNodeFree(R); return st; }
            /* row s+1 (N's old row, its key the bound of both halves) now points at R */
            P->Rows[s + 1].Child = R;
            st = SetIndexRow(T, &P->Rows[s], key, kl, N);
            if (!RFS_SUCCESS(st)) return st;
        }
        if (N->Level == 0) DescAdd(T, 0, 1);
    }
    return RFS_OK;
}

/* ---- the operations ----------------------------------------------------------------------------- */
RFS_STATUS
RfsTreeInsert(PRFS_TABLE T, const UCHAR *Key, ULONG KeyLen, const UCHAR *Val, ULONG ValLen, USHORT Flags, BOOLEAN AllowEqual)
{
    RFS_CURSOR c;
    PRFS_NODE leaf;
    ULONG s;
    RFS_STATUS st;
    if (T->Vol->ReadOnly) return RFS_READ_ONLY;
    if (KeyLen > RFS_MAX_KEY) return RFS_NOT_SUPPORTED;
    st = PathTo(T, Key, KeyLen, &c);
    if (!RFS_SUCCESS(st)) return st;
    leaf = c.Node[c.Depth];
    s = c.Slot[c.Depth];
    if (!AllowEqual && s < leaf->NRows && RfsKeyCompare(T->Vol, T->Rule, leaf->Rows[s].Key, leaf->Rows[s].KeyLen, Key, KeyLen) == 0) {
        RfsCursorClose(&c);
        return RFS_EXISTS;
    }
    if (AllowEqual) {
        while (s < leaf->NRows && RfsKeyCompare(T->Vol, T->Rule, leaf->Rows[s].Key, leaf->Rows[s].KeyLen, Key, KeyLen) == 0) s++;
    }
    MarkPathDirty(&c);
    st = NodeInsertSlot(leaf, s);
    if (RFS_SUCCESS(st)) st = RowAssign(&leaf->Rows[s], Key, KeyLen, Val, ValLen, Flags);
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st; }
    if (IsBare(T, leaf)) leaf->Rows[s].KeyLen = 0, leaf->Rows[s].Key = NULL;
    DescAdd(T, 1, 0);
    c.Slot[c.Depth] = s;
    st = Rebalance(T, &c);
    RfsCursorClose(&c);
    return st;
}

/* a bare record (extent tables): no key of its own */
RFS_STATUS
RfsTreeInsertRecord(PRFS_TABLE T, const UCHAR *Rec, ULONG Len)
{
    RFS_CURSOR c;
    PRFS_NODE leaf;
    UCHAR key[16];
    RFS_ROW tmp;
    ULONG s;
    RFS_STATUS st;
    if (T->Vol->ReadOnly) return RFS_READ_ONLY;
    RfsZero(&tmp, sizeof(tmp));
    tmp.Val = (PUCHAR)Rec;
    tmp.ValLen = Len;
    RfsRecordKey(T->Rule, &tmp, key);
    st = PathTo(T, key, 16, &c);
    if (!RFS_SUCCESS(st)) return st;
    leaf = c.Node[c.Depth];
    s = c.Slot[c.Depth];
    MarkPathDirty(&c);
    st = NodeInsertSlot(leaf, s);
    if (RFS_SUCCESS(st)) st = RowAssign(&leaf->Rows[s], NULL, 0, Rec, Len, 0);
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st; }
    DescAdd(T, 1, 0);
    st = Rebalance(T, &c);
    RfsCursorClose(&c);
    return st;
}

/* replaces the value of the row at the cursor (the cursor is closed) */
RFS_STATUS
RfsTreeUpdateAt(PRFS_CURSOR C, const UCHAR *Val, ULONG ValLen, USHORT Flags)
{
    PRFS_TABLE T = C->Table;
    PRFS_ROW r = RfsCurRow(C);
    RFS_STATUS st;
    UCHAR key[RFS_MAX_KEY];
    ULONG kl = r->KeyLen;
    if (T->Vol->ReadOnly) { RfsCursorClose(C); return RFS_READ_ONLY; }
    RfsCopy(key, r->Key, kl);
    MarkPathDirty(C);
    st = RowAssign(r, key, kl, Val, ValLen, Flags);
    if (RFS_SUCCESS(st)) st = Rebalance(T, C);
    RfsCursorClose(C);
    return st;
}

/* key and value of the row at the cursor (the key must sort the same); the cursor is closed */
RFS_STATUS
RfsTreeReplaceAt(PRFS_CURSOR C, const UCHAR *Key, ULONG KeyLen, const UCHAR *Val, ULONG ValLen, USHORT Flags)
{
    PRFS_TABLE T = C->Table;
    PRFS_ROW r = RfsCurRow(C);
    RFS_STATUS st;
    if (T->Vol->ReadOnly) { RfsCursorClose(C); return RFS_READ_ONLY; }
    if (KeyLen > RFS_MAX_KEY) { RfsCursorClose(C); return RFS_NOT_SUPPORTED; }
    MarkPathDirty(C);
    st = RowAssign(r, Key, KeyLen, Val, ValLen, Flags);
    if (RFS_SUCCESS(st)) st = Rebalance(T, C);
    RfsCursorClose(C);
    return st;
}

RFS_STATUS
RfsTreeUpdate(PRFS_TABLE T, const UCHAR *Key, ULONG KeyLen, const UCHAR *Val, ULONG ValLen, USHORT Flags)
{
    RFS_CURSOR c;
    RFS_STATUS st = RfsLookup(T, Key, KeyLen, &c);
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st; }
    return RfsTreeUpdateAt(&c, Val, ValLen, Flags);
}

/* the row at the cursor goes (the cursor is closed) */
RFS_STATUS
RfsTreeDeleteAt(PRFS_CURSOR C)
{
    PRFS_TABLE T = C->Table;
    LONG d;
    if (T->Vol->ReadOnly) { RfsCursorClose(C); return RFS_READ_ONLY; }
    MarkPathDirty(C);
    NodeRemoveSlot(C->Node[C->Depth], C->Slot[C->Depth], TRUE);
    DescAdd(T, -1, 0);
    /* empty nodes leave their parents */
    for (d = (LONG)C->Depth; d > 0; d--) {
        PRFS_NODE N = C->Node[d], P = C->Node[d - 1];
        ULONG s = C->Slot[d - 1];
        if (N->NRows) break;
        if (N->Level == 0) DescAdd(T, 0, -1);
        RfsTreeFreeNode(T, N);
        NodeRemoveSlot(P, s, TRUE);          /* frees N */
        if (P->NRows && s == P->NRows) {
            /* the last child went: the new last row has no bound */
            PRFS_ROW r = &P->Rows[P->NRows - 1];
            PRFS_NODE ch = r->Child;
            r->Child = NULL;
            if (!RFS_SUCCESS(RowAssign(r, NULL, 0, r->Val, RFS_TREEREF(T->Vol), REFS_RF_LASTCHILD))) { RfsCursorClose(C); return RFS_NO_MEMORY; }
            r->Child = ch;
            r->Overlap = FALSE;
        }
    }
    /* a root index with one child gives way to it */
    while (T->Root->Level > 0 && T->Root->NRows == 1) {
        PRFS_NODE R = T->Root, ch;
        RFS_STATUS st = RfsLoadChild(T->Vol, T, R, 0, &ch);
        ULONG i;
        if (!RFS_SUCCESS(st)) { RfsCursorClose(C); return st; }
        R->Rows[0].Child = NULL;
        RowRelease(&R->Rows[0]);
        R->NRows = 0;
        if (!RFS_SUCCESS(NodeGrowRows(R, ch->NRows))) { RfsCursorClose(C); return RFS_NO_MEMORY; }
        for (i = 0; i < ch->NRows; i++) {
            if (!RFS_SUCCESS(RfsRowOwn(&ch->Rows[i]))) { RfsCursorClose(C); return RFS_NO_MEMORY; }
            R->Rows[i] = ch->Rows[i];
        }
        R->NRows = ch->NRows;
        ch->NRows = 0;
        R->Level = ch->Level;
        R->Flags = (UCHAR)(ch->Flags | REFS_NF_ROOT);
        R->NodeHdr[6] = ch->NodeHdr[6];
        R->NodeHdr[7] = ch->NodeHdr[7];
        R->Dirty = TRUE;
        if (R->Level == 0 && R->Desc && R->DescLen >= REFS_IR_DATA) RfsPut64(R->Desc + REFS_IR_LEAVES, 0);
        RfsTreeFreeNode(T, ch);
        RfsNodeFree(ch);
    }
    RfsCursorClose(C);
    return RFS_OK;
}

RFS_STATUS
RfsTreeDelete(PRFS_TABLE T, const UCHAR *Key, ULONG KeyLen)
{
    RFS_CURSOR c;
    RFS_STATUS st = RfsLookup(T, Key, KeyLen, &c);
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st; }
    return RfsTreeDeleteAt(&c);
}

/* ---- embedded tables ---------------------------------------------------------------------------- */
/* the encoded size of an embedded table's root (the row value) */
ULONG
RfsEmbeddedSize(PRFS_TABLE T)
{
    return T->Root->DescLen + RfsNodeSize(T, T->Root);
}

/*
 * Writes the dirty child pages of embedded table T now (they are not
 * reachable until the row that holds T is committed) and encodes its root
 * into a new buffer: *Val (RfsHostFree it), *Len.
 */
RFS_STATUS
RfsEmbeddedStore(PRFS_TABLE T, PUCHAR *Val, PULONG Len)
{
    RFS_STATUS st;
    ULONG n;
    PUCHAR b;
    *Val = NULL;
    st = RfsWriteDirtyChildren(T, T->Root);
    if (!RFS_SUCCESS(st)) return st;
    n = RfsEmbeddedSize(T);
    b = (PUCHAR)RfsAllocZero(n);
    if (!b) return RFS_NO_MEMORY;
    st = Encode(T, T->Root, b, 0, n);
    if (!RFS_SUCCESS(st)) { RfsHostFree(b); return st; }
    *Val = b;
    *Len = n;
    return RFS_OK;
}

/* the value of the row at the cursor, to change in place (same size); the cursor stays open */
RFS_STATUS
RfsTreeModifyAt(PRFS_CURSOR C, PUCHAR *Val)
{
    PRFS_ROW r = RfsCurRow(C);
    RFS_STATUS st;
    if (C->Table->Vol->ReadOnly) return RFS_READ_ONLY;
    st = RfsRowOwn(r);
    if (!RFS_SUCCESS(st)) return st;
    MarkPathDirty(C);
    *Val = r->Val;
    return RFS_OK;
}

/* the node's page goes back to its allocator (after the commit) */
VOID
RfsTreeFreeNode(PRFS_TABLE T, PRFS_NODE N)
{
    PRFS_VOL V = T->Vol;
    if (N->HasRef) {
        if (V->V1) RfsFreePageV1(V, &N->Ref);
        else RfsFreePage(V, T->Tier, T->Real, &N->Ref);
        N->HasRef = FALSE;
    }
    if (N->Placed) {
        if (V->V1) RfsFreePageV1(V, &N->NewRef);
        else RfsFreePage(V, T->Tier, T->Real, &N->NewRef);
        N->Placed = FALSE;
    }
}

static RFS_STATUS
FreeSubtree(PRFS_TABLE T, PRFS_NODE N)
{
    ULONG i;
    RFS_STATUS st;
    if (N->Level) {
        for (i = 0; i < N->NRows; i++) {
            PRFS_NODE ch;
            st = RfsLoadChild(T->Vol, T, N, i, &ch);
            if (!RFS_SUCCESS(st)) return st;
            st = FreeSubtree(T, ch);
            if (!RFS_SUCCESS(st)) return st;
        }
    }
    RfsTreeFreeNode(T, N);
    return RFS_OK;
}

/* every page of a table that is going away */
RFS_STATUS
RfsTreeFreeAll(PRFS_TABLE T)
{
    return FreeSubtree(T, T->Root);
}

/*
 * Writes the dirty non-root nodes under N (an embedded table's pages, which
 * only the row holding the table reaches) to new pages now, bottom up.
 */
RFS_STATUS
RfsWriteDirtyChildren(PRFS_TABLE T, PRFS_NODE N)
{
    PRFS_VOL V = T->Vol;
    ULONG i;
    RFS_STATUS st;
    if (!N->Level) return RFS_OK;
    for (i = 0; i < N->NRows; i++) {
        PRFS_ROW r = &N->Rows[i];
        PRFS_NODE ch = r->Child;
        RFS_REF where, out;
        ULONGLONG tclock;
        if (!ch || !ch->Dirty) continue;
        st = RfsWriteDirtyChildren(T, ch);
        if (!RFS_SUCCESS(st)) return st;
        RfsTreeFreeNode(T, ch);
        st = V->V1 ? RfsAllocPageV1(V, &where) : RfsAllocPage(V, T->Tier, T->Real, &where);
        if (!RFS_SUCCESS(st)) return st;
        tclock = T->Owner ? T->Owner->TreeClock + 1 : 1;
        st = RfsEncodePage(T, ch, &where, V->Clock + 1, tclock, V->Scratch, &out);
        if (RFS_SUCCESS(st)) st = RfsWritePage(V, &where, T->Real, V->Scratch);
        if (!RFS_SUCCESS(st)) return st;
        st = RfsRowOwn(r);
        if (!RFS_SUCCESS(st)) return st;
        RfsRefPut(V, r->Val, &out);
        ch->Ref = out;
        ch->HasRef = TRUE;
        ch->Dirty = FALSE;
        RfsCopy(ch->PageHdr, V->Scratch, V->HeaderSize);
    }
    return RFS_OK;
}

/*
 * A new, empty table: Desc (DescLen bytes: the index root descriptor and
 * the header data) is copied; its size field is set. Object tables join the
 * table cache (dirty: they have no page until the commit).
 */
RFS_STATUS
RfsNewTable(PRFS_VOL V, UCHAR Where, ULONGLONG Oid, ULONG Schema, const UCHAR *Desc, ULONG DescLen, PRFS_TABLE *Out)
{
    PRFS_TABLE T;
    PRFS_NODE N;
    *Out = NULL;
    T = (PRFS_TABLE)RfsAllocZero(sizeof(RFS_TABLE));
    N = (PRFS_NODE)RfsAllocZero(sizeof(RFS_NODE));
    if (!T || !N) { RfsHostFree(T); RfsHostFree(N); return RFS_NO_MEMORY; }
    N->Desc = (PUCHAR)RfsAllocZero(DescLen);
    N->Rows = (PRFS_ROW)RfsAllocZero(4 * sizeof(RFS_ROW));
    if (!N->Desc || !N->Rows) { RfsNodeFree(N); RfsHostFree(T); return RFS_NO_MEMORY; }
    N->CapRows = 4;
    RfsCopy(N->Desc, Desc, DescLen);
    RfsPut32(N->Desc, DescLen);
    RfsPut32(N->Desc + REFS_IR_SCHEMA, Schema);
    RfsPut32(N->Desc + REFS_IR_SCHEMA2, Schema);
    RfsPut64(N->Desc + REFS_IR_LEAVES, 0);
    RfsPut64(N->Desc + REFS_IR_ROWS, 0);
    N->DescLen = DescLen;
    N->IsRoot = TRUE;
    N->Flags = REFS_NF_ROOT;
    N->Dirty = TRUE;
    N->Table = T;
    if (!V->V1) {
        RfsCopy(N->PageHdr, "MSB+", 4);
        RfsPut32(N->PageHdr + REFS3_PH_VERSION, 2);
        RfsPut32(N->PageHdr + REFS3_PH_VOLSIG, V->VolSig);
        RfsPut64(N->PageHdr + REFS3_PH_TABLE, Oid);
    } else {
        RfsPut64(N->PageHdr + REFS1_PH_TABLE, Oid);
    }
    T->Vol = V;
    T->Where = Where;
    T->Oid = Oid;
    T->Schema = Schema;
    T->Rule = RfsRuleOf(V, Schema);
    T->Root = N;
    T->Refs = 1;
    T->Tier = RFS_TIER_MEDIUM;
    if (Where == RFS_TROOT_OBJECT) {
        ULONG h = (ULONG)(Oid * 0x9E3779B1UL) & (RFS_TABLE_HASH - 1);
        T->HashNext = V->TableHash[h];
        V->TableHash[h] = T;
        V->TableCount++;
        RfsTableDirty(T);
    }
    *Out = T;
    return RFS_OK;
}

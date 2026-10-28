/*
 * REFSNT - Minstore B+ trees: nodes, tables, cursors, key comparison.
 *
 * A node is a page (or, for an embedded table, a row value) holding:
 *   [index root descriptor + header data]   root nodes only
 *   node header (0x28 bytes)
 *   rows: a 16-byte row header, the key, the value (8-byte aligned)
 *         (leaf nodes of stream tables hold bare extent records instead)
 *   free space
 *   the key index: u32 a row, in key order: u16 offset from the node
 *         header, u16 marker
 * An index row's key is an upper bound of its child's keys (the last index
 * row may have no key: no bound); its value is a page reference.
 */
#include "rfseng.h"

/* ---- key comparison ----------------------------------------------------------------------------- */
static int
CmpU64s(const UCHAR *A, ULONG ALen, const UCHAR *B, ULONG BLen)
{
    ULONG o;
    for (o = 0; o + 8 <= ALen && o + 8 <= BLen; o += 8) {
        ULONGLONG a = RfsGet64(A + o), b = RfsGet64(B + o);
        if (a != b) return a < b ? -1 : 1;
    }
    /* a shorter tail: by its bytes as a little-endian number, then by length */
    {
        ULONGLONG a = 0, b = 0;
        ULONG i;
        for (i = o; i < ALen && i < o + 8; i++) a |= (ULONGLONG)A[i] << (8 * (i - o));
        for (i = o; i < BLen && i < o + 8; i++) b |= (ULONGLONG)B[i] << (8 * (i - o));
        if (a != b) return a < b ? -1 : 1;
    }
    return ALen == BLen ? 0 : (ALen < BLen ? -1 : 1);
}

static int
CmpLe(const UCHAR *A, ULONG ALen, const UCHAR *B, ULONG BLen)
{
    ULONG n = ALen > BLen ? ALen : BLen;
    while (n--) {
        UCHAR a = n < ALen ? A[n] : 0, b = n < BLen ? B[n] : 0;
        if (a != b) return a < b ? -1 : 1;
    }
    return 0;
}

int
RfsNameCompare(PRFS_VOL V, const USHORT *A, ULONG ALen, const USHORT *B, ULONG BLen)
{
    ULONG i, n = ALen < BLen ? ALen : BLen;
    for (i = 0; i < n; i++) {
        USHORT a = V->Upcase ? V->Upcase[A[i]] : A[i], b = V->Upcase ? V->Upcase[B[i]] : B[i];
        if (a != b) return a < b ? -1 : 1;
    }
    return ALen == BLen ? 0 : (ALen < BLen ? -1 : 1);
}

static int
CmpName16(PRFS_VOL V, const UCHAR *A, ULONG ALen, const UCHAR *B, ULONG BLen)
{
    ULONG i, n = (ALen < BLen ? ALen : BLen) / 2;
    for (i = 0; i < n; i++) {
        USHORT a = RfsGet16(A + 2 * i), b = RfsGet16(B + 2 * i);
        if (V->Upcase) { a = V->Upcase[a]; b = V->Upcase[b]; }
        if (a != b) return a < b ? -1 : 1;
    }
    return ALen == BLen ? 0 : (ALen < BLen ? -1 : 1);
}

/* directory tables: by row type (the low half of the first word), then per type */
static int
CmpDir(PRFS_VOL V, const UCHAR *A, ULONG ALen, const UCHAR *B, ULONG BLen)
{
    USHORT ta = ALen >= 2 ? RfsGet16(A) : 0, tb = BLen >= 2 ? RfsGet16(B) : 0;
    if (ta != tb) return ta < tb ? -1 : 1;
    if (ta == REFS_KEY_NAME && ALen >= 4 && BLen >= 4) return CmpName16(V, A + 4, ALen - 4, B + 4, BLen - 4);
    if (ALen >= 8 && BLen >= 8) return CmpU64s(A + 8, ALen - 8, B + 8, BLen - 8);
    return CmpLe(A, ALen, B, BLen);
}

/*
 * a file's attribute rows: {u64 size (not compared), u32 kind, ...}; attributes (kind
 * 0x80000001 single, 0x80000002 multi-instance; before other kinds) go on {u16 type,
 * u16 sub} and then
 * per type: $OBJ_LINK {u64 parent, u64} (then made order), named streams / $I30 by name, others
 * by their ids; other kinds (a named stream's own extent record: 3) by their bytes
 */
static int
CmpAttr(PRFS_VOL V, const UCHAR *A, ULONG ALen, const UCHAR *B, ULONG BLen)
{
    LONG ka, kb;
    USHORT ta, tb;
    if (ALen < 16 || BLen < 16) return CmpLe(A, ALen, B, BLen);
    ka = (LONG)RfsGet32(A + 8); kb = (LONG)RfsGet32(B + 8);
    if ((ka < 0) != (kb < 0)) return ka < 0 ? -1 : 1;          /* attributes first */
    if (ka >= 0) {
        if (ka != kb) return ka < kb ? -1 : 1;
        return CmpU64s(A + 12, ALen - 12, B + 12, BLen - 12);
    }
    ta = RfsGet16(A + 12); tb = RfsGet16(B + 12);
    if (ta != tb) return ta < tb ? -1 : 1;
    ta = RfsGet16(A + 14); tb = RfsGet16(B + 14);
    if (ta != tb) return ta < tb ? -1 : 1;
    switch (RfsGet16(A + 12)) {
    case REFS_AT_OBJLINK:
        /* the names of a file in one directory are kept in the order they were made */
        if (ALen >= 32 && BLen >= 32) return CmpU64s(A + 16, 16, B + 16, 16);
        break;
    case REFS_AT_NAMED:
    case REFS_AT_I30:
        return CmpName16(V, A + 16, ALen - 16, B + 16, BLen - 16);
    }
    return CmpU64s(A + 16, ALen - 16, B + 16, BLen - 16);
}

/* ranges {u64 start, u64 length}: overlapping ranges are equal (index separators are {last, 1}) */
static int
CmpRange(const UCHAR *A, ULONG ALen, const UCHAR *B, ULONG BLen)
{
    ULONGLONG as, al, bs, bl;
    if (ALen < 16 || BLen < 16) return CmpU64s(A, ALen, B, BLen);
    as = RfsGet64(A); al = RfsGet64(A + 8);
    bs = RfsGet64(B); bl = RfsGet64(B + 8);
    if (!al) al = 1;
    if (!bl) bl = 1;
    if (as + al <= bs) return -1;
    if (bs + bl <= as) return 1;
    return 0;
}

int
RfsKeyCompare(PRFS_VOL V, UCHAR Rule, const UCHAR *A, ULONG ALen, const UCHAR *B, ULONG BLen)
{
    switch (Rule) {
    case 2:
        return CmpLe(A, ALen, B, BLen);
    case 3:
        return CmpRange(A, ALen, B, BLen);
    case 6: case 8: case 0x0A:
        return CmpU64s(A, ALen, B, BLen);
    case 9:
        return CmpDir(V, A, ALen, B, BLen);
    case 0x0B:
        return CmpRange(A, ALen, B, BLen);
    case 0x0F:
        return CmpAttr(V, A, ALen, B, BLen);
    case RFS_RULE_V1ATTR:
        /* {u64 value size, u32 type, UTF-16 name}: the type, then the name */
        if (ALen >= 12 && BLen >= 12) {
            ULONG ta = RfsGet32(A + 8), tb = RfsGet32(B + 8);
            if (ta != tb) return ta < tb ? -1 : 1;
            {
                ULONG i, n = ALen < BLen ? ALen : BLen;
                for (i = 12; i + 1 < n; i += 2) {
                    USHORT ca = RfsGet16(A + i), cb = RfsGet16(B + i);
                    if (ca != cb) return ca < cb ? -1 : 1;
                }
                return ALen == BLen ? 0 : (ALen < BLen ? -1 : 1);
            }
        }
        return CmpLe(A, ALen, B, BLen);
    case 1:
        return CmpLe(A, ALen, B, BLen);
    }
    return CmpLe(A, ALen, B, BLen);
}

/* the key of a bare record (leaf rows of stream nodes): extents {file cluster, run}, others their first 16 bytes */
VOID
RfsRecordKey(UCHAR Rule, const RFS_ROW *R, PUCHAR Key)
{
    RfsZero(Key, 16);
    if (Rule == 0x0B) {
        if (R->ValLen >= REFS_EXT_LENGTH) {
            RfsPut64(Key, RfsGet32(R->Val + REFS_EXT_VCN));
            RfsPut64(Key + 8, RfsGet32(R->Val + REFS_EXT_RUN));
        }
    } else {
        RfsCopy(Key, R->Val, R->ValLen < 16 ? R->ValLen : 16);
    }
}

/* ---- nodes ----------------------------------------------------------------------------------------- */
VOID
RfsNodeFree(PRFS_NODE N)
{
    ULONG i;
    if (!N) return;
    for (i = 0; i < N->NRows; i++) {
        if (N->Rows[i].Child) RfsNodeFree(N->Rows[i].Child);
        if (N->Rows[i].Owned) RfsHostFree(N->Rows[i].Key ? N->Rows[i].Key : N->Rows[i].Val);
    }
    if (N->Rows) RfsHostFree(N->Rows);
    if (N->Buf) RfsHostFree(N->Buf);
    if (N->Desc) RfsHostFree(N->Desc);
    RfsHostFree(N);
}

/*
 * Decodes the node whose header sits at HdrOff of Buf (a copy is kept). Root:
 * the index root descriptor is at DescOff = HdrOff - its own offset field;
 * the caller passes the descriptor's position as HdrOff and Root = TRUE.
 */
RFS_STATUS
RfsNodeDecode(PRFS_VOL V, PRFS_TABLE T, const UCHAR *Src, ULONG Len, ULONG At, BOOLEAN Root, PRFS_NODE *Out)
{
    PRFS_NODE N;
    PUCHAR b, h;
    ULONG hdr, ks, ke, cnt, i, d;
    BOOLEAN bare;
    *Out = NULL;
    if (At + 8 > Len) return RFS_CORRUPT;
    d = RfsGet32(Src + At);
    hdr = At + d;
    if (d < 8 || hdr + REFS_NH_SIZE > Len) return RFS_CORRUPT;
    N = (PRFS_NODE)RfsAllocZero(sizeof(RFS_NODE));
    if (!N) return RFS_NO_MEMORY;
    b = (PUCHAR)RfsHostAlloc(Len);
    if (!b) { RfsHostFree(N); return RFS_NO_MEMORY; }
    RfsCopy(b, Src, Len);
    N->Buf = b;
    N->BufLen = Len;
    N->Table = T;
    h = b + hdr;
    for (i = 0; i < 10; i++) N->NodeHdr[i] = RfsGet32(h + 4 * i);
    N->Level = h[REFS_NH_LEVEL];
    N->Flags = h[REFS_NH_FLAGS];
    ks = RfsGet32(h + REFS_NH_KEYSTART);
    cnt = RfsGet32(h + REFS_NH_COUNT);
    ke = RfsGet32(h + ((V->V1 || V->V2) ? REFS1_NH_KEYEND : REFS_NH_KEYEND));
    if (hdr + ks + 4 * cnt > Len || (ke && ke < ks + 4 * cnt)) { RfsNodeFree(N); return RFS_CORRUPT; }
    N->IsRoot = (BOOLEAN)(Root && d > 8);
    if (N->IsRoot) {
        N->DescLen = d;
        N->Desc = (PUCHAR)RfsHostAlloc(d);
        if (!N->Desc) { RfsNodeFree(N); return RFS_NO_MEMORY; }
        RfsCopy(N->Desc, b + At, d);
    }
    /* leaf rows of extent tables (rule 0x0B) are bare records */
    {
        UCHAR rule = T ? T->Rule : 0;
        if (N->IsRoot && d >= 0x10) rule = RfsRuleOf(V, RfsGet32(b + At + REFS_IR_SCHEMA));
        bare = (BOOLEAN)(N->Level == 0 && (N->Flags & REFS_NF_BARE) && rule == 0x0B);
    }
    N->Rows = (PRFS_ROW)RfsAllocZero((cnt ? cnt : 1) * sizeof(RFS_ROW));
    if (!N->Rows) { RfsNodeFree(N); return RFS_NO_MEMORY; }
    N->CapRows = cnt ? cnt : 1;
    for (i = 0; i < cnt; i++) {
        ULONG e = RfsGet32(h + ks + 4 * i), ro = hdr + (e & 0xFFFF);
        PRFS_ROW r = &N->Rows[i];
        r->Marker = (USHORT)(e >> 16);
        if (bare) {
            /* a bare extent record */
            ULONG rs;
            if (ro + REFS_EXT_LENGTH > Len) { RfsNodeFree(N); return RFS_CORRUPT; }
            rs = RfsGet16(b + ro + REFS_EXT_RECSIZE);
            if (rs < REFS_EXT_LENGTH || ro + rs > Len) rs = REFS_EXT_LENGTH;
            r->Val = b + ro;
            r->ValLen = rs;
        } else {
            ULONG rs, ko, kl, vo, vl;
            if (ro + REFS_RH_LENGTH > Len) { RfsNodeFree(N); return RFS_CORRUPT; }
            rs = RfsGet32(b + ro + REFS_RH_SIZE);
            ko = RfsGet16(b + ro + REFS_RH_KEYOFF);
            kl = RfsGet16(b + ro + REFS_RH_KEYLEN);
            vo = RfsGet16(b + ro + REFS_RH_VALOFF);
            vl = RfsGet16(b + ro + REFS_RH_VALLEN);
            if (V->V1) vl = RfsGet32(b + ro + REFS_RH_VALLEN);
            if (ro + ko + kl > Len || ro + vo + vl > Len || (rs && (ko + kl > rs || vo + vl > rs))) { RfsNodeFree(N); return RFS_CORRUPT; }
            r->Key = b + ro + ko;
            r->KeyLen = (USHORT)kl;
            r->Val = b + ro + vo;
            r->ValLen = vl;
            r->Flags = RfsGet16(b + ro + REFS_RH_FLAGS);
            r->Overlap = (UCHAR)(ko == vo);
        }
    }
    N->NRows = cnt;
    *Out = N;
    return RFS_OK;
}

/* reads a page as a node of table T */
static RFS_STATUS
LoadPageNode(PRFS_VOL V, PRFS_TABLE T, const RFS_REF *Ref, BOOLEAN Root, PRFS_NODE *Out)
{
    PRFS_PAGE P;
    RFS_STATUS st = RfsPageRead(V, Ref, T->Real, &P);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsNodeDecode(V, T, P->Data, V->PageSize, V->HeaderSize, Root, Out);
    if (RFS_SUCCESS(st)) {
        RfsCopy((*Out)->PageHdr, P->Data, V->HeaderSize);
        (*Out)->Ref = *Ref;
        (*Out)->HasRef = TRUE;
    }
    RfsPagePut(V, P);
    return st;
}

RFS_STATUS
RfsLoadChild(PRFS_VOL V, PRFS_TABLE T, PRFS_NODE Parent, ULONG Slot, PRFS_NODE *Child)
{
    PRFS_ROW r = &Parent->Rows[Slot];
    RFS_REF ref;
    RFS_STATUS st;
    if (r->Child) { *Child = r->Child; return RFS_OK; }
    if (r->ValLen < (V->V1 ? REFS1_REF_SIZE : 0x28)) return RFS_CORRUPT;
    RfsRefGet(V, r->Val, &ref);
    st = LoadPageNode(V, T, &ref, FALSE, Child);
    if (!RFS_SUCCESS(st)) return st;
    if ((*Child)->Level + 1 != Parent->Level && Parent->Level != 0) {
        /* levels must step down by one */
        RfsNodeFree(*Child);
        *Child = NULL;
        return RFS_CORRUPT;
    }
    r->Child = *Child;
    return RFS_OK;
}

/* ---- tables -------------------------------------------------------------------------------------- */
static RFS_STATUS
TableLoadRoot(PRFS_VOL V, PRFS_TABLE T)
{
    RFS_STATUS st = LoadPageNode(V, T, &T->RootRef, TRUE, &T->Root);
    if (!RFS_SUCCESS(st)) return st;
    if (T->Root->Desc) {
        T->Schema = RfsGet32(T->Root->Desc + REFS_IR_SCHEMA);
        T->Rule = RfsRuleOf(V, T->Schema);
    }
    return RFS_OK;
}

RFS_STATUS
RfsTableOpenRoot(PRFS_VOL V, ULONG Index, PRFS_TABLE *Out)
{
    PRFS_TABLE T;
    RFS_STATUS st;
    *Out = NULL;
    if (Index >= V->NRoots) return RFS_CORRUPT;
    T = (PRFS_TABLE)RfsAllocZero(sizeof(RFS_TABLE));
    if (!T) return RFS_NO_MEMORY;
    T->Vol = V;
    T->Where = RFS_TROOT_CHECKPOINT;
    T->Index = Index;
    T->Real = (BOOLEAN)(!V->V1 && (Index == REFS_ROOT_CONTAINER || Index == REFS_ROOT_CONTAINER2 || Index == REFS_ROOT_SMALL));
    T->RootRef = V->Roots[Index];
    T->Refs = 1;
    T->Tier = RFS_TIER_MEDIUM;
    st = TableLoadRoot(V, T);
    if (!RFS_SUCCESS(st)) { RfsHostFree(T); return st; }
    *Out = T;
    return RFS_OK;
}

/* the object table row of Oid: its root reference */
static RFS_STATUS
ObjectRoot(PRFS_VOL V, ULONGLONG Oid, PRFS_REF Ref)
{
    UCHAR key[16];
    RFS_CURSOR c;
    RFS_STATUS st;
    PRFS_ROW r;
    RfsZero(key, 16);
    RfsPut64(key + 8, Oid);
    st = RfsLookup(V->Object, key, 16, &c);
    if (!RFS_SUCCESS(st)) return st;
    r = RfsCurRow(&c);
    if (V->V1) {
        if (r->ValLen < REFS1_REF_SIZE) st = RFS_CORRUPT;
        else RfsRefGet(V, r->Val, Ref);
    } else {
        if (r->ValLen < 0x20 + 0x28) st = RFS_CORRUPT;
        else RfsRefGet(V, r->Val + 0x20, Ref);
    }
    RfsCursorClose(&c);
    return st;
}

static ULONG TableHashOf(ULONGLONG Oid) { return (ULONG)(Oid * 0x9E3779B1UL) & (RFS_TABLE_HASH - 1); }

static VOID
TableLruRemove(PRFS_VOL V, PRFS_TABLE T)
{
    if (!T->LruPrev && V->TableLruHead != T) return;
    if (T->LruPrev) T->LruPrev->LruNext = T->LruNext; else V->TableLruHead = T->LruNext;
    if (T->LruNext) T->LruNext->LruPrev = T->LruPrev; else V->TableLruTail = T->LruPrev;
    T->LruPrev = T->LruNext = NULL;
}

static VOID
TableDestroy(PRFS_VOL V, PRFS_TABLE T)
{
    PRFS_TABLE *pp = &V->TableHash[TableHashOf(T->Oid)];
    while (*pp && *pp != T) pp = &(*pp)->HashNext;
    if (*pp) *pp = T->HashNext;
    TableLruRemove(V, T);
    if (T->Root) RfsNodeFree(T->Root);
    RfsHostFree(T);
    V->TableCount--;
}

RFS_STATUS
RfsTableOpenObject(PRFS_VOL V, ULONGLONG Oid, PRFS_TABLE *Out)
{
    PRFS_TABLE T;
    RFS_STATUS st;
    *Out = NULL;
    for (T = V->TableHash[TableHashOf(Oid)]; T; T = T->HashNext) {
        if (T->Where == RFS_TROOT_OBJECT && T->Oid == Oid) {
            if (T->Refs++ == 0) TableLruRemove(V, T);
            *Out = T;
            return RFS_OK;
        }
    }
    T = (PRFS_TABLE)RfsAllocZero(sizeof(RFS_TABLE));
    if (!T) return RFS_NO_MEMORY;
    T->Vol = V;
    T->Where = RFS_TROOT_OBJECT;
    T->Oid = Oid;
    T->Tier = RFS_TIER_MEDIUM;
    st = ObjectRoot(V, Oid, &T->RootRef);
    if (RFS_SUCCESS(st)) st = TableLoadRoot(V, T);
    if (!RFS_SUCCESS(st)) { RfsHostFree(T); return st; }
    T->Refs = 1;
    T->HashNext = V->TableHash[TableHashOf(Oid)];
    V->TableHash[TableHashOf(Oid)] = T;
    V->TableCount++;
    *Out = T;
    return RFS_OK;
}

RFS_STATUS
RfsTableOpenEmbedded(PRFS_VOL V, ULONGLONG Owner, const UCHAR *Val, ULONG Len, PRFS_TABLE *Out)
{
    PRFS_TABLE T;
    RFS_STATUS st;
    *Out = NULL;
    T = (PRFS_TABLE)RfsAllocZero(sizeof(RFS_TABLE));
    if (!T) return RFS_NO_MEMORY;
    T->Vol = V;
    T->Where = RFS_TROOT_EMBEDDED;
    T->Oid = Owner;
    T->Refs = 1;
    T->Tier = RFS_TIER_MEDIUM;
    st = RfsNodeDecode(V, T, Val, Len, 0, TRUE, &T->Root);
    if (!RFS_SUCCESS(st)) { RfsHostFree(T); return st; }
    if (T->Root->Desc && T->Root->DescLen >= 0x10) {
        T->Schema = RfsGet32(T->Root->Desc + REFS_IR_SCHEMA);
        T->Rule = RfsRuleOf(V, T->Schema);
    }
    *Out = T;
    return RFS_OK;
}

VOID
RfsTableClose(PRFS_VOL V, PRFS_TABLE T)
{
    if (!T) return;
    if (T->Refs) T->Refs--;
    if (T->Refs) return;
    if (T->Where != RFS_TROOT_OBJECT) {
        if (T->Root) RfsNodeFree(T->Root);
        RfsHostFree(T);
        return;
    }
    if (T->Dirty) return;                       /* kept until the commit */
    /* to the LRU; the oldest clean ones go */
    T->LruNext = V->TableLruHead;
    T->LruPrev = NULL;
    if (V->TableLruHead) V->TableLruHead->LruPrev = T; else V->TableLruTail = T;
    V->TableLruHead = T;
    while (V->TableCount > V->TableMax && V->TableLruTail) TableDestroy(V, V->TableLruTail);
}

/* forgets every clean, unused cached table (after a commit or on memory pressure) */
VOID
RfsTableTrim(PRFS_VOL V)
{
    while (V->TableLruTail) TableDestroy(V, V->TableLruTail);
}

/* ---- cursors ------------------------------------------------------------------------------------ */
PRFS_ROW
RfsCurRow(PRFS_CURSOR C)
{
    PRFS_NODE n = C->Node[C->Depth];
    return &n->Rows[C->Slot[C->Depth]];
}

VOID
RfsCursorClose(PRFS_CURSOR C)
{
    C->Valid = FALSE;
    if (C->HoldsRef) {
        C->HoldsRef = FALSE;
        RfsTableClose(C->Table->Vol, C->Table);
    }
}

/* the key of row r for comparison (stream leaf records: their file cluster) */
BOOLEAN
RfsNodeBare(PRFS_TABLE T, PRFS_NODE N)
{
    return (BOOLEAN)(N->Level == 0 && (N->Flags & REFS_NF_BARE) && T->Rule == 0x0B);
}

static int
RowCmp(PRFS_TABLE T, PRFS_NODE N, PRFS_ROW r, const UCHAR *Key, ULONG KeyLen)
{
    if (RfsNodeBare(T, N)) {
        UCHAR k[16];
        RfsRecordKey(T->Rule, r, k);
        return RfsKeyCompare(T->Vol, T->Rule, k, 16, Key, KeyLen);
    }
    return RfsKeyCompare(T->Vol, T->Rule, r->Key, r->KeyLen, Key, KeyLen);
}

/* first slot whose key >= Key (index nodes: the child that may hold Key) */
static ULONG
FindSlot(PRFS_TABLE T, PRFS_NODE N, const UCHAR *Key, ULONG KeyLen)
{
    ULONG lo = 0, hi = N->NRows;
    if (!Key) return 0;
    while (lo < hi) {
        ULONG mid = (lo + hi) / 2;
        PRFS_ROW r = &N->Rows[mid];
        int c;
        if (N->Level && (r->KeyLen == 0 || (r->Flags & REFS_RF_LASTCHILD))) c = 1;     /* no bound */
        else c = RowCmp(T, N, r, Key, KeyLen);
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}

/* walks down to the leaf, from Depth, taking FindSlot's child at each level */
static RFS_STATUS
Descend(PRFS_CURSOR C, const UCHAR *Key, ULONG KeyLen)
{
    PRFS_TABLE T = C->Table;
    for (;;) {
        PRFS_NODE n = C->Node[C->Depth], child;
        ULONG s;
        RFS_STATUS st;
        if (n->Level == 0) {
            C->Slot[C->Depth] = FindSlot(T, n, Key, KeyLen);
            return RFS_OK;
        }
        s = FindSlot(T, n, Key, KeyLen);
        if (s >= n->NRows) s = n->NRows ? n->NRows - 1 : 0;
        if (!n->NRows) return RFS_CORRUPT;
        C->Slot[C->Depth] = s;
        if (C->Depth + 1 >= RFS_MAX_DEPTH) return RFS_CORRUPT;
        st = RfsLoadChild(T->Vol, T, n, s, &child);
        if (!RFS_SUCCESS(st)) return st;
        C->Depth++;
        C->Node[C->Depth] = child;
    }
}

/* moves to a valid row: past the end of a leaf goes to the next leaf */
static RFS_STATUS
Settle(PRFS_CURSOR C)
{
    PRFS_TABLE T = C->Table;
    while (C->Slot[C->Depth] >= C->Node[C->Depth]->NRows) {
        ULONG d;
        /* up to a level with a next child */
        for (d = C->Depth; d > 0; d--) {
            if (C->Slot[d - 1] + 1 < C->Node[d - 1]->NRows) break;
        }
        if (d == 0) return RFS_NO_MORE;
        d--;
        C->Slot[d]++;
        C->Depth = d;
        /* down the leftmost path */
        while (C->Node[C->Depth]->Level != 0) {
            PRFS_NODE child;
            RFS_STATUS st = RfsLoadChild(T->Vol, T, C->Node[C->Depth], C->Slot[C->Depth], &child);
            if (!RFS_SUCCESS(st)) return st;
            if (C->Depth + 1 >= RFS_MAX_DEPTH) return RFS_CORRUPT;
            C->Depth++;
            C->Node[C->Depth] = child;
            C->Slot[C->Depth] = 0;
        }
    }
    return RFS_OK;
}

RFS_STATUS
RfsSeek(PRFS_TABLE T, const UCHAR *Key, ULONG KeyLen, PRFS_CURSOR C)
{
    RFS_STATUS st;
    RfsZero(C, sizeof(*C));
    C->Table = T;
    if (!T->Root) return RFS_CORRUPT;
    T->Refs++;
    C->HoldsRef = TRUE;
    C->Node[0] = T->Root;
    st = Descend(C, Key, KeyLen);
    if (RFS_SUCCESS(st)) st = Settle(C);
    C->Valid = (BOOLEAN)RFS_SUCCESS(st);
    return st;
}

RFS_STATUS
RfsNext(PRFS_CURSOR C)
{
    RFS_STATUS st;
    if (!C->Valid) return RFS_NO_MORE;
    C->Slot[C->Depth]++;
    st = Settle(C);
    C->Valid = (BOOLEAN)RFS_SUCCESS(st);
    return st;
}

RFS_STATUS
RfsLookup(PRFS_TABLE T, const UCHAR *Key, ULONG KeyLen, PRFS_CURSOR C)
{
    RFS_STATUS st = RfsSeek(T, Key, KeyLen, C);
    if (st == RFS_NO_MORE) return RFS_NOT_FOUND;
    if (!RFS_SUCCESS(st)) return st;
    if (RowCmp(T, C->Node[C->Depth], RfsCurRow(C), Key, KeyLen) != 0) { C->Valid = FALSE; return RFS_NOT_FOUND; }
    return RFS_OK;
}

/* ---- for writing ------------------------------------------------------------------------------------ */
/* the leaf where Key belongs and the slot it would take (may be past the last row) */
RFS_STATUS
RfsSeekLeaf(PRFS_TABLE T, const UCHAR *Key, ULONG KeyLen, PRFS_CURSOR C)
{
    RFS_STATUS st;
    RfsZero(C, sizeof(*C));
    C->Table = T;
    if (!T->Root) return RFS_CORRUPT;
    T->Refs++;
    C->HoldsRef = TRUE;
    C->Node[0] = T->Root;
    st = Descend(C, Key, KeyLen);
    C->Valid = (BOOLEAN)RFS_SUCCESS(st);
    if (!RFS_SUCCESS(st)) RfsCursorClose(C);
    return st;
}

/* the table has changes: global and object tables are kept (a reference) until the commit */
VOID
RfsTableDirty(PRFS_TABLE T)
{
    PRFS_VOL V = T->Vol;
    T->Dirty = TRUE;
    V->Dirty = TRUE;
    if (T->Where == RFS_TROOT_EMBEDDED || T->OnDirtyList) return;
    T->OnDirtyList = TRUE;
    T->Refs++;
    T->DirtyNext = V->DirtyHead;
    V->DirtyHead = T;
}

/* a checkpoint-root table, kept open while mounted */
RFS_STATUS
RfsGlobal(PRFS_VOL V, ULONG Root, PRFS_TABLE *Out)
{
    RFS_STATUS st;
    *Out = NULL;
    if (Root >= V->NRoots) return RFS_CORRUPT;
    if (!V->Global[Root] && V->Object && V->Object->Index == Root) {
        V->Global[Root] = V->Object;
        V->Object->Oid = RfsGet64(V->Object->Root->PageHdr + (V->V1 ? REFS1_PH_TABLE : REFS3_PH_TABLE));
        V->Object->Tier = RfsTierOf(V, V->Object);
    }
    if (!V->Global[Root]) {
        st = RfsTableOpenRoot(V, Root, &V->Global[Root]);
        if (!RFS_SUCCESS(st)) return st;
        V->Global[Root]->Oid = RfsGet64(V->Global[Root]->Root->PageHdr + (V->V1 ? REFS1_PH_TABLE : REFS3_PH_TABLE));
        V->Global[Root]->Tier = RfsTierOf(V, V->Global[Root]);
    }
    *Out = V->Global[Root];
    return RFS_OK;
}

/* a deleted object: its table must not be found again */
VOID
RfsTableForget(PRFS_VOL V, PRFS_TABLE T)
{
    PRFS_TABLE *pp = &V->TableHash[TableHashOf(T->Oid)];
    while (*pp && *pp != T) pp = &(*pp)->HashNext;
    if (*pp) { *pp = T->HashNext; V->TableCount--; }
    T->HashNext = NULL;
    T->Where = RFS_TROOT_EMBEDDED;      /* freed on its last close, never cached */
    if (T->OnDirtyList) {
        PRFS_TABLE *dp = &V->DirtyHead;
        while (*dp && *dp != T) dp = &(*dp)->DirtyNext;
        if (*dp) *dp = T->DirtyNext;
        T->OnDirtyList = FALSE;
        T->Dirty = FALSE;
        T->Refs--;
    }
}

/* the last row of the table */
RFS_STATUS
RfsSeekLast(PRFS_TABLE T, PRFS_CURSOR C)
{
    RfsZero(C, sizeof(*C));
    C->Table = T;
    if (!T->Root) return RFS_CORRUPT;
    T->Refs++;
    C->HoldsRef = TRUE;
    C->Node[0] = T->Root;
    for (;;) {
        PRFS_NODE n = C->Node[C->Depth], child;
        RFS_STATUS st;
        if (!n->NRows) { RfsCursorClose(C); return RFS_NO_MORE; }
        C->Slot[C->Depth] = n->NRows - 1;
        if (n->Level == 0) break;
        if (C->Depth + 1 >= RFS_MAX_DEPTH) { RfsCursorClose(C); return RFS_CORRUPT; }
        st = RfsLoadChild(T->Vol, T, n, n->NRows - 1, &child);
        if (!RFS_SUCCESS(st)) { RfsCursorClose(C); return st; }
        C->Depth++;
        C->Node[C->Depth] = child;
    }
    C->Valid = TRUE;
    return RFS_OK;
}

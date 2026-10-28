/*
 * REFSNT - changing files and directories (ReFS 3.x).
 *
 * A file lives in its directory's table as a name row {0x30, 1, name}
 * whose value is the file's own table (schema 0x1E0): the header data hold
 * times, attributes, security id, sizes and the file id; the rows are its
 * attributes. Files made here get the layout Windows gives a written file:
 *   $DATA stream set   key {len, 0x80000002, 0x80, 0x0E, 8, 8, 1}
 *                      value {0x1001, 1, 0, 0, 0}
 *   $DATA live stream  key {len, 0x80000002, 0x80, 0x0E, 0x1000, 8, 0}
 *                      value: the extent table (schema 0x200), header data
 *                      {0, 0x28, alloc, size, valid, alloc, 1}, rows: bare
 *                      24-byte extents {vlcn, flags, 24, vcn, run}
 * and a reverse-index row {0x80000020, 0, file id, 0} -> {0, 0x0C, name
 * bytes, name}. File ids come from the directory's object-table row
 * (the 8 bytes after the payload: the last id given out).
 *
 * A directory is an object of its own: an object-table row (in both object
 * tables), a table (schema 0x130) whose 0x10 row is the directory's record
 * (schema 0x1F0: times etc. as header data; rows $OBJ_LINK {parent, name}
 * and the $I30 stub), a parent-child row {0, parent, 0, child}, and in the
 * parent an index entry {0x30, 2, name} -> {0, oid, times, 0, 0, attributes
 * | 0x10000000}.
 *
 * The first u64 of an attribute key is the value's length (comparisons
 * skip it).
 */
#include "rfseng.h"

#define FILE_DESC       0xA8
#define STREAM_DESC     0x88
#define FH_FILEID       0x80
#define FH_LINKS        0x98
#define SH_ALLOC        0x08    /* stream header data (desc + 0x28) */
#define SH_SIZE         0x10
#define SH_VALID        0x18
#define SH_ALLOC2       0x20
#define SH_ONE          0x28

#define EXTF_NEW        (REFS_EXTF_DATA | REFS_EXTF_WRITTEN)

/* ---- keys --------------------------------------------------------------------------------------------- */
static RFS_STATUS
NameKey(const USHORT *Name, ULONG Len, USHORT Flags, PUCHAR Key, PULONG KeyLen)
{
    ULONG i;
    if (Len == 0 || Len > RFS_MAX_NAME) return RFS_BAD_NAME;
    RfsPut16(Key, REFS_KEY_NAME);
    RfsPut16(Key + 2, Flags);
    for (i = 0; i < Len; i++) RfsPut16(Key + 4 + 2 * i, Name[i]);
    *KeyLen = 4 + 2 * Len;
    return RFS_OK;
}

static VOID
RevKey(ULONGLONG FileId, PUCHAR Key)
{
    RfsZero(Key, 24);
    RfsPut32(Key, 0x80000000UL | REFS_KEY_REVERSE);
    RfsPut64(Key + 8, FileId);
}

static ULONG
RevVal(const USHORT *Name, ULONG Len, PUCHAR Val)
{
    ULONG i;
    RfsZero(Val, 12);
    RfsPut16(Val + 8, 0x0C);
    RfsPut16(Val + 10, (USHORT)(2 * Len));
    for (i = 0; i < Len; i++) RfsPut16(Val + 12 + 2 * i, Name[i]);
    return 12 + 2 * Len;
}

static VOID
StreamKey(PUCHAR Key, ULONG ValLen, ULONGLONG Sub, ULONGLONG A, ULONGLONG B)
{
    RfsZero(Key, 40);
    RfsPut64(Key, ValLen);
    RfsPut32(Key + 8, REFS_AT_MARKER_MULTI);
    RfsPut16(Key + 12, REFS_AT_DATA);
    RfsPut16(Key + 14, 0x0E);
    RfsPut64(Key + 16, Sub);
    RfsPut64(Key + 24, A);
    RfsPut64(Key + 32, B);
}

static VOID
DescInit(PUCHAR D, ULONG Len, ULONG Schema)
{
    RfsZero(D, Len);
    RfsPut32(D, Len);
    RfsPut16(D + 4, 0x28);
    RfsPut16(D + 6, 1);
    RfsPut32(D + 8, 1);
    RfsPut32(D + REFS_IR_SCHEMA, Schema);
    RfsPut32(D + REFS_IR_SCHEMA2, Schema);
    RfsPut32(D + 0x14, 2);
}

/* ---- object rows ----------------------------------------------------------------------------------- */
static RFS_STATUS
ObjectRowOpen(PRFS_VOL V, ULONG Root, ULONGLONG Oid, PRFS_CURSOR C)
{
    PRFS_TABLE T;
    UCHAR key[16];
    RFS_STATUS st = RfsGlobal(V, Root, &T);
    if (!RFS_SUCCESS(st)) return st;
    RfsZero(key, 16);
    RfsPut64(key + 8, Oid);
    st = RfsLookup(T, key, 16, C);
    if (!RFS_SUCCESS(st)) RfsCursorClose(C);
    return st;
}

/* the next file id of directory Oid (from its object row's trailer, both copies updated) */
static RFS_STATUS
NextFileId(PRFS_VOL V, ULONGLONG Dir, PULONGLONG Id)
{
    static const ULONG roots[2] = { REFS_ROOT_OBJECT, REFS_ROOT_OBJECT2 };
    ULONGLONG id = 0;
    ULONG k;
    for (k = 0; k < 2; k++) {
        RFS_CURSOR c;
        PRFS_ROW r;
        ULONG po, tl;
        RFS_STATUS st = ObjectRowOpen(V, roots[k], Dir, &c);
        if (!RFS_SUCCESS(st)) return st;
        r = RfsCurRow(&c);
        po = r->ValLen >= 0x18 ? RfsGet32(r->Val + 0x10) : 0;
        tl = r->ValLen >= 0x18 ? RfsGet32(r->Val + 0x14) : 0;
        if (tl != 8 || po + 8 > r->ValLen) { RfsCursorClose(&c); return RFS_NOT_SUPPORTED; }
        if (k == 0) {
            id = RfsGet64(r->Val + po) + 1;
            if (id < 2) id = 2;
        }
        {
            PUCHAR val;
            st = RfsTreeModifyAt(&c, &val);
            if (RFS_SUCCESS(st)) RfsPut64(val + po, id);
        }
        RfsCursorClose(&c);
        if (!RFS_SUCCESS(st)) return st;
    }
    *Id = id;
    return RFS_OK;
}

/* a new object id: one past the largest */
static RFS_STATUS
NewOid(PRFS_VOL V, PULONGLONG Oid)
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

/* ---- a row of a directory --------------------------------------------------------------------------- */
typedef struct _ROWCOPY {
    UCHAR   Key[RFS_MAX_KEY];
    ULONG   KeyLen;
    PUCHAR  Val;
    ULONG   ValLen;
    USHORT  Flags;
} ROWCOPY;

static RFS_STATUS
RowCopy(const RFS_ROW *R, ROWCOPY *Out)
{
    if (R->KeyLen > RFS_MAX_KEY) return RFS_CORRUPT;
    RfsCopy(Out->Key, R->Key, R->KeyLen);
    Out->KeyLen = R->KeyLen;
    Out->Val = (PUCHAR)RfsHostAlloc(R->ValLen ? R->ValLen : 1);
    if (!Out->Val) return RFS_NO_MEMORY;
    RfsCopy(Out->Val, R->Val, R->ValLen);
    Out->ValLen = R->ValLen;
    Out->Flags = R->Flags;
    return RFS_OK;
}

/* the name row of Name in table T (exact by the directory's collation) */
static RFS_STATUS
FindName(PRFS_TABLE T, const USHORT *Name, ULONG Len, PRFS_CURSOR C)
{
    PUCHAR key = (PUCHAR)RfsHostAlloc(RFS_MAX_KEY);
    ULONG kl;
    RFS_STATUS st;
    if (!key) return RFS_NO_MEMORY;
    st = NameKey(Name, Len, 0, key, &kl);
    if (RFS_SUCCESS(st)) st = RfsSeek(T, key, kl, C);
    else { RfsHostFree(key); return st; }
    RfsHostFree(key);
    if (st == RFS_NO_MORE) return RFS_NOT_FOUND;
    if (!RFS_SUCCESS(st)) { RfsCursorClose(C); return st; }
    {
        PRFS_ROW r = RfsCurRow(C);
        if (r->KeyLen < 4 || RfsGet16(r->Key) != REFS_KEY_NAME ||
            RfsNameCompare(T->Vol, (const USHORT *)(r->Key + 4), (r->KeyLen - 4) / 2, Name, Len) != 0) {
            RfsCursorClose(C);
            return RFS_NOT_FOUND;
        }
    }
    return RFS_OK;
}

/* the directory record (0x10 row): times changed */
static RFS_STATUS
TouchDir(PRFS_VOL V, PRFS_TABLE D, BOOLEAN Write)
{
    RFS_CURSOR c;
    UCHAR key[4];
    PUCHAR val;
    ULONGLONG now = RfsHostNowNt();
    RFS_STATUS st;
    RfsPut32(key, REFS_KEY_DIRSELF);
    st = RfsLookup(D, key, 4, &c);
    if (st == RFS_NOT_FOUND) { RfsCursorClose(&c); return RFS_OK; }
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st; }
    if (RfsCurRow(&c)->ValLen < REFS_FH_ATIME + 8) { RfsCursorClose(&c); return RFS_OK; }
    st = RfsTreeModifyAt(&c, &val);
    if (RFS_SUCCESS(st)) {
        if (Write) RfsPut64(val + REFS_FH_MTIME, now);
        RfsPut64(val + REFS_FH_CHTIME, now);
    }
    RfsCursorClose(&c);
    (VOID)V;
    return st;
}

/* a security id for a new file or directory in D */
static ULONGLONG
PickSecurity(PRFS_VOL V, PRFS_TABLE D, BOOLEAN Dir)
{
    RFS_CURSOR c;
    UCHAR key[4];
    ULONGLONG id = 0;
    RFS_STATUS st;
    (VOID)V;
    if (!Dir) {
        /* a file beside it */
        RfsPut16(key, REFS_KEY_NAME);
        RfsPut16(key + 2, 0);
        st = RfsSeek(D, key, 4, &c);
        while (RFS_SUCCESS(st)) {
            PRFS_ROW r = RfsCurRow(&c);
            if (r->KeyLen < 4 || RfsGet16(r->Key) != REFS_KEY_NAME) break;
            if (RfsGet16(r->Key + 2) == REFS_NAME_EMBEDDED && r->ValLen >= REFS_FH_SECID + 8) {
                id = RfsGet64(r->Val + REFS_FH_SECID);
                if (id) break;
            }
            st = RfsNext(&c);
        }
        RfsCursorClose(&c);
        if (id) return id;
    }
    RfsPut32(key, REFS_KEY_DIRSELF);
    if (RFS_SUCCESS(RfsLookup(D, key, 4, &c))) {
        PRFS_ROW r = RfsCurRow(&c);
        if (r->ValLen >= REFS_FH_SECID + 8) id = RfsGet64(r->Val + REFS_FH_SECID);
    }
    RfsCursorClose(&c);
    return id;
}

/* ---- new files ------------------------------------------------------------------------------------- */
/* an empty file's table: header, stream set and an empty extent table */
static RFS_STATUS
NewFileValue(PRFS_VOL V, ULONGLONG Dir, ULONGLONG FileId, ULONG Attr, ULONGLONG SecId, PUCHAR *Val, PULONG Len)
{
    UCHAR desc[FILE_DESC], sdesc[STREAM_DESC], key[40], set[0x28];
    PRFS_TABLE F = NULL, S = NULL;
    PUCHAR sval = NULL;
    ULONG slen;
    ULONGLONG now = RfsHostNowNt();
    RFS_STATUS st;
    DescInit(desc, FILE_DESC, REFS_SCHEMA_FILE);
    RfsPut64(desc + REFS_FH_CTIME, now);
    RfsPut64(desc + REFS_FH_MTIME, now);
    RfsPut64(desc + REFS_FH_CHTIME, now);
    RfsPut64(desc + REFS_FH_ATIME, now);
    RfsPut32(desc + REFS_FH_ATTR, Attr);
    RfsPut64(desc + REFS_FH_SECID, SecId);
    RfsPut64(desc + FH_FILEID, FileId);
    RfsPut64(desc + FH_LINKS, 1);
    DescInit(sdesc, STREAM_DESC, REFS_SCHEMA_STREAM);
    RfsPut32(sdesc + REFS_IR_DATA + 4, 0x28);
    RfsPut64(sdesc + REFS_IR_DATA + SH_ONE, 1);
    st = RfsNewTable(V, RFS_TROOT_EMBEDDED, Dir, REFS_SCHEMA_STREAM, sdesc, STREAM_DESC, &S);
    if (!RFS_SUCCESS(st)) return st;
    S->Root->Flags = REFS_NF_ROOT | REFS_NF_RANGE | REFS_NF_BARE;
    st = RfsEmbeddedStore(S, &sval, &slen);
    RfsTableClose(V, S);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsNewTable(V, RFS_TROOT_EMBEDDED, Dir, REFS_SCHEMA_FILE, desc, FILE_DESC, &F);
    if (RFS_SUCCESS(st)) {
        RfsZero(set, sizeof(set));
        RfsPut64(set, 0x1001);
        RfsPut64(set + 8, 1);
        StreamKey(key, sizeof(set), REFS_STREAM_META, 8, 1);
        st = RfsTreeInsert(F, key, 40, set, sizeof(set), 0, FALSE);
    }
    if (RFS_SUCCESS(st)) {
        StreamKey(key, slen, REFS_STREAM_LIVE, 8, 0);
        st = RfsTreeInsert(F, key, 40, sval, slen, REFS_RF_EMBEDDED, FALSE);
    }
    if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(F, Val, Len);
    RfsHostFree(sval);
    if (F) RfsTableClose(V, F);
    return st;
}

/* a new directory's record: the parent's as a pattern ($I30 kept), its own link */
static RFS_STATUS
NewDirRecord(PRFS_VOL V, PRFS_TABLE Parent, ULONGLONG ParentOid, ULONGLONG Oid, const USHORT *Name, ULONG Len,
             ULONG Attr, PUCHAR *Val, PULONG VLen)
{
    RFS_CURSOR c;
    UCHAR key[4], lk[8 + 24 + 2 * RFS_MAX_NAME];
    PRFS_TABLE pat = NULL, R = NULL;
    RFS_STATUS st;
    ULONGLONG now = RfsHostNowNt();
    ULONG i, ll;
    PUCHAR desc;
    RfsPut32(key, REFS_KEY_DIRSELF);
    st = RfsLookup(Parent, key, 4, &c);
    if (!RFS_SUCCESS(st)) { RfsCursorClose(&c); return st == RFS_NOT_FOUND ? RFS_CORRUPT : st; }
    {
        PRFS_ROW r = RfsCurRow(&c);
        st = RfsTableOpenEmbedded(V, Oid, r->Val, r->ValLen, &pat);
    }
    RfsCursorClose(&c);
    if (!RFS_SUCCESS(st)) return st;
    if (pat->Root->Level || !pat->Root->Desc) { RfsTableClose(V, pat); return RFS_NOT_SUPPORTED; }
    desc = (PUCHAR)RfsHostAlloc(pat->Root->DescLen);
    if (!desc) { RfsTableClose(V, pat); return RFS_NO_MEMORY; }
    RfsCopy(desc, pat->Root->Desc, pat->Root->DescLen);
    if (pat->Root->DescLen >= REFS_FH_ATTR + 4) {
        RfsPut64(desc + REFS_FH_CTIME, now);
        RfsPut64(desc + REFS_FH_MTIME, now);
        RfsPut64(desc + REFS_FH_CHTIME, now);
        RfsPut64(desc + REFS_FH_ATIME, now);
        RfsPut32(desc + REFS_FH_ATTR, Attr & ~0x10UL);
    }
    st = RfsNewTable(V, RFS_TROOT_EMBEDDED, Oid, pat->Schema, desc, pat->Root->DescLen, &R);
    RfsHostFree(desc);
    if (!RFS_SUCCESS(st)) { RfsTableClose(V, pat); return st; }
    R->Root->Flags = pat->Root->Flags;
    /* $OBJ_LINK: key = {u64 length} + value {0x80000002, 0x39, 0x0D, parent, 0, name} */
    ll = 24 + 2 * Len;
    RfsZero(lk, sizeof(lk));
    RfsPut64(lk, ll);
    RfsPut32(lk + 8, REFS_AT_MARKER_MULTI);
    RfsPut16(lk + 12, REFS_AT_OBJLINK);
    RfsPut16(lk + 14, 0x0D);
    RfsPut64(lk + 16, ParentOid);
    for (i = 0; i < Len; i++) RfsPut16(lk + 32 + 2 * i, Name[i]);
    st = RfsTreeInsert(R, lk, 8 + ll, lk + 8, ll, 0, FALSE);
    /* the other rows of the pattern but its own link */
    for (i = 0; RFS_SUCCESS(st) && i < pat->Root->NRows; i++) {
        PRFS_ROW r = &pat->Root->Rows[i];
        if (r->KeyLen >= 14 && RfsGet16(r->Key + 12) == REFS_AT_OBJLINK) continue;
        if (r->KeyLen >= 14 && RfsGet16(r->Key + 12) != REFS_AT_I30) continue;
        st = RfsTreeInsert(R, r->Key, r->KeyLen, r->Val, r->ValLen, r->Flags, FALSE);
    }
    if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(R, Val, VLen);
    RfsTableClose(V, R);
    RfsTableClose(V, pat);
    return st;
}

static ULONG
IndexEntry(PRFS_VOL V, PUCHAR E, ULONGLONG FileId, ULONGLONG Home, const RFS_TIMES *T, ULONGLONG Alloc, ULONGLONG Size, ULONG Attr)
{
    ULONG n = V->Minor >= 10 ? REFS_IE_LENGTH : REFS_IE_LENGTH_OLD;
    RfsZero(E, REFS_IE_LENGTH);
    RfsPut64(E + REFS_IE_FILEID, FileId);
    RfsPut64(E + REFS_IE_HOME, Home);
    RfsPut64(E + REFS_IE_CTIME, T->Create);
    RfsPut64(E + REFS_IE_MTIME, T->Write);
    RfsPut64(E + REFS_IE_CHTIME, T->Change);
    RfsPut64(E + REFS_IE_ATIME, T->Access);
    RfsPut64(E + REFS_IE_ALLOC, Alloc);
    RfsPut64(E + REFS_IE_SIZE, Size);
    RfsPut32(E + REFS_IE_ATTR, Attr);
    return n;
}

static RFS_STATUS
ParentChild(PRFS_VOL V, ULONGLONG Parent, ULONGLONG Child, BOOLEAN Add)
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

static RFS_STATUS
ObjectRows(PRFS_VOL V, ULONGLONG Oid, BOOLEAN Add)
{
    static const ULONG roots[2] = { REFS_ROOT_OBJECT, REFS_ROOT_OBJECT2 };
    UCHAR key[16], val[0x20 + 104 + 8];
    ULONG k, vl = 0x20 + V->RefSize + 8;
    RfsZero(key, 16);
    RfsPut64(key + 8, Oid);
    RfsZero(val, sizeof(val));
    RfsPut32(val, 2);
    RfsPut32(val + 0x08, 0x18);
    RfsPut32(val + 0x0C, 0x30);
    RfsPut32(val + 0x10, 0x20 + V->RefSize);
    RfsPut32(val + 0x14, 8);
    RfsPut64(val + 0x18, RfsGet64(V->Chkp + 0x70));
    RfsPut64(val + 0x20 + V->RefSize, 1);
    for (k = 0; k < 2; k++) {
        PRFS_TABLE T;
        RFS_STATUS st = RfsGlobal(V, roots[k], &T);
        if (!RFS_SUCCESS(st)) return st;
        if (Add) st = RfsTreeInsert(T, key, 16, val, vl, 0, FALSE);
        else { st = RfsTreeDelete(T, key, 16); if (st == RFS_NOT_FOUND) st = RFS_OK; }
        if (!RFS_SUCCESS(st)) return st;
    }
    return RFS_OK;
}

static RFS_STATUS RFS_NOINLINE
Create3(PRFS_VOL V, ULONGLONG DirOid, const USHORT *Name, ULONG Len, BOOLEAN Dir, ULONG Attr, PRFS_DIRENT Ent, PUCHAR key, PUCHAR rv)
{
    PRFS_TABLE D;
    RFS_CURSOR c;
    UCHAR rk[24];
    ULONG kl;
    RFS_STATUS st;
    ULONGLONG now = RfsHostNowNt();
    st = RfsTableOpenObject(V, DirOid, &D);
    if (!RFS_SUCCESS(st)) return st;
    st = FindName(D, Name, Len, &c);
    if (RFS_SUCCESS(st)) { RfsCursorClose(&c); RfsTableClose(V, D); return RFS_EXISTS; }
    if (st != RFS_NOT_FOUND) { RfsTableClose(V, D); return st; }
    RfsZero(Ent, sizeof(*Ent));
    Ent->Times.Create = Ent->Times.Write = Ent->Times.Change = Ent->Times.Access = now;
    {
        ULONG i;
        for (i = 0; i < Len && i < RFS_MAX_NAME; i++) Ent->Name[i] = Name[i];
        Ent->NameLen = Len;
    }
    if (!Dir) {
        ULONGLONG id;
        PUCHAR val;
        ULONG vl;
        if (!Attr) Attr = 0x20;
        st = NextFileId(V, DirOid, &id);
        if (RFS_SUCCESS(st)) st = NewFileValue(V, DirOid, id, Attr & ~0x10UL, PickSecurity(V, D, FALSE), &val, &vl);
        if (RFS_SUCCESS(st)) {
            NameKey(Name, Len, REFS_NAME_EMBEDDED, key, &kl);
            st = RfsTreeInsert(D, key, kl, val, vl, REFS_RF_EMBEDDED, FALSE);
            RfsHostFree(val);
        }
        if (RFS_SUCCESS(st)) {
            RevKey(id, rk);
            st = RfsTreeInsert(D, rk, 24, rv, RevVal(Name, Len, rv), 0, FALSE);
        }
        Ent->Type = RFS_FT_FILE;
        Ent->KeyFlags = REFS_NAME_EMBEDDED;
        Ent->Home = DirOid;
        Ent->FileId = id;
        Ent->Attributes = Attr & ~0x10UL;
    } else {
        ULONGLONG oid = 0;
        PRFS_TABLE N = NULL;
        PUCHAR rec = NULL;
        ULONG rl;
        UCHAR ie[REFS_IE_LENGTH];
        st = NewOid(V, &oid);
        if (RFS_SUCCESS(st)) st = ObjectRows(V, oid, TRUE);
        if (RFS_SUCCESS(st)) st = NewDirRecord(V, D, DirOid, oid, Name, Len, Attr, &rec, &rl);
        if (RFS_SUCCESS(st)) {
            st = RfsNewTable(V, RFS_TROOT_OBJECT, oid, REFS_SCHEMA_DIR, D->Root->Desc, D->Root->DescLen, &N);
            if (RFS_SUCCESS(st)) {
                UCHAR k4[4];
                ULONG i;
                /* the parent's header data are kept but its counters */
                for (i = REFS_IR_DATA; i < N->Root->DescLen; i++) N->Root->Desc[i] = 0;
                N->Root->Flags = REFS_NF_ROOT;
                N->TreeClock = 0;
                RfsPut32(k4, REFS_KEY_DIRSELF);
                st = RfsTreeInsert(N, k4, 4, rec, rl, REFS_RF_EMBEDDED, FALSE);
                RfsTableClose(V, N);
            }
        }
        RfsHostFree(rec);
        if (RFS_SUCCESS(st)) st = ParentChild(V, DirOid, oid, TRUE);
        if (RFS_SUCCESS(st)) {
            NameKey(Name, Len, REFS_NAME_INDEX, key, &kl);
            st = RfsTreeInsert(D, key, kl, ie, IndexEntry(V, ie, 0, oid, &Ent->Times, 0, 0, (Attr & ~0x10UL) | REFS_ATTR_DIRECTORY), 0, FALSE);
        }
        Ent->Type = RFS_FT_DIR;
        Ent->KeyFlags = REFS_NAME_INDEX;
        Ent->Oid = oid;
        Ent->Home = DirOid;
        Ent->Attributes = (Attr & ~0x10UL) | 0x10;
    }
    if (RFS_SUCCESS(st)) st = TouchDir(V, D, TRUE);
    RfsTableClose(V, D);
    return st;
}

RFS_STATUS
RfsCreate(PRFS_VOL V, ULONGLONG DirOid, const USHORT *Name, ULONG Len, BOOLEAN Dir, ULONG Attr, PRFS_DIRENT Ent)
{
    PUCHAR k;
    RFS_STATUS st;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (RFS_V1REC(V)) return RfsCreateV1(V, DirOid, Name, Len, Dir, Attr, Ent);
    k = (PUCHAR)RfsHostAlloc(RFS_MAX_KEY + 12 + 2 * RFS_MAX_NAME);
    if (!k) return RFS_NO_MEMORY;
    st = Create3(V, DirOid, Name, Len, Dir, Attr, Ent, k, k + RFS_MAX_KEY);
    RfsHostFree(k);
    return st;
}

/* ---- the file record --------------------------------------------------------------------------------- */
/* the row holding a file's record: in its directory (embedded) or its home's 0x40 row */
static RFS_STATUS
RecordRow(PRFS_VOL V, PRFS_FILE F, PRFS_TABLE *T, PRFS_CURSOR C)
{
    RFS_STATUS st;
    if (F->KeyFlags == REFS_NAME_EMBEDDED || !F->KeyFlags) {
        st = RfsTableOpenObject(V, F->Parent, T);
        if (!RFS_SUCCESS(st)) return st;
        st = FindName(*T, F->Name, F->NameLen, C);
    } else {
        UCHAR key[24];
        st = RfsTableOpenObject(V, F->Home, T);
        if (!RFS_SUCCESS(st)) return st;
        RfsZero(key, 24);
        RfsPut32(key, 0x80000000UL | REFS_KEY_BACKING);
        RfsPut64(key + 8, F->FileId);
        RfsPut64(key + 16, F->Home);
        st = RfsLookup(*T, key, 24, C);
        if (!RFS_SUCCESS(st)) RfsCursorClose(C);
    }
    if (!RFS_SUCCESS(st)) { RfsTableClose(V, *T); *T = NULL; }
    return st;
}

/* the extent table rebuilt from F's extents */
static RFS_STATUS
StoreExtents(PRFS_VOL V, PRFS_FILE F, PRFS_TABLE S)
{
    PRFS_NODE r = S->Root;
    ULONG i;
    RFS_STATUS st;
    if (F->ExtChanged) {
        /* every page below the root goes; the root starts empty */
        if (r->Level) {
            for (i = 0; i < r->NRows; i++) {
                PRFS_NODE ch;
                st = RfsLoadChild(V, S, r, i, &ch);
                if (!RFS_SUCCESS(st)) return st;
            }
            st = RfsTreeFreeAll(S);
            if (!RFS_SUCCESS(st)) return st;
        }
        for (i = 0; i < r->NRows; i++) {
            if (r->Rows[i].Child) RfsNodeFree(r->Rows[i].Child);
            r->Rows[i].Child = NULL;
            if (r->Rows[i].Owned) RfsHostFree(r->Rows[i].Key ? r->Rows[i].Key : r->Rows[i].Val);
            r->Rows[i].Owned = FALSE;
        }
        r->NRows = 0;
        r->Level = 0;
        r->Flags = REFS_NF_ROOT | REFS_NF_RANGE | REFS_NF_BARE;
        r->Dirty = TRUE;
        RfsPut64(r->Desc + REFS_IR_LEAVES, 0);
        RfsPut64(r->Desc + REFS_IR_ROWS, 0);
        for (i = 0; i < F->NExt; i++) {
            UCHAR rec[REFS_EXT_LENGTH];
            PRFS_EXTENT e = &F->Ext[i];
            RfsZero(rec, sizeof(rec));
            RfsPut64(rec + REFS_EXT_VLCN, e->Lcn);
            RfsPut16(rec + REFS_EXT_FLAGS, e->Flags);
            RfsPut16(rec + REFS_EXT_RECSIZE, REFS_EXT_LENGTH);
            RfsPut64(rec + REFS_EXT_VCN, e->Vcn);
            RfsPut32(rec + REFS_EXT_RUN, e->Run);
            st = RfsTreeInsertRecord(S, rec, REFS_EXT_LENGTH);
            if (!RFS_SUCCESS(st)) return st;
        }
    }
    {
        PUCHAR h = r->Desc + REFS_IR_DATA;
        ULONG o = RfsGet32(h + 4) == 0x28 ? 0 : 4;
        RfsPut64(h + SH_ALLOC + o, F->Alloc);
        RfsPut64(h + SH_SIZE + o, F->Size);
        RfsPut64(h + SH_VALID + o, F->ValidSize);
        RfsPut64(h + SH_ALLOC2 + o, F->Alloc);
        r->Dirty = TRUE;
    }
    return RFS_OK;
}

/* the file's table with its header, $DATA and extents from F; *Val the new row value */
static RFS_STATUS
StoreRecord(PRFS_VOL V, PRFS_FILE F, PRFS_TABLE Dir, const UCHAR *Old, ULONG OldLen, PUCHAR *Val, PULONG Len)
{
    ULONGLONG Owner = Dir->Oid;
    PRFS_TABLE T;
    RFS_CURSOR c;
    UCHAR key[40];
    RFS_STATUS st;
    BOOLEAN found = FALSE;
    st = RfsTableOpenEmbedded(V, Owner, Old, OldLen, &T);
    if (!RFS_SUCCESS(st)) return st;
    T->Owner = Dir;
    if (T->Root->DescLen >= REFS_FH_ALLOC + 8) {
        PUCHAR d = T->Root->Desc;
        RfsPut64(d + REFS_FH_CTIME, F->Times.Create);
        RfsPut64(d + REFS_FH_MTIME, F->Times.Write);
        RfsPut64(d + REFS_FH_CHTIME, F->Times.Change);
        RfsPut64(d + REFS_FH_ATIME, F->Times.Access);
        RfsPut32(d + REFS_FH_ATTR, F->Attributes & ~0x10UL);
        RfsPut64(d + REFS_FH_SIZE, F->Size);
        RfsPut64(d + REFS_FH_ALLOC, F->Alloc);
        T->Root->Dirty = TRUE;
    }
    if (!F->Inline) {
        /* the live stream row */
        st = RfsSeek(T, NULL, 0, &c);
        while (RFS_SUCCESS(st)) {
            PRFS_ROW r = RfsCurRow(&c);
            if (r->KeyLen >= 24 && RfsGet32(r->Key + 8) == REFS_AT_MARKER_MULTI && RfsGet16(r->Key + 12) == REFS_AT_DATA &&
                RfsGet64(r->Key + 16) == REFS_STREAM_LIVE && (r->Flags & REFS_RF_EMBEDDED)) {
                PRFS_TABLE S;
                PUCHAR sv;
                ULONG sl, kl = r->KeyLen;
                found = TRUE;
                st = RfsTableOpenEmbedded(V, Owner, r->Val, r->ValLen, &S);
                if (!RFS_SUCCESS(st)) break;
                S->Owner = Dir;
                st = StoreExtents(V, F, S);
                if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(S, &sv, &sl);
                RfsTableClose(V, S);
                if (!RFS_SUCCESS(st)) break;
                if (kl > sizeof(key)) kl = sizeof(key);
                RfsCopy(key, r->Key, kl);
                RfsPut64(key, sl);
                st = RfsTreeReplaceAt(&c, key, kl, sv, sl, r->Flags);
                RfsHostFree(sv);
                break;
            }
            st = RfsNext(&c);
        }
        RfsCursorClose(&c);
        if (st == RFS_NO_MORE) st = RFS_OK;
        if (RFS_SUCCESS(st) && !found) {
            /* no stream yet (an inline file made resident-free, or a file without $DATA) */
            UCHAR sdesc[STREAM_DESC], set[0x28];
            PRFS_TABLE S;
            PUCHAR sv;
            ULONG sl;
            DescInit(sdesc, STREAM_DESC, REFS_SCHEMA_STREAM);
            RfsPut32(sdesc + REFS_IR_DATA + 4, 0x28);
            RfsPut64(sdesc + REFS_IR_DATA + SH_ONE, 1);
            st = RfsNewTable(V, RFS_TROOT_EMBEDDED, Owner, REFS_SCHEMA_STREAM, sdesc, STREAM_DESC, &S);
            if (RFS_SUCCESS(st)) {
                S->Owner = Dir;
                S->Root->Flags = REFS_NF_ROOT | REFS_NF_RANGE | REFS_NF_BARE;
                F->ExtChanged = TRUE;
                st = StoreExtents(V, F, S);
                if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(S, &sv, &sl);
                RfsTableClose(V, S);
                if (RFS_SUCCESS(st)) {
                    RfsZero(set, sizeof(set));
                    RfsPut64(set, 0x1001);
                    RfsPut64(set + 8, 1);
                    StreamKey(key, sizeof(set), REFS_STREAM_META, 8, 1);
                    st = RfsTreeInsert(T, key, 40, set, sizeof(set), 0, FALSE);
                    if (st == RFS_EXISTS) st = RFS_OK;
                    StreamKey(key, sl, REFS_STREAM_LIVE, 8, 0);
                    if (RFS_SUCCESS(st)) st = RfsTreeInsert(T, key, 40, sv, sl, REFS_RF_EMBEDDED, FALSE);
                    RfsHostFree(sv);
                }
            }
        }
    }
    if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(T, Val, Len);
    RfsTableClose(V, T);
    return st;
}

/* the index entry of a file whose record is elsewhere (its sizes and times) */
static RFS_STATUS
UpdateIndexEntry(PRFS_VOL V, PRFS_FILE F)
{
    PRFS_TABLE D;
    RFS_CURSOR c;
    PUCHAR val;
    RFS_STATUS st = RfsTableOpenObject(V, F->Parent, &D);
    if (!RFS_SUCCESS(st)) return st;
    st = FindName(D, F->Name, F->NameLen, &c);
    if (RFS_SUCCESS(st)) {
        if (RfsCurRow(&c)->ValLen >= REFS_IE_ATTR + 4) {
            st = RfsTreeModifyAt(&c, &val);
            if (RFS_SUCCESS(st)) {
                RfsPut64(val + REFS_IE_CTIME, F->Times.Create);
                RfsPut64(val + REFS_IE_MTIME, F->Times.Write);
                RfsPut64(val + REFS_IE_CHTIME, F->Times.Change);
                RfsPut64(val + REFS_IE_ATIME, F->Times.Access);
                RfsPut64(val + REFS_IE_ALLOC, F->Alloc);
                RfsPut64(val + REFS_IE_SIZE, F->Size);
                RfsPut32(val + REFS_IE_ATTR, (RfsGet32(val + REFS_IE_ATTR) & REFS_ATTR_DIRECTORY) | (F->Attributes & ~0x10UL));
            }
        }
        RfsCursorClose(&c);
    }
    RfsTableClose(V, D);
    return st;
}

static RFS_STATUS
SaveDirTimes(PRFS_VOL V, PRFS_FILE F)
{
    PRFS_TABLE D;
    RFS_CURSOR c;
    UCHAR key[4];
    PUCHAR val;
    RFS_STATUS st = RfsTableOpenObject(V, F->Oid, &D);
    if (!RFS_SUCCESS(st)) return st;
    RfsPut32(key, REFS_KEY_DIRSELF);
    st = RfsLookup(D, key, 4, &c);
    if (RFS_SUCCESS(st) && RfsCurRow(&c)->ValLen >= REFS_FH_ATTR + 4) {
        st = RfsTreeModifyAt(&c, &val);
        if (RFS_SUCCESS(st)) {
            RfsPut64(val + REFS_FH_CTIME, F->Times.Create);
            RfsPut64(val + REFS_FH_MTIME, F->Times.Write);
            RfsPut64(val + REFS_FH_CHTIME, F->Times.Change);
            RfsPut64(val + REFS_FH_ATIME, F->Times.Access);
            RfsPut32(val + REFS_FH_ATTR, F->Attributes & ~0x10UL);
        }
    } else if (st == RFS_NOT_FOUND) {
        st = RFS_OK;
    }
    RfsCursorClose(&c);
    RfsTableClose(V, D);
    if (RFS_SUCCESS(st) && F->NameLen) st = UpdateIndexEntry(V, F);
    return st;
}

static RFS_STATUS RFS_NOINLINE
FileSave3(PRFS_VOL V, PRFS_FILE F)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    PUCHAR val;
    ULONG vl;
    RFS_STATUS st;
    if (!F->Changed && !F->ExtChanged) return RFS_OK;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (F->Type == RFS_FT_DIR) {
        st = SaveDirTimes(V, F);
        if (RFS_SUCCESS(st)) F->Changed = FALSE;
        return st;
    }
    st = RecordRow(V, F, &T, &c);
    if (!RFS_SUCCESS(st)) return st;
    {
        PRFS_ROW r = RfsCurRow(&c);
        USHORT flags = r->Flags;
        st = StoreRecord(V, F, T, r->Val, r->ValLen, &val, &vl);
        if (RFS_SUCCESS(st)) {
            st = RfsTreeUpdateAt(&c, val, vl, flags);
            RfsHostFree(val);
        } else {
            RfsCursorClose(&c);
        }
    }
    RfsTableClose(V, T);
    if (RFS_SUCCESS(st) && F->KeyFlags == REFS_NAME_INDEX) st = UpdateIndexEntry(V, F);
    if (RFS_SUCCESS(st)) F->Changed = F->ExtChanged = FALSE;
    return st;
}

RFS_STATUS
RfsFileSave(PRFS_VOL V, PRFS_FILE F)
{
    if (RFS_V1REC(V)) {
        if (!F->Changed && !F->ExtChanged) return RFS_OK;
        if (V->ReadOnly) return RFS_READ_ONLY;
        return RfsFileSaveV1(V, F);
    }
    return FileSave3(V, F);
}

/* ---- data ------------------------------------------------------------------------------------------------ */
static RFS_STATUS
ExtAppend(PRFS_FILE F, ULONGLONG Vcn, ULONGLONG Vlcn, ULONG Run)
{
    if (F->NExt) {
        PRFS_EXTENT e = &F->Ext[F->NExt - 1];
        if (e->Vcn + e->Run == Vcn && e->Lcn && e->Lcn + e->Run == Vlcn && e->Flags == EXTF_NEW && e->Run + Run > e->Run) {
            e->Run += Run;
            return RFS_OK;
        }
    }
    if (F->NExt == F->CapExt) {
        ULONG cap = F->CapExt ? F->CapExt * 2 : 16;
        PRFS_EXTENT n = (PRFS_EXTENT)RfsHostAlloc(cap * sizeof(RFS_EXTENT));
        if (!n) return RFS_NO_MEMORY;
        if (F->Ext) { RfsCopy(n, F->Ext, F->NExt * sizeof(RFS_EXTENT)); RfsHostFree(F->Ext); }
        F->Ext = n;
        F->CapExt = cap;
    }
    F->Ext[F->NExt].Vcn = Vcn;
    F->Ext[F->NExt].Lcn = Vlcn;
    F->Ext[F->NExt].Run = Run;
    F->Ext[F->NExt].Flags = EXTF_NEW;
    F->NExt++;
    return RFS_OK;
}

/* clusters F's extents cover (the end of the last one) */
static ULONGLONG
ExtEnd(PRFS_FILE F)
{
    return F->NExt ? F->Ext[F->NExt - 1].Vcn + F->Ext[F->NExt - 1].Run : 0;
}

static RFS_STATUS
WriteZeros(PRFS_VOL V, PRFS_FILE F, ULONGLONG From, ULONGLONG To);

/* an inline file's data move to clusters */
static RFS_STATUS
MakeNonResident(PRFS_VOL V, PRFS_FILE F)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    RFS_STATUS st;
    PUCHAR data = F->InlineData;
    ULONG n = F->InlineLen;
    /* the inline $DATA row goes */
    st = RecordRow(V, F, &T, &c);
    if (!RFS_SUCCESS(st)) return st;
    {
        PRFS_ROW r = RfsCurRow(&c);
        PRFS_TABLE R;
        RFS_CURSOR c2;
        PUCHAR val;
        ULONG vl;
        USHORT flags = r->Flags;
        st = RfsTableOpenEmbedded(V, T->Oid, r->Val, r->ValLen, &R);
        if (RFS_SUCCESS(st)) {
            st = RfsSeek(R, NULL, 0, &c2);
            while (RFS_SUCCESS(st)) {
                PRFS_ROW a = RfsCurRow(&c2);
                if (a->KeyLen >= 16 && RfsGet32(a->Key + 8) == REFS_AT_MARKER_SINGLE && RfsGet16(a->Key + 12) == REFS_AT_DATA) {
                    st = RfsTreeDeleteAt(&c2);
                    break;
                }
                st = RfsNext(&c2);
            }
            RfsCursorClose(&c2);
            if (st == RFS_NO_MORE) st = RFS_OK;
            if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(R, &val, &vl);
            RfsTableClose(V, R);
            if (RFS_SUCCESS(st)) {
                st = RfsTreeUpdateAt(&c, val, vl, flags);
                RfsHostFree(val);
            } else RfsCursorClose(&c);
        } else RfsCursorClose(&c);
    }
    RfsTableClose(V, T);
    if (!RFS_SUCCESS(st)) return st;
    F->Inline = FALSE;
    F->InlineData = NULL;
    F->InlineLen = 0;
    F->Alloc = 0;
    F->ValidSize = 0;
    {
        ULONGLONG size = F->Size;
        F->Size = 0;
        F->ExtChanged = TRUE;
        st = RfsFileSetSize(V, F, size, FALSE);
        if (RFS_SUCCESS(st) && n) st = RfsFileWrite(V, F, 0, n, data);
    }
    RfsHostFree(data);
    return st;
}

static RFS_STATUS
CheckWritable(PRFS_VOL V, PRFS_FILE F)
{
    ULONG i;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (RFS_V1REC(V)) return RFS_NOT_SUPPORTED;
    if (F->Type != RFS_FT_FILE) return RFS_IS_A_DIRECTORY;
    for (i = 0; i < F->NExt; i++) {
        if (F->Ext[i].Flags & REFS_EXTF_CHECKSUMS) { F->Integrity = TRUE; return RFS_NOT_SUPPORTED; }
    }
    return RFS_OK;
}

/* an empty file takes clusters [Vlcn, Vlcn + Run) the caller allocated, as its whole data */
RFS_STATUS
RfsFileAdopt(PRFS_VOL V, PRFS_FILE F, ULONGLONG Vlcn, ULONG Run)
{
    RFS_STATUS st = CheckWritable(V, F);
    if (!RFS_SUCCESS(st)) return st;
    if (F->Size) return RFS_INVALID;
    if (F->Inline) {
        st = MakeNonResident(V, F);
        if (!RFS_SUCCESS(st)) return st;
    }
    if (F->NExt) return RFS_INVALID;
    st = ExtAppend(F, 0, Vlcn, Run);
    if (!RFS_SUCCESS(st)) return st;
    F->Size = F->ValidSize = F->Alloc = (ULONGLONG)Run << V->ClusterShift;
    F->ExtChanged = F->Changed = TRUE;
    return RfsFileSave(V, F);
}

RFS_STATUS
RfsFileSetSize(PRFS_VOL V, PRFS_FILE F, ULONGLONG Size, BOOLEAN KeepAlloc)
{
    ULONG cs = V->ClusterSize;
    ULONGLONG need = (Size + cs - 1) >> V->ClusterShift, have;
    RFS_STATUS st;
    if (RFS_V1REC(V)) return RfsFileSetSizeV1(V, F, Size, KeepAlloc);
    st = CheckWritable(V, F);
    if (!RFS_SUCCESS(st)) return st;
    if (F->Inline) {
        if (Size == F->Size) return RFS_OK;
        st = MakeNonResident(V, F);
        if (!RFS_SUCCESS(st)) return st;
    }
    have = ExtEnd(F);
    if (need > have) {
        /* more clusters, in as few runs as the allocator gives */
        while (have < need) {
            ULONGLONG p, vl;
            ULONG got, want = (ULONG)((need - have) > 0x10000000ULL ? 0x10000000UL : (need - have));
            st = RfsAlloc(V, RFS_TIER_MEDIUM, RFS_FOR_DATA, 1, want, 1, &p, &got);
            if (!RFS_SUCCESS(st)) break;
            st = RfsToVirtual(V, p, &vl);
            if (RFS_SUCCESS(st)) st = ExtAppend(F, have, vl, got);
            if (!RFS_SUCCESS(st)) break;
            have += got;
        }
        F->ExtChanged = TRUE;
        F->Alloc = have << V->ClusterShift;
        if (!RFS_SUCCESS(st)) {
            /* what was taken stays (as allocation); the size does not grow */
            F->Changed = TRUE;
            return st;
        }
    } else if (need < have && !KeepAlloc) {
        /* clusters past the end go */
        while (F->NExt) {
            PRFS_EXTENT e = &F->Ext[F->NExt - 1];
            ULONG keep;
            if (e->Vcn >= need) {
                if (e->Lcn && !(e->Flags & REFS_EXTF_SPARSE)) {
                    st = RfsFreeData(V, e->Lcn, e->Run);
                    if (!RFS_SUCCESS(st)) return st;
                }
                F->NExt--;
                continue;
            }
            if (e->Vcn + e->Run <= need) break;
            keep = (ULONG)(need - e->Vcn);
            if (e->Lcn && !(e->Flags & REFS_EXTF_SPARSE)) {
                st = RfsFreeData(V, e->Lcn + keep, e->Run - keep);
                if (!RFS_SUCCESS(st)) return st;
            }
            e->Run = keep;
            break;
        }
        F->ExtChanged = TRUE;
        F->Alloc = ExtEnd(F) << V->ClusterShift;
    }
    F->Size = Size;
    if (F->ValidSize > Size) F->ValidSize = Size;
    F->Changed = TRUE;
    return RFS_OK;
}

/*
 * Clusters [the end of the extents, Clusters) are allocated; the new ones
 * are zeroed but those in [Keep, Keep + KeepCount) (the caller writes them).
 */
RFS_STATUS
RfsFileAllocTo(PRFS_VOL V, PRFS_FILE F, ULONGLONG Clusters, ULONGLONG Keep, ULONGLONG KeepCount)
{
    ULONGLONG have;
    RFS_STATUS st;
    if (RFS_V1REC(V)) return RfsFileAllocToV1(V, F, Clusters, Keep, KeepCount);
    st = CheckWritable(V, F);
    if (!RFS_SUCCESS(st)) return st;
    if (F->Inline) {
        st = MakeNonResident(V, F);
        if (!RFS_SUCCESS(st)) return st;
    }
    have = ExtEnd(F);
    while (have < Clusters) {
        ULONGLONG p, vl;
        ULONG got, want = (ULONG)((Clusters - have) > 0x10000000ULL ? 0x10000000UL : (Clusters - have));
        st = RfsAlloc(V, RFS_TIER_MEDIUM, RFS_FOR_DATA, 1, want, 1, &p, &got);
        if (!RFS_SUCCESS(st)) break;
        st = RfsToVirtual(V, p, &vl);
        if (RFS_SUCCESS(st)) st = ExtAppend(F, have, vl, got);
        if (!RFS_SUCCESS(st)) { RfsFree(V, RFS_TIER_MEDIUM, p, got); break; }
        F->ExtChanged = F->Changed = TRUE;
        F->Alloc = (have + got) << V->ClusterShift;
        /* zeros where the caller does not write */
        {
            ULONGLONG a = have, b = have + got;
            ULONGLONG ka = Keep, kb = Keep + KeepCount;
            if (ka < a) ka = a;
            if (kb > b) kb = b;
            if (ka >= kb) { ka = kb = b; }
            if (a < ka) st = WriteZeros(V, F, a << V->ClusterShift, ka << V->ClusterShift);
            if (RFS_SUCCESS(st) && kb < b) st = WriteZeros(V, F, kb << V->ClusterShift, b << V->ClusterShift);
            if (!RFS_SUCCESS(st)) break;
        }
        have += got;
    }
    return st;
}

/* clusters from Clusters on are freed (the size is the caller's) */
RFS_STATUS
RfsFileTruncTo(PRFS_VOL V, PRFS_FILE F, ULONGLONG Clusters)
{
    RFS_STATUS st;
    if (RFS_V1REC(V)) return RfsFileTruncToV1(V, F, Clusters);
    st = CheckWritable(V, F);
    if (!RFS_SUCCESS(st)) return st;
    if (F->Inline) return RFS_OK;
    while (F->NExt) {
        PRFS_EXTENT e = &F->Ext[F->NExt - 1];
        ULONG keep;
        if (e->Vcn >= Clusters) {
            if (e->Lcn && !(e->Flags & REFS_EXTF_SPARSE)) {
                st = RfsFreeData(V, e->Lcn, e->Run);
                if (!RFS_SUCCESS(st)) return st;
            }
            F->NExt--;
            F->ExtChanged = TRUE;
            continue;
        }
        if (e->Vcn + e->Run <= Clusters) break;
        keep = (ULONG)(Clusters - e->Vcn);
        if (e->Lcn && !(e->Flags & REFS_EXTF_SPARSE)) {
            st = RfsFreeData(V, e->Lcn + keep, e->Run - keep);
            if (!RFS_SUCCESS(st)) return st;
        }
        e->Run = keep;
        F->ExtChanged = TRUE;
        break;
    }
    F->Alloc = ExtEnd(F) << V->ClusterShift;
    F->Changed = TRUE;
    return RFS_OK;
}

/* a directory's parent and name, from its record's link */
RFS_STATUS
RfsDirParent(PRFS_VOL V, ULONGLONG Oid, PULONGLONG Parent, USHORT *Name, PULONG Len)
{
    PRFS_TABLE D, R;
    RFS_CURSOR c, c2;
    UCHAR key[4];
    RFS_STATUS st;
    *Parent = REFS_OID_ROOT;
    if (Len) *Len = 0;
    if (Oid == REFS_OID_ROOT) return RFS_OK;
    st = RfsTableOpenObject(V, Oid, &D);
    if (!RFS_SUCCESS(st)) return st;
    RfsPut32(key, REFS_KEY_DIRSELF);
    st = RfsLookup(D, key, 4, &c);
    if (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        st = RfsTableOpenEmbedded(V, Oid, r->Val, r->ValLen, &R);
        if (RFS_SUCCESS(st)) {
            st = RfsSeek(R, NULL, 0, &c2);
            while (RFS_SUCCESS(st)) {
                PRFS_ROW a = RfsCurRow(&c2);
                if (a->KeyLen >= 32 && RfsGet16(a->Key + 12) == REFS_AT_OBJLINK) {
                    ULONG n = (a->KeyLen - 32) / 2, i;
                    *Parent = RfsGet64(a->Key + 16);
                    if (Name && Len) {
                        if (n > RFS_MAX_NAME) n = RFS_MAX_NAME;
                        for (i = 0; i < n; i++) Name[i] = RfsGet16(a->Key + 32 + 2 * i);
                        *Len = n;
                    }
                    break;
                }
                st = RfsNext(&c2);
            }
            RfsCursorClose(&c2);
            if (st == RFS_NO_MORE) st = RFS_NOT_FOUND;
            RfsTableClose(V, R);
        }
    }
    RfsCursorClose(&c);
    RfsTableClose(V, D);
    return st;
}

/* ---- moving a file's clusters (the boot view) ------------------------------------------------------ */
/* does every cluster of the file lie in [Lo, Hi) (physical), none missing? */
BOOLEAN
RfsFilePlaced(PRFS_VOL V, PRFS_FILE F, ULONGLONG Lo, ULONGLONG Hi)
{
    ULONGLONG need = (F->Size + V->ClusterSize - 1) >> V->ClusterShift, vcn = 0;
    ULONG i;
    if (F->Inline) return (BOOLEAN)(F->Size == 0);
    for (i = 0; i < F->NExt && vcn < need; i++) {
        PRFS_EXTENT e = &F->Ext[i];
        ULONG k = 0;
        if (e->Vcn != vcn || !e->Lcn || (e->Flags & REFS_EXTF_SPARSE)) return FALSE;
        while (k < e->Run) {
            ULONGLONG p;
            ULONG run;
            if (!RFS_SUCCESS(RfsTranslate(V, e->Lcn + k, &p, &run))) return FALSE;
            if (run > e->Run - k) run = e->Run - k;
            if (p < Lo || p + run > Hi) return FALSE;
            k += run;
        }
        vcn += e->Run;
    }
    return (BOOLEAN)(vcn >= need);
}

typedef struct _MOVEPIECE {
    ULONGLONG   Vcn, OldVlcn, NewVlcn;
    ULONG       Run;
    BOOLEAN     Hole;
} MOVEPIECE;

static RFS_STATUS
CopyClusters(PRFS_VOL V, ULONGLONG FromP, ULONGLONG ToP, ULONG Count, PUCHAR Buf, ULONG BufClusters, BOOLEAN Zero)
{
    while (Count) {
        ULONG n = Count > BufClusters ? BufClusters : Count;
        RFS_STATUS st;
        if (Zero) RfsZero(Buf, n << V->ClusterShift);
        else {
            st = RfsHostRead(V, FromP << V->ClusterShift, n << V->ClusterShift, Buf);
            if (!RFS_SUCCESS(st)) return st;
        }
        st = RfsHostWrite(V, ToP << V->ClusterShift, n << V->ClusterShift, Buf);
        if (!RFS_SUCCESS(st)) return st;
        FromP += n; ToP += n; Count -= n;
    }
    return RFS_OK;
}

/*
 * Moves the clusters of the file that lie outside [Lo, Hi) (Into) or inside
 * it (!Into) to the other side, copying their data; the record is saved with
 * the new extents before the old clusters are freed. *Moved: clusters moved.
 */
RFS_STATUS
RfsFileRelocate(PRFS_VOL V, PRFS_FILE F, ULONGLONG Lo, ULONGLONG Hi, BOOLEAN Into, PULONG Moved)
{
    ULONGLONG need = (F->Size + V->ClusterSize - 1) >> V->ClusterShift, vcn;
    MOVEPIECE *mv = NULL;
    ULONG nmv = 0, cap = 0, i, bufc = 16;
    PRFS_EXTENT ne = NULL;
    ULONG nne = 0, capne = 0;
    PUCHAR buf = NULL;
    RFS_STATUS st = CheckWritable(V, F);
    *Moved = 0;
    if (!RFS_SUCCESS(st)) return st;
    if (F->Inline) {
        if (F->Size == 0) return RFS_OK;
        st = MakeNonResident(V, F);
        if (!RFS_SUCCESS(st)) return st;
    }
    if (F->NExt == 0 && need == 0) return RFS_OK;
    buf = (PUCHAR)RfsHostAlloc(bufc << V->ClusterShift);
    if (!buf) return RFS_NO_MEMORY;
#define ADDPIECE(V_, O_, R_, H_) do { \
        if (nmv == cap) { MOVEPIECE *x = (MOVEPIECE *)RfsHostAlloc((cap ? cap * 2 : 16) * sizeof(MOVEPIECE)); \
            if (!x) { st = RFS_NO_MEMORY; goto out; } if (mv) { RfsCopy(x, mv, nmv * sizeof(MOVEPIECE)); RfsHostFree(mv); } \
            mv = x; cap = cap ? cap * 2 : 16; } \
        mv[nmv].Vcn = (V_); mv[nmv].OldVlcn = (O_); mv[nmv].Run = (R_); mv[nmv].Hole = (H_); mv[nmv].NewVlcn = 0; nmv++; } while (0)
    /* the pieces on the wrong side (and, moving in, the holes below the size) */
    vcn = 0;
    for (i = 0; i < F->NExt || (Into && vcn < need); ) {
        if (i < F->NExt && F->Ext[i].Vcn == vcn) {
            PRFS_EXTENT e = &F->Ext[i];
            ULONG k = 0;
            if (!e->Lcn || (e->Flags & REFS_EXTF_SPARSE)) {
                if (Into && e->Vcn < need) ADDPIECE(e->Vcn, 0, e->Run, TRUE);
            } else {
                while (k < e->Run) {
                    ULONGLONG p;
                    ULONG run;
                    BOOLEAN inside;
                    st = RfsTranslate(V, e->Lcn + k, &p, &run);
                    if (!RFS_SUCCESS(st)) goto out;
                    if (run > e->Run - k) run = e->Run - k;
                    inside = (BOOLEAN)(p >= Lo && p < Hi);
                    if (inside) {
                        if (p + run > Hi) run = (ULONG)(Hi - p);
                    } else if (p < Lo && p + run > Lo) {
                        run = (ULONG)(Lo - p);
                    }
                    if (inside != Into) ADDPIECE(e->Vcn + k, e->Lcn + k, run, FALSE);
                    k += run;
                }
            }
            vcn = e->Vcn + e->Run;
            i++;
        } else {
            /* a hole up to the next extent (or the size) */
            ULONGLONG end = i < F->NExt ? F->Ext[i].Vcn : need;
            if (end <= vcn) { if (i < F->NExt) { vcn = F->Ext[i].Vcn; continue; } break; }
            if (Into) ADDPIECE(vcn, 0, (ULONG)(end - vcn), TRUE);
            vcn = end;
        }
    }
    if (!nmv) goto out;
    /* new clusters, the data copied */
    for (i = 0; i < nmv; i++) {
        MOVEPIECE *m = &mv[i];
        ULONG done = 0;
        while (done < m->Run) {
            ULONGLONG np, vl, op = 0;
            ULONG got, orun = m->Run - done;
            if (Into) st = RfsAllocIn(V, Lo, Hi, 1, m->Run - done, 1, &np, &got);
            else st = RfsAlloc(V, RFS_TIER_MEDIUM, RFS_FOR_DATA, 1, m->Run - done, 1, &np, &got);
            if (!RFS_SUCCESS(st)) goto out;
            if (!m->Hole) {
                st = RfsTranslate(V, m->OldVlcn + done, &op, &orun);
                if (RFS_SUCCESS(st) && orun < got) {
                    /* the old run ends at a container: give back the rest */
                    RfsFree(V, RFS_TIER_MEDIUM, np + orun, got - orun);
                    got = orun;
                }
            }
            if (RFS_SUCCESS(st)) st = CopyClusters(V, op, np, got, buf, bufc, m->Hole);
            if (RFS_SUCCESS(st)) st = RfsToVirtual(V, np, &vl);
            if (!RFS_SUCCESS(st)) { RfsFree(V, RFS_TIER_MEDIUM, np, got); goto out; }
            /* the piece splits where the new space does */
            if (got < m->Run - done) {
                MOVEPIECE rest = *m;
                ADDPIECE(0, 0, 0, FALSE);           /* room */
                m = &mv[i];
                RfsMove(&mv[i + 2], &mv[i + 1], (nmv - i - 2) * sizeof(MOVEPIECE));
                mv[i + 1] = rest;
                mv[i + 1].Vcn = m->Vcn + done + got;
                if (!rest.Hole) mv[i + 1].OldVlcn = m->OldVlcn + done + got;
                mv[i + 1].Run = m->Run - done - got;
                m->Run = done + got;
            }
            m->NewVlcn = vl - done;             /* (the piece's start) */
            done += got;
            *Moved += got;
        }
    }
    /* the new extent list: old extents with the moved pieces cut out and put back */
    {
        ULONG pi = 0;
        vcn = 0;
        for (i = 0; i <= F->NExt; i++) {
            ULONGLONG ev = i < F->NExt ? F->Ext[i].Vcn : ~0ULL;
            /* pieces (holes) before this extent */
            while (pi < nmv && mv[pi].Vcn < ev && (i == F->NExt || mv[pi].Vcn < F->Ext[i].Vcn)) {
                RFS_EXTENT x;
                x.Vcn = mv[pi].Vcn; x.Lcn = mv[pi].NewVlcn; x.Run = mv[pi].Run; x.Flags = EXTF_NEW;
                if (nne == capne) { PRFS_EXTENT y = (PRFS_EXTENT)RfsHostAlloc((capne ? capne * 2 : 16) * sizeof(RFS_EXTENT)); if (!y) { st = RFS_NO_MEMORY; goto out; } if (ne) { RfsCopy(y, ne, nne * sizeof(RFS_EXTENT)); RfsHostFree(ne); } ne = y; capne = capne ? capne * 2 : 16; }
                ne[nne++] = x;
                pi++;
            }
            if (i == F->NExt) break;
            {
                PRFS_EXTENT e = &F->Ext[i];
                ULONGLONG v = e->Vcn, end = e->Vcn + e->Run;
                while (v < end) {
                    RFS_EXTENT x;
                    if (pi < nmv && mv[pi].Vcn == v) {
                        x.Vcn = v; x.Lcn = mv[pi].NewVlcn; x.Run = mv[pi].Run; x.Flags = EXTF_NEW;
                        v += mv[pi].Run;
                        pi++;
                    } else {
                        ULONGLONG stop = (pi < nmv && mv[pi].Vcn < end) ? mv[pi].Vcn : end;
                        x.Vcn = v; x.Lcn = e->Lcn ? e->Lcn + (v - e->Vcn) : 0; x.Run = (ULONG)(stop - v); x.Flags = e->Flags;
                        v = stop;
                    }
                    if (nne == capne) { PRFS_EXTENT y = (PRFS_EXTENT)RfsHostAlloc((capne ? capne * 2 : 16) * sizeof(RFS_EXTENT)); if (!y) { st = RFS_NO_MEMORY; goto out; } if (ne) { RfsCopy(y, ne, nne * sizeof(RFS_EXTENT)); RfsHostFree(ne); } ne = y; capne = capne ? capne * 2 : 16; }
                    /* contiguous with the last: one extent */
                    if (nne && ne[nne - 1].Vcn + ne[nne - 1].Run == x.Vcn && ne[nne - 1].Lcn && x.Lcn &&
                        ne[nne - 1].Lcn + ne[nne - 1].Run == x.Lcn && ne[nne - 1].Flags == x.Flags && (x.Lcn >> V->ContainerShift) == (ne[nne - 1].Lcn >> V->ContainerShift))
                        ne[nne - 1].Run += x.Run;
                    else ne[nne++] = x;
                }
            }
        }
    }
    {
        PRFS_EXTENT old = F->Ext;
        ULONG oldn = F->NExt;
        F->Ext = ne; F->NExt = nne; F->CapExt = capne;
        ne = NULL;
        F->ExtChanged = F->Changed = TRUE;
        if (F->Alloc < (need << V->ClusterShift)) F->Alloc = need << V->ClusterShift;
        st = RfsFileSave(V, F);
        if (!RFS_SUCCESS(st)) { RfsHostFree(old); goto out; }
        /* the old clusters go */
        for (i = 0; i < nmv; i++) {
            if (!mv[i].Hole) {
                RFS_STATUS s2 = RfsFreeData(V, mv[i].OldVlcn, mv[i].Run);
                if (!RFS_SUCCESS(s2) && RFS_SUCCESS(st)) st = s2;
            }
        }
        RfsHostFree(old);
        (VOID)oldn;
    }
out:
#undef ADDPIECE
    if (!RFS_SUCCESS(st) && mv && ne) {
        /* the new extents were not installed: their clusters go back */
        for (i = 0; i < nmv; i++) {
            if (mv[i].NewVlcn) {
                ULONGLONG p;
                ULONG run;
                if (RFS_SUCCESS(RfsTranslate(V, mv[i].NewVlcn, &p, &run))) RfsFree(V, RFS_TIER_MEDIUM, p, mv[i].Run);
            }
        }
    } else if (!RFS_SUCCESS(st) && mv && !ne && *Moved && F->Ext != NULL) {
        /* failed while copying: the same */
        for (i = 0; i < nmv; i++) {
            if (mv[i].NewVlcn) {
                ULONGLONG p;
                ULONG run;
                BOOLEAN used = FALSE;
                ULONG k;
                for (k = 0; k < F->NExt; k++) if (F->Ext[k].Lcn == mv[i].NewVlcn) used = TRUE;
                if (!used && RFS_SUCCESS(RfsTranslate(V, mv[i].NewVlcn, &p, &run))) RfsFree(V, RFS_TIER_MEDIUM, p, mv[i].Run);
            }
        }
    }
    if (mv) RfsHostFree(mv);
    if (ne) RfsHostFree(ne);
    if (buf) RfsHostFree(buf);
    return st;
}

RFS_STATUS
RfsFileSetValid(PRFS_VOL V, PRFS_FILE F, ULONGLONG Valid)
{
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (Valid > F->Size) Valid = F->Size;
    if (Valid != F->ValidSize) {
        F->ValidSize = Valid;
        F->Changed = TRUE;
    }
    return RFS_OK;
}

/* Len bytes at Off of the file's clusters (allocated) */
static RFS_STATUS
WriteRaw(PRFS_VOL V, PRFS_FILE F, ULONGLONG Off, ULONG Len, const UCHAR *Buf)
{
    ULONG done = 0, cs = V->ClusterSize;
    while (done < Len) {
        ULONGLONG pos = Off + done, plcn;
        ULONG within = (ULONG)(pos & (cs - 1)), run, n;
        RFS_STATUS st = RfsFileMap(V, F, pos >> V->ClusterShift, &plcn, &run);
        if (!RFS_SUCCESS(st)) return st;
        if (!plcn) return RFS_CORRUPT;                  /* (holes are not made here) */
        n = Len - done;
        if ((ULONGLONG)run * cs - within < n) n = (ULONG)((ULONGLONG)run * cs - within);
        if (Buf) st = RfsHostWrite(V, (plcn << V->ClusterShift) + within, n, Buf + done);
        else {
            /* zeros, a cluster at a time */
            ULONG k = 0;
            PUCHAR z = (PUCHAR)RfsAllocZero(cs);
            if (!z) return RFS_NO_MEMORY;
            while (RFS_SUCCESS(st) && k < n) {
                ULONG m = n - k > cs ? cs : n - k;
                st = RfsHostWrite(V, (plcn << V->ClusterShift) + within + k, m, z);
                k += m;
            }
            RfsHostFree(z);
        }
        if (!RFS_SUCCESS(st)) return st;
        done += n;
    }
    return RFS_OK;
}

static RFS_STATUS
WriteZeros(PRFS_VOL V, PRFS_FILE F, ULONGLONG From, ULONGLONG To)
{
    while (From < To) {
        ULONG n = (To - From) > 0x100000 ? 0x100000 : (ULONG)(To - From);
        RFS_STATUS st = WriteRaw(V, F, From, n, NULL);
        if (!RFS_SUCCESS(st)) return st;
        From += n;
    }
    return RFS_OK;
}

RFS_STATUS
RfsFileWrite(PRFS_VOL V, PRFS_FILE F, ULONGLONG Off, ULONG Len, const VOID *Buf)
{
    RFS_STATUS st;
    if (RFS_V1REC(V)) return RfsFileWriteV1(V, F, Off, Len, Buf);
    st = CheckWritable(V, F);
    if (!RFS_SUCCESS(st)) return st;
    if (!Len) return RFS_OK;
    if (F->Inline) {
        st = MakeNonResident(V, F);
        if (!RFS_SUCCESS(st)) return st;
    }
    if (Off + Len > F->Size) {
        st = RfsFileSetSize(V, F, Off + Len, FALSE);
        if (!RFS_SUCCESS(st)) return st;
    }
    if (Off > F->ValidSize) {
        st = WriteZeros(V, F, F->ValidSize, Off);
        if (!RFS_SUCCESS(st)) return st;
    }
    st = WriteRaw(V, F, Off, Len, (const UCHAR *)Buf);
    if (!RFS_SUCCESS(st)) return st;
    if (Off + Len > F->ValidSize) {
        F->ValidSize = Off + Len;
        F->Changed = TRUE;
    }
    return RFS_OK;
}

RFS_STATUS
RfsFileSetInfo(PRFS_VOL V, PRFS_FILE F, const RFS_TIMES *T, ULONG Attr)
{
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (T) {
        if (T->Create) F->Times.Create = T->Create;
        if (T->Write) F->Times.Write = T->Write;
        if (T->Change) F->Times.Change = T->Change;
        if (T->Access) F->Times.Access = T->Access;
    }
    if (Attr) F->Attributes = (F->Attributes & 0x10) | (Attr & ~0x10UL);
    F->Changed = TRUE;
    return RFS_OK;
}

/* ---- removing ----------------------------------------------------------------------------------------- */
/* every page and cluster under a file's table value */
static RFS_STATUS
FreeRecord(PRFS_VOL V, ULONGLONG Owner, const UCHAR *Val, ULONG Len, ULONG Depth)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    RFS_STATUS st;
    BOOLEAN stream;
    if (Depth > 3) return RFS_OK;
    st = RfsTableOpenEmbedded(V, Owner, Val, Len, &T);
    if (!RFS_SUCCESS(st)) return st;
    stream = (BOOLEAN)(T->Rule == 0x0B);
    st = RfsSeek(T, NULL, 0, &c);
    while (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        if (stream) {
            if (r->ValLen >= REFS_EXT_LENGTH) {
                ULONGLONG lcn = RfsGet64(r->Val + REFS_EXT_VLCN);
                USHORT fl = RfsGet16(r->Val + REFS_EXT_FLAGS);
                ULONG run = RfsGet32(r->Val + REFS_EXT_RUN);
                if (lcn && !(fl & REFS_EXTF_SPARSE)) st = RfsFreeData(V, lcn, run);
            }
        } else if (r->Flags & REFS_RF_EMBEDDED) {
            st = FreeRecord(V, Owner, r->Val, r->ValLen, Depth + 1);
        }
        if (!RFS_SUCCESS(st)) break;
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    if (st == RFS_NO_MORE) st = RfsTreeFreeAll(T);
    RfsTableClose(V, T);
    return st;
}

static BOOLEAN
DirEmpty(PRFS_TABLE D)
{
    RFS_CURSOR c;
    UCHAR key[4];
    RFS_STATUS st;
    BOOLEAN empty = TRUE;
    RfsPut16(key, REFS_KEY_NAME);
    RfsPut16(key + 2, 0);
    st = RfsSeek(D, key, 4, &c);
    if (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        if (r->KeyLen >= 2 && RfsGet16(r->Key) == REFS_KEY_NAME) empty = FALSE;
    }
    RfsCursorClose(&c);
    return empty;
}

/* the reverse-index row of FileId in table T, if any */
static RFS_STATUS
DropReverse(PRFS_TABLE T, ULONGLONG FileId)
{
    UCHAR rk[24];
    RFS_STATUS st;
    RevKey(FileId, rk);
    st = RfsTreeDelete(T, rk, 24);
    return st == RFS_NOT_FOUND ? RFS_OK : st;
}

/* removes the name row at C (in D) and what only it held; C is closed */
static RFS_STATUS
RemoveAt(PRFS_VOL V, PRFS_TABLE D, ULONGLONG DirOid, PRFS_CURSOR C, BOOLEAN FreeData)
{
    PRFS_ROW r = RfsCurRow(C);
    USHORT kf = RfsGet16(r->Key + 2);
    RFS_STATUS st;
    if (kf == REFS_NAME_EMBEDDED) {
        ULONGLONG id = r->ValLen >= FH_FILEID + 8 ? RfsGet64(r->Val + FH_FILEID) : 0;
        if (FreeData) {
            st = FreeRecord(V, DirOid, r->Val, r->ValLen, 0);
            if (!RFS_SUCCESS(st)) { RfsCursorClose(C); return st; }
        }
        st = RfsTreeDeleteAt(C);
        if (RFS_SUCCESS(st) && id) st = DropReverse(D, id);
        return st;
    }
    if (r->ValLen < REFS_IE_ATTR + 4) { RfsCursorClose(C); return RFS_CORRUPT; }
    if (RfsGet32(r->Val + REFS_IE_ATTR) & REFS_ATTR_DIRECTORY) {
        ULONGLONG oid = RfsGet64(r->Val + REFS_IE_HOME);
        PRFS_TABLE S;
        PUCHAR nkey;
        ULONG nkl = r->KeyLen;
        if (nkl > RFS_MAX_KEY) { RfsCursorClose(C); return RFS_CORRUPT; }
        nkey = (PUCHAR)RfsHostAlloc(RFS_MAX_KEY);
        if (!nkey) { RfsCursorClose(C); return RFS_NO_MEMORY; }
        RfsCopy(nkey, r->Key, nkl);
        RfsCursorClose(C);
        st = RfsTableOpenObject(V, oid, &S);
        if (!RFS_SUCCESS(st)) { RfsHostFree(nkey); return st; }
        if (FreeData) {
            RFS_CURSOR c2;
            if (!DirEmpty(S)) { RfsTableClose(V, S); RfsHostFree(nkey); return RFS_NOT_EMPTY; }
            /* its record's pages (if it has any) and the table's */
            st = RfsSeek(S, NULL, 0, &c2);
            while (RFS_SUCCESS(st)) {
                PRFS_ROW a = RfsCurRow(&c2);
                if (a->Flags & REFS_RF_EMBEDDED) {
                    st = FreeRecord(V, oid, a->Val, a->ValLen, 0);
                    if (!RFS_SUCCESS(st)) break;
                }
                st = RfsNext(&c2);
            }
            RfsCursorClose(&c2);
            if (st == RFS_NO_MORE) st = RfsTreeFreeAll(S);
            if (RFS_SUCCESS(st)) RfsTableForget(V, S);
            RfsTableClose(V, S);
            if (RFS_SUCCESS(st)) st = ObjectRows(V, oid, FALSE);
            if (RFS_SUCCESS(st)) st = ParentChild(V, DirOid, oid, FALSE);
        } else {
            RfsTableClose(V, S);
        }
        if (RFS_SUCCESS(st)) st = RfsTreeDelete(D, nkey, nkl);      /* the name row again (the cursor was let go) */
        RfsHostFree(nkey);
        return st;
    }
    /* a file whose record is in its home directory (hard links): the name goes, the record when the last does */
    {
        ULONGLONG id = RfsGet64(r->Val + REFS_IE_FILEID), home = RfsGet64(r->Val + REFS_IE_HOME);
        PRFS_TABLE H;
        RFS_CURSOR c2;
        UCHAR key[24];
        st = RfsTreeDeleteAt(C);
        if (!RFS_SUCCESS(st)) return st;
        st = RfsTableOpenObject(V, home, &H);
        if (!RFS_SUCCESS(st)) return st;
        RfsZero(key, 24);
        RfsPut32(key, 0x80000000UL | REFS_KEY_BACKING);
        RfsPut64(key + 8, id);
        RfsPut64(key + 16, home);
        st = RfsLookup(H, key, 24, &c2);
        if (RFS_SUCCESS(st)) {
            PRFS_ROW a = RfsCurRow(&c2);
            ULONGLONG links = a->ValLen >= FH_LINKS + 8 ? RfsGet64(a->Val + FH_LINKS) : 1;
            if (links > 1) {
                PUCHAR val;
                st = RfsTreeModifyAt(&c2, &val);
                if (RFS_SUCCESS(st)) RfsPut64(val + FH_LINKS, links - 1);
                RfsCursorClose(&c2);
            } else if (FreeData) {
                st = FreeRecord(V, home, a->Val, a->ValLen, 0);
                if (RFS_SUCCESS(st)) st = RfsTreeDeleteAt(&c2);
                else RfsCursorClose(&c2);
                if (RFS_SUCCESS(st)) st = DropReverse(H, id);
            } else {
                RfsCursorClose(&c2);
            }
        } else {
            RfsCursorClose(&c2);
            if (st == RFS_NOT_FOUND) st = RFS_OK;
        }
        RfsTableClose(V, H);
        return st;
    }
}

RFS_STATUS
RfsRemove(PRFS_VOL V, ULONGLONG DirOid, const USHORT *Name, ULONG Len)
{
    PRFS_TABLE D;
    RFS_CURSOR c;
    RFS_STATUS st;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (RFS_V1REC(V)) return RfsRemoveV1(V, DirOid, Name, Len);
    st = RfsTableOpenObject(V, DirOid, &D);
    if (!RFS_SUCCESS(st)) return st;
    st = FindName(D, Name, Len, &c);
    if (RFS_SUCCESS(st)) st = RemoveAt(V, D, DirOid, &c, TRUE);
    if (RFS_SUCCESS(st)) st = TouchDir(V, D, TRUE);
    RfsTableClose(V, D);
    return st;
}

/* ---- renaming ------------------------------------------------------------------------------------------ */
/* every page of an embedded table (and those of the tables in its rows) rewritten for a new owner */
static RFS_STATUS
Rehome(PRFS_VOL V, ULONGLONG Owner, const UCHAR *Val, ULONG Len, PUCHAR *Out, PULONG OutLen, ULONG Depth)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    RFS_STATUS st;
    *Out = NULL;
    st = RfsTableOpenEmbedded(V, Owner, Val, Len, &T);
    if (!RFS_SUCCESS(st)) return st;
    if (Depth < 3) {
        st = RfsSeek(T, NULL, 0, &c);
        while (RFS_SUCCESS(st)) {
            PRFS_ROW r = RfsCurRow(&c);
            if ((r->Flags & REFS_RF_EMBEDDED) && T->Rule != 0x0B) {
                PUCHAR nv;
                ULONG nl;
                st = Rehome(V, Owner, r->Val, r->ValLen, &nv, &nl, Depth + 1);
                if (!RFS_SUCCESS(st)) break;
                if (nv) {
                    PUCHAR val;
                    if (nl != r->ValLen) { RfsHostFree(nv); st = RFS_CORRUPT; break; }
                    st = RfsTreeModifyAt(&c, &val);
                    if (RFS_SUCCESS(st)) RfsCopy(val, nv, nl);
                    RfsHostFree(nv);
                    if (!RFS_SUCCESS(st)) break;
                }
            }
            st = RfsNext(&c);
        }
        RfsCursorClose(&c);
        if (st != RFS_NO_MORE && !RFS_SUCCESS(st)) { RfsTableClose(V, T); return st; }
        st = RFS_OK;
    }
    st = RFS_OK;
    if (T->Root->Level) {
        /* the pages below the root: dirty, so they are written anew under the new owner */
        ULONG i;
        st = RFS_OK;
        for (i = 0; RFS_SUCCESS(st) && i < T->Root->NRows; i++) {
            PRFS_NODE ch;
            st = RfsLoadChild(V, T, T->Root, i, &ch);
            if (RFS_SUCCESS(st)) {
                ULONG j;
                ch->Dirty = TRUE;
                for (j = 0; RFS_SUCCESS(st) && ch->Level && j < ch->NRows; j++) {
                    PRFS_NODE g;
                    st = RfsLoadChild(V, T, ch, j, &g);
                    if (RFS_SUCCESS(st)) g->Dirty = TRUE;
                }
            }
        }
        T->Root->Dirty = TRUE;
        if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(T, Out, OutLen);
    }
    RfsTableClose(V, T);
    return st;
}

typedef struct { ROWCOPY Row; UCHAR Key[RFS_MAX_KEY], Rv[12 + 2 * RFS_MAX_NAME], Lk[8 + 24 + 2 * RFS_MAX_NAME]; } RN3SCR;

static RFS_STATUS RFS_NOINLINE
Rename3(RN3SCR *x, PRFS_VOL V, ULONGLONG FromDir, const USHORT *From, ULONG FromLen, ULONGLONG ToDir, const USHORT *To, ULONG ToLen, BOOLEAN Replace)
{
    PRFS_TABLE S = NULL, D = NULL;
    RFS_CURSOR c;
    PUCHAR key = x->Key, rv = x->Rv;
    UCHAR rk[24];
    ULONG kl;
    USHORT kf;
    RFS_STATUS st;
    BOOLEAN same = (BOOLEAN)(FromDir == ToDir);
    x->Row.Val = NULL;
    st = RfsTableOpenObject(V, FromDir, &S);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsTableOpenObject(V, ToDir, &D);
    if (!RFS_SUCCESS(st)) { RfsTableClose(V, S); return st; }
    st = FindName(S, From, FromLen, &c);
    if (!RFS_SUCCESS(st)) goto out;
    st = RowCopy(RfsCurRow(&c), &x->Row);
    RfsCursorClose(&c);
    if (!RFS_SUCCESS(st)) goto out;
    kf = RfsGet16(x->Row.Key + 2);
    /* the destination name */
    st = FindName(D, To, ToLen, &c);
    if (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        BOOLEAN itself = (BOOLEAN)(same && r->KeyLen == x->Row.KeyLen &&
                                   RfsNameCompare(V, (const USHORT *)(r->Key + 4), (r->KeyLen - 4) / 2, From, FromLen) == 0);
        if (itself) {
            RfsCursorClose(&c);
        } else if (!Replace) {
            RfsCursorClose(&c);
            st = RFS_EXISTS;
            goto out;
        } else {
            BOOLEAN dir = (BOOLEAN)(RfsGet16(r->Key + 2) != REFS_NAME_EMBEDDED && r->ValLen >= REFS_IE_ATTR + 4 &&
                                    (RfsGet32(r->Val + REFS_IE_ATTR) & REFS_ATTR_DIRECTORY));
            if (dir) { RfsCursorClose(&c); st = RFS_IS_A_DIRECTORY; goto out; }
            st = RemoveAt(V, D, ToDir, &c, TRUE);
            if (!RFS_SUCCESS(st)) goto out;
        }
    } else if (st != RFS_NOT_FOUND) {
        goto out;
    }
    /* the old name row goes (what it holds moves) */
    st = FindName(S, From, FromLen, &c);
    if (RFS_SUCCESS(st)) st = RfsTreeDeleteAt(&c);
    if (!RFS_SUCCESS(st)) goto out;
    NameKey(To, ToLen, kf, key, &kl);
    if (kf == REFS_NAME_EMBEDDED) {
        ULONGLONG id = x->Row.ValLen >= FH_FILEID + 8 ? RfsGet64(x->Row.Val + FH_FILEID) : 0, nid = id;
        PUCHAR nv = NULL;
        ULONG nl;
        if (id) st = DropReverse(S, id);
        if (RFS_SUCCESS(st) && !same) {
            /* a new id in the new directory; pages under the record get the new owner */
            st = NextFileId(V, ToDir, &nid);
            if (RFS_SUCCESS(st) && x->Row.ValLen >= FH_FILEID + 8) RfsPut64(x->Row.Val + FH_FILEID, nid);
            if (RFS_SUCCESS(st)) st = Rehome(V, ToDir, x->Row.Val, x->Row.ValLen, &nv, &nl, 0);
            if (RFS_SUCCESS(st) && nv) { RfsHostFree(x->Row.Val); x->Row.Val = nv; x->Row.ValLen = nl; }
        }
        if (RFS_SUCCESS(st) && x->Row.ValLen >= REFS_FH_CHTIME + 8) RfsPut64(x->Row.Val + REFS_FH_CHTIME, RfsHostNowNt());
        if (RFS_SUCCESS(st)) st = RfsTreeInsert(D, key, kl, x->Row.Val, x->Row.ValLen, x->Row.Flags, FALSE);
        if (RFS_SUCCESS(st) && nid) {
            RevKey(nid, rk);
            st = RfsTreeInsert(D, rk, 24, rv, RevVal(To, ToLen, rv), 0, FALSE);
        }
    } else {
        BOOLEAN dir = (BOOLEAN)(x->Row.ValLen >= REFS_IE_ATTR + 4 && (RfsGet32(x->Row.Val + REFS_IE_ATTR) & REFS_ATTR_DIRECTORY));
        st = RfsTreeInsert(D, key, kl, x->Row.Val, x->Row.ValLen, x->Row.Flags, FALSE);
        if (RFS_SUCCESS(st) && dir) {
            /* the directory's link: its parent and name */
            ULONGLONG oid = RfsGet64(x->Row.Val + REFS_IE_HOME);
            PRFS_TABLE N;
            RFS_CURSOR c2;
            UCHAR k4[4];
            st = RfsTableOpenObject(V, oid, &N);
            if (RFS_SUCCESS(st)) {
                RfsPut32(k4, REFS_KEY_DIRSELF);
                st = RfsLookup(N, k4, 4, &c2);
                if (RFS_SUCCESS(st)) {
                    PRFS_ROW r = RfsCurRow(&c2);
                    PRFS_TABLE R;
                    USHORT fl = r->Flags;
                    st = RfsTableOpenEmbedded(V, oid, r->Val, r->ValLen, &R);
                    if (RFS_SUCCESS(st)) {
                        RFS_CURSOR c3;
                        PUCHAR lk = x->Lk;
                        ULONG ll = 24 + 2 * ToLen, i;
                        PUCHAR val;
                        ULONG vl;
                        st = RfsSeek(R, NULL, 0, &c3);
                        while (RFS_SUCCESS(st)) {
                            PRFS_ROW a = RfsCurRow(&c3);
                            if (a->KeyLen >= 14 && RfsGet16(a->Key + 12) == REFS_AT_OBJLINK) { st = RfsTreeDeleteAt(&c3); break; }
                            st = RfsNext(&c3);
                        }
                        RfsCursorClose(&c3);
                        if (st == RFS_NO_MORE) st = RFS_OK;
                        RfsZero(lk, sizeof(x->Lk));
                        RfsPut64(lk, ll);
                        RfsPut32(lk + 8, REFS_AT_MARKER_MULTI);
                        RfsPut16(lk + 12, REFS_AT_OBJLINK);
                        RfsPut16(lk + 14, 0x0D);
                        RfsPut64(lk + 16, ToDir);
                        for (i = 0; i < ToLen; i++) RfsPut16(lk + 32 + 2 * i, To[i]);
                        if (RFS_SUCCESS(st)) st = RfsTreeInsert(R, lk, 8 + ll, lk + 8, ll, 0, FALSE);
                        if (RFS_SUCCESS(st) && R->Root->DescLen >= REFS_FH_CHTIME + 8) RfsPut64(R->Root->Desc + REFS_FH_CHTIME, RfsHostNowNt());
                        if (RFS_SUCCESS(st)) st = RfsEmbeddedStore(R, &val, &vl);
                        RfsTableClose(V, R);
                        if (RFS_SUCCESS(st)) { st = RfsTreeUpdateAt(&c2, val, vl, fl); RfsHostFree(val); }
                        else RfsCursorClose(&c2);
                    } else RfsCursorClose(&c2);
                } else RfsCursorClose(&c2);
                RfsTableClose(V, N);
            }
            if (RFS_SUCCESS(st) && !same) {
                st = ParentChild(V, FromDir, oid, FALSE);
                if (RFS_SUCCESS(st)) st = ParentChild(V, ToDir, oid, TRUE);
            }
        }
    }
    if (RFS_SUCCESS(st)) st = TouchDir(V, S, TRUE);
    if (RFS_SUCCESS(st) && !same) st = TouchDir(V, D, TRUE);
out:
    if (x->Row.Val) RfsHostFree(x->Row.Val);
    RfsTableClose(V, D);
    RfsTableClose(V, S);
    return st;
}

RFS_STATUS
RfsRename(PRFS_VOL V, ULONGLONG FromDir, const USHORT *From, ULONG FromLen, ULONGLONG ToDir, const USHORT *To, ULONG ToLen, BOOLEAN Replace)
{
    RN3SCR *x;
    RFS_STATUS st;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (RFS_V1REC(V)) return RfsRenameV1(V, FromDir, From, FromLen, ToDir, To, ToLen, Replace);
    x = (RN3SCR *)RfsHostAlloc(sizeof(*x));
    if (!x) return RFS_NO_MEMORY;
    st = Rename3(x, V, FromDir, From, FromLen, ToDir, To, ToLen, Replace);
    RfsHostFree(x);
    return st;
}

RFS_STATUS
RfsSetLabel(PRFS_VOL V, const char *Label)
{
    /* the label row (key 0x510) of the volume information tables 0x500 and
     * 0x501, kept alike: UTF-16 without a terminator; no label, no row */
    static const ULONGLONG oids[2] = { REFS_OID_VOLINFO, REFS_OID_VOLINFO2 };
    USHORT w[33];
    UCHAR key[8], val[64];
    ULONG n, len = 0, i, k, done = 0;
    RFS_STATUS st = RFS_OK;
    if (V->ReadOnly) return RFS_READ_ONLY;
    if (V->V1) return RFS_NOT_SUPPORTED;
    while (Label && Label[len]) len++;
    n = len ? RfsUtf8ToUtf16(Label, len, w, 33) : 0;
    if (n == (ULONG)-1 || n > 32) return RFS_BAD_NAME;
    for (i = 0; i < n; i++) RfsPut16(val + 2 * i, w[i]);
    RfsZero(key, sizeof(key));
    RfsPut16(key, 0x510);
    for (k = 0; k < 2; k++) {
        PRFS_TABLE T;
        if (!RFS_SUCCESS(RfsTableOpenObject(V, oids[k], &T))) continue;
        if (n == 0) {
            st = RfsTreeDelete(T, key, 8);
            if (st == RFS_NOT_FOUND) st = RFS_OK;
        } else {
            st = RfsTreeUpdate(T, key, 8, val, 2 * n, 0);
            if (st == RFS_NOT_FOUND) st = RfsTreeInsert(T, key, 8, val, 2 * n, 0, FALSE);
        }
        RfsTableClose(V, T);
        if (!RFS_SUCCESS(st)) return st;
        done++;
    }
    if (!done) return RFS_CORRUPT;
    V->Label[RfsUtf16ToUtf8(w, n, V->Label, sizeof(V->Label) - 1)] = 0;
    return RFS_OK;
}

/*
 * REFSNT - directories and files (reading).
 *
 * A directory is an object: its table (schema 0x130) holds
 *   0x10   its own row: an embedded table whose header data are the
 *          directory's times and attributes
 *   0x20   FileId -> name rows (the reverse index)
 *   0x30   name rows: key {u16 0x30, u16 flags, UTF-16 name}
 *            flags 1: the value is the file's own table (embedded record)
 *            flags 2: an index entry {FileId, home OID, times, sizes,
 *                     attributes}; a directory (attribute 0x10000000, the
 *                     home OID is then the directory's object) or a file
 *                     whose record is the 0x40 row {FileId, home} of its
 *                     home directory
 *   0x40   records of files split out of their name rows
 * A file's table (schema 0x1E0) has the file's times, attributes, security
 * id and sizes as header data, and attribute rows: $DATA (0x80) either
 * inline (single-instance marker) or as a stream table of extents
 * (multi-instance, sub-stream 0x1000), named streams (0xB0), reparse
 * data (0xC0), ...
 */
#include "rfseng.h"

static VOID
TimesFromHeader(const UCHAR *Val, ULONG Len, RFS_TIMES *T, PULONG Attr)
{
    if (Len < REFS_FH_ATTR + 4) return;
    T->Create = RfsGet64(Val + REFS_FH_CTIME);
    T->Write = RfsGet64(Val + REFS_FH_MTIME);
    T->Change = RfsGet64(Val + REFS_FH_CHTIME);
    T->Access = RfsGet64(Val + REFS_FH_ATIME);
    *Attr = RfsGet32(Val + REFS_FH_ATTR);
}

static BOOLEAN
EntFromRow(PRFS_VOL V, ULONGLONG Dir, const RFS_ROW *R, PRFS_DIRENT E)
{
    ULONG n, i;
    (VOID)V;
    if (R->KeyLen < 4 || RfsGet16(R->Key) != REFS_KEY_NAME) return FALSE;
    RfsZero(E, sizeof(*E));
    n = (R->KeyLen - 4) / 2;
    if (n > RFS_MAX_NAME) n = RFS_MAX_NAME;
    for (i = 0; i < n; i++) E->Name[i] = RfsGet16(R->Key + 4 + 2 * i);
    E->NameLen = n;
    E->KeyFlags = (UCHAR)RfsGet16(R->Key + 2);
    if (RFS_V1REC(V) && E->KeyFlags == REFS1_ENT_DIR) {
        /* 1.x subdirectory entry: 72-byte value, child OID at +0 (2.x: at +8, as 3.x) */
        if (R->ValLen < 0x44) return FALSE;
        E->Type = RFS_FT_DIR;
        E->Oid = RfsGet64(R->Val + 0x00);
        if (V->V2 && RfsGet64(R->Val + REFS_IE_HOME)) E->Oid = RfsGet64(R->Val + REFS_IE_HOME);
        E->Home = Dir;
        E->Times.Create = RfsGet64(R->Val + 0x10);
        E->Times.Write = RfsGet64(R->Val + 0x18);
        E->Times.Change = RfsGet64(R->Val + 0x20);
        E->Times.Access = RfsGet64(R->Val + 0x28);
        E->Attributes = (RfsGet32(R->Val + 0x40) & ~REFS_ATTR_DIRECTORY) | 0x10;
        return TRUE;
    }
    if (V->V1 && E->KeyFlags == REFS1_ENT_FILE) {
        E->Type = RFS_FT_FILE;
        E->Home = Dir;
        if (R->ValLen >= REFS1_FH_NEXTID) E->FileId = RfsGet64(R->Val + 0x58);
        TimesFromHeader(R->Val, R->ValLen, &E->Times, &E->Attributes);
        if (R->ValLen >= REFS1_FH_ALLOC + 8) {
            E->Size = RfsGet64(R->Val + REFS1_FH_SIZE);
            E->Alloc = RfsGet64(R->Val + REFS1_FH_ALLOC);
        }
        return TRUE;
    }
    if (E->KeyFlags == REFS_NAME_EMBEDDED) {
        E->Type = RFS_FT_FILE;
        E->Home = Dir;
        if (R->ValLen >= 0x88) E->FileId = RfsGet64(R->Val + 0x80);
        TimesFromHeader(R->Val, R->ValLen, &E->Times, &E->Attributes);
        if (R->ValLen >= REFS_FH_ALLOC + 8) {
            E->Size = RfsGet64(R->Val + REFS_FH_SIZE);
            E->Alloc = RfsGet64(R->Val + REFS_FH_ALLOC);
        }
        return TRUE;
    }
    if (R->ValLen < 0x44) return FALSE;
    E->FileId = RfsGet64(R->Val + REFS_IE_FILEID);
    E->Home = RfsGet64(R->Val + REFS_IE_HOME);
    E->Times.Create = RfsGet64(R->Val + REFS_IE_CTIME);
    E->Times.Write = RfsGet64(R->Val + REFS_IE_MTIME);
    E->Times.Change = RfsGet64(R->Val + REFS_IE_CHTIME);
    E->Times.Access = RfsGet64(R->Val + REFS_IE_ATIME);
    E->Alloc = RfsGet64(R->Val + REFS_IE_ALLOC);
    E->Size = RfsGet64(R->Val + REFS_IE_SIZE);
    E->Attributes = RfsGet32(R->Val + REFS_IE_ATTR);
    if (E->Attributes & REFS_ATTR_DIRECTORY) {
        E->Type = RFS_FT_DIR;
        E->Oid = E->Home;
        E->Home = Dir;
        E->Attributes &= ~REFS_ATTR_DIRECTORY;
        E->Attributes |= 0x10;
    } else {
        E->Type = RFS_FT_FILE;
    }
    return TRUE;
}

/* enumeration: First positions at the first name row */
RFS_STATUS
RfsDirNext(PRFS_VOL V, ULONGLONG Dir, PRFS_CURSOR C, BOOLEAN First, PRFS_DIRENT E)
{
    RFS_STATUS st;
    if (First) {
        PRFS_TABLE T;
        UCHAR key[4];
        st = RfsTableOpenObject(V, Dir, &T);
        if (!RFS_SUCCESS(st)) return st;
        RfsPut16(key, REFS_KEY_NAME);
        RfsPut16(key + 2, 0);
        st = RfsSeek(T, key, 4, C);
        RfsTableClose(V, T);                    /* the cursor's nodes stay with the cached table */
    } else {
        st = RfsNext(C);
    }
    while (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(C);
        if (r->KeyLen < 2 || RfsGet16(r->Key) != REFS_KEY_NAME) { st = RFS_NO_MORE; break; }
        if (EntFromRow(V, Dir, r, E)) return RFS_OK;
        st = RfsNext(C);
    }
    RfsCursorClose(C);                          /* the end (or an error) lets the table go */
    return st;
}

static RFS_STATUS NameKey(const USHORT *Name, ULONG NameLen, PUCHAR Key, PULONG KeyLen);

/* the first entry after Name (Name NULL: the first entry) */
RFS_STATUS
RfsDirAfter(PRFS_VOL V, ULONGLONG Dir, const USHORT *Name, ULONG NameLen, PRFS_DIRENT E)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    PUCHAR key;
    ULONG kl = 4;
    RFS_STATUS st;
    key = (PUCHAR)RfsHostAlloc(4 + 2 * RFS_MAX_NAME);      /* (off the stack) */
    if (!key) return RFS_NO_MEMORY;
    if (Name) {
        st = NameKey(Name, NameLen, key, &kl);
        if (!RFS_SUCCESS(st)) { RfsHostFree(key); return st; }
    } else {
        RfsPut16(key, REFS_KEY_NAME);
        RfsPut16(key + 2, 0);
    }
    st = RfsTableOpenObject(V, Dir, &T);
    if (!RFS_SUCCESS(st)) { RfsHostFree(key); return st; }
    st = RfsSeek(T, key, kl, &c);
    RfsHostFree(key);
    RfsTableClose(V, T);
    while (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        if (r->KeyLen < 2 || RfsGet16(r->Key) != REFS_KEY_NAME) { st = RFS_NO_MORE; break; }
        if (Name && r->KeyLen >= 4 &&
            RfsNameCompare(V, (const USHORT *)(r->Key + 4), (r->KeyLen - 4) / 2, Name, NameLen) <= 0) {
            st = RfsNext(&c);
            continue;
        }
        if (EntFromRow(V, Dir, r, E)) break;
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    return st;
}

static RFS_STATUS
NameKey(const USHORT *Name, ULONG NameLen, PUCHAR Key, PULONG KeyLen)
{
    ULONG i;
    if (NameLen == 0 || NameLen > RFS_MAX_NAME) return RFS_BAD_NAME;
    RfsPut16(Key, REFS_KEY_NAME);
    RfsPut16(Key + 2, 0);
    for (i = 0; i < NameLen; i++) RfsPut16(Key + 4 + 2 * i, Name[i]);
    *KeyLen = 4 + 2 * NameLen;
    return RFS_OK;
}

RFS_STATUS
RfsDirLookup(PRFS_VOL V, ULONGLONG Dir, const USHORT *Name, ULONG NameLen, PRFS_DIRENT E)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    PUCHAR key;
    ULONG kl;
    RFS_STATUS st;
    key = (PUCHAR)RfsHostAlloc(4 + 2 * RFS_MAX_NAME);      /* (off the stack) */
    if (!key) return RFS_NO_MEMORY;
    st = NameKey(Name, NameLen, key, &kl);
    if (RFS_SUCCESS(st)) st = RfsTableOpenObject(V, Dir, &T);
    if (!RFS_SUCCESS(st)) { RfsHostFree(key); return st; }
    st = RfsSeek(T, key, kl, &c);
    RfsHostFree(key);
    if (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        if (r->KeyLen >= 4 && RfsGet16(r->Key) == REFS_KEY_NAME &&
            EntFromRow(V, Dir, r, E) && RfsNameCompare(V, E->Name, E->NameLen, Name, NameLen) == 0) {
            st = RFS_OK;
        } else {
            st = RFS_NOT_FOUND;
        }
    } else if (st == RFS_NO_MORE) {
        st = RFS_NOT_FOUND;
    }
    RfsCursorClose(&c);
    RfsTableClose(V, T);
    return st;
}

/* ---- a file's own table ------------------------------------------------------------------------- */
static RFS_STATUS
AddExtent(PRFS_FILE F, ULONGLONG Vcn, ULONGLONG Lcn, ULONG Run, USHORT Flags)
{
    if (F->NExt == F->CapExt) {
        ULONG cap = F->CapExt ? F->CapExt * 2 : 16;
        PRFS_EXTENT n = (PRFS_EXTENT)RfsHostAlloc(cap * sizeof(RFS_EXTENT));
        if (!n) return RFS_NO_MEMORY;
        if (F->Ext) { RfsCopy(n, F->Ext, F->NExt * sizeof(RFS_EXTENT)); RfsHostFree(F->Ext); }
        F->Ext = n;
        F->CapExt = cap;
    }
    F->Ext[F->NExt].Vcn = Vcn;
    F->Ext[F->NExt].Lcn = Lcn;
    F->Ext[F->NExt].Run = Run;
    F->Ext[F->NExt].Flags = Flags;
    F->NExt++;
    return RFS_OK;
}

/* the extents of a stream table (embedded in the $DATA row) */
static RFS_STATUS
ReadStream(PRFS_VOL V, ULONGLONG Owner, const UCHAR *Val, ULONG Len, PRFS_FILE F)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    RFS_STATUS st;
    ULONG i, j;
    st = RfsTableOpenEmbedded(V, Owner, Val, Len, &T);
    if (!RFS_SUCCESS(st)) return st;
    if (T->Root->DescLen >= 0x58) {
        const UCHAR *h = T->Root->Desc + REFS_IR_DATA;
        ULONG o = RfsGet32(h + 4) == 0x28 ? 0 : 4;      /* 3.4 keeps the sizes 4 bytes further */
        F->Alloc = RfsGet64(h + 0x08 + o);
        F->Size = RfsGet64(h + 0x10 + o);
        F->ValidSize = RfsGet64(h + 0x18 + o);
    }
    st = RfsSeek(T, NULL, 0, &c);
    while (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        if (V->V1 || (V->V2 && !RfsNodeBare(T, c.Node[c.Depth]))) {
            if (r->ValLen >= REFS1_EXT_FLAGS) {
                ULONGLONG vcn = RfsGet64(r->Val + REFS1_EXT_VCN);
                ULONGLONG cnt = RfsGet64(r->Val + REFS1_EXT_COUNT);
                ULONGLONG lcn = RfsGet64(r->Val + REFS1_EXT_LCN);
                USHORT fl = (r->ValLen >= REFS1_EXT_CKLEN) ? RfsGet16(r->Val + REFS1_EXT_FLAGS) : 0;
                if (fl & 1) F->Integrity = TRUE;      /* per-block checksums follow */
                st = AddExtent(F, vcn, lcn, (ULONG)cnt, fl & 1 ? REFS_EXTF_CHECKSUMS : 0);
                if (!RFS_SUCCESS(st)) break;
            }
        } else if (r->ValLen >= REFS_EXT_LENGTH) {
            ULONGLONG lcn = RfsGet64(r->Val + REFS_EXT_VLCN);
            USHORT fl = RfsGet16(r->Val + REFS_EXT_FLAGS);
            ULONG vcn = RfsGet32(r->Val + REFS_EXT_VCN), run = RfsGet32(r->Val + REFS_EXT_RUN);
            if (fl & REFS_EXTF_SPARSE) lcn = 0;
            if (fl & REFS_EXTF_CHECKSUMS) F->Integrity = TRUE;
            st = AddExtent(F, vcn, lcn, run, fl);
            if (!RFS_SUCCESS(st)) break;
        }
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    RfsTableClose(V, T);
    if (st != RFS_NO_MORE) return st;
    /* by file cluster */
    for (i = 1; i < F->NExt; i++) {
        RFS_EXTENT e = F->Ext[i];
        for (j = i; j > 0 && F->Ext[j - 1].Vcn > e.Vcn; j--) F->Ext[j] = F->Ext[j - 1];
        F->Ext[j] = e;
    }
    return RFS_OK;
}

/* the attributes of a file's table: header data and $DATA */
static RFS_STATUS
ReadFileTable(PRFS_VOL V, ULONGLONG Owner, const UCHAR *Val, ULONG Len, PRFS_FILE F)
{
    PRFS_TABLE T;
    RFS_CURSOR c;
    RFS_STATUS st;
    BOOLEAN gotData = FALSE;
    TimesFromHeader(Val, Len, &F->Times, &F->Attributes);
    if (RFS_V1REC(V)) {
        if (V->V2) {
            if (Len >= REFS_FH_NEXTID + 8 && !F->FileId) F->FileId = RfsGet64(Val + REFS_FH_NEXTID);
            if (Len >= REFS_FH_ALLOC + 8) {
                F->SecurityId = RfsGet32(Val + REFS_FH_SECID);
                F->Size = F->ValidSize = RfsGet64(Val + REFS_FH_SIZE);
                F->Alloc = RfsGet64(Val + REFS_FH_ALLOC);
            }
        } else if (Len >= REFS1_FH_ALLOC + 8) {
            if (!F->FileId) F->FileId = RfsGet64(Val + 0x58);
            F->SecurityId = RfsGet32(Val + REFS1_FH_SECHASH);    /* hash; the security table is keyed by it */
            F->Size = F->ValidSize = RfsGet64(Val + REFS1_FH_SIZE);
            F->Alloc = RfsGet64(Val + REFS1_FH_ALLOC);
        }
        st = RfsTableOpenEmbedded(V, Owner, Val, Len, &T);
        if (!RFS_SUCCESS(st)) return st;
        st = RfsSeek(T, NULL, 0, &c);
        while (RFS_SUCCESS(st)) {
            PRFS_ROW r = RfsCurRow(&c);
            /* attribute key: {u64 value-size, u32 type, UTF-16 name} */
            if (r->KeyLen >= 12) {
                ULONG type = RfsGet32(r->Key + 8);
                if (type == REFS_AT_DATA && r->KeyLen <= 14 && !gotData && (r->Flags & REFS1_RF_EMBEDDED)) {
                    st = ReadStream(V, Owner, r->Val, r->ValLen, F);
                    if (!RFS_SUCCESS(st)) break;
                    gotData = TRUE;
                } else if (type == REFS_AT_REPARSE) {
                    F->Reparse = TRUE;
                }
            }
            st = RfsNext(&c);
        }
        RfsCursorClose(&c);
        RfsTableClose(V, T);
        if (st != RFS_NO_MORE) return st;
        if (!gotData) { F->Size = F->ValidSize = 0; }
        return RFS_OK;
    }
    if (Len >= 0x98 + 8) {
        if (!F->FileId) F->FileId = RfsGet64(Val + 0x80);
        F->LinkCount = (ULONG)RfsGet64(Val + 0x98);
    }
    if (Len >= REFS_FH_ALLOC + 8) {
        F->IFlags = RfsGet32(Val + REFS_FH_IFLAGS);
        F->SecurityId = RfsGet64(Val + REFS_FH_SECID);
        F->Size = F->ValidSize = RfsGet64(Val + REFS_FH_SIZE);
        F->Alloc = RfsGet64(Val + REFS_FH_ALLOC);
    }
    st = RfsTableOpenEmbedded(V, Owner, Val, Len, &T);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsSeek(T, NULL, 0, &c);
    while (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        if (r->KeyLen >= 16) {
            ULONG marker = RfsGet32(r->Key + 8);
            USHORT type = RfsGet16(r->Key + 12);
            if (type == REFS_AT_DATA && marker == REFS_AT_MARKER_SINGLE && !gotData && r->ValLen >= 0x3C) {
                /* inline content: {0, area size, 0x0C, summary size, summary, data} */
                ULONG co = RfsGet32(r->Val + 8), ss = RfsGet32(r->Val + 12), at = co + ss;
                ULONGLONG size = RfsGet64(r->Val + 0x20);
                if (co >= 8 && at <= r->ValLen && size <= r->ValLen - at) {
                    F->Inline = TRUE;
                    F->InlineLen = (ULONG)size;
                    F->InlineData = (PUCHAR)RfsHostAlloc(F->InlineLen ? F->InlineLen : 1);
                    if (!F->InlineData) { st = RFS_NO_MEMORY; break; }
                    RfsCopy(F->InlineData, r->Val + at, F->InlineLen);
                    F->Size = F->ValidSize = size;
                    F->Alloc = RfsGet64(r->Val + 0x18);
                    gotData = TRUE;
                }
            } else if (type == REFS_AT_DATA && marker == REFS_AT_MARKER_MULTI && r->KeyLen >= 24 &&
                       RfsGet64(r->Key + 16) == REFS_STREAM_LIVE && !gotData && (r->Flags & REFS_RF_EMBEDDED)) {
                st = ReadStream(V, Owner, r->Val, r->ValLen, F);
                if (!RFS_SUCCESS(st)) break;
                gotData = TRUE;
            } else if (type == REFS_AT_REPARSE) {
                F->Reparse = TRUE;
            }
        }
        st = RfsNext(&c);
    }
    RfsCursorClose(&c);
    RfsTableClose(V, T);
    if (st != RFS_NO_MORE) return st;
    if (!gotData) { F->Size = F->ValidSize = 0; }
    return RFS_OK;
}

RFS_STATUS
RfsFileOpen(PRFS_VOL V, ULONGLONG Dir, const RFS_DIRENT *E, PRFS_FILE *Out)
{
    PRFS_FILE F;
    PRFS_TABLE T;
    RFS_CURSOR c;
    PUCHAR key;
    ULONG kl;
    RFS_STATUS st;
    if (E->Type == RFS_FT_DIR) {
        st = RfsFileOpenDir(V, E->Oid, Out);
        if (RFS_SUCCESS(st)) {
            (*Out)->Parent = Dir;
            RfsCopy((*Out)->Name, E->Name, E->NameLen * sizeof(USHORT));
            (*Out)->NameLen = E->NameLen;
            (*Out)->KeyFlags = E->KeyFlags;
        }
        return st;
    }
    *Out = NULL;
    F = (PRFS_FILE)RfsAllocZero(sizeof(RFS_FILE));
    if (!F) return RFS_NO_MEMORY;
    F->Type = RFS_FT_FILE;
    F->Parent = Dir;
    RfsCopy(F->Name, E->Name, E->NameLen * sizeof(USHORT));
    F->NameLen = E->NameLen;
    F->KeyFlags = E->KeyFlags;
    key = (PUCHAR)RfsHostAlloc(4 + 2 * RFS_MAX_NAME);      /* (off the stack) */
    if (!key) { RfsFileClose(F); return RFS_NO_MEMORY; }
    if (E->KeyFlags == REFS_NAME_EMBEDDED) {
        F->Home = Dir;
        st = NameKey(E->Name, E->NameLen, key, &kl);
        if (RFS_SUCCESS(st)) st = RfsTableOpenObject(V, Dir, &T);
        if (RFS_SUCCESS(st)) {
            st = RfsLookup(T, key, kl, &c);
            if (RFS_SUCCESS(st)) {
                PRFS_ROW r = RfsCurRow(&c);
                st = ReadFileTable(V, Dir, r->Val, r->ValLen, F);
            }
            RfsCursorClose(&c);
            RfsTableClose(V, T);
        }
    } else {
        /* the record in the home directory: key {0x80000040, 0, FileId, home} */
        F->Home = E->Home;
        F->FileId = E->FileId;
        RfsZero(key, 24);
        RfsPut32(key, 0x80000000UL | REFS_KEY_BACKING);
        RfsPut64(key + 8, E->FileId);
        RfsPut64(key + 16, E->Home);
        st = RfsTableOpenObject(V, E->Home, &T);
        if (RFS_SUCCESS(st)) {
            st = RfsLookup(T, key, 24, &c);
            if (RFS_SUCCESS(st)) {
                PRFS_ROW r = RfsCurRow(&c);
                st = ReadFileTable(V, E->Home, r->Val, r->ValLen, F);
            }
            RfsCursorClose(&c);
            RfsTableClose(V, T);
        }
    }
    RfsHostFree(key);
    if (!RFS_SUCCESS(st)) { RfsFileClose(F); return st; }
    *Out = F;
    return RFS_OK;
}

RFS_STATUS
RfsFileOpenDir(PRFS_VOL V, ULONGLONG Oid, PRFS_FILE *Out)
{
    PRFS_FILE F;
    PRFS_TABLE T;
    RFS_CURSOR c;
    UCHAR key[4];
    RFS_STATUS st;
    *Out = NULL;
    F = (PRFS_FILE)RfsAllocZero(sizeof(RFS_FILE));
    if (!F) return RFS_NO_MEMORY;
    F->Type = RFS_FT_DIR;
    F->Oid = Oid;
    st = RfsTableOpenObject(V, Oid, &T);
    if (!RFS_SUCCESS(st)) { RfsHostFree(F); return st; }
    RfsPut32(key, REFS_KEY_DIRSELF);
    st = RfsLookup(T, key, 4, &c);
    if (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        TimesFromHeader(r->Val, r->ValLen, &F->Times, &F->Attributes);
        if (r->ValLen >= REFS_FH_SECID + 8) {
            F->IFlags = RfsGet32(r->Val + REFS_FH_IFLAGS);
            F->SecurityId = RfsGet64(r->Val + REFS_FH_SECID);
        }
        F->Attributes |= 0x10;
    } else if (st == RFS_NOT_FOUND) {
        st = RFS_OK;
        F->Attributes = 0x10;
    }
    RfsCursorClose(&c);
    RfsTableClose(V, T);
    if (!RFS_SUCCESS(st)) { RfsHostFree(F); return st; }
    *Out = F;
    return RFS_OK;
}

VOID
RfsFileClose(PRFS_FILE F)
{
    if (!F) return;
    if (F->InlineData) RfsHostFree(F->InlineData);
    if (F->Ext) RfsHostFree(F->Ext);
    RfsHostFree(F);
}

/* ---- data --------------------------------------------------------------------------------------------- */
RFS_STATUS
RfsFileMap(PRFS_VOL V, PRFS_FILE F, ULONGLONG Vcn, PULONGLONG Plcn, PULONG Run)
{
    ULONG lo = 0, hi = F->NExt;
    *Plcn = 0;
    *Run = 1;
    while (lo < hi) {
        ULONG mid = (lo + hi) / 2;
        if (F->Ext[mid].Vcn + F->Ext[mid].Run <= Vcn) lo = mid + 1; else hi = mid;
    }
    if (lo < F->NExt && F->Ext[lo].Vcn <= Vcn) {
        PRFS_EXTENT e = &F->Ext[lo];
        ULONG within = (ULONG)(Vcn - e->Vcn), crun;
        *Run = e->Run - within;
        if (e->Lcn == 0 || (e->Flags & REFS_EXTF_SPARSE)) return RFS_OK;
        if (V->V1) { *Plcn = e->Lcn + within; return RFS_OK; }
        {
            RFS_STATUS st = RfsTranslate(V, e->Lcn + within, Plcn, &crun);
            if (!RFS_SUCCESS(st)) return st;
            if (crun < *Run) *Run = crun;
        }
        return RFS_OK;
    }
    /* a hole up to the next extent */
    if (lo < F->NExt) *Run = (ULONG)(F->Ext[lo].Vcn - Vcn);
    else *Run = 0x7FFFFFFF;
    return RFS_OK;
}

RFS_STATUS
RfsFileRead(PRFS_VOL V, PRFS_FILE F, ULONGLONG Off, ULONG Len, PVOID Buf, PULONG Got)
{
    PUCHAR out = (PUCHAR)Buf;
    ULONG done = 0;
    ULONG unit = V->V1 ? REFS1_PAGE : V->ClusterSize;
    ULONG shift = V->V1 ? REFS1_BLOCK_SHIFT : V->ClusterShift;
    *Got = 0;
    if (Off >= F->Size) return RFS_OK;
    if (Len > F->Size - Off) Len = (ULONG)(F->Size - Off);
    if (F->Inline) {
        ULONG n = Len;
        if (Off >= F->InlineLen) n = 0;
        else if (n > F->InlineLen - Off) n = (ULONG)(F->InlineLen - Off);
        RfsCopy(out, F->InlineData + Off, n);
        RfsZero(out + n, Len - n);
        *Got = Len;
        return RFS_OK;
    }
    while (done < Len) {
        ULONGLONG pos = Off + done, vcn = pos >> shift, plcn;
        ULONG within = (ULONG)(pos & (unit - 1)), run, n;
        RFS_STATUS st = RfsFileMap(V, F, vcn, &plcn, &run);
        if (!RFS_SUCCESS(st)) return st;
        n = Len - done;
        if ((ULONGLONG)run * unit - within < n) n = (ULONG)((ULONGLONG)run * unit - within);
        if (plcn == 0 || pos >= F->ValidSize) {
            RfsZero(out + done, n);
        } else {
            st = RfsHostRead(V, (plcn << shift) + within, n, out + done);
            if (!RFS_SUCCESS(st)) return st;
            if (pos + n > F->ValidSize) RfsZero(out + done + (ULONG)(F->ValidSize - pos), (ULONG)(pos + n - F->ValidSize));
        }
        done += n;
    }
    *Got = done;
    return RFS_OK;
}

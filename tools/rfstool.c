/*
 * REFSNT - rfstool: the ReFS engine on Linux (or Windows), for tests.
 *
 *   rfstool IMAGE info
 *   rfstool IMAGE ls [PATH]          rfstool IMAGE lsr [PATH]
 *   rfstool IMAGE cat PATH           rfstool IMAGE get PATH LOCAL
 *   rfstool IMAGE files              (every file: path, size, md5-less checksum)
 *   rfstool IMAGE order              (every table walked: key order, page checksums)
 */
#include "rfseng.h"
#include "rfsimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/time.h>
#include <dirent.h>
#include <sys/stat.h>
#endif

static RFS_IMAGE Img;
static RFS_VOL Vol;

RFS_STATUS RfsHostRead(PRFS_VOL V, ULONGLONG Off, ULONG Len, PVOID Buf) { (void)V; return RimgRead(&Img, Off, Buf, Len) ? RFS_OK : RFS_IO_ERROR; }
RFS_STATUS RfsHostWrite(PRFS_VOL V, ULONGLONG Off, ULONG Len, const VOID *Buf) { (void)V; if (RimgWrite(&Img, Off, Buf, Len)) return RFS_OK; fprintf(stderr, "write failed at %llx len %lx (size %llx)\n", (unsigned long long)Off, (unsigned long)Len, (unsigned long long)Img.Size); return RFS_IO_ERROR; }
RFS_STATUS RfsHostFlush(PRFS_VOL V) { (void)V; return RFS_OK; }
PVOID RfsHostAlloc(ULONG Bytes) { return malloc(Bytes ? Bytes : 1); }
VOID RfsHostFree(PVOID P) { free(P); }
ULONGLONG
RfsHostNowNt(VOID)
{
#ifdef _WIN32
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return 116444736000000000ULL + (ULONGLONG)tv.tv_sec * 10000000ULL + (ULONGLONG)tv.tv_usec * 10;
#endif
}

static void
Die(const char *What, RFS_STATUS St)
{
    fprintf(stderr, "%s: status %08lX\n", What, (unsigned long)(ULONG)St);
    exit(1);
}

static void
Name8(const USHORT *N, ULONG L, char *Out, ULONG Max)
{
    RfsUtf16ToUtf8(N, L, Out, Max);
}

/* walks a path ("/a/b") to its directory entry; the root is a directory with Oid 0x600 */
static RFS_STATUS
Walk(const char *Path, PRFS_DIRENT Ent, PULONGLONG ParentOut)
{
    ULONGLONG dir = REFS_OID_ROOT, parent = REFS_OID_ROOT;
    const char *p = Path;
    memset(Ent, 0, sizeof(*Ent));
    Ent->Type = RFS_FT_DIR;
    Ent->Oid = REFS_OID_ROOT;
    while (*p) {
        const char *q;
        USHORT name[RFS_MAX_NAME + 1];
        ULONG n;
        RFS_STATUS st;
        while (*p == '/') p++;
        if (!*p) break;
        q = p;
        while (*q && *q != '/') q++;
        if (Ent->Type != RFS_FT_DIR) return RFS_NOT_A_DIRECTORY;
        n = RfsUtf8ToUtf16(p, (ULONG)(q - p), name, RFS_MAX_NAME);
        if (n == (ULONG)-1) return RFS_BAD_NAME;
        st = RfsDirLookup(&Vol, dir, name, n, Ent);
        if (!RFS_SUCCESS(st)) return st;
        parent = dir;
        if (Ent->Type == RFS_FT_DIR) dir = Ent->Oid;
        p = q;
    }
    if (ParentOut) *ParentOut = parent;
    return RFS_OK;
}

static void
Info(void)
{
    printf("ReFS %u.%u, cluster %lu, page %lu, %llu clusters, reference %lu bytes, checksum type %u (self %u)\n",
           Vol.Major, Vol.Minor, (unsigned long)Vol.ClusterSize, (unsigned long)Vol.PageSize,
           (unsigned long long)Vol.TotalClusters, (unsigned long)Vol.RefSize, Vol.CkType, Vol.SelfCkType);
    printf("label '%s', serial %016llX, checkpoint clock %llu (slot %lu), flags %lX, %lu containers of %lu clusters\n",
           Vol.Label, (unsigned long long)Vol.Serial, (unsigned long long)Vol.Clock, (unsigned long)Vol.ChkpSlot,
           (unsigned long)Vol.ChkpFlags, (unsigned long)Vol.NConts, (unsigned long)Vol.ClustersPerContainer);
}

static void
Ls(ULONGLONG Dir, const char *Prefix, int Recursive)
{
    RFS_CURSOR c;
    RFS_DIRENT e;
    RFS_STATUS st = RfsDirNext(&Vol, Dir, &c, TRUE, &e);
    while (RFS_SUCCESS(st)) {
        char nm[1024];
        Name8(e.Name, e.NameLen, nm, sizeof(nm));
        if (Recursive) printf("%s %12llu %s%s\n", e.Type == RFS_FT_DIR ? "d" : "-", (unsigned long long)e.Size, Prefix, nm);
        else printf("%s %08lX %12llu %c %s\n", e.Type == RFS_FT_DIR ? "d" : "-", (unsigned long)e.Attributes,
                    (unsigned long long)e.Size, e.KeyFlags == REFS_NAME_EMBEDDED ? 'e' : 'i', nm);
        if (Recursive && e.Type == RFS_FT_DIR) {
            char sub[4096];
            RFS_CURSOR save = c;
            snprintf(sub, sizeof(sub), "%s%s/", Prefix, nm);
            Ls(e.Oid, sub, 1);
            c = save;
        }
        st = RfsDirNext(&Vol, Dir, &c, FALSE, &e);
    }
    if (st != RFS_NO_MORE) fprintf(stderr, "listing %s: %08lX\n", Prefix, (unsigned long)(ULONG)st);
}

/* a checksum of a file's content (FNV-1a 64), for comparing with other readers */
static RFS_STATUS
Digest(PRFS_FILE F, unsigned long long *Out, FILE *To)
{
    static UCHAR buf[1 << 20];
    unsigned long long h = 1469598103934665603ULL, off = 0;
    while (off < F->Size) {
        ULONG got, i;
        ULONG n = F->Size - off > sizeof(buf) ? sizeof(buf) : (ULONG)(F->Size - off);
        RFS_STATUS st = RfsFileRead(&Vol, F, off, n, buf, &got);
        if (!RFS_SUCCESS(st)) return st;
        if (To) fwrite(buf, 1, got, To);
        for (i = 0; i < got; i++) { h ^= buf[i]; h *= 1099511628211ULL; }
        off += got;
        if (!got) break;
    }
    *Out = h;
    return RFS_OK;
}

static unsigned long Files, Dirs, Errors;

static void
AllFiles(ULONGLONG Dir, const char *Prefix)
{
    RFS_CURSOR c;
    RFS_DIRENT e;
    RFS_STATUS st = RfsDirNext(&Vol, Dir, &c, TRUE, &e);
    while (RFS_SUCCESS(st)) {
        char nm[1024], path[4096];
        Name8(e.Name, e.NameLen, nm, sizeof(nm));
        snprintf(path, sizeof(path), "%s%s", Prefix, nm);
        if (e.Type == RFS_FT_DIR) {
            RFS_CURSOR save = c;
            char sub[4100];
            Dirs++;
            printf("D %s\n", path);
            snprintf(sub, sizeof(sub), "%s/", path);
            AllFiles(e.Oid, sub);
            c = save;
        } else {
            PRFS_FILE f;
            unsigned long long h = 0;
            RFS_STATUS s2 = RfsFileOpen(&Vol, Dir, &e, &f);
            Files++;
            if (RFS_SUCCESS(s2)) { s2 = Digest(f, &h, NULL); }
            if (!RFS_SUCCESS(s2)) { Errors++; printf("F %s ERROR %08lX\n", path, (unsigned long)(ULONG)s2); }
            else printf("F %s %llu %016llx%s\n", path, (unsigned long long)f->Size, h, f->Reparse ? " reparse" : "");
            if (RFS_SUCCESS(s2) || f) RfsFileClose(f);
        }
        st = RfsDirNext(&Vol, Dir, &c, FALSE, &e);
    }
    if (st != RFS_NO_MORE) { Errors++; fprintf(stderr, "listing %s: %08lX\n", Prefix, (unsigned long)(ULONG)st); }
}

/* ---- walking every table: key order ------------------------------------------------------------ */
static unsigned long Pages, Rows, OrderErrors;

static void
CheckNode(PRFS_TABLE T, PRFS_NODE N, const char *What, const UCHAR *Upper, ULONG UpperLen, int Depth)
{
    ULONG i;
    if (Depth > 8) return;
    Pages++;
    for (i = 0; i < N->NRows; i++) {
        PRFS_ROW r = &N->Rows[i];
        UCHAR k[16], pk[16];
        const UCHAR *key = r->Key, *pkey = NULL;
        ULONG kl = r->KeyLen, pkl = 0;
        Rows++;
        if (RfsNodeBare(T, N)) {
            RfsRecordKey(T->Rule, r, k); key = k; kl = 16;
        }
        if (i > 0) {
            PRFS_ROW p = &N->Rows[i - 1];
            pkey = p->Key; pkl = p->KeyLen;
            if (RfsNodeBare(T, N)) {
                RfsRecordKey(T->Rule, p, pk); pkey = pk; pkl = 16;
            }
            int cmp = RfsKeyCompare(&Vol, T->Rule, pkey, pkl, key, kl);
            if (T->Rule < 0x10 && !(N->Level && (r->Flags & REFS_RF_LASTCHILD)) &&
                (cmp > 0 || (cmp == 0 && (pkl == kl && !memcmp(pkey, key, kl))))) {
                OrderErrors++;
                if (OrderErrors < 20) {
                    ULONG j;
                    printf("ORDER %s schema %lx rule %u level %u row %lu: ", What, (unsigned long)T->Schema, T->Rule, N->Level, (unsigned long)i);
                    for (j = 0; j < pkl && j < 40; j++) printf("%02x", pkey[j]);
                    printf(" !< ");
                    for (j = 0; j < kl && j < 40; j++) printf("%02x", key[j]);
                    printf("\n");
                }
            }
        }
        if (Upper && !(N->Level == 0 && (N->Flags & REFS_NF_BARE) && 0) && RfsKeyCompare(&Vol, T->Rule, key, kl, Upper, UpperLen) > 0) {
            OrderErrors++;
            if (OrderErrors < 20) printf("BOUND %s schema %lx row %lu above its index key\n", What, (unsigned long)T->Schema, (unsigned long)i);
        }
        if (N->Level) {
            PRFS_NODE child;
            RFS_STATUS st = RfsLoadChild(&Vol, T, N, i, &child);
            if (!RFS_SUCCESS(st)) { Errors++; printf("CHILD %s row %lu: %08lX\n", What, (unsigned long)i, (unsigned long)(ULONG)st); continue; }
            CheckNode(T, child, What, (r->KeyLen && !(r->Flags & REFS_RF_LASTCHILD)) ? r->Key : Upper,
                      (r->KeyLen && !(r->Flags & REFS_RF_LASTCHILD)) ? r->KeyLen : UpperLen, Depth + 1);
        } else if (r->Flags & REFS_RF_EMBEDDED) {
            PRFS_TABLE e;
            if (RFS_SUCCESS(RfsTableOpenEmbedded(&Vol, T->Oid, r->Val, r->ValLen, &e))) {
                char w[300];
                snprintf(w, sizeof(w), "%s>emb", What);
                CheckNode(e, e->Root, w, NULL, 0, Depth + 1);
                RfsTableClose(&Vol, e);
            } else {
                Errors++;
                printf("EMBEDDED %s row %lu does not decode\n", What, (unsigned long)i);
            }
        }
    }
}

static void
Order(void)
{
    ULONG i;
    RFS_CURSOR c;
    RFS_STATUS st;
    for (i = 0; i < Vol.NRoots; i++) {
        PRFS_TABLE t;
        char w[32];
        snprintf(w, sizeof(w), "root%lu", (unsigned long)i);
        if (!RFS_SUCCESS(RfsTableOpenRoot(&Vol, i, &t))) { Errors++; printf("root %lu does not open\n", (unsigned long)i); continue; }
        CheckNode(t, t->Root, w, NULL, 0, 0);
        RfsTableClose(&Vol, t);
    }
    st = RfsSeek(Vol.Object, NULL, 0, &c);
    while (RFS_SUCCESS(st)) {
        PRFS_ROW r = RfsCurRow(&c);
        ULONGLONG oid = RfsGet64(r->Key + 8);
        PRFS_TABLE t;
        char w[32];
        snprintf(w, sizeof(w), "oid%llx", (unsigned long long)oid);
        if (RFS_SUCCESS(RfsTableOpenObject(&Vol, oid, &t))) {
            CheckNode(t, t->Root, w, NULL, 0, 0);
            RfsTableClose(&Vol, t);
        } else {
            Errors++;
            printf("object %llx does not open\n", (unsigned long long)oid);
        }
        st = RfsNext(&c);
    }
    printf("%lu pages, %lu rows, %lu order errors, %lu other errors, %lu checksum errors\n",
           Pages, Rows, OrderErrors, Errors, (unsigned long)Vol.ChecksumErrors);
}


/* ---- consistency: every referenced cluster is allocated in its allocator, none twice -------------------- */
static PUCHAR Owned;
static unsigned long long OwnedCount, Dups, NotAlloc, CheckPages, MediumCovered, MediumFree;

static int
TierIsAlloc(UCHAR Tier, ULONGLONG P)
{
    static const ULONG roots[3] = { REFS_ROOT_MEDIUM, REFS_ROOT_CONTALLOC, REFS_ROOT_SMALL };
    PRFS_TABLE t;
    RFS_CURSOR c;
    UCHAR key[16];
    int res = -1;
    if (!RFS_SUCCESS(RfsTableOpenRoot(&Vol, roots[Tier], &t))) return -1;
    RfsPut64(key, P);
    RfsPut64(key + 8, 1);
    if (RFS_SUCCESS(RfsLookup(t, key, 16, &c))) {
        PRFS_ROW r = RfsCurRow(&c);
        ULONGLONG s = RfsGet64(r->Val);
        if (RfsGet16(r->Val + 0x12) & 1) {
            ULONG i = (ULONG)(P - s), bo = r->Val[0x14] ? r->Val[0x14] : 0x18;
            res = (r->Val[bo + (i >> 3)] >> (i & 7)) & 1;
        } else {
            res = RfsGet16(r->Val + 0x10) == 0;
        }
    }
    RfsCursorClose(&c);
    RfsTableClose(&Vol, t);
    return res;
}

static void
Own(ULONGLONG P, UCHAR Tier, const char *What)
{
    int a;
    if (P >= Vol.TotalClusters) { NotAlloc++; if (NotAlloc < 20) printf("RANGE %s cluster %llx\n", What, (unsigned long long)P); return; }
    if (Owned[P >> 3] & (1 << (P & 7))) { Dups++; if (Dups < 20) printf("DUP %s cluster %llx\n", What, (unsigned long long)P); return; }
    Owned[P >> 3] |= (UCHAR)(1 << (P & 7));
    OwnedCount++;
    a = TierIsAlloc(Tier, P);
    if (a != 1) { NotAlloc++; if (NotAlloc < 20) printf("FREE %s cluster %llx (tier %u: %d)\n", What, (unsigned long long)P, Tier, a); }
}

static void
OwnRun(ULONGLONG Vlcn, ULONG Run, const char *What)
{
    while (Run) {
        ULONGLONG p;
        ULONG n, k;
        if (!RFS_SUCCESS(RfsTranslate(&Vol, Vlcn, &p, &n))) { NotAlloc++; printf("UNMAPPED %s %llx\n", What, (unsigned long long)Vlcn); return; }
        if (n > Run) n = Run;
        for (k = 0; k < n; k++) Own(p + k, 0, What);
        Vlcn += n; Run -= n;
    }
}

static void CheckTable(PRFS_TABLE T, PRFS_NODE N, UCHAR Tier, const char *What, int Depth);

static void
CheckRows(PRFS_TABLE T, PRFS_NODE N, const char *What, int Depth)
{
    ULONG i;
    for (i = 0; i < N->NRows; i++) {
        PRFS_ROW r = &N->Rows[i];
        if (RfsNodeBare(T, N)) {
            if (r->ValLen >= REFS_EXT_LENGTH) {
                ULONGLONG l = RfsGet64(r->Val);
                USHORT f = RfsGet16(r->Val + 8);
                if (l && !(f & REFS_EXTF_SPARSE)) OwnRun(l, RfsGet32(r->Val + REFS_EXT_RUN), What);
            }
        } else if (r->Flags & REFS_RF_EMBEDDED) {
            PRFS_TABLE e;
            if (RFS_SUCCESS(RfsTableOpenEmbedded(&Vol, T->Oid, r->Val, r->ValLen, &e))) {
                CheckTable(e, e->Root, 0, What, Depth + 1);
                RfsTableClose(&Vol, e);
            }
        }
    }
}

static void
CheckTable(PRFS_TABLE T, PRFS_NODE N, UCHAR Tier, const char *What, int Depth)
{
    ULONG i;
    if (Depth > 12) return;
    if (N->HasRef) {
        CheckPages++;
        for (i = 0; i < Vol.ClustersPerPage; i++) {
            if (T->Real) Own(N->Ref.Lcn[i], Tier, What);
            else OwnRun(N->Ref.Lcn[i], 1, What);
        }
        if (!T->Real && Tier) {
            /* a virtual page of a tier-2 table: check that tier too */
            ULONGLONG p; ULONG n;
            if (RFS_SUCCESS(RfsTranslate(&Vol, N->Ref.Lcn[0], &p, &n)) && TierIsAlloc(Tier, p) != 1) {
                NotAlloc++; if (NotAlloc < 20) printf("TIER %s page %llx not in tier %u\n", What, (unsigned long long)p, Tier);
            }
        }
    }
    if (N->Level) {
        for (i = 0; i < N->NRows; i++) {
            PRFS_NODE ch;
            if (RFS_SUCCESS(RfsLoadChild(&Vol, T, N, i, &ch))) CheckTable(T, ch, Tier, What, Depth + 1);
            else { Errors++; printf("CHILD %s\n", What); }
        }
    } else {
        CheckRows(T, N, What, Depth);
    }
}

/* the allocator rows: free counts against their bitmaps, the summaries */
static void
CheckAllocators(void)
{
    static const ULONG roots[3] = { REFS_ROOT_MEDIUM, REFS_ROOT_CONTALLOC, REFS_ROOT_SMALL };
    ULONG t;
    for (t = 0; t < 3; t++) {
        PRFS_TABLE T;
        RFS_CURSOR c;
        RFS_STATUS st;
        unsigned long long freeSum = 0, bad = 0;
        if (!RFS_SUCCESS(RfsTableOpenRoot(&Vol, roots[t], &T))) continue;
        st = RfsSeek(T, NULL, 0, &c);
        while (RFS_SUCCESS(st)) {
            PRFS_ROW r = RfsCurRow(&c);
            ULONGLONG len = RfsGet64(r->Val + 8);
            ULONG f = RfsGet16(r->Val + 0x10), n = 0, i;
            if (RfsGet16(r->Val + 0x12) & 1) {
                ULONG bo = r->Val[0x14] ? r->Val[0x14] : 0x18;
                for (i = 0; i < (ULONG)len; i++) if (!((r->Val[bo + (i >> 3)] >> (i & 7)) & 1)) n++;
                if (n != f && !(f == 0xFFFF && n > 0xFFFF)) { bad++; if (bad < 5) printf("ALLOC tier %lu row %llx: free %lu, bitmap %lu\n", (unsigned long)t, (unsigned long long)RfsGet64(r->Val), (unsigned long)f, (unsigned long)n); }
                freeSum += n;
            } else if (f) {
                freeSum += len;
            }
            st = RfsNext(&c);
        }
        RfsCursorClose(&c);
        if (t == 0) MediumFree = freeSum;
        if (t == 0 && T->Root->Desc && T->Root->DescLen >= 0x40) MediumCovered = RfsGet64(T->Root->Desc + REFS_IR_DATA + RFS_SUM_COVERED(&Vol));
        if (T->Root->Desc && T->Root->DescLen >= 0x40) {
            unsigned long long sum = RfsGet64(T->Root->Desc + REFS_IR_DATA + RFS_SUM_FREE(&Vol));
            printf("tier %lu: %llu clusters free in rows, summary %llu%s, %llu bad rows\n", (unsigned long)t, freeSum, sum,
                   sum == freeSum ? "" : " (DIFFERS)", bad);
        }
        RfsTableClose(&Vol, T);
    }
}

static void
Check(void)
{
    ULONG i;
    RFS_CURSOR c;
    RFS_STATUS st;
    Owned = (PUCHAR)calloc(1, (size_t)(Vol.TotalClusters / 8 + 1));
    if (!Owned) { printf("no memory\n"); return; }
    for (i = 0; i < Vol.NRoots; i++) {
        PRFS_TABLE t;
        char w[32];
        UCHAR tier = (i == 1 || i == 2 || i == 6 || i == 11) ? 1 : (i == 7 || i == 8 || i == 12) ? 2 : 0;
        snprintf(w, sizeof(w), "root%lu", (unsigned long)i);
        if (!RFS_SUCCESS(RfsTableOpenRoot(&Vol, i, &t))) { Errors++; continue; }
        CheckTable(t, t->Root, tier, w, 0);
        RfsTableClose(&Vol, t);
    }
    st = RfsSeek(Vol.Object, NULL, 0, &c);
    while (RFS_SUCCESS(st)) {
        ULONGLONG oid = RfsGet64(RfsCurRow(&c)->Key + 8);
        PRFS_TABLE t;
        char w[32];
        snprintf(w, sizeof(w), "oid%llx", (unsigned long long)oid);
        if (RFS_SUCCESS(RfsTableOpenObject(&Vol, oid, &t))) {
            CheckTable(t, t->Root, 0, w, 0);
            RfsTableClose(&Vol, t);
        }
        st = RfsNext(&c);
    }
    CheckAllocators();
    printf("allocated but not referenced: %llu\n", MediumCovered - MediumFree - OwnedCount);
    printf("%llu pages, %llu clusters referenced, %llu referenced twice, %llu not allocated, %lu errors, %lu checksum errors\n",
           CheckPages, OwnedCount, Dups, NotAlloc, Errors, (unsigned long)Vol.ChecksumErrors);
    free(Owned);
}

/* ---- changes: a list of operations in one mount ------------------------------------------------------------- */
static int
Utf16Of(const char *S, USHORT *Out, ULONG *Len)
{
    ULONG n = RfsUtf8ToUtf16(S, (ULONG)strlen(S), Out, RFS_MAX_NAME);
    if (n == (ULONG)-1) return 0;
    *Len = n;
    return 1;
}

/* the directory holding Path's last component, and the component */
static RFS_STATUS
SplitPath(const char *Path, PULONGLONG Dir, USHORT *Name, ULONG *Len)
{
    char buf[4096], *slash;
    RFS_DIRENT e;
    RFS_STATUS st;
    snprintf(buf, sizeof(buf), "%s", Path);
    slash = strrchr(buf, '/');
    if (!slash) return RFS_BAD_NAME;
    *slash = 0;
    st = Walk(buf[0] ? buf : "/", &e, NULL);
    if (!RFS_SUCCESS(st)) return st;
    if (e.Type != RFS_FT_DIR) return RFS_NOT_A_DIRECTORY;
    *Dir = e.Oid;
    return Utf16Of(slash + 1, Name, Len) ? RFS_OK : RFS_BAD_NAME;
}

static RFS_STATUS
OpenPath(const char *Path, PRFS_FILE *F)
{
    RFS_DIRENT e;
    ULONGLONG parent;
    RFS_STATUS st = Walk(Path, &e, &parent);
    if (!RFS_SUCCESS(st)) return st;
    return RfsFileOpen(&Vol, parent, &e, F);
}

static RFS_STATUS
PutFile(const char *Local, const char *Path, ULONGLONG At, int Create)
{
    FILE *in = fopen(Local, "rb");
    static UCHAR buf[1 << 20];
    PRFS_FILE f;
    RFS_STATUS st;
    size_t n;
    if (!in) { perror(Local); return RFS_IO_ERROR; }
    if (Create) {
        ULONGLONG dir;
        USHORT name[RFS_MAX_NAME + 1];
        ULONG len;
        RFS_DIRENT e;
        st = SplitPath(Path, &dir, name, &len);
        if (RFS_SUCCESS(st)) st = RfsCreate(&Vol, dir, name, len, FALSE, 0x20, &e);
        if (!RFS_SUCCESS(st)) { fclose(in); return st; }
    }
    st = OpenPath(Path, &f);
    if (!RFS_SUCCESS(st)) { fclose(in); return st; }
    while (RFS_SUCCESS(st) && (n = fread(buf, 1, sizeof(buf), in)) > 0) {
        st = RfsFileWrite(&Vol, f, At, (ULONG)n, buf);
        At += n;
    }
    fclose(in);
    if (RFS_SUCCESS(st)) st = RfsFileSave(&Vol, f);
    RfsFileClose(f);
    return st;
}

#ifndef _WIN32
/* a local directory tree into the volume (Path: an existing directory, "" = the root) */
static RFS_STATUS
PutTree(const char *Local, const char *Path, unsigned long *Files)
{
    DIR *d = opendir(Local);
    struct dirent *de;
    RFS_STATUS st = RFS_OK;
    if (!d) { perror(Local); return RFS_IO_ERROR; }
    while (RFS_SUCCESS(st) && (de = readdir(d)) != NULL) {
        char lp[4096], vp[4096];
        struct stat sb;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        snprintf(lp, sizeof(lp), "%s/%s", Local, de->d_name);
        snprintf(vp, sizeof(vp), "%s/%s", Path, de->d_name);
        if (stat(lp, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) {
            ULONGLONG dir; USHORT name[RFS_MAX_NAME + 1]; ULONG len; RFS_DIRENT e;
            st = SplitPath(vp, &dir, name, &len);
            if (RFS_SUCCESS(st)) st = RfsCreate(&Vol, dir, name, len, TRUE, 0x10, &e);
            if (st == RFS_EXISTS) st = RFS_OK;
            if (RFS_SUCCESS(st)) st = PutTree(lp, vp, Files);
        } else if (S_ISREG(sb.st_mode)) {
            st = PutFile(lp, vp, 0, 1);
            if (RFS_SUCCESS(st)) (*Files)++;
        }
        if (!RFS_SUCCESS(st)) fprintf(stderr, "%s: %08lX\n", vp, (unsigned long)st);
        if (RFS_SUCCESS(st) && *Files % 256 == 255) st = RfsCommit(&Vol);
    }
    closedir(d);
    return st;
}
#endif

static RFS_STATUS
DoOne(char *Op)
{
    char *a[5] = { 0 };
    int n = 0;
    char *p = Op;
    a[n++] = p;
    while (*p && n < 5) { if (*p == ':') { *p = 0; a[n++] = p + 1; } p++; }
    if (!strcmp(a[0], "commit")) return RfsCommit(&Vol);
    if (!strcmp(a[0], "mkdir") && n >= 2) {
        ULONGLONG dir; USHORT name[RFS_MAX_NAME + 1]; ULONG len; RFS_DIRENT e;
        RFS_STATUS st = SplitPath(a[1], &dir, name, &len);
        return RFS_SUCCESS(st) ? RfsCreate(&Vol, dir, name, len, TRUE, 0x10, &e) : st;
    }
    if (!strcmp(a[0], "touch") && n >= 2) {
        ULONGLONG dir; USHORT name[RFS_MAX_NAME + 1]; ULONG len; RFS_DIRENT e;
        RFS_STATUS st = SplitPath(a[1], &dir, name, &len);
        return RFS_SUCCESS(st) ? RfsCreate(&Vol, dir, name, len, FALSE, 0x20, &e) : st;
    }
    if (!strcmp(a[0], "put") && n >= 3) return PutFile(a[1], a[2], 0, 1);
#ifndef _WIN32
    if (!strcmp(a[0], "putr") && n >= 3) {
        unsigned long files = 0;
        RFS_STATUS st = PutTree(a[1], strcmp(a[2], "/") ? a[2] : "", &files);
        printf("%lu files\n", files);
        return st;
    }
#endif
    if (!strcmp(a[0], "write") && n >= 4) return PutFile(a[3], a[1], strtoull(a[2], NULL, 0), 0);
    if (!strcmp(a[0], "rm") && n >= 2) {
        ULONGLONG dir; USHORT name[RFS_MAX_NAME + 1]; ULONG len;
        RFS_STATUS st = SplitPath(a[1], &dir, name, &len);
        return RFS_SUCCESS(st) ? RfsRemove(&Vol, dir, name, len) : st;
    }
    if ((!strcmp(a[0], "mv") || !strcmp(a[0], "mvf")) && n >= 3) {
        ULONGLONG d1, d2; USHORT n1[RFS_MAX_NAME + 1], n2[RFS_MAX_NAME + 1]; ULONG l1, l2;
        RFS_STATUS st = SplitPath(a[1], &d1, n1, &l1);
        if (RFS_SUCCESS(st)) st = SplitPath(a[2], &d2, n2, &l2);
        return RFS_SUCCESS(st) ? RfsRename(&Vol, d1, n1, l1, d2, n2, l2, !strcmp(a[0], "mvf")) : st;
    }
    if (!strcmp(a[0], "trunc") && n >= 3) {
        PRFS_FILE f;
        RFS_STATUS st = OpenPath(a[1], &f);
        if (!RFS_SUCCESS(st)) return st;
        st = RfsFileSetSize(&Vol, f, strtoull(a[2], NULL, 0), FALSE);
        if (RFS_SUCCESS(st)) st = RfsFileSave(&Vol, f);
        RfsFileClose(f);
        return st;
    }
    if (!strcmp(a[0], "reloc") && n >= 5) {
        PRFS_FILE f;
        ULONG moved;
        RFS_STATUS st = OpenPath(a[1], &f);
        if (!RFS_SUCCESS(st)) return st;
        st = RfsFileRelocate(&Vol, f, strtoull(a[2], NULL, 0), strtoull(a[3], NULL, 0), a[4][0] == 'i', &moved);
        printf("%s: %lu clusters moved, placed %d\n", a[1], (unsigned long)moved,
               RfsFilePlaced(&Vol, f, strtoull(a[2], NULL, 0), strtoull(a[3], NULL, 0)));
        RfsFileClose(f);
        return st;
    }
    fprintf(stderr, "unknown operation %s\n", a[0]);
    return RFS_NOT_SUPPORTED;
}

int
main(int argc, char **argv)
{
    RFS_STATUS st;
    const char *cmd;
    if (argc < 3) {
        fprintf(stderr, "usage: rfstool IMAGE info|ls|lsr|cat|get|files|order|check|do OPS...\n");
        return 2;
    }
    if (!strcmp(argv[2], "mkfs1")) {
        RFS_STATUS st;
        if (!RimgOpen(&Img, argv[1], -1, 0)) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
        Vol.DeviceBytes = Img.Size; Vol.SectorSize = Img.Sector;
        st = RfsFormatV1(&Vol, argc > 3 ? argv[3] : "ReFS", 65536);
        printf("mkfs1: %08lX\n", (unsigned long)st);
        RimgClose(&Img);
        return RFS_SUCCESS(st) ? 0 : 1;
    }
    cmd = argv[2];
    {
        int rw = !strcmp(cmd, "do");
        if (!RimgOpen(&Img, argv[1], -1, !rw)) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
        Vol.DeviceBytes = Img.Size;
        Vol.SectorSize = Img.Sector;
        st = RfsMount(&Vol, (BOOLEAN)!rw);
        if (!RFS_SUCCESS(st)) Die("mount", st);
        if (rw && Vol.ReadOnly) { fprintf(stderr, "read-only: %s\n", Vol.ReadOnlyWhy ? Vol.ReadOnlyWhy : "?"); return 1; }
    }
    if (!strcmp(cmd, "info")) {
        Info();
    } else if (!strcmp(cmd, "ls") || !strcmp(cmd, "lsr")) {
        RFS_DIRENT e;
        st = Walk(argc > 3 ? argv[3] : "/", &e, NULL);
        if (!RFS_SUCCESS(st)) Die("walk", st);
        if (e.Type != RFS_FT_DIR) Die("not a directory", RFS_NOT_A_DIRECTORY);
        Ls(e.Oid, "", !strcmp(cmd, "lsr"));
    } else if ((!strcmp(cmd, "cat") || !strcmp(cmd, "get")) && argc > 3) {
        RFS_DIRENT e;
        ULONGLONG parent;
        PRFS_FILE f;
        unsigned long long h;
        FILE *out = stdout;
        st = Walk(argv[3], &e, &parent);
        if (!RFS_SUCCESS(st)) Die("walk", st);
        st = RfsFileOpen(&Vol, parent, &e, &f);
        if (!RFS_SUCCESS(st)) Die("open", st);
        if (!strcmp(cmd, "get")) {
            if (argc < 5) return 2;
            out = fopen(argv[4], "wb");
            if (!out) { perror(argv[4]); return 1; }
        }
        st = Digest(f, &h, out);
        if (out != stdout) fclose(out);
        if (!RFS_SUCCESS(st)) Die("read", st);
        RfsFileClose(f);
    } else if (!strcmp(cmd, "files")) {
        AllFiles(REFS_OID_ROOT, "/");
        fprintf(stderr, "%lu files, %lu directories, %lu errors, %lu checksum errors\n", Files, Dirs, Errors, (unsigned long)Vol.ChecksumErrors);
    } else if (!strcmp(cmd, "order")) {
        Order();
    } else if (!strcmp(cmd, "check")) {
        Check();
    } else if (!strcmp(cmd, "do")) {
        int i;
        for (i = 3; i < argc; i++) {
            char op[8192];
            snprintf(op, sizeof(op), "%s", argv[i]);
            st = DoOne(op);
            if (!RFS_SUCCESS(st)) { fprintf(stderr, "%s: %08lX\n", argv[i], (unsigned long)(ULONG)st); break; }
        }
        if (RFS_SUCCESS(st)) st = RfsCommit(&Vol);
        if (!RFS_SUCCESS(st)) Die("commit", st);
        printf("free clusters %llu, clock %llu\n", (unsigned long long)RfsFreeCount(&Vol), (unsigned long long)Vol.Clock);
    } else {
        fprintf(stderr, "unknown command\n");
        return 2;
    }
    RfsUnmount(&Vol);
    RimgClose(&Img);
    return 0;
}

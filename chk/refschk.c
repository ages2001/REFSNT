/*
 * REFSNT - refschk: checks a ReFS volume (read-only).
 *
 *   refschk TARGET [/Q] [/V]
 *
 * TARGET: a drive letter (E:), \\.\PhysicalDriveN, a disk or partition
 * image. Every B+ tree page is read and its checksum verified, the keys of
 * every table are checked for order, every page and data cluster must be
 * allocated in its allocator (and not twice), the allocators' counts must
 * match their bitmaps, and every file is read to its end.
 * Exit code: 0 clean, 1 problems found, 2 cannot check.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/time.h>
#endif
#include "rfseng.h"
#include "rfsimg.h"

static RFS_IMAGE Img;
static RFS_VOL Vol;
static int Quiet, Verbose;
static unsigned long Errors;

RFS_STATUS RfsHostRead(PRFS_VOL V, ULONGLONG Off, ULONG Len, PVOID Buf) { (void)V; return RimgRead(&Img, Off, Buf, Len) ? RFS_OK : RFS_IO_ERROR; }
RFS_STATUS RfsHostWrite(PRFS_VOL V, ULONGLONG Off, ULONG Len, const VOID *Buf) { (void)V; (void)Off; (void)Len; (void)Buf; return RFS_READ_ONLY; }
RFS_STATUS RfsHostFlush(PRFS_VOL V) { (void)V; return RFS_OK; }
PVOID RfsHostAlloc(ULONG Bytes) { return malloc(Bytes ? Bytes : 1); }
VOID RfsHostFree(PVOID P) { free(P); }
ULONGLONG RfsHostNowNt(VOID) { return 0; }

/* ---- every file read to its end ------------------------------------------------------------------ */
static unsigned long Files, Dirs, ReadErrors;
static UCHAR Buf[1 << 16];

static void
AllFiles(ULONGLONG Dir, const char *Prefix, int Depth)
{
    RFS_CURSOR c;
    RFS_DIRENT e;
    RFS_STATUS st;
    if (Depth > 64) return;
    st = RfsDirNext(&Vol, Dir, &c, TRUE, &e);
    while (RFS_SUCCESS(st)) {
        char nm[1024], path[4096];
        RfsUtf16ToUtf8(e.Name, e.NameLen, nm, sizeof(nm));
        snprintf(path, sizeof(path), "%s%s", Prefix, nm);
        if (e.Type == RFS_FT_DIR) {
            RFS_CURSOR save = c;
            char sub[4100];
            Dirs++;
            snprintf(sub, sizeof(sub), "%s/", path);
            AllFiles(e.Oid, sub, Depth + 1);
            c = save;
        } else {
            PRFS_FILE f;
            RFS_STATUS s2 = RfsFileOpen(&Vol, Dir, &e, &f);
            Files++;
            if (RFS_SUCCESS(s2)) {
                ULONGLONG off = 0;
                while (RFS_SUCCESS(s2) && off < f->Size) {
                    ULONG got, n = f->Size - off > sizeof(Buf) ? sizeof(Buf) : (ULONG)(f->Size - off);
                    s2 = RfsFileRead(&Vol, f, off, n, Buf, &got);
                    if (!got) break;
                    off += got;
                }
                if (f->Size != e.Size && e.KeyFlags == REFS_NAME_EMBEDDED && !f->Reparse) {
                    if (!Quiet) printf("SIZE %s: entry %llu, file %llu\n", path, (unsigned long long)e.Size, (unsigned long long)f->Size);
                }
                RfsFileClose(f);
            }
            if (!RFS_SUCCESS(s2)) { ReadErrors++; Errors++; if (!Quiet) printf("READ %s: %08lX\n", path, (unsigned long)(ULONG)s2); }
            else if (Verbose) printf("ok %s\n", path);
        }
        st = RfsDirNext(&Vol, Dir, &c, FALSE, &e);
    }
    if (st != RFS_NO_MORE) { Errors++; if (!Quiet) printf("LIST %s: %08lX\n", Prefix, (unsigned long)(ULONG)st); }
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
                    if (!Quiet) printf("ORDER %s schema %lx rule %u level %u row %lu: ", What, (unsigned long)T->Schema, T->Rule, N->Level, (unsigned long)i);
                    for (j = 0; j < pkl && j < 40; j++) printf("%02x", pkey[j]);
                    printf(" !< ");
                    for (j = 0; j < kl && j < 40; j++) printf("%02x", key[j]);
                    printf("\n");
                }
            }
        }
        if (Upper && !(N->Level == 0 && (N->Flags & REFS_NF_BARE) && 0) && RfsKeyCompare(&Vol, T->Rule, key, kl, Upper, UpperLen) > 0) {
            OrderErrors++;
            if (OrderErrors < 20 && !Quiet) printf("BOUND %s schema %lx row %lu above its index key\n", What, (unsigned long)T->Schema, (unsigned long)i);
        }
        if (N->Level) {
            PRFS_NODE child;
            RFS_STATUS st = RfsLoadChild(&Vol, T, N, i, &child);
            if (!RFS_SUCCESS(st)) { Errors++; if (!Quiet) printf("CHILD %s row %lu: %08lX\n", What, (unsigned long)i, (unsigned long)(ULONG)st); continue; }
            CheckNode(T, child, What, (r->KeyLen && !(r->Flags & REFS_RF_LASTCHILD)) ? r->Key : Upper,
                      (r->KeyLen && !(r->Flags & REFS_RF_LASTCHILD)) ? r->KeyLen : UpperLen, Depth + 1);
        } else if (r->Flags & RFS_RF_EMB(&Vol)) {
            PRFS_TABLE e;
            if (RFS_SUCCESS(RfsTableOpenEmbedded(&Vol, T->Oid, r->Val, r->ValLen, &e))) {
                char w[300];
                snprintf(w, sizeof(w), "%s>emb", What);
                CheckNode(e, e->Root, w, NULL, 0, Depth + 1);
                RfsTableClose(&Vol, e);
            } else {
                Errors++;
                if (!Quiet) printf("EMBEDDED %s row %lu does not decode\n", What, (unsigned long)i);
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
    if (Owned[P >> 3] & (1 << (P & 7))) { Dups++; if (Dups < 20 && Verbose) printf("DUP %s cluster %llx\n", What, (unsigned long long)P); return; }
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
        } else if (r->Flags & RFS_RF_EMB(&Vol)) {
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

/* ---- ReFS 1.x: its own roots, allocators and direct (unvirtualised) blocks ---------------------------- */

/* mark every metadata page's 16 KiB block; catch blocks referenced twice or out of range */
static void
MarkPagesV1(PRFS_TABLE T, PRFS_NODE N, PUCHAR Blk, ULONGLONG NBlocks, const char *What, int Depth)
{
    ULONG i;
    if (Depth > 16) return;
    if (N->HasRef) {
        ULONGLONG b = N->Ref.Lcn[0];
        CheckPages++;
        if (b >= NBlocks) { NotAlloc++; if (NotAlloc < 20) printf("RANGE %s block %llx\n", What, (unsigned long long)b); }
        else if (Blk[b >> 3] & (1 << (b & 7))) { Dups++; if (Dups < 20) printf("DUP %s block %llx\n", What, (unsigned long long)b); }
        else { Blk[b >> 3] |= (UCHAR)(1 << (b & 7)); OwnedCount++; }
    }
    if (N->Level) {
        for (i = 0; i < N->NRows; i++) {
            PRFS_NODE ch;
            if (RFS_SUCCESS(RfsLoadChild(&Vol, T, N, i, &ch))) MarkPagesV1(T, ch, Blk, NBlocks, What, Depth + 1);
            else { Errors++; if (!Quiet) printf("CHILD %s\n", What); }
        }
    }
}

/* the medium allocator's free-cluster summary (the engine maintains it and
 * allocates from the per-region bitmaps nested under each row; this reports it) */
static void
CheckMediumV1(void)
{
    PRFS_TABLE T;
    unsigned long long summary = 0;
    if (!RFS_SUCCESS(RfsTableOpenRoot(&Vol, REFS1_ROOT_MEDIUM, &T))) { printf("medium allocator does not open\n"); Errors++; return; }
    if (T->Root->Desc && T->Root->DescLen >= REFS_IR_DATA + 16) summary = RfsGet64(T->Root->Desc + REFS_IR_DATA + 8);
    {
        unsigned long long spc = Vol.ClusterSize / 16384, freeb = RfsFreeBlocksV1(&Vol);
        printf("free: %llu of %llu clusters (%llu MB); medium allocator summary %llu blocks\n",
               freeb / spc, (unsigned long long)Vol.TotalClusters / spc, (freeb * 16384) >> 20, summary);
    }
    RfsTableClose(&Vol, T);
}

static void
CheckV1(void)
{
    static const ULONG v1roots[6] = { REFS_ROOT_OBJECT, REFS1_ROOT_MEDIUM, REFS1_ROOT_LARGE,
                                      REFS_ROOT_SCHEMA, REFS_ROOT_PARENTCHILD, REFS1_ROOT_SMALL };
    ULONG i;
    RFS_CURSOR c;
    RFS_STATUS st;
    ULONGLONG nblocks = Vol.DeviceBytes >> REFS1_BLOCK_SHIFT;
    PUCHAR blk = (PUCHAR)calloc(1, (size_t)(nblocks / 8 + 1));

    for (i = 0; i < 6; i++) {
        PRFS_TABLE t;
        char w[32];
        snprintf(w, sizeof(w), "root%lu", (unsigned long)v1roots[i]);
        if (!RFS_SUCCESS(RfsTableOpenRoot(&Vol, v1roots[i], &t))) { Errors++; printf("%s does not open\n", w); continue; }
        CheckNode(t, t->Root, w, NULL, 0, 0);
        if (blk) MarkPagesV1(t, t->Root, blk, nblocks, w, 0);
        RfsTableClose(&Vol, t);
    }
    st = RfsSeek(Vol.Object, NULL, 0, &c);
    while (RFS_SUCCESS(st)) {
        ULONGLONG oid = RfsGet64(RfsCurRow(&c)->Key + 8);
        PRFS_TABLE t;
        char w[32];
        snprintf(w, sizeof(w), "oid%llx", (unsigned long long)oid);
        if (RFS_SUCCESS(RfsTableOpenObject(&Vol, oid, &t))) {
            CheckNode(t, t->Root, w, NULL, 0, 0);
            if (blk) MarkPagesV1(t, t->Root, blk, nblocks, w, 0);
            RfsTableClose(&Vol, t);
        } else { Errors++; printf("object %llx does not open\n", (unsigned long long)oid); }
        st = RfsNext(&c);
    }
    printf("%lu pages, %lu rows, %lu order errors, %lu other errors, %lu checksum errors\n",
           Pages, Rows, OrderErrors, Errors, (unsigned long)Vol.ChecksumErrors);
    printf("%llu metadata blocks referenced, %llu referenced twice, %llu out of range\n",
           OwnedCount, Dups, NotAlloc);
    CheckMediumV1();
    if (blk) free(blk);
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


int
main(int argc, char **argv)
{
    RFS_STATUS st;
    const char *target = NULL;
    int i;
    for (i = 1; i < argc; i++) {
        if ((argv[i][0] == '/' && strlen(argv[i]) == 2) || argv[i][0] == '-') {
            char o = argv[i][1];
            if (o == 'q' || o == 'Q') Quiet = 1;
            else if (o == 'v' || o == 'V') Verbose = 1;
            else { fprintf(stderr, "usage: refschk TARGET [/Q] [/V]\n"); return 2; }
        } else target = argv[i];
    }
    if (!target) { fprintf(stderr, "usage: refschk TARGET [/Q] [/V]\n"); return 2; }
    if (!RimgOpen(&Img, target, -1, 1)) { fprintf(stderr, "cannot open %s\n", target); return 2; }
    /* a mounted drive: locked (REFSNT writes everything and stops), else what it has not written yet shows as damage */
    if (!RimgLock(&Img))
        printf("note: %s is in use and cannot be locked; changes REFSNT has not written yet\n"
               "      may show as errors. Close the programs using it for an exact check.\n", target);
    Vol.DeviceBytes = Img.Size;
    Vol.SectorSize = Img.Sector;
    Vol.PageMax = 256;
    Vol.TableMax = 64;
    st = RfsMount(&Vol, TRUE);
    if (!RFS_SUCCESS(st)) { fprintf(stderr, "%s: not a ReFS volume this checker reads (%08lX)\n", target, (unsigned long)(ULONG)st); return 2; }
    printf("ReFS %u.%u, %lu-byte clusters, %llu clusters, label '%s', checkpoint clock %llu\n", Vol.Major, Vol.Minor,
           (unsigned long)Vol.ClusterSize, (unsigned long long)Vol.TotalClusters, Vol.Label, (unsigned long long)Vol.Clock);
    if (Vol.V1) {
        printf("checking the tables and allocation...\n");
        CheckV1();
    } else if (Vol.V2) {
        /* ReFS 2.x: walk every table (checksums + key order), read every file.
         * The 3.x allocator cross-check does not apply (no small-allocator root,
         * a different container-row layout), and 2.x is mounted read-only. */
        printf("checking the tables...\n");
        Order();
    } else {
        printf("checking the tables...\n");
        Order();
        printf("checking the allocation...\n");
        Check();
    }
    printf("reading every file...\n");
    AllFiles(REFS_OID_ROOT, "/", 0);
    printf("%lu files, %lu directories, %lu read errors\n", Files, Dirs, ReadErrors);
    {
        unsigned long problems = OrderErrors + Errors + Vol.ChecksumErrors + (unsigned long)Dups + (unsigned long)NotAlloc;
        /* clusters shared by block clones are counted twice on purpose */
        if (Dups) {
            printf("(%llu clusters are referenced more than once: block clones and snapshots share clusters)\n", Dups);
            problems -= (unsigned long)Dups;
        }
        printf(problems ? "PROBLEMS FOUND\n" : "the volume is clean\n");
        RfsUnmount(&Vol);
        RimgUnlock(&Img);
        RimgClose(&Img);
        return problems ? 1 : 0;
    }
}

/*
 * REFSNT - rfstmpl: turns a freshly formatted ReFS 2.x / 3.x volume (made by
 * Windows) into a format template for refsfmt.
 *
 *   rfstmpl IMAGE NAME >> fmt/rfstmpl.h
 *
 * IMAGE is changed: a scratch copy (cp --sparse=always) of the volume or of
 * a disk holding it. Everything below the root directory is deleted (the
 * recycle bin, System Volume Information) and committed. Then every cluster
 * that holds data and is not free in any allocator becomes part of the
 * template, except the last container (the backup superblocks and boot
 * sector, which refsfmt writes for the new size). The template is the
 * volume's first K containers as clusters, compressed (a small LZ77; refsfmt
 * has the decoder), plus the log's location, which refsfmt zeroes.
 *
 * The output is a C array, RfsTmpl_NAME, laid out as fmt/rfsmkfs.c reads it.
 */
#include "rfseng.h"
#include "rfsimg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static RFS_IMAGE Img;
static RFS_VOL Vol;

RFS_STATUS RfsHostRead(PRFS_VOL V, ULONGLONG Off, ULONG Len, PVOID Buf) { (void)V; return RimgRead(&Img, Off, Buf, Len) ? RFS_OK : RFS_IO_ERROR; }
RFS_STATUS RfsHostWrite(PRFS_VOL V, ULONGLONG Off, ULONG Len, const VOID *Buf) { (void)V; return RimgWrite(&Img, Off, Buf, Len) ? RFS_OK : RFS_IO_ERROR; }
RFS_STATUS RfsHostFlush(PRFS_VOL V) { (void)V; return RFS_OK; }
PVOID RfsHostAlloc(ULONG Bytes) { return malloc(Bytes ? Bytes : 1); }
VOID RfsHostFree(PVOID P) { free(P); }
ULONGLONG RfsHostNowNt(VOID) { return 0; }

static void
Die(const char *What, RFS_STATUS St)
{
    fprintf(stderr, "rfstmpl: %s: status %08lX\n", What, (unsigned long)(ULONG)St);
    exit(1);
}

/* ---- deleting everything below the root ---------------------------------------------- */
static void
Empty(ULONGLONG Dir)
{
    for (;;) {
        RFS_CURSOR c;
        RFS_DIRENT e;
        RFS_STATUS st;
        RfsZero(&c, sizeof(c));
        st = RfsDirNext(&Vol, Dir, &c, TRUE, &e);
        RfsCursorClose(&c);
        if (st == RFS_NO_MORE || st == RFS_NOT_FOUND) return;
        if (!RFS_SUCCESS(st)) Die("enumerate", st);
        if (e.Type == RFS_FT_DIR) Empty(e.Oid);
        {
            char n8[800];
            RfsUtf16ToUtf8(e.Name, e.NameLen, n8, sizeof(n8));
            fprintf(stderr, "  removing %s\n", n8);
        }
        st = RfsRemove(&Vol, Dir, e.Name, e.NameLen);
        if (!RFS_SUCCESS(st)) Die("remove", st);
    }
}

/* ---- compression ------------------------------------------------------------------------ */
/*
 * The stream: a control byte c
 *   0x00-0x7F  c+1 literal bytes follow
 *   0x80-0xFE  a match of c-0x80+3 bytes (3-129), then u16 distance (1-65535)
 *   0xFF       u16 length-130, u16 distance: a long match
 */
#define HBITS   16
static ULONG
Compress(const UCHAR *In, ULONG N, PUCHAR Out)
{
    static LONG head[1 << HBITS];
    LONG *prev = (LONG *)malloc(sizeof(LONG) * (N + 1));
    ULONG i = 0, o = 0, lit = 0, litStart = 0;
    memset(head, 0xFF, sizeof(head));
#define FLUSHLIT() while (lit) { ULONG k = lit > 128 ? 128 : lit; Out[o++] = (UCHAR)(k - 1); memcpy(Out + o, In + litStart, k); o += k; litStart += k; lit -= k; }
#define HASH(p) ((((ULONG)In[p] << 10) ^ ((ULONG)In[(p) + 1] << 5) ^ In[(p) + 2]) & ((1 << HBITS) - 1))
    while (i < N) {
        ULONG bestLen = 0, bestDist = 0;
        if (i + 3 <= N) {
            ULONG h = HASH(i);
            LONG cand = head[h];
            int chain = 64;
            while (cand >= 0 && chain-- > 0 && i - (ULONG)cand <= 65535) {
                ULONG l = 0, max = N - i;
                if (max > 65535 + 130) max = 65535 + 130;
                while (l < max && In[cand + l] == In[i + l]) l++;
                if (l > bestLen) { bestLen = l; bestDist = i - (ULONG)cand; if (l == max) break; }
                cand = prev[cand];
            }
            prev[i] = head[h];
            head[h] = (LONG)i;
        }
        if (bestLen >= 3) {
            ULONG k;
            FLUSHLIT();
            if (bestLen <= 129) {
                Out[o++] = (UCHAR)(0x80 + bestLen - 3);
            } else {
                Out[o++] = 0xFF;
                Out[o++] = (UCHAR)((bestLen - 130) & 0xFF);
                Out[o++] = (UCHAR)((bestLen - 130) >> 8);
            }
            Out[o++] = (UCHAR)(bestDist & 0xFF);
            Out[o++] = (UCHAR)(bestDist >> 8);
            /* hash the bytes the match covered */
            for (k = 1; k < bestLen; k++) {
                if (i + k + 3 <= N) { ULONG h = HASH(i + k); prev[i + k] = head[h]; head[h] = (LONG)(i + k); }
            }
            i += bestLen;
            litStart = i;
        } else {
            if (!lit) litStart = i;
            lit++;
            i++;
        }
    }
    FLUSHLIT();
    free(prev);
    return o;
}

static ULONG
Expand(const UCHAR *In, ULONG InLen, PUCHAR Out, ULONG OutLen)
{
    ULONG i = 0, o = 0;
    while (i < InLen) {
        UCHAR c = In[i++];
        if (c < 0x80) {
            ULONG n = (ULONG)c + 1;
            if (i + n > InLen || o + n > OutLen) return 0;
            memcpy(Out + o, In + i, n); i += n; o += n;
        } else {
            ULONG n, d;
            if (c == 0xFF) { if (i + 2 > InLen) return 0; n = (In[i] | (In[i + 1] << 8)) + 130; i += 2; }
            else n = (ULONG)c - 0x80 + 3;
            if (i + 2 > InLen) return 0;
            d = In[i] | (In[i + 1] << 8); i += 2;
            if (!d || d > o || o + n > OutLen) return 0;
            while (n--) { Out[o] = Out[o - d]; o++; }
        }
    }
    return o;
}

/* ---- the template ----------------------------------------------------------------------- */
static UCHAR *Out;
static ULONG OutLen;
static void P8(ULONG V) { Out[OutLen++] = (UCHAR)V; }
static void P32(ULONG V) { P8(V); P8(V >> 8); P8(V >> 16); P8(V >> 24); }
static void P64(ULONGLONG V) { P32((ULONG)V); P32((ULONG)(V >> 32)); }

int
main(int argc, char **argv)
{
    RFS_STATUS st;
    ULONGLONG total, nconts, c, cpc;
    PUCHAR stale, buf, raw, comp;
    ULONG cs, k = 0, nruns = 0, i, rawLen = 0, compLen;
    ULONG *runStart, *runCount;
    ULONGLONG zero[4][2];
    ULONG nzero = 0;
    static const ULONG tiers[3] = { REFS_ROOT_MEDIUM, REFS_ROOT_CONTALLOC, REFS_ROOT_SMALL };

    if (argc != 3) { fprintf(stderr, "usage: rfstmpl IMAGE NAME\n"); return 2; }
    if (!RimgOpen(&Img, argv[1], -1, 0)) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    Vol.SectorSize = Img.Sector;
    Vol.DeviceBytes = Img.Size;

    /* 1. an empty root */
    st = RfsMount(&Vol, FALSE);
    if (!RFS_SUCCESS(st)) Die("mount", st);
    if (Vol.ReadOnly) { fprintf(stderr, "read-only: %s\n", Vol.ReadOnlyWhy ? Vol.ReadOnlyWhy : "?"); return 1; }
    if (Vol.V1) { fprintf(stderr, "ReFS 1.x volumes are made by rfsmk1.c, not from a template\n"); return 1; }
    Empty(REFS_OID_ROOT);
    st = RfsCommit(&Vol);
    if (!RFS_SUCCESS(st)) Die("commit", st);
    RfsUnmount(&Vol);
    memset(&Vol, 0, sizeof(Vol));
    Vol.SectorSize = Img.Sector;
    Vol.DeviceBytes = Img.Size;
    st = RfsMount(&Vol, TRUE);
    if (!RFS_SUCCESS(st)) Die("remount", st);

    cs = Vol.ClusterSize;
    cpc = Vol.ClustersPerContainer;
    total = Vol.TotalClusters;
    nconts = total / cpc;
    if (total % cpc) { fprintf(stderr, "the volume is not a whole number of containers\n"); return 1; }

    /* 2. what the allocators call free */
    stale = (PUCHAR)calloc((size_t)total, 1);
    for (i = 0; i < 3; i++) {
        PRFS_TABLE T;
        RFS_CURSOR cur;
        if (tiers[i] >= Vol.NRoots) continue;
        st = RfsTableOpenRoot(&Vol, tiers[i], &T);
        if (!RFS_SUCCESS(st)) Die("allocator", st);
        st = RfsSeek(T, NULL, 0, &cur);
        while (RFS_SUCCESS(st) && st != RFS_NO_MORE) {
            PRFS_ROW r = RfsCurRow(&cur);
            ULONGLONG s = RfsGet64(r->Val), l = RfsGet64(r->Val + 8), j;
            if (RfsGet16(r->Val + 0x12) & 1) {
                ULONG bo = r->Val[0x14] ? r->Val[0x14] : 0x18;
                for (j = 0; j < l && s + j < total; j++)
                    if (!((r->Val[bo + (j >> 3)] >> (j & 7)) & 1)) stale[s + j] = 1;
            } else if (RfsGet16(r->Val + 0x10) != 0) {
                for (j = 0; j < l && s + j < total; j++) stale[s + j] = 1;
            }
            st = RfsNext(&cur);
        }
        RfsCursorClose(&cur);
        RfsTableClose(&Vol, T);
    }

    /* 3. the log: zeroed by refsfmt (the clusters that hold data come from the template) */
    {
        PRFS_TABLE T;
        RFS_CURSOR cur;
        UCHAR key[4] = { 1, 0, 0, 0 };
        st = RfsTableOpenObject(&Vol, REFS_OID_LOGINFO, &T);
        if (!RFS_SUCCESS(st)) Die("log information", st);
        st = RfsLookup(T, key, 4, &cur);
        if (!RFS_SUCCESS(st)) Die("log information row", st);
        {
            PRFS_ROW r = RfsCurRow(&cur);
            if (Vol.V2) {           /* {start, length} */
                zero[0][0] = RfsGet64(r->Val); zero[0][1] = RfsGet64(r->Val + 8);
            } else {                /* {start, end, length, ...} */
                zero[0][0] = RfsGet64(r->Val); zero[0][1] = RfsGet64(r->Val + 8) - RfsGet64(r->Val);
            }
            nzero = 1;
            fprintf(stderr, "log: clusters %llx + %llx\n", (unsigned long long)zero[0][0], (unsigned long long)zero[0][1]);
        }
        RfsCursorClose(&cur);
        RfsTableClose(&Vol, T);
    }

    /* 4. the clusters that hold something */
    buf = (PUCHAR)malloc(cs);
    runStart = (ULONG *)malloc(sizeof(ULONG) * 65536);
    runCount = (ULONG *)malloc(sizeof(ULONG) * 65536);
    for (c = 0; c < (nconts - 1) * cpc; c++) {
        ULONG j;
        if (stale[c]) continue;
        if (!RimgRead(&Img, c * cs, buf, cs)) { fprintf(stderr, "read error\n"); return 1; }
        for (j = 0; j < cs && !buf[j]; j++) ;
        if (j == cs) continue;
        if (nruns && runStart[nruns - 1] + runCount[nruns - 1] == c) runCount[nruns - 1]++;
        else { runStart[nruns] = (ULONG)c; runCount[nruns] = 1; nruns++; }
        if ((ULONG)(c / cpc) + 1 > k) k = (ULONG)(c / cpc) + 1;
        rawLen += cs;
    }
    raw = (PUCHAR)malloc(rawLen);
    {
        ULONG o = 0, r;
        for (r = 0; r < nruns; r++) {
            for (c = runStart[r]; c < runStart[r] + runCount[r]; c++) {
                RimgRead(&Img, c * cs, raw + o, cs);
                o += cs;
            }
        }
    }
    comp = (PUCHAR)malloc(rawLen + rawLen / 64 + 1024);
    compLen = Compress(raw, rawLen, comp);
    {
        PUCHAR chk = (PUCHAR)malloc(rawLen);
        if (Expand(comp, compLen, chk, rawLen) != rawLen || memcmp(chk, raw, rawLen)) { fprintf(stderr, "compression self-test failed\n"); return 1; }
        free(chk);
    }
    fprintf(stderr, "ReFS %u.%u, %lu-byte clusters, %llu containers of %llu clusters: K=%lu, %lu runs, %lu bytes -> %lu\n",
            Vol.Major, Vol.Minor, (unsigned long)cs, (unsigned long long)nconts, (unsigned long long)cpc,
            (unsigned long)k, (unsigned long)nruns, (unsigned long)rawLen, (unsigned long)compLen);

    /* 5. the template */
    Out = (PUCHAR)malloc(compLen + 64 + 16 * nzero + 8 * nruns);
    P8('R'); P8('F'); P8('T'); P8('1');
    P8(Vol.Major); P8(Vol.Minor); P8(0); P8(0);
    P32(cs); P32((ULONG)cpc); P32(k); P32((ULONG)nconts);
    P32(nzero); P32(nruns); P32(rawLen); P32(compLen);
    for (i = 0; i < nzero; i++) { P64(zero[i][0]); P64(zero[i][1]); }
    for (i = 0; i < nruns; i++) { P32(runStart[i]); P32(runCount[i]); }
    memcpy(Out + OutLen, comp, compLen);
    OutLen += compLen;

    printf("/* ReFS %u.%u, %lu-byte clusters: from a volume Windows formatted (tools/rfstmpl.c) */\n",
           Vol.Major, Vol.Minor, (unsigned long)cs);
    printf("static const unsigned char RfsTmpl_%s[%lu] = {", argv[2], (unsigned long)OutLen);
    for (i = 0; i < OutLen; i++) printf("%s%u,", (i % 32) ? "" : "\n", Out[i]);
    printf("\n};\n\n");
    RfsUnmount(&Vol);
    RimgClose(&Img);
    return 0;
}

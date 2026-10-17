/*
 * REFSNT - the FAT16 view that lets Windows NT start from a ReFS volume
 *
 * NTLDR reads only FAT and NTFS (and on 3.x HPFS). So the files it needs are
 * also given a FAT16 view, kept inside the ReFS volume:
 *
 *   sectors 0-1     the REFSNT boot loader with a FAT16 BPB (ReFS keeps its
 *                   first clusters free and allocated); the ReFS boot sector
 *                   moves to sector 2 (and stays at the end of the volume)
 *   boot area       the clusters of REFSBOOT.SYS, a hidden system file in the
 *                   root, low on the volume: the FAT, the root directory,
 *                   then the clusters of the FAT subdirectories
 *   clusters        4 KB (or the ReFS cluster, at most 32 KB): cluster 2
 *                   begins right after the root directory, and a FAT file's
 *                   clusters are the very clusters of the ReFS file, so the
 *                   view always shows the current contents
 *
 * The view holds NTLDR, NTDETECT.COM, BOOT.INI (and the other loader files
 * of the root), and from the Windows directory what the loader reads: the
 * kernel and HAL, the SYSTEM hive, the NLS tables and fonts, and every
 * driver in SYSTEM32\DRIVERS. Files lying outside the 65518 clusters the
 * FAT can reach are moved inside (ReFS keeps small files in their records:
 * those get clusters too). Clusters the view does not use are marked bad.
 * While a volume has a view, REFSNT allocates in the window last, so the
 * boot files can grow in place.
 */

#include "rfsshim.h"

#define FAT_SECTORS     256             /* 65526 entries of 2 bytes */
#define ROOT_SECTORS    16              /* 256 entries */
#define DIR_BYTES       (128 * 1024)    /* the subdirectories' clusters */
#define MAX_CLUSTERS    65524
#define WIN_CLUSTERS    65518
#define VBR_COPY        2               /* the sector the ReFS boot sector moves to */

typedef struct _SFILE {
    UCHAR       Name[11];
    UCHAR       Case;               /* NT case flags */
    UCHAR       Attr;
    int         Dir;                /* subdirectory index, or -1 */
    ULONG       Size;
    ULONGLONG   Mtime;
    ULONGLONG   Parent;             /* the directory holding it */
    ULONGLONG   Oid;                /* subdirectories: their object */
    ULONGLONG   Home, FileId;       /* files: their identity */
    USHORT      UName[64];          /* its ReFS name */
    ULONG       ULen;
    ULONG       First;              /* first cluster */
} SFILE;

typedef struct _SDIR {
    ULONGLONG   Oid;
    int         Parent;
    SFILE      *F;
    ULONG       Count, Cap;
    ULONG       First;              /* first cluster (0 = the root) */
    ULONG       Clusters;
} SDIR;

#define SHIM_MAX_DIRS   16

typedef struct _SHIM {
    PRFS_SHIM_HOST H;
    PRFS_VOL    Vol;
    PRFS_SHIM_RESULT R;
    ULONG       B, Spb;             /* cluster size, sectors a cluster (ReFS) */
    ULONG       C, Spc;             /* cluster size, sectors a cluster (the view) */
    ULONGLONG   A, M, K;            /* boot area: first cluster, clusters, FAT + root clusters */
    ULONG       Fs;                 /* FAT sectors */
    ULONGLONG   Base, WinEnd;       /* the cluster of FAT cluster 2, the end of the window */
    ULONGLONG   DataSec;            /* the sector of FAT cluster 2 */
    ULONG       Clusters;           /* clusters the FAT describes */
    ULONG       DirClusters;
    ULONGLONG   AreaHome, AreaId;   /* REFSBOOT.SYS */
    SDIR        D[SHIM_MAX_DIRS];
    ULONG       NDirs;
    USHORT     *Fat;
} SHIM;

/* ------------------------------------------------------------------ */
/* Small helpers (no C library: this file is built into the driver)    */
/* ------------------------------------------------------------------ */

static PVOID
ShAlloc(ULONG Bytes)
{
    PVOID P = RfsHostAlloc(Bytes ? Bytes : 1);
    if (P) RfsZero(P, Bytes);
    return P;
}

static VOID
ShFree(PVOID P)
{
    if (P) RfsHostFree(P);
}

static BOOLEAN
ShGrow(PVOID *Arr, PULONG Cap, ULONG Need, ULONG Elem)
{
    ULONG N;
    PVOID X;
    if (Need <= *Cap) return TRUE;
    N = *Cap ? *Cap : 16;
    while (N < Need) N *= 2;
    X = ShAlloc(N * Elem);
    if (!X) return FALSE;
    if (*Arr) { RfsCopy(X, *Arr, *Cap * Elem); RfsHostFree(*Arr); }
    *Arr = X;
    *Cap = N;
    return TRUE;
}

static VOID
ShFill(PVOID P, int C, ULONG N)
{
    PUCHAR B = (PUCHAR)P;
    while (N--) *B++ = (UCHAR)C;
}

static ULONG
ShLen(const char *S)
{
    ULONG N = 0;
    while (S[N]) N++;
    return N;
}

static const char *
ShChr(const char *S, char C)
{
    for (; *S; S++) if (*S == C) return S;
    return 0;
}

static int
Fail(SHIM *S, const char *Msg)
{
    if (!S->R->Error) S->R->Error = Msg;
    return -1;
}

static VOID
FailName(SHIM *S, const char *Msg, const UCHAR *Name)
{
    if (S->R->Error) return;
    S->R->Error = Msg;
    RfsCopy(S->R->ErrorName, Name, 11);
    S->R->ErrorName[11] = 0;
}

/* Volume sectors (512 bytes), in pieces */
static int
ShRead(SHIM *S, ULONGLONG Sector, ULONG Count, PVOID Buf)
{
    PUCHAR P = (PUCHAR)Buf;
    while (Count) {
        ULONG n = Count > 64 ? 64 : Count;
        if (!RFS_SUCCESS(RfsHostRead(S->Vol, Sector << 9, n << 9, P))) return -1;
        Sector += n; Count -= n; P += n * 512;
    }
    return 0;
}

static int
ShWrite(SHIM *S, ULONGLONG Sector, ULONG Count, const VOID *Buf)
{
    const UCHAR *P = (const UCHAR *)Buf;
    while (Count) {
        ULONG n = Count > 64 ? 64 : Count;
        if (!RFS_SUCCESS(RfsHostWrite(S->Vol, Sector << 9, n << 9, P))) return -1;
        Sector += n; Count -= n; P += n * 512;
    }
    return 0;
}

static VOID
ShCommit(SHIM *S)
{
    if (S->H->Commit) (VOID)S->H->Commit(S->H->Ctx);
    else (VOID)RfsCommit(S->Vol);
}

BOOLEAN
RfsShimPresent(const UCHAR *Sectors)
{
    return (BOOLEAN)(!RfsCmp(Sectors + 3, RFS_SHIM_OEM, 8) && Sectors[510] == 0x55 && Sectors[511] == 0xAA);
}

/* The view's cluster size for a ReFS cluster size: 4 KB, or the cluster, at most 32 KB */
static ULONG
ClusterBytes(ULONG B)
{
    return B < 4096 ? 4096 : B > 32768 ? 32768 : B;
}

BOOLEAN
RfsShimWindow(PRFS_VOL Vol, const UCHAR *S0, PULONGLONG Lo, PULONGLONG Hi)
{
    ULONG Spb, Spc, Res, Spf;
    ULONGLONG Base, End;
    if (!RfsShimPresent(S0) || Vol->V1) return FALSE;
    Spb = Vol->ClusterSize / 512;
    Spc = S0[0x0D];
    Res = S0[0x0E] | (S0[0x0F] << 8);
    Spf = S0[0x16] | (S0[0x17] << 8);
    if (!Spc || !Spb) return FALSE;
    Base = (Res + Spf + ROOT_SECTORS) / Spb;
    End = Base + ((ULONGLONG)WIN_CLUSTERS * Spc) / Spb;
    if (End > Vol->TotalClusters) End = Vol->TotalClusters;
    *Lo = Base;
    *Hi = End;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Names                                                               */
/* ------------------------------------------------------------------ */

/* A name as a FAT 8.3 name (upper case, space padded); FALSE if it is not one */
static BOOLEAN
To83(const char *Name, ULONG Len, UCHAR *Out, UCHAR *Case)
{
    static const char Ok[] = "$%'-_@~`!(){}^#&";
    ULONG i, Dot = Len, n;
    BOOLEAN BaseLower = FALSE, BaseUpper = FALSE, ExtLower = FALSE, ExtUpper = FALSE;
    ShFill(Out, ' ', 11);
    *Case = 0;
    if (Len == 0 || Name[0] == '.') return FALSE;
    for (i = 0; i < Len; i++) if (Name[i] == '.') { if (Dot != Len) return FALSE; Dot = i; }
    if (Dot == 0 || Dot > 8 || (Dot < Len && Len - Dot - 1 > 3) || Dot == Len - 1) return FALSE;
    for (i = 0, n = 0; i < Len; i++) {
        char C = Name[i];
        BOOLEAN InExt = (BOOLEAN)(i > Dot);
        if (i == Dot) continue;
        if (C >= 'a' && C <= 'z') { if (InExt) ExtLower = TRUE; else BaseLower = TRUE; C = (char)(C - 32); }
        else if (C >= 'A' && C <= 'Z') { if (InExt) ExtUpper = TRUE; else BaseUpper = TRUE; }
        else if (!(C >= '0' && C <= '9') && !ShChr(Ok, C)) return FALSE;
        Out[InExt ? 8 + (i - Dot - 1) : n++] = (UCHAR)C;
    }
    if (BaseLower && !BaseUpper) *Case |= 0x08;
    if (ExtLower && !ExtUpper) *Case |= 0x10;
    return TRUE;
}

/* Wildcard match of an 8.3 pattern ("HAL*.DLL") against a name, no case */
static BOOLEAN
Pattern(const char *Name, ULONG Len, const char *Pat)
{
    if (*Pat == 0) return (BOOLEAN)(Len == 0);
    if (*Pat == '*') {
        ULONG i;
        for (i = 0; i <= Len; i++) if (Pattern(Name + i, Len - i, Pat + 1)) return TRUE;
        return FALSE;
    }
    if (Len == 0) return FALSE;
    {
        char a = *Name, b = *Pat;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b && b != '?') return FALSE;
    }
    return Pattern(Name + 1, Len - 1, Pat + 1);
}

/* An entry's name as plain ASCII (FALSE: it has other characters, or is long) */
static BOOLEAN
AsciiName(const RFS_DIRENT *E, char *Out, PULONG Len)
{
    ULONG i;
    if (E->NameLen > 63) return FALSE;
    for (i = 0; i < E->NameLen; i++) {
        if (E->Name[i] < 0x20 || E->Name[i] > 0x7E) return FALSE;
        Out[i] = (char)E->Name[i];
    }
    Out[i] = 0;
    *Len = E->NameLen;
    return TRUE;
}

/* Finds Name (any case) in directory Dir; FALSE if it is not there (or not of the kind wanted) */
static BOOLEAN
Find(SHIM *S, ULONGLONG Dir, const char *Name, BOOLEAN WantDir, PRFS_DIRENT Ent)
{
    USHORT U[64];
    ULONG n = ShLen(Name), i;
    if (n == 0 || n > 63) return FALSE;
    for (i = 0; i < n; i++) U[i] = (UCHAR)Name[i];
    if (!RFS_SUCCESS(RfsDirLookup(S->Vol, Dir, U, n, Ent))) return FALSE;
    return (BOOLEAN)((Ent->Type == RFS_FT_DIR) == WantDir);
}

static int
AddEntry(SHIM *S, int Dir, ULONGLONG Parent, const RFS_DIRENT *E, int SubDir)
{
    SDIR *D = &S->D[Dir];
    SFILE *F;
    ULONG k, len;
    char Asc[64];
    UCHAR N83[11], Case;
    if (!AsciiName(E, Asc, &len) || !To83(Asc, len, N83, &Case)) return 0;
    for (k = 0; k < D->Count; k++) if (!RfsCmp(D->F[k].Name, N83, 11)) return 0;   /* two names, one 8.3 */
    if (SubDir < 0 && (E->Size >= 0xFFFFFFFFULL || E->Type != RFS_FT_FILE)) return 0;
    if (!ShGrow((PVOID *)&D->F, &D->Cap, D->Count + 1, sizeof(SFILE))) return -1;
    F = &D->F[D->Count++];
    ShFill(F, 0, sizeof(*F));
    RfsCopy(F->Name, N83, 11);
    F->Case = Case;
    F->Dir = SubDir;
    F->Attr = (UCHAR)(SubDir >= 0 ? 0x10 : 0x20);
    F->Size = SubDir >= 0 ? 0 : (ULONG)E->Size;
    F->Mtime = E->Times.Write;
    F->Parent = Parent;
    F->Oid = E->Oid;
    F->Home = E->Home;
    F->FileId = E->FileId;
    RfsCopy(F->UName, E->Name, E->NameLen * sizeof(USHORT));
    F->ULen = E->NameLen;
    return 1;
}

static int
NewDir(SHIM *S, int Parent, ULONGLONG ParentOid, const RFS_DIRENT *E)
{
    int Idx;
    if (S->NDirs == sizeof(S->D) / sizeof(S->D[0])) return -1;
    Idx = (int)S->NDirs++;
    ShFill(&S->D[Idx], 0, sizeof(SDIR));
    S->D[Idx].Oid = E ? E->Oid : REFS_OID_ROOT;
    S->D[Idx].Parent = Parent;
    if (Parent >= 0 && AddEntry(S, Parent, ParentOid, E, Idx) <= 0) return -1;
    return Idx;
}

/* Every file of directory Oid whose name matches one of the patterns (NULL = all) */
static int
AddFiles(SHIM *S, int Dir, ULONGLONG Oid, const char *const *Pats)
{
    RFS_DIRENT *E;
    RFS_CURSOR Cur;
    RFS_STATUS St;
    int Rc = 0;
    E = (RFS_DIRENT *)ShAlloc(sizeof(RFS_DIRENT));
    if (!E) return -1;
    St = RfsDirNext(S->Vol, Oid, &Cur, TRUE, E);
    while (Rc == 0 && RFS_SUCCESS(St)) {
        char Asc[64];
        ULONG len;
        int k, Take = (Pats == NULL);
        if (E->Type == RFS_FT_FILE && AsciiName(E, Asc, &len) && Asc[0] != '.') {
            for (k = 0; Pats && Pats[k] && !Take; k++) if (Pattern(Asc, len, Pats[k])) Take = 1;
            if (Take && AddEntry(S, Dir, Oid, E, -1) < 0) Rc = -1;
        }
        St = RfsDirNext(S->Vol, Oid, &Cur, FALSE, E);
    }
    if (RFS_SUCCESS(St)) RfsCursorClose(&Cur);
    ShFree(E);
    return Rc;
}

static BOOLEAN
IsBootFile(SHIM *S, const RFS_DIRENT *E)
{
    ULONG d, k;
    if (E->Type == RFS_FT_FILE && E->Home == S->AreaHome && E->FileId == S->AreaId) return TRUE;
    for (d = 0; d < S->NDirs; d++) {
        for (k = 0; k < S->D[d].Count; k++) {
            SFILE *F = &S->D[d].F[k];
            if (F->Dir < 0 && E->Type == RFS_FT_FILE && F->Home == E->Home && F->FileId == E->FileId) return TRUE;
        }
    }
    return FALSE;
}

/* the engine file of a view file */
static RFS_STATUS
ShOpenFile(SHIM *S, ULONGLONG Parent, const USHORT *Name, ULONG Len, PRFS_FILE *Out)
{
    RFS_DIRENT *E = (RFS_DIRENT *)ShAlloc(sizeof(RFS_DIRENT));
    RFS_STATUS St;
    if (!E) return RFS_NO_MEMORY;
    St = RfsDirLookup(S->Vol, Parent, Name, Len, E);
    if (RFS_SUCCESS(St)) St = RfsFileOpen(S->Vol, Parent, E, Out);
    ShFree(E);
    return St;
}

/* ------------------------------------------------------------------ */
/* Space in the window                                                 */
/* ------------------------------------------------------------------ */

static ULONG
FreeClusters(SHIM *S, ULONGLONG Lo, ULONGLONG Hi)
{
    ULONGLONG c;
    ULONG N = 0;
    for (c = Lo; c < Hi; c++) if (RfsClusterFree(S->Vol, c)) N++;
    return N;
}

/* Clusters a file needs moved to sit in the window */
static ULONG
ClustersToMove(SHIM *S, SFILE *F)
{
    PRFS_FILE I;
    ULONG N = 0;
    if (!RFS_SUCCESS(ShOpenFile(S, F->Parent, F->UName, F->ULen, &I))) return 0;
    if (!RfsFilePlaced(S->Vol, I, S->Base, S->WinEnd)) N = (ULONG)((I->Size + S->B - 1) / S->B);
    RfsFileClose(I);
    return N;
}

/* Moves files other than the boot files out of [Lo, Hi) until Need clusters are free in it */
static int
MakeRoom(SHIM *S, ULONGLONG Lo, ULONGLONG Hi, ULONG Need)
{
    PULONGLONG Stack = NULL;
    ULONG NStack = 0, CStack = 0, Free;
    RFS_DIRENT *E;
    int Rc = 0;

    Free = FreeClusters(S, Lo, Hi);
    if (Free >= Need) return 0;
    E = (RFS_DIRENT *)ShAlloc(sizeof(RFS_DIRENT));
    if (!E || !ShGrow((PVOID *)&Stack, &CStack, 1, sizeof(ULONGLONG))) { ShFree(E); return -1; }
    Stack[NStack++] = REFS_OID_ROOT;
    while (NStack && Free < Need) {
        ULONGLONG Dir = Stack[--NStack];
        RFS_CURSOR Cur;
        RFS_STATUS St = RfsDirNext(S->Vol, Dir, &Cur, TRUE, E);
        /* the names first (moving changes the directory under the cursor) */
        PRFS_DIRENT List = NULL;
        ULONG NList = 0, CList = 0, i;
        while (RFS_SUCCESS(St)) {
            if (E->Type == RFS_FT_DIR) {
                if (NStack < 4096 && ShGrow((PVOID *)&Stack, &CStack, NStack + 1, sizeof(ULONGLONG))) Stack[NStack++] = E->Oid;
            } else if (!IsBootFile(S, E) && ShGrow((PVOID *)&List, &CList, NList + 1, sizeof(RFS_DIRENT))) {
                List[NList++] = *E;
            }
            St = RfsDirNext(S->Vol, Dir, &Cur, FALSE, E);
        }
        if (RFS_SUCCESS(St)) RfsCursorClose(&Cur);
        for (i = 0; i < NList && Free < Need; i++) {
            PRFS_FILE I;
            ULONG Moved = 0;
            if (S->H->CanMove && !S->H->CanMove(S->H->Ctx, List[i].Home, List[i].FileId)) continue;
            if (!RFS_SUCCESS(RfsFileOpen(S->Vol, Dir, &List[i], &I))) continue;
            if (!I->Integrity && !I->Inline &&
                RFS_SUCCESS(RfsFileRelocate(S->Vol, I, Lo, Hi, FALSE, &Moved)) && Moved) {
                S->R->MovedOut++;
            }
            RfsFileClose(I);
            if (Moved) {
                if (S->H->Moved) S->H->Moved(S->H->Ctx, List[i].Home, List[i].FileId);
                ShCommit(S);
                Free = FreeClusters(S, Lo, Hi);
            }
        }
        ShFree(List);
    }
    ShFree(Stack);
    ShFree(E);
    Free = FreeClusters(S, Lo, Hi);
    if (Free < Need) Rc = -1;
    return Rc;
}

/* ------------------------------------------------------------------ */
/* The boot area                                                       */
/* ------------------------------------------------------------------ */

static int
Geometry(SHIM *S)
{
    PRFS_VOL V = S->Vol;
    if (V->V1) return Fail(S, "a boot view needs ReFS 2 or 3");
    if (V->SectorSize && V->SectorSize != 512) return Fail(S, "a boot view needs 512-byte sectors");
    S->B = V->ClusterSize;
    S->Spb = S->B / 512;
    S->C = ClusterBytes(S->B);
    S->Spc = S->C / 512;
    S->K = (FAT_SECTORS + ROOT_SECTORS + S->Spb - 1) / S->Spb;
    S->Fs = (ULONG)(S->K * S->Spb) - ROOT_SECTORS;
    S->R->ClusterBytes = S->C;
    return 0;
}

static ULONGLONG
AreaClusters(SHIM *S)
{
    ULONG Bytes = 16 * S->C, Dir;
    if (Bytes < DIR_BYTES) Bytes = DIR_BYTES;
    Dir = Bytes / S->B;
    return S->K + (Dir ? Dir : 1);
}

/* The highest cluster the boot area may start at: its FAT within the BPB's 65535 reserved sectors */
static ULONGLONG
AreaLimit(SHIM *S)
{
    return 65535 / S->Spb;
}

/* Finds REFSBOOT.SYS: one contiguous run, low on the disk */
static int
FindBootArea(SHIM *S)
{
    RFS_DIRENT *E;
    PRFS_FILE F;
    ULONGLONG p;
    ULONG run;
    E = (RFS_DIRENT *)ShAlloc(sizeof(RFS_DIRENT));
    if (!E) return Fail(S, "out of memory");
    if (!Find(S, REFS_OID_ROOT, RFS_SHIM_AREA_NAME, FALSE, E)) {
        ShFree(E);
        return Fail(S, "the volume has no boot area: run refsboot");
    }
    S->AreaHome = E->Home;
    S->AreaId = E->FileId;
    if (!RFS_SUCCESS(RfsFileOpen(S->Vol, REFS_OID_ROOT, E, &F))) { ShFree(E); return Fail(S, "the boot area cannot be read"); }
    ShFree(E);
    if (F->Inline || F->NExt != 1 || F->Ext[0].Vcn != 0 || !F->Ext[0].Lcn || (F->Size % S->B) != 0 ||
        F->Ext[0].Run < F->Size / S->B || !RFS_SUCCESS(RfsTranslate(S->Vol, F->Ext[0].Lcn, &p, &run)) ||
        run < F->Size / S->B) {
        RfsFileClose(F);
        return Fail(S, "the boot area (" RFS_SHIM_AREA_NAME ") is not one contiguous run: run refsboot again");
    }
    S->A = p;
    S->M = F->Size / S->B;
    RfsFileClose(F);
    if (S->A > AreaLimit(S) || S->M < S->K + 1 || ((S->M - S->K) * (ULONGLONG)S->B) % S->C != 0)
        return Fail(S, "the boot area is not usable: run refsboot again");
    S->Base = S->A + S->K;
    S->DataSec = S->Base * S->Spb;
    S->DirClusters = (ULONG)(((S->M - S->K) * S->B) / S->C);
    {
        ULONGLONG Cl = ((S->Vol->TotalClusters - S->Base) * S->B) / S->C;
        if (Cl > MAX_CLUSTERS) Cl = MAX_CLUSTERS;
        S->Clusters = (ULONG)Cl;
    }
    if (S->Clusters < 4085) return Fail(S, "the volume is too small for the boot view");
    S->WinEnd = S->Base + ((ULONGLONG)(S->Clusters < WIN_CLUSTERS ? S->Clusters : WIN_CLUSTERS) * S->C) / S->B;
    if (S->WinEnd > S->Vol->TotalClusters) S->WinEnd = S->Vol->TotalClusters;
    return 0;
}

RFS_STATUS
RfsShimReserve(PRFS_SHIM_HOST Host, PRFS_SHIM_RESULT Result)
{
    SHIM *S;
    PRFS_VOL V = Host->Vol;
    RFS_DIRENT *E = NULL;
    PRFS_FILE F = NULL;
    ULONGLONG M, Hi, Plcn = 0, Vlcn, k;
    ULONG Got = 0;
    RFS_STATUS St = RFS_OK;
    USHORT Name[16];
    ULONG n = 0;
    BOOLEAN Created = FALSE;

    RfsZero(Result, sizeof(*Result));
    if (V->ReadOnly) { Result->Error = "the volume is read-only"; return RFS_READ_ONLY; }
    S = (SHIM *)ShAlloc(sizeof(SHIM));
    E = (RFS_DIRENT *)ShAlloc(sizeof(RFS_DIRENT));
    if (!S || !E) { ShFree(S); ShFree(E); Result->Error = "out of memory"; return RFS_NO_MEMORY; }
    S->H = Host;
    S->Vol = V;
    S->R = Result;
    if (Geometry(S) != 0) { St = RFS_NOT_SUPPORTED; goto out; }
    if (Find(S, REFS_OID_ROOT, RFS_SHIM_AREA_NAME, FALSE, E)) goto out;     /* there is one */

    M = AreaClusters(S);
    Hi = AreaLimit(S) + M;
    if (Hi > V->TotalClusters) Hi = V->TotalClusters;
    St = RfsAllocIn(V, 1, Hi, (ULONG)M, (ULONG)M, 1, &Plcn, &Got);
    if (St == RFS_DISK_FULL) {
        /* other files out of the way: then the start of the volume has room */
        S->NDirs = 0;
        (VOID)MakeRoom(S, 0, Hi, (ULONG)M * 2);
        St = RfsAllocIn(V, 1, Hi, (ULONG)M, (ULONG)M, 1, &Plcn, &Got);
    }
    if (!RFS_SUCCESS(St)) { Fail(S, "no room for the boot area near the start of the volume"); goto out; }
    /* zeros, then the file */
    {
        PUCHAR Z = (PUCHAR)ShAlloc(S->B);
        if (!Z) { St = RFS_NO_MEMORY; goto undo; }
        for (k = 0; k < M && RFS_SUCCESS(St); k++) St = RfsHostWrite(V, (Plcn + k) * S->B, S->B, Z);
        ShFree(Z);
    }
    if (RFS_SUCCESS(St)) St = RfsToVirtual(V, Plcn, &Vlcn);
    for (n = 0; RFS_SHIM_AREA_NAME[n]; n++) Name[n] = (UCHAR)RFS_SHIM_AREA_NAME[n];
    if (RFS_SUCCESS(St)) {
        St = RfsCreate(V, REFS_OID_ROOT, Name, n, FALSE, 0x27, E);
        Created = RFS_SUCCESS(St);
    }
    if (RFS_SUCCESS(St)) St = RfsFileOpen(V, REFS_OID_ROOT, E, &F);
    if (RFS_SUCCESS(St)) {
        St = RfsFileAdopt(V, F, Vlcn, (ULONG)M);
        if (RFS_SUCCESS(St)) Plcn = ~0ULL;      /* the file's now */
    }
    if (F) RfsFileClose(F);
undo:
    if (!RFS_SUCCESS(St)) {
        if (Created) (VOID)RfsRemove(V, REFS_OID_ROOT, Name, n);
        if (Plcn != ~0ULL && Got) (VOID)RfsFree(V, RFS_TIER_MEDIUM, Plcn, Got);
        Fail(S, "the boot area could not be created");
    }
    ShCommit(S);
out:
    ShFree(E);
    ShFree(S);
    return St;
}

/* ------------------------------------------------------------------ */
/* The files of the view                                               */
/* ------------------------------------------------------------------ */

static VOID
CopyName(char *To, const char *From, ULONG Max)
{
    ULONG n = 0;
    if (*From == '\\' || *From == '/') From++;
    while (From[n] && n + 1 < Max) { To[n] = From[n]; n++; }
    To[n] = 0;
}

/* The Windows directory: given, recorded by refsboot, BOOT.INI's default entry, or found */
static VOID
FindWinDir(SHIM *S, const char *Given)
{
    char *Root = S->R->WinDir;
    UCHAR Sec[1024];
    RFS_DIRENT *E;
    ULONG k;

    Root[0] = 0;
    if (Given && *Given) { CopyName(Root, Given, sizeof(S->R->WinDir)); return; }
    if (ShRead(S, 0, 2, Sec) == 0 && RfsShimPresent(Sec) && Sec[RFS_SHIM_WINDIR] > ' ') {
        RfsCopy(Root, Sec + RFS_SHIM_WINDIR, 15);
        Root[15] = 0;
        return;
    }
    E = (RFS_DIRENT *)ShAlloc(sizeof(RFS_DIRENT));
    if (!E) return;
    if (Find(S, REFS_OID_ROOT, "BOOT.INI", FALSE, E)) {
        PRFS_FILE I;
        char *Txt = (char *)ShAlloc(4096);
        ULONG Got = 0;
        if (Txt && RFS_SUCCESS(RfsFileOpen(S->Vol, REFS_OID_ROOT, E, &I))) {
            if (RFS_SUCCESS(RfsFileRead(S->Vol, I, 0, 4095, Txt, &Got))) {
                char *P;
                Txt[Got] = 0;
                for (P = Txt; *P; P++) {
                    if ((P == Txt || P[-1] == '\n') && (P[0] | 0x20) == 'd' && !RfsCmp(P + 1, "efault", 6)) {
                        const char *Q = ShChr(P, '\\');
                        if (Q) {
                            ULONG n = 0;
                            Q++;
                            while (*Q && *Q != '\r' && *Q != '\n' && *Q != ' ' && *Q != '\t' && n < 15) Root[n++] = *Q++;
                            Root[n] = 0;
                        }
                        break;
                    }
                }
            }
            RfsFileClose(I);
        }
        ShFree(Txt);
        if (Root[0]) { ShFree(E); return; }
    }
    {
        static const char *const Guess[] = { "WINNT", "WINDOWS", "WINNT40", "WINNT35", "WINNT4", "WINNT351", "WINNT31", 0 };
        RFS_DIRENT *X = (RFS_DIRENT *)ShAlloc(sizeof(RFS_DIRENT));
        for (k = 0; X && Guess[k]; k++) {
            if (Find(S, REFS_OID_ROOT, Guess[k], TRUE, E) && Find(S, E->Oid, "SYSTEM32", TRUE, X)) {
                CopyName(Root, Guess[k], 16);
                break;
            }
        }
        ShFree(X);
    }
    ShFree(E);
}

static int
BuildTree(SHIM *S)
{
    static const char *const RootFiles[] = { "NTLDR", "NTDETECT.COM", "BOOT.INI", "BOOTSECT.DOS", "NTBOOTDD.SYS",
                                             "BOOTFONT.BIN", 0 };
    static const char *const Sys32Files[] = { "NT*.EXE", "HAL*.DLL", "KD*.DLL", "BOOTVID.DLL", "*.NLS", "*.FON",
                                              "C_*.*", "L_*.*", 0 };
    static const char *const ConfigFiles[] = { "SYSTEM", "SYSTEM.*", 0 };
    static const char *const FontFiles[] = { "*.FON", 0 };
    static const char *const SysRootFiles[] = { "BOOTSTAT.DAT", 0 };
    static const char *const AppPatchFiles[] = { "DRVMAIN.SDB", 0 };
    static const char *const InfFiles[] = { "BIOSINFO.INF", 0 };
    RFS_DIRENT *SysRoot, *Sys32, *E;
    int Wd, S32, Sr, Rc = 0;

    if (!S->R->WinDir[0]) return Fail(S, "cannot tell the Windows directory: give it with /S");
    if (ShChr(S->R->WinDir, '\\')) return Fail(S, "the Windows directory must be in the root of the volume");
    SysRoot = (RFS_DIRENT *)ShAlloc(3 * sizeof(RFS_DIRENT));
    if (!SysRoot) return Fail(S, "out of memory");
    Sys32 = SysRoot + 1;
    E = SysRoot + 2;
    if (!Find(S, REFS_OID_ROOT, S->R->WinDir, TRUE, SysRoot) || !Find(S, SysRoot->Oid, "SYSTEM32", TRUE, Sys32)) {
        ShFree(SysRoot);
        return Fail(S, "the Windows directory (with SYSTEM32) was not found");
    }
    S->NDirs = 0;
    NewDir(S, -1, 0, NULL);
    if (AddFiles(S, 0, REFS_OID_ROOT, RootFiles) < 0) { Rc = Fail(S, "out of memory"); goto out; }
    if (!Find(S, REFS_OID_ROOT, "NTLDR", FALSE, E)) { Rc = Fail(S, "NTLDR is not in the root of the volume"); goto out; }
    Wd = NewDir(S, 0, REFS_OID_ROOT, SysRoot);
    if (Wd < 0) { Rc = Fail(S, "the Windows directory has no 8.3 name"); goto out; }
    if (AddFiles(S, Wd, SysRoot->Oid, SysRootFiles) < 0) { Rc = Fail(S, "out of memory"); goto out; }
    S32 = NewDir(S, Wd, SysRoot->Oid, Sys32);
    if (S32 < 0 || AddFiles(S, S32, Sys32->Oid, Sys32Files) < 0) { Rc = Fail(S, "out of memory"); goto out; }
    if (Find(S, Sys32->Oid, "DRIVERS", TRUE, E)) {
        Sr = NewDir(S, S32, Sys32->Oid, E);
        if (Sr < 0 || AddFiles(S, Sr, E->Oid, 0) < 0) { Rc = Fail(S, "out of memory"); goto out; }
    }
    if (Find(S, Sys32->Oid, "CONFIG", TRUE, E)) {
        Sr = NewDir(S, S32, Sys32->Oid, E);
        if (Sr < 0 || AddFiles(S, Sr, E->Oid, ConfigFiles) < 0) { Rc = Fail(S, "out of memory"); goto out; }
    }
    if (Find(S, SysRoot->Oid, "FONTS", TRUE, E)) {
        Sr = NewDir(S, Wd, SysRoot->Oid, E);
        if (Sr < 0 || AddFiles(S, Sr, E->Oid, FontFiles) < 0) { Rc = Fail(S, "out of memory"); goto out; }
    }
    if (Find(S, SysRoot->Oid, "SYSTEM", TRUE, E)) {
        Sr = NewDir(S, Wd, SysRoot->Oid, E);
        if (Sr < 0 || AddFiles(S, Sr, E->Oid, FontFiles) < 0) { Rc = Fail(S, "out of memory"); goto out; }
    }
    if (Find(S, SysRoot->Oid, "INF", TRUE, E)) {
        Sr = NewDir(S, Wd, SysRoot->Oid, E);
        if (Sr < 0 || AddFiles(S, Sr, E->Oid, InfFiles) < 0) { Rc = Fail(S, "out of memory"); goto out; }
    }
    if (Find(S, SysRoot->Oid, "APPPATCH", TRUE, E)) {
        Sr = NewDir(S, Wd, SysRoot->Oid, E);
        if (Sr < 0 || AddFiles(S, Sr, E->Oid, AppPatchFiles) < 0) { Rc = Fail(S, "out of memory"); goto out; }
    }
out:
    ShFree(SysRoot);
    return Rc;
}

/* Every file of the view inside the window (other files moved out of the way first) */
static int
PlaceFiles(SHIM *S, BOOLEAN Initial)
{
    ULONG Need = 0, d, k;
    for (d = 0; d < S->NDirs; d++)
        for (k = 0; k < S->D[d].Count; k++)
            if (S->D[d].F[k].Dir < 0 && S->D[d].F[k].Size) Need += ClustersToMove(S, &S->D[d].F[k]);
    if (Need) {
        Need += 16 + Need / 32;
        if (Initial) {
            /* room for the files to grow (the registry), so that later updates need not move others */
            ULONG Extra = (4UL << 20) / S->B;
            if (Extra > S->Clusters / 16) Extra = S->Clusters / 16;
            Need += Extra;
        }
        if (MakeRoom(S, S->Base, S->WinEnd, Need) != 0)
            return Fail(S, "not enough free space near the start of the volume");
    }
    for (d = 0; d < S->NDirs; d++) {
        for (k = 0; k < S->D[d].Count; k++) {
            SFILE *F = &S->D[d].F[k];
            PRFS_FILE I;
            ULONG Moved = 0;
            RFS_STATUS St;
            BOOLEAN Ok;
            if (F->Dir >= 0 || F->Size == 0) continue;
            if (!RFS_SUCCESS(ShOpenFile(S, F->Parent, F->UName, F->ULen, &I))) { FailName(S, "a file of the view cannot be read", F->Name); return -1; }
            if (RfsFilePlaced(S->Vol, I, S->Base, S->WinEnd)) { RfsFileClose(I); continue; }
            if (I->Integrity) { RfsFileClose(I); FailName(S, "a file of the view is an integrity stream and cannot be moved", F->Name); return -1; }
            if (S->H->CanMove && !S->H->CanMove(S->H->Ctx, F->Home, F->FileId)) {
                RfsFileClose(I);
                FailName(S, "a file of the view is in use and cannot be moved", F->Name);
                return -1;
            }
            St = RfsFileRelocate(S->Vol, I, S->Base, S->WinEnd, TRUE, &Moved);
            Ok = (BOOLEAN)(RFS_SUCCESS(St) && RfsFilePlaced(S->Vol, I, S->Base, S->WinEnd));
            F->Size = (ULONG)I->Size;
            RfsFileClose(I);
            if (S->H->Moved) S->H->Moved(S->H->Ctx, F->Home, F->FileId);
            ShCommit(S);
            if (!Ok) {
                FailName(S, "no room to move a file near the start of the volume", F->Name);
                return -1;
            }
            S->R->MovedIn++;
        }
    }
    return 0;
}

/* Chains a file's clusters in the FAT; returns its first cluster (0: not on the grid) */
static ULONG
Chain(SHIM *S, SFILE *F)
{
    PRFS_FILE I;
    ULONG N = (ULONG)(((ULONGLONG)F->Size + S->C - 1) / S->C), i, Prev = 0, First = 0;
    if (!RFS_SUCCESS(ShOpenFile(S, F->Parent, F->UName, F->ULen, &I))) return 0;
    if (I->Inline) { RfsFileClose(I); return 0; }
    for (i = 0; i < N; i++) {
        ULONGLONG Off = (ULONGLONG)i * S->C, P, Sec;
        ULONG Run, C;
        if (!RFS_SUCCESS(RfsFileMap(S->Vol, I, Off / S->B, &P, &Run)) || !P) { First = 0; break; }
        Sec = P * S->Spb + (ULONG)(Off % S->B) / 512;
        if (Sec < S->DataSec || ((Sec - S->DataSec) % S->Spc) != 0) { First = 0; break; }
        C = (ULONG)((Sec - S->DataSec) / S->Spc) + 2;
        if (C >= S->Clusters + 2) { First = 0; break; }
        if (Prev) S->Fat[Prev] = (USHORT)C; else First = C;
        Prev = C;
    }
    RfsFileClose(I);
    if (Prev && First) S->Fat[Prev] = 0xFFFF;
    return First;
}

/* ------------------------------------------------------------------ */
/* Writing the view                                                    */
/* ------------------------------------------------------------------ */

static void
DosTime(ULONGLONG Nt, PUCHAR Time, PUCHAR Date)
{
    ULONGLONG Secs1601 = Nt / 10000000ULL;
    ULONG Unix = Secs1601 > 11644473600ULL ? (ULONG)(Secs1601 - 11644473600ULL) : 0;
    ULONG Days = Unix / 86400, Secs = Unix % 86400, Y = 1970, M, D;
    static const UCHAR Ml[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    for (;;) {
        ULONG Yd = ((Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0) ? 366 : 365;
        if (Days < Yd) break;
        Days -= Yd; Y++;
    }
    for (M = 0; M < 12; M++) {
        ULONG L = Ml[M] + (M == 1 && ((Y % 4 == 0 && Y % 100 != 0) || Y % 400 == 0));
        if (Days < L) break;
        Days -= L;
    }
    D = Days + 1;
    if (Y < 1980) { Y = 1980; M = 0; D = 1; }
    if (Y > 2107) { Y = 2107; M = 11; D = 31; }
    {
        ULONG T = ((Secs / 3600) << 11) | (((Secs / 60) % 60) << 5) | ((Secs % 60) / 2);
        ULONG Dt = ((Y - 1980) << 9) | ((M + 1) << 5) | D;
        Time[0] = (UCHAR)T; Time[1] = (UCHAR)(T >> 8);
        Date[0] = (UCHAR)Dt; Date[1] = (UCHAR)(Dt >> 8);
    }
}

static void
PutEntry(PUCHAR E, const UCHAR *Name, UCHAR Attr, UCHAR Case, ULONGLONG Mtime, ULONG Cluster, ULONG Size)
{
    ShFill(E, 0, 32);
    RfsCopy(E, Name, 11);
    E[11] = Attr;
    E[12] = Case;
    DosTime(Mtime, E + 22, E + 24);
    E[16] = E[24]; E[17] = E[25];
    E[18] = E[24]; E[19] = E[25];
    E[26] = (UCHAR)Cluster; E[27] = (UCHAR)(Cluster >> 8);
    E[28] = (UCHAR)Size; E[29] = (UCHAR)(Size >> 8); E[30] = (UCHAR)(Size >> 16); E[31] = (UCHAR)(Size >> 24);
}

/* The entries of directory d (with "." and ".." for subdirectories) */
static PUCHAR
DirImage(SHIM *S, ULONG d, PULONG Bytes)
{
    SDIR *D = &S->D[d];
    ULONG n = D->Count + (d ? 2 : 0), Size, k, Pos = 0;
    PUCHAR Img;
    static const UCHAR Dot[11] = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    static const UCHAR DotDot[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
    Size = d ? ((n * 32 + S->C - 1) / S->C) * S->C : ROOT_SECTORS * 512;
    Img = (PUCHAR)ShAlloc(Size);
    if (!Img) return NULL;
    if (d) {
        PutEntry(Img, Dot, 0x10, 0, 0, D->First, 0);
        PutEntry(Img + 32, DotDot, 0x10, 0, 0, S->D[D->Parent].First, 0);
        Pos = 64;
    }
    for (k = 0; k < D->Count; k++, Pos += 32) {
        SFILE *F = &D->F[k];
        PutEntry(Img + Pos, F->Name, F->Attr, F->Case, F->Mtime, F->Dir >= 0 ? S->D[F->Dir].First : F->First, F->Size);
    }
    *Bytes = Size;
    return Img;
}

/* The FAT, the root directory and the subdirectories */
static int
WriteView(SHIM *S)
{
    ULONG d, k, FatBytes, Bytes, Next = 0;
    ULONGLONG AreaSec = S->A * S->Spb;
    PUCHAR FatImg, Dir;
    int Rc;

    S->Fat = (USHORT *)ShAlloc((S->Clusters + 2) * sizeof(USHORT));
    if (!S->Fat) return Fail(S, "out of memory");
    for (k = 0; k < S->Clusters + 2; k++) S->Fat[k] = 0xFFF7;        /* bad: nothing may go there */
    S->Fat[0] = 0xFFF8;
    S->Fat[1] = 0xFFFF;

    /* subdirectory clusters: the boot area after the root directory */
    for (d = 1; d < S->NDirs; d++) {
        ULONG Need = ((S->D[d].Count + 2) * 32 + S->C - 1) / S->C, c;
        if (Next + Need > S->DirClusters) return Fail(S, "the boot area is too small for the directories");
        S->D[d].First = Next + 2;
        S->D[d].Clusters = Need;
        for (c = 0; c < Need; c++) {
            ULONG Cl = S->D[d].First + c;
            S->Fat[Cl] = (USHORT)(c + 1 < Need ? Cl + 1 : 0xFFFF);
        }
        Next += Need;
    }
    for (d = 0; d < S->NDirs; d++) {
        for (k = 0; k < S->D[d].Count; k++) {
            SFILE *F = &S->D[d].F[k];
            if (F->Dir >= 0 || F->Size == 0) continue;
            F->First = Chain(S, F);
            if (!F->First) { FailName(S, "a file of the view is not on the cluster grid", F->Name); return -1; }
        }
    }

    FatBytes = S->Fs * 512;
    FatImg = (PUCHAR)ShAlloc(FatBytes);
    if (!FatImg) return Fail(S, "out of memory");
    for (k = 0; k < S->Clusters + 2 && k * 2 + 1 < FatBytes; k++) {
        FatImg[k * 2] = (UCHAR)S->Fat[k];
        FatImg[k * 2 + 1] = (UCHAR)(S->Fat[k] >> 8);
    }
    Rc = ShWrite(S, AreaSec, S->Fs, FatImg);
    ShFree(FatImg);
    if (Rc != 0) return Fail(S, "write error (FAT)");
    for (d = 0; d < S->NDirs; d++) {
        if (d == 0 && S->D[0].Count > 256) return Fail(S, "too many files in the root");
        Dir = DirImage(S, d, &Bytes);
        if (!Dir) return Fail(S, "out of memory");
        if (d == 0) Rc = ShWrite(S, AreaSec + S->Fs, ROOT_SECTORS, Dir);
        else Rc = ShWrite(S, S->DataSec + (ULONGLONG)(S->D[d].First - 2) * S->Spc, Bytes / 512, Dir);
        ShFree(Dir);
        if (Rc != 0) return Fail(S, "write error (directory)");
    }
    return 0;
}

static int
WriteBootSector(SHIM *S, const UCHAR *Code, ULONG CodeSize)
{
    UCHAR B[1024], V0[512];
    ULONG Reserved = (ULONG)(S->A * S->Spb);
    ULONGLONG Total64 = S->DataSec + (ULONGLONG)S->Clusters * S->Spc;
    ULONG Total = Total64 > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (ULONG)Total64;
    ULONG Spt = S->H->SectorsPerTrack ? S->H->SectorsPerTrack : 63;
    ULONG Heads = S->H->Heads ? S->H->Heads : 255;
    ULONG Hidden = S->H->PartitionStart;
    ULONG Serial = (ULONG)S->Vol->Serial;
    if (CodeSize != 1024) return Fail(S, "the boot loader is not 1024 bytes");
    /* the ReFS boot sector moves to sector 2 (once) */
    if (ShRead(S, 0, 1, V0) != 0) return Fail(S, "read error (boot sector)");
    if (RfsProbe(V0)) {
        if (ShWrite(S, VBR_COPY, 1, V0) != 0) return Fail(S, "write error (boot sector copy)");
    } else if (!RfsShimPresent(V0)) {
        return Fail(S, "sector 0 is neither the ReFS boot sector nor REFSNT's");
    }
    RfsCopy(B, Code, 1024);
    RfsCopy(B + 3, RFS_SHIM_OEM, 8);
    B[0x0B] = 0x00; B[0x0C] = 0x02;          /* 512 bytes a sector */
    B[0x0D] = (UCHAR)S->Spc;
    B[0x0E] = (UCHAR)Reserved; B[0x0F] = (UCHAR)(Reserved >> 8);
    B[0x10] = 1;                             /* one FAT */
    B[0x11] = (UCHAR)(ROOT_SECTORS * 16); B[0x12] = (UCHAR)((ROOT_SECTORS * 16) >> 8);
    B[0x13] = 0; B[0x14] = 0;
    B[0x15] = 0xF8;
    B[0x16] = (UCHAR)S->Fs; B[0x17] = (UCHAR)(S->Fs >> 8);
    B[0x18] = (UCHAR)Spt; B[0x19] = 0;
    B[0x1A] = (UCHAR)Heads; B[0x1B] = (UCHAR)(Heads >> 8);
    B[0x1C] = (UCHAR)Hidden; B[0x1D] = (UCHAR)(Hidden >> 8);
    B[0x1E] = (UCHAR)(Hidden >> 16); B[0x1F] = (UCHAR)(Hidden >> 24);
    B[0x20] = (UCHAR)Total; B[0x21] = (UCHAR)(Total >> 8); B[0x22] = (UCHAR)(Total >> 16); B[0x23] = (UCHAR)(Total >> 24);
    B[0x24] = 0x80; B[0x25] = 0; B[0x26] = 0x29;
    B[0x27] = (UCHAR)Serial; B[0x28] = (UCHAR)(Serial >> 8); B[0x29] = (UCHAR)(Serial >> 16); B[0x2A] = (UCHAR)(Serial >> 24);
    RfsCopy(B + 0x2B, "REFSNT BOOT", 11);
    RfsCopy(B + 0x36, "FAT16   ", 8);
    ShFill(B + RFS_SHIM_WINDIR, 0, 16);
    RfsCopy(B + RFS_SHIM_WINDIR, S->R->WinDir, ShLen(S->R->WinDir));
    if (ShWrite(S, 0, 2, B) != 0) return -1;
    RfsCopy(S->Vol->BootSectors, B, 1024);
    S->Vol->BootView = TRUE;
    return 0;
}

RFS_STATUS
RfsShimUpdate(PRFS_SHIM_HOST Host, const char *WinDir, const UCHAR *Code, ULONG CodeSize, PRFS_SHIM_RESULT Result)
{
    SHIM *S;
    ULONG d, k;
    ULONGLONG SavedLo, SavedHi;
    int Rc;

    RfsZero(Result, sizeof(*Result));
    if (Host->Vol->ReadOnly) { Result->Error = "the volume is read-only"; return RFS_READ_ONLY; }
    S = (SHIM *)ShAlloc(sizeof(SHIM));
    if (!S) { Result->Error = "out of memory"; return RFS_NO_MEMORY; }
    S->H = Host;
    S->Vol = Host->Vol;
    S->R = Result;
    SavedLo = S->Vol->AvoidLo; SavedHi = S->Vol->AvoidHi;

    Rc = Geometry(S);
    if (Rc == 0) Rc = FindBootArea(S);
    if (Rc == 0) {
        /* while files move, nothing new goes into the window */
        S->Vol->AvoidLo = S->Base; S->Vol->AvoidHi = S->WinEnd;
        FindWinDir(S, WinDir);
        Rc = BuildTree(S);
    }
    if (Rc == 0) Rc = PlaceFiles(S, (BOOLEAN)(Code != 0));
    if (Rc == 0) Rc = WriteView(S);
    if (Rc == 0 && Code && WriteBootSector(S, Code, CodeSize) != 0) Rc = Fail(S, "write error (boot sector)");
    ShCommit(S);
    if (Rc == 0) {
        /* from now on the window is allocated last */
        S->Vol->AvoidLo = S->Base; S->Vol->AvoidHi = S->WinEnd;
    } else {
        S->Vol->AvoidLo = SavedLo; S->Vol->AvoidHi = SavedHi;
    }

    for (d = 0; d < S->NDirs; d++) {
        for (k = 0; k < S->D[d].Count; k++) if (S->D[d].F[k].Dir < 0) Result->Files++;
        ShFree(S->D[d].F);
    }
    Result->Dirs = S->NDirs;
    ShFree(S->Fat);
    ShFree(S);
    return Rc == 0 ? RFS_OK : RFS_CORRUPT;
}

/*
 * Takes the view away: sector 0 gets the ReFS boot sector back (from sector
 * 2) and REFSBOOT.SYS goes. Then the volume is plain ReFS again (Windows 8
 * and later read it), and no longer starts Windows NT.
 */
RFS_STATUS
RfsShimRemove(PRFS_SHIM_HOST Host, PRFS_SHIM_RESULT Result)
{
    PRFS_VOL V = Host->Vol;
    UCHAR S[1536];
    USHORT Name[16];
    ULONG n;
    RFS_STATUS St;
    RfsZero(Result, sizeof(*Result));
    if (V->ReadOnly) { Result->Error = "the volume is read-only"; return RFS_READ_ONLY; }
    St = RfsHostRead(V, 0, 1536, S);
    if (!RFS_SUCCESS(St)) { Result->Error = "read error"; return St; }
    if (!RfsShimPresent(S)) { Result->Error = "the volume is not bootable (no REFSNT boot loader)"; return RFS_NOT_FOUND; }
    if (!RfsProbe(S + 1024)) { Result->Error = "the ReFS boot sector copy (sector 2) is missing"; return RFS_CORRUPT; }
    for (n = 0; RFS_SHIM_AREA_NAME[n]; n++) Name[n] = (UCHAR)RFS_SHIM_AREA_NAME[n];
    St = RfsRemove(V, REFS_OID_ROOT, Name, n);
    if (St == RFS_NOT_FOUND) St = RFS_OK;
    if (RFS_SUCCESS(St)) St = Host->Commit ? Host->Commit(Host->Ctx) : RfsCommit(V);
    if (!RFS_SUCCESS(St)) { Result->Error = "the boot area could not be removed"; return St; }
    RfsCopy(S, S + 1024, 512);
    RfsZero(S + 512, 1024);
    St = RfsHostWrite(V, 0, 512, S);
    if (RFS_SUCCESS(St)) St = RfsHostWrite(V, 512, 1024, S + 512);       /* sectors 1-2 back to zeros */
    if (!RFS_SUCCESS(St)) { Result->Error = "write error (boot sector)"; return St; }
    V->BootView = FALSE;
    V->AvoidLo = V->AvoidHi = 0;
    return RfsHostFlush(V);
}

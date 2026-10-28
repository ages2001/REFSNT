/*
 * REFSNT - the driver layer's engine interface on ReFS (see rfx.h).
 *
 * The engine's files (RFS_FILE) are kept open in a small cache keyed by
 * inode number; the layer's inode copies are filled from them and written
 * back field by field. A changed file is saved into its directory row before
 * a commit (or when it leaves the cache).
 */
#include "rfseng.h"
#include "rfx.h"
#include "rfsshim.h"

#define V2R(V)      (&(V)->Rfs)
#define EPOCH_100NS 116444736000000000ULL      /* 1601 -> 1970 */

#define DOS_READONLY    0x01
#define DOS_HIDDEN      0x02
#define DOS_SYSTEM      0x04
#define DOS_ARCHIVE     0x20

ULONGLONG RfxHostNowNt(VOID);

/* ---- time ---------------------------------------------------------------- */
VOID
RfxSetTime(ULONG *Field, ULONG *Extra, ULONGLONG Nt100ns)
{
    LONGLONG Unix100 = (LONGLONG)Nt100ns - (LONGLONG)EPOCH_100NS;
    LONGLONG Secs = Unix100 / 10000000LL, Rem = Unix100 % 10000000LL;
    ULONG Nsec;
    if (Rem < 0) { Rem += 10000000LL; Secs--; }
    if (Secs < -0x80000000LL) Secs = -0x80000000LL;
    if (Secs > 0x37FFFFFFFLL) Secs = 0x37FFFFFFFLL;
    Nsec = (ULONG)(Rem * 100);
    *Field = (ULONG)Secs;
    if (Extra) *Extra = ((ULONG)((Secs - (LONGLONG)(LONG)(ULONG)Secs) >> 32) & 3) | (Nsec << 2);
}

ULONGLONG
RfxGetTime(ULONG Field, ULONG Extra)
{
    LONGLONG Secs = (LONGLONG)(LONG)Field;
    ULONG Nsec = Extra >> 2;
    Secs += (LONGLONG)(Extra & 3) << 32;
    if (Nsec > 999999999) Nsec = 0;
    if (Secs * 10000000LL + (LONGLONG)EPOCH_100NS < 0) return 0;
    return (ULONGLONG)((LONGLONG)EPOCH_100NS + Secs * 10000000LL + Nsec / 100);
}

VOID RfxOpBegin(PRFX_VOL Vol) { (VOID)Vol; }
RFX_STATUS RfxOpEnd(PRFX_VOL Vol, RFX_STATUS Status) { (VOID)Vol; return Status; }

#define CHANGE_BEGIN(V)     if ((V)->ReadOnly || V2R(V)->ReadOnly) return RFX_READ_ONLY

/* ---- names ----------------------------------------------------------------- */
static BOOLEAN
ToUtf16(const char *Name, ULONG Len, USHORT *Out, PULONG OutLen)
{
    ULONG n = RfsUtf8ToUtf16(Name, Len, Out, RFS_MAX_NAME);
    if (n == (ULONG)-1 || n == 0) return FALSE;
    *OutLen = n;
    return TRUE;
}

static VOID
ToEntryName(PRFX_ENTRY Entry, const USHORT *Name, ULONG Len)
{
    ULONG n = RfsUtf16ToUtf8(Name, Len, Entry->Name, RFX_MAX_NAME + 1);
    if (n > RFX_MAX_NAME) n = RFX_MAX_NAME;
    Entry->Name[n] = 0;
    Entry->NameLength = (USHORT)n;
}

/* ---- inode numbers ------------------------------------------------------- */
static ULONGLONG
DirOid(ULONG Ino)
{
    return Ino == RFX_ROOT_INO ? REFS_OID_ROOT : (ULONGLONG)Ino;
}

static ULONG
DirIno(ULONGLONG Oid)
{
    if (Oid == REFS_OID_ROOT) return RFX_ROOT_INO;
    if (Oid >= RFX_VIRTUAL_INO || Oid == RFX_ROOT_INO) return 0;     /* (never seen: object ids grow from 0x701) */
    return (ULONG)Oid;
}

#define VINO(V, i)  (&(V)->VinoChunk[(i) >> RFX_VINO_SHIFT][(i) & (RFX_VINO_CHUNK - 1)])

static ULONG
HashOf(PRFX_VOL V, ULONGLONG Home, ULONGLONG Id)
{
    ULONGLONG h = (Home * 0x9E3779B97F4A7C15ULL) ^ (Id * 0xC2B2AE3D27D4EB4FULL);
    return (ULONG)(h >> 33) & (V->VinoHashSize - 1);
}

static BOOLEAN
VinoRehash(PRFX_VOL V, ULONG Size)
{
    PULONG n = (PULONG)RfxHostAlloc(Size * sizeof(ULONG));
    ULONG i;
    if (!n) return FALSE;
    RfsZero(n, Size * sizeof(ULONG));
    if (V->VinoHash) RfxHostFree(V->VinoHash);
    V->VinoHash = n;
    V->VinoHashSize = Size;
    for (i = 0; i < V->VinoCount; i++) {
        ULONG h;
        if (VINO(V, i)->Gone) continue;
        h = HashOf(V, VINO(V, i)->Home, VINO(V, i)->FileId);
        while (n[h]) h = (h + 1) & (Size - 1);
        n[h] = i + 1;
    }
    return TRUE;
}

static ULONG
VinoFind(PRFX_VOL V, ULONGLONG Home, ULONGLONG Id)
{
    ULONG h;
    if (!V->VinoHashSize) return 0;
    h = HashOf(V, Home, Id);
    while (V->VinoHash[h]) {
        RFX_VINO *e = VINO(V, V->VinoHash[h] - 1);
        if (!e->Gone && e->Home == Home && e->FileId == Id) return V->VinoHash[h];
        h = (h + 1) & (V->VinoHashSize - 1);
    }
    return 0;
}

static BOOLEAN
VinoSetName(RFX_VINO *E, const USHORT *Name, ULONG Len)
{
    USHORT *n = (USHORT *)RfxHostAlloc((Len ? Len : 1) * sizeof(USHORT));
    if (!n) return FALSE;
    RfsCopy(n, Name, Len * sizeof(USHORT));
    if (E->Name) RfxHostFree(E->Name);
    E->Name = n;
    E->NameLen = (USHORT)Len;
    return TRUE;
}

/* the inode number of a directory entry (a file's number is made the first time it is seen) */
static ULONG
EntIno(PRFX_VOL V, ULONGLONG Dir, const RFS_DIRENT *E)
{
    ULONG i;
    if (E->Type == RFS_FT_DIR) return DirIno(E->Oid);
    i = VinoFind(V, E->Home, E->FileId);
    if (i) {
        RFX_VINO *e = VINO(V, i - 1);
        /* (a hard link seen under another name keeps its first one) */
        if (e->Parent == Dir || !e->NameLen) { e->Parent = Dir; VinoSetName(e, E->Name, E->NameLen); e->KeyFlags = E->KeyFlags; }
        return RFX_VIRTUAL_INO | (i - 1);
    }
    if (V->VinoCount == V->VinoCap) {
        RFX_VINO *c;
        if (V->VinoChunks == V->VinoChunkCap) {
            ULONG cap = V->VinoChunkCap ? V->VinoChunkCap * 2 : 16;
            RFX_VINO **n = (RFX_VINO **)RfxHostAlloc(cap * sizeof(RFX_VINO *));
            if (!n) return 0;
            if (V->VinoChunk) { RfsCopy(n, V->VinoChunk, V->VinoChunks * sizeof(RFX_VINO *)); RfxHostFree(V->VinoChunk); }
            V->VinoChunk = n;
            V->VinoChunkCap = cap;
        }
        c = (RFX_VINO *)RfxHostAlloc(RFX_VINO_CHUNK * sizeof(RFX_VINO));
        if (!c) return 0;
        V->VinoChunk[V->VinoChunks++] = c;
        V->VinoCap += RFX_VINO_CHUNK;
    }
    if ((V->VinoCount + 1) * 2 > V->VinoHashSize && !VinoRehash(V, V->VinoHashSize ? V->VinoHashSize * 2 : 1024)) return 0;
    i = V->VinoCount;
    RfsZero(VINO(V, i), sizeof(RFX_VINO));
    VINO(V, i)->Parent = Dir;
    VINO(V, i)->Home = E->Home;
    VINO(V, i)->FileId = E->FileId;
    VINO(V, i)->KeyFlags = E->KeyFlags;
    if (!VinoSetName(VINO(V, i), E->Name, E->NameLen)) return 0;
    V->VinoCount++;
    {
        ULONG h = HashOf(V, E->Home, E->FileId);
        while (V->VinoHash[h]) h = (h + 1) & (V->VinoHashSize - 1);
        V->VinoHash[h] = i + 1;
    }
    return RFX_VIRTUAL_INO | i;
}

static RFX_VINO *
VinoOf(PRFX_VOL V, ULONG Ino)
{
    ULONG i = Ino & ~RFX_VIRTUAL_INO;
    if (!(Ino & RFX_VIRTUAL_INO) || i >= V->VinoCount || VINO(V, i)->Gone) return NULL;
    return VINO(V, i);
}

/* a file's identity changed (moved to another directory): its number follows it */
static VOID
VinoMoved(PRFX_VOL V, ULONG Ino, ULONGLONG Parent, ULONGLONG Home, ULONGLONG FileId, const USHORT *Name, ULONG Len)
{
    RFX_VINO *e = VinoOf(V, Ino);
    if (!e) return;
    e->Parent = Parent;
    e->Home = Home;
    e->FileId = FileId;
    VinoSetName(e, Name, Len);
    VinoRehash(V, V->VinoHashSize);
}

/* ---- the file cache ------------------------------------------------------ */
static VOID
LruUnlink(PRFX_VOL V, PRFX_NODE N)
{
    if (N->Prev2) N->Prev2->Next2 = N->Next2; else V->LruHead = N->Next2;
    if (N->Next2) N->Next2->Prev2 = N->Prev2; else V->LruTail = N->Prev2;
    N->Prev2 = N->Next2 = NULL;
}

static VOID
LruFront(PRFX_VOL V, PRFX_NODE N)
{
    N->Next2 = V->LruHead;
    N->Prev2 = NULL;
    if (V->LruHead) V->LruHead->Prev2 = N; else V->LruTail = N;
    V->LruHead = N;
}

static RFS_STATUS
NodeSave(PRFX_VOL V, PRFX_NODE N)
{
    PRFS_FILE F = N->F;
    RFS_STATUS st = RFS_OK;
    if (!N->Dirty) return RFS_OK;
    if (V2R(V)->ReadOnly) { N->Dirty = FALSE; return RFS_OK; }
    if (F->Type == RFS_FT_FILE && !F->Inline) {
        /* the disk always has the clusters the size needs (zeroed) */
        ULONGLONG need = (F->Size + V->BlockSize - 1) >> V->BlockShift;
        if ((F->Alloc >> V->BlockShift) < need) st = RfsFileAllocTo(V2R(V), F, need, 0, 0);
        F->ValidSize = F->Size;
    }
    F->Changed = TRUE;
    if (RFS_SUCCESS(st)) st = RfsFileSave(V2R(V), F);
    if (RFS_SUCCESS(st)) N->Dirty = FALSE;
    return st;
}

static VOID
NodeDrop(PRFX_VOL V, PRFX_NODE N)
{
    PRFX_NODE *pp = &V->NodeHash[N->Ino % RFX_NODE_HASH];
    while (*pp && *pp != N) pp = &(*pp)->Next;
    if (*pp) *pp = N->Next;
    LruUnlink(V, N);
    RfsFileClose(N->F);
    RfxHostFree(N);
    V->NodeCount--;
}

/* the cached file of Ino, loading it */
static RFS_STATUS
Get(PRFX_VOL V, ULONG Ino, PRFX_NODE *Out)
{
    PRFX_NODE N;
    PRFS_FILE F = NULL;
    RFS_STATUS st;
    for (N = V->NodeHash[Ino % RFX_NODE_HASH]; N; N = N->Next) {
        if (N->Ino == Ino) {
            LruUnlink(V, N);
            LruFront(V, N);
            *Out = N;
            return RFS_OK;
        }
    }
    if (Ino & RFX_VIRTUAL_INO) {
        RFX_VINO *e = VinoOf(V, Ino);
        RFS_DIRENT *d;
        if (!e) return RFX_NOT_FOUND;
        d = (RFS_DIRENT *)RfxHostAlloc(sizeof(RFS_DIRENT));
        if (!d) return RFX_NO_MEMORY;
        RfsZero(d, sizeof(*d));
        d->Type = RFS_FT_FILE;
        d->KeyFlags = e->KeyFlags;
        d->Home = e->Home;
        d->FileId = e->FileId;
        RfsCopy(d->Name, e->Name, e->NameLen * sizeof(USHORT));
        d->NameLen = e->NameLen;
        st = RfsFileOpen(V2R(V), e->Parent, d, &F);
        RfxHostFree(d);
    } else {
        ULONGLONG oid = DirOid(Ino), parent;
        st = RfsFileOpenDir(V2R(V), oid, &F);
        if (RFS_SUCCESS(st) && oid != REFS_OID_ROOT &&
            RFS_SUCCESS(RfsDirParent(V2R(V), oid, &parent, F->Name, &F->NameLen))) {
            F->Parent = parent;
            F->KeyFlags = REFS_NAME_INDEX;
        }
    }
    if (!RFS_SUCCESS(st)) return st;
    /* room: clean files go, oldest first */
    while (V->NodeCount >= RFX_NODE_MAX) {
        PRFX_NODE old = V->LruTail;
        while (old && old->Dirty) old = old->Prev2;
        if (!old) break;
        NodeDrop(V, old);
    }
    N = (PRFX_NODE)RfxHostAlloc(sizeof(RFX_NODE));
    if (!N) { RfsFileClose(F); return RFX_NO_MEMORY; }
    RfsZero(N, sizeof(*N));
    N->Ino = Ino;
    N->F = F;
    N->Next = V->NodeHash[Ino % RFX_NODE_HASH];
    V->NodeHash[Ino % RFX_NODE_HASH] = N;
    LruFront(V, N);
    V->NodeCount++;
    *Out = N;
    return RFS_OK;
}

/* forgets a file's cached copy (after a change made around it) */
static VOID
Forget(PRFX_VOL V, ULONG Ino)
{
    PRFX_NODE N;
    for (N = V->NodeHash[Ino % RFX_NODE_HASH]; N; N = N->Next) {
        if (N->Ino == Ino) {
            if (N->Dirty) NodeSave(V, N);
            NodeDrop(V, N);
            return;
        }
    }
}

/* saves every changed file */
static RFS_STATUS
SaveAll(PRFX_VOL V)
{
    PRFX_NODE N;
    RFS_STATUS st = RFS_OK;
    for (N = V->LruHead; N; N = N->Next2) {
        if (N->Dirty) {
            RFS_STATUS s2 = NodeSave(V, N);
            if (!RFS_SUCCESS(s2) && RFS_SUCCESS(st)) st = s2;
        }
    }
    return st;
}

static BOOLEAN
AnyDirty(PRFX_VOL V)
{
    PRFX_NODE N;
    for (N = V->LruHead; N; N = N->Next2) if (N->Dirty) return TRUE;
    return FALSE;
}

/* the number of a file the driver has seen (0: none) */
ULONG
RfxInoOfFile(PRFX_VOL Vol, ULONGLONG Home, ULONGLONG FileId)
{
    ULONG i = VinoFind(Vol, Home, FileId);
    return i ? (RFX_VIRTUAL_INO | (i - 1)) : 0;
}

/* its cached copy is let go (its data moved under it) */
VOID
RfxForget(PRFX_VOL Vol, ULONG Ino)
{
    Forget(Vol, Ino);
}

ULONGLONG
RfxInodeNumber(PRFX_VOL Vol, ULONG Ino)
{
    RFX_VINO *e = VinoOf(Vol, Ino);
    if (!(Ino & RFX_VIRTUAL_INO)) return DirOid(Ino);
    if (!e) return Ino;
    return (e->Home << 32) | (e->FileId & 0xFFFFFFFFULL);
}

/* ---- volume -------------------------------------------------------------- */
BOOLEAN
RfxIsRefs(const UCHAR *S0)
{
    /* a ReFS boot sector, or REFSNT's boot loader (the ReFS one then in sector 2: the mount checks it) */
    return (BOOLEAN)(RfsProbe(S0) || RfsShimPresent(S0));
}

VOID
RfxCountFree(PRFX_VOL Vol)
{
    PRFS_VOL R = V2R(Vol);
    if (R->V1) {
        ULONG cpb = R->ClusterSize / REFS1_PAGE;             /* 16 KiB blocks a cluster */
        /* in 16 KiB blocks (the volume's block size here): TotalClusters
         * already counts blocks on 1.x, the free count whole clusters */
        Vol->DataBlocks = R->TotalClusters;
        Vol->FreeBlocksCount = RfsFreeCount(R) * cpb;
        if (Vol->FreeBlocksCount > Vol->DataBlocks) Vol->FreeBlocksCount = Vol->DataBlocks;
        return;
    }
    Vol->DataBlocks = R->TotalClusters;
    Vol->FreeBlocksCount = R->ReadOnly && !R->FreeClusters ? 0 : RfsFreeCount(R);
    if (!Vol->FreeBlocksCount && R->ReadOnly) {
        /* read-only: the Medium allocator's summary */
        PRFS_TABLE t;
        if (RFS_SUCCESS(RfsTableOpenRoot(R, REFS_ROOT_MEDIUM, &t))) {
            if (t->Root->Desc && t->Root->DescLen >= REFS_IR_DATA + 0x18) {
                Vol->DataBlocks = RfsGet64(t->Root->Desc + REFS_IR_DATA + RFS_SUM_COVERED(R));
                Vol->FreeBlocksCount = RfsGet64(t->Root->Desc + REFS_IR_DATA + RFS_SUM_FREE(R));
            }
            RfsTableClose(R, t);
        }
    } else if (!R->ReadOnly) {
        PRFS_TABLE t;
        if (RFS_SUCCESS(RfsGlobal(R, REFS_ROOT_MEDIUM, &t)) && t->Root->Desc && t->Root->DescLen >= REFS_IR_DATA + 0x18)
            Vol->DataBlocks = RfsGet64(t->Root->Desc + REFS_IR_DATA + RFS_SUM_COVERED(R));
    }
    if (Vol->FreeBlocksCount > Vol->DataBlocks) Vol->FreeBlocksCount = Vol->DataBlocks;
}

RFX_STATUS
RfxMount(PRFX_VOL Vol, BOOLEAN ReadOnly)
{
    PRFS_VOL R = V2R(Vol);
    RFS_STATUS st;
    ULONG i;
    RfsZero(R, sizeof(*R));
    R->HostContext = Vol->HostContext;
    R->SectorSize = Vol->DeviceSector;
    R->DeviceBytes = Vol->DeviceBytes;
    R->PageMax = R->ClusterSize > 16384 ? 24 : 64;      /* (set again below, once the page size is known) */
    R->TableMax = 48;
    st = RfsMount(R, ReadOnly);
    if (!RFS_SUCCESS(st)) return st;
    Vol->Mounted = TRUE;
    R->PageMax = R->PageSize > 16384 ? 24 : 64;            /* 1.5 MB of pages either way */
    if (R->V1) {
        /* ReFS 1.x addresses files in 16 KiB blocks (clusters are 64 KiB) */
        ULONG sec = Vol->DeviceSector ? Vol->DeviceSector : 512;
        Vol->BlockSize = REFS1_PAGE;
        Vol->BlockShift = REFS1_BLOCK_SHIFT;
        Vol->SectorsPerBlock = REFS1_PAGE / sec;
        Vol->BlocksCount = R->DeviceBytes >> REFS1_BLOCK_SHIFT;
        Vol->NodeSize = REFS1_PAGE;
    } else {
        Vol->BlockSize = R->ClusterSize;
        Vol->BlockShift = R->ClusterShift;
        Vol->SectorsPerBlock = R->ClusterSize / (Vol->DeviceSector ? Vol->DeviceSector : 512);
        Vol->BlocksCount = R->TotalClusters;
        Vol->NodeSize = R->PageSize;
    }
    Vol->InodeSize = 256;
    Vol->GroupCount = R->NConts;
    RfsZero(Vol->VolumeName, sizeof(Vol->VolumeName));
    for (i = 0; i < 255 && R->Label[i]; i++) Vol->VolumeName[i] = R->Label[i];
    RfsCopy(Vol->Uuid, R->Guid, 16);
    Vol->SerialNumber = (ULONG)R->Serial;
    Vol->ReadOnly = R->ReadOnly;
    Vol->ReadOnlyForced = (BOOLEAN)(R->ReadOnly && !ReadOnly);
    Vol->DataCopies = FALSE;
    Vol->Dirty = FALSE;
    RfxCountFree(Vol);
    return RFX_OK;
}

VOID
RfxUnmountFree(PRFX_VOL Vol)
{
    ULONG i;
    while (Vol->LruHead) NodeDrop(Vol, Vol->LruHead);
    if (Vol->Mounted) RfsUnmount(V2R(Vol));
    Vol->Mounted = FALSE;
    for (i = 0; i < Vol->VinoCount; i++) if (VINO(Vol, i)->Name) RfxHostFree(VINO(Vol, i)->Name);
    for (i = 0; i < Vol->VinoChunks; i++) RfxHostFree(Vol->VinoChunk[i]);
    if (Vol->VinoChunk) RfxHostFree(Vol->VinoChunk);
    if (Vol->VinoHash) RfxHostFree(Vol->VinoHash);
    Vol->VinoChunk = NULL;
    Vol->VinoHash = NULL;
    Vol->VinoCount = Vol->VinoCap = Vol->VinoHashSize = Vol->VinoChunks = Vol->VinoChunkCap = 0;
    RfsZero(Vol->Enum, sizeof(Vol->Enum));
}

/* the volume on the disk is (still) this one: the same serial number and checkpoint slots */
BOOLEAN
RfxSameVolume(PRFX_VOL Vol)
{
    PRFS_VOL R = V2R(Vol);
    UCHAR *b;
    BOOLEAN same = TRUE;
    if (!Vol->Mounted) return TRUE;
    b = (UCHAR *)RfxHostAlloc(1536);
    if (!b) return TRUE;
    if (RFS_SUCCESS(RfsHostRead(R, 0, 1536, b))) {
        /* made bootable: the ReFS boot sector is in sector 2 */
        UCHAR *v = RfsShimPresent(b) ? b + 1024 : b;
        same = (BOOLEAN)(RfsProbe(v) && RfsGet64(v + REFS_VBR_SERIAL) == R->Serial);
    }
    RfxHostFree(b);
    return same;
}

RFX_STATUS RfxStartWriting(PRFX_VOL Vol) { return Vol->ReadOnly ? RFX_READ_ONLY : RFX_OK; }

RFX_STATUS
RfxTxnCommit(PRFX_VOL Vol)
{
    RFS_STATUS st;
    if (Vol->ReadOnly) return RFX_OK;
    st = SaveAll(Vol);
    if (RFS_SUCCESS(st)) st = RfsCommit(V2R(Vol));
    if (!RFS_SUCCESS(st)) Vol->ReadOnly = TRUE;
    RfxCountFree(Vol);
    return st;
}

RFX_STATUS RfxStopWriting(PRFX_VOL Vol) { return RfxTxnCommit(Vol); }
BOOLEAN    RfxTxnPending(PRFX_VOL Vol) { return (BOOLEAN)(!Vol->ReadOnly && (V2R(Vol)->Dirty || AnyDirty(Vol))); }
ULONG      RfxDirtyBytes(PRFX_VOL Vol) { return V2R(Vol)->NQuar * 4096; }

RFX_STATUS
RfxSetLabel(PRFX_VOL Vol, const char *Label)
{
    RFS_STATUS st;
    CHANGE_BEGIN(Vol);
    st = RfsSetLabel(V2R(Vol), Label);
    if (RFS_SUCCESS(st)) {
        ULONG n = 0;
        while (Label[n] && n < 255) n++;
        RfsZero(Vol->VolumeName, 256);
        RfsCopy(Vol->VolumeName, Label, n);
    }
    return st;
}

RFX_STATUS RfxProcessOrphans(PRFX_VOL Vol) { (VOID)Vol; return RFX_OK; }

/* ---- inodes -------------------------------------------------------------- */
static VOID
Fill(PRFX_VOL V, ULONG Ino, PRFS_FILE F, PRFX_INODE X)
{
    ULONGLONG blocks512 = F->Alloc >> 9;
    RfsZero(X, sizeof(*X));
    if (F->Type == RFS_FT_DIR) {
        X->Mode = RFX_S_IFDIR | 0755;
        X->LinksCount = 2;
    } else {
        X->Mode = RFX_S_IFREG | 0644;
        X->LinksCount = (USHORT)(F->LinkCount ? F->LinkCount : 1);
        X->SizeLo = (ULONG)F->Size;
        X->SizeHi = (ULONG)(F->Size >> 32);
        if (F->Inline) X->Flags |= RFX_IF_INLINE_DATA;
        if (F->Integrity) X->Flags |= RFX_IF_IMMUTABLE;     /* integrity streams are not written here */
    }
    if (F->Attributes & DOS_READONLY) X->Mode &= (USHORT)~0222;
    RfxSetTime(&X->Atime, &X->AtimeExtra, F->Times.Access ? F->Times.Access : EPOCH_100NS);
    RfxSetTime(&X->Mtime, &X->MtimeExtra, F->Times.Write ? F->Times.Write : EPOCH_100NS);
    RfxSetTime(&X->Ctime, &X->CtimeExtra, F->Times.Change ? F->Times.Change : EPOCH_100NS);
    RfxSetTime(&X->Crtime, &X->CrtimeExtra, F->Times.Create ? F->Times.Create : EPOCH_100NS);
    X->BlocksLo = (ULONG)blocks512;
    X->BlocksHi = (USHORT)(blocks512 >> 32);
    X->ExtraIsize = 32;
    X->UniqueId = RfxInodeNumber(V, Ino);
    X->Generation = (ULONG)X->UniqueId;
}

RFX_STATUS
RfxReadInode(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode)
{
    PRFX_NODE N;
    RFS_STATUS st = Get(Vol, Ino, &N);
    if (!RFS_SUCCESS(st)) return st;
    Fill(Vol, Ino, N->F, Inode);
    return RFX_OK;
}

/* the fields the driver owns: the read-only bit, size (files), times */
RFX_STATUS
RfxWriteInode(PRFX_VOL Vol, ULONG Ino, PRFX_INODE X)
{
    PRFX_NODE N;
    PRFS_FILE F;
    RFS_STATUS st;
    CHANGE_BEGIN(Vol);
    st = Get(Vol, Ino, &N);
    if (!RFS_SUCCESS(st)) return st;
    F = N->F;
    F->Times.Access = RfxGetTime(X->Atime, X->AtimeExtra);
    F->Times.Write = RfxGetTime(X->Mtime, X->MtimeExtra);
    F->Times.Change = RfxGetTime(X->Ctime, X->CtimeExtra);
    if (X->Crtime) F->Times.Create = RfxGetTime(X->Crtime, X->CrtimeExtra);
    if (X->Mode & 0200) F->Attributes &= ~DOS_READONLY; else F->Attributes |= DOS_READONLY;
    if (F->Type == RFS_FT_FILE) {
        ULONGLONG size = X->SizeLo | ((ULONGLONG)X->SizeHi << 32);
        if (size != F->Size) {
            if (F->Inline) {
                st = RfsFileSetSize(V2R(Vol), F, size, TRUE);      /* (moves the data into clusters) */
                if (!RFS_SUCCESS(st)) return st;
            }
            F->Size = size;
            if (F->ValidSize > size) F->ValidSize = size;
        }
    }
    N->Dirty = TRUE;
    return RFX_OK;
}

ULONGLONG RfxInodeSize(PRFX_VOL Vol, PRFX_INODE I) { (VOID)Vol; return I->SizeLo | ((ULONGLONG)I->SizeHi << 32); }
VOID      RfxInodeSetSize(PRFX_VOL Vol, PRFX_INODE I, ULONGLONG Size) { (VOID)Vol; I->SizeLo = (ULONG)Size; I->SizeHi = (ULONG)(Size >> 32); }
ULONGLONG RfxInodeBlocks(PRFX_VOL Vol, PRFX_INODE I) { (VOID)Vol; return I->BlocksLo | ((ULONGLONG)I->BlocksHi << 32); }

/* the engine-kept fields of a copy the caller holds, after a change */
static VOID
Refresh(PRFX_VOL V, ULONG Ino, PRFX_INODE X)
{
    RFX_INODE N;
    if (!X || !RFS_SUCCESS(RfxReadInode(V, Ino, &N))) return;
    X->LinksCount = N.LinksCount;
    X->BlocksLo = N.BlocksLo;
    X->BlocksHi = N.BlocksHi;
    X->Flags = (X->Flags & ~RFX_IF_INLINE_DATA) | (N.Flags & RFX_IF_INLINE_DATA);
    if ((N.Mode & RFX_S_IFMT) == RFX_S_IFDIR) {
        X->Mtime = N.Mtime; X->MtimeExtra = N.MtimeExtra;
        X->Ctime = N.Ctime; X->CtimeExtra = N.CtimeExtra;
    }
}

/* ---- directories --------------------------------------------------------- */
static VOID
FromEnt(PRFX_VOL V, ULONGLONG Dir, const RFS_DIRENT *E, PRFX_ENTRY Entry)
{
    Entry->Inode = EntIno(V, Dir, E);
    Entry->FileType = E->Type == RFS_FT_DIR ? RFX_FT_DIR : RFX_FT_REG_FILE;
    ToEntryName(Entry, E->Name, E->NameLen);
}

/* a directory entry plus two UTF-16 name buffers in one heap block (kept off
   the 12K NT 3.x kernel stack) */
#define RFX_ESCR            (sizeof(RFS_DIRENT) + 2 * (RFS_MAX_NAME + 1) * sizeof(USHORT) + 8)
#define RFX_ESCR_N(e, i)    ((USHORT *)((PUCHAR)(e) + ((sizeof(RFS_DIRENT) + 7) & ~7UL)) + (i) * (RFS_MAX_NAME + 1))

RFX_STATUS
RfxLookup(PRFX_VOL Vol, ULONG DirInoNum, PRFX_INODE Dir, const char *Name, ULONG NameLen, PRFX_ENTRY Entry)
{
    RFS_DIRENT *e;
    USHORT *n;
    ULONG len;
    RFS_STATUS st;
    (VOID)Dir;
    if (DirInoNum & RFX_VIRTUAL_INO) return RFX_NOT_A_DIRECTORY;
    e = (RFS_DIRENT *)RfxHostAlloc(RFX_ESCR);
    if (!e) return RFX_NO_MEMORY;
    n = RFX_ESCR_N(e, 0);
    if (!ToUtf16(Name, NameLen, n, &len)) { RfxHostFree(e); return RFX_NOT_FOUND; }
    st = RfsDirLookup(V2R(Vol), DirOid(DirInoNum), n, len, e);
    if (RFS_SUCCESS(st)) {
        FromEnt(Vol, DirOid(DirInoNum), e, Entry);
        Entry->Cookie = 0;
        if (!Entry->Inode) st = RFX_NO_MEMORY;
    }
    RfxHostFree(e);
    return st;
}

/*
 * Enumeration: the cookie counts the entries given so far; where the last
 * enumerations stopped is remembered by name, so going on costs one search.
 */
RFX_STATUS
RfxEnum(PRFX_VOL Vol, ULONG DirInoNum, PRFX_INODE Dir, ULONGLONG Cookie, PRFX_ENTRY Entry)
{
    RFS_DIRENT *e;
    RFS_STATUS st;
    ULONGLONG oid = DirOid(DirInoNum), k;
    RFX_ENUMPOS *pos = NULL;
    ULONG i;
    (VOID)Dir;
    if (DirInoNum & RFX_VIRTUAL_INO) return RFX_NOT_A_DIRECTORY;
    e = (RFS_DIRENT *)RfxHostAlloc(RFX_ESCR);
    if (!e) return RFX_NO_MEMORY;
    if (Cookie == 0) {
        /* a new enumeration: what was remembered about this directory may be
         * from before it changed (a file added in front of the place an old
         * slot names would make the next call skip an entry) */
        for (i = 0; i < RFX_ENUM_SLOTS; i++)
            if (Vol->Enum[i].DirIno == DirInoNum) Vol->Enum[i].DirIno = 0;
    }
    for (i = 0; Cookie && i < RFX_ENUM_SLOTS; i++) {
        if (Vol->Enum[i].DirIno == DirInoNum && Vol->Enum[i].Cookie == Cookie) { pos = &Vol->Enum[i]; break; }
    }
    if (pos) {
        st = RfsDirAfter(V2R(Vol), oid, pos->Name, pos->NameLen, e);
    } else {
        /* from the start, Cookie entries on */
        st = RfsDirAfter(V2R(Vol), oid, NULL, 0, e);
        USHORT *n = RFX_ESCR_N(e, 0);
        for (k = 0; RFS_SUCCESS(st) && k < Cookie; k++) {
            ULONG l = e->NameLen;
            RfsCopy(n, e->Name, l * sizeof(USHORT));
            st = RfsDirAfter(V2R(Vol), oid, n, l, e);
        }
    }
    if (st == RFS_NO_MORE || st == RFS_NOT_FOUND) { RfxHostFree(e); return RFX_NO_MORE_ENTRIES; }
    if (!RFS_SUCCESS(st)) { RfxHostFree(e); return st; }
    FromEnt(Vol, oid, e, Entry);
    Entry->Cookie = Cookie + 1;
    if (!Entry->Inode) st = RFX_NO_MEMORY;
    if (pos) {
        /* the slot moves on; another one may still name the next place */
        for (i = 0; i < RFX_ENUM_SLOTS; i++)
            if (&Vol->Enum[i] != pos && Vol->Enum[i].DirIno == DirInoNum && Vol->Enum[i].Cookie == Cookie + 1) Vol->Enum[i].DirIno = 0;
    } else {
        for (i = 0; i < RFX_ENUM_SLOTS; i++)
            if (Vol->Enum[i].DirIno == DirInoNum && Vol->Enum[i].Cookie == Cookie + 1) { pos = &Vol->Enum[i]; break; }
        if (!pos) {
            pos = &Vol->Enum[Vol->EnumNext];
            Vol->EnumNext = (Vol->EnumNext + 1) % RFX_ENUM_SLOTS;
        }
    }
    pos->DirIno = DirInoNum;
    pos->Cookie = Cookie + 1;
    pos->NameLen = (USHORT)e->NameLen;
    RfsCopy(pos->Name, e->Name, e->NameLen * sizeof(USHORT));
    RfxHostFree(e);
    return st;
}

RFX_STATUS
RfxDirIsEmpty(PRFX_VOL Vol, ULONG DirInoNum, PRFX_INODE Dir, PBOOLEAN Empty)
{
    RFS_DIRENT *e;
    RFS_CURSOR c;
    RFS_STATUS st;
    (VOID)Dir;
    e = (RFS_DIRENT *)RfxHostAlloc(sizeof(RFS_DIRENT));
    if (!e) return RFX_NO_MEMORY;
    st = RfsDirNext(V2R(Vol), DirOid(DirInoNum), &c, TRUE, e);
    if (RFS_SUCCESS(st)) { RfsCursorClose(&c); *Empty = FALSE; }
    else if (st == RFS_NO_MORE) { *Empty = TRUE; st = RFX_OK; }
    RfxHostFree(e);
    return st;
}

RFX_STATUS
RfxGetParent(PRFX_VOL Vol, ULONG DirInoNum, PRFX_INODE Dir, PULONG Parent)
{
    ULONGLONG p;
    RFS_STATUS st;
    (VOID)Dir;
    if (DirInoNum == RFX_ROOT_INO) { *Parent = RFX_ROOT_INO; return RFX_OK; }
    st = RfsDirParent(V2R(Vol), DirOid(DirInoNum), &p, NULL, NULL);
    if (!RFS_SUCCESS(st)) return st;
    *Parent = DirIno(p);
    return *Parent ? RFX_OK : RFX_NOT_FOUND;
}

RFX_STATUS
RfxCreate(PRFX_VOL Vol, ULONG DirInoNum, PRFX_INODE Dir, const char *Name, ULONG NameLen,
          USHORT Mode, const char *Target, PULONG NewIno, PRFX_INODE NewInode)
{
    RFS_DIRENT *e;
    USHORT *n;
    ULONG len;
    RFS_STATUS st;
    BOOLEAN dir;
    (VOID)Target;
    CHANGE_BEGIN(Vol);
    switch (Mode & RFX_S_IFMT) {
    case RFX_S_IFDIR: dir = TRUE; break;
    case RFX_S_IFREG: dir = FALSE; break;
    default: return RFX_NOT_SUPPORTED;
    }
    e = (RFS_DIRENT *)RfxHostAlloc(RFX_ESCR);
    if (!e) return RFX_NO_MEMORY;
    n = RFX_ESCR_N(e, 0);
    if (!ToUtf16(Name, NameLen, n, &len)) { RfxHostFree(e); return RFX_BAD_NAME; }
    st = RfsCreate(V2R(Vol), DirOid(DirInoNum), n, len, dir, (Mode & 0200) ? (dir ? 0x10 : 0x20) : (dir ? 0x11 : 0x21), e);
    if (RFS_SUCCESS(st)) {
        *NewIno = EntIno(Vol, DirOid(DirInoNum), e);
        if (!*NewIno) st = RFX_NO_MEMORY;
        else if (NewInode) st = RfxReadInode(Vol, *NewIno, NewInode);
    }
    RfxHostFree(e);
    if (RFS_SUCCESS(st)) { Forget(Vol, DirInoNum); Refresh(Vol, DirInoNum, Dir); }
    return st;
}

static RFX_STATUS
DoUnlink(PRFX_VOL Vol, ULONG DirInoNum, PRFX_INODE Dir, const char *Name, ULONG NameLen)
{
    RFS_DIRENT *e;
    USHORT *n;
    ULONG len, ino = 0;
    RFS_STATUS st;
    CHANGE_BEGIN(Vol);
    e = (RFS_DIRENT *)RfxHostAlloc(RFX_ESCR);
    if (!e) return RFX_NO_MEMORY;
    n = RFX_ESCR_N(e, 0);
    if (!ToUtf16(Name, NameLen, n, &len)) { RfxHostFree(e); return RFX_NOT_FOUND; }
    /* the cached copy goes first (unsaved changes do not matter any more) */
    if (RFS_SUCCESS(RfsDirLookup(V2R(Vol), DirOid(DirInoNum), n, len, e))) {
        ino = e->Type == RFS_FT_DIR ? DirIno(e->Oid) : VinoFind(Vol, e->Home, e->FileId);
        if (e->Type != RFS_FT_DIR && ino) ino = RFX_VIRTUAL_INO | (ino - 1);
    }
    if (ino) {
        PRFX_NODE N;
        for (N = Vol->NodeHash[ino % RFX_NODE_HASH]; N; N = N->Next) {
            if (N->Ino == ino) { NodeDrop(Vol, N); break; }
        }
    }
    st = RfsRemove(V2R(Vol), DirOid(DirInoNum), n, len);
    if (RFS_SUCCESS(st) && ino & RFX_VIRTUAL_INO) {
        RFX_VINO *v = VinoOf(Vol, ino);
        if (v && e->Type == RFS_FT_FILE && (e->KeyFlags == REFS_NAME_EMBEDDED || v->Parent == DirOid(DirInoNum))) {
            v->Gone = TRUE;
            VinoRehash(Vol, Vol->VinoHashSize);
        }
    }
    RfxHostFree(e);
    if (RFS_SUCCESS(st)) { Forget(Vol, DirInoNum); Refresh(Vol, DirInoNum, Dir); }
    return st;
}

RFX_STATUS RfxUnlink(PRFX_VOL Vol, ULONG DirInoNum, PRFX_INODE Dir, const char *Name, ULONG NameLen)
{ return DoUnlink(Vol, DirInoNum, Dir, Name, NameLen); }

RFX_STATUS
RfxRemoveEntry(PRFX_VOL Vol, ULONG DirInoNum, PRFX_INODE Dir, const char *Name, ULONG NameLen)
{
    (VOID)Vol; (VOID)DirInoNum; (VOID)Dir; (VOID)Name; (VOID)NameLen;
    return RFX_NOT_SUPPORTED;               /* (hard links are not made here) */
}

RFX_STATUS
RfxLink(PRFX_VOL Vol, ULONG DirInoNum, PRFX_INODE Dir, const char *Name, ULONG NameLen, ULONG TargetIno, PRFX_INODE Target)
{
    (VOID)Vol; (VOID)DirInoNum; (VOID)Dir; (VOID)Name; (VOID)NameLen; (VOID)TargetIno; (VOID)Target;
    return RFX_NOT_SUPPORTED;
}

RFX_STATUS
RfxRename(PRFX_VOL Vol, ULONG OldDir, PRFX_INODE OldDirInode, const char *OldName, ULONG OldLen,
          ULONG NewDir, PRFX_INODE NewDirInode, const char *NewName, ULONG NewLen)
{
    RFS_DIRENT *e;
    USHORT *on, *nn;
    ULONG ol, nl, ino = 0;
    RFS_STATUS st;
    CHANGE_BEGIN(Vol);
    e = (RFS_DIRENT *)RfxHostAlloc(RFX_ESCR);
    if (!e) return RFX_NO_MEMORY;
    on = RFX_ESCR_N(e, 0);
    nn = RFX_ESCR_N(e, 1);
    if (!ToUtf16(OldName, OldLen, on, &ol)) { RfxHostFree(e); return RFX_NOT_FOUND; }
    if (!ToUtf16(NewName, NewLen, nn, &nl)) { RfxHostFree(e); return RFX_BAD_NAME; }
    st = RfsDirLookup(V2R(Vol), DirOid(OldDir), on, ol, e);
    if (!RFS_SUCCESS(st)) { RfxHostFree(e); return st; }
    if (e->Type == RFS_FT_DIR) ino = DirIno(e->Oid);
    else {
        ino = VinoFind(Vol, e->Home, e->FileId);
        ino = ino ? (RFX_VIRTUAL_INO | (ino - 1)) : 0;
    }
    /* its cached copy is saved under the old name, then forgotten */
    if (ino) Forget(Vol, ino);
    st = RfsRename(V2R(Vol), DirOid(OldDir), on, ol, DirOid(NewDir), nn, nl, FALSE);
    if (RFS_SUCCESS(st) && (ino & RFX_VIRTUAL_INO)) {
        RFS_DIRENT *n2 = (RFS_DIRENT *)RfxHostAlloc(sizeof(RFS_DIRENT));
        if (n2 && RFS_SUCCESS(RfsDirLookup(V2R(Vol), DirOid(NewDir), nn, nl, n2)))
            VinoMoved(Vol, ino, DirOid(NewDir), n2->Home, n2->FileId, nn, nl);
        if (n2) RfxHostFree(n2);
    }
    RfxHostFree(e);
    if (RFS_SUCCESS(st)) {
        Forget(Vol, OldDir);
        Refresh(Vol, OldDir, OldDirInode);
        if (NewDir != OldDir) { Forget(Vol, NewDir); Refresh(Vol, NewDir, NewDirInode); }
    }
    return st;
}

RFX_STATUS
RfxReadLink(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, char *Buf, ULONG BufSize, PULONG Len)
{
    (VOID)Vol; (VOID)Ino; (VOID)Inode; (VOID)Buf; (VOID)BufSize; (VOID)Len;
    return RFX_NOT_SUPPORTED;
}

/* ---- file data ----------------------------------------------------------- */
RFX_STATUS
RfxReadFile(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, ULONGLONG Offset, ULONG Length, PVOID Buffer, PULONG Read)
{
    PRFX_NODE N;
    RFS_STATUS st = Get(Vol, Ino, &N);
    (VOID)Inode;
    if (!RFS_SUCCESS(st)) return st;
    return RfsFileRead(V2R(Vol), N->F, Offset, Length, Buffer, Read);
}

RFX_STATUS
RfxMapFileBlock(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, ULONGLONG FileBlk, PULONGLONG Disk, PULONG RunLen)
{
    PRFX_NODE N;
    ULONG run;
    RFS_STATUS st = Get(Vol, Ino, &N);
    (VOID)Inode;
    if (!RFS_SUCCESS(st)) return st;
    if (N->F->Inline) return RFX_NOT_SUPPORTED;
    st = RfsFileMap(V2R(Vol), N->F, FileBlk, Disk, &run);
    if (!RFS_SUCCESS(st)) return st;
    if (run > 0xFFFFFFF) run = 0xFFFFFFF;
    *RunLen = run ? run : 1;
    return RFX_OK;
}

RFX_STATUS
RfxAllocateRange(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, ULONGLONG FirstBlk, ULONGLONG Count)
{
    PRFX_NODE N;
    RFS_STATUS st;
    CHANGE_BEGIN(Vol);
    st = Get(Vol, Ino, &N);
    if (!RFS_SUCCESS(st)) return st;
    /* blocks larger than a page: the cache writes them a page at a time, the rest must be zeros */
    st = RfsFileAllocTo(V2R(Vol), N->F, FirstBlk + Count, FirstBlk, Vol->BlockSize > 4096 ? 0 : Count);
    if (RFS_SUCCESS(st)) { N->Dirty = TRUE; Refresh(Vol, Ino, Inode); }
    return st;
}

RFX_STATUS
RfxGetFileBlock(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, ULONGLONG FileBlk, PULONGLONG Disk, PBOOLEAN Fresh)
{
    ULONG Run;
    RFX_STATUS st = RfxMapFileBlock(Vol, Ino, Inode, FileBlk, Disk, &Run);
    if (!RFS_SUCCESS(st)) return st;
    *Fresh = FALSE;
    if (*Disk) return RFX_OK;
    st = RfxAllocateRange(Vol, Ino, Inode, FileBlk, 1);
    if (!RFS_SUCCESS(st)) return st;
    *Fresh = TRUE;
    st = RfxMapFileBlock(Vol, Ino, Inode, FileBlk, Disk, &Run);
    if (RFS_SUCCESS(st) && *Disk == 0) st = RFX_CORRUPT;
    return st;
}

RFX_STATUS
RfxUninline(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode)
{
    PRFX_NODE N;
    RFS_STATUS st;
    CHANGE_BEGIN(Vol);
    st = Get(Vol, Ino, &N);
    if (!RFS_SUCCESS(st)) return st;
    if (N->F->Inline) {
        st = RfsFileSetSize(V2R(Vol), N->F, N->F->Size, TRUE);
        if (RFS_SUCCESS(st)) st = RfsFileSave(V2R(Vol), N->F);
    }
    if (RFS_SUCCESS(st)) Refresh(Vol, Ino, Inode);
    return st;
}

RFX_STATUS
RfxTruncateBlocks(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, ULONGLONG NewBlocks)
{
    PRFX_NODE N;
    RFS_STATUS st;
    CHANGE_BEGIN(Vol);
    st = Get(Vol, Ino, &N);
    if (!RFS_SUCCESS(st)) return st;
    st = RfsFileTruncTo(V2R(Vol), N->F, NewBlocks);
    if (RFS_SUCCESS(st)) {
        if (N->F->Size > (NewBlocks << Vol->BlockShift)) N->F->Size = NewBlocks << Vol->BlockShift;
        N->Dirty = TRUE;
        Refresh(Vol, Ino, Inode);
    }
    return st;
}

ULONG
RfxDataMirrors(PRFX_VOL Vol, ULONGLONG DiskBlock, PULONGLONG Other, ULONG Max)
{
    (VOID)Vol; (VOID)DiskBlock; (VOID)Other; (VOID)Max;
    return 0;
}

/* ---- extended attributes: user.DOSATTRIB only (hidden, system, archive) ---- */
static BOOLEAN
IsDosAttrib(UCHAR Index, const char *Name, ULONG Len)
{
    return (BOOLEAN)(Index == RFX_XATTR_INDEX_USER && Len == 9 && !RfsCmp(Name, "DOSATTRIB", 9));
}

RFX_STATUS
RfxGetXattr(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, UCHAR NameIndex, const char *Name,
            ULONG NameLen, PUCHAR Value, ULONG ValueSize, PULONG ValueLen)
{
    PRFX_NODE N;
    RFS_STATUS st;
    ULONG a;
    static const char hex[] = "0123456789abcdef";
    (VOID)Inode;
    if (!IsDosAttrib(NameIndex, Name, NameLen)) return RFX_NO_SUCH_EA;
    st = Get(Vol, Ino, &N);
    if (!RFS_SUCCESS(st)) return st;
    a = N->F->Attributes & (DOS_HIDDEN | DOS_SYSTEM | DOS_ARCHIVE);
    if (!a) return RFX_NO_SUCH_EA;
    *ValueLen = 4;
    if (ValueSize < 4) return RFX_BUFFER_TOO_SMALL;
    Value[0] = '0'; Value[1] = 'x';
    Value[2] = (UCHAR)hex[(a >> 4) & 15];
    Value[3] = (UCHAR)hex[a & 15];
    return RFX_OK;
}

RFX_STATUS
RfxSetXattr(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode, UCHAR NameIndex, const char *Name,
            ULONG NameLen, const UCHAR *Value, ULONG ValueLen)
{
    PRFX_NODE N;
    RFS_STATUS st;
    ULONG a = 0, i;
    (VOID)Inode;
    if (!IsDosAttrib(NameIndex, Name, NameLen)) return RFX_NOT_SUPPORTED;
    if (Value && ValueLen >= 3 && Value[0] == '0' && (Value[1] == 'x' || Value[1] == 'X')) {
        for (i = 2; i < ValueLen; i++) {
            UCHAR c = Value[i];
            ULONG d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            a = (a << 4) | d;
        }
    }
    CHANGE_BEGIN(Vol);
    st = Get(Vol, Ino, &N);
    if (!RFS_SUCCESS(st)) return st;
    N->F->Attributes = (N->F->Attributes & ~(ULONG)(DOS_HIDDEN | DOS_SYSTEM | DOS_ARCHIVE)) |
                       (a & (DOS_HIDDEN | DOS_SYSTEM | DOS_ARCHIVE));
    N->Dirty = TRUE;
    return RFX_OK;
}

RFX_STATUS
RfxListXattr(PRFX_VOL Vol, ULONG Ino, PRFX_INODE Inode,
             VOID (*Report)(PVOID Ctx, UCHAR NameIndex, const char *Name, ULONG NameLen,
                            const UCHAR *Value, ULONG ValueLen), PVOID Ctx)
{
    (VOID)Vol; (VOID)Ino; (VOID)Inode; (VOID)Report; (VOID)Ctx;
    return RFX_OK;
}

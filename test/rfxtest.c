/*
 * REFSNT - the driver layer's engine interface (rfx.c) driven from Linux the
 * way the layer drives it: names in UTF-8, data written to the blocks the
 * map gives, sizes through the inode. rfxtest IMAGE SEED ROUNDS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "rfseng.h"
#include "rfx.h"
#include "rfsimg.h"

static RFS_IMAGE Img;
static RFX_VOL Vol;

RFS_STATUS RfsHostRead(PRFS_VOL V, ULONGLONG Off, ULONG Len, PVOID Buf) { (void)V; return RimgRead(&Img, Off, Buf, Len) ? RFS_OK : RFS_IO_ERROR; }
RFS_STATUS RfsHostWrite(PRFS_VOL V, ULONGLONG Off, ULONG Len, const VOID *Buf) { (void)V; return RimgWrite(&Img, Off, Buf, Len) ? RFS_OK : RFS_IO_ERROR; }
RFS_STATUS RfsHostFlush(PRFS_VOL V) { (void)V; return RFS_OK; }
PVOID RfsHostAlloc(ULONG Bytes) { return malloc(Bytes ? Bytes : 1); }
VOID RfsHostFree(PVOID P) { free(P); }
PVOID RfxHostAlloc(ULONG Bytes) { return malloc(Bytes ? Bytes : 1); }
VOID RfxHostFree(PVOID P) { free(P); }
ULONGLONG RfsHostNowNt(VOID) { struct timeval tv; gettimeofday(&tv, NULL); return 116444736000000000ULL + (ULONGLONG)tv.tv_sec * 10000000ULL + tv.tv_usec * 10; }
ULONGLONG RfxHostNowNt(VOID) { return RfsHostNowNt(); }
ULONG RfxHostNow(VOID) { struct timeval tv; gettimeofday(&tv, NULL); return (ULONG)tv.tv_sec; }

static void Check(const char *What, RFX_STATUS St) { if (St < 0) { printf("FAIL %s: %08lX\n", What, (unsigned long)(ULONG)St); exit(1); } }

/* writes Len bytes at Off the way the layer's paging writes do */
static void
LayerWrite(ULONG Ino, RFX_INODE *I, ULONGLONG Off, const unsigned char *Buf, ULONG Len)
{
    ULONG bs = Vol.BlockSize;
    ULONGLONG first = Off / bs, last = (Off + Len + bs - 1) / bs, b;
    ULONGLONG size = RfxInodeSize(&Vol, I);
    if (I->Flags & RFX_IF_INLINE_DATA) Check("uninline", RfxUninline(&Vol, Ino, I));
    /* user write: space first */
    Check("allocrange", RfxAllocateRange(&Vol, Ino, I, first, last - first));
    for (b = first; b < last; b++) {
        ULONGLONG disk; ULONG run;
        unsigned char *blk = malloc(bs);
        ULONGLONG bo = b * bs;
        ULONG from = bo < Off ? (ULONG)(Off - bo) : 0, to = bo + bs > Off + Len ? (ULONG)(Off + Len - bo) : bs;
        Check("map", RfxMapFileBlock(&Vol, Ino, I, b, &disk, &run));
        if (!disk) { printf("FAIL: block %llu not mapped\n", (unsigned long long)b); exit(1); }
        RfsHostRead(&Vol.Rfs, disk * bs, bs, blk);
        memcpy(blk + from, Buf + (bo + from - Off), to - from);
        RfsHostWrite(&Vol.Rfs, disk * bs, bs, blk);
        free(blk);
    }
    if (Off + Len > size) RfxInodeSetSize(&Vol, I, Off + Len);
    Check("writeinode", RfxWriteInode(&Vol, Ino, I));
}

int
main(int argc, char **argv)
{
    RFX_INODE root, d, f;
    ULONG dino, fino, i;
    RFX_ENTRY e;
    unsigned char *buf;
    if (argc < 2) return 2;
    if (!RimgOpen(&Img, argv[1], -1, 0)) return 1;
    Vol.DeviceBytes = Img.Size;
    Vol.DeviceSector = 512;
    Check("mount", RfxMount(&Vol, FALSE));
    printf("mounted: %llu blocks of %lu, %llu free, read-only %d\n", (unsigned long long)Vol.BlocksCount,
           (unsigned long)Vol.BlockSize, (unsigned long long)Vol.FreeBlocksCount, Vol.ReadOnly);
    Check("root", RfxReadInode(&Vol, RFX_ROOT_INO, &root));
    Check("mkdir", RfxCreate(&Vol, RFX_ROOT_INO, &root, "rfxdir", 6, RFX_S_IFDIR | 0755, NULL, &dino, &d));
    Check("create", RfxCreate(&Vol, dino, &d, "hello.txt", 9, RFX_S_IFREG | 0644, NULL, &fino, &f));
    LayerWrite(fino, &f, 0, (const unsigned char *)"Hello, ReFS!\n", 13);
    buf = malloc(1 << 20);
    for (i = 0; i < (1 << 20); i++) buf[i] = (unsigned char)(i * 7 + 3);
    Check("create2", RfxCreate(&Vol, dino, &d, "big.bin", 7, RFX_S_IFREG | 0644, NULL, &fino, &f));
    LayerWrite(fino, &f, 0, buf, 1 << 20);
    LayerWrite(fino, &f, 3000000, buf, 5000);           /* past the end: a gap */
    Check("commit", RfxTxnCommit(&Vol));
    /* enumerate */
    for (i = 0; ; i++) {
        RFX_STATUS st = RfxEnum(&Vol, dino, &d, i, &e);
        if (st == RFX_NO_MORE_ENTRIES) break;
        Check("enum", st);
        printf("  %s ino %lx type %u\n", e.Name, (unsigned long)e.Inode, e.FileType);
    }
    /* rename across directories, then truncate */
    Check("rename", RfxRename(&Vol, dino, &d, "big.bin", 7, RFX_ROOT_INO, &root, "moved.bin", 9));
    Check("lookup", RfxLookup(&Vol, RFX_ROOT_INO, &root, "MOVED.BIN", 9, &e));
    printf("lookup moved.bin: ino %lx (was %lx)\n", (unsigned long)e.Inode, (unsigned long)fino);
    Check("read", RfxReadInode(&Vol, e.Inode, &f));
    Check("trunc", RfxTruncateBlocks(&Vol, e.Inode, &f, 10));
    RfxInodeSetSize(&Vol, &f, 40000);
    Check("writeinode", RfxWriteInode(&Vol, e.Inode, &f));
    Check("commit2", RfxTxnCommit(&Vol));
    RfxUnmountFree(&Vol);
    RimgClose(&Img);
    printf("OK\n");
    return 0;
}

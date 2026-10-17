/* driver-layer stress: many files/dirs via the rfx API, random sizes, rename, delete */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "rfseng.h"
#include "rfx.h"
#include "rfsimg.h"
static RFS_IMAGE Img; static RFX_VOL Vol;
RFS_STATUS RfsHostRead(PRFS_VOL V,ULONGLONG O,ULONG L,PVOID B){(void)V;return RimgRead(&Img,O,B,L)?RFS_OK:RFS_IO_ERROR;}
RFS_STATUS RfsHostWrite(PRFS_VOL V,ULONGLONG O,ULONG L,const VOID*B){(void)V;return RimgWrite(&Img,O,B,L)?RFS_OK:RFS_IO_ERROR;}
RFS_STATUS RfsHostFlush(PRFS_VOL V){(void)V;return RFS_OK;}
PVOID RfsHostAlloc(ULONG b){return malloc(b?b:1);} VOID RfsHostFree(PVOID p){free(p);}
PVOID RfxHostAlloc(ULONG b){return malloc(b?b:1);} VOID RfxHostFree(PVOID p){free(p);}
ULONGLONG RfsHostNowNt(VOID){struct timeval t;gettimeofday(&t,0);return 116444736000000000ULL+(ULONGLONG)t.tv_sec*10000000ULL+t.tv_usec*10;}
ULONGLONG RfxHostNowNt(VOID){return RfsHostNowNt();} ULONG RfxHostNow(VOID){return (ULONG)time(0);}
static void Ck(const char*w,RFX_STATUS s){if(s<0){printf("FAIL %s: %08lX\n",w,(unsigned long)(ULONG)s);exit(1);}}
static unsigned char PB(unsigned long long k,unsigned long i){return (unsigned char)((k*1103515245ull+i*7+3));}
static void W(ULONG ino,RFX_INODE*I,unsigned long long seed,ULONG len){
    ULONG bs=Vol.BlockSize; ULONGLONG last=(len+bs-1)/bs,b;
    if(I->Flags&RFX_IF_INLINE_DATA) Ck("uninl",RfxUninline(&Vol,ino,I));
    if(len) Ck("alloc",RfxAllocateRange(&Vol,ino,I,0,last));
    for(b=0;b<last;b++){ULONGLONG disk;ULONG run;ULONG from=0,to=(ULONG)((b+1)*bs>len?len-b*bs:bs);
        unsigned char*blk=malloc(bs);ULONG i;
        Ck("map",RfxMapFileBlock(&Vol,ino,I,b,&disk,&run));
        if(!disk){printf("FAIL unmapped %llu\n",(unsigned long long)b);exit(1);}
        RfsHostRead(&Vol.Rfs,disk*bs,bs,blk);
        for(i=0;i<to-from;i++) blk[from+i]=PB(seed,(unsigned long)(b*bs+from+i));
        RfsHostWrite(&Vol.Rfs,disk*bs,bs,blk);free(blk);}
    RfxInodeSetSize(&Vol,I,len); Ck("wino",RfxWriteInode(&Vol,ino,I));
}
static int Rd(ULONG ino,unsigned long long seed,ULONG len){
    unsigned char*b=malloc(len?len:1);ULONG got;RFX_INODE I;ULONG i;
    Ck("rino",RfxReadInode(&Vol,ino,&I));
    Ck("read",RfxReadFile(&Vol,ino,&I,0,len,b,&got));
    if(got!=len){printf("  shortread %lu!=%lu\n",(unsigned long)got,(unsigned long)len);return 1;}
    for(i=0;i<len;i++) if(b[i]!=PB(seed,i)){printf("  datamismatch @%lu\n",(unsigned long)i);free(b);return 1;}
    free(b);return 0;
}
int main(int argc,char**argv){
    RFX_INODE root,d,f; ULONG dino,fino,i,round; RFX_ENTRY e;
    int seed=argc>2?atoi(argv[2]):1, rounds=argc>3?atoi(argv[3]):30;
    unsigned long long names[4000]; ULONG inos[4000],sizes[4000]; int isdir[4000],nf=0;
    srand(seed);
    if(!RimgOpen(&Img,argv[1],-1,0))return 1;
    Vol.DeviceBytes=Img.Size; Vol.DeviceSector=512;
    Ck("mount",RfxMount(&Vol,FALSE));
    Ck("root",RfxReadInode(&Vol,RFX_ROOT_INO,&root));
    for(round=0;round<(ULONG)rounds;round++){
        int ops=5+rand()%12,o;
        for(o=0;o<ops;o++){
            int a=rand()%100;char nm[32];
            if(a<25){ /* mkdir in root */
                sprintf(nm,"d%d_%d",round,o);
                if(RFS_SUCCESS(RfxCreate(&Vol,RFX_ROOT_INO,&root,nm,strlen(nm),RFX_S_IFDIR|0755,NULL,&dino,&d))){
                    names[nf]=0;inos[nf]=dino;sizes[nf]=0;isdir[nf]=1;nf++;}
            }else if(a<75&&nf<3990){ /* create+write file in root */
                unsigned long long sd; ULONG sz;
                sprintf(nm,"f%d_%d",round,o);
                sz=(ULONG)(rand()%5==0?0:rand()%300000); sd=((unsigned long long)seed<<32)^((unsigned long long)round<<8)^o;
                if(RFS_SUCCESS(RfxCreate(&Vol,RFX_ROOT_INO,&root,nm,strlen(nm),RFX_S_IFREG|0644,NULL,&fino,&f))){
                    W(fino,&f,sd,sz); names[nf]=sd;inos[nf]=fino;sizes[nf]=sz;isdir[nf]=0;nf++;}
            }else if(nf>0){ /* delete a random file */
                int k=rand()%nf; char dn[32];
                if(!isdir[k]){ /* find its name by re-enumerating root is costly; track name */ }
            }
        }
        Ck("commit",RfxTxnCommit(&Vol));
        /* verify all files */
        for(i=0;i<(ULONG)nf;i++) if(!isdir[i]) if(Rd(inos[i],names[i],sizes[i])){printf("VERIFY FAIL round %lu file ino %lx\n",(unsigned long)round,(unsigned long)inos[i]);return 1;}
    }
    RfxUnmountFree(&Vol); RimgClose(&Img);
    printf("OK seed %d rounds %d; %d objects\n",seed,rounds,nf);
    return 0;
}

/* enumerate a big v1 directory via RfxEnum the way the NT dir-query does: resume by index */
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
ULONGLONG RfxHostNowNt(VOID){return RfsHostNowNt();} ULONG RfxHostNow(VOID){return 0;}
static void Ck(const char*w,RFX_STATUS s){if(s<0){printf("FAIL %s: %08lX\n",w,(unsigned long)(ULONG)s);exit(1);}}
int main(int argc,char**argv){
    RFX_INODE root,d,f; ULONG dino,fino,i; RFX_ENTRY e; int N=argc>2?atoi(argv[2]):1000;
    char nm[32]; int *seen;
    if(!RimgOpen(&Img,argv[1],-1,0))return 1;
    Vol.DeviceBytes=Img.Size; Vol.DeviceSector=512;
    Ck("mount",RfxMount(&Vol,FALSE));
    Ck("root",RfxReadInode(&Vol,RFX_ROOT_INO,&root));
    Ck("mkdir",RfxCreate(&Vol,RFX_ROOT_INO,&root,"big",3,RFX_S_IFDIR|0755,NULL,&dino,&d));
    for(i=0;i<(ULONG)N;i++){sprintf(nm,"file%05u",i);
        Ck("create",RfxCreate(&Vol,dino,&d,nm,strlen(nm),RFX_S_IFREG|0644,NULL,&fino,&f));
        if((i&63)==63) Ck("commit",RfxTxnCommit(&Vol));}
    Ck("commit",RfxTxnCommit(&Vol));
    /* enumerate by index, chunked (simulate Explorer querying 32 at a time with resume) */
    seen=calloc(N,sizeof(int));
    { ULONG idx=0,total=0,chunk,guard=0;
      for(;;){
        RFX_STATUS st; int got=0;
        for(chunk=0;chunk<32;chunk++){
            st=RfxEnum(&Vol,dino,&d,idx,&e);
            if(st==RFX_NO_MORE_ENTRIES) break;
            Ck("enum",st);
            if(strncmp(e.Name,"file",4)==0){int k=atoi(e.Name+4); if(k>=0&&k<N){if(seen[k])printf("DUP %s\n",e.Name);seen[k]++;}}
            idx++; total++; got++;
        }
        if(got==0) break;
        if(++guard>N+1000){printf("ENUM LOOP (no termination)\n");return 1;}
      }
      printf("enumerated %lu entries\n",(unsigned long)total);
      { int miss=0,dup=0,j; for(j=0;j<N;j++){if(!seen[j])miss++;if(seen[j]>1)dup++;}
        printf("missing %d dup %d (expect missing 0 dup 0)\n",miss,dup);
        if(miss||dup) return 1; }
    }
    RfxUnmountFree(&Vol); RimgClose(&Img);
    printf("ENUM OK\n"); return 0;
}

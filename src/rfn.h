/*
 * REFSNT - ReFS (Resilient File System) driver for Windows NT 3.1, 3.5,
 * 3.51, 4.0, 2000, XP and XP x64 / Server 2003 x64.
 *
 *   RFN_CROSS  GCC cross build: one i386 binary for NT 3.1 - XP
 *
 * The on-disk work is done by the shared engine (rfssup.c, rfsvol.c,
 * rfstree.c, rfsfile.c, rfswrite.c, rfsalloc.c, rfscommit.c, rfsops.c),
 * reached through rfx.c. File data is cached by the Cache Manager in each
 * file's own section and goes to the disk non-cached, mapped through the
 * file's extents (rfx.h explains how ReFS files are made fit for that).
 */

#ifndef _RFN_H_
#define _RFN_H_

#ifndef RFS_KERNEL
#define RFS_KERNEL
#endif

#ifdef RFN_CROSS
#ifndef RFN_NT31
#define RFN_NT31
#endif
#ifndef RFN_NT4
#define RFN_NT4
#endif
#endif /* RFN_CROSS */

/*
 * GCC has no __try/__finally: the handlers' cleanup blocks become plain code
 * reached by goto (__leave). Exceptions are caught with RfnTryCall.
 */
#ifdef __GNUC__
#define __try
#define __finally               __exn_leave: __attribute__((unused));
#define __except(x)             if (0)
#define __leave                 goto __exn_leave
#ifndef AbnormalTermination
#define AbnormalTermination()   0
#endif
#ifndef GetExceptionInformation
#define GetExceptionInformation() ((PEXCEPTION_POINTERS)0)
#endif
#ifndef GetExceptionCode
#define GetExceptionCode()      0
#endif
#ifndef EXCEPTION_EXECUTE_HANDLER
#define EXCEPTION_EXECUTE_HANDLER 1
#endif
#ifndef EXCEPTION_CONTINUE_SEARCH
#define EXCEPTION_CONTINUE_SEARCH 0
#endif
#endif /* __GNUC__ */

#include <ntifs.h>
#include <ntdddisk.h>

#include "rfx.h"
#include "rfsshim.h"
#include "rfnctl.h"

/* ------------------------------------------------------------------ */
/* NT 3.1 import profile                                               */
/* ------------------------------------------------------------------ */

/*
 * Windows NT 3.1 does not export the Lite executive-resource functions,
 * MmCanFileBeTruncated, or the FsRtlNotifyFull / NotifySync change-
 * notification family (all introduced in NT 3.5/4.0). Mapping the Lite
 * calls to their originals is exact; truncation always allowed is the safe
 * choice where the memory manager cannot answer; change notification is an
 * optional feature and is left out on NT 3.1. One binary then loads on NT
 * 3.1 through 4.0. (NT 3.51 and later export the Lite set, so a build
 * without RFN_NT31 uses it directly.)
 */
#ifdef RFN_NT31

/*
 * The mingw DDK aliases the legacy resource names to their Lite forms (the
 * only ones modern ntoskrnl exports). Undo that and import the executive-
 * resource functions that are common to EVERY NT the one binary must load
 * on - 3.1, 3.51 and 4.0 - resolving the Lite names to them.
 *
 * The catch is the shared acquire: NT 3.1 exports ExAcquireResourceShared
 * but not the Lite form, while NT 3.51+ export ExAcquireResourceSharedLite
 * but DROPPED ExAcquireResourceShared. There is no shared-acquire name
 * common to all three, and an import the running kernel cannot resolve makes
 * the OS loader reject the driver as "missing or corrupt" (this is exactly
 * why the single binary failed to load on NT 3.51). The exclusive acquire,
 * by contrast, is exported under the same name (ExAcquireResourceExclusive)
 * by every one of them. So the one binary acquires exclusively wherever the
 * code asks for shared: exclusive is always a safe substitute for shared
 * (it is strictly more restrictive, the lock hierarchy stays acyclic, so no
 * new deadlock is possible), at the only cost of serialising concurrent
 * readers - immaterial on the uniprocessor retro systems this targets.
 * (A Visual C++ build pinned to a single NT version can instead import that
 * version's real shared acquire and keep true reader concurrency.)
 */
#undef ExAcquireResourceExclusive
#undef ExAcquireResourceShared
#undef ExReleaseResourceForThread
#undef ExInitializeResource
#undef ExDeleteResource
#undef ExAcquireResourceExclusiveLite
#undef ExAcquireResourceSharedLite
#undef ExReleaseResourceForThreadLite
#undef ExInitializeResourceLite
#undef ExDeleteResourceLite

/*
 * Resolved at DriverEntry from the running ntoskrnl (see exnres.c): the Lite
 * form on NT 3.51+, the legacy form on NT 3.1. The code calls the Lite names;
 * they expand to calls through these pointers. (Init/Delete return NTSTATUS in
 * the Lite ABI and VOID in the legacy one; the pointer type is NTSTATUS and
 * the legacy return is simply ignored - harmless on stdcall x86.)
 */
typedef NTSTATUS (NTAPI *RFN_PINITRES)(PERESOURCE);
typedef BOOLEAN  (NTAPI *RFN_PACQRES)(PERESOURCE, BOOLEAN);
typedef VOID     (NTAPI *RFN_PRELRES)(PERESOURCE, ERESOURCE_THREAD);
typedef NTSTATUS (NTAPI *RFN_PDELRES)(PERESOURCE);

extern RFN_PINITRES ExnpInitRes;
extern RFN_PACQRES  ExnpAcqExcl;
extern RFN_PACQRES  ExnpAcqShared;
extern RFN_PDELRES  ExnpDelRes;
extern RFN_PRELRES  ExnpRelForThread;

BOOLEAN RfnResolveResourceApi(VOID);

#define ExInitializeResourceLite(r)          ExnpInitRes(r)
#define ExAcquireResourceExclusiveLite(r,w)  ExnpAcqExcl((r),(w))
#define ExAcquireResourceSharedLite(r,w)     ExnpAcqShared((r),(w))
#define ExDeleteResourceLite(r)              ExnpDelRes(r)
#define ExReleaseResourceForThreadLite(r,t)  ExnpRelForThread((r),(t))

/*
 * NT 4.0 introduced the fast-call Iof/Obf forms; NT 3.1 has only the
 * stdcall originals. The DDK maps the plain names to the fast-call ones,
 * so undo that and declare the stdcall exports NT 3.1 actually has.
 */
#undef IoCallDriver
#undef IoCompleteRequest
#undef ObDereferenceObject
__declspec(dllimport) NTSTATUS NTAPI IoCallDriver(PDEVICE_OBJECT, PIRP);
__declspec(dllimport) VOID     NTAPI IoCompleteRequest(PIRP, CCHAR);
__declspec(dllimport) VOID     NTAPI ObDereferenceObject(PVOID);

#define MmCanFileBeTruncated(S, N)      TRUE
#define FsRtlNotifyInitializeSync(S)    ((void)0)
#define FsRtlNotifyUninitializeSync(S)  ((void)0)
#define FsRtlNotifyCleanup(a, b, c)     ((void)0)
#define FsRtlNotifyFullChangeDirectory(a,b,c,d,e,f,g,h,i,j)  ((void)0)
#define FsRtlNotifyFullReportChange(a,b,c,d,e,f,g,h,i)       ((void)0)
#endif /* RFN_NT31 */

#ifdef __GNUC__
/*
 * The mingw DDK IO_STACK_LOCATION already carries the file-system members,
 * so the RFN_NT4 extended-stack cast is a plain identity there.
 */
typedef IO_STACK_LOCATION EXTENDED_IO_STACK_LOCATION, *PEXTENDED_IO_STACK_LOCATION;

/* The DDK headers redefine these SEH intrinsics; re-assert the shim so no
   _abnormal_termination / exception-record references are emitted. */
#undef AbnormalTermination
#define AbnormalTermination()       0
#undef GetExceptionCode
#define GetExceptionCode()          0
#undef GetExceptionInformation
#define GetExceptionInformation()   ((PEXCEPTION_POINTERS)0)
#endif

#ifdef RFN_CROSS
/*
 * NT 3.1 does not export ExAllocatePoolWithTag, the Ke critical-region
 * calls, or the top-level-IRP / verify / requestor helpers (newer builds
 * access them inline through the thread). The universal binary drops the
 * pool tag, makes the critical region a no-op pair, and keeps the top-level
 * IRP in one process-wide slot. A Visual C++ NT 3.1 build would instead use
 * the real inline thread-field accessors.
 */
#define ExAllocatePoolWithTag(Type, Size, Tag)  ExAllocatePool((Type), (Size))
#define KeEnterCriticalRegion()                 ((void)0)
#define KeLeaveCriticalRegion()                 ((void)0)
extern PIRP RfnXTopLevelIrp;
#define IoGetTopLevelIrp()          (RfnXTopLevelIrp)
#define IoSetTopLevelIrp(I)         (RfnXTopLevelIrp = (PIRP)(I))
/* (cleanup runs in the process closing the handle: its locks go) */
#define IoGetRequestorProcess(Irp)  IoGetCurrentProcess()
#define IoGetDeviceToVerify(T)      ((PDEVICE_OBJECT)0)
#define IoSetDeviceToVerify(T, D)   ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Target configuration                                                */
/* ------------------------------------------------------------------ */

/*
 * The NT4 DDK IO_STACK_LOCATION lacks the file system members
 * (QueryDirectory, NotifyDirectory, FileSystemControl, LockControl);
 * the free ntifs.h provides them through EXTENDED_IO_STACK_LOCATION.
 */
#ifdef RFN_NT4
#define RfnXSp(IrpSp)           ((PEXTENDED_IO_STACK_LOCATION)(IrpSp))
#define RfnMdlAddress(Mdl)      MmGetSystemAddressForMdl(Mdl)
#define RFN_STATUS_DISMOUNTED   STATUS_FILE_INVALID
#else
#define RfnXSp(IrpSp)           (IrpSp)
#define RfnMdlAddress(Mdl)      MmGetSystemAddressForMdlSafe((Mdl), NormalPagePriority)
#define RFN_STATUS_DISMOUNTED   STATUS_VOLUME_DISMOUNTED
#endif

/* Same as the W2K IoSkipCurrentIrpStackLocation, which NT4 lacks */
#define RfnSkipStack(Irp) {                         \
    (Irp)->CurrentLocation++;                       \
    (Irp)->Tail.Overlay.CurrentStackLocation++;     \
}

#define RfnCopyStackToNext(Irp) {                                           \
    PIO_STACK_LOCATION _Sp = IoGetCurrentIrpStackLocation(Irp);            \
    PIO_STACK_LOCATION _Next = IoGetNextIrpStackLocation(Irp);             \
    RtlCopyMemory(_Next, _Sp, FIELD_OFFSET(IO_STACK_LOCATION, CompletionRoutine)); \
    _Next->Control = 0;                                                     \
}

/* ExReleaseResourceLite is not exported by every NT4 build */
#define RfnRelease(Resource) \
    ExReleaseResourceForThreadLite((Resource), ExGetCurrentResourceThread())

#ifndef VPB_LOCKED
#define VPB_LOCKED                  0x00000002
#endif

/* File system IRP details some DDK headers leave out */
#ifndef IRP_MN_QUERY_DIRECTORY
#define IRP_MN_QUERY_DIRECTORY          0x01
#endif
#ifndef IRP_MN_NOTIFY_CHANGE_DIRECTORY
#define IRP_MN_NOTIFY_CHANGE_DIRECTORY  0x02
#endif
#ifndef IRP_MN_USER_FS_REQUEST
#define IRP_MN_USER_FS_REQUEST          0x00
#endif
#ifndef IRP_MN_MOUNT_VOLUME
#define IRP_MN_MOUNT_VOLUME             0x01
#endif
#ifndef IRP_MN_VERIFY_VOLUME
#define IRP_MN_VERIFY_VOLUME            0x02
#endif
#ifndef SL_RESTART_SCAN
#define SL_RESTART_SCAN                 0x01
#endif
#ifndef SL_RETURN_SINGLE_ENTRY
#define SL_RETURN_SINGLE_ENTRY          0x02
#endif
#ifndef SL_INDEX_SPECIFIED
#define SL_INDEX_SPECIFIED              0x04
#endif
#ifndef SL_WATCH_TREE
#define SL_WATCH_TREE                   0x01
#endif
#ifndef SL_OPEN_PAGING_FILE
#define SL_OPEN_PAGING_FILE             0x02
#endif
#ifndef SL_OPEN_TARGET_DIRECTORY
#define SL_OPEN_TARGET_DIRECTORY        0x04
#endif
#ifndef SL_OVERRIDE_VERIFY_VOLUME
#define SL_OVERRIDE_VERIFY_VOLUME       0x02
#endif

#ifndef IO_REMOUNT
#define IO_REMOUNT                  0x00000001
#endif

#ifndef FILE_READ_ONLY_VOLUME
#define FILE_READ_ONLY_VOLUME       0x00080000
#endif

#ifndef FSCTL_IS_VOLUME_DIRTY
#define FSCTL_IS_VOLUME_DIRTY       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 30, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifndef VOLUME_IS_DIRTY
#define VOLUME_IS_DIRTY             0x00000001
#endif

#ifndef FILE_WRITE_TO_END_OF_FILE
#define FILE_WRITE_TO_END_OF_FILE   0xffffffff
#endif

#ifndef IOCTL_DISK_IS_WRITABLE
#define IOCTL_DISK_IS_WRITABLE      CTL_CODE(IOCTL_DISK_BASE, 0x0009, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

/* Status codes newer than some DDK headers */
#ifndef STATUS_USER_MAPPED_FILE
#define STATUS_USER_MAPPED_FILE     ((NTSTATUS)0xC0000243L)
#endif
#ifndef STATUS_CANNOT_MAKE
#define STATUS_CANNOT_MAKE          ((NTSTATUS)0xC000016BL)
#endif

/* Information classes newer than NT4, answered on both targets */
#define RFN_CLASS_ATTRIBUTE_TAG             35
#define RFN_CLASS_FS_FULL_SIZE              7

typedef struct _RFN_FILE_ATTRIBUTE_TAG_INFORMATION {
    ULONG       FileAttributes;
    ULONG       ReparseTag;
} RFN_FILE_ATTRIBUTE_TAG_INFORMATION, *PRFN_FILE_ATTRIBUTE_TAG_INFORMATION;

typedef struct _RFN_FILE_FS_FULL_SIZE_INFORMATION {
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER CallerAvailableAllocationUnits;
    LARGE_INTEGER ActualAvailableAllocationUnits;
    ULONG       SectorsPerAllocationUnit;
    ULONG       BytesPerSector;
} RFN_FILE_FS_FULL_SIZE_INFORMATION, *PRFN_FILE_FS_FULL_SIZE_INFORMATION;

/* ------------------------------------------------------------------ */
/* Debug output                                                        */
/* ------------------------------------------------------------------ */

#define RFN_PFX                 "[REFSNT] "

#if defined(RFN_DBGPORT)
VOID     RfnDbgPrintf(const char *Format, ...);
#define RFN_DBG(args)           RfnDbgPrintf args
#elif DBG || defined(RFN_DEBUG)
#define RFN_DBG(args)           DbgPrint args
#else
#define RFN_DBG(args)
#endif

/*
 * Debug trace to the QEMU "debugcon" port (0xE9): readable bytes with no
 * kernel-debugger protocol. Boot QEMU with -debugcon file:/path. Compile
 * with -DEXN_DBGPORT to enable; a no-op otherwise.
 */
#ifdef RFN_DBGPORT
static __inline void RfnDbgCh(unsigned char c)
{ unsigned short port = 0x00E9;
  __asm__ __volatile__ ("outb %b0, %w1" : : "a"(c), "d"(port)); }
static __inline void RfnDbgStr(const char *s)
{ while (*s) RfnDbgCh((unsigned char)*s++); }
static __inline void RfnDbgHex(ULONG_PTR v)
{ int i, n = (int)(sizeof(v) * 2); char h[17]; const char *d="0123456789abcdef";
  for (i=n-1;i>=0;i--){ h[i]=d[v&0xF]; v>>=4; } h[n]=0; RfnDbgStr(h); }
static __inline void RfnDbgUni(const void *us)
{ const unsigned short *p; unsigned short len, i;
  len = ((const UNICODE_STRING *)us)->Length;
  p   = (const unsigned short *)((const UNICODE_STRING *)us)->Buffer;
  if (!p) { RfnDbgStr("(null)"); return; }
  for (i=0;i<len/2 && i<120;i++) RfnDbgCh((unsigned char)(p[i]<128?p[i]:'?')); }
#define DBGP(s)     RfnDbgStr(s)
#define DBGX(v)     RfnDbgHex((ULONG_PTR)(v))
#define DBGU(u)     RfnDbgUni(&(u))
#else
#define DBGP(s)     ((void)0)
#define DBGX(v)     ((void)0)
#define DBGU(u)     ((void)0)
#endif


/* ------------------------------------------------------------------ */
/* Structured exception handling                                       */
/* ------------------------------------------------------------------ */

/*
 * The Cache Manager, the Memory Manager and the probe routines report I/O
 * errors and bad user buffers by raising. Code that calls them runs that
 * part through RfnTryCall (exnseh.c), which returns the exception code
 * (STATUS_SUCCESS when nothing was raised). With ExpectedOnly only the
 * codes FsRtlIsNtstatusExpected accepts (and in-page errors) are caught;
 * anything else is a bug and stops the system where it happened.
 */
typedef VOID (*RFN_TRY_BODY)(PVOID Context);

NTSTATUS RfnTryCall(RFN_TRY_BODY Body, PVOID Context, BOOLEAN ExpectedOnly);

/* RtlCopyMemory to or from a caller's buffer, faults caught (mapped status) */
NTSTATUS RfnSafeCopy(PVOID Destination, const VOID *Source, SIZE_T Length);

/* Maps an exception code to the status a request returns */
NTSTATUS RfnExceptionStatus(NTSTATUS Code);

/* ------------------------------------------------------------------ */
/* Limits and tags                                                     */
/* ------------------------------------------------------------------ */

#define RFN_FS_DEVICE_NAME      L"\\Refsnt"
#define RFN_CDFS_DEVICE_NAME    L"\\RefsntCdrom"
#define RFN_TAG                 'tnfR'
#define RFN_TAG_FCB             'FnfR'
#define RFN_TAG_CCB             'CnfR'
#define RFN_TAG_RUNS            'RnfR'
#define RFN_TAG_BUFFER          'BnfR'
#define RFN_TAG_NAME            'NnfR'
#define RFN_TAG_WORK            'WnfR'
#define RFN_TAG_META            'MnfR'
#define RFN_TAG_EA              'EnfR'
#define RFN_TAG_ENGINE          'enfR'
#define RFN_TAG_INDEX           'InfR'
#define RFN_TAG_REGISTRY        'GnfR'

#define RFN_PAGE_SIZE           0x1000
#define RFN_ZERO_CHUNK          0x10000
#define RFN_MAX_TRANSFER        0x100000    /* cap one disk IRP; miniport can't take a multi-hundred-MB transfer */
#define RFN_MAX_LABEL           32          /* NT's longest label (ReFS keeps 32 characters) */
#define RFN_MAX_NAME            RFX_MAX_NAME   /* bytes of UTF-8 */
#define RFN_SECTOR_SIZE         (Vcb->SectorSize)       /* the device's sector (512 - 4096) */
#define RFN_SECTOR_SHIFT        (Vcb->SectorShift)
#define RFN_MAX_LINK_DEPTH      8           /* symbolic links followed per open */

/* NT attribute bits we keep (the rest are computed) */
#define RFN_ATTR_SETTABLE       (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | \
                                 FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE)

/* ------------------------------------------------------------------ */
/* Node types                                                          */
/* ------------------------------------------------------------------ */

#define RFN_NTC_VCB             ((CSHORT)0x0E21)
#define RFN_NTC_FCB             ((CSHORT)0x0E22)    /* file (or symbolic link opened as itself) */
#define RFN_NTC_DCB             ((CSHORT)0x0E23)    /* directory */
#define RFN_NTC_VFCB            ((CSHORT)0x0E24)    /* whole volume (DASD) */
#define RFN_NTC_CCB             ((CSHORT)0x0E25)

#define RfnNodeType(Ptr)        (*((CSHORT *)(Ptr)))

/* ------------------------------------------------------------------ */
/* Structures                                                          */
/* ------------------------------------------------------------------ */

typedef struct _RFN_VCB RFN_VCB, *PRFN_VCB;

/*
 * NT 3.1's ERESOURCE is larger than later headers describe:
 * every embedded ERESOURCE is followed by padding.
 */
#define RFN_ERESOURCE_PAD       0x40

/* A run of a paging file (kept nonpaged: paging I/O never enters the engine) */
typedef struct _RFN_RUN {
    ULONGLONG   FileBlock;
    ULONGLONG   DiskBlock;
    ULONG       Length;
} RFN_RUN, *PRFN_RUN;

/* Case-insensitive name index of a big directory (see extdctl.c) */
typedef struct _RFN_NAME_INDEX RFN_NAME_INDEX, *PRFN_NAME_INDEX;

typedef struct _RFN_FCB {
    FSRTL_COMMON_FCB_HEADER Header;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    ERESOURCE   Resource;
    UCHAR       ResourcePad[RFN_ERESOURCE_PAD];
    ERESOURCE   PagingIoResource;
    UCHAR       PagingIoResourcePad[RFN_ERESOURCE_PAD];
    PRFN_VCB    Vcb;
    struct _RFN_FCB *ParentDcb;     /* where it was first opened (directories: the parent) */
    LIST_ENTRY  FcbLinks;
    ULONG       RefCount;           /* file objects + child FCBs */
    ULONG       UncleanCount;
    ULONG       NonCachedUncleanCount;
    ULONG       FcbState;
    SHARE_ACCESS ShareAccess;
    FILE_LOCK   FileLock;
    ULONG       FileLockSpare[16];  /* NT 3.1's FILE_LOCK is larger than the headers' */
    PKTHREAD    LazyWriteThread;

    ULONG       Ino;                /* the file's identity */
    RFX_INODE   Inode;              /* in-memory copy (under the engine) */
    ULONG       Attributes;         /* NT attributes */
    ULONG       DosAttributes;      /* kept in user.DOSATTRIB: hidden/system/archive */
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER ChangeTime;
    UNICODE_STRING Name;            /* last component as NT sees it */
    UNICODE_STRING FullName;        /* built on demand */
    ULONG       RfxNameLength;      /* the name as stored (UTF-8 bytes) */
    UCHAR       RfxName[RFN_MAX_NAME + 1];

    /* last mapping, so sequential paging I/O does not walk the tree each time
       (under the paging resource; cleared whenever the map changes) */
    ULONGLONG   MapFileBlock;
    ULONGLONG   MapDiskBlock;
    ULONG       MapRun;             /* 0 = nothing cached */
    BOOLEAN     MapHole;

    /* a paging file's whole map */
    PRFN_RUN    Runs;
    ULONG       RunCount;

    PRFN_NAME_INDEX NameIndex;      /* directories */

    /* the name a pending delete removes (the handle that asked for it:
       with hard links that is not necessarily the FCB's own name) */
    ULONG       DeleteParentIno;
    ULONG       DeleteNameLength;
    UCHAR       DeleteName[RFN_MAX_NAME + 1];
} RFN_FCB, *PRFN_FCB;

#define FCB_STATE_ROOT              0x0001
#define FCB_STATE_DELETE_PENDING    0x0004
#define FCB_STATE_DELETED           0x0008  /* off the disk, kept for Cc and Mm */
#define FCB_STATE_INODE_DIRTY       0x0010  /* the inode lags behind the FCB */
#define FCB_STATE_TRUNCATE_ON_CLOSE 0x0020
#define FCB_STATE_SYMLINK           0x0040  /* a symbolic link opened as itself */
#define FCB_STATE_PAGING_FILE       0x0200
#define FCB_STATE_VISITED           0x0400  /* volume flush walk */

typedef struct _RFN_CCB {
    CSHORT      NodeTypeCode;
    CSHORT      NodeByteSize;
    ULONG       Flags;
    ULONG       QueryState;         /* 0: ".", 1: "..", 2: entries */
    ULONGLONG   Cookie;             /* directory position of the next entry */
    ULONG       EaIndex;            /* next EA for a query-EA continuation */
    UNICODE_STRING Pattern;         /* upcased */
    /* the name this handle was opened under (hard links: delete/rename act on it) */
    ULONG       ParentIno;
    ULONG       NameLength;
    UCHAR       Name[RFN_MAX_NAME + 1];
} RFN_CCB, *PRFN_CCB;

#define CCB_FLAG_PATTERN_SET        0x0001
#define CCB_FLAG_WILDCARD           0x0002
#define CCB_FLAG_MATCH_ALL          0x0004
#define CCB_FLAG_VOLUME_OPEN        0x0008
#define CCB_FLAG_DISMOUNTED_VOLUME  0x0010
#define CCB_FLAG_DELETE_ON_CLOSE    0x0020
#define CCB_FLAG_USER_SET_WRITE     0x0040
#define CCB_FLAG_USER_SET_ACCESS    0x0080
#define CCB_FLAG_WRITE_HANDLE       0x0100
#define CCB_FLAG_HAVE_NAME          0x0200

#define RFN_COMMIT_SECONDS      5                   /* transaction commit interval */

struct _RFN_VCB {
    CSHORT      NodeTypeCode;
    CSHORT      NodeByteSize;
    ERESOURCE   Resource;
    UCHAR       ResourcePad[RFN_ERESOURCE_PAD];
    ERESOURCE   EngineResource;     /* one thread in the engine at a time */
    UCHAR       EngineResourcePad[RFN_ERESOURCE_PAD];
    LIST_ENTRY  VcbLinks;
    PVPB        Vpb;
    PDEVICE_OBJECT TargetDeviceObject;
    PDEVICE_OBJECT VolumeDeviceObject;
    ULONG       VcbState;
    ULONG       OpenCount;
    ULONG       UncleanCount;
    ULONG       WriteCount;
    PFILE_OBJECT LockFileObject;
    PKTHREAD    VerifyThread;
    PRFN_FCB    VolumeFcb;
    PRFN_FCB    RootDcb;
    LIST_ENTRY  FcbList;

    ULONG       SectorSize;         /* the device's */
    ULONG       SectorShift;
    BOOLEAN     Optical;            /* a CD/DVD/BD drive */
    RFX_VOL     Vol;                /* engine state */
    LONGLONG    VolumeBytes;        /* file system size */
    LONGLONG    PartitionBytes;
    ULONG       SerialNumber;
    BOOLEAN     Writing;            /* RfxStartWriting done: the volume is in use */
    BOOLEAN     BootView;           /* refsboot made it bootable: keep its FAT view current */
    ULONG       LabelLength;        /* characters */
    WCHAR       Label[RFN_MAX_LABEL];

    ULONG       Commits;            /* transactions written */
    PUCHAR      Bounce;             /* the engine's staging buffer (nonpaged; under EngineResource) */

    /* Periodic journal commit */
    KTIMER      CommitTimer;
    KDPC        CommitDpc;
    WORK_QUEUE_ITEM CommitItem;
    LONG        CommitQueued;
    BOOLEAN     TimerSet;
    KEVENT      CommitIdle;         /* signalled when no commit item is pending */

    PNOTIFY_SYNC NotifySync;
    LIST_ENTRY  DirNotifyList;
};

#define VCB_STATE_MOUNTED           0x0001
#define VCB_STATE_LOCKED            0x0002
#define VCB_STATE_DISMOUNTED        0x0004
#define VCB_STATE_IN_DISMOUNT       0x0008
#define VCB_STATE_DELETE_PENDING    0x0010
#define VCB_STATE_DASD_WRITTEN      0x0020
#define VCB_STATE_REMOVABLE         0x0040
#define VCB_STATE_PNP_LOCKED        0x0080
#define VCB_STATE_FREE_VPB          0x0100
#define VCB_STATE_REMOVED           0x0200
#define VCB_STATE_READ_ONLY         0x0400
#define VCB_STATE_FLUSH_ON_CLOSE    0x1000
#define VCB_STATE_SHUTDOWN          0x2000

typedef struct _RFN_DATA {
    PDRIVER_OBJECT  DriverObject;
    PDEVICE_OBJECT  FileSystemDeviceObject;
    PDEVICE_OBJECT  CdromDeviceObject;      /* the same, for CD / DVD / BD drives */
    ERESOURCE       Resource;
    UCHAR           ResourcePad[RFN_ERESOURCE_PAD];
    LIST_ENTRY      VcbList;
    FAST_IO_DISPATCH FastIoDispatch;
    CACHE_MANAGER_CALLBACKS CacheManagerCallbacks;
    UNICODE_STRING  RegistryPath;
    BOOLEAN         HideDotFiles;
    BOOLEAN         MountOptical;   /* registry: mount ReFS in CD / DVD / BD drives (off) */
    BOOLEAN         FollowLinks;    /* registry: follow symbolic links on open */
    BOOLEAN         CaseSensitive;  /* registry: exact-case lookups only */
    BOOLEAN         Writable;       /* registry: allow writing at all */
    ULONG           DefaultUid, DefaultGid;
    ULONG           FileMode, DirMode;  /* permission bits of new files / dirs */
    BOOLEAN         ShuttingDown;   /* IRP_MJ_SHUTDOWN: skip the disk cache-flush */
} RFN_DATA, *PRFN_DATA;

extern RFN_DATA RfnData;

typedef struct _RFN_IRP_CONTEXT {
    PIRP            Irp;
    PIO_STACK_LOCATION IrpSp;
    PDEVICE_OBJECT  DeviceObject;
    PRFN_VCB        Vcb;
    ULONG           Flags;
    NTSTATUS        ExceptionStatus;
    NTSTATUS        DispatchStatus;
} RFN_IRP_CONTEXT, *PRFN_IRP_CONTEXT;

#define RFN_CTX_TOP_LEVEL           0x0001
#define RFN_CTX_NO_COMPLETE         0x0002

typedef struct _RFN_CLOSE_ITEM {
    WORK_QUEUE_ITEM Item;
    PRFN_VCB    Vcb;
    PRFN_FCB    Fcb;
    PRFN_CCB    Ccb;
} RFN_CLOSE_ITEM, *PRFN_CLOSE_ITEM;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

#define RfnIsFcb(Fcb)       ((Fcb)->Header.NodeTypeCode == RFN_NTC_FCB)
#define RfnIsDcb(Fcb)       ((Fcb)->Header.NodeTypeCode == RFN_NTC_DCB)
#define RfnIsVfcb(Fcb)      ((Fcb)->Header.NodeTypeCode == RFN_NTC_VFCB)

#define RfnRoundUp(Value, Size) \
    (((Value) + ((Size) - 1)) & ~((LONGLONG)(Size) - 1))

#define RFN_WRITE_ACCESS    (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | \
                             FILE_WRITE_ATTRIBUTES | DELETE | FILE_DELETE_CHILD)

/* the largest file the volume's mapping can describe */
#define RfnMaxFileSize(Vcb) (0x7FFFFFFFFFFFF000ULL)    /* ReFS: 2^64 bytes; NT: 2^63 */

/* engine status -> NT status (most engine codes are NT codes already) */
NTSTATUS RfnStatus(RFX_STATUS S);

/* ------------------------------------------------------------------ */
/* Prototypes                                                          */
/* ------------------------------------------------------------------ */

/* extinit.c */
NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
BOOLEAN RfnWriteSupportEnabled(VOID);

/* extdisp.c */
NTSTATUS NTAPI RfnFsdDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS RfnVerifyVcb(PRFN_IRP_CONTEXT Ctx, PRFN_VCB Vcb);
NTSTATUS RfnVerifyWritable(PRFN_IRP_CONTEXT Ctx, PRFN_VCB Vcb);

/* extcrea.c */
NTSTATUS RfnCommonCreate(PRFN_IRP_CONTEXT Ctx);

/* extclos.c */
NTSTATUS RfnCommonCleanup(PRFN_IRP_CONTEXT Ctx);
NTSTATUS RfnCommonClose(PRFN_IRP_CONTEXT Ctx);
BOOLEAN  RfnTryTeardown(PRFN_VCB Vcb, ULONG InFlightReferences);
VOID     RfnDeleteVcb(PRFN_VCB Vcb);
BOOLEAN  RfnDismountVcb(PRFN_VCB Vcb);
VOID     RfnUnlockVcb(PRFN_VCB Vcb);
VOID     RfnPurgeCachedFiles(PRFN_VCB Vcb);
NTSTATUS RfnUnlinkName(PRFN_VCB Vcb, PRFN_FCB Fcb);
VOID     RfnSetDeleteName(PRFN_FCB Fcb, PRFN_CCB Ccb);
PRFX_INODE RfnDirInode(PRFN_VCB Vcb, ULONG Ino, PRFX_INODE Temp);

/* extread.c / extwrit.c */
NTSTATUS RfnCommonRead(PRFN_IRP_CONTEXT Ctx);
NTSTATUS RfnCommonWrite(PRFN_IRP_CONTEXT Ctx);
NTSTATUS RfnZeroFileRange(PRFN_FCB Fcb, PFILE_OBJECT FileObject, LONGLONG Start, LONGLONG End);

/* extdctl.c */
NTSTATUS RfnCommonDirectoryControl(PRFN_IRP_CONTEXT Ctx);
VOID     RfnNotifyCleanupList(PRFN_VCB Vcb, PRFN_CCB Ccb);
VOID     RfnNotifyChange(PRFN_VCB Vcb, PRFN_FCB Fcb, ULONG Filter, ULONG Action);
NTSTATUS RfnLookupName(PRFN_VCB Vcb, PRFN_FCB Dir, PCUNICODE_STRING Name, BOOLEAN CaseSensitive,
                       PRFX_ENTRY Entry);
VOID     RfnIndexAdd(PRFN_FCB Dir, const UCHAR *Name, ULONG Length, ULONG Ino);
VOID     RfnIndexRemove(PRFN_FCB Dir, const UCHAR *Name, ULONG Length);
VOID     RfnIndexFree(PRFN_FCB Dir);

/* extinfo.c */
NTSTATUS RfnCommonQueryInformation(PRFN_IRP_CONTEXT Ctx);
ULONG    RfnNtAttributes(ULONG Attributes);
VOID     RfnFillBasicInfo(PRFN_FCB Fcb, PFILE_BASIC_INFORMATION Info);
VOID     RfnFillStandardInfo(PRFN_FCB Fcb, PFILE_STANDARD_INFORMATION Info);
VOID     RfnFillNetworkOpenInfo(PRFN_FCB Fcb, PFILE_NETWORK_OPEN_INFORMATION Info);

/* extseti.c */
NTSTATUS RfnCommonSetInformation(PRFN_IRP_CONTEXT Ctx);
NTSTATUS RfnSetFileSize(PRFN_IRP_CONTEXT Ctx, PRFN_FCB Fcb, PFILE_OBJECT FileObject, LONGLONG NewSize);

/* exteas.c */
NTSTATUS RfnCommonQueryEa(PRFN_IRP_CONTEXT Ctx);
NTSTATUS RfnCommonSetEa(PRFN_IRP_CONTEXT Ctx);
NTSTATUS RfnSetEaBuffer(PRFN_VCB Vcb, PRFN_FCB Fcb, PFILE_FULL_EA_INFORMATION Buffer, ULONG Length,
                        BOOLEAN Replace);
ULONG    RfnNtEaSize(PRFN_VCB Vcb, PRFN_FCB Fcb);
NTSTATUS RfnCheckEaBuffer(PFILE_FULL_EA_INFORMATION Buffer, ULONG Length, PULONG ErrorOffset);

/* extvol.c */
NTSTATUS RfnCommonQueryVolumeInformation(PRFN_IRP_CONTEXT Ctx);
NTSTATUS RfnCommonSetVolumeInformation(PRFN_IRP_CONTEXT Ctx);

/* extfsct.c */
NTSTATUS RfnCommonFileSystemControl(PRFN_IRP_CONTEXT Ctx);
NTSTATUS RfnCommonPnp(PRFN_IRP_CONTEXT Ctx);
NTSTATUS RfnUserFsRequest(PRFN_IRP_CONTEXT Ctx);
VOID     RfnLoadLabel(PRFN_VCB Vcb);
NTSTATUS RfnLoadVolume(PRFN_VCB Vcb);
BOOLEAN  RfnSameVolume(PRFN_VCB Vcb);

/* extletr.c */
VOID     RfnStartDriveLetters(VOID);
VOID     RfnClaimRegistryLetter(PRFN_VCB Vcb, PDEVICE_OBJECT TargetDevice, LONGLONG StartingOffset);

/* extmisc.c */
NTSTATUS RfnCommonDeviceControl(PRFN_IRP_CONTEXT Ctx);
NTSTATUS RfnCommonLockControl(PRFN_IRP_CONTEXT Ctx);

/* extflsh.c */
NTSTATUS RfnCommonFlushBuffers(PRFN_IRP_CONTEXT Ctx);
NTSTATUS RfnCommonShutdown(PRFN_IRP_CONTEXT Ctx);
NTSTATUS RfnFlushFile(PRFN_VCB Vcb, PRFN_FCB Fcb);
NTSTATUS RfnFlushVolume(PRFN_VCB Vcb, BOOLEAN Final);
NTSTATUS RfnStartWriting(PRFN_VCB Vcb);
NTSTATUS RfnStopWriting(PRFN_VCB Vcb);
VOID     RfnResumeWriting(PRFN_VCB Vcb);
VOID     RfnUpdateBootView(PRFN_VCB Vcb);

/* extfast.c */
VOID     RfnInitializeFastIo(PFAST_IO_DISPATCH FastIo);
VOID     RfnInitializeCacheCallbacks(VOID);
UCHAR    RfnIsFastIoPossible(PRFN_FCB Fcb);

/* extmeta.c: the engine's host and the metadata cache */
VOID     RfnEngineEnter(PRFN_VCB Vcb);
NTSTATUS RfnEngineLeave(PRFN_VCB Vcb, NTSTATUS Status);
#ifdef RFN_NT31
extern BOOLEAN RfnNt31;                       /* running on NT 3.1 (extres.c) */
#endif       /* extshim view (engine not held) */
NTSTATUS RfnCommit(PRFN_VCB Vcb);              /* journal commit (engine held) */
NTSTATUS RfnCommitNow(PRFN_VCB Vcb);           /* takes the engine itself */
VOID     RfnBounceFree(PRFN_VCB Vcb);           /* the staging buffer goes (volume deleted) */
VOID     RfnMetaPurge(PRFN_VCB Vcb);
VOID     RfnMetaInitialize(PRFN_VCB Vcb);
VOID     RfnCommitTimerStart(PRFN_VCB Vcb);
VOID     RfnCommitTimerStop(PRFN_VCB Vcb);

/* extio.c */
NTSTATUS RfnSyncIo(PDEVICE_OBJECT Device, UCHAR MajorFunction, LONGLONG Offset,
                   ULONG Length, PVOID Buffer, BOOLEAN OverrideVerify);
NTSTATUS RfnDeviceIoctl(PDEVICE_OBJECT Device, ULONG IoControlCode, PVOID InputBuffer,
                        ULONG InputLength, PVOID OutputBuffer, ULONG OutputLength,
                        BOOLEAN OverrideVerify, PULONG Information);
NTSTATUS RfnFlushDevice(PRFN_VCB Vcb);
NTSTATUS RfnLockUserBuffer(PIRP Irp, LOCK_OPERATION Operation, ULONG Length);
PVOID    RfnMapUserBuffer(PIRP Irp);
NTSTATUS RfnNonCachedIo(PRFN_IRP_CONTEXT Ctx, PRFN_FCB Fcb, UCHAR MajorFunction,
                        LONGLONG StartingVbo, ULONG ByteCount, LONGLONG ValidData);
NTSTATUS RfnZeroDisk(PRFN_VCB Vcb, PRFN_FCB Fcb, LONGLONG Start, LONGLONG End);
NTSTATUS RfnVboToLbo(PRFN_VCB Vcb, PRFN_FCB Fcb, LONGLONG Vbo, BOOLEAN Allocate,
                     PLONGLONG Lbo, PULONG Contiguous);
NTSTATUS RfnReadDisk(PRFN_VCB Vcb, ULONGLONG Sector, ULONG Count, PVOID Buffer);
NTSTATUS RfnWriteDisk(PRFN_VCB Vcb, ULONGLONG Sector, ULONG Count, const VOID *Buffer);
NTSTATUS RfnAllocateRange(PRFN_VCB Vcb, PRFN_FCB Fcb, LONGLONG Start, LONGLONG End);
NTSTATUS RfnPrepareForWrite(PRFN_VCB Vcb, PRFN_FCB Fcb);
NTSTATUS RfnWriteData(PRFN_VCB Vcb, ULONGLONG Sector, ULONG Count, const VOID *Buffer);

#define RfnIsFsDevice(D)    ((D) == RfnData.FileSystemDeviceObject || (D) == RfnData.CdromDeviceObject)
#define RfnReadSectors(Device, Offset, Length, Buffer, Override) \
    RfnSyncIo((Device), IRP_MJ_READ, (Offset), (Length), (Buffer), (Override))
#define RfnWriteSectors(Device, Offset, Length, Buffer, Override) \
    RfnSyncIo((Device), IRP_MJ_WRITE, (Offset), (Length), (Buffer), (Override))

/* extstrc.c */
PRFN_FCB RfnCreateVolumeFcb(PRFN_VCB Vcb);
PRFN_FCB RfnCreateRootDcb(PRFN_VCB Vcb);
NTSTATUS RfnCreateFcb(PRFN_VCB Vcb, PRFN_FCB ParentDcb, PRFX_ENTRY Entry, BOOLEAN AsLink, PRFN_FCB *Fcb);
VOID     RfnDeleteFcb(PRFN_FCB Fcb);
VOID     RfnDereferenceFcb(PRFN_VCB Vcb, PRFN_FCB Fcb);
PRFN_FCB RfnFindFcbByIno(PRFN_VCB Vcb, ULONG Ino, BOOLEAN AsLink);
PRFN_CCB RfnCreateCcb(VOID);
VOID     RfnDeleteCcb(PRFN_CCB Ccb);
NTSTATUS RfnBuildFullName(PRFN_FCB Fcb);
VOID     RfnForgetNames(PRFN_VCB Vcb, PRFN_FCB Fcb);
BOOLEAN  RfnIsAncestor(PRFN_FCB Ancestor, PRFN_FCB Fcb);
LARGE_INTEGER RfnToNtTime(ULONG Seconds, ULONG Extra);
VOID     RfnFromNtTime(LARGE_INTEGER Time, PULONG Seconds, PULONG Extra);
NTSTATUS RfnLoadInode(PRFN_VCB Vcb, PRFN_FCB Fcb);
VOID     RfnInodeToFcb(PRFN_VCB Vcb, PRFN_FCB Fcb);
NTSTATUS RfnUpdateInode(PRFN_VCB Vcb, PRFN_FCB Fcb);
NTSTATUS RfnSetAllocation(PRFN_VCB Vcb, PRFN_FCB Fcb, ULONGLONG Bytes);
NTSTATUS RfnLoadRuns(PRFN_VCB Vcb, PRFN_FCB Fcb);
VOID     RfnForgetMapping(PRFN_FCB Fcb);
NTSTATUS RfnUnicodeToExt(PCUNICODE_STRING Name, PUCHAR Out, PULONG OutLength);
NTSTATUS RfnExtToUnicode(const UCHAR *In, ULONG InLength, PUNICODE_STRING Name);
BOOLEAN  RfnIsValidUnicodeName(PCUNICODE_STRING Name);
ULONG    RfnComputeAttributes(PRFN_VCB Vcb, PRFX_INODE Inode, const UCHAR *Name, ULONG NameLength,
                              ULONG DosAttributes);
ULONG    RfnLoadDosAttributes(PRFN_VCB Vcb, ULONG Ino, PRFX_INODE Inode);
NTSTATUS RfnStoreDosAttributes(PRFN_VCB Vcb, PRFN_FCB Fcb, ULONG Attributes);

#endif /* _RFN_H_ */

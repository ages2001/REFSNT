/*
 * REFSNT - driver entry and settings
 *
 * Settings live in the service key (HKLM\System\CurrentControlSet\
 * Services\Refsnt) and are read at every mount, so a change applies to
 * volumes mounted afterwards:
 *
 *   EnableWriteSupport (REG_DWORD)  1 or missing: read/write, 0: read-only
 *   HideDotFiles       (REG_DWORD)  1: ".name" is hidden (0 or missing: not)
 *   MountOptical       (REG_DWORD)  1: ReFS in CD / DVD / BD drives too (0 or missing: not)
 *   FollowSymlinks     (REG_DWORD)  1 or missing: opens follow symbolic links
 *   CaseSensitive      (REG_DWORD)  0 or missing: names are found in any case
 *                                   (an exact match first), 1: exact only
 *   DefaultUid, DefaultGid (REG_DWORD)  owner of new files (0 = root)
 *   FileMode, DirMode  (REG_DWORD)  permissions of new files (0644) and
 *                                   directories (0755)
 */

#include "rfn.h"

RFN_DATA RfnData;

/* extinst recognises this driver by this string */
#ifdef __GNUC__
__attribute__((used))
#endif
const char RfnSignature[] = "REFSNT file system driver";

#ifdef RFN_CROSS
PIRP RfnXTopLevelIrp;
#endif

/* Keeps a NUL-terminated copy of the service key path */
static VOID
RfnSaveRegistryPath (
    PUNICODE_STRING RegistryPath
    )
{
    USHORT Size;

    if (RegistryPath == NULL || RegistryPath->Length == 0) {
        return;
    }

    Size = (USHORT)(RegistryPath->Length + sizeof(WCHAR));
    RfnData.RegistryPath.Buffer = (PWCHAR)ExAllocatePoolWithTag(PagedPool, Size, RFN_TAG_REGISTRY);

    if (RfnData.RegistryPath.Buffer != NULL) {
        RtlCopyMemory(RfnData.RegistryPath.Buffer, RegistryPath->Buffer, RegistryPath->Length);
        RfnData.RegistryPath.Buffer[RegistryPath->Length / sizeof(WCHAR)] = 0;
        RfnData.RegistryPath.Length = RegistryPath->Length;
        RfnData.RegistryPath.MaximumLength = Size;
    }
}

static ULONG
RfnQueryDword (
    PCWSTR Name,
    ULONG Default
    )
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = Default;

    if (RfnData.RegistryPath.Buffer == NULL) {
        return Default;
    }

    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = REG_DWORD;
    Table[0].DefaultData = &Default;
    Table[0].DefaultLength = sizeof(ULONG);

    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, RfnData.RegistryPath.Buffer,
                                           Table, NULL, NULL))) {
        return Default;
    }
    return Value;
}

/* Reads the settings; called at every mount */
static VOID
RfnLoadSettings (
    VOID
    )
{
    RfnData.Writable = (BOOLEAN)(RfnQueryDword(L"EnableWriteSupport", 1) != 0);
    RfnData.HideDotFiles = (BOOLEAN)(RfnQueryDword(L"HideDotFiles", 0) != 0);
    RfnData.MountOptical = (BOOLEAN)(RfnQueryDword(L"MountOptical", 0) != 0);
    RfnData.FollowLinks = (BOOLEAN)(RfnQueryDword(L"FollowSymlinks", 1) != 0);
    RfnData.CaseSensitive = (BOOLEAN)(RfnQueryDword(L"CaseSensitive", 0) != 0);
    RfnData.DefaultUid = RfnQueryDword(L"DefaultUid", 0);
    RfnData.DefaultGid = RfnQueryDword(L"DefaultGid", 0);
    RfnData.FileMode = RfnQueryDword(L"FileMode", 0644) & 07777;
    RfnData.DirMode = RfnQueryDword(L"DirMode", 0755) & 07777;
}

/* Settings again (a mount is starting); FALSE: mount read-only */
BOOLEAN
RfnWriteSupportEnabled (
    VOID
    )
{
    RfnLoadSettings();
    return RfnData.Writable;
}

#ifdef RFN_SEHTEST
/* Self-test of RfnTryCall (debug builds): two raises, both must be caught */
static VOID RfnRaiseBody(PVOID Context) { ExRaiseStatus(*(PNTSTATUS)Context); }
static VOID RfnNestedBody(PVOID Context) { (VOID)RfnTryCall(RfnRaiseBody, Context, TRUE); ExRaiseStatus(STATUS_DISK_FULL); }
static VOID
RfnSehSelfTest (
    VOID
    )
{
    NTSTATUS Code = STATUS_INVALID_USER_BUFFER, R;
    R = RfnTryCall(RfnRaiseBody, &Code, FALSE);
    DBGP("[BTN sehtest 1 "); DBGX(R); DBGP("]\n");
    Code = STATUS_IN_PAGE_ERROR;
    R = RfnTryCall(RfnNestedBody, &Code, TRUE);
    DBGP("[BTN sehtest 2 "); DBGX(R); DBGP("]\n");
    R = RfnSafeCopy(&Code, (PVOID)(ULONG_PTR)16, sizeof(Code));
    DBGP("[BTN sehtest 3 "); DBGX(R); DBGP("]\n");
}
#endif

NTSTATUS
NTAPI
DriverEntry (
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    UNICODE_STRING Name;
    PDEVICE_OBJECT DeviceObject;
    NTSTATUS Status;
    ULONG i;

#ifdef RFN_NT31
    /* Bind the executive-resource API to the running kernel (Lite on NT 3.51+,
     * legacy on NT 3.1) before any resource is initialised. */
    if (!RfnResolveResourceApi()) {
        DBGP("[REFSNT:resource API not found]\n");
        return STATUS_PROCEDURE_NOT_FOUND;
    }
#endif

    RtlZeroMemory(&RfnData, sizeof(RfnData));

#ifdef RFN_SEHTEST
    RfnSehSelfTest();
#endif

    RfnSaveRegistryPath(RegistryPath);
    RfnLoadSettings();

    RtlInitUnicodeString(&Name, RFN_FS_DEVICE_NAME);

    Status = IoCreateDevice(DriverObject,
                            0,
                            &Name,
                            FILE_DEVICE_DISK_FILE_SYSTEM,
                            0,
                            FALSE,
                            &DeviceObject);

    if (!NT_SUCCESS(Status)) {
        RFN_DBG((RFN_PFX "IoCreateDevice failed %lX\n", Status));
        return Status;
    }

    RfnData.DriverObject = DriverObject;
    RfnData.FileSystemDeviceObject = DeviceObject;

    ExInitializeResourceLite(&RfnData.Resource);
    InitializeListHead(&RfnData.VcbList);

    /*
     * Wire the dispatch table. The DRIVER_OBJECT.MajorFunction[] array is
     * sized IRP_MJ_MAXIMUM_FUNCTION+1 by the running kernel, and that count
     * grew over time: NT 3.1 - 3.51 stop well below the value the modern
     * (WDK / mingw) headers define for IRP_MJ_MAXIMUM_FUNCTION (0x1b), so a
     * loop to the header's maximum writes past the end of the kernel's array
     * and corrupts the adjacent pool block. Only the low majors exist on NT
     * 3.1 anyway (no Power/WMI/PnP), so cap at the highest one REFSNT actually
     * serves - IRP_MJ_CLEANUP (0x12), which every NT since 3.1 provides.
     */
#ifdef RFN_NT31
    for (i = 0; i <= IRP_MJ_CLEANUP; i++) {
        DriverObject->MajorFunction[i] = RfnFsdDispatch;
    }
#else
    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
        DriverObject->MajorFunction[i] = RfnFsdDispatch;
    }
#endif

    RfnInitializeFastIo(&RfnData.FastIoDispatch);
    DriverObject->FastIoDispatch = &RfnData.FastIoDispatch;

    RfnInitializeCacheCallbacks();

#ifdef DO_DEVICE_INITIALIZING
    DeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
#endif

    /* Volumes are left clean at shutdown */
    (VOID)IoRegisterShutdownNotification(DeviceObject);

    IoRegisterFileSystem(DeviceObject);

    /* optical drives: a second file system device (off unless asked for) */
    if (RfnData.MountOptical) {
        UNICODE_STRING CdName;
        PDEVICE_OBJECT Cd;
        RtlInitUnicodeString(&CdName, RFN_CDFS_DEVICE_NAME);
        if (NT_SUCCESS(IoCreateDevice(DriverObject, 0, &CdName, FILE_DEVICE_CD_ROM_FILE_SYSTEM, 0, FALSE, &Cd))) {
#ifdef DO_DEVICE_INITIALIZING
            Cd->Flags &= ~DO_DEVICE_INITIALIZING;
#endif
            RfnData.CdromDeviceObject = Cd;
            IoRegisterFileSystem(Cd);
        }
    }

    /* Linux partitions get no drive letter from Windows: give them one */
    RfnStartDriveLetters();

    RFN_DBG((RFN_PFX "Loaded\n"));

    return STATUS_SUCCESS;
}

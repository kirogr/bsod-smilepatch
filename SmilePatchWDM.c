#include <ntddk.h>

#define IOCTL_SMILEPATCH_APPLY \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define DEVICE_NAME L"\\Device\\SmilePatch"
#define SYMLINK     L"\\DosDevices\\SmilePatch"

static const UCHAR g_BgpProlog[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08,
    0x4C, 0x89, 0x44, 0x24, 0x18,
    0x48, 0x89, 0x54, 0x24, 0x10,
    0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57,
    0x48, 0x8B, 0xEC,
    0x48, 0x83, 0xEC, 0x50
};

/*
 *   ':' = 3a 00
 *   '(' = 28 00
 *   nul = 00 00
 */
static const UCHAR g_SadBytes[] = { 0x3A, 0x00, 0x28, 0x00, 0x00, 0x00 };

static PUCHAR ScanMemory(
    PUCHAR start,
    SIZE_T length,
    const UCHAR* pattern,
    const UCHAR* mask,
    SIZE_T patternLen)
{
    SIZE_T i, j;

    for (i = 0; i <= length - patternLen; i++)
    {
        for (j = 0; j < patternLen; j++)
        {
            if (mask[j] && start[i + j] != pattern[j])
                break;
        }

        if (j == patternLen)
            return start + i;
    }

    return NULL;
}

static PVOID GetNtBase(PVOID KernelAddr)
{
    ULONG_PTR addr = (ULONG_PTR)KernelAddr & ~0xFFFULL;

    while (addr > 0xFFFF000000000000ULL)
    {
        if (*(USHORT*)addr == 0x5A4D) // 0x5A4D -> "MZ" DOS header signature
        {
            ULONG offset = *(ULONG*)(addr + 0x3C);

            if (offset <= 0x1000 && *(ULONG*)(addr + offset) == 0x00004550)
            {
                return (PVOID)addr;
            }
        }

        addr -= 0x1000;
    }

    return NULL;
}

static NTSTATUS PatchEmoticon(void)
{
    /* anchor */
    UNICODE_STRING exportName = RTL_CONSTANT_STRING(L"KeBugCheckEx");
    PVOID anchor = MmGetSystemRoutineAddress(&exportName);
    if (!anchor) {
        DbgPrint("[SmilePatch] ERROR: KeBugCheckEx not found\n");
        return STATUS_NOT_FOUND;
    }
    DbgPrint("[SmilePatch] KeBugCheckEx @ %p\n", anchor);

    /* ntoskrnl base and image size */
    PVOID ntBase = GetNtBase(anchor);
    if (!ntBase) {
        DbgPrint("[SmilePatch] ERROR: ntoskrnl base not found\n");
        return STATUS_NOT_FOUND;
    }
    ULONG peOff = *(ULONG*)((ULONG_PTR)ntBase + 0x3C);
    ULONG imgSize = *(ULONG*)((ULONG_PTR)ntBase + peOff + 0x50);
    DbgPrint("[SmilePatch] ntoskrnl base=%p  size=0x%X\n", ntBase, imgSize);

    /* find BgpFwDisplayBugCheckScreen */
    UCHAR prologMask[sizeof(g_BgpProlog)];
    RtlFillMemory(prologMask, sizeof(prologMask), 0xFF);
    PUCHAR bgpFunc = ScanMemory(
        (PUCHAR)ntBase, imgSize,
        g_BgpProlog, prologMask, sizeof(g_BgpProlog));
    if (bgpFunc)
        DbgPrint("[SmilePatch] BgpFwDisplayBugCheckScreen @ %p\n", bgpFunc);
    else
        DbgPrint("[SmilePatch] WARNING: BgpFwDisplayBugCheckScreen not found"
            " (prologue may differ on this build — continuing anyway)\n");

    /* scan for sad bytes :( */
    UCHAR mask[sizeof(g_SadBytes)];
    RtlFillMemory(mask, sizeof(mask), 0xFF);

    PUCHAR hit = ScanMemory(
        (PUCHAR)ntBase,
        imgSize,
        g_SadBytes,
        mask,
        sizeof(g_SadBytes)
    );

    if (hit == NULL)
    {
        DbgPrint("[SmilePatch] ':(' not found in ntoskrnl\n");
        DbgPrint("[SmilePatch] Check the Windows build.\n");
        return STATUS_NOT_FOUND;
    }

    DbgPrint("[SmilePatch] Found ':(' at %p (%02X %02X %02X %02X)\n", hit, hit[0], hit[1], hit[2], hit[3]);

    /* already patched this boot? */
    if (hit[2] == 0x29) {
        DbgPrint("[SmilePatch] Already ':)' — patch already applied this boot.\n");
        return STATUS_SUCCESS;
    }

    /* patch byte[2] from 0x28 '(' to 0x29 ')' via MDL */
    PMDL mdl = IoAllocateMdl(hit, sizeof(g_SadBytes), FALSE, FALSE, NULL);
    if (!mdl) {
        DbgPrint("[SmilePatch] ERROR: IoAllocateMdl failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    MmBuildMdlForNonPagedPool(mdl);
    mdl->MdlFlags |= MDL_MAPPED_TO_SYSTEM_VA;

    PUCHAR writable = (PUCHAR)MmMapLockedPagesSpecifyCache(
        mdl, KernelMode, MmNonCached, NULL, FALSE, NormalPagePriority);

    if (!writable) {
        DbgPrint("[SmilePatch] ERROR: MmMapLockedPagesSpecifyCache failed\n");
        IoFreeMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    writable[2] = 0x29;   /* ':(' -> ':)' */

    MmUnmapLockedPages(writable, mdl);
    IoFreeMdl(mdl);

    /* verify the original pointer */
    if (hit[2] == 0x29) {
        DbgPrint("[SmilePatch] SUCCESS: bytes now %02X %02X %02X %02X -> ':)'\n", hit[0], hit[1], hit[2], hit[3]);
        return STATUS_SUCCESS;
    }

    DbgPrint("[SmilePatch] WARNING: write did not take. Page may be protected by hypervisor.\n");

    return STATUS_UNSUCCESSFUL;
}

/* IRP */
NTSTATUS DispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS DispatchIoControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;

    if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_SMILEPATCH_APPLY) {
        DbgPrint("[SmilePatch] IOCTL_SMILEPATCH_APPLY received\n");
        status = PatchEmoticon();
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

VOID DriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symlink = RTL_CONSTANT_STRING(SYMLINK);
    IoDeleteSymbolicLink(&symlink);
    IoDeleteDevice(DriverObject->DeviceObject);
    DbgPrint("[SmilePatch] Unloaded.\n");
}

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNREFERENCED_PARAMETER(RegistryPath);
    DbgPrint("[SmilePatch] DriverEntry — creating device\n");

    UNICODE_STRING deviceName = RTL_CONSTANT_STRING(DEVICE_NAME);
    PDEVICE_OBJECT deviceObject = NULL;

    NTSTATUS status = IoCreateDevice(
        DriverObject, 0, &deviceName,
        FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
        FALSE, &deviceObject);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[SmilePatch] ERROR: IoCreateDevice: 0x%X\n", status);
        return status;
    }

    UNICODE_STRING symlink = RTL_CONSTANT_STRING(SYMLINK);
    status = IoCreateSymbolicLink(&symlink, &deviceName);
    if (!NT_SUCCESS(status)) {
        DbgPrint("[SmilePatch] ERROR: IoCreateSymbolicLink: 0x%X\n", status);
        IoDeleteDevice(deviceObject);
        return status;
    }

    DriverObject->DriverUnload = DriverUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchIoControl;

    DbgPrint("[SmilePatch] Device ready. Waiting for IOCTL from agent.\n");
    return STATUS_SUCCESS;
}

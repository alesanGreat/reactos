/* Temporary kernel-mode fixture for the RAMDISK I/O bounds investigation. */

#include <ntddk.h>
#include <ntdddisk.h>

#define RAMDISK_DEVICE_NAME L"\\Device\\Ramdisk{D9B257FC-684E-4DCB-AB79-03CFA2F6B750}"
#define RAMDISK_PROBE_TAG 'pRdR'

static
NTSTATUS
ProbeDeviceControl(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ ULONG ControlCode,
    _Out_writes_bytes_(OutputLength) PVOID OutputBuffer,
    _In_ ULONG OutputLength)
{
    KEVENT Event;
    IO_STATUS_BLOCK IoStatusBlock;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    RtlZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));

    Irp = IoBuildDeviceIoControlRequest(ControlCode,
                                        DeviceObject,
                                        NULL,
                                        0,
                                        OutputBuffer,
                                        OutputLength,
                                        FALSE,
                                        &Event,
                                        &IoStatusBlock);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        Status = IoStatusBlock.Status;
    }
    else if (NT_SUCCESS(Status) || NT_WARNING(Status))
    {
        Status = IoStatusBlock.Status;
    }

    return Status;
}

static
NTSTATUS
ProbeRead(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_z_ PCSTR Name,
    _In_ LONGLONG Offset,
    _In_ ULONG Length)
{
    KEVENT Event;
    IO_STATUS_BLOCK IoStatusBlock;
    LARGE_INTEGER ByteOffset;
    PIRP Irp;
    PVOID Buffer;
    NTSTATUS Status;

    Buffer = ExAllocatePoolWithTag(NonPagedPool, Length, RAMDISK_PROBE_TAG);
    if (!Buffer)
    {
        DbgPrint("RAMDISK_PROBE_KERNEL_READ_ALLOC_FAIL name=%s length=%lu\r\n",
                 Name,
                 Length);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    RtlZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));
    ByteOffset.QuadPart = Offset;

    Irp = IoBuildSynchronousFsdRequest(IRP_MJ_READ,
                                       DeviceObject,
                                       Buffer,
                                       Length,
                                       &ByteOffset,
                                       &Event,
                                       &IoStatusBlock);
    if (!Irp)
    {
        ExFreePoolWithTag(Buffer, RAMDISK_PROBE_TAG);
        DbgPrint("RAMDISK_PROBE_KERNEL_READ_IRP_FAIL name=%s length=%lu\r\n",
                 Name,
                 Length);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = IoCallDriver(DeviceObject, Irp);
    if (Status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event,
                              Executive,
                              KernelMode,
                              FALSE,
                              NULL);
        Status = IoStatusBlock.Status;
    }
    else if (NT_SUCCESS(Status) || NT_WARNING(Status))
    {
        Status = IoStatusBlock.Status;
    }

    DbgPrint("RAMDISK_PROBE_KERNEL_READ name=%s offset=%I64d length=%lu status=0x%08lx iosb=0x%08lx bytes=%Iu\r\n",
             Name,
             Offset,
             Length,
             Status,
             IoStatusBlock.Status,
             IoStatusBlock.Information);

    ExFreePoolWithTag(Buffer, RAMDISK_PROBE_TAG);
    return Status;
}

static
VOID
NTAPI
RamdiskProbeUnload(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);
}

NTSTATUS
NTAPI
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(RAMDISK_DEVICE_NAME);
    PFILE_OBJECT FileObject = NULL;
    PDEVICE_OBJECT TopDeviceObject = NULL;
    PDEVICE_OBJECT BaseDeviceObject = NULL;
    GET_LENGTH_INFORMATION LengthInfo;
    DISK_GEOMETRY Geometry;
    LONGLONG DiskLength;
    ULONG SectorSize;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(RegistryPath);
    DriverObject->DriverUnload = RamdiskProbeUnload;

    DbgPrint("RAMDISK_PROBE_KERNEL_ENTRY\r\n");

    /*
     * IoGetDeviceObjectPointer returns the related/top device object for the
     * named RAMDISK.  Deliberately walk back to the base of that attachment
     * stack so these fixture IRPs reach ramdisk.sys rather than the mounted
     * filesystem that normal NtReadFile calls traverse.
     */
    Status = IoGetDeviceObjectPointer(&DeviceName,
                                      READ_CONTROL,
                                      &FileObject,
                                      &TopDeviceObject);
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("RAMDISK_PROBE_KERNEL_OPEN_FAIL status=0x%08lx\r\n", Status);
        DbgPrint("RAMDISK_PROBE_KERNEL_ABORT\r\n");
        return Status;
    }

    BaseDeviceObject = IoGetDeviceAttachmentBaseRef(TopDeviceObject);
    ObDereferenceObject(FileObject);
    FileObject = NULL;

    if (!BaseDeviceObject)
    {
        DbgPrint("RAMDISK_PROBE_KERNEL_BASE_FAIL\r\n");
        DbgPrint("RAMDISK_PROBE_KERNEL_ABORT\r\n");
        return STATUS_NO_SUCH_DEVICE;
    }

    DbgPrint("RAMDISK_PROBE_KERNEL_TARGET top=%p base=%p driver=%wZ type=%lu flags=0x%08lx stack=%u\r\n",
             TopDeviceObject,
             BaseDeviceObject,
             &BaseDeviceObject->DriverObject->DriverName,
             BaseDeviceObject->DeviceType,
             BaseDeviceObject->Flags,
             BaseDeviceObject->StackSize);

    RtlZeroMemory(&LengthInfo, sizeof(LengthInfo));
    Status = ProbeDeviceControl(BaseDeviceObject,
                                IOCTL_DISK_GET_LENGTH_INFO,
                                &LengthInfo,
                                sizeof(LengthInfo));
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("RAMDISK_PROBE_KERNEL_LENGTH_FAIL status=0x%08lx\r\n", Status);
        DbgPrint("RAMDISK_PROBE_KERNEL_ABORT\r\n");
        ObDereferenceObject(BaseDeviceObject);
        return Status;
    }

    RtlZeroMemory(&Geometry, sizeof(Geometry));
    Status = ProbeDeviceControl(BaseDeviceObject,
                                IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                &Geometry,
                                sizeof(Geometry));
    if (!NT_SUCCESS(Status))
    {
        DbgPrint("RAMDISK_PROBE_KERNEL_GEOMETRY_FAIL status=0x%08lx\r\n", Status);
        DbgPrint("RAMDISK_PROBE_KERNEL_ABORT\r\n");
        ObDereferenceObject(BaseDeviceObject);
        return Status;
    }

    DiskLength = LengthInfo.Length.QuadPart;
    SectorSize = Geometry.BytesPerSector;
    DbgPrint("RAMDISK_PROBE_KERNEL_INFO length=%I64d sector=%lu media=%u\r\n",
             DiskLength,
             SectorSize,
             Geometry.MediaType);

    if ((DiskLength <= 0) ||
        (SectorSize == 0) ||
        (SectorSize > 4096) ||
        (DiskLength < 4 * (LONGLONG)SectorSize))
    {
        DbgPrint("RAMDISK_PROBE_KERNEL_INVALID_GEOMETRY\r\n");
        DbgPrint("RAMDISK_PROBE_KERNEL_ABORT\r\n");
        ObDereferenceObject(BaseDeviceObject);
        return STATUS_INVALID_PARAMETER;
    }

    ProbeRead(BaseDeviceObject,
              "last-sector",
              DiskLength - SectorSize,
              SectorSize);
    ProbeRead(BaseDeviceObject,
              "at-eof",
              DiskLength,
              SectorSize);
    ProbeRead(BaseDeviceObject,
              "cross-eof",
              DiskLength - SectorSize,
              SectorSize * 2);
    ProbeRead(BaseDeviceObject,
              "misaligned-offset",
              SectorSize + 1,
              SectorSize);
    ProbeRead(BaseDeviceObject,
              "misaligned-length",
              SectorSize,
              SectorSize + 1);

    ObDereferenceObject(BaseDeviceObject);
    DbgPrint("RAMDISK_PROBE_DONE\r\n");
    return STATUS_SUCCESS;
}

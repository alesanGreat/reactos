/* Temporary runtime probe for RAMDISK create-failure rollback. */

#define WIN32_NO_STATUS
#include <windows.h>
#define NTOS_MODE_USER
#include <ndk/iofuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>
#include <reactos/drivers/ntddrdsk.h>
#include <stdio.h>
#include <stdarg.h>

#define PROBE_DEVICE_NAME L"\\Device\\Ramdisk{A1E5C10A-1F52-4AA8-9A44-E56AC942CAFE}"
#define PROBE_LINK_NAME L"\\GLOBAL??\\Ramdisk{A1E5C10A-1F52-4AA8-9A44-E56AC942CAFE}"

static HANDLE LogHandle = INVALID_HANDLE_VALUE;

static VOID
LogLine(const char *Format, ...)
{
    CHAR Line[512];
    DWORD Written;
    va_list Args;
    int Length;

    va_start(Args, Format);
    Length = _vsnprintf(Line, sizeof(Line) - 1, Format, Args);
    va_end(Args);
    if (Length < 0)
        Length = sizeof(Line) - 1;
    Line[Length] = '\0';

    DbgPrint("%s", Line);
    if (LogHandle != INVALID_HANDLE_VALUE)
    {
        WriteFile(LogHandle, Line, (DWORD)Length, &Written, NULL);
        FlushFileBuffers(LogHandle);
    }
}

static NTSTATUS
OpenNativeFile(PCWSTR Name, PHANDLE Handle)
{
    UNICODE_STRING ObjectName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;

    RtlInitUnicodeString(&ObjectName, Name);
    InitializeObjectAttributes(&ObjectAttributes,
                               &ObjectName,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);
    RtlZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));
    *Handle = NULL;
    return NtOpenFile(Handle,
                      FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                      &ObjectAttributes,
                      &IoStatusBlock,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                      FILE_SYNCHRONOUS_IO_NONALERT);
}

static NTSTATUS
OpenNativeLink(PCWSTR Name, PHANDLE Handle)
{
    UNICODE_STRING ObjectName;
    OBJECT_ATTRIBUTES ObjectAttributes;

    RtlInitUnicodeString(&ObjectName, Name);
    InitializeObjectAttributes(&ObjectAttributes,
                               &ObjectName,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);
    *Handle = NULL;
    return NtOpenSymbolicLinkObject(Handle, SYMBOLIC_LINK_QUERY, &ObjectAttributes);
}

int
main(void)
{
    static const GUID ProbeGuid =
        {0xA1E5C10A, 0x1F52, 0x4AA8, {0x9A, 0x44, 0xE5, 0x6A, 0xC9, 0x42, 0xCA, 0xFE}};
    UNICODE_STRING BusName = RTL_CONSTANT_STRING(L"\\Device\\Ramdisk");
    OBJECT_ATTRIBUTES ObjectAttributes;
    IO_STATUS_BLOCK IoStatusBlock;
    RAMDISK_CREATE_INPUT Input;
    HANDLE BusHandle = NULL;
    HANDLE ObjectHandle = NULL;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    NTSTATUS DeviceStatus = STATUS_UNSUCCESSFUL;
    NTSTATUS LinkStatus = STATUS_UNSUCCESSFUL;
    int Result = 1;

    DbgPrint("RAMDISK_CREATE_PROBE_ENTRY\r\n");
    LogHandle = CreateFileA("\\\\.\\COM2",
                            GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL,
                            OPEN_EXISTING,
                            0,
                            NULL);
    LogLine("RAMDISK_CREATE_PROBE_BEGIN\r\n");

    InitializeObjectAttributes(&ObjectAttributes,
                               &BusName,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);
    RtlZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));
    Status = NtOpenFile(&BusHandle,
                        FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
    {
        LogLine("RAMDISK_CREATE_PROBE_BUS_OPEN_FAIL status=0x%08lx\r\n", Status);
        goto Finish;
    }

    RtlZeroMemory(&Input, sizeof(Input));
    Input.Version = sizeof(Input);
    Input.DiskGuid = ProbeGuid;
    Input.DiskType = RAMDISK_BOOT_DISK;
    Input.DiskLength.QuadPart = 16 * 1024 * 1024;
    Input.BasePage = 1;
    Input.DriveLetter = L'Z';

    RtlZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));
    Status = NtDeviceIoControlFile(BusHandle,
                                   NULL,
                                   NULL,
                                   NULL,
                                   &IoStatusBlock,
                                   FSCTL_CREATE_RAM_DISK,
                                   &Input,
                                   sizeof(Input),
                                   NULL,
                                   0);
    if (Status == STATUS_PENDING)
    {
        WaitForSingleObject(BusHandle, INFINITE);
        Status = IoStatusBlock.Status;
    }
    else if (NT_SUCCESS(Status) || NT_WARNING(Status))
    {
        Status = IoStatusBlock.Status;
    }

    LogLine("RAMDISK_CREATE_PROBE_REQUEST status=0x%08lx iosb=0x%08lx\r\n",
            Status,
            IoStatusBlock.Status);
    NtClose(BusHandle);
    BusHandle = NULL;

    DeviceStatus = OpenNativeFile(PROBE_DEVICE_NAME, &ObjectHandle);
    if (NT_SUCCESS(DeviceStatus))
    {
        NtClose(ObjectHandle);
        ObjectHandle = NULL;
    }
    LogLine("RAMDISK_CREATE_PROBE_DEVICE_OPEN status=0x%08lx\r\n", DeviceStatus);

    LinkStatus = OpenNativeLink(PROBE_LINK_NAME, &ObjectHandle);
    if (NT_SUCCESS(LinkStatus))
    {
        NtClose(ObjectHandle);
        ObjectHandle = NULL;
    }
    LogLine("RAMDISK_CREATE_PROBE_LINK_OPEN status=0x%08lx\r\n", LinkStatus);

    if ((Status == STATUS_INSUFFICIENT_RESOURCES) &&
        !NT_SUCCESS(DeviceStatus) &&
        !NT_SUCCESS(LinkStatus))
    {
        LogLine("RAMDISK_CREATE_PROBE_RESULT cleaned\r\n");
        Result = 0;
    }
    else if (NT_SUCCESS(DeviceStatus) || NT_SUCCESS(LinkStatus))
    {
        LogLine("RAMDISK_CREATE_PROBE_RESULT leaked\r\n");
    }
    else
    {
        LogLine("RAMDISK_CREATE_PROBE_RESULT unexpected\r\n");
    }

Finish:
    if (BusHandle != NULL)
        NtClose(BusHandle);
    if (ObjectHandle != NULL)
        NtClose(ObjectHandle);

    LogLine("RAMDISK_CREATE_PROBE_OBSERVED\r\n");
    if (LogHandle != INVALID_HANDLE_VALUE)
        CloseHandle(LogHandle);

    return Result;
}

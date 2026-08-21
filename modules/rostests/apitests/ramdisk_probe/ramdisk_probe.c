/* Temporary runtime probe for the RAMDISK I/O bounds investigation. */

#define WIN32_NO_STATUS
#include <windows.h>
#define NTOS_MODE_USER
#include <ndk/iofuncs.h>
#include <ndk/obfuncs.h>
#include <ndk/rtlfuncs.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdarg.h>

static HANDLE LogHandle = INVALID_HANDLE_VALUE;
static BYTE Buffer[8192];

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

    /* COM1 carries the kernel debugger, so always mirror probe telemetry there. */
    DbgPrint("%s", Line);

    if (LogHandle != INVALID_HANDLE_VALUE)
    {
        WriteFile(LogHandle, Line, (DWORD)Length, &Written, NULL);
        FlushFileBuffers(LogHandle);
    }
}

static VOID
ProbeRead(HANDLE DiskHandle,
          const char *Name,
          LONGLONG Offset,
          DWORD Length)
{
    LARGE_INTEGER Position;
    IO_STATUS_BLOCK IoStatusBlock;
    NTSTATUS Status;

    Position.QuadPart = Offset;
    RtlZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));
    Status = NtReadFile(DiskHandle,
                        NULL,
                        NULL,
                        NULL,
                        &IoStatusBlock,
                        Buffer,
                        Length,
                        &Position,
                        NULL);
    if (Status == STATUS_PENDING)
    {
        WaitForSingleObject(DiskHandle, INFINITE);
        Status = IoStatusBlock.Status;
    }

    LogLine("RAMDISK_PROBE_READ name=%s offset=%I64d length=%lu status=0x%08lx iosb=0x%08lx bytes=%Iu\r\n",
            Name,
            Offset,
            Length,
            Status,
            IoStatusBlock.Status,
            IoStatusBlock.Information);
}

static BOOL
LoadKernelProbe(VOID)
{
    WCHAR DriverPath[MAX_PATH];
    SC_HANDLE ScmHandle;
    SC_HANDLE ServiceHandle;
    DWORD Error;
    UINT Length;

    Length = GetSystemDirectoryW(DriverPath, ARRAYSIZE(DriverPath));
    if ((Length == 0) || (Length >= ARRAYSIZE(DriverPath)))
    {
        LogLine("RAMDISK_PROBE_KERNEL_PATH_FAIL error=%lu\r\n", GetLastError());
        return FALSE;
    }

    if (lstrlenW(DriverPath) + lstrlenW(L"\\drivers\\ramdisk_probe_drv.sys") + 1 >= ARRAYSIZE(DriverPath))
    {
        LogLine("RAMDISK_PROBE_KERNEL_PATH_TOO_LONG\r\n");
        return FALSE;
    }
    lstrcatW(DriverPath, L"\\drivers\\ramdisk_probe_drv.sys");

    ScmHandle = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!ScmHandle)
    {
        LogLine("RAMDISK_PROBE_SCM_FAIL error=%lu\r\n", GetLastError());
        return FALSE;
    }

    ServiceHandle = CreateServiceW(ScmHandle,
                                   L"RamdiskProbeFixture",
                                   L"RAMDISK Probe Fixture",
                                   SERVICE_START | SERVICE_QUERY_STATUS | DELETE,
                                   SERVICE_KERNEL_DRIVER,
                                   SERVICE_DEMAND_START,
                                   SERVICE_ERROR_NORMAL,
                                   DriverPath,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL,
                                   NULL);
    if (!ServiceHandle)
    {
        Error = GetLastError();
        if (Error == ERROR_SERVICE_EXISTS)
        {
            ServiceHandle = OpenServiceW(ScmHandle,
                                         L"RamdiskProbeFixture",
                                         SERVICE_START | SERVICE_QUERY_STATUS | DELETE);
        }
    }

    if (!ServiceHandle)
    {
        LogLine("RAMDISK_PROBE_SERVICE_CREATE_FAIL error=%lu path=%S\r\n",
                GetLastError(),
                DriverPath);
        CloseServiceHandle(ScmHandle);
        return FALSE;
    }

    LogLine("RAMDISK_PROBE_KERNEL_START path=%S\r\n", DriverPath);
    if (!StartServiceW(ServiceHandle, 0, NULL))
    {
        Error = GetLastError();
        if (Error != ERROR_SERVICE_ALREADY_RUNNING)
        {
            LogLine("RAMDISK_PROBE_SERVICE_START_FAIL error=%lu\r\n", Error);
            CloseServiceHandle(ServiceHandle);
            CloseServiceHandle(ScmHandle);
            return FALSE;
        }
    }

    CloseServiceHandle(ServiceHandle);
    CloseServiceHandle(ScmHandle);
    return TRUE;
}

int
main(void)
{
    HANDLE DiskHandle = NULL;
    GET_LENGTH_INFORMATION LengthInfo;
    DISK_GEOMETRY Geometry;
    IO_STATUS_BLOCK IoStatusBlock;
    UNICODE_STRING DeviceName;
    OBJECT_ATTRIBUTES ObjectAttributes;
    NTSTATUS Status;
    DWORD SectorSize;
    LONGLONG DiskLength;

    DbgPrint("RAMDISK_PROBE_ENTRY\r\n");

    LogHandle = CreateFileA("\\\\.\\COM2",
                            GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL,
                            OPEN_EXISTING,
                            0,
                            NULL);
    if (LogHandle == INVALID_HANDLE_VALUE)
        DbgPrint("RAMDISK_PROBE_COM2_FAIL error=%lu\r\n", GetLastError());

    LogLine("RAMDISK_PROBE_BEGIN\r\n");

    RtlInitUnicodeString(&DeviceName,
                         L"\\Device\\Ramdisk{D9B257FC-684E-4DCB-AB79-03CFA2F6B750}");
    InitializeObjectAttributes(&ObjectAttributes,
                               &DeviceName,
                               OBJ_CASE_INSENSITIVE,
                               NULL,
                               NULL);
    RtlZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));
    Status = NtOpenFile(&DiskHandle,
                        FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                        &ObjectAttributes,
                        &IoStatusBlock,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_SYNCHRONOUS_IO_NONALERT);
    if (!NT_SUCCESS(Status))
    {
        LogLine("RAMDISK_PROBE_OPEN_FAIL status=0x%08lx iosb=0x%08lx\r\n",
                Status, IoStatusBlock.Status);
        LogLine("RAMDISK_PROBE_ABORT\r\n");
        if (LogHandle != INVALID_HANDLE_VALUE) CloseHandle(LogHandle);
        return 3;
    }

    RtlZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));
    Status = NtDeviceIoControlFile(DiskHandle,
                                   NULL,
                                   NULL,
                                   NULL,
                                   &IoStatusBlock,
                                   IOCTL_DISK_GET_LENGTH_INFO,
                                   NULL,
                                   0,
                                   &LengthInfo,
                                   sizeof(LengthInfo));
    if (Status == STATUS_PENDING)
    {
        WaitForSingleObject(DiskHandle, INFINITE);
        Status = IoStatusBlock.Status;
    }
    if (!NT_SUCCESS(Status))
    {
        LogLine("RAMDISK_PROBE_LENGTH_FAIL status=0x%08lx iosb=0x%08lx\r\n",
                Status, IoStatusBlock.Status);
        NtClose(DiskHandle);
        LogLine("RAMDISK_PROBE_ABORT\r\n");
        if (LogHandle != INVALID_HANDLE_VALUE) CloseHandle(LogHandle);
        return 4;
    }

    RtlZeroMemory(&IoStatusBlock, sizeof(IoStatusBlock));
    Status = NtDeviceIoControlFile(DiskHandle,
                                   NULL,
                                   NULL,
                                   NULL,
                                   &IoStatusBlock,
                                   IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                   NULL,
                                   0,
                                   &Geometry,
                                   sizeof(Geometry));
    if (Status == STATUS_PENDING)
    {
        WaitForSingleObject(DiskHandle, INFINITE);
        Status = IoStatusBlock.Status;
    }
    if (!NT_SUCCESS(Status))
    {
        LogLine("RAMDISK_PROBE_GEOMETRY_FAIL status=0x%08lx iosb=0x%08lx\r\n",
                Status, IoStatusBlock.Status);
        NtClose(DiskHandle);
        LogLine("RAMDISK_PROBE_ABORT\r\n");
        if (LogHandle != INVALID_HANDLE_VALUE) CloseHandle(LogHandle);
        return 5;
    }

    DiskLength = LengthInfo.Length.QuadPart;
    SectorSize = Geometry.BytesPerSector;
    LogLine("RAMDISK_PROBE_INFO length=%I64d sector=%lu media=%u\r\n",
            DiskLength, SectorSize, Geometry.MediaType);

    if ((DiskLength <= 0) ||
        (SectorSize == 0) ||
        (SectorSize > sizeof(Buffer) / 2) ||
        (DiskLength < 4 * (LONGLONG)SectorSize))
    {
        LogLine("RAMDISK_PROBE_INVALID_GEOMETRY\r\n");
        NtClose(DiskHandle);
        LogLine("RAMDISK_PROBE_ABORT\r\n");
        if (LogHandle != INVALID_HANDLE_VALUE) CloseHandle(LogHandle);
        return 6;
    }

    ProbeRead(DiskHandle, "last-sector", DiskLength - SectorSize, SectorSize);
    ProbeRead(DiskHandle, "at-eof", DiskLength, SectorSize);
    ProbeRead(DiskHandle, "cross-eof", DiskLength - SectorSize, SectorSize * 2);
    ProbeRead(DiskHandle, "misaligned-offset", SectorSize + 1, SectorSize);
    ProbeRead(DiskHandle, "misaligned-length", SectorSize, SectorSize + 1);

    NtClose(DiskHandle);
    LogLine("RAMDISK_PROBE_USER_DONE\r\n");

    /*
     * NtReadFile reaches the mounted filesystem/top attached device for this
     * boot RAMDISK.  Load a temporary kernel fixture to issue the same reads
     * directly to the base RAMDISK device object.  Only that kernel fixture
     * emits RAMDISK_PROBE_DONE after its five direct IRPs complete.
     */
    if (!LoadKernelProbe())
    {
        LogLine("RAMDISK_PROBE_ABORT\r\n");
        if (LogHandle != INVALID_HANDLE_VALUE) CloseHandle(LogHandle);
        return 7;
    }

    LogLine("RAMDISK_PROBE_KERNEL_LOAD_OK\r\n");
    if (LogHandle != INVALID_HANDLE_VALUE) CloseHandle(LogHandle);
    Sleep(INFINITE);
    return 0;
}

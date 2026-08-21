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

ULONG __cdecl DbgPrint(IN PCH Format, IN ...);

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
    LogLine("RAMDISK_PROBE_DONE\r\n");
    if (LogHandle != INVALID_HANDLE_VALUE) CloseHandle(LogHandle);
    Sleep(INFINITE);
    return 0;
}

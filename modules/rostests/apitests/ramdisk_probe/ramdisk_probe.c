/* Temporary runtime probe for the RAMDISK I/O bounds investigation. */

#include <windows.h>
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

    WriteFile(LogHandle, Line, (DWORD)Length, &Written, NULL);
    FlushFileBuffers(LogHandle);
}

static VOID
ProbeRead(HANDLE DiskHandle,
          const char *Name,
          LONGLONG Offset,
          DWORD Length)
{
    LARGE_INTEGER Position;
    DWORD BytesRead = 0;
    DWORD Error;
    BOOL Success;

    Position.QuadPart = Offset;
    SetLastError(ERROR_SUCCESS);
    Success = SetFilePointerEx(DiskHandle, Position, NULL, FILE_BEGIN);
    if (!Success)
    {
        Error = GetLastError();
        LogLine("RAMDISK_PROBE_READ name=%s offset=%I64d length=%lu seek=0 error=%lu\r\n",
                Name, Offset, Length, Error);
        return;
    }

    SetLastError(ERROR_SUCCESS);
    Success = ReadFile(DiskHandle, Buffer, Length, &BytesRead, NULL);
    Error = Success ? ERROR_SUCCESS : GetLastError();

    LogLine("RAMDISK_PROBE_READ name=%s offset=%I64d length=%lu ok=%u error=%lu bytes=%lu\r\n",
            Name, Offset, Length, Success, Error, BytesRead);
}

int
main(void)
{
    HANDLE DiskHandle;
    GET_LENGTH_INFORMATION LengthInfo;
    DISK_GEOMETRY Geometry;
    DWORD BytesReturned;
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
        return 2;

    LogLine("RAMDISK_PROBE_BEGIN\r\n");

    DiskHandle = CreateFileW(L"\\\\.\\X:",
                             GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL,
                             OPEN_EXISTING,
                             0,
                             NULL);
    if (DiskHandle == INVALID_HANDLE_VALUE)
    {
        LogLine("RAMDISK_PROBE_OPEN_FAIL error=%lu\r\n", GetLastError());
        LogLine("RAMDISK_PROBE_DONE\r\n");
        CloseHandle(LogHandle);
        return 3;
    }

    if (!DeviceIoControl(DiskHandle,
                         IOCTL_DISK_GET_LENGTH_INFO,
                         NULL,
                         0,
                         &LengthInfo,
                         sizeof(LengthInfo),
                         &BytesReturned,
                         NULL))
    {
        LogLine("RAMDISK_PROBE_LENGTH_FAIL error=%lu\r\n", GetLastError());
        CloseHandle(DiskHandle);
        LogLine("RAMDISK_PROBE_DONE\r\n");
        CloseHandle(LogHandle);
        return 4;
    }

    if (!DeviceIoControl(DiskHandle,
                         IOCTL_DISK_GET_DRIVE_GEOMETRY,
                         NULL,
                         0,
                         &Geometry,
                         sizeof(Geometry),
                         &BytesReturned,
                         NULL))
    {
        LogLine("RAMDISK_PROBE_GEOMETRY_FAIL error=%lu\r\n", GetLastError());
        CloseHandle(DiskHandle);
        LogLine("RAMDISK_PROBE_DONE\r\n");
        CloseHandle(LogHandle);
        return 5;
    }

    DiskLength = LengthInfo.Length.QuadPart;
    SectorSize = Geometry.BytesPerSector;
    LogLine("RAMDISK_PROBE_INFO length=%I64d sector=%lu media=%u\r\n",
            DiskLength, SectorSize, Geometry.MediaType);

    if ((DiskLength <= 0) ||
        (SectorSize == 0) ||
        (SectorSize > sizeof(Buffer) / 2) ||
        (DiskLength < SectorSize))
    {
        LogLine("RAMDISK_PROBE_INVALID_GEOMETRY\r\n");
    }
    else
    {
        ProbeRead(DiskHandle, "last-sector", DiskLength - SectorSize, SectorSize);
        ProbeRead(DiskHandle, "at-eof", DiskLength, SectorSize);
        ProbeRead(DiskHandle, "cross-eof", DiskLength - SectorSize, SectorSize * 2);
        ProbeRead(DiskHandle, "misaligned-offset", DiskLength - SectorSize + 1, SectorSize);
        ProbeRead(DiskHandle, "misaligned-length", DiskLength - SectorSize, SectorSize + 1);
    }

    CloseHandle(DiskHandle);
    LogLine("RAMDISK_PROBE_DONE\r\n");
    CloseHandle(LogHandle);
    Sleep(INFINITE);
    return 0;
}

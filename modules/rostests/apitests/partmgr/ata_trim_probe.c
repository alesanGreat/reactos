/* Temporary ATA TRIM runtime probe. Not intended for upstream. */

#include <windows.h>
#include <winioctl.h>
#include <ntddstor.h>
#include <stdio.h>
#include <string.h>

#define PROBE_OFFSET       (16LL * 1024 * 1024)
#define PROBE_LENGTH       (1UL * 1024 * 1024)
#define WRITE_CHUNK_SIZE   (64UL * 1024)
#define RANGE_OFFSET       ((sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES) + 7) & ~7)

int
main(void)
{
    STORAGE_PROPERTY_QUERY Query;
    DEVICE_TRIM_DESCRIPTOR TrimDescriptor;
    PDEVICE_MANAGE_DATA_SET_ATTRIBUTES Attributes;
    PDEVICE_DATA_SET_RANGE Range;
    LARGE_INTEGER Offset;
    HANDLE Disk;
    PBYTE Buffer;
    PBYTE DsmBuffer;
    DWORD DsmSize;
    DWORD Bytes;
    DWORD Written;
    DWORD TotalWritten = 0;
    BOOL Result;

    Disk = CreateFileW(L"\\\\.\\PhysicalDrive1",
                       GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL,
                       OPEN_EXISTING,
                       FILE_FLAG_WRITE_THROUGH,
                       NULL);
    if (Disk == INVALID_HANDLE_VALUE)
    {
        printf("TRIM_PROBE_OPEN_FAIL error=%lu\n", GetLastError());
        return 1;
    }

    ZeroMemory(&Query, sizeof(Query));
    Query.PropertyId = StorageDeviceTrimProperty;
    Query.QueryType = PropertyStandardQuery;
    ZeroMemory(&TrimDescriptor, sizeof(TrimDescriptor));

    Result = DeviceIoControl(Disk,
                             IOCTL_STORAGE_QUERY_PROPERTY,
                             &Query,
                             sizeof(Query),
                             &TrimDescriptor,
                             sizeof(TrimDescriptor),
                             &Bytes,
                             NULL);
    if (!Result)
    {
        printf("TRIM_PROBE_QUERY_FAIL error=%lu\n", GetLastError());
        CloseHandle(Disk);
        return 2;
    }

    printf("TRIM_PROBE_TRIM_ENABLED=%u\n", TrimDescriptor.TrimEnabled);
    if (!TrimDescriptor.TrimEnabled)
    {
        CloseHandle(Disk);
        return 3;
    }

    Buffer = HeapAlloc(GetProcessHeap(), 0, WRITE_CHUNK_SIZE);
    if (!Buffer)
    {
        CloseHandle(Disk);
        return 4;
    }
    memset(Buffer, 0xA5, WRITE_CHUNK_SIZE);

    Offset.QuadPart = PROBE_OFFSET;
    if (!SetFilePointerEx(Disk, Offset, NULL, FILE_BEGIN))
    {
        printf("TRIM_PROBE_SEEK_FAIL error=%lu\n", GetLastError());
        HeapFree(GetProcessHeap(), 0, Buffer);
        CloseHandle(Disk);
        return 5;
    }

    while (TotalWritten < PROBE_LENGTH)
    {
        Result = WriteFile(Disk, Buffer, WRITE_CHUNK_SIZE, &Written, NULL);
        if (!Result || Written != WRITE_CHUNK_SIZE)
        {
            printf("TRIM_PROBE_WRITE_FAIL error=%lu written=%lu total=%lu\n",
                   GetLastError(), Written, TotalWritten);
            HeapFree(GetProcessHeap(), 0, Buffer);
            CloseHandle(Disk);
            return 6;
        }
        TotalWritten += Written;
    }

    if (!FlushFileBuffers(Disk))
    {
        printf("TRIM_PROBE_FLUSH_FAIL error=%lu\n", GetLastError());
        HeapFree(GetProcessHeap(), 0, Buffer);
        CloseHandle(Disk);
        return 7;
    }

    printf("TRIM_PROBE_WRITE_OK offset=%I64d length=%lu\n",
           (LONGLONG)PROBE_OFFSET, PROBE_LENGTH);

    DsmSize = RANGE_OFFSET + sizeof(DEVICE_DATA_SET_RANGE);
    DsmBuffer = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DsmSize);
    if (!DsmBuffer)
    {
        HeapFree(GetProcessHeap(), 0, Buffer);
        CloseHandle(Disk);
        return 8;
    }

    Attributes = (PDEVICE_MANAGE_DATA_SET_ATTRIBUTES)DsmBuffer;
    Attributes->Size = sizeof(*Attributes);
    Attributes->Action = DeviceDsmAction_Trim;
    Attributes->Flags = 0;
    Attributes->ParameterBlockOffset = 0;
    Attributes->ParameterBlockLength = 0;
    Attributes->DataSetRangesOffset = RANGE_OFFSET;
    Attributes->DataSetRangesLength = sizeof(DEVICE_DATA_SET_RANGE);

    Range = (PDEVICE_DATA_SET_RANGE)(DsmBuffer + RANGE_OFFSET);
    Range->StartingOffset = PROBE_OFFSET;
    Range->LengthInBytes = PROBE_LENGTH;

    Result = DeviceIoControl(Disk,
                             IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES,
                             DsmBuffer,
                             DsmSize,
                             NULL,
                             0,
                             &Bytes,
                             NULL);
    if (!Result)
    {
        printf("TRIM_PROBE_TRIM_FAIL error=%lu\n", GetLastError());
        HeapFree(GetProcessHeap(), 0, DsmBuffer);
        HeapFree(GetProcessHeap(), 0, Buffer);
        CloseHandle(Disk);
        return 9;
    }

    printf("TRIM_PROBE_TRIM_OK offset=%I64d length=%lu\n",
           (LONGLONG)PROBE_OFFSET, PROBE_LENGTH);

    HeapFree(GetProcessHeap(), 0, DsmBuffer);
    HeapFree(GetProcessHeap(), 0, Buffer);
    CloseHandle(Disk);
    printf("TRIM_PROBE_DONE\n");
    return 0;
}

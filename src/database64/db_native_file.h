#pragma once
#include <windows.h>
#include <stdint.h>

// Buffered overlapped I/O permits the 12-byte prefix and subsequent unaligned offsets.
static inline bool DB64_ReadNativeFile(HANDLE file, HANDLE event, uint64_t *offset, void *buffer,
                                      DWORD size, DWORD *bytes)
{
    OVERLAPPED read = {};
    read.Offset = (DWORD)*offset;
    read.OffsetHigh = (DWORD)(*offset >> 32);
    read.hEvent = event;
    ResetEvent(event);
    *bytes = 0;
    if (!ReadFile(file, buffer, size, bytes, &read))
    {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING)
        {
            error = GetOverlappedResult(file, &read, bytes, TRUE) ? ERROR_SUCCESS : GetLastError();
        }
        if (error == ERROR_HANDLE_EOF)
        {
            *bytes = 0;
            return true;
        }
        if (error != ERROR_SUCCESS)
        {
            return false;
        }
    }
    *offset += *bytes;
    return true;
}

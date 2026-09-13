#include <universal/q_shared.h>
#include <universal/com_files.h>
#include <msslib/mss.h>
#include <database64/db_memory_stream.h>
#if defined(_WIN64) && !defined(KIWI_RAW_ONLY)
#include <database64/database.h>
#include <database64/db_package.h>
#endif

struct MSSFileHandle
{
    DB64MemoryStream *memory;
    int file;
};

uint __stdcall MSS_FileOpenCallback(const MSS_FILE *pszFilename, UINTa *phFileHandle)
{
    if (!pszFilename || !phFileHandle)
    {
        return 0;
    }
    *phFileHandle = 0;
    MSSFileHandle *handle = (MSSFileHandle *)calloc(1, sizeof(MSSFileHandle));
    if (!handle)
    {
        return 0;
    }
#if defined(_WIN64) && !defined(KIWI_RAW_ONLY)
    char normalized[DB64_PACKAGE_PATH];
    if (IsFastFileLoad() && DB64_NormalizePath(pszFilename, normalized, sizeof(normalized)))
    {
        handle->memory = DB64_OpenRawFileStream(normalized);
    }
#endif
    const int fileSize = handle->memory ? 0 : (int)FS_FOpenFileReadStream(pszFilename, &handle->file);
    if (fileSize < 0)
    {
        free(handle);
        return 0;
    }
    *phFileHandle = (UINTa)handle;
    return 1;
}
void __stdcall MSS_FileCloseCallback(UINTa hFileHandle)
{
    MSSFileHandle *handle = (MSSFileHandle *)hFileHandle;
    if (!handle)
    {
        return;
    }
    if (handle->memory)
    {
        free(handle->memory);
    }
    else
    {
        FS_FCloseFile(handle->file);
    }
    free(handle);
}
int __stdcall MSS_FileSeekCallback(UINTa hFileHandle, int offset, uint type)
{
    MSSFileHandle *handle = (MSSFileHandle *)hFileHandle;
    if (!handle || type > 2)
    {
        return -1;
    }
    if (handle->memory)
    {
        return DB64_SeekMemoryStream(handle->memory, offset, type);
    }
    if (type)
    {
        if (type == 1)
        {
            FS_Seek(handle->file, offset, 0);
        }
        else
        {
            if (type != 2)
            {
                return 0;
            }
            FS_Seek(handle->file, offset, 1);
        }
    }
    else
    {
        FS_Seek(handle->file, offset, 2);
    }
    return FS_FTell(handle->file);
}
uint __stdcall MSS_FileReadCallback(UINTa hFileHandle, void *pBuffer, uint bytes)
{
    MSSFileHandle *handle = (MSSFileHandle *)hFileHandle;
    if (!handle || !pBuffer)
    {
        return 0;
    }
    if (handle->memory)
    {
        return DB64_ReadMemoryStream(handle->memory, pBuffer, bytes);
    }
    const int count = FS_Read((byte *)pBuffer, bytes > INT_MAX ? INT_MAX : (int)bytes, handle->file);
    return count < 0 ? 0 : count;
}


#include <universal/q_shared.h>
#include "database.h"
#include <limits.h>

size_t DB_StreamArraySize(size_t elementSize, int count)
{
    if (!elementSize || count < 0 || (size_t)count > SIZE_MAX / elementSize)
    {
        Com_Error(ERR_DROP, "Invalid native fastfile array size");
    }
    return elementSize * (size_t)count;
}

void Load_Stream(bool atStreamStart, uint8_t *ptr, size_t size)
{
    if (!atStreamStart || !size)
    {
        return;
    }
    if (ptr != DB_GetStreamPos() || size > INT_MAX)
    {
        Com_Error(ERR_DROP, "Invalid native fastfile stream read");
    }
    DB_IncStreamPos(size);
    if (g_streamPosIndex == 1)
    {
        memset(ptr, 0, size);
    }
    else if (g_streamPosIndex == 2 || g_streamPosIndex == 3)
    {
        if (g_streamDelayIndex >= ARRAY_COUNT(g_streamDelayArray))
        {
            Com_Error(ERR_DROP, "Too many native fastfile delayed streams");
        }
        g_streamDelayArray[g_streamDelayIndex].ptr = ptr;
        g_streamDelayArray[g_streamDelayIndex++].size = size;
    }
    else
    {
        DB_LoadXFileData(ptr, (uint)size);
    }
}

void Load_DelayStream()
{
    for (uint index = 0; index < g_streamDelayIndex; ++index)
    {
        DB_LoadXFileData((uint8_t *)g_streamDelayArray[index].ptr, (uint)g_streamDelayArray[index].size);
    }
}

// The upper four bits select a block; the lower bits encode offset + 1.
static uint8_t *DB_ResolveOffset(uintptr_t encoded, size_t required)
{
    if (!encoded || encoded >= UINTPTR_MAX - 1)
    {
        Com_Error(ERR_DROP, "Invalid native fastfile pointer token");
    }
    const uintptr_t offset = encoded - 1;
    const unsigned int shift = sizeof(uintptr_t) * CHAR_BIT - 4;
    const unsigned int block = (unsigned int)(offset >> shift);
    const uintptr_t position = offset & (UINTPTR_MAX >> 4);
    if (block >= ARRAY_COUNT(g_streamZoneMem->blocks))
    {
        Com_Error(ERR_DROP, "Invalid native fastfile pointer block");
    }
    const XBlock *stream = &g_streamZoneMem->blocks[block];
    if (!stream->data || position > stream->size || required > stream->size - position)
    {
        Com_Error(ERR_DROP, "Native fastfile pointer outside its block");
    }
    return stream->data + position;
}

void DB_ConvertOffsetToAlias(uintptr_t *data)
{
    uintptr_t encoded;
    memcpy(&encoded, data, sizeof(uintptr_t));
    const uint8_t *alias = DB_ResolveOffset(encoded, sizeof(void *));
    memcpy(data, alias, sizeof(void *));
}

void DB_ConvertOffsetToPointer(uintptr_t *data)
{
    uintptr_t encoded;
    memcpy(&encoded, data, sizeof(uintptr_t));
    const void *pointer = DB_ResolveOffset(encoded, 1);
    memcpy(data, &pointer, sizeof(void *));
}

void DB64_ConvertOffsetRange(uintptr_t *data, size_t size)
{
    uintptr_t token;
    memcpy(&token, data, sizeof(uintptr_t));
    const void *pointer = DB_ResolveOffset(token, size);
    memcpy(data, &pointer, sizeof(void *));
}

void Load_XStringCustom(char **str)
{
    *str = (char *)DB_GetStreamPos();
    for (;;)
    {
        uint8_t *character = DB_GetStreamPos();
        DB_IncStreamPos(sizeof(char));
        DB_LoadXFileData(character, sizeof(char));
        if (!*character)
        {
            break;
        }
    }
}

void Load_TempStringCustom(char **str)
{
    // ScriptStringList contains pointers; intern when resolving each handle.
    Load_XStringCustom(str);
}

void DB64_LoadAssetString(const char **str)
{
    if ((uintptr_t)*str == UINTPTR_MAX)
    {
        Load_XStringCustom((char **)str);
    }
    else if (*str)
    {
        const uintptr_t token = (uintptr_t)*str;
        const uint8_t *resolved = DB_ResolveOffset(token, sizeof(char));
        const unsigned int block = (unsigned int)((token - 1) >> (sizeof(uintptr_t) * CHAR_BIT - 4));
        const XBlock *stream = &g_streamZoneMem->blocks[block];
        const size_t remaining = stream->size - (resolved - stream->data);
        if (!memchr(resolved, 0, remaining))
        {
            Com_Error(ERR_DROP, "Unterminated native fastfile string reference");
        }
        *str = (const char *)resolved;
    }
}

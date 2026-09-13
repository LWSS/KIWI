#include <universal/q_shared.h>
#include "database.h"

uint g_streamDelayIndex;
XBlock *g_streamBlocks;
uint8_t *g_streamPosArray[9];
StreamDelayInfo g_streamDelayArray[4096];
uint g_streamPosIndex;
XZoneMemory *g_streamZoneMem;
uint8_t *g_streamPos;

StreamPosInfo g_streamPosStack[64];
uint g_streamPosStackIndex;

// --- file-local forward declarations (moved out of database.h) ---
static void __cdecl DB_SetStreamIndex(uint index);

void __cdecl DB_InitStreams(XZoneMemory *zoneMem)
{
    int i; // [esp+0h] [ebp-4h]

    g_streamZoneMem = zoneMem;
    g_streamPos = zoneMem->blocks[0].data;
    g_streamPosIndex = 0;
    g_streamDelayIndex = 0;
    g_streamPosStackIndex = 0;
    for (i = 0; i < 9; ++i)
    {
        g_streamPosArray[i] = zoneMem->blocks[i].data;
    }
}

void __cdecl DB_PushStreamPos(uint index)
{
    if (index >= ARRAY_COUNT(g_streamPosArray) || g_streamPosStackIndex >= ARRAY_COUNT(g_streamPosStack))
    {
        Com_Error(ERR_DROP, "Native fastfile stream stack overflow");
    }
    iassert(index < ARRAY_COUNT(g_streamPosArray));
    iassert(g_streamPosIndex < ARRAY_COUNT(g_streamPosArray));
    iassert(g_streamPosStackIndex < ARRAY_COUNT(g_streamPosStack));

    g_streamPosStack[g_streamPosStackIndex].index = g_streamPosIndex;
    DB_SetStreamIndex(index);

    g_streamPosStack[g_streamPosStackIndex++].pos = g_streamPos;
}

void __cdecl DB_CloneStreamData(uint8_t *destStart)
{
    if (destStart)
    {
        memcpy(&destStart[g_streamPosArray[g_streamPosIndex] - g_streamZoneMem->blocks[g_streamPosIndex].data],
               g_streamPosArray[g_streamPosIndex], g_streamPos - g_streamPosArray[g_streamPosIndex]);
    }
}

void __cdecl DB_SetStreamIndex(uint index)
{
    if (index != g_streamPosIndex)
    {
        if (g_streamPosIndex == 7)
        {
            DB_CloneStreamData(g_streamZoneMem->lockedVertexData);
        }
        else if (g_streamPosIndex == 8)
        {
            DB_CloneStreamData(g_streamZoneMem->lockedIndexData);
        }
        iassert(index < arr_cnt(g_streamPosArray));
        g_streamPosArray[g_streamPosIndex] = g_streamPos;
        g_streamPosIndex = index;
        g_streamPos = g_streamPosArray[index];
    }
}

void __cdecl DB_PopStreamPos()
{
    if (!g_streamPosStackIndex)
    {
        Com_Error(ERR_DROP, "Native fastfile stream stack underflow");
    }
    vassert(g_streamPosStackIndex > 0, "(g_streamPosStackIndex = %d)", g_streamPosStackIndex);
    --g_streamPosStackIndex;
    if (!g_streamPosIndex)
    {
        g_streamPos = g_streamPosStack[g_streamPosStackIndex].pos;
    }
    DB_SetStreamIndex(g_streamPosStack[g_streamPosStackIndex].index);
}

uint8_t *__cdecl DB_GetStreamPos()
{
    return g_streamPos;
}

uint8_t *__cdecl DB_AllocStreamPos(int alignment)
{
    if (alignment < 0 || (alignment & (alignment + 1)) != 0)
    {
        Com_Error(ERR_DROP, "Invalid native fastfile alignment");
    }
    const size_t padding = (-(uintptr_t)g_streamPos) & (uintptr_t)alignment;
    DB_IncStreamPos(padding);
    return g_streamPos;
}

void __cdecl DB_IncStreamPos(size_t size)
{
    const XBlock *block = &g_streamZoneMem->blocks[g_streamPosIndex];
    const uintptr_t offset = (uintptr_t)g_streamPos - (uintptr_t)block->data;
    if (!g_streamPos || !block->data || offset > block->size || size > block->size - offset)
    {
        Com_Error(ERR_DROP, "Native fastfile stream exceeds block allocation");
    }
    g_streamPos += size;
}

const void **__cdecl DB_InsertPointer()
{
    const void **pData; // [esp+0h] [ebp-4h]

    DB_PushStreamPos(4);
    pData = (const void **)DB_AllocStreamPos(alignof(void *) - 1);
    DB_IncStreamPos(sizeof(void *));
    DB_PopStreamPos();
    return pData;
}

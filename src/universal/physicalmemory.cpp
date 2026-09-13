#include <universal/q_shared.h>
#include "physicalmemory.h"

#include <Windows.h>
#include "assertive.h"
#include <qcommon/mem_track.h>
#include "q_shared.h"
#include <qcommon/qcommon.h>
#include <win32/win_local.h>

PhysicalMemory g_mem;
int g_overAllocatedSize;

void __cdecl PMem_Init()
{
#ifdef _WIN64
    // Reserve address space without committing the entire fastfile arena up front.
    const uint memorySize = 0x80000000;
#else
    const uint memorySize = 0x08000000;
#endif
    uint8_t *memory = (uint8_t *)VirtualAlloc(NULL, memorySize, MEM_RESERVE, PAGE_READWRITE);
    if (!memory)
    {
        Sys_OutOfMemErrorInternal(__FILE__, __LINE__);
        return;
    }
    PMem_InitPhysicalMemory(&g_mem, memory, memorySize);
}

void __cdecl PMem_DumpMemStats()
{
    double v0; // st7
    int FreeAmount; // eax
    double v2; // st7
    double v3; // st7
    signed int j; // [esp+8h] [ebp-14h]
    uint i; // [esp+Ch] [ebp-10h]
    uint top; // [esp+14h] [ebp-8h]
    uint bottom; // [esp+18h] [ebp-4h]

    for (i = 0; i < g_mem.prim[1].allocListCount; ++i)
    {
        if (i == g_mem.prim[1].allocListCount - 1)
            bottom = g_mem.prim[1].pos;
        else
            bottom = g_mem.prim[1].allocList[i + 1].pos;
        v0 = ConvertToMB(g_mem.prim[1].allocList[i].pos - bottom);
        Com_Printf(CON_CHANNEL_SYSTEM, "%-18.18s %5.1f\n", g_mem.prim[1].allocList[i].name, v0);
    }
    FreeAmount = PMem_GetFreeAmount();
    v2 = ConvertToMB(FreeAmount);
    Com_Printf(CON_CHANNEL_SYSTEM, "free physical      %5.1f\n", v2);
    top = g_mem.prim[0].pos;
    for (j = g_mem.prim[0].allocListCount - 1; j >= 0; --j)
    {
        v3 = ConvertToMB(top - g_mem.prim[0].allocList[j].pos);
        Com_Printf(CON_CHANNEL_SYSTEM, "%-18.18s %5.1f\n", g_mem.prim[0].allocList[j].name, v3);
        top = g_mem.prim[0].allocList[j].pos;
    }
    Com_Printf(CON_CHANNEL_SYSTEM, "------------------------\n");
}

void __cdecl PMem_InitPhysicalMemory(PhysicalMemory *pmem, uint8_t *memory, uint memorySize)
{
    iassert(pmem);
    iassert(memory);
    memset((uint8_t *)pmem, 0, sizeof(PhysicalMemory));
    pmem->buf = memory;
    pmem->prim[1].pos = memorySize;
}

void __cdecl PMem_BeginAlloc(const char *name, uint allocType)
{
    bcassert(allocType, 2);
    PMem_BeginAllocInPrim(&g_mem.prim[allocType], name);
}

void __cdecl PMem_BeginAllocInPrim(PhysicalMemoryPrim *prim, const char *name)
{
    PhysicalMemoryAllocation *allocEntry; // [esp+0h] [ebp-4h]

    iassert(!prim->allocName);
    if (prim->allocListCount >= 0x20)
        MyAssertHandler(".\\universal\\physicalmemory.cpp", 333, 0, "%s", "prim->allocListCount < MAX_PHYSICAL_ALLOCATIONS");
    prim->allocName = name;
    allocEntry = &prim->allocList[prim->allocListCount++];
    allocEntry->name = name;
    allocEntry->pos = prim->pos;
}

void __cdecl PMem_EndAlloc(const char *name, uint allocType)
{
    bcassert(allocType, 2);
    PMem_EndAllocInPrim(&g_mem.prim[allocType], name);
}

void __cdecl PMem_EndAllocInPrim(PhysicalMemoryPrim *prim, const char *name)
{
    __int64 v2; // rax

    iassert(prim->allocName == name);
    prim->allocName = 0;
    if (!prim->allocListCount)
    {
        MyAssertHandler(".\\universal\\physicalmemory.cpp", 368, 0, "%s", "prim->allocListCount > 0");
    }
    v2 = (int64_t)prim->pos - prim->allocList[prim->allocListCount - 1].pos;
    // track_physical_alloc((HIunsigned int(v2) ^ v2) - HIunsigned int(v2), name, 10);
}

void __cdecl PMem_Free(const char *name, uint allocType)
{
    bcassert(allocType, 2);
    PMem_FreeInPrim(&g_mem.prim[allocType], name);
}

void __cdecl PMem_FreeInPrim(PhysicalMemoryPrim *prim, const char *name)
{
    uint allocIndex; // [esp+0h] [ebp-8h]

    for (allocIndex = 0; allocIndex < prim->allocListCount; ++allocIndex)
    {
        if (prim->allocList[allocIndex].name == name)
        {
            PMem_FreeIndex(prim, allocIndex);
            return;
        }
    }
}

void __cdecl PMem_FreeIndex(PhysicalMemoryPrim *prim, uint allocIndex)
{
    __int64 v2;                           // rax
    const char *v3;                       // eax
    __int64 v4;                           // rax
    PhysicalMemoryAllocation *allocEntry; // [esp+0h] [ebp-Ch]
    const char *name;                     // [esp+4h] [ebp-8h]

    iassert(!prim->allocName);
    allocEntry = &prim->allocList[allocIndex];
    name = allocEntry->name;
    if (!allocEntry->name)
    {
        MyAssertHandler(".\\universal\\physicalmemory.cpp", 400, 0, "%s", "name");
    }
    allocEntry->name = 0;
    if (allocIndex == prim->allocListCount - 1)
    {
        v4 = prim->pos - prim->allocList[allocIndex].pos;
        // track_physical_alloc(HIunsigned int(v4) - (HIunsigned int(v4) ^ v4), name, 10);
        do
        {
            prim->pos = allocEntry->pos;
            iassert(prim->allocListCount);
            if (!--prim->allocListCount)
            {
                break;
            }
            allocEntry = &prim->allocList[prim->allocListCount - 1];
        } while (!allocEntry->name);
    }
    else
    {
        iassert(allocIndex + 1 < prim->allocListCount);
        v2 = prim->allocList[allocIndex + 1].pos - prim->allocList[allocIndex].pos;
        // track_physical_alloc(HIunsigned int(v2) - (HIunsigned int(v2) ^ v2), name, 10);
        if (!alwaysfails)
        {
            v3 = va("freeing '%s' caused a memory hole\n", name);
            MyAssertHandler(".\\universal\\physicalmemory.cpp", 411, 0, v3);
        }
    }
}

int __cdecl PMem_GetOverAllocatedSize()
{
    return g_overAllocatedSize;
}

uint8_t *__cdecl PMem_Alloc(
    uint size,
    uint alignment,
    uint type,
    uint allocType)
{
    if (allocType >= 2 || !size || !alignment || (alignment & (alignment - 1)))
    {
        Com_Error(ERR_FATAL, "Invalid PMem allocation: size %u, alignment %u, allocType %u",
                  size, alignment, allocType);
        return NULL;
    }

    PhysicalMemoryPrim *prim = &g_mem.prim[allocType];
    iassert(prim->allocName);
    const uint64_t alignmentMask = (uint64_t)alignment - 1;
    const uint64_t low = g_mem.prim[0].pos;
    const uint64_t high = g_mem.prim[1].pos;
    uint64_t lowPos;
    uint64_t endPos;
    uint64_t shortage = 0;
    if (allocType == 1)
    {
        if (size > high)
        {
            shortage = (uint64_t)size - high + low;
            lowPos = 0;
        }
        else
        {
            lowPos = (high - size) & ~alignmentMask;
            if (lowPos < low)
            {
                shortage = low - lowPos;
            }
        }
        endPos = high;
    }
    else
    {
        lowPos = (low + alignmentMask) & ~alignmentMask;
        endPos = lowPos + size;
        if (endPos > high)
        {
            shortage = endPos - high;
        }
    }
    g_overAllocatedSize = (int)(shortage > INT_MAX ? INT_MAX : shortage);
    if (shortage)
    {
        return NULL;
    }

    uint8_t *memory = &g_mem.buf[(size_t)lowPos];
    if (!VirtualAlloc(memory, size, MEM_COMMIT, PAGE_READWRITE))
    {
        g_overAllocatedSize = (int)(size > INT_MAX ? INT_MAX : size);
        Com_PrintError(CON_CHANNEL_SYSTEM, "PMem could not commit %u bytes (Windows error %lu)\n",
                       size, GetLastError());
        return NULL;
    }
    prim->pos = (uint)(allocType == 1 ? lowPos : endPos);
    return memory;
}

uint __cdecl PMem_GetFreeAmount()
{
    return g_mem.prim[1].pos - g_mem.prim[0].pos;
}


#pragma once
#include <cstdint>

struct PhysicalMemoryAllocation // sizeof=0x8
{                                       // ...
    const char *name;                   // ...
    uint pos;                   // ...
};
struct PhysicalMemoryPrim // sizeof=0x10C
{                                       // ...
    const char *allocName;
    uint allocListCount;        // ...
    uint pos;                   // ...
    PhysicalMemoryAllocation allocList[32]; // ...
};
struct PhysicalMemory // sizeof=0x21C
{                                       // ...
    uint8_t *buf;
    PhysicalMemoryPrim prim[2];         // ...
};

void __cdecl PMem_Init();
void __cdecl PMem_DumpMemStats();
void __cdecl PMem_InitPhysicalMemory(PhysicalMemory *pmem, uint8_t *memory, uint memorySize);
void __cdecl PMem_BeginAlloc(const char *name, uint allocType);
void __cdecl PMem_BeginAllocInPrim(PhysicalMemoryPrim *prim, const char *name);
void __cdecl PMem_EndAlloc(const char *name, uint allocType);
void __cdecl PMem_EndAllocInPrim(PhysicalMemoryPrim *prim, const char *name);
void __cdecl PMem_Free(const char *name, uint allocType);
void __cdecl PMem_FreeInPrim(PhysicalMemoryPrim *prim, const char *name);
void __cdecl PMem_FreeIndex(PhysicalMemoryPrim *prim, uint allocIndex);
int __cdecl PMem_GetOverAllocatedSize();
uint8_t *__cdecl PMem_Alloc(
    uint size,
    uint alignment,
    uint type,
    uint allocType);
uint __cdecl PMem_GetFreeAmount();
void __cdecl PMem_DumpMemStats();

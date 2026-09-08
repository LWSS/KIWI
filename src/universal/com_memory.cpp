#include <universal/q_shared.h>
#include "com_memory.h"
#include "assertive.h"

#include "script/scr_stringlist.h"

#include <string.h>
#include <qcommon/qcommon.h>
#include <qcommon/threads.h>

#include <qcommon/mem_track.h>
#include <win32/win_local.h>
#include <win32/win_net.h>
#include <database/database.h>
#include "com_files.h"
#include <qcommon/cmd.h>
#include <gfx_d3d/r_dvars.h>
#include "physicalmemory.h"

#include <cstdint>

HunkUser *g_user;
fileData_s *com_hunkData;

static HunkUser* g_debugUser;
static int g_largeLocalPos;
alignas(16) static byte g_largeLocalBuf[0x80000];

struct hunkUsed_t // sizeof=0x8
{                                       // ...
	int permanent;                      // ...
	int temp;                           // ...
};
struct alignas(16) hunkHeader_t // sizeof=0x10
{
	uint magic;
	int size;
	const char* name;
	int dummy;
};

static hunkUsed_t hunk_high;
static hunkUsed_t hunk_low;

byte* s_hunkData;
uint8_t *s_origHunkData;
int s_hunkTotal;

void __cdecl Hunk_AddAsset(XAssetHeader header, void *data)
{
    AssetList *list = (AssetList *)data;
    iassert(list);
    if (list->assetCount >= list->maxCount)
    {
        Com_Error(ERR_DROP, "Asset list capacity exceeded");
    }
    list->assets[list->assetCount++] = header;
}

void Com_TouchMemory()
{
    int sum; // [esp+4h] [ebp-10h]
    DWORD start; // [esp+8h] [ebp-Ch]
    DWORD end; // [esp+Ch] [ebp-8h]
    int i; // [esp+10h] [ebp-4h]
    int ia; // [esp+10h] [ebp-4h]

    iassert(Sys_IsMainThread());
    start = Sys_Milliseconds();
    sum = 0;
    for (i = 0; i < hunk_low.permanent >> 2; i += 64)
        sum += *(_DWORD *)&s_hunkData[4 * i];
    for (ia = (s_hunkTotal - hunk_high.permanent) >> 2; ia < hunk_high.permanent >> 2; ia += 64)
        sum += *(_DWORD *)&s_hunkData[4 * ia];
    end = Sys_Milliseconds();
    Com_Printf(CON_CHANNEL_SYSTEM, "Com_TouchMemory: %i msec. Using sum: %d\n", end - start, sum);
}

/*
========================
CopyString

 NOTE:	never write over the memory CopyString returns because
		memory from a memstatic_t might be returned
========================
*/
const char* CopyString(const char* in)
{
	uint out;

	iassert(in);

	out = SL_GetString_(in, 0, MT_TYPE_GENERIC);
	return SL_ConvertToString(out);
}

void FreeString(const char* str)
{
	iassert(str);

	uint out = SL_FindString(str);

	iassert(out);

	SL_RemoveRefToString(out);
}


uint8_t* __cdecl Hunk_AllocXAnimPrecache(uint size)
{
    return Hunk_AllocAlign(size, 4, "XAnimPrecache", 11);
}

uint8_t* __cdecl Hunk_AllocPhysPresetPrecache(uint size)
{
    return Hunk_Alloc(size, "PhysPresetPrecache", 0);
}

uint8_t* __cdecl Hunk_AllocXAnimClient(uint size)
{
    return Hunk_Alloc(size, "Hunk_AllocXAnimClient", 11);
}


uint8_t* __cdecl Hunk_AllocXAnimServer(uint size)
{
    return Hunk_AllocLow(size, "Hunk_AllocXAnimServer", 11);
}

//void __cdecl TRACK_com_memory()
//{
//    track_static_alloc_internal(com_fileDataHashTable, 4096, "com_fileDataHashTable", 10);
//    track_static_alloc_internal(g_largeLocalBuf, 0x80000, "g_largeLocalBuf", 10);
//}

void* __cdecl Z_VirtualReserve(int size)
{
    void* buf; // [esp+0h] [ebp-4h]

    vassert((size > 0), "(size) = %i", size);
    buf = VirtualAlloc(0, size, 0x2000u, 4u);
    iassert(buf);
    return buf;
}

void __cdecl Z_VirtualDecommitInternal(void* ptr, int size)
{
    vassert((size >= 0), "(size) = %i", size);
    VirtualFree(ptr, size, 0x4000u);
}

void __cdecl Z_VirtualFreeInternal(void* ptr)
{
    VirtualFree(ptr, 0, 0x8000u);
}

void* __cdecl Z_TryVirtualAllocInternal(int size)
{
    void* ptr; // [esp+0h] [ebp-4h]

    ptr = Z_VirtualReserve(size);
    if (Z_TryVirtualCommitInternal(ptr, size))
        return ptr;
    Z_VirtualFree(ptr);
    return 0;
}

bool __cdecl Z_TryVirtualCommitInternal(void* ptr, int size)
{
    iassert(size >= 0);
    return VirtualAlloc(ptr, size, 0x1000u, 4u) != 0;
}

void __cdecl Z_VirtualCommitInternal(void* ptr, int size)
{
    if (!Z_TryVirtualCommitInternal(ptr, size))
        Sys_OutOfMemErrorInternal(".\\universal\\com_memory.cpp", 426);
}

void __cdecl Z_VirtualFree(void* ptr)
{
    Z_VirtualFreeInternal(ptr);
}

void __cdecl Z_VirtualDecommit(void* ptr, int size)
{
    Z_VirtualDecommitInternal(ptr, size);
}

char *__cdecl Z_TryVirtualAlloc(int size, const char *name, int type)
{
    char *buf; // [esp+0h] [ebp-4h]

    buf = (char *)Z_TryVirtualAllocInternal(size);
    if (buf)
    {
        track_z_commit((size + 4095) & ~(uintptr_t)0xFFF, type);
    }
    return buf;
}

char* __cdecl Z_VirtualAlloc(int size, const char* name, int type)
{
    char* buf; // [esp+0h] [ebp-4h]

    buf = Z_TryVirtualAlloc(size, name, type);
    if (!buf)
        Sys_OutOfMemErrorInternal(".\\universal\\com_memory.cpp", 716);
    return buf;
}

void __cdecl Z_VirtualCommit(void* ptr, int size)
{
    Z_VirtualCommitInternal(ptr, size);
}

const char* __cdecl CopyString(char* in)
{
    iassert(in);

    return SL_ConvertToString(SL_GetString_(in, 0, MT_TYPE_GENERIC));
}

void __cdecl ReplaceString(const char** str, const char* in)
{
    const char* newStr; // [esp+0h] [ebp-4h]

    iassert(str);
    iassert(in);
    newStr = CopyString(in);
    if (*str)
        FreeString(*str);
    *str = newStr;
}

cmd_function_s Com_Meminfo_f_VAR;
cmd_function_s Com_TempMeminfo_f_VAR;

void __cdecl Com_TempMeminfo_f()
{
    iassert(Sys_IsMainThread());
    //track_PrintTempInfo();
    Com_Printf(CON_CHANNEL_DONT_FILTER, "Related commands: meminfo, imagelist, gfx_world, gfx_model, cg_drawfps, com_statmon, tempmeminfo\n");
}

void Com_InitHunkMemory()
{
    iassert(Sys_IsMainThread());
    iassert(!s_hunkData);

    if (FS_LoadStack())
        Com_Error(ERR_FATAL, "Hunk initialization failed. File system load stack not zero");

    if (!IsFastFileLoad())
    {
#ifdef KISAK_PURE
        s_hunkTotal = 0xA000000;
#else
        s_hunkTotal = 0x20000000; // LWSS: MOAR! !
#endif
    }
    if (IsFastFileLoad())
    {
        s_hunkTotal = 0xA00000;
    }
    R_ReflectionProbeRegisterDvars();
    if (r_reflectionProbeGenerate->current.enabled)
        s_hunkTotal = 0x20000000;
    s_hunkData = (byte*)Z_VirtualReserve(s_hunkTotal);
    if (!s_hunkData)
        Sys_OutOfMemErrorInternal(".\\universal\\com_memory.cpp", 1318);
    s_origHunkData = s_hunkData;
    track_set_hunk_size(s_hunkTotal);
    Hunk_Clear();
    Cmd_AddCommandInternal("meminfo", Com_Meminfo_f, &Com_Meminfo_f_VAR);
    Cmd_AddCommandInternal("tempmeminfo", Com_TempMeminfo_f, &Com_TempMeminfo_f_VAR);
}

void __cdecl Com_Meminfo_f()
{
    iassert(Sys_IsMainThread());
    track_PrintInfo();
    if (IsFastFileLoad())
        PMem_DumpMemStats();
    Com_Printf(CON_CHANNEL_DONT_FILTER, "Related commands: meminfo, imagelist, gfx_world, gfx_model, cg_drawfps, com_statmon, tempmeminfo\n");
}

void* __cdecl Hunk_FindDataForFile(int type, const char* name)
{
    int hash; // [esp+0h] [ebp-4h]

    hash = FS_HashFileName(name, 1024);
    return Hunk_FindDataForFileInternal(type, name, hash);
}

void* __cdecl Hunk_FindDataForFileInternal(int type, const char* name, int hash)
{
    fileData_s* searchFileData; // [esp+0h] [ebp-4h]

    for (searchFileData = com_fileDataHashTable[hash]; searchFileData; searchFileData = searchFileData->next)
    {
        if (searchFileData->type == type && !I_stricmp(searchFileData->name, name))
            return searchFileData->data;
    }
    return 0;
}

bool __cdecl Hunk_DataOnHunk(uint8_t* data)
{
#ifdef KISAK_MP
    iassert(Sys_IsMainThread());
#endif
    iassert(s_hunkData);
    return data >= s_hunkData && data < &s_hunkData[s_hunkTotal];
}

char *__cdecl Hunk_SetDataForFile(int type, const char *name, void *data, void *(__cdecl *alloc)(int))
{
    char v5;              // [esp+3h] [ebp-25h]
    char *v6;             // [esp+8h] [ebp-20h]
    const char *v7;       // [esp+Ch] [ebp-1Ch]
    int hash;             // [esp+20h] [ebp-8h]
    fileData_s *fileData; // [esp+24h] [ebp-4h]

#ifdef KISAK_MP
    iassert(Sys_IsMainThread());
#endif
    hash = FS_HashFileName(name, 1024);
    iassert(!Hunk_FindDataForFileInternal(type, name, hash));
    fileData = (fileData_s *)alloc(offsetof(fileData_s, name) + strlen(name) + 1);
    if (!Hunk_DataOnHunk((uint8_t *)fileData))
    {
        MyAssertHandler(".\\universal\\com_memory.cpp", 1488, 0, "%s", "Hunk_DataOnHunk( fileData )");
    }
    fileData->data = data;
    fileData->type = type;
    iassert(type == fileData->type);
    v7 = name;
    v6 = fileData->name;
    do
    {
        v5 = *v7;
        *v6++ = *v7++;
    } while (v5);
    fileData->next = com_fileDataHashTable[hash];
    com_fileDataHashTable[hash] = fileData;
    return fileData->name;
}

void __cdecl Hunk_AddData(int type, void *data, void *(__cdecl *alloc)(int))
{
    fileData_s *fileData; // [esp+0h] [ebp-4h]

    iassert(Sys_IsMainThread());
    fileData = (fileData_s *)alloc(sizeof(fileData_s));
    if (!Hunk_DataOnHunk((uint8_t *)fileData))
    {
        MyAssertHandler(".\\universal\\com_memory.cpp", 1516, 0, "%s", "Hunk_DataOnHunk( fileData )");
    }
    fileData->data = data;
    fileData->type = type;
    iassert(type == fileData->type);
    fileData->next = com_hunkData;
    com_hunkData = fileData;
}

void Hunk_ClearData()
{
    uint8_t* low; // [esp+0h] [ebp-10h]
    uint hash; // [esp+4h] [ebp-Ch]
    uint8_t* high; // [esp+Ch] [ebp-4h]

    iassert(Sys_IsMainThread());
    low = &s_hunkData[hunk_low.permanent];
    high = &s_hunkData[s_hunkTotal - hunk_high.permanent];
    for (hash = 0; hash < 0x400; ++hash)
        Hunk_ClearDataFor(&com_fileDataHashTable[hash], low, high);
    Hunk_ClearDataFor(&com_hunkData, low, high);
}

void __cdecl Hunk_ClearDataFor(fileData_s** pFileData, uint8_t* low, uint8_t* high)
{
    uint8_t type; // [esp+0h] [ebp-Ch]
    XAnim_s* data; // [esp+4h] [ebp-8h]
    fileData_s* fileData; // [esp+8h] [ebp-4h]

    iassert(Sys_IsMainThread());
    while (*pFileData)
    {
        fileData = *pFileData;
        if (*pFileData >= (fileData_s*)low && fileData < (fileData_s*)high)
        {
            *pFileData = fileData->next;
            data = (XAnim_s*)fileData->data;
            type = fileData->type;
            switch (type)
            {
            case 2u:
                XAnimFreeList(data);
                break;
            case 4u:
                XModelPartsFree((XModelPartsLoad*)data);
                break;
            case 6u:
                XAnimFree((XAnimParts*)data);
                break;
            }
        }
        else
        {
            pFileData = &fileData->next;
        }
    }
}

void __cdecl Hunk_ClearToMarkLow(int mark)
{
    uint8_t *endBuf;   // [esp+0h] [ebp-Ch]
    uint8_t *beginBuf; // [esp+8h] [ebp-4h]

    iassert(Sys_IsMainThread());
    Hunk_CheckTempMemoryClear();
    endBuf = (uint8_t *)((uintptr_t)&s_hunkData[hunk_low.temp + 4095] & ~(uintptr_t)0xFFF);
    hunk_low.temp = mark;
    hunk_low.permanent = mark;
    Hunk_ClearData();
    beginBuf = (uint8_t *)((uintptr_t)&s_hunkData[hunk_low.temp + 4095] & ~(uintptr_t)0xFFF);
    if (endBuf != beginBuf)
    {
        Z_VirtualDecommit(beginBuf, endBuf - beginBuf);
    }
    track_hunk_ClearToMarkLow(mark);
}

void Hunk_Clear()
{
    iassert(Sys_IsMainThread());
    hunk_low.permanent = 0;
    hunk_low.temp = 0;
    hunk_high.permanent = 0;
    hunk_high.temp = 0;
    Hunk_ClearData();
    Z_VirtualDecommit(s_hunkData, s_hunkTotal);
    track_hunk_ClearToMarkLow(0);
    track_hunk_ClearToMarkHigh(0);
    track_hunk_ClearToStart();
}

int __cdecl Hunk_Used()
{
    iassert(Sys_IsMainThread() || Sys_IsRenderThread());
    return hunk_high.permanent + hunk_low.permanent;
}

uint8_t* __cdecl Hunk_Alloc(uint size, const char* name, int type)
{
#ifdef KISAK_MP
    iassert(Sys_IsMainThread());
#endif
    return Hunk_AllocAlign(size, 32, name, type);
}

uint8_t *__cdecl Hunk_AllocAlign(uint size, int alignment, const char *name, int type)
{
    int old_permanent; // [esp+0h] [ebp-14h]
    uint8_t *buf;      // [esp+4h] [ebp-10h]
    uint8_t *endBuf;   // [esp+8h] [ebp-Ch]
    int alignmenta;    // [esp+20h] [ebp+Ch]

#ifdef KISAK_MP
    iassert(Sys_IsMainThread());
#endif
    iassert(s_hunkData);
    iassert(!(alignment & (alignment - 1)));
    iassert(alignment <= HUNK_MAX_ALIGNEMT);
    alignmenta = alignment - 1;
    Hunk_CheckTempMemoryHighClear();
    if (alignment <= 0 || alignment > HUNK_MAX_ALIGNEMT || (alignment & (alignment - 1))
        || (((uint64_t)hunk_high.permanent + size + alignment - 1) & ~(uint64_t)(alignment - 1)) + hunk_low.temp > s_hunkTotal)
    {
        Com_Error(ERR_DROP, "Hunk_AllocAlign: invalid or exhausted allocation");
    }
    old_permanent = hunk_high.permanent;
    endBuf = (uint8_t *)((uintptr_t)&s_hunkData[s_hunkTotal - hunk_high.permanent] & ~(uintptr_t)0xFFF);
    hunk_high.permanent += size;
    hunk_high.permanent = ~alignmenta & (alignmenta + hunk_high.permanent);
    hunk_high.temp = hunk_high.permanent;
    if (hunk_high.permanent + hunk_low.temp > s_hunkTotal)
    {
        track_PrintAllInfo();
        Com_Error(ERR_DROP, "Hunk_AllocAlign failed on %i bytes (total %i MB, low %i MB, high %i MB)", size, s_hunkTotal / 0x100000, hunk_low.temp / 0x100000, hunk_high.temp / 0x100000);
    }
    buf = &s_hunkData[s_hunkTotal - hunk_high.permanent];
    if ((alignmenta & (uintptr_t)buf) != 0)
    {
        MyAssertHandler(".\\universal\\com_memory.cpp", 2011, 0, "%s", "!(((psize_int)buf) & alignment)");
    }
    if (endBuf != (uint8_t *)((uintptr_t)buf & ~(uintptr_t)0xFFF))
    {
        Z_VirtualCommit((void *)((uintptr_t)buf & ~(uintptr_t)0xFFF), (int)((uintptr_t)endBuf - ((uintptr_t)buf & ~(uintptr_t)0xFFF)));
    }
    track_hunk_alloc(hunk_high.permanent - old_permanent, hunk_high.temp, name, type);
    memset(buf, 0, size);
    return buf;
}

void *__cdecl Hunk_AllocateTempMemoryHigh(int size, const char* name)
{
    uint8_t *buf; // [esp+0h] [ebp-10h]
    uint8_t* endBuf; // [esp+4h] [ebp-Ch]

    iassert(Sys_IsMainThread());
    iassert(s_hunkData);
    endBuf = (uint8_t *)((uintptr_t)&s_hunkData[s_hunkTotal - hunk_high.temp] & ~(uintptr_t)0xFFF);
    hunk_high.temp += size;
    hunk_high.temp = (hunk_high.temp + 15) & 0xFFFFFFF0;
    if (hunk_high.temp + hunk_low.temp > s_hunkTotal)
    {
        track_PrintAllInfo();
        Com_Error(ERR_DROP, "Hunk_AllocateTempMemoryHigh: failed on %i bytes (total %i MB, low %i MB, high %i MB)", size, s_hunkTotal / 0x100000, hunk_low.temp / 0x100000, hunk_high.temp / 0x100000);
    }
    buf = &s_hunkData[s_hunkTotal - hunk_high.temp];
    iassert(!((uintptr_t)buf & 15));
    uint8_t *commitStart = (uint8_t *)((uintptr_t)buf & ~(uintptr_t)0xFFF);
    if (endBuf != commitStart)
    {
        Z_VirtualCommit(commitStart, (int)(endBuf - commitStart));
    }
    track_temp_high_alloc(size, hunk_high.temp + hunk_low.temp, hunk_high.permanent, name);
    return buf;
}

void Hunk_ClearTempMemoryHigh()
{
    uint commitSize;   // [esp+4h] [ebp-8h]
    uint8_t *beginBuf; // [esp+8h] [ebp-4h]

    iassert(Sys_IsMainThread());
    beginBuf = (uint8_t *)((uintptr_t)&s_hunkData[s_hunkTotal - hunk_high.temp] & ~(uintptr_t)0xFFF);
    hunk_high.temp = hunk_high.permanent;
    commitSize = ((uintptr_t)&s_hunkData[s_hunkTotal - hunk_high.permanent] & ~(uintptr_t)0xFFF) - (uintptr_t)beginBuf;
    if (commitSize)
    {
        Z_VirtualDecommit(beginBuf, commitSize);
    }
    track_temp_high_clear(hunk_high.permanent);
}

uint8_t* __cdecl Hunk_AllocLow(uint size, const char* name, int type)
{
#ifdef KISAK_MP
    iassert(Sys_IsMainThread());
#endif
    return Hunk_AllocLowAlign(size, 32, name, type);
}

uint8_t *__cdecl Hunk_AllocLowAlign(uint size, int alignment, const char *name, int type)
{
    int old_permanent; // [esp+0h] [ebp-14h]
    uint8_t *buf;      // [esp+4h] [ebp-10h]
    uint commitSize;   // [esp+Ch] [ebp-8h]
    uint8_t *beginBuf; // [esp+10h] [ebp-4h]
    int alignmenta;    // [esp+20h] [ebp+Ch]

#ifdef KISAK_MP
    iassert(Sys_IsMainThread());
#endif
    iassert(s_hunkData);
    iassert(!(alignment & (alignment - 1)));
    iassert(alignment <= HUNK_MAX_ALIGNEMT);
    alignmenta = alignment - 1;
    Hunk_CheckTempMemoryClear();
    if (alignment <= 0 || alignment > HUNK_MAX_ALIGNEMT || (alignment & (alignment - 1))
        || (((uint64_t)hunk_low.permanent + alignment - 1) & ~(uint64_t)(alignment - 1)) + size + hunk_high.temp > s_hunkTotal)
    {
        Com_Error(ERR_DROP, "Hunk_AllocLowAlign: invalid or exhausted allocation");
    }
    old_permanent = hunk_low.permanent;
    beginBuf = (uint8_t *)((uintptr_t)&s_hunkData[hunk_low.permanent + 4095] & ~(uintptr_t)0xFFF);
    hunk_low.permanent = ~alignmenta & (alignmenta + hunk_low.permanent);
    buf = &s_hunkData[hunk_low.permanent];
    if ((alignmenta & (uintptr_t)&s_hunkData[hunk_low.permanent]) != 0)
    {
        MyAssertHandler(".\\universal\\com_memory.cpp", 2210, 0, "%s", "!(((psize_int)buf) & alignment)");
    }
    hunk_low.permanent += size;
    hunk_low.temp = hunk_low.permanent;
    if (hunk_high.temp + hunk_low.permanent > s_hunkTotal)
    {
        track_PrintAllInfo();
        Com_Error(ERR_DROP, "Hunk_AllocLowAlign failed on %i bytes (total %i MB, low %i MB, high %i MB)", size, s_hunkTotal / 0x100000, hunk_low.temp / 0x100000, hunk_high.temp / 0x100000);
    }
    commitSize = ((uintptr_t)&s_hunkData[hunk_low.permanent + 4095] & ~(uintptr_t)0xFFF) - (uintptr_t)beginBuf;
    if (commitSize)
    {
        Z_VirtualCommit(beginBuf, commitSize);
    }
    track_hunk_allocLow(hunk_low.permanent - old_permanent, hunk_low.permanent, name, type);
    memset(buf, 0, size);
    return buf;
}

uint *__cdecl Hunk_AllocateTempMemory(int size, const char *name)
{
    hunkHeader_t *hdr; // [esp+0h] [ebp-18h]
    uint8_t *buf;      // [esp+4h] [ebp-14h]
    void *bufa;        // [esp+4h] [ebp-14h]
    int prev_temp;     // [esp+8h] [ebp-10h]
    uint commitSize;   // [esp+10h] [ebp-8h]
    uint8_t *beginBuf; // [esp+14h] [ebp-4h]
    int sizea;         // [esp+20h] [ebp+8h]

#ifdef KISAK_MP
    iassert(Sys_IsMainThread());
#endif
    if (!s_hunkData)
    {
        return (uint *)Z_Malloc(size, name, 10);
    }
    if (size < 0 || (((uint64_t)hunk_low.temp + 15) & ~(uint64_t)15) + (uint)size + sizeof(hunkHeader_t) + hunk_high.temp > s_hunkTotal)
    {
        Com_Error(ERR_DROP, "Hunk_AllocateTempMemory: invalid or exhausted allocation");
    }
    sizea = size + sizeof(hunkHeader_t);
    prev_temp = hunk_low.temp;
    beginBuf = (uint8_t *)((uintptr_t)&s_hunkData[hunk_low.temp + 4095] & ~(uintptr_t)0xFFF);
    hunk_low.temp = (hunk_low.temp + 15) & 0xFFFFFFF0;
    buf = &s_hunkData[hunk_low.temp];
    hunk_low.temp += sizea;
    if (hunk_high.temp + hunk_low.temp > s_hunkTotal)
    {
        track_PrintAllInfo();
        Com_Error(
            ERR_DROP,
            "Hunk_AllocateTempMemory: failed on %i bytes (total %i MB, low %i MB, high %i MB), needs %i more hunk bytes",
            sizea,
            s_hunkTotal / 0x100000,
            hunk_low.temp / 0x100000,
            hunk_high.temp / 0x100000,
            hunk_high.temp + hunk_low.temp - s_hunkTotal);
    }
    hdr = (hunkHeader_t *)buf;
    bufa = buf + sizeof(hunkHeader_t);
    if (((uintptr_t)bufa & 0xF) != 0)
    {
        MyAssertHandler(".\\universal\\com_memory.cpp", 2303, 0, "%s", "!(((psize_int)buf) & 15)");
    }
    commitSize = ((uintptr_t)&s_hunkData[hunk_low.temp + 4095] & ~(uintptr_t)0xFFF) - (uintptr_t)beginBuf;
    if (commitSize)
    {
        Z_VirtualCommit(beginBuf, commitSize);
    }
    hdr->magic = -1991018350;
    hdr->size = hunk_low.temp - prev_temp;
    track_temp_alloc(hdr->size, hunk_high.temp + hunk_low.temp, hunk_low.permanent, name);
    hdr->name = name;
    return (uint *)bufa;
}

void __cdecl Hunk_FreeTempMemory(char *buf)
{
    hunkHeader_t *hdr; // [esp+0h] [ebp-10h]
    uint8_t *endBuf;   // [esp+4h] [ebp-Ch]
    uint8_t *beginBuf; // [esp+Ch] [ebp-4h]

#ifdef KISAK_MP
    iassert(Sys_IsMainThread());
#endif

    if (s_hunkData)
    {
        iassert(buf);
        hdr = (hunkHeader_t *)(buf - sizeof(hunkHeader_t));
        if (hdr->magic != 0x89537892)
        {
            Com_Error(ERR_FATAL, "Hunk_FreeTempMemory: bad magic");
        }
        hdr->magic = -1991018349;
        if (hdr != (hunkHeader_t *)&s_hunkData[(hunk_low.temp - hdr->size + 15) & 0xFFFFFFF0])
        {
            MyAssertHandler(
                ".\\universal\\com_memory.cpp",
                2362,
                0,
                "%s",
                "hdr == (void *)( s_hunkData + ((hunk_low.temp - hdr->size + 15) & ~15) )");
        }
        endBuf = (uint8_t *)((uintptr_t)&s_hunkData[hunk_low.temp + 4095] & ~(uintptr_t)0xFFF);
        hunk_low.temp -= hdr->size;
        track_temp_free(hdr->size, hunk_low.permanent, hdr->name);
        beginBuf = (uint8_t *)((uintptr_t)&s_hunkData[hunk_low.temp + 4095] & ~(uintptr_t)0xFFF);
        if (endBuf != beginBuf)
        {
            Z_VirtualDecommit(beginBuf, endBuf - beginBuf);
        }
    }
    else
    {
        Z_Free(buf, 10);
    }
}

void Hunk_ClearTempMemory()
{
    uint8_t *endBuf;   // [esp+0h] [ebp-Ch]
    uint8_t *beginBuf; // [esp+8h] [ebp-4h]

    iassert(Sys_IsMainThread());
    iassert(s_hunkData);
    endBuf = (uint8_t *)((uintptr_t)&s_hunkData[hunk_low.temp + 4095] & ~(uintptr_t)0xFFF);
    hunk_low.temp = hunk_low.permanent;
    beginBuf = (uint8_t *)((uintptr_t)&s_hunkData[hunk_low.permanent + 4095] & ~(uintptr_t)0xFFF);
    if (endBuf != beginBuf)
    {
        Z_VirtualDecommit(beginBuf, endBuf - beginBuf);
    }
    // track_temp_clear(hunk_low.permanent);
}

void Hunk_CheckTempMemoryClear()
{
#ifdef KISAK_MP
    iassert(Sys_IsMainThread());
#endif
    iassert(s_hunkData);
    iassert(hunk_low.temp == hunk_low.permanent);
}

void Hunk_CheckTempMemoryHighClear()
{
#ifdef KISAK_MP
    iassert(Sys_IsMainThread());
#endif
    iassert(s_hunkData);
    iassert(hunk_high.temp == hunk_high.permanent);
}

int __cdecl Hunk_HideTempMemory()
{
    int mark; // [esp+0h] [ebp-4h]

    iassert(Sys_IsMainThread());
    mark = hunk_low.permanent;
    hunk_low.permanent = hunk_low.temp;
    return mark;
}

void __cdecl Hunk_ShowTempMemory(int mark)
{
    iassert(Sys_IsMainThread());
    Hunk_CheckTempMemoryClear();
    hunk_low.permanent = mark;
}

int __cdecl LargeLocalBegin(int size)
{
    int startPos; // [esp+0h] [ebp-4h]
    uint sizea;   // [esp+Ch] [ebp+8h]

    iassert(Sys_IsMainThread());
    sizea = LargeLocalRoundSize(size);
    startPos = g_largeLocalPos;
    if (sizea > sizeof(g_largeLocalBuf) - g_largeLocalPos)
    {
        Com_Error(ERR_DROP, "LargeLocalBegin: out of memory");
    }
    g_largeLocalPos += sizea;
    return startPos;
}

uint __cdecl LargeLocalRoundSize(int size)
{
    if (size < 0 || size > sizeof(g_largeLocalBuf))
    {
        Com_Error(ERR_DROP, "LargeLocalRoundSize: invalid size %d", size);
    }
    return (size + 15u) & ~15u;
}

void __cdecl LargeLocalEnd(int startPos)
{
    iassert(Sys_IsMainThread());
    g_largeLocalPos = startPos;
}

uint8_t* __cdecl LargeLocalGetBuf(int startPos)
{
    iassert(Sys_IsMainThread());
    return &g_largeLocalBuf[startPos];
}

LargeLocal::LargeLocal(int sizeParam)
{
    iassert(Sys_IsMainThread());

    this->startPos = LargeLocalBegin(sizeParam);
    this->size = sizeParam;
}

LargeLocal::~LargeLocal()
{
    iassert(Sys_IsMainThread());
    LargeLocalEnd(this->startPos);
}

uint8_t* LargeLocal::GetBuf()
{
    iassert(Sys_IsMainThread());
    return LargeLocalGetBuf(this->startPos);
}

void __cdecl LargeLocalReset()
{
    iassert(Sys_IsMainThread());
    g_largeLocalPos = 0;
}

void __cdecl Hunk_InitDebugMemory()
{
    iassert(Sys_IsMainThread());
    iassert(!g_debugUser);
    g_debugUser = Hunk_UserCreate(0x1000000, "Hunk_InitDebugMemory", 0, 0, 0);
}

void __cdecl Hunk_ShutdownDebugMemory()
{
    iassert(Sys_IsMainThread());
    iassert(g_debugUser);
    Hunk_UserDestroy(g_debugUser);
    g_debugUser = 0;
}

void __cdecl Hunk_ResetDebugMem()
{
    iassert(Sys_IsMainThread());
    iassert(g_debugUser);
    Hunk_UserReset(g_debugUser);
}

void* Hunk_AllocDebugMem(uint size)
{
    iassert(Sys_IsMainThread());
    iassert(g_debugUser);

    return Hunk_UserAlloc(g_debugUser, size, alignof(void *));
}

void __cdecl Hunk_FreeDebugMem(void* ptr)
{
    iassert(Sys_IsMainThread());
    iassert(g_debugUser);
}

HunkUser *Hunk_UserCreate(int maxSize, const char *name, bool fixed, bool tempMem, int type)
{
    if (maxSize < 4096 || maxSize % 4096)
    {
        Com_Error(ERR_FATAL, "Invalid user hunk size");
    }
    HunkUser *user = (HunkUser *)Z_VirtualReserve(maxSize);
    Z_VirtualCommit(user, offsetof(HunkUser, buf));
    user->end = (uintptr_t)user + maxSize;
    user->pos = (uintptr_t)user->buf;
    user->maxSize = maxSize;
    user->current = user;
    user->next = 0;
    user->fixed = fixed;
    user->name = name;
    user->tempMem = tempMem;
    user->type = type;
    return user;
}

void *Hunk_UserAlloc(HunkUser *user, uint size, int alignment)
{
    iassert(user);
    if (alignment <= 0 || (alignment & (alignment - 1)) || alignment > HUNK_MAX_ALIGNEMT || size > (size_t)user->maxSize - offsetof(HunkUser, buf))
    {
        Com_Error(ERR_FATAL, "Invalid user hunk allocation");
    }
    uintptr_t mask = (uintptr_t)alignment - 1;
    for (;;)
    {
        HunkUser *current = user->current;
        uintptr_t result = (current->pos + mask) & ~mask;
        if (result <= current->end && size <= current->end - result)
        {
            uintptr_t commitStart = (current->pos + 4095) & ~(uintptr_t)4095;
            current->pos = result + size;
            if (current->pos > commitStart)
            {
                Z_VirtualCommit((void *)commitStart, (int)(current->pos - commitStart));
            }
            return (void *)result;
        }
        if (user->fixed || size > (size_t)user->maxSize - ((offsetof(HunkUser, buf) + mask) & ~mask))
        {
            Com_Error(ERR_FATAL, "Hunk_UserAlloc: out of memory");
        }
        HunkUser *next = Hunk_UserCreate(user->maxSize, user->name, false, user->tempMem, user->type);
        current->next = next;
        user->current = next;
    }
}

void* Hunk_UserAllocAlignStrict(HunkUser* user, uint size)
{
    return Hunk_UserAlloc(user, size, 1);
}

void __cdecl Hunk_UserSetPos(HunkUser* user, uint8_t* pos)
{
    iassert(user->fixed);
    iassert(pos >= user->buf);
    iassert((uintptr_t)pos <= user->pos); // (psize_int)pos <= user->pos

    user->pos = (uintptr_t)pos;
}

void __cdecl Hunk_UserReset(HunkUser *user)
{
    if (user->next)
    {
        Hunk_UserDestroy(user->next);
        user->next = 0;
    }
    user->current = user;
    uintptr_t firstPageEnd = (uintptr_t)user + 4096;
    if (user->pos > firstPageEnd)
    {
        Z_VirtualDecommit((void *)firstPageEnd, (int)(user->pos - firstPageEnd));
    }
    user->pos = (uintptr_t)user->buf;
    memset(user->buf, 0, 4096 - offsetof(HunkUser, buf));
}

void __cdecl Hunk_UserDestroy(HunkUser* user)
{
    HunkUser* current; // [esp+0h] [ebp-8h]
    HunkUser* newCurrent; // [esp+4h] [ebp-4h]

    for (current = user->next; current; current = newCurrent)
    {
        newCurrent = current->next;
        Z_VirtualFree(current);
    }
    Z_VirtualFree(user);
}

char* __cdecl Hunk_CopyString(HunkUser* user, const char* in)
{
    char v3; // [esp+3h] [ebp-21h]
    char* v4; // [esp+8h] [ebp-1Ch]
    const char* v5; // [esp+Ch] [ebp-18h]
    char* out; // [esp+20h] [ebp-4h]

    out = (char*)Hunk_UserAlloc(user, strlen(in) + 1, 1);
    v5 = in;
    v4 = out;
    do
    {
        v3 = *v5;
        *v4++ = *v5++;
    } while (v3);
    return out;
}

uint8_t* __cdecl Hunk_AllocXModelPrecache(uint size)
{
    return Hunk_Alloc(size, "Hunk_AllocXModelPrecache", 21);
}

uint8_t* __cdecl Hunk_AllocXModelPrecacheColl(uint size)
{
    return Hunk_Alloc(size, "Hunk_AllocXModelPrecacheColl", 27);
}

int Hunk_SetMarkLow()
{
    iassert(Sys_IsMainThread());
    Hunk_CheckTempMemoryClear();
    return hunk_low.permanent;
}











static char* __cdecl Z_TryMallocGarbage(int size, const char* name, int type)
{
    char* buf; // [esp+0h] [ebp-4h]

    buf = (char*)malloc(size);
    // LWSS: remove this +32. Not needed
    //buf = (char*)malloc(size + 32);
    //if (buf)
    //{
    //    buf += 32;
    //    track_z_alloc(size + 72, name, type, buf, 0, 32); // KISAKMEMTRACK
    //}
    return buf;
}

static uint* __cdecl Z_TryMalloc(int size, const char* name, int type)
{
    uint* buf; // [esp+0h] [ebp-4h]

    buf = (uint*)Z_TryMallocGarbage(size, name, type);
    if (buf)
        Com_Memset(buf, 0, size);
    return buf;
}

static void __cdecl Z_MallocFailed(int size)
{
    Com_PrintError(CON_CHANNEL_SYSTEM, "Failed to Z_Malloc %i bytes\n", size);
    Sys_OutOfMemErrorInternal(".\\universal\\com_memory.cpp", 593);
}

void* Z_Malloc(int size, const char* name, int type)
{
    uint* buf; // [esp+0h] [ebp-4h]

    buf = Z_TryMalloc(size, name, type);

    //if (!buf)
    //    Z_MallocFailed(size + 32);

    return buf;
}

void __cdecl Z_Free(void *ptr, int type)
{
    if (ptr)
    {
       // track_z_free(type, ptr, 32);
       //free((char*)ptr - 32);
       free(ptr);
    }
}


char *__cdecl Z_MallocGarbage(int size, const char *name, int type)
{
    char *buf; // [esp+0h] [ebp-4h]

    buf = Z_TryMallocGarbage(size, name, type);
    //if (!buf)
    //    Z_MallocFailed(size + 32);
    return buf;
}

char *__cdecl TempMalloc(uint len)
{
    return (char*)Hunk_UserAlloc(g_user, len, 1);
}

void __cdecl TempMemorySetPos(char *pos)
{
    Hunk_UserSetPos(g_user, (byte*)pos);
}

void __cdecl TempMemoryReset(HunkUser *user)
{
    g_user = user;
}

char *__cdecl TempMallocAlignStrict(uint len)
{
    return (char *)Hunk_UserAllocAlignStrict(g_user, len);
}

bool __cdecl TempInfoSort(TempMemInfo *info1, TempMemInfo *info2)
{
    if (!info1->data.type && info2->data.type)
        return 1;
    if (info1->data.type && !info2->data.type)
        return 0;
    if (info1->highExtra < info2->highExtra)
        return 1;
    if (info1->highExtra <= info2->highExtra)
        return info1->high < info2->high;
    return 0;
}

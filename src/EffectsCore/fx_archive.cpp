#include <universal/q_shared.h>
#include "fx_system.h"

#include <database64/database.h>

#include <physics/phys_local.h>

void __cdecl FX_Restore(int clientIndex, MemoryFile *memFile)
{
    FxSystem *system = FX_GetSystem(clientIndex);
    FxSystemBuffers *buffers = FX_GetSystemBuffers(clientIndex);
    FxEffectDefTable table;
    uintptr_t savedSystemAddress;
    iassert(system && buffers);
    FX_RestoreEffectDefTable(memFile, &table);
    MemFile_ReadData(memFile, sizeof(FxSystem), (uint8_t *)system);
    if (!system->isArchiving || system->iteratorCount)
        Com_Error(ERR_DROP, "Invalid save file");
    /* Visibility buffers belong to the separately allocated buffer pool. */
    uintptr_t relocation = (uintptr_t)buffers->visState - (uintptr_t)system->visState;
    FX_RelocateSystem(system, relocation);
    FX_LinkSystemBuffers(system, buffers);
    MemFile_ReadData(memFile, sizeof(FxSystemBuffers), (uint8_t *)buffers);
    FX_FixupEffectDefHandles(system, &table);
    MemFile_ReadData(memFile, sizeof(uintptr_t), (uint8_t *)&savedSystemAddress);
    FX_RestorePhysicsData(system, memFile);
    system->isArchiving = false;
}

void __cdecl FX_RestoreEffectDefTable(MemoryFile *memFile, FxEffectDefTable *table)
{
    uintptr_t p; // [esp+0h] [ebp-10h] BYREF
    const FxEffectDef *effectDef; // [esp+4h] [ebp-Ch]
    uintptr_t key; // [esp+8h] [ebp-8h]
    const char *effectDefName; // [esp+Ch] [ebp-4h]

    table->count = 0;
    while (1)
    {
        effectDefName = MemFile_ReadCString(memFile);
        if (!*effectDefName)
            break;
        MemFile_ReadData(memFile, sizeof(uintptr_t), (uint8_t *)&p);
        key = p;
        effectDef = FX_Register((char *)effectDefName);
        FX_AddEffectDefTableEntry(table, key, effectDef);
    }
}

void __cdecl FX_AddEffectDefTableEntry(FxEffectDefTable *table, uintptr_t key, const FxEffectDef *effectDef)
{
    iassert(table);
    bcassert(table->count, 0x400u);
    iassert(effectDef);
    table->entries[table->count].key = key;
    table->entries[table->count++].effectDef = effectDef;
}

void __cdecl FX_FixupEffectDefHandles(FxSystem *system, FxEffectDefTable *table)
{
    const FxEffectDef *effectDef; // [esp+Ch] [ebp-10h]
    FxEffect *effect; // [esp+10h] [ebp-Ch]
    volatile int activeIndex; // [esp+18h] [ebp-4h]

    iassert(system);
    iassert(system->isArchiving);
    for (activeIndex = system->firstActiveEffect; activeIndex != system->firstNewEffect; ++activeIndex)
    {
        effect = FX_EffectFromHandle(system, system->allEffectHandles[activeIndex & 0x3FF]);
        effectDef = FX_FindEffectDefInTable(table, (uintptr_t)effect->def);
        iassert(effectDef);
        effect->def = effectDef;
    }
}

FxEffect *__cdecl FX_EffectFromHandle(FxSystem *system, uint16_t handle)
{
    const char *v2; // eax

    iassert(system);
    if (handle >= FX_EFFECT_LIMIT * sizeof(FxEffect) / 4 || handle % (sizeof(FxEffect) / 4))
    {
        v2 = va("%p %i", system->effects, handle);
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\effectscore\\fx_system.h",
            257,
            0,
            "%s\n\t%s",
            "handle < FX_EFFECT_LIMIT * sizeof( FxEffect ) / FxEffect::HANDLE_SCALE && handle % (sizeof( FxEffect ) / FxEffect:"
            ":HANDLE_SCALE) == 0",
            v2);
    }
    return (FxEffect *)((char *)system->effects + 4 * handle);
}

const FxEffectDef *__cdecl FX_FindEffectDefInTable(const FxEffectDefTable *table, uintptr_t key)
{
    int index; // [esp+0h] [ebp-4h]

    for (index = 0; index < table->count; ++index)
    {
        if (table->entries[index].key == key)
            return table->entries[index].effectDef;
    }
    return 0;
}

void __cdecl FX_RestorePhysicsData(FxSystem *system, MemoryFile *memFile)
{
    const XModel *visuals; // [esp+18h] [ebp-20h]
    uint16_t elemHandle; // [esp+1Ch] [ebp-1Ch]
    const FxElemDef *elemDef; // [esp+20h] [ebp-18h]
    const FxEffect *effect; // [esp+24h] [ebp-14h]
    uint16_t elemHandleNext; // [esp+2Ch] [ebp-Ch]
    FxPool<FxElem> *elem; // [esp+30h] [ebp-8h]
    volatile int activeIndex; // [esp+34h] [ebp-4h]

    iassert(system);
    iassert(system->isArchiving);
    for (activeIndex = system->firstActiveEffect; activeIndex != system->firstNewEffect; ++activeIndex)
    {
        effect = FX_EffectFromHandle(system, system->allEffectHandles[activeIndex & 0x3FF]);
        for (elemHandle = effect->firstElemHandle[1]; elemHandle != 0xFFFF; elemHandle = elemHandleNext)
        {
            iassert(system);
            elem = FX_PoolFromHandle_Generic<FxElem, 2048>(system->elems, elemHandle);
            elemDef = &effect->def->elemDefs[elem->item.defIndex];
            elemHandleNext = elem->item.nextElemHandleInEffect;
            if (elemDef->elemType == 5 && (elemDef->flags & 0x8000000) != 0)
            {
                elem->item.physObjId = (uintptr_t)Phys_ObjLoad(PHYS_WORLD_FX, memFile);
                visuals = FX_GetElemVisuals(
                    elemDef,
                    (296 * elem->item.sequence + elem->item.msecBegin + (uint)effect->randomSeed) % 0x1DF).model;
                Phys_ObjSetCollisionFromXModel(visuals, PHYS_WORLD_FX, (dxBody *)elem->item.physObjId);
            }
        }
    }
}

FxElemVisuals __cdecl FX_GetElemVisuals(const FxElemDef *elemDef, int randomSeed)
{
    if (!elemDef->visualCount)
        MyAssertHandler(
            "c:\\trees\\cod3\\src\\effectscore\\fx_draw.h",
            79,
            0,
            "%s\n\t(elemDef->visualCount) = %i",
            "(elemDef->visualCount > 0)",
            elemDef->visualCount);
    if (elemDef->visualCount == 1)
        return elemDef->visuals.instance;
    else
        return elemDef->visuals.array[(elemDef->visualCount
            * LOWORD(fx_randomTable[randomSeed + 21])) >> 16];
}

void __cdecl FX_Save(int clientIndex, MemoryFile *memFile)
{
    uint UsedSize; // eax
    uint v3; // eax
    FxSystem *p; // [esp+0h] [ebp-Ch] BYREF
    FxSystem *system; // [esp+4h] [ebp-8h]
    FxSystemBuffers *systemBuffers; // [esp+8h] [ebp-4h]

    system = FX_GetSystem(clientIndex);
    iassert(system);
    systemBuffers = FX_GetSystemBuffers(clientIndex);
    iassert(systemBuffers);
    iassert(!system->isArchiving);
    system->isArchiving = 1;
    FX_SaveEffectDefTable(system, memFile);
    MemFile_WriteData(memFile, sizeof(FxSystem), system);
    UsedSize = MemFile_GetUsedSize(memFile);
    // ProfMem_Begin("systemBuffers", UsedSize);
    MemFile_WriteData(memFile, sizeof(FxSystemBuffers), systemBuffers);
    v3 = MemFile_GetUsedSize(memFile);
    // ProfMem_End(v3);
    p = system;
    MemFile_WriteData(memFile, sizeof(FxSystem *), &p);
    FX_SavePhysicsData(system, memFile);
    system->isArchiving = 0;
}

void __cdecl FX_SaveEffectDefTable(FxSystem *system, MemoryFile *memFile)
{
    if (IsFastFileLoad())
        FX_SaveEffectDefTable_FastFile(memFile);
    else
        FX_SaveEffectDefTable_LoadObj(memFile);
    MemFile_WriteCString(memFile, "");
}

void __cdecl FX_SaveEffectDefTableEntry_FileLoadObj(const FxEffectDef* effectDef, MemoryFile* data)
{
    const FxEffectDef* p; // [esp+0h] [ebp-4h] BYREF

    MemFile_WriteCString(data, (char*)effectDef->name);
    p = effectDef;
    MemFile_WriteData(data, sizeof(const FxEffectDef *), &p);
}

void __cdecl FX_SaveEffectDefTable_LoadObj(MemoryFile* memFile)
{
    FX_ForEachEffectDef((void(__cdecl*)(const FxEffectDef*, void*))FX_SaveEffectDefTableEntry_FileLoadObj, memFile);
}

void __cdecl FX_SaveEffectDefTable_FastFile(MemoryFile *memFile)
{
    DB_EnumXAssets(
        ASSET_TYPE_FX,
        (void(__cdecl *)(XAssetHeader, void *))FX_SaveEffectDefTableEntry_FileLoadObj,
        memFile,
        0);
}

void __cdecl FX_SavePhysicsData(FxSystem *system, MemoryFile *memFile)
{
    uint16_t elemHandle; // [esp+Ch] [ebp-18h]
    const FxElemDef *elemDef; // [esp+10h] [ebp-14h]
    const FxEffect *effect; // [esp+14h] [ebp-10h]
    uint16_t elemHandleNext; // [esp+18h] [ebp-Ch]
    FxPool<FxElem> *elem; // [esp+1Ch] [ebp-8h]
    volatile int activeIndex; // [esp+20h] [ebp-4h]

    iassert(system);
    iassert(system->isArchiving);
    for (activeIndex = system->firstActiveEffect; activeIndex != system->firstNewEffect; ++activeIndex)
    {
        effect = FX_EffectFromHandle(system, system->allEffectHandles[activeIndex & 0x3FF]);
        for (elemHandle = effect->firstElemHandle[1]; elemHandle != 0xFFFF; elemHandle = elemHandleNext)
        {
            iassert(system);
            elem = FX_PoolFromHandle_Generic<FxElem, 2048>(system->elems, elemHandle);
            elemDef = &effect->def->elemDefs[elem->item.defIndex];
            elemHandleNext = elem->item.nextElemHandleInEffect;
            if (elemDef->elemType == 5 && (elemDef->flags & 0x8000000) != 0)
                Phys_ObjSave((dxBody *)elem->item.physObjId, memFile);
        }
    }
}

void __cdecl FX_Archive(int clientIndex, MemoryFile *memFile)
{
    if (MemFile_IsWriting(memFile))
        FX_Save(clientIndex, memFile);
    else
        FX_Restore(clientIndex, memFile);
}


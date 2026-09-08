#include <universal/q_shared.h>
#include "pool_allocator.h"
#include "assertive.h"
#include <qcommon/qcommon.h>
#include <cstdint>

void __cdecl Pool_Init(char *pool, pooldata_t *pooldata, uint itemSize, uint itemCount)
{
    iassert(pool);
    iassert(pooldata);
    pooldata->firstFree = 0;
    pooldata->activeCount = 0;
    if (!itemCount)
    {
        return;
    }
    if (itemSize < sizeof(freenode) || itemSize % alignof(freenode) || (uintptr_t)pool % alignof(freenode))
    {
        Com_Error(ERR_FATAL, "Invalid pool element size or alignment");
    }
    pooldata->firstFree = pool;
    for (uint itemIndex = 0; itemIndex < itemCount; ++itemIndex)
    {
        freenode *item = (freenode *)(pool + (size_t)itemSize * itemIndex);
        item->next = itemIndex + 1 < itemCount ? (freenode *)(pool + (size_t)itemSize * (itemIndex + 1)) : 0;
    }
}

freenode *__cdecl Pool_Alloc(pooldata_t *pooldata)
{
    freenode *item; // [esp+0h] [ebp-4h]

    iassert(pooldata);
    item = (freenode *)pooldata->firstFree;
    if (!pooldata->firstFree)
        return 0;
    pooldata->firstFree = item->next;
    ++pooldata->activeCount;
    return item;
}

void __cdecl Pool_Free(freenode *data, pooldata_t *pooldata)
{
    freenode *item; // [esp+0h] [ebp-4h]

    iassert(data);
    iassert(pooldata);
    for (item = (freenode *)pooldata->firstFree; item; item = item->next)
    {
        iassert(item != data);
    }
    data->next = (freenode *)pooldata->firstFree;
    pooldata->firstFree = data;
    --pooldata->activeCount;
}

uint __cdecl Pool_FreeCount(const pooldata_t *pooldata)
{
    const freenode *item; // [esp+0h] [ebp-8h]
    uint count; // [esp+4h] [ebp-4h]

    iassert(pooldata);
    count = 0;
    for (item = (const freenode *)pooldata->firstFree; item; item = item->next)
        ++count;
    return count;
}


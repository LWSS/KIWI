#include <universal/q_shared.h>
#include "database.h"
#include "db_model_surfaces.h"

static void LoadArray(void **pointer, size_t size, int alignment)
{
    if ((uintptr_t)*pointer == UINTPTR_MAX)
    {
        *pointer = DB_AllocStreamPos(alignment);
        Load_Stream(true, (uint8_t *)*pointer, size);
    }
    else if (*pointer)
    {
        DB64_ConvertOffsetRange((uintptr_t *)pointer, size);
    }
}

static void LoadTree(XSurfaceCollisionTree **tree)
{
    const bool inlineTree = (uintptr_t)*tree == UINTPTR_MAX;
    LoadArray((void **)tree, sizeof(XSurfaceCollisionTree), 15);
    if (!inlineTree)
    {
        return;
    }
    XSurfaceCollisionTree *value = *tree;
    if (value->nodeCount > 65536 || value->leafCount > 65536 || (!!value->nodes != (value->nodeCount != 0)) ||
        (!!value->leafs != (value->leafCount != 0)))
    {
        Com_Error(ERR_DROP, "Invalid native model collision-tree counts");
    }
    LoadArray((void **)&value->nodes, value->nodeCount * sizeof(XSurfaceCollisionNode), 15);
    LoadArray((void **)&value->leafs, value->leafCount * sizeof(XSurfaceCollisionLeaf), 1);
}

void DB64_LoadModelSurface(XSurface *surface, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)surface, sizeof(XSurface));
    unsigned int blendCount = 0;
    unsigned int vertexCount = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (surface->vertInfo.vertCount[i] < 0)
        {
            Com_Error(ERR_DROP, "Negative native model skin partition");
        }
        vertexCount += surface->vertInfo.vertCount[i];
        blendCount += (2 * i + 1) * surface->vertInfo.vertCount[i];
    }
    uint8_t deformed;
    memcpy(&deformed, &surface->deformed, sizeof(uint8_t));
    if (deformed > 1 || (deformed && vertexCount != surface->vertCount) || (!deformed && vertexCount) ||
        (!!surface->vertInfo.vertsBlend != (blendCount != 0)) || (!!surface->verts0 != (surface->vertCount != 0)) ||
        (!!surface->triIndices != (surface->triCount != 0)) || surface->vertListCount > surface->vertCount ||
        (!!surface->vertList != (surface->vertListCount != 0)))
    {
        Com_Error(ERR_DROP, "Invalid native model surface counts");
    }
    Load_GetCurrentZoneHandle(&surface->zoneHandle);
    LoadArray((void **)&surface->vertInfo.vertsBlend, blendCount * sizeof(uint16_t), 1);
    DB_PushStreamPos(7);
    LoadArray((void **)&surface->verts0, surface->vertCount * sizeof(GfxPackedVertex), 15);
    DB_PopStreamPos();
    const bool inlineLists = (uintptr_t)surface->vertList == UINTPTR_MAX;
    LoadArray((void **)&surface->vertList, surface->vertListCount * sizeof(XRigidVertList), 15);
    if (inlineLists)
    {
        for (unsigned int i = 0; i < surface->vertListCount; ++i)
        {
            LoadTree(&surface->vertList[i].collisionTree);
        }
    }
    DB_PushStreamPos(8);
    LoadArray((void **)&surface->triIndices, 3 * surface->triCount * sizeof(uint16_t), 15);
    DB_PopStreamPos();
    char error[256];
    if (!DB64_ValidateModelSurface(surface, 128, error, sizeof(error)))
    {
        Com_Error(ERR_DROP, "%s", error);
    }
}

#include "db_external_assets.h"
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include "database.h"
#include "db_clipmap_assets.h"
#include "db_clipmap_layout.h"
#include "db_collision_primitives.h"
#include "db_model_assets.h"
#include "db_world_assets.h"
#include "db_model_pieces.h"
#include "db_preset_assets.h"
#include "db_effect_references.h"
#include <DynEntity/DynEntity_client.h>

static bool LoadArray(void **array, size_t size, int alignment = 15)
{
    if ((uintptr_t)*array == UINTPTR_MAX)
    {
        *array = DB_AllocStreamPos(alignment);
        Load_Stream(true, (uint8_t *)*array, size);
        return true;
    }
    if (*array)
    {
        DB64_ConvertOffsetRange((uintptr_t *)array, size);
    }
    return false;
}

static void LoadMap(clipMap_t *map)
{
    Load_Stream(true, (uint8_t *)map, sizeof(clipMap_t));
    char error[256];
    if (!DB64_ValidateClipMapHeader(map, error, sizeof(error)))
    {
        Com_Error(ERR_DROP, "%s", error);
    }
    DB_PushStreamPos(4);
    DB64_LoadAssetString(&map->name);
    LoadArray((void **)&map->planes, map->planeCount * sizeof(cplane_s));
    if (LoadArray((void **)&map->staticModelList, map->numStaticModels * sizeof(cStaticModel_s)))
    {
        for (unsigned int i = 0; i < map->numStaticModels; ++i)
        {
            DB64_LoadModelAsset((XAssetHeader *)&map->staticModelList[i].xmodel, false);
        }
    }
    if ((uintptr_t)map->materials == UINTPTR_MAX)
    {
        dmaterial_t *materials = (dmaterial_t *)DB_AllocStreamPos(15);
        Load_Stream(true, (uint8_t *)materials, ((size_t)map->numMaterials + 1) * sizeof(dmaterial_t));
        const dmaterial_t empty = {};
        if (memcmp(materials, &empty, sizeof(dmaterial_t)))
        {
            Com_Error(ERR_DROP, "Invalid native collision fallback material");
        }
        map->materials = materials + 1;
    }
    else if (map->materials)
    {
        const uintptr_t encoded = (uintptr_t)map->materials;
        if (((encoded - 1) & (UINTPTR_MAX >> 4)) < sizeof(dmaterial_t))
        {
            Com_Error(ERR_DROP, "Native collision material array has no fallback slot");
        }
        uintptr_t start = encoded - sizeof(dmaterial_t);
        DB64_ConvertOffsetRange(&start, ((size_t)map->numMaterials + 1) * sizeof(dmaterial_t));
        const dmaterial_t empty = {};
        if (memcmp((void *)start, &empty, sizeof(dmaterial_t)))
        {
            Com_Error(ERR_DROP, "Invalid shared native collision fallback material");
        }
        map->materials = (dmaterial_t *)start + 1;
    }
    if (LoadArray((void **)&map->brushsides, map->numBrushSides * sizeof(cbrushside_t)))
    {
        for (unsigned int i = 0; i < map->numBrushSides; ++i)
        {
            DB64_LoadBrushSide(&map->brushsides[i], false);
        }
    }
    LoadArray((void **)&map->brushEdges, map->numBrushEdges * sizeof(uint8_t), 0);
    if (LoadArray((void **)&map->nodes, map->numNodes * sizeof(cNode_t)))
    {
        for (unsigned int i = 0; i < map->numNodes; ++i)
        {
            if (!map->nodes[i].plane)
            {
                Com_Error(ERR_DROP, "Native BSP node has no plane");
            }
            LoadArray((void **)&map->nodes[i].plane, sizeof(cplane_s));
            for (int child = 0; child < 2; ++child)
            {
                const int index = map->nodes[i].children[child];
                if ((index >= 0 && (unsigned int)index >= map->numNodes) ||
                    (index < 0 && (unsigned int)(-1 - index) >= map->numLeafs))
                {
                    Com_Error(ERR_DROP, "Native BSP node child is out of range");
                }
            }
        }
    }
    if (!DB64_ValidateBspNodes(map, error, sizeof(error)))
    {
        Com_Error(ERR_DROP, "%s", error);
    }
    LoadArray((void **)&map->leafs, map->numLeafs * sizeof(cLeaf_t));
    LoadArray((void **)&map->leafbrushes, map->numLeafBrushes * sizeof(uint16_t), 1);
    if (LoadArray((void **)&map->leafbrushNodes, map->leafbrushNodesCount * sizeof(cLeafBrushNode_s)))
    {
        for (unsigned int i = 0; i < map->leafbrushNodesCount; ++i)
        {
            cLeafBrushNode_s *node = &map->leafbrushNodes[i];
            if (node->leafBrushCount > 0)
            {
                if (!node->data.leaf.brushes)
                {
                    Com_Error(ERR_DROP, "Native leaf brush node has no brush indices");
                }
                LoadArray((void **)&node->data.leaf.brushes, node->leafBrushCount * sizeof(uint16_t), 1);
            }
        }
    }
    LoadArray((void **)&map->leafsurfaces, map->numLeafSurfaces * sizeof(uint));
    LoadArray((void **)&map->verts, map->vertCount * sizeof(float[3]));
    LoadArray((void **)&map->triIndices, map->triCount * sizeof(uint16_t[3]), 1);
    LoadArray((void **)&map->triEdgeIsWalkable, DB64_ClipMapWalkableBytes(map->triCount), 0);
    LoadArray((void **)&map->borders, map->borderCount * sizeof(CollisionBorder));
    if (LoadArray((void **)&map->partitions, map->partitionCount * sizeof(CollisionPartition)))
    {
        for (int i = 0; i < map->partitionCount; ++i)
        {
            DB64_LoadCollisionPartition(&map->partitions[i], false);
        }
    }
    LoadArray((void **)&map->aabbTrees, map->aabbTreeCount * sizeof(CollisionAabbTree));
    if (!DB64_ValidateCollisionTrees(map, error, sizeof(error)))
    {
        Com_Error(ERR_DROP, "%s", error);
    }
    LoadArray((void **)&map->cmodels, map->numSubModels * sizeof(cmodel_t));
    if (LoadArray((void **)&map->brushes, map->numBrushes * sizeof(cbrush_t)))
    {
        for (unsigned int i = 0; i < map->numBrushes; ++i)
        {
            DB64_LoadCollisionBrush(&map->brushes[i], false);
        }
    }
    if (map->visibility)
    {
        LoadArray((void **)&map->visibility, (size_t)map->numClusters * map->clusterBytes, 0);
    }
    DB64_LoadWorldAsset(ASSET_TYPE_MAP_ENTS, (XAssetHeader *)&map->mapEnts, false);
    if (LoadArray((void **)&map->box_brush, sizeof(cbrush_t)))
    {
        DB64_LoadCollisionBrush(map->box_brush, false);
    }
    for (int group = 0; group < 2; ++group)
    {
        const size_t count = map->dynEntCount[group];
        if (LoadArray((void **)&map->dynEntDefList[group], count * sizeof(DynEntityDef)))
        {
            for (size_t i = 0; i < count; ++i)
            {
                DynEntityDef *entity = &map->dynEntDefList[group][i];
                if (!DB64_ValidateDynamicEntity(entity, group, map->numSubModels))
                {
                    Com_Error(ERR_DROP, "Invalid native dynamic entity definition");
                }
                DB64_LoadModelAsset((XAssetHeader *)&entity->xModel, false);
                DB64_LoadAssetString((const char **)&entity->destroyFx);
                DB64_DeferEffectReference((FxEffectDefRef *)&entity->destroyFx);
                DB64_LoadModelPieces((XAssetHeader *)&entity->destroyPieces, false);
                DB64_LoadPresetAsset(ASSET_TYPE_PHYSPRESET, (XAssetHeader *)&entity->physPreset, false);
            }
        }
    }
    DB_PushStreamPos(1);
    for (int kind = 0; kind < 3; ++kind)
    {
        for (int group = 0; group < 2; ++group)
        {
            void **array = kind == 0   ? (void **)&map->dynEntPoseList[group]
                           : kind == 1 ? (void **)&map->dynEntClientList[group]
                                       : (void **)&map->dynEntCollList[group];
            const size_t stride = kind == 0   ? sizeof(DynEntityPose)
                                  : kind == 1 ? sizeof(DynEntityClient)
                                              : sizeof(DynEntityColl);
            if ((uintptr_t)*array != (map->dynEntCount[group] ? UINTPTR_MAX : 0))
            {
                Com_Error(ERR_DROP, "Invalid native dynamic entity runtime array");
            }
            LoadArray(array, map->dynEntCount[group] * stride);
        }
    }
    DB_PopStreamPos();
    DB_PopStreamPos();
}

void DB64_LoadClipMapAsset(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_CLIPMAP, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
    {
        header->clipMap = (clipMap_t *)DB_AllocStreamPos(15);
        const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
        LoadMap(header->clipMap);
        Load_ClipMapAsset(header);
        if (inserted)
        {
            *inserted = header->data;
        }
    }
    else if (token)
    {
        DB_ConvertOffsetToAlias((uintptr_t *)header);
    }
    DB_PopStreamPos();
}

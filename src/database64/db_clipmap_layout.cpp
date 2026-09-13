#include <stdlib.h>
#include <math.h>
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <xanim/xanim.h>
#include <DynEntity/DynEntity_client.h>
#include "db_clipmap_layout.h"
#include <limits.h>
#include <stdio.h>

size_t DB64_ClipMapWalkableBytes(int triangleCount)
{
    if (triangleCount < 0 || triangleCount > INT_MAX / sizeof(uint16_t[3]))
    {
        return SIZE_MAX;
    }
    return (((size_t)triangleCount * 3 + 31) / 32) * sizeof(uint32_t);
}

static bool Count(const void *data, int64_t count, size_t stride, const char *name, char *error, size_t errorSize)
{
    if (count < 0 || (uint64_t)count > INT_MAX / stride || (!!data != (count != 0)))
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid native clipmap %s count or pointer", name);
        }
        return false;
    }
    return true;
}

bool DB64_ValidateClipMapHeader(const clipMap_t *map, char *error, size_t errorSize)
{
    if (!map || !map->name)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Native clipmap has no name");
        }
        return false;
    }
#define CHECK_ARRAY(field, count, type)                                                                                \
    if (!Count(map->field, map->count, sizeof(type), #field, error, errorSize))                                        \
    {                                                                                                                  \
        return false;                                                                                                  \
    }
    CHECK_ARRAY(planes, planeCount, cplane_s);
    CHECK_ARRAY(staticModelList, numStaticModels, cStaticModel_s);
    CHECK_ARRAY(materials, numMaterials, dmaterial_t);
    CHECK_ARRAY(brushsides, numBrushSides, cbrushside_t);
    CHECK_ARRAY(brushEdges, numBrushEdges, uint8_t);
    CHECK_ARRAY(nodes, numNodes, cNode_t);
    CHECK_ARRAY(leafs, numLeafs, cLeaf_t);
    CHECK_ARRAY(leafbrushNodes, leafbrushNodesCount, cLeafBrushNode_s);
    CHECK_ARRAY(leafbrushes, numLeafBrushes, uint16_t);
    CHECK_ARRAY(leafsurfaces, numLeafSurfaces, uint);
    CHECK_ARRAY(verts, vertCount, float[3]);
    CHECK_ARRAY(triIndices, triCount, uint16_t[3]);
    CHECK_ARRAY(borders, borderCount, CollisionBorder);
    CHECK_ARRAY(partitions, partitionCount, CollisionPartition);
    CHECK_ARRAY(aabbTrees, aabbTreeCount, CollisionAabbTree);
    CHECK_ARRAY(cmodels, numSubModels, cmodel_t);
    CHECK_ARRAY(brushes, numBrushes, cbrush_t);
#undef CHECK_ARRAY
    const size_t walkable = DB64_ClipMapWalkableBytes(map->triCount);
    if (walkable == SIZE_MAX ||
        !Count(map->triEdgeIsWalkable, (int64_t)walkable, sizeof(uint8_t), "walkable edges", error, errorSize))
    {
        return false;
    }
    if (map->numClusters < 0 || map->clusterBytes < 0 || (uint64_t)map->numClusters * map->clusterBytes > INT_MAX)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Native clipmap visibility size overflows");
        }
        return false;
    }
    // Visibility can be omitted when the map has no authored PVS.
    if (map->visibility && (!map->numClusters || !map->clusterBytes))
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Native clipmap visibility has no dimensions");
        }
        return false;
    }
    for (int i = 0; i < 2; ++i)
    {
        if (!Count(map->dynEntDefList[i], map->dynEntCount[i], sizeof(DynEntityDef), "dynamic entities", error,
                   errorSize))
        {
            return false;
        }
    }
    return true;
}

bool DB64_ValidateDynamicEntity(const DynEntityDef *entity, int group, unsigned int submodelCount)
{
    if (!entity || entity->type <= DYNENT_TYPE_INVALID || entity->type >= DYNENT_TYPE_COUNT ||
        (group == 0 && (!entity->xModel || entity->brushModel)) ||
        (group == 1 && (entity->xModel || !entity->brushModel || entity->brushModel >= submodelCount)) ||
        (entity->physicsBrushModel && entity->physicsBrushModel >= submodelCount))
    {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        if (!isfinite(entity->pose.origin[axis]) || !isfinite(entity->mass.centerOfMass[axis]) ||
            !isfinite(entity->mass.momentsOfInertia[axis]) || !isfinite(entity->mass.productsOfInertia[axis]))
        {
            return false;
        }
    }
    float lengthSquared = 0;
    for (int axis = 0; axis < 4; ++axis)
    {
        if (!isfinite(entity->pose.quat[axis]))
        {
            return false;
        }
        lengthSquared += entity->pose.quat[axis] * entity->pose.quat[axis];
    }
    return isfinite(lengthSquared) && lengthSquared > 0;
}

bool DB64_ValidateCollisionTrees(const clipMap_t *map, char *error, size_t errorSize)
{
    const char *reason = NULL;
    if (!map || map->aabbTreeCount < 0 || map->aabbTreeCount > INT_MAX / sizeof(CollisionAabbTree) ||
        (!!map->aabbTrees != (map->aabbTreeCount != 0)))
    {
        reason = "Invalid collision AABB count";
    }
    if (reason)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "%s", reason);
        }
        return false;
    }
    const int count = map->aabbTreeCount;
    if (!count)
    {
        return true;
    }
    uint32_t *indegree = (uint32_t *)calloc((size_t)count, sizeof(uint32_t));
    int *queue = (int *)malloc((size_t)count * sizeof(int));
    if (!indegree || !queue)
    {
        reason = "Out of memory validating collision AABB graph";
    }
    for (int i = 0; i < count && !reason; ++i)
    {
        const CollisionAabbTree *tree = &map->aabbTrees[i];
        if (tree->materialIndex >= map->numMaterials)
        {
            reason = "Collision AABB references an invalid material";
            break;
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!isfinite(tree->origin[axis]) || !isfinite(tree->halfSize[axis]) || tree->halfSize[axis] < 0)
            {
                reason = "Collision AABB has invalid bounds";
            }
        }
        const int first = tree->u.firstChildIndex;
        if (tree->childCount)
        {
            if (first < 0 || first > count || tree->childCount > count - first)
            {
                reason = "Collision AABB child range is invalid";
                break;
            }
            for (int j = 0; j < tree->childCount; ++j)
            {
                ++indegree[first + j];
            }
        }
        else if (first < 0 || first >= map->partitionCount)
        {
            reason = "Collision AABB references an invalid partition";
        }
    }
    if (!reason)
    {
        int read = 0, write = 0;
        for (int i = 0; i < count; ++i)
        {
            if (!indegree[i])
            {
                queue[write++] = i;
            }
        }
        while (read < write)
        {
            const CollisionAabbTree *tree = &map->aabbTrees[queue[read++]];
            for (int j = 0; j < tree->childCount; ++j)
            {
                const int child = tree->u.firstChildIndex + j;
                if (!--indegree[child])
                {
                    queue[write++] = child;
                }
            }
        }
        if (read != count)
        {
            reason = "Collision AABB graph contains a cycle";
        }
    }
    free(indegree);
    free(queue);
    if (reason && errorSize)
    {
        snprintf(error, errorSize, "%s", reason);
    }
    return !reason;
}

bool DB64_ValidateBspNodes(const clipMap_t *map, char *error, size_t errorSize)
{
    const char *reason = NULL;
    if (!map || map->numNodes > 32768 || map->numLeafs > 32768 || (!!map->nodes != (map->numNodes != 0)))
    {
        reason = "Invalid native BSP node count";
    }
    uint32_t *indegree = !reason && map->numNodes ? (uint32_t *)calloc(map->numNodes, sizeof(uint32_t)) : NULL;
    uint32_t *queue = !reason && map->numNodes ? (uint32_t *)malloc(map->numNodes * sizeof(uint32_t)) : NULL;
    if (!reason && map->numNodes && (!indegree || !queue))
    {
        reason = "Out of memory validating BSP nodes";
    }
    for (uint i = 0; !reason && i < map->numNodes; ++i)
    {
        const uintptr_t plane = (uintptr_t)map->nodes[i].plane;
        const uintptr_t base = (uintptr_t)map->planes;
        if (!base || plane < base || (plane - base) % sizeof(cplane_s) ||
            (plane - base) / sizeof(cplane_s) >= map->planeCount)
        {
            reason = "Native BSP node has an invalid plane";
            break;
        }
        for (int j = 0; j < 2; ++j)
        {
            const int child = map->nodes[i].children[j];
            if ((child >= 0 && (uint)child >= map->numNodes) || (child < 0 && (uint)(-1 - child) >= map->numLeafs))
            {
                reason = "Native BSP node has an invalid child";
                break;
            }
            if (child >= 0)
            {
                ++indegree[child];
            }
        }
    }
    if (!reason)
    {
        uint read = 0, write = 0;
        for (uint i = 0; i < map->numNodes; ++i)
        {
            if (!indegree[i])
            {
                queue[write++] = i;
            }
        }
        while (read < write)
        {
            const cNode_t *node = &map->nodes[queue[read++]];
            for (int j = 0; j < 2; ++j)
            {
                const int child = node->children[j];
                if (child >= 0 && !--indegree[child])
                {
                    queue[write++] = child;
                }
            }
        }
        if (read != map->numNodes)
        {
            reason = "Native BSP node graph contains a cycle";
        }
    }
    free(indegree);
    free(queue);
    if (reason && errorSize)
    {
        snprintf(error, errorSize, "%s", reason);
    }
    return !reason;
}

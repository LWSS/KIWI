#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <xanim/xanim.h>
#include "native_collision_tree.h"
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <float.h>
#include <math.h>

struct BuildNode
{
    cLeafBrushNode_s node;
    size_t firstBrush;
};
struct BrushTreeBuilder
{
    const clipMap_t *map;
    BuildNode *nodes;
    size_t nodeCount;
    size_t nodeCapacity;
    uint16_t *brushes;
    size_t brushCount;
    size_t brushCapacity;
    char *error;
    size_t errorSize;
};

static bool Fail(BrushTreeBuilder *builder, const char *message)
{
    if (builder->errorSize)
    {
        snprintf(builder->error, builder->errorSize, "%s", message);
    }
    return false;
}

static size_t NewNode(BrushTreeBuilder *builder)
{
    if (builder->nodeCount == builder->nodeCapacity)
    {
        const size_t capacity = builder->nodeCapacity ? builder->nodeCapacity * 2 : 64;
        if (capacity > INT_MAX / sizeof(BuildNode))
        {
            Fail(builder, "Collision brush tree exceeds allocation limits");
            return SIZE_MAX;
        }
        BuildNode *nodes = (BuildNode *)realloc(builder->nodes, capacity * sizeof(BuildNode));
        if (!nodes)
        {
            Fail(builder, "Out of memory building collision brush tree");
            return SIZE_MAX;
        }
        builder->nodes = nodes;
        builder->nodeCapacity = capacity;
    }
    const size_t index = builder->nodeCount++;
    memset(&builder->nodes[index], 0, sizeof(BuildNode));
    return index;
}

static int Side(const cbrush_t *brush, int axis, float distance)
{
    if (brush->mins[axis] >= distance)
    {
        return 0;
    }
    if (brush->maxs[axis] <= distance)
    {
        return 1;
    }
    return 2;
}

static size_t BuildTree(BrushTreeBuilder *builder, const uint16_t *indices, int count, int depth)
{
    const size_t index = NewNode(builder);
    if (index == SIZE_MAX)
    {
        return SIZE_MAX;
    }
    int axis = -1, bestScore = 0;
    float distance = 0;
    if (count > 4 && depth < 48)
    {
        for (int candidate = 0; candidate < 3; ++candidate)
        {
            float lo = FLT_MAX, hi = -FLT_MAX;
            for (int i = 0; i < count; ++i)
            {
                const cbrush_t *brush = &builder->map->brushes[indices[i]];
                if (brush->mins[candidate] < lo)
                {
                    lo = brush->mins[candidate];
                }
                if (brush->maxs[candidate] > hi)
                {
                    hi = brush->maxs[candidate];
                }
            }
            const float split = (float)(((double)lo + hi) * 0.5);
            int sides[3] = {};
            for (int i = 0; i < count; ++i)
            {
                ++sides[Side(&builder->map->brushes[indices[i]], candidate, split)];
            }
            const int score = (sides[0] < sides[1] ? sides[0] : sides[1]) * 2 - sides[2];
            if (sides[0] && sides[1] && score > bestScore)
            {
                bestScore = score;
                axis = candidate;
                distance = split;
            }
        }
    }
    if (axis < 0)
    {
        BuildNode *leaf = &builder->nodes[index];
        leaf->node.leafBrushCount = (int16_t)count;
        leaf->firstBrush = builder->brushCount;
        if (builder->brushCount + count > builder->brushCapacity)
        {
            Fail(builder, "Collision brush tree exceeded its source list size");
            return SIZE_MAX;
        }
        for (int i = 0; i < count; ++i)
        {
            builder->brushes[builder->brushCount++] = indices[i];
            leaf->node.contents |= builder->map->brushes[indices[i]].contents;
        }
        return index;
    }
    uint16_t *scratch = (uint16_t *)malloc((size_t)count * sizeof(uint16_t));
    if (!scratch)
    {
        Fail(builder, "Out of memory partitioning collision brushes");
        return SIZE_MAX;
    }
    builder->nodes[index].node.axis = (uint8_t)axis;
    builder->nodes[index].node.data.children.dist = distance;
    // A zero gap is conservative, and avoids excluding brushes at a split boundary.
    builder->nodes[index].node.data.children.range = 0;
    const int order[3] = {2, 0, 1};
    for (int pass = 0; pass < 3; ++pass)
    {
        const int side = order[pass];
        int childCount = 0;
        for (int i = 0; i < count; ++i)
        {
            if (Side(&builder->map->brushes[indices[i]], axis, distance) == side)
            {
                scratch[childCount++] = indices[i];
            }
        }
        if (!childCount)
        {
            continue;
        }
        const size_t child = BuildTree(builder, scratch, childCount, depth + 1);
        if (child == SIZE_MAX || (side != 2 && child - index > UINT16_MAX))
        {
            free(scratch);
            Fail(builder, "Collision brush tree child offset exceeds 16 bits");
            return SIZE_MAX;
        }
        cLeafBrushNode_s *node = &builder->nodes[index].node;
        node->contents |= builder->nodes[child].node.contents;
        if (side == 2)
        {
            node->leafBrushCount = -1;
        }
        else
        {
            node->data.children.childOffset[side] = (uint16_t)(child - index);
        }
    }
    free(scratch);
    return index;
}

bool Linker_SubdivideCollisionBrushes(clipMap_t *map, char *error, size_t errorSize)
{
    if (!error && errorSize)
    {
        return false;
    }
    if (errorSize)
    {
        error[0] = 0;
    }
    BrushTreeBuilder builder = {};
    builder.map = map;
    builder.error = error;
    builder.errorSize = errorSize;
    if (!map || (map->numLeafs && !map->leafs) || (map->numSubModels && !map->cmodels) ||
        (map->numBrushes && !map->brushes) || (map->leafbrushNodesCount && !map->leafbrushNodes) ||
        (map->numLeafBrushes && !map->leafbrushes))
    {
        return Fail(&builder, "Invalid collision map arrays");
    }
    const size_t rootCount = (size_t)map->numLeafs + map->numSubModels;
    if (rootCount > INT_MAX / sizeof(size_t))
    {
        return Fail(&builder, "Too many collision brush roots");
    }
    size_t *roots = rootCount ? (size_t *)calloc(rootCount, sizeof(size_t)) : NULL;
    if (rootCount && !roots)
    {
        return Fail(&builder, "Out of memory allocating collision brush roots");
    }
    for (size_t i = 0; i < rootCount; ++i)
    {
        const cLeaf_t *leaf = i < map->numLeafs ? &map->leafs[i] : &map->cmodels[i - map->numLeafs].leaf;
        if (leaf->leafBrushNode)
        {
            if (leaf->leafBrushNode < 0 || (uint)leaf->leafBrushNode >= map->leafbrushNodesCount)
            {
                free(roots);
                return Fail(&builder, "Invalid flat collision brush root");
            }
            const cLeafBrushNode_s *source = &map->leafbrushNodes[leaf->leafBrushNode];
            const uintptr_t base = (uintptr_t)map->leafbrushes;
            const uintptr_t pointer = (uintptr_t)source->data.leaf.brushes;
            if (source->leafBrushCount <= 0 || !base || pointer < base || (pointer - base) % sizeof(uint16_t) ||
                (pointer - base) / sizeof(uint16_t) > map->numLeafBrushes ||
                (uint)source->leafBrushCount > map->numLeafBrushes - (pointer - base) / sizeof(uint16_t))
            {
                free(roots);
                return Fail(&builder, "Subdivision requires valid flat collision brush lists");
            }
            for (int j = 0; j < source->leafBrushCount; ++j)
            {
                if (source->data.leaf.brushes[j] >= map->numBrushes)
                {
                    free(roots);
                    return Fail(&builder, "Collision brush list index is invalid");
                }
            }
            for (int j = 0; j < source->leafBrushCount; ++j)
            {
                const cbrush_t *brush = &map->brushes[source->data.leaf.brushes[j]];
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (!isfinite(brush->mins[axis]) || !isfinite(brush->maxs[axis]) ||
                        brush->mins[axis] > brush->maxs[axis])
                    {
                        free(roots);
                        return Fail(&builder, "Invalid collision brush bounds");
                    }
                }
            }
            builder.brushCapacity += source->leafBrushCount;
        }
    }
    if (builder.brushCapacity > INT_MAX / sizeof(uint16_t))
    {
        free(roots);
        return Fail(&builder, "Collision brush lists exceed allocation limits");
    }
    builder.brushes = builder.brushCapacity ? (uint16_t *)malloc(builder.brushCapacity * sizeof(uint16_t)) : NULL;
    bool ok = (!builder.brushCapacity || builder.brushes) && NewNode(&builder) != SIZE_MAX;
    for (size_t i = 0; ok && i < rootCount; ++i)
    {
        const cLeaf_t *leaf = i < map->numLeafs ? &map->leafs[i] : &map->cmodels[i - map->numLeafs].leaf;
        if (leaf->leafBrushNode)
        {
            const cLeafBrushNode_s *source = &map->leafbrushNodes[leaf->leafBrushNode];
            roots[i] = BuildTree(&builder, source->data.leaf.brushes, source->leafBrushCount, 0);
            ok = roots[i] != SIZE_MAX;
        }
    }
    cLeafBrushNode_s *nodes = ok ? (cLeafBrushNode_s *)malloc(builder.nodeCount * sizeof(cLeafBrushNode_s)) : NULL;
    ok = ok && nodes;
    if (ok)
    {
        for (size_t i = 0; i < builder.nodeCount; ++i)
        {
            nodes[i] = builder.nodes[i].node;
            if (nodes[i].leafBrushCount > 0)
            {
                nodes[i].data.leaf.brushes = builder.brushes + builder.nodes[i].firstBrush;
            }
        }
        for (size_t i = 0; i < rootCount; ++i)
        {
            cLeaf_t *leaf = i < map->numLeafs ? &map->leafs[i] : &map->cmodels[i - map->numLeafs].leaf;
            leaf->leafBrushNode = (int)roots[i];
        }
        free(map->leafbrushNodes);
        free(map->leafbrushes);
        map->leafbrushNodes = nodes;
        map->leafbrushNodesCount = (uint)builder.nodeCount;
        map->leafbrushes = builder.brushes;
        map->numLeafBrushes = (uint)builder.brushCount;
    }
    else
    {
        free(nodes);
        free(builder.brushes);
        if (!errorSize || !error[0])
        {
            Fail(&builder, "Cannot build collision brush subdivision");
        }
    }
    free(builder.nodes);
    free(roots);
    return ok;
}

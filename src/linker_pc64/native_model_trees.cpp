#include <universal/q_shared.h>
#include <gfx_d3d/r_gfx.h>
#include "native_bsp.h"
#include "native_model_cells.h"
#include "native_model_trees.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ModelTreeNode
{
    GfxAabbTree data;
    unsigned int *children;
    unsigned int modelCapacity;
};
struct ModelTreeBuilder
{
    ModelTreeNode *nodes;
    unsigned int count, capacity;
};

static bool ReserveNode(ModelTreeBuilder *builder)
{
    if (builder->count < builder->capacity)
    {
        return true;
    }
    const unsigned int maximum = INT_MAX / sizeof(ModelTreeNode);
    if (builder->count >= maximum)
    {
        return false;
    }
    unsigned int capacity = builder->capacity ? builder->capacity * 2 : 64;
    if (capacity > maximum)
    {
        capacity = maximum;
    }
    ModelTreeNode *nodes = (ModelTreeNode *)realloc(builder->nodes, capacity * sizeof(ModelTreeNode));
    if (!nodes)
    {
        return false;
    }
    memset(nodes + builder->capacity, 0, (capacity - builder->capacity) * sizeof(ModelTreeNode));
    builder->nodes = nodes;
    builder->capacity = capacity;
    return true;
}

static bool Contains(const GfxAabbTree *tree, const GfxStaticModelInst *model)
{
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (model->mins[i] < tree->mins[i] || model->maxs[i] > tree->maxs[i])
        {
            return false;
        }
    }
    return true;
}

static bool InsertModel(ModelTreeBuilder *builder, unsigned int nodeIndex, unsigned int modelIndex,
                        const GfxStaticModelInst *model)
{
    unsigned int visits = 0;
    for (;;)
    {
        if (nodeIndex >= builder->count || ++visits > builder->count)
        {
            return false;
        }
        ModelTreeNode *node = &builder->nodes[nodeIndex];
        unsigned int count = node->data.smodelIndexCount;
        if (count == 65535)
        {
            return false;
        }
        if (count == node->modelCapacity)
        {
            unsigned int capacity = count ? count * 2 : 4;
            if (capacity > 65535)
            {
                capacity = 65535;
            }
            uint16_t *indices = (uint16_t *)realloc(node->data.smodelIndexes, capacity * sizeof(uint16_t));
            if (!indices)
            {
                return false;
            }
            node->data.smodelIndexes = indices;
            node->modelCapacity = capacity;
        }
        node->data.smodelIndexes[count] = (uint16_t)modelIndex;
        ++node->data.smodelIndexCount;
        for (unsigned int i = 0; i < 3; ++i)
        {
            if (model->mins[i] < node->data.mins[i])
            {
                node->data.mins[i] = model->mins[i];
            }
            if (model->maxs[i] > node->data.maxs[i])
            {
                node->data.maxs[i] = model->maxs[i];
            }
        }
        if (!node->data.childCount)
        {
            return true;
        }
        unsigned int child = UINT_MAX, empty = UINT_MAX;
        for (unsigned int i = 0; i < node->data.childCount; ++i)
        {
            unsigned int index = node->children[i];
            if (index >= builder->count)
            {
                return false;
            }
            if (Contains(&builder->nodes[index].data, model))
            {
                child = index;
                break;
            }
            if (empty == UINT_MAX && !builder->nodes[index].data.surfaceCount)
            {
                empty = index;
            }
        }
        if (child == UINT_MAX)
        {
            child = empty;
        }
        if (child == UINT_MAX)
        {
            if (node->data.childCount == 65535 || !ReserveNode(builder))
            {
                return false;
            }
            node = &builder->nodes[nodeIndex]; // realloc may have moved the node table.
            unsigned int *children =
                (unsigned int *)realloc(node->children, (node->data.childCount + 1) * sizeof(unsigned int));
            if (!children)
            {
                return false;
            }
            node->children = children;
            child = builder->count++;
            node->children[node->data.childCount++] = child;
            memcpy(builder->nodes[child].data.mins, model->mins, sizeof(float[3]));
            memcpy(builder->nodes[child].data.maxs, model->maxs, sizeof(float[3]));
        }
        nodeIndex = child;
    }
}

struct FlattenFrame
{
    unsigned int source, destination;
};

static bool Flatten(const ModelTreeBuilder *builder, const unsigned int *roots, unsigned int cellCount,
                    LinkerModelTrees *output)
{
    FlattenFrame *stack = (FlattenFrame *)malloc(builder->count * sizeof(FlattenFrame));
    uint8_t *visited = (uint8_t *)calloc(builder->count, sizeof(uint8_t));
    bool valid = stack && visited;
    unsigned int used = 0;
    for (unsigned int cell = 0; valid && cell < cellCount; ++cell)
    {
        if (used == builder->count)
        {
            valid = false;
            break;
        }
        output->cellRoots[cell] = used;
        unsigned int depth = 1;
        stack[0] = {roots[cell], used++};
        while (valid && depth)
        {
            const FlattenFrame frame = stack[--depth];
            if (frame.source >= builder->count || visited[frame.source])
            {
                valid = false;
                break;
            }
            visited[frame.source] = 1;
            const ModelTreeNode *node = &builder->nodes[frame.source];
            GfxAabbTree *tree = &output->trees[frame.destination];
            *tree = node->data;
            tree->smodelIndexes = NULL;
            tree->childrenOffset = 0;
            if (tree->smodelIndexCount)
            {
                tree->smodelIndexes = (uint16_t *)malloc(tree->smodelIndexCount * sizeof(uint16_t));
                if (!tree->smodelIndexes)
                {
                    valid = false;
                    break;
                }
                memcpy(tree->smodelIndexes, node->data.smodelIndexes, tree->smodelIndexCount * sizeof(uint16_t));
            }
            if (tree->childCount)
            {
                if (tree->childCount > builder->count - used || tree->childCount > builder->count - depth)
                {
                    valid = false;
                    break;
                }
                unsigned int first = used;
                used += tree->childCount;
                tree->childrenOffset = (ptrdiff_t)(first - frame.destination) * sizeof(GfxAabbTree);
                for (unsigned int i = tree->childCount; i != 0; --i)
                {
                    stack[depth++] = {node->children[i - 1], first + i - 1};
                }
            }
        }
        output->cellTreeCounts[cell] = used - output->cellRoots[cell];
    }
    if (valid)
    {
        output->treeCount = used;
    }
    free(stack);
    free(visited);
    return valid;
}

void Linker_FreeModelTrees(LinkerModelTrees *trees)
{
    if (trees)
    {
        for (unsigned int i = 0; trees->trees && i < trees->treeCount; ++i)
        {
            free(trees->trees[i].smodelIndexes);
        }
        free(trees->trees);
        free(trees->cellRoots);
        free(trees->cellTreeCounts);
        free(trees);
    }
}

bool Linker_BuildModelTrees(const LinkerBspRenderTrees *sourceTrees, const GfxAabbTree *surfaceTrees,
                            const LinkerBspRenderCells *cells, const LinkerModelCells *membership,
                            const GfxStaticModelInst *models, LinkerModelTrees **result, char *error, size_t errorSize)
{
    if (!result || (!error && errorSize))
    {
        return false;
    }
    *result = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    bool valid = sourceTrees && sourceTrees->trees && sourceTrees->treeCount && sourceTrees->treeCount <= 65536 &&
                 surfaceTrees && cells && cells->cells && cells->cellCount && cells->cellCount <= 1024 && membership &&
                 membership->cellCount == cells->cellCount && membership->modelCount <= 65535 &&
                 membership->wordsPerModel == (cells->cellCount + 31) / 32 &&
                 (!membership->modelCount || (membership->bits && models));
    ModelTreeBuilder builder = {};
    unsigned int *roots = valid ? (unsigned int *)malloc(cells->cellCount * sizeof(unsigned int)) : NULL;
    valid = valid && roots;
    for (unsigned int i = 0; valid && i < sourceTrees->treeCount; ++i)
    {
        valid = ReserveNode(&builder);
        if (!valid)
        {
            break;
        }
        ModelTreeNode *node = &builder.nodes[builder.count++];
        node->data = surfaceTrees[i];
        node->data.smodelIndexes = NULL;
        valid = !surfaceTrees[i].smodelIndexCount && !surfaceTrees[i].smodelIndexes;
        for (unsigned int axis = 0; valid && axis < 3; ++axis)
        {
            valid = isfinite(node->data.mins[axis]) && isfinite(node->data.maxs[axis]) &&
                    node->data.mins[axis] <= node->data.maxs[axis];
        }
        if (valid && node->data.childCount)
        {
            ptrdiff_t offset = node->data.childrenOffset;
            valid = offset > 0 && offset % sizeof(GfxAabbTree) == 0 &&
                    (size_t)offset / sizeof(GfxAabbTree) < sourceTrees->treeCount - i;
            size_t first = valid ? i + offset / sizeof(GfxAabbTree) : 0;
            valid = valid && node->data.childCount <= sourceTrees->treeCount - first;
            if (valid)
            {
                node->children = (unsigned int *)malloc(node->data.childCount * sizeof(unsigned int));
                valid = node->children != NULL;
                for (unsigned int j = 0; valid && j < node->data.childCount; ++j)
                {
                    node->children[j] = (unsigned int)first + j;
                }
            }
        }
    }
    for (unsigned int m = 0; valid && m < membership->modelCount; ++m)
    {
        for (unsigned int i = 0; valid && i < 3; ++i)
        {
            valid =
                isfinite(models[m].mins[i]) && isfinite(models[m].maxs[i]) && models[m].mins[i] <= models[m].maxs[i];
        }
    }
    for (unsigned int c = 0; valid && c < cells->cellCount; ++c)
    {
        uintptr_t base = (uintptr_t)sourceTrees->trees, root = (uintptr_t)cells->cells[c].aabbTree;
        valid = root >= base && (root - base) % sizeof(GfxAabbTree) == 0 &&
                (root - base) / sizeof(GfxAabbTree) < sourceTrees->treeCount;
        if (!valid)
        {
            break;
        }
        roots[c] = (unsigned int)((root - base) / sizeof(GfxAabbTree));
        for (unsigned int m = 0; valid && m < membership->modelCount; ++m)
        {
            if (membership->bits[(size_t)m * membership->wordsPerModel + c / 32] & ((uint32_t)1 << (c % 32)))
            {
                valid = InsertModel(&builder, roots[c], m, &models[m]);
            }
        }
    }
    LinkerModelTrees *output = valid ? (LinkerModelTrees *)calloc(1, sizeof(LinkerModelTrees)) : NULL;
    valid = valid && output;
    if (valid)
    {
        output->treeCount = builder.count;
        output->cellCount = cells->cellCount;
        output->trees = (GfxAabbTree *)calloc(builder.count, sizeof(GfxAabbTree));
        output->cellRoots = (unsigned int *)calloc(cells->cellCount, sizeof(unsigned int));
        output->cellTreeCounts = (unsigned int *)calloc(cells->cellCount, sizeof(unsigned int));
        valid = output->trees && output->cellRoots && output->cellTreeCounts &&
                Flatten(&builder, roots, cells->cellCount, output);
    }
    for (unsigned int i = 0; i < builder.count; ++i)
    {
        free(builder.nodes[i].children);
        free(builder.nodes[i].data.smodelIndexes);
    }
    free(builder.nodes);
    free(roots);
    if (!valid)
    {
        Linker_FreeModelTrees(output);
        if (errorSize)
        {
            snprintf(error, errorSize, "Cannot build model AABB trees: invalid topology, bounds or capacity");
        }
        return false;
    }
    *result = output;
    return true;
}

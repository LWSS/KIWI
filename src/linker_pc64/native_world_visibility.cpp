#include <universal/q_shared.h>
#include <gfx_d3d/r_bsp.h>
#include "native_bsp.h"
#include "native_world_sort.h"
#include "native_world_visibility.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct VisibilityTriangle
{
    float xyz[3][3];
    unsigned int model, surface, materialRank;
};

static int CompareTriangleGeometry(const VisibilityTriangle *a, const VisibilityTriangle *b)
{
    if (a->model != b->model)
    {
        return (a->model > b->model) - (a->model < b->model);
    }
    for (unsigned int i = 0; i < 3; ++i)
    {
        for (unsigned int j = 0; j < 3; ++j)
        {
            if (a->xyz[i][j] != b->xyz[i][j])
            {
                return (a->xyz[i][j] > b->xyz[i][j]) - (a->xyz[i][j] < b->xyz[i][j]);
            }
        }
    }
    return 0;
}

static int CompareTriangles(const void *left, const void *right)
{
    const VisibilityTriangle *a = (const VisibilityTriangle *)left;
    const VisibilityTriangle *b = (const VisibilityTriangle *)right;
    int result = CompareTriangleGeometry(a, b);
    if (!result)
    {
        result = (a->materialRank > b->materialRank) - (a->materialRank < b->materialRank);
    }
    return result;
}

static bool ClassifyDecals(const LinkerBspRenderGeometry *geometry, const LinkerBspRenderGroups *groups,
                           const uint16_t *ranks, unsigned int rankCount, const LinkerWorldSurfaceOrder *order,
                           uint8_t *nonDecal)
{
    size_t triangleCount = 0;
    uint8_t *assigned = (uint8_t *)calloc(order->surfaceCount ? order->surfaceCount : 1, sizeof(uint8_t));
    bool valid = assigned != NULL;
    for (unsigned int model = 0; valid && model < groups->modelCount; ++model)
    {
        const GfxBrushModel *group = &groups->models[model];
        if (!group->surfaceCount)
        {
            continue;
        }
        valid = group->startSurfIndex <= order->surfaceCount &&
                group->surfaceCount <= order->surfaceCount - group->startSurfIndex;
        for (unsigned int i = 0; valid && i < group->surfaceCount; ++i)
        {
            unsigned int index = group->startSurfIndex + i;
            const GfxSurface *surface = &order->surfaces[index];
            valid = !assigned[index] && order->materialIndices[index] < rankCount &&
                    surface->tris.firstVertex <= geometry->vertexCount &&
                    surface->tris.vertexCount <= geometry->vertexCount - surface->tris.firstVertex &&
                    surface->tris.baseIndex <= geometry->indexCount &&
                    surface->tris.triCount <= (geometry->indexCount - surface->tris.baseIndex) / 3;
            assigned[index] = 1;
            triangleCount += surface->tris.triCount;
            valid = valid && triangleCount <= INT_MAX / sizeof(VisibilityTriangle);
        }
    }
    for (unsigned int i = 0; valid && i < order->surfaceCount; ++i)
    {
        valid = assigned[i] != 0;
    }
    free(assigned);
    VisibilityTriangle *triangles =
        valid && triangleCount ? (VisibilityTriangle *)calloc(triangleCount, sizeof(VisibilityTriangle)) : NULL;
    valid = valid && (!triangleCount || (triangles && geometry->vertices && geometry->indices && ranks));
    size_t next = 0;
    for (unsigned int model = 0; valid && model < groups->modelCount; ++model)
    {
        const GfxBrushModel *group = &groups->models[model];
        for (unsigned int i = 0; valid && i < group->surfaceCount; ++i)
        {
            const unsigned int index = group->startSurfIndex + i;
            const GfxSurface *surface = &order->surfaces[index];
            for (unsigned int t = 0; valid && t < surface->tris.triCount; ++t)
            {
                VisibilityTriangle *triangle = &triangles[next++];
                triangle->model = model;
                triangle->surface = index;
                triangle->materialRank = ranks[order->materialIndices[index]];
                float xyz[3][3];
                for (unsigned int v = 0; valid && v < 3; ++v)
                {
                    unsigned int local = geometry->indices[surface->tris.baseIndex + 3 * t + v];
                    valid = local < surface->tris.vertexCount;
                    if (valid)
                    {
                        memcpy(xyz[v], geometry->vertices[surface->tris.firstVertex + local].xyz, sizeof(float[3]));
                        valid = isfinite(xyz[v][0]) && isfinite(xyz[v][1]) && isfinite(xyz[v][2]);
                    }
                }
                if (!valid)
                {
                    break;
                }
                memcpy(triangle->xyz, xyz, sizeof(xyz));
                // Cyclic rotations match; reversed winding does not. Numeric comparisons equate -0 and +0.
                for (unsigned int rotation = 1; rotation < 3; ++rotation)
                {
                    VisibilityTriangle candidate = *triangle;
                    for (unsigned int v = 0; v < 3; ++v)
                    {
                        memcpy(candidate.xyz[v], xyz[(v + rotation) % 3], sizeof(float[3]));
                    }
                    if (CompareTriangleGeometry(&candidate, triangle) < 0)
                    {
                        memcpy(triangle->xyz, candidate.xyz, sizeof(candidate.xyz));
                    }
                }
            }
        }
    }
    if (valid)
    {
        if (triangleCount > 1)
        {
            qsort(triangles, triangleCount, sizeof(VisibilityTriangle), CompareTriangles);
        }
        size_t first = 0;
        for (size_t i = 0; i < triangleCount; ++i)
        {
            if (CompareTriangleGeometry(&triangles[first], &triangles[i]))
            {
                first = i;
            }
            if (triangles[i].materialRank == triangles[first].materialRank)
            {
                // A surface is a decal layer only when every triangle has a lower-ranked coincident triangle.
                nonDecal[triangles[i].surface] = 1;
            }
        }
    }
    free(triangles);
    return valid;
}

struct VisibilityFrame
{
    unsigned int node, child, firstOutput;
};

static bool BuildTreeIndices(const LinkerBspRenderTrees *source, const LinkerBspRenderCells *cells,
                             const LinkerWorldSurfaceOrder *order, const uint8_t *nonDecal,
                             LinkerWorldVisibility *output)
{
    VisibilityFrame *stack = (VisibilityFrame *)calloc(source->treeCount, sizeof(VisibilityFrame));
    uint8_t *visited = (uint8_t *)calloc(source->treeCount, sizeof(uint8_t));
    uint8_t *surfacesSeen =
        (uint8_t *)calloc(order->staticSurfaceCount ? order->staticSurfaceCount : 1, sizeof(uint8_t));
    uint8_t *sortedSeen = (uint8_t *)calloc(order->staticSurfaceCount ? order->staticSurfaceCount : 1, sizeof(uint8_t));
    bool valid = stack && visited && surfacesSeen && sortedSeen;
    unsigned int cursor = order->staticSurfaceCount;
    for (unsigned int cell = 0; valid && cell < cells->cellCount; ++cell)
    {
        const uintptr_t base = (uintptr_t)source->trees, root = (uintptr_t)cells->cells[cell].aabbTree;
        valid = root >= base && (root - base) % sizeof(GfxAabbTree) == 0 &&
                (root - base) / sizeof(GfxAabbTree) < source->treeCount;
        if (!valid)
        {
            break;
        }
        unsigned int depth = 1;
        stack[0] = {(unsigned int)((root - base) / sizeof(GfxAabbTree)), 0, cursor};
        while (valid && depth)
        {
            VisibilityFrame *frame = &stack[depth - 1];
            GfxAabbTree *tree = &output->trees[frame->node];
            if (!frame->child)
            {
                valid = !visited[frame->node] && cursor <= UINT16_MAX;
                if (!valid)
                {
                    break;
                }
                visited[frame->node] = 1;
                tree->startSurfIndexNoDecal = (uint16_t)cursor;
                frame->firstOutput = cursor;
            }
            if (frame->child < tree->childCount)
            {
                valid = tree->childrenOffset > 0 && tree->childrenOffset % sizeof(GfxAabbTree) == 0;
                const size_t child =
                    valid ? frame->node + tree->childrenOffset / sizeof(GfxAabbTree) + frame->child : 0;
                valid = valid && child < source->treeCount && depth < source->treeCount;
                if (valid)
                {
                    ++frame->child;
                    stack[depth++] = {(unsigned int)child, 0, cursor};
                }
                continue;
            }
            if (!tree->childCount)
            {
                valid = tree->startSurfIndex <= order->staticSurfaceCount &&
                        tree->surfaceCount <= order->staticSurfaceCount - tree->startSurfIndex;
                for (unsigned int i = 0; valid && i < tree->surfaceCount; ++i)
                {
                    unsigned int original = tree->startSurfIndex + i;
                    unsigned int sorted = order->originalToSorted[original];
                    valid = !surfacesSeen[original] && sorted < order->staticSurfaceCount && !sortedSeen[sorted];
                    surfacesSeen[original] = 1;
                    if (valid)
                    {
                        sortedSeen[sorted] = 1;
                    }
                    if (valid && nonDecal[sorted])
                    {
                        valid = cursor < output->sortedIndexCount;
                        if (valid)
                        {
                            output->sortedSurfIndex[cursor++] = (uint16_t)sorted;
                        }
                    }
                }
            }
            valid = valid && cursor - frame->firstOutput <= UINT16_MAX;
            tree->surfaceCountNoDecal = (uint16_t)(cursor - frame->firstOutput);
            --depth;
        }
    }
    for (unsigned int i = 0; valid && i < order->staticSurfaceCount; ++i)
    {
        valid = surfacesSeen[i] != 0;
    }
    valid = valid && cursor == output->sortedIndexCount;
    free(stack);
    free(visited);
    free(surfacesSeen);
    free(sortedSeen);
    return valid;
}

void Linker_FreeWorldVisibility(LinkerWorldVisibility *visibility)
{
    if (visibility)
    {
        free(visibility->models);
        free(visibility->trees);
        free(visibility->sortedSurfIndex);
        free(visibility);
    }
}

bool Linker_BuildWorldVisibility(const LinkerBspRenderGeometry *geometry, const LinkerBspRenderGroups *groups,
                                 const LinkerBspRenderTrees *trees, const LinkerBspRenderCells *cells,
                                 const uint16_t *materialRanks, unsigned int materialRankCount,
                                 LinkerWorldSurfaceOrder *order, LinkerWorldVisibility **visibility, char *error,
                                 size_t errorSize)
{
    if (!visibility || (!error && errorSize))
    {
        return false;
    }
    *visibility = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    bool valid = geometry && groups && groups->models && groups->modelCount && groups->modelCount <= 1024 && trees &&
                 trees->treeCount && trees->treeCount <= 65536 && trees->trees && cells && cells->cellCount &&
                 cells->cellCount <= 1024 && cells->cells && order && order->surfaceCount == geometry->surfaceCount &&
                 order->surfaceCount <= 65536 && order->staticSurfaceCount == groups->models[0].surfaceCount &&
                 order->staticSurfaceCount <= order->surfaceCount &&
                 (!order->surfaceCount || (order->surfaces && order->materialIndices)) &&
                 (!order->staticSurfaceCount || order->originalToSorted) && materialRankCount <= 65536 &&
                 (!materialRankCount || materialRanks);
    LinkerWorldVisibility *result = valid ? (LinkerWorldVisibility *)calloc(1, sizeof(LinkerWorldVisibility)) : NULL;
    uint8_t *nonDecal =
        valid ? (uint8_t *)calloc(order->surfaceCount ? order->surfaceCount : 1, sizeof(uint8_t)) : NULL;
    valid = valid && result && nonDecal &&
            ClassifyDecals(geometry, groups, materialRanks, materialRankCount, order, nonDecal);
    if (valid)
    {
        result->models = (GfxBrushModel *)malloc(groups->modelCount * sizeof(GfxBrushModel));
        result->trees = (GfxAabbTree *)malloc(trees->treeCount * sizeof(GfxAabbTree));
        valid = result->models && result->trees;
    }
    if (valid)
    {
        memcpy(result->models, groups->models, groups->modelCount * sizeof(GfxBrushModel));
        memcpy(result->trees, trees->trees, trees->treeCount * sizeof(GfxAabbTree));
        for (unsigned int m = 0; m < groups->modelCount; ++m)
        {
            GfxBrushModel *model = &result->models[m];
            model->surfaceCountNoDecal = 0;
            for (unsigned int i = 0; i < model->surfaceCount; ++i)
            {
                model->surfaceCountNoDecal += nonDecal[model->startSurfIndex + i];
            }
        }
        result->staticSurfaceCountNoDecal = result->models[0].surfaceCountNoDecal;
        result->sortedIndexCount = order->staticSurfaceCount + result->staticSurfaceCountNoDecal;
        if (result->sortedIndexCount)
        {
            result->sortedSurfIndex = (uint16_t *)malloc(result->sortedIndexCount * sizeof(uint16_t));
            valid = result->sortedSurfIndex != NULL;
            if (valid && order->staticSurfaceCount)
            {
                memcpy(result->sortedSurfIndex, order->originalToSorted, order->staticSurfaceCount * sizeof(uint16_t));
            }
        }
    }
    valid = valid && BuildTreeIndices(trees, cells, order, nonDecal, result);
    if (valid)
    {
        for (unsigned int i = 0; i < order->surfaceCount; ++i)
        {
            order->surfaces[i].flags = (order->surfaces[i].flags & ~2) | (nonDecal[i] ? 0 : 2);
        }
    }
    free(nonDecal);
    if (!valid)
    {
        Linker_FreeWorldVisibility(result);
        if (errorSize)
        {
            snprintf(error, errorSize,
                     "Cannot build world visibility: invalid geometry, tree coverage or index capacity");
        }
        return false;
    }
    *visibility = result;
    return true;
}

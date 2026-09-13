#include <universal/q_shared.h>
#include <gfx_d3d/r_bsp.h>
#include "database.h"
#include "db_render_cells.h"
#include <limits.h>
#include <math.h>

static void *ReadArray(const void *token, size_t count, size_t stride, int alignment)
{
    if (!!token != (count != 0) || count > INT_MAX / stride)
    {
        Com_Error(ERR_DROP, "Invalid native cell array size");
    }
    if (!count)
    {
        return NULL;
    }
    void *result = DB_AllocStreamPos(alignment);
    Load_Stream(true, (uint8_t *)result, count * stride);
    return result;
}

static void Bounds(const float *mins, const float *maxs)
{
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (!isfinite(mins[i]) || !isfinite(maxs[i]) || mins[i] > maxs[i])
        {
            Com_Error(ERR_DROP, "Invalid native cell bounds");
        }
    }
}

void DB64_LoadRenderCells(GfxWorld *world)
{
    const int count = world->dpvsPlanes.cellCount;
    if (count < 0 || count > 1024 || world->cullGroupCount < 0 || world->reflectionProbeCount > 256 ||
        world->dpvs.staticSurfaceCount > 65536 ||
        world->dpvs.staticSurfaceCountNoDecal > world->dpvs.staticSurfaceCount || world->dpvs.smodelCount > 65536)
    {
        Com_Error(ERR_DROP, "Invalid native cell dimensions");
    }
    world->cells = (GfxCell *)ReadArray(world->cells, count, sizeof(GfxCell), 15);
    const size_t sortedCount = world->dpvs.staticSurfaceCount + world->dpvs.staticSurfaceCountNoDecal;
    for (int c = 0; c < count; ++c)
    {
        GfxCell *cell = &world->cells[c];
        Bounds(cell->mins, cell->maxs);
        cell->aabbTree = (GfxAabbTree *)ReadArray(cell->aabbTree, cell->aabbTreeCount, sizeof(GfxAabbTree), 15);
        for (int t = 0; t < cell->aabbTreeCount; ++t)
        {
            GfxAabbTree *tree = &cell->aabbTree[t];
            Bounds(tree->mins, tree->maxs);
            if (tree->startSurfIndex > world->dpvs.staticSurfaceCount ||
                tree->surfaceCount > world->dpvs.staticSurfaceCount - tree->startSurfIndex ||
                tree->startSurfIndexNoDecal > sortedCount ||
                tree->surfaceCountNoDecal > sortedCount - tree->startSurfIndexNoDecal)
            {
                Com_Error(ERR_DROP, "Invalid native cell surface span");
            }
            if (tree->childCount)
            {
                if (tree->childrenOffset <= 0 || tree->childrenOffset % sizeof(GfxAabbTree))
                {
                    Com_Error(ERR_DROP, "Invalid native cell child offset");
                }
                const size_t first = t + (size_t)tree->childrenOffset / sizeof(GfxAabbTree);
                if (first > (size_t)cell->aabbTreeCount || tree->childCount > (size_t)cell->aabbTreeCount - first)
                {
                    Com_Error(ERR_DROP, "Native cell children exceed tree allocation");
                }
            }
            else if (tree->childrenOffset)
            {
                Com_Error(ERR_DROP, "Native leaf has a child offset");
            }
            if (!!tree->smodelIndexes != (tree->smodelIndexCount != 0))
            {
                Com_Error(ERR_DROP, "Invalid native cell model array");
            }
            if (tree->smodelIndexes == (uint16_t *)UINTPTR_MAX)
            {
                tree->smodelIndexes =
                    (uint16_t *)ReadArray(tree->smodelIndexes, tree->smodelIndexCount, sizeof(uint16_t), 1);
            }
            else if (tree->smodelIndexes)
            {
                DB64_ConvertOffsetRange((uintptr_t *)&tree->smodelIndexes, tree->smodelIndexCount * sizeof(uint16_t));
                if ((uintptr_t)tree->smodelIndexes % alignof(uint16_t))
                {
                    Com_Error(ERR_DROP, "Misaligned native cell model references");
                }
            }
            for (unsigned int i = 0; i < tree->smodelIndexCount; ++i)
            {
                if (tree->smodelIndexes[i] >= world->dpvs.smodelCount)
                {
                    Com_Error(ERR_DROP, "Invalid native cell model reference");
                }
            }
        }
        cell->portals = (GfxPortal *)ReadArray(cell->portals, cell->portalCount, sizeof(GfxPortal), 15);
        for (int p = 0; p < cell->portalCount; ++p)
        {
            GfxPortal *portal = &cell->portals[p];
            // All cells are already in the world array; portals must refer to that array.
            DB64_ConvertOffsetRange((uintptr_t *)&portal->cell, sizeof(GfxCell));
            const uintptr_t offset = (uintptr_t)portal->cell - (uintptr_t)world->cells;
            if (offset >= (size_t)count * sizeof(GfxCell) || offset % sizeof(GfxCell) || portal->vertexCount < 3)
            {
                Com_Error(ERR_DROP, "Invalid native portal destination or vertex count");
            }
            portal->writable = {};
            portal->vertices = (float(*)[3])ReadArray(portal->vertices, portal->vertexCount, sizeof(float[3]), 15);
            for (unsigned int v = 0; v < portal->vertexCount; ++v)
            {
                for (unsigned int i = 0; i < 3; ++i)
                {
                    if (!isfinite(portal->vertices[v][i]))
                    {
                        Com_Error(ERR_DROP, "Invalid native portal vertex");
                    }
                }
            }
        }
        cell->cullGroups = (int *)ReadArray(cell->cullGroups, cell->cullGroupCount, sizeof(int), 15);
        for (int i = 0; i < cell->cullGroupCount; ++i)
        {
            if (cell->cullGroups[i] < 0 || cell->cullGroups[i] >= world->cullGroupCount)
            {
                Com_Error(ERR_DROP, "Invalid native cell cull-group reference");
            }
        }
        cell->reflectionProbes =
            (uint8_t *)ReadArray(cell->reflectionProbes, cell->reflectionProbeCount, sizeof(uint8_t), 0);
        for (unsigned int i = 0; i < cell->reflectionProbeCount; ++i)
        {
            if (cell->reflectionProbes[i] >= world->reflectionProbeCount)
            {
                Com_Error(ERR_DROP, "Invalid native cell reflection probe");
            }
        }
    }
}

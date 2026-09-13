#include <universal/q_shared.h>
#include <gfx_d3d/r_bsp.h>
#include "native_bsp.h"
#include "native_world_sort.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SurfaceSortEntry
{
    GfxSurface surface;
    unsigned int originalIndex;
    uint16_t materialIndex, materialRank;
    bool lit, emissive;
};

static int CompareUnsigned(unsigned int a, unsigned int b)
{
    return (a > b) - (a < b);
}

static int CompareSurfaces(const void *left, const void *right)
{
    const SurfaceSortEntry *a = (const SurfaceSortEntry *)left;
    const SurfaceSortEntry *b = (const SurfaceSortEntry *)right;
    int result = CompareUnsigned(b->lit, a->lit);
    if (!result && !a->lit)
    {
        result = CompareUnsigned(b->emissive, a->emissive);
    }
    if (!result)
    {
        result = CompareUnsigned(a->surface.material->info.sortKey, b->surface.material->info.sortKey);
    }
    if (!result)
    {
        result = CompareUnsigned(a->surface.primaryLightIndex, b->surface.primaryLightIndex);
    }
    if (!result)
    {
        result = CompareUnsigned(a->materialRank, b->materialRank);
    }
    if (!result)
    {
        result = CompareUnsigned(a->surface.reflectionProbeIndex, b->surface.reflectionProbeIndex);
    }
    if (!result)
    {
        result = CompareUnsigned(a->surface.lightmapIndex, b->surface.lightmapIndex);
    }
    if (!result)
    {
        result = CompareUnsigned(a->surface.tris.firstVertex, b->surface.tris.firstVertex);
    }
    // R_SortSurfaces temporarily stored this index in vertexCount for its final tie-break.
    if (!result)
    {
        result = CompareUnsigned(a->originalIndex, b->originalIndex);
    }
    return result;
}

void Linker_FreeWorldSurfaceOrder(LinkerWorldSurfaceOrder *order)
{
    if (order)
    {
        free(order->surfaces);
        free(order->materialIndices);
        free(order->originalToSorted);
        free(order);
    }
}

bool Linker_SortWorldSurfaces(const LinkerBspRenderGeometry *geometry, const LinkerBspRenderGroups *groups,
                              const uint16_t *materialRanks, unsigned int materialRankCount,
                              LinkerWorldSurfaceOrder **order, char *error, size_t errorSize)
{
    if (!order || (!error && errorSize))
    {
        return false;
    }
    *order = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    bool valid = geometry && groups && groups->modelCount && groups->models && geometry->surfaceCount <= 65536 &&
                 materialRankCount <= 65536 && materialRankCount == geometry->materialCount &&
                 (!materialRankCount || materialRanks) &&
                 (!geometry->surfaceCount || (geometry->surfaces && geometry->materialIndices));
    unsigned int staticCount = valid ? groups->models[0].surfaceCount : 0;
    valid = valid && staticCount <= geometry->surfaceCount && (!staticCount || groups->models[0].startSurfIndex == 0);
    // A brush submodel must not overlap the world range that is about to move.
    for (unsigned int i = 1; valid && i < groups->modelCount; ++i)
    {
        const GfxBrushModel *model = &groups->models[i];
        valid = !model->surfaceCount ||
                (model->startSurfIndex >= staticCount && model->startSurfIndex <= geometry->surfaceCount &&
                 model->surfaceCount <= geometry->surfaceCount - model->startSurfIndex);
    }
    SurfaceSortEntry *entries =
        valid && staticCount ? (SurfaceSortEntry *)calloc(staticCount, sizeof(SurfaceSortEntry)) : NULL;
    // Distinct materials cannot share a rank; duplicate BSP slots may share the same material.
    Material **rankMaterials = valid ? (Material **)calloc(65536, sizeof(Material *)) : NULL;
    valid = valid && (!staticCount || entries) && rankMaterials;
    for (unsigned int i = 0; valid && i < geometry->surfaceCount; ++i)
    {
        const GfxSurface *surface = &geometry->surfaces[i];
        const unsigned int materialIndex = geometry->materialIndices[i];
        Material *material = surface->material;
        valid = materialIndex < materialRankCount && material && material->techniqueSet && material->info.sortKey < 64;
        if (!valid)
        {
            break;
        }
        const unsigned int rank = materialRanks[materialIndex];
        valid = !rankMaterials[rank] || rankMaterials[rank] == material;
        rankMaterials[rank] = material;
        if (i < staticCount)
        {
            SurfaceSortEntry *entry = &entries[i];
            entry->surface = *surface;
            entry->originalIndex = i;
            entry->materialIndex = (uint16_t)materialIndex;
            entry->materialRank = (uint16_t)rank;
            entry->lit = material->techniqueSet->techniques[TECHNIQUE_LIT_BEGIN] != NULL;
            entry->emissive = material->techniqueSet->techniques[TECHNIQUE_EMISSIVE] != NULL;
        }
    }
    free(rankMaterials);
    LinkerWorldSurfaceOrder *result =
        valid ? (LinkerWorldSurfaceOrder *)calloc(1, sizeof(LinkerWorldSurfaceOrder)) : NULL;
    valid = valid && result;
    if (valid && geometry->surfaceCount)
    {
        result->surfaces = (GfxSurface *)malloc(geometry->surfaceCount * sizeof(GfxSurface));
        result->materialIndices = (uint16_t *)malloc(geometry->surfaceCount * sizeof(uint16_t));
        valid = result->surfaces && result->materialIndices;
        if (valid)
        {
            memcpy(result->surfaces, geometry->surfaces, geometry->surfaceCount * sizeof(GfxSurface));
            memcpy(result->materialIndices, geometry->materialIndices, geometry->surfaceCount * sizeof(uint16_t));
        }
    }
    if (valid && staticCount)
    {
        result->originalToSorted = (uint16_t *)malloc(staticCount * sizeof(uint16_t));
        valid = result->originalToSorted != NULL;
    }
    if (valid)
    {
        if (staticCount > 1)
        {
            qsort(entries, staticCount, sizeof(SurfaceSortEntry), CompareSurfaces);
        }
        result->surfaceCount = geometry->surfaceCount;
        result->staticSurfaceCount = staticCount;
        for (unsigned int i = 0; i < staticCount; ++i)
        {
            result->surfaces[i] = entries[i].surface;
            result->materialIndices[i] = entries[i].materialIndex;
            result->originalToSorted[entries[i].originalIndex] = (uint16_t)i;
        }
        unsigned int i = 0;
        while (i < staticCount && entries[i].lit && entries[i].surface.material->info.sortKey < 24)
        {
            ++i;
        }
        result->litSurfsEnd = result->decalSurfsBegin = i;
        while (i < staticCount && entries[i].lit)
        {
            ++i;
        }
        result->decalSurfsEnd = result->emissiveSurfsBegin = i;
        while (i < staticCount && entries[i].emissive)
        {
            ++i;
        }
        result->emissiveSurfsEnd = i;
    }
    free(entries);
    if (!valid)
    {
        Linker_FreeWorldSurfaceOrder(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Cannot sort world surfaces: invalid model range, material rank or allocation");
        }
        return false;
    }
    *order = result;
    return true;
}

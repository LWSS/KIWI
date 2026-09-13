#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_image.h>
#include <database64/db_render_world_layout.h>
#include "native_world_assemble.h"
#include "native_world_files.h"
#include "native_world_sort.h"
#include "native_world_visibility.h"
#include "native_model_trees.h"
#include "native_grid_collision.h"
#include "native_image.h"
#include "native_sun.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

struct LinkerAssembledWorld
{
    GfxWorld world;
    unsigned int allocationCount;
    void *allocations[64];
    bool failed;
    double outdoorBounds[2][3];
};

static void *Allocate(LinkerAssembledWorld *owner, size_t count, size_t stride)
{
    if (!count)
    {
        return NULL;
    }
    if (owner->allocationCount == ARRAY_COUNT(owner->allocations) || count > SIZE_MAX / stride)
    {
        owner->failed = true;
        return NULL;
    }
    void *memory = calloc(count, stride);
    if (!memory)
    {
        owner->failed = true;
        return NULL;
    }
    owner->allocations[owner->allocationCount++] = memory;
    return memory;
}

void Linker_FreeAssembledWorld(LinkerAssembledWorld *world)
{
    if (world)
    {
        Linker_FreeImage(world->world.outdoorImage);
        for (unsigned int i = 0; i < world->allocationCount; ++i)
        {
            free(world->allocations[i]);
        }
        free(world);
    }
}

GfxWorld *Linker_GetAssembledWorld(LinkerAssembledWorld *world)
{
    return world ? &world->world : NULL;
}

static bool Index(const void *pointer, const void *base, size_t count, size_t stride, size_t *index)
{
    const uintptr_t address = (uintptr_t)pointer, begin = (uintptr_t)base;
    if (!base || address < begin || (address - begin) % stride || (address - begin) / stride >= count)
    {
        return false;
    }
    *index = (address - begin) / stride;
    return true;
}

static bool CopyCells(LinkerAssembledWorld *owner, const LinkerWorldSource *source)
{
    const LinkerBspRenderCells *input = source->cells;
    GfxWorld *world = &owner->world;
    world->cells = (GfxCell *)Allocate(owner, input->cellCount, sizeof(GfxCell));
    GfxPortal *portals = (GfxPortal *)Allocate(owner, input->portalCount, sizeof(GfxPortal));
    if (owner->failed)
    {
        return false;
    }
    for (unsigned int i = 0; i < input->portalCount; ++i)
    {
        size_t cellIndex;
        if (!Index(input->portals[i].cell, input->cells, input->cellCount, sizeof(GfxCell), &cellIndex))
        {
            return false;
        }
        portals[i] = input->portals[i];
        portals[i].cell = &world->cells[cellIndex];
        portals[i].writable = {};
    }
    for (unsigned int i = 0; i < input->cellCount; ++i)
    {
        GfxCell *cell = &world->cells[i];
        *cell = input->cells[i];
        const unsigned int root = source->modelTrees->cellRoots[i], count = source->modelTrees->cellTreeCounts[i];
        if (root >= source->modelTrees->treeCount || count > source->modelTrees->treeCount - root ||
            cell->portalCount < 0)
        {
            return false;
        }
        cell->aabbTree = source->modelTrees->trees + root;
        cell->aabbTreeCount = count;
        cell->portals = NULL;
        if (cell->portalCount)
        {
            size_t first;
            if (!Index(input->cells[i].portals, input->portals, input->portalCount, sizeof(GfxPortal), &first) ||
                (size_t)cell->portalCount > input->portalCount - first)
            {
                return false;
            }
            cell->portals = portals + first;
        }
    }
    return true;
}

static bool SetBoundsAndSky(LinkerAssembledWorld *owner)
{
    GfxWorld *world = &owner->world;
    const Material *sky = NULL;
    world->skySurfCount = 0;
    for (int i = 0; i < world->surfaceCount; ++i)
    {
        const GfxSurface *surface = &world->dpvs.surfaces[i];
        if (!surface->material)
        {
            return false;
        }
        for (unsigned int j = 0; j < 3; ++j)
        {
            const float lo = surface->bounds[0][j], hi = surface->bounds[1][j];
            if (!isfinite(lo) || !isfinite(hi) || lo > hi)
            {
                return false;
            }
            if (!i || lo < world->mins[j])
            {
                world->mins[j] = lo;
            }
            if (!i || hi > world->maxs[j])
            {
                world->maxs[j] = hi;
            }
        }
        if (surface->material->info.gameFlags & 8)
        {
            if ((sky && sky != surface->material) || surface->primaryLightIndex != world->sunPrimaryLightIndex)
            {
                return false;
            }
            sky = surface->material;
            ++world->skySurfCount;
        }
    }
    if (!sky)
    {
        return true;
    }
    if (!sky->textureTable)
    {
        return false;
    }
    unsigned int colorMapHash = 0;
    for (const char *p = "colorMap"; *p; ++p)
    {
        colorMapHash = (*p | 0x20) ^ (33 * colorMapHash);
    }
    for (unsigned int i = 0; i < sky->textureCount; ++i)
    {
        const MaterialTextureDef *texture = &sky->textureTable[i];
        if (texture->nameHash == colorMapHash)
        {
            if (texture->semantic == TS_WATER_MAP || !texture->u.image || texture->u.image->mapType != MAPTYPE_CUBE)
            {
                return false;
            }
            world->skyImage = texture->u.image;
            world->skySamplerState = texture->samplerState;
            break;
        }
    }
    if (!world->skyImage)
    {
        return false;
    }
    world->skyStartSurfs = (int *)Allocate(owner, world->skySurfCount, sizeof(int));
    if (owner->failed)
    {
        return false;
    }
    unsigned int index = 0;
    for (int i = 0; i < world->surfaceCount; ++i)
    {
        if (world->dpvs.surfaces[i].material == sky)
        {
            world->skyStartSurfs[index++] = i;
        }
    }
    return true;
}

static bool SetOutdoorTransform(LinkerAssembledWorld *owner, const LinkerWorldSource *source)
{
    GfxWorld *world = &owner->world;
    double mins[3] = {}, maxs[3] = {};
    bool found = false;
    for (unsigned int i = 0; i < world->dpvs.staticSurfaceCount; ++i)
    {
        const GfxSurface *surface = &world->dpvs.surfaces[i];
        if (!source->surfaceOrder->materialIndices || !source->geometry->materials)
        {
            return false;
        }
        const unsigned int material = source->surfaceOrder->materialIndices[i];
        if (material >= source->geometry->materialCount)
        {
            return false;
        }
        if ((surface->material->info.gameFlags & 8) || !(source->geometry->materials[material].contentFlags & 8193))
        {
            continue;
        }
        for (unsigned int j = 0; j < 3; ++j)
        {
            if (!found || surface->bounds[0][j] < mins[j])
            {
                mins[j] = surface->bounds[0][j];
            }
            if (!found || surface->bounds[1][j] > maxs[j])
            {
                maxs[j] = surface->bounds[1][j];
            }
        }
        found = true;
    }
    memset(world->outdoorLookupMatrix, 0, sizeof(float[4][4]));
    world->outdoorLookupMatrix[3][3] = 1;
    for (unsigned int i = 0; i < 3; ++i)
    {
        // Match Outdoor_ApplyBoundingBox, using explicit maxima instead of its decompiled scale[-3] alias.
        if (maxs[i] - mins[i] < 1)
        {
            mins[i] -= 0.5;
            maxs[i] += 0.5;
        }
        const double scale = 1 / (maxs[i] - mins[i]);
        const float translate = (float)(-mins[i] * scale);
        if (!isfinite(scale) || !isfinite(translate))
        {
            return false;
        }
        world->outdoorLookupMatrix[i][i] = (float)scale;
        world->outdoorLookupMatrix[3][i] = translate;
        owner->outdoorBounds[0][i] = mins[i];
        owner->outdoorBounds[1][i] = maxs[i];
    }
    return true;
}

static bool GenerateOutdoorImage(LinkerAssembledWorld *owner, const LinkerWorldSource *source, char *error,
                                 size_t errorSize)
{
    LinkerGridCollision *collision = NULL;
    if (!Linker_CreateGridCollision(source->collision, &collision, error, errorSize))
    {
        return false;
    }
    const unsigned int width = 512, height = 512;
    uint8_t *pixels = (uint8_t *)malloc((size_t)width * height * sizeof(uint8_t));
    bool valid = pixels != NULL;
    const double *mins = owner->outdoorBounds[0], *maxs = owner->outdoorBounds[1];
    const double xStep = (maxs[0] - mins[0]) / (width - 1);
    const double yStep = (maxs[1] - mins[1]) / (height - 1);
    const double zScale = 255 / (maxs[2] - mins[2]);
    for (unsigned int y = 0; valid && y < height; ++y)
    {
        for (unsigned int x = 0; valid && x < width; ++x)
        {
            float start[3] = {(float)(mins[0] + (x + 0.5) * xStep), (float)(mins[1] + (y + 0.5) * yStep),
                              (float)(maxs[2] + 1)};
            float end[3] = {start[0], start[1], (float)(mins[2] - 1)};
            float fraction;
            valid = Linker_GridCollisionFraction(start, end, &fraction, collision);
            if (valid)
            {
                const double worldHeight = start[2] + ((double)end[2] - start[2]) * fraction;
                const double texel = (worldHeight - mins[2]) * zScale - 0.4999999990686774;
                pixels[(size_t)y * width + x] = texel <= 0 ? 0 : (texel >= 255 ? 255 : (uint8_t)texel);
            }
        }
    }
    char name[1024];
    const int length = snprintf(name, sizeof(name), "outdoor/%s", owner->world.name);
    valid = valid && length > 0 && length < sizeof(name);
    valid = valid && Linker_CreateImage(name, width, height, D3DFMT_L8, IMG_FLAG_NOPICMIP | IMG_FLAG_NOMIPMAPS,
                                        IMG_CATEGORY_AUTO_GENERATED, TS_FUNCTION, IMAGE_TRACK_MISC, pixels,
                                        (size_t)width * height, &owner->world.outdoorImage, error, errorSize);
    free(pixels);
    Linker_FreeGridCollision(collision);
    return valid;
}

static bool AllocateRuntime(LinkerAssembledWorld *owner, char *error, size_t errorSize)
{
    GfxWorld *world = &owner->world;
    DB64WorldRuntimeLayout layout;
    if (!DB64_GetWorldRuntimeLayout(world, &layout, error, errorSize))
    {
        return false;
    }
    world->cellBitsCount = layout.cellBitsBytes;
    world->dpvs.smodelVisDataCount = layout.smodelVisWords;
    world->dpvs.surfaceVisDataCount = layout.surfaceVisWords;
    for (unsigned int i = 0; i < 3; ++i)
    {
        world->dpvs.smodelVisData[i] = (uint8_t *)Allocate(owner, world->dpvs.smodelCount, sizeof(uint8_t));
        world->dpvs.surfaceVisData[i] = (uint8_t *)Allocate(owner, world->dpvs.staticSurfaceCount, sizeof(uint8_t));
        for (unsigned int j = 0; j < 2; ++j)
        {
            world->dpvsDyn.dynEntVisData[j][i] = (uint8_t *)Allocate(owner, layout.dynEntVisBytes[j], sizeof(uint8_t));
        }
    }
    world->dpvs.lodData = (uint *)Allocate(owner, (size_t)layout.smodelVisWords * 2, sizeof(uint));
    world->dpvs.surfaceMaterials = (GfxDrawSurf *)Allocate(owner, world->dpvs.staticSurfaceCount, sizeof(GfxDrawSurf));
    world->dpvs.surfaceCastsSunShadow = (uint *)Allocate(owner, layout.surfaceVisWords, sizeof(uint));
    world->cellCasterBits = (uint *)Allocate(owner, layout.cellCasterWords, sizeof(uint));
    world->dpvsPlanes.sceneEntCellBits = (uint *)Allocate(owner, layout.sceneEntCellWords, sizeof(uint));
    world->primaryLightEntityShadowVis = (uint *)Allocate(owner, layout.primaryLightEntityShadowWords, sizeof(uint));
    for (unsigned int i = 0; i < 2; ++i)
    {
        world->dpvsDyn.dynEntClientWordCount[i] = layout.dynEntWords[i];
        world->dpvsDyn.dynEntCellBits[i] = (uint *)Allocate(owner, layout.dynEntCellWords[i], sizeof(uint));
        world->primaryLightDynEntShadowVis[i] =
            (uint *)Allocate(owner, layout.primaryLightDynEntShadowWords[i], sizeof(uint));
    }
    world->sceneDynModel =
        (GfxSceneDynModel *)Allocate(owner, world->dpvsDyn.dynEntClientCount[0], sizeof(GfxSceneDynModel));
    world->sceneDynBrush =
        (GfxSceneDynBrush *)Allocate(owner, world->dpvsDyn.dynEntClientCount[1], sizeof(GfxSceneDynBrush));
    world->nonSunPrimaryLightForModelDynEnt =
        (uint8_t *)Allocate(owner, world->dpvsDyn.dynEntClientCount[0], sizeof(uint8_t));
    world->reflectionProbeTextures = (GfxTexture *)Allocate(owner, world->reflectionProbeCount, sizeof(GfxTexture));
    world->lightmapPrimaryTextures = (GfxTexture *)Allocate(owner, world->lightmapCount, sizeof(GfxTexture));
    world->lightmapSecondaryTextures = (GfxTexture *)Allocate(owner, world->lightmapCount, sizeof(GfxTexture));
    return !owner->failed && DB64_ValidateWorldRuntimeCounts(world, error, errorSize);
}

bool Linker_AssembleWorldGeometry(const LinkerWorldSource *source, const char *name, LinkerAssembledWorld **result,
                                  char *error, size_t errorSize)
{
    if (!result || (!error && errorSize))
    {
        return false;
    }
    *result = NULL;
    bool valid = source && name && *name && source->metadata && source->collision && source->geometry &&
                 source->groups && source->surfaceOrder && source->visibility && source->cells && source->modelTrees &&
                 source->lightmaps && source->probes && source->lightGrid && source->regions && source->sunParse &&
                 source->sunLight && source->groups->modelCount && source->visibility->models &&
                 source->cells->cellCount == source->modelTrees->cellCount;
    LinkerAssembledWorld *owner = valid ? (LinkerAssembledWorld *)calloc(1, sizeof(LinkerAssembledWorld)) : NULL;
    valid = valid && owner;
    if (valid)
    {
        GfxWorld *world = &owner->world;
        char *ownedName = (char *)Allocate(owner, strlen(name) + 1, sizeof(char));
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;
        const char *extension = strrchr(base, '.');
        const size_t length = extension ? (size_t)(extension - base) : strlen(base);
        char *baseName = (char *)Allocate(owner, length + 1, sizeof(char));
        if (!owner->failed)
        {
            strcpy(ownedName, name);
            memcpy(baseName, base, length);
        }
        world->name = ownedName;
        world->baseName = baseName;
        world->planeCount = source->cells->planeCount;
        world->nodeCount = source->nodeWordCount;
        world->indexCount = source->geometry->indexCount;
        world->indices = source->geometry->indices;
        world->surfaceCount = source->surfaceOrder->surfaceCount;
        world->vertexCount = source->geometry->vertexCount;
        world->vd.vertices = source->geometry->vertices;
        world->vertexLayerDataSize = source->geometry->layerDataSize;
        world->vld.data = source->geometry->layerData;
        world->sunParse = *source->sunParse;
        world->sunLight = source->sunLight;
        if (source->sunEffects)
        {
            world->sun = source->sunEffects->sun;
        }
        memcpy(world->sunColorFromBsp, source->sunLight->color, sizeof(float[3]));
        world->sunPrimaryLightIndex = source->sunPrimaryLightIndex;
        world->primaryLightCount = source->metadata->common.primaryLightCount;
        world->cullGroupCount = source->groups->cullGroupCount;
        world->reflectionProbeCount = source->probes->count;
        world->reflectionProbes = source->probes->probes;
        world->dpvsPlanes.cellCount = source->cells->cellCount;
        world->dpvsPlanes.planes = source->cells->planes;
        world->dpvsPlanes.nodes = source->nodes;
        world->lightmapCount = source->lightmaps->count;
        world->lightmaps = source->lightmaps->lightmaps;
        world->lightGrid = *source->lightGrid;
        world->modelCount = source->groups->modelCount;
        world->models = source->visibility->models;
        world->checksum = source->collision->checksum;
        world->shadowGeom = source->surfaceShadows;
        world->lightRegion = source->regions->regions;
        world->dpvs.smodelCount = source->staticModelPlacementCount;
        world->dpvs.staticSurfaceCount = source->surfaceOrder->staticSurfaceCount;
        world->dpvs.staticSurfaceCountNoDecal = source->visibility->staticSurfaceCountNoDecal;
        world->dpvs.litSurfsBegin = source->surfaceOrder->litSurfsBegin;
        world->dpvs.litSurfsEnd = source->surfaceOrder->litSurfsEnd;
        world->dpvs.decalSurfsBegin = source->surfaceOrder->decalSurfsBegin;
        world->dpvs.decalSurfsEnd = source->surfaceOrder->decalSurfsEnd;
        world->dpvs.emissiveSurfsBegin = source->surfaceOrder->emissiveSurfsBegin;
        world->dpvs.emissiveSurfsEnd = source->surfaceOrder->emissiveSurfsEnd;
        world->dpvs.sortedSurfIndex = source->visibility->sortedSurfIndex;
        world->dpvs.smodelInsts = source->modelInstances;
        world->dpvs.surfaces = source->surfaceOrder->surfaces;
        world->dpvs.cullGroups = source->groups->cullGroups;
        world->dpvs.smodelDrawInsts = source->modelDrawInstances;
        for (unsigned int i = 0; i < 2; ++i)
        {
            world->dpvsDyn.dynEntClientCount[i] = source->collision->dynEntCount[i];
        }
        valid = !owner->failed && SetBoundsAndSky(owner) && SetOutdoorTransform(owner, source) &&
                CopyCells(owner, source) && AllocateRuntime(owner, error, errorSize) &&
                GenerateOutdoorImage(owner, source, error, errorSize);
    }
    if (!valid)
    {
        Linker_FreeAssembledWorld(owner);
        if (errorSize)
        {
            snprintf(error, errorSize, "Cannot assemble native render world geometry or runtime arrays");
        }
        return false;
    }
    *result = owner;
    return true;
}

#include "db_external_assets.h"
#include <universal/q_shared.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_buffers.h>
#include "database.h"
#include "db_render_world.h"
#include "db_render_world_layout.h"
#include "db_render_cells.h"
#include "db_render_light_grid.h"
#include "db_render_shadows.h"
#include "db_render_visibility.h"
#include "db_image_assets.h"
#include "db_material_assets.h"
#include <limits.h>

static void *Array(const void *token, size_t count, size_t stride, int alignment)
{
    if (!!token != (count != 0) || count > INT_MAX / stride)
    {
        Com_Error(ERR_DROP, "Invalid native render-world array");
    }
    if (!count)
    {
        return NULL;
    }
    void *result = DB_AllocStreamPos(alignment);
    Load_Stream(true, (uint8_t *)result, count * stride);
    return result;
}

static void *Runtime(const void *token, size_t count, size_t stride, int alignment)
{
    DB_PushStreamPos(1);
    void *result = Array(token, count, stride, alignment);
    DB_PopStreamPos();
    return result;
}

void DB64_LoadRenderWorld(GfxWorld *world, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)world, sizeof(GfxWorld));
    DB64WorldRuntimeLayout layout;
    char error[256];
    if (!DB64_ValidateWorldRuntimeCounts(world, error, sizeof(error)) ||
        !DB64_GetWorldRuntimeLayout(world, &layout, error, sizeof(error)))
    {
        Com_Error(ERR_DROP, "%s", error);
    }
    if (!world->name || !world->baseName || world->reflectionProbeCount > 256 || world->lightmapCount < 0 ||
        world->lightmapCount > 31 || world->vd.worldVb || world->vld.layerVb)
    {
        Com_Error(ERR_DROP, "Invalid native render-world header");
    }
    DB_PushStreamPos(4);
    DB64_LoadAssetString(&world->name);
    DB64_LoadAssetString(&world->baseName);
    world->indices = (uint16_t *)Array(world->indices, world->indexCount, sizeof(uint16_t), 1);
    world->skyStartSurfs = (int *)Array(world->skyStartSurfs, world->skySurfCount, sizeof(int), 15);
    for (int i = 0; i < world->skySurfCount; ++i)
    {
        if (world->skyStartSurfs[i] < 0 || world->skyStartSurfs[i] >= world->surfaceCount)
        {
            Com_Error(ERR_DROP, "Invalid native sky surface reference");
        }
    }
    DB64_LoadImageAsset((XAssetHeader *)&world->skyImage, false);
    if (world->sunLight == (GfxLight *)UINTPTR_MAX)
    {
        world->sunLight = (GfxLight *)Array(world->sunLight, 1, sizeof(GfxLight), 15);
        DB64_LoadLightDefAsset((XAssetHeader *)&world->sunLight->def, false);
    }
    else if (world->sunLight)
    {
        DB64_ConvertOffsetRange((uintptr_t *)&world->sunLight, sizeof(GfxLight));
    }
    world->reflectionProbes = (GfxReflectionProbe *)Array(world->reflectionProbes, world->reflectionProbeCount,
                                                          sizeof(GfxReflectionProbe), 15);
    for (unsigned int i = 0; i < world->reflectionProbeCount; ++i)
    {
        DB64_LoadImageAsset((XAssetHeader *)&world->reflectionProbes[i].reflectionImage, false);
    }
    world->reflectionProbeTextures =
        (GfxTexture *)Runtime(world->reflectionProbeTextures, world->reflectionProbeCount, sizeof(GfxTexture), 15);
    if (world->dpvsPlanes.planes == (cplane_s *)UINTPTR_MAX)
    {
        world->dpvsPlanes.planes = (cplane_s *)Array(world->dpvsPlanes.planes, world->planeCount, sizeof(cplane_s), 15);
    }
    else if (world->dpvsPlanes.planes && world->planeCount > 0 && world->planeCount <= INT_MAX / sizeof(cplane_s))
    {
        DB64_ConvertOffsetRange((uintptr_t *)&world->dpvsPlanes.planes, world->planeCount * sizeof(cplane_s));
    }
    else if (world->dpvsPlanes.planes || world->planeCount)
    {
        Com_Error(ERR_DROP, "Invalid native world plane array");
    }
    world->dpvsPlanes.nodes = (uint16_t *)Array(world->dpvsPlanes.nodes, world->nodeCount, sizeof(uint16_t), 1);
    world->dpvsPlanes.sceneEntCellBits =
        (uint *)Runtime(world->dpvsPlanes.sceneEntCellBits, layout.sceneEntCellWords, sizeof(uint), 15);
    DB64_LoadRenderCells(world);
    world->lightmaps = (GfxLightmapArray *)Array(world->lightmaps, world->lightmapCount, sizeof(GfxLightmapArray), 15);
    for (int i = 0; i < world->lightmapCount; ++i)
    {
        DB64_LoadImageAsset((XAssetHeader *)&world->lightmaps[i].primary, false);
        DB64_LoadImageAsset((XAssetHeader *)&world->lightmaps[i].secondary, false);
    }
    DB64_LoadRenderLightGrid(&world->lightGrid, world->primaryLightCount, false);
    world->lightmapPrimaryTextures =
        (GfxTexture *)Runtime(world->lightmapPrimaryTextures, world->lightmapCount, sizeof(GfxTexture), 15);
    world->lightmapSecondaryTextures =
        (GfxTexture *)Runtime(world->lightmapSecondaryTextures, world->lightmapCount, sizeof(GfxTexture), 15);
    world->models = (GfxBrushModel *)Array(world->models, world->modelCount, sizeof(GfxBrushModel), 15);
    world->materialMemory =
        (MaterialMemory *)Array(world->materialMemory, world->materialMemoryCount, sizeof(MaterialMemory), 15);
    for (int i = 0; i < world->materialMemoryCount; ++i)
    {
        DB64_LoadMaterialAsset((XAssetHeader *)&world->materialMemory[i].material, false);
    }
    world->vd.vertices = (GfxWorldVertex *)Array(world->vd.vertices, world->vertexCount, sizeof(GfxWorldVertex), 15);
    world->vld.data = (uint8_t *)Array(world->vld.data, world->vertexLayerDataSize, sizeof(uint8_t), 0);
    DB64_LoadMaterialAsset((XAssetHeader *)&world->sun.spriteMaterial, false);
    DB64_LoadMaterialAsset((XAssetHeader *)&world->sun.flareMaterial, false);
    DB64_LoadImageAsset((XAssetHeader *)&world->outdoorImage, false);
    world->cellCasterBits = (uint *)Runtime(world->cellCasterBits, layout.cellCasterWords, sizeof(uint), 15);
    world->sceneDynModel = (GfxSceneDynModel *)Runtime(world->sceneDynModel, world->dpvsDyn.dynEntClientCount[0],
                                                       sizeof(GfxSceneDynModel), 15);
    world->sceneDynBrush = (GfxSceneDynBrush *)Runtime(world->sceneDynBrush, world->dpvsDyn.dynEntClientCount[1],
                                                       sizeof(GfxSceneDynBrush), 15);
    world->primaryLightEntityShadowVis =
        (uint *)Runtime(world->primaryLightEntityShadowVis, layout.primaryLightEntityShadowWords, sizeof(uint), 15);
    for (unsigned int type = 0; type < 2; ++type)
    {
        world->primaryLightDynEntShadowVis[type] = (uint *)Runtime(
            world->primaryLightDynEntShadowVis[type], layout.primaryLightDynEntShadowWords[type], sizeof(uint), 15);
    }
    world->nonSunPrimaryLightForModelDynEnt = (uint8_t *)Runtime(
        world->nonSunPrimaryLightForModelDynEnt, world->dpvsDyn.dynEntClientCount[0], sizeof(uint8_t), 0);
    DB64_LoadRenderShadows(world);
    DB64_LoadRenderStaticVisibility(world, false);
    DB64_LoadRenderDynamicVisibility(world, false);
    DB_PopStreamPos();
    Load_VertexBuffer(&world->vd.worldVb, (uint8_t *)world->vd.vertices, world->vertexCount * sizeof(GfxWorldVertex));
    Load_VertexBuffer(&world->vld.layerVb, world->vld.data, world->vertexLayerDataSize);
}

void DB64_LoadRenderWorldAsset(XAssetHeader *header, bool atStreamStart)
{
    Load_Stream(atStreamStart, (uint8_t *)header, sizeof(XAssetHeader));
    if (DB64_LoadExternalAsset(ASSET_TYPE_GFXWORLD, header))
    {
        return;
    }
    DB_PushStreamPos(0);
    const uintptr_t token = (uintptr_t)header->data;
    if (token == UINTPTR_MAX || token == UINTPTR_MAX - 1)
    {
        header->data = DB_AllocStreamPos(15);
        const void **inserted = token == UINTPTR_MAX - 1 ? DB_InsertPointer() : NULL;
        DB64_LoadRenderWorld(header->gfxWorld, true);
        Load_GfxWorldAsset(header);
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

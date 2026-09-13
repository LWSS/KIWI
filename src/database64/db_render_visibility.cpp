#include <universal/q_shared.h>
#include <gfx_d3d/r_bsp.h>
#include "database.h"
#include "db_render_visibility.h"
#include "db_render_world_layout.h"
#include "db_material_assets.h"
#include "db_model_assets.h"
#include <limits.h>

static void *Array(const void *token, size_t count, size_t stride, int alignment)
{
    if (!!token != (count != 0) || count > INT_MAX / stride)
    {
        Com_Error(ERR_DROP, "Invalid native render visibility array");
    }
    if (!count)
    {
        return NULL;
    }
    void *result = DB_AllocStreamPos(alignment);
    Load_Stream(true, (uint8_t *)result, count * stride);
    return result;
}

static DB64WorldRuntimeLayout Layout(const GfxWorld *world)
{
    DB64WorldRuntimeLayout layout;
    char error[256];
    if (!DB64_ValidateWorldRuntimeCounts(world, error, sizeof(error)) ||
        !DB64_GetWorldRuntimeLayout(world, &layout, error, sizeof(error)))
    {
        Com_Error(ERR_DROP, "%s", error);
    }
    return layout;
}

void DB64_LoadRenderDynamicVisibility(GfxWorld *world, bool atStreamStart)
{
    GfxWorldDpvsDynamic *dpvs = &world->dpvsDyn;
    Load_Stream(atStreamStart, (uint8_t *)dpvs, sizeof(GfxWorldDpvsDynamic));
    const DB64WorldRuntimeLayout layout = Layout(world);
    DB_PushStreamPos(1);
    for (unsigned int type = 0; type < 2; ++type)
    {
        dpvs->dynEntCellBits[type] =
            (uint *)Array(dpvs->dynEntCellBits[type], layout.dynEntCellWords[type], sizeof(uint), 15);
    }
    for (unsigned int view = 0; view < 3; ++view)
    {
        for (unsigned int type = 0; type < 2; ++type)
        {
            dpvs->dynEntVisData[type][view] =
                (uint8_t *)Array(dpvs->dynEntVisData[type][view], layout.dynEntVisBytes[type], sizeof(uint8_t), 15);
        }
    }
    DB_PopStreamPos();
}

void DB64_LoadRenderStaticVisibility(GfxWorld *world, bool atStreamStart)
{
    GfxWorldDpvsStatic *dpvs = &world->dpvs;
    Load_Stream(atStreamStart, (uint8_t *)dpvs, sizeof(GfxWorldDpvsStatic));
    const DB64WorldRuntimeLayout layout = Layout(world);
    if (world->cullGroupCount < 0 || dpvs->litSurfsBegin > dpvs->litSurfsEnd ||
        dpvs->litSurfsEnd > dpvs->staticSurfaceCount || dpvs->decalSurfsBegin > dpvs->decalSurfsEnd ||
        dpvs->decalSurfsEnd > dpvs->staticSurfaceCount || dpvs->emissiveSurfsBegin > dpvs->emissiveSurfsEnd ||
        dpvs->emissiveSurfsEnd > dpvs->staticSurfaceCount)
    {
        Com_Error(ERR_DROP, "Invalid native render surface ranges");
    }
    DB_PushStreamPos(1);
    for (unsigned int view = 0; view < 3; ++view)
    {
        dpvs->smodelVisData[view] = (uint8_t *)Array(dpvs->smodelVisData[view], dpvs->smodelCount, sizeof(uint8_t), 0);
    }
    for (unsigned int view = 0; view < 3; ++view)
    {
        dpvs->surfaceVisData[view] =
            (uint8_t *)Array(dpvs->surfaceVisData[view], dpvs->staticSurfaceCount, sizeof(uint8_t), 0);
    }
    dpvs->lodData = (uint *)Array(dpvs->lodData, 2 * layout.smodelVisWords, sizeof(uint), 127);
    DB_PopStreamPos();
    const unsigned int sortedCount = dpvs->staticSurfaceCount + dpvs->staticSurfaceCountNoDecal;
    dpvs->sortedSurfIndex = (uint16_t *)Array(dpvs->sortedSurfIndex, sortedCount, sizeof(uint16_t), 1);
    for (unsigned int i = 0; i < sortedCount; ++i)
    {
        if (dpvs->sortedSurfIndex[i] >= dpvs->staticSurfaceCount)
        {
            Com_Error(ERR_DROP, "Invalid native sorted surface reference");
        }
    }
    dpvs->smodelInsts =
        (GfxStaticModelInst *)Array(dpvs->smodelInsts, dpvs->smodelCount, sizeof(GfxStaticModelInst), 15);
    dpvs->surfaces = (GfxSurface *)Array(dpvs->surfaces, world->surfaceCount, sizeof(GfxSurface), 15);
    for (int i = 0; i < world->surfaceCount; ++i)
    {
        if (!dpvs->surfaces[i].material)
        {
            Com_Error(ERR_DROP, "Missing native world surface material");
        }
        DB64_LoadMaterialAsset((XAssetHeader *)&dpvs->surfaces[i].material, false);
    }
    dpvs->cullGroups = (GfxCullGroup *)Array(dpvs->cullGroups, world->cullGroupCount, sizeof(GfxCullGroup), 15);
    for (int i = 0; i < world->cullGroupCount; ++i)
    {
        const GfxCullGroup *group = &dpvs->cullGroups[i];
        if (!group->surfaceCount && group->startSurfIndex == -1)
        {
            continue;
        }
        if (group->startSurfIndex < 0 || group->surfaceCount < 0 || (unsigned int)group->startSurfIndex > sortedCount ||
            (unsigned int)group->surfaceCount > sortedCount - group->startSurfIndex)
        {
            Com_Error(ERR_DROP, "Invalid native cull-group surface span");
        }
    }
    dpvs->smodelDrawInsts =
        (GfxStaticModelDrawInst *)Array(dpvs->smodelDrawInsts, dpvs->smodelCount, sizeof(GfxStaticModelDrawInst), 15);
    for (unsigned int i = 0; i < dpvs->smodelCount; ++i)
    {
        if (!dpvs->smodelDrawInsts[i].model)
        {
            Com_Error(ERR_DROP, "Missing native static model asset");
        }
        DB64_LoadModelAsset((XAssetHeader *)&dpvs->smodelDrawInsts[i].model, false);
    }
    DB_PushStreamPos(1);
    dpvs->surfaceMaterials =
        (GfxDrawSurf *)Array(dpvs->surfaceMaterials, dpvs->staticSurfaceCount, sizeof(GfxDrawSurf), 15);
    dpvs->surfaceCastsSunShadow = (uint *)Array(dpvs->surfaceCastsSunShadow, layout.surfaceVisWords, sizeof(uint), 127);
    DB_PopStreamPos();
    dpvs->usageCount = 0;
}

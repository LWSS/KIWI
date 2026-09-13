#include <universal/q_shared.h>
#include <gfx_d3d/r_bsp.h>
#include "database.h"
#include "db_render_shadows.h"
#include <limits.h>
#include <math.h>

static void *ReadArray(const void *token, size_t count, size_t stride, int alignment)
{
    if (!!token != (count != 0) || count > INT_MAX / stride)
    {
        Com_Error(ERR_DROP, "Invalid native shadow array size");
    }
    if (!count)
    {
        return NULL;
    }
    void *result = DB_AllocStreamPos(alignment);
    Load_Stream(true, (uint8_t *)result, count * stride);
    return result;
}

void DB64_LoadRenderShadows(GfxWorld *world)
{
    if (!world->primaryLightCount || world->primaryLightCount > 255 || world->surfaceCount < 0 ||
        world->surfaceCount > 65536 || world->dpvs.smodelCount > 65536)
    {
        Com_Error(ERR_DROP, "Invalid native shadow world dimensions");
    }
    if (world->shadowGeom)
    {
        world->shadowGeom =
            (GfxShadowGeometry *)ReadArray(world->shadowGeom, world->primaryLightCount, sizeof(GfxShadowGeometry), 15);
        for (unsigned int light = 0; light < world->primaryLightCount; ++light)
        {
            GfxShadowGeometry *shadow = &world->shadowGeom[light];
            shadow->sortedSurfIndex =
                (uint16_t *)ReadArray(shadow->sortedSurfIndex, shadow->surfaceCount, sizeof(uint16_t), 1);
            shadow->smodelIndex = (uint16_t *)ReadArray(shadow->smodelIndex, shadow->smodelCount, sizeof(uint16_t), 1);
            for (unsigned int i = 0; i < shadow->surfaceCount; ++i)
            {
                if (shadow->sortedSurfIndex[i] >= world->dpvs.staticSurfaceCount)
                {
                    Com_Error(ERR_DROP, "Invalid native shadow surface reference");
                }
            }
            for (unsigned int i = 0; i < shadow->smodelCount; ++i)
            {
                if (shadow->smodelIndex[i] >= world->dpvs.smodelCount)
                {
                    Com_Error(ERR_DROP, "Invalid native shadow model reference");
                }
            }
        }
    }
    if (world->lightRegion)
    {
        world->lightRegion =
            (GfxLightRegion *)ReadArray(world->lightRegion, world->primaryLightCount, sizeof(GfxLightRegion), 15);
        for (unsigned int light = 0; light < world->primaryLightCount; ++light)
        {
            GfxLightRegion *region = &world->lightRegion[light];
            region->hulls =
                (GfxLightRegionHull *)ReadArray(region->hulls, region->hullCount, sizeof(GfxLightRegionHull), 15);
            for (unsigned int h = 0; h < region->hullCount; ++h)
            {
                GfxLightRegionHull *hull = &region->hulls[h];
                for (unsigned int k = 0; k < 9; ++k)
                {
                    if (!isfinite(hull->kdopMidPoint[k]) || !isfinite(hull->kdopHalfSize[k]) ||
                        hull->kdopHalfSize[k] < 0)
                    {
                        Com_Error(ERR_DROP, "Invalid native light-region bounds");
                    }
                }
                hull->axis =
                    (GfxLightRegionAxis *)ReadArray(hull->axis, hull->axisCount, sizeof(GfxLightRegionAxis), 15);
                for (unsigned int a = 0; a < hull->axisCount; ++a)
                {
                    const GfxLightRegionAxis *axis = &hull->axis[a];
                    if (!isfinite(axis->midPoint) || !isfinite(axis->halfSize) || axis->halfSize < 0 ||
                        !isfinite(axis->dir[0]) || !isfinite(axis->dir[1]) || !isfinite(axis->dir[2]))
                    {
                        Com_Error(ERR_DROP, "Invalid native light-region axis");
                    }
                }
            }
        }
    }
}

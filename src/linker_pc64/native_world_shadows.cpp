#include <universal/q_shared.h>
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_primarylights.h>
#include <xanim/xmodel.h>
#include <xanim/xanim.h>
#include "native_bsp.h"
#include "native_world_sort.h"
#include "native_world_shadows.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static bool ValidLightRegion(const ComPrimaryLight *light, const GfxLightRegion *region,
                             const LinkerBspLightRegions *regions)
{
    if (!isfinite(light->radius) || light->radius < 0 || !isfinite(light->cosHalfFovExpanded) ||
        light->cosHalfFovExpanded < -1 || light->cosHalfFovExpanded > 1 || region->hullCount > regions->hullCount ||
        (region->hullCount && !region->hulls))
    {
        return false;
    }
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (!isfinite(light->origin[i]) || !isfinite(light->dir[i]))
        {
            return false;
        }
    }
    for (unsigned int h = 0; h < region->hullCount; ++h)
    {
        const GfxLightRegionHull *hull = &region->hulls[h];
        if (hull->axisCount > regions->axisCount || hull->axisCount > UINT_MAX - 9 || (hull->axisCount && !hull->axis))
        {
            return false;
        }
        for (unsigned int i = 0; i < 9; ++i)
        {
            if (!isfinite(hull->kdopMidPoint[i]) || !isfinite(hull->kdopHalfSize[i]) || hull->kdopHalfSize[i] < 0)
            {
                return false;
            }
        }
        for (unsigned int i = 0; i < hull->axisCount; ++i)
        {
            const GfxLightRegionAxis *axis = &hull->axis[i];
            if (!isfinite(axis->midPoint) || !isfinite(axis->halfSize) || axis->halfSize < 0)
            {
                return false;
            }
            for (unsigned int j = 0; j < 3; ++j)
            {
                if (!isfinite(axis->dir[j]))
                {
                    return false;
                }
            }
        }
    }
    return true;
}

static bool OutsideLight(const ComPrimaryLight *light, const double *mid, const double *half)
{
    double delta[3], distanceSquared = 0;
    for (unsigned int i = 0; i < 3; ++i)
    {
        delta[i] = mid[i] - light->origin[i];
        double distance = fabs(delta[i]) - half[i];
        if (distance > 0)
        {
            distanceSquared += distance * distance;
        }
    }
    if (distanceSquared > (double)light->radius * light->radius)
    {
        return true;
    }
    if (light->type != GFX_LIGHT_TYPE_SPOT || light->cosHalfFovExpanded < 0)
    {
        return false;
    }
    double corner[3], perpendicular[3], distance = 0, perpendicularSquared = 0;
    for (unsigned int i = 0; i < 3; ++i)
    {
        corner[i] = delta[i] - half[i] * (light->dir[i] < 0 ? -1 : 1);
        distance += corner[i] * light->dir[i];
    }
    if (distance >= 0)
    {
        return true;
    }
    for (unsigned int i = 0; i < 3; ++i)
    {
        perpendicular[i] = corner[i] - distance * light->dir[i];
        perpendicularSquared += perpendicular[i] * perpendicular[i];
    }
    const double cosine = light->cosHalfFovExpanded;
    const double sineSquared = 1 - cosine * cosine;
    if (distance * distance * sineSquared >= perpendicularSquared * cosine * cosine)
    {
        return false;
    }
    if (sineSquared == 0)
    {
        // A zero-width cone is a ray in the negative light direction, clipped to its radius.
        double nearDistance = 0, farDistance = light->radius;
        for (unsigned int i = 0; i < 3; ++i)
        {
            double direction = -light->dir[i];
            if (direction == 0)
            {
                if (fabs(delta[i]) > half[i])
                {
                    return true;
                }
                continue;
            }
            double a = (delta[i] - half[i]) / direction, b = (delta[i] + half[i]) / direction;
            if (a > b)
            {
                double swap = a;
                a = b;
                b = swap;
            }
            if (a > nearDistance)
            {
                nearDistance = a;
            }
            if (b < farDistance)
            {
                farDistance = b;
            }
        }
        return nearDistance > farDistance;
    }
    const double scale = cosine / sqrt(perpendicularSquared * sineSquared);
    double separation = 0;
    for (unsigned int i = 0; i < 3; ++i)
    {
        const double axis = light->dir[i] + scale * perpendicular[i];
        separation += axis * delta[i] - fabs(axis) * half[i];
    }
    return separation >= 0;
}

static bool OutsideHull(const GfxLightRegionHull *hull, const double *mid, const double *half)
{
    static const int directions[9][3] = {{1, 0, 0}, {0, 1, 0},  {0, 0, 1}, {1, 1, 0}, {1, -1, 0},
                                         {1, 0, 1}, {1, 0, -1}, {0, 1, 1}, {0, 1, -1}};
    for (unsigned int i = 0; i < 9 + hull->axisCount; ++i)
    {
        double projection = 0, radius = 0;
        for (unsigned int j = 0; j < 3; ++j)
        {
            const double direction = i < 9 ? directions[i][j] : hull->axis[i - 9].dir[j];
            projection += mid[j] * direction;
            radius += half[j] * fabs(direction);
        }
        const double center = i < 9 ? hull->kdopMidPoint[i] : hull->axis[i - 9].midPoint;
        const double extent = i < 9 ? hull->kdopHalfSize[i] : hull->axis[i - 9].halfSize;
        if (fabs(projection - center) >= radius + extent)
        {
            return true;
        }
    }
    return false;
}

static bool Affected(const GfxSurface *surface, unsigned int index, const ComPrimaryLight *light,
                     const GfxLightRegion *region)
{
    if (surface->material->info.gameFlags & 2)
    {
        return surface->primaryLightIndex == index;
    }
    double mid[3], half[3], relative[3];
    for (unsigned int i = 0; i < 3; ++i)
    {
        mid[i] = ((double)surface->bounds[0][i] + surface->bounds[1][i]) * 0.5;
        half[i] = mid[i] - surface->bounds[0][i] + 1;
        relative[i] = mid[i] - light->origin[i];
    }
    if (OutsideLight(light, mid, half))
    {
        return false;
    }
    for (unsigned int i = 0; i < region->hullCount; ++i)
    {
        if (!OutsideHull(&region->hulls[i], relative, half))
        {
            return true;
        }
    }
    return !region->hullCount;
}

void Linker_FreeSurfaceShadows(GfxShadowGeometry *shadows, unsigned int lightCount)
{
    if (shadows)
    {
        for (unsigned int i = 0; i < lightCount; ++i)
        {
            free(shadows[i].sortedSurfIndex);
            free(shadows[i].smodelIndex);
        }
        free(shadows);
    }
}

bool Linker_BuildSurfaceShadows(const ComWorld *common, unsigned int sunIndex, const LinkerBspLightRegions *regions,
                                const LinkerWorldSurfaceOrder *order, GfxShadowGeometry **shadows, char *error,
                                size_t errorSize)
{
    if (!shadows || (!error && errorSize))
    {
        return false;
    }
    *shadows = NULL;
    bool valid = common && common->primaryLights && common->primaryLightCount <= 255 && sunIndex <= 1 &&
                 common->primaryLightCount > sunIndex && regions && regions->regions &&
                 regions->regionCount == common->primaryLightCount && order && order->staticSurfaceCount <= 65535 &&
                 (!order->staticSurfaceCount || order->surfaces);
    GfxShadowGeometry *result =
        valid ? (GfxShadowGeometry *)calloc(common->primaryLightCount, sizeof(GfxShadowGeometry)) : NULL;
    valid = valid && result;
    for (unsigned int i = 0; valid && i < order->staticSurfaceCount; ++i)
    {
        const GfxSurface *surface = &order->surfaces[i];
        valid = surface->material && surface->primaryLightIndex < common->primaryLightCount;
        for (unsigned int j = 0; valid && j < 3; ++j)
        {
            valid = isfinite(surface->bounds[0][j]) && isfinite(surface->bounds[1][j]) &&
                    surface->bounds[0][j] <= surface->bounds[1][j];
        }
    }
    for (unsigned int lightIndex = sunIndex + 1; valid && lightIndex < common->primaryLightCount; ++lightIndex)
    {
        const ComPrimaryLight *light = &common->primaryLights[lightIndex];
        const GfxLightRegion *region = &regions->regions[lightIndex];
        valid = ValidLightRegion(light, region, regions);
        for (unsigned int pass = 0; valid && pass < 2; ++pass)
        {
            unsigned int count = 0;
            for (unsigned int i = 0; i < order->staticSurfaceCount; ++i)
            {
                const GfxSurface *surface = &order->surfaces[i];
                if ((surface->material->info.gameFlags & 0x40) && Affected(surface, lightIndex, light, region))
                {
                    if (pass)
                    {
                        result[lightIndex].sortedSurfIndex[count] = (uint16_t)i;
                    }
                    ++count;
                }
            }
            if (!pass)
            {
                result[lightIndex].surfaceCount = (uint16_t)count;
                result[lightIndex].sortedSurfIndex = count ? (uint16_t *)malloc(count * sizeof(uint16_t)) : NULL;
                valid = !count || result[lightIndex].sortedSurfIndex;
            }
        }
    }
    if (!valid)
    {
        Linker_FreeSurfaceShadows(result, common ? common->primaryLightCount : 0);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid surface shadow geometry input or allocation");
        }
        return false;
    }
    *shadows = result;
    return true;
}

static bool PointInLight(const double *point, const ComPrimaryLight *light, const GfxLightRegion *region)
{
    double relative[3], lengthSquared = 0, dot = 0;
    for (unsigned int i = 0; i < 3; ++i)
    {
        relative[i] = point[i] - light->origin[i];
        lengthSquared += relative[i] * relative[i];
        dot += relative[i] * light->dir[i];
    }
    if (lengthSquared > (double)light->radius * light->radius)
    {
        return false;
    }
    if (light->type == GFX_LIGHT_TYPE_SPOT)
    {
        const double cosine = light->cosHalfFovExpanded;
        if ((cosine >= 0 && (dot > 0 || cosine * cosine * lengthSquared > dot * dot)) ||
            (cosine < 0 && dot > 0 && cosine * cosine * lengthSquared < dot * dot))
        {
            return false;
        }
    }
    const double zero[3] = {};
    for (unsigned int i = 0; i < region->hullCount; ++i)
    {
        if (!OutsideHull(&region->hulls[i], relative, zero))
        {
            return true;
        }
    }
    return !region->hullCount;
}

static bool SelectModelLocalLight(const ComWorld *common, const LinkerBspLightRegions *regions,
                                  const GfxStaticModelInst *instance, const GfxStaticModelDrawInst *draw,
                                  uint8_t *selected)
{
    const XModel *model = draw->model;
    if (!model || model->bad || model->numLods < 1 || model->numLods > 4 || !model->surfs ||
        !isfinite(draw->placement.scale) || draw->placement.scale == 0)
    {
        return false;
    }
    double mid[3], half[3];
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (!isfinite(instance->mins[i]) || !isfinite(instance->maxs[i]) || instance->mins[i] > instance->maxs[i] ||
            !isfinite(draw->placement.origin[i]))
        {
            return false;
        }
        for (unsigned int j = 0; j < 3; ++j)
        {
            if (!isfinite(draw->placement.axis[i][j]))
            {
                return false;
            }
        }
        mid[i] = ((double)instance->mins[i] + instance->maxs[i]) * 0.5;
        half[i] = mid[i] - instance->mins[i];
    }
    if ((model->flags & 1) && instance->groundLighting.packed)
    {
        *selected = draw->primaryLightIndex;
        return *selected < common->primaryLightCount;
    }
    bool candidates[256] = {};
    uint64_t votes[256] = {}, mostVotes = 0;
    *selected = 0;
    for (unsigned int i = 1; i < common->primaryLightCount; ++i)
    {
        const ComPrimaryLight *light = &common->primaryLights[i];
        candidates[i] = (light->type == GFX_LIGHT_TYPE_OMNI || light->type == GFX_LIGHT_TYPE_SPOT) &&
                        !OutsideLight(light, mid, half);
    }
    const XModelLodInfo *lod = &model->lodInfo[model->numLods - 1];
    if (!lod->numsurfs || lod->surfIndex > model->numsurfs || lod->numsurfs > model->numsurfs - lod->surfIndex)
    {
        return false;
    }
    for (unsigned int s = 0; s < lod->numsurfs; ++s)
    {
        const XSurface *surface = &model->surfs[lod->surfIndex + s];
        if (!surface->vertCount || !surface->verts0)
        {
            return false;
        }
        for (unsigned int v = 0; v < surface->vertCount; ++v)
        {
            const float *vertex = surface->verts0[v].xyz;
            double point[3];
            for (unsigned int i = 0; i < 3; ++i)
            {
                point[i] = draw->placement.origin[i] +
                           draw->placement.scale * ((double)vertex[0] * draw->placement.axis[0][i] +
                                                    (double)vertex[1] * draw->placement.axis[1][i] +
                                                    (double)vertex[2] * draw->placement.axis[2][i]);
                if (!isfinite(point[i]))
                {
                    return false;
                }
            }
            for (unsigned int i = 1; i < common->primaryLightCount; ++i)
            {
                if (candidates[i] && PointInLight(point, &common->primaryLights[i], &regions->regions[i]))
                {
                    if (++votes[i] > mostVotes)
                    {
                        mostVotes = votes[i];
                        *selected = (uint8_t)i;
                    }
                    // Overlapping lights use the first matching light for this vertex.
                    break;
                }
            }
        }
    }
    return true;
}

bool Linker_AssignModelLocalLights(const ComWorld *common, const LinkerBspLightRegions *regions,
                                   const GfxStaticModelInst *instances, unsigned int modelCount,
                                   GfxStaticModelDrawInst *draw, char *error, size_t errorSize)
{
    if (!error && errorSize)
    {
        return false;
    }
    bool valid = common && common->primaryLights && common->primaryLightCount && common->primaryLightCount <= 255 &&
                 regions && regions->regions && regions->regionCount == common->primaryLightCount &&
                 modelCount <= 65535 && (!modelCount || (instances && draw));
    for (unsigned int i = 0; valid && i < common->primaryLightCount; ++i)
    {
        const ComPrimaryLight *light = &common->primaryLights[i];
        valid = light->type <= GFX_LIGHT_TYPE_OMNI && ValidLightRegion(light, &regions->regions[i], regions);
    }
    uint8_t *selected = valid && modelCount ? (uint8_t *)malloc(modelCount * sizeof(uint8_t)) : NULL;
    valid = valid && (!modelCount || selected);
    for (unsigned int i = 0; valid && i < modelCount; ++i)
    {
        valid = SelectModelLocalLight(common, regions, &instances[i], &draw[i], &selected[i]);
    }
    if (valid)
    {
        for (unsigned int i = 0; i < modelCount; ++i)
        {
            draw[i].primaryLightIndex = selected[i];
        }
    }
    else if (errorSize)
    {
        snprintf(error, errorSize, "Invalid static-model primary lighting input or allocation");
    }
    free(selected);
    return valid;
}

bool Linker_BuildModelShadows(GfxShadowGeometry *shadows, unsigned int lightCount, const GfxStaticModelDrawInst *draw,
                              unsigned int modelCount, char *error, size_t errorSize)
{
    if (!error && errorSize)
    {
        return false;
    }
    bool valid = shadows && lightCount && lightCount <= 255 && modelCount <= 65535 && (!modelCount || draw);
    unsigned int counts[256] = {}, cursors[256] = {};
    uint16_t *lists[256] = {};
    for (unsigned int i = 0; valid && i < modelCount; ++i)
    {
        const unsigned int light = draw[i].primaryLightIndex;
        valid = light < lightCount;
        if (valid)
        {
            ++counts[light];
        }
    }
    for (unsigned int i = 0; valid && i < lightCount; ++i)
    {
        lists[i] = counts[i] ? (uint16_t *)malloc(counts[i] * sizeof(uint16_t)) : NULL;
        valid = !counts[i] || lists[i];
    }
    if (valid)
    {
        for (unsigned int i = 0; i < modelCount; ++i)
        {
            const unsigned int light = draw[i].primaryLightIndex;
            lists[light][cursors[light]++] = (uint16_t)i;
        }
        for (unsigned int i = 0; i < lightCount; ++i)
        {
            free(shadows[i].smodelIndex);
            shadows[i].smodelIndex = lists[i];
            shadows[i].smodelCount = (uint16_t)counts[i];
        }
    }
    else
    {
        for (unsigned int i = 0; i < 256; ++i)
        {
            free(lists[i]);
        }
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid static-model shadow list input or allocation");
        }
    }
    return valid;
}

#include <universal/q_shared.h>
#include <gfx_d3d/r_gfx.h>
#include <xanim/xmodel.h>
#include <xanim/xanim.h>
#include "native_bsp.h"
#include "native_world_models.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool Linker_BuildStaticModelCollision(const GfxStaticModelDrawInst *instances, unsigned int count,
                                      cStaticModel_s **collision, char *error, size_t errorSize)
{
    if (!collision)
    {
        return false;
    }
    *collision = NULL;
    cStaticModel_s *result = count && count <= 65535 ? (cStaticModel_s *)calloc(count, sizeof(cStaticModel_s)) : NULL;
    bool valid = count <= 65535 && (!count || (instances && result));
    for (unsigned int m = 0; valid && m < count; ++m)
    {
        const GfxStaticModelDrawInst *instance = &instances[m];
        const XModel *model = instance->model;
        cStaticModel_s *out = &result[m];
        const double scale = instance->placement.scale;
        valid = model && isfinite(scale) && scale != 0 && model->numCollSurfs >= 0 &&
                (!model->numCollSurfs || model->collSurfs);
        if (!valid)
        {
            break;
        }
        out->xmodel = instance->model;
        for (unsigned int i = 0; valid && i < 3; ++i)
        {
            out->origin[i] = instance->placement.origin[i];
            out->absmin[i] = model->numCollSurfs ? FLT_MAX : 0;
            out->absmax[i] = model->numCollSurfs ? -FLT_MAX : 0;
            valid = isfinite(out->origin[i]);
            for (unsigned int j = 0; valid && j < 3; ++j)
            {
                out->invScaledAxis[i][j] = (float)(instance->placement.axis[j][i] / scale);
                valid = isfinite(out->invScaledAxis[i][j]);
            }
        }
        for (int s = 0; valid && s < model->numCollSurfs; ++s)
        {
            const XModelCollSurf_s *surface = &model->collSurfs[s];
            for (unsigned int i = 0; i < 3; ++i)
            {
                valid = valid && isfinite(surface->mins[i]) && isfinite(surface->maxs[i]) &&
                        surface->mins[i] <= surface->maxs[i];
            }
            for (unsigned int corner = 0; valid && corner < 8; ++corner)
            {
                for (unsigned int i = 0; valid && i < 3; ++i)
                {
                    double value = out->origin[i];
                    for (unsigned int j = 0; j < 3; ++j)
                    {
                        value += (corner & (1 << j) ? surface->maxs[j] : surface->mins[j]) * scale *
                                 instance->placement.axis[j][i];
                    }
                    valid = isfinite(value) && value >= -FLT_MAX && value <= FLT_MAX;
                    if (value < out->absmin[i])
                    {
                        out->absmin[i] = (float)value;
                    }
                    if (value > out->absmax[i])
                    {
                        out->absmax[i] = (float)value;
                    }
                }
            }
        }
        valid = valid && (model->numCollSurfs || !model->contents);
    }
    if (!valid)
    {
        free(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid static model collision transform or bounds");
        }
        return false;
    }
    *collision = result;
    return true;
}

static bool BuildInstance(const LinkerBspStaticModel *source, XModel *model, GfxStaticModelDrawInst *draw,
                          GfxStaticModelInst *instance)
{
    if (!model || model->bad || !model->surfs || model->numLods < 1 || model->numLods > 4 || !isfinite(source->scale) ||
        source->scale == 0)
    {
        return false;
    }
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (!isfinite(source->origin[i]) || !isfinite(source->angles[i]))
        {
            return false;
        }
    }
    const XModelLodInfo *lod = &model->lodInfo[0];
    if (!lod->numsurfs || lod->surfIndex > model->numsurfs || lod->numsurfs > model->numsurfs - lod->surfIndex)
    {
        return false;
    }
    const double radians = 0.017453292519943295;
    const double pitch = source->angles[0] * radians, yaw = source->angles[1] * radians;
    const double roll = source->angles[2] * radians;
    const double sp = sin(pitch), cp = cos(pitch), sy = sin(yaw), cy = cos(yaw), sr = sin(roll), cr = cos(roll);
    float(*axis)[3] = draw->placement.axis;
    axis[0][0] = (float)(cp * cy);
    axis[0][1] = (float)(cp * sy);
    axis[0][2] = (float)-sp;
    axis[1][0] = (float)(sr * sp * cy - cr * sy);
    axis[1][1] = (float)(sr * sp * sy + cr * cy);
    axis[1][2] = (float)(sr * cp);
    axis[2][0] = (float)(cr * sp * cy + sr * sy);
    axis[2][1] = (float)(cr * sp * sy - sr * cy);
    axis[2][2] = (float)(cr * cp);
    memcpy(draw->placement.origin, source->origin, sizeof(float[3]));
    draw->placement.scale = source->scale;
    draw->model = model;
    draw->flags = source->flags;
    const double cullDist = (double)model->lodInfo[model->numLods - 1].dist * fabs(source->scale);
    if (!isfinite(cullDist) || cullDist < 0 || cullDist > FLT_MAX)
    {
        return false;
    }
    draw->cullDist = (float)cullDist;
    for (unsigned int i = 0; i < 3; ++i)
    {
        instance->mins[i] = FLT_MAX;
        instance->maxs[i] = -FLT_MAX;
    }
    // Transform actual LOD-zero vertices, not the corners of the model's axis-aligned bounds.
    for (unsigned int s = 0; s < lod->numsurfs; ++s)
    {
        const XSurface *surface = &model->surfs[lod->surfIndex + s];
        if (!surface->vertCount || !surface->verts0)
        {
            return false;
        }
        for (unsigned int v = 0; v < surface->vertCount; ++v)
        {
            const float *position = surface->verts0[v].xyz;
            for (unsigned int i = 0; i < 3; ++i)
            {
                const double transformed = source->origin[i] + source->scale * ((double)position[0] * axis[0][i] +
                                                                                (double)position[1] * axis[1][i] +
                                                                                (double)position[2] * axis[2][i]);
                if (!isfinite(transformed) || transformed < -FLT_MAX || transformed > FLT_MAX)
                {
                    return false;
                }
                const float coordinate = (float)transformed;
                if (coordinate < instance->mins[i])
                {
                    instance->mins[i] = coordinate;
                }
                if (coordinate > instance->maxs[i])
                {
                    instance->maxs[i] = coordinate;
                }
            }
        }
    }
    if ((model->flags & 1) && source->hasGroundLighting)
    {
        memcpy(&instance->groundLighting, source->groundLighting, sizeof(uint8_t[4]));
        if (instance->groundLighting.packed)
        {
            draw->primaryLightIndex = source->primaryLightIndex;
        }
    }
    return true;
}

bool Linker_BuildStaticModelInstances(const LinkerBspStaticModel *placements, XModel *const *models, unsigned int count,
                                      GfxStaticModelDrawInst **drawInstances, GfxStaticModelInst **instances,
                                      char *error, size_t errorSize)
{
    if (!drawInstances || !instances || (!error && errorSize))
    {
        return false;
    }
    *drawInstances = NULL;
    *instances = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    bool valid = count <= 65535 && (!count || (placements && models));
    GfxStaticModelDrawInst *draw =
        valid && count ? (GfxStaticModelDrawInst *)calloc(count, sizeof(GfxStaticModelDrawInst)) : NULL;
    GfxStaticModelInst *inst = valid && count ? (GfxStaticModelInst *)calloc(count, sizeof(GfxStaticModelInst)) : NULL;
    valid = valid && (!count || (draw && inst));
    for (unsigned int i = 0; valid && i < count; ++i)
    {
        valid = BuildInstance(&placements[i], models[i], &draw[i], &inst[i]);
        if (!valid && errorSize)
        {
            snprintf(error, errorSize, "Invalid static model '%s' at entity %u: geometry, transform or LOD distance",
                     placements[i].model, placements[i].entityIndex);
        }
    }
    if (!valid)
    {
        free(draw);
        free(inst);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Invalid static-model instance count or allocation");
        }
        return false;
    }
    *drawInstances = draw;
    *instances = inst;
    return true;
}

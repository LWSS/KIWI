#include <universal/q_shared.h>
#include <gfx_d3d/r_gfx.h>
#include "native_model_cells.h"
#include "native_bsp.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ModelCellFrame
{
    unsigned int node, limit;
    float mins[3], maxs[3];
};

static bool ValidateCellTree(const uint16_t *nodes, unsigned int wordCount, const cplane_s *planes,
                             unsigned int planeCount, unsigned int cellCount)
{
    ModelCellFrame *stack = (ModelCellFrame *)malloc(wordCount * sizeof(ModelCellFrame));
    if (!stack)
    {
        return false;
    }
    unsigned int depth = 1;
    stack[0].node = 0;
    stack[0].limit = wordCount;
    bool valid = true;
    while (valid && depth)
    {
        const ModelCellFrame frame = stack[--depth];
        if (frame.node >= frame.limit || frame.limit > wordCount)
        {
            valid = false;
            break;
        }
        const unsigned int code = nodes[frame.node];
        if (code <= cellCount)
        {
            valid = frame.limit == frame.node + 1;
            continue;
        }
        const unsigned int planeIndex = code - cellCount - 1;
        if (planeIndex >= planeCount || frame.limit - frame.node < 4)
        {
            valid = false;
            break;
        }
        const unsigned int offset = nodes[frame.node + 1];
        const cplane_s *plane = &planes[planeIndex];
        valid = offset >= 3 && offset < frame.limit - frame.node && isfinite(plane->dist);
        double lengthSquared = 0;
        for (unsigned int axis = 0; valid && axis < 3; ++axis)
        {
            valid = isfinite(plane->normal[axis]) &&
                    (plane->type >= 3 || plane->normal[axis] == (axis == plane->type ? 1 : 0));
            lengthSquared += (double)plane->normal[axis] * plane->normal[axis];
        }
        valid = valid && lengthSquared > 0 && depth + 2 <= wordCount;
        if (valid)
        {
            stack[depth].node = frame.node + offset;
            stack[depth++].limit = frame.limit;
            stack[depth].node = frame.node + 2;
            stack[depth++].limit = frame.node + offset;
        }
    }
    free(stack);
    return valid;
}

static bool FilterModel(const uint16_t *nodes, unsigned int wordCount, const cplane_s *planes, unsigned int planeCount,
                        unsigned int cellCount, const GfxStaticModelInst *model, ModelCellFrame *stack, uint32_t *bits)
{
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (!isfinite(model->mins[i]) || !isfinite(model->maxs[i]) || model->mins[i] > model->maxs[i])
        {
            return false;
        }
    }
    unsigned int depth = 1;
    stack[0].node = 0;
    stack[0].limit = wordCount;
    memcpy(stack[0].mins, model->mins, sizeof(float[3]));
    memcpy(stack[0].maxs, model->maxs, sizeof(float[3]));
    while (depth)
    {
        ModelCellFrame frame = stack[--depth];
        if (frame.node >= frame.limit || frame.limit > wordCount)
        {
            return false;
        }
        unsigned int code = nodes[frame.node];
        if (code <= cellCount)
        {
            if (code)
            {
                unsigned int cell = code - 1;
                bits[cell / 32] |= (uint32_t)1 << (cell % 32);
            }
            continue;
        }
        const unsigned int planeIndex = code - cellCount - 1;
        if (planeIndex >= planeCount || frame.limit - frame.node < 4)
        {
            return false;
        }
        const unsigned int offset = nodes[frame.node + 1];
        if (offset < 3 || offset >= frame.limit - frame.node)
        {
            return false;
        }
        const cplane_s *plane = &planes[planeIndex];
        if (!isfinite(plane->dist))
        {
            return false;
        }
        double nearDistance = 0, farDistance = 0;
        for (unsigned int i = 0; i < 3; ++i)
        {
            if (!isfinite(plane->normal[i]))
            {
                return false;
            }
            const double normal = plane->normal[i];
            nearDistance += normal * (normal < 0 ? frame.maxs[i] : frame.mins[i]);
            farDistance += normal * (normal < 0 ? frame.mins[i] : frame.maxs[i]);
        }
        bool front = farDistance >= plane->dist;
        bool back = nearDistance < plane->dist;
        ModelCellFrame frontFrame = frame, backFrame = frame;
        frontFrame.node = frame.node + 2;
        frontFrame.limit = frame.node + offset;
        backFrame.node = frame.node + offset;
        if (front && back && plane->type < 3)
        {
            // Axial BSP planes use a positive unit normal. Reject inconsistent type metadata.
            for (unsigned int i = 0; i < 3; ++i)
            {
                if (plane->normal[i] != (i == plane->type ? 1 : 0))
                {
                    return false;
                }
            }
            front = frame.maxs[plane->type] > plane->dist;
            frontFrame.mins[plane->type] = plane->dist;
            backFrame.maxs[plane->type] = plane->dist;
        }
        if (depth + (unsigned int)front + (unsigned int)back > wordCount)
        {
            return false;
        }
        if (back)
        {
            stack[depth++] = backFrame;
        }
        if (front)
        {
            stack[depth++] = frontFrame;
        }
    }
    return true;
}

void Linker_FreeModelCells(LinkerModelCells *membership)
{
    if (membership)
    {
        free(membership->bits);
        free(membership);
    }
}

bool Linker_AssignModelReflectionProbes(const uint16_t *nodes, unsigned int nodeWordCount,
                                        const LinkerBspRenderCells *cells, const LinkerBspReflectionProbes *probes,
                                        const GfxStaticModelInst *models, unsigned int modelCount,
                                        GfxStaticModelDrawInst *draw, char *error, size_t errorSize)
{
    if (!error && errorSize)
    {
        return false;
    }
    if (errorSize)
    {
        error[0] = 0;
    }
    bool valid = nodes && nodeWordCount && nodeWordCount <= 131073 && cells && cells->cells && cells->planes &&
                 cells->cellCount && cells->cellCount <= 1024 && cells->planeCount && cells->planeCount <= 65536 &&
                 probes && probes->probes && probes->count && probes->count <= 256 && modelCount <= 65535 &&
                 (!modelCount || (models && draw));
    valid = valid && ValidateCellTree(nodes, nodeWordCount, cells->planes, cells->planeCount, cells->cellCount);
    uint8_t *selected = valid && modelCount ? (uint8_t *)malloc(modelCount * sizeof(uint8_t)) : NULL;
    valid = valid && (!modelCount || selected);
    for (unsigned int i = 0; valid && i < probes->count; ++i)
    {
        for (unsigned int j = 0; valid && j < 3; ++j)
        {
            valid = isfinite(probes->probes[i].origin[j]);
        }
    }
    for (unsigned int i = 0; valid && i < cells->cellCount; ++i)
    {
        const GfxCell *cell = &cells->cells[i];
        valid = cell->reflectionProbeCount && cell->reflectionProbes;
        for (unsigned int j = 0; valid && j < cell->reflectionProbeCount; ++j)
        {
            valid = cell->reflectionProbes[j] < probes->count;
        }
    }
    for (unsigned int m = 0; valid && m < modelCount; ++m)
    {
        double center[3];
        for (unsigned int j = 0; valid && j < 3; ++j)
        {
            valid =
                isfinite(models[m].mins[j]) && isfinite(models[m].maxs[j]) && models[m].mins[j] <= models[m].maxs[j];
            center[j] = ((double)models[m].mins[j] + models[m].maxs[j]) * 0.5;
        }
        unsigned int node = 0, limit = nodeWordCount;
        while (valid && nodes[node] > cells->cellCount)
        {
            const unsigned int planeIndex = nodes[node] - cells->cellCount - 1;
            valid = planeIndex < cells->planeCount && limit - node >= 4;
            if (!valid)
            {
                break;
            }
            const unsigned int offset = nodes[node + 1];
            valid = offset >= 3 && offset < limit - node;
            if (!valid)
            {
                break;
            }
            const cplane_s *plane = &cells->planes[planeIndex];
            double distance = -plane->dist;
            valid = isfinite(plane->dist);
            for (unsigned int j = 0; valid && j < 3; ++j)
            {
                valid = isfinite(plane->normal[j]);
                distance += center[j] * plane->normal[j];
            }
            if (distance <= 0)
            {
                node += offset;
            }
            else
            {
                limit = node + offset;
                node += 2;
            }
        }
        if (!valid)
        {
            break;
        }
        const GfxCell *cell = nodes[node] ? &cells->cells[nodes[node] - 1] : NULL;
        const unsigned int count = cell ? cell->reflectionProbeCount : probes->count - 1;
        unsigned int best = 0;
        double nearest = DBL_MAX;
        for (unsigned int i = 0; i < count; ++i)
        {
            const unsigned int probe = cell ? cell->reflectionProbes[i] : i + 1;
            double distance = 0;
            for (unsigned int j = 0; j < 3; ++j)
            {
                const double delta = center[j] - probes->probes[probe].origin[j];
                distance += delta * delta;
            }
            if (distance < nearest)
            {
                nearest = distance;
                best = probe;
            }
        }
        selected[m] = (uint8_t)best;
    }
    if (valid)
    {
        for (unsigned int m = 0; m < modelCount; ++m)
        {
            draw[m].reflectionProbeIndex = selected[m];
        }
    }
    free(selected);
    if (!valid && errorSize)
    {
        snprintf(error, errorSize, "Invalid BSP cell or reflection-probe data while lighting static models");
    }
    return valid;
}

bool Linker_BuildModelCells(const uint16_t *nodes, unsigned int nodeWordCount, const cplane_s *planes,
                            unsigned int planeCount, unsigned int cellCount, const GfxStaticModelInst *models,
                            unsigned int modelCount, LinkerModelCells **membership, char *error, size_t errorSize)
{
    if (!membership || (!error && errorSize))
    {
        return false;
    }
    *membership = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    bool valid = nodes && nodeWordCount && nodeWordCount <= 131073 && planes && planeCount && planeCount <= 65536 &&
                 cellCount && cellCount <= 1024 && modelCount <= 65535 && (!modelCount || models);
    valid = valid && ValidateCellTree(nodes, nodeWordCount, planes, planeCount, cellCount);
    LinkerModelCells *result = valid ? (LinkerModelCells *)calloc(1, sizeof(LinkerModelCells)) : NULL;
    ModelCellFrame *stack =
        valid && modelCount ? (ModelCellFrame *)malloc(nodeWordCount * sizeof(ModelCellFrame)) : NULL;
    valid = valid && result && (!modelCount || stack);
    if (valid)
    {
        result->cellCount = cellCount;
        result->modelCount = modelCount;
        result->wordsPerModel = (cellCount + 31) / 32;
        if (modelCount)
        {
            result->bits = (uint32_t *)calloc((size_t)modelCount * result->wordsPerModel, sizeof(uint32_t));
            valid = result->bits != NULL;
        }
    }
    for (unsigned int i = 0; valid && i < modelCount; ++i)
    {
        valid = FilterModel(nodes, nodeWordCount, planes, planeCount, cellCount, &models[i], stack,
                            result->bits + (size_t)i * result->wordsPerModel);
    }
    free(stack);
    if (!valid)
    {
        Linker_FreeModelCells(result);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid BSP node stream, bounds or allocation while assigning model cells");
        }
        return false;
    }
    *membership = result;
    return true;
}

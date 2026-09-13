#include <universal/q_shared.h>
#include <gfx_d3d/r_bsp.h>
#include "native_bsp.h"
#include "native_surface_transforms.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct PortalVertex
{
    unsigned int parent, weight;
    double sum[3];
};

static unsigned int Root(PortalVertex *vertices, unsigned int index)
{
    while (vertices[index].parent != index)
    {
        vertices[index].parent = vertices[vertices[index].parent].parent;
        index = vertices[index].parent;
    }
    return index;
}

static bool Transform(const LinkerBspRenderGeometry *geometry, const GfxSurface *surface, GfxWorldVertex *output)
{
    const unsigned int first = surface->tris.firstVertex, count = surface->tris.vertexCount;
    const size_t indexCount = (size_t)surface->tris.triCount * 3;
    if (!count || !indexCount || first > geometry->vertexCount || count > geometry->vertexCount - first ||
        surface->tris.baseIndex > geometry->indexCount || indexCount > geometry->indexCount - surface->tris.baseIndex)
    {
        return false;
    }
    PortalVertex *vertices = (PortalVertex *)calloc(count, sizeof(PortalVertex));
    if (!vertices)
    {
        return false;
    }
    for (unsigned int i = 0; i < count; ++i)
    {
        vertices[i].parent = i;
    }
    const uint16_t *indices = geometry->indices + surface->tris.baseIndex;
    bool valid = true;
    for (size_t i = 0; valid && i < indexCount; i += 3)
    {
        valid = indices[i] < count && indices[i + 1] < count && indices[i + 2] < count;
        if (valid)
        {
            const unsigned int root = Root(vertices, indices[i]);
            vertices[Root(vertices, indices[i + 1])].parent = root;
            vertices[Root(vertices, indices[i + 2])].parent = root;
        }
    }
    for (size_t i = 0; valid && i < indexCount; ++i)
    {
        PortalVertex *component = &vertices[Root(vertices, indices[i])];
        ++component->weight;
        for (unsigned int axis = 0; axis < 3; ++axis)
        {
            const float coordinate = geometry->vertices[first + indices[i]].xyz[axis];
            valid = valid && isfinite(coordinate);
            component->sum[axis] += coordinate;
        }
    }
    for (size_t i = 0; valid && i < indexCount; ++i)
    {
        const PortalVertex *component = &vertices[Root(vertices, indices[i])];
        GfxWorldVertex *vertex = &output[first + indices[i]];
        vertex->texCoord[0] = (float)(component->sum[0] / component->weight);
        vertex->texCoord[1] = (float)(component->sum[1] / component->weight);
        vertex->lmapCoord[0] = (float)(component->sum[2] / component->weight);
        vertex->lmapCoord[1] = 1;
    }
    free(vertices);
    return valid;
}

bool Linker_TransformMagicPortals(LinkerBspRenderGeometry *geometry, char *error, size_t errorSize)
{
    if (!error && errorSize)
    {
        return false;
    }
    bool valid = geometry && (!geometry->surfaceCount || geometry->surfaces) &&
                 (!geometry->vertexCount || geometry->vertices) && (!geometry->indexCount || geometry->indices);
    bool needed = false;
    for (unsigned int i = 0; valid && i < geometry->surfaceCount; ++i)
    {
        valid = geometry->surfaces[i].material != NULL;
        needed = needed || (valid && (geometry->surfaces[i].material->info.gameFlags & 0x20));
    }
    GfxWorldVertex *output =
        valid && needed ? (GfxWorldVertex *)malloc((size_t)geometry->vertexCount * sizeof(GfxWorldVertex)) : NULL;
    valid = valid && (!needed || output);
    if (valid && needed)
    {
        memcpy(output, geometry->vertices, (size_t)geometry->vertexCount * sizeof(GfxWorldVertex));
        for (unsigned int i = 0; valid && i < geometry->surfaceCount; ++i)
        {
            if (geometry->surfaces[i].material->info.gameFlags & 0x20)
            {
                valid = Transform(geometry, &geometry->surfaces[i], output);
            }
        }
        if (valid)
        {
            memcpy(geometry->vertices, output, (size_t)geometry->vertexCount * sizeof(GfxWorldVertex));
        }
    }
    free(output);
    if (!valid && errorSize)
    {
        snprintf(error, errorSize, "Invalid magic-portal geometry or allocation failure");
    }
    return valid;
}

#include <universal/q_shared.h>
#include <qcommon/com_bsp.h>
#include <qcommon/qcommon.h>
#include <xanim/xmodel.h>
#include <DynEntity/DynEntity_client.h>
#include <database64/db_clipmap_layout.h>
#include <zlib/zlib.h>
#include <database64/db_package.h>
#include "native_bsp.h"
#include "native_collision_tree.h"
#include "native_model.h"
#include "native_image.h"
#include <gfx_d3d/r_bsp.h>
#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_reflection_probe.h>
#include <gfx_d3d/r_primarylights.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

struct BspInput
{
    const uint8_t *data;
    size_t size;
    size_t offsets[100];
    BspChunk chunks[100];
    uint count;
    char *error;
    size_t errorSize;
};

static bool Fail(BspInput *input, const char *message)
{
    if (input->errorSize)
    {
        snprintf(input->error, input->errorSize, "%s", message);
    }
    return false;
}

static bool Validate(BspInput *input)
{
    uint32_t header[3];
    if (!input->data || input->size < sizeof(header) || input->size > INT_MAX)
    {
        return Fail(input, "Invalid BSP file size");
    }
    memcpy(header, input->data, sizeof(header));
    if (memcmp(input->data, "IBSP", 4) || header[1] != BSP_VERSION || header[2] > ARRAY_COUNT(input->chunks))
    {
        return Fail(input, "Native linker requires a KIWI BSP version 22");
    }
    input->count = header[2];
    const size_t directorySize = input->count * sizeof(BspChunk);
    if (directorySize > input->size - sizeof(header))
    {
        return Fail(input, "Truncated BSP lump directory");
    }
    memcpy(input->chunks, input->data + sizeof(header), directorySize);
    size_t offset = sizeof(header) + directorySize;
    for (uint i = 0; i < input->count; ++i)
    {
        for (uint j = 0; j < i; ++j)
        {
            if (input->chunks[i].type == input->chunks[j].type)
            {
                return Fail(input, "Duplicate BSP lump");
            }
        }
        const size_t size = input->chunks[i].length;
        if (offset > input->size || size > input->size - offset)
        {
            return Fail(input, "BSP lump extends beyond input file");
        }
        input->offsets[i] = offset;
        offset += (size + 3) & ~(size_t)3;
    }
    // Only the last lump's optional alignment padding may be absent.
    if (offset < input->size || offset - input->size > 3)
    {
        return Fail(input, "Unexpected data after BSP lumps");
    }
    return true;
}

static const void *Lump(BspInput *input, LumpType type, size_t elementSize, size_t *count, bool optional = false)
{
    for (uint i = 0; i < input->count; ++i)
    {
        if (input->chunks[i].type == type)
        {
            if (input->chunks[i].length % elementSize)
            {
                Fail(input, "BSP lump has a partial element");
                return NULL;
            }
            *count = input->chunks[i].length / elementSize;
            return input->data + input->offsets[i];
        }
    }
    if (optional)
    {
        *count = 0;
        return input->data;
    }
    Fail(input, "BSP is missing a required world lump");
    return NULL;
}

void Linker_FreeRenderGeometry(LinkerBspRenderGeometry *geometry)
{
    if (geometry)
    {
        free(geometry->vertices);
        free(geometry->surfaces);
        free(geometry->indices);
        free(geometry->materialIndices);
        free(geometry->materials);
        free(geometry->layerData);
        free(geometry);
    }
}

bool Linker_ImportRenderGeometry(const void *bsp, size_t size, bool layered, LinkerBspRenderGeometry **geometry,
                                 char *error, size_t errorSize)
{
    if (!geometry || (!error && errorSize))
    {
        return false;
    }
    *geometry = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    bool valid = Validate(&input);
    size_t vertexCount = 0, surfaceCount = 0, indexCount = 0, materialCount = 0;
    const void *diskVertices =
        valid ? Lump(&input, layered ? LUMP_DRAWVERTS : LUMP_UNLAYERED_DRAWVERTS, sizeof(DiskGfxVertex), &vertexCount)
              : NULL;
    const void *diskSurfaces = valid ? Lump(&input, layered ? LUMP_TRIANGLES : LUMP_UNLAYERED_TRIANGLES,
                                            sizeof(DiskTriangleSoup), &surfaceCount)
                                     : NULL;
    const void *diskIndices =
        valid ? Lump(&input, layered ? LUMP_DRAWINDICES : LUMP_UNLAYERED_DRAWINDICES, sizeof(uint16_t), &indexCount)
              : NULL;
    const void *diskMaterials = valid ? Lump(&input, LUMP_MATERIALS, sizeof(dmaterial_t), &materialCount) : NULL;
    valid = valid && diskVertices && diskSurfaces && diskIndices && diskMaterials && vertexCount && surfaceCount &&
            surfaceCount <= 65536 && indexCount && materialCount && materialCount <= 65536;
    LinkerBspRenderGeometry *result =
        valid ? (LinkerBspRenderGeometry *)calloc(1, sizeof(LinkerBspRenderGeometry)) : NULL;
    valid = valid && result;
    if (valid)
    {
        result->vertexCount = (unsigned int)vertexCount;
        result->surfaceCount = (unsigned int)surfaceCount;
        result->materialCount = (unsigned int)materialCount;
        result->vertices = (GfxWorldVertex *)calloc(vertexCount, sizeof(GfxWorldVertex));
        result->surfaces = (GfxSurface *)calloc(surfaceCount, sizeof(GfxSurface));
        result->materialIndices = (uint16_t *)malloc(surfaceCount * sizeof(uint16_t));
        result->materials = (dmaterial_t *)malloc(materialCount * sizeof(dmaterial_t));
        valid = result->vertices && result->surfaces && result->materialIndices && result->materials;
    }
    if (valid)
    {
        memcpy(result->materials, diskMaterials, materialCount * sizeof(dmaterial_t));
        for (unsigned int i = 0; valid && i < materialCount; ++i)
        {
            valid = result->materials[i].material[0] && memchr(result->materials[i].material, 0, 64);
        }
        result->layerDataSize = 4;
        const void *layer = NULL;
        if (layered)
        {
            for (unsigned int i = 0; i < input.count; ++i)
            {
                if (input.chunks[i].type == LUMP_VERTEX_LAYER_DATA && input.chunks[i].length)
                {
                    result->layerDataSize = input.chunks[i].length;
                    layer = input.data + input.offsets[i];
                    break;
                }
            }
        }
        result->layerData = (uint8_t *)calloc(result->layerDataSize, 1);
        valid = valid && result->layerData;
        if (valid && layer)
        {
            memcpy(result->layerData, layer, result->layerDataSize);
        }
    }
    for (unsigned int i = 0; valid && i < vertexCount; ++i)
    {
        DiskGfxVertex source;
        memcpy(&source, (const uint8_t *)diskVertices + i * sizeof(DiskGfxVertex), sizeof(source));
        GfxWorldVertex *vertex = &result->vertices[i];
        memcpy(vertex->xyz, source.xyz, sizeof(float[3]));
        memcpy(&vertex->color, source.color, sizeof(uint8_t[4]));
        memcpy(vertex->texCoord, source.texCoord, sizeof(float[2]));
        memcpy(vertex->lmapCoord, source.lmapCoord, sizeof(float[2]));
        for (int a = 0; a < 3; ++a)
        {
            valid = valid && isfinite(source.xyz[a]) && isfinite(source.binormal[a]);
        }
        for (int a = 0; a < 2; ++a)
        {
            valid = valid && isfinite(source.texCoord[a]) && isfinite(source.lmapCoord[a]);
        }
        valid = valid && Linker_PackUnitVector(source.normal, &vertex->normal) &&
                Linker_PackUnitVector(source.tangent, &vertex->tangent);
        double handedness = 0;
        for (int a = 0; a < 3; ++a)
        {
            const int b = (a + 1) % 3, c = (a + 2) % 3;
            handedness +=
                ((double)source.normal[b] * source.tangent[c] - (double)source.normal[c] * source.tangent[b]) *
                source.binormal[a];
        }
        vertex->binormalSign = handedness < 0 ? -1.0f : 1.0f;
    }
    size_t totalIndices = 0;
    for (unsigned int i = 0; valid && i < surfaceCount; ++i)
    {
        DiskTriangleSoup source;
        const uint8_t *bytes = (const uint8_t *)diskSurfaces + i * sizeof(DiskTriangleSoup);
        memcpy(&source, bytes, sizeof(source));
        valid = bytes[offsetof(DiskTriangleSoup, castsSunShadow)] <= 1 && source.materialIndex < materialCount &&
                source.firstVertex <= vertexCount && source.vertexCount &&
                source.vertexCount <= vertexCount - source.firstVertex && source.firstIndex >= 0 &&
                (size_t)source.firstIndex <= indexCount && source.indexCount && source.indexCount % 3 == 0 &&
                source.indexCount <= indexCount - source.firstIndex && source.lightmapIndex < 32 &&
                source.vertexLayerData >= -1 && (!layered || source.vertexLayerData < (int)result->layerDataSize);
        totalIndices += source.indexCount;
        valid = valid && totalIndices <= INT_MAX / sizeof(uint16_t);
    }
    if (valid)
    {
        result->indexCount = (unsigned int)totalIndices;
        result->indices = (uint16_t *)malloc(totalIndices * sizeof(uint16_t));
        valid = result->indices != NULL;
    }
    unsigned int first = 0;
    for (unsigned int i = 0; valid && i < surfaceCount; ++i)
    {
        DiskTriangleSoup source;
        memcpy(&source, (const uint8_t *)diskSurfaces + i * sizeof(DiskTriangleSoup), sizeof(source));
        GfxSurface *surface = &result->surfaces[i];
        surface->tris.baseIndex = first;
        surface->tris.firstVertex = (int)source.firstVertex;
        surface->tris.vertexCount = source.vertexCount;
        surface->tris.triCount = source.indexCount / 3;
        surface->tris.vertexLayerData = layered ? source.vertexLayerData : 0;
        surface->lightmapIndex = source.lightmapIndex;
        surface->reflectionProbeIndex = source.reflectionProbeIndex;
        surface->primaryLightIndex = source.primaryLightIndex;
        surface->flags = source.castsSunShadow ? 1 : 0;
        result->materialIndices[i] = source.materialIndex;
        memcpy(result->indices + first, (const uint8_t *)diskIndices + source.firstIndex * sizeof(uint16_t),
               source.indexCount * sizeof(uint16_t));
        for (int a = 0; a < 3; ++a)
        {
            surface->bounds[0][a] = FLT_MAX;
            surface->bounds[1][a] = -FLT_MAX;
        }
        for (unsigned int j = 0; valid && j < source.indexCount; ++j)
        {
            const unsigned int index = result->indices[first + j];
            if (index >= source.vertexCount)
            {
                valid = false;
                break;
            }
            const GfxWorldVertex *vertex = &result->vertices[source.firstVertex + index];
            for (int a = 0; a < 3; ++a)
            {
                surface->bounds[0][a] = fminf(surface->bounds[0][a], vertex->xyz[a]);
                surface->bounds[1][a] = fmaxf(surface->bounds[1][a], vertex->xyz[a]);
            }
        }
        first += source.indexCount;
    }
    if (!valid)
    {
        Linker_FreeRenderGeometry(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Invalid BSP render geometry or allocation failure");
        }
        return false;
    }
    *geometry = result;
    return true;
}

bool Linker_BindRenderMaterials(LinkerBspRenderGeometry *geometry, Material *const *materials,
                                unsigned int materialCount, char *error, size_t errorSize)
{
    if (!error && errorSize)
    {
        return false;
    }
    if (errorSize)
    {
        error[0] = 0;
    }
    bool valid = geometry && materials && materialCount == geometry->materialCount &&
                 (!geometry->surfaceCount || (geometry->surfaces && geometry->materialIndices));
    // MTL_WORLDVERT_* order. Matches g_layerDataStride[worldVertFormat + 2] in the renderer.
    const unsigned int strides[] = {0, 8, 12, 16, 20, 24, 24, 28, 32, 32, 36, 40};
    unsigned int surfaceIndex = 0;
    for (; valid && surfaceIndex < geometry->surfaceCount; ++surfaceIndex)
    {
        const unsigned int index = geometry->materialIndices[surfaceIndex];
        valid = index < materialCount && materials[index] && materials[index]->techniqueSet &&
                materials[index]->techniqueSet->worldVertFormat < ARRAY_COUNT(strides);
        if (!valid)
        {
            break;
        }
        const GfxSurface *surface = &geometry->surfaces[surfaceIndex];
        const unsigned int stride = strides[materials[index]->techniqueSet->worldVertFormat];
        if (stride)
        {
            const int offset = surface->tris.vertexLayerData;
            valid = geometry->layerData && offset >= 0 && (unsigned int)offset <= geometry->layerDataSize &&
                    (size_t)surface->tris.vertexCount * stride <= geometry->layerDataSize - (unsigned int)offset;
            // All layer streams are composed of packed 4-byte vertex attributes.
            valid = valid && !(offset & 3);
        }
    }
    if (!valid)
    {
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid material or vertex-layer span at BSP surface %u", surfaceIndex);
        }
        return false;
    }
    for (unsigned int i = 0; i < geometry->surfaceCount; ++i)
    {
        geometry->surfaces[i].material = materials[geometry->materialIndices[i]];
    }
    return true;
}

void Linker_FreeRenderGroups(LinkerBspRenderGroups *groups)
{
    if (groups)
    {
        free(groups->models);
        free(groups->cullGroups);
        free(groups);
    }
}

bool Linker_ImportRenderGroups(const void *bsp, size_t size, bool layered, unsigned int surfaceCount,
                               LinkerBspRenderGroups **groups, char *error, size_t errorSize)
{
    if (!groups || (!error && errorSize))
    {
        return false;
    }
    *groups = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    bool valid = Validate(&input) && surfaceCount <= 65536;
    size_t modelCount = 0, cullGroupCount = 0;
    const void *models = valid ? Lump(&input, LUMP_MODELS, sizeof(DiskBrushModel), &modelCount) : NULL;
    const void *culls = valid ? Lump(&input, layered ? LUMP_CULLGROUPS : LUMP_UNLAYERED_CULLGROUPS,
                                     sizeof(DiskGfxCullGroup), &cullGroupCount, true)
                              : NULL;
    valid = valid && models && culls && modelCount && modelCount <= 1024;
    LinkerBspRenderGroups *result = valid ? (LinkerBspRenderGroups *)calloc(1, sizeof(LinkerBspRenderGroups)) : NULL;
    valid = valid && result;
    if (valid)
    {
        result->modelCount = (unsigned int)modelCount;
        result->cullGroupCount = (unsigned int)cullGroupCount;
        result->models = (GfxBrushModel *)calloc(modelCount, sizeof(GfxBrushModel));
        result->cullGroups = cullGroupCount ? (GfxCullGroup *)calloc(cullGroupCount, sizeof(GfxCullGroup)) : NULL;
        valid = result->models && (!cullGroupCount || result->cullGroups);
    }
    const int type = layered ? 0 : 1;
    for (unsigned int i = 0; valid && i < modelCount; ++i)
    {
        DiskBrushModel source;
        memcpy(&source, (const uint8_t *)models + i * sizeof(DiskBrushModel), sizeof(source));
        GfxBrushModel *model = &result->models[i];
        const unsigned int first = source.firstTriSoup[type], count = source.triSoupCount[type];
        valid = !count || (first <= surfaceCount && count <= surfaceCount - first);
        // The renderer expects the world model's surfaces at the beginning of the array.
        valid = valid && (i || !count || !first);
        for (int a = 0; valid && a < 3; ++a)
        {
            valid = isfinite(source.mins[a]) && isfinite(source.maxs[a]) && source.mins[a] <= source.maxs[a];
            model->bounds[0][a] = source.mins[a];
            model->bounds[1][a] = source.maxs[a];
            model->writable.mins[a] = source.mins[a];
            model->writable.maxs[a] = source.maxs[a];
        }
        model->surfaceCount = (uint16_t)count;
        model->startSurfIndex = count ? (uint16_t)first : UINT16_MAX;
        // surfaceCountNoDecal is assigned after material-based surface sorting.
    }
    for (unsigned int i = 0; valid && i < cullGroupCount; ++i)
    {
        DiskGfxCullGroup source;
        memcpy(&source, (const uint8_t *)culls + i * sizeof(DiskGfxCullGroup), sizeof(source));
        GfxCullGroup *group = &result->cullGroups[i];
        valid = !source.surfaceCount ||
                (source.firstSurface <= surfaceCount && source.surfaceCount <= surfaceCount - source.firstSurface);
        for (int a = 0; valid && a < 3; ++a)
        {
            valid = isfinite(source.mins[a]) && isfinite(source.maxs[a]) && source.mins[a] <= source.maxs[a];
            group->mins[a] = source.mins[a];
            group->maxs[a] = source.maxs[a];
        }
        group->surfaceCount = (int)source.surfaceCount;
        group->startSurfIndex = source.surfaceCount ? (int)source.firstSurface : -1;
    }
    if (!valid)
    {
        Linker_FreeRenderGroups(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Invalid BSP brush-model or cull-group bounds/ranges");
        }
        return false;
    }
    *groups = result;
    return true;
}

void Linker_FreeRenderTrees(LinkerBspRenderTrees *trees)
{
    if (trees)
    {
        free(trees->trees);
        free(trees->roots);
        free(trees);
    }
}

bool Linker_ImportRenderTrees(const void *bsp, size_t size, bool layered, const LinkerBspRenderGeometry *geometry,
                              LinkerBspRenderTrees **trees, char *error, size_t errorSize)
{
    if (!trees || (!error && errorSize))
    {
        return false;
    }
    *trees = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    bool valid = Validate(&input) && geometry && (!geometry->surfaceCount || geometry->surfaces);
    size_t count = 0;
    const void *disk =
        valid ? Lump(&input, layered ? LUMP_AABBTREES : LUMP_UNLAYERED_AABBTREES, sizeof(DiskGfxAabbTree), &count)
              : NULL;
    valid = valid && disk && count && count <= 65536;
    LinkerBspRenderTrees *result = valid ? (LinkerBspRenderTrees *)calloc(1, sizeof(LinkerBspRenderTrees)) : NULL;
    valid = valid && result;
    struct TreeFrame
    {
        unsigned int node, firstChild, nextChild;
    };
    TreeFrame *stack = valid ? (TreeFrame *)calloc(count, sizeof(TreeFrame)) : NULL;
    valid = valid && stack;
    if (valid)
    {
        result->treeCount = (unsigned int)count;
        result->trees = (GfxAabbTree *)calloc(count, sizeof(GfxAabbTree));
        result->roots = (uint8_t *)calloc(count, sizeof(uint8_t));
        valid = result->trees && result->roots;
    }
    for (unsigned int i = 0; valid && i < count; ++i)
    {
        DiskGfxAabbTree source;
        memcpy(&source, (const uint8_t *)disk + i * sizeof(DiskGfxAabbTree), sizeof(source));
        valid = source.childCount <= UINT16_MAX && source.surfaceCount <= UINT16_MAX &&
                (!source.surfaceCount ||
                 (source.firstSurface <= UINT16_MAX && source.firstSurface <= geometry->surfaceCount &&
                  source.surfaceCount <= geometry->surfaceCount - source.firstSurface));
        GfxAabbTree *tree = &result->trees[i];
        tree->childCount = (uint16_t)source.childCount;
        tree->surfaceCount = (uint16_t)source.surfaceCount;
        tree->startSurfIndex = source.surfaceCount ? (uint16_t)source.firstSurface : 0;
        for (int a = 0; a < 3; ++a)
        {
            tree->mins[a] = FLT_MAX;
            tree->maxs[a] = -FLT_MAX;
        }
    }
    unsigned int used = 0;
    while (valid && used < count)
    {
        const unsigned int root = used++;
        result->roots[root] = 1;
        unsigned int depth = 1;
        stack[0] = {root, 0, 0};
        while (valid && depth)
        {
            TreeFrame *frame = &stack[depth - 1];
            GfxAabbTree *node = &result->trees[frame->node];
            if (node->childCount && !frame->firstChild)
            {
                if (node->childCount > count - used)
                {
                    valid = Fail(&input, "BSP render tree children extend beyond the tree array");
                    break;
                }
                frame->firstChild = used;
                used += node->childCount;
                node->childrenOffset = (ptrdiff_t)(frame->firstChild - frame->node) * sizeof(GfxAabbTree);
                unsigned int nextSurface = node->startSurfIndex;
                for (unsigned int i = 0; valid && i < node->childCount; ++i)
                {
                    const GfxAabbTree *child = &result->trees[frame->firstChild + i];
                    if (child->surfaceCount)
                    {
                        valid = child->startSurfIndex == nextSurface;
                        nextSurface += child->surfaceCount;
                    }
                }
                if (!valid || nextSurface != (unsigned int)node->startSurfIndex + node->surfaceCount)
                {
                    valid = Fail(&input, "BSP render tree children do not cover the parent surface range");
                    break;
                }
            }
            if (frame->nextChild < node->childCount)
            {
                const unsigned int child = frame->firstChild + frame->nextChild++;
                const GfxAabbTree *childNode = &result->trees[child];
                if (childNode->surfaceCount && (childNode->startSurfIndex < node->startSurfIndex ||
                                                (unsigned int)childNode->startSurfIndex + childNode->surfaceCount >
                                                    (unsigned int)node->startSurfIndex + node->surfaceCount))
                {
                    valid = Fail(&input, "BSP render tree child surfaces exceed the parent range");
                    break;
                }
                stack[depth++] = {child, 0, 0};
                continue;
            }
            if (!node->childCount)
            {
                for (unsigned int i = 0; i < node->surfaceCount; ++i)
                {
                    const GfxSurface *surface = &geometry->surfaces[node->startSurfIndex + i];
                    for (int a = 0; a < 3; ++a)
                    {
                        node->mins[a] = fminf(node->mins[a], surface->bounds[0][a]);
                        node->maxs[a] = fmaxf(node->maxs[a], surface->bounds[1][a]);
                    }
                }
            }
            --depth;
            if (depth && node->surfaceCount)
            {
                GfxAabbTree *parent = &result->trees[stack[depth - 1].node];
                for (int a = 0; a < 3; ++a)
                {
                    parent->mins[a] = fminf(parent->mins[a], node->mins[a]);
                    parent->maxs[a] = fmaxf(parent->maxs[a], node->maxs[a]);
                }
            }
            if (!node->surfaceCount)
            {
                memset(node->mins, 0, sizeof(float[3]));
                memset(node->maxs, 0, sizeof(float[3]));
            }
        }
    }
    free(stack);
    if (!valid)
    {
        Linker_FreeRenderTrees(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Invalid BSP render tree or allocation failure");
        }
        return false;
    }
    *trees = result;
    return true;
}

void Linker_FreeRenderCells(LinkerBspRenderCells *cells)
{
    if (cells)
    {
        for (unsigned int i = 0; cells->cells && i < cells->cellCount; ++i)
        {
            free(cells->cells[i].reflectionProbes);
        }
        free(cells->cells);
        free(cells->portals);
        free(cells->vertices);
        free(cells->planes);
        free(cells->cullIndices);
        free(cells);
    }
}

bool Linker_ImportRenderCells(const void *bsp, size_t size, bool layered, const LinkerBspRenderTrees *trees,
                              const LinkerBspRenderGroups *groups, LinkerBspRenderCells **cells, char *error,
                              size_t errorSize)
{
    if (!cells || (!error && errorSize))
    {
        return false;
    }
    *cells = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    bool valid = Validate(&input) && trees && trees->trees && trees->roots && groups;
    size_t cellCount = 0, portalCount = 0, vertexCount = 0, planeCount = 0, cullCount = 0;
    const uint8_t *diskCells = valid ? (const uint8_t *)Lump(&input, LUMP_CELLS, 112, &cellCount) : NULL;
    const uint8_t *diskPortals = valid ? (const uint8_t *)Lump(&input, LUMP_PORTALS, 16, &portalCount, true) : NULL;
    const void *diskVertices = valid ? Lump(&input, LUMP_PORTALVERTS, sizeof(float[3]), &vertexCount, true) : NULL;
    const void *diskPlanes = valid ? Lump(&input, LUMP_PLANES, sizeof(float[4]), &planeCount) : NULL;
    const void *diskCulls = valid ? Lump(&input, LUMP_CULLGROUPINDICES, sizeof(int32_t), &cullCount, true) : NULL;
    valid = valid && diskCells && diskPortals && diskVertices && diskPlanes && diskCulls && cellCount &&
            cellCount <= 1024 && planeCount && planeCount <= 65536;
    LinkerBspRenderCells *result = valid ? (LinkerBspRenderCells *)calloc(1, sizeof(LinkerBspRenderCells)) : NULL;
    valid = valid && result;
    if (valid)
    {
        result->cellCount = (unsigned int)cellCount;
        result->portalCount = (unsigned int)portalCount;
        result->vertexCount = (unsigned int)vertexCount;
        result->planeCount = (unsigned int)planeCount;
        result->cullIndexCount = (unsigned int)cullCount;
        result->cells = (GfxCell *)calloc(cellCount, sizeof(GfxCell));
        result->portals = portalCount ? (GfxPortal *)calloc(portalCount, sizeof(GfxPortal)) : NULL;
        result->vertices = vertexCount ? (float(*)[3])malloc(vertexCount * sizeof(float[3])) : NULL;
        result->planes = (cplane_s *)calloc(planeCount, sizeof(cplane_s));
        result->cullIndices = cullCount ? (int *)malloc(cullCount * sizeof(int)) : NULL;
        valid = result->cells && (!portalCount || result->portals) && (!vertexCount || result->vertices) &&
                result->planes && (!cullCount || result->cullIndices);
    }
    for (unsigned int i = 0; valid && i < planeCount; ++i)
    {
        float source[4];
        memcpy(source, (const uint8_t *)diskPlanes + i * sizeof(float[4]), sizeof(source));
        cplane_s *plane = &result->planes[i];
        plane->type = 3;
        plane->dist = source[3];
        double length = 0;
        valid = isfinite(source[3]);
        for (int a = 0; valid && a < 3; ++a)
        {
            valid = isfinite(source[a]);
            plane->normal[a] = source[a];
            length += (double)source[a] * source[a];
            if (source[a] < 0)
            {
                plane->signbits |= 1 << a;
            }
            if (source[a] == 1)
            {
                plane->type = (uint8_t)a;
            }
        }
        valid = valid && fabs(length - 1) < 0.002;
    }
    for (unsigned int i = 0; valid && i < vertexCount; ++i)
    {
        memcpy(result->vertices[i], (const uint8_t *)diskVertices + i * sizeof(float[3]), sizeof(float[3]));
        for (int a = 0; valid && a < 3; ++a)
        {
            valid = isfinite(result->vertices[i][a]);
        }
    }
    for (unsigned int i = 0; valid && i < cullCount; ++i)
    {
        memcpy(&result->cullIndices[i], (const uint8_t *)diskCulls + i * sizeof(int32_t), sizeof(int32_t));
        valid = result->cullIndices[i] >= 0 && (unsigned int)result->cullIndices[i] < groups->cullGroupCount;
    }
    for (unsigned int i = 0; valid && i < cellCount; ++i)
    {
        const uint8_t *source = diskCells + i * 112;
        GfxCell *cell = &result->cells[i];
        memcpy(cell->mins, source, sizeof(float[3]));
        memcpy(cell->maxs, source + 12, sizeof(float[3]));
        for (int a = 0; valid && a < 3; ++a)
        {
            valid = isfinite(cell->mins[a]) && isfinite(cell->maxs[a]) && cell->mins[a] <= cell->maxs[a];
        }
        uint16_t tree;
        uint32_t ranges[4];
        memcpy(&tree, source + (layered ? 24 : 26), sizeof(tree));
        memcpy(ranges, source + 28, sizeof(ranges));
        valid = valid && tree < trees->treeCount && trees->roots[tree] && ranges[0] <= portalCount &&
                ranges[1] <= portalCount - ranges[0] && ranges[2] <= cullCount && ranges[3] <= cullCount - ranges[2] &&
                source[44] <= 64;
        if (!valid)
        {
            break;
        }
        unsigned int end = tree + 1;
        while (end < trees->treeCount && !trees->roots[end])
        {
            ++end;
        }
        cell->aabbTree = &trees->trees[tree];
        cell->aabbTreeCount = (int)(end - tree);
        cell->portalCount = (int)ranges[1];
        cell->portals = ranges[1] ? &result->portals[ranges[0]] : NULL;
        cell->cullGroupCount = (int)ranges[3];
        cell->cullGroups = ranges[3] ? &result->cullIndices[ranges[2]] : NULL;
        cell->reflectionProbeCount = source[44] ? source[44] : 1;
        cell->reflectionProbes = (uint8_t *)calloc(cell->reflectionProbeCount, sizeof(uint8_t));
        valid = cell->reflectionProbes != NULL;
        if (valid && source[44])
        {
            memcpy(cell->reflectionProbes, source + 45, cell->reflectionProbeCount);
        }
    }
    for (unsigned int i = 0; valid && i < portalCount; ++i)
    {
        const uint8_t *source = diskPortals + i * 16;
        uint32_t refs[3];
        memcpy(refs, source, sizeof(refs));
        const unsigned int count = source[12];
        valid = refs[0] < planeCount && refs[1] < cellCount && refs[2] <= vertexCount && count >= 3 &&
                count <= vertexCount - refs[2];
        if (!valid)
        {
            break;
        }
        GfxPortal *portal = &result->portals[i];
        const cplane_s *plane = &result->planes[refs[0]];
        portal->cell = &result->cells[refs[1]];
        portal->vertices = &result->vertices[refs[2]];
        portal->vertexCount = (uint8_t)count;
        memcpy(portal->plane.coeffs, plane->normal, sizeof(float[3]));
        portal->plane.coeffs[3] = -plane->dist;
        int least = 0;
        for (int a = 0; a < 3; ++a)
        {
            portal->plane.side[a] = (uint8_t)(a * sizeof(float) + (plane->normal[a] > 0 ? sizeof(float[3]) : 0));
            if (fabsf(plane->normal[a]) < fabsf(plane->normal[least]))
            {
                least = a;
            }
        }
        double length = 0;
        for (int a = 0; a < 3; ++a)
        {
            portal->hullAxis[0][a] = (a == least ? 1.0f : 0.0f) - plane->normal[a] * plane->normal[least];
            length += (double)portal->hullAxis[0][a] * portal->hullAxis[0][a];
        }
        const float scale = (float)(1 / sqrt(length));
        for (int a = 0; a < 3; ++a)
        {
            portal->hullAxis[0][a] *= scale;
        }
        for (int a = 0; a < 3; ++a)
        {
            const int b = (a + 1) % 3, c = (a + 2) % 3;
            portal->hullAxis[1][a] =
                plane->normal[b] * portal->hullAxis[0][c] - plane->normal[c] * portal->hullAxis[0][b];
        }
    }
    if (!valid)
    {
        Linker_FreeRenderCells(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Invalid BSP cell/portal bounds or references");
        }
        return false;
    }
    *cells = result;
    return true;
}

void Linker_FreeLightmaps(LinkerBspLightmaps *lightmaps)
{
    if (lightmaps)
    {
        for (unsigned int i = 0; lightmaps->lightmaps && i < lightmaps->count; ++i)
        {
            Linker_FreeImage(lightmaps->lightmaps[i].primary);
            Linker_FreeImage(lightmaps->lightmaps[i].secondary);
        }
        free(lightmaps->lightmaps);
        free(lightmaps);
    }
}

bool Linker_ImportLightmaps(const void *bsp, size_t size, const char *mapName, const LinkerBspRenderGeometry *geometry,
                            LinkerBspLightmaps **lightmaps, char *error, size_t errorSize)
{
    if (!lightmaps || (!error && errorSize))
    {
        return false;
    }
    *lightmaps = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    char normalized[DB64_PACKAGE_PATH];
    bool valid = Validate(&input) && mapName && DB64_NormalizePath(mapName, normalized, sizeof(normalized)) &&
                 geometry && (!geometry->surfaceCount || geometry->surfaces);
    size_t count = 0;
    const uint8_t *pixels = valid ? (const uint8_t *)Lump(&input, LUMP_LIGHTBYTES, 3145728, &count, true) : NULL;
    unsigned int used = 0;
    for (unsigned int i = 0; valid && i < geometry->surfaceCount; ++i)
    {
        const unsigned int index = geometry->surfaces[i].lightmapIndex;
        valid = index <= 31;
        if (index < 31 && index >= used)
        {
            used = index + 1;
        }
    }
    valid = valid && pixels && count <= 31 && (!count || count == used);
    LinkerBspLightmaps *result = valid ? (LinkerBspLightmaps *)calloc(1, sizeof(LinkerBspLightmaps)) : NULL;
    valid = valid && result;
    uint8_t *white = valid && used && !count ? (uint8_t *)malloc(3145728) : NULL;
    valid = valid && (!used || count || white);
    if (white)
    {
        memset(white, 255, 3145728);
    }
    if (valid)
    {
        result->count = used;
        result->lightmaps = used ? (GfxLightmapArray *)calloc(used, sizeof(GfxLightmapArray)) : NULL;
        valid = !used || result->lightmaps;
    }
    for (unsigned int i = 0; valid && i < used; ++i)
    {
        const uint8_t *tile = count ? pixels + (size_t)i * 3145728 : white;
        char name[DB64_PACKAGE_PATH];
        int length = snprintf(name, sizeof(name), "lightmaps/%s/%u_primary", normalized, i);
        valid = length > 0 && (size_t)length < sizeof(name);
        if (valid)
        {
            valid = Linker_CreateImage(name, 1024, 1024, D3DFMT_L8, IMG_FLAG_NOPICMIP | IMG_FLAG_NOMIPMAPS,
                                       IMG_CATEGORY_LIGHTMAP, TS_FUNCTION, 4, tile + 2097152, 1048576,
                                       &result->lightmaps[i].primary, error, errorSize);
        }
        length = snprintf(name, sizeof(name), "lightmaps/%s/%u_secondary", normalized, i);
        valid = valid && length > 0 && (size_t)length < sizeof(name);
        if (valid)
        {
            valid = Linker_CreateImage(name, 512, 1024, D3DFMT_A8R8G8B8, IMG_FLAG_NOPICMIP | IMG_FLAG_NOMIPMAPS,
                                       IMG_CATEGORY_LIGHTMAP, TS_FUNCTION, 4, tile, 2097152,
                                       &result->lightmaps[i].secondary, error, errorSize);
        }
    }
    free(white);
    if (!valid)
    {
        Linker_FreeLightmaps(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Invalid BSP lightmap count or allocation failure");
        }
        return false;
    }
    *lightmaps = result;
    return true;
}

bool Linker_ParseReflectionCorrections(const void *data, size_t size, ColorCorrectionData **corrections,
                                       unsigned int *count, char *error, size_t errorSize)
{
    if (!corrections || !count || (!error && errorSize))
    {
        return false;
    }
    *corrections = NULL;
    *count = 0;
    if (errorSize)
    {
        error[0] = 0;
    }
    bool valid = data && size && size <= 1048576 && !memchr(data, 0, size);
    ColorCorrectionData *table = valid ? (ColorCorrectionData *)calloc(1024, sizeof(ColorCorrectionData)) : NULL;
    valid = valid && table;
    const char *cursor = (const char *)data;
    const char *end = valid ? cursor + size : cursor;
    unsigned int rows = 0, entries = 0;
    while (valid && cursor < end)
    {
        while (cursor < end && (*cursor == '\r' || *cursor == '\n' || *cursor == ' ' || *cursor == '\t'))
        {
            ++cursor;
        }
        if (cursor == end)
        {
            break;
        }
        char fields[5][64] = {};
        for (unsigned int column = 0; valid && column < 5; ++column)
        {
            while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
            {
                ++cursor;
            }
            const bool quoted = cursor < end && *cursor == '"';
            bool closed = !quoted;
            if (quoted)
            {
                ++cursor;
            }
            size_t length = 0;
            while (valid && cursor < end)
            {
                char ch = *cursor;
                if (ch == '\r' || ch == '\n' || (!quoted && ch == ','))
                {
                    break;
                }
                ++cursor;
                if (quoted && ch == '"')
                {
                    if (cursor < end && *cursor == '"')
                    {
                        ++cursor;
                    }
                    else
                    {
                        closed = true;
                        break;
                    }
                }
                if (length == 63)
                {
                    valid = false;
                    break;
                }
                fields[column][length++] = ch;
            }
            if (!quoted)
            {
                while (length && (fields[column][length - 1] == ' ' || fields[column][length - 1] == '\t'))
                {
                    fields[column][--length] = 0;
                }
            }
            while (cursor < end && (*cursor == ' ' || *cursor == '\t'))
            {
                ++cursor;
            }
            valid = valid && closed && length;
            if (column < 4)
            {
                valid = valid && cursor < end && *cursor == ',';
                if (valid)
                {
                    ++cursor;
                }
            }
            else
            {
                valid = valid && (cursor == end || *cursor == '\r' || *cursor == '\n');
            }
        }
        if (valid && !rows)
        {
            const char *names[] = {"name", "black_level", "white_level", "gamma", "saturation"};
            for (unsigned int i = 0; valid && i < 5; ++i)
            {
                valid = !_stricmp(fields[i], names[i]);
            }
        }
        else if (valid)
        {
            valid = entries < 1024;
            float values[4];
            for (unsigned int i = 0; valid && i < 4; ++i)
            {
                char *tail;
                values[i] = strtof(fields[i + 1], &tail);
                valid = tail != fields[i + 1] && !*tail && isfinite(values[i]);
            }
            if (valid)
            {
                valid = values[1] > values[0] && isfinite(values[1] - values[0]) && values[2] > 0;
                ColorCorrectionData *entry = &table[entries++];
                memcpy(entry->name, fields[0], sizeof(entry->name));
                entry->black_level = values[0];
                entry->white_level = values[1];
                entry->gamma = values[2];
                entry->saturation = values[3];
                entry->range = values[1] - values[0];
            }
        }
        ++rows;
    }
    if (!valid || !rows)
    {
        free(table);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid reflections.csv near row %u", rows ? rows : 1);
        }
        return false;
    }
    *corrections = table;
    *count = entries;
    return true;
}

void Linker_FreeReflectionProbes(LinkerBspReflectionProbes *probes)
{
    if (probes)
    {
        for (unsigned int i = 0; probes->probes && i < probes->count; ++i)
        {
            Linker_FreeImage(probes->probes[i].reflectionImage);
        }
        free(probes->probes);
        free(probes);
    }
}

static bool CorrectProbePixel(const ColorCorrectionData *correction, const uint8_t *source, uint8_t *dest)
{
    float color[3];
    for (int a = 0; a < 3; ++a)
    {
        const float level = (source[a] / 255.0f - correction->black_level) / correction->range;
        color[a] = (float)pow(fmaxf(0, level), correction->gamma);
        if (!isfinite(color[a]))
        {
            return false;
        }
    }
    const float intensity = color[0] * 0.114f + color[1] * 0.587f + color[2] * 0.299f;
    float maximum = 0.1f;
    for (int a = 0; a < 3; ++a)
    {
        color[a] = correction->saturation * color[a] + (1 - correction->saturation) * intensity;
        if (!isfinite(color[a]))
        {
            return false;
        }
        color[a] = fminf(4, color[a]);
        maximum = fmaxf(maximum, color[a]);
    }
    for (int a = 0; a < 3; ++a)
    {
        dest[a] = (uint8_t)(fminf(1, fmaxf(0, color[a] / maximum)) * 255);
    }
    dest[3] = (uint8_t)(maximum / 4 * 255);
    return true;
}

bool Linker_ImportReflectionProbes(const void *bsp, size_t size, const char *mapName,
                                   const ColorCorrectionData *corrections, unsigned int correctionCount,
                                   LinkerBspReflectionProbes **probes, char *error, size_t errorSize)
{
    if (!probes || (!error && errorSize))
    {
        return false;
    }
    *probes = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    char normalized[DB64_PACKAGE_PATH];
    bool valid = Validate(&input) && mapName && DB64_NormalizePath(mapName, normalized, sizeof(normalized)) &&
                 correctionCount <= 1024 && (!correctionCount || corrections);
    for (unsigned int i = 0; valid && i < correctionCount; ++i)
    {
        const ColorCorrectionData *correction = &corrections[i];
        valid = correction->name[0] && memchr(correction->name, 0, sizeof(correction->name)) &&
                isfinite(correction->black_level) && isfinite(correction->white_level) &&
                correction->white_level > correction->black_level && isfinite(correction->gamma) &&
                correction->gamma > 0 && isfinite(correction->saturation);
    }
    size_t count = 0;
    const uint8_t *disk =
        valid ? (const uint8_t *)Lump(&input, LUMP_REFLECTION_PROBES, sizeof(DiskGfxReflectionProbe), &count, true)
              : NULL;
    valid = valid && disk && count < 256;
    LinkerBspReflectionProbes *result =
        valid ? (LinkerBspReflectionProbes *)calloc(1, sizeof(LinkerBspReflectionProbes)) : NULL;
    uint8_t *pixels = valid ? (uint8_t *)malloc(131064) : NULL;
    valid = valid && result && pixels;
    if (valid)
    {
        result->count = (unsigned int)count + 1;
        result->probes = (GfxReflectionProbe *)calloc(result->count, sizeof(GfxReflectionProbe));
        valid = result->probes != NULL;
    }
    for (unsigned int i = 0; valid && i <= count; ++i)
    {
        const uint8_t *source = i ? disk + (i - 1) * sizeof(DiskGfxReflectionProbe) : NULL;
        const char *correctionName = source ? (const char *)(source + 12) : "default";
        if (source && !memchr(correctionName, 0, 64))
        {
            valid = false;
            break;
        }
        if (!correctionName[0])
        {
            correctionName = "default";
        }
        ColorCorrectionData correction = {};
        correction.white_level = correction.gamma = correction.saturation = correction.range = 1;
        if (correctionCount)
        {
            correction = corrections[0]; // Matches the engine's first-entry fallback for an unknown name.
            for (unsigned int j = 0; j < correctionCount; ++j)
            {
                if (!_stricmp(correctionName, corrections[j].name))
                {
                    correction = corrections[j];
                    break;
                }
            }
            correction.range = correction.white_level - correction.black_level;
        }
        if (!isfinite(correction.range) || correction.range <= 0)
        {
            valid = false;
            break;
        }
        if (source)
        {
            memcpy(result->probes[i].origin, source, sizeof(float[3]));
        }
        for (int a = 0; valid && a < 3; ++a)
        {
            valid = isfinite(result->probes[i].origin[a]);
        }
        const uint8_t defaultPixel[4] = {0, 0, 255, 255};
        const size_t faceBytes = 21844;
        // Disk order is six base faces, then all smaller mips for each face.
        // Native image loading consumes each complete face consecutively.
        for (unsigned int face = 0; valid && face < 6; ++face)
        {
            size_t output = face * faceBytes;
            size_t inputOffset = face * 16384;
            for (unsigned int mip = 0; valid && mip < 7; ++mip)
            {
                const unsigned int side = 64 >> mip;
                const size_t bytes = side * side * 4;
                for (size_t pixel = 0; valid && pixel < bytes; pixel += 4)
                {
                    const uint8_t *from = source ? source + 76 + inputOffset + pixel : defaultPixel;
                    valid = CorrectProbePixel(&correction, from, pixels + output + pixel);
                }
                output += bytes;
                inputOffset = mip ? inputOffset + bytes : 6 * 16384 + face * (faceBytes - 16384);
            }
        }
        char name[DB64_PACKAGE_PATH];
        const int length = snprintf(name, sizeof(name), "reflections/%s/%u", normalized, i);
        valid = valid && length > 0 && (size_t)length < sizeof(name);
        if (valid)
        {
            valid = Linker_CreateImage(name, 64, 64, D3DFMT_A8R8G8B8, IMG_FLAG_NOPICMIP | IMG_FLAG_CUBEMAP,
                                       IMG_CATEGORY_AUTO_GENERATED, TS_FUNCTION, 0, pixels, 131064,
                                       &result->probes[i].reflectionImage, error, errorSize);
        }
    }
    free(pixels);
    if (!valid)
    {
        Linker_FreeReflectionProbes(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Invalid BSP reflection probe or color correction data");
        }
        return false;
    }
    *probes = result;
    return true;
}

void Linker_FreeLightRegions(LinkerBspLightRegions *regions)
{
    if (regions)
    {
        free(regions->regions);
        free(regions->hulls);
        free(regions->axes);
        free(regions);
    }
}

bool Linker_ImportLightRegions(const void *bsp, size_t size, unsigned int primaryLightCount,
                               LinkerBspLightRegions **regions, char *error, size_t errorSize)
{
    if (!regions || (!error && errorSize))
    {
        return false;
    }
    *regions = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    bool valid = Validate(&input) && primaryLightCount && primaryLightCount <= 255;
    size_t regionCount = 0, hullCount = 0, axisCount = 0;
    const uint8_t *diskRegions = valid ? (const uint8_t *)Lump(&input, LUMP_LIGHTREGIONS, 1, &regionCount, true) : NULL;
    const uint8_t *diskHulls =
        valid ? (const uint8_t *)Lump(&input, LUMP_LIGHTREGION_HULLS, 76, &hullCount, true) : NULL;
    const uint8_t *diskAxes =
        valid ? (const uint8_t *)Lump(&input, LUMP_LIGHTREGION_AXES, sizeof(GfxLightRegionAxis), &axisCount, true)
              : NULL;
    valid = valid && diskRegions && diskHulls && diskAxes &&
            (regionCount ? regionCount == primaryLightCount : !hullCount && !axisCount);
    LinkerBspLightRegions *result = valid ? (LinkerBspLightRegions *)calloc(1, sizeof(LinkerBspLightRegions)) : NULL;
    valid = valid && result;
    if (valid)
    {
        result->regionCount = primaryLightCount;
        result->hullCount = (unsigned int)hullCount;
        result->axisCount = (unsigned int)axisCount;
        result->present = regionCount != 0;
        result->regions = (GfxLightRegion *)calloc(primaryLightCount, sizeof(GfxLightRegion));
        result->hulls = hullCount ? (GfxLightRegionHull *)calloc(hullCount, sizeof(GfxLightRegionHull)) : NULL;
        result->axes = axisCount ? (GfxLightRegionAxis *)malloc(axisCount * sizeof(GfxLightRegionAxis)) : NULL;
        valid = result->regions && (!hullCount || result->hulls) && (!axisCount || result->axes);
    }
    size_t usedHulls = 0, usedAxes = 0;
    for (unsigned int i = 0; valid && i < regionCount; ++i)
    {
        const unsigned int count = diskRegions[i];
        valid = count <= hullCount - usedHulls;
        if (valid)
        {
            result->regions[i].hullCount = count;
            result->regions[i].hulls = count ? result->hulls + usedHulls : NULL;
            usedHulls += count;
        }
    }
    valid = valid && usedHulls == hullCount;
    for (unsigned int i = 0; valid && i < hullCount; ++i)
    {
        GfxLightRegionHull *hull = &result->hulls[i];
        const uint8_t *source = diskHulls + i * 76;
        memcpy(hull->kdopMidPoint, source, sizeof(float[9]));
        memcpy(hull->kdopHalfSize, source + 36, sizeof(float[9]));
        memcpy(&hull->axisCount, source + 72, sizeof(uint32_t));
        valid = hull->axisCount <= axisCount - usedAxes;
        for (int a = 0; valid && a < 9; ++a)
        {
            valid = isfinite(hull->kdopMidPoint[a]) && isfinite(hull->kdopHalfSize[a]) && hull->kdopHalfSize[a] >= 0;
        }
        if (valid)
        {
            hull->axis = hull->axisCount ? result->axes + usedAxes : NULL;
            usedAxes += hull->axisCount;
        }
    }
    valid = valid && usedAxes == axisCount;
    for (unsigned int i = 0; valid && i < axisCount; ++i)
    {
        GfxLightRegionAxis *axis = &result->axes[i];
        memcpy(axis, diskAxes + i * sizeof(GfxLightRegionAxis), sizeof(GfxLightRegionAxis));
        valid = isfinite(axis->midPoint) && isfinite(axis->halfSize) && axis->halfSize >= 0;
        double length = 0;
        for (int a = 0; valid && a < 3; ++a)
        {
            valid = isfinite(axis->dir[a]);
            length += (double)axis->dir[a] * axis->dir[a];
        }
        valid = valid && length > 0;
    }
    if (!valid)
    {
        Linker_FreeLightRegions(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Invalid BSP light region, hull or axis data");
        }
        return false;
    }
    *regions = result;
    return true;
}

void Linker_FreeLightGrid(GfxLightGrid *grid)
{
    if (grid)
    {
        free(grid->rowDataStart);
        free(grid->rawRowData);
        free(grid->entries);
        free(grid->colors);
        free(grid);
    }
}

static void DefaultGridColors(GfxLightGridColors *colors)
{
    const float axes[3][3] = {{0.47140452f, 0, 0.33333334f},
                              {-0.23570226f, 0.40824828f, 0.33333334f},
                              {-0.23570226f, -0.40824828f, 0.33333334f}};
    unsigned int sample = 0;
    for (int z = 0; z < 4; ++z)
    {
        for (int y = 0; y < 4; ++y)
        {
            for (int x = 0; x < 4; ++x)
            {
                if (x != 0 && x != 3 && y != 0 && y != 3 && z != 0 && z != 3)
                {
                    continue;
                }
                const float delta[3] = {x * (2.0f / 3.0f) - 1, y * (2.0f / 3.0f) - 1, z * (2.0f / 3.0f) - 1};
                float rotated[3];
                float length = 0;
                for (int a = 0; a < 3; ++a)
                {
                    rotated[a] = delta[0] * axes[a][0] + delta[1] * axes[a][1] + delta[2] * axes[a][2];
                    length = fmaxf(length, fabsf(rotated[a]));
                }
                for (int a = 0; a < 3; ++a)
                {
                    const float value = (rotated[a] / length * 0.5f + 0.5f) * 255;
                    colors->rgb[sample][a] = (uint8_t)fminf(255, fmaxf(0, value));
                }
                ++sample;
            }
        }
    }
}

static bool ValidateGridRows(BspInput *input, const GfxLightGrid *grid, unsigned int rowCount)
{
    for (unsigned int i = 0; i < rowCount; ++i)
    {
        if (grid->rowDataStart[i] == UINT16_MAX)
        {
            continue;
        }
        size_t offset = (size_t)grid->rowDataStart[i] * 4;
        if (offset > grid->rawRowDataSize || sizeof(GfxLightGridRow) > grid->rawRowDataSize - offset)
        {
            return Fail(input, "Light grid row header is outside row data");
        }
        GfxLightGridRow row;
        memcpy(&row, grid->rawRowData + offset, sizeof(row));
        offset += sizeof(row);
        if (!row.colCount || !row.zCount || row.colStart < grid->mins[grid->colAxis] ||
            (unsigned int)row.colStart + row.colCount > (unsigned int)grid->maxs[grid->colAxis] + 1 ||
            row.zStart < grid->mins[2] || (unsigned int)row.zStart + row.zCount > (unsigned int)grid->maxs[2] + 1 ||
            row.firstEntry >= grid->entryCount)
        {
            return Fail(input, "Invalid light grid row bounds or first entry");
        }
        unsigned int columns = 0;
        size_t entry = row.firstEntry;
        while (columns < row.colCount)
        {
            if (offset > grid->rawRowDataSize || grid->rawRowDataSize - offset < 2)
            {
                return Fail(input, "Truncated light grid run");
            }
            const unsigned int count = grid->rawRowData[offset];
            const unsigned int height = grid->rawRowData[offset + 1];
            const unsigned int runSize = height ? 3 + (row.zCount > 255) : 2;
            if (!count || count > row.colCount - columns || runSize > grid->rawRowDataSize - offset)
            {
                return Fail(input, "Invalid light grid run length");
            }
            if (height)
            {
                unsigned int base = grid->rawRowData[offset + 2];
                if (row.zCount > 255)
                {
                    base |= (unsigned int)grid->rawRowData[offset + 3] << 8;
                }
                if (base + height > row.zCount || (size_t)count * height > grid->entryCount - entry)
                {
                    return Fail(input, "Light grid run exceeds its height or entry range");
                }
                entry += count * height;
            }
            columns += count;
            offset += runSize;
        }
    }
    return true;
}

bool Linker_ImportLightGrid(const void *bsp, size_t size, unsigned int sunPrimaryLightIndex,
                            unsigned int primaryLightCount, GfxLightGrid **grid, char *error, size_t errorSize)
{
    if (!grid || (!error && errorSize))
    {
        return false;
    }
    *grid = NULL;
    if (errorSize)
    {
        error[0] = 0;
    }
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    bool valid =
        Validate(&input) && primaryLightCount && primaryLightCount <= 255 && sunPrimaryLightIndex < primaryLightCount;
    size_t headerSize = 0, rowSize = 0, entryCount = 0, colorCount = 0;
    const uint8_t *header = valid ? (const uint8_t *)Lump(&input, LUMP_LIGHTGRIDHEADER, 1, &headerSize) : NULL;
    const void *rows = valid ? Lump(&input, LUMP_LIGHTGRIDROWS, 1, &rowSize, true) : NULL;
    const void *entries =
        valid ? Lump(&input, LUMP_LIGHTGRIDENTRIES, sizeof(GfxLightGridEntry), &entryCount, true) : NULL;
    const void *colors =
        valid ? Lump(&input, LUMP_LIGHTGRIDCOLORS, sizeof(GfxLightGridColors), &colorCount, true) : NULL;
    valid = valid && header && headerSize >= 20 && rows && entries && colors && colorCount < 65536 && rowSize <= 262144;
    GfxLightGrid *result = valid ? (GfxLightGrid *)calloc(1, sizeof(GfxLightGrid)) : NULL;
    valid = valid && result;
    unsigned int rowCount = 0;
    if (valid)
    {
        memcpy(result->mins, header, sizeof(uint16_t[3]));
        memcpy(result->maxs, header + 6, sizeof(uint16_t[3]));
        memcpy(&result->rowAxis, header + 12, sizeof(uint32_t));
        memcpy(&result->colAxis, header + 16, sizeof(uint32_t));
        valid = result->rowAxis < 2 && result->colAxis < 2 && result->rowAxis != result->colAxis;
        for (int a = 0; valid && a < 3; ++a)
        {
            valid = result->mins[a] <= result->maxs[a] && result->maxs[a] < (a == 2 ? 4096 : 8192);
        }
        if (valid)
        {
            rowCount = result->maxs[result->rowAxis] - result->mins[result->rowAxis] + 1;
            valid = headerSize == 20 + rowCount * sizeof(uint16_t);
        }
    }
    if (valid)
    {
        result->sunPrimaryLightIndex = sunPrimaryLightIndex;
        result->rawRowDataSize = (unsigned int)rowSize;
        result->entryCount = (unsigned int)entryCount;
        result->colorCount = (unsigned int)colorCount + 1;
        result->rowDataStart = (uint16_t *)malloc(rowCount * sizeof(uint16_t));
        result->rawRowData = rowSize ? (uint8_t *)malloc(rowSize) : NULL;
        result->entries = entryCount ? (GfxLightGridEntry *)malloc(entryCount * sizeof(GfxLightGridEntry)) : NULL;
        result->colors = (GfxLightGridColors *)malloc(result->colorCount * sizeof(GfxLightGridColors));
        valid = result->rowDataStart && (!rowSize || result->rawRowData) && (!entryCount || result->entries) &&
                result->colors;
    }
    if (valid)
    {
        memcpy(result->rowDataStart, header + 20, rowCount * sizeof(uint16_t));
        if (rowSize)
        {
            memcpy(result->rawRowData, rows, rowSize);
        }
        if (entryCount)
        {
            // colorsIndex is a full little-endian uint16_t, not the first byte of the disk entry.
            memcpy(result->entries, entries, entryCount * sizeof(GfxLightGridEntry));
        }
        memcpy(result->colors, colors, colorCount * sizeof(GfxLightGridColors));
        DefaultGridColors(&result->colors[colorCount]);
        for (unsigned int i = 0; valid && i < entryCount; ++i)
        {
            valid = result->entries[i].colorsIndex < colorCount &&
                    (result->entries[i].primaryLightIndex == 255 ||
                     result->entries[i].primaryLightIndex < primaryLightCount);
        }
        valid = valid && ValidateGridRows(&input, result, rowCount);
    }
    if (!valid)
    {
        Linker_FreeLightGrid(result);
        if (errorSize && !error[0])
        {
            snprintf(error, errorSize, "Invalid BSP light grid or allocation failure");
        }
        return false;
    }
    *grid = result;
    return true;
}

// BSP writers emit quoted key/value pairs. Decode the two escapes also handled
// by Com_Parse, while preserving the original source spans in the final asset.
static bool SkipSpace(BspInput *input, const char **cursor, const char *end)
{
    const char *p = *cursor;
    for (;;)
    {
        while (p < end && (unsigned char)*p <= ' ')
        {
            ++p;
        }
        if (end - p >= 2 && p[0] == '/' && p[1] == '/')
        {
            p += 2;
            while (p < end && *p != '\n')
            {
                ++p;
            }
        }
        else if (end - p >= 2 && p[0] == '/' && p[1] == '*')
        {
            p += 2;
            while (end - p >= 2 && !(p[0] == '*' && p[1] == '/'))
            {
                ++p;
            }
            if (end - p < 2)
            {
                return Fail(input, "Unterminated BSP entity comment");
            }
            p += 2;
        }
        else
        {
            *cursor = p;
            return true;
        }
    }
}

static bool Quoted(BspInput *input, const char **cursor, const char *end, char *token, size_t capacity)
{
    if (!SkipSpace(input, cursor, end))
    {
        return false;
    }
    const char *p = *cursor;
    if (p == end || *p++ != '"')
    {
        return Fail(input, "Expected a quoted BSP entity key/value");
    }
    size_t size = 0;
    while (p < end)
    {
        char c = *p++;
        if (c == '"')
        {
            token[size] = 0;
            *cursor = p;
            return true;
        }
        if (c == '\\' && p < end && (*p == '\\' || *p == '"'))
        {
            c = *p++;
        }
        if (size + 1 >= capacity)
        {
            return Fail(input, "BSP entity token exceeds the engine limit");
        }
        token[size++] = c;
    }
    return Fail(input, "Unterminated BSP entity string");
}

static bool SunNumbers(const char *text, float *values, unsigned int count)
{
    for (unsigned int i = 0; i < count; ++i)
    {
        char *end;
        values[i] = strtof(text, &end);
        if (end == text || !isfinite(values[i]))
        {
            return false;
        }
        text = end;
    }
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
    {
        ++text;
    }
    return !*text;
}

static bool NormalizeSunColor(float *color)
{
    float maximum = color[0];
    for (unsigned int i = 1; i < 3; ++i)
    {
        if (maximum < color[i])
        {
            maximum = color[i];
        }
    }
    for (unsigned int i = 0; i < 3; ++i)
    {
        color[i] = maximum == 0 ? 1 : color[i] / maximum;
        if (!isfinite(color[i]))
        {
            return false;
        }
    }
    return true;
}

bool Linker_ImportSunSettings(const void *bsp, size_t size, SunLightParseParams *params, GfxLight *light, char *error,
                              size_t errorSize)
{
    if (!params || !light || (!error && errorSize))
    {
        return false;
    }
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    if (!Validate(&input))
    {
        return false;
    }
    size_t length = 0;
    const char *text = (const char *)Lump(&input, LUMP_ENTITIES, sizeof(char), &length);
    if (!text || !length || text[length - 1] || memchr(text, 0, length - 1))
    {
        return Fail(&input, "Invalid sun settings entity string");
    }
    const char *end = text + length - 1;
    if (!SkipSpace(&input, &text, end) || text == end || *text++ != '{')
    {
        return Fail(&input, "Missing worldspawn for sun settings");
    }
    SunLightParseParams result = {};
    result.diffuseFraction = 0.5f;
    for (;;)
    {
        if (!SkipSpace(&input, &text, end) || text == end)
        {
            return Fail(&input, "Unterminated worldspawn sun settings");
        }
        if (*text == '}')
        {
            break;
        }
        char key[2048], value[2048];
        if (!Quoted(&input, &text, end, key, sizeof(key)) || !Quoted(&input, &text, end, value, sizeof(value)))
        {
            return false;
        }
        bool valid = true;
        if (!_stricmp(key, "ambient"))
        {
            valid = SunNumbers(value, &result.ambientScale, 1);
            if (result.ambientScale > 2)
            {
                result.ambientScale *= 4.0f / 255.0f;
            }
        }
        else if (!_stricmp(key, "_color"))
        {
            valid = SunNumbers(value, result.ambientColor, 3);
        }
        else if (!_stricmp(key, "diffuseFraction"))
        {
            valid = SunNumbers(value, &result.diffuseFraction, 1);
        }
        else if (!_stricmp(key, "sunlight"))
        {
            valid = SunNumbers(value, &result.sunLight, 1);
        }
        else if (!_stricmp(key, "sundirection"))
        {
            valid = SunNumbers(value, result.angles, 3);
        }
        else if (!_stricmp(key, "suncolor"))
        {
            valid = SunNumbers(value, result.sunColor, 3) && NormalizeSunColor(result.sunColor);
        }
        else if (!_stricmp(key, "sundiffusecolor"))
        {
            valid = SunNumbers(value, result.diffuseColor, 3) && NormalizeSunColor(result.diffuseColor);
            result.diffuseColorHasBeenSet = true;
        }
        else if (!_stricmp(key, "name"))
        {
            strncpy(result.name, value, sizeof(result.name) - 1);
        }
        if (!valid)
        {
            return Fail(&input, "Invalid numeric worldspawn sun setting");
        }
    }
    GfxLight sun = {};
    sun.type = 1;
    const double radians = 0.017453292519943295;
    const double pitch = result.angles[0] * radians, yaw = result.angles[1] * radians;
    sun.dir[0] = (float)(cos(pitch) * cos(yaw));
    sun.dir[1] = (float)(cos(pitch) * sin(yaw));
    sun.dir[2] = (float)-sin(pitch);
    const double scale = (1.0 - result.diffuseFraction) * ((double)result.sunLight - result.ambientScale);
    for (unsigned int i = 0; i < 3; ++i)
    {
        sun.color[i] = (float)(scale * result.sunColor[i]);
        if (!isfinite(sun.color[i]))
        {
            return Fail(&input, "Worldspawn sun intensity exceeds the renderer's numeric range");
        }
    }
    *params = result;
    *light = sun;
    return true;
}

static int GroundHex(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

bool Linker_ImportStaticModelPlacements(const void *bsp, size_t size, unsigned int sunIndex,
                                        unsigned int primaryLightCount, LinkerBspStaticModel **placements,
                                        unsigned int *count, char *error, size_t errorSize)
{
    if (!placements || !count || (!error && errorSize))
    {
        return false;
    }
    *placements = NULL;
    *count = 0;
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    if (!Validate(&input) || sunIndex > 1 || primaryLightCount <= sunIndex || primaryLightCount > 255)
    {
        return Fail(&input, "Invalid static-model placement input");
    }
    size_t length = 0;
    const char *text = (const char *)Lump(&input, LUMP_ENTITIES, sizeof(char), &length);
    if (!text || !length || text[length - 1] || memchr(text, 0, length - 1))
    {
        return Fail(&input, "Invalid static-model entity string");
    }
    const char *end = text + length - 1;
    LinkerBspStaticModel *result = NULL;
    unsigned int used = 0, capacity = 0, entity = 0;
    bool valid = true;
    const char *keys[] = {"classname", "model", "origin", "angles", "angle", "modelscale", "spawnflags", "gndLt"};
    while (valid)
    {
        valid = SkipSpace(&input, &text, end);
        if (!valid || text == end)
        {
            break;
        }
        if (*text++ != '{')
        {
            valid = false;
            break;
        }
        char values[8][1024] = {};
        bool present[8] = {};
        unsigned int pairs = 0;
        size_t bytes = 0;
        for (;;)
        {
            if (!SkipSpace(&input, &text, end) || text == end)
            {
                valid = false;
                break;
            }
            if (*text == '}')
            {
                ++text;
                break;
            }
            char key[1024], value[1024];
            if (!Quoted(&input, &text, end, key, sizeof(key)) || !Quoted(&input, &text, end, value, sizeof(value)))
            {
                valid = false;
                break;
            }
            bytes += strlen(key) + strlen(value) + 2;
            if (++pairs > 64 || bytes > 2048)
            {
                valid = false;
                break;
            }
            for (unsigned int i = 0; i < ARRAY_COUNT(keys); ++i)
            {
                if (!present[i] && !_stricmp(key, keys[i]))
                {
                    strcpy(values[i], value);
                    present[i] = true;
                }
            }
        }
        ++entity;
        valid = valid && present[0];
        if (!valid)
        {
            break;
        }
        if (_stricmp(values[0], "misc_model"))
        {
            continue;
        }
        LinkerBspStaticModel placement = {};
        placement.entityIndex = entity;
        placement.scale = 1;
        const char *name = values[1];
        if (!_strnicmp(name, "xmodel", 6) && (name[6] == '/' || name[6] == '\\'))
        {
            name += 7;
        }
        valid = present[1] && present[2] && DB64_NormalizePath(name, placement.model, sizeof(placement.model)) &&
                SunNumbers(values[2], placement.origin, 3);
        float yaw = 0;
        valid = valid && (!present[4] || SunNumbers(values[4], &yaw, 1));
        if (yaw != 0)
        {
            placement.angles[1] = yaw;
        }
        else
        {
            valid = valid && (!present[3] || SunNumbers(values[3], placement.angles, 3));
        }
        valid = valid && (!present[5] || SunNumbers(values[5], &placement.scale, 1)) && placement.scale != 0;
        if (valid && present[6])
        {
            char *tail;
            long long flags = strtoll(values[6], &tail, 10);
            const bool digits = tail != values[6];
            while (*tail == ' ' || *tail == '\t')
            {
                ++tail;
            }
            valid = digits && !*tail && flags >= INT_MIN && flags <= INT_MAX;
            placement.flags = (flags & 2) ? 1 : 0;
        }
        placement.hasGroundLighting = present[7];
        if (valid && present[7])
        {
            size_t digits = strlen(values[7]);
            valid = digits == 8 || digits == 10;
            uint8_t decoded[5] = {0, 0, 0, 0, (uint8_t)sunIndex};
            for (unsigned int i = 0; valid && i < digits / 2; ++i)
            {
                int high = GroundHex(values[7][2 * i]), low = GroundHex(values[7][2 * i + 1]);
                valid = high >= 0 && low >= 0;
                decoded[i] = (uint8_t)(high * 16 + low);
            }
            placement.groundLighting[0] = decoded[2];
            placement.groundLighting[1] = decoded[1];
            placement.groundLighting[2] = decoded[0];
            placement.groundLighting[3] = decoded[3];
            placement.primaryLightIndex = decoded[4];
            valid = valid && placement.primaryLightIndex < primaryLightCount;
        }
        if (valid && used == capacity)
        {
            unsigned int next = capacity ? capacity * 2 : 64;
            if (next > 65535)
            {
                next = 65535;
            }
            LinkerBspStaticModel *grown =
                next > capacity ? (LinkerBspStaticModel *)realloc(result, next * sizeof(LinkerBspStaticModel)) : NULL;
            valid = grown != NULL;
            if (valid)
            {
                result = grown;
                capacity = next;
            }
        }
        if (valid)
        {
            result[used++] = placement;
        }
    }
    if (!valid)
    {
        free(result);
        return Fail(&input, "Invalid static-model entity, lighting, or placement capacity");
    }
    *placements = result;
    *count = used;
    return true;
}

static bool Purge(const char *classname, bool primaryLight)
{
    // Same rule as MapEnts_CanPurgeEntity in cm_load_obj.cpp.
    const char *classes[] = {"misc_model",       "misc_prefab", "dyn_brushmodel", "dyn_model",
                             "reflection_probe", "info_null",   "func_group"};
    for (uint i = 0; i < ARRAY_COUNT(classes); ++i)
    {
        if (!_stricmp(classname, classes[i]))
        {
            return true;
        }
    }
    return !_stricmp(classname, "light") && !primaryLight;
}

static bool Entities(BspInput *input, LinkerMapWorlds *worlds)
{
    size_t size = 0;
    const char *source = (const char *)Lump(input, LUMP_ENTITIES, sizeof(char), &size);
    if (!source)
    {
        return false;
    }
    if (!size || source[size - 1] || memchr(source, 0, size - 1))
    {
        return Fail(input, "Invalid BSP entity string terminator");
    }
    char *output = (char *)malloc(size);
    if (!output)
    {
        return Fail(input, "Out of memory importing map entities");
    }
    worlds->entities.entityString = output;
    const char *p = source;
    const char *end = source + size - 1;
    size_t used = 0;
    unsigned int entityCount = 0;
    for (;;)
    {
        const char *begin = p;
        if (!SkipSpace(input, &p, end))
        {
            return false;
        }
        if (p == end)
        {
            break;
        }
        if (*p++ != '{')
        {
            return Fail(input, "Expected opening BSP entity brace");
        }
        char classname[1024] = {};
        bool foundClass = false;
        bool primaryLight = false;
        unsigned int pairs = 0;
        size_t tokenBytes = 0;
        for (;;)
        {
            if (!SkipSpace(input, &p, end))
            {
                return false;
            }
            if (p < end && *p == '}')
            {
                ++p;
                break;
            }
            char key[1024];
            char value[1024];
            if (!Quoted(input, &p, end, key, sizeof(key)) || !Quoted(input, &p, end, value, sizeof(value)))
            {
                return false;
            }
            tokenBytes += strlen(key) + strlen(value) + 2;
            if (++pairs > 64 || tokenBytes > 2048)
            {
                return Fail(input, "BSP entity exceeds engine spawn-variable limits");
            }
            if (!foundClass && !_stricmp(key, "classname"))
            {
                strcpy_s(classname, value);
                foundClass = true;
            }
            if (!_stricmp(key, "pl#"))
            {
                primaryLight = true;
            }
        }
        if (!entityCount && _stricmp(classname, "worldspawn"))
        {
            return Fail(input, "The first BSP entity must be worldspawn");
        }
        ++entityCount;
        if (!Purge(classname, primaryLight))
        {
            const size_t length = p - begin;
            memcpy(output + used, begin, length);
            used += length;
        }
    }
    if (!entityCount)
    {
        return Fail(input, "BSP contains no worldspawn entity");
    }
    output[used] = 0;
    worlds->entities.numEntityChars = (int)used + 1;
    return true;
}

static bool Lights(BspInput *input, LinkerMapWorlds *worlds)
{
    size_t count = 0;
    const uint8_t *source = (const uint8_t *)Lump(input, LUMP_PRIMARY_LIGHTS, sizeof(DiskPrimaryLight), &count);
    if (!source)
    {
        return false;
    }
    if (count < 2 || count > 255)
    {
        return Fail(input, "Invalid BSP primary light count");
    }
    worlds->common.primaryLights = (ComPrimaryLight *)calloc(count, sizeof(ComPrimaryLight));
    worlds->lightNames = (char *)calloc(count, sizeof(char[64]));
    if (!worlds->common.primaryLights || !worlds->lightNames)
    {
        return Fail(input, "Out of memory importing BSP lights");
    }
    worlds->common.primaryLightCount = (uint)count;
    worlds->common.isInUse = 1;
    for (size_t i = 0; i < count; ++i)
    {
        DiskPrimaryLight disk;
        memcpy(&disk, source + i * sizeof(DiskPrimaryLight), sizeof(DiskPrimaryLight));
        if (disk.exponent < 0 || disk.exponent > 255 || disk.type > 3 ||
            (disk.type > 1 && !memchr(disk.defName, 0, sizeof(disk.defName))) || (i == 0 && disk.type != 0) ||
            (i == 1 && disk.type != 1))
        {
            return Fail(input, "Invalid BSP light type, exponent or definition name");
        }
        const float *scalars[] = {disk.color,
                                  disk.dir,
                                  disk.origin,
                                  &disk.radius,
                                  &disk.cosHalfFovOuter,
                                  &disk.cosHalfFovInner,
                                  &disk.rotationLimit,
                                  &disk.translationLimit};
        for (uint field = 0; field < ARRAY_COUNT(scalars); ++field)
        {
            for (uint component = 0; component < (field < 3 ? 3 : 1); ++component)
            {
                if (!_finite(scalars[field][component]))
                {
                    return Fail(input, "Non-finite BSP light value");
                }
            }
        }
        if (fabsf(disk.cosHalfFovOuter) > 1 || fabsf(disk.cosHalfFovInner) > 1 || fabsf(disk.rotationLimit) > 1)
        {
            return Fail(input, "BSP light cosine is outside [-1, 1]");
        }
        ComPrimaryLight *light = &worlds->common.primaryLights[i];
        light->type = disk.type;
        light->canUseShadowMap = disk.canUseShadowMap;
        light->exponent = (uint8_t)disk.exponent;
        memcpy(light->color, disk.color, sizeof(float[3]));
        memcpy(light->dir, disk.dir, sizeof(float[3]));
        memcpy(light->origin, disk.origin, sizeof(float[3]));
        light->radius = disk.radius;
        light->cosHalfFovOuter = disk.cosHalfFovOuter;
        light->cosHalfFovInner = disk.cosHalfFovInner;
        light->rotationLimit = disk.rotationLimit;
        light->translationLimit = disk.translationLimit;
        light->cosHalfFovExpanded = disk.cosHalfFovOuter;
        if (disk.type != 0 && disk.type != 1)
        {
            char *name = worlds->lightNames + i * sizeof(disk.defName);
            memcpy(name, disk.defName, sizeof(disk.defName));
            light->defName = name;
            if (light->cosHalfFovOuter >= light->cosHalfFovInner)
            {
                light->cosHalfFovInner = (float)(light->cosHalfFovOuter * 0.75 + 0.25);
            }
            if (light->rotationLimit != 1)
            {
                if (light->rotationLimit > -light->cosHalfFovOuter)
                {
                    const float sinSq0 = 1 - light->cosHalfFovOuter * light->cosHalfFovOuter;
                    const float sinSq1 = 1 - light->rotationLimit * light->rotationLimit;
                    light->cosHalfFovExpanded = light->cosHalfFovOuter * light->rotationLimit - sqrtf(sinSq0 * sinSq1);
                }
                else
                {
                    light->cosHalfFovExpanded = -1;
                }
            }
        }
    }
    return true;
}

void Linker_FreeMapWorlds(LinkerMapWorlds *worlds)
{
    if (worlds)
    {
        free(worlds->entities.entityString);
        free(worlds->common.primaryLights);
        free(worlds->lightNames);
        free(worlds->name);
        free(worlds);
    }
}

bool Linker_ImportMapWorlds(const void *bsp, size_t size, const char *assetName, LinkerMapWorlds **worlds, char *error,
                            size_t errorSize)
{
    if (!worlds || (!error && errorSize))
    {
        return false;
    }
    *worlds = NULL;
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    if (errorSize)
    {
        error[0] = 0;
    }
    char name[DB64_PACKAGE_PATH];
    if (!assetName || !DB64_NormalizePath(assetName, name, sizeof(name)) || strncmp(name, "maps/mp/", 8))
    {
        return Fail(&input, "Expected a multiplayer BSP asset name under maps/mp/");
    }
    const char *extension = strrchr(name, '.');
    if (!extension || strcmp(extension, ".d3dbsp"))
    {
        return Fail(&input, "Invalid BSP asset name extension");
    }
    char map[DB64_PACKAGE_PATH];
    strcpy_s(map, name + 8);
    map[extension - (name + 8)] = 0;
    if (!DB64_ValidMapName(map))
    {
        return Fail(&input, "Invalid BSP map name");
    }
    if (!Validate(&input))
    {
        return false;
    }
    LinkerMapWorlds *result = (LinkerMapWorlds *)calloc(1, sizeof(LinkerMapWorlds));
    if (!result)
    {
        return Fail(&input, "Out of memory allocating native map worlds");
    }
    result->name = _strdup(name);
    if (!result->name)
    {
        Linker_FreeMapWorlds(result);
        return Fail(&input, "Out of memory copying map name");
    }
    result->entities.name = result->name;
    result->common.name = result->name;
    result->multiplayer.name = result->name;
    if (!Entities(&input, result) || !Lights(&input, result))
    {
        Linker_FreeMapWorlds(result);
        return false;
    }
    *worlds = result;
    return true;
}

struct LinkerDiskPartition
{
    uint16_t checkStamp;
    uint8_t triCount;
    uint8_t borderCount;
    int32_t firstTriIndex;
    int32_t firstBorderIndex;
};
static_assert(sizeof(LinkerDiskPartition) == 12);

bool Linker_ImportDynamicEntityParams(const void *bsp, size_t size, DynEntityCreateParams **entities,
                                      unsigned int *count, char *error, size_t errorSize)
{
    if (!entities || !count || (!error && errorSize))
    {
        return false;
    }
    *entities = NULL;
    *count = 0;
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    if (!Validate(&input))
    {
        return false;
    }
    size_t length = 0;
    const char *text = (const char *)Lump(&input, LUMP_ENTITIES, sizeof(char), &length);
    if (!text || !length || text[length - 1] || memchr(text, 0, length - 1))
    {
        return Fail(&input, "Invalid dynamic-entity string");
    }
    const char *end = text + length - 1;
    const char *keys[] = {"classname", "type", "model", "physicsmodel", "destroyEfx", "destroyPieces", "physPreset",
                          "origin", "angles", "health", "centerofmass", "momofinertia", "prodofinertia"};
    DynEntityCreateParams *result = NULL;
    unsigned int used = 0;
    bool valid = true;
    while (valid)
    {
        valid = SkipSpace(&input, &text, end);
        if (!valid || text == end)
        {
            break;
        }
        if (*text++ != '{')
        {
            valid = false;
            break;
        }
        char values[13][1024] = {};
        bool closed = false;
        while (valid && SkipSpace(&input, &text, end) && text != end)
        {
            if (*text == '}')
            {
                ++text;
                closed = true;
                break;
            }
            char key[1024], value[1024];
            valid = Quoted(&input, &text, end, key, sizeof(key)) && Quoted(&input, &text, end, value, sizeof(value));
            for (unsigned int k = 0; valid && k < ARRAY_COUNT(keys); ++k)
            {
                if (!_stricmp(key, keys[k]))
                {
                    strcpy(values[k], value);
                }
            }
        }
        valid = valid && closed;
        if (!valid || (_stricmp(values[0], "dyn_model") && _stricmp(values[0], "dyn_brushmodel")))
        {
            continue;
        }
        DynEntityCreateParams item = {};
        char *names[] = {item.typeName, item.modelName, item.physModelName, item.destroyFxFile,
                         item.destroyPiecesFile, item.physPresetFile};
        for (unsigned int n = 0; valid && n < ARRAY_COUNT(names); ++n)
        {
            const char *value = values[n + 1];
            if ((n == 1 || n == 2) && !_strnicmp(value, "xmodel", 6) && (value[6] == '/' || value[6] == '\\'))
            {
                value += 7;
            }
            valid = strlen(value) < sizeof(item.modelName);
            if (valid)
            {
                strcpy(names[n], value);
                if (n == 3 || n == 4)
                {
                    char *extension = strrchr(names[n], '.');
                    const char *slash = strrchr(names[n], '/');
                    if (extension && (!slash || extension > slash))
                    {
                        *extension = 0;
                    }
                }
            }
        }
        float *vectors[] = {item.origin, item.angles, item.centerOfMass, item.momentsOfInertia, item.productsOfInertia};
        const unsigned int indices[] = {7, 8, 10, 11, 12};
        for (unsigned int v = 0; valid && v < ARRAY_COUNT(vectors); ++v)
        {
            valid = !values[indices[v]][0] || SunNumbers(values[indices[v]], vectors[v], 3);
        }
        if (valid && values[9][0])
        {
            char *tail;
            const long long health = strtoll(values[9], &tail, 10);
            valid = !*tail && health >= INT_MIN && health <= INT_MAX;
            item.health = (int)health;
        }
        valid = valid && item.modelName[0] && (!item.typeName[0] || !_stricmp(item.typeName, "clutter") ||
                                              !_stricmp(item.typeName, "destruct")) && used < 131070;
        if (valid)
        {
            DynEntityCreateParams *next = (DynEntityCreateParams *)realloc(result, (used + 1) * sizeof(DynEntityCreateParams));
            valid = next != NULL;
            if (valid)
            {
                result = next;
                result[used++] = item;
            }
        }
    }
    valid = valid && text == end;
    if (!valid)
    {
        free(result);
        return Fail(&input, "Invalid dynamic-entity fields or allocation failure");
    }
    *entities = result;
    *count = used;
    return true;
}

void Linker_FreeCollisionGeometry(clipMap_t *map)
{
    if (map)
    {
        free(map->staticModelList);
        for (unsigned int group = 0; group < 2; ++group)
        {
            free(map->dynEntDefList[group]);
            free(map->dynEntPoseList[group]);
            free(map->dynEntClientList[group]);
            free(map->dynEntCollList[group]);
        }
        free(map->verts);
        free(map->triIndices);
        free(map->triEdgeIsWalkable);
        free(map->borders);
        free(map->partitions);
        free(map->planes);
        free(map->materials);
        free(map->brushsides);
        free(map->brushEdges);
        free(map->brushes);
        free(map->aabbTrees);
        free(map->nodes);
        free(map->leafs);
        free(map->leafbrushes);
        free(map->leafbrushNodes);
        free(map->leafsurfaces);
        free(map->cmodels);
        free(map->box_brush);
        free(map->visibility);
        free(map);
    }
}

static const void *CollisionLump(BspInput *input, LumpType type, size_t stride, size_t *count)
{
    for (uint i = 0; i < input->count; ++i)
    {
        if (input->chunks[i].type == type)
        {
            return Lump(input, type, stride, count);
        }
    }
    // Brush-only maps omit empty collision triangle lumps.
    *count = 0;
    return input->data;
}

static bool CopyCollisionLump(BspInput *input, LumpType type, size_t stride, void **out, int *count)
{
    size_t elements = 0;
    const void *source = CollisionLump(input, type, stride, &elements);
    if (!source || elements > INT_MAX / stride)
    {
        return false;
    }
    *count = (int)elements;
    if (!elements)
    {
        *out = NULL;
        return true;
    }
    *out = malloc(elements * stride);
    if (!*out)
    {
        return Fail(input, "Out of memory importing collision geometry");
    }
    memcpy(*out, source, elements * stride);
    return true;
}

struct LinkerDiskBrush
{
    int16_t sideCount;
    int16_t material;
};
struct LinkerDiskBrushSide
{
    union {
        int32_t plane;
        float bound;
    } value;
    int32_t material;
};
static_assert(sizeof(LinkerDiskBrush) == 4 && sizeof(LinkerDiskBrushSide) == 8);

static bool CollisionBrushes(BspInput *input, clipMap_t *map)
{
    int materials;
    if (!CopyCollisionLump(input, LUMP_MATERIALS, sizeof(dmaterial_t), (void **)&map->materials, &materials) ||
        !materials)
    {
        return Fail(input, "BSP has no valid collision materials");
    }
    map->numMaterials = materials;
    for (int i = 0; i < materials; ++i)
    {
        if (!memchr(map->materials[i].material, 0, sizeof(map->materials[i].material)))
        {
            return Fail(input, "BSP collision material name is not terminated");
        }
        map->materials[i].contentFlags &= 0xDFFFFFFB;
    }
    size_t planeCount = 0;
    const float(*planes)[4] = (const float(*)[4])Lump(input, LUMP_PLANES, sizeof(float[4]), &planeCount);
    if (!planes || !planeCount || planeCount > 65536)
    {
        return Fail(input, "Invalid BSP collision plane count");
    }
    map->planeCount = (int)planeCount;
    map->planes = (cplane_s *)calloc(planeCount, sizeof(cplane_s));
    if (!map->planes)
    {
        return Fail(input, "Out of memory importing collision planes");
    }
    for (size_t i = 0; i < planeCount; ++i)
    {
        cplane_s *plane = &map->planes[i];
        float disk[4];
        memcpy(disk, planes[i], sizeof(disk));
        plane->type = 3;
        plane->dist = disk[3];
        for (int axis = 0; axis < 4; ++axis)
        {
            if (!isfinite(disk[axis]))
            {
                return Fail(input, "BSP collision plane is not finite");
            }
            if (axis < 3)
            {
                plane->normal[axis] = disk[axis];
                if (disk[axis] < 0)
                {
                    plane->signbits |= 1 << axis;
                }
                if (disk[axis] == 1 && plane->type == 3)
                {
                    plane->type = axis;
                }
            }
        }
    }
    size_t brushCount = 0, sideCount = 0, edgeCount = 0, countCount = 0;
    const LinkerDiskBrush *brushes =
        (const LinkerDiskBrush *)CollisionLump(input, LUMP_BRUSHES, sizeof(LinkerDiskBrush), &brushCount);
    const LinkerDiskBrushSide *sides =
        (const LinkerDiskBrushSide *)CollisionLump(input, LUMP_BRUSHSIDES, sizeof(LinkerDiskBrushSide), &sideCount);
    const uint8_t *counts = (const uint8_t *)CollisionLump(input, LUMP_BRUSHSIDEEDGECOUNTS, 1, &countCount);
    const uint8_t *edges = (const uint8_t *)CollisionLump(input, LUMP_BRUSHEDGES, 1, &edgeCount);
    if (!brushes || !sides || !counts || !edges || brushCount > 65535 || sideCount != countCount ||
        brushCount * 6 > sideCount || sideCount - brushCount * 6 > INT_MAX / sizeof(cbrushside_t))
    {
        return Fail(input, "Invalid BSP brush lump counts");
    }
    map->numBrushes = (uint16_t)brushCount;
    map->numBrushSides = (uint)(sideCount - brushCount * 6);
    map->numBrushEdges = (uint)edgeCount;
    map->brushes = brushCount ? (cbrush_t *)calloc(brushCount, sizeof(cbrush_t)) : NULL;
    map->brushsides = map->numBrushSides ? (cbrushside_t *)calloc(map->numBrushSides, sizeof(cbrushside_t)) : NULL;
    map->brushEdges = edgeCount ? (uint8_t *)malloc(edgeCount) : NULL;
    if ((brushCount && !map->brushes) || (map->numBrushSides && !map->brushsides) || (edgeCount && !map->brushEdges))
    {
        return Fail(input, "Out of memory importing collision brushes");
    }
    if (edgeCount)
    {
        memcpy(map->brushEdges, edges, edgeCount);
    }
    size_t diskSide = 0, outputSide = 0, edge = 0;
    for (size_t i = 0; i < brushCount; ++i)
    {
        const int sidesInBrush = brushes[i].sideCount;
        if (sidesInBrush < 6 || sidesInBrush > 256 || (size_t)sidesInBrush > sideCount - diskSide ||
            brushes[i].material < 0 || brushes[i].material >= materials ||
            (size_t)(sidesInBrush - 6) > map->numBrushSides - outputSide)
        {
            return Fail(input, "Invalid BSP brush side count or material");
        }
        cbrush_t *brush = &map->brushes[i];
        brush->numsides = sidesInBrush - 6;
        brush->sides = brush->numsides ? &map->brushsides[outputSide] : NULL;
        brush->contents = map->materials[brushes[i].material].contentFlags;
        const size_t firstEdge = edge;
        for (int j = 0; j < sidesInBrush; ++j, ++diskSide)
        {
            const LinkerDiskBrushSide *side = &sides[diskSide];
            const size_t offset = edge - firstEdge;
            if (side->material < 0 || side->material >= materials || offset > INT16_MAX ||
                counts[diskSide] > edgeCount - edge)
            {
                return Fail(input, "Invalid BSP brush adjacency or material");
            }
            for (unsigned int k = 0; k < counts[diskSide]; ++k)
            {
                if (edges[edge + k] >= sidesInBrush)
                {
                    return Fail(input, "BSP brush edge references an invalid side");
                }
            }
            if (j < 6)
            {
                const int axis = j / 2, direction = j % 2;
                if (!isfinite(side->value.bound) || side->material > INT16_MAX)
                {
                    return Fail(input, "Invalid BSP axial brush side");
                }
                if (direction)
                {
                    brush->maxs[axis] = side->value.bound;
                }
                else
                {
                    brush->mins[axis] = side->value.bound;
                }
                brush->axialMaterialNum[direction][axis] = side->material;
                brush->edgeCount[direction][axis] = counts[diskSide];
                brush->firstAdjacentSideOffsets[direction][axis] = (int16_t)offset;
            }
            else
            {
                if (side->value.plane < 0 || (size_t)side->value.plane >= planeCount ||
                    outputSide >= map->numBrushSides)
                {
                    return Fail(input, "Invalid BSP brush plane reference");
                }
                cbrushside_t *out = &map->brushsides[outputSide++];
                out->plane = &map->planes[side->value.plane];
                out->materialNum = side->material;
                out->edgeCount = counts[diskSide];
                out->firstAdjacentSideOffset = (int16_t)offset;
            }
            edge += counts[diskSide];
        }
        brush->baseAdjacentSide = edge != firstEdge ? &map->brushEdges[firstEdge] : NULL;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (brush->mins[axis] > brush->maxs[axis])
            {
                return Fail(input, "BSP brush bounds are inverted");
            }
        }
    }
    if (diskSide != sideCount || outputSide != map->numBrushSides || edge != edgeCount)
    {
        return Fail(input, "BSP brush lumps contain unreferenced data");
    }
    return true;
}

static bool CollisionGeometry(BspInput *input, clipMap_t *map)
{
    int walkableBytes;
    int vertexCount;
    if (!CopyCollisionLump(input, LUMP_COLLISIONVERTS, sizeof(float[3]), (void **)&map->verts, &vertexCount) ||
        !CopyCollisionLump(input, LUMP_COLLISIONTRIS, sizeof(uint16_t[3]), (void **)&map->triIndices, &map->triCount) ||
        !CopyCollisionLump(input, LUMP_COLLISIONEDGEWALKABLE, 1, (void **)&map->triEdgeIsWalkable, &walkableBytes) ||
        !CopyCollisionLump(input, LUMP_COLLISIONBORDERS, sizeof(CollisionBorder), (void **)&map->borders,
                           &map->borderCount))
    {
        return false;
    }
    map->vertCount = vertexCount;
    if ((size_t)walkableBytes != DB64_ClipMapWalkableBytes(map->triCount))
    {
        return Fail(input, "BSP collision walkable bitset has the wrong size");
    }
    for (int i = 0; i < map->vertCount; ++i)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!isfinite(map->verts[i][axis]))
            {
                return Fail(input, "BSP collision vertex is not finite");
            }
        }
    }
    for (size_t i = 0; i < (size_t)map->triCount * 3; ++i)
    {
        if (map->triIndices[i] >= map->vertCount)
        {
            return Fail(input, "BSP collision triangle references a missing vertex");
        }
    }
    size_t count = 0;
    const LinkerDiskPartition *disk = (const LinkerDiskPartition *)CollisionLump(input, LUMP_COLLISIONPARTITIONS,
                                                                                 sizeof(LinkerDiskPartition), &count);
    if (!disk || count > INT_MAX / sizeof(CollisionPartition))
    {
        return false;
    }
    map->partitionCount = (int)count;
    map->partitions = count ? (CollisionPartition *)calloc(count, sizeof(CollisionPartition)) : NULL;
    if (count && !map->partitions)
    {
        return Fail(input, "Out of memory importing collision partitions");
    }
    for (size_t i = 0; i < count; ++i)
    {
        LinkerDiskPartition source;
        memcpy(&source, &disk[i], sizeof(LinkerDiskPartition));
        if (source.firstTriIndex < 0 || source.firstTriIndex > map->triCount ||
            source.triCount > map->triCount - source.firstTriIndex || source.firstBorderIndex < 0 ||
            source.firstBorderIndex > map->borderCount ||
            source.borderCount > map->borderCount - source.firstBorderIndex)
        {
            return Fail(input, "BSP collision partition range is invalid");
        }
        CollisionPartition *partition = &map->partitions[i];
        partition->triCount = source.triCount;
        partition->borderCount = source.borderCount;
        partition->firstTri = source.firstTriIndex;
        partition->borders = source.borderCount ? &map->borders[source.firstBorderIndex] : NULL;
    }
    return true;
}

struct LinkerDiskNode
{
    int32_t plane;
    int32_t children[2];
    int32_t bounds[6];
};
struct LinkerDiskLeaf
{
    int32_t cluster;
    int32_t firstAabb;
    int32_t aabbCount;
    int32_t firstBrush;
    int32_t brushCount;
    int32_t cell;
};
static_assert(sizeof(LinkerDiskNode) == 36 && sizeof(LinkerDiskLeaf) == 24);

bool Linker_ImportRenderNodes(const void *bsp, size_t size, unsigned int cellCount, unsigned int planeCount,
                              uint16_t **nodes, unsigned int *wordCount, char *error, size_t errorSize)
{
    if (!nodes || !wordCount || (!error && errorSize))
    {
        return false;
    }
    *nodes = NULL;
    *wordCount = 0;
    if (errorSize)
    {
        error[0] = 0;
    }
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    bool valid = Validate(&input) && cellCount && cellCount <= 1024 && planeCount && planeCount <= 65536;
    size_t nodeCount = 0, leafCount = 0;
    const void *diskNodes = valid ? Lump(&input, LUMP_NODES, sizeof(LinkerDiskNode), &nodeCount) : NULL;
    const void *diskLeaves = valid ? Lump(&input, LUMP_LEAFS, sizeof(LinkerDiskLeaf), &leafCount) : NULL;
    valid = valid && diskNodes && diskLeaves && nodeCount && leafCount && nodeCount <= 32768 && leafCount <= 32768;
    struct CellNode
    {
        LinkerDiskNode source;
        unsigned int words;
        int cell;
        uint8_t state, parents;
    };
    struct NodeFrame
    {
        unsigned int node, next;
    };
    CellNode *work = valid ? (CellNode *)calloc(nodeCount, sizeof(CellNode)) : NULL;
    int *leafCells = valid ? (int *)malloc(leafCount * sizeof(int)) : NULL;
    NodeFrame *stack = valid ? (NodeFrame *)malloc((nodeCount + 1) * sizeof(NodeFrame)) : NULL;
    valid = valid && work && leafCells && stack;
    for (unsigned int i = 0; valid && i < leafCount; ++i)
    {
        LinkerDiskLeaf source;
        memcpy(&source, (const uint8_t *)diskLeaves + i * sizeof(LinkerDiskLeaf), sizeof(source));
        valid = source.cell >= -1 && (source.cell < 0 || (unsigned int)source.cell < cellCount);
        leafCells[i] = source.cell;
    }
    for (unsigned int i = 0; valid && i < nodeCount; ++i)
    {
        memcpy(&work[i].source, (const uint8_t *)diskNodes + i * sizeof(LinkerDiskNode), sizeof(LinkerDiskNode));
        valid = work[i].source.plane >= 0 && (unsigned int)work[i].source.plane < planeCount;
        for (int c = 0; valid && c < 2; ++c)
        {
            const int child = work[i].source.children[c];
            if (child < 0)
            {
                valid = (uint64_t)(-1 - (int64_t)child) < leafCount;
            }
            else
            {
                valid = (unsigned int)child < nodeCount && child != 0;
                if (valid)
                {
                    valid = ++work[child].parents == 1;
                }
            }
        }
    }
    for (unsigned int root = 0; valid && root < nodeCount; ++root)
    {
        if (work[root].state == 2)
        {
            continue;
        }
        unsigned int depth = 1;
        stack[0] = {root, 0};
        work[root].state = 1;
        while (valid && depth)
        {
            NodeFrame *frame = &stack[depth - 1];
            CellNode *node = &work[frame->node];
            if (frame->next < 2)
            {
                const int child = node->source.children[frame->next++];
                if (child >= 0)
                {
                    if (work[child].state == 1)
                    {
                        valid = Fail(&input, "BSP render nodes contain a cycle");
                    }
                    else if (work[child].state == 0)
                    {
                        work[child].state = 1;
                        stack[depth++] = {(unsigned int)child, 0};
                    }
                }
                continue;
            }
            int childCells[2];
            unsigned int childWords[2];
            for (int c = 0; c < 2; ++c)
            {
                const int child = node->source.children[c];
                childCells[c] = child < 0 ? leafCells[-1 - child] : work[child].cell;
                childWords[c] = child < 0 ? 1 : work[child].words;
            }
            node->cell = childCells[0] == childCells[1] ? childCells[0] : -2;
            node->words = node->cell != -2 ? 1 : 2 + childWords[0] + childWords[1];
            valid = node->words <= 3 * nodeCount + 1;
            if (node->cell == -2)
            {
                valid = valid && childWords[0] + 2 <= UINT16_MAX &&
                        (unsigned int)node->source.plane + cellCount + 1 <= UINT16_MAX;
            }
            node->state = 2;
            --depth;
        }
    }
    uint16_t *output = valid ? (uint16_t *)calloc(work[0].words, sizeof(uint16_t)) : NULL;
    valid = valid && output;
    if (valid)
    {
        unsigned int depth = 1;
        stack[0] = {0, 0}; // During emission, next stores the output word offset.
        while (depth)
        {
            const NodeFrame frame = stack[--depth];
            const CellNode *node = &work[frame.node];
            if (node->cell != -2)
            {
                output[frame.next] = (uint16_t)(node->cell + 1);
                continue;
            }
            const int left = node->source.children[0];
            const unsigned int rightOffset = 2 + (left < 0 ? 1 : work[left].words);
            output[frame.next] = (uint16_t)(node->source.plane + cellCount + 1);
            output[frame.next + 1] = (uint16_t)rightOffset;
            for (int c = 1; c >= 0; --c)
            {
                const int child = node->source.children[c];
                const unsigned int offset = frame.next + (c ? rightOffset : 2);
                if (child < 0)
                {
                    output[offset] = (uint16_t)(leafCells[-1 - child] + 1);
                }
                else
                {
                    stack[depth++] = {(unsigned int)child, offset};
                }
            }
        }
        *wordCount = work[0].words;
        *nodes = output;
    }
    free(work);
    free(leafCells);
    free(stack);
    if (!valid && errorSize && !error[0])
    {
        snprintf(error, errorSize, "Invalid BSP render node references or uint16_t encoding overflow");
    }
    return valid;
}

static bool CollisionTopology(BspInput *input, clipMap_t *map)
{
    size_t leafCount = 0, nodeCount = 0, brushCount = 0;
    const LinkerDiskLeaf *leafs = (const LinkerDiskLeaf *)Lump(input, LUMP_LEAFS, sizeof(LinkerDiskLeaf), &leafCount);
    const LinkerDiskNode *nodes = (const LinkerDiskNode *)Lump(input, LUMP_NODES, sizeof(LinkerDiskNode), &nodeCount);
    const uint32_t *brushes = (const uint32_t *)CollisionLump(input, LUMP_LEAFBRUSHES, sizeof(uint32_t), &brushCount);
    if (!leafs || !nodes || !brushes || !leafCount || leafCount > 32768 || !nodeCount || nodeCount > 32768 ||
        brushCount > INT_MAX / sizeof(uint16_t))
    {
        return Fail(input, "Invalid BSP node, leaf or leaf-brush counts");
    }
    map->numLeafs = (uint)leafCount;
    map->numNodes = (uint)nodeCount;
    map->numLeafBrushes = (uint)brushCount;
    map->leafbrushNodesCount = (uint)leafCount + 1;
    map->leafs = (cLeaf_t *)calloc(leafCount, sizeof(cLeaf_t));
    map->nodes = (cNode_t *)calloc(nodeCount, sizeof(cNode_t));
    map->leafbrushNodes = (cLeafBrushNode_s *)calloc(leafCount + 1, sizeof(cLeafBrushNode_s));
    map->leafbrushes = brushCount ? (uint16_t *)malloc(brushCount * sizeof(uint16_t)) : NULL;
    if (!map->leafs || !map->nodes || !map->leafbrushNodes || (brushCount && !map->leafbrushes))
    {
        return Fail(input, "Out of memory importing BSP topology");
    }
    for (size_t i = 0; i < brushCount; ++i)
    {
        if (brushes[i] >= map->numBrushes)
        {
            return Fail(input, "BSP leaf references an invalid brush");
        }
        map->leafbrushes[i] = (uint16_t)brushes[i];
    }
    for (size_t i = 0; i < leafCount; ++i)
    {
        const LinkerDiskLeaf *source = &leafs[i];
        if (source->cluster < -1 || source->cluster > INT16_MAX || source->firstAabb < 0 ||
            source->firstAabb > UINT16_MAX || source->firstAabb > map->aabbTreeCount || source->aabbCount < 0 ||
            source->aabbCount > UINT16_MAX || source->aabbCount > map->aabbTreeCount - source->firstAabb ||
            source->firstBrush < 0 || (size_t)source->firstBrush > brushCount || source->brushCount < 0 ||
            source->brushCount > INT16_MAX || (size_t)source->brushCount > brushCount - source->firstBrush)
        {
            return Fail(input, "Invalid BSP leaf ranges");
        }
        cLeaf_t *leaf = &map->leafs[i];
        leaf->cluster = (int16_t)source->cluster;
        leaf->firstCollAabbIndex = (uint16_t)source->firstAabb;
        leaf->collAabbCount = (uint16_t)source->aabbCount;
        if (source->cluster >= map->numClusters)
        {
            map->numClusters = source->cluster + 1;
        }
        for (int j = 0; j < source->aabbCount; ++j)
        {
            leaf->terrainContents |= map->materials[map->aabbTrees[source->firstAabb + j].materialIndex].contentFlags;
        }
        if (source->brushCount)
        {
            cLeafBrushNode_s *node = &map->leafbrushNodes[i + 1];
            leaf->leafBrushNode = (int)i + 1;
            node->leafBrushCount = (int16_t)source->brushCount;
            node->data.leaf.brushes = &map->leafbrushes[source->firstBrush];
            for (int axis = 0; axis < 3; ++axis)
            {
                leaf->mins[axis] = FLT_MAX;
                leaf->maxs[axis] = -FLT_MAX;
            }
            for (int j = 0; j < source->brushCount; ++j)
            {
                const cbrush_t *brush = &map->brushes[node->data.leaf.brushes[j]];
                leaf->brushContents |= brush->contents;
                for (int axis = 0; axis < 3; ++axis)
                {
                    if (brush->mins[axis] < leaf->mins[axis])
                    {
                        leaf->mins[axis] = brush->mins[axis];
                    }
                    if (brush->maxs[axis] > leaf->maxs[axis])
                    {
                        leaf->maxs[axis] = brush->maxs[axis];
                    }
                }
            }
            node->contents = leaf->brushContents;
            for (int axis = 0; axis < 3; ++axis)
            {
                leaf->mins[axis] -= 0.125f;
                leaf->maxs[axis] += 0.125f;
            }
        }
    }
    for (size_t i = 0; i < nodeCount; ++i)
    {
        if (nodes[i].plane < 0 || (unsigned int)nodes[i].plane >= map->planeCount)
        {
            return Fail(input, "BSP node references an invalid plane");
        }
        map->nodes[i].plane = &map->planes[nodes[i].plane];
        for (int j = 0; j < 2; ++j)
        {
            const int child = nodes[i].children[j];
            if (child < INT16_MIN || child > INT16_MAX || (child >= 0 && (size_t)child >= nodeCount) ||
                (child < 0 && (size_t)(-1 - child) >= leafCount))
            {
                return Fail(input, "BSP node references an invalid child");
            }
            map->nodes[i].children[j] = (int16_t)child;
        }
    }
    int surfaceCount;
    if (!CopyCollisionLump(input, LUMP_LEAFSURFACES, sizeof(uint), (void **)&map->leafsurfaces, &surfaceCount))
    {
        return false;
    }
    map->numLeafSurfaces = surfaceCount;
    return DB64_ValidateBspNodes(map, input->error, input->errorSize);
}

struct LinkerDiskBrushModel
{
    float mins[3];
    float maxs[3];
    uint16_t firstTriSoup[2];
    uint16_t triSoupCount[2];
    int32_t firstSurface;
    int32_t surfaceCount;
    int32_t firstBrush;
    int32_t brushCount;
};
static_assert(sizeof(LinkerDiskBrushModel) == 48);

static bool CollisionSubmodels(BspInput *input, clipMap_t *map)
{
    size_t count = 0;
    const LinkerDiskBrushModel *disk =
        (const LinkerDiskBrushModel *)Lump(input, LUMP_MODELS, sizeof(LinkerDiskBrushModel), &count);
    if (!disk || !count || count > 4095)
    {
        return Fail(input, "Invalid BSP submodel count");
    }
    size_t totalBrushes = map->numLeafBrushes;
    for (size_t i = 1; i < count; ++i)
    {
        const LinkerDiskBrushModel *source = &disk[i];
        if (source->firstBrush < 0 || source->firstBrush > map->numBrushes || source->brushCount < 0 ||
            source->brushCount > INT16_MAX || source->brushCount > map->numBrushes - source->firstBrush ||
            source->firstSurface < 0 || source->firstSurface > UINT16_MAX ||
            source->firstSurface > map->aabbTreeCount || source->surfaceCount < 0 ||
            source->surfaceCount > UINT16_MAX || source->surfaceCount > map->aabbTreeCount - source->firstSurface)
        {
            return Fail(input, "Invalid BSP submodel collision range");
        }
        totalBrushes += source->brushCount;
    }
    if (totalBrushes > INT_MAX / sizeof(uint16_t))
    {
        return Fail(input, "BSP submodel brush lists exceed the allocation limit");
    }
    uint16_t *allBrushes = totalBrushes ? (uint16_t *)malloc(totalBrushes * sizeof(uint16_t)) : NULL;
    if (totalBrushes && !allBrushes)
    {
        return Fail(input, "Out of memory importing submodel brushes");
    }
    if (map->numLeafBrushes)
    {
        memcpy(allBrushes, map->leafbrushes, map->numLeafBrushes * sizeof(uint16_t));
    }
    for (uint i = 0; i < map->leafbrushNodesCount; ++i)
    {
        if (map->leafbrushNodes[i].leafBrushCount > 0)
        {
            const size_t offset = map->leafbrushNodes[i].data.leaf.brushes - map->leafbrushes;
            map->leafbrushNodes[i].data.leaf.brushes = allBrushes + offset;
        }
    }
    free(map->leafbrushes);
    map->leafbrushes = allBrushes;
    const size_t firstNode = map->leafbrushNodesCount;
    const size_t nodeCount = firstNode + count - 1;
    cLeafBrushNode_s *nodes = (cLeafBrushNode_s *)realloc(map->leafbrushNodes, nodeCount * sizeof(cLeafBrushNode_s));
    if (!nodes)
    {
        return Fail(input, "Out of memory importing submodel brush nodes");
    }
    map->leafbrushNodes = nodes;
    memset(nodes + firstNode, 0, (count - 1) * sizeof(cLeafBrushNode_s));
    map->leafbrushNodesCount = (uint)nodeCount;
    map->cmodels = (cmodel_t *)calloc(count, sizeof(cmodel_t));
    if (!map->cmodels)
    {
        return Fail(input, "Out of memory importing submodels");
    }
    map->numSubModels = (uint)count;
    size_t brushOffset = map->numLeafBrushes;
    map->numLeafBrushes = (uint)totalBrushes;
    for (size_t i = 0; i < count; ++i)
    {
        cmodel_t *model = &map->cmodels[i];
        const LinkerDiskBrushModel *source = &disk[i];
        double radiusSquared = 0;
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!isfinite(source->mins[axis]) || !isfinite(source->maxs[axis]) ||
                source->mins[axis] > source->maxs[axis])
            {
                return Fail(input, "Invalid BSP submodel bounds");
            }
            model->mins[axis] = source->mins[axis] - 1;
            model->maxs[axis] = source->maxs[axis] + 1;
            const double extent = fmax(fabs(model->mins[axis]), fabs(model->maxs[axis]));
            radiusSquared += extent * extent;
        }
        model->radius = (float)sqrt(radiusSquared);
        if (!isfinite(model->radius))
        {
            return Fail(input, "BSP submodel radius overflows");
        }
        if (!i)
        {
            continue;
        }
        cLeaf_t *leaf = &model->leaf;
        leaf->firstCollAabbIndex = (uint16_t)source->firstSurface;
        leaf->collAabbCount = (uint16_t)source->surfaceCount;
        for (int j = 0; j < source->surfaceCount; ++j)
        {
            leaf->terrainContents |=
                map->materials[map->aabbTrees[source->firstSurface + j].materialIndex].contentFlags;
        }
        if (!source->brushCount)
        {
            continue;
        }
        leaf->leafBrushNode = (int)(firstNode + i - 1);
        cLeafBrushNode_s *node = &nodes[leaf->leafBrushNode];
        node->leafBrushCount = (int16_t)source->brushCount;
        node->data.leaf.brushes = allBrushes + brushOffset;
        for (int axis = 0; axis < 3; ++axis)
        {
            leaf->mins[axis] = FLT_MAX;
            leaf->maxs[axis] = -FLT_MAX;
        }
        for (int j = 0; j < source->brushCount; ++j)
        {
            const int brushIndex = source->firstBrush + j;
            allBrushes[brushOffset++] = (uint16_t)brushIndex;
            const cbrush_t *brush = &map->brushes[brushIndex];
            leaf->brushContents |= brush->contents;
            for (int axis = 0; axis < 3; ++axis)
            {
                if (brush->mins[axis] < leaf->mins[axis])
                {
                    leaf->mins[axis] = brush->mins[axis];
                }
                if (brush->maxs[axis] > leaf->maxs[axis])
                {
                    leaf->maxs[axis] = brush->maxs[axis];
                }
            }
        }
        node->contents = leaf->brushContents;
        for (int axis = 0; axis < 3; ++axis)
        {
            leaf->mins[axis] -= 0.125f;
            leaf->maxs[axis] += 0.125f;
        }
    }
    return true;
}

static bool CollisionBoxHull(BspInput *input, clipMap_t *map)
{
    map->box_brush = (cbrush_t *)calloc(1, sizeof(cbrush_t));
    if (!map->box_brush)
    {
        return Fail(input, "Out of memory importing collision box hull");
    }
    map->box_brush->contents = -1;
    map->box_model.leaf.brushContents = -1;
    for (int axis = 0; axis < 3; ++axis)
    {
        map->box_model.leaf.mins[axis] = FLT_MAX;
        map->box_model.leaf.maxs[axis] = -FLT_MAX;
        map->box_brush->axialMaterialNum[0][axis] = -1;
        map->box_brush->axialMaterialNum[1][axis] = -1;
    }
    const size_t brushCount = (size_t)map->numLeafBrushes + 1;
    uint16_t *brushes = (uint16_t *)malloc(brushCount * sizeof(uint16_t));
    if (!brushes)
    {
        return Fail(input, "Out of memory importing collision box brush reference");
    }
    if (map->numLeafBrushes)
    {
        memcpy(brushes, map->leafbrushes, map->numLeafBrushes * sizeof(uint16_t));
    }
    for (uint i = 0; i < map->leafbrushNodesCount; ++i)
    {
        cLeafBrushNode_s *node = &map->leafbrushNodes[i];
        if (node->leafBrushCount > 0)
        {
            node->data.leaf.brushes = brushes + (node->data.leaf.brushes - map->leafbrushes);
        }
    }
    free(map->leafbrushes);
    map->leafbrushes = brushes;
    brushes[map->numLeafBrushes] = map->numBrushes;
    cLeafBrushNode_s *nodes = (cLeafBrushNode_s *)realloc(map->leafbrushNodes, ((size_t)map->leafbrushNodesCount + 1) *
                                                                                   sizeof(cLeafBrushNode_s));
    if (!nodes)
    {
        return Fail(input, "Out of memory importing collision box node");
    }
    map->leafbrushNodes = nodes;
    cLeafBrushNode_s *boxNode = &nodes[map->leafbrushNodesCount];
    memset(boxNode, 0, sizeof(cLeafBrushNode_s));
    boxNode->leafBrushCount = 1;
    boxNode->data.leaf.brushes = &brushes[map->numLeafBrushes++];
    map->box_model.leaf.leafBrushNode = (int)map->leafbrushNodesCount++;
    return true;
}

static bool CollisionVisibility(BspInput *input, clipMap_t *map)
{
    size_t length = 0;
    const uint8_t *data = (const uint8_t *)CollisionLump(input, LUMP_VISIBILITY, 1, &length);
    if (!data)
    {
        return false;
    }
    size_t bytes;
    if (length)
    {
        int32_t dimensions[2];
        if (length < sizeof(dimensions))
        {
            return Fail(input, "Truncated BSP visibility header");
        }
        memcpy(dimensions, data, sizeof(dimensions));
        if (dimensions[0] <= 0 || dimensions[1] <= 0 || dimensions[0] < map->numClusters ||
            (uint64_t)dimensions[0] * dimensions[1] != length - sizeof(dimensions) ||
            (uint64_t)dimensions[1] * 8 < (uint32_t)dimensions[0])
        {
            return Fail(input, "Invalid BSP visibility dimensions");
        }
        map->numClusters = dimensions[0];
        map->clusterBytes = dimensions[1];
        map->vised = 1;
        bytes = length - sizeof(dimensions);
    }
    else
    {
        // Without authored PVS every cluster sees every other cluster.
        map->clusterBytes = ((map->numClusters + 63) & ~63) >> 3;
        map->numClusters = 1;
        bytes = map->clusterBytes;
    }
    map->visibility = bytes ? (uint8_t *)malloc(bytes) : NULL;
    if (bytes && !map->visibility)
    {
        return Fail(input, "Out of memory importing BSP visibility");
    }
    if (bytes)
    {
        if (length)
        {
            memcpy(map->visibility, data + 8, bytes);
        }
        else
        {
            memset(map->visibility, 255, bytes);
        }
    }
    return true;
}

bool Linker_ImportCollisionGeometry(const void *bsp, size_t size, clipMap_t **map, char *error, size_t errorSize)
{
    if (!map || (!error && errorSize))
    {
        return false;
    }
    *map = NULL;
    BspInput input = {};
    input.data = (const uint8_t *)bsp;
    input.size = size;
    input.error = error;
    input.errorSize = errorSize;
    if (!Validate(&input))
    {
        return false;
    }
    clipMap_t *result = (clipMap_t *)calloc(1, sizeof(clipMap_t));
    if (!result)
    {
        return Fail(&input, "Out of memory importing collision geometry");
    }
    static_assert(sizeof(CollisionAabbTree) == 32);
    if (!CollisionGeometry(&input, result) || !CollisionBrushes(&input, result) ||
        !CopyCollisionLump(&input, LUMP_COLLISIONAABBS, sizeof(CollisionAabbTree), (void **)&result->aabbTrees,
                           &result->aabbTreeCount) ||
        !DB64_ValidateCollisionTrees(result, error, errorSize) || !CollisionTopology(&input, result) ||
        !CollisionSubmodels(&input, result) || !Linker_SubdivideCollisionBrushes(result, error, errorSize) ||
        !CollisionBoxHull(&input, result) || !CollisionVisibility(&input, result))
    {
        Linker_FreeCollisionGeometry(result);
        return false;
    }
    result->checksum = (uint)crc32(0, input.data, (uInt)input.size);
    *map = result;
    return true;
}

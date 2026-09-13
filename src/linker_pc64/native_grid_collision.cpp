#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <xanim/xanim.h>
#include "native_grid_collision.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

struct LinkerGridCollision
{
    const clipMap_t *map;
    unsigned int brushCount, triangleCount;
    unsigned int *brushes, *triangles;
};

void Linker_FreeGridCollision(LinkerGridCollision *context)
{
    if (context)
    {
        free(context->brushes);
        free(context->triangles);
        free(context);
    }
}

static bool MarkBrushes(const clipMap_t *map, unsigned int root, uint8_t *seen, unsigned int *stack, uint8_t *brushes)
{
    if (!root)
    {
        return true;
    }
    if (root >= map->leafbrushNodesCount)
    {
        return false;
    }
    unsigned int count = 0;
    if (!seen[root])
    {
        stack[count++] = root;
        seen[root] = 1;
    }
    while (count)
    {
        const unsigned int index = stack[--count];
        const cLeafBrushNode_s *node = &map->leafbrushNodes[index];
        if (node->leafBrushCount > 0)
        {
            const uintptr_t base = (uintptr_t)map->leafbrushes, pointer = (uintptr_t)node->data.leaf.brushes;
            if (!base || pointer < base || (pointer - base) % sizeof(uint16_t) ||
                (pointer - base) / sizeof(uint16_t) > map->numLeafBrushes ||
                (unsigned int)node->leafBrushCount > map->numLeafBrushes - (pointer - base) / sizeof(uint16_t))
            {
                return false;
            }
            for (int i = 0; i < node->leafBrushCount; ++i)
            {
                const unsigned int brush = node->data.leaf.brushes[i];
                if (brush >= map->numBrushes)
                {
                    return false;
                }
                if (map->brushes[brush].contents & 8193)
                {
                    brushes[brush] = 1;
                }
            }
        }
        else
        {
            const unsigned int offsets[3] = {node->data.children.childOffset[0], node->data.children.childOffset[1],
                                             node->leafBrushCount < 0 ? 1 : 0};
            for (unsigned int i = 0; i < 3; ++i)
            {
                if (!offsets[i])
                {
                    continue;
                }
                if (offsets[i] >= map->leafbrushNodesCount - index)
                {
                    return false;
                }
                const unsigned int child = index + offsets[i];
                if (!seen[child])
                {
                    seen[child] = 1;
                    stack[count++] = child;
                }
            }
        }
    }
    return true;
}

static bool MarkTriangles(const clipMap_t *map, unsigned int root, uint8_t *seen, unsigned int *stack,
                          uint8_t *triangles)
{
    if (root >= (unsigned int)map->aabbTreeCount || map->aabbTrees[root].materialIndex >= map->numMaterials)
    {
        return false;
    }
    if (!(map->materials[map->aabbTrees[root].materialIndex].contentFlags & 8193))
    {
        return true;
    }
    unsigned int count = 0;
    if (!seen[root])
    {
        seen[root] = 1;
        stack[count++] = root;
    }
    while (count)
    {
        const CollisionAabbTree *node = &map->aabbTrees[stack[--count]];
        const int first = node->u.firstChildIndex;
        if (node->childCount)
        {
            if (first < 0 || first >= map->aabbTreeCount || node->childCount > map->aabbTreeCount - first)
            {
                return false;
            }
            for (unsigned int i = 0; i < node->childCount; ++i)
            {
                const unsigned int child = first + i;
                if (!seen[child])
                {
                    seen[child] = 1;
                    stack[count++] = child;
                }
            }
        }
        else
        {
            if (first < 0 || first >= map->partitionCount)
            {
                return false;
            }
            const CollisionPartition *partition = &map->partitions[first];
            if (partition->firstTri < 0 || partition->firstTri > map->triCount ||
                partition->triCount > map->triCount - partition->firstTri)
            {
                return false;
            }
            for (unsigned int i = 0; i < partition->triCount; ++i)
            {
                triangles[partition->firstTri + i] = 1;
            }
        }
    }
    return true;
}

bool Linker_CreateGridCollision(const clipMap_t *map, LinkerGridCollision **result, char *error, size_t errorSize)
{
    if (!result || (!error && errorSize))
    {
        return false;
    }
    *result = NULL;
    bool valid = map && map->triCount >= 0 && map->partitionCount >= 0 && map->aabbTreeCount >= 0 &&
                 (!map->numLeafs || map->leafs) && (!map->numBrushes || map->brushes) &&
                 (!map->leafbrushNodesCount || map->leafbrushNodes) && (!map->aabbTreeCount || map->aabbTrees) &&
                 (!map->partitionCount || map->partitions) && (!map->triCount || (map->triIndices && map->verts)) &&
                 (!map->numMaterials || map->materials);
    LinkerGridCollision *context = valid ? (LinkerGridCollision *)calloc(1, sizeof(LinkerGridCollision)) : NULL;
    valid = valid && context;
    const size_t nodeCount =
        valid ? (map->leafbrushNodesCount > (unsigned int)map->aabbTreeCount ? map->leafbrushNodesCount
                                                                             : (unsigned int)map->aabbTreeCount)
              : 0;
    uint8_t *seenBrush = valid ? (uint8_t *)calloc(map->leafbrushNodesCount + (size_t)1, sizeof(uint8_t)) : NULL;
    uint8_t *seenTree = valid ? (uint8_t *)calloc(map->aabbTreeCount + (size_t)1, sizeof(uint8_t)) : NULL;
    uint8_t *brushes = valid ? (uint8_t *)calloc(map->numBrushes + (size_t)1, sizeof(uint8_t)) : NULL;
    uint8_t *triangles = valid ? (uint8_t *)calloc(map->triCount + (size_t)1, sizeof(uint8_t)) : NULL;
    unsigned int *stack = valid ? (unsigned int *)malloc((nodeCount + 1) * sizeof(unsigned int)) : NULL;
    valid = valid && seenBrush && seenTree && brushes && triangles && stack;
    for (unsigned int i = 0; valid && i < map->numLeafs; ++i)
    {
        const cLeaf_t *leaf = &map->leafs[i];
        valid = MarkBrushes(map, leaf->leafBrushNode, seenBrush, stack, brushes) &&
                leaf->firstCollAabbIndex <= map->aabbTreeCount &&
                leaf->collAabbCount <= map->aabbTreeCount - leaf->firstCollAabbIndex;
        for (unsigned int j = 0; valid && j < leaf->collAabbCount; ++j)
        {
            valid = MarkTriangles(map, leaf->firstCollAabbIndex + j, seenTree, stack, triangles);
        }
    }
    if (valid)
    {
        context->map = map;
        context->brushes = (unsigned int *)malloc((map->numBrushes + (size_t)1) * sizeof(unsigned int));
        context->triangles = (unsigned int *)malloc((map->triCount + (size_t)1) * sizeof(unsigned int));
        valid = context->brushes && context->triangles;
    }
    for (unsigned int i = 0; valid && i < map->numBrushes; ++i)
    {
        if (!brushes[i])
        {
            continue;
        }
        const cbrush_t *brush = &map->brushes[i];
        valid = !brush->numsides || brush->sides;
        for (unsigned int j = 0; valid && j < 3; ++j)
        {
            valid = isfinite(brush->mins[j]) && isfinite(brush->maxs[j]) && brush->mins[j] <= brush->maxs[j];
        }
        for (unsigned int j = 0; valid && j < brush->numsides; ++j)
        {
            const cplane_s *plane = brush->sides[j].plane;
            valid = plane && isfinite(plane->dist);
            for (unsigned int k = 0; valid && k < 3; ++k)
            {
                valid = isfinite(plane->normal[k]);
            }
        }
        context->brushes[context->brushCount++] = i;
    }
    for (int i = 0; valid && i < map->triCount; ++i)
    {
        if (!triangles[i])
        {
            continue;
        }
        for (unsigned int j = 0; valid && j < 3; ++j)
        {
            const unsigned int vertex = map->triIndices[(size_t)i * 3 + j];
            valid = vertex < map->vertCount;
            for (unsigned int k = 0; valid && k < 3; ++k)
            {
                valid = isfinite(map->verts[vertex][k]);
            }
        }
        context->triangles[context->triangleCount++] = i;
    }
    free(seenBrush);
    free(seenTree);
    free(brushes);
    free(triangles);
    free(stack);
    if (!valid)
    {
        Linker_FreeGridCollision(context);
        if (errorSize)
        {
            snprintf(error, errorSize, "Invalid world collision for light-grid visibility or allocation failure");
        }
        return false;
    }
    *result = context;
    return true;
}

static bool ClipPlane(double start, double end, double *enter, double *leave)
{
    if (start > 0 && end > 0)
    {
        return false;
    }
    if (start > 0)
    {
        const double fraction = start / (start - end);
        if (fraction > *enter)
        {
            *enter = fraction;
        }
    }
    else if (end > 0)
    {
        const double fraction = start / (start - end);
        if (fraction < *leave)
        {
            *leave = fraction;
        }
    }
    return *enter < *leave;
}

static bool HitsBrush(const cbrush_t *brush, const float *start, const float *end)
{
    double enter = 0, leave = 1;
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (!ClipPlane((double)brush->mins[i] - start[i], (double)brush->mins[i] - end[i], &enter, &leave) ||
            !ClipPlane((double)start[i] - brush->maxs[i], (double)end[i] - brush->maxs[i], &enter, &leave))
        {
            return false;
        }
    }
    for (unsigned int i = 0; i < brush->numsides; ++i)
    {
        const cplane_s *plane = brush->sides[i].plane;
        double a = -plane->dist, b = -plane->dist;
        for (unsigned int j = 0; j < 3; ++j)
        {
            a += (double)start[j] * plane->normal[j];
            b += (double)end[j] * plane->normal[j];
        }
        if (!ClipPlane(a, b, &enter, &leave))
        {
            return false;
        }
    }
    return true;
}

static void Cross(const double *a, const double *b, double *result)
{
    result[0] = a[1] * b[2] - a[2] * b[1];
    result[1] = a[2] * b[0] - a[0] * b[2];
    result[2] = a[0] * b[1] - a[1] * b[0];
}

static double Dot(const double *a, const double *b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static bool HitsTriangle(const clipMap_t *map, unsigned int triangle, const float *start, const float *end,
                         double *fraction)
{
    const uint16_t *indices = map->triIndices + (size_t)triangle * 3;
    double edge1[3], edge2[3], delta[3], relative[3], normal[3], tracePlane[3];
    for (unsigned int i = 0; i < 3; ++i)
    {
        edge1[i] = (double)map->verts[indices[0]][i] - map->verts[indices[1]][i];
        edge2[i] = (double)map->verts[indices[0]][i] - map->verts[indices[2]][i];
        delta[i] = (double)end[i] - start[i];
        relative[i] = (double)map->verts[indices[0]][i] - start[i];
    }
    Cross(edge2, edge1, normal);
    const double area = Dot(delta, normal), t = Dot(relative, normal);
    if (area >= 0 || t > 0 || t <= area)
    {
        return false;
    }
    Cross(delta, relative, tracePlane);
    const double v = Dot(tracePlane, edge1), negativeU = Dot(tracePlane, edge2);
    const bool hit = v <= 0 && v >= area && negativeU >= 0 && v - negativeU >= area;
    if (hit && fraction)
    {
        *fraction = t / area;
    }
    return hit;
}

bool Linker_GridCollisionTrace(const float *start, const float *end, bool *visible, void *context)
{
    const LinkerGridCollision *query = (const LinkerGridCollision *)context;
    if (!query || !start || !end || !visible)
    {
        return false;
    }
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (!isfinite(start[i]) || !isfinite(end[i]))
        {
            return false;
        }
    }
    for (unsigned int i = 0; i < query->brushCount; ++i)
    {
        if (HitsBrush(&query->map->brushes[query->brushes[i]], start, end))
        {
            *visible = false;
            return true;
        }
    }
    for (unsigned int i = 0; i < query->triangleCount; ++i)
    {
        if (HitsTriangle(query->map, query->triangles[i], start, end, NULL))
        {
            *visible = false;
            return true;
        }
    }
    *visible = true;
    return true;
}

static bool TracePlane(double a, double b, double *enter, double *leave, bool *lead, bool *allsolid)
{
    if (a > 0)
    {
        if (fmin(0.125, a) <= b)
        {
            return false;
        }
        const double fraction = (a - 0.125) / (a - b);
        if (fraction >= *leave)
        {
            return false;
        }
        if (fraction > *enter)
        {
            *enter = fraction;
        }
        *lead = true;
    }
    else if (b > 0)
    {
        const double fraction = a / (a - b);
        if (fraction <= *enter)
        {
            return false;
        }
        if (fraction < *leave)
        {
            *leave = fraction;
        }
    }
    if (b > 0)
    {
        *allsolid = false;
    }
    return true;
}

static double BrushFraction(const cbrush_t *brush, const float *start, const float *end, double closest)
{
    double enter = 0, leave = closest;
    bool lead = false, allsolid = true;
    for (unsigned int side = 0; side < 2; ++side)
    {
        for (unsigned int i = 0; i < 3; ++i)
        {
            const double a = side ? (double)start[i] - brush->maxs[i] : (double)brush->mins[i] - start[i];
            const double b = side ? (double)end[i] - brush->maxs[i] : (double)brush->mins[i] - end[i];
            if (!TracePlane(a, b, &enter, &leave, &lead, &allsolid))
            {
                return closest;
            }
        }
    }
    for (unsigned int i = 0; i < brush->numsides; ++i)
    {
        const cplane_s *plane = brush->sides[i].plane;
        double a = -plane->dist, b = -plane->dist;
        for (unsigned int j = 0; j < 3; ++j)
        {
            a += (double)start[j] * plane->normal[j];
            b += (double)end[j] * plane->normal[j];
        }
        if (!TracePlane(a, b, &enter, &leave, &lead, &allsolid))
        {
            return closest;
        }
    }
    return lead ? enter : (allsolid ? 0 : closest);
}

bool Linker_GridCollisionFraction(const float *start, const float *end, float *fraction, void *context)
{
    const LinkerGridCollision *query = (const LinkerGridCollision *)context;
    if (!query || !start || !end || !fraction)
    {
        return false;
    }
    for (unsigned int i = 0; i < 3; ++i)
    {
        if (!isfinite(start[i]) || !isfinite(end[i]))
        {
            return false;
        }
    }
    double closest = 1;
    for (unsigned int i = 0; closest > 0 && i < query->brushCount; ++i)
    {
        closest = BrushFraction(&query->map->brushes[query->brushes[i]], start, end, closest);
    }
    for (unsigned int i = 0; closest > 0 && i < query->triangleCount; ++i)
    {
        double hit;
        if (HitsTriangle(query->map, query->triangles[i], start, end, &hit) && hit < closest)
        {
            closest = hit;
        }
    }
    *fraction = (float)closest;
    return true;
}

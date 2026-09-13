#include <universal/q_shared.h>
#include <xanim/xanim.h>
#include "db_model_surfaces.h"
#include <stdio.h>
#include <stdlib.h>

static bool TreeIsAcyclic(const XSurfaceCollisionTree *tree)
{
    unsigned int *indegree = (unsigned int *)calloc(tree->nodeCount, sizeof(unsigned int));
    unsigned int *queue = (unsigned int *)malloc(tree->nodeCount * sizeof(unsigned int));
    if (!indegree || !queue)
    {
        free(indegree);
        free(queue);
        return false;
    }
    for (unsigned int i = 0; i < tree->nodeCount; ++i)
    {
        const XSurfaceCollisionNode *node = &tree->nodes[i];
        if (!(node->childCount & 32768))
        {
            for (unsigned int j = 0; j < node->childCount; ++j)
            {
                ++indegree[node->childBeginIndex + j];
            }
        }
    }
    unsigned int end = 0;
    for (unsigned int i = 0; i < tree->nodeCount; ++i)
    {
        if (!indegree[i])
        {
            queue[end++] = i;
        }
    }
    for (unsigned int begin = 0; begin < end; ++begin)
    {
        const XSurfaceCollisionNode *node = &tree->nodes[queue[begin]];
        if (!(node->childCount & 32768))
        {
            for (unsigned int j = 0; j < node->childCount; ++j)
            {
                const unsigned int child = node->childBeginIndex + j;
                if (!--indegree[child])
                {
                    queue[end++] = child;
                }
            }
        }
    }
    free(indegree);
    free(queue);
    return end == tree->nodeCount;
}

static bool Invalid(char *error, size_t size, const char *text)
{
    if (size)
    {
        snprintf(error, size, "%s", text);
    }
    return false;
}

bool DB64_ValidateModelSurface(const XSurface *surface, unsigned int boneCount, char *error, size_t errorSize)
{
    if (!surface || !boneCount || boneCount > 128)
    {
        return Invalid(error, errorSize, "Invalid native model surface or bone count");
    }
    uint8_t deformed;
    memcpy(&deformed, &surface->deformed, sizeof(uint8_t));
    unsigned int vertices = 0;
    unsigned int blends = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (surface->vertInfo.vertCount[i] < 0)
        {
            return Invalid(error, errorSize, "Negative native model skin partition");
        }
        vertices += surface->vertInfo.vertCount[i];
        blends += (2 * i + 1) * surface->vertInfo.vertCount[i];
    }
    if (deformed > 1 || (deformed && vertices != surface->vertCount) || (!deformed && vertices) ||
        (!!surface->verts0 != (surface->vertCount != 0)) || (!!surface->triIndices != (surface->triCount != 0)) ||
        (!!surface->vertInfo.vertsBlend != (blends != 0)) || surface->vertListCount > surface->vertCount ||
        (!!surface->vertList != (surface->vertListCount != 0)))
    {
        return Invalid(error, errorSize, "Invalid native model surface counts");
    }
    const uint16_t *blend = surface->vertInfo.vertsBlend;
    for (int weight = 0; weight < 4; ++weight)
    {
        for (int vertex = 0; vertex < surface->vertInfo.vertCount[weight]; ++vertex)
        {
            for (int influence = 0; influence <= weight; ++influence)
            {
                const unsigned int offset = blend[influence ? 2 * influence - 1 : 0];
                if (offset % sizeof(DObjSkelMat) || offset / sizeof(DObjSkelMat) >= boneCount)
                {
                    return Invalid(error, errorSize, "Native model skin weight references an invalid bone");
                }
            }
            blend += 2 * weight + 1;
        }
    }
    for (unsigned int i = 0; i < 3 * (unsigned int)surface->triCount; ++i)
    {
        if (surface->triIndices[i] >= surface->vertCount)
        {
            return Invalid(error, errorSize, "Native model triangle references an invalid vertex");
        }
    }
    unsigned int rigidVertices = 0;
    for (unsigned int i = 0; i < surface->vertListCount; ++i)
    {
        const XRigidVertList *list = &surface->vertList[i];
        rigidVertices += list->vertCount;
        if (rigidVertices > surface->vertCount || list->boneOffset % sizeof(DObjSkelMat) ||
            list->boneOffset / sizeof(DObjSkelMat) >= boneCount ||
            (unsigned int)list->triOffset + list->triCount > surface->triCount)
        {
            return Invalid(error, errorSize, "Invalid native model rigid vertex range");
        }
        const XSurfaceCollisionTree *tree = list->collisionTree;
        if (!tree)
        {
            continue;
        }
        if (!tree->nodeCount || tree->nodeCount > 65536 || tree->leafCount > 65536 || !tree->nodes ||
            (!!tree->leafs != (tree->leafCount != 0)))
        {
            return Invalid(error, errorSize, "Invalid native model collision-tree counts");
        }
        for (unsigned int j = 0; j < tree->nodeCount; ++j)
        {
            const XSurfaceCollisionNode *node = &tree->nodes[j];
            const unsigned int count = node->childCount & 32767;
            const unsigned int limit = (node->childCount & 32768) ? tree->leafCount : tree->nodeCount;
            if (!count || (unsigned int)node->childBeginIndex + count > limit)
            {
                return Invalid(error, errorSize, "Invalid native model collision-tree child range");
            }
        }
        for (unsigned int j = 0; j < tree->leafCount; ++j)
        {
            const unsigned int first = tree->leafs[j].triangleBeginIndex & 32767;
            const unsigned int count = (tree->leafs[j].triangleBeginIndex & 32768) ? 2 : 1;
            if (first + count > surface->triCount)
            {
                return Invalid(error, errorSize, "Invalid native model collision-tree triangle range");
            }
        }
        if (!TreeIsAcyclic(tree))
        {
            return Invalid(error, errorSize, "Cyclic native model collision tree or insufficient validation memory");
        }
    }
    if (!deformed && rigidVertices != surface->vertCount)
    {
        return Invalid(error, errorSize, "Native model rigid lists do not cover its vertices");
    }
    return true;
}

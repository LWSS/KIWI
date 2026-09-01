/*
 * modelcollision.c — Model collision mesh building and tracing.
 *
 * All functions verified against cod2rad64 LST line by line.
 * Source: modelcollision.cpp (from LST assert strings: .\\modelcollision.cpp)
 * Naming conventions from cod2map collision system (CM_ prefix).
 */

#include "cod2rad64.h"

/* CM_TraceBox assert-disable flags (byte_6312C15/14/13/12) */
static char s_assertDisable_CM_TraceBox_boxHeight;
static char s_assertDisable_CM_TraceBox_boundsX;
static char s_assertDisable_CM_TraceBox_boundsY;
static char s_assertDisable_CM_TraceBox_boundsZ;

BSPSubdivNode_t      g_bspSubdivNodes[1024];
float                g_maxCollisionRadius;
CollisionMeshNode_t *g_collisionMeshList;
CollisionInstance_t *g_collisionModelList;
int                  g_bspSubdivNodeCount;
void                *g_modelBoundsData;
/*
 * BuildBSPSubdivision — recursively subdivide a bounding box into a BSP tree.
 * Address: 0x4178A0 | Size: 214 bytes
 *
 * ecx=nodeIndex, rdx=mins (float[2]), r8=maxs (float[2])
 *
 * Splits along the longest axis (X or Y), stores split plane in global node array,
 * then recurses into left child (nodeIndex*2+1) and right child (nodeIndex*2+2).
 * Max depth limited by nodeIndex > 0x3FF.
 */

void BuildBSPSubdivision(int nodeIndex, float *mins, float *maxs)
{
    int splitAxis;
    float splitPos;
    float newBounds[2];
    int otherAxis;

    if (nodeIndex > 0x3FF)
        return;

    /* pick split axis: whichever has larger range (Y vs X) */
    if ((maxs[1] - mins[1]) > (maxs[0] - mins[0]))
        splitAxis = 1;
    else
        splitAxis = 0;

    otherAxis = 1 - splitAxis;

    /* compute split position = midpoint */
    splitPos = (mins[splitAxis] + maxs[splitAxis]) * 0.5f;

    /* store in node */
    g_bspSubdivNodes[nodeIndex].splitAxis = splitAxis;
    g_bspSubdivNodes[nodeIndex].splitPos = splitPos;

    /* left child: same mins, maxs clamped to splitPos on split axis */
    newBounds[splitAxis] = splitPos;
    newBounds[otherAxis] = maxs[otherAxis];
    BuildBSPSubdivision(nodeIndex * 2 + 1, mins, newBounds);

    /* right child: mins clamped to splitPos on split axis, same maxs */
    newBounds[otherAxis] = mins[otherAxis];
    BuildBSPSubdivision(nodeIndex * 2 + 2, newBounds, maxs);
}

/*
 * CollisionModel — linked list node for static model collision instances.
 * Accessed at offsets: +0x48(origin[0]), +0x4C(origin[1]), +0x50(origin[2]),
 * +0x54..+0x58(halfSize per axis), +0x5C(radius), +0x60(next pointer).
 */

/*
 * BuildModelCollision — build BSP subdivision tree and insert collision models.
 * Address: 0x417980 | Size: 383 bytes
 *
 * rcx=mins (float[2]), rdx=maxs (float[2])
 *
 * Builds the 2D BSP subdivision, then walks the global collision model list,
 * inserting each model into the BSP tree based on its position and radius.
 */
void BuildModelCollision(float *mins, float *maxs)
{
    int splitAxis;
    float splitPos;
    float newBounds[2];
    int otherAxis;
    char *model;       /* collision model pointer */
    char *nextModel;
    float radius;
    float boundsMargin;
    int nodeIndex;
    int childIndex;
    int instanceCount = 0;
    float nodePos;

    /* find initial split axis (same logic as BuildBSPSubdivision) */
    if ((maxs[1] - mins[1]) > (maxs[0] - mins[0]))
        splitAxis = 1;
    else
        splitAxis = 0;

    otherAxis = 1 - splitAxis;

    /* compute split position = midpoint */
    splitPos = (mins[splitAxis] + maxs[splitAxis]) * 0.5f;

    /* store root node */
    g_bspSubdivNodes[0].splitAxis = splitAxis;
    g_bspSubdivNodes[0].splitPos = splitPos;



    /* subdivide left child */
    newBounds[splitAxis] = splitPos;
    newBounds[otherAxis] = maxs[otherAxis];
    BuildBSPSubdivision(1, mins, newBounds);

    /* subdivide right child */
    newBounds[otherAxis] = mins[otherAxis];
    BuildBSPSubdivision(2, newBounds, maxs);

    /* walk collision instance linked list */
    {
        CollisionInstance_t *inst = g_collisionModelList;
        CollisionInstance_t *nextInst;
        boundsMargin = 0.001f; /* dword_457AF0 — loaded at sub_417980+B3 (movss xmm2,cs:dword_457AF0) */
        g_maxCollisionRadius = -131072.0f; /* dword_458A90 = 0xC8000000 */

        if (!inst)
            goto done;

        do
        {
            nextInst = inst->next;

            /* track max radius */
            radius = inst->absMaxs[2]; /* +0x5C = absMaxs[2] used as radius */
            if (radius > g_maxCollisionRadius)
                g_maxCollisionRadius = radius;

            /* walk BSP tree to find insertion leaf */
            nodeIndex = 0;
            childIndex = 0;
            g_bspSubdivNodes[0].childCount++; /* binary increments root childCount (was g_bspSubdivNodeCount) */

            for (;;)
            {
                BSPSubdivNode_t *bspNode = &g_bspSubdivNodes[nodeIndex];
                int axis = bspNode->splitAxis;
                nodePos = bspNode->splitPos;

                /* check if model is fully on one side of split */
                if (nodePos - boundsMargin > inst->absMaxs[axis])
                {
                    /* model is on left side */
                    childIndex = childIndex * 2 + 1;
                    nodeIndex = nodeIndex * 2 + 1;
                }
                else if (inst->absMins[axis] > nodePos + boundsMargin)
                {
                    /* model is on right side */
                    childIndex = childIndex * 2 + 2;
                    nodeIndex = nodeIndex * 2 + 2;
                }
                else
                {
                    /* model straddles split — insert at this node */
                    break;
                }

                /* binary increments childCount at each intermediate node during traversal */
                g_bspSubdivNodes[nodeIndex].childCount++;

                /* check if we've exceeded max depth */
                if (childIndex * 2 + 2 >= 0x3FF)
                    break;
            }

            /* insert into node's linked list */
            inst->next = g_bspSubdivNodes[nodeIndex].list;
            g_bspSubdivNodes[nodeIndex].list = inst;
            ++instanceCount;

            inst = nextInst;
        } while (nextInst);
    }

done:
    g_collisionModelList = NULL;
    if (g_modelShadows && g_gridSampleCount > 0
        && instanceCount < g_gridSampleCount / 2)
    {
        WarningMsg(1,
            "WARNING: only %i of %i shadow-enabled static models entered the baked shadow trace world; rejected models will have shadows only inside runtime shadow-map range.\n",
            instanceCount, g_gridSampleCount / 2);
    }
    else if (instanceCount > 0)
    {
        Com_Printf("Built static-model shadow trace world with %i instances\n",
                   instanceCount);
    }
}

/*
 * AddStaticModelCollision — build AABB tree for a static model's collision tris.
 * Address: 0x417B00 | Size: 697 bytes
 *
 * ecx=numTris, rdx=triData (80 bytes per tri), r8=triMins (float[3] per tri),
 * r9=triMaxs (float[3] per tri)
 *
 * Returns pointer to allocated CollisionAabbTree_t array.
 */
CollisionAabbTree_t *AddStaticModelCollision(int numTris, void *triData,
                                              float *triMins, float *triMaxs)
{
    AabbTreeBuilder_t builder;
    AabbTreeNode_t treeNodes[0x2000];
    int nodeCount;
    CollisionAabbTree_t *result;
    int i;

    builder.itemData = triData;
    builder.itemCount = numTris;
    builder.itemStride = 0x50;  /* 80 bytes per tri */
    builder.hasBoundsData = 1;
    builder.itemMins = triMins;
    builder.itemMaxs = triMaxs;
    builder.nodes = treeNodes;
    builder.maxNodes = 0x2000;
    builder.minPartitionSize = 0x10;
    builder.minLeafItems = 0x20;

    nodeCount = BuildAabbTree(&builder);

    /* allocate output array: nodeCount * 0x38 (56 bytes each) */
    result = (CollisionAabbTree_t *)malloc(nodeCount * sizeof(CollisionAabbTree_t));
    if (!result)
    {
        ErrorMsg("Couldn't allocate %i bytes for model collision tree\n",
                 nodeCount * (int)sizeof(CollisionAabbTree_t));
    }

    /* convert each tree node to CollisionAabbTree_t */
    for (i = 0; i < nodeCount; i++)
    {
        AabbTreeNode_t *src = &treeNodes[i];
        CollisionAabbTree_t *dst = &result[i];

        /* assert: firstItem >= 0 (line 0xBA) */
        Assert("treeNodes[nodeIndex].firstItem >= 0",
               ".\\modelcollision.cpp", 0xBA, 0, 1);

        /* assert: firstItem + itemCount <= numTris (line 0xBB) */
        Assert("treeNodes[nodeIndex].firstItem + treeNodes[nodeIndex].itemCount <= numStaticModelCollisionTris",
               ".\\modelcollision.cpp", 0xBB, 0, 1);

        /* itemCount */
        dst->itemCount = src->itemCount;

        /* data = triData + firstItem */
        dst->data = (CollisionTri_t *)triData + src->firstItem;

        /* childCount */
        dst->childCount = src->childCount;

        /* firstChild = &result[src->firstChild] (or NULL if no children) */
        dst->firstChild = &result[src->firstChild];

        /* compute bounds from triMins/triMaxs for items in this node */
        ClearBounds(dst->mins, dst->maxs);
        {
            int j;
            int endItem = src->firstItem + src->itemCount;
            for (j = src->firstItem; j < endItem; j++)
            {
                ExpandBounds(&triMins[j * 3], &triMaxs[j * 3], dst->mins, dst->maxs);
            }
        }
    }

    return result;
}

extern CollisionMeshNode_t *g_collisionMeshList;  /* qword_630CC10 */

/* DObjGetSurface, PlaneFromPoints — in cod2rad64.h */
extern void ComputeBaryCoords(float *v0, float *v1, float *v2, float *plane,
                               float *outBary0, float *outBary1);
extern void __security_check_cookie(void *cookie);

/*
 * BuildStaticModelCollisionMesh — build collision mesh for a static model.
 * Address: 0x417DC0 | Size: 1540 bytes
 *
 * rcx=model (XModel*)
 * Returns CollisionMeshNode* (linked into global list).
 *
 * Creates a DObj, calculates bone transforms, deforms vertices,
 * builds collision triangles from deformed positions, then calls
 * AddStaticModelCollision to build the AABB tree.
 */
CollisionMeshNode_t *BuildStaticModelCollisionMesh(void *model)
{
    CollisionMeshNode_t *mesh;
    char dobj[0x100];           /* local DObj (large enough for stack DObj) */
    char boneMats[0x10000];     /* bone matrices buffer */
    int partBits[4];
    short surfMap[256];         /* surface map from DObjGetSurfaces */
    unsigned short indices[0x8000]; /* index buffer for R_XSurfaceCopyIndices */
    float positions[0x8000 * 3]; /* deformed positions */
    float texcoords[0x8000 * 2]; /* deformed texcoords */
    void *materials[256];       /* material pointers per surface */
    int surfCount;
    int totalTris;
    int i, j;
    void *surface;
    MaterialDef_t *material;
    unsigned char lodBuf[256];
    void *triData;
    float *triMins;
    float *triMaxs;
    int triOffset;
    CollisionAabbTree_t *collTree;

    /* allocate collision mesh node */
    mesh = (CollisionMeshNode_t *)malloc(sizeof(CollisionMeshNode_t));
    if (!mesh)
        Com_Printf("Out of memory for model collision mesh node\n");

    /* initialize mesh node */
    mesh->model = model;
    mesh->collTree = NULL;

    /* link into global list */
    mesh->next = g_collisionMeshList;
    g_collisionMeshList = mesh;

    /* check if model is bad */
    if (XModelBad(model))
        return mesh;

    /* create DObj from model */
    {
        struct { void *model; void *boneName; int ignoreCollision; int pad; } dobjModel;
        dobjModel.model = model;
        dobjModel.boneName = NULL;
        dobjModel.ignoreCollision = 0;
        DObjCreate(&dobjModel, 1, 0, dobj, 0);
    }

    /* calculate skeleton */
    DObjCreateSkel(dobj, boneMats, 0);

    /* calculate animation with full partBits */
    partBits[0] = -1;
    partBits[1] = -1;
    partBits[2] = -1;
    partBits[3] = -1;
    DObjCalcAnim(dobj, partBits);
    DObjCalcSkel(dobj, partBits);

    /* get all surfaces */
    memset(lodBuf, 0, sizeof(lodBuf));
    surfCount = DObjGetSurfaces(dobj, surfMap, partBits, lodBuf);

    /* get bone matrices for deformation — use separate buffer from skel */
    {
        char outMats[0x10000];
        memset(outMats, 0, sizeof(outMats));
        DObjGetMatrices(dobj, partBits, outMats); /* dobj_41C750 */
        memcpy(boneMats, outMats, sizeof(outMats));
    }


    /* first pass: count total tris and check materials */
    totalTris = 0;
    for (i = 0; i < surfCount; i++)
    {
        /* DObjGetSurfaceName returns material name string (DObj stores surfNames[subMatIdx] via SL_ConvertToString) */
        const char *surfMatName = DObjGetSurfaceName(dobj, surfMap[i * 2], surfMap[i * 2 + 1], lodBuf[surfMap[i * 2]]);
        material = LoadMaterial(surfMatName);

        /* Retail CoD4Rad sub_41A590 (0x41A6F7..0x41A71F) accepts a
         * surface when it is a patch or has an alpha collision mask, its
         * material contents include solid/foliage/clipshot (0x2003), and
         * SURF_NOCASTSHADOW is clear.  The old port applied 0x2003 to
         * surfaceFlags and 0x40000 to contents, reversing the two fields. */
        {
            if (((material->surfaceType & SURFTYPE_MASK) == SURFTYPE_PATCH
                 || material->extraData)
                && (material->contents
                    & (CONTENTS_SOLID | CONTENTS_FOLIAGE | CONTENTS_CLIPSHOT))
                && !(material->surfaceFlags & SURF_NOCASTSHADOW))
            {
                /* valid collision material */
                surface = DObjGetSurface(dobj, surfMap[i * 2], surfMap[i * 2 + 1], 0);
                /* KIWI: the fixed stack buffers below hold 0x8000 indices /
                 * 0x8000 verts; an oversized surface silently smashed the
                 * stack (garbage verts, collvec scale assert). Skip it with a
                 * loud warning instead. */
                if (XSurfaceGetNumTris(surface) * 3 > 0x8000
                    || ((XSurface_t *)surface)->vertCount > 0x8000)
                {
                    Com_Printf("WARNING: model surface %i (%s) exceeds cod4rad limits "
                               "(%i tris / %i verts, max 10922 / 32768) - skipped for "
                               "collision/shadows\n",
                               i, surfMatName, XSurfaceGetNumTris(surface),
                               (int)((XSurface_t *)surface)->vertCount);
                    materials[i] = NULL;
                    continue;
                }
                totalTris += XSurfaceGetNumTris(surface);
                materials[i] = material;
            }
            else
            {
                materials[i] = NULL;
            }
        }
    }

    if (totalTris == 0)
        return mesh;

    /* allocate tri data: 0x50 (80) bytes per tri */
    triData = malloc((long long)totalTris * 0x50);
    if (!triData)
        Com_Printf("Out of memory for model collision triangles: %i tris (%i bytes)\n",
                    totalTris, totalTris * 0x50);

    /* allocate mins/maxs: 3 floats each * totalTris * 2 (mins + maxs) */
    triMins = (float *)malloc((long long)totalTris * 2 * 3 * sizeof(float));
    if (!triMins)
        Com_Printf("Out of memory for model collision triangle bounds: %i tris (%i bytes)\n",
                    totalTris, totalTris * 2 * 3 * (int)sizeof(float));

    triMaxs = triMins + totalTris * 3;

    /* second pass: build collision tris */
    triOffset = 0;
    for (i = 0; i < surfCount; i++)
    {
        if (!materials[i])
            continue;

        /* get surface with bone info */
        surface = DObjGetSurface(dobj, surfMap[i * 2], surfMap[i * 2 + 1], 0);

        /* copy indices (no offset) */
        R_XSurfaceCopyIndices(surface, indices, 0);

        R_XSurfaceDeformVerts(surface, boneMats, positions, texcoords, NULL);

        /* get tri count for this surface */
        {
            int numTris = XSurfaceGetNumTris(surface);
            int lastIdx = numTris * 3;

            /* check for degenerate last tri (last 2 indices equal) */
            if (indices[lastIdx - 3] == indices[lastIdx - 2])
                numTris--;

            if (numTris > 0)
            {
                CollisionTri_t *tri = (CollisionTri_t *)triData + triOffset;
                float *minsDst = triMins + triOffset * 3;
                float *maxsDst = triMaxs + triOffset * 3;
                unsigned short *idxPtr = indices;

                for (j = 0; j < numTris; j++)
                {
                    unsigned short idx0 = idxPtr[0];
                    unsigned short idx1 = idxPtr[1];
                    unsigned short idx2 = idxPtr[2];
                    float *v0 = &positions[idx0 * 3];
                    float *v1 = &positions[idx1 * 3];
                    float *v2 = &positions[idx2 * 3];

                    /* compute triangle plane: normal + dist */
                    PlaneFromPoints(tri->plane, v0, v1, v2);

                    /* compute barycentric coords */
                    ComputeBaryCoords(v0, v1, v2, tri->plane,
                                      tri->baryCoords0, tri->baryCoords1);

                    /* surface reference — LST sub_417DC0 loads r15 from the
                     * per-surface material array built in the first pass and
                     * stores it at offset 0x30 (tri->surfaceRef). Used by
                     * TraceModelCollision_r to check alpha mask. */
                    tri->surfaceRef = materials[i];

                    /* store texcoords for each vertex */
                    tri->texcoord0[0] = texcoords[idx0 * 2 + 0];
                    tri->texcoord0[1] = texcoords[idx0 * 2 + 1];
                    tri->texcoord1[0] = texcoords[idx1 * 2 + 0];
                    tri->texcoord1[1] = texcoords[idx1 * 2 + 1];
                    tri->texcoord2[0] = texcoords[idx2 * 2 + 0];
                    tri->texcoord2[1] = texcoords[idx2 * 2 + 1];

                    /* initialize bounds: mins = maxs = v0 position */
                    minsDst[0] = v0[0];
                    minsDst[1] = v0[1];
                    minsDst[2] = v0[2];
                    maxsDst[0] = v0[0];
                    maxsDst[1] = v0[1];
                    maxsDst[2] = v0[2];

                    /* expand bounds with v1 and v2 */
                    AddPointToBounds(v1, minsDst, maxsDst);
                    AddPointToBounds(v2, minsDst, maxsDst);

                    minsDst += 3;
                    maxsDst += 3;
                    tri++;
                    idxPtr += 3;
                }

                triOffset += numTris;
            }
        }
    }

    /* build AABB tree from collision tris */
    collTree = AddStaticModelCollision(triOffset, triData, triMins, triMaxs);
    mesh->collTree = collTree;

    /* free position/bounds buffer */
    free(triMins);

    return mesh;
}

/*
 * CM_TraceBox — trace a box against a static model's collision mesh.
 * Address: 0x4183D0 | Size: 941 bytes
 *
 * rcx=model (XModel*), rdx=scale (float[3]), r8=origin (float[3]),
 * r9=outCenter (float[3]), [rsp+arg_20]=outBoxHeight (float*)
 *
 * Finds or builds the collision mesh for the model, transforms the collision
 * bounds by the scale matrix, computes the center and box height.
 * If the model has no collision, copies origin to outCenter.
 */
extern void MatrixInverse(float *scaleMatrix, float *outInverse);                                  /* com_math_429600 */
/* CM_TraceBoundsTest — now in cm_tracebox.c */

void CM_TraceBox(void *model, float *scale, ModelPlacement_t *placement,
                 float *outCenter, float *outBoxHeight)
{
    CollisionMeshNode_t *mesh;
    float scaledMatrix[9];  /* 3x3 scale * rotation matrix */
    float absBounds[6];    /* mins[3] + maxs[3] contiguous */
    CollisionInstance_t *collisionInst;


    /* search collision mesh list for this model */
    mesh = g_collisionMeshList;
    while (mesh)
    {
        if (mesh->model == model)
            break;
        mesh = mesh->next;
    }

    /* build mesh if not found */
    if (!mesh)
    {
        mesh = BuildStaticModelCollisionMesh(model);
    }

    /* if mesh has no collision tree, return origin as center */
    if (!mesh || !mesh->collTree) {
        mesh = NULL;
    }

    if (!mesh)
    {
        outCenter[0] = placement->origin[0];
        outCenter[1] = placement->origin[1];
        outCenter[2] = placement->origin[2];
        return;
    }

    /* build scaled rotation matrix: each row scaled by corresponding scale factor */
    scaledMatrix[0] = scale[0] * placement->axis[0][0];
    scaledMatrix[1] = scale[0] * placement->axis[0][1];
    scaledMatrix[2] = scale[0] * placement->axis[0][2];
    scaledMatrix[3] = scale[1] * placement->axis[1][0];
    scaledMatrix[4] = scale[1] * placement->axis[1][1];
    scaledMatrix[5] = scale[1] * placement->axis[1][2];
    scaledMatrix[6] = scale[2] * placement->axis[2][0];
    scaledMatrix[7] = scale[2] * placement->axis[2][1];
    scaledMatrix[8] = scale[2] * placement->axis[2][2];

    /* compute absolute bounds from collision tree (GetRotatedBounds inlined) */
    GetRotatedBounds((float *)mesh->collTree, placement->origin, scaledMatrix, absBounds);

    /* compute center = (mins + maxs) * 0.5 */
    outCenter[0] = (absBounds[0] + absBounds[3]) * 0.5f;
    outCenter[1] = (absBounds[1] + absBounds[4]) * 0.5f;
    outCenter[2] = (absBounds[2] + absBounds[5]) * 0.5f;

    /* box height = diagonal length of absBounds (Vec3Distance mins..maxs) */
    *outBoxHeight = Vec3Distance(&absBounds[0], &absBounds[3]);

    /* assert: boxHeight >= 0 (line 0x165) */
    Assert(*outBoxHeight >= 0.0f, s_assertDisable_CM_TraceBox_boxHeight);

    /* skip collision instance setup if model shadows disabled (byte_480894) */
    if (!g_modelShadows) return;

    /* assert: bounds extents >= 0 for each axis (lines 0x16A-0x16C, gated on shadow flag) */
    Assert(absBounds[3] - absBounds[0] >= 0.0f, s_assertDisable_CM_TraceBox_boundsX);
    Assert(absBounds[4] - absBounds[1] >= 0.0f, s_assertDisable_CM_TraceBox_boundsY);
    Assert(absBounds[5] - absBounds[2] >= 0.0f, s_assertDisable_CM_TraceBox_boundsZ);

    /* allocate collision instance (0x68 = 104 bytes) and link into global list */
    collisionInst = (CollisionInstance_t *)malloc(sizeof(CollisionInstance_t));
    if (!collisionInst)
        Com_Error(1, "Out of memory on static model instance");

    /* link into g_collisionModelList */
    collisionInst->next = g_collisionModelList;
    g_collisionModelList = collisionInst;

    /* store origin */
    collisionInst->origin[0] = placement->origin[0];
    collisionInst->origin[1] = placement->origin[1];
    collisionInst->origin[2] = placement->origin[2];

    /* store mesh pointer */
    collisionInst->mesh = mesh;

    /* invert scaled matrix and store */
    MatrixInverse(scaledMatrix, collisionInst->invMatrix);

    /* store absolute bounds */
    collisionInst->absMins[0] = absBounds[0];
    collisionInst->absMins[1] = absBounds[1];
    collisionInst->absMins[2] = absBounds[2];
    collisionInst->absMaxs[0] = absBounds[3];
    collisionInst->absMaxs[1] = absBounds[4];
    collisionInst->absMaxs[2] = absBounds[5];
}

/*
 * TraceModelCollision_r — recursive collision trace against a CollisionAabbTree node.
 * Address: 0x418780 | Size: 450 bytes
 *
 * rcx=node (CollisionAabbTree_t*), rdx=traceData
 * Returns 1 (al) if hit, 0 if no hit.
 *
 * If node has children (childCount > 0), recurse into each child.
 * If node is a leaf (childCount == 0), test each tri (itemCount items, 0x50 stride)
 * against the trace box.
 */
extern int TestAlphaMask(void *collData, float *localTrace); /* geometry_40C070 */

int TraceModelCollision_r(CollisionAabbTree_t *node, float *traceData)
{
    int i;

    /* test trace box against node bounds */
    if (CM_TraceBoundsTest(traceData, node->mins, node->maxs, 1.0f))
        return 0;

    /* if node has children, recurse */
    if (node->childCount > 0)
    {
        for (i = 0; i < node->childCount; i++)
        {
            if (TraceModelCollision_r(&node->firstChild[i], traceData))
                return 1;
        }
        return 0;
    }

    /* leaf node: test each tri via SweepPointThroughModelTriangle (sub_40C440) */
    {
        CollisionTri_t *tri = node->data;
        for (i = 0; i < node->itemCount; i++)
        {
            float baryU;
            float baryV;
            if (SweepPointThroughModelTriangle(tri->plane, tri->baryCoords0,
                                               traceData, traceData + 3,
                                               &baryU, &baryV))
            {
                if (tri->surfaceRef && ((MaterialDef_t *)tri->surfaceRef)->extraData)
                {
                    /* compute interpolated texcoord at hit point */
                    float localTrace[2];
                    float baryW = 1.0f - baryU - baryV;

                    localTrace[0] = baryW * tri->texcoord0[0]
                                  + baryU * tri->texcoord1[0]
                                  + baryV * tri->texcoord2[0];
                    localTrace[1] = baryW * tri->texcoord0[1]
                                  + baryU * tri->texcoord1[1]
                                  + baryV * tri->texcoord2[1];

                    if (!TestAlphaMask(tri->surfaceRef, localTrace))
                        goto next_tri;
                }
                return 1;
            }
next_tri:
            tri++;
        }
    }
    return 0;
}

/*
 * TraceModelCollision_Walk — walk the collision model linked list and trace.
 * Address: 0x418950 | Size: 293 bytes
 *
 * rcx=modelInst (collision instance, linked list node), rdx=traceData
 * Returns 1 (al) if any model hit, 0 if none.
 *
 * For each collision model instance in the linked list:
 *   1. Test trace box against instance bounds (+0x48..+0x54)
 *   2. Transform trace into model-local space using inverse matrix (+0x24)
 *   3. Call TraceModelCollision_r on the collision tree
 */
/* CM_CalcTraceExtents and CM_TraceBoundsTest now in cm_tracebox.c */

int TraceModelCollision_Walk(CollisionInstance_t *inst, float *traceData)
{
    float localPos[3];
    float localBounds[9];

    if (!inst)
        return 0;

    do
    {
        /* test trace box against instance bounds */
        if (CM_TraceBoundsTest(traceData, inst->absMins, inst->absMaxs, 1.0f))
            goto next_inst;

        /* transform trace mins into model-local space */
        localPos[0] = traceData[0] - inst->origin[0];
        localPos[1] = traceData[1] - inst->origin[1];
        localPos[2] = traceData[2] - inst->origin[2];
        MatrixTransformVector3(localPos, inst->invMatrix, &localBounds[0]);

        /* transform trace maxs into model-local space */
        localPos[0] = traceData[3] - inst->origin[0];
        localPos[1] = traceData[4] - inst->origin[1];
        localPos[2] = traceData[5] - inst->origin[2];
        MatrixTransformVector3(localPos, inst->invMatrix, &localBounds[3]);

        /* setup and trace */
        CM_CalcTraceExtents(localBounds);
        if (TraceModelCollision_r(inst->mesh->collTree, localBounds))
            return 1;

next_inst:
        inst = inst->next;
    } while (inst);

    return 0;
}

/*
 * TraceModelCollision_Node — trace through BSP subdivision tree node.
 * Address: 0x418A80 | Size: 649 bytes
 *
 * ecx=nodeIndex, rdx=start (float[3]), r8=end (float[3])
 * Returns 1 (al) if hit, 0 if no hit.
 *
 * Walks the BSP subdivision tree. At each node:
 *   1. Walk the collision model list for that node
 *   2. If both start and end are on the same side of the split, recurse into that child
 *   3. If they straddle the split, compute the intersection point, recurse near side first,
 *      then far side
 */
int TraceModelCollision_Node(int nodeIndex, float *start, float *end)
{
    float localBounds[9];
    float splitPos;
    int splitAxis;
    int childIndex;
    BSPSubdivNode_t *node;
    float startVal, endVal;
    float t;
    float mid[3];

    /* early out: if both start and end Z are above max collision radius, no hit */
    if (start[2] >= g_maxCollisionRadius && end[2] >= g_maxCollisionRadius)
        return 0;

    /* copy start/end into local bounds */
    localBounds[0] = start[0];
    localBounds[1] = start[1];
    localBounds[2] = start[2];
    localBounds[3] = end[0];
    localBounds[4] = end[1];
    localBounds[5] = end[2];

    /* setup trace */
    CM_CalcTraceExtents(localBounds);

    /* walk BSP tree */
    childIndex = nodeIndex;
    node = &g_bspSubdivNodes[nodeIndex];

    /* if this node has collision models, trace them */
    while (node->childCount > 0)
    {
        /* walk model list at this node */
        if (TraceModelCollision_Walk(node->list, localBounds))
            return 1;

        /* check if children exist (depth limit) */
        if (childIndex * 2 + 2 >= 0x3FF)
            return 0;

        splitAxis = node->splitAxis;
        splitPos = node->splitPos;
        startVal = start[splitAxis];
        endVal = end[splitAxis];

        /* both on left side */
        if (splitPos >= endVal && splitPos >= startVal)
        {
            childIndex = childIndex * 2 + 1;
            node = &g_bspSubdivNodes[childIndex];
            continue;
        }

        /* both on right side */
        if (startVal >= splitPos && endVal >= splitPos)
        {
            childIndex = childIndex * 2 + 2;
            node = &g_bspSubdivNodes[childIndex];
            continue;
        }

        /* straddle: assert end != start on split axis (line 0x1E5) */
        Assert("end[node->axis] - start[node->axis] != 0",
               ".\\modelcollision.cpp", 0x1E5, 1, 1);

        /* compute intersection t and midpoint */
        t = (splitPos - startVal) / (endVal - startVal);
        mid[0] = (end[0] - start[0]) * t + start[0];
        mid[1] = (end[1] - start[1]) * t + start[1];
        mid[2] = (end[2] - start[2]) * t + start[2];
        mid[splitAxis] = splitPos;

        /* recurse near side first, then far side */
        childIndex = childIndex * 2;
        if (splitPos > startVal)
        {
            /* start is on left side — trace left first */
            if (TraceModelCollision_Node(childIndex + 1, start, mid))
                return 1;
            return TraceModelCollision_Node(childIndex + 2, mid, end);
        }
        else
        {
            /* start is on right side — trace right first */
            if (TraceModelCollision_Node(childIndex + 2, start, mid))
                return 1;
            return TraceModelCollision_Node(childIndex + 1, mid, end);
        }
    }

    /* leaf node: no children */
    return 0;
}

extern int g_useEmbree;
extern int TraceStaticModels_Embree(float *start, float *end);

int TraceStaticModels(float *start, float *end)
{
    if (g_useEmbree)
        return TraceStaticModels_Embree(start, end);
    return TraceModelCollision_Node(0, start, end);
}

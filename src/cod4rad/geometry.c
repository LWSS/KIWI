/*
 * geometry.c — Triangle_t geometry, collision BSP, and lighting sample setup.
 */

#include "cod2rad64.h"
extern char *AssertFormat(const char *fmt, const char *expr, ...);
extern void ComputeBaryCoords(float *v0, float *v1, float *v2,
                              float *plane, float *out0, float *out1);
extern void MatrixTransformPoint(void *texcoordData,
                                   DrawVert_t *vertData, float *pos);
extern void Lighting_RegisterLightmap(int lightmapIdx);
extern void memcpy_fast(void *dst, const void *src, int size);
#include <stdlib.h>
extern void *Hunk_AllocateTempMemory(int bytes);
extern void Hunk_FreeTempMemory(void *ptr);
extern void *LocalAlloc_wrapper(int bytes);
extern void *HeapReAlloc_wrapper(void *ptr, int bytes);
extern void ForEachQuantum(int count, void (*callback)(int, int), int threadCount);
extern void BuildLightTransfers_PerTri(int triIndex, int edx, int count, void *callback);
extern void GetLightingSubSample(int lightmapIdx, float u, float v, void *result);
extern void ProcessLightingSampleArea(float areaX2, float *centroid,
                                      float *polyVerts, int vertCount,
                                      void *userData, int areaIndex);
extern int CompareFunction(const void *a, const void *b);
extern void qsort_wrapper(void *base, __int64 num, int size,
                           int (*cmp)(const void *, const void *));
extern float ceilf_wrapper(float x);
extern void ForEach2dArea(void *coords, int vertCount,
                          int sWidth, int tHeight,
                          float sStart, float tStart,
                          float sPixelSize, float tPixelSize,
                          ForEach2dAreaCallback callback, void *userData);
extern int FindLightingTransfers_inner(int sampleIdx, float *position, float *normal,
                                       float subAreaFactor, float skyFactor,
                                       unsigned char primaryLightIndex,
                                       void *subSample);
extern int ComputeLinearMappingForTriangle(Triangle_t *tri, void *outMapping);
extern void MatrixTransformDirection(void *lightmapMatrix, void *vertData, void *output);
extern int SetupLinearMapping(float *basis, float *baryCoords, float *outCoords,
                              float *outAxes, float *outMapping);
extern void OrientationDirToWorldDir(float *mapping, float xmm1, float xmm2,
                                  float xmm3, float *output);     /* linearmapping_4152C0 */

/* g_bspPlanes / g_bspFileNodes are same arrays as bspPlanes / bspNodes in bspfile.c */
#define g_bspPlanes ((float(*)[4])bspPlanes)
#define g_bspFileNodes ((BspFileNode_t *)bspNodes)
float         g_worldMins[3];
float         g_worldMaxs[3];
float         g_modelBoundsData[MAX_MAP_MODELS][MODEL_BOUNDS_FLOATS];
/* g_materialData is the same array as bspMaterials (same address in binary) */
#define g_materialData ((char(*)[MATERIAL_DATA_STRIDE])bspMaterials)

int                 g_triCount;
Triangle_t          g_triangles[MAX_RAD_TRIANGLES];
int                 g_vertCount;
float              g_vertPositions[MAX_MAP_VERTEXES][3];
/* g_vertData is the same array as bspDrawVerts (same address in binary) */
#define g_vertData ((DrawVert_t *)bspDrawVerts)
/* g_drawIndices is the same array as bspDrawIndexes (same address in binary) */
#define g_drawIndices bspDrawIndexes
#define g_numBSPNodes numBSPNodes
int                 g_bspNodeCount;
BSPCollisionNode_t *g_bspNodeData;
int                 g_bspNodeAlloc;
int                 g_bspLeafCount;
BSPCollisionLeaf_t *g_bspLeafData;
Triangle_t        **g_triPointerArray;
int                 g_bspRefCount;
int                 g_visCache[4];
/* g_lightTransferCount is the SAME variable as g_superSample in cmdline.c */
#define g_lightTransferCount g_superSample
extern int g_superSample;
int                 g_totalLightmapPixels;
static int         *g_triTexelCount;
static int         *g_triWorkOrder;

static int g_lightmapPixelParam;

/* Embree ray tracing (implementation in embree_trace.cpp) */
extern int g_useEmbree;
static void *g_lightmapPixelCallback;
static float *g_bspSortMins;
static float *g_bspSortMaxs;
static float *g_bspSortEqual;

static char s_assertDisable_AddTriangle_vertIndex;
static char s_assertDisable_AddTriangle_or;
static char s_assertDisable_AddTrianglesForSurface_modelIndex;
static char s_assertDisable_AddInvisibleOpaqueTriangle_si;
static char s_assertDisable_TestAlphaMask_width;
static char s_assertDisable_TestAlphaMask_height;
static char s_assertDisable_Partition_triRefs;
static char s_assertDisable_Partition_triCount;
static char s_assertDisable_Partition_leafCount;
static char s_assertDisable_ClassifySide;
static char s_assertDisable_SetupBSP_nodes;
static char s_assertDisable_SetupBSP_dnode;
static char s_assertDisable_SetupBSP_bspNode;
static char s_assertDisable_ForEach_boundsU;
static char s_assertDisable_ForEach_boundsV;
static char s_assertDisable_ForEach_sBase;
static char s_assertDisable_ForEach_tBase;
static char s_assertDisable_ForEach_sEnd;
static char s_assertDisable_ForEach_tEnd;
static char s_assertDisable_Process_area;
static char s_assertDisable_Process_userData;
static char s_assertDisable_Process_nanC0;
static char s_assertDisable_Process_nanC1;
static char s_assertDisable_Process_nanB0pre;
static char s_assertDisable_Process_nanB1pre;
static char s_assertDisable_Process_nanB2pre;
static char s_assertDisable_Process_nanB0post;
static char s_assertDisable_Process_nanB1post;
static char s_assertDisable_Process_nanB2post;

typedef struct RadiosityColorMapping_s {
    Triangle_t *tri;
    float uCoeff[3];
    float vCoeff[3];
} RadiosityColorMapping_t;

typedef struct RadiosityColorAccum_s {
    float area;
    float rgb[3];
    MaterialDef_t *material;
} RadiosityColorAccum_t;

static void BuildRadiosityColor_PerTri(int triIndex);

/*
================
BuildTriangleNormal

Computes triangle normal, plane distance, bary coords, and area.
Returns area (half cross product magnitude), or 0 if degenerate.
================
*/
float BuildTriangleNormal(Triangle_t *tri)
{
    float *v0, *v1, *v2;
    float edge1[3], edge2[3];
    float length;

    v0 = g_vertPositions[tri->vertIndex[0]];
    v1 = g_vertPositions[tri->vertIndex[1]];
    v2 = g_vertPositions[tri->vertIndex[2]];

    /* edge1 = v1 - v0 */
    edge1[0] = v1[0] - v0[0];
    edge1[1] = v1[1] - v0[1];
    edge1[2] = v1[2] - v0[2];

    /* edge2 = v2 - v0 */
    edge2[0] = v2[0] - v0[0];
    edge2[1] = v2[1] - v0[1];
    edge2[2] = v2[2] - v0[2];

    /* normal = cross(edge2, edge1) */
    Vec3Cross(edge2, edge1, tri->normal);

    /* normalize and get length */
    length = Vec3Normalize(tri->normal);

    if (length == 0.0f)
        return 0.0f;

    /* plane distance = dot(normal, v0) */
    tri->dist = tri->normal[1] * v0[1] + v0[0] * tri->normal[0] + tri->normal[2] * v0[2];

    /* compute barycentric coordinate vectors */
    ComputeBaryCoords(v0, v1, v2, tri->normal, tri->baryVec0, tri->baryVec1);

    /* area = half the cross product magnitude */
    return length * 0.5f;
}

/*
================
AddTriangle

Adds a triangle from a surface. Looks up vertex indices,
copies vertex data, builds the triangle normal.
Only increments g_triCount if area > 0.
================
*/
void AddTriangle(SurfaceInfo_t *surface, unsigned short materialIdx,
                 void *texcoordData, void *material,
                 int initialTriCount, int triIndex,
                 unsigned char primaryLightIndex)
{
    Triangle_t *tri;
    unsigned short *indices;
    int i;
    int vertIndex;
    float area;

    tri = &g_triangles[g_triCount];
    tri->material = material;
    tri->materialIdx = materialIdx;
    tri->lightmapIdx = surface->lightmapIdx;
    tri->primaryLightIndex = primaryLightIndex;

    if (surface->lightmapIdx != 0x1F)
        Lighting_RegisterLightmap((int)surface->lightmapIdx);

    indices = &g_drawIndices[surface->indicesOffset + triIndex * 3];

    for (i = 0; i < 3; i++)
    {
        vertIndex = indices[i] + surface->vertBase;
        tri->vertIndex[i] = vertIndex;

        Assert(vertIndex >= 0 && vertIndex < numBSPDrawVerts, s_assertDisable_AddTriangle_vertIndex);
        Assert(texcoordData || 0, s_assertDisable_AddTriangle_or);

        MatrixTransformPoint(texcoordData, &g_vertData[vertIndex], g_vertPositions[vertIndex]);
    }

    area = BuildTriangleNormal(tri);

    /* only count triangle if it has non-zero area */
    if (area != 0.0f)
        g_triCount++;
}

/*
================
AddTrianglesForSurface

Adds all triangles for a surface. Divides index count by 3
to get triangle count, looks up material, then loops.
================
*/
void AddTrianglesForSurface(SurfaceInfo_t *surface, int modelIndex, void *texcoordData)
{
    int triCount;
    int i;
    int surfaceIndex;
    unsigned char primaryLightIndex;
    void *material;

    Assert(modelIndex >= 0 && modelIndex < 0x3FF, s_assertDisable_AddTrianglesForSurface_modelIndex);

    /* copy 48 bytes of texcoord data to model bounds array */
    memcpy_fast(g_modelBoundsData[modelIndex], texcoordData, sizeof(g_modelBoundsData[0]));

    /* look up material from surface material reference */
    material = LoadMaterial(g_materialData[surface->materialRef]);

    triCount = (int)surface->indexCount / 3;
    surfaceIndex = (int)((BspTriSoup_t *)surface - bspTriangles);
    primaryLightIndex = surfaceIndex >= 0 && surfaceIndex < numBSPTriSoups
        ? bspTriSoupPrimaryLightIndices[surfaceIndex]
        : 0;

    for (i = 0; i < triCount; i++)
    {
        AddTriangle(surface, modelIndex, texcoordData, material, g_triCount, i,
                    primaryLightIndex);
    }
}

/*
================
InitGeometry_AddVerts

Adds a vertex, deduplicating against recent vertices.
Searches backward through the last 2000 verts for a match.
Returns the vertex index (existing or newly allocated).
================
*/
int InitGeometry_AddVerts(float *position, float *texcoord)
{
    int vertCount;
    int startIdx;
    int searchIdx;
    int newIdx;

    vertCount = g_vertCount;
    startIdx = numBSPDrawVerts;

    /* search at most 2000 vertices back */
    if (vertCount - 2000 > startIdx)
        startIdx = vertCount - 2000;

    /* search backward for duplicate vertex */
    for (searchIdx = vertCount - 1; searchIdx >= startIdx; searchIdx--)
    {
        if (position[0] == g_vertPositions[searchIdx][0]
            && position[1] == g_vertPositions[searchIdx][1]
            && position[2] == g_vertPositions[searchIdx][2]
            && texcoord[0] == g_vertData[searchIdx].texCoord[0]
            && texcoord[1] == g_vertData[searchIdx].texCoord[1])
        {
            return searchIdx;
        }
    }

    /* check MAX_MAP_DRAW_VERTS */
    if (vertCount == 0x80000)
    {
        ErrorMsg( "MAX_MAP_DRAW_VERTS (%i) exceeded.  The BSP has %i and cod2rad is trying to add %i more.\n",
                  vertCount, numBSPDrawVerts, vertCount - numBSPDrawVerts);
        vertCount = g_vertCount; /* re-read after error */
    }

    /* check MAX_MAP_TRIANGLES */
    if (g_triCount == 0x100000)
    {
        ErrorMsg( "MAX_MAP_TRIANGLES (%i) exceeded.  The BSP has %i and cod2rad is trying to add %i more.\n",
                  0x100000, 0x100000, 0);
        vertCount = g_vertCount;
    }

    /* allocate new vertex */
    newIdx = vertCount;
    g_vertCount = vertCount + 1;

    /* copy position to vertex positions array */
    g_vertPositions[newIdx][0] = position[0];
    g_vertPositions[newIdx][1] = position[1];
    g_vertPositions[newIdx][2] = position[2];

    /* initialize vertex data with defaults */
    g_vertData[newIdx].pos[0] = position[0];
    g_vertData[newIdx].pos[1] = position[1];
    g_vertData[newIdx].pos[2] = position[2];
    g_vertData[newIdx].normal[0] = 0.0f;
    g_vertData[newIdx].normal[1] = 0.0f;
    g_vertData[newIdx].normal[2] = 1.0f;
    g_vertData[newIdx].color[0] = 0xFF;
    g_vertData[newIdx].color[1] = 0xFF;
    g_vertData[newIdx].color[2] = 0xFF;
    g_vertData[newIdx].color[3] = 0xFF;
    g_vertData[newIdx].texCoord[0] = texcoord[0];
    g_vertData[newIdx].texCoord[1] = texcoord[1];
    g_vertData[newIdx].lmCoord[0] = 0.0f;
    g_vertData[newIdx].lmCoord[1] = 0.0f;
    g_vertData[newIdx].tangent[0] = 1.0f;
    g_vertData[newIdx].tangent[1] = 0.0f;
    g_vertData[newIdx].tangent[2] = 0.0f;
    g_vertData[newIdx].binormal[0] = 0.0f;
    g_vertData[newIdx].binormal[1] = 1.0f;
    g_vertData[newIdx].binormal[2] = 0.0f;

    return newIdx;
}

/*
================
AddInvisibleOpaqueTriangle

Adds a triangle for an invisible/opaque surface.
Checks material flags before adding. Each vertex is added
via InitGeometry_AddVerts with separate position/texcoord pairs.
================
*/
void AddInvisibleOpaqueTriangle(MaterialDef_t *si, float *pos1, float *pos2,
                                float *pos3, float *tc1, float *tc2, float *tc3)
{
    Triangle_t *tri;
    float area;

    Assert(si, s_assertDisable_AddInvisibleOpaqueTriangle_si);

    /* check if material should produce collision geometry */
    if (si->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY))
        goto add_tri;

    if (si->contents & CONTENTS_TELEPORTER)
        return;

    if ((si->surfaceType & SURFTYPE_MASK) == SURFTYPE_PATCH)
        goto add_tri;

    if (!si->extraData)
        return;

add_tri:
    tri = &g_triangles[g_triCount];

    tri->vertIndex[0] = InitGeometry_AddVerts(pos1, tc1);
    tri->vertIndex[1] = InitGeometry_AddVerts(pos2, tc2);
    tri->vertIndex[2] = InitGeometry_AddVerts(pos3, tc3);

    tri->material = si;
    tri->materialIdx = 0;
    tri->lightmapIdx = 0x1F;

    area = BuildTriangleNormal(tri);

    if (area > 0.0f)
        g_triCount++;
}

/*
================
TestAlphaMask

Tests if a texcoord hits an opaque pixel in the material's alpha mask.
Returns 1 if the pixel is set (opaque), 0 if transparent.
================
*/
int TestAlphaMask(MaterialDef_t *si, float *texcoord)
{
    int width, height;
    int u, v;
    int pixelIndex;
    int byteIndex;
    int bitIndex;

    width = si->width;
    Assert((width & (width - 1)) == 0, s_assertDisable_TestAlphaMask_width);

    height = si->height;
    Assert((height & (height - 1)) == 0, s_assertDisable_TestAlphaMask_height);

    /* compute V: floor(height * texcoord[1] + 0.5) wrapped to [0, height-1] */
    v = (int)floorf((float)height * texcoord[1] + 0.5f);
    v = v & (height - 1);
    v = v * width;

    /* compute U: floor(width * texcoord[0] + 0.5) wrapped to [0, width-1] */
    u = (int)floorf((float)width * texcoord[0] + 0.5f);
    u = u & (width - 1);

    pixelIndex = v + u;
    byteIndex = pixelIndex >> 3;
    bitIndex = pixelIndex & 7;

    return (((unsigned char *)si->extraData)[byteIndex] & (1 << bitIndex)) != 0;
}

/*
================
InitGeometry_Reset

Resets triangle count and sets vertex base to disk vert count.
================
*/
void InitGeometry_Reset(void)
{
    g_vertCount = numBSPDrawVerts;
    g_triCount = 0;
}

/*
================
RayTriangleIntersect

Tests a ray against a triangle. Uses vis cache to skip
already-tested triangles. Computes barycentric coords
and optional alpha mask test. Updates hitResult if closer.
================
*/
void RayTriangleIntersect(RayTraceContext_t *ray, Triangle_t *tri)
{
    int idx;
    float directionDotNormal;
    float facingSign;
    float startNumerator;
    float fraction;
    float fromStart[3];
    float rayCross[3];
    float edge01[3];
    float edge02[3];
    float edgeCross[3];
    float edge01Test;
    float edge02Test;
    float denominator;
    float baryU, baryV;
    float tc[2];
    RayHitResult_t *hit;
    const float *vert0;
    const float *vert1;
    const float *vert2;

    /* vis cache dedup */
    idx = ray->cacheIndex;
    if (tri->cacheStamp[idx] == g_visCache[idx])
        return;
    tri->cacheStamp[idx] = g_visCache[idx];

    /* compute signed distances to triangle plane */
    directionDotNormal = ray->delta[0] * tri->normal[0]
                       + ray->delta[1] * tri->normal[1]
                       + ray->delta[2] * tri->normal[2];
    facingSign = 1.0f;
    if (directionDotNormal >= 0.0f)
    {
        directionDotNormal = -directionDotNormal;
        facingSign = -1.0f;
    }

    /* Native edge/cross-product intersection uses vertex 0 as its origin. */
    vert0 = g_vertData[tri->vertIndex[0]].pos;
    vert1 = g_vertData[tri->vertIndex[1]].pos;
    vert2 = g_vertData[tri->vertIndex[2]].pos;
    fromStart[0] = vert0[0] - ray->start[0];
    fromStart[1] = vert0[1] - ray->start[1];
    fromStart[2] = vert0[2] - ray->start[2];
    startNumerator = (fromStart[0] * tri->normal[0]
                    + fromStart[1] * tri->normal[1]
                    + fromStart[2] * tri->normal[2]) * facingSign;
    if (startNumerator > 0.0f
     || directionDotNormal * ray->hitResult->fraction >= startNumerator)
        return;

    rayCross[0] = ray->delta[1] * fromStart[2]
                - ray->delta[2] * fromStart[1];
    rayCross[1] = ray->delta[2] * fromStart[0]
                - ray->delta[0] * fromStart[2];
    rayCross[2] = ray->delta[0] * fromStart[1]
                - ray->delta[1] * fromStart[0];

    edge01[0] = vert0[0] - vert1[0];
    edge01[1] = vert0[1] - vert1[1];
    edge01[2] = vert0[2] - vert1[2];
    edge01Test = (rayCross[0] * edge01[0]
                + rayCross[1] * edge01[1]
                + rayCross[2] * edge01[2]) * facingSign;
    if (edge01Test > 0.0f)
        return;

    edge02[0] = vert0[0] - vert2[0];
    edge02[1] = vert0[1] - vert2[1];
    edge02[2] = vert0[2] - vert2[2];
    edge02Test = (rayCross[0] * edge02[0]
                + rayCross[1] * edge02[1]
                + rayCross[2] * edge02[2]) * facingSign;
    if (edge02Test < 0.0f)
        return;

    edgeCross[0] = edge02[1] * edge01[2] - edge02[2] * edge01[1];
    edgeCross[1] = edge02[2] * edge01[0] - edge02[0] * edge01[2];
    edgeCross[2] = edge02[0] * edge01[1] - edge02[1] * edge01[0];
    denominator = (ray->delta[0] * edgeCross[0]
                 + ray->delta[1] * edgeCross[1]
                 + ray->delta[2] * edgeCross[2]) * facingSign;
    if (denominator > edge01Test - edge02Test)
        return;

    fraction = startNumerator / directionDotNormal;
    baryU = -edge02Test / denominator;
    baryV = edge01Test / denominator;

    /* alpha mask test if material has alpha data */
    if ((tri->material)->extraData)
    {
        float baryW = 1.0f - baryU - baryV;

        /* interpolate texcoords using barycentric weights */
        tc[0] = baryW * g_vertData[tri->vertIndex[0]].texCoord[0]
              + baryU * g_vertData[tri->vertIndex[1]].texCoord[0]
              + baryV * g_vertData[tri->vertIndex[2]].texCoord[0];
        tc[1] = baryW * g_vertData[tri->vertIndex[0]].texCoord[1]
              + baryU * g_vertData[tri->vertIndex[1]].texCoord[1]
              + baryV * g_vertData[tri->vertIndex[2]].texCoord[1];

        if (!TestAlphaMask(tri->material, tc))
            return;
    }

    /* update hit result */
    hit = ray->hitResult;
    hit->triangle = tri;
    hit->baryU = baryU;
    hit->baryV = baryV;
    hit->fraction = fraction;
}

/*
================
SweepPointThroughModelTriangle

Tests if a point moving from start to end intersects a triangle
defined by plane + barycentric basis. Outputs baryU/baryV.
Returns 1 on hit, 0 on miss.
================
*/
int SweepPointThroughModelTriangle(float *plane, float *baryData,
                                   float *start, float *end,
                                   float *outBaryU, float *outBaryV)
{
    float startDist, endDist;
    float fraction;
    float dirX, dirY, dirZ;
    float hitX, hitY, hitZ;
    float baryU, baryV;

    /* compute signed distances to plane */
    endDist = end[0] * plane[0] + end[1] * plane[1] + end[2] * plane[2] - plane[3];
    startDist = start[0] * plane[0] + start[1] * plane[1] + start[2] * plane[2] - plane[3];

    /* both on same side — no intersection */
    if (startDist * endDist > 0.0f)
        return 0;
    if (startDist == endDist)
        return 0;

    /* direction = end - start */
    dirX = end[0] - start[0];
    dirY = end[1] - start[1];
    dirZ = end[2] - start[2];

    /* fraction */
    fraction = startDist / (startDist - endDist);

    /* hit point = start + fraction * direction */
    hitX = dirX * fraction + start[0];
    hitY = dirY * fraction + start[1];
    hitZ = dirZ * fraction + start[2];

    /* barycentric U */
    baryU = hitX * baryData[0] + hitY * baryData[1] + hitZ * baryData[2] - baryData[3];
    if (0.0f > baryU)
        return 0;
    if (baryU > 1.0f)
        return 0;

    /* barycentric V */
    baryV = hitX * baryData[4] + hitY * baryData[5] + hitZ * baryData[6] - baryData[7];
    if (0.0f > baryV)
        return 0;
    if (baryU + baryV > 1.0f)
        return 0;

    *outBaryU = baryU;
    *outBaryV = baryV;
    return 1;
}

/*
================
TraceShadowBatched

Tests a ray against a batch of triangles for shadow occlusion.
Returns 0 if any triangle blocks the ray (shadow),
returns 1 if the ray passes through all triangles (lit).
================
*/
int TraceShadowBatched(RayTraceContext_t *ray, Triangle_t **triArray, int triCount)
{
    int i;
    int idx;
    float startDist, endDist;
    float fraction;
    float dirX, dirY, dirZ;
    float hitX, hitY, hitZ;
    float baryU, baryV;
    float tc[2];
    Triangle_t *tri;

    for (i = 0; i < triCount; i++)
    {
        tri = triArray[i];

        /* vis cache dedup */
        idx = ray->cacheIndex;
        if (tri->cacheStamp[idx] == g_visCache[idx])
            continue;
        tri->cacheStamp[idx] = g_visCache[idx];

        /* signed distances to triangle plane */
        endDist = ray->end[0] * tri->normal[0]
                + ray->end[1] * tri->normal[1]
                + ray->end[2] * tri->normal[2]
                - tri->dist;

        startDist = ray->start[1] * tri->normal[1]
                  + ray->start[0] * tri->normal[0]
                  + ray->start[2] * tri->normal[2]
                  - tri->dist;

        /* both same side — no intersection */
        if (startDist * endDist > 0.0f)
            continue;
        if (startDist == endDist)
            continue;

        /* direction = end - start */
        dirY = ray->end[1] - ray->start[1];
        dirZ = ray->end[2] - ray->start[2];
        dirX = ray->end[0] - ray->start[0];

        /* fraction */
        fraction = startDist / (startDist - endDist);

        /* hit point */
        hitY = dirY * fraction + ray->start[1];
        hitZ = dirZ * fraction + ray->start[2];
        hitX = dirX * fraction + ray->start[0];

        /* bary U */
        baryU = hitX * tri->baryVec0[0] + hitY * tri->baryVec0[1]
              + hitZ * tri->baryVec0[2] - tri->baryVec0[3];
        if (0.0f > baryU)
            continue;
        if (baryU > 1.0f)
            continue;

        /* bary V */
        baryV = hitX * tri->baryVec1[0] + hitY * tri->baryVec1[1]
              + hitZ * tri->baryVec1[2] - tri->baryVec1[3];
        if (0.0f > baryV)
            continue;
        if (baryU + baryV > 1.0f)
            continue;

        /* alpha mask check */
        if (!(tri->material)->extraData)
            return 0; /* no alpha -> hit confirmed -> blocked */

        {
            float baryW = 1.0f - baryU - baryV;

            tc[0] = baryW * g_vertData[tri->vertIndex[0]].texCoord[0]
                  + baryU * g_vertData[tri->vertIndex[1]].texCoord[0]
                  + baryV * g_vertData[tri->vertIndex[2]].texCoord[0];
            tc[1] = baryW * g_vertData[tri->vertIndex[0]].texCoord[1]
                  + baryU * g_vertData[tri->vertIndex[1]].texCoord[1]
                  + baryV * g_vertData[tri->vertIndex[2]].texCoord[1];

            if (TestAlphaMask(tri->material, tc))
                return 0; /* opaque pixel -> blocked */
        }
    }

    return 1; /* no blocking triangle found */
}

/*
================
TraceBSP_r

Recursive BSP tree traversal for ray intersection.
Walks nodes, at leaves calls RayTriangleIntersect per triangle.
When ray crosses a split plane, recurses both sides.
================
*/
void TraceBSP_r(int nodeIndex, float *endPos, float *startPos, RayTraceContext_t *ray)
{
    BSPCollisionNode_t *node;
    float startDist, endDist;
    int side;
    float splitPoint[3];
    float invDenom;

    if (nodeIndex < 0)
        goto leaf;

    /* load start and end positions (cached in registers across the loop) */
    while (nodeIndex >= 0)
    {
        node = &g_bspNodeData[nodeIndex];

        /* dot products against split plane */
        endDist = endPos[0] * node->normal[0] + endPos[1] * node->normal[1]
                + endPos[2] * node->normal[2] - node->dist;

        startDist = startPos[0] * node->normal[0] + startPos[1] * node->normal[1]
                  + startPos[2] * node->normal[2] - node->dist;

        /* both on same side — pick child and continue */
        if (startDist * endDist < 0.0f)
            goto split;

        /* determine which side both points are on */
        if (startDist + endDist > 0.0f)
            side = 0;
        else
            side = 1;

        nodeIndex = node->child[side];
    }

leaf:
    if (nodeIndex == -1)
        return; /* empty leaf */

    {
        int leafIdx = -2 - nodeIndex;
        int triCount = g_bspLeafData[leafIdx].triCount;
        int triOffset = g_bspLeafData[leafIdx].triOffset;
        Triangle_t **triList = &g_triPointerArray[triOffset];
        int i;

        for (i = 0; i < triCount; i++)
            RayTriangleIntersect(ray, triList[i]);
    }
    return;

split:
    /* compute split point: (start*endDist - end*startDist) / (endDist - startDist) */
    invDenom = 1.0f / (endDist - startDist);
    splitPoint[0] = (startPos[0] * endDist - endPos[0] * startDist) * invDenom;
    splitPoint[1] = (startPos[1] * endDist - endPos[1] * startDist) * invDenom;
    splitPoint[2] = (startPos[2] * endDist - endPos[2] * startDist) * invDenom;

    /* determine which side the end point is on */
    if (endDist > 0.0f)
        side = 0;
    else
        side = 1;

    /* recurse near side (end's side) with (splitPoint -> end) */
    TraceBSP_r(node->child[side], endPos, splitPoint, ray);

    /* recurse far side (start's side) with (start -> splitPoint) */
    TraceBSP_r(node->child[1 - side], splitPoint, startPos, ray);
}

/*
================
TraceVisibility_r

Recursive BSP tree traversal for shadow/visibility.
Same structure as TraceBSP_r but calls TraceShadowBatched
at leaves and early-exits on any blocking hit.
Returns 1 if visible (no block), 0 if blocked.
================
*/
int TraceVisibility_r(int nodeIndex, float *endPos, float *startPos, RayTraceContext_t *ray)
{
    BSPCollisionNode_t *node;
    float startDist, endDist;
    int side;
    float splitPoint[3];
    float invDenom;

    if (nodeIndex < 0)
        goto leaf;

    while (nodeIndex >= 0)
    {
        node = &g_bspNodeData[nodeIndex];

        endDist = endPos[0] * node->normal[0] + endPos[1] * node->normal[1]
                + endPos[2] * node->normal[2] - node->dist;

        startDist = startPos[0] * node->normal[0] + startPos[1] * node->normal[1]
                  + startPos[2] * node->normal[2] - node->dist;

        if (startDist * endDist < 0.0f)
            goto split;

        if (startDist + endDist > 0.0f)
            side = 0;
        else
            side = 1;

        nodeIndex = node->child[side];
    }

leaf:
    if (nodeIndex == -1)
        return 1; /* empty leaf — visible */

    {
        int leafIdx = -2 - nodeIndex;
        int triCount = g_bspLeafData[leafIdx].triCount;
        int triOffset = g_bspLeafData[leafIdx].triOffset;
        Triangle_t **triList = &g_triPointerArray[triOffset];

        return TraceShadowBatched(ray, triList, triCount);
    }

split:
    invDenom = 1.0f / (endDist - startDist);
    splitPoint[0] = (startPos[0] * endDist - endPos[0] * startDist) * invDenom;
    splitPoint[1] = (startPos[1] * endDist - endPos[1] * startDist) * invDenom;
    splitPoint[2] = (startPos[2] * endDist - endPos[2] * startDist) * invDenom;

    if (endDist > 0.0f)
        side = 0;
    else
        side = 1;

    /* recurse near side — if blocked, return immediately */
    if (!TraceVisibility_r(node->child[side], endPos, splitPoint, ray))
        return 0;

    /* recurse far side */
    return TraceVisibility_r(node->child[1 - side], splitPoint, startPos, ray);
}

/*
================
TraceSetup_and_Dispatch

Sets up a RayTraceContext_t on the stack and dispatches TraceBSP_r
to find the closest ray-triangle intersection.
================
*/
void TraceSetup_and_Dispatch(int cacheIndex, float *startPos, float *endPos, RayHitResult_t *hitResult)
{
    if (g_useEmbree)
    {
        TraceSetup_Embree(cacheIndex, startPos, endPos, hitResult);
        return;
    }

    {
        RayTraceContext_t ray;

        ray.end[0] = endPos[0];
        ray.end[1] = endPos[1];
        ray.end[2] = endPos[2];
        ray.start[0] = startPos[0];
        ray.start[1] = startPos[1];
        ray.start[2] = startPos[2];
        ray.delta[0] = endPos[0] - startPos[0];
        ray.delta[1] = endPos[1] - startPos[1];
        ray.delta[2] = endPos[2] - startPos[2];
        ray.cacheIndex = cacheIndex;
        ray.hitResult = hitResult;

        /* clear hit result */
        hitResult->triangle = 0;
        hitResult->baryU = 0.0f;
        hitResult->baryV = 0.0f;
        hitResult->fraction = 1.0f;

        /* increment vis cache to invalidate old stamps */
        g_visCache[cacheIndex]++;

        TraceBSP_r(0, startPos, endPos, &ray);
    }
}

/*
================
TraceVisibility

Sets up a RayTraceContext_t and dispatches TraceVisibility_r
to test if a ray is blocked by any triangle.
Returns 1 if visible, 0 if blocked.
================
*/
int TraceVisibility(int cacheIndex, float *startPos, float *endPos)
{
    RayTraceContext_t ray;

    if (g_useEmbree)
        return TraceVisibility_Embree(startPos, endPos);

    ray.cacheIndex = cacheIndex;
    ray.hitResult = 0;

    ray.end[0] = endPos[0];
    ray.end[1] = endPos[1];
    ray.end[2] = endPos[2];
    ray.start[0] = startPos[0];
    ray.start[1] = startPos[1];
    ray.start[2] = startPos[2];
    ray.delta[0] = endPos[0] - startPos[0];
    ray.delta[1] = endPos[1] - startPos[1];
    ray.delta[2] = endPos[2] - startPos[2];

    /* increment vis cache */
    g_visCache[cacheIndex]++;

    return TraceVisibility_r(0, startPos, endPos, &ray);
}

/*
================
ScoreTrianglesAgainstPlane

Scores a set of triangles against a split plane for BSP building.
Classifies each triangle as front/back/on-plane/straddling
based on vertex distances (threshold 0.001).
Returns |front-back| + 2*(straddle+onPlane), or 0x7FFFFFFF
if all triangles are on one side (bad split).
================
*/
int ScoreTrianglesAgainstPlane(Triangle_t **triArray, int triCount, float *plane)
{
    int i;
    int frontCount, backCount, onPlaneCount, straddleCount;
    int category;
    int remaining;
    int score;
    float dist;
    int hasFront, hasBack;

    frontCount = 0;
    backCount = 0;
    onPlaneCount = 0;
    straddleCount = 0;

    for (i = 0; i < triCount; i++)
    {
        Triangle_t *tri = triArray[i];
        hasFront = 0;
        hasBack = 0;

        /* vertex 0 */
        dist = plane[1] * g_vertPositions[tri->vertIndex[0]][1]
             + plane[0] * g_vertPositions[tri->vertIndex[0]][0]
             + plane[2] * g_vertPositions[tri->vertIndex[0]][2]
             - plane[3];
        if (dist > 0.001f)
            hasFront = 1;
        else if (-0.001f > dist)
            hasBack = 1;

        /* vertex 1 */
        dist = plane[1] * g_vertPositions[tri->vertIndex[1]][1]
             + plane[0] * g_vertPositions[tri->vertIndex[1]][0]
             + plane[2] * g_vertPositions[tri->vertIndex[1]][2]
             - plane[3];
        if (dist > 0.001f)
            hasFront = 1;
        else if (-0.001f > dist)
            hasBack = 1;

        /* vertex 2 */
        dist = plane[1] * g_vertPositions[tri->vertIndex[2]][1]
             + plane[0] * g_vertPositions[tri->vertIndex[2]][0]
             + plane[2] * g_vertPositions[tri->vertIndex[2]][2]
             - plane[3];
        if (dist > 0.001f)
            hasFront = 1;
        else if (-0.001f > dist)
            hasBack = 1;

        /* classify triangle */
        if (hasFront)
        {
            if (hasBack)
                category = 3; /* straddle */
            else
                category = 0; /* front */
        }
        else
        {
            if (hasBack)
                category = 1; /* back */
            else
                category = 2; /* on plane */
        }

        switch (category)
        {
            case 0: frontCount++; break;
            case 1: backCount++; break;
            case 2: onPlaneCount++; break;
            case 3: straddleCount++; break;
        }
    }

    remaining = triCount - (onPlaneCount + straddleCount);

    /* if all triangles are on one side, this is a bad split plane */
    if (frontCount == remaining || backCount == remaining)
        return 0x7FFFFFFF;

    /* score = |front - back| + 2 * (straddle + onPlane) */
    score = frontCount - backCount;
    if (score < 0)
        score = -score;
    score += 2 * (onPlaneCount + straddleCount);

    return score;
}

/*
================
GetLightingSubSampleWithLock

Callback for ForEachLightingSampleInTriangle. Scales texcoord
by 2.0, looks up the lightmap sample, acquires lock,
accumulates weight, releases lock.
================
*/
void GetLightingSubSampleWithLock(float area, float *samplePos,
                                  float *polyVerts, int vertCount,
                                  void *userData, int areaIndex)
{
    Triangle_t *tri = (Triangle_t *)userData;
    LightingSampleResult_t result;
    int combinedIndex;

    (void)polyVerts;
    (void)vertCount;
    (void)areaIndex;

    GetLightingSubSample((int)tri->lightmapIdx, samplePos[0] * 2.0f, samplePos[1] * 2.0f, &result);

    AcquireThreadLock((unsigned int)(uintptr_t)result.lock);

    /* accumulate total weight */
    result.lock->weight += area;

    /* accumulate per-channel weight */
    combinedIndex = result.index0 + result.index1 * 2;
    result.lock->channelWeight[combinedIndex] += area;

    ReleaseThreadLock((unsigned int)(uintptr_t)result.lock);
}

static void CountTexelsCallback(float area, float *samplePos,
                                float *polyVerts, int vertCount,
                                void *userData, int areaIndex)
{
    int *count = (int *)userData;
    (void)area;
    (void)samplePos;
    (void)polyVerts;
    (void)vertCount;
    (void)areaIndex;
    (*count)++;
}

/*
================
SetLightingSampleAreas_Callback

Per-triangle callback for SetLightingSampleAreas.
If triangle has a lightmap, iterates over its lighting samples
calling GetLightingSubSampleWithLock with area=1.
Also counts texels per triangle for sorted dispatch.
================
*/
void SetLightingSampleAreas_Callback(int triIndex, int unused)
{
    Triangle_t *tri = &g_triangles[triIndex];

    if (tri->lightmapIdx != 0x1F)
    {
        ForEachLightingSampleInTriangle(tri, 1, GetLightingSubSampleWithLock, tri);
        if (g_triTexelCount)
        {
            int count = 0;
            ForEachLightingSampleInTriangle(tri, 1, CountTexelsCallback, &count);
            g_triTexelCount[triIndex] = count;
        }
    }
}

/*
================
BuildLightTransfers_Callback

Per-triangle callback for BuildLightTransfers.
Tail-calls BuildLightTransfers_PerTri with transfer count
and ProcessLightingSampleArea as the processing callback.
================
*/
void BuildLightTransfers_Callback(int triIndex, int unused)
{
    if (!g_lightingTransportPass)
        BuildRadiosityColor_PerTri(triIndex);
    else
        BuildLightTransfers_PerTri(triIndex, unused, g_lightTransferCount,
                                   ProcessLightingSampleArea);
}

int g_lightingTransportPass = 1;
int g_lightingSeedSkyPass;

static void RadiosityColorSample(float areaX2, float *centroid,
                                 float *polyVerts, int vertCount,
                                 void *userData, int areaIndex)
{
    RadiosityColorAccum_t *accum = (RadiosityColorAccum_t *)userData;
    MaterialDef_t *material = accum->material;
    int x = ((int)centroid[0]) & (material->width - 1);
    int y = ((int)centroid[1]) & (material->height - 1);
    int blockWidth = material->width / 4;
    const unsigned char *rgb = (const unsigned char *)material->colorMask
                             + 3 * ((x / 4) + blockWidth * (y / 4));
    /* Native geometry.cpp 0x40B650 converts each normalized mask channel
     * through sub_414580 (pow(channel, g_degamma)) before area weighting. */
    float r = DegammaColorChannel((float)rgb[0] * (1.0f / 255.0f));
    float g = DegammaColorChannel((float)rgb[1] * (1.0f / 255.0f));
    float b = DegammaColorChannel((float)rgb[2] * (1.0f / 255.0f));

    (void)polyVerts;
    (void)vertCount;
    (void)areaIndex;
    accum->area += areaX2;
    accum->rgb[0] += r * areaX2;
    accum->rgb[1] += g * areaX2;
    accum->rgb[2] += b * areaX2;
}

static void RadiosityColorForPolygon(const RadiosityColorMapping_t *mapping,
                                     const float *polyVerts, int vertCount,
                                     float outColor[3])
{
    union {
        float verts[MAX_VERTS_PER_POLY][2];
        unsigned char slots[4 * POLY_SLOT_STRIDE];
    } mapped;
    RadiosityColorAccum_t accum;
    float mins[2] = { 3.402823466e+38f, 3.402823466e+38f };
    float maxs[2] = { -3.402823466e+38f, -3.402823466e+38f };
    float startX, startY, spanX, spanY, cellX, cellY;
    int countX, countY;
    int i;

    for (i = 0; i < vertCount; i++)
    {
        float x = polyVerts[i * 2 + 0];
        float y = polyVerts[i * 2 + 1];
        mapped.verts[i][0] = x * mapping->uCoeff[0]
                           + y * mapping->uCoeff[1] + mapping->uCoeff[2];
        mapped.verts[i][1] = x * mapping->vCoeff[0]
                           + y * mapping->vCoeff[1] + mapping->vCoeff[2];
        if (mapped.verts[i][0] < mins[0]) mins[0] = mapped.verts[i][0];
        if (mapped.verts[i][1] < mins[1]) mins[1] = mapped.verts[i][1];
        if (mapped.verts[i][0] > maxs[0]) maxs[0] = mapped.verts[i][0];
        if (mapped.verts[i][1] > maxs[1]) maxs[1] = mapped.verts[i][1];
    }

    startX = floorf(mins[0]);
    startY = floorf(mins[1]);
    spanX = maxs[0] - startX;
    spanY = maxs[1] - startY;
    cellX = ceilf_wrapper(spanX * 0.25f);
    cellY = ceilf_wrapper(spanY * 0.25f);
    if (cellX < 1.0f) cellX = 1.0f;
    if (cellY < 1.0f) cellY = 1.0f;
    countX = (int)ceilf_wrapper(spanX / cellX);
    countY = (int)ceilf_wrapper(spanY / cellY);

    accum.area = 0.0f;
    accum.rgb[0] = accum.rgb[1] = accum.rgb[2] = 0.0f;
    accum.material = mapping->tri->material;

    if (countX > 0 && countY > 0)
    {
        ForEach2dArea(mapped.slots, vertCount, countX, countY,
                      startX, startY, cellX, cellY,
                      RadiosityColorSample, &accum);
    }

    if (accum.area > 0.0f)
    {
        float invArea = 1.0f / accum.area;
        outColor[0] = accum.rgb[0] * invArea;
        outColor[1] = accum.rgb[1] * invArea;
        outColor[2] = accum.rgb[2] * invArea;
    }
    else
    {
        outColor[0] = outColor[1] = outColor[2] = 1.0f;
    }
}

static void RadiosityColorArea(float areaX2, float *centroid,
                               float *polyVerts, int vertCount,
                               void *userData, int areaIndex)
{
    RadiosityColorMapping_t *mapping = (RadiosityColorMapping_t *)userData;
    LightingSampleResult_t result;
    LightmapSample_t *sample;
    float color[3];
    float scale;

    (void)areaIndex;

    GetLightingSubSample((int)mapping->tri->lightmapIdx,
                         centroid[0] * 2.0f, centroid[1] * 2.0f, &result);
    sample = result.lock;
    if (!sample || !sample->vars || sample->weight <= 0.0f)
        return;

    scale = areaX2 / sample->weight;
    RadiosityColorForPolygon(mapping, polyVerts, vertCount, color);

    AcquireThreadLock((unsigned int)(uintptr_t)sample);
    sample->vars->scattered[0] += color[0] * scale;
    sample->vars->scattered[1] += color[1] * scale;
    sample->vars->scattered[2] += color[2] * scale;
    ReleaseThreadLock((unsigned int)(uintptr_t)sample);
}

static int RadiosityColorAffine(const float p[3][2], const float f[3], float out[3])
{
    double p00 = p[0][0], p01 = p[0][1];
    double p10 = p[1][0], p11 = p[1][1];
    double p20 = p[2][0], p21 = p[2][1];
    double f0 = f[0], f1 = f[1], f2 = f[2];
    double denom = p00 * (p11 - p21)
                 + p10 * (p21 - p01)
                 + p20 * (p01 - p11);
    if (denom == 0.0f)
        return 0;

    out[0] = (float)((f0 * (p11 - p21)
                       + f1 * (p21 - p01)
                       + f2 * (p01 - p11)) / denom);
    out[1] = (float)((f0 * (p20 - p10)
                       + f1 * (p00 - p20)
                       + f2 * (p10 - p00)) / denom);
    out[2] = (float)((f0 * (p10 * p21 - p20 * p11)
                       + f1 * (p20 * p01 - p00 * p21)
                       + f2 * (p00 * p11 - p10 * p01)) / denom);
    return 1;
}

static void BuildRadiosityColor_PerTri(int triIndex)
{
    Triangle_t *tri = &g_triangles[triIndex];
    MaterialDef_t *material = tri->material;
    RadiosityColorMapping_t mapping;
    float lm[3][2];
    float texU[3], texV[3];
    int i;

    if (tri->lightmapIdx == LIGHTMAP_NONE || !material || !material->colorMask)
        return;
    if (material->width <= 0 || material->height <= 0)
        return;

    mapping.tri = tri;
    for (i = 0; i < 3; i++)
    {
        DrawVert_t *vert = &g_vertData[tri->vertIndex[i]];
        lm[i][0] = vert->lmCoord[0] * 512.0f;
        lm[i][1] = vert->lmCoord[1] * 512.0f;
        texU[i] = vert->texCoord[0] * (float)material->width;
        texV[i] = vert->texCoord[1] * (float)material->height;
    }

    if (!RadiosityColorAffine(lm, texU, mapping.uCoeff)
        || !RadiosityColorAffine(lm, texV, mapping.vCoeff))
        return;

    ForEachLightingSampleInTriangle(tri, 1, RadiosityColorArea, &mapping);
}

/*
================
SetLightingSampleAreas

Dispatches SetLightingSampleAreas_Callback over all triangles.
================
*/
void SetLightingSampleAreas(int threadCount)
{
    g_triTexelCount = (int *)calloc(g_triCount, sizeof(int));
    ForEachQuantum(g_triCount, SetLightingSampleAreas_Callback, threadCount);
}

static int CompareTriByTexelCount(const void *a, const void *b)
{
    return g_triTexelCount[*(const int *)b] - g_triTexelCount[*(const int *)a];
}

static void BuildLightTransfers_Sorted(unsigned int workIdx, unsigned int threadId)
{
    BuildLightTransfers_Callback(g_triWorkOrder[workIdx], threadId);
}

/*
================
BuildLightTransfers

Dispatches BuildLightTransfers_Callback over all triangles,
sorted by texel count descending (LPT scheduling).
================
*/
void BuildLightTransfers(int threadCount)
{
    int i, lmCount;

    if (g_triTexelCount)
    {
        g_triWorkOrder = (int *)malloc(g_triCount * sizeof(int));
        for (i = 0; i < g_triCount; i++)
            g_triWorkOrder[i] = i;
        qsort(g_triWorkOrder, g_triCount, sizeof(int), CompareTriByTexelCount);

        lmCount = 0;
        for (i = 0; i < g_triCount; i++)
        {
            if (g_triTexelCount[g_triWorkOrder[i]] > 0)
                lmCount++;
        }

        Com_Printf("%d lightmapped triangles of %d total\n", lmCount, g_triCount);
        ForEachQuantum(lmCount, BuildLightTransfers_Sorted, threadCount);

        free(g_triWorkOrder);
        g_triWorkOrder = NULL;
        free(g_triTexelCount);
        g_triTexelCount = NULL;
    }
    else
    {
        ForEachQuantum(g_triCount, BuildLightTransfers_Callback, threadCount);
    }
}

/*
================
ForEachLightmapPixel_Callback

Per-triangle callback for ForEachLightmapPixelInPoly.
If triangle has a lightmap, calls ForEachLightingSampleInTriangle
with the stored callback and parameter.
================
*/
void ForEachLightmapPixel_Callback(int triIndex, int unused)
{
    Triangle_t *tri = &g_triangles[triIndex];

    if (tri->lightmapIdx != 0x1F)
        ForEachLightingSampleInTriangle(tri, g_lightmapPixelParam,
                                        g_lightmapPixelCallback,
                                        (void *)&tri->lightmapIdx);
}

/*
================
ForEachLightmapPixelInPoly

Stores a callback and parameter in globals, then dispatches
ForEachLightmapPixel_Callback over all triangles.
================
*/
void ForEachLightmapPixelInPoly(void *callback, int param, int threadCount)
{
    g_lightmapPixelCallback = callback;
    g_lightmapPixelParam = param;
    ForEachQuantum(g_triCount, ForEachLightmapPixel_Callback, threadCount);
}

/*
================
GramSchmidt

Gram-Schmidt orthonormalization of a 3x3 basis.
Input: 9 floats at v — three 3D vectors (v0, v1, v2).
Output: orthonormal basis.
================
*/
void GramSchmidt(float *v)
{
    float dot;

    /* normalize v0 */
    Vec3Normalize(v);

    /* orthogonalize v1 against v0 */
    dot = v[0] * v[3] + v[4] * v[1] + v[5] * v[2];
    v[3] -= v[0] * dot;
    v[4] -= v[1] * dot;
    v[5] -= v[2] * dot;

    /* normalize v1 */
    Vec3Normalize(&v[3]);

    /* orthogonalize v2 against v0 */
    dot = v[6] * v[0] + v[7] * v[1] + v[8] * v[2];
    v[6] -= v[0] * dot;
    v[7] -= v[1] * dot;
    v[8] -= v[2] * dot;

    /* orthogonalize v2 against v1 */
    dot = v[6] * v[3] + v[7] * v[4] + v[8] * v[5];
    v[6] -= v[3] * dot;
    v[7] -= v[4] * dot;
    v[8] -= v[5] * dot;

    /* normalize v2 */
    Vec3Normalize(&v[6]);
}

/*
================
FindBestAxisSplit

Tries axis-aligned split planes along X, Y, Z axes.
For each axis, builds sorted min/max/equal arrays, sweeps
through to find the position that best balances triangles.
Returns best score and writes the split plane to outPlane.
================
*/
static char s_assertDisable_FindBest_countMinMax;
static char s_assertDisable_FindBest_countCoplanar;
static char s_assertDisable_FindBest_total;
static char s_assertDisable_FindBest_classify;
static char s_assertDisable_FindBest_front;
static char s_assertDisable_FindBest_back;
static char s_assertDisable_FindBest_cross;
static char s_assertDisable_FindBest_on;

int FindBestAxisSplit(Triangle_t **triArray, int triCount, float *outPlane)
{
    int axis;
    int bestScore;
    int bestAxis;
    float bestSplitPos;
    int countMinMax, countCoplanar;
    int i;
    float minVal, maxVal, val;
    float splitPos, nextPos, prevSplitPos;
    int frontCount, backCount, crossCount, onCount;
    int remaining;
    int score;

    bestScore = 0x7FFFFFFF;
    bestAxis = -1;
    bestSplitPos = 0.0f;

    for (axis = 0; axis < 3; axis++)
    {
        /* classify triangles along this axis */
        countMinMax = 0;
        countCoplanar = 0;

        for (i = 0; i < triCount; i++)
        {
            Triangle_t *tri = triArray[i];
            float v0 = g_vertPositions[tri->vertIndex[0]][axis];
            float v1 = g_vertPositions[tri->vertIndex[1]][axis];
            float v2 = g_vertPositions[tri->vertIndex[2]][axis];

            /* find min and max */
            minVal = v0;
            maxVal = v0;
            if (v1 < minVal) minVal = v1;
            else if (v1 > maxVal) maxVal = v1;
            if (v2 < minVal) minVal = v2;
            else if (v2 > maxVal) maxVal = v2;

            if (minVal == maxVal)
            {
                /* coplanar triangle */
                g_bspSortEqual[countCoplanar++] = minVal;
            }
            else
            {
                g_bspSortMins[countMinMax] = minVal;
                g_bspSortMaxs[countMinMax] = maxVal;
                countMinMax++;
            }
        }

        Assert(countMinMax <= triCount, s_assertDisable_FindBest_countMinMax);
        Assert(countCoplanar <= triCount, s_assertDisable_FindBest_countCoplanar);
        Assert(countMinMax + countCoplanar == triCount, s_assertDisable_FindBest_total);

        /* sort all three arrays */
        qsort_wrapper(g_bspSortMins, countMinMax, 4, CompareFunction);
        qsort_wrapper(g_bspSortMaxs, countMinMax, 4, CompareFunction);
        qsort_wrapper(g_bspSortEqual, countCoplanar, 4, CompareFunction);

        /* sweep to find best split */
        {
            int minIdx = 0, maxIdx = 0, eqIdx = 0;
            int sameMinCount, sameMaxCount, sameEqCount;

            /* find starting position (smallest of mins[0] and equals[0]) */
            splitPos = (countMinMax > 0) ? g_bspSortMins[0] : 3.4028235e+38f;
            if (countCoplanar > 0 && g_bspSortEqual[0] <= splitPos)
                splitPos = g_bspSortEqual[0];

            prevSplitPos = -131073.0f;
            frontCount = 0;
            backCount = triCount;
            crossCount = 0;
            onCount = 0;

            {
                int prevMinCount = 0, prevEqCount = 0;

            while (splitPos < 3.4028235e+38f)
            {
                /* apply deferred transitions from previous step */
                crossCount += prevMinCount;   /* prev mins: back->crossing */
                backCount -= prevMinCount;
                frontCount += prevEqCount;    /* prev equals: on->front */
                onCount -= prevEqCount;

                /* count mins at current position (DEFERRED to next step) */
                sameMinCount = 0;
                while (minIdx < countMinMax && g_bspSortMins[minIdx] == splitPos)
                {
                    sameMinCount++;
                    minIdx++;
                }

                /* count maxs at current position: crossing->front (IMMEDIATE) */
                sameMaxCount = 0;
                while (maxIdx < countMinMax && g_bspSortMaxs[maxIdx] == splitPos)
                {
                    sameMaxCount++;
                    maxIdx++;
                }
                frontCount += sameMaxCount;
                crossCount -= sameMaxCount;

                /* count equals at current position: back->on (IMMEDIATE, front deferred) */
                sameEqCount = 0;
                while (eqIdx < countCoplanar && g_bspSortEqual[eqIdx] == splitPos)
                {
                    sameEqCount++;
                    eqIdx++;
                }
                onCount += sameEqCount;
                backCount -= sameEqCount;

                /* find next position */
                nextPos = 3.4028235e+38f;
                if (minIdx < countMinMax && g_bspSortMins[minIdx] < nextPos)
                    nextPos = g_bspSortMins[minIdx];
                if (maxIdx < countMinMax && g_bspSortMaxs[maxIdx] < nextPos)
                    nextPos = g_bspSortMaxs[maxIdx];
                if (eqIdx < countCoplanar && g_bspSortEqual[eqIdx] < nextPos)
                    nextPos = g_bspSortEqual[eqIdx];

                /* assert side counts */
                Assert(frontCount + backCount + crossCount + onCount == triCount, s_assertDisable_FindBest_classify);
                Assert(frontCount >= 0, s_assertDisable_FindBest_front);
                Assert(backCount >= 0, s_assertDisable_FindBest_back);
                Assert(crossCount >= 0, s_assertDisable_FindBest_cross);
                Assert(onCount >= 0, s_assertDisable_FindBest_on);

                /* score this split if position gaps are large enough */
                if (nextPos - splitPos >= 0.002f && splitPos - prevSplitPos >= 0.002f)
                {
                    int absDiff = frontCount - backCount;
                    if (absDiff < 0) absDiff = -absDiff;

                    if (absDiff <= (triCount / 2))
                    {
                        remaining = triCount - (crossCount + onCount);
                        if (frontCount == remaining || backCount == remaining)
                            score = 0x7FFFFFFF;
                        else
                            score = absDiff + (crossCount + onCount) * 2;

                        if (score < bestScore)
                        {
                            bestScore = score;
                            bestSplitPos = splitPos;
                            bestAxis = axis;
                        }
                    }
                }

                /* save deferred counts for next step */
                prevMinCount = sameMinCount;
                prevEqCount = sameEqCount;

                prevSplitPos = splitPos;
                splitPos = nextPos;
            }
            } /* end of prevMinCount/prevEqCount scope */
        }
    }

    /* write output plane if a valid split was found */
    if (bestAxis >= 0)
    {
        outPlane[0] = 0.0f;
        outPlane[1] = 0.0f;
        outPlane[2] = 0.0f;
        outPlane[bestAxis] = 1.0f;
        outPlane[3] = bestSplitPos;
    }

    return bestScore;
}

/*
================
FindBestSplitPlane

Finds the best split plane for BSP building from triangle planes.
First tries axis-aligned splits (FindBestAxisSplit), then for
small sets (<=128) also tries each triangle's plane.
Returns 1 if a valid split was found, 0 if not.
================
*/
int FindBestSplitPlane(Triangle_t **triArray, int triCount, float *outPlane)
{
    int bestScore;
    int i;
    int score;
    Triangle_t *tri;

    if (triCount <= 16)
        return 0;

    bestScore = FindBestAxisSplit(triArray, triCount, outPlane);

    /* for small sets, also try each triangle's plane */
    if (triCount <= 128)
    {
        for (i = 0; i < triCount; i++)
        {
            tri = triArray[i];
            score = ScoreTrianglesAgainstPlane(triArray, triCount, tri->normal);
            if (score < bestScore)
            {
                bestScore = score;
                outPlane[0] = tri->normal[0];
                outPlane[1] = tri->normal[1];
                outPlane[2] = tri->normal[2];
                outPlane[3] = tri->dist;
            }
        }
    }

    return bestScore < 0x7FFFFFFF;
}

/*
================
BuildCollisionBSP_Partition

Creates a BSP leaf node. Allocates a leaf entry, copies triangle
refs to the global pointer array, returns negative-encoded leaf index.
================
*/
int BuildCollisionBSP_Partition(Triangle_t **triRefs, int triCount)
{
    int leafIdx;

    Assert(triRefs, s_assertDisable_Partition_triRefs);
    Assert(triCount, s_assertDisable_Partition_triCount);

    /* check ref overflow */
    if (g_bspRefCount + triCount > g_bspNodeAlloc)
        Com_Printf( "Ran out of triangle refs");

    /* check leaf overflow */
    Assert(g_bspLeafCount < 0x40001, s_assertDisable_Partition_leafCount);

    /* allocate leaf */
    leafIdx = g_bspLeafCount++;
    g_bspLeafData[leafIdx].triCount = triCount;
    g_bspLeafData[leafIdx].triOffset = g_bspRefCount;
    g_bspRefCount += triCount;

    /* copy triangle pointers */
    memcpy_fast(&g_triPointerArray[g_bspLeafData[leafIdx].triOffset],
                triRefs, triCount * 8);

    return -2 - leafIdx;
}

/*
================
ClassifyTriangleSide

Partitions a triangle array by a split plane. Triangles on the
"other" side are moved to the end. Returns count remaining
on the target side (includes on-plane and straddling).
================
*/
int ClassifyTriangleSide(float *plane, int side, Triangle_t **triArray, int triCount)
{
    int i;
    int remaining;
    int otherSide;
    int category;
    float dist;
    int hasFront, hasBack;
    Triangle_t *temp;

    Assert(side == 0 || side == 1, s_assertDisable_ClassifySide);

    otherSide = 1 - side;
    remaining = triCount;
    i = 0;

    while (i < remaining)
    {
        Triangle_t *tri = triArray[i];
        hasFront = 0;
        hasBack = 0;

        /* vertex 0 */
        dist = plane[1] * g_vertPositions[tri->vertIndex[0]][1]
             + plane[0] * g_vertPositions[tri->vertIndex[0]][0]
             + plane[2] * g_vertPositions[tri->vertIndex[0]][2]
             - plane[3];
        if (dist > 0.001f)
            hasFront = 1;
        else if (-0.001f > dist)
            hasBack = 1;

        /* vertex 1 */
        dist = plane[1] * g_vertPositions[tri->vertIndex[1]][1]
             + plane[0] * g_vertPositions[tri->vertIndex[1]][0]
             + plane[2] * g_vertPositions[tri->vertIndex[1]][2]
             - plane[3];
        if (dist > 0.001f)
            hasFront = 1;
        else if (-0.001f > dist)
            hasBack = 1;

        /* vertex 2 */
        dist = plane[1] * g_vertPositions[tri->vertIndex[2]][1]
             + plane[0] * g_vertPositions[tri->vertIndex[2]][0]
             + plane[2] * g_vertPositions[tri->vertIndex[2]][2]
             - plane[3];
        if (dist > 0.001f)
            hasFront = 1;
        else if (-0.001f > dist)
            hasBack = 1;

        /* classify */
        if (hasFront)
            category = hasBack ? 3 : 0;
        else
            category = hasBack ? 1 : 2;

        if (category == otherSide)
        {
            /* swap to end */
            remaining--;
            temp = triArray[remaining];
            triArray[remaining] = tri;
            triArray[i] = temp;
        }
        else
        {
            i++;
        }
    }

    return remaining;
}

/*
================
BuildCollisionBSP_r

Recursive BSP tree builder. If a good split plane is found,
creates an internal node and recurses. Otherwise creates a leaf.
================
*/
int BuildCollisionBSP_r(int nodeIndex, Triangle_t **triArray, int triCount)
{
    BSPCollisionNode_t *node;
    int childCount;
    int side;

    if (triCount == 0)
        return -1; /* empty */

    if (nodeIndex < 0)
    {
        /* allocate new node */
        nodeIndex = g_bspNodeCount;
        node = &g_bspNodeData[nodeIndex];

        if (g_bspNodeCount == 0x40000)
            goto make_leaf;

        if (!FindBestSplitPlane(triArray, triCount, node->normal))
            goto make_leaf;

        /* create internal node */
        node->child[0] = -1;
        node->child[1] = -1;
        g_bspNodeCount++;
    }
    else
    {
        node = &g_bspNodeData[nodeIndex];
    }

    /* partition and recurse for each child */
    for (side = 0; side < 2; side++)
    {
        childCount = ClassifyTriangleSide(node->normal, side, triArray, triCount);
        node->child[side] = BuildCollisionBSP_r(node->child[side], triArray, childCount);
    }

    return nodeIndex;

make_leaf:
    return BuildCollisionBSP_Partition(triArray, triCount);
}

/*
================
BuildCollisionBSP

Top-level BSP builder. Allocates temp triangle pointer array,
filters triangles (only opaque/collidable), allocates sort arrays,
runs the recursive builder, frees temp memory.
================
*/
void BuildCollisionBSP(void)
{
    Triangle_t **tempTriArray;
    int i;
    int count;
    MaterialDef_t *mat;

    /* allocate temp triangle pointer array */
    tempTriArray = (Triangle_t **)Hunk_AllocateTempMemory(g_triCount * 8);
    if (!tempTriArray)
    {
        Com_Printf( "Couldn't allocate %i bytes for triangle sort refs\n",
                  (long long)g_triCount * 8);
    }

    /* filter: only include opaque/collidable triangles */
    count = 0;
    for (i = 0; i < g_triCount; i++)
    {
        if (g_triangles[i].materialIdx != 0)
            continue;

        mat = g_triangles[i].material;
        if (mat->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY))
            goto include;
        if (mat->contents & CONTENTS_TELEPORTER)
            continue;
        if ((mat->surfaceType & SURFTYPE_MASK) == SURFTYPE_PATCH)
            goto include;
        if (!mat->extraData)
            continue;

    include:
        tempTriArray[count++] = &g_triangles[i];
    }

    /* allocate sort arrays for axis-aligned split finding */
    g_bspSortMins = (float *)Hunk_AllocateTempMemory(count * 4);
    g_bspSortMaxs = (float *)Hunk_AllocateTempMemory(count * 4);
    g_bspSortEqual = (float *)Hunk_AllocateTempMemory(count * 4);

    if (!g_bspSortMins || !g_bspSortMaxs || !g_bspSortEqual)
    {
        Com_Printf( "Couldn't allocate %i bytes for triangle sort arrays\n",
                  (long long)count * 12);
    }

    /* build the BSP tree */
    if (count > 0)
    {
        BSPCollisionNode_t *rootNode = &g_bspNodeData[0];
        int side;

        for (side = 0; side < 2; side++)
        {
            int childCount = ClassifyTriangleSide(rootNode->normal, side,
                                                  tempTriArray, count);
            rootNode->child[side] = BuildCollisionBSP_r(rootNode->child[side],
                                                        tempTriArray, childCount);
        }
    }
    else if (g_bspNodeCount > 0)
    {
        /* The file BSP's negative children index file leaves, not the
         * transient lighting-leaf array.  When the material filter yields no
         * traceable triangles there is nothing to graft onto that topology;
         * make the root explicitly empty instead of leaving stale leaf IDs. */
        g_bspNodeData[0].child[0] = -1;
        g_bspNodeData[0].child[1] = -1;
    }

    /* free temp memory */
    Hunk_FreeTempMemory(g_bspSortEqual);
    Hunk_FreeTempMemory(g_bspSortMaxs);
    Hunk_FreeTempMemory(g_bspSortMins);
    Hunk_FreeTempMemory(tempTriArray);
}

/*
================
SetupBSPNodes

Copies BSP node data from the loaded BSP file into the
internal collision node format. Reads plane data and child
indices from the BSP file structures.
================
*/
void SetupBSPNodes(void)
{
    int i;
    int planeNum;
    BSPCollisionNode_t *destNode;
    BspFileNode_t *srcNode;

    Assert(g_bspNodeData, s_assertDisable_SetupBSP_nodes);

    g_bspNodeCount = g_numBSPNodes;

    for (i = 0; i < g_bspNodeCount; i++)
    {
        srcNode = &g_bspFileNodes[i];
        destNode = &g_bspNodeData[i];

        Assert(srcNode, s_assertDisable_SetupBSP_dnode);
        Assert(destNode, s_assertDisable_SetupBSP_bspNode);

        planeNum = srcNode->planeNum;

        /* copy plane from BSP plane array */
        destNode->normal[0] = g_bspPlanes[planeNum][0];
        destNode->normal[1] = g_bspPlanes[planeNum][1];
        destNode->normal[2] = g_bspPlanes[planeNum][2];
        destNode->dist = g_bspPlanes[planeNum][3];

        /* copy child indices */
        destNode->child[0] = srcNode->children[0];
        destNode->child[1] = srcNode->children[1];
    }
}

/*
================
InitGeometry

Main geometry initialization. Allocates collision data,
builds BSP tree, reallocates to actual size, and sets up
world bounds.
================
*/
void InitGeometry(void)
{
    unsigned int startTime;
    int allocSize;
    float mins[3], maxs[3];

    Com_Printf("building collision data...\n");
    startTime = timeGetTime();

    /* allocate triangle ref data: g_triCount * 32 entries * 8 bytes */
    allocSize = g_triCount << 5;
    g_bspNodeAlloc = allocSize;
    g_triPointerArray = (Triangle_t **)malloc((size_t)allocSize * 8);
    if (!g_triPointerArray)
        ErrorMsg("Couldn't allocate %i bytes for triangle refs\n",
                 (long long)allocSize * 8);

    /* assert node count fits */
    Assert(g_numBSPNodes <= 0x40000, s_assertDisable_SetupBSP_nodes);

    /* allocate BSP node array: 0x600000 bytes for 0x40000 nodes */
    g_bspNodeData = (BSPCollisionNode_t *)malloc(0x600000);
    if (!g_bspNodeData)
        ErrorMsg("Couldn't allocate %i bytes for internal bsp nodes\n", 0x600000);

    /* allocate BSP leaf array: 0x200008 bytes */
    g_bspLeafData = (BSPCollisionLeaf_t *)malloc(0x200008);
    if (!g_bspLeafData)
        ErrorMsg("Couldn't allocate %i bytes for internal bsp leafs\n", 0x200008);

    /* setup BSP nodes from file data and build collision BSP */
    SetupBSPNodes();
    BuildCollisionBSP();

    Com_Printf("building collision took %.1f seconds\n",
                 (float)(timeGetTime() - startTime) * 0.001f);

    /* reallocate to actual used size */
    g_bspNodeData = (BSPCollisionNode_t *)realloc(
        g_bspNodeData, g_bspNodeCount * 24);
    g_bspLeafData = (BSPCollisionLeaf_t *)realloc(
        g_bspLeafData, g_bspLeafCount * 8);

    /* world bounds from BSP root node (node 0) — stored as INTEGERS, convert to float via cvtsi2ss */
    {
        BspFileNode_t *rootFileNode = &g_bspFileNodes[0];
        mins[0] = (float)rootFileNode->mins[0];
        mins[1] = (float)rootFileNode->mins[1];
        mins[2] = (float)rootFileNode->mins[2];
        maxs[0] = (float)rootFileNode->maxs[0];
        maxs[1] = (float)rootFileNode->maxs[1];
        maxs[2] = (float)rootFileNode->maxs[2];
    }
    BuildModelCollision(mins, maxs);
}

/*
================
BuildLightTransfers_PerTri

Per-triangle light transfer builder. Checks if the triangle
has valid lightmap coords, computes linear mapping, then
iterates over lighting samples.
================
*/
void BuildLightTransfers_PerTri(int triIndex, int param, int transferCount,
                                void (*callback)(void))
{
    Triangle_t *tri;
    LightTransferMapping_t localMapping;

    tri = &g_triangles[triIndex];

    if (tri->lightmapIdx == 0x1F)
        return;

    /* check that all 3 vertices have distinct lightmap coords */
    {
        float *lm0 = g_vertData[tri->vertIndex[0]].lmCoord;
        float *lm1 = g_vertData[tri->vertIndex[1]].lmCoord;
        float *lm2 = g_vertData[tri->vertIndex[2]].lmCoord;

        if (lm0[0] == lm1[0] && lm0[1] == lm1[1])
            return;
        if (lm1[0] == lm2[0] && lm1[1] == lm2[1])
            return;
        if (lm2[0] == lm0[0] && lm2[1] == lm0[1])
            return;
    }

    /* compute linear mapping for lightmap sample iteration */
    if (!ComputeLinearMappingForTriangle(tri, &localMapping))
        return;

    /* overwrite header: param and lightmapIdx */
    localMapping.param = param;
    localMapping.lightmapIdx = (int)tri->lightmapIdx;
    localMapping.triangleIndex = triIndex;
    localMapping.primaryLightIndex = tri->primaryLightIndex;

    /* iterate over lighting samples with mapping data */
    {
        typedef void (*SampleIterFunc)(Triangle_t *, int, void *, void *);
        ((SampleIterFunc)ForEachLightingSampleInTriangle)(
            tri, transferCount, (void *)callback, &localMapping);
    }
}

/*
================
ComputeLinearMappingForTriangle

Computes the linear mapping between world-space positions
and lightmap UV coordinates for a triangle. Transforms
vertex tangent/binormal/normal through lightmap matrices.
Output is a mapping structure used by ForEachLightingSampleInTriangle.
Returns 1 on success, 0 if mapping fails.
================
*/
int ComputeLinearMappingForTriangle(Triangle_t *tri, void *outMapping)
{
    float basis[3];             /* {0, 0, 1} */
    float *vertPos[3];          /* pointers to vertex positions */
    float scaledUV[3][3];       /* {u*512, v*512, 0} per vertex — 3 floats with padding */
    float tangentXform[3][3];   /* transformed tangent per vertex */
    float binormalXform[3][3];  /* transformed binormal per vertex */
    float normalXform[3][3];    /* transformed normal per vertex */
    float mappingData[0x40];    /* intermediate mapping result */
    float evalResult[4];        /* eval output: [0]=uCoeff, [1]=vCoeff, [2]=?, [3]=constant */
    int i;
    int materialRef;
    float *lmMatrix;
    int success;

    basis[0] = 0.0f;
    basis[1] = 0.0f;
    basis[2] = 1.0f;

    /* gather per-vertex data */
    for (i = 0; i < 3; i++)
    {
        int vi = tri->vertIndex[i];
        vertPos[i] = g_vertPositions[vi];

        /* scale lightmap coords to pixel space, zero-padded to vec3 */
        scaledUV[i][0] = g_vertData[vi].lmCoord[0] * 512.0f;
        scaledUV[i][1] = g_vertData[vi].lmCoord[1] * 512.0f;
        scaledUV[i][2] = 0.0f;

        materialRef = tri->materialIdx;
        lmMatrix = g_modelBoundsData[materialRef];

        /* transform tangent through lightmap matrix */
        MatrixTransformDirection(lmMatrix, &g_vertData[vi].tangent, &tangentXform[i]);

        /* transform binormal through lightmap matrix */
        MatrixTransformDirection(lmMatrix, &g_vertData[vi].binormal, &binormalXform[i]);

        /* transform normal through lightmap matrix */
        MatrixTransformDirection(lmMatrix, &g_vertData[vi].normal, &normalXform[i]);
    }

    /* build the linear mapping from scaled UV coordinates */
    success = SetupLinearMapping(basis, scaledUV[0], scaledUV[1], scaledUV[2], mappingData);
    if (!success)
        return 0;

    /*
     * Evaluate mapping for each axis component (X=0, Y=1, Z=2).
     * For each eval, xmm1/xmm2/xmm3 = vertex 0/1/2 data for component i.
     * Output: evalResult[0]=uCoeff, evalResult[1]=vCoeff, evalResult[3]=constant.
     */
    {
        float *outF = (float *)outMapping;

        for (i = 0; i < 3; i++)
        {
            /* position mapping: use vertex world positions */
            OrientationDirToWorldDir(mappingData, vertPos[0][i], vertPos[1][i],
                                  vertPos[2][i], evalResult);
            outF[2 + i]  = evalResult[3]; /* pos.const[i] at +0x08 */
            outF[5 + i]  = evalResult[0]; /* pos.uCoeff[i] at +0x14 */
            outF[8 + i]  = evalResult[1]; /* pos.vCoeff[i] at +0x20 */

            /* tangent mapping: use transformed tangent vectors */
            OrientationDirToWorldDir(mappingData, tangentXform[0][i], tangentXform[1][i],
                                  tangentXform[2][i], evalResult);
            outF[11 + i] = evalResult[3]; /* tan.const[i] at +0x2C */
            outF[14 + i] = evalResult[0]; /* tan.uCoeff[i] at +0x38 */
            outF[17 + i] = evalResult[1]; /* tan.vCoeff[i] at +0x44 */

            /* binormal mapping: use transformed binormal vectors */
            OrientationDirToWorldDir(mappingData, binormalXform[0][i], binormalXform[1][i],
                                  binormalXform[2][i], evalResult);
            outF[20 + i] = evalResult[3]; /* binorm.const[i] at +0x50 */
            outF[23 + i] = evalResult[0]; /* binorm.uCoeff[i] at +0x5C */
            outF[26 + i] = evalResult[1]; /* binorm.vCoeff[i] at +0x68 */

            /* normal mapping: use transformed normal vectors */
            OrientationDirToWorldDir(mappingData, normalXform[0][i], normalXform[1][i],
                                  normalXform[2][i], evalResult);
            outF[29 + i] = evalResult[3]; /* norm.const[i] at +0x74 */
            outF[32 + i] = evalResult[0]; /* norm.uCoeff[i] at +0x80 */
            outF[35 + i] = evalResult[1]; /* norm.vCoeff[i] at +0x8C */
        }
    }

    return 1;
}

/*
================
ForEachLightingSampleInTriangle

Sets up lightmap UV coordinates for a triangle, computes
the bounding pixel range, validates bounds, then delegates
to the rasterizer which iterates over pixels
and calls the callback for each one inside the triangle.
================
*/
void ForEachLightingSampleInTriangle(Triangle_t *tri, int count,
                                     void *callback, void *userData)
{
    union {
        float verts[3][2];
        char  _polySlots[4 * POLY_SLOT_STRIDE];
    } scaledBuf;
#define scaledCoords scaledBuf.verts
    float mins[2], maxs[2];   /* 2D bounding box */
    int resolution;
    float resF;
    int sBase, tBase, sCount, tCount;
    float pixelSize, sStart, tStart;
    int i;

    ClearBounds2D(mins, maxs);

    /* scale lightmap coords by 512 and compute bounds */
    for (i = 0; i < 3; i++)
    {
        int vi = tri->vertIndex[i];
        scaledCoords[i][0] = g_vertData[vi].lmCoord[0] * 512.0f;
        scaledCoords[i][1] = g_vertData[vi].lmCoord[1] * 512.0f;
        AddPointToBounds2D(scaledCoords[i], mins, maxs);
    }

    /* validate bounds: mins >= 0 && maxs <= 512 */
    Assert(mins[0] >= 0.0f && maxs[0] <= 512.0f && maxs[0] >= mins[0],
           s_assertDisable_ForEach_boundsU);
    Assert(mins[1] >= 0.0f && maxs[1] <= 512.0f && maxs[1] >= mins[1],
           s_assertDisable_ForEach_boundsV);

    /* compute pixel range */
    resolution = count * 2;
    resF = (float)resolution;

    sBase = (int)floorf(resF * mins[0]);
    tBase = (int)floorf(resF * mins[1]);
    sCount = (int)ceilf_wrapper(resF * maxs[0]) - sBase;
    tCount = (int)ceilf_wrapper(resF * maxs[1]) - tBase;

    /* validate pixel ranges */
    Assert(sBase >= 0 && sBase < resolution * 1024, s_assertDisable_ForEach_sBase);
    Assert(tBase >= 0 && tBase < resolution * 1024, s_assertDisable_ForEach_tBase);
    Assert(sBase + sCount + 1 >= 0 && sBase + sCount + 1 <= resolution * 1024,
           s_assertDisable_ForEach_sEnd);
    Assert(tBase + sCount + 1 >= 0 && tBase + sCount + 1 <= resolution * 1024,
           s_assertDisable_ForEach_tEnd);

    /* compute UV start and pixel size */
    pixelSize = 1.0f / resF;
    sStart = (float)sBase * pixelSize;
    tStart = (float)tBase * pixelSize;

    /* delegate to rasterizer */
    ForEach2dArea((float *)scaledCoords, 3,
                             sCount + 1, tCount + 1,
                             sStart, tStart, pixelSize, pixelSize,
                             (ForEach2dAreaCallback)callback, userData);
#undef scaledCoords
}

/*
================
ClampAreaToWeight

Native 0x40B570: max(0, min(areaX2, weight)), using the binary's exact
subtract-and-compare sequence.
================
*/
static float ClampAreaToWeight(float areaX2, float weight)
{
    float val = (weight - areaX2 < 0.0f) ? weight : areaX2;
    return (0.0f - val < 0.0f) ? val : 0.0f;
}

/*
================
ProcessLightingSampleArea

Callback for lighting sample processing. Maps lightmap UV
to world position and tangent frame using linear mapping
coefficients, orthonormalizes with GramSchmidt, validates
for NaN, then calls GramSchmidt.

Mapping data layout (from ComputeLinearMappingForTriangle):
  +0x00: param (int)           +0x04: lightmapIdx (int)
  +0x08: position constant[3]  +0x14: position uCoeff[3]
  +0x20: position vCoeff[3]    +0x2C: tangent constant[3]
  +0x38: tangent uCoeff[3]     +0x44: tangent vCoeff[3]
  +0x50: binormal constant[3]  +0x5C: binormal uCoeff[3]
  +0x68: binormal vCoeff[3]    +0x74: normal constant[3]
  +0x80: normal uCoeff[3]      +0x8C: normal vCoeff[3]
================
*/
void ProcessLightingSampleArea(float areaX2, float *centroid,
                               float *polyVerts, int vertCount,
                               void *userData, int areaIndex)
{
    LightingSampleResult_t sampleResult;
    LightTransferMapping_t *mapping;
    float subAreaWeight, areaWeight;
    float worldPos[3];
    float basis[9]; /* 3x3: tangent, binormal, normal */
    float u, v;
    int combinedIndex;

    (void)polyVerts;
    (void)vertCount;

    Assert(areaX2 > 0.0f, s_assertDisable_Process_area);
    Assert(userData, s_assertDisable_Process_userData);

    /* validate centroid not NaN */
    Assert(!IS_NAN_FLOAT(centroid[0]), s_assertDisable_Process_nanC0);
    Assert(!IS_NAN_FLOAT(centroid[1]), s_assertDisable_Process_nanC1);

    /* gather lightmap sample */
    mapping = (LightTransferMapping_t *)userData;
    GetLightingSubSample(mapping->lightmapIdx,
                         centroid[0] * 2.0f, centroid[1] * 2.0f,
                         &sampleResult);

    /* clamp the cell area against each accumulator separately (native 0x40bd9d
     * channel, 0x40bdc8 total).  The channel clamp gates the cell; the pair is
     * what every later consumer sees: transport and the direct gathers receive
     * (total, channel) (0x40c100/0x40c0fd push arg_0 then arg_10), and the
     * reject/suppression subtract removes each clamp from its own accumulator
     * (0x40bc53 weight -= a2, 0x40bc6b channel -= a3, called with (arg_0,
     * arg_10)).  Subtracting one shared value desyncs the accumulators and
     * trips the subAreaFactor > 0 sanity check downstream. */
    combinedIndex = sampleResult.index0 + sampleResult.index1 * 2;
    subAreaWeight = ClampAreaToWeight(areaX2, sampleResult.lock->channelWeight[combinedIndex]);
    if (subAreaWeight == 0.0f)
        goto done;
    areaWeight = ClampAreaToWeight(areaX2, sampleResult.lock->weight);

    /* Native geometry.cpp 0x40BC80 replays the exact rejected raster-cell
     * ranges stored in radtrans.bin and removes their area before doing any
     * direct-light work. */
    if (Relight_IsSuppressed(mapping->triangleIndex, areaIndex))
    {
        AcquireThreadLock((unsigned int)(uintptr_t)sampleResult.lock);
        sampleResult.lock->weight -= areaWeight;
        sampleResult.lock->channelWeight[combinedIndex] -= subAreaWeight;
        ReleaseThreadLock((unsigned int)(uintptr_t)sampleResult.lock);
        goto done;
    }

    /* map UV to world position using linear mapping */
    u = centroid[0];
    v = centroid[1];

    worldPos[0] = mapping->posConst[0] + u * mapping->posUCoeff[0] + v * mapping->posVCoeff[0];
    worldPos[1] = mapping->posConst[1] + u * mapping->posUCoeff[1] + v * mapping->posVCoeff[1];
    worldPos[2] = mapping->posConst[2] + u * mapping->posUCoeff[2] + v * mapping->posVCoeff[2];

    /* map UV to tangent frame */
    basis[0] = mapping->tanConst[0] + u * mapping->tanUCoeff[0] + v * mapping->tanVCoeff[0];
    basis[1] = mapping->tanConst[1] + u * mapping->tanUCoeff[1] + v * mapping->tanVCoeff[1];
    basis[2] = mapping->tanConst[2] + u * mapping->tanUCoeff[2] + v * mapping->tanVCoeff[2];

    basis[3] = mapping->binConst[0] + u * mapping->binUCoeff[0] + v * mapping->binVCoeff[0];
    basis[4] = mapping->binConst[1] + u * mapping->binUCoeff[1] + v * mapping->binVCoeff[1];
    basis[5] = mapping->binConst[2] + u * mapping->binUCoeff[2] + v * mapping->binVCoeff[2];

    basis[6] = mapping->normConst[0] + u * mapping->normUCoeff[0] + v * mapping->normVCoeff[0];
    basis[7] = mapping->normConst[1] + u * mapping->normUCoeff[1] + v * mapping->normVCoeff[1];
    basis[8] = mapping->normConst[2] + u * mapping->normUCoeff[2] + v * mapping->normVCoeff[2];

    /* validate no NaN before GramSchmidt */
    Assert(!IS_NAN_FLOAT(basis[0]) && !IS_NAN_FLOAT(basis[1]) && !IS_NAN_FLOAT(basis[2]),
           s_assertDisable_Process_nanB0pre);
    Assert(!IS_NAN_FLOAT(basis[3]) && !IS_NAN_FLOAT(basis[4]) && !IS_NAN_FLOAT(basis[5]),
           s_assertDisable_Process_nanB1pre);
    Assert(!IS_NAN_FLOAT(basis[6]) && !IS_NAN_FLOAT(basis[7]) && !IS_NAN_FLOAT(basis[8]),
           s_assertDisable_Process_nanB2pre);

    /* orthonormalize tangent frame */
    GramSchmidt(basis);

    /* validate no NaN after GramSchmidt */
    Assert(!IS_NAN_FLOAT(basis[0]) && !IS_NAN_FLOAT(basis[1]) && !IS_NAN_FLOAT(basis[2]),
           s_assertDisable_Process_nanB0post);
    Assert(!IS_NAN_FLOAT(basis[3]) && !IS_NAN_FLOAT(basis[4]) && !IS_NAN_FLOAT(basis[5]),
           s_assertDisable_Process_nanB1post);
    Assert(!IS_NAN_FLOAT(basis[6]) && !IS_NAN_FLOAT(basis[7]) && !IS_NAN_FLOAT(basis[8]),
           s_assertDisable_Process_nanB2post);

    /* Retail forwards the clamp pair to transport.  subAreaFactor is the
     * CHANNEL clamp: GatherSkyLighting accumulates it into the per-subsample
     * slot that compile.c:847 later divides by channelWeight, and the gate
     * above guarantees it non-zero, which is what the transport-side
     * `subAreaFactor > 0` sanity checks rely on.  skyFactor is the TOTAL
     * clamp scaling the energy colors that NormalizeLightTransfers divides
     * by sample->areaX2. */
    if (!FindLightingTransfers_inner(mapping->param, worldPos, basis,
                                     subAreaWeight, areaWeight,
                                     mapping->primaryLightIndex,
                                     &sampleResult))
    {
        /* 0x40BC40 subtracts each clamp from its own accumulator when
         * transport rejects the cell. */
        AcquireThreadLock((unsigned int)(uintptr_t)sampleResult.lock);
        sampleResult.lock->weight -= areaWeight;
        sampleResult.lock->channelWeight[combinedIndex] -= subAreaWeight;
        ReleaseThreadLock((unsigned int)(uintptr_t)sampleResult.lock);
        Relight_RecordSuppressed(mapping->triangleIndex, areaIndex);
        goto done;
    }

    if (g_aoEnabled && g_aoFactors)
    {
        long long pixelIdx = (long long)((LightmapSample_t *)sampleResult.lock
                           - (LightmapSample_t *)g_lightingSamples);
        long long totalPixels = (long long)g_lightmapSize * 512 * 512;
        if (pixelIdx >= 0 && pixelIdx < totalPixels && g_aoFactors[pixelIdx] == 1.0f)
            g_aoFactors[pixelIdx] = ComputeAmbientOcclusion(worldPos, basis, g_aoSamples, g_aoDist);
    }

done:
    return;
}

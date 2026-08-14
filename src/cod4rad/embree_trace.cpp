/*
 * embree_trace.cpp — Embree 4 ray tracing integration for cod2rad64.
 * Compiled as C++. All exported functions use extern "C".
 */

#include <embree4/rtcore.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>

extern "C" {
#include "cod2rad64.h"
}

static RTCDevice g_embreeDevice;
static RTCScene  g_embreeScene;
extern "C" int g_useEmbree;
int g_useEmbree;

static unsigned int g_solidGeomID;
static unsigned int g_alphaGeomID;
static unsigned int g_modelGeomID;
static Triangle_t **g_solidPrimMap;
static Triangle_t **g_alphaPrimMap;
static CollisionTri_t **g_modelPrimMap;

extern "C" float g_vertPositions[][3];
extern "C" int g_triCount;
extern "C" Triangle_t g_triangles[];
extern "C" int TestAlphaMask(MaterialDef_t *si, float *texcoord);
extern "C" BspDrawVert_t bspDrawVerts[];
extern "C" CollisionInstance_t *g_collisionModelList;
extern "C" BSPSubdivNode_t g_bspSubdivNodes[];
extern "C" void MatrixInverse(float *in, float *out);
#define g_vertData ((DrawVert_t *)bspDrawVerts)


static void CountModelTris(CollisionAabbTree_t *node, int *count)
{
    if (node->childCount > 0)
    {
        for (int i = 0; i < node->childCount; i++)
            CountModelTris(&node->firstChild[i], count);
    }
    else
    {
        *count += node->itemCount;
    }
}

static void ReconstructTriVerts(CollisionTri_t *tri, float v0[3], float v1[3], float v2[3])
{
    float *n = tri->plane;
    float *a = tri->baryCoords0;
    float *b = tri->baryCoords1;

    float det = n[0]*(a[1]*b[2] - a[2]*b[1])
              - n[1]*(a[0]*b[2] - a[2]*b[0])
              + n[2]*(a[0]*b[1] - a[1]*b[0]);

    if (det == 0.0f || (det > -1e-10f && det < 1e-10f))
    {
        v0[0] = v0[1] = v0[2] = v1[0] = v1[1] = v1[2] = v2[0] = v2[1] = v2[2] = 3.402823e+38f;
        return;
    }
    float inv = 1.0f / det;

    float inv00 = (a[1]*b[2] - a[2]*b[1]) * inv;
    float inv01 = (n[2]*b[1] - n[1]*b[2]) * inv;
    float inv02 = (n[1]*a[2] - n[2]*a[1]) * inv;
    float inv10 = (a[2]*b[0] - a[0]*b[2]) * inv;
    float inv11 = (n[0]*b[2] - n[2]*b[0]) * inv;
    float inv12 = (n[2]*a[0] - n[0]*a[2]) * inv;
    float inv20 = (a[0]*b[1] - a[1]*b[0]) * inv;
    float inv21 = (n[1]*b[0] - n[0]*b[1]) * inv;
    float inv22 = (n[0]*a[1] - n[1]*a[0]) * inv;

    v0[0] = inv00*n[3] + inv01*a[3] + inv02*b[3];
    v0[1] = inv10*n[3] + inv11*a[3] + inv12*b[3];
    v0[2] = inv20*n[3] + inv21*a[3] + inv22*b[3];

    v1[0] = v0[0] + inv01;
    v1[1] = v0[1] + inv11;
    v1[2] = v0[2] + inv21;

    v2[0] = v0[0] + inv02;
    v2[1] = v0[1] + inv12;
    v2[2] = v0[2] + inv22;
}

static void EmitModelTris(CollisionAabbTree_t *node, float *fwd, float *origin,
                          float *vb, unsigned *ib, CollisionTri_t **primMap, int *cnt)
{
    if (node->childCount > 0)
    {
        for (int i = 0; i < node->childCount; i++)
            EmitModelTris(&node->firstChild[i], fwd, origin, vb, ib, primMap, cnt);
        return;
    }

    CollisionTri_t *tri = node->data;
    for (int i = 0; i < node->itemCount; i++, tri++)
    {
        float lv0[3], lv1[3], lv2[3];
        ReconstructTriVerts(tri, lv0, lv1, lv2);
        if (lv0[0] == 3.402823e+38f) continue;

        int c = *cnt;
        int b = c * 9;

        vb[b+0] = fwd[0]*lv0[0] + fwd[3]*lv0[1] + fwd[6]*lv0[2] + origin[0];
        vb[b+1] = fwd[1]*lv0[0] + fwd[4]*lv0[1] + fwd[7]*lv0[2] + origin[1];
        vb[b+2] = fwd[2]*lv0[0] + fwd[5]*lv0[1] + fwd[8]*lv0[2] + origin[2];

        vb[b+3] = fwd[0]*lv1[0] + fwd[3]*lv1[1] + fwd[6]*lv1[2] + origin[0];
        vb[b+4] = fwd[1]*lv1[0] + fwd[4]*lv1[1] + fwd[7]*lv1[2] + origin[1];
        vb[b+5] = fwd[2]*lv1[0] + fwd[5]*lv1[1] + fwd[8]*lv1[2] + origin[2];

        vb[b+6] = fwd[0]*lv2[0] + fwd[3]*lv2[1] + fwd[6]*lv2[2] + origin[0];
        vb[b+7] = fwd[1]*lv2[0] + fwd[4]*lv2[1] + fwd[7]*lv2[2] + origin[1];
        vb[b+8] = fwd[2]*lv2[0] + fwd[5]*lv2[1] + fwd[8]*lv2[2] + origin[2];

        ib[c*3+0] = c*3; ib[c*3+1] = c*3+1; ib[c*3+2] = c*3+2;
        primMap[c] = tri;
        (*cnt)++;
    }
}

static void EmbreeFilterAlpha(const RTCFilterFunctionNArguments *args)
{
    int *valid = args->valid;
    RTCHitN *hit = args->hit;
    unsigned int N = args->N;

    for (unsigned int i = 0; i < N; i++)
    {
        if (valid[i] != -1) continue;
        unsigned int primID = RTCHitN_primID(hit, N, i);
        Triangle_t *tri = g_alphaPrimMap[primID];
        if (tri && tri->material && tri->material->extraData)
        {
            float u = RTCHitN_u(hit, N, i);
            float v = RTCHitN_v(hit, N, i);
            float w = 1.0f - u - v;
            int v0 = tri->vertIndex[0], v1 = tri->vertIndex[1], v2 = tri->vertIndex[2];
            float tc[2];
            tc[0] = w*g_vertData[v0].texCoord[0] + u*g_vertData[v1].texCoord[0] + v*g_vertData[v2].texCoord[0];
            tc[1] = w*g_vertData[v0].texCoord[1] + u*g_vertData[v1].texCoord[1] + v*g_vertData[v2].texCoord[1];
            if (!TestAlphaMask(tri->material, tc))
                valid[i] = 0;
        }
    }
}

static void EmbreeFilterModelAlpha(const RTCFilterFunctionNArguments *args)
{
    int *valid = args->valid;
    RTCHitN *hit = args->hit;
    unsigned int N = args->N;

    for (unsigned int i = 0; i < N; i++)
    {
        if (valid[i] != -1) continue;
        unsigned int primID = RTCHitN_primID(hit, N, i);
        CollisionTri_t *tri = g_modelPrimMap[primID];
        if (tri && tri->surfaceRef && ((MaterialDef_t *)tri->surfaceRef)->extraData)
        {
            float u = RTCHitN_u(hit, N, i);
            float v = RTCHitN_v(hit, N, i);
            float w = 1.0f - u - v;
            float tc[2];
            tc[0] = w*tri->texcoord0[0] + u*tri->texcoord1[0] + v*tri->texcoord2[0];
            tc[1] = w*tri->texcoord0[1] + u*tri->texcoord1[1] + v*tri->texcoord2[1];
            if (!TestAlphaMask((MaterialDef_t *)tri->surfaceRef, tc))
                valid[i] = 0;
        }
    }
}

extern "C" void InitEmbreeScene(void)
{
    int solidCount = 0, alphaCount = 0;

    {
        extern int g_numThreads;
        char cfg[64];
        sprintf(cfg, "threads=%d,start_threads=0", g_numThreads);
        g_embreeDevice = rtcNewDevice(cfg);
    }
    if (!g_embreeDevice) { g_useEmbree = 0; return; }

    g_embreeScene = rtcNewScene(g_embreeDevice);
    rtcSetSceneBuildQuality(g_embreeScene, RTC_BUILD_QUALITY_HIGH);

    /* Count triangles using same filter as BuildCollisionBSP */
    for (int i = 0; i < g_triCount; i++)
    {
        Triangle_t *tri = &g_triangles[i];
        MaterialDef_t *mat = tri->material;

        if (tri->materialIdx != 0) continue;
        if (mat->contents & CONTENTS_TELEPORTER) continue;
        if (!(mat->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY))
            && !((mat->surfaceType & SURFTYPE_MASK) == SURFTYPE_PATCH)
            && !mat->extraData) continue;

        if (mat->extraData) alphaCount++; else solidCount++;
    }

    g_solidPrimMap = (Triangle_t **)malloc((solidCount > 0 ? solidCount : 1) * sizeof(Triangle_t *));
    g_alphaPrimMap = (Triangle_t **)malloc((alphaCount > 0 ? alphaCount : 1) * sizeof(Triangle_t *));

    /* Solid geometry */
    RTCGeometry solidGeom = rtcNewGeometry(g_embreeDevice, RTC_GEOMETRY_TYPE_TRIANGLE);
    float *sv = (float *)rtcSetNewGeometryBuffer(solidGeom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 12, (solidCount > 0 ? solidCount : 1) * 3);
    unsigned *si = (unsigned *)rtcSetNewGeometryBuffer(solidGeom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 12, (solidCount > 0 ? solidCount : 1));

    /* Alpha geometry */
    RTCGeometry alphaGeom = rtcNewGeometry(g_embreeDevice, RTC_GEOMETRY_TYPE_TRIANGLE);
    float *av = NULL; unsigned *ai = NULL;
    if (alphaCount > 0) {
        av = (float *)rtcSetNewGeometryBuffer(alphaGeom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 12, alphaCount * 3);
        ai = (unsigned *)rtcSetNewGeometryBuffer(alphaGeom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 12, alphaCount);
    }

    int sc = 0, ac = 0;
    for (int i = 0; i < g_triCount; i++)
    {
        Triangle_t *tri = &g_triangles[i];
        MaterialDef_t *mat = tri->material;

        /* Same filter as BuildCollisionBSP */
        if (tri->materialIdx != 0) continue;
        if (mat->contents & CONTENTS_TELEPORTER) continue;
        if (!(mat->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY))
            && !((mat->surfaceType & SURFTYPE_MASK) == SURFTYPE_PATCH)
            && !mat->extraData) continue;

        int v0 = tri->vertIndex[0], v1 = tri->vertIndex[1], v2 = tri->vertIndex[2];
        int isAlpha = (mat->extraData != NULL);

        float *vb; unsigned *ib; int *cnt; Triangle_t ***map;
        if (isAlpha && alphaCount > 0) { vb = av; ib = ai; cnt = &ac; map = &g_alphaPrimMap; }
        else { vb = sv; ib = si; cnt = &sc; map = &g_solidPrimMap; }

        int c = *cnt;
        int b = c * 9;
        vb[b+0]=g_vertPositions[v0][0]; vb[b+1]=g_vertPositions[v0][1]; vb[b+2]=g_vertPositions[v0][2];
        vb[b+3]=g_vertPositions[v1][0]; vb[b+4]=g_vertPositions[v1][1]; vb[b+5]=g_vertPositions[v1][2];
        vb[b+6]=g_vertPositions[v2][0]; vb[b+7]=g_vertPositions[v2][1]; vb[b+8]=g_vertPositions[v2][2];
        ib[c*3+0]=c*3; ib[c*3+1]=c*3+1; ib[c*3+2]=c*3+2;
        (*map)[c] = tri;
        (*cnt)++;
    }

    rtcSetGeometryMask(solidGeom, 0x01);
    rtcCommitGeometry(solidGeom);
    g_solidGeomID = rtcAttachGeometry(g_embreeScene, solidGeom);
    rtcReleaseGeometry(solidGeom);

    if (alphaCount > 0) {
        rtcSetGeometryMask(alphaGeom, 0x01);
        rtcSetGeometryOccludedFilterFunction(alphaGeom, EmbreeFilterAlpha);
        rtcSetGeometryIntersectFilterFunction(alphaGeom, EmbreeFilterAlpha);
        rtcCommitGeometry(alphaGeom);
        g_alphaGeomID = rtcAttachGeometry(g_embreeScene, alphaGeom);
        rtcReleaseGeometry(alphaGeom);
    } else {
        rtcReleaseGeometry(alphaGeom);
        g_alphaGeomID = (unsigned)-1;
    }

    /* Model collision geometry — walk BSP subdivision tree to find all instances,
     * reconstruct vertices from plane/bary, transform to world space */
    {
        int modelTriCount = 0;
        int nodeIdx;
        CollisionInstance_t *inst;

        for (nodeIdx = 0; nodeIdx < 1024; nodeIdx++)
        {
            for (inst = g_bspSubdivNodes[nodeIdx].list; inst; inst = inst->next)
                CountModelTris(inst->mesh->collTree, &modelTriCount);
        }

        if (modelTriCount > 0)
        {
            g_modelPrimMap = (CollisionTri_t **)malloc(modelTriCount * sizeof(CollisionTri_t *));

            RTCGeometry modelGeom = rtcNewGeometry(g_embreeDevice, RTC_GEOMETRY_TYPE_TRIANGLE);
            float *mv = (float *)rtcSetNewGeometryBuffer(modelGeom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 12, modelTriCount * 3);
            unsigned *mi = (unsigned *)rtcSetNewGeometryBuffer(modelGeom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 12, modelTriCount);
            int mc = 0;

            for (nodeIdx = 0; nodeIdx < 1024; nodeIdx++)
            {
                for (inst = g_bspSubdivNodes[nodeIdx].list; inst; inst = inst->next)
                {
                    float fwdMatrix[9];
                    MatrixInverse(inst->invMatrix, fwdMatrix);
                    EmitModelTris(inst->mesh->collTree, fwdMatrix, inst->origin, mv, mi, g_modelPrimMap, &mc);
                }
            }

            rtcSetGeometryMask(modelGeom, 0x02);
            rtcSetGeometryOccludedFilterFunction(modelGeom, EmbreeFilterModelAlpha);
            rtcCommitGeometry(modelGeom);
            g_modelGeomID = rtcAttachGeometry(g_embreeScene, modelGeom);
            rtcReleaseGeometry(modelGeom);
        }
        else
        {
            g_modelGeomID = (unsigned)-1;
        }
    }

    rtcCommitScene(g_embreeScene);
    g_useEmbree = 1;
}

extern "C" int TraceStaticModels_Embree(float *startPos, float *endPos)
{
    float dx = endPos[0]-startPos[0], dy = endPos[1]-startPos[1], dz = endPos[2]-startPos[2];
    float lenSq = dx*dx + dy*dy + dz*dz;
    if (lenSq < 0.000001f) return 0;
    float invLen = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(lenSq)));
    float len = lenSq * invLen;

    RTCRay shadow;
    shadow.org_x = startPos[0]; shadow.org_y = startPos[1]; shadow.org_z = startPos[2];
    shadow.dir_x = dx * invLen; shadow.dir_y = dy * invLen; shadow.dir_z = dz * invLen;
    shadow.tnear = 0.01f;
    shadow.tfar = len - 0.01f;
    shadow.mask = 0x02;
    shadow.id = 0;
    shadow.flags = 0;
    shadow.time = 0.0f;

    rtcOccluded1(g_embreeScene, &shadow, NULL);
    return shadow.tfar < 0.0f;
}

extern "C" int TraceVisibility_Embree(float *startPos, float *endPos)
{
    float dx = endPos[0]-startPos[0], dy = endPos[1]-startPos[1], dz = endPos[2]-startPos[2];
    float lenSq = dx*dx + dy*dy + dz*dz;
    if (lenSq < 0.000001f) return 1;
    float invLen = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(lenSq)));
    float len = lenSq * invLen;

    RTCRay shadow;
    shadow.org_x = startPos[0]; shadow.org_y = startPos[1]; shadow.org_z = startPos[2];
    shadow.dir_x = dx * invLen; shadow.dir_y = dy * invLen; shadow.dir_z = dz * invLen;
    shadow.tnear = 0.01f;
    shadow.tfar = len - 0.01f;
    shadow.mask = 0x01;
    shadow.id = 0;
    shadow.flags = 0;
    shadow.time = 0.0f;

    rtcOccluded1(g_embreeScene, &shadow, NULL);
    return shadow.tfar >= 0.0f;
}

/*
 * TraceSetup_Embree — closest-hit intersection using Embree.
 * Replaces TraceSetup_and_Dispatch + TraceBSP_r for the main lighting pipeline.
 */
extern "C" void TraceSetup_Embree(int cacheIndex, float *startPos, float *endPos, RayHitResult_t *hitResult)
{
    float dx = endPos[0]-startPos[0], dy = endPos[1]-startPos[1], dz = endPos[2]-startPos[2];
    float lenSq = dx*dx + dy*dy + dz*dz;

    hitResult->triangle = NULL;
    hitResult->fraction = 1.0f;
    hitResult->baryU = 0.0f;
    hitResult->baryV = 0.0f;

    if (lenSq < 0.000001f) return;

    float invLen = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(lenSq)));
    float len = lenSq * invLen;

    RTCRayHit rayhit;
    rayhit.ray.org_x = startPos[0]; rayhit.ray.org_y = startPos[1]; rayhit.ray.org_z = startPos[2];
    rayhit.ray.dir_x = dx * invLen; rayhit.ray.dir_y = dy * invLen; rayhit.ray.dir_z = dz * invLen;
    rayhit.ray.tnear = 0.001f;
    rayhit.ray.tfar = len;
    rayhit.ray.mask = 0x01;
    rayhit.ray.id = 0;
    rayhit.ray.flags = 0;
    rayhit.ray.time = 0.0f;
    rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
    rayhit.hit.primID = RTC_INVALID_GEOMETRY_ID;

    rtcIntersect1(g_embreeScene, &rayhit, NULL);

    if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID)
    {
        Triangle_t *tri = NULL;
        if (rayhit.hit.geomID == g_solidGeomID)
            tri = g_solidPrimMap[rayhit.hit.primID];
        else if (rayhit.hit.geomID == g_alphaGeomID)
            tri = g_alphaPrimMap[rayhit.hit.primID];

        if (tri)
        {
            hitResult->triangle = tri;
            hitResult->fraction = rayhit.ray.tfar / len;
            hitResult->baryU = rayhit.hit.u;
            hitResult->baryV = rayhit.hit.v;
        }
    }
}

extern "C" int rand_int(void);

extern "C" float ComputeAmbientOcclusion(float *pos, float *basis, int numSamples, float maxDist)
{
    if (!g_embreeScene)
        return 1.0f;

    float origin[3];
    origin[0] = pos[0] + basis[6] * 0.1f;
    origin[1] = pos[1] + basis[7] * 0.1f;
    origin[2] = pos[2] + basis[8] * 0.1f;

    float invMaxDist = 1.0f / maxDist;
    float occlusionSum = 0.0f;

    for (int i = 0; i < numSamples; i++)
    {
        float u1 = (float)rand_int() / 32767.0f;
        float u2 = (float)rand_int() / 32767.0f;

        float r = sqrtf(u1);
        float theta = 6.2831853f * u2;
        float localX = r * cosf(theta);
        float localY = r * sinf(theta);
        float localZ = sqrtf(1.0f - u1);

        float dirX = localX * basis[0] + localY * basis[3] + localZ * basis[6];
        float dirY = localX * basis[1] + localY * basis[4] + localZ * basis[7];
        float dirZ = localX * basis[2] + localY * basis[5] + localZ * basis[8];

        RTCRayHit rayhit;
        rayhit.ray.org_x = origin[0]; rayhit.ray.org_y = origin[1]; rayhit.ray.org_z = origin[2];
        rayhit.ray.dir_x = dirX; rayhit.ray.dir_y = dirY; rayhit.ray.dir_z = dirZ;
        rayhit.ray.tnear = 0.0f;
        rayhit.ray.tfar = maxDist;
        rayhit.ray.mask = 0x03;
        rayhit.ray.id = 0;
        rayhit.ray.flags = 0;
        rayhit.ray.time = 0.0f;
        rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;

        rtcIntersect1(g_embreeScene, &rayhit, NULL);

        if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID)
            occlusionSum += 1.0f - rayhit.ray.tfar * invMaxDist;
    }

    return 1.0f - occlusionSum / (float)numSamples;
}

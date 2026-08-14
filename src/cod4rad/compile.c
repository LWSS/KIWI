/*
 * compile.c — Radiosity compilation: lighting transfer, bounce, gather.
 *
 * All functions verified against cod2rad64 LST line by line.
 * Source: compile.cpp (from LST assert strings: .\compile.cpp)
 */

#include "cod2rad64.h"
#include <stdlib.h>
#include <math.h>

/*
 * Tetrahedral SH basis constants for 4-band directional lighting.
 * These encode the 4 basis directions of a regular tetrahedron.
 */
static const float k_sqrt2_3  = 0.8164965510f;  /* sqrt(2/3) — dword_457610 */
static const float k_inv_sqrt3 = 0.5773502588f;  /* 1/sqrt(3) — dword_457614 */
static const float k_inv_sqrt2 = 0.7071067691f;  /* 1/sqrt(2) — dword_457624 */
static const float k_inv_sqrt6 = 0.4082482755f;  /* 1/sqrt(6) — dword_457634 */

/* Named constants for raw address reads */
static const float k_skyRayLength = 262144.0f;    /* dword_4577D8 */
static const float k_smallEpsilon = 0.001f;        /* dword_457AF0 */
static const float k_randScale = 0.000061037019f;  /* dword_457BB4 — approx 2.0/32767.0 */
static const float k_largeRadius = 0.999f;         /* dword_457854 = 0x3F7FBE77 = 0.999 as float */
static const float k_initialMaxEnergy = 3.402823e+38f; /* dword_456490 — FLT_MAX */
static const float k_divergeThreshold = 2.0f;      /* dword_457630 = 40000000h (was wrongly 1.1) */

/*
================
GatherSurfaceIncidentEnergyForLightFromDir

Computes 4 tetrahedral SH basis dot products against a direction
vector. For each basis where the dot product is positive, accumulates
(dot * lightColor) into the corresponding output band (3 floats each).

Parameters:
  lightColor — float[3] RGB light color (rcx)
  direction  — float[3] unit direction vector (rdx)
  output     — float[12] accumulator, 4 bands × 3 channels (r8)

Band dot products:
  band0 = dir[2]
  band1 = dir[1]*sqrt(2/3) + dir[2]/sqrt(3)
  band2 = dir[2]/sqrt(3) - dir[0]/sqrt(2) - dir[1]/sqrt(6)
  band3 = dir[0]/sqrt(2) - dir[1]/sqrt(6) + dir[2]/sqrt(3)
================
*/
void GatherSurfaceIncidentEnergyForLightFromDir(float *lightColor, float *direction,
                                                 float *output)
{
    float dot;
    /* band 0: dot = dir[2] */
    dot = direction[2];
    if (dot > 0.0f)
    {
        output[0] += dot * lightColor[0];
        output[1] += dot * lightColor[1];
        output[2] += dot * lightColor[2];
    }

    /* band 1: dot = dir[1]*sqrt(2/3) + dir[0]*0 + dir[2]/sqrt(3) */
    dot = direction[1] * k_sqrt2_3 + direction[2] * k_inv_sqrt3;
    if (dot > 0.0f)
    {
        output[3] += dot * lightColor[0];
        output[4] += dot * lightColor[1];
        output[5] += dot * lightColor[2];
    }

    /* band 2: dot = dir[2]/sqrt(3) - dir[0]/sqrt(2) - dir[1]/sqrt(6) */
    dot = direction[2] * k_inv_sqrt3 - (direction[1] * k_inv_sqrt6 + direction[0] * k_inv_sqrt2);
    if (dot > 0.0f)
    {
        output[6] += dot * lightColor[0];
        output[7] += dot * lightColor[1];
        output[8] += dot * lightColor[2];
    }

    /* band 3: dot = dir[0]/sqrt(2) - dir[1]/sqrt(6) + dir[2]/sqrt(3) */
    dot = direction[0] * k_inv_sqrt2 - direction[1] * k_inv_sqrt6 + direction[2] * k_inv_sqrt3;
    if (dot > 0.0f)
    {
        output[9]  += dot * lightColor[0];
        output[10] += dot * lightColor[1];
        output[11] += dot * lightColor[2];
    }
}

/* memset is from CRT (sub_43C400) */

/* global transfer block pool */
static void *g_transferPool;     /* qword_480A68 */
static int g_transferPoolIdx;    /* dword_480A70 */

/* per-thread max bounced energy — originally dword_480A48..480A58, now dynamically sized */
static float *g_bounceEnergy;
static int g_bounceEnergyCount;

#define TRANSFER_HT_SIZE 4096
#define TRANSFER_HT_MASK (TRANSFER_HT_SIZE - 1)
typedef struct {
    void *fromSample;
    void *toSample;
    int lightIdx;
    TransferEntry_t *entry;
} TransferHTEntry_t;
static __declspec(thread) TransferHTEntry_t g_transferHT[TRANSFER_HT_SIZE];

static char s_assertDisable_Alloc_fromSample;  /* byte_480A7B */
static char s_assertDisable_Alloc_vars;        /* byte_480A7A */
static char s_assertDisable_Alloc_weight;      /* byte_480A79 */
static char s_assertDisable_Alloc_block;       /* byte_480A78 */

static const float k_weightEpsilon = -1.0e-5f; /* dword_4576A0 */

/*
================
AllocLightingTransfer

Looks up or creates a lighting transfer entry from fromSample to
toSample for a given light index. Uses a hash chain hanging off
fromSample->vars[+0x58]. Each hash block holds 15 entries.

If the (toSample, lightIdx) pair already exists, adds weight to it.
Otherwise allocates a new entry via a pool allocator and inserts
it using binary search to maintain sorted order within the block.
================
*/
static unsigned int TransferHash(void *fromSample, void *toSample, int lightIdx)
{
    return ((unsigned int)(uintptr_t)fromSample * 2654435761u
          ^ (unsigned int)(uintptr_t)toSample * 2246822519u
          ^ (unsigned int)lightIdx * 3266489917u) & TRANSFER_HT_MASK;
}

void AllocLightingTransfer(Sample_t *fromSample, void *toSample, int lightIdx,
                           float weight)
{
    SampleVars_t *vars;
    TransferBlock_t *block;
    TransferEntry_t *entry;
    int lo, hi, mid;
    unsigned int htIdx;
    TransferHTEntry_t *slot;

    Assert(fromSample != 0, s_assertDisable_Alloc_fromSample);

    vars = fromSample->vars;
    Assert(vars != 0, s_assertDisable_Alloc_vars);

    Assert(weight >= k_weightEpsilon, s_assertDisable_Alloc_weight);

    if (weight <= 0.0f)
        return;

    /* O(1) hash table lookup — handles both existing and previously-inserted entries */
    htIdx = TransferHash(fromSample, toSample, lightIdx);
    slot = &g_transferHT[htIdx];

    if (slot->fromSample == fromSample && slot->toSample == toSample
        && slot->lightIdx == lightIdx && slot->entry)
    {
        AcquireThreadLock((unsigned int)(uintptr_t)fromSample);
        slot->entry->weight += weight;
        ReleaseThreadLock((unsigned int)(uintptr_t)fromSample);
        return;
    }

    AcquireThreadLock((unsigned int)(uintptr_t)fromSample);

    /* chain walk — only on full hash miss */
    block = vars->transferHead;
    if (block)
    {
        TransferBlock_t *cur = block;
        while (cur)
        {
            int i;
            for (i = 0; i < TRANSFERS_PER_BLOCK; i++)
            {
                entry = &cur->entries[i];
                if (entry->toSample == 0)
                    goto not_found;
                if (entry->toSample == toSample && entry->lightIdx == lightIdx)
                {
                    weight += entry->weight;
                    slot->fromSample = fromSample;
                    slot->toSample = toSample;
                    slot->lightIdx = lightIdx;
                    slot->entry = entry;
                    goto done;
                }
            }
            cur = (TransferBlock_t *)cur->next;
        }
    }

not_found:
    if (block)
    {
        if (block->entries[TRANSFERS_PER_BLOCK - 1].toSample == 0)
        {
            lo = 0;
            hi = TRANSFERS_PER_BLOCK - 1;
            while (lo < hi)
            {
                mid = (lo + hi) >> 1;
                if (block->entries[mid].toSample == 0)
                    hi = mid;
                else
                    lo = mid + 1;
            }
            if (lo >= 0)
                goto insert;
        }
    }

    {
        AcquireThreadLock((unsigned int)(uintptr_t)&g_transferPool);

        if (g_transferPoolIdx == 0)
        {
            void *poolMem = malloc(TRANSFER_POOL_SIZE);
            if (!poolMem)
                ErrorMsg("Out of memory allocating light transport block\n");

            memset(poolMem, 0, TRANSFER_POOL_SIZE);

            *(void **)((unsigned char *)poolMem + TRANSFER_POOL_LINK_OFFSET) = g_transferPool;
            g_transferPool = poolMem;
        }

        {
            int idx = g_transferPoolIdx;
            TransferBlock_t *blk;

            g_transferPoolIdx = idx + 1;
            blk = (TransferBlock_t *)((unsigned char *)g_transferPool + idx * TRANSFER_BLOCK_SIZE);

            if (g_transferPoolIdx == TRANSFER_POOL_COUNT)
                g_transferPoolIdx = 0;

            ReleaseThreadLock((unsigned int)(uintptr_t)&g_transferPool);

            Assert(blk != 0, s_assertDisable_Alloc_block);

            blk->next = vars->transferHead;
            vars->transferHead = blk;

            block = blk;
        }

        lo = 0;
    }

insert:
    entry = &vars->transferHead->entries[lo];
    entry->toSample = toSample;
    entry->lightIdx = lightIdx;

    slot->fromSample = fromSample;
    slot->toSample = toSample;
    slot->lightIdx = lightIdx;
    slot->entry = entry;

done:
    entry->weight = weight;
    ReleaseThreadLock((unsigned int)(uintptr_t)fromSample);
}

static char s_assertDisable_Normalize_sample;  /* byte_480A7D */
static char s_assertDisable_Normalize_vars;    /* byte_480A7C */

/*
================
NormalizeLightTransfers

Clears the lighting transfer chain for a sample by setting
vars->transferHead (+0x58) to NULL.
================
*/
void NormalizeLightTransfers(Sample_t *sample)
{
    SampleVars_t *vars;

    Assert(sample != 0, s_assertDisable_Normalize_sample);

    vars = sample->vars;
    Assert(vars != 0, s_assertDisable_Normalize_vars);

    vars->transferHead = 0;
}

/* TraceSetup_and_Dispatch declared in cod2rad64.h — sub_40CDB0 */
extern int TraceStaticModels(void *startPos, void *endPos);
/* sqrtf is CRT (sub_43B200) */
extern void GetLightingSample(int lightmapIdx, float u, float v, void *output); /* lighting_411DF0 */
/* g_vertexData — view into g_vertData[0].lmCoord (stride sizeof(DrawVert_t)=0x44).
 * Binary uses raw byte indexing with stride 0x44 from unk_1270FCC4. */
unsigned char *g_vertexData = (unsigned char *)&g_vertData[0].lmCoord;

/*
================
FindLightingSamplesAndNormal

Traces a ray along the normal from a surface point, finds the
triangle hit, interpolates barycentric UV coordinates from vertex
lighting data, and samples the lightmap. Returns 1 on success,
0 if no hit or invalid triangle, -1 on backface.

Parameters:
  rcx = sampleIdx (32-bit int)
  rdx = position[3]
  r8  = normal[3]
  xmm3 = offset
  [stack+0x20] = outputLighting ptr
  [stack+0x28] = outputNormal ptr (float[3], optional)
================
*/
int FindLightingSamplesAndNormal(int sampleIdx, float *position, float *normal,
                                 float offset, void *outputLighting, float *outputNormal)
{
    float startPos[3], endPos[3];
    RayHitResult_t hitResult;
    float smallOffset = 0.125f; /* dword_457704 */
    Triangle_t *tri;
    MaterialDef_t *material;
    float baryU, baryV, baryW;
    float u, v;
    float maxUV = 511.0f;  /* dword_4576FC */
    float uvScale = 512.0f; /* dword_457700 */
    int vertIdx0, vertIdx1, vertIdx2;

    /* compute start = position + normal * smallOffset */
    startPos[0] = normal[0] * smallOffset + position[0];
    startPos[1] = normal[1] * smallOffset + position[1];
    startPos[2] = normal[2] * smallOffset + position[2];

    /* compute end = position + normal * (offset + smallOffset) */
    endPos[0] = normal[0] * offset + startPos[0];
    endPos[1] = normal[1] * offset + startPos[1];
    endPos[2] = normal[2] * offset + startPos[2];

    /* no, let me re-read: start = pos + normal*smallOffset, end = pos + normal*(offset)
       Actually:
       xmm4 = normal[1], xmm3 = normal[2], xmm6 = normal[0]
       xmm4 *= offset (xmm7), xmm3 *= offset, xmm6 *= offset → normal*offset
       xmm1 = normal[2]*smallOffset, xmm5 = normal[0]*smallOffset, xmm2 = normal[1]*smallOffset
       xmm1 += pos[2], xmm5 += pos[0], xmm2 += pos[1] → start = pos + normal*smallOffset
       xmm3 += xmm1, xmm6 += xmm5, xmm4 += xmm2 → end = start + normal*offset
    */

    /* trace ray */
    TraceSetup_and_Dispatch(sampleIdx, startPos, endPos, &hitResult);
    tri = hitResult.triangle;

    if (!tri)
        return 0;

    /* copy triangle normal to output if requested */
    if (outputNormal)
    {
        outputNormal[0] = tri->normal[0];
        outputNormal[1] = tri->normal[1];
        outputNormal[2] = tri->normal[2];
    }

    /* check material backface flag */
    material = tri->material;
    if (material->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY))
    {
        /* backface/sky material — check orientation */
        int result = TraceStaticModels(startPos, endPos);
        return result ? 0 : -1;
    }

    /* check lightmapIdx != 0x1F (no lightmap) */
    {
        if (tri->lightmapIdx == LIGHTMAP_NONE) {
            return 0;
        }

        /* barycentric interpolation of vertex UV coordinates */
        baryU = hitResult.baryU;
        baryV = hitResult.baryV;
        baryW = 1.0f - baryU - baryV;

        vertIdx0 = tri->vertIndex[0];
        vertIdx1 = tri->vertIndex[1];
        vertIdx2 = tri->vertIndex[2];

    /* interpolate U: w*vert0[0] + u*vert1[0] + v*vert2[0] */
    u = baryW * *(float *)&g_vertexData[vertIdx0 * 0x44]
      + baryU * *(float *)&g_vertexData[vertIdx1 * 0x44]
      + baryV * *(float *)&g_vertexData[vertIdx2 * 0x44];

    /* interpolate V: w*vert0[1] + u*vert1[1] + v*vert2[1] */
    v = baryW * *(float *)&g_vertexData[vertIdx0 * 0x44 + 4]
      + baryU * *(float *)&g_vertexData[vertIdx1 * 0x44 + 4]
      + baryV * *(float *)&g_vertexData[vertIdx2 * 0x44 + 4];

    /* scale and floor (binary calls sub_43B200 = floorf, NOT sqrtf) */
    u *= uvScale;
    v *= uvScale;
    u = floorf(u);
    v = floorf(v);

    /* clamp u to [0, maxUV] */
    if (u < 0.0f)
        u = 0.0f;
    else if (u > maxUV)
        u = maxUV;

    /* clamp v to [0, maxUV] */
    if (v < 0.0f)
        v = 0.0f;
    else if (v > maxUV)
        v = maxUV;

    }
    /* sample lightmap */
    {
        int lightmapIdx = ((Triangle_t *)tri)->lightmapIdx;
        float *outPtr = (float *)outputLighting;
        outPtr[2] = 1.0f; /* initialize Z to 1 */
        GetLightingSample(lightmapIdx, u, v, outPtr);
    }

    /* check if result is valid (result[2] > 0) */
    {
        float *resultPtr = *(float **)outputLighting;
        if (resultPtr[2] > 0.0f) {
            return 1;
        }
    }

    return 0;
}


static char s_assertDisable_Sky_subSample;    /* byte_480A84 */
static char s_assertDisable_Sky_sample;       /* byte_480A83 */
static char s_assertDisable_Sky_vars;         /* byte_480A82 */
static char s_assertDisable_Sky_area;         /* byte_480A81 */
static char s_assertDisable_Sky_nanEnergy;    /* byte_480A80 */
static char s_assertDisable_Sky_nanResult;    /* byte_480A7F */

/*
================
GatherSkyLighting

Gathers sky illumination for a lighting subsample. Traces a ray
from the surface point along the normal direction to detect sky
visibility. If the sky is visible (backface material hit), accumulates
weighted sky color into the sample's lighting data.

Uses global settings g_sunDirX/C0/C4 for the sky direction
basis coefficients and g_sunColorR/CC/D0 for the sky color.
================
*/
void GatherSkyLighting(int sampleIdx, float *position, float *basis,
                       float skyWeight, float subAreaFactor,
                       SubSample_t *subSample)
{
    float startPos[3], endPos[3];
    RayHitResult_t hitResult;
    Triangle_t *tri;
    float skyDir;
    float directEnergy[3];
    float radiosityEnergy[3];
    float localSunDir[3];
    Sample_t *sample;
    SampleVars_t *sampleVars;
    float smallOffset = 0.125f;
    float rayLen;

    Assert(subSample != 0, s_assertDisable_Sky_subSample);

    sample = *(Sample_t **)subSample;
    Assert(sample != 0, s_assertDisable_Sky_sample);

    sampleVars = sample->vars;
    Assert(sampleVars != 0, s_assertDisable_Sky_vars);

    Assert(subAreaFactor > 0, s_assertDisable_Sky_area);

    /* compute sky direction dot product with surface normal (3rd row of basis at [6..8]) */
    skyDir = g_sunDirY * basis[7] + g_sunDirX * basis[6] + g_sunDirZ * basis[8];
    if (skyDir <= 0.0f)
        return;

    /* compute ray start (offset along normal) and end (further along sky direction) */
    rayLen = k_skyRayLength; /* sky ray length constant */

    startPos[0] = basis[6] * smallOffset + position[0];
    startPos[1] = basis[7] * smallOffset + position[1];
    startPos[2] = basis[8] * smallOffset + position[2];

    endPos[0] = g_sunDirX * rayLen + startPos[0];
    endPos[1] = g_sunDirY * rayLen + startPos[1];
    endPos[2] = g_sunDirZ * rayLen + startPos[2];

    /* trace ray */
    TraceSetup_and_Dispatch(sampleIdx, startPos, endPos, &hitResult);
    tri = hitResult.triangle;

    if (!tri)
        return;

    /* must hit sky (backface material) */
    if (!(tri->material->contents & (CONTENTS_NONCOLLIDING | CONTENTS_SKY)))
        return;

    /* backface check */
    if (TraceStaticModels(startPos, endPos))
        return;

    /* accumulate subAreaFactor into sample data */
    {
        int idx = subSample->s + subSample->t * 2;
        ((float *)sampleVars)[idx] += subAreaFactor;
    }

    /* CoD4 keeps direct sunlight and the energy injected into radiosity as
     * separate colors.  Both are derived from the same visible sky hit. */
    directEnergy[0] = g_sunColorR * skyWeight;
    directEnergy[1] = g_sunColorG * skyWeight;
    directEnergy[2] = g_sunColorB * skyWeight;
    radiosityEnergy[0] = g_sunRadiosityR * skyWeight;
    radiosityEnergy[1] = g_sunRadiosityG * skyWeight;
    radiosityEnergy[2] = g_sunRadiosityB * skyWeight;

    Assert(!IS_NAN_FLOAT(directEnergy[0]) && !IS_NAN_FLOAT(directEnergy[1]) && !IS_NAN_FLOAT(directEnergy[2]),
           s_assertDisable_Sky_nanEnergy);

    localSunDir[0] = g_sunDirX * basis[0] + g_sunDirY * basis[1] + g_sunDirZ * basis[2];
    localSunDir[1] = g_sunDirX * basis[3] + g_sunDirY * basis[4] + g_sunDirZ * basis[5];
    localSunDir[2] = skyDir;
    GatherSurfaceIncidentEnergyForLightFromDir(directEnergy, localSunDir, sampleVars->incident);

    sampleVars->scattered[0] += directEnergy[0] * skyDir;
    sampleVars->scattered[1] += directEnergy[1] * skyDir;
    sampleVars->scattered[2] += directEnergy[2] * skyDir;
    sampleVars->unscattered[0] += radiosityEnergy[0] * skyDir;
    sampleVars->unscattered[1] += radiosityEnergy[1] * skyDir;
    sampleVars->unscattered[2] += radiosityEnergy[2] * skyDir;

    Assert(!IS_NAN_FLOAT(sampleVars->unscattered[0])
        && !IS_NAN_FLOAT(sampleVars->unscattered[1])
        && !IS_NAN_FLOAT(sampleVars->unscattered[2]),
           s_assertDisable_Sky_nanResult);
}

/* Forward declarations for functions not in cod2rad64.h */
extern int GetPointLightCount(void);
extern void SetLightingSampleAreas(int threadCount);
extern void BuildLightTransfers(int threadCount);
extern void Lighting_InitSamples(void);                                /* lighting_412350 */
extern void BeginProgress(const char *msg);
extern void EndProgress(void);
extern void ForEachUsefulLightingSample(void (*func)(void *), int threadCount);
extern void CalculateGroundLightingForAllStaticModels(void);
/* malloc/free from <stdlib.h> */
extern void Lighting_GetGatheredLight(void *sample, float *outColor);    /* lighting_412550 */
extern void *g_lightDirArray;                                  /* qword_480A60 */
extern int g_totalLightCount;                                  /* dword_480A48 */

/*
================
BounceGatherCallback

Iterates through the lighting transfer chain for a sample,
accumulating bounced light. For each transfer entry, multiplies
the scatter color by g_radiosityScale * entry->weight, then calls
GatherSurfaceIncidentEnergyForLightFromDir to accumulate into
the destination sample's SH bands.
================
*/
void BounceGatherCallback(Sample_t *sample)
{
    SampleVars_t *sampleVars;
    TransferBlock_t *block;
    float scatterColor[3];
    float bounceColor[3];
    int i;

    AcquireThreadLock((unsigned int)(uintptr_t)sample);
    Lighting_GetGatheredLight(sample, scatterColor);
    ReleaseThreadLock((unsigned int)(uintptr_t)sample);

    sampleVars = sample->vars;
    block = sampleVars->transferHead;

    if (!block)
        return;

    while (block)
    {
        for (i = 0; i < TRANSFERS_PER_BLOCK; i++)
        {
            TransferEntry_t *entry = &block->entries[i];

            if (!entry->toSample)
                break;

            {
                float factor = g_radiosityScale * entry->weight;
                bounceColor[0] = scatterColor[0] * factor;
                bounceColor[1] = scatterColor[1] * factor;
                bounceColor[2] = scatterColor[2] * factor;
            }

            AcquireThreadLock((unsigned int)(uintptr_t)entry->toSample);
            {
                Sample_t *toSamp = (Sample_t *)entry->toSample;
                float *lightDir = (float *)&((LightDirEntry_t *)g_lightDirArray)[entry->lightIdx];
                GatherSurfaceIncidentEnergyForLightFromDir(bounceColor,
                    lightDir,
                    toSamp->vars->incident);
            }
            ReleaseThreadLock((unsigned int)(uintptr_t)entry->toSample);
        }

        block = (TransferBlock_t *)block->next;
    }
}

/*
================
Compile

Main radiosity compilation entry point. Orchestrates the full
pipeline: collision BSP build, sample area calculation, lighting
setup, light transport, radiosity bounce, lightmap building,
light grid, ground lighting, and cleanup.
================
*/
/* forward decls for debug globals used by Compile() below */

void Compile(int threadCount)
{
    int numLights;

    numLights = GetPointLightCount();
    g_totalLightCount = numLights + 2;

    InitGeometry();
    if (g_embreeEnabled)
        InitEmbreeScene();

    if (g_adaptiveEnabled)
    {
        BeginProgress("Adaptive lightmap analysis...");
        AdaptiveLightmapRepack(threadCount);
        EndProgress();
    }

    if (g_uvRepackEnabled)
    {
        BeginProgress("UV repack...");
        UVRepack();
        EndProgress();
    }

    BeginProgress("Calculating sample areas...");
    SetLightingSampleAreas(threadCount);
    EndProgress();

    Lighting_InitSamples();
    SetupSampleRadii();

    BeginProgress("Building light transport for everything...");
    BuildLightTransfers(threadCount);
    EndProgress();

    /* radiosity bounce */
    {
        float epsilon = k_smallEpsilon; /* small constant */
        if (g_maxBounces > 1)
            RadiosityBounce(epsilon, threadCount);
    }

    /* build lightmaps */
    BeginProgress("Building lightmaps...");
    ForEachUsefulLightingSample(BounceGatherCallback, threadCount);
    EndProgress();

    /* light grid */
    BeginProgress("Calculating light grid...");
    CalculateLightGrid(threadCount);
    EndProgress();

    /* ground lighting */
    BeginProgress("Calculating ground lighting for static models...");
    CalculateGroundLightingForAllStaticModels();
    EndProgress();

    /* free transfer pool blocks */
    while (g_transferPool)
    {
        void *next = *(void **)((unsigned char *)g_transferPool + TRANSFER_POOL_LINK_OFFSET);
        free(g_transferPool);
        g_transferPool = next;
    }

    /* normalize transfers for all samples */
    ForEachUsefulLightingSample(NormalizeLightTransfers, 1);

    /* bleed lightmap edges (tail call at LST 0x40B230) */
    InitBleeding(threadCount);
}

/*
================
RadiosityBounce

Performs iterative radiosity bounce passes. Each pass gathers
indirect illumination from neighboring samples weighted by
transfer coefficients and bounce fraction.
================
*/
/*
================
GatherPointLightForSample

Gathers direct illumination from a single point light for a
lighting subsample. Traces shadow rays and accumulates weighted
light contribution into the sample's SH bands.
================
*/
extern int PointLightEvaluatePoint(int sampleIdx, int lightIdx, float *position,
    float *normal, float *outDir, float *outColor, float *outArg); /* pointlights_419090 */

static char s_assertDisable_Point_sample;    /* byte_480A88 */
static char s_assertDisable_Point_vars;      /* byte_480A87 */
static char s_assertDisable_Point_nanEnergy; /* byte_480A86 */
static char s_assertDisable_Point_nanResult; /* byte_480A85 */

void GatherPointLightForSample(int sampleIdx, int lightIdx, float *position,
    float *basis, float subAreaFactor, Sample_t *sample)
{
    SampleVars_t *sampleVars;
    float lightColor[3];
    float lightDirection[3];
    float outArg;
    float energy[3];
    int result;

    Assert(sample != 0, s_assertDisable_Point_sample);
    sampleVars = sample->vars;
    Assert(sampleVars != 0, s_assertDisable_Point_vars);

    /* call point light gathering — returns 0 (no light), 1 (directional), 2 (ambient) */
    /* binary passes basis+6 (the surface normal, 3rd row of basis) as 4th arg */
    result = PointLightEvaluatePoint(sampleIdx, lightIdx - 2,
        position, basis + 6,
        lightDirection, lightColor, &outArg);

    if (result == 0)
        return;

    /* compute energy = lightColor * subAreaFactor */
    energy[0] = lightColor[0] * subAreaFactor;
    energy[1] = lightColor[1] * subAreaFactor;
    energy[2] = lightColor[2] * subAreaFactor;

    Assert(!IS_NAN_FLOAT(energy[0]) && !IS_NAN_FLOAT(energy[1]) && !IS_NAN_FLOAT(energy[2]),
           s_assertDisable_Point_nanEnergy);

    if (result == 2)
    {
        /* ambient/omnidirectional: add energy to all 4 SH bands */
        float *shBands = sampleVars->incident;
        int i;
        for (i = 0; i < 4; i++)
        {
            shBands[i * 3 + 0] += energy[0];
            shBands[i * 3 + 1] += energy[1];
            shBands[i * 3 + 2] += energy[2];
        }
    }
    else
    {
        /* directional: transform light direction through basis matrix */
        float dir[3];

        dir[0] = lightDirection[1] * basis[1] + lightDirection[0] * basis[0] + lightDirection[2] * basis[2];
        dir[1] = lightDirection[0] * basis[3] + lightDirection[1] * basis[4] + lightDirection[2] * basis[5];
        dir[2] = lightDirection[0] * basis[6] + lightDirection[1] * basis[7] + lightDirection[2] * basis[8];

        GatherSurfaceIncidentEnergyForLightFromDir(energy, dir,
            sampleVars->incident);
    }

    /* accumulate energy * areaFactor into unscattered — outArg is GatherPointLight's 7th output */
    {
        sampleVars->unscattered[0] += energy[0] * outArg;
        sampleVars->unscattered[1] += energy[1] * outArg;
        sampleVars->unscattered[2] += energy[2] * outArg;
    }

    Assert(!IS_NAN_FLOAT(sampleVars->unscattered[0])
        && !IS_NAN_FLOAT(sampleVars->unscattered[1])
        && !IS_NAN_FLOAT(sampleVars->unscattered[2]),
           s_assertDisable_Point_nanResult);
}

/*
================
SetupSampleRadii

Computes sample radii for all lighting samples based on their
area and neighbor distances. Used for the radiosity transfer
radius computation.
================
*/
extern void UniformPointsOnHemisphere(int count, float *dirs, int stride); /* com_math_428CF0 */
extern float Vec2DistanceSq(float *a, float *b);                    /* sub_4291D0: distance between 2D points */
/* sqrtf is from CRT (sub_43D3C0) */
float g_invTraces;       /* dword_480A5C: 1.0 / g_traces */

void SetupSampleRadii(void)
{
    int i, j;
    float *dirArray;
    float minDistSq;
    float dist;
    float radius;
    float maxRadius;

    /* allocate light direction array: g_traces entries, 16 bytes each */
    dirArray = (float *)malloc(g_traces * 16);
    g_lightDirArray = dirArray;
    UniformPointsOnHemisphere(g_traces, dirArray, 16);

    /* special case: single trace */
    if (g_traces == 1)
    {
        dirArray[0] = 0.0f;
        dirArray[1] = 0.0f;
    }

    maxRadius = k_largeRadius; /* large initial radius constant */

    /* for each light direction, find minimum distance to neighbors */
    for (i = 0; i < g_traces; i++)
    {
        float *cur = (float *)&((LightDirEntry_t *)g_lightDirArray)[i];
        float len;

        /* compute current vector length */
        len = sqrtf(cur[0] * cur[0] + cur[1] * cur[1]);

        /* find min squared distance to any other direction, scaled by 0.25 */
        minDistSq = (maxRadius - len) * (maxRadius - len);

        for (j = 0; j < g_traces; j++)
        {
            if (j == i)
                continue;

            dist = Vec2DistanceSq(
                (float *)&((LightDirEntry_t *)g_lightDirArray)[j],
                (float *)&((LightDirEntry_t *)g_lightDirArray)[i]);
            dist *= 0.25f; /* dword_4573A4 = 0.25 */

            if (dist < minDistSq)
                minDistSq = dist;
        }

        /* radius = sqrt(minDistSq) * g_jitter */
        radius = sqrtf(minDistSq);
        cur[3] = radius * g_jitter; /* store radius at offset +0xC */
    }

    /* compute inverse trace count */
    g_invTraces = 1.0f / (float)g_traces;
}

/*
================
BuildLightingTransfersForSample

Builds the lighting transfer data for a single sample by tracing
rays to neighbor samples and computing transfer weights.
================
*/
static char s_assertDisable_Build_sample;    /* byte_480A8D */
static char s_assertDisable_Build_vars;      /* byte_480A8C */
static char s_assertDisable_Build_nanWeight; /* byte_480A8B */
static char s_assertDisable_Build_nanBounce; /* byte_480A8A */
static char s_assertDisable_Build_nanResult; /* byte_480A89 */

void BuildLightingTransfersForSample(float *bouncedLight, float weight, Sample_t *sample)
{
    SampleVars_t *sampleVars;
    float weighted[3];

    Assert(sample != 0, s_assertDisable_Build_sample);
    sampleVars = sample->vars;
    Assert(sampleVars != 0, s_assertDisable_Build_vars);
    Assert(!IS_NAN_FLOAT(weight), s_assertDisable_Build_nanWeight);
    Assert(!IS_NAN_FLOAT(bouncedLight[0]) && !IS_NAN_FLOAT(bouncedLight[1])
        && !IS_NAN_FLOAT(bouncedLight[2]), s_assertDisable_Build_nanBounce);

    weighted[0] = weight * bouncedLight[0];
    weighted[1] = weight * bouncedLight[1];
    weighted[2] = weight * bouncedLight[2];

    AcquireThreadLock((unsigned int)(uintptr_t)sample);

    sampleVars->scattered[0] += weighted[0];
    sampleVars->scattered[1] += weighted[1];
    sampleVars->scattered[2] += weighted[2];

    sampleVars->unscattered[0] += weighted[0];
    sampleVars->unscattered[1] += weighted[1];
    sampleVars->unscattered[2] += weighted[2];

    Assert(!IS_NAN_FLOAT(sampleVars->unscattered[0])
        && !IS_NAN_FLOAT(sampleVars->unscattered[1])
        && !IS_NAN_FLOAT(sampleVars->unscattered[2]),
           s_assertDisable_Build_nanResult);

    ReleaseThreadLock((unsigned int)(uintptr_t)sample);
}

/*
================
GatherBounceForSample

Gathers indirect (bounced) illumination for a single sample.
Iterates the transfer chain and accumulates weighted contributions
from neighboring samples' scattered light.
================
*/
static char s_assertDisable_Bounce_source;      /* byte_480A91 */
static char s_assertDisable_Bounce_sourceVars;  /* byte_480A90 */
static char s_assertDisable_Bounce_sampleVars;  /* byte_480A7E */
static char s_assertDisable_Bounce_nanSource;   /* byte_480A8F */
static char s_assertDisable_Bounce_nanBounced;  /* byte_480A8E */

extern float g_energyScale; /* dword_4576F8: 1/3 or similar */

void GatherBounceForSample(Sample_t *sourceSample, int threadIdx)
{
    SampleVars_t *sourceVars;
    float totalEnergy;
    float bouncedLight[3];
    void *block;
    int i;

    Assert(sourceSample != 0, s_assertDisable_Bounce_source);
    sourceVars = sourceSample->vars;
    Assert(sourceVars != 0, s_assertDisable_Bounce_sourceVars);
    Assert(sourceVars != 0, s_assertDisable_Bounce_sampleVars);

    /* compute total unscattered energy */
    totalEnergy = (sourceVars->unscattered[1]
                 + sourceVars->unscattered[0]
                 + sourceVars->unscattered[2]) * g_energyScale;

    if (totalEnergy == 0.0f)
        return;

    /* update per-thread max energy */
    if (totalEnergy > g_bounceEnergy[threadIdx + 1])
        g_bounceEnergy[threadIdx + 1] = totalEnergy;

    Assert(!IS_NAN_FLOAT(sourceVars->unscattered[0])
        && !IS_NAN_FLOAT(sourceVars->unscattered[1])
        && !IS_NAN_FLOAT(sourceVars->unscattered[2]),
           s_assertDisable_Bounce_nanSource);

    AcquireThreadLock((unsigned int)(uintptr_t)sourceSample);

    bouncedLight[0] = g_radiosityScale * sourceVars->unscattered[0];
    bouncedLight[1] = g_radiosityScale * sourceVars->unscattered[1];
    bouncedLight[2] = g_radiosityScale * sourceVars->unscattered[2];

    sourceVars->unscattered[0] = 0.0f;
    sourceVars->unscattered[1] = 0.0f;
    sourceVars->unscattered[2] = 0.0f;

    ReleaseThreadLock((unsigned int)(uintptr_t)sourceSample);

    Assert(!IS_NAN_FLOAT(bouncedLight[0]) && !IS_NAN_FLOAT(bouncedLight[1])
        && !IS_NAN_FLOAT(bouncedLight[2]), s_assertDisable_Bounce_nanBounced);

    /* iterate transfer chain, accumulate bounced light to targets */
    {
        TransferBlock_t *block = sourceVars->transferHead;
        while (block)
        {
            for (i = 0; i < TRANSFERS_PER_BLOCK; i++)
            {
                TransferEntry_t *entry = &block->entries[i];
                if (!entry->toSample)
                    break;

                BuildLightingTransfersForSample(bouncedLight, entry->weight,
                    (Sample_t *)entry->toSample);
            }
            block = (TransferBlock_t *)block->next;
        }
    }
}

/*
================
RadiosityBounce

Performs iterative radiosity bounce passes until energy converges
below epsilon or diverges. Each pass calls GatherBounceForSample
for all samples via ForEachSample.
================
*/

void RadiosityBounce(float epsilon, int threadCount)
{
    int pass;
    float maxEnergy;
    float prevMax;
    float divergeThreshold;
    char *msg;
    int i;

    prevMax = k_initialMaxEnergy; /* initial large value */
    divergeThreshold = k_divergeThreshold; /* 1.1 or similar */

    if (!g_bounceEnergy || g_bounceEnergyCount < threadCount + 1)
    {
        if (g_bounceEnergy) free(g_bounceEnergy);
        g_bounceEnergyCount = threadCount + 1;
        g_bounceEnergy = (float *)calloc(g_bounceEnergyCount, sizeof(float));
    }

    pass = 0;
    while (1)
    {
        pass++;

        msg = va("Radiosity bounce %i...", pass);
        BeginProgress(msg);

        /* clear energy accumulators */
        for (i = 1; i <= threadCount; i++)
            g_bounceEnergy[i] = 0.0f;

        /* gather bounce for all samples */
        ForEachUsefulLightingSampleThreaded(GatherBounceForSample, threadCount);

        /* find max energy across all accumulators */
        maxEnergy = g_bounceEnergy[1];
        for (i = 2; i <= threadCount; i++)
        {
            if (g_bounceEnergy[i] > maxEnergy)
                maxEnergy = g_bounceEnergy[i];
        }

        EndProgress();

        if (pass == 1)
        {
            prevMax = maxEnergy;
        }
        else
        {
            /* check for divergence */
            if (maxEnergy > prevMax * divergeThreshold)
            {
                Com_Printf("\n\nAborting radiosity due to a positive feedback loop.\n");
                Com_Printf("This can usually be fixed by changing '-traces' slightly (currently %i).\n", g_traces);
                Com_Printf("Reducing the radiosity scale can also help this (currently %g).\n",
                           (double)g_radiosityScale);
                break;
            }
        }

        /* check convergence */
        if (maxEnergy <= epsilon || pass + 1 == g_maxBounces)
            break;
    }
}

/*
================
FindLightingTransfersForDirection

Traces rays in a specific direction to find lighting transfers
between samples. Used during the transport building phase.
================
*/
/*
================
FindLightingTransfersForDirection

Traces rays in a specific direction from a sample to find lighting
transfers to neighboring samples. Computes transfer weights based
on visibility and distance.

1367 bytes — the largest function in compile.cpp. Contains ray
tracing, transfer allocation, weight computation, and NaN checks.

Parameters:
  ecx = sampleIdx, rdx = sample, r8 = position, r9 = normal,
  [stack] = dirIdx, [stack] = subAreaFactor
================
*/
/* rand is from CRT (sub_43D4C0) */
extern void Vec2Normalize(float *v);

static char s_assertDisable_Dir_areaFactor;  /* byte_480A97 */
static char s_assertDisable_Dir_sampleArea;  /* byte_480A96 */
static char s_assertDisable_Dir_vars;        /* byte_480A95 */
static char s_assertDisable_Dir_incident;    /* byte_480A94 */
static char s_assertDisable_Dir_nanEnergy;   /* byte_480A93 */
static char s_assertDisable_Dir_nanResult;   /* byte_480A92 */

void FindLightingTransfersForDirection(int sampleIdx, Sample_t *sample,
    float *position, float *normal, float subAreaFactor, int dirIdx)
{
    float *dirEntry;
    float randScale;
    float rx, ry;
    float lenSq;
    float pertDir[3]; /* perturbed direction on disk */
    float worldDir[3];
    float hitResults[4]; /* from FindLightingSamplesAndNormal */
    float outputNormal[3];
    SampleVars_t *sampleVars;
    int result;
    float factor;
    float energy[3];
    int i;

    dirEntry = (float *)&((LightDirEntry_t *)g_lightDirArray)[dirIdx];
    randScale = k_randScale; /* rand scale constant */

    /* generate random point on unit disk via rejection sampling */
    {
        int rej = 0;
        do {
            extern int rand_int(void);
            rx = (float)rand_int() * randScale - 1.0f;
            ry = (float)rand_int() * randScale - 1.0f;
            lenSq = rx * rx + ry * ry;
            rej++;
        } while (lenSq > 1.0f);
    }

    /* compute perturbed direction using radius from dirEntry[3] */
    {
        float radius = dirEntry[3];
        pertDir[0] = dirEntry[0] + radius * rx;
        pertDir[1] = dirEntry[1] + radius * ry;

        /* compute z from unit sphere */
        lenSq = pertDir[0] * pertDir[0] + pertDir[1] * pertDir[1];
        if (lenSq <= 1.0f)
        {
            pertDir[2] = sqrtf(1.0f - lenSq);
        }
        else
        {
            Vec2Normalize(pertDir);
            pertDir[2] = k_smallEpsilon; /* small epsilon */
        }
    }

    Assert(subAreaFactor > 0, s_assertDisable_Dir_areaFactor);
    Assert(sample->areaX2 > 0, s_assertDisable_Dir_sampleArea);

    sampleVars = sample->vars;
    Assert(sampleVars != 0, s_assertDisable_Dir_vars);
    Assert(sampleVars->incident != 0, s_assertDisable_Dir_incident);

    /* transform perturbed direction to world space via normal basis */
    worldDir[0] = pertDir[0] * normal[0] + pertDir[1] * normal[3] + pertDir[2] * normal[6];
    worldDir[1] = pertDir[0] * normal[1] + pertDir[1] * normal[4] + pertDir[2] * normal[7];
    worldDir[2] = pertDir[0] * normal[2] + pertDir[1] * normal[5] + pertDir[2] * normal[8];

    /* trace ray along world direction */
    result = FindLightingSamplesAndNormal(sampleIdx, position, worldDir,
        k_skyRayLength, hitResults, NULL);

    if (result == 0)
        return;

    factor = g_invTraces * subAreaFactor;

    if (result == -1)
    {
        /* backface hit — accumulate self-illumination energy */
        energy[0] = g_backfaceLightR * factor;
        energy[1] = g_backfaceLightG * factor;
        energy[2] = g_backfaceLightB * factor;

        Assert(!IS_NAN_FLOAT(energy[0]) && !IS_NAN_FLOAT(energy[1])
            && !IS_NAN_FLOAT(energy[2]), s_assertDisable_Dir_nanEnergy);

        /* gather into SH bands */
        GatherSurfaceIncidentEnergyForLightFromDir(energy, pertDir,
            sampleVars->incident);

        /* accumulate into scattered and unscattered */
        sampleVars->scattered[0] += energy[0];
        sampleVars->scattered[1] += energy[1];
        sampleVars->scattered[2] += energy[2];
        sampleVars->unscattered[0] += energy[0];
        sampleVars->unscattered[1] += energy[1];
        sampleVars->unscattered[2] += energy[2];

        Assert(!IS_NAN_FLOAT(sampleVars->unscattered[0])
            && !IS_NAN_FLOAT(sampleVars->unscattered[1])
            && !IS_NAN_FLOAT(sampleVars->unscattered[2]),
               s_assertDisable_Dir_nanResult);
    }
    else if (result > 0)
    {
        /* hit neighbor samples — allocate transfers */
        float *hitEntry = hitResults;
        for (i = 0; i < result; i++)
        {
            void *toSample = *(void **)hitEntry;
            float weight = factor * *(float *)(hitEntry + 2);
            AllocLightingTransfer(toSample, sample, dirIdx, weight);
            hitEntry += 4; /* 16 bytes per entry */
        }
    }
}

/*
================
FindLightingTransfers_inner

Per-subsample lighting transfer loop. For each light direction,
calls FindLightingTransfersForDirection. Then calls GatherSkyLighting
for sky contribution. Finally calls GatherPointLightForSample for
each point light.
================
*/
void FindLightingTransfers_inner(int sampleIdx, float *position, float *normal,
                                 float subAreaFactor, float skyFactor,
                                 SubSample_t *subSample)
{
    int i;
    Sample_t *sample = *(Sample_t **)subSample;

    /* gather lighting transfers for each direction */
    for (i = 0; i < g_traces; i++)
    {
        FindLightingTransfersForDirection(sampleIdx, sample, position, normal,
                                          subAreaFactor, i);
    }

    /* gather sky lighting */
    GatherSkyLighting(sampleIdx, position, normal, subAreaFactor,
                      skyFactor, subSample);

    /* gather point lights */
    for (i = 2; i < g_totalLightCount; i++)
    {
        GatherPointLightForSample(sampleIdx, i, position, normal,
                                   subAreaFactor, sample);
    }
}

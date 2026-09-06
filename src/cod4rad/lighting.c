/*
 * lighting.c — Lightmap allocation, light sampling, gamma correction, final lightmap building
 */

#include "cod2rad64.h"

/* The recovered MSVC 2005 powf experiment depends on a private lookup-table
 * header that was never included in this source bundle.  Keep the standalone
 * build reproducible with the CRT implementation; native Cod4Rad math-runtime
 * classification is tracked independently from compiler-owned lighting code. */

extern void memcpy_fast(void *dst, const void *src, int size);
extern void BuildFinalLightmap_PerPixel(int lightmapIdx, int col, int row);

float          g_ambientR;
float          g_ambientG;
float          g_ambientB;
float          g_contrastScale = 2.0f;

/* g_contrastGamma is the same variable as g_contrastGain — alias so cmdline
 * or worldspawn override flows through to AdjustLightingContrast */
extern float g_contrastGain;
#define g_contrastGamma g_contrastGain

/* YUV luminance weights — bit-exact from binary .rdata */
float          g_lumWeightR = 0.2989999949932098388671875f;
float          g_lumWeightG = 0.5870000123977661132812500f;
float          g_lumWeightB = 0.1140000000596046447753906f;
unsigned char g_lightmapOutput[MAX_RAD_LIGHTMAP_BYTES];

void          *g_lightingSampleCallback;
static void   *g_lightingThreadSampleCallback;
void          *g_lightingPixelCallback;
int            g_lightmapSize = 0;
void          *g_lightingSamples;
int            g_usefulSampleCount = 0;
int            g_lightSourceCount;
float         *g_aoFactors;

/* g_degamma is the same variable as g_gamma — alias for readability */
extern float g_gamma;
#define g_degamma g_gamma

void          *g_sampleVarsPool;
int            g_totalSampleCount;
float          g_energyScale = 0.333333343f; /* 1/3 */
int            g_totalLightCount;
float          g_lightScale = 2.0f;
/* Map_Write also asks for final lightmaps as a legacy write-time operation.
 * Retail performs that conversion during Compile, before ground/grid work.
 * Keep the later call harmless without running the quantizer twice. */
static int      s_finalLightmapsBuilt;
void          *g_sunDirGlobals;

/* lightmap_bleed.cpp adjacency masks and exact 3x3 kernel tables. */
extern void *g_primaryBleedData;
extern void *g_secondaryBleedData;
extern float g_bleedNeighborWeight[9];
extern unsigned char g_bleedNeighborMask[9];

/* g_numTraceDirections is the same variable as g_lightmapHeight */
extern int g_lightmapHeight;
#define g_numTraceDirections g_lightmapHeight

float         *g_traceDirections;
float         *g_lightDirArray;

/* 8 SH basis direction vectors (sky + 6 tetrahedral + sky-up) */
const float g_shBasis[24] = {
    0.0f, 0.0f, -1.0f,
    0.942809045f, 0.0f, 0.333333343f,
    -0.471404523f, 0.816496551f, 0.333333343f,
    0.471404523f, 0.816496551f, 0.333333343f,
    -0.471404523f, -0.816496551f, -0.333333343f,
    0.471404523f, -0.816496551f, -0.333333343f,
    -0.942809045f, 0.0f, -0.333333343f,
    0.0f, 0.0f, 1.0f
};

#define SAMPLE_VARS_SIZE ((int)sizeof(SampleVars_t))

/*
================
Lighting_AllocLightmapData

Allocate the lightmap data buffer.
================
*/
void Lighting_AllocLightmapData(void)
{
    Assert("(lightingGlob.lmapCount >= 0)", ".\\lighting.cpp", 0x36, 0, 1);

    g_lightingSamples = malloc((unsigned long long)g_lightmapSize * LIGHTMAP_DATA_SIZE);
    if (!g_lightingSamples)
    {
        ErrorMsg("Couldn't allocate %g MB for lightmap data in %i lightmaps\n",
                    (double)((float)g_lightmapSize * (float)LIGHTMAP_DATA_SIZE / (1024.0f * 1024.0f)),
                    g_lightmapSize);
    }

    memset(g_lightingSamples, 0, (unsigned long long)g_lightmapSize << 23);

    g_lightSourceCount = GetPointLightCount() + 2;

    if (g_aoEnabled)
    {
        long long totalPixels = (long long)g_lightmapSize * 512 * 512;
        g_aoFactors = (float *)malloc(totalPixels * sizeof(float));
        if (!g_aoFactors)
            ErrorMsg("Couldn't allocate AO buffer (%lld pixels)\n", totalPixels);
        for (long long i = 0; i < totalPixels; i++)
            g_aoFactors[i] = 1.0f;
        Com_Printf("AO enabled: %d samples, %.0f unit distance, %lld pixels\n",
                    g_aoSamples, (double)g_aoDist, totalPixels);
    }
}

/*
================
Lighting_RegisterLightmap

Register a lightmap index, update max count.
================
*/
void Lighting_RegisterLightmap(int lmapIndex)
{
    int newCount;

    Assert("lmapIndex != LIGHTMAP_NONE", ".\\lighting.cpp", 0x0D, 0, 1);
    Assert("lightingGlob.lmapDefs == NULL", ".\\lighting.cpp", 0x2D, 0, 1);

    newCount = lmapIndex + 1;
    if (g_lightmapSize < newCount)
        g_lightmapSize = newCount;
}

/*
================
GetLightingSample

Look up a lighting sample by lightmap index and UV coords.
================
*/
void GetLightingSample(int lmapIndex, float sScaled, float tScaled, void **outSample)
{
    int s, t;

    Assert("(lmapIndex >= 0 && lmapIndex < ((124 * 512)))", ".\\lighting.cpp", 0x46, 0, 1);
    Assert("sample", ".\\lighting.cpp", 0x47, 0, 1);

    s = (int)floorf(sScaled);
    t = (int)floorf(tScaled);

    Assert("(s >= 0 && s < ((512 < 1024) ? 512 : 1024))", ".\\lighting.cpp", 0x4C, 0, 1);
    Assert("(t >= 0 && t < ((512 < 1024) ? 512 : 1024))", ".\\lighting.cpp", 0x4D, 0, 1);

    *outSample = (char *)g_lightingSamples
               + (((long long)lmapIndex * 512 + t) * 512 + s) * 32;
}

/*
================
GetLightingSubSample

Look up a subsample with different range checks.
================
*/
void GetLightingSubSample(int lmapIndex, float sScaled, float tScaled, SubSample_t *outSubSample)
{
    int s, t;

    Assert("(lmapIndex >= 0 && lmapIndex < ((124 * 512)))", ".\\lighting.cpp", 0x58, 0, 1);
    Assert("subSample", ".\\lighting.cpp", 0x59, 0, 1);

    s = (int)floorf(sScaled);
    t = (int)floorf(tScaled);

    Assert("(s >= 0 && s < ((512 < 1024) ? 512 : 1024))", ".\\lighting.cpp", 0x5E, 0, 1);
    Assert("(t >= 0 && t < ((512 < 1024) ? 512 : 1024))", ".\\lighting.cpp", 0x5F, 0, 1);

    outSubSample->s = s & 1;
    outSubSample->t = t & 1;

    outSubSample->sample = (Sample_t *)((char *)g_lightingSamples
                  + (((long long)lmapIndex * 512 + (t / 2)) * 512 + (s / 2)) * 32);
}

/*
================
DegammaColorChannel

Apply degamma (raise to g_gamma power).
================
*/
float DegammaColorChannel(float color)
{
    Assert("(color >= 0)", ".\\lighting.cpp", 0xBD, 0, 1);

    return powf(color, g_degamma);
}

/*
================
GammaCorrectColorChannel

Apply inverse gamma correction.
================
*/
float GammaCorrectColorChannel(float color)
{
    Assert("(color >= 0)", ".\\lighting.cpp", 0xCF, 0, 1);

    return powf(color, 1.0f / g_degamma);
}

/*
================
DegammaColor

Apply degamma to RGB color in place.
================
*/
void DegammaColor(float *color)
{
    /* KIWI FIX: the retail assert is a string literal (always true, see AUDIT_cod4rad F4),
       so a negative channel sailed into powf and came out NaN.  A negative colour has no
       meaning; clamp it so every caller (point, spot, sun, ambient) is safe. */
    if (color[0] < 0.0f) color[0] = 0.0f;
    if (color[1] < 0.0f) color[1] = 0.0f;
    if (color[2] < 0.0f) color[2] = 0.0f;

    Assert("(color >= 0)", ".\\lighting.cpp", 0xBD, 0, 1);
    color[0] = powf(color[0], g_degamma);

    Assert("(color >= 0)", ".\\lighting.cpp", 0xBD, 0, 1);
    color[1] = powf(color[1], g_degamma);

    Assert("(color >= 0)", ".\\lighting.cpp", 0xBD, 0, 1);
    color[2] = powf(color[2], g_degamma);
}

/*
================
Lighting_InitSamples

Count useful samples, allocate vars pool, assign vars.
================
*/
void Lighting_InitSamples(void)
{
    int allocSize;

    s_finalLightmapsBuilt = 0;

    Assert("lightingGlob.totalSampleCount == 0", ".\\lighting.cpp", 0x9D, 0, 1);

    g_totalSampleCount = g_lightmapSize << 18;

    Assert("lightingGlob.usefulSampleCount == 0", ".\\lighting.cpp", 0xA0, 0, 1);

    /* pass 1: count useful samples */
    g_lightingSampleCallback = (void *)Lighting_IncrementUsefulSampleCount;
    ForEachLightmapPixel(g_lightmapSize << 18,
                          (void *)Lighting_SampleCallback_Trampoline1, 1);

    Assert("lightingGlob.usefulSampleCount <= lightingGlob.totalSampleCount",
           ".\\lighting.cpp", 0xA3, 0, 1);

    /* allocate vars pool */
    allocSize = g_usefulSampleCount * SAMPLE_VARS_SIZE;
    g_sampleVarsPool = malloc((unsigned long long)allocSize);
    if (!g_sampleVarsPool)
        Com_Printf("Couldn't allocate %.2g MB for %i useful samples\n",
                    (double)((float)allocSize / (1024.0f * 1024.0f)),
                    g_usefulSampleCount);

    memset(g_sampleVarsPool, 0, allocSize);

    /* pass 2: assign vars to each useful sample */
    g_usefulSampleCount = 0;
    g_lightingSampleCallback = (void *)Lighting_AllocSampleVars;
    ForEachLightmapPixel(g_lightmapSize << 18,
                          (void *)Lighting_SampleCallback_Trampoline1, 1);
}

/*
================
Lighting_InitAntiBleed

Allocate anti-bleed vars for empty samples.
================
*/
void Lighting_InitAntiBleed(void)
{
    int antiBleedCount;
    int allocSize;
    char *antiBleedPool;
    int antiBleedIndex;
    int lmap, row, col;
    long long sampleOffset = 0;

    antiBleedCount = g_totalSampleCount - g_usefulSampleCount;
    allocSize = antiBleedCount * SAMPLE_VARS_SIZE;

    antiBleedPool = (char *)malloc((unsigned long long)antiBleedCount * SAMPLE_VARS_SIZE);
    if (!antiBleedPool)
        Com_Printf("Couldn't allocate %i bytes to fix lightmap bleeding\n", allocSize);

    memset(antiBleedPool, 0, allocSize);

    antiBleedIndex = 0;

    for (lmap = 0; lmap < g_lightmapSize; lmap++)
    {
        for (row = 0; row < 512; row++)
        {
            for (col = 0; col < 512; col++)
            {
                char *sampleSlot;

                sampleSlot = (char *)g_lightingSamples + sampleOffset;

                if (*(void **)sampleSlot == NULL)
                {
                    Assert("antiBleedIndex < lightingGlob.totalSampleCount - lightingGlob.usefulSampleCount",
                           ".\\lighting.cpp", 0x1AA, 0, 1);

                    *(void **)sampleSlot = antiBleedPool
                        + (long long)antiBleedIndex * SAMPLE_VARS_SIZE;
                    antiBleedIndex++;
                }

                sampleOffset += 32;
            }
        }
    }

    Assert("antiBleedIndex == lightingGlob.totalSampleCount - lightingGlob.usefulSampleCount",
           ".\\lighting.cpp", 0x1B1, 0, 1);
}

/*
================
InitBleeding

Init anti-bleed, then start bilinear bleeding pass.
================
*/
void InitBleeding(int threadCount)
{
    Lmap_InitBilinearBleeding(g_lightmapSize, threadCount);
}

/*
================
EncodeGammaCorrectedByte

Gamma correct a color channel, encode as byte [0..255].
================
*/
unsigned char EncodeGammaCorrectedByte(float value)
{
    float corrected;

    if (value <= 0.0f)
        return 0;
    if (value >= 1.0f)
        return 255;

    Assert("(color >= 0)", ".\\lighting.cpp", 0xCF, 0, 1);

    corrected = powf(value, 1.0f / g_degamma);

    if (corrected <= 0.0f)
        return 0;
    if (corrected >= 1.0f)
        return 255;

    return (unsigned char)(long long)floorf(corrected * 255.0f + 0.5f);
}

/*
================
BuildFinalLightmaps_TripleLoop

Iterate all lightmap pixels and build final data.
================
*/
void BuildFinalLightmaps_TripleLoop(void)
{
    int lmap, row, col;

    if (s_finalLightmapsBuilt)
        return;

    Com_Printf("Saving lightmaps...\n");

    for (lmap = 0; lmap < g_lightmapSize; lmap++)
    {
        for (row = 0; row < 512; row++)
        {
            for (col = 0; col < 512; col++)
            {
                BuildFinalLightmap_PerPixel(lmap, col, row);
            }
        }
    }

    /* CoD4 stores three MiB per 512x512 lightmap: two RGBA coefficient
     * images followed by one 1024x1024 scalar image. */
    { extern int numBSPLightBytes; numBSPLightBytes = g_lightmapSize * 0x300000; }
    s_finalLightmapsBuilt = 1;
}

#define MAX_NATIVE_LIGHTING_BASIS 256

/* lighting.cpp 0x414370 constructs the final-lightmap basis independently of
 * the transport trace directions.  Retail defaults to 32 directions. */
static float s_nativeLightmapBasis[MAX_NATIVE_LIGHTING_BASIS][3];
static int s_nativeLightmapBasisCount;

static int Lighting_InitNativeLightmapBasis(void)
{
    float radiusStep;
    float radius;
    float spiralX = 1.0f;
    float spiralY = 0.0f;
    int count = g_basisDirCount;
    int i;

    if (count < 16)
        count = 16;
    else if (count > MAX_NATIVE_LIGHTING_BASIS)
        count = MAX_NATIVE_LIGHTING_BASIS;

    if (s_nativeLightmapBasisCount == count)
        return count;

    radiusStep = 0.5f / (float)count;
    radius = 0.5f * radiusStep;
    for (i = 0; i < count; ++i)
    {
        float oldSpiralY;
        s_nativeLightmapBasis[i][0] = spiralX * radius;
        s_nativeLightmapBasis[i][1] = spiralY * radius;
        s_nativeLightmapBasis[i][2] = sqrtf(1.0f - radius * radius);
        radius += radiusStep;

        oldSpiralY = spiralY;
        spiralY = spiralY * -0.7373688817024231f
                - spiralX *  0.6754903197288513f;
        spiralX = spiralX * -0.7373688817024231f
                + oldSpiralY * 0.6754903197288513f;
    }

    s_nativeLightmapBasisCount = count;
    return count;
}

static float Lighting_Clamp01(float value)
{
    if (value <= 0.0f)
        return 0.0f;
    if (value >= 1.0f)
        return 1.0f;
    return value;
}

static int Lighting_ClampColor(float color[3])
{
    int changed = 0;
    int channel;

    for (channel = 0; channel < 3; ++channel)
    {
        float clamped = Lighting_Clamp01(color[channel]);
        if (clamped != color[channel])
            changed = 1;
        color[channel] = clamped;
    }

    return changed;
}

static unsigned char Lighting_EncodeCod4Byte(float value)
{
    /* Native cod4rad truncates here (the tiny positive bias is below one ULP
     * for useful values); it does not use the rounded CoD2 byte encoder. */
    if (value <= 0.0f)
        return 0;
    if (value >= 1.0f)
        return 255;
    return (unsigned char)(int)(value * 255.0f
                              + 9.313225746154785e-10f);
}

/* lighting.cpp 0x4144F0. */
static void Lighting_ProjectNativeBasis(const float color[3],
                                        const float direction[3],
                                        float *basisColors, int basisCount)
{
    int i;
    for (i = 0; i < basisCount; ++i)
    {
        float dot = s_nativeLightmapBasis[i][0] * direction[0]
                  + s_nativeLightmapBasis[i][1] * direction[1]
                  + s_nativeLightmapBasis[i][2] * direction[2];
        if (dot > 0.0f)
        {
            basisColors[i * 3 + 0] += color[0] * dot;
            basisColors[i * 3 + 1] += color[1] * dot;
            basisColors[i * 3 + 2] += color[2] * dot;
        }
    }
}

/* lighting.cpp 0x406A80: materialize the directional field from the native
 * sky/direct/inbound transport representation. */
static int Lighting_BuildNativeDirectionalField(const SampleVars_t *vars,
                                                float *basisColors)
{
    int basisCount = Lighting_InitNativeLightmapBasis();
    int i;
    TransferBlock_t *block;

    for (i = 0; i < basisCount; ++i)
    {
        basisColors[i * 3 + 0] = vars->coincident[0] + g_ambientR;
        basisColors[i * 3 + 1] = vars->coincident[1] + g_ambientG;
        basisColors[i * 3 + 2] = vars->coincident[2] + g_ambientB;
    }

    if (vars->skyInfluences && g_lightDirArray)
    {
        for (i = 0; i < g_traces; ++i)
        {
            float weight = vars->skyInfluences[i];
            if (weight != 0.0f)
            {
                const LightDirEntry_t *direction =
                    &((const LightDirEntry_t *)g_lightDirArray)[i];
                float color[3] = {
                    g_sunRadiosityR * weight,
                    g_sunRadiosityG * weight,
                    g_sunRadiosityB * weight
                };
                Lighting_ProjectNativeBasis(color, (const float *)direction,
                                            basisColors, basisCount);
            }
        }
    }

    for (i = 0; i < (int)vars->directTransportCount; ++i)
    {
        const DirectTransport_t *direct = &vars->directTransports[i];
        Lighting_ProjectNativeBasis(direct->color, direct->direction,
                                    basisColors, basisCount);
    }

    for (block = vars->transferHead; block; block = block->next)
    {
        int entryIndex;
        for (entryIndex = 0; entryIndex < TRANSFERS_PER_BLOCK; ++entryIndex)
        {
            const TransferEntry_t *entry = &block->entries[entryIndex];
            const SampleVars_t *sourceVars;
            const LightDirEntry_t *direction;
            float color[3];

            if (!entry->toSample)
                break;
            sourceVars = entry->toSample->vars;
            if (!sourceVars || entry->lightIdx < 0 || entry->lightIdx >= g_traces)
                continue;

            color[0] = sourceVars->gatheredIncident[0]
                     * sourceVars->scattered[0] * entry->weight;
            color[1] = sourceVars->gatheredIncident[1]
                     * sourceVars->scattered[1] * entry->weight;
            color[2] = sourceVars->gatheredIncident[2]
                     * sourceVars->scattered[2] * entry->weight;
            direction = &((const LightDirEntry_t *)g_lightDirArray)[entry->lightIdx];
            Lighting_ProjectNativeBasis(color, (const float *)direction,
                                        basisColors, basisCount);
        }
    }

    return basisCount;
}

static void Lighting_AdjustNativeContrast(float *colors, int sampleCount,
                                          int baseIndex);

/* lighting.cpp 0x4157B0: build the complete gamma/contrast-adjusted field
 * for a valid sample.  The final fit consumes this representation directly,
 * and 0x415990 blends neighbouring fields in this same space. */
static int Lighting_BuildFinalDirectionalField(int lmapIndex, int col, int row,
                                               const SampleVars_t *vars,
                                               float *basisColors)
{
    int basisCount = Lighting_BuildNativeDirectionalField(vars, basisColors);
    int i;
    int channel;

    if (g_aoEnabled && g_aoFactors)
    {
        long long pixelIndex = ((long long)lmapIndex * 512 + row) * 512 + col;
        float ao = g_aoFactors[pixelIndex];
        for (i = 0; i < basisCount * 3; ++i)
            basisColors[i] *= ao;
    }

    for (i = 0; i < basisCount; ++i)
    {
        for (channel = 0; channel < 3; ++channel)
        {
            float value = basisColors[i * 3 + channel];
            if (value < 0.0f)
                value = 0.0f;
            basisColors[i * 3 + channel] = GammaCorrectColorChannel(value);
        }
    }
    Lighting_AdjustNativeContrast(basisColors, basisCount, 0);
    return basisCount;
}

/* 0x416F80/0x416F20 use the direction mask stored on the neighbour and a
 * separable [1 6 1]^2 / 64 kernel.  The local mask table is indexed as
 * neighbour-minus-target (the reverse of the native table), which is exactly
 * how lightmap_bleed.c's existing traversal consumes it. */
static float Lighting_GetBleedNeighbourWeight(const unsigned char *maskBase,
                                              int width, int lmapIndex,
                                              int targetS, int targetT,
                                              int neighbourS, int neighbourT)
{
    int kernelIndex = (neighbourS - targetS + 1)
                    + 3 * (neighbourT - targetT + 1);
    long long maskIndex = ((long long)lmapIndex * width + neighbourT) * width
                        + neighbourS;

    if (!maskBase || kernelIndex < 0 || kernelIndex >= 9)
        return 0.0f;
    if (!(maskBase[maskIndex] & g_bleedNeighborMask[kernelIndex]))
        return 0.0f;
    return g_bleedNeighborWeight[kernelIndex];
}

/* lighting.cpp 0x415990: reconstruct an invalid coefficient texel from
 * adjacent valid full-resolution directional fields. */
static int Lighting_ReconstructDirectionalField(int lmapIndex, int col, int row,
                                                float *basisColors)
{
    int basisCount = Lighting_InitNativeLightmapBasis();
    float neighbourColors[MAX_NATIVE_LIGHTING_BASIS * 3];
    float weightSum = 0.0f;
    int minCol = col > 0 ? col - 1 : 0;
    int maxCol = col < 511 ? col + 1 : 511;
    int minRow = row > 0 ? row - 1 : 0;
    int maxRow = row < 511 ? row + 1 : 511;
    int neighbourRow;
    int neighbourCol;
    int i;

    memset(basisColors, 0, (size_t)basisCount * 3 * sizeof(float));
    for (neighbourRow = minRow; neighbourRow <= maxRow; ++neighbourRow)
    {
        for (neighbourCol = minCol; neighbourCol <= maxCol; ++neighbourCol)
        {
            LightmapSample_t *neighbour;
            SampleVars_t *neighbourVars;
            float weight;

            if (neighbourCol == col && neighbourRow == row)
                continue;
            neighbour = (LightmapSample_t *)g_lightingSamples
                      + ((long long)lmapIndex * 512 + neighbourRow) * 512
                      + neighbourCol;
            neighbourVars = neighbour->vars;
            if (!neighbourVars || !neighbourVars->validMask)
                continue;

            weight = Lighting_GetBleedNeighbourWeight(
                (const unsigned char *)g_primaryBleedData, 512, lmapIndex,
                col, row, neighbourCol, neighbourRow);
            if (weight == 0.0f)
                continue;

            Lighting_BuildFinalDirectionalField(lmapIndex, neighbourCol,
                                                neighbourRow, neighbourVars,
                                                neighbourColors);
            weightSum += weight;
            for (i = 0; i < basisCount * 3; ++i)
                basisColors[i] += neighbourColors[i] * weight;
        }
    }

    if (weightSum != 0.0f && weightSum != 1.0f)
    {
        float invWeight = 1.0f / weightSum;
        for (i = 0; i < basisCount * 3; ++i)
            basisColors[i] *= invWeight;
    }
    return basisCount;
}

/* lighting.cpp 0x415B80: fetch a valid primary-light subsample or reconstruct
 * it from the doubled 1024x1024 adjacency mask.  Retail intentionally adds
 * neighbour scalar values without multiplying by the kernel weight, then
 * divides that sum by the accumulated kernel weights. */
static int Lighting_GetFinalScalarSample(int lmapIndex, int col, int row,
                                         int subS, int subT, float *outValue)
{
    LightmapSample_t *sample = (LightmapSample_t *)g_lightingSamples
                             + ((long long)lmapIndex * 512 + row) * 512 + col;
    SampleVars_t *vars = sample->vars;
    int subIndex = subS + 2 * subT;
    int targetS;
    int targetT;
    int minS;
    int maxS;
    int minT;
    int maxT;
    int neighbourT;
    int neighbourS;
    float valueSum = 0.0f;
    float weightSum = 0.0f;

    if (vars && (vars->validMask & (1u << subIndex)))
    {
        *outValue = vars->intensity[subIndex];
        return 1;
    }

    *outValue = 0.0f;
    targetS = col * 2 + subS;
    targetT = row * 2 + subT;
    minS = targetS > 0 ? targetS - 1 : 0;
    maxS = targetS < 1023 ? targetS + 1 : 1023;
    minT = targetT > 0 ? targetT - 1 : 0;
    maxT = targetT < 1023 ? targetT + 1 : 1023;

    for (neighbourT = minT; neighbourT <= maxT; ++neighbourT)
    {
        for (neighbourS = minS; neighbourS <= maxS; ++neighbourS)
        {
            LightmapSample_t *neighbour;
            SampleVars_t *neighbourVars;
            int neighbourSubIndex;
            float weight;

            if (neighbourS == targetS && neighbourT == targetT)
                continue;
            neighbour = (LightmapSample_t *)g_lightingSamples
                      + ((long long)lmapIndex * 512 + (neighbourT >> 1)) * 512
                      + (neighbourS >> 1);
            neighbourVars = neighbour->vars;
            neighbourSubIndex = (neighbourS & 1) + 2 * (neighbourT & 1);
            if (!neighbourVars
             || !(neighbourVars->validMask & (1u << neighbourSubIndex)))
                continue;

            weight = Lighting_GetBleedNeighbourWeight(
                (const unsigned char *)g_secondaryBleedData, 1024, lmapIndex,
                targetS, targetT, neighbourS, neighbourT);
            if (weight == 0.0f)
                continue;
            weightSum += weight;
            valueSum += neighbourVars->intensity[neighbourSubIndex];
        }
    }

    if (weightSum == 0.0f)
        return 0;
    *outValue = valueSum / weightSum;
    return 1;
}

/* lighting.cpp 0x415D90: reconstruct all four scalar cells and fill any
 * unresolved cells with the mean of those that succeeded. */
static void Lighting_BuildFinalScalarSamples(int lmapIndex, int col, int row,
                                             float values[4])
{
    unsigned char valid[4];
    float sum = 0.0f;
    int validCount = 0;
    int subT;
    int subS;
    int i;

    for (subT = 0; subT < 2; ++subT)
    {
        for (subS = 0; subS < 2; ++subS)
        {
            int index = subS + 2 * subT;
            valid[index] = (unsigned char)Lighting_GetFinalScalarSample(
                lmapIndex, col, row, subS, subT, &values[index]);
            sum += values[index];
            validCount += valid[index];
        }
    }

    if (validCount > 0 && validCount < 4)
    {
        float average = sum / (float)validCount;
        for (i = 0; i < 4; ++i)
        {
            if (!valid[i])
                values[i] = average;
        }
    }
}

/* lighting.cpp 0x415270.  Unlike the old helper this operates in place and
 * does not add ambient; the native final producer adds ambient before gamma. */
static void Lighting_AdjustNativeContrast(float *colors, int sampleCount,
                                          int baseIndex)
{
    float luminance[MAX_NATIVE_LIGHTING_BASIS];
    float minLuminance = 3.402823466e+38f;
    float maxLuminance = -3.402823466e+38f;
    float baseLuminance;
    float contrast;
    int i;

    for (i = 0; i < sampleCount; ++i)
    {
        float value = colors[i * 3 + 0] * g_lumWeightR
                    + colors[i * 3 + 1] * g_lumWeightG
                    + colors[i * 3 + 2] * g_lumWeightB;
        luminance[i] = value;
        if (value < minLuminance) minLuminance = value;
        if (value > maxLuminance) maxLuminance = value;
    }

    baseLuminance = baseIndex == -1
                  ? (minLuminance + maxLuminance) * 0.5f
                  : luminance[baseIndex];
    contrast = maxLuminance - minLuminance;
    if (contrast != 0.0f && contrast < 0.5f)
    {
        float contrastScale = powf(contrast * 2.0f, -g_contrastGain);
        for (i = 0; i < sampleCount; ++i)
        {
            float adjusted = (luminance[i] - baseLuminance) * contrastScale
                           + baseLuminance;
            if (adjusted > 0.0f)
            {
                float scale = adjusted / luminance[i];
                colors[i * 3 + 0] *= scale;
                colors[i * 3 + 1] *= scale;
                colors[i * 3 + 2] *= scale;
            }
            else
            {
                colors[i * 3 + 0] = 0.0f;
                colors[i * 3 + 1] = 0.0f;
                colors[i * 3 + 2] = 0.0f;
            }
        }
    }
}

/* lighting.cpp 0x414E70. */
static void Lighting_DeriveNativeDirection(const float *colors, int count,
                                           float direction[3])
{
    float intensity[MAX_NATIVE_LIGHTING_BASIS];
    float weightedIntensity = 0.0f;
    float weightSum = 0.0f;
    float averageIntensity;
    float length;
    int i;

    for (i = 0; i < count; ++i)
    {
        float weight = s_nativeLightmapBasis[i][2] * 0.5f + 0.5f;
        intensity[i] = colors[i * 3 + 0]
                     + colors[i * 3 + 1]
                     + colors[i * 3 + 2];
        weightedIntensity += intensity[i] * weight;
        weightSum += weight;
    }

    averageIntensity = weightedIntensity / weightSum;
    direction[0] = direction[1] = direction[2] = 0.0f;
    for (i = 0; i < count; ++i)
    {
        float weight = s_nativeLightmapBasis[i][2] * 0.5f + 0.5f;
        float residual = intensity[i] - weight * averageIntensity;
        direction[0] += s_nativeLightmapBasis[i][0] * residual;
        direction[1] += s_nativeLightmapBasis[i][1] * residual;
        direction[2] += s_nativeLightmapBasis[i][2] * residual;
    }

    length = sqrtf(direction[0] * direction[0]
                 + direction[1] * direction[1]
                 + direction[2] * direction[2]);
    if (length > 0.0f)
    {
        float invLength = 1.0f / length;
        direction[0] *= invLength;
        direction[1] *= invLength;
        direction[2] *= invLength;
    }
}

/* lighting.cpp 0x4149D0/0x414940. */
static void Lighting_FitNativeCoefficients(const float *colors, int count,
                                           const float direction[3],
                                           float ambient[3],
                                           float directional[3])
{
    float szz = 0.0f;
    float szd = 0.0f;
    float sdd = 0.0f;
    float cz[3] = {0.0f, 0.0f, 0.0f};
    float cd[3] = {0.0f, 0.0f, 0.0f};
    float determinant;
    int sampleIndex;
    int channel;

    for (sampleIndex = 0; sampleIndex < count; ++sampleIndex)
    {
        float zWeight = s_nativeLightmapBasis[sampleIndex][2] * 0.5f + 0.5f;
        float directionWeight = s_nativeLightmapBasis[sampleIndex][0] * direction[0]
                              + s_nativeLightmapBasis[sampleIndex][1] * direction[1]
                              + s_nativeLightmapBasis[sampleIndex][2] * direction[2];
        if (directionWeight < 0.0f)
            directionWeight = 0.0f;

        szz += zWeight * zWeight;
        szd += zWeight * directionWeight;
        sdd += directionWeight * directionWeight;
        for (channel = 0; channel < 3; ++channel)
        {
            cz[channel] += colors[sampleIndex * 3 + channel] * zWeight;
            cd[channel] += colors[sampleIndex * 3 + channel] * directionWeight;
        }
    }

    determinant = sdd * szz - szd * szd;
    if (determinant == 0.0f)
    {
        ambient[0] = ambient[1] = ambient[2] = 0.0f;
        directional[0] = directional[1] = directional[2] = 0.0f;
        return;
    }

    for (channel = 0; channel < 3; ++channel)
    {
        ambient[channel] = (sdd * cz[channel] - szd * cd[channel]) / determinant;
        directional[channel] = (szz * cd[channel] - szd * cz[channel]) / determinant;
    }

    /* Match native's constrained least-squares ordering: clamp the directional
     * term, refit ambient if needed, then clamp/refit once in the other order. */
    if (Lighting_ClampColor(directional) && szz > 0.0f)
    {
        for (channel = 0; channel < 3; ++channel)
            ambient[channel] = (cz[channel] - szd * directional[channel]) / szz;
    }
    if (Lighting_ClampColor(ambient) && sdd > 0.0f)
    {
        for (channel = 0; channel < 3; ++channel)
            directional[channel] = (cd[channel] - szd * ambient[channel]) / sdd;
        Lighting_ClampColor(directional);
    }
}

/* lighting.cpp 0x414850. */
static float Lighting_NativeFitError(const float *colors, int count,
                                     const float ambient[3],
                                     const float directional[3],
                                     const float direction[3])
{
    float error = 0.0f;
    int i;
    for (i = 0; i < count; ++i)
    {
        float zWeight = s_nativeLightmapBasis[i][2] * 0.5f + 0.5f;
        float directionWeight = s_nativeLightmapBasis[i][0] * direction[0]
                              + s_nativeLightmapBasis[i][1] * direction[1]
                              + s_nativeLightmapBasis[i][2] * direction[2];
        float dr, dg, db;
        if (directionWeight < 0.0f)
            directionWeight = 0.0f;
        dr = colors[i * 3 + 0]
           - (ambient[0] * zWeight + directional[0] * directionWeight);
        dg = colors[i * 3 + 1]
           - (ambient[1] * zWeight + directional[1] * directionWeight);
        db = colors[i * 3 + 2]
           - (ambient[2] * zWeight + directional[2] * directionWeight);
        error += dr * dr + dg * dg + db * db;
    }
    return error;
}

/* lighting.cpp 0x414CF0: gradient in the encoded tangent-direction plane. */
static void Lighting_NativeDirectionGradient(const float *colors, int count,
                                             const float ambient[3],
                                             const float directional[3],
                                             const float direction[3],
                                             float gradient[2])
{
    float sumX = 0.0f;
    float sumY = 0.0f;
    int i;
    for (i = 0; i < count; ++i)
    {
        const float *basis = s_nativeLightmapBasis[i];
        float zWeight = basis[2] * 0.5f + 0.5f;
        float dot = basis[0] * direction[0]
                  + basis[1] * direction[1]
                  + basis[2] * direction[2];
        float directionWeight = dot > 0.0f ? dot : 0.0f;
        float residual[3];
        float projectedResidual;

        residual[0] = colors[i * 3 + 0]
                    - (ambient[0] * zWeight + directional[0] * directionWeight);
        residual[1] = colors[i * 3 + 1]
                    - (ambient[1] * zWeight + directional[1] * directionWeight);
        residual[2] = colors[i * 3 + 2]
                    - (ambient[2] * zWeight + directional[2] * directionWeight);
        projectedResidual = directional[0] * residual[0]
                          + directional[1] * residual[1]
                          + directional[2] * residual[2];
        sumX += (basis[0] - dot * direction[0]) * projectedResidual;
        sumY += (basis[1] - dot * direction[1]) * projectedResidual;
    }

    gradient[0] = direction[2] * 2.0f * sumX;
    gradient[1] = direction[2] * 2.0f * sumY;
}

/* lighting.cpp 0x416830. */
static void Lighting_RefineNativeDirection(const float *colors, int count,
                                           float ambient[3],
                                           float directional[3],
                                           float direction[3])
{
    float error = Lighting_NativeFitError(colors, count, ambient, directional,
                                          direction);
    int encodedX;
    int encodedY;
    int step;

    if (error == 0.0f)
        return;

    encodedX = Lighting_EncodeCod4Byte(direction[0] / direction[2] * 0.25f + 0.5f);
    encodedY = Lighting_EncodeCod4Byte(direction[1] / direction[2] * 0.25f + 0.5f);
    step = 4;
    while (step)
    {
        float gradient[2];
        float maxGradient;

        Lighting_NativeDirectionGradient(colors, count, ambient, directional,
                                         direction, gradient);
        maxGradient = fabsf(gradient[0]);
        if (fabsf(gradient[1]) > maxGradient)
            maxGradient = fabsf(gradient[1]);
        if (maxGradient == 0.0f)
        {
            step /= 2;
            continue;
        }

        /* Native keeps the same gradient while reducing a rejected step and
         * truncates its signed delta toward zero. */
        for (;;)
        {
            float gradientScale = (float)step / maxGradient;
            int trialX = encodedX + (int)(gradient[0] * gradientScale
                                         + 9.313225746154785e-10f);
            int trialY = encodedY + (int)(gradient[1] * gradientScale
                                         + 9.313225746154785e-10f);
            float trialDirection[3];
            float trialAmbient[3];
            float trialDirectional[3];
            float trialError;
            float length;

            if (trialX < 0) trialX = 0;
            else if (trialX > 255) trialX = 255;
            if (trialY < 0) trialY = 0;
            else if (trialY > 255) trialY = 255;

            trialDirection[0] = (float)trialX * 0.01568627543747425f - 2.0f;
            trialDirection[1] = (float)trialY * 0.01568627543747425f - 2.0f;
            trialDirection[2] = 1.0f;
            length = sqrtf(trialDirection[0] * trialDirection[0]
                         + trialDirection[1] * trialDirection[1] + 1.0f);
            trialDirection[0] /= length;
            trialDirection[1] /= length;
            trialDirection[2] /= length;

            Lighting_FitNativeCoefficients(colors, count, trialDirection,
                                           trialAmbient, trialDirectional);
            trialError = Lighting_NativeFitError(colors, count, trialAmbient,
                                                 trialDirectional,
                                                 trialDirection);
            if (trialError < error)
            {
                ambient[0] = trialAmbient[0];
                ambient[1] = trialAmbient[1];
                ambient[2] = trialAmbient[2];
                directional[0] = trialDirectional[0];
                directional[1] = trialDirectional[1];
                directional[2] = trialDirectional[2];
                direction[0] = trialDirection[0];
                direction[1] = trialDirection[1];
                direction[2] = trialDirection[2];
                encodedX = trialX;
                encodedY = trialY;
                error = trialError;
                break;
            }

            step /= 2;
            if (!step)
                return;
        }
    }
}

/*
================
Lighting_GetGatheredLight

Build the outgoing radiosity color from accumulated incident light and the
sample's material reflectivity.  Native 0x414320 multiplies vars[8..10] by
vars[5..7]; the x64 layout keeps those meanings in gatheredIncident and
scattered respectively.
================
*/
void Lighting_GetGatheredLight(LightingSample_t *sample, float *outColor)
{
    SampleVars_t *vars;

    Assert("sample", ".\\lighting.cpp", 0xB2, 0, 1);
    Assert("sample->vars", ".\\lighting.cpp", 0xB3, 0, 1);

    vars = sample->vars;

    Assert("!IS_NAN((sample->vars->gathered)[0]) && !IS_NAN((sample->vars->gathered)[1]) && !IS_NAN((sample->vars->gathered)[2])",
           ".\\lighting.cpp", 0xB4, 0, 1);

    outColor[0] = vars->gatheredIncident[0] * vars->scattered[0];
    outColor[1] = vars->gatheredIncident[1] * vars->scattered[1];
    outColor[2] = vars->gatheredIncident[2] * vars->scattered[2];
}

/*
================
AdjustLightingContrast

Adjust contrast of lighting samples.
If baseIndex == -1, uses midpoint of min/max luminance as base.
Otherwise uses luminance of sample[baseIndex].
================
*/
void AdjustLightingContrast(int sampleCount, int baseIndex, float *srcSamples, float *dstColors)
{
    int i;
    float luminances[16];
    float minLum, maxLum;
    float baseLum;
    float contrast;
    float contrastPow;

    Assert("(sampleCount > 0 && sampleCount <= (sizeof(luminances) / sizeof(luminances[0])))",
           ".\\lighting.cpp", 0x12B, 0, 1);
    Assert("((baseIndex >= 0 && baseIndex < sampleCount) || baseIndex == -1)",
           ".\\lighting.cpp", 0x12C, 0, 1);
    Assert("srcSamples", ".\\lighting.cpp", 0x12D, 0, 1);
    Assert("dstColors", ".\\lighting.cpp", 0x12E, 0, 1);

    minLum = 3.402823e+38f;
    maxLum = -3.402823e+38f;

    for (i = 0; i < sampleCount; i++)
    {
        float r, g, b, lum;

        r = g_ambientR + srcSamples[i * 3 + 0];
        g = g_ambientG + srcSamples[i * 3 + 1];
        b = g_ambientB + srcSamples[i * 3 + 2];
        dstColors[i * 3 + 0] = r;
        dstColors[i * 3 + 1] = g;
        dstColors[i * 3 + 2] = b;

        lum = r * g_lumWeightR + g * g_lumWeightG + b * g_lumWeightB;
        luminances[i] = lum;

        if (lum < minLum) minLum = lum;
        if (lum > maxLum) maxLum = lum;
    }

    if (baseIndex == -1)
        baseLum = (maxLum + minLum) * 0.5f;
    else
        baseLum = luminances[baseIndex];

    contrast = maxLum - minLum;

    Assert("contrast >= 0.0f", ".\\lighting.cpp", 0x141, 0, 1);

    if (contrast == 0.0f || contrast >= 0.5f)
        goto done;

    contrastPow = powf(contrast * g_contrastScale, -0.0f - g_contrastGamma);

    for (i = 0; i < sampleCount; i++)
    {
        float lum = luminances[i];
        float adjusted = (lum - baseLum) * contrastPow + baseLum;

        if (adjusted <= 0.0f)
        {
            dstColors[i * 3 + 0] = 0.0f;
            dstColors[i * 3 + 1] = 0.0f;
            dstColors[i * 3 + 2] = 0.0f;
        }
        else
        {
            float scale = adjusted / lum;
            dstColors[i * 3 + 0] *= scale;
            dstColors[i * 3 + 1] *= scale;
            dstColors[i * 3 + 2] *= scale;
        }
    }

done:
    return;
}

/*
================
BuildFinalLightmap_PerPixel

Build final lightmap data for a single pixel.
Adjusts contrast for 4 light sources, gamma corrects and encodes
each RGB channel as a byte, writes to lightmap output.
================
*/
void BuildFinalLightmap_PerPixel(int lmapIndex, int col, int row)
{
    LightmapSample_t *sample;
    SampleVars_t *vars;
    float basisColors[MAX_NATIVE_LIGHTING_BASIS * 3];
    float ambient[3];
    float directional[3];
    float dominantDir[3];
    float gammaCorrectedVars[4];
    int basisCount;
    int i, j;
    unsigned char *dst;

    sample = (LightmapSample_t *)g_lightingSamples
           + ((long long)lmapIndex * 512 + row) * 512 + col;
    vars = sample->vars;

    if (vars && vars->validMask)
        basisCount = Lighting_BuildFinalDirectionalField(
            lmapIndex, col, row, vars, basisColors);
    else
        basisCount = Lighting_ReconstructDirectionalField(
            lmapIndex, col, row, basisColors);

    Lighting_DeriveNativeDirection(basisColors, basisCount, dominantDir);
    Lighting_FitNativeCoefficients(basisColors, basisCount, dominantDir,
                                   ambient, directional);
    Lighting_RefineNativeDirection(basisColors, basisCount, ambient,
                                   directional, dominantDir);

    /* Native forwards primary-light visibility directly to the byte encoder;
     * it is a scalar coverage value, not a gamma-corrected color channel. */
    Lighting_BuildFinalScalarSamples(lmapIndex, col, row,
                                     gammaCorrectedVars);

    /* CoD4's secondary image is two vertically stacked 512x512 RGBA maps.
     * Disk bytes are BGR, and their alpha pair encodes x/z and y/z. */
    {
        int baseOffset = lmapIndex * 1536 + row;
        int pixelAddr = (baseOffset * 512 + col) * 4;
        unsigned char directionX = 0;
        unsigned char directionY = 0;
        dst = g_lightmapOutput + pixelAddr;

        if (ambient[0] != 0.0f || ambient[1] != 0.0f || ambient[2] != 0.0f
         || directional[0] != 0.0f || directional[1] != 0.0f || directional[2] != 0.0f)
        {
            directionX = Lighting_EncodeCod4Byte(dominantDir[0] / dominantDir[2] * 0.25f + 0.5f);
            directionY = Lighting_EncodeCod4Byte(dominantDir[1] / dominantDir[2] * 0.25f + 0.5f);
        }

        dst[0] = Lighting_EncodeCod4Byte(ambient[2]);
        dst[1] = Lighting_EncodeCod4Byte(ambient[1]);
        dst[2] = Lighting_EncodeCod4Byte(ambient[0]);
        dst[3] = directionX;

        dst += 0x100000;
        dst[0] = Lighting_EncodeCod4Byte(directional[2]);
        dst[1] = Lighting_EncodeCod4Byte(directional[1]);
        dst[2] = Lighting_EncodeCod4Byte(directional[0]);
        dst[3] = directionY;
    }

    /* The primary L8 image is a 2x2 scalar sample for each lightmap texel. */
    {
        int gridBase = lmapIndex * 1536 + row;
        int gridAddr = 0x200000 + (gridBase * 1024 + col) * 2;
        dst = g_lightmapOutput + gridAddr;

        for (i = 0; i < 2; i++)
        {
            for (j = 0; j < 2; j++)
            {
                *dst = Lighting_EncodeCod4Byte(gammaCorrectedVars[i * 2 + j]);
                dst++;
            }
            dst += 0x3FE;
        }
    }
}

/*
================
EncodeFloatInByte

Clamp float to [0,1], encode as byte [0..255].
================
*/
unsigned char EncodeFloatInByte(float value)
{
    if (value <= 0.0f)
        return 0;
    if (value >= 1.0f)
        return 255;

    return (unsigned char)(int)floorf(value * 255.0f + 0.5f);
}

/*
================
Lighting_AllocSampleVars

Assign vars from pool to a lighting sample.
================
*/
void Lighting_AllocSampleVars(LightingSample_t *sample)
{
    Assert("sample->vars == NULL", ".\\lighting.cpp", 0x93, 0, 1);

    sample->vars = (float *)((char *)g_sampleVarsPool
                   + (long long)g_usefulSampleCount * SAMPLE_VARS_SIZE);
    g_usefulSampleCount++;
}

/*
================
Lighting_IncrementUsefulSampleCount

Increment the useful sample counter.
================
*/
void Lighting_IncrementUsefulSampleCount(void)
{
    g_usefulSampleCount++;
}

/*
================
Lighting_SampleCallback_Trampoline1

Trampoline for per-sample callback.
================
*/
typedef void (*SampleCallbackFn)(void *sample);

void Lighting_SampleCallback_Trampoline1(int sampleIndex)
{
    LightmapSample_t *sample;

    sample = (LightmapSample_t *)g_lightingSamples + sampleIndex;
    if (sample->weight != 0.0f)
    {
        ((SampleCallbackFn)g_lightingSampleCallback)(sample);
    }
}

/* Native 0x414260 scales the direct radiosity vector by
 * min(radiosityScale, 0.99 / length).  The local representation keeps that
 * vector in `scattered`; preserve the zero-vector case exactly. */
void Lighting_ApplyRadiosityScale(LightingSample_t *sample)
{
    SampleVars_t *vars;
    float length;
    float scale;

    if (!sample || !(vars = sample->vars))
        return;

    length = sqrtf(vars->scattered[0] * vars->scattered[0]
                 + vars->scattered[1] * vars->scattered[1]
                 + vars->scattered[2] * vars->scattered[2]);
    if (length == 0.0f)
        return;

    scale = 0.9900000095367432f / length;
    if (scale > g_radiosityScale)
        scale = g_radiosityScale;

    vars->scattered[0] *= scale;
    vars->scattered[1] *= scale;
    vars->scattered[2] *= scale;
}

/* Variant used by radiosity bounce passes, whose callback needs the worker
 * index for its per-thread convergence accumulator. */
static void Lighting_SampleThreadCallback_Trampoline(int sampleIndex, int threadIndex)
{
    LightmapSample_t *sample;
    typedef void (*ThreadSampleCallbackFn)(void *sample, int threadIndex);

    sample = (LightmapSample_t *)g_lightingSamples + sampleIndex;
    if (sample->weight != 0.0f)
        ((ThreadSampleCallbackFn)g_lightingThreadSampleCallback)(sample, threadIndex);
}

/*
================
Lighting_PixelCallback_Trampoline

Trampoline for per-pixel callback.
================
*/
typedef void (*PixelCallbackFn)(void *pixel, int index, int a2);

void Lighting_PixelCallback_Trampoline(int pixelIndex, int a2)
{
    void *pixel;

    pixel = (char *)g_lightingSamples + ((long long)pixelIndex << 23);
    ((PixelCallbackFn)g_lightingPixelCallback)(pixel, pixelIndex, a2);
}

/*
================
ForEachUsefulLightingSample

Iterate all useful lighting samples.
================
*/
void ForEachUsefulLightingSample(void *callback, int a2)
{
    g_lightingSampleCallback = callback;
    ForEachLightmapPixel(g_lightmapSize << 18, (void *)Lighting_SampleCallback_Trampoline1, a2);
}

void ForEachUsefulLightingSampleThreaded(void *callback, int threadCount)
{
    g_lightingThreadSampleCallback = callback;
    ForEachLightmapPixel(g_lightmapSize << 18,
                         (void *)Lighting_SampleThreadCallback_Trampoline,
                         threadCount);
}

/*
================
Lighting_ForEachPixel_Helper

Iterate all lightmap pixels.
================
*/
void Lighting_ForEachPixel_Helper(void *callback, int a2)
{
    g_lightingPixelCallback = callback;
    ForEachLightmapPixel(g_lightmapSize, (void *)Lighting_PixelCallback_Trampoline, a2);
}

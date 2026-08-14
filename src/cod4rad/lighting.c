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
void          *g_sunDirGlobals;

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

#define SAMPLE_VARS_SIZE 96

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
    allocSize = g_usefulSampleCount * 3 * 32;
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
    allocSize = antiBleedCount * 3 * 32;

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
    Lighting_InitAntiBleed();
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
}

/* The CoD2 solver retained by this reconstruction accumulates four fixed
 * upper-hemisphere basis samples.  CoD4's on-disk lightmap stores a constrained
 * two-term fit of those samples instead: ambient * (dir.z * .5 + .5) plus a
 * directional term * max(dot(dir, dominantDir), 0).  These directions are the
 * four basis vectors used by GatherSurfaceIncidentEnergyForLightFromDir. */
static const float s_cod2LightmapBasis[4][3] = {
    { 0.0f,          0.0f,          1.0f },
    { 0.0f,          0.8164965510f, 0.5773502588f },
    {-0.7071067691f,-0.4082482755f, 0.5773502588f },
    { 0.7071067691f,-0.4082482755f, 0.5773502588f }
};

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
    return (unsigned char)(int)(value * 255.0f);
}

static void Lighting_FitCod4Coefficients(const float colors[4][3],
                                         float ambient[3],
                                         float directional[3],
                                         float dominantDir[3])
{
    float sampleIntensity[4];
    float zWeight[4];
    float weightedIntensity = 0.0f;
    float weightSum = 0.0f;
    float dirLength;
    float szz = 0.0f, szd = 0.0f, sdd = 0.0f;
    float cz[3] = {0.0f, 0.0f, 0.0f};
    float cd[3] = {0.0f, 0.0f, 0.0f};
    float determinant;
    int sampleIndex, channel;

    dominantDir[0] = 0.0f;
    dominantDir[1] = 0.0f;
    dominantDir[2] = 0.0f;

    for (sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
    {
        sampleIntensity[sampleIndex] = colors[sampleIndex][0]
                                     + colors[sampleIndex][1]
                                     + colors[sampleIndex][2];
        zWeight[sampleIndex] = s_cod2LightmapBasis[sampleIndex][2] * 0.5f + 0.5f;
        weightedIntensity += sampleIntensity[sampleIndex] * zWeight[sampleIndex];
        weightSum += zWeight[sampleIndex];
    }

    if (weightSum > 0.0f)
    {
        float baseIntensity = weightedIntensity / weightSum;
        for (sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
        {
            float residual = sampleIntensity[sampleIndex]
                           - zWeight[sampleIndex] * baseIntensity;
            dominantDir[0] += s_cod2LightmapBasis[sampleIndex][0] * residual;
            dominantDir[1] += s_cod2LightmapBasis[sampleIndex][1] * residual;
            dominantDir[2] += s_cod2LightmapBasis[sampleIndex][2] * residual;
        }
    }

    dirLength = sqrtf(dominantDir[0] * dominantDir[0]
                    + dominantDir[1] * dominantDir[1]
                    + dominantDir[2] * dominantDir[2]);
    if (dirLength > 1.0e-6f && dominantDir[2] > 1.0e-6f)
    {
        float invLength = 1.0f / dirLength;
        dominantDir[0] *= invLength;
        dominantDir[1] *= invLength;
        dominantDir[2] *= invLength;
    }
    else
    {
        dominantDir[0] = 0.0f;
        dominantDir[1] = 0.0f;
        dominantDir[2] = 1.0f;
    }

    for (sampleIndex = 0; sampleIndex < 4; ++sampleIndex)
    {
        float directionWeight = s_cod2LightmapBasis[sampleIndex][0] * dominantDir[0]
                              + s_cod2LightmapBasis[sampleIndex][1] * dominantDir[1]
                              + s_cod2LightmapBasis[sampleIndex][2] * dominantDir[2];
        if (directionWeight < 0.0f)
            directionWeight = 0.0f;

        szz += zWeight[sampleIndex] * zWeight[sampleIndex];
        szd += zWeight[sampleIndex] * directionWeight;
        sdd += directionWeight * directionWeight;
        for (channel = 0; channel < 3; ++channel)
        {
            cz[channel] += colors[sampleIndex][channel] * zWeight[sampleIndex];
            cd[channel] += colors[sampleIndex][channel] * directionWeight;
        }
    }

    determinant = sdd * szz - szd * szd;
    if (fabsf(determinant) <= 1.0e-8f)
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

/*
================
Lighting_GetGatheredLight

Read gathered light RGB from a sample's vars.
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

    outColor[0] = vars->gathered[0];
    outColor[1] = vars->gathered[1];
    outColor[2] = vars->gathered[2];
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
    float adjustedColors[4 * 3];
    float gammaColors[4][3];
    float ambient[3];
    float directional[3];
    float dominantDir[3];
    float gammaCorrectedVars[4];
    int i, j;
    unsigned char *dst;

    sample = (LightmapSample_t *)g_lightingSamples
           + ((long long)lmapIndex * 512 + row) * 512 + col;

    if (sample->weight <= 0.0f)
    {
        gammaCorrectedVars[0] = 0.0f;
        gammaCorrectedVars[1] = 0.0f;
        gammaCorrectedVars[2] = 0.0f;
        gammaCorrectedVars[3] = 0.0f;
        ambient[0] = ambient[1] = ambient[2] = 0.0f;
        directional[0] = directional[1] = directional[2] = 0.0f;
        dominantDir[0] = dominantDir[1] = 0.0f;
        dominantDir[2] = 1.0f;
        goto write_output;
    }

    vars = sample->vars;
    Assert("sample->vars", ".\\lighting.cpp", 0x16A, 0, 1);

    AdjustLightingContrast(4, 0, vars->incident, adjustedColors);

    if (g_aoEnabled && g_aoFactors)
    {
        long long pixelIdx = ((long long)lmapIndex * 512 + row) * 512 + col;
        float ao = g_aoFactors[pixelIdx];
        for (i = 0; i < 12; i++)
            adjustedColors[i] *= ao;
    }

    /* Native CoD4 performs the directional fit in gamma-corrected space. */
    for (i = 0; i < 4; ++i)
    {
        for (j = 0; j < 3; ++j)
        {
            float value = adjustedColors[i * 3 + j];
            if (value < 0.0f)
                value = 0.0f;
            gammaColors[i][j] = GammaCorrectColorChannel(value);
        }
    }
    Lighting_FitCod4Coefficients(gammaColors, ambient, directional, dominantDir);

    /* gamma correct intensity values */
    {
        float *srcPtr = vars->intensity;
        float *dstPtr = gammaCorrectedVars;
        for (i = 0; i < 2; i++)
        {
            for (j = 0; j < 2; j++)
            {
                float val = *srcPtr++;
                Assert("(color >= 0)", ".\\lighting.cpp", 0xCF, 0, 1);
                *dstPtr++ = powf(val, 1.0f / g_degamma);
            }
        }
    }

write_output:
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

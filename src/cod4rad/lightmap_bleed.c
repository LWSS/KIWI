/*
 * lightmap_bleed.c — Lightmap bleeding/edge extension for texture filtering.
 */

#include "cod2rad64.h"

/* external functions not in master header */
extern float GammaCorrectColorChannel(float val);    /* lighting_4128C0 */
extern float DegammaColorChannel(float val);      /* lighting_412690 */
extern void memcpy_fast(void *dst, const void *src, int size);
#include <stdlib.h>
extern void memset_fast(void *dst, int val, int size);
extern void ForEachLightmapPixelInPoly(void *callback, int param, int threadCount);
extern void Lighting_ForEachPixel_Helper(void *callback, int threadCount);
extern void Lmap_ApplyBleeding(void *lmapBase, int lmapIdx); /* lightmap_bleed_4145C0 */

void          *g_primaryBleedData;
void          *g_secondaryBleedData;

/* forward decls for bleed callback tables defined near Lmap_ApplyBleeding */
extern void *g_bleedFuncTablePrimary[4];
extern void *g_bleedFuncTableSecondary[4];

/* 3x3 gaussian kernel weights (sum = 1.0) */
float g_bleedNeighborWeight[9] = {
    0.015625f,  0.09375f,  0.015625f,
    0.09375f,   0.5625f,   0.09375f,
    0.015625f,  0.09375f,  0.015625f,
};

/* 3x3 direction-bit mask (center=0, 8 cardinal/diagonal bits) */
unsigned char g_bleedNeighborMask[9] = {
    0x08, 0x10, 0x20,
    0x04, 0x00, 0x40,
    0x02, 0x01, 0x80,
};

extern void Lmap_FindBleedingForSample(void *data, int lmapIdx, int width, int height,
                                       float u, float v); /* lightmap_bleed_413720 */

/*
================
Bleed_GetSampleAreaX2_Secondary

Returns the areaX2 field from a secondary lightmap sample.
Index: (t * 512 + s) * 32 + 8.
================
*/
float Bleed_GetSampleAreaX2_Secondary(void *data, int s, int t)
{
    LmapSample_t *sample = &((LmapSample_t *)data)[t * LMAP_STRIDE + s];
    return sample->areaX2;
}

/*
================
Bleed_GetSampleAreaX2_Primary

Returns one sub-sample from a primary lightmap sample.
Splits s/t into base (s/2, t/2) and sub (s&1, t&1) components,
then reads data[baseIdx].subSample[subIdx].
================
*/
float Bleed_GetSampleAreaX2_Primary(void *data, int s, int t)
{
    int baseIdx = (s / 2) + (t / 2) * LMAP_STRIDE;
    int subIdx  = (s & 1) + (t & 1) * 2;
    LmapSample_t *sample = &((LmapSample_t *)data)[baseIdx];
    return sample->subSample[subIdx];
}

/*
================
Bleed_CopySampleX2_Primary

Copies one sub-sample from a primary lightmap source to dest.
Computes base sample from s/2, t/2 and sub-index from s&1, t&1.
Copies both the sample field at +0x0C[subIdx] and the
pointed-to color data at ptr[subIdx].
================
*/
void Bleed_CopySampleX2_Primary(void *srcData, int s, int t, void *dstData)
{
    int sBase, tBase, sSub, tSub;
    int sampleIdx, subIdx;
    LmapSample_t *srcSample, *dstSample;
    int **srcPtr, **dstPtr;

    sSub = s & 1;
    tSub = t & 1;
    sBase = s / 2;
    tBase = t / 2;

    sampleIdx = sBase + tBase * LMAP_STRIDE;
    srcSample = (LmapSample_t *)srcData + sampleIdx;
    dstSample = (LmapSample_t *)dstData + sampleIdx;

    subIdx = sSub + tSub * 2;

    /* copy sub-sample flag/value at +0x0C */
    *(int *)&dstSample->subSample[subIdx] = *(int *)&srcSample->subSample[subIdx];

    /* copy color data through pointer at +0x00 */
    ((int *)dstSample->colorData)[subIdx] = ((int *)srcSample->colorData)[subIdx];
}

/*
================
Bleed_ScaleSampleX2_Secondary

Scales all data in a secondary lightmap sample by a factor.
Scales areaX2, bulk color data (12 floats), and conditionally
scales per-sub-sample color data where flags are zero.
================
*/
void Bleed_ScaleSampleX2_Secondary(float scale, void *data, int s, int t)
{
    int sampleIdx;
    LmapSample_t *sample;
    float *ptr;
    int i;

    sampleIdx = t * LMAP_STRIDE + s;
    sample = (LmapSample_t *)data + sampleIdx;

    /* scale area */
    sample->areaX2 *= scale;

    /* scale 12 bulk floats at ptr[0x10..0x3C] */
    ptr = sample->colorData;
    for (i = 0x10; i <= 0x3C; i += 4)
        ptr[i / 4] *= scale;

    /* scale per-sub-sample color where flag is zero */
    for (i = 0; i < 4; i++)
    {
        if (sample->subSample[i] == 0.0f)
            ptr[i] *= scale;
    }
}

/*
================
Bleed_ScaleSampleX2_Primary

Scales one sub-sample in a primary lightmap sample by a factor.
Computes base sample from s/2, t/2 and sub-index from s&1, t&1.
Scales both the field at sample+0x0C[subIdx] and ptr[subIdx].
================
*/
void Bleed_ScaleSampleX2_Primary(float scale, void *data, int s, int t)
{
    int sBase, tBase, sSub, tSub;
    int sampleIdx, subIdx;
    LmapSample_t *sample;
    float *ptr;

    sSub = s & 1;
    tSub = t & 1;
    sBase = s / 2;
    tBase = t / 2;

    sampleIdx = sBase + tBase * LMAP_STRIDE;
    sample = (LmapSample_t *)data + sampleIdx;
    subIdx = sSub + tSub * 2;

    /* scale sub-sample field at +0x0C */
    sample->subSample[subIdx] *= scale;

    /* scale color data through pointer */
    ptr = sample->colorData;
    ptr[subIdx] *= scale;
}

/*
================
Bleed_AddWeightedSample_Secondary

Adds a weighted source secondary sample to a destination secondary sample.
Accumulates areaX2, bulk color data (12 floats), and per-sub-sample
color data with clamping and normalization.
================
*/
void Bleed_AddWeightedSample_Secondary(void *srcData, int srcS, int srcT,
                                       float weight, void *dstData, int dstS, int dstT)
{
    int srcIdx, dstIdx;
    LmapSample_t *srcSample, *dstSample;
    float *srcPtr, *dstPtr;
    float totalWeight, totalClamped;
    float scaledWeight;
    int i, j;

    srcIdx = srcT * LMAP_STRIDE + srcS;
    dstIdx = dstT * LMAP_STRIDE + dstS;
    srcSample = (LmapSample_t *)srcData + srcIdx;
    dstSample = (LmapSample_t *)dstData + dstIdx;

    /* accumulate weighted areaX2 */
    dstSample->areaX2 += weight * srcSample->areaX2;

    /* accumulate 12 bulk floats through pointer */
    srcPtr = srcSample->colorData;
    dstPtr = dstSample->colorData;
    for (i = 4; i < 16; i++)
        dstPtr[i] += weight * srcPtr[i];

    /* accumulate per-sub-sample with clamping */
    totalWeight = 0.0f;
    totalClamped = 0.0f;
    for (i = 0; i < 4; i++)
    {
        float flag = srcSample->subSample[i];
        if (flag > 0.0f)
        {
            float val = GammaCorrectColorChannel(srcPtr[i]);
            if (val < 0.0f) val = 0.0f;
            else if (val > 1.0f) val = 1.0f;
            totalClamped += val;
            totalWeight += 1.0f;
        }
    }

    /* normalize and apply to dest sub-samples */
    if (totalWeight != 0.0f)
    {
        scaledWeight = DegammaColorChannel(totalClamped / totalWeight) * weight;
        for (i = 0; i < 4; i++)
        {
            if (dstSample->subSample[i] == 0.0f)
                dstPtr[i] += scaledWeight;
        }
    }
}

/*
================
Bleed_AddWeightedSubSample_Primary

Adds a weighted sub-sample from a source primary sample to a
destination primary sample. Different source and dest coordinates.
================
*/
void Bleed_AddWeightedSubSample_Primary(void *srcData, int srcS, int srcT,
                                        float weight, void *dstData, int dstS, int dstT)
{
    int srcBase, dstBase, srcSubIdx, dstSubIdx;
    LmapSample_t *srcSample, *dstSample;
    float *srcPtr, *dstPtr;

    srcBase = srcS / 2 + (srcT / 2) * LMAP_STRIDE;
    srcSubIdx = (srcS & 1) + (srcT & 1) * 2;

    dstBase = dstS / 2 + (dstT / 2) * LMAP_STRIDE;
    dstSubIdx = (dstS & 1) + (dstT & 1) * 2;

    srcSample = (LmapSample_t *)srcData + srcBase;
    dstSample = (LmapSample_t *)dstData + dstBase;

    /* accumulate weighted sub-sample field */
    dstSample->subSample[dstSubIdx] +=
        weight * srcSample->subSample[srcSubIdx];

    /* accumulate weighted color through pointers */
    srcPtr = srcSample->colorData;
    dstPtr = dstSample->colorData;
    dstPtr[dstSubIdx] += weight * srcPtr[srcSubIdx];
}

/*
================
Lmap_FindBleeding_Callback

Per-lightmap-pixel callback that calls the bleed finder for
both primary (512) and secondary (1024, coords scaled by 2x) data.
================
*/
void Lmap_FindBleeding_Callback(float unused, float *samplePos,
                                int unused1, int unused2, int *lmapIdxPtr)
{
    int lmapIdx = *lmapIdxPtr;

    /* primary: 512x512, original coords */
    Lmap_FindBleedingForSample(g_primaryBleedData, lmapIdx, 512, 512,
                               samplePos[0], samplePos[1]);

    /* secondary: 1024x1024, coords scaled by 2 */
    Lmap_FindBleedingForSample(g_secondaryBleedData, lmapIdx, 1024, 1024,
                               samplePos[0] * 2.0f, samplePos[1] * 2.0f);
}

/*
================
Bleed_CopySampleX2_Secondary

Copies a secondary lightmap sample from source to dest.
Copies areaX2 directly, copies 48 bytes of bulk color data,
and normalizes per-sub-sample data.
================
*/
void Bleed_CopySampleX2_Secondary(void *srcData, int s, int t, void *dstData)
{
    int sampleIdx;
    LmapSample_t *srcSample, *dstSample;
    float *srcPtr, *dstPtr;
    float totalClamped, totalWeight, normalized;
    int i;

    sampleIdx = t * LMAP_STRIDE + s;
    srcSample = (LmapSample_t *)srcData + sampleIdx;
    dstSample = (LmapSample_t *)dstData + sampleIdx;

    /* copy areaX2 */
    *(int *)&dstSample->areaX2 = *(int *)&srcSample->areaX2;

    /* copy 48 bytes of bulk color data (ptr[4..15]) */
    srcPtr = srcSample->colorData;
    dstPtr = dstSample->colorData;
    memcpy_fast(dstPtr + 4, srcPtr + 4, 0x30);

    /* compute normalized sub-sample value */
    totalClamped = 0.0f;
    totalWeight = 0.0f;
    for (i = 0; i < 4; i++)
    {
        float flag = srcSample->subSample[i];
        if (flag > 0.0f)
        {
            float val = GammaCorrectColorChannel(srcPtr[i]);
            if (val < 0.0f) val = 0.0f;
            else if (val > 1.0f) val = 1.0f;
            totalClamped += val;
            totalWeight += 1.0f;
        }
    }

    /* write normalized value to dest sub-samples where flag is zero */
    if (totalWeight != 0.0f)
    {
        normalized = DegammaColorChannel(totalClamped / totalWeight);
        for (i = 0; i < 4; i++)
        {
            if (dstSample->subSample[i] == 0.0f)
                dstPtr[i] = normalized;
        }
    }
}

/*
================
Lmap_InitBilinearBleeding

Allocates and builds the two native lightmap-adjacency masks.  Retail keeps
these masks through final-lightmap construction; lighting.cpp uses them to
reconstruct invalid coefficient texels and primary-light subsamples.
================
*/
void Lmap_InitBilinearBleeding(int lmapCount, int threadCount)
{
    int allocSize;
    void *data;

    allocSize = lmapCount * 5 * (1 << 18);
    data = malloc(allocSize);
    g_primaryBleedData = data;
    if (!data)
        ErrorMsg("Out of memory trying to allocate lightmap bleed info (%i bytes)\n", allocSize);

    memset_fast(g_primaryBleedData, 0, allocSize);

    g_secondaryBleedData = (char *)g_primaryBleedData + lmapCount * (1 << 18);

    /* find bleeding for all lightmap pixels */
    ForEachLightmapPixelInPoly(Lmap_FindBleeding_Callback, 2, threadCount);

    /* Native cod4rad does not run the older Cod2 iterative sample-field
     * mutator here.  The masks remain live for lighting.cpp 0x415990,
     * 0x415B80 and 0x415D90 during final image construction. */
}

/*
================
Lmap_IterativeBleed

Single ping-pong bleed pass. Writes to pingPong[1-startIter] from
pingPong[startIter] using bleedData + the 3x3 g_bleedNeighborMask /
g_bleedNeighborWeight tables. Returns (1 - startIter).

Function table layout:
  [0]: GetSampleAreaX2(data, s, t) -> float
  [1]: CopySampleX2(srcData, s, t, dstData)
  [2]: ScaleSampleX2(scale, data, s, t)
  [3]: AddWeightedSample(srcData, srcS, weight, srcT, dstData, dstS, dstT)
================
*/
typedef float (*GetAreaFunc)(void *, int, int);
typedef void (*CopyFunc)(void *, int, int, void *);
typedef void (*ScaleFunc)(float, void *, int, int);
typedef void (*AddWeightedFunc)(void *, int, int, float, void *, int, int);

static char s_assertDisable_IterBleed_area;

static int Lmap_IterativeBleed(void **funcTable, void **pingPong,
                               unsigned char *bleedData, int width, int height, int startIter)
{
    GetAreaFunc getArea = (GetAreaFunc)funcTable[0];
    CopyFunc copySample = (CopyFunc)funcTable[1];
    ScaleFunc scaleSample = (ScaleFunc)funcTable[2];
    AddWeightedFunc addWeighted = (AddWeightedFunc)funcTable[3];

    int src = 1 - startIter;
    int iter;
    int maxRC = height - 1;
    int maxCol = width - 1;

    for (iter = 0; iter < height; iter++)
    {
        int rowStart = (iter + 1 >= 2) ? iter - 1 : 0;
        int rowEnd = (iter + 1 < maxRC) ? iter + 1 : maxRC;
        int row, col;

        for (row = 0; row < width; row++)
        {
            int colStart = (row + 1 >= 2) ? row - 1 : 0;
            int colEnd = (row + 1 < maxCol) ? row + 1 : maxCol;

            /* check dest area should be 0 */
            {
                float dstArea = getArea(pingPong[1 - startIter], row, iter);
                Assert(dstArea == 0.0f, s_assertDisable_IterBleed_area);
            }

            /* check src area */
            {
                float srcArea = getArea(pingPong[startIter], row, iter);
                if (srcArea > 0.0f)
                {
                    /* pixel already filled — copy to dest */
                    copySample(pingPong[startIter], row, iter, pingPong[1 - startIter]);
                    goto next_col;
                }
            }

            /* empty pixel — scan neighbors and accumulate weighted contributions */
            {
                float totalWeight = 0.0f;
                int nr, nc;

                for (nr = rowStart; nr <= rowEnd; nr++)
                {
                    for (nc = colStart; nc <= colEnd; nc++)
                    {
                        float neighborArea = getArea(pingPong[startIter], nc, nr);
                        if (neighborArea == 0.0f)
                            continue;

                        /* check bleed direction with lookup table — center-relative 3x3 */
                        {
                            int bleedIdx = nr * width + nc;
                            unsigned char bleedByte = bleedData[bleedIdx];
                            int neighborIdx = (nr - iter + 1) * 3 + (nc - row + 1);

                            if (!(g_bleedNeighborMask[neighborIdx] & bleedByte))
                                continue;

                            addWeighted(pingPong[startIter], nc, nr,
                                       g_bleedNeighborWeight[neighborIdx],
                                       pingPong[1 - startIter], row, iter);
                            totalWeight += g_bleedNeighborWeight[neighborIdx];
                        }
                    }
                }

                if (totalWeight > 0.0f && totalWeight != 1.0f)
                {
                    scaleSample(1.0f / totalWeight, pingPong[1 - startIter], row, iter);
                }
            }

        next_col:;
        }
    }

    return src;
}

/*
================
Lmap_ApplyBleeding

Top-level bleed application. Allocates ping-pong buffers,
links sample pointers, runs iterative bleed for both primary
and secondary data, copies results back with sub-sample filling.
================
*/
/* off_4591E8 — primary-to-secondary bleed callbacks */
void *g_bleedFuncTablePrimary[4] = {
    (void *)Bleed_GetSampleAreaX2_Secondary,
    (void *)Bleed_CopySampleX2_Secondary,
    (void *)Bleed_ScaleSampleX2_Secondary,
    (void *)Bleed_AddWeightedSample_Secondary,
};

/* off_459208 — secondary-to-primary bleed callbacks */
void *g_bleedFuncTableSecondary[4] = {
    (void *)Bleed_GetSampleAreaX2_Primary,
    (void *)Bleed_CopySampleX2_Primary,
    (void *)Bleed_ScaleSampleX2_Primary,
    (void *)Bleed_AddWeightedSubSample_Primary,
};

static char s_assertDisable_ApplyBleed_match;
static char s_assertDisable_ApplyBleed_ping;

void Lmap_ApplyBleeding(void *lmapData, int lmapIdx)
{
    void *secondaryBuf;
    void *primaryBuf;
    char *p;
    int i;
    int iterSecondary, iterPrimary;
    void *pingPongState[4]; /* state for ping-pong iteration */
    void *srcBuffer;

    /* allocate secondary ping-pong buffer: 0x1800000 bytes */
    secondaryBuf = malloc(0x1800000);
    if (!secondaryBuf)
        Com_Printf("Couldn't allocate ping pong buffers to bleed lightmaps\n", 0x1800000);
    memset_fast(secondaryBuf, 0, 0x1800000);

    /* allocate primary ping-pong buffer: 0x800000 bytes */
    primaryBuf = malloc(0x800000);
    if (!primaryBuf)
        Com_Printf("Couldn't allocate ping pong buffers to bleed lightmaps\n", 0x800000);
    memset_fast(primaryBuf, 0, 0x800000);

    /* link each primary sample's pointer to secondary buffer */
    {
        LmapSample_t *samples = (LmapSample_t *)primaryBuf;
        for (i = 0; i < 0x40000; i++)
            samples[i].colorData = (float *)((char *)secondaryBuf + i * 0x60);
    }

    /* run iterative bleed for primary (512x512) */
    pingPongState[0] = primaryBuf;
    pingPongState[1] = lmapData;
    iterPrimary = Lmap_IterativeBleed(g_bleedFuncTablePrimary, pingPongState,
                                       (unsigned char *)g_primaryBleedData + lmapIdx * (1 << 18),
                                       LMAP_STRIDE,
                                       LMAP_STRIDE, 1);

    /* run iterative bleed for secondary (1024x1024) */
    iterSecondary = Lmap_IterativeBleed(g_bleedFuncTableSecondary, pingPongState,
                                         (unsigned char *)g_secondaryBleedData + lmapIdx * (1 << 20),
                                         1024, 1024, 1);

    Assert(iterPrimary == iterSecondary, s_assertDisable_ApplyBleed_match);

    /* copy results back from ping-pong buffer */
    srcBuffer = pingPongState[iterPrimary];
    if (srcBuffer != lmapData)
        Assert(srcBuffer == primaryBuf, s_assertDisable_ApplyBleed_ping);

    if (srcBuffer != lmapData)
    {
        /* apply bleeding with sub-sample filling */
        for (i = 0; i < LMAP_STRIDE; i++)
        {
            int j;
            for (j = 0; j < LMAP_STRIDE; j++)
            {
                int s = j * 2;
                int t = i * 2;

                Bleed_CopySampleX2_Secondary(srcBuffer, j, i, lmapData);
                Bleed_CopySampleX2_Primary(srcBuffer, s, t, lmapData);
                Bleed_CopySampleX2_Primary(srcBuffer, s + 1, t, lmapData);
                Bleed_CopySampleX2_Primary(srcBuffer, s, t + 1, lmapData);
                Bleed_CopySampleX2_Primary(srcBuffer, s + 1, t + 1, lmapData);
            }
        }
    }

    Bleed_FillEmptySubSamples(lmapData);

    free(primaryBuf);
    free(secondaryBuf);
}

/*
================
Lmap_FindBleedingForSample

Finds which bleeding direction a lightmap sample needs based on
its sub-pixel position. Looks up a direction byte from a table
and ORs it into the bleed data array.
================
*/
/* 2x2 direction byte lookup, indexed by (sHalf + tHalf*2) */
unsigned char g_bleedDirTable[4] = { 0xC1, 0x07, 0x70, 0x1C };
static char s_assertDisable_FindBleed_s;
static char s_assertDisable_FindBleed_t;

void Lmap_FindBleedingForSample(void *data, int lmapIdx, int width, int height,
                                float u, float v)
{
    int s, t;
    float sFrac, tFrac;
    int sHalf, tHalf;
    int tableIdx;
    int sampleIdx;

    s = (int)floorf(u);
    t = (int)floorf(v);

    Assert(s >= 0 && s < width, s_assertDisable_FindBleed_s);
    Assert(t >= 0 && t < width, s_assertDisable_FindBleed_t);

    sFrac = u - (float)s;
    tFrac = v - (float)t;

    sHalf = (sFrac >= 0.5f) ? 1 : 0;
    tHalf = (tFrac >= 0.5f) ? 1 : 0;

    tableIdx = sHalf + tHalf * 2;

    sampleIdx = ((lmapIdx * height + t) * width) + s;
    ((unsigned char *)data)[sampleIdx] |= g_bleedDirTable[tableIdx];
}

/*
================
Bleed_FillEmptySubSamples

Iterates over all 512x512 secondary samples and fills empty
sub-samples with the normalized average of filled sub-samples.
================
*/
void Bleed_FillEmptySubSamples(void *data)
{
    LmapSample_t *sample;
    float *ptr;
    float totalClamped, totalWeight, normalized;
    int row, col, i;

    sample = (char *)data;
    for (row = 0; row < LMAP_STRIDE; row++)
    {
        for (col = 0; col < LMAP_STRIDE; col++)
        {
            ptr = sample->colorData;

            /* accumulate clamped values from filled sub-samples */
            totalClamped = 0.0f;
            totalWeight = 0.0f;
            for (i = 0; i < 4; i++)
            {
                float flag = sample->subSample[i];
                if (flag > 0.0f)
                {
                    float val = GammaCorrectColorChannel(ptr[i]);
                    if (val < 0.0f) val = 0.0f;
                    else if (val > 1.0f) val = 1.0f;
                    totalClamped += val;
                    totalWeight += 1.0f;
                }
            }

            /* fill empty sub-samples with normalized value */
            if (totalWeight != 0.0f)
            {
                normalized = DegammaColorChannel(totalClamped / totalWeight);
                for (i = 0; i < 4; i++)
                {
                    if (sample->subSample[i] == 0.0f)
                        ptr[i] = normalized;
                }
            }

            sample++;
        }
    }
}

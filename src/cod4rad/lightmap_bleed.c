/*
 * lightmap_bleed.c — Lightmap bleeding/edge extension for texture filtering.
 */

#include "cod2rad64.h"

/* external functions not in master header */
#include <stdlib.h>
extern void ForEachLightmapPixelInPoly(void *callback, int param, int threadCount);

void          *g_primaryBleedData;
void          *g_secondaryBleedData;

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

/*
================
Lmap_FindBleeding_Callback

Per-lightmap-pixel callback that calls the bleed finder for
both primary (512) and secondary (1024, coords scaled by 2x) data.
================
*/
void Lmap_FindBleeding_Callback(float unused, float *samplePos,
                                int unused1, int unused2, void *userData)
{
    /* KIWI: userData is &Triangle_t::lightmapIdx, an unsigned short (an int read
     * pulled in the next field's bytes and only worked through index wrap) */
    int lmapIdx = *(const unsigned short *)userData;

    /* primary: 512x512, original coords */
    Lmap_FindBleedingForSample(g_primaryBleedData, lmapIdx, 512, 512,
                               samplePos[0], samplePos[1]);

    /* secondary: 1024x1024, coords scaled by 2 */
    Lmap_FindBleedingForSample(g_secondaryBleedData, lmapIdx, 1024, 1024,
                               samplePos[0] * 2.0f, samplePos[1] * 2.0f);
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
    size_t allocSize;
    void *data;

    if (lmapCount < 0)
        ErrorMsg("Invalid lightmap count %i\n", lmapCount);
    allocSize = (size_t)lmapCount * 5 * (1 << 18);
    data = malloc(allocSize ? allocSize : 1);
    g_primaryBleedData = data;
    if (!data)
        ErrorMsg("Out of memory trying to allocate lightmap bleed info (%zu bytes)\n", allocSize);

    memset(g_primaryBleedData, 0, allocSize);

    g_secondaryBleedData = (char *)g_primaryBleedData + (size_t)lmapCount * (1 << 18);

    /* find bleeding for all lightmap pixels */
    ForEachLightmapPixelInPoly(Lmap_FindBleeding_Callback, 2, threadCount);

    /* Native cod4rad does not run the older Cod2 iterative sample-field
     * mutator here.  The masks remain live for lighting.cpp 0x415990,
     * 0x415B80 and 0x415D90 during final image construction.
     * KIWI: that cod2rad ping-pong bleed (Lmap_ApplyBleeding, its iterative
     * pass and sample callbacks) is gone. */
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
    /* KIWI FIX: triangles sharing a texel run on different threads; a plain |=
     * lost bits, so multi-threaded lightmaps differed run to run */
    InterlockedOr8((volatile char *)data + sampleIdx, (char)g_bleedDirTable[tableIdx]);
}

/*
 * adaptive.c — Adaptive lightmap resolution via forward shadow projection.
 *
 * Projects shadow stencils from each light source onto each surface.
 * Surfaces with complex shadow edges get more lightmap texels.
 * Surfaces with uniform lighting get fewer.
 */

#include "cod2rad64.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

extern int TraceVisibility_Embree(float *startPos, float *endPos);
extern int TraceStaticModels_Embree(float *startPos, float *endPos);

typedef struct {
    int surfIndex;
    int lightmapIdx;
    int firstVertex;
    int vertexCount;
    int firstIndex;
    int indexCount;
    float center[3];
    float normal[3];
    float lmMins[2];
    float lmMaxs[2];
    float worldWidth;
    float worldHeight;
    int groupId;
    int origWidth;
    int origHeight;
    int newWidth;
    int newHeight;
    float shadowScore;
    int newLightmapIdx;
    int newOffsetS;
    int newOffsetT;
} AdaptiveSurfInfo_t;

typedef struct LmBlock_s {
    int s, t, w, h;
    struct LmBlock_s *next;
} LmBlock_t;

static AdaptiveSurfInfo_t *g_adaptSurfs;
static int g_adaptSurfCount;
static int *g_pixelLabel;
static int g_numFloodIslands;

#define ADAPT_LM_SIZE 512
#define ADAPT_MAX_PAGES 31
#define ADAPT_PADDING 2

static void Adaptive_BuildSurfaceInfo(void)
{
    int i, v;
    int count = 0;

    for (i = 0; i < numBSPTriSoups; i++)
    {
        if (bspTriangles[i].lightmapIndex != 0x1F)
            count++;
    }

    g_adaptSurfs = (AdaptiveSurfInfo_t *)calloc(count, sizeof(AdaptiveSurfInfo_t));
    g_adaptSurfCount = 0;

    for (i = 0; i < numBSPTriSoups; i++)
    {
        BspTriSoup_t *ts = &bspTriangles[i];
        AdaptiveSurfInfo_t *s;
        DrawVert_t *dv;
        float invCount;

        if (ts->lightmapIndex == 0x1F)
            continue;

        s = &g_adaptSurfs[g_adaptSurfCount++];
        s->surfIndex = i;
        s->lightmapIdx = ts->lightmapIndex;
        s->firstVertex = ts->firstVertex;
        s->vertexCount = ts->vertexCount;
        s->firstIndex = ts->firstIndex;
        s->indexCount = ts->indexCount;

        s->center[0] = s->center[1] = s->center[2] = 0.0f;
        s->normal[0] = s->normal[1] = s->normal[2] = 0.0f;
        s->lmMins[0] = s->lmMins[1] = 1e30f;
        s->lmMaxs[0] = s->lmMaxs[1] = -1e30f;

        dv = &((DrawVert_t *)bspDrawVerts)[ts->firstVertex];
        for (v = 0; v < ts->vertexCount; v++)
        {
            s->center[0] += dv[v].pos[0];
            s->center[1] += dv[v].pos[1];
            s->center[2] += dv[v].pos[2];
            s->normal[0] += dv[v].normal[0];
            s->normal[1] += dv[v].normal[1];
            s->normal[2] += dv[v].normal[2];

            if (dv[v].lmCoord[0] < s->lmMins[0]) s->lmMins[0] = dv[v].lmCoord[0];
            if (dv[v].lmCoord[1] < s->lmMins[1]) s->lmMins[1] = dv[v].lmCoord[1];
            if (dv[v].lmCoord[0] > s->lmMaxs[0]) s->lmMaxs[0] = dv[v].lmCoord[0];
            if (dv[v].lmCoord[1] > s->lmMaxs[1]) s->lmMaxs[1] = dv[v].lmCoord[1];
        }

        invCount = 1.0f / (float)ts->vertexCount;
        s->center[0] *= invCount;
        s->center[1] *= invCount;
        s->center[2] *= invCount;

        {
            float len = sqrtf(s->normal[0]*s->normal[0] + s->normal[1]*s->normal[1] + s->normal[2]*s->normal[2]);
            if (len > 0.0f) { s->normal[0] /= len; s->normal[1] /= len; s->normal[2] /= len; }
        }

        s->origWidth = (int)ceilf(s->lmMaxs[0] * ADAPT_LM_SIZE + 0.5f) - (int)floorf(s->lmMins[0] * ADAPT_LM_SIZE - 0.5f) + 1;
        s->origHeight = (int)ceilf(s->lmMaxs[1] * ADAPT_LM_SIZE + 0.5f) - (int)floorf(s->lmMins[1] * ADAPT_LM_SIZE - 0.5f) + 1;
        if (s->origWidth < 1) s->origWidth = 1;
        if (s->origHeight < 1) s->origHeight = 1;

        /* compute world-space size from vertex bounding box */
        {
            float wmins[3] = { 1e30f, 1e30f, 1e30f };
            float wmaxs[3] = { -1e30f, -1e30f, -1e30f };
            int axis0, axis1;
            float absN[3];
            for (v = 0; v < ts->vertexCount; v++)
            {
                if (dv[v].pos[0] < wmins[0]) wmins[0] = dv[v].pos[0];
                if (dv[v].pos[1] < wmins[1]) wmins[1] = dv[v].pos[1];
                if (dv[v].pos[2] < wmins[2]) wmins[2] = dv[v].pos[2];
                if (dv[v].pos[0] > wmaxs[0]) wmaxs[0] = dv[v].pos[0];
                if (dv[v].pos[1] > wmaxs[1]) wmaxs[1] = dv[v].pos[1];
                if (dv[v].pos[2] > wmaxs[2]) wmaxs[2] = dv[v].pos[2];
            }
            absN[0] = fabsf(s->normal[0]);
            absN[1] = fabsf(s->normal[1]);
            absN[2] = fabsf(s->normal[2]);
            if (absN[2] >= absN[0] && absN[2] >= absN[1])
                { axis0 = 0; axis1 = 1; }
            else if (absN[0] >= absN[1])
                { axis0 = 1; axis1 = 2; }
            else
                { axis0 = 0; axis1 = 2; }
            s->worldWidth = wmaxs[axis0] - wmins[axis0];
            s->worldHeight = wmaxs[axis1] - wmins[axis1];
            if (s->worldWidth < 1.0f) s->worldWidth = 1.0f;
            if (s->worldHeight < 1.0f) s->worldHeight = 1.0f;
        }

        s->groupId = g_adaptSurfCount - 1; /* default: each surface is its own group */

        s->newWidth = s->origWidth;
        s->newHeight = s->origHeight;
    }

    /* find UV islands by actual shared vertex indices from index buffer */
    {
        extern int numBSPDrawVerts;
        extern unsigned short bspDrawIndexes[];
        int *parent;
        int *vertOwner;
        int numIslands, si;

        parent = (int *)malloc(g_adaptSurfCount * sizeof(int));
        for (si = 0; si < g_adaptSurfCount; si++)
            parent[si] = si;

        vertOwner = (int *)malloc(numBSPDrawVerts * sizeof(int));
        memset(vertOwner, -1, numBSPDrawVerts * sizeof(int));

        /* check actual index buffer references, not vertex ranges */
        for (si = 0; si < g_adaptSurfCount; si++)
        {
            int idx;
            for (idx = g_adaptSurfs[si].firstIndex; idx < g_adaptSurfs[si].firstIndex + g_adaptSurfs[si].indexCount; idx++)
            {
                int vi = bspDrawIndexes[idx];
                if (vi >= numBSPDrawVerts) continue;
                if (vertOwner[vi] == -1)
                    vertOwner[vi] = si;
                else if (vertOwner[vi] != si)
                {
                    int a = vertOwner[vi], b = si;
                    while (parent[a] != a) a = parent[a];
                    while (parent[b] != b) b = parent[b];
                    if (a != b) parent[b] = a;
                }
            }
        }
        free(vertOwner);

        numIslands = 0;
        for (si = 0; si < g_adaptSurfCount; si++)
        {
            int r = si;
            while (parent[r] != r) r = parent[r];
            g_adaptSurfs[si].groupId = r;
            if (r == si) numIslands++;
        }
        free(parent);

        /* merge island bounds to roots */
        for (si = 0; si < g_adaptSurfCount; si++)
        {
            int r = g_adaptSurfs[si].groupId;
            if (r != si)
            {
                if (g_adaptSurfs[si].lmMins[0] < g_adaptSurfs[r].lmMins[0])
                    g_adaptSurfs[r].lmMins[0] = g_adaptSurfs[si].lmMins[0];
                if (g_adaptSurfs[si].lmMins[1] < g_adaptSurfs[r].lmMins[1])
                    g_adaptSurfs[r].lmMins[1] = g_adaptSurfs[si].lmMins[1];
                if (g_adaptSurfs[si].lmMaxs[0] > g_adaptSurfs[r].lmMaxs[0])
                    g_adaptSurfs[r].lmMaxs[0] = g_adaptSurfs[si].lmMaxs[0];
                if (g_adaptSurfs[si].lmMaxs[1] > g_adaptSurfs[r].lmMaxs[1])
                    g_adaptSurfs[r].lmMaxs[1] = g_adaptSurfs[si].lmMaxs[1];
            }
        }

        for (si = 0; si < g_adaptSurfCount; si++)
        {
            if (g_adaptSurfs[si].groupId == si)
            {
                AdaptiveSurfInfo_t *s = &g_adaptSurfs[si];
                s->origWidth = (int)ceilf(s->lmMaxs[0]*512.0f+0.5f) - (int)floorf(s->lmMins[0]*512.0f-0.5f) + 1;
                s->origHeight = (int)ceilf(s->lmMaxs[1]*512.0f+0.5f) - (int)floorf(s->lmMins[1]*512.0f-0.5f) + 1;
                if (s->origWidth < 1) s->origWidth = 1;
                if (s->origHeight < 1) s->origHeight = 1;
            }
        }

        Com_Printf("Adaptive: %d UV islands from %d surfaces\n", numIslands, g_adaptSurfCount);
    }

    Com_Printf("Adaptive: %d lightmapped surfaces\n", g_adaptSurfCount);
}

static float Adaptive_AnalyzeSurface(AdaptiveSurfInfo_t *surf)
{
    DrawVert_t *dv = &((DrawVert_t *)bspDrawVerts)[surf->firstVertex];
    int gridRes, totalPoints, i, j;
    float *testPoints;
    int *sunHits;
    float sunBrightness, bestLightScore;
    int sunTransitions;
    float smallOffset = 0.125f;

    gridRes = (int)sqrtf((float)(surf->origWidth * surf->origHeight) * 0.25f);
    if (gridRes < 4) gridRes = 4;
    if (gridRes > 16) gridRes = 16;
    totalPoints = gridRes * gridRes;

    testPoints = (float *)malloc(totalPoints * 3 * sizeof(float));
    sunHits = (int *)calloc(totalPoints, sizeof(int));

    /* generate test points via world-space bounding box grid on surface plane */
    {
        float wmins[3] = { 1e30f, 1e30f, 1e30f };
        float wmaxs[3] = { -1e30f, -1e30f, -1e30f };
        int axis0, axis1;
        float absN[3];
        int vi;

        for (vi = 0; vi < surf->vertexCount; vi++)
        {
            if (dv[vi].pos[0] < wmins[0]) wmins[0] = dv[vi].pos[0];
            if (dv[vi].pos[1] < wmins[1]) wmins[1] = dv[vi].pos[1];
            if (dv[vi].pos[2] < wmins[2]) wmins[2] = dv[vi].pos[2];
            if (dv[vi].pos[0] > wmaxs[0]) wmaxs[0] = dv[vi].pos[0];
            if (dv[vi].pos[1] > wmaxs[1]) wmaxs[1] = dv[vi].pos[1];
            if (dv[vi].pos[2] > wmaxs[2]) wmaxs[2] = dv[vi].pos[2];
        }

        absN[0] = fabsf(surf->normal[0]);
        absN[1] = fabsf(surf->normal[1]);
        absN[2] = fabsf(surf->normal[2]);
        if (absN[2] >= absN[0] && absN[2] >= absN[1])
            { axis0 = 0; axis1 = 1; }
        else if (absN[0] >= absN[1])
            { axis0 = 1; axis1 = 2; }
        else
            { axis0 = 0; axis1 = 2; }

        for (i = 0; i < gridRes; i++)
        {
            for (j = 0; j < gridRes; j++)
            {
                float u = ((float)i + 0.5f) / (float)gridRes;
                float v = ((float)j + 0.5f) / (float)gridRes;
                int idx = i * gridRes + j;
                testPoints[idx*3+0] = surf->center[0] + surf->normal[0] * smallOffset;
                testPoints[idx*3+1] = surf->center[1] + surf->normal[1] * smallOffset;
                testPoints[idx*3+2] = surf->center[2] + surf->normal[2] * smallOffset;
                testPoints[idx*3+axis0] = wmins[axis0] + (wmaxs[axis0] - wmins[axis0]) * u;
                testPoints[idx*3+axis1] = wmins[axis1] + (wmaxs[axis1] - wmins[axis1]) * v;
            }
        }
    }

    /* sun stencil */
    sunBrightness = g_sunColorR * 0.299f + g_sunColorG * 0.587f + g_sunColorB * 0.114f;
    {
        float sunDot = surf->normal[0]*g_sunDirX + surf->normal[1]*g_sunDirY + surf->normal[2]*g_sunDirZ;
        if (sunDot > 0.0f)
        {
            for (i = 0; i < totalPoints; i++)
            {
                float endPos[3];
                endPos[0] = testPoints[i*3+0] + g_sunDirX * 262144.0f;
                endPos[1] = testPoints[i*3+1] + g_sunDirY * 262144.0f;
                endPos[2] = testPoints[i*3+2] + g_sunDirZ * 262144.0f;

                sunHits[i] = TraceVisibility_Embree(&testPoints[i*3], endPos) ? 0 : 1;
            }
        }
    }

    /* count sun transitions */
    sunTransitions = 0;
    for (i = 0; i < gridRes; i++)
    {
        for (j = 0; j < gridRes; j++)
        {
            int idx = i * gridRes + j;
            if (j < gridRes-1 && sunHits[idx] != sunHits[idx+1]) sunTransitions++;
            if (i < gridRes-1 && sunHits[idx] != sunHits[idx+gridRes]) sunTransitions++;
        }
    }

    bestLightScore = 0.0f;

    /* point/spot light stencils */
    {
        int *lightHits = (int *)calloc(totalPoints, sizeof(int));
        int li;

        for (li = 0; li < g_numPointLights; li++)
        {
            PointLight_t *light = &g_pointLights[li];
            float dx, dy, dz, dist, dot, intensity;
            int lightTransitions;

            dx = light->origin[0] - surf->center[0];
            dy = light->origin[1] - surf->center[1];
            dz = light->origin[2] - surf->center[2];
            dist = sqrtf(dx*dx + dy*dy + dz*dz);

            if (dist > light->radius || dist < 0.001f)
                continue;

            dot = (dx*surf->normal[0] + dy*surf->normal[1] + dz*surf->normal[2]) / dist;
            if (dot < 0.0f)
                continue;

            if (light->isSpot)
            {
                float spotDot = -(dx*light->spotDir[0] + dy*light->spotDir[1] + dz*light->spotDir[2]) / dist;
                if (spotDot < light->spotCosAngle)
                    continue;
            }

            intensity = (light->color[0]*0.299f + light->color[1]*0.587f + light->color[2]*0.114f)
                      * (1.0f - dist / light->radius);

            if (intensity < 0.01f)
                continue;

            memset(lightHits, 0, totalPoints * sizeof(int));
            for (i = 0; i < totalPoints; i++)
            {
                float vis = TraceVisibility_Embree(&testPoints[i*3], light->origin);
                lightHits[i] = vis ? 0 : 1;
            }

            lightTransitions = 0;
            for (i = 0; i < gridRes; i++)
            {
                for (j = 0; j < gridRes; j++)
                {
                    int idx = i * gridRes + j;
                    if (j < gridRes-1 && lightHits[idx] != lightHits[idx+1]) lightTransitions++;
                    if (i < gridRes-1 && lightHits[idx] != lightHits[idx+gridRes]) lightTransitions++;
                }
            }

            {
                float score = (float)lightTransitions * intensity;
                if (score > bestLightScore) bestLightScore = score;
            }
        }

        free(lightHits);
    }

    free(testPoints);
    free(sunHits);

    {
        int maxTransitions = 2 * gridRes * (gridRes - 1);
        float sunScore = (maxTransitions > 0) ? ((float)sunTransitions / (float)maxTransitions) * sunBrightness : 0.0f;
        float combined = sunScore > bestLightScore ? sunScore : bestLightScore;
        return combined;
    }
}

static int CompareAdaptSurfByArea(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    int areaA = g_adaptSurfs[ia].newWidth * g_adaptSurfs[ia].newHeight;
    int areaB = g_adaptSurfs[ib].newWidth * g_adaptSurfs[ib].newHeight;
    return areaB - areaA;
}

static int CompareFloatDesc(const void *a, const void *b)
{
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    if (fa > fb) return -1;
    if (fa < fb) return 1;
    return 0;
}

static LmBlock_t *g_freeBlocks[ADAPT_MAX_PAGES];
static int g_numAdaptPages;

/* -----------------------------------------------------------------------
 * Bitmap-based lightmap atlas packer
 *
 * Each page is a 512x512 occupancy grid. Islands are sorted by area
 * (largest first), then placed at the first position where they fit
 * without overlapping any occupied texel. 1 texel of padding is added
 * around each island.
 * ----------------------------------------------------------------------- */

#define BP_PAGE_SIZE  512
#define BP_MAX_PAGES  31
#define BP_PADDING    1

typedef struct {
    int width;      /* island bounding box width in texels */
    int height;     /* island bounding box height in texels */
    unsigned char *mask; /* rasterized shape: mask[y*width+x], 1=occupied */
    int pageOut;    /* output: which page (0-30) */
    int xOut;       /* output: x offset on page */
    int yOut;       /* output: y offset on page */
    int packed;     /* output: 1 if successfully packed, 0 if failed */
} PackIsland_t;

/* occupancy bitmaps — one per page, 0 = free, 1 = occupied */
static unsigned char *bp_pages[BP_MAX_PAGES];
static int bp_numPages;

/* clear all page bitmaps */
void BitmapPacker_Init(void)
{
    int i;
    for (i = 0; i < BP_MAX_PAGES; i++)
        bp_pages[i] = NULL;
    bp_numPages = 0;
}

/* free all allocated page bitmaps */
void BitmapPacker_Free(void)
{
    int i;
    for (i = 0; i < BP_MAX_PAGES; i++)
    {
        if (bp_pages[i])
        {
            free(bp_pages[i]);
            bp_pages[i] = NULL;
        }
    }
    bp_numPages = 0;
}

/* allocate and zero a new page bitmap, returns page index or -1 if full */
static int BitmapPacker_NewPage(void)
{
    if (bp_numPages >= BP_MAX_PAGES)
        return -1;

    bp_pages[bp_numPages] = (unsigned char *)calloc(BP_PAGE_SIZE * BP_PAGE_SIZE, 1);
    if (!bp_pages[bp_numPages])
        return -1;

    return bp_numPages++;
}

/* check if an island's SHAPE fits at position (px, py) on the given page.
 * Uses the island's bitmask — only checks texels where mask is 1.
 * Adds BP_PADDING (1 pixel) around each occupied texel. */
static int BitmapPacker_TryFitShape(int page, int px, int py, PackIsland_t *isle)
{
    unsigned char *bitmap;
    int y, x, mx, my;
    int pad = BP_PADDING;

    if (px + isle->width + pad*2 > BP_PAGE_SIZE || py + isle->height + pad*2 > BP_PAGE_SIZE)
        return 0;

    bitmap = bp_pages[page];
    for (my = 0; my < isle->height; my++)
    {
        for (mx = 0; mx < isle->width; mx++)
        {
            if (!isle->mask[my * isle->width + mx])
                continue;
            /* check this texel + padding neighborhood */
            for (y = my + py - pad; y <= my + py + pad; y++)
            {
                if (y < 0 || y >= BP_PAGE_SIZE) continue;
                for (x = mx + px - pad; x <= mx + px + pad; x++)
                {
                    if (x < 0 || x >= BP_PAGE_SIZE) continue;
                    if (bitmap[y * BP_PAGE_SIZE + x])
                        return 0;
                }
            }
        }
    }
    return 1;
}

/* mark an island's shape as occupied on a page */
static void BitmapPacker_MarkShape(int page, int px, int py, PackIsland_t *isle)
{
    unsigned char *bitmap = bp_pages[page];
    int mx, my;

    for (my = 0; my < isle->height; my++)
    {
        for (mx = 0; mx < isle->width; mx++)
        {
            if (isle->mask[my * isle->width + mx])
                bitmap[(my + py) * BP_PAGE_SIZE + (mx + px)] = 1;
        }
    }
}

/* try to place an island's shape on an existing page.
 * scans left-to-right, top-to-bottom for the first free spot. */
static int BitmapPacker_PlaceOnPage(int page, PackIsland_t *isle, int *outX, int *outY)
{
    int y, x;
    int maxY = BP_PAGE_SIZE - isle->height;
    int maxX = BP_PAGE_SIZE - isle->width;

    for (y = 0; y <= maxY; y++)
    {
        for (x = 0; x <= maxX; x++)
        {
            if (BitmapPacker_TryFitShape(page, x, y, isle))
            {
                *outX = x;
                *outY = y;
                return 1;
            }
        }
    }
    return 0;
}

/* we need the island array accessible during qsort, so use a file-scope pointer */
static PackIsland_t *bp_sortIslands;

static int BitmapPacker_CompareByArea(const void *a, const void *b)
{
    int idxA = *(const int *)a;
    int idxB = *(const int *)b;
    int areaA = bp_sortIslands[idxA].width * bp_sortIslands[idxA].height;
    int areaB = bp_sortIslands[idxB].width * bp_sortIslands[idxB].height;

    /* largest first */
    if (areaB > areaA) return 1;
    if (areaB < areaA) return -1;

    /* tie-break: taller first, then wider */
    if (bp_sortIslands[idxB].height != bp_sortIslands[idxA].height)
        return bp_sortIslands[idxB].height - bp_sortIslands[idxA].height;
    return bp_sortIslands[idxB].width - bp_sortIslands[idxA].width;
}

/* pack all islands, returns number of pages used.
 * each island gets 1 texel of padding on all sides.
 * islands too large for a single page (>510 in either dimension) are marked as failed. */
int BitmapPacker_Pack(PackIsland_t *islands, int count)
{
    int *order;
    int i, placed;

    if (count <= 0)
        return 0;

    /* allocate sort-order index array */
    order = (int *)malloc(count * sizeof(int));
    for (i = 0; i < count; i++)
    {
        order[i] = i;
        islands[i].packed = 0;
        islands[i].pageOut = -1;
        islands[i].xOut = 0;
        islands[i].yOut = 0;
    }

    /* sort by area, largest first */
    bp_sortIslands = islands;
    qsort(order, count, sizeof(int), BitmapPacker_CompareByArea);

    /* ensure at least one page exists */
    if (bp_numPages == 0)
    {
        if (BitmapPacker_NewPage() < 0)
        {
            free(order);
            return 0;
        }
    }

    /* place each island using shape-based fitting */
    for (i = 0; i < count; i++)
    {
        int idx = order[i];
        PackIsland_t *isle = &islands[idx];
        int page;
        int ox, oy;

        if (!isle->mask || isle->width <= 0 || isle->height <= 0)
            continue;
        if (isle->width > BP_PAGE_SIZE || isle->height > BP_PAGE_SIZE)
            continue;

        placed = 0;

        for (page = 0; page < bp_numPages; page++)
        {
            if (BitmapPacker_PlaceOnPage(page, isle, &ox, &oy))
            {
                BitmapPacker_MarkShape(page, ox, oy, isle);
                isle->pageOut = page;
                isle->xOut = ox;
                isle->yOut = oy;
                isle->packed = 1;
                placed = 1;
                break;
            }
        }

        if (!placed)
        {
            page = BitmapPacker_NewPage();
            if (page >= 0 && BitmapPacker_PlaceOnPage(page, isle, &ox, &oy))
            {
                BitmapPacker_MarkShape(page, ox, oy, isle);
                isle->pageOut = page;
                isle->xOut = ox;
                isle->yOut = oy;
                isle->packed = 1;
            }
        }
    }

    free(order);
    bp_sortIslands = NULL;

    return bp_numPages;
}

/* ----------------------------------------------------------------------- */

static int Adaptive_FindAndAlloc(int reqW, int reqH, int *outPage, int *outS, int *outT)
{
    int page;
    LmBlock_t *blk, *prev, *best, *bestPrev;
    int bestWaste, bestPage;

    reqW += ADAPT_PADDING;
    reqH += ADAPT_PADDING;

    best = NULL; bestPrev = NULL; bestWaste = 0x7FFFFFFF; bestPage = -1;

    for (page = 0; page < g_numAdaptPages; page++)
    {
        prev = NULL;
        for (blk = g_freeBlocks[page]; blk; prev = blk, blk = blk->next)
        {
            if (blk->w >= reqW && blk->h >= reqH)
            {
                int waste = blk->w * blk->h - reqW * reqH;
                if (waste < bestWaste) { best = blk; bestPrev = prev; bestWaste = waste; bestPage = page; }
            }
            if (blk->w >= reqH && blk->h >= reqW)
            {
                int waste = blk->w * blk->h - reqW * reqH;
                if (waste < bestWaste) { best = blk; bestPrev = prev; bestWaste = waste; bestPage = page; }
            }
        }
    }

    if (!best)
    {
        if (g_numAdaptPages >= ADAPT_MAX_PAGES)
            return 0;
        page = g_numAdaptPages++;
        best = (LmBlock_t *)malloc(sizeof(LmBlock_t));
        best->s = 0; best->t = 0; best->w = ADAPT_LM_SIZE; best->h = ADAPT_LM_SIZE;
        best->next = NULL;
        g_freeBlocks[page] = best;
        bestPrev = NULL;
        bestPage = page;
    }

    /* check if we need to swap for rotation */
    if (best->w >= reqW && best->h >= reqH)
    {
        *outS = best->s; *outT = best->t; *outPage = bestPage;
    }
    else
    {
        int tmp = reqW; reqW = reqH; reqH = tmp;
        *outS = best->s; *outT = best->t; *outPage = bestPage;
    }

    /* split remainder */
    {
        int remW = best->w - reqW;
        int remH = best->h - reqH;

        if (bestPrev) bestPrev->next = best->next;
        else g_freeBlocks[bestPage] = best->next;

        if (remW > ADAPT_PADDING && remH > ADAPT_PADDING)
        {
            LmBlock_t *right = (LmBlock_t *)malloc(sizeof(LmBlock_t));
            right->s = best->s + reqW; right->t = best->t;
            right->w = remW; right->h = reqH;
            right->next = g_freeBlocks[bestPage];
            g_freeBlocks[bestPage] = right;

            LmBlock_t *bottom = (LmBlock_t *)malloc(sizeof(LmBlock_t));
            bottom->s = best->s; bottom->t = best->t + reqH;
            bottom->w = best->w; bottom->h = remH;
            bottom->next = g_freeBlocks[bestPage];
            g_freeBlocks[bestPage] = bottom;
        }
        else if (remW > ADAPT_PADDING)
        {
            LmBlock_t *right = (LmBlock_t *)malloc(sizeof(LmBlock_t));
            right->s = best->s + reqW; right->t = best->t;
            right->w = remW; right->h = best->h;
            right->next = g_freeBlocks[bestPage];
            g_freeBlocks[bestPage] = right;
        }
        else if (remH > ADAPT_PADDING)
        {
            LmBlock_t *bottom = (LmBlock_t *)malloc(sizeof(LmBlock_t));
            bottom->s = best->s; bottom->t = best->t + reqH;
            bottom->w = best->w; bottom->h = remH;
            bottom->next = g_freeBlocks[bestPage];
            g_freeBlocks[bestPage] = bottom;
        }

        free(best);
    }

    return 1;
}

static void Adaptive_RewriteUVs(void)
{
    int i, v;
    DrawVert_t *allVerts = (DrawVert_t *)bspDrawVerts;

    for (i = 0; i < g_adaptSurfCount; i++)
    {
        AdaptiveSurfInfo_t *s = &g_adaptSurfs[i];
        float tangent[3], bitangent[3];
        float wmins[3] = { 1e30f, 1e30f, 1e30f };
        float wmaxs[3] = { -1e30f, -1e30f, -1e30f };
        int axis0, axis1, axisN;
        float absN[3];
        float wExtent0, wExtent1;

        /* scale and reposition — use island root's combined bounds */
        {
            int r = s->groupId;
            double origMinS = (double)floorf(g_adaptSurfs[r].lmMins[0] * ADAPT_LM_SIZE - 0.5f);
            double origMinT = (double)floorf(g_adaptSurfs[r].lmMins[1] * ADAPT_LM_SIZE - 0.5f);
            double origW = (double)g_adaptSurfs[r].origWidth;
            double origH = (double)g_adaptSurfs[r].origHeight;
            double newW = (double)s->newWidth;
            double newH = (double)s->newHeight;

            if (origW < 1.0) origW = 1.0;
            if (origH < 1.0) origH = 1.0;

            for (v = 0; v < s->vertexCount; v++)
            {
                DrawVert_t *dv = &allVerts[s->firstVertex + v];
                double origPixS = (double)dv->lmCoord[0] * ADAPT_LM_SIZE;
                double origPixT = (double)dv->lmCoord[1] * ADAPT_LM_SIZE;

                double normS = (origPixS - origMinS) / origW;
                double normT = (origPixT - origMinT) / origH;

                double newPixS = normS * newW + (double)s->newOffsetS;
                double newPixT = normT * newH + (double)s->newOffsetT;

                if (newPixS < 0.0) newPixS = 0.0;
                if (newPixT < 0.0) newPixT = 0.0;
                if (newPixS > (double)ADAPT_LM_SIZE) newPixS = (double)ADAPT_LM_SIZE;
                if (newPixT > (double)ADAPT_LM_SIZE) newPixT = (double)ADAPT_LM_SIZE;

                dv->lmCoord[0] = (float)(newPixS / (double)ADAPT_LM_SIZE);
                dv->lmCoord[1] = (float)(newPixT / (double)ADAPT_LM_SIZE);
            }
        }

        bspTriangles[s->surfIndex].lightmapIndex = (unsigned short)s->newLightmapIdx;
    }

    /* patch g_triangles lightmapIdx — match by vertex index range */
    {
        extern int g_triCount;
        extern Triangle_t g_triangles[];
        for (i = 0; i < g_triCount; i++)
        {
            if (g_triangles[i].lightmapIdx == 0x1F)
                continue;
            {
                int vi = g_triangles[i].vertIndex[0];
                int si;
                for (si = 0; si < g_adaptSurfCount; si++)
                {
                    if (vi >= g_adaptSurfs[si].firstVertex &&
                        vi < g_adaptSurfs[si].firstVertex + g_adaptSurfs[si].vertexCount)
                    {
                        g_triangles[i].lightmapIdx = (unsigned short)g_adaptSurfs[si].newLightmapIdx;
                        break;
                    }
                }
            }
        }
    }
}

static void ExportUVLayoutBMP(const char *filename, int pageCount)
{
    /* export all lightmap pages as one tall BMP — each page 512x512, stacked vertically */
    /* color each surface's UV region with a unique color */
    extern int numBSPDrawVerts;
    DrawVert_t *allVerts = (DrawVert_t *)bspDrawVerts;
    int totalH = pageCount * 512;
    int rowBytes = (512 * 3 + 3) & ~3;
    unsigned char *pixels;
    FILE *fp;
    int i, v;

    if (pageCount < 1) pageCount = 1;
    if (pageCount > 31) pageCount = 31;
    totalH = pageCount * 512;

    pixels = (unsigned char *)calloc(rowBytes * totalH, 1);

    for (i = 0; i < g_adaptSurfCount; i++)
    {
        AdaptiveSurfInfo_t *s = &g_adaptSurfs[i];
        int page = bspTriangles[s->surfIndex].lightmapIndex;
        unsigned char r = (unsigned char)((i * 73 + 37) & 0xFF);
        unsigned char g = (unsigned char)((i * 151 + 89) & 0xFF);
        unsigned char b = (unsigned char)((i * 211 + 13) & 0xFF);
        if (r < 40) r += 40; if (g < 40) g += 40; if (b < 40) b += 40;

        if (page >= pageCount || page == 0x1F) continue;

        for (v = 0; v < s->vertexCount; v++)
        {
            DrawVert_t *dv = &allVerts[s->firstVertex + v];
            int px = (int)(dv->lmCoord[0] * 511.0f);
            int py = (int)(dv->lmCoord[1] * 511.0f) + page * 512;
            if (px < 0) px = 0; if (px > 511) px = 511;
            if (py < 0) py = 0; if (py >= totalH) py = totalH - 1;

            /* draw a small cross at each vertex */
            {
                int dx, dy;
                for (dx = -1; dx <= 1; dx++)
                    for (dy = -1; dy <= 1; dy++)
                    {
                        int xx = px + dx, yy = py + dy;
                        if (xx >= 0 && xx < 512 && yy >= 0 && yy < totalH)
                        {
                            pixels[yy * rowBytes + xx * 3 + 0] = b;
                            pixels[yy * rowBytes + xx * 3 + 1] = g;
                            pixels[yy * rowBytes + xx * 3 + 2] = r;
                        }
                    }
            }
        }
    }

    fp = fopen(filename, "wb");
    if (fp)
    {
        unsigned char hdr[54] = {0};
        int fileSize = 54 + rowBytes * totalH;
        hdr[0]='B'; hdr[1]='M';
        *(int*)&hdr[2] = fileSize;
        *(int*)&hdr[10] = 54;
        *(int*)&hdr[14] = 40;
        *(int*)&hdr[18] = 512;
        *(int*)&hdr[22] = totalH;
        *(short*)&hdr[26] = 1;
        *(short*)&hdr[28] = 24;
        fwrite(hdr, 1, 54, fp);
        /* BMP stores rows bottom-up */
        for (i = totalH - 1; i >= 0; i--)
            fwrite(&pixels[i * rowBytes], 1, rowBytes, fp);
        fclose(fp);
        Com_Printf("Exported UV layout: %s (%dx%d)\n", filename, 512, totalH);
    }
    free(pixels);
}

void AdaptiveLightmapRepack(int threadCount)
{
    extern int numBSPDrawVerts;
    extern unsigned short bspDrawIndexes[];
    extern int g_triCount;
    extern Triangle_t g_triangles[];

    DrawVert_t *allVerts = (DrawVert_t *)bspDrawVerts;
    unsigned char *pixGrid;       /* 512x512 occupancy for rasterization */
    int *labels;                  /* 512x512 flood-fill labels */
    int numIslands = 0;
    int i, s, v;

    typedef struct { unsigned char *mask; int w, h, offX, offY, pixels, placed; int newX, newY; } IslandInfo_t;
    IslandInfo_t *islandArr;
    int *islandOfSurf;            /* which island each surface belongs to */

    int origPageCount = g_lightmapSize;
    int numPages;

    Adaptive_BuildSurfaceInfo();

    if (g_adaptSurfCount == 0)
        return;

    if (origPageCount < 1) origPageCount = 1;

    /* Step 1: rasterize filled UV triangles onto 512x512 grid */
    pixGrid = (unsigned char *)calloc(512 * 512, 1);
    for (s = 0; s < g_adaptSurfCount; s++)
    {
        AdaptiveSurfInfo_t *surf = &g_adaptSurfs[s];
        int numTris = surf->indexCount / 3;
        int t;
        for (t = 0; t < numTris; t++)
        {
            int idx = surf->firstIndex + t * 3;
            int base = surf->firstVertex;
            float x0 = allVerts[base + bspDrawIndexes[idx+0]].lmCoord[0] * 511.0f;
            float y0 = allVerts[base + bspDrawIndexes[idx+0]].lmCoord[1] * 511.0f;
            float x1 = allVerts[base + bspDrawIndexes[idx+1]].lmCoord[0] * 511.0f;
            float y1 = allVerts[base + bspDrawIndexes[idx+1]].lmCoord[1] * 511.0f;
            float x2 = allVerts[base + bspDrawIndexes[idx+2]].lmCoord[0] * 511.0f;
            float y2 = allVerts[base + bspDrawIndexes[idx+2]].lmCoord[1] * 511.0f;
            int minY = (int)floorf(y0<y1?(y0<y2?y0:y2):(y1<y2?y1:y2));
            int maxY = (int)ceilf(y0>y1?(y0>y2?y0:y2):(y1>y2?y1:y2));
            int scanY;
            if (minY < 0) minY = 0; if (maxY > 511) maxY = 511;
            for (scanY = minY; scanY <= maxY; scanY++)
            {
                float fy = (float)scanY + 0.5f;
                float xI[6]; int nX = 0;
                float ex[3][2] = {{x0,y0},{x1,y1},{x2,y2}};
                int e;
                for (e = 0; e < 3; e++)
                {
                    float ay = ex[e][1], by = ex[(e+1)%3][1];
                    if ((ay <= fy && by > fy) || (by <= fy && ay > fy))
                    { float tt = (fy-ay)/(by-ay); xI[nX++] = ex[e][0]+tt*(ex[(e+1)%3][0]-ex[e][0]); }
                }
                if (nX >= 2)
                {
                    int xMin, xMax, fx;
                    if (xI[0] > xI[1]) { float tmp=xI[0]; xI[0]=xI[1]; xI[1]=tmp; }
                    xMin = (int)floorf(xI[0]); xMax = (int)ceilf(xI[1]);
                    if (xMin < 0) xMin = 0; if (xMax > 511) xMax = 511;
                    for (fx = xMin; fx <= xMax; fx++) pixGrid[scanY*512+fx] = 1;
                }
            }
        }
    }

    /* Step 2: flood-fill 4-connected to find islands */
    labels = (int *)malloc(512 * 512 * sizeof(int));
    memset(labels, -1, 512 * 512 * sizeof(int));
    {
        int *stack = (int *)malloc(512 * 512 * sizeof(int));
        int x, y;
        for (y = 0; y < 512; y++)
            for (x = 0; x < 512; x++)
            {
                if (!pixGrid[y*512+x] || labels[y*512+x] >= 0) continue;
                { int top = 0; stack[top++] = y*512+x; labels[y*512+x] = numIslands;
                  while (top > 0) {
                    int ci = stack[--top], cy = ci/512, cx = ci%512;
                    int dirs[4][2] = {{0,-1},{0,1},{-1,0},{1,0}}; int d;
                    for (d = 0; d < 4; d++) {
                        int nx=cx+dirs[d][0], ny=cy+dirs[d][1];
                        if (nx>=0&&nx<512&&ny>=0&&ny<512&&pixGrid[ny*512+nx]&&labels[ny*512+nx]<0)
                        { labels[ny*512+nx] = numIslands; stack[top++] = ny*512+nx; }
                    }
                  }
                  numIslands++;
                }
            }
        free(stack);
    }
    free(pixGrid);
    Com_Printf("Adaptive: %d UV islands from %d surfaces\n", numIslands, g_adaptSurfCount);

    /* Step 3: extract island bitmasks with 1px dilation */
    islandArr = (IslandInfo_t *)calloc(numIslands, sizeof(IslandInfo_t));
    for (i = 0; i < numIslands; i++) { islandArr[i].offX = 512; islandArr[i].offY = 512; }
    for (i = 0; i < 512*512; i++)
    {
        if (labels[i] >= 0) {
            int lbl = labels[i], px = i%512, py = i/512;
            islandArr[lbl].pixels++;
            if (px < islandArr[lbl].offX) islandArr[lbl].offX = px;
            if (py < islandArr[lbl].offY) islandArr[lbl].offY = py;
            if (px - islandArr[lbl].offX + 1 > islandArr[lbl].w) islandArr[lbl].w = px - islandArr[lbl].offX + 1;
            if (py - islandArr[lbl].offY + 1 > islandArr[lbl].h) islandArr[lbl].h = py - islandArr[lbl].offY + 1;
        }
    }
    for (i = 0; i < numIslands; i++)
        if (islandArr[i].pixels > 0) islandArr[i].mask = (unsigned char *)calloc(islandArr[i].w * islandArr[i].h, 1);
    for (i = 0; i < 512*512; i++)
        if (labels[i] >= 0) {
            int lbl = labels[i];
            int lx = i%512 - islandArr[lbl].offX, ly = i/512 - islandArr[lbl].offY;
            if (islandArr[lbl].mask) islandArr[lbl].mask[ly * islandArr[lbl].w + lx] = 1;
        }

    /* dilate masks by 1px */
    for (i = 0; i < numIslands; i++)
    {
        int dw, dh; unsigned char *dilated; int mx, my;
        if (!islandArr[i].mask) continue;
        dw = islandArr[i].w + 2; dh = islandArr[i].h + 2;
        dilated = (unsigned char *)calloc(dw * dh, 1);
        for (my = 0; my < islandArr[i].h; my++)
            for (mx = 0; mx < islandArr[i].w; mx++)
                if (islandArr[i].mask[my * islandArr[i].w + mx])
                { int dx, dy; for (dy=0;dy<=2;dy++) for(dx=0;dx<=2;dx++) dilated[(my+dy)*dw+(mx+dx)]=1; }
        free(islandArr[i].mask);
        islandArr[i].mask = dilated; islandArr[i].w = dw; islandArr[i].h = dh;
    }

    /* Step 4: brute force bitmap pack — largest first */
    {
        int *sortOrder = (int *)malloc(numIslands * sizeof(int));
        unsigned char *atlas = (unsigned char *)calloc(512 * 512, 1);
        int placed = 0;

        for (i = 0; i < numIslands; i++) sortOrder[i] = i;
        /* bubble sort by pixels desc (good enough for <2000 islands) */
        for (i = 0; i < numIslands-1; i++)
            for (v = i+1; v < numIslands; v++)
                if (islandArr[sortOrder[v]].pixels > islandArr[sortOrder[i]].pixels)
                { int tmp=sortOrder[i]; sortOrder[i]=sortOrder[v]; sortOrder[v]=tmp; }

        for (i = 0; i < numIslands; i++)
        {
            int idx = sortOrder[i];
            IslandInfo_t *isle = &islandArr[idx];
            int ay, ax;
            if (!isle->mask || isle->pixels == 0 || isle->w > 512 || isle->h > 512) continue;

            for (ay = 0; ay <= 512 - isle->h; ay++)
            {
                int found = 0;
                for (ax = 0; ax <= 512 - isle->w; ax++)
                {
                    int fits = 1; int my, mx;
                    for (my = 0; my < isle->h && fits; my++)
                        for (mx = 0; mx < isle->w && fits; mx++)
                            if (isle->mask[my*isle->w+mx] && atlas[(ay+my)*512+(ax+mx)]) fits = 0;
                    if (fits)
                    {
                        for (my = 0; my < isle->h; my++)
                            for (mx = 0; mx < isle->w; mx++)
                                if (isle->mask[my*isle->w+mx]) atlas[(ay+my)*512+(ax+mx)] = 1;
                        isle->placed = 1; isle->newX = ax; isle->newY = ay; placed++;
                        found = 1; break;
                    }
                }
                if (found) break;
            }
        }
        Com_Printf("Adaptive: packed %d / %d islands into 1 page\n", placed, numIslands);
        free(sortOrder); free(atlas);
    }

    /* Step 5: assign each surface to its island */
    islandOfSurf = (int *)malloc(g_adaptSurfCount * sizeof(int));
    memset(islandOfSurf, -1, g_adaptSurfCount * sizeof(int));
    for (s = 0; s < g_adaptSurfCount; s++)
    {
        AdaptiveSurfInfo_t *surf = &g_adaptSurfs[s];
        for (v = 0; v < surf->vertexCount && islandOfSurf[s] < 0; v++)
        {
            DrawVert_t *dv = &allVerts[surf->firstVertex + v];
            int px = (int)(dv->lmCoord[0] * 511.0f);
            int py = (int)(dv->lmCoord[1] * 511.0f);
            if (px >= 0 && px < 512 && py >= 0 && py < 512 && labels[py*512+px] >= 0)
                islandOfSurf[s] = labels[py*512+px];
        }
    }

    { int assigned = 0, unassigned = 0;
      for (s = 0; s < g_adaptSurfCount; s++)
          if (islandOfSurf[s] >= 0) assigned++; else unassigned++;
      Com_Printf("Adaptive: %d surfaces assigned to islands, %d unassigned\n", assigned, unassigned);
    }

    /* Step 6: rewrite UVs — transform each vertex from old island position to new */
    {
        char *vertDone = (char *)calloc(numBSPDrawVerts, 1);
        for (s = 0; s < g_adaptSurfCount; s++)
        {
            int isle = islandOfSurf[s];
            if (isle < 0 || !islandArr[isle].placed) continue;

            for (v = 0; v < g_adaptSurfs[s].vertexCount; v++)
            {
                int vi = g_adaptSurfs[s].firstVertex + v;
                DrawVert_t *dv;
                double oldPixS, oldPixT, newPixS, newPixT;

                if (vi >= numBSPDrawVerts || vertDone[vi]) continue;
                vertDone[vi] = 1;

                dv = &allVerts[vi];
                oldPixS = (double)dv->lmCoord[0] * 512.0;
                oldPixT = (double)dv->lmCoord[1] * 512.0;

                /* shift from old island position to new */
                newPixS = oldPixS - (double)islandArr[isle].offX + (double)islandArr[isle].newX + 1.0;
                newPixT = oldPixT - (double)islandArr[isle].offY + (double)islandArr[isle].newY + 1.0;

                if (newPixS < 0.0) newPixS = 0.0;
                if (newPixT < 0.0) newPixT = 0.0;
                if (newPixS > 512.0) newPixS = 512.0;
                if (newPixT > 512.0) newPixT = 512.0;

                dv->lmCoord[0] = (float)(newPixS / 512.0);
                dv->lmCoord[1] = (float)(newPixT / 512.0);
            }

            bspTriangles[g_adaptSurfs[s].surfIndex].lightmapIndex = 0;
        }

        /* patch g_triangles */
        for (i = 0; i < g_triCount; i++)
        {
            if (g_triangles[i].lightmapIdx != 0x1F)
                g_triangles[i].lightmapIdx = 0;
        }

        free(vertDone);
    }

    /* update lightmap size */
    if (g_lightmapSize < 1) g_lightmapSize = 1;
    {
        extern void *g_lightingSamples;
        if (g_lightingSamples)
        {
            free(g_lightingSamples);
            g_lightingSamples = malloc((unsigned long long)g_lightmapSize * 0x800000);
            if (g_lightingSamples)
                memset(g_lightingSamples, 0, (unsigned long long)g_lightmapSize << 23);
        }
    }

    Com_Printf("Adaptive: lightmap pages %d\n", g_lightmapSize);

    /* cleanup */
    for (i = 0; i < numIslands; i++)
        if (islandArr[i].mask) free(islandArr[i].mask);
    free(islandArr);
    free(islandOfSurf);
    free(labels);
    free(g_adaptSurfs);
    g_adaptSurfs = NULL;
}

#if 0 /* DEAD CODE — old approach */
    { int *islandRoots = (int *)malloc(g_adaptSurfCount * sizeof(int));
        for (i = 0; i < g_adaptSurfCount; i++)
        {
            if (g_adaptSurfs[i].groupId == i)
                islandRoots[numIslands++] = i;
        }

        /* rasterize each island's filled UV triangles into a bitmask */
        islandPack = (PackIsland_t *)calloc(numIslands, sizeof(PackIsland_t));
        {
            extern unsigned short bspDrawIndexes[];
            DrawVert_t *allVerts = (DrawVert_t *)bspDrawVerts;

            for (i = 0; i < numIslands; i++)
            {
                int r = islandRoots[i];
                int w = g_adaptSurfs[r].origWidth;
                int h = g_adaptSurfs[r].origHeight;
                float minS = floorf(g_adaptSurfs[r].lmMins[0] * 512.0f - 0.5f);
                float minT = floorf(g_adaptSurfs[r].lmMins[1] * 512.0f - 0.5f);
                unsigned char *mask;
                int si;

                if (w > 510) w = 510;
                if (h > 510) h = 510;
                if (w < 1) w = 1;
                if (h < 1) h = 1;

                mask = (unsigned char *)calloc(w * h, 1);

                /* rasterize filled triangles for all surfaces in this island */
                for (si = 0; si < g_adaptSurfCount; si++)
                {
                    int t, numTris;
                    if (g_adaptSurfs[si].groupId != r) continue;

                    numTris = g_adaptSurfs[si].indexCount / 3;
                    for (t = 0; t < numTris; t++)
                    {
                        int idx = g_adaptSurfs[si].firstIndex + t * 3;
                        float x0 = allVerts[bspDrawIndexes[idx+0]].lmCoord[0]*512.0f - minS;
                        float y0 = allVerts[bspDrawIndexes[idx+0]].lmCoord[1]*512.0f - minT;
                        float x1 = allVerts[bspDrawIndexes[idx+1]].lmCoord[0]*512.0f - minS;
                        float y1 = allVerts[bspDrawIndexes[idx+1]].lmCoord[1]*512.0f - minT;
                        float x2 = allVerts[bspDrawIndexes[idx+2]].lmCoord[0]*512.0f - minS;
                        float y2 = allVerts[bspDrawIndexes[idx+2]].lmCoord[1]*512.0f - minT;
                        int scanMinY = (int)floorf(y0<y1?(y0<y2?y0:y2):(y1<y2?y1:y2));
                        int scanMaxY = (int)ceilf(y0>y1?(y0>y2?y0:y2):(y1>y2?y1:y2));
                        int scanY;
                        if (scanMinY < 0) scanMinY = 0;
                        if (scanMaxY >= h) scanMaxY = h - 1;

                        for (scanY = scanMinY; scanY <= scanMaxY; scanY++)
                        {
                            float fy = (float)scanY + 0.5f;
                            float xI[6]; int nX = 0;
                            float ex[3][2] = {{x0,y0},{x1,y1},{x2,y2}};
                            int e;
                            for (e = 0; e < 3; e++)
                            {
                                float ay = ex[e][1], by = ex[(e+1)%3][1];
                                if ((ay <= fy && by > fy) || (by <= fy && ay > fy))
                                {
                                    float tt = (fy - ay) / (by - ay);
                                    xI[nX++] = ex[e][0] + tt * (ex[(e+1)%3][0] - ex[e][0]);
                                }
                            }
                            if (nX >= 2)
                            {
                                int xMin, xMax, fx;
                                if (xI[0] > xI[1]) { float tmp=xI[0]; xI[0]=xI[1]; xI[1]=tmp; }
                                xMin = (int)floorf(xI[0]);
                                xMax = (int)ceilf(xI[1]);
                                if (xMin < 0) xMin = 0;
                                if (xMax >= w) xMax = w - 1;
                                for (fx = xMin; fx <= xMax; fx++)
                                    mask[scanY * w + fx] = 1;
                            }
                        }
                    }
                }

                islandPack[i].width = w;
                islandPack[i].height = h;
                islandPack[i].mask = mask;
            }
        }

        for (i = 0; i < numIslands; i++)
        {
            if (islandPack[i].width > 200 || islandPack[i].height > 200)
                Com_Printf("  island %d (root %d): %dx%d texels, %d surfaces\n",
                    i, islandRoots[i], islandPack[i].width, islandPack[i].height,
                    /* count members */ 0);
        }

        BitmapPacker_Init();
        numPages = BitmapPacker_Pack(islandPack, numIslands);
        Com_Printf("Adaptive: packed %d islands into %d pages\n", numIslands, numPages);

        /* apply packing results to all surfaces in each island */
        {
            int packed = 0, failed = 0;
            for (i = 0; i < numIslands; i++)
            {
                int r = islandRoots[i];
                int si;

                if (!islandPack[i].packed)
                {
                    failed++;
                    continue;
                }
                packed++;

                /* set root */
                g_adaptSurfs[r].newLightmapIdx = islandPack[i].pageOut;
                g_adaptSurfs[r].newOffsetS = islandPack[i].xOut;
                g_adaptSurfs[r].newOffsetT = islandPack[i].yOut;
                g_adaptSurfs[r].newWidth = g_adaptSurfs[r].origWidth;
                g_adaptSurfs[r].newHeight = g_adaptSurfs[r].origHeight;

                /* propagate to all members */
                for (si = 0; si < g_adaptSurfCount; si++)
                {
                    if (g_adaptSurfs[si].groupId == r && si != r)
                    {
                        g_adaptSurfs[si].newLightmapIdx = islandPack[i].pageOut;
                        g_adaptSurfs[si].newOffsetS = islandPack[i].xOut;
                        g_adaptSurfs[si].newOffsetT = islandPack[i].yOut;
                        g_adaptSurfs[si].newWidth = g_adaptSurfs[r].origWidth;
                        g_adaptSurfs[si].newHeight = g_adaptSurfs[r].origHeight;
                    }
                }
            }
            Com_Printf("Adaptive: %d islands packed, %d failed\n", packed, failed);
        }

        BitmapPacker_Free();
        for (i = 0; i < numIslands; i++)
            if (islandPack[i].mask) free(islandPack[i].mask);
        free(islandPack);
        free(islandRoots);
    }

    /* rewrite UVs — process each vertex ONCE to avoid double-transform */
    {
        DrawVert_t *allVerts = (DrawVert_t *)bspDrawVerts;
        extern int g_triCount;
        extern int numBSPDrawVerts;
        extern Triangle_t g_triangles[];
        char *vertDone = (char *)calloc(numBSPDrawVerts, 1);

        for (i = 0; i < g_adaptSurfCount; i++)
        {
            AdaptiveSurfInfo_t *s = &g_adaptSurfs[i];
            double origMinS, origMinT, origW, origH;
            int v;

            {
                int r = s->groupId;
                if (s->newLightmapIdx < 0) continue;

                origMinS = (double)floorf(g_adaptSurfs[r].lmMins[0] * 512.0f - 0.5f);
                origMinT = (double)floorf(g_adaptSurfs[r].lmMins[1] * 512.0f - 0.5f);
                origW = (double)g_adaptSurfs[r].origWidth;
                origH = (double)g_adaptSurfs[r].origHeight;
            if (origW < 1.0) origW = 1.0;
            if (origH < 1.0) origH = 1.0;

            for (v = 0; v < s->vertexCount; v++)
            {
                int vi = s->firstVertex + v;
                DrawVert_t *dv = &allVerts[vi];
                double pixS, pixT;

                if (vi < numBSPDrawVerts && vertDone[vi]) continue;
                if (vi < numBSPDrawVerts) vertDone[vi] = 1;

                pixS = (double)dv->lmCoord[0] * 512.0;
                pixT = (double)dv->lmCoord[1] * 512.0;

                double normS = (pixS - origMinS) / origW;
                double normT = (pixT - origMinT) / origH;

                double newPixS = normS * (double)s->newWidth + (double)s->newOffsetS;
                double newPixT = normT * (double)s->newHeight + (double)s->newOffsetT;

                if (newPixS < 0.0) newPixS = 0.0;
                if (newPixT < 0.0) newPixT = 0.0;
                if (newPixS > 512.0) newPixS = 512.0;
                if (newPixT > 512.0) newPixT = 512.0;

                dv->lmCoord[0] = (float)(newPixS / 512.0);
                dv->lmCoord[1] = (float)(newPixT / 512.0);
            }

            bspTriangles[s->surfIndex].lightmapIndex = (unsigned short)s->newLightmapIdx;
            }
        }

        free(vertDone);

        /* patch g_triangles */
        for (i = 0; i < g_triCount; i++)
        {
            if (g_triangles[i].lightmapIdx == 0x1F) continue;
            {
                int vi = g_triangles[i].vertIndex[0];
                int si;
                for (si = 0; si < g_adaptSurfCount; si++)
                {
                    if (vi >= g_adaptSurfs[si].firstVertex &&
                        vi < g_adaptSurfs[si].firstVertex + g_adaptSurfs[si].vertexCount)
                    {
                        g_triangles[i].lightmapIdx = (unsigned short)g_adaptSurfs[si].newLightmapIdx;
                        break;
                    }
                }
            }
        }
    }

    /* update lightmap size and reallocate if needed */
    if (numPages > g_lightmapSize)
    {
        extern void *g_lightingSamples;
        g_lightmapSize = numPages;
        if (g_lightingSamples)
        {
            free(g_lightingSamples);
            g_lightingSamples = malloc((unsigned long long)g_lightmapSize * 0x800000);
            if (g_lightingSamples)
                memset(g_lightingSamples, 0, (unsigned long long)g_lightmapSize << 23);
        }
    }

    /* export REPACKED UV layout */
    ExportUVLayoutBMP("D:\\cod2rad\\lightmap_analysis\\uv_repacked.bmp", numPages > 0 ? numPages : 1);

    Com_Printf("Adaptive: lightmap pages %d\n", g_lightmapSize);

#endif

#if 0 /* OLD CODE — keeping for reference */
    /* Phase 2: shadow stencil analysis */
    Com_Printf("Analyzing shadow stencils for %d surfaces...\n", g_adaptSurfCount);
    for (i = 0; i < g_adaptSurfCount; i++)
        g_adaptSurfs[i].shadowScore = Adaptive_AnalyzeSurface(&g_adaptSurfs[i]);

    /* compute 95th percentile score for normalization */
    scores = (float *)malloc(g_adaptSurfCount * sizeof(float));
    for (i = 0; i < g_adaptSurfCount; i++)
        scores[i] = g_adaptSurfs[i].shadowScore;
    qsort(scores, g_adaptSurfCount, sizeof(float), CompareFloatDesc);
    p95Score = scores[(int)(g_adaptSurfCount * 0.05f)];
    if (p95Score < 0.01f) p95Score = 0.01f;
    free(scores);

    {
        float totalArea = 0;
        int maxW = 0, maxH = 0;
        float maxScore = 0, minScore = 1e30f;
        int zeroScores = 0;
        for (i = 0; i < g_adaptSurfCount; i++)
        {
            if (g_adaptSurfs[i].shadowScore > maxScore) maxScore = g_adaptSurfs[i].shadowScore;
            if (g_adaptSurfs[i].shadowScore < minScore) minScore = g_adaptSurfs[i].shadowScore;
            if (g_adaptSurfs[i].shadowScore == 0.0f) zeroScores++;
        }
        Com_Printf("Adaptive: scores — min=%.3f max=%.3f p95=%.3f zero=%d/%d\n",
                    minScore, maxScore, p95Score, zeroScores, g_adaptSurfCount);
    }

    /* Phase 3: resolution assignment
     * adaptiveMin/Max are world units per texel.
     * Lower number = higher resolution.
     * Score 0 (no shadows) → adaptiveMin (low res)
     * Score 1 (complex shadows) → adaptiveMax (high res)
     * Note: adaptiveMin >= adaptiveMax in world-units-per-texel
     * because min quality = MORE world units per texel */
    {
        float wuptLow = g_adaptiveMax;   /* world units per texel for score 0 (low res) */
        float wuptHigh = g_adaptiveMin;  /* world units per texel for score 1 (high res) */

        if (wuptLow < wuptHigh) { float tmp = wuptLow; wuptLow = wuptHigh; wuptHigh = tmp; }
        if (wuptHigh < 1.0f) wuptHigh = 1.0f;
        if (wuptLow < 1.0f) wuptLow = 1.0f;

        /* propagate max shadow score to island roots */
        for (i = 0; i < g_adaptSurfCount; i++)
        {
            int r = g_adaptSurfs[i].groupId;
            if (r != i && g_adaptSurfs[i].shadowScore > g_adaptSurfs[r].shadowScore)
                g_adaptSurfs[r].shadowScore = g_adaptSurfs[i].shadowScore;
        }

        /* assign resolution to island roots only */
        for (i = 0; i < g_adaptSurfCount; i++)
        {
            AdaptiveSurfInfo_t *s = &g_adaptSurfs[i];
            float normalized, wupt;

            if (s->groupId != i) continue;

            normalized = s->shadowScore / p95Score;
            if (normalized > 1.0f) normalized = 1.0f;
            wupt = wuptLow + (wuptHigh - wuptLow) * powf(normalized, 1.0f - g_adaptiveBias);

            s->newWidth = (int)ceilf(s->worldWidth / wupt);
            s->newHeight = (int)ceilf(s->worldHeight / wupt);
            if (s->newWidth < 4) s->newWidth = 4;
            if (s->newHeight < 4) s->newHeight = 4;
            if (s->newWidth > ADAPT_LM_SIZE) s->newWidth = ADAPT_LM_SIZE;
            if (s->newHeight > ADAPT_LM_SIZE) s->newHeight = ADAPT_LM_SIZE;
            s->newWidth = (s->newWidth + 1) & ~1;
            s->newHeight = (s->newHeight + 1) & ~1;
        }

        /* copy root's resolution to all island members */
        for (i = 0; i < g_adaptSurfCount; i++)
        {
            int r = g_adaptSurfs[i].groupId;
            if (r != i)
            {
                g_adaptSurfs[i].newWidth = g_adaptSurfs[r].newWidth;
                g_adaptSurfs[i].newHeight = g_adaptSurfs[r].newHeight;
            }
        }
    }

    {
        long long totalTexels = 0;
        int maxNewW = 0, maxNewH = 0;
        for (i = 0; i < g_adaptSurfCount; i++)
        {
            totalTexels += g_adaptSurfs[i].newWidth * g_adaptSurfs[i].newHeight;
            if (g_adaptSurfs[i].newWidth > maxNewW) maxNewW = g_adaptSurfs[i].newWidth;
            if (g_adaptSurfs[i].newHeight > maxNewH) maxNewH = g_adaptSurfs[i].newHeight;
            if (i < 5)
                Com_Printf("  surf[%d] world=%.0fx%.0f orig=%dx%d new=%dx%d score=%.3f\n",
                    i, g_adaptSurfs[i].worldWidth, g_adaptSurfs[i].worldHeight,
                    g_adaptSurfs[i].origWidth, g_adaptSurfs[i].origHeight,
                    g_adaptSurfs[i].newWidth, g_adaptSurfs[i].newHeight,
                    g_adaptSurfs[i].shadowScore);
        }
        Com_Printf("Adaptive: total demand = %lld texels (%.1f pages), max surface = %dx%d\n",
                    totalTexels, (float)totalTexels / (512.0f * 512.0f), maxNewW, maxNewH);
    }

    /* page budget constraint — scale down if total exceeds 31 pages */
    {
        long long totalTexels = 0;
        long long maxTexels = (long long)ADAPT_MAX_PAGES * ADAPT_LM_SIZE * ADAPT_LM_SIZE;
        long long usableTexels = (long long)(maxTexels * 0.75f); /* 75% packing efficiency */

        for (i = 0; i < g_adaptSurfCount; i++)
        {
            if (g_adaptSurfs[i].groupId != i) continue; /* only count island roots */
            totalTexels += (long long)(g_adaptSurfs[i].newWidth + ADAPT_PADDING)
                         * (g_adaptSurfs[i].newHeight + ADAPT_PADDING);
        }

        if (totalTexels > usableTexels)
        {
            float shrink = sqrtf((float)usableTexels / (float)totalTexels);
            Com_Printf("Adaptive: demand %lld > budget %lld, shrinking by %.2f\n",
                        totalTexels, usableTexels, shrink);
            for (i = 0; i < g_adaptSurfCount; i++)
            {
                if (g_adaptSurfs[i].groupId != i) continue;
                g_adaptSurfs[i].newWidth = (int)(g_adaptSurfs[i].newWidth * shrink);
                g_adaptSurfs[i].newHeight = (int)(g_adaptSurfs[i].newHeight * shrink);
                if (g_adaptSurfs[i].newWidth < 2) g_adaptSurfs[i].newWidth = 2;
                if (g_adaptSurfs[i].newHeight < 2) g_adaptSurfs[i].newHeight = 2;
                g_adaptSurfs[i].newWidth = (g_adaptSurfs[i].newWidth + 1) & ~1;
                g_adaptSurfs[i].newHeight = (g_adaptSurfs[i].newHeight + 1) & ~1;
            }
            /* re-propagate shrunk sizes to island members */
            for (i = 0; i < g_adaptSurfCount; i++)
            {
                int r = g_adaptSurfs[i].groupId;
                if (r != i)
                {
                    g_adaptSurfs[i].newWidth = g_adaptSurfs[r].newWidth;
                    g_adaptSurfs[i].newHeight = g_adaptSurfs[r].newHeight;
                }
            }
        }
    }

    /* Phase 4: UV repacking — sort index array by area, pack island roots largest first */
    {
        int *packOrder = (int *)malloc(g_adaptSurfCount * sizeof(int));
        for (i = 0; i < g_adaptSurfCount; i++)
            packOrder[i] = i;
        qsort(packOrder, g_adaptSurfCount, sizeof(int), CompareAdaptSurfByArea);

    g_numAdaptPages = 0;
    memset(g_freeBlocks, 0, sizeof(g_freeBlocks));

    {
        int packed = 0, failed = 0;
        for (i = 0; i < g_adaptSurfCount; i++)
        {
            int si = packOrder[i];
            AdaptiveSurfInfo_t *s = &g_adaptSurfs[si];
            int page, offS, offT;

            if (s->groupId != si) continue; /* not an island root */

            if (Adaptive_FindAndAlloc(s->newWidth, s->newHeight, &page, &offS, &offT))
            {
                s->newLightmapIdx = page;
                s->newOffsetS = offS;
                s->newOffsetT = offT;
                packed++;
            }
            else
            {
                s->newLightmapIdx = 0;
                s->newOffsetS = 0;
                s->newOffsetT = 0;
                failed++;
            }
        }

        /* propagate root's placement to all island members */
        for (i = 0; i < g_adaptSurfCount; i++)
        {
            int r = g_adaptSurfs[i].groupId;
            if (r != i)
            {
                g_adaptSurfs[i].newLightmapIdx = g_adaptSurfs[r].newLightmapIdx;
                g_adaptSurfs[i].newOffsetS = g_adaptSurfs[r].newOffsetS;
                g_adaptSurfs[i].newOffsetT = g_adaptSurfs[r].newOffsetT;
                g_adaptSurfs[i].newWidth = g_adaptSurfs[r].newWidth;
                g_adaptSurfs[i].newHeight = g_adaptSurfs[r].newHeight;
            }
        }

        Com_Printf("Adaptive: packed %d surfaces into %d pages (%d failed)\n", packed, g_numAdaptPages, failed);
    }

    free(packOrder);
    }

    /* free packer blocks */
    for (i = 0; i < ADAPT_MAX_PAGES; i++)
    {
        LmBlock_t *blk = g_freeBlocks[i];
        while (blk) { LmBlock_t *next = blk->next; free(blk); blk = next; }
        g_freeBlocks[i] = NULL;
    }

    /* Phase 5: rewrite UVs and update lightmap size */
    Adaptive_RewriteUVs();

    if (g_numAdaptPages > g_lightmapSize)
    {
        extern void *g_lightingSamples;
        extern int g_lightSourceCount;
        g_lightmapSize = g_numAdaptPages;

        if (g_lightingSamples)
        {
            free(g_lightingSamples);
            g_lightingSamples = malloc((unsigned long long)g_lightmapSize * 0x800000);
            if (g_lightingSamples)
                memset(g_lightingSamples, 0, (unsigned long long)g_lightmapSize << 23);
        }
    }

    /* validate UV bounds */
    {
        DrawVert_t *allVerts = (DrawVert_t *)bspDrawVerts;
        int badCount = 0;
        for (i = 0; i < numBSPDrawVerts; i++)
        {
            if (allVerts[i].lmCoord[0] < -0.01f || allVerts[i].lmCoord[0] > 1.01f ||
                allVerts[i].lmCoord[1] < -0.01f || allVerts[i].lmCoord[1] > 1.01f)
            {
                if (badCount < 5)
                    Com_Printf("  BAD UV: vert %d = (%.4f, %.4f)\n", i, allVerts[i].lmCoord[0], allVerts[i].lmCoord[1]);
                badCount++;
            }
        }
        if (badCount)
            Com_Printf("Adaptive: %d vertices with out-of-range UVs!\n", badCount);
    }

    /* count why triangles are rejected */
    {
        extern int g_triCount;
        extern Triangle_t g_triangles[];
        extern int ComputeLinearMappingForTriangle(Triangle_t *tri, void *outMapping);
        int noLm = 0, dupUV = 0, mappingFail = 0, ok = 0;
        char dummyMapping[160];
        for (i = 0; i < g_triCount; i++)
        {
            Triangle_t *tri = &g_triangles[i];
            float *lm0, *lm1, *lm2;
            if (tri->lightmapIdx == 0x1F) { noLm++; continue; }
            lm0 = ((DrawVert_t *)bspDrawVerts)[tri->vertIndex[0]].lmCoord;
            lm1 = ((DrawVert_t *)bspDrawVerts)[tri->vertIndex[1]].lmCoord;
            lm2 = ((DrawVert_t *)bspDrawVerts)[tri->vertIndex[2]].lmCoord;
            if ((lm0[0]==lm1[0] && lm0[1]==lm1[1]) ||
                (lm1[0]==lm2[0] && lm1[1]==lm2[1]) ||
                (lm2[0]==lm0[0] && lm2[1]==lm0[1])) { dupUV++; continue; }
            if (!ComputeLinearMappingForTriangle(tri, dummyMapping)) { mappingFail++; continue; }
            ok++;
        }
        Com_Printf("Adaptive: tri rejection — noLm=%d dupUV=%d mappingFail=%d ok=%d\n",
                    noLm, dupUV, mappingFail, ok);
    }

    /* verify triangle patching */
    {
        extern int g_triCount;
        extern Triangle_t g_triangles[];
        int unpatched = 0, patched = 0;
        for (i = 0; i < g_triCount; i++)
        {
            if (g_triangles[i].lightmapIdx != 0x1F)
            {
                if (g_triangles[i].lightmapIdx < g_numAdaptPages)
                    patched++;
                else
                    unpatched++;
            }
        }
        Com_Printf("Adaptive: triangles — %d patched, %d unpatched (bad idx)\n", patched, unpatched);
    }

    Com_Printf("Adaptive: lightmap pages %d\n", g_lightmapSize);

    free(g_adaptSurfs);
    g_adaptSurfs = NULL;
}
#endif /* OLD CODE */

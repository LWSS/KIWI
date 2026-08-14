/*
 * water_volume.c — Water volume detection and light absorption.
 *
 * Finds brushes matching a user-specified material (e.g. clip_water),
 * tests if lighting sample points are inside those volumes, and
 * attenuates light using Beer's law based on depth below water surface.
 *
 * BSP brush side format (from cod2map EmitBrushes):
 *   First 6 sides are axial AABB planes — stored as dist, not planeNum.
 *   Even axial sides (0,2,4) store -dist, odd (1,3,5) store +dist.
 *   Sides 6+ store planeNum indexing into bspPlanes[].
 */

#include "cod2rad64.h"
#include <math.h>
#include <string.h>

#define BRUSH_AXIAL_SIDES 6

extern int numBSPMaterials;
extern Dmaterial_t bspMaterials[];
extern int numBSPBrushes;
extern BspBrush_t bspBrushes[];
extern int numBSPBrushSides;
extern BspBrushSide_t bspBrushSidesData[];
extern int numBSPPlanes;
extern BspPlane_disk_t bspPlanes[];

typedef struct WaterBrush_s {
    float mins[3];
    float maxs[3];
    int firstSide;
    int numSides;
} WaterBrush_t;

static WaterBrush_t *g_waterBrushes;
int g_waterBrushCount;

char g_waterMaterial[64];
float g_waterAbsorption[3] = { 0.0f, 0.0f, 0.0f };
float g_waterColor[3] = { 0.0f, 0.05f, 0.12f };

void InitWaterVolumes(void)
{
    int i, sideOffset;
    int matIdx = -1;

    g_waterBrushCount = 0;
    g_waterBrushes = NULL;

    if (!g_waterMaterial[0] || (g_waterAbsorption[0] == 0.0f && g_waterAbsorption[1] == 0.0f && g_waterAbsorption[2] == 0.0f))
        return;

    for (i = 0; i < numBSPMaterials; i++)
    {
        if (_stricmp(bspMaterials[i].material, g_waterMaterial) == 0)
        {
            matIdx = i;
            break;
        }
    }

    if (matIdx < 0)
    {
        Com_Printf("Water material '%s' not found in BSP.\n", g_waterMaterial);
        return;
    }

    sideOffset = 0;
    for (i = 0; i < numBSPBrushes; i++)
    {
        if (bspBrushes[i].shaderNum == matIdx)
            g_waterBrushCount++;
        sideOffset += bspBrushes[i].numSides;
    }

    if (g_waterBrushCount == 0)
    {
        Com_Printf("No brushes found with material '%s'.\n", g_waterMaterial);
        return;
    }

    g_waterBrushes = (WaterBrush_t *)malloc(g_waterBrushCount * sizeof(WaterBrush_t));

    sideOffset = 0;
    {
        int idx = 0;
        for (i = 0; i < numBSPBrushes; i++)
        {
            if (bspBrushes[i].shaderNum == matIdx)
            {
                WaterBrush_t *wb = &g_waterBrushes[idx];
                wb->firstSide = sideOffset;
                wb->numSides = bspBrushes[i].numSides;

                /* extract AABB from first 6 axial sides */
                wb->mins[0] = bspBrushSidesData[sideOffset + 0].dist;
                wb->maxs[0] = bspBrushSidesData[sideOffset + 1].dist;
                wb->mins[1] = bspBrushSidesData[sideOffset + 2].dist;
                wb->maxs[1] = bspBrushSidesData[sideOffset + 3].dist;
                wb->mins[2] = bspBrushSidesData[sideOffset + 4].dist;
                wb->maxs[2] = bspBrushSidesData[sideOffset + 5].dist;

                idx++;
            }
            sideOffset += bspBrushes[i].numSides;
        }
    }

    Com_Printf("Found %d water volume brushes (material: %s)\n", g_waterBrushCount, g_waterMaterial);
}

static int PointInsideWaterBrush(WaterBrush_t *wb, float *point)
{
    int j;

    /* quick AABB reject */
    if (point[0] < wb->mins[0] || point[0] > wb->maxs[0]) return 0;
    if (point[1] < wb->mins[1] || point[1] > wb->maxs[1]) return 0;
    if (point[2] < wb->mins[2] || point[2] > wb->maxs[2]) return 0;

    /* test non-axial planes (sides 6+) */
    for (j = BRUSH_AXIAL_SIDES; j < wb->numSides; j++)
    {
        int planeNum = bspBrushSidesData[wb->firstSide + j].planeNum;
        if (planeNum < 0 || planeNum >= numBSPPlanes) return 0;

        float *normal = bspPlanes[planeNum].normal;
        float dist = bspPlanes[planeNum].dist;
        float d = point[0]*normal[0] + point[1]*normal[1] + point[2]*normal[2] - dist;

        if (d > 0.0f)
            return 0;
    }

    return 1;
}

int GetWaterBrushPlaneCount(int idx)
{
    if (idx < 0 || idx >= g_waterBrushCount) return 0;
    WaterBrush_t *wb = &g_waterBrushes[idx];
    int count = 6; /* 6 axial planes from AABB */
    count += (wb->numSides > BRUSH_AXIAL_SIDES) ? (wb->numSides - BRUSH_AXIAL_SIDES) : 0;
    return count;
}

void GetWaterBrushPlane(int idx, int planeIdx, float *normal, float *dist)
{
    if (idx < 0 || idx >= g_waterBrushCount) return;
    WaterBrush_t *wb = &g_waterBrushes[idx];

    if (planeIdx < 6)
    {
        /* axial planes from AABB: +X, -X, +Y, -Y, +Z, -Z */
        int axis = planeIdx / 2;
        int sign = planeIdx & 1;
        normal[0] = normal[1] = normal[2] = 0.0f;
        if (sign == 0) { normal[axis] = -1.0f; *dist = -wb->mins[axis]; }
        else           { normal[axis] =  1.0f; *dist =  wb->maxs[axis]; }
    }
    else
    {
        /* non-axial planes from brush sides */
        int sideIdx = wb->firstSide + BRUSH_AXIAL_SIDES + (planeIdx - 6);
        if (sideIdx < numBSPBrushSides)
        {
            int planeNum = bspBrushSidesData[sideIdx].planeNum;
            if (planeNum >= 0 && planeNum < numBSPPlanes)
            {
                normal[0] = bspPlanes[planeNum].normal[0];
                normal[1] = bspPlanes[planeNum].normal[1];
                normal[2] = bspPlanes[planeNum].normal[2];
                *dist = bspPlanes[planeNum].dist;
            }
        }
    }
}

void GetWaterBrushAABB(int idx, float *mins, float *maxs)
{
    if (idx < 0 || idx >= g_waterBrushCount) return;
    mins[0] = g_waterBrushes[idx].mins[0];
    mins[1] = g_waterBrushes[idx].mins[1];
    mins[2] = g_waterBrushes[idx].mins[2];
    maxs[0] = g_waterBrushes[idx].maxs[0];
    maxs[1] = g_waterBrushes[idx].maxs[1];
    maxs[2] = g_waterBrushes[idx].maxs[2];
}

float GetWaterDepth(float *point)
{
    int i;

    for (i = 0; i < g_waterBrushCount; i++)
    {
        WaterBrush_t *wb = &g_waterBrushes[i];

        if (PointInsideWaterBrush(wb, point))
        {
            float maxDepth = wb->maxs[2] - point[2];
            int j;
            for (j = BRUSH_AXIAL_SIDES; j < wb->numSides; j++)
            {
                int planeNum = bspBrushSidesData[wb->firstSide + j].planeNum;
                if (planeNum < 0 || planeNum >= numBSPPlanes) continue;
                float *n = bspPlanes[planeNum].normal;
                if (n[2] > 0.5f)
                {
                    float d = n[0]*point[0] + n[1]*point[1] + n[2]*point[2] - bspPlanes[planeNum].dist;
                    float depth = -d / n[2];
                    if (depth < maxDepth) maxDepth = depth;
                }
            }
            return maxDepth > 0.0f ? maxDepth : 0.0f;
        }
    }

    return 0.0f;
}


typedef struct WaterSampleTag_s {
    void *sample;
    float atten[3];
} WaterSampleTag_t;

static WaterSampleTag_t *g_waterSampleTags;
static volatile long g_waterSampleTagCount;
static int g_waterSampleTagCapacity;

void InitWaterSampleTags(int maxSamples)
{
    g_waterSampleTagCapacity = maxSamples;
    g_waterSampleTagCount = 0;
    if (maxSamples > 0 && g_waterBrushCount > 0)
        g_waterSampleTags = (WaterSampleTag_t *)calloc(maxSamples, sizeof(WaterSampleTag_t));
    else
        g_waterSampleTags = NULL;
}

void TagSampleWithWater(void *sample, float *position)
{
    float wa[3], sunEnd[3];
    long i, idx;
    extern float g_sunDirX, g_sunDirY, g_sunDirZ;

    if (!g_waterSampleTags || g_waterBrushCount == 0)
        return;

    /* skip if this sample was already tagged */
    for (i = g_waterSampleTagCount - 1; i >= 0 && i > g_waterSampleTagCount - 64; i--)
    {
        if (g_waterSampleTags[i].sample == sample)
            return;
    }

    sunEnd[0] = g_sunDirX * 262144.0f + position[0];
    sunEnd[1] = g_sunDirY * 262144.0f + position[1];
    sunEnd[2] = g_sunDirZ * 262144.0f + position[2];
    ComputeWaterAttenuation(position, sunEnd, wa);

    if (wa[0] >= 1.0f && wa[1] >= 1.0f && wa[2] >= 1.0f)
        return;

    idx = InterlockedIncrement(&g_waterSampleTagCount) - 1;
    if (idx >= g_waterSampleTagCapacity)
        return;

    g_waterSampleTags[idx].sample = sample;
    g_waterSampleTags[idx].atten[0] = wa[0];
    g_waterSampleTags[idx].atten[1] = wa[1];
    g_waterSampleTags[idx].atten[2] = wa[2];
}

void ApplyWaterSampleTags(void)
{
    long i;
    int applied = 0;

    if (!g_waterSampleTags)
        return;

    for (i = 0; i < g_waterSampleTagCount; i++)
    {
        WaterSampleTag_t *tag = &g_waterSampleTags[i];
        SampleVars_t *vars = ((Sample_t *)tag->sample)->vars;
        if (!vars) continue;

        vars->unscattered[0] *= tag->atten[0];
        vars->unscattered[1] *= tag->atten[1];
        vars->unscattered[2] *= tag->atten[2];
        vars->scattered[0] *= tag->atten[0];
        vars->scattered[1] *= tag->atten[1];
        vars->scattered[2] *= tag->atten[2];
        { int b; for (b = 0; b < 12; b++) {
            int ch = b % 3;
            float t = tag->atten[ch];
            vars->incident[b] = vars->incident[b] * t + (1.0f - t) * g_waterColor[ch];
        }}
        applied++;
    }

    if (applied > 0)
        Com_Printf("Water shadow applied to %d samples\n", applied);

    free(g_waterSampleTags);
    g_waterSampleTags = NULL;
}

void ApplyWaterAbsorptionToColor(float *color, float depth)
{
    color[0] *= expf(-g_waterAbsorption[0] * depth);
    color[1] *= expf(-g_waterAbsorption[1] * depth);
    color[2] *= expf(-g_waterAbsorption[2] * depth);
}

void ComputeWaterAttenuation(float *start, float *end, float *atten)
{
    float dx, dy, dz, rayLen;
    float totalWaterDist = 0.0f;
    int i;

    atten[0] = atten[1] = atten[2] = 1.0f;

    if (g_waterBrushCount == 0)
        return;

    dx = end[0] - start[0];
    dy = end[1] - start[1];
    dz = end[2] - start[2];
    rayLen = sqrtf(dx*dx + dy*dy + dz*dz);

    if (rayLen < 0.001f)
        return;

    for (i = 0; i < g_waterBrushCount; i++)
    {
        WaterBrush_t *wb = &g_waterBrushes[i];
        float tEntry = 0.0f;
        float tExit = 1.0f;
        int miss = 0;
        int j;

        /* test axial planes from AABB */
        {
            float invDx = (dx != 0.0f) ? 1.0f / dx : 1e30f;
            float invDy = (dy != 0.0f) ? 1.0f / dy : 1e30f;
            float invDz = (dz != 0.0f) ? 1.0f / dz : 1e30f;

            float t1, t2;

            t1 = (wb->mins[0] - start[0]) * invDx;
            t2 = (wb->maxs[0] - start[0]) * invDx;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tEntry) tEntry = t1;
            if (t2 < tExit) tExit = t2;
            if (tEntry > tExit) continue;

            t1 = (wb->mins[1] - start[1]) * invDy;
            t2 = (wb->maxs[1] - start[1]) * invDy;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tEntry) tEntry = t1;
            if (t2 < tExit) tExit = t2;
            if (tEntry > tExit) continue;

            t1 = (wb->mins[2] - start[2]) * invDz;
            t2 = (wb->maxs[2] - start[2]) * invDz;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tEntry) tEntry = t1;
            if (t2 < tExit) tExit = t2;
            if (tEntry > tExit) continue;
        }

        /* test non-axial planes */
        for (j = BRUSH_AXIAL_SIDES; j < wb->numSides; j++)
        {
            int planeNum = bspBrushSidesData[wb->firstSide + j].planeNum;
            float *normal, dist, startDot, dirDot, t;

            if (planeNum < 0 || planeNum >= numBSPPlanes) { miss = 1; break; }

            normal = bspPlanes[planeNum].normal;
            dist = bspPlanes[planeNum].dist;

            startDot = start[0]*normal[0] + start[1]*normal[1] + start[2]*normal[2] - dist;
            dirDot = dx*normal[0] + dy*normal[1] + dz*normal[2];

            if (dirDot == 0.0f)
            {
                if (startDot > 0.0f) { miss = 1; break; }
                continue;
            }

            t = -startDot / dirDot;

            if (dirDot > 0.0f)
            {
                if (t < tExit) tExit = t;
            }
            else
            {
                if (t > tEntry) tEntry = t;
            }

            if (tEntry > tExit) { miss = 1; break; }
        }

        if (!miss && tEntry < tExit)
        {
            if (tEntry < 0.0f) tEntry = 0.0f;
            if (tExit > 1.0f) tExit = 1.0f;
            if (tEntry < tExit)
                totalWaterDist += (tExit - tEntry) * rayLen;
        }
    }

    if (totalWaterDist > 0.0f)
    {
        atten[0] = expf(-g_waterAbsorption[0] * totalWaterDist);
        atten[1] = expf(-g_waterAbsorption[1] * totalWaterDist);
        atten[2] = expf(-g_waterAbsorption[2] * totalWaterDist);
    }
}

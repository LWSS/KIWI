/*
 * poly2d.c — 2D polygon operations: area, centroid, splitting.
 */

#include "cod2rad64.h"

extern void *memcpy(void *dst, const void *src, unsigned __int64 size);

/*
================
AreaX2AndCentroidFor2dPoly

Computes the signed area×2 and centroid of a 2D polygon using the
shoelace formula. Points stored as interleaved float pairs
(x0, y0, x1, y1, ...). Accumulates in double precision.

Returns areaX2. Stores centroid in centroidOut[0..1].
================
*/
__declspec(noinline) float AreaX2AndCentroidFor2dPoly(float *points, int numPoints, float *centroidOut)
{
    int i;
    double areaSum;
    double centroidXSum;
    double centroidYSum;

    areaSum = 0.0;
    centroidXSum = 0.0;
    centroidYSum = 0.0;

    for (i = 0; i < numPoints; i++)
    {
        int ip1 = (i + 1 < numPoints) ? i + 1 : 0;
        int im1 = (i > 0) ? i - 1 : numPoints - 1;

        double xi   = (double)points[i * 2];
        double yi   = (double)points[i * 2 + 1];
        double xip1 = (double)points[ip1 * 2];
        double yip1 = (double)points[ip1 * 2 + 1];
        double xim1 = (double)points[im1 * 2];
        double yim1 = (double)points[im1 * 2 + 1];

        double areaTermA = xi * (yim1 - yip1);
        double areaTermB = yi * (xip1 - xim1);

        areaSum      += areaTermA;
        centroidXSum += (xi + xip1 + xim1) * areaTermB;
        centroidYSum += (yi + yip1 + yim1) * areaTermA;
    }

    centroidOut[0] = (float)(centroidXSum / (3.0 * areaSum));
    centroidOut[1] = (float)(centroidYSum / (3.0 * areaSum));

    return (float)areaSum;
}

static char s_assertDisable_Split_coords;
static char s_assertDisable_Split_vertCount;
static char s_assertDisable_Split_axis;
static char s_assertDisable_Split_coordsFront;
static char s_assertDisable_Split_frontCount;
static char s_assertDisable_Split_coordsBack;
static char s_assertDisable_Split_backCount;
static char s_assertDisable_Split_neqFront;
static char s_assertDisable_Split_neqBack;
static char s_assertDisable_Split_frontNeqBack;

/*
================
Split2dPolyAlongAxis

Splits a 2D polygon along an axis-aligned line. Vertices on the
split line go to both sides. Computes intersection points where
edges cross the split line via linear interpolation.
================
*/
void Split2dPolyAlongAxis(float *coords, int vertCount, int axis, float splitValue,
                          float *coordsFront, int *frontCountOut,
                          float *coordsBack, int *backCountOut)
{
    int sides[MAX_VERTS_PER_POLY];
    int frontCount, backCount;
    int i;
    float epsilon;
    float splitHigh, splitLow;

    Assert(coords, s_assertDisable_Split_coords);
    Assert(vertCount >= 3 && vertCount <= MAX_VERTS_PER_POLY,
           s_assertDisable_Split_vertCount);
    Assert(axis == 0 || axis == 1, s_assertDisable_Split_axis);
    Assert(coordsFront, s_assertDisable_Split_coordsFront);
    Assert(frontCountOut, s_assertDisable_Split_frontCount);
    Assert(coordsBack, s_assertDisable_Split_coordsBack);
    Assert(backCountOut, s_assertDisable_Split_backCount);
    Assert(coords != coordsFront, s_assertDisable_Split_neqFront);
    Assert(coords != coordsBack, s_assertDisable_Split_neqBack);
    Assert(coordsFront != coordsBack, s_assertDisable_Split_frontNeqBack);

    epsilon = 0.001f;
    splitHigh = splitValue + epsilon;
    splitLow = splitValue - epsilon;

    *frontCountOut = 0;
    frontCount = 0;
    backCount = 0;
    *backCountOut = 0;

    for (i = 0; i < vertCount; i++)
    {
        float val = coords[i * 2 + axis];
        if (val > splitHigh)
        {
            sides[i] = 0;
            frontCount++;
        }
        else if (val < splitLow)
        {
            sides[i] = 1;
            backCount++;
        }
        else
        {
            sides[i] = 2;
        }
    }

    if (backCount == 0)
    {
        memcpy(coordsFront, coords, vertCount * 8);
        *frontCountOut = vertCount;
        return;
    }

    if (frontCount == 0)
    {
        memcpy(coordsBack, coords, vertCount * 8);
        *backCountOut = vertCount;
        return;
    }

    {
        int curIdx = vertCount - 1;
        int nextIdx = 0;

        for (i = 0; i < vertCount; i++)
        {
            int curSide = sides[curIdx];
            float *curVert = &coords[curIdx * 2];
            int nextSide = sides[nextIdx];

            if (curSide == 2)
            {
                coordsFront[(*frontCountOut) * 2]     = curVert[0];
                coordsFront[(*frontCountOut) * 2 + 1] = curVert[1];
                (*frontCountOut)++;
                coordsBack[(*backCountOut) * 2]     = curVert[0];
                coordsBack[(*backCountOut) * 2 + 1] = curVert[1];
                (*backCountOut)++;
            }
            else if (curSide == 0)
            {
                coordsFront[(*frontCountOut) * 2]     = curVert[0];
                coordsFront[(*frontCountOut) * 2 + 1] = curVert[1];
                (*frontCountOut)++;
            }
            else if (curSide == 1)
            {
                coordsBack[(*backCountOut) * 2]     = curVert[0];
                coordsBack[(*backCountOut) * 2 + 1] = curVert[1];
                (*backCountOut)++;
            }

            if (nextSide != 2 && nextSide != curSide && curSide != 2)
            {
                float *nextVert = &coords[nextIdx * 2];
                int otherAxis = 1 - axis;
                float t;
                float interpPoint[2];

                t = (splitValue - curVert[axis]) / (nextVert[axis] - curVert[axis]);
                interpPoint[otherAxis] = curVert[otherAxis] + t * (nextVert[otherAxis] - curVert[otherAxis]);
                interpPoint[axis] = splitValue;

                coordsFront[(*frontCountOut) * 2]     = interpPoint[0];
                coordsFront[(*frontCountOut) * 2 + 1] = interpPoint[1];
                (*frontCountOut)++;
                coordsBack[(*backCountOut) * 2]     = interpPoint[0];
                coordsBack[(*backCountOut) * 2 + 1] = interpPoint[1];
                (*backCountOut)++;
            }

            curIdx = nextIdx;
            nextIdx++;
        }
    }

    if (*frontCountOut > MAX_VERTS_PER_POLY || *backCountOut > MAX_VERTS_PER_POLY)
    {
        ErrorMsg("MAX_VERTS_PER_POLY exceeded on 2d poly split\n");
    }
}

/*
 * Polygon slot size: each slot holds up to 8 vertices × 2 floats = 64 bytes,
 * Retail reserves a 112-byte temporary slot and permits ten clipped vertices.
 */

/* helper: get pointer to polygon slot within the buffer */
#define POLY_SLOT(base, idx) ((float *)((unsigned char *)(base) + (idx) * POLY_SLOT_STRIDE))

/*
================
ForEach2dArea

Iterates over a grid of cells (numCellsY rows × gridSizeX columns),
splitting polygons at cell boundaries. For each resulting sub-polygon,
computes area and centroid, then calls the callback.

Uses 4 polygon buffer slots (indices 0-3) within polyArray, rotating
between them during successive splits. Outer loop splits along Y
(axis 1), inner loop splits along X (axis 0).

If a sub-polygon's |area×2| is below minArea threshold, it is skipped.
If below halfArea threshold, the centroid is computed as vertex
average instead of the shoelace centroid.
================
*/
void ForEach2dArea(void *polyArray, int numPolys, int gridSizeX, int numCellsY,
                   float startX, float startY, float cellSizeX, float cellSizeY,
                   ForEach2dAreaCallback callback, void *userData)
{
    float minArea;
    float halfArea;
    int slotA, slotB, slotC, slotD;
    int cellY, cellX;
    int yFrontCount, yBackCount;
    int xFrontCount, xBackCount;
    float splitY, splitX;
    float centroid[2];
    float areaX2;
    double areaAbs;
    int areaIndex;

    minArea = cellSizeX * 2.00000022232416086e-6f * cellSizeY;
    halfArea = cellSizeX * 0.5f * cellSizeY;

    /* slot indices: rotate through 4 slots (0,1,2,3) */
    slotA = 0;  /* esi: initial source */
    slotB = 1;  /* r14d */
    slotC = 2;  /* ebp */
    slotD = 3;  /* r11d */
    areaIndex = 0;

    if (numCellsY <= 0)
        return;

    splitY = startY;

    /* outer loop: Y cells */
    for (cellY = 0; cellY < numCellsY; cellY++)
    {
        int curYVertCount;
        int innerSlotSrc, innerSlotXFront;

        if (numPolys == 0)
            goto end_outer_iter;

        if (cellY == numCellsY - 1)
        {
            /* last Y row: no split, use source directly */
            innerSlotSrc = slotA;
            innerSlotXFront = slotD;
            curYVertCount = numPolys;
        }
        else
        {
            /* split along Y axis (axis=1) */
            splitY += cellSizeY;

            Split2dPolyAlongAxis(
                POLY_SLOT(polyArray, slotA), numPolys, 1, splitY,
                POLY_SLOT(polyArray, slotD), &yFrontCount,
                POLY_SLOT(polyArray, slotB), &yBackCount);

            /* front side becomes source for next Y iteration */
            numPolys = yFrontCount;
            curYVertCount = yBackCount;

            /* 3-way rotate: A←D, D←B, B←A (verified via Frida trace)
               X loop uses: xSrc=old B, xFront=old A */
            {
                int oldA = slotA;
                int oldB = slotB;
                slotA = slotD;
                slotD = slotB;
                slotB = oldA;
                innerSlotSrc = oldB;
                innerSlotXFront = oldA;
            }
        }

        /* inner loop: X cells — use local slot vars to avoid corrupting Y-level slots */
        splitX = startX;
        { int xSrc = innerSlotSrc, xFront = innerSlotXFront, xBack = slotC;
        for (cellX = 0; cellX < gridSizeX; cellX++)
        {
            int curVertCount;
            int polySlotIdx;

            if (curYVertCount == 0)
                goto end_inner_iter;

            if (cellX == gridSizeX - 1)
            {
                /* last X column: no split, use remaining polygon */
                curVertCount = curYVertCount;
                polySlotIdx = xSrc;
            }
            else
            {
                /* split along X axis (axis=0) */
                splitX += cellSizeX;

                Split2dPolyAlongAxis(
                    POLY_SLOT(polyArray, xSrc), curYVertCount, 0, splitX,
                    POLY_SLOT(polyArray, xFront), &xFrontCount,
                    POLY_SLOT(polyArray, xBack), &xBackCount);

                curVertCount = xBackCount;
                curYVertCount = xFrontCount;
                polySlotIdx = xBack;

                /* rotate: front becomes new source */
                {
                    int tmp = xSrc;
                    xSrc = xFront;
                    xFront = tmp;
                }
            }

            /* process sub-polygon if it has enough vertices */
            if (curVertCount >= 3)
            {
                float *polyVerts = POLY_SLOT(polyArray, polySlotIdx);

                areaX2 = AreaX2AndCentroidFor2dPoly(polyVerts, curVertCount, centroid);

                /* absolute value via double precision */
                areaAbs = (double)areaX2;
                if (areaAbs < 0.0)
                    areaAbs = -areaAbs;
                areaX2 = (float)areaAbs;

                /* skip if area too small */
                if (areaX2 < minArea)
                    goto end_inner_iter;

                /* if area below half threshold, use vertex average centroid */
                if (areaX2 < halfArea)
                {
                    float sumX = 0.0f;
                    float sumY = 0.0f;
                    float invCount;
                    int v;

                    for (v = 0; v < curVertCount; v++)
                    {
                        sumX += polyVerts[v * 2];
                        sumY += polyVerts[v * 2 + 1];
                    }
                    invCount = 1.0f / (float)curVertCount;
                    centroid[0] = sumX * invCount;
                    centroid[1] = sumY * invCount;
                }

                /* call callback with areaX2 as xmm0 (binary's contract: geometry_40EAB0 reads it from xmm0) */
                callback(areaX2, centroid, polyVerts, curVertCount,
                         userData, areaIndex++);
            }

        end_inner_iter:
            ;
        }
        }

    end_outer_iter:
        ;
    }
}

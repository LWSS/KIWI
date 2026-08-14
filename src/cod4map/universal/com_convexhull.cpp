/*
com_convexhull.cpp - Native CoD4 2D convex-hull helpers.
*/

#include "cod4map.h"

static void Com_TranslatePoints(float points[64][2], unsigned int pointCount,
                                const float *offset);
static void Com_SwapHullPoints(unsigned int *pointOrder,
                               unsigned int pointIndex0,
                               unsigned int pointIndex1);
static void Com_InitialHull(const float points[64][2],
                            unsigned int *pointOrder,
                            unsigned int pointCount,
                            unsigned int *hullOrder);
static unsigned int Com_GrowInitialHull(const float points[64][2],
                                        unsigned int *pointOrder,
                                        unsigned int pointCount,
                                        unsigned int *hullOrder);
static unsigned int Com_AddPointToHull(unsigned int pointIndex,
                                       unsigned int newIndex,
                                       unsigned int *hullOrder,
                                       unsigned int hullPointCount);
static unsigned int Com_RecursivelyGrowHull(const float points[64][2],
                                             unsigned int *pointOrder,
                                             unsigned int pointCount,
                                             unsigned int firstIndex,
                                             unsigned int secondIndex,
                                             unsigned int *hullOrder,
                                             unsigned int hullPointCount);

/* CoD4 0x466AE0. */
unsigned int Com_ConvexHull(float (*points)[2], unsigned int pointCount,
                            float (*hull)[2])
{
  unsigned int pointOrder[64];
  unsigned int hullOrder[64];
  unsigned int hullPointCount;
  unsigned int pointIndex;
  float offset[2];

  Assert(pointCount >= 3 && pointCount <= 64, g_assertFlags4);
  Assert(hull != points, g_assertFlags4);
  Assert(hull >= points + pointCount || points >= hull + pointCount,
         g_assertFlags4);

  offset[0] = -points[0][0];
  offset[1] = -points[0][1];
  Com_TranslatePoints((float (*)[2])points, pointCount, offset);
  Com_InitialHull((const float (*)[2])points, pointOrder, pointCount,
                  hullOrder);
  hullPointCount = Com_GrowInitialHull((const float (*)[2])points,
                                        pointOrder, pointCount - 2,
                                        hullOrder);

  for (pointIndex = 0; pointIndex < hullPointCount; ++pointIndex)
  {
    hull[pointIndex][0] = points[hullOrder[pointIndex]][0] - offset[0];
    hull[pointIndex][1] = points[hullOrder[pointIndex]][1] - offset[1];
  }
  return hullPointCount;
}

/* CoD4 0x466C40. */
static void Com_InitialHull(const float points[64][2],
                            unsigned int *pointOrder,
                            unsigned int pointCount,
                            unsigned int *hullOrder)
{
  unsigned int pointIndex;
  unsigned int minIndex;
  unsigned int maxIndex;

  minIndex = 0;
  maxIndex = 0;
  pointOrder[0] = 0;
  for (pointIndex = 1; pointIndex < pointCount; ++pointIndex)
  {
    pointOrder[pointIndex] = pointIndex;
    if (points[pointIndex][1] < points[maxIndex][1])
    {
      if (points[minIndex][1] > points[pointIndex][1])
        minIndex = pointIndex;
    }
    else
    {
      maxIndex = pointIndex;
    }
  }

  hullOrder[0] = minIndex;
  hullOrder[1] = maxIndex;
  if (minIndex <= maxIndex)
  {
    Com_SwapHullPoints(pointOrder, maxIndex, pointCount - 1);
    Com_SwapHullPoints(pointOrder, minIndex, pointCount - 2);
  }
  else
  {
    Com_SwapHullPoints(pointOrder, minIndex, pointCount - 1);
    Com_SwapHullPoints(pointOrder, maxIndex, pointCount - 2);
  }
}

/* CoD4 0x466D70. */
static void Com_SwapHullPoints(unsigned int *pointOrder,
                               unsigned int pointIndex0,
                               unsigned int pointIndex1)
{
  unsigned int swapCache = pointOrder[pointIndex0];

  pointOrder[pointIndex0] = pointOrder[pointIndex1];
  pointOrder[pointIndex1] = swapCache;
}

/* CoD4 0x466DB0. */
static unsigned int Com_GrowInitialHull(const float points[64][2],
                                        unsigned int *pointOrder,
                                        unsigned int pointCount,
                                        unsigned int *hullOrder)
{
  float edgeEq[2];
  float edgeDist;
  float dot;
  float dist;
  float frontDist;
  float backDist;
  unsigned int hullPointCount;
  int botIndex;
  int topIndex;
  int frontIndex;
  int backIndex;

  edgeEq[0] = points[hullOrder[1]][1] - points[hullOrder[0]][1];
  edgeEq[1] = points[hullOrder[0]][0] - points[hullOrder[1]][0];
  Vec2Normalize(edgeEq);
  edgeDist = points[hullOrder[0]][1] * edgeEq[1]
           + points[hullOrder[0]][0] * edgeEq[0];
  botIndex = 0;
  topIndex = pointCount - 1;
  frontDist = 0.001000000047497451f;
  frontIndex = -1;
  backDist = -0.001000000047497451f;
  backIndex = -1;

  while (botIndex <= topIndex)
  {
    for (;;)
    {
      dot = points[pointOrder[botIndex]][1] * edgeEq[1]
          + points[pointOrder[botIndex]][0] * edgeEq[0];
      dist = dot - edgeDist;
      if (dist < 0.0f)
        break;
      if (dist > frontDist)
      {
        frontDist = dist;
        frontIndex = botIndex;
      }
      if (++botIndex > topIndex)
        goto initial_split_done;
    }
    if (dist < backDist)
    {
      backDist = dist;
      backIndex = botIndex;
    }
    for (;;)
    {
      dot = points[pointOrder[topIndex]][1] * edgeEq[1]
          + points[pointOrder[topIndex]][0] * edgeEq[0];
      dist = dot - edgeDist;
      if (dist > 0.0f)
        break;
      if (dist < backDist)
      {
        backDist = dist;
        backIndex = topIndex;
      }
      if (botIndex > --topIndex)
        goto initial_split_done;
    }
    if (dist > frontDist)
    {
      frontDist = dist;
      frontIndex = botIndex;
    }
    Com_SwapHullPoints(pointOrder, botIndex, topIndex);
    if (backIndex == botIndex)
      backIndex = topIndex;
    ++botIndex;
    --topIndex;
  }

initial_split_done:
  if (frontIndex < 0 && backIndex < 0)
    return 0;

  hullPointCount = 2;
  if (frontIndex >= 0)
  {
    Com_SwapHullPoints(pointOrder, frontIndex, topIndex);
    hullPointCount = Com_AddPointToHull(pointOrder[topIndex], 2, hullOrder,
                                        hullPointCount);
    if (topIndex > 0)
    {
      hullPointCount = Com_RecursivelyGrowHull(points, pointOrder, topIndex,
                                                2, 0, hullOrder,
                                                hullPointCount);
      hullPointCount = Com_RecursivelyGrowHull(points, pointOrder, topIndex,
                                                1, 2, hullOrder,
                                                hullPointCount);
    }
  }
  if (backIndex >= 0)
  {
    Com_SwapHullPoints(pointOrder, backIndex, botIndex);
    hullPointCount = Com_AddPointToHull(pointOrder[botIndex], 1, hullOrder,
                                        hullPointCount);
    if (pointCount != (unsigned int)(botIndex + 1))
    {
      hullPointCount = Com_RecursivelyGrowHull(
          points, &pointOrder[botIndex + 1], pointCount - (botIndex + 1),
          1, 2, hullOrder, hullPointCount);
      return Com_RecursivelyGrowHull(
          points, &pointOrder[botIndex + 1], pointCount - (botIndex + 1),
          0, 1, hullOrder, hullPointCount);
    }
  }
  return hullPointCount;
}

/* CoD4 0x4671F0. */
static unsigned int Com_AddPointToHull(unsigned int pointIndex,
                                       unsigned int newIndex,
                                       unsigned int *hullOrder,
                                       unsigned int hullPointCount)
{
  memmove(&hullOrder[newIndex + 1], &hullOrder[newIndex],
          4 * (hullPointCount - newIndex));
  hullOrder[newIndex] = pointIndex;
  return hullPointCount + 1;
}

/* CoD4 0x467260. */
static unsigned int Com_RecursivelyGrowHull(const float points[64][2],
                                             unsigned int *pointOrder,
                                             unsigned int pointCount,
                                             unsigned int firstIndex,
                                             unsigned int secondIndex,
                                             unsigned int *hullOrder,
                                             unsigned int hullPointCount)
{
  float edgeEq[2];
  float edgeDist;
  float dot;
  float dist;
  float frontDist;
  int botIndex;
  int topIndex;
  int frontIndex;

  edgeEq[0] = points[hullOrder[firstIndex]][1]
            - points[hullOrder[secondIndex]][1];
  edgeEq[1] = points[hullOrder[secondIndex]][0]
            - points[hullOrder[firstIndex]][0];
  Vec2Normalize(edgeEq);
  edgeDist = points[hullOrder[firstIndex]][1] * edgeEq[1]
           + points[hullOrder[firstIndex]][0] * edgeEq[0];
  botIndex = 0;
  topIndex = pointCount - 1;
  frontDist = 0.001000000047497451f;
  frontIndex = -1;

  while (botIndex <= topIndex)
  {
    for (;;)
    {
      dot = points[pointOrder[botIndex]][1] * edgeEq[1]
          + points[pointOrder[botIndex]][0] * edgeEq[0];
      dist = dot - edgeDist;
      if (dist <= 0.0f)
        break;
      if (dist > frontDist)
      {
        frontDist = dist;
        frontIndex = botIndex;
      }
      if (++botIndex > topIndex)
        goto split_done;
    }
    for (;;)
    {
      dot = points[pointOrder[topIndex]][1] * edgeEq[1]
          + points[pointOrder[topIndex]][0] * edgeEq[0];
      dist = dot - edgeDist;
      if (dist > 0.0f)
        break;
      if (botIndex > --topIndex)
        goto split_done;
    }
    if (dist > frontDist)
    {
      frontDist = dist;
      frontIndex = botIndex;
    }
    Com_SwapHullPoints(pointOrder, botIndex++, topIndex--);
  }

split_done:
  if (frontIndex < 0)
    return hullPointCount;

  Com_SwapHullPoints(pointOrder, frontIndex, topIndex);
  hullPointCount = Com_AddPointToHull(pointOrder[topIndex], firstIndex + 1,
                                      hullOrder, hullPointCount);
  if (!topIndex)
    return hullPointCount;
  if (secondIndex)
    secondIndex = firstIndex + 2;
  hullPointCount = Com_RecursivelyGrowHull(points, pointOrder, topIndex,
                                            firstIndex + 1, secondIndex,
                                            hullOrder, hullPointCount);
  return Com_RecursivelyGrowHull(points, pointOrder, topIndex, firstIndex,
                                  firstIndex + 1, hullOrder, hullPointCount);
}

/* CoD4 0x4675C0. */
static void Com_TranslatePoints(float points[64][2], unsigned int pointCount,
                                const float *offset)
{
  unsigned int pointIndex;

  for (pointIndex = 0; pointIndex < pointCount; ++pointIndex)
  {
    points[pointIndex][0] += offset[0];
    points[pointIndex][1] += offset[1];
  }
}

/* CoD4 0x467620. */
unsigned int Com_ConvexHullIndices(float points[64][2],
                                    unsigned int pointCount,
                                    unsigned int *hullOrder)
{
  unsigned int pointOrder[64];
  float offset[2];

  offset[0] = -ceilf(points[0][0]);
  offset[1] = -ceilf(points[0][1]);
  Com_TranslatePoints(points, pointCount, offset);
  Com_InitialHull((const float (*)[2])points, pointOrder, pointCount,
                  hullOrder);
  return Com_GrowInitialHull((const float (*)[2])points, pointOrder,
                              pointCount - 2, hullOrder);
}

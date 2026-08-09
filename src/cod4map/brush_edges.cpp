/* Collision brush-side edge adjacency (CoD4 0x4032A0). */
#include "cod4map.h"

#define BRUSH_EDGE_CONTENTS_MASK 0x0280E491

static char s_assertDisable_BrushEdges;

/* CoD4 0x402C50: retain a three-plane intersection only when it is inside
   every collision plane other than the planes that formed it. */
/* CoD4 0x405590. */
static BrushAdjacencyWinding_t *AllocBrushAdjacencyWinding(int numSides)
{
  BrushAdjacencyWinding_t *winding;

  winding = (BrushAdjacencyWinding_t *)malloc(sizeof(int) * (numSides + 1));
  if ( !winding )
    Com_Error("Out of memory");
  memset(winding, 0, sizeof(int) * (numSides + 1));
  return winding;
}

/* CoD4 0x405B60. */
static void ReverseBrushAdjacencyWinding(BrushAdjacencyWinding_t *winding)
{
  int *first = winding->sides;
  int *last = &winding->sides[winding->numsides - 1];

  while ( first < last )
  {
    int side = *first;
    *first++ = *last;
    *last-- = side;
  }
}

/* CoD4 0x405D90. */
static int IsPtFormedByThisPlane(int planeIndex, const BrushPoint_t *point)
{
  return point->planeIndices[0] == planeIndex
      || point->planeIndices[1] == planeIndex
      || point->planeIndices[2] == planeIndex;
}

/* CoD4 0x405E50 and 0x405EC0. */
static int OtherPlane(const BrushPoint_t *point, int planeIndex)
{
  int i;

  for ( i = 0; i < 3; ++i )
    if ( point->planeIndices[i] != planeIndex )
      return point->planeIndices[i];
  Assert(0, s_assertDisable_BrushEdges);
  return -1;
}

static int ThirdPlane(const BrushPoint_t *point, int plane0, int plane1)
{
  int i;

  for ( i = 0; i < 3; ++i )
    if ( point->planeIndices[i] != plane0 && point->planeIndices[i] != plane1 )
      return point->planeIndices[i];
  Assert(0, s_assertDisable_BrushEdges);
  return -1;
}

/* CoD4 0x405FA0 and 0x405F40. */
static BrushPoint_t **FindPointFormedByPlane(int planeIndex, BrushPoint_t **begin,
                                              BrushPoint_t **end)
{
  while ( begin != end && !IsPtFormedByThisPlane(planeIndex, *begin) )
    ++begin;
  return begin;
}

static BrushPoint_t *PopPointFormedByPlane(int planeIndex, BrushPoint_t **begin,
                                            BrushPoint_t **end)
{
  BrushPoint_t **found = FindPointFormedByPlane(planeIndex, begin, end);
  BrushPoint_t *point;

  if ( found == end )
    return NULL;
  point = *found;
  memcpy(found, found + 1, sizeof(*found) * (end - found - 1));
  return point;
}

/* CoD4 0x405DD0. */
static int SharedPlaneExcept(const BrushPoint_t *point0, const BrushPoint_t *point1,
                             int excludedPlane, int *outPlane)
{
  int i;
  int j;

  for ( i = 0; i < 3; ++i )
    for ( j = 0; j < 3; ++j )
      if ( point0->planeIndices[i] == point1->planeIndices[j]
        && point0->planeIndices[i] != excludedPlane )
      {
        *outPlane = point0->planeIndices[i];
        return 1;
      }
  return 0;
}

/* CoD4 0x405D00 and 0x406AB0. */
static int PointsFormedByPlane(int planeIndex, BrushPoint_t *points, int pointCount,
                               BrushPoint_t **out, int maxOut)
{
  int i;
  int count = 0;

  for ( i = 0; i < pointCount; ++i )
  {
    if ( IsPtFormedByThisPlane(planeIndex, &points[i]) )
    {
      if ( count == maxOut )
        return 0;
      out[count++] = &points[i];
    }
  }
  return count;
}

static int IntInList(const int *values, int count, int value)
{
  int i;

  for ( i = 0; i < count; ++i )
    if ( values[i] == value )
      return 1;
  return 0;
}

/* CoD4 0x4074F0. */
static int CountUniqueAdjacencyPoints(BrushPoint_t *const *points, int pointCount)
{
  BrushPoint_t *unique[MAX_POINTS_ON_WINDING + 1];
  int uniqueCount = 0;
  int i;

  Assert(points != NULL, s_assertDisable_BrushEdges);
  Assert(pointCount > 2, s_assertDisable_BrushEdges);
  for ( i = 0; i < pointCount; ++i )
  {
    int j;
    for ( j = 0; j < uniqueCount; ++j )
      if ( VectorCompareEpsilon(points[i]->xyz, unique[j]->xyz, 0.01f, 3) )
        break;
    if ( j == uniqueCount )
      unique[uniqueCount++] = points[i];
  }
  return uniqueCount;
}

/* CoD4 0x405FE0. */
static float CycleLength(BrushPoint_t *const *points, int pointCount)
{
  float length;
  int i;

  Assert(pointCount > 2, s_assertDisable_BrushEdges);
  length = (float)VectorDistance(points[0]->xyz, points[pointCount - 1]->xyz);
  for ( i = 1; i < pointCount; ++i )
    length += (float)VectorDistance(points[i]->xyz, points[i - 1]->xyz);
  return length;
}

/* CoD4 0x4061B0 and 0x406070. */
static int IsConvexPointCycle(BrushPoint_t *const *points, int pointCount)
{
  vec3_t filtered[1024];
  vec3_t edge0;
  vec3_t edge1;
  vec3_t firstNormal;
  vec3_t normal;
  int previous = pointCount - 1;
  int previousPrevious = pointCount - 2;
  int i;
  int foundNormal = 0;

  Assert(pointCount > 2, s_assertDisable_BrushEdges);
  for ( i = 0; i < pointCount; ++i )
    VectorCopy(points[i]->xyz, filtered[i]);

  /* Native 0x406070 removes adjacent points less than one unit apart before
     entering the strict 0x4061B0 convexity predicate. */
  i = 1;
  while ( i < pointCount )
  {
    if ( VectorDistance(filtered[i], filtered[i - 1]) >= 1.0f )
    {
      ++i;
    }
    else
    {
      memmove(&filtered[i], &filtered[i + 1],
        sizeof(filtered[0]) * (pointCount - i - 1));
      --pointCount;
    }
  }
  if ( VectorDistance(filtered[0], filtered[pointCount - 1]) < 1.0f )
    --pointCount;
  if ( pointCount < 3 )
    return 0;

  previous = pointCount - 1;
  previousPrevious = pointCount - 2;
  for ( i = 0; i < pointCount; ++i )
  {
    VectorSubtract(filtered[previous], filtered[previousPrevious], edge0);
    VectorSubtract(filtered[i], filtered[previous], edge1);
    Assert(DotProduct(edge0, edge0) > 0.0f, s_assertDisable_BrushEdges);
    Assert(DotProduct(edge1, edge1) > 0.0f, s_assertDisable_BrushEdges);
    CrossProduct(edge0, edge1, normal);
    if ( VecNormalize(normal) >= 0.01f )
    {
      if ( !foundNormal )
      {
        VectorCopy(normal, firstNormal);
        foundNormal = 1;
      }
      else if ( DotProduct(firstNormal, normal) < 0.5f )
      {
        return 0;
      }
    }
    else if ( DotProduct(edge0, edge1) < 0.0f )
    {
      return 0;
    }
    previousPrevious = previous;
    previous = i;
  }
  return 1;
}

static int ShouldPreferSecondCycle(int firstIsConvex, int secondIsConvex,
                                   float firstLength, float secondLength,
                                   int firstCount, int secondCount)
{
  if ( firstIsConvex )
  {
    if ( !secondIsConvex )
      return 0;
  }
  else if ( secondIsConvex )
  {
    return 1;
  }
  return firstLength < secondLength - 1.0f
      || (secondLength >= firstLength - 1.0f && firstCount > secondCount);
}

/* CoD4 0x406AF0. */
typedef struct AdjacencyCycleNode_s {
  BrushPoint_t *point;
  int plane;
  int depth;
  struct AdjacencyCycleNode_s *previous;
} AdjacencyCycleNode_t;

static int FindPointCycle(int basePlane, BrushPoint_t **points, int pointCount,
                          BrushPoint_t *start, BrushPoint_t *end, int connectingPlane,
                          BrushPoint_t **outCycle, int *outCount)
{
  AdjacencyCycleNode_t queue[MAX_POINTS_ON_WINDING];
  int queueHead = 0;
  int queueTail = 1;
  int goalPlane;
  int i;
  AdjacencyCycleNode_t *node;

  Assert(IsPtFormedByThisPlane(connectingPlane, start), s_assertDisable_BrushEdges);
  Assert(IsPtFormedByThisPlane(connectingPlane, end), s_assertDisable_BrushEdges);
  goalPlane = ThirdPlane(end, basePlane, connectingPlane);
  queue[0].point = start;
  queue[0].plane = ThirdPlane(start, basePlane, connectingPlane);
  queue[0].depth = 1;
  queue[0].previous = NULL;

  while ( queueHead < queueTail )
  {
    BrushPoint_t **candidate = FindPointFormedByPlane(queue[queueHead].plane, points, points + pointCount);
    while ( candidate != points + pointCount )
    {
      int nextPlane = ThirdPlane(*candidate, basePlane, queue[queueHead].plane);
      int seenPlane = 0;
      int i;

      for ( i = 0; i < queueTail; ++i )
        if ( queue[i].plane == nextPlane )
        {
          seenPlane = 1;
          break;
        }

      if ( nextPlane != connectingPlane && !seenPlane )
      {
        if ( queueTail >= MAX_POINTS_ON_WINDING )
          Com_Error("Out of memory");
        queue[queueTail].point = *candidate;
        queue[queueTail].plane = nextPlane;
        queue[queueTail].depth = queue[queueHead].depth + 1;
        queue[queueTail].previous = &queue[queueHead];
        ++queueTail;
        if ( nextPlane == goalPlane )
          goto found;
      }
      candidate = FindPointFormedByPlane(queue[queueHead].plane, candidate + 1, points + pointCount);
    }
    ++queueHead;
  }

  *outCount = 0;
  return 0;

found:
  node = &queue[queueTail - 1];
  *outCount = node->depth + 1;
  i = node->depth;
  while ( node )
  {
    outCycle[i--] = node->point;
    node = node->previous;
  }
  Assert(i == 0, s_assertDisable_BrushEdges);
  outCycle[0] = end;
  return 1;
}

/* CoD4 0x406E40, 0x406F10, 0x406FB0 and 0x407460. */
static int CountPlaneOccurrences(int planeIndex, BrushPoint_t **points, int pointCount)
{
  int count = 0;
  BrushPoint_t **point;

  Assert(points != NULL, s_assertDisable_BrushEdges);
  for ( point = FindPointFormedByPlane(planeIndex, points, points + pointCount);
        point != points + pointCount;
        point = FindPointFormedByPlane(planeIndex, point + 1, points + pointCount) )
    ++count;
  return count;
}

static int RemovePointsWithInsufficientPlaneOccurrences(BrushPoint_t **points, int pointCount)
{
  int i = 0;

  while ( i < pointCount )
  {
    if ( CountPlaneOccurrences(points[i]->planeIndices[0], points, pointCount) >= 2
      && CountPlaneOccurrences(points[i]->planeIndices[1], points, pointCount) >= 2
      && CountPlaneOccurrences(points[i]->planeIndices[2], points, pointCount) >= 2 )
    {
      ++i;
    }
    else
    {
      memcpy(&points[i], &points[i + 1], sizeof(*points) * (pointCount - i - 1));
      --pointCount;
      i = 0;
    }
  }
  return pointCount;
}

static int FindPlaneOccurrences(int planeIndex, BrushPoint_t **points, int pointCount,
                                BrushPoint_t **out, int maxOut)
{
  int count = 0;
  BrushPoint_t **point;

  Assert(points != NULL, s_assertDisable_BrushEdges);
  Assert(out != NULL, s_assertDisable_BrushEdges);
  for ( point = FindPointFormedByPlane(planeIndex, points, points + pointCount);
        point != points + pointCount;
        point = FindPointFormedByPlane(planeIndex, point + 1, points + pointCount) )
  {
    Assert(count < maxOut, s_assertDisable_BrushEdges);
    out[count++] = *point;
  }
  return count;
}

static int RemovePoint(BrushPoint_t **points, int pointCount, BrushPoint_t *point)
{
  int i;

  for ( i = 0; i < pointCount && points[i] != point; ++i )
    ;
  if ( i == pointCount )
    return pointCount;
  memcpy(&points[i], &points[i + 1], sizeof(*points) * (pointCount - i - 1));
  --pointCount;
  return pointCount >= 3 ? RemovePointsWithInsufficientPlaneOccurrences(points, pointCount) : pointCount;
}

/* CoD4 0x4072E0. */
static int PartitionPointCycles(int basePlane, int connectingPlane, BrushPoint_t **points,
                                int pointCount, BrushPoint_t **occurrences, int occurrenceCount,
                                int *partitions)
{
  BrushPoint_t *cycle[MAX_POINTS_ON_WINDING + 1];
  int cycleCount;
  int partitionCount = 1;

  partitions[0] = 1;
  while ( partitions[partitionCount - 1] < occurrenceCount )
  {
    int current = partitions[partitionCount - 1];
    int i;

    for ( i = 0; i < partitionCount; ++i )
    {
      if ( FindPointCycle(basePlane, points, pointCount,
                          occurrences[partitions[i] - 1], occurrences[current],
                          connectingPlane, cycle, &cycleCount) )
      {
        int j;

        if ( i < partitionCount - 1 )
        {
          BrushPoint_t *point = occurrences[current];
          /* cod4map's CRT _memcpy detects this overlap and copies downward. */
          memmove(&occurrences[partitions[i] + 1], &occurrences[partitions[i]],
                  sizeof(*occurrences) * (current - partitions[i]));
          occurrences[partitions[i]] = point;
        }
        for ( j = i; j < partitionCount; ++j )
          ++partitions[j];
        break;
      }
    }
    if ( i == partitionCount )
      partitions[partitionCount++] = current + 1;
  }
  return partitionCount;
}

/* CoD4 0x4070B0. */
static int SelectPointToRemove(int basePlane, int connectingPlane, BrushPoint_t **points,
                               int pointCount, BrushPoint_t **occurrences)
{
  BrushPoint_t *cycles[3][MAX_POINTS_ON_WINDING + 1];
  int cycleCounts[3];
  int isCycle[3];
  float lengths[3];
  int best;

  isCycle[0] = FindPointCycle(basePlane, points, pointCount, occurrences[0], occurrences[1],
                              connectingPlane, cycles[0], &cycleCounts[0]);
  isCycle[1] = FindPointCycle(basePlane, points, pointCount, occurrences[0], occurrences[2],
                              connectingPlane, cycles[1], &cycleCounts[1]);
  isCycle[2] = FindPointCycle(basePlane, points, pointCount, occurrences[1], occurrences[2],
                              connectingPlane, cycles[2], &cycleCounts[2]);
  Assert(isCycle[0] && isCycle[1] && isCycle[2], s_assertDisable_BrushEdges);
  lengths[0] = CycleLength(cycles[0], cycleCounts[0]);
  lengths[1] = CycleLength(cycles[1], cycleCounts[1]);
  lengths[2] = CycleLength(cycles[2], cycleCounts[2]);
  best = ShouldPreferSecondCycle(IsConvexPointCycle(cycles[0], cycleCounts[0]),
                                 IsConvexPointCycle(cycles[1], cycleCounts[1]),
                                 lengths[0], lengths[1], cycleCounts[0], cycleCounts[1]);
  if ( ShouldPreferSecondCycle(IsConvexPointCycle(cycles[best], cycleCounts[best]),
                               IsConvexPointCycle(cycles[2], cycleCounts[2]),
                               lengths[best], lengths[2], cycleCounts[best], cycleCounts[2]) )
    best = 2;
  return 2 - best;
}

/* CoD4 0x406510. */
static int RemoveRedundantAdjacencyPoints(int basePlane, BrushPoint_t **points, int pointCount)
{
  int planeList[MAX_POINTS_ON_WINDING];
  BrushPoint_t *occurrences[MAX_POINTS_ON_WINDING];
  BrushPoint_t *cycle0[MAX_POINTS_ON_WINDING + 1];
  BrushPoint_t *cycle1[MAX_POINTS_ON_WINDING + 1];
  int planeCount = 0;
  int i;

  pointCount = RemovePointsWithInsufficientPlaneOccurrences(points, pointCount);
  if ( pointCount < 3 )
    return pointCount;
  for ( i = 0; i < pointCount; ++i )
  {
    int j;
    for ( j = 0; j < 3; ++j )
    {
      int plane = points[i]->planeIndices[j];
      if ( plane != basePlane && !IntInList(planeList, planeCount, plane) )
      {
        Assert(planeCount < MAX_POINTS_ON_WINDING, s_assertDisable_BrushEdges);
        planeList[planeCount++] = plane;
      }
    }
  }
  Assert(planeCount > 2, s_assertDisable_BrushEdges);

  for ( i = 0; i < planeCount; ++i )
  {
    for ( ;; )
    {
      int partitions[MAX_POINTS_ON_WINDING + 1];
      int occurrenceCount = FindPlaneOccurrences(planeList[i], points, pointCount, occurrences,
                                                 MAX_POINTS_ON_WINDING);
      int partitionCount;
      int start = 0;
      int partition;

      if ( occurrenceCount <= 2 )
        break;
      partitionCount = PartitionPointCycles(basePlane, planeList[i], points, pointCount,
                                            occurrences, occurrenceCount, partitions);
      for ( partition = 0; partition < partitionCount && partition < 2; ++partition )
      {
        int partitionSize = partitions[partition] - start;

        Assert(partitionSize > 0, s_assertDisable_BrushEdges);
        if ( partitionSize == 1 )
        {
          pointCount = RemovePoint(points, pointCount, occurrences[start]);
          break;
        }
        if ( partitionSize > 2 )
        {
          int point = SelectPointToRemove(basePlane, planeList[i], points, pointCount,
                                          &occurrences[start]);
          pointCount = RemovePoint(points, pointCount, occurrences[start + point]);
          break;
        }
        start = partitions[partition];
      }
      if ( partition < 2 )
      {
        if ( pointCount < 3 )
          return pointCount;
        continue;
      }

      Assert(partitions[0] == 2, s_assertDisable_BrushEdges);
      Assert(partitions[1] == 4, s_assertDisable_BrushEdges);
      {
        int cycle0Count;
        int cycle1Count;
        int cycle0Valid = FindPointCycle(basePlane, points, pointCount, occurrences[0], occurrences[1],
                                         planeList[i], cycle0, &cycle0Count);
        int cycle1Valid = FindPointCycle(basePlane, points, pointCount, occurrences[2], occurrences[3],
                                         planeList[i], cycle1, &cycle1Count);
        Assert(cycle0Valid && cycle1Valid, s_assertDisable_BrushEdges);
        if ( ShouldPreferSecondCycle(IsConvexPointCycle(cycle0, cycle0Count),
                                     IsConvexPointCycle(cycle1, cycle1Count),
                                     CycleLength(cycle0, cycle0Count), CycleLength(cycle1, cycle1Count),
                                     cycle0Count, cycle1Count) )
        {
          pointCount = RemovePoint(points, pointCount, occurrences[0]);
          pointCount = RemovePoint(points, pointCount, occurrences[1]);
        }
        else
        {
          pointCount = RemovePoint(points, pointCount, occurrences[2]);
          pointCount = RemovePoint(points, pointCount, occurrences[3]);
        }
      }
      if ( pointCount < 3 )
        return pointCount;
    }
  }
  return pointCount;
}

/* CoD4 0x405BC0, 0x4074F0 and 0x4055E0. */
static float LargestCycleTriangleArea(BrushPoint_t *const *points, int pointCount,
                                      const float *normal, int *out0, int *out1, int *out2)
{
  float largest = 0.0f;
  int i;
  int j;
  int k;

  *out0 = 0;
  *out1 = 1;
  *out2 = 2;
  for ( i = 2; i < pointCount; ++i )
    for ( j = 1; j < i; ++j )
      for ( k = 0; k < j; ++k )
      {
        vec3_t edge0;
        vec3_t edge1;
        vec3_t cross;
        float area;

        VectorSubtract(points[i]->xyz, points[j]->xyz, edge0);
        VectorSubtract(points[k]->xyz, points[j]->xyz, edge1);
        CrossProduct(edge1, edge0, cross);
        area = (float)fabs(DotProduct(cross, normal));
        if ( largest < area )
        {
          largest = area;
          *out0 = k;
          *out1 = j;
          *out2 = i;
        }
      }
  return largest;
}

static BrushAdjacencyWinding_t *BuildBrushAdjacencyWinding(const Plane_t *basePlane,
                                                            int basePlaneIndex,
                                                            BrushPoint_t *points,
                                                            int pointCount,
                                                            BrushAdjacencyWinding_t *output,
                                                            int maxEdges)
{
  BrushPoint_t *remaining[MAX_POINTS_ON_WINDING];
  BrushPoint_t *bestCycle[MAX_POINTS_ON_WINDING];
  BrushPoint_t *candidateCycle[MAX_POINTS_ON_WINDING];
  int remainingCount;
  int bestCount = 0;
  int triangle0;
  int triangle1;
  int triangle2;
  BrushAdjacencyWinding_t *winding;

  remainingCount = PointsFormedByPlane(basePlaneIndex, points, pointCount,
                                       remaining, MAX_POINTS_ON_WINDING);
  if ( remainingCount < 3 )
    return NULL;
  if ( CountUniqueAdjacencyPoints(remaining, remainingCount) < 3 )
    return NULL;
  remainingCount = RemoveRedundantAdjacencyPoints(basePlaneIndex, remaining, remainingCount);
  if ( remainingCount < 3 )
    return NULL;

  while ( remainingCount )
  {
    int candidateCount = 1;
    int currentPlane;

    candidateCycle[0] = remaining[--remainingCount];
    currentPlane = OtherPlane(candidateCycle[0], basePlaneIndex);
    while ( remainingCount )
    {
      BrushPoint_t *point = PopPointFormedByPlane(currentPlane, remaining,
                                                   remaining + remainingCount);
      if ( !point )
        break;
      candidateCycle[candidateCount++] = point;
      currentPlane = ThirdPlane(point, basePlaneIndex, currentPlane);
      --remainingCount;
    }
    Assert(IsPtFormedByThisPlane(currentPlane, candidateCycle[0]), s_assertDisable_BrushEdges);
    if ( !bestCount
      || ShouldPreferSecondCycle(IsConvexPointCycle(bestCycle, bestCount),
                                 IsConvexPointCycle(candidateCycle, candidateCount),
                                 CycleLength(bestCycle, bestCount), CycleLength(candidateCycle, candidateCount),
                                 bestCount, candidateCount) )
    {
      memcpy(bestCycle, candidateCycle, sizeof(bestCycle[0]) * candidateCount);
      bestCount = candidateCount;
    }
  }

  Assert(bestCount > 2, s_assertDisable_BrushEdges);
  if ( output )
  {
    winding = output;
    if ( maxEdges < bestCount )
    {
      Com_Error("Brush face has too many edges");
      return NULL;
    }
  }
  else
  {
    winding = AllocBrushAdjacencyWinding(bestCount);
  }
  Assert(winding != NULL, s_assertDisable_BrushEdges);
  if ( !SharedPlaneExcept(bestCycle[0], bestCycle[bestCount - 1], basePlaneIndex,
                          &winding->sides[0]) )
    Assert(0, s_assertDisable_BrushEdges);
  winding->numsides = 1;
  for ( ; winding->numsides < bestCount; ++winding->numsides )
    winding->sides[winding->numsides] = ThirdPlane(bestCycle[winding->numsides - 1],
                                                    basePlaneIndex,
                                                    winding->sides[winding->numsides - 1]);
  Assert(winding->sides[0] == ThirdPlane(bestCycle[bestCount - 1], basePlaneIndex,
                                          winding->sides[winding->numsides - 1]),
         s_assertDisable_BrushEdges);
  if ( LargestCycleTriangleArea(bestCycle, winding->numsides, basePlane->normal,
                                &triangle0, &triangle1, &triangle2) < 0.001f )
  {
    if ( !output )
      FreeBrushAdjacencyWinding(winding);
    return NULL;
  }
  {
    vec4_t windingPlane;
    PlaneFromPoints(windingPlane, bestCycle[triangle0]->xyz, bestCycle[triangle1]->xyz,
                    bestCycle[triangle2]->xyz);
    if ( DotProduct(windingPlane, basePlane->normal) < 0.0f )
      ReverseBrushAdjacencyWinding(winding);
  }
  return winding;
}

/* CoD4 0x4031F0. */
static void RemapAdjacencyPlanesToSides(Brush_t *brush, BrushAdjacencyWinding_t *winding)
{
  int i;

  for ( i = 0; i < winding->numsides; ++i )
  {
    int sideIndex;
    for ( sideIndex = 0; sideIndex < brush->numCollisionSides; ++sideIndex )
      if ( brush->collisionSides[sideIndex].planenum == winding->sides[i] )
      {
        winding->sides[i] = sideIndex;
        break;
      }
    Assert(sideIndex < brush->numCollisionSides, s_assertDisable_BrushEdges);
  }
}

void BuildBrushSideAdjacencies(Brush_t *brush)
{
  int pointCount;
  int sideIndex;

  Assert(brush->collisionSides != NULL, s_assertDisable_BrushEdges);
  pointCount = BrushToPoints(brush, brush->collisionSides, brush->numCollisionSides,
                             0.0f, 0.0f, qfalse, AddBrushPoint);
  if ( pointCount < 4 )
    return;
  for ( sideIndex = 0; sideIndex < brush->numCollisionSides; ++sideIndex )
  {
    BrushSide_t *side = &brush->collisionSides[sideIndex];
    int skipSide = side->contentFlags == CONTENTS_SOLID && (side->surfaceFlags & 0x80);

    Assert(side->adjacencyWinding == NULL, s_assertDisable_BrushEdges);
    if ( side->winding && !skipSide && (side->contentFlags & BRUSH_EDGE_CONTENTS_MASK) )
    {
      side->adjacencyWinding = BuildBrushAdjacencyWinding(MAP_PLANE(side->planenum), side->planenum,
                                                           (BrushPoint_t *)g_brushPoints, pointCount,
                                                           NULL, 0);
      if ( side->adjacencyWinding )
        RemapAdjacencyPlanesToSides(brush, side->adjacencyWinding);
    }
  }
}

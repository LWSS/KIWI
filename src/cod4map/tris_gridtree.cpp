/*
tris_gridtree.c — Grid tree for spatial lookups

Reconstructed from cod2map.exe by Rose.

Spatial acceleration structure for the tris coalescing system.
Divides world bounds into a grid of cells for fast neighbor queries
during winding merging and overlap detection.
*/

#include "cod4map.h"

GridTreeNode_t *gridTreeRoot;
static float gridTreeBoundsMins[3];
static float gridTreeBoundsMaxs[3];

char      g_fmtTwoInts[8] = { '%', 'i', ',', ' ', '%', 'i', '\0', '\0' };
const int g_gridTreeSplitAxis[GRID_TREE_AXIS_COUNT] = { 0, 1, 0, 1, 0, 1, 2, 0, 1, 0, 1, 0, 1, 2, 0, 1, 0, 1, 0, 1, 2 };

char s_assertDisable_GridTree_ClassifyNode;
char s_assertDisable_GridTree_FindNode;
char s_assertDisable_GridTree_FindNode;
char s_assertDisable_GridTree_FindNode;
char s_assertDisable_GridTree_FindNode;
char s_assertDisable_GridTree_Init;
char s_assertDisable_GridTree_Insert;
char s_assertDisable_GridTree_Remove;
char s_assertDisable_GridTree_Shutdown;
char s_assertDisable_SetGridDivisionPoints;


/*
================
GridTree_ClassifyNode

Classifies a surface AABB against a grid tree split plane.
Returns 1 (front/left), 0 (back/right), or 3 (both/straddling).
================
*/
int GridTree_ClassifyNode(int nodeIdx, unsigned int depth, float *mins, float *maxs)
{
  int splitAxis;

  Assert(depth < GRID_TREE_MAX_DEPTH, s_assertDisable_GridTree_ClassifyNode);
  splitAxis = g_gridTreeSplitAxis[depth];
  
  /* volatile forces float-to-float FPU comparison, not double */
  volatile float maxsVal = maxs[splitAxis];
  volatile float minsVal = mins[splitAxis];
  volatile float nodeMin = gridTreeRoot[nodeIdx].min;
  volatile float nodeMax = gridTreeRoot[nodeIdx].max;

  if ( nodeMax >= maxsVal )
    return 1;
  if ( nodeMin > minsVal )
    return 3;
  return 0;
}

/*
================
GridTree_FindNode

Finds the grid tree leaf node for a given AABB by walking
the binary tree using split plane classification at each depth.
================
*/
GridTreeNode_t *GridTree_FindNode(float *mins, float *maxs)
{
  int nodeIdx;
  unsigned int depth;
  unsigned int side;

  Assert(mins, s_assertDisable_GridTree_FindNode);
  Assert(maxs, s_assertDisable_GridTree_FindNode);
  Assert(gridTreeRoot, s_assertDisable_GridTree_FindNode);
  nodeIdx = 0;
  depth = 0;
  while ( 1 )
  {
    side = GridTree_ClassifyNode(nodeIdx, depth, mins, maxs);
    if ( side == 3 )
      break;
    Assert(side < 2, s_assertDisable_GridTree_FindNode);
    ++depth;
    nodeIdx = side + 2 * nodeIdx + 1;
    if ( depth >= GRID_TREE_MAX_DEPTH )
      return &gridTreeRoot[nodeIdx];
  }
  return &gridTreeRoot[nodeIdx];
}

/* Native 0x450A00 list unlink, used by the rebuild pass to maintain the
   same intrusive-list invariants as the public removal operation. */
static void GridTree_RemoveFromNode(GridTreeNode_t *node, TriSurf_t *ts)
{
  Assert(node, s_assertDisable_GridTree_Remove);
  Assert(ts, s_assertDisable_GridTree_Remove);

  if ( ts->gridNext )
    ts->gridNext->gridPrev = ts->gridPrev;

  if ( ts->gridPrev )
  {
    Assert(node->surfListHead != ts, s_assertDisable_GridTree_Remove);
    ts->gridPrev->gridNext = ts->gridNext;
  }
  else
  {
    Assert(node->surfListHead == ts, s_assertDisable_GridTree_Remove);
    node->surfListHead = ts->gridNext;
    Assert(!node->surfListHead || !node->surfListHead->gridPrev,
           s_assertDisable_GridTree_Remove);
  }

  ts->gridPrev = NULL;
  ts->gridNext = NULL;
}

/* Native prepend operation used by 0x4505F0 and the 0x450760 rebuilder. */
static TriSurf_t *GridTree_InsertIntoNode(GridTreeNode_t *node, TriSurf_t *ts)
{
  Assert(node, s_assertDisable_GridTree_Insert);
  Assert(ts, s_assertDisable_GridTree_Insert);

  ts->gridPrev = NULL;
  ts->gridNext = node->surfListHead;
  if ( ts->gridNext )
    ts->gridNext->gridPrev = ts;
  node->surfListHead = ts;
  return ts;
}

/*
================
GridTree_Insert

Inserts a surface into the grid tree by finding its leaf node
and prepending it to the node's linked list.
================
*/
TriSurf_t *GridTree_Insert(TriSurf_t *ts)
{
  GridTreeNode_t *leafNode;

  leafNode = GridTree_FindNode(ts->mins, ts->maxs);

  return GridTree_InsertIntoNode(leafNode, ts);
}

int g_mergeCallNum = 0;  /* set in MergeVisGroupList, read in GridTree_Remove */

/*
================
GridTree_Remove

Removes a surface from the grid tree by unlinking it
from its leaf node's doubly-linked list.
================
*/
void GridTree_Remove(TriSurf_t *ts)
{
  GridTreeNode_t *leafNode;

  if ( gridTreeRoot )
  {
    if ( ts->gridNext )
      ts->gridNext->gridPrev = ts->gridPrev;
    if ( ts->gridPrev )
    {
      ts->gridPrev->gridNext = ts->gridNext;
      ts->gridPrev = NULL;
      ts->gridNext = NULL;
    }
    else
    {
      leafNode = GridTree_FindNode(ts->mins, ts->maxs);
      Assert(leafNode, s_assertDisable_GridTree_Remove);
      if ( leafNode->surfListHead == ts )
        leafNode->surfListHead = ts->gridNext;
      ts->gridPrev = NULL;
      ts->gridNext = NULL;
    }
  }
}

/*
================
GridTree_ForEach_r

Recursively traverses the grid tree, calling the callback
on every surface whose AABB intersects the query bounds.
================
*/
void GridTree_ForEach_r( float *queryMins, float *queryMaxs, int nodeIdx, int depth, void (*callback)(TriSurf_t *) )
{
  TriSurf_t *ts;
  int splitAxis;

  while ( 1 )
  {
    GridTreeNode_t *node = &gridTreeRoot[nodeIdx];

    /* call back for each surface in this node that intersects the query
       (save next before callback — callback may unlink the surface) */
    { TriSurf_t *next;
    for ( ts = node->surfListHead; ts; ts = next )
    {
      next = ts->gridNext;
      if ( BoundsIntersect(ts->mins, ts->maxs, queryMins, queryMaxs) )
        callback(ts);
    }
    }

    if ( depth == GRID_TREE_MAX_DEPTH )
      break;

    /* recurse into children that overlap the query */
    splitAxis = g_gridTreeSplitAxis[depth];
    if ( node->min >= queryMins[splitAxis] )
      GridTree_ForEach_r(queryMins, queryMaxs, 2 * nodeIdx + 2, depth + 1, callback);
    if ( node->max > queryMaxs[splitAxis] )
      break;

    /* tail-recurse into the other child */
    ++depth;
    nodeIdx = 2 * nodeIdx + 1;
  }
}

/*
================
GridTree_ForEach

Iterates all grid tree surfaces whose AABB intersects
the query bounds, calling the callback on each.
================
*/
void GridTree_ForEach(float *queryMins, float *queryMaxs, void (*callback)(TriSurf_t *))
{
  GridTree_ForEach_r(queryMins, queryMaxs, 0, 0, callback);
}

/* Native 0x451230: interpolate a point along a segment. */
static void GridTree_SegmentPointAt(float *start, float *end, float fraction, float *out)
{
  out[0] = (end[0] - start[0]) * fraction + start[0];
  out[1] = (end[1] - start[1]) * fraction + start[1];
  out[2] = (end[2] - start[2]) * fraction + start[2];
}

/*
================
GridTree_SegmentTraverse_r

Native 0x450D50.  Tests each intersecting node list, descending directly
while the clipped segment lies on one side of the node's adaptive extrema;
when it spans both extrema, it clips and visits the near child first.
================
*/
int GridTree_SegmentTraverse_r(float *segmentStart, float *segmentEnd,
                                float *clippedStart, float *clippedEnd,
                                int nodeIdx, int depth, int userData,
                                GridTreeSegmentCallback_t callback)
{
  float clippedMins[3];
  float clippedMaxs[3];
  float atNodeMin[3];
  float atNodeMax[3];
  GridTreeNode_t *node;
  TriSurf_t *ts;
  TriSurf_t *next;
  int splitAxis;
  float fraction;

  VectorCopy(clippedStart, clippedMins);
  VectorCopy(clippedStart, clippedMaxs);
  AddPointToBounds(clippedEnd, clippedMins, clippedMaxs);

  for ( ;; )
  {
    for ( ;; )
    {
      node = &gridTreeRoot[nodeIdx];
      for ( ts = node->surfListHead; ts; ts = next )
      {
        next = ts->gridNext;
        if ( BoundsIntersect(ts->mins, ts->maxs, clippedMins, clippedMaxs)
          && !callback(ts, segmentStart, segmentEnd, userData) )
          return 0;
      }

      if ( depth == GRID_TREE_MAX_DEPTH )
        return 1;

      splitAxis = g_gridTreeSplitAxis[depth++];
      if ( node->max < clippedMaxs[splitAxis] )
        break;

      /* Entire clipped segment lies in the native side-1 (low) range. */
      nodeIdx = 2 * nodeIdx + 2;
    }

    if ( node->min > clippedMins[splitAxis] )
      break;

    /* Entire clipped segment lies in the native side-0 (high) range. */
    nodeIdx = 2 * nodeIdx + 1;
  }

  fraction = (node->min - segmentStart[splitAxis])
           / (segmentEnd[splitAxis] - segmentStart[splitAxis]);
  GridTree_SegmentPointAt(segmentStart, segmentEnd, fraction, atNodeMin);
  atNodeMin[splitAxis] = node->min;

  fraction = (node->max - segmentStart[splitAxis])
           / (segmentEnd[splitAxis] - segmentStart[splitAxis]);
  GridTree_SegmentPointAt(segmentStart, segmentEnd, fraction, atNodeMax);
  atNodeMax[splitAxis] = node->max;

  if ( segmentEnd[splitAxis] <= segmentStart[splitAxis] )
  {
    if ( !GridTree_SegmentTraverse_r(segmentStart, segmentEnd,
                                     clippedStart, atNodeMax,
                                     2 * nodeIdx + 1, depth, userData, callback) )
      return 0;
    return GridTree_SegmentTraverse_r(segmentStart, segmentEnd,
                                       atNodeMin, clippedEnd,
                                       2 * nodeIdx + 2, depth, userData, callback);
  }

  if ( !GridTree_SegmentTraverse_r(segmentStart, segmentEnd,
                                   clippedStart, atNodeMin,
                                   2 * nodeIdx + 2, depth, userData, callback) )
    return 0;
  return GridTree_SegmentTraverse_r(segmentStart, segmentEnd,
                                     atNodeMax, clippedEnd,
                                     2 * nodeIdx + 1, depth, userData, callback);
}

/* Native 0x450D20. */
int GridTree_SegmentTraverse(float *segmentStart, float *segmentEnd, int userData,
                              GridTreeSegmentCallback_t callback)
{
  return GridTree_SegmentTraverse_r(segmentStart, segmentEnd,
                                    segmentStart, segmentEnd,
                                    0, 0, userData, callback);
}

/*
================
GridTree_CountIntersecting_r

Recursively counts surfaces intersecting the query bounds,
with early-out when count exceeds maxCount.
================
*/
int GridTree_CountIntersecting_r(float *queryMins, float *queryMaxs, int nodeIdx, int depth, int maxCount)
{
  GridTreeNode_t *node = &gridTreeRoot[nodeIdx];
  TriSurf_t *ts;
  int count = 0, splitAxis;

  for ( ts = node->surfListHead; ts; ts = ts->gridNext )
  {
    if ( BoundsIntersect(ts->mins, ts->maxs, queryMins, queryMaxs) )
      if ( ++count > maxCount )
        return count;
  }

  if ( depth < GRID_TREE_MAX_DEPTH )
  {
    splitAxis = g_gridTreeSplitAxis[depth];
    if ( node->min >= queryMins[splitAxis] )
    {
      count += GridTree_CountIntersecting_r(queryMins, queryMaxs, 2 * nodeIdx + 2, depth + 1, maxCount - count);
      if ( count > maxCount )
        return count;
    }
    if ( node->max <= queryMaxs[splitAxis] )
      count += GridTree_CountIntersecting_r(queryMins, queryMaxs, 2 * nodeIdx + 1, depth + 1, maxCount - count);
  }
  return count;
}

/*
================
GridTree_CountIntersecting

Counts grid tree surfaces whose AABB intersects the query
bounds, with early-out when count exceeds maxCount.
================
*/
int GridTree_CountIntersecting(float *queryMins, float *queryMaxs, int maxCount)
{
  return GridTree_CountIntersecting_r(queryMins, queryMaxs, 0, 0, maxCount);
}

/*
================
GridTree_Init

Allocates the native 12-byte grid-node carrier.
================
*/
GridTreeNode_t *GridTree_Init(void)
{
  Assert(!gridTreeRoot, s_assertDisable_GridTree_Init);
  /* 2^21 - 1 nodes, matching cod4map's 0x17FFFF4-byte allocation. */
  /* This TU is compiled as C by the legacy project.  malloc/free provide the
     same contiguous carrier lifetime as native new[]/delete[] here. */
  gridTreeRoot = (GridTreeNode_t *)malloc(sizeof(GridTreeNode_t) * GRID_TREE_MAX_NODES);
  return gridTreeRoot;
}

/*
================
GridTree_Shutdown

Frees the grid tree root array and resets the pointer.
================
*/
void GridTree_Shutdown(void)
{
  Assert(gridTreeRoot, s_assertDisable_GridTree_Shutdown);
  free(gridTreeRoot);
  gridTreeRoot = 0;
}

/*
================
SetGridDivisionPoints_r

Recursively initializes native per-node lower/upper traversal bounds.
================
*/
float *SetGridDivisionPoints_r(int nodeIdx, float *mins, float *maxs, int depth)
{
  int splitAxis;
  float childMins[3], childMaxs[3];

  splitAxis = g_gridTreeSplitAxis[depth];
  gridTreeRoot[nodeIdx].min = MIDF(mins[splitAxis], maxs[splitAxis]);
  gridTreeRoot[nodeIdx].max = gridTreeRoot[nodeIdx].min;
  gridTreeRoot[nodeIdx].surfListHead = NULL;

  if ( depth != GRID_TREE_MAX_DEPTH )
  {
    VectorCopy(mins, childMins);
    VectorCopy(maxs, childMaxs);
    childMins[splitAxis] = gridTreeRoot[nodeIdx].max;
    childMaxs[splitAxis] = gridTreeRoot[nodeIdx].min;
    SetGridDivisionPoints_r(2 * nodeIdx + 2, mins, childMaxs, depth + 1);
    return SetGridDivisionPoints_r(2 * nodeIdx + 1, childMins, maxs, depth + 1);
  }
  return mins;
}

/*
================
SetGridDivisionPoints

Sets grid tree split distances from the given world bounds
by recursively bisecting along alternating axes.
================
*/
float *SetGridDivisionPoints(float *mins, float *maxs)
{
  Assert(gridTreeRoot, s_assertDisable_SetGridDivisionPoints);
  VectorCopy(mins, gridTreeBoundsMins);
  VectorCopy(maxs, gridTreeBoundsMaxs);
  return SetGridDivisionPoints_r(0, mins, maxs, 0);
}

/* Native 0x450B10.  Before rebuilding the adaptive traversal bounds, move
   every child list back into the root list without changing list order inside
   a child. */
static void GridTree_CollapseLists(void)
{
  int nodeIdx;

  for ( nodeIdx = 1; nodeIdx < GRID_TREE_MAX_NODES; ++nodeIdx )
  {
    TriSurf_t *head = gridTreeRoot[nodeIdx].surfListHead;
    TriSurf_t *tail;

    if ( !head )
      continue;

    gridTreeRoot[nodeIdx].surfListHead = NULL;
    tail = head;
    while ( tail->gridNext )
      tail = tail->gridNext;

    tail->gridNext = gridTreeRoot[0].surfListHead;
    if ( tail->gridNext )
      tail->gridNext->gridPrev = tail;
    gridTreeRoot[0].surfListHead = head;
  }

  Assert(!gridTreeRoot[0].surfListHead || !gridTreeRoot[0].surfListHead->gridPrev,
         s_assertDisable_GridTree_Remove);
}

/* Native 0x450760.  Repartition a collapsed list, retaining straddling
   surfaces at the current node and tightening its traversal bounds around
   the surfaces moved into either child. */
static void GridTree_RedistributeNode(int nodeIdx, float *mins, float *maxs, int depth)
{
  GridTreeNode_t *node = &gridTreeRoot[nodeIdx];
  GridTreeNode_t *side1;
  GridTreeNode_t *side0;
  int splitAxis;
  float midpoint;
  float slack;
  TriSurf_t *ts;
  TriSurf_t *next;
  float highBounds[3];
  float lowBounds[3];

  if ( !node->surfListHead )
    return;

  side1 = &gridTreeRoot[2 * nodeIdx + 2];
  side0 = &gridTreeRoot[2 * nodeIdx + 1];
  splitAxis = g_gridTreeSplitAxis[depth];
  midpoint = MIDF(mins[splitAxis], maxs[splitAxis]);
  slack = (maxs[splitAxis] - mins[splitAxis])
        * ((float)depth * 0.007894736714661121f + 0.1000000014901161f);

  node->min = midpoint;
  node->max = midpoint;

  for ( ts = node->surfListHead; ts; ts = next )
  {
    float lowOffset;
    float highOffset;

    next = ts->gridNext;
    lowOffset = ts->mins[splitAxis] - midpoint;
    highOffset = ts->maxs[splitAxis] - midpoint;

    if ( highOffset <= -lowOffset )
    {
      if ( slack > highOffset )
      {
        GridTree_RemoveFromNode(node, ts);
        GridTree_InsertIntoNode(side1, ts);
        if ( ts->maxs[splitAxis] > node->min )
          node->min = ts->maxs[splitAxis];
      }
    }
    else if ( slack > -lowOffset )
    {
      GridTree_RemoveFromNode(node, ts);
      GridTree_InsertIntoNode(side0, ts);
      if ( ts->mins[splitAxis] < node->max )
        node->max = ts->mins[splitAxis];
    }
  }

  if ( depth == GRID_TREE_MAX_DEPTH - 1 )
    return;

  VectorCopy(maxs, highBounds);
  highBounds[splitAxis] = node->min;
  VectorCopy(mins, lowBounds);
  lowBounds[splitAxis] = node->max;
  GridTree_RedistributeNode(2 * nodeIdx + 2, mins, highBounds, depth + 1);
  GridTree_RedistributeNode(2 * nodeIdx + 1, lowBounds, maxs, depth + 1);
}

/* Native 0x450740. */
void GridTree_RebuildPartitionBounds(void)
{
  GridTree_CollapseLists();
  GridTree_RedistributeNode(0, gridTreeBoundsMins, gridTreeBoundsMaxs, 0);
}

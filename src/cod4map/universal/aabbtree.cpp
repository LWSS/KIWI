/*
aabbtree.c — AABB tree builder for collision and shadow partitioning

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

int    g_aabbNodeCount = 0;
float *g_aabbSortEqual = NULL;
float *g_aabbSortMaxs = NULL;
float *g_aabbSortMins = NULL;

char g_aabbAssert_firstChild = '\0';
char g_aabbAssert_itemCount = '\0';
char g_aabbAssert_sideAll = '\0';
char g_aabbAssert_sideBack = '\0';
char g_aabbAssert_sideFront = '\0';
char g_aabbAssert_sideOn = '\0';
char g_aabbAssert_sideSplit = '\0';


/*
================
CompareFunction

qsort comparator for float values.
================
*/
int CompareFunction(const void *a, const void *b)
{
  double diff;

  diff = *(const float *)a - *(const float *)b;
  if ( diff >= 0.0 )
    return diff > 0.0;
  else
    return -1;
}

/*
================
AabbAllocNode

Allocates the next AABB tree node from the builder's node buffer.
Returns pointer to the newly allocated AabbTreeNode_t.
Fatally errors if the node buffer is full.
================
*/
AabbTreeNode_t *AabbAllocNode(AabbTreeBuilder_t *builder)
{
  int nodeIdx;

  nodeIdx = g_aabbNodeCount;
  if ( g_aabbNodeCount == builder->maxNodes )
  {
    Com_Error("More than %i AABB nodes needed\n", builder->maxNodes);
    nodeIdx = g_aabbNodeCount;
  }
  g_aabbNodeCount = nodeIdx + 1;
  return &builder->nodes[nodeIdx];
}

/*
================
AabbFindBestSplitPlane

Finds the best axis and position to split AABB items.

Algorithm:
  1. Compute overall bounds of all items and per-axis scale factors
  2. For each axis (X=0, Y=1, Z=2):
     a. Categorize items: non-degenerate (min != max) -> sortMins/sortMaxs,
        degenerate (min == max) -> sortEqual
     b. Sort all three buffers
     c. Sweep sorted positions tracking front/back/split/on counts
     d. Score = itemCount + axisScale - 4*splitCount - onCount - |front - back|
  3. Return best axis and split position

Returns TRUE if a valid split was found, FALSE otherwise.
================
*/
qboolean AabbFindBestSplitPlane(float *itemMins, float *itemMaxs, int *indices, int itemCount, int *outBestAxis, float *outSplitPos)
{
  int i, j, axis;
  int idx;
  int longestAxis;
  int sortCount;
  int equalCount;
  int axisScale[3];
  float overallMins[3];
  float overallMaxs[3];
  float candidateRange;
  float minVal, maxVal;
  int bestScore;
  int minsIdx, maxsIdx, equalIdx;
  int minsStep, minsCount, splitTemp, equalStep;
  int prevEqualStep;
  float currentSplitPos, nextSplitPos;
  int sideBackCount, sideFrontCount, sideOnCount, sideSplitCount;
  int score;
  float splitDist;
  int splitBonus;

  ClearBounds(overallMins, overallMaxs);
  for ( i = 0; i < itemCount; ++i )
    AddBoundsToBounds(&itemMins[indices[i] * 3], &itemMaxs[indices[i] * 3], overallMins, overallMaxs);

  /* boolean trick: 0 if Y extent >= X extent, 1 if X extent > Y extent; then check Z */
  longestAxis = (overallMaxs[0] - overallMins[0]) > (overallMaxs[1] - overallMins[1]);
  if ( (overallMaxs[longestAxis] - overallMins[longestAxis]) > (overallMaxs[2] - overallMins[2]) )
    longestAxis = 2;

  for ( j = 0; j < 3; ++j )
  {
    candidateRange = (overallMaxs[j] - overallMins[j] + 1.0) * 10.0 / (overallMaxs[longestAxis] - overallMins[longestAxis] + 1.0);
    axisScale[j] = RoundPositiveFloatToInt(candidateRange);
  }

  bestScore = INT_MIN;
  for ( axis = 0; axis < 3; ++axis )
  {
    /* Categorize items: degenerate (min==max) go to sortEqual, others to sortMins/sortMaxs */
    sortCount = 0;
    equalCount = 0;
    for ( i = 0; i < itemCount; ++i )
    {
      idx = indices[i];
      minVal = itemMins[3 * idx + axis];
      maxVal = itemMaxs[3 * idx + axis];
      if ( minVal == maxVal )
      {
        g_aabbSortEqual[equalCount] = minVal;
        equalCount++;
      }
      else
      {
        g_aabbSortMins[sortCount] = minVal;
        g_aabbSortMaxs[sortCount] = maxVal;
        sortCount++;
      }
    }

    qsort(g_aabbSortMins, sortCount, sizeof(*g_aabbSortMins), CompareFunction);
    qsort(g_aabbSortMaxs, sortCount, sizeof(*g_aabbSortMaxs), CompareFunction);
    qsort(g_aabbSortEqual, equalCount, sizeof(*g_aabbSortEqual), CompareFunction);

    /* Sweep sorted positions to find best split */
    sideFrontCount = 0;
    sideSplitCount = 0;
    sideOnCount = 0;
    sideBackCount = itemCount;
    minsIdx = 0;
    maxsIdx = 0;
    equalIdx = 0;
    minsStep = 0;
    prevEqualStep = 0;

    if ( g_aabbSortEqual[0] - g_aabbSortMins[0] < 0.0 )
      nextSplitPos = g_aabbSortEqual[0];
    else
      nextSplitPos = g_aabbSortMins[0];

    while ( nextSplitPos < FLT_MAX )
    {
      currentSplitPos = nextSplitPos;
      sideBackCount -= minsStep;
      sideSplitCount += minsStep;
      minsStep = 0;
      nextSplitPos = FLT_MAX;

      /* Advance mins cursor past currentSplitPos */
      if ( minsIdx < sortCount )
      {
        minsCount = 0;
        while ( minsIdx + minsCount < sortCount && g_aabbSortMins[minsIdx + minsCount] == currentSplitPos )
          ++minsCount;
        minsIdx += minsCount;
        minsStep = minsCount;
        if ( minsIdx < sortCount && g_aabbSortMins[minsIdx] < FLT_MAX )
          nextSplitPos = g_aabbSortMins[minsIdx];
      }

      /* Advance maxs cursor past currentSplitPos */
      if ( maxsIdx < sortCount )
      {
        splitTemp = sideSplitCount;
        while ( maxsIdx < sortCount && g_aabbSortMaxs[maxsIdx] == currentSplitPos )
        {
          --splitTemp;
          ++maxsIdx;
          ++sideFrontCount;
        }
        sideSplitCount = splitTemp;
        if ( maxsIdx < sortCount && g_aabbSortMaxs[maxsIdx] < nextSplitPos )
          nextSplitPos = g_aabbSortMaxs[maxsIdx];
      }

      /* Advance equal cursor past currentSplitPos */
      sideOnCount -= prevEqualStep;
      sideFrontCount += prevEqualStep;
      equalStep = 0;
      prevEqualStep = 0;
      if ( equalIdx < equalCount )
      {
        while ( equalIdx + equalStep < equalCount && g_aabbSortEqual[equalIdx + equalStep] == currentSplitPos )
          ++equalStep;
        equalIdx += equalStep;
        prevEqualStep = equalStep;
      }
      sideOnCount += equalStep;
      sideBackCount -= equalStep;
      if ( equalIdx < equalCount && g_aabbSortEqual[equalIdx] < nextSplitPos )
        nextSplitPos = g_aabbSortEqual[equalIdx];

      Assert(sideFrontCount + sideBackCount + sideSplitCount + sideOnCount == itemCount, g_aabbAssert_sideAll);
      Assert(sideFrontCount >= 0, g_aabbAssert_sideFront);
      Assert(sideBackCount >= 0, g_aabbAssert_sideBack);
      Assert(sideSplitCount >= 0, g_aabbAssert_sideSplit);
      Assert(sideOnCount >= 0, g_aabbAssert_sideOn);

      if ( sideFrontCount > 1 && sideBackCount > 1 )
      {
        score = itemCount + axisScale[axis] - 4 * sideSplitCount - sideOnCount - abs(sideFrontCount - sideBackCount);
        if ( !sideOnCount && !sideSplitCount && !minsStep )
        {
          splitDist = nextSplitPos - currentSplitPos;
          splitBonus = xs_RoundToInt(splitDist + FISTP_BIAS);
          score += splitBonus;
        }
        if ( score > bestScore )
        {
          bestScore = score;
          *outBestAxis = axis;
          if ( sideOnCount || sideSplitCount || minsStep )
            *outSplitPos = currentSplitPos;
          else
            *outSplitPos = MIDF(currentSplitPos, nextSplitPos);
        }
      }
    }
  }
  return bestScore != INT_MIN;
}

/*
================
AabbPartitionItems

Partitions items into front/back groups around a split plane.
Uses AabbFindBestSplitPlane to find the best axis/position, then does a
quicksort-like swap to separate items. Falls back to volume-based heuristic
for degenerate cases.

Returns TRUE if partition succeeded, FALSE if no valid split found.
================
*/
qboolean AabbPartitionItems(int *indices, int itemCount, AabbTreeBuilder_t *builder, int *outFrontCount, int *outMidStart)
{
  int frontIdx, backIdx, midScanIdx;
  int frontItem, backItem, midItem, temp;
  int minPartSize;
  float frontMins[3], frontMaxs[3];
  float backMins[3], backMaxs[3];
  float tryFrontMins[3], tryFrontMaxs[3];
  float tryBackMins[3], tryBackMaxs[3];
  double frontVol, backVol, newFrontVol, newBackVol;
  int splitAxis;
  float splitPos;
  float *minsBuf, *maxsBuf;

  minsBuf = builder->itemMins;
  maxsBuf = builder->itemMaxs;
  if ( !AabbFindBestSplitPlane(minsBuf, maxsBuf, indices, itemCount, &splitAxis, &splitPos) )
    return 0;

  ClearBounds(frontMins, frontMaxs);
  ClearBounds(backMins, backMaxs);
  frontIdx = 0;
  backIdx = itemCount - 1;
  if ( backIdx < 0 )
    return 0;

  /* Three-way partition: items are classified as front (below split),
     back (above split), or straddling. Uses quicksort-style swaps
     with a mid-scan fallback for unresolvable straddle pairs. */

scan_pass:
  if ( frontIdx > backIdx )
    goto scan_done;

  /* advance frontIdx past items clearly in front */
  while ( frontIdx <= backIdx )
  {
    frontItem = indices[frontIdx];
    if ( maxsBuf[3 * frontItem + splitAxis] > (double)splitPos || minsBuf[3 * frontItem + splitAxis] >= (double)splitPos )
      break;
    AddBoundsToBounds(&minsBuf[3 * frontItem], &maxsBuf[3 * frontItem], frontMins, frontMaxs);
    ++frontIdx;
  }
  if ( frontIdx > backIdx )
    goto scan_done;

  /* retreat backIdx past items clearly in back */
  while ( frontIdx <= backIdx )
  {
    backItem = indices[backIdx];
    if ( minsBuf[3 * backItem + splitAxis] < (double)splitPos || maxsBuf[3 * backItem + splitAxis] <= (double)splitPos )
      break;
    AddBoundsToBounds(&minsBuf[3 * backItem], &maxsBuf[3 * backItem], backMins, backMaxs);
    --backIdx;
  }
  if ( frontIdx > backIdx )
    goto scan_done;

  /* check if front and back items can be directly swapped */
  frontItem = indices[frontIdx];
  if ( minsBuf[3 * frontItem + splitAxis] < (double)splitPos || maxsBuf[3 * frontItem + splitAxis] <= (double)splitPos )
  {
    backItem = indices[backIdx];
    if ( maxsBuf[3 * backItem + splitAxis] > (double)splitPos || minsBuf[3 * backItem + splitAxis] >= (double)splitPos )
      goto mid_scan; /* both genuinely straddling -- need mid-scan */
  }
  indices[frontIdx] = indices[backIdx];
  indices[backIdx] = frontItem;
  goto scan_pass;

mid_scan:

  /* scan from frontIdx for an item that resolves the straddle */
  midScanIdx = frontIdx;
  if ( frontIdx >= backIdx )
    goto scan_done;

  for ( ;; )
  {
    midItem = indices[midScanIdx];

    /* found a clearly-back item -- swap it to back position */
    if ( minsBuf[3 * midItem + splitAxis] >= (double)splitPos && maxsBuf[3 * midItem + splitAxis] > (double)splitPos )
    {
      temp = indices[midScanIdx];
      indices[midScanIdx] = indices[backIdx];
      indices[backIdx] = temp;
      goto scan_pass;
    }

    /* found a clearly-front item -- swap straddler out of frontIdx */
    if ( maxsBuf[3 * midItem + splitAxis] <= (double)splitPos && minsBuf[3 * midItem + splitAxis] < (double)splitPos )
    {
      temp = indices[midScanIdx];
      indices[midScanIdx] = frontItem;
      indices[frontIdx] = temp;
      goto scan_pass;
    }

    /* still straddling -- advance scan */
    if ( ++midScanIdx >= backIdx )
      goto scan_done;
  }

scan_done:

  /* volume-based heuristic for small or uneven partitions */
  if ( frontIdx > backIdx )
    goto finish;
  minPartSize = builder->minPartitionSize;
  if ( frontIdx >= minPartSize && backIdx - frontIdx + 1 >= minPartSize && itemCount - backIdx - 1 >= minPartSize )
    goto finish;

vol_pass:

  /* compute volume cost of assigning boundary item to front vs back */
  VectorCopy(frontMins, tryFrontMins);
  VectorCopy(frontMaxs, tryFrontMaxs);
  frontItem = indices[frontIdx];
  AddBoundsToBounds(&minsBuf[3 * frontItem], &maxsBuf[3 * frontItem], tryFrontMins, tryFrontMaxs);
  newFrontVol = BoundsVolume(tryFrontMins, tryFrontMaxs);
  frontVol = BoundsVolume(frontMins, frontMaxs);

  VectorCopy(backMins, tryBackMins);
  VectorCopy(backMaxs, tryBackMaxs);
  AddBoundsToBounds(&minsBuf[3 * frontItem], &maxsBuf[3 * frontItem], tryBackMins, tryBackMaxs);
  backVol = BoundsVolume(backMins, backMaxs);
  newBackVol = BoundsVolume(tryBackMins, tryBackMaxs);

  if ( newBackVol - backVol < newFrontVol - frontVol )
  {
    /* back is cheaper -- pull items from back side */
    if ( frontIdx > backIdx )
      goto finish;
    do
    {
      VectorCopy(backMins, tryBackMins);
      VectorCopy(backMaxs, tryBackMaxs);
      backItem = indices[backIdx];
      AddBoundsToBounds(&minsBuf[3 * backItem], &maxsBuf[3 * backItem], tryBackMins, tryBackMaxs);
      newBackVol = BoundsVolume(tryBackMins, tryBackMaxs);
      backVol = BoundsVolume(backMins, backMaxs);

      VectorCopy(frontMins, tryFrontMins);
      VectorCopy(frontMaxs, tryFrontMaxs);
      AddBoundsToBounds(&minsBuf[3 * backItem], &maxsBuf[3 * backItem], tryFrontMins, tryFrontMaxs);
      newFrontVol = BoundsVolume(tryFrontMins, tryFrontMaxs);
      frontVol = BoundsVolume(frontMins, frontMaxs);

      if ( newFrontVol - frontVol < newBackVol - backVol )
        break;
      AddBoundsToBounds(&minsBuf[3 * indices[backIdx]], &maxsBuf[3 * indices[backIdx]], backMins, backMaxs);
      --backIdx;
    }
    while ( frontIdx <= backIdx );

    if ( frontIdx > backIdx )
      goto finish;
    if ( frontIdx == backIdx )
    {
      /* one item left -- assign to smaller partition */
      if ( 2 * frontIdx < itemCount )
        ++frontIdx;
      else
        --backIdx;
      goto finish;
    }
    temp = indices[frontIdx];
    indices[frontIdx] = indices[backIdx];
    indices[backIdx] = temp;
    ++frontIdx;
    --backIdx;
    if ( frontIdx <= backIdx )
      goto vol_pass;
    goto finish;
  }

  /* front is cheaper */
  AddBoundsToBounds(&minsBuf[3 * indices[frontIdx]], &maxsBuf[3 * indices[frontIdx]], frontMins, frontMaxs);
  if ( ++frontIdx <= backIdx )
    goto vol_pass;

finish:
  if ( !frontIdx || frontIdx == itemCount )
    return 0;

  *outFrontCount = frontIdx;
  *outMidStart = backIdx + 1;
  return 1;
}

/*
================
AabbBuildSubtree

Creates child nodes for a tree partition.
If itemCount > minLeafItems and partition succeeds, creates 2-3 children:
  - Front: items [0 .. frontCount-1]
  - Middle: items [frontCount .. midStart-1] (only if frontCount < midStart)
  - Back: items [midStart .. count-1]
Otherwise creates a single leaf node.
Returns pointer to the last allocated node.
================
*/
AabbTreeNode_t *AabbBuildSubtree(AabbTreeBuilder_t *builder, AabbTreeNode_t *parent, int *indices, int offset, int count)
{
  int frontCount;
  AabbTreeNode_t *node;
  int midStart;
  qboolean hasMidPart;
  AabbTreeNode_t *midNode;
  AabbTreeNode_t *backNode;
  int partFrontCount;
  int partMidStart;

  if ( count > builder->minLeafItems && AabbPartitionItems(indices + offset, count, builder, &partMidStart, &partFrontCount) )
  {
    /* Allocate front child node */
    node = AabbAllocNode(builder);
    frontCount = partMidStart;
    midStart = partFrontCount;
    hasMidPart = partMidStart < partFrontCount;
    node->firstItem = offset + parent->firstItem;
    node->itemCount = frontCount;

    /* Allocate middle child node if front < mid */
    if ( hasMidPart )
    {
      midNode = AabbAllocNode(builder);
      midNode->firstItem = offset + frontCount + parent->firstItem;
      midNode->itemCount = midStart - frontCount;
    }

    /* Allocate back child node */
    backNode = AabbAllocNode(builder);
    backNode->firstItem = offset + midStart + parent->firstItem;
    backNode->itemCount = count - midStart;
  }
  else
  {
    /* Leaf node - no partition possible */
    backNode = AabbAllocNode(builder);
    backNode->firstItem = offset + parent->firstItem;
    backNode->itemCount = count;
  }
  return backNode;
}

/*
================
AabbBuildTree_r

Recursively builds the AABB tree from a partitioned node.
If itemCount > minLeafItems and a partition is found:
  - Creates child nodes via AabbBuildSubtree (2-3 children: front, optional middle, back)
  - Recursively builds each child subtree
Otherwise: the node remains a leaf (stores items directly).
================
*/
int AabbBuildTree_r(AabbTreeNode_t *node, AabbTreeBuilder_t *builder, int *indices)
{
  int frontCount, midStart;
  int i;
  AabbTreeNode_t *children;

  Assert(node->itemCount, g_aabbAssert_itemCount);
  node->firstChild = g_aabbNodeCount;
  node->childCount = 0;

  if ( node->itemCount <= builder->minLeafItems )
    return 0;

  if ( !AabbPartitionItems(indices, node->itemCount, builder, &frontCount, &midStart) )
    return 0;

  AssertFatal(node->firstChild == g_aabbNodeCount, g_aabbAssert_firstChild);
  children = &builder->nodes[g_aabbNodeCount];

  /* create child nodes: front, optional middle, back */
  AabbBuildSubtree(builder, node, indices, 0, frontCount);
  if ( frontCount < midStart )
    AabbBuildSubtree(builder, node, indices, frontCount, midStart - frontCount);
  AabbBuildSubtree(builder, node, indices, midStart, node->itemCount - midStart);
  node->childCount = g_aabbNodeCount - node->firstChild;

  /* recurse into each child */
  for ( i = 0; i < node->childCount; i++ )
    AabbBuildTree_r(&children[i], builder, indices + children[i].firstItem - node->firstItem);

  return node->childCount;
}


/*
================
AabbBuildTree

Entry point for AABB tree construction.
1. Allocates sort buffers (stack if <= 0x8000 items, heap otherwise)
2. Initializes index array [0, 1, 2, ..., itemCount-1]
3. Sets up root node and calls AabbBuildTree_r recursively
4. Reorders item data (and mins/maxs if present) to match tree node order
5. Returns total number of tree nodes
================
*/
#define AABB_STACK_ITEMS 0x1000 /* native 0x465748 stack/heap threshold */

int AabbBuildTree(AabbTreeBuilder_t *builder)
{
  int itemCount;
  int i;
  int *indexBuf;
  int heapAllocated;
  char *itemDataCopy;
  float *boundsCopy;
  float sortBufMinsLocal[AABB_STACK_ITEMS];
  float sortBufMaxsLocal[AABB_STACK_ITEMS];
  float sortBufEqualLocal[AABB_STACK_ITEMS];
  int indexBufLocal[AABB_STACK_ITEMS];

  itemCount = builder->itemCount;
  if ( itemCount > AABB_STACK_ITEMS )
  {
    indexBuf = malloc(itemCount * sizeof(*indexBuf));
    g_aabbSortMins = malloc(itemCount * sizeof(*g_aabbSortMins));
    g_aabbSortMaxs = malloc(itemCount * sizeof(*g_aabbSortMaxs));
    g_aabbSortEqual = malloc(itemCount * sizeof(*g_aabbSortEqual));
    heapAllocated = 1;
  }
  else
  {
    indexBuf = indexBufLocal;
    g_aabbSortMins = sortBufMinsLocal;
    g_aabbSortMaxs = sortBufMaxsLocal;
    g_aabbSortEqual = sortBufEqualLocal;
    heapAllocated = 0;
  }

  for ( i = 0; i < itemCount; ++i )
    indexBuf[i] = i;

  /* Initialize root node */
  builder->nodes[0].firstItem = 0;
  builder->nodes[0].itemCount = itemCount;
  g_aabbNodeCount = 1;
  AabbBuildTree_r(&builder->nodes[0], builder, indexBuf);

  /* Reorder item data to match tree node order */
  itemDataCopy = malloc(itemCount * builder->itemStride);
  memcpy(itemDataCopy, builder->itemData, itemCount * builder->itemStride);
  for ( i = 0; i < itemCount; ++i )
  {
    memcpy(
      (char *)builder->itemData + i * builder->itemStride,
      itemDataCopy + builder->itemStride * indexBuf[i],
      builder->itemStride);
  }
  free(itemDataCopy);

  /* Reorder mins/maxs arrays if separate bounds data exists */
  if ( builder->hasBoundsData )
  {
    boundsCopy = malloc(3 * sizeof(*boundsCopy) * itemCount);

    memcpy(boundsCopy, builder->itemMins, 3 * sizeof(*boundsCopy) * itemCount);
    for ( i = 0; i < itemCount; ++i )
      VectorCopy(&boundsCopy[3 * indexBuf[i]], &builder->itemMins[3 * i]);

    memcpy(boundsCopy, builder->itemMaxs, 3 * sizeof(*boundsCopy) * itemCount);
    for ( i = 0; i < itemCount; ++i )
      VectorCopy(&boundsCopy[3 * indexBuf[i]], &builder->itemMaxs[3 * i]);

    free(boundsCopy);
  }

  if ( heapAllocated )
  {
    free(indexBuf);
    free(g_aabbSortMins);
    free(g_aabbSortMaxs);
    free(g_aabbSortEqual);
  }
  return g_aabbNodeCount;
}

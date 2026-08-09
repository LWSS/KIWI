/*
facebsp.c — Face BSP tree building

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

char   g_bspTreeByte0;
float  k_classifyDistSqThreshold[3] = { 0.040000003f, 0.0025000002f, 0.0 };
float  k_classifyDistTolerance[3] = { 0.0099999998f, 0.02f, 0.1f };
float  k_classifyDotThresholds[3] = { 0.96592581f, 0.99144489f, 1.0 };
float  k_classifyMaxExtent[3] = { 1.0, 0.2f, 0.0 };
float *splitCoplanarArray;
float *splitMaxsArray;
float *splitMinsArray;

char s_assertDisable_SelectSplitPlane_BlockSubdivision;
char s_assertDisable_SelectSplitPlane_BlockSubdivision;
char s_assertDisable_SelectSplitPlane_BlockSubdivision;
char s_assertDisable_SelectSplitPlane_DirectEval;
char s_assertDisable_SelectSplitPlane_EvaluateAxes;
char s_assertDisable_SelectSplitPlane_EvaluateAxes;
char s_assertDisable_SelectSplitPlane_EvaluateAxes;
char s_assertDisable_SelectSplitPlane_EvaluateAxes;
char s_assertDisable_SelectSplitPlane_EvaluateAxes;
char s_assertDisable_SelectSplitPlane_EvaluateAxes;
char s_assertDisable_SelectSplitPlane_EvaluateAxes;
char s_assertDisable_SelectSplitPlane_EvaluateAxes;


/*
================
FloatCompare

qsort comparison callback for float arrays.
================
*/
int FloatCompare(const void *va, const void *vb)
{
  double diff;

  diff = *(const float *)va - *(const float *)vb;
  if ( diff >= 0.0 )
    return diff > 0.0;
  else
    return -1;
}

/*
================
SelectSplitPlane_Score

CoD4 ranks a valid split by strongly preferring coplanar faces, then fewer
splits, then a balanced front/back distribution.  The CoD2 donor used a
different (much weaker) facing-minus-splits heuristic.
================
*/
static int SelectSplitPlane_Score(int coplanarCount, int frontCount, int backCount, int splitCount)
{
  if ( !frontCount || !backCount )
    return INT_MIN;

  return (coplanarCount << 7) - (splitCount << 8) - abs(frontCount - backCount);
}

/* CoD4 0x414F10: every temporary face record is a cleared 16-byte object. */
static Face_t *AllocFace(void)
{
  Face_t *face = (Face_t *)malloc(sizeof(*face));
  memset(face, 0, sizeof(*face));
  return face;
}

/* CoD4 0x414F40: face ownership includes its winding. */
static void FreeFace(Face_t *face)
{
  if ( face->w )
    FreeWinding(face->w);
  free(face);
}

/*
================
CollectBrushWindings

Collect windings from structural brushes for BSP tree construction.
================
*/
Face_t *CollectBrushWindings(Brush_t *brushList)
{
  Brush_t *b;
  Face_t *f;
  Face_t *list;
  int i;

  list = NULL;

  for ( b = brushList; b; b = b->next )
  {
    if ( b->detail && !b->forceVisible )
      continue;

    for ( i = 0; i < b->numSides; i++ )
    {
      Winding_t *w = b->sides[i].visibleHull;
      if ( !w )
        continue;

      if ( b->detail && b->sides[i].shaderInfo->surfaceFlags >= 0 )
        continue;

      /* create face reference for this side's winding */
      f = AllocFace();
      f->next = list;
      f->planenum = b->sides[i].planenum & ~1;
      f->checked = 0;
      f->w = CopyWinding(w);
      list = f;
    }
  }

  return list;
}

/*
================
CollectDetailWindings

Collects winding references from detail brushes.
Similar to CollectBrushWindings but filters for detail/non-structural brushes.
Returns a linked list of winding references.
================
*/
Face_t *CollectDetailWindings(Brush_t *brushList)
{
  Brush_t *b;
  Face_t *f;
  Face_t *list;
  Winding_t *w;
  int i;

  list = NULL;

  for ( b = brushList; b; b = b->next )
  {
    /* skip non-detail brushes (unless forced visible) */
    if ( b->detail && !b->forceVisible )
      continue;

    for ( i = 0; i < b->numSides; i++ )
    {
      w = b->sides[i].visibleHull;
      if ( !w )
        continue;
	
      /* detail brushes only include sides with negative surface flags */
      if ( b->detail && b->sides[i].shaderInfo->surfaceFlags >= 0 )
        continue;

      f = AllocFace();
      f->next = list;
      f->planenum = b->sides[i].planenum & ~1;
      f->checked = 0;
      f->w = CopyWinding(w);
      list = f;
    }
  }

  return list;
}

/*
================
ClassifyWindingAgainstPlane

Determines how a winding relates to a split plane.
Returns: 0=FRONT, 1=BACK, 3=SPLIT, CLASSIFY_COPLANAR=skip
================
*/
#define CLASSIFY_COPLANAR 0x80000000
int ClassifyWindingAgainstPlane(Face_t *face, float *planeNormal, float planeDist, int axis)
{
  float minDist, maxDist;
  float negMax;

  WindingPlaneDistExtent(face->w, planeNormal, planeDist, &minDist, &maxDist);
  negMax = -k_classifyMaxExtent[axis];

  /* coplanar or near-coplanar — check dot product threshold */
  if ( minDist > negMax && maxDist < k_classifyMaxExtent[axis]
    || (minDist * minDist < k_classifyDistSqThreshold[axis] || maxDist * maxDist < k_classifyDistSqThreshold[axis])
    && DotProduct210(MAP_PLANE(face->planenum)->normal, planeNormal) > k_classifyDotThresholds[axis] )
    return CLASSIFY_COPLANAR;

  /* entirely in front */
  if ( -k_classifyDistTolerance[axis] < minDist )
    return 0;

  /* entirely behind */
  if ( maxDist < k_classifyDistTolerance[axis] )
    return 1;

  /* near-coplanar edge cases */
  if ( minDist > negMax )
    return CLASSIFY_COPLANAR;
  if ( maxDist < k_classifyMaxExtent[axis] )
    return CLASSIFY_COPLANAR;

  /* split */
  return 3;
}

/*
================
CheckPlaneAgainstAllWindings

Checks if a plane produces any coplanar result against any winding in the list.
================
*/
int CheckPlaneAgainstAllWindings(float *planeNormal, Face_t *list, int axis)
{
  Face_t *f;

  for ( f = list; f; f = f->next )
  {
    if ( ClassifyWindingAgainstPlane(f, planeNormal, planeNormal[3], axis) == CLASSIFY_COPLANAR )
      return 0;
  }
  return 1;
}

/*
================
SelectSplitPlane_DirectEval

Evaluates all winding planes directly — score = facing - 2*splits.
================
*/
void SelectSplitPlane_DirectEval(int *evalResult, Face_t *list, int axis)
{
  Face_t *split, *check;
  float *plane;
  int side, facing, frontCount, backCount, splits, value;
  int skip;

  /* evaluate each face's plane as a potential split */
  for ( split = list; split; split = split->next )
  {
    plane = MAP_PLANE(split->planenum)->normal;
    facing = 0;
    frontCount = 0;
    backCount = 0;
    splits = 0;
    skip = 0;

    /* classify all other faces against this plane */
    for ( check = list; check; check = check->next )
    {
      if ( split->planenum == check->planenum )
      {
        facing++;
        continue;
      }
      side = ClassifyWindingAgainstPlane(check, plane, plane[3], axis);
      if ( side == 3 )
        splits++;
      else if ( side == 0 )
        frontCount++;
      else if ( side == 1 )
        backCount++;
      else if ( side == CLASSIFY_COPLANAR )
      {
        skip = 1;
        break;
      }
      else if ( side != 0 && side != 1 )
        Assert(g_assertsDisabled, s_assertDisable_SelectSplitPlane_DirectEval);
    }

    if ( skip )
      continue;

    value = SelectSplitPlane_Score(facing, frontCount, backCount, splits);
    if ( value > evalResult[1] || (value == evalResult[1] && splits < evalResult[2]) )
    {
      evalResult[0] = split->planenum;
      evalResult[1] = value;
      evalResult[2] = splits;
    }
  }
}

/*
================
SelectSplitPlane_EvaluateAxes

Axis-based split plane selection — sweeps sorted min/max arrays
to find optimal split distance.
================
*/
void SelectSplitPlane_EvaluateAxes(int *evalResult, Face_t *windingList, signed int faceCount, int axis)
{
  Face_t *w;
  int nMins, nCoplanar;
  int minIdx, maxIdx, copIdx;
  int frontCount, backCount, splitCount, coplanarCount;
  int prevMinsConsumed, prevCoplanarConsumed;
  int minsConsumed, coplanarConsumed;
  float curDist, nextDist;
  int score;
  int found;
  float bestNormal[3], bestDist;
  float testNormal[3], testDist;
  int i, j;

  #define SPLIT_MIN_EXTENT 0.001f

  found = 0;
  for ( i = 0; i < 3; ++i )
  {
    /* phase 1: collect min/max extents and coplanar positions per axis */
    nMins = 0;
    nCoplanar = 0;
    for ( w = windingList; w; w = w->next )
    {
      float lo = w->w->points[0][i];
      float hi = lo;
      for ( j = 1; j < w->w->numpoints; j++ )
      {
        float c = w->w->points[j][i];
        if ( c < lo ) lo = c;
        else if ( c > hi ) hi = c;
      }
      if ( hi - lo >= SPLIT_MIN_EXTENT )
      {
        splitMinsArray[nMins] = lo;
        splitMaxsArray[nMins] = hi;
        nMins++;
      }
      else
      {
        splitCoplanarArray[nCoplanar++] = MIDF(hi, lo);
      }
    }
    Assert(nMins <= faceCount, s_assertDisable_SelectSplitPlane_EvaluateAxes);
    Assert(nCoplanar <= faceCount, s_assertDisable_SelectSplitPlane_EvaluateAxes);
    Assert(nMins + nCoplanar == faceCount, s_assertDisable_SelectSplitPlane_EvaluateAxes);

    /* phase 2: sort for sweep-line */
    qsort(splitMinsArray, nMins, sizeof(float), FloatCompare);
    qsort(splitMaxsArray, nMins, sizeof(float), FloatCompare);
    qsort(splitCoplanarArray, nCoplanar, sizeof(float), FloatCompare);

    /* phase 3: sweep sorted positions, scoring each split candidate */
    backCount = faceCount;
    frontCount = 0;
    splitCount = 0;
    coplanarCount = 0;
    minIdx = 0;
    maxIdx = 0;
    copIdx = 0;
    prevMinsConsumed = 0;
    prevCoplanarConsumed = 0;

    /* find first candidate position */
    if ( nMins && nCoplanar )
      nextDist = (splitMinsArray[0] < (double)splitCoplanarArray[0]) ? splitMinsArray[0] : splitCoplanarArray[0];
    else if ( nMins )
      nextDist = splitMinsArray[0];
    else if ( nCoplanar )
      nextDist = splitCoplanarArray[0];
    else
      nextDist = FLT_MAX;

    while ( nextDist < FLT_MAX )
    {
      curDist = nextDist;
      nextDist = FLT_MAX;

      /* transfer previous iteration's consumed counts */
      splitCount += prevMinsConsumed;
      backCount -= prevMinsConsumed;
      frontCount += prevCoplanarConsumed;
      coplanarCount -= prevCoplanarConsumed;

      /* advance mins cursor: new splits start here */
      minsConsumed = 0;
      while ( minIdx < nMins && splitMinsArray[minIdx] == curDist )
      {
        minsConsumed++;
        minIdx++;
      }
      if ( minIdx < nMins && splitMinsArray[minIdx] < FLT_MAX )
        nextDist = splitMinsArray[minIdx];

      /* advance maxs cursor: splits end here, become front */
      while ( maxIdx < nMins && splitMaxsArray[maxIdx] == curDist )
      {
        frontCount++;
        splitCount--;
        maxIdx++;
      }
      if ( maxIdx < nMins && splitMaxsArray[maxIdx] < (double)nextDist )
        nextDist = splitMaxsArray[maxIdx];

      /* advance coplanar cursor */
      coplanarConsumed = 0;
      while ( copIdx < nCoplanar && splitCoplanarArray[copIdx] == curDist )
      {
        coplanarConsumed++;
        copIdx++;
      }
      coplanarCount += coplanarConsumed;
      backCount -= coplanarConsumed;
      if ( copIdx < nCoplanar && splitCoplanarArray[copIdx] < (double)nextDist )
        nextDist = splitCoplanarArray[copIdx];

      Assert(coplanarCount + frontCount + backCount + splitCount == faceCount, s_assertDisable_SelectSplitPlane_EvaluateAxes);
      Assert(coplanarCount >= 0, s_assertDisable_SelectSplitPlane_EvaluateAxes);
      Assert(frontCount >= 0, s_assertDisable_SelectSplitPlane_EvaluateAxes);
      Assert(backCount >= 0, s_assertDisable_SelectSplitPlane_EvaluateAxes);
      Assert(splitCount >= 0, s_assertDisable_SelectSplitPlane_EvaluateAxes);

      /* score this candidate: must have items on both sides, balance check */
      if ( frontCount && backCount && abs(frontCount - backCount) >= faceCount / 2 )
      {
        score = SelectSplitPlane_Score(coplanarCount, frontCount, backCount, splitCount);
        if ( score > evalResult[1] || (score == evalResult[1] && splitCount < evalResult[2]) )
        {
          /* verify no winding is coplanar with this split */
          VectorClear(testNormal);
          testNormal[i] = 1.0f;
          testDist = curDist;

          for ( w = windingList; w; w = w->next )
          {
            if ( ClassifyWindingAgainstPlane(w, testNormal, testDist, axis) == CLASSIFY_COPLANAR )
              break;
          }
          if ( !w )
          {
            /* no coplanar winding — accept this candidate */
            evalResult[1] = score;
            evalResult[2] = splitCount;
            VectorCopy(testNormal, bestNormal);
            bestDist = testDist;
            found = 1;
          }
        }
      }

      prevMinsConsumed = minsConsumed;
      prevCoplanarConsumed = coplanarConsumed;
    }
  }

  if ( found )
  {
    *evalResult = FindFloatPlane(bestNormal, bestDist);
    return;
  }
}

/*
================
SelectSplitPlane_Optimized

Optimized BSP split plane selection. Used when useOptimizedSplit is non-zero.
Algorithm: 0 windings → leaf (-1), 1 winding → that plane,
small sets (<=64) or small bounds → direct evaluation,
otherwise sweep each axis (X/Y/Z) to find optimal split.
================
*/
int SelectSplitPlane_Optimized(Node_t *node, int windingCount, Face_t *windingList)
{
  int axis;
  int bestPlane;
  int evalResult[3];
  int useDirectEval;

  if ( !windingCount )
    return -1;
  if ( windingCount == 1 )
    return windingList->planenum;

  evalResult[0] = -1;
  evalResult[1] = INT_MIN;
  evalResult[2] = INT_MAX;

  /* use direct eval for small counts or small spatial bounds */
  #define DIRECT_EVAL_THRESHOLD 64
  useDirectEval = (windingCount <= DIRECT_EVAL_THRESHOLD ||
    (node->maxs[0] - node->mins[0] < blockSize && node->maxs[1] - node->mins[1] < blockSize));

  /* try each axis */
  bestPlane = -1;
  for ( axis = 0; axis < 3 && bestPlane < 0; axis++ )
  {
    if ( useDirectEval )
      SelectSplitPlane_DirectEval(evalResult, windingList, axis);
    SelectSplitPlane_EvaluateAxes(evalResult, windingList, windingCount, axis);
    bestPlane = evalResult[0];

    if ( bestPlane < 0 && !useDirectEval )
    {
      SelectSplitPlane_DirectEval(evalResult, windingList, axis);
      bestPlane = evalResult[0];
    }
  }

  return bestPlane;
}

/*
================
SelectSplitPlane_BlockSubdivision

Block-based BSP plane selection. Two modes:
1. blockSize == 0: scores all winding planes directly
2. blockSize != 0: creates axis-aligned planes at block boundaries
================
*/
int SelectSplitPlane_BlockSubdivision(Node_t *node, Face_t *windingList, int axis)
{
  Face_t *check, *split, *f;
  float *plane;
  float normal[3];
  int bestValue, facing, frontCount, backCount, splits;
  Face_t *bestSplit;

  if ( blockSize != 0.0 )
  {
    /* block subdivision: find first axis spanning more than one block */
    int ax;
    for ( ax = 0; ax < 2; ax++ )
    {
      if ( (floor(node->mins[ax] / blockSize) + 1.0) * blockSize < node->maxs[ax] )
      {
        double midpoint = node->maxs[ax] + node->mins[ax];
        float ratio = midpoint / blockSize * 0.5f;
        /* CoD4's helper adds 2^-30 and truncates to int; it does not round
         * to the nearest block as the CoD2 donor did. */
        float dist = (float)((int)(ratio + 9.313225746154785e-10f) * blockSize);
        int planenum;

        VectorClear(normal);
        normal[ax] = 1.0f;
        Assert(dist > node->mins[ax], s_assertDisable_SelectSplitPlane_BlockSubdivision);
        Assert(dist < (double)node->maxs[ax], s_assertDisable_SelectSplitPlane_BlockSubdivision);
        planenum = FindFloatPlane(normal, dist);
        Assert((planenum & 1) == 0, s_assertDisable_SelectSplitPlane_BlockSubdivision);
        return planenum;
      }
    }
  }

  /* direct eval: score each winding plane as potential split */
  if ( !windingList )
    return -1;

  for ( f = windingList; f; f = f->next )
    f->checked = 0;

  bestValue = INT_MIN;
  bestSplit = windingList;
  check = windingList;

  for ( split = windingList; split; split = split->next )
  {
    if ( split->checked )
      continue;

    plane = MAP_PLANE(split->planenum)->normal;
    splits = 0;
    facing = 0;
    frontCount = 0;
    backCount = 0;

    for ( check = windingList; check; check = check->next )
    {
      if ( check->planenum == split->planenum )
      {
        facing++;
        check->checked = 1;
      }
      else
      {
        int side = ClassifyWindingAgainstPlane(check, plane, plane[3], axis);
        if ( side == CLASSIFY_COPLANAR )
          goto next_split;
        if ( side == 3 )
          splits++;
        else if ( side == 0 )
          frontCount++;
        else if ( side == 1 )
          backCount++;
      }
    }

    { int value = SelectSplitPlane_Score(facing, frontCount, backCount, splits);
    if ( MAP_PLANE(split->planenum)->type < PLANE_NON_AXIAL )
      value += 640;
    if ( value > bestValue )
    {
      bestValue = value;
      bestSplit = split;
    } }

  next_split: ;
  }

  return bestSplit ? bestSplit->planenum : -1;
}

/*
================
BuildBspTree_Recursive

Recursively partitions face list to build BSP tree.
Selects a split plane, partitions the face list into front/back/coplanar,
allocates child nodes, copies parent bounds, tightens axis-aligned splits,
and recurses into both children.
================
*/
int BuildBspTree_Recursive(Node_t *node, Face_t *list, int faceCount)
{
  #define SPLIT_CLIP_EPSILON 0.2f
  Face_t *frontList, *backList, *cur, *next;
  int frontCount, backCount, splitPlane, ax, i;
  float *plane;

  /* select best split plane */
  splitPlane = -1;
  if ( useOptimizedSplit )
  {
    splitPlane = SelectSplitPlane_Optimized(node, faceCount, list);
  }
  else
  {
    for ( ax = 0; ax < 3; ax++ )
    {
      splitPlane = SelectSplitPlane_BlockSubdivision(node, list, ax);
      if ( splitPlane >= 0 )
        break;
    }
  }

  /* no valid split — leaf node */
  if ( splitPlane == -1 )
  {
    node->planenum = -1;
    return ++ArgList;
  }
  node->planenum = splitPlane;
  plane = MAP_PLANE(splitPlane)->normal;

  /* partition face list into front / back / coplanar (discarded) */
  frontList = NULL;
  backList = NULL;
  frontCount = 0;
  backCount = 0;

  for ( cur = list; cur; cur = next )
  {
    next = cur->next;

    if ( cur->planenum == splitPlane )
    {
      /* coplanar with split — discard */
      FreeFace(cur);
      continue;
    }

    switch ( WindingPlaneSide(cur->w, plane, plane[3]) )
    {
    case 3: /* SIDE_CROSS — clip */
    {
      Winding_t *fw, *bw;
      ClipWindingEpsilon(cur->w, plane, plane[3], SPLIT_CLIP_EPSILON, &fw, &bw, 0);
      if ( fw )
      {
        Face_t *f = AllocFace();
        f->w = fw;
        f->next = frontList;
        f->planenum = cur->planenum;
        f->checked = 0;
        frontList = f;
        frontCount++;
      }
      if ( bw )
      {
        Face_t *f = AllocFace();
        f->w = bw;
        f->next = backList;
        f->planenum = cur->planenum;
        f->checked = 0;
        backList = f;
        backCount++;
      }
      FreeFace(cur);
      break;
    }
    case 1: /* SIDE_BACK */
      cur->next = backList;
      backList = cur;
      backCount++;
      break;
    case 0: /* SIDE_FRONT */
      cur->next = frontList;
      frontList = cur;
      frontCount++;
      break;
    }
  }

  /* allocate child nodes, copy parent bounds */
  for ( i = 0; i < 2; i++ )
  {
    Node_t *child = AllocNode();
    node->children[i] = child;
    child->brushlist.parentNode = node;
    VectorCopy(node->mins, child->mins);
    VectorCopy(node->maxs, child->maxs);
  }

  /* tighten bounds at axis-aligned split planes */
  for ( ax = 0; ax < 3; ax++ )
  {
    if ( plane[ax] == 1.0f )
    {
      node->children[0]->mins[ax] = plane[3];
      node->children[1]->maxs[ax] = plane[3];
      break;
    }
  }

  /* recurse: front child, then back child */
  BuildBspTree_Recursive(node->children[0], frontList, frontCount);
  return BuildBspTree_Recursive(node->children[1], backList, backCount);
  #undef SPLIT_CLIP_EPSILON
}

/*
================
BuildBspTree

Creates initial BSP tree structure from winding list.
Iterates through collected windings, computes overall bounds, creates
the root node, and kicks off recursive BSP construction.
================
*/
Tree_t *BuildBspTree(Face_t *windingList)
{
  Face_t *f;
  int windingCount;
  Tree_t *tree;
  Winding_t *w;
  Node_t *root;
  int i;

  Com_DPrintf("--- FaceBSP---\n");
  tree = AllocTree();

  /* count windings and compute overall bounds */
  windingCount = 0;
  for ( f = windingList; f; f = f->next )
  {
    w = f->w;
    windingCount++;
    for ( i = 0; i < w->numpoints; i++ )
      AddPointToBounds(w->points[i], tree->mins, tree->maxs);
  }

  Com_DPrintf("%5i faces\n", windingCount);

  /* create root node with tree bounds */
  root = AllocNode();
  tree->headnode = root;
  VectorCopy(tree->mins, root->mins);
  VectorCopy(tree->maxs, root->maxs);

  ArgList = 0;

  /* build BSP tree recursively */
  if ( useOptimizedSplit )
  {
    splitCoplanarArray = malloc(sizeof(float) * windingCount);
    splitMinsArray = malloc(sizeof(float) * windingCount);
    splitMaxsArray = malloc(sizeof(float) * windingCount);
    BuildBspTree_Recursive(root, windingList, windingCount);
    free(splitMaxsArray);
    free(splitMinsArray);
    free(splitCoplanarArray);
  }
  else
  {
    BuildBspTree_Recursive(root, windingList, windingCount);
  }

  Com_DPrintf("%5i leafs\n", ArgList);
  return tree;
}

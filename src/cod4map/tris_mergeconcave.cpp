/*
tris_mergeconcave.c — Concave winding merging

Reconstructed from cod2map.exe by Rose.

Merges coplanar triangle surfaces into concave (non-convex) windings.
Walks visibility groups, finds bridge edges between adjacent windings,
splices them together, validates holes, and plugs notches. The merged
concave windings produce fewer triangles than individual convex windings.
*/

#include "cod4map.h"

MergeCallback_t   mergeCallbackFn;
TriSurf_t       **mergeSurfArray;
TriSurf_t       **mergeVisGroupListHead;
TriSurf_t        *mergeCurrentSurf;

int    (*mergeGroupableCallback)(TriSurf_t *, TriSurf_t *);
int      mergeSurfCapacity;
size_t   mergeSurfCount;
int      mergeTraversalStamp;
void    *mergeVertMap;
int      mergeVertMapCount;

char s_assertDisable_AddWindingWithoutOverlap_r;
char s_assertDisable_AddWindingWithoutOverlap_r;
char s_assertDisable_AddWindingWithoutOverlap_r;
char s_assertDisable_AllocIntWinding;
char s_assertDisable_AllocIntWinding;
char s_assertDisable_BuildIndexList;
char s_assertDisable_CollapseCollinearVerts;
char s_assertDisable_CollapseCollinearVerts;
char s_assertDisable_FindBestBridgeEdge;
char s_assertDisable_FindBestBridgeEdge;
char s_assertDisable_FindBestBridgeEdge;
char s_assertDisable_FindBestBridgeEdge;
char s_assertDisable_FindBestBridgeEdge;
char s_assertDisable_FindBestBridgeEdge;
char s_assertDisable_FindHoles;
char s_assertDisable_FindHoles;
char s_assertDisable_FindHoles;
char s_assertDisable_FindHoles;
char s_assertDisable_FindHoles;
char s_assertDisable_FreeIntWinding;
char s_assertDisable_FreeMergeVertMap;
char s_assertDisable_InitVertMap;
char s_assertDisable_InitVertMap;
char s_assertDisable_InsertHole;
char s_assertDisable_InsertHole;
char s_assertDisable_InsertHole;
char s_assertDisable_InsertHole;
char s_assertDisable_InsertHole;
char s_assertDisable_InsertHole;
char s_assertDisable_InsertHole;
char s_assertDisable_InsertHole;
char s_assertDisable_IntWindingToWinding;
char s_assertDisable_IsValidHole;
char s_assertDisable_MergeCheckGroupCallback;
char s_assertDisable_MergeConcave_AddSurf;
char s_assertDisable_MergeConcave_FindMergeGroup;
char s_assertDisable_MergeConcave_FindMergeGroup;
char s_assertDisable_MergeConcave_FindMergeGroup;
char s_assertDisable_MergeConcave_PropagateGroup;
char s_assertDisable_MergeConcave_PropagateGroup;
char s_assertDisable_MergeConcave_PropagateGroup;
char s_assertDisable_MergeGroupSurfaces;
char s_assertDisable_MergeHoles;
char s_assertDisable_MergeHoles;
char s_assertDisable_MergeHoles;
char s_assertDisable_MergeMarkGroupCallback;
char s_assertDisable_MergeSurfaces_Init;
char s_assertDisable_MergeSurfaces_Init;
char s_assertDisable_MergeSurfaces_Shutdown;
char s_assertDisable_MergeVisGroupList;
char s_assertDisable_MergeVisGroupList;
char s_assertDisable_MergeVisGroupList;
char s_assertDisable_FindNotchSpan;
char s_assertDisable_TryPlugNotch;
char s_assertDisable_TryPlugNotch;
char s_assertDisable_MergeWindingsAtBridge;
char s_assertDisable_MergeWindingsAtBridge;
char s_assertDisable_MergeWindingsAtBridge;
char s_assertDisable_MergeWindingsAtBridge;
char s_assertDisable_MergeWindingsAtBridge;
char s_assertDisable_MergeWindingsAtBridge;
char s_assertDisable_MergeWindingsAtBridge;
char s_assertDisable_MergeWindingsAtBridge;
char s_assertDisable_MergeWindingsAtBridge;
char s_assertDisable_MergeWindingsAtBridge;
char s_assertDisable_MergeWindingsAtBridge;
char s_assertDisable_MergeWindingsAtBridge;
char s_assertDisable_RemoveDuplicateIndices;
char s_assertDisable_RemoveDuplicateIndices;
char s_assertDisable_RemoveSharedBoundaryPoints;
char s_assertDisable_RemoveSharedBoundaryPoints;
char s_assertDisable_SpliceWindingsAtSharedEdge;
char s_assertDisable_SpliceWindingsAtSharedEdge;
char s_assertDisable_SpliceWindingsAtSharedEdge;


/*
================
AllocIntWinding

Allocates COD4's count-plus-index winding.  The optional auxiliary buffer is
only retained for KIWI's shadow-caster compatibility callback route.
================
*/
IntWinding_t *AllocIntWinding(int ptCount, int auxElemSize)
{
  IntWinding_t *w;

  Assert(ptCount > 0, s_assertDisable_AllocIntWinding);
  Assert(auxElemSize >= 0, s_assertDisable_AllocIntWinding);
  w = calloc(1, sizeof(IntWinding_t) + sizeof(int) * ptCount);
  if ( !w )
    Com_Error("Out of memory on %i-point integer winding", ptCount);
  if ( auxElemSize )
  {
    w->auxData = malloc(auxElemSize * ptCount);
    if ( !w->auxData )
      Com_Error("Out of memory on %i bytes of aux data for %i-point integer winding", auxElemSize * ptCount, ptCount);
  }
  return w;
}

/*
================
FreeIntWinding

Frees the compact index winding and, only for the legacy shadow callback
route, its separately owned compatibility sidecar.
================
*/
void FreeIntWinding(IntWinding_t *w)
{
  Assert(w, s_assertDisable_FreeIntWinding);
  free(w->auxData);
  free(w);
}

/*
================
IntWindingToWinding

Converts a compact integer winding to a float winding via vertMap lookup.
================
*/
Winding_t *IntWindingToWinding( IntWinding_t *iw, Winding_t **outWinding )
{
  int i, numPts;
  Winding_t *w;

  Assert(iw, s_assertDisable_IntWindingToWinding);

  numPts = iw->numpoints;
  w = AllocWinding(numPts);
  w->numpoints = numPts;

  for ( i = 0; i < numPts; i++ )
  {
    float *src = (float *)((char *)mergeVertMap + sizeof(vec3_t) * iw->indices[i]);
    VectorCopy(src, w->points[i]);
  }
  *outWinding = w;

  /* The native path has no index-winding auxiliary payload.  Preserve the
     existing shadow-only sidecar transfer when that optional route supplied
     one, without making normal concave merging depend on it. */
  outWinding[1] = (Winding_t *)iw->auxData;
  iw->auxData = NULL;
  return w;
}

/*
================
GetOrAddMergeVertex

Finds or adds a vertex in mergeGlob.vertMap.
Returns the index of the matching or newly added vertex.
================
*/
int GetOrAddMergeVertex( int *vert )
{
  int i;
  float *v = (float *)vert;
  float *map = (float *)mergeVertMap;

  /* search for existing vertex within epsilon */
  for ( i = 0; i < mergeVertMapCount; i++ )
  {
    if ( VectorCompareEpsilon(v, &map[i * 3], ON_EPSILON, 3) )
      return i;
  }

  /* not found — add new vertex */
  VectorCopy(v, &map[i * 3]);
  ++mergeVertMapCount;
  return i;
}

/*
================
FreeMergeVertMap

Frees mergeGlob.vertMap and resets vertex count.
================
*/
void FreeMergeVertMap()
{
  Assert(mergeVertMap, s_assertDisable_FreeMergeVertMap);
  free(mergeVertMap);
  mergeVertMap = 0;
  mergeVertMapCount = 0;
}

/*
================
AddWindingWithoutOverlap_r

Recursively clips a winding against existing surfaces and adds
the non-overlapping remainder as new surfaces.
================
*/
signed int AddWindingWithoutOverlap_r(int startIdx, int surfCount, int prevSurfCount, WindingAuxPair_t *windingPair, int windingBase, TriSurfProps_t *props)
{
  TriSurf_t *surfTs;
  Winding_t *ow;
  int surfIdx, e;
  float *surfNormal;
  vec3_t edgeDir, clipNormal;
  float clipDist;
  WindingAuxPair_t curWinding, frontPiece, backPiece;

  curWinding = *windingPair;

  for ( surfIdx = startIdx; surfIdx < prevSurfCount; surfIdx++ )
  {
    surfTs = mergeSurfArray[surfIdx];
    ow = surfTs->winding;
    surfNormal = surfTs->props->plane;
    if ( CheckWindingSeparationBoth(ow, curWinding.winding, surfNormal, ON_EPSILON) || ow->numpoints <= 0 )
      continue;

    /* clip curWinding against each edge of the overlapping winding */
    for ( e = 1; e <= ow->numpoints; e++ )
    {
      float *prevVert = ow->points[e > 0 ? e - 1 : 0];
      float *nextVert = ow->points[e % ow->numpoints];

      VectorSubtract(nextVert, prevVert, edgeDir);
      CrossProduct(surfNormal, edgeDir, clipNormal);
      VecNormalize(clipNormal);
      clipDist = (float)DotProduct120(clipNormal, prevVert);
      /* Native ClipTriWinding's first output is recursively emitted; its second
         output remains the current winding while the obstacle's next edge is
         applied.  Keep the established local roles rather than their names. */
      ClipTriWindingWithAux(&curWinding, ow, windingBase, clipNormal, clipDist, ON_EPSILON, &backPiece, &frontPiece);

      if ( frontPiece.winding )
      {
        if ( backPiece.winding )
          surfCount = AddWindingWithoutOverlap_r(startIdx + 1, surfCount, prevSurfCount, &backPiece, windingBase, props);
        FreeVerts(&curWinding);
        curWinding = frontPiece;
        if ( e == ow->numpoints )
        {
          FreeVerts(&curWinding);
          return surfCount;
        }
      }
      else if ( backPiece.winding )
      {
        FreeVerts(&curWinding);
        curWinding = backPiece;
        break;
      }
      else
      {
        Com_Error("code bug: AddWindingWithoutOverlap_r: winding disappeared\n");
      }
    }
  }
  #define MAX_WINDINGS_PER_CONCAVE_GROUP 0x8000
  if ( surfCount == MAX_WINDINGS_PER_CONCAVE_GROUP )
    Com_Error("MAX_WINDINGS_PER_CONCAVE_GROUP (%i) exceeded\n", MAX_WINDINGS_PER_CONCAVE_GROUP);
  Assert(surfCount >= prevSurfCount, s_assertDisable_AddWindingWithoutOverlap_r);
  Assert(surfCount <= (int)mergeSurfCount, s_assertDisable_AddWindingWithoutOverlap_r);
  if ( surfCount != prevSurfCount )
  {
    mergeSurfArray[mergeSurfCount] = mergeSurfArray[surfCount];
    mergeSurfArray[surfCount] = AllocTriSurf(curWinding.winding, curWinding.auxData, windingBase, props);
    mergeSurfCount++;
  }
  mergeSurfArray[surfCount]->winding = curWinding.winding;
  mergeSurfArray[surfCount]->auxData = curWinding.auxData;
  AssertFatal(mergeSurfArray[surfCount]->props == props, s_assertDisable_AddWindingWithoutOverlap_r);

  return surfCount + 1;
}

/*
================
CompareSurfaces

qsort callback comparing surfaces by projection axis position.
================
*/
int CompareSurfaces(const void *surfA, const void *surfB)
{
  TriSurf_t *tsA = *(TriSurf_t **)surfA;
  TriSurf_t *tsB = *(TriSurf_t **)surfB;
  int axisU, axisV;

  GetProjectionAxes((tsA->props)->plane, &axisU, &axisV);

  float minA_U = tsA->mins[axisU];
  float minB_U = tsB->mins[axisU];
  float minA_V = tsA->mins[axisV];
  float minB_V = tsB->mins[axisV];

  if ( minA_U < minB_U )
    return -1;
  if ( minA_U > minB_U )
    return 1;
  if ( minA_V < minB_V )
    return -1;
  if ( minA_V > minB_V )
    return 1;

  return 0;
}

/*
================
RemoveOverlappingWindings

Removes overlapping winding regions from the surface array.
================
*/
void RemoveOverlappingWindings( int mode )
{
  TriSurf_t *ts;
  int start, i, next;

  if ( !mode )
    return;

  /* second pass skips the base surface at index 0 */
  start = (mode == 2) ? 1 : 0;

  for ( i = start; i < (int)mergeSurfCount; i = next )
  {
    ts = mergeSurfArray[i];
    next = AddWindingWithoutOverlap_r( start, i, i, (WindingAuxPair_t *)&ts->winding, ts->auxElemSize, ts->props );

    if ( next == i )
    {
      /* surface was fully overlapped — free and swap-remove */
      ts->winding = 0;
      ts->auxData = 0;
      FreeTriSurf( ts );
      --mergeSurfCount;
      mergeSurfArray[i] = mergeSurfArray[mergeSurfCount];
    }
  }
}

/*
================
FindSharedEdge

Finds the longest shared vertex sequence between two windings.
Returns the shared edge length, or 0 if none found.
================
*/
int FindSharedEdge(IntWinding_t *windingA, IntWinding_t *windingB, int *outStartA, int *outStartB)
{
  int nA = windingA->numpoints, nB = windingB->numpoints;
  int maxLen = nA < nB ? nA : nB;
  int a, b, sharedLen;
  int wrapIdx, wrapA = 0, wrapB = 0;

  #define VERT_A(i) windingA->indices[(i)]
  #define VERT_B(i) windingB->indices[(i)]

  if ( nA <= 0 )
    return 0;

  for ( a = 0; a < nA; a++ )
  {
    for ( b = 0; b < nB; b++ )
    {
      if ( VERT_A(a) != VERT_B(b) )
        continue;

      sharedLen = 1;

      /* extend backward in A (wrap from end), forward in B — only at a==0 */
      if ( !a )
      {
        for ( wrapIdx = nA - 1; sharedLen < maxLen; wrapIdx-- )
        {
          wrapA = wrapIdx % nA;
          wrapB = (sharedLen + b) % nB;
          if ( VERT_A(wrapA) != VERT_B(wrapB) )
            break;
          ++sharedLen;
        }
        if ( sharedLen >= maxLen )
        {
          *outStartA = wrapA;
          *outStartB = wrapB;
          return sharedLen;
        }
        if ( sharedLen > 1 )
        {
          a = wrapA + 1;
          b = (wrapB + nB - 1) % nB;
        }
      }

      /* extend forward in A, backward in B */
      while ( sharedLen < maxLen )
      {
        if ( VERT_A((sharedLen + a) % nA) != VERT_B((b + nB - sharedLen) % nB) )
          break;
        ++sharedLen;
      }

      if ( sharedLen == 1 )
        continue;

      *outStartA = a;
      *outStartB = b;
      return sharedLen;
    }
  }

  #undef VERT_A
  #undef VERT_B

  return 0;
}

/*
================
WindingArrayShift

Shifts winding array elements via memcpy.
================
*/
void *WindingArrayShift(int elemSize, intptr_t arrayBase, int destIdx, int shiftAmount, int arrayLen)
{
  return memcpy((void *)(arrayBase + destIdx * elemSize), (const void *)(arrayBase + elemSize * (destIdx + shiftAmount)), elemSize * (arrayLen - destIdx));
}

/*
================
MergeConcave_ResetSurfCount

Resets mergeGlob.surfCount to zero.
================
*/
void MergeConcave_ResetSurfCount()
{
  mergeSurfCount = 0;
}

/*
================
MergeConcave_AddSurf

Adds a surface to the mergeGlob.surfs array.
================
*/
size_t MergeConcave_AddSurf( TriSurf_t *ts, TriSurf_t **listHead )
{
  Assert(ts, s_assertDisable_MergeConcave_AddSurf);
  if ( mergeSurfCount == mergeSurfCapacity )
    Com_Error("more than %i windings\n", mergeSurfCapacity);

  UnlinkAndFreeSurf(ts, listHead);
  mergeSurfArray[mergeSurfCount] = ts;
  return ++mergeSurfCount;
}

/*
================
MergeConcave_GetSurfCount

Returns mergeGlob.surfCount.
================
*/
size_t MergeConcave_GetSurfCount()
{
  return mergeSurfCount;
}

/*
================
MergeCheckGroupCallback

Grid tree callback that adds a groupable surface to the merge group.
================
*/
size_t MergeCheckGroupCallback(TriSurf_t *ts)
{
  size_t result;

  Assert(GetTrisTransientMode() == 2, s_assertDisable_MergeCheckGroupCallback);
  result = mergeTraversalStamp;
  if ( (intptr_t)ts->origWinding != mergeTraversalStamp )
  {
    ts->origWinding = (void *)(intptr_t)mergeTraversalStamp;
    result = mergeGroupableCallback(mergeSurfArray[0], ts);
    if ( (char)result )
      return MergeConcave_AddSurf(ts, mergeVisGroupListHead);
  }
  return result;
}

/*
================
MergeConcave_FindMergeGroup

Finds a merge group by flood-filling groupable neighbors
starting from the seed surface.
================
*/
size_t MergeConcave_FindMergeGroup(TriSurf_t *seedSurf)
{
  size_t result;
  int i;
  TriSurf_t **listHead;

  Assert(mergeGroupableCallback, s_assertDisable_MergeConcave_FindMergeGroup);
  Assert(mergeSurfArray, s_assertDisable_MergeConcave_FindMergeGroup);
  ++mergeTraversalStamp;
  listHead = mergeVisGroupListHead;
  *mergeSurfArray = seedSurf;
  mergeSurfCount = 1;
  UnlinkAndFreeSurf(seedSurf, listHead);
  result = mergeSurfCount;
  for ( i = 0; i < (int)mergeSurfCount; ++i )
  {
    mergeCurrentSurf = mergeSurfArray[i];
    AssertFatal(mergeCurrentSurf, s_assertDisable_MergeConcave_FindMergeGroup);
    GridTree_ForEach(mergeCurrentSurf->mins, mergeCurrentSurf->maxs, MergeCheckGroupCallback);
    result = mergeSurfCount;
  }

  return result;
}

/*
================
MergeMarkGroupCallback

Grid tree callback that copies a groupable surface into the merge group.
================
*/
void MergeMarkGroupCallback( TriSurf_t *ts )
{
  Assert(GetTrisTransientMode() == 2, s_assertDisable_MergeMarkGroupCallback);

  /* skip already-visited surfaces (origWinding used as traversal stamp) */
  if ( (intptr_t)ts->origWinding == mergeTraversalStamp )
    return;
  ts->origWinding = (void *)(intptr_t)mergeTraversalStamp;

  if ( !mergeGroupableCallback( mergeCurrentSurf, ts ) )
    return;

  if ( mergeSurfCount == mergeSurfCapacity )
    Com_Error( "more than %i windings\n", mergeSurfCapacity );

  mergeSurfArray[mergeSurfCount] = CopyTriSurf( ts );
  mergeSurfCount++;
}

/*
================
MergeConcave_PropagateGroup

Propagates a merge group via grid tree flood-fill with copy.
================
*/
size_t MergeConcave_PropagateGroup(TriSurf_t *seedSurf)
{
  int i;
  size_t result;

  Assert(mergeGroupableCallback, s_assertDisable_MergeConcave_PropagateGroup);
  Assert(mergeSurfArray, s_assertDisable_MergeConcave_PropagateGroup);
  ++mergeTraversalStamp;
  mergeSurfCount = 0;
  for ( i = -1; ; seedSurf = mergeSurfArray[i] )
  {
    mergeCurrentSurf = seedSurf;
    AssertFatal(seedSurf != NULL, s_assertDisable_MergeConcave_PropagateGroup);
    GridTree_ForEach(mergeCurrentSurf->mins, mergeCurrentSurf->maxs, MergeMarkGroupCallback);
    result = mergeSurfCount;
    if ( ++i == mergeSurfCount )
      break;
  }
  return result;
}

/*
================
MergeConcave_BuildGridTree

Builds a grid tree from a surface linked list.
================
*/
char MergeConcave_BuildGridTree(TriSurf_t *surfList, float *worldMins, float *worldMaxs)
{
  TriSurf_t *ts;

  SetGridDivisionPoints(worldMins, worldMaxs);
  for ( ts = surfList; ts; ts = ts->next )
  {
    #define GRID_BOUNDS_EXPAND 0.0099999998f
    { int ax;
    WindingBounds(ts->winding, ts->mins, ts->maxs);
    for ( ax = 0; ax < 3; ax++ )
    {
      ts->mins[ax] -= GRID_BOUNDS_EXPAND;
      ts->maxs[ax] += GRID_BOUNDS_EXPAND;
    } }
    GridTree_Insert(ts);
  }
  TJunc_SetSnapTolerance(TJUNC_SNAP_TOLERANCE);
  return TJunc_SetCreateNonAxial(0);
}

/*
================
MergeSurfaces_Init

Initializes the merge system with callbacks and surface array.
================
*/
void *MergeSurfaces_Init(int maxSurfs, int (*groupableCallback)(TriSurf_t *, TriSurf_t *), MergeCallback_t mergeCallback)
{
  void *result;

  Assert(!mergeGroupableCallback, s_assertDisable_MergeSurfaces_Init);
  Assert(!mergeSurfArray, s_assertDisable_MergeSurfaces_Init);
  mergeGroupableCallback = groupableCallback;
  mergeCallbackFn = mergeCallback;
  mergeSurfCapacity = maxSurfs;
  TJunc_MergeScratchInit();
  result = malloc(sizeof(TriSurf_t *) * maxSurfs);
  mergeSurfArray = result;
  if ( !result )
    { Com_Error("Couldn't allocate memory for merging up to %i windings\n", maxSurfs); return NULL; }
  return result;
}

/*
================
MergeSurfaces_Shutdown

Frees the merge surface array and clears callbacks.
================
*/
void MergeSurfaces_Shutdown(void)
{
  Assert(mergeSurfArray, s_assertDisable_MergeSurfaces_Shutdown);
  TJunc_MergeScratchShutdown();
  free(mergeSurfArray);
  mergeSurfArray = NULL;
  mergeGroupableCallback = NULL;
}

/*
================
WindingEdgesIntersect

Tests if two edges intersect in 2D projection.
================
*/
bool WindingEdgesIntersect(float *p1, float *p2, float *p3, float *p4, int axisU, int axisV)
{
  float edgeDir[3];
  float sideP4;
  float sideP3;
  float sideP2;
  float sideP1;

  /* 2D cross product: classify p3 and p4 against edge p1→p2 */
  VectorSubtract(p2, p1, edgeDir);
  sideP3 = Det2x2((double)p3[axisU] - p1[axisU], edgeDir[axisV], (double)p3[axisV] - p1[axisV], edgeDir[axisU]);
  sideP4 = Det2x2((double)p4[axisU] - p1[axisU], edgeDir[axisV], (double)p4[axisV] - p1[axisV], edgeDir[axisU]);
  
  /* both points must be on opposite sides (or one on the line) */
  if ( (sideP3 > -PLANESIDE_EPSILON || sideP4 > -PLANESIDE_EPSILON) && (sideP3 < PLANESIDE_EPSILON || sideP4 < PLANESIDE_EPSILON) )
  {
    /* classify p1 and p2 against edge p3→p4 */
    VectorSubtract(p4, p3, edgeDir);
    sideP1 = Det2x2((double)p1[axisU] - p3[axisU], edgeDir[axisV], (double)p1[axisV] - p3[axisV], edgeDir[axisU]);
    sideP2 = Det2x2((double)p2[axisU] - p3[axisU], edgeDir[axisV], (double)p2[axisV] - p3[axisV], edgeDir[axisU]);
    if ( (sideP1 > -PLANESIDE_EPSILON || sideP2 > -PLANESIDE_EPSILON)
      && (sideP1 < PLANESIDE_EPSILON || sideP2 < PLANESIDE_EPSILON)
      && !VectorCompareEpsilon((float *)p1, p3, PLANESIDE_EPSILON, 3)
      && !VectorCompareEpsilon((float *)p1, p4, PLANESIDE_EPSILON, 3)
      && !VectorCompareEpsilon((float *)p2, p3, PLANESIDE_EPSILON, 3)
      && !VectorCompareEpsilon((float *)p2, p4, PLANESIDE_EPSILON, 3)
      && (sideP3 * sideP3 >= COLLINEAR_EPSILON_SQ
       || sideP4 * sideP4 >= COLLINEAR_EPSILON_SQ
       || sideP1 * sideP1 >= COLLINEAR_EPSILON_SQ
       || sideP2 * sideP2 >= COLLINEAR_EPSILON_SQ) )
    {
      return 1;
    }
  }
  return 0;
}

/*
================
WindingHasCrossingEdge

Tests if any winding edge crosses the given test edge.
================
*/
char WindingHasCrossingEdge( Winding_t *w, float *edgeStart, float *edgeEnd, int axisU, int axisV )
{
  int i, n = w->numpoints;

  /* test each winding edge against the given edge */
  for ( i = 0; i < n; i++ )
  {
    int prev = (i + n - 1) % n;
    if ( WindingEdgesIntersect(w->points[prev], w->points[i], edgeStart, edgeEnd, axisU, axisV) )
      return 1;
  }
  return 0;
}

/*
================
AnyWindingHasCrossingEdge

Tests if any winding in the array has a crossing edge.
================
*/
char AnyWindingHasCrossingEdge( int axisU, int axisV, WindingAuxPair_t *windingPairs, int pairCount, float *edgeStart, float *edgeEnd )
{
  int i;

  for ( i = 0; i < pairCount; i++ )
  {
    if ( WindingHasCrossingEdge(windingPairs[i].winding, edgeStart, edgeEnd, axisU, axisV) )
      return 1;
  }
  return 0;
}

/*
================
FindWindingDiagonal

Finds a self-touching diagonal in a winding where two
non-adjacent vertices share the same position, used for
splitting the winding into two sub-windings
================
*/
char FindWindingDiagonal(Winding_t *w, int *outStart0, int *outEnd0, int *outStart1, int *outEnd1)
{
  #define DIAG_EPSILON 0.1f
  int n = w->numpoints;
  int a, b, sep, nextA, prevB;

  if ( n <= 6 )
    return 0;

  /* find two non-adjacent vertices that share the same position */
  for ( a = 0; a < n; a++ )
  {
    for ( sep = 3; sep < n - 3; sep++ )
    {
      b = (a + sep) % n;
      if ( !VectorCompareEpsilon(w->points[a], w->points[b], DIAG_EPSILON, 3) )
        continue;
      nextA = (a + 1) % n;
      prevB = (b + n - 1) % n;
      if ( !VectorCompareEpsilon(w->points[nextA], w->points[prevB], DIAG_EPSILON, 3) )
        continue;

      *outStart0 = a;
      *outEnd0 = b;

      /* extend the shared run backwards from a, forwards from b */
      if ( !a )
      {
        int ws = n - 1, we = (b + 1) % n;
        while ( VectorCompareEpsilon(w->points[ws], w->points[we], DIAG_EPSILON, 3) )
        {
          *outStart0 = ws--;
          *outEnd0 = we;
          we = (we + 1) % n;
        }
      }

      /* extend the shared run forward from nextA, backward from prevB */
      do
      {
        *outStart1 = nextA;
        *outEnd1 = prevB;
        nextA = (nextA + 1) % n;
        prevB = (prevB + n - 1) % n;
      } while ( *outStart0 != *outStart1 && VectorCompareEpsilon(w->points[nextA], w->points[prevB], DIAG_EPSILON, 3) );

      return 1;
    }
  }
  return 0;
  #undef DIAG_EPSILON
}

/*
================
WindingCopyArc

Copies an arc of vertices from a source winding pair to a
destination winding pair, handling wrap-around when start > end
================
*/
int WindingCopyArc(int startIdx, WindingAuxPair_t *srcPair, WindingAuxPair_t *dstPair, int endIdx, int auxElemSize)
{
  Winding_t *dstW;
  Winding_t *srcW;
  int result;
  int newCount;
  char *srcAux;
  char *dstAux;
  int arcLen;
  int wrapLen;

  dstW = dstPair->winding;
  srcW = srcPair->winding;
  srcAux = (char *)srcPair->auxData;
  dstAux = (char *)dstPair->auxData;
  if ( startIdx > endIdx )
  {
    wrapLen = srcW->numpoints - startIdx;
    memcpy(dstW->points[dstW->numpoints], srcW->points[startIdx], sizeof(vec3_t) * wrapLen);
    if ( auxElemSize )
      AuxDataCopy(auxElemSize, srcAux, startIdx, wrapLen, dstAux, dstW->numpoints);
    result = endIdx + 1;
    newCount = wrapLen + dstW->numpoints;
    dstW->numpoints = newCount;
    memcpy(dstW->points[newCount], srcW->points[0], sizeof(vec3_t) * (endIdx + 1));
    if ( auxElemSize )
    {
      AuxDataCopy(auxElemSize, srcAux, 0, result, dstAux, dstW->numpoints);
      result = endIdx + 1;
    }
    dstW->numpoints += result;
  }
  else
  {
    arcLen = endIdx - startIdx + 1;
    memcpy(dstW->points[dstW->numpoints], srcW->points[startIdx], sizeof(vec3_t) * arcLen);
    if ( auxElemSize )
      AuxDataCopy(auxElemSize, srcAux, startIdx, arcLen, dstAux, dstW->numpoints);
    result = arcLen + dstW->numpoints;
    dstW->numpoints = result;
  }
  return result;
}

/*
================
AllocWindingForArc

Allocates a new winding and copies an arc of points
from the source winding into it
================
*/
Winding_t *AllocWindingForArc(int extraDataSize, WindingAuxPair_t *outPair,
                              WindingAuxPair_t *srcPair, int startIdx, int endIdx)
{
  int arcLen;

  arcLen = endIdx - startIdx;
  if ( endIdx - startIdx < 0 )
    arcLen += srcPair->winding->numpoints;
  outPair->winding = AllocWinding(arcLen);
  if ( extraDataSize )
    outPair->auxData = malloc(extraDataSize * arcLen);
  else
    outPair->auxData = 0;
  outPair->winding->numpoints = 0;
  WindingCopyArc(startIdx, srcPair, outPair,
                 (srcPair->winding->numpoints + endIdx - 1) % srcPair->winding->numpoints,
                 extraDataSize);
  return outPair->winding;
}

/*
================
IsEdgeConcave

Tests if an edge between two vertices is concave by building
clip planes and checking for violations against winding vertices
================
*/
char IsEdgeConcave(float *vertA, float *vertB, Winding_t *w, float *normal, float *adjVert)
{
  #define CONCAVE_OFFSET 0.0099999998f
  #define BRIDGE_MATCH_EPSILON 0.001f
  
  /* volatile: forces 32-bit float precision, prevents FPU register hoisting */
  volatile float planes[8];
  vec3_t edgeDir;
  int i, n, threshold;

  /* build clip plane for edge A→B */
  VectorSubtract(vertB, vertA, edgeDir);
  CrossProduct(edgeDir, normal, (float *)&planes[0]);
  VecNormalize((float *)&planes[0]);
  planes[3] = DotProduct201(planes, vertB) + CONCAVE_OFFSET;

  /* build clip plane for edge B→adj */
  VectorSubtract(adjVert, vertB, edgeDir);
  CrossProduct(edgeDir, normal, (float *)&planes[4]);
  VecNormalize((float *)&planes[4]);
  planes[7] = DotProduct201(&planes[4], vertB) + CONCAVE_OFFSET;

  /* if adjVert is already behind first plane, only one violation needed */
  threshold = 2;
  if ( DotProduct201(planes, adjVert) - planes[3] <= 0.0 )
    threshold = 1;

  n = w->numpoints;
  for ( i = 0; i < n; i++ )
  {
    int prev = (i + n - 1) % n;
    double d0;
    double d1;

    if ( !VectorCompareEpsilon(w->points[prev], vertB, BRIDGE_MATCH_EPSILON, 3) )
      continue;

    /* vertex prev matches vertB — check the vertices on either side */
    { float *before = w->points[(i + n - 2) % n];
    d0 = DotProduct120(planes, before) - planes[3];
    d1 = DotProduct120(&planes[4], before) - planes[7];
    if ( (d0 > 0.0) + (d1 > 0.0) >= threshold )
      return 1;
    }

    { float *after = w->points[i];
    d0 = DotProduct(planes, after) - planes[3];
    d1 = DotProduct(&planes[4], after) - planes[7];
    if ( (d0 > 0.0) + (d1 > 0.0) >= threshold )
      return 1;
    }
  }
  return 0;
  #undef CONCAVE_OFFSET
  #undef BRIDGE_MATCH_EPSILON
}

/*
================
MergeWindingsAtBridge

Merges inner and outer windings at a bridge point by copying
arcs from both windings into the destination winding
================
*/
Winding_t *MergeWindingsAtBridge(WindingAuxPair_t *vertsInner, WindingAuxPair_t *vertsOuter,
                                 int bridgeInner, int bridgeOuter, int auxElemSize,
                                 WindingAuxPair_t *verts)
{
  int numPtsOuter;
  int numPtsInner;
  int adjustedInner;
  char bridgeMatch;

  Assert(verts, s_assertDisable_MergeWindingsAtBridge);
  Assert(vertsInner, s_assertDisable_MergeWindingsAtBridge);
  Assert(vertsOuter, s_assertDisable_MergeWindingsAtBridge);
  Assert(verts->winding, s_assertDisable_MergeWindingsAtBridge);
  Assert(vertsInner->winding, s_assertDisable_MergeWindingsAtBridge);
  Assert(vertsOuter->winding, s_assertDisable_MergeWindingsAtBridge);
  Assert(verts != vertsInner, s_assertDisable_MergeWindingsAtBridge);
  Assert(verts != vertsOuter, s_assertDisable_MergeWindingsAtBridge);
  Assert(vertsInner != vertsOuter, s_assertDisable_MergeWindingsAtBridge);
  Assert(verts->winding != vertsInner->winding, s_assertDisable_MergeWindingsAtBridge);
  Assert(verts->winding != vertsOuter->winding, s_assertDisable_MergeWindingsAtBridge);
  Assert(vertsInner->winding != vertsOuter->winding, s_assertDisable_MergeWindingsAtBridge);
  numPtsOuter = vertsOuter->winding->numpoints;
  numPtsInner = vertsInner->winding->numpoints;
  adjustedInner = numPtsInner;
  if ( vertsInner->winding->points[bridgeInner][0] == vertsOuter->winding->points[bridgeOuter][0]
    && vertsInner->winding->points[bridgeInner][1] == vertsOuter->winding->points[bridgeOuter][1]
    && vertsInner->winding->points[bridgeInner][2] == vertsOuter->winding->points[bridgeOuter][2] )
  {
    bridgeMatch = 1;
    adjustedInner = numPtsInner - 1;
  }
  else
  {
    bridgeMatch = 0;
  }
  verts->winding->numpoints = 0;
  WindingCopyArc((bridgeInner + 1) % numPtsInner, vertsInner, verts, (adjustedInner + bridgeInner) % numPtsInner, auxElemSize);
  WindingCopyArc(bridgeOuter, vertsOuter, verts, (numPtsOuter + bridgeOuter - 1) % numPtsOuter, auxElemSize);
  WindingCopyArc(bridgeOuter, vertsOuter, verts, bridgeOuter, auxElemSize);
  if ( !bridgeMatch )
    WindingCopyArc(bridgeInner, vertsInner, verts, bridgeInner, auxElemSize);
  return verts->winding;
}

/*
================
FindBestBridgeEdge

Finds the shortest non-crossing non-concave bridge edge
between an inner and outer winding for hole insertion
================
*/
char FindBestBridgeEdge(float *plane, Winding_t *wInner, Winding_t *wOuter, WindingAuxPair_t *holeWindings, int holeCount, int *outInnerIdx, int *outOuterIdx, float *bestDistSq)
{
  int iI, iO, nI, nO;
  int axisU, axisV;
  char found = 0;

  Assert(plane, s_assertDisable_FindBestBridgeEdge);
  Assert(wInner, s_assertDisable_FindBestBridgeEdge);
  Assert(wOuter, s_assertDisable_FindBestBridgeEdge);
  Assert(wInner != wOuter, s_assertDisable_FindBestBridgeEdge);
  Assert(WindingSignedArea(wOuter, plane) >= 0.0, s_assertDisable_FindBestBridgeEdge);
  Assert(WindingSignedArea(wInner, plane) <= 0.0, s_assertDisable_FindBestBridgeEdge);
  GetProjectionAxes(plane, &axisU, &axisV);

  nI = wInner->numpoints;
  nO = wOuter->numpoints;

  for ( iI = 0; iI < nI; iI++ )
  {
    float *vI = wInner->points[iI];
    int prevI = (iI + nI - 1) % nI;
    int nextI = (iI + 1) % nI;

    for ( iO = 0; iO < nO; iO++ )
    {
      float *vO = wOuter->points[iO];
      double distSq;

      distSq = VecDistSq(vI, vO);
      if ( distSq >= *bestDistSq )
        continue;

      /* check for crossing edges */
      if ( WindingHasCrossingEdge(wInner, vI, vO, axisU, axisV) )
        continue;
      if ( WindingHasCrossingEdge(wOuter, vI, vO, axisU, axisV) )
        continue;

      /* check for concavity at all four edge junctions */
      if ( IsEdgeConcave(vO, vI, wInner, plane, wInner->points[nextI]) )
        continue;
      if ( IsEdgeConcave(wInner->points[prevI], vI, wInner, plane, vO) )
        continue;
      { int prevO = (iO + nO - 1) % nO;
      if ( IsEdgeConcave(wOuter->points[prevO], vO, wOuter, plane, vI) )
        continue;
      }
      { int nextO = (iO + 1) % nO;
      if ( IsEdgeConcave(vI, vO, wOuter, plane, wOuter->points[nextO]) )
        continue;
      }

      /* check for crossing edges in any hole winding */
      if ( AnyWindingHasCrossingEdge(axisU, axisV, holeWindings, holeCount, vI, vO) )
        continue;

      /* all checks passed — new best bridge */
      *bestDistSq = (float)distSq;
      *outInnerIdx = iI;
      *outOuterIdx = iO;
      found = 1;
    }
  }
  return found;
}

/* CoD4 0x458BF0.  Small holes, or holes with too little area per point, do
   not need the expensive opaque-brush visible-hull reduction. */
static bool HoleAreaSkipsVisibilityReduction(const Winding_t *winding, float area)
{
  return area >= 0.0625f
      && (float)winding->numpoints * 64.00099945068359f <= area;
}

/*
================
InsertHole

Recursively splits a winding at self-touching diagonals and
inserts the resulting hole windings into a sorted array by area
================
*/
int InsertHole(WindingAuxPair_t *verts, float area, float *plane,
               WindingAuxPair_t *holes, float *holeArea, int holeCount,
               int auxElemSize, int a8)
{
  #define MAX_HOLES 3276
  int startDiag0, endDiag0, startDiag1, endDiag1;
  int i, insertIdx;

  Assert(verts, s_assertDisable_InsertHole);
  Assert(verts->winding, s_assertDisable_InsertHole);
  Assert(verts->winding->numpoints >= 3, s_assertDisable_InsertHole);
  Assert(area < 0.0, s_assertDisable_InsertHole);
  Assert(plane, s_assertDisable_InsertHole);
  Assert(holes, s_assertDisable_InsertHole);
  Assert(holeArea, s_assertDisable_InsertHole);
  Assert(holeCount >= 0 && holeCount + 1 < MAX_HOLES, s_assertDisable_InsertHole);

  if ( FindWindingDiagonal(verts->winding, &startDiag0, &endDiag0, &startDiag1, &endDiag1) )
  {
    WindingAuxPair_t pairs[2];

    if ( startDiag0 == startDiag1 )
    {
      FreeVerts(verts);
      return holeCount;
    }

    AllocWindingForArc(auxElemSize, &pairs[0], verts, endDiag0, startDiag0);
    AllocWindingForArc(auxElemSize, &pairs[1], verts, startDiag1, endDiag1);
    FreeVerts(verts);

    for ( i = 0; i < 2; i++ )
    {
      double subArea = WindingSignedArea(pairs[i].winding, plane);
      if ( subArea < 0.0 )
        holeCount = InsertHole(&pairs[i], (float)subArea, plane, holes, holeArea, holeCount, auxElemSize, a8);
      else
        FreeVerts(&pairs[i]);
    }
    return holeCount;
  }

  /* no diagonal — insert this hole sorted by area (descending by magnitude) */
  /* CoD4 0x458780 reduces every eligible concave hole through the
     opaque-brush tree; its final parameter is unused. */
  if (!HoleAreaSkipsVisibilityReduction(verts->winding, -area))
  {
    Winding_t *reversed = ReverseWinding(verts->winding);

    if (WindingIsConvex(reversed, plane))
    {
      Winding_t *visible = BrushSides_BuildVisibleHull(reversed, NULL, plane);
      if (!visible)
      {
        FreeVerts(verts);
        return holeCount;
      }
      if (visible != reversed)
      {
        float visibleArea = (float)WindingSignedArea(visible, plane);

        if (visibleArea == 0.0f)
        {
          FreeWinding(visible);
          FreeVerts(verts);
          return holeCount;
        }
        Assert(visibleArea <= -area + 0.001000000047497451f,
               s_assertDisable_InsertHole);
        if (visibleArea >= -area - 0.001000000047497451f
            || HoleAreaSkipsVisibilityReduction(visible, visibleArea))
        {
          Winding_t *reduced = ReverseWinding(visible);
          void *reducedAux = NULL;

          /* The only compatibility auxiliary route through concave merging
             is the shadow-caster SmAuxVert carrier.  Reprojecting the
             retained hull vertices reconstructs its original/projected
             pairs after native's geometry-only visibility reduction. */
          if (auxElemSize == sizeof(SmAuxVert_t))
            reducedAux = SM_AllocProjectedPoints(reduced);
          else if (auxElemSize)
            Com_Error("InsertHole: unsupported auxiliary vertex size %i", auxElemSize);
          FreeVerts(verts);
          verts->winding = reduced;
          verts->auxData = reducedAux;
          FreeWinding(visible);
          area = -visibleArea;
        }
        else
        {
          FreeWinding(visible);
        }
      }
      else
      {
        FreeWinding(visible);
      }
    }
    else
    {
      FreeWinding(reversed);
    }
  }

  { float negArea = -area;
  for ( insertIdx = holeCount; insertIdx > 0 && holeArea[insertIdx - 1] < (double)negArea; insertIdx-- )
  {
    holes[insertIdx] = holes[insertIdx - 1];
    holeArea[insertIdx] = holeArea[insertIdx - 1];
  }
  holes[insertIdx] = *verts;
  holeArea[insertIdx] = negArea;
  }
  return holeCount + 1;
  #undef MAX_HOLES
}

/*
================
FindHoles

Finds holes in a winding by splitting at self-touching
diagonals and collecting the resulting sub-windings
================
*/
int FindHoles(WindingAuxPair_t *verts, float *plane, WindingAuxPair_t *holes,
              float *holeArea, int auxElemSize, int extraParam)
{
  WindingAuxPair_t *src = verts;
  WindingAuxPair_t pairs[2];
  float areas[2];
  int end0, start0, start1, end1;
  int outerIdx, innerIdx, holeCount;

  holeCount = 0;
  if ( !FindWindingDiagonal(src->winding, &end0, &start0, &start1, &end1) )
    return 0;

  while ( 1 )
  {
    int s1 = start1, e1 = end1;

    if ( end0 == start1 )
      return -1;

    /* when both diagonal endpoints coincide, swap the sub-winding bounds */
    if ( start0 == start1 )
    {
      Assert(end0 == end1, s_assertDisable_FindHoles);
      Assert(start0 != end0, s_assertDisable_FindHoles);
      s1 = end0;
      e1 = start0;
    }
    else
    {
      Assert(end0 != end1, s_assertDisable_FindHoles);
    }

    /* CoD4 0x458230 calls the common 0x458710 arc allocator for both
       diagonal pieces.  Keeping that route also keeps the sidecar carrier
       synchronized with the native winding operation. */
    AllocWindingForArc(auxElemSize, &pairs[0], src, start0, end0);
    AllocWindingForArc(auxElemSize, &pairs[1], src, s1, e1);

    areas[0] = (float)WindingSignedArea(pairs[0].winding, plane);
    areas[1] = (float)WindingSignedArea(pairs[1].winding, plane);

    if ( (areas[0] >= 0.0 && areas[1] >= 0.0) || end0 == s1 )
    {
      /* both positive or degenerate — keep the larger, discard the other */
      outerIdx = areas[1] > (double)areas[0];
      FreeVerts(&pairs[1 - outerIdx]);
    }
    else
    {
      /* one is a hole (negative area), one is outer (positive) */
      AssertFatalMsg(areas[0] >= 0.0 || areas[1] >= 0.0, s_assertDisable_FindHoles,
             "%s\n\t%s", "subArea[0] >= 0 || subArea[1] >= 0", va("%g, %g", areas[0], areas[1]));

      innerIdx = areas[0] >= 0.0 ? 1 : 0;
      outerIdx = 1 - innerIdx;

      AssertFatalMsg(areas[outerIdx] > -areas[innerIdx], s_assertDisable_FindHoles,
             "%s\n\t%s", "subArea[outerIndex] > -subArea[innerIndex]", va("%g > %g", areas[outerIdx], -areas[innerIdx]));

      holeCount = InsertHole(&pairs[innerIdx], areas[innerIdx], plane, holes, holeArea, holeCount, auxElemSize, extraParam);
    }

    /* replace source winding with the outer sub-winding */
    FreeVerts(src);
    src->winding = pairs[outerIdx].winding;
    src->auxData = pairs[outerIdx].auxData;

    if ( !FindWindingDiagonal(src->winding, &end0, &start0, &start1, &end1) )
      return holeCount;
  }
}

/*
================
WindingInsideBSP

Recursively tests whether a winding is contained within
the solid space of a BSP tree
================
*/
char WindingInsideBSP(int *w, float *plane, Node_t *node)
{
  #define COPLANAR_DOT  0.99900001f
  #define COPLANAR_DIST 0.001f
  float *splitPlane;
  double dot;

  if ( !w )
    return 0;

  /* leaf node — free winding and return whether cell is non-opaque */
  if ( node->cellnum != CELLNUM_UNKNOWN )
  {
    FreeWinding((Winding_t *)w);
    return node->cellnum != CELLNUM_OPAQUE;
  }

  splitPlane = MAP_PLANE(node->planenum)->normal;
  dot = DotProduct210(splitPlane, plane);

  /* coplanar with split plane — recurse into back child only */
  if ( dot > COPLANAR_DOT && fabs(plane[3] - splitPlane[3]) < COPLANAR_DIST )
    return WindingInsideBSP(w, plane, node->children[0]);

  /* coplanar with flipped split plane — recurse into front child only */
  if ( dot < -COPLANAR_DOT && fabs(splitPlane[3] + plane[3]) < COPLANAR_DIST )
    return WindingInsideBSP(w, plane, node->children[1]);

  /* split the winding and check both sides */
  { Winding_t *backW, *frontW;
  char result;
  ClipWindingEpsilon((Winding_t *)w, splitPlane, splitPlane[3], ON_EPSILON, &backW, &frontW, 0);
  result = WindingInsideBSP((int *)backW, plane, node->children[0]);
  if ( !result )
    result = WindingInsideBSP((int *)frontW, plane, node->children[1]);
  FreeWinding((Winding_t *)w);
  return result;
  }
  #undef COPLANAR_DOT
  #undef COPLANAR_DIST
}

/*
================
IsValidHole

Validates a hole by checking area threshold and
testing BSP containment
================
*/
bool IsValidHole(Tree_t *bspHead, Winding_t *w, float area, float *plane)
{
  #define MIN_HOLE_AREA     0.0625
  #define AREA_PER_VERTEX   64.000999

  Assert(w, s_assertDisable_IsValidHole);
  if ( area < MIN_HOLE_AREA )
    return 0;
  if ( !bspHead || (double)w->numpoints * AREA_PER_VERTEX < area )
    return 1;
  return WindingInsideBSP((int *)ReverseWinding(w), plane, bspHead->headnode) != 0;

  #undef MIN_HOLE_AREA
  #undef AREA_PER_VERTEX
}

/*
================
CollectHoles

Collects holes from a surface winding by finding self-touching diagonals.
CoD4's per-hole opaque-brush visibility reduction happens in InsertHole.
================
*/
void CollectHoles(TriSurf_t *ts, Tree_t *bspHead, int auxElemSize)
{
  #define MAX_HOLE_BUF 3276
  WindingAuxPair_t *src = (WindingAuxPair_t *)&ts->winding;
  WindingAuxPair_t copyPair;
  WindingAuxPair_t holesBuf[MAX_HOLE_BUF];
  float areasBuf[MAX_HOLE_BUF];
  int totalHoles, i;

  (void)bspHead;

  ts->holeCount = 0;
  ts->holes = 0;

  if ( ts->winding->numpoints <= 8 )
    return;

  CopyVerts(src, ts->auxElemSize, &copyPair);
  totalHoles = FindHoles(&copyPair, ts->props->plane, holesBuf, areasBuf,
                         ts->auxElemSize, auxElemSize);
  Assert(totalHoles >= 0, s_assertDisable_FindHoles);

  if ( totalHoles > 0 )
  {
    for ( i = 0; i < totalHoles; i++ )
    {
      int n = ts->holeCount;
      holesBuf[n] = holesBuf[i];
      ts->holeCount = n + 1;
    }

    if ( ts->holeCount )
    {
      int sz = sizeof(WindingAuxPair_t) * ts->holeCount;
      ts->holes = (WindingAuxPair_t *)malloc(sz);
      memcpy(ts->holes, holesBuf, sz);
    }

  }

  /* Native 0x458110 always publishes the copied/processed carrier for a
     winding above the eight-point gate, even when FindHoles found none. */
  FreeVerts(src);
  src->winding = copyPair.winding;
  src->auxData = copyPair.auxData;
  #undef MAX_HOLE_BUF
}

/*
================
MergeHoles

Merges all collected holes into the surface winding by
finding bridge edges and splicing the windings together
================
*/
void MergeHoles( TriSurf_t *ts )
{
  WindingAuxPair_t *holes;
  WindingAuxPair_t basePair, holePair, mergedPair;
  Winding_t *mergedW, *baseW;
  void *mergedAux, *baseAux;
  int totalPts, bridgeInner, bridgeOuter, best, i;
  float bestDistSq;

  if ( !ts->holeCount )
    return;

  /* count total points needed: outer + all holes + 2 per bridge */
  totalPts = ts->winding->numpoints;
  holes = ts->holes;
  for ( i = 0; i < ts->holeCount; i++ )
    totalPts += holes[i].winding->numpoints + 2;

  mergedW = AllocWinding( totalPts );
  AssertFatal( mergedW->numpoints == 0, s_assertDisable_MergeHoles );

  mergedAux = ts->auxElemSize ? malloc( totalPts * ts->auxElemSize ) : 0;
  baseW = ts->winding;
  baseAux = ts->auxData;
  basePair.winding = baseW;
  basePair.auxData = baseAux;
  mergedPair.winding = mergedW;
  mergedPair.auxData = mergedAux;

  /* repeatedly find nearest hole, bridge it into the base winding */
  while ( ts->holeCount )
  {
    best = -1;
    bridgeInner = 0;
    bridgeOuter = 0;
    bestDistSq = FLT_MAX;

    for ( i = 0; i < ts->holeCount; i++ )
    {
      Assert( totalPts >= ts->holes[i].winding->numpoints + baseW->numpoints + 2, s_assertDisable_MergeHoles );
      if ( FindBestBridgeEdge(
             ts->props->plane,
             ts->holes[i].winding,
             baseW,
             ts->holes,
             ts->holeCount,
             &bridgeInner,
             &bridgeOuter,
             &bestDistSq ) )
      {
        best = i;
      }
    }
    Assert( best >= 0, s_assertDisable_MergeHoles );

    /* merge chosen hole into base, output into mergedW */
    holePair = ts->holes[best];
    MergeWindingsAtBridge(&holePair, &basePair, bridgeInner, bridgeOuter,
                          ts->auxElemSize, &mergedPair);

    FreeWinding( baseW );
    FreeWinding( ts->holes[best].winding );
    free( baseAux );
    free( ts->holes[best].auxData );

    /* swap-remove the merged hole */
    ts->holeCount--;
    ts->holes[best] = ts->holes[ts->holeCount];

    if ( !ts->holeCount )
      break;

    /* copy merged result as new base for next iteration */
    baseW = CopyWinding( mergedW );
    if ( ts->auxElemSize )
    {
      baseAux = malloc( ts->auxElemSize * mergedW->numpoints );
      memcpy( baseAux, mergedAux, ts->auxElemSize * mergedW->numpoints );
    }
    else
    {
      baseAux = NULL;
    }
    basePair.winding = baseW;
    basePair.auxData = baseAux;
  }

  free( ts->holes );
  ts->winding = mergedW;
  ts->holes = 0;
  ts->auxData = mergedAux;
}

/*
================
RemoveDuplicateIndices

Detects degenerate A-B-A spike patterns (indices[im2] == indices[cur],
distance 2 apart) and removes the spike vertex pair.
Restarts the full scan after each removal.
================
*/
int RemoveDuplicateIndices(int numPts, int *indices, char *auxData, int auxElemSize)
{
  int n = numPts;

  for (;;)
  {
    int im2, prev, cur;
    int matched = 0;

    im2 = n - 2;
    prev = n - 1;

    for ( cur = 0; cur < n; cur++ )
    {
      if ( indices[im2] != indices[cur] )
      {
        im2 = prev;
        prev = cur;
        continue;
      }

      /* degenerate spike (A-B-A) found — remove 2 vertices */
      n -= 2;
      if ( n < 3 )
        return 0;

      /* if prev or im2 landed exactly at new n, the removed vertices
         were at the tail — just truncate and restart */
      if ( prev == n || im2 == n )
      {
        matched = 1;
        break;
      }

      if ( prev == 0 )
      {
        /* wrap case: spike spans end/start — remove first 2 elements */
        memcpy(indices, &indices[2], sizeof(int) * n);
        if ( auxElemSize )
          memcpy(auxData, &auxData[auxElemSize * 2], auxElemSize * n);
      }
      else
      {
        /* normal case: shift from cur to im2, removing prev and cur-1 */
        int tail = n - im2;
        Assert(n + 2 - cur > 0, s_assertDisable_RemoveDuplicateIndices);
        Assert(im2 < cur, s_assertDisable_RemoveDuplicateIndices);
        memcpy(&indices[im2], &indices[cur], sizeof(int) * tail);
        if ( auxElemSize )
          memcpy(&auxData[auxElemSize * im2], &auxData[auxElemSize * (im2 + 2)], auxElemSize * tail);
      }

      matched = 1;
      break;
    }

    if ( !matched )
      break;
  }

  return n;
}

/*
================
BuildIndexList

Converts a float winding to an integer winding by mapping
vertices through the merge vertex table and removing duplicates
================
*/
IntWinding_t *BuildIndexList(WindingAuxPair_t *vertsPair, int auxElemSize)
{
  #define MAX_INDEX_LIST_POINTS 1024
  Winding_t *w = vertsPair->winding;
  int indices[MAX_INDEX_LIST_POINTS];
  void *auxCopy = auxElemSize ? malloc(auxElemSize * w->numpoints) : 0;
  int i, n, dstCount = 0;
  IntWinding_t *result = 0;

  Assert(w->numpoints <= MAX_INDEX_LIST_POINTS, s_assertDisable_BuildIndexList);

  /* map each float vertex to a merge-table index, skip consecutive duplicates */
  n = w->numpoints;
  for ( i = 0; i < n; i++ )
  {
    int idx = GetOrAddMergeVertex((int *)w->points[i]);
    indices[dstCount] = idx;
    if ( !dstCount || idx != indices[dstCount - 1] )
    {
      if ( auxElemSize )
        AuxDataCopy(auxElemSize, (char *)vertsPair->auxData, i, 1, (char *)auxCopy, dstCount);
      dstCount++;
    }
  }

  /* trim trailing vertices that duplicate the first */
  while ( dstCount && indices[dstCount - 1] == indices[0] )
    dstCount--;

  if ( dstCount >= 3 )
  {
    int cleanCount = RemoveDuplicateIndices(dstCount, indices, (char *)auxCopy, auxElemSize);
    if ( cleanCount >= 3 )
    {
      IntWinding_t *iw = AllocIntWinding(cleanCount, auxElemSize);
      iw->numpoints = cleanCount;
      memcpy(iw->indices, indices, sizeof(int) * cleanCount);
      if ( auxElemSize )
        memcpy(iw->auxData, auxCopy, auxElemSize * cleanCount);
      result = iw;
    }
  }

  free(auxCopy);
  return result;
  #undef MAX_INDEX_LIST_POINTS
}

/*
================
InitVertMap

Initializes the merge vertex map and builds index lists
for all surfaces in the merge group
================
*/
int InitVertMap(IntWinding_t **indexLists, int totalVerts)
{
  int i;

  Assert(!mergeVertMap, s_assertDisable_InitVertMap);
  Assert(!mergeVertMapCount, s_assertDisable_InitVertMap);

  for ( i = 0; i < (int)mergeSurfCount; i++ )
    totalVerts += mergeSurfArray[i]->winding->numpoints;
  mergeVertMap = malloc(sizeof(vec3_t) * totalVerts);
  mergeVertMapCount = 0;

  i = 0;
  while ( i < (int)mergeSurfCount )
  {
    indexLists[i] = BuildIndexList((WindingAuxPair_t *)&mergeSurfArray[i]->winding, mergeSurfArray[i]->auxElemSize);
    FreeVerts((WindingAuxPair_t *)&mergeSurfArray[i]->winding);
    if ( indexLists[i] )
    {
      i++;
    }
    else
    {
      /* degenerate surface — remove and shift array */
      mergeSurfCount--;
      FreeTriSurf(mergeSurfArray[i]);
      memcpy(&mergeSurfArray[i], &mergeSurfArray[i + 1], sizeof(TriSurf_t *) * (mergeSurfCount - i));
    }
  }
  return i;
}

/*
================
CanRemoveVert

Tests if a vertex can be removed from the winding without
creating any crossing edges
================
*/
char CanRemoveVert(int vertIdx, Winding_t *w, int anchorIdx, int axisU, int axisV)
{
  int next, test;

  next = (vertIdx + 1) % w->numpoints;
  test = (vertIdx + 2) % w->numpoints;

  /* Native 0x456A30 walks every remaining edge and rejects the removal when
     the replacement edge would intersect one of them in projection. */
  while ( test != anchorIdx )
  {
    if ( WindingEdgesIntersect(w->points[anchorIdx], w->points[vertIdx],
                               w->points[next], w->points[test], axisU, axisV) )
      return 0;
    next = test;
    test = (test + 1) % w->numpoints;
  }
  return 1;
}

/*
================
CollapseCollinearVerts

Removes collinear vertices from a winding by checking if
adjacent vertices are coplanar and can be safely removed
================
*/
Winding_t *CollapseCollinearVerts(Winding_t **verts, int auxElemSize, int axisU, int axisV, float epsilon)
{
  Winding_t *w;
  char *auxData;
  int i;

  Assert(verts, s_assertDisable_CollapseCollinearVerts);
  Assert(*verts, s_assertDisable_CollapseCollinearVerts);

  w = *verts;
  auxData = (char *)(intptr_t)((int **)verts)[1];
  i = 0;

  while ( i < w->numpoints )
  {
    int next = (i + 1) % w->numpoints;
    int test = (i + 2) % w->numpoints;

    if ( (PlaneEqual(w->points[i], w->points[next], w->points[test], epsilon)
          || VectorCompareEpsilon(w->points[next], w->points[test], epsilon, 3))
      && CanRemoveVert(test, w, i, axisU, axisV) )
    {
      /* remove the middle vertex (next) by shifting tail left */
      w->numpoints--;
      if ( test )
      {
        memcpy(w->points[next], w->points[test], sizeof(vec3_t) * (w->numpoints - next));
        if ( auxElemSize )
          memcpy(&auxData[auxElemSize * next], &auxData[auxElemSize * (next + 1)], auxElemSize * (w->numpoints - next));
      }
    }
    else
    {
      i++;
    }
  }
  return w;
}

/*
================
ValidateHoles

Validates holes by collapsing collinear vertices and
removing holes that become degenerate or have wrong winding
================
*/
char ValidateHoles(TriSurf_t *ts, float epsilon)
{
  int axisU, axisV;
  int i;

  GetProjectionAxes(ts->props->plane, &axisU, &axisV);
  CollapseCollinearVerts(&ts->winding, ts->auxElemSize, axisU, axisV, epsilon);
  if ( ts->winding->numpoints < 3 )
    return 0;

  i = 0;
  while ( i < ts->holeCount )
  {
    WindingAuxPair_t *hole = &ts->holes[i];
    CollapseCollinearVerts(&hole->winding, ts->auxElemSize, axisU, axisV, epsilon);

    /* keep valid holes (>= 3 points, negative signed area) */
    if ( hole->winding->numpoints >= 3 && WindingSignedArea(hole->winding, ts->props->plane) < 0.0 )
    {
      i++;
      continue;
    }

    /* invalid hole — free and swap-remove from array */
    FreeWinding(hole->winding);
    free(hole->auxData);
    ts->holeCount--;
    if ( !ts->holeCount )
    {
      free(ts->holes);
      ts->holes = 0;
      return 1;
    }
    *hole = ts->holes[ts->holeCount];
  }
  return 1;
}

/*
================
RemoveSharedBoundaryPoints

Removes shared boundary points from a merged winding after
splicing, handling wrap-around and collinear vertex collapse
================
*/
IntWinding_t *RemoveSharedBoundaryPoints(IntWinding_t *w0, int auxElemSize, int startIdx, int dupCount)
{
  int n, start, count, remEnd, next, prev;
  char endsMismatch = 0;

  start = startIdx;
  count = dupCount;

  /* if boundary endpoints don't match, shrink the removal range inward */
  if ( w0->indices[startIdx] != w0->indices[(startIdx + dupCount) % w0->numpoints] )
  {
    start = (startIdx + 1 == w0->numpoints) ? 0 : startIdx + 1;
    count = dupCount - 2;
    dupCount -= 2;
    endsMismatch = 1;
  }

  n = w0->numpoints;
  Assert(n - count >= 3 || n == count || s_assertDisable_RemoveSharedBoundaryPoints,
         s_assertDisable_RemoveSharedBoundaryPoints);

  if ( n == count )
    return w0;

  /* remove 'count' indices starting at 'start', handling wrap-around */
  remEnd = start + count;
  if ( remEnd != n )
  {
    if ( remEnd > n )
    {
      remEnd -= n;
      start = 0;
    }
    memcpy(&w0->indices[start], &w0->indices[remEnd], sizeof(int) * (n - remEnd));
    if ( auxElemSize )
      WindingArrayShift(auxElemSize, (intptr_t)w0->auxData, start, remEnd - start, start + w0->numpoints - remEnd);
  }
  w0->numpoints -= dupCount;

  /* if endpoints matched, iteratively collapse newly-adjacent duplicates */
  if ( !endsMismatch )
  {
    n = w0->numpoints;
    prev = (n + start - 1) % n;
    next = (start + 1) % n;

    while ( w0->indices[prev] == w0->indices[next] )
    {
      w0->numpoints -= 2;
      n = w0->numpoints;

      if ( next )
      {
        if ( start == n )
        {
          start = prev;
        }
        else
        {
          memcpy(&w0->indices[start], &w0->indices[start + 2], sizeof(int) * (n - start));
          if ( auxElemSize )
            memcpy((void *)((intptr_t)w0->auxData + start * auxElemSize),
                   (const void *)((intptr_t)w0->auxData + auxElemSize * (start + 2)),
                   auxElemSize * n - start * auxElemSize);
          start = (start < prev) ? prev - 2 : prev;
        }
      }
      else
      {
        start = 0;
      }

      Assert(w0->numpoints >= 3, s_assertDisable_RemoveSharedBoundaryPoints);
      next = (start + 1) % w0->numpoints;
      prev = (w0->numpoints + start - 1) % w0->numpoints;
    }

    return w0;
  }

  return w0;
}

/*
================
SpliceWindingsAtSharedEdge

Splices two windings at a shared edge into a combined winding
================
*/
IntWinding_t *SpliceWindingsAtSharedEdge(IntWinding_t *w0, IntWinding_t *w1, int auxElemSize, int unused4, int unused5, int startA, int startB, int dupCount)
{
  int nA = w0->numpoints, nB = w1->numpoints;
  IntWinding_t *out = AllocIntWinding(nB - 2 * dupCount + nA + 2, auxElemSize);
  int i, skip;

  #define OUT_ADD(src, srcIdx) do { \
    out->indices[out->numpoints] = (src)->indices[((srcIdx) % (src == w0 ? nA : nB))]; \
    if ( auxElemSize ) \
      AuxDataCopy(auxElemSize, (char *)(src)->auxData, (srcIdx) % (src == w0 ? nA : nB), 1, (char *)out->auxData, out->numpoints); \
    out->numpoints++; \
  } while(0)

  out->numpoints = 0;
  AssertFatal(dupCount > 1, s_assertDisable_SpliceWindingsAtSharedEdge);
  AssertFatal(nA > 1, s_assertDisable_SpliceWindingsAtSharedEdge);

  /* copy non-shared portion of w0 (skip first dupCount-1 shared vertices) */
  for ( i = dupCount - 1; i < nA; i++ )
    OUT_ADD(w0, i + startA);

  AssertFatal(out->numpoints, s_assertDisable_SpliceWindingsAtSharedEdge);

  /* skip leading w1 vertices that duplicate the last output vertex */
  skip = 0;
  { int w1Remain = nB - dupCount;
  while ( skip <= w1Remain && w1->indices[(skip + startB) % nB] == out->indices[out->numpoints - 1] )
    skip++;
  }

  /* copy non-shared portion of w1 */
  for ( i = skip; i <= nB - dupCount; i++ )
    OUT_ADD(w1, i + startB);

  /* trim trailing vertices that duplicate the first */
  while ( out->numpoints > 0 && out->indices[out->numpoints - 1] == out->indices[0] )
    out->numpoints--;

  return out;
  #undef OUT_ADD
}

/*
================
MergeGroupSurfaces

Orchestrates merge: sort, tjunc fix, vertex map, merge callback
================
*/
size_t MergeGroupSurfaces(
        int mode,
        TriSurf_t *baseSurf,
        MergeCallback_t mergeCallback,
        int mergeFlags)
{
  IntWinding_t *indexLists[MAX_CONCAVE_INDEX_LISTS];
  IntWinding_t *baseIndexList = 0;
  int i, baseVertCount;

  Assert((int)mergeSurfCount >= 1, s_assertDisable_MergeGroupSurfaces);
  RemoveOverlappingWindings(mode);
  if ( mergeSurfCount == 1 && !baseSurf )
    return 1;

  /* sort surfaces by projection axis position (skip first if second pass) */
  if ( mode == 2 )
    qsort(&mergeSurfArray[1], mergeSurfCount - 1, sizeof(TriSurf_t *), CompareSurfaces);
  else
    qsort(mergeSurfArray, mergeSurfCount, sizeof(TriSurf_t *), CompareSurfaces);

  TjuncFixFaces((intptr_t *)mergeSurfArray, (int)mergeSurfCount, baseSurf);
  baseVertCount = baseSurf ? baseSurf->winding->numpoints : 0;
  InitVertMap(indexLists, baseVertCount);

  if ( baseSurf )
    baseIndexList = BuildIndexList((WindingAuxPair_t *)&baseSurf->winding, baseSurf->auxElemSize);

  if ( !baseSurf || baseIndexList )
    mergeSurfCount = mergeCallback(mergeSurfArray, indexLists, mergeSurfCount, mergeSurfArray[0]->auxElemSize, baseSurf, baseIndexList, mergeFlags);

  /* convert intWindings back to float windings */
  for ( i = 0; i < (int)mergeSurfCount; i++ )
  {
    IntWindingToWinding(indexLists[i], &mergeSurfArray[i]->winding);
    FreeIntWinding(indexLists[i]);
  }

  if ( baseSurf && baseIndexList )
  {
    IntWindingToWinding(baseIndexList, &baseSurf->winding);
    FreeIntWinding(baseIndexList);
  }

  FreeMergeVertMap();
  return mergeSurfCount;
}

/*
================
MergeVisGroupList

Merges surfaces in vis group via grid tree flood-fill
================
*/
/* CoD4 0x458DB0.  Finds the largest run after pointIndex that can be
   discarded without exposing a concave or visible notch. */
static int FindNotchSpan(Winding_t *winding, int pointIndex, float *plane)
{
  int first = (pointIndex + 1) % winding->numpoints;
  int current = first;
  int previous = pointIndex;
  int span;

  for (span = 0; ; ++span)
  {
    int next = (current + 1) % winding->numpoints;

    Assert(next != pointIndex, s_assertDisable_FindNotchSpan);
    if (WindingTripleIsConcave(winding, next, current, previous, plane)
        || WindingTripleIsConcave(winding, pointIndex, next, current, plane)
        || WindingTripleIsConcave(winding, first, pointIndex, next, plane))
      break;

    previous = current;
    current = next;
  }

  while (span)
  {
    Winding_t *notch = AllocWinding(span + 2);
    int i;

    notch->numpoints = span + 2;
    for (i = 0; i < notch->numpoints; ++i)
      VectorCopy(winding->points[(notch->numpoints + pointIndex - i - 1) % winding->numpoints],
                 notch->points[i]);

    Assert(WindingIsConvex(notch, plane), s_assertDisable_FindNotchSpan);
    /* BrushSides_IsWindingVisible consumes notch through its clipping list. */
    if (!BrushSides_IsWindingVisible(notch))
      break;
    --span;
  }
  return span;
}

/* Local copy of the CoD4 0x42E110 clearance test used exclusively by the
   native notch repair.  The common implementation is currently TU-local,
   so exposing it here would require widening an unrelated interface. */
static float NotchSmallestEdgeDistance(const Winding_t *winding,
                                       float *planeNormal)
{
  float smallest = FLT_MAX;
  int previous;
  int current;

  Assert(winding, s_assertDisable_TryPlugNotch);
  Assert(winding->numpoints >= 3, s_assertDisable_TryPlugNotch);
  previous = winding->numpoints - 1;
  for (current = 0; current < winding->numpoints; ++current)
  {
    vec3_t edge, edgePlane;
    float edgeLength, edgeDist, largest = -FLT_MAX;
    int test = (current + 1) % winding->numpoints;

    VectorSubtract(winding->points[current], winding->points[previous], edge);
    CrossProduct(edge, planeNormal, edgePlane);
    /* Native 0x42E110 normalizes the edge plane in place before all dot
       distances.  The returned pre-normalization length is retained only
       for the degenerate-edge assertion below. */
    edgeLength = (float)VecNormalize(edgePlane);
    edgeDist = (float)DotProduct(edgePlane, winding->points[previous]);

    do
    {
      float distance = DotProduct(winding->points[test], edgePlane) - edgeDist;
      Assert(distance >= -0.10000000149011612f || edgeLength < 1.0f,
             s_assertDisable_TryPlugNotch);
      if (largest > distance)
        break;
      largest = distance;
      test = (test + 1) % winding->numpoints;
    } while (test != previous);

    if (largest < smallest)
      smallest = largest;
    previous = current;
  }
  return smallest;
}

/* CoD4 0x459370.  The replacement point may not cross the outer boundary
   or any retained hole boundary. */
static bool CanInsertNotchVertex(TriSurf_t *ts, float *candidate,
                                 float *edgeEnd, int axisU, int axisV)
{
  return !WindingHasCrossingEdge(ts->winding, candidate, edgeEnd, axisU, axisV)
      && !AnyWindingHasCrossingEdge(axisU, axisV, ts->holes, ts->holeCount,
                                    candidate, edgeEnd);
}

/* CoD4 0x458F80.  Intersects the two notch boundary rays, projects their
   midpoint onto the surface plane, and accepts it only when the small test
   triangle is fully opaque and both replacement edges remain uncrossed. */
static char TryPlugNotch(TriSurf_t *ts, int pointIndex, unsigned int endIndex,
                         float *plane, int axisU, int axisV)
{
  Winding_t *winding = ts->winding;
  unsigned int previous = (pointIndex + winding->numpoints - 1) % winding->numpoints;
  unsigned int end = endIndex % winding->numpoints;
  unsigned int next = (endIndex + 1) % winding->numpoints;
  vec3_t edgeA, edgeB, toEnd, candidateA, candidateB, candidate;
  float dotAA, dotAB, dotBB, determinant, alongA, alongB, planeError;
  Winding_t *testTriangle;

  if (next == previous)
    return 0;
  Assert(previous != end && pointIndex != (int)end && pointIndex != (int)next,
         s_assertDisable_TryPlugNotch);

  VectorSubtract(winding->points[pointIndex], winding->points[previous], edgeA);
  VectorSubtract(winding->points[next], winding->points[end], edgeB);
  dotAA = DotProduct(edgeA, edgeA);
  dotAB = DotProduct(edgeA, edgeB);
  dotBB = DotProduct(edgeB, edgeB);
  determinant = dotAA * dotBB - dotAB * dotAB;
  if (determinant <= 1.0f)
    return 0;

  VectorSubtract(winding->points[end], winding->points[pointIndex], toEnd);
  alongA = (DotProduct(edgeA, toEnd) * dotBB - DotProduct(edgeB, toEnd) * dotAB)
         / determinant;
  if (alongA <= 0.001000000047497451f)
    return 0;
  alongB = (DotProduct(edgeB, toEnd) * dotAA - DotProduct(edgeA, toEnd) * dotAB)
         / determinant;
  if (alongB <= 0.001000000047497451f)
    return 0;

  VectorMA(winding->points[pointIndex], alongA, edgeA, candidateA);
  VectorMA(winding->points[end], -alongB, edgeB, candidateB);
  candidate[0] = (candidateA[0] + candidateB[0]) * 0.5f;
  candidate[1] = (candidateA[1] + candidateB[1]) * 0.5f;
  candidate[2] = (candidateA[2] + candidateB[2]) * 0.5f;
  planeError = DotProduct(candidate, plane) - plane[3];
  VectorMA(candidate, -planeError, plane, candidate);

  testTriangle = AllocWinding(3);
  testTriangle->numpoints = 3;
  VectorCopy(candidate, testTriangle->points[0]);
  VectorCopy(winding->points[end], testTriangle->points[1]);
  VectorCopy(winding->points[pointIndex], testTriangle->points[2]);

  if (WindingSignedArea(testTriangle, plane) < 0.001000000047497451f
      || NotchSmallestEdgeDistance(testTriangle, plane) > 8.00100040435791f)
  {
    FreeWinding(testTriangle);
    return 0;
  }

  /* BrushSides_IsWindingVisible owns testTriangle once called. */
  if (BrushSides_IsWindingVisible(testTriangle)
      || !CanInsertNotchVertex(ts, candidate, winding->points[pointIndex], axisU, axisV)
      || !CanInsertNotchVertex(ts, candidate, winding->points[end], axisU, axisV))
    return 0;

  VectorCopy(candidate, winding->points[pointIndex]);
  return 1;
}

/* CoD4 0x458C30.  Removes a visibility-safe concave notch in place.  The
   native carrier only has position data; KIWI mirrors the exact removal into
   the optional sidecar so every surviving point keeps its matching payload. */
static char PlugNotchesInWinding(TriSurf_t *ts)
{
  Winding_t *winding = ts->winding;
  int axisU, axisV;
  int pointIndex = 0;
  char changed = 0;

  if (winding->numpoints <= 4)
    return 0;

  GetProjectionAxes(ts->props->plane, &axisU, &axisV);
  while (pointIndex < winding->numpoints)
  {
    int span = FindNotchSpan(winding, pointIndex, ts->props->plane);

    if (span)
    {
      unsigned int end = span + pointIndex + 1;

      if (TryPlugNotch(ts, pointIndex, end, ts->props->plane, axisU, axisV))
      {
        ++end;
        ++span;
      }

      if (end <= (unsigned int)winding->numpoints)
      {
        int copyCount = winding->numpoints - end;
        memcpy(&winding->points[pointIndex + 1], &winding->points[end],
               sizeof(vec3_t) * copyCount);
        if (ts->auxElemSize)
          memcpy((char *)ts->auxData + ts->auxElemSize * (pointIndex + 1),
                 (char *)ts->auxData + ts->auxElemSize * end,
                 ts->auxElemSize * copyCount);
      }
      else
      {
        int copyCount = winding->numpoints - span;
        memcpy(winding->points, &winding->points[end - winding->numpoints],
               sizeof(vec3_t) * copyCount);
        if (ts->auxElemSize)
          memcpy(ts->auxData,
                 (char *)ts->auxData + ts->auxElemSize * (end - winding->numpoints),
                 ts->auxElemSize * copyCount);
      }

      winding->numpoints -= span;
      changed = 1;
      pointIndex = 0;
    }
    else
    {
      ++pointIndex;
    }
  }
  return changed;
}

TriSurf_t *MergeVisGroupList(
        TriSurf_t **visGroupList,
        float *worldMins,
        float *worldMaxs,
        Tree_t *bspHead,
        MergeCallback_t mergeCallback)
{
  #define MAX_SURFACE_HOLES 0xCCC
  TriSurf_t *outputHead = NULL;
  int i;

  Assert(visGroupList, s_assertDisable_MergeVisGroupList);
  if ( !*visGroupList )
    return NULL;

  MergeConcave_BuildGridTree(*visGroupList, worldMins, worldMaxs);
  mergeVisGroupListHead = visGroupList;

  while ( *mergeVisGroupListHead )
  {
    g_mergeCallNum++;
    MergeConcave_FindMergeGroup(*mergeVisGroupListHead);
    MergeGroupSurfaces(1, 0, mergeCallback, 0);

    for ( i = 0; i < (int)mergeSurfCount; i++ )
    {
      TriSurf_t *cur = mergeSurfArray[i];
      CollectHoles(cur, bspHead, 0);
      if ( ValidateHoles(cur, HOLE_VALIDATE_EPSILON)
        && (!PlugNotchesInWinding(cur) || ValidateHoles(cur, HOLE_VALIDATE_EPSILON)) )
      {
        cur->next = outputHead;
        cur->prev = 0;
        if ( outputHead )
          outputHead->prev = cur;
        outputHead = cur;
        AssertFatal(cur->holeCount <= MAX_SURFACE_HOLES, s_assertDisable_MergeVisGroupList);
        AssertFatal((cur->holeCount == 0) == (cur->holes == 0), s_assertDisable_MergeVisGroupList);
      }
      else
      {
        FreeTriSurf(cur);
      }
    }
  }

  *visGroupList = outputHead;
  mergeVisGroupListHead = NULL;
  return *visGroupList;
  #undef MAX_SURFACE_HOLES
}

/*
================
PlugNotchesInList

Plugs notch surfaces by merging with neighbors
================
*/
char PlugNotchesInList(
        TriSurf_t **surfList,
        float *worldMins,
        float *worldMaxs,
        char (*isNotchCallback)(TriSurf_t *),
        MergeCallback_t mergeCallback)
{
  TriSurf_t *cur, *next;
  int i;

  MergeConcave_BuildGridTree(*surfList, worldMins, worldMaxs);

  for ( cur = *surfList; cur; cur = next )
  {
    next = cur->next;
    if ( isNotchCallback(cur) )
      continue;

    MergeConcave_PropagateGroup(cur);
    if ( !mergeSurfCount )
      continue;

    { int prevPts = cur->winding->numpoints;
    MergeGroupSurfaces(1, cur, mergeCallback, 0);
    if ( cur->winding->numpoints != prevPts && !ValidateHoles(cur, HOLE_VALIDATE_EPSILON) )
    {
      UnlinkAndFreeSurf(cur, surfList);
      FreeTriSurf(cur);
    } }

    for ( i = 0; i < (int)mergeSurfCount; i++ )
      FreeTriSurf(mergeSurfArray[i]);
  }
  return (char)mergeSurfCount;
}

/*
================
FreeSurface

Frees a surface allocation
================
*/
void FreeSurface(void *surf)
{
  free(surf);
}

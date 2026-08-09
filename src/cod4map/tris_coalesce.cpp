/*
tris_coalesce.c — Winding coalescing

Reconstructed from cod2map.exe by Rose.

Groups triangle surfaces by shared properties (material, plane, lightmap)
using a spatial BSP tree for fast neighbor lookup. Coplanar surfaces with
matching properties are merged into larger windings to reduce triangle count.
Hash buckets track unique property combinations.
*/

#include "cod4map.h"
#include "fixedpoint.h"
#include <limits.h>
#include <math.h>

CoalesceBspNode_t  *coalesceTreeRoot;
TriSurf_t         **coalesceSurfListHead;

/* Native CoD4 keeps a FixedWinding_t in TriSurf_t::auxData while building
   the coalesce tree.  KIWI's appended compatibility payload owns auxData,
   so retain the native fixed bounds in an out-of-line sidecar instead. */
typedef struct NativeCoalesceBounds_s {
  TriSurf_t *surf;
  int mins[3];
  int maxs[3];
  unsigned short checkCount;
  unsigned char alreadyVisited;
  struct NativeCoalesceBounds_s *next;
} NativeCoalesceBounds_t;

/* The executable addresses this state directly from its transient surface
   carrier.  KIWI cannot occupy that slot, so use an identity hash instead of
   the earlier O(surface-count) sidecar scan.  This table is private to one
   coalesce pass and preserves the same pointer-keyed lifetime. */
static NativeCoalesceBounds_t **nativeCoalesceBoundsBuckets;
static size_t nativeCoalesceBoundsBucketCount;
static size_t nativeCoalesceBoundsCount;
static unsigned short nativeCoalesceCheckCount;
static int nativeCoalesceTreeBounds[6];

enum NativeCoalesceSide_e {
  NATIVE_COALESCE_FRONT = 0,
  NATIVE_COALESCE_BACK = 1,
  NATIVE_COALESCE_CROSS = 3
};

static int ClassifyCoalesceSplitAxis(int mins, int maxs, int splitCoord);

static size_t NativeCoalesceBounds_BucketIndex(const TriSurf_t *surf)
{
  uintptr_t key = (uintptr_t)surf;

  key ^= key >> 16;
  key *= (uintptr_t)0x45D9F3Bu;
  key ^= key >> 16;
  return (size_t)(key & (nativeCoalesceBoundsBucketCount - 1));
}

static void NativeCoalesceBounds_EnsureCapacity(size_t requiredCount)
{
  NativeCoalesceBounds_t **newBuckets;
  size_t newBucketCount;
  size_t bucketIndex;

  if (nativeCoalesceBoundsBucketCount && requiredCount * 4 < nativeCoalesceBoundsBucketCount * 3)
    return;

  newBucketCount = nativeCoalesceBoundsBucketCount ? nativeCoalesceBoundsBucketCount : 64;
  while (requiredCount * 4 >= newBucketCount * 3)
    newBucketCount <<= 1;

  newBuckets = (NativeCoalesceBounds_t **)calloc(newBucketCount, sizeof(*newBuckets));
  if (!newBuckets)
    Com_Error("NativeCoalesceBounds_EnsureCapacity: out of memory");

  for (bucketIndex = 0; bucketIndex < nativeCoalesceBoundsBucketCount; ++bucketIndex)
  {
    NativeCoalesceBounds_t *entry = nativeCoalesceBoundsBuckets[bucketIndex];

    while (entry)
    {
      NativeCoalesceBounds_t *next = entry->next;
      uintptr_t key = (uintptr_t)entry->surf;

      key ^= key >> 16;
      key *= (uintptr_t)0x45D9F3Bu;
      key ^= key >> 16;
      entry->next = newBuckets[(size_t)(key & (newBucketCount - 1))];
      newBuckets[(size_t)(key & (newBucketCount - 1))] = entry;
      entry = next;
    }
  }
  free(nativeCoalesceBoundsBuckets);
  nativeCoalesceBoundsBuckets = newBuckets;
  nativeCoalesceBoundsBucketCount = newBucketCount;
}

static void NativeCoalesceBounds_Calculate(NativeCoalesceBounds_t *entry, TriSurf_t *surf)
{
  int pointIndex;

  entry->surf = surf;
  entry->mins[0] = entry->mins[1] = entry->mins[2] = INT_MAX;
  entry->maxs[0] = entry->maxs[1] = entry->maxs[2] = INT_MIN;
  entry->checkCount = 0;
  entry->alreadyVisited = 0;
  for (pointIndex = 0; pointIndex < surf->winding->numpoints; ++pointIndex)
  {
    int axis;

    for (axis = 0; axis < 3; ++axis)
    {
      int coordinate = FixedFromFloat(surf->winding->points[pointIndex][axis]);

      if (coordinate < entry->mins[axis])
        entry->mins[axis] = coordinate;
      if (coordinate > entry->maxs[axis])
        entry->maxs[axis] = coordinate;
    }
  }
}

static void NativeCoalesceBounds_Clear(void)
{
  size_t bucketIndex;

  for (bucketIndex = 0; bucketIndex < nativeCoalesceBoundsBucketCount; ++bucketIndex)
  {
    NativeCoalesceBounds_t *entry = nativeCoalesceBoundsBuckets[bucketIndex];

    while (entry)
    {
      NativeCoalesceBounds_t *next = entry->next;
      free(entry);
      entry = next;
    }
  }
  free(nativeCoalesceBoundsBuckets);
  nativeCoalesceBoundsBuckets = NULL;
  nativeCoalesceBoundsBucketCount = 0;
  nativeCoalesceBoundsCount = 0;
  nativeCoalesceCheckCount = 0;
}

static NativeCoalesceBounds_t *NativeCoalesceBounds_Add(TriSurf_t *surf)
{
  NativeCoalesceBounds_t *entry;
  size_t bucketIndex;

  NativeCoalesceBounds_EnsureCapacity(nativeCoalesceBoundsCount + 1);
  entry = (NativeCoalesceBounds_t *)malloc(sizeof(*entry));
  if (!entry)
    Com_Error("NativeCoalesceBounds_Add: out of memory");
  NativeCoalesceBounds_Calculate(entry, surf);
  bucketIndex = NativeCoalesceBounds_BucketIndex(surf);
  entry->next = nativeCoalesceBoundsBuckets[bucketIndex];
  nativeCoalesceBoundsBuckets[bucketIndex] = entry;
  ++nativeCoalesceBoundsCount;
  return entry;
}

static NativeCoalesceBounds_t *NativeCoalesceBounds_Find(TriSurf_t *surf)
{
  NativeCoalesceBounds_t *entry;
  size_t bucketIndex;

  if (!nativeCoalesceBoundsBucketCount)
    return NULL;
  bucketIndex = NativeCoalesceBounds_BucketIndex(surf);
  for (entry = nativeCoalesceBoundsBuckets[bucketIndex]; entry; entry = entry->next)
  {
    if ( entry->surf == surf )
      return entry;
  }
  return NULL;
}

static NativeCoalesceBounds_t *NativeCoalesceBounds_Ensure(TriSurf_t *surf)
{
  NativeCoalesceBounds_t *entry = NativeCoalesceBounds_Find(surf);

  return entry ? entry : NativeCoalesceBounds_Add(surf);
}

/* InsertTriSurfAfter may reuse an address released by an earlier merge.
   Recompute at this producer boundary so stale pointer-keyed state never
   becomes a bound for a newly allocated surface. */
static NativeCoalesceBounds_t *NativeCoalesceBounds_Rebuild(TriSurf_t *surf)
{
  NativeCoalesceBounds_t *entry = NativeCoalesceBounds_Find(surf);

  if (entry)
  {
    NativeCoalesceBounds_Calculate(entry, surf);
    return entry;
  }
  return NativeCoalesceBounds_Add(surf);
}

static bool NativeCoalesceBounds_Intersect(const int *bounds0, const int *bounds1, int tolerance)
{
  int axis;

  for ( axis = 0; axis < 3; ++axis )
  {
    int max1WithTolerance = bounds1[axis + 3] + tolerance;
    int max0WithTolerance = bounds0[axis + 3] + tolerance;

    if ( bounds0[axis] > max1WithTolerance && max1WithTolerance > bounds1[axis + 3] )
      return false;
    if ( bounds1[axis] > max0WithTolerance && max0WithTolerance > bounds0[axis + 3] )
      return false;
  }
  return true;
}

char s_assertDisable_ClipSurfToWinding;
char s_assertDisable_ClipSurfToWinding;
char s_assertDisable_ClipSurfToWinding;
char s_assertDisable_CoalesceChainMatchesArray;
char s_assertDisable_CoalesceChainMatchesArray;
char s_assertDisable_CoalesceChainMatchesArray;
char s_assertDisable_CollectCoalesceLeaves;
char s_assertDisable_FindOrCreateCoalescedSurfProps;
char s_assertDisable_FindOrCreateCoalescedSurfProps;
char s_assertDisable_FreeCoalesceTree;
char s_assertDisable_InitBspLeafNode;
char s_assertDisable_MergeCoplanarSurfs;
char s_assertDisable_MergeCoplanarSurfs;
char s_assertDisable_MergeCoplanarSurfs;
char s_assertDisable_MergeCoplanarSurfs;
char s_assertDisable_MergeCoplanarSurfs;
char s_assertDisable_MergeCoplanarSurfs;
char s_assertDisable_MergeCoplanarSurfs;
char s_assertDisable_MergeCoplanarSurfs;
char s_assertDisable_MergeCoplanarSurfs;
char s_assertDisable_ValidateVisGroupList;
char s_assertDisable_ValidateVisGroupList;
char s_assertDisable_ValidateVisGroupList;


/*
================
CoalesceTreeSearch

Recursively searches coalesce BSP tree, invokes callback on overlapping leaves.
================
*/
int CoalesceTreeSearch(CoalesceBspNode_t *node, float *mins, float *maxs, int (*callback)(CoalesceBspNode_t *, TriSurf_t *), TriSurf_t *userData)
{
  CoalesceBspNode_t *curNode;
  int queryMins[3];
  int queryMaxs[3];
  int splitAxis;
  int axis;

  for ( axis = 0; axis < 3; ++axis )
  {
    queryMins[axis] = FixedFromFloat(mins[axis]);
    queryMaxs[axis] = FixedFromFloat(maxs[axis]);
  }

  curNode = node;
  for ( ;; )
  {
    splitAxis = curNode->splitAxis;
    if ( splitAxis == -1 )
      return callback(curNode, userData);

    if ( queryMins[splitAxis] > curNode->splitCoord + 819 )
    {
      curNode = curNode->front;
    }
    else if ( queryMaxs[splitAxis] < curNode->splitCoord - 819 )
    {
      curNode = curNode->back;
    }
    else
    {
      if ( CoalesceTreeSearch(curNode->back, mins, maxs, callback, userData) == 1 )
        return 1;
      curNode = curNode->front;
    }
  }
}

/* CoD4 0x44E030: fixed-coordinate tree search used while walking the native
   coalescing traversal. */
static int CoalesceTreeSearchNative(CoalesceBspNode_t *node, const int *bounds,
                                    int (*callback)(CoalesceBspNode_t *, TriSurf_t *), TriSurf_t *userData)
{
  while ( node->splitAxis != -1 )
  {
    int side = ClassifyCoalesceSplitAxis(bounds[node->splitAxis], bounds[node->splitAxis + 3], node->splitCoord);

    if ( side == NATIVE_COALESCE_CROSS )
    {
      if ( CoalesceTreeSearchNative(node->back, bounds, callback, userData) == 1 )
        return 1;
      node = node->front;
    }
    else
    {
      node = side == NATIVE_COALESCE_FRONT ? node->front : node->back;
    }
  }
  return callback(node, userData);
}

/*
================
CoalesceLeafRemove

Removes surface from coalesce leaf linked list.
================
*/
int CoalesceLeafRemove(CoalesceBspNode_t *leafNode, TriSurf_t *surfPtr)
{
  CoalesceSurfListNode_t **listPtr;
  CoalesceSurfListNode_t *freeNode;

  listPtr = (CoalesceSurfListNode_t **)&leafNode->surfList;
  while ( *listPtr )
  {
    if ( (*listPtr)->surf == surfPtr )
    {
      freeNode = *listPtr;
      *listPtr = freeNode->next;
      free(freeNode);
    }
    else
    {
      listPtr = &(*listPtr)->next;
    }
  }
  return 0;
}

/*
================
CoalesceLeafInsert

Inserts surface into coalesce leaf linked list.
================
*/
int CoalesceLeafInsert(CoalesceBspNode_t *leafNode, TriSurf_t *surfPtr)
{
  CoalesceSurfListNode_t *newNode;
  CoalesceSurfListNode_t **tail;

  newNode = malloc(sizeof(CoalesceSurfListNode_t));
  newNode->surf = surfPtr;
  newNode->next = NULL;
  /* CoD4 0x44EF80 preserves the candidate order by appending here.  New
     fragments are subsequently scanned in this order by 0x44E0D0. */
  tail = (CoalesceSurfListNode_t **)&leafNode->surfList;
  while ( *tail )
    tail = &(*tail)->next;
  *tail = newNode;
  return 0;
}

/*
================
CollectCoalesceLeaves

Collects groupable surface props into sorted array.
================
*/
int CollectCoalesceLeaves(uintptr_t surfProps, intptr_t *outArray, int propsCount, int *groupCallback)
{
  CoalesceNode_t *chainPtr;
  int result;
  int insertIdx;
  uintptr_t *sortPtr;
  float errorPos[3];

  chainPtr = ((TriSurfProps_t *)surfProps)->coalesceChain;
  if ( chainPtr )
  {
    result = propsCount;
    do
    {
      result = CollectCoalesceLeaves(chainPtr->value, outArray, result, groupCallback);
      chainPtr = chainPtr->next;
    }
    while ( chainPtr );
    return result;
  }

  insertIdx = 0;
  while ( insertIdx < propsCount )
  {
    if ( TriSurfPropsGroupable((TriSurfProps_t *)outArray[insertIdx], (TriSurfProps_t *)surfProps, groupCallback) )
      return propsCount;
    ++insertIdx;
  }

  #define MAX_COINCIDENT_WINDINGS 8
  if ( propsCount == MAX_COINCIDENT_WINDINGS )
  {
    WindingCenter((Winding_t *)groupCallback, errorPos);
    Com_Error(
      "MAX_COINCIDENT_WINDINGS (%i) exceeded.  Too many polys overlap at %g %g %g.\n",
      MAX_COINCIDENT_WINDINGS,
      errorPos[0],
      errorPos[1],
      errorPos[2]);
  }
  AssertFatal(insertIdx == propsCount, s_assertDisable_CollectCoalesceLeaves);
  if ( insertIdx > 0 )
  {
    sortPtr = (uintptr_t *)&outArray[insertIdx - 1];
	
    /* Native 0x44EAC0 orders leaf properties by loaded-material order before
       chain hashing; this must not depend on allocator addresses. */
    do
    {
      TriSurfProps_t *previous = (TriSurfProps_t *)*sortPtr;
      TriSurfProps_t *incoming = (TriSurfProps_t *)surfProps;

      Assert(previous->si && incoming->si, s_assertDisable_CollectCoalesceLeaves);
      if ( !IsLoadedMaterialBefore(incoming->si, previous->si) )
        break;
      sortPtr[1] = *sortPtr;
      --insertIdx;
      --sortPtr;
    }
    while ( insertIdx > 0 );
  }
  outArray[insertIdx] = surfProps;
  return propsCount + 1;
}

/*
================
CoalesceChainMatchesArray

Checks if coalesce chain matches sorted array.
================
*/
int CoalesceChainMatchesArray(CoalesceNode_t *chain, int count, intptr_t *array)
{
  CoalesceNode_t *chainPtr;
  int matchIdx;

  chainPtr = chain;
  Assert(chain, s_assertDisable_CoalesceChainMatchesArray);
  Assert(array, s_assertDisable_CoalesceChainMatchesArray);
  Assert(count >= 2, s_assertDisable_CoalesceChainMatchesArray);
  matchIdx = 0;
  if ( count <= 0 )
    return chainPtr == 0;
  while ( chainPtr && array[matchIdx] == chainPtr->value )
  {
    chainPtr = chainPtr->next;
    if ( ++matchIdx >= count )
      return chainPtr == 0;
  }
  return 0;
}

/*
================
BuildCoalesceChain

Builds coalesce chain linked list from sorted array.
================
*/
CoalesceNode_t *BuildCoalesceChain(intptr_t *array, int count)
{
  CoalesceNode_t *newNode;

  if ( !count )
    return 0;
  newNode = malloc(sizeof(*newNode));
  newNode->value = array[0];
  newNode->next = BuildCoalesceChain(array + 1, count - 1);
  return newNode;
}

/* CoD4 0x44ED80 hashes the native 32-bit property pointers, byte for byte.
   Do not use host pointer width: it changes the bucket selection when KIWI is
   inspected or hosted as a 64-bit process. */
static unsigned char NativeCoalesceChainHash(const intptr_t *array, int count)
{
  unsigned char hash = 0;
  int i;

  for ( i = 0; i < count; ++i )
  {
    uint32_t value = (uint32_t)array[i];
    const unsigned char *bytes = (const unsigned char *)&value;
    int byteIndex;

    for ( byteIndex = 0; byteIndex < 4; ++byteIndex )
    {
      int nativeIndex = i * 4 + byteIndex;
      hash = (unsigned char)(hash + hash * (nativeIndex + 117) + bytes[byteIndex]);
    }
  }
  return hash;
}

/* CoD4 0x44F1A0: this only clears bucket heads.  Ownership is released by
   FreeTriSurfProps at the matching end-of-emission boundary. */
void ClearCoalescePropsHash(void)
{
  memset(g_windingHashBuckets, 0, sizeof(g_windingHashBuckets));
}

/*
================
InitBspLeafNode

Initializes BSP node as leaf with surface list.
================
*/
CoalesceBspNode_t *InitBspLeafNode(CoalesceBspNode_t *bspNode, CoalesceSurfListNode_t *surfList)
{
  Assert(bspNode, s_assertDisable_InitBspLeafNode);
  bspNode->splitAxis = -1;
  bspNode->surfList = surfList;
  return bspNode;
}

/*
================
FreeCoalesceTree

Recursively frees coalesce BSP tree.
================
*/
void FreeCoalesceTree(CoalesceBspNode_t *node)
{
  DrawSurfRef_t *freeNode;

  Assert(node, s_assertDisable_FreeCoalesceTree);
  if ( node->splitAxis == -1 )
  {
    while ( node->surfList )
    {
      freeNode = (DrawSurfRef_t *)node->surfList;
      node->surfList = freeNode->next;
      free(freeNode);
    }
  }
  else
  {
    FreeCoalesceTree(node->front);
    FreeCoalesceTree(node->back);
  }
  free(node);
}

/*
================
ValidateVisGroupList

Validates vis group doubly-linked list integrity.
================
*/
void ValidateVisGroupList(TriSurf_t **listHead)
{
  TriSurf_t *curSurf;
  TriSurf_t *nextSurf;

  Assert(listHead, s_assertDisable_ValidateVisGroupList);
  curSurf = *listHead;
  if ( curSurf )
  {
    Assert(!curSurf->prev, s_assertDisable_ValidateVisGroupList);
    for ( nextSurf = curSurf->next; nextSurf; nextSurf = curSurf->next )
    {
      Assert(nextSurf->prev == curSurf, s_assertDisable_ValidateVisGroupList);
      curSurf = curSurf->next;
    }
  }
}

/*
================
FindOrCreateCoalescedSurfProps

Finds or creates merged surface properties.
================
*/
TriSurfProps_t *FindOrCreateCoalescedSurfProps(int *groupCallback, TriSurfProps_t *props0, TriSurfProps_t *props1)
{
  TriSurfProps_t *found;
  TriSurfPropsSidecar_t *selectedSidecar;
  intptr_t coalesceArray[8];
  int count, hiddenCount, i, hash, bucket;
  CoalesceNode_t *chain;

  if ( TriSurfPropsGroupable(props0, props1, groupCallback) )
    return props0;

  /* collect all leaf props from both sides */
  count = CollectCoalesceLeaves((uintptr_t)props0, coalesceArray, 0, groupCallback);
  count = CollectCoalesceLeaves((uintptr_t)props1, coalesceArray, count, groupCallback);
  if ( count == 1 )
    return (TriSurfProps_t *)coalesceArray[0];
  Assert(count >= 2, s_assertDisable_FindOrCreateCoalescedSurfProps);

  /* Native 0x44EDE0 starts at the last non-leading layer whose material
     flags select the hidden path.  0x44E6F0 then carries only the remaining
     visible chain; when every entry is hidden it returns that leaf directly.
     ShaderInfo_t::surfaceFlags is KIWI's retained form of this material
     flag word. */
  hiddenCount = 0;
  for ( i = 1; i < count; ++i )
  {
    TriSurfProps_t *leaf = (TriSurfProps_t *)coalesceArray[i];

    Assert(leaf && leaf->si, s_assertDisable_FindOrCreateCoalescedSurfProps);
    if ( (leaf->si->surfaceFlags & 0x70) == 0x10 )
      hiddenCount = i;
  }
  if ( hiddenCount == count - 1 )
  {
    TriSurfProps_t *lastLeaf = (TriSurfProps_t *)coalesceArray[count - 1];

    Assert(lastLeaf, s_assertDisable_FindOrCreateCoalescedSurfProps);
    Assert(lastLeaf->coalesceChain == NULL, s_assertDisable_FindOrCreateCoalescedSurfProps);
    Assert(lastLeaf->si, s_assertDisable_FindOrCreateCoalescedSurfProps);
    return (TriSurfProps_t *)coalesceArray[count - 1];
  }

  /* 0x44E6F0 hashes only the surviving chain, not the hidden prefix. */
  hash = NativeCoalesceChainHash(&coalesceArray[hiddenCount], count - hiddenCount);
  bucket = (unsigned char)hash;

  /* search hash bucket for existing match */
  for ( found = g_windingHashBuckets[bucket]; found; found = found->freeListNext )
  {
    Assert(found->coalesceChain, s_assertDisable_FindOrCreateCoalescedSurfProps);
    if ( CoalesceChainMatchesArray(found->coalesceChain, count - hiddenCount,
                                   &coalesceArray[hiddenCount]) )
      return found;
  }

  /* create new merged props */
  found = malloc(sizeof(TriSurfProps_t));
  memset(found, 0, sizeof(TriSurfProps_t));
  Vector4Copy(props0->plane, found->plane);
  /* Native 0x44E979 copies ds->mtlTex from the selected first surviving
     leaf.  In KIWI this is the ShaderInfo_t material identity used by the
     later 0x451600 partition. */
  found->si = ((TriSurfProps_t *)coalesceArray[hiddenCount])->si;
  found->lmapGroupId = -1; /* native 0x44E97E */
  found->lmapIndex = 31;   /* native 0x44E988 / LIGHTMAP_NONE */
  selectedSidecar = TrisPropsSidecar_Get((TriSurfProps_t *)coalesceArray[hiddenCount]);
  Assert(selectedSidecar, s_assertDisable_FindOrCreateCoalescedSurfProps);
  /* The native 0xAC carrier owns only lmap group/index state.  Preserve the
     KIWI-only smoothing, reflection, and legacy light-style state from the
     exact selected leaf in its sidecar. */
  TrisPropsSidecar_Create(found, selectedSidecar->smoothAngle, selectedSidecar->smoothing,
                          selectedSidecar->reflectionProbeIndex, selectedSidecar->reflectionCellMode,
                          selectedSidecar->lightStyle);
  TrisPropsSidecar_SetMapDrawSurf(found, selectedSidecar->mapDrawSurf);

  /* Native 0x44EE30 chooses the first surviving material with gameFlag bit
     2 and copies its lmap vectors; a composite with none keeps the cleared
     zero vectors. */
  for ( i = hiddenCount; i < count; ++i )
  {
    TriSurfProps_t *leaf = (TriSurfProps_t *)coalesceArray[i];

    if ( leaf->si && (leaf->si->gameFlags & 2) != 0 )
    {
      memcpy(found->lmapVecs, leaf->lmapVecs, sizeof(found->lmapVecs));
      break;
    }
  }
  found->contentFlagBit8 = props0->contentFlagBit8 || props1->contentFlagBit8;
  found->surfFlagBit7 = props0->surfFlagBit7 && props1->surfFlagBit7;

  /* subdivisions: if either is 0, sum them; otherwise take the smaller */
  if ( props0->subdivisions == 0.0f || props1->subdivisions == 0.0f )
    found->subdivisions = props0->subdivisions + props1->subdivisions;
  else
    found->subdivisions = props0->subdivisions < props1->subdivisions ? props0->subdivisions : props1->subdivisions;

  /* 0x44ED20 recursively builds the selected suffix and leaves each
     constituent as an emitted layer. */
  chain = BuildCoalesceChain(&coalesceArray[hiddenCount], count - hiddenCount);
  found->coalesceChain = chain;
  found->freeListNext = g_windingHashBuckets[bucket];
  g_windingHashBuckets[bucket] = found;
  return found;
}

/*
================
BuildBspSplitNode

Attempts BSP split of surface list along chosen axis.
================
*/
char BuildBspSplitNode(CoalesceSurfListNode_t *surfList, int splitAxis, CoalesceBspNode_t *outNode, float *parentMins, float *parentMaxs, int surfCount)
{
  CoalesceSurfListNode_t *node, *next, *leftList, *rightList, *dup;
  volatile float mid = MIDF(parentMins[splitAxis], parentMaxs[splitAxis]);
  vec3_t leftMins, rightMaxs;
  int frontCount = 0, backCount = 0;

  /* count how many surfaces go to each side */
  for ( node = surfList; node; node = node->next )
  {
    TriSurf_t *s = node->surf;
    if ( s->mins[splitAxis] > mid )
      frontCount++;
    else
    {
      backCount++;
      if ( s->maxs[splitAxis] >= mid )
        frontCount++;
    }
  }
  if ( frontCount == surfCount || backCount == surfCount )
    return 0;

  /* partition surfaces into left (back) and right (front) lists */
  leftList = rightList = NULL;
  for ( node = surfList; node; node = next )
  {
    TriSurf_t *s = node->surf;
    next = node->next;

    if ( s->mins[splitAxis] <= mid )
    {
      if ( s->maxs[splitAxis] < mid )
      {
        /* entirely on back side */
        node->next = leftList;
        leftList = node;
        continue;
      }
	  
      /* straddles — duplicate node for back side */
      dup = malloc(sizeof(CoalesceSurfListNode_t));
      dup->surf = s;
      dup->next = leftList;
      leftList = dup;
    }
	
    /* front side (or straddling original) */
    node->next = rightList;
    rightList = node;
  }

  /* split parent bounds at midpoint and recurse */
  VectorCopy(parentMins, leftMins);
  leftMins[splitAxis] = mid;
  VectorCopy(parentMaxs, rightMaxs);
  rightMaxs[splitAxis] = mid;

  outNode->splitDist = mid;
  outNode->splitAxis = splitAxis;
  outNode->back = BuildCoalesceBspNode(parentMins, rightMaxs, leftList, backCount);
  outNode->front = BuildCoalesceBspNode(leftMins, parentMaxs, rightList, frontCount);
  return 1;
}

CoalesceBspNode_t *BuildCoalesceBspNode(float *mins, float *maxs, CoalesceSurfListNode_t *surfList, int surfCount);
int g_buildBspNodeCount = 0;

/* CoD4 0x44D5F0: clear an integer AABB then accumulate every native
   fixed-winding bound carried by the candidate list. */
static void AccumulateCoalesceCandidateBounds(CoalesceSurfListNode_t *surfList, int unused, int *bounds)
{
  CoalesceSurfListNode_t *node;

  (void)unused;
  bounds[0] = bounds[1] = bounds[2] = INT_MAX;
  bounds[3] = bounds[4] = bounds[5] = INT_MIN;
  for ( node = surfList; node; node = node->next )
  {
    NativeCoalesceBounds_t *surfaceBounds = NativeCoalesceBounds_Find(node->surf);
    int axis;

    Assert(surfaceBounds, s_assertDisable_InitBspLeafNode);
    for ( axis = 0; axis < 3; ++axis )
    {
      if ( surfaceBounds->mins[axis] < bounds[axis] )
        bounds[axis] = surfaceBounds->mins[axis];
      if ( surfaceBounds->maxs[axis] > bounds[axis + 3] )
        bounds[axis + 3] = surfaceBounds->maxs[axis];
    }
  }
}

/* CoD4 0x44D930: classify an integer range against a split plane with the
   native +/- 0x333 fixed-coordinate tolerance. */
static int ClassifyCoalesceSplitAxis(int mins, int maxs, int splitCoord)
{
  if ( mins > splitCoord + 819 )
    return NATIVE_COALESCE_FRONT;
  if ( maxs >= splitCoord - 819 )
    return NATIVE_COALESCE_CROSS;
  return NATIVE_COALESCE_BACK;
}

/* CoD4 0x44D820: for each non-crossing candidate axis, accumulate the
   eligible integer bounds and count its eligible surfaces. */
static void AccumulateCoalesceSplitBounds(CoalesceSurfListNode_t *surfList, int unused,
                                          int *bounds, const int *splitCoords, int *axisCounts)
{
  int axis;

  (void)unused;
  bounds[0] = bounds[1] = bounds[2] = INT_MAX;
  bounds[3] = bounds[4] = bounds[5] = INT_MIN;
  for ( axis = 0; axis < 3; ++axis )
  {
    CoalesceSurfListNode_t *node;

    axisCounts[axis] = 0;
    for ( node = surfList; node; node = node->next )
    {
      NativeCoalesceBounds_t *surfaceBounds = NativeCoalesceBounds_Find(node->surf);

      Assert(surfaceBounds, s_assertDisable_InitBspLeafNode);
      if ( ClassifyCoalesceSplitAxis(surfaceBounds->mins[axis], surfaceBounds->maxs[axis], splitCoords[axis]) != NATIVE_COALESCE_CROSS )
      {
        ++axisCounts[axis];
        if ( bounds[axis] > surfaceBounds->mins[axis] )
          bounds[axis] = surfaceBounds->mins[axis];
        if ( bounds[axis + 3] < surfaceBounds->maxs[axis] )
          bounds[axis + 3] = surfaceBounds->maxs[axis];
      }
    }
  }
}

/* CoD4 0x44DB70: count front/back/cross classifications and reject planes
   with no exclusive surface on either side. */
static bool ScoreNativeCoalesceSplitPlane(int splitCoord, CoalesceSurfListNode_t *surfList,
                                          int surfCount, int *score)
{
  CoalesceSurfListNode_t *node;
  int axis = score[0];

  score[1] = 0;
  score[2] = 0;
  score[3] = 0;
  for ( node = surfList; node; node = node->next )
  {
    NativeCoalesceBounds_t *surfaceBounds = NativeCoalesceBounds_Find(node->surf);
    int side;

    Assert(surfaceBounds, s_assertDisable_InitBspLeafNode);
    side = ClassifyCoalesceSplitAxis(surfaceBounds->mins[axis], surfaceBounds->maxs[axis], splitCoord);
    Assert(side == NATIVE_COALESCE_FRONT || side == NATIVE_COALESCE_BACK || side == NATIVE_COALESCE_CROSS,
           s_assertDisable_InitBspLeafNode);
    if ( side == NATIVE_COALESCE_FRONT )
      ++score[1];
    if ( side == NATIVE_COALESCE_BACK )
      ++score[2];
    if ( side == NATIVE_COALESCE_CROSS )
      ++score[3];
  }
  return score[3] + score[1] != surfCount && score[3] + score[2] != surfCount;
}

static int NativeCoalesceSplitScore(const int *score)
{
  int imbalance = score[1] - score[2];

  return score[3] + (imbalance < 0 ? -imbalance : imbalance);
}

/* CoD4 0x44DAD0: select the lowest crossing-plus-imbalance score from the
   three candidate axes. */
static bool SelectNativeCoalesceSplitPlane(int *outScore, CoalesceSurfListNode_t *surfList,
                                           int surfCount, const int *splitCoords)
{
  int candidate[4];
  int axis;

  outScore[0] = -1;
  for ( axis = 0; axis < 3; ++axis )
  {
    candidate[0] = axis;
    if ( ScoreNativeCoalesceSplitPlane(splitCoords[axis], surfList, surfCount, candidate)
      && (outScore[0] == -1 || NativeCoalesceSplitScore(candidate) < NativeCoalesceSplitScore(outScore)) )
    {
      memcpy(outScore, candidate, sizeof(candidate));
    }
  }
  return outScore[0] != -1;
}

/* CoD4 0x44D960: partition in place, duplicate crossing list links for the
   back child, and recursively construct native fixed-coordinate children. */
static void BuildNativeCoalesceBspSplit(CoalesceBspNode_t *node, int splitCoord,
                                        int splitAxis, CoalesceSurfListNode_t *surfList)
{
  CoalesceSurfListNode_t *backList = NULL;
  CoalesceSurfListNode_t *frontList = NULL;
  int backCount = 0;
  int frontCount = 0;

  while ( surfList )
  {
    CoalesceSurfListNode_t *next = surfList->next;
    NativeCoalesceBounds_t *surfaceBounds = NativeCoalesceBounds_Find(surfList->surf);
    int side;

    Assert(surfaceBounds, s_assertDisable_InitBspLeafNode);
    side = ClassifyCoalesceSplitAxis(surfaceBounds->mins[splitAxis], surfaceBounds->maxs[splitAxis], splitCoord);
    if ( side == NATIVE_COALESCE_BACK )
    {
      surfList->next = backList;
      backList = surfList;
      ++backCount;
    }
    else
    {
      Assert(side == NATIVE_COALESCE_FRONT || side == NATIVE_COALESCE_CROSS, s_assertDisable_InitBspLeafNode);
      if ( side == NATIVE_COALESCE_CROSS )
      {
        CoalesceSurfListNode_t *duplicate = (CoalesceSurfListNode_t *)malloc(sizeof(*duplicate));

        duplicate->surf = surfList->surf;
        duplicate->next = backList;
        backList = duplicate;
        ++backCount;
      }
      surfList->next = frontList;
      frontList = surfList;
      ++frontCount;
    }
    surfList = next;
  }

  node->splitAxis = splitAxis;
  node->splitCoord = splitCoord;
  node->back = BuildCoalesceBspNode(NULL, NULL, backList, backCount);
  node->front = BuildCoalesceBspNode(NULL, NULL, frontList, frontCount);
}

/*
================
BuildCoalesceBspNode

Recursively builds coalesce BSP node.
================
*/
CoalesceBspNode_t *BuildCoalesceBspNode(float *mins, float *maxs, CoalesceSurfListNode_t *surfList, int surfCount)
{
  CoalesceBspNode_t *newNode;
  int candidateBounds[6];
  int splitCoords[3];
  int splitScore[4];
  int axisCounts[3];
  int attempt;
  int axis;

  (void)mins;
  (void)maxs;

  g_buildBspNodeCount++;
  newNode = malloc(sizeof(*newNode));
  if ( surfCount <= 16 )
  {
    InitBspLeafNode(newNode, surfList);
    return newNode;
  }

  AccumulateCoalesceCandidateBounds(surfList, surfCount, candidateBounds);
  for ( axis = 0; axis < 3; ++axis )
    splitCoords[axis] = (int)(((int64_t)candidateBounds[axis] + candidateBounds[axis + 3]) >> 1);
  if ( SelectNativeCoalesceSplitPlane(splitScore, surfList, surfCount, splitCoords) )
  {
    BuildNativeCoalesceBspSplit(newNode, splitCoords[splitScore[0]], splitScore[0], surfList);
    return newNode;
  }

  for ( attempt = 0; attempt < 10; ++attempt )
  {
    AccumulateCoalesceSplitBounds(surfList, surfCount, candidateBounds, splitCoords, axisCounts);
    for ( axis = 0; axis < 3; ++axis )
      splitCoords[axis] = (int)(((int64_t)candidateBounds[axis] + candidateBounds[axis + 3]) >> 1);
    if ( SelectNativeCoalesceSplitPlane(splitScore, surfList, surfCount, splitCoords) )
    {
      BuildNativeCoalesceBspSplit(newNode, splitCoords[splitScore[0]], splitScore[0], surfList);
      return newNode;
    }
    if ( surfCount < 256 )
    {
      InitBspLeafNode(newNode, surfList);
      return newNode;
    }
  }

  printf("Failed to find a valid split plane for leaf with %d triangles.\n", surfCount);
  InitBspLeafNode(newNode, surfList);
  return newNode;
}

/*
================
BuildCoalesceBspTree

Builds coalesce BSP tree from vis group list.
================
*/
CoalesceBspNode_t *BuildCoalesceBspTree(TriSurf_t **listHead)
{
  TriSurf_t *curSurf;
  CoalesceSurfListNode_t *listNode;
  CoalesceBspNode_t *result;
  float treeMins[3];
  float treeMaxs[3];
  CoalesceSurfListNode_t *surfList;
  int surfCount;

  NativeCoalesceBounds_Clear();
  ClearBounds(treeMins, treeMaxs);
  surfCount = 0;
  surfList = NULL;
  for ( curSurf = *listHead; curSurf; curSurf = curSurf->next )
  {
    if ( curSurf->props->si->surfaceFlags & SURF_NODRAW )
      continue;
    WindingBounds(curSurf->winding, curSurf->mins, curSurf->maxs);
    NativeCoalesceBounds_Add(curSurf);
    AddBoundsToBounds(curSurf->mins, curSurf->maxs, treeMins, treeMaxs);
    surfCount++;
    listNode = malloc(sizeof(CoalesceSurfListNode_t));
    listNode->surf = curSurf;
    listNode->next = surfList;
    surfList = listNode;
  }
  AccumulateCoalesceCandidateBounds(surfList, surfCount, nativeCoalesceTreeBounds);
  result = BuildCoalesceBspNode(treeMins, treeMaxs, surfList, surfCount);
  coalesceTreeRoot = result;
  coalesceSurfListHead = listHead;
  return result;
}

/*
================
SurfsAreMergeable

Checks if two surfaces are coplanar and geometrically mergeable.
================
*/
static double NativeWindingMaxPlaneDistance(const Winding_t *winding, const float *plane)
{
  double maxDistance = 0.0;
  int pointIndex;

  for ( pointIndex = 0; pointIndex < winding->numpoints; ++pointIndex )
  {
    FixedVec3_t point;
    double distance;

    FixedVec3FromFloat(winding->points[pointIndex], &point);
    distance = FixedPointPlaneDistance(&point, plane, plane[3]);
    if ( fabs(distance) > maxDistance )
      maxDistance = fabs(distance);
  }
  return maxDistance;
}

char SurfsAreMergeable(TriSurf_t *ts0, TriSurf_t *ts1)
{
  TriSurfProps_t *p0 = ts0->props;
  TriSurfProps_t *p1 = ts1->props;
  NativeCoalesceBounds_t *bounds0 = NativeCoalesceBounds_Ensure(ts0);
  NativeCoalesceBounds_t *bounds1 = NativeCoalesceBounds_Ensure(ts1);
  double distance0;
  double distance1;
  int mergeOrder;

  if ( DotProduct210(p0->plane, p1->plane) < 0.9900000095367432f
    || !NativeCoalesceBounds_Intersect(bounds0->mins, bounds1->mins, 819) )
    return 0;

  distance0 = NativeWindingMaxPlaneDistance(ts1->winding, p0->plane);
  if ( distance0 > 0.05000000074505806 )
    return 0;
  distance1 = NativeWindingMaxPlaneDistance(ts0->winding, p1->plane);
  if ( distance1 > 0.05000000074505806 )
    return 0;
  mergeOrder = distance1 < distance0 ? 2 : 1;

  if ( p0->contentFlagBit8 || p1->contentFlagBit8 )
  {
    if ( CheckWindingSeparation(ts0->winding, ts1->winding, p0->plane, MERGE_EPSILON)
      || CheckWindingSeparation(ts1->winding, ts0->winding, p1->plane, MERGE_EPSILON) )
    {
      return 0;
    }
  }
  else if ( !CheckWindingContainment(ts0->winding, ts1->winding, p0->plane, MERGE_EPSILON)
         || !CheckWindingContainment(ts1->winding, ts0->winding, p1->plane, MERGE_EPSILON) )
  {
    return 0;
  }
  return mergeOrder;
}

/* CoD4 0x44EF00.  The native routine consumes a FixedWinding_t and rejects
   clockwise output before inserting it.  KIWI's clipping path additionally
   owns per-vertex auxiliary data, so retain that payload while applying the
   same insert/bounds/tree-link sequence. */
static void InsertCoalescedTriSurf(Winding_t *winding, void *auxData, int auxElemSize,
                                   TriSurfProps_t *props, TriSurf_t *prev)
{
  TriSurf_t *surf;

  if ( !winding )
    return;
  surf = InsertTriSurfAfter(winding, auxData, auxElemSize, props, prev);
  WindingBounds(surf->winding, surf->mins, surf->maxs);
  NativeCoalesceBounds_Rebuild(surf);
  CoalesceTreeSearch(coalesceTreeRoot, surf->mins, surf->maxs, CoalesceLeafInsert, surf);
}

/*
================
ClipSurfToWinding

Clips surface by winding edges, splits into fragments.
Returns clipped winding in EAX and auxiliary vert data via outAuxVerts (EDX in original).
================
*/
Winding_t *ClipSurfToWinding(TriSurf_t *ts, Winding_t *chopWinding, float *planeNormal, void **outAuxVerts)
{
  Winding_t *result;
  int numEdges, edgeIdx;
  vec3_t edgePlane, edgeDir;
  float edgePlaneDist;
  WindingAuxPair_t remainPair, frontWinding, backWinding;

  Assert(ts, s_assertDisable_ClipSurfToWinding);
  Assert(chopWinding, s_assertDisable_ClipSurfToWinding);
  Assert(planeNormal, s_assertDisable_ClipSurfToWinding);
  remainPair.winding = ts->winding;
  remainPair.auxData = ts->auxData;
  ts->winding = NULL;
  ts->auxData = NULL;
  numEdges = chopWinding->numpoints;

  for ( edgeIdx = 0; edgeIdx < numEdges && remainPair.winding; edgeIdx++ )
  {
    float *curVert = chopWinding->points[edgeIdx];
    float *prevVert = chopWinding->points[(edgeIdx + numEdges - 1) % numEdges];

    VectorSubtract(curVert, prevVert, edgeDir);
    CrossProduct(planeNormal, edgeDir, edgePlane);
    if ( VecNormalize(edgePlane) == 0.0 )
      break;
    edgePlaneDist = (float)DotProduct120(edgePlane, prevVert);

    ClipTriWindingWithAux(&remainPair, chopWinding, ts->auxElemSize, edgePlane, edgePlaneDist, MERGE_EPSILON, &frontWinding, &backWinding);
    if ( frontWinding.winding )
    {
      InsertCoalescedTriSurf(frontWinding.winding, frontWinding.auxData, ts->auxElemSize, ts->props, ts);
    }
    FreeVerts(&remainPair);
    remainPair.winding = backWinding.winding;
    remainPair.auxData = backWinding.auxData;
  }

  result = remainPair.winding;
  if ( outAuxVerts )
    *outAuxVerts = remainPair.auxData;
  return result;
}

/*
================
MergeCoplanarSurfs

Merges two coplanar surfaces into one.
================
*/
int MergeCoplanarSurfs(TriSurf_t *ts0, TriSurf_t *ts1)
{
  TriSurfProps_t *mergedProps;
  Winding_t *windingCopy;
  
  /* clipResult and clipAux must be contiguous — FreeVerts writes to both fields */
  WindingAuxPair_t clipPair;
  #define clipResult clipPair.winding
  #define clipAux clipPair.auxData

  Assert(ts0, s_assertDisable_MergeCoplanarSurfs);
  Assert(ts1, s_assertDisable_MergeCoplanarSurfs);
  AssertFatal(ts0->winding, s_assertDisable_MergeCoplanarSurfs);
  AssertFatal(ts1->winding, s_assertDisable_MergeCoplanarSurfs);
  AssertFatal(ts0->winding != ts1->winding, s_assertDisable_MergeCoplanarSurfs);
  AssertFatal(ts0->props, s_assertDisable_MergeCoplanarSurfs);
  AssertFatal(ts1->props, s_assertDisable_MergeCoplanarSurfs);
  { TriSurfProps_t *p0 = ts0->props;
  TriSurfProps_t *p1 = ts1->props;
  Assert(!CheckWindingSeparationBoth(ts0->winding, ts1->winding, p0->plane, -MERGE_EPSILON), s_assertDisable_MergeCoplanarSurfs);
  Assert(!CheckWindingSeparationBoth(ts0->winding, ts1->winding, p1->plane, -MERGE_EPSILON), s_assertDisable_MergeCoplanarSurfs);
  CoalesceTreeSearch(coalesceTreeRoot, ts0->mins, ts0->maxs, CoalesceLeafRemove, ts0);
  CoalesceTreeSearch(coalesceTreeRoot, ts1->mins, ts1->maxs, CoalesceLeafRemove, ts1);
  windingCopy = CopyWinding(ts0->winding);
  clipResult = ClipSurfToWinding(ts0, ts1->winding, p0->plane, &clipAux);
  if ( clipResult )
    FreeVerts(&clipPair);
  clipResult = ClipSurfToWinding(ts1, windingCopy, p0->plane, &clipAux);
  if ( clipResult )
  {
    mergedProps = FindOrCreateCoalescedSurfProps((int *)clipResult, ts0->props, ts1->props);
    InsertCoalescedTriSurf(clipResult, clipAux, ts0->auxElemSize, mergedProps, ts0);
  }
  FreeWinding(windingCopy);
  ts0->winding = 0;
  ts1->winding = 0;
  UnlinkAndFreeSurf(ts0, coalesceSurfListHead);
  UnlinkAndFreeSurf(ts1, coalesceSurfListHead);
  FreeTriSurf(ts0);
  return FreeTriSurf(ts1); }
}
#undef clipResult
#undef clipAux

/*
================
FindAndMergeSurfInLeaf

BSP callback: finds mergeable surface in leaf and merges it.
================
*/
int FindAndMergeSurfInLeaf(CoalesceBspNode_t *leafNode, TriSurf_t *targetSurf)
{
  CoalesceSurfListNode_t *listPtr;
  NativeCoalesceBounds_t *targetState = NativeCoalesceBounds_Ensure(targetSurf);

  Assert(targetState->checkCount == nativeCoalesceCheckCount, s_assertDisable_InitBspLeafNode);
  Assert(targetState->alreadyVisited, s_assertDisable_InitBspLeafNode);
  for ( listPtr = (CoalesceSurfListNode_t *)leafNode->surfList; listPtr; listPtr = listPtr->next )
  {
    TriSurf_t *candidateSurf = listPtr->surf;
    NativeCoalesceBounds_t *candidateState;
    int mergeOrder;

    if ( candidateSurf == targetSurf )
      return 0;
    candidateState = NativeCoalesceBounds_Ensure(candidateSurf);
    if ( candidateState->checkCount == nativeCoalesceCheckCount )
      continue;
    candidateState->checkCount = nativeCoalesceCheckCount;
    mergeOrder = SurfsAreMergeable(targetSurf, candidateSurf);
    if ( mergeOrder )
    {
      if ( mergeOrder == 1 )
        MergeCoplanarSurfs(targetSurf, candidateSurf);
      else
        MergeCoplanarSurfs(candidateSurf, targetSurf);
      return 1;
    }
  }
  return 0;
}

/* CoD4 0x44DEF0: depth-first native coalesce traversal.  The check counter
   and visited bit live in the bounds sidecar because KIWI retains a
   compatibility pointer at the native transient offset. */
static void CoalesceVisGroupNativeTraversal(CoalesceBspNode_t *node, int *bounds)
{
  while ( node->splitAxis != -1 )
  {
    int axis = node->splitAxis;
    int savedMax = bounds[axis + 3];

    bounds[axis + 3] = node->splitCoord;
    CoalesceVisGroupNativeTraversal(node->back, bounds);
    bounds[axis + 3] = savedMax;
    bounds[axis] = node->splitCoord;
    node = node->front;
  }

  {
    CoalesceSurfListNode_t *listNode = (CoalesceSurfListNode_t *)node->surfList;

    while ( listNode )
    {
      TriSurf_t *surf = listNode->surf;
      NativeCoalesceBounds_t *surfaceState = NativeCoalesceBounds_Ensure(surf);

      if ( surfaceState->alreadyVisited )
      {
        listNode = listNode->next;
        continue;
      }

      ++nativeCoalesceCheckCount;
      surfaceState->alreadyVisited = 1;
      surfaceState->checkCount = nativeCoalesceCheckCount;
      if ( NativeCoalesceBounds_Intersect(surfaceState->mins, bounds, 819) )
      {
        if ( FindAndMergeSurfInLeaf(node, surf) == 1 )
          listNode = (CoalesceSurfListNode_t *)node->surfList;
        else
          listNode = listNode->next;
      }
      else if ( CoalesceTreeSearchNative(coalesceTreeRoot, bounds, FindAndMergeSurfInLeaf, surf) == 1 )
      {
        listNode = (CoalesceSurfListNode_t *)node->surfList;
      }
      else
      {
        listNode = listNode->next;
      }
    }
  }
}

/* CoD4 0x44DE90: issue the floating-static-surface diagnostic after the
   native traversal.  KIWI has no MaterialNameInStaticList export, so the
   recovered coalesced material flag is the compatibility predicate. */
static void WarnFloatingStaticSurfacesNative(TriSurf_t *surfList)
{
  TriSurf_t *surf;

  for ( surf = surfList; surf; surf = surf->next )
  {
    if ( surf->props->coalesceChain )
    {
      TriSurfProps_t *leafProps = (TriSurfProps_t *)surf->props->coalesceChain->value;

      if ( leafProps && leafProps->si && (leafProps->si->surfaceFlags & 0x70) == 0x70 )
        printf("surface '%s' is partially floating or needs to be aligned\n", leafProps->si->name);
    }
  }
}

/*
================
CoalesceVisGroup

Merges coplanar surfaces within a vis group.
================
*/
void CoalesceVisGroup( TriSurf_t **listHead )
{
  ValidateVisGroupList(listHead);
  BuildCoalesceBspTree(listHead);

  SetTrisTransientMode(3, 0);
  CoalesceVisGroupNativeTraversal(coalesceTreeRoot, nativeCoalesceTreeBounds);
  SetTrisTransientMode(0, 1);
  WarnFloatingStaticSurfacesNative(*listHead);
  FreeCoalesceTree(coalesceTreeRoot);
  coalesceTreeRoot = 0;
  coalesceSurfListHead = NULL;
  NativeCoalesceBounds_Clear();
}

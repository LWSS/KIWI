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

/* CoD4 0x416DE0: fragments inserted by 0x44EF00 retain their exact source
   winding, so calculate the tree AABB directly from 18.14 coordinates. */
static NativeCoalesceBounds_t *NativeCoalesceBounds_RebuildFixed(TriSurf_t *surf,
                                                                  const FixedWinding_t *winding)
{
  NativeCoalesceBounds_t *entry = NativeCoalesceBounds_Find(surf);
  uint32_t pointIndex;

  if (!entry)
    entry = NativeCoalesceBounds_Add(surf);
  entry->surf = surf;
  entry->mins[0] = entry->mins[1] = entry->mins[2] = INT_MAX;
  entry->maxs[0] = entry->maxs[1] = entry->maxs[2] = INT_MIN;
  entry->checkCount = 0;
  entry->alreadyVisited = 0;
  for (pointIndex = 0; pointIndex < winding->ptCount; ++pointIndex)
  {
    const int *point = (const int *)&winding->points[pointIndex];
    int axis;

    for (axis = 0; axis < 3; ++axis)
    {
      if (point[axis] < entry->mins[axis])
        entry->mins[axis] = point[axis];
      if (point[axis] > entry->maxs[axis])
        entry->maxs[axis] = point[axis];
    }
  }
  return entry;
}

static bool NativeCoalesceBounds_Intersect(const int *bounds0, const int *bounds1, int tolerance)
{
  int axis;

  for ( axis = 0; axis < 3; ++axis )
  {
    /* 0x4170E0 deliberately treats an overflowing ``max + tolerance`` as
       non-separating: its second signed comparison fails after the 32-bit
       wrap.  Spell that out rather than relying on signed-overflow UB. */
    if (bounds1[axis + 3] <= INT_MAX - tolerance
      && bounds0[axis] > bounds1[axis + 3] + tolerance)
      return false;
    if (bounds0[axis + 3] <= INT_MAX - tolerance
      && bounds1[axis] > bounds0[axis + 3] + tolerance)
      return false;
  }
  return true;
}

/* CoD4 0x416FF0.  This is deliberately not an intersection test: while
   walking a leaf, 0x44DEF0 asks whether the current surface AABB is wholly
   inside that leaf's traversal bounds, with the native 819-unit margin.  A
   surface which crosses a leaf boundary must be sent through 0x44E030 so
   every duplicated leaf is considered. */
static bool NativeCoalesceBounds_WithinTraversalBounds(const int *surfaceBounds,
                                                        const int *traversalBounds,
                                                        int tolerance)
{
  int axis;

  for (axis = 0; axis < 3; ++axis)
  {
    /* Match 0x416FF0's overflow guards.  The prior form accidentally let a
       wrapped lower/upper bound through because it used the inverse guard. */
    if (traversalBounds[axis] > INT_MAX - tolerance
      || surfaceBounds[axis] < traversalBounds[axis] + tolerance
      || traversalBounds[axis + 3] < INT_MIN + tolerance
      || surfaceBounds[axis + 3] > traversalBounds[axis + 3] - tolerance)
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
  TriSurfPropsSidecar_t *sidecar;

  if ( !count )
    return 0;
  newNode = malloc(sizeof(*newNode));
  newNode->value = array[0];
  /* Native 0x44ED54 marks every constituent MapDrawSurf at +0x35 as soon
     as it is admitted to a coalesce chain.  TriSurfProps_t retains
     ShaderInfo_t at +0 in KIWI, so the native source carrier lives in its
     sidecar.  The marker is consumed later by the draw-surface emission
     route; it is not padding. */
  sidecar = TrisPropsSidecar_Get((TriSurfProps_t *)newNode->value);
  Assert(sidecar && sidecar->mapDrawSurf, s_assertDisable_FindOrCreateCoalescedSurfProps);
  sidecar->mapDrawSurf->hasCoalesceChain = 1;
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
      CoalesceSurfListNode_t *surfNode =
        (CoalesceSurfListNode_t *)node->surfList;
      TriSurf_t *surf = surfNode->surf;

      /* Native 0x44DD10 releases the temporary fixed winding retained at
         TriSurf +4 and clears the slot during tree teardown.  CROSS
         surfaces can occur in multiple leaves, so the cleared pointer also
         prevents duplicate ownership here.  KIWI uses this slot for other
         auxiliary formats too; only the zero-stride fixed carrier belongs
         to the coalescer. */
      if (surf && !surf->auxElemSize && surf->auxData)
      {
        FixedWinding_t *fixedWinding = (FixedWinding_t *)surf->auxData;
        if (fixedWinding->ptCount >= 3
            && fixedWinding->ptCount <= FIXEDWINDING_MAX_POINTS
            && (!surf->winding
                || fixedWinding->ptCount == (uint32_t)surf->winding->numpoints))
        {
          FreeFixedWinding(fixedWinding);
          surf->auxData = NULL;
        }
      }
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
     tool flags select the hidden path.  0x44E6F0 then carries only the
     remaining visible chain; when every entry is hidden it returns that
     leaf directly. */
  hiddenCount = 0;
  for ( i = 1; i < count; ++i )
  {
    TriSurfProps_t *leaf = (TriSurfProps_t *)coalesceArray[i];

    Assert(leaf && leaf->si, s_assertDisable_FindOrCreateCoalescedSurfProps);
    if ( (leaf->si->toolFlagsWord & 0x70) == 0x10 )
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
static FixedWinding_t *NativeFixedWindingCopyForSurf(const TriSurf_t *surf);

/* CoD4 0x416B00.  The edge planes and point distances are deliberately
   evaluated from the native 18.14 winding, including its adaptive epsilon. */
static bool NativeFixedWindingSeparates(const FixedWinding_t *edgeWinding,
                                        const FixedWinding_t *testWinding,
                                        const float *planeNormal, double epsilon)
{
  double smallEpsilon = epsilon * 0.01;
  uint32_t pointIndex;

  for (pointIndex = 0; pointIndex < edgeWinding->ptCount; ++pointIndex)
  {
    uint32_t nextIndex = (pointIndex + 1) % edgeWinding->ptCount;
    float edgePlane[3];
    double edgePlaneDist;
    double minDistance;
    double maxDistance;

    FixedEdgePlane(&edgeWinding->points[pointIndex],
      &edgeWinding->points[nextIndex], planeNormal, edgePlane, &edgePlaneDist);
    FixedWindingDistanceRange(testWinding, edgePlane, edgePlaneDist,
      &minDistance, &maxDistance);
    if (-smallEpsilon <= minDistance)
      return true;
    if (-epsilon < minDistance && minDistance * -3.0 < maxDistance)
    {
      double referenceDistance = FixedWindingMaxPlaneDistance(edgeWinding,
        edgePlane, edgePlaneDist);
      if (referenceDistance * -0.25 <= minDistance)
        return true;
    }
  }
  return false;
}

/* CoD4 0x416C00. */
static bool NativeFixedWindingsSeparate(const FixedWinding_t *first,
                                         const FixedWinding_t *second,
                                         const float *planeNormal, double epsilon)
{
  return NativeFixedWindingSeparates(first, second, planeNormal, epsilon)
      || NativeFixedWindingSeparates(second, first, planeNormal, epsilon);
}

/* CoD4 0x416C70. */
static bool NativeFixedWindingContains(const FixedWinding_t *edgeWinding,
                                       const FixedWinding_t *testWinding,
                                       const float *planeNormal, double epsilon)
{
  double smallEpsilon = epsilon * 0.01;
  uint32_t pointIndex;

  for (pointIndex = 0; pointIndex < edgeWinding->ptCount; ++pointIndex)
  {
    uint32_t nextIndex = (pointIndex + 1) % edgeWinding->ptCount;
    float edgePlane[3];
    double edgePlaneDist;
    double minDistance;
    double maxDistance;

    FixedEdgePlane(&edgeWinding->points[pointIndex],
      &edgeWinding->points[nextIndex], planeNormal, edgePlane, &edgePlaneDist);
    FixedWindingDistanceRange(testWinding, edgePlane, edgePlaneDist,
      &minDistance, &maxDistance);
    if (smallEpsilon >= maxDistance)
      return false;
    if (epsilon > maxDistance && maxDistance * -3.0 > minDistance)
    {
      double referenceDistance = FixedWindingMaxPlaneDistance(edgeWinding,
        edgePlane, edgePlaneDist);
      if (referenceDistance * 0.25 > maxDistance)
        return false;
    }
  }
  return true;
}

/* CoD4 0x416D70 requires containment in both directions. */
static bool NativeFixedWindingsMutuallyContain(const FixedWinding_t *first,
                                                const FixedWinding_t *second,
                                                const float *planeNormal,
                                                double epsilon)
{
  return NativeFixedWindingContains(first, second, planeNormal, epsilon)
      && NativeFixedWindingContains(second, first, planeNormal, epsilon);
}

char SurfsAreMergeable(TriSurf_t *ts0, TriSurf_t *ts1)
{
  TriSurfProps_t *p0 = ts0->props;
  TriSurfProps_t *p1 = ts1->props;
  NativeCoalesceBounds_t *bounds0 = NativeCoalesceBounds_Ensure(ts0);
  NativeCoalesceBounds_t *bounds1 = NativeCoalesceBounds_Ensure(ts1);
  double distance0;
  double distance1;
  FixedWinding_t *fixed0;
  FixedWinding_t *fixed1;
  float *mergePlane;
  int mergeOrder;

  if ( DotProduct210(p0->plane, p1->plane) < 0.9900000095367432f )
    return 0;
  if ( !NativeCoalesceBounds_Intersect(bounds0->mins, bounds1->mins, 819) )
    return 0;

  fixed0 = NativeFixedWindingCopyForSurf(ts0);
  fixed1 = NativeFixedWindingCopyForSurf(ts1);
  Assert(fixed0 && fixed1, s_assertDisable_InitBspLeafNode);
  distance0 = FixedWindingMaxPlaneDistance(fixed1, p0->plane, p0->plane[3]);
  if ( distance0 > 0.05000000074505806 )
  {
    FreeFixedWinding(fixed1);
    FreeFixedWinding(fixed0);
    return 0;
  }
  distance1 = FixedWindingMaxPlaneDistance(fixed0, p1->plane, p1->plane[3]);
  if ( distance1 > 0.05000000074505806 )
  {
    FreeFixedWinding(fixed1);
    FreeFixedWinding(fixed0);
    return 0;
  }
  /* CoD4 0x44E250 selects exactly one of the two near-coplanar planes:
     the one whose opposing winding had the smaller fixed-point distance.
     CheckWindingSeparation/Containment already test both winding directions
     (0x416C00/0x416D70), so repeating the operation for the other plane
     rejects native-eligible pairs. */
  if ( distance1 < distance0 )
  {
    mergePlane = p1->plane;
    mergeOrder = 2;
  }
  else
  {
    mergePlane = p0->plane;
    mergeOrder = 1;
  }

  if ( p0->contentFlagBit8 || p1->contentFlagBit8 )
  {
    if ( NativeFixedWindingsSeparate(fixed0, fixed1, mergePlane, MERGE_EPSILON) )
    {
      FreeFixedWinding(fixed1);
      FreeFixedWinding(fixed0);
      return 0;
    }
  }
  else if ( !NativeFixedWindingsMutuallyContain(fixed0, fixed1, mergePlane,
                                                MERGE_EPSILON) )
  {
    FreeFixedWinding(fixed1);
    FreeFixedWinding(fixed0);
    return 0;
  }
  FreeFixedWinding(fixed1);
  FreeFixedWinding(fixed0);
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
  /* 0x44EF00 recomputes the FixedWinding bounds at surf + 12, then calls
     0x44E030 directly with those integer coordinates.  Re-entering through
     the legacy float query is observably different at the +/-819 threshold
     and can leave a fresh fragment out of one of its duplicated leaves. */
  {
    const FixedWinding_t *fixedWinding = NULL;
    NativeCoalesceBounds_t *fixedBounds;

    if (!auxElemSize && auxData)
    {
      const FixedWinding_t *candidate = (const FixedWinding_t *)auxData;

      if (candidate->ptCount == (uint32_t)winding->numpoints
          && candidate->ptCount >= 3
          && candidate->ptCount <= FIXEDWINDING_MAX_POINTS)
        fixedWinding = candidate;
    }
    fixedBounds = fixedWinding ? NativeCoalesceBounds_RebuildFixed(surf, fixedWinding)
                               : NativeCoalesceBounds_Rebuild(surf);

    CoalesceTreeSearchNative(coalesceTreeRoot, fixedBounds->mins, CoalesceLeafInsert, surf);
  }
}

/* CoD4 0x4165F0 / 0x43D3B0 use a [count, fixed vec3...] winding as the
   coalescer's source of truth.  TriSurf_t's KIWI compatibility payload can
   occupy auxData in other routes, so create that native carrier locally here
   instead of reinterpreting a non-coalesce auxiliary buffer. */
static FixedWinding_t *NativeFixedWindingFromFloat(const Winding_t *winding)
{
  FixedWinding_t *fixedWinding;
  int pointIndex;

  if (!winding || winding->numpoints < 3 || winding->numpoints > FIXEDWINDING_MAX_POINTS)
    return NULL;

  fixedWinding = AllocFixedWinding(winding->numpoints);
  if (!fixedWinding)
    return NULL;
  fixedWinding->ptCount = winding->numpoints;
  for (pointIndex = 0; pointIndex < winding->numpoints; ++pointIndex)
    FixedVec3FromFloat(winding->points[pointIndex], &fixedWinding->points[pointIndex]);
  return fixedWinding;
}

static FixedWinding_t *NativeFixedWindingCopyForSurf(const TriSurf_t *surf)
{
  const FixedWinding_t *fixedWinding;

  if (!surf || !surf->winding)
    return NULL;

  /* A prior native-style insertion retains its exact 18.14 source in this
     slot.  Do not reconstruct it from the float view: coordinates beyond
     float's unit-resolution range would otherwise lose fixed-point bits. */
  if (!surf->auxElemSize && surf->auxData)
  {
    fixedWinding = (const FixedWinding_t *)surf->auxData;
    if (fixedWinding->ptCount == (uint32_t)surf->winding->numpoints
        && fixedWinding->ptCount >= 3
        && fixedWinding->ptCount <= FIXEDWINDING_MAX_POINTS)
      return CopyFixedWinding(fixedWinding);
  }
  return NativeFixedWindingFromFloat(surf->winding);
}

static Winding_t *NativeWindingFromFixed(const FixedWinding_t *fixedWinding)
{
  Winding_t *winding;
  uint32_t pointIndex;

  if (!fixedWinding || fixedWinding->ptCount < 3 || fixedWinding->ptCount > FIXEDWINDING_MAX_POINTS)
    return NULL;

  winding = AllocWinding(fixedWinding->ptCount);
  winding->numpoints = fixedWinding->ptCount;
  for (pointIndex = 0; pointIndex < fixedWinding->ptCount; ++pointIndex)
    FixedVec3ToFloat((FixedVec3_t *)&fixedWinding->points[pointIndex], winding->points[pointIndex]);
  return winding;
}

/* CoD4 0x44EF00.  0x43D3B0 takes ownership of the fixed winding, while its
   floating representation is only the KIWI-facing view of that carrier. */
static void InsertFixedCoalescedTriSurf(FixedWinding_t *fixedWinding,
                                        TriSurfProps_t *props, TriSurf_t *prev)
{
  Winding_t *winding;

  if (!fixedWinding)
    return;
  if (FixedWindingIsClockwise(fixedWinding, props->plane))
  {
    FreeFixedWinding(fixedWinding);
    return;
  }

  winding = NativeWindingFromFixed(fixedWinding);
  if (!winding)
  {
    FreeFixedWinding(fixedWinding);
    return;
  }
  InsertCoalescedTriSurf(winding, fixedWinding, 0, props, prev);
}

/* CoD4 0x44EFE0 -> 0x4171D0.  The fixed result is intentionally kept until
   the next edge (and ultimately 0x44EF00); converting after every edge would
   change the native distance, extent, copy, and fixed-lerp decisions. */
static FixedWinding_t *ClipSurfToFixedWinding(TriSurf_t *ts, FixedWinding_t *remain,
                                              const FixedWinding_t *chopWinding,
                                              const float *planeNormal)
{
  uint32_t edgeIndex;
  int anyFrontYet;

  Assert(ts, s_assertDisable_ClipSurfToWinding);
  Assert(remain, s_assertDisable_ClipSurfToWinding);
  Assert(chopWinding, s_assertDisable_ClipSurfToWinding);
  Assert(planeNormal, s_assertDisable_ClipSurfToWinding);

  /* The native routine detaches surf->w/fW before it starts clipping. */
  FreeWinding(ts->winding);
  free(ts->auxData);
  ts->winding = NULL;
  ts->auxData = NULL;
  anyFrontYet = 0;

  for (edgeIndex = 0; edgeIndex < chopWinding->ptCount && remain; ++edgeIndex)
  {
    uint32_t previousIndex = edgeIndex ? edgeIndex - 1 : chopWinding->ptCount - 1;
    float edgePlane[3];
    double edgePlaneDist;
    FixedWinding_t *frontWinding;
    FixedWinding_t *backWinding;

    if (!FixedEdgePlane(&chopWinding->points[previousIndex], &chopWinding->points[edgeIndex],
                        planeNormal, edgePlane, &edgePlaneDist))
    {
      FreeFixedWinding(remain);
      return NULL;
    }

    FixedWindingClipEpsilon(remain, chopWinding, edgePlane, edgePlaneDist,
                            MERGE_EPSILON, &frontWinding, &backWinding);
    if (!backWinding && !anyFrontYet)
      Assert(backWinding || anyFrontYet, s_assertDisable_ClipSurfToWinding);
    if (frontWinding)
    {
      InsertFixedCoalescedTriSurf(frontWinding, ts->props, ts);
      anyFrontYet = 1;
    }
    FreeFixedWinding(remain);
    remain = backWinding;
  }
  return remain;
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
  /* Native 0x44E410 passes the fixed AABBs stored at +12 to 0x44E030.
     The sidecar is the KIWI representation of that transient native slot. */
  CoalesceTreeSearchNative(coalesceTreeRoot, NativeCoalesceBounds_Ensure(ts0)->mins,
                           CoalesceLeafRemove, ts0);
  CoalesceTreeSearchNative(coalesceTreeRoot, NativeCoalesceBounds_Ensure(ts1)->mins,
                           CoalesceLeafRemove, ts1);

  /* Native 0x44E410 does not round-trip through Winding_t here: it copies
     surf0->fw, then 0x44EFE0 clips each surface against fixed winding edges.
     Coalesced map surfaces have no KIWI auxiliary vertices; retain the
     auxiliary-aware floating implementation only for the separate shadow
     compatibility route. */
  if (!ts0->auxElemSize && !ts1->auxElemSize)
  {
    FixedWinding_t *fixed0 = NativeFixedWindingCopyForSurf(ts0);
    FixedWinding_t *fixed1 = NativeFixedWindingCopyForSurf(ts1);
    FixedWinding_t *fixedCopy;
    FixedWinding_t *fixedResult;

    Assert(fixed0, s_assertDisable_MergeCoplanarSurfs);
    Assert(fixed1, s_assertDisable_MergeCoplanarSurfs);
    fixedCopy = CopyFixedWinding(fixed0);
    Assert(fixedCopy, s_assertDisable_MergeCoplanarSurfs);

    fixedResult = ClipSurfToFixedWinding(ts0, fixed0, fixed1, p0->plane);
    FreeFixedWinding(fixedResult);
    fixedResult = ClipSurfToFixedWinding(ts1, fixed1, fixedCopy, p0->plane);
    if (fixedResult)
    {
      /* FindOrCreateCoalescedSurfProps uses its winding only for the
         MAX_COINCIDENT_WINDINGS diagnostic.  Give it the native fixed result
         expressed as a temporary KIWI Winding_t, then transfer the original
         fixed carrier to 0x44EF00's insertion analogue. */
      Winding_t *groupWinding = NativeWindingFromFixed(fixedResult);

      Assert(groupWinding, s_assertDisable_MergeCoplanarSurfs);
      mergedProps = FindOrCreateCoalescedSurfProps((int *)groupWinding, ts0->props, ts1->props);
      FreeWinding(groupWinding);
      InsertFixedCoalescedTriSurf(fixedResult, mergedProps, ts0);
    }
    FreeFixedWinding(fixedCopy);
    ts0->winding = 0;
    ts1->winding = 0;
    UnlinkAndFreeSurf(ts0, coalesceSurfListHead);
    UnlinkAndFreeSurf(ts1, coalesceSurfListHead);
    FreeTriSurf(ts0);
    return FreeTriSurf(ts1);
  }

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
      if ( NativeCoalesceBounds_WithinTraversalBounds(surfaceState->mins, bounds, 819) )
      {
        if ( FindAndMergeSurfInLeaf(node, surf) == 1 )
          listNode = (CoalesceSurfListNode_t *)node->surfList;
        else
          listNode = listNode->next;
      }
      /* Native 0x44DEF0 passes surf->fixedBounds (+12), not the traversal
         node bounds, when the surface is duplicated outside this leaf. */
      else if ( CoalesceTreeSearchNative(coalesceTreeRoot, surfaceState->mins,
                                         FindAndMergeSurfInLeaf, surf) == 1 )
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
  FreeCoalesceTree(coalesceTreeRoot);
  coalesceTreeRoot = 0;
  coalesceSurfListHead = NULL;
  NativeCoalesceBounds_Clear();
}

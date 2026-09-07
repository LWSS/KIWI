/*
shadows.c — Shadow map compilation

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

/*
================
TriangleFacesSun

Carrier-independent facing test used by the native sun-shadow preprocessor.

cod4map.exe: 0x45AAC0
================
*/
int TriangleFacesSun(float *vert0, float *vert1, float *vert2, float *sunDirection)
{
  float plane[4];

  if (!PlaneFromPoints(plane, vert0, vert1, vert2))
    return 0;
  return DotProduct(plane, sunDirection) <= 0.009999999776482582f;
}

/*
================
Vector2MA

Native two-dimensional multiply-add.  The native ABI returns the direction
argument (not the output), and callers use only the two output components.

cod4map.exe: 0x45AB20
================
*/
float *Vector2MA(float *start, float scale, float *direction, float *out)
{
  out[0] = start[0] + scale * direction[0];
  out[1] = start[1] + scale * direction[1];
  return direction;
}

/*
================
ProjectedWindingContainsPoint

Native wrapper around the shared projected-winding containment primitive.

cod4map.exe: 0x45AB50
================
*/
int ProjectedWindingContainsPoint(const float *verts, int vertCount, float *plane,
                                  const float *point, float epsilon)
{
  int axisU;
  int axisV;

  GetProjectionAxes(plane, &axisU, &axisV);
  return ProjectedWindingContainsCoplanarPoint(verts, vertCount, axisU, axisV, point, epsilon);
}

ExtraShadowTri_t   *g_shadowTriFreeList;
ShadowAabb_t       *g_shadowAabbFreeList;
ShadowAabb_t       *g_shadowAabbPool;
ShadowPartition_t  *g_shadowPartitionData;
ShadowPartition_t **g_shadowSortedMaxs;
ShadowPartition_t **g_shadowSortedMins;
ShadowTri_t        *g_shadowTriPool;

float g_shadowDummyVerts[16] =
{
  0.0,
  0.0,
  0.0,
  0.0,
  0.0,
  0.0,
  0.0,
  0.0,
  0.0,
  0.0,
  0.0,
  0.0,
  0.0,
  0.0,
  0.0,
  0.0
};

int     g_numShadowAabbNodes;
size_t  g_numShadowPartitions;
size_t  g_numShadowTris;
int    *g_shadowCategoryTable;
int    *g_shadowPartitionSizes;
int     g_shadowSplitAxis;
int    *g_shadowVertHashTable;
int    *g_shadowVertPartition;
int     g_shadowcasterTexSortKey;

char s_assertDisable_CastsSunShadowConfirmed;
char s_assertDisable_AddShadowTri;
char s_assertDisable_BuildShadowClusters;
char s_assertDisable_BuildShadowPartitions;
char s_assertDisable_BuildShadowPartitions;
char s_assertDisable_BuildShadowPartitions;
char s_assertDisable_ChooseSplitAxes;
char s_assertDisable_ClassifyShadowTriSoup;
char s_assertDisable_ClassifyShadowTriSoup;
char s_assertDisable_CreateShadowAabbLeaves;
char s_assertDisable_CreateShadowAabbLeaves;
char s_assertDisable_CreateShadowAabbLeaves;
char s_assertDisable_CreateShadowAabbLeaves;
char s_assertDisable_CreateShadowAabbLeaves;
char s_assertDisable_CreateShadowAabbLeaves;
char s_assertDisable_CreateShadowAabbLeaves;
char s_assertDisable_EmitShadowAabb;
char s_assertDisable_EmitShadowAabb;
char s_assertDisable_EmitSharedShadowIndices;
char s_assertDisable_EmitSharedShadowIndices;
char s_assertDisable_EmitSharedShadowIndices;
char s_assertDisable_FindSmallestEnclosingAabb;
char s_assertDisable_FindSmallestEnclosingAabb;
char s_assertDisable_FindSmallestEnclosingAabb;
char s_assertDisable_GetMaterialShadowCategory;
char s_assertDisable_GetMaterialShadowCategory;
char s_assertDisable_GetMaterialShadowCategory;
char s_assertDisable_NestShadowAabbs;
char s_assertDisable_ShadowAABB_Insert;
char s_assertDisable_ShadowAABB_Insert;
char s_assertDisable_ShadowAABB_Insert;
char s_assertDisable_ShadowAABB_Insert;
char s_assertDisable_ShadowAABB_Insert;
char s_assertDisable_ShadowAABB_Unlink;
char s_assertDisable_ShouldCastShadow;
char s_assertDisable_SortAndRemapShadowVerts;
char s_assertDisable_SortAndRemapShadowVerts;
char s_assertDisable_SortAndRemapShadowVerts;
char s_assertDisable_SortAndRemapShadowVerts;
char s_assertDisable_SortAndRemapShadowVerts;
char s_assertDisable_SortAndRemapShadowVerts;
char s_assertDisable_SplitShadowPartition;
char s_assertDisable_SplitShadowPartition;
char s_assertDisable_SplitShadowPartition;

/*
================
CastsSunShadowConfirmed

Converts the native transient sun-shadow classification to the boolean
serialized in BspTriSoup_t::castsSunShadow.

cod4map.exe: 0x45A700
================
*/
int CastsSunShadowConfirmed(unsigned char castsSunShadow)
{
  AssertMsg(castsSunShadow == CASTS_SUN_SHADOW_CONFIRMED ||
            castsSunShadow == CASTS_SUN_SHADOW_DENIED,
            s_assertDisable_CastsSunShadowConfirmed,
            "%s\n\t%i",
            "(castsSunShadow == CASTS_SUN_SHADOW_CONFIRMED || castsSunShadow == CASTS_SUN_SHADOW_DENIED)",
            castsSunShadow);
  return castsSunShadow == CASTS_SUN_SHADOW_CONFIRMED;
}

/* Native transient TriSurfProps stores its MapDrawSurf pointer at +0.  KIWI
   retains ShaderInfo at that native slot for its established consumers, so
   the exact source-surface relation is carried beside it. */
static MapDrawSurf_t *SunShadowMapDrawSurf(const TriSurfProps_t *props)
{
  TriSurfPropsSidecar_t *sidecar;

  Assert(props, s_assertDisable_CastsSunShadowConfirmed);
  sidecar = TrisPropsSidecar_Get((TriSurfProps_t *)props);
  Assert(sidecar, s_assertDisable_CastsSunShadowConfirmed);
  Assert(sidecar->mapDrawSurf, s_assertDisable_CastsSunShadowConfirmed);
  return sidecar->mapDrawSurf;
}

/* CoD4 0x45A670.  Coalesced props represent several input draw surfaces;
   the native category must therefore be written to every leaf, not merely
   the composite owner. */
char SetCoalescedSunShadowCategory(TriSurf_t *surf, char category)
{
  TriSurfProps_t *props;
  CoalesceNode_t *node;

  Assert(surf, s_assertDisable_CastsSunShadowConfirmed);
  props = surf->props;
  Assert(props, s_assertDisable_CastsSunShadowConfirmed);
  if (props->coalesceChain)
  {
    TriSurfProps_t *firstLeaf = (TriSurfProps_t *)props->coalesceChain->value;

    Assert(SunShadowMapDrawSurf(props) == SunShadowMapDrawSurf(firstLeaf),
           s_assertDisable_CastsSunShadowConfirmed);
    for (node = props->coalesceChain; node; node = node->next)
      SunShadowMapDrawSurf((TriSurfProps_t *)node->value)->castsSunShadow = (unsigned char)category;
  }
  else
  {
    SunShadowMapDrawSurf(props)->castsSunShadow = (unsigned char)category;
  }
  return category;
}

/* CoD4 0x45A650. */
int ConfirmCoalescedSunShadowCategory(TriSurf_t *surf)
{
  return SetCoalescedSunShadowCategory(surf, CASTS_SUN_SHADOW_CONFIRMED);
}

/* CoD4 0x45A7F0. */
int DisableSunShadowTrisForEntityRange(void)
{
  int drawSurfIndex;

  for (drawSurfIndex = g_entities[g_currentEntityIndex].firstDrawSurf;
       drawSurfIndex < numMapDrawSurfs;
       ++drawSurfIndex)
  {
    g_nativeMapDrawSurfs[drawSurfIndex].castsSunShadow = CASTS_SUN_SHADOW_CONFIRMED;
  }
  return drawSurfIndex;
}

/* CoD4 0x45A9E0.  Returning zero terminates the native grid-tree trace. */
unsigned char SunShadowSegmentHitCallback(TriSurf_t *surf, float *segmentStart,
                                          float *segmentEnd, int userData)
{
  float startSide;
  float endSide;
  float fraction;
  float hitPoint[3];
  float *plane;

  (void)userData;
  Assert(surf, s_assertDisable_CastsSunShadowConfirmed);
  Assert(surf->props, s_assertDisable_CastsSunShadowConfirmed);
  if ((surf->props->si->gameFlags & 2) == 0)
    return 1;

  plane = surf->props->plane;
  startSide = DotProduct(segmentStart, plane) - plane[3];
  endSide = DotProduct(segmentEnd, plane) - plane[3];
  if (startSide * endSide >= 0.0f)
    return 1;

  fraction = startSide / (startSide - endSide);
  hitPoint[0] = (segmentEnd[0] - segmentStart[0]) * fraction + segmentStart[0];
  hitPoint[1] = (segmentEnd[1] - segmentStart[1]) * fraction + segmentStart[1];
  hitPoint[2] = (segmentEnd[2] - segmentStart[2]) * fraction + segmentStart[2];
  return ProjectedWindingContainsPoint(&surf->winding->points[0][0], surf->winding->numpoints,
                                       plane, hitPoint, 0.0099999998f) == 0;
}

/* CoD4 0x45A970. */
int SunShadowVertexIsExposed(MeshVert_t *vert)
{
  float segmentStart[3];
  float segmentEnd[3];

  Vector2MA(vert->pos, 0.5f, vert->normal, segmentStart);
  segmentStart[2] = vert->pos[2] - 0.5f;
  VectorCopy(segmentStart, segmentEnd);
  segmentEnd[2] -= 16384.0f;
  return GridTree_SegmentTraverse(segmentStart, segmentEnd, 0, SunShadowSegmentHitCallback);
}

/* CoD4 0x45A830. */
void ClassifySunShadowTri(MapDrawSurf_t *drawSurf, float *sunDirection)
{
  int index;

  drawSurf->castsSunShadow = CASTS_SUN_SHADOW_CONFIRMED;
  for (index = 0; index < drawSurf->vertCount; ++index)
  {
    if (drawSurf->verts[index].lmSt[1] < 0.5f)
      return;
  }
  for (index = 0; index < drawSurf->u.indexed.indexCount; index += 3)
  {
    int *indexes = drawSurf->u.indexed.indexes;

    if (TriangleFacesSun(drawSurf->verts[indexes[index]].pos,
                         drawSurf->verts[indexes[index + 1]].pos,
                         drawSurf->verts[indexes[index + 2]].pos,
                         sunDirection))
      return;
  }
  for (index = 0; index < drawSurf->vertCount; ++index)
  {
    if (!SunShadowVertexIsExposed(&drawSurf->verts[index]))
      return;
  }
  drawSurf->castsSunShadow = CASTS_SUN_SHADOW_DENIED;
}

/* CoD4 0x45A750. */
int PreprocessSunShadowTriCategories(void)
{
  float sunAngles[3];
  float sunDirection[3];
  int drawSurfIndex;

  if (g_currentEntityIndex)
    return DisableSunShadowTrisForEntityRange();

  GetVectorForKey(&g_entities[0], "sundirection", sunAngles);
  AngleVectors(sunAngles, sunDirection, NULL, NULL);
  for (drawSurfIndex = 0; drawSurfIndex < numMapDrawSurfs; ++drawSurfIndex)
  {
    MapDrawSurf_t *drawSurf = &g_nativeMapDrawSurfs[drawSurfIndex];

    if (drawSurf->isTerrain)
      ClassifySunShadowTri(drawSurf, sunDirection);
    else
      drawSurf->castsSunShadow = CASTS_SUN_SHADOW_CONFIRMED;
  }
  return drawSurfIndex;
}


/*
================
AllocShadowAABB

Allocates a shadow AABB node from the pool.
================
*/
ShadowAabb_t *AllocShadowAABB(void)
{
  ShadowAabb_t *aabb;

  if ( g_numShadowAabbNodes == MAX_MAP_SHADOW_AABBTREES )
    Com_Error("MAX_MAP_SHADOW_AABBS (%i) exceeded\n", MAX_MAP_SHADOW_AABBTREES);
  aabb = &g_shadowAabbPool[g_numShadowAabbNodes++];
  memset(aabb, 0, sizeof(ShadowAabb_t));
  return aabb;
}

/*
================
ShadowAABB_Unlink

Removes an AABB node from its sibling list.
================
*/
int ShadowAABB_Unlink(ShadowAabb_t *aabb)
{
  Assert(aabb, s_assertDisable_ShadowAABB_Unlink);

  /* update next sibling's prev pointer */
  if ( aabb->nextSibling )
    aabb->nextSibling->prevSibling = aabb->prevSibling;

  /* update prev sibling or parent's first child */
  if ( aabb->prevSibling )
  {
    aabb->prevSibling->nextSibling = aabb->nextSibling;
  }
  else if ( aabb->parent )
  {
    aabb->parent->firstChild = aabb->nextSibling;
  }
  else
  {
    g_shadowAabbFreeList = aabb->nextSibling;
  }

  aabb->parent = NULL;
  aabb->nextSibling = NULL;
  aabb->prevSibling = NULL;
  return 0;
}

/*
================
ShadowAABB_Insert

Inserts an AABB as child of parent, or into root list.
================
*/
void ShadowAABB_Insert(ShadowAabb_t *parent, ShadowAabb_t *aabb)
{
  ShadowAabb_t *nextSib;

  Assert(aabb, s_assertDisable_ShadowAABB_Insert);
  Assert(!aabb->parent, s_assertDisable_ShadowAABB_Insert);
  Assert(!aabb->nextSibling, s_assertDisable_ShadowAABB_Insert);
  Assert(!aabb->prevSibling, s_assertDisable_ShadowAABB_Insert);
  if ( parent )
  {
    aabb->nextSibling = parent->firstChild;
    parent->firstChild = aabb;
  }
  else
  {
    aabb->nextSibling = g_shadowAabbFreeList;
    g_shadowAabbFreeList = aabb;
  }
  nextSib = aabb->nextSibling;
  if ( nextSib )
    nextSib->prevSibling = aabb;
  AssertFatal(!aabb->prevSibling, s_assertDisable_ShadowAABB_Insert);
  aabb->parent = parent;
}

/*
================
GetMaterialShadowCategory

Returns shadow category for a material (1=shared, 2=unique, 3=none).
================
*/
int GetMaterialShadowCategory(int materialIndex, double unusedFpu)
{
  ShaderInfo_t *si;
  int *catTable;

  Assert(materialIndex >= 0, s_assertDisable_GetMaterialShadowCategory);
  Assert(materialIndex < MAX_MAP_MATERIALS, s_assertDisable_GetMaterialShadowCategory);

  catTable = g_shadowCategoryTable;
  if ( catTable[materialIndex] )
    return catTable[materialIndex];

  /* look up material and classify */
  si = LoadMaterial(bspMaterials[materialIndex].material);
  Assert(si, s_assertDisable_GetMaterialShadowCategory);

  if ( si->texSortKey == g_shadowcasterTexSortKey )
    catTable[materialIndex] = 1;  /* shared shadowcaster */
  else if ( si->texSortKey != 0 )
    catTable[materialIndex] = 2;  /* unique shadowcaster */
  else
    catTable[materialIndex] = 3;  /* no shadow */

  return catTable[materialIndex];
}

/*
================
PlaneFromPointsReversed

Computes plane from reversed winding, negates dist.
================
*/
char PlaneFromPointsReversed(float *p0, float *p1, float *p2, float *plane)
{
  if ( !PlaneFromPoints(plane, p1, p2, p0) )
    return 0;

  plane[3] = -plane[3];
  return 1;
}

/*
================
ComputeShadowAABBBounds

Computes bounds from a triSoup's vertices.
================
*/
void ComputeShadowAABBBounds(BspTriSoup_t *ts, float *mins, float *maxs)
{
  BspDrawVert_t *vertBase;
  unsigned short *indices;
  int i;

  ClearBounds(mins, maxs);
  vertBase = &bspTriSoupData[ts->firstVertex];
  indices = &bspDrawIndexes[ts->firstIndex];

  for ( i = 0; i < ts->indexCount; i++ )
    AddPointToBounds(vertBase[indices[i]].pos, mins, maxs);
}

/*
================
ShadowPartitionCompareMaxs

qsort comparator: partitions by max bound on split axis.
================
*/
int ShadowPartitionCompareMaxs(ShadowPartition_t **lhs, ShadowPartition_t **rhs)
{
  double diff;
  ShadowPartition_t *a = *lhs, *b = *rhs;

  diff = a->maxs[g_shadowSplitAxis] - b->maxs[g_shadowSplitAxis];
  if ( diff == 0.0 )
    diff = a->mins[g_shadowSplitAxis] - b->mins[g_shadowSplitAxis];
  if ( diff >= 0.0 )
    return diff > 0.0;
  else
    return -1;
}

/*
================
ShadowPartitionCompareMins

qsort comparator: partitions by min bound on split axis.
================
*/
int ShadowPartitionCompareMins(ShadowPartition_t **lhs, ShadowPartition_t **rhs)
{
  double diff;
  ShadowPartition_t *a = *lhs, *b = *rhs;

  diff = a->mins[g_shadowSplitAxis] - b->mins[g_shadowSplitAxis];
  if ( diff == 0.0 )
    diff = a->maxs[g_shadowSplitAxis] - b->maxs[g_shadowSplitAxis];
  if ( diff >= 0.0 )
    return diff > 0.0;
  else
    return -1;
}

/*
================
SplitShadowPartition

Splits a partition into two halves along an axis.
================
*/
char SplitShadowPartition(int numPartitions, int totalVerts, int partitionId, int axis)
{
  ShadowPartition_t *part;
  int usedCount, frontVertCount, backVertCount;
  int frontPartId, backPartId;
  int frontIndex, backIndex;
  vec3_t frontMins, frontMaxs, backMins, backMaxs;
  int i, done;

  /* collect all partitions matching this ID into sort arrays */
  usedCount = 0;
  for ( i = 0; i < numPartitions; i++ )
  {
    if ( g_shadowPartitionData[i].partitionId == partitionId )
    {
      g_shadowSortedMaxs[usedCount] = &g_shadowPartitionData[i];
      g_shadowSortedMins[usedCount] = &g_shadowPartitionData[i];
      usedCount++;
    }
  }

  if ( usedCount <= 1 )
    Assert(0, s_assertDisable_SplitShadowPartition);

  /* sort by max and min bounds along the split axis */
  g_shadowSplitAxis = axis;
  qsort(g_shadowSortedMaxs, usedCount, sizeof(ShadowPartition_t *), (int (*)(const void*, const void*))ShadowPartitionCompareMaxs);
  qsort(g_shadowSortedMins, usedCount, sizeof(ShadowPartition_t *), (int (*)(const void*, const void*))ShadowPartitionCompareMins);

  /* assign partitions to front (high maxs) and back (low mins) halves */
  frontPartId = (int)g_numShadowPartitions;
  backPartId = (int)g_numShadowPartitions + 1;
  frontVertCount = 0;
  backVertCount = 0;
  frontIndex = 0;
  backIndex = 0;
  ClearBounds(frontMins, frontMaxs);
  ClearBounds(backMins, backMaxs);

  done = 0;
  while ( !done )
  {
    /* assign from front (sorted by maxs) until front has more verts */
    while ( backVertCount >= frontVertCount )
    {
      Assert(frontIndex < usedCount, s_assertDisable_SplitShadowPartition);
      part = g_shadowSortedMaxs[frontIndex++];
      if ( part->partitionId == partitionId )
      {
        AddBoundsToBounds(part->mins, part->maxs, frontMins, frontMaxs);
        part->partitionId = frontPartId;
        frontVertCount += part->vertCount;
        if ( backVertCount + frontVertCount == totalVerts )
        { done = 1; break; }
      }
    }
    if ( done ) break;

    /* assign from back (sorted by mins) until back has more verts */
    while ( 1 )
    {
      Assert(backIndex < usedCount, s_assertDisable_SplitShadowPartition);
      part = g_shadowSortedMins[backIndex++];
      if ( part->partitionId == partitionId )
      {
        AddBoundsToBounds(part->mins, part->maxs, backMins, backMaxs);
        backVertCount += part->vertCount;
        part->partitionId = backPartId;
        if ( backVertCount + frontVertCount == totalVerts )
        { done = 1; break; }
      }
      if ( backVertCount > frontVertCount )
        break;
    }
  }

  g_numShadowPartitions += 2;
  PartitionShadowVerts(numPartitions, frontVertCount, frontPartId, frontMins, frontMaxs);
  PartitionShadowVerts(numPartitions, backVertCount, backPartId, backMins, backMaxs);
  return 1;
}

/*
================
ShadowClusterCompareSize

qsort comparator: clusters by size descending.
================
*/
int ShadowClusterCompareSize(int *lhs, int *rhs)
{
  return g_shadowPartitionSizes[*rhs] - g_shadowPartitionSizes[*lhs];
}

/*
================
BuildShadowClusters

Merges partitions into clusters (max 65536 verts each).
================
*/
void BuildShadowClusters(int numPartitions)
{
  int *sizeArray, *sortedIndices, *clusterMap, *clusterIdMap;
  int i, j, k, merged;
  int overallFirstVert, clusterIdx;

  /* accumulate vertex counts per partition */
  sizeArray = malloc(g_numShadowPartitions * sizeof(int));
  memset(sizeArray, 0, g_numShadowPartitions * sizeof(int));
  g_shadowPartitionSizes = sizeArray;

  for ( i = 0; i < numPartitions; i++ )
    sizeArray[g_shadowPartitionData[i].partitionId] += g_shadowPartitionData[i].vertCount;

  /* build sorted index array for greedy merging */
  sortedIndices = malloc(g_numShadowPartitions * sizeof(int));
  for ( i = 0; i < (int)g_numShadowPartitions; i++ )
    sortedIndices[i] = i;

  /* greedily merge small partitions until no pair fits in 65536 verts */
  for ( ;; )
  {
    merged = 0;
    qsort(sortedIndices, g_numShadowPartitions, sizeof(int), (int (*)(const void*, const void*))ShadowClusterCompareSize);

    for ( j = 0; j < (int)g_numShadowPartitions; j++ )
    {
      if ( !sizeArray[sortedIndices[j]] )
        break;
      for ( k = j + 1; k < (int)g_numShadowPartitions; k++ )
      {
        if ( !sizeArray[sortedIndices[k]] )
          break;
        #define MAX_SHADOW_CLUSTER_VERTS 65536
        if ( sizeArray[sortedIndices[j]] + sizeArray[sortedIndices[k]] <= MAX_SHADOW_CLUSTER_VERTS )
        {
          /* merge partition k into j */
          for ( i = 0; i < numPartitions; i++ )
          {
            if ( g_shadowPartitionData[i].partitionId == sortedIndices[k] )
              g_shadowPartitionData[i].partitionId = sortedIndices[j];
          }
          sizeArray[sortedIndices[j]] += sizeArray[sortedIndices[k]];
          sizeArray[sortedIndices[k]] = 0;
          merged = 1;
          break;
        }
      }
      if ( merged )
        break;
    }
    if ( !merged )
      break;
  }
  free(sortedIndices);

  /* assign clusters from non-empty partitions */
  clusterMap = malloc(g_numShadowPartitions * sizeof(int));
  clusterIdMap = malloc(g_numShadowPartitions * sizeof(int));
  overallFirstVert = 0;

  for ( i = 0; i < (int)g_numShadowPartitions; i++ )
  {
    if ( !sizeArray[i] )
      continue;

    if ( numBSPShadowClusters == MAX_MAP_SHADOW_CLUSTERS )
      Com_Error("MAX_MAP_SHADOW_CLUSTERS (%i) exceeded\n", MAX_MAP_SHADOW_CLUSTERS);

    clusterIdx = numBSPShadowClusters++;
    bspShadowClusters[clusterIdx].firstVert = overallFirstVert;
    bspShadowClusters[clusterIdx].count = sizeArray[i];
    clusterMap[i] = overallFirstVert;
    clusterIdMap[i] = clusterIdx;
    overallFirstVert += sizeArray[i];
  }

  AssertFatal(overallFirstVert == numBSPShadowVerts, s_assertDisable_BuildShadowClusters);

  /* remap partition data to cluster assignments */
  for ( i = 0; i < numPartitions; i++ )
  {
    int pid = g_shadowPartitionData[i].partitionId;
    g_shadowPartitionData[i].firstVert = clusterMap[pid];
    g_shadowPartitionData[i].partitionId = clusterIdMap[pid];
  }

  free(clusterMap);
  free(clusterIdMap);
  free(g_shadowPartitionSizes);
}

/*
================
ShadowTriPartitionCompare

qsort comparator: tris by cluster then index.
================
*/
int ShadowTriPartitionCompare(int *lhs, int *rhs)
{
  int triA;
  int triB;

  triA = g_shadowVertPartition[*lhs];
  triB = g_shadowVertPartition[*rhs];
  if ( g_shadowPartitionData[triA].partitionId == g_shadowPartitionData[triB].partitionId )
    return triA - triB;
  else
    return g_shadowPartitionData[triA].partitionId - g_shadowPartitionData[triB].partitionId;
}

/*
================
PartitionShadowTris_r

Recursively reassigns tris from one partition to another.
================
*/
int PartitionShadowTris_r(int targetPartition, int sourcePartition, int numTris, int uniqueCount)
{
  int i, j, matchCount;

  if ( !sourcePartition || sourcePartition == targetPartition )
    return uniqueCount;
  if ( numTris <= 0 )
    return uniqueCount - 1;

  /* reassign all vertices from source to target partition */
  for ( i = 0; i < numTris; i++ )
  {
    for ( j = 0; j < 3; j++ )
    {
      if ( g_shadowVertPartition[g_shadowTriPool[i].vertIdx[j]] == sourcePartition )
        g_shadowVertPartition[g_shadowTriPool[i].vertIdx[j]] = targetPartition;
    }
  }

  /* walk tris and recursively merge connected partitions */
  for ( i = 0; i < numTris; i++ )
  {
    matchCount = 0;
    for ( j = 0; j < 3; j++ )
    {
      if ( g_shadowVertPartition[g_shadowTriPool[i].vertIdx[j]] == targetPartition )
        matchCount++;
    }

    if ( matchCount == 0 )
      continue;

    g_shadowTriPool[i].partitionId = targetPartition;

    /* if not all 3 verts matched, recurse into neighbors */
    if ( matchCount != 3 )
    {
      for ( j = 0; j < 3; j++ )
        uniqueCount = PartitionShadowTris_r(targetPartition, g_shadowVertPartition[g_shadowTriPool[i].vertIdx[j]], numTris, uniqueCount);
    }
  }

  return uniqueCount - 1;
}

/*
================
ShadowTriSortCompare

qsort comparator: tris by partition ID.
================
*/
int ShadowTriSortCompare(ShadowTri_t *lhs, ShadowTri_t *rhs)
{
  return lhs->partitionId - rhs->partitionId;
}

/*
================
ComputeShadowTriBounds

Computes bounds of a shadow tri from its vertices.
================
*/
void ComputeShadowTriBounds(ShadowTri_t *tri, float *mins, float *maxs)
{
  /* seed bounds from first vertex, expand with remaining two */
  VectorCopy(bspShadowVerts[tri->vertIdx[0]].xyz, mins);
  VectorCopy(bspShadowVerts[tri->vertIdx[0]].xyz, maxs);
  AddPointToBounds(bspShadowVerts[tri->vertIdx[1]].xyz, mins, maxs);
  AddPointToBounds(bspShadowVerts[tri->vertIdx[2]].xyz, mins, maxs);
}

/*
================
CreateShadowAabbLeaves

Creates AABB leaf nodes from build tree results.
================
*/
int CreateShadowAabbLeaves(AabbTreeBuilder_t *builder, int firstTriIndex, int nodeCount)
{
  ShadowAabb_t *pool;
  ShadowAabb_t *node, *child;
  ShadowTri_t *tri;
  ShadowBuildNode_t *buildNodes;
  int baseIndex, i, j;

  pool = g_shadowAabbPool;
  buildNodes = (ShadowBuildNode_t *)builder->nodes;
  baseIndex = g_numShadowAabbNodes;

  /* phase 1: allocate and initialize leaf nodes */
  for ( i = 0; i < nodeCount; i++ )
  {
    if ( g_numShadowAabbNodes == MAX_MAP_SHADOW_AABBTREES )
      Com_Error("MAX_MAP_SHADOW_AABBS (%i) exceeded\n", MAX_MAP_SHADOW_AABBTREES);

    node = &pool[g_numShadowAabbNodes++];
    memset(node, 0, sizeof(ShadowAabb_t));

    AssertFatal(baseIndex + i == g_numShadowAabbNodes - 1, s_assertDisable_CreateShadowAabbLeaves);
    AssertFatal(node == &pool[baseIndex + i], s_assertDisable_CreateShadowAabbLeaves);

    node->nodeType = 1;
    node->materialPartition = g_shadowTriPool[firstTriIndex].clusterId;
    node->triSoupIndex = firstTriIndex + buildNodes[i].triStartIndex;
    node->triCount = buildNodes[i].triCount;

    AssertFatal(!node->parent, s_assertDisable_CreateShadowAabbLeaves);
    AssertFatal(!node->firstChild, s_assertDisable_CreateShadowAabbLeaves);
    AssertFatal(!node->prevSibling, s_assertDisable_CreateShadowAabbLeaves);
    AssertFatal(!node->nextSibling, s_assertDisable_CreateShadowAabbLeaves);

    /* compute bounds from all tris in this leaf */
    ClearBounds(node->mins, node->maxs);
    for ( j = 0; j < node->triCount; j++ )
    {
      tri = &g_shadowTriPool[node->triSoupIndex + j];
      AddPointToBounds(bspShadowVerts[tri->vertIdx[0]].xyz, node->mins, node->maxs);
      AddPointToBounds(bspShadowVerts[tri->vertIdx[1]].xyz, node->mins, node->maxs);
      AddPointToBounds(bspShadowVerts[tri->vertIdx[2]].xyz, node->mins, node->maxs);
    }
  }

  /* insert first node into free list */
  ShadowAABB_Insert(NULL, &pool[baseIndex]);

  /* phase 2: link parent-child relationships from build tree */
  for ( i = 0; i < nodeCount; i++ )
  {
    node = &pool[baseIndex + i];

    if ( buildNodes[i].childCount )
    {
      node->firstChild = &pool[baseIndex + buildNodes[i].firstChildIndex];

      for ( j = 0; j < buildNodes[i].childCount; j++ )
      {
        child = &pool[baseIndex + buildNodes[i].firstChildIndex + j];
        child->nextSibling = (j < buildNodes[i].childCount - 1) ? child + 1 : NULL;
        child->prevSibling = (j > 0) ? child - 1 : NULL;
        child->parent = node;
      }
    }
    else
    {
      AssertFatal(!node->firstChild, s_assertDisable_CreateShadowAabbLeaves);
    }
  }

  return nodeCount > 0 ? nodeCount - 1 : 0;
}

/*
================
BuildShadowAabbTrees

Builds AABB trees for each shadow tri partition.
================
*/
void BuildShadowAabbTrees(void)
{
  int groupEnd, nodeCount, partitionId;
  int triIdxStart;
  AabbTreeBuilder_t builder;

  builder.hasBoundsData = 0;
  builder.itemMins = malloc(g_numShadowTris * sizeof(vec3_t));
  builder.itemMaxs = malloc(g_numShadowTris * sizeof(vec3_t));
  builder.nodes = malloc(MAX_AABB_BUILD_NODES * sizeof(AabbTreeNode_t));
  builder.maxNodes = MAX_AABB_BUILD_NODES;
  builder.minPartitionSize = AABB_MIN_PARTITION_SIZE;
  builder.minLeafItems = AABB_MIN_LEAF_ITEMS;

  triIdxStart = 0;
  while ( triIdxStart < (int)g_numShadowTris )
  {
    /* compute bounds for first tri in group */
    ComputeShadowTriBounds(&g_shadowTriPool[triIdxStart], builder.itemMins, builder.itemMaxs);
    partitionId = g_shadowTriPool[triIdxStart].partitionId;
    groupEnd = triIdxStart + 1;

    /* extend group while consecutive tris share the same partition */
    while ( groupEnd < (int)g_numShadowTris && g_shadowTriPool[groupEnd].partitionId == partitionId )
    {
      int localIdx = groupEnd - triIdxStart;
      ComputeShadowTriBounds(&g_shadowTriPool[groupEnd], &builder.itemMins[localIdx * 3], &builder.itemMaxs[localIdx * 3]);
      groupEnd++;
    }

    /* build AABB tree for this group */
    builder.itemData = &g_shadowTriPool[triIdxStart];
    builder.itemCount = groupEnd - triIdxStart;
    builder.itemStride = sizeof(ShadowTri_t);
    nodeCount = AabbBuildTree(&builder);
    CreateShadowAabbLeaves(&builder, triIdxStart, nodeCount);

    triIdxStart = groupEnd;
  }

  free(builder.itemMins);
  free(builder.itemMaxs);
  free(builder.nodes);
}

/*
================
FindSmallestEnclosingAabb

Finds smallest enclosing AABB for nesting.
================
*/
ShadowAabb_t *FindSmallestEnclosingAabb(ShadowAabb_t *aabb, ShadowAabb_t *searchList, ShadowAabb_t *best, float *bestVolume)
{
  ShadowAabb_t *candidate, *childResult;
  vec3_t size;
  double volume;

  Assert(aabb, s_assertDisable_FindSmallestEnclosingAabb);
  Assert(bestVolume, s_assertDisable_FindSmallestEnclosingAabb);

  for ( candidate = searchList; candidate; candidate = candidate->nextSibling )
  {
    /* skip self and unique leaves */
    if ( candidate == aabb || candidate->nodeType == 2 )
      continue;

    /* check if candidate fully encloses aabb */
    if ( candidate->mins[0] > (double)aabb->mins[0]
      || candidate->mins[1] > (double)aabb->mins[1]
      || candidate->mins[2] > (double)aabb->mins[2]
      || candidate->maxs[0] < (double)aabb->maxs[0]
      || candidate->maxs[1] < (double)aabb->maxs[1]
      || candidate->maxs[2] < (double)aabb->maxs[2] )
      continue;

    /* compute candidate volume */
    VectorSubtract(candidate->maxs, candidate->mins, size);
    volume = size[0] * size[1] * size[2];
    if ( volume > *bestVolume )
      continue;

    /* candidate is smaller — recurse into its children */
    *bestVolume = volume;
    childResult = FindSmallestEnclosingAabb(aabb, candidate->firstChild, candidate, bestVolume);
    AssertFatal(childResult, s_assertDisable_FindSmallestEnclosingAabb);

    /* only accept if combined tri count fits in index buffer */
    if ( 3 * (aabb->triCount + childResult->triCount) < MAX_SHADOW_GROUP_INDEXES )
      best = childResult;
  }

  return best;
}

/*
================
NestShadowAabbs

Nests smaller AABBs inside larger enclosing ones.
================
*/
void NestShadowAabbs(void)
{
  ShadowAabb_t *aabb, *nextAabb, *container;
  float minVolume;

  for ( aabb = g_shadowAabbFreeList; aabb; aabb = nextAabb )
  {
    nextAabb = aabb->nextSibling;
    if ( aabb->nodeType == 2 )
      continue;

    /* find the smallest AABB that fully contains this one */
    minVolume = FLT_MAX;
    container = FindSmallestEnclosingAabb(aabb, g_shadowAabbFreeList, NULL, &minVolume);
    if ( container )
    {
      AssertFatal(aabb != container, s_assertDisable_NestShadowAabbs);
      ShadowAABB_Unlink(aabb);
      ShadowAABB_Insert(container, aabb);
    }
  }
}

/*
================
EmitSharedShadowIndices

Emits shadow indices for shared-shadow AABB nodes.
================
*/
void EmitSharedShadowIndices(ShadowAabb_t *aabb, DiskShadowAabb_t *disk)
{
  int indexBase, triIdx, startIndex;
  ShadowTri_t *tri;
  ShadowAabb_t *child;
  DiskShadowAabb_t *childDisk;

  Assert(aabb, s_assertDisable_EmitSharedShadowIndices);
  Assert(aabb->nodeType == 1, s_assertDisable_EmitSharedShadowIndices);

  disk->triSoupIndex = numBSPShadowIndices;
  indexBase = numBSPShadowIndices;

  if ( aabb->triCount + numBSPShadowIndices + 2 * aabb->triCount > MAX_MAP_SHADOW_INDEXES )
  {
    Com_Error("MAX_MAP_SHADOW_INDICES (%i) exceeded\n", MAX_MAP_SHADOW_INDEXES);
    indexBase = numBSPShadowIndices;
  }

  /* emit indices for tris not covered by child nodes */
  for ( triIdx = aabb->triSoupIndex; triIdx < aabb->triSoupIndex + aabb->triCount; triIdx++ )
  {
    tri = &g_shadowTriPool[triIdx];

    /* check if this tri belongs to a child AABB */
    child = aabb->firstChild;
    while ( child )
    {
      if ( triIdx >= child->triSoupIndex && triIdx < child->triSoupIndex + child->triCount )
        break;
      child = child->nextSibling;
    }

    if ( !child )
    {
      /* tri not in any child — emit its indices */
      bspShadowIndexes[indexBase] = (unsigned short)tri->vertIdx[0] - (unsigned short)tri->firstVert;
      bspShadowIndexes[indexBase + 1] = (unsigned short)tri->vertIdx[1] - (unsigned short)tri->firstVert;
      indexBase += 3;
      bspShadowIndexes[indexBase - 1] = (unsigned short)tri->vertIdx[2] - (unsigned short)tri->firstVert;
      numBSPShadowIndices = indexBase;
    }
  }

  /* recurse into shared-shadow children */
  child = aabb->firstChild;
  childDisk = &bspShadowData[disk->firstChildIndex];
  while ( child )
  {
    if ( child->nodeType == 1 )
    {
      EmitSharedShadowIndices(child, childDisk);
      indexBase = numBSPShadowIndices;
    }
    child = child->nextSibling;
    childDisk++;
  }

  startIndex = disk->triSoupIndex;
  disk->indexCount = indexBase - startIndex;
  Assert(indexBase != startIndex, s_assertDisable_EmitSharedShadowIndices);
}

/*
================
EmitSharedShadowIndices_Siblings

Emits shared indices for each sibling AABB.
================
*/
void EmitSharedShadowIndices_Siblings(DiskShadowAabb_t *disk, ShadowAabb_t *aabbList)
{
  ShadowAabb_t *aabb;

  for ( aabb = aabbList; aabb; aabb = aabb->nextSibling, disk++ )
  {
    if ( aabb->nodeType == 1 )
      EmitSharedShadowIndices(aabb, disk);
  }
}

/*
================
EmitShadowAabb

Emits one AABB node to BSP shadow data.
================
*/
int EmitShadowAabb(ShadowAabb_t *aabb)
{
  DiskShadowAabb_t *disk;

  Assert(aabb, s_assertDisable_EmitShadowAabb);
  Assert(numBSPShadowAabbTrees < MAX_MAP_SHADOW_AABBTREES, s_assertDisable_EmitShadowAabb);
  disk = bspShadowData + numBSPShadowAabbTrees++;
  disk->materialPartition = (char)aabb->materialPartition;
  if ( aabb->nodeType == 2 )
  {
    disk->isLeaf = 1;
    disk->triSoupIndex = aabb->triSoupIndex;
  }
  else
  {
    disk->isLeaf = 0;
  }
  VectorCopy(aabb->mins, disk->mins);
  VectorCopy(aabb->maxs, disk->maxs);
  return 0;
}

/*
================
EmitShadowAabbTree_r

Recursively emits AABB tree to BSP data.
================
*/
int EmitShadowAabbTree_r(ShadowAabb_t *aabbList)
{
  ShadowAabb_t *aabb;
  DiskShadowAabb_t *disk;
  int baseIndex, count;

  baseIndex = numBSPShadowAabbTrees;

  /* first pass: emit all sibling nodes at this level */
  for ( aabb = aabbList; aabb; aabb = aabb->nextSibling )
    EmitShadowAabb(aabb);

  /* second pass: recurse into children and link disk nodes */
  count = 0;
  disk = bspShadowData + baseIndex;
  for ( aabb = aabbList; aabb; aabb = aabb->nextSibling, disk++, count++ )
  {
    disk->firstChildIndex = numBSPShadowAabbTrees;
    disk->childCount = EmitShadowAabbTree_r(aabb->firstChild);
  }

  return count;
}

/*
================
ShadowInit

Initializes shadow system pools and shadowcaster material.
================
*/
int ShadowInit(double unusedFpu)
{
  /* allocate shadow category lookup table */
  g_shadowCategoryTable = malloc(MAX_MAP_MATERIALS * sizeof(int));
  memset(g_shadowCategoryTable, 0, MAX_MAP_MATERIALS * sizeof(int));
  g_shadowcasterTexSortKey = LoadMaterial("shadowcaster")->texSortKey;

  /* allocate shadow pools */
  #define MAX_SHADOW_TRIS       (0x1800000 / sizeof(ShadowTri_t))
  #define SHADOW_VERT_HASH_SIZE  0x100000  /* 1M entries */
  g_shadowTriPool = malloc(MAX_SHADOW_TRIS * sizeof(ShadowTri_t));
  g_shadowAabbPool = malloc(MAX_MAP_SHADOW_AABBTREES * sizeof(ShadowAabb_t));
  g_shadowAabbFreeList = NULL;
  g_numShadowAabbNodes = 0;

  /* allocate and clear vertex hash table (0xFF = empty) */
  g_shadowVertHashTable = malloc(SHADOW_VERT_HASH_SIZE * sizeof(int));
  memset(g_shadowVertHashTable, 0xFF, SHADOW_VERT_HASH_SIZE * sizeof(int));
  return -1;
}

/*
================
ShouldCastShadow

Returns true if surface should cast shadows.
================
*/
int ShouldCastShadow(ShaderInfo_t *si)
{
  char *assertMsg;

  Assert(si->surfaceFlags & SURF_NODRAW, s_assertDisable_ShouldCastShadow);

  return (si->surfaceFlags & SURF_NOCASTSHADOW) == 0;
}

/*
================
AddShadowTri

Adds a shadow tri to the extra tri linked list.
================
*/
int AddShadowTri(int usage, float *v0, float *v1, float *v2)
{
  ExtraShadowTri_t *tri;

  Assert(usage, s_assertDisable_AddShadowTri);
  tri = malloc(sizeof(*tri));
  if ( !tri )
    Com_Error("Out of memory");

  tri->usage = usage;
  VectorCopy(v0, tri->v0);
  VectorCopy(v1, tri->v1);
  VectorCopy(v2, tri->v2);
  tri->next = g_shadowTriFreeList;
  g_shadowTriFreeList = tri;

  return 0;
}

/*
================
ShadowVertHash

Computes hash index for a shadow vertex position.
================
*/
int ShadowVertHash(float *pos)
{
  return (SHADOW_HASH_PRIME_X * xs_RoundToInt(pos[0] + FISTP_BIAS)
        + SHADOW_HASH_PRIME_Y * xs_RoundToInt(pos[1] + FISTP_BIAS)
        + SHADOW_HASH_PRIME_Z * xs_RoundToInt(pos[2] + FISTP_BIAS)) & SHADOW_VERT_HASH_MASK;
}

/*
================
FindOrAddShadowVert

Finds or adds a shadow vertex by position.
================
*/
size_t FindOrAddShadowVert(float *pos)
{
  int slot, existingIdx;
  size_t newIdx;

  /* open-address hash lookup */
  slot = ShadowVertHash(pos);
  existingIdx = g_shadowVertHashTable[slot];

  while ( existingIdx >= 0 )
  {
    if ( pos[0] == bspShadowVerts[existingIdx].xyz[0]
      && pos[1] == bspShadowVerts[existingIdx].xyz[1]
      && pos[2] == bspShadowVerts[existingIdx].xyz[2] )
      return existingIdx;

    slot = (slot + 1) & SHADOW_VERT_HASH_MASK;
    existingIdx = g_shadowVertHashTable[slot];
  }

  /* not found — add new vertex */
  if ( numBSPShadowVerts == MAX_MAP_SHADOW_VERTS )
    Com_Error("MAX_MAP_SHADOW_VERTS (%i) exceeded\n", MAX_MAP_SHADOW_VERTS);

  newIdx = numBSPShadowVerts++;
  g_shadowVertHashTable[slot] = (int)newIdx;
  VectorCopy(pos, bspShadowVerts[newIdx].xyz);
  return newIdx;
}

/*
================
EmitShadowTriIndices

Emits 3 vertex indices for a shadow triangle.
================
*/
size_t EmitShadowTriIndices(float *triVerts)
{
  ShadowTri_t *tri;

  #define MAX_MAP_SHADOW_INDICES MAX_MAP_SHADOW_INDEXES
  if ( (int)(3 * g_numShadowTris) > MAX_MAP_SHADOW_INDICES )
    Com_Error("MAX_MAP_SHADOW_INDICES (%i) exceeded\n", MAX_MAP_SHADOW_INDICES);

  tri = &g_shadowTriPool[g_numShadowTris];
  tri->vertIdx[0] = (int)FindOrAddShadowVert(&triVerts[0]);
  tri->vertIdx[1] = (int)FindOrAddShadowVert(&triVerts[3]);
  tri->vertIdx[2] = (int)FindOrAddShadowVert(&triVerts[6]);
  g_numShadowTris++;

  return tri->vertIdx[2];
}

/*
================
ChooseSplitAxes

Chooses split axes sorted by AABB extent size.
================
*/
int ChooseSplitAxes(float *mins, float *maxs, int *splitAxis)
{
  int secondAxis;
  float size[3];

  /* compute AABB extent per axis */
  size[0] = maxs[0] - mins[0];
  size[1] = maxs[1] - mins[1];
  size[2] = maxs[2] - mins[2];

  if ( size[0] <= size[1] )
  {
    *splitAxis = 1;
    splitAxis[1] = 0;
  }
  else
  {
    *splitAxis = 0;
    splitAxis[1] = 1;
  }
  secondAxis = splitAxis[1];
  if ( size[2] <= (double)size[secondAxis] )
  {
    splitAxis[2] = 2;
  }
  else
  {
    splitAxis[2] = secondAxis;
    if ( size[2] <= size[*splitAxis] )
    {
      splitAxis[1] = 2;
    }
    else
    {
      splitAxis[1] = *splitAxis;
      *splitAxis = 2;
    }
  }

  if ( size[splitAxis[2]] != 0.0 )
    return 3;
  if ( size[splitAxis[1]] != 0.0 )
    return 2;
  Assert(size[*splitAxis] != 0.0, s_assertDisable_ChooseSplitAxes);
  return 1;
}

/*
================
PartitionShadowVerts

Recursively splits partition if > MAX_SHADOW_PARTITION_VERTS verts.
================
*/
void PartitionShadowVerts(int numPartitions, int vertCount, int partitionId, float *mins, float *maxs)
{
  int axisIdx;
  int splitAxis[3];
  int numAxes;

  if ( vertCount > MAX_SHADOW_PARTITION_VERTS )
  {
    axisIdx = 0;
    numAxes = ChooseSplitAxes(mins, maxs, splitAxis);
    if ( numAxes <= 0 )
    {
      Com_Error("Partitioning shadow verts failed\n");
    }
    else
    {
      while ( !SplitShadowPartition(numPartitions, vertCount, partitionId, splitAxis[axisIdx]) )
      {
        if ( ++axisIdx >= numAxes )
        {
          Com_Error("Partitioning shadow verts failed\n");
          break;
        }
      }
    }
  }
}

/*
================
SortAndRemapShadowVerts

Sorts verts by partition, remaps tri indices.
================
*/
void SortAndRemapShadowVerts(int numPartitions)
{
  ShadowPartition_t *partData, *part;
  int *vertSortOrder, *vertMap;
  int i, j, oldIdx;
  vec3_t globalMins, globalMaxs;
  BspShadowVert_t *vertCopy;

  /* allocate work buffers */
  g_shadowSortedMaxs = malloc(numPartitions * sizeof(ShadowPartition_t *));
  g_shadowSortedMins = malloc(numPartitions * sizeof(ShadowPartition_t *));
  partData = malloc(numPartitions * sizeof(ShadowPartition_t));
  g_shadowPartitionData = partData;

  /* initialize partitions */
  for ( i = 0; i < numPartitions; i++ )
  {
    partData[i].partitionId = 0;
    partData[i].vertCount = 0;
    ClearBounds(partData[i].mins, partData[i].maxs);
  }
  g_numShadowPartitions = 1;

  /* assign verts to partitions and build sort order */
  vertMap = malloc(numBSPShadowVerts * sizeof(int));
  vertSortOrder = malloc(numBSPShadowVerts * sizeof(int));

  for ( i = 0; i < (int)numBSPShadowVerts; i++ )
  {
    vertSortOrder[i] = i;
    part = &g_shadowPartitionData[g_shadowVertPartition[i]];
    part->partitionId = (int)g_numShadowPartitions;
    part->vertCount++;
    AddPointToBounds(bspShadowVerts[i].xyz, part->mins, part->maxs);
    if ( part->vertCount > MAX_SHADOW_PARTITION_VERTS )
      Com_Error("more than %i contiguous shadow-casting vertex positions", MAX_SHADOW_PARTITION_VERTS);
  }

  /* compute global bounds from all non-empty partitions */
  ClearBounds(globalMins, globalMaxs);
  for ( i = 0; i < numPartitions; i++ )
  {
    if ( partData[i].vertCount )
      AddBoundsToBounds(partData[i].mins, partData[i].maxs, globalMins, globalMaxs);
  }

  /* split large partitions and build clusters */
  PartitionShadowVerts(numPartitions, numBSPShadowVerts, (int)g_numShadowPartitions++, globalMins, globalMaxs);
  BuildShadowClusters(numPartitions);

  /* sort verts by partition and build remap table */
  qsort(vertSortOrder, numBSPShadowVerts, sizeof(int), (int (*)(const void*, const void*))ShadowTriPartitionCompare);
  for ( i = 0; i < (int)numBSPShadowVerts; i++ )
    vertMap[vertSortOrder[i]] = i;

  /* remap tri vertex indices */
  for ( i = 0; i < (int)g_numShadowTris; i++ )
  {
    ShadowTri_t *tri = &g_shadowTriPool[i];
    part = &g_shadowPartitionData[tri->partitionId];
    tri->clusterId = part->partitionId;
    tri->firstVert = part->firstVert;

    Assert(g_shadowVertPartition[tri->vertIdx[0]] == tri->partitionId, s_assertDisable_SortAndRemapShadowVerts);
    Assert(g_shadowVertPartition[tri->vertIdx[1]] == tri->partitionId, s_assertDisable_SortAndRemapShadowVerts);
    Assert(g_shadowVertPartition[tri->vertIdx[2]] == tri->partitionId, s_assertDisable_SortAndRemapShadowVerts);
    Assert(vertMap[tri->vertIdx[0]] >= tri->firstVert, s_assertDisable_SortAndRemapShadowVerts);
    Assert(vertMap[tri->vertIdx[1]] >= tri->firstVert, s_assertDisable_SortAndRemapShadowVerts);
    Assert(vertMap[tri->vertIdx[2]] >= tri->firstVert, s_assertDisable_SortAndRemapShadowVerts);

    for ( j = 0; j < 3; j++ )
    {
      oldIdx = tri->vertIdx[j];
      tri->vertIdx[j] = vertMap[oldIdx];
    }
  }

  /* reorder vertex data according to remap */
  vertCopy = malloc(numBSPShadowVerts * sizeof(BspShadowVert_t));
  memcpy(vertCopy, bspShadowVerts, numBSPShadowVerts * sizeof(BspShadowVert_t));
  for ( i = 0; i < (int)numBSPShadowVerts; i++ )
    memcpy(&bspShadowVerts[vertMap[i]], &vertCopy[i], sizeof(BspShadowVert_t));

  free(vertMap);
  free(vertSortOrder);
  free(vertCopy);
  free(g_shadowSortedMaxs);
  free(g_shadowSortedMins);
  free(g_shadowPartitionData);
}

/*
================
BuildShadowPartitions

Builds shadow vertex partitions from shadow tris.
================
*/
void BuildShadowPartitions(void)
{
  int *vertPartition;
  ShadowTri_t *tri;
  int i, j, assertResult;
  int vertParts[3], chosenPartition;
  int nextPartitionId, uniquePartitionCount, finalPartCount;

  if ( !numBSPShadowVerts )
    return;

  AssertFatal(!g_shadowVertPartition, s_assertDisable_BuildShadowPartitions);
  vertPartition = malloc(numBSPShadowVerts * sizeof(int));
  memset(vertPartition, 0, numBSPShadowVerts * sizeof(int));
  g_shadowVertPartition = vertPartition;

  /* seed first tri into partition 1 */
  tri = &g_shadowTriPool[0];
  tri->partitionId = 1;
  for ( j = 0; j < 3; j++ )
    vertPartition[tri->vertIdx[j]] = 1;

  nextPartitionId = 1;
  uniquePartitionCount = 1;

  /* assign remaining tris to partitions */
  for ( i = 1; i < (int)g_numShadowTris; i++ )
  {
    tri = &g_shadowTriPool[i];

    /* read vertex partition IDs */
    for ( j = 0; j < 3; j++ )
      vertParts[j] = vertPartition[tri->vertIdx[j]];

    if ( vertParts[0] == vertParts[1] && vertParts[1] == vertParts[2] )
    {
      /* all 3 verts in same partition (or all unassigned) */
      if ( vertParts[0] )
        chosenPartition = vertParts[0];
      else
      {
        /* new island — assign new partition */
        chosenPartition = ++nextPartitionId;
        uniquePartitionCount++;
      }
    }
    else
    {
      /* verts span multiple partitions — choose max and merge */
      chosenPartition = vertParts[0];
      if ( vertParts[1] > chosenPartition ) chosenPartition = vertParts[1];
      if ( vertParts[2] > chosenPartition ) chosenPartition = vertParts[2];

      Assert(chosenPartition, s_assertDisable_BuildShadowPartitions);

      /* merge connected partitions */
      for ( j = 0; j < 3; j++ )
        uniquePartitionCount = PartitionShadowTris_r(chosenPartition, vertPartition[tri->vertIdx[j]], i, uniquePartitionCount);
    }

    /* assign tri and its verts to the chosen partition */
    tri->partitionId = chosenPartition;
    for ( j = 0; j < 3; j++ )
      vertPartition[tri->vertIdx[j]] = chosenPartition;
  }

  finalPartCount = uniquePartitionCount;
  SortAndRemapShadowVerts(nextPartitionId + 1);
  free(g_shadowVertPartition);
  g_shadowVertPartition = NULL;

  AssertFatal(finalPartCount > 0, s_assertDisable_BuildShadowPartitions);
  if ( finalPartCount != 1 )
    qsort(g_shadowTriPool, g_numShadowTris, sizeof(ShadowTri_t), (int (*)(const void*, const void*))ShadowTriSortCompare);
}

/*
================
TestTriFacingLight

Emits shadow tri if it faces the light.
================
*/
void TestTriFacingLight(float *lightPlane, float *triVerts)
{
  float plane[4];

  if ( PlaneFromPoints(plane, triVerts, &triVerts[3], &triVerts[6]) )
  {
    plane[3] = -plane[3];
	
    /* test: dot(normal, lightNormal) + dist * lightDist <= 0 */
    if ( DotProduct(plane, lightPlane) + plane[3] * lightPlane[3] <= 0.0 )
      EmitShadowTriIndices(triVerts);
  }
}

/*
================
ClassifyShadowTriSoup

Classifies triSoup shadow category and processes tris.
================
*/
void ClassifyShadowTriSoup(int triSoupIndex, double unusedFpu, float *lightPlane)
{
  BspTriSoup_t *ts;
  ShadowAabb_t *aabb;
  BspDrawVert_t *vertBase;
  unsigned short *indices;
  int category, i;
  float triVerts[9];

  ts = &bspTriangles[triSoupIndex];
  category = GetMaterialShadowCategory(ts->materialIndex, unusedFpu);

  if ( category == 3 )
    return;

  if ( category == 2 )
  {
    /* unique shadow — create leaf AABB */
    aabb = AllocShadowAABB();
    AssertFatal(aabb, s_assertDisable_ClassifyShadowTriSoup);
    aabb->triSoupIndex = triSoupIndex;
    aabb->nodeType = 2;
    aabb->materialPartition = 0;
    ComputeShadowAABBBounds(ts, aabb->mins, aabb->maxs);
    ShadowAABB_Insert(NULL, aabb);
    return;
  }

  /* shared shadow — emit tris facing the light */
  AssertFatal(category == 1, s_assertDisable_ClassifyShadowTriSoup);
  vertBase = &bspTriSoupData[ts->firstVertex];
  indices = &bspDrawIndexes[ts->firstIndex];

  for ( i = 0; i < ts->indexCount; i += 3 )
  {
    VectorCopy(vertBase[indices[i]].pos, &triVerts[0]);
    VectorCopy(vertBase[indices[i + 1]].pos, &triVerts[3]);
    VectorCopy(vertBase[indices[i + 2]].pos, &triVerts[6]);
    TestTriFacingLight(lightPlane, triVerts);
  }
}

/*
================
CollectShadowTris

Collects shadow tris from all triSoups and extra tri list.
================
*/
void CollectShadowTris(float *lightPlane, double unusedFpu)
{
  int triSoupIdx;
  ExtraShadowTri_t *extra;
  float plane[4];

  /* classify all triSoups */
  for ( triSoupIdx = 0; triSoupIdx < numBSPTriSoups; triSoupIdx++ )
    ClassifyShadowTriSoup(triSoupIdx, unusedFpu, lightPlane);

  /* process extra shadow tris from linked list */
  for ( extra = g_shadowTriFreeList; extra; extra = extra->next )
  {
    if ( PlaneFromPoints(plane, extra->v0, extra->v1, extra->v2) )
    {
      if ( DotProduct(plane, lightPlane) - plane[3] * lightPlane[3] <= 0.0 )
        EmitShadowTriIndices(extra->v0);
    }
  }
}

/*
================
BuildShadowCluster

Builds a complete shadow cluster for a light.
================
*/
int BuildShadowCluster(BspLight_t *light, double unusedFpu)
{
  int baseAabbIndex, treeNodeCount, clusterIndex;
  DiskShadowAabb_t *diskAabb;
  float lightPlane[4];

  /* copy light record into global array */
  bspLights[numBSPLights] = *light;
  numBSPLights++;

  /* extract light direction and plane W based on light type */
  if ( light->lightType == 1 )
  {
    VectorCopy(light->origin, lightPlane);
    lightPlane[3] = 0.0f;
  }
  else
  {
    VectorCopy(light->dir, lightPlane);
    lightPlane[3] = 1.0f;
  }

  /* collect, partition, and build AABB trees for shadow tris */
  CollectShadowTris(lightPlane, unusedFpu);
  printf("building AABBs for shadow tris...\n");
  BuildShadowPartitions();
  BuildShadowAabbTrees();
  NestShadowAabbs();

  /* emit AABB tree and shared shadow indices */
  baseAabbIndex = numBSPShadowAabbTrees;
  diskAabb = &bspShadowData[numBSPShadowAabbTrees];
  treeNodeCount = (unsigned short)EmitShadowAabbTree_r(g_shadowAabbFreeList);
  EmitSharedShadowIndices_Siblings(diskAabb, g_shadowAabbFreeList);

  /* register shadow source */
  if ( numBSPShadowSources == MAX_MAP_SHADOW_SOURCES )
    Com_Error("MAX_MAP_SHADOW_CLUSTERS (%i) exceeded\n", MAX_MAP_SHADOW_SOURCES);

  clusterIndex = numBSPShadowSources++;
  bspShadowSources[clusterIndex].firstAabbIndex = baseAabbIndex;
  bspShadowSources[clusterIndex].aabbCount = treeNodeCount;
  return clusterIndex;
}

/*
================
ChopConcaveCasterWindings

Chops concave caster windings by surface planes.
================
*/
int ChopConcaveCasterWindings(void)
{
  TriSurf_t *ts;
  TriSurfProps_t *p;
  int i;

  for ( ts = g_smCasterWindingList; ts; ts = ts->next )
  {
    p = ts->props;

    /* snap main winding to surface plane */
    ChopWinding(ts->winding, p->plane, p->plane[3]);

    /* snap each hole winding to the same plane */
    for ( i = 0; i < ts->holeCount; i++ )
      ChopWinding(ts->holes[i].winding, p->plane, p->plane[3]);
  }

  return 0;
}

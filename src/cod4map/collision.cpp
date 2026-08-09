/*
collision.c — Collision mesh generation

Reconstructed from cod2map.exe by Rose.

Builds collision data from curve/terrain surfaces: allocates collision
triangles, partitions into spatial groups, builds AABB tree, computes
barycentric planes, then emits verts/edges/tris/borders/partitions
to BSP collision lumps.
*/

#include "cod4map.h"

int collisionNodeLimit = INT_MAX;
static char s_assertDisable_SetCollisionNodeLimitFromWorldspawn;
static char s_assertDisable_CM_BuildCollisionAabbTree;

void SetCollisionNodeLimitFromWorldspawn(Entity_t *entity)
{
  const char *value;

  value = ValueForKey(entity, "coll_node_limit");
  Assert(value != NULL, s_assertDisable_SetCollisionNodeLimitFromWorldspawn);
  if ( *value && *value != '0' )
    collisionNodeLimit = atoi(value);
}

CmPartition_t   *g_cmPartitionPool;
CmVert_t        *g_cmVertPool;
CollisionEdge_t *g_cmEdgeHashTable[PLANE_HASH_SIZE];
CollisionEdge_t *g_cmEdgePool;
CollisionTri_t  *g_cmTriPool;

int   g_cmEdgeReuses;
int   g_cmEdgeStartIdx;
int   g_cmMaxEdges;
float g_cmMaxPartExtentX;
float g_cmMaxPartExtentY;
float g_cmMaxPartExtentZ;
int   g_cmMaxPartitions;
int   g_cmMaxTris;
int   g_cmMaxVerts;
int   g_cmMinTrisPerLeaf;
int   g_cmNumConvexEdges;
int   g_cmNumEdges;
int   g_cmNumPartitions;
int   g_cmNumTris;
int   g_cmNumVerts;
float g_cmOverlapRatio;
int   g_cmPartStartIdx;
int   g_cmTriStartIdx;
int   g_cmVertStartIdx;
char  g_collisionByte0;

char s_assertDisable_CM_AssignTriToPartitionSide;
char s_assertDisable_CM_AssignTriToPartitionSide;
char s_assertDisable_CM_AssignTriToPartitionSide;
char s_assertDisable_CM_AssignTriToPartitionSide;
char s_assertDisable_CM_AssignTriToPartitionSide;
char s_assertDisable_CM_AssignTriToPartitionSide;
char s_assertDisable_CM_BuildLeafCollision;
char s_assertDisable_CM_BuildLeafCollision;
char s_assertDisable_CM_BuildNodeCollision_r;
char s_assertDisable_CM_CompareTrisByMaterial;
char s_assertDisable_CM_CompareTrisByMaterial;
char s_assertDisable_CM_CompareTrisByMaterial;
char s_assertDisable_CM_CompareTrisByMaterial;
char s_assertDisable_CM_ComputeBarycentricPlane;
char s_assertDisable_CM_EmitCollisionPartitions;
char s_assertDisable_CM_FilterWindingIntoTree;
char s_assertDisable_CM_FilterWindingIntoTree;
char s_assertDisable_CM_FilterWindingIntoTree;
char s_assertDisable_CM_PartitionTris;
char s_assertDisable_CM_PartitionTris;
char s_assertDisable_CM_PartitionTris;
char s_assertDisable_CM_PartitionTris;
char s_assertDisable_CM_PartitionTris;
char s_assertDisable_CM_PartitionTris;
char s_assertDisable_CM_PartitionTris;
char s_assertDisable_CM_SortTrisByPartition;
char s_assertDisable_CM_SortTrisByPartition;
char s_assertDisable_CM_SubdivideTriArray;
char s_assertDisable_CM_SubdivideTriArray;
char s_assertDisable_CM_SubdivideTriArray;
char s_assertDisable_CM_SubdivideTriArray;
char s_assertDisable_CM_SubdivideTriArray;
char s_assertDisable_CollisionAddEdge;
char s_assertDisable_CollisionAddSurfaces;
char s_assertDisable_CollisionAddSurfaces;
char s_assertDisable_CollisionAddTriangle;
char s_assertDisable_CollisionAddTriangle;
char s_assertDisable_CollisionAddTriangle;


/*
================
CM_AllocCollisionTri

Allocates a new collision triangle from the global pool, returns pointer to tri struct
================
*/
CollisionTri_t *CM_AllocCollisionTri()
{
  int triIdx;

  triIdx = g_cmNumTris;
  if ( g_cmNumTris == g_cmMaxTris )
  {
    Com_Error("max collision triangles (%i) exceeded\n", g_cmMaxTris);
    triIdx = g_cmNumTris;
  }
  g_cmNumTris = triIdx + 1;
  return &g_cmTriPool[triIdx];
}

/*
================
CM_PartitionTris

Flood-fill assigns partition IDs to connected triangles via shared vertices
================
*/
int CM_PartitionTris()
{
  CollisionTri_t *tri;
  CmVert_t *v[3];
  int nextUnvisited;
  int partitionId;
  int scanIdx;
  int changed;
  int j, k;

  nextUnvisited = g_cmTriStartIdx;
  partitionId = g_cmNumPartitions + 1;

  /* assign partitions to connected triangle groups */
  while ( 1 )
  {
    /* seed: assign partition to first unvisited triangle's verts */
    tri = &g_cmTriPool[nextUnvisited];
    v[0] = &g_cmVertPool[tri->vertIdx[0]];
    v[1] = &g_cmVertPool[tri->vertIdx[1]];
    v[2] = &g_cmVertPool[tri->vertIdx[2]];
    Assert(!v[0]->partitionId, s_assertDisable_CM_PartitionTris);
    Assert(!v[1]->partitionId, s_assertDisable_CM_PartitionTris);
    Assert(!v[2]->partitionId, s_assertDisable_CM_PartitionTris);
    v[0]->partitionId = partitionId;
    v[1]->partitionId = partitionId;
    v[2]->partitionId = partitionId;

    /* flood-fill: propagate partition to connected tris */
    while ( 1 )
    {
      Assert(g_cmTriPool[nextUnvisited].partitionId == 0, s_assertDisable_CM_PartitionTris);

      if ( nextUnvisited >= g_cmNumTris )
      {
        g_cmNumPartitions = partitionId;
        return g_cmNumTris;
      }

      changed = 0;
      scanIdx = nextUnvisited;
      nextUnvisited = -1;

      /* scan all tris from current position */
      for ( k = scanIdx; k < g_cmNumTris; k++ )
      {
        tri = &g_cmTriPool[k];
        if ( tri->partitionId )
          continue;

        /* get this tri's verts */
        v[0] = &g_cmVertPool[tri->vertIdx[0]];
        v[1] = &g_cmVertPool[tri->vertIdx[1]];
        v[2] = &g_cmVertPool[tri->vertIdx[2]];

        /* assert vert partition consistency */
        Assert(v[0]->partitionId == 0 || v[0]->partitionId == partitionId, s_assertDisable_CM_PartitionTris);
        Assert(v[1]->partitionId == 0 || v[1]->partitionId == partitionId, s_assertDisable_CM_PartitionTris);
        Assert(v[2]->partitionId == 0 || v[2]->partitionId == partitionId, s_assertDisable_CM_PartitionTris);

        /* if any vert belongs to this partition, adopt the whole tri */
        if ( v[0]->partitionId || v[1]->partitionId || v[2]->partitionId )
        {
          for ( j = 0; j < 3; j++ )
          {
            if ( !v[j]->partitionId )
            {
              v[j]->partitionId = partitionId;
              changed = 1;
            }
          }
          tri->partitionId = partitionId;
        }
        else
        {
          /* track first unvisited tri for next partition */
          if ( nextUnvisited < 0 )
            nextUnvisited = k;
        }
      }

      if ( nextUnvisited < 0 )
      {
        g_cmNumPartitions = partitionId;
        /* Native 0x410100 leaves the scan index at the terminal triangle
           count when the final connected component is complete. */
        return g_cmNumTris;
      }
      if ( !changed )
      {
        partitionId++;
        break;
      }
    }
  }
}

/*
================
CM_CompareTrisByMaterial

qsort comparator: sorts collision tris by material index, then by texture sort key
================
*/
int CM_CompareTrisByMaterial(const void *va, const void *vb)
{
  const CollisionTri_t *a = va;
  const CollisionTri_t *b = vb;
  DrawSurf_t *dsA, *dsB;
  int diff;

  Assert(a->drawSurf, s_assertDisable_CM_CompareTrisByMaterial);
  Assert(b->drawSurf, s_assertDisable_CM_CompareTrisByMaterial);

  dsA = a->drawSurf;
  dsB = b->drawSurf;

  /* compare by surface flags first */
  diff = dsA->surfaceFlags - dsB->surfaceFlags;
  if ( diff )
    return diff;

  /* then by shader surface flags */
  Assert(dsA->shaderInfo, s_assertDisable_CM_CompareTrisByMaterial);
  Assert(dsB->shaderInfo, s_assertDisable_CM_CompareTrisByMaterial);
  return dsA->shaderInfo->surfaceFlags - dsB->shaderInfo->surfaceFlags;
}

/*
================
CM_SortTrisByPartition

Sorts collision tris by material, then assigns partition IDs to groups of same-material tris
================
*/
int CM_SortTrisByPartition(CollisionTri_t *tris, int triCount)
{
  int partitionId;
  int i;

  Assert(tris, s_assertDisable_CM_SortTrisByPartition);
  Assert(triCount, s_assertDisable_CM_SortTrisByPartition);

  /* sort by material */
  qsort(tris, triCount, sizeof(CollisionTri_t), CM_CompareTrisByMaterial);

  /* assign partition IDs to groups of same-material tris */
  partitionId = tris[0].partitionId;
  for ( i = 0; i < triCount - 1; i++ )
  {
    /* new partition when material changes */
    if ( tris[i].drawSurf->surfaceFlags != tris[i + 1].drawSurf->surfaceFlags ||
         tris[i].drawSurf->shaderInfo->surfaceFlags != tris[i + 1].drawSurf->shaderInfo->surfaceFlags )
      partitionId = ++g_cmNumPartitions;
    tris[i + 1].partitionId = partitionId;
  }
  return 0;
}

/*
================
CM_AssignTriToPartitionSide

Assigns a tri to one side (0=low, 1=high) of a partition split, swapping it into position
================
*/
int CM_AssignTriToPartitionSide(CollisionTri_t *tri, int side, CmPartition_t *partition, CollisionTri_t *triArray, unsigned int axis)
{
  CollisionTri_t *swapTri;
  CollisionTri_t temp;

  Assert(tri, s_assertDisable_CM_AssignTriToPartitionSide);
  Assert(triArray, s_assertDisable_CM_AssignTriToPartitionSide);
  Assert(!(axis > 2), s_assertDisable_CM_AssignTriToPartitionSide);
  Assert(partition, s_assertDisable_CM_AssignTriToPartitionSide);
  Assert(!(partition->triCount <= 0), s_assertDisable_CM_AssignTriToPartitionSide);

  /* update side bounds */
  if ( partition->sideMin[side] > (double)tri->mins[axis] )
    partition->sideMin[side] = tri->mins[axis];
  if ( partition->sideMax[side] < (double)tri->maxs[axis] )
    partition->sideMax[side] = tri->maxs[axis];

  Assert(!(side >= 2), s_assertDisable_CM_AssignTriToPartitionSide);

  /* swap tri into position */
  swapTri = &triArray[partition->sideCount[0] + side * (partition->triCount - 1)];
  if ( swapTri != tri )
  {
    memcpy(&temp, swapTri, sizeof(CollisionTri_t));
    memcpy(swapTri, tri, sizeof(CollisionTri_t));
    memcpy(tri, &temp, sizeof(CollisionTri_t));
  }

  partition->triCount--;
  partition->sideCount[side]++;
  return side;
}

/*
================
CM_SubdivideTriArray

Subdivides a collision triangle array along the given axis, splitting into two groups
================
*/
int CM_SubdivideTriArray(CollisionTri_t *triArray, int triCount, unsigned int axis, int failureMode)
{
  struct { CmPartition_t part; int skipCount; } _pb;
  #define part _pb.part
  int remaining, i;

  part.sideMin[0] = part.sideMin[1] = MAX_WORLD_COORD;
  part.sideMax[0] = part.sideMax[1] = MIN_WORLD_COORD;
  part.sideCount[0] = part.sideCount[1] = 0;
  part.triCount = triCount;
  remaining = triCount;

  if ( !triCount )
    goto subdivision_complete;

  /* alternating passes: assign tris with lowest max to side 0, highest min to side 1 */
  while ( 1 )
  {
    /* side 0 pass: find tri with lowest maxs[axis], assign it and all tris at same max */
    if ( part.sideCount[0] <= part.sideCount[1] && (double)part.sideMax[1] - part.sideMin[1] >= (double)part.sideMax[0] - part.sideMin[0] )
    {
      CollisionTri_t *best = &triArray[part.sideCount[0]];
      for ( i = 1; i < remaining; i++ )
        if ( best->maxs[axis] > (double)triArray[part.sideCount[0] + i].maxs[axis] )
          best = &triArray[part.sideCount[0] + i];
      CM_AssignTriToPartitionSide(best, 0, &part, triArray, axis);

      /* also assign any remaining tris with the same max value */
      _pb.skipCount = 0;
      { CollisionTri_t *scan = &triArray[part.sideCount[0]];
      while ( _pb.skipCount < part.triCount )
      {
        Assert(part.sideMax[0] <= scan->maxs[axis] || s_assertDisable_CM_SubdivideTriArray, s_assertDisable_CM_SubdivideTriArray);
        if ( part.sideMax[0] == scan->maxs[axis] )
          CM_AssignTriToPartitionSide(scan, 0, &part, triArray, axis);
        else
          _pb.skipCount++;
        scan++;
        Assert(scan == &triArray[part.sideCount[0] + _pb.skipCount] || s_assertDisable_CM_SubdivideTriArray, s_assertDisable_CM_SubdivideTriArray);
      } }
    }
    else
    {
      /* side 1 pass: find tri with highest mins[axis], assign it and all tris at same min */
      CollisionTri_t *best = &triArray[part.sideCount[0]];
      for ( i = 1; i < remaining; i++ )
        if ( best->mins[axis] < (double)triArray[part.sideCount[0] + i].mins[axis] )
          best = &triArray[part.sideCount[0] + i];
      CM_AssignTriToPartitionSide(best, 1, &part, triArray, axis);

      _pb.skipCount = 0;
      { CollisionTri_t *scan = &triArray[part.triCount + part.sideCount[0] - 1];
      while ( _pb.skipCount < part.triCount )
      {
        Assert(part.sideMin[1] >= scan->mins[axis] || s_assertDisable_CM_SubdivideTriArray, s_assertDisable_CM_SubdivideTriArray);
        if ( part.sideMin[1] == scan->mins[axis] )
          CM_AssignTriToPartitionSide(scan, 1, &part, triArray, axis);
        else
          _pb.skipCount++;
        scan--;
        Assert(scan == &triArray[part.triCount - _pb.skipCount + part.sideCount[0] - 1] || s_assertDisable_CM_SubdivideTriArray, s_assertDisable_CM_SubdivideTriArray);
      } }
    }

    if ( !part.triCount )
      goto subdivision_complete;
    remaining = part.triCount;
  }

subdivision_complete:
  { int side0 = part.sideCount[0], side1 = part.sideCount[1];

  if ( failureMode == 1 )
  {
    if ( !side0 || !side1 )
    {
      part.sideCount[0] = triCount / 2;
      side0 = triCount / 2;
    }
  }
  else
  {
    Assert(!failureMode || s_assertDisable_CM_SubdivideTriArray, s_assertDisable_CM_SubdivideTriArray);
    if ( !side0 || !side1 )
      return 0;
    { double gap = (double)part.sideMax[0] - part.sideMin[1];
#ifdef _WIN64
    if ( gap > 0.0 && gap > (double)(part.sideMax[1] - part.sideMin[0]) * g_cmOverlapRatio )
#else
    if ( gap > 0.0 && gap > (part.sideMax[1] - part.sideMin[0]) * g_cmOverlapRatio )
#endif
      return 0;
    }
  }

  g_cmNumPartitions++;
  for ( i = 0; i < side0; i++ )
    triArray[i].partitionId = g_cmNumPartitions;
  return side0;
  }
  #undef part
}

/*
================
CM_GetAxisSortOrder

Determines axis sort order (largest extent first) for partition subdivision
================
*/
void CM_GetAxisSortOrder(int *order, float *extents)
{
  int temp;

  /* start with identity order */
  order[0] = 0;
  order[1] = 1;
  order[2] = 2;

  /* sort by descending extent — largest axis first */
  if ( extents[1] > extents[0] )
  {
    order[0] = 1;
    order[1] = 0;
  }
  if ( extents[2] > extents[order[0]] )
  {
    temp = order[1];
    order[2] = temp;
    order[1] = order[0];
    order[0] = 2;
  }
  else if ( extents[2] > extents[order[1]] )
  {
    order[2] = order[1];
    order[1] = 2;
  }
}

/*
================
CM_ForEachPartitionGroup

Iterates over groups of tris with the same partition ID, calling callback(triArray, triCount) for each group
================
*/
int CM_ForEachPartitionGroup(int (*callback)(CollisionTri_t *, int))
{
  int start, end;
  int partId;
  int result = 0;

  /* walk tris in partition-ID groups */
  for ( start = g_cmTriStartIdx; start < g_cmNumTris; start = end )
  {
    /* find end of this partition group */
    partId = g_cmTriPool[start].partitionId;
    for ( end = start + 1; end < g_cmNumTris; end++ )
    {
      if ( g_cmTriPool[end].partitionId != partId )
        break;
    }

    result = callback(&g_cmTriPool[start], end - start);
  }
  return result;
}

/*
================
CM_ComparePartitionId

qsort comparator: sorts collision tris by partition ID
================
*/
int CM_ComparePartitionId(const void *va, const void *vb)
{
  const CollisionTri_t *a = va;
  const CollisionTri_t *b = vb;
  return a->partitionId - b->partitionId;
}

/*
================
CM_FilterWindingIntoTree

Clips a winding against the BSP tree, distributing partition IDs to leaf nodes
================
*/
int CM_FilterWindingIntoTree(Winding_t *w, int partitionId, Node_t *node)
{
  DrawSurfRef_t *ref;
  DrawSurfRef_t *newRef;
  Winding_t *front, *back;
  int count;

  Assert(w, s_assertDisable_CM_FilterWindingIntoTree);

  /* leaf node — add partition reference */
  if ( node->planenum == -1 )
  {
    FreeWinding(w);
    if ( node->opaque )
      return 0;

    /* check if partition already in this leaf's list */
    for ( ref = node->drawSurfRefs; ref; ref = ref->next )
    {
      if ( ref->partId == partitionId )
        return 0;
    }

    /* add new reference */
    newRef = malloc(sizeof(*newRef));
    newRef->partId = partitionId;
    newRef->next = node->drawSurfRefs;
    node->drawSurfRefs = newRef;
    return 1;
  }

  /* internal node — clip and recurse */
  ClipWindingEpsilon(w, MAP_PLANE(node->planenum)->normal, MAP_PLANE(node->planenum)->dist, ON_EPSILON, &front, &back, 1);

  Assert(!(front == w), s_assertDisable_CM_FilterWindingIntoTree);
  Assert(!(back == w), s_assertDisable_CM_FilterWindingIntoTree);

  /* free original winding — front/back are new allocations from ClipWindingEpsilon */
  FreeWinding(w);
  count = 0;
  if ( front )
    count = CM_FilterWindingIntoTree(front, partitionId, node->children[0]);
  if ( back )
    count += CM_FilterWindingIntoTree(back, partitionId, node->children[1]);
  return count;
}

/*
================
VectorCompareExact

Returns 1 if two vectors are exactly equal.
================
*/
int VectorCompareExact(float *a, float *b)
{
  return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

/*
================
CM_BuildTriWindings

Builds windings from collision tri vertices and filters them into the BSP tree
================
*/
int CM_BuildTriWindings(Node_t *tree)
{
  CmPartition_t *part;
  CollisionTri_t *tri;
  Winding_t *w;
  CmVert_t *v;
  int i, j, k;
  int result;

  result = g_cmPartStartIdx;

  /* build a winding for each tri and filter into BSP tree */
  for ( i = g_cmPartStartIdx; i < g_cmNumPartitions; i++ )
  {
    part = &g_cmPartitionPool[i];

    for ( j = 0; j < part->triCount; j++ )
    {
      result = j;
      tri = &part->triArray[j];
      w = AllocWinding(3);
      w->numpoints = 3;

      /* copy vertex positions into winding */
      for ( k = 0; k < 3; k++ )
      {
        v = &g_cmVertPool[tri->vertIdx[k]];
        VectorCopy(v->xyz, w->points[k]);
      }

      CM_FilterWindingIntoTree(w, i, tree);
    }
    result = j;
  }
  return result;
}

/*
================
CM_ComparePartitions

qsort comparator: sorts partitions by material index, then by texture sort key
================
*/
int CM_ComparePartitions(const void *elem1, const void *elem2)
{
  CmPartition_t *a = *(CmPartition_t **)elem1;
  CmPartition_t *b = *(CmPartition_t **)elem2;
  DrawSurf_t *dsA = a->triArray->drawSurf;
  DrawSurf_t *dsB = b->triArray->drawSurf;
  int diff;

  diff = dsA->surfaceFlags - dsB->surfaceFlags;
  if ( !diff )
    return dsA->shaderInfo->surfaceFlags - dsB->shaderInfo->surfaceFlags;
  return diff;
}

/*
================
CM_CopyVec4

Copies a 4-component vector (e.g. plane equation) from src to dst
================
*/
void CM_CopyVec4(float *src, float *dst)
{
  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
  dst[3] = src[3];
}

static CmCollideBox_t *CM_AllocCollisionAabbLeaf(CmPartition_t *partition, CmCollideBox_t *next)
{
  CmCollideBox_t *aabb;

  aabb = (CmCollideBox_t *)malloc(sizeof(*aabb));
  memcpy(aabb->mins, partition, sizeof(aabb->mins));
  memcpy(aabb->maxs, (char *)partition + sizeof(aabb->mins), sizeof(aabb->maxs));
  aabb->partition = partition;
  aabb->child = NULL;
  aabb->next = next;
  return aabb;
}

static void CM_PrependCollisionAabb(CmCollideBox_t **list, int *listCount, CmCollideBox_t *aabb)
{
  Assert(aabb != NULL, s_assertDisable_CM_BuildCollisionAabbTree);
  Assert(aabb->next == NULL, s_assertDisable_CM_BuildCollisionAabbTree);
  aabb->next = *list;
  *list = aabb;
  ++*listCount;
}

static void CM_AppendCollisionAabbList(CmCollideBox_t *list, CmCollideBox_t *tail)
{
  CmCollideBox_t *last;

  Assert(list != NULL, s_assertDisable_CM_BuildCollisionAabbTree);
  for ( last = list; last->next; last = last->next )
    ;
  last->next = tail;
}

static CmCollideBox_t *CM_WrapCollisionAabbList(CmCollideBox_t *list)
{
  CmCollideBox_t *aabb;
  CmCollideBox_t *item;

  aabb = (CmCollideBox_t *)malloc(sizeof(*aabb));
  aabb->partition = NULL;
  aabb->child = list;
  aabb->next = NULL;
  ClearBounds(aabb->mins, aabb->maxs);
  for ( item = list; item; item = item->next )
    AddBoundsToBounds(item->mins, item->maxs, aabb->mins, aabb->maxs);
  return aabb;
}

static float CM_CollisionAabbMeanCenter(CmCollideBox_t *list, int listCount, int axis)
{
  CmCollideBox_t *aabb;
  float sum;
  int localCount;

  Assert(listCount > 0, s_assertDisable_CM_BuildCollisionAabbTree);
  sum = 0.0f;
  localCount = 0;
  for ( aabb = list; aabb; aabb = aabb->next )
  {
    sum += (aabb->mins[axis] + aabb->maxs[axis]) * 0.5f;
    ++localCount;
  }
  Assert(localCount == listCount, s_assertDisable_CM_BuildCollisionAabbTree);
  return sum / listCount;
}

static int CM_CollisionAabbSplitScore(CmCollideBox_t *list, int listCount, int axis, float split)
{
  CmCollideBox_t *aabb;
  int frontCount;
  int backCount;

  Assert(listCount > 0, s_assertDisable_CM_BuildCollisionAabbTree);
  frontCount = 0;
  backCount = 0;
  for ( aabb = list; aabb; aabb = aabb->next )
  {
    if ( split < aabb->mins[axis] )
      ++frontCount;
    else if ( split > aabb->maxs[axis] )
      ++backCount;
  }
  return frontCount < backCount ? frontCount : backCount;
}

static int CM_EvaluateCollisionAabbSplitAxis(
    CmCollideBox_t *list, int listCount, unsigned int axis, float *split)
{
  Assert(axis < 3, s_assertDisable_CM_BuildCollisionAabbTree);
  *split = CM_CollisionAabbMeanCenter(list, listCount, axis);
  return CM_CollisionAabbSplitScore(list, listCount, axis, *split);
}

static qboolean CM_FindBestCollisionAabbSplit(
    CmCollideBox_t *list, int listCount, int *splitAxis, float *split)
{
  float axisSplit;
  int bestScore;
  int score;
  int axis;

  if ( listCount < collisionNodeLimit )
    return qfalse;

  bestScore = INT_MIN;
  for ( axis = 0; axis < 3; ++axis )
  {
    score = CM_EvaluateCollisionAabbSplitAxis(list, listCount, axis, &axisSplit);
    if ( score > bestScore )
    {
      bestScore = score;
      *splitAxis = axis;
      *split = axisSplit;
    }
  }
  if ( bestScore <= 0 )
    return qfalse;
  Assert((unsigned int)*splitAxis < 3, s_assertDisable_CM_BuildCollisionAabbTree);
  return qtrue;
}

static void CM_SplitCollisionAabbList(
    CmCollideBox_t **list, int *listCount,
    CmCollideBox_t **otherList, int *otherCount,
    int axis, float split)
{
  CmCollideBox_t *remainingList;
  CmCollideBox_t *aabb;
  CmCollideBox_t *next;
  int remainingCount;
  int originalCount;

  Assert(*listCount > 0, s_assertDisable_CM_BuildCollisionAabbTree);
  originalCount = *listCount;
  remainingList = NULL;
  remainingCount = 0;
  *otherList = NULL;
  *otherCount = 0;

  for ( aabb = *list; aabb; aabb = next )
  {
    Assert(aabb->child == NULL, s_assertDisable_CM_BuildCollisionAabbTree);
    next = aabb->next;
    aabb->next = NULL;
    if ( split <= (aabb->mins[axis] + aabb->maxs[axis]) * 0.5f )
      CM_PrependCollisionAabb(otherList, otherCount, aabb);
    else
      CM_PrependCollisionAabb(&remainingList, &remainingCount, aabb);
  }

  Assert(originalCount == *otherCount + remainingCount, s_assertDisable_CM_BuildCollisionAabbTree);
  *list = remainingList;
  *listCount = remainingCount;
}

static CmCollideBox_t *CM_BuildCollisionAabbTree(CmCollideBox_t *list, int listCount)
{
  CmCollideBox_t *otherList;
  int otherCount;
  int splitAxis;
  float split;

  if ( listCount == 1 )
    return list;

  if ( CM_FindBestCollisionAabbSplit(list, listCount, &splitAxis, &split) )
  {
    otherList = NULL;
    otherCount = 0;
    CM_SplitCollisionAabbList(
        &list, &listCount, &otherList, &otherCount, splitAxis, split);
    Assert(listCount > 0, s_assertDisable_CM_BuildCollisionAabbTree);
    Assert(otherCount > 0, s_assertDisable_CM_BuildCollisionAabbTree);

    list = CM_BuildCollisionAabbTree(list, listCount);
    otherList = CM_BuildCollisionAabbTree(otherList, otherCount);
    if ( listCount + otherCount >= collisionNodeLimit )
    {
      list = CM_WrapCollisionAabbList(list);
      otherList = CM_WrapCollisionAabbList(otherList);
    }
    else
    {
      listCount += otherCount;
    }
    CM_AppendCollisionAabbList(list, otherList);
  }
  return CM_WrapCollisionAabbList(list);
}

/*
================
CM_BuildLeafCollision

Builds collision box hierarchy for BSP leaf — groups partitions by material, links into leaf->collideBoxList
================
*/
void CM_BuildLeafCollision(Node_t *leaf)
{
  DrawSurfRef_t *ref;
  CmPartition_t *part;
  CmCollideBox_t *box, *head;
  int numParts, start, end;
  CmPartition_t *partPtrs[MAX_MAP_COLLISION_PARTS];

  Assert(!(leaf->planenum != PLANENUM_LEAF), s_assertDisable_CM_BuildLeafCollision);

  if ( !leaf->drawSurfRefs )
    return;

  /* collect partition pointers from draw surface refs */
  numParts = 0;
  for ( ref = leaf->drawSurfRefs; ref; ref = ref->next )
    partPtrs[numParts++] = &g_cmPartitionPool[ref->partId];

  /* sort by material */
  qsort(partPtrs, numParts, sizeof(*partPtrs), CM_ComparePartitions);

  /* group partitions by material and build collision boxes */
  for ( start = 0; start < numParts; )
  {
    /* create box for first partition in group — copy split bounds */
    part = partPtrs[start];
    box = CM_AllocCollisionAabbLeaf(part, NULL);
    head = box;

    /* find end of same-material group */
    for ( end = start + 1; end < numParts; end++ )
    {
      DrawSurf_t *dsA = partPtrs[end]->triArray->drawSurf;
      DrawSurf_t *dsB = part->triArray->drawSurf;
      if ( dsA->surfaceFlags != dsB->surfaceFlags ||
           dsA->shaderInfo->surfaceFlags != dsB->shaderInfo->surfaceFlags )
        break;

      /* add box for each additional partition in group */
      box = CM_AllocCollisionAabbLeaf(partPtrs[end], head);
      head = box;
    }

    Assert(!(leaf->collisionAABBs && !start), s_assertDisable_CM_BuildLeafCollision);

    head = CM_BuildCollisionAabbTree(head, end - start);

    /* link into leaf's collision list */
    head->next = leaf->collisionAABBs;
    leaf->collisionAABBs = head;
    start = end;
  }
}

/*
================
CM_BuildNodeCollision_r

Recursively walks the BSP tree, building collision data at each leaf
================
*/
void CM_BuildNodeCollision_r(Node_t *node)
{
  Assert(node, s_assertDisable_CM_BuildNodeCollision_r);

  /* recurse until leaf */
  while ( node->planenum != PLANENUM_LEAF )
  {
    CM_BuildNodeCollision_r(node->children[0]);
    node = node->children[1];
    Assert(node, s_assertDisable_CM_BuildNodeCollision_r);
  }

  CM_BuildLeafCollision(node);
}

/*
================
CM_EmitCollisionVerts

Emits collision vertices to the BSP file format
================
*/
int CM_EmitCollisionVerts()
{
  CmVert_t *cv;
  int i;

  for ( i = g_cmVertStartIdx; i < g_cmNumVerts; i++ )
  {
    cv = &g_cmVertPool[i];

    if ( numBSPCollisionVerts == MAX_MAP_COLLISION_VERTS )
      Com_Error("MAX_MAP_COLLISIONVERTS (%i) exceeded", MAX_MAP_COLLISION_VERTS);

    /* CoD4 writes every position directly; triangle indices remain the
       compact pool indices, not a remapped emitted-index stream. */
    VectorCopy(cv->xyz, bspCollisionVerts[numBSPCollisionVerts].xyz);
    numBSPCollisionVerts++;
  }
  return g_cmNumVerts;
}

/*
================
CM_EmitCollisionEdges

Emits collision edges to the BSP file format
================
*/
int CM_EmitCollisionEdges()
{
  CollisionEdge_t *edge;
  BspCollisionEdge_t *emit;
  int i;

  for ( i = g_cmEdgeStartIdx; i < g_cmNumEdges; i++ )
  {
    edge = &g_cmEdgePool[i];

    /* skip reverse-matched edges */
    if ( edge->reverseMatch )
    {
      edge->reserved84 = -1;
      continue;
    }

    if ( numBSPCollisionEdges == MAX_MAP_COLLISION_EDGES )
      Com_Error("MAX_MAP_COLLISIONEDGES (%i) exceeded", MAX_MAP_COLLISION_EDGES);

    edge->reserved84 = numBSPCollisionEdges;

    /* write edge to BSP lump */
    emit = &bspCollisionEdgeData[numBSPCollisionEdges];
    emit->reserved = 0;
    VectorCopy(edge->vertPoolData, emit->vertData);
    AxisCopy(edge->perpVec, (float *)emit->perpVec);
    emit->edgeLen = edge->edgeLen;
    numBSPCollisionEdges++;
  }
  return i;
}

/*
================
CM_EmitCollisionTris

Emits collision triangles to the BSP file format, resolving vertex/edge indices
================
*/
int CM_EmitCollisionTris(int triCount, CollisionTri_t *triArray)
{
  BspCollisionTri_t *emit;
  int i, j;

  for ( i = 0; i < triCount; i++ )
  {
    if ( numBSPCollisionTris == MAX_MAP_COLLISION_TRIS )
      Com_Error("MAX_MAP_COLLISIONTRIS (%i) exceeded", MAX_MAP_COLLISION_TRIS);

    emit = &bspCollisionTriData[numBSPCollisionTris];

    /* CoD4's disk triangle is only three uint16 vertex indices.  Edge
       walkability is kept in its separate bit-packed lump. */
    for ( j = 0; j < 3; j++ )
    {
      Assert((uint16_t)triArray[i].vertIdx[j] == triArray[i].vertIdx[j], s_assertDisable_CM_EmitCollisionPartitions);
      emit->vertIdx[j] = (uint16_t)triArray[i].vertIdx[j];
      if ( g_cmEdgePool[triArray[i].edgeIdx[j]].walkable )
        bspCollisionEdgeWalkable[(j + 3 * numBSPCollisionTris) >> 3] |= 1 << ((j + 3 * numBSPCollisionTris) & 7);
    }

    numBSPCollisionTris++;
  }
  return 0;
}

/*
================
CM_EmitCollisionBorders

Emits collision border data — computes edge normals, plane distances, and cross-product orientation
================
*/
char CM_EmitCollisionBorders(int triCount, CollisionTri_t *triArray)
{
  CollisionEdge_t *edges = g_cmEdgePool;
  CollisionEdge_t *edge;
  BspCollisionBorder_t *border;
  float *vert;
  float edgeDelta[3];
  float normLen, planeDist, scaledDist;
  int i, j;
  char borderCount;

  borderCount = 0;

  for ( i = 0; i < triCount; i++ )
  {
    for ( j = 0; j < 3; j++ )
    {
      edge = &edges[triArray[i].edgeIdx[j]];

      /* skip reverse-matched or unprocessed edges */
      if ( edge->reverseMatch || !edge->processed )
        continue;

      /* clear processed flag if material differs (but still emit border) */
      if ( edge->matId )
        edge->processed = 0;

      if ( numBSPCollisionBorders == MAX_MAP_COLLISION_BORDERS )
        Com_Error("MAX_MAP_COLLISIONBORDERS (%i) exceeded", MAX_MAP_COLLISION_BORDERS);

      /* Native 0x411FA0 reconstructs this delta from the compact edge core
         and the 16-byte collision-vertex pool instead of retaining edge
         geometry alongside the hash record. */
      vert = g_cmVertPool[edge->vertA].xyz;
      VectorSubtract(g_cmVertPool[edge->vertB].xyz, vert, edgeDelta);

      /* compute 2D edge normal (perpendicular in XY plane) */
      border = &bspCollisionBorders[numBSPCollisionBorders];
      border->edgeNormal[0] = -edgeDelta[1];
      border->edgeNormal[1] = edgeDelta[0];
      normLen = Vec2Normalize(border->edgeNormal);
      if ( normLen == 0.0 )
        continue;

      /* compute plane distance and orient normal away from opposite vertex */
      planeDist = MulAdd2(vert[0], border->edgeNormal[0], vert[1], border->edgeNormal[1]);
      border->planeDist = planeDist;

      { CmVert_t *oppV = &g_cmVertPool[edge->oppositeVert];
      if ( MulAdd2(oppV->xyz[0], border->edgeNormal[0], oppV->xyz[1], border->edgeNormal[1]) - planeDist >= 0.0 )
      {
        /* flip normal to point away from opposite vertex */
        border->edgeNormal[0] = -border->edgeNormal[0];
        border->edgeNormal[1] = -border->edgeNormal[1];
        border->planeDist = -border->planeDist;
        vert = g_cmVertPool[edge->vertB].xyz;
        scaledDist = -(edgeDelta[2] / normLen);
      }
      else
      {
        scaledDist = edgeDelta[2] / normLen;
      } }

      border->perpDist = scaledDist;
      border->vertZ = vert[2];
      border->crossProduct = Det2x2(border->edgeNormal[1], vert[0], vert[1], border->edgeNormal[0]);
      border->edgeLen = normLen;
      numBSPCollisionBorders++;
      borderCount++;
    }
  }
  return borderCount;
}

/*
================
CM_EmitCollisionPartitions

Emits all collision partitions to the BSP file, writing tris and borders for each
================
*/
int CM_EmitCollisionPartitions()
{
  CmPartition_t *part;
  BspCollisionPart_t *emit;
  int i;

  for ( i = g_cmPartStartIdx; i < g_cmNumPartitions; i++ )
  {
    if ( numBSPCollisionParts == MAX_MAP_COLLISION_PARTS )
      Com_Error("MAX_MAP_COLLISIONPARTITIONS (%i) exceeded", MAX_MAP_COLLISION_PARTS);

    part = &g_cmPartitionPool[i];
    emit = &bspCollisionParts[numBSPCollisionParts];

    emit->reserved0 = 0;
    emit->triCount = part->triCount;

    /* assert tri count fits in byte */
    Assert(!(emit->triCount != part->triCount), s_assertDisable_CM_EmitCollisionPartitions);

    /* emit tris and borders for this partition */
    emit->firstTri = numBSPCollisionTris;
    emit->firstBorder = numBSPCollisionBorders;
    CM_EmitCollisionTris(part->triCount, part->triArray);
    emit->borderCount = CM_EmitCollisionBorders(part->triCount, part->triArray);
    numBSPCollisionParts++;
  }
  return numBSPCollisionParts;
}

/*
================
CollisionBegin

Initializes (or resets) the collision system globals — allocates tri/vert/edge/partition pools on first call
================
*/
void *CollisionBegin()
{
  if ( g_collisionByte0 )
  {
    /* re-entrant call — advance start indices to current counts */
    g_cmTriStartIdx = g_cmNumTris;
    g_cmVertStartIdx = g_cmNumVerts;
    g_cmEdgeStartIdx = g_cmNumEdges;
    g_cmPartStartIdx = g_cmNumPartitions;
    return NULL;
  }

  /* first call — allocate all collision pools */
  g_collisionByte0 = 1;

  g_cmTriStartIdx = 0;
  g_cmMaxTris = MAX_MAP_COLLISION_TRIS;
  g_cmNumTris = 0;
  g_cmTriPool = malloc(MAX_MAP_COLLISION_TRIS * sizeof(CollisionTri_t));

  g_cmVertStartIdx = 0;
  g_cmMaxVerts = MAX_MAP_COLLISION_VERTS;
  g_cmNumVerts = 0;
  g_cmVertPool = malloc(MAX_MAP_COLLISION_VERTS * sizeof(CmVert_t));

  g_cmMaxEdges = MAX_MAP_COLLISION_EDGES;
  g_cmNumEdges = 0;
  g_cmNumConvexEdges = 0;
  g_cmEdgePool = malloc(MAX_MAP_COLLISION_EDGES * sizeof(CollisionEdge_t));
  memset(&g_cmEdgeHashTable, 0, sizeof(g_cmEdgeHashTable));

  g_cmPartStartIdx = 0;
  g_cmMaxPartitions = MAX_MAP_COLLISION_PARTS;
  g_cmNumPartitions = 0;
  g_cmPartitionPool = malloc(MAX_MAP_COLLISION_PARTS * sizeof(CmPartition_t));

  g_cmMinTrisPerLeaf = CM_MIN_LEAF_TRIS;
  g_cmMaxPartExtentX = CM_MAX_EXTENT;
  g_cmMaxPartExtentY = CM_MAX_EXTENT;
  g_cmMaxPartExtentZ = CM_MAX_EXTENT;
  g_cmOverlapRatio = CM_OVERLAP_RATIO;

  return g_cmPartitionPool;
}

/*
================
CollisionAddVertex

Adds a vertex to the collision vertex pool (deduplicates by exact position match)
================
*/
int CollisionAddVertex(float *xyz)
{
  CmVert_t *cv;
  int i;

  /* search for existing vertex with same position */
  for ( i = g_cmVertStartIdx; i < g_cmNumVerts; i++ )
  {
    cv = &g_cmVertPool[i];
    if ( VectorCompareExact(xyz, cv->xyz) )
      return i;
  }

  /* add new vertex */
  if ( g_cmNumVerts == g_cmMaxVerts )
    Com_Error("max collision vertices (%i) exceeded\n", g_cmMaxVerts);

  cv = &g_cmVertPool[g_cmNumVerts];
  cv->partitionId = 0;
  VectorCopy(xyz, cv->xyz);
  return g_cmNumVerts++;
}

/*
================
CollisionEdgeIsConvex

Tests whether an edge is convex by checking if the opposite vertex is above the triangle plane
================
*/
int CollisionEdgeIsConvex(float *plane, int matId, CollisionEdge_t *edge)
{
  CmVert_t *opp;
  double distSq2;

  /* material mismatch — mark as cross-material edge */
  if ( edge->matId != matId )
  {
    edge->matId = 0;
    edge->processed = 1;
    return 0;
  }
  if ( edge->processed )
    return 0;

  /* check if opposite vertex is above the plane */
  opp = &g_cmVertPool[edge->oppositeVert];
  {
    float dotResult = DotProduct210(opp->xyz, plane) - plane[3];
    float distSq1 = DistanceSquared(g_cmVertPool[edge->vertA].xyz, opp->xyz);

    if ( dotResult <= 0.0 )
      return 0;

    /* convexity threshold: dot^2 must exceed maxDist * epsilon */
    distSq2 = DistanceSquared(g_cmVertPool[edge->vertB].xyz, opp->xyz);
    if ( distSq1 - distSq2 >= 0.0 )
      distSq2 = distSq1;
  
    /* x87 loads dotResult from float → 80-bit, multiplies at 80-bit.
       On x64, float*float stays at 32-bit. Promote to match. */
    return distSq2 * CM_CONVEX_EPSILON <= (double)dotResult * dotResult;
  }
}

/* Native 0x40FD00.  The collision edge hash is symmetric in its two
   uint16 vertex IDs, so either winding reaches the same chain. */
static int CollisionEdgePairHash(int vertIdx0, int vertIdx1)
{
  return ((vertIdx1 + 31337) * (vertIdx0 + 31337)) & 0x3FF;
}

/*
================
CollisionAddEdge

Adds an edge to the hash table — deduplicates by vertex pair, tests convexity when 2 tris share edge
================
*/
int CollisionAddEdge(int vertIdx0, int vertIdx1, int oppositeVert, int matId, float *plane)
{
  CollisionEdge_t *edges = g_cmEdgePool;
  CollisionEdge_t *ce;
  CmVert_t *cvA, *cvB;
  int hashSlot;

  /* hash lookup by vertex pair */
  hashSlot = CollisionEdgePairHash(vertIdx0, vertIdx1);

  /* search hash chain for existing edge with same vertex pair */
  ce = NULL;
  for ( ce = g_cmEdgeHashTable[hashSlot]; ce; ce = ce->hashNext )
  {
    if ( (vertIdx0 == ce->vertA && vertIdx1 == ce->vertB) ||
         (vertIdx0 == ce->vertB && vertIdx1 == ce->vertA) )
      break;
  }

  /* existing edge found — update use count */
  if ( ce )
  {
    ce->useCount++;

    if ( ce->useCount == 2 )
    {
      /* second tri sharing this edge — test convexity */
      ce->reverseMatch = CollisionEdgeIsConvex(plane, matId, ce);
      if ( ce->reverseMatch )
        return (int)(ce - g_cmEdgePool);

      /* check for normal flip (concave edge) */
      if ( !ce->processed )
      {
        if ( (double)ce->normalZ * plane[2] <= 0.0 && plane[2] != ce->normalZ )
          ce->processed = 1;
      }
      if ( ce->processed && (double)plane[2] * plane[2] > (double)ce->normalZ * ce->normalZ )
      {
        ce->oppositeVert = oppositeVert;
        return (int)(ce - g_cmEdgePool);
      }
    }
    else
    {
      /* 3+ tris sharing edge — non-manifold, disable convexity */
      Assert(!(ce->useCount < 3), s_assertDisable_CollisionAddEdge);
      ce->reverseMatch = 0;
      ce->processed = 1;
    }
    return (int)(ce - g_cmEdgePool);
  }

  /* allocate new edge */
  if ( g_cmNumEdges == g_cmMaxEdges )
    Com_Error("max collision edges (%i) exceeded\n", g_cmMaxEdges);

  ce = &edges[g_cmNumEdges++];
  ce->hashNext = g_cmEdgeHashTable[hashSlot];
  g_cmEdgeHashTable[hashSlot] = ce;
  ce->matId = matId;
  ce->useCount = 1;
  ce->walkable = plane[2] >= 0.699999988079071f;
  ce->processed = 0;
  ce->reverseMatch = 0;
  ce->oppositeVert = oppositeVert;
  ce->normalZ = plane[2];
  ce->vertA = vertIdx0;
  ce->vertB = vertIdx1;

  /* compute edge geometry */
  cvA = &g_cmVertPool[vertIdx0];
  cvB = &g_cmVertPool[vertIdx1];
  VectorCopy(cvA->xyz, ce->vertPoolData);
  VectorSubtract(cvB->xyz, cvA->xyz, ce->edgeDir);
  ce->edgeLen = VecNormalize(ce->edgeDir);
  PerpendicularVector(ce->edgeDir, ce->perpVec);
  CrossProduct(ce->edgeDir, ce->perpVec, ce->crossVec);

  return g_cmNumEdges - 1;
}

/*
================
CollisionAddTriangle

Adds a triangle to the collision system: computes plane, bounds, barycentric coords, vertices, and edges
================
*/
static int CollisionTriangleKey(const uint16_t vertIdx[3])
{
  int minIndex;
  int maxIndex;

  minIndex = vertIdx[0] < vertIdx[1] ? vertIdx[0] : vertIdx[1];
  if ( vertIdx[2] < minIndex )
    minIndex = vertIdx[2];
  maxIndex = vertIdx[0] > vertIdx[1] ? vertIdx[0] : vertIdx[1];
  if ( vertIdx[2] > maxIndex )
    maxIndex = vertIdx[2];

  return (maxIndex << 16) + (minIndex << 8) + vertIdx[0] + vertIdx[1] + vertIdx[2];
}

static int CollisionTriVertexOrderMatches(const uint16_t lhs[3], const uint16_t rhs[3])
{
  return lhs[0] == rhs[0] && lhs[1] == rhs[1] && lhs[2] == rhs[2];
}

/* Native 0x40FDB0 removes only identically wound triangles.  A matching
   collision contents class keeps the earliest loaded material unless the
   existing triangle is translucent; opaque input always replaces it. */
static int CollisionReplaceDuplicateTriangle(int duplicateKey, const uint16_t vertIdx[3],
                                             unsigned char alpha, DrawSurf_t *drawSurf)
{
  CollisionTri_t *existing;
  int i;

  for ( i = 0; i < g_cmNumTris; ++i )
  {
    existing = &g_cmTriPool[i];
    if ( existing->duplicateKey != duplicateKey ||
         existing->drawSurf->surfaceFlags != drawSurf->surfaceFlags ||
         !CollisionTriVertexOrderMatches(existing->vertIdx, vertIdx) )
      continue;

    if ( IsLoadedMaterialBefore(drawSurf->shaderInfo, existing->drawSurf->shaderInfo) )
    {
      if ( existing->alpha < 0x80 )
      {
        existing->drawSurf = drawSurf;
        existing->alpha = alpha;
      }
    }
    else if ( alpha >= 0x80 )
    {
      existing->drawSurf = drawSurf;
      existing->alpha = alpha;
    }
    return 1;
  }
  return 0;
}

int CollisionAddTriangle(float *v0, float *v1, float *v2, unsigned char alpha, DrawSurf_t *ds)
{
  CollisionTri_t *tri;
  vec4_t plane;
  int vertCountBefore;

  Assert(ds->surfaceFlags != CONTENTS_NONE, s_assertDisable_CollisionAddTriangle);

  if ( !PlaneFromPoints(plane, v0, v1, v2) )
    return 0;

  vertCountBefore = g_cmNumVerts;
  {
    uint16_t vertIdx[3];

    vertIdx[0] = (uint16_t)CollisionAddVertex(v0);
    vertIdx[1] = (uint16_t)CollisionAddVertex(v1);
    vertIdx[2] = (uint16_t)CollisionAddVertex(v2);
    if ( g_cmNumVerts == vertCountBefore &&
         CollisionReplaceDuplicateTriangle(CollisionTriangleKey(vertIdx), vertIdx, alpha, ds) )
      return 0;

    /* allocate and initialize tri */
    tri = CM_AllocCollisionTri();
    VectorCopy(v0, tri->mins);
    VectorCopy(v0, tri->maxs);
    AddPointToBounds(v1, tri->mins, tri->maxs);
    AddPointToBounds(v2, tri->mins, tri->maxs);
    tri->drawSurf = ds;
    tri->partitionId = 0;
    CM_CopyVec4(plane, tri->plane);
    CM_ComputeBarycentricCoords(v0, v1, v2, plane, tri->baryCoords0, tri->baryCoords1);
    tri->duplicateKey = CollisionTriangleKey(vertIdx);
    tri->vertIdx[0] = vertIdx[0];
    tri->vertIdx[1] = vertIdx[1];
    tri->vertIdx[2] = vertIdx[2];
    tri->alpha = alpha;
  }

  Assert(VectorCompareExact(g_cmVertPool[tri->vertIdx[0]].xyz, v0), s_assertDisable_CollisionAddTriangle);
  Assert(VectorCompareExact(g_cmVertPool[tri->vertIdx[1]].xyz, v1), s_assertDisable_CollisionAddTriangle);
  Assert(VectorCompareExact(g_cmVertPool[tri->vertIdx[2]].xyz, v2), s_assertDisable_CollisionAddTriangle);

  /* add edges (deduplicated, convexity tested) */
  {
    uint16_t *v = tri->vertIdx;
    tri->edgeIdx[0] = CollisionAddEdge(v[1], v[2], v[0], ds->surfaceFlags, plane);
    tri->edgeIdx[1] = CollisionAddEdge(v[2], v[0], v[1], ds->surfaceFlags, plane);
    tri->edgeIdx[2] = CollisionAddEdge(v[0], v[1], v[2], ds->surfaceFlags, plane);
  }

  return tri->edgeIdx[2];
}

/*
================
CollisionAddPatchTris

Tessellates a patch surface into triangles and adds them to the collision system
================
*/
void CollisionAddPatchTris(DrawSurf_t *ds)
{
  Mesh_t *mesh;
  Mesh_t *stripped;
  MeshVert_t *v;
  float subdivLevel;
  int w, row, col, base;

  subdivLevel = (float)ds->subdivLevel;

  /* subdivide patch into mesh grid */
  mesh = SubdivideMesh(ds->patchWidth, ds->patchHeight, 0, ds->verts, 0, subdivLevel, SUBDIV_NO_MIN_LENGTH, 0, 0);
  PutMeshOnCurve(mesh->width, mesh->height, mesh->reserved, mesh->verts);
  stripped = RemoveLinearMeshColumnsRows(mesh, 0, 0);
  FreeMesh(mesh);

  /* walk grid quads, emit 2 tris per quad */
  w = stripped->width;
  v = stripped->verts;
  for ( row = 0; row < w - 1; row++ )
  {
    for ( col = 0; col < stripped->height - 1; col++ )
    {
      base = row + col * w;
      CollisionAddTriangle(v[base + w].pos, v[base + w + 1].pos, v[base + 1].pos, 255, ds);
      CollisionAddTriangle(v[base + 1].pos, v[base].pos, v[base + w].pos, 255, ds);
    }
  }
  FreeMesh(stripped);
}

/*
================
CollisionAddMeshTris

Adds pre-indexed mesh triangles to the collision system
================
*/
int CollisionAddMeshTris(DrawSurf_t *surf)
{
  MeshVert_t *verts = surf->verts;
  int i;

  if ( surf->numIndexes <= 0 )
    return surf->numIndexes;

  /* Native 0x40FFC0 gives tool-flag 0x10 terrain full opacity; all
     other terrain derives collision opacity from the three vertex alphas. */
  for ( i = 0; i < surf->numIndexes; i += 3 )
  {
    unsigned char alpha;
    int alphaSum;
    MeshVert_t *v0 = &verts[surf->indexes[i]];
    MeshVert_t *v1 = &verts[surf->indexes[i + 1]];
    MeshVert_t *v2 = &verts[surf->indexes[i + 2]];

    if ( (surf->shaderInfo->toolFlagsWord & 0x70) == 0x10 )
      alpha = 255;
    else
    {
      alphaSum = ((unsigned char *)&v0->color)[3] + ((unsigned char *)&v1->color)[3] +
                 ((unsigned char *)&v2->color)[3] + 2;
      alpha = (unsigned char)(alphaSum / 3);
    }
    if ( alpha )
      CollisionAddTriangle(v0->pos, v1->pos, v2->pos, alpha, surf);
  }
  return surf->numIndexes;
}

/*
================
CollisionAddSurfaces

Iterates model's surfaces, dispatches patch/mesh tris, then validates edge vertex use counts
================
*/
int CollisionAddSurfaces(Entity_t *model)
{
  DrawSurf_t *surf;
  int i;

  /* add collision tris from each surface */
  for ( i = model->firstDrawSurf; i < numMapDrawSurfs; i++ )
  {
    surf = &g_drawSurfs[i];
	
    /* skip empty surfaces and sky */
    if ( !surf->numVerts || (surf->surfaceFlags & SURF_SKY) )
      continue;

    if ( surf->isPatch )
      CollisionAddPatchTris(surf);
    else if ( surf->isTerrain )
      CollisionAddMeshTris(surf);
  }

  return i;
}

/*
================
CollisionComputePartitionBounds

Computes the bounding box (mins/maxs) enclosing all tris in the given array
================
*/
void CollisionComputePartitionBounds(CollisionTri_t *tris, int triCount, float *mins, float *maxs)
{
  int i;

  /* seed bounds from first tri */
  VectorCopy(tris[0].mins, mins);
  VectorCopy(tris[0].maxs, maxs);

  /* expand to enclose remaining tris */
  for ( i = 1; i < triCount; i++ )
    AddBoundsToBounds(tris[i].mins, tris[i].maxs, mins, maxs);
}

/*
================
CollisionPartitionTris

Attempts to partition tris by splitting along the axis with largest extent
================
*/
int CollisionPartitionTris(CollisionTri_t *triArray, int triCount)
{
  int i, result;
  int order[3];
  vec3_t mins, maxs, extents;
  int fitsInByte;

  CollisionComputePartitionBounds(triArray, triCount, mins, maxs);

  /* check if tri count fits in byte and extents are within limits */
  fitsInByte = (triCount == (unsigned char)triCount);
  extents[0] = (float)((double)maxs[0] - mins[0] - g_cmMaxPartExtentX);
  extents[1] = (float)((double)maxs[1] - mins[1] - g_cmMaxPartExtentY);
  extents[2] = (float)((double)maxs[2] - mins[2] - g_cmMaxPartExtentZ);

  /* no split needed if count fits and all extents within limits */
  if ( fitsInByte && extents[0] < 0.0 && extents[1] < 0.0 && extents[2] < 0.0 )
    return 0;

  /* try splitting along each axis in order of largest extent */
  CM_GetAxisSortOrder(order, extents);
  for ( i = 0; i < 3; i++ )
  {
    if ( extents[order[i]] <= 0.0 )
      break;
    result = CM_SubdivideTriArray(triArray, triCount, order[i], 0);
    if ( result )
      return result;
  }

  /* if count overflows byte, force split on largest axis */
  if ( fitsInByte )
    return 0;
  return CM_SubdivideTriArray(triArray, triCount, order[0], 1);
}

/*
================
CM_BuildCollisionTreeSplitRecursive

Recursively splits a tri array into partitions until each is below minTrisPerLeaf threshold
================
*/
int CM_BuildCollisionTreeSplitRecursive(CollisionTri_t *triArray, int triCount)
{
  int splitCount;

  splitCount = 2 * g_cmMinTrisPerLeaf;
  if ( triCount >= splitCount )
  {
    splitCount = CollisionPartitionTris(triArray, triCount);
    if ( splitCount )
    {
      CM_BuildCollisionTreeSplitRecursive(triArray, splitCount);
      return CM_BuildCollisionTreeSplitRecursive(triArray + splitCount, triCount - splitCount);
    }
  }
  return splitCount;
}

/*
================
CM_BuildCollisionTreeLeaf

Creates a leaf node in the collision tree from the given tri array
================
*/
int CM_BuildCollisionTreeLeaf(CollisionTri_t *triArray, int triCount)
{
  CmPartition_t *part;
  int idx;

  part = &g_cmPartitionPool[triArray->partitionId - 1];
  part->triArray = triArray;
  part->triCount = triCount;
  CollisionComputePartitionBounds(triArray, triCount, part->sideMin, &part->sideMax[1]);
  idx = (int)(part - g_cmPartitionPool);
  part->emitIndex = idx;
  return idx;
}

/*
================
CM_BuildCollisionTree

Builds the collision tree: partitions tris by material, subdivides by axis, creates leaf nodes
================
*/
int CM_BuildCollisionTree()
{

  /* assign partition IDs by connected vertex groups */
  CM_PartitionTris();

  /* sort tris by partition ID */
  qsort(&g_cmTriPool[g_cmTriStartIdx], g_cmNumTris - g_cmTriStartIdx,
        sizeof(CollisionTri_t), CM_ComparePartitionId);

  /* sort within each partition by material, subdivide, then create leaf nodes */
  CM_ForEachPartitionGroup(CM_SortTrisByPartition);
  CM_ForEachPartitionGroup(CM_BuildCollisionTreeSplitRecursive);
  return CM_ForEachPartitionGroup(CM_BuildCollisionTreeLeaf);
}

/*
================
EmitLeafBrushes

Entry point: builds and emits collision data for curve/terrain surfaces in a BSP model
================
*/
int EmitLeafBrushes(Entity_t *model, Tree_t *tree)
{
  int result;

  if ( !g_currentEntityIndex )
    printf("\nbuilding curve/terrain collision...\n");

  /* collect collision tris from patch/terrain surfaces */
  CollisionBegin();
  CollisionAddSurfaces(model);
  result = g_cmNumTris;

  /* if any tris were added, build and emit collision data */
  if ( g_cmNumTris != g_cmTriStartIdx )
  {
    CM_BuildCollisionTree();
    CM_BuildTriWindings(tree->headnode);
    CM_BuildNodeCollision_r(tree->headnode);
    CM_EmitCollisionVerts();
    return CM_EmitCollisionPartitions();
  }
  return result;
}

/*
================
CM_ComputeBarycentricPlane

Computes a barycentric plane from cross product of two vectors, normalized by projected scale
================
*/
void CM_ComputeBarycentricPlane(float *point, float *outPlane, float *base, float *target, float *vecA, float *vecB)
{
  double crossX;
  double crossY;
  double crossZ;
  double offset;
  double scale;

  /* cross product of vecA x vecB */
  crossX = Det2x2(vecB[2], vecA[1], vecA[2], vecB[1]);
  crossY = Det2x2(vecA[2], vecB[0], vecA[0], vecB[2]);
  crossZ = Det2x2(vecA[0], vecB[1], vecB[0], vecA[1]);

  /* project base+point midpoint and target onto cross product */
#ifdef _WIN64
  offset = (((double)base[2] + point[2]) * crossZ + ((double)base[1] + point[1]) * crossY + ((double)base[0] + point[0]) * crossX) * 0.5;
  scale = (double)target[2] * crossZ + (double)target[1] * crossY + (double)target[0] * crossX - offset;
#else
  offset = ((base[2] + point[2]) * crossZ + (base[1] + point[1]) * crossY + (base[0] + point[0]) * crossX) * 0.5;
  scale = target[2] * crossZ + target[1] * crossY + target[0] * crossX - offset;
#endif
  AssertMsg(scale > 0, s_assertDisable_CM_ComputeBarycentricPlane, "%s\n\t%s", "scale > 0", va("scale = %lg", scale));
  outPlane[0] = crossX * (1.0 / scale);
  outPlane[1] = crossY * (1.0 / scale);
  outPlane[2] = crossZ * (1.0 / scale);
  outPlane[3] = 1.0 / scale * offset;
}

/*
================
CM_ComputeBarycentricCoords

Computes barycentric coordinate planes U and V for a triangle
================
*/
void CM_ComputeBarycentricCoords(float *vertA, float *vertB, float *vertC, float *plane, float *baryPlaneU, float *baryPlaneV)
{
  vec3_t edgeAB, edgeAC;

  VectorSubtract(vertB, vertA, edgeAB);
  VectorSubtract(vertC, vertA, edgeAC);
  CM_ComputeBarycentricPlane(vertC, baryPlaneU, vertA, vertB, plane, edgeAC);
  CM_ComputeBarycentricPlane(vertB, baryPlaneV, vertA, vertC, edgeAB, plane);
}

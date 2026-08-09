/*
tris.c — Triangle surface system

Reconstructed from cod2map.exe by Rose.

Pipeline: FindTriangleWindings -> CoalesceWindings -> MergeConcaveWindings
-> FixTJunctions -> FindIndexMapping -> Triangulate -> SmoothNormals -> Emit

The tris system converts BSP face windings into triangle meshes for the
d3dbsp output. Surfaces are coalesced by material, merged into concave
windings where possible, T-junction fixed, then triangulated and emitted
as triSoup entries.
*/

#include "cod4map.h"
#include "fixedpoint.h"

void (*g_lerpAuxDataCallback)(float *from, float *to, double frac, float *result) = &NullLerpAuxDataCallback;

PFN_D3DXOptimizeFaces    pfn_D3DXOptimizeFaces;
PFN_D3DXOptimizeVertices pfn_D3DXOptimizeVertices;

int d3dx_loaded = 0;

void load_d3dx(void) {
    if (d3dx_loaded) return;
    /* cod4map.exe imports its face/vertex cache optimizer from d3dx9_34. */
    HMODULE hmod = LoadLibraryA("d3dx9_34.dll");
    if (!hmod) {
        exit(1);
    }
    pfn_D3DXOptimizeFaces = (PFN_D3DXOptimizeFaces)GetProcAddress(hmod, "D3DXOptimizeFaces");
    pfn_D3DXOptimizeVertices = (PFN_D3DXOptimizeVertices)GetProcAddress(hmod, "D3DXOptimizeVertices");
    if (!pfn_D3DXOptimizeFaces || !pfn_D3DXOptimizeVertices) {
        exit(1);
    }
    d3dx_loaded = 1;
}

int D3DXOptimizeFaces(void* meshData, unsigned int faceCount, unsigned int vertexCount, int adjacency, void* faceRemap) {
    load_d3dx();
    return pfn_D3DXOptimizeFaces(meshData, faceCount, vertexCount, adjacency, faceRemap);
}

int D3DXOptimizeVertices(void* meshData, unsigned int faceCount, unsigned int vertexCount, int adjacency, void* vertexRemap) {
    load_d3dx();
    return pfn_D3DXOptimizeVertices(meshData, faceCount, vertexCount, adjacency, vertexRemap);
}

DrawVert_t       drawVertBuffer[MAX_MAP_VERTEXES];
MatSortEntry_t   materialGroupSortTable[MAX_MAP_MATERIALS];
PlatformEntry_t *g_targetPlatform = 0;
TriRecord_t      triRecords[MAX_MAP_TRIANGLES];
TriSurfProps_t  *g_windingHashBuckets[WINDING_HASH_BUCKETS];
static unsigned char g_emitReflectionProbeIndex;
static unsigned char g_emitPrimaryLightIndex;
static unsigned char g_emitCastsSunShadow;
static TriSurfPropsSidecar_t *g_triSurfPropsSidecars;
TriSurf_t       *triSurfCellArray[TRISURFS_CELL_ARRAY_SIZE]; /* surface list head per cell slot */

FILE   *g_debugPolyFile;
int     g_totalSplits;
int     g_triSurf_alloc_seqNo = 0;
double  g_trisTimerStart;
int     g_vertexHashTable[VERTEX_HASH_TABLE_SIZE];
int     numDrawVertices;
int     numTriSurfs;
size_t  numTriangles;
int    *reorderIndexBuffer;
int    *reorderOptimizeRemap;
int    *reorderInverseRemap;
int    *reorderOptimizedBuf;
int    *reorderVertexRemap;
int     tesselateDegenerateCount;
int     triFirstVertIndex;
void   *triSurfFreeList;
int     trisTransientMode;

char s_assertDisable_AdjustTriangleUVs_r;
char s_assertDisable_AuxDataCopy;
char s_assertDisable_BuildTriSoupAABBTree;
char s_assertDisable_ClassifyTriVertsByAxis;
char s_assertDisable_ClipTriWinding;
char s_assertDisable_ClipTriWinding;
char s_assertDisable_ClipTriWinding;
char s_assertDisable_ClipTriWinding;
char s_assertDisable_ClipTriWinding;
char s_assertDisable_ClipTriWinding;
char s_assertDisable_ClipTriWinding;
char s_assertDisable_ClipTriWinding;
char s_assertDisable_ClipTriWinding;
char s_assertDisable_ClipTriWinding;
char s_assertDisable_ClipTriWinding;
char s_assertDisable_CopyTriSurf;
char s_assertDisable_CopyVerts;
char s_assertDisable_CopyVerts;
char s_assertDisable_CopyVerts;
char s_assertDisable_EmitPatchSurface;
char s_assertDisable_EmitPatchSurface;
char s_assertDisable_EmitTriSoupRecord;
char s_assertDisable_EmitTriSoupRecord;
char s_assertDisable_EmitTriSoupRecord;
char s_assertDisable_EmitTriSoupRecord;
char s_assertDisable_EmitTriSoupRecord;
char s_assertDisable_EmitTriSoupRecord;
char s_assertDisable_EmitTriSoupRecord;
char s_assertDisable_EmitTriSurfForProps;
char s_assertDisable_EmitTriSurfForProps;
char s_assertDisable_EmitTriSurfForProps;
char s_assertDisable_EmitTriSurface;
char s_assertDisable_EmitTriSurface;
char s_assertDisable_EmitTriSurface;
char s_assertDisable_EmitTriSurface;
char s_assertDisable_EmitTriSurface;
char s_assertDisable_EmitTriSurfaceForDrawSurf;
char s_assertDisable_EmitTriSurface_r;
char s_assertDisable_EmitTriSurface_r;
char s_assertDisable_EmitTriangleSoup;
char s_assertDisable_EmitTriangleSoup;
char s_assertDisable_FreeVerts;
char s_assertDisable_GroupAdjacentTris;
char s_assertDisable_GroupTriSurfsIntoSubgroups;
char s_assertDisable_GroupTriangles;
char s_assertDisable_GroupTriangles;
char s_assertDisable_I_fclamp;
char s_assertDisable_InsertTriSurfAfter;
char s_assertDisable_MergeTriGroups;
char s_assertDisable_MergeTriGroups;
char s_assertDisable_MergeTriGroups;
char s_assertDisable_MergeTriGroups;
char s_assertDisable_MergeTriGroups;
char s_assertDisable_MergeTriGroups;
char s_assertDisable_MergeTriGroups;
char s_assertDisable_MergeTriGroups;
char s_assertDisable_MergeTriGroups;
char s_assertDisable_NullLerpAuxDataCallback;
char s_assertDisable_PrependTriSurf;
char s_assertDisable_SetTrisTransientMode;
char s_assertDisable_SmoothVertexNormals;
char s_assertDisable_SmoothVertexNormals;
char s_assertDisable_SortTriIndices;
char s_assertDisable_SortTriIndices;
char s_assertDisable_SortTriIndices;
char s_assertDisable_SplitSoupOnAxis;
char s_assertDisable_SplitSoupOnAxis;
char s_assertDisable_SplitSoupOnAxis;
char s_assertDisable_SplitSoupOnAxis;
char s_assertDisable_SplitSoupOnAxis;
char s_assertDisable_SplitTrisByClassification;
char s_assertDisable_TriangulateSurf;
char s_assertDisable_TriangulateSurf;
char s_assertDisable_TriangulateSurf;
char s_assertDisable_TriangulateSurf;
char s_assertDisable_TrisSnapNearbyVertices;
char s_assertDisable_TrisSnapNearbyVertices;
char s_assertDisable_TrisSnapWindingToVertices;
char s_assertDisable_TrisSnapWindingToVertices;
char s_assertDisable_TrisSnapWindingToVertices;
char s_assertDisable_TrisSnapWindingToVertices;
char s_assertDisable_Tris_CopyVertexToDrawVerts;
char s_assertDisable_Tris_CopyVertexToDrawVerts;
char s_assertDisable_Tris_CopyVertexToDrawVerts;
char s_assertDisable_Tris_EmitTriangles;
char s_assertDisable_Tris_EmitTriangles;
char s_assertDisable_Tris_FindNextEntity;
char s_assertDisable_Tris_FindWindings;
char s_assertDisable_Tris_ReorderAddSurfaceVerts;
char s_assertDisable_Tris_ReorderAddSurfaceVerts;
char s_assertDisable_Tris_ReorderAndOptimizeVerts;
char s_assertDisable_Tris_ReorderAndOptimizeVerts;
char s_assertDisable_Tris_ReorderAndOptimizeVerts;
char s_assertDisable_Tris_ReorderAndOptimizeVerts;
char s_assertDisable_Tris_ReorderAndOptimizeVerts;
char s_assertDisable_Tris_ReorderAndOptimizeVerts;
char s_assertDisable_Tris_ReorderAndOptimizeVerts;
char s_assertDisable_Tris_ReorderAndOptimizeVerts;
char s_assertDisable_Tris_ReorderAndOptimizeVerts;
char s_assertDisable_Tris_ReorderDrawVerts;
char s_assertDisable_Tris_ReorderDrawVerts;
char s_assertDisable_Tris_ReorderTriSurfaces;
char s_assertDisable_Tris_ReorderTriSurfaces;
char s_assertDisable_Tris_ReorderTriSurfaces;
char s_assertDisable_Tris_ReorderTriSurfaces;
char s_assertDisable_UnlinkAndFreeSurf;
char s_assertDisable_ValidateTriSurfLmapCoords;
char s_assertDisable_ValidateTriSurfLmapCoords;

/*
   EmitDrawSurfaces in the native compiler runs the emitter twice: type 1
   fills the unlayered BSP family and type 0 fills the layered family.  Keep
   the old names local to this translation unit, but make them aliases for
   the selected family so every emitter helper (including the AABB builder
   and vertex reorderer) operates on one coherent set of buffers.
*/
typedef struct DrawSurfaceContext_s
{
  BspTriSoup_t *triangles;
  BspDrawVert_t *drawVerts;
  BspDrawVert_t *triSoupVerts;
  unsigned short *drawIndexes;
  BspAabbTreeEntry_t *aabbTrees;
  int *numTriSoups;
  int *numDrawVerts;
  int *numDrawVertsEmitted;
  int *numDrawIndexes;
  int *numAabbTrees;
} DrawSurfaceContext_t;

static DrawSurfaceContext_t s_drawSurfaceContexts[2] =
{
  {
    bspTriangles, bspDrawVerts, g_drawVertexBuf, bspDrawIndexes, bspAabbTrees,
    &numBSPTriSoups, &numBSPDrawVerts, &numBSPDrawVertsEmitted,
    &numBSPDrawIndexes, &numBSPAabbTrees
  },
  {
    bspUnlayeredTriangles, bspUnlayeredDrawVerts, g_unlayeredDrawVertexBuf,
    bspUnlayeredDrawIndexes, bspUnlayeredAabbTrees,
    &numBSPUnlayeredTriSoups, &numBSPUnlayeredDrawVerts,
    &numBSPUnlayeredDrawVertsEmitted, &numBSPUnlayeredDrawIndexes,
    &numBSPUnlayeredAabbTrees
  }
};

static DrawSurfaceContext_t *s_drawSurfaceContext = &s_drawSurfaceContexts[0];

/* CoD4 0x449CF0.  The pointer/capacity part of the native 72-byte
   descriptor is static in KIWI; the output counters must still begin each
   BSP with the same zero state. */
static void Tris_InitDrawSurfaceContext(DrawSurfaceContext_t *context)
{
  *context->numTriSoups = 0;
  *context->numDrawVerts = 0;
  *context->numDrawVertsEmitted = 0;
  *context->numDrawIndexes = 0;
  *context->numAabbTrees = 0;
}

/* CoD4 0x449CA0. */
void Tris_InitDrawSurfaceContexts(void)
{
  Tris_InitDrawSurfaceContext(&s_drawSurfaceContexts[0]);
  Tris_InitDrawSurfaceContext(&s_drawSurfaceContexts[1]);
  s_drawSurfaceContext = &s_drawSurfaceContexts[0];
}

static void Tris_SelectDrawSurfaceContext(int unlayered)
{
  s_drawSurfaceContext = &s_drawSurfaceContexts[unlayered != 0];
}

#define bspTriangles              (s_drawSurfaceContext->triangles)
#define bspDrawVerts              (s_drawSurfaceContext->drawVerts)
#define g_drawVertexBuf           (s_drawSurfaceContext->triSoupVerts)
#define bspDrawIndexes            (s_drawSurfaceContext->drawIndexes)
#define bspAabbTrees              (s_drawSurfaceContext->aabbTrees)
#define numBSPTriSoups            (*s_drawSurfaceContext->numTriSoups)
#define numBSPDrawVerts           (*s_drawSurfaceContext->numDrawVerts)
#define numBSPDrawVertsEmitted    (*s_drawSurfaceContext->numDrawVertsEmitted)
#define numBSPDrawIndexes         (*s_drawSurfaceContext->numDrawIndexes)
#define numBSPAabbTrees           (*s_drawSurfaceContext->numAabbTrees)


/*
================
GroupTriSurfsIntoSubgroups

Assigns sequential group IDs to triangles sorted by material index,
computes bounding boxes per group, and merges spatially overlapping groups.
================
*/
int GroupTriSurfsIntoSubgroups(TriRecord_t *records, int numTris, int groupCount)
{
    TriGroup_t *groups;
    TriGroup_t *grp;
    TriRecord_t *tri;
    int groupId;
    int curTriIdx;
    int nextTriIdx;
    int prevGroupCount;
    int i, j, k;
    int canMerge;
    float mergedMins[3];
    float mergedMaxs[3];
    float mergedExtents[3];
    float minVal, maxVal;
    float extentVal, combinedExtent;

    if (groupCount > MAX_TRI_SUBGROUPS) {
        Com_Error("%i > MAX_TRI_SUBGROUPS", groupCount);
    }

    groups = malloc(groupCount * sizeof(*groups) + 64);
    if (!groups) {
        return 0;
    }

    groupId = 0;
    curTriIdx = 0;

    if (numTris > 0) {
        grp = groups;

        while (curTriIdx < numTris) {
            ClearBounds(grp->mins, grp->maxs);
            grp->firstTri = curTriIdx;

            nextTriIdx = curTriIdx + 1;

            if (nextTriIdx < numTris) {
                int firstGroupId = records[curTriIdx].groupId;

                while (nextTriIdx < numTris && records[nextTriIdx].groupId == firstGroupId) {
                    tri = &records[nextTriIdx];
                    tri->groupId = groupId;

                    AddPointToBounds(DRAW_VERT(tri->vertIdx[0]).pos, grp->mins, grp->maxs);
                    AddPointToBounds(DRAW_VERT(tri->vertIdx[1]).pos, grp->mins, grp->maxs);
                    AddPointToBounds(DRAW_VERT(tri->vertIdx[2]).pos, grp->mins, grp->maxs);

                    nextTriIdx++;
                }
            }

            tri = &records[curTriIdx];
            tri->groupId = groupId;

            AddPointToBounds(DRAW_VERT(tri->vertIdx[0]).pos, grp->mins, grp->maxs);
            AddPointToBounds(DRAW_VERT(tri->vertIdx[1]).pos, grp->mins, grp->maxs);
            AddPointToBounds(DRAW_VERT(tri->vertIdx[2]).pos, grp->mins, grp->maxs);

            VectorSubtract(grp->maxs, grp->mins, grp->extents);

            grp->triCount = nextTriIdx - curTriIdx;

            groupId++;
            curTriIdx = nextTriIdx;
            ++grp;
        }
    }

    Assert(groupId == groupCount, s_assertDisable_GroupTriSurfsIntoSubgroups);

    /* Merge spatially overlapping groups until stable */
    do {
        prevGroupCount = groupCount;
        i = groupCount - 2;

        while (i >= 0) {
            j = i + 1;
            while (j < groupCount) {
                if (groups[i].triCount + groups[j].triCount <= MAX_TRIS_PER_GROUP) {
                    canMerge = 1;

                    for (k = 0; k < 3 && canMerge; k++) {
                        minVal = (groups[j].mins[k] < groups[i].mins[k]) ? groups[j].mins[k] : groups[i].mins[k];
                        maxVal = (groups[j].maxs[k] > groups[i].maxs[k]) ? groups[j].maxs[k] : groups[i].maxs[k];
                        extentVal = maxVal - minVal;

                        mergedMins[k] = minVal;
                        mergedMaxs[k] = maxVal;
                        mergedExtents[k] = extentVal;

                        combinedExtent = groups[i].extents[k] + groups[j].extents[k] + GROUP_OVERLAP_TOLERANCE;
                        if (extentVal > combinedExtent)
                            canMerge = 0;
                    }

                    if (canMerge) {
                        MergeTriGroups(records, numTris, i, j,
                                       mergedMins, mergedMaxs, mergedExtents,
                                       groups, groupCount);
                        groupCount--;
                        j = i;
                    }
                }

                j++;
            }

            i--;
        }
    } while (groupCount != prevGroupCount);

    free(groups);
    return groupCount;
}


/*
================
GetTrisTransientMode

Returns the current transient mode from triGlob.
================
*/
int GetTrisTransientMode(void)
{
  return trisTransientMode;
}

/*
================
SetTrisTransientMode

Sets triGlob.transientMode and optionally clears
surface transient data for all cells.
================
*/
void SetTrisTransientMode(int mode, int preserveSurfaces)
{
  int i, cellCount;
  TriSurf_t *ts;

  Assert(!(mode && trisTransientMode), s_assertDisable_SetTrisTransientMode);
  trisTransientMode = mode;

  if ( !preserveSurfaces )
  {
    cellCount = g_currentEntityIndex <= 0 ? numBSPCullGroups + numBSPCells : 1;
    for ( i = 0; i < cellCount; i++ )
    {
      for ( ts = triSurfCellArray[i]; ts; ts = ts->next )
        ts->origWinding = NULL;
    }
  }
}

/*
================
FreeVerts

Frees a verts struct: releases the winding and auxData,
then zeroes both pointers.
================
*/
void FreeVerts(WindingAuxPair_t *verts)
{
  Assert(verts, s_assertDisable_FreeVerts);
  FreeWinding(verts->winding);
  free(verts->auxData);
  verts->winding = NULL;
  verts->auxData = NULL;
}

/*
================
CopyVerts

Copies a verts struct by duplicating the winding and
allocating a new auxData buffer with memcpy.
================
*/
void CopyVerts(WindingAuxPair_t *from, int auxElemSize, WindingAuxPair_t *dest)
{
  int copySize;

  Assert(from, s_assertDisable_CopyVerts);
  Assert(from->winding, s_assertDisable_CopyVerts);
  Assert(from->auxData || !auxElemSize, s_assertDisable_CopyVerts);

  dest->winding = CopyWinding(from->winding);

  if ( auxElemSize )
  {
    copySize = auxElemSize * dest->winding->numpoints;
    dest->auxData = malloc(copySize);
    memcpy(dest->auxData, from->auxData, copySize);
  }
  else
  {
    dest->auxData = NULL;
  }
}

/*
================
NullLerpAuxDataCallback

Default aux data lerp callback that always asserts.
Must be replaced with a real callback before use.
================
*/
void NullLerpAuxDataCallback(float *from, float *to, double frac, float *result)
{
  AssertFatal(g_assertsDisabled, s_assertDisable_NullLerpAuxDataCallback);
}

/*
================
GetTrisCount

Returns the current triSurf count.
================
*/
int GetTrisCount(void)
{
  return numTriSurfs;
}

/*
================
ForEachSurf

Iterates all surfaces in all cells, calling
callback(surf, isTarget) for each surface.
================
*/
int ForEachSurf(void (*callback)(TriSurf_t *, bool), TriSurf_t *targetSurf)
{
  TriSurf_t *ts;
  TriSurf_t *next;
  int i, cellCount;

  cellCount = g_currentEntityIndex <= 0 ? numBSPCells + numBSPCullGroups : 1;

  for ( i = 0; i < cellCount; i++ )
  {
    /* 0x43D080 snapshots next before the callback, so callbacks such as
       0x43EC20 may unlink and free their current surface. */
    for ( ts = triSurfCellArray[i]; ts; ts = next )
    {
      next = ts->next;
      callback(ts, ts == targetSurf);
    }
  }

  return cellCount;
}

/*
================
UnlinkAndFreeSurf

Unlinks a surface from its doubly-linked list and
removes it from the grid tree.
================
*/
void UnlinkAndFreeSurf(TriSurf_t *ts, TriSurf_t **listHead)
{
  Assert(ts, s_assertDisable_UnlinkAndFreeSurf);
  if ( ts->prev )
  {
    ts->prev->next = ts->next;
  }
  else if ( listHead )
  {
    *listHead = ts->next;
  }
  if ( ts->next )
    ts->next->prev = ts->prev;
  ts->prev = NULL;
  ts->next = NULL;
  GridTree_Remove(ts);
}

/*
================
FreeTriSurf

Frees a triSurf and all its windings and auxData,
then decrements the global surface count.
================
*/
int FreeTriSurf(TriSurf_t *ts)
{
  int i;

  /* Native 0x43D260 unlinks from the cell list itself.  Callers which have
     already unlinked the surface leave no matching head, which is likewise
     a no-op in the original 0x43D360 helper. */
  if ( ts->next )
    ts->next->prev = ts->prev;
  if ( ts->prev )
  {
    ts->prev->next = ts->next;
  }
  else
  {
    int cellCount = g_currentEntityIndex <= 0 ? numBSPCullGroups + numBSPCells : 1;
    for ( i = 0; i < cellCount; i++ )
    {
      if ( triSurfCellArray[i] == ts )
      {
        triSurfCellArray[i] = ts->next;
        break;
      }
    }
  }

  TrisLmap_FreeAssignmentsForSurface(ts);
  TrisLmap_FreeSurface(ts);

  if ( ts->winding )
    FreeWinding(ts->winding);
  free(ts->auxData);

  if ( trisTransientMode == 1 && ts->origWinding )
    FreeWinding(ts->origWinding);

  /* free hole windings and aux data */
  for ( i = 0; i < ts->holeCount; i++ )
  {
    FreeWinding(ts->holes[i].winding);
    free(ts->holes[i].auxData);
  }
  if ( ts->holeCount )
    free(ts->holes);

  free(ts);
  return --numTriSurfs;
}

/*
================
I_fclamp

Clamps a float value to [min, max] range.
================
*/
/* Native TriSurfProps_t is an exact 0xAC carrier.  The smoothing and
   reflection state introduced by KIWI is intentionally external so it cannot
   displace its plane or list links (0x43FB60, 0x44E6F0). */
TriSurfPropsSidecar_t *TrisPropsSidecar_Get(TriSurfProps_t *props)
{
  TriSurfPropsSidecar_t *entry;

  for (entry = g_triSurfPropsSidecars; entry; entry = entry->next)
  {
    if (entry->props == props)
      return entry;
  }
  return NULL;
}

void TrisPropsSidecar_Create(TriSurfProps_t *props, float smoothAngleValue, int smoothingValue,
  unsigned char reflectionProbeIndex, unsigned char reflectionCellMode, int lightStyle)
{
  TriSurfPropsSidecar_t *entry;

  Assert(props, s_assertDisable_EmitTriSurface);
  entry = TrisPropsSidecar_Get(props);
  if (!entry)
  {
    entry = (TriSurfPropsSidecar_t *)malloc(sizeof(*entry));
    if (!entry)
      Com_Error("TrisPropsSidecar_Create: out of memory");
    entry->props = props;
    entry->mapDrawSurf = NULL;
    entry->next = g_triSurfPropsSidecars;
    g_triSurfPropsSidecars = entry;
  }
  entry->smoothAngle = smoothAngleValue;
  entry->smoothing = smoothingValue;
  entry->lightStyle = lightStyle;
  entry->reflectionProbeIndex = reflectionProbeIndex;
  entry->reflectionCellMode = reflectionCellMode;
}

void TrisPropsSidecar_SetMapDrawSurf(TriSurfProps_t *props, MapDrawSurf_t *mapDrawSurf)
{
  TriSurfPropsSidecar_t *entry = TrisPropsSidecar_Get(props);

  Assert(entry, s_assertDisable_EmitTriSurface);
  entry->mapDrawSurf = mapDrawSurf;
}

void TrisPropsSidecar_Clone(TriSurfProps_t *source, TriSurfProps_t *clone)
{
  TriSurfPropsSidecar_t *entry = TrisPropsSidecar_Get(source);

  if (entry)
  {
    TrisPropsSidecar_Create(clone, entry->smoothAngle, entry->smoothing,
      entry->reflectionProbeIndex, entry->reflectionCellMode, entry->lightStyle);
    TrisPropsSidecar_SetMapDrawSurf(clone, entry->mapDrawSurf);
  }
}

void TrisPropsSidecar_Destroy(TriSurfProps_t *props)
{
  TriSurfPropsSidecar_t **link = &g_triSurfPropsSidecars;

  while (*link)
  {
    TriSurfPropsSidecar_t *entry = *link;
    if (entry->props == props)
    {
      *link = entry->next;
      free(entry);
      return;
    }
    link = &entry->next;
  }
}

double I_fclamp(float val, float min, float max)
{
  Assert(min < (double)max, s_assertDisable_I_fclamp);
  if ( val < (double)min )
    return min;
  if ( val <= (double)max )
    return val;
  return max;
}

/*
================
FreeTriSurfProps

Releases the native triSurfProps allocation list and its coalesce chains.
================
*/
void FreeTriSurfProps(void)
{
  /* walk the triSurfProps free list and free each allocation */
  TriSurfProps_t *cur = (TriSurfProps_t *)triSurfFreeList;
  TriSurfProps_t *next;
  int bucket;

  while ( cur )
  {
    CoalesceNode_t *chain = cur->coalesceChain;
    CoalesceNode_t *chainNext;

    while (chain)
    {
      chainNext = chain->next;
      free(chain);
      chain = chainNext;
    }
    next = cur->freeListNext;
    TrisPropsSidecar_Destroy(cur);
    free(cur);
    cur = next;
  }
  triSurfFreeList = 0;

  /* Coalesced props live only in the transient hash buckets, not in the
     emitted-props free list above.  Release their chains and KIWI sidecars at
     the same post-emission boundary before the source props are recreated. */
  for (bucket = 0; bucket < WINDING_HASH_BUCKETS; ++bucket)
  {
    cur = g_windingHashBuckets[bucket];
    g_windingHashBuckets[bucket] = NULL;
    while (cur)
    {
      CoalesceNode_t *chain = cur->coalesceChain;
      CoalesceNode_t *chainNext;

      next = cur->freeListNext;
      while (chain)
      {
        chainNext = chain->next;
        free(chain);
        chain = chainNext;
      }
      TrisPropsSidecar_Destroy(cur);
      free(cur);
      cur = next;
    }
  }
}

int *FreeAllTriSurfs(void)
{
  /* Retained donor-facing entry point; CoD4 0x43D450 is FreeTriSurfProps. */
  FreeTriSurfProps();
  return 0;
}

/*
================
TriSurfPlanesMatch

Checks if a point has matching distance to two planes
within the given tolerance.
================
*/
bool TriSurfPlanesMatch(float *point, float *plane1, float *plane2, float tolerance)
{
  double diff;

  /* difference of signed distances from point to each plane — FPU order preserved */
  diff = DotProduct120(plane1, point) + plane1[3]
       - (DotProduct120(plane2, point) + plane2[3]);
  return diff * diff <= tolerance;
}

/*
================
TriSurfPlanesMatchScaled

Scaled plane distance match with optional wrap-around check.
================
*/
bool TriSurfPlanesMatchScaled(float *point, float *plane1, float *plane2, int scale, float tolerance, char wrapAround)
{
  long double scaledDiff;
  bool result;
  float scaleFloat;
  float wrappedDiff;

  scaleFloat = (float)scale;
  scaledDiff = fabs(DotProduct120(plane1, point) + plane1[3]
                  - (DotProduct120(plane2, point) + plane2[3]))
             * scaleFloat;
  result = 1;
  if ( scaledDiff > tolerance )
  {
    if ( !wrapAround ) {
      result = 0;
    } else {
      wrappedDiff = scaledDiff;
      if ( fabs((double)wrappedDiff - scaleFloat) > tolerance )
        result = 0;
    }
  }

  return result;
}

/*
================
CoalesceChainGroupable

Checks if two property chains are pairwise groupable
via bijective matching.
================
*/
char CoalesceChainGroupable(CoalesceNode_t *chain1, CoalesceNode_t *chain2, int *testPoints)
{
  unsigned int count1;
  CoalesceNode_t *countIter1;
  CoalesceNode_t *chain2Ptr;
  int count2;
  CoalesceNode_t *countIter2;
  CoalesceNode_t *chain1Ptr;
  int matchIdx;
  char matched[MAX_COALESCE_CHAIN_LEN];

  count1 = 0;
  for ( countIter1 = chain1; countIter1; ++count1 )
    countIter1 = countIter1->next;
  chain2Ptr = chain2;
  count2 = 0;
  for ( countIter2 = chain2; countIter2; ++count2 )
    countIter2 = countIter2->next;
  if ( count1 != count2 || count1 > MAX_COALESCE_CHAIN_LEN )
    return 0;
  memset(matched, 0, sizeof(matched));
  chain1Ptr = chain1;
  if ( !chain1 )
    return 1;
  for (;;)
  {
    for ( matchIdx = 0; ; ++matchIdx )
    {
      if ( !chain2Ptr )
        return 0;
      if ( !matched[matchIdx] && (unsigned char)TriSurfPropsGroupable((TriSurfProps_t *)chain1Ptr->value, (TriSurfProps_t *)chain2Ptr->value, testPoints) )
        break;
      chain2Ptr = chain2Ptr->next;
    }
    chain1Ptr = chain1Ptr->next;
    matched[matchIdx] = 1;
    if ( chain1Ptr )
    {
      chain2Ptr = chain2;
      continue;
    }
    break;
  }
  return 1;
}

/*
================
TriSurfPropsGroupable

Checks if two triSurfs have groupable properties
across all test points.
================
*/
char TriSurfPropsGroupable(TriSurfProps_t *s1, TriSurfProps_t *s2, int *testPoints)
{
  #define GROUP_DOT_THRESHOLD    0.9f
  #define GROUP_PLANE_DIST_SQ    0.0001f
  #define GROUP_TEX_TOLERANCE    0.5f
  #define GROUP_LMAP_TOLERANCE   0.00000095367432f
  #define GROUP_COLOR_TOLERANCE  0.25f
  int i, n;
  float *pt;

  if ( s1 == s2 )
    return 1;

  /* coalesce chain delegation */
  if ( s1->coalesceChain )
    return s2->coalesceChain ? CoalesceChainGroupable(s1->coalesceChain, s2->coalesceChain, testPoints) : 0;
  if ( s2->coalesceChain )
    return 0;

  /* basic property checks */
  if ( s1->si != s2->si || s1->lmapIndex != s2->lmapIndex
    || s1->surfFlagBit7 != s2->surfFlagBit7 || s1->contentFlagBit8 != s2->contentFlagBit8 )
    return 0;
  if ( DotProduct210(s1->plane, s2->plane) < GROUP_DOT_THRESHOLD )
    return 0;

  /* test all sample points against both planes and UV mappings */
  n = *testPoints;
  if ( n <= 0 )
    return 1;

  for ( i = 0, pt = (float *)(testPoints + 1); i < n; i++, pt += 3 )
  {
    double d;
    int noClamp;

    d = DotProduct210(pt, s2->plane) - s2->plane[3];
    if ( d * d > GROUP_PLANE_DIST_SQ ) return 0;
    d = DotProduct210(pt, s1->plane) - s1->plane[3];
    if ( d * d > GROUP_PLANE_DIST_SQ ) return 0;

    noClamp = !s2->si->globalTexture;
    if ( !TriSurfPlanesMatchScaled(pt, &s1->texVecs[0], &s2->texVecs[0], s2->si->autoTexScaleW, GROUP_TEX_TOLERANCE, noClamp) ) return 0;
    if ( !TriSurfPlanesMatchScaled(pt, &s1->texVecs[4], &s2->texVecs[4], s2->si->autoTexScaleH, GROUP_TEX_TOLERANCE, noClamp) ) return 0;

    /* Native 0x43D550 gates lmap-vector compatibility on material game flag
       bit 2.  lmapIndex is still an independent basic-property comparison
       above; it is 31 before allocation, including for lightmapped surfaces. */
    if ( (s2->si->gameFlags & 2) != 0 )
    {
      if ( !TriSurfPlanesMatch(pt, &s1->lmapVecs[0], &s2->lmapVecs[0], GROUP_LMAP_TOLERANCE) ) return 0;
      if ( !TriSurfPlanesMatch(pt, &s1->lmapVecs[4], &s2->lmapVecs[4], GROUP_LMAP_TOLERANCE) ) return 0;
    }

    { int c;
    for ( c = 0; c < 16; c += 4 )
      if ( !TriSurfPlanesMatch(pt, &s1->colorVecs[c], &s2->colorVecs[c], GROUP_COLOR_TOLERANCE) ) return 0;
    }
  }
  return 1;

  #undef GROUP_DOT_THRESHOLD
  #undef GROUP_PLANE_DIST_SQ
  #undef GROUP_TEX_TOLERANCE
  #undef GROUP_LMAP_TOLERANCE
  #undef GROUP_COLOR_TOLERANCE
}

/* CoD4 0x43DC40.  The property comparator consumes the common
   [point-count, vec3...] prefix shared by Winding_t. */
static char TriSurfPropsGroupableForWinding(TriSurfProps_t *left,
                                            TriSurfProps_t *right,
                                            Winding_t *winding)
{
  Winding_t *copy;
  char result;

  copy = CopyWinding(winding);
  result = TriSurfPropsGroupable(left, right, (int *)copy);
  FreeWinding(copy);
  return result;
}

/* CoD4 0x43DEA0. */
static float *TriEdgePlaneFromPoints(const float *from, const float *to,
                                     const float *surfacePlane, float *edgePlane)
{
  float edge[3];

  VectorSubtract(to, from, edge);
  CrossProduct((float *)surfacePlane, edge, edgePlane);
  VecNormalize(edgePlane);
  edgePlane[3] = DotProduct210(edgePlane, from);
  return edgePlane;
}

/* CoD4 0x43DD30.  The three signed edge tests deliberately preserve the
   original winding orientation rather than treating every edge symmetrically. */
static bool TriangleContainsCoplanarPoint(const float *point, Winding_t *winding,
                                          const float *surfacePlane)
{
  const float epsilon = 0.025f;
  float edgePlane[4];
  float distance;

  Assert(winding, s_assertDisable_EmitTriSurface);
  Assert(winding->numpoints == 3, s_assertDisable_EmitTriSurface);

  if ( fabsf((float)(DotProduct210(surfacePlane, point) - surfacePlane[3])) > epsilon )
    return false;

  TriEdgePlaneFromPoints(winding->points[0], winding->points[1], surfacePlane, edgePlane);
  distance = (float)(DotProduct210(edgePlane, point) - edgePlane[3]);
  if ( distance < -epsilon )
    return false;

  TriEdgePlaneFromPoints(winding->points[1], winding->points[2], surfacePlane, edgePlane);
  distance = (float)(DotProduct210(edgePlane, point) - edgePlane[3]);
  if ( distance > epsilon )
    return false;

  TriEdgePlaneFromPoints(winding->points[2], winding->points[0], surfacePlane, edgePlane);
  distance = (float)(DotProduct210(edgePlane, point) - edgePlane[3]);
  return distance <= epsilon;
}

/*
================
TriSurfGroupableCallback

Callback wrapper that extracts triSurfs from entries
and tests groupability.
================
*/
char TriSurfGroupableCallback(intptr_t entry1, intptr_t entry2)
{
  TriSurf_t *ts1 = (TriSurf_t *)entry1;
  TriSurf_t *ts2 = (TriSurf_t *)entry2;

  return TriSurfPropsGroupableForWinding(ts1->props, ts2->props, ts2->winding);
}

/* CoD4 0x4422A0.  The merge API already passes transient triSurfs directly;
   retain this typed entry point instead of casting the generic list callback. */
static int TriSurfCellGroupableCallback(TriSurf_t *ts1, TriSurf_t *ts2)
{
  return TriSurfPropsGroupableForWinding(ts1->props, ts2->props, ts2->winding);
}

/*
================
EmitTriSurface

Allocates and initializes a triSurf struct with all
surface properties from the given vectors and plane.
================
*/
TriSurfProps_t *EmitTriSurface( float *texVecs, float *lmapVecs, float *colorVecs, float *plane, ShaderInfo_t *si, int flags, int drawOrder, float smoothAngle) {
  TriSurfProps_t *props;
  float texVecT;

  Assert(plane, s_assertDisable_EmitTriSurface);
  Assert(si, s_assertDisable_EmitTriSurface);
  Assert(texVecs, s_assertDisable_EmitTriSurface);
  Assert(lmapVecs, s_assertDisable_EmitTriSurface);
  Assert(colorVecs, s_assertDisable_EmitTriSurface);
  props = malloc(sizeof(*props));
  if ( !props )
    Com_Error("Out of memory: couldn't allocate %i bytes for surface properties\n", sizeof(*props));

  props->si = si;
  props->lmapGroupId = -1;
  props->lmapIndex = 31;
  props->subdivisions = si->subdivisions;
  props->contentFlagBit8 = (drawOrder >> 8) & 1;
  props->surfFlagBit7 = si->surfFlags_bit7 != 0;
  VectorCopy(plane, props->plane);
  props->plane[3] = plane[3];
  memcpy(props->texVecs, texVecs, sizeof(props->texVecs));
  memcpy(props->lmapVecs, lmapVecs, sizeof(props->lmapVecs));
  memcpy(props->colorVecs, colorVecs, sizeof(props->colorVecs));
  props->coalesceChain = NULL;
  props->freeListNext = (TriSurfProps_t *)triSurfFreeList;
  triSurfFreeList = props;
  TrisPropsSidecar_Create(props, smoothAngle, 1, 0, 0, flags);
  if ( !si->globalTexture )
  {
    texVecT = props->texVecs[7];
    props->texVecs[3] = props->texVecs[3] - (double)fistp_sub((float)props->texVecs[3], FISTP_HALF_BIAS);
    props->texVecs[7] = props->texVecs[7] - (double)fistp_sub((float)texVecT, FISTP_HALF_BIAS);
  }
  return props;
}

/* Native 0x43DC80/0x44D3B0.  The native props representation is not KIWI's
   donor layout, but its copy semantics are: copy the full property object,
   recursively copy coalesce nodes and their property values, then retain the
   result in the normal triSurf property free list. */
static CoalesceNode_t *TrisLmap_CloneCoalesceChain(TriSurf_t *surf, CoalesceNode_t *source)
{
  CoalesceNode_t *clone;

  if (!source)
    return NULL;
  clone = (CoalesceNode_t *)malloc(sizeof(*clone));
  if (!clone)
    Com_Error("TrisLmap_CloneCoalesceChain: out of memory");
  clone->value = (intptr_t)TrisLmap_CloneProps(surf, (TriSurfProps_t *)source->value);
  clone->next = TrisLmap_CloneCoalesceChain(surf, source->next);
  return clone;
}

TriSurfProps_t *TrisLmap_CloneProps(TriSurf_t *surf, TriSurfProps_t *props)
{
  TriSurfProps_t *clone;

  Assert(surf, s_assertDisable_CopyTriSurf);
  Assert(props, s_assertDisable_CopyTriSurf);
  clone = (TriSurfProps_t *)malloc(sizeof(*clone));
  if (!clone)
    Com_Error("TrisLmap_CloneProps: out of memory");
  memcpy(clone, props, sizeof(*clone));
  clone->coalesceChain = TrisLmap_CloneCoalesceChain(surf, props->coalesceChain);
  clone->freeListNext = (TriSurfProps_t *)triSurfFreeList;
  triSurfFreeList = clone;
  TrisPropsSidecar_Clone(props, clone);
  TrisLmap_CopyAssignmentForPropsClone(surf, props, surf, clone);
  return clone;
}

/*
================
EmitTriSurfaceForDrawSurf

Emits a triSurf from a draw surface's properties.
================
*/
TriSurfProps_t *EmitTriSurfaceForDrawSurf(DrawSurf_t *ds, float *texVecs, float *lmapVecs, float *plane, float smoothAngle, float *colorVecs)
{
  Assert(ds, s_assertDisable_EmitTriSurfaceForDrawSurf);

  TriSurfProps_t *result = EmitTriSurface(texVecs, lmapVecs, colorVecs, plane, ds->shaderInfo, ds->lightStyle, ds->contentFlags, smoothAngle);
  TrisPropsSidecar_SetMapDrawSurf(result, NativeMapDrawSurfFor(ds));
  if ( ds->sideRef )
    TrisPropsSidecar_Get(result)->smoothing = ds->sideRef->compileFlags;

  return result;
}

/*
================
EmitShadowcasterTriSurface

Loads the shadowcaster material, computes the plane
from the winding, and emits a triSurf.
================
*/
TriSurfProps_t *EmitShadowcasterTriSurface(double unusedFpu, Winding_t *winding)
{
  ShaderInfo_t *shadowMaterial;
  float plane[4];

  shadowMaterial = LoadMaterial("shadowcaster");
  shadowMaterial->surfFlags_bit7 = 0;
  PlaneFromWinding(winding->numpoints, winding->points[0], plane, &plane[3]);
  return EmitTriSurface(g_shadowDummyVerts, g_shadowDummyVerts, g_shadowDummyVerts, plane, shadowMaterial, LIGHTSTYLE_NONE, 0, 0.0f);
}

/*
================
EmitTriSurface_r

Recursively clips winding against BSP tree to place
triSurf in the correct leaf.
================
*/
int EmitTriSurface_r(Winding_t *winding, TriSurfProps_t *props, Node_t *node)
{
  float *nodePlane;
  int windingSide, frontResult, backResult;
  vec3_t negPlane;
  Winding_t *w;

  if ( node->cellnum != CELLNUM_UNKNOWN )
    return node->cellnum;

  Assert(node->planenum != PLANENUM_LEAF, s_assertDisable_EmitTriSurface_r);
  nodePlane = MAP_PLANE(node->planenum)->normal;
  windingSide = WindingPlaneSide(winding, nodePlane, nodePlane[3]);

  switch ( windingSide )
  {
    case SIDE_ON:
	
      /* on the plane — choose side based on surface normal alignment */
      if ( DotProduct210(props->plane, nodePlane) <= 0.0 )
        return EmitTriSurface_r(winding, props, node->children[1]);
      return EmitTriSurface_r(winding, props, node->children[0]);
    case SIDE_FRONT:
      return EmitTriSurface_r(winding, props, node->children[0]);
    case SIDE_BACK:
      return EmitTriSurface_r(winding, props, node->children[1]);
  }

  Assert(windingSide == SIDE_CROSS, s_assertDisable_EmitTriSurface_r);

  /* SIDE_CROSS — clip winding and recurse both halves */
  w = CopyWinding(winding);
  ClipWindingByPlane(&w, nodePlane, nodePlane[3], ON_EPSILON);
  frontResult = EmitTriSurface_r(w, props, node->children[0]);
  FreeWinding(w);

  if ( frontResult == CELLNUM_UNKNOWN )
    return CELLNUM_UNKNOWN;

  w = CopyWinding(winding);
  VectorScale(nodePlane, -1, negPlane);
  ClipWindingByPlane(&w, negPlane, -nodePlane[3], ON_EPSILON);
  backResult = EmitTriSurface_r(w, props, node->children[1]);
  FreeWinding(w);

  if ( frontResult != backResult && backResult != CELLNUM_OPAQUE )
  {
    if ( frontResult == CELLNUM_OPAQUE )
      return backResult;
    return CELLNUM_UNKNOWN;
  }

  return frontResult;
}

/*
================
ludcmp

Numerical Recipes Crout LU decomposition with partial pivoting.
3x3 double matrix stored row-major. big/sum use long double
to match original 80-bit FPU precision. vv[] is 1-based (NR convention).
================
*/
#define LU_N 3
#define MAT(i,j) matrix[(i)*LU_N+(j)]

char ludcmp(double *matrix, int *perm)
{
  int i, j, k, pivotRow = 0;
  long double big, sum, temp;
  double dum;
  double vv[4];  /* 1-based: vv[1]..vv[3] */

  /* find implicit scaling of each row */
  for ( i = 0; i < LU_N; i++ )
  {
    big = 0.0;
    for ( j = 0; j < LU_N; j++ )
    {
      temp = fabs(MAT(i, j));
      if ( temp > big )
        big = temp;
    }
    if ( big == 0.0 )
      return 0;
    vv[i + 1] = 1.0 / big;
  }

  /* Crout's method: column by column */
  for ( j = 0; j < LU_N; j++ )
  {
    /* compute upper triangle elements */
    for ( i = 0; i < j; i++ )
    {
      sum = MAT(i, j);
      for ( k = 0; k < i; k++ )
        sum -= MAT(i, k) * MAT(k, j);
      MAT(i, j) = sum;
    }

    /* find pivot and compute lower triangle */
    big = 0.0;
    for ( i = j; i < LU_N; i++ )
    {
      sum = MAT(i, j);
      for ( k = 0; k < j; k++ )
        sum -= MAT(i, k) * MAT(k, j);
      MAT(i, j) = sum;

      temp = vv[i + 1] * fabs(sum);
      if ( big < temp )
      {
        big = temp;
        pivotRow = i;
      }
    }

    /* check for singular matrix */
    if ( MAT(pivotRow, j) == 0.0 )
      return 0;

    /* interchange rows if needed */
    if ( j != pivotRow )
    {
      for ( k = 0; k < LU_N; k++ )
      {
        dum = MAT(pivotRow, k);
        MAT(pivotRow, k) = MAT(j, k);
        MAT(j, k) = dum;
      }
      vv[pivotRow + 1] = vv[j + 1];
    }
    perm[j] = pivotRow;

    /* divide below-diagonal elements by pivot */
    if ( j < LU_N - 1 )
    {
      dum = 1.0 / MAT(j, j);
      for ( i = j + 1; i < LU_N; i++ )
        MAT(i, j) *= dum;
    }
  }

  return 1;
}

#undef MAT
#undef LU_N

/*
================
lubksb

LU back-substitution (Numerical Recipes in C, 2nd ed).
Solves the set of 3 linear equations L*U*x = b.
luMatrix and perm are output from ludcmp.
rhs is input as b, returns solution x.
================
*/
double *lubksb( double *luMatrix, int *perm, double *rhs )
{
  int i, ii, j, ip;
  double sum;
  double product;

  ii = -1;

  /* forward substitution: solve L*y = P*b */
  for ( i = 0; i < 3; i++ )
  {
    ip = perm[i];
    sum = rhs[ip];
    rhs[ip] = rhs[i];

    if ( ii >= 0 )
    {
      for ( j = ii; j < i; j++ )
      {
        product = luMatrix[i * 3 + j] * rhs[j];
        sum = sum - product;
      }
    }
    else if ( sum != 0.0 )
    {
      ii = i;
    }

    rhs[i] = sum;
  }

  /* back substitution: solve U*x = y */
  for ( i = 2; i >= 0; i-- )
  {
    sum = rhs[i];
    for ( j = i + 1; j < 3; j++ )
    {
      product = luMatrix[i * 3 + j] * rhs[j];
      sum = sum - product;
    }
    rhs[i] = sum / luMatrix[i * 3 + i];
  }

  return rhs;
}

/*
================
mprove

Numerical Recipes iterative improvement. Computes
residual r=Ax-b, solves via LU, and refines x.
================
*/
double *mprove(double *origMatrix, double *luMatrix, int *perm, double *origRHS, double *solution)
{
  double residual[3];

  /* compute residual: r = A*x - b */
  for (int i = 0; i < 3; i++)
  {
    residual[i] = -origRHS[i];
    for (int j = 0; j < 3; j++)
      residual[i] += origMatrix[i * 3 + j] * solution[j];
  }

  /* solve LU * dx = residual */
  lubksb(luMatrix, perm, residual);

  /* refine solution: x = x - dx */
  solution[0] = solution[0] - residual[0];
  solution[1] = solution[1] - residual[1];
  solution[2] = solution[2] - residual[2];

  return solution;
}

/*
================
SolveTextureVector

Solves for one texture vector component via LU
decomposition and iterative refinement.
================
*/
float *SolveTextureVector( double *origMatrix, double *luMatrix, int *perm, float val0, float val1, float val2, float *outVec, int idx0, int idx1, int idx2)
{
  double origValues[3];
  double workVec[3];

  workVec[0] = (double)val0;
  workVec[1] = (double)val1;
  workVec[2] = (double)val2;
  origValues[0] = (double)val0;
  origValues[1] = (double)val1;
  origValues[2] = (double)val2;

  lubksb(luMatrix, perm, workVec);
  mprove(origMatrix, luMatrix, perm, origValues, workVec);

  outVec[idx0] = (float)workVec[0];
  outVec[idx1] = (float)workVec[1];
  outVec[idx2] = 0.0f;
  outVec[3] = (float)workVec[2];

  return outVec;
}

/*
================
DeriveTextureVectors

Computes texture, lightmap, and color vectors from
triangle vertices via LU decomposition.
================
*/
/* CoD4 0x440B00.  The native reporter is deliberately nonfatal.  Its props
   carrier holds source provenance, which the donor vector-derivation API
   does not receive; the current entity is the narrowest valid fallback. */
static void ReportDegenerateTriangle(const float *triVerts)
{
  Winding_t *winding;
  int mapInfoIndex = 0;
  int entityNum = -1;

  winding = AllocWinding(3);
  winding->numpoints = 3;
  VectorCopy(triVerts, winding->points[0]);
  VectorCopy(triVerts + 3, winding->points[1]);
  VectorCopy(triVerts + 6, winding->points[2]);
  if (g_currentEntityIndex >= 0 && g_currentEntityIndex < num_entities)
  {
    mapInfoIndex = g_entities[g_currentEntityIndex].mapInfoIndex;
    entityNum = g_entities[g_currentEntityIndex].entityNum;
  }
  WindingError(0, winding, mapInfoIndex, entityNum, -1,
               "%s", "Degenerate triangle (ie, point or line).");
  FreeWinding(winding);
}

/* CoD4 0x4400D0. */
unsigned char DeriveTextureVectors(float *planeNormal, float *triVerts, float *texVecs, float *texVecOut, float *lmapVecs, float *lmapVecOut, unsigned char *colorData, float *colorVecOut)
{
  int ax1, ax2, drop, i, ch;
  double origMatrix[9], luMatrix[9];
  int perm[3];

  /* drop the axis with the largest normal component */
  drop = fabs(planeNormal[0]) < fabs(planeNormal[1]) ? 1 : 0;
  if ( fabs(planeNormal[drop]) < fabs(planeNormal[2]) )
    drop = 2;
  ax1 = (drop & 1) == 0;
  ax2 = ~(char)drop & 2;

  /* build 3x3 matrix: project each vertex onto the 2 kept axes + constant 1 */
  for ( i = 0; i < 3; i++ )
  {
    origMatrix[i * 3]     = triVerts[ax1 + i * 3];
    origMatrix[i * 3 + 1] = triVerts[ax2 + i * 3];
    origMatrix[i * 3 + 2] = 1.0;
  }
  memcpy(luMatrix, origMatrix, sizeof(luMatrix));

  if ( !ludcmp(luMatrix, perm) )
  {
    ReportDegenerateTriangle(triVerts);
    return 0;
  }

  /* solve texture S/T vectors */
  if ( texVecs )
  {
    SolveTextureVector(origMatrix, luMatrix, perm, texVecs[0], texVecs[2], texVecs[4], texVecOut,     ax1, ax2, drop);
    SolveTextureVector(origMatrix, luMatrix, perm, texVecs[1], texVecs[3], texVecs[5], texVecOut + 4, ax1, ax2, drop);
  }

  /* solve lightmap S/T vectors */
  if ( lmapVecs )
  {
    SolveTextureVector(origMatrix, luMatrix, perm, lmapVecs[0], lmapVecs[2], lmapVecs[4], lmapVecOut,     ax1, ax2, drop);
    SolveTextureVector(origMatrix, luMatrix, perm, lmapVecs[1], lmapVecs[3], lmapVecs[5], lmapVecOut + 4, ax1, ax2, drop);
  }

  /* solve RGBA color vectors (4 channels × 3 vertices, stride 4 bytes per vertex) */
  if ( colorData )
  {
    for ( ch = 0; ch < 4; ch++ )
    {
      SolveTextureVector(origMatrix, luMatrix, perm,
        (float)colorData[ch], (float)colorData[ch + 4], (float)colorData[ch + 8], colorVecOut + ch * 4,
        ax1, ax2, drop);
    }
  }

  return 1;
}

/* Native 0x440B90 keeps lightmap-vector offsets within one 512-unit tile.
   It deliberately uses floor (not the nearest-integer snapping used for
   brush texture coordinates). */
static void FloorLightmapVectorOffsets(float *lmapVecs)
{
  lmapVecs[3] -= floorf(lmapVecs[3] * 512.0f) * (1.0f / 512.0f);
  lmapVecs[7] -= floorf(lmapVecs[7] * 512.0f) * (1.0f / 512.0f);
}

/*
================
PickBestTriangle

Selects the best non-degenerate triangle from the vertex
set for texture vector derivation.
================
*/
#define VERT_STRIDE 11  /* xyz(3) + st(2) + lmSt(2) + normal(3) + color(1) */

float *PickBestTriangle(float *normal, float *verts, int numVerts, float planeDist, float *outPositions, float *outTexCoords, float *outLmapCoords)
{
  int i, j, k, best[3] = {0, 0, 0};
  float bestScore = 0.0f;
  vec3_t edge1, edge2, cross;
  float *v;

  /* find the vertex triple whose cross product best aligns with the plane normal */
  for ( i = 2; i < numVerts; i++ )
  {
    for ( j = 1; j < i; j++ )
    {
      VectorSubtract(&verts[i * VERT_STRIDE], &verts[j * VERT_STRIDE], edge2);
      for ( k = 0; k < j; k++ )
      {
        VectorSubtract(&verts[k * VERT_STRIDE], &verts[j * VERT_STRIDE], edge1);
        CrossProduct(edge1, edge2, cross);
        { double score = DotProduct(cross, normal);
        if ( score > bestScore )
        {
          bestScore = score;
          best[0] = k;
          best[1] = j;
          best[2] = i;
        } }
      }
    }
  }

  /* extract position, texcoord, and lightmap for each vertex of the best triangle */
  for ( i = 0; i < 3; i++ )
  {
    v = &verts[best[i] * VERT_STRIDE];
    VectorCopy(v, &outPositions[i * 3]);
    outTexCoords[i * 2]      = v[3];
    outTexCoords[i * 2 + 1]  = v[4];
    outLmapCoords[i * 2]     = v[5];
    outLmapCoords[i * 2 + 1] = v[6];
  }
  return v;
}

#undef VERT_STRIDE

/*
================
SurfacesCoplanar

Checks if two surfaces are coplanar within tolerance
by verifying normal dot product and vertex distances.
================
*/
char SurfacesCoplanar(float *normal1, Winding_t *winding1, float *normal2, float dist1, Winding_t *winding2, float dist2)
{
  int i;

  /* local thresholds — different from header's broader COPLANAR_DOT_THRESHOLD (0.999 vs 0.99) */
  #undef COPLANAR_DOT_THRESHOLD
  #define COPLANAR_DOT_THRESHOLD 0.99900001f
  #define COPLANAR_DIST_EPSILON  0.001f

  if ( DotProduct120(normal1, normal2) < COPLANAR_DOT_THRESHOLD
    || fabs(dist1 - dist2) > COPLANAR_DIST_EPSILON )
    return 0;

  for ( i = 0; i < winding1->numpoints; i++ )
  {
    if ( DotProduct021(winding1->points[i], normal1) - dist2 > COPLANAR_DIST_EPSILON )
      return 0;
  }

  for ( i = 0; i < winding2->numpoints; i++ )
  {
    if ( DotProduct021(winding2->points[i], normal2) - dist1 > COPLANAR_DIST_EPSILON )
      return 0;
  }

  #undef COPLANAR_DOT_THRESHOLD
  #undef COPLANAR_DIST_EPSILON

  return 1;
}

/*
================
TexVecsMatch

Checks if all winding vertices produce matching UVs
from two texture vector sets within tolerance.
================
*/
static float TrisDotProductFloat(const float *a, const float *b)
{
  return (float)(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
}

char TexVecsMatch(Winding_t *winding, float *texVec1, float *texVec2, float scale)
{
  int i;
  float *v;
  float uv1, uv2;

  for ( i = 0; i < winding->numpoints; i++ )
  {
    v = winding->points[i];
    uv1 = TrisDotProductFloat(v, texVec1) + texVec1[3];
    uv2 = TrisDotProductFloat(v, texVec2) + texVec2[3];
    if ( fabsf(uv1 - uv2) * scale > PLANESIDE_EPSILON )
      return 0;
  }
  return 1;
}

/* CoD4 0x440E30: compare both windings against a pair of vector planes. */
static char TexVecsMatchBoth(Winding_t *winding0, Winding_t *winding1,
                             float *vec0, float *vec1, float scale)
{
  return TexVecsMatch(winding0, vec0, vec1, scale)
      && TexVecsMatch(winding1, vec0, vec1, scale);
}

/* CoD4 0x440C00.  Patch triangles form a quad only when their planes and
   every generated texture, lightmap, and color vector agree on both
   windings. */
static char PatchWindingPairMatches(const DrawSurf_t *surf, Winding_t *windings[2],
                                    float *planes, float *texVecs,
                                    float *lmapVecs, float *colorVecs)
{
  int channel;

  if (!SurfacesCoplanar(planes + 4, windings[0], planes, planes[3],
                         windings[1], planes[7]))
    return 0;
  if (!TexVecsMatchBoth(windings[0], windings[1], texVecs, texVecs + 8,
                         (float)surf->shaderInfo->autoTexScaleW))
    return 0;
  if (!TexVecsMatchBoth(windings[0], windings[1], texVecs + 4, texVecs + 12,
                         (float)surf->shaderInfo->autoTexScaleH))
    return 0;
  if (!TexVecsMatchBoth(windings[0], windings[1], lmapVecs, lmapVecs + 8, 512.0f))
    return 0;
  if (!TexVecsMatchBoth(windings[0], windings[1], lmapVecs + 4, lmapVecs + 12, 512.0f))
    return 0;

  for (channel = 0; channel < 4; ++channel)
  {
    if (!TexVecsMatchBoth(windings[0], windings[1], colorVecs + channel * 4,
                           colorVecs + 16 + channel * 4, 1.0f / 255.0f))
      return 0;
  }
  return 1;
}

/* CoD4 0x441320.  The candidate fourth terrain vertex must reproduce the
   first triangle's generated attributes before the two triangles can become
   one quad.  Color conversion is the native truncation-with-tiny-bias path. */
static char TerrainQuadVertexMatches(const ShaderInfo_t *si, const MeshVert_t *vertex,
                                     const float *texVecs, const float *lmapVecs,
                                     const float *colorVecs)
{
  const unsigned char *color = (const unsigned char *)&vertex->color;
  float value;
  int channel;

  value = TrisDotProductFloat(vertex->pos, texVecs) + texVecs[3] - vertex->st[0];
  if (fabsf(value) * (float)si->autoTexScaleW > 0.25f)
    return 0;
  value = TrisDotProductFloat(vertex->pos, texVecs + 4) + texVecs[7] - vertex->st[1];
  if (fabsf(value) * (float)si->autoTexScaleH > 0.25f)
    return 0;
  value = TrisDotProductFloat(vertex->pos, lmapVecs) + lmapVecs[3] - vertex->lmSt[0];
  if (fabsf(value) > 0.00048828125f)
    return 0;
  value = TrisDotProductFloat(vertex->pos, lmapVecs + 4) + lmapVecs[7] - vertex->lmSt[1];
  if (fabsf(value) > 0.00048828125f)
    return 0;

  for (channel = 0; channel < 4; ++channel)
  {
    value = TrisDotProductFloat(vertex->pos, colorVecs + channel * 4)
          + colorVecs[channel * 4 + 3];
    if ((int)(value + 9.313225746154785e-10f) != color[channel])
      return 0;
  }
  return 1;
}

/*
================
CoalesceTriSurfCells

CoD4 0x441B40: coalesces every active tri-surface cell.
================
*/
int CoalesceTriSurfCells(void)
{
  int cellCount;
  TriSurf_t **cellPtr;
  int i;

  if ( g_currentEntityIndex <= 0 )
    cellCount = numBSPCullGroups + numBSPCells;
  else
    cellCount = 1;

  cellPtr = triSurfCellArray;
  for ( i = 0; i < cellCount; i++ )
    CoalesceVisGroup(cellPtr++);
  return cellCount;
}

/* CoD4 0x441B90.  Native validates a copy through the opaque-brush tree;
   the validator owns that copy and returns false only when no visible
   fragment remains.  Keep the link pointer on deletion, just as the native
   list walk does. */
int RemoveInvalidTriSurfs(void)
{
  int cellCount;
  int cell;

  if (g_currentEntityIndex <= 0)
    cellCount = numBSPCullGroups + numBSPCells;
  else
    cellCount = 1;

  for (cell = 0; cell < cellCount; ++cell)
  {
    TriSurf_t **link = &triSurfCellArray[cell];
    while (*link)
    {
      TriSurf_t *surf = *link;
      Winding_t *windingCopy = CopyWinding(surf->winding);

      if (BrushSides_IsWindingVisible(windingCopy))
      {
        link = &surf->next;
      }
      else
      {
        *link = surf->next;
        FreeTriSurf(surf);
      }
    }
  }
  return cellCount;
}

/*
================
TrisCheckLightmapCellMatch

Checks if two surfaces occupy the same lightmap cell
on a given axis.
================
*/
bool TrisCheckLightmapCellMatch( int axis, TriSurf_t *ts1, TriSurf_t *ts2, float cellSize )
{
  float inv = 1.0f / cellSize;

  #define LMAP_CELL_EPSILON 0.0020000001f
  float min1 = MULF(inv, ts1->mins[axis], LMAP_CELL_EPSILON);
  float min2 = MULF(inv, ts2->mins[axis], LMAP_CELL_EPSILON);
  if ( fistp_sub(min1, -FISTP_HALF_BIAS) != fistp_sub(min2, -FISTP_HALF_BIAS) )
    return 0;

  float max1 = MULF(inv, ts1->maxs[axis], -LMAP_CELL_EPSILON);
  float max2 = MULF(inv, ts2->maxs[axis], -LMAP_CELL_EPSILON);
  return fistp_sub(max1, FISTP_HALF_BIAS) == fistp_sub(max2, FISTP_HALF_BIAS);
}

/*
================
TrisCanCoalesceWindings

Checks if two surfaces can be coalesced by verifying
bounds intersection and same lightmap cell.
================
*/
bool TrisCanCoalesceWindings(TriSurf_t *surf1, TriSurf_t *surf2)
{
  float cellSize;

  if ( !BoundsIntersect(surf1->mins, surf1->maxs, surf2->mins, surf2->maxs) )
    return 0;

  cellSize = surf1->props->subdivisions;
  if ( cellSize == 0.0f )
    return 1;

  /* all 3 axes must map to the same lightmap cell */
  return TrisCheckLightmapCellMatch(0, surf1, surf2, cellSize)
      && TrisCheckLightmapCellMatch(1, surf1, surf2, cellSize)
      && TrisCheckLightmapCellMatch(2, surf1, surf2, cellSize);
}

/*
================
TrisComputeWindingBounds

Computes per-cell bounding boxes from all surface
windings, expanding by 0.2 units.
================
*/
int TrisComputeWindingBounds(void)
{
  int cellIdx;
  TriSurf_t *ts;
  int cellCount;

  if ( g_currentEntityIndex <= 0 )
    cellCount = numBSPCells + numBSPCullGroups;
  else
    cellCount = 1;

  for ( cellIdx = 0; cellIdx < cellCount; cellIdx++ )
  {
    ClearBounds(cellBoundsData[cellIdx].mins, cellBoundsData[cellIdx].maxs);

    for ( ts = triSurfCellArray[cellIdx]; ts; ts = ts->next )
    {
      if ( !ts->props->surfFlagBit7 )
      {
        #define TJUNC_BOUNDS_EXPAND 0.2f
#define MERGE_EPSILON 0.050000001f  /* geometric tolerance for coalesce/merge plane tests */
        WindingBounds(ts->winding, ts->mins, ts->maxs);
        { int ax;
        for ( ax = 0; ax < 3; ax++ )
        {
          ts->mins[ax] -= TJUNC_BOUNDS_EXPAND;
          ts->maxs[ax] += TJUNC_BOUNDS_EXPAND;
        } }
        AddBoundsToBounds(ts->mins, ts->maxs, cellBoundsData[cellIdx].mins, cellBoundsData[cellIdx].maxs);
      }
    }
  }

  return cellCount;
}

/* CoD4 0x4425A0.
================
TrisSetupTJunctionKDTree

Initializes the T-junction KD tree and processes
all brushes for T-junction fixing.
================
*/
void TrisSetupTJunctionKDTree(void)
{
  int cellCount, i;
  vec3_t treeMins, treeMaxs;

  if ( g_currentEntityIndex <= 0 )
    cellCount = numBSPCullGroups + numBSPCells;
  else
    cellCount = 1;

  /* compute tree bounds from all cell bounds */
  ClearBounds(treeMins, treeMaxs);
  for ( i = 0; i < cellCount; i++ )
    AddBoundsToBounds(cellBoundsData[i].mins, cellBoundsData[i].maxs, treeMins, treeMaxs);

  SetGridDivisionPoints(treeMins, treeMaxs);

  for ( i = 0; i < cellCount; i++ )
    TJunc_ProcessBrushList(triSurfCellArray[i]);

  TJunc_Init(0);
  TJunc_MergeScratchInit();
  #define TJUNC_SNAP_TOLERANCE_INIT  0.15000001f
  #define TJUNC_SNAP_TOLERANCE_FINAL 0.1f
  TJunc_SetSnapTolerance(TJUNC_SNAP_TOLERANCE_INIT);
  TJunc_SetCreateNonAxial(1);
  TjuncFixAll(treeMins, treeMaxs);
  TJunc_SetSnapTolerance(TJUNC_SNAP_TOLERANCE_FINAL);
  TJunc_MergeScratchShutdown();

}

/*
================
TrisGenerateVerticesFromWinding

Generates vertex data (position, UV, color, normal)
from a winding using the triSurf's texture vectors.
================
*/
static TriSurfProps_t *g_trisGenVertsCount;
static TriSurf_t *g_trisGenVertsSurf;

int TrisGenerateVerticesFromWinding(int a1_unused, Winding_t *winding)
{
  TriSurfProps_t *props = g_trisGenVertsCount;
  const TrisLmapAssignmentPayload_t *assignment;
  const float *lmapVecs;
  float *pos;
  DrawVert_t *dv;
  int i;

  assignment = g_trisGenVertsSurf
      ? TrisLmap_GetAssignment(g_trisGenVertsSurf, props) : NULL;
  lmapVecs = assignment ? &assignment->vecs[0][0] : &props->lmapVecs[0];

  for ( i = 0; i < winding->numpoints; i++ )
  {
    pos = winding->points[i];
    dv = &drawVertBuffer[numDrawVertices++];

    VectorCopy(pos, dv->pos);
    VectorCopy(pos, dv->faceNormal);
    VectorCopy(props->plane, dv->normal);

    /* texture UV = dot(pos, texVecS/T) + offset */
    dv->uv[0] = DotProduct021(pos, &props->texVecs[0]) + props->texVecs[3];
    dv->uv[1] = DotProduct210(pos, &props->texVecs[4]) + props->texVecs[7];


    /* lightmap UV = dot(pos, lmapVecS/T) + offset, clamped to [0,1] */
    dv->lmUv[0] = (float)I_fclamp(DotProduct210(pos, lmapVecs) + lmapVecs[3], 0.0, 1.0);
    dv->lmUv[1] = (float)I_fclamp(DotProduct021(pos, lmapVecs + 4) + lmapVecs[7], 0.0, 1.0);

    /* post-clamp: double→float cast may push boundary values slightly
       outside [0,1], so re-clamp the stored floats */
    if (dv->lmUv[0] < 0.0f) dv->lmUv[0] = 0.0f;
    else if (dv->lmUv[0] > 1.0f) dv->lmUv[0] = 1.0f;
    if (dv->lmUv[1] < 0.0f) dv->lmUv[1] = 0.0f;
    else if (dv->lmUv[1] > 1.0f) dv->lmUv[1] = 1.0f;
  }
  return i;
}

/*
================
TrisGenerateAllVertices

Generates vertices for all surfaces in all cells by
iterating surfaces and calling TrisGenerateVerticesFromWinding.
================
*/
int TrisGenerateAllVertices(void)
{
  TriSurf_t *ts;
  CoalesceNode_t *propsChain;
  int i;
  int cellCount;

  if ( g_currentEntityIndex <= 0 )
    cellCount = numBSPCells + numBSPCullGroups;
  else
    cellCount = 1;

  for ( i = 0; i < cellCount; i++ )
  {
    for ( ts = triSurfCellArray[i]; ts; ts = ts->next )
    {
      g_trisGenVertsSurf = ts;
      propsChain = ts->props->coalesceChain;
      if ( propsChain )
      {
        for ( ; propsChain; propsChain = propsChain->next )
        {
          g_trisGenVertsCount = (TriSurfProps_t *)propsChain->value;
          TrisGenerateVerticesFromWinding(0, ts->winding);
        }
      }
      else
      {
        g_trisGenVertsCount = ts->props;
        TrisGenerateVerticesFromWinding(0, ts->winding);
      }
    }
  }
  g_trisGenVertsSurf = NULL;
  return cellCount;
}

/*
================
TrisSnapNearbyVertices

Snaps nearby vertices together using spatial proximity
checks with projection axes.
================
*/
static int *g_trisSnapCount;

/* CoD4 0x442C70.  Collapse every later representative that names `second`
   onto the lower representative.  The caller supplies two map values rather
   than vertex indices, exactly as the native nearby-vertex loop does. */
static int Tris_MergeVertexMapEntries(int first, int second, int *indexMap)
{
  int index;

  if (second < first)
  {
    int swap = first;

    first = second;
    second = swap;
  }
  for (index = second; index < numDrawVertices; ++index)
  {
    if (indexMap[index] == second)
      indexMap[index] = first;
  }
  return index;
}

void TrisSnapNearbyVertices( int a1_unused )
{
  int *indexMap = g_trisSnapCount;
  int cellIdx, cellCount;
  int firstVertIndex;
  TriSurf_t *ts;

  cellCount = (g_currentEntityIndex <= 0) ? numBSPCells + numBSPCullGroups : 1;
  firstVertIndex = triFirstVertIndex;

  for ( cellIdx = 0; cellIdx < cellCount; cellIdx++ )
  {
    for ( ts = triSurfCellArray[cellIdx]; ts; ts = ts->next )
    {
      int numVerts = ts->winding->numpoints;
      int propsCount, projAxis1, projAxis2;
      int outer, inner, p;
      CoalesceNode_t *chain;
      double d1, d2;

      GetProjectionAxes(ts->props->plane, &projAxis1, &projAxis2);

      /* count coalesce chain entries */
      propsCount = 0;
      for ( chain = ts->props->coalesceChain; chain; chain = chain->next )
        propsCount++;
      if ( !propsCount )
        propsCount = 1;
      Assert(propsCount >= 1, s_assertDisable_TrisSnapNearbyVertices);

      /* for each pair of vertices on this surface, snap if within threshold */
      for ( outer = 0; outer < numVerts - 1; outer++ )
      {
        int outerGlobal = firstVertIndex + outer;

        for ( inner = outer + 1; inner < numVerts; inner++ )
        {
          int innerGlobal = firstVertIndex + inner;

          if ( indexMap[outerGlobal] == indexMap[innerGlobal] )
            continue;

          d1 = drawVertBuffer[outerGlobal].pos[projAxis1] - drawVertBuffer[innerGlobal].pos[projAxis1];
          d2 = drawVertBuffer[outerGlobal].pos[projAxis2] - drawVertBuffer[innerGlobal].pos[projAxis2];

          #define SNAP_DIST_SQ 0.0000019083022f
          if ( MulAdd2(d2,d2, d1,d1) >= SNAP_DIST_SQ )
            continue;

          /* merge index maps across all props layers */
          for ( p = 0; p < propsCount; p++ )
          {
            int layerOfs = p * numVerts;
            int a = indexMap[layerOfs + innerGlobal];
            int b = indexMap[layerOfs + outerGlobal];

            Tris_MergeVertexMapEntries(a, b, indexMap);
          }
        }
      }

      firstVertIndex += propsCount * numVerts;
    }
  }
  Assert(firstVertIndex == numDrawVertices, s_assertDisable_TrisSnapNearbyVertices);
}

/*
================
TrisSnapWindingToVertices

Snaps winding points to nearby vertices and splits edges as needed
================
*/
void TrisSnapWindingToVertices(int *indexMap)
{
  int cellCount;
  int cellIdx;
  int numSnappedVerts;
  int vertIdx;
  CoalesceNode_t *propsNode;
  int propsCount;
  #define MAX_SNAP_WINDING_VERTS 0x4000
  int snappedWinding[MAX_SNAP_WINDING_VERTS];
  TriSurf_t **surfLinkPtr;
  int firstVertIndex;

  if ( g_currentEntityIndex <= 0 )
    cellCount = numBSPCullGroups + numBSPCells;
  else
    cellCount = 1;
  Assert(trisTransientMode == 1, s_assertDisable_TrisSnapWindingToVertices);
  firstVertIndex = triFirstVertIndex;

  for ( cellIdx = 0; cellIdx < cellCount; cellIdx++ )
  {
    surfLinkPtr = &triSurfCellArray[cellIdx];
    while ( *surfLinkPtr )
    {
      TriSurf_t *ts = *surfLinkPtr;

      Assert(ts->winding->numpoints <= MAX_SNAP_WINDING_VERTS, s_assertDisable_TrisSnapWindingToVertices);
      numSnappedVerts = SnapAndMergeWinding(firstVertIndex, ts->winding->numpoints, indexMap, snappedWinding, (char *)ts->auxData, ts->auxElemSize);
      ts->origWinding = AllocWinding(numSnappedVerts);

      { Winding_t *w = ts->winding;
      Winding_t *ow = ts->origWinding;

      /* copy snapped vertex positions and face normals */
      for ( vertIdx = 0; vertIdx < numSnappedVerts; vertIdx++ )
      {
        VectorCopy(DRAW_VERT(snappedWinding[vertIdx]).pos, w->points[vertIdx]);
        VectorCopy(DRAW_VERT(snappedWinding[vertIdx]).faceNormal, ow->points[vertIdx]);
      }

      /* count coalesce chain entries */
      propsCount = 0;
      for ( propsNode = ts->props->coalesceChain; propsNode; propsNode = propsNode->next )
        propsCount++;
      if ( !propsCount )
        propsCount = 1;
      Assert(propsCount >= 1, s_assertDisable_TrisSnapWindingToVertices);
      firstVertIndex += propsCount * w->numpoints;

      if ( numSnappedVerts >= 3 )
      {
        w->numpoints = numSnappedVerts;
        ow->numpoints = numSnappedVerts;
        surfLinkPtr = &ts->next;
      }
      else
      {
        *surfLinkPtr = ts->next;
        FreeTriSurf(ts);
      }
      } /* Winding_t scope */
    }
  }
  Assert(firstVertIndex == numDrawVertices, s_assertDisable_TrisSnapWindingToVertices);
}

/*
================
TrisTimerCheck

Prints elapsed time if >= 5 seconds since last mark
================
*/
void TrisTimerCheck(void *this)
{
  double elapsed;

  if ( g_trisTimerStart != 0.0 )
  {
    elapsed = I_FloatTime() - g_trisTimerStart;
    if ( elapsed >= 5.0 )
      printf("finished in %.0lf seconds\n", elapsed);
    g_trisTimerStart = 0.0;
  }
}

/*
================
SmoothVertexNormalsForGroup

Smooths vertex normals by averaging across a sorted vertex group
================
*/
void SmoothVertexNormalsForGroup(int *sortedVerts, int groupSize)
{
  int i, j;

  /* Native 0x444A50 carries the map's smoothing enum in the transient
     104-byte draw vertex.  Only smoothing_smooth (1) requests averaging;
     smoothing_smooth_other (2) contributes to a type-1 vertex but retains
     its own face normal, and smoothing_hard (0) always retains its face
     normal.  This also preserves the original signed-zero provenance. */
  for ( i = 0; i < groupSize; ++i )
  {
    DrawVert_t *ref = &DRAW_VERT(sortedVerts[i]);
    vec3_t sameMean;
    vec3_t otherMean;

    if ( ref->smoothing != 1 )
    {
      VectorCopy(ref->normal, ref->smoothNormal);
      continue;
    }

    VectorClear(sameMean);
    VectorClear(otherMean);
    for ( j = 0; j < groupSize; ++j )
    {
      DrawVert_t *other = &DRAW_VERT(sortedVerts[j]);
      if ( DotProduct210(ref->normal, other->normal) < (double)smoothAngle )
        continue;

      if ( other->smoothing == 2 )
        VectorAdd(otherMean, other->normal, otherMean);
      else if ( other->smoothing == 1 )
        VectorAdd(sameMean, other->normal, sameMean);
    }

    if ( DotProduct(otherMean, otherMean) > 0.0 )
      VectorCopy(otherMean, sameMean);
    Assert(DotProduct(sameMean, sameMean) > 0.0, s_assertDisable_SmoothVertexNormals);
    VecNormalize(sameMean);
    VectorCopy(sameMean, ref->smoothNormal);
  }
}

/*
================
CompareVertsByIndexMap

Qsort comparator that orders vertices by their index map values
================
*/
int CompareVertsByIndexMap(int *a1, int *a2)
{
  return triVertexIndexMap[a1[0]] - triVertexIndexMap[a2[0]];
}

/*
================
SmoothVertexNormals

Sorts vertices by map index and smooths normals per group
================
*/
void SmoothVertexNormals(int *indexMap)
{
  int numSortVerts;
  int *sortedVerts;
  int vertIdx;
  int thisIndexMap;
  int i;

  /* build sorted vertex index array */
  numSortVerts = numDrawVertices - triFirstVertIndex;
  sortedVerts = malloc(sizeof(*sortedVerts) * numSortVerts);
  for ( i = 0; i < numSortVerts; i++ )
    sortedVerts[i] = triFirstVertIndex + i;

  /* sort by index map value to group coincident vertices */
  AssertFatal(!triVertexIndexMap, s_assertDisable_SmoothVertexNormals);
  triVertexIndexMap = indexMap;
  qsort(sortedVerts, numSortVerts, sizeof(int), (int (*)(const void *, const void *))CompareVertsByIndexMap);
  triVertexIndexMap = 0;

  /* smooth normals for each group of coincident vertices */
  vertIdx = 0;
  while ( vertIdx < numSortVerts )
  {
    thisIndexMap = indexMap[sortedVerts[vertIdx]];
    Assert(!vertIdx || thisIndexMap > indexMap[sortedVerts[vertIdx - 1]], s_assertDisable_SmoothVertexNormals);

    /* find end of group */
    for ( i = vertIdx + 1; i < numSortVerts; i++ )
    {
      if ( indexMap[sortedVerts[i]] != thisIndexMap )
        break;
    }
    SmoothVertexNormalsForGroup(&sortedVerts[vertIdx], i - vertIdx);
    vertIdx = i;
  }
  free(sortedVerts);
}

/*
================
CompareTrisByMaterialAndVertexIndices

Qsort comparator that orders triangles by material index, then by vertex indices
================
*/
int CompareTrisByMaterialAndVertexIndices( int *a, int *b )
{
  TriRecord_t *tri0 = (TriRecord_t *)a;
  TriRecord_t *tri1 = (TriRecord_t *)b;
  int d;

  d = tri0->drawOrder - tri1->drawOrder;
  if ( d ) return d;
  d = tri0->cullGroupIdx - tri1->cullGroupIdx;
  if ( d ) return d;
  d = (int)((char *)tri0->si - (char *)tri1->si);
  if ( d ) return d;
  d = (int)tri0->reflectionProbeIndex - (int)tri1->reflectionProbeIndex;
  if ( d ) return d;
  d = (int)tri0->primaryLightIndex - (int)tri1->primaryLightIndex;
  if ( d ) return d;
  d = (int)tri0->castsSunShadow - (int)tri1->castsSunShadow;
  if ( d ) return d;
  return tri0->lightStyle - tri1->lightStyle;
}

/*
 * CoD4 0x444D20 invokes VS2005's std::sort wrapper (0x44C1C0) over a
 * 96-byte direct-triangle record.  KIWI's TriRecord_t is intentionally
 * wider, so never sort it as though it were the native carrier.  This
 * adapter preserves the native key offsets and carries only a source-record
 * index; the resulting permutation is applied back to the expanded records.
 * It is deliberately not a 0x449BB0 combine carrier: native 0x44F250 needs
 * lyrMtlDesc at +8, three direct-vertex indices at +12/+16/+20, and the
 * referenced 104-byte multi-UV/weight/color vertices.  None exist in this
 * sort-only adapter or in TriRecord_t's single-layer DrawVert_t payload.
 *
 * The <=32 native path was deliberately not substituted for KIWI's qsort:
 * doing so regressed the sealed oracle because the expanded record does not
 * yet preserve all native direct-emission semantics.  Only the >32 path uses
 * this isolated carrier.
 */
typedef struct NativeDirectTriSortCarrier_s {
  short         visGroup;       /* [0] native signed short key */
  unsigned short pad2;          /* [2] */
  unsigned char key4;           /* [4] */
  unsigned char key5;           /* [5] */
  unsigned char key6;           /* [6] */
  unsigned char key7;           /* [7] optional sun-shadow key */
  ShaderInfo_t *material;       /* [8] */
  int           sourceIndex;    /* [12] adapter-only payload */
  unsigned char unused[80];     /* [16..95] native carrier extent */
} NativeDirectTriSortCarrier_t;

typedef char static_assert_native_direct_tri_sort_carrier_size[
  sizeof(NativeDirectTriSortCarrier_t) == 0x60 ? 1 : -1];

static bool NativeDirectTriCarrierLess(const NativeDirectTriSortCarrier_t *left,
                                       const NativeDirectTriSortCarrier_t *right)
{
  if (left->visGroup != right->visGroup)
    return left->visGroup < right->visGroup;
  if (left->material != right->material)
    return (uintptr_t)left->material < (uintptr_t)right->material;
  if (left->key4 != right->key4)
    return left->key4 < right->key4;
  if (left->key6 != right->key6)
    return left->key6 < right->key6;
  if (left->key5 != right->key5)
    return left->key5 < right->key5;
  if (left->unused[0] != right->unused[0])
    return left->unused[0] < right->unused[0];
  return left->key7 < right->key7;
}

static void NativeDirectTriCarrierSwap(NativeDirectTriSortCarrier_t *left,
                                       NativeDirectTriSortCarrier_t *right)
{
  NativeDirectTriSortCarrier_t held = *left;
  *left = *right;
  *right = held;
}

/* 0x44CE60: place the median of the three selected records in the middle. */
static void NativeDirectTriCarrierMedian(NativeDirectTriSortCarrier_t *records,
                                         int first, int mid, int last)
{
  if (NativeDirectTriCarrierLess(&records[mid], &records[first]))
    NativeDirectTriCarrierSwap(&records[mid], &records[first]);
  if (NativeDirectTriCarrierLess(&records[last], &records[mid]))
    NativeDirectTriCarrierSwap(&records[last], &records[mid]);
  if (NativeDirectTriCarrierLess(&records[mid], &records[first]))
    NativeDirectTriCarrierSwap(&records[mid], &records[first]);
}

/* 0x44CA90: median-of-three, or Tukey's ninther for ranges above 40. */
static void NativeDirectTriCarrierGuessMedian(NativeDirectTriSortCarrier_t *records,
                                              int first, int mid, int last)
{
  int count = last - first + 1;

  if (count > 40)
  {
    int step = (count + 1) >> 3;
    int twoStep = step << 1;
    NativeDirectTriCarrierMedian(records, first, first + step, first + twoStep);
    NativeDirectTriCarrierMedian(records, mid - step, mid, mid + step);
    NativeDirectTriCarrierMedian(records, last - twoStep, last - step, last);
    NativeDirectTriCarrierMedian(records, first + step, mid, last - step);
  }
  else
  {
    NativeDirectTriCarrierMedian(records, first, mid, last);
  }
}

/* 0x44C950: the VS2005 std::sort small-range insertion tail. */
static void NativeDirectTriCarrierInsertionSort(NativeDirectTriSortCarrier_t *records,
                                                int first, int last)
{
  int mid;

  for (mid = first + 1; mid < last; ++mid)
  {
    NativeDirectTriSortCarrier_t value = records[mid];
    int hole = mid;

    if (NativeDirectTriCarrierLess(&value, &records[first]))
    {
      do
      {
        records[hole] = records[hole - 1];
        --hole;
      } while (hole > first);
    }
    else
    {
      while (hole > first && NativeDirectTriCarrierLess(&value, &records[hole - 1]))
      {
        records[hole] = records[hole - 1];
        --hole;
      }
    }
    records[hole] = value;
  }
}

/* 0x44D040 / 0x44D0A0: heap fallback used after the division budget. */
static void NativeDirectTriCarrierPopHeapHole(NativeDirectTriSortCarrier_t *records,
                                              int hole, int bottom,
                                              NativeDirectTriSortCarrier_t value)
{
  int top = hole;
  int index = hole;
  int maxNonLeaf = (bottom - 1) >> 1;

  while (index < maxNonLeaf)
  {
    index = 2 * index + 2;
    if (NativeDirectTriCarrierLess(&records[index], &records[index - 1]))
      --index;
    records[hole] = records[index];
    hole = index;
  }
  if (index == maxNonLeaf && !(bottom & 1))
  {
    records[hole] = records[bottom - 1];
    hole = bottom - 1;
  }
  while (top < hole)
  {
    int parent = (hole - 1) >> 1;
    if (!NativeDirectTriCarrierLess(&records[parent], &value))
      break;
    records[hole] = records[parent];
    hole = parent;
  }
  records[hole] = value;
}

static void NativeDirectTriCarrierHeapSort(NativeDirectTriSortCarrier_t *records,
                                           int first, int last)
{
  int count = last - first;
  int hole;

  for (hole = count >> 1; hole > 0; )
  {
    --hole;
    NativeDirectTriCarrierPopHeapHole(records + first, hole, count,
      records[first + hole]);
  }
  for (hole = count; hole >= 2; --hole)
  {
    NativeDirectTriSortCarrier_t value = records[first + hole - 1];
    records[first + hole - 1] = records[first];
    NativeDirectTriCarrierPopHeapHole(records + first, 0, hole - 1, value);
  }
}

/* 0x44C590: three-way partition around the selected median. */
static void NativeDirectTriCarrierPartition(NativeDirectTriSortCarrier_t *records,
                                            int first, int last,
                                            int *outFirst, int *outLast)
{
  int mid = first + ((last - first) >> 1);
  int pfirst;
  int plast;
  int gfirst;
  int glast;

  NativeDirectTriCarrierGuessMedian(records, first, mid, last - 1);
  pfirst = mid;
  plast = pfirst + 1;

  while (first < pfirst
    && !NativeDirectTriCarrierLess(&records[pfirst - 1], &records[pfirst])
    && !NativeDirectTriCarrierLess(&records[pfirst], &records[pfirst - 1]))
    --pfirst;
  while (plast < last
    && !NativeDirectTriCarrierLess(&records[plast], &records[pfirst])
    && !NativeDirectTriCarrierLess(&records[pfirst], &records[plast]))
    ++plast;

  gfirst = plast;
  glast = pfirst;
  for (;;)
  {
    for (; gfirst < last; ++gfirst)
    {
      if (NativeDirectTriCarrierLess(&records[pfirst], &records[gfirst]))
        continue;
      if (NativeDirectTriCarrierLess(&records[gfirst], &records[pfirst]))
        break;
      if (plast != gfirst)
        NativeDirectTriCarrierSwap(&records[plast], &records[gfirst]);
      ++plast;
    }
    for (; first < glast; --glast)
    {
      int previous = glast - 1;
      if (NativeDirectTriCarrierLess(&records[previous], &records[pfirst]))
        continue;
      if (NativeDirectTriCarrierLess(&records[pfirst], &records[previous]))
        break;
      --pfirst;
      if (pfirst != previous)
        NativeDirectTriCarrierSwap(&records[pfirst], &records[previous]);
    }
    if (glast == first && gfirst == last)
      break;
    if (glast == first)
    {
      if (plast != gfirst)
        NativeDirectTriCarrierSwap(&records[pfirst], &records[plast]);
      ++plast;
      NativeDirectTriCarrierSwap(&records[pfirst], &records[gfirst]);
      ++pfirst;
      ++gfirst;
    }
    else if (gfirst == last)
    {
      --glast;
      --pfirst;
      if (glast != pfirst)
        NativeDirectTriCarrierSwap(&records[glast], &records[pfirst]);
      --plast;
      NativeDirectTriCarrierSwap(&records[pfirst], &records[plast]);
    }
    else
    {
      --glast;
      NativeDirectTriCarrierSwap(&records[gfirst], &records[glast]);
      ++gfirst;
    }
  }
  *outFirst = pfirst;
  *outLast = plast;
}

/* 0x44C3F0: VS2005 introsort, retaining its smaller-half recursion rule. */
static void NativeDirectTriCarrierSortRange(NativeDirectTriSortCarrier_t *records,
                                            int first, int last, int ideal)
{
  for (;;)
  {
    int count = last - first;
    int partFirst;
    int partLast;

    if (count <= 32)
    {
      NativeDirectTriCarrierInsertionSort(records, first, last);
      return;
    }
    if (ideal <= 0)
    {
      NativeDirectTriCarrierHeapSort(records, first, last);
      return;
    }
    NativeDirectTriCarrierPartition(records, first, last, &partFirst, &partLast);
    ideal = ideal / 2 + ideal / 4;
    if (partFirst - first < last - partLast)
    {
      NativeDirectTriCarrierSortRange(records, first, partFirst, ideal);
      first = partLast;
    }
    else
    {
      NativeDirectTriCarrierSortRange(records, partLast, last, ideal);
      last = partFirst;
    }
  }
}

static void Tris_SortLargeDirectRecordCarrier(TriRecord_t *records, int count)
{
  NativeDirectTriSortCarrier_t *carriers;
  int *originAtPosition;
  int *positionForOrigin;
  int i;

  Assert(count > 32, s_assertDisable_Tris_EmitTriangles);
  carriers = (NativeDirectTriSortCarrier_t *)malloc(sizeof(*carriers) * count);
  originAtPosition = (int *)malloc(sizeof(*originAtPosition) * count);
  positionForOrigin = (int *)malloc(sizeof(*positionForOrigin) * count);
  if (!carriers || !originAtPosition || !positionForOrigin)
    Com_Error("Tris_SortLargeDirectRecordCarrier: out of memory");

  for (i = 0; i < count; ++i)
  {
    TriRecord_t *record = &records[i];
    NativeDirectTriSortCarrier_t *carrier = &carriers[i];
    memset(carrier, 0, sizeof(*carrier));
    carrier->visGroup = (short)record->drawOrder;
    carrier->key4 = (unsigned char)record->cullGroupIdx;
    carrier->key5 = (unsigned char)record->lightStyle;
    carrier->key6 = record->reflectionProbeIndex;
    carrier->key7 = record->castsSunShadow;
    carrier->unused[0] = record->primaryLightIndex;
    carrier->material = record->si;
    carrier->sourceIndex = i;
    originAtPosition[i] = i;
    positionForOrigin[i] = i;
  }

  NativeDirectTriCarrierSortRange(carriers, 0, count, count);

  /* Apply the sorted carrier as an in-place permutation: expanded records
     stay expanded and no native 96-byte assumption reaches their payload. */
  for (i = 0; i < count; ++i)
  {
    int desiredOrigin = carriers[i].sourceIndex;
    int currentPosition = positionForOrigin[desiredOrigin];
    if (currentPosition != i)
    {
      int displacedOrigin = originAtPosition[i];
      TriRecord_t held = records[i];
      records[i] = records[currentPosition];
      records[currentPosition] = held;
      originAtPosition[i] = desiredOrigin;
      originAtPosition[currentPosition] = displacedOrigin;
      positionForOrigin[desiredOrigin] = i;
      positionForOrigin[displacedOrigin] = currentPosition;
    }
  }

  free(positionForOrigin);
  free(originAtPosition);
  free(carriers);
}

/*
================
CompareTrisByGroupId

Qsort comparator that orders triangles by group ID
================
*/
int CompareTrisByGroupId(int *a, int *b)
{
  TriRecord_t *tri0 = (TriRecord_t *)a;
  TriRecord_t *tri1 = (TriRecord_t *)b;
  return tri0->groupId - tri1->groupId;
}

/*
================
SortTriIndices

Sorts three triangle vertex indices into canonical ascending order
================
*/
int SortTriIndices( intptr_t *sortedOut, int *unsorted )
{
  int a, b, c, t;

  Assert(unsorted, s_assertDisable_SortTriIndices);
  Assert(sortedOut, s_assertDisable_SortTriIndices);
  Assert((intptr_t *)unsorted != sortedOut, s_assertDisable_SortTriIndices);

  a = unsorted[0];
  b = unsorted[1];
  c = unsorted[2];

  /* 3-element sort via comparison swaps */
  if ( a > b ) { t = a; a = b; b = t; }
  if ( b > c ) { t = b; b = c; c = t; }
  if ( a > b ) { t = a; a = b; b = t; }

  sortedOut[0] = a;
  sortedOut[1] = b;
  sortedOut[2] = c;
  return c;
}

/*
================
CompareSortedTriIndices

Qsort comparator that compares three sorted vertex indices
================
*/
int CompareSortedTriIndices( intptr_t *tri0, intptr_t *tri1 )
{
  if ( tri0[0] != tri1[0] )
    return (int)(tri0[0] - tri1[0]);
  if ( tri0[1] != tri1[1] )
    return (int)(tri0[1] - tri1[1]);
  return (int)(tri0[2] - tri1[2]);
}

/* CoD4 0x43E490.  The inputs are canonical ascending triples, so the
   native decision tree identifies whether they share an edge without a
   general-purpose intersection loop. */
static bool SortedTriIndicesShareEdge(const intptr_t *left, const intptr_t *right)
{
  if ( left[2] == right[2] )
  {
    if ( left[0] == right[0] )
      return true;
    return left[1] == right[0] || left[0] == right[1] || left[1] == right[1];
  }
  if ( left[2] == right[1] )
    return left[0] == right[0] || left[1] == right[0];
  if ( left[1] == right[2] )
    return left[0] == right[0] || left[0] == right[1];
  return left[1] == right[1] && left[0] == right[0];
}

/*
================
GroupAdjacentTris

Groups triangles sharing edges by assigning triGroupId
================
*/
int GroupAdjacentTris(TriRecord_t *records, int numTris, int *indexMap)
{
  #define SORT_STRIDE 4  /* slots per entry: sorted idx0, idx1, idx2, triRecord* */
  intptr_t *sortBuf;
  TriRecord_t *cur, *scan;
  int i, j, k, lowScan, nextGroupId, groupCount;
  int sortedIndices[3];

  Assert(numTris > 0, s_assertDisable_GroupAdjacentTris);
  sortBuf = malloc(SORT_STRIDE * sizeof(intptr_t) * numTris);

  /* build sort buffer: [sorted idx0, idx1, idx2, triRecord*] */
  for ( i = 0; i < numTris; i++ )
  {
    records[i].groupId = 0;
    sortedIndices[0] = indexMap[records[i].vertIdx[0]];
    sortedIndices[1] = indexMap[records[i].vertIdx[1]];
    sortedIndices[2] = indexMap[records[i].vertIdx[2]];
    SortTriIndices(&sortBuf[i * SORT_STRIDE], sortedIndices);
    sortBuf[i * SORT_STRIDE + 3] = (intptr_t)&records[i];
  }
  qsort(sortBuf, numTris, SORT_STRIDE * sizeof(intptr_t), (int (*)(const void *, const void *))CompareSortedTriIndices);

  /* scan for shared edges and merge groups */
  lowScan = 0;
  nextGroupId = 1;
  groupCount = 0;

  #define SORT_IDX0(n) ((int)sortBuf[(n) * SORT_STRIDE + 0])
  #define SORT_IDX1(n) ((int)sortBuf[(n) * SORT_STRIDE + 1])
  #define SORT_IDX2(n) ((int)sortBuf[(n) * SORT_STRIDE + 2])
  #define SORT_REC(n)  ((TriRecord_t *)sortBuf[(n) * SORT_STRIDE + 3])

  for ( i = 0; i < numTris; i++ )
  {
    /* advance lowScan past entries whose max index is below current min */
    while ( lowScan < i && SORT_IDX2(lowScan) < SORT_IDX1(i) )
      lowScan++;

    cur = SORT_REC(i);

    /* check all earlier entries in range for shared edges */
    for ( j = lowScan; j < i; j++ )
    {
      scan = SORT_REC(j);
      if ( scan->groupId == cur->groupId )
        continue;

      if ( !SortedTriIndicesShareEdge(&sortBuf[i * SORT_STRIDE],
                                      &sortBuf[j * SORT_STRIDE]) )
        goto no_shared_edge;

      /* merge: reassign all of cur's group to scan's group */
      if ( cur->groupId )
      {
        int oldGroup = cur->groupId;
        int newGroup = scan->groupId;
        for ( k = 0; k <= i; k++ )
          if ( SORT_REC(k)->groupId == oldGroup )
            SORT_REC(k)->groupId = newGroup;
        groupCount--;
      }
      cur->groupId = scan->groupId;
no_shared_edge: (void)0;
    }

    /* assign new group if still ungrouped */
    if ( !cur->groupId )
    {
      if ( groupCount == 0xFFFF )
        Com_Error("MAX_TRI_SUBGROUPS");
      cur->groupId = nextGroupId++;
      groupCount++;
    }
  }

  #undef SORT_IDX0
  #undef SORT_IDX1
  #undef SORT_IDX2
  #undef SORT_REC
  #undef SORT_STRIDE

  free(sortBuf);
  Assert(groupCount >= 1, s_assertDisable_GroupAdjacentTris);
  if ( groupCount > 1 )
    qsort(records, numTris, sizeof(TriRecord_t), (int (*)(const void *, const void *))CompareTrisByGroupId);
  return groupCount;
}

/* CoD4 0x445820.
================
MergeTriGroups

Merges secondGroup into firstGroup, updates group metadata
================
*/
int MergeTriGroups(TriRecord_t *triRecs, int triCount, int firstGroup, int secondGroup, float *mins, float *maxs, float *extents, TriGroup_t *groups, int groupCount)
{
  TriGroup_t *groupA = &groups[firstGroup];
  TriGroup_t *groupB = &groups[secondGroup];
  int i, secondCount, firstEnd;

  Assert(triRecs, s_assertDisable_MergeTriGroups);
  Assert(firstGroup < secondGroup, s_assertDisable_MergeTriGroups);
  Assert(secondGroup < groupCount, s_assertDisable_MergeTriGroups);
  Assert(groupA->triCount + groupA->firstTri <= groupB->firstTri, s_assertDisable_MergeTriGroups);
  Assert(groupB->triCount + groupB->firstTri <= triCount, s_assertDisable_MergeTriGroups);
  Assert(mins, s_assertDisable_MergeTriGroups);
  Assert(maxs, s_assertDisable_MergeTriGroups);

  /* reassign second group's tris to first, decrement all after */
  for ( i = groupB->firstTri; i < groupB->firstTri + groupB->triCount; i++ )
  {
    AssertFatal(triRecs[i].groupId == secondGroup, s_assertDisable_MergeTriGroups);
    triRecs[i].groupId = firstGroup;
  }
  for ( ; i < triCount; i++ )
  {
    AssertFatal(triRecs[i].groupId > secondGroup, s_assertDisable_MergeTriGroups);
    triRecs[i].groupId--;
  }

  secondCount = groupB->triCount;

  /* if groups aren't adjacent, rotate second group's tris to be after first */
  if ( secondGroup - firstGroup > 1 )
  {
    firstEnd = groupA->firstTri + groupA->triCount;
    { unsigned int bufSize = sizeof(TriRecord_t) * secondCount;
    void *temp = malloc(bufSize);
    memcpy(temp, &triRecs[groupB->firstTri], bufSize);
    memcpy(&triRecs[firstEnd + secondCount], &triRecs[firstEnd], sizeof(TriRecord_t) * (groupB->firstTri - firstEnd));
    memcpy(&triRecs[firstEnd], temp, bufSize);
    free(temp); }

    for ( i = firstGroup + 1; i < secondGroup; i++ )
      groups[i].firstTri += secondCount;
  }

  /* remove second group entry by shifting array down */
  for ( i = secondGroup + 1; i < groupCount; i++ )
    groups[i - 1] = groups[i];

  groupA = &groups[firstGroup];
  groupA->triCount += secondCount;
  VectorCopy(mins, groupA->mins);
  VectorCopy(maxs, groupA->maxs);
  VectorCopy(extents, groupA->extents);
  return groupCount - 1;
}

/* CoD4 0x4451E0.
================
GroupAndMergeTriSubgroups

Groups adjacent tris, then merges subgroups if >1 group.
Originally a naked wrapper passing EDI/ESI/EAX registers.
================
*/
int GroupAndMergeTriSubgroups(TriRecord_t *tris, int triCount, int *indexMap)
{
  int result;

  result = GroupAdjacentTris(tris, triCount, indexMap);
  if ( result > 1 )
    return GroupTriSurfsIntoSubgroups(tris, triCount, result);
  return result;
}

/* CoD4 0x4467B0.
================
ClipSideIntoTree_r

Recursively clips winding through BSP tree, marks leaf visibility
================
*/
void ClipSideIntoTree_r(Winding_t *winding, float *side, TriRecord_t *sideData, Node_t *node)
{
  #define COPLANAR_DOT  0.99900001f
  #define COPLANAR_DIST 0.001f
  float *plane;
  double dot;
  Winding_t *front, *back;

  if ( !winding )
    return;

  if ( node->cellnum != CELLNUM_UNKNOWN )
  {
    if ( node->cellnum >= 0 )
      g_brushSideBitmap[(sideData->cullGroupIdx + (node->cellnum << CELL_BITMAP_SHIFT)) >> 3] |= 1 << ((char)sideData->cullGroupIdx & 7);
    FreeWinding(winding);
    return;
  }

  plane = MAP_PLANE(node->planenum)->normal;
  dot = DotProduct210(plane, side);

  if ( dot > COPLANAR_DOT && fabs((double)side[3] - (double)plane[3]) < COPLANAR_DIST )
  {
    ClipSideIntoTree_r(winding, side, sideData, node->children[0]);
  }
  else if ( dot < -COPLANAR_DOT && fabs((double)plane[3] + (double)side[3]) < COPLANAR_DIST )
  {
    ClipSideIntoTree_r(winding, side, sideData, node->children[1]);
  }
  else
  {
    ClipWindingEpsilon(winding, plane, plane[3], ON_EPSILON, &front, &back, 0);
    ClipSideIntoTree_r(front, side, sideData, node->children[0]);
    ClipSideIntoTree_r(back, side, sideData, node->children[1]);
    FreeWinding(winding);
  }
  #undef COPLANAR_DOT
  #undef COPLANAR_DIST
}

/*
================
CanMergeVerts

Checks if two vertices can be merged based on position, normal, UV, color, texvec.
Originally __usercall (ECX=current_vertex, EDI=compare_vertex, EAX=compare_tri),
now takes all params on stack. Caller (MergeDuplicateVerts) pushes the extra 3.
================
*/
static const float k_mergeNormalThresh = MERGE_NORMAL_THRESHOLD;
static const float k_mergeUvThresh    = MERGE_UV_THRESHOLD;
static const float k_mergeLmScale     = (float)LIGHTMAP_SIZE;

/* CoD4 0x447290. */
static bool MatchLightmapUVs(const BspDrawVert_t *dvA, const BspDrawVert_t *dvB)
{
  volatile float scaledDu = MULF(k_mergeLmScale, dvB->lmUv[0], -dvA->lmUv[0]);
  double scaledDv = MULF(k_mergeLmScale, dvB->lmUv[1], -dvA->lmUv[1]);

  return MulAdd2(scaledDv, scaledDv, scaledDu, scaledDu) <= k_mergeUvThresh;
}

/* CoD4 0x4481B0. */
static bool MatchTextureUVs(const BspDrawVert_t *dvA, const BspDrawVert_t *dvB,
                            const ShaderInfo_t *material)
{
  volatile float scaledDu = (float)((double)material->autoTexScaleW *
                                    (dvA->uv[0] - dvB->uv[0]));
  double scaledDv = (double)material->autoTexScaleH * (dvA->uv[1] - dvB->uv[1]);
  double distSq = (double)scaledDu * scaledDu + scaledDv * scaledDv;

  return distSq <= (double)k_mergeUvThresh;
}

/* CoD4 0x447ED0. */
char CanMergeVerts( BspDrawVert_t *cmp, BspDrawVert_t *cur, TriRecord_t *cmpTri, TriRecord_t *curTri, int flags )
{
  unsigned char *curColor = (unsigned char *)&cur->color;
  unsigned char *cmpColor = (unsigned char *)&cmp->color;

  /* position must be within epsilon */
  if ( !VectorCompareEpsilon(cmp->pos, cur->pos, PLANESIDE_EPSILON, 3) )
    return 0;

  /* normals must be nearly parallel */
  if ( DotProduct210(cur->normal, cmp->normal) < (double)k_mergeNormalThresh )
    return 0;

  /* UV distance scaled by texture dimensions must be small.
     Original keeps du/dv at 80-bit FPU precision (never stores to float).
     scaledDu is stored as float (var_8), scaledDv stays at 80-bit. */
  if (!MatchTextureUVs(cmp, cur, cmpTri->si))
    return 0;

  /* lightmap UV check (skip for surfaces with all lightmap styles).
     Same precision pattern as UV check: du/dv stay at 80-bit,
     scaledDu stored as float, scaledDv stays at 80-bit. */
  #define LMAP_ALL_STYLES 0x1F
  if ( flags != LMAP_ALL_STYLES )
  {
    volatile float scaledDu = (float)((cmp->lmUv[0] - cur->lmUv[0]) * k_mergeLmScale);
    double scaledDv = (double)(cmp->lmUv[1] - cur->lmUv[1]) * k_mergeLmScale;
    double distSq = (double)scaledDu * scaledDu + scaledDv * scaledDv;
    if ( !(distSq <= (double)k_mergeUvThresh) )
      return 0;
  }

  /* color must be identical */
  if ( cmpColor[0] != curColor[0] ) return 0;
  if ( cmpColor[1] != curColor[1] ) return 0;
  if ( cmpColor[2] != curColor[2] ) return 0;
  if ( cmpColor[3] != curColor[3] ) return 0;

  /* texture vectors must point the same direction (skip if same triangle) */
  if ( cmpTri != curTri )
  {
    double dot1 = DotProduct210(cmpTri->texVecS, curTri->texVecS);
    if ( dot1 > 0.0 )
    {
      double dot2 = DotProduct210(cmpTri->texVecT, curTri->texVecT);
      if ( dot2 > 0.0 )
        return 1;
    }
    numUnmergeableTexVerts++;
    return 0;
  }

  return 1;
}

/* CoD4 0x446980.
================
EmitDrawVert

Emits a draw vertex to the global array, reorders fields.
Original passes a1 in EBX register; now uses g_emitDrawVertCount static global.
================
*/
static TriSoup_t *g_emitDrawVertCount;

int EmitDrawVert( int a1_unused, DrawVert_t *src )
{
  /* a1 was set by caller into g_emitDrawVertCount */
  TriSoup_t *emitHdr = g_emitDrawVertCount;
  BspDrawVert_t tmp;
  int destIdx;

  /* Remap DrawVert_t (72 bytes) → BspDrawVert_t (68 bytes):
     pos ← pos, normal ← smoothNormal, color ← color, uv ← uv, lmUv ← lmUv
     (tangent and binormal are filled later by TangentSpaceGenerate). */
  VectorCopy(src->pos, tmp.pos);
  VectorCopy(src->smoothNormal, tmp.normal);
  tmp.color = src->color;
  tmp.uv[0] = src->uv[0];
  tmp.uv[1] = src->uv[1];
  tmp.lmUv[0] = src->lmUv[0];
  tmp.lmUv[1] = src->lmUv[1];

  if ( numBSPDrawVertsEmitted == MAX_MAP_DRAW_VERTS )
    Com_Error("MAX_MAP_DRAW_VERTS (%i) exceeded\n", MAX_MAP_DRAW_VERTS);

  destIdx = emitHdr->numVerts + 1;
  emitHdr->numVerts = destIdx;
  memcpy(&g_drawVertexBuf[destIdx + emitHdr->firstVert], &tmp, sizeof(BspDrawVert_t));
  return emitHdr->numVerts - 1;
}

/* CoD4 0x4470A0.
================
MatchVertWithUVOffset

Checks vertex match within tolerances, outputs integer UV offsets
================
*/
bool MatchVertWithUVOffset( int *outUOffset, BspDrawVert_t *dvA, BspDrawVert_t *dvB, ShaderInfo_t *material, int flags, int *outVOffset )
{
  unsigned char *colA = (unsigned char *)&dvA->color;
  unsigned char *colB = (unsigned char *)&dvB->color;
  double ndot;

  /* position must match */
  if ( !VectorCompareEpsilon(dvA->pos, dvB->pos, PLANESIDE_EPSILON, 3) )
    return 0;

  /* normals must be nearly parallel */
  ndot = DotProduct210(dvA->normal, dvB->normal);
  if ( ndot < k_mergeNormalThresh )
    return 0;

  /* lightmap UV distance check (skip if all lightmap styles) */
  #define LMAP_ALL_STYLES 0x1F
  if ( flags != LMAP_ALL_STYLES )
  {
    if ( !MatchLightmapUVs(dvA, dvB) )
      return 0;
  }

  /* color must be identical */
  if ( colA[0] != colB[0] || colA[1] != colB[1] || colA[2] != colB[2] || colA[3] != colB[3] )
    return 0;

  /* compute integer UV tile offset and check fractional residual */
#ifdef _WIN64
  /* x87 inlines RoundFloatToInt, keeping float-float subtraction at 53-bit.
     Match by computing subtraction at double before rounding. */
  outUOffset[0] = xs_RoundToInt((double)dvB->uv[0] - (double)dvA->uv[0] + FISTP_BIAS);
  outVOffset[0] = xs_RoundToInt((double)dvB->uv[1] - (double)dvA->uv[1] + FISTP_BIAS);
#else
	
  /* Original inlines: fld B; fsub A; fadd BIAS; fistp — subtraction at 80-bit,
     no truncation to float before adding bias. Must NOT use RoundFloatToInt
     which truncates to float via the function call.
     Original: fld B; fsub A; fadd BIAS; fistp — 80-bit throughout.
     xs_RoundToInt truncates to 64-bit double, causing 1-ULP rounding diffs.
     Use fistp_add which matches the original's fistp pattern. */
  outUOffset[0] = fistp_add(dvB->uv[0] - dvA->uv[0], FISTP_BIAS);
  outVOffset[0] = fistp_add(dvB->uv[1] - dvA->uv[1], FISTP_BIAS);
#endif
  {
#ifdef _WIN64
    float adj_du = (float)((double)dvB->uv[0] - (double)dvA->uv[0] - (double)(float)(int)outUOffset[0]);
    double adj_dv = (double)dvB->uv[1] - (double)dvA->uv[1] - (double)(float)(int)outVOffset[0];
#else
    float adj_du = (float)((double)dvB->uv[0] - (double)dvA->uv[0] - (double)(int)outUOffset[0]);
    double adj_dv = (double)dvB->uv[1] - (double)dvA->uv[1] - (double)(int)outVOffset[0];
#endif
    double scaled_du = (double)material->autoTexScaleW * adj_du;
    double scaled_dv = (double)material->autoTexScaleH * adj_dv;
    if ( MulAdd2(scaled_du,scaled_du, scaled_dv,scaled_dv) <= k_mergeUvThresh )
      return 1;
  }
  return 0;
}

/* CoD4 0x446F30.
================
CheckTriangleUVCompatibility

Checks if two triangles have consistent UV offsets at shared vertices
================
*/
char CheckTriangleUVCompatibility( unsigned short *idx, int triOffset1, BspDrawVert_t *verts, ShaderInfo_t *material, int flags, int triOffset2, int *outUOffset, int *outVOffset )
{
  int i, j;
  int tempU, tempV;
  char foundMatch = 0;

  /* for each shared vertex between tri2 and tri1, verify UV offset consistency */
  for ( i = 0; i < 3; i++ )
  {
    BspDrawVert_t *v2 = &verts[idx[triOffset2 + i]];

    for ( j = 0; j < 3; j++ )
    {
      BspDrawVert_t *v1 = &verts[idx[triOffset1 + j]];

      if ( v2->pos[0] != v1->pos[0] || v2->pos[1] != v1->pos[1] || v2->pos[2] != v1->pos[2] )
        continue;

      /* shared vertex — check UV offset */
      if ( !MatchVertWithUVOffset(&tempU, v2, v1, material, flags, &tempV) )
        return 0;

      if ( foundMatch )
      {
        if ( outUOffset[0] != tempU || outVOffset[0] != tempV )
          return 0;
      }
      else
      {
        outUOffset[0] = tempU;
        outVOffset[0] = tempV;
        foundMatch = 1;
      }
    }
  }
  return 1;
}

/* CoD4 0x446C90.
================
FindTriangleNeighbors

Finds adjacent triangles sharing edges
================
*/
int FindTriangleNeighbors( BspDrawVert_t *verts, unsigned short *idx, int indexCount, int *neighbors )
{
  int outerTri, innerTri, oe, ie;

  memset(neighbors, 0xFF, sizeof(int) * indexCount);

  /* For each pair of triangles, check if they share an edge.
     Edge e of a triangle at offset t: from = idx[t+(e+2)%3], to = idx[t+e].
     Shared edge: outer (from→to) matches inner (to→from) — reverse winding. */
  for ( outerTri = 3; outerTri < indexCount; outerTri += 3 )
  {
    for ( innerTri = 0; innerTri < outerTri; innerTri += 3 )
    {
      for ( oe = 0; oe < 3; oe++ )
      {
        float *oFrom = verts[idx[outerTri + (oe + 2) % 3]].pos;
        float *oTo   = verts[idx[outerTri + oe]].pos;

        for ( ie = 0; ie < 3; ie++ )
        {
          float *iFrom = verts[idx[innerTri + ie]].pos;
          float *iTo   = verts[idx[innerTri + (ie + 2) % 3]].pos;

          if ( oFrom[0] == iFrom[0] && oFrom[1] == iFrom[1] && oFrom[2] == iFrom[2]
            && oTo[0] == iTo[0] && oTo[1] == iTo[1] && oTo[2] == iTo[2] )
          {
            neighbors[outerTri + (oe + 2) % 3] = innerTri;
            neighbors[innerTri + (ie + 2) % 3] = outerTri;
            goto nextInnerTri;
          }
        }
      }
      nextInnerTri:;
    }
  }
  return indexCount > 3 ? 2 * (indexCount - 3) : -1;
}

/* CoD4 0x446DF0 (recursive walk) and 0x4473C0 (base UV adjustment).
================
AdjustTriangleUVs_r

Recursively adjusts UVs for consistency across shared edges
================
*/
int AdjustTriangleUVs_r( BspDrawVert_t *verts, unsigned short *idx, ShaderInfo_t *material, int flags, int *neighbors, int triIdx, char *visited )
{
  int edge, neighborTri;
  int uvOffsetU, uvOffsetV;

  Assert(!(triIdx % 3), s_assertDisable_AdjustTriangleUVs_r);
  visited[triIdx] = 1;

  /* for each edge, propagate UV tile offsets to unvisited neighbors */
  for ( edge = 0; edge < 3; edge++ )
  {
    neighborTri = neighbors[triIdx + edge];
    if ( neighborTri < 0 || visited[neighborTri] )
      continue;

    if ( CheckTriangleUVCompatibility(idx, neighborTri, verts, material, flags, triIdx, &uvOffsetU, &uvOffsetV) )
    {
      double du = (double)uvOffsetU;
      double dv = (double)uvOffsetV;
      int k;

      /* shift all 3 vertices of the neighbor triangle */
      for ( k = 0; k < 3; k++ )
      {
        BspDrawVert_t *v = &verts[idx[neighborTri + k]];
        v->uv[0] -= (float)du;
        v->uv[1] -= (float)dv;
      }

      AdjustTriangleUVs_r(verts, idx, material, flags, neighbors, neighborTri, visited);
    }
  }
  return 0;
}

/* CoD4 0x446B90.
================
StitchTriangleUVs

Finds triangle neighbors and stitches UVs across shared edges
================
*/
void StitchTriangleUVs( BspDrawVert_t *verts, signed int indexCount, int unused, unsigned short *idx, ShaderInfo_t *material, int flags )
{
  int i;
  int *neighbors = malloc(sizeof(int) * indexCount);
  char *visited = malloc(indexCount);

  FindTriangleNeighbors(verts, idx, indexCount, neighbors);
  memset(visited, 0, indexCount);

  /* walk each connected component of the triangle graph */
  for ( i = 0; i < indexCount; i += 3 )
  {
    if ( !visited[i] )
      AdjustTriangleUVs_r(verts, idx, material, flags, neighbors, i, visited);
  }

  free(visited);
  free(neighbors);
}

/* Native 0x447560 dispatches every non-global material layer to 0x447610.
   KIWI retains only the base 68-byte BSP vertex stream; its simpler global
   centering path is recorded as a pending range/fallback port. */
/*
================
CenterTextureCoordinates

Centers UV coordinates around origin for numerical stability
================
*/

/* BSP vertices are 68 bytes (17 floats). UV at float offsets 7 and 8. */
#define BSP_VERT_FLOATS 17
#define BSP_UV_U 7
#define BSP_UV_V 8

void CenterTextureCoordinates(int numVerts, float *vertArray)
{
  int i;
  float *v;
  float maxU, maxV, minU, minV;
  float midU, midV, centerU, centerV;

  /* find min/max UV bounds */
  minU = vertArray[BSP_UV_U];
  maxU = vertArray[BSP_UV_U];
  minV = vertArray[BSP_UV_V];
  maxV = minV;

  for ( i = 1; i < numVerts; i++ )
  {
    v = &vertArray[i * BSP_VERT_FLOATS];
    if ( v[BSP_UV_U] < minU )
      minU = v[BSP_UV_U];
    else if ( v[BSP_UV_U] > maxU )
      maxU = v[BSP_UV_U];
    if ( v[BSP_UV_V] < minV )
      minV = v[BSP_UV_V];
    else if ( v[BSP_UV_V] > maxV )
      maxV = v[BSP_UV_V];
  }

  /* compute center and snap to integer boundary */
  midU = MIDF(minU, maxU);
  centerU = (float)fistp_sub(midU, FISTP_HALF_BIAS);
  midV = MIDF(minV, maxV);
  centerV = (float)fistp_sub(midV, FISTP_HALF_BIAS);

  /* shift all UVs by the offset */
  for ( i = 0; i < numVerts; i++ )
  {
    v = &vertArray[i * BSP_VERT_FLOATS];
    v[BSP_UV_U] -= centerU;
    v[BSP_UV_V] -= centerV;
  }
}

#undef BSP_VERT_FLOATS
#undef BSP_UV_U
#undef BSP_UV_V

/* CoD4 0x447CC0. */
/*
================
MergeDuplicateVerts

Merges duplicate vertices and remaps indices via CanMergeVerts.
Originally __usercall, converted to C in session 224.
triSurfMap[] maps each vertex slot to its triSurf index.
================
*/
#define MAX_MERGE_VERTS 0x20000

int MergeDuplicateVerts( BspDrawVert_t *verts, int numVerts, unsigned short *indices, int numIndices, TriRecord_t *triRecs, int flags )
{
  int triMap[MAX_MERGE_VERTS + 4];  /* maps each vertex to its triangle index */
  unsigned short cur, cmp;
  int i;

  if ( numVerts > MAX_MERGE_VERTS )
    Com_Error("MergeDuplicateVerts: %i vertices > %i\n", numVerts, MAX_MERGE_VERTS);

  /* assign each vertex to its triangle (3 consecutive verts per triangle) */
  for ( i = 0; i < numVerts; i++ )
    triMap[i] = i / 3;

  if ( numVerts <= 1 )
    return numVerts;

  /* for each vertex, try to merge with an earlier duplicate */
  cur = 1;
  while ( cur < numVerts )
  {
    int merged = 0;

    for ( cmp = 0; cmp < cur; cmp++ )
    {
      if ( CanMergeVerts(&verts[cmp], &verts[cur],
                         &triRecs[triMap[cmp]], &triRecs[triMap[cur]], flags) )
      {
        /* swap-remove: move last vertex into cur's slot */
        numVerts--;
        verts[cur] = verts[numVerts];
        triMap[cur] = triMap[numVerts];

        /* remap indices: cur→cmp (merged), last→cur (moved) */
        for ( i = 0; i < numIndices; i++ )
        {
          if ( indices[i] == (unsigned short)cur )
            indices[i] = cmp;
          else if ( (unsigned int)indices[i] == numVerts )
            indices[i] = (unsigned short)cur;
        }
        merged = 1;
        break;
      }
    }

    if ( !merged )
      cur++;
  }

  return numVerts;
}

#undef MAX_MERGE_VERTS

/* CoD4 0x446B00. */
static int StitchAndMergeTriangleVerts(BspDrawVert_t *verts, int numVerts,
                                       unsigned short *indices, int numIndices,
                                       TriRecord_t *firstRec, int flags)
{
  ShaderInfo_t *material = firstRec->si;

  if (!material->globalTexture)
  {
    StitchTriangleUVs(verts, numIndices, numVerts, indices, material, flags);
    CenterTextureCoordinates(numVerts, (float *)verts);
  }

  return MergeDuplicateVerts(verts, numVerts, indices, numIndices, firstRec, flags);
}

/*
================
TriEdgesShareVerts

Checks if two triangles share an edge (two matching vertex positions)
================
*/
char TriEdgesShareVerts( TriSoup_t *ts, int triIdx1, int triIdx2 )
{
  unsigned short *idx = &bspDrawIndexes[ts->firstIndex];
  BspDrawVert_t *verts = &bspTriSoupData[ts->firstVert];
  int i, j;

  /* Native 0x4488F0 compares the shared edge in opposite winding
     directions: triangle 1 predecessor->current against triangle 2
     current->predecessor. */
  for ( i = 0; i < 3; i++ )
  {
    float *e1from = verts[idx[triIdx1 + (i + 2) % 3]].pos;
    float *e1to   = verts[idx[triIdx1 + i]].pos;

    for ( j = 0; j < 3; j++ )
    {
      float *e2from = verts[idx[triIdx2 + j]].pos;
      float *e2to   = verts[idx[triIdx2 + (j + 2) % 3]].pos;

      if ( e1from[0] == e2from[0] && e1from[1] == e2from[1] && e1from[2] == e2from[2]
        && e1to[0] == e2to[0] && e1to[1] == e2to[1] && e1to[2] == e2to[2] )
        return 1;
    }
  }
  return 0;
}

/* CoD4 0x4489F0. */
static void MergeTriGroupIds(short *groupIds, int groupCount,
                             short fromGroup, short toGroup)
{
  int groupIndex;

  for (groupIndex = 0; groupIndex < groupCount; ++groupIndex)
  {
    if (groupIds[groupIndex] == fromGroup)
      groupIds[groupIndex] = toGroup;
  }
}

/* CoD4 0x4487B0.
================
CountTriGroups

Assigns group IDs to triangles based on shared edges
================
*/
int CountTriGroups( short *groupIds, TriSoup_t *ts )
{
  int numTris, numGroups, nextGroupId;
  int cur, prev;

  numTris = ts->numIndices / 3;
  if ( numTris <= 0 )
    return 0;

  numGroups = 0;
  nextGroupId = 1;

  for ( cur = 0; cur < numTris; cur++ )
  {
    groupIds[cur] = 0;

    /* check against all previously processed triangles for shared edges */
    for ( prev = 0; prev < cur; prev++ )
    {
      if ( groupIds[cur] != groupIds[prev] && TriEdgesShareVerts(ts, cur * 3, prev * 3) )
      {
        /* merge: reassign cur's group to prev's group */
        if ( groupIds[cur] )
        {
          short oldGroup = groupIds[cur];
          short newGroup = groupIds[prev];
          MergeTriGroupIds(groupIds, cur, oldGroup, newGroup);
          --numGroups;
        }
        groupIds[cur] = groupIds[prev];
      }
    }

    /* if no group assigned, start a new one */
    if ( !groupIds[cur] )
    {
      groupIds[cur] = nextGroupId++;
      ++numGroups;
    }
  }
  return numGroups;
}

/* CoD4 0x4483B0.
================
GroupTriangles

Partitions triangles into groups by shared edges, emits sub-soups
================
*/
char GroupTriangles( TriSoup_t *ts )
{
  int numGroups, numTris, totalIndexCount;
  short *groupIds;
  BspDrawVert_t *savedVerts;
  unsigned short *savedIndices;
  unsigned short *remap;
  int grp, groupId, tri, v, srcVert;

  /* grpSoup MUST be contiguous — PartitionTriSoupRecursive reads via *(a1+4) etc. */
  struct {
    unsigned short materialSort[2];
    int firstVert;
    int vertCount;
    int indexCount;
    int firstIndex;
  } grpSoup;

  groupIds = malloc(sizeof(short) * (ts->numIndices / 3));
  numGroups = CountTriGroups(groupIds, ts);
  Assert(numGroups >= 1, s_assertDisable_GroupTriangles);

  if ( numGroups == 1 )
  {
    free(groupIds);
    return 0;
  }

  /* save vertex and index data — we'll rebuild per group */
  totalIndexCount = ts->numIndices;
  numTris = totalIndexCount / 3;
  savedVerts = malloc(sizeof(BspDrawVert_t) * ts->numVerts);
  memcpy(savedVerts, &bspTriSoupData[ts->firstVert], sizeof(BspDrawVert_t) * ts->numVerts);
  savedIndices = malloc(sizeof(unsigned short) * totalIndexCount);
  memcpy(savedIndices, &bspDrawIndexes[ts->firstIndex], sizeof(unsigned short) * totalIndexCount);
  remap = malloc(sizeof(unsigned short) * totalIndexCount);

  /* emit each group as a separate sub-soup */
  groupId = 0;
  for ( grp = 0; grp < numGroups; grp++ )
  {
    grpSoup.materialSort[0] = ts->materialSort;
    grpSoup.materialSort[1] = ts->lightmapSort;
    grpSoup.firstVert = numBSPDrawVertsEmitted;
    grpSoup.firstIndex = numBSPDrawIndexes;
    grpSoup.vertCount = 0;
    grpSoup.indexCount = 0;
    memset(remap, 0xFF, sizeof(unsigned short) * totalIndexCount);

    /* find the next groupId that has triangles */
    do
    {
      ++groupId;
      for ( tri = 0; tri < numTris; tri++ )
      {
        if ( groupIds[tri] != groupId )
          continue;

        /* emit this triangle's 3 vertices */
        for ( v = 0; v < 3; v++ )
        {
          srcVert = savedIndices[tri * 3 + v];

          /* add vertex if not yet remapped */
          if ( remap[srcVert] >= grpSoup.indexCount )
          {
            remap[srcVert] = (unsigned short)grpSoup.vertCount;
            bspTriSoupData[grpSoup.firstVert + grpSoup.vertCount] = savedVerts[srcVert];
            Assert((unsigned short)grpSoup.vertCount == grpSoup.vertCount, s_assertDisable_GroupTriangles);
            ++grpSoup.vertCount;
          }

          bspDrawIndexes[grpSoup.firstIndex + grpSoup.indexCount] = remap[srcVert];
          ++grpSoup.indexCount;
        }
      }
    }
    while ( !grpSoup.indexCount );

    PartitionTriSoupRecursive((TriSoup_t *)&grpSoup, 0);
  }

  free(remap);
  free(savedIndices);
  free(savedVerts);
  free(groupIds);
  return 1;
}

/* CoD4 0x448F60.
================
ClassifyTriVertsByAxis

Classifies tri vertices as below/above split plane on given axis
================
*/
int ClassifyTriVertsByAxis( TriSoup_t *ts, int axis, float splitValue, char *belowFlags, char *aboveFlags )
{
  int triOfs, j, v;
  int belowCount, aboveCount;
  unsigned char isBelow, isAbove;
  #define CLASSIFY_EPSILON 0.001f
  volatile float lo = splitValue - CLASSIFY_EPSILON;
  float hi = splitValue + CLASSIFY_EPSILON;

  memset(belowFlags, 0, ts->numVerts);
  memset(aboveFlags, 0, ts->numVerts);
  belowCount = 0;
  aboveCount = 0;

  for ( triOfs = 0; triOfs < ts->numIndices; triOfs += 3 )
  {
    /* classify each vertex of this triangle */
    isBelow = 0;
    isAbove = 0;
    for ( j = 0; j < 3; j++ )
    {
      v = (unsigned short)bspDrawIndexes[ts->firstIndex + triOfs + j];
      Assert(v < ts->numVerts, s_assertDisable_ClassifyTriVertsByAxis);

      if ( bspTriSoupData[v + ts->firstVert].pos[axis] < (double)lo )
        isBelow = 1;
      else if ( bspTriSoupData[v + ts->firstVert].pos[axis] > (double)hi )
        isAbove = 1;
    }

    /* if triangle straddles neither side, assign to the less-populated side */
    if ( !isBelow && !isAbove )
    {
      if ( belowCount >= aboveCount )
        isAbove = 1;
      else
        isBelow = 1;
    }

    belowCount += isBelow;
    aboveCount += isAbove;

    /* mark all 3 vertices with this triangle's classification */
    for ( j = 0; j < 3; j++ )
    {
      v = (unsigned short)bspDrawIndexes[ts->firstIndex + triOfs + j];
      belowFlags[v] |= isBelow;
      aboveFlags[v] |= isAbove;
    }
  }

  return ts->numIndices;
  #undef CLASSIFY_EPSILON
}

/* CoD4 0x4491A0.
================
SplitTrisByClassification

Splits tris into below/above sets by vertex classification
================
*/
int SplitTrisByClassification( int *vertCounts, TriSoup_t *ts, char *belowArr, char *aboveArr, unsigned short **remapBufs, unsigned short **idxBufsArr, int *indexCounts, BspDrawVert_t **vBufsArr )
{
  BspDrawVert_t *srcVerts = &bspTriSoupData[ts->firstVert];
  int triOfs, j, side;
  int v[3];

  if ( !vBufsArr )
  {
    vertCounts[0] = ts->numVerts;
    vertCounts[1] = ts->numVerts;
  }
  indexCounts[0] = 0;
  indexCounts[1] = 0;

  for ( triOfs = 0; triOfs < ts->numIndices; triOfs += 3 )
  {
    /* read this triangle's 3 vertex indices */
    v[0] = (unsigned short)bspDrawIndexes[ts->firstIndex + triOfs];
    v[1] = (unsigned short)bspDrawIndexes[ts->firstIndex + triOfs + 1];
    v[2] = (unsigned short)bspDrawIndexes[ts->firstIndex + triOfs + 2];

    /* determine which side: below (0) or above (1) */
    {
      bool allBelow = belowArr[v[0]] && belowArr[v[1]] && belowArr[v[2]];
      bool allAbove = aboveArr[v[0]] && aboveArr[v[1]] && aboveArr[v[2]];

      if ( allBelow && allAbove && indexCounts[0] < indexCounts[1] )
        allAbove = 0;
      if ( !allBelow )
        Assert(allAbove, s_assertDisable_SplitTrisByClassification);

      side = allAbove;
    }

    /* emit 3 vertices to the chosen side */
    for ( j = 0; j < 3; j++ )
    {
      unsigned short idx = bspDrawIndexes[ts->firstIndex + triOfs + j];

      if ( vBufsArr )
      {
        unsigned short *remap = remapBufs[side];
        int origVert = idx;

        if ( remap[origVert] >= vertCounts[side] )
        {
          remap[origVert] = vertCounts[side];
          vBufsArr[side][vertCounts[side]] = srcVerts[origVert];
          ++vertCounts[side];
        }
        idx = remap[origVert];
      }

      idxBufsArr[side][indexCounts[side]++] = idx;
    }
  }

  return triOfs;
}

/* CoD4 0x448A40.
================
SplitSoupOnAxis

Splits triangle soup along an axis at splitValue,
partitions each half recursively
================
*/
char SplitSoupOnAxis( TriSoup_t *ts, int axis, float splitValue )
{
  int i;
  bool shouldCopyVerts;
  char result;

  /* subSoup MUST be contiguous — PartitionTriSoupRecursive reads via *(a1+4) etc. */
  struct {
    unsigned short materialSort[2];
    int firstVert;
    int vertCount;
    int indexCount;
    int firstIndex;
  } subSoup;

  /* per-side buffers: [0]=below, [1]=above */
  BspDrawVert_t *vertBufs[2];
  unsigned short *indexBufs[2], *vertRemaps[2];
  int indexCounts[2], vertCounts[2];
  char *belowFlags, *aboveFlags;

  /* allocate per-side buffers */
  for ( i = 0; i < 2; i++ )
  {
    vertBufs[i] = malloc(sizeof(BspDrawVert_t) * ts->numVerts);
    indexBufs[i] = malloc(sizeof(unsigned short) * ts->numIndices);
    vertRemaps[i] = malloc(sizeof(unsigned short) * ts->numVerts);
  }
  belowFlags = malloc(ts->numVerts);
  aboveFlags = malloc(ts->numVerts);

  /* classify and split */
  ClassifyTriVertsByAxis(ts, axis, splitValue, belowFlags, aboveFlags);
  memset(vertRemaps[0], 0xFF, sizeof(unsigned short) * ts->numVerts);
  memset(vertRemaps[1], 0xFF, sizeof(unsigned short) * ts->numVerts);
  vertCounts[0] = 0;
  vertCounts[1] = 0;
  #define SPLIT_COPY_VERT_THRESHOLD 5450
  shouldCopyVerts = ts->numVerts > SPLIT_COPY_VERT_THRESHOLD;
  SplitTrisByClassification(vertCounts, ts, belowFlags, aboveFlags, vertRemaps, indexBufs, indexCounts, shouldCopyVerts ? vertBufs : NULL);

  if ( indexCounts[0] && indexCounts[1] )
  {
    Assert(vertCounts[0], s_assertDisable_SplitSoupOnAxis);
    Assert(vertCounts[1], s_assertDisable_SplitSoupOnAxis);
    AssertFatal(vertCounts[0] + vertCounts[1] >= ts->numVerts, s_assertDisable_SplitSoupOnAxis);

    subSoup.materialSort[0] = ts->materialSort;
    subSoup.materialSort[1] = ts->lightmapSort;

    /* emit each half as a sub-soup */
    for ( i = 0; i < 2; i++ )
    {
      if ( shouldCopyVerts )
      {
        subSoup.firstVert = numBSPDrawVertsEmitted;
        subSoup.vertCount = vertCounts[i];
        memcpy(&bspTriSoupData[numBSPDrawVertsEmitted], vertBufs[i], sizeof(BspDrawVert_t) * subSoup.vertCount);
      }
      else
      {
        subSoup.firstVert = ts->firstVert;
        subSoup.vertCount = ts->numVerts;
      }
      subSoup.firstIndex = numBSPDrawIndexes;
      subSoup.indexCount = indexCounts[i];
      memcpy(&bspDrawIndexes[numBSPDrawIndexes], indexBufs[i], sizeof(unsigned short) * subSoup.indexCount);
      PartitionTriSoupRecursive((TriSoup_t *)&subSoup, 0);
      Assert(numBSPDrawVertsEmitted >= subSoup.firstVert + subSoup.vertCount, s_assertDisable_SplitSoupOnAxis);
      Assert(numBSPDrawIndexes == subSoup.firstIndex + subSoup.indexCount, s_assertDisable_SplitSoupOnAxis);
    }
    result = 1;
  }
  else
  {
    result = 0;
  }

  /* free all buffers */
  free(aboveFlags);
  free(belowFlags);
  for ( i = 1; i >= 0; i-- )
  {
    free(vertRemaps[i]);
    free(indexBufs[i]);
    free(vertBufs[i]);
  }
  return result;
  #undef SPLIT_COPY_VERT_THRESHOLD
}

/* CoD4 0x449470 (middle address 0x449590).
================
EmitTriSoupRecord

Emits a triangle soup record to BSP arrays,
validates vertex and index counts
================
*/
intptr_t EmitTriSoupRecord(TriSoup_t *ts)
{
  BspTriSoup_t *tris;

  Assert(ts->numVerts, s_assertDisable_EmitTriSoupRecord);
  Assert(ts->numIndices, s_assertDisable_EmitTriSoupRecord);
  if ( numBSPTriSoups == MAX_MAP_TRISOUPS )
    Com_Error("MAX_MAP_TRIANGLE_SURFS");
  if ( numBSPDrawIndexes + ts->numIndices > MAX_MAP_DRAW_INDEXES )
    Com_Error("MAX_MAP_DRAW_INDICES");

  tris = &bspTriangles[numBSPTriSoups++];
  tris->materialIndex = ts->materialSort;
  tris->lightmapIndex = ts->lightmapSort;
  tris->reflectionProbeIndex = g_emitReflectionProbeIndex;
  tris->primaryLightIndex = g_emitPrimaryLightIndex;
  tris->castsSunShadow = CastsSunShadowConfirmed(g_emitCastsSunShadow);
  tris->firstVertex = ts->firstVert;
  tris->vertexCount = (unsigned short)ts->numVerts;
  tris->indexCount = (unsigned short)ts->numIndices;

  AssertMsg(tris->vertexCount == ts->numVerts, s_assertDisable_EmitTriSoupRecord, "%s\n\t(soup->vertCount) = %i", "(tris->vertexCount == soup->vertCount)", ts->numVerts);
  AssertMsg(tris->indexCount == ts->numIndices, s_assertDisable_EmitTriSoupRecord, "%s\n\t(soup->indexCount) = %i", "(tris->indexCount == soup->indexCount)", ts->numIndices);

  tris->firstIndex = ts->firstIndex;

  Assert(tris->vertexCount == ts->numVerts, s_assertDisable_EmitTriSoupRecord);
  Assert(tris->indexCount == ts->numIndices, s_assertDisable_EmitTriSoupRecord);

  numBSPDrawIndexes += tris->indexCount;
  AssertFatal(numBSPDrawVertsEmitted == tris->firstVertex || numBSPDrawVertsEmitted == tris->firstVertex + tris->vertexCount, s_assertDisable_EmitTriSoupRecord);
  numBSPDrawVertsEmitted = tris->firstVertex + tris->vertexCount;

  if ( numBSPDrawVertsEmitted > MAX_MAP_DRAW_VERTS )
    Com_Error("MAX_MAP_DRAW_VERTS (%i) exceeded\n", MAX_MAP_DRAW_VERTS);
  return numBSPDrawVertsEmitted;
}

/* CoD4 0x448230.
================
PartitionTriSoupRecursive

Recursively partitions triangle soup by longest axis,
emits record when small enough
================
*/
/* CoD4 0x449710. */
static void SortTriSoupAxes(const float extents[3], int axes[3])
{
  if (extents[1] < extents[0])
  {
    if (extents[2] < extents[0])
    {
      if (extents[2] < extents[1])
      {
        axes[0] = 2;
        axes[1] = 1;
      }
      else
      {
        axes[0] = 1;
        axes[1] = 2;
      }
      axes[2] = 0;
    }
    else
    {
      axes[0] = 1;
      axes[1] = 0;
      axes[2] = 2;
    }
  }
  else if (extents[2] < extents[1])
  {
    if (extents[2] < extents[0])
    {
      axes[0] = 2;
      axes[1] = 0;
    }
    else
    {
      axes[0] = 0;
      axes[1] = 2;
    }
    axes[2] = 1;
  }
  else
  {
    axes[0] = 0;
    axes[1] = 1;
    axes[2] = 2;
  }
}

char PartitionTriSoupRecursive(TriSoup_t *ts, char tryGroupSplit)
{
  #define MAX_SOUP_VERTS   5450
  #define MAX_SOUP_INDICES 384
  #define MAX_SOUP_INDICES_HARD 32704
  int i, axes[3];
  vec3_t mins, maxs, extents;

  if ( ts->numVerts <= MAX_SOUP_VERTS && ts->numIndices <= MAX_SOUP_INDICES )
    return (unsigned char)EmitTriSoupRecord(ts);

  if ( tryGroupSplit && GroupTriangles(ts) )
    return 1;

  /* compute bounds and extents */
  ClearBounds(mins, maxs);
  for ( i = 0; i < ts->numIndices; i++ )
    AddPointToBounds(bspTriSoupData[ts->firstVert + (unsigned short)bspDrawIndexes[i + ts->firstIndex]].pos, mins, maxs);
  VectorSubtract(maxs, mins, extents);

  /* Native uses strict comparisons, preserving its deterministic order when
     two extents are equal. */
  SortTriSoupAxes(extents, axes);

  /* try splitting on each axis, longest first */
  for ( i = 0; i < 3; i++ )
  {
    float split = MIDF(mins[axes[i]], maxs[axes[i]]);
    if ( SplitSoupOnAxis(ts, axes[i], split) )
      return 1;
  }

  if ( ts->numVerts > MAX_SOUP_VERTS || ts->numIndices > MAX_SOUP_INDICES_HARD )
    Com_Error("couldn't partition large draw surface on any axis");
  return (unsigned char)EmitTriSoupRecord(ts);
  #undef MAX_SOUP_VERTS
  #undef MAX_SOUP_INDICES
  #undef MAX_SOUP_INDICES_HARD
}

/* CoD4 0x446110.
================
EmitTriangleSoup

Emits triangles as draw verts, stitches UVs,
merges duplicates, generates tangents, then partitions
================
*/
int EmitTriangleSoup(TriRecord_t *triRecs, int triCount, Tree_t *bspTree)
{
  TriRecord_t *firstRec, *rec;
  int triIdx, batchEnd, v;
  unsigned short *indexBase;
  ShaderInfo_t *mat;
  TangentSources_t tangentData;
  TriSoup_t soup;
  float plane[4];

  tangentData.posStride = sizeof(BspDrawVert_t);
  tangentData.normalStride = sizeof(BspDrawVert_t);
  tangentData.uvStride = sizeof(BspDrawVert_t);
  tangentData.tangentStride = sizeof(BspDrawVert_t);
  tangentData.bitangentStride = sizeof(BspDrawVert_t);

  triIdx = 0;
  while ( triIdx < triCount )
  {
    firstRec = &triRecs[triIdx];
    mat = firstRec->si;
    soup.materialSort = EmitMaterial(mat, mat->contentFlags);
    soup.lightmapSort = (unsigned short)firstRec->lightStyle;
    g_emitReflectionProbeIndex = firstRec->reflectionProbeIndex;
    g_emitPrimaryLightIndex = firstRec->primaryLightIndex;
    g_emitCastsSunShadow = firstRec->castsSunShadow;
    soup.firstVert = numBSPDrawVertsEmitted;
    soup.numVerts = 0;
    soup.firstIndex = numBSPDrawIndexes;
    soup.numIndices = 0;

    /* emit all triangles in this group */
    for ( batchEnd = triIdx; batchEnd < triCount; batchEnd++ )
    {
      rec = &triRecs[batchEnd];
      if ( rec->groupId != firstRec->groupId )
        break;

      /* emit 3 vertices + 3 indices */
      g_emitDrawVertCount = &soup;
      for ( v = 0; v < 3; v++ )
      {
        g_emitDrawVertCount = &soup;
        bspDrawIndexes[soup.firstIndex + soup.numIndices] = EmitDrawVert(0, &DRAW_VERT(rec->vertIdx[v]));
        soup.numIndices++;
      }

      /* clip into BSP tree for cull group bounds */
      if ( !g_currentEntityIndex && firstRec->cullGroupIdx >= 0 )
      {
        Winding_t *w = AllocWinding(3);
        w->numpoints = 3;
        for ( v = 0; v < 3; v++ )
        {
          VectorCopy(DRAW_VERT(rec->vertIdx[v]).pos, w->points[v]);
          AddPointToBounds(w->points[v], bspCullGroups[firstRec->cullGroupIdx].mins, bspCullGroups[firstRec->cullGroupIdx].maxs);
        }
        PlaneFromPoints(plane, w->points[0], w->points[1], w->points[2]);
        ClipSideIntoTree_r(w, plane, rec, bspTree->headnode);
      }
    }
    Assert(soup.numVerts > 0, s_assertDisable_EmitTriangleSoup);

    /* stitch UVs for non-lightmapped surfaces and merge duplicates */
    indexBase = &bspDrawIndexes[soup.firstIndex];
    soup.numVerts = StitchAndMergeTriangleVerts(&bspTriSoupData[soup.firstVert],
                                                  soup.numVerts, indexBase,
                                                  soup.numIndices, firstRec,
                                                  firstRec->lightStyle);
    Assert(soup.numVerts > 0, s_assertDisable_EmitTriangleSoup);

    /* generate tangent space */
    { BspDrawVert_t *base = &bspTriSoupData[soup.firstVert];
    tangentData.pos       = (char *)base->pos;
    tangentData.normal    = (char *)base->normal;
    tangentData.uv        = (char *)base->uv;
    tangentData.tangent   = (char *)base->binormal;
    tangentData.bitangent = (char *)base->tangent; }
    TangentSpaceGenerate(&tangentData, soup.numVerts, indexBase, soup.numIndices);

    PartitionTriSoupRecursive(&soup, 1);
    triIdx = batchEnd;
  }
  return triCount;
}

/* CoD4 0x449840.
================
BuildTriSoupAABBTree

Builds AABB tree for triangle soups,
copies tree nodes to global array
================
*/
void BuildTriSoupAABBTree(BspAabbTreeEntry_t *treeBase)
{
  /* aabbBuilderNode_t */
  typedef struct aabbBuilderNode_s {
      int firstTriSoup; /* [0]  first trisoup index */
      int triSoupCount; /* [4]  number of trisoups */
      int _pad;         /* [8]  padding */
      int childIndex;   /* [12] child node index */
  } aabbBuilderNode_t;
  int soupCount, i, j, treeNodeCount, nodeBase;
  vec3_t minsBuf[MAX_MAP_TRISOUPS], maxsBuf[MAX_MAP_TRISOUPS];
  AabbTreeBuilder_t tb;

  soupCount = treeBase->triSoupCount;

  if ( soupCount <= 16 )
  {
    treeBase->childIndex = 0;
    Assert(treeBase == &bspAabbTrees[numBSPAabbTrees], s_assertDisable_BuildTriSoupAABBTree);
    numBSPAabbTrees++;
    return;
  }

  /* set up tree builder */
  tb.itemData = &bspTriangles[treeBase->firstTriSoup];
  tb.itemCount = soupCount;
  tb.itemStride = sizeof(bspTriangles[0]);
  tb.hasBoundsData = 0;
  tb.itemMins = (soupCount > MAX_MAP_TRISOUPS) ? malloc(sizeof(vec3_t) * soupCount) : (float *)minsBuf;
  tb.itemMaxs = (soupCount > MAX_MAP_TRISOUPS) ? malloc(sizeof(vec3_t) * soupCount) : (float *)maxsBuf;
  tb.nodes = malloc(sizeof(aabbBuilderNode_t) * MAX_MAP_AABBTREES);
  tb.maxNodes = MAX_MAP_AABBTREES;
  tb.minPartitionSize = TRISOUP_AABB_MIN_PARTITION;
  tb.minLeafItems = TRISOUP_AABB_MIN_LEAF;

  /* compute per-soup bounding boxes */
  for ( i = 0; i < soupCount; i++ )
  {
    BspTriSoup_t *soup = &bspTriangles[treeBase->firstTriSoup + i];
    float *mn = tb.itemMins + i * 3;
    float *mx = tb.itemMaxs + i * 3;
    ClearBounds(mn, mx);
    for ( j = 0; j < soup->vertexCount; j++ )
      AddPointToBounds(bspTriSoupData[soup->firstVertex + j].pos, mn, mx);
  }

  treeNodeCount = AabbBuildTree(&tb);
  if ( numBSPAabbTrees + treeNodeCount > MAX_MAP_AABBTREES )
    Com_Error("MAX_MAP_AABBTREES (%i) exceeded\n", MAX_MAP_AABBTREES);

  /* copy tree nodes to global array */
  nodeBase = numBSPAabbTrees;
  { aabbBuilderNode_t *src = (aabbBuilderNode_t *)tb.nodes;
  for ( i = 0; i < treeNodeCount; i++, src++ )
  {
    bspAabbTrees[nodeBase + i].firstTriSoup = treeBase->firstTriSoup + src->firstTriSoup;
    bspAabbTrees[nodeBase + i].triSoupCount = src->triSoupCount;
    bspAabbTrees[nodeBase + i].childIndex = src->childIndex;
  } }
  numBSPAabbTrees += treeNodeCount;

  free(tb.nodes);
  if ( tb.itemMins != (float *)minsBuf )
  {
    free(tb.itemMins);
    free(tb.itemMaxs);
  }
}

/*
================
Tris_AddNoDrawShadowTris

Adds shadow triangles for nodraw surfaces
that cast shadows
================
*/
int Tris_AddNoDrawShadowTris( int triCount, TriRecord_t *triData )
{
  int shadowType, i;

  shadowType = ShouldCastShadow(triData->si);
  if ( !shadowType )
    return 0;

  for ( i = 0; i < triCount; i++ )
  {
    AddShadowTri(shadowType,
      DRAW_VERT(triData[i].vertIdx[0]).pos,
      DRAW_VERT(triData[i].vertIdx[1]).pos,
      DRAW_VERT(triData[i].vertIdx[2]).pos);
  }
  return shadowType;
}

/*
================
Tris_EmitTriangles

Sorts triangles by material, groups adjacent tris,
emits triangle soups, and builds AABB trees
================
*/
void Tris_EmitTriangles(double unusedFpu, Tree_t *bspTree, int *indexMap)
{
  #define MAX_BATCH_TRIS 21845
  TriRecord_t *startRec;
  int i, batchStart, batchEnd, batchSize;
  int curBrushModel, curCullGroup;
  int hasAABBTree;

  /* init cull group bounds and cell AABB indices */
  if ( !g_currentEntityIndex )
  {
    memset(g_brushSideBitmap, 0, sizeof(g_brushSideBitmap));
    for ( i = 0; i < numBSPCullGroups; i++ )
      ClearBounds(bspCullGroups[i].mins, bspCullGroups[i].maxs);
    for ( i = 0; i < numBSPCells; i++ )
      bspCells[i].aabbTreeIndex = -1;
  }

  /* Keep the proven qsort route for small expanded-record batches.  The
     native VS2005 introsort carrier is isolated to larger batches, where it
     never changes the sealed fixture's <=32 ordering semantics. */
  if (numTriangles > 32)
    Tris_SortLargeDirectRecordCarrier(triRecords, (int)numTriangles);
  else
    qsort(triRecords, numTriangles, sizeof(TriRecord_t),
      (int (*)(const void *, const void *))CompareTrisByMaterialAndVertexIndices);


  curBrushModel = -1;
  curCullGroup = -1;
  hasAABBTree = 0;

  for ( batchStart = 0; batchStart < (int)numTriangles; batchStart = batchEnd )
  {
    startRec = &triRecords[batchStart];

    /* find end of batch: same drawOrder, cullGroup, material, lightStyle */
    for ( batchEnd = batchStart + 1; batchEnd < (int)numTriangles && batchEnd - batchStart < MAX_BATCH_TRIS; batchEnd++ )
    {
      TriRecord_t *r = &triRecords[batchEnd];
      if ( r->drawOrder != startRec->drawOrder || r->cullGroupIdx != startRec->cullGroupIdx
        || r->si != startRec->si || r->lightStyle != startRec->lightStyle
        || r->reflectionProbeIndex != startRec->reflectionProbeIndex
        || r->primaryLightIndex != startRec->primaryLightIndex
        || r->castsSunShadow != startRec->castsSunShadow )
        break;
    }
    batchSize = batchEnd - batchStart;

    /* nodraw surfaces only emit shadows */
    if ( (startRec->si->surfaceFlags & SURF_NODRAW) != 0 )
    {
      if ( !g_currentEntityIndex )
        Tris_AddNoDrawShadowTris(batchSize, startRec);
      continue;
    }

    Assert(batchSize >= 1, s_assertDisable_Tris_EmitTriangles);

    /* track cull group / brush model transitions */
    if ( !g_currentEntityIndex )
    {
      int drawOrder = startRec->drawOrder;
      if ( drawOrder < 0 )
      {
        /* cull group */
        Assert(startRec->cullGroupIdx >= 0, s_assertDisable_Tris_EmitTriangles);
        if ( curCullGroup != startRec->cullGroupIdx )
        {
          curCullGroup = startRec->cullGroupIdx;
          curBrushModel = -1;
          bspCullGroups[curCullGroup].firstTriSoup = numBSPTriSoups;
          bspCullGroups[curCullGroup].triSoupCount = 0;
        }
      }
      else if ( curBrushModel != drawOrder )
      {
        /* new brush model cell */
        curBrushModel = drawOrder;
        curCullGroup = -1;
        if ( hasAABBTree )
          BuildTriSoupAABBTree(&bspAabbTrees[numBSPAabbTrees]);
        bspCells[drawOrder].aabbTreeIndex = numBSPAabbTrees;
        bspAabbTrees[numBSPAabbTrees].firstTriSoup = numBSPTriSoups;
        bspAabbTrees[numBSPAabbTrees].triSoupCount = 0;
        bspAabbTrees[numBSPAabbTrees].childIndex = 0;
        hasAABBTree = 1;
      }
    }

    /* group, merge subgroups, emit */
    GroupAndMergeTriSubgroups(startRec, batchSize, indexMap);
    EmitTriangleSoup(startRec, batchSize, bspTree);

    /* update soup counts */
    if ( !g_currentEntityIndex )
    {
      if ( startRec->drawOrder < 0 )
        bspCullGroups[startRec->cullGroupIdx].triSoupCount = numBSPTriSoups - bspCullGroups[startRec->cullGroupIdx].firstTriSoup;
      else
      {
        int aabbIdx = bspCells[startRec->drawOrder].aabbTreeIndex;
        bspAabbTrees[aabbIdx].triSoupCount = numBSPTriSoups - bspAabbTrees[aabbIdx].firstTriSoup;
      }
    }
  }

  if ( curBrushModel >= 0 && hasAABBTree )
    BuildTriSoupAABBTree(&bspAabbTrees[numBSPAabbTrees]);

  /* assign empty AABB trees to cells with no soups */
  if ( !g_currentEntityIndex )
  {
    for ( i = 0; i < numBSPCells; i++ )
    {
      if ( bspCells[i].aabbTreeIndex < 0 )
      {
        bspCells[i].aabbTreeIndex = numBSPAabbTrees;
        bspAabbTrees[numBSPAabbTrees].firstTriSoup = 0;
        bspAabbTrees[numBSPAabbTrees].triSoupCount = 0;
        bspAabbTrees[numBSPAabbTrees].childIndex = 0;
        BuildTriSoupAABBTree(&bspAabbTrees[numBSPAabbTrees]);
      }
    }
  }
  #undef MAX_BATCH_TRIS
}

/*
================
Tris_BuildMaterialRemapTable

Maps duplicate material names to first occurrence slot
================
*/
int Tris_BuildMaterialRemapTable(int materialIdx, int *remapTable, int nextSlot)
{
  int i;

  /* check if this material already appeared under a different slot */
  if ( materialIdx > 0 )
  {
    for ( i = 0; i < materialIdx; ++i )
    {
      if ( !_stricmp(bspMaterials[i].material, bspMaterials[materialIdx].material) )
      {
        remapTable[materialIdx] = remapTable[i];
        return nextSlot;
      }
    }
  }
  remapTable[materialIdx] = nextSlot;
  return nextSlot + 1;
}

/*
================
Tris_CopyVertexToDrawVerts

Copies a vertex to draw verts output array,
validates normal/binormal/tangent normalization
================
*/
int Tris_CopyVertexToDrawVerts(int srcIndex, int dstIndex)
{
  BspDrawVert_t *sv = &bspTriSoupData[srcIndex];
  BspDrawVert_t *dv;

  Assert(Vec3IsNormalized(sv->normal)   || s_assertDisable_Tris_CopyVertexToDrawVerts, s_assertDisable_Tris_CopyVertexToDrawVerts);
  Assert(Vec3IsNormalized(sv->tangent)  || s_assertDisable_Tris_CopyVertexToDrawVerts, s_assertDisable_Tris_CopyVertexToDrawVerts);
  Assert(Vec3IsNormalized(sv->binormal) || s_assertDisable_Tris_CopyVertexToDrawVerts, s_assertDisable_Tris_CopyVertexToDrawVerts);

  /* copy vertex, swapping tangent/binormal field order */
  dv = &bspDrawVerts[dstIndex];
  memcpy(dv, sv, offsetof(BspDrawVert_t, tangent));
  memcpy(dv->tangent, sv->binormal, sizeof(vec3_t));
  memcpy(dv->binormal, sv->tangent, sizeof(vec3_t));
  return sizeof(bspDrawVerts[0]) * dstIndex;
}

/*
================
Tris_ReorderAddSurfaceVerts

Adds surface verts to reorder map,
returns 0 if vertex limit exceeded
================
*/
char Tris_ReorderAddSurfaceVerts(intptr_t triSurf, int *indexBuf, int *indexCountPtr, int *vertCountPtr)
{
  BspTriSoup_t *soup = (BspTriSoup_t *)triSurf;
  int curIndexCount;
  int idxOffset;
  int vertIndex;
  int i;
  int curVertCount;
  char limitExceeded;

  curIndexCount = indexCountPtr[0];
  curVertCount = vertCountPtr[0];
  limitExceeded = 0;

  if ( !soup->indexCount )
  {
    indexCountPtr[0] = curIndexCount;
    vertCountPtr[0] = curVertCount;
    return 1;
  }

  /* remap each index to the reordered vertex array */
  for ( idxOffset = 0; idxOffset < soup->indexCount; idxOffset++ )
  {
    vertIndex = soup->firstVertex + (unsigned short)bspDrawIndexes[idxOffset + soup->firstIndex];
    Assert(vertIndex >= 0, s_assertDisable_Tris_ReorderAddSurfaceVerts);

    if ( reorderVertexRemap[vertIndex] < 0 )
    {
      if ( curVertCount >= MAX_DRAW_SURF_VERTS )
      {
        limitExceeded = 1;
        break;
      }
      reorderVertexRemap[vertIndex] = (unsigned short)curVertCount;
      Assert((unsigned short)curVertCount == curVertCount, s_assertDisable_Tris_ReorderAddSurfaceVerts);
      ++curVertCount;
    }
    indexBuf[curIndexCount] = reorderVertexRemap[vertIndex];
    ++curIndexCount;
  }

  /* roll back remap if vertex limit was exceeded */
  if ( limitExceeded )
  {
    for ( i = 0; i < numBSPDrawVertsEmitted; ++i )
    {
      if ( reorderVertexRemap[i] >= vertCountPtr[0] )
        reorderVertexRemap[i] = -1;
    }
    return 0;
  }
  indexCountPtr[0] = curIndexCount;
  vertCountPtr[0] = curVertCount;
  return 1;
}

/*
================
Tris_ReorderAndOptimizeVerts

Reorders and optionally D3DX-optimizes vertices
for triangle surfaces
================
*/
int Tris_ReorderAndOptimizeVerts(TrisReorderEntry_t *reorderList, int reorderCount)
{
  BspTriSoup_t *soup;
  int i, j, t, numProcessed, totalVerts, totalIndices;
  int firstIndex, surfCount, newFirstVert, oldVert, newIdx;
  int optimizedIndexBase;

  Assert(reorderList, s_assertDisable_Tris_ReorderAndOptimizeVerts);
  Assert(reorderCount > 0, s_assertDisable_Tris_ReorderAndOptimizeVerts);
  memset(reorderVertexRemap, 0xFF, sizeof(int) * numBSPDrawVertsEmitted);

  /* phase 1: gather surfaces that fit within the index limit */
  totalVerts = 0;
  totalIndices = 0;
  numProcessed = 0;
  for ( i = 0; i < reorderCount; i++ )
  {
    soup = reorderList[i].triSurf;
    Assert(soup, s_assertDisable_Tris_ReorderAndOptimizeVerts);
    if ( totalIndices + soup->indexCount > MAX_MAP_DRAW_VERTS )
      break;
    if ( !Tris_ReorderAddSurfaceVerts((intptr_t)soup, reorderIndexBuffer, &totalIndices, &totalVerts) )
      break;
    numProcessed = i + 1;
  }
  if ( !numProcessed )
    Com_Error("No valid vertex groups.  This is a code bug.\n");

  /* phase 2: optimize face/vertex order or build identity remap */
  if ( reorderTris )
  {
    /* per-surface face optimization → remap triangles into optimized buffer */
    firstIndex = 0;
    for ( i = 0; i < numProcessed; i++ )
    {
      soup = reorderList[i].triSurf;
      surfCount = soup->indexCount;
      Assert(firstIndex + surfCount <= MAX_MAP_DRAW_VERTS, s_assertDisable_Tris_ReorderAndOptimizeVerts);

      D3DXOptimizeFaces(&reorderIndexBuffer[firstIndex], surfCount / 3, totalVerts, 1, reorderOptimizeRemap);
      { unsigned short *optBuf = (unsigned short *)reorderOptimizedBuf;
      for ( t = 0; t < surfCount / 3; t++ )
      {
        int src = reorderOptimizeRemap[t];
        optBuf[3 * t + firstIndex]     = (unsigned short)reorderIndexBuffer[firstIndex + 3 * src];
        optBuf[3 * t + firstIndex + 1] = (unsigned short)reorderIndexBuffer[firstIndex + 3 * src + 1];
        optBuf[3 * t + firstIndex + 2] = (unsigned short)reorderIndexBuffer[firstIndex + 3 * src + 2];
      } }
      firstIndex += surfCount;
    }
    D3DXOptimizeVertices((unsigned short *)reorderOptimizedBuf, totalIndices / 3, totalVerts, 0, reorderOptimizeRemap);
  }
  else
  {
    /* no optimization — identity remap */
    for ( i = 0; i < totalIndices; i++ )
      reorderOptimizeRemap[i] = (unsigned short)i;
  }

  /* D3DXOptimizeVertices returns the native old vertex index for each new
     vertex slot.  CoD4 0x44A991 explicitly inverts that table before either
     copying vertices or rewriting indices.  Using the D3DX table directly
     only works for identity or self-inverse remaps. */
  for ( i = 0; i < totalVerts; ++i )
  {
    Assert((unsigned int)reorderOptimizeRemap[i] < (unsigned int)totalVerts,
      s_assertDisable_Tris_ReorderAndOptimizeVerts);
    reorderInverseRemap[reorderOptimizeRemap[i]] = i;
  }

  /* phase 3: copy vertices to draw vert buffer in optimized order */
  newFirstVert = numBSPDrawVerts;
  for ( i = 0; i < numBSPDrawVertsEmitted; i++ )
  {
    if ( reorderVertexRemap[i] >= 0 )
    {
      newIdx = reorderInverseRemap[reorderVertexRemap[i]];
      Assert(reorderVertexRemap[i] < totalVerts, s_assertDisable_Tris_ReorderAndOptimizeVerts);
      Assert((unsigned int)newIdx < (unsigned int)totalVerts, s_assertDisable_Tris_ReorderAndOptimizeVerts);
      Tris_CopyVertexToDrawVerts(i, newFirstVert + newIdx);
      numBSPDrawVerts++;
    }
  }
  Assert(numBSPDrawVerts == totalVerts + newFirstVert, s_assertDisable_Tris_ReorderAndOptimizeVerts);

  /* phase 4: remap draw indices and update soup records */
  optimizedIndexBase = 0;
  for ( i = 0; i < numProcessed; i++ )
  {
    soup = reorderList[i].triSurf;
    for ( j = 0; j < soup->indexCount; j++ )
    {
      oldVert = soup->firstVertex + (unsigned short)bspDrawIndexes[j + soup->firstIndex];
      /* D3DXOptimizeFaces has already placed the source faces in
         reorderOptimizedBuf.  The donor port only remapped the old stream,
         discarding that face order; CoD4 writes the optimized stream. */
      if ( reorderTris )
        newIdx = reorderInverseRemap[
            ((unsigned short *)reorderOptimizedBuf)[optimizedIndexBase + j]];
      else
        newIdx = reorderInverseRemap[reorderVertexRemap[oldVert]];
      Assert((unsigned int)newIdx < (unsigned int)totalVerts, s_assertDisable_Tris_ReorderAndOptimizeVerts);
      bspDrawIndexes[j + soup->firstIndex] = (unsigned short)newIdx;
      Assert((unsigned short)bspDrawIndexes[j + soup->firstIndex] == newIdx, s_assertDisable_Tris_ReorderAndOptimizeVerts);
    }
    soup->vertexCount = totalVerts;
    soup->firstVertex = newFirstVert;
    optimizedIndexBase += soup->indexCount;
  }
  return numProcessed;
}

/*
================
Tris_ReorderSortCompare

Qsort comparator: by material ascending,
then index count descending
================
*/
int Tris_ReorderSortCompare(const void *a, const void *b)
{
  const TrisReorderEntry_t *entry0 = (const TrisReorderEntry_t *)a;
  const TrisReorderEntry_t *entry1 = (const TrisReorderEntry_t *)b;
  int d;

  d = entry0->minVert - entry1->minVert; /* native record +8, ascending */
  if ( d ) return d;
  return entry1->maxVert - entry0->maxVert; /* native record +12, descending */
}

/*
================
Tris_ReorderTriSurfaces

Sorts and batches surfaces for vertex reordering
================
*/
static void Tris_ReorderTriSurfaces(signed int reorderCount, int entityIndex, TrisReorderEntry_t *list)
{
  int startIdx, batchUsed;

  Assert(list, s_assertDisable_Tris_ReorderTriSurfaces);
  Assert(reorderCount > 0, s_assertDisable_Tris_ReorderTriSurfaces);
  /* Native 0x44A550: qsort(list, reorderCount, 0x10, 0x44B790). */
  qsort(list, reorderCount, sizeof(*list), Tris_ReorderSortCompare);

  for ( startIdx = 0; startIdx < reorderCount; startIdx += batchUsed )
  {
    batchUsed = Tris_ReorderAndOptimizeVerts(&list[startIdx], reorderCount - startIdx);
    Assert(batchUsed > 0, s_assertDisable_Tris_ReorderTriSurfaces);
    Assert(startIdx + batchUsed <= reorderCount, s_assertDisable_Tris_ReorderTriSurfaces);
    if ( entityIndex && batchUsed != reorderCount )
      Com_Error("brush model entity %i uses more than 65536 vertices of material '%s'",
        entityIndex, bspMaterials[list[0].triSurf->materialIndex].material);
  }
}

/*
================
Tris_FindNextEntity

Finds next entity with non-zero firstTriSurf
================
*/
int Tris_FindNextEntity( int entityIndex )
{
  int next;

  /* scan forward for the next entity that has triangle soups */
  for ( next = entityIndex + 1; next < num_entities; next++ )
  {
    if ( g_entities[next].firstTriSoup )
      break;
  }

  Assert(next == num_entities || g_entities[next].firstTriSoup > g_entities[entityIndex].firstTriSoup, s_assertDisable_Tris_FindNextEntity);
  return next;
}

/*
================
Tris_ReorderDrawVerts

Top-level function that reorders draw verts
by material and entity
================
*/
void Tris_ReorderDrawVerts(void)
{
  BspTriSoup_t *soups = bspTriangles;
  TrisReorderEntry_t surfReorderList[MAX_MAP_TRISOUPS];
  int materialRemapTable[MAX_MAP_MATERIALS];
  int mat, slot, nextSlot, remapSlot;
  int reorderCount, curEntity, nextEntity;
  int s, k;

  Assert(numBSPTriSoups <= MAX_MAP_TRISOUPS, s_assertDisable_Tris_ReorderDrawVerts);

  /* allocate reorder work buffers */
  reorderVertexRemap = malloc(sizeof(int) * numBSPDrawVertsEmitted);
  #define REORDER_BUF_SIZE     0x200000  /* 2MB per remap/index buffer */
  #define REORDER_OPT_BUF_SIZE 0x100000  /* 1MB optimized output buffer */
  reorderOptimizeRemap = malloc(REORDER_BUF_SIZE);
  reorderInverseRemap = malloc(REORDER_BUF_SIZE);
  reorderIndexBuffer = malloc(REORDER_BUF_SIZE);
  reorderOptimizedBuf = malloc(REORDER_OPT_BUF_SIZE);

  /* build material dedup remap table */
  remapSlot = 0;
  for ( mat = 0; mat < numBSPMaterials; mat++ )
    remapSlot = Tris_BuildMaterialRemapTable(mat, materialRemapTable, remapSlot);

  /* process each unique material slot */
  nextSlot = 0;
  for ( mat = 0; mat < numBSPMaterials; mat++ )
  {
    slot = materialRemapTable[mat];
    if ( slot != nextSlot )
      continue;
    ++nextSlot;

    /* collect all soups with this material, batched by entity */
    reorderCount = 0;
    curEntity = 0;
    nextEntity = Tris_FindNextEntity(0);

    for ( s = 0; s < numBSPTriSoups; s++ )
    {
      /* check for entity boundary */
      if ( nextEntity != num_entities && g_entities[nextEntity].firstTriSoup == s )
      {
        if ( reorderCount )
        {
          Tris_ReorderTriSurfaces(reorderCount, curEntity, surfReorderList);
          reorderCount = 0;
        }
        curEntity = nextEntity;
        nextEntity = Tris_FindNextEntity(nextEntity);
      }

      /* skip soups that don't match this material */
      if ( materialRemapTable[soups[s].materialIndex] != slot )
        continue;

      Assert(soups[s].indexCount, s_assertDisable_Tris_ReorderDrawVerts);

      /* add soup to reorder list with min/max vertex index range */
      {
        TrisReorderEntry_t *entry = &surfReorderList[reorderCount];
        int minVert, maxVert;

        entry->triSurf = &soups[s];
        entry->context = s_drawSurfaceContext;

        /* find min/max draw index for this soup */
        minVert = (unsigned short)bspDrawIndexes[soups[s].firstIndex];
        maxVert = minVert;
        for ( k = 1; k < soups[s].indexCount; k++ )
        {
          int idx = (unsigned short)bspDrawIndexes[soups[s].firstIndex + k];
          if ( idx < minVert ) minVert = idx;
          if ( idx > maxVert ) maxVert = idx;
        }

        /* offset by firstVertex to get global vertex indices */
        entry->minVert = minVert + soups[s].firstVertex;
        entry->maxVert = maxVert + soups[s].firstVertex;
        ++reorderCount;
      }
    }

    if ( reorderCount )
      Tris_ReorderTriSurfaces(reorderCount, curEntity, surfReorderList);
  }

  free(reorderVertexRemap);
  free(reorderOptimizeRemap);
  free(reorderInverseRemap);
  free(reorderIndexBuffer);
  free(reorderOptimizedBuf);
}

/* Reorder the second, unlayered family with its own entity range markers.
   The shared reorder implementation intentionally remains context-agnostic. */
void Tris_ReorderUnlayeredDrawVerts(void)
{
  int savedFirstTriSoups[MAX_MAP_ENTITIES];
  int i;

  for ( i = 0; i < num_entities; ++i )
  {
    savedFirstTriSoups[i] = g_entities[i].firstTriSoup;
    g_entities[i].firstTriSoup = g_entities[i].firstTriSoupSimple;
  }

  Tris_SelectDrawSurfaceContext(1);
  if ( numBSPTriSoups )
    Tris_ReorderDrawVerts();
  Tris_SelectDrawSurfaceContext(0);

  for ( i = 0; i < num_entities; ++i )
    g_entities[i].firstTriSoup = savedFirstTriSoups[i];
}

/*
================
TryMergeWindingPair

Tries merging winding pairs that share edges
================
*/
int TryMergeWindingPair( int startIdx, intptr_t *surfArray, IntWinding_t **windingArray, int count, int epsilon )
{
  IntWinding_t *curWinding;
  IntWinding_t *splicedWinding;
  int i, sharedCount, edgeA, edgeB;

  curWinding = windingArray[startIdx];
  if ( startIdx + 1 >= count )
    return count;

  for ( i = startIdx + 1; i < count; )
  {
    TriSurf_t *curSurf = (TriSurf_t *)surfArray[startIdx];
    TriSurf_t *otherSurf = (TriSurf_t *)surfArray[i];
    IntWinding_t *otherWinding = windingArray[i];

    if ( !TrisCanCoalesceWindings(curSurf, otherSurf)
      || !(sharedCount = FindSharedEdge(curWinding, otherWinding, &edgeA, &edgeB)) )
    {
      i++;
      continue;
    }

    /* if current is fully contained, swap so we keep the larger one */
    if ( sharedCount == curWinding->numpoints )
    {
      intptr_t tmp = surfArray[startIdx];
      surfArray[startIdx] = surfArray[i];
      surfArray[i] = tmp;
      windingArray[startIdx] = otherWinding;
      windingArray[i] = curWinding;
      curWinding = windingArray[startIdx];
      otherWinding = windingArray[i];
      { int adjEdge = (edgeA + sharedCount - 1) % otherWinding->numpoints;
      edgeA = (curWinding->numpoints - sharedCount + edgeB + 1) % curWinding->numpoints;
      edgeB = adjEdge; }
    }

    if ( sharedCount == otherWinding->numpoints )
    {
      /* fully consumed — remove shared boundary */
      RemoveSharedBoundaryPoints(curWinding, epsilon, edgeA, sharedCount);
      FreeTriSurf((TriSurf_t *)surfArray[i]);
      FreeIntWinding(otherWinding);
      count--;
      memmove(&surfArray[i], &surfArray[i + 1], sizeof(intptr_t) * (count - i));
      memmove(&windingArray[i], &windingArray[i + 1], sizeof(IntWinding_t *) * (count - i));
    }
    else
    {
      /* partial overlap — splice */
      splicedWinding = SpliceWindingsAtSharedEdge(curWinding, otherWinding, epsilon, startIdx, i, edgeA, edgeB, sharedCount);
      AddBoundsToBounds(otherSurf->mins, otherSurf->maxs, curSurf->mins, curSurf->maxs);
      FreeTriSurf((TriSurf_t *)surfArray[i]);
      FreeIntWinding(curWinding);
      FreeIntWinding(otherWinding);
      curWinding = splicedWinding;
      windingArray[startIdx] = splicedWinding;
      count--;
      memmove(&surfArray[i], &surfArray[i + 1], sizeof(intptr_t) * (count - i));
      memmove(&windingArray[i], &windingArray[i + 1], sizeof(IntWinding_t *) * (count - i));
    }
    windingArray[count] = NULL;
    i = startIdx + 1;  /* restart scan after merge */
  }

  return count;
}

/*
================
ValidateTriSurfLmapCoords

Validates lightmap UVs are within [0,1] range
================
*/
void ValidateTriSurfLmapCoords(Winding_t *winding, TriSurfProps_t *material)
{
  #define LMAP_UV_EPSILON (0.01f / LIGHTMAP_SIZE)
  #define LMAP_UV_MIN  (-LMAP_UV_EPSILON)
  #define LMAP_UV_MAX  (1.0f + LMAP_UV_EPSILON)
  float *pos, lmapU, lmapV;
  int i;

  if ( !winding || material->lmapIndex == LMAP_ALL_STYLES )
    return;

  Assert(material->lmapIndex < LMAP_ALL_STYLES, s_assertDisable_ValidateTriSurfLmapCoords);

  for ( i = 0; i < winding->numpoints; i++ )
  {
    pos = winding->points[i];
    lmapU = DotProduct102(pos, &material->lmapVecs[0]) + material->lmapVecs[3];
    lmapV = DotProduct210(pos, &material->lmapVecs[4]) + material->lmapVecs[7];
    Assert(lmapU >= LMAP_UV_MIN && lmapU <= LMAP_UV_MAX, s_assertDisable_ValidateTriSurfLmapCoords);
    Assert(lmapV >= LMAP_UV_MIN && lmapV <= LMAP_UV_MAX, s_assertDisable_ValidateTriSurfLmapCoords);
  }
  #undef LMAP_UV_EPSILON
  #undef LMAP_UV_MIN
  #undef LMAP_UV_MAX
}

/* Native 0x451590.  Coalesced properties are a material set, so a composite
   surface participates in the lightmap validators when any constituent does. */
static bool Tris_SurfaceHasLightmap(const TriSurfProps_t *props)
{
  const CoalesceNode_t *node;

  Assert(props, s_assertDisable_ValidateTriSurfLmapCoords);
  if (!props->coalesceChain)
    return props->si && (props->si->gameFlags & 2) != 0;

  for (node = props->coalesceChain; node; node = node->next)
  {
    if (Tris_SurfaceHasLightmap((const TriSurfProps_t *)node->value))
      return true;
  }
  return false;
}

/* CoD4 0x43EF60.  The native props carrier has source map/entity/brush
   fields at this point.  KIWI's fixed 0xAC props carrier has no room for
   them, but its winding still supplies the exact error position and plane;
   use the active entity's safe map context without inventing a brush. */
static void ReportTriSurfValidationError(const TriSurf_t *surf,
                                         const TriSurfProps_t *props,
                                         const char *message)
{
  int mapInfoIndex = 0;
  int entityNum = -1;

  (void)props;
  if (g_currentEntityIndex >= 0 && g_currentEntityIndex < num_entities)
  {
    mapInfoIndex = g_entities[g_currentEntityIndex].mapInfoIndex;
    entityNum = g_entities[g_currentEntityIndex].entityNum;
  }
  WindingError(0, surf->winding, mapInfoIndex, entityNum, -1, "%s", message);
}

/* CoD4 0x43EDF0.  A layered chain reports through its first lightmapped
   leaf, preserving the native leaf-selection order. */
static void ValidateTriSurfLmapChain(const TriSurf_t *surf, const char *message)
{
  TriSurfProps_t *props;
  CoalesceNode_t *node;

  props = surf->props;
  if (props->coalesceChain)
  {
    for (node = props->coalesceChain; node; node = node->next)
    {
      TriSurfProps_t *leaf = (TriSurfProps_t *)node->value;
      if (Tris_SurfaceHasLightmap(leaf))
      {
        ReportTriSurfValidationError(surf, leaf, message);
        return;
      }
    }
    Assert(0, s_assertDisable_ValidateTriSurfLmapCoords);
  }
  else
  {
    Assert(Tris_SurfaceHasLightmap(props), s_assertDisable_ValidateTriSurfLmapCoords);
  }

  ReportTriSurfValidationError(surf, props, message);
}

/* CoD4 0x43EEE0. */
static bool ValidateTriSurfTextureRepeats(const TriSurf_t *surf,
                                          const TriSurfProps_t *props)
{
  int axis;

  for (axis = 0; axis < 2; ++axis)
  {
    const float *vec = props->texVecs + axis * 4;
    float magnitude = sqrtf(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2]);

    if (magnitude > 1.001000046730042f)
    {
      ReportTriSurfValidationError(surf, props, "Texture repeats too many times.");
      return false;
    }
  }
  return true;
}

/* CoD4 0x43F0F0. */
static bool ProjectedLightmapExtentTest(const Winding_t *winding,
                                        const float *direction, float scale,
                                        float tolerance)
{
  float minimum = FLT_MAX;
  float maximum = -FLT_MAX;
  int pointIndex;

  for (pointIndex = 0; pointIndex < winding->numpoints; ++pointIndex)
  {
    float projected = DotProduct(direction, winding->points[pointIndex]);
    if (projected < minimum)
      minimum = projected;
    if (projected > maximum)
      maximum = projected;
  }
  return tolerance + 0.001000000047497451f < (maximum - minimum) * scale;
}

/* CoD4 0x43EFA0. */
static bool ValidateTriSurfLightmapDegeneracy(const TriSurf_t *surf,
                                              const float *lmapVecs)
{
  float magnitudes[2];
  float cross[3];
  int axis;

  for (axis = 0; axis < 2; ++axis)
  {
    const float *vec = lmapVecs + axis * 4;
    magnitudes[axis] = sqrtf(vec[0] * vec[0] + vec[1] * vec[1] + vec[2] * vec[2]);
    if (magnitudes[axis] == 0.0f)
    {
      ValidateTriSurfLmapChain(surf,
        "Lightmap coordinates don't change in at least one direction; try the LMAP or NATURAL buttons in the surface dialog.");
      return false;
    }
    if (magnitudes[axis] > 0.001955078216269612f
      && ProjectedLightmapExtentTest(surf->winding, vec, 512.0f, 2.0f))
    {
      ValidateTriSurfLmapChain(surf, "Lightmap uses more than one pixel per inch.");
      return false;
    }
  }

  CrossProduct(lmapVecs, lmapVecs + 4, cross);
  if (magnitudes[0] * 0.001000000047497451f * magnitudes[1]
      <= fabsf(DotProduct(cross, surf->props->plane)))
    return true;

  ValidateTriSurfLmapChain(surf,
    "Lightmap coordinates are degenerate; try the LMAP or NATURAL buttons in the surface dialog.");
  return false;
}

/* CoD4 0x43EC20.  This is deliberately a post-coalesce callback: native
   validates every constituent's texture scale, then the composite lightmap
   vectors, and frees the first failing surface before primary-light work. */
static void ValidateTriSurfLmapCoordsNative(TriSurf_t *surf, bool isTarget)
{
  TriSurfProps_t *props;
  CoalesceNode_t *node;
  float mins[2];
  float maxs[2];
  int span[2];
  int pointIndex;
  char message[256];

  (void)isTarget;
  props = surf->props;
  if (props->coalesceChain)
  {
    for (node = props->coalesceChain; node; node = node->next)
    {
      if (!ValidateTriSurfTextureRepeats(surf, (TriSurfProps_t *)node->value))
      {
        FreeTriSurf(surf);
        return;
      }
    }
  }
  else if (!ValidateTriSurfTextureRepeats(surf, props))
  {
    FreeTriSurf(surf);
    return;
  }

  if (!Tris_SurfaceHasLightmap(props))
    return;
  if (!ValidateTriSurfLightmapDegeneracy(surf, props->lmapVecs))
  {
    FreeTriSurf(surf);
    return;
  }

  ClearBounds2D(mins, maxs);
  for (pointIndex = 0; pointIndex < surf->winding->numpoints; ++pointIndex)
  {
    float st[2];
    st[0] = DotProduct(surf->winding->points[pointIndex], props->lmapVecs) + props->lmapVecs[3];
    st[1] = DotProduct(surf->winding->points[pointIndex], props->lmapVecs + 4) + props->lmapVecs[7];
    AddPointToBounds2D(st, mins, maxs);
  }
  span[0] = fistp_add(maxs[0] * 512.0f, FISTP_HALF_BIAS) + 1
          - fistp_add(mins[0] * 512.0f, -FISTP_HALF_BIAS);
  span[1] = fistp_add(maxs[1] * 512.0f, FISTP_HALF_BIAS) + 1
          - fistp_add(mins[1] * 512.0f, -FISTP_HALF_BIAS);
  if (span[0] > 512 || span[1] > 512)
  {
    float maximum = (float)(span[0] > span[1] ? span[0] : span[1]) * (1.0f / 512.0f);
    float scale = ceilf(maximum * 10.0f / sampleScale) * 0.1f;

    _snprintf(message, sizeof(message),
      "Lightmap for surface needs to be %ix%i, which is larger than %ix%i; try multiplying sampleScale by at least %.1lf",
      span[0], span[1], 512, 512, scale);
    message[sizeof(message) - 1] = 0;
    ValidateTriSurfLmapChain(surf, message);
    FreeTriSurf(surf);
  }
}

/*
================
AllocTriSurf

Allocates and initializes a TriSurf_t struct.
================
*/
TriSurf_t *AllocTriSurf(Winding_t *winding, void *auxData, int flags, TriSurfProps_t *material)
{
  TriSurf_t *ts;

  ts = malloc(sizeof(*ts));
  memset(ts, 0, sizeof(*ts));
  ts->auxElemSize = flags;
  ts->winding = winding;
  ts->props = material;
  ts->auxData = auxData;
  ++numTriSurfs;
  return ts;
}

/*
================
CopyTriSurf

Copies a triSurf including winding and aux data
================
*/
TriSurf_t *CopyTriSurf( TriSurf_t *src )
{
  Winding_t *windingCopy;
  void *auxCopy = NULL;
  TriSurf_t *ts;

  windingCopy = CopyWinding(src->winding);

  /* copy aux data if present */
  if ( src->auxElemSize )
  {
    int auxSize = src->auxElemSize * src->winding->numpoints;
    auxCopy = malloc(auxSize);
    memcpy(auxCopy, src->auxData, auxSize);
  }

  /* Native 0x43D140 delegates carrier construction to 0x43D0F0.  The aux
     copy is KIWI's donor compatibility payload; it remains outside the
     native 0x40 prefix. */
  ts = AllocTriSurf(windingCopy, auxCopy, src->auxElemSize, src->props);
  VectorCopy(src->mins, ts->mins);
  VectorCopy(src->maxs, ts->maxs);
  TrisLmap_CopyAssignmentsForSurface(src, ts);
  Assert(!src->holeCount, s_assertDisable_CopyTriSurf);
  return ts;
}

/*
================
PrependTriSurf

Prepends a new triSurf to the head of a linked list
================
*/
TriSurf_t *PrependTriSurf( Winding_t *winding, void *auxData, int flags, TriSurfProps_t *material, TriSurf_t **listHead )
{
  TriSurf_t *ts;

  Assert(listHead, s_assertDisable_PrependTriSurf);
  ts = malloc(sizeof(*ts));
  memset(ts, 0, sizeof(*ts));
  ts->winding = winding;
  ts->auxData = auxData;
  ts->props = material;
  ts->auxElemSize = flags;
  ++numTriSurfs;

  /* prepend to doubly-linked list */
  ts->prev = NULL;
  ts->next = *listHead;
  if ( ts->next )
    ts->next->prev = ts;
  *listHead = ts;

  return ts;
}

/*
================
InsertTriSurfAfter

Inserts a new triSurf after a given node in the linked list
================
*/
TriSurf_t *InsertTriSurfAfter( Winding_t *winding, void *auxData, int flags, TriSurfProps_t *material, TriSurf_t *tsAfter )
{
  TriSurf_t *ts;

  Assert(tsAfter, s_assertDisable_InsertTriSurfAfter);
  ts = malloc(sizeof(*ts));
  memset(ts, 0, sizeof(*ts));
  ts->winding = winding;
  ts->auxData = auxData;
  ts->props = material;
  ts->auxElemSize = flags;
  ++numTriSurfs;

  /* insert after tsAfter in doubly-linked list */
  ts->prev = tsAfter;
  ts->next = tsAfter->next;
  tsAfter->next = ts;
  if ( ts->next )
    ts->next->prev = ts;

  return ts;
}

/* CoD4 0x43D3B0.  This is the fixed-winding insertion path used by the
   coalescer.  The KIWI auxData carrier owns a fixed-source copy, while the
   converted floating winding occupies the native w slot. */
static TriSurf_t *CopyAndInsertTriSurfAfter(const FixedWinding_t *fixedWinding,
                                            TriSurfProps_t *props,
                                            TriSurf_t *prev)
{
  FixedWinding_t *fixedCopy;
  Winding_t *winding;
  TriSurf_t *surf;
  uint32_t i;

  Assert(prev, s_assertDisable_InsertTriSurfAfter);
  Assert(fixedWinding, s_assertDisable_InsertTriSurfAfter);

  winding = AllocWinding(fixedWinding->ptCount);
  winding->numpoints = fixedWinding->ptCount;
  for ( i = 0; i < fixedWinding->ptCount; ++i )
    FixedVec3ToFloat((FixedVec3_t *)&fixedWinding->points[i], winding->points[i]);

  /* 0x43D3B0 gives AllocTriSurf an owned fixed-winding copy.  The KIWI
     auxData carrier is released by FreeTriSurf, so retaining the caller's
     borrowed source here would make this insertion path double-free it. */
  fixedCopy = CopyFixedWinding(fixedWinding);
  surf = AllocTriSurf(winding, fixedCopy, 0, props);
  surf->prev = prev;
  surf->next = prev->next;
  prev->next = surf;
  if ( surf->next )
    surf->next->prev = surf;
  return surf;
}

/*
================
EmitPatchSurface

Recursively clips a patch winding through the BSP tree
and emits triSurfs at leaf nodes
================
*/
void EmitPatchSurface( int *Block, TriSurfProps_t *material, Node_t *node )
{
  float *plane;
  int side;

  if ( node->cellnum == CELLNUM_UNKNOWN )
  {
    Assert(node->planenum != PLANENUM_LEAF, s_assertDisable_EmitPatchSurface);
    plane = MAP_PLANE(node->planenum)->normal;
    side = WindingPlaneSide((Winding_t *)Block, plane, plane[3]);

    if ( side == SIDE_CROSS )
    {
      int *backWinding = (int *)CopyWinding((Winding_t *)Block);
      float negPlane[3];
      VectorNegate(plane, negPlane);
      ClipWindingByPlane((Winding_t **)&backWinding, negPlane, -plane[3], ON_EPSILON);
      ClipWindingByPlane((Winding_t **)&Block, plane, plane[3], ON_EPSILON);
      /* Native 0x43FF10 visits the negative copy first, then continues
         with the positive original.  PrependTriSurf makes this traversal
         order observable in each cell's surface list. */
      EmitPatchSurface(backWinding, material, node->children[1]);
      EmitPatchSurface(Block, material, node->children[0]);
      return;
    }

    if ( side == SIDE_BACK || (side == SIDE_ON && DotProduct210(material->plane, plane) <= 0.0) )
    {
      EmitPatchSurface(Block, material, node->children[1]);
      return;
    }

    EmitPatchSurface(Block, material, node->children[0]);
    return;
  }

  if ( node->cellnum == CELLNUM_OPAQUE )
  {
    /* opaque leaf — discard winding */
    FreeWinding((Winding_t *)Block);
    return;
  }

  /* visible leaf — emit triSurf into this cell */
  Assert(node->cellnum >= 0, s_assertDisable_EmitPatchSurface);
  PrependTriSurf((Winding_t *)Block, 0, 0, material, &triSurfCellArray[node->cellnum]);
}

/*
================
EmitTriangleRecord

Emits a single triangle with full vertex attributes including
texture coords, lightmap coords, normals, and RGBA color
================
*/
int EmitTriangleRecord( TriSurf_t *sourceSurf, Winding_t *origWinding, Winding_t *windingVerts,
  TriSurfProps_t *baseProps, TriSurfProps_t *material, int vertIdx0, int vertIdx1,
  int vertIdx2, int drawOrder, int triFlags, float *triPlane )
{
  float texS[4], texT[4];
  const TrisLmapAssignmentPayload_t *assignment;
  const float *lmapVecs;
  TriSurfPropsSidecar_t *sidecar;
  TriSurfPropsSidecar_t *baseSidecar;
  int lightStyle;
  int i, ch;

  if ( (int)numTriangles >= MAX_MAP_TRIANGLES )
    Com_Error("MAX_MAP_TRIANGLES (%i) exceeded\n", MAX_MAP_TRIANGLES);
  if ( numDrawVertices + 3 > MAX_MAP_VERTEXES )
    Com_Error("MAX_MAP_VERTEXES (%i) exceeded\n", MAX_MAP_VERTEXES);
  sidecar = TrisPropsSidecar_Get(material);
  if (!sidecar)
    Com_Error("EmitTriangleRecord: missing TriSurfProps sidecar");
  /* Native 0x4431E0 passes the owner props independently from the current
     chain layer.  `sourceSurf->props` happens to agree for simple surfaces,
     but is not the carrier when 0x443F50 consumes a coalesce chain. */
  Assert(baseProps, s_assertDisable_TriangulateSurf);
  baseSidecar = TrisPropsSidecar_Get(baseProps);
  if (!baseSidecar)
    Com_Error("EmitTriangleRecord: missing base TriSurfProps sidecar");
  Vector4Copy(&material->texVecs[0], texS);
  Vector4Copy(&material->texVecs[4], texT);
  /* Native 0x443310 receives the owning/base props separately from the
     layer material props.  Lightmap vectors and atlas selection always come
     from that base carrier; texture/color/material data below use `material`. */
  assignment = sourceSurf ? TrisLmap_GetAssignment(sourceSurf, baseProps) : NULL;
  lmapVecs = assignment ? &assignment->vecs[0][0] : &baseProps->lmapVecs[0];
  lightStyle = assignment ? assignment->lightmapIndex : baseSidecar->lightStyle;

  /* snap texture offset to integer for non-global textures */
  if ( !material->si->globalTexture )
  {
    texS[3] = (float)(texS[3] - (double)fistp_sub((float)texS[3], FISTP_HALF_BIAS));
    texT[3] = (float)(texT[3] - (double)fistp_sub((float)texT[3], FISTP_HALF_BIAS));
  }
  /* set positions from original winding (used for UV/color computation) */
  { DrawVert_t *dv = &drawVertBuffer[numDrawVertices];
  int triVerts[3] = { vertIdx0, vertIdx1, vertIdx2 };
  for ( i = 0; i < 3; i++ )
    VectorCopy(origWinding->points[triVerts[i]], dv[i].pos);

  /* compute per-vertex attributes */
  for ( i = 0; i < 3; i++ )
  {
    unsigned char *color = (unsigned char *)&dv[i].color;

    /* texture coordinates — x87 order 1,2,0 for S; 0,1,2 for T */
    dv[i].uv[0] = DotProduct120(dv[i].pos, texS) + texS[3];
    dv[i].uv[1] = DotProduct(dv[i].pos, texT) + texT[3];


    /* lightmap coordinates — x87 order 0,2,1 for both S and T */
    dv[i].lmUv[0] = DotProduct021(dv[i].pos, lmapVecs) + lmapVecs[3];
    dv[i].lmUv[1] = DotProduct021(dv[i].pos, lmapVecs + 4) + lmapVecs[7];

    /* normal plus CoD4's map smoothing carrier. */
    VectorCopy(triPlane, dv[i].normal);
    dv[i].smoothAngle = sidecar->smoothAngle;
    dv[i].smoothing = sidecar->smoothing;

    /* RGBA color: clamp(colorVec . pos, 0, 255) per channel */
    for ( ch = 0; ch < 4; ch++ )
    {
      float *cv = &material->colorVecs[ch * 4];
      float val = DotProduct210(dv[i].pos, cv) + cv[3];
      int rounded = xs_RoundToInt(val + FISTP_BIAS);
      if ( rounded < 0 ) rounded = 0;
      if ( rounded > 255 ) rounded = 255;
      color[ch] = rounded;
    }

    /* clamp lightmap UVs to [0, 1] */
    if ( dv[i].lmUv[0] < 0.0 ) dv[i].lmUv[0] = 0.0;
    else if ( dv[i].lmUv[0] > 1.0 ) dv[i].lmUv[0] = 1.0;
    if ( dv[i].lmUv[1] < 0.0 ) dv[i].lmUv[1] = 0.0;
    else if ( dv[i].lmUv[1] > 1.0 ) dv[i].lmUv[1] = 1.0;
  }

  /* overwrite positions with t-junction fixed winding */
  for ( i = 0; i < 3; i++ )
    VectorCopy(windingVerts->points[triVerts[i]], dv[i].pos);
  } /* end DrawVert_t scope */

  /* emit triangle record */
  { TriRecord_t *tri = &triRecords[numTriangles];
  tri->drawOrder = drawOrder;
  tri->cullGroupIdx = triFlags;
  tri->si = material->si;
  tri->lightStyle = lightStyle;
  tri->reflectionProbeIndex = sidecar->reflectionProbeIndex;
  Assert(sidecar->mapDrawSurf, s_assertDisable_TriangulateSurf);
  tri->primaryLightIndex = (unsigned char)(baseSidecar->mapDrawSurf->lightmapIndex == 256
      ? 0 : baseSidecar->mapDrawSurf->lightmapIndex);
  Assert(!tri->primaryLightIndex || tri->primaryLightIndex < numBspPrimaryLights,
         s_assertDisable_TriangulateSurf);
  tri->castsSunShadow = sidecar->mapDrawSurf->castsSunShadow;
  tri->vertIdx[0] = numDrawVertices;
  tri->vertIdx[1] = numDrawVertices + 1;
  tri->vertIdx[2] = numDrawVertices + 2;
  memcpy(tri->texVecS, texS, sizeof(float) * 4);
  memcpy(tri->texVecT, texT, sizeof(float) * 4);
  memcpy(tri->lmapVecS, lmapVecs, sizeof(float) * 4);
  memcpy(tri->lmapVecT, lmapVecs + 4, sizeof(float) * 4);
  memcpy(tri->colorVecR, &material->colorVecs[0], sizeof(float) * 4);
  memcpy(tri->colorVecG, &material->colorVecs[4], sizeof(float) * 4);
  memcpy(tri->colorVecB, &material->colorVecs[8], sizeof(float) * 4);
  memcpy(tri->colorVecA, &material->colorVecs[12], sizeof(float) * 4);
  }

  ++numTriangles;
  numDrawVertices += 3;
  return numDrawVertices;
}

/*
================
TriangulateSurf

Computes a triangle plane from three vertex indices and emits
the triangle through the material's coalesce chain
================
*/
static int TriangulateSurfImpl(TriSurf_t *sourceSurf, Winding_t *windingData,
  Winding_t *origWinding, TriSurfProps_t *matProps, int vertIdx0, int vertIdx1,
  int vertIdx2, int drawOrder, int triFlags)
{
  int result;
  CoalesceNode_t *chainPtr;
  float triPlane[4];

  Assert(sourceSurf, s_assertDisable_TriangulateSurf);
  Assert(windingData, s_assertDisable_TriangulateSurf);
  Assert(origWinding, s_assertDisable_TriangulateSurf);
  Assert(matProps, s_assertDisable_TriangulateSurf);
  result = PlaneFromPoints(triPlane, windingData->points[vertIdx0], windingData->points[vertIdx1], windingData->points[vertIdx2]);
  if ( result )
  {
    if ( matProps->coalesceChain )
    {
      for ( chainPtr = matProps->coalesceChain; chainPtr; chainPtr = chainPtr->next )
        result = EmitTriangleRecord(sourceSurf, origWinding, windingData, matProps,
          (TriSurfProps_t *)chainPtr->value, vertIdx0, vertIdx1, vertIdx2,
          drawOrder, triFlags, triPlane);
    }
    else
    {
      result = EmitTriangleRecord(sourceSurf, origWinding, windingData, matProps,
        matProps, vertIdx0, vertIdx1, vertIdx2, drawOrder, triFlags, triPlane);
    }
  }
  return result;
}

int TriangulateSurf(TriSurf_t *ts, int vertIdx0, int vertIdx1, int vertIdx2, int drawOrder, int triFlags)
{
  Assert(ts, s_assertDisable_TriangulateSurf);
  Assert(trisTransientMode == 1, s_assertDisable_TriangulateSurf);
  return TriangulateSurfImpl(ts, ts->winding, ts->origWinding, ts->props,
    vertIdx0, vertIdx1, vertIdx2, drawOrder, triFlags);
}

/* Native 0x4431E0 is the seven-argument callback consumed by 0x45B6F0.
   KIWI retains the donor split cell/cull-group fields, so project the native
   combined visibility-group index at this boundary. */
static void Tris_TriangulateNativeCallback(Winding_t *winding, Winding_t *origWinding,
  TriSurfProps_t *props, int vertIdx0, int vertIdx1, int vertIdx2,
  int visGroupIndex, void *userData)
{
  TriSurf_t *sourceSurf = (TriSurf_t *)userData;
  int drawOrder;
  int cullGroupIdx;

  if ( visGroupIndex < numBSPCells )
  {
    drawOrder = visGroupIndex;
    cullGroupIdx = -1;
  }
  else
  {
    drawOrder = -1;
    cullGroupIdx = visGroupIndex - numBSPCells;
  }
  TriangulateSurfImpl(sourceSurf, winding, origWinding, props, vertIdx0,
    vertIdx1, vertIdx2, drawOrder, cullGroupIdx);
}

/*
================
EmitTriSurfForProps

Emits a triangle surface for the given material properties,
handling BSP visibility and cell assignment
================
*/
int EmitTriSurfForProps(Winding_t *winding, Tree_t *bspTree, TriSurfProps_t *material, int cellIndex)
{
  int numVerts, i;
  double sumX, sumY, sumZ, invCount;
  int visResult, result;

  /* surf_buf must be a contiguous struct — TesselateWinding accesses it
     as a single 68-byte TriSurf_t starting from &surfData[0]. */
  struct { intptr_t surfData[4]; float boundsMin[3]; float boundsMax[10]; } surf_buf;

  Assert(material, s_assertDisable_EmitTriSurfForProps);
  Assert(!material->coalesceChain, s_assertDisable_EmitTriSurfForProps);

  /* determine cell visibility */
  if ( g_currentEntityIndex )
  {
    visResult = 0;
  }
  else if ( cellIndex < 0 )
  {
    visResult = EmitTriSurface_r(winding, material, bspTree->headnode);
    if ( visResult == CELLNUM_UNKNOWN )
    {
      /* compute winding centroid for error message */
      numVerts = winding->numpoints;
      sumX = 0.0;
      sumY = 0.0;
      sumZ = 0.0;
      for ( i = 0; i < numVerts; i++ )
      {
        sumX += winding->points[i][0];
        sumY += winding->points[i][1];
        sumZ += winding->points[i][2];
      }
      invCount = 1.0 / (double)numVerts;
      Com_Error(
        "unsplittable surface crosses a cell boundary (ref pos = %g %g %g, material = %s)\n",
        sumX * invCount, sumY * invCount, sumZ * invCount,
        material->si->name);
    }
  }
  else
  {
    visResult = CELLNUM_UNKNOWN;
  }

  /* build temporary triSurf for tesselation */
  Assert(!trisTransientMode, s_assertDisable_EmitTriSurfForProps);
  trisTransientMode = 1;
  surf_buf.surfData[0] = 0;
  surf_buf.surfData[1] = (intptr_t)CopyWinding(winding);
  surf_buf.surfData[2] = 0;
  ((TriSurf_t *)surf_buf.surfData)->origWinding = winding;
  surf_buf.surfData[3] = (intptr_t)material;

  /* compute winding bounds */
  ClearBounds(surf_buf.boundsMin, surf_buf.boundsMax);
  for ( i = 0; i < winding->numpoints; i++ )
    AddPointToBounds(winding->points[i], surf_buf.boundsMin, surf_buf.boundsMax);

  surf_buf.boundsMax[5] = 0.0;
  surf_buf.boundsMax[4] = 0.0;
  memset(&surf_buf.boundsMax[7], 0, 12);

  result = TesselateWinding((TriSurf_t *)surf_buf.surfData, visResult, cellIndex, TriangulateSurf);
  trisTransientMode = 0;
  return result;
}

/*
================
Tris_EmitWindingAsDrawSurf

Emits a winding as a draw surface, handling BSP cell
assignment and material coalesce chains
================
*/
void Tris_EmitWindingAsDrawSurf(
        float *colorVecs,
        float *plane,
        DrawSurf_t *surf,
        Tree_t *bspTree,
        Winding_t *Block,
        float smoothAngle,
        float *texOutput,
        float *lmapOutput)
{
  TriSurfProps_t *emittedSurf;
  int cellIndex;

  /* 0x43FAC0 retains a winding only for tool class 0x10 or whenever the
     derived alpha plane is not the all-zero vector. */
  if ( (surf->shaderInfo->toolFlagsWord & 0x70) != 0x10
    && colorVecs[12] == 0.0f && colorVecs[13] == 0.0f
    && colorVecs[14] == 0.0f && colorVecs[15] == 0.0f )
    return;

  /* inlined EmitTriSurfaceForDrawSurf — original was __usercall (EAX=ds) */
  Assert(surf, s_assertDisable_EmitTriSurfaceForDrawSurf);
  emittedSurf = EmitTriSurface(texOutput, lmapOutput, colorVecs, plane, surf->shaderInfo, surf->lightStyle, surf->contentFlags, smoothAngle);
  TriSurfPropsSidecar_t *sidecar = TrisPropsSidecar_Get(emittedSurf);
  sidecar->mapDrawSurf = NativeMapDrawSurfFor(surf);
  if ( surf->sideRef )
    sidecar->smoothing = surf->sideRef->compileFlags;
  sidecar->reflectionCellMode = (unsigned char)(surf->isPatch || surf->isTerrain);
  sidecar->reflectionProbeIndex = sidecar->mapDrawSurf->reflectionProbeIndex;

  if ( surf->shaderInfo->noClip )
  {
    EmitTriSurfForProps(Block, bspTree, emittedSurf, surf->outputNum);
  }
  else
  {
    cellIndex = surf->outputNum;
    if ( cellIndex < 0 )
    {
      if ( g_currentEntityIndex <= 0 ) {
        EmitPatchSurface((int *)Block, emittedSurf, bspTree->headnode);
      }
      else {
        PrependTriSurf(Block, 0, 0, emittedSurf, triSurfCellArray);
      }
    }
    else
    {
      PrependTriSurf(Block, 0, 0, emittedSurf, &triSurfCellArray[numBSPCells + cellIndex]);
    }
  }
}

/*
================
AuxDataCopy

Copies auxiliary data between buffers with overlap checking
================
*/
int AuxDataCopy(int elemSize, char *srcBuf, int srcIndex, int copyCount, char *dstBuf, int dstIndex)
{
  unsigned int copyBytes;
  char *srcPtr;
  char *dstPtr;
  int result;

  copyBytes = copyCount * elemSize;
  srcPtr = srcBuf + srcIndex * elemSize;
  dstPtr = dstBuf + dstIndex * elemSize;
  Assert(!(&srcPtr[copyCount * elemSize] > dstPtr && &dstPtr[copyBytes] > srcPtr), s_assertDisable_AuxDataCopy);
  result = copyCount * elemSize;
  memcpy(dstPtr, srcPtr, copyBytes);
  return result;
}

/* 0x43F9E0/0x440F40.  Patch control vertices retain only their alpha after
   material tinting.  Tool class 0x60 modulates RGB by alpha and forces the
   output alpha opaque; every other class uses the material tint RGB and the
   source alpha unchanged. */
static int Tris_GetDrawSurfVertexColor(const DrawSurf_t *surf, int sourceColor)
{
  const unsigned char *source = (const unsigned char *)&sourceColor;
  const unsigned char *tint = surf->shaderInfo->colorTint;
  unsigned char result[4];
  int packedColor;

  if ((surf->contentFlags & 0x70) == 0x60)
  {
    result[2] = (unsigned char)((source[3] * tint[2] + 127) / 255);
    result[1] = (unsigned char)((source[3] * tint[1] + 127) / 255);
    result[0] = (unsigned char)((source[3] * tint[0] + 127) / 255);
    result[3] = 255;
  }
  else
  {
    result[0] = tint[0];
    result[1] = tint[1];
    result[2] = tint[2];
    result[3] = source[3];
  }
  memcpy(&packedColor, result, sizeof(packedColor));
  return packedColor;
}

/*
================
Tris_FindPatchWindings

Subdivides a patch surface into triangle windings,
derives texture vectors, and emits them as draw surfaces
================
*/
void Tris_FindPatchWindings(DrawSurf_t *drawSurf, Tree_t *bspTree)
{
  Mesh_t *subdivided, *mesh;
  MeshVert_t *verts;
  Winding_t *w[2];
  int row, col, idx0, idx1, idx2, half;
  float st[6], lmSt[6];
  int colors[3], planeValid[2];
  
  /* DeriveTextureVectors output buffers must be contiguous */
  struct { float out1[16]; float out2[16]; } bspBuf;
  struct { float vecs1[4]; float extra1[4]; float vecs2[4]; float extra2[4]; } lmapBuf;
  struct { float vecs1[4]; float extra1[4]; float vecs2[4]; float extra2[4]; } texBuf;
  struct { float plane1[4]; float plane2[4]; } planeBuf;

  subdivided = SubdivideMesh(drawSurf->patchWidth, drawSurf->patchHeight, colors[0], drawSurf->verts, colors[2], (float)drawSurf->subdivLevel, SUBDIV_NO_MIN_LENGTH, 0, 0);
  PutMeshOnCurve(subdivided->width, subdivided->height, subdivided->reserved, subdivided->verts);
  mesh = RemoveLinearMeshColumnsRows(subdivided, 0, 0);
  FreeMesh(subdivided);

  verts = mesh->verts;

  /* vertex(row, col) = col * width + row (column-major layout) */
  for ( row = 0; row < mesh->width - 1; row++ )
  {
    for ( col = 0; col < mesh->height - 1; col++ )
    {
      /* four corners of this quad */
      idx0 = col * mesh->width + row;           /* v(row,   col)   */
      idx1 = col * mesh->width + row + 1;       /* v(row+1, col)   */
      idx2 = (col + 1) * mesh->width + row;     /* v(row,   col+1) */
      half = (col + 1) * mesh->width + row + 1; /* v(row+1, col+1) — reuse 'half' as idx3 */

      /* fill winding vertex from mesh: copy pos, st, lmSt, color */
      #define FILL_VERT(wnd, slot, vi) do { \
        MeshVert_t *_v = &verts[vi]; \
        VectorCopy(_v->pos, (wnd)->points[slot]); \
        st[slot*2] = _v->st[0];  st[slot*2+1] = _v->st[1]; \
        lmSt[slot*2] = _v->lmSt[0];  lmSt[slot*2+1] = _v->lmSt[1]; \
        colors[slot] = Tris_GetDrawSurfVertexColor(drawSurf, _v->color); \
      } while(0)

      /* triangle 0: v01, v11, v10 */
      w[0] = AllocWinding(4);
      w[0]->numpoints = 3;
      FILL_VERT(w[0], 0, idx2);
      FILL_VERT(w[0], 1, half);
      FILL_VERT(w[0], 2, idx1);
      planeValid[0] = PlaneFromPoints(planeBuf.plane1, w[0]->points[0], w[0]->points[1], w[0]->points[2]);
      if ( planeValid[0] )
      {
        planeValid[0] = DeriveTextureVectors(planeBuf.plane1, w[0]->points[0], st, texBuf.vecs1, lmSt, lmapBuf.vecs1, (unsigned char *)colors, bspBuf.out1);
        if (planeValid[0])
          FloorLightmapVectorOffsets(lmapBuf.vecs1);
        else
          FreeWinding(w[0]);
      }
      else
        FreeWinding(w[0]);

      /* triangle 1: v10, v00, v01 */
      w[1] = AllocWinding(3);
      w[1]->numpoints = 3;
      FILL_VERT(w[1], 0, idx1);
      FILL_VERT(w[1], 1, idx0);
      FILL_VERT(w[1], 2, idx2);
      planeValid[1] = PlaneFromPoints(planeBuf.plane2, w[1]->points[0], w[1]->points[1], w[1]->points[2]);
      if ( planeValid[1] )
      {
        planeValid[1] = DeriveTextureVectors(planeBuf.plane2, w[1]->points[0], st, texBuf.vecs2, lmSt, lmapBuf.vecs2, (unsigned char *)colors, bspBuf.out2);
        if (planeValid[1])
          FloorLightmapVectorOffsets(lmapBuf.vecs2);
        else
          FreeWinding(w[1]);
      }
      else
        FreeWinding(w[1]);

      #undef FILL_VERT

      /* CoD4 0x440C00 compares all generated vector planes, including
         vertex colors, before joining the two patch triangles. */
      if (planeValid[0] && planeValid[1]
        && PatchWindingPairMatches(drawSurf, w, planeBuf.plane1, texBuf.vecs1,
                                   lmapBuf.vecs1, bspBuf.out1))
      {
        VectorCopy(w[1]->points[1], w[0]->points[3]);
        w[0]->numpoints = 4;
        FreeWinding(w[1]);
        planeValid[1] = 0;
      }

      /* emit both triangles (or merged quad) */
      { float *planes[2] = { planeBuf.plane1, planeBuf.plane2 };
      float *bspOuts[2] = { bspBuf.out1, bspBuf.out2 };
      float *texOuts[2] = { texBuf.vecs1, texBuf.vecs2 };
      float *lmOuts[2] = { lmapBuf.vecs1, lmapBuf.vecs2 };
      for ( half = 0; half < 2; half++ )
      {
        if ( planeValid[half] )
        {
          Tris_EmitWindingAsDrawSurf(
            bspOuts[half], planes[half], drawSurf, bspTree,
            w[half], smoothAngle,
            texOuts[half], lmOuts[half]);
        }
      }
      }
    }
  }
  FreeMesh(mesh);
}

/*
================
Tris_FindIndexedMeshWindings

Creates triangle windings from an indexed mesh and emits
them as draw surfaces with derived texture vectors
================
*/
int Tris_FindIndexedMeshWindings( DrawSurf_t *surf, int **windingList )
{
  MeshVert_t *allVerts = surf->verts;
  Winding_t *w[2];
  float texInfo[2][16], texVecs[2][8], lmapVecs[2][8], triPlane[2][4];
  float st[6], lmSt[6];
  int colors[3];
  int planeValid[2];
  int i, j, windingCount;

  for ( i = 0; i < surf->numIndexes; i += 3 )
  {
    /* Native 0x440FD0 recognizes the two-triangle quad encoding emitted by
       MeshToTriangles, then submits one four-point winding in a specific
       rotation.  This happens before the general coalesce/t-junction path. */
    windingCount = 1;
    if ( i + 3 < surf->numIndexes
      && surf->indexes[i] == surf->indexes[i + 4]
      && surf->indexes[i + 1] == surf->indexes[i + 3] )
      windingCount = 2;

    for ( int windingIndex = 0; windingIndex < windingCount; ++windingIndex )
    {
      w[windingIndex] = AllocWinding(4);
      w[windingIndex]->numpoints = 3;
      for ( j = 0; j < 3; ++j )
      {
        MeshVert_t *mv = &allVerts[surf->indexes[i + windingIndex * 3 + j]];
        VectorCopy(mv->pos, w[windingIndex]->points[j]);
        st[j * 2]     = mv->st[0];
        st[j * 2 + 1] = mv->st[1];
        lmSt[j * 2]     = mv->lmSt[0];
        lmSt[j * 2 + 1] = mv->lmSt[1];
        colors[j] = Tris_GetDrawSurfVertexColor(surf, mv->color);
      }

      planeValid[windingIndex] = PlaneFromPoints(
          triPlane[windingIndex], w[windingIndex]->points[0],
          w[windingIndex]->points[1], w[windingIndex]->points[2]);
      if ( planeValid[windingIndex] )
      {
        planeValid[windingIndex] = DeriveTextureVectors(triPlane[windingIndex], w[windingIndex]->points[0],
            st, texVecs[windingIndex], lmSt, lmapVecs[windingIndex],
            (unsigned char *)colors, texInfo[windingIndex]);
        if (planeValid[windingIndex])
          FloorLightmapVectorOffsets(lmapVecs[windingIndex]);
        else
          FreeWinding(w[windingIndex]);
      }
      else
      {
        FreeWinding(w[windingIndex]);
      }
    }

    if ( windingCount == 2 )
    {
      if ( planeValid[0] && planeValid[1]
        && TriangleContainsCoplanarPoint(w[1]->points[2], w[0], triPlane[0])
        && TerrainQuadVertexMatches(surf->shaderInfo,
                                    &allVerts[surf->indexes[i + 5]],
                                    texVecs[0], lmapVecs[0], texInfo[0]) )
      {
        /* [a,b,c] + [b,a,d] becomes [a,d,b,c] (0x44127c..0x4412bc). */
        VectorCopy(w[0]->points[2], w[0]->points[3]);
        VectorCopy(w[0]->points[1], w[0]->points[2]);
        VectorCopy(w[1]->points[2], w[0]->points[1]);
        w[0]->numpoints = 4;
        FreeWinding(w[1]);
        planeValid[1] = 0;
      }
    }

    for ( j = 0; j < windingCount; ++j )
    {
      if ( planeValid[j] )
      {
        Tris_EmitWindingAsDrawSurf(texInfo[j], triPlane[j], surf,
            (Tree_t *)windingList, w[j], smoothAngle, texVecs[j], lmapVecs[j]);
      }
    }

    if ( windingCount == 2 )
      i += 3;
  }

  return surf->numIndexes;
}

static DrawSurf_t *g_findBrushWindingsCount;

/*
================
Tris_FindBrushWindings

Creates a winding from brush vertices, derives texture
and lightmap vectors, and emits it as a draw surface
================
*/
void Tris_FindBrushWindings(int a1_unused, Tree_t *bspTree)
{
  DrawSurf_t *surf = g_findBrushWindingsCount;
  BrushSide_t *side = surf->sideRef;
  MeshVert_t *firstVert = surf->verts;
  Winding_t *w;
  float plane[4], texVecs[8], lmapSt[6], colorVecs[16];
  int lmapVecs[8];
  float uvDiff;
  int i;

  /* build winding from brush vertex positions */
  w = AllocWinding(surf->numVerts);
  w->numpoints = surf->numVerts;
  for ( i = 0; i < surf->numVerts; i++ )
    VectorCopy(surf->verts[i].pos, w->points[i]);

  /* copy plane from brush side */
  VectorCopy(MAP_PLANE(side->planenum)->normal, plane);
  plane[3] = mapplanes[side->planenum].dist;

  /* copy texture vectors, snap UV offset to nearest integer */
  Vector4Copy(side->texVecs[0], texVecs);
  Vector4Copy(side->texVecs[1], &texVecs[4]);


  uvDiff = DotProduct210(firstVert->pos, texVecs) + texVecs[3] - firstVert->st[0];
  texVecs[3] -= RoundFloatToInt(uvDiff);
  uvDiff = DotProduct210(firstVert->pos, &texVecs[4]) + texVecs[7] - firstVert->st[1];
  texVecs[7] -= RoundFloatToInt(uvDiff);

  /* Native 0x441713 uses MaterialInfo::gameFlags bit 1 to select the
     lightmap-vector derivation path.  lightStyle is assigned later by the
     lightmap allocator, so it cannot make this pre-allocation decision. */
  if ( surf->shaderInfo->gameFlags & 2 )
  {
    PickBestTriangle(plane, (float *)surf->verts, surf->numVerts, plane[3], (float *)&colorVecs[7], (float *)&lmapVecs[2], lmapSt);
    if (!DeriveTextureVectors(plane, (float *)&colorVecs[7], 0, 0, lmapSt, (float *)lmapVecs, 0, 0))
    {
      FreeWinding(w);
      return;
    }
    FloorLightmapVectorOffsets((float *)lmapVecs);
  }
  else
  {
    memset(lmapVecs, 0, sizeof(lmapVecs));
  }

  /* 0x4417DD loads the material tint into the four constant color planes. */
  memset(colorVecs, 0, sizeof(colorVecs));
  colorVecs[3] = (float)surf->shaderInfo->colorTint[0];
  colorVecs[7] = (float)surf->shaderInfo->colorTint[1];
  colorVecs[11] = (float)surf->shaderInfo->colorTint[2];
  colorVecs[15] = (float)surf->shaderInfo->colorTint[3];

  Tris_EmitWindingAsDrawSurf(colorVecs, plane, surf, bspTree, w, smoothAngle, texVecs, (float *)lmapVecs);
}

/*
================
Tris_FindWindings

Iterates draw surfaces for an entity and dispatches each
to the appropriate winding finder (patch, mesh, or brush)
================
*/
int Tris_FindWindings( Entity_t *entityData, Tree_t *bspTree )
{
  int cellCount, i;

  if ( g_currentEntityIndex <= 0 )
    cellCount = numBSPCells + numBSPCullGroups;
  else
    cellCount = 1;

  memset(triSurfCellArray, 0, sizeof(TriSurf_t *) * cellCount);
  numTriangles = 0;
  numDrawVertices = 0;

  /* dispatch each draw surface to the appropriate winding finder */
  for ( i = entityData->firstDrawSurf; i < numMapDrawSurfs; i++ )
  {
    DrawSurf_t *surf = &g_drawSurfs[i];
    if ( !surf->numVerts )
      continue;

    Assert(surf->shaderInfo, s_assertDisable_Tris_FindWindings);

    if ( surf->isPatch )
    {
      Tris_FindPatchWindings(surf, bspTree);
    }
    else if ( surf->isTerrain )
    {
      Tris_FindIndexedMeshWindings(surf, (int **)bspTree);
    }
    else
    {
      g_findBrushWindingsCount = surf;
      Tris_FindBrushWindings(0, bspTree);
    }
  }

  triFirstVertIndex = numDrawVertices;
  return numDrawVertices;
}

/*
================
Tris_TriangulateWindings

Triangulates all windings across all cells by iterating
each cell's surface list and calling TesselateWinding
================
*/
/* CoD4 0x43E8B0.  The native pipeline performs this after snapping and
   before 0x443110 tessellates.  Keep it separate from tessellation so every
   surviving surface reaches the latter with a valid, preprocessed winding. */
static int RemoveDegenerateTriSurfs(void)
{
  int totalCells;
  int cell;
  TriSurf_t **link;
  TriSurf_t *surf;

  if ( g_currentEntityIndex <= 0 )
    totalCells = numBSPCells + numBSPCullGroups;
  else
    totalCells = 1;

  for ( cell = 0; cell < totalCells; ++cell )
  {
    for ( link = &triSurfCellArray[cell]; *link; )
    {
      surf = *link;
      if ( PreprocessLmapWinding(surf) < 3 )
      {
        /* FreeTriSurf repairs either the cell head or prev->next.  link
           intentionally stays put so it observes the replacement surface. */
        FreeTriSurf(surf);
      }
      else
      {
        link = &surf->next;
      }
    }
  }

  return totalCells;
}

int Tris_TriangulateWindings(void)
{
  int totalCells, cell;
  TriSurf_t *ts, *next;

  if ( g_currentEntityIndex <= 0 )
    totalCells = numBSPCells + numBSPCullGroups;
  else
    totalCells = 1;

  for ( cell = 0; cell < totalCells; cell++ )
  {
    /* CoD4 0x443110 passes the combined visibility-group index to the
       non-mutating 0x45B6F0 tessellator. */
    for ( ts = triSurfCellArray[cell]; ts; ts = next )
    {
      if ( !TrisNative_TesselateWinding(ts, cell, Tris_TriangulateNativeCallback, ts) )
      {
        TesselateWindingFailure(ts);
      }
      next = ts->next;
      FreeTriSurf(ts);
    }
    triSurfCellArray[cell] = NULL;
  }

  return totalCells;
}

/*
================
Tris_SortWindingsComparatorPass

Repeatedly merges winding pairs until no more merges occur
================
*/
int Tris_SortWindingsComparatorPass( TriSurf_t **surfArray, IntWinding_t **windingArray, int count, int auxElemSize, TriSurf_t *baseSurf, IntWinding_t *baseWinding, int flags )
{
  int prev, i;

  /* repeat merge passes until no more merges occur */
  do
  {
    prev = count;
    for ( i = 0; i < count; i++ )
      count = TryMergeWindingPair(i, (intptr_t *)surfArray, windingArray, count, auxElemSize);
  }
  while ( count != prev );

  return count;
}

/*
================
ClipTriWinding

Clips a triangle winding by a plane into front and back
halves, interpolating vertices at the split point
================
*/
static float ClipTriWindingReferenceMaxDist(const Winding_t *winding, const float *normal, float dist)
{
  float maxDist = 0.0f;
  int i;

  for (i = 0; i < winding->numpoints; ++i)
  {
    float pointDist = DotProduct(winding->points[i], normal) - dist;
    if (pointDist < 0.0f)
      pointDist = -pointDist;
    if (pointDist > maxDist)
      maxDist = pointDist;
  }
  return maxDist;
}

WindingAuxPair_t *ClipTriWindingWithAux( WindingAuxPair_t *windingPair, Winding_t *reference, int auxDataSize, float *normal, float dist, float epsilon, WindingAuxPair_t *frontOut, WindingAuxPair_t *backOut )
{
  Winding_t *w;
  char *aux;
  int numPts;
  int maxPts;
  double dots[MAX_POINTS_ON_WINDING + 1];
  int sides[MAX_POINTS_ON_WINDING + 1], counts[3] = {0,0,0};
  int axialAxis = -1, isAxial = 1, i, k;
  float axialDist = 0.0f;
  float minDist, maxDist, referenceMaxDist;
  float smallEpsilon;
  Winding_t *f, *b;
  void *fAux = NULL, *bAux = NULL;

  Assert(windingPair, s_assertDisable_ClipTriWinding);
  w = windingPair->winding;
  aux = (char *)windingPair->auxData;
  Assert(w, s_assertDisable_ClipTriWinding);
  numPts = w->numpoints;
  maxPts = numPts + 4;
  Assert(normal, s_assertDisable_ClipTriWinding);
  Assert(DotProduct(normal, normal) > 0.0f, s_assertDisable_ClipTriWinding);
  Assert(frontOut, s_assertDisable_ClipTriWinding);
  Assert(backOut, s_assertDisable_ClipTriWinding);
  Assert(!auxDataSize || aux, s_assertDisable_ClipTriWinding);

  frontOut->winding = frontOut->auxData = NULL;
  backOut->winding = backOut->auxData = NULL;

  /* Native 0x43C889-0x43CA76: reference-winding-dependent tolerance
     reduction prevents thin fragments from being split inconsistently. */
  smallEpsilon = epsilon * 0.01f;
  WindingPlaneDistExtent(w, normal, dist, &minDist, &maxDist);
  if (smallEpsilon >= maxDist)
  {
    CopyVerts(windingPair, auxDataSize, backOut);
    return backOut;
  }
  if (minDist >= -smallEpsilon)
  {
    CopyVerts(windingPair, auxDataSize, frontOut);
    return frontOut;
  }

  Assert(reference, s_assertDisable_ClipTriWinding);
  if (epsilon > maxDist && minDist < -3.0f * maxDist)
  {
    referenceMaxDist = ClipTriWindingReferenceMaxDist(reference, normal, dist);
    if (maxDist <= 0.25f * referenceMaxDist)
    {
      CopyVerts(windingPair, auxDataSize, backOut);
      return backOut;
    }
    if (epsilon > 0.25f * referenceMaxDist)
      epsilon = 0.25f * referenceMaxDist;
  }
  if (minDist > -epsilon && maxDist > -3.0f * minDist)
  {
    referenceMaxDist = ClipTriWindingReferenceMaxDist(reference, normal, dist);
    if (minDist >= -0.25f * referenceMaxDist)
    {
      CopyVerts(windingPair, auxDataSize, frontOut);
      return frontOut;
    }
    if (epsilon > 0.25f * referenceMaxDist)
      epsilon = 0.25f * referenceMaxDist;
  }
  if (epsilon > (maxDist - minDist) * 0.125f)
    epsilon = (maxDist - minDist) * 0.125f;

  /* classify vertices */
  for ( i = 0; i < numPts; i++ )
  {
    dots[i] = DotProduct021(w->points[i], normal) - dist;
    sides[i] = dots[i] > epsilon ? SIDE_FRONT : dots[i] < -epsilon ? SIDE_BACK : SIDE_ON;
    counts[sides[i]]++;
  }
  dots[numPts] = dots[0];
  sides[numPts] = sides[0];

  /* early out — all on one side */
  Assert(counts[SIDE_FRONT], s_assertDisable_ClipTriWinding);
  Assert(counts[SIDE_BACK], s_assertDisable_ClipTriWinding);

  /* detect axial plane for exact snap */
  for ( i = 0; i < 3; i++ )
  {
    if ( normal[i] == 1.0f )       { axialAxis = i; axialDist = dist; }
    else if ( normal[i] == -1.0f ) { axialAxis = i; axialDist = -dist; }
    else if ( normal[i] != 0.0f )  { isAxial = 0; break; }
  }
  Assert(!isAxial || (axialAxis >= 0 && axialAxis < 3), s_assertDisable_ClipTriWinding);

  /* allocate output */
  f = AllocWinding(maxPts);
  b = AllocWinding(maxPts);
  if ( auxDataSize ) { fAux = malloc(auxDataSize * maxPts); bAux = malloc(auxDataSize * maxPts); }

  #define EMIT(wnd, auxBuf, pt, srcIdx) do { \
    VectorCopy(pt, (wnd)->points[(wnd)->numpoints]); \
    if (auxDataSize) AuxDataCopy(auxDataSize, aux, srcIdx, 1, (char *)(auxBuf), (wnd)->numpoints); \
    (wnd)->numpoints++; \
  } while(0)

  /* split loop */
  for ( i = 0; i < numPts; i++ )
  {
    float *p = w->points[i];

    /* copy vertex to its side (ON goes to both) */
    if ( sides[i] != SIDE_BACK )  EMIT(f, fAux, p, i);
    if ( sides[i] != SIDE_FRONT ) EMIT(b, bAux, p, i);

    /* interpolate at plane crossing */
    if ( sides[i] != SIDE_ON && sides[i + 1] != SIDE_ON && sides[i + 1] != sides[i] )
    {
      float *next = w->points[(i + 1) % numPts];
      double dd = dots[i] - dots[i + 1];
      float split[3];

      for ( k = 0; k < 3; k++ )
        split[k] = (float)((next[k] * dots[i] - p[k] * dots[i + 1]) / dd);
      if ( isAxial )
        split[axialAxis] = axialDist;

      VectorCopy(split, f->points[f->numpoints]);
      VectorCopy(split, b->points[b->numpoints]);
      if ( auxDataSize )
      {
        int nextIdx = (i + 1) % numPts;
        double frac = dots[i] / dd;
        g_lerpAuxDataCallback(
            (float *)((char *)aux + i * auxDataSize),
            (float *)((char *)aux + nextIdx * auxDataSize),
            frac,
            (float *)((char *)fAux + f->numpoints * auxDataSize));
        g_lerpAuxDataCallback(
            (float *)((char *)aux + i * auxDataSize),
            (float *)((char *)aux + nextIdx * auxDataSize),
            frac,
            (float *)((char *)bAux + b->numpoints * auxDataSize));
      }
      f->numpoints++;
      b->numpoints++;
    }
  }

  #undef EMIT

  Assert(f->numpoints <= maxPts, s_assertDisable_ClipTriWinding);
  Assert(b->numpoints <= maxPts, s_assertDisable_ClipTriWinding);
  Assert(f->numpoints <= MAX_POINTS_ON_WINDING, s_assertDisable_ClipTriWinding);
  Assert(b->numpoints <= MAX_POINTS_ON_WINDING, s_assertDisable_ClipTriWinding);

  frontOut->winding = f;  frontOut->auxData = fAux;
  backOut->winding = b;   backOut->auxData = bAux;
  return backOut;
}

/* Native 0x43C790 ABI.  CoD4 clips only geometry here; KIWI's legacy
   auxiliary payload handling remains explicitly isolated in
   ClipTriWindingWithAux above. */
Winding_t **ClipTriWinding(Winding_t *in, Winding_t *reference, float *planeNormal, float planeDist, float epsilon,
                           Winding_t **front, Winding_t **back)
{
  WindingAuxPair_t input;
  WindingAuxPair_t frontOut;
  WindingAuxPair_t backOut;

  input.winding = in;
  input.auxData = NULL;
  ClipTriWindingWithAux(&input, reference, 0, planeNormal, planeDist, epsilon, &frontOut, &backOut);
  *front = frontOut.winding;
  *back = backOut.winding;
  return back;
}

/*
================
Tris_FixTJunctions

Initializes merge surfaces and processes all visibility
cells to fix T-junctions in triangle windings
================
*/
void Tris_FixTJunctions( Tree_t *bspTree )
{
  int cellCount, i;

  #define MAX_BSP_MERGE_SURFS 0x40000
  MergeSurfaces_Init(MAX_BSP_MERGE_SURFS, TriSurfCellGroupableCallback, (MergeCallback_t)1);
  SetTrisTransientMode(2, 0);

  if ( g_currentEntityIndex <= 0 )
    cellCount = numBSPCullGroups + numBSPCells;
  else
    cellCount = 1;

  /* merge and fix T-junctions for each visibility cell */
  for ( i = 0; i < cellCount; i++ )
  {
    MergeVisGroupList(&triSurfCellArray[i], cellBoundsData[i].mins, cellBoundsData[i].maxs, bspTree, Tris_SortWindingsComparatorPass);
  }

  SetTrisTransientMode(0, 1);
  MergeSurfaces_Shutdown();
}

/* 0x4420E0.  The large-winding splitter deliberately measures texture, not
   lightmap, coordinates.  Coalesced layers are handled by the recursive
   selector below, exactly as native does. */
static void Tris_GetTextureBounds(const Winding_t *winding, const TriSurfProps_t *props,
                                  float *mins, float *maxs)
{
  int pointIndex;

  Assert(!props->coalesceChain, s_assertDisable_Tris_FindWindings);
  ClearBounds2D(mins, maxs);
  for (pointIndex = 0; pointIndex < winding->numpoints; ++pointIndex)
  {
    float st[2];

    st[0] = DotProduct(winding->points[pointIndex], props->texVecs) + props->texVecs[3];
    st[1] = DotProduct(winding->points[pointIndex], props->texVecs + 4) + props->texVecs[7];
    AddPointToBounds2D(st, mins, maxs);
  }
}

/* 0x442060. */
static void Tris_GetTextureSpan(const Winding_t *winding, const TriSurfProps_t *props,
                                int *span)
{
  float mins[2];
  float maxs[2];

  Tris_GetTextureBounds(winding, props, mins, maxs);
  span[0] = (int)ceil(maxs[0]) - (int)floor(mins[0]);
  span[1] = (int)ceil(maxs[1]) - (int)floor(mins[1]);
}

/* 0x4421D0. */
static int Tris_GetTextureAxisLimit(void)
{
  return 0x2000;
}

/* 0x441DB0.  Native 0x4421D0 returns 0x2000 for each texture axis; split
   when its inclusive integer span exceeds twice that limit. */
static int Tris_SelectTriSurfSplitPlane(const Winding_t *winding, const TriSurfProps_t *props,
                                        float *plane)
{
  const CoalesceNode_t *node;
  int textureSpan[2];
  int excessU;
  int excessV;
  int textureAxis;
  float mins[3];
  float maxs[3];
  float weightedExtent[3];
  int worldAxis;
  float midpoint;
  int worldSpan;
  unsigned int highestBit;
  int gridStep;
  float closestDistance;
  int pointIndex;

  if (props->coalesceChain)
  {
    for (node = props->coalesceChain; node; node = node->next)
    {
      if (Tris_SelectTriSurfSplitPlane(winding, (const TriSurfProps_t *)node->value, plane))
        return 1;
    }
    return 0;
  }

  Tris_GetTextureSpan(winding, props, textureSpan);
  excessU = textureSpan[0] - 2 * Tris_GetTextureAxisLimit();
  excessV = textureSpan[1] - 2 * Tris_GetTextureAxisLimit();
  if (excessU <= 0 && excessV <= 0)
    return 0;

  /* 0x441E6A: ties select the first texture axis. */
  textureAxis = excessU < excessV;
  VectorCopy(winding->points[0], mins);
  VectorCopy(winding->points[0], maxs);
  for (pointIndex = 1; pointIndex < winding->numpoints; ++pointIndex)
    AddPointToBounds(winding->points[pointIndex], mins, maxs);

  weightedExtent[0] = (maxs[0] - mins[0]) * props->texVecs[textureAxis * 4 + 0];
  weightedExtent[1] = (maxs[1] - mins[1]) * props->texVecs[textureAxis * 4 + 1];
  weightedExtent[2] = (maxs[2] - mins[2]) * props->texVecs[textureAxis * 4 + 2];
  worldAxis = VecLargestAxis(weightedExtent);
  midpoint = (mins[worldAxis] + maxs[worldAxis]) * 0.5f;
  worldSpan = (int)ceil(maxs[worldAxis]) - (int)floor(mins[worldAxis]);

  /* 0x44C040 is 31 - bsr(value); 0x441F8B therefore selects the
     half-highest power-of-two grid step. */
  Assert(worldSpan > 1, s_assertDisable_Tris_FindWindings);
  highestBit = 0;
  for (unsigned int value = (unsigned int)worldSpan; value >>= 1; ++highestBit)
  {
  }
  gridStep = 1 << (highestBit - 1);

  plane[0] = 0.0f;
  plane[1] = 0.0f;
  plane[2] = 0.0f;
  plane[3] = (float)(~(gridStep - 1) & (gridStep / 2 + (int)midpoint));
  closestDistance = (float)worldSpan * 0.25f;
  for (pointIndex = 0; pointIndex < winding->numpoints; ++pointIndex)
  {
    float distance = fabsf(winding->points[pointIndex][worldAxis] - midpoint);

    if (closestDistance > distance)
    {
      closestDistance = distance;
      plane[3] = winding->points[pointIndex][worldAxis];
    }
  }
  plane[worldAxis] = 1.0f;
  return 1;
}

/* 0x441CA0. */
static void Tris_SplitTriSurfs_r(TriSurf_t *surf, TriSurf_t **listHead)
{
  Winding_t *front;
  Winding_t *back;
  float plane[4];
  TriSurfProps_t *props;

  props = surf->props;
  if (!Tris_SelectTriSurfSplitPlane(surf->winding, props, plane))
    return;

  ClipWindingEpsilon(surf->winding, plane, plane[3], 0.1f, &front, &back, 0);
  Assert(front, s_assertDisable_Tris_FindWindings);
  Assert(back, s_assertDisable_Tris_FindWindings);
  UnlinkAndFreeSurf(surf, listHead);
  FreeTriSurf(surf);
  Tris_SplitTriSurfs_r(PrependTriSurf(front, NULL, 0, props, listHead), listHead);
  Tris_SplitTriSurfs_r(PrependTriSurf(back, NULL, 0, props, listHead), listHead);
}

/* 0x441C30.  Cache next before recursion because splitting replaces the
   current list node with two newly prepended nodes. */
static void Tris_SplitLargeTriSurfs(void)
{
  int cellIndex;
  int cellCount = g_currentEntityIndex <= 0 ? numBSPCullGroups + numBSPCells : 1;

  for (cellIndex = 0; cellIndex < cellCount; ++cellIndex)
  {
    TriSurf_t *surf;

    for (surf = triSurfCellArray[cellIndex]; surf; )
    {
      TriSurf_t *next = surf->next;
      Tris_SplitTriSurfs_r(surf, &triSurfCellArray[cellIndex]);
      surf = next;
    }
  }
}

/* Native 0x4424D0.  The terrain exposure classifier traces against the
   post-coalesce grid, so populate and repartition that same carrier before
   assigning the MapDrawSurf +52 categories. */
static int Tris_PreprocessSunShadowPipeline(void)
{
  float worldMins[3];
  float worldMaxs[3];
  int cellCount;
  int cellIndex;

  cellCount = numBSPCullGroups + numBSPCells;
  if (!(numBspPrimaryLights > 1 && bspPrimaryLights[1].type == 1))
  {
    for (cellIndex = 0; cellIndex < cellCount; ++cellIndex)
    {
      TriSurf_t *surf;

      for (surf = triSurfCellArray[cellIndex]; surf; surf = surf->next)
        ConfirmCoalescedSunShadowCategory(surf);
    }
    return cellCount;
  }

  ClearBounds(worldMins, worldMaxs);
  for (cellIndex = 0; cellIndex < cellCount; ++cellIndex)
    AddBoundsToBounds(cellBoundsData[cellIndex].mins, cellBoundsData[cellIndex].maxs,
                      worldMins, worldMaxs);
  SetGridDivisionPoints(worldMins, worldMaxs);

  for (cellIndex = 0; cellIndex < cellCount; ++cellIndex)
  {
    TriSurf_t *surf;

    for (surf = triSurfCellArray[cellIndex]; surf; surf = surf->next)
    {
      if (!surf->props->surfFlagBit7)
        GridTree_Insert(surf);
    }
  }
  GridTree_RebuildPartitionBounds();
  return PreprocessSunShadowTriCategories();
}

/*
================
TriangulateEntity

Orchestrates the full triangle processing pipeline for an
entity: finds windings, coalesces, merges, fixes T-junctions,
snaps vertices, and triangulates
================
*/
void TriangulateEntity( Entity_t *entityData, Tree_t *bspTree )
{
  int *mergeMap;

  GridTree_Init();

  /* step 1: find windings */
  if ( !g_currentEntityIndex )
  {
    TrisTimerCheck(NULL);
    printf("%s...\n", "\nfinding triangle windings");
    g_trisTimerStart = I_FloatTime();
  }
  Tris_FindWindings(entityData, bspTree);
  TrisComputeWindingBounds();

  /* step 2: coalesce */
  if ( !g_currentEntityIndex )
  {
    TrisTimerCheck(NULL);
    printf("%s...\n", "coalescing coincident windings");
    g_trisTimerStart = I_FloatTime();
  }
  CoalesceTriSurfCells();

  /* Native removes fully occluded fragments immediately after the
     coincident-winding coalesce pass, before any downstream topology work. */
  RemoveInvalidTriSurfs();
  ForEachSurf(ValidateTriSurfLmapCoordsNative, NULL);

  /* Native 0x43EAD3..0x43EAE0: coalescing/removal can replace the winding
     bounds, so recompute them before the one-unit primary-light grid walk. */
  if ( !g_currentEntityIndex )
  {
    TrisTimerCheck(NULL);
    printf("%s...\n", "assigning primary lights");
    g_trisTimerStart = I_FloatTime();
  }
  TrisComputeWindingBounds();
  AssignPrimaryLightsToTriSurfCells();

  if (g_currentEntityIndex)
  {
    DisableSunShadowTrisForEntityRange();
  }
  else
  {
    TrisTimerCheck(NULL);
    printf("%s...\n", "finding sun shadow casters");
    g_trisTimerStart = I_FloatTime();
    Tris_PreprocessSunShadowPipeline();
  }

  /* Native 0x43EB0C..0x43EB14.  This must precede concave merging so no
     oversized texture-projected winding can reach lightmap grouping. */
  if ( !g_currentEntityIndex )
  {
    TrisTimerCheck(NULL);
    printf("%s...\n", "splitting large windings");
    g_trisTimerStart = I_FloatTime();
  }
  Tris_SplitLargeTriSurfs();

  /* step 3: merge into concave windings */
  if ( !g_currentEntityIndex )
  {
    TrisTimerCheck(NULL);
    printf("%s...\n", "merging into concave windings");
    g_trisTimerStart = I_FloatTime();
  }
  Tris_FixTJunctions(bspTree);

  /* step 4: fix T-junctions */
  if ( !g_currentEntityIndex )
  {
    TrisTimerCheck(NULL);
    printf("%s...\n", "fixing t-junctions");
    g_trisTimerStart = I_FloatTime();
  }
  TrisSetupTJunctionKDTree();

  /* step 5: tether holes */
  if ( !g_currentEntityIndex )
  {
    TrisTimerCheck(NULL);
    printf("%s...\n", "tethering holes to their concave windings");
    g_trisTimerStart = I_FloatTime();
  }
  ForEachSurf((void (*)(TriSurf_t *, bool))MergeHoles, (TriSurf_t *)g_largeBuf);
  /* Native 0x43EB6D..0x43EB95: create mode-4 lmap carriers after all
     topology operations, assign them, then return to a preserved neutral
     state before the existing mode-1 snap/triangulate consumers.  This is
     intentionally runtime-off until the complete native grouping pass is
     reconciled on real maps. */
  if (TrisLmap_NativeRouteEnabled())
  {
    SetTrisTransientMode(4, 0);
    TrisLmap_BuildAndAssign();
    SetTrisTransientMode(0, 1);
  }
  /* step 6: snap vertices */
  if ( !g_currentEntityIndex )
  {
    TrisTimerCheck(NULL);
    printf("%s...\n", "finding index mapping and snapping vertices");
    g_trisTimerStart = I_FloatTime();
  }
  Assert(!trisTransientMode, s_assertDisable_SetTrisTransientMode);
  trisTransientMode = 1;
  TrisGenerateAllVertices();
  mergeMap = BuildMergeMap((char *)drawVertBuffer, 3, sizeof(DrawVert_t), numDrawVertices, EDGE_LENGTH);
  g_trisSnapCount = mergeMap;
  TrisSnapNearbyVertices(0);
  ApplyMergeMap((char *)drawVertBuffer, sizeof(DrawVert_t), numDrawVertices, triFirstVertIndex, mergeMap);
  TrisSnapWindingToVertices(mergeMap);
  FreeSurface(mergeMap);
  RemoveDegenerateTriSurfs();

  /* step 7: triangulate */
  if ( !g_currentEntityIndex )
  {
    TrisTimerCheck(NULL);
    printf("%s...\n", "triangulating all windings");
    g_trisTimerStart = I_FloatTime();
  }
  Tris_TriangulateWindings();
  if ( !g_currentEntityIndex )
    printf("%i self-tjunctions fixed\n%i degenerate tris removed\n", numSelfTjunctions, numDegenerateTrisRemoved);

  trisTransientMode = 0;
  TrisTimerCheck(NULL);
  GridTree_Shutdown();
}

/*
================
EmitDrawSurfaces

Emits all draw surfaces for an entity by triangulating,
smoothing normals, and emitting triangle data to BSP
================
*/
int *EmitDrawSurfaces(Entity_t *entity, Tree_t *tree)
{
  int result;
  int *mergeMap;
  int *freeResult;

  result = entity->firstDrawSurf;
  if ( result == numMapDrawSurfs )
  {
    entity->firstTriSoup = numBSPTriSoups;
    entity->firstTriSoupSimple = numBSPUnlayeredTriSoups;
  }
  else
  {
    /* Native 0x442F90 emits type 1 first.  It is an independently executed
       context, not a byte-for-byte copy of the layered output. */
    entity->firstTriSoupSimple = numBSPUnlayeredTriSoups;
    entity->firstTriSoup = numBSPTriSoups;
    if ( TrisLmap_NativeRouteEnabled() )
      TrisLmap_SaveAllocatorCheckpoint();
    TJunc_SetBounds(tree->mins, tree->maxs);
    TriangulateEntity(entity, tree);
    if ( !g_currentEntityIndex )
      printf("smoothing normals...\n");
    mergeMap = BuildMergeMap((char *)drawVertBuffer, 3, sizeof(DrawVert_t), numDrawVertices, EDGE_LENGTH);
    SmoothVertexNormals(mergeMap);
    if ( !g_currentEntityIndex )
      printf("emitting triangles...\n");

    Tris_SelectDrawSurfaceContext(1);
    Tris_EmitTriangles(0.0, tree, mergeMap);
    if ( entity->firstTriSoupSimple == numBSPTriSoups )
      entity->firstTriSoupSimple = 0;
    FreeSurface(mergeMap);
    FreeAllTriSurfs();

    /* Type 0 is the layered path.  Native additionally combines layered
       materials here; that producer has no reconstructed counterpart yet,
       so this context rebuilds and consumes the shared source geometry
       independently.  EmitTriangleSoup mutates its TriRecord/vertex input,
       which is why reusing the first pass's records is not valid. */
    /* Native 0x43E7C0 emits both types from one prior 0x43EA70 pass.  This
       donor reconstruction must rebuild for type 0, so replay the exact
       pre-pass allocator state; the second native-route assignment then
       occupies the same blocks while leaving the allocator at its native
       post-pass state. */
    if ( TrisLmap_NativeRouteEnabled() )
      TrisLmap_RestoreAllocatorCheckpoint();
    TJunc_SetBounds(tree->mins, tree->maxs);
    TriangulateEntity(entity, tree);
    if ( !g_currentEntityIndex )
      printf("smoothing normals...\n");
    mergeMap = BuildMergeMap((char *)drawVertBuffer, 3, sizeof(DrawVert_t), numDrawVertices, EDGE_LENGTH);
    SmoothVertexNormals(mergeMap);
    if ( !g_currentEntityIndex )
      printf("emitting triangles...\n");
    Tris_SelectDrawSurfaceContext(0);
    Tris_EmitTriangles(0.0, tree, mergeMap);
    if ( !g_currentEntityIndex )
    {
      printf("%i vertices couldn't be merged because the textures point different ways\n", numUnmergeableTexVerts);
      LeafNode(entity, tree->headnode);
    }
    if ( entity->firstTriSoup == numBSPTriSoups )
      entity->firstTriSoup = 0;
    FreeSurface(mergeMap);
    freeResult = FreeAllTriSurfs();
    if ( TrisLmap_NativeRouteEnabled() )
      TrisLmap_ClearAllocatorCheckpoint();
    return freeResult;
  }
  return (int *)(intptr_t)result;
}

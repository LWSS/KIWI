/*
tris_tjunc.c — T-junction fixing

Reconstructed from cod2map.exe by Rose.

Detects and fixes T-junctions in triangle winding edges. When a vertex
from one triangle lies on another triangle's edge, the edge must be split
to prevent rendering cracks. Uses spatial hashing for edge line lookup
and a sorted point list per edge line for insertion order.
*/

#include "cod4map.h"
#include <stddef.h>

#define MAX_CONCAVE_WINDING_POINTS 0x4000

Dvar_t          *fs_copyfiles;
TjuncEdgeLine_t *tjuncNodeHash[TJUNC_HASH_TABLE_SIZE];
TjuncEdgeLine_t  tjuncEdgeLines[MAX_TJUNC_EDGE_LINES];

int    numDegenerateTrisRemoved;
int    numSelfTjunctions;
int    numUnmergeableTexVerts;
void  *tjuncAuxBuffer;
int    tjuncAuxElemSize;
float  tjuncBoundsMax[3];
float  tjuncBoundsMin[3];
char   tjuncCreateNonAxial;
float  tjuncHashTolSq;
int    tjuncLineCount;
int    tjuncPointCount;
char   tjuncPoints[sizeof(TjuncPoint_t) * MAX_TJUNC_POINTS];
float  tjuncSnapTolSq;
void  *tjuncSpatialHash[TJUNC_SPATIAL_HASH_SLOTS];
int   *triVertexIndexMap;

/* Native 0x45CE40 owns one zeroed 0xD30300-byte TJunc arena for each
   MergeSurfaces_Init/Shutdown lifetime.  KIWI still uses typed static
   working arrays, but preserves the native ownership boundary explicitly. */
static void *tjuncMergeScratch;
#define TJUNC_MERGE_SCRATCH_SIZE 0xD30300u

char s_assertDisable_FixSurfaceJunctions;
char s_assertDisable_RemoveDegenerateEdges;
char s_assertDisable_RemoveDegenerateEdges;
char s_assertDisable_RemoveDegenerateEdges;
char s_assertDisable_RemoveDegenerateEdges;
char s_assertDisable_TJunc_FindEdge;
char s_assertDisable_TJunc_Init;
char s_assertDisable_TJunc_ProcessSurface;
char s_assertDisable_TJunc_ProcessWinding;
char s_assertDisable_TJunc_ProcessWinding;
char s_assertDisable_TJunc_ProcessWinding;
char s_assertDisable_TjuncClampNode;
char s_assertDisable_TjuncClampNode;
char s_assertDisable_TjuncClampNode;
char s_assertDisable_TjuncFixFaces;
char s_assertDisable_TjuncFixFaces;

/* CoD4 0x45DF10: clear the native directional and spatial edge-hash
   carriers without touching line or point counters. */
static void TJunc_ClearHashTablesNative(void)
{
  if ( tjuncCreateNonAxial )
    memset(tjuncSpatialHash, 0, sizeof(tjuncSpatialHash));
  memset(tjuncNodeHash, 0, sizeof(tjuncNodeHash));
}

/*
================
TjuncReset

Resets T-junction globals: point count, edge lines, hash table
================
*/
int TjuncReset(void)
{
  tjuncLineCount = 0;
  tjuncPointCount = 0;
  TJunc_ClearHashTablesNative();
  return 0;
}

/*
================
TjuncClampNode

Clamps octree (face, s, t) to valid range, returns node index
================
*/
TjuncEdgeLine_t *TjuncClampNode(unsigned int face, unsigned int s, unsigned int t)
{
  /* Cube-face coordinate wrapping: 3 faces x 8x8 grid.
     When s or t goes out of range (-1 or 8), wrap to the
     adjacent face with mirrored/rotated coordinates.
     Loops because one wrap can push the other axis out of range. */
  #define GRID_MAX 7
  #define GRID_SIZE 8

  while ( 1 )
  {
    while ( 1 )
    {
      while ( 1 )
      {
        /* s underflow: wrap to adjacent face along -s edge */
        while ( s == (unsigned)-1 )
        {
          if ( face == 0 )      { t = GRID_MAX - t; s = 0; face = 1; }
          else if ( face == 1 ) { s = 0; t = GRID_MAX - t; face = 0; }
          else                  { s = GRID_MAX - t; face = 1; t = 0; }
        }
		
        /* s overflow: wrap to adjacent face along +s edge */
        if ( s != GRID_SIZE )
          break;
        if ( face == 0 )      { s = GRID_MAX; face = 1; }
        else if ( face == 1 ) { s = GRID_MAX; face = 0; }
        else                  { s = t; face = 1; t = GRID_MAX; }
      }
	  
      /* t underflow: wrap to adjacent face along -t edge */
      if ( t != (unsigned)-1 )
        break;
      if ( face == 0 )      { t = 0; s = GRID_MAX - s; face = 2; }
      else if ( face == 1 ) { t = GRID_MAX - s; s = 0; face = 2; }
      else                  { t = 0; s = GRID_MAX - s; face = 0; }
    }
	
    /* t overflow: wrap to adjacent face along +t edge */
    if ( t != GRID_SIZE )
      break;
    if ( face == 0 )      { t = GRID_MAX; face = 2; }
    else if ( face == 1 ) { t = s; s = GRID_MAX; face = 2; }
    else                  { t = GRID_MAX; face = 0; }
  }
  Assert(face <= 2, s_assertDisable_TjuncClampNode);
  Assert(s < GRID_SIZE, s_assertDisable_TjuncClampNode);
  Assert(t < GRID_SIZE, s_assertDisable_TjuncClampNode);
  return tjuncNodeHash[GRID_SIZE * GRID_SIZE * face + GRID_SIZE * t + s];
  #undef GRID_MAX
  #undef GRID_SIZE
}

/*
================
TJunc_ProcessBrushList

Iterates brush list, inserts non-detail brushes into grid tree
================
*/
/* CoD4 0x45E870: retain the native result carrier while inserting every
   non-detail surface from the linked list into the grid tree. */
int FixTriSurfTJunctions(TriSurf_t *brushList)
{
  TriSurf_t *brush;
  intptr_t result;

  result = (intptr_t)brushList;
  for ( brush = brushList; brush; brush = brush->next )
  {
    result = (intptr_t)brush;
    if ( !brush->props->surfFlagBit7 )
      result = (intptr_t)GridTree_Insert(brush);
  }
  return (int)result;
}

int TJunc_ProcessBrushList(TriSurf_t *brushList)
{
  return FixTriSurfTJunctions(brushList);
}

/*
================
TJunc_SetBounds

Sets global bounding box for edge line hash computation
================
*/
/* CoD4 0x45E8B0: copy the active preprocessing bounds and return the
   destination of the second native vector copy. */
float *SetTriSurfaceBounds(float *boundsMin, float *boundsMax)
{
  VectorCopy(boundsMin, tjuncBoundsMin);
  VectorCopy(boundsMax, tjuncBoundsMax);
  return tjuncBoundsMax;
}

void TJunc_SetBounds(float *boundsMin, float *boundsMax)
{
  SetTriSurfaceBounds(boundsMin, boundsMax);
}

/*
================
TJunc_SetSnapTolerance

Sets squared snap tolerance globals
================
*/
/* CoD4 0x45E8E0: squared one-cell tolerance and the exact 23-cell search
   extent used by non-axial line matching. */
void SetTJuncCellSize(float cellSize)
{
  float searchExtent = cellSize * 23.0f;

  tjuncSnapTolSq = cellSize * cellSize;
  tjuncHashTolSq = searchExtent * searchExtent;
}

void TJunc_SetSnapTolerance(float tolerance)
{
  SetTJuncCellSize(tolerance);
}

/*
================
TJunc_SetCreateNonAxial

Sets flag for non-axial edge line creation
================
*/
/* CoD4 0x45E910: enable or disable non-axial T-junction line creation. */
char SetTJuncEnabled(char enabled)
{
  tjuncCreateNonAxial = enabled;
  return enabled;
}

char TJunc_SetCreateNonAxial(char enabled)
{
  return SetTJuncEnabled(enabled);
}

/*
================
TJunc_Init

Initializes T-junction system, allocates aux data buffer
================
*/
void *TJunc_Init( int auxElemSize )
{
  Assert(!tjuncPointCount, s_assertDisable_TJunc_Init);

  if ( auxElemSize != tjuncAuxElemSize )
  {
    tjuncAuxElemSize = auxElemSize;
    free(tjuncAuxBuffer);

    if ( tjuncAuxElemSize )
      tjuncAuxBuffer = malloc(tjuncAuxElemSize * MAX_TJUNC_AUX_POINTS);
    else
      tjuncAuxBuffer = NULL;
  }
  return tjuncAuxBuffer;
}

void *TJunc_MergeScratchInit(void)
{
  Assert(!tjuncMergeScratch, s_assertDisable_TJunc_Init);
  tjuncMergeScratch = malloc(TJUNC_MERGE_SCRATCH_SIZE);
  if ( !tjuncMergeScratch )
    Com_Error("Out of memory allocating T-junction merge scratch");
  memset(tjuncMergeScratch, 0, TJUNC_MERGE_SCRATCH_SIZE);
  return tjuncMergeScratch;
}

void TJunc_MergeScratchShutdown(void)
{
  free(tjuncMergeScratch);
  tjuncMergeScratch = NULL;
}

/*
================
TJunc_UpdateSnapPoint

Update snap point XYZ if new vertex is closer to edge line
================
*/
void TJunc_UpdateSnapPoint( float *newVertex, TjuncPoint_t *pt, int flags, TjuncEdgeLine_t *el )
{
  double dOldA, dOldB, dNewA, dNewB;

  pt->flags |= flags;

  /* perpendicular distance of existing snap point to edge planes */
  dOldA = DotProduct120(pt->pos, el->normalA) - (double)el->distA;
  dOldB = DotProduct210(pt->pos, el->normalB) - (double)el->distB;

  /* perpendicular distance of new vertex to edge planes */
  dNewA = DotProduct210(newVertex, el->normalA) - (double)el->distA;
  dNewB = DotProduct210(newVertex, el->normalB) - (double)el->distB;

  /* update if new vertex is closer to edge line */
  { double distNew = MulAdd2(dNewA,dNewA, dNewB,dNewB);
    double distOld = MulAdd2(dOldA,dOldA, dOldB,dOldB);
    if ( distNew < distOld )
      VectorCopy(newVertex, pt->pos);
  }
}

/*
================
TJunc_InsertPoint

Inserts point into edge line's sorted chain
================
*/
void TJunc_InsertPoint(float *vertex, float intercept, int flags, TjuncEdgeLine_t *edgeLine)
{
  TjuncPoint_t *newPt;
  TjuncPoint_t *scan;

  if ( tjuncPointCount == MAX_TJUNC_POINTS )
    Com_Error("MAX_TJUNC_POINTS");

  newPt = &((TjuncPoint_t *)tjuncPoints)[tjuncPointCount];
  newPt->intercept = intercept;
  VectorCopy(vertex, newPt->pos);
  newPt->flags = flags;

  /* Native 0x45D100 merges solely on line intercept tolerance; auxiliary
     vertex payload belongs to winding reconstruction, not the point chain. */
  scan = edgeLine->sentinel.next;
  while ( scan != &edgeLine->sentinel )
  {
    double diff = scan->intercept - intercept;

    if ( diff * diff < edgeLine->tolerance )
    {
      TJunc_UpdateSnapPoint(vertex, scan, flags, edgeLine);
      return;
    }

    if ( intercept < (double)scan->intercept )
      break;
    scan = scan->next;
  }

  tjuncPointCount++;
  newPt->next = scan;
  newPt->prev = scan->prev;
  scan->prev->next = newPt;
  scan->prev = newPt;
}

/*
================
TJunc_ClassifyAndInsertEdge

Classifies edge direction, inserts both endpoints
================
*/
void TJunc_ClassifyAndInsertEdge(float *vertA, float *vertB, TjuncEdgeLine_t *el, float tolerance)
{
  float dotA = DotProduct210(vertA, el->edgeDir);
  float dotB = DotProduct210(vertB, el->edgeDir);
  double diff = (double)dotB - dotA;
  int dirFlags;

  if ( diff * diff >= tolerance )
    dirFlags = (diff >= 0.0) ? 1 : 2;
  else
    dirFlags = 4;

  if ( el->tolerance > (double)tolerance )
    el->tolerance = tolerance;
  TJunc_InsertPoint(vertA, dotA, dirFlags, el);
  TJunc_InsertPoint(vertB, dotB, dirFlags, el);
}

/*
================
TJunc_HashLookup

Computes spatial hash bucket from axis + vertex position
================
*/
float **TJunc_HashLookup(int axis, float *vertex)
{
  #define SPATIAL_DIM 128
  int ax0 = (~axis) & 1;
  int ax1 = (~axis) & 2;
  int col, row;

  /* normalize vertex position to [0, SPATIAL_DIM) grid, clamp */
  col = fistp_sub((vertex[ax0] - tjuncBoundsMin[ax0]) / (tjuncBoundsMax[ax0] - tjuncBoundsMin[ax0] + 1.0) * SPATIAL_DIM, FISTP_HALF_BIAS);
  if ( col < 0 ) col = 0; else if ( col > SPATIAL_DIM - 1 ) col = SPATIAL_DIM - 1;

  row = fistp_sub((vertex[ax1] - tjuncBoundsMin[ax1]) / (tjuncBoundsMax[ax1] - tjuncBoundsMin[ax1] + 1.0) * SPATIAL_DIM, FISTP_HALF_BIAS);
  if ( row < 0 ) row = 0; else if ( row > SPATIAL_DIM - 1 ) row = SPATIAL_DIM - 1;

  return (float **)&tjuncSpatialHash[col + SPATIAL_DIM * (row + SPATIAL_DIM * axis)];
  #undef SPATIAL_DIM
}

/*
================
TJunc_DirectionToHashKey

Converts direction to octree hash key (axis, s, t)
================
*/
int TJunc_DirectionToHashKey(int *outS, float *direction, int *outAxis, int *outT)
{
  int result;
  float normCoordT;
  float normCoordS;

  result = VecLargestAxis(direction);
  int idx1 = (~result) & 1;
  normCoordS = (direction[idx1] / direction[result] + 1.0f) * TJUNC_COORD_SCALE;
  *outS = fistp_sub(normCoordS, FISTP_HALF_BIAS);

  int idx2 = (~result) & 2;
  normCoordT = (direction[idx2] / direction[result] + 1.0f) * TJUNC_COORD_SCALE;
  *outT = fistp_sub(normCoordT, FISTP_HALF_BIAS);
  *outAxis = result;

  return result;
}

/*
================
TJunc_AddEdgeLine

Creates edge line struct, adds to hash, inserts endpoints
================
*/
void TJunc_AddEdgeLine(float *vertA, float *vertB, int hashAxis, float tolerance)
{
  #define MAX_EDGE_LINES 0x10000
  #define TJUNC_HASH_DIM 8
  TjuncEdgeLine_t *el;
  vec3_t edgeDir;
  int hashAxisOut, hashS, hashT, hashIdx;

  if ( tjuncLineCount == MAX_EDGE_LINES )
    Com_Error("MAX_EDGE_LINES");

  el = &tjuncEdgeLines[tjuncLineCount];
  VectorSubtract(vertB, vertA, edgeDir);
  if ( Vec3NormalizeTo(edgeDir, el->edgeDir) == 0.0 )
    return;

  VectorCopy(vertA, el->startVert);
  MakeNormalVectors(el->edgeDir, el->normalA, el->normalB);
  el->distA = DotProduct120(el->normalA, vertA);
  el->distB = DotProduct120(el->normalB, vertA);
  el->sentinel.prev = &el->sentinel;
  el->sentinel.next = &el->sentinel;
  el->tolerance = tolerance;

  TJunc_DirectionToHashKey(&hashS, edgeDir, &hashAxisOut, &hashT);
  hashIdx = (hashS >> 1) + TJUNC_HASH_DIM * ((hashT >> 1) + TJUNC_HASH_DIM * hashAxisOut);
  el->hashNext = tjuncNodeHash[hashIdx];
  tjuncNodeHash[hashIdx] = el;

  TJunc_ClassifyAndInsertEdge(vertA, vertB, el, tolerance);
  tjuncLineCount++;

  if ( hashAxis >= 0 )
  {
    float **bucket = TJunc_HashLookup(hashAxis, vertA);
    el->hashNext2 = (TjuncEdgeLine_t *)*bucket;
    *bucket = (float *)el;
  }
  #undef MAX_EDGE_LINES
  #undef TJUNC_HASH_DIM
}

/*
================
TJunc_DistToEdgeLine

Max squared perpendicular distance of two vertices to edge line.
================
*/
double TJunc_DistToEdgeLine(TjuncEdgeLine_t *el, float *vertB, float *vertA, float epsilon)
{
  double d;
  float sq, maxSq;

  /* test each vertex against both perpendicular planes — early out if too far */
  d = DotProduct120(vertA, el->normalA) - (double)el->distA;
  maxSq = d * d;
  if ( maxSq > (double)epsilon ) return maxSq;

  d = DotProduct210(vertA, el->normalB) - (double)el->distB;
  sq = d * d;
  if ( sq > (double)epsilon ) return sq;
  if ( sq > maxSq ) maxSq = sq;

  d = DotProduct120(vertB, el->normalA) - (double)el->distA;
  sq = d * d;
  if ( sq > (double)epsilon ) return sq;
  if ( sq > maxSq ) maxSq = sq;

  d = DotProduct210(vertB, el->normalB) - (double)el->distB;
  sq = d * d;
  if ( sq > (double)epsilon ) return sq;
  if ( sq > maxSq ) maxSq = sq;

  return maxSq;
}

/* CoD4 0x45D830: examine one directional hash cell and retain its closest
   line through the caller's candidate and epsilon carriers. */
static void TJunc_SearchHashCell(float *firstVertex, float *secondVertex,
                                 int face, int s, int t, float *epsilon,
                                 TjuncEdgeLine_t **bestLine)
{
  TjuncEdgeLine_t *line;

  for ( line = TjuncClampNode(face, s, t); line; line = line->hashNext )
  {
    double distance = TJunc_DistToEdgeLine(line, firstVertex, secondVertex, *epsilon);
    if ( distance < *epsilon )
    {
      *epsilon = (float)distance;
      *bestLine = line;
    }
  }
}

/*
================
TJunc_FindEdgeLineByHash

CoD4 0x45D780 searches its enclosing two-by-two directional hash cells.
================
*/
TjuncEdgeLine_t *TJunc_FindEdgeLineByHash(float *direction, float *firstVertex,
                                           float *secondVertex, float tolerance)
{
  int face;
  int hashS;
  int hashT;
  int row;
  int column;
  TjuncEdgeLine_t *bestLine = NULL;

  TJunc_DirectionToHashKey(&hashS, direction, &face, &hashT);
  hashS = (hashS + 1) >> 1;
  hashT = (hashT + 1) >> 1;
  for ( row = -1; row != 1; ++row )
  {
    for ( column = -1; column != 1; ++column )
      TJunc_SearchHashCell(firstVertex, secondVertex, face,
                           hashS + column, hashT + row,
                           &tolerance, &bestLine);
  }
  return bestLine;
}

/* CoD4 0x45DBF0: the low-magnitude route performs its full distance walk
   without selecting an edge for the caller. */
void TJunc_FindEdgeLineBrute(float *firstVertex, float *secondVertex, float tolerance)
{
  int lineIndex;

  for ( lineIndex = 0; lineIndex < tjuncLineCount; ++lineIndex )
    (void)TJunc_DistToEdgeLine(&tjuncEdgeLines[lineIndex], firstVertex, secondVertex, tolerance);
}

/*
================
TJunc_FindMatchingEdgeLine

CoD4 0x45D720 dispatches low-magnitude directions to the native brute walk;
larger directions use the cube-face directional hash.
================
*/
TjuncEdgeLine_t *TJunc_FindMatchingEdgeLine(float *firstVertex, float *secondVertex,
                                             float *direction, float epsilon)
{
  if ( tjuncHashTolSq >= DotProduct(direction, direction) )
  {
    TJunc_FindEdgeLineBrute(firstVertex, secondVertex, epsilon);
    return NULL;
  }
  return TJunc_FindEdgeLineByHash(direction, firstVertex, secondVertex, epsilon);
}

/* CoD4 0x45DC70: examine every axis-spatial bucket entry without promoting
   a selected edge to the caller. */
void TJunc_FindEdgeLineByIndex(int hashAxis, float *firstVertex,
                               float *secondVertex, float tolerance)
{
  TjuncEdgeLine_t *line;

  for ( line = (TjuncEdgeLine_t *)*TJunc_HashLookup(hashAxis, firstVertex);
        line;
        line = line->hashNext2 )
    (void)TJunc_DistToEdgeLine(line, firstVertex, secondVertex, tolerance);
}

/*
================
TJunc_FindEdge

Main edge finder: computes epsilon, dispatches to index or matching search
================
*/
TjuncEdgeLine_t *TJunc_FindEdge(float *curVertex, float *outEpsilon, float *lastVertex, int hashAxis)
{
  vec3_t dir;
  double lenSqScaled;
  float epsilon;

  VectorSubtract(curVertex, lastVertex, dir);
  lenSqScaled = DotProduct(dir, dir) * 0.25;
  epsilon = (tjuncSnapTolSq > lenSqScaled) ? (float)lenSqScaled : tjuncSnapTolSq;

  Assert(outEpsilon, s_assertDisable_TJunc_FindEdge);
  *outEpsilon = epsilon;

  if ( hashAxis >= 0 )
  {
    /* 0x45DC70 performs the axis-bucket distance walk for this route, but
       like the native helper it leaves no selected-edge return value. */
    TJunc_FindEdgeLineByIndex(hashAxis, curVertex, lastVertex, epsilon);
    return NULL;
  }
  return TJunc_FindMatchingEdgeLine(curVertex, lastVertex, dir, epsilon);
}

/*
================
TJunc_ClassifyEdgeAxis

Classifies edge as axis-aligned (0=X, 1=Y, 2=Z) or non-axial (-1)
================
*/
int TJunc_ClassifyEdgeAxis(float *vertA, float *vertB)
{

  if ( !tjuncCreateNonAxial ) {
    return -1;
  }

  volatile float threshold = tjuncSnapTolSq * 0.25f;

  /* compute per-axis significance flags */
  double dx = (double)vertB[0] - vertA[0];
  volatile float dy = vertB[1] - vertA[1];
  volatile float dz = vertB[2] - vertA[2];
  int flags = 0;

  if (dx * dx > threshold) flags |= 1;
  if ((double)dy * dy > threshold) flags |= 2;
  if ((double)dz * dz > threshold) flags |= 4;

  /* return axis only if exactly one axis is significant */
  if (flags == 1) return 0;
  if (flags == 2) return 1;
  if (flags == 4) return 2;
  return -1;
}

/* CoD4 0x45CFB0: dispatch one previous-current winding edge to a matching
   line or to native line creation. */
static void TJunc_ProcessEdgeNative(float *previous, float *current)
{
  float tolerance;
  int hashAxis;
  TjuncEdgeLine_t *edgeLine;

  hashAxis = TJunc_ClassifyEdgeAxis(previous, current);
  edgeLine = TJunc_FindEdge(previous, &tolerance, current, hashAxis);
  if ( edgeLine )
    TJunc_ClassifyAndInsertEdge(previous, current, edgeLine, tolerance);
  else
    TJunc_AddEdgeLine(previous, current, hashAxis, tolerance);
}

/*
================
TJunc_ProcessWinding

CoD4 0x45CF20 walks previous-current pairs of the packed winding directly;
auxiliary winding data is intentionally not part of this edge-line pass.
================
*/
int TJunc_ProcessWinding(Winding_t *winding)
{
  int previous;
  unsigned int pointIndex;

  Assert(winding, s_assertDisable_TJunc_ProcessWinding);
  previous = winding->numpoints - 1;
  for ( pointIndex = 0; pointIndex < (unsigned int)winding->numpoints; ++pointIndex )
  {
    TJunc_ProcessEdgeNative(winding->points[previous], winding->points[pointIndex]);
    previous = (int)pointIndex;
  }
  return (int)pointIndex;
}

/*
================
TJunc_ProcessSurface

CoD4 0x45CED0 is passed the embedded WindingAuxPair at TriSurf::winding,
then follows the native holeCount/holes fields immediately after that pair.
================
*/
int TJunc_ProcessSurface(WindingAuxPair_t *surface)
{
  TriSurf_t *ts;
  int holeIndex;

  Assert(surface, s_assertDisable_TJunc_ProcessSurface);
  ts = (TriSurf_t *)((char *)surface - offsetof(TriSurf_t, winding));
  Assert(ts->auxElemSize == tjuncAuxElemSize || !tjuncAuxElemSize, s_assertDisable_TJunc_ProcessSurface);

  TJunc_ProcessWinding(surface->winding);
  for ( holeIndex = 0; holeIndex < ts->holeCount; ++holeIndex )
    TJunc_ProcessWinding(ts->holes[holeIndex].winding);
  return holeIndex;
}

/*
================
FixSurfaceJunctions

Inserts T-junction points into winding, expands vertex list
================
*/
void FixSurfaceJunctions(WindingAuxPair_t *verts, int auxElemSize, float *surfPlane)
{
  Winding_t *winding, *newWinding;
  TjuncEdgeLine_t *edge;
  TjuncPoint_t *chainNode, *sentinel;
  float *curVert, *nextVert, *outVert;
  vec3_t outVertBuf[MAX_CONCAVE_WINDING_POINTS];
  void *auxBlock;
  int srcIdx, dstIdx, nextIdx, edgeAxis;
  float epsilonOut, snapDist;
  float curIntercept, nextIntercept;
  double planeDist;
  int forward;

  auxBlock = auxElemSize ? malloc(auxElemSize * MAX_CONCAVE_WINDING_POINTS) : NULL;
  winding = verts->winding;
  dstIdx = 0;

  for ( srcIdx = 0; srcIdx < winding->numpoints; srcIdx++ )
  {
    curVert = winding->points[srcIdx];

    /* copy current vertex to output */
    if ( dstIdx == MAX_CONCAVE_WINDING_POINTS )
      Com_Error("MAX_POINTS_ON_CONCAVE_WINDING");
    outVert = outVertBuf[dstIdx];
    VectorCopy(curVert, outVert);
    if ( auxElemSize )
      AuxDataCopy(auxElemSize, (char *)verts->auxData, srcIdx, 1, (char *)auxBlock, dstIdx);
    dstIdx++;

    /* find edge line between this vertex and the next */
    nextIdx = (srcIdx + 1) % winding->numpoints;
    nextVert = winding->points[nextIdx];
    edgeAxis = TJunc_ClassifyEdgeAxis(nextVert, curVert);
    edge = TJunc_FindEdge(curVert, &epsilonOut, nextVert, edgeAxis);
    if ( !edge )
      continue;

    /* tighten tolerance and compute snap distance */
    if ( edge->tolerance > (double)epsilonOut )
      edge->tolerance = epsilonOut;
    snapDist = (float)sqrt(edge->tolerance);

    /* project both endpoints onto the edge line */
    curIntercept = DotProduct210(edge->edgeDir, curVert);
    nextIntercept = DotProduct210(edge->edgeDir, nextVert);

    /* walk the chain of junction points along this edge */
    forward = curIntercept >= (double)nextIntercept;
    chainNode = forward ? edge->sentinel.prev : edge->sentinel.next;
    sentinel = &edge->sentinel;

    while ( chainNode != sentinel )
    {
      float chainIntercept = chainNode->intercept;

      /* check if we've passed the endpoint
         On x86, x87 keeps float+float at 80-bit extended for comparison — matches original.
         On x64, SSE2 does float+float at 32-bit, losing the extra precision.
         Use double on x64 to match x86's 80-bit behavior. */
#ifdef _WIN64
      if ( !forward && (double)nextIntercept - (double)snapDist < chainIntercept )
        break;
      if ( forward && (double)nextIntercept + (double)snapDist > chainIntercept )
        break;

      /* check if junction point is between the two vertices */
      if ( (!forward && (double)curIntercept + (double)snapDist < chainIntercept)
        || (forward && (double)curIntercept - (double)snapDist > chainIntercept) )
#else
      if ( !forward && nextIntercept - snapDist < chainIntercept )
        break;
      if ( forward && nextIntercept + snapDist > chainIntercept )
        break;

      /* check if junction point is between the two vertices */
      if ( (!forward && curIntercept + snapDist < chainIntercept)
        || (forward && curIntercept - snapDist > chainIntercept) )
#endif
      {
        /* check if junction point lies on the surface plane */
        planeDist = DotProduct210(chainNode->pos, surfPlane) - surfPlane[3];
        if ( planeDist > -snapDist && planeDist < snapDist )
        {
          if ( dstIdx == MAX_CONCAVE_WINDING_POINTS )
            Com_Error("MAX_POINTS_ON_CONCAVE_WINDING");
          outVert = outVertBuf[dstIdx];
          VectorCopy(chainNode->pos, outVert);
          if ( auxElemSize )
          {
            double frac = (chainNode->intercept - curIntercept) / (nextIntercept - curIntercept);
            g_lerpAuxDataCallback(
                (float *)((char *)verts->auxData + srcIdx * auxElemSize),
                (float *)((char *)verts->auxData + nextIdx * auxElemSize),
                frac,
                (float *)((char *)auxBlock + dstIdx * auxElemSize)
            );
          }
          dstIdx++;
        }
      }
      chainNode = forward ? chainNode->prev : chainNode->next;
    }
  }

  /* replace winding if new vertices were added */
  Assert(dstIdx >= verts->winding->numpoints, s_assertDisable_FixSurfaceJunctions);
  if ( dstIdx > verts->winding->numpoints )
  {
    FreeWinding(verts->winding);
    newWinding = AllocWinding(dstIdx);
    verts->winding = newWinding;
    newWinding->numpoints = dstIdx;
    memcpy(newWinding->points, outVertBuf, sizeof(vec3_t) * dstIdx);
    if ( auxElemSize )
    {
      free(verts->auxData);
      verts->auxData = malloc(auxElemSize * dstIdx);
      memcpy(verts->auxData, auxBlock, auxElemSize * dstIdx);
    }
  }

  free(auxBlock);
}

/* CoD4 0x45E080: direct native winding reconstruction.  This path carries
   positions only; the older public helper above retains the donor aux-data
   interpolation path for callers that require it. */
static void FixSurfaceJunctionsNative(Winding_t **windingPtr, float *surfPlane)
{
  Winding_t *winding;
  Winding_t *newWinding;
  TjuncEdgeLine_t *edge;
  TjuncPoint_t *node;
  TjuncPoint_t *sentinel;
  vec3_t outPoints[MAX_CONCAVE_WINDING_POINTS];
  int srcIndex;
  int dstIndex;

  Assert(windingPtr, s_assertDisable_FixSurfaceJunctions);
  Assert(*windingPtr, s_assertDisable_FixSurfaceJunctions);
  winding = *windingPtr;
  dstIndex = 0;

  for ( srcIndex = 0; srcIndex < winding->numpoints; ++srcIndex )
  {
    float *point = winding->points[srcIndex];
    float *nextPoint = winding->points[(srcIndex + 1) % winding->numpoints];
    float tolerance;
    float toleranceLog;
    float pointIntercept;
    float nextIntercept;
    int edgeAxis;

    Assert(dstIndex != MAX_CONCAVE_WINDING_POINTS, s_assertDisable_FixSurfaceJunctions);
    VectorCopy(point, outPoints[dstIndex]);
    ++dstIndex;

    edgeAxis = TJunc_ClassifyEdgeAxis(point, nextPoint);
    edge = TJunc_FindEdge(point, &tolerance, nextPoint, edgeAxis);
    if ( !edge )
      continue;

    if ( tolerance < edge->tolerance )
      edge->tolerance = tolerance;
    toleranceLog = (float)log(edge->tolerance);

    pointIntercept = DotProduct210(point, edge->edgeDir);
    nextIntercept = DotProduct210(nextPoint, edge->edgeDir);
    sentinel = &edge->sentinel;
    node = nextIntercept <= pointIntercept ? sentinel->prev : sentinel->next;

    while ( node != sentinel )
    {
      if ( nextIntercept <= pointIntercept )
      {
        if ( node->intercept < nextIntercept + toleranceLog )
          break;
      }
      else if ( node->intercept > nextIntercept - toleranceLog )
      {
        break;
      }

      if ( (nextIntercept > pointIntercept && node->intercept > pointIntercept + toleranceLog)
        || (nextIntercept < pointIntercept && node->intercept < pointIntercept - toleranceLog) )
      {
        float planeDistance = DotProduct210(node->pos, surfPlane) - surfPlane[3];

        if ( planeDistance > -toleranceLog && toleranceLog > planeDistance )
        {
          Assert(dstIndex != MAX_CONCAVE_WINDING_POINTS, s_assertDisable_FixSurfaceJunctions);
          VectorCopy(node->pos, outPoints[dstIndex]);
          ++dstIndex;
        }
      }

      node = nextIntercept <= pointIntercept ? node->prev : node->next;
    }
  }

  Assert(dstIndex >= winding->numpoints, s_assertDisable_FixSurfaceJunctions);
  if ( dstIndex > winding->numpoints )
  {
    FreeWinding(winding);
    newWinding = AllocWinding(dstIndex);
    newWinding->numpoints = dstIndex;
    memcpy(newWinding->points, outPoints, sizeof(vec3_t) * dstIndex);
    *windingPtr = newWinding;
  }
}

/* CoD4 0x45DF60: reset every edge-line sentinel before the second collection
   pass, then restart packed point allocation at zero. */
static void TJunc_ResetEdgePointListsNative(void)
{
  int lineIndex;

  for ( lineIndex = 0; lineIndex < tjuncLineCount; ++lineIndex )
  {
    tjuncEdgeLines[lineIndex].sentinel.prev = &tjuncEdgeLines[lineIndex].sentinel;
    tjuncEdgeLines[lineIndex].sentinel.next = &tjuncEdgeLines[lineIndex].sentinel;
  }
  tjuncPointCount = 0;
}

/* CoD4 0x45DFD0: process every embedded surface winding pair in the input
   pointer array. */
static void TJunc_ProcessSurfaceArrayNative(intptr_t *surfArray, int surfCount)
{
  int surfIndex;

  for ( surfIndex = 0; surfIndex < surfCount; ++surfIndex )
    TJunc_ProcessSurface((WindingAuxPair_t *)&((TriSurf_t *)surfArray[surfIndex])->winding);
}

/* CoD4 0x45E010: fix the main winding and each hole through the surface's
   native packed WindingAuxPair carrier. */
static void TJunc_FixSurfaceEdgesNative(WindingAuxPair_t *surface)
{
  TriSurf_t *ts = (TriSurf_t *)((char *)surface - offsetof(TriSurf_t, winding));
  int holeIndex;

  FixSurfaceJunctionsNative(&surface->winding, ts->props->plane);
  for ( holeIndex = 0; holeIndex < ts->holeCount; ++holeIndex )
    FixSurfaceJunctionsNative(&ts->holes[holeIndex].winding, ts->props->plane);
}

/*
================
TjuncFixSurfaceEdges

Fixes junctions for surface main winding + holes.
================
*/
int TjuncFixSurfaceEdges(TriSurf_t *ts)
{
  TJunc_FixSurfaceEdgesNative((WindingAuxPair_t *)&ts->winding);
  return ts->holeCount;
}

/*
================
TjuncFixFaces

Orchestrates T-junction fixing: collect edges, reset chains, re-insert, fix surfaces
================
*/
int TjuncFixFaces(intptr_t *surfArray, int surfCount, TriSurf_t *extraSurf)
{
  int i;

  Assert(tjuncMergeScratch, s_assertDisable_TjuncFixFaces);
  Assert(!tjuncLineCount, s_assertDisable_TjuncFixFaces);
  Assert(!tjuncPointCount, s_assertDisable_TjuncFixFaces);

  /* pass 1: collect all edges */
  if ( extraSurf )
    TJunc_ProcessSurface((WindingAuxPair_t *)&extraSurf->winding);
  TJunc_ProcessSurfaceArrayNative(surfArray, surfCount);

  /* reset all edge line linked lists to empty (sentinel → self) */
  TJunc_ResetEdgePointListsNative();

  /* pass 2: re-insert all edges, then fix junctions */
  if ( extraSurf )
    TJunc_ProcessSurface((WindingAuxPair_t *)&extraSurf->winding);
  TJunc_ProcessSurfaceArrayNative(surfArray, surfCount);
  for ( i = 0; i < surfCount; i++ )
    TJunc_FixSurfaceEdgesNative((WindingAuxPair_t *)&((TriSurf_t *)surfArray[i])->winding);

  TjuncReset();
  return 0;
}

/*
================
RemoveDegenerateEdges

Removes near-zero-length edges from winding
================
*/
void RemoveDegenerateEdges(WindingAuxPair_t *verts, int auxElemSize)
{
  #define DEGENERATE_EPSILON 0.001f
  Winding_t *w;
  char *auxData;
  int i, next;

  Assert(verts, s_assertDisable_RemoveDegenerateEdges);
  Assert(verts->winding, s_assertDisable_RemoveDegenerateEdges);
  Assert(verts->auxData || !auxElemSize, s_assertDisable_RemoveDegenerateEdges);

  w = verts->winding;
  auxData = (char *)verts->auxData;
  Assert(w->numpoints > 0 && w->numpoints < MAX_CONCAVE_WINDING_POINTS, s_assertDisable_RemoveDegenerateEdges);

  i = 0;
  while ( i < w->numpoints )
  {
    next = (i + 1) % w->numpoints;
    if ( !VectorCompareEpsilon(w->points[i], w->points[next], DEGENERATE_EPSILON, 3) )
    {
      i++;
      continue;
    }

    /* degenerate edge found — remove next vertex by shifting tail */
    if ( next )
    {
      int tail = w->numpoints - next;
      memcpy(w->points[i], w->points[next], sizeof(vec3_t) * tail);
      if ( auxElemSize )
        memcpy(&auxData[auxElemSize * i], &auxData[auxElemSize * next], auxElemSize * tail);
    }
    else
    {
      /* next wrapped to 0 — back up one vertex instead */
      i--;
    }
    w->numpoints--;
  }
  #undef DEGENERATE_EPSILON
}

/* CoD4 0x45E5B0: the native cleaner shifts only the packed 12-byte points. */
static void TJunc_RemoveDegenerateEdgesWindingNative(Winding_t *winding)
{
  int pointIndex;

  Assert(winding, s_assertDisable_RemoveDegenerateEdges);
  Assert(winding->numpoints > 0 && winding->numpoints < MAX_CONCAVE_WINDING_POINTS, s_assertDisable_RemoveDegenerateEdges);

  pointIndex = 0;
  while ( pointIndex < winding->numpoints )
  {
    int nextIndex = (pointIndex + 1) % winding->numpoints;

    if ( !VectorCompareEpsilon(winding->points[pointIndex], winding->points[nextIndex], 0.001f, 3) )
    {
      ++pointIndex;
      continue;
    }

    if ( nextIndex )
      memcpy(winding->points[pointIndex], winding->points[nextIndex], sizeof(vec3_t) * (winding->numpoints - nextIndex));
    else
      --pointIndex;
    --winding->numpoints;
  }
}

/* CoD4 0x45E560: dispatch the packed main winding and each hole. */
static void TJunc_RemoveDegenerateSurfaceNative(WindingAuxPair_t *surface)
{
  TriSurf_t *ts = (TriSurf_t *)((char *)surface - offsetof(TriSurf_t, winding));
  int holeIndex;

  TJunc_RemoveDegenerateEdgesWindingNative(surface->winding);
  for ( holeIndex = 0; holeIndex < ts->holeCount; ++holeIndex )
    TJunc_RemoveDegenerateEdgesWindingNative(ts->holes[holeIndex].winding);
}

/* CoD4 0x45E540: native post-collection callback. */
static void TJunc_FixAndRemoveSurfaceNative(WindingAuxPair_t *surface)
{
  TJunc_FixSurfaceEdgesNative(surface);
  TJunc_RemoveDegenerateSurfaceNative(surface);
}

/*
================
RemoveDegenerateEdgesForFace

Removes degenerate edges from surface main winding + holes
================
*/
int RemoveDegenerateEdgesForFace(TriSurf_t *ts)
{
  int i;
  RemoveDegenerateEdges((WindingAuxPair_t *)&ts->winding, ts->auxElemSize);
  for ( i = 0; i < ts->holeCount; i++ )
    RemoveDegenerateEdges(&ts->holes[i], ts->auxElemSize);
  return ts->holeCount;
}

/*
================
TjuncProcessFace

Fixes T-junctions and removes degenerate edges for one face
================
*/
int TjuncProcessFace(TriSurf_t *surf)
{
  TJunc_FixAndRemoveSurfaceNative((WindingAuxPair_t *)&surf->winding);
  return surf->holeCount;
}

static void TJunc_ProcessGridSurface(TriSurf_t *surf)
{
  TJunc_ProcessSurface((WindingAuxPair_t *)&surf->winding);
}

static void TJunc_FixAndRemoveGridSurfaceNative(TriSurf_t *surf)
{
  TJunc_FixAndRemoveSurfaceNative((WindingAuxPair_t *)&surf->winding);
}

/*
================
TjuncOctreeProcess

Recursive octree T-junction processor: subdivides or processes directly
================
*/
int TjuncOctreeProcess(float *boundsMin, float *boundsMax, void (*faceCallback)(TriSurf_t *))
{
  #define OCTREE_MIN_EXTENT 8.0
  #define OCTREE_MAX_SURFACES 256
  float extent[3];
  int maxSurfs, count, oct;
  vec3_t mid, childMin, childMax;

  extent[0] = boundsMax[0] - boundsMin[0];
  extent[1] = boundsMax[1] - boundsMin[1];
  extent[2] = boundsMax[2] - boundsMin[2];
  maxSurfs = (extent[0] >= OCTREE_MIN_EXTENT || extent[1] >= OCTREE_MIN_EXTENT || extent[2] >= OCTREE_MIN_EXTENT) ? OCTREE_MAX_SURFACES : INT_MAX;

  count = GridTree_CountIntersecting(boundsMin, boundsMax, maxSurfs);
  if ( count <= 1 )
    return count;

  /* small enough to process directly */
  if ( count <= maxSurfs )
  {
    GridTree_ForEach(boundsMin, boundsMax, TJunc_ProcessGridSurface);
    GridTree_ForEach(boundsMin, boundsMax, faceCallback);
    TjuncReset();
    return 0;
  }

  /* subdivide into 8 octants */
  mid[0] = MIDF(boundsMin[0], boundsMax[0]);
  mid[1] = MIDF(boundsMin[1], boundsMax[1]);
  mid[2] = MIDF(boundsMin[2], boundsMax[2]);

  for ( oct = 0; oct < 8; oct++ )
  {
    int ax;
    for ( ax = 0; ax < 3; ax++ )
    {
      if ( oct & (1 << ax) )
      { childMin[ax] = boundsMin[ax]; childMax[ax] = mid[ax]; }
      else
      { childMin[ax] = mid[ax]; childMax[ax] = boundsMax[ax]; }
    }
    count = TjuncOctreeProcess(childMin, childMax, faceCallback);
  }
  return count;
  #undef OCTREE_MIN_EXTENT
  #undef OCTREE_MAX_SURFACES
}

/*
================
TjuncFixAll

Entry point: runs octree T-junction fixing on world bounds
================
*/
int TjuncFixAll(float *boundsMin, float *boundsMax)
{
  Assert(tjuncMergeScratch, s_assertDisable_TjuncFixFaces);
  return TjuncOctreeProcess(boundsMin, boundsMax, TJunc_FixAndRemoveGridSurfaceNative);
}

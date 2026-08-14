/*
tris_tesselate.c â€” Tesselation and triangulation

Reconstructed from cod2map.exe by Rose.

Triangulates concave windings into triangle fans using ear-clipping.
Handles split point insertion along edges, degenerate edge removal,
self-intersection detection and repair, and ear validity checking
against the winding's 2D projection.
*/

#include "cod4map.h"

int g_tessClipEarTable[NUM_TESS_CLIP_EAR_PASSES * 2] = {
  0x00000000, 0x40000000,  /* command=0, tol=2.0f */
  0x00000000, 0x3F000000,  /* command=0, tol=0.5f */
  0x00000000, 0x3E000000,  /* command=0, tol=0.125f */
  0x00000000, 0x3C23D70A,  /* command=0, tol=0.01f */
  0x00000001, 0x40000000,  /* command=1, tol=2.0f */
  0x00000001, 0x3F000000,  /* command=1, tol=0.5f */
  0x00000001, 0x3E000000,  /* command=1, tol=0.125f */
  0x00000001, 0x3C23D70A,  /* command=1, tol=0.01f */
  0x00000000, 0x3A03126F,  /* command=0, tol=0.0005f */
  0x00000000, 0x00000000,  /* command=0, tol=0.0f */
  0x00000001, 0x3A03126F,  /* command=1, tol=0.0005f */
  0x00000001, 0x00000000,  /* command=1, tol=0.0f */
};

char s_assertDisable_SetupTriSide;
char s_assertDisable_SetupTriSide;
char s_assertDisable_TesselateCheckEarValidity;
char s_assertDisable_TesselateCheckEarValidity;
char s_assertDisable_TesselateCheckEarValidity;
char s_assertDisable_TesselateCheckEarValidity;
char s_assertDisable_TesselateCheckEarValidity;
char s_assertDisable_TesselateCheckEarValidity;
char s_assertDisable_TesselateClipEar;
char s_assertDisable_TesselateClipEar;
char s_assertDisable_TesselateFixIntersections;
char s_assertDisable_TesselateFixIntersections;
char s_assertDisable_TesselateFixIntersections;
char s_assertDisable_TesselateRemoveDegenerateEdges;
char s_assertDisable_TesselateRemoveDegenerateEdges;
char s_assertDisable_TesselateRemoveVertex;
char s_assertDisable_TesselateWinding;
char s_assertDisable_TesselateWinding;


/*
================
TessSplitPointCompare

qsort callback: compares split points by edge index then position
================
*/
/* tessSplitPoint_t */
typedef struct TessSplitPoint_s {
    int      edgeIdx;  /* [0]  edge index in winding */
    float    position; /* [4]  position along edge */
    int      vertIdx;  /* [8]  source vertex index */
    intptr_t auxData;  /* [12] aux data pointer (freed after use) */
} tessSplitPoint_t;

int TessSplitPointCompare(tessSplitPoint_t *splitA, tessSplitPoint_t *splitB)
{
  if ( splitA->edgeIdx != splitB->edgeIdx )
    return splitA->edgeIdx - splitB->edgeIdx;
  if ( splitA->position >= (double)splitB->position )
    return 1;
  return -1;
}

/*
================
SetupTriSide

Computes 2D triangle side: direction, normal, signed distances
================
*/
void SetupTriSide(float *vertA, float *vertB, int axisX, int axisY, double *side)
{
  /* side layout: [0]=normalX [1]=normalY [2]=normalDist [3]=perpDist [4]=length */
  double nx = (double)vertA[axisY] - (double)vertB[axisY];
  double ny = (double)vertB[axisX] - (double)vertA[axisX];
  double len = sqrt(nx * nx + ny * ny);
  double inv;

  side[TRISIDE_LENGTH] = len;
  /* Native 0x45C3C0 deliberately leaves the normal at zero for a
     zero-length projected side.  The caller rejects the resulting ear via
     its distance/tolerance checks; asserting or dividing here changes that
     control path on coincident projected vertices. */
  if (len > 0.0)
  {
    inv = 1.0 / len;
    nx *= inv;
    ny *= inv;
  }
  side[0] = nx;
  side[1] = ny;
  side[2] = MulAdd2(vertA[axisX], nx, vertA[axisY], ny);
  side[3] = Det2x2(vertA[axisX], ny, vertA[axisY], nx);
  Assert(fabs(vertB[axisX] * ny - vertB[axisY] * nx - side[3] - len) < 0.001000000047497451, s_assertDisable_SetupTriSide);
}

/*
================
TriSideCheck

Point-in-triangle side test for ear clipping
================
*/
bool TriSideCheck(double *side0, int axisY, double *side2, double *side1, float *vert, int axisX)
{
  return MulAdd2(vert[axisY], side0[1], vert[axisX], side0[0]) - side0[2] < 0.0
      || MulAdd2(vert[axisY], side1[1], vert[axisX], side1[0]) - side1[2] < 0.0
      || MulAdd2(vert[axisY], side2[1], vert[axisX], side2[0]) - side2[2] >= 0.0;
}

/*
================
TrisCheckBothEdges

Checks both vertices of an edge against triangle sides
================
*/
bool TrisCheckBothEdges( double *side2, int axisY, float *vertA, double *side0, float *vertB, int axisX, double *side1)
{
  if ( !TriSideCheck(side0, axisY, side2, side1, vertA, axisX) )
    return 0;
  return TriSideCheck(side0, axisY, side2, side1, vertB, axisX);
}

/*
================
TrisVerticesCoincident2D

Checks if two vertex pairs are 2D-coincident
================
*/
bool TrisVerticesCoincident2D(int idxA, int axisX, int *vertArray, int axisY, int idxB, int idxC, int idxD)
{
  float *pts = (float *)&vertArray[1]; /* skip numpoints, points start at offset 4 */
  return pts[3 * idxB + axisX] == pts[3 * idxA + axisX]
      && pts[3 * idxB + axisY] == pts[3 * idxA + axisY]
      && pts[3 * idxC + axisX] == pts[3 * idxD + axisX]
      && pts[3 * idxC + axisY] == pts[3 * idxD + axisY];
}

/*
================
TrisLargestAngleCosine

Law of cosines: returns cosine of the largest triangle angle
================
*/

/* law of cosines: cos(C) = (a^2 + b^2 - c^2) / (2ab) where c is the longest edge */
double TrisLargestAngleCosine(double lenA, double lenB, double lenC)
{
  if ( lenA < lenB )
  {
    if ( lenA < lenC )
#ifdef _WIN64
      return ((double)lenB*lenB + (double)lenC*lenC - (double)lenA*lenA) * 0.5 / ((double)lenB*lenC);
#else
      return (lenB * lenB + lenC * lenC - lenA * lenA) * 0.5 / (lenB * lenC);
#endif
  }
  else if ( lenB < lenC )
  {
#ifdef _WIN64
    return ((double)lenA*lenA + (double)lenC*lenC - (double)lenB*lenB) * 0.5 / ((double)lenA*lenC);
#else
    return (lenA * lenA + lenC * lenC - lenB * lenB) * 0.5 / (lenA * lenC);
#endif
  }
#ifdef _WIN64
  return ((double)lenA*lenA + (double)lenB*lenB - (double)lenC*lenC) * 0.5 / ((double)lenA*lenB);
#else
  return (lenA * lenA + lenB * lenB - lenC * lenC) * 0.5 / (lenA * lenB);
#endif
}

/*
================
TrisSetupTrianglePlanes

Sets up 3 side planes for ear triangle, validates distances
================
*/
char TrisSetupTrianglePlanes( int vertIdxA, int axisX, int axisY, double *sideBA, int *windingArray, int vertIdxB, int vertIdxC, float tolerance, double *sideAC, double *sideCB )
{
  float *pts = (float *)&windingArray[1];  /* skip numpoints */
  float *vertA = &pts[3 * vertIdxA];
  float *vertB = &pts[3 * vertIdxB];
  float *vertC = &pts[3 * vertIdxC];
  double distA, distB, distC;

  /* compute 2D side planes and signed distances to opposite vertices */
  SetupTriSide( vertB, vertA, axisX, axisY, sideBA );
  distC = MulAdd2(vertC[axisY], sideBA[1], vertC[axisX], sideBA[0]) - sideBA[2];
  sideBA[TRISIDE_DIST] = distC;
  if ( distC < tolerance )
    return 0;

  SetupTriSide( vertA, vertC, axisX, axisY, sideAC );
  distB = MulAdd2(vertB[axisY], sideAC[1], vertB[axisX], sideAC[0]) - sideAC[2];
  sideAC[TRISIDE_DIST] = distB;
  if ( distB < tolerance )
    return 0;

  SetupTriSide( vertC, vertB, axisX, axisY, sideCB );
  distA = MulAdd2(vertA[axisY], sideCB[1], vertA[axisX], sideCB[0]) - sideCB[2];
  sideCB[TRISIDE_DIST] = distA;
  if ( distA < tolerance )
    return 0;

  /* shrink planes inward by tolerance, expand max distances to match */
  sideBA[2] -= tolerance;
  sideAC[2] -= tolerance;
  sideCB[2] -= tolerance;
  sideBA[TRISIDE_DIST] += tolerance + tolerance;
  sideAC[TRISIDE_DIST] += tolerance + tolerance;
  sideCB[TRISIDE_DIST] += tolerance + tolerance;
  return 1;
}

/*
================
TesselateCheckEarValidity

Validates ear: no other vertex inside ear triangle
================
*/
char TesselateCheckEarValidity( int axisX, int *vertArray, int earPrev, int earTip, int earNext, int axisY, char allowCoincident, double *sideBA, double *sideAC, double *sideCB)
{
  int n = *vertArray;
  float *pts = (float *)&vertArray[1];
  int firstScan = (earNext + 1) % n;
  int prev, cur, next;

  #define PT(i) (&pts[3 * (i)])

  for ( prev = earNext, cur = firstScan, next = (earNext + 2) % n;
        cur != earPrev;
        prev = cur, cur = next, next = (next + 1) % n )
  {
    float *v = PT(cur);
    double dBA = MulAdd2(v[axisY], sideBA[1], v[axisX], sideBA[0]) - sideBA[2];
    double dAC, dCB;

    if ( dBA < 0.0 || dBA > sideBA[TRISIDE_DIST] )
      continue;
    dAC = MulAdd2(v[axisY], sideAC[1], v[axisX], sideAC[0]) - sideAC[2];
    if ( dAC < 0.0 || dAC > sideAC[TRISIDE_DIST] )
      continue;
    dCB = MulAdd2(v[axisY], sideCB[1], v[axisX], sideCB[0]) - sideCB[2];
    if ( dCB < 0.0 || dCB > sideCB[TRISIDE_DIST] )
      continue;

    /* vertex is inside the ear triangle â€” check if it coincides with an ear vertex */
    if ( v[axisX] == pts[3 * earPrev + axisX] && v[axisY] == pts[3 * earPrev + axisY] )
    {
      if ( !TrisCheckBothEdges(sideAC, axisY, PT(prev), sideBA, PT(next), axisX, sideCB) )
        return 0;
      if ( !allowCoincident && TrisVerticesCoincident2D(earTip, axisX, vertArray, axisY, prev, next, (n + earPrev - 1) % n) )
        return 0;
    }
    else if ( v[axisX] == pts[3 * earTip + axisX] && v[axisY] == pts[3 * earTip + axisY] )
    {
      if ( !TrisCheckBothEdges(sideBA, axisY, PT(prev), sideCB, PT(next), axisX, sideAC) )
        return 0;
      if ( !allowCoincident && TrisVerticesCoincident2D(earNext, axisX, vertArray, axisY, prev, next, earPrev) )
        return 0;
    }
    else if ( v[axisX] == pts[3 * earNext + axisX] && v[axisY] == pts[3 * earNext + axisY]
           && TrisCheckBothEdges(sideCB, axisY, PT(prev), sideAC, PT(next), axisX, sideBA)
           && (allowCoincident || !TrisVerticesCoincident2D(firstScan, axisX, vertArray, axisY, prev, next, earTip)) )
    {
      /* coincident with earNext â€” OK */
    }
    else
    {
      return 0;
    }
  }
  return 1;

  #undef PT
}

/*
================
TrisLoadWindingBin

Loads test winding from winding.bin for tessellation debugging
================
*/
TriSurf_t *TrisLoadWindingBin()
{
  FILE *binFile;
  Winding_t *winding;
  Winding_t *origWinding;
  TriSurf_t *surf;
  TriSurfProps_t *props;
  size_t numPoints;

  binFile = fopen("winding.bin", "rb");
  if ( !binFile )
    return NULL;
  /* Native 0x45AC40 persists only the 16-byte plane carrier, not a raw
     compiler-layout TriSurfProps_t.  Keep this debug artifact independent
     of KIWI's expanded props structure. */
  props = calloc(1, sizeof(*props));
  fread_locked(props->plane, sizeof(props->plane), 1, binFile);
  fread_locked(&numPoints, sizeof(int), 1, binFile);
  winding = AllocWinding((int)numPoints);
  winding->numpoints = (int)numPoints;
  origWinding = AllocWinding((int)numPoints);
  origWinding->numpoints = (int)numPoints;
  fread_locked(winding->points, sizeof(vec3_t), numPoints, binFile);
  fread_locked(origWinding->points, sizeof(vec3_t), numPoints, binFile);
  fclose(binFile);
  surf = malloc(sizeof(TriSurf_t));
  memset(surf, 0, sizeof(TriSurf_t));
  surf->winding = winding;
  surf->props = props;
  surf->origWinding = origWinding;
  return surf;
}

/* Native 0x45AD90.  This is deliberately a narrow diagnostic format: the
   tessellator only needs the surface plane plus paired winding points to
   replay a failure through TrisLoadWindingBin. */
static void DumpTessellationWindingBin(const TriSurf_t *ts)
{
  FILE *binFile = fopen("winding.bin", "wb");

  if (!binFile)
    return;
  fwrite(ts->props->plane, sizeof(ts->props->plane), 1, binFile);
  fwrite(&ts->winding->numpoints, sizeof(ts->winding->numpoints), 1, binFile);
  fwrite(ts->winding->points, sizeof(vec3_t), ts->winding->numpoints, binFile);
  fwrite(ts->origWinding->points, sizeof(vec3_t), ts->winding->numpoints, binFile);
  fclose(binFile);
}

/* Native 0x45AD70. */
void TesselateWindingFailure(const TriSurf_t *ts)
{
  DumpTessellationWindingBin(ts);
  Com_Error("code bug: triangulation failed!\n"
            "Include 'winding.bin' from the cod2map directory in your bug report.\n");
}

/*
================
TesselateInsertVertices

Inserts split vertices into winding at edge positions
================
*/
char **TesselateInsertVertices( char *splitPoints, int numSplits, Winding_t **w, Winding_t **wOrig, void **auxData, int auxElemSize )
{
  tessSplitPoint_t *splits = (tessSplitPoint_t *)splitPoints;
  Winding_t *srcW = *w;
  Winding_t *srcWOrig = wOrig ? *wOrig : NULL;
  Winding_t *newW, *newWOrig;
  void *newAux;
  int numPts, src, dst, si, splitVert;

  Assert( w, s_assertDisable_TesselateCheckEarValidity );
  Assert( *w, s_assertDisable_TesselateCheckEarValidity );
  Assert( !wOrig || *wOrig, s_assertDisable_TesselateCheckEarValidity );
  Assert( !auxElemSize || (auxData && *auxData), s_assertDisable_TesselateCheckEarValidity );

  g_totalSplits += numSplits;
  numPts = srcW->numpoints;
  if ( numSplits > 1 )
    qsort( splits, numSplits, sizeof(tessSplitPoint_t), (int (*)(const void*, const void*))TessSplitPointCompare );

  newW = AllocWinding( numSplits + numPts );
  newWOrig = srcWOrig ? AllocWinding( numSplits + numPts ) : NULL;
  newAux = auxElemSize ? malloc( auxElemSize * (numSplits + numPts) ) : 0;

  /* walk source vertices, inserting split points at edge boundaries */
  dst = 0;
  si = 0;
  for ( src = 0; src < numPts; src++ )
  {
    /* copy source vertex */
    VectorCopy( srcW->points[src], newW->points[dst] );
    if ( srcWOrig )
      VectorCopy( srcWOrig->points[src], newWOrig->points[dst] );
    if ( auxElemSize )
      AuxDataCopy( auxElemSize, (char *)*auxData, src, 1, (char *)newAux, dst );
    dst++;

    /* insert any split points belonging to this edge */
    while ( si < numSplits && splits[si].edgeIdx == src )
    {
      /* skip near-duplicate splits on the same edge */
      if ( !si || splits[si - 1].edgeIdx != src || splits[si].position - splits[si - 1].position > PLANESIDE_EPSILON )
      {
        splitVert = splits[si].vertIdx;
        VectorCopy( srcW->points[splitVert], newW->points[dst] );
        if ( srcWOrig )
          VectorCopy( srcWOrig->points[splitVert], newWOrig->points[dst] );
        if ( auxElemSize )
          AuxDataCopy( auxElemSize, (char *)splits[si].auxData, 0, 1, (char *)newAux, dst );
        dst++;
      }
      if ( auxElemSize )
        free( (void *)splits[si].auxData );
      si++;
    }
  }

  newW->numpoints = dst;
  if ( newWOrig )
    newWOrig->numpoints = dst;
  FreeWinding( srcW );
  if ( srcWOrig )
    FreeWinding( srcWOrig );
  if ( auxElemSize )
    free( *auxData );
  *w = newW;
  if ( wOrig )
    *wOrig = newWOrig;
  if ( auxData )
    *auxData = newAux;
  return (void *)wOrig;
}

/*
================
TesselateFindSplitCandidates

Finds vertices close to edges, generates split points for T-junction fixing
================
*/
/* Native 0x45B3D0.  Measure how far the candidate vertex lies from the chord
   through its two winding neighbours in the same 2D projection.  0x45AEC0
   rejects a remote-edge split whose distance is more than twice this local
   corner distance; without that gate every nearby collinear vertex can be
   inserted into an unrelated edge. */
static float TesselateSplitCandidateNeighborDistance(const Winding_t *w,
  int testIndex, int axisX, int axisY)
{
  int previousIndex = testIndex ? testIndex - 1 : w->numpoints - 1;
  int nextIndex = testIndex + 1 == w->numpoints ? 0 : testIndex + 1;
  float direction[2];
  float perpendicularOffset;
  float perpendicularDistance;

  direction[0] = w->points[nextIndex][axisX] - w->points[previousIndex][axisX];
  direction[1] = w->points[nextIndex][axisY] - w->points[previousIndex][axisY];
  Vec2Normalize(direction);
  perpendicularOffset = direction[1] * w->points[previousIndex][axisX]
                      - direction[0] * w->points[previousIndex][axisY];
  perpendicularDistance = direction[1] * w->points[testIndex][axisX]
                        - direction[0] * w->points[testIndex][axisY]
                        - perpendicularOffset;
  return fabsf(perpendicularDistance);
}

void *TesselateFindSplitCandidates( float *surfNormal, Winding_t **wPtr, Winding_t **wOrigPtr, void **auxDataPtr, unsigned int auxElemSize )
{
  Winding_t *w;
  float *pts;
  int numPts, axisX, axisY, edge, prev, inner, testIdx;
  size_t numSplits;
#ifdef _WIN64
  double edgeLen, proj;  /* x87 keeps these at 53-bit in FPU registers */
#else
  float edgeLen, proj;
#endif
  double edgeProjOfs, edgePerpOfs;
  double perpDist, projDist;
  float candidatePerpDistance;
  tessSplitPoint_t *sp;
  char splitBuf[MAX_SPLIT_BUF_SIZE];
  volatile float edgeDir[2];

  #define TESS_PERP_THRESHOLD   0.1225000023841858f
  #define TESS_EDGE_MARGIN      0.061250001

  GetProjectionAxes( surfNormal, &axisX, &axisY );
  w = *wPtr;
  pts = (float *)w->points;
  numPts = w->numpoints;
  numSplits = 0;

  prev = numPts - 1;
  for ( edge = 0; edge < numPts; edge++ )
  {
    edgeDir[0] = pts[3 * edge + axisX] - pts[3 * prev + axisX];
    edgeDir[1] = pts[3 * edge + axisY] - pts[3 * prev + axisY];
    edgeLen = Vec2Normalize( (float *)edgeDir );

    edgeProjOfs = MulAdd2(edgeDir[0], pts[3 * prev + axisX], edgeDir[1], pts[3 * prev + axisY]);
    edgePerpOfs = Det2x2(edgeDir[1], pts[3 * prev + axisX], edgeDir[0], pts[3 * prev + axisY]);

    w = *wPtr;
    pts = (float *)w->points;
    numPts = w->numpoints;
    for ( inner = 1; inner < numPts - 1; inner++ )
    {
      testIdx = (edge + inner) % numPts;

      perpDist = Det2x2(edgeDir[1], pts[3 * testIdx + axisX], edgeDir[0], pts[3 * testIdx + axisY]) - edgePerpOfs;
      candidatePerpDistance = fabsf((float)perpDist);
      if ( candidatePerpDistance >= TESS_PERP_THRESHOLD )
        continue;

      projDist = MulAdd2(edgeDir[0], pts[3 * testIdx + axisX], edgeDir[1], pts[3 * testIdx + axisY]) - edgeProjOfs;
      proj = projDist;
      if ( projDist <= TESS_EDGE_MARGIN || (double)edgeLen - TESS_EDGE_MARGIN <= (double)proj )
        continue;
      if ( candidatePerpDistance > 2.0f * TesselateSplitCandidateNeighborDistance(
             w, testIdx, axisX, axisY) )
        continue;

      sp = (tessSplitPoint_t *)&splitBuf[sizeof(tessSplitPoint_t) * numSplits];
      sp->edgeIdx = prev;
      sp->position = proj;
      sp->vertIdx = testIdx;
      if ( auxElemSize )
      {
        char *auxBase;

        Assert(auxDataPtr && *auxDataPtr, s_assertDisable_TesselateCheckEarValidity);
        auxBase = (char *)*auxDataPtr;
        sp->auxData = (intptr_t)malloc( auxElemSize );
        g_lerpAuxDataCallback(
            (float *)(auxBase + prev * auxElemSize),
            (float *)(auxBase + edge * auxElemSize),
            (double)projDist / (double)edgeLen,
            (float *)sp->auxData);
      }
      else
      {
        sp->auxData = 0;
      }
      numSplits++;
    }

    prev = edge;
  }

  if ( numSplits )
    return TesselateInsertVertices( splitBuf, (int)numSplits, wPtr, wOrigPtr, auxDataPtr, auxElemSize );
  return NULL;

  #undef TESS_PERP_THRESHOLD
  #undef TESS_EDGE_MARGIN
}

/*
================
TesselateRemoveDegenerateEdges

Removes zero-length edges from winding
================
*/
int TesselateRemoveDegenerateEdges( Winding_t *w, Winding_t *wOrig, char *auxData, int auxElemSize )
{
  int pp, prev, cur, n;

  /* repeatedly scan for double-back vertices: points[pp] == points[cur] */
  for ( ;; )
  {
    n = w->numpoints;
    pp = n - 2;
    prev = n - 1;
    for ( cur = 0; cur < n; cur++ )
    {
      if ( w->points[pp][0] == w->points[cur][0]
        && w->points[pp][1] == w->points[cur][1]
        && w->points[pp][2] == w->points[cur][2] )
      {
        /* found degenerate: remove pp and prev vertices */
        w->numpoints -= 2;
        if ( wOrig )
          wOrig->numpoints -= 2;
        tesselateDegenerateCount++;
        if ( w->numpoints < 3 )
        {
          tesselateDegenerateCount++;
          return w->numpoints;
        }

        /* shift remaining vertices to close the gap */
        if ( prev != w->numpoints && pp != w->numpoints )
        {
          if ( prev )
          {
            Assert( w->numpoints - cur + 2 > 0, s_assertDisable_TesselateRemoveDegenerateEdges );
            Assert( pp < cur, s_assertDisable_TesselateRemoveDegenerateEdges );
            memmove( w->points[pp], w->points[cur], sizeof(vec3_t) * (w->numpoints - pp) );
            if ( wOrig )
              memmove( wOrig->points[pp], wOrig->points[cur], sizeof(vec3_t) * (wOrig->numpoints - pp) );
            if ( auxElemSize )
              memmove( &auxData[auxElemSize * pp], &auxData[auxElemSize * (pp + 2)], auxElemSize * (w->numpoints - pp) );
          }
          else
          {
            memmove( w->points[0], w->points[2], sizeof(vec3_t) * w->numpoints );
            if ( wOrig )
              memmove( wOrig->points[0], wOrig->points[2], sizeof(vec3_t) * wOrig->numpoints );
            if ( auxElemSize )
              memmove( auxData, &auxData[2 * auxElemSize], auxElemSize * w->numpoints );
          }
        }
        break;  /* restart scan */
      }
      pp = prev;
      prev = cur;
    }

    if ( cur >= n )
      return n;  /* no degenerate found â€” done */
  }
}

/* Native 0x45AE40.  COD4 keeps the original-winding channel only in
   TRIS_TRANSIENT_MAIN (mode 1); the lightmap and merge modes preprocess the
   surface winding alone.  KIWI's auxiliary payload is an integration
   sidecar, so it follows the same insert/remove operations when present. */
int PreprocessLmapWinding( TriSurf_t *surf )
{
  Winding_t **origWinding;

  Assert(surf, s_assertDisable_TesselateCheckEarValidity);
  Assert(surf->winding, s_assertDisable_TesselateCheckEarValidity);
  Assert(surf->props, s_assertDisable_TesselateCheckEarValidity);
  if (surf->winding->numpoints == 3)
    return 3;

  origWinding = NULL;
  if (GetTrisTransientMode() == 1)
  {
    Assert(surf->origWinding, s_assertDisable_TesselateCheckEarValidity);
    origWinding = &surf->origWinding;
  }
  TesselateFindSplitCandidates(surf->props->plane, &surf->winding, origWinding,
                               surf->auxElemSize ? &surf->auxData : NULL,
                               surf->auxElemSize);
  return TesselateRemoveDegenerateEdges(surf->winding,
                                        origWinding ? *origWinding : NULL,
                                        (char *)surf->auxData,
                                        surf->auxElemSize);
}

/*
================
TesselateEdgesIntersect

2D edge-edge intersection test with hit point computation
================
*/
int TesselateEdgesIntersect( float *edgeA0, float *edgeA1, float *edgeB0, int axisX, float *edgeB1, int axisY, float *hitPoint, float *tA, float *tB )
{
  vec3_t dir;
  double crossA0, crossA1, crossB0, crossB1;
  double ta, tb;
#ifdef _WIN64
  double hitBx, hitBy, hitBz;
#else
  float hitBx, hitBy, hitBz;
#endif

  /* test A endpoints against edge B */
  VectorSubtract( edgeB1, edgeB0, dir );
  crossA0 = Det2x2((double)edgeA0[axisX] - edgeB0[axisX], dir[axisY], (double)edgeA0[axisY] - edgeB0[axisY], dir[axisX]);
  crossA1 = Det2x2((double)edgeA1[axisX] - edgeB0[axisX], dir[axisY], (double)edgeA1[axisY] - edgeB0[axisY], dir[axisX]);
  if ( crossA0 <= -PLANESIDE_EPSILON && crossA1 <= -PLANESIDE_EPSILON )
    return 0;
  if ( crossA0 >= PLANESIDE_EPSILON && crossA1 >= PLANESIDE_EPSILON )
    return 0;

  /* test B endpoints against edge A */
  VectorSubtract( edgeA1, edgeA0, dir );
  crossB0 = Det2x2((double)edgeB0[axisX] - edgeA0[axisX], dir[axisY], (double)edgeB0[axisY] - edgeA0[axisY], dir[axisX]);
  crossB1 = Det2x2((double)edgeB1[axisX] - edgeA0[axisX], dir[axisY], (double)edgeB1[axisY] - edgeA0[axisY], dir[axisX]);
  if ( crossB0 <= -PLANESIDE_EPSILON && crossB1 <= -PLANESIDE_EPSILON )
    return 0;
  if ( crossB0 >= PLANESIDE_EPSILON && crossB1 >= PLANESIDE_EPSILON )
    return 0;

  /* all four cross products near zero â€” edges are collinear */
  if ( crossA0 * crossA0 < COLLINEAR_EPSILON_SQ && crossA1 * crossA1 < COLLINEAR_EPSILON_SQ
    && crossB0 * crossB0 < COLLINEAR_EPSILON_SQ && crossB1 * crossB1 < COLLINEAR_EPSILON_SQ )
    return 2;

  /* compute intersection parameters and hit point (average of both) */
  /* 0x45BF40 returns the first fraction on (edgeA0,edgeA1), then the
     second on (edgeB0,edgeB1).  0x45BC80 uses those fractions to choose
     the respective snap endpoints. */
  *tA = DIVS(crossA0, crossA0, crossA1);
  *tB = DIVS(crossB0, crossB0, crossB1);
  ta = *tA;
  tb = *tB;
  hitBx = FMA1(edgeB0[0], edgeB1[0] - edgeB0[0], tb);
  hitBy = FMA1(edgeB0[1], edgeB1[1] - edgeB0[1], tb);
  hitBz = FMA1(edgeB0[2], tb, edgeB1[2] - edgeB0[2]);
  hitPoint[0] = MIDF(FMA1(edgeA0[0], edgeA1[0] - edgeA0[0], ta), hitBx);
  hitPoint[1] = MIDF(FMA1(edgeA0[1], edgeA1[1] - edgeA0[1], ta), hitBy);
  hitPoint[2] = MIDF(FMA1(edgeA0[2], edgeA1[2] - edgeA0[2], ta), hitBz);
  return 1;
}

/*
================
TesselateFixIntersections

Fixes self-intersecting edges by snapping to intersection point
================
*/
char TesselateFixIntersections( TriSurf_t *ts, int axisX, int axisY, float tolerance )
{
  Winding_t *w;
  int numPts, prev, cur, edgeA, edgeB, snapA, snapB, t;
#ifdef _WIN64
  float toleranceSq, hitTA, hitTB;
  double dA, dB;
#else
  float toleranceSq, hitTA, hitTB, dA, dB;
#endif
  float hitPoint[3];
  float *snap;
  char modified;

  Assert( ts, s_assertDisable_TesselateFixIntersections );
  Assert( ts->winding, s_assertDisable_TesselateFixIntersections );
  Assert( GetTrisTransientMode() == 1, s_assertDisable_TesselateFixIntersections );

  w = ts->winding;
  numPts = w->numpoints;
  toleranceSq = tolerance * tolerance;
  modified = 0;

  if ( numPts <= 0 )
    return 0;

  /* for each edge (prevâ†’cur), test against non-adjacent edges for intersection */
  prev = numPts - 1;
  for ( cur = 0; cur < numPts; cur++ )
  {
    for ( t = 2; t < numPts - 2; t++ )
    {
      edgeA = (prev + t) % numPts;
      edgeB = (edgeA + 1) % numPts;
      if ( TesselateEdgesIntersect( w->points[prev], w->points[cur], w->points[edgeA], axisX, w->points[edgeB], axisY, hitPoint, &hitTA, &hitTB ) != 1 )
      {
        w = ts->winding;
        numPts = w->numpoints;
        continue;
      }

      /* pick snap vertex: closest endpoint on each edge */
      snapA = (hitTA >= 0.5) ? cur : prev;
      dA = DistanceSquared( w->points[snapA], hitPoint );
      if ( dA < toleranceSq )
        goto next_test;

      snapB = (hitTB >= 0.5) ? edgeB : edgeA;
      dB = DistanceSquared( w->points[snapB], hitPoint );
      if ( dB < toleranceSq )
        goto next_test;

      /* snap the closer vertex to the hit point */
      modified = 1;
      snap = w->points[(dB < dA) ? snapB : snapA];
      VectorCopy( hitPoint, snap );

    next_test:
      w = ts->winding;
      numPts = w->numpoints;
    }
    prev = cur;
  }

  if ( !modified )
    return 0;

  TesselateFindSplitCandidates( ts->props->plane, &ts->winding, &ts->origWinding, &ts->auxData, ts->auxElemSize );
  return 1;
}

/*
================
TesselateRemoveVertex

Removes a vertex from winding and original winding
================
*/
int TesselateRemoveVertex( int vertIdx, TriSurf_t *ts )
{
  Winding_t *w, *wOrig;
  int idx = vertIdx;
  int remaining;

  Assert( GetTrisTransientMode() == 1, s_assertDisable_TesselateRemoveVertex );

  w = ts->winding;
  wOrig = ts->origWinding;
  w->numpoints--;
  wOrig->numpoints--;

  if ( idx == w->numpoints )
    return w->numpoints;

  /* shift vertices left to fill the gap */
  remaining = w->numpoints - idx;
  memmove( w->points[idx], w->points[idx + 1], sizeof(vec3_t) * remaining );
  memmove( wOrig->points[idx], wOrig->points[idx + 1], sizeof(vec3_t) * remaining );
  if ( ts->auxElemSize )
    memmove( (char *)ts->auxData + idx * ts->auxElemSize,
             (char *)ts->auxData + (idx + 1) * ts->auxElemSize,
             ts->auxElemSize * remaining );

  return w->numpoints;
}

/*
================
TesselateClipEar

Clips best ear from polygon: scan all control passes, emit triangle, remove vertex
================
*/
void TesselateClipEar( TriSurf_t *ts, int cellIndex, int cullGroupIndex, int axisX, int axisY, void (*triCallback)(TriSurf_t *, int, int, int, int, int) )
{
  Winding_t *w;
  int numPts, bestEar, pass, prev, ear, after, beforeClip, afterClip, prevClip, checkIdx;
  double sideBA[TRISIDE_ELEMENTS], sideAC[TRISIDE_ELEMENTS], sideCB[TRISIDE_ELEMENTS];
  double earCos, bestCos;
  float tolerance;
  int allowCoinc;

  /* find best ear across all control passes; retry after fixing intersections */
  for ( ;; )
  {
    Assert( GetTrisTransientMode() == 1, s_assertDisable_TesselateClipEar );
    w = ts->winding;
    numPts = w->numpoints;
    bestEar = -1;
    bestCos = FLT_MAX;

    for ( pass = 0; pass < NUM_TESS_CLIP_EAR_PASSES && bestEar == -1; pass++ )
    {
      AssertFatal( g_tessClipEarTable[2 * pass] < 2, s_assertDisable_TesselateClipEar );
      allowCoinc = g_tessClipEarTable[2 * pass] == 1;
      tolerance = *(float *)&g_tessClipEarTable[2 * pass + 1];

      /* scan all vertices as ear candidates: prevâ†’earâ†’after */
      prev = numPts - 2;
      ear = numPts - 1;
      for ( after = 0; after < numPts; after++ )
      {
        if ( TrisSetupTrianglePlanes( after, axisX, axisY, sideBA, (int *)w, prev, ear, tolerance, sideAC, sideCB ) )
        {
          earCos = TrisLargestAngleCosine( sideBA[TRISIDE_LENGTH], sideAC[TRISIDE_LENGTH], sideCB[TRISIDE_LENGTH] );
          if ( earCos < bestCos
            && TesselateCheckEarValidity( axisX, (int *)w, prev, ear, after, axisY, allowCoinc, sideBA, sideAC, sideCB ) )
          {
            bestCos = earCos;
            bestEar = ear;
          }
        }
        prev = ear;
        ear = after;
      }
    }

    if ( bestEar >= 0 )
      break;
    if ( !TesselateFixIntersections( ts, axisX, axisY, TESS_INTERSECT_EPSILON )
      && !TesselateFixIntersections( ts, axisX, axisY, TESS_INTERSECT_EPSILON_FINE )
      && !TesselateFixIntersections( ts, axisX, axisY, PLANESIDE_EPSILON ) )
    {
      TesselateWindingFailure(ts);
      break;
    }
  }

  /* emit the ear triangle and remove the ear vertex */
  beforeClip = (numPts + bestEar - 1) % numPts;
  afterClip = (bestEar + 1) % numPts;
  triCallback( ts, beforeClip, bestEar, afterClip, cellIndex, cullGroupIndex );
  TesselateRemoveVertex( bestEar, ts );

  /* adjust indices after vertex removal */
  numPts = w->numpoints;
  if ( bestEar != numPts )
  {
    afterClip--;
    if ( !bestEar )
      beforeClip--;
  }

  /* check if clipping created degenerate edges */
  prevClip = (numPts + beforeClip - 1) % numPts;
  checkIdx = (afterClip + 1) % numPts;
  if ( (w->points[beforeClip][0] == w->points[checkIdx][0]
     && w->points[beforeClip][1] == w->points[checkIdx][1]
     && w->points[beforeClip][2] == w->points[checkIdx][2])
    || (w->points[prevClip][0] == w->points[afterClip][0]
     && w->points[prevClip][1] == w->points[afterClip][1]
     && w->points[prevClip][2] == w->points[afterClip][2]) )
  {
    TesselateRemoveDegenerateEdges( w, ts->origWinding, (char *)ts->auxData, ts->auxElemSize );
  }
}

/*
================
TesselateWinding

Main tessellation entry: split, degenerate removal, ear-clip loop
================
*/
int TesselateWinding( TriSurf_t *ts, int cellIndex, int cullGroupIndex, void (*triCallback)(TriSurf_t *, int, int, int, int, int) )
{
  Winding_t *w;
  float normal[3];
  float dist;
  int axisX, axisY;

  if ( !((cellIndex == TESS_CELL_SHADOW && cullGroupIndex >= 0) || (cellIndex >= 0 && cullGroupIndex == TESS_CULLGROUP_NONE)) )
    Assert( 0, s_assertDisable_TesselateWinding );
  Assert( GetTrisTransientMode() == 1, s_assertDisable_TesselateWinding );

  /* trivial case: already a triangle */
  w = ts->winding;
  if ( w->numpoints == 3 )
  {
    triCallback( ts, 0, 1, 2, cellIndex, cullGroupIndex );
    return w->numpoints;
  }

  /* insert split points and remove degenerate edges */
  TesselateFindSplitCandidates( ts->props->plane, &ts->winding, &ts->origWinding, &ts->auxData, ts->auxElemSize );
  w = ts->winding;
  TesselateRemoveDegenerateEdges( w, ts->origWinding, (char *)ts->auxData, ts->auxElemSize );

  if ( w->numpoints < 3 )
    return w->numpoints;

  /* compute winding plane and projection axes */
  if ( !PlaneFromWinding( w->numpoints, w->points[0], normal, &dist ) )
    return w->numpoints;

  GetProjectionAxes( normal, &axisX, &axisY );

  /* ear-clip until only a triangle remains */
  while ( w->numpoints > 3 )
    TesselateClipEar( ts, cellIndex, cullGroupIndex, axisX, axisY, triCallback );

  if ( w->numpoints == 3 )
  {
    triCallback( ts, 0, 1, 2, cellIndex, cullGroupIndex );
    return w->numpoints;
  }

  return w->numpoints;
}

/*
================
TrisLmap_TesselateWinding

Mode-4 lightmap callback tessellator (0x45B6F0/0x45B940).  Unlike the
mode-1 route above, the native code retains the source winding and clips an
index worklist.  That matters because a lightmap raster callback needs source
vertex indices while the main surface must remain available for later passes.
================
*/
typedef struct TrisLmapTessWork_s {
  Winding_t *w;
  Winding_t *wOrig;
  int        indices[MAX_POINTS_ON_WINDING];
  int        count;
  int        ownsWinding;
} TrisLmapTessWork_t;

static void TrisLmap_TessFreeWork(TrisLmapTessWork_t *work)
{
  if (work->ownsWinding)
  {
    FreeWinding(work->w);
    if (work->wOrig)
      FreeWinding(work->wOrig);
  }
}

static Winding_t *TrisLmap_TessBuildOrderedWindingFrom(const TrisLmapTessWork_t *work,
                                                        const Winding_t *source)
{
  Winding_t *ordered;
  int i;

  Assert(source, s_assertDisable_TesselateWinding);
  ordered = AllocWinding(work->count);
  ordered->numpoints = work->count;
  for (i = 0; i < work->count; ++i)
    VectorCopy(source->points[work->indices[i]], ordered->points[i]);
  return ordered;
}

static Winding_t *TrisLmap_TessBuildOrderedWinding(const TrisLmapTessWork_t *work)
{
  return TrisLmap_TessBuildOrderedWindingFrom(work, work->w);
}

/* 0x45CC50, expressed against the native work index list. */
static int TrisLmap_TessRemoveDegenerateEdges(TrisLmapTessWork_t *work)
{
  int before;
  int previous;
  int current;
  int pointCount;

  for (;;)
  {
    pointCount = work->count;
    before = pointCount - 2;
    previous = pointCount - 1;
    for (current = 0; current < pointCount; ++current)
    {
      if (VectorCompare(work->w->points[work->indices[before]], work->w->points[work->indices[current]]))
      {
        work->count -= 2;
        ++tesselateDegenerateCount;
        if (work->count < 3)
        {
          ++tesselateDegenerateCount;
          return work->count;
        }

        if (previous != work->count && before != work->count)
        {
          if (previous)
          {
            Assert(work->count + 2 - current > 0, s_assertDisable_TesselateRemoveDegenerateEdges);
            Assert(before < current, s_assertDisable_TesselateRemoveDegenerateEdges);
            /* Native 0x45CC50 uses the CRT memcpy entry for this forward
               compaction. */
            memcpy(&work->indices[before], &work->indices[current], sizeof(work->indices[0]) * (work->count - before));
          }
          else
          {
            memcpy(work->indices, &work->indices[2], sizeof(work->indices[0]) * work->count);
          }
        }
        break;
      }
      before = previous;
      previous = current;
    }
    if (current == pointCount)
      return work->count;
  }
}

/* Native 0x45BC80 makes an isolated winding before this step.  Keep the
   same isolation so a malformed lightmap surface cannot alter the mode-4
   source surface. */
static int TrisLmap_TessFixIntersections(TrisLmapTessWork_t *work, TriSurfProps_t *props, int axisX, int axisY, float tolerance)
{
  Winding_t *ordered;
  Winding_t *orderedOrig;
  int prev;
  int current;
  int step;
  int edgeA;
  int edgeB;
  int snapA;
  int snapB;
  int modified;
  float hitPoint[3];
  float hitTA;
  float hitTB;
  float toleranceSq;

  /* 0x45B940 copies the complete backing winding, not the clipped index
     worklist.  The live index list and ptCount remain unchanged if repair
     replaces that backing winding. */
  ordered = CopyWinding(work->w);
  toleranceSq = tolerance * tolerance;
  modified = 0;
  prev = ordered->numpoints - 1;
  for (current = 0; current < ordered->numpoints; ++current)
  {
    for (step = 2; step < ordered->numpoints - 2; ++step)
    {
      edgeA = (prev + step) % ordered->numpoints;
      edgeB = (edgeA + 1) % ordered->numpoints;
      if (TesselateEdgesIntersect(ordered->points[prev], ordered->points[current], ordered->points[edgeA], axisX,
                                  ordered->points[edgeB], axisY, hitPoint, &hitTA, &hitTB) != 1)
        continue;
      snapA = hitTA >= 0.5f ? current : prev;
      if (DistanceSquared(ordered->points[snapA], hitPoint) < toleranceSq)
        continue;
      snapB = hitTB >= 0.5f ? edgeB : edgeA;
      if (DistanceSquared(ordered->points[snapB], hitPoint) < toleranceSq)
        continue;
      modified = 1;
      VectorCopy(hitPoint, ordered->points[DistanceSquared(ordered->points[snapB], hitPoint) < DistanceSquared(ordered->points[snapA], hitPoint) ? snapB : snapA]);
    }
    prev = current;
  }

  if (!modified)
  {
    FreeWinding(ordered);
    return 0;
  }

  orderedOrig = work->wOrig ? CopyWinding(work->wOrig) : NULL;
  TesselateFindSplitCandidates(props->plane, &ordered,
                               orderedOrig ? &orderedOrig : NULL,
                               NULL, 0);
  TrisLmap_TessFreeWork(work);
  work->w = ordered;
  work->wOrig = orderedOrig;
  work->ownsWinding = 1;
  TesselateRemoveDegenerateEdges(ordered, orderedOrig, NULL, 0);
  return 1;
}

/* 0x45B940: selects the lowest-largest-angle valid ear in native control
   pass order.  The point winding is rebuilt only for the generic geometric
   predicates; emitted indices are always the worklist's native indices. */
static int TrisLmap_TessClipEar(TrisLmapTessWork_t *work, TriSurfProps_t *props, int axisX, int axisY,
                                int visGroupIndex, TrisLmapTessCallback_t triCallback, void *userData)
{
  Winding_t *ordered;
  double sideBA[TRISIDE_ELEMENTS];
  double sideAC[TRISIDE_ELEMENTS];
  double sideCB[TRISIDE_ELEMENTS];
  double bestCos;
  double earCos;
  int bestEar;
  int pass;
  int prev;
  int ear;
  int after;
  int allowCoincident;
  float tolerance;
  int beforeClip;
  int afterClip;
  int prevClip;
  int checkIdx;

  for (;;)
  {
    ordered = TrisLmap_TessBuildOrderedWinding(work);
    bestEar = -1;
    bestCos = FLT_MAX;
    for (pass = 0; pass < NUM_TESS_CLIP_EAR_PASSES && bestEar == -1; ++pass)
    {
      AssertFatal(g_tessClipEarTable[2 * pass] < 2, s_assertDisable_TesselateClipEar);
      allowCoincident = g_tessClipEarTable[2 * pass] == 1;
      tolerance = *(float *)&g_tessClipEarTable[2 * pass + 1];
      prev = work->count - 2;
      ear = work->count - 1;
      for (after = 0; after < work->count; ++after)
      {
        if (TrisSetupTrianglePlanes(after, axisX, axisY, sideBA, (int *)ordered, prev, ear, tolerance, sideAC, sideCB))
        {
          earCos = TrisLargestAngleCosine(sideBA[TRISIDE_LENGTH], sideAC[TRISIDE_LENGTH], sideCB[TRISIDE_LENGTH]);
          if (earCos < bestCos && TesselateCheckEarValidity(axisX, (int *)ordered, prev, ear, after, axisY,
                                                             (char)allowCoincident, sideBA, sideAC, sideCB))
          {
            bestCos = earCos;
            bestEar = ear;
          }
        }
        prev = ear;
        ear = after;
      }
    }
    FreeWinding(ordered);
    if (bestEar >= 0)
      break;
    if (!TrisLmap_TessFixIntersections(work, props, axisX, axisY, TESS_INTERSECT_EPSILON)
     && !TrisLmap_TessFixIntersections(work, props, axisX, axisY, TESS_INTERSECT_EPSILON_FINE)
     && !TrisLmap_TessFixIntersections(work, props, axisX, axisY, PLANESIDE_EPSILON))
      return 0;
  }

  beforeClip = (work->count + bestEar - 1) % work->count;
  afterClip = (bestEar + 1) % work->count;
  triCallback(work->w, work->wOrig, props, work->indices[beforeClip], work->indices[bestEar], work->indices[afterClip],
              visGroupIndex, userData);
  --work->count;
  if (bestEar != work->count)
    /* 0x45CDF0 calls the CRT memcpy entry here.  Its source begins one
       element after the destination; retain the native operation rather
       than substituting a different overlap policy. */
    memcpy(&work->indices[bestEar], &work->indices[bestEar + 1], sizeof(work->indices[0]) * (work->count - bestEar));

  /* 0x45CAF0 only runs 0x45CC50 when clipping has exposed a coincident
     pair across the removed ear.  Removing every pre-existing degenerate
     pair here changes the native worklist (and therefore later callback
     indices) on complex windings. */
  if (bestEar != work->count)
  {
    --afterClip;
    if (!bestEar)
      --beforeClip;
  }
  prevClip = (beforeClip + work->count - 1) % work->count;
  checkIdx = (afterClip + 1) % work->count;
  if (VectorCompare(work->w->points[work->indices[beforeClip]],
                    work->w->points[work->indices[checkIdx]])
   || VectorCompare(work->w->points[work->indices[prevClip]],
                    work->w->points[work->indices[afterClip]]))
  {
    TrisLmap_TessRemoveDegenerateEdges(work);
  }
  return 1;
}

int TrisLmap_TesselateWinding(const TriSurf_t *surf, int visGroupIndex, TrisLmapTessCallback_t triCallback, void *userData)
{
  TrisLmapTessWork_t work;
  float normal[3];
  float dist;
  int axisX;
  int axisY;
  int i;
  int result;

  Assert(surf, s_assertDisable_TesselateWinding);
  Assert(surf->winding, s_assertDisable_TesselateWinding);
  Assert(surf->props, s_assertDisable_TesselateWinding);
  Assert(triCallback, s_assertDisable_TesselateWinding);
  Assert(visGroupIndex == TESS_CELL_SHADOW || visGroupIndex >= 0, s_assertDisable_TesselateWinding);
  Assert(surf->winding->numpoints <= MAX_POINTS_ON_WINDING, s_assertDisable_TesselateWinding);

  memset(&work, 0, sizeof(work));
  work.w = surf->winding;
  work.wOrig = GetTrisTransientMode() == 1 ? surf->origWinding : NULL;
  work.count = surf->winding->numpoints;
  if (work.count == 3)
  {
    triCallback(work.w, work.wOrig, surf->props, 0, 1, 2, visGroupIndex, userData);
    return 1;
  }
  /* Native callers run 0x45AE40 on the surface before entering 0x45B6F0.
     Intersection repair below is the only operation that may replace this
     local work winding and set ownsWinding. */
  for (i = 0; i < work.count; ++i)
    work.indices[i] = i;
  if (work.count < 3 || !PlaneFromWinding(work.count, work.w->points[0], normal, &dist))
  {
    TrisLmap_TessFreeWork(&work);
    return 1;
  }

  GetProjectionAxes(normal, &axisX, &axisY);
  result = 1;
  while (work.count > 3)
  {
    if (!TrisLmap_TessClipEar(&work, surf->props, axisX, axisY, visGroupIndex, triCallback, userData))
    {
      result = 0;
      break;
    }
  }
  if (result && work.count == 3)
    triCallback(work.w, work.wOrig, surf->props, work.indices[0], work.indices[1], work.indices[2], visGroupIndex, userData);
  TrisLmap_TessFreeWork(&work);
  return result;
}

/* Exact 0x45B6F0 entry used by Tris_TriangulateWindings at 0x443110.
   Unlike the lightmap preparation route, this entry consumes the surface's
   existing winding/original-winding pair without inserting split points. */
int TrisNative_TesselateWinding(const TriSurf_t *surf, int visGroupIndex,
  TrisLmapTessCallback_t triCallback, void *userData)
{
  TrisLmapTessWork_t work;
  float normal[3];
  float dist;
  int axisX;
  int axisY;
  int i;
  int result;

  Assert(surf, s_assertDisable_TesselateWinding);
  Assert(surf->winding, s_assertDisable_TesselateWinding);
  Assert(surf->props, s_assertDisable_TesselateWinding);
  Assert(triCallback, s_assertDisable_TesselateWinding);
  Assert(visGroupIndex == TESS_CELL_SHADOW || visGroupIndex >= 0,
    s_assertDisable_TesselateWinding);
  Assert(surf->winding->numpoints <= MAX_POINTS_ON_WINDING,
    s_assertDisable_TesselateWinding);

  memset(&work, 0, sizeof(work));
  work.w = surf->winding;
  /* 0x45B6F0 exposes the original winding to callbacks only while the
     transient tris mode is 1; other modes pass NULL. */
  work.wOrig = GetTrisTransientMode() == 1 ? surf->origWinding : NULL;
  work.count = surf->winding->numpoints;
  if (work.count == 3)
  {
    triCallback(work.w, work.wOrig, surf->props, 0, 1, 2,
      visGroupIndex, userData);
    return 1;
  }
  if (!PlaneFromWinding(work.count, work.w->points[0], normal, &dist))
    return 1;

  GetProjectionAxes(normal, &axisX, &axisY);
  for (i = 0; i < work.count; ++i)
    work.indices[i] = i;
  result = 1;
  while (work.count > 3)
  {
    if (!TrisLmap_TessClipEar(&work, surf->props, axisX, axisY,
      visGroupIndex, triCallback, userData))
    {
      result = 0;
      break;
    }
  }
  if (result && work.count == 3)
    triCallback(work.w, work.wOrig, surf->props, work.indices[0],
      work.indices[1], work.indices[2], visGroupIndex, userData);
  TrisLmap_TessFreeWork(&work);
  return result;
}


void TesselateNoopCallback(TriSurf_t *ts, int v0, int v1, int v2, int cellIndex, int cullGroupIndex) {}

/*
================
TrisTestWindingBin

Debug: loads winding.bin and runs tessellation test
================
*/
void TrisTestWindingBin()
{
  TriSurf_t *surf;

  surf = TrisLoadWindingBin();
  if ( surf )
  {
    SetTrisTransientMode(1, 1);
    TesselateWinding(surf, TESS_CELL_SHADOW, 0, TesselateNoopCallback);
    SetTrisTransientMode(0, 1);
  }
}

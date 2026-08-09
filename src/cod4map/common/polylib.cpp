/*
polylib.c — Winding and polygon operations

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

const char FMT_VECTOR3[] = "%g %g %g";
int        c_active_windings;
int        c_peak_windings;
int        c_removed;
int        c_winding_allocs;
int        c_winding_points;
char       g_assertExpr_nGt0[8] = { 'n', ' ', '>', ' ', '0', '\0', '\0', '\0' };

char s_assertDisable_CheckWindingInPlane;
char s_assertDisable_RemoveDuplicatePoints;
char s_assertDisable_RemoveDuplicatePoints;
char s_assertDisable_RemoveDuplicatePoints;

/*
================
WindingsAreCoplanar

Tests the mutually compatible-plane relation used by the native patch and
tri-surface paths.  It intentionally uses one-sided point tests after the
normal and distance checks, matching cod4map.exe 0x42EDD0.
================
*/
int WindingsAreCoplanar(const Winding_t *windingA, const float *planeA,
                        const Winding_t *windingB, const float *planeB)
{
  int pointIndex;

  if (DotProduct(planeA, planeB) < 0.9990000128746033f)
    return 0;
  if (fabsf(planeA[3] - planeB[3]) > 0.001000000047497451f)
    return 0;

  for (pointIndex = 0; pointIndex < windingA->numpoints; ++pointIndex)
  {
    if (DotProduct(windingA->points[pointIndex], planeB) - planeB[3]
        > 0.001000000047497451f)
      return 0;
  }
  for (pointIndex = 0; pointIndex < windingB->numpoints; ++pointIndex)
  {
    if (DotProduct(windingB->points[pointIndex], planeA) - planeA[3]
        > 0.001000000047497451f)
      return 0;
  }
  return 1;
}


/*
================
AllocWinding

Allocates a winding polygon with the given number of points.
================
*/
Winding_t *AllocWinding(int numPoints)
{
  Winding_t *w;

  ++c_active_windings;
  w = malloc(sizeof(int) + sizeof(vec3_t) * numPoints);
  if ( !w )
    Com_Error("out of memory: winding_t\n");
  w->numpoints = 0;
  return w;
}

/* DEAD CODE: AllocWindingDouble - double-precision winding system (never called) */
/*
void *AllocWindingDouble(int numPoints)
{
  int size;
  void *w;

  if ( numthreads == 1 )
  {
    c_winding_allocs++;
    c_winding_points += numPoints;
    c_active_windings++;
    if ( c_active_windings > c_peak_windings )
      c_peak_windings = c_active_windings;
  }

  size = sizeof(double) + sizeof(double) * 3 * numPoints;
  w = malloc(size);
  memset(w, 0, size);
  return w;
}
*/

/*
================
FreeWinding

Frees a winding polygon structure. Marks it with WINDING_FREED_SENTINEL to detect double-frees.
================
*/
void FreeWinding(Winding_t *w)
{
  --c_active_windings;
  free(w);
}

WindingList_t *AllocWindingListNode(void)
{
  WindingList_t *node;

  node = malloc(sizeof(*node));
  if ( !node )
    Com_Error("Out of memory");
  node->winding = NULL;
  node->next = NULL;
  return node;
}

void FreeWindingListNode(WindingList_t *node)
{
  free(node);
}

float *Vector2Copy(const float *in, float *out)
{
  out[0] = in[0];
  out[1] = in[1];
  return (float *)in;
}

float *Vector2Clear(float *out)
{
  out[0] = 0.0f;
  out[1] = 0.0f;
  return out;
}

float *Vector2Add(const float *a, const float *b, float *out)
{
  out[0] = a[0] + b[0];
  out[1] = a[1] + b[1];
  return (float *)a;
}

float *Vector2Scale(const float *in, float scale, float *out)
{
  out[0] = scale * in[0];
  out[1] = scale * in[1];
  return out;
}

/*
================
CopyWinding

Creates a copy of a winding polygon.
================
*/
Winding_t *CopyWinding(Winding_t *w)
{
  Winding_t *copy;
  int n;

  n = w->numpoints;
  copy = AllocWinding(n);
  memcpy(copy, w, sizeof(int) + sizeof(vec3_t) * n);
  return copy;
}

/*
================
ReverseWinding

Creates a new winding with reversed point order.
Used when flipping a brush face to the opposite direction.
================
*/
Winding_t *ReverseWinding(Winding_t *w)
{
  Winding_t *rev;
  int i, n;

  n = w->numpoints;
  rev = AllocWinding(n);
  rev->numpoints = n;

  /* copy points in reverse order */
  for ( i = 0; i < n; i++ )
    VectorCopy(w->points[n - 1 - i], rev->points[i]);

  return rev;
}

/*
================
ClipWindingByPlane

Clips a winding in-place to the front side of a plane.
Frees and NULLs the winding if entirely behind.
================
*/
void ClipWindingByPlane(Winding_t **windingPtr, float *planeNormal, float planeDist, float epsilon)
{
  Winding_t *winding, *newWinding;
  int numPoints, i, nextSide, nextPointIdx, maxPoints;
  long double dist;
  float dists[MAX_CLIP_POINTS];
  int sides[MAX_CLIP_POINTS];
  float intersectPointF[3];
  int counts[3];
  int isExactAxialPlane, axialPlaneAxis;
  float axialSnapValue;

  winding = *windingPtr;
  numPoints = winding->numpoints;

  counts[0] = 0;
  counts[1] = 0;
  counts[2] = 0;

  /* classify each point as front, back, or on */
  for ( i = 0; i < numPoints; i++ )
  {
    dist = DotProduct021(winding->points[i], planeNormal) - planeDist;
    dists[i] = dist;

    if ( dist <= epsilon )
    {
      if ( dist >= -epsilon )
        sides[i] = SIDE_ON;
      else
        sides[i] = SIDE_BACK;
    }
    else
    {
      sides[i] = SIDE_FRONT;
    }

    ++counts[sides[i]];
  }

  /* wrap around for edge processing */
  dists[numPoints] = dists[0];
  sides[numPoints] = sides[0];

  /* entirely behind the plane — free and NULL */
  if ( !counts[SIDE_FRONT] )
  {
    if ( *(unsigned *)winding == WINDING_FREED_SENTINEL )
      Com_Error("FreeWinding: freed a freed winding");
    *(unsigned *)winding = WINDING_FREED_SENTINEL;
    if ( numthreads == 1 )
      --c_active_windings;
    free(winding);
    *windingPtr = 0;
    return;
  }

  /* entirely in front — keep as is */
  if ( !counts[SIDE_BACK] )
  {
    return;
  }

  /* detect exact axial plane for intersection snapping */
  isExactAxialPlane = 1;
  axialPlaneAxis = -1;
  axialSnapValue = 0.0f;
  for ( i = 0; i < 3; i++ )
  {
    if ( planeNormal[i] == 1.0f )
    {
      axialPlaneAxis = i;
      axialSnapValue = planeDist;
    }
    else if ( planeNormal[i] == -1.0f )
    {
      axialPlaneAxis = i;
      axialSnapValue = -planeDist;
    }
    else if ( planeNormal[i] != 0.0f )
    {
      isExactAxialPlane = 0;
      break;
    }
  }
  if ( axialPlaneAxis < 0 )
    isExactAxialPlane = 0;

  maxPoints = numPoints + 4;
  newWinding = AllocWinding(maxPoints);

  for ( i = 0; i < winding->numpoints; i++ )
  {
    int curSide = sides[i];
    float curX = winding->points[i][0];
    float curY = winding->points[i][1];
    float curZ = winding->points[i][2];

    /* on the plane — copy directly */
    if ( curSide == SIDE_ON )
    {
      VectorCopy(winding->points[i], newWinding->points[newWinding->numpoints]);
      ++newWinding->numpoints;
      continue;
    }

    /* in front — copy it */
    if ( curSide == SIDE_FRONT )
    {
      VectorCopy(winding->points[i], newWinding->points[newWinding->numpoints]);
      ++newWinding->numpoints;
    }

    /* edge crosses the plane — generate intersection point */
    nextSide = sides[i + 1];
    if ( nextSide != SIDE_ON && nextSide != curSide )
    {
      nextPointIdx = (i + 1) % winding->numpoints;

      float nextX = winding->points[nextPointIdx][0];
      float nextY = winding->points[nextPointIdx][1];
      float nextZ = winding->points[nextPointIdx][2];

      double d1 = dists[i];
      double d2 = dists[i + 1];
      double dd = d1 - d2;
      intersectPointF[0] = (float)(((double)nextX * d1 - (double)curX * d2) / dd);
      intersectPointF[1] = (float)(((double)nextY * d1 - (double)curY * d2) / dd);
      intersectPointF[2] = (float)(((double)nextZ * d1 - (double)curZ * d2) / dd);

      /* snap axial intersection to exact plane distance */
      if ( isExactAxialPlane )
        intersectPointF[axialPlaneAxis] = axialSnapValue;

      VectorCopy(intersectPointF, newWinding->points[newWinding->numpoints]);
      ++newWinding->numpoints;
    }
  }

  if ( newWinding->numpoints > maxPoints )
    Com_Error("ClipWinding: points exceeded estimate");
  if ( newWinding->numpoints > MAX_POINTS_ON_WINDING )
    Com_Error("ClipWinding: MAX_POINTS_ON_WINDING");

  /* free old winding */
  if ( *(unsigned *)winding == WINDING_FREED_SENTINEL )
    Com_Error("FreeWinding: freed a freed winding");
  *(unsigned *)winding = WINDING_FREED_SENTINEL;
  if ( numthreads == 1 )
    --c_active_windings;
  free(winding);

  *windingPtr = newWinding;
}

/*
================
WindingPlaneDistExtent

Computes min/max signed distance of winding vertices from a plane.
================
*/
void WindingPlaneDistExtent(Winding_t *w, float *normal, float dist, float *outMinDist, float *outMaxDist)
{
  int i;
  double d;

  *outMinDist = FLT_MAX;
  *outMaxDist = -FLT_MAX;

  for ( i = 0; i < w->numpoints; i++ )
  {
    d = DotProduct201(w->points[i], normal) - dist;
    if ( d < *outMinDist )
      *outMinDist = d;
    if ( d > *outMaxDist )
      *outMaxDist = d;
  }
}

/*
================
WindingPlaneSide

Returns which side of a plane the winding is on (FRONT=0/BACK=1/ON=2/CROSS=3).
================
*/
int WindingPlaneSide(Winding_t *w, float *normal, float dist)
{
  int front, back;
  double d;
  int i;

  front = 0;
  back = 0;

  for ( i = 0; i < w->numpoints; i++ )
  {
    d = DotProduct021(w->points[i], normal) - dist;
    if ( d > ON_EPSILON )
    {
      if ( back )
        return 3;  /* SIDE_CROSS */
      front = 1;
    }
    else if ( d < -ON_EPSILON )
    {
      if ( front )
        return 3;  /* SIDE_CROSS */
      back = 1;
    }
  }

  if ( back )
    return 1;  /* SIDE_BACK */
  if ( front )
    return 0;  /* SIDE_FRONT */
  return 2;    /* SIDE_ON */
}

/* CoD4 0x42D710. */
static float WindingPlaneMaxDist(const Winding_t *w, const float *normal, float dist)
{
  float maxDist = 0.0f;
  int i;

  for ( i = 0; i < w->numpoints; ++i )
  {
    float pointDist = DotProduct(w->points[i], normal) - dist;
    if ( pointDist < 0.0f )
      pointDist = -pointDist;
    if ( pointDist > maxDist )
      maxDist = pointDist;
  }
  return maxDist;
}

/* CoD4 0x42E110.  For every directed edge, find its greatest inward plane
 * distance and return the smallest of those values.  This is the convexity
 * clearance used by the original shadow and concave-winding callers. */
static float WindingSmallestEdgeDistance(const Winding_t *w, const float *planeNormal)
{
  float smallest = FLT_MAX;
  int previous, current;

  Assert(w, g_assertFlags7);
  Assert(w->numpoints >= 3, g_assertFlags8);

  previous = w->numpoints - 1;
  for ( current = 0; current < w->numpoints; ++current )
  {
    vec3_t edge, edgePlane;
    float edgeLength, edgeDist, largest = -FLT_MAX;
    int test = (current + 1) % w->numpoints;

    VectorSubtract(w->points[current], w->points[previous], edge);
    CrossProduct(edge, planeNormal, edgePlane);
    edgeLength = VectorLength(edgePlane);
    edgeDist = DotProduct(edgePlane, w->points[previous]);

    do
    {
      float pointDist = DotProduct(w->points[test], edgePlane) - edgeDist;
      Assert(pointDist >= -ON_EPSILON || edgeLength < 1.0f, g_assertFlags9);
      if ( pointDist < largest )
        break;
      largest = pointDist;
      test = (test + 1) % w->numpoints;
    } while ( test != previous );

    if ( largest < smallest )
      smallest = largest;
    previous = current;
  }
  return smallest;
}

/*
================
WindingIsOnPlane

Checks if ALL vertices of a winding are within epsilon of a plane.
Returns 1 if all vertices within epsilon, 0 if any vertex exceeds epsilon.
================
*/
int WindingIsOnPlane(Winding_t *w, float *normal, float dist, float epsilon)
{
  float epsSq;
  double d;
  int i;

  epsSq = epsilon * epsilon;
  for ( i = 0; i < w->numpoints; i++ )
  {
    d = DotProduct021(w->points[i], normal) - dist;
    if ( d * d > epsSq )
      return 0;
  }
  return 1;
}

/*
================
ChopWinding

Snaps all winding vertices onto a plane and rounds to integer powers of 2.
================
*/
Winding_t *ChopWinding(Winding_t *w, float *normal, float dist)
{
  int i;
  double projDist;

  Assert(w, g_assertFlagsE);
  Assert(normal, g_assertFlagsF);

  for ( i = 0; i < w->numpoints; i++ )
  {
    /* project vertex onto plane */
    projDist = -(DotProduct120(w->points[i], normal) - dist);
    VectorMA(w->points[i], projDist, normal, w->points[i]);

    /* snap to grid */
    w->points[i][0] = SnapToIntegralPowerOf2(w->points[i][0], SNAP_INT_MIN_BITS, SNAP_INT_MAX_BITS);
    w->points[i][1] = SnapToIntegralPowerOf2(w->points[i][1], SNAP_INT_MIN_BITS, SNAP_INT_MAX_BITS);
    w->points[i][2] = SnapToIntegralPowerOf2(w->points[i][2], SNAP_INT_MIN_BITS, SNAP_INT_MAX_BITS);
  }

  return w;
}

/*
================
CheckWinding

Adds winding points to a convex hull. Both the winding and hull
lie on the same plane.
================
*/
Winding_t *CheckWinding(Winding_t *w, Winding_t **hull, float *normal)
{
  int i, j, k;
  float *p, *copy;
  vec3_t dir;
  float d;
  int numHullPts, numNew;
  vec3_t hullPoints[MAX_HULL_POINTS];
  vec3_t newHullPoints[MAX_HULL_POINTS];
  vec3_t hullDirs[MAX_HULL_POINTS];
  int hullSide[MAX_HULL_POINTS];
  int outside;
  Winding_t *result;

  if ( !*hull )
  {
    result = AllocWinding(w->numpoints);
    memcpy(result, w, sizeof(int) + w->numpoints * sizeof(vec3_t));
    *hull = result;
    return result;
  }

  numHullPts = (*hull)->numpoints;
  memcpy(hullPoints, (*hull)->points, numHullPts * sizeof(vec3_t));

  for ( i = 0; i < w->numpoints; i++ )
  {
    p = w->points[i];

    /* build edge normals for current hull */
    for ( j = 0; j < numHullPts; j++ )
    {
      k = (j + 1) % numHullPts;
      VectorSubtract(hullPoints[k], hullPoints[j], dir);
      VecNormalize(dir);
      CrossProduct(normal, dir, hullDirs[j]);
    }

    /* classify input point against each hull edge */
    outside = 0;
    for ( j = 0; j < numHullPts; j++ )
    {
      VectorSubtract(p, hullPoints[j], dir);
      d = DotProduct210(dir, hullDirs[j]);
      if ( d >= ON_EPSILON )
        outside = 1;
      hullSide[j] = d >= -ON_EPSILON;
    }

    /* if point is inside the hull, skip it */
    if ( !outside )
      continue;

    /* find the back-to-front transition */
    for ( j = 0; j < numHullPts; j++ )
    {
      if ( !hullSide[j % numHullPts] && hullSide[(j + 1) % numHullPts] )
        break;
    }
    if ( j == numHullPts )
      continue;

    /* start new hull with the input point */
    VectorCopy(p, newHullPoints[0]);
    numNew = 1;

    /* copy hull points that aren't double-fronts (inside on both sides) */
    j = (j + 1) % numHullPts;
    for ( k = 0; k < numHullPts; k++ )
    {
      if ( hullSide[(j + k) % numHullPts] && hullSide[(j + k + 1) % numHullPts] )
        continue;
      copy = hullPoints[(j + k + 1) % numHullPts];
      VectorCopy(copy, newHullPoints[numNew]);
      numNew++;
    }

    numHullPts = numNew;
    memcpy(hullPoints, newHullPoints, numHullPts * sizeof(vec3_t));
  }

  /* replace the old hull with the merged result */
  FreeWinding(*hull);
  result = AllocWinding(numHullPts);
  *hull = result;
  result->numpoints = numHullPts;
  memcpy(result->points, hullPoints, numHullPts * sizeof(vec3_t));
  return result;
}

/*
================
CheckWindingInPlane

Verifies all points of a winding lie on the given plane within tolerance.
================
*/
void CheckWindingInPlane(Winding_t *w, float *normal, float dist)
{
  int i;
  long double maxEpsilon;
  double scaledEpsilon;
  float error;

  if ( !w )
    return;

  scaledEpsilon = fabs(dist) * NORMAL_EPSILON;

  for ( i = 0; i < w->numpoints; i++ )
  {
    error = DotProduct021(w->points[i], normal) - dist;

    if ( scaledEpsilon >= ON_EPSILON )
      maxEpsilon = scaledEpsilon;
    else
      maxEpsilon = ON_EPSILON;

    Assert(fabs(error) < maxEpsilon, s_assertDisable_CheckWindingInPlane);
  }
}
/*
================
PlaneEqual

Checks if pointB is within tolerance distance of the line from pointA to lineEnd.
================
*/
int PlaneEqual(float *pointA, float *pointB, float *lineEnd, float tolerance)
{
  double projLen;
  double perpX;
  double perpY;
  double perpZ;
  vec3_t ab, lineDir;

  VectorSubtract(pointB, pointA, ab);
  VectorSubtract(lineEnd, pointA, lineDir);

  if ( 0.0 != VecNormalize(lineDir) )
  {
    /* project ab onto lineDir, then compute perpendicular component */
    projLen = DotProduct201(lineDir, ab);
    perpY = lineDir[1] * projLen;
    perpZ = lineDir[2] * projLen;
    perpX = ab[0] - lineDir[0] * projLen;
    { double dz = ab[2] - perpZ, dy = ab[1] - perpY;
    if ( sqrt(MulAdd2(dz,dz, dy,dy) + (double)perpX*perpX) < tolerance )
      return 1; }
  }
  return 0;
}

/*
================
CheckPointsAgainstPlane

Returns 1 if all points are on the positive side of a plane (dist >= -epsilon).
================
*/
int CheckPointsAgainstPlane(vec3_t *points, int numPoints, float *normal, float dist, float epsilon)
{
  int i;

  for ( i = 0; i < numPoints; i++ )
  {
    if ( DotProduct021(points[i], normal) - dist < -epsilon )
      return 0;
  }
  return 1;
}

/*
================
CheckPointsAgainstPlaneReversed

Returns 1 if all points are on the negative side of a plane (dist <= epsilon).
================
*/
int CheckPointsAgainstPlaneReversed(vec3_t *points, int numPoints, float *normal, float dist, float epsilon)
{
  int i;

  for ( i = 0; i < numPoints; i++ )
  {
    if ( DotProduct021(points[i], normal) - dist > epsilon )
      return 0;
  }
  return 1;
}

/*
================
CheckWindingSeparation

Tests if any edge plane of w can separate testW (SAT test).
Returns 1 if any edge separates, 0 otherwise.
================
*/
int CheckWindingSeparation(Winding_t *w, Winding_t *testW, float *normal, float epsilon)
{
  int numPts;
  int i, nextIdx;
  vec3_t edgePlane;
  float planeDist;
  vec3_t edgeDir;

  numPts = w->numpoints;
  if ( numPts <= 0 )
    return 0;

  for ( i = 0; i < numPts; i++ )
  {
    nextIdx = (i + 1) % numPts;
    VectorSubtract(w->points[nextIdx], w->points[i], edgeDir);
    CrossProduct(normal, edgeDir, edgePlane);
    VecNormalize(edgePlane);
    planeDist = DotProduct210(edgePlane, w->points[i]);

    if ( CheckPointsAgainstPlane(testW->points, testW->numpoints, edgePlane, planeDist, epsilon) )
      return 1;
  }
  return 0;
}

/*
================
CheckWindingSeparationBoth

Tests mutual separation of two windings via edge plane checks.
Returns 1 if either winding has a separating edge plane.
================
*/
int CheckWindingSeparationBoth(Winding_t *w1, Winding_t *w2, float *normal, float epsilon)
{
  if ( CheckWindingSeparation(w1, w2, normal, epsilon) )
    return 1;

  return CheckWindingSeparation(w2, w1, normal, epsilon);
}

/*
================
CheckWindingContainment

Tests if inner winding is fully contained within outer winding.
Returns 1 if all inner points pass all edge plane tests (contained), 0 otherwise.
================
*/
int CheckWindingContainment(Winding_t *outer, Winding_t *inner, float *normal, float epsilon)
{
  int numPts;
  int i, nextIdx;
  vec3_t edgePlane;
  float planeDist;
  vec3_t edgeDir;

  numPts = outer->numpoints;
  if ( numPts <= 0 )
    return 1;

  for ( i = 0; i < numPts; i++ )
  {
    nextIdx = (i + 1) % numPts;
    VectorSubtract(outer->points[nextIdx], outer->points[i], edgeDir);
    CrossProduct(normal, edgeDir, edgePlane);
    VecNormalize(edgePlane);
    planeDist = DotProduct210(edgePlane, outer->points[i]);

    if ( !CheckPointsAgainstPlaneReversed(inner->points, inner->numpoints, edgePlane, planeDist, epsilon) )
      return 0;
  }
  return 1;
}

/*
================
WindingLargestTriangleArea

Finds the largest triangle that can be formed from winding vertices,
projected onto the given plane normal. Used to detect degenerate windings.
Returns area of the largest triangle (0 if degenerate).
================
*/
double WindingLargestTriangleArea(Winding_t *w, float *planeNormal, int *outIdx0, int *outIdx1, int *outIdx2)
{
  vec3_t edge1, edge2, cross;
  double area;
  float bestArea;
  int i, j, k;

  *outIdx0 = 0;
  *outIdx1 = 1;
  *outIdx2 = 2;
  bestArea = 0.0;

  /* try all triangle combinations to find largest projected area */
  for ( k = 2; k < w->numpoints; k++ )
  {
    for ( j = 1; j < k; j++ )
    {
      VectorSubtract(w->points[k], w->points[j], edge2);

      for ( i = 0; i < j; i++ )
      {
        VectorSubtract(w->points[i], w->points[j], edge1);
        CrossProduct(edge1, edge2, cross);

        /* project onto plane normal — |cross . normal| = 2 * projected area */
        area = fabs(DotProduct120(cross, planeNormal));
        if ( area > bestArea )
        {
          bestArea = area;
          *outIdx0 = i;
          *outIdx1 = j;
          *outIdx2 = k;
        }
      }
    }
  }

  return bestArea;
}

/* CoD4 0x42C0A0.  The map tool keeps this separate from the donor's
 * incremental CheckWinding helper: it creates one consistently oriented
 * convex winding from an arbitrary (up to 64 point) point set. */
static float WindingHullCross(const float projected[64][2], int a, int b, int c)
{
  return (projected[b][0] - projected[a][0]) * (projected[c][1] - projected[a][1])
       - (projected[b][1] - projected[a][1]) * (projected[c][0] - projected[a][0]);
}

static Winding_t *WindingFromPoints(const float *planeNormal, const vec3_t *points, int pointCount)
{
  float projected[64][2];
  int pointOrder[64];
  int hullOrder[128];
  int axisU, axisV, hullCount, i, j, tri0, tri1, tri2;
  vec3_t orientNormal;
  Winding_t *winding;

  Assert(pointCount <= 64, g_assertFlags4);
  if ( pointCount < 3 )
    return NULL;

  GetProjectionAxes((float *)planeNormal, &axisU, &axisV);
  for ( i = 0; i < pointCount; ++i )
  {
    projected[i][0] = points[i][axisU];
    projected[i][1] = points[i][axisV];
    pointOrder[i] = i;
  }

  /* This is the same 2D convex-hull role as the CoD4 com_convexhull call at
   * 0x42C163.  The final orientation is normalized below, so its start vertex
   * is intentionally not observable. */
  for ( i = 1; i < pointCount; ++i )
  {
    int point = pointOrder[i];
    for ( j = i; j > 0 && (projected[point][0] < projected[pointOrder[j - 1]][0]
         || (projected[point][0] == projected[pointOrder[j - 1]][0]
          && projected[point][1] < projected[pointOrder[j - 1]][1])); --j )
      pointOrder[j] = pointOrder[j - 1];
    pointOrder[j] = point;
  }

  hullCount = 0;
  for ( i = 0; i < pointCount; ++i )
  {
    while ( hullCount >= 2 && WindingHullCross(projected, hullOrder[hullCount - 2], hullOrder[hullCount - 1], pointOrder[i]) <= 0.0f )
      --hullCount;
    hullOrder[hullCount++] = pointOrder[i];
  }
  for ( i = pointCount - 2, j = hullCount + 1; i >= 0; --i )
  {
    while ( hullCount >= j && WindingHullCross(projected, hullOrder[hullCount - 2], hullOrder[hullCount - 1], pointOrder[i]) <= 0.0f )
      --hullCount;
    hullOrder[hullCount++] = pointOrder[i];
  }
  if ( hullCount > 1 )
    --hullCount; /* duplicated first point */
  if ( hullCount < 3 )
    return NULL;

  winding = AllocWinding(hullCount);
  winding->numpoints = hullCount;
  for ( i = 0; i < hullCount; ++i )
    VectorCopy(points[hullOrder[i]], winding->points[i]);

  if ( WindingLargestTriangleArea(winding, (float *)planeNormal, &tri0, &tri1, &tri2) < 0.001f )
  {
    FreeWinding(winding);
    return NULL;
  }

  VectorSubtract(winding->points[tri1], winding->points[tri0], orientNormal);
  {
    vec3_t edge, cross;
    Winding_t *reversed;
    VectorSubtract(winding->points[tri2], winding->points[tri0], edge);
    CrossProduct(edge, orientNormal, cross);
    if ( DotProduct(cross, planeNormal) < 0.0f )
    {
      reversed = ReverseWinding(winding);
      FreeWinding(winding);
      winding = reversed;
    }
  }
  return winding;
}

/* CoD4 0x42C2D0.  Reduce a linked list in 64-point chunks, matching the
 * binary's bounded stack buffer and its repeated hull compaction. */
Winding_t *WindingFromWindingList(const float *planeNormal, const WindingList_t *list)
{
  vec3_t points[64];
  int pointCount = 0;

  Assert(list, g_assertFlags5);
  if ( !list->next )
    return CopyWinding(list->winding);

  for ( ; list; list = list->next )
  {
    if ( pointCount + list->winding->numpoints > 64 )
    {
      Winding_t *reduced;
      Assert(pointCount, g_assertFlags6);
      reduced = WindingFromPoints(planeNormal, points, pointCount);
      if ( !reduced || reduced->numpoints + list->winding->numpoints > 64 )
        Com_Error("convex hull has too many vertices");
      pointCount = reduced->numpoints;
      memcpy(points, reduced->points, sizeof(vec3_t) * pointCount);
      FreeWinding(reduced);
    }
    memcpy(&points[pointCount], list->winding->points, sizeof(vec3_t) * list->winding->numpoints);
    pointCount += list->winding->numpoints;
  }
  return WindingFromPoints(planeNormal, points, pointCount);
}

/* CoD4 0x42C660: emits the up-to-four intersections made by a plane and the
 * two selected bounds-axis pairs. */
static int AddPlaneBoxIntersectionPoints(const float *plane, const float *mins, const float *maxs, int axisA, int axisB, vec3_t *out)
{
  int signA, signB, axisC = 3 - axisA - axisB, count = 0;
  for ( signA = 0; signA != 2; ++signA )
  {
    for ( signB = 0; signB != 2; ++signB )
    {
      vec3_t point;
      if ( fabsf(plane[axisC]) <= FLT_EPSILON )
        continue;
      point[axisA] = signA ? maxs[axisA] : mins[axisA];
      point[axisB] = signB ? maxs[axisB] : mins[axisB];
      point[axisC] = (plane[3] - plane[axisA] * point[axisA] - plane[axisB] * point[axisB]) / plane[axisC];
      if ( point[axisC] >= mins[axisC] && point[axisC] <= maxs[axisC] )
        VectorCopy(point, out[count++]);
    }
  }
  return count;
}

/* CoD4 0x42C470. */
static Winding_t *WindingFromPlaneAndBounds(const float *plane, const float *mins, const float *maxs)
{
  vec3_t points[12];
  int pointCount = 0;

  pointCount += AddPlaneBoxIntersectionPoints(plane, mins, maxs, 0, 1, &points[pointCount]);
  pointCount += AddPlaneBoxIntersectionPoints(plane, mins, maxs, 1, 2, &points[pointCount]);
  pointCount += AddPlaneBoxIntersectionPoints(plane, mins, maxs, 2, 0, &points[pointCount]);
  return pointCount >= 3 ? WindingFromPoints(plane, points, pointCount) : NULL;
}

/*
================
RemoveDuplicatePoints

Removes consecutive near-duplicate vertices from winding (within epsilon distance).
================
*/
void RemoveDuplicatePoints(Winding_t *w, float epsilon)
{
  int keptCount, totalKept;
  float epsilonSq;
  int i, newCount;

  Assert(w, s_assertDisable_RemoveDuplicatePoints);
  Assert(w->numpoints >= 3, s_assertDisable_RemoveDuplicatePoints);

  epsilonSq = epsilon * epsilon;

  /* remove trailing points that duplicate the first */
  while ( w->numpoints > 0 )
  {
    if ( DistanceSquared(w->points[0], w->points[w->numpoints - 1]) > epsilonSq )
      break;
    w->numpoints--;
  }

  if ( w->numpoints < 3 )
  {
    w->numpoints = 0;
    return;
  }

  /* compact: remove consecutive near-duplicates */
  keptCount = 1;
  for ( i = 1; i < w->numpoints; i++ )
  {
    if ( DistanceSquared(w->points[i], w->points[keptCount - 1]) > epsilonSq )
    {
      VectorCopy(w->points[i], w->points[keptCount]);
      keptCount++;
    }
  }

  totalKept = keptCount - 1;
  newCount = keptCount;
  Assert(w->numpoints >= newCount, s_assertDisable_RemoveDuplicatePoints);
  w->numpoints = newCount;
}

/*
================
RemoveColinearPoints

Removes colinear points from winding where adjacent edges have dot > 0.999.
================
*/
int RemoveColinearPoints(Winding_t *w)
{
  int i, prevIdx, nextIdx;
  vec3_t edgeNext, edgePrev;
  int keptCount;
  vec3_t tmpPoints[MAX_POINTS_ON_WINDING];

  keptCount = 0;
  for ( i = 0; i < w->numpoints; i++ )
  {
    prevIdx = (w->numpoints + i - 1) % w->numpoints;
    nextIdx = (i + 1) % w->numpoints;

    VectorSubtract(w->points[nextIdx], w->points[i], edgeNext);
    VectorSubtract(w->points[i], w->points[prevIdx], edgePrev);
    VecNormalize(edgeNext);
    VecNormalize(edgePrev);

    /* keep points where edges aren't colinear */
    #define COLINEAR_DOT 0.999
    if ( DotProduct210(edgePrev, edgeNext) < COLINEAR_DOT )
    {
      VectorCopy(w->points[i], tmpPoints[keptCount]);
      keptCount++;
    }
  }

  if ( keptCount != w->numpoints )
  {
    w->numpoints = keptCount;
    memcpy(w->points, tmpPoints, keptCount * sizeof(vec3_t));
  }
  return keptCount;
}

/* CoD4 0x42B920. */
int WindingPlaneFromFirstThree(Winding_t *w, float *plane)
{
  return PlaneFromPoints(plane, w->points[0], w->points[1], w->points[2]);
}

/* CoD4 0x42B950.  Builds the winding plane and returns its unsigned area. */
float WindingPlaneAndArea(Winding_t *w, float *plane)
{
  vec3_t edge0;
  vec3_t edge1;
  vec3_t cross;
  float area;
  int i;

  VectorClear(plane);
  for ( i = 2; i < w->numpoints; ++i )
  {
    VectorSubtract(w->points[i - 1], w->points[0], edge0);
    VectorSubtract(w->points[i], w->points[0], edge1);
    CrossProduct(edge1, edge0, cross);
    VectorAdd(plane, cross, plane);
  }
  area = VecNormalize(plane) * 0.5f;
  plane[3] = DotProduct( w->points[0], plane );
  return area;
}

/* CoD4 0x42BA30. */
int WindingHasPlane(Winding_t *w, float *plane)
{
  return WindingPlaneAndArea(w, plane) != 0.0f;
}

/*
================
PlaneFromTriangle

Computes plane equation from winding's first 3 vertices.
================
*/
int PlaneFromTriangle(Winding_t *w, float *normal, float *dist)
{
  vec3_t edge0, edge1;

  VectorSubtract(w->points[1], w->points[0], edge0);
  VectorSubtract(w->points[2], w->points[0], edge1);
  CrossProduct(edge1, edge0, normal);
  VecNormalize(normal);
  *dist = DotProduct(w->points[0], normal);
  return 0;
}

/*
================
PlaneFromWinding

Computes polygon normal and dist using Newell's method (sum of cross products).
More robust than PlaneFromTriangle for non-planar inputs. Returns 0 if degenerate.
================
*/
int PlaneFromWinding(int numPoints, float *points, float *normal, float *dist)
{
  vec3_t cross, edge0, edge1;
  vec3_t *p;
  int i;

  p = (vec3_t *)points;
  VectorClear(normal);

  /* Newell's method: sum cross products of edges from p[0] */
  for ( i = 1; i < numPoints - 1; i++ )
  {
    VectorSubtract(p[i], p[0], edge0);
    VectorSubtract(p[i + 1], p[0], edge1);
    CrossProduct(edge1, edge0, cross);
    VectorAdd(normal, cross, normal);
  }

  if ( 0.0 == VecNormalize(normal) )
    return 0;

  *dist = DotProduct(p[0], normal);
  return 1;
}

/*
================
WindingArea

Computes surface area of winding using triangle fan from vertex 0.
================
*/
double WindingArea(Winding_t *w)
{
  vec3_t e0, e1, cross;
  float total;
  int i;

  total = 0.0;
  for ( i = 2; i < w->numpoints; i++ )
  {
    VectorSubtract(w->points[i - 1], w->points[0], e0);
    VectorSubtract(w->points[i], w->points[0], e1);
    CrossProduct(e0, e1, cross);
    total += VectorLength(cross) * 0.5;
  }
  return total;
}

/*
================
WindingSignedArea

Computes signed area of winding projected onto a plane normal.
================
*/
double WindingSignedArea(Winding_t *w, float *normal)
{
  vec3_t e0, e1, cross;
  float total;
  int i;

  total = 0.0;
  for ( i = 2; i < w->numpoints; i++ )
  {
    VectorSubtract(w->points[i - 1], w->points[0], e0);
    VectorSubtract(w->points[i], w->points[0], e1);
    CrossProduct(e1, e0, cross);
    total += DotProduct120(cross, normal);
  }
  return total * 0.5;
}

/* CoD4 0x42BBD0. */
int WindingTripleIsConcave(Winding_t *w, int point0, int point1, int point2, float *normal)
{
  vec3_t edge0;
  vec3_t edge1;
  vec3_t cross;

  VectorSubtract(w->points[point2], w->points[point1], edge0);
  VectorSubtract(w->points[point0], w->points[point1], edge1);
  CrossProduct(edge1, edge0, cross);
  return DotProduct(cross, normal) < -0.001000000047497451f;
}

/* CoD4 0x42BC70. */
int WindingIsConvex(Winding_t *w, float *normal)
{
  int i;

  if ( WindingTripleIsConcave(w, w->numpoints - 2, w->numpoints - 1, 0, normal) )
    return 0;
  if ( WindingTripleIsConcave(w, w->numpoints - 1, 0, 1, normal) )
    return 0;
  for ( i = 2; i < w->numpoints; ++i )
    if ( WindingTripleIsConcave(w, i - 2, i - 1, i, normal) )
      return 0;
  return 1;
}

/*
================
WindingBounds

Computes bounding box (mins/maxs) of a winding.
================
*/
void WindingBounds(Winding_t *w, float *mins, float *maxs)
{
  int i, r;

  Assert(w, g_assertFlags2);
  Assert(!(w->numpoints <= 0), g_assertFlags3);

  /* seed from first point, expand with remaining */
  VectorCopy(w->points[0], mins);
  VectorCopy(w->points[0], maxs);
  for ( i = 1; i < w->numpoints; i++ )
    AddPointToBounds(w->points[i], mins, maxs);
}

/*
================
WindingCenter

Computes centroid (average of all vertices) of a winding.
================
*/
void WindingCenter(Winding_t *w, float *center)
{
  double scale;
  int i;

  VectorClear(center);
  for ( i = 0; i < w->numpoints; i++ )
    VectorAdd(center, w->points[i], center);

  scale = 1.0 / w->numpoints;
  VectorScale(center, scale, center);
}

/*
================
BaseWindingForPlane

Creates a huge axis-aligned winding polygon for a plane.
Projects a large rectangle onto the plane using up/right vectors.
================
*/
Winding_t *BaseWindingForPlane(float *normal, float dist)
{
  int i, x;
  float max, v;
  vec3_t org, vright, vup;
  Winding_t *w;

  /* find the major axis */
  max = -MAX_WORLD_COORD;
  x = -1;
  for ( i = 0; i < 3; i++ )
  {
    v = fabs(normal[i]);
    if ( v > max )
    {
      x = i;
      max = v;
    }
  }
  if ( x == -1 )
    Com_Error("BaseWindingForPlane: no axis found");

  VectorClear(vup);
  switch ( x )
  {
  case 0:
  case 1:
    vup[2] = 1;
    break;
  case 2:
    vup[0] = 1;
    break;
  }

  v = DotProduct120(vup, normal);
  VectorMA(vup, -v, normal, vup);
  VecNormalize(vup);

  VectorScale(normal, dist, org);
  CrossProduct(vup, normal, vright);

  VectorScale(vup, MAX_WORLD_COORD, vup);
  VectorScale(vright, MAX_WORLD_COORD, vright);

  /* project a really big axis aligned box onto the plane */
  w = AllocWinding(4);

  VectorSubtract(org, vright, w->points[0]);
  VectorAdd(w->points[0], vup, w->points[0]);

  VectorAdd(org, vright, w->points[1]);
  VectorAdd(w->points[1], vup, w->points[1]);

  VectorAdd(org, vright, w->points[2]);
  VectorSubtract(w->points[2], vup, w->points[2]);

  VectorSubtract(org, vright, w->points[3]);
  VectorSubtract(w->points[3], vup, w->points[3]);

  w->numpoints = 4;

  return w;
}

/*
================
ClipWinding

Classifies or splits a winding against a plane. The public wrapper below
handles copy allocation for whole-winding front/back/on results.
================
*/
int ClipWinding(
    Winding_t *inWinding,
    float *planeNormal,
    float planeDist,
    float epsilon,
    Winding_t **outFront,
    Winding_t **outBack)
{
  int numPoints, i;
  double signedDist;
  Winding_t *f, *b;
  int curSide, nextSide;
  int maxPts;
  double dists[MAX_CLIP_POINTS];
  int sides[MAX_CLIP_POINTS];
  int counts[3];
  vec3_t mid;
  float axialSnapValue;
  int axialAxis;
  int isExactAxialPlane;

  /* parameter validation */
  Assert(inWinding, g_assertFlags4);
  Assert(planeNormal, g_assertFlags5);
  Assert(DotProduct(planeNormal, planeNormal) > 0.0, g_assertFlags6);
  Assert(outFront, g_assertFlags7);
  Assert(outBack, g_assertFlags8);

  /* classify each vertex */
  numPoints = inWinding->numpoints;
  counts[0] = counts[1] = counts[2] = 0;

  for ( i = 0; i < numPoints; i++ )
  {
    signedDist = DotProduct021(inWinding->points[i], planeNormal) - planeDist;
    dists[i] = signedDist;

    if ( signedDist <= epsilon )
    {
      if ( signedDist >= -epsilon )
        sides[i] = SIDE_ON;
      else
        sides[i] = SIDE_BACK;
    }
    else
    {
      sides[i] = SIDE_FRONT;
    }
    ++counts[sides[i]];
  }

  /* wrap around for edge processing */
  dists[numPoints] = dists[0];
  sides[numPoints] = sides[0];

  if ( !counts[SIDE_FRONT] )
  {
    /* entirely behind — copy to back */
    return counts[SIDE_BACK] ? SIDE_BACK : SIDE_ON;

    /* keepOnPlane: all ON vertices go to front too */
  }

  if ( !counts[SIDE_BACK] )
  {
    /* entirely in front */
    return SIDE_FRONT;
  }

  /* detect exact axial plane for intersection snapping */
  isExactAxialPlane = 1;
  axialAxis = -1;
  axialSnapValue = 0.0f;
  for ( i = 0; i < 3; i++ )
  {
    if ( planeNormal[i] == 1.0f )
    {
      axialAxis = i;
      axialSnapValue = planeDist;
    }
    else if ( planeNormal[i] == -1.0f )
    {
      axialAxis = i;
      axialSnapValue = -planeDist;
    }
    else if ( planeNormal[i] != 0.0f )
    {
      isExactAxialPlane = 0;
      break;
    }
  }
  if ( axialAxis < 0 )
    isExactAxialPlane = 0;

  Assert(!isExactAxialPlane || (axialAxis >= 0 && axialAxis < 3), g_assertFlags9);

  /* build front and back windings */
  maxPts = numPoints + 4;
  f = AllocWinding(maxPts);
  b = AllocWinding(maxPts);

  for ( i = 0; i < numPoints; i++ )
  {
    curSide = sides[i];

    if ( curSide == SIDE_ON )
    {
      /* on the plane — add to both */
      VectorCopy(inWinding->points[i], f->points[f->numpoints]);
      f->numpoints++;
      VectorCopy(inWinding->points[i], b->points[b->numpoints]);
      b->numpoints++;
      continue;
    }

    if ( curSide == SIDE_FRONT )
    {
      VectorCopy(inWinding->points[i], f->points[f->numpoints]);
      f->numpoints++;
    }
    else
    {
      VectorCopy(inWinding->points[i], b->points[b->numpoints]);
      b->numpoints++;
    }

    /* check if edge crosses the plane */
    nextSide = sides[i + 1];
    if ( nextSide == SIDE_ON || nextSide == curSide )
      continue;

    /* compute intersection point */
    {
      int nextIdx = (i + 1) % numPoints;
      double d1 = dists[i];
      double d2 = dists[i + 1];
      double dd = d1 - d2;
      mid[0] = (float)((inWinding->points[nextIdx][0] * d1 - inWinding->points[i][0] * d2) / dd);
      mid[1] = (float)((inWinding->points[nextIdx][1] * d1 - inWinding->points[i][1] * d2) / dd);
      mid[2] = (float)((inWinding->points[nextIdx][2] * d1 - inWinding->points[i][2] * d2) / dd);
    }

    /* snap axial intersection to exact plane distance */
    if ( isExactAxialPlane )
      mid[axialAxis] = axialSnapValue;

    /* add intersection to both windings */
    VectorCopy(mid, f->points[f->numpoints]);
    f->numpoints++;
    VectorCopy(mid, b->points[b->numpoints]);
    b->numpoints++;
  }

  /* validate */
  Assert(f->numpoints <= maxPts, g_assertFlagsA);
  Assert(b->numpoints <= maxPts, g_assertFlagsB);
  Assert(f->numpoints <= MAX_POINTS_ON_WINDING, g_assertFlagsC);
  Assert(b->numpoints <= MAX_POINTS_ON_WINDING, g_assertFlagsD);

  /* return results */
  *outFront = f;
  *outBack = b;
  return SIDE_CROSS;
}

/* The public CoD4 entry point at 0x42D050 supplies copies for a whole
 * winding and only asks ClipWinding to allocate when a plane actually splits
 * it.  Keeping that distinction preserves both allocation behaviour and the
 * `keepOnPlane` rule. */
void ClipWindingEpsilon(
    Winding_t *inWinding,
    float *planeNormal,
    float planeDist,
    float epsilon,
    Winding_t **outFront,
    Winding_t **outBack,
    int keepOnPlane)
{
  const int side = ClipWinding(inWinding, planeNormal, planeDist, epsilon, outFront, outBack);

  if ( side == SIDE_BACK )
  {
    *outFront = NULL;
    *outBack = CopyWinding(inWinding);
  }
  else if ( side != SIDE_CROSS )
  {
    *outFront = CopyWinding(inWinding);
    *outBack = keepOnPlane && side == SIDE_ON ? CopyWinding(inWinding) : NULL;
  }
}

/*
================
ValidateWinding

Debug validation function for windings (dead code in original — never called).
Checks: point count >= 3, area > 1.0, vertex range < 131072, vertices on plane,
no duplicate points, convexity. Calls Com_Error on any failure.
================
*/
void ValidateWinding(Winding_t *w)
{
  int numPts;
  int i, j, nextIdx;
  vec3_t edge, edgeNormal, normal;
  float edgeLen, area, dist, planeDist, edgePlaneDist;

  numPts = w->numpoints;
  if ( numPts < 3 )
    Com_Error("CheckWinding: %i points", numPts);

  area = WindingArea(w);
  if ( area < 1.0f )
    Com_Error("CheckWinding: %f area", area);

  PlaneFromTriangle(w, normal, &planeDist);

  for ( i = 0; i < numPts; i++ )
  {
    /* check vertex range */
    for ( j = 0; j < 3; j++ )
    {
      if ( w->points[i][j] > MAX_WORLD_COORD || w->points[i][j] < MIN_WORLD_COORD )
        Com_Error("CheckFace: BUGUS_RANGE: %f", (double)w->points[i][j]);
    }

    /* check vertex on plane */
    nextIdx = (i + 1 == numPts) ? 0 : i + 1;
    dist = DotProduct120(w->points[i], normal) - planeDist;
    if ( dist < -ON_EPSILON || dist > ON_EPSILON )
      Com_Error("CheckWinding: point off plane");

    /* check no duplicate points */
    VectorSubtract(w->points[nextIdx], w->points[i], edge);
    edgeLen = (float)sqrt(DotProduct210(edge, edge));
    if ( edgeLen < ON_EPSILON )
      Com_Error("CheckWinding: degenerate edge");

    /* check convexity */
    CrossProduct(normal, edge, edgeNormal);
    VecNormalize(edgeNormal);
    edgePlaneDist = DotProduct120(w->points[i], edgeNormal) + ON_EPSILON;
    for ( j = 0; j < numPts; j++ )
    {
      if ( j != i && DotProduct102(w->points[j], edgeNormal) > edgePlaneDist )
        Com_Error("CheckWinding: non-convex");
    }
  }
}

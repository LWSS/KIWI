/*
 * primarylights_region.cpp -- primary-light convex-region list primitives.
 * Native reference: cod4map.exe 0x436030-0x436338.
 */

#include "cod4map.h"

#define PRIMARY_LIGHT_REGION_MAX_WINDING_POINTS 1024

static char s_assertDisable_BuildHullLightConeWinding;
static char s_assertDisable_BuildHullLightConePointCount;
static char s_assertDisable_BuildHullLightConeInitialCosine;
static char s_assertDisable_BuildHullLightConePositiveMinimum;
static char s_assertDisable_BuildHullLightConeIterationCount;
static char s_assertDisable_BuildHullLightConeDirectionChanged;
static char s_assertDisable_BuildHullLightConeFinalCosine;
static char s_assertDisable_GenerateEnclosingPrimaryLightHullWinding;
static char s_assertDisable_GenerateEnclosingPrimaryLightHullClippedWinding;
static char s_assertDisable_ClipPrimaryLightCastersSide;
static char s_assertDisable_ClipPrimaryLightCastersPointCount;
static char s_assertDisable_PrimaryLightRegionSmallestEdgeDistance;
static char s_assertDisable_DecomposeConvexPrimaryLightRegionHullLimit;
static char s_assertDisable_EmitReducedPrimaryLightKdopAxisCount;
static char s_assertDisable_PrimaryLightCandidateDirectionLimit;
static char s_assertDisable_BasePrimaryLightKdopAxisCount;
static char s_assertDisable_BasePrimaryLightKdopAxisPair;

/* Native 0x4FEE98: 26 k-DOP directions used by 0x4367F0. */
static const float s_primaryLightKdopDirs[26][3] =
{
  { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
  { 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
  { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f },
  { 0.70710677f, 0.70710677f, 0.0f }, { -0.70710677f, -0.70710677f, 0.0f },
  { 0.70710677f, -0.70710677f, 0.0f }, { -0.70710677f, 0.70710677f, 0.0f },
  { 0.70710677f, 0.0f, 0.70710677f }, { -0.70710677f, 0.0f, -0.70710677f },
  { 0.70710677f, 0.0f, -0.70710677f }, { -0.70710677f, 0.0f, 0.70710677f },
  { 0.0f, 0.70710677f, 0.70710677f }, { 0.0f, -0.70710677f, -0.70710677f },
  { 0.0f, 0.70710677f, -0.70710677f }, { 0.0f, -0.70710677f, 0.70710677f },
  { 0.57735026f, 0.57735026f, 0.57735026f }, { -0.57735026f, -0.57735026f, -0.57735026f },
  { 0.57735026f, 0.57735026f, -0.57735026f }, { -0.57735026f, -0.57735026f, 0.57735026f },
  { 0.57735026f, -0.57735026f, 0.57735026f }, { -0.57735026f, 0.57735026f, -0.57735026f },
  { 0.57735026f, -0.57735026f, -0.57735026f }, { -0.57735026f, 0.57735026f, 0.57735026f }
};

/* Local exact copies of the private 0x42C0A0/0x42C470 polylib chain.  The
 * original symbols are private to polylib.cpp, while 0x4367F0 calls them. */
static float PrimaryLightRegionHullCross(const float projected[64][2], int a, int b, int c)
{
  return (projected[b][0] - projected[a][0]) * (projected[c][1] - projected[a][1])
       - (projected[b][1] - projected[a][1]) * (projected[c][0] - projected[a][0]);
}

static Winding_t *PrimaryLightRegionWindingFromPoints(const float *planeNormal, const vec3_t *points, int pointCount)
{
  float projected[64][2];
  int pointOrder[64];
  int hullOrder[128];
  int axisU, axisV, hullCount, pointIndex, sortIndex, tri0, tri1, tri2;
  vec3_t orientNormal;
  Winding_t *winding;

  if (pointCount < 3)
    return NULL;

  GetProjectionAxes((float *)planeNormal, &axisU, &axisV);
  for (pointIndex = 0; pointIndex < pointCount; ++pointIndex)
  {
    projected[pointIndex][0] = points[pointIndex][axisU];
    projected[pointIndex][1] = points[pointIndex][axisV];
    pointOrder[pointIndex] = pointIndex;
  }

  for (pointIndex = 1; pointIndex < pointCount; ++pointIndex)
  {
    int point = pointOrder[pointIndex];
    for (sortIndex = pointIndex; sortIndex > 0 && (projected[point][0] < projected[pointOrder[sortIndex - 1]][0]
         || (projected[point][0] == projected[pointOrder[sortIndex - 1]][0]
          && projected[point][1] < projected[pointOrder[sortIndex - 1]][1])); --sortIndex)
      pointOrder[sortIndex] = pointOrder[sortIndex - 1];
    pointOrder[sortIndex] = point;
  }

  hullCount = 0;
  for (pointIndex = 0; pointIndex < pointCount; ++pointIndex)
  {
    while (hullCount >= 2 && PrimaryLightRegionHullCross(projected, hullOrder[hullCount - 2], hullOrder[hullCount - 1], pointOrder[pointIndex]) <= 0.0f)
      --hullCount;
    hullOrder[hullCount++] = pointOrder[pointIndex];
  }
  for (pointIndex = pointCount - 2, sortIndex = hullCount + 1; pointIndex >= 0; --pointIndex)
  {
    while (hullCount >= sortIndex && PrimaryLightRegionHullCross(projected, hullOrder[hullCount - 2], hullOrder[hullCount - 1], pointOrder[pointIndex]) <= 0.0f)
      --hullCount;
    hullOrder[hullCount++] = pointOrder[pointIndex];
  }
  if (hullCount > 1)
    --hullCount;
  if (hullCount < 3)
    return NULL;

  winding = AllocWinding(hullCount);
  winding->numpoints = hullCount;
  for (pointIndex = 0; pointIndex < hullCount; ++pointIndex)
    VectorCopy(points[hullOrder[pointIndex]], winding->points[pointIndex]);

  if (WindingLargestTriangleArea(winding, (float *)planeNormal, &tri0, &tri1, &tri2) < 0.001f)
  {
    FreeWinding(winding);
    return NULL;
  }

  VectorSubtract(winding->points[tri1], winding->points[tri0], orientNormal);
  {
    vec3_t edge, cross;
    VectorSubtract(winding->points[tri2], winding->points[tri0], edge);
    CrossProduct(edge, orientNormal, cross);
    if (DotProduct(cross, planeNormal) < 0.0f)
    {
      Winding_t *reversed = ReverseWinding(winding);
      FreeWinding(winding);
      winding = reversed;
    }
  }
  return winding;
}

static int PrimaryLightRegionAddPlaneBoxIntersectionPoints(const float *plane, const float *mins, const float *maxs, int axisA, int axisB, vec3_t *out)
{
  int signA, signB, axisC = 3 - axisA - axisB, count = 0;
  for (signA = 0; signA != 2; ++signA)
  {
    for (signB = 0; signB != 2; ++signB)
    {
      vec3_t point;
      if (fabsf(plane[axisC]) <= FLT_EPSILON)
        continue;
      point[axisA] = signA ? maxs[axisA] : mins[axisA];
      point[axisB] = signB ? maxs[axisB] : mins[axisB];
      point[axisC] = (plane[3] - plane[axisA] * point[axisA] - plane[axisB] * point[axisB]) / plane[axisC];
      if (point[axisC] >= mins[axisC] && point[axisC] <= maxs[axisC])
        VectorCopy(point, out[count++]);
    }
  }
  return count;
}

static Winding_t *PrimaryLightRegionWindingFromPlaneAndBounds(const float *plane, const float *mins, const float *maxs)
{
  vec3_t points[12];
  int pointCount = 0;

  pointCount += PrimaryLightRegionAddPlaneBoxIntersectionPoints(plane, mins, maxs, 0, 1, &points[pointCount]);
  pointCount += PrimaryLightRegionAddPlaneBoxIntersectionPoints(plane, mins, maxs, 1, 2, &points[pointCount]);
  pointCount += PrimaryLightRegionAddPlaneBoxIntersectionPoints(plane, mins, maxs, 2, 0, &points[pointCount]);
  return pointCount >= 3 ? PrimaryLightRegionWindingFromPoints(plane, points, pointCount) : NULL;
}

/* Local exact copy of private 0x42C980. */
static int PrimaryLightRegionClipWinding(Winding_t *inWinding, float *planeNormal, float planeDist, float epsilon, Winding_t **outFront, Winding_t **outBack)
{
  int pointCount, pointIndex, nextSide, maxPoints;
  double signedDist;
  Winding_t *front, *back;
  int currentSide;
  double dists[MAX_CLIP_POINTS];
  int sides[MAX_CLIP_POINTS];
  int counts[3];
  vec3_t mid;
  float axialSnapValue;
  int axialAxis;
  int isExactAxialPlane;

  pointCount = inWinding->numpoints;
  counts[0] = counts[1] = counts[2] = 0;
  for (pointIndex = 0; pointIndex < pointCount; ++pointIndex)
  {
    signedDist = DotProduct021(inWinding->points[pointIndex], planeNormal) - planeDist;
    dists[pointIndex] = signedDist;
    if (signedDist <= epsilon)
      sides[pointIndex] = signedDist >= -epsilon ? SIDE_ON : SIDE_BACK;
    else
      sides[pointIndex] = SIDE_FRONT;
    ++counts[sides[pointIndex]];
  }
  dists[pointCount] = dists[0];
  sides[pointCount] = sides[0];

  if (!counts[SIDE_FRONT])
    return counts[SIDE_BACK] ? SIDE_BACK : SIDE_ON;
  if (!counts[SIDE_BACK])
    return SIDE_FRONT;

  isExactAxialPlane = 1;
  axialAxis = -1;
  axialSnapValue = 0.0f;
  for (pointIndex = 0; pointIndex < 3; ++pointIndex)
  {
    if (planeNormal[pointIndex] == 1.0f)
    {
      axialAxis = pointIndex;
      axialSnapValue = planeDist;
    }
    else if (planeNormal[pointIndex] == -1.0f)
    {
      axialAxis = pointIndex;
      axialSnapValue = -planeDist;
    }
    else if (planeNormal[pointIndex] != 0.0f)
    {
      isExactAxialPlane = 0;
      break;
    }
  }
  if (axialAxis < 0)
    isExactAxialPlane = 0;

  maxPoints = pointCount + 4;
  front = AllocWinding(maxPoints);
  back = AllocWinding(maxPoints);
  for (pointIndex = 0; pointIndex < pointCount; ++pointIndex)
  {
    currentSide = sides[pointIndex];
    if (currentSide == SIDE_ON)
    {
      VectorCopy(inWinding->points[pointIndex], front->points[front->numpoints++]);
      VectorCopy(inWinding->points[pointIndex], back->points[back->numpoints++]);
      continue;
    }
    if (currentSide == SIDE_FRONT)
      VectorCopy(inWinding->points[pointIndex], front->points[front->numpoints++]);
    else
      VectorCopy(inWinding->points[pointIndex], back->points[back->numpoints++]);

    nextSide = sides[pointIndex + 1];
    if (nextSide == SIDE_ON || nextSide == currentSide)
      continue;
    {
      int nextIndex = (pointIndex + 1) % pointCount;
      double d1 = dists[pointIndex];
      double d2 = dists[pointIndex + 1];
      double d = d1 - d2;
      mid[0] = (float)((inWinding->points[nextIndex][0] * d1 - inWinding->points[pointIndex][0] * d2) / d);
      mid[1] = (float)((inWinding->points[nextIndex][1] * d1 - inWinding->points[pointIndex][1] * d2) / d);
      mid[2] = (float)((inWinding->points[nextIndex][2] * d1 - inWinding->points[pointIndex][2] * d2) / d);
    }
    if (isExactAxialPlane)
      mid[axialAxis] = axialSnapValue;
    VectorCopy(mid, front->points[front->numpoints++]);
    VectorCopy(mid, back->points[back->numpoints++]);
  }
  *outFront = front;
  *outBack = back;
  return SIDE_CROSS;
}

/* Local exact copy of private 0x42E110. */
static float PrimaryLightRegionSmallestEdgeDistance(const Winding_t *winding, const float *planeNormal)
{
  float smallest = FLT_MAX;
  int previous = winding->numpoints - 1;
  int current;

  for (current = 0; current < winding->numpoints; ++current)
  {
    vec3_t edge, edgePlane;
    float edgeLength, edgeDist, largest = -FLT_MAX;
    int test = (current + 1) % winding->numpoints;

    VectorSubtract(winding->points[current], winding->points[previous], edge);
    CrossProduct(edge, planeNormal, edgePlane);
    edgeLength = VectorLength(edgePlane);
    edgeDist = DotProduct(edgePlane, winding->points[previous]);
    do
    {
      float pointDist = DotProduct(winding->points[test], edgePlane) - edgeDist;
      Assert(pointDist >= -ON_EPSILON || edgeLength < 1.0f, s_assertDisable_PrimaryLightRegionSmallestEdgeDistance);
      if (pointDist < largest)
        break;
      largest = pointDist;
      test = (test + 1) % winding->numpoints;
    }
    while (test != previous);

    if (largest < smallest)
      smallest = largest;
    previous = current;
  }
  return smallest;
}

/* Native 0x435FA0. */
static float PrimaryLightConeProjection(float cosA, float cosB)
{
  return cosA * cosB - sqrtf((1.0f - cosA * cosA) * (1.0f - cosB * cosB));
}

/* Defined by the following native primarylights_region.cpp clusters. */
PrimaryLightRegionNode_t **ClipOneHullByPlane(const float *plane, PrimaryLightRegionNode_t **slot);
PrimaryLightRegionNode_t **ClipOneHullByCone(const float *dir, float sinHalfFov, float cosHalfFov, PrimaryLightRegionNode_t **slot);
unsigned int AddBasePrimaryLightKdopAxes(float (*axes)[5], unsigned int axisCount);
unsigned int InsertPrimaryLightCandidateDirection(const float *dir, float (*axes)[5], unsigned int axisCount);
unsigned int AddPrimaryLightCasterEdgeAxes(PrimaryLightRegionNode_t *caster, float (*axes)[5], unsigned int axisCount);
unsigned int AddPrimaryLightConeAxes(const PrimaryLightRegionDesc_t *light, float (*axes)[5], unsigned int axisCount);
float PrimaryLightKdopAreaMetric(const float (*axes)[5], unsigned int axisCount);
void ClipHullListByPlane(PrimaryLightRegionNode_t **list, const float *plane);
void DispatchPrimaryLightConeClip(PrimaryLightRegionNode_t **list, const float *dir, float cosHalfFov);
unsigned int CollectPrimaryLightKdopAxes(PrimaryLightRegionNode_t *list, PrimaryLightRegionNode_t *caster, const PrimaryLightRegionDesc_t *light, float (*axes)[5]);
void PrimaryLightProjectionExtent(PrimaryLightRegionNode_t *list, PrimaryLightRegionNode_t *caster, const float *axis, float *outMin, float *outMax);
unsigned int SelectPrimaryLightKdopAxes(unsigned int *kdop, float (*axes)[5], unsigned int axisCount);
void ClipRegionByPrimaryLightCone(PrimaryLightRegionNode_t **list, const PrimaryLightRegionDesc_t *light);
void EmitReducedPrimaryLightKdop(PrimaryLightRegionNode_t *list, PrimaryLightRegionNode_t *caster, const PrimaryLightRegionDesc_t *light, void *outKdop);
void *EmitPackedPrimaryLightRegion(void *kdop);
PrimaryLightRegionNode_t *MergeCoplanarPrimaryLightHulls(PrimaryLightRegionNode_t *list);
void ClipHullListByCasterWinding(PrimaryLightRegionNode_t **list, PrimaryLightRegionNode_t *caster);

/* Native 0x436730. */
static int UpdatePrimaryLightConeBounds(PrimaryLightRegionNode_t *node, const float *testDir, const float (*pointDirs)[3], unsigned int pointCount, float *cosHalfFovMin, float *cosHalfFovMax)
{
  float cosine;
  unsigned int pointIndex;

  *cosHalfFovMin = 1.0f;
  *cosHalfFovMax = -1.0f;
  for (pointIndex = 0; pointIndex < pointCount; ++pointIndex)
  {
    cosine = DotProduct(pointDirs[pointIndex], testDir);
    if (cosine < *cosHalfFovMin)
      *cosHalfFovMin = cosine;
    if (cosine > *cosHalfFovMax)
      *cosHalfFovMax = cosine;
  }

  if (*cosHalfFovMin <= node->cosHalfFov)
    return 0;

  node->cosHalfFov = *cosHalfFovMin;
  VectorNegate(testDir, node->dir);
  return 1;
}

/* Native 0x436340. */
void BuildHullLightCone(PrimaryLightRegionNode_t *node)
{
  float testDir[3];
  float pointDirs[PRIMARY_LIGHT_REGION_MAX_WINDING_POINTS][3];
  float directionSum[3];
  float cosHalfFovMin;
  float cosHalfFovMax;
  float cosineThreshold;
  float lastCosHalfFov;
  float pointCosine;
  float coneAdjust;
  unsigned int pointIndex;
  int iterations;
  Winding_t *winding;

  Assert(node->w, s_assertDisable_BuildHullLightConeWinding);
  Assert(node->w->numpoints <= PRIMARY_LIGHT_REGION_MAX_WINDING_POINTS, s_assertDisable_BuildHullLightConePointCount);

  winding = node->w;
  VectorClear(directionSum);
  for (pointIndex = 0; pointIndex < (unsigned int)winding->numpoints; ++pointIndex)
  {
    Vec3NormalizeTo(winding->points[pointIndex], pointDirs[pointIndex]);
    VectorAdd(pointDirs[pointIndex], directionSum, directionSum);
  }
  Vec3NormalizeTo(directionSum, testDir);

  node->cosHalfFov = -2.0f;
  UpdatePrimaryLightConeBounds(node, testDir, pointDirs, winding->numpoints, &cosHalfFovMin, &cosHalfFovMax);
  AssertFatal(node->cosHalfFov > -1.0f, s_assertDisable_BuildHullLightConeInitialCosine);

  if (node->cosHalfFov < 0.9800000190734863f)
  {
    VectorNegate(node->plane, testDir);
    UpdatePrimaryLightConeBounds(node, testDir, pointDirs, winding->numpoints, &cosHalfFovMin, &cosHalfFovMax);
    Assert(cosHalfFovMin > 0.0f, s_assertDisable_BuildHullLightConePositiveMinimum);

    if (node->cosHalfFov < 0.9800000190734863f)
    {
      iterations = 0;
      coneAdjust = 0.0625f;
      do
      {
        ++iterations;
        Assert(iterations < 10000, s_assertDisable_BuildHullLightConeIterationCount);

        cosineThreshold = coneAdjust * 0.5f * (cosHalfFovMax - cosHalfFovMin) + cosHalfFovMin;
        VectorNegate(node->dir, directionSum);
        for (pointIndex = 0; pointIndex < (unsigned int)winding->numpoints; ++pointIndex)
        {
          pointCosine = -DotProduct(pointDirs[pointIndex], node->dir);
          if (cosineThreshold >= pointCosine)
            VectorMA(directionSum, coneAdjust, pointDirs[pointIndex], directionSum);
        }
        Vec3NormalizeTo(directionSum, testDir);
        lastCosHalfFov = node->cosHalfFov;
        Assert(!VectorCompare(node->dir, testDir), s_assertDisable_BuildHullLightConeDirectionChanged);
      }
      while (UpdatePrimaryLightConeBounds(node, testDir, pointDirs, winding->numpoints, &cosHalfFovMin, &cosHalfFovMax)
             && node->cosHalfFov >= lastCosHalfFov + 0.00009999999747378752f);

      AssertFatal(node->cosHalfFov > 0.0f, s_assertDisable_BuildHullLightConeFinalCosine);
    }
  }
}

/* Native 0x4367F0. */
void GenerateEnclosingPrimaryLightHull(PrimaryLightRegionNode_t **outList, const PrimaryLightRegionDesc_t *light)
{
  float mins[3];
  float maxs[3];
  float plane[4];
  Winding_t *winding;
  unsigned int directionIndex;
  unsigned int clipDirectionIndex;

  mins[0] = -light->radius;
  mins[1] = -light->radius;
  mins[2] = -light->radius;
  maxs[0] = light->radius;
  maxs[1] = light->radius;
  maxs[2] = light->radius;

  for (directionIndex = 0; directionIndex < 26; ++directionIndex)
  {
    VectorCopy(s_primaryLightKdopDirs[directionIndex], plane);
    plane[3] = -light->radius;
    winding = PrimaryLightRegionWindingFromPlaneAndBounds(plane, mins, maxs);
    for (clipDirectionIndex = 6; clipDirectionIndex < 26; ++clipDirectionIndex)
    {
      if (clipDirectionIndex != directionIndex)
      {
        Assert(winding, s_assertDisable_GenerateEnclosingPrimaryLightHullWinding);
        ClipWindingByPlane(&winding, (float *)s_primaryLightKdopDirs[clipDirectionIndex], -light->radius, 0.1f);
      }
    }
    Assert(winding, s_assertDisable_GenerateEnclosingPrimaryLightHullClippedWinding);
    InsertPrimaryLightHullLink(outList, winding, plane, 1, NULL);
  }
}

/* Native 0x4369F0. */
PrimaryLightRegionNode_t *CopyPrimaryLightHullList(PrimaryLightRegionNode_t *source)
{
  PrimaryLightRegionNode_t *head;
  PrimaryLightRegionNode_t **tail;

  head = NULL;
  tail = &head;
  while (source)
  {
    *tail = AllocPrimaryLightRegionNode();
    memcpy(*tail, source, sizeof(**tail));
    (*tail)->w = CopyWinding(source->w);
    tail = &(*tail)->next;
    source = source->next;
  }
  *tail = NULL;
  return head;
}

/* Native 0x436A70.  Splits the node at *slot by one caster and returns the
 * next list link to visit. */
PrimaryLightRegionNode_t **ClipPrimaryLightCasters(PrimaryLightRegionNode_t *caster, PrimaryLightRegionNode_t **slot)
{
  Winding_t *pieces[1026];
  Winding_t *frontWinding;
  Winding_t *backWinding;
  Winding_t *workingWinding;
  PrimaryLightRegionNode_t *region;
  PrimaryLightRegionNode_t *newNode;
  float edgeNormal[3];
  float coneDot;
  unsigned int pointIndex;
  int previousPointIndex;
  int pieceCount;
  int side;

  region = *slot;
  coneDot = DotProduct(caster->dir, region->dir);
  if (coneDot < PrimaryLightConeProjection(caster->cosHalfFov, region->cosHalfFov) - 0.001000000047497451f)
    return &region->next;

  side = PrimaryLightRegionClipWinding(region->w, caster->plane, caster->plane[3], 0.009999999776482582f, &frontWinding, &backWinding);
  if (side == SIDE_FRONT || side == SIDE_ON)
    return &region->next;

  if (side == SIDE_BACK)
  {
    workingWinding = region->w;
    pieceCount = 0;
  }
  else
  {
    AssertFatal(side == SIDE_CROSS, s_assertDisable_ClipPrimaryLightCastersSide);
    workingWinding = backWinding;
    pieces[0] = frontWinding;
    pieceCount = 1;
  }

  Assert(caster->w->numpoints <= PRIMARY_LIGHT_REGION_MAX_WINDING_POINTS, s_assertDisable_ClipPrimaryLightCastersPointCount);
  previousPointIndex = caster->w->numpoints - 1;
  for (pointIndex = 0; pointIndex < (unsigned int)caster->w->numpoints; ++pointIndex)
  {
    CrossProduct(caster->w->points[pointIndex], caster->w->points[previousPointIndex], edgeNormal);
    if (VecNormalize(edgeNormal) != 0.0)
    {
      side = PrimaryLightRegionClipWinding(workingWinding, edgeNormal, 0.0f, 0.009999999776482582f, &pieces[pieceCount], &backWinding);
      if (side != SIDE_BACK)
      {
        if (workingWinding != region->w)
          FreeWinding(workingWinding);

        if (side != SIDE_CROSS)
        {
          while (pieceCount)
            FreeWinding(pieces[--pieceCount]);
          return &region->next;
        }

        workingWinding = backWinding;
        if (PrimaryLightRegionSmallestEdgeDistance(pieces[pieceCount], region->plane) >= 0.02000999823212624f)
          ++pieceCount;
        else
          FreeWinding(pieces[pieceCount]);
      }
    }
    previousPointIndex = pointIndex;
  }

  if (workingWinding != region->w)
    FreeWinding(workingWinding);

  *slot = region->next;
  while (pieceCount)
  {
    --pieceCount;
    newNode = AllocPrimaryLightRegionNode();
    newNode->next = *slot;
    *slot = newNode;
    slot = &newNode->next;
    newNode->sourceSurface = region->sourceSurface;
    newNode->w = pieces[pieceCount];
    Vector4Copy(region->plane, newNode->plane);
    BuildHullLightCone(newNode);
  }
  DestroyPrimaryLightRegionNode(region);
  return slot;
}

/* Native 0x4371E0. */
static int ComparePrimaryLightConeAngle(const PrimaryLightRegionNode_t *first, const PrimaryLightRegionNode_t *second)
{
  return second->cosHalfFov > first->cosHalfFov;
}

/* Native 0x437090.  Alternating-run merge sort, descending cone cosine. */
static PrimaryLightRegionNode_t **MergeSortPrimaryLightHullList(PrimaryLightRegionNode_t **list)
{
  PrimaryLightRegionNode_t *runs[2];
  PrimaryLightRegionNode_t *runHeads[2];
  PrimaryLightRegionNode_t **tail;
  int runIndex;
  int takeRun;

  runs[0] = NULL;
  runs[1] = NULL;
  runIndex = 0;
  runHeads[0] = *list;
  while (runHeads[runIndex])
  {
    runHeads[1 - runIndex] = runHeads[runIndex]->next;
    runHeads[runIndex]->next = runs[runIndex];
    runs[runIndex] = runHeads[runIndex];
    runIndex = 1 - runIndex;
  }

  if (runs[0]->next)
  {
    MergeSortPrimaryLightHullList(&runs[0]);
    if (runs[1]->next)
      MergeSortPrimaryLightHullList(&runs[1]);
  }

  runHeads[0] = runs[0];
  runHeads[1] = runs[1];
  tail = list;
  takeRun = !ComparePrimaryLightConeAngle(runs[0], runs[1]);
  for (;;)
  {
    *tail = runHeads[takeRun];
    tail = &(*tail)->next;
    runHeads[takeRun] = *tail;
    if (!runHeads[takeRun])
      break;
    if (ComparePrimaryLightConeAngle(runHeads[1 - takeRun], runHeads[takeRun]))
      takeRun = 1 - takeRun;
  }
  *tail = runHeads[1 - takeRun];
  return tail;
}

/* Native 0x437060. */
static PrimaryLightRegionNode_t **ConditionalSortPrimaryLightHullList(PrimaryLightRegionNode_t **list)
{
  if (*list && (*list)->next)
    return MergeSortPrimaryLightHullList(list);
  return list;
}

/* Native 0x437210. */
static PrimaryLightRegionNode_t **ClipHullListAgainstCaster(PrimaryLightRegionNode_t *casters, PrimaryLightRegionNode_t **list)
{
  PrimaryLightRegionNode_t *caster;
  PrimaryLightRegionNode_t **slot;

  slot = (PrimaryLightRegionNode_t **)casters;
  for (caster = casters; caster; caster = caster->next)
  {
    for (slot = list; *slot; slot = ClipPrimaryLightCasters(caster, slot))
      ;
  }
  return slot;
}

/* Native 0x437260.  The native routine reduces every projected 64-point
 * chunk through com_convexhull before it emits the replacement winding.
 * WindingFromWindingList is the already-ported native bounded reducer for
 * this same coplanar winding representation. */
PrimaryLightRegionNode_t *MergeCoplanarPrimaryLightHulls(PrimaryLightRegionNode_t *list)
{
  PrimaryLightRegionNode_t *node;
  WindingList_t *windingList;
  WindingList_t **tail;
  Winding_t *merged;
  float plane[4];

  if (!list || !list->next)
    return list;

  windingList = NULL;
  tail = &windingList;
  for (node = list; node; node = node->next)
  {
    *tail = AllocWindingListNode();
    (*tail)->winding = node->w;
    tail = &(*tail)->next;
  }

  merged = WindingFromWindingList(list->plane, windingList);
  while (windingList)
  {
    WindingList_t *next = windingList->next;
    FreeWindingListNode(windingList);
    windingList = next;
  }

  if (!merged || !WindingHasPlane(merged, plane))
  {
    FreePrimaryLightHullList(list);
    if (merged)
      FreeWinding(merged);
    return NULL;
  }

  FreePrimaryLightHullList(list->next);
  list->next = NULL;
  FreeWinding(list->w);
  if (DotProduct(plane, list->plane) >= 0.0f)
  {
    list->w = merged;
  }
  else
  {
    list->w = ReverseWinding(merged);
    FreeWinding(merged);
  }
  return list;
}

/* Native 0x4375D0. */
void ClipRegionByPrimaryLightCone(PrimaryLightRegionNode_t **list, const PrimaryLightRegionDesc_t *light)
{
  float plane[4];

  if (light->type != 3 && light->cosHalfFov > -1.0f)
  {
    if (light->cosHalfFov < 0.0f)
    {
      VectorCopy(light->dir, plane);
      plane[3] = light->cosHalfFov * light->radius;
      ClipHullListByPlane(list, plane);
    }
    else
    {
      DispatchPrimaryLightConeClip(list, light->dir, light->cosHalfFov);
    }
  }
}

/* Native 0x437950. */
void ClipHullListByCasterWinding(PrimaryLightRegionNode_t **list, PrimaryLightRegionNode_t *caster)
{
  float plane[4];
  int previous;
  unsigned int pointIndex;

  plane[0] = -caster->plane[0];
  plane[1] = -caster->plane[1];
  plane[2] = -caster->plane[2];
  plane[3] = -caster->plane[3];
  ClipHullListByPlane(list, plane);

  previous = caster->w->numpoints - 1;
  for (pointIndex = 0; pointIndex < (unsigned int)caster->w->numpoints; ++pointIndex)
  {
    CrossProduct(caster->w->points[previous], caster->w->points[pointIndex], plane);
    VecNormalize(plane);
    plane[3] = DotProduct(caster->w->points[pointIndex], plane);
    ClipHullListByPlane(list, plane);
    previous = pointIndex;
  }
}

/* Native 0x437660. */
void DispatchPrimaryLightConeClip(PrimaryLightRegionNode_t **list, const float *dir, float cosHalfFov)
{
  PrimaryLightRegionNode_t **slot;
  float sinHalfFov;

  sinHalfFov = sqrtf(1.0f - cosHalfFov * cosHalfFov);
  for (slot = list; *slot; slot = ClipOneHullByCone(dir, sinHalfFov, cosHalfFov, slot))
    ;
}

/* Native 0x4378B0. */
void ClipHullListByPlane(PrimaryLightRegionNode_t **list, const float *plane)
{
  PrimaryLightRegionNode_t **slot;

  for (slot = list; *slot; slot = ClipOneHullByPlane(plane, slot))
    ;
}

/* Native 0x437C30. */
void PrimaryLightProjectionExtent(PrimaryLightRegionNode_t *list, PrimaryLightRegionNode_t *caster, const float *axis, float *outMin, float *outMax)
{
  float projection;
  float minProjection;
  float maxProjection;
  unsigned int pointIndex;

  if (caster)
  {
    minProjection = DotProduct(caster->w->points[0], axis);
    maxProjection = minProjection;
    for (pointIndex = 1; pointIndex < (unsigned int)caster->w->numpoints; ++pointIndex)
    {
      projection = DotProduct(caster->w->points[pointIndex], axis);
      if (projection >= minProjection)
      {
        if (projection > maxProjection)
          maxProjection = projection;
      }
      else
      {
        minProjection = projection;
      }
    }
  }
  else
  {
    minProjection = 0.0f;
    maxProjection = 0.0f;
  }

  for (; list; list = list->next)
  {
    for (pointIndex = 0; pointIndex < (unsigned int)list->w->numpoints; ++pointIndex)
    {
      projection = DotProduct(list->w->points[pointIndex], axis);
      if (projection >= minProjection)
      {
        if (projection > maxProjection)
          maxProjection = projection;
      }
      else
      {
        minProjection = projection;
      }
    }
  }
  *outMin = minProjection;
  *outMax = maxProjection;
}

/* Native 0x437D80. */
unsigned int CollectPrimaryLightKdopAxes(PrimaryLightRegionNode_t *list, PrimaryLightRegionNode_t *caster, const PrimaryLightRegionDesc_t *light, float (*axes)[5])
{
  PrimaryLightRegionNode_t *node;
  float plane[4];
  unsigned int axisCount;

  axisCount = AddBasePrimaryLightKdopAxes(axes, 0);
  for (node = list; node; node = node->next)
  {
    WindingHasPlane(node->w, plane);
    if (plane[3] <= 0.0f)
      axisCount = InsertPrimaryLightCandidateDirection(plane, axes, axisCount);
  }
  if (caster)
    return AddPrimaryLightCasterEdgeAxes(caster, axes, axisCount);
  return AddPrimaryLightConeAxes(light, axes, axisCount);
}

/* Native 0x438240. */
unsigned int SelectPrimaryLightKdopAxes(unsigned int *kdop, float (*axes)[5], unsigned int axisCount)
{
  unsigned int axisIndex;
  unsigned int selected;
  float baselineMetric;
  float bestMetric;
  float candidateMetric;

  baselineMetric = PrimaryLightKdopAreaMetric((const float (*)[5])(kdop + 1), kdop[0]);
  selected = axisCount;
  bestMetric = baselineMetric * 0.9990000128746033f;
  for (axisIndex = 0; axisIndex < axisCount; ++axisIndex)
  {
    memcpy(kdop + 5 * kdop[0] + 1, axes[axisIndex], sizeof(axes[0]));
    candidateMetric = PrimaryLightKdopAreaMetric((const float (*)[5])(kdop + 1), kdop[0] + 1);
    if (candidateMetric < bestMetric)
    {
      bestMetric = candidateMetric;
      selected = axisIndex;
    }
  }
  return selected;
}

/* Native 0x4376C0. */
PrimaryLightRegionNode_t **ClipOneHullByCone(const float *dir, float sinHalfFov, float cosHalfFov, PrimaryLightRegionNode_t **slot)
{
  PrimaryLightRegionNode_t *node;
  Winding_t *clipped;
  float directionCosine;
  float pointCosine;
  float perpendicular[3];
  float perpendicularLength;
  float clipValue;
  float plane[3];
  unsigned int pointIndex;

  node = *slot;
  directionCosine = DotProduct(node->dir, dir);
  if (directionCosine < PrimaryLightConeProjection(node->cosHalfFov, cosHalfFov))
    goto remove_node;
  if (cosHalfFov >= PrimaryLightConeProjection(node->cosHalfFov, directionCosine))
    return &node->next;

  clipped = CopyWinding(node->w);
  for (pointIndex = 0; pointIndex < (unsigned int)node->w->numpoints; ++pointIndex)
  {
    pointCosine = DotProduct(node->w->points[pointIndex], dir);
    VectorMA(node->w->points[pointIndex], -pointCosine, dir, perpendicular);
    perpendicularLength = VectorLength(perpendicular);
    clipValue = pointCosine * sinHalfFov + perpendicularLength * cosHalfFov;
    if (clipValue > 0.0f)
    {
      VectorScale(dir, -sinHalfFov, plane);
      VectorMA(plane, -cosHalfFov / perpendicularLength, perpendicular, plane);
      ClipWindingByPlane(&clipped, plane, 0.0f, 0.1f);
      if (!clipped)
        break;
    }
  }
  if (clipped)
  {
    FreeWinding(node->w);
    node->w = clipped;
    return &node->next;
  }

remove_node:
  *slot = node->next;
  DestroyPrimaryLightRegionNode(node);
  return slot;
}

/* Native 0x4378E0. */
PrimaryLightRegionNode_t **ClipOneHullByPlane(const float *plane, PrimaryLightRegionNode_t **slot)
{
  PrimaryLightRegionNode_t *node;

  node = *slot;
  ClipWindingByPlane(&node->w, (float *)plane, plane[3], 0.009999999776482582f);
  if (node->w)
    return &node->next;

  *slot = node->next;
  DestroyPrimaryLightRegionNode(node);
  return slot;
}

/* Native 0x437E30. */
unsigned int InsertPrimaryLightCandidateDirection(const float *dir, float (*axes)[5], unsigned int axisCount)
{
  unsigned int axisIndex;

  for (axisIndex = 0; axisIndex < axisCount; ++axisIndex)
  {
    if (fabsf(DotProduct(dir, axes[axisIndex])) > 0.9800000190734863f)
      return axisCount;
  }
  Assert(axisCount < 1024, s_assertDisable_PrimaryLightCandidateDirectionLimit);
  VectorCopy(dir, axes[axisCount]);
  return axisCount + 1;
}

/* Native 0x437ED0. */
unsigned int AddPrimaryLightCasterEdgeAxes(PrimaryLightRegionNode_t *caster, float (*axes)[5], unsigned int axisCount)
{
  float axis[3];
  unsigned int pointIndex;
  int previous;

  axis[0] = -caster->plane[0];
  axis[1] = -caster->plane[1];
  axis[2] = -caster->plane[2];
  axisCount = InsertPrimaryLightCandidateDirection(axis, axes, axisCount);
  previous = caster->w->numpoints - 1;
  for (pointIndex = 0; pointIndex < (unsigned int)caster->w->numpoints; ++pointIndex)
  {
    CrossProduct(caster->w->points[pointIndex], caster->w->points[previous], axis);
    VecNormalize(axis);
    axisCount = InsertPrimaryLightCandidateDirection(axis, axes, axisCount);
    previous = pointIndex;
  }
  return axisCount;
}

/* Native 0x437F90. */
unsigned int AddPrimaryLightConeAxes(const PrimaryLightRegionDesc_t *light, float (*axes)[5], unsigned int axisCount)
{
  float scaledDirection[3];
  float perpendicular[3];
  float side[3];
  float current[3];
  float previous[3];
  float axis[3];
  float cosine;
  float sine;
  float sinHalfFov;
  float scale;
  unsigned int sampleIndex;

  if (light->type == 3)
    return axisCount;

  axisCount = InsertPrimaryLightCandidateDirection(light->dir, axes, axisCount);
  if (light->cosHalfFov < 0.10000000149011612f)
    return axisCount;

  sinHalfFov = sqrtf(1.0f - light->cosHalfFov * light->cosHalfFov);
  scale = cosf(0.39269909262657166f) * light->cosHalfFov / sinHalfFov;
  VectorScale(light->dir, -scale, scaledDirection);
  PerpendicularVector((float *)light->dir, perpendicular);
  CrossProduct(perpendicular, (float *)light->dir, side);

  cosine = cosf(-0.39269909262657166f);
  sine = sinf(-0.39269909262657166f);
  current[0] = scaledDirection[0] + cosine * side[0] + sine * perpendicular[0];
  current[1] = scaledDirection[1] + cosine * side[1] + sine * perpendicular[1];
  current[2] = scaledDirection[2] + cosine * side[2] + sine * perpendicular[2];
  for (sampleIndex = 0; sampleIndex < 8; ++sampleIndex)
  {
    VectorCopy(current, previous);
    cosine = cosf(((float)sampleIndex + 0.5f) * 0.78539818525314331f);
    sine = sinf(((float)sampleIndex + 0.5f) * 0.78539818525314331f);
    current[0] = scaledDirection[0] + cosine * side[0] + sine * perpendicular[0];
    current[1] = scaledDirection[1] + cosine * side[1] + sine * perpendicular[1];
    current[2] = scaledDirection[2] + cosine * side[2] + sine * perpendicular[2];
    CrossProduct(current, previous, axis);
    VecNormalize(axis);
    axisCount = InsertPrimaryLightCandidateDirection(axis, axes, axisCount);
  }
  return axisCount;
}

/* Native 0x438170. */
unsigned int AddBasePrimaryLightKdopAxes(float (*axes)[5], unsigned int axisCount)
{
  unsigned int directionIndex;

  Assert(axisCount == 0, s_assertDisable_BasePrimaryLightKdopAxisCount);
  for (directionIndex = 0; directionIndex < 26; directionIndex += 2)
  {
    Assert(DotProduct(s_primaryLightKdopDirs[directionIndex], s_primaryLightKdopDirs[directionIndex + 1]) <= -0.99998998641967773f, s_assertDisable_BasePrimaryLightKdopAxisPair);
    VectorCopy(s_primaryLightKdopDirs[directionIndex], axes[axisCount]);
    ++axisCount;
  }
  return axisCount;
}

typedef struct PrimaryLightKdopPoint_s {
  float point[3];
  int faceAxis[3];
} PrimaryLightKdopPoint_t;

/* Native 0x438650/0x438B20: solve the eight slab corners of an axis triple,
 * then retain only points inside every other k-DOP slab. */
static int PrimaryLightKdopTripleCorners(const float (*axes)[5], unsigned int axis0, unsigned int axis1, unsigned int axis2, float corners[8][3])
{
  const float *a = axes[axis0];
  const float *b = axes[axis1];
  const float *c = axes[axis2];
  float crossBC[3], crossCA[3], crossAB[3];
  float determinant;
  unsigned int cornerIndex;

  CrossProduct((float *)b, (float *)c, crossBC);
  determinant = DotProduct(a, crossBC);
  if (fabsf(determinant) < 0.001000000047497451f)
    return 0;
  CrossProduct((float *)c, (float *)a, crossCA);
  CrossProduct((float *)a, (float *)b, crossAB);

  for (cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
  {
    float distanceA = a[3] + ((cornerIndex & 1) ? a[4] : -a[4]);
    float distanceB = b[3] + ((cornerIndex & 2) ? b[4] : -b[4]);
    float distanceC = c[3] + ((cornerIndex & 4) ? c[4] : -c[4]);
    corners[cornerIndex][0] = (distanceA * crossBC[0] + distanceB * crossCA[0] + distanceC * crossAB[0]) / determinant;
    corners[cornerIndex][1] = (distanceA * crossBC[1] + distanceB * crossCA[1] + distanceC * crossAB[1]) / determinant;
    corners[cornerIndex][2] = (distanceA * crossBC[2] + distanceB * crossCA[2] + distanceC * crossAB[2]) / determinant;
  }
  return 1;
}

static int PrimaryLightKdopPointInside(const float (*axes)[5], unsigned int axisCount, const float *point)
{
  unsigned int axisIndex;

  for (axisIndex = 0; axisIndex < axisCount; ++axisIndex)
  {
    if (fabsf(DotProduct(point, axes[axisIndex]) - axes[axisIndex][3]) - axes[axisIndex][4] > 0.001000000047497451f)
      return 0;
  }
  return 1;
}

static unsigned int PrimaryLightKdopEnumeratePoints(const float (*axes)[5], unsigned int axisCount, PrimaryLightKdopPoint_t *points)
{
  unsigned int axis0, axis1, axis2, cornerIndex;
  unsigned int pointCount = 0;

  for (axis2 = 2; axis2 < axisCount; ++axis2)
  {
    for (axis1 = 1; axis1 < axis2; ++axis1)
    {
      for (axis0 = 0; axis0 < axis1; ++axis0)
      {
        float corners[8][3];
        if (!PrimaryLightKdopTripleCorners(axes, axis0, axis1, axis2, corners))
          continue;
        for (cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
        {
          if (PrimaryLightKdopPointInside(axes, axisCount, corners[cornerIndex]))
          {
            Assert(pointCount < 5440, s_assertDisable_PrimaryLightCandidateDirectionLimit);
            VectorCopy(corners[cornerIndex], points[pointCount].point);
            points[pointCount].faceAxis[0] = (cornerIndex & 1) + 2 * axis0;
            points[pointCount].faceAxis[1] = ((cornerIndex >> 1) & 1) + 2 * axis1;
            points[pointCount].faceAxis[2] = ((cornerIndex >> 2) & 1) + 2 * axis2;
            ++pointCount;
          }
        }
      }
    }
  }
  return pointCount;
}

static Winding_t *PrimaryLightKdopFaceWinding(const float *normal, vec3_t *points, unsigned int pointCount)
{
  Winding_t *first;
  Winding_t *second;
  WindingList_t list[2];
  unsigned int firstCount;

  if (pointCount <= 64)
    return PrimaryLightRegionWindingFromPoints(normal, points, (int)pointCount);

  firstCount = pointCount / 2;
  first = PrimaryLightKdopFaceWinding(normal, points, firstCount);
  second = PrimaryLightKdopFaceWinding(normal, points + firstCount, pointCount - firstCount);
  if (!first)
    return second;
  if (!second)
    return first;

  list[0].winding = first;
  list[0].next = &list[1];
  list[1].winding = second;
  list[1].next = NULL;
  first = WindingFromWindingList(normal, list);
  FreeWinding(second);
  FreeWinding(list[0].winding);
  return first;
}

static unsigned int PrimaryLightKdopBuildFaces(const float (*axes)[5], unsigned int axisCount, Winding_t **faces)
{
  PrimaryLightKdopPoint_t points[5440];
  vec3_t facePoints[136];
  unsigned int pointCount;
  unsigned int faceIndex;
  unsigned int outputCount = 0;

  pointCount = PrimaryLightKdopEnumeratePoints(axes, axisCount, points);
  for (faceIndex = 0; faceIndex < 2 * axisCount; ++faceIndex)
  {
    float normal[3];
    unsigned int pointIndex;
    unsigned int facePointCount = 0;
    Winding_t *winding;

    for (pointIndex = 0; pointIndex < pointCount; ++pointIndex)
    {
      if (points[pointIndex].faceAxis[0] == (int)faceIndex || points[pointIndex].faceAxis[1] == (int)faceIndex || points[pointIndex].faceAxis[2] == (int)faceIndex)
      {
        Assert(facePointCount < 136, s_assertDisable_PrimaryLightCandidateDirectionLimit);
        VectorCopy(points[pointIndex].point, facePoints[facePointCount++]);
      }
    }
    if (facePointCount < 3)
      continue;

    VectorCopy(axes[faceIndex / 2], normal);
    if (faceIndex & 1)
    {
      normal[0] = -normal[0];
      normal[1] = -normal[1];
      normal[2] = -normal[2];
    }
    winding = PrimaryLightKdopFaceWinding(normal, facePoints, facePointCount);
    if (winding)
      faces[outputCount++] = winding;
  }
  return outputCount;
}

static float PrimaryLightKdopFaceVolume(Winding_t **faces, unsigned int faceCount)
{
  float center[3] = { 0.0f, 0.0f, 0.0f };
  float plane[4];
  float volume = 0.0f;
  unsigned int totalPoints = 0;
  unsigned int faceIndex;
  unsigned int pointIndex;

  for (faceIndex = 0; faceIndex < faceCount; ++faceIndex)
  {
    totalPoints += faces[faceIndex]->numpoints;
    for (pointIndex = 0; pointIndex < (unsigned int)faces[faceIndex]->numpoints; ++pointIndex)
      VectorAdd(center, faces[faceIndex]->points[pointIndex], center);
  }
  VectorScale(center, 1.0f / totalPoints, center);
  for (faceIndex = 0; faceIndex < faceCount; ++faceIndex)
  {
    float area = WindingPlaneAndArea(faces[faceIndex], plane);
    volume += area * (DotProduct(center, plane) - plane[3]);
  }
  return volume / 3.0f;
}

/* Native 0x438300. */
float PrimaryLightKdopAreaMetric(const float (*axes)[5], unsigned int axisCount)
{
  Winding_t *faces[34];
  unsigned int faceCount;
  unsigned int faceIndex;
  float metric;

  faceCount = PrimaryLightKdopBuildFaces(axes, axisCount, faces);
  metric = PrimaryLightKdopFaceVolume(faces, faceCount);
  for (faceIndex = 0; faceIndex < faceCount; ++faceIndex)
    FreeWinding(faces[faceIndex]);
  return metric;
}

/* Native 0x437A20. */
void EmitReducedPrimaryLightKdop(PrimaryLightRegionNode_t *list, PrimaryLightRegionNode_t *caster, const PrimaryLightRegionDesc_t *light, void *outKdop)
{
  float axes[1024][5];
  unsigned int *kdop;
  unsigned int axisCount;
  unsigned int maxAxisCount;
  unsigned int axisIndex;
  unsigned int selected;
  float minProjection;
  float maxProjection;

  kdop = (unsigned int *)outKdop;
  if (!list)
  {
    kdop[0] = 0;
    return;
  }

  axisCount = CollectPrimaryLightKdopAxes(list, caster, light, axes);
  Assert(axisCount >= 9, s_assertDisable_EmitReducedPrimaryLightKdopAxisCount);
  for (axisIndex = 0; axisIndex < axisCount; ++axisIndex)
  {
    PrimaryLightProjectionExtent(list, caster, axes[axisIndex], &minProjection, &maxProjection);
    axes[axisIndex][3] = (maxProjection + minProjection) * 0.5f;
    axes[axisIndex][4] = (maxProjection - minProjection) * 0.5f;
  }

  memcpy(kdop + 1, axes, 9 * sizeof(axes[0]));
  kdop[0] = 9;
  maxAxisCount = axisCount < 17 ? axisCount : 17;
  axisCount -= 9;
  memcpy(axes, &axes[axisCount], 9 * sizeof(axes[0]));

  while (kdop[0] != maxAxisCount)
  {
    selected = SelectPrimaryLightKdopAxes(kdop, axes, axisCount);
    if (selected == axisCount)
      break;
    memcpy(kdop + 5 * kdop[0] + 1, axes[selected], sizeof(axes[0]));
    ++kdop[0];
    --axisCount;
    memcpy(axes[selected], axes[axisCount], sizeof(axes[0]));
  }
}

/* Native 0x438F30. */
void *EmitPackedPrimaryLightRegion(void *kdopData)
{
  unsigned int *kdop;
  float *packed;
  unsigned int axisIndex;
  unsigned int extraAxisCount;

  kdop = (unsigned int *)kdopData;
  packed = (float *)malloc(20 * (kdop[0] - 9) + 76);
  if (!packed)
    Com_Error("Out of memory");

  for (axisIndex = 0; axisIndex < 3; ++axisIndex)
  {
    packed[axisIndex] = ((float *)kdop)[5 * axisIndex + 4];
    packed[axisIndex + 9] = ((float *)kdop)[5 * axisIndex + 5];
  }
  while (axisIndex < 9)
  {
    packed[axisIndex] = ((float *)kdop)[5 * axisIndex + 4] / 0.7071067690849304f;
    packed[axisIndex + 9] = ((float *)kdop)[5 * axisIndex + 5] / 0.7071067690849304f;
    ++axisIndex;
  }

  extraAxisCount = kdop[0] - 9;
  ((unsigned int *)packed)[18] = extraAxisCount;
  memcpy(packed + 19, kdop + 46, 20 * extraAxisCount);
  return packed;
}

/* Native 0x436DE0. */
int DecomposeConvexPrimaryLightRegion(PrimaryLightRegionNode_t **list, const PrimaryLightRegionDesc_t *light, int hullLimit, void **outHulls)
{
  unsigned int kdop[87];
  PrimaryLightRegionNode_t *copy;
  PrimaryLightRegionNode_t *caster;
  PrimaryLightRegionNode_t *mergedCaster;
  PrimaryLightRegionNode_t **slot;
  int hullCount;

  Assert(hullLimit >= 1, s_assertDisable_DecomposeConvexPrimaryLightRegionHullLimit);
  ConditionalSortPrimaryLightHullList(list);

  copy = CopyPrimaryLightHullList(*list);
  ClipHullListAgainstCaster(*list, &copy);
  ClipRegionByPrimaryLightCone(&copy, light);
  EmitReducedPrimaryLightKdop(copy, NULL, light, kdop);
  FreePrimaryLightHullList(copy);
  if (kdop[0])
  {
    *outHulls = EmitPackedPrimaryLightRegion(kdop);
    hullCount = 1;
  }
  else
  {
    hullCount = 0;
  }

  slot = list;
  while (*slot)
  {
    if ((*slot)->exterior)
    {
      slot = &(*slot)->next;
    }
    else
    {
      caster = *slot;
      *slot = caster->next;
      caster->next = NULL;

      ClipHullListAgainstCaster(*list, &caster);
      ClipRegionByPrimaryLightCone(&caster, light);
      mergedCaster = MergeCoplanarPrimaryLightHulls(caster);
      if (mergedCaster)
      {
        copy = CopyPrimaryLightHullList(*list);
        ClipHullListAgainstCaster(*list, &copy);
        ClipHullListByCasterWinding(&copy, mergedCaster);
        EmitReducedPrimaryLightKdop(copy, mergedCaster, light, kdop);
        FreePrimaryLightHullList(copy);
        DestroyPrimaryLightRegionNode(mergedCaster);
        if (kdop[0])
        {
          if (hullCount == hullLimit)
            Com_Error("More than %i convex regions affected by primary light at %g %g %g\n", hullLimit, light->origin[0], light->origin[1], light->origin[2]);
          outHulls[hullCount++] = EmitPackedPrimaryLightRegion(kdop);
        }
      }
    }
  }
  return hullCount;
}

void FreePrimaryLightHullList(PrimaryLightRegionNode_t *list)
{
  PrimaryLightRegionNode_t *node;

  while (list)
  {
    node = list;
    list = list->next;
    DestroyPrimaryLightRegionNode(node);
  }
}

void DestroyPrimaryLightRegionNode(PrimaryLightRegionNode_t *node)
{
  if (node->w)
    FreeWinding(node->w);
  free(node);
}

void AppendClippedWindingHull(PrimaryLightRegionNode_t **outList, const PrimaryLightRegionDesc_t *light, Winding_t *winding, int exterior, void *sourceSurface)
{
  float plane[4];
  float lightToVertex[3];
  float clipNormal[3];
  float radiusSq;
  float vertexDistSq;
  Winding_t *lightWinding;
  Winding_t *reversed;
  int pointIndex;

  lightWinding = AllocWinding(winding->numpoints);
  lightWinding->numpoints = winding->numpoints;
  for (pointIndex = 0; pointIndex < winding->numpoints; ++pointIndex)
    VectorSubtract(winding->points[pointIndex], light->origin, lightWinding->points[pointIndex]);

  if (!WindingHasPlane(lightWinding, plane) || fabsf(plane[3]) < 0.001000000047497451f)
  {
    FreeWinding(lightWinding);
    return;
  }

  if (plane[3] > 0.0f)
  {
    if (exterior)
    {
      FreeWinding(lightWinding);
      return;
    }

    plane[0] = -plane[0];
    plane[1] = -plane[1];
    plane[2] = -plane[2];
    plane[3] = -plane[3];
    reversed = ReverseWinding(lightWinding);
    FreeWinding(lightWinding);
    lightWinding = reversed;
  }

  radiusSq = light->radius * light->radius;
  for (pointIndex = 0; pointIndex < winding->numpoints; ++pointIndex)
  {
    VectorSubtract(light->origin, winding->points[pointIndex], lightToVertex);
    vertexDistSq = DotProduct(lightToVertex, lightToVertex);
    if (vertexDistSq > radiusSq)
    {
      VectorScale(lightToVertex, 1.0f / sqrtf(vertexDistSq), clipNormal);
      ClipWindingByPlane(&lightWinding, clipNormal, -light->radius, 0.1f);
      if (!lightWinding)
        return;
    }
  }

  InsertPrimaryLightHullLink(outList, lightWinding, plane, exterior, sourceSurface);
}

void InsertPrimaryLightHullLink(PrimaryLightRegionNode_t **outList, Winding_t *winding, float *plane, int exterior, void *sourceSurface)
{
  PrimaryLightRegionNode_t *node;

  node = AllocPrimaryLightRegionNode();
  node->sourceSurface = sourceSurface;
  node->exterior = exterior;
  node->w = winding;
  Vector4Copy(plane, node->plane);
  node->next = *outList;
  *outList = node;
  BuildHullLightCone(node);
}

PrimaryLightRegionNode_t *AllocPrimaryLightRegionNode(void)
{
  PrimaryLightRegionNode_t *node;

  node = (PrimaryLightRegionNode_t *)malloc(sizeof(*node));
  if (!node)
    Com_Error("Out of memory");
  return node;
}

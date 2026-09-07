/* CoD4 primary-light extraction recovered from cod4map.exe. */

#include "cod4map.h"

#define MAX_MAP_PRIMARY_LIGHTS 255

/*
================
ProjectWindingToLightmapGrid

Projects each winding point through its lightmap S/T vectors.  The primary
light region path works in the native 1024-unit grid, rather than normalized
lightmap coordinates.

cod4map.exe: 0x42EB20
================
*/
void ProjectWindingToLightmapGrid(const Winding_t *winding, const float *lightmapVecs, float *outPoints)
{
  int pointIndex;

  for (pointIndex = 0; pointIndex < winding->numpoints; ++pointIndex)
  {
    const float *point = winding->points[pointIndex];

    outPoints[2 * pointIndex] =
        (DotProduct(point, lightmapVecs) + lightmapVecs[3]) * 1024.0f;
    outPoints[2 * pointIndex + 1] =
        (DotProduct(point, lightmapVecs + 4) + lightmapVecs[7]) * 1024.0f;
  }
}

/*
================
BuildLightmapGridToWorldTransform

Inverts the plane/S/T lightmap basis.  The resulting origin and two steps
map native 1024-grid coordinates back onto the source plane.

cod4map.exe: 0x42EBB0
================
*/
void BuildLightmapGridToWorldTransform(
    const float *plane,
    const float *lightmapVecs,
    float *outS,
    float *outT,
    float *outOrigin)
{
  const float *s = lightmapVecs;
  const float *t = lightmapVecs + 4;
  float determinant;
  float inverseDeterminant;
  float planeDistScale;

  determinant =
      (s[0] * t[1] - s[1] * t[0]) * plane[2]
    + (s[2] * t[0] - s[0] * t[2]) * plane[1]
    + (s[1] * t[2] - s[2] * t[1]) * plane[0];
  if (determinant == 0.0f)
  {
    Com_Error("singular lightmap coordinates");
    return;
  }

  inverseDeterminant = 1.0f / determinant;
  planeDistScale = plane[3] * inverseDeterminant;

  outS[0] = (t[1] * plane[2] - t[2] * plane[1]) * inverseDeterminant;
  outS[1] = (t[2] * plane[0] - t[0] * plane[2]) * inverseDeterminant;
  outS[2] = (t[0] * plane[1] - t[1] * plane[0]) * inverseDeterminant;

  outT[0] = (s[2] * plane[1] - s[1] * plane[2]) * inverseDeterminant;
  outT[1] = (s[0] * plane[2] - s[2] * plane[0]) * inverseDeterminant;
  outT[2] = (s[1] * plane[0] - s[0] * plane[1]) * inverseDeterminant;

  outOrigin[0] = (s[1] * t[2] - s[2] * t[1]) * planeDistScale;
  outOrigin[1] = (s[2] * t[0] - s[0] * t[2]) * planeDistScale;
  outOrigin[2] = (s[0] * t[1] - s[1] * t[0]) * planeDistScale;

  outOrigin[0] = -s[3] * outS[0] + outOrigin[0] - t[3] * outT[0];
  outOrigin[1] = -s[3] * outS[1] + outOrigin[1] - t[3] * outT[1];
  outOrigin[2] = -s[3] * outS[2] + outOrigin[2] - t[3] * outT[2];
  VectorScale(outS, 0.0009765625f, outS);
  VectorScale(outT, 0.0009765625f, outT);
}

DiskPrimaryLight_t bspPrimaryLights[MAX_MAP_PRIMARY_LIGHTS];
unsigned char bspLightRegions[MAX_MAP_PRIMARY_LIGHTS];
int numBspPrimaryLights;
static char s_assertDisable_DescribePrimaryLightIndex;
static char s_assertDisable_CheckedPrimaryLightByteCast;
static char s_assertDisable_PrimaryLightRegionSurfaceEligible;
static unsigned char CheckedPrimaryLightByteCast(int value);

/* Native 0x4337E0 tree node.  It is intentionally private to the
   primary-light region path and retains direct MapDrawSurf leaf ranges. */
typedef struct PrimaryLightSurfaceAabbNode_s {
  float              center[3];
  float              halfSize[3];
  MapDrawSurf_t     *firstSurface;
  int                surfaceCount;
  struct PrimaryLightSurfaceAabbNode_s *children;
  int                childCount;
} PrimaryLightSurfaceAabbNode_t;
typedef char primarylight_surfaceaabbnode_size_must_be_0x28[
    sizeof(PrimaryLightSurfaceAabbNode_t) == COD4MAP_NATIVE_LAYOUT(0x28, 0x38) ? 1 : -1];
static PrimaryLightSurfaceAabbNode_t *s_primaryLightSurfaceAabbNodes;

/* Native 0x434550/0x4344F0 K-DOP payload.  0x4360A0 owns allocation and
   population; these predicates deliberately only consume the packed record. */
typedef struct PrimaryLightHullAxis_s {
  float normal[3];
  float min;
  float extent;
} PrimaryLightHullAxis_t;
typedef char primarylight_hullaxis_size_must_be_0x14[
    sizeof(PrimaryLightHullAxis_t) == 0x14 ? 1 : -1];

typedef struct PrimaryLightKdopHull_s {
  float min[9];
  float extent[9];
  int axisCount;
  PrimaryLightHullAxis_t axes[];
} PrimaryLightKdopHull_t;
typedef char primarylight_kdophull_axes_offset_must_be_0x4c[
    offsetof(PrimaryLightKdopHull_t, axes) == 0x4c ? 1 : -1];

typedef struct PrimaryLightHullList_s {
  int hullCount;
  PrimaryLightKdopHull_t *hulls[];
} PrimaryLightHullList_t;

typedef struct PrimaryLightRegionOutput_s {
  int hullCount;
  PrimaryLightKdopHull_t *hulls[8];
} PrimaryLightRegionOutput_t;
typedef char primarylight_regionoutput_size_must_be_0x24[
    sizeof(PrimaryLightRegionOutput_t) == COD4MAP_NATIVE_LAYOUT(0x24, 0x48) ? 1 : -1];
static PrimaryLightRegionOutput_t s_primaryLightRegionOutput[MAX_MAP_PRIMARY_LIGHTS];

/* Native 0x432D90.  The first non-sentinel primary light is the sun when
   its on-disk type is one; region generation skips that light in this case. */
static int PrimaryLightRegionSurfaceEligible(void)
{
  Assert(numBspPrimaryLights > 0, s_assertDisable_PrimaryLightRegionSurfaceEligible);
  return numBspPrimaryLights > 1 && bspPrimaryLights[1].type == 1;
}

/* Native 0x4333D0.  Tests the six-float primary-light segment against one
   triangle, with the native absolute-value comparison before the barycentric
   edge tests. */
static int IntersectSegmentTriangle(float *segment,
                                    float *vert0,
                                    float *vert1,
                                    float *vert2)
{
  float segmentDelta[3];
  float edge01[3];
  float edge02[3];
  float normal[3];
  float toVert0[3];
  float segmentToVert[3];
  float planeEndDistance;
  float orientation;
  float startDistance;
  float edge01Distance;
  float edge02Distance;

  VectorSubtract(segment, segment + 3, segmentDelta);
  VectorSubtract(vert0, vert1, edge01);
  VectorSubtract(vert0, vert2, edge02);
  CrossProduct(edge02, edge01, normal);

  planeEndDistance = DotProduct(segment + 3, normal);
  orientation = 1.0f;
  if ( planeEndDistance >= 0.0f )
  {
    planeEndDistance = -planeEndDistance;
    orientation = -1.0f;
  }

  VectorSubtract(vert0, segment, toVert0);
  startDistance = fabsf(DotProduct(toVert0, normal));
  if ( -planeEndDistance <= startDistance )
    return 0;

  CrossProduct(segment + 3, toVert0, segmentToVert);
  edge01Distance = DotProduct(segmentToVert, edge01) * orientation;
  if ( edge01Distance > 0.0f || planeEndDistance > edge01Distance )
    return 0;

  edge02Distance = DotProduct(segmentToVert, edge02) * orientation;
  return edge02Distance >= 0.0f && planeEndDistance <= edge01Distance - edge02Distance;
}

/* Native 0x433330. */
static int IntersectTriangleSurface(float *segment, MapDrawSurf_t *surface)
{
  int index;

  for (index = 0; index < surface->u.indexed.indexCount; index += 3)
  {
    if (IntersectSegmentTriangle(segment,
                                 surface->verts[surface->u.indexed.indexes[index]].pos,
                                 surface->verts[surface->u.indexed.indexes[index + 1]].pos,
                                 surface->verts[surface->u.indexed.indexes[index + 2]].pos))
      return 1;
  }
  return 0;
}

/* Native 0x433550. */
static int IntersectPatchSurface(float *segment, MapDrawSurf_t *surface)
{
  Mesh_t *subdivided;
  Mesh_t *reduced;
  int column;
  int row;

  /* The native argument slots a3/a5 are not read by SubdivideMesh. */
  subdivided = SubdivideMesh(surface->u.patch.width, surface->u.patch.height, 0,
                             surface->verts, 0, (float)surface->u.patch.subdivLevel,
                             SUBDIV_NO_MIN_LENGTH, 0, 0);
  PutMeshOnCurve(subdivided->width, subdivided->height, subdivided->reserved, subdivided->verts);
  reduced = RemoveLinearMeshColumnsRows(subdivided, 0, 0);
  FreeMesh(subdivided);

  for (column = 0; column < reduced->width - 1; ++column)
  {
    for (row = 0; row < reduced->height - 1; ++row)
    {
      const int bottomLeft = column + reduced->width * (row + 1);
      const int bottomRight = reduced->width * (row + 1) + column + 1;
      const int topRight = reduced->width * row + column + 1;
      const int topLeft = reduced->width * row + column;

      if (IntersectSegmentTriangle(segment,
                                   reduced->verts[bottomLeft].pos,
                                   reduced->verts[bottomRight].pos,
                                   reduced->verts[topRight].pos)
          || IntersectSegmentTriangle(segment,
                                      reduced->verts[topRight].pos,
                                      reduced->verts[topLeft].pos,
                                      reduced->verts[bottomLeft].pos))
      {
        FreeMesh(reduced);
        return 1;
      }
    }
  }

  FreeMesh(reduced);
  return 0;
}

/* Native 0x433770. */
static int IntersectWindingTerrainSurface(float *segment, MapDrawSurf_t *surface)
{
  int vertexIndex;

  for (vertexIndex = 2; vertexIndex < surface->vertCount; ++vertexIndex)
  {
    if (IntersectSegmentTriangle(segment,
                                 surface->verts[0].pos,
                                 surface->verts[vertexIndex - 1].pos,
                                 surface->verts[vertexIndex].pos))
      return 1;
  }
  return 0;
}

/* Native 0x4332E0. */
static int DispatchDrawSurfaceIntersection(float *segment, MapDrawSurf_t *surface)
{
  if (surface->isTerrain)
    return IntersectTriangleSurface(segment, surface);
  if (surface->isPatch)
    return IntersectPatchSurface(segment, surface);
  return IntersectWindingTerrainSurface(segment, surface);
}

/* Native 0x4338B0.  The caller builds this nine-float query directly on its
   stack: three position values, three direction-derived values, then three
   absolute-value bounds values.  Keep the native comparison ordering intact. */
static int SegmentIntersectsAabb(const float *query, const float *mins, const float *maxs)
{
  float deltaX;
  float deltaY;
  float deltaZ;
  float crossX;
  float crossY;
  float crossZ;

  deltaX = query[0] - mins[0];
  if ( query[6] + maxs[0] < fabsf(deltaX) )
    return 0;
  deltaY = query[1] - mins[1];
  if ( query[7] + maxs[1] < fabsf(deltaY) )
    return 0;
  deltaZ = query[2] - mins[2];
  if ( query[8] + maxs[2] < fabsf(deltaZ) )
    return 0;

  crossX = query[4] * deltaZ - query[5] * deltaY;
  if ( maxs[1] * query[8] + maxs[2] * query[7] < fabsf(crossX) )
    return 0;
  crossY = query[5] * deltaX - query[3] * deltaZ;
  if ( maxs[2] * query[6] + maxs[0] * query[8] < fabsf(crossY) )
    return 0;
  crossZ = query[3] * deltaY - query[4] * deltaX;
  return maxs[0] * query[7] + maxs[1] * query[6] >= fabsf(crossZ);
}

/* Native 0x4337E0. */
static MapDrawSurf_t *TraversePrimaryLightAabb(PrimaryLightSurfaceAabbNode_t *node, float *segment)
{
  int index;
  MapDrawSurf_t *surface;

  if (!node->childCount)
  {
    for (index = 0; index < node->surfaceCount; ++index)
    {
      surface = node->firstSurface + index;
      if (DispatchDrawSurfaceIntersection(segment, surface))
        return surface;
    }
    return NULL;
  }

  for (index = 0; index < node->childCount; ++index)
  {
    PrimaryLightSurfaceAabbNode_t *child = node->children + index;

    /* Native 0x433869 deliberately re-tests the current node bounds for
       every child (a1 / a1+12), then visits the children in stored order. */
    if (SegmentIntersectsAabb(segment, node->center, node->halfSize))
    {
      surface = TraversePrimaryLightAabb(child, segment);
      if (surface)
        return surface;
    }
  }
  return NULL;
}

/* Native 0x432DF0.  Shared builder 0x465730 both partitions this prefix and
   permutes its 68-byte MapDrawSurf records.  The permutation is observable:
   0x4337E0 returns the first intersecting blocker in leaf order. */
static int BuildPrimaryLightSurfaceAabbTree(void)
{
  typedef struct PrimaryLightSurfacePair_s {
    MapDrawSurf_t native;
    DrawSurf_t expanded;
  } PrimaryLightSurfacePair_t;

  AabbTreeBuilder_t options;
  AabbTreeNode_t *genericNodes;
  PrimaryLightSurfacePair_t *surfacePairs;
  float (*surfaceMins)[3];
  float (*surfaceMaxs)[3];
  int nodeCount;
  int nodeIndex;
  int surfaceCount;
  int surfaceIndex;

  Assert(g_entities[0].firstDrawSurf == 0, s_assertDisable_PrimaryLightRegionSurfaceEligible);
  for (surfaceCount = 0;
       surfaceCount < numMapDrawSurfs
       && (g_nativeMapDrawSurfs[surfaceCount].material->toolFlagsWord & 0x70) == 0x10
       && (g_nativeMapDrawSurfs[surfaceCount].material->surfaceFlags & 0x40004) == 0;
       ++surfaceCount)
  {
  }
  if (!surfaceCount)
    Com_Error("ERROR: Map must have at least one visible non-sky surface\n");

  surfaceMins = (float (*)[3])malloc(sizeof(*surfaceMins) * surfaceCount);
  surfaceMaxs = (float (*)[3])malloc(sizeof(*surfaceMaxs) * surfaceCount);
  genericNodes = (AabbTreeNode_t *)malloc(sizeof(*genericNodes) * surfaceCount);
  surfacePairs = (PrimaryLightSurfacePair_t *)malloc(sizeof(*surfacePairs) * surfaceCount);
  if (!surfaceMins || !surfaceMaxs || !genericNodes || !surfacePairs)
    Com_Error("ERROR: out of memory");

  for (surfaceIndex = 0; surfaceIndex < surfaceCount; ++surfaceIndex)
  {
    MapDrawSurf_t *surface = &g_nativeMapDrawSurfs[surfaceIndex];
    int vertexIndex;

    surfacePairs[surfaceIndex].native = *surface;
    surfacePairs[surfaceIndex].expanded = g_drawSurfs[surfaceIndex];

    ClearBounds(surfaceMins[surfaceIndex], surfaceMaxs[surfaceIndex]);
    for (vertexIndex = 0; vertexIndex < surface->vertCount; ++vertexIndex)
      AddPointToBounds(surface->verts[vertexIndex].pos,
                       surfaceMins[surfaceIndex], surfaceMaxs[surfaceIndex]);
  }

  memset(&options, 0, sizeof(options));
  /* Native permutes its complete 0x44-byte surface records in place.  KIWI
     keeps donor-expanded DrawSurf records in a parallel array, so carry both
     representations through the one native partition.  A second partition
     would not preserve equal-key ordering. */
  options.itemData = surfacePairs;
  options.itemCount = surfaceCount;
  options.itemStride = sizeof(*surfacePairs);
  options.hasBoundsData = (void *)1;
  options.itemMins = (float *)surfaceMins;
  options.itemMaxs = (float *)surfaceMaxs;
  options.nodes = genericNodes;
  options.maxNodes = surfaceCount;
  options.minPartitionSize = 2;
  options.minLeafItems = 4;
  nodeCount = AabbBuildTree(&options);

  for (surfaceIndex = 0; surfaceIndex < surfaceCount; ++surfaceIndex)
  {
    g_nativeMapDrawSurfs[surfaceIndex] = surfacePairs[surfaceIndex].native;
    g_drawSurfs[surfaceIndex] = surfacePairs[surfaceIndex].expanded;
  }
  free(surfacePairs);

  free(s_primaryLightSurfaceAabbNodes);
  s_primaryLightSurfaceAabbNodes = (PrimaryLightSurfaceAabbNode_t *)malloc(
      sizeof(*s_primaryLightSurfaceAabbNodes) * nodeCount);
  if (!s_primaryLightSurfaceAabbNodes)
    Com_Error("ERROR: out of memory");

  for (nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
  {
    AabbTreeNode_t *source = &genericNodes[nodeIndex];
    PrimaryLightSurfaceAabbNode_t *destination =
        &s_primaryLightSurfaceAabbNodes[nodeIndex];
    float mins[3];
    float maxs[3];
    int itemIndex;

    Assert(source->itemCount > 0, s_assertDisable_PrimaryLightRegionSurfaceEligible);
    VectorCopy(surfaceMins[source->firstItem], mins);
    VectorCopy(surfaceMaxs[source->firstItem], maxs);
    for (itemIndex = 1; itemIndex < source->itemCount; ++itemIndex)
    {
      int surface = source->firstItem + itemIndex;

      AddBoundsToBounds(surfaceMins[surface], surfaceMaxs[surface], mins, maxs);
    }
    destination->center[0] = (mins[0] + maxs[0]) * 0.5f;
    destination->center[1] = (mins[1] + maxs[1]) * 0.5f;
    destination->center[2] = (mins[2] + maxs[2]) * 0.5f;
    VectorSubtract(maxs, destination->center, destination->halfSize);
    destination->firstSurface = &g_nativeMapDrawSurfs[source->firstItem];
    destination->surfaceCount = source->itemCount;
    destination->children = s_primaryLightSurfaceAabbNodes + source->firstChild;
    destination->childCount = source->childCount;
  }

  free(genericNodes);
  free(surfaceMaxs);
  free(surfaceMins);
  return surfaceCount;
}

/* Native 0x433240. */
static MapDrawSurf_t *QueryPrimaryLightSurfaceAabb(const float *start, const float *end,
                                                    MapDrawSurf_t *previousSurface)
{
  float segment[9];

  segment[0] = (start[0] + end[0]) * 0.5f;
  segment[1] = (start[1] + end[1]) * 0.5f;
  segment[2] = (start[2] + end[2]) * 0.5f;
  segment[3] = segment[0] - start[0];
  segment[4] = segment[1] - start[1];
  segment[5] = segment[2] - start[2];
  segment[6] = fabsf(segment[3]);
  segment[7] = fabsf(segment[4]);
  segment[8] = fabsf(segment[5]);
  if (previousSurface && DispatchDrawSurfaceIntersection(segment, previousSurface))
    return previousSurface;
  return TraversePrimaryLightAabb(s_primaryLightSurfaceAabbNodes, segment);
}

/* Native 0x434000. */
static MapDrawSurf_t *SelectPrimaryLightSegmentEndpoint(const DiskPrimaryLight_t *light,
                                                         const float *point,
                                                         MapDrawSurf_t *previousSurface)
{
  float endpoint[3];

  if (light->type != 1)
    return QueryPrimaryLightSurfaceAabb(point, light->origin, previousSurface);
  VectorMA(point, 262144.0f, light->dir, endpoint);
  return QueryPrimaryLightSurfaceAabb(point, endpoint, previousSurface);
}

/* Native 0x434070.  Tests the radius, point/spot cone, and movable-light
   extension of a primary light at one candidate world-space point. */
static int PrimaryLightInfluencesSurface(const DiskPrimaryLight_t *light, const float *point)
{
  float lightToPoint[3];
  float distanceSquared;
  float radiusExtent;
  float directionDot;
  float coneProjection;

  if ( light->type == 1 )
    return 1;

  VectorSubtract(light->origin, point, lightToPoint);
  distanceSquared = DotProduct(lightToPoint, lightToPoint);
  radiusExtent = light->radius + light->translationLimit;
  if ( distanceSquared > radiusExtent * radiusExtent )
    return 0;
  if ( light->type == 3 )
    return 1;

  if ( light->rotationLimit == 1.0f )
  {
    directionDot = DotProduct(lightToPoint, light->dir);
    return directionDot >= 0.0f
        && directionDot * directionDot >= light->cosHalfFovOuter * light->cosHalfFovOuter * distanceSquared;
  }
  if ( light->rotationLimit <= -light->cosHalfFovOuter )
    return 1;

  coneProjection = light->cosHalfFovOuter * light->rotationLimit
                 - sqrtf((1.0f - light->cosHalfFovOuter * light->cosHalfFovOuter)
                       * (1.0f - light->rotationLimit * light->rotationLimit));
  directionDot = DotProduct(lightToPoint, light->dir);
  return directionDot >= coneProjection * sqrtf(distanceSquared);
}

/* Native 0x434B20.  Tests a winding's projection interval against one
   projected primary-light hull interval. */
static int WindingSeparatesPrimaryLight(Winding_t *winding, float axisX, float axisY, float axisZ,
                                       float projectedPoint, float hullMin, float hullExtent)
{
  float axis[3];
  float minDist;
  float maxDist;

  axis[0] = axisX;
  axis[1] = axisY;
  axis[2] = axisZ;
  WindingPlaneDistExtent(winding, axis, projectedPoint, &minDist, &maxDist);
  return hullExtent + (maxDist - minDist) * 0.5f
      <= fabsf(hullMin - (maxDist + minDist) * 0.5f);
}

/* Native 0x434550.  The first nine intervals correspond to x, y, z, then
   the six pairwise diagonal axes.  Variable axes are retained as their
   packed 20-byte records and use the winding's exact projection helper. */
static int HullSeparatesPrimaryLight(Winding_t *winding,
                                    const float *referenceOrigin,
                                    const float *sourceHalfSize,
                                    const float *candidateCenter,
                                    const PrimaryLightKdopHull_t *hull)
{
  int axisIndex;
  float projectedPoint;
  float minDist;
  float maxDist;

  if (hull->extent[0] + sourceHalfSize[0] + 0.1f <= fabsf(hull->min[0] - referenceOrigin[0]))
    return 1;
  if (hull->extent[1] + sourceHalfSize[1] + 0.1f <= fabsf(hull->min[1] - referenceOrigin[1]))
    return 1;
  if (hull->extent[2] + sourceHalfSize[2] + 0.1f <= fabsf(hull->min[2] - referenceOrigin[2]))
    return 1;

  if (hull->extent[3] + sourceHalfSize[0] + sourceHalfSize[1] + 0.1f
      <= fabsf(hull->min[3] - referenceOrigin[0] - referenceOrigin[1]))
    return 1;
  if (hull->extent[4] + sourceHalfSize[0] + sourceHalfSize[1] + 0.1f
      <= fabsf(hull->min[4] - referenceOrigin[0] + referenceOrigin[1]))
    return 1;
  if (hull->extent[5] + sourceHalfSize[0] + sourceHalfSize[2] + 0.1f
      <= fabsf(hull->min[5] - referenceOrigin[0] - referenceOrigin[2]))
    return 1;
  if (hull->extent[6] + sourceHalfSize[0] + sourceHalfSize[2] + 0.1f
      <= fabsf(hull->min[6] - referenceOrigin[0] + referenceOrigin[2]))
    return 1;
  if (hull->extent[7] + sourceHalfSize[1] + sourceHalfSize[2] + 0.1f
      <= fabsf(hull->min[7] - referenceOrigin[1] - referenceOrigin[2]))
    return 1;
  if (hull->extent[8] + sourceHalfSize[1] + sourceHalfSize[2] + 0.1f
      <= fabsf(hull->min[8] - referenceOrigin[1] + referenceOrigin[2]))
    return 1;

  for (axisIndex = 0; axisIndex < hull->axisCount; ++axisIndex)
  {
    const PrimaryLightHullAxis_t *axis = &hull->axes[axisIndex];

    projectedPoint = DotProduct(axis->normal, candidateCenter);
    WindingPlaneDistExtent(winding, axis->normal, projectedPoint, &minDist, &maxDist);
    if (axis->extent + (maxDist - minDist) * 0.5f + 0.1f
        <= fabsf(axis->min - (maxDist + minDist) * 0.5f))
      return 1;
  }

  if (WindingSeparatesPrimaryLight(winding, 1.0f, 1.0f, 0.0f,
                                   candidateCenter[0] + candidateCenter[1], hull->min[3], hull->extent[3] + 0.1f))
    return 1;
  if (WindingSeparatesPrimaryLight(winding, 1.0f, -1.0f, 0.0f,
                                   candidateCenter[0] - candidateCenter[1], hull->min[4], hull->extent[4] + 0.1f))
    return 1;
  if (WindingSeparatesPrimaryLight(winding, 1.0f, 0.0f, 1.0f,
                                   candidateCenter[0] + candidateCenter[2], hull->min[5], hull->extent[5] + 0.1f))
    return 1;
  if (WindingSeparatesPrimaryLight(winding, 1.0f, 0.0f, -1.0f,
                                   candidateCenter[0] - candidateCenter[2], hull->min[6], hull->extent[6] + 0.1f))
    return 1;
  if (WindingSeparatesPrimaryLight(winding, 0.0f, 1.0f, 1.0f,
                                   candidateCenter[1] + candidateCenter[2], hull->min[7], hull->extent[7] + 0.1f))
    return 1;
  return WindingSeparatesPrimaryLight(winding, 0.0f, 1.0f, -1.0f,
                                      candidateCenter[1] - candidateCenter[2], hull->min[8], hull->extent[8] + 0.1f);
}

/* Native 0x4344F0. */
static int PrimaryLightConvexHullOcclusion(Winding_t *winding,
                                           const float *referenceOrigin,
                                           const float *sourceHalfSize,
                                           const float *candidateCenter,
                                           const PrimaryLightHullList_t *hulls)
{
  int hullIndex;

  for (hullIndex = 0; hullIndex < hulls->hullCount; ++hullIndex)
  {
    if (!HullSeparatesPrimaryLight(winding, referenceOrigin, sourceHalfSize,
                                   candidateCenter, hulls->hulls[hullIndex]))
      return 0;
  }
  return 1;
}

/* Native 0x46EB90. */
static float PrimaryLightDistanceSqToBounds(const float *point, const float *mins, const float *maxs)
{
  float distanceSquared = 0.0f;
  int axis;

  for (axis = 0; axis < 3; ++axis)
  {
    float delta = mins[axis] - point[axis];
    if (delta <= 0.0f)
    {
      delta = point[axis] - maxs[axis];
      if (delta > 0.0f)
        distanceSquared += delta * delta;
    }
    else
    {
      distanceSquared += delta * delta;
    }
  }
  return distanceSquared;
}

/* Native 0x4341E0.  This consumes the pre-lightmap TriSurf carrier directly:
   its winding, bounds, and props plane all retain native offsets. */
static int SurfaceRelevantToPrimaryLight(int primaryLightIndex, TriSurf_t *surface)
{
  const DiskPrimaryLight_t *light = &bspPrimaryLights[primaryLightIndex];
  float lightOriginRelative[3];
  float center[3];
  float halfSize[3];
  float coneExtent;
  float planeDistance;

  if (light->type == 1)
    return DotProduct(light->dir, surface->props->plane) > 0.0f;

  VectorSubtract(light->origin, g_entities[g_currentEntityIndex].origin, lightOriginRelative);
  planeDistance = DotProduct(lightOriginRelative, surface->props->plane) - surface->props->plane[3];
  if (planeDistance < 0.0f || light->radius < planeDistance)
    return 0;

  if (light->type == 3)
  {
    if (light->radius * light->radius
        < PrimaryLightDistanceSqToBounds(lightOriginRelative, surface->mins, surface->maxs))
      return 0;
  }
  else
  {
    Assert(light->type == 2, s_assertDisable_PrimaryLightRegionSurfaceEligible);
    coneExtent = PrimaryLightSpotConeExtent(light);
    if (coneExtent > 0.0f)
    {
      center[0] = (surface->mins[0] + surface->maxs[0]) * 0.5f;
      center[1] = (surface->mins[1] + surface->maxs[1]) * 0.5f;
      center[2] = (surface->mins[2] + surface->maxs[2]) * 0.5f;
      VectorSubtract(center, surface->mins, halfSize);
      if (CullBoxFromConicSectionOfSphere(lightOriginRelative, light->dir, coneExtent,
                                           light->radius, center, halfSize))
        return 0;
    }
    else
    {
      if (light->radius * light->radius
          < PrimaryLightDistanceSqToBounds(lightOriginRelative, surface->mins, surface->maxs))
        return 0;
      if (!WindingPlaneSide(surface->winding, (float *)light->dir,
                            DotProduct(light->origin, light->dir) - coneExtent * light->radius))
        return 0;
    }
  }

  center[0] = (surface->mins[0] + surface->maxs[0]) * 0.5f;
  center[1] = (surface->mins[1] + surface->maxs[1]) * 0.5f;
  center[2] = (surface->mins[2] + surface->maxs[2]) * 0.5f;
  VectorSubtract(center, surface->mins, halfSize);
  VectorSubtract(center, lightOriginRelative, center);
  return !PrimaryLightConvexHullOcclusion(surface->winding, center, halfSize,
                                           lightOriginRelative,
                                           (const PrimaryLightHullList_t *)&s_primaryLightRegionOutput[primaryLightIndex]);
}

/* Native 0x433A50 uses four 0x4004-vertex (two floats per vertex) scratch
   buffers.  Keep the native 512 KiB per-surface stack allocation: subdivision
   alternates these buffers while it visits each one-unit lightmap-grid cell. */
#define PRIMARY_LIGHT_GRID_BUFFER_FLOATS (0x4004 * 2)
#define PRIMARY_LIGHT_GRID_SCRATCH_FLOATS (4 * PRIMARY_LIGHT_GRID_BUFFER_FLOATS)

/* Native 0x433EA0.  At this pre-lightmap stage MapDrawSurf +12 is the
   primary-light assignment carrier: 256 means unassigned and 0 denotes a
   reported conflict.  It is later consumed/replaced by the lightmap route. */
static int AssignPrimaryLightToSurface(TriSurf_t *surface, const float *origin,
                                       int primaryLightIndex)
{
  TriSurfPropsSidecar_t *sidecar;
  MapDrawSurf_t *drawSurf;
  int assignedPrimaryLight;

  sidecar = TrisPropsSidecar_Get(surface->props);
  Assert(sidecar && sidecar->mapDrawSurf, s_assertDisable_PrimaryLightRegionSurfaceEligible);
  drawSurf = sidecar->mapDrawSurf;
  assignedPrimaryLight = drawSurf->lightmapIndex;

  if (assignedPrimaryLight == 256)
  {
    drawSurf->lightmapIndex = primaryLightIndex;
    return 1;
  }
  if (assignedPrimaryLight == primaryLightIndex)
    return 1;

  Error(0, origin, surface->props->plane, drawSurf->sourceInfo.mapInfoIndex,
        drawSurf->entityNum, drawSurf->sourceIndex.brushNum,
        "Surface '%s' is affected by more than one primary light: %s and %s",
        drawSurf->material->name, DescribePrimaryLight(assignedPrimaryLight),
        DescribePrimaryLight(primaryLightIndex));
  drawSurf->lightmapIndex = 0;
  return 0;
}

/* Native 0x433D50.  Once a primary light's segment to a grid-cell centroid
   becomes unobstructed, it is retired from this surface's candidate set so
   each remaining cell tests only still-occluded lights. */
typedef struct PrimaryLightGridContext_s {
  TriSurf_t     *surface;                              /* [0x000] */
  float          origin[3];                            /* [0x004] */
  float          stepS[3];                             /* [0x010] */
  float          stepT[3];                             /* [0x01C] */
  unsigned char  active[256];                          /* [0x028] */
  MapDrawSurf_t *lastBlockingSurface[MAX_MAP_PRIMARY_LIGHTS]; /* [0x128] */
} PrimaryLightGridContext_t;
typedef char primarylight_gridcontext_size_must_be_0x524[
    sizeof(PrimaryLightGridContext_t) == COD4MAP_NATIVE_LAYOUT(0x524, 0x928) ? 1 : -1];

static void AssignPrimaryLightsToGridCell(float logArea, float *centroid,
                                          float *coords, int vertCount,
                                          void *userData, int polygonIndex)
{
  PrimaryLightGridContext_t *context;
  TriSurfPropsSidecar_t *sidecar;
  MapDrawSurf_t *drawSurf;
  float point[3];
  int primaryLightIndex;

  (void)logArea;
  (void)coords;
  (void)vertCount;
  (void)polygonIndex;

  context = (PrimaryLightGridContext_t *)userData;
  sidecar = TrisPropsSidecar_Get(context->surface->props);
  Assert(sidecar && sidecar->mapDrawSurf, s_assertDisable_PrimaryLightRegionSurfaceEligible);
  drawSurf = sidecar->mapDrawSurf;
  if (!drawSurf->lightmapIndex)
    return;

  point[0] = context->origin[0] + centroid[0] * context->stepS[0] + centroid[1] * context->stepT[0];
  point[1] = context->origin[1] + centroid[0] * context->stepS[1] + centroid[1] * context->stepT[1];
  point[2] = context->origin[2] + centroid[0] * context->stepS[2] + centroid[1] * context->stepT[2];
  VectorMA(point, 0.1f, context->surface->props->plane, point);

  for (primaryLightIndex = 0; primaryLightIndex < numBspPrimaryLights; ++primaryLightIndex)
  {
    if (!context->active[primaryLightIndex])
      continue;
    if (!PrimaryLightInfluencesSurface(&bspPrimaryLights[primaryLightIndex], point))
      continue;

    context->lastBlockingSurface[primaryLightIndex] =
        SelectPrimaryLightSegmentEndpoint(&bspPrimaryLights[primaryLightIndex], point,
                                          context->lastBlockingSurface[primaryLightIndex]);
    if (context->lastBlockingSurface[primaryLightIndex])
      continue;

    context->active[primaryLightIndex] = 0;
    if (!AssignPrimaryLightToSurface(context->surface, point, primaryLightIndex))
      break;
  }
}

/* Native 0x433A50. */
static void AssignPrimaryLightsToTriSurf(TriSurf_t *surface)
{
  TriSurfPropsSidecar_t *sidecar;
  MapDrawSurf_t *drawSurf;
  float polygonBuffers[PRIMARY_LIGHT_GRID_SCRATCH_FLOATS];
  float mins[2];
  float maxs[2];
  PrimaryLightGridContext_t context;
  int pointIndex;
  int primaryLightIndex;
  int gridMinsX;
  int gridMinsY;
  int gridCountX;
  int gridCountY;

  sidecar = TrisPropsSidecar_Get(surface->props);
  Assert(sidecar && sidecar->mapDrawSurf, s_assertDisable_PrimaryLightRegionSurfaceEligible);
  drawSurf = sidecar->mapDrawSurf;
  if (!(drawSurf->material->gameFlags & 2) || !drawSurf->lightmapIndex)
    return;

  ProjectWindingToLightmapGrid(surface->winding, surface->props->lmapVecs, polygonBuffers);
  ClearBounds2D(mins, maxs);
  for (pointIndex = 0; pointIndex < surface->winding->numpoints; ++pointIndex)
    AddPointToBounds2D(polygonBuffers + 2 * pointIndex, mins, maxs);

  gridMinsX = (int)floorf(mins[0]);
  gridMinsY = (int)floorf(mins[1]);
  gridCountX = (int)ceilf(maxs[0]) - gridMinsX;
  gridCountY = (int)ceilf(maxs[1]) - gridMinsY;

  memset(&context, 0, sizeof(context));
  context.surface = surface;
  BuildLightmapGridToWorldTransform(surface->props->plane, surface->props->lmapVecs,
                                    context.stepS, context.stepT, context.origin);
  VectorAdd(context.origin, g_entities[g_currentEntityIndex].origin, context.origin);

  for (primaryLightIndex = 1; primaryLightIndex < numBspPrimaryLights; ++primaryLightIndex)
  {
    if (drawSurf->lightmapIndex != primaryLightIndex
        && SurfaceRelevantToPrimaryLight(primaryLightIndex, surface))
    {
      context.active[primaryLightIndex] = 1;
    }
  }

  Subdivide2DPolygonGrid(polygonBuffers, surface->winding->numpoints,
                          gridCountX, gridCountY, (float)gridMinsX, (float)gridMinsY,
                          1.0f, 1.0f, AssignPrimaryLightsToGridCell, &context);
}

/* Native 0x442410.  The primary-light conflict walk consumes the same
   transient per-visibility-cell TriSurf lists as the later shadow pass; it
   deliberately does not traverse GridTreeNode_t links. */
int AssignPrimaryLightsToTriSurfCells(void)
{
  int cellCount;
  int cellIndex;

  cellCount = g_currentEntityIndex <= 0 ? numBSPCells + numBSPCullGroups : 1;
  for (cellIndex = 0; cellIndex < cellCount; ++cellIndex)
  {
    TriSurf_t *surface;

    for (surface = triSurfCellArray[cellIndex]; surface; surface = surface->next)
      AssignPrimaryLightsToTriSurf(surface);
  }
  return cellCount;
}

#undef PRIMARY_LIGHT_GRID_SCRATCH_FLOATS
#undef PRIMARY_LIGHT_GRID_BUFFER_FLOATS

/* Native 0x435E20. */
static void BoundsForPrimaryLightSurface(int surfaceIndex, float *center, float *halfSize)
{
  MapDrawSurf_t *surface = &g_nativeMapDrawSurfs[surfaceIndex];
  float mins[3];
  float maxs[3];
  int vertexIndex;

  ClearBounds(mins, maxs);
  for (vertexIndex = 0; vertexIndex < surface->vertCount; ++vertexIndex)
    AddPointToBounds(surface->verts[vertexIndex].pos, mins, maxs);
  center[0] = (maxs[0] + mins[0]) * 0.5f;
  center[1] = (maxs[1] + mins[1]) * 0.5f;
  center[2] = (maxs[2] + mins[2]) * 0.5f;
  VectorSubtract(center, mins, halfSize);
}

/* Native 0x435B80. */
static int CullOnePrimaryLightSurface(const PrimaryLightRegionDesc_t *light, int surfaceIndex,
                                      const float *center, const float *halfSize, int outputCount,
                                      MapDrawSurf_t **outSurfaces)
{
  if (light->type == 3 || light->cosHalfFov < 0.0f)
  {
    if (CullBoxFromSphere(light->origin, light->radius, center, halfSize))
      return outputCount;
  }
  else if (CullBoxFromConicSectionOfSphere(light->origin, light->dir, light->cosHalfFov,
                                            light->radius, center, halfSize))
  {
    return outputCount;
  }
  outSurfaces[outputCount] = &g_nativeMapDrawSurfs[surfaceIndex];
  return outputCount + 1;
}

/* Native 0x435AC0. */
static int CullPrimaryLightCandidateSurfaces(const PrimaryLightRegionDesc_t *light,
                                             int directCount, int indexedCount, const int *indexes,
                                             const float *centers, const float *halfSizes,
                                             MapDrawSurf_t **outSurfaces)
{
  int outputCount = 0;
  int index;

  for (index = 0; index < directCount; ++index)
    outputCount = CullOnePrimaryLightSurface(light, index, centers + 3 * index,
                                              halfSizes + 3 * index, outputCount, outSurfaces);
  for (index = 0; index < indexedCount; ++index)
    outputCount = CullOnePrimaryLightSurface(light, indexes[index], centers + 3 * (index + directCount),
                                              halfSizes + 3 * (index + directCount), outputCount, outSurfaces);
  return outputCount;
}

/* Native 0x4354E0. */
static void BuildPatchMeshHull(const PrimaryLightRegionDesc_t *light, MapDrawSurf_t *surface,
                               int exterior, PrimaryLightRegionNode_t **outList)
{
  Mesh_t *subdivided = SubdivideMesh(surface->u.patch.width, surface->u.patch.height, 0,
                                     surface->verts, 0, (float)surface->u.patch.subdivLevel,
                                     SUBDIV_NO_MIN_LENGTH, 0, 0);
  Mesh_t *reduced;
  Winding_t *winding0;
  Winding_t *winding1;
  int column, row;

  PutMeshOnCurve(subdivided->width, subdivided->height, subdivided->reserved, subdivided->verts);
  reduced = RemoveLinearMeshColumnsRows(subdivided, 0, 0);
  FreeMesh(subdivided);
  winding0 = AllocWinding(4);
  winding1 = AllocWinding(3);

  for (column = 0; column < reduced->width - 1; ++column)
  {
    for (row = 0; row < reduced->height - 1; ++row)
    {
      const int bottomLeft = column + reduced->width * (row + 1);
      const int bottomRight = reduced->width * (row + 1) + column + 1;
      const int topRight = reduced->width * row + column + 1;
      const int topLeft = reduced->width * row + column;
      float plane0[4], plane1[4];
      int valid0, valid1;

      valid0 = PlaneFromPoints(plane0, reduced->verts[bottomLeft].pos,
                               reduced->verts[bottomRight].pos, reduced->verts[topRight].pos);
      if (valid0)
      {
        winding0->numpoints = 3;
        VectorCopy(reduced->verts[bottomLeft].pos, winding0->points[0]);
        VectorCopy(reduced->verts[bottomRight].pos, winding0->points[1]);
        VectorCopy(reduced->verts[topRight].pos, winding0->points[2]);
      }
      valid1 = PlaneFromPoints(plane1, reduced->verts[topRight].pos,
                               reduced->verts[topLeft].pos, reduced->verts[bottomLeft].pos);
      if (valid1)
      {
        winding1->numpoints = 3;
        VectorCopy(reduced->verts[topRight].pos, winding1->points[0]);
        VectorCopy(reduced->verts[topLeft].pos, winding1->points[1]);
        VectorCopy(reduced->verts[bottomLeft].pos, winding1->points[2]);
      }
      if (valid0 && valid1 && WindingsAreCoplanar(winding0, plane0, winding1, plane1))
      {
        VectorCopy(winding1->points[1], winding0->points[3]);
        winding0->numpoints = 4;
        valid1 = 0;
      }
      if (valid0)
        AppendClippedWindingHull(outList, light, winding0, exterior, surface);
      if (valid1)
        AppendClippedWindingHull(outList, light, winding1, exterior, surface);
    }
  }
  FreeWinding(winding0);
  FreeWinding(winding1);
  FreeMesh(reduced);
}

/* Native 0x43DD30/0x43DEA0, used by 0x435870 to recognize the adjacent
   triangle that can be represented as one four-point winding. */
static void BuildPrimaryLightTriangleEdgePlane(const float *point0, const float *point1,
                                               const float *trianglePlane, float *outPlane)
{
  float edge[3];

  VectorSubtract(point1, point0, edge);
  CrossProduct(trianglePlane, edge, outPlane);
  VecNormalize(outPlane);
  outPlane[3] = DotProduct(outPlane, point0);
}

static int IndexedMeshMergeVertexPassesTriangleTest(const float *point, const Winding_t *triangle,
                                                     const float *trianglePlane)
{
  float edgePlane[4];
  const float epsilon = 0.025f;

  if (fabsf(DotProduct(trianglePlane, point) - trianglePlane[3]) > epsilon)
    return 0;
  BuildPrimaryLightTriangleEdgePlane(triangle->points[0], triangle->points[1], trianglePlane, edgePlane);
  if (-epsilon > DotProduct(edgePlane, point) - edgePlane[3])
    return 0;
  BuildPrimaryLightTriangleEdgePlane(triangle->points[1], triangle->points[2], trianglePlane, edgePlane);
  if (epsilon < DotProduct(edgePlane, point) - edgePlane[3])
    return 0;
  BuildPrimaryLightTriangleEdgePlane(triangle->points[2], triangle->points[0], trianglePlane, edgePlane);
  return epsilon >= DotProduct(edgePlane, point) - edgePlane[3];
}

/* Native 0x435870. */
static void BuildIndexedMeshHull(const PrimaryLightRegionDesc_t *light, MapDrawSurf_t *surface,
                                 int exterior, PrimaryLightRegionNode_t **outList)
{
  Winding_t *winding = AllocWinding(4);
  int index;

  for (index = 0; index < surface->u.indexed.indexCount; index += 3)
  {
    float plane[4];
    int pointIndex;

    winding->numpoints = 3;
    for (pointIndex = 0; pointIndex < 3; ++pointIndex)
      VectorCopy(surface->verts[surface->u.indexed.indexes[index + pointIndex]].pos,
                 winding->points[pointIndex]);

    if (PlaneFromPoints(plane, winding->points[0], winding->points[1], winding->points[2]))
    {
      if (index + 3 < surface->u.indexed.indexCount
          && surface->u.indexed.indexes[index] == surface->u.indexed.indexes[index + 4]
          && surface->u.indexed.indexes[index + 1] == surface->u.indexed.indexes[index + 3])
      {
        const int mergeVertex = surface->u.indexed.indexes[index + 5];
        if (IndexedMeshMergeVertexPassesTriangleTest(surface->verts[mergeVertex].pos, winding, plane))
        {
          VectorCopy(winding->points[2], winding->points[3]);
          VectorCopy(winding->points[1], winding->points[2]);
          VectorCopy(surface->verts[mergeVertex].pos, winding->points[1]);
          winding->numpoints = 4;
          index += 3;
        }
      }
      AppendClippedWindingHull(outList, light, winding, exterior, surface);
    }
  }
  FreeWinding(winding);
}

/* Native 0x435A40. */
static void BuildBrushSideHull(const PrimaryLightRegionDesc_t *light, MapDrawSurf_t *surface,
                               int exterior, PrimaryLightRegionNode_t **outList)
{
  Assert(surface->u.brush.side != NULL, s_assertDisable_PrimaryLightRegionSurfaceEligible);
  Assert(surface->u.brush.side->winding != NULL, s_assertDisable_PrimaryLightRegionSurfaceEligible);
  AppendClippedWindingHull(outList, light, surface->u.brush.side->winding, exterior, surface);
}

/* Native 0x435400. */
static PrimaryLightRegionNode_t *BuildEligibleSurfaceHull(const PrimaryLightRegionDesc_t *light,
                                                          MapDrawSurf_t **surfaces,
                                                          unsigned int surfaceCount)
{
  unsigned int surfaceIndex;
  PrimaryLightRegionNode_t *list = NULL;
  for (surfaceIndex = 0; surfaceIndex < surfaceCount; ++surfaceIndex)
  {
    MapDrawSurf_t *surface = surfaces[surfaceIndex];
    const int exterior = (surface->material->toolFlagsWord & 0x2000) == 0;
    if (surface->isPatch)
      BuildPatchMeshHull(light, surface, exterior, &list);
    else if (surface->isTerrain)
      BuildIndexedMeshHull(light, surface, exterior, &list);
    else
      BuildBrushSideHull(light, surface, exterior, &list);
  }
  return list;
}

/* Native 0x435C30.  The packed headers/axis tails are written in the same
   native order through the BSP-file bridge. */
static void EmitPrimaryLightRegions(void)
{
  int lightIndex;
  int lightRegionCount = 0;

  BeginBspPrimaryLightRegionEmission();
  for (lightIndex = 0; lightIndex < numBspPrimaryLights; ++lightIndex)
  {
    PrimaryLightRegionOutput_t *region = &s_primaryLightRegionOutput[lightIndex];
    int hullIndex;

    Assert(lightRegionCount == lightIndex, s_assertDisable_PrimaryLightRegionSurfaceEligible);
    if (bspPrimaryLights[lightIndex].type && bspPrimaryLights[lightIndex].type != 1)
    {
      bspLightRegions[lightRegionCount++] = CheckedPrimaryLightByteCast(region->hullCount);
      Assert(region->hullCount <= 8, s_assertDisable_PrimaryLightRegionSurfaceEligible);
      for (hullIndex = 0; hullIndex < region->hullCount; ++hullIndex)
        AppendBspPrimaryLightRegionHull(region->hulls[hullIndex]);
    }
    else
    {
      bspLightRegions[lightRegionCount++] = 0;
    }
  }
}

/* Native 0x424F70. */
static int PrimaryLightMaterialEligible(const MapDrawSurf_t *surface)
{
  if (surface->material->toolFlagsWord & 0x2000)
    return 1;
  return (surface->material->toolFlagsWord & 0x70) == 0x10
      && (surface->material->surfaceFlags & 0x40000) == 0;
}

/* Native 0x434F40.  EmitWorldBSP invokes this only for the world model,
   immediately after 0x432DF0 has prepared the direct visible-surface
   acceleration data. */
void BuildPrimaryLightRegions(void)
{
  int directCount;
  int indexedCount;
  int *indexedSurfaces = NULL;
  MapDrawSurf_t **candidateSurfaces;
  float *centers;
  float *halfSizes;
  int totalCount;
  int index;
  int primaryLightIndex;

  Assert(g_currentEntityIndex == 0, s_assertDisable_PrimaryLightRegionSurfaceEligible);
  Assert(g_entities[0].firstDrawSurf == 0, s_assertDisable_PrimaryLightRegionSurfaceEligible);
  BuildPrimaryLightSurfaceAabbTree(); /* native 0x432DF0 predecessor */
  for (directCount = 0;
       directCount < numMapDrawSurfs
       && (g_nativeMapDrawSurfs[directCount].material->toolFlagsWord & 0x70) == 0x10
       && (g_nativeMapDrawSurfs[directCount].material->surfaceFlags & 0x40004) == 0;
       ++directCount)
  {
  }
  if (!directCount)
    Com_Error("ERROR: Map must have at least one visible non-sky surface\n");

  indexedCount = 0;
  for (index = directCount; index < numMapDrawSurfs; ++index)
  {
    if (PrimaryLightMaterialEligible(&g_nativeMapDrawSurfs[index]))
      ++indexedCount;
  }
  if (indexedCount)
  {
    indexedSurfaces = (int *)malloc(sizeof(*indexedSurfaces) * indexedCount);
    if (!indexedSurfaces)
      Com_Error("ERROR: out of memory");
    indexedCount = 0;
    for (index = directCount; index < numMapDrawSurfs; ++index)
    {
      if (PrimaryLightMaterialEligible(&g_nativeMapDrawSurfs[index]))
        indexedSurfaces[indexedCount++] = index;
    }
  }

  totalCount = directCount + indexedCount;
  candidateSurfaces = (MapDrawSurf_t **)malloc(sizeof(*candidateSurfaces) * totalCount);
  centers = (float *)malloc(sizeof(*centers) * 3 * totalCount);
  halfSizes = (float *)malloc(sizeof(*halfSizes) * 3 * totalCount);
  if (!candidateSurfaces || !centers || !halfSizes)
    Com_Error("ERROR: out of memory");
  for (index = 0; index < directCount; ++index)
    BoundsForPrimaryLightSurface(index, centers + 3 * index, halfSizes + 3 * index);
  for (index = 0; index < indexedCount; ++index)
    BoundsForPrimaryLightSurface(indexedSurfaces[index], centers + 3 * (index + directCount),
                                 halfSizes + 3 * (index + directCount));

  memset(s_primaryLightRegionOutput, 0, sizeof(s_primaryLightRegionOutput));
  for (primaryLightIndex = (PrimaryLightRegionSurfaceEligible() != 0) + 1;
       primaryLightIndex < numBspPrimaryLights;
       ++primaryLightIndex)
  {
    const DiskPrimaryLight_t *diskLight = &bspPrimaryLights[primaryLightIndex];
    PrimaryLightRegionDesc_t light;
    PrimaryLightRegionNode_t *list;
    PrimaryLightRegionOutput_t *region = &s_primaryLightRegionOutput[primaryLightIndex];
    int candidateCount;

    light.type = diskLight->type;
    VectorCopy(diskLight->origin, light.origin);
    VectorCopy(diskLight->dir, light.dir);
    light.radius = diskLight->radius;
    light.cosHalfFov = PrimaryLightSpotConeExtent(diskLight);
    candidateCount = CullPrimaryLightCandidateSurfaces(&light, directCount, indexedCount,
                                                        indexedSurfaces, centers, halfSizes,
                                                        candidateSurfaces);
    list = BuildEligibleSurfaceHull(&light, candidateSurfaces, candidateCount);
    GenerateEnclosingPrimaryLightHull(&list, &light);
    region->hullCount = DecomposeConvexPrimaryLightRegion(&list, &light, 8, (void **)region->hulls);
    Assert(region->hullCount <= 8, s_assertDisable_PrimaryLightRegionSurfaceEligible);
    FreePrimaryLightHullList(list);
  }

  free(candidateSurfaces);
  free(centers);
  free(halfSizes);
  free(indexedSurfaces);
  EmitPrimaryLightRegions();
}

/* Native 0x435FF0. */
static unsigned char CheckedPrimaryLightByteCast(int value)
{
  Assert(value == (unsigned char)value, s_assertDisable_CheckedPrimaryLightByteCast);
  return (unsigned char)value;
}

/* Native 0x433F60. */
const char *DescribePrimaryLight(int primaryLightIndex)
{
  DiskPrimaryLight_t *light;
  const char *kind;

  Assert(primaryLightIndex != 0, s_assertDisable_DescribePrimaryLightIndex);
  light = &bspPrimaryLights[primaryLightIndex];
  if (light->type == 1)
    return "sun";
  kind = light->type == 2 ? "spotlight" : "point light";
  return va("%s at %.0f %.0f %.0f", kind, light->origin[0], light->origin[1], light->origin[2]);
}

/* Native 0x434BC0. */
float PrimaryLightSpotConeExtent(const DiskPrimaryLight_t *light)
{
  float sineA;
  float sineB;

  if (light->rotationLimit == 1.0f)
    return light->cosHalfFovOuter;
  if (light->rotationLimit <= -light->cosHalfFovOuter)
    return -1.0f;
  sineA = sqrtf(1.0f - light->cosHalfFovOuter * light->cosHalfFovOuter);
  sineB = sqrtf(1.0f - light->cosHalfFovInner * light->cosHalfFovInner);
  return light->cosHalfFovOuter * light->cosHalfFovInner - sineA * sineB;
}

static float PrimaryLight_Clamp(float value, float minimum, float maximum)
{
  if ( value < minimum )
    return minimum;
  if ( value > maximum )
    return maximum;
  return value;
}

static float PrimaryLight_Normalize(float *vector)
{
  float length;

  length = sqrtf(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
  if ( length != 0.0f )
  {
    const float inverseLength = 1.0f / length;
    vector[0] *= inverseLength;
    vector[1] *= inverseLength;
    vector[2] *= inverseLength;
  }
  return length;
}

static float PrimaryLight_ColorNormalize(const float *input, float *output)
{
  float maximum;

  maximum = input[0];
  if ( maximum < input[1] )
    maximum = input[1];
  if ( maximum < input[2] )
    maximum = input[2];
  if ( maximum == 0.0f )
  {
    output[0] = 1.0f;
    output[1] = 1.0f;
    output[2] = 1.0f;
    return 0.0f;
  }
  output[0] = input[0] / maximum;
  output[1] = input[1] / maximum;
  output[2] = input[2] / maximum;
  return maximum;
}

static void PrimaryLightWarning(Entity_t *entity, const float *origin, const char *message)
{
  Error(0, entity->origin, origin, entity->mapInfoIndex, entity->entityNum, 0, "%s", message);
  SetKeyValue(entity, "intensity", "0");
}

static int PrimaryLightLess(const DiskPrimaryLight_t *light0, const DiskPrimaryLight_t *light1)
{
  int axis;
  int nameCompare;

  if ( light0->type != light1->type )
    return light0->type < light1->type;

  nameCompare = _stricmp(light0->defName, light1->defName);
  if ( nameCompare )
    return nameCompare < 0;

  if ( light0->type != 1 )
  {
    for ( axis = 0; axis < 3; ++axis )
    {
      if ( light0->origin[axis] != light1->origin[axis] )
        return light0->origin[axis] < light1->origin[axis];
    }
    if ( light0->radius != light1->radius )
      return light0->radius < light1->radius;
    if ( light0->cosHalfFovOuter != light1->cosHalfFovOuter )
      return light0->cosHalfFovOuter < light1->cosHalfFovOuter;
    if ( light0->cosHalfFovInner != light1->cosHalfFovInner )
      return light0->cosHalfFovInner < light1->cosHalfFovInner;
    if ( light0->type == 2 && light0->exponent != light1->exponent )
      return light0->exponent < light1->exponent;
  }

  /*
   * The on-disk color is deliberately not a sort key.  cod4map orders two
   * otherwise identical primary lights by their direction vector, so that
   * changing an intensity/color does not perturb the primary-light index
   * assigned to movable lights.  (cod4map.exe 0x432740.)
   */
  for ( axis = 0; axis < 3; ++axis )
  {
    if ( light0->dir[axis] != light1->dir[axis] )
      return light0->dir[axis] < light1->dir[axis];
  }
  return 0;
}

static void CreateSunPrimaryLight(void)
{
  DiskPrimaryLight_t *light;
  const char *sunIsPrimaryLight;
  float ambient;
  float ambientColor[3];
  float diffuseAmount;
  float diffuseColor[3];
  float diffuseFraction;
  float sunlight;
  float sunColor[3];
  float sunIsPrimaryValue;
  float sunAngles[3];
  int axis;
  int parsedValue;

  sunIsPrimaryLight = ValueForKey(&g_entities[0], "sunIsPrimaryLight");
  if ( sscanf(sunIsPrimaryLight, "%i", &parsedValue) == 1 && !parsedValue )
    return;

  sunlight = (float)FloatForKey(&g_entities[0], "sunlight");
  GetVectorForKey(&g_entities[0], "suncolor", sunColor);
  if ( PrimaryLight_ColorNormalize(sunColor, sunColor) == 0.0f )
    VectorClear(sunColor);

  diffuseFraction = (float)FloatForKey(&g_entities[0], "diffusefraction");
  GetVectorForKey(&g_entities[0], "sundiffusecolor", diffuseColor);
  if ( PrimaryLight_ColorNormalize(diffuseColor, diffuseColor) == 0.0f )
    VectorClear(diffuseColor);

  ambient = (float)FloatForKey(&g_entities[0], "ambient");
  GetVectorForKey(&g_entities[0], "_color", ambientColor);
  if ( PrimaryLight_ColorNormalize(ambientColor, ambientColor) == 0.0f )
    VectorClear(ambientColor);

  if ( sunlight < ambient )
  {
    Com_Printf("WARNING: ambient %g > sunlight %g, increasing sunlight to match ambient\n", ambient, sunlight);
    sunlight = ambient;
  }
  if ( diffuseFraction < 0.0f || diffuseFraction > 1.0f )
  {
    Com_Printf("WARNING: clamping diffuseFraction %g to the range [0, 1]\n", diffuseFraction);
    diffuseFraction = PrimaryLight_Clamp(diffuseFraction, 0.0f, 1.0f);
  }

  diffuseAmount = (sunlight - ambient) * diffuseFraction;
  sunIsPrimaryValue = sunlight - ambient - diffuseAmount;
  for ( axis = 0; axis < 3; ++axis )
  {
    const float ambientLinear = ambientColor[axis] * ambient;
    const float diffuseLinear = diffuseColor[axis] * diffuseAmount;
    const float sunLinear = sunColor[axis] * sunIsPrimaryValue;
    const float nonAmbient = diffuseLinear + sunLinear;
    const float ambientGamma = powf(ambientLinear, 2.2f);

    ambientColor[axis] = ambientGamma;
    if ( nonAmbient == 0.0f )
    {
      diffuseColor[axis] = 0.0f;
      sunColor[axis] = 0.0f;
    }
    else
    {
      const float totalGamma = powf(nonAmbient + ambientLinear, 2.2f);
      diffuseColor[axis] = (totalGamma - ambientGamma) * diffuseLinear / nonAmbient;
      sunColor[axis] = (totalGamma - ambientGamma) * sunLinear / nonAmbient;
    }
  }

  light = &bspPrimaryLights[numBspPrimaryLights++];
  memset(light, 0, sizeof(*light));
  light->type = 1;
  for ( axis = 0; axis < 3; ++axis )
    light->color[axis] = powf(sunColor[axis], 1.0f / 2.2f);
  GetVectorForKey(&g_entities[0], "sundirection", sunAngles);
  AngleVectors(sunAngles, light->dir, NULL, NULL);
}

void CreatePrimaryLights(void)
{
  DiskPrimaryLight_t light;
  Entity_t *entity;
  Entity_t *target;
  const char *classname;
  const char *definitionName;
  const char *targetName;
  float color[3];
  float distanceSquared;
  float fov;
  float intensity;
  float maxTurn;
  int entityIndex;
  int lightEntityIndex[MAX_MAP_PRIMARY_LIGHTS];
  int lightIndex;
  int spawnFlags;
  char buffer[16];

  numBspPrimaryLights = 0;
  memset(&bspPrimaryLights[0], 0, sizeof(bspPrimaryLights[0]));
  ++numBspPrimaryLights;
  memset(lightEntityIndex, 0, sizeof(lightEntityIndex));
  CreateSunPrimaryLight();

  for ( entityIndex = 1; entityIndex < num_entities; ++entityIndex )
  {
    entity = &g_entities[entityIndex];
    classname = ValueForKey(entity, "classname");
    if ( strcmp(classname, "light") )
      continue;

    DeleteKey(entity, "pl#");
    spawnFlags = IntForKey(entity, "spawnflags");
    if ( !(spawnFlags & 3) )
      continue;

    targetName = ValueForKey(entity, "target");
    if ( !*targetName )
    {
      PrimaryLightWarning(entity, NULL, "ignoring primary light without a 'target' key");
      continue;
    }
    target = FindEntityByKeyValue(g_entities, num_entities, "targetname", targetName);
    if ( !target )
    {
      PrimaryLightWarning(entity, NULL, va("ignoring primary light because target '%s' is missing", targetName));
      continue;
    }
    if ( numBspPrimaryLights >= MAX_MAP_PRIMARY_LIGHTS )
    {
      PrimaryLightWarning(entity, NULL, va("ignoring primary light because max primary lights (%i) exceeded", MAX_MAP_PRIMARY_LIGHTS));
      continue;
    }

    memset(&light, 0, sizeof(light));
    definitionName = ValueForKey(entity, "def");
    if ( !*definitionName )
      definitionName = "light_point_linear";
    if ( strlen(definitionName) >= sizeof(light.defName) )
    {
      PrimaryLightWarning(entity, NULL, va("ignoring primary light because def name %s is longer than %i characters", definitionName, 63));
      continue;
    }
    strcpy(light.defName, definitionName);
    VectorCopy(entity->origin, light.origin);
    VectorSubtract(entity->origin, target->origin, light.dir);
    distanceSquared = DotProduct(light.dir, light.dir);
    PrimaryLight_Normalize(light.dir);

    light.radius = (float)FloatForKey(entity, "radius");
    if ( light.radius <= 0.0f )
    {
      PrimaryLightWarning(entity, light.dir, va("ignoring primary light with invalid radius %g", light.radius));
      continue;
    }

    fov = (float)FloatForKey(entity, "fov_outer");
    if ( fov == 0.0f )
      light.cosHalfFovOuter = sqrtf(distanceSquared / (distanceSquared + 4096.0f));
    else
    {
      light.cosHalfFovOuter = cosf(fov * 0.01745329238474369f * 0.5f);
      if ( light.cosHalfFovOuter <= 0.0f )
      {
        PrimaryLightWarning(entity, light.dir, "ignoring primary light with fov_outer >= 180 degrees");
        continue;
      }
    }

    GetVectorForKey(entity, "_color", color);
    PrimaryLight_ColorNormalize(color, color);
    intensity = (float)FloatForKey(entity, "intensity");
    VectorScale(color, intensity, light.color);

    fov = (float)FloatForKey(entity, "fov_inner");
    light.cosHalfFovInner = cosf(fov * 0.01745329238474369f * 0.5f);
    if ( spawnFlags & 2 )
    {
      light.type = 2;
      if ( light.cosHalfFovOuter > light.cosHalfFovInner )
      {
        PrimaryLightWarning(entity, light.dir, "ignoring primary spotlight because fov_inner > fov_outer");
        continue;
      }
      light.exponent = IntForKey(entity, "exponent");
    }
    else
    {
      light.type = 3;
      if ( light.cosHalfFovOuter > light.cosHalfFovInner )
        light.cosHalfFovInner = light.cosHalfFovOuter * 0.75f + 0.25f;
      light.exponent = 0;
    }

    if ( spawnFlags & 4 )
    {
      light.translationLimit = (float)FloatForKey(entity, "maxmove");
      maxTurn = PrimaryLight_Clamp((float)FloatForKey(entity, "maxturn"), 0.0f, 180.0f);
      light.rotationLimit = cosf(maxTurn * 0.01745329238474369f);
    }
    else
    {
      light.translationLimit = 0.0f;
      light.rotationLimit = 1.0f;
    }

    light.canUseShadowMap = 0;
    if ( !(spawnFlags & 8) )
    {
      if ( light.cosHalfFovOuter <= 0.4990000128746033f )
      {
        PrimaryLightWarning(entity, light.dir, "ignoring primary light because PRIMARY_NOSHADOWMAP is not checked and fov_outer > 120");
        continue;
      }
      light.canUseShadowMap = 1;
    }

    for ( lightIndex = numBspPrimaryLights;
          lightIndex > 1 && PrimaryLightLess(&light, &bspPrimaryLights[lightIndex - 1]);
          --lightIndex )
    {
      bspPrimaryLights[lightIndex] = bspPrimaryLights[lightIndex - 1];
      lightEntityIndex[lightIndex] = lightEntityIndex[lightIndex - 1];
    }
    bspPrimaryLights[lightIndex] = light;
    lightEntityIndex[lightIndex] = entityIndex;
    ++numBspPrimaryLights;
  }

  for ( lightIndex = 0; lightIndex < numBspPrimaryLights; ++lightIndex )
  {
    if ( bspPrimaryLights[lightIndex].type == 2 || bspPrimaryLights[lightIndex].type == 3 )
    {
      entity = &g_entities[lightEntityIndex[lightIndex]];
      if ( IntForKey(entity, "spawnflags") & 4 )
      {
        _itoa(lightIndex, buffer, 10);
        SetKeyValue(entity, "pl#", buffer);
      }
    }
  }
}

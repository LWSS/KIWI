/*
===========================================================================

fixedpoint.cpp -- Native CoD4 fixed-point polygon helpers.

Recovered from cod4map.exe 0x417AE0-0x417E2F.  Keep this TU independent of
cod4map.h until the native fixed-point splitter chain is ported.

===========================================================================
*/

#include "fixedpoint.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FIXEDPOINT_SCALE 16384.0
#define FIXEDPOINT_INV_SCALE 0.00006103515625

static float *FixedCrossProduct(const float *a, const float *b, float *out)
{
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
  return (float *)b;
}

FixedVec3_t *FixedVec3Subtract(const FixedVec3_t *a, const FixedVec3_t *b, FixedVec3_t *out)
{
  out->x = a->x - b->x;
  out->y = a->y - b->y;
  out->z = a->z - b->z;
  /* Native 0x417D50 leaves its second argument in EAX. */
  return (FixedVec3_t *)b;
}

FixedVec3_t *FixedVec3Copy(const FixedVec3_t *in, FixedVec3_t *out)
{
  out->x = in->x;
  out->y = in->y;
  out->z = in->z;
  return out;
}

float FixedToFloat(int value)
{
  return (float)((double)value * FIXEDPOINT_INV_SCALE);
}

FixedVec3_t *FixedVec3ToFloat(FixedVec3_t *in, float *out)
{
  out[0] = FixedToFloat(in->x);
  out[1] = FixedToFloat(in->y);
  out[2] = FixedToFloat(in->z);
  /* Native 0x417C80 leaves its input pointer in EAX. */
  return in;
}

int FixedFromFloat(float value)
{
  /* Native 0x417C50: floorf(value * 16384 + 0.5), then __ftol2_sse. */
  return (int)floor((double)value * FIXEDPOINT_SCALE + 0.5);
}

int FixedVec3FromFloat(const float *in, FixedVec3_t *out)
{
  out->x = FixedFromFloat(in[0]);
  out->y = FixedFromFloat(in[1]);
  out->z = FixedFromFloat(in[2]);
  return out->z;
}

#pragma warning(push)
#pragma warning(disable : 4172) /* Native returns CrossProduct's stack-resident second input in EAX. */
float *FixedTriangleNormal(const FixedVec3_t *p0, const FixedVec3_t *p1, const FixedVec3_t *p2, float *out)
{
  FixedVec3_t edge1;
  FixedVec3_t edge2;
  float edge1Float[3];
  float edge2Float[3];

  FixedVec3Subtract(p1, p0, &edge1);
  FixedVec3Subtract(p2, p0, &edge2);
  FixedVec3ToFloat(&edge1, edge1Float);
  FixedVec3ToFloat(&edge2, edge2Float);

  /* 0x417B90 calls CrossProduct(edge2, edge1), in this order. */
  /* 0x4075E0 returns its second input, which is edge1Float here. */
  return FixedCrossProduct(edge2Float, edge1Float, out);
}
#pragma warning(pop)

int FixedWindingIsClockwise(const FixedWinding_t *winding, const float *planeNormal)
{
  uint32_t i;
  float normal[3];
  float signedArea = 0.0f;

  for (i = 2; i < winding->ptCount; ++i)
  {
    FixedTriangleNormal(&winding->points[0], &winding->points[i - 1], &winding->points[i], normal);
    signedArea += normal[0] * planeNormal[0]
                + normal[1] * planeNormal[1]
                + normal[2] * planeNormal[2];
  }

  return signedArea < 0.0f;
}

double FixedPointPlaneDistance(const FixedVec3_t *point, const float *planeNormal, double planeDist)
{
  return ((double)point->z * planeNormal[2]
        + (double)point->y * planeNormal[1]
        + (double)point->x * planeNormal[0]) * FIXEDPOINT_INV_SCALE - planeDist;
}

int FixedPlaneDistanceFromFloat(double value)
{
  return (int)floor(value * FIXEDPOINT_SCALE + 0.5);
}

int FixedLerpComponent(int current, int next, double currentDist, double nextDist)
{
  return (int)floor(((double)current * nextDist - (double)next * currentDist)
                   / (nextDist - currentDist) + 0.5);
}

FixedWinding_t *AllocFixedWinding(uint32_t ptCount)
{
  if (ptCount < 3)
    return NULL;

  return (FixedWinding_t *)malloc(sizeof(uint32_t) + sizeof(FixedVec3_t) * ptCount);
}

void FreeFixedWinding(FixedWinding_t *winding)
{
  free(winding);
}

FixedWinding_t *CopyFixedWinding(const FixedWinding_t *winding)
{
  FixedWinding_t *copy;

  if (!winding)
    return NULL;

  copy = AllocFixedWinding(winding->ptCount);
  if (!copy)
    return NULL;

  memcpy(copy, winding, sizeof(uint32_t) + sizeof(FixedVec3_t) * winding->ptCount);
  return copy;
}

double FixedWindingMaxPlaneDistance(const FixedWinding_t *winding, const float *planeNormal, double planeDist)
{
  uint32_t i;
  double maxDistance = 0.0;

  for (i = 0; i < winding->ptCount; ++i)
  {
    double distance = FixedPointPlaneDistance(&winding->points[i], planeNormal, planeDist);
    double absoluteDistance = fabs(distance);
    if (absoluteDistance > maxDistance)
      maxDistance = absoluteDistance;
  }

  return maxDistance;
}

FixedWinding_t *FixedWindingDistanceRange(const FixedWinding_t *winding, const float *planeNormal,
                                           double planeDist, double *minDist, double *maxDist)
{
  uint32_t i;

  *minDist = DBL_MAX;
  *maxDist = -DBL_MAX;
  for (i = 0; i < winding->ptCount; ++i)
  {
    double distance = FixedPointPlaneDistance(&winding->points[i], planeNormal, planeDist);
    if (distance < *minDist)
      *minDist = distance;
    if (distance > *maxDist)
      *maxDist = distance;
  }

  /* Native 0x416930 leaves the input winding pointer in EAX. */
  return (FixedWinding_t *)winding;
}

int FixedEdgePlane(const FixedVec3_t *previous, const FixedVec3_t *current,
                   const float *surfacePlaneNormal, float *outNormal, double *outDist)
{
  FixedVec3_t edge;
  double crossX;
  double crossY;
  double crossZ;
  double lengthSq;

  FixedVec3Subtract(current, previous, &edge);

  /* Native 0x4169D0: CrossProduct(surfacePlaneNormal, edge). */
  crossX = (double)edge.z * surfacePlaneNormal[1] - (double)edge.y * surfacePlaneNormal[2];
  crossY = (double)edge.x * surfacePlaneNormal[2] - (double)edge.z * surfacePlaneNormal[0];
  crossZ = (double)edge.y * surfacePlaneNormal[0] - (double)edge.x * surfacePlaneNormal[1];
  lengthSq = crossX * crossX + crossY * crossY + crossZ * crossZ;

  if (lengthSq == 0.0)
  {
    outNormal[0] = 0.0f;
    outNormal[1] = 0.0f;
    outNormal[2] = 0.0f;
    *outDist = 0.0;
    return 0;
  }

  {
    double reciprocalLength = 1.0 / sqrt(lengthSq);
    outNormal[0] = (float)(crossX * reciprocalLength);
    outNormal[1] = (float)(crossY * reciprocalLength);
    outNormal[2] = (float)(crossZ * reciprocalLength);
  }

  *outDist = ((double)previous->x * outNormal[0]
            + (double)previous->y * outNormal[1]
            + (double)previous->z * outNormal[2]) * FIXEDPOINT_INV_SCALE;
  return 1;
}

enum FixedPlaneSide_e
{
  FIXED_SIDE_FRONT = 0,
  FIXED_SIDE_BACK = 1,
  FIXED_SIDE_ON = 2
};

static int FixedWindingAppend(FixedWinding_t *winding, uint32_t maxPts, const FixedVec3_t *point)
{
  if (winding->ptCount >= maxPts || winding->ptCount >= FIXEDWINDING_MAX_POINTS)
    return 0;

  FixedVec3Copy(point, &winding->points[winding->ptCount]);
  ++winding->ptCount;
  return 1;
}

FixedWinding_t *FixedWindingClipEpsilon(const FixedWinding_t *in,
                                         const FixedWinding_t *epsilonReference,
                                         const float *planeNormal, double planeDist, double epsilon,
                                         FixedWinding_t **front, FixedWinding_t **back)
{
  double distances[FIXEDWINDING_MAX_POINTS + 4];
  int sides[FIXEDWINDING_MAX_POINTS + 4];
  double minDistance;
  double maxDistance;
  uint32_t maxPts;
  uint32_t i;
  int sideCounts[3] = { 0, 0, 0 };
  int exactAxialPlane = 1;
  int axialPlaneAxis = -1;
  int axialPlaneDistance = 0;
  FixedWinding_t *frontWinding;
  FixedWinding_t *backWinding;

  if (!front || !back)
    return NULL;
  *front = NULL;
  *back = NULL;

  if (!in || !epsilonReference || !planeNormal || in->ptCount < 3 || in->ptCount > FIXEDWINDING_MAX_POINTS)
    return NULL;

  FixedWindingDistanceRange(in, planeNormal, planeDist, &minDistance, &maxDistance);

  if (epsilon * 0.01 >= maxDistance)
  {
    *back = CopyFixedWinding(in);
    return *back;
  }
  if (-epsilon * 0.01 <= minDistance)
  {
    *front = CopyFixedWinding(in);
    return *front;
  }

  if (epsilon > maxDistance && maxDistance * -3.0 > minDistance)
  {
    double refinedEpsilon = FixedWindingMaxPlaneDistance(epsilonReference, planeNormal, planeDist) * 0.25;
    if (refinedEpsilon >= maxDistance)
    {
      *back = CopyFixedWinding(in);
      return *back;
    }
    if (refinedEpsilon < epsilon)
      epsilon = refinedEpsilon;
  }

  if (-epsilon < minDistance && minDistance * -3.0 < maxDistance)
  {
    double refinedEpsilon = FixedWindingMaxPlaneDistance(epsilonReference, planeNormal, planeDist) * 0.25;
    if (-refinedEpsilon <= minDistance)
    {
      *front = CopyFixedWinding(in);
      return *front;
    }
    if (refinedEpsilon < epsilon)
      epsilon = refinedEpsilon;
  }

  {
    double rangeEpsilon = (maxDistance - minDistance) * 0.125;
    if (rangeEpsilon < epsilon)
      epsilon = rangeEpsilon;
  }

  for (i = 0; i < in->ptCount; ++i)
  {
    distances[i] = FixedPointPlaneDistance(&in->points[i], planeNormal, planeDist);
    if (epsilon > distances[i])
      sides[i] = (-epsilon < distances[i]) ? FIXED_SIDE_ON : FIXED_SIDE_BACK;
    else
      sides[i] = FIXED_SIDE_FRONT;
    ++sideCounts[sides[i]];
  }
  distances[in->ptCount] = distances[0];
  sides[in->ptCount] = sides[0];

  if (!sideCounts[FIXED_SIDE_FRONT] || !sideCounts[FIXED_SIDE_BACK])
    return NULL;

  for (i = 0; i < 3; ++i)
  {
    if (planeNormal[i] == 1.0f)
    {
      axialPlaneAxis = (int)i;
      axialPlaneDistance = FixedPlaneDistanceFromFloat(planeDist);
    }
    else if (planeNormal[i] == -1.0f)
    {
      axialPlaneAxis = (int)i;
      axialPlaneDistance = FixedPlaneDistanceFromFloat(-planeDist);
    }
    else if (planeNormal[i] != 0.0f)
    {
      exactAxialPlane = 0;
      break;
    }
  }
  if (exactAxialPlane && (axialPlaneAxis < 0 || axialPlaneAxis >= 3))
    return NULL;

  maxPts = in->ptCount + 4;
  frontWinding = AllocFixedWinding(maxPts);
  backWinding = AllocFixedWinding(maxPts);
  if (!frontWinding || !backWinding)
  {
    FreeFixedWinding(frontWinding);
    FreeFixedWinding(backWinding);
    return NULL;
  }
  frontWinding->ptCount = 0;
  backWinding->ptCount = 0;

  for (i = 0; i < in->ptCount; ++i)
  {
    const FixedVec3_t *point = &in->points[i];

    if (sides[i] == FIXED_SIDE_ON)
    {
      if (!FixedWindingAppend(frontWinding, maxPts, point) || !FixedWindingAppend(backWinding, maxPts, point))
        goto fail;
    }
    else
    {
      if (sides[i] == FIXED_SIDE_FRONT && !FixedWindingAppend(frontWinding, maxPts, point))
        goto fail;
      if (sides[i] == FIXED_SIDE_BACK && !FixedWindingAppend(backWinding, maxPts, point))
        goto fail;

      if (sides[i + 1] != FIXED_SIDE_ON && sides[i + 1] != sides[i])
      {
        FixedVec3_t intersection;
        intersection.x = FixedLerpComponent(point->x, in->points[(i + 1) % in->ptCount].x,
                                            distances[i], distances[i + 1]);
        intersection.y = FixedLerpComponent(point->y, in->points[(i + 1) % in->ptCount].y,
                                            distances[i], distances[i + 1]);
        intersection.z = FixedLerpComponent(point->z, in->points[(i + 1) % in->ptCount].z,
                                            distances[i], distances[i + 1]);
        if (exactAxialPlane)
          ((int *)&intersection)[axialPlaneAxis] = axialPlaneDistance;
        if (!FixedWindingAppend(frontWinding, maxPts, &intersection)
            || !FixedWindingAppend(backWinding, maxPts, &intersection))
          goto fail;
      }
    }
  }

  *front = frontWinding;
  *back = backWinding;
  return frontWinding;

fail:
  FreeFixedWinding(frontWinding);
  FreeFixedWinding(backWinding);
  return NULL;
}

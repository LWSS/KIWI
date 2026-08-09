/*
===========================================================================

fixedpoint.h -- Native CoD4 fixed-point polygon helpers.

The compiler represents these winding coordinates as signed 18.14 fixed
point values.  This header deliberately does not include cod4map.h: the
module is not wired into the reconstructed geometry path yet.

===========================================================================
*/

#ifndef COD4MAP_FIXEDPOINT_H
#define COD4MAP_FIXEDPOINT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FixedVec3_s
{
  int32_t x;
  int32_t y;
  int32_t z;
} FixedVec3_t;

/* Native layout: DWORD count followed immediately by 12-byte points. */
typedef struct FixedWinding_s
{
  uint32_t ptCount;
  FixedVec3_t points[1];
} FixedWinding_t;

#define FIXEDWINDING_MAX_POINTS 1024

#ifdef __cplusplus
static_assert(sizeof(FixedVec3_t) == 12, "FixedVec3_t must match native layout");
static_assert(offsetof(FixedWinding_t, points) == 4, "FixedWinding_t point offset must match native layout");
#else
typedef char FixedVec3_must_be_12_bytes[(sizeof(FixedVec3_t) == 12) ? 1 : -1];
typedef char FixedWinding_points_must_start_at_4[(offsetof(FixedWinding_t, points) == 4) ? 1 : -1];
#endif

int FixedWindingIsClockwise(const FixedWinding_t *winding, const float *planeNormal);
FixedVec3_t *FixedVec3Subtract(const FixedVec3_t *a, const FixedVec3_t *b, FixedVec3_t *out);
FixedVec3_t *FixedVec3Copy(const FixedVec3_t *in, FixedVec3_t *out);
float *FixedTriangleNormal(const FixedVec3_t *p0, const FixedVec3_t *p1, const FixedVec3_t *p2, float *out);
int FixedFromFloat(float value);
int FixedVec3FromFloat(const float *in, FixedVec3_t *out);
float FixedToFloat(int value);
FixedVec3_t *FixedVec3ToFloat(FixedVec3_t *in, float *out);
double FixedPointPlaneDistance(const FixedVec3_t *point, const float *planeNormal, double planeDist);
int FixedPlaneDistanceFromFloat(double value);
int FixedLerpComponent(int current, int next, double currentDist, double nextDist);

FixedWinding_t *AllocFixedWinding(uint32_t ptCount);
void FreeFixedWinding(FixedWinding_t *winding);
FixedWinding_t *CopyFixedWinding(const FixedWinding_t *winding);
double FixedWindingMaxPlaneDistance(const FixedWinding_t *winding, const float *planeNormal, double planeDist);
FixedWinding_t *FixedWindingDistanceRange(const FixedWinding_t *winding, const float *planeNormal,
                                           double planeDist, double *minDist, double *maxDist);
int FixedEdgePlane(const FixedVec3_t *previous, const FixedVec3_t *current,
                   const float *surfacePlaneNormal, float *outNormal, double *outDist);
FixedWinding_t *FixedWindingClipEpsilon(const FixedWinding_t *in,
                                         const FixedWinding_t *epsilonReference,
                                         const float *planeNormal, double planeDist, double epsilon,
                                         FixedWinding_t **front, FixedWinding_t **back);

#ifdef __cplusplus
}
#endif

#endif

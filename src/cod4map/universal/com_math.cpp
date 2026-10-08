/*
com_math.c — Math utilities

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

/*
================
VectorCompareEpsilon

Returns 1 if all 'count' float pairs are within epsilon.
volatile forces float32 truncation matching original x87 fstp/fld sequence.
================
*/
int VectorCompareEpsilon( float *v1, float *v2, float epsilon, int count )
{
	volatile float epsSq = epsilon * epsilon;
	int i;

	for ( i = 0; i < count; i++ )
	{
		volatile float diff = v1[i] - v2[i];
		if ( !(diff * diff <= epsSq) )
			return 0;
	}
	return 1;
}

/*
================
CrossProduct

cross = v1 x v2
================
*/
float *CrossProduct( float *v1, float *v2, float *cross )
{
  Assert(v1 != cross, s_assertDisable_CrossProduct);
  Assert(v2 != cross, s_assertDisable_CrossProduct);
#ifdef _WIN64
  cross[0] = (float)((double)v1[1]*v2[2] - (double)v1[2]*v2[1]);
  cross[1] = (float)((double)v1[2]*v2[0] - (double)v1[0]*v2[2]);
  cross[2] = (float)((double)v1[0]*v2[1] - (double)v1[1]*v2[0]);
#else
  cross[0] = v1[1] * v2[2] - v1[2] * v2[1];
  cross[1] = v1[2] * v2[0] - v1[0] * v2[2];
  cross[2] = v1[0] * v2[1] - v1[1] * v2[0];
#endif
  return v2;
}

/*
================
VecNormalize

Normalizes vec in place, returns original length.
================
*/
double VecNormalize( float *vec )
{
  float lengthSq = DotProduct(vec, vec);
  double length = sqrt(lengthSq);
  float lengthF = (float)length;

  if ( lengthF != 0.0f )
  {
    float ilength = (float)((double)1.0f / lengthF);
    vec[0] = (float)((double)vec[0] * ilength);
    vec[1] = (float)((double)vec[1] * ilength);
    vec[2] = (float)((double)vec[2] * ilength);
  }

  return lengthF;
}

/*
================
Vec2Normalize

Normalizes 2D vector in place, returns original length.
================
*/
double Vec2Normalize( float *vec )
{
  float lengthSq = DotProduct2D(vec, vec);
  float length = sqrt(lengthSq);

  if ( length != 0.0 )
  {
    float ilength = 1.0 / length;
    vec[0] *= ilength;
    vec[1] *= ilength;
  }

  return length;
}

/*
================
vectopitch_no360

Returns pitch angle in degrees, no [0,360) normalization.
================
*/
double vectopitch_no360( float *vec )
{
	if ( vec[1] == 0.0 && vec[0] == 0.0 )
	{
		if ( vec[2] <= 0.0 )
			return 90.0;
		else
			return -90.0;
	}

	float forwardSq = (float)((double)vec[0] * vec[0] + (double)vec[1] * vec[1]);
	float forward = (float)sqrt(forwardSq);
	{ float atanP = (float)atan2(vec[2], forward);
	return (float)((double)atanP * -180.0 / Q_PI); }
}

/*
================
vectoangles

Converts direction vector to [pitch, yaw, 0] Euler angles.
================
*/
float *vectoangles( float *vec, float *angles )
{
	float yaw, pitch;

	if ( vec[1] == 0.0 && vec[0] == 0.0 )
	{
		yaw = 0.0;
		if ( vec[2] <= 0.0 )
			pitch = 90.0f;
		else
			pitch = 270.0f;
	}
	else
	{
		float atanYaw = (float)atan2(vec[1], vec[0]);
		yaw = (float)((double)atanYaw * 180.0 / Q_PI);
		if ( yaw < 0.0f )
			yaw += 360.0f;

		float forwardSq = (float)((double)vec[0] * vec[0] + (double)vec[1] * vec[1]);
		float forward = (float)sqrt(forwardSq);
		float atanPitch = (float)atan2(vec[2], forward);
		pitch = (float)((double)atanPitch * -180.0 / Q_PI);
		if ( pitch < 0.0f )
			pitch += 360.0f;
	}

	angles[0] = pitch;
	angles[1] = yaw;
	angles[2] = 0.0;
	return angles;
}

/*
================
AngleVectors

Derives forward/right/up direction vectors from Euler angles.
================
*/
float *AngleVectors( float *angles, float *forward, float *right, float *up )
{
  float rad, sy, cy, sp, cp, sr, cr;

  rad = angles[1] * DEG2RAD;
  cy = (float)cos(rad);
  sy = (float)sin(rad);
  rad = angles[0] * DEG2RAD;
  cp = (float)cos(rad);
  sp = (float)sin(rad);

  if ( forward )
  {
    forward[0] = cp * cy;
    forward[1] = cp * sy;
    forward[2] = -sp;
  }

  if ( right || up )
  {
    rad = angles[2] * DEG2RAD;
    cr = (float)cos(rad);
    sr = (float)sin(rad);

    if ( right )
    {
      float srsp = sr * sp;
      right[0] = Det2x2(cr, sy, srsp, cy);
      right[1] = -MulAdd2(cr, cy, srsp, sy);
      right[2] = -(sr * cp);
    }
    if ( up )
    {
      float crsp = cr * sp;
      up[0] = MulAdd2(crsp, cy, sr, sy);
      up[1] = Det2x2(crsp, sy, sr, cy);
      up[2] = cr * cp;
    }
  }

  return forward;
}

/*
================
PerpendicularVector

Finds a vector perpendicular to src, writes to dst.
================
*/
void PerpendicularVector(float *src, float *dst)
{
  float sq[3];
  int minAxis;
  float negComp;

  Assert(Vec3IsNormalized(src), s_assertDisable_PerpendicularVector);

  /* find the axis with smallest absolute component */
  sq[0] = (float)((double)src[0] * src[0]);
  sq[1] = (float)((double)src[1] * src[1]);
  sq[2] = (float)((double)src[2] * src[2]);
  minAxis = sq[1] < sq[0] ? 1 : 0;
  if ( sq[2] < sq[minAxis] )
    minAxis = 2;

  /* build perpendicular: negate src component along minAxis, add 1 on that axis */
  negComp = -src[minAxis];
  dst[0] = (float)((double)negComp * src[0]);
  dst[1] = (float)((double)negComp * src[1]);
  dst[2] = (float)((double)negComp * src[2]);
  dst[minAxis] += 1.0f;
  VecNormalize(dst);
}

/*
================
ProjectPointOntoVector

Projects point onto line segment vStart->vEnd, writes to out.
================
*/
float *ProjectPointOntoVector(float *point, float *vStart, float *vEnd, float *out)
{
  float dir[3], delta[3];
  float dot, lengthSq, t;

  VectorSubtract(vEnd, vStart, dir);
  lengthSq = DotProduct201(dir, dir);

  if ( lengthSq == 0.0 )
  {
    VectorCopy(vStart, out);
  }
  else
  {
    VectorSubtract(point, vStart, delta);
    dot = DotProduct201(delta, dir);
    t = dot / lengthSq;
    VectorMA(vStart, t, dir, out);
  }
  return out;
}

/*
================
ClearBounds

Initialize 3D bounding box to inverted state.
================
*/
float *ClearBounds( float *mins, float *maxs )
{
  mins[0] = MAX_WORLD_COORD;
  mins[1] = MAX_WORLD_COORD;
  mins[2] = MAX_WORLD_COORD;
  maxs[0] = MIN_WORLD_COORD;
  maxs[1] = MIN_WORLD_COORD;
  maxs[2] = MIN_WORLD_COORD;
  return maxs;
}

/*
================
ClearBounds2D

Initialize 2D bounding box to inverted state.
================
*/
float *ClearBounds2D( float *mins, float *maxs )
{
  mins[0] = MAX_WORLD_COORD;
  mins[1] = MAX_WORLD_COORD;
  maxs[0] = MIN_WORLD_COORD;
  maxs[1] = MIN_WORLD_COORD;
  return maxs;
}

/*
================
AddPointToBounds

Expand 3D bounding box to include point.
================
*/
void AddPointToBounds( float *point, float *mins, float *maxs )
{
  if ( *point < (double)*mins )
    *mins = *point;
  if ( *point > (double)*maxs )
    *maxs = *point;
  if ( point[1] < (double)mins[1] )
    mins[1] = point[1];
  if ( point[1] > (double)maxs[1] )
    maxs[1] = point[1];
  if ( point[2] < (double)mins[2] )
    mins[2] = point[2];
  if ( point[2] > (double)maxs[2] )
    maxs[2] = point[2];
}

/*
================
AddPointToBounds2D

Expand 2D bounding box to include point.
================
*/
void AddPointToBounds2D( float *point, float *mins, float *maxs )
{
  if ( *point < (double)*mins )
    *mins = *point;
  if ( *point > (double)*maxs )
    *maxs = *point;
  if ( point[1] < (double)mins[1] )
    mins[1] = point[1];
  if ( point[1] > (double)maxs[1] )
    maxs[1] = point[1];
}

/*
================
PointInBounds

Test if point is inside 3D bounding box.
================
*/
int PointInBounds( float *point, float *mins, float *maxs )
{
  Assert(point, s_assertDisable_PointInBounds);
  Assert(mins, s_assertDisable_PointInBounds);
  Assert(maxs, s_assertDisable_PointInBounds);
  return *point >= (double)*mins
      && *point <= (double)*maxs
      && point[1] >= (double)mins[1]
      && point[1] <= (double)maxs[1]
      && point[2] >= (double)mins[2]
      && point[2] <= (double)maxs[2];
}

/*
================
PointInBounds2D

Test if point is inside 2D bounding box.
================
*/
int PointInBounds2D( float *point, float *mins, float *maxs )
{
  Assert(point, s_assertDisable_PointInBounds);
  Assert(mins, s_assertDisable_PointInBounds);
  Assert(maxs, s_assertDisable_PointInBounds);
  return point[0] >= mins[0]
      && point[0] <= maxs[0]
      && point[1] >= mins[1]
      && point[1] <= maxs[1];
}

/*
================
BoundsIntersect

Test if two 3D bounding boxes overlap.
================
*/
int BoundsIntersect( float *mins1, float *maxs1, float *mins2, float *maxs2 )
{
  return mins1[0] <= (double)maxs2[0]
      && mins2[0] <= (double)maxs1[0]
      && mins1[1] <= (double)maxs2[1]
      && mins2[1] <= (double)maxs1[1]
      && mins1[2] <= (double)maxs2[2]
      && mins2[2] <= (double)maxs1[2];
}

/*
================
BoundsIntersect2D

Test if two 2D bounding boxes overlap.
================
*/
int BoundsIntersect2D( float *mins1, float *maxs1, float *mins2, float *maxs2 )
{
  return mins1[0] <= maxs2[0]
      && mins2[0] <= maxs1[0]
      && mins1[1] <= maxs2[1]
      && mins2[1] <= maxs1[1];
}

/*
================
BoundsIntersectTolerance

Test if two 3D bounding boxes overlap with tolerance.
================
*/
int BoundsIntersectTolerance( float *mins1, float *maxs1, float *mins2, float *maxs2, float tolerance )
{
  return tolerance + maxs2[0] >= mins1[0]
      && tolerance + maxs1[0] >= mins2[0]
      && tolerance + maxs2[1] >= mins1[1]
      && tolerance + maxs1[1] >= mins2[1]
      && tolerance + maxs2[2] >= mins1[2]
      && tolerance + maxs1[2] >= mins2[2];
}

/*
================
AddBoundsToBounds

Expand dst bounds to enclose src bounds.
================
*/
void AddBoundsToBounds( float *srcMins, float *srcMaxs, float *dstMins, float *dstMaxs )
{
  int i;
  for ( i = 0; i < 3; i++ )
  {
    if ( srcMins[i] < dstMins[i] )
      dstMins[i] = srcMins[i];
    if ( srcMaxs[i] > dstMaxs[i] )
      dstMaxs[i] = srcMaxs[i];
  }
}

/*
================
AnglesToMatrix

Convert Euler angles to a 3x3 axis matrix.
Calls AngleVectors for forward/right/up, then negates the right vector.
================
*/
static void AnglesToMatrixSinCos(float radians, float *sinOut, float *cosOut)
{
#if defined(_M_IX86)
  __asm
  {
    fld radians
    fsincos
    mov eax, cosOut
    fstp dword ptr [eax]
    mov eax, sinOut
    fstp dword ptr [eax]
  }
#else
  *cosOut = (float)cos(radians);
  *sinOut = (float)sin(radians);
#endif
}

float *AnglesToMatrix(float *angles, float *axis)
{
  float radians;
  float sinYaw, cosYaw;
  float sinPitch, cosPitch;
  float sinRoll, cosRoll;

  radians = (float)((double)angles[1] * 0.01745329238474369);
  AnglesToMatrixSinCos(radians, &sinYaw, &cosYaw);
  radians = (float)((double)angles[0] * 0.01745329238474369);
  AnglesToMatrixSinCos(radians, &sinPitch, &cosPitch);

  axis[0] = cosPitch * cosYaw;
  axis[1] = cosPitch * sinYaw;
  axis[2] = -sinPitch;

  radians = (float)((double)angles[2] * 0.01745329238474369);
  AnglesToMatrixSinCos(radians, &sinRoll, &cosRoll);
  axis[3] = sinRoll * sinPitch * cosYaw + -sinYaw * cosRoll;
  axis[4] = sinRoll * sinPitch * sinYaw + cosRoll * cosYaw;
  axis[5] = sinRoll * cosPitch;
  axis[6] = cosRoll * sinPitch * cosYaw + -sinRoll * -sinYaw;
  axis[7] = cosRoll * sinPitch * sinYaw + -sinRoll * cosYaw;
  axis[8] = cosRoll * cosPitch;
  return axis;
}

/*
================
ApplyRotationToAngles

Extract roll angle from rotated source axis.
================
*/
void ApplyRotationToAngles(float *srcAxis, float *angles)
{
  float srcRight_x, srcUp;
  float rad;
  float cosVal, sinVal;
  float rotX_temp;
  
  /* rotVec[0..2] must be contiguous — vectopitch_no360 reads all 3 */
  float rotVec[3];
  float pitch;

  vectoangles(srcAxis, angles);
  srcRight_x = srcAxis[4];
  srcUp = srcAxis[5];
  rad = -angles[1] * DEG2RAD;
  rotVec[0] = srcAxis[3];
  cosVal = (float)cos(rad);
  sinVal = (float)sin(rad);
  rotX_temp = Det2x2(rotVec[0], cosVal, srcRight_x, sinVal);
  rotVec[1] = MulAdd2(rotVec[0], sinVal, srcRight_x, cosVal);
  rad = -angles[0] * DEG2RAD;
  cosVal = (float)cos(rad);
  sinVal = (float)sin(rad);
  rotVec[0] = MulAdd2(srcUp, sinVal, rotX_temp, cosVal);
  rotVec[2] = Det2x2(srcUp, cosVal, rotX_temp, sinVal);
  pitch = vectopitch_no360(rotVec);
  if ( rotVec[1] < 0.0f )
  {
    float adj = 180.0f;
    if ( (double)pitch >= 0.0f )
      adj = -180.0f;
    angles[2] = adj + pitch;
  }
  else
  {
    angles[2] = -(double)pitch;
  }
}

/*
================
PlaneIntersection3

Find intersection point of 3 planes via Cramer's rule.
================
*/
int PlaneIntersection3(float **planes, float *outPoint)
{
  float *p0, *p1, *p2;
  double det, invDet;

  p2 = planes[2];
  p0 = planes[0];
  p1 = planes[1];

  det = Det2x2(p1[2],p0[1],p1[1],p0[2]) * p2[0]
      + Det2x2(p2[1],p0[2],p2[2],p0[1]) * p1[0]
      + Det2x2(p2[2],p1[1],p1[2],p2[1]) * p0[0];

  if ( fabs(det) < PLANESIDE_EPSILON )
    return 0;
  invDet = 1.0 / det;

  outPoint[0] = (Det2x2(p1[2],p0[1],p1[1],p0[2]) * p2[3]
               + Det2x2(p2[1],p0[2],p2[2],p0[1]) * p1[3]
               + Det2x2(p2[2],p1[1],p1[2],p2[1]) * p0[3]) * invDet;
  outPoint[1] = (Det2x2(p1[0],p0[2],p1[2],p0[0]) * p2[3]
               + Det2x2(p0[0],p2[2],p2[0],p0[2]) * p1[3]
               + Det2x2(p1[2],p2[0],p1[0],p2[2]) * p0[3]) * invDet;
  outPoint[2] = (Det2x2(p1[0],p2[1],p1[1],p2[0]) * p0[3]
               + Det2x2(p2[0],p0[1],p0[0],p2[1]) * p1[3]
               + Det2x2(p0[0],p1[1],p1[0],p0[1]) * p2[3]) * invDet;

  return 1;
}

/*
================
SnapPlaneIntersection

Snap 3-plane intersection point to grid.
================
*/
void SnapPlaneIntersection(float **planes, float *point, float gridSize, float tolerance)
{
  float snapped[3];
  int axis;

  for (axis = 0; axis < 3; axis++) {
    float coord = point[axis];
    float gridUnits = coord / gridSize;
    int nearestGrid = RoundFloatToInt(gridUnits);
    float snappedCoord = (float)nearestGrid * gridSize;
    float snapDist = fabs(snappedCoord - coord);
    if (snapDist < (double)tolerance)
      snapped[axis] = snappedCoord;
    else
      snapped[axis] = coord;
  }

  if (snapped[0] != point[0] || snapped[1] != point[1] || snapped[2] != point[2]) {
    float maxAllowed = tolerance;
    float maxError = 0.0f;

    /* KIWI: the allowance is the UNSNAPPED point's own error (as the engine's twin,
       universal/com_math.cpp) - both came from the snapped point, so a snap that improved
       an intersection already off its planes by more than tolerance was refused. */
    for (axis = 0; axis < 3; axis++) {
      float *plane = planes[axis];
      float absDist = fabs(DotProduct210(snapped, plane) - plane[3]);
      float baseDist = fabs(DotProduct210(point, plane) - plane[3]);
      if (absDist > maxError)
        maxError = absDist;
      if (baseDist > maxAllowed)
        maxAllowed = baseDist;
    }

    if (maxError < maxAllowed) {
      point[0] = snapped[0];
      point[1] = snapped[1];
      point[2] = snapped[2];
    }
  }
}

/*
================
SnapPointToPlanes

Project a point onto nearby planes, then snap each coordinate
to the nearest grid point. Validates the snapped position
against all planes before accepting.
================
*/
void SnapPointToPlanes(float *planeArray, int numPlanes, float *point, float gridSize, float tolerance)
{
  int i, k;
  float *pl;
  float planeDist;
  float snapped[3];
  float gridUnits, snappedCoord, snapError;
  int gridInt;
  float maxError, maxAbsDist, fabsDist;

  /* project point onto each nearby plane */
  for ( i = 0; i < numPlanes; i++ )
  {
    pl = &planeArray[4 * i];
    planeDist = DotProduct201(pl, point) - pl[3];
    if ( planeDist <= tolerance && planeDist >= -tolerance )
    {
      VectorMA(point, -planeDist, pl, point);
    }
  }

  /* snap each coordinate to grid if within tolerance */
  for ( k = 0; k < 3; k++ )
  {
    gridUnits = point[k] / gridSize;
    gridInt = fistp_add(gridUnits, FISTP_BIAS);
    snappedCoord = (float)gridInt * gridSize;
    snapError = fabs(snappedCoord - point[k]);
    snapped[k] = (snapError < tolerance) ? snappedCoord : point[k];
  }

  /* accept snapped position only if it's closer to all planes */
  if ( snapped[0] != point[0] || snapped[1] != point[1] || snapped[2] != point[2] )
  {
    maxError = 0.0;
    maxAbsDist = tolerance;

    for ( i = 0; i < numPlanes; i++ )
    {
      pl = &planeArray[4 * i];
      planeDist = DotProduct210(pl, snapped) - pl[3];
      fabsDist = (float)fabs(planeDist);
      if ( fabsDist > maxError )
        maxError = fabsDist;
      if ( fabsDist > maxAbsDist )
        maxAbsDist = fabsDist;
    }

    if ( maxError < maxAbsDist )
      VectorCopy(snapped, point);
  }
}

/*
================
PlaneFromPoints

Returns 0 if the triangle is degenerate.
================
*/
#if defined(_MSC_VER)
__declspec(noinline)
#endif
static float PlaneVectorLengthSquaredNative(const float *vector)
{
	float lengthSquared;

#if defined(_M_IX86)
	__asm
	{
		mov eax, vector
		fld dword ptr [eax]
		fmul dword ptr [eax]
		fld dword ptr [eax+4]
		fmul dword ptr [eax+4]
		faddp st(1), st
		fld dword ptr [eax+8]
		fmul dword ptr [eax+8]
		faddp st(1), st
		fstp lengthSquared
	}
#else
	lengthSquared = (float)((double)vector[0] * vector[0]
		+ (double)vector[1] * vector[1]
		+ (double)vector[2] * vector[2]);
#endif
	return lengthSquared;
}

static void PlaneNormalizeDirectNative(float *plane, float normalLength)
{
#if defined(_M_IX86)
	__asm
	{
		mov eax, plane
		fld normalLength
		fld dword ptr [eax]
		fdiv st, st(1)
		fstp dword ptr [eax]
		fld dword ptr [eax+4]
		fdiv st, st(1)
		fstp dword ptr [eax+4]
		fld dword ptr [eax+8]
		fdiv st, st(1)
		fstp dword ptr [eax+8]
		fstp st(0)
	}
#else
	plane[0] = plane[0] / normalLength;
	plane[1] = plane[1] / normalLength;
	plane[2] = plane[2] / normalLength;
#endif
}

int PlaneFromPoints( float *planeOut, float *point0, float *point1, float *point2 )
{
	vec3_t	d1, d2;
	float lengthSquared;
	float length;

	VectorSubtract( point1, point0, d1 );
	VectorSubtract( point2, point0, d2 );

	CrossProduct( d2, d1, planeOut );
	lengthSquared = PlaneVectorLengthSquaredNative( planeOut );

	/* 0x473871..0x47393A: small crosses must also be large enough relative
	   to their source edges.  Retry about point1 before declaring a nearly
	   collinear triangle degenerate; the original cross length remains the
	   normalization scale exactly as in the native routine. */
	if ( lengthSquared < 2.0f )
	{
		const double relativeEpsilon = 0.00000100000011116208;
		double firstEdgeLengthSquared;
		double secondEdgeLengthSquared;

		if ( lengthSquared == 0.0f )
			return 0;
		firstEdgeLengthSquared = PlaneVectorLengthSquaredNative( d1 );
		secondEdgeLengthSquared = PlaneVectorLengthSquaredNative( d2 );
		if ( (double)lengthSquared <=
			secondEdgeLengthSquared * firstEdgeLengthSquared * relativeEpsilon )
		{
			VectorSubtract( point2, point1, d1 );
			VectorSubtract( point0, point1, d2 );
			CrossProduct( d2, d1, planeOut );
			firstEdgeLengthSquared = PlaneVectorLengthSquaredNative( d1 );
			secondEdgeLengthSquared = PlaneVectorLengthSquaredNative( d2 );
			if ( (double)lengthSquared <=
				secondEdgeLengthSquared * firstEdgeLengthSquared * relativeEpsilon )
				return 0;
		}
	}

	/* Native 0x473820 deliberately divides every component by the computed
	   length.  VectorNormalize instead forms one reciprocal and multiplies;
	   that rounding difference is large enough to defeat FindFloatPlane's
	   strict 1e-7 comparison for equal planes produced at different scales. */
	length = sqrtf( lengthSquared );
	PlaneNormalizeDirectNative( planeOut, length );

	planeOut[3] = DotProductDP(planeOut, point0);
	return 1;
}

/*
================
Q_rint

Round float to nearest integer.
================
*/
double Q_rint( float val )
{
	return (float)floor( val + 0.5 );
}

int ProjectedWindingContainsCoplanarPoint(
  const float *verts,
  int vertCount,
  int x,
  int y,
  const float *point,
  float epsilon)
{
  int prev = vertCount - 1;
  int i;

  for (i = 0; i < vertCount; ++i)
  {
    float edgeX = verts[3 * i + y] - verts[3 * prev + y];
    float edgeY = verts[3 * prev + x] - verts[3 * i + x];
    float pointX = point[x] - verts[3 * prev + x];
    float pointY = point[y] - verts[3 * prev + y];
    float projectionDist = edgeY * pointY + edgeX * pointX;
    float edgeLengthSq = edgeX * edgeX + edgeY * edgeY;
    if (projectionDist < 0.0f && edgeLengthSq * epsilon < projectionDist * projectionDist)
      return 0;
    prev = i;
  }
  return 1;
}

/*
================
SinCos

Computes sine and cosine for a given angle in degrees.
Special-cases 0, 90, 180, 270 for exact results.
================
*/
void SinCos( float degrees, float *sinOut, float *cosOut )
{
	float	radians;

	Assert(sinOut != 0, s_assertDisable_SinCos);
	Assert(cosOut != 0, s_assertDisable_SinCos);

	if ( degrees < 0.0 )
		degrees = degrees + 360.0;

	if ( degrees == 0.0 )
	{
		*cosOut = 1.0;
		*sinOut = 0.0;
	}
	else if ( degrees == 90.0 )
	{
		*cosOut = 0.0;
		*sinOut = 1.0;
	}
	else if ( degrees == 180.0 )
	{
		*cosOut = -1.0;
		*sinOut = 0.0;
	}
	else if ( degrees == 270.0 )
	{
		*cosOut = 0.0;
		*sinOut = -1.0;
	}
	else
	{
		radians = degrees * DEG2RAD;
		*cosOut = cos( radians );
		*sinOut = sin( radians );
	}
}

/*
================
SnapToIntegralPowerOf2

Snaps a float value's IEEE 754 representation to the nearest
integral power of 2 boundary if within tolerance.
================
*/
double SnapToIntegralPowerOf2( float value, int tolerance, char powerBits )
{
	union { float f; int i; } val, res;
	int		power;
	int		fraction;

	power = 1 << powerBits;
	val.f = value;
	res.f = value;
	fraction = val.i & (power - 1);
	if ( fraction <= tolerance )
		res.i = val.i - fraction;
	else if ( power - fraction <= tolerance )
		res.i = power - fraction + val.i;
	return res.f;
}

/*
================
VectorDistance

Returns the 3D distance between two points.
================
*/
double VectorDistance( float *point1, float *point2 )
{
	vec3_t delta;

	VectorSubtract(point2, point1, delta);
	return (float)VectorLength(delta);
}

/*
================
DistanceSquared

Returns the squared 3D distance between two points.
================
*/
double DistanceSquared( float *point1, float *point2 )
{
	vec3_t delta;

	VectorSubtract(point2, point1, delta);
	return DotProduct210(delta, delta);
}

/*
================
VecLargestAxis

Returns the index (0, 1, or 2) of the axis with the
largest-magnitude component in the given vector.
================
*/
int VecLargestAxis( float *v )
{
	float	absV[3];
	int	largest;

	Assert(v != 0, s_assertDisable_VecLargestAxis);

	/* The CoD4 binary takes absolute values before doing strict comparisons.
	 * Squaring is usually equivalent, but changes overflow and NaN behavior. */
	absV[0] = (float)fabs(v[0]);
	absV[1] = (float)fabs(v[1]);
	absV[2] = (float)fabs(v[2]);
	largest = absV[0] < absV[1];
	if ( absV[largest] < absV[2] )
		return 2;
	return largest;
}

/*
================
GetProjectionAxes

Given a normal vector, determines the two axis indices
for projecting onto the plane perpendicular to the dominant axis.
Output axis order depends on the sign of the dominant component.
================
*/
void GetProjectionAxes( float *normal, int *axisU, int *axisV )
{
	float	xx, yy, zz;

	xx = normal[0] * normal[0];
	yy = normal[1] * normal[1];
	zz = normal[2] * normal[2];

	if ( !((double)zz >= (double)xx) || !((double)zz >= (double)yy) )
	{
		if ( !((double)yy >= (double)xx) || !((double)yy >= (double)zz) )
		{
			/* X is dominant */
			if ( !(normal[0] > 0.0) )
			{
				*axisU = 2;
				*axisV = 1;
			}
			else
			{
				*axisU = 1;
				*axisV = 2;
			}
		}
		else
		{
			/* Y is dominant */
			if ( !(normal[1] > 0.0) )
			{
				*axisU = 0;
				*axisV = 2;
			}
			else
			{
				*axisU = 2;
				*axisV = 0;
			}
		}
	}
	else
	{
		/* Z is dominant */
		if ( !(normal[2] > 0.0) )
		{
			*axisU = 1;
			*axisV = 0;
		}
		else
		{
			*axisU = 0;
			*axisV = 1;
		}
	}
}

/*
================
Vec3NormalizeTo

Normalizes vector 'in' and stores the result in 'out'.
Returns the original length. If zero-length, 'out' is zeroed.
================
*/
double Vec3NormalizeTo( float *in, float *out )
{
	float	lengthSq, length, invLength;

	lengthSq = DotProduct(in, in);
	length = (float)sqrt( lengthSq );
	if ( length == 0.0f )
	{
		VectorClear(out);
	}
	else
	{
		invLength = 1.0f / length;
		out[0] = invLength * in[0];
		out[1] = invLength * in[1];
		out[2] = invLength * in[2];
	}
	return length;
}

/*
================
MakeNormalVectors

Given a normalized forward vector, create two
other perpendicular vectors (right and up).
================
*/
float *MakeNormalVectors( float *forward, float *right, float *up )
{
	float	dot;

	/* rotate and negate to get a non-colinear vector */
	right[1] = -forward[0];
	right[2] = forward[1];
	right[0] = forward[2];

	/* orthogonalize: right -= dot(forward, right) * forward */
	dot = DotProduct021(forward, right);
	VectorMA(right, -dot, forward, right);
	VecNormalize( right );

	/* up = right x forward */
	CrossProduct( right, forward, up );
	return up;
}

int CullBoxFromSphere(const float *sphereOrg, float radius, const float *boxCenter, const float *boxHalfSize)
{
  float distFromBoxToMid[3];
  float delta;

  delta = fabsf(sphereOrg[0] - boxCenter[0]) - boxHalfSize[0];
  distFromBoxToMid[0] = delta < 0.0f ? 0.0f : delta;
  delta = fabsf(sphereOrg[1] - boxCenter[1]) - boxHalfSize[1];
  distFromBoxToMid[1] = delta < 0.0f ? 0.0f : delta;
  delta = fabsf(sphereOrg[2] - boxCenter[2]) - boxHalfSize[2];
  distFromBoxToMid[2] = delta < 0.0f ? 0.0f : delta;
  return radius * radius < (float)VectorLengthSquared(distFromBoxToMid);
}

int CullBoxFromConicSectionOfSphere(
  const float *coneOrg,
  const float *coneDir,
  float cosHalfFov,
  float radius,
  const float *boxCenter,
  const float *boxHalfSize)
{
  float deltaMid[3], distFromBoxToMid[3], farCorner[3];
  float perpendicular[3], scaledSepAxis[3];
  float delta, dist, perpLenSq, cosHalfFovSq, sinHalfFovSq, scale, scaledSepDist;

  Assert(cosHalfFov >= 0.0f, s_assertDisable_CullBoxFromConicSectionOfSphere);
  VectorSubtract(boxCenter, coneOrg, deltaMid);

  delta = fabsf(deltaMid[0]) - boxHalfSize[0];
  distFromBoxToMid[0] = delta < 0.0f ? 0.0f : delta;
  delta = fabsf(deltaMid[1]) - boxHalfSize[1];
  distFromBoxToMid[1] = delta < 0.0f ? 0.0f : delta;
  delta = fabsf(deltaMid[2]) - boxHalfSize[2];
  distFromBoxToMid[2] = delta < 0.0f ? 0.0f : delta;
  if (radius * radius < (float)VectorLengthSquared(distFromBoxToMid))
    return 1;

  farCorner[0] = deltaMid[0] - boxHalfSize[0] * (coneDir[0] < 0.0f ? -1.0f : 1.0f);
  farCorner[1] = deltaMid[1] - boxHalfSize[1] * (coneDir[1] < 0.0f ? -1.0f : 1.0f);
  farCorner[2] = deltaMid[2] - boxHalfSize[2] * (coneDir[2] < 0.0f ? -1.0f : 1.0f);
  dist = (float)DotProduct(farCorner, coneDir);
  if (dist >= 0.0f)
    return 1;

  VectorMA(farCorner, -dist, coneDir, perpendicular);
  perpLenSq = (float)VectorLengthSquared(perpendicular);
  cosHalfFovSq = cosHalfFov * cosHalfFov;
  sinHalfFovSq = 1.0f - cosHalfFovSq;
  if (dist * dist * sinHalfFovSq >= perpLenSq * cosHalfFovSq)
    return 0;

  scale = cosHalfFov / sqrtf(perpLenSq * sinHalfFovSq);
  VectorMA(coneDir, scale, perpendicular, scaledSepAxis);
  scaledSepDist = (float)DotProduct(scaledSepAxis, deltaMid);
  scaledSepDist -= fabsf(scaledSepAxis[0] * boxHalfSize[0]);
  scaledSepDist -= fabsf(scaledSepAxis[1] * boxHalfSize[1]);
  scaledSepDist -= fabsf(scaledSepAxis[2] * boxHalfSize[2]);
  return scaledSepDist >= 0.0f;
}


/*
================
RoundFloatToInt

Rounds a float to the nearest integer with FISTP_BIAS for precision matching.
================
*/
int RoundFloatToInt(float value)
{
  return xs_RoundToInt((double)value + FISTP_BIAS);
}

/*
================
fistp_sub

Replaces lrint(val - bias) pattern with 80-bit precision round-to-nearest.
================
*/
int fistp_sub(float val, double bias)
{
  return xs_RoundToInt((double)val - bias);
}

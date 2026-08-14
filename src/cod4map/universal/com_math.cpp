/*
com_math.c — Math utilities

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

float g_identityMatrix44[16] = {
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f
};

char s_assertDisable_BoxOnPlaneSide;
char s_assertDisable_MatrixIdentity44;
char s_assertDisable_MatrixInverse44;
char s_assertDisable_MatrixInverse44;
char s_assertDisable_MatrixMultiply44;
char s_assertDisable_MatrixMultiply44;
char s_assertDisable_MatrixTransformVector;
char s_assertDisable_MatrixTranspose33;
char s_assertDisable_MatrixTranspose44;
char s_assertDisable_CullBoxFromConicSectionOfSphere;
char s_assertDisable_AxisToQuat;
char s_assertDisable_ConvertQuatToMat;
char s_assertDisable_PerpendicularVector;
char s_assertDisable_PointInBounds;
char s_assertDisable_PointInBounds;
char s_assertDisable_PointInBounds;
char s_assertDisable_ProjectPointOnPlane;
char s_assertDisable_QuatToAxis;
char s_assertDisable_QuatToAxis;
char s_assertDisable_QuatToAxis;
char s_assertDisable_RotatePointAroundVector;
char s_assertDisable_RotatePointAroundVector;
char s_assertDisable_RotatePointAroundVector;
char s_assertDisable_RotatePointAroundVector;
char s_assertDisable_SetPerspectiveProjection;
char s_assertDisable_SetPerspectiveProjection;
char s_assertDisable_SinCos;
char s_assertDisable_SinCos;
char s_assertDisable_SnapToGrid;
char s_assertDisable_SnapToGrid;
char s_assertDisable_SnapToGrid;
char s_assertDisable_VecLargestAxis;
char s_assertDisable_CrossProduct;


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
vectoyaw

Returns yaw angle in degrees from a direction vector.
================
*/
double vectoyaw( float *vec )
{
  float yaw;

  if ( vec[1] == 0.0 && vec[0] == 0.0 )
    return 0.0;

  { float atanVal = (float)atan2(vec[1], vec[0]);
  yaw = (float)((double)atanVal * 180.0 / Q_PI); }
  if ( yaw < 0.0f )
    yaw += 360.0f;
  return yaw;
}

/*
================
vectopitch

Returns pitch angle in degrees from a direction vector,
normalized to [0,360).
================
*/
double vectopitch( float *vec )
{
	if ( vec[1] == 0.0 && vec[0] == 0.0 )
	{
		if ( vec[2] <= 0.0 )
			return 90.0;
		else
			return 270.0;
	}

	float forwardSq = (float)((double)vec[0] * vec[0] + (double)vec[1] * vec[1]);
	float forward = (float)sqrt(forwardSq);
	float atanP = (float)atan2(vec[2], forward);
	float pitch = (float)((double)atanP * -180.0 / Q_PI);
	if ( pitch < 0.0f )
		pitch += 360.0f;
	return pitch;
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
vectoangles_alt

Converts direction vector to [pitch, yaw, 0], no 360 wrap.
================
*/
float *vectoangles_alt( float *vec, float *angles )
{
  if ( vec[1] == 0.0 && vec[0] == 0.0 )
  {
    if ( vec[2] <= 0.0 )
      angles[0] = 90.0;
    else
      angles[0] = -90.0;
    angles[1] = 0.0;
    angles[2] = 0.0;
  }
  else
  {
    float atanYaw = (float)atan2(vec[1], vec[0]);
    float yaw = (float)((double)atanYaw * 180.0 / Q_PI);
    float forwardSq = (float)((float)(vec[0] * vec[0]) + (float)(vec[1] * vec[1]));
    float forward = (float)sqrt(forwardSq);
    float atanPitch = (float)atan2(vec[2], forward);
    float pitch = (float)((double)atanPitch * -180.0 / Q_PI);
    angles[0] = pitch;
    angles[1] = yaw;
    angles[2] = 0.0;
  }
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
YawVectors

Derives forward/right direction vectors from a yaw angle.
================
*/
float *YawVectors( float yaw, float *forward, float *right )
{
	float rad = yaw * DEG2RAD;
	float cy = cos(rad);
	float sy = sin(rad);
	float *result = 0;

	if ( forward )
	{
		forward[0] = cy;
		forward[1] = sy;
		forward[2] = 0.0f;
		result = forward;
	}
	if ( right )
	{
		right[0] = sy;
		right[1] = -cy;
		right[2] = 0.0f;
		result = right;
	}
	return result;
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
MatrixIdentity44

Sets a 4x4 matrix to identity.
================
*/
void MatrixIdentity44( void *mtx )
{
  Assert( mtx, s_assertDisable_MatrixIdentity44 );
  memcpy( mtx, &g_identityMatrix44, sizeof(g_identityMatrix44) );
}

/*
================
MatrixMultiply33

out = in1 * in2 (3x3 row-major)
================
*/
float *MatrixMultiply33( float *in1, float *in2, float *out )
{
#ifdef _WIN64
  #define MM3(a,b,c,d,e,f) (float)((double)(a)*(b) + (double)(c)*(d) + (double)(e)*(f))
  out[0] = MM3(in1[0],in2[0], in1[1],in2[3], in1[2],in2[6]);
  out[1] = MM3(in1[0],in2[1], in1[1],in2[4], in1[2],in2[7]);
  out[2] = MM3(in1[0],in2[2], in1[1],in2[5], in1[2],in2[8]);
  out[3] = MM3(in1[3],in2[0], in1[4],in2[3], in1[5],in2[6]);
  out[4] = MM3(in1[3],in2[1], in1[4],in2[4], in1[5],in2[7]);
  out[5] = MM3(in1[3],in2[2], in1[4],in2[5], in1[5],in2[8]);
  out[6] = MM3(in1[6],in2[0], in1[7],in2[3], in1[8],in2[6]);
  out[7] = MM3(in1[6],in2[1], in1[7],in2[4], in1[8],in2[7]);
  out[8] = MM3(in1[6],in2[2], in1[7],in2[5], in1[8],in2[8]);
  #undef MM3
#else
  out[0] = in1[0] * in2[0] + in1[1] * in2[3] + in1[2] * in2[6];
  out[1] = in1[0] * in2[1] + in1[1] * in2[4] + in1[2] * in2[7];
  out[2] = in1[0] * in2[2] + in1[1] * in2[5] + in1[2] * in2[8];
  out[3] = in1[3] * in2[0] + in1[4] * in2[3] + in1[5] * in2[6];
  out[4] = in1[3] * in2[1] + in1[4] * in2[4] + in1[5] * in2[7];
  out[5] = in1[3] * in2[2] + in1[4] * in2[5] + in1[5] * in2[8];
  out[6] = in1[6] * in2[0] + in1[7] * in2[3] + in1[8] * in2[6];
  out[7] = in1[6] * in2[1] + in1[7] * in2[4] + in1[8] * in2[7];
  out[8] = in1[6] * in2[2] + in1[7] * in2[5] + in1[8] * in2[8];
#endif
  return in1;
}

/*
================
MatrixMultiply44

4x4 matrix multiply: out = in1 * in2
================
*/
void MatrixMultiply44(float *in1, float *in2, float *out)
{
  Assert( in1 != out, s_assertDisable_MatrixMultiply44 );
  Assert( in2 != out, s_assertDisable_MatrixMultiply44 );

#ifdef _WIN64
  #define MM4(a,b,c,d,e,f,g,h) (float)((double)(a)*(b) + (double)(c)*(d) + (double)(e)*(f) + (double)(g)*(h))
  out[0]  = MM4(in1[0],in2[0],  in1[1],in2[4],  in1[2],in2[8],  in1[3],in2[12]);
  out[1]  = MM4(in1[0],in2[1],  in1[1],in2[5],  in1[2],in2[9],  in1[3],in2[13]);
  out[2]  = MM4(in1[0],in2[2],  in1[1],in2[6],  in1[2],in2[10], in1[3],in2[14]);
  out[3]  = MM4(in1[0],in2[3],  in1[1],in2[7],  in1[2],in2[11], in1[3],in2[15]);
  out[4]  = MM4(in1[4],in2[0],  in1[5],in2[4],  in1[6],in2[8],  in1[7],in2[12]);
  out[5]  = MM4(in1[4],in2[1],  in1[5],in2[5],  in1[6],in2[9],  in1[7],in2[13]);
  out[6]  = MM4(in1[4],in2[2],  in1[5],in2[6],  in1[6],in2[10], in1[7],in2[14]);
  out[7]  = MM4(in1[4],in2[3],  in1[5],in2[7],  in1[6],in2[11], in1[7],in2[15]);
  out[8]  = MM4(in1[8],in2[0],  in1[9],in2[4],  in1[10],in2[8],  in1[11],in2[12]);
  out[9]  = MM4(in1[8],in2[1],  in1[9],in2[5],  in1[10],in2[9],  in1[11],in2[13]);
  out[10] = MM4(in1[8],in2[2],  in1[9],in2[6],  in1[10],in2[10], in1[11],in2[14]);
  out[11] = MM4(in1[8],in2[3],  in1[9],in2[7],  in1[10],in2[11], in1[11],in2[15]);
  out[12] = MM4(in1[12],in2[0], in1[13],in2[4], in1[14],in2[8],  in1[15],in2[12]);
  out[13] = MM4(in1[12],in2[1], in1[13],in2[5], in1[14],in2[9],  in1[15],in2[13]);
  out[14] = MM4(in1[12],in2[2], in1[13],in2[6], in1[14],in2[10], in1[15],in2[14]);
  out[15] = MM4(in1[12],in2[3], in1[13],in2[7], in1[14],in2[11], in1[15],in2[15]);
  #undef MM4
#else
  out[0]  = in1[0] * in2[0]  + in1[1] * in2[4]  + in1[2] * in2[8]  + in1[3] * in2[12];
  out[1]  = in1[0] * in2[1]  + in1[1] * in2[5]  + in1[2] * in2[9]  + in1[3] * in2[13];
  out[2]  = in1[0] * in2[2]  + in1[1] * in2[6]  + in1[2] * in2[10] + in1[3] * in2[14];
  out[3]  = in1[0] * in2[3]  + in1[1] * in2[7]  + in1[2] * in2[11] + in1[3] * in2[15];
  out[4]  = in1[4] * in2[0]  + in1[5] * in2[4]  + in1[6] * in2[8]  + in1[7] * in2[12];
  out[5]  = in1[4] * in2[1]  + in1[5] * in2[5]  + in1[6] * in2[9]  + in1[7] * in2[13];
  out[6]  = in1[4] * in2[2]  + in1[5] * in2[6]  + in1[6] * in2[10] + in1[7] * in2[14];
  out[7]  = in1[4] * in2[3]  + in1[5] * in2[7]  + in1[6] * in2[11] + in1[7] * in2[15];
  out[8]  = in1[8] * in2[0]  + in1[9] * in2[4]  + in1[10] * in2[8]  + in1[11] * in2[12];
  out[9]  = in1[8] * in2[1]  + in1[9] * in2[5]  + in1[10] * in2[9]  + in1[11] * in2[13];
  out[10] = in1[8] * in2[2]  + in1[9] * in2[6]  + in1[10] * in2[10] + in1[11] * in2[14];
  out[11] = in1[8] * in2[3]  + in1[9] * in2[7]  + in1[10] * in2[11] + in1[11] * in2[15];
  out[12] = in1[12] * in2[0] + in1[13] * in2[4] + in1[14] * in2[8]  + in1[15] * in2[12];
  out[13] = in1[12] * in2[1] + in1[13] * in2[5] + in1[14] * in2[9]  + in1[15] * in2[13];
  out[14] = in1[12] * in2[2] + in1[13] * in2[6] + in1[14] * in2[10] + in1[15] * in2[14];
  out[15] = in1[12] * in2[3] + in1[13] * in2[7] + in1[14] * in2[11] + in1[15] * in2[15];
#endif
}

/*
================
MatrixTranspose33

3x3 matrix transpose: out[i][j] = in[j][i]
================
*/
void MatrixTranspose33( float *in, float *out )
{
  Assert( in != out, s_assertDisable_MatrixTranspose33 );

  out[0] = in[0];  out[1] = in[3];  out[2] = in[6];
  out[3] = in[1];  out[4] = in[4];  out[5] = in[7];
  out[6] = in[2];  out[7] = in[5];  out[8] = in[8];
}

/*
================
MatrixTranspose44

4x4 matrix transpose: out[i][j] = in[j][i]
================
*/
void MatrixTranspose44( float *in, float *out )
{
  Assert( in != out, s_assertDisable_MatrixTranspose44 );

  out[0]  = in[0];  out[1]  = in[4];  out[2]  = in[8];  out[3]  = in[12];
  out[4]  = in[1];  out[5]  = in[5];  out[6]  = in[9];  out[7]  = in[13];
  out[8]  = in[2];  out[9]  = in[6];  out[10] = in[10]; out[11] = in[14];
  out[12] = in[3];  out[13] = in[7];  out[14] = in[11]; out[15] = in[15];
}

/*
================
QuatToAxis

Convert a normalized quaternion to a 3x3 rotation matrix.
================
*/
void QuatToAxis(float *quat, float *axis)
{
	float x2, y2, z2;
	float xx, xy, xz, xw;
	float yy, yz, yw, zz, zw;

	Assert(fabs(quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2] + quat[3] * quat[3] - 1.0f) < 0.002f, s_assertDisable_QuatToAxis);

	/* CoD4 accepts a four-float quaternion and derives this scale itself. */
	x2 = 2.0f * quat[0];
	y2 = 2.0f * quat[1];
	z2 = 2.0f * quat[2];
	xx = x2 * quat[0];
	xy = x2 * quat[1];
	xz = x2 * quat[2];
	xw = x2 * quat[3];
	yy = y2 * quat[1];
	yz = y2 * quat[2];
	yw = y2 * quat[3];
	zz = z2 * quat[2];
	zw = z2 * quat[3];
	axis[0] = 1.0f - (yy + zz);
	axis[1] = xy + zw;
	axis[2] = xz - yw;
	axis[3] = xy - zw;
	axis[4] = 1.0f - (xx + zz);
	axis[5] = yz + xw;
	axis[6] = xz + yw;
	axis[7] = yz - xw;
	axis[8] = 1.0f - (xx + yy);
}

float Vec4Dot(const float *a, const float *b)
{
  return (float)((double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2] + (double)a[3] * b[3]);
}

float Vec4LengthSq(const float *v)
{
  return v[0] * v[0] + v[1] * v[1] + v[2] * v[2] + v[3] * v[3];
}

int Vec4IsNormalized(const float *v)
{
  return fabsf(Vec4LengthSq(v) - 1.0f) < 0.0020000001f;
}

float *Vec3Mul(const float *a, const float *b, float *product)
{
  product[0] = a[0] * b[0];
  product[1] = a[1] * b[1];
  product[2] = a[2] * b[2];
  return (float *)a;
}

float *Vec4Lerp(const float *from, const float *to, float frac, float *result)
{
  result[0] = (to[0] - from[0]) * frac + from[0];
  result[1] = (to[1] - from[1]) * frac + from[1];
  result[2] = (to[2] - from[2]) * frac + from[2];
  result[3] = (to[3] - from[3]) * frac + from[3];
  return result;
}

float Q_fabs(float value)
{
  union { float f; unsigned int u; } bits;
  bits.f = value;
  bits.u &= 0x7FFFFFFFu;
  return bits.f;
}

void FastSinCos(float radians, float *sinOut, float *cosOut)
{
  *sinOut = sinf(radians);
  *cosOut = cosf(radians);
}

float *ConvertQuatToMat(float *mat, float *axis)
{
  float scaled[3];
  float xx, xy, xz, xw, yy, yz, yw, zz, zw;

  Assert(!isnan(mat[0]) && !isnan(mat[1]) && !isnan(mat[2]) && !isnan(mat[3]), s_assertDisable_ConvertQuatToMat);
  Assert(!isnan(mat[7]), s_assertDisable_ConvertQuatToMat);
  scaled[0] = mat[0] * mat[7]; scaled[1] = mat[1] * mat[7]; scaled[2] = mat[2] * mat[7];
  xx = scaled[0] * mat[0]; xy = scaled[0] * mat[1]; xz = scaled[0] * mat[2]; xw = scaled[0] * mat[3];
  yy = scaled[1] * mat[1]; yz = scaled[1] * mat[2]; yw = scaled[1] * mat[3];
  zz = scaled[2] * mat[2]; zw = scaled[2] * mat[3];
  axis[0] = 1.0f - (yy + zz); axis[1] = xy + zw; axis[2] = xz - yw;
  axis[3] = xy - zw; axis[4] = 1.0f - (xx + zz); axis[5] = yz + xw;
  axis[6] = xz + yw; axis[7] = yz - xw; axis[8] = 1.0f - (xx + yy);
  return axis;
}

float *MatrixTransformVectorQuatTrans(float *in, float *mat, float *out)
{
  float axis[9];
  ConvertQuatToMat(mat, axis);
  out[0] = in[0] * axis[0] + in[1] * axis[3] + in[2] * axis[6] + mat[4];
  out[1] = in[0] * axis[1] + in[1] * axis[4] + in[2] * axis[7] + mat[5];
  out[2] = in[0] * axis[2] + in[1] * axis[5] + in[2] * axis[8] + mat[6];
  return mat;
}

/*
================
AxisToQuat

Build a normalized quaternion from a 3x3 axis matrix.  The four candidate
forms avoid loss of precision for rotations whose trace is close to -1.
================
*/
void AxisToQuat(const float *mat, float *out)
{
  float test[4][4];
  float testSizeSq;
  int best;

  test[0][0] = mat[5] - mat[7];
  test[0][1] = mat[6] - mat[2];
  test[0][2] = mat[1] - mat[3];
  test[0][3] = mat[0] + mat[4] + mat[8] + 1.0f;
  testSizeSq = Vec4LengthSq(test[0]);
  if (testSizeSq < 1.0f)
  {
    test[1][0] = mat[6] + mat[2];
    test[1][1] = mat[7] + mat[5];
    test[1][2] = mat[8] - mat[4] - mat[0] + 1.0f;
    test[1][3] = test[0][2];
    testSizeSq = Vec4LengthSq(test[1]);
    if (testSizeSq < 1.0f)
    {
      test[2][0] = mat[0] - mat[4] - mat[8] + 1.0f;
      test[2][1] = mat[3] + mat[1];
      test[2][2] = test[1][0];
      test[2][3] = test[0][0];
      testSizeSq = Vec4LengthSq(test[2]);
      if (testSizeSq < 1.0f)
      {
        test[3][0] = test[2][1];
        test[3][1] = mat[4] - mat[0] - mat[8] + 1.0f;
        test[3][2] = test[1][1];
        test[3][3] = test[0][1];
        testSizeSq = Vec4LengthSq(test[3]);
        Assert(testSizeSq >= 1.0f, s_assertDisable_AxisToQuat);
        best = 3;
      }
      else
        best = 2;
    }
    else
      best = 1;
  }
  else
    best = 0;

  Assert(testSizeSq != 0.0f, s_assertDisable_AxisToQuat);
  testSizeSq = 1.0f / sqrtf(testSizeSq);
  out[0] = test[best][0] * testSizeSq;
  out[1] = test[best][1] * testSizeSq;
  out[2] = test[best][2] * testSizeSq;
  out[3] = test[best][3] * testSizeSq;
}

/*
================
MatrixInverse44

4x4 matrix inverse via cofactor expansion.
================
*/
float *MatrixInverse44(float *in, float *out)
{
  int i;
  float m00, m01, m02, m03;
  float m10, m11, m12, m13;
  float m20, m21, m22, m23;
  float m30, m31, m32, m33;
  float s0, s1, s2, s3, s4, s5;  /* 2x2 sub-determinants phase 1 */
  float c0, c1, c2, c3, c4, c5;  /* 2x2 sub-determinants phase 2 */
  float t0, t1, t2, t3, t4, t5;  /* shared temporaries */
  float det;
  volatile float invDet;  /* force 32-bit precision per multiply */

  Assert(in != out, s_assertDisable_MatrixInverse44);
  /* cache all 16 matrix elements */
  m00 = in[0];  m03 = in[3];  m20 = in[6];  m01 = in[1];
  m02 = in[2];  m21 = in[9];  m10 = in[4];  m23 = in[7];
  m11 = in[5];  m30 = in[12]; m33 = in[15]; m22 = in[10];
  m12 = in[8];  m13 = in[13]; m31 = in[11]; m32 = in[14];
  /* phase 1: 2x2 sub-determinants from columns 2-3 */
  s0 = m33 * m22;
  s1 = m31 * m32;
  s2 = m20 * m33;
  s3 = m23 * m32;
  s4 = m20 * m31;
  s5 = m23 * m22;
  t0 = m02 * m33;
  t1 = m03 * m32;
  t2 = m02 * m31;
  t3 = m03 * m22;
  t4 = m02 * m23;
  t5 = m03 * m20;
  /* cofactor matrix rows 0-1 */
#ifdef _WIN64
  #define COF(a,b,c,d,e,f,g,h,i,j,k,l) (float)((double)(a)*(b)+(double)(c)*(d)+(double)(e)*(f)-((double)(g)*(h)+(double)(i)*(j)+(double)(k)*(l)))
#else
  #define COF(a,b,c,d,e,f,g,h,i,j,k,l) ((a)*(b)+(c)*(d)+(e)*(f)-((g)*(h)+(i)*(j)+(k)*(l)))
#endif
  out[0]  = COF(m13,s4, m21,s3, m11,s0, m13,s5, m21,s2, m11,s1);
  out[1]  = COF(m13,t3, m21,t0, m01,s1, m13,t2, m21,t1, m01,s0);
  out[2]  = COF(m13,t4, m11,t1, m01,s2, m13,t5, m11,t0, m01,s3);
  out[3]  = COF(m21,t5, m11,t2, m01,s5, m21,t4, m11,t3, m01,s4);
  out[4]  = COF(m30,s5, m12,s2, m10,s1, m30,s4, m12,s3, m10,s0);
  out[5]  = COF(m30,t2, m00,s0, m12,t1, m30,t3, m00,s1, m12,t0);
  out[6]  = COF(m30,t5, m00,s3, m10,t0, m30,t4, m00,s2, m10,t1);
  out[7]  = COF(m00,s4, m12,t4, m10,t3, m00,s5, m12,t5, m10,t2);
  /* phase 2: 2x2 sub-determinants from rows 0-1 */
  c0 = m12 * m13;
  c1 = m30 * m21;
  c2 = m10 * m13;
  c3 = m30 * m11;
  c4 = m10 * m21;
  c5 = m12 * m11;
  t0 = m00 * m13;
  t1 = m30 * m01;
  t2 = m00 * m21;
  t3 = m12 * m01;
  t4 = m00 * m11;
  t5 = m10 * m01;
  /* cofactor matrix rows 2-3 */
  out[8]  = COF(c3,m31, m23,c0, c4,m33, m23,c1, c2,m31, c5,m33);
  out[9]  = COF(m03,c1, t0,m31, t3,m33, t1,m31, m03,c0, t2,m33);
  out[10] = COF(t1,m23, m03,c2, t4,m33, m03,c3, t0,m23, t5,m33);
  out[11] = COF(m03,c5, t2,m23, t5,m31, m03,c4, t3,m23, t4,m31);
  out[12] = COF(c5,m32, m20,c1, c2,m22, c4,m32, m20,c0, c3,m22);
  out[13] = COF(t2,m32, m02,c0, t1,m22, t3,m32, m02,c1, t0,m22);
  out[14] = COF(m02,c3, t0,m20, t5,m32, t1,m20, m02,c2, t4,m32);
  out[15] = COF(m02,c4, t3,m20, t4,m22, m02,c5, t2,m20, t5,m22);
  #undef COF
  /* determinant from row 0 dot cofactor row 0 */
#ifdef _WIN64
  det = (float)((double)m10 * out[1] + (double)m00 * out[0] + (double)m12 * out[2] + (double)m30 * out[3]);
#else
  det = m10 * out[1] + m00 * out[0] + m12 * out[2] + m30 * out[3];
#endif
  Assert( det != 0.0, s_assertDisable_MatrixInverse44 );

  /* scale all 16 elements by 1/det */
  invDet = 1.0 / det;
  for ( i = 0; i < 16; i++ )
    out[i] = invDet * out[i];

  return out;
}

/*
================
MatrixTransformVector

Transform vec3 by 3x3 matrix: out = mat * vec
================
*/
float *MatrixTransformVector(float *vec, float *mat, float *out)
{
  Assert(vec != out, s_assertDisable_MatrixTransformVector);
#ifdef _WIN64
  out[0] = (float)((double)vec[0]*mat[0] + (double)vec[1]*mat[3] + (double)vec[2]*mat[6]);
  out[1] = (float)((double)vec[0]*mat[1] + (double)vec[1]*mat[4] + (double)vec[2]*mat[7]);
  out[2] = (float)((double)vec[0]*mat[2] + (double)vec[1]*mat[5] + (double)vec[2]*mat[8]);
#else
  out[0] = vec[0] * mat[0] + vec[1] * mat[3] + vec[2] * mat[6];
  out[1] = vec[0] * mat[1] + vec[1] * mat[4] + vec[2] * mat[7];
  out[2] = vec[0] * mat[2] + vec[1] * mat[5] + vec[2] * mat[8];
#endif
  return out;
}

/*
================
QuatMultiply

Hamilton product: out = q1 * q2
================
*/
float *QuatMultiply(float *q1, float *q2, float *out)
{
  Assert(q1 != out, s_assertDisable_MatrixMultiply44);
  Assert(q2 != out, s_assertDisable_MatrixMultiply44);
#ifdef _WIN64
  out[0] = (float)((double)q1[3]*q2[0] + (double)q1[0]*q2[3] + (double)q1[2]*q2[1] - (double)q1[1]*q2[2]);
  out[1] = (float)((double)q1[3]*q2[1] + (double)q1[1]*q2[3] + (double)q1[0]*q2[2] - (double)q1[2]*q2[0]);
  out[2] = (float)((double)q1[3]*q2[2] + (double)q1[2]*q2[3] + (double)q1[1]*q2[0] - (double)q1[0]*q2[1]);
  out[3] = (float)((double)q1[3]*q2[3] - (double)q1[0]*q2[0] - (double)q1[1]*q2[1] - (double)q1[2]*q2[2]);
#else
  out[0] = q1[3] * q2[0] + q1[0] * q2[3] + q1[2] * q2[1] - q1[1] * q2[2];
  out[1] = q1[3] * q2[1] + q1[1] * q2[3] + q1[0] * q2[2] - q1[2] * q2[0];
  out[2] = q1[3] * q2[2] + q1[2] * q2[3] + q1[1] * q2[0] - q1[0] * q2[1];
  out[3] = q1[3] * q2[3] - q1[0] * q2[0] - q1[1] * q2[1] - q1[2] * q2[2];
#endif
  return q2;
}

/*
================
QuatRunningAveragePortion

Compute xyz portion of quaternion magnitude.
================
*/
double QuatRunningAveragePortion(float *quat)
{
  float xx, yy, zz;
  float xxN, yyN, zzN;
  float lengthSq;
  float invLengthSq;

  xx = quat[0] * quat[0];
  yy = quat[1] * quat[1];
  zz = quat[2] * quat[2];
#ifdef _WIN64
  lengthSq = (float)((double)quat[3] * quat[3] + (double)zz + (double)yy + (double)xx);
#else
  lengthSq = quat[3] * quat[3] + zz + yy + xx;
#endif
  if ( lengthSq == 0.0 )
    return 0.0;
  invLengthSq = 1.0 / lengthSq;
  xxN = invLengthSq * xx;
  yyN = invLengthSq * yy;
  zzN = invLengthSq * zz;
  return zzN + yyN + xxN;
}

/*
================
SetPerspectiveProjection

Build 4x4 perspective projection matrix.
================
*/
int SetPerspectiveProjection(float *mat, float fovX, float fovY, float zNear)
{
  float cotX;
  float cotY;

  Assert( mat != 0, s_assertDisable_SetPerspectiveProjection );
  Assert( zNear > 0.0, s_assertDisable_SetPerspectiveProjection );
  memset( mat, 0, sizeof(float) * 16 );
  cotX = tan( (90.0 - fovX * 0.5) * DEG2RAD );
  mat[0]  = cotX * NEAR_CLIP_FACTOR;
  cotY = tan( (90.0 - fovY * 0.5) * DEG2RAD );
  mat[10] = NEAR_CLIP_FACTOR;
  mat[11] = 1.0f;
  mat[5]  = cotY * NEAR_CLIP_FACTOR;
  mat[14] = zNear * -NEAR_CLIP_FACTOR;
  return 0;
}

/*
================
AngleSubtract

Compute shortest angular difference, normalized to [-180, 180].
================
*/
float AngleSubtract(float angle1, float angle2)
{
  return AngleDelta(angle1, angle2);
}

float AngleDelta(float angle1, float angle2)
{
  float scaled = (angle1 - angle2) * 0.002777777845039964f;
  return (scaled - floorf(scaled + 0.5f)) * 360.0f;
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
AddBoundsToBounds2D

Expand 2D destination bounds to enclose source bounds.
================
*/
void AddBoundsToBounds2D( float *srcMins, float *srcMaxs, float *dstMins, float *dstMaxs )
{
  if ( srcMins[0] < dstMins[0] )
    dstMins[0] = srcMins[0];
  if ( srcMaxs[0] > dstMaxs[0] )
    dstMaxs[0] = srcMaxs[0];
  if ( srcMins[1] < dstMins[1] )
    dstMins[1] = srcMins[1];
  if ( srcMaxs[1] > dstMaxs[1] )
    dstMaxs[1] = srcMaxs[1];
}

/*
================
MatrixTransformBounds

Transform an axis-aligned bounding box by a 3x3 matrix.
For each of the 3 output axes, selects min/max corners based
on half-size sign to compute transformed mins and maxs.
================
*/
float *MatrixTransformBounds(float *mat, float *origin, float *halfSize, float *outBounds)
{
  float *outMins = outBounds;
  float *outMaxs = outBounds + 3;
  int i, j, sign;

  /* for each output axis, start from origin then add/subtract
     each half-size component scaled by the matrix row */
  for ( i = 0; i < 3; i++ )
  {
    outMins[i] = origin[i];
    outMaxs[i] = origin[i];
    for ( j = 0; j < 3; j++ )
    {
      sign = halfSize[j * 3 + i] >= 0.0f ? 0 : 3;
      outMins[i] = FMA1(outMins[i], mat[sign + j], halfSize[j * 3 + i]);
      outMaxs[i] = FMA1(outMaxs[i], mat[3 - sign + j], halfSize[j * 3 + i]);
    }
  }
  return outBounds;
}

/*
================
AxisCopy

Copy a 3x3 axis matrix (9 floats).
================
*/
void AxisCopy(float *src, float *dst)
{
  memcpy(dst, src, 9 * sizeof(float));
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

    for (axis = 0; axis < 3; axis++) {
      float *plane = planes[axis];
      float dist = DotProduct210(snapped, plane) - plane[3];
      float absDist = fabs(dist);
      if (absDist > maxError)
        maxError = absDist;
      if (absDist > maxAllowed)
        maxAllowed = absDist;
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
ProjectPointOnPlane

Projects a point onto the plane through the origin defined by normal.

================
*/
float *ProjectPointOnPlane( float *point, float *normal, float *out )
{
	float	dot;

	Assert(Vec3IsNormalized(normal), s_assertDisable_ProjectPointOnPlane);

	dot = -DotProduct(point, normal);
	VectorMA(point, dot, normal, out);

	return out;
}

/*
================
SetPlaneSignbits

Computes sign bits from the plane normal components.
Bit 0 = normal[0] < 0, bit 1 = normal[1] < 0, bit 2 = normal[2] < 0.
Stores result at plane byte offset 17 (signbits field).
================
*/
void SetPlaneSignbits( float *plane )
{
	char	signbits;

	signbits = 0;
	if ( plane[0] < 0.0 )
		signbits |= 1;
	if ( plane[1] < 0.0 )
		signbits |= 2;
	if ( plane[2] < 0.0 )
		signbits |= 4;

	((unsigned char *)plane)[17] = signbits;
}

/*
================
BoxOnPlaneSide

Returns 1, 2, or 1+2 (=3).
Classifies an AABB against a plane using signbits to select
which corner is nearest/farthest from the plane.
================
*/
int BoxOnPlaneSide( float *emins, float *emaxs, float *plane )
{
	float	*normal;
	float	dist;
	float	dist1, dist2;
	int		sides;
	int		signbits;

	normal = plane;
	dist = plane[3];

	/* general case */
	signbits = ((unsigned char *)plane)[17];

	switch ( signbits )
	{
#ifdef _WIN64
	#define BOPD(a,b,c) (float)((double)normal[0]*(a) + (double)normal[1]*(b) + (double)normal[2]*(c))
#else
	#define BOPD(a,b,c) (normal[0]*(a) + normal[1]*(b) + normal[2]*(c))
#endif
	case 0: dist1 = BOPD(emaxs[0],emaxs[1],emaxs[2]); dist2 = BOPD(emins[0],emins[1],emins[2]); break;
	case 1: dist1 = BOPD(emins[0],emaxs[1],emaxs[2]); dist2 = BOPD(emaxs[0],emins[1],emins[2]); break;
	case 2: dist1 = BOPD(emaxs[0],emins[1],emaxs[2]); dist2 = BOPD(emins[0],emaxs[1],emins[2]); break;
	case 3: dist1 = BOPD(emins[0],emins[1],emaxs[2]); dist2 = BOPD(emaxs[0],emaxs[1],emins[2]); break;
	case 4: dist1 = BOPD(emaxs[0],emaxs[1],emins[2]); dist2 = BOPD(emins[0],emins[1],emaxs[2]); break;
	case 5: dist1 = BOPD(emins[0],emaxs[1],emins[2]); dist2 = BOPD(emaxs[0],emins[1],emaxs[2]); break;
	case 6: dist1 = BOPD(emaxs[0],emins[1],emins[2]); dist2 = BOPD(emins[0],emaxs[1],emaxs[2]); break;
	case 7: dist1 = BOPD(emins[0],emins[1],emins[2]); dist2 = BOPD(emaxs[0],emaxs[1],emaxs[2]); break;
	#undef BOPD
	default:
		dist1 = dist2 = 0;
		Assert(g_assertsDisabled, s_assertDisable_BoxOnPlaneSide);
		break;
	}

	sides = 0;
	if ( dist1 >= dist )
		sides = 1;
	if ( dist2 < dist )
		sides |= 2;

	return sides;
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

/*
================
RotatePoint

Rotates a 3D point by Euler angles (pitch, yaw, roll).
Each axis applies a 2D rotation to the other two axes.
================
*/
float *RotatePoint( float *point, float *angles, float *out )
{
	/* for axis i, rotLookup[2*i] and rotLookup[2*i+1] are the two axes that rotate */
	static const int rotLookup[6] = { 1, 2, 2, 0, 0, 1 };
	vec3_t result, input;
	long double radians, cosVal, sinVal;
	int i, ax1, ax2;

	VectorCopy(point, input);
	VectorCopy(point, result);

	for ( i = 0; i < 3; i++ )
	{
		if ( angles[i] != 0.0 )
		{
			radians = angles[i] * Q_PI / 180.0;
			cosVal = cos( radians );
			sinVal = sin( radians );
			ax1 = rotLookup[2 * i];
			ax2 = rotLookup[2 * i + 1];
			result[ax1] = (float)(input[ax1] * cosVal - input[ax2] * sinVal);
			result[ax2] = (float)(input[ax2] * cosVal + input[ax1] * sinVal);
		}
		VectorCopy(result, input);
	}

	VectorCopy(result, out);
	return out;
}

float ColorNormalize(const float *in, float *out)
{
  float max = in[0];
  if (max < in[1])
    max = in[1];
  if (max < in[2])
    max = in[2];
  if (max == 0.0f)
  {
    out[0] = 1.0f;
    out[1] = 1.0f;
    out[2] = 1.0f;
    return 0.0f;
  }
  VectorScale(in, 1.0f / max, out);
  return max;
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
SnapToGrid

Snaps a float (stored as int bits) to the nearest grid point
defined by granularity, if within epsilon tolerance.
First snaps the IEEE 754 mantissa to 12-bit boundaries,
then rounds to the nearest grid multiple.
================
*/
double SnapToGrid( int value, float granularity, float epsilon )
{
	unsigned int	fraction;
	float	scaled, rounded, snapped, diff;

	Assert(granularity > 0.0, s_assertDisable_SnapToGrid);
	Assert(epsilon > 0.0, s_assertDisable_SnapToGrid);
	Assert(epsilon < 0.5 / granularity, s_assertDisable_SnapToGrid);

	/* snap to 12-bit boundary if within 4 ULPs */
	union { float f; int i; } snap;
	snap.i = value;
	fraction = snap.i & SNAP_FLOAT_MASK;
	if ( fraction <= 4 )
		snap.i = snap.i - fraction;
	else if ( (int)(SNAP_FLOAT_SIZE - fraction) <= 4 )
		snap.i = SNAP_FLOAT_SIZE - fraction + snap.i;

	/* round to nearest grid multiple */
	scaled = snap.f * granularity;
	rounded = (float)xs_RoundToInt( scaled + FISTP_BIAS );
	snapped = rounded / granularity;
	diff = fabs( snapped - snap.f );
	if ( diff > (double)epsilon )
		return snap.f;
	else
		return snapped;
}

/*
================
DiffTrack

Tracks a value toward a target at a fixed rate.
Step magnitude is rate * deltaTime regardless of distance;
only direction depends on sign of (target - current).
================
*/
double DiffTrack( float target, float current, float rate, float deltaTime )
{
	float	step, diff, absDiff, absStep;

	step = rate * deltaTime;
	if ( target - current <= 0.0f )
		step = -step;

	diff = target - current;
	absDiff = fabs( diff );
	if ( absDiff <= PLANESIDE_EPSILON )
		return target;

	absStep = fabs( step );
	if ( absDiff < (double)absStep )
		return target;

	return step + current;
}

/*
================
DiffTrackSimple

Tracks a value toward a target with proportional step.
Step magnitude is proportional to distance: diff * rate * deltaTime.
================
*/
double DiffTrackSimple( float target, float current, float rate, float deltaTime )
{
	float	diff, step, absDiff, absStep;

	diff = target - current;
	step = diff * rate * deltaTime;
	absDiff = fabs( diff );
	if ( absDiff <= PLANESIDE_EPSILON )
		return target;

	absStep = fabs( step );
	if ( absDiff < (double)absStep )
		return target;

	return step + current;
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
RotatePointAroundVector

Rotates 'point' around 'dir' by 'degrees' and stores result in 'dst'.
Builds rotation via basis change: m * zrot * m^T
================
*/
float *RotatePointAroundVector( float *dst, float *dir, float *point, float degrees )
{
	float	m[9], im[10], zrot[9], tmpmat[9], rot[9];
	float	vup[3], vr[3], vf[3];
	float	rad;

	Assert( dir[0] || dir[1] || dir[2], s_assertDisable_RotatePointAroundVector );

	VectorCopy( dir, vf );
	PerpendicularVector( dir, vr );
	CrossProduct( vr, vf, vup );

	/* build rotation basis matrix (columns = vr, vup, vf) */
	m[0] = vr[0];    m[1] = vup[0];   m[2] = vf[0];
	m[3] = vr[1];    m[4] = vup[1];   m[5] = vf[1];
	m[6] = vr[2];    m[7] = vup[2];   m[8] = vf[2];

	/* build inverse (transpose of m) */
	memcpy( im, m, sizeof(float) * 9 );
	im[1] = vr[1];    im[2] = vr[2];
	im[3] = vup[0];   im[5] = vup[2];
	im[6] = vf[0];    im[7] = vf[1];

	/* z-rotation matrix */
	zrot[0] = 1.0;   zrot[1] = 0.0;   zrot[2] = 0.0;
	zrot[3] = 0.0;   zrot[4] = 1.0;   zrot[5] = 0.0;
	zrot[6] = 0.0;   zrot[7] = 0.0;   zrot[8] = 1.0;

	rad = degrees * DEG2RAD;
	Assert( !isnan(rad), s_assertDisable_RotatePointAroundVector );

	zrot[0] = cos( rad );
	zrot[1] = sin( rad );
	Assert( !isnan(zrot[1]), s_assertDisable_RotatePointAroundVector );
	Assert( !isnan(zrot[0]), s_assertDisable_RotatePointAroundVector );
	zrot[3] = -zrot[1];
	zrot[4] = zrot[0];

	/* rot = m * zrot * m^T */
	MatrixMultiply33( m, zrot, tmpmat );
	MatrixMultiply33( tmpmat, im, rot );

	/* apply rotation */
#ifdef _WIN64
	dst[0] = (float)((double)rot[0]*point[0] + (double)rot[1]*point[1] + (double)rot[2]*point[2]);
	dst[1] = (float)((double)rot[3]*point[0] + (double)rot[4]*point[1] + (double)rot[5]*point[2]);
	dst[2] = (float)((double)rot[6]*point[0] + (double)rot[7]*point[1] + (double)rot[8]*point[2]);
#else
	dst[0] = rot[0] * point[0] + rot[1] * point[1] + rot[2] * point[2];
	dst[1] = rot[3] * point[0] + rot[4] * point[1] + rot[5] * point[2];
	dst[2] = rot[6] * point[0] + rot[7] * point[1] + rot[8] * point[2];
#endif
	return dst;
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

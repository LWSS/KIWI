/*
 * com_math.c — Vector, matrix, and bounds math.
 * Pure float precision throughout (no double promotion).
 */

#include "cod2rad64.h"

const float vec3_origin[3] = {0.0f, 0.0f, 0.0f};

static char s_assertDisable_MatrixTransformVector;
static char s_assertDisable_Vec3MajorAxis;
static char s_assertDisable_MatrixInverse;
static char s_assertDisable_MatrixInverse_det;
static char s_assertDisable_UniformPointsOnHemisphere;
static char s_assertDisable_UniformPointsOnHemisphere_stride;
static char s_assertDisable_UniformPointsOnSphere;
static char s_assertDisable_UniformPointsOnSphere_stride;

/* CompareFunction (float qsort comparator) is defined in aabbtree.c */

/*
================
Vec2DistanceSq

Squared 2D distance between two points.
================
*/
float Vec2DistanceSq(const float *a, const float *b)
{
    float dx = *b - *a;
    float dy = b[1] - a[1];
    return dy * dy + dx * dx;
}

/*
================
Vec3Cross

Cross product: out = v1 x v2.
================
*/
void Vec3Cross(const float *v1, const float *v2, float *out)
{
    out[0] = v1[1] * v2[2] - v1[2] * v2[1];
    out[1] = v1[2] * v2[0] - v1[0] * v2[2];
    out[2] = v1[0] * v2[1] - v1[1] * v2[0];
}

/*
================
Vec3Normalize

Normalizes vec in place, returns original length.
================
*/
float Vec3Normalize(float *vec)
{
    float v0 = vec[0];
    float v1 = vec[1];
    float v2 = vec[2];
    float length = sqrtf(v0 * v0 + v1 * v1 + v2 * v2);

    if (length != 0.0f)
    {
        float ilength = 1.0f / length;
        vec[0] = v0 * ilength;
        vec[1] = v1 * ilength;
        vec[2] = v2 * ilength;
    }

    return length;
}

/*
================
Vec2Normalize

Normalizes 2D vector in place.
================
*/
void Vec2Normalize(float *vec)
{
    float v0 = vec[0];
    float v1 = vec[1];
    float length = sqrtf(v0 * v0 + v1 * v1);

    if (length != 0.0f)
    {
        float ilength = 1.0f / length;
        vec[0] = v0 * ilength;
        vec[1] = v1 * ilength;
    }
}

/*
================
AngleVectors

Derives forward/right/up direction vectors from Euler angles.
================
*/
extern float x87_sinf(float x);
extern float x87_cosf(float x);

void AngleVectors(float *angles, float *forward, float *right, float *up)
{
    float sy, cy, sp, cp, sr, cr;

    sy = x87_sinf(angles[1] * DEG2RAD);
    cy = x87_cosf(angles[1] * DEG2RAD);
    sp = x87_sinf(angles[0] * DEG2RAD);
    cp = x87_cosf(angles[0] * DEG2RAD);

    if (forward)
    {
        forward[0] = cp * cy;
        forward[1] = cp * sy;
        forward[2] = -sp;
    }

    if (right || up)
    {
        sr = x87_sinf(angles[2] * DEG2RAD);
        cr = x87_cosf(angles[2] * DEG2RAD);

        if (right)
        {
            float srsp = sr * sp;
            right[0] = cr * sy - srsp * cy;
            right[1] = -(cr * cy + srsp * sy);
            right[2] = -(sr * cp);
        }
        if (up)
        {
            float crsp = cr * sp;
            up[0] = crsp * cy + sr * sy;
            up[1] = crsp * sy - sr * cy;
            up[2] = cr * cp;
        }
    }
}

/*
================
AnglesToAxis

Converts Euler angles to 3x3 axis matrix.
================
*/
void AnglesToAxis(float *angles, float *axis)
{
    float right[3];

    AngleVectors(angles, axis, right, axis + 6);
    axis[3] = 0.0f - right[0];
    axis[4] = 0.0f - right[1];
    axis[5] = 0.0f - right[2];
}

/*
================
QuatMultiply

Hamilton product: out = q1 * q2.
================
*/
void QuatMultiply(float *q1, float *q2, float *out)
{
    out[0] = q1[3] * q2[0] + q1[0] * q2[3] + q1[2] * q2[1] - q1[1] * q2[2];
    out[1] = q1[3] * q2[1] + q1[1] * q2[3] + q1[0] * q2[2] - q1[2] * q2[0];
    out[2] = q1[3] * q2[2] + q1[2] * q2[3] + q1[1] * q2[0] - q1[0] * q2[1];
    out[3] = q1[3] * q2[3] - q1[0] * q2[0] - q1[1] * q2[1] - q1[2] * q2[2];
}

/*
================
ClearBounds

Initialize 3D bounding box to inverted state.
================
*/
void ClearBounds(float *mins, float *maxs)
{
    mins[0] = MAX_WORLD_COORD;
    mins[1] = MAX_WORLD_COORD;
    mins[2] = MAX_WORLD_COORD;
    maxs[0] = MIN_WORLD_COORD;
    maxs[1] = MIN_WORLD_COORD;
    maxs[2] = MIN_WORLD_COORD;
}

/*
================
ClearBounds2D

Initialize 2D bounding box to inverted state.
================
*/
void ClearBounds2D(float *mins, float *maxs)
{
    mins[0] = MAX_WORLD_COORD;
    mins[1] = MAX_WORLD_COORD;
    maxs[0] = MIN_WORLD_COORD;
    maxs[1] = MIN_WORLD_COORD;
}

/*
================
AddPointToBounds

Expand 3D bounding box to include point.
================
*/
void AddPointToBounds(float *point, float *mins, float *maxs)
{
    if (point[0] < mins[0]) mins[0] = point[0];
    if (point[0] > maxs[0]) maxs[0] = point[0];
    if (point[1] < mins[1]) mins[1] = point[1];
    if (point[1] > maxs[1]) maxs[1] = point[1];
    if (point[2] < mins[2]) mins[2] = point[2];
    if (point[2] > maxs[2]) maxs[2] = point[2];
}

/*
================
AddPointToBounds2D

Expand 2D bounding box to include point.
================
*/
void AddPointToBounds2D(float *point, float *mins, float *maxs)
{
    if (point[0] < mins[0]) mins[0] = point[0];
    if (point[0] > maxs[0]) maxs[0] = point[0];
    if (point[1] < mins[1]) mins[1] = point[1];
    if (point[1] > maxs[1]) maxs[1] = point[1];
}

/*
================
ExpandBounds

Expand bounds (mins/maxs) to include another bounds pair.
================
*/
void ExpandBounds(float *point1, float *point2, float *mins, float *maxs)
{
    if (point1[0] < mins[0]) mins[0] = point1[0];
    if (point2[0] > maxs[0]) maxs[0] = point2[0];
    if (point1[1] < mins[1]) mins[1] = point1[1];
    if (point2[1] > maxs[1]) maxs[1] = point2[1];
    if (point1[2] < mins[2]) mins[2] = point1[2];
    if (point2[2] > maxs[2]) maxs[2] = point2[2];
}

/*
================
GetRotatedBounds

Compute axis-aligned bounds from oriented bounds.
Loops 3 iterations, each axis checks sign of 3 matrix row elements.
================
*/
void GetRotatedBounds(float *box, float *origin, float *matrix, float *outBounds)
{
    float *outMins = outBounds;
    float *outMaxs = outBounds + 3;
    int i, j, sel;

    for (i = 0; i < 3; i++)
    {
        outMins[i] = origin[i];
        outMaxs[i] = origin[i];
        for (j = 0; j < 3; j++)
        {
            float m = matrix[j * 3 + i];
            sel = (*(int *)&m < 0) ? 3 : 0;
            outMins[i] += box[sel + j]     * m;
            outMaxs[i] += box[3 - sel + j] * m;
        }
    }
}

/*
================
PointOnSphereFromUniformDeviates

Convert two uniform random [0,1] deviates to a point on the unit sphere.
================
*/
void PointOnSphereFromUniformDeviates(float u1, float u2, float *out)
{
    float z = u1 * 2.0f - 1.0f;
    float r = sqrtf(1.0f - z * z);
    float phi = u2 * 6.2831855f; /* 2*pi as float */

    { extern float x87_cosf(float); out[0] = x87_cosf(phi) * r; }
    { extern float x87_sinf(float); out[1] = x87_sinf(phi) * r; }
    out[2] = z;
}

/*
================
PlaneFromPoints

Compute plane normal + distance from 3 points. Returns 0 if degenerate.
================
*/
int PlaneFromPoints(float *plane, float *point0, float *point1, float *point2)
{
    float d1[3], d2[3];
    float n0, n1, n2, length;
    float ilength;

    d1[0] = point2[0] - point0[0];
    d1[1] = point2[1] - point0[1];
    d1[2] = point2[2] - point0[2];
    d2[0] = point1[0] - point0[0];
    d2[1] = point1[1] - point0[1];
    d2[2] = point1[2] - point0[2];

    n0 = d1[1] * d2[2] - d1[2] * d2[1];
    n1 = d1[2] * d2[0] - d1[0] * d2[2];
    n2 = d1[0] * d2[1] - d1[1] * d2[0];

    plane[0] = n0;
    plane[1] = n1;
    plane[2] = n2;

    length = sqrtf(n0 * n0 + n1 * n1 + n2 * n2);
    if (length == 0.0f)
        return 0;

    ilength = 1.0f / length;
    plane[0] = n0 * ilength;
    plane[1] = n1 * ilength;
    plane[2] = n2 * ilength;
    plane[3] = plane[0] * point0[0] + plane[1] * point0[1] + plane[2] * point0[2];

    return 1;
}

/*
================
VectorCompareEpsilon

Returns 1 if all 'count' float pairs are within epsilon.
================
*/
int VectorCompareEpsilon(const float *v1, const float *v2, float epsilon, int count)
{
    float epsSq = epsilon * epsilon;
    int i;

    for (i = 0; i < count; i++)
    {
        float diff = v1[i] - v2[i];
        if (diff * diff > epsSq)
            return 0;
    }
    return 1;
}

/*
================
MatrixTransformVector

Transform vec3 by 3x3 matrix: out = mat * vec.
================
*/
void MatrixTransformVector(float *in1, float *in2, float *out)
{
    Assert(in1 != out, s_assertDisable_MatrixTransformVector);

    out[0] = in1[0] * in2[0] + in1[1] * in2[3] + in1[2] * in2[6];
    out[1] = in1[0] * in2[1] + in1[1] * in2[4] + in1[2] * in2[7];
    out[2] = in1[0] * in2[2] + in1[1] * in2[5] + in1[2] * in2[8];
}

/*
================
Vec3Distance

3D Euclidean distance between two points.
================
*/
float Vec3Distance(const float *a, const float *b)
{
    float dx = b[0] - a[0];
    float dy = b[1] - a[1];
    float dz = b[2] - a[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/*
================
Vec3MajorAxis

Returns index of the component with largest absolute value.
================
*/
int Vec3MajorAxis(const float *dir)
{
    float sq[3];
    int best;

    Assert(dir, s_assertDisable_Vec3MajorAxis);

    sq[0] = dir[0] * dir[0];
    sq[1] = dir[1] * dir[1];
    sq[2] = dir[2] * dir[2];

    best = 1;
    if (sq[1] <= sq[0])
        best = 0;
    if (sq[2] > sq[best])
        return 2;
    return best;
}

/*
================
MatrixInverse

Inverse of a 3x3 matrix using adjugate/determinant method.
================
*/
void MatrixInverse(float *in, float *out)
{
    float det, invDet;

    Assert(in != out, s_assertDisable_MatrixInverse);

    det = (in[4] * in[8] - in[5] * in[7]) * in[0]
        - (in[1] * in[8] - in[2] * in[7]) * in[3]
        + (in[1] * in[5] - in[2] * in[4]) * in[6];

    Assert(det != 0.0f, s_assertDisable_MatrixInverse_det);

    invDet = 1.0f / det;
    out[0] =  (in[4] * in[8] - in[7] * in[5]) * invDet;
    out[1] = -(in[1] * in[8] - in[7] * in[2]) * invDet;
    out[2] =  (in[1] * in[5] - in[2] * in[4]) * invDet;
    out[3] = -(in[3] * in[8] - in[6] * in[5]) * invDet;
    out[4] =  (in[0] * in[8] - in[2] * in[6]) * invDet;
    out[5] = -(in[0] * in[5] - in[2] * in[3]) * invDet;
    out[6] =  (in[3] * in[7] - in[6] * in[4]) * invDet;
    out[7] = -(in[0] * in[7] - in[1] * in[6]) * invDet;
    out[8] =  (in[0] * in[4] - in[1] * in[3]) * invDet;
}

/*
================
UniformPointsOnHemisphere

Distributes points on the upper hemisphere using golden-angle spiral.
================
*/
void UniformPointsOnHemisphere(unsigned int numPoints, float *points, int stride)
{
    float sinAngle = 0.0f;
    float cosAngle = 1.0f;
    float step, z, r, oldCos;

    Assert(points, s_assertDisable_UniformPointsOnHemisphere);
    Assert(stride >= 12, s_assertDisable_UniformPointsOnHemisphere_stride);

    step = 1.0f / (float)(int)numPoints;
    z = step * 0.5f;

    while (numPoints > 0)
    {
        r = sqrtf(1.0f - z * z);
        points[0] = r * cosAngle;
        points[1] = r * sinAngle;
        points[2] = z;
        points = (float *)((char *)points + stride);
        numPoints--;
        z += step;
        oldCos = cosAngle;
        cosAngle = cosAngle * -0.73736888f + sinAngle * 0.67549032f;
        sinAngle = sinAngle * -0.73736888f - oldCos * 0.67549032f;
    }
}

/*
================
UniformPointsOnSphere

Distributes points on the full sphere using golden-angle spiral.
================
*/
void UniformPointsOnSphere(unsigned int numPoints, float *points, int stride)
{
    float sinAngle = 0.0f;
    float cosAngle = 1.0f;
    float step, t, z, r, oldCos;

    Assert(points, s_assertDisable_UniformPointsOnSphere);
    Assert(stride >= 12, s_assertDisable_UniformPointsOnSphere_stride);

    step = 1.0f / (float)(int)numPoints;
    t = step * 0.5f;

    while (numPoints > 0)
    {
        z = t * 2.0f - 1.0f;
        r = sqrtf(1.0f - z * z);
        points[0] = r * cosAngle;
        points[1] = r * sinAngle;
        points[2] = z;
        points = (float *)((char *)points + stride);
        numPoints--;
        t += step;
        oldCos = cosAngle;
        cosAngle = cosAngle * -0.73736888f + sinAngle * 0.67549032f;
        sinAngle = sinAngle * -0.73736888f - oldCos * 0.67549032f;
    }
}

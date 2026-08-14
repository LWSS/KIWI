/*
 * collvec.c — Collision vector math helpers.
 *
 * Source: collvec.cpp (from LST annotation ..\common\collvec.cpp)
 * Computes barycentric-like projection coefficients for collision tests.
 */

#include "cod2rad64.h"

static char s_assertDisable_collvec_scale;

/*
================
ComputeBaryCoordsInternal

Computes 4 output floats:
  cross  = vec2 × vec1            (cross product, vec2-then-vec1 order)
  midSum = (vec3 + vec5) * 0.5    (midpoint of two vectors)
  scale  = dot(vec4, cross) - dot(midSum, cross)
  out[0..2] = cross  / scale
  out[3]    = dot(midSum, cross) / scale

Asserts scale > 0. All math is done in double precision internally.
================
*/
void ComputeBaryCoordsInternal(const float *vec1, const float *vec2,
                               const float *vec3, const float *vec4,
                               const float *vec5, float *out)
{
    double cross[3];
    double midSum[3];
    double dotMidCross;
    double dotV4Cross;
    double scale;

    /* cross = vec2 × vec1 (note unusual order — vec2 first, vec1 second) */
    cross[0] = (double)(vec2[2] * vec1[1] - vec2[1] * vec1[2]);
    cross[1] = (double)(vec2[0] * vec1[2] - vec1[0] * vec2[2]);
    cross[2] = (double)(vec1[0] * vec2[1] - vec2[0] * vec1[1]);

    /* midSum = vec3 + vec5 (will be multiplied by 0.5 below) */
    midSum[0] = (double)(vec3[0] + vec5[0]);
    midSum[1] = (double)(vec3[1] + vec5[1]);
    midSum[2] = (double)(vec3[2] + vec5[2]);

    dotMidCross = (midSum[1] * cross[1] + midSum[0] * cross[0] + midSum[2] * cross[2]) * 0.5;
    dotV4Cross  = (double)vec4[1] * cross[1] + (double)vec4[0] * cross[0] + (double)vec4[2] * cross[2];

    scale = dotV4Cross - dotMidCross;

    Assert(scale > 0.0, s_assertDisable_collvec_scale);

    {
        double invScale = 1.0 / scale;
        out[0] = (float)(cross[0]    * invScale);
        out[1] = (float)(cross[1]    * invScale);
        out[2] = (float)(cross[2]    * invScale);
        out[3] = (float)(dotMidCross * invScale);
    }
}

/*
================
ComputeBaryCoords

Wrapper: builds B = v1-v0 and A = v2-v0 on the stack, then calls
ComputeBaryCoordsInternal twice with different parameter permutations
to compute two barycentric coordinate sets (bary0 at arg5, bary1 at arg6).
================
*/
void ComputeBaryCoords(const float *v0, const float *v1, const float *v2,
                       const float *arg4, float *bary0, float *bary1)
{
    float A[3];  /* v2 - v0 */
    float B[3];  /* v1 - v0 */

    B[0] = v1[0] - v0[0];
    B[1] = v1[1] - v0[1];
    B[2] = v1[2] - v0[2];

    A[0] = v2[0] - v0[0];
    A[1] = v2[1] - v0[1];
    A[2] = v2[2] - v0[2];

    /* first call: bary0 */
    ComputeBaryCoordsInternal(arg4, A, v0, v1, v2, bary0);

    /* second call: bary1 */
    ComputeBaryCoordsInternal(B, arg4, v0, v2, v1, bary1);
}

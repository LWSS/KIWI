/*
 * xanim_public.c — quaternion-to-matrix helpers.
 *
 * Source: ..\src\xanim\xanim_public.h
 *
 * Both functions were originally inline helpers in xanim_public.h
 * (MSVC emitted them as standalone functions because they are too
 * large to inline). They convert a DObjAnimMat (quat + trans +
 * transWeight) into either a packed 3x3 axis matrix or a 64-byte
 * DObjSkelMat with translation.
 *
 * The transWeight field is the caller's precomputed (2.0 / |q|^2)
 * scale — when multiplied into every quaternion product, it folds
 * the factor-of-2 and normalization out of the matrix formula:
 *
 *     xs = qx * s; ys = qy * s; zs = qz * s
 *     xx = qx * xs; yy = qy * ys; zz = qz * zs
 *     xy = qy * xs; xz = qz * xs; yz = qz * ys
 *     wx = qw * xs; wy = qw * ys; wz = qw * zs
 *
 *     axis[0] = ( 1 - (yy+zz),  xy + wz,  xz - wy )
 *     axis[1] = ( xy - wz,      1 - (xx+zz),  yz + wx )
 *     axis[2] = ( xz + wy,      yz - wx,  1 - (xx+yy) )
 */

#include "cod2rad64.h"

static char s_assertDisable_DObjAnimMatToAxis_axis2;
static char s_assertDisable_DObjAnimMatToAxis_axis1;
static char s_assertDisable_DObjAnimMatToAxis_axis0;
static char s_assertDisable_DObjAnimMatToSkelMat_axis2;
static char s_assertDisable_DObjAnimMatToSkelMat_axis1;
static char s_assertDisable_DObjAnimMatToSkelMat_axis0;
static char s_assertDisable_DObjAnimMatToSkelMat_quat;

/*
================
DObjAnimMatToAxis

Converts a DObjAnimMat's quaternion into a packed 3x3 axis matrix.
Asserts that each resulting axis row is finite.
================
*/
void DObjAnimMatToAxis(const DObjAnimMat_t *mat, float axis[3][3])
{
    float qx = mat->quat[0];
    float qy = mat->quat[1];
    float qz = mat->quat[2];
    float qw = mat->quat[3];
    float s  = mat->transWeight;

    float xs = qx * s;
    float ys = qy * s;
    float zs = qz * s;

    float xx = qx * xs;
    float yy = qy * ys;
    float zz = qz * zs;

    float xy = qy * xs;
    float xz = qz * xs;
    float yz = qz * ys;

    float wx = qw * xs;
    float wy = qw * ys;
    float wz = qw * zs;

    axis[0][0] = 1.0f - (yy + zz);
    axis[0][1] = xy + wz;
    axis[0][2] = xz - wy;

    axis[1][0] = xy - wz;
    axis[1][1] = 1.0f - (xx + zz);
    axis[1][2] = yz + wx;

    axis[2][0] = xz + wy;
    axis[2][1] = yz - wx;
    axis[2][2] = 1.0f - (xx + yy);

    Assert(!IS_NAN(axis[0][0]) && !IS_NAN(axis[0][1]) && !IS_NAN(axis[0][2]),
           s_assertDisable_DObjAnimMatToAxis_axis0);
    Assert(!IS_NAN(axis[1][0]) && !IS_NAN(axis[1][1]) && !IS_NAN(axis[1][2]),
           s_assertDisable_DObjAnimMatToAxis_axis1);
    Assert(!IS_NAN(axis[2][0]) && !IS_NAN(axis[2][1]) && !IS_NAN(axis[2][2]),
           s_assertDisable_DObjAnimMatToAxis_axis2);
}

/*
================
DObjAnimMatToSkelMat

Converts a DObjAnimMat into a 64-byte DObjSkelMat (3 axis rows with
zero pad + translation row with 1.0f in the w slot). Asserts that
the input quaternion is finite before the math, and that each
resulting axis row is finite afterwards.
================
*/
void DObjAnimMatToSkelMat(const DObjAnimMat_t *mat, DObjSkelMat_t *skelMat)
{
    float qx, qy, qz, qw, s;
    float xs, ys, zs;
    float xx, yy, zz;
    float xy, xz, yz;
    float wx, wy, wz;

    Assert(!IS_NAN(mat->quat[0]) && !IS_NAN(mat->quat[1]) &&
           !IS_NAN(mat->quat[2]) && !IS_NAN(mat->quat[3]),
           s_assertDisable_DObjAnimMatToSkelMat_quat);

    qx = mat->quat[0];
    qy = mat->quat[1];
    qz = mat->quat[2];
    qw = mat->quat[3];
    s  = mat->transWeight;

    skelMat->axis[0][3] = 0.0f;
    skelMat->axis[1][3] = 0.0f;
    skelMat->axis[2][3] = 0.0f;

    xs = qx * s;
    ys = qy * s;
    zs = qz * s;

    xx = qx * xs;
    yy = qy * ys;
    zz = qz * zs;

    xy = qy * xs;
    xz = qz * xs;
    yz = qz * ys;

    wx = qw * xs;
    wy = qw * ys;
    wz = qw * zs;

    skelMat->axis[0][0] = 1.0f - (yy + zz);
    skelMat->axis[0][1] = xy + wz;
    skelMat->axis[0][2] = xz - wy;

    skelMat->axis[1][0] = xy - wz;
    skelMat->axis[1][1] = 1.0f - (xx + zz);
    skelMat->axis[1][2] = yz + wx;

    skelMat->axis[2][0] = xz + wy;
    skelMat->axis[2][1] = yz - wx;
    skelMat->axis[2][2] = 1.0f - (xx + yy);

    Assert(!IS_NAN(skelMat->axis[0][0]) && !IS_NAN(skelMat->axis[0][1]) && !IS_NAN(skelMat->axis[0][2]),
           s_assertDisable_DObjAnimMatToSkelMat_axis0);
    Assert(!IS_NAN(skelMat->axis[1][0]) && !IS_NAN(skelMat->axis[1][1]) && !IS_NAN(skelMat->axis[1][2]),
           s_assertDisable_DObjAnimMatToSkelMat_axis1);
    Assert(!IS_NAN(skelMat->axis[2][0]) && !IS_NAN(skelMat->axis[2][1]) && !IS_NAN(skelMat->axis[2][2]),
           s_assertDisable_DObjAnimMatToSkelMat_axis2);

    skelMat->trans[0] = mat->trans[0];
    skelMat->trans[1] = mat->trans[1];
    skelMat->trans[2] = mat->trans[2];
    skelMat->trans[3] = 1.0f;
}

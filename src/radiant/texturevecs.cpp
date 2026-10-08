#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\common\texturevecs.cpp — the texture-projection axis/matrix TU (its assert
// strings name this file).  Carved out of brush.cpp 2026-07-30: Ed_Normal_Calc
// (TextureAxisFromPlane, 0x459A60), Ed_texturevecs (0x459BA0), Face_MoveTexture
// (0x45A1C0, texdef -> 2x4 matrix), texturevecs_02_exact (0x459CC0, the inverse), plus
// the com_math leaf Ed_SinCos (0x4AABC0, the single canonical copy).

#include "stdafx.h"
#include <universal/assertive.h>    // iassert/vassert (USE_ASSERTS always on)
#include <cmath>
#include "kiwi_vec.h"               // KIWI: KIWI_RAD2DEG

// ── Ed_baseaxis (flt_6DE31C) — TextureAxisFromPlane table, verbatim from the IDB
//    (6 entries × { xv[3], yv[3], normal[3] }; Ed_Normal_Calc reads only xv/yv). ──
static const float Ed_baseaxis[6][9] =
{
    { 1, 0, 0,   0,-1, 0,   0, 0,-1 },
    { 1, 0, 0,   0,-1, 0,   1, 0, 0 },
    { 0, 1, 0,   0, 0,-1,  -1, 0, 0 },
    { 0, 1, 0,   0, 0,-1,   0, 1, 0 },
    { 1, 0, 0,   0, 0,-1,   0,-1, 0 },
    { 1, 0, 0,   0, 0,-1,   0, 0, 0 },
};

// 0x459A60  Normal_Calc — pick the dominant ±axis of `normal` (dot-test order
// +z,-z,+x,-x,+y,-y → index 0..5) and copy its base S/T vectors into xv/yv.
// strict >/< compares, tie keeps the lower index.  KIWI: a tie within KTEXAXIS_TIE keeps it too -
// a face turned to exactly 45 deg had its projection axis (and so what its texdef means)
// decided by noise in the normal, and a thin face's normal carries ~1e-4 of it (a 2-unit
// face 2500 out: one ulp of plane-point rounding over a 2-unit edge).  cod4map's
// TexturePlanesFromBasis uses the same band.
#define KTEXAXIS_TIE 1.0e-4f
void Ed_Normal_Calc( const float *normal, float *xv, float *yv )
{
    int   v3  = 0;
    float v18 = 0.0f;
    float v12 = normal[2];   if ( v12 > 0.0f )  v18 = v12;
    float v13 = -normal[2];  if ( v18 + ( v18 > 0.0f ? KTEXAXIS_TIE : 0.0f ) < v13 ) { v18 = v13; v3 = 1; }
    float v14 = normal[0];   if ( v18 + ( v18 > 0.0f ? KTEXAXIS_TIE : 0.0f ) < v14 ) { v18 = v14; v3 = 2; }
    float v15 = -normal[0];  if ( v18 + ( v18 > 0.0f ? KTEXAXIS_TIE : 0.0f ) < v15 ) { v18 = v15; v3 = 3; }
    float v16 = normal[1];   if ( v18 + ( v18 > 0.0f ? KTEXAXIS_TIE : 0.0f ) < v16 ) { v18 = v16; v3 = 4; }
    float v17 = -normal[1];  if ( v18 + ( v18 > 0.0f ? KTEXAXIS_TIE : 0.0f ) < v17 ) {            v3 = 5; }
    xv[0] = Ed_baseaxis[v3][0]; xv[1] = Ed_baseaxis[v3][1]; xv[2] = Ed_baseaxis[v3][2];
    yv[0] = Ed_baseaxis[v3][3]; yv[1] = Ed_baseaxis[v3][4]; yv[2] = Ed_baseaxis[v3][5];
}

// 0x459BA0  texturevecs_xv_yv_s_t_normal — base S/T vectors + the s/t axis indices.
// Normal_Calc(normal,xv,yv) + s/t axis selection; carries the 5
// texturevecs.cpp:46-50 level-0 param-null asserts.
static void Ed_texturevecs( float *yv, float *xv, const float *normal, int *s, int *t )
{
    iassert( normal );   // texturevecs.cpp:46
    iassert( xv );       // texturevecs.cpp:47
    iassert( yv );       // texturevecs.cpp:48
    iassert( s );        // texturevecs.cpp:49
    iassert( t );        // texturevecs.cpp:50
    Ed_Normal_Calc( normal, xv, yv );
    if ( 0.0f == xv[0] ) *s = ( 0.0f == xv[1] ) ? 2 : 1; else *s = 0;
    if ( 0.0f == yv[0] ) *t = ( 0.0f == yv[1] ) ? 2 : 1; else *t = 0;
}

// 0x4AABC0  SinCos (com_math) — degrees → (*s, *c), exact at 0/90/180/270.
// radians/wrap narrow to FLOAT before fsincos (see inline); carries the 2
// com_math.cpp:3864-3865 param-null asserts.
void Ed_SinCos( float deg, float *s, float *c )
{
    iassert( s );   // com_math.cpp:3864
    iassert( c );   // com_math.cpp:3865
    double v = deg;
    if ( deg < 0.0 ) v = (float)( v + 360.0 );   // IDA 0x4aac21: fstp/fld [deg] narrows the +360 wrap to float
    if      ( 0.0   == v ) { *c = 1.0f;  *s = 0.0f; }
    else if ( 90.0  == v ) { *c = 0.0f;  *s = 1.0f; }
    else if ( 180.0 == v ) { *c = -1.0f; *s = 0.0f; }
    else if ( 270.0 == v ) { *c = 0.0f;  *s = -1.0f; }
    // IDA 0x4aac9d: fstp/fld [deg] narrows radians to FLOAT before fsincos (deg is a 4-byte slot).
    else { float r = (float)( DEG2RAD( v ) ); *c = (float)cos( (double)r ); *s = (float)sin( (double)r ); }
}

// 0x4A47D0  Vec3Normalize (out-of-place) — normalize inVecs[0..2] into outS[0..2].
// lensq narrows to float; zero-guard (-len >= 0 -> 1.0).
void sub_4A47D0( float *outS, const float *inVecs )
{
    float       *a1 = (float *)outS;
    const float *a2 = (const float *)inVecs;
    float len = (float)sqrt( (double)( a2[1]*a2[1] + a2[0]*a2[0] + a2[2]*a2[2] ) );
    if ( -len >= 0.0f ) len = 1.0f;             // length <= 0 ⇒ divide by 1
    float inv = 1.0f / len;
    a1[0] = a2[0] * inv;
    a1[1] = inv * a2[1];
    a1[2] = inv * a2[2];
}

// 0x45A1C0  Face_MoveTexture — build the 2×4 texture-projection matrix `outVecs`
// from a texdef (scale@surfDef[0,1], shift@uvBase[0,1], rotate, crossterm) and the
// face normal. surfDef/outVecs/uvBase are byte-pointers (IDA usercall convention,
// shared with Brush_ApplyTextureProjection's caller).
// full 2x4 matrix chain, pure x87; bit-exact rotation depends on Ed_SinCos.
// Carries the 7 texturevecs.cpp:157,160-166 level-1 output-contract asserts (s!=t +
// xv/yv unit-vector properties).
void Face_MoveTexture( const float *surfDef, const float *normal, float *outVecs,
                       const float *uvBase, float rotate, float crossterm )
{
    const float *scale = (const float *)surfDef;   // [0]=sizeX [1]=sizeY
    const float *shift = (const float *)uvBase;     // [0]=shiftX [1]=shiftY
    float       *a3    = (float *)outVecs;          // out matrix [0..7]

    float sx = scale[0]; if ( 0.0f == sx ) sx = 128.0f;
    float sy = scale[1]; if ( 0.0f == sy ) sy = 128.0f;

    float pvecs[2][3];                             // the binary's array (assert strings)
    float *xv = pvecs[0], *yv = pvecs[1];
    int   s, t;
    Ed_texturevecs( yv, xv, normal, &s, &t );
    // texturevecs output-contract validation (common/texturevecs.cpp:157,160-166; ALL LEVEL 1) —
    // RESTORED (the prior port dropped these 7 asserts; the strings keep the original `pvecs` names).
    vassert( (s != t), "(s) = %i", s );   // texturevecs.cpp:157
    int r = 3 - s - t;
    vassert( (pvecs[0][s] == 1 || pvecs[0][s] == -1), "(pvecs[0][s]) = %g", pvecs[0][s] );   // :160
    vassert( (pvecs[0][t] == 0), "(pvecs[0][t]) = %g", pvecs[0][t] );   // :161
    vassert( (pvecs[0][r] == 0), "(pvecs[0][r]) = %g", pvecs[0][r] );   // :162
    vassert( (pvecs[1][s] == 0), "(pvecs[1][s]) = %g", pvecs[1][s] );   // :164
    vassert( (pvecs[1][t] == 1 || pvecs[1][t] == -1), "(pvecs[1][t]) = %g", pvecs[1][t] );   // :165
    vassert( (pvecs[1][r] == 0), "(pvecs[1][r]) = %g", pvecs[1][r] );   // :166

    float sinv, cosv;
    Ed_SinCos( rotate, &sinv, &cosv );

    float v10 = xv[s] / sx;
    a3[s] = cosv * v10;
    a3[t] = v10 * sinv;
    a3[r] = 0.0f;
    float v12 = yv[t] / sy;
    a3[s + 4] = -sinv * v12;
    a3[t + 4] = v12 * cosv;
    a3[r + 4] = 0.0f;
    a3[0] = a3[4] * crossterm + a3[0];
    a3[1] = a3[5] * crossterm + a3[1];
    a3[2] = crossterm * a3[6] + a3[2];
    a3[3] = -shift[0] / sx;
    a3[7] = -shift[1] / sy;
}

// ════════════════════════════════════════════════════════════════════════════
//  0x459CC0  texturevecs_02 (common/texturevecs.cpp) — the INVERSE of Face_MoveTexture:
//  decompose a world texture matrix (two 4-vec rows) back into a texdef.  Phases: B rows
//  into the projection plane, C crossterm = row0.row1 / |row1|^2, D de-skew row 0, E/F
//  sizes = 1 / |row|, G size[1] sign from the orientation term, I rotate = atan2, K shift.
//  KIWI (2026-10-05): the one decomposition, in double, used by every caller (texture lock,
//  Match Face, face align, bevel, UV editor).  It snaps onto the binary's grids (1/8 size /
//  shift, 0.25 deg, 0.001 crossterm) only within float noise (8 ulps of the value, or of
//  `scale` - the quantity's own size, so a value near 0 still sheds its noise: crossterm 1,
//  rotate 90 deg, shift one repeat).  The binary's float version (with its sub_4AAD00
//  snapper) moved REAL values within 0.001 / 0.005 deg: a locked size of 52.1259 became
//  52.125, sliding the texture 0.009 repeats 450 repeats from its origin.
// ════════════════════════════════════════════════════════════════════════════
static double KiwiTexSnap( double v, double granularity, double scale )
{
    const double r = floor( v * granularity + 0.5 ) / granularity;
    double m = fabs( r ) > fabs( v ) ? fabs( r ) : fabs( v );
    if ( scale > m )
        m = scale;
    return fabs( v - r ) <= m * 9.6e-7 ? r : v;
}

void texturevecs_02_exact( float *outSize, const double *texMat, const float *planeNormal, double planeDist,
                           float *outShift, float *outRotate, float *outCrossterm )
{
    float pvecs[2][3];
    float *xv = pvecs[0], *yv = pvecs[1];
    int   s, t;
    Ed_texturevecs( yv, xv, planeNormal, &s, &t );
    const int r = 3 - s - t;
    double m[8];
    for ( int i = 0; i < 8; ++i )
        m[i] = texMat[i];
    for ( int row = 0; row < 2; ++row )                // Phase B: into the projection plane
    {
        double *w = m + row * 4;
        if ( w[r] == 0.0 || planeNormal[r] == 0.0f )
            continue;
        const double ratio = w[r] / planeNormal[r];
        for ( int k = 0; k < 3; ++k )
            w[k] -= ratio * planeNormal[k];
        w[3] += ratio * planeDist;
    }
    double cross = ( m[4] * m[0] + m[5] * m[1] + m[6] * m[2] ) / ( m[4] * m[4] + m[5] * m[5] + m[6] * m[6] );
    cross = KiwiTexSnap( cross, 1000.0, 1.0 );
    for ( int k = 0; k < 3; ++k )                      // Phase D: de-skew row 0
        m[k] -= cross * m[4 + k];
    double size0 = 1.0 / sqrt( m[0] * m[0] + m[1] * m[1] + m[2] * m[2] );
    double size1 = 1.0 / sqrt( m[4] * m[4] + m[5] * m[5] + m[6] * m[6] );
    if ( m[4 + t] * m[s] * xv[s] * yv[t] - m[4 + s] * m[t] * xv[s] * yv[t] < 0.0 )
        size1 = -size1;                                // Phase G
    size0 = KiwiTexSnap( size0, 8.0, 0.0 );
    size1 = KiwiTexSnap( size1, 8.0, 0.0 );
    const double rotate = KiwiTexSnap( atan2( m[t] * xv[s], m[s] * xv[s] ) * KIWI_RAD2DEG, 4.0, 90.0 );
    outSize[0]    = (float)size0;
    outSize[1]    = (float)size1;
    *outCrossterm = (float)cross;
    *outRotate    = (float)rotate;
    outShift[0]   = (float)KiwiTexSnap( -m[3] * size0, 8.0, fabs( size0 ) );
    outShift[1]   = (float)KiwiTexSnap( -m[7] * size1, 8.0, fabs( size1 ) );
}


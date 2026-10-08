#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Kept separate from kiwi_lines.h so vector-only callers do not depend on the
// overlay renderer; feature-specific predicates and tolerances stay local.

#include <float.h>
#include <math.h>

// The one PI spelling (double).  Float math casts once - (float)KIWI_PI - so the
// expression stays float.
#define KIWI_PI      3.14159265358979323846
#define KIWI_DEG2RAD ( KIWI_PI / 180.0 )
#define KIWI_RAD2DEG ( 180.0 / KIWI_PI )

inline float Dot3( const float *a, const float *b )
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline void Copy3( const float *a, float *o ) { o[0]=a[0]; o[1]=a[1]; o[2]=a[2]; }
inline void Sub3( const float *a, const float *b, float *o )
{ o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
inline void Add3( const float *a, const float *b, float *o )
{ o[0]=a[0]+b[0]; o[1]=a[1]+b[1]; o[2]=a[2]+b[2]; }
inline void Mad3( const float *a, const float *d, float s, float *o )
{ o[0]=a[0]+d[0]*s; o[1]=a[1]+d[1]*s; o[2]=a[2]+d[2]*s; }
// `o` may alias an input.
inline void Cross3( const float *a, const float *b, float *o )
{
    const float x = a[1]*b[2] - a[2]*b[1];
    const float y = a[2]*b[0] - a[0]*b[2];
    const float z = a[0]*b[1] - a[1]*b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
inline float Len3( const float *a ) { return sqrtf( Dot3( a, a ) ); }
inline float DistSq3( const float *a, const float *b )
{
    const float ex = a[0] - b[0], ey = a[1] - b[1], ez = a[2] - b[2];
    return ex * ex + ey * ey + ez * ez;
}
inline float Dist3( const float *a, const float *b ) { return sqrtf( DistSq3( a, b ) ); }
// Per-axis box test: every component within `tol`.
inline bool PointNear( const float *a, const float *b, float tol )
{
    return fabsf( a[0] - b[0] ) <= tol && fabsf( a[1] - b[1] ) <= tol && fabsf( a[2] - b[2] ) <= tol;
}

// False leaves `v` untouched. At the shared 1e-6 tolerance, strict `l > eps`
// keeps the majority form over kiwi_loft's old `<` check and rejects equality/NaN.
inline bool Norm3( float *v, float eps = 1.0e-6f )
{
    const float l = Len3( v );
    if ( !( l > eps ) )
        return false;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return true;
}

// Returns the length; normalizes (by the reciprocal) only above `eps`.
inline float Len3Norm( float *v, float eps = 1.0e-6f )
{
    const float l = Len3( v );
    if ( l > eps )
    {
        const float inv = 1.0f / l;
        v[0] *= inv; v[1] *= inv; v[2] *= inv;
    }
    return l;
}

// Double twins.
inline double Dot3d( const double *a, const double *b )
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline void Cross3d( const double *a, const double *b, double *o )
{
    const double x = a[1]*b[2] - a[2]*b[1];
    const double y = a[2]*b[0] - a[0]*b[2];
    const double z = a[0]*b[1] - a[1]*b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
inline void Scale3d( double *v, double s ) { v[0] *= s; v[1] *= s; v[2] *= s; }
// False (v untouched) for a length^2 at or under 1e-24, or not finite.
inline bool Norm3d( double *v )
{
    const double l2 = Dot3d( v, v );
    if ( !( l2 > 1.0e-24 && l2 <= DBL_MAX ) )
        return false;
    Scale3d( v, 1.0 / sqrt( l2 ) );
    return true;
}

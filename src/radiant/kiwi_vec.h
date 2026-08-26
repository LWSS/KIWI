#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Kept separate from kiwi_lines.h so vector-only callers do not depend on the
// overlay renderer; feature-specific predicates and tolerances stay local.

#include <math.h>

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
// `o` must not alias either input; later components reuse earlier input values.
inline void Cross3( const float *a, const float *b, float *o )
{
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
}
inline float Len3( const float *a ) { return sqrtf( Dot3( a, a ) ); }

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

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_vec.h — KIWI-UX (CLEANUP, A-15): the ONE spelling of the three-float
// helpers.
//
// These eight were copy-pasted into an anonymous namespace in 25 kiwi_*.cpp
// files (85 definitions).  Every arithmetic body was byte-identical modulo
// whitespace, so they are collected here verbatim — same names, same signatures,
// same operand order — and the per-file copies deleted.  Call sites are
// unchanged by design: this file makes the names global inlines, so a TU that
// used to define its own now only adds the include.
//
// WHY A NEW HEADER RATHER THAN kiwi_lines.h: kiwi_lines.h is the line-batcher's
// budget contract and pulls the overlay renderer in with it.  A file that only
// needs Dot3 (kiwi_validity, kiwi_selext, kiwi_pick) must not acquire a
// dependency on the renderer to get it.  This header includes nothing but
// <math.h> and has no link edge at all.
//
// ── THE ONE DISAGREEMENT, RECORDED ───────────────────────────────────────────
// Norm3 was seven copies at ONE epsilon (1.0e-6f) but two comparison forms:
// six wrote `if ( !( l > 1.0e-6f ) ) return false;` and kiwi_loft.cpp wrote
// `if ( l < KLOFT_EPS ) return false;` (KLOFT_EPS is itself 1.0e-6f).  The two
// differ only on a NaN length and on l == eps exactly, where the `<` form
// ACCEPTS and then divides.  The strict form below is the one kept — it is the
// majority spelling and it is the safe side of that pair.  A caller that needs a
// different tolerance passes it: the epsilon is an explicit defaulted argument
// so a per-site value never has to be re-hidden inside a private copy.
//
// Nothing else belongs here.  Per-file predicates that happen to be built out of
// these (PointNear, RayPlane, Dist3, …) stay where their tolerance is argued.
// ─────────────────────────────────────────────────────────────────────────────

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
inline void Cross3( const float *a, const float *b, float *o )
{
    o[0] = a[1]*b[2] - a[2]*b[1];
    o[1] = a[2]*b[0] - a[0]*b[2];
    o[2] = a[0]*b[1] - a[1]*b[0];
}
inline float Len3( const float *a ) { return sqrtf( Dot3( a, a ) ); }

// Normalise in place.  False (and `v` untouched) when the length is not above
// `eps` — see THE ONE DISAGREEMENT above before changing the comparison.
inline bool Norm3( float *v, float eps = 1.0e-6f )
{
    const float l = Len3( v );
    if ( !( l > eps ) )
        return false;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return true;
}

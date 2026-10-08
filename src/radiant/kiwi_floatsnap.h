#pragma once
// KIWI (float round trip): the one noise-only half-grid snap.  The binary rounds plane
// points / patch bounds / rotated control points to 0.5; KIWI moves a value onto the 0.5
// grid only when it is within float noise of it - 2 ulps of the larger of |x| and `scale`,
// the magnitude of the coordinates it was computed with (a turn leaves ~1 ulp of the
// radius even on a coordinate near 0).  63.99999 -> 64 and 4e-6 -> 0 on a 128 face still
// clean up; 64.0007 is kept (the old 0.001 floor moved it on every paste / nudge), and so is
// a 0.01 on a face 30000 out (4 ulps there were 0.0144).

#include <math.h>

// Two float ulps at magnitude m (the spacing of floats in [2^(e-1), 2^e) is 2^(e-24)).
inline double KiwiSnap_TwoUlps( double m )
{
    int e;
    frexp( m, &e );
    return ldexp( 1.0, e - 23 );
}

// True (and *out = the grid value) when x is within that noise of the 0.5 grid.
inline bool KiwiSnap_Half( float x, double scale, float *out )
{
    const double r = floor( 0.5 + 2.0 * (double)x ) * 0.5;
    double m = fabs( r );
    if ( scale > m )
        m = scale;
    const bool on = m == 0.0 ? x == 0.0f : fabs( (double)x - r ) <= KiwiSnap_TwoUlps( m );
    *out = on ? (float)r : x;
    return on;
}

inline float KiwiSnap_NoiseHalf( float x, double scale )
{
    float out;
    KiwiSnap_Half( x, scale, &out );
    return out;
}

// The binary's inline fistp (round to nearest, no fldcw) with its 2^-30 guard, added in
// DOUBLE - a float add loses it.  Round sends x.5 up; Floor / Ceil are its
// x -/+ 0.49999999907 idioms.  A C cast (truncate) is not any of these.
#define KIWI_FISTP_BIAS 9.313225746154785e-10
inline long KiwiRound_Fistp( double x ) { return lrint( x + KIWI_FISTP_BIAS ); }
inline long KiwiFloor_Fistp( double x ) { return lrint( x - ( 0.5 - KIWI_FISTP_BIAS ) ); }
inline long KiwiCeil_Fistp( double x )  { return lrint( x + ( 0.5 - KIWI_FISTP_BIAS ) ); }

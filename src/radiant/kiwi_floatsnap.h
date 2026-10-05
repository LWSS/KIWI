#pragma once
// KIWI (float round trip): the one noise-only half-grid snap.  The binary rounds plane
// points / patch bounds / rotated control points to 0.5; KIWI moves a value onto the 0.5
// grid only when it is within float noise of it - 4 ulps of the larger of |x| and `scale`,
// the magnitude of the coordinates it was computed with (a turn leaves ~1 ulp of the
// radius even on a coordinate near 0).  63.99999 -> 64 and 4e-6 -> 0 on a 128 face still
// clean up; 64.0007 is kept (the old 0.001 floor moved it on every paste / nudge).

#include <math.h>

#define KIWI_SNAP_NOISE_ULPS 4.8e-7       // 4 float ulps, relative (2^-23 each)

// True (and *out = the grid value) when x is within that noise of the 0.5 grid.
inline bool KiwiSnap_Half( float x, double scale, float *out )
{
    const double r = floor( 0.5 + 2.0 * (double)x ) * 0.5;
    double m = fabs( r );
    if ( scale > m )
        m = scale;
    const bool on = fabs( (double)x - r ) <= m * KIWI_SNAP_NOISE_ULPS;
    *out = on ? (float)r : x;
    return on;
}

inline float KiwiSnap_NoiseHalf( float x, double scale )
{
    float out;
    KiwiSnap_Half( x, scale, &out );
    return out;
}

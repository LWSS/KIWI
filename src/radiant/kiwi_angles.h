#pragma once
// KIWI (2026-10-05, precision): entity "angles" composed in DOUBLE.  The engine's float
// AnglesToAxis -> MatrixMultiply -> AxisToAngles left ~1e-6 deg of noise per turn, which the
// exact "angles" writers now show and every turn re-rounded.  Conventions are engine_stubs'
// AnglesToAxis / AxisToAngles (pitch yaw roll; rows forward / left / up; pitch and yaw out in
// [0, 360), roll in (-180, 180]); a turn composes as axis * rotM (Select_RotateFixedSize).

#include <math.h>

// sin / cos of degrees, exact at multiples of 90 (cos of 90 deg in radians is 6e-17, not 0).
inline void KiwiAng_SinCos( double deg, double *s, double *c )
{
    double r = fmod( deg, 360.0 );
    if ( r < 0.0 )
        r += 360.0;
    if ( r == 0.0 )   { *s =  0.0; *c =  1.0; return; }
    if ( r == 90.0 )  { *s =  1.0; *c =  0.0; return; }
    if ( r == 180.0 ) { *s =  0.0; *c = -1.0; return; }
    if ( r == 270.0 ) { *s = -1.0; *c =  0.0; return; }
    const double rad = r * ( 3.14159265358979323846 / 180.0 );
    *s = sin( rad );
    *c = cos( rad );
}

inline void KiwiAng_ToAxis( const double ang[3], double axis[3][3] )
{
    double sp, cp, sy, cy, sr, cr;
    KiwiAng_SinCos( ang[0], &sp, &cp );
    KiwiAng_SinCos( ang[1], &sy, &cy );
    KiwiAng_SinCos( ang[2], &sr, &cr );
    axis[0][0] = cp * cy;                 axis[0][1] = cp * sy;                 axis[0][2] = -sp;
    axis[1][0] = cy * sr * sp - cr * sy;  axis[1][1] = sr * sp * sy + cr * cy;  axis[1][2] = sr * cp;
    axis[2][0] = cy * cr * sp + sy * sr;  axis[2][1] = sy * cr * sp - cy * sr;  axis[2][2] = cr * cp;
}

inline void KiwiAng_FromAxis( const double axis[3][3], double ang[3] )
{
    const double r2d = 180.0 / 3.14159265358979323846;
    const double *f  = axis[0];
    if ( fabs( f[0] ) < 1.0e-12 && fabs( f[1] ) < 1.0e-12 )
    {
        ang[1] = 0.0;                          // gimbal lock: yaw 0, roll carries the rest
        ang[0] = f[2] > 0.0 ? 270.0 : 90.0;
    }
    else
    {
        ang[1] = atan2( f[1], f[0] ) * r2d;
        if ( ang[1] < 0.0 )
            ang[1] += 360.0;
        ang[0] = -atan2( f[2], sqrt( f[0] * f[0] + f[1] * f[1] ) ) * r2d;
        if ( ang[0] < 0.0 )
            ang[0] += 360.0;
    }
    double sy, cy, sp, cp;     // the left vector back through -yaw, -pitch: its pitch is roll
    KiwiAng_SinCos( -ang[1], &sy, &cy );
    KiwiAng_SinCos( -ang[0], &sp, &cp );
    const double t  = axis[1][0] * cy - axis[1][1] * sy;
    const double ry = axis[1][1] * cy + sy * axis[1][0];
    const double r0 = t * cp + axis[1][2] * sp;
    const double r2 = cp * axis[1][2] - t * sp;
    double roll;
    if ( fabs( r0 ) < 1.0e-12 && fabs( ry ) < 1.0e-12 )
        roll = r2 > 0.0 ? -90.0 : 90.0;
    else
        roll = -atan2( r2, sqrt( r0 * r0 + ry * ry ) ) * r2d;
    if ( ry >= 0.0 )        ang[2] = -roll;
    else if ( roll >= 0.0 ) ang[2] = roll - 180.0;
    else                    ang[2] = roll + 180.0;
}

// v wrapped into the 360 window of its base's sign: [0, 360) for a base >= 0 (the binary's
// AxisToAngles range), (-360, 0] for a negative one - so turns of any count come back to the
// same text in either family ("0 37.25 0" x4 R 90, "0 -90 0" R 90 / -90), and double noise
// (1e-15) is dropped.
inline double KiwiAng_Window( double v, double base )
{
    v = floor( v * 1.0e9 + 0.5 ) * 1.0e-9;                      // double noise
    v = fmod( v, 360.0 );                                       // (-360, 360)
    if ( base >= 0.0 ) { if ( v < 0.0 )  v += 360.0; }
    else               { if ( v > 0.0 )  v -= 360.0; }
    return v + 0.0;                                             // no "-0"
}

// Angular distance of two angles in degrees, 0..180.
inline double KiwiAng_Dist( double a, double b )
{
    double d = fmod( fabs( a - b ), 360.0 );
    return d > 180.0 ? 360.0 - d : d;
}

// The angles of an orientation written closest to the ones it started from: of (p, y, r)
// and its twin (180 - p, y + 180, r + 180) the nearer, and at gimbal lock (pitch 90 / 270,
// where only r - y / r + y matter) the base's roll kept first (a world-Z turn moves yaw),
// else its yaw; then each component in its base's window (KiwiAng_Window).
inline void KiwiAng_Near( const double in[3], const double base[3], double out[3] )
{
    double cand[6][3];
    int    n = 0;
    double sp, cp;
    KiwiAng_SinCos( in[0], &sp, &cp );
    if ( cp == 0.0 )                           // FromAxis' lock branch: yaw 0, roll L = r - y / r + y
    {
        const double L = in[2];
        cand[n][0] = in[0]; cand[n][2] = base[2];                                    // keep roll
        cand[n][1] = sp > 0.0 ? base[2] - L : L - base[2];
        ++n;
        cand[n][0] = in[0]; cand[n][1] = base[1];                                    // keep yaw
        cand[n][2] = sp > 0.0 ? L + base[1] : L - base[1];
        ++n;
    }
    cand[n][0] = in[0]; cand[n][1] = in[1]; cand[n][2] = in[2]; ++n;
    for ( int i = 0, m = n; i < m; ++i, ++n )  // each one's 180 twin
    {
        cand[n][0] = 180.0 - cand[i][0];
        cand[n][1] = cand[i][1] + 180.0;
        cand[n][2] = cand[i][2] + 180.0;
    }
    int    pick = 0;
    double best = 1.0e300;
    for ( int t = 0; t < n; ++t )
    {
        const double cost = KiwiAng_Dist( cand[t][0], base[0] ) + KiwiAng_Dist( cand[t][1], base[1] )
                          + KiwiAng_Dist( cand[t][2], base[2] );
        if ( cost < best - 1.0e-9 )
        {
            best = cost;
            pick = t;
        }
    }
    for ( int k = 0; k < 3; ++k )
        out[k] = KiwiAng_Window( cand[pick][k], base[k] );
}

// base turned by the row-major 3x3 `turn` (rows-as-axes: newAxis = axis * turn).
inline void KiwiAng_Turn( const double base[3], const double turn[3][3], double out[3] )
{
    double axis[3][3], turned[3][3], ang[3];
    KiwiAng_ToAxis( base, axis );
    for ( int a = 0; a < 3; ++a )
        for ( int b = 0; b < 3; ++b )
            turned[a][b] = axis[a][0] * turn[0][b] + axis[a][1] * turn[1][b] + axis[a][2] * turn[2][b];
    KiwiAng_FromAxis( turned, ang );
    KiwiAng_Near( ang, base, out );
}

// Mirror's 180 degree flip of one component as an INVOLUTION: + 180 on even half-turns,
// - 180 on odd ones, so flipping twice gives the same number back (the binary's
// AngleNormalize360 sent "-90" to 90 and back to 270, in float).
inline double KiwiAng_Flip180( double v )
{
    return ( (long long)floor( v / 180.0 ) & 1 ) ? v - 180.0 : v + 180.0;
}

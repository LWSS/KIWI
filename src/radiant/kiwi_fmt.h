#ifndef KIWI_FMT_H
#define KIWI_FMT_H

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// KIWI: Fixed notation for user-visible and map-authored numbers.
inline const char *KiwiFmt_Num( char *buf, size_t n, double v, int maxDecimals = 3 )
{
    if ( !buf || n == 0 )
        return "";

    if ( maxDecimals < 0 ) maxDecimals = 0;
    if ( maxDecimals > 15 ) maxDecimals = 15;
    _snprintf( buf, n, "%.*f", maxDecimals, v );
    buf[n - 1] = '\0';

    char *dot = strchr( buf, '.' );
    if ( dot )
    {
        char *end = buf + strlen( buf );
        while ( end > dot && end[-1] == '0' )
            --end;
        if ( end > dot && end[-1] == '.' )
            --end;
        *end = '\0';
    }

    if ( !strcmp( buf, "-0" ) )
    {
        buf[0] = '0';
        buf[1] = '\0';
    }
    return buf;
}

#define KIWI_FMT_FLOAT "%.3f"

// KIWI (2026-10-05): the fewest decimals - at least one, as the binary's "%.1f" - that
// read back as the same float.  Values on the 0.1 grid print exactly as before; anything
// finer keeps its digits instead of being moved to the 0.1 grid.
// minDecimals 0 writes whole values bare ("90"), as the binary's "%g" did for "angles".
inline const char *KiwiFmt_Exact( char *buf, size_t n, float v, int minDecimals = 1 )
{
    if ( !buf || n == 0 )
        return "";
    for ( int d = minDecimals; d <= 12; ++d )
    {
        _snprintf( buf, n, "%.*f", d, (double)v );
        buf[n - 1] = '\0';
        if ( strtof( buf, 0 ) == v && (float)strtod( buf, 0 ) == v )   // sscanf / atof readers
            return buf;
    }
    _snprintf( buf, n, "%.9g", (double)v );       // tiny / huge: 9 digits always read back
    buf[n - 1] = '\0';
    return buf;
}

// KIWI (2026-10-05, load/save gauntlet): "x y z" that reads back as the same three floats -
// for keys like "angles", which the ported writers printed with %g (6 significant digits).
inline const char *KiwiFmt_Vec3Exact( char *buf, size_t n, const float *v, int minDecimals = 1 )
{
    char x[48], y[48], z[48];
    _snprintf( buf, n, "%s %s %s", KiwiFmt_Exact( x, sizeof( x ), v[0] + 0.0f, minDecimals ),   // +0: no "-0.0"
               KiwiFmt_Exact( y, sizeof( y ), v[1] + 0.0f, minDecimals ),
               KiwiFmt_Exact( z, sizeof( z ), v[2] + 0.0f, minDecimals ) );
    buf[n - 1] = '\0';
    return buf;
}

#endif

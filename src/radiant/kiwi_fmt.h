#ifndef KIWI_FMT_H
#define KIWI_FMT_H

#include <stddef.h>
#include <stdio.h>
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

#endif

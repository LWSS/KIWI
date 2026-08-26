#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Unit preferences stay outside IDA-shaped prefData_t and persist as strings so
// fractional, non-power-of-two grid spacing survives restarts.

#include "stdafx.h"
#include "kiwi_fmt.h"
#include "kiwi_units.h"
#include "radiant_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

extern int Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:118

namespace
{
    const char *KUX_SECTION = "KiwiUX";

    // Prevent near-zero divisors in grid emission, snapping, and unit conversion.
    const float KUNITS_MIN = 0.0001f;

    float s_perInch      = 1.0f;
    // The 1-inch default intentionally ignores persisted 10-inch values under the old key.
    float s_gridInches   = 1.0f;
    bool  s_loaded       = false;

    const char *KUX_GRID_ENTRY = "GridSpacingInches2";

    float ReadFloat( const char *entry, float defVal )
    {
        std::string s = Radiant_ProfileGetString( KUX_SECTION, entry, "" );
        if ( s.empty() )
            return defVal;
        const float v = (float)atof( s.c_str() );
        return ( v >= KUNITS_MIN ) ? v : defVal;
    }

    void WriteFloat( const char *entry, float v )
    {
        char buf[64];
        KiwiFmt_Num( buf, sizeof( buf ), v, 6 );
        Radiant_ProfileSetString( KUX_SECTION, entry, buf );
    }

    void Load()
    {
        if ( s_loaded )
            return;
        s_loaded     = true;                 // Profile reads may re-enter Load().
        s_perInch    = ReadFloat( "UnitsPerInch", 1.0f );
        s_gridInches = ReadFloat( KUX_GRID_ENTRY, 1.0f );
    }
}

// Units
float KiwiUnits_PerInch()
{
    Load();
    return s_perInch;
}

void KiwiUnits_SetPerInch( float unitsPerInch )
{
    Load();
    // The negated comparison rejects NaN as well as values below the floor.
    if ( !( unitsPerInch >= KUNITS_MIN ) )
    {
        Sys_Printf( "Units: %g units/inch refused (minimum is %g) — still %g.\n",
                    (double)unitsPerInch, (double)KUNITS_MIN, (double)s_perInch );
        return;
    }
    if ( unitsPerInch == s_perInch )
        return;
    s_perInch = unitsPerInch;
    WriteFloat( "UnitsPerInch", s_perInch );
}

float Units_ToDisplay( float world )
{
    return world / KiwiUnits_PerInch();
}

float Units_FromDisplay( float display )
{
    return display * KiwiUnits_PerInch();
}

const char *KiwiUnits_Format( char *buf, int bufSize, float world )
{
    if ( !buf || bufSize < 1 )
        return buf;
    // Use inches below 10 ft, feet below 300 ft, then yards.
    // Suppress sub-0.005-inch remainders instead of printing a trailing "0 in".
    const double in   = (double)Units_ToDisplay( world );
    const char  *sign = ( in < 0.0 ) ? "-" : "";
    const double a    = ( in < 0.0 ) ? -in : in;

    if ( a < 120.0 )
    {
        char value[64];
        _snprintf( buf, (size_t)bufSize, "%s in",
                   KiwiFmt_Num( value, sizeof( value ), in, 6 ) );
    }
    else if ( a < 3600.0 )                       // < 300 ft
    {
        const int    ft  = (int)( a / 12.0 );
        const double rem = a - (double)ft * 12.0;
        if ( rem < 0.005 )
            _snprintf( buf, (size_t)bufSize, "%s%d ft", sign, ft );
        else
        {
            char value[64];
            _snprintf( buf, (size_t)bufSize, "%s%d ft %s in", sign, ft,
                       KiwiFmt_Num( value, sizeof( value ), rem, 4 ) );
        }
    }
    else
    {
        const int    yd    = (int)( a / 36.0 );
        const double remIn = a - (double)yd * 36.0;
        const int    ft    = (int)( remIn / 12.0 );
        const double rem   = remIn - (double)ft * 12.0;
        if ( rem < 0.005 )
            _snprintf( buf, (size_t)bufSize, "%s%d yd %d ft", sign, yd, ft );
        else
        {
            char value[64];
            _snprintf( buf, (size_t)bufSize, "%s%d yd %d ft %s in", sign, yd, ft,
                       KiwiFmt_Num( value, sizeof( value ), rem, 4 ) );
        }
    }
    buf[bufSize - 1] = '\0';
    return buf;
}

// Grid spacing
float KiwiUnits_GridSpacingInches()
{
    Load();
    return s_gridInches;
}

void KiwiUnits_SetGridSpacingInches( float inches )
{
    Load();
    // The negated comparison also rejects NaN.
    if ( !( inches >= KUNITS_MIN ) )
    {
        Sys_Printf( "Units: grid spacing %g in refused (minimum is %g) — still %g in.\n",
                    (double)inches, (double)KUNITS_MIN, (double)s_gridInches );
        return;
    }
    if ( inches == s_gridInches )
        return;
    s_gridInches = inches;
    WriteFloat( KUX_GRID_ENTRY, s_gridInches );
}

float KiwiUnits_GridSpacingWorld()
{
    return Units_FromDisplay( KiwiUnits_GridSpacingInches() );
}

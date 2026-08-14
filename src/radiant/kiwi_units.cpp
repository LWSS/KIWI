#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_units.cpp — RADIANT_UX_DESIGN §17 implementation.  See kiwi_units.h.
//
// Persistence goes through the existing settings helpers (radiant_registry.h,
// kiwi_radiant.ini next to the exe).  NOTHING is added to prefData_t: the ported
// prefs struct is IDA-shaped and stays that way.  Floats round-trip as strings so
// arbitrary (non power-of-two, fractional) spacings survive a restart.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "kiwi_units.h"
#include "radiant_registry.h"

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

namespace
{
    const char *KUX_SECTION = "KiwiUX";

    // Floor for both prefs.  A zero/denormal spacing would make the grid emitter
    // and KiwiGrid_Snap divide by ~0; a zero unit factor would break the display
    // boundary in both directions.
    const float KUNITS_MIN = 0.0001f;

    float s_perInch      = 1.0f;
    // ── KIWI-UX (ROUND U): THE DEFAULT GRID IS ONE INCH ─────────────────────
    // USER DIRECTIVE, verbatim: "Make the default grid 1 inch."
    //
    // THE REGISTRY ENTRY IS RENAMED with the change (KUX_GRID_ENTRY below), for
    // the FlySpeedScale reason (kiwi_camera.cpp:102-108): the spacing is PERSISTED
    // on every change, so every existing session already has a "10" written under
    // the old name and a bare default change would be invisible to everyone who
    // has ever run the editor.  Renaming unpins those stored 10s exactly once;
    // anyone who genuinely wants 10 sets it again and it persists under the new
    // name.  The OLD entry is deliberately NOT read as a fallback — reading it
    // would restore the very value the rename exists to drop.
    float s_gridInches   = 1.0f;
    bool  s_loaded       = false;

    // v2: the default moved 10 in -> 1 in (ROUND U).  See the note above.
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
        sprintf( buf, "%.6g", (double)v );
        Radiant_ProfileSetString( KUX_SECTION, entry, buf );
    }

    void Load()
    {
        if ( s_loaded )
            return;
        s_loaded     = true;                 // set FIRST: the readers below are re-entrant-safe
        s_perInch    = ReadFloat( "UnitsPerInch", 1.0f );
        s_gridInches = ReadFloat( KUX_GRID_ENTRY, 1.0f );
    }
}

// ─── units ───────────────────────────────────────────────────────────────────
float KiwiUnits_PerInch()
{
    Load();
    return s_perInch;
}

void KiwiUnits_SetPerInch( float unitsPerInch )
{
    Load();
    if ( !( unitsPerInch >= KUNITS_MIN ) )
        return;
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
    // USER DIRECTIVE (shakeout A follow-up): tier the readout for big values —
    // plain inches below 120 in (10 ft), feet+inches below 300 ft, and
    // yards+feet+inches from there.  %.6g / %.4g keep integral values integral
    // ("128 in") and still show fractions; sub-0.005in remainders are dropped so
    // "40 ft" never prints as "40 ft 0 in".
    const double in   = (double)Units_ToDisplay( world );
    const char  *sign = ( in < 0.0 ) ? "-" : "";
    const double a    = ( in < 0.0 ) ? -in : in;

    if ( a < 120.0 )
    {
        _snprintf( buf, (size_t)bufSize, "%.6g in", in );
    }
    else if ( a < 3600.0 )                       // < 300 ft
    {
        const int    ft  = (int)( a / 12.0 );
        const double rem = a - (double)ft * 12.0;
        if ( rem < 0.005 )
            _snprintf( buf, (size_t)bufSize, "%s%d ft", sign, ft );
        else
            _snprintf( buf, (size_t)bufSize, "%s%d ft %.4g in", sign, ft, rem );
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
            _snprintf( buf, (size_t)bufSize, "%s%d yd %d ft %.4g in", sign, yd, ft, rem );
    }
    buf[bufSize - 1] = '\0';
    return buf;
}

// ─── grid spacing ────────────────────────────────────────────────────────────
float KiwiUnits_GridSpacingInches()
{
    Load();
    return s_gridInches;
}

void KiwiUnits_SetGridSpacingInches( float inches )
{
    Load();
    if ( !( inches >= KUNITS_MIN ) )
        return;
    if ( inches == s_gridInches )
        return;
    s_gridInches = inches;
    WriteFloat( KUX_GRID_ENTRY, s_gridInches );
}

float KiwiUnits_GridSpacingWorld()
{
    return Units_FromDisplay( KiwiUnits_GridSpacingInches() );
}

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Display-facing values are inches; geometry, winding math, planes, and .map data
// remain in raw game units. Units_ToDisplay/Units_FromDisplay are the boundary.
// CoD defaults to one world unit per inch.
//
// Modern grid spacing accepts fractional inches and is independent of the legacy
// g_qeglobals.d_gridsize/grid_sizes[] presets used by ported snap sites.
// Its versioned key intentionally ignores persisted 10-inch values from the old default.

// World units per displayed inch; minimum 0.0001, default 1:1.
float KiwiUnits_PerInch();
void  KiwiUnits_SetPerInch( float unitsPerInch );

// Convert only at the raw-world/display-inch boundary.
float Units_ToDisplay  ( float world );
float Units_FromDisplay( float display );

// "128 in" style formatting of a raw world value.  Returns `buf`.
const char *KiwiUnits_Format( char *buf, int bufSize, float world );

// Modern grid spacing in displayed inches; minimum 0.0001, fractions allowed.
float KiwiUnits_GridSpacingInches();
void  KiwiUnits_SetGridSpacingInches( float inches );

// Convert grid spacing back to raw world units for geometry.
float KiwiUnits_GridSpacingWorld();

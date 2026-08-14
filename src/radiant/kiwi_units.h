#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_units.h — RADIANT_UX_DESIGN §17: the display-units layer.
//
// "Inches everywhere the user sees a number."  Internal geometry, winding math,
// plane equations and .map serialization stay in RAW GAME UNITS — this header is
// the ONLY conversion boundary, and nothing here may ever be persisted back into
// map data.  One constant (`UNITS_PER_INCH`, a pref) is applied at the
// display/entry edge through the Units_ToDisplay / Units_FromDisplay pair used by
// the HUD, numeric entry, grid labels and inspector readouts.
//
// CoD's own convention is 1 unit == 1 inch, hence the 1.0 default: the pref only
// exists so a project that disagrees can say so in one place.
//
// The modern grid spacing lives here too (spec §17: ANY positive value —
// deliberately not restricted to powers of two).  ROUND U: the DEFAULT is now
// 1 INCH, per the directive "Make the default grid 1 inch"; §17 shipped with 10.
// The registry entry was renamed to "GridSpacingInches2" with the change, because
// the spacing is persisted on every edit and every existing session would
// otherwise keep restoring its stored 10 — the FlySpeedScale precedent
// (kiwi_camera.cpp:102).  It is a
// SEPARATE quantity from the legacy `g_qeglobals.d_gridsize` index into
// `grid_sizes[]`; the classic power-of-two presets and every ported snap site
// that assumes them are untouched by this file.
// ─────────────────────────────────────────────────────────────────────────────

// World units per displayed inch.  Always > 0.
float KiwiUnits_PerInch();
void  KiwiUnits_SetPerInch( float unitsPerInch );

// THE conversion boundary.  raw game units <-> displayed inches.
float Units_ToDisplay  ( float world );
float Units_FromDisplay( float display );

// "128 in" style formatting of a raw world value.  Returns `buf`.
const char *KiwiUnits_Format( char *buf, int bufSize, float world );

// Modern grid spacing, held in INCHES (spec §17).  Always > 0.
float KiwiUnits_GridSpacingInches();
void  KiwiUnits_SetGridSpacingInches( float inches );

// The same spacing expressed in raw world units — what geometry code must use.
float KiwiUnits_GridSpacingWorld();

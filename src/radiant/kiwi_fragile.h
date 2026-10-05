#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Fragile corners.  KIWI-cod4map writes a "fragile corner" .errlog entry wherever a corner is
// too flat for retail cod4map (which turns it into a void strip; KIWI-cod4map compensates).
// Each entry names the face to act on and what to do: delete a shallow-crease face, line a
// face up with a neighbouring brush, or (no automatic fix) nudge the brush.  The Build panel
// lists them with Show / Fix / Fix all (undoable), and the faces flash in every view with the
// engine's out-of-bounds blink.  Build passes -fragileStop, so cod4map quits right after its
// BSP when it finds any (exit KIWI_FRAGILE_STOP_EXIT_CODE).

// cod4map.h FRAGILE_STOP_EXIT_CODE / FRAGILE_CHECK_LEAKED_EXIT_CODE - keep them equal.
#define KIWI_FRAGILE_STOP_EXIT_CODE         70
#define KIWI_FRAGILE_CHECK_LEAKED_EXIT_CODE 71

// Live check: once an edit settles, Tick writes the map to a snapshot beside the editor
// (kiwi_cache\fragile) and runs KIWI-cod4map -fragileCheck on it in the background - the
// build's own BSP and test, stopped right after the report - then loads what it found.
// A build cancels a running check (its own BSP answers the same question).
void KiwiFragile_LiveCancel();

// Replace the list with the "fragile corner" entries of `errlogPath` whose map token is
// `mapPath`.  Entries for other files (prefabs: prefab-local coordinates) only count in
// `skipped`; they stay in the error list.  Returns the count.
int  KiwiFragile_Load( const char *errlogPath, const char *mapPath, int *skipped );
void KiwiFragile_Clear();
int  KiwiFragile_Count();

// Once per pump tick: drives the live check, drops the list when another map is open, and
// marks the 2D views dirty when the blink flips.
void KiwiFragile_Tick();

// Camera tail (always on top) and 2D overlay tail.
void KiwiFragile_DrawCamera();
void KiwiFragile_DrawXY( int viewType, float scale );

// Build panel section: the live-check toggle and status, then the issue list with Show / Fix,
// plus Fix all and Clear.  With
// `offerBuildAnyway` (the last build stopped at these) it also shows "Build anyway" and
// returns true when it is clicked.
bool KiwiFragile_DrawBuildRow( bool offerBuildAnyway );

// Clean Up Precision (KIWI_CMD_CLEAN_PRECISION): square brush faces within 1e-4 rad of an axis
// onto it and flatten patches within 0.02 of a plane (axial when near one) - the selection,
// else the whole map, one undo record.  Repairs what the old 0.5 rounding / rotation
// residuals left behind (planes cod4map keeps apart or cannot square onto an axis).
void KiwiFragile_CleanPrecision();
void KiwiFragile_RegisterCommands();
bool KiwiFragile_DispatchInstant( unsigned int cmdId );

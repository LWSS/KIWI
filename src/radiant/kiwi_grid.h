#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// World axes and grey ground lattice; each overlay has its own UI switch.
// Coordinates and distances in this file are raw world units unless noted.
//
// Display spacing is baseSpacing * 2^k, but snapping always uses baseSpacing.
// A single Schmitt-latched LOD keeps compressed cells legible at 9 pixels;
// 22/9 > 2 gives a stable dead band across a one-step double or halve.
//
// In roll-free ortho, world-per-pixel is constant and ground compression is
// uniform. The two line-family pitches are spacing / wpp and
// spacing * abs(vpn.z) / wpp; the latter drives LOD. The abs(vpn.z) floor is
// KGRID_EDGE_FULL so coarsening hands off to the edge fade at one threshold.
//
// The exact ortho footprint uses ground-projected screen-up
// g = vup - vpn * (vup.z / vpn.z):
//   need[a] = halfW * abs(vright[a]) + halfH * abs(g[a]), a = X,Y.
// It is centered on the screen-centre ground hit. A per-axis cell cap pulls a
// clamped centre toward the camera's ground projection without changing phase.
// The ortho far ring uses the same footprint and a Schmitt latch.
//
// Perspective is opt-in session state. It evaluates world-per-pixel at the orbit
// pivot, shares the LOD/emitter path, centers the window on pivot XY, and limits
// reach by both the cell cap and the near-tier pixel floor. It has no far ring.
//
// Segment budget: near grid <= 482, axes <= 6, far majors <= 50; total <= 538.
#define KGRID_PX_MIN         9.0f
#define KGRID_PX_REFINE      22.0f
// Keep this tied to EdgeFade's full-strength threshold; the regimes must not drift.
#define KGRID_ORTHO_MIN_SIN  KGRID_EDGE_FULL

// One width-1 batch. 1024 leaves headroom above the proven 538-segment maximum.
// KiwiLines auto-flushes its smaller staging array, so this cap may exceed it.
#define KGRID_MAX_SEGMENTS   1024       // width-1 batch: near grid + far ring + axes

// 3 means 8x spacing/reach; majors-only keeps the far pass near 50 segments.
#define KGRID_FAR_STEP       3
// 1.30/1.05 Schmitt thresholds prevent blinking at the reach boundary.
#define KGRID_FAR_ON         1.30f
#define KGRID_FAR_OFF        1.05f
// Dim context only; the near lattice must remain visually dominant.
#define KGRID_FAR_MUL        0.62f

// Suppress the ortho far ring near the horizon or when too few majors remain to
// read as a grid. Its angle floor follows EdgeFade's ramp, and six lines avoids
// isolated diagonals. These gates never affect the near lattice or axes.
#define KGRID_FAR_MIN_SIN    ( 2.0f * KGRID_EDGE_FULL )
#define KGRID_FAR_MIN_LINES  6

// Per-axis half-width in cells; 2*120+1 bounds each family to 241 lines.
#define KGRID_HALF_CELLS     120

// Bounds both the selected tier and PickLodOrtho's guard loop.
#define KGRID_LOD_MAX          20       // 2^20 * base, beyond practical map scales

// Near and far are brightness tiers, not distance bands; ortho distance bands
// would paint a camera-centered bullseye.
//
// At edge-on pitch, ground-line separation collapses with abs(vpn.z). Fade the
// lattice from 0.08 (~4.6 deg) to 0.03 (~1.7 deg), where 241 lines collapse into
// a few rows; remove 10x-denser minors halfway through. Scale RGB, not alpha:
// $line treats alpha as colour interpolation. Axes remain outside this fade.
#define KGRID_EDGE_OFF       0.03f
#define KGRID_EDGE_FULL      0.08f
#define KGRID_EDGE_MINOR     0.5f

// Finite ruler/axis reach in world units. 524288 matches the ortho depth slab;
// the cell cap still bounds line count, so increasing extent adds no segments.
#define KGRID_MAX_EXTENT     524288.0f      // matches the ortho depth slab

// Cam_Draw hook; emits nothing when both overlay switches are off.
void KiwiGrid_Draw();

// Snap a world point in raw world units. On disabled/unusable spacing, copy input
// through and return false so SnapManager can demote to SNAP_NONE.
bool KiwiGrid_Snap( const float in[3], float out[3] );

// Persistent grid-only snapping preference, default on. Ctrl remains the
// per-gesture override and geometry/face/construction snapping is unaffected.
// Enforcement lives in KiwiGrid_Snap and KiwiSnap_LatticeAxis so callers cannot
// bypass the preference.
bool KiwiGrid_SnapEnabled();
void KiwiGrid_SetSnapEnabled( bool on );

// Perspective lattice is an explicit per-session opt-in, default off and not
// persisted because its near-horizon limitations are a view choice.
// It uses pivot-depth world-per-pixel, the common foreshortened LOD, and a window
// centered on pivot XY. Near the horizon, minors fade first and then all ground
// lines fade; axes remain.
bool KiwiGrid_PerspGrid();
void KiwiGrid_SetPerspGrid( bool on );

// The same switch for ORTHO: default on, persisted ([KiwiUX] OrthoGrid).  The view cube's
// lattice button drives whichever of the two belongs to the current projection.
bool KiwiGrid_OrthoGrid();
void KiwiGrid_SetOrthoGrid( bool on );

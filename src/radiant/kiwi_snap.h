#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "kiwi_pick.h"

// KiwiSnap_Query applies this priority order after KiwiCmd_SnapEngaged enables
// snapping. Construction commands snap by default and Ctrl frees them; transforms
// are free by default and Ctrl engages snapping. An always-engaged context, used
// for pivot placement, ignores Ctrl.
//
// All capture radii below are SCREEN PIXELS unless explicitly marked otherwise.
// Earlier entries win; arms that share a line rank compete by pixel distance.
//
//   0b. SNAP_ENDPOINT      open chain's first point (8 px)
//   1.  SNAP_ENDPOINT      construction anchor (7 px)
//   2.  SNAP_VERTEX        brush corner or patch control point (8 px)
//   3.  SNAP_INTERSECTION  construction-segment crossing (7 px)
//   3b. SNAP_EDGE_MID      construction-segment midpoint or quarter point (7 px)
//   4.  SNAP_EDGE_MID      brush-edge midpoint or quarter point (7 px), over EVERY
//                          edge near the cursor, not only the one the edge Pick names
//                          (SNAP_EDGE_QUARTER for the quarters).  An edge's points are
//                          its LINE's: collinear brush edges that overlap or touch merge
//                          into one (Plasticity's topological edge of a union)
//   4b. SNAP_FACE_CENTER   brush-face vertex-average center (6 px)
//   4c/5. LINE             axis guide (10-30 px), brush edge (6 px), or
//                          construction segment (8 px); closest pixel distance wins.
//                          Axis wins an exact tie; a brush edge wins an exact tie
//                          with a construction line.
//   5b. SNAP_ANGLE         free-tool 15-degree capture band
//   6.  SNAP_FACE          unsnapped surface hit for non-planar placement
//   7.  SNAP_ANGLE         planar placer's hard 15-degree quantization
//   8.  SNAP_CPLANE        planar placer's in-plane grid
//   9.  SNAP_GRID          ground grid, then the view-aligned fallback grid
//
// Point snaps outrank line snaps, which outrank the area hit and grid. Construction
// intersections use 3D closest approach within KCON_ISECT_DIST (0.25 WORLD UNITS);
// adjacent segments, parallel/collinear pairs, and shared endpoints are rejected.
// Brush-edge intersections are intentionally unsupported until they have an
// occlusion policy.
// Free-tool angle capture allows 6 SCREEN PIXELS of lateral miss, floored at
// 1 degree and capped at 0.22 of one 15-degree step.
//
// A surface hit occludes point and real-line candidates farther down the ray by
// more than KSNAP_OCCLUDE_SLOP_PX at the candidate's depth. The open-chain target
// and axis guides are deliberately exempt. Planar placers may extend the reference
// depth to their own plane.
//
// With snapping disengaged, SNAP_NONE is a valid raw answer. Raw resolution is the
// planar placer's plane, else the surface hit, else ray intersection with Z=0, else
// a view-aligned plane through the tool anchor (or KSNAP_FALLBACK_DIST down the ray).
//
// Construction geometry is outside Pick(), so it is scanned directly. Intersection
// pairing is capped by KSNAP_MAX_CSEGS. Brush/patch candidates come from Pick(), and
// pickFlags is forwarded so a live transform can exclude selected geometry.
// One query makes at most four Pick() calls.
//
// Grid quantization uses KiwiGrid_Snap and KiwiUnits_GridSpacingWorld, not the legacy
// d_gridsize. The spacing is in WORLD UNITS (displayed as inches by the UI).

// Per-class screen-space capture radii.
#define KSNAP_R_CON_POINT     7.0f               // construction point targets, pixels
#define KSNAP_R_VERTEX        PICK_VERT_PIXELS   // brush/patch points, 8 pixels
#define KSNAP_R_EDGE_MID      7.0f               // brush midpoint, pixels
#define KSNAP_R_FACE_CENTER   6.0f               // most-derived point target, pixels
#define KSNAP_R_CON_SEG       8.0f               // construction snap radius, pixels
// Construction selection remains KCON_LINE_PIXELS (10 px); selection and snapping
// intentionally use different slack.

// Occlusion slack in SCREEN PIXELS, converted at each candidate's depth.
#define KSNAP_OCCLUDE_SLOP_PX 6.0f

// WORLD-UNIT distance down the ray when neither geometry nor a plane is available.
#define KSNAP_FALLBACK_DIST 512.0f

// Maximum construction segments paired by one intersection query.
#define KSNAP_MAX_CSEGS     128

enum snap_type_t
{
    SNAP_NONE = 0,
    SNAP_GRID,
    SNAP_VERTEX,         // brush winding corner or patch control point
    SNAP_EDGE_MID,       // brush or construction segment midpoint
    SNAP_EDGE,           // brush edge or construction segment
    SNAP_FACE,
    SNAP_FACE_CENTER,
    SNAP_ENDPOINT,       // construction anchor or open-chain start
    SNAP_INTERSECTION,   // construction segment x segment
    SNAP_AXIS,           // axis line through the tool's last point
    SNAP_CPLANE,         // planar placement only
    SNAP_ANGLE,
    SNAP_EDGE_QUARTER,   // KIWI: brush or construction segment quarter point
    SNAP_TYPE_COUNT
};

struct snap_result_t
{
    bool        valid       = false;
    snap_type_t type        = SNAP_NONE;
    float       position[3] = { 0.0f, 0.0f, 0.0f };
    sel_item_t  source;             // null for grid and construction targets

    // Plane used to resolve this frame's point. Geometry snaps inherit that plane,
    // allowing bearing readouts to use the same coordinate space as placement.
    bool        havePlane      = false;
    float       planeNormal[3] = { 0.0f, 0.0f, 1.0f };   // unit vector
};

inline bool KiwiSnap_Active( const snap_result_t &s )
{
    return s.valid && s.type != SNAP_NONE;
}

// True only for targets that name existing brush, patch, or construction geometry.
// Grid, axes, angle locks, and construction-plane placement are excluded.
inline bool KiwiSnap_IsGeometry( snap_type_t t )
{
    return t == SNAP_VERTEX || t == SNAP_EDGE_MID || t == SNAP_EDGE
        || t == SNAP_FACE   || t == SNAP_FACE_CENTER || t == SNAP_ENDPOINT
        || t == SNAP_INTERSECTION || t == SNAP_EDGE_QUARTER;
}

// Resolves a geometry snap to signed WORLD-UNIT depth along a unit axis from ref.
// SNAP_FACE intersects the target face plane with the axis; point targets project
// their exact position. A face with |normal dot axis| below 0.05 is effectively
// edge-on and would produce an unstable, very distant result.
#define KSNAP_AXIS_PARALLEL 0.05f
bool KiwiSnap_AxisDepth( const snap_result_t &r, const float *ref,
                         const float *axis, float *outDist );

// Lattice capture uses WORLD-UNIT grid spacing. A world-aligned gesture quantizes
// the absolute world coordinate; a slanted gesture quantizes its signed distance.
// HARD mode always picks a cell. SOFT mode captures only inside a 5 SCREEN-PIXEL
// band, converted at the gesture depth and capped at 0.22 of a cell.
//
// Every tenth cell is a major line, matching kiwi_grid.cpp. A major candidate wins
// within 1.5 times the active band; HARD mode uses a half-cell band, so its major
// preference reaches 0.75 cell without making neighboring cells unreachable.
#define KSNAP_LIGHT_BAND_PIX  5.0f
#define KSNAP_LIGHT_MAX_FRAC  0.22f
#define KSNAP_MAJOR_STRIDE    10
#define KSNAP_MAJOR_BAND_MUL  1.5f
float KiwiSnap_LatticeAxis( float d, const float *ref, const float *axis,
                            bool hard, bool *outMajor );

// Applies an area target only within 14 SCREEN PIXELS of the cursor-mapped scalar,
// converted to WORLD UNITS at the point supplied by at. Named targets remain exact.
#define KSNAP_AREA_BAND_PIX  14.0f
float KiwiSnap_AreaMagnet( float cur, float target, const float *at );

// imgX/imgY use camera-image coordinates with a top-left origin. pickFlags is
// forwarded to every Pick() call. Returns out->valid.
bool KiwiSnap_Query( const ray_t &ray, int imgX, int imgY, snap_result_t *out,
                     unsigned pickFlags = PICKF_NONE );

// Shared preview/placement entry point: ranked snap, planar placement when required,
// surface hit, ground grid, then view-aligned fallback.
bool KiwiSnap_ResolvePoint( const ray_t &ray, int imgX, int imgY, float out[3] );

// Short marker label. "off" is SNAP_NONE: snapping was not engaged, not a failure.
const char *KiwiSnap_TypeName( snap_type_t t );

// Emits into the caller's open kiwi_lines batch; no-op when invalid or hidden.
void KiwiSnap_EmitMarker( const snap_result_t &r );

// Draws advertised snap points while a compatible command is active and snapping is
// engaged: the hovered face's corners and centre, and black nubs on the midpoints and
// quarter points of every edge near the cursor. It owns its kiwi_lines batch.
void KiwiSnap_DrawFaceAccents();
// Selected/hovered cylinder centres and measured axis; screen-space camera overlay.
void KiwiSnap_DrawCylinderOverlay( float imgMinX, float imgMinY, float imgW, float imgH );

// Emits one tool-owned spot into the current kiwi_lines batch. pixRadius is in
// SCREEN PIXELS at p's depth; solid chooses a filled disc instead of a ring.
void  KiwiSnap_EmitSpot( const float *p, float pixRadius, bool solid );
float KiwiSnap_AccentPixels();
float KiwiSnap_RingPixels();

// Draws the live line-rank axis candidate through the tool anchor. Free tools use
// world X/Y/Z plus capped extension directions from incident construction/brush
// edges; planar placers use their U/V/N basis. Candidates compete with real edges
// by SCREEN-PIXEL distance. Capture ramps from 10 px to 30 px as the camera flattens;
// projections shorter than 8 px are rejected. The dashed guide spans about 420 px
// each way at the anchor depth and owns its kiwi_lines batch.
void KiwiSnap_DrawAxisGuides();

// Draws the cursor label during the ImGui frame. Positions are formatted in inches.
void KiwiSnap_DrawLabel( const snap_result_t &r, float imgMinX, float imgMinY,
                         float imgW, float imgH );

// Marker/label visibility setting; it does not change snapping behavior.
bool KiwiSnap_ShowMarkers();
void KiwiSnap_SetShowMarkers( bool on );

// KIWI (2026-09-16): unit direction of the MOST RECENT KiwiSnap_Query's edge / axis /
// construction-segment answer (the same vector its along-line marker tick uses). False
// when the last snap named no line (a vertex, face, grid, or nothing). One query per
// MouseMove, so this belongs to the snap just handed to the caller.
bool KiwiSnap_LastEdgeDir( float out[3] );

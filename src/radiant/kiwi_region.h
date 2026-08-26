#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Regions are derived, not stored, from closed objects, welded open-object cycles,
// and planar-arrangement cells.  Every point must fit a derived plane; degree-3+
// endpoint nodes are ambiguous and rejected rather than guessed.
// Endpoint welding and plane bands are grid-aware world distances.  Arrangement
// also finds bounded cells at mid-span crossings; all routes share acceptance.
// Holes are unsupported: nested closed loops remain separate regions.
// The engine fill follows the ported selected-face and 3D-marquee triangle path.
// 0x408106
// 0x40CC50
// A depthless ImGui overlay is retained as the guaranteed-visible fallback.
// Polygon helpers consume CCW plane-space loops and use ear clipping followed by
// Hertel-Mehlhorn convex merging.

#include <vector>

#include "kiwi_construct.h"

struct ray_t;                       // kiwi_pick.h

#define KREG_JOIN_DIST   0.5f       // world-unit floor for 3D endpoint chaining

// Snapped grid nodes are at least one grid step apart, so a quarter-step tolerance
// cannot merge distinct snapped endpoints.  Clamp both weld and plane band between
// their legacy 0.5-unit floors and a 16-unit ceiling for unsnapped coarse-grid work.
#define KREG_TOL_MAX     16.0f      // world units — the ceiling on both of the above

// Current endpoint weld; chaining must not read KREG_JOIN_DIST directly.
float KiwiRegion_WeldDist();

// Grid scale does not bound tessellated arc edges, so limit it toward 0.4 of the
// shortest edge above KREG_JOIN_DIST.  The legacy 0.5 floor still wins below a
// 1.25-unit edge; such nondegenerate edges remain longer than the weld.
#define KREG_WELD_EDGE_FRAC  0.4f

float KiwiRegion_WeldFor( float finestEdge );

// Return the shortest consecutive edge above KREG_JOIN_DIST, or zero for no bound.
// Stride is 2 for plane space or 3 for world space; closed includes the wrap edge.
// ArrangeWeld scans independent segments instead and therefore cannot use this.
float KiwiRegion_FinestEdge( const float *pts, int count, int stride, bool closed );

// Current grid-aware point-to-plane membership band.
float KiwiRegion_PlaneBand();

// Report the nearest visible open-end gap, throttled once per store generation.
// `force` also prints the no-gap answer for explicit diagnostics.
void KiwiRegion_ReportGaps( bool force );
#define KREG_MIN_AREA    1.0f       // square world units — below this it is a scratch

// A derived loop can combine several tessellated objects, so its cap is distinct
// from KCON_SEGS_MAX.  128 admits two fully tessellated round boundaries and must
// remain aligned with KEXT_MAX_PROFILE to keep every filled region extrudable.
#define KREG_MAX_LOOP    128        // vertices per derived region loop

// Count the new-region flash in drawn frames so idle time cannot consume it unseen.
#define KREG_FLASH_FRAMES 60

// Lift coplanar fills toward the viewer along the oriented region-plane normal.
// Match the 0.5-unit face-fill displacement; a fan needs more margin than an outline.
#define KREG_FILL_NUDGE   0.50f     // world units along viewer-oriented plane normal

struct kregion_t
{
    kconPlane_t        plane;
    std::vector<float> pts;         // plane-space loop, CCW, first point NOT repeated
    int                sourceObject = -1;   // >= 0 when one closed object IS the region
    // Carried across re-derives by the world-space match key below.
    int                flash = 0;
    float              centroid[3] = { 0.0f, 0.0f, 0.0f };   // world, the match key
};

// The current regions.  Re-derived when the store generation moved; cheap to
// call every frame.
const std::vector<kregion_t> &KiwiRegion_All();

// Force a re-derive (the store calls this on a change; callers rarely need it).
void KiwiRegion_Invalidate();

// The region under a cursor ray — nearest plane hit whose in-plane point is
// inside the loop.  -1 for none.
int KiwiRegion_PickAt( const ray_t &ray );

// The DISTANCE from the ray origin to that hit, for the click arbitration in
// kiwi_boxselect.cpp: a region only wins a click when no brush face is closer.
// False when the index is not a region or the ray misses its plane.
bool KiwiRegion_HitDistance( const ray_t &ray, int index, float *outDist );

// Region selection is KIWI-owned because derived regions cannot enter selection_t.
// Store a world-space match key per member: derived indices change on every rebuild,
// while vanished keys simply stop resolving.  Select replaces the set; SelectedIndex
// returns its first still-live member for single-target callers.
void KiwiRegion_Select( int index );        // -1 clears; REPLACES the set
void KiwiRegion_ToggleSelect( int index );  // Shift+click: in if out, out if in
void KiwiRegion_ClearSelection();
int  KiwiRegion_SelectedIndex();            // -1 = nothing selected / it is gone
bool KiwiRegion_HasSelection();
int  KiwiRegion_SelectedCount();            // members that still resolve
int  KiwiRegion_SelectedAt( int which );    // 0..count-1 -> live region index, or -1
bool KiwiRegion_IsSelected( int index );

// Engine-route fill and the sole once-per-drawn-frame owner of flash aging.
void KiwiRegion_DrawFills( int highlightIndex );

// Guaranteed-visible ImGui fallback, called before the other camera HUD elements.
// Its rectangle is in ImGui screen coordinates.  The deliberate depthless overlay
// shows through occluding world geometry; the engine route remains available.
// This path is read-only and must never age flash counters a second time.
void KiwiRegion_DrawFillsOverlay( float imgMinX, float imgMinY, float imgW, float imgH );

// Shared Join Lines/region walk: weld each object's first and last vertices in 3D
// using KiwiRegion_WeldDist before plane fitting.  Drop self-closing objects, reject
// degree-3+ nodes, and require one walk to consume every edge.  Start at a degree-1
// node when present so open chains retain natural end-to-end order.
struct kchainStep_t
{
    int  object;        // index into the construction store
    bool forward;       // false = walk this object's vertices in reverse
};

bool KiwiRegion_ChainWalk( const int *objects, int count,
                           std::vector<kchainStep_t> *outSteps, bool *outClosed );

// True only when every tessellated point lies within KiwiRegion_PlaneBand;
// exported because grouping and Join ask the same point-to-plane question.
bool KiwiRegion_ObjectOnPlane( const kconObject_t &o, const kconPlane_t &plane );

// Preserve the joined/unjoined invariant by bounded dedupe, collinear removal at
// KVALID_PLANE_DOT, then dedupe again.  Input is an unrepeated plane-space ring;
// callers reject results below three points.  Winding remains an acceptance gate.
void KiwiRegion_SanitizeRing( std::vector<float> &pts );

// 2D polygon toolkit.
// Signed area of a plane-space loop; > 0 == CCW.
float KiwiRegion_SignedArea( const std::vector<float> &pts );

// True when the loop has a self-intersection (non-adjacent segments crossing).
bool KiwiRegion_SelfIntersects( const std::vector<float> &pts );

// True when the loop is convex (all cross products of consecutive edges share a
// sign; exactly-collinear turns are tolerated).
bool KiwiRegion_IsConvex( const std::vector<float> &pts );

// Ear-clip triangulation.  `outTris` gets 3 vertex indices per triangle.  False
// on a degenerate or self-intersecting loop.  The input must be CCW.
bool KiwiRegion_Triangulate( const std::vector<float> &pts, std::vector<int> *outTris );

// Triangulate, then Hertel-Mehlhorn-merge into convex pieces.  Each piece is a
// CCW index list into `pts`.  False when triangulation failed.
bool KiwiRegion_ConvexPieces( const std::vector<float> &pts,
                              std::vector< std::vector<int> > *outPieces );

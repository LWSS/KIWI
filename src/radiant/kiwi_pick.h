#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_pick.h — RADIANT_UX_DESIGN §2: the unified pick API.
//
// ONE entry point over the ported Test_Ray chain (brush / model / prefab / patch)
// plus screen-space vertex and edge picking.  Hover state, selected state, the
// active item and (later) snap markers all render from this API's results.
//
//   faces / objects : ray intersection — Test_Ray, unmodified.
//   edges / vertices: SCREEN-SPACE pixel tolerance.  Candidates are projected with
//                     the exact inverse of CameraCalcRayDir and ranked by pixel
//                     distance (~8 px verts, ~6 px edges).  Never a world-unit
//                     tolerance — that breaks at distance.
//
// Candidate set = the frame's visible brush lists (active_brushes then
// selected_brushes), filtered the same way the ported walker sub_48D460 filters
// them.  No new spatial structure until profiling demands one; budget is one
// hover pick per frame.
//
// ── COORDINATE CONVENTIONS ───────────────────────────────────────────────────
// Everything in this header that says "screen" or "pixel" means the CAMERA RTT
// IMAGE, image-relative, TOP-LEFT origin — the same space
// `ImGuiShell_CameraPaintCursor` reports and the same space the shell hands the
// camera window handlers.  The flip base is `camera_s.height` (kept in step with
// the ImGui dock cell by CamWnd_RenderToRT), NOT GetClientRect on the hidden
// native child.  The bottom-left origin CameraCalcRayDir wants is an internal
// detail of this file.
//
// ── EDGE / VERTEX INDEXING ───────────────────────────────────────────────────
// See kiwi_selection.h DESIGN NOTE 3.  Short form: a brush edge is
// (faceIndex, edgeIndex) = the winding edge `w->p[e] → w->p[(e+1) % numpoints]`
// of `brush->def->faces[faceIndex].w`; a brush vertex is (faceIndex, vertIndex)
// = `w->p[vertIndex]` of that winding; a patch vertex is faceIndex = -1 with
// vertIndex = col * height + row into the control grid.  A world corner shared by
// N faces yields N candidates at the same pixel — the LOWEST faceIndex wins, so
// the result is deterministic frame to frame.
//
// ── PICK FLAGS (Phase 3) ─────────────────────────────────────────────────────
// `pickFlags` defaults to 0, so every Phase-1/2 call site keeps its exact
// behaviour.  PICKF_EXCLUDE_SELECTED is what a live transform needs: while a
// gesture is dragging geometry, the geometry being dragged must not be a snap or
// pick candidate for itself (you would snap the thing you are moving to itself and
// the gesture would lock up).  "Selected" means the union of
//   * the legacy `selected_brushes` sentinel list, and
//   * every brush named by an item in KiwiSel()
// because a FACE selection lives only in the second (a face-selected brush is NOT
// on selected_brushes — kiwi_selection.h DESIGN NOTE 2), and a gesture only ever
// mutates brushes named by the selection.
//
// Note on the area pass: the ported walker sub_48D460 ALREADY skips brushes
// carrying BRUSHFLAG_SELECTED (0x80), i.e. everything on selected_brushes, so
// Test_Ray never returns one of those with or without the flag.  The flag's area
// arm therefore only ever fires for the face-selected case, where it drops the
// surface hit entirely (Test_Ray keeps one nearest hit per call — see the
// dedup/slot logic in sub_48D460 — so there is no "next surface behind it" to
// fall back to).  The snap then falls through to the ground-plane grid.
// ─────────────────────────────────────────────────────────────────────────────

#include "kiwi_selection.h"

#include <math.h>

// Pixel tolerances (spec §2).
#define PICK_VERT_PIXELS 8.0f
#define PICK_EDGE_PIXELS 6.0f

// Pick flags (see the note above).  0 = the Phase-1/2 behaviour, unchanged.
#define PICKF_NONE              0u
#define PICKF_EXCLUDE_SELECTED  1u

struct ray_t
{
    float origin[3] = { 0.0f, 0.0f, 0.0f };
    float dir[3]    = { 0.0f, 0.0f, 0.0f };   // normalised
};

struct pick_result_t
{
    bool       valid      = false;
    sel_item_t item;                          // what was hit, at the filter's granularity
    float      point[3]   = { 0.0f, 0.0f, 0.0f };   // world hit point
    float      screenDist = 0.0f;             // px from the cursor (0 for area hits)
    // ── the SURFACE NORMAL at an area hit ────────────────────────────────────
    // edTrace_t carries it already (qe3.h:335, filled by Brush_Ray for a brush
    // face and by PMESH_51 for a patch), so this costs a copy and no second pick.
    // False for the screen-space vertex/edge passes, which name a point and not a
    // surface, and for any miss.  Consumers must not read `normal` without it.
    bool       haveNormal = false;
    float      normal[3]  = { 0.0f, 0.0f, 1.0f };
};

// ── ray construction ─────────────────────────────────────────────────────────
// Build the pick ray for a cursor position inside the camera RTT image
// (image-relative, TOP-LEFT origin).  Returns false when the camera viewport has
// no valid size yet.  Uses the ported CameraCalcRayDir through its // KIWI-UX
// forwarder — the pick ray is bit-identical to the one Drag_Begin builds.
bool Pick_RayFromImagePos( int imgX, int imgY, ray_t *out );

// Same, for wherever the shell last saw the cursor over the camera image
// (ImGuiShell_CameraPaintCursor).  False when the cursor is not over it.
bool Pick_RayFromCursor( ray_t *out );

// ── projection (exact inverse of CameraCalcRayDir) ───────────────────────────
// World → camera-image pixels, TOP-LEFT origin.  False when the point is at or
// behind the eye plane, or the viewport has no size.
bool Pick_WorldToImage( const float *world, float *outX, float *outY );

// KIWI-UX (CLEANUP, A-12): the ONE spelling of "how far is the cursor from this
// SEGMENT, in camera-image pixels".  `outT` (optional) is the clamped parameter
// along a→b, which every caller needs to carry the screen answer back onto the
// world segment.  Promoted verbatim out of kiwi_pick.cpp's anonymous namespace —
// it was already the canonical copy — and the five open-coded duplicates
// (kiwi_snap ScanConSegments/ScanAxes, kiwi_conselect PickAt, kiwi_trim HoverAt,
// kiwi_split PickLineAt) now call it.  Callers still own the ACCEPTANCE radius
// (PICK_EDGE_PIXELS / KCON_LINE_PIXELS / KCON_CLICK_PIXELS / an axis radius) and
// still decide for themselves what a behind-the-eye endpoint means.
inline float Pick_SegDist2D( float px, float py, float ax, float ay,
                             float bx, float by, float *outT )
{
    const float dx = bx - ax, dy = by - ay;
    const float len2 = dx * dx + dy * dy;
    float t = 0.0f;
    if ( len2 > 1e-6f )
    {
        t = ( ( px - ax ) * dx + ( py - ay ) * dy ) / len2;
        if ( t < 0.0f )      t = 0.0f;
        else if ( t > 1.0f ) t = 1.0f;
    }
    const float ex = px - ( ax + dx * t );
    const float ey = py - ( ay + dy * t );
    if ( outT )
        *outT = t;
    return sqrtf( ex * ex + ey * ey );
}

// KIWI-UX (Phase 1b): the candidate-admission filter this file's screen-space scan
// uses, EXPORTED rather than duplicated so box selection (kiwi_boxselect.cpp) walks
// exactly the same brush set a click pick would.  Mirrors the ported walker
// sub_48D460's rules; see the definition in kiwi_pick.cpp.
bool Pick_BrushPickable( selbrush_t *b );

// The Test_Ray `contents` mask a 3D-camera-viewport pick uses — the same bits
// Drag_Begin computes for viewz == 2 (prefs gates + the 0x1000 camera-view flag),
// minus the light-preview bit.
int Pick_CameraContents();

// ── the pick ─────────────────────────────────────────────────────────────────
// kindMask filters AT PICK TIME (spec §1: modes are a pick-time mask, never a
// post-conversion of an object hit).  Resolution order is point → line → area:
// a vertex within PICK_VERT_PIXELS beats an edge within PICK_EDGE_PIXELS, which
// beats the Test_Ray surface hit.
pick_result_t Pick( const ray_t &ray, sel_mask_t kindMask, unsigned pickFlags = PICKF_NONE );

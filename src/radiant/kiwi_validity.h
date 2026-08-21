#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_validity.h — RADIANT_UX_DESIGN §19: the validity contract, plus the
// planept/control-point BASELINE snapshot every §20/§21/§22 edit rolls back to.
//
// §19 in one line: classic brushes are PLANE-defined and windings are DERIVED, so
// every brush edit is a plane edit followed by rebuild + validation, and an edit
// that fails validation is REJECTED (v1 policy: reject > repair).
//
//     edit planepts  →  Brush_BuildWindings  →  KiwiValid_CheckBrush
//        ok   : keep
//        bad  : KiwiValid_Restore( baseline ) → rebuild → HUD "INVALID", commit
//               disabled while invalid
//
// ── EXACTLY WHICH CHECKS RUN (KiwiValid_CheckBrush, in this order) ───────────
// All of them run AFTER Brush_BuildWindings has rebuilt planes, windings and the
// def's [mins,maxs].  The first failure wins and names itself in `outWhy`.
//
//   V1  NO FACES          faceCount < 4 (a solid needs four half-spaces), or the
//                         face array is missing.
//   V2  DEGENERATE PLANE  |plane.normal| is not ~1 after Face_MakePlane's
//                         normalise, i.e. the 3 planepts became collinear /
//                         coincident and the cross product collapsed.
//   V3  NULL / SHORT / OVERFLOWED WINDING
//                         a face has w == NULL or w->numpoints < 3 (reported as
//                         "face collapsed"), or w->numpoints is past
//                         MAX_POINTS_ON_WINDING (reported as "winding overflow" —
//                         KIWI-UX (CLEANUP, A-38)).  This is the
//                         single most important check: Brush_BuildWindings drops
//                         the winding of any face that the OTHER planes clipped
//                         away entirely, which is exactly what "you pushed this
//                         face past the opposite one" looks like (§20).
//   V4  ZERO-AREA FACE    a face winding whose polygon area is below
//                         KVALID_MIN_FACE_AREA — a winding that survived as a
//                         sliver rather than disappearing outright.
//   V5  DUPLICATE PLANE   two faces with dot(n_i,n_j) > KVALID_PLANE_DOT and
//                         |d_i - d_j| < KVALID_PLANE_DIST: the same half-space
//                         twice, which makes the brush ambiguous to the compiler.
//   V6  OPPOSED / CROSSED PLANES
//                         two faces with dot(n_i,n_j) < -KVALID_PLANE_DOT and
//                         d_i + d_j < KVALID_MIN_THICKNESS.  With outward normals
//                         and the interior at n·p <= d, two opposed planes bound a
//                         slab of thickness (d_i + d_j); <= 0 means the planes
//                         crossed and the solid is inside-out, and a hair above 0
//                         means a zero-thickness brush.
//   V7  BOUNDS SANITY     any axis with mins > maxs (Brush_BuildWindings seeds
//                         mins=+131072 / maxs=-131072 and only expands, so this is
//                         also how "no intersection points at all" reports), or a
//                         span > KVALID_MAX_SPAN (65536, the map bound), or a
//                         coordinate outside ±KVALID_MAX_COORD.
//
// NOT checked in v1, on purpose: full convexity (§19 lists it, but
// Brush_BuildWindings only ever emits the convex hull of the half-space
// intersection, so a non-convex result is unrepresentable — the failure mode
// shows up as V3/V4/V6 instead) and "no tiny edges" (it would reject legitimate
// detail brushes; revisit when CSG repair lands).
//
// ── V8, THE CLOSURE TEST (ROUND AA, ITEM 4) — AND WHY V1..V7 NEEDED IT ───────
// USER REPORT, verbatim: "(see pic) match face should support this operation.
// You still can't delete a chamfer that was made on a brush."
//
// Both halves of that report are "take a half-space AWAY from a brush", and the
// question a plane-defined brush cannot answer from V1..V7 alone is whether the
// REMAINING half-spaces still enclose a finite cell.  V1..V7 are all read off
// what Brush_BuildWindings produced, and Brush_BuildWindings has NO world box:
// CM_ForEachBrushPlaneIntersection (brush.cpp:1303) collects the triple-plane
// intersection points that lie inside every other half-space, and def->[mins,maxs]
// is the box of THOSE POINTS (brush.cpp:1459-1463).  An OPEN cell still has
// vertices — a box with its lid removed still has its four floor corners — so its
// point set is finite, its bounds are finite, and V7 passes on a solid that runs
// away to infinity.
//
// In practice V3 catches most opens (the side faces of a lidless box keep only
// TWO intersection points each, so their windings come back NULL), but "most" is
// not a decision procedure and this is the one operation whose whole job is to
// remove a bounding plane.  So V8 asks the question directly:
//
//     Give the brush a PROBE BOX — its own bounds grown by KVALID_CLOSE_MARGIN —
//     and rebuild every face winding against it with the ported
//     Brush_MakeFaceWinding (brush.cpp:4693, 0x471260), which is exactly
//     "Winding_BaseForPlane over def->[mins,maxs]±1, then clip behind every other
//     face plane".  A CLOSED brush's faces are its real faces and lie inside its
//     own bounds, i.e. a whole margin away from the probe box.  An OPEN cell's
//     faces run out to the probe box and get clipped BY it, so at least one
//     winding point lands on the box surface.  That point IS the leak.
//
// It is a SEPARATE entry point rather than a V8 inside KiwiValid_CheckBrush on
// purpose: every §20/§21/§22 drag calls CheckBrush per frame, none of them can
// remove a half-space, and none of them should start paying faceCount² winding
// clips for a question they cannot raise.  The two verbs that CAN raise it —
// Remove Face and Match Face's planarize-away — call it themselves.
//
// ── WHY A BASELINE AND NOT JUST UNDO ─────────────────────────────────────────
// The undo bracket restores at CANCEL time.  A validity rejection happens
// MID-GESTURE, dozens of times per drag, and must not touch the undo stack at
// all — so the commands keep a cheap in-memory copy of exactly what they mutate
// (planepts for brushes, ctrl xyz for patches) and roll back to that.  The
// baseline is also what makes "apply from baseline" possible: an edit re-derives
// geometry from the ORIGINAL planepts every frame instead of accumulating
// increments, so a drag that goes out and comes back lands exactly where it
// started (RADIANT_UX_DESIGN §13 / the Phase-3 design decisions).
// ─────────────────────────────────────────────────────────────────────────────

#include <vector>

struct brush_t;
struct patchMesh_t;

// V4..V7 thresholds.  Deliberately loose: this gate exists to catch geometry that
// is BROKEN, not geometry that is merely small.
#define KVALID_MIN_FACE_AREA   0.1f       // square world units
#define KVALID_PLANE_DOT       0.999f     // |cos| above this = parallel planes
#define KVALID_PLANE_DIST      0.01f      // world units, duplicate-plane test
#define KVALID_MIN_THICKNESS   0.01f      // world units, opposed-plane slab
#define KVALID_MAX_SPAN        65536.0f   // spec §19 "map bound"
#define KVALID_MAX_COORD       131072.0f  // the editor's own sentinel box

// ── KIWI-UX (CLEANUP, B-11): THE PATCH CONTROL-GRID WIDTH BOUND, NAMED ──────
// Patch_GenericMesh refuses a control-grid width outside 3..15 (pmesh.cpp:1550).
// That ONE format constraint was spelled as the bare literal 15 in five places
// across kiwi_loft.cpp and kiwi_patchfillet.cpp — one of which SIZED A STACK
// ARRAY (`float top[15][3], bot[15][3]`) — and encoded a sixth and seventh time
// as the derived KLOFT_MAX_CURVE_SEGS (7) and KPF_MAX_SPANS (7), each with a
// comment doing the 2*7+1 arithmetic by hand.  Named here, in the header that
// owns "is this geometry acceptable", and static_asserted against those two
// derived constants at their use sites so they cannot drift apart.
//
// NOT the same number as KIWI_PATCH_MAX_DIM (16, kiwi_selection.h): that one is
// `patchMesh_t::ctrl`'s ARRAY EXTENT, i.e. what can be indexed; this one is what
// the mesh BUILDER accepts.  15 <= 16 is the invariant between them.
#define KPATCH_MIN_WIDTH       3
#define KPATCH_MAX_WIDTH       15

// V8 (ROUND AA, ITEM 4).  The probe box is the brush's own bounds grown by
// MARGIN; a winding point within TOUCH of that box's surface means the face was
// clipped BY the box rather than by the brush, i.e. the solid leaks.  MARGIN is
// deliberately huge next to TOUCH: a closed brush's faces lie inside its bounds,
// so they clear the probe box by a whole margin and no epsilon choice can turn a
// good brush into a leak.
#define KVALID_CLOSE_MARGIN    4096.0f    // world units the probe box adds per side
#define KVALID_CLOSE_TOUCH     1.0f       // world units, "this point is ON the box"

// ── the baseline ─────────────────────────────────────────────────────────────
// One entry per brush def the gesture will touch.  EXACTLY ONE of the two arrays
// is filled:
//   * a PATCH gets `ctrl` (width*height*3 floats, in the SAME linear order
//     kiwi_selection.h DESIGN NOTE 3 uses: index = col * height + row) and
//     faceCount 0.  Its brush def is a bounding box that Patch_Rebuild →
//     Brush_RebuildBrush FREES and re-creates from the recomputed bounds, so its
//     planepts are not a stable thing to snapshot — the control grid IS the
//     geometry.
//   * anything else gets `planepts` (faceCount*9 floats in face order) and
//     patchW 0.
struct kiwiBaseBrush_t
{
    brush_t           *def       = nullptr;
    int                faceCount = 0;
    int                patchW    = 0;
    int                patchH    = 0;
    std::vector<float> planepts;
    std::vector<float> ctrl;
};

// Snapshot `def` (planepts and, when it is a patch, the control grid).  Safe to
// call on a def with no faces; returns false only for a NULL def.
bool KiwiValid_Snapshot( brush_t *def, kiwiBaseBrush_t *out );

// Copy the baseline planepts/ctrl back into the def.  Does NOT rebuild — the
// caller decides whether it needs Brush_BuildWindings (brushes) or Patch_Rebuild
// (patches), because a caller that is about to write a fresh edit on top would
// otherwise pay for two rebuilds per frame.  False when the def's face count or
// patch dimensions changed under us (which means the snapshot no longer
// describes it and the caller must abandon the gesture).
bool KiwiValid_RestoreOnly( const kiwiBaseBrush_t &base );

// RestoreOnly + the appropriate rebuild + the version bump, i.e. the whole
// rollback.  This is what a validity rejection and a Cancel() call.
bool KiwiValid_Restore( const kiwiBaseBrush_t &base );

// The §19 rebuild step, factored so every edit path uses ONE spelling of it:
// Brush_BuildWindings(def, 0) — bFull 0 because bFull 1 runs Brush_SnapPlanepts,
// i.e. the LEGACY power-of-two grid snap, which would quietly quantise an edit
// the modern layer has already snapped its own way (§17).  Brush_BuildWindings
// itself calls Brush_MakeFacePlanes first (brush.cpp:1412), so planes and
// windings are both current afterwards.  Also bumps def->version and marks the
// map modified, exactly as Brush_Move's tail does.
void KiwiValid_Rebuild( brush_t *def );

// THE gate.  Call after KiwiValid_Rebuild.  True = the brush is usable.
// `outWhy` (optional) receives a static string naming the failed check, for the
// HUD; it is untouched on success.
bool KiwiValid_CheckBrush( brush_t *def, const char **outWhy = 0 );

// ── ROUND AA, ITEM 4: THE SAME GATE, WITH ONE FACE TAKEN OUT OF THE BRUSH ────
// V1..V6 run over the face set MINUS `ignoreFace` (V1's four-half-space floor is
// counted after the exclusion); V7 is untouched, because the def's box comes from
// the half-space intersection and an excluded face that is a DUPLICATE of another
// contributes no point the twin did not already contribute.
//
// WHAT IT IS FOR, and why it is not a weakened gate.  Match Face copying a
// chamfer's plane onto its neighbour's makes the two faces COPLANAR, and V5
// ("duplicate plane") rejects that — correctly, because a brush written to disk
// with the same half-space twice is ambiguous to the compiler.  But the result
// the user asked for is the brush with the chamfer face GONE, and that brush has
// no duplicate in it.  So the caller gates the brush it is going to KEEP: the
// rebuilt geometry is bit-for-bit the same either way (a duplicated half-space
// clips nothing its twin did not already clip), so excluding the redundant face
// from V1..V6 asks about the real result rather than about the intermediate.
// `ignoreFace` < 0 means "exclude nothing", i.e. KiwiValid_CheckBrush exactly.
bool KiwiValid_CheckBrushIgnoringFace( brush_t *def, int ignoreFace,
                                       const char **outWhy = 0 );

// V8 — "do these planes still enclose a finite solid?"  See kiwi_validity.h's V8
// section for the probe box and for why V1..V7 cannot answer this.  Call it AFTER
// KiwiValid_Rebuild, and only from the verbs that REMOVE a half-space; it costs
// faceCount² winding clips, which is nothing for a one-shot op and far too much
// for a per-frame drag.  `def->[mins,maxs]` are saved and restored across the
// call, so the brush is byte-identical afterwards.
bool KiwiValid_BrushCloses( brush_t *def, const char **outWhy = 0 );

// V7 ALONE.  For geometry whose validity is already guaranteed by the ported core
// that produced it, and where V1..V6 would be wrong to run:
//   * PATCHES — not plane-defined, so the plane checks say nothing; the control
//     grid is free-form by construction.  Patch_Rebuild writes the box through
//     Brush_RebuildBrush, so the bounds ARE meaningful.
//   * BRUSH-VERTEX moves — Brush_MoveVertex (brush.cpp 0x471C30) already accepts
//     only a result that passes its own Brush_Convex test and otherwise reverts
//     the move itself, AND it deliberately does not call Brush_BuildWindings, so
//     def->[mins,maxs] are stale afterwards.  Running V4/V5/V6 over the triangles
//     its split/collapse pass produces would raise false rejections on perfectly
//     good geometry, and V7 would be reading last-rebuild's box.  See the caller's
//     note in kiwi_transform.cpp ApplyVerts.
bool KiwiValid_CheckBounds( brush_t *def, const char **outWhy = 0 );

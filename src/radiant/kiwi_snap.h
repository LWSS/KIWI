#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_snap.h — RADIANT_UX_DESIGN §6: the SnapManager.  V3 (Phase 4) = the v2 set
// (GRID + VERTEX + EDGE_MID + EDGE + FACE) plus the construction-geometry
// targets: ENDPOINT, INTERSECTION, CPLANE and ANGLE.
//
// V4 (SHAKEOUT F) adds the three targets the user asked for by name:
//   * CONSTRUCTION SEGMENT MIDPOINTS   (arm 3b, reported as SNAP_EDGE_MID)
//   * SNAP_FACE_CENTER                 (arm 4b) — now PRODUCED, brush faces only
//   * a RELATIVE base direction for SNAP_ANGLE (arm 7)
// SNAP_AXIS remains declared-but-unproduced; it lands with its consumer.
//
// ── THE RANKING THIS FILE IMPLEMENTS (spec §6: point > line > area > grid) ────
// For a cursor ray, in order — each arm gated by its OWN pixel radius:
//
//   0. CTRL HELD  →  snapping is SUPPRESSED (spec §6, deliberately inverted vs
//      other apps).  `type` = SNAP_NONE and `position` is the RAW point: the
//      ACTIVE CONSTRUCTION PLANE hit while a drawing tool runs (a tool must place
//      on its own plane even with snapping off), else the surface hit if the ray
//      hits geometry, else the ray∩Z=0 ground-plane point, else a point
//      KSNAP_FALLBACK_DIST down the ray.  `valid` is still true — "no snap" is an
//      answer, not a failure.
//
//   1. POINT — SNAP_ENDPOINT  →  a CONSTRUCTION anchor within PICK_VERT_PIXELS
//      (8 px): a line/polyline/rect defining point, or a circle/arc centre or
//      quadrant/end point (kiwi_construct.h KiwiCon_AnchorWorld).  V3 makes the
//      enum's ENDPOINT member real: it now means "a construction point", which is
//      the one endpoint kind a brush winding corner is NOT.  Ranked ABOVE brush
//      vertices because construction geometry is scaffolding the user placed
//      deliberately and is aiming at on purpose.
//
//   2. POINT — SNAP_VERTEX  →  a brush winding corner or a patch control point
//      within PICK_VERT_PIXELS (8 px).  §6 also lists SNAP_ENDPOINT for this, but
//      a winding corner IS an endpoint of both its edges, so the one name
//      SNAP_VERTEX covers the brush case and arm 1 owns the construction case.
//
//   3. POINT — SNAP_INTERSECTION  →  where two CONSTRUCTION segments cross,
//      within PICK_VERT_PIXELS.  SHAKEOUT H: solved in 3D — the pair's CLOSEST
//      APPROACH, accepted under KCON_ISECT_DIST (0.25) via KiwiCon_SegSegClosest,
//      reporting the midpoint.  It used to gather only the segments lying on the
//      ACTIVE construction plane and solve in 2D there, which with a world-space
//      store (kiwi_construct.h ruling 3) discarded almost every real crossing.
//      Still construction-only: brush-edge × brush-edge intersections need an
//      occlusion story the pick layer does not have yet (RADIANT_KNOWN_ISSUES).
//
//   3b. POINT — SNAP_EDGE_MID  →  the MIDPOINT of a CONSTRUCTION segment within
//      PICK_VERT_PIXELS (shakeout F).  Same type and same radius as arm 4 — a
//      midpoint is a midpoint — and above it for the same reason arm 1 sits above
//      arm 2: construction geometry is scaffolding the user placed deliberately.
//      `source` is a null item (construction has no sel_item_t).
//
//   4. POINT — SNAP_EDGE_MID  →  the MIDPOINT of the nearest brush edge, when
//      that midpoint itself projects within PICK_VERT_PIXELS of the cursor.  A
//      midpoint is a point-type snap, so it is tested with the POINT radius and
//      outranks the line snaps below — but only when the cursor is actually near
//      it, which is why it cannot shadow SNAP_EDGE along the rest of the edge.
//
//   4b. POINT — SNAP_FACE_CENTER  →  the WINDING CENTROID of the brush face under
//      the cursor, within PICK_VERT_PIXELS (shakeout F).  It comes from one extra
//      Pick(ray, SEL_MASK_FACE, flags): that mask is what makes the Test_Ray hit
//      resolve to a faceIndex at all (kiwi_pick.cpp needs the FACE bit set and the
//      OBJECT bit clear).  Last of the POINT ranks — a centroid is derived, and
//      anything the user actually placed or built outranks it.  SHAKEOUT H: NO
//      LONGER SUPPRESSED while a drawing tool runs — a tool now places AT its snap
//      instead of projecting it onto the working plane, so a face centre is a
//      legitimate place to start a line.  Arm 6 stays suppressed (see there).
//      V1 IS BRUSH FACES ONLY;
//      a patch has no winding and its "centre" is a tessellation question
//      (RADIANT_KNOWN_ISSUES).
//
//   4c. LINE — SNAP_AXIS  →  ROUND P.  One of the AXIS LINES through the drawing
//      tool's last placed point: world Z always, plus the working plane's U and V.
//      Competes with arm 5 on pixel distance (see below) and, like it, is a LINE
//      rank — so every POINT snap above still wins.  Full argument on
//      KiwiSnap_DrawAxisGuides below.
//
//   5. LINE — SNAP_EDGE  →  the closest point on the nearest LINE candidate
//      within PICK_EDGE_PIXELS (6 px).  Two sources compete here and the smaller
//      pixel distance wins outright: a brush edge (from ONE
//      Pick(ray, SEL_MASK_EDGE, flags) call, so the edge that snaps is always the
//      edge that would have been picked) and a construction segment.  `source` is
//      a null item for the construction case — construction geometry has no
//      sel_item_t (kiwi_construct.h scope ruling 1).
//
//   6. AREA — SNAP_FACE  →  the ported Test_Ray surface hit, UNSNAPPED.  §6 ranks
//      area above grid, and a surface point is what "put it on that face" means.
//      CHANGED FROM V1 (flagged): v1 grid-snapped the surface point.  A user who
//      wants the grid there holds nothing special — they point at empty space (arm
//      8) — or types an exact value, which is what §6's "numeric hygiene" note
//      really asks for.  Face push/pull along an axis-aligned normal still lands
//      on-grid because the transform grid-snaps its own scalar, not the cursor.
//      SUPPRESSED WHILE A DRAWING TOOL RUNS: a tool places on its own plane, and
//      an area snap would silently drag the point off it.
//
//   7. SNAP_ANGLE  →  drawing tools only, and only once the tool has an anchor
//      (its last placed point).  The cplane point's direction from that anchor is
//      quantised to KCON_ANGLE_STEP (15°) IN PLANE, keeping the distance.  Ranked
//      below every geometry snap on purpose: real geometry always beats a
//      constructed angle.
//
//      SHAKEOUT F — THE BASE DIRECTION.  The stops used to be measured from the
//      plane's u axis and nothing else.  They are now measured from whatever LINE
//      THE ANCHOR IS SITTING ON: the nearest construction segment or brush edge
//      whose point-to-segment distance from the anchor is under the store's weld
//      tolerance (KREG_JOIN_DIST — the same number that decides two endpoints are
//      the same endpoint, so "on it" means one thing everywhere), projected into
//      the plane's (u,v).  0° then means "along that line", which puts 45/90/135
//      relative on the ladder exactly where the directive asks for them.  The
//      INCREMENT is unchanged, and with nothing under the anchor the base is 0 and
//      the arm is bit-identical to v3.  The label reads "45 deg rel" in the
//      relative case and "45 deg" in the absolute one.
//
//   8. SNAP_CPLANE  →  drawing tools only: ray ∩ the active construction plane,
//      grid-snapped IN PLANE with the §17 spacing.  This is the arm that makes a
//      drawing tool work over empty space at all.
//
//   9. GRID on the GROUND PLANE  →  nothing under the cursor and no tool running:
//      intersect the ray with Z=0 (the §17 ground grid, which is the drawn snap
//      target) and snap that.  A ray that never crosses Z=0 (parallel, or pointing
//      away) falls back to KSNAP_FALLBACK_DIST down the ray, snapped.
//
// ── CONSTRUCTION CANDIDATE BUDGET ────────────────────────────────────────────
// Construction objects are walked directly (they are not in the Pick candidate
// set — kiwi_construct.h scope ruling 1).  Segments whose BOTH ends are behind
// the eye plane are skipped, and the intersection arm considers at most
// KSNAP_MAX_CSEGS segments, so its pair scan is bounded whatever the store holds.
//
// The grid arm uses KiwiGrid_Snap (kiwi_grid.h), i.e. the §17 MODERN spacing in
// inches — NOT the legacy power-of-two `d_gridsize`.  Ported snap sites are
// untouched.
//
// ── DEVIATION: the radii are the PICK radii, not a second set ────────────────
// §6 says "px radius ~10".  This file drives Pick() with SEL_MASK_VERTEX /
// SEL_MASK_EDGE, whose radii are PICK_VERT_PIXELS (8) and PICK_EDGE_PIXELS (6),
// instead of running its own screen-space scans.  Reasons: one candidate walk,
// one projection, one ranking — so what snaps is always exactly what would have
// been picked/hovered, and there is no second copy of ScanList to drift.  8 px is
// inside "~10" and is the tighter, safer end of it.
//
// ── EXCLUDING THE GEOMETRY BEING MOVED (Phase 3) ─────────────────────────────
// `pickFlags` is forwarded verbatim to every Pick() call.  A live transform passes
// PICKF_EXCLUDE_SELECTED so the thing it is dragging is not a snap candidate for
// itself; everything else passes 0 and gets the v1 candidate set.
//
// Cost is up to FOUR Pick() calls per query since shakeout F (vertex-mask,
// edge-mask, object-mask and — only when no drawing tool is running — face-mask
// for arm 4b; the screen-space masks skip Test_Ray, the area ones skip the screen
// scan).  Budget is one query per frame, same as the hover pick.  The shakeout-F
// relative-angle arm additionally walks brush edges near the tool ANCHOR, whole-
// brush bbox-rejected first and hard-capped at KSNAP_MAX_ANCHOR_EDGES.
// ─────────────────────────────────────────────────────────────────────────────

#include "kiwi_pick.h"

// Distance down the ray used when nothing is hit and the ray misses Z=0.
#define KSNAP_FALLBACK_DIST 512.0f

// Construction segments the intersection arm may pair up in one query.
#define KSNAP_MAX_CSEGS     128

enum snap_type_t
{
    SNAP_NONE = 0,
    SNAP_GRID,           // v1
    SNAP_VERTEX,         // v1  — brush winding corners / patch control points
    SNAP_EDGE_MID,       // v2 (Phase 3)
    SNAP_EDGE,           // v2  — brush edges; v3 also construction segments
    SNAP_FACE,           // v2
    SNAP_FACE_CENTER,    // v4 (shakeout F) — brush face winding centroid
    SNAP_ENDPOINT,       // v3 (Phase 4) — CONSTRUCTION anchors
    SNAP_INTERSECTION,   // v3 (Phase 4) — construction segment × segment
    SNAP_AXIS,           // ROUND P — an axis line through the tool's last point
    SNAP_CPLANE,         // v3 (Phase 4)
    SNAP_ANGLE,          // v3 (Phase 4)
    SNAP_TYPE_COUNT
};

struct snap_result_t
{
    bool        valid       = false;
    snap_type_t type        = SNAP_NONE;
    float       position[3] = { 0.0f, 0.0f, 0.0f };
    sel_item_t  source;             // what we snapped to (marker + label); null for grid
};

// True for the types that named REAL GEOMETRY (as opposed to the grid, or nothing).
// A transform that wants "drag my reference point onto that thing" keys off this:
// a geometry snap defines an exact target position, the grid instead quantises the
// transform's own delta.
inline bool KiwiSnap_IsGeometry( snap_type_t t )
{
    return t == SNAP_VERTEX || t == SNAP_EDGE_MID || t == SNAP_EDGE
        || t == SNAP_FACE   || t == SNAP_FACE_CENTER || t == SNAP_ENDPOINT
        || t == SNAP_INTERSECTION;
}

// ── ROUND Z, ITEM 3: RESOLVE A SNAP ONTO A ONE-AXIS GESTURE'S AXIS ───────────
// USER REPORT: "When snapping an extrusion to another face.  The entire face should
// give the same result."
//
// The signed distance along the unit `axis`, measured from `ref`, that this snap
// result means for a gesture whose only freedom is that axis — the face push/pull
// (kiwi_transform.cpp RecomputeFace), the region extrude and the face extrude /
// un-extrude (kiwi_extrude.cpp).  ONE definition, shared, so the three cannot drift
// apart the way their three hand-rolled `dot( pos - ref, axis )` copies did.
//
// A SURFACE (SNAP_FACE) is resolved by intersecting the TARGET FACE'S PLANE with
// the axis line, so the answer is the same everywhere on that face; every point
// candidate keeps its exact position and is projected.  False = this snap has no
// usable answer (not a geometry snap, or a face edge-on to the push axis) and the
// caller must leave its distance alone.  The full argument is on the definition.
//
// |n · axis| below this is "edge-on": the plane crosses the axis at a distance the
// user cannot have meant, and a hair of cursor movement would move it by hundreds
// of units.  0.05 is about 87 degrees off the axis.
#define KSNAP_AXIS_PARALLEL 0.05f
bool KiwiSnap_AxisDepth( const snap_result_t &r, const float *ref,
                         const float *axis, float *outDist );

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AG, ITEM 11 — LIGHT GRID SNAPPING, AND THE PARTIAL WALK-BACK OF D-Z2
// ═════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: "There should be light snapping to the global grid
// (disabled with ctrl)."
//
// ROUND Z, ITEM 2 (D-Z2) made the three ONE-AXIS gestures raw by default on the
// user's own report ("When extruding, it should not snap by default […] It's
// just not good to use in a cluttered scene").  Both directives are right and
// they are not in conflict once you separate the two things "snapping" meant:
//
//   THE GEOMETRY ARMS were what made a cluttered scene unusable — the query is
//     RANKED, it reaches across the whole viewport, and it drags the drag onto
//     whichever of a hundred nearby edges/verts/faces won.  Those stay OFF by
//     default.  This is D-Z2 and it is NOT walked back.
//   THE GRID is a fixed, predictable, global lattice that is exactly where a
//     mapper wants to land, and it was collateral damage: turning off the ranked
//     query also turned off the grid quantisation, so a plain drag could not land
//     on a round number at all without Ctrl (which brings the clutter back with
//     it).  THAT is what this restores, and only that.
//
// LIGHT means two things, both load-bearing:
//   1. GRID ONLY.  No geometry candidates, ever.  A default drag can be pulled
//      onto the lattice and onto nothing else.
//   2. A CAPTURE BAND, NOT A QUANTISER.  The value is nudged only when it is
//      ALREADY within KSNAP_LIGHT_BAND_PIX screen pixels of a lattice value;
//      outside the band it passes through completely untouched.  So free dragging
//      still feels continuous — it does not step — and the lattice reads as a
//      magnet rather than as a ratchet.  That difference is the whole directive:
//      full quantisation IS what Ctrl already does.
// The band is in SCREEN PIXELS (converted at the gesture's own depth through
// KiwiCam_WorldPerPixel) so the stickiness feels identical zoomed in and zoomed
// out, and it is additionally capped at KSNAP_LIGHT_MAX_FRAC of a cell so that
// zooming far out can never widen it into full snapping.
//
// PRECEDENCE, unchanged: the NUMERIC FIELD outranks everything and is tested
// first by every caller; Ctrl runs the full ranked query; otherwise this.
// KEXT_SELF_SNAP_BAND is untouched — it lives on the geometry arm.
//
// ── WHICH COORDINATE IS PUT ON THE LATTICE ──────────────────────────────────
// Round P's rule, reused verbatim rather than re-decided: when `axis` IS a world
// axis the ABSOLUTE world coordinate is snapped (so an off-grid start does not
// drag its offset along with it — "the global grid" means the global grid); when
// it is not, there is no world coordinate to be on the grid of and the DISTANCE
// is snapped instead.
//
// `d`    the gesture's signed distance along `axis` from `ref`
// `ref`  the gesture's origin in world space
// `axis` the unit axis
// Returns `d`, possibly nudged.  A zero/absent grid spacing returns `d` unchanged.
#define KSNAP_LIGHT_BAND_PIX  5.0f      // screen pixels either side of a lattice value
#define KSNAP_LIGHT_MAX_FRAC  0.22f     // …and never more than this much of one cell
float KiwiSnap_LightGridAxis( float d, const float *ref, const float *axis );

// THE query.  `imgX`/`imgY` are the camera-image cursor position (TOP-LEFT origin,
// kiwi_pick.h conventions) — recorded for the screen-space label anchor; the ray
// is what actually drives the result.  `pickFlags` is forwarded to every Pick()
// call (PICKF_EXCLUDE_SELECTED during a live transform).  Returns `out->valid`.
bool KiwiSnap_Query( const ray_t &ray, int imgX, int imgY, snap_result_t *out,
                     unsigned pickFlags = PICKF_NONE );

// Short tag for the marker label: "vert", "mid", "edge", "face", "grid", "off".
const char *KiwiSnap_TypeName( snap_type_t t );

// World-space marker for a snap result, emitted into the CURRENTLY OPEN
// kiwi_lines batch (the caller owns Begin/Flush and the budget).  No-op when the
// result is invalid or the marker toggle is off.
void KiwiSnap_EmitMarker( const snap_result_t &r );

// ── ROUND N: the hovered-face SNAP ACCENTS ───────────────────────────────────
// USER DIRECTIVE: "when the line tool(or similar) is in use, and a face is
// hovered, in plasticity (see pic) the points have an accent to help identifying
// snapping spots.  Add that."
//
// Small dark dots at every snap target of the ONE face under the cursor — its
// winding corners (arm 2), its edge midpoints (arm 4) and its centroid (arm 4b) —
// so a drawing tool advertises where it CAN land before the cursor is near any of
// them.  Emits nothing unless a command that WantsClicks is live and the marker
// toggle is on; opens and flushes its OWN kiwi_lines batch (the budget and the
// full argument are on the definition in kiwi_snap.cpp).
void KiwiSnap_DrawFaceAccents();

// ── ROUND Y, ITEM 3: ONE SPOT, ON DEMAND ────────────────────────────────────
// USER DIRECTIVE: "when using the split tool, show the center dot so I can find
// it easier."  KiwiSnap_DrawFaceAccents is gated on a command that WantsClicks,
// which a DRAG tool such as the live face split is not, so a tool that wants to
// mark ONE place asks for it here instead of the accent pass being relaxed for
// everybody.  Emits into the CURRENTLY OPEN kiwi_lines batch (the caller owns
// Begin/Flush, the budget and the colour run); `pixRadius` is screen pixels at
// the point's own depth, `solid` picks the filled dot glyph over the ring.
// Ignores the marker toggle: this is a tool's own preview, not a snap readout.
void  KiwiSnap_EmitSpot( const float *p, float pixRadius, bool solid );
float KiwiSnap_AccentPixels();      // KSNAP_ACCENT_PIX — the small filled dot
float KiwiSnap_RingPixels();        // KSNAP_RING_PIX   — the separated ring

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND P — THE AXIS GUIDES (arm 4c).  DRAWING VERTICAL, BY AIMING.
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "It's still hard to draw a vertical Z line from a 2D xy
// line, even when using the camera.  Make it more easy like plasticity.  The
// camera angle should really help."
//
// ── WHAT PLASTICITY DOES (cited, from the LGPL tree) ────────────────────────
// Its point picker keeps a set of `straightSnaps` — X, Y and Z
// (PointPickerModel.ts:18,25; AxisSnap.ts:29-31) — and REBUILDS them through the
// LAST PICKED POINT every time a point is placed:
//     work = work.concat(new PointSnap(undefined, last.point).axes(axes));
//                                        (PointPickerModel.ts:102-130)
// `PointSnap.axes` translates each AxisSnap to the point (PointSnap.ts:21-29) and
// `AxisSnap.move` turns it into a named `PointAxisSnap` (AxisSnap.ts:69-71).  On a
// face the FACE-ORIENTED axes are added too (`facePreferenceMode = 'weak'`,
// CurveCommand.ts:23) with world-aligned duplicates suppressed.  They are ordinary
// snap candidates: raycast against a `Line2` at `Line2: { threshold: 30 }` SCREEN
// PIXELS (PointPicker.ts:138-142) and ranked as type 2, i.e. below every point
// snap and above faces and planes (SnapPicker.ts:136-161).  The guide is drawn
// only while that axis is among the hit snaps (SnapPresenter.ts:71-84): a
// ±100,000-unit line, dim grey 0xaaaaaa, dashed material, plus a dot on the source
// point (AxisSnap.ts:8-27, 113-127).  x/y/z also LOCK the axis outright
// (`snaps:set-x/y/z`, default-keymap.ts:353-360).
//
// ── WHAT THIS IMPLEMENTS, AND EVERY PLACE IT DEVIATES ───────────────────────
//   * THE AXES: world Z always, plus the working plane's U and V (our plane basis
//     is the local frame Plasticity gets from the hovered face's orientation).  A
//     plane axis that is parallel to world Z is dropped — Plasticity's
//     `isAxisAligned` dedup (PointPickerModel.ts:320-323).
//   * THE RADIUS: 10 px, not 30.  30 px of capture is right in an app whose
//     viewport holds a handful of curves; this editor's viewport is full of brush
//     edges at 6 px and construction segments at 10 px, and an axis that grabbed
//     three times wider than either would shadow them both.
//   * THE CAMERA ANGLE HELPS, which is the half of the directive that matters:
//     the VERTICAL axis's radius widens to 16 px as the camera flattens toward the
//     horizon (|vpn.z| <= 0.35), tapering back to 10 px by |vpn.z| >= 0.70.
//     Looking at the horizon is the posture in which "up" is what the user is
//     reaching for and also the posture in which the vertical is easiest to aim
//     at; looking down at the floor it is the opposite, and near-vertically down
//     the axis projects to a DOT and is refused outright (an 8 px minimum
//     projected length).
//   * THE POINT IS GRID-SNAPPED ALONG THE AXIS, so "straight up, exactly three
//     cells" is one gesture — and ABSOLUTELY when the axis is a world axis, which
//     is ROUND P's own rule for the transform's grid arm and has always been the
//     Z lock's (it quantises the world z).  An off-grid anchor therefore does not
//     drag its offset up the axis.  A slanted plane axis has no world coordinate
//     to be on the grid of and quantises the distance instead.
//   * RANK: it competes with arm 5 on PIXEL DISTANCE rather than taking a fixed
//     priority over brush edges.  Plasticity ranks axes above faces but has no
//     brush-edge snap to rank against; here a real edge under the cursor is a
//     thing the user is aiming at, and "closer wins" is already this file's rule
//     for the two LINE sources it had.
//   * NO x/y/z LOCK KEY.  The shakeout-H Z toggle (kiwi_construct.cpp) is the lock
//     we already have and it still works — it is now the SECOND way to go vertical,
//     with aiming at the guide being the first.  Both are advertised in the tool's
//     HUD.
//
// The guide itself: DASHED (drawn as a run of short segments — kiwi_lines has no
// dash mode), dim grey, clipped to ~420 px each way at the anchor's depth rather
// than Plasticity's ±100,000, because a world-length guide either vanishes when
// you are close or crosses the whole map when you are far.  Its own batch, like
// the face accents.  Emits nothing unless a drawing tool has a last placed point
// and an axis is actually the live snap.
void KiwiSnap_DrawAxisGuides();

// The screen-space label near the cursor: the snap tag plus the position, always
// formatted in INCHES through KiwiUnits_Format.  Drawn during the ImGui frame,
// inside the camera image (kiwi_viewport.cpp).
void KiwiSnap_DrawLabel( const snap_result_t &r, float imgMinX, float imgMinY,
                         float imgW, float imgH );

// §6 marker/label on-off (KiwiUX settings, registry-persisted).  Snapping itself
// is always on — CTRL is the temporary off switch.
bool KiwiSnap_ShowMarkers();
void KiwiSnap_SetShowMarkers( bool on );

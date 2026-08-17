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
// V5 (ROUND P) adds SNAP_AXIS — arm 4c, documented below and produced at
// kiwi_snap.cpp:1723.  KIWI-UX (CLEANUP, A-4)   // KIWI-UX (ROUND BQ): re-cited
//
// ── THE RANKING THIS FILE IMPLEMENTS (spec §6: point > line > area > grid) ────
// For a cursor ray, in order — each arm gated by its OWN pixel radius:
//
//   0. CTRL HELD  →  snapping is SUPPRESSED (spec §6, deliberately inverted vs
//      other apps).  `type` = SNAP_NONE and `position` is the RAW point: the
//      PLANAR PLACER's plane hit while one runs (a rect/circle/arc/n-gon or a §16b
//      primitive must place on its own plane even with snapping off — ROUND BS),
//      else the surface hit if the ray hits geometry, else the ray∩Z=0
//      ground-plane point, else a point KSNAP_FALLBACK_DIST down the ray.  `valid`
//      is still true — "no snap" is an answer, not a failure.  NOTE THE LABEL:
//      KiwiSnap_TypeName(SNAP_NONE) is the string **"off"** — it means "snapping
//      is not engaged", it has never meant "refused", and a transform shows it
//      whenever Ctrl is not held (see the ROUND BS block below).
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
//      9) — or types an exact value, which is what §6's "numeric hygiene" note
//      really asks for.  Face push/pull along an axis-aligned normal still lands
//      on-grid because the transform grid-snaps its own scalar, not the cursor.
//      ROUND BS: THIS IS THE USER'S "WHEREVER A RAY TRACE HITS AGAINST AN OBJECT"
//      AND IT IS NO LONGER SUPPRESSED FOR THE LINE TOOLS.  It is suppressed only
//      while a PLANAR PLACER runs (rect/circle/arc/n-gon, §16b primitives), where
//      an area hit on some other surface would be projected onto the tool's plane
//      and land under the wall instead of under the cursor.
//
//   7. SNAP_ANGLE  →  PLANAR PLACERS only (ROUND BS), and only once the tool has
//      an anchor
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
//   8. SNAP_CPLANE  →  PLANAR PLACERS only (ROUND BS): ray ∩ the placer's own
//      plane, grid-snapped IN PLANE with the §17 spacing.  This is the arm that
//      makes a rect on a wall work over empty space.  UNREACHABLE for the line,
//      polyline and spline tools — they have no plane at all now.
//
//   9. GRID on the GROUND PLANE  →  nothing under the cursor and no planar placer
//      running: intersect the ray with Z=0 (the §17 ground grid, which is the
//      drawn snap target) and snap that.  A ray that never crosses Z=0 (parallel,
//      or pointing away) falls back to KSNAP_FALLBACK_DIST down the ray, snapped.
//      ROUND BS: this is the user's "the world ground" — the last resort, and the
//      only "plane" left anywhere in placement.
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
// edge-mask, object-mask and face-mask for arm 4b — the face-mask is
// unconditional on a valid cursor pixel since shakeout H; the screen-space masks
// skip Test_Ray, the area ones skip the screen scan).  KIWI-UX (CLEANUP, A-5).  Budget is one query per frame, same as the hover pick.  The shakeout-F
// relative-angle arm additionally walks brush edges near the tool ANCHOR, whole-
// brush bbox-rejected first and hard-capped at KSNAP_MAX_ANCHOR_EDGES.
// ─────────────────────────────────────────────────────────────────────────────

#include "kiwi_pick.h"

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BN, ITEM 7) — THE SNAP LADDER, RE-STATED, AND THE ONE RULE
//  THAT WAS MISSING
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: *"(see pic) Snap dragging is buggy.  Here it should just
// snap to the nearest plane of the building unless I'm near a detailed point.  Try
// to revamp this.  (For all tools.)"*  The picture is an edge drag on a stepped
// rooftop with construction lines all over it, and the drag is being pulled onto
// something that is nowhere near the roof.
//
// ── THE LADDER THE USER IS DESCRIBING IS THE ONE THIS FILE ALREADY HAS ──────
// point > line > area > grid, with every point arm gated by a small pixel radius.
// So the reported failure cannot be the ORDER, and it is not: it is that PIXEL
// PROXIMITY IS NOT WORLD PROXIMITY under a perspective divide.  A construction
// anchor a hundred metres behind the building projects within 8 px of the cursor
// just as easily as the roof corner in front of it does, wins arm 1 outright, and
// the drag teleports across the map.  That is the whole bug, and it is why "it
// should just snap to the nearest plane of the building" reads as a ranking
// complaint when it is really an occlusion complaint.
//
// ── PLASTICITY'S SnapPicker, READ, AND WHAT IT ACTUALLY DOES ────────────────
// Two mechanisms, not one, and KIWI had only the first:
//   1. A PRIORITY SORT, which is our ladder almost exactly —
//      `FaceCenterPointSnap.priority = 0.99`, `PointSnap = 1`,
//      `CurveSnap / AxisSnap / CurveEdgeSnap = 2`,
//      `FaceConstructionPlaneSnap / FaceSnap = 3`, `PlaneSnap = 4`,
//      `ConstructionPlaneSnap = 5`, sorted by `i1.snap.priority - i2.snap.priority`
//      (SnapPicker.ts:136-161).  Points beat edges beat the face beat the plane:
//      the brief's ladder, and ours.
//   2. A DEPTH FILTER APPLIED **BEFORE** THAT SORT.  `processXRay` runs
//      `findAllIntersectionsVeryCloseTogether(results, minDistance)` whenever the
//      viewport is not in X-RAY mode (SnapPickerStrategy.ts:93-105), and that
//      function keeps ONLY the candidates whose raycast distance is within 10e-3
//      of the nearest one:
//          for (const intersection of intersections)
//              if (Math.abs(minDistance - intersection.distance) < 10e-3)
//                  result.push(intersection);
//      Everything further down the ray — i.e. everything the surface under the
//      cursor is standing in front of — is DISCARDED before priority is consulted.
//      Their pixel radii are enormous by our standards (`Points: { threshold: 26 }`,
//      `Line2: { threshold: 30 }`, SnapPickerStrategy.ts:14-18) and that is only
//      liveable BECAUSE of this filter.
//
// ── WHAT ROUND BN ADDS, THEREFORE ───────────────────────────────────────────
//   * THE OCCLUSION GATE (KSNAP_OCCLUDE_SLOP_PX).  When the ray hits a surface,
//     every POINT and LINE candidate that lies further down the ray than that
//     surface — by more than a few pixels' worth of depth at its own scale — is
//     refused.  Candidates in FRONT are kept (this is deliberately weaker than
//     Plasticity's keep-only-the-nearest-cluster: a vertex floating in front of a
//     wall is a legitimate target, and dropping it would be a new complaint).
//     With the roof under the cursor, the construction scaffolding behind it stops
//     competing and the FACE PLANE — arm 6, the "glue to the surface" the user is
//     asking for — is what the ladder falls through to.  Exactly the described feel.
//   * ONE RADIUS CONSTANT PER CLASS, below, instead of three arms sharing
//     PICK_VERT_PIXELS by coincidence and a fourth using the construction CLICK
//     box.  The numbers are the brief's (6-8 px), ordered so that the more DERIVED
//     a point is, the closer you have to be to it.
// The ORDER of the arms is unchanged and the AP self-snap mutes and the AA/AP
// exclusion rules are untouched — they run inside the candidate scans, upstream of
// all of this.
// ── KIWI-UX (ROUND BO, ITEM 3): …AND THIS LADDER IS USER-APPROVED ───────────
// USER RESPONSE to round BN, verbatim: *"Dragging is a lot better.  Keep it the
// same."*  Round BO changes WHEN the ladder engages (arm 0's one modifier rule)
// and NOTHING about how it behaves: the radii, the occlusion gate, the ranking and
// the SNAP_FACE glue below are byte-for-byte BN's.  DO NOT REGRESS THEM.  The
// contract page is RADIANT_UX_DESIGN §35.
//
// PER-CLASS PIXEL RADII.  A detail point must be AIMED AT; the ladder is only
// allowed to prefer one over the big obvious surface when the cursor is genuinely
// on it.
#define KSNAP_R_CON_POINT     7.0f   // construction anchor / intersection / midpoint
#define KSNAP_R_VERTEX        PICK_VERT_PIXELS   // 8 — brush corners / patch control points
#define KSNAP_R_EDGE_MID      7.0f   // a brush edge's midpoint
#define KSNAP_R_FACE_CENTER   6.0f   // a face centroid: the most derived point there is
#define KSNAP_R_CON_SEG       8.0f   // the construction segment SNAP box.  The construction
                                     // CLICK box stays KCON_LINE_PIXELS (10) — picking a
                                     // line to select it and being dragged onto it are
                                     // different questions and want different slack.
// How far BEYOND the surface under the cursor a candidate may still be and count
// as "on it".  In SCREEN PIXELS, converted at the candidate's own depth, so the
// tolerance is the same visual slack near and far.  Generous enough that a vertex
// lying exactly on the hit surface can never be refused by float error, tight
// enough that anything in the next room is.
#define KSNAP_OCCLUDE_SLOP_PX 6.0f

// ── KIWI-UX (ROUND BS) — THE OFF-PLANE GATE IS DELETED ──────────────────────
// TOMBSTONE.  Round BP's working-plane depth rule (KSNAP_ONPLANE_PX /
// KSNAP_OFFPLANE_PX and the `planeOn` half of kiwi_snap.cpp's CandGate) is gone,
// with both constants.  Its premise was *"while a tool places on a working plane,
// that plane IS the depth the user is working at"*; this round removes the working
// plane from the drawing path entirely, so there is no such depth and the gate has
// nothing left to mean.
//
// USER RULING, verbatim: *"You seriously need to fucking get rid of the
// construction plane. […] It just needs to be wherever a fucking ray trace hits
// against an object OR a snapping point."*
//
// THE OCCLUSION GATE ABOVE (KSNAP_OCCLUDE_SLOP_PX, round BN — *"Dragging is a lot
// better.  Keep it the same."*) IS THE ONLY DEPTH RULE LEFT, and it applies in
// every context.  Everything the deleted gate was written to stop is stopped by
// its ROOT cause being gone: the 61-ft endpoints it was refusing came from a plane
// latched at roof height, and no plane is latched from placement any more.

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

// KIWI-UX (CLEANUP, SnapActive): "did this query land on anything at all".
// `valid` alone is not the test — a result can be valid and still carry
// SNAP_NONE — so the two conditions belong together, and they were written out
// as a private SnapActive() in FOUR command classes (kiwi_transform.cpp,
// kiwi_uv.cpp, kiwi_bevel.cpp and, before round T folded it away, the bevel
// command).  kiwi_bevel.cpp's copy had no caller left at all (B-24) and dies
// with this consolidation.  One spelling, beside the struct.
inline bool KiwiSnap_Active( const snap_result_t &s )
{
    return s.valid && s.type != SNAP_NONE;
}

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
//  THE LATTICE BAND — round AG's numbers, round BO's placement
// ═════════════════════════════════════════════════════════════════════════════
// ── KIWI-UX (ROUND BO, ITEM 3): ROUND AG'S "LIGHT GRID SNAPPING" IS DELETED ──
// USER REPORT, verbatim: *"The grid snapping is better, but now it's impossible to
// get fine details.  Make it so it only snaps while holding Ctrl."*
//
// Round AG, item 11 answered an earlier directive ("There should be light snapping
// to the global grid (disabled with ctrl)") by giving the one-axis gestures a
// capture band that fired in the branch where the ranked query had been SUPPRESSED
// — i.e. it was snapping that happened precisely when the user had asked for none.
// Under round BO's one rule that branch is "the user is not holding Ctrl during a
// transform", which is exactly where fine detail has to be reachable, so the
// wrapper (KiwiSnap_LightGridAxis) and its three call sites are gone.
//
// THE BAND ITSELF IS NOT DEAD.  It is the SOFT mode of KiwiSnap_LatticeAxis below,
// and the object move still reaches it for a SNAP_FACE hit — under Ctrl, where it
// belongs.  These are its two constants and they are unchanged:
//   * A CAPTURE BAND, NOT A QUANTISER.  The value is nudged only when it is
//     ALREADY within KSNAP_LIGHT_BAND_PIX screen pixels of a lattice value;
//     outside the band it passes through completely untouched, so dragging over a
//     surface still feels continuous rather than stepping.
//   * SCREEN PIXELS, converted at the gesture's own depth through
//     KiwiCam_WorldPerPixel, so the stickiness feels identical zoomed in and
//     zoomed out — and capped at KSNAP_LIGHT_MAX_FRAC of a cell so that zooming
//     far out can never widen it into full snapping.
//
// PRECEDENCE: the NUMERIC FIELD outranks everything and is tested first by every
// caller; below it, arm 0 decides whether snapping is engaged at all, and only
// then does the ladder-vs-lattice ranking matter.
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
// ── KIWI-UX (ROUND BK, ITEM 6d): THE MAJOR LINES ARE STICKIER ───────────────
// USER DIRECTIVE, verbatim: *"…and it should be easier to lock to the major lines
// of the grid."*  A MAJOR is every tenth cell — the grid draw's own definition,
// `i % 10 == 0` (kiwi_grid.cpp EmitRun), and the one this file must agree with or
// the magnet would stick to lines the user cannot see.  A candidate that is also a
// major gets a band this much wider, and when both a minor and a major are in
// reach the major wins.  1.5 is small on purpose: it biases a tie, it does not
// make the majors a second lattice.
#define KSNAP_MAJOR_STRIDE    10        // cells per major — kiwi_grid.cpp EmitRun
#define KSNAP_MAJOR_BAND_MUL  1.5f
// KIWI-UX (ROUND BO, ITEM 3): KiwiSnap_LightGridAxis is DELETED.  Its three call
// sites were all the "ranked query suppressed" branch of a TRANSFORM, which is now
// exactly where the user asked for nothing to happen.  The SOFT band above is not
// dead — KiwiSnap_LatticeAxis( …, hard=false, … ) still serves the object move's
// SNAP_FACE arm, under Ctrl.

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BL, ITEM 4) — ONE LATTICE RULE, HARD OR SOFT
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: *"It's still not possible to snap to the major grid lines
// while using a tool.  This makes it really hard to align things.  You need to keep
// the pivot point in line with the cursor like this and snap it."*
//
// Round BK gave the MAGNET its major-line preference (KSNAP_MAJOR_BAND_MUL above)
// and left the HARD quantiser — the one the object move's grid arm actually runs
// when nothing is under the cursor — with no notion of a major at all: it rounds to
// the nearest CELL, so a pivot 0.3 cells from a major landed on the minor beside it
// and the major was unreachable by aiming.  Two quantisers, one of which had never
// heard of the feature.
//
// So both are this one function now and the major rule is stated once:
//   * the nearest MAJOR is computed independently of the nearest cell (they are not
//     the same line) and wins whenever it is inside KSNAP_MAJOR_BAND_MUL times the
//     band — in HARD mode the band is a whole cell, so a major within 1.5 cells
//     takes the value;
//   * otherwise HARD rounds to the nearest cell unconditionally, and SOFT nudges
//     only inside the KSNAP_LIGHT_BAND_PIX capture band and passes the value
//     through untouched outside it.
// `outMajor` (optional) reports whether a MAJOR line took the value, which is what
// the move's HUD says out loud so the lock is legible.
// KIWI-UX (ROUND BO, ITEM 3): SOFT now has exactly ONE caller — the object move's
// SNAP_FACE arm — and it is reached only while snapping is ENGAGED (Ctrl, in a
// transform).  HARD is every other lattice answer.
float KiwiSnap_LatticeAxis( float d, const float *ref, const float *axis,
                            bool hard, bool *outMajor );

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BK, ITEM 6a/6c) — AN AREA HIT IS A MAGNET, NOT A TELEPORT
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORTS, verbatim: *"I can no longer snap to the grid when moving with the
// gizmo."* and *"the center of the gizmo should always track my mouse."*
//
// THE ROOT CAUSE OF BOTH IS ONE RANKING.  Arm 6 of KiwiSnap_Query (kiwi_snap.cpp,
// "AREA — the Test_Ray surface point") answers SNAP_FACE for ANY surface under the
// cursor and RETURNS, so arm 9 — the grid — is unreachable whenever the ray hits
// geometry, which in a built scene is nearly always.  The Move command then routes
// every KiwiSnap_IsGeometry result, SNAP_FACE included, through its ABSOLUTE arm
// (`total = snapPos - ref`, or KiwiSnap_AxisDepth under an axis lock), so:
//   * the grid quantiser in the `else` branch never runs   -> "no grid snap", and
//   * the selection is dragged onto whatever plane happens to be under the cursor
//     rather than tracking the cursor                       -> "the gizmo lags".
//
// AND THE NAMED ARMS ARE INNOCENT.  A vertex, an edge midpoint, a face CENTRE, a
// construction endpoint or an intersection is a target the user aimed at, and
// dragging the reference point exactly onto it is the whole point of the feature
// ("able to snap corners together easily by placing a pivot").  SNAP_FACE is the
// one arm that names no feature — it is wherever the ray happened to land — so it
// is the one that becomes a magnet.
//
// `cur`    the value the CURSOR mapping produced (world units along some axis)
// `target` the value the area snap would impose
// `at`     a world point at the gesture's current position, for the pixel scale
// Returns `target` when it is within KSNAP_AREA_BAND_PIX screen pixels of `cur`,
// otherwise `cur` untouched.  Round AA ITEM 5's "X-lock this brush until it meets
// that wall" therefore still fires — as you approach the wall, which is when it
// was ever wanted — and stops rewriting the drag from across the room.
#define KSNAP_AREA_BAND_PIX  14.0f
float KiwiSnap_AreaMagnet( float cur, float target, const float *at );

// THE query.  `imgX`/`imgY` are the camera-image cursor position (TOP-LEFT origin,
// kiwi_pick.h conventions) — recorded for the screen-space label anchor; the ray
// is what actually drives the result.  `pickFlags` is forwarded to every Pick()
// call (PICKF_EXCLUDE_SELECTED during a live transform).  Returns `out->valid`.
bool KiwiSnap_Query( const ray_t &ray, int imgX, int imgY, snap_result_t *out,
                     unsigned pickFlags = PICKF_NONE );

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BS) — **THE** PLACEMENT RESOLUTION.  ONE FUNCTION.
// ═════════════════════════════════════════════════════════════════════════════
// USER RULING, verbatim: *"It just needs to be wherever a fucking ray trace hits
// against an object OR a snapping point."*
//
// A DRAWN OR PLACED POINT IS, IN THIS EXACT ORDER:
//   1. THE ENGAGED SNAP POINT — the BN ladder (arms 0b-5: chain start, construction
//      anchor, brush vertex, crossing, midpoints, face centre, axis guide, edge).
//      Construction context: snapping is ON by default and Ctrl frees it; transform
//      context: OFF by default and Ctrl engages it (the BO contract,
//      KiwiCmd_SnapEngaged).
//   2. ELSE THE RAY'S SURFACE HIT ON GEOMETRY — arm 6, the Test_Ray point on the
//      brush face or patch under the cursor, verbatim and unsnapped.
//   3. ELSE THE WORLD GROUND, z = 0, GRID-SNAPPED — arm 9.  Not a system, not a
//      plane object, not state: the floor.  (A ray that never crosses z = 0 falls
//      back KSNAP_FALLBACK_DIST down itself so there is always an answer.)
// The ONLY exception is a PLANAR PLACER — rect / circle / arc / n-gon and the §16b
// primitives — whose SHAPE cannot exist off a plane: between 1 and 2 they take
// their own plane's hit (arms 7-8), and they derive that plane from the FIRST
// point's surface.  The line, polyline and spline tools have no plane at all.
//
// PREVIEW AND PLACEMENT COME THROUGH HERE AND NOWHERE ELSE.  The rubber band is
// `snap.position`, the stored point is `snap.position`, and the seed a tool latches
// at Begin() is this function — so the marker can never draw in one place while the
// geometry goes to another (the failure the user reported four rounds running).
// Returns false only when the query itself refused (no camera, degenerate ray).
bool KiwiSnap_ResolvePoint( const ray_t &ray, int imgX, int imgY, float out[3] );

// Short tag for the marker label: "vert", "mid", "edge", "face", "grid", "off".
// "off" IS SNAP_NONE AND MEANS "SNAPPING IS NOT ENGAGED" — arm 0's answer, i.e.
// a transform without Ctrl (or an unusable grid spacing on arm 9).  It has never
// meant "refused" or "off-plane"; ROUND BS re-states this because a screenshot of
// an extrude showing `off` was read as a plane refusal leaking into a transform.
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

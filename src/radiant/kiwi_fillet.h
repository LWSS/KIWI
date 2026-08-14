#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_fillet.h — ROUND J: FILLET CORNERS (construction curves).
//
//     KIWI_CMD_FILLET_CURVE  34061   MODAL, modern key B
//
// Round a construction chain's corners into arcs at a dragged or typed radius.
// With the §23 extruder this is how a rounded room, a rounded pillar footprint or
// a curved corridor gets drawn without hand-placing an arc at every corner.
//
// ── THE PLASTICITY SOURCE, AND THE ONE PLACE KIWI DIVERGES ─────────────────
// Curve filleting in Plasticity is NOT `b`.  `b` is `command:fillet-solid`
// (default-keymap.ts:264), which is FilletCommand.ts:16's MultiFilletFactory over
// SOLID EDGES — KIWI has no solid fillet at all and does not pretend to.
//
// Curve corners are filleted by `modify-contour`, whose gizmo carries a
// FILLET-ALL magnitude (ModifyContourGizmo.ts:14 —
// `new FilletCornerGizmo("modify-contour:fillet-all", editor, true)`, bound to
// `d` inside the command, default-keymap.ts:169-171) plus one per-corner gizmo.
// modify-contour has NO top-level chord in Plasticity's keymap; it is reached
// from a selection.  So the VERB is ported and the KEY is KIWI's own — bare B,
// because B is Plasticity's fillet letter and nothing in KIWI wants it (the
// existing "Bevel Edge" is a solid op reached from the palette and the face
// verb strip, and it keeps its unbound row).  The kiwi_keymap.h audit records
// the SameTargetname 36121 displacement that buys it.
//
// The MATH is ContourFilletFactory.ts, read and matched rather than invented:
//
//   * ContourFilletFactory.ts:44-47 sizes the radius array as
//         fillNumber = contour.GetSegmentsCount() - (contour.IsClosed() ? 0 : 1)
//     A chain of n points has n segments closed and n-1 open, so that is n
//     fillable corners closed and n-2 open — i.e. EVERY vertex of a closed chain
//     and every INTERIOR vertex of an open one.  KIWI uses exactly that set.
//   * ContourFilletFactory.ts:53-72 (`cornerAngles`): when CONTROL POINTS are
//     selected, only THOSE corners are filleted; with none selected, all of them.
//     KIWI has the same distinction for free, because the construction selection
//     addresses points (KCONSEL_POINT, kiwi_conselect.h): select the chain in
//     Object/Edge mode and every corner rounds; select individual anchors in
//     Point mode (1) and only those do.  That is the per-corner control the
//     round's brief said could be deferred — it is not deferred, it falls out of
//     a selection granularity that already exists.
//   * The keymap's `fillet-all` is ONE RADIUS for every chosen corner
//     (FilletCornerGizmo constructed with `all` = true).  KIWI ships that: one
//     scalar, applied to every chosen corner, each corner CLAMPED to what it can
//     physically take.  Per-corner radii would need a per-corner gizmo, which is
//     a separate feature and is not half-shipped here.
//
// ── THE PER-CORNER CLAMP (why a fillet never mangles a chain) ───────────────
// A fillet of radius r at a corner whose interior angle is θ consumes a TANGENT
// LENGTH t = r / tan(θ/2) along each of the two adjacent edges.  If two corners
// share an edge and both take more than half of it, the two arcs cross and the
// chain folds through itself.
//
// So each corner's radius is clamped to what HALF the shorter adjacent edge can
// pay for: t_avail = 0.5 * min(|BA|, |BC|), and r_corner = min(r,
// t_avail * tan(θ/2)).  Half is used even at an open chain's free ends, where the
// full edge is actually available, because one rule that is always safe beats two
// rules that differ by which end you are at.  A corner too shallow (nearly
// straight) or too sharp (a spike) is SKIPPED rather than approximated — its
// vertex passes through untouched.
//
// The consequence, stated plainly: TYPING A HUGE RADIUS DOES NOT FAIL, it
// saturates.  Every corner rounds as much as its own edges allow, which is what a
// user dragging outwards expects to see, and the HUD says when clamping is
// happening.
//
// ── FILLET REPLACES; OFFSET ADDS ───────────────────────────────────────────
// ContourFilletFactory carries an `originalItem` (ContourFilletFactory.ts:74-76),
// which is how a Plasticity GeometryFactory says "my result REPLACES this".
// Contrast OffsetCurveCommand, which only removes the source from the SELECTION
// and leaves it in the database (kiwi_offset.h).  KIWI keeps both behaviours:
// fillet removes the source object and adds the rounded one; offset adds a second
// object next to the first.
//
// ── THE RESULT IS A POLYLINE, AND THAT IS DELIBERATE ───────────────────────
// The arcs are TESSELLATED into the resulting chain rather than stored as
// KCON_ARC objects.  kiwi_construct.h already made this call for the §16b spline
// and polygon tools ("a POLYGON is a closed KCON_POLYLINE of `sides` points and a
// SPLINE is a closed-or-open KCON_POLYLINE of its tessellation… a new kconType_t
// would be a sidecar format change bought for nothing"), and a filleted chain has
// the same property: it is one continuous curve, and the store has no
// arc-plus-line CONTOUR type to hold it in.  Density follows the store's own
// KCON_SEGS_PER_UNIT rule so a filleted corner is exactly as smooth as a drawn
// arc of the same radius.
//
// ── REFUSALS ───────────────────────────────────────────────────────────────
//   * the chain does not fit a plane (KiwiCon_ObjectPlane) — an arc off its own
//     plane is not an arc;
//   * fewer than one fillable corner;
//   * the result would exceed KCON_MAX_POINTS (the store's cap);
//   * every chosen corner clamped to nothing (a radius below KFIL_MIN_RADIUS
//     everywhere), which is a no-op rather than an edit.
// A refusal shows the HUD RED (§19) and Commit becomes Cancel.
//
// ── UNDO ───────────────────────────────────────────────────────────────────
// CONSTRUCTION domain (kiwi_construct.h ruling 2 / kiwi_undo.h): one
// KiwiCon_UndoPush at Commit, immediately before the RemoveAt + Add pair, and
// only after the result has been built and validated — kiwi_trim.cpp's "nothing
// can fail after the push" rule, which is why the whole chain is computed into a
// buffer during the gesture and merely written at the end.  The push mints one
// ticket in the unified journal, so one Ctrl+Z restores the sharp chain.
//
// The gesture itself mutates nothing (the preview is drawn, never stored), so a
// cancelled fillet leaves the store byte-identical.
// ─────────────────────────────────────────────────────────────────────────────

class KiwiEditorCommand;

#define KFIL_MIN_RADIUS      0.5f      // below this a corner is left sharp
// Arc density per corner.  KCON_SEGS_PER_UNIT is the store's rule; these are the
// per-corner floor and ceiling, chosen so a filleted 32-point rectangle stays far
// inside KCON_MAX_POINTS.
#define KFIL_SEGS_MIN        2
#define KFIL_SEGS_MAX        16
// Corner angles outside this band are skipped: within KFIL_FLAT_DOT of straight
// there is nothing to round, and within it of a full reversal there is no room.
#define KFIL_FLAT_DOT        0.9995f

// §3 palette predicate: a selected construction chain with at least one corner.
bool KiwiFillet_CanFillet();

void KiwiFillet_RegisterCommands();
KiwiEditorCommand *KiwiFillet_CommandForId( int commandId );

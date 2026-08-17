#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
#include "kiwi_construct.h"   // KIWI-UX (CLEANUP, B-9): KCON_WELD_2D
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_offset.h — ROUND J: OFFSET CURVE.
//
//     KIWI_CMD_OFFSET_CURVE  34060   MODAL, modern key O
//
// A parallel copy of a construction chain at a dragged or typed distance.  With
// the §23 extruder next door this is the wall tool: draw the inside face of a
// room, offset it by the wall thickness, join the two loops, extrude.
//
// ── THE PLASTICITY SOURCE ───────────────────────────────────────────────────
// `o` -> `command:offset-curve` (default-keymap.ts:268), whose gizmo has ONE
// scalar, `gizmo:offset-curve:distance` bound to `d` inside the command
// (default-keymap.ts:133-135).  The command itself is
// src/commands/curve/OffsetCurveCommand.ts:
//
//   * it takes `selected.curves.first` (a FACE or a set of EDGES are the other two
//     inputs it accepts — OffsetCurveCommand.ts:8-21);
//   * it feeds the factory `this.editor.activeViewport?.constructionPlane`
//     (line 15) — the construction plane is what decides "sideways" when the
//     curve itself does not;
//   * it runs ONE gizmo for the distance and commits;
//   * THE ORIGINAL SURVIVES.  The tail only removes the source from the
//     SELECTION (`selected.removeCurve(curve)`, line 34) and then selects the new
//     curve.  Offset ADDS; it does not replace.  (Contrast Fillet, which
//     genuinely does replace — kiwi_fillet.h.)
//
// KIWI ports the CURVE arm.  The FACE arm (offset a solid face's boundary loop
// into a new curve) is NOT shipped: it needs a face-winding → construction-object
// converter that does not exist yet, and half of it would be a worse feature than
// none.  It is a clean later addition — the whole of this file's 2D core would be
// reused unchanged.
//
// ── WHAT IT OFFSETS, AND THE PLANE IT OFFSETS IN ────────────────────────────
// The source is the FIRST selected construction object (kiwi_conselect.h), at any
// granularity — selecting a point or a segment of a chain still names the chain,
// which is the same "framing the owner" rule kiwi_focus.h uses.
//
//   LINE / POLYLINE / RECT   the real work: a 2D polygon offset (below).
//   CIRCLE / ARC             parametric, so the offset is `radius += d` with the
//                            centre, plane and sweep untouched.  Exact, not
//                            tessellated — which is the entire reason
//                            kiwi_construct.h keeps those two parametric.
//
// THE PLANE.  Offsetting is a 2D operation and needs one:
//   1. KiwiCon_ObjectPlane (kiwi_construct.h) — the object's own Newell fit.  This
//      is the answer for anything with three non-collinear points.
//   2. A STRAIGHT chain (a 2-point line, or a polyline whose points are collinear)
//      does not determine a plane, and Newell correctly refuses it.  The ACTIVE
//      CONSTRUCTION PLANE's normal then decides which way "sideways" is — exactly
//      the fallback Plasticity feeds its factory (OffsetCurveCommand.ts:15) — and
//      that normal is orthogonalised against the chain direction so the built
//      plane genuinely contains the line.  A chain running ALONG the construction
//      plane's normal has no sideways at all and is refused with a message that
//      says which plane to change.
//
// ── THE 2D OFFSET, AND WHAT IT REFUSES ──────────────────────────────────────
// Per edge: shift the edge's line by `d` along its normal.  Per joint: intersect
// the two shifted lines — a MITER join.  Two escapes:
//
//   * NEARLY PARALLEL EDGES (the intersection is at infinity or the corner is a
//     180-degree spike) — the joint takes the two shifted endpoints instead.
//   * THE MITER LIMIT.  A sharp corner's miter length grows without bound as the
//     corner closes; past KOFF_MITER_LIMIT times |d| the joint degrades to the
//     same two-point BEVEL.  This is the standard stroke-join treatment and it is
//     what keeps an acute corner from firing a spike across the map.
//
// SIGN, AND WHAT THE DRAG ACTUALLY MAPS.  For a CLOSED loop positive `d` is
// OUTWARD (the loop's own winding decides which side that is, via
// KiwiRegion_SignedArea).  For an OPEN chain there is no outward, so positive is
// to the RIGHT of travel.  The user does not have to know either convention,
// because the drag reads the cursor's own SIGNED SIDE of the chain — move the
// cursor to the outside and the offset goes outside.
//
// It is a DELTA from where the cursor was when the command started, not an
// absolute "the curve passes under your cursor".  That is the house mapping (every
// other dragged scalar in this layer works that way — kiwi_bevel.cpp,
// kiwi_extrude.cpp) and it is what makes Rebase possible: a PAUSED -> HOT edge
// re-latches the origin biased by the current value, so resuming does not zero the
// offset or teleport it by however far the cursor wandered while parked.
// In the common case the two readings coincide anyway: you invoke O right after
// clicking the chain, so the latched start is ~0 and the offset does track the
// cursor.
//
// REFUSALS — and per the round's brief this file refuses rather than mangles:
//   * the offset chain SELF-INTERSECTS.  For a closed loop that is asked with the
//     §8 toolkit's own KiwiRegion_SelfIntersects, so an offset loop that would be
//     rejected as a REGION is rejected here first, in one spelling.  For an open
//     chain the same non-adjacent-crossing test is applied locally (the region
//     helper reads its input as a ring, which an open chain is not).
//   * a closed loop whose WINDING FLIPPED — an inward offset that has eaten past
//     the shape's own medial axis.  Signed area is the cheap, total test for it.
//   * a circle / arc whose radius would reach KOFF_MIN_RADIUS or below.
//   * a result with fewer than two points, or more than KCON_MAX_POINTS.
// A refusal shows the HUD RED (§19) and Commit becomes Cancel: nothing is written.
//
// ── UNDO ────────────────────────────────────────────────────────────────────
// CONSTRUCTION domain, so the ported brush bracket is not involved at all
// (kiwi_construct.h ruling 2 / kiwi_undo.h): the store's own snapshot, pushed
// exactly once, at Commit, immediately before the single KiwiCon_Add — and only
// after every refusal test has passed, so a pushed snapshot is always a snapshot
// of a store that is about to change.  That is kiwi_trim.cpp's lesson ("nothing
// can fail after the push") applied to a command with one mutation instead of
// three.  The push mints one ticket in the unified journal, so Ctrl+Z takes the
// offset back and Ctrl+Y puts it there again.
//
// The gesture itself mutates NOTHING — the preview is drawn through kiwi_lines,
// never stored — so a cancelled offset has no snapshot to roll back and leaves
// the store byte-identical.
// ─────────────────────────────────────────────────────────────────────────────

class KiwiEditorCommand;

// The miter join degrades to a bevel past this multiple of |distance|.  4 is the
// usual stroke-join default and corresponds to corners sharper than about 29
// degrees.
#define KOFF_MITER_LIMIT     4.0f
// Below this the gesture is a no-op rather than a zero-width duplicate.
#define KOFF_MIN_DIST        0.5f       // == KCON_PLANE_FIT_DIST, the layer's one slop
// A circle may not be offset down to (or through) a point.
#define KOFF_MIN_RADIUS      1.0f
// Two plane-space points closer than this are the same point (duplicate cull).
// KIWI-UX (CLEANUP, B-9): one number, KCON_WELD_2D (kiwi_construct.h, included
// at the top) —
// kiwi_fillet.cpp spelled the same value as KFIL_WELD_2D for the same question.
#define KOFF_WELD_2D         KCON_WELD_2D

// §3 palette predicate: exactly one offsettable construction object is selected.
bool KiwiOffset_CanOffset();

void KiwiOffset_RegisterCommands();
KiwiEditorCommand *KiwiOffset_CommandForId( int commandId );

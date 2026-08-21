#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_construct.h — RADIANT_UX_DESIGN §7 (construction geometry store +
// persistence), §16 (construction planes) and the Phase-4 drawing tools
// (items 21 + 22).  Region detection lives next door in kiwi_region.h; region
// extrusion in kiwi_extrude.h.
//
// ── SCOPE RULING 1: construction geometry is NOT part of selection_t ─────────
// A construction object is editor-only scaffolding, not map data, and it has no
// brush behind it — so it cannot be named by a `sel_item_t` (kiwi_selection.h
// DESIGN NOTE 1: every item is (selbrush_t*, index)).  Rather than widen the
// typed selection for a thing that owns no brush, v1 keeps construction geometry
// in ITS OWN store with ITS OWN hover/pick INSIDE the drawing tools, and the
// extrude command picks a region directly from the cursor ray.  Consequences,
// all deliberate:
//   * the 1-5 selection modes never see construction objects,
//   * box select never touches them,
//   * G/R/S never move them (redraw instead; v1),
//   * Delete never deletes them — "Construction: Clear All" does.
// Unifying them with the typed selection means giving sel_item_t a second
// addressing mode, which is a later phase's decision, not this one's.
//
// ── SHAKEOUT F: THE RULING STANDS, THE CONSEQUENCES DO NOT ──────────────────
// USER REPORT: "Using 2(edge) you can't select lines.  You should be able to
// select lines like this and join them.  Lines aren't selectable with any mode."
// The four bullets above were the price of the ruling, and three of them turned
// out to be unaffordable.  What did NOT change is the ruling itself: sel_item_t
// still has exactly one addressing mode and construction geometry still never
// enters selection_t or Sel_SyncToLegacy.  What changed is that there is now a
// SECOND, PARALLEL, KIWI-owned selection list for it (kiwi_conselect.h), so:
//   * the 1-5 modes DO reach construction objects — Point picks anchors, Edge
//     picks segments, Face/Object/All pick whole objects;
//   * box select DOES include them, by the same containment/crossing rules;
//   * G moves them (a construction ARM inside the existing Move command, so
//     there is still exactly one "Move" in the editor);
//   * Delete deletes them, but ONLY when nothing is selected brush-side — the
//     arbitration is in KiwiUX_KeyFunnel and 33003 is untouched.
// R and S still do not: rotating and scaling scaffolding needs a pivot story that
// nothing has asked for yet.
//
// THE ONE EXCEPTION, and the reason the store is worth having at all:
// construction points and segments DO join the SnapManager (kiwi_snap.cpp v3).
// Scaffolding you cannot snap to is decoration.
//
// ── SCOPE RULING 2: construction edits are NOT on the legacy undo stack ──────
// The ported undo (undo.cpp) is BRUSH-SNAPSHOT based: a record holds cloned
// brush_t defs and cloned entity defs, and Undo_Undo rebuilds the live lists
// from them.  There is no room in it for a thing that is neither.  So the store
// carries its OWN small undo stack — one whole-store snapshot per completed tool
// commit, KCON_UNDO_DEPTH deep.
//   * Ctrl+Z WHILE A CONSTRUCTION TOOL IS ACTIVE pops the construction stack
//     (the tool's KeyDown consumes it, so the legacy Ctrl+Z in
//     Radiant_PreTranslateMessage never sees the key).
//   * Ctrl+Z with no construction tool running is the LEGACY undo, unchanged.
// Logged in RADIANT_KNOWN_ISSUES ("UX overhaul") because a split undo is exactly
// the kind of thing that surprises a user once and then never again.
//
// ── SCOPE RULING 3 (SHAKEOUT H): points are stored in WORLD SPACE ───────────
// THE ORIGINAL RULING WAS: "every object carries its own kconPlane_t and stores
// its points as 2D (u,v) pairs in that basis", on the argument that coplanarity
// is what makes a loop a REGION (§8) and a prism (§23), and that in plane space
// coplanarity is INTRINSIC and therefore cannot be lost.
//
// IT FELL, and the reason is worth writing down because it is the whole shape of
// this round.  USER REPORT, verbatim: "the lines are only in 2D.  It's not
// possible, even with an aggressive camera angle, to get them to go up or down on
// Z." and "When using the line tool, snapping to corners on a brush is buggy."
// Those are ONE bug, and plane space IS the bug:
//
//   * a tool placed every point by ray∩plane, so no gesture could ever leave the
//     plane — 2D lines, by construction;
//   * worse, a SNAP was PROJECTED onto the plane before being stored
//     (KiwiCon_WorldToPlane drops the normal component by definition).  Snap to a
//     brush corner 64 units above the working plane and the point lands at that
//     corner's SHADOW, not at the corner.  The snap marker said one thing and the
//     stored point was somewhere else, which is exactly "snapping to corners is
//     buggy".
//
// Plasticity does not do this, and its own source says so in as many words: a
// picked point's position is the SNAP'S OWN project() — the stored vertex for a
// PointSnap (plasticity/src/editor/snaps/PointSnap.ts:15-19 returns
// `this.position` verbatim), the kernel's near-point for a curve/face snap — and
// the construction plane is a LAST-RESORT candidate at priority 5, below points
// (1), curves/axes (2) and faces (3) (src/editor/snaps/SnapPicker.ts:136-156),
// which it takes only when nothing else answered
// (src/editor/snaps/PointPickerSnapPicker.ts:68-88, "the construction plane can
// either act just as a fallback … OR it can act like a real object").
//
// SO: `pts` is now 3 FLOATS PER POINT, IN WORLD SPACE, for LINE / POLYLINE /
// RECT.  CIRCLE and ARC keep a STORED plane and stay parametric — they are
// intrinsically planar, a circle off its own plane is not a circle, and nothing
// about them was ever the complaint.
//
// WHAT PAYS FOR COPLANARITY NOW.  It is DERIVED, per object, on demand:
// KiwiCon_ObjectPlane fits a plane through the points (Newell's method, which is
// area-weighted and so is stable for a nearly-degenerate loop) and REJECTS the
// fit when any point is further than KCON_PLANE_FIT_DIST off it.  A REGION (§8)
// is then "a closed chain whose points fit one plane", at ANY orientation, and
// the extruder still receives a plane plus a 2D loop — projected into the fitted
// plane, with the deviation already bounded by the fit — so §23 is untouched.
// An object that does NOT fit a plane is perfectly legal: it just never becomes a
// region.  That is the honest trade, and it is the one Plasticity makes too (its
// PlanarCurveDatabase silently declines to fragment a non-planar curve —
// src/editor/curves/PlanarCurveDatabase.ts:46-47).
//
// ── CIRCLES AND ARCS ARE PARAMETRIC ─────────────────────────────────────────
// A circle/arc stores centre + radius + start/end angle (degrees, CCW about the
// plane normal) and is TESSELLATED on demand for drawing, snapping, region
// detection and extrusion.  One tessellation rule, in one place:
//
//     segs = clamp( round( radius * KCON_SEGS_PER_UNIT ), KCON_SEGS_MIN, KCON_SEGS_MAX )
//
// i.e. 8 segments at radius 32, 32 at radius 128, 64 at radius 256 and above.
// An arc gets the same density pro-rata over its sweep, never below 2.
// The profile cap in §23's extrusion is KEXT_MAX_PROFILE (128, via KREG_MAX_LOOP
// — kiwi_extrude.h:139, kiwi_region.h:228); KCON_SEGS_MAX (64) is deliberately
// HALF of it, so the densest circle this store can produce is comfortably inside
// the largest profile the extruder accepts.  KIWI-UX (CLEANUP, A-44)
//
// ── PERSISTENCE (§7, decision D-5) ──────────────────────────────────────────
// A SIDECAR file `<mapname>.kiwi`, written next to the .map, never into it —
// stock compilers and stock Radiant must keep loading our maps untouched.
// Versioned line-based text, written tmp+rename so a crash mid-write cannot
// destroy the previous sidecar.  A corrupt or unknown-version sidecar prints one
// console warning and is IGNORED: it must never crash and never block a map load.
// Format sample and the parser's rules are in kiwi_construct.cpp.
//
// SHAKEOUT H bumps the format to KIWI2 (world points, `wpt x y z`).  KIWI1 files
// STILL LOAD: the `pt u v` + `plane` parser path is kept verbatim and the points
// are converted to world at the object's `end`.  Only KIWI2 is ever WRITTEN, so a
// map saved by this build cannot be read by an older one — which is the same deal
// every sidecar version bump makes, and the sidecar is editor-only scaffolding.
//
// ROUND W adds two keywords and does NOT bump the version, for the same reason
// ROUND U's `hidden` did not:
//   `congroup N "name"`   at the TOP LEVEL, one per declared construction group;
//   `group N`             inside an object block, naming the group it belongs to.
// Back-compatible in BOTH directions by construction — an older reader skips every
// keyword it does not know (the load loop's tail) and treats a top-level line
// outside an object block as noise, and this reader defaults `group` to -1 when the
// line is absent, which is exactly what every KIWI2 file written before ROUND W is.
//
// ── KEYS ────────────────────────────────────────────────────────────────────
// NOTHING here binds a key this phase.  The obvious candidates all collide with
// audited classic bindings (L = none free but shares the letter-key policy,
// C = Clone, P = ..., A = SelectAll) and the modern profile's letter budget is
// already spent on G/R/S/F.  Everything is reachable from the command palette
// and the View-menu "Construct" submenu, which §11 says is enough.
//
// SHAKEOUT C REVISION: still true — no DRAWING TOOL binds a key.  What the modern
// profile now binds is ONE chord, Shift+A, to the §16b ADD MENU (kiwi_addmenu.h),
// which lists every creation command with its own live binding.  That is the
// Plasticity inventory reached through a Blender-shaped door, and it costs the
// letter budget one chord instead of eight.
//
// SHAKEOUT F REVERSAL: the user rejected that trade outright — "the shift-A menu
// is unacceptable.  Shift-A is for LINES.  Start the line tool immediately and the
// other ones are on other keys."  So the modern profile now binds Plasticity's own
// eight creation chords DIRECTLY, and the letter budget pays for all eight:
//   Shift+A Line · Shift+S Spline · Shift+Q Rectangle · Shift+C Circle ·
//   Shift+W Box · Shift+V Box (centre) · Shift+X Cylinder · Shift+Z Sphere
// (ROUND AF gave Shift+V to the centre BOX and moved the centre rect to Alt+V;
//  ROUND AG item 4 swapped Shift+C and Shift+W on the user's own directive —
//  see kiwi_keymap.cpp at the SHIFT+C IS THE CIRCLE fence.)
// Five compiled-in occupants were displaced with the house two-step; the complete
// chain table, with every key's full occupancy, is in kiwi_keymap.h.  The add menu
// survives UNBOUND (palette + KIWI panel), so nothing became unreachable.  The
// remaining tools — Polyline, Arc, Circle (2-point), Polygon — keep no chord and
// stay on the add menu and the palette, which is what §11 says is enough.
// ─────────────────────────────────────────────────────────────────────────────

#include <string>   // ROUND X, ITEM 10 — kconObject_t::name
#include <vector>

struct ray_t;                       // kiwi_pick.h
struct pick_result_t;               // kiwi_pick.h
struct selbrush_t;                  // qe3.h:31 (the brush INSTANCE / list node)
class  KiwiEditorCommand;           // kiwi_command.h

// ── tessellation + budgets ───────────────────────────────────────────────────
#define KCON_SEGS_PER_UNIT   0.25f  // 32 segments at radius 128 (see the note above)
#define KCON_SEGS_MIN        8
#define KCON_SEGS_MAX        64
// ROUND AF, ITEM 7: the floor for a USER-TYPED side count.  Deliberately 3 rather
// than KCON_SEGS_MIN (8): the automatic rule is choosing a smoothness and 8 is the
// right floor for that, but a typed count is a request and is honoured much lower.
// KIWI-UX (CLEANUP, A-46) — WHAT A TYPED 3 ACTUALLY PRODUCES.  On the CIRCLE/ARC
// path this floor is not the last word: CircleSegsFor clamps to it and then hands
// the count to CardinalSegs (kiwi_construct.cpp:218-226), which opens with
// `if ( n < 4 ) n = 4` for round AG item 5(b)'s cardinal rule.  So typing 3 into a
// circle yields a SQUARE, not a triangle, and the HUD reports the rounded 4.  A
// three-sided POLYGON is reachable — the polygon tool clamps to
// KCON_POLY_SIDES_MIN and never goes through CardinalSegs.
// The ceiling stays KCON_SEGS_MAX (64), half of the KEXT_MAX_PROFILE (128) cap an
// extruded profile is held to.
#define KCON_SIDES_MIN       3
#define KCON_MAX_POINTS      256    // per object; a polyline that long is a mistake
#define KCON_UNDO_DEPTH      32     // whole-store snapshots (ruling 2)
#define KCON_DRAW_SEGMENTS   1600   // kiwi_lines budget for the whole construction pass
#define KCON_ANGLE_STEP      15.0f  // §6 SNAP_ANGLE increment, degrees, in-plane
#define KCON_JOIN_PIXELS     10.0f  // "click near the first point" closes a polyline

// ── KIWI-UX (CLEANUP, A-72): THE DOUBLE-CLICK GRAMMAR ───────────────────────
// The window and the movement slop KiwiDrawTool::Click uses to read two clicks as
// one double-click.  400 ms is a system UI constant in spirit — GetDoubleClickTime()
// is the value the shell would give — and it sits here with the other pixel
// tolerances rather than as a bare literal in the click path.
#define KCON_DBLCLICK_MS     400u
#define KCON_DBLCLICK_SLOP_PX 4     // per axis, in pixels

// ── KIWI-UX (CLEANUP, A-71): THE MATHS CONSTANTS, ONE SPELLING EACH ─────────
// kiwi_construct.cpp used to carry seven hand-typed spellings of four constants,
// two of which disagreed with their own twin in the same file at a different
// precision.  These are the ONE spelling of each, and the more precise of the two
// literals wins wherever they differed.  Per the project's rule there are no
// precision variants: no _F/_DBL pairs, one name, one value.
#define KCON_PI              3.14159265358979f
#define KCON_TWO_PI          6.283185307179586f
#define KCON_DEG2RAD         0.01745329252f
#define KCON_RAD2DEG         57.29577951308232f

// KIWI-UX (ROUND AA, ITEM 6.3): the half-extent, IN PIXELS AT THE ANCHOR, of the
// bounded working-plane indicator a live drawing tool draws (KiwiDrawTool::
// DrawWorkingPlane).  Big enough to read the plane's orientation at a glance,
// small enough that it never reads as a piece of the map — the same "sized on
// screen, not in the world" rule every other accent in this layer follows.
#define KCON_PLANE_HALF_PIXELS  110.0f
// KIWI-UX (CLEANUP, A-72): the rest of that indicator's appearance, named.
// KCON_PLANE_FADE_MIN is the floor on Plasticity's grazing dot² fade below which
// the indicator is not drawn at all (it would be one bright smear along a screen
// row).  KCON_PLANE_CROSS_FRAC is the anchor cross's arm length as a fraction of
// the square's half-extent.
#define KCON_PLANE_FADE_MIN     0.06f
#define KCON_PLANE_CROSS_FRAC   0.18f

// ── SHAKEOUT H: the two WORLD-SPACE tolerances ruling 3 now needs ────────────
// KCON_PLANE_FIT_DIST is the max distance a point may sit off its object's
// fitted plane before the fit is refused.  It is DELIBERATELY the same number as
// kiwi_region.h's KREG_JOIN_DIST — "half a unit" is the one slop this layer admits
// anywhere, and two different half-units would be two things to argue about.  It
// REPLACES kiwi_region.h's old KREG_PLANE_DIST/KREG_PLANE_DOT pair outright: §8
// coplanarity is now a point-vs-plane question with exactly one tolerance.
#define KCON_PLANE_FIT_DIST  0.5f
// How close two 3D segments must come to count as CROSSING (the trim tool and
// the SNAP_INTERSECTION arm).  Tighter than the weld tolerance on purpose: a weld
// says "these two ends are the same end", an intersection says "these two lines
// genuinely meet", and the second deserves the stricter test.
#define KCON_ISECT_DIST      0.25f

// ── KIWI-UX (CLEANUP, B-9): THE PLANE-SPACE WELD, AND THE PREVIEW PALETTE ───
// "Two PLANE-SPACE points closer than this are the same point" — the tolerance
// the construction-plane drag commands use while walking a chain they are about
// to fillet or offset.  It was two constants at one value with one meaning:
// kiwi_fillet.cpp's KFIL_WELD_2D and kiwi_offset.h's KOFF_WELD_2D, in what are
// otherwise two copies of the same command (B-8).  Weld tolerances are the thing
// this codebase has been burned by most often, so it is one number now.
//
// It is DELIBERATELY NOT KiwiRegion_WeldFor: that one is grid-scaled and bounded
// by the finest edge, which is the right rule for deriving a REGION from user
// geometry.  This is a fixed plane-space epsilon inside one gesture's own solve,
// and switching it to the grid-scaled rule would be a behaviour change, not a
// consolidation — flagged here rather than folded in.
#define KCON_WELD_2D         0.01f

// §18: the construction-ROSE preview pair — one "this will commit", one "this is
// refused".  kiwi_fillet.cpp and kiwi_offset.cpp each spelled both literals; they
// are one palette entry, shared by the two commands that draw the same kind of
// preview over the same kind of geometry.  (kiwi_bevel.cpp is a DIFFERENT §18
// family — blue OK, its own red — and is deliberately not folded in here.)
extern const float KCON_PREVIEW_OK[3];
extern const float KCON_PREVIEW_BAD[3];
// ── ROUND K: the CONSTRUCTION LINE CLICKBOX ─────────────────────────────────
// USER DIRECTIVE: the construction segment click tolerance goes 6 px -> 10 px.
// A DEDICATED number rather than widening PICK_EDGE_PIXELS, because that macro is
// the BRUSH edge radius too and widening it would make brush edges out-pick brush
// faces at the same time — a construction line is a one-pixel-wide hairline with
// nothing behind it, which is not the same aiming problem as a brush edge.
// Used by every construction-segment pixel test in the editor, so hover (the snap
// query's arm 5), click (kiwi_conselect's pick), the plane-inheritance rung and
// the trim tool all agree; the MARQUEE has no pixel radius at all (it is a rect
// containment/crossing test) and so is consistent by construction.
#define KCON_LINE_PIXELS     10.0f
// ── ROUND R: the CLICK radius is WIDER THAN THE HOVER radius ────────────────
// USER REPORT, verbatim: "Lines are still hard to click."  Round K's 6 -> 10 was
// not enough, and the honest reason is that HOVER and CLICK are not the same
// aiming problem.  A hover radius that is too big is a nuisance — it lights up
// lines the user is not pointing at, and it feeds the plane-inheritance rung
// (KiwiCon_PlaneFromCursorConstruction) which then silently adopts a plane from a
// line the user was only passing over.  A CLICK radius that is too small is a
// FAILURE: the user aimed, pressed, and nothing happened.
//
// So they split.  KCON_LINE_PIXELS (10) stays the number for everything
// CONTINUOUS — the snap query's construction arm, the plane-inheritance rung, the
// trim and split previews — and KCON_CLICK_PIXELS (14) is the number for the one
// DISCRETE act, kiwi_conselect.cpp's KiwiConSel_PickAt.
//
// The ARBITRATION is unaffected and was re-checked rather than assumed
// (kiwi_boxselect.cpp ClickSelect): construction wins outright unless the brush
// pick is a VERTEX or an EDGE — an area hit (SEL_FACE / SEL_OBJECT) always loses
// to a construction line — so widening the radius cannot make a line lose to the
// brush FACE it is drawn on top of, which is exactly the reported picture.  Only
// the vertex/edge comparison is affected, and there the test is a pixel-distance
// compare, so a genuinely nearer brush edge still wins.
#define KCON_CLICK_PIXELS    14.0f

// ── §16b (shakeout C) the rest of the creation suite ────────────────────────
// The four new curve tools add NO store type and NO sidecar keyword: a POLYGON
// is a closed KCON_POLYLINE of `sides` points and a SPLINE is a closed-or-open
// KCON_POLYLINE of its tessellation.  That is deliberate — a new kconType_t
// would be a sidecar format change (and a version bump) bought for nothing,
// since neither shape needs to be re-editable in v1.
#define KCON_POLY_SIDES_MIN  3
// ROUND AF, ITEM 7: 32 -> 64, matching KCON_SEGS_MAX.  KIWI-UX (CLEANUP, A-44):
// KEXT_MAX_PROFILE is 128 (via KREG_MAX_LOOP), so 64 is deliberately half of the
// largest profile the region extruder will take — an n-gon drawn here is always
// extrudable, with room to spare.
#define KCON_POLY_SIDES_MAX  64     // == the extruder's comfort zone, half of KEXT_MAX_PROFILE
#define KCON_POLY_SIDES_DEF  6
// Spline tessellation: this many segments per CONTROL-POINT SPAN, the same
// "one rule in one place" shape the circle's KCON_SEGS_PER_UNIT has.  A span is
// short by construction (the user clicked both ends), so density is per-span
// rather than per-unit.  The total is clamped to KCON_MAX_POINTS by the emitter.
#define KCON_SPLINE_SEGS     8

// ── an object's own plane (§16) ──────────────────────────────────────────────
// `u`,`v`,`normal` are an ORTHONORMAL right-handed basis: u × v == normal.
struct kconPlane_t
{
    float origin[3] = { 0.0f, 0.0f, 0.0f };
    float normal[3] = { 0.0f, 0.0f, 1.0f };
    float u[3]      = { 1.0f, 0.0f, 0.0f };
    float v[3]      = { 0.0f, 1.0f, 0.0f };
};

enum kconType_t
{
    KCON_LINE = 0,      // 2 points
    KCON_POLYLINE,      // n points, `closed` says whether it wraps
    KCON_RECT,          // 4 points, axis-aligned in-plane, always closed
    KCON_CIRCLE,        // parametric, always closed
    KCON_ARC,           // parametric, never closed
    KCON_TYPE_COUNT
};

// One construction object.  EXACTLY ONE of the two payloads is meaningful:
// `pts` for LINE/POLYLINE/RECT, the centre/radius/angle block for CIRCLE/ARC.
//
// SHAKEOUT H — WHAT `plane` MEANS NOW, and it is two different things:
//   * CIRCLE / ARC   AUTHORITATIVE.  The shape only exists in that basis, and it
//                    is what the centre/radius/angle block is measured in.
//   * LINE/POLYLINE/RECT   A CACHE of the last successful fit through `pts`,
//                    maintained by the store (KiwiCon_Add / KiwiCon_NoteMutated
//                    refit).  `planeValid` says whether that fit succeeded.
//                    NOTHING may read it without checking, and the supported
//                    spelling is KiwiCon_ObjectPlane, which does the check.
struct kconObject_t
{
    kconType_t         type   = KCON_LINE;
    kconPlane_t        plane;
    bool               planeValid = false;        // see above; always true for CIRCLE/ARC
    std::vector<float> pts;                       // 3 floats per point, WORLD SPACE
    bool               closed = false;
    float              centre[2] = { 0.0f, 0.0f };// CIRCLE / ARC, plane space
    float              radius    = 0.0f;
    float              ang0      = 0.0f;          // degrees, CCW about `plane.normal`
    float              ang1      = 360.0f;
    // ── KIWI-UX (ROUND AF, ITEM 7): THE USER'S OWN SEGMENT COUNT ────────────
    // USER DIRECTIVE, verbatim: "I want another option for side-density on the
    // round shapes.  Sometimes you want more sides on the circles/cylinders.  Add
    // this to the [tab] typing menu for the tools that use segments like that."
    //
    // 0 means AUTO, i.e. the radius-driven rule this file has always used
    // (KCON_SEGS_PER_UNIT, clamped to [KCON_SEGS_MIN, KCON_SEGS_MAX]) — so every
    // circle drawn before this round, and every one the user does not touch the
    // field on, tessellates byte for byte as it did.  A positive value OVERRIDES
    // it, clamped to [KCON_SIDES_MIN, KCON_SEGS_MAX].
    //
    // It is stored PER OBJECT rather than as a tool setting because the count is a
    // property of the shape: two circles of the same radius may legitimately want
    // 8 sides and 48, and a later region / extrude reading the store has to get the
    // one the user chose for THAT circle.  ARC honours it pro-rata over its sweep,
    // exactly as the automatic rule does.
    //
    // Sidecar: written as `segs N` and omitted when 0, so a KIWI2 file that has
    // never seen a custom count is byte-identical to one this round did not write.
    // The loader's unknown-keyword arm means an older build reads a newer file and
    // simply falls back to AUTO, which is the correct degradation.
    int                segs      = 0;             // 0 = automatic (radius-driven)
    // ── ROUND U: HIDDEN (the H key's construction half) ─────────────────────
    // USER DIRECTIVE, verbatim: "We dont have a hide mechanic right now, you should
    // add that(H) and add (unhide all) in the search menu".
    //
    // HIDDEN MEANS INERT, not merely invisible.  A hidden object is skipped by the
    // draw pass, by both pick paths (click and marquee), by the snap scan and by
    // the §8 REGION derivation.  The last one is the only debatable member and it
    // is deliberate: scaffolding is hidden precisely BECAUSE it has done its job,
    // and a region silently held together by a line nobody can see is a region
    // nobody can fix.  "Hidden = it is not there" is one rule; "hidden = it is not
    // there except for the six places it still is" is six.
    //
    // It is NOT a selection state: hiding CLEARS the selection of what it hid
    // (KiwiConSel_HideSelected), because an inert object you cannot see must not
    // still be the thing G moves.
    bool               hidden = false;
    // ── ROUND W: the OUTLINER's construction GROUP (kiwi_outliner.h) ────────
    // USER DIRECTIVE: "Support groups in this list ... When a group is created,
    // it has a group 'folder' in the list that you can drag to."
    //
    // The SOLID half of that directive is served by func_group entities, which is
    // real map data.  Construction geometry is editor-only and never reaches the
    // .map, so its groups cannot be entities and get ONE int here instead:
    //   -1  ungrouped (the default, and what every sidecar written before ROUND W
    //       means by saying nothing)
    //   >=0 the id of a group whose NAME lives in the store's own table
    //       (KiwiCon_GroupName below), written to the sidecar as `congroup`.
    // It is NOT a hierarchy: exactly one level, because that is what the directive
    // describes and because a tree would need a cycle rule the flat form cannot
    // have.  The id is a HANDLE, not an index — removing a group never renumbers
    // the others, so a `group N` in a sidecar always means the same group.
    int                group = -1;
    // ── ROUND X, ITEM 10: the OUTLINER's per-object NAME ────────────────────
    // USER DIRECTIVE: "Allow renaming of items in the outliner by double clicking
    // the item and typing."
    //
    // Plasticity names every node the same way — one map from node key to string
    // (plasticity/src/editor/Nodes.ts:84-98, `node2name.set(k, name)`), consulted
    // by the outliner row and by nothing else.  A construction object is editor-only
    // and never reaches the .map, so its name belongs in the sidecar beside it,
    // which is the closest thing this tree has to that map.
    //
    // EMPTY MEANS "no name": the row then falls back to the generated
    // "<type> <ordinal>" label, exactly as Plasticity falls back to
    // `${klass} ${id}` (Outliner.tsx:158).  It is NOT a handle and nothing keys off
    // it — two objects may carry the same name and that is the user's business.
    // std::string rather than a char array because this struct is already
    // vector-copied wholesale by the undo snapshots and `pts` is a vector too.
    std::string        name;
};

// A circle/arc: parametric, intrinsically planar, `plane` authoritative.
inline bool KiwiCon_IsParametric( const kconObject_t &o )
{
    return o.type == KCON_CIRCLE || o.type == KCON_ARC;
}

// ── plane math (the ONE spelling of it; region + extrude both use these) ─────
void KiwiCon_PlaneToWorld( const kconPlane_t &p, const float uv[2], float out[3] );
void KiwiCon_WorldToPlane( const kconPlane_t &p, const float world[3], float outUV[2] );
// Ray ∩ plane.  False when the ray is parallel to it or the hit is behind the eye.
// STRICT: no window, no clamp — this is the hit-TEST form (kiwi_region.cpp's
// point-in-cell arm needs a hit that is genuinely where the ray met the plane).
bool KiwiCon_RayPlane( const kconPlane_t &p, const ray_t &ray, float outWorld[3] );

// ── ROUND X, ITEM 2: THE BOUNDED FORM — ray ∩ a FINITE plane ────────────────
// USER DIRECTIVE, verbatim: "when at a relatively shallow angle, its impossible to
// snap/guide the line onto the xy plane as seen in the pic. Fix this. I would just
// like identical plasticity behavior."
//
// PLASTICITY'S CONSTRUCTION PLANE IS A FINITE QUAD, not an infinite plane.  It is a
// mesh — `PlaneSnap.geometry = new THREE.PlaneGeometry(10000, 10000, 2, 2)` and
// `snapper = new THREE.Mesh(PlaneSnap.geometry, ...)`
// (plasticity/src/editor/snaps/PlaneSnap.ts:12-14) — and the picker RAYCASTS that
// mesh (plasticity/src/editor/snaps/PointPickerSnapPickerStrategy.ts:25-28) before
// orthogonally projecting the hit onto the analytic plane (PlaneSnap.ts:87-93).
// There is NO near-parallel guard anywhere in their picking path; the finite quad
// IS the guard.  A grazing ray runs off the quad, `intersections.length === 0`, and
// the construction plane simply stops being a candidate.
//
// KIWI's `KiwiCon_RayPlane` was the infinite form with a `t > 1e6` sanity reject,
// so a ray 0.5° off the plane resolved a hundred thousand units away — the point
// left the drawn grid entirely, which is the picture the directive attaches — and a
// ray pointing infinitesimally the WRONG side of the plane failed outright, which
// dropped snap arms 7+8 and let the drawing tool place OFF its own plane.  Both
// halves are "impossible to snap the line onto the xy plane".
//
// This form fixes both by being finite:
//   * a forward hit is CLAMPED into the ±KCON_PLANE_REACH square in the plane's own
//     (u,v) — that square IS Plasticity's quad, in plane space, centred on the
//     plane origin exactly as theirs is;
//   * no forward hit (parallel, or the plane behind the eye) slides from the ray
//     origin's own plane projection along the ray's IN-PLANE bearing out to the same
//     square.  Plasticity drops the candidate here; KIWI cannot, because a drawing
//     tool with no plane point has nowhere to put the click at all — so it pins at
//     the quad edge, which is the last thing the user saw before Plasticity's quad
//     ran out.  Bounded and on-plane either way, which is the whole point.
// Returns false only for a degenerate plane.
bool KiwiCon_RayPlaneBounded( const kconPlane_t &p, const ray_t &ray, float outWorld[3] );

// The finite plane's half-extent, in world units, measured in plane space from the
// plane origin.  Plasticity's is 5000 (half of its 10000 quad) in a tree whose unit
// is roughly a metre; 16384 is the same gesture in CoD inches — one eighth of the
// engine's own ±131072 world bound (brush.cpp:1459) and comfortably outside any
// CoD4 playable area, so it can only ever bite on a hit that was already absurd.
#define KCON_PLANE_REACH     16384.0f

// ── KIWI-UX (ROUND AT, ITEM 2) — THREE NUMBERS THE PLANE LADDER NEEDS ────────
// USER REPORT, verbatim: "when clicking like this […] The lines should just hit
// the nearest major plane?  Why dont they?  Fix it.  The construction plane here
// should be flat."
//
// KCON_PLANE_PARALLEL — "these two planes are the same plane, differently
// offset".  Already the rung-0 test's literal (round Y); named here because the
// major-plane offset rule (ITEM 2b) asks the same question and one spelling of a
// tolerance is this project's rule.
//
// KCON_PLANE_FACING_MIN — a plane seen closer to EDGE-ON than this cannot be
// drawn on and must not be handed back by rungs 2 or 3.  cos(85 degrees): at 85
// degrees one pixel of cursor travel already slides the plane point by tens of
// units, and past 90 the point jumps behind the eye entirely.  This is not a
// robustness nicety — it is the mechanism by which a grazing face 4000 units away
// captured a sketch drawn in open space.
//
// KCON_FACE_PLANE_SLACK — how far OFF a selected face the cursor may be and still
// mean "draw on that face", as a multiple of the face's own largest extent.  1.0
// keeps round U's flow intact (the cursor drifting off the face by up to a whole
// face-width still draws on it) while refusing a face the user selected minutes
// ago and is nowhere near.
#define KCON_PLANE_PARALLEL     0.999f
#define KCON_PLANE_FACING_MIN   0.0872f    // cos(85 degrees)
#define KCON_FACE_PLANE_SLACK   1.0f
#define KCON_FACE_PLANE_MINGROW 16.0f      // world units, so a tiny face still has reach
// Build an orthonormal basis around `normal`, seeding `u` from `hintU` when that
// is usable (the face-winding's longest edge) and from the dominant world axis
// otherwise.  Normalises and orthogonalises; false for a degenerate normal.
bool KiwiCon_MakePlane( const float origin[3], const float normal[3],
                        const float *hintU, kconPlane_t *out );

// ── KIWI: THE CANONICAL BEARING BASIS ─────────────────
// USER REPORT, verbatim: *"the angle readout is way broken.  It only shows 178~
// degrees and changes very little.  It should be an absolute angle like in
// plasticity."*
//
// A bearing is only a number if its ZERO is a fact about the world.  Two zeroes
// were in use and neither is:
//   * the free 3D tools (line / polyline / spline) measured against the WORLD
//     GROUND frame — u = +X, v = +Y — whatever surface the user was drawing on.
//     Sketching on a wall that runs along world X therefore projects every
//     in-wall direction onto ±X: horizontal reads ~180, sweeping up the wall
//     barely moves it, and past 45 degrees of rise the out-of-plane gate drops
//     the readout entirely.  That is the report, exactly.
//   * a planar tool's plane takes its u from the picked FACE's winding (the
//     longest edge, PlaneFromFacePick), which is authoring order, not geometry.
//
// The canonical answer, and it is the one Plasticity uses for its construction
// planes: derive the in-plane axes from the NORMAL alone.
//   * a non-ground plane: u = normalize( worldZ x n ) — the in-plane HORIZONTAL,
//     so 0 degrees is level on the surface — and v = n x u, which points UP the
//     surface, so 90 degrees is straight up it;
//   * a ground-ish plane (|n.z| ~ 1, where there is no horizontal to single out):
//     u = world +X, v = n x u, i.e. +Y on a floor.
// Reference: plasticity/src/editor/snaps/PlaneSnap.ts:33-45
// (`avoidNumericalPrecisionProblems` — `n.dot(Z) === 1` takes the IDENTITY
// orientation, i.e. u = +X / v = +Y, and every other normal goes through
// `mat.lookAt(n, origin, Z)`, whose first column is normalize(Z x n)), with the
// tolerant degeneracy test from plasticity/src/commands/rect/RectangleFactory.ts:81
// (`Math.abs(normal.dot(Z)) > 1 - 10e-6`, which covers a downward normal too).
// World up is +Z in that tree as well (plasticity/src/editor/Editor.ts:46,
// `THREE.Object3D.DefaultUp = Z`), so the convention ports across unchanged.
//
// ── THE BASIS IS CANONICAL, THE *NORMAL* WAS NOT ─────────
// This helper is correct; what its callers once handed it was not: they latched
// the surface under each PLACED POINT (world ground in the void), and a bearing
// measured against a plane PERPENDICULAR to the one the segment lies in has
// dv = 0 by construction — atan2 pins to 0/180 and sweeping cannot move it, the
// same stuck signature in a different place.  Drawing into the void in a SIDE
// ortho view does exactly that: the point slides on a VERTICAL view-aligned
// plane while the bearing was read against the ground.  The normal now comes
// from the plane the snap ladder actually resolved the cursor onto this frame
// (kiwi_snap.h snap_result_t::havePlane), for the readout and the typed field
// alike, with the per-point latch kept only as the no-plane fallback.
//
// RANGE: [0, 360).  Plasticity has no absolute bearing readout to copy — its
// angle HUD is an unwrapped accumulated delta (MiniGizmos.ts:643-647 formats
// rad2deg(value) with no wrap) and only its rotate DIALOG imposes a range, the
// signed [-360, 360] scrubber at RotateDialog.tsx:20-22.  A compass reading is
// what the user asked for, so the readout is unsigned and wraps once; the typed
// field accepts any value and normalises it, so 45 and -315 are the same segment.
void KiwiCon_BearingBasis( const float normal[3], float outU[3], float outV[3] );
// Absolute bearing of `dir` in that basis, degrees in [0, 360).  False when the
// direction has no usable in-plane component (`inPlaneOut`, when given, receives
// the in-plane length so the caller can apply its own omission gate).
bool KiwiCon_BearingOf( const float normal[3], const float dir[3], float *outDeg,
                        float *inPlaneOut, float *outOfPlaneOut );
// The inverse: the unit in-plane direction a typed bearing names.
void KiwiCon_BearingDir( const float normal[3], float deg, float out[3] );
// Degrees wrapped into [0, 360).
float KiwiCon_WrapDeg( float deg );
// Grid-snap a plane-space point using the §17 modern spacing, measured ALONG the
// plane's own u/v.
//
// ── ROUND R: THE LATTICE IS ANCHORED AT THE WORLD ORIGIN, NOT AT p.origin ────
// USER REPORT, verbatim: "grid snapping completely breaks with custom grid
// spacing.  This needs to be fixed.  Yeah not sure why it broke but even deleting
// the whole scene doesn't fix it."  The screenshot's cplane readout was
// "67.0515 in, -17.7568 in, 16.5 in" at 5 in spacing — a snapped point that is
// nowhere near the 5-inch lattice on x or y, and exactly ON it (16.5 is the face
// height) on z.
//
// ROOT CAUSE, and it is one line of arithmetic.  This function used to be
//     uv = round( uv / s ) * s
// with `uv` measured FROM p.origin.  Converting back through
// KiwiCon_PlaneToWorld gives world = p.origin + u*uv0 + v*uv1, so every snapped
// world coordinate is congruent to p.origin's OWN coordinate modulo the spacing.
// An active plane whose origin is off-grid therefore poisons every later snap by
// its fractional part, forever.  And the active plane's origin is off-grid by
// construction on two documented paths:
//   * KiwiCon_SetPlaneFromCursorFace -> PlaneFromFacePick builds the plane at
//     `pick.point`, the RAY HIT on the face — an arbitrary point under the cursor;
//   * KiwiDrawTool::PushPoint re-seats the plane through EVERY placed point, and
//     a point placed on a geometry snap (a brush corner, a midpoint, a crossing)
//     is under no obligation to be on the grid at all.
// Both write it through KiwiCon_SetActivePlane, i.e. into the FILE-STATIC s_plane,
// which no map load and no scene clear ever reset — hence "even deleting the whole
// scene doesn't fix it".  (ROUND R resets it in KiwiCon_ClearAll as well; see
// there.  Both halves ship, because either one alone leaves the other's instance
// of the bug reachable.)
//
// THE RULE NOW: the lattice is anchored at the WORLD ORIGIN's projection onto the
// plane.  In code that is one extra term — snap the ABSOLUTE in-plane coordinate
// (world·u, world·v) rather than the origin-relative one — and it gives exactly
// what the directive asks for on both kinds of plane:
//   * an AXIS-ALIGNED plane has u and v on world axes, so `world·u` IS ±x (or ±y,
//     ±z) and snapping it is snapping the WORLD coordinate to the WORLD lattice.
//     No special case is needed for it; it falls out.  The NORMAL component is
//     untouched, so a working plane at z = 16.5 still places at exactly z = 16.5
//     with x and y on the world lattice — the reported case, answered.
//   * a TILTED plane snaps in its own u/v, as it must, but from a fixed anchor
//     instead of from wherever the tool happened to last put the origin, so the
//     same cursor position on the same plane always snaps to the same point.
void KiwiCon_SnapUV( const kconPlane_t &p, float uv[2] );

// ── SHAKEOUT H: the DERIVED plane (ruling 3) ────────────────────────────────
// Fit a plane through a WORLD point list (3 floats per point) by NEWELL'S METHOD
// — the area-weighted normal sum, which is the standard robust polygon-normal fit
// and, unlike a three-point cross product, does not collapse when the first three
// points happen to be nearly collinear.  The origin is the centroid, u is seeded
// from the longest edge so the basis lines up with the shape.
//
// FALSE when: fewer than 3 points, a degenerate (zero-area) normal, or ANY point
// lying further than KCON_PLANE_FIT_DIST off the resulting plane.  A caller that
// gets false is holding a genuinely non-planar chain and must not pretend
// otherwise — that is the whole point of the check.
bool KiwiCon_FitPlane( const float *worldPts, int count, kconPlane_t *out );

// THE supported way to ask an object for its plane.  Parametric objects answer
// from their stored (authoritative) plane; point objects answer from the cached
// fit and return false when there is not one.  Never read `o.plane` directly.
bool KiwiCon_ObjectPlane( const kconObject_t &o, kconPlane_t *out );

// Closest approach between two 3D segments (the ONE spelling of it: the trim tool
// and the SNAP_INTERSECTION arm both call this).  `outMid` gets the midpoint of
// the two closest points, `outTa`/`outTb` their parameters along each segment.
// Returns the distance between them; a caller decides what "crossing" means by
// comparing it against KCON_ISECT_DIST.
float KiwiCon_SegSegClosest( const float a0[3], const float a1[3],
                             const float b0[3], const float b1[3],
                             float *outTa, float *outTb, float outMid[3] );

// ── tessellated geometry (drawing, snapping, regions, extrusion) ─────────────
int  KiwiCon_VertCount   ( const kconObject_t &o );
bool KiwiCon_VertWorld   ( const kconObject_t &o, int i, float out[3] );
int  KiwiCon_SegmentCount( const kconObject_t &o );
bool KiwiCon_SegmentWorld( const kconObject_t &o, int i, float a[3], float b[3] );

// ── KIWI-UX (CLEANUP, PickLineAt): the ONE "which segment is under the cursor" ─
// The plain question, at the plain tolerance: scan every VISIBLE object's
// tessellated segments and return the nearest one within KCON_LINE_PIXELS, in
// camera-image pixels.  Returns false when construction geometry is hidden or
// nothing is close enough.  Any out-parameter may be null.
//
// It existed twice — kiwi_split.cpp's PickLineAt and kiwi_dupe.cpp's
// PickConstructionSegment, the second documented as "a DELIBERATE second copy"
// on the grounds that the two tools want different PAYLOADS.  They do; the
// payloads are what differ, and the twenty-line SCAN under them was identical
// including the hidden test, the behind-the-eye skip and the tolerance.  So the
// scan is here and each caller keeps its own payload on top.
//
// NOT KiwiConSel_PickAt, deliberately, and for the reason both copies already
// wrote down: that entry point answers at the granularity the current SELECTION
// MODE asks for, and in Object / Face / All mode it names a whole polyline with
// no segment.  These callers need ONE segment whatever mode the user is in.
bool KiwiCon_PickSegmentAt( int imgX, int imgY, float outA[3], float outB[3],
                            int *outObj, int *outSeg, float *outPixels );

// ── snap ANCHORS — the point-rank snap targets (kiwi_snap.cpp v3) ────────────
// Deliberately NOT the tessellated vertices: 64 rim points on one circle would
// bury every other snap candidate.  For LINE/POLYLINE/RECT the anchors ARE the
// defining points; a CIRCLE offers its centre plus the four quadrant points, an
// ARC its centre plus both ends.
int  KiwiCon_AnchorCount( const kconObject_t &o );
bool KiwiCon_AnchorWorld( const kconObject_t &o, int i, float out[3] );

// ── the store ────────────────────────────────────────────────────────────────
// ── ROUND R: KiwiCon_Add NORMALISES THE POINT LIST (the seam rule) ──────────
// USER REPORT, verbatim: "Yeah even when doing fresh lines on the grid it does a
// reject ? duplicate plane."  A CLOSED polyline in this store does NOT repeat its
// first point — the wrap is implicit (KiwiCon_SegmentCount) — and a stored object
// that DOES repeat it carries a zero-length profile edge into §23's prism builder,
// which then writes two side faces off one point and trips §19's V5 duplicate-plane
// gate.  Every producer already knew the rule; relying on all of them to keep
// knowing it is what made this reachable.
//
// So the STORE enforces it, at its single entry point, for LINE / POLYLINE / RECT:
//   * consecutive points closer together than KREG_JOIN_DIST collapse to one, and
//   * on a CLOSED object the last point collapses onto the first the same way.
// Parametric objects (CIRCLE / ARC) are untouched — they have no point list.
// KiwiCon_LoadSidecar runs the SAME normalisation on every object it parses, so a
// sidecar written by an older build cannot reintroduce the shape either.
int                 KiwiCon_Count();
const kconObject_t *KiwiCon_At( int index );
int                 KiwiCon_Add( const kconObject_t &o );   // index, or -1 when rejected
// KIWI-UX (CLEANUP, A-50): what every TOOL COMMIT should call.  Validates first
// and only then mints the undo record, so a refused object cannot leave an empty
// undo record behind (a Ctrl+Z that does nothing visible).  Same return as
// KiwiCon_Add; on -1 nothing was pushed and the refusal has already been printed.
int                 KiwiCon_AddWithUndo( const kconObject_t &o );
bool                KiwiCon_RemoveAt( int index );
// ROUND R: ALSO resets the active construction plane to ground XY through the
// world origin.  Map_NewMap (map.cpp) calls this on File->New AND at the head of
// every map load, which makes it the one place a "the editor is starting over"
// reset can live — and the working plane is exactly the kind of hidden global that
// has to be in it (see the KiwiCon_SnapUV note for what an off-grid plane origin
// does to every later snap, and for why the user could not clear it by deleting
// the scene).  Unconditional: it must run even when the store is already empty.
void                KiwiCon_ClearAll();

// KIWI-UX (CLEANUP, A-48): ClearAll PLUS both construction undo stacks and the
// unified journal (KiwiUndo_Reset).  Use this on a NEW DOCUMENT — the palette's
// "Construction: Clear All" verb must keep using KiwiCon_ClearAll, because it
// pushes its own undo record first.  Called from Map_NewMap's KIWI-UX fence.
void                KiwiCon_ResetForNewMap();

// ── ROUND U: the HIDDEN flag, read and written through the store ────────────
// Index-safe: an out-of-range index reads as HIDDEN, which is the answer that
// makes every caller's loop correct without a second guard.  (The per-object loops
// that already hold a `kconObject_t *` read `o->hidden` directly — this is for the
// callers that only have an index.)
bool                KiwiCon_Hidden( int index );

// ── ROUND X, ITEM 10: the per-object NAME (kconObject_t::name) ──────────────
// KiwiCon_Name returns "" for an out-of-range index and for an unnamed object;
// the outliner treats both the same way and falls back to its generated label.
// KiwiCon_SetName trims nothing and validates nothing except the length cap — a
// name is user text — but it DOES refuse a name containing a double quote,
// because the sidecar's name syntax is quote-delimited (KiwiCon_SaveSidecar).
const char         *KiwiCon_Name( int index );
void                KiwiCon_SetName( int index, const char *name );
#define KCON_NAME_MAX 64
// Both push NO undo record of their own — the CALLER brackets, because a hide is
// one act with the selection change that goes with it (KiwiConSel_HideSelected).
void                KiwiCon_SetHidden( int index, bool hidden );
bool                KiwiCon_HasHidden();
// Returns how many were revealed; brackets its OWN store-undo snapshot, because it
// is reached as a standalone palette verb with nothing else to bracket it.
int                 KiwiCon_UnhideAll();

// ── ROUND W: the GROUP field + the group-name table ─────────────────────────
// See kconObject_t::group for what a group id means.  All of these are cheap
// (the table is a short vector) and none of them pushes an undo snapshot — the
// CALLER brackets with KiwiCon_UndoPush, because a regroup is one act with
// whatever selection change came with it, exactly as the hide pair works.
//
// Index-safe: an out-of-range index reads as UNGROUPED and writes are ignored.
int         KiwiCon_Group   ( int index );
void        KiwiCon_SetGroup( int index, int group );
// Mint a new group id and name it.  `name` may be NULL/empty -> "Group N".
int         KiwiCon_NewGroup( const char *name );
// The declared groups, in creation order.  A group with no members still exists
// (that is what makes an empty folder a legal drag target).
int         KiwiCon_GroupCount();
int         KiwiCon_GroupIdAt( int i );
bool        KiwiCon_GroupExists( int group );
const char *KiwiCon_GroupName( int group );          // "" for an unknown id
void        KiwiCon_SetGroupName( int group, const char *name );
// Dissolve: every member goes back to ungrouped and the id stops existing.
bool        KiwiCon_RemoveGroup( int group );
// How many objects name this group (the folder row's count).
int         KiwiCon_GroupMemberCount( int group );

// ── SHAKEOUT F: in-place edit access ────────────────────────────────────────
// The construction-selection layer (kiwi_conselect.h) MOVES objects, which means
// writing a live object's plane origin — the store had no spelling for that, only
// add and remove.  Deliberately NOT a general "give me the vector": the returned
// pointer is valid until the next Add/RemoveAt/ClearAll/UndoPop and the caller
// MUST call KiwiCon_NoteMutated() when it is done, because the generation counter
// is what tells kiwi_region a loop may have changed shape.
//
// SHAKEOUT H: NoteMutated also REFITS every point object's cached plane, which is
// why it is not optional and why nothing may cache `o.plane` across a mutation.
kconObject_t       *KiwiCon_MutableAt( int index );
void                KiwiCon_NoteMutated();

// Bumped on every store change.  kiwi_region.cpp re-derives its regions when it
// sees a new value; nothing else may cache across it.
unsigned KiwiCon_Generation();

// ── the store's own undo (ruling 2, REVISED in shakeout I) ──────────────────
// Push BEFORE mutating.  Pop restores the last pushed snapshot; RedoPop puts it
// back.  The DEPTH limit and the snapshots are still this file's; the ORDER is
// not — see kiwi_undo.h.
//
// SHAKEOUT I, and it retires the second half of RULING 2 above: KiwiCon_UndoPush
// now mints a ticket in the UNIFIED journal and KiwiCon_UndoPop/RedoPop are
// driven BY that journal, so "Ctrl+Z means the construction stack inside a
// drawing tool and the legacy stack outside one" is gone.  There is one Ctrl+Z,
// it pops whichever step is genuinely newest, and there is a construction REDO
// for the first time.  Ruling 2's FIRST half stands unchanged: the ported undo
// is brush-snapshot based and still has no room for a thing that is neither a
// brush nor an entity, so the two stores stay separate.
void KiwiCon_UndoPush();
bool KiwiCon_UndoPop();
bool KiwiCon_RedoPop();
void KiwiCon_ClearRedo();
int  KiwiCon_UndoDepth();

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BS) — THE CONSTRUCTION PLANE IS OUT OF THE DRAWING PATH
// ═════════════════════════════════════════════════════════════════════════════
// USER RULING, verbatim, after four rounds of snapping reports: *"Snapping is
// broken when extruding.  You seriously need to fucking get rid of the
// construction plane.  It's getting worse every update.  Go back to
// pre-construction plane.  This is not hard.  It just needs to be wherever a
// fucking ray trace hits against an object OR a snapping point."*
//
// WHERE A POINT GOES, EVERYWHERE, IN THIS ORDER (kiwi_snap.h, and one function —
// KiwiSnap_ResolvePoint — answers it for preview AND placement):
//   1. the engaged SNAP point (the BN ladder; construction context snaps by
//      default and Ctrl frees it, per the BO contract);
//   2. else the RAY'S SURFACE HIT on geometry;
//   3. else the world GROUND, z = 0, grid-snapped.
// Rung 3 is not a system and not a plane object.  It is the floor.
//
// WHAT THE PLANE API BELOW IS STILL FOR, and it is a short list:
//   * the PLANAR draw tools (rect / rect-from-centre / circle / 2-point circle /
//     arc / n-gon — KiwiDrawTool::PlanarOnly) and the §16b primitives, whose
//     SHAPES cannot exist off a plane.  A planar tool's plane is the EXPLICIT one
//     when the user set one, and otherwise is derived from the FIRST POINT'S
//     SURFACE (KiwiDrawTool::PushPoint) — the face that click landed on, the world
//     ground in void.
//   * [Space] on a face (kiwi_focus.cpp) and the §16 palette rows, which set an
//     EXPLICIT plane for those tools to use.  They cost the line path nothing:
//     nothing in the line/polyline/spline path reads the active plane.
//   * the entity-browser drop and kiwi_offset.cpp, which are neither drawing nor
//     transform and were never part of the report.
//
// WHAT IS GONE: PushPoint's per-point re-seat and KiwiCurveTool::ReseatPlane, the
// round-AA chain latch and round BR's SnapPlacesVerbatim that narrowed it,
// LatchFromCursor's ray∩plane seed, the working-plane off-plane snap gate
// (kiwi_snap.cpp CandGate::planeOn, KSNAP_ONPLANE_PX, KSNAP_OFFPLANE_PX), the
// arm-6 suppression for the free tools, and the PLANE banner for line work.
// ═════════════════════════════════════════════════════════════════════════════

// ── the active construction plane (§16) ──────────────────────────────────────
const kconPlane_t &KiwiCon_ActivePlane();
void KiwiCon_SetActivePlane( const kconPlane_t &p );
// axis is the plane NORMAL's world axis: 2 = XY, 1 = XZ, 0 = YZ.  Height comes
// from the last placement (spec §16: "XY at last hit height").
void KiwiCon_SetPlaneAxis( int axis );
void KiwiCon_SetPlaneFromView();
bool KiwiCon_SetPlaneFromCursorFace();          // false when no face is under the cursor
// ROUND U: the working plane of the ONE selected brush face.  False when the
// selection holds no face or more than one (see the body for why "more than one"
// has no honest answer).  Rung 2 of KiwiCon_AutoPlaneForTool — this is what makes
// "click a face, Shift+A, draw" put the sketch ON that face.
// KIWI-UX (ROUND BR): …and it now fires ONLY when KiwiCon_ArmSelectedFacePlane has
// armed it, i.e. inside the Shift+A gesture itself.  See the arm below.
bool KiwiCon_SetPlaneFromSelectedFace();
// ROUND BP: the ONE construction behind it — winding centre, face normal, longest
// edge as the u hint.  `requireReach` applies round AT's "the face has to be
// somewhere the user could be drawing on it" cursor test; [Space] passes false
// because the camera has just been flown to the face.  Does NOT mark the plane
// explicit — the caller decides that (KiwiCon_MarkPlaneExplicit).
bool KiwiCon_SetPlaneFromFace( selbrush_t *node, int faceIndex, bool requireReach );
// ROUND T: the plane NORMAL becomes the world axis most aligned with the camera's
// view direction (look down -> XY, at a wall along X -> YZ, along Y -> XZ), kept
// at the current plane's position along that axis.  False only when the view
// matrix is degenerate.  See the definition for why it quantises to a world axis
// instead of taking the view plane itself.
// KIWI-UX (ROUND BP, ITEM 3): DEMOTED — no longer a rung of KiwiCon_AutoPlaneForTool.
// It survives as a named operation (nothing calls it today) because the round-T flow
// it serves — "face a wall, draw a circle, pull it toward you" — is now reached by
// pressing Space ON that wall, which flies the camera there AND sets the plane.
bool KiwiCon_SetPlaneFromViewDominantAxis();
// ── KIWI-UX (ROUND BP, ITEM 3) — THE LADDER IS GONE.  THE USER'S RULING. ─────
// USER, verbatim: *"construction planes should only be made with [space], usually,
// the current behavior is too strict.  That's what it is."*
//
// What a tool start does now, in full:
//   1. a plane was set EXPLICITLY  -> it STANDS, untouched, and says so
//   2. exactly ONE brush face SELECTED, within its own extent, AND THE RUNG ARMED
//      -> that face's plane, MARKED EXPLICIT   (round U's Shift+A flow)
//   3. otherwise             -> KiwiCon_DefaultPlaneForView(), which has no memory
// The demoted rungs — the axis-view rung (round Y), the construction object under
// the cursor (shakeout F), the face merely under the cursor (§16 rung 3), the
// view-dominant quantiser (round T) and every offset-inheritance arm (round AT) —
// are DELETED, not disabled.  The table with a reason per rung is
// RADIANT_UX_DESIGN §16.
//
// ── KIWI-UX (ROUND BR): RUNG 2 IS ARMED, NOT AMBIENT ────────────────────────
// USER RULING: *"construction planes should only be made with [space], usually"* —
// and a face SELECTION is not a plane request.  Round BP's rung 2 fired at every
// tool start with one face selected, which in Face mode is the ordinary state
// (one click selects a face), so it minted an EXPLICIT plane — banner, the round-BQ
// off-plane snap gate armed, and a latch that outlived the gesture — from a click
// the user meant as a selection.  It now fires only for the ONE gesture the
// exception exists for: a creation chord pressed with a face parked (Shift+A and
// its siblings), which arms it through KiwiCon_ArmSelectedFacePlane.  Every other
// route into a drawing tool gets the default plane.
void KiwiCon_AutoPlaneForTool();

// KIWI-UX (ROUND BR): arm rung 2 for the NEXT KiwiCon_AutoPlaneForTool, and for
// that one only — it is consumed there unconditionally, before the explicit-plane
// early-out.  The single caller is kiwi_command.cpp's round-U preempt rung, which
// is the Shift+A-with-a-face-parked gesture and nothing else.
void KiwiCon_ArmSelectedFacePlane();

// The default plane: on an AXIS VIEW that view's world-axis plane, otherwise world
// ground — always THROUGH THE WORLD ORIGIN.  No elevation is ever inherited; that
// inheritance is what put a drawing plane at 61 ft with nothing on screen saying so.
void KiwiCon_DefaultPlaneForView();

// ── the EXPLICIT latch ───────────────────────────────────────────────────────
// A plane is explicit only when a gesture the user MADE installed it: Space on a
// face (kiwi_focus.cpp), a §16 palette row, or a selected face.  Marking is a
// SEPARATE call from installing, because KiwiDrawTool::PushPoint re-seats the
// active plane at every placed point and that must never latch anything.
void        KiwiCon_MarkPlaneExplicit( const char *desc );
bool        KiwiCon_PlaneIsExplicit();
const char *KiwiCon_PlaneDesc();          // "" when the plane is the default
// What the PLANE chip and the console print: the explicit description, or the
// default plane named ("ground XY (default)" / "XZ (default)" / "YZ (default)").
const char *KiwiCon_PlaneDescLive();
// Drop the latch and re-derive the default.  Space over empty space runs this.
void        KiwiCon_ClearPlaneToDefault();

// ── draw (Cam_Draw tail) ─────────────────────────────────────────────────────
// Construction lines + points + region fills + (while a tool runs) the finite
// construction-plane grid patch around the cursor.  Self-budgeted.
void KiwiCon_DrawWorld();

// -- ROUND AF, ITEM 7: the round tools remembered side count ----------------
// 0 = AUTOMATIC (the radius-driven KCON_SEGS_PER_UNIT rule).  A positive value
// is clamped to [KCON_SIDES_MIN, KCON_SEGS_MAX] and persisted in
// kiwi_radiant.ini under [KiwiUX] RoundToolSides.  Read once per gesture by
// KiwiDrawTool::Begin; written by the Tab field and by the n-gon's [ ] keys,
// which are two views of the same number.  See the .cpp for why it is ONE
// setting and not four.
int  KiwiCon_ToolSides();
void KiwiCon_SetToolSides( int sides );

bool KiwiCon_ShowConstruction();
void KiwiCon_SetShowConstruction( bool on );

// True while one of the drawing tools owns the gesture — ANY of them, planar or
// free.  KIWI-UX (ROUND BS): the SNAP_CPLANE / SNAP_ANGLE arms are no longer gated
// on this (they are gated on KiwiCon_PlanePlacement, i.e. planar placers only);
// this predicate now means only "a chain is being drawn", which is what its other
// readers — the construction draw pass and the tool-anchor accessors — ask.
bool KiwiCon_ToolActive();

// ── ROUND K: PLANE PLACEMENT, FOR COMMANDS THAT ARE NOT DRAWING TOOLS ────────
// THE BOX BUG.  USER REPORT, verbatim: "Creating a 3d brush vertically is
// impossible right now, any camera angle creates it horizontally. […] I can't
// even create a box, the line never expands."
//
// ROOT CAUSE.  kiwi_snap.cpp's arm 8 (SNAP_CPLANE — "ray ∩ the active
// construction plane, grid-snapped in plane") is gated on `KiwiCon_ToolActive()`,
// and that predicate means EXACTLY "one of kiwi_construct.cpp's nine KiwiDrawTool
// objects is s_activeTool".  The four §16b PRIMITIVES (kiwi_primitive.cpp) are
// KiwiEditorCommands in a different file and were never visible to it, so with a
// Box gesture live the snap query fell through to arm 6 (the Test_Ray surface
// hit) or arm 9 (ray ∩ the world Z=0 ground grid) and the primitive then
// PROJECTED that answer onto its own working plane.  On a HORIZONTAL plane at
// z == 0 that projection happens to track the cursor, which is why boxes came out
// horizontal and appeared to work at all; on a VERTICAL plane the ground answer
// has a constant height, so the in-plane v coordinate never moved and the base
// rectangle could not grow in that direction — "the line never expands".
//
// THE FIX IS ONE PREDICATE, NOT A SECOND SNAP PATH.  A command that places points
// ON THE ACTIVE CONSTRUCTION PLANE announces itself here for the length of its
// gesture, and kiwi_snap.cpp asks KiwiCon_PlanePlacement() instead of
// KiwiCon_ToolActive().  KiwiCon_ToolActive keeps its old, narrower meaning (the
// plane PATCH and KiwiCon_ToolAnchor are genuinely about a drawing tool's chain),
// so nothing else changes: with no primitive running the two predicates are
// identical and every snap answer is bit-for-bit what it was.
//
// Set/cleared from Begin() and Commit()/Cancel().  Idempotent and self-clearing:
// KiwiCon_SetPlanePlacement(false) with nothing set is a no-op, so a command that
// refuses inside Begin cannot leave it latched.
//
// ── KIWI-UX (ROUND BS): THIS PREDICATE NOW MEANS **PLANAR** PLACEMENT ────────
// USER RULING, verbatim: *"You seriously need to get rid of the construction
// plane. […] It just needs to be wherever a ray trace hits against an object OR a
// snapping point."*
//
// It answers TRUE for exactly the placers whose SHAPE cannot exist off a plane —
// the rect / rect-from-centre / circle / 2-point circle / arc / n-gon draw tools
// (KiwiDrawTool::PlanarOnly) and the §16b primitives, which announce themselves
// through KiwiCon_SetPlanePlacement — and FALSE for the LINE, POLYLINE and SPLINE
// tools, which now have no working plane at all.  It is the ONE switch that keeps
// every plane-borne branch of kiwi_snap.cpp (the raw fallback, the occlusion
// relaxation, the arm-6 suppression, arms 7 + 8) and kiwi_hints.cpp's working-plane
// chip out of the line path, and it is the reason those branches did not need a
// second gate each.
void KiwiCon_SetPlanePlacement( bool on );
bool KiwiCon_PlanePlacement();      // a PLANAR draw tool OR a plane-placing command

// The point the active tool is drawing FROM (its last placed point, or the
// rubber-band origin).  This is what SNAP_ANGLE measures its 15° increments
// against; false when the tool has not placed anything yet.
bool KiwiCon_ToolAnchor( float out[3] );

// SHAKEOUT F: the point BEFORE the anchor — the far end of the segment the tool
// has most recently COMMITTED to its in-progress chain.  False when the chain
// holds fewer than two points.  This is the RELATIVE ANGLE SNAP's first choice of
// base direction (kiwi_snap.h arm 7): a polyline's next segment should be able to
// go 45° off the one just placed, and that segment is not in the store yet — the
// tool has not committed its object.
bool KiwiCon_ToolPrevAnchor( float out[3] );

// ── KIWI-UX (ROUND AA, ITEM 3a): THE LOOP-CLOSING TARGET IS A SNAP ───────────
// USER REPORT, verbatim: "We need a light snapping point here for this line
// connect."  (Screenshot: a rectangle being drawn, the cursor coming back around
// to the first point, and nothing accenting it.)
//
// The chain's FIRST point is the one target a closing gesture is aiming at, and
// it was the one point in the scene the snap query could not see: the chain lives
// in the tool's own m_pts and is not committed to the construction store until
// the object is finished, so arm 1 (which scans KiwiCon_At()) never had it.  The
// tool already knows when the cursor is near it — NearFirstPoint / KCON_JOIN_PIXELS
// is what closes the loop on a click — but "the click will close" and "the point
// will land exactly on the first point" were two different tests, and only the
// first one existed.  So a loop closed VISUALLY and left a hairline gap.
//
// This exposes it as a snap candidate.  False unless a chain of at least three
// points is open (fewer than three cannot enclose anything, so there is nothing
// to close and offering the target would just be a magnet on the segment you are
// still drawing).
bool KiwiCon_ToolLoopStart( float out[3] );

// ── §7 sidecar persistence ───────────────────────────────────────────────────
// `mapPath` is the .map path; the sidecar is the same path with the extension
// replaced by ".kiwi".  Save is a no-op (returning true) when the store is empty
// AND no sidecar exists, so a map that never used construction geometry never
// grows a file.  Load NEVER fails a map load: a bad file warns and is ignored.
bool KiwiCon_SaveSidecar( const char *mapPath );
bool KiwiCon_LoadSidecar( const char *mapPath );

// ── commands ─────────────────────────────────────────────────────────────────
void KiwiCon_RegisterCommands();
KiwiEditorCommand *KiwiCon_CommandForId( int commandId );   // the 9 drawing tools
bool KiwiCon_DispatchInstant( unsigned int commandId );     // the plane / clear commands
bool KiwiCon_HasObjects();                                  // palette canExecute
// KIWI-UX (CLEANUP, A-69): KiwiCon_CanDraw is gone -- it could only answer true.
// The nine drawing rows in kiwi_command.cpp pass a null canExecute instead.

// The View-menu "Construct" submenu (drawn inside the KiwiUX settings block).
void KiwiCon_MenuItems();

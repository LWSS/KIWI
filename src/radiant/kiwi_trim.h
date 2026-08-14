#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_trim.h — SHAKEOUT H: TRIM (T), the "cut the overhang off" verb.
//
// USER DIRECTIVE, verbatim: "Add a Trim tool (T) that only works on lines.  Its
// job is to trim off excess bits when lines are hastily drawn (see pic) This
// exists in plasticity."  The picture is two lines crossing in an X with the four
// tails sticking out past the crossing; clicking a tail removes it, leaving a
// clean corner.
//
// ── WHAT PLASTICITY ACTUALLY DOES (read, not remembered) ────────────────────
// plasticity/src/commands/curve/TrimCommand.ts, bound to a BARE `t` in
// src/startup/default-keymap.ts:267 (`"t": "command:trim"`, inside the
// `body:not([gizmo])` scope).  Three facts shaped this port:
//
//   1. IT IS CURVES ONLY.  The picker is `objectPicker.mode.set(SelectionMode.
//      Curve)` (TrimCommand.ts:18) and LayerManager.showFragments() actively
//      DISABLES solid/face/region picking for the duration
//      (src/editor/LayerManager.ts:37-50).  So "only works on lines" is not a
//      simplification of Plasticity — it is exactly Plasticity.
//   2. THE PIECES EXIST BEFORE YOU CLICK.  PlanarCurveDatabase splits every
//      coplanar curve at every mutual intersection into invisible "fragments" the
//      moment a curve is added (src/editor/curves/PlanarCurveDatabase.ts:24-33,
//      :79 `c3d.CurveEnvelope.IntersectWithAll`), and Trim just turns that layer
//      visible and lets you pick one.  KIWI computes the spans ON HOVER instead —
//      same answer, no second database to keep in sync, and the store is small
//      enough that the scan is free.
//   3. REMOVING A MIDDLE SPAN SPLITS THE CURVE IN TWO.  TrimFactory computes the
//      COMPLEMENT of the removed parameter range (`interval.multitrim(...)`,
//      TrimFactory.ts:84-91) and Interval.trim returns TWO intervals when the cut
//      is interior (src/commands/curve/Interval.ts:31), one when it touches an
//      end, and none when it swallows the whole curve.  KIWI does the same three
//      cases, in world space.
//   4. IT KEEPS GOING.  There is no loop: the command re-enqueues ITSELF after
//      each trim (`this.editor.enqueue(new TrimCommand(this.editor), false)`,
//      TrimCommand.ts:30) until Escape.  KIWI uses the shakeout-E multi-click
//      grammar instead — WantsClicks, so a click is an event and RMB/Enter ends
//      the command — which is the same behaviour through the machinery this
//      editor already has.
//
// ── WHAT KIWI TRIMS ─────────────────────────────────────────────────────────
// KCON_LINE and KCON_POLYLINE, open or closed.  NOT circles or arcs (they are
// parametric — trimming one means converting it to a polyline, which silently
// destroys the thing the user drew), and NOT rects (same argument: a trimmed rect
// is not a rect).  Those are simply not hover candidates, and the HUD says so.
// A polygon and a spline ARE trimmable, because the store keeps both as
// KCON_POLYLINE (kiwi_construct.h §16b note).
//
// ── THE MATH ────────────────────────────────────────────────────────────────
// Intersections are 3D, because the store is 3D now (kiwi_construct.h ruling 3).
// Two segments "cross" when their CLOSEST APPROACH is under KCON_ISECT_DIST
// (0.25 world units) — KiwiCon_SegSegClosest, the same helper the SNAP_INTERSECTION
// arm uses, so what trims is what snaps.  A true 3D line-line intersection test
// would be exact-zero and therefore useless on floats.
//
// The hovered object is flattened into a cumulative ARC-LENGTH parameterisation.
// Every crossing gives one parameter along it; the SPAN under the cursor is
// bounded by the nearest crossing below and the nearest above, falling back to the
// object's own ends.  That is the whole rule, and it is why the X in the user's
// picture works: each tail is bounded by the crossing on one side and by an
// endpoint on the other.
//
// ── UNDO ────────────────────────────────────────────────────────────────────
// ONE store-undo snapshot per TRIM CLICK (kiwi_construct.h ruling 2), not one per
// command.  A trim session is a sequence of independent removals — Plasticity
// makes each one its own command for exactly the same reason — and rolling five
// of them back together would surprise anyone who only wanted the last one gone.
//
// THE PUSH IS THE LAST THING THAT CAN GO WRONG, and it has to be: since shakeout I
// every KiwiCon_UndoPush ALSO mints a ticket in the unified journal (kiwi_undo.h),
// and there is no "discard" spelling — KiwiCon_UndoPop RESTORES and is driven BY
// the journal, so calling it to unwind a pointless push would leave an orphan
// CONSTRUCTION ticket pointing at somebody else's snapshot.  The work is therefore
// split: PrepareTrim reads the store and builds the two keep-lists with no
// mutation and may decline; only once it has succeeded is the snapshot pushed, and
// CommitTrim cannot decline.  No new store API, no journal surgery.
//
// ── THE KEY ─────────────────────────────────────────────────────────────────
// Bare T in the modern profile.  vk 0x54's full occupancy in the compiled-in
// table: mods 0 ViewTextures 33018 (mainfrm.cpp:998) · mods 1 ToggleTexMoveLock
// 32785 (:1064) · mods 5 ThickenPatch 32904 (:999).  (NOT ToggleTexLock at mods 0
// — that is 32785 and it is on Shift+T.)  mods 3 is free, so ViewTextures takes
// the house two-step to Shift+Alt+T; it stays on the View menu and in the palette.
// T is not in res/radiant.rc's accelerator table (:490-501), which matters because
// TranslateAccelerator runs BEFORE the hotkey table and a collision there would
// have been silent.
// ─────────────────────────────────────────────────────────────────────────────

class KiwiEditorCommand;

// Palette predicate: at least one trimmable object exists.
bool KiwiTrim_CanTrim();

void               KiwiTrim_RegisterCommands();
KiwiEditorCommand *KiwiTrim_CommandForId( int commandId );

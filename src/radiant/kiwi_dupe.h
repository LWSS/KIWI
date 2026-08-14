#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_dupe.h — RADIANT_UX_DESIGN §25's duplication ops: mirror and arrays.
//
//     Mirror X / Y / Z    — NOT new commands.  See below.
//     KIWI_CMD_ARRAY_LINEAR  34042   MODAL "Array (Linear)"
//     KIWI_CMD_ARRAY_RADIAL  34043   MODAL "Array (Radial)"
//
// ═════════════════════════════════════════════════════════════════════════════
//  MIRROR — already ported, already wired, therefore NOT re-implemented
// ═════════════════════════════════════════════════════════════════════════════
// §25 lists "mirror" as a later op.  In this port it already exists end to end:
//
//   select.cpp:2411  Select_FlipAxis( int axis )   0x48FD50
//        identity 3x3 with `axis` negated, applied through
//        Select_ApplyMatrix_SelectedBrushes( 1, m, 0.0f, 1 ) — bSwap 1 reverses the
//        windings so reflected faces keep OUTWARD-facing normals.
//   select.cpp:2434  DoFlip( int axis, const char *opName )   0x424F30
//        the undo-bracketed command core: Undo_ClearRedo → Undo_GeneralStart →
//        Undo_AddBrushList → Select_FlipAxis → for every selected FIXED-SIZE
//        entity also flip its `angles` key by 180° on the mirror axis and rebuild
//        → Undo_EndBrushList → Undo_End.
//   mainfrm.cpp:5267-5269  command ids 32956 / 32957 / 32958  (Brush→Flip→X/Y/Z)
//        Cmd_OnBrushFlipx/y/z → DoFlip( axis, "flip X|Y|Z" ).
//
// The pivot is `Select_GetMid` — the selection mid — which is exactly what §25's
// "mirror about the selection mid" asks for.  So the Phase-5 work here is ZERO
// behaviour and only presentation: §3 metadata rows on the EXISTING ids (display
// name "Mirror X (Flip)", category "Duplicate", a canExecute that greys the row
// with an empty selection) plus three buttons in the Modeling panel that call
// Radiant_ExecCommand with those same classic ids.  No new command id is minted,
// nothing is duplicated, and menu / hotkey / palette / panel all reach one
// handler.
//
// ═════════════════════════════════════════════════════════════════════════════
//  ARRAYS
// ═════════════════════════════════════════════════════════════════════════════
// ── THE DUPLICATION CORE ────────────────────────────────────────────────────
// `Clone_Selection( float gridSize )` (select.cpp:2482, 0x48F0D0 — the core behind
// classic id 33001 "Clone Selection").  Read, not assumed:
//   * it SNAPSHOTS selected_brushes first (Brush_AddToList2 appends to the same
//     list, so iterating while adding would clone the clones forever);
//   * it SKIPS patches and fixed-size-entity brushes (they need the clipboard
//     path), so an array of those produces nothing and says so;
//   * it then `Select_Deselect(1)` — THE ORIGINALS ARE DESELECTED — and leaves
//     exactly the COPIES selected;
//   * it takes NO undo bracket of its own (neither does the classic handler
//     Cmd_OnSelectionClone, mainfrm.cpp 0x425480).
//   * `gridSize` is unused in this port (the binary's cosmetic paste offset).
//
// ── HENCE: ONE UNDO RECORD FOR THE WHOLE ARRAY ──────────────────────────────
// Because the core brackets nothing, this file owns the bracket outright and the
// answer to "one record or N" is ONE.  The ordering is forced by undo.cpp and is
// the mirror image of kiwi_extrude.h's creation note:
//
//   KiwiCmd_UndoBegin("array")           Undo_AddBrushList clones the ORIGINALS
//                                        (which are selected at this moment)
//   N-1 × [ Clone_Selection → transform ]  each step clones the PREVIOUS copy, so
//                                        copy i lands at i × step with no
//                                        re-selection of the originals needed
//   re-select originals + every copy      via selection_t + Sel_SyncToLegacy
//   KiwiCmd_UndoCommit                   Undo_EndBrushList stamps ALL of them
//
// Undo_Undo then removes every stamped brush that has no saved clone (the copies)
// and restores the ones that do (the originals) — which is exactly "undo the
// array".  The requirement this satisfies is undo.cpp's: everything
// Undo_AddBrushList cloned at BEGIN must still be in the list at COMMIT, or its
// clone would be restored while the live original stayed and the brush would
// double.  That is why the originals are put back into the selection at the end
// rather than left deselected.
//
// ── LINEAR: DRAG = OFFSET, TYPED = COUNT (the documented split) ─────────────
// The two array parameters are a VECTOR and an INTEGER, and the numeric layer
// (kiwi_numeric.h) offers exactly one scalar.  So:
//     DRAG   sets the offset VECTOR — the cursor mapped onto the plane through
//            the selection mid whose normal is the camera vpn latched at Begin
//            (kiwi_transform.h's unconstrained mapping, and latched for the same
//            reason: orbiting mid-gesture must not swing the offset plane).
//            Grid snapping applies to the vector.
//     TYPED  sets the COUNT, clamped to [KARR_MIN_COUNT, KARR_MAX_COUNT].  The
//            digits are read back through Units_ToDisplay because a count is not
//            a length and the numeric layer has already converted it.
// The HUD says which is which, every frame.
//
// ── RADIAL ─────────────────────────────────────────────────────────────────
// N copies about the SELECTION MID on the Z axis, evenly spaced over a full
// circle (step = 360/N).  Typed digits set N; the drag sets NOTHING in v1 (an
// arbitrary sweep angle and a pickable pivot are the obvious v2).  Each step uses
// the canonical ported transform pattern — Select_RotateAxis builds the matrix
// about the latched pivot, Select_ApplyMatrix_SelectedBrushes applies it — which
// is byte-for-byte what mainfrm.cpp's Radiant_RotateSelection does for the
// classic Rotate X/Y/Z commands.
//
// The pivot is latched ONCE at Begin with Select_GetMid.  Recomputing it per step
// would drift: the bbox mid of a rotated brush set is not the rotated bbox mid.
//
// ── PREVIEW ────────────────────────────────────────────────────────────────
// Wireframe ghosts through kiwi_lines, on a hard budget (the Cam_Draw-tail batch
// is 64 segments and it is shared with the snap marker).  A box costs 12 segments,
// so the preview draws full boxes while they fit and degrades to a 3-segment axis
// cross per copy when they do not — never spilling, per kiwi_lines.h TRAP 1.
// ─────────────────────────────────────────────────────────────────────────────

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND J — DUPLICATE (Shift+D), the Plasticity verb this file was missing
// ═════════════════════════════════════════════════════════════════════════════
//     KIWI_CMD_DUPLICATE  34107   INSTANT, modern key Shift+D
//
// ── THE PLASTICITY SOURCE ───────────────────────────────────────────────────
// `shift-d` -> `command:duplicate` (default-keymap.ts:280).
// src/commands/duplicate/DuplicateCommand.ts, in full, is: duplicate every
// selected item, REPLACE THE SELECTION WITH THE COPIES (it removes each source
// from `selected` and adds the results), and then — the last line of the file —
//
//     this.editor.enqueue(new MoveCommand(this.editor), false);
//
// i.e. duplicate ALWAYS hands off into Move.  Duplicating without placing is not
// a thing Plasticity does, which is exactly the shape KIWI already built for the
// classic Paste and Clone in shakeout G (KiwiCmd_AfterPaste, kiwi_command.h).
//
// ── SO THIS IS THE SAME TWO PARTS, AND BOTH ALREADY EXIST ──────────────────
//   the COPY   Clone_Selection (select.cpp:2492, 0x48F0D0) — read in full in the
//              ARRAYS section below: it snapshots the source list first, skips
//              patches and fixed-size-entity brushes, and ends with
//              Select_Deselect(1) + the copies on selected_brushes.  "Replace the
//              selection with the copies" is what the ported core already does.
//   the MOVE   KiwiCmd_AfterPaste (kiwi_command.cpp:590) — starts KIWI_CMD_MOVE
//              PAUSED over whatever is selected, which is the gizmo-up,
//              nothing-follows-the-cursor-yet state shakeout E defined.
//
// The classic tail is replicated exactly: Cmd_OnSelectionClone (mainfrm.cpp:2973)
// passes `grid_sizes[g_qeglobals.d_gridsize]` (unused by this port — the binary's
// cosmetic paste offset) and then refreshes the special-material flag on every
// brush def in both display lists via sub_47B940 (brush.cpp:5811,
// Brush_UpdateSpecialMaterialFlag).  Both loops are reproduced here rather than
// skipped: they are what keeps the 2D back-face-cull hint correct on the copies.
//
// ── AND ONE THING THE CLASSIC CLONE GETS WRONG, FIXED HERE ─────────────────
// Cmd_OnSelectionClone OPENS NO UNDO BRACKET (mainfrm.cpp:2973-2980, and
// kiwi_dupe.h's ARRAYS section already noted it).  Undo_Undo only removes brushes
// carrying an undo record's stamp (undo.cpp:576-590 Undo_EndBrushList writes it),
// so an UNSTAMPED clone is invisible to undo: pressing Ctrl+Z after a classic
// Clone undoes whatever came BEFORE it and leaves the copies sitting there.
//
// KIWI_CMD_DUPLICATE brackets itself, with the CREATION shape kiwi_extrude.h
// documents rather than the mutation shape KiwiCmd_UndoBegin provides:
//
//     Undo_ClearRedo()  →  Undo_GeneralStart("duplicate")
//     Clone_Selection(...)                      ← the copies land selected
//     Undo_EndBrushList(&selected_brushes)      ← stamps EXACTLY the copies
//     Undo_End()                                ← mints the journal ticket
//
// NO Undo_AddBrushList, and that is the whole point.  AddBrushList is what SAVES
// a restorable clone of an existing brush; the originals are not modified by a
// duplicate, so there is nothing to save, and Undo_Undo's "remove every stamped
// brush that has no saved clone" is precisely "delete the copies".  It is the
// same bracket kiwi_extrude.h reaches by deselecting first — there the selection
// is empty at BEGIN, here nothing is added at begin at all, and both end up with
// a record whose only members are the new brushes.  Verified against undo.cpp:
// Undo_EndBrushList does not require a preceding Undo_AddBrushList (it only needs
// an open record — undo.cpp:578-579).
//
// THE MOVE THAT FOLLOWS OPENS ITS OWN BRACKET, so a duplicate-then-place is TWO
// Ctrl+Z presses: one puts the copies back where they landed, the second removes
// them.  That is the same two-step the classic Paste + Move already gives and it
// is the honest decomposition — the two acts are separately undoable because they
// are separately intentional.

class KiwiEditorCommand;

#define KARR_MIN_COUNT      2
#define KARR_MAX_COUNT      64
#define KARR_DEF_LINEAR     3     // sensible defaults; the user types over them
#define KARR_DEF_RADIAL     6
#define KARR_MIN_OFFSET     0.5f  // world units — below this a linear array is a no-op
#define KARR_MAX_SOURCE     512   // v1 cap on source brushes in one array

// ── KIWI-UX (ROUND AA, ITEM 8): THE HAND-AIM DEADZONE AROUND A PICKED LINE ──
// USER REPORT, verbatim: "The linear array tool needs to accept a line to go
// across.  Doing it by hand is really hard and pivot (V) support needs to be
// there as well."
//
// A picked construction line and the hand-drag are two sources for ONE quantity
// (the step vector), so exactly one of them has to be in force at a time, and the
// user has to be able to get back to the other without leaving the gesture.
//
// WHY A PIXEL DEADZONE AND NOT "any mouse move drops the line".  The linear array
// has NO GRAB GATE: Recompute() maps the cursor to the step on EVERY MouseMove,
// pressed or not (kiwi_dupe.cpp Recompute), so in this command "dragging" and
// "moving the mouse at all" are the same event.  Releasing the line on the raw
// event would make line mode last exactly until the next twitch — i.e. it would
// not exist.  Measuring from the PIXEL THAT PICKED THE LINE and only releasing
// past a radius separates the two things the cursor does after a pick: settling /
// reading the HUD (inside), versus re-aiming the array by hand (outside).
//
// 48 px is deliberately generous — an accidental drop costs the user a re-click on
// a 10 px line (KCON_LINE_PIXELS), which is the expensive half of the mistake,
// while an over-sticky line costs only one more mouse move.  The release is never
// silent: it prints, and the HUD names the mode in force on every frame.
#define KARR_LINE_BREAK_PIXELS 48.0f

void KiwiDupe_RegisterCommands();
KiwiEditorCommand *KiwiDupe_CommandForId( int commandId );

// §3 canExecute predicates.
bool KiwiDupe_CanMirror();   // attaches to the CLASSIC ids 32956 / 32957 / 32958
bool KiwiDupe_CanArray();    // at least one cloneable brush is selected
bool KiwiDupe_CanDuplicate();// …the same precondition; a separate name so the two
                             // palette rows can diverge later without a rename

// ROUND J: the instant Duplicate (Shift+D).  Returns false for an id it does not
// own, exactly like the other DispatchInstant arms.
bool KiwiDupe_DispatchInstant( unsigned int cmdId );

// The "Duplicate" block in the shell panel (mirror trio + the two array commands).
void KiwiDupe_MenuItems();

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_boolean.h — ROUND L: Q, the BOOLEAN verb (difference and union).
//
// USER DIRECTIVE, verbatim: "Add a new feature for solids(brush).  Boolean and
// difference. (Q).  First select a solid, then press Q, another solid is expected
// to be picked.  Click on that solid and it automatically enters difference mode,
// however press Q again and it enters boolean(combining) mode.  Difference mode
// carves a chunk into the solid(Brush), while boolean mode tries to combine them
// into 1 solid(Brush).  This is very crucial in plasticity."
//
// ── WHAT PLASTICITY ACTUALLY DOES (read, not assumed) ───────────────────────
// plasticity/src/commands/boolean/.  There is ONE command class, BooleanCommand
// (BooleanCommand.ts:13 `export class BooleanCommand extends Command {`); there is
// no UnionCommand / DifferenceCommand / IntersectionCommand and no BooleanGizmo —
// the operation is a FIELD on the factory, not a class.
//
//   THE KEY IS Q, AND THE DIRECTIVE'S CHOICE IS PLASTICITY'S OWN.
//     src/startup/default-keymap.ts:294  `"q": "command:boolean",`
//   inside the `body:not([gizmo])` block (:239).  While the command is live it
//   opens its own scope, default-keymap.ts:66 `"[command='boolean']
//   plasticity-viewport": {`, with THREE ABSOLUTE bindings rather than a cycle:
//     :67 `"q": "gizmo:boolean:union"`   :68 `"w": ...:difference"`
//     :69 `"e": "gizmo:boolean:intersect"`
//
//   TARGET vs TOOL.  Both are SelectionMode.Solid.  The pre-existing selection is
//   copied into an ObjectPicker (BooleanCommand.ts:21-22
//   `objectPicker.copy(this.editor.selection)`); the TARGETS are then CONSUMED off
//   the front of it — :43 `return objectPicker.shift(SelectionMode.Solid, 1)
//   .resource(this)`, :46 `boolean.targets = [...solids];` — and whatever is LEFT
//   becomes the tools, :91 `const set = await setToolsAndGizmo([...objectPicker
//   .selection.selected.solids]);`.  `shift` REMOVES what it hands back
//   (ObjectPicker.ts:296-307 → :230 `if (shouldRemove) this.selection.selected
//   .remove(item);`), and when nothing is pre-selected it opens a modal sub-picker
//   (ObjectPicker.ts:245-252) — i.e. "click the other solid".  Each side is then
//   re-editable through a picker that PROHIBITS the other (:51 / :97).
//
//   DEFAULT OPERATION IS DIFFERENCE.  BooleanFactory.ts:272 `private
//   _operationType = c3d.OperationType.Difference;` (and :28 on the single-solid
//   factory).  The keyboard gizmo maps its command's last path segment to a
//   c3d.OperationType and re-runs (BooleanCommand.ts:30-38, :36-38 `boolean
//   .operationType = operationType; boolean.update(); dialog.render();`), fanning
//   it out to one factory per target (BooleanFactory.ts:274-278).
//
//   THE PREVIEW is translucent PHANTOMS of both operands, coloured BY OPERATION:
//   BooleanFactory.ts:203-205 `if (operationType === c3d.OperationType.Difference)
//   material = phantom_red` / `else if (... Intersect) ... phantom_green` / `else
//   ... phantom_blue`, with the targets always blue (:215).  The materials are
//   MeshBasicMaterial at `opacity = 0.1` (:351-392).
//
//   THE TOOL IS CONSUMED BY DEFAULT.  BooleanFactory.ts:264 `@delegate
//   .default(false) keepTools!: boolean;` and :325-328 `const tools = this
//   .keepTools ? [] : this._tools.views; return [...this._targets.views,
//   ...tools];` — `originalItem` is what GeometryFactory.doCommit removes
//   (GeometryFactory.ts:160-168).  `keepTools` is a checkbox on BooleanDialog
//   (BooleanDialog.tsx:34).
//
// ── THE FOUR DELIBERATE DEVIATIONS, each with its reason ────────────────────
//
//  1. Q TOGGLES; IT DOES NOT SELECT AN OPERATION.  The directive is explicit
//     ("press Q again and it enters boolean(combining) mode"), and it is the right
//     call for KIWI anyway: Plasticity can afford q/w/e because its command scope
//     rebinds those keys for the duration, and KIWI's key funnel hands every key
//     to the active command already — but W and E are the fly-strafe and the
//     extrude keys, and teaching two more meanings for them inside one gesture is
//     worse than one key that alternates between the only two operations KIWI has.
//
//  2. THERE IS NO INTERSECTION.  Not a scope cut for its own sake: a subtract is
//     "keep the OUTSIDE half at every tool plane", which the ported splitter gives
//     directly; an intersection is "keep the INSIDE half at every tool plane",
//     which is the SAME loop — but its result is the tool-shaped lump, which for a
//     convex tool is just the tool, and for a concave one is not representable as
//     one classic brush anyway.  It would be a verb that either does nothing
//     visible or refuses.  Logged in RADIANT_KNOWN_ISSUES rather than shipped.
//
//  3. [WITHDRAWN IN ROUND N] THE TOOL SURVIVES.
//     ROUND L argued for keeping it: "classic Radiant's own subtract idiom is a
//     cutter brush you position, apply, and then re-apply somewhere else, and a
//     difference that silently ate the cutter would make the second use
//     impossible."  THE USER OVERRULED IT, verbatim: "When doing a difference
//     command, it needs to delete the original solid(Brush) along with the 'tool'
//     solid.  Same with boolean, it should leave only 1 (if it doesn't already)."
//     So KIWI now matches Plasticity outright — keepTools = false, no checkbox and
//     no deviation to explain.  The re-apply workflow is Ctrl+Z (which restores the
//     tool along with the carve, because kiwi_boolean.cpp covers it explicitly —
//     see THE TOOL IS CONSUMED TOO there) or Shift+D on the tool before pressing Q.
//     UNION ALWAYS CONSUMED IT AND CANNOT BE OTHERWISE: CSG_Merge's whole job is to
//     replace its inputs with one brush (csg.cpp:604-624 frees every merged node
//     and Brush_AddToList2's the single merged result), so a union already leaves
//     exactly ONE brush — verified, not assumed, and both operands are on
//     selected_brushes when it runs because DoUnion selects the targets AND the
//     tool before dispatching it.
//
//  4. ONE TOOL, NOT A SET.  Plasticity's tools are a re-editable multi-selection;
//     the directive says "another solid is expected to be picked … click on that
//     solid", which is one click and one solid.  Multi-target IS supported (every
//     selected solid is carved by the one tool), because that is free — the carve
//     is per-target already.
//
// ── DIFFERENCE: THE N-PLANE SUBTRACT ────────────────────────────────────────
// THERE IS NO SUBTRACT CORE IN THIS PORT.  kiwi_csg.h's Phase-5 inventory of
// csg.cpp lists CSG_MakeHollow, Brush_MergeList and CSG_Merge and nothing else,
// and kiwi_csg.cpp says so out loud in the panel ("Subtract: not in this port").
// So the carve is built here, out of the ONE thing the port does have: the
// two-halves splitter over Brush_SplitBrushByFace (kiwi_split.h).
//
// The algorithm is the classic brush-CSG subtract, and it is exact for convex
// operands (which is all a classic brush can be):
//
//     remainder = target
//     pieces    = {}
//     for each FACE PLANE (n_i, d_i) of the tool, outward normals:
//         split remainder by that plane
//         front = { n_i·p >= d_i }   the part OUTSIDE the tool at this plane
//         back  = { n_i·p <= d_i }   the part that may still be inside it
//         front == NULL  ->  remainder is wholly inside this half-space; carry on
//         back  == NULL  ->  remainder is wholly OUTSIDE the tool: no intersection
//                            at all, so this target is not carved.  Stop.
//         otherwise      ->  pieces += front;  remainder = back
//     discard remainder            it IS target ∩ tool, the volume being removed
//
// Every piece is a convex intersection of half-spaces by construction, so every
// piece is a legal classic brush; their union is exactly target \ tool.  The count
// is at most one piece per tool face (six for a box), which is also the classic
// result — subtracting a box from a box gives up to six brushes, not one.
//
// ── KIWI-UX (ROUND AA, ITEM 9): N TOOLS, NOT ONE ────────────────────────────
// USER REPORT, verbatim: "the difference command needs to accept multiple
// shift-clicked brushes.  For example, if I'm making a sidewalk and I want to
// boolean each crack, I have to do it 1 by 1 current, that's awful.  Also support
// shift box select."
//
// The loop above is now the INNER one.  Around it:
//
//     pieces = { target }
//     for each tool t:
//         pieces = union over p in pieces of ( p \ t )      the loop above, per p
//
// which is `target \ (t0 ∪ t1 ∪ …)` without ever computing the union of the tools
// — deliberately, because a union of two cracks that do not touch is not convex
// and there is no classic brush that could hold it.  A tool that MISSES a piece
// leaves that piece alone (that is the sidewalk case: each crack meets one
// fragment and misses the rest); a piece a later tool swallows whole simply
// vanishes, which is NOT the §19 refusal below — that refusal is about the TARGET
// disappearing, and it is still raised, once, if the tools between them leave no
// piece at all.  Everything else in this header is unchanged, per target.
//
// ── VALIDATION AND THE ALL-OR-NOTHING POLICY (§19) ──────────────────────────
// Every half comes back §19-gated (KiwiSplit_DefByPlane runs KiwiValid_CheckBrush
// on both and frees BOTH if either fails), and the pieces are built as UNLINKED
// defs — they are on no display list until the whole carve for that target has
// succeeded.  So a rejection is FREE, exactly as it is for the region extrude
// (kiwi_extrude.h REJECTION IS FREE): the defs are released and the map, the
// selection and the undo stack are untouched.
//
// THE POLICY, stated once: a target that produces ZERO pieces, or whose carve hits
// ANY invalid piece, is LEFT COMPLETELY UNTOUCHED and says so on the console.
// Reject > mangle (§19's v1 rule).  Two consequences worth naming:
//   * A TARGET ENTIRELY INSIDE THE TOOL(S) produces zero pieces.  Mathematically its
//     difference is the empty set and classic Radiant would delete it; KIWI leaves
//     it and prints why, because "the verb removed a brush you can no longer see
//     to undo" is the worse failure of the two.  Deleting it is one Del away.
//   * A SLIVER INTERSECTION can make the discarded remainder fail §19 and refuse
//     the whole carve.  That is the conservative half of the same rule; nudging
//     the tool off the coincident plane fixes it, and the message says so.
//
// ── UNDO: ONE RECORD FOR THE WHOLE VERB ─────────────────────────────────────
// The CSG_MakeHollow wrapper's shape, which kiwi_split.h derives from undo.cpp in
// full: KiwiCmd_UndoBegin clones every brush on selected_brushes (the targets),
// the pieces are LANDED SELECTED (Brush_AddToList + Brush_AddToList2) and only
// THEN is each carved original freed with Brush_Free, and KiwiCmd_UndoCommit's
// Undo_EndBrushList stamps whatever is in the list at that moment — the pieces.
// Undo_Undo then removes the stamped pieces (Phase 1) and re-links the cloned
// originals (Phase 4), which is exactly "the carve never happened".
// ONE bracket for every target, so one Ctrl+Z undoes the whole verb.
//
// The TOOL is deliberately not put on selected_brushes for the difference: it is
// not modified, so it needs neither a clone nor a stamp.
//
// UNION goes the other way and owns no bracket at all: it drives the CLASSIC
// command id 32927 through Radiant_ExecCommand, exactly as kiwi_join.cpp's face
// arm does, so there is still ONE CSG-merge handler, ONE undo record and ONE
// console report in the editor.  Every refusal (patches, fixed-size entities,
// different owner entities, a union that is not convex) is CSG_Merge's own and is
// printed by CSG_Merge — this file does not second-guess it and does NOT fake a
// non-convex union.
// ─────────────────────────────────────────────────────────────────────────────

class KiwiEditorCommand;

void KiwiBool_RegisterCommands();
KiwiEditorCommand *KiwiBool_CommandForId( int commandId );

// §3 palette predicate: at least one brush that CSG can legally consume is
// selected.  The TOOL is picked inside the gesture, so it is not a precondition —
// the same shape ROUND L gave Cut.
bool KiwiBool_CanBoolean();

// KIWI-UX (CLEANUP, A-16): round AF's by-def cascade (DifferenceByDef, WouldCarve,
// PointInSolid) was withdrawn by round AP and deleted here — the extrude was its
// only caller, and Q carves a brush the user CLICKED through CarveTarget directly.
// The design record is in RADIANT_UX_DESIGN §19; the one part siblings depend on
// is the on-plane tolerance, which outlives it as KBOOL_ONPLANE_EPS below.

// KIWI-UX (CLEANUP, BoolPointTol): THE EDITOR'S ON-PLANE EPSILON, NAMED.
// "n·p - dist is this small, so the point counts as ON the face rather than
// outside it" — the tolerance the boolean's half-space tests carry, and the one
// kiwi_loft.cpp (KLOFT_ONPLANE_EPS) and kiwi_patchfillet.cpp (KFIL_ONPLANE_EPS)
// both cite as normative for their own copies.  It is named HERE, on the header,
// so those cites resolve to a constant rather than to a function.  Same value it
// has always been.
#define KBOOL_ONPLANE_EPS 0.01f

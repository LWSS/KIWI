#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_command.h — RADIANT_UX_DESIGN §3 (command metadata) + §4 (modal command
// framework) + §5 (input arbitration, the command half).
//
// ── ONE REGISTRY, NOT TWO ────────────────────────────────────────────────────
// `g_radiantCommands` (mainfrm.cpp) stays THE source of truth for "what commands
// exist and what key runs them", and `Radiant_ExecCommand` →
// `Radiant_DispatchCommandDirect` stays THE dispatcher.  This header adds only a
// PARALLEL METADATA TABLE keyed by command id (display name / category / the
// selection kinds the command wants / a canExecute predicate), plus the modal
// lifecycle the new commands need.  Nothing here duplicates dispatch: the palette
// runs classic ids through Radiant_ExecCommand and KIWI ids through KiwiCmd_Start.
//
// Commands with no metadata are still listed by the palette — they fall back to
// the name already in `g_radiantCommands` and the category "Classic".
//
// ── THE RESERVED ID RANGE ────────────────────────────────────────────────────
// 34000..34199 is the KIWI range (34000..34099 until shakeout D — see THE SECOND
// INSTANT BLOCK below).  Proof of non-collision (checked, not assumed):
//   * the ~313-case Radiant_DispatchCommandDirect switch uses 9, 10, 189..213,
//     1085, 32xxx, 33xxx, 35001..35042, 36100..36127 and 57601..57644, plus the
//     8000..8009 MRU range and the 60000..60767 texture-filter ranges;
//   * g_radiantCommandsDefault's 187 ids are a subset of those;
//   * res/resource.h has 486 #defines and NONE lands in 34000..34999.
// 34200..34999 is left free as headroom for later phases.
//
// Within the range:
//   34001..34029  instant commands (dispatch, act, done) — selection modes, the
//                 palette, the grid/snap helpers, the §16 construction planes and
//                 the §25 selection-expansion helpers
//   34030..34069  MODAL commands   (dispatch = KiwiCmd_Start, then a live gesture)
//   34100..34199  instant commands, BLOCK 2 (shakeout D onward)
// Allocated so far: instant-1 34001..34029 (FULL), modal 34030..34065,
//                   instant-2 34100..34120.
//                   Next free: instant 34121, modal 34066.
// (ROUND AF took modal 34064 for the Centre Box and 34065 for Loft — kiwi_loft.h.
//  FOUR modal ids are left, 34066..34069, and the modal block cannot grow: any id
//  inside 34030..34069 is routed to KiwiCmd_Start by KiwiCmd_Dispatch regardless of
//  what it is, so widening the block would swallow instant ids.  A sixth modal verb
//  past that needs a SECOND modal range and a second KiwiCmd_IsModalId clause.)
// (ROUND AB took instant 34120 for Auto Bool — kiwi_autobool.h.)
// (ROUND S took instant 34112 for the clipboard Cut that Ctrl+X now runs.)
// (ROUND M took instant 34111 for the camera's orthographic/perspective toggle.)
// (ROUND L took modal 34062 for the Q boolean — kiwi_boolean.h.)
// (ROUND J took modal 34060/34061 for Offset Curve and Fillet Corners, and
//  instant 34107..34110 for Duplicate, Focus Selection, Invert Hidden and Repeat
//  Last Command — the Plasticity verbs in kiwi_offset.h / kiwi_fillet.h /
//  kiwi_dupe.h / kiwi_focus.h / kiwi_visibility.h.)
// (Shakeout H took modal 34059 for Trim — kiwi_trim.h.)
// (Shakeout G took modal 34055..34058 for the four Plasticity modelling verbs —
//  Cut, Match Face, Face Split, Extrude Face — and instant 34106 for Join.)
// (Shakeout F took instant 34104/34105 for the construction selection's Join and
//  Delete — kiwi_conselect.h.)
// (Shakeout B took 34007/34008/34009 and 34018/34019 for the §9 window toggles —
//  the five gaps that were left between the earlier blocks.  Shakeout C took the
//  last three instant ids, 34027..34029, for the Shift+A add menu and the two
//  native View-menu toggles, and modal 34047..34054 for the §16b creation suite.)
//
// ── THE SECOND INSTANT BLOCK (shakeout D) ───────────────────────────────────
// The first instant block filled up in shakeout C, and shakeout D needs four more
// instant ids for the Ctrl+1..4 selection conversion (kiwi_selconv.h).  They may
// NOT take 34055+ out of the modal block's tail: KiwiCmd_Dispatch tests
// KiwiCmd_IsModalId FIRST, so ANY id inside 34030..34069 is routed to
// KiwiCmd_Start regardless of what it is (the same trap the Phase-6 "Pick
// Texture" note below records).  So block 2 opens at 34100, out of the headroom.
//
// THAT REQUIRED WIDENING THE DISPATCH GATE.  Radiant_DispatchCommandDirect
// (mainfrm.cpp) routed `cmdId >= 34000 && cmdId <= 34099` into KiwiCmd_Dispatch;
// an id at 34100 would have fallen through the whole switch and been silently
// dropped.  The gate now reads 34000..34199, with a // KIWI-UX comment recording
// the widening and re-stating the non-collision proof for the added hundred (the
// proof above already covers 34000..34999 — the compiled-in ids cluster nowhere
// near it).  KIWI_CMD_LAST below moved with it, so KiwiCmd_IsKiwiId still means
// "in the KIWI range".
//
// NOTE for Phase 6 (kiwi_uv.h repeats this): "Pick Texture" is an INSTANT command
// and therefore CANNOT live at 34047 — KiwiCmd_Dispatch tests KiwiCmd_IsModalId
// FIRST, so any id inside 34030..34069 is routed to KiwiCmd_Start regardless of
// what it is.  It takes 34006, the next free instant id.
//
// ── MODAL LIFECYCLE (§4) ─────────────────────────────────────────────────────
//   Begin        → latch state, open the undo bracket if the command mutates
//   MouseMove    → live update from the pick + snap under the cursor (HOT only)
//   KeyDown      → axis locks / numeric entry (numeric is fed in for you)
//   Rebase       → re-latch the input mapping at the cursor (a PAUSE→HOT edge)
//   Commit       → close the bracket: ONE undo record per gesture
//   Cancel       → exact restore
// Exactly one command is active at a time (`g_activeCommand` in the .cpp).
//
// ── HOT vs PAUSED (shakeout E — THE CONFIRM FLOW REWORK) ────────────────────
// USER DIRECTIVE, verbatim: "Releasing an action shouldn't commit it, it should
// just pause the wip move.  A right click OR an enter press confirms it."
//
// That is exactly Plasticity's shape: its gizmo state machine ends a drag on
// pointer-up but the COMMAND keeps running (AbstractGizmo.ts:135-144 —
// `onPointerUp` resolves the gizmo's promise only when the caller asked for
// Mode.Persistent to be OFF; the default IS Persistent, line 92), and the command
// itself is finished by a separate explicit signal ("gizmo:finish",
// AbstractGizmo.ts:117-120).  Releasing the mouse parks the edit; something else
// ends it.
//
// So a modal command now has two states, owned by this file:
//
//   HOT      geometry follows the cursor.  The state a command STARTS in.
//   PAUSED   the preview stays exactly where it was, the value is latched, and
//            KiwiCmd_MouseMove stops reaching the command entirely.
//
//   HOT    --LMB press (no gizmo handle under it)--> PAUSED      [KiwiCmd_MouseButton]
//   HOT    --gizmo-handle RELEASE--------------->    PAUSED      [KiwiGizmo_Release]
//   PAUSED --ANY LMB press in the viewport------>    HOT+Rebase  [KiwiCmd_MouseButton]
//   PAUSED --gizmo-handle PRESS----------------->    HOT+Rebase  [KiwiGizmo_MouseDown]
//   either --RMB CLICK (no drag) / Enter-------->    COMMIT
//   either --Esc-------------------------------->    numeric-clear, then CANCEL
//
// THE RESUME RULE IS DELIBERATELY THE BROADEST ONE: *any* LMB press inside the
// camera image resumes, not just a press on the geometry.  A press on the gizmo
// resumes AND grabs that handle; a press anywhere else resumes the free drag.
// The alternative ("only a press on the dragged geometry resumes") needs a pick
// against a preview that may not be geometry yet — a construction line, a
// primitive that has not been landed — and would leave the user with a command
// they cannot get back into.  Broadest wins; Esc is always one key away.
//
// MULTI-CLICK TOOLS (WantsClicks — the drawing tools and the primitives) DO NOT
// PAUSE.  A click there PLACES A POINT; that is their whole grammar and it is
// unchanged, byte for byte.  They gain only the new confirm: RMB-click now does
// what Enter does.
//
// ── INPUT ARBITRATION (§5) ───────────────────────────────────────────────────
// The active command outranks selection and hover, but NEVER camera navigation:
// kiwi_viewport.cpp routes LMB and mouse-move-for-preview into the command while
// letting MMB orbit, the wheel dolly and RMB-DRAG (pan since shakeout E; Alt+RMB
// mouselook) straight through.  Losing orbit mid-gesture is the exact failure
// this ordering exists to prevent.
//
// The ONE place RMB now reaches a command is the no-drag RMB CLICK (travel under
// KVP_RMB_CLICK_PIXELS): with a command live that is the CONFIRM, and the legacy
// entity context menu is not replayed — a context menu about the selection is
// meaningless in the middle of an edit to it.  With no command live it is the
// classic context menu, exactly as shakeout A left it.
//
// ── UNDO (§4 "preview strategy", verified against undo.cpp) ──────────────────
// The ported bracket protocol, mirrored from two real call sites —
// drag.cpp:377-378 (`Undo_Start("drag selection"); Undo_AddBrushList(&selected_-
// brushes);`) and drag.cpp:1705-1706 (`Undo_EndBrushList(&selected_brushes);
// Undo_End();`), with select.cpp:1873-1874 confirming the expanded head
// (`Undo_ClearRedo(); Undo_GeneralStart(op);` — Undo_Start is exactly those two):
//
//   BEGIN   Undo_ClearRedo()  →  Undo_GeneralStart(op)  →  Undo_AddBrushList(list)
//   COMMIT  Undo_EndBrushList(list)  →  Undo_End()
//   CANCEL  Undo_EndBrushList(list)  →  Undo_End()  →  Undo_Undo()  →  Undo_ClearRedo()
//
// WHAT A COMMAND MUST FEED:
//   * brushes it will move/reshape → Undo_AddBrushList(&selected_brushes) covers
//     the whole selection (that is what KiwiCmd_UndoBegin does).  A command that
//     touches brushes OUTSIDE the selection must additionally call
//     Undo_AddBrush(def) for each, BEFORE mutating it.
//   * entities whose key/values it edits → Undo_AddEntity_W(ent) (that variant
//     also folds in the entity's own brush defs).
//   Nothing may be added after Undo_End(); adding brushes after an entity prints a
//   warning (undo.cpp:539), so add brushes first.
//
// `operation` IS STORED BY POINTER (undo.cpp:390 `_undo->operation = operation`)
// and the record outlives the gesture, so it MUST be a string literal / static.
//
// A command that mutates nothing must NOT open a bracket at all — an empty record
// would make one Ctrl+Z do nothing.
// ─────────────────────────────────────────────────────────────────────────────

#include "kiwi_numeric.h"                // kiwiNumField_t (the shakeout-E field table)
#include "kiwi_pick.h"
#include "kiwi_snap.h"

// ── the reserved range ───────────────────────────────────────────────────────
#define KIWI_CMD_FIRST              34000
#define KIWI_CMD_LAST               34199   // shakeout D: was 34099 (instant block 2)
#define KIWI_CMD_MODAL_FIRST        34030
#define KIWI_CMD_MODAL_LAST         34069

#define KIWI_CMD_SELMODE_POINT      34001   // §11 selection-mode chips, modern keys 1..5
#define KIWI_CMD_SELMODE_EDGE       34002
#define KIWI_CMD_SELMODE_FACE       34003
#define KIWI_CMD_SELMODE_OBJECT     34004
#define KIWI_CMD_SELMODE_ALL        34005
#define KIWI_CMD_PALETTE            34010   // §15 command palette (modern F)
#define KIWI_CMD_GRID_HALVE         34020   // §17 modern grid spacing, modern [
#define KIWI_CMD_GRID_DOUBLE        34021   // §17 modern grid spacing, modern ]
#define KIWI_CMD_SNAP_TOGGLE        34022   // §6  snap markers on/off
#define KIWI_CMD_CPLANE_XY          34011   // §16 construction planes (kiwi_construct)
#define KIWI_CMD_CPLANE_XZ          34012
#define KIWI_CMD_CPLANE_YZ          34013
#define KIWI_CMD_CPLANE_FACE        34014
#define KIWI_CMD_CPLANE_VIEW        34015
#define KIWI_CMD_CONSTRUCT_CLEAR    34016   // §7  drop every construction object
#define KIWI_CMD_CONSTRUCT_UNDO     34017   // §7  pop the construction store's own undo
#define KIWI_CMD_SELECT_COPLANAR    34023   // §25 selection expansion (kiwi_selext)
#define KIWI_CMD_SELECT_TOUCHING    34024
#define KIWI_CMD_SELECT_MATERIAL    34025
#define KIWI_CMD_SELECT_CONNECTED   34026   // …also the camera double-click action
#define KIWI_CMD_PICK_TEXTURE       34006   // §26 pick the material under the 3D cursor (kiwi_uv)
// §9 shakeout B — the native "Windows" popup's five per-window visibility toggles
// (kiwi_windows.h owns the flags, the ini persistence and the CheckMenuItem sync).
#define KIWI_CMD_WINDOW_XY          34007   // "2D View"
#define KIWI_CMD_WINDOW_Z           34008   // "Z"
#define KIWI_CMD_WINDOW_TEXTURE     34009   // "Textures"
#define KIWI_CMD_WINDOW_CONSOLE     34018   // "Console"
#define KIWI_CMD_WINDOW_SHELL       34019   // "KIWI ImGui shell"
// §16b shakeout C — the Shift+A add menu and the two native View-menu toggles.
#define KIWI_CMD_ADD_MENU           34027   // §16b creation palette at the cursor (kiwi_addmenu)
#define KIWI_CMD_VIEW_SHOW_GRID     34028   // §17 grid on/off, mirrored in the native View menu
#define KIWI_CMD_VIEW_SHOW_AXES     34029   // §17 axes on/off, likewise
#define KIWI_CMD_SELFTEST           34030   // MODAL: "UX: Modal Self-Test" (palette-only)
#define KIWI_CMD_MOVE               34031   // MODAL: §13 G — context-aware move (kiwi_transform)
#define KIWI_CMD_ROTATE             34032   // MODAL: §13 R — whole-selection rotate
#define KIWI_CMD_SCALE              34033   // MODAL: §13 S — whole-selection scale
#define KIWI_CMD_DRAW_LINE          34034   // MODAL: §7 drawing tools (kiwi_construct)
#define KIWI_CMD_DRAW_POLYLINE      34035
#define KIWI_CMD_DRAW_RECT          34036
#define KIWI_CMD_DRAW_CIRCLE        34037
#define KIWI_CMD_DRAW_ARC           34038
#define KIWI_CMD_EXTRUDE_REGION     34039   // MODAL: §23 region → brushes (kiwi_extrude)
#define KIWI_CMD_BEVEL_EDGE         34040   // MODAL: §25 bevel/chamfer (kiwi_bevel)
#define KIWI_CMD_INSET_FACE         34041   // MODAL: §25 inset, the clone compound (kiwi_bevel)
#define KIWI_CMD_ARRAY_LINEAR       34042   // MODAL: §25 arrays (kiwi_dupe)
#define KIWI_CMD_ARRAY_RADIAL       34043
#define KIWI_CMD_TEX_SHIFT          34044   // MODAL: §26 texture shift  (kiwi_uv)
#define KIWI_CMD_TEX_ROTATE         34045   // MODAL: §26 texture rotate (kiwi_uv)
#define KIWI_CMD_TEX_SCALE          34046   // MODAL: §26 texture scale  (kiwi_uv)
// §16b shakeout C — the rest of the creation suite mapped out of the Plasticity
// inventory (RADIANT_UX_DESIGN §16b.3).  The four curve tools live next to the
// original five in kiwi_construct.cpp; the four SOLIDS land real brushes and live
// in kiwi_primitive.cpp.
#define KIWI_CMD_DRAW_RECT_CENTER   34047   // MODAL: centre + corner   (kiwi_construct)
#define KIWI_CMD_DRAW_CIRCLE_2PT    34048   // MODAL: diameter endpoints
#define KIWI_CMD_DRAW_POLYGON       34049   // MODAL: centre + radius, 3..32 sides
#define KIWI_CMD_DRAW_SPLINE        34050   // MODAL: Catmull-Rom through clicked points
#define KIWI_CMD_PRIM_BOX           34051   // MODAL: §16b solids (kiwi_primitive)
#define KIWI_CMD_PRIM_CYLINDER      34052
#define KIWI_CMD_PRIM_SPHERE        34053
#define KIWI_CMD_PRIM_CONE          34054
// ── SHAKEOUT G — the Plasticity MODELLING verbs ─────────────────────────────
// Four modal commands, all gated on a selection rather than on a cursor position,
// and all on the shakeout-E HOT/PAUSED confirm flow.  The keymap audit for C / Z /
// Ctrl+R / E (and the bare J the instant Join below takes) is in kiwi_keymap.h.
#define KIWI_CMD_CUT                34055   // MODAL: C      solid + construction line -> 2 solids
#define KIWI_CMD_MATCH_FACE         34056   // MODAL: Z      make the source face coplanar with a picked one
#define KIWI_CMD_SPLIT_FACE         34057   // MODAL: Ctrl+R split the owning brush across a face
#define KIWI_CMD_EXTRUDE_FACE       34058   // MODAL: E      face winding -> a NEW brush, original untouched
// ── SHAKEOUT H — TRIM (kiwi_trim.h) ─────────────────────────────────────────
// Modal, multi-click, construction lines only.  Bare T in the modern profile;
// the vk 0x54 audit and the ViewTextures displacement are in kiwi_keymap.h.
#define KIWI_CMD_TRIM               34059   // MODAL: T      remove a line's span between crossings
// ── INSTANT BLOCK 2 (shakeout D) — Ctrl+1..4 selection conversion ───────────
// kiwi_selconv.h carries the Plasticity semantics these port and the four places
// KIWI's mapping deliberately differs.
#define KIWI_CMD_SELCONV_POINT      34100   // Ctrl+1  selection -> its points
#define KIWI_CMD_SELCONV_EDGE       34101   // Ctrl+2  selection -> its edges
#define KIWI_CMD_SELCONV_FACE       34102   // Ctrl+3  selection -> its faces
#define KIWI_CMD_SELCONV_OBJECT     34103   // Ctrl+4  selection -> its owning objects
// Shakeout F — the construction SELECTION's two verbs (kiwi_conselect.h).  Both
// instant: neither has a live gesture, they act and are done.  MOVE is not here
// because it is not a new command — G grows a construction arm instead
// (kiwi_transform.cpp), so there is still exactly one "Move" in the editor.
#define KIWI_CMD_CONSTRUCT_JOIN     34104   // Ctrl+J  chain the selected lines into one polyline
#define KIWI_CMD_CONSTRUCT_DELETE   34105   // (Delete, via the key funnel — see below)
// Shakeout G — the CONTEXT verb J (kiwi_join.h).  Instant: it inspects the two
// selections, runs one of two ported cores and is done.  Ctrl+J stays bound to
// KIWI_CMD_CONSTRUCT_JOIN above as the lines-only alias.
#define KIWI_CMD_JOIN               34106   // J  faces -> CSG_Merge, lines -> JoinLines
// ── ROUND J — the last Plasticity verbs this overhaul was missing ───────────
// Two MODAL construction-curve editors and four INSTANT commands.  The keymap
// audit for O / B / Shift+D / "/" / Ctrl+H / Shift+R is in kiwi_keymap.h; the
// Plasticity sources each one ports are cited in its own header.
#define KIWI_CMD_OFFSET_CURVE       34060   // MODAL: O   parallel copy of a construction chain
#define KIWI_CMD_FILLET_CURVE       34061   // MODAL: B   round a chain's corners into arcs
// ── ROUND L — Q, the BOOLEAN verb (kiwi_boolean.h) ──────────────────────────
// Difference and union over classic brushes, with the tool solid picked by a
// click inside the gesture.  Plasticity binds `q` to `command:boolean` bare
// (default-keymap.ts:294); the vk 0x51 audit is in kiwi_keymap.h.
#define KIWI_CMD_BOOLEAN            34062   // MODAL: Q   difference / union of solids
// ROUND Q — chamfer a brush edge and lay a q3 bezier patch into the notch
// (kiwi_patchfillet.h).  UNBOUND in the key table on purpose: B reaches it
// through KiwiPatchFillet_ContextB, which redirects KIWI_CMD_FILLET_CURVE when
// brush EDGES are selected — one key, one concept, arbitrated by the selection
// the way J already is.  The palette lists it by name and runs it directly.
#define KIWI_CMD_FILLET_EDGE        34063   // MODAL: B (brush edges) fillet -> patch
#define KIWI_CMD_DUPLICATE          34107   // Shift+D  clone the selection, then Move PAUSED
#define KIWI_CMD_FOCUS_SELECTION    34108   // /        frame the selection (kiwi_focus.h)
#define KIWI_CMD_HIDE_INVERT        34109   // Ctrl+H   swap hidden and visible (kiwi_visibility.h)
#define KIWI_CMD_REPEAT_LAST        34110   // Shift+R  re-run the last KIWI command
// ROUND M: the camera's projection toggle.  Unbound by default — the pill under
// the view cube is the primary control (kiwi_viewcube.h) and this is the palette /
// View-menu / remappable route to the same KiwiCam_SetOrtho call.
#define KIWI_CMD_VIEW_ORTHO         34111   // toggle orthographic / perspective 3D camera
// ── ROUND S: CUT, THE CLIPBOARD ONE ─────────────────────────────────────────
// USER DIRECTIVE: "Ctrl-X should not quit the app!!!"  It used to (the .rc
// accelerator bound Ctrl+X to ID_FILE_EXIT_RAD 32951 — res/radiant.rc records the
// removal).  The modern profile gives the key the meaning every other application
// gives it.  There is NO ported clipboard-cut core to call: the binary ships Copy
// (33039 -> XYWnd_CopyClip) and Paste (33040) and nothing between them, so this id
// is Copy followed by the ported Delete Selection (33003), in ONE undo bracket.
#define KIWI_CMD_CLIP_CUT           34112   // Ctrl+X   copy the selection, then delete it
// ROUND T — "Once a chamfer/bevel is made, allow (DEL) key to remove the chamfer
// and restore the original edge/corner."  An INSTANT verb (no drag, no preview),
// reached from the Delete-key funnel below and from the palette by name; the rule
// and its §19 validation are stated in kiwi_bevel.h REMOVE FACE (RESTORE EDGE).
#define KIWI_CMD_REMOVE_FACE        34113   // Delete   one selected FACE -> gone, edge back
// ROUND U — the CONSTRUCTION half of the hide family (kiwi_conselect.h).  USER
// DIRECTIVE: "We dont have a hide mechanic right now, you should add that(H) and
// add (unhide all) in the search menu".  H itself is arbitrated in the key funnel
// (KiwiConSel_OwnsHide) exactly as Delete already is, because the key is SHARED
// with the ported Hide Selected (32923) and a mixed selection must hide BOTH.
// These two ids are the palette's route to the same two acts.
#define KIWI_CMD_CONSTRUCT_HIDE     34114   // (H, via the key funnel)
#define KIWI_CMD_CONSTRUCT_UNHIDE   34115   // "Unhide All (construction)"
// ── ROUND W — the OUTLINER (kiwi_outliner.h) ────────────────────────────────
// USER DIRECTIVE: "Please create a collapsible giant list of all brushes on the
// left like it's Plasticity.  Support groups in this list."  One window toggle
// (the §9 table owns the flag) and the two group verbs the panel header exposes,
// listed here so the palette can reach them without the panel being open.
#define KIWI_CMD_WINDOW_OUTLINER    34116   // "Outliner"
#define KIWI_CMD_GROUP_CREATE       34117   // selection -> a func_group / construction group
#define KIWI_CMD_GROUP_UNGROUP      34118   // dissolve the selection's groups
// ── ROUND Y — the permanent MATERIAL-STATE readout (kiwi_material.h) ────────
// USER REPORT: "z-order bug is still here … (Maybe material related??)".  It was,
// for the third round running, and each round had to guess which material and
// what state.  This prints the ground truth — techset, sortKey, the technique the
// camera binds and its decoded depthTest/depthWrite — for the face under the
// cursor and for the current material TEMPLATE.  Unbound; palette / command-list
// only, because it is a diagnostic and not a modelling verb.
#define KIWI_CMD_MATINFO            34119   // "Material info (under cursor)"
// ── ROUND AB — AUTO BOOL (kiwi_autobool.h) ──────────────────────────────────
// USER REQUEST: "You should add an 'auto bool' that attempts to consolidate a
// large group of brushes all at once to reduce brush-count."  Greedy pairwise
// CSG merge to a fixed point over the selection, one undo record for the run.
// INSTANT (block 2), and registered UNBOUND: the user's own "might be a bad idea"
// is reason enough not to give it a key it can be hit by accident.
#define KIWI_CMD_AUTO_BOOL          34120   // "Auto Bool (consolidate brushes)"
// ── ROUND AF — the two new creation verbs (RADIANT_UX_DESIGN §60) ───────────
// ITEM 8: the CENTRE box, which shakeout F predicted would land here ("the chord
// will not have to move when the centre box does ship", kiwi_keymap.cpp:314-322).
// Plasticity ships corner-box and centre-box as SEPARATE commands on shift+c /
// shift+v (BoxCommand.ts:60/:155, default-keymap.ts:288-289) and KIWI now does
// too — kiwi_primitive.h states the semantics and kiwi_keymap.h the displacement.
#define KIWI_CMD_PRIM_BOX_CENTER    34064   // MODAL: Shift+V  centre + half-extents
// ITEM 6: LOFT — bridge two brush faces (kiwi_loft.h).  Bare L, Plasticity's own
// chord (default-keymap.ts:270); the vk 0x4C audit is in kiwi_keymap.h.
#define KIWI_CMD_LOFT               34065   // MODAL: L        face + face -> a bridge
// §25 mirror is NOT here: Select_FlipAxis / DoFlip are already ported AND already
// wired to the classic ids 32956 / 32957 / 32958, so Phase 5 adds only palette
// metadata over those (kiwi_dupe.h explains).

inline bool KiwiCmd_IsKiwiId ( int id ) { return id >= KIWI_CMD_FIRST && id <= KIWI_CMD_LAST; }
inline bool KiwiCmd_IsModalId( int id ) { return id >= KIWI_CMD_MODAL_FIRST && id <= KIWI_CMD_MODAL_LAST; }

// ── §3 metadata, keyed by EXISTING command ids ───────────────────────────────
struct kiwiCommandInfo_t
{
    const char *displayName;    // "Extrude Region" — palette label
    const char *category;       // "Modeling" / "Selection" / "KIWI" / "Classic"
    sel_mask_t  selKindMask;    // the selection kinds this command is meaningful for (0 = any)
    bool      (*canExecute)();  // NULL = always available.  Greys the palette row.
};

// The metadata for a command id, or NULL when it has none (→ palette falls back
// to the g_radiantCommands name and category "Classic").
const kiwiCommandInfo_t *KiwiCmd_Info( int commandId );

// Convenience for the palette: metadata's canExecute, or true when there is none.
bool KiwiCmd_CanExecute( int commandId );

// ── registration into the shared table (called BY mainfrm.cpp's seeder) ──────
// Appends the KIWI rows to `g_radiantCommands` so hotkeys, the palette, the
// command-list panel and the radiant.ini [Commands] remap all see them.  The
// bindings registered here are the CLASSIC-profile ones (mostly unbound);
// kiwi_keymap.cpp applies the modern profile on top.
void KiwiCmd_RegisterCommands();

// ── §3 dispatch tail for the reserved range (called BY mainfrm.cpp) ──────────
// Instant ids act immediately; modal ids call KiwiCmd_Start.
bool KiwiCmd_Dispatch( unsigned int cmdId );

// ── SHAKEOUT G: the POST-DISPATCH tail on the classic Paste (33040) and Clone
// (33001), called from mainfrm.cpp's switch as a // KIWI-UX one-liner after each
// ported handler.  Starts the Move command PAUSED over whatever the paste/clone
// left selected.  The long note is on the definition in kiwi_command.cpp.
void KiwiCmd_AfterPaste();

// ── ROUND J: REPEAT LAST COMMAND (Plasticity Shift+R) ───────────────────────
// Plasticity's `edit:repeat-last-command` is two lines (CommandExecutor.ts:58-61):
// it re-CONSTRUCTS the last Command class it ran and enqueues it.  `lastCommand`
// is set only by the executor, i.e. only for things that are Command SUBCLASSES —
// its viewport actions (`viewport:focus`, `viewport:grid:*`), its selection-mode
// and selection-convert actions and its `edit:*` actions are plain callbacks on
// the editor and never touch it (Editor.ts:214 is itself one of them).
//
// KIWI mirrors exactly that split.  KiwiCmd_Dispatch records the id it just ran,
// EXCEPT for the ids that correspond to Plasticity's non-`command:` verbs — the
// selection modes and conversions, the palette, the grid/snap helpers, the
// construction-plane and window/view toggles, the unified undo, Focus, and Repeat
// itself.  Everything left is a modelling / creation verb, which is precisely the
// set a mapper wants to repeat.
//
// CLASSIC ids are NOT recorded and deliberately so: Radiant_DispatchCommandDirect
// is ported code and hooking every one of its ~313 cases to record a "last
// command" would be a ported-logic change for a feature none of them ask for.
// The two classic ids that DO matter here (Clone 33001 and Paste 33040) already
// have KIWI equivalents or tails.
// ── ROUND N: the §17 grid spacing stepper, shared by the keys and the readout ─
// Step the modern grid spacing, clamped to the [ ] range and reported on the
// console.  KIWI_CMD_GRID_DOUBLE / _HALVE (the [ ] and PageUp / PageDown keys) and
// the top-right grid readout (kiwi_viewcube.cpp) all land here, so there is
// exactly one clamp and one message in the editor.
//
// ── ROUND R: IT IS A NICE-NUMBER LADDER, NOT A DOUBLING ─────────────────────
// USER DIRECTIVE, verbatim: "let me type in the grid spacing manually, also it
// should be whole numbers by default with the arrows."  Halving and doubling makes
// EVERY value reachable from a fractional one fractional — start at 10 and one
// press of [-] gives 5, two gives 2.5, three gives 1.25, which is the number the
// user was looking at.  The step is now a walk along a fixed table of round
// numbers (KCMD_GRID_LADDER in kiwi_command.cpp), so the arrows can only ever
// land on one of them and a value typed off the ladder still steps to the
// nearest rung above (up) or below (down) rather than doubling from where it is.
void KiwiCmd_StepGrid( bool doubleIt );

// ROUND R: the TYPED spacing (the grid pill's input popup, kiwi_viewcube.cpp).
// ANY positive value is legal — §17 says so out loud, "ANY positive value,
// deliberately not restricted to powers of two" — so this does NOT snap to the
// ladder; it only applies the same [ ] clamp and prints the same console line.
// Returns false (and changes nothing) for a value that is not positive.
bool KiwiCmd_SetGridSpacing( float inches );

bool KiwiCmd_HasRepeatable();
const char *KiwiCmd_RepeatLabel();       // display name of the recorded id, or NULL
bool KiwiCmd_RepeatLast();               // false = nothing recorded / it refused

// ROUND S: the clipboard CUT that Ctrl+X now runs — ported Copy (33039) followed
// by ported Delete Selection (33003), which carries the undo record.  No-op with
// an empty selection (the clipboard is left alone).  See the .cpp for the whole
// story of what Ctrl+X used to do.
void KiwiCmd_ClipCut();

// ── shakeout I: one keyboard prompt for the bottom-left strip (kiwi_hints.h) ─
// A keycap plus what it does.  Both point at literals / statics.
struct kiwiPrompt_t
{
    const char *key;
    const char *label;
};

// ── KIWI-UX (ROUND AI, ITEM 3): one row of the in-command options panel ──────
// See KiwiEditorCommand::CommandOptions for the whole rationale.  Four kinds,
// which is the complete set the existing commands need:
//
//   KOPT_ENUM     a segmented button group — Plasticity's radio+label row
//                 (FilletDialog.tsx:78-81).  `choices` is `choiceCount` labels.
//   KOPT_TOGGLE   a single on/off pill.  (An enum of two named states is usually
//                 clearer and is what loft uses; this is for a real on/off.)
//   KOPT_INT      a `-  N  +` stepper clamped to [lo, hi].
//   KOPT_NUMFIELD a scalar row that mirrors the command's NUMERIC FIELD `field` —
//                 it READS NumericFieldValue and WRITES NumericFieldChanged, so
//                 the panel and the Tab-and-type path are the same state.
//                 `lo`/`hi` are the display-unit clamps for the slider.
//
// `enabledBy` is an option index whose value must be non-zero for this row to be
// live, or -1 for "always".  It is how loft greys `tension` out in RULED mode —
// Plasticity does the same with a `class="disabled"` row (FilletDialog.tsx:73).
enum kiwiOptKind_t
{
    KOPT_ENUM = 0,
    KOPT_TOGGLE,
    KOPT_INT,
    KOPT_NUMFIELD
};

struct kiwiOption_t
{
    const char        *label;
    kiwiOptKind_t      kind;
    const char *const *choices;      // KOPT_ENUM only
    int                choiceCount;  // KOPT_ENUM only
    int                field;        // KOPT_NUMFIELD: the NumericFields index
    float              lo, hi;       // KOPT_INT / KOPT_NUMFIELD clamps
    int                enabledBy;    // option index gating this row, or -1
};

// ── §4 the modal command interface ───────────────────────────────────────────
class KiwiEditorCommand
{
public:
    virtual ~KiwiEditorCommand() {}

    virtual const char *Name() const = 0;                 // HUD label; static storage
    virtual bool CanExecute()                    { return true; }

    // false = refuse to enter (nothing was changed, no bracket was opened).
    virtual bool Begin()                         { return true; }

    // One per frame while the gesture runs: what is under the cursor, and where
    // the SnapManager says the point actually is.
    virtual void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) { (void)pick; (void)snap; }

    // Axis locks etc.  Return true to CONSUME.  Digits / '.' / '-' / backspace are
    // taken by the numeric entry BEFORE this is called; Esc and Enter are taken by
    // the framework AFTER it, so a command may still veto them by consuming here.
    // (Phase 4 made the implementation match this contract — see the note on the
    // Esc ladder in KiwiCmd_KeyDown.)
    virtual bool KeyDown( int vk, unsigned int mods ) { (void)vk; (void)mods; return false; }

    // The numeric entry changed.  `has` false = cleared.  `world` is already in RAW
    // WORLD UNITS (Units_FromDisplay applied) — never re-convert it.
    //
    // This is the SINGLE-SCALAR hook and it is unchanged: the framework reaches it
    // through NumericFieldChanged's default body, for field 0 only.
    virtual void NumericChanged( bool has, float world ) { (void)has; (void)world; }

    virtual void Commit()                        {}
    virtual void Cancel()                        {}

    // Cam_Draw-tail overlay for the live gesture (kiwi_lines).  Budget your own batch.
    virtual void DrawWorld()                     {}

    // ── KIWI-UX (ROUND AF, ITEM 3): A COMMAND MAY ASK FOR A BIGGER LINE BATCH ──
    // USER REPORT, verbatim: "When adding a bunch of objects to a boolean (windows
    // on a building), After about 10-12, the red previewer stopped working on newly
    // selected diff tools.  I still clicked all of them and the operation worked as
    // expected, but the previewer always broke."
    //
    // THE ARITHMETIC IS THE DIAGNOSIS.  KiwiCmd_DrawWorld opens ONE 288-segment
    // batch for the whole gesture (kiwi_command.cpp), a box brush costs 6 faces x 4
    // edges = 24 segments, and 288 / 24 = TWELVE.  Every overlay whose cost is a
    // FUNCTION OF A SET THE USER IS STILL GROWING therefore has a hard cliff built
    // into a constant that predates the set — for the boolean's tool list, at
    // exactly the count the report names.
    //
    // A command that owns such a set answers here with the segments it actually
    // wants for the CURRENT state; the framework clamps to KCMD_LINE_BUDGET_MAX and
    // uses the larger of that and its own default.  0 (the default) means "the
    // framework's number", which is every other command, byte for byte.
    //
    // This does NOT license silent truncation past the ceiling — a command whose
    // want exceeds it must still degrade in a way the user can see and understand
    // (kiwi_boolean.cpp draws the NEWEST operands first and says so once on the
    // console).  The budget exists because the render-command buffer really can
    // overflow (kiwi_lines.h TRAP 1); it is not an excuse for a blank preview.
    virtual int LineBudget() const               { return 0; }

    // ── Phase 3 additions ───────────────────────────────────────────────────
    // Pick/snap flags the FRAMEWORK uses when it builds this command's MouseMove
    // inputs (kiwi_pick.h).  A transform returns PICKF_EXCLUDE_SELECTED so it can
    // never snap or pick the geometry it is currently dragging to itself.
    virtual unsigned PickFlags() const           { return PICKF_NONE; }

    // §13 HUD: an extra fragment appended to the numeric HUD line — the op's
    // context, its constraint and its live value ("faces  axis X  64 in").
    // NULL = nothing extra.  The storage must outlive the draw (the HUD does not
    // copy); a command owning a char buffer is the expected shape.
    virtual const char *HudStatus() const        { return 0; }

    // True while the §19 validity gate is rejecting the current state.  The HUD
    // turns red and the command is expected to treat Commit as Cancel.
    virtual bool HudInvalid() const              { return false; }

    // ── Phase 4 additions: MULTI-CLICK tools ────────────────────────────────
    // The Phase-2/3 rule is "one LMB click commits the gesture", which is right
    // for a transform (the click ends a drag) and wrong for a drawing tool (the
    // click PLACES A POINT).  A command that returns true here receives Click()
    // instead of being committed for it; everything else is unchanged, byte for
    // byte, because the default is false.
    virtual bool WantsClicks() const             { return false; }

    // Only called when WantsClicks() is true.  The framework has ALREADY run this
    // pixel through MouseMove, so the command reads its own latched state rather
    // than the click position.  Return true to keep the gesture running, false to
    // COMMIT it (which is what the framework would have done unconditionally).
    virtual bool Click()                         { return true; }

    // ══════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AQ, ITEM 8) — ENTER ADVANCES A STAGE, IT DOES NOT
    //                               EARLY-COMMIT ONE.
    // ══════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: "When creating an object (like a cylinder, but keep
    // ALL in mind), I tab, type in 120, then press enter.  This should confirm
    // step1 of the creation operation, not submit it early with 0 z height (thus
    // invalidating it)."
    //
    // WHY IT DID THE WRONG THING.  Enter reaches KiwiCmd_KeyDown's rung 5 and
    // calls KiwiCmd_Commit unconditionally, and a staged creation tool's Commit
    // refuses outright when its stage counter has not reached the last one
    // ("not enough points — nothing created").  So typing a radius and pressing
    // Enter destroyed the gesture: the most natural way to say "yes, that
    // number" was the one input that guaranteed nothing would be built.  Only
    // the MOUSE could advance a stage, which makes the typed field decorative on
    // every tool with more than one of them.
    //
    // THE RULE, stated once and enforced in exactly one place (rung 5):
    //   * Enter with a PENDING TYPED VALUE consumes that value — the command
    //     folds it into the current stage and advances, rebasing whatever the
    //     next stage maps the cursor to.  The gesture stays live.
    //   * Enter with NO typed value keeps its old meaning exactly: confirm.
    //   * Enter with typed text that does not EVALUATE (round AQ item 4's
    //     expressions make that a real state — "12 *") advances nothing and
    //     commits nothing; it is swallowed, so a half-typed number can never
    //     early-commit either.
    // A second Enter therefore always confirms, because the first one cleared the
    // field on its way through.
    //
    // Returning false — the default — means "I have no stage to advance", which
    // is the right answer for every single-stage command and for the last stage
    // of a staged one, and it lands those on the ordinary commit path unchanged.
    // Overriders must ONLY advance and must not commit; the framework owns that.
    virtual bool AdvanceStage()                  { return false; }

    // ── KIWI-UX (ROUND AA, ITEM 9): THE MARQUEE, LENT TO A LIVE COMMAND ─────
    // USER REPORT, verbatim: "the difference command needs to accept multiple
    // shift-clicked brushes.  For example, if I'm making a sidewalk and I want to
    // boolean each crack, I have to do it 1 by 1 current, that's awful.  Also
    // support shift box select."
    //
    // THE GAP THIS CLOSES.  The marquee is UNREACHABLE while any command is live:
    // kiwi_viewport.cpp's LMB arm hands the press to KiwiCmd_MouseButton and
    // returns long before the KiwiBox_Begin arm at the bottom of that function.
    // That is correct for every command that existed before this round — a press
    // during a transform means "resume / park the drag", never "select something"
    // — so widening it unconditionally would break all of them.  A command that
    // BUILDS A SET out of clicks (today: the boolean's tool set) is the exception,
    // and it says so here.
    //
    // The rung is SHIFT-ONLY at the dispatch site, because Shift is additive
    // everywhere in this editor (the marquee's own grammar, the no-command click
    // grammar, IdlePressReselect) and a BARE press must keep meaning whatever the
    // command already made it mean.  A shift press under the click threshold is
    // handed back to Click() as an ordinary press, so opting in never costs a
    // command its shift-CLICK.
    virtual bool WantsMarquee() const            { return false; }

    // The resolved rect, in camera-RTT image space (TOP-LEFT origin, kiwi_pick.h's
    // convention).  x0/y0 is the PRESS corner and x1/y1 the RELEASE corner — NOT
    // normalised, because `crossing` is derived from their order and the callee may
    // want the direction too.  `crossing` is true for a right-to-left drag, i.e.
    // "anything the rect touches" rather than "only what it fully encloses"
    // (kiwi_boxselect.h §12).  The command resolves the rect into geometry itself,
    // through KiwiBox_CollectBrushes — the framework owns no selection policy.
    //
    // Only called when WantsMarquee() is true.  The framework has NOT run this
    // through MouseMove: a marquee names a region, not a point, so there is no
    // cursor answer to latch first.
    virtual void Marquee( int x0, int y0, int x1, int y1, bool crossing, bool shift )
    { (void)x0; (void)y0; (void)x1; (void)y1; (void)crossing; (void)shift; }

    // ── Shakeout E additions: NAMED NUMERIC FIELDS + the value bubble ───────
    //
    // WHY THIS IS A SECOND VIRTUAL AND NOT A DEFAULTED THIRD PARAMETER.  The
    // obvious change is `NumericChanged( bool, float, int field = 0 )`, and it
    // does not work: a default argument does not make an override match, so every
    // existing `void NumericChanged( bool, float ) override` in kiwi_transform /
    // kiwi_construct / kiwi_primitive / kiwi_extrude / kiwi_bevel / kiwi_dupe /
    // kiwi_uv would stop overriding anything and fail to compile.  Adding a
    // SEPARATE field-aware entry point whose default body forwards field 0 to the
    // old one keeps all seven of those files untouched AND byte-identical in
    // behaviour, while a multi-field command overrides this one instead.
    //
    // `world` has been through Units_FromDisplay for EVERY field kind — see the
    // "KIND IS A DISPLAY FACT" note in kiwi_numeric.h.  A command whose field is
    // not a length undoes that with Units_ToDisplay exactly as it always has.
    virtual void NumericFieldChanged( int field, bool has, float world )
    {
        if ( field == 0 )
            NumericChanged( has, world );
    }

    // The fields this command wants Tab to cycle.  Return 0 (the default) for the
    // pre-shakeout-E grammar: ONE unnamed editable LENGTH field.  The storage must
    // be STATIC — the numeric layer copies the structs but not the label strings.
    // Called by KiwiCmd_Start, after KiwiNum_Reset and BEFORE Begin, so a command
    // may still relabel a field per stage from inside Begin/Click
    // (KiwiNum_SetFieldLabel — the primitives walk "base" → "height" that way).
    virtual int NumericFields( const kiwiNumField_t **out ) const
    { (void)out; return 0; }

    // The LIVE value of one field, in that field's own natural unit (raw world
    // units for KNUM_LENGTH, degrees for KNUM_ANGLE, a bare number for the other
    // two).  This is what the §13b bubble shows when the user has typed nothing.
    // False = this field has nothing to display yet and gets no row.
    virtual bool NumericFieldValue( int field, float *out ) const
    { (void)field; (void)out; return false; }

    // Where the §13b value bubble is pinned, in world space — the ACTION
    // GEOMETRY, not the cursor.  False (the default) means "use the framework's
    // fallback", which is the last snap point KiwiCmd_LastSnap reports: for a
    // drawing tool that IS the moving end of the segment, which is exactly where
    // Plasticity puts it.  A transform overrides it with its own latched pivot.
    virtual bool BubbleAnchor( float *out3 ) const { (void)out3; return false; }

    // ── Shakeout G additions ────────────────────────────────────────────────
    // CLAIM AN LMB PRESS OUTRIGHT, before the framework's pause / resume arms see
    // it.  Return true to consume it.
    //
    // WHY THIS AND NOT WantsClicks.  WantsClicks changes a command's WHOLE
    // grammar: it opts out of pausing entirely (KiwiCmd_Pause returns early for
    // one) and every click becomes a Click() event.  The movable pivot needs
    // exactly ONE press to mean something else, for the handful of frames a
    // placement is live, and needs the HOT/PAUSED confirm flow back the instant it
    // ends.  A one-press veto is the smallest thing that does that; the default
    // body is false, so every pre-shakeout-G command is byte-identical.
    //
    // The framework has already latched the press pixel when this is called, so
    // KiwiCmd_LastCursor is current inside it.
    virtual bool PressIntercept( int imgX, int imgY ) { (void)imgX; (void)imgY; return false; }

    // ── KIWI-UX (ROUND T): RUN THE SNAP QUERY SOMEWHERE ELSE ────────────────
    // USER REPORT, verbatim: "when I move the pivot point, and then try to snap
    // to another object, my mouse should just be able to snap onto it while I'm
    // dragging.  Currently the pivot point does not ride where I want it to ride.
    // I want the pivot to change where the gizmo RIDES on the object and lets me
    // snap directly using the gizmo basically."
    //
    // THE GAP.  Round L got the MAPPING right — the pivot is the reference point,
    // the draw anchor rides the applied delta, and a geometry snap resolves to
    // `snapPos - m_ref`, so landing the pivot on a corner is what "snap corner to
    // corner" means.  But the SNAP QUERY still ran at the CURSOR: it asks "what
    // geometry is near THIS PIXEL" (kiwi_snap.h — every arm is screen-space).
    // With the pivot placed on a far corner of the box and the user dragging an
    // AXIS ARROW, the cursor is nowhere near the pivot, so the query lit up
    // whatever happened to be near the ARROW and the target the pivot was
    // actually passing through was never a candidate.
    //
    // THE HOOK.  A command may name an IMAGE-SPACE position (camera RTT pixels,
    // top-left origin — the kiwi_pick.h convention) at which its snap query
    // should run instead.  The CURSOR is still what drives the movement mapping;
    // only the "what is near here" question moves.  Return false (the default)
    // for the pre-round-T behaviour, which is every other command.
    virtual bool SnapQueryAnchor( int *outX, int *outY ) const
    { (void)outX; (void)outY; return false; }

    // ── Shakeout I additions: THE COMMAND'S OWN KEYBOARD PROMPTS ────────────
    // The bottom-left prompt strip (kiwi_hints.cpp) derives the FRAMEWORK's keys
    // on its own — confirm / cancel / Tab / digits / axis locks are the same for
    // every modal command, so making each one restate them would be six copies of
    // one truth.  What it cannot derive is the keys a command invents for itself:
    // the drawing tools' Z vertical constraint, a tool's Esc-clears-the-chain rung.
    // Those were completely unadvertised before this round.
    //
    // Return a count and point `out` at a STATIC array — the strip copies the
    // structs but NOT the strings, and it is rebuilt every frame, so anything with
    // a shorter lifetime than the command would dangle.  The default is 0, which
    // is why every pre-shakeout-I command keeps exactly the derived list it had.
    //
    // `key` is the literal keycap text ("Z", "Ctrl+R", "Esc").  These are keys a
    // command owns INSIDE its own gesture, so they are not in g_radiantCommands and
    // there is nothing to look up — unlike every row of the bottom-right verb strip,
    // which still reads its binding live from the table.
    virtual int HudPrompts( const struct kiwiPrompt_t **out ) const
    { (void)out; return 0; }

    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AI, ITEM 3) — THE IN-COMMAND OPTIONS PANEL
    // ═══════════════════════════════════════════════════════════════════════
    // USER DIRECTIVE, verbatim: "The lofting is pretty cool, but make the options
    // clickable buttons like in plasticity.  Open a temp lofting panel (still
    // allows enter/rightclick completion)."
    //
    // WHY THIS IS GENERIC AND NOT A LOFT PANEL.  The directive is about ONE
    // command, but everything in it is already general — the numeric fields are
    // declared (NumericFields), the keycaps are declared (HudPrompts), and the
    // ONLY thing a running command cannot describe to a UI is a BOOLEAN or an
    // ENUM: loft's Ruled/Tangent lives solely as the `D` key and a substring of
    // HudStatus().  Three virtuals close that hole for every command at once, and
    // a loft-only panel would have needed roughly the same code with none of the
    // reuse.  The cylinder's `P` patch toggle and the drawing tools' Z plane cycle
    // are the obvious next tenants; they are NOT converted in this round.
    //
    // PLASTICITY IS THE MODEL, AND ITS SHAPE IS THE SAME ONE.  A command builds a
    // dialog over its factory's PARAMS object and hands it a re-run callback
    // (plasticity/src/commands/loft/LoftCommand.ts:17-21); the dialog's widgets are
    // named after params keys and a single generic onChange writes
    // `params[name] = value` and fires that callback
    // (plasticity/src/command/AbstractDialog.ts:48-55).  An ENUM is a row of hidden
    // radios sharing one `name` with styled labels
    // (plasticity/src/commands/fillet/FilletDialog.tsx:78-81) — a segmented button
    // group, which is exactly KOPT_ENUM; a scalar is a
    // `<plasticity-number-scrubber>` (LoftDialog.tsx:27-28), which is what KIWI
    // already has as a numeric field.  So the panel does not invent a second state:
    // it CALLS the same handlers the keyboard calls, exactly as Plasticity's dialog
    // writes into the same params the viewport gestures write into.
    //
    // THE ONE RULE FOR IMPLEMENTORS: OptionChanged must do exactly what the
    // command's own key handler does — call it, or share a helper with it.  A panel
    // that duplicated the state transition would drift from the key on the first
    // change to either.
    //
    // Return a count and point `out` at a STATIC array, the same lifetime contract
    // HudPrompts has and for the same reason (the panel is rebuilt every frame and
    // copies nothing).  The default 0 means "no panel", which is every command that
    // existed before this round.
    virtual int CommandOptions( const struct kiwiOption_t **out ) const
    { (void)out; return 0; }

    // The option's CURRENT value: a 0/1 for KOPT_TOGGLE, a choice index for
    // KOPT_ENUM, the integer itself for KOPT_INT.  Scalar rows (KOPT_NUMFIELD) do
    // not come through here — they read NumericFieldValue, so there is exactly one
    // source of truth for a number.
    virtual int OptionValue( int opt ) const { (void)opt; return 0; }

    // The user clicked / stepped an option.  Same argument encoding as
    // OptionValue.  Ignored by default.
    virtual void OptionChanged( int opt, int value ) { (void)opt; (void)value; }

    // ── ROUND K additions: THE LOLLIPOP EXTRUDE HANDLE ──────────────────────
    // Return true, with the handle's CURRENT world anchor and its SIGNED outward
    // direction, to replace the three-arrow translate gizmo with the Plasticity
    // lollipop for the length of this gesture (kiwi_lollipop.h).
    //
    // "CURRENT" is the whole contract: this is called every frame and must answer
    // with where the face/region is NOW — including the push applied so far — so
    // the handle rides the moving surface instead of being buried by it.  The
    // direction must carry the push's SIGN, so a face being pulled inward gets a
    // stem on the inward side rather than one poking out of the far face.
    //
    // A command that answers true is also declaring that kiwi_gizmo.cpp must stand
    // down entirely: GizmoUsable() refuses while a lollipop is wanted, so the move
    // arrows are neither drawn nor hit-tested and cannot swallow the ball's press.
    virtual bool LollipopHandle( float outAnchor[3], float outDir[3] ) const
    { (void)outAnchor; (void)outDir; return false; }

    // ── ROUND L: ONE GRAB GATE FOR EVERY HANDLE ─────────────────────────────
    // A HANDLE was taken hold of / let go — the lollipop's ball, one of the move
    // gizmo's arrows / plane corners / centre square, or a rotate ring.  The
    // framework has ALREADY latched the press pixel and re-latched the mapping
    // (Rebase), so this is only the command's own grab GATE and its own
    // "the drag starts HERE" latch; a command with neither needs nothing here.
    //
    // Round K called this LollipopGrab and only the lollipop ever raised it, which
    // is why the gizmo had to hand-roll the same four-step arm in a second place
    // (kiwi_gizmo.cpp) and drifted from it.  There is now exactly ONE arm —
    // KiwiCmd_HandleGrab below — and every handle in the editor raises this.
    virtual void HandleGrab( bool held ) { (void)held; }

    // ── ROUND K addition: RECLAIMING A PLAIN CLICK WHILE PARKED ─────────────
    // USER REPORT, verbatim: "When clicking a face, left clicking is disabled.  It
    // shouldn't be disabled.  I should be able to click multiple faces while
    // holding shift or move to another face without de-selecting first."
    //
    // WHY IT WAS DISABLED, mechanically: clicking a face in mode 3 auto-enters a
    // PAUSED push/pull (kiwi_boxselect.cpp), and shakeout E's resume rule is
    // deliberately the BROADEST one — "any LMB press inside the camera image
    // resumes".  So the very next click, wherever it landed, was eaten as a resume
    // and never reached the selection layer at all.
    //
    // This hook is offered ABOVE that resume arm, and ONLY while the gesture is
    // PAUSED and has applied nothing.  A command that returns true has consumed
    // the press and may have cancelled ITSELF in the process (the face path does
    // exactly that), so the framework re-reads the active command afterwards and
    // touches nothing it does not still own.
    //
    // The gate "PAUSED and nothing applied" is what keeps this from stealing a
    // resume in the middle of a real edit: once the user has actually moved
    // something, the shakeout-E semantics are back, unchanged, and the click
    // resumes exactly as it always did.
    virtual bool IdlePressReselect( int imgX, int imgY, bool shift )
    { (void)imgX; (void)imgY; (void)shift; return false; }

    // ── ROUND N addition: A CONTEXT VERB MAY PREEMPT AN AUTO-ENTERED GESTURE ──
    // USER REPORT, verbatim: "the Ctrl-R feature I asked for still isn't in the
    // editor (splitting of a face on a solid)."
    //
    // IT WAS SHIPPED (shakeout G) AND IT COULD NEVER FIRE.  The chain, end to end:
    //   * clicking a face in mode 3 AUTO-ENTERS a PAUSED push/pull
    //     (kiwi_boxselect.cpp), so in Face mode a face selection and a live modal
    //     command are the SAME state — you cannot have one without the other;
    //   * KiwiUX_KeyFunnel routes EVERY key to KiwiCmd_KeyDown while a command is
    //     live, and KiwiCmd_KeyDown's last rung SWALLOWS everything it did not
    //     recognise (kiwi_command.cpp, "Everything else is SWALLOWED");
    //   * Ctrl+R is not Tab, not numeric, not Esc/Enter and not one of Move's own
    //     keys, so it died on that last rung — every single time.
    //   * and KiwiSplit_CanSplitFace needs exactly ONE face selected, which is
    //     precisely the state that guarantees the swallow.  The same trap holds Z
    //     (match face), J (join) and E (extrude face): every FACE-CONTEXT verb in
    //     the editor was unreachable from the mouse-driven flow that produces a
    //     face selection.
    //
    // THE RUNG.  A command that returns true here is declaring "I was entered off
    // a click, I have applied nothing, and a verb about my own selection may take
    // me over" — exactly round K's IdlePressReselect gate, minus the pixels.  The
    // funnel then CANCELS this gesture (provably record-free, for the reasons round
    // K wrote out on IdlePressReselect) and lets the chord fall through to the
    // ordinary hotkey table, which starts the verb over the selection that is still
    // there.  Cancel-and-start, the same shape a Shift+click already uses.
    //
    // It is deliberately NOT "swallow less": a command must keep owning arbitrary
    // keys mid-edit (that swallow is what stops a half-finished gesture collecting
    // an unrelated undo record).  Only an UNMOVED, AUTO-ENTERED gesture yields, and
    // only to the small explicit id set in kiwi_command.cpp.
    virtual bool PreemptIdle() const { return false; }

    // ── ROUND Z, ITEM 1: A TOOL SWAP TAKES OVER A LIVE GESTURE ──────────────
    // USER REPORT, verbatim: "when pasting a brush, it goes into move mode
    // automatically, however if I want to paste and rotate(or similar) a brush, it
    // requires a de-selection first.  This is unacceptable, allow tool swaps."
    //
    // PreemptIdle (above) is round N's rung and it is deliberately narrow: an
    // UNMOVED, auto-entered FACE gesture, yielding to a verb ABOUT that face.  A
    // paste lands in an OBJECT move that may already have been dragged, so it
    // answers false and R / S / G died on KiwiCmd_KeyDown's catch-all rung — the
    // same swallow, one state further along.
    //
    // ── PLASTICITY'S RULE, WHICH THIS MIRRORS ────────────────────────────────
    // `CommandExecutor.enqueue( command, interrupt = true )` interrupts whatever is
    // running (CommandExecutor.ts:48-56, `this.active?.interrupt()`), and
    // `CancellableRegistor.interrupt` (CancellableRegistor.ts:48-70) forks on the
    // command's STATE, in its own words: "Normally this should cancel the current
    // command.  However, when the command is in the 'Awaiting' state, the command
    // is ready to be commit […] and we generally interpret that as a commit."  A
    // command in 'None' (still collecting input) is marked 'Interrupted', its
    // `finish()` becomes a no-op (CancellableRegistor.ts:39) and CommandExecutor's
    // `if ( command.state === 'Finished' )` fails, so `originator.discardSideEffects`
    // rolls it back with NO history entry (CommandExecutor.ts:98-112).  One in
    // 'Awaiting' finishes and `history.add` stamps a record (CommandExecutor.ts:105).
    //
    // KIWI HAS NO 'Awaiting'; it has something better, because it can ask the
    // gesture whether it actually DID anything.  So:
    //     GestureMoved() true   → COMMIT.  The record closes on its own edit,
    //                             exactly as an Enter would have, and the new tool
    //                             starts over the result.
    //     GestureMoved() false  → CANCEL.  Provably record-free by the same proof
    //                             PreemptIdle rests on (no bracket was opened, so
    //                             KiwiCmd_UndoCancel closes nothing), and the
    //                             selection survives a Cancel untouched.
    //
    // CanSwapTo is the command's own veto and defaults to FALSE, so every command
    // that does not opt in keeps the pre-round-Z swallow byte for byte.  The
    // ALLOW-LIST of ids that may swap in is kiwi_command.cpp's SwapVerb — the same
    // shape (and the same safety argument) as PreemptVerb.
    //
    // Esc is untouched: it still cancels the whole gesture and starts nothing.
    virtual bool CanSwapTo( int commandId ) const { (void)commandId; return false; }

    // "Has this gesture applied anything a commit would keep?"  Only consulted
    // after CanSwapTo said yes, so the default is the SAFE answer for a command
    // that never opts in: treat it as having done work and commit rather than
    // silently discard.
    virtual bool GestureMoved() const { return true; }

    // ── ROUND Z, ITEM 2: SNAPPING IS OPT-IN FOR THE ONE-AXIS GESTURES ───────
    // USER REPORT, verbatim: "When extruding, it should not snap by default.  Make
    // it snap only when holding CTRL.  It's just not good to use in a cluttered
    // scene."
    //
    // A command that returns true here INVERTS arm 0 of the snap layer for as long
    // as it is running: the plain drag gets `SNAP_NONE` + the raw point (no
    // geometry arms, no grid quantisation — the numeric field is untouched and
    // still the way to be exact), and CTRL HELD runs the full ranked query.
    //
    // THAT IS PLASTICITY'S OWN SHAPE, not an invention.  Ctrl is bound to
    // `snaps:temporarily-disable` / `-enable` on keydown/keyup
    // (default-keymap.ts:353,365-366) and sets `snaps.xor`; the manager then reads
    // `get enabled() { return this._enabled !== this.xor }`
    // (SnapManager.ts:28-49) — a genuine XOR, so the modifier INVERTS whatever the
    // context's default is rather than always meaning "off".  KIWI's default was
    // "on everywhere"; for these three gestures it is "off", and Ctrl means the
    // same thing it always did: the other one.
    //
    // DELIBERATELY ASYMMETRIC, and only here — see RADIANT_UX_DESIGN D-Z2.
    virtual bool SnapOptIn() const { return false; }

    // A PAUSED → HOT edge (shakeout E).  The framework has already latched the
    // cursor at the press pixel; re-latch whatever "where the drag started"
    // state this command maps against, so resuming does not TELEPORT the
    // geometry by however far the cursor wandered while the command was parked.
    // A command with no delta-from-start mapping needs nothing here.
    virtual void Rebase()                        {}
};

// ── §4 the single active-command owner ───────────────────────────────────────
KiwiEditorCommand *KiwiCmd_Active();
bool KiwiCmd_Start( int commandId );          // false = unknown id / CanExecute said no
void KiwiCmd_Commit();
void KiwiCmd_Cancel();

// ── SHAKEOUT G: hand off to another command when THIS gesture finishes ───────
// Park a KiwiCmd_Start (optionally followed by KiwiCmd_Pause) to run at the very
// END of KiwiCmd_Commit — after Commit(), after KiwiCmd_UndoCommit and after
// KiwiNum_Reset.  Calling KiwiCmd_Start from inside Commit() directly cannot
// work: the bracket close would land on the NEW command and the numeric reset
// would throw away the field table the new command just installed.
//
// A CANCELLED gesture drops the request (there is no result to hand over), and a
// request is dropped rather than queued if something else is already active.
// Used by E (a new body enters Move, so it can be placed) — the same handoff the
// classic Paste / Clone tail performs from outside a gesture.
void KiwiCmd_StartDeferred( int commandId, bool paused );

// ── §4 the HOT / PAUSED gesture state (shakeout E — see the state table above) ─
// True while the geometry follows the cursor.  A command always starts HOT.
bool KiwiCmd_IsHot();

// Park the gesture: the preview stays, the value stays, MouseMove stops reaching
// the command.  No-op when nothing is active, when it is already paused, or when
// the active command WantsClicks (those keep their click grammar).
void KiwiCmd_Pause();

// Un-park it and Rebase() the command at the latched cursor.  No-op when nothing
// is active or it is already hot.
void KiwiCmd_Resume();

// ── ROUND L: THE ONE HANDLE-GRAB ENTRY ───────────────────────────────────────
// USER REPORT, verbatim: "When clicking on the gizmo after selecting a shape, as
// soon as the gizmo is clicked it pre-calculates the mouse delta and applies it.
// It should ONLY move with gizmo drag, no pre existing mouse offset."
//
// Round K fixed exactly one path — the lollipop's ball — by writing a careful
// four-step arm in kiwi_lollipop.cpp, and left kiwi_gizmo.cpp's own copy of that
// arm in place.  Two copies of a sequence whose whole value is its ORDER is one
// copy too many, and the gizmo's copy was missing a rung.  This is that arm, once:
//
//   1. FEED THE PRESS PIXEL.  KiwiCmd_LastCursor becomes the grab point.  The
//      command's gate is still shut, so this move changes nothing.
//   2. RESUME (which Rebase()s) when the gesture was PAUSED, or Rebase() it
//      directly when it was already HOT.  Either way the command's mapping origin
//      is re-latched AT the grab pixel — this is the rung the gizmo's copy skipped
//      whenever the command was already hot, i.e. on the FIRST grab after G.
//   3. OPEN THE COMMAND'S OWN GATE (HandleGrab true), which may itself move the
//      mapping origin (Move's grab gate re-latches the constraint).
//   4. REBASE AGAIN, so a gate that moved the origin cannot leave a stale start.
//      Idempotent by construction: a second fold of a zero delta is a no-op.
//   5. FEED THE MOVE ONCE MORE, now with the gate open — the first fed frame, and
//      its delta is exactly zero.
//
// The commands additionally hold their total FROZEN until the cursor leaves the
// grab pixel (kiwi_transform.cpp's grab-freshness latch), which is what stops the
// SNAP arm — an ABSOLUTE mapping, not a delta — from teleporting the selection
// onto whatever happened to be under the handle at the instant of the press.
//
// False = nothing is active.  The caller aims its constraint AFTER this returns.
bool KiwiCmd_HandleGrab( int imgX, int imgY );

// The release edge: shut the command's gate.  Does NOT pause or commit — the
// caller owns that decision (kiwi_gizmo.cpp pauses, kiwi_gizmo.cpp's abort arm
// cancels).  No-op when nothing is active.
void KiwiCmd_HandleRelease();

// The RMB-click confirm.  Routed through the SAME ladder Enter takes
// (KiwiCmd_KeyDown with VK_RETURN), so a command that vetoes Enter — the polyline
// "finish the chain" rung — vetoes an RMB click identically, in one place.
void KiwiCmd_Confirm();

// Key funnel entry.  True = CONSUMED (the caller must not run the hotkey table).
// Feeds the numeric entry first, then the command, then Esc/Enter.
bool KiwiCmd_KeyDown( int vk, unsigned int mods );

// Mouse, from kiwi_viewport.cpp.  Each returns true when the command consumed it.
// MMB / RMB / wheel are NEVER offered here — camera navigation stays live (§4).
bool KiwiCmd_MouseMove  ( int imgX, int imgY );
// ROUND K: `shift` is plumbed through for IdlePressReselect, which needs it to
// tell "select that face instead" from "add that face too".  Defaulted so the one
// existing caller (kiwi_viewport.cpp) is the only place that has to know.
bool KiwiCmd_MouseButton( int btn, int imgX, int imgY, bool shift = false );

// The last snap the active command was fed (for the marker + label draw).
const snap_result_t &KiwiCmd_LastSnap();

// The camera-image cursor position (TOP-LEFT origin, kiwi_pick.h conventions) the
// active command was last fed — seeded from ImGuiShell_CameraPaintCursor when a
// command starts and updated by every KiwiCmd_MouseMove.  False when the cursor
// has never been over the camera image during this gesture.
//
// This exists because MouseMove hands a command a PICK and a SNAP, not a ray:
// commands whose mapping is screen-relative (R's degrees-per-pixel, S's
// factor-per-pixel) and commands that need to re-cast the ray under their own
// constraint (G's axis/plane mapping) both need the raw pixel.  Reading it back
// here keeps ONE cursor source instead of letting each command poll the shell.
bool KiwiCmd_LastCursor( int *imgX, int *imgY );

// ── KIWI-UX (ROUND AA, ITEM 9): …AND THE MODIFIER THAT PRESS CARRIED ─────────
// True when the LAST LMB PRESS fed to KiwiCmd_MouseButton had Shift held.  Valid
// for the whole of that press's handling — PressIntercept, IdlePressReselect and
// Click() all run inside it — and reset by KiwiCmd_Start so a modifier can never
// leak from one gesture into the next.
//
// WHY A LATCH AND NOT A Click() PARAMETER.  Click() is `virtual bool Click()` and
// is overridden by kiwi_matchface.cpp, kiwi_construct.cpp, kiwi_primitive.cpp,
// kiwi_trim.cpp and kiwi_split.cpp.  Widening the signature would silently stop
// every one of those overriding anything (a default argument does not make an
// override match — the same argument kiwi_command.h already makes at
// NumericFieldChanged), so the five files would compile and quietly do nothing.
// The press pixel is already published exactly this way, one function up, and for
// exactly this reason; the modifier is the same fact about the same event.
bool KiwiCmd_LastShift();

// ── KIWI-UX (ROUND AA, ITEM 9): the marquee rung, for kiwi_viewport.cpp ──────
// The viewport asks whether the ACTIVE command wants a Shift+LMB drag as a
// marquee, and hands it the resolved rect at the release edge.  Both are false /
// no-ops with nothing running, so the idle editor is untouched.
bool KiwiCmd_WantsMarquee();
void KiwiCmd_Marquee( int x0, int y0, int x1, int y1, bool crossing, bool shift );

// Cam_Draw tail hook — the active command's world overlay plus the snap marker.
void KiwiCmd_DrawWorld();

// ── KIWI-UX (ROUND AF, ITEM 3): the gesture batch's default and its CEILING ───
// The default is the pre-round-AF constant and is what every command that does not
// override LineBudget() still gets.  The ceiling is the most any one command may
// ask for, and it is deliberately of the same order as the construction pass's own
// KCON_DRAW_SEGMENTS (1600, kiwi_construct.h) — that pass has drawn 1600 segments a
// frame since shakeout H without ever being the thing that overflows the render
// command buffer, which is the empirical justification for this number.  Same-colour
// segments merge into one RC_DRAW_LINES per 256-segment staging flush
// (r_rendercmds.cpp Ed_EmitLineBatch + kiwi_lines.cpp KLINES_VERTS), so 1536
// segments in a handful of colour runs is ~6 draw commands, not 1536.
#define KCMD_LINE_BUDGET      288
#define KCMD_LINE_BUDGET_MAX  1536

// ── ROUND Z, ITEM 2: does the ACTIVE command want snapping on Ctrl only? ─────
// The one reader is kiwi_snap.cpp's arm 0, which is a free function with no
// command in hand.  False when nothing is running, so the idle editor and every
// non-opted-in command see arm 0 exactly as they always did.
bool KiwiCmd_SnapOptIn();

// ── §5 the shell key funnel (called from Radiant_PreTranslateMessage) ────────
// True = swallow the key.  Sits AFTER the ImGuiShell_WantsKeyboard gate (so a
// focused ImGui text field still outranks everything) and BEFORE
// TranslateAccelerator / Radiant_TryHotkey.
bool KiwiUX_KeyFunnel( unsigned int vk );

// ── §4 undo bracket helpers (see the protocol note at the top) ───────────────
// `operation` MUST be a string literal — the undo record stores the pointer.
void KiwiCmd_UndoBegin ( const char *operation );
void KiwiCmd_UndoCommit();
void KiwiCmd_UndoCancel();
bool KiwiCmd_UndoOpen();

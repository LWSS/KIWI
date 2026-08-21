#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_patchverts.h — RADIANT_UX_DESIGN §62.6: PATCH VERTEX MODE, revived.
//
// USER DIRECTIVE (round AI, item 6), verbatim: "Vertex Mode (V) (Legacy Feature).
// I want you to enable use of the old vertex mode that allows you to move points.
// This is used for q3 curve manipulation.  Right now pressing V opens it but it
// doesn't allow dragging.  Fix it so it works and give it the modern gizmos that
// make it easier to handle."
//
// ═════════════════════════════════════════════════════════════════════════════
//  THE V BINDING — THE FINAL RULE
// ═════════════════════════════════════════════════════════════════════════════
//   1. V WHILE A MODAL COMMAND IS RUNNING  →  THE MOVABLE PIVOT (§29).
//      Unchanged, and untouched by this round.  It is COMMAND-LOCAL: the key
//      funnel offers every key to the live command before the hotkey table is
//      consulted (KiwiUX_KeyFunnel, kiwi_command.cpp), so it never occupied the
//      global 0x56 slot in the first place — kiwi_keymap.cpp:705-715 says exactly
//      that, at length, and is still true.
//   2. V WITH NOTHING RUNNING AND A PATCH IN THE SELECTION  →  PATCH VERTEX MODE
//      (this file).  A toggle: press it again to leave.
//   3. V WITH NOTHING RUNNING AND NO PATCH  →  the classic 33005 Drag Vertices,
//      byte for byte what it did before.
// No chord moves, nothing is displaced, and both keymap profiles behave the same.
// Rule 2 is a REFINEMENT of what V already did — 33005's own body already routed
// a pure-patch selection to Patch_EditPatch (mainfrm.cpp:2223-2228); it just had
// no working drag on the other side of it.
//
// ═════════════════════════════════════════════════════════════════════════════
//  WHY THE LEGACY DRAG WAS DEAD, AND WHY THIS DOES NOT REVIVE IT LITERALLY
// ═════════════════════════════════════════════════════════════════════════════
// Three independent kills, any one of which is fatal on its own:
//
//   KILL 1 — THE CAMERA NEVER REACHES `Drag_Begin`.  The only camera route into it
//     is CamWnd_OnLButtonDown (camwnd.cpp:3569), and `KiwiVP_CameraButtonDown`
//     returns true for a bare LMB unconditionally (kiwi_viewport.cpp:639-677), so
//     `VP_Down` is skipped (imgui_shell.cpp:388-393).  Move and up are skipped the
//     same way.  kiwi_viewport.cpp:394-404 already records that the legacy 3D
//     marquee is "UNREACHABLE from any modern flow" for exactly this reason.
//   KILL 2 — THE MODE IS RESET ON EVERY MODERN CLICK.  `Sel_SyncToLegacy` opens
//     with `Select_Deselect(1)` (kiwi_selection.cpp:314), which zeroes
//     `d_num_move_points` (select.cpp:1446) and calls `ResetSelectMode()`
//     (select.cpp:1455 → :244-252), which writes `d_select_mode = sel_brush`.  So
//     even a correctly entered `sel_curvepoint` survived until the next click.
//   KILL 3 — THE HANDLE LISTS ARE EMPTY.  Shakeout D removed the SEL_VERTEX →
//     `selected_brushes` promotion, and every legacy handle builder
//     (`SetupVertexSelection`, `Patch_EditPatch`) walks `selected_brushes`.
//
// Un-killing 1 would mean punching a hole through the modern viewport dispatch for
// one mode — and it would still leave the mode itself resettable (2) and the
// handles empty (3).  The cheaper and far more honest route is the one shakeout D
// already took for brush vertices: THE MODERN LAYER OWNS THE FINE KINDS.  And for
// patch control points it already does, completely —
//
//   * `kiwi_pick.cpp:336-359` picks a patch control point by SCREEN DISTANCE
//     (PICK_VERT_PIXELS = 8 px), for any pickable patch, selected or not;
//   * `kiwi_boxselect.cpp`'s SEL_VERTEX arm marquees them;
//   * `kiwi_transform.cpp` MOVES them — `m_verts[i].patchPoint` (:1912-1931),
//     the baseline restore (`AddBaseline`, so a cancelled drag is exact), the
//     apply (:2735-2760) and ONE `Patch_Rebuild` per patch however many of its
//     points moved (:2765-2781);
//   * the move GIZMO and the grid snap ride on that Move command for free.
//
// So the whole of "make dragging work, with modern gizmos, snapping and one undo
// record per drag" is ALREADY BUILT.  What was missing is a way to GET THERE: the
// pick-time mode mask had to become SEL_MASK_VERTEX, and the control points had to
// be VISIBLE so there is something to aim at.  This file is those two things and
// nothing else.
//
// ── WHY IT DRAWS THE CONTROL GRID ITSELF ────────────────────────────────────
// The ported `Patch_DrawControlPoints` (brush.cpp:6230) is gated on
// `d_select_mode == sel_curvepoint` AND reached only from the SELECTED-patch
// wireframe pass — but the instant the user clicks a control point the modern
// selection replaces the object item with a vertex item, the patch leaves
// `selected_brushes` (shakeout D's rule), and the ported draw stops.  The points
// would vanish the moment you touched one.  Drawing from this file's own latched
// patch list has no such dependency, and it is the same decision shakeout D made
// for brush vertices, for the same reason.
//
// ── UNDO ─────────────────────────────────────────────────────────────────────
// One record per drag, and it is the Move command's existing bracket:
// `KiwiCmd_UndoBegin` + `UndoCoverBrush` per touched brush BEFORE the first
// mutation, `KiwiCmd_UndoCommit` at the end (kiwi_command.cpp:2297-2336, the
// pattern kiwi_patchfillet.cpp:1053-1066 documents).  A patch's def is cloned
// through `Brush_FullClone_sub475E80`, whose FIRST branch is the patch branch
// (brush.cpp:7409-7421 → `Patch_Duplicate`, the whole 20 556-byte struct including
// the 16x16 grid), so the control grid is inside the record.  Entering and leaving
// the MODE is not journalled — it changes no geometry.
// ─────────────────────────────────────────────────────────────────────────────

struct selbrush_t;

// V's handler asks this first.  True = it took the key (entered or left patch
// vertex mode); false = there is no patch involved, so the caller must fall
// through to the classic 33005 brush-vertex toggle.
bool KiwiPatchVerts_ToggleForSelection();

// Is patch vertex mode live?
bool KiwiPatchVerts_Active();

// Leave it (restoring the previous pick mask).  Safe to call when inactive.
// Called by anything that invalidates the mode — a map load, a delete.
void KiwiPatchVerts_Exit();

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AJ, ITEM 1 — THE LIFECYCLE.  "The curve vertex mode needs to hide the
//  points when the curve is no longer selected."
//
// ROUND AI LATCHED THE PATCH LIST AT ENTRY AND NEVER LET GO, and it had a real
// reason to: entering the mode CLEARS the selection so the first click picks a
// point, and clicking a point replaces the object item with a vertex item — so
// "is the patch still selected" is false almost immediately and cannot be the
// test.  An immortal latch is the other extreme: the lattice outlived the patch
// leaving the selection by any route, which is the report.
//
// SO THE MODE GETS EXPLICIT EXIT TRIGGERS, checked once per frame from
// KiwiVP_CameraTick (NOT from the draw — see D-AI21: the draw runs inside
// Cam_Draw, which is walking the brush sentinel lists, and a full exit relinks
// brushes between them):
//
//   * THE SELECTION NAMES SOMETHING THAT IS NOT A LATCHED PATCH.  An EMPTY
//     selection does NOT trigger it — clicking empty space drops the POINT
//     selection and stays in the mode, which is what "click empty = deselect the
//     points" has to mean while the patch is still the thing being edited.
//   * THE MODE MASK CHANGED.  Pressing 1..5 or running a command that sets the
//     mask is a statement about granularity, and the mode owns SEL_MASK_VERTEX;
//     it leaves WITHOUT restoring the mask, because the user has just chosen one.
//   * ESCAPE (kiwi_command.cpp's idle key funnel).  Leaves the mode and hands the
//     patches back as whole objects — one level out, not a full deselect.
//   * NO LATCHED PATCH IS LIVE ANY MORE (a delete, a map load).
//
// The walk is gated on Sel_Generation() so an idle frame costs one integer
// compare.
void KiwiPatchVerts_Update();

// Escape, offered from the idle key funnel.  True = the mode took the key.
bool KiwiPatchVerts_HandleEscape();

// True when `b` is one of the patches this mode is scoped to.  Used by the
// marquee preview (kiwi_boxselect.cpp) to decide that a control-point rect is
// worth drawing markers for.
bool KiwiPatchVerts_OwnsPatch( const selbrush_t *b );

// Cam_Draw tail hook (// KIWI-UX in camwnd.cpp): the control lattice and its point
// markers for every patch the mode is scoped to.  Emits nothing when inactive.
void KiwiPatchVerts_DrawWorld();

// Bottom-of-screen status line for the mode, or NULL.  Same contract as a
// command's HudStatus (static storage, read every frame).
const char *KiwiPatchVerts_Status();

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_gizmo.h — RADIANT_UX_DESIGN §14: the MOVE gizmo (v1) and, since shakeout
// D, the ROTATE RINGS.
//
// ── WHY NOT ImGuizmo (a documented departure from §14) ──────────────────────
// §14 proposed wrapping ImGuizmo.  It is not used here, for three reasons that
// are structural rather than aesthetic:
//   1. ImGuizmo draws into an ImDrawList in SCREEN space over a matrix it is
//      handed.  Everything else in this layer draws in the Cam_Draw WORLD tail
//      through kiwi_lines (which is the ported R_Add3DLine path, budgeted, and
//      correctly ordered against the selected-outline depth clear).  A screen
//      overlay would sit over the ImGui image and could not share that ordering.
//   2. It wants a view/projection matrix pair.  This editor's camera is
//      origin+angles, and the projection is assembled inside CamWnd_SetupScene
//      with the binary's own 0.99951171875 constants; handing ImGuizmo a
//      reconstructed matrix is a second source of truth for the projection.
//   3. The geometry is ~200 lines here versus a new third-party dependency and
//      a matrix bridge.  (Shakeout D added rotate; the reason still holds.)
//
// ── ONE COMMAND, TWO ENTRY POINTS ───────────────────────────────────────────
// The gizmo never moves geometry itself.  Grabbing a handle only ever presets a
// CONSTRAINT (move) or an AXIS + an angle (rotate) on the command that is
// ALREADY running, so the drag runs the SAME KiwiMoveCommand / KiwiRotateCommand
// G and R run, with the same snapping, the same numeric entry, the same validity
// gate and the same single undo record (spec §13: "both the gizmo and G/R/S feed
// the SAME command object").
//
// ── VISIBILITY: MODAL CHROME, NOT AN IDLE AFFORDANCE (shakeout D) ───────────
// USER DIRECTIVE: "The Move gizmo should show up when pressing G, nothing should
// show up by default" · "the Rotate key should spawn a rotate gizmo like in
// modern plasticity."
//
// So the visibility rule INVERTED in shakeout D.  It used to be "a selection
// exists and NO command is running"; it is now:
//
//     move arrows   drawn  <=>  KiwiXform_IsMoveActive()
//     rotate rings  drawn  <=>  KiwiXform_IsRotateActive()
//     nothing at all otherwise — including with a fat selection and an idle
//     viewport, which is exactly what "nothing should show up by default" asks
//     for.
//
// (plus the two gates that were always there: modern input on, and this file's
// own persisted toggle, which is now an ADDITIONAL gate rather than the main
// one.)  The anchor is the LIVE command's own anchor — KiwiXform_ActivePivot, and
// only that: a point re-derived from the selection answers a different question
// entirely.  KIWI-UX (CLEANUP, A-17).
//
// ROUND L: that anchor now RIDES the applied translation (the latched reference
// point plus this gesture's delta), because "the gizmo doesn't move with the
// object" was a reported bug and not, as shakeout D assumed, a safeguard.  The
// safeguard it was standing in for — the drag mapping must not move under the
// drag — is provided by the mapping keeping the LATCHED point instead.  See
// kiwi_transform.h KiwiXform_ActivePivot.
//
// ── THE INTERACTION (SHAKEOUT E: click-to-PAUSE, not click-to-commit) ───────
// G / R are click-to-PAUSE (press G, move, click — the preview parks).  A HANDLE
// grab inside that gesture is DRAG-to-PAUSE (press on the handle, drag, release).
// RMB-click or Enter is what CONFIRMS either of them; another LMB press resumes.
// The full state table lives in kiwi_command.h (HOT vs PAUSED), and the user
// directive it implements is "Releasing an action shouldn't commit it, it should
// just pause the wip move.  A right click OR an enter press confirms it."
//
// kiwi_viewport.cpp owns the routing: gesture KG_GIZMO calls KiwiGizmo_Release on
// the release edge instead of letting the LMB-up reach the framework's click arm,
// and its LMB-down runs THIS file's hit test BEFORE the modal arm (which would
// otherwise pause the command on any click, including one aimed at a handle).
// Esc mid-drag still cancels through the framework, and the release then finds no
// active command and does nothing.
//
// ── THE ROTATE RING MAPPING ─────────────────────────────────────────────────
// A held ring measures the cursor's ANGULAR SWEEP about the pivot in that ring's
// own plane (ray ∩ plane(pivot, axis), angle from the grab point, accumulated
// across ±180 wraps so a multi-turn drag keeps counting).  That replaces R's
// horizontal-pixels rule for as long as the ring is held; a free R drag with no
// ring grabbed keeps 0.5°/px exactly as before.  The swept angle is fed to the
// command as DEGREES (KiwiXform_FeedRotateDegrees), so the 5° snap, the numeric
// override, the HUD and the undo bracket are all inherited untouched.
//
// All pixel coordinates are camera-RTT-image relative, TOP-LEFT origin
// (kiwi_pick.h's convention).
// ─────────────────────────────────────────────────────────────────────────────

// Hover feedback.  Called from the viewport's move arms; recomputes which handle
// (if any) is under the cursor for the next DrawWorld.  Cheap: one pivot query
// plus at most 17 Pick_WorldToImage projections in the move HitTest — KIWI-UX
// (CLEANUP, C-45) recounted: 1 anchor + 3 planes * 4 corners + 1 normal tip +
// 3 axis tips — or 3 * (1 + 48) in RingHitTest (rotate, only while R is live).
void KiwiGizmo_Hover( int imgX, int imgY, bool over );

// LMB-down hit test.  True = a handle was grabbed and the LIVE command has been
// aimed at it; the caller owns the gesture (KG_GIZMO).  False = nothing was hit,
// and the caller must fall through to the modal click-pause / box select.
//
// SHAKEOUT E: a successful grab is also a PAUSED → HOT edge — it resumes the
// command (and Rebase()s it) before aiming the handle.
//
// SHAKEOUT D: this NO LONGER STARTS a command.  It is a no-op unless the move or
// rotate command is already active, which is what makes the "no command → no
// gizmo test at all" path in kiwi_viewport.cpp free.
bool KiwiGizmo_MouseDown( int imgX, int imgY );

// Drag feed for the KG_GIZMO gesture, one call per mouse move.  Only the ROTATE
// rings need it (they compute the swept angle here and feed it to the command);
// the move handles are driven by the command's own MouseMove, so this is a no-op
// for them.  Safe to call unconditionally.
void KiwiGizmo_Drag( int imgX, int imgY );

// The KG_GIZMO gesture's two end edges, owned here so the grab latch has exactly
// one owner:
//   Release — the normal LMB-up.  PAUSES the live command (shakeout E; it used to
//             commit).  RMB-click / Enter is what confirms.
//   Abort   — the shell's stuck-drag teardown.  Cancels it (exact restore).
// Both are no-ops when no handle is grabbed, and both tolerate the command having
// already ended (Esc mid-drag cancels through the framework).
void KiwiGizmo_Release();
void KiwiGizmo_Abort();

// True while a handle is being dragged — the viewport's gesture predicate.
bool KiwiGizmo_Grabbed();

// Cam_Draw tail hook (// KIWI-UX in camwnd.cpp).  Emits nothing when no gizmo is
// visible.  Self-budgeted (KGIZMO_MAX_SEGMENTS).
void KiwiGizmo_DrawWorld();

// Own on/off switch (persisted, default ON).  A pure addition — nothing legacy
// drew a gizmo — so it is NOT under the modern-input master toggle; it is simply
// also dead while that toggle is off, because the input arm that feeds it is.
// Since shakeout D it gates BOTH gizmos, and it is an ADDITIONAL gate on top of
// "the matching command is running".
bool KiwiGizmo_Show();
void KiwiGizmo_SetShow( bool on );

// Hard per-frame segment budget for whichever gizmo is up (kiwi_lines.h TRAP 1).
// Only ONE of the two can be visible at a time (they key off two mutually
// exclusive active commands), so the budget is the larger of:
//   MOVE    3 axes  * (shaft + 3 chevron)          = 12   (EmitAxis)
//         + 3 planes * 4 perimeter                 = 12   (EmitPlane)
//         + 1 origin ring * KGZ_CENTER_SEGS (24)   = 24   (EmitCentreRing)
//         + 1 face-normal arrow (shaft + 3)        =  4   (EmitNormalArrow,
//                                                          only while pushing
//                                                   faces)      = 52
//   ROTATE  3 rings * KGZ_RING_SEGMENTS (48)       = 144
//         + 1 view ring * KGZ_VIEW_RING_SEG (40)   =  40         = 184
// KIWI-UX (CLEANUP, C-45): the MOVE row above was retallied against the current
// emitters; it used to read 9 / 6 / 4 = 19 and named the origin ring a "centre
// square".  RAISED 48 -> 192 in shakeout D for the rings, with headroom for the
// ring tick marks.  The hover re-emphasis pass opens its OWN 32-segment batch, so
// it does not consume this one.  KiwiCmd_DrawWorld's own 192-segment gesture
// batch is UNCHANGED — the rings live in this file's batch, not in the command's.
#define KGIZMO_MAX_SEGMENTS 192

// ── ROUND AK, ITEM 4: THE FILL BUDGET, SEPARATELY ───────────────────────────
// USER DIRECTIVE, verbatim: "The new gizmo's are better, but I want them slightly
// brighter colors and filled in (no hollow shapes)."
//
// The fills are TRIANGLES on R_AddRenderCmdDrawTris, not segments, so they do not
// consume KGIZMO_MAX_SEGMENTS at all — the line budget above is untouched and
// every outline round AJ shipped still draws.  Their own bound, from
// kiwi_gizmo.cpp THE FILLS:
// KIWI-UX (CLEANUP, C-45): retallied — round AN replaced both cone fills with
// single billboarded triangles, so the old 60 + 20 cone rows are gone.
//   MOVE   3 axis heads      * 1 tri                      =  3
//        + 1 face-normal head (only while pushing faces)  =  1
//        + 3 plane squares   * 2                          =  6
//        + 1 origin disc     * KGZ_CENTER_SEGS (24)       = 24   =  34 tris
//          in 8 R_AddRenderCmdDrawTris commands, one per element
//   ROTATE nothing — its rings are lines and always were.
// One MATERIAL_COLOR bracket opens and closes the whole pass, and the staging
// arrays are sized by the LARGEST SINGLE ELEMENT (the disc, 25 verts) because
// every element flushes before the next one starts.

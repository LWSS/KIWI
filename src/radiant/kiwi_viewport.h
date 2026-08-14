#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_viewport.h — the ONE bridge between the ImGui shell and the Phase-1b
// camera-viewport layer (RADIANT_UX_DESIGN §10 camera, §11 chips, §12 marquee,
// §18 hover).
//
// The shell owns exactly two call sites for this file:
//   * ImGuiShell_ViewportInput  (POST-PRESENT, from the pump) — the KiwiVP_Camera*
//     input entry points below.  Each returns true when the modern layer CONSUMED
//     the event, in which case the shell must NOT also run the legacy CamWnd_On*
//     handler.  Every one of them is a no-op returning false while the
//     modern-input master toggle (kiwi_ux.h) is off, so the legacy dispatch is
//     bit-for-bit what it was before Phase 1b.
//   * ImGuiShell_DrawViewportImage (DURING the frame) — KiwiVP_DrawCameraOverlay,
//     the screen-space chips + marquee rectangle.
//
// All coordinates are camera-RTT-image relative, TOP-LEFT origin (kiwi_pick.h).
//
// LEGACY PATHS THIS REPLACES WHILE THE TOGGLE IS ON (and only then):
//   camera MMB  — the shell never dispatched button 2 for RTT_CAMERA at all
//                 (VP_Down has no MMB arm), so the only legacy effect of an MMB
//                 drag was CamWnd_OnMouseMove -> CamWnd_MouseMoved reaching
//                 Drag_MouseMoved with drag_ok == 0, i.e. nothing.
//   camera wheel— VP_Wheel's default arm: "camera/Z: no wheel".  (CamWnd_Scroll,
//                 the binary's CameraUseWheel dolly, is only reachable from the
//                 retired MFC CMainFrame::OnScroll path.)
//   camera LMB  — CamWnd_OnLButtonDown -> CamWnd_DropModelsToPlane -> Drag_Begin,
//                 the classic 3D pick / drag-select / drag-to-move.
//                 ONE EXCEPTION (round AO, item Y): a BARE ALT+LMB while the
//                 advanced-terrain paint tool is ARMED (sub_401D50, the same gate
//                 the cursor ring is drawn from) is NOT consumed — the press, the
//                 drag and the release all go to that legacy chain, because
//                 Drag_Begin's own LABEL_34 arm IS the paint-stroke start.  See
//                 the long note in KiwiVP_CameraButtonDown's LMB block.
//   camera RMB  — (shakeout A, decision D-1 RESOLVED) CamWnd_OnRButtonDown ->
//                 CamWnd_DropModelsToPlane's free-look fly / camera_mode drive,
//                 and CamWnd_OnRButtonUp -> Cam_MouseUp + CamWnd_ContextMenu.
//                 A modern RMB DRAG is mouselook and legacy sees NOTHING; a modern
//                 RMB CLICK replays that exact down+up pair so the classic context
//                 menu still fires from the pump.  See the RMB note in the .cpp.
// ─────────────────────────────────────────────────────────────────────────────

// btn: 0 = LMB, 1 = RMB, 2 = MMB (ImGui's numbering, as the shell's loop uses).
bool KiwiVP_CameraButtonDown( int btn, int imgX, int imgY, bool shift, bool ctrl );
bool KiwiVP_CameraMouseMove ( int imgX, int imgY );
bool KiwiVP_CameraButtonUp  ( int btn, int imgX, int imgY );
bool KiwiVP_CameraWheel     ( float steps, int imgX, int imgY );

// Idle hover (no button down).  `over` false = the cursor left the image.
void KiwiVP_CameraHover( int imgX, int imgY, bool over );

// ONE per-tick poll, from the shell's post-present dispatch AFTER the per-viewport
// loop: the keyboard fly (shakeout A).  `cursorOver` is the camera image's hover
// for this frame.  A no-op while the modern-input master toggle is off, so the
// legacy dispatch stays bit-for-bit what it was.
void KiwiVP_CameraTick( bool cursorOver );

// Stuck-drag teardown.  True when a modern gesture was live and has been torn
// down here — the shell must then NOT also run CamWnd_AbortDrag (that would be a
// foreign viewport's teardown).
bool KiwiVP_CameraAbort();

// Screen-space overlay drawn INSIDE the camera image: the §11 selection-mode chip
// row (top-left) and the §12 marquee rectangle.  Returns true when a chip is under
// the cursor, so the shell can drop the image's hover for this frame and a chip
// click never also starts a marquee.
bool KiwiVP_DrawCameraOverlay( float imgMinX, float imgMinY, float imgW, float imgH );

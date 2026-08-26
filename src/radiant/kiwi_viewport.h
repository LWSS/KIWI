#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_viewport.h — the bridge between the ImGui shell and the camera-viewport
// input/overlay layer.  Two shell call sites:
//   * ImGuiShell_ViewportInput (post-present pump) -> the KiwiVP_Camera* entry
//     points below.  Each returns true when the modern layer CONSUMED the event,
//     so the shell must NOT also run the legacy CamWnd_On* handler.  All are no-ops
//     returning false while the modern-input toggle (kiwi_ux.h) is off, leaving the
//     legacy dispatch unchanged.
//   * ImGuiShell_DrawViewportImage (during the frame) -> KiwiVP_DrawCameraOverlay.
// Coordinates are camera-RTT-image relative, TOP-LEFT origin (kiwi_pick.h).
//
// Legacy paths replaced while the toggle is on:
//   MMB   -> orbit / Shift-pan (legacy never dispatched camera MMB).
//   wheel -> dolly (legacy had no camera wheel).
//   LMB   -> pick / drag-select / move (CamWnd_OnLButtonDown -> DropModelsToPlane
//            -> Drag_Begin).  EXCEPTION: a bare Alt+LMB while the terrain-paint tool
//            is armed (sub_401D50) is NOT consumed — press/drag/release go to the
//            legacy paint chain (Drag_Begin LABEL_34).  See KiwiVP_CameraButtonDown.
//   RMB   -> mouselook (Alt) or pan; a no-drag RMB CONFIRMS a live modal command.
//            The classic context menu was removed (user directive) — a bare RMB
//            click does nothing; the CamWnd_OnRButton replay helper is dormant.

// btn: 0 = LMB, 1 = RMB, 2 = MMB (ImGui's numbering, as the shell's loop uses).
bool KiwiVP_CameraButtonDown( int btn, int imgX, int imgY, bool shift, bool ctrl );
bool KiwiVP_CameraMouseMove ( int imgX, int imgY );
bool KiwiVP_CameraButtonUp  ( int btn, int imgX, int imgY );
bool KiwiVP_CameraWheel     ( float steps, int imgX, int imgY );

// Idle hover (no button down).  `over` false = the cursor left the image.
void KiwiVP_CameraHover( int imgX, int imgY, bool over );

// One per-tick poll, after the per-viewport loop: the keyboard fly.  `cursorOver`
// is the camera image's hover for this frame.  No-op while the modern-input toggle
// is off.
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

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_ux.h — RADIANT_UX_DESIGN Phase-1b toggles.
//
// ── THE MASTER TOGGLE ────────────────────────────────────────────────────────
// `KiwiUX_ModernInput()` (default ON, persisted) gates EVERY behavior that
// REPLACES a legacy input path — camera MMB, camera wheel, camera LMB drag.  With
// it OFF the editor's input must behave exactly as it did before Phase 1b: the
// shell's camera dispatch falls straight through to the ported CamWnd_On*
// handlers, byte for byte.
//
// Everything else here is a PURE ADDITION and therefore not gated by the master
// toggle — the ground grid, the world axes and the hover highlight draw nothing
// the legacy editor drew, so they get their own independent on/off switches
// (all default ON).
//
// Persistence goes through radiant_registry.h (kiwi_radiant.ini beside the exe).
// Nothing is added to prefData_t.
// ─────────────────────────────────────────────────────────────────────────────

bool KiwiUX_ModernInput();                 // master: orbit / dolly / marquee replace legacy
void KiwiUX_SetModernInput( bool on );

bool KiwiUX_ShowGrid();                    // §17 grey ground grid on Z=0
void KiwiUX_SetShowGrid( bool on );

bool KiwiUX_ShowAxes();                    // §17 world X/Y/Z axis lines
void KiwiUX_SetShowAxes( bool on );

bool KiwiUX_ShowHover();                   // §18 hover outline + active-item accent
void KiwiUX_SetShowHover( bool on );

// The ImGui settings block for all of the above plus the §17 units/spacing prefs.
// Drawn inside the shell window from ImGuiPanels_Menu (// KIWI-UX hook).
//
// Phase 2 folds two more switches into the SAME block rather than starting a
// second settings panel; both own their state elsewhere, so nothing is added to
// this header:
//   * the §6 snap marker/label toggle   — KiwiSnap_ShowMarkers (kiwi_snap.h)
//   * the §11 keymap profile switcher   — KiwiKeymap_DrawSettings (kiwi_keymap.h)
void KiwiUX_DrawSettings();

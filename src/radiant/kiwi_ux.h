#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ModernInput defaults on and gates replacement input paths; when off, those paths must
// fall through to the ported handlers unchanged. Grid, axes, and hover are independent.
// All four settings persist under [KiwiUX] in kiwi_radiant.ini, not prefData_t.

bool KiwiUX_ModernInput();                 // master gate for replacement input paths
void KiwiUX_SetModernInput( bool on );

bool KiwiUX_ShowGrid();                    // ground grid on Z=0
void KiwiUX_SetShowGrid( bool on );

bool KiwiUX_ShowAxes();                    // world X/Y/Z axis lines
void KiwiUX_SetShowAxes( bool on );

bool KiwiUX_ShowHover();                   // hover outline + active-item accent
void KiwiUX_SetShowHover( bool on );

// View > Leak Finder Background: the 3D camera clears to a strobing magenta / green instead
// of the background colour, so a gap in the terrain (or any hole to the void) flashes.
bool KiwiUX_LeakBackground();
void KiwiUX_SetLeakBackground( bool on );
bool KiwiUX_ShowTriCount();                // camera bottom-right "N tris" readout (View menu)
void KiwiUX_SetShowTriCount( bool on );

// KIWI (2026-09-09): the selected-entity facing arrows (kiwi_entarrow) — View menu toggle.
bool KiwiUX_ShowFacingArrows();
void KiwiUX_SetShowFacingArrows( bool on );

// Draws the KIWI UX settings block from ImGuiPanels_Menu.
void KiwiUX_DrawSettings();

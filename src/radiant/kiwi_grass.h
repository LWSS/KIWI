#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Grass Scatter is an armed camera paint tool.  The panel owns settings; the
// viewport bridge owns the full Alt+LMB press/release cycle.

void KiwiGrass_MenuItem();
void KiwiGrass_Draw();

// Windows-menu / palette route (KIWI_CMD_GRASS_PANEL).
void KiwiGrass_TogglePanel();
bool KiwiGrass_PanelVisible();

bool KiwiGrass_IsArmed();
bool KiwiGrass_HandleDown( int imgX, int imgY );
void KiwiGrass_HandleDrag( int imgX, int imgY );
void KiwiGrass_HandleUp();
void KiwiGrass_HandleAbort();
bool KiwiGrass_HandleEscape();

void KiwiGrass_Hover( int imgX, int imgY, bool over );
void KiwiGrass_DrawWorld();

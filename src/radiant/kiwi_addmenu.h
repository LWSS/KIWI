#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Cursor-anchored, categorized view over registered creation-related commands.
// This module owns only their ordering and grouping; metadata stays in
// g_radiantCommands.
//
// Shift+A is reserved for Line. The unbound menu remains reachable from the
// command palette, the KIWI settings panel, and radiant.ini command remapping.
// Rows post WM_COMMAND so Win32 modal commands cannot nest their message pumps
// inside ImGui composition. KiwiUX_KeyFunnel owns keys while the menu is open.

void KiwiAdd_Open();
void KiwiAdd_Close();
void KiwiAdd_Toggle();
bool KiwiAdd_IsOpen();

// Draw at top level so the menu paints above viewport windows.
void KiwiAdd_Draw();

// KIWI settings-panel entry; the menu has no default binding.
void KiwiAdd_MenuItem();

void KiwiAdd_RegisterCommands();
bool KiwiAdd_DispatchInstant( unsigned int cmdId );

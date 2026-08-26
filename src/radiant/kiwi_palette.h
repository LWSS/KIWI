#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Derive every row from g_radiantCommands plus kiwi_command metadata; do not add a
// second registry that can drift from menus and hotkeys.
//
// Auto-focused InputText makes ImGuiShell_WantsKeyboard suppress Radiant hotkeys.
// KiwiUX_KeyFunnel covers opening-to-focus and clicks away from the field; palette
// navigation is read from ImGui, never Win32.

void KiwiPalette_Open();
void KiwiPalette_Close();
void KiwiPalette_Toggle();
bool KiwiPalette_IsOpen();

// Draw at ImGui top level so centering uses the whole dockspace, not a viewport window.
void KiwiPalette_Draw();

// Panel button lets classic-keymap users reach the palette without the modern F binding.
void KiwiPalette_MenuItem();

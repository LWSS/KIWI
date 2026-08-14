#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_palette.h — RADIANT_UX_DESIGN §15: the F command palette.
//
// Backed by `g_radiantCommands` + the §3 metadata (kiwi_command.h).  NO second
// registry: every row is a command id that already exists in the shared table, so
// the palette can never drift out of step with the menus or the hotkeys.  It is
// also the guarantee §11 leans on — a keymap_classic user reaches every new
// feature through here.
//
//   * type to filter, subsequence fuzzy match over "<display> <category> <name>"
//   * Up/Down navigate, Enter runs the highlighted row, click runs a row
//   * Esc closes
//   * the shortcut text is derived from the LIVE g_radiantCommands vk/mods
//     through mainfrm.cpp's own CommandList_Mods / CommandList_KeyName, so a
//     keymap-profile switch or a radiant.ini remap shows up immediately
//   * rows whose metadata canExecute() says no are greyed and non-clickable
//
// ── KEY CAPTURE (the WantTextInput contract) ─────────────────────────────────
// The filter field is an ImGui InputText that is auto-focused on open, so
// `ImGuiShell_WantsKeyboard()` (io.WantTextInput) is true while the palette is
// up — and Radiant_PreTranslateMessage already returns early on that, which is
// what keeps typed text out of the hotkey table.  Nothing about that gate
// changes.  KiwiUX_KeyFunnel additionally swallows keys while
// KiwiPalette_IsOpen(), covering the one frame between opening and focusing (and
// any frame where the user clicked off the field).  All of the palette's OWN keys
// — arrows, Enter, Esc — are read from ImGui inside the draw, never from Win32.
// ─────────────────────────────────────────────────────────────────────────────

void KiwiPalette_Open();
void KiwiPalette_Close();
void KiwiPalette_Toggle();
bool KiwiPalette_IsOpen();

// Drawn during the ImGui frame, at TOP LEVEL (not inside a viewport window) so it
// can centre itself over the whole dockspace — called from ImGuiPanels_Draw.
void KiwiPalette_Draw();

// The View-menu entry, next to KiwiUX_DrawSettings, so classic-keymap users can
// reach the palette without the modern F binding.
void KiwiPalette_MenuItem();

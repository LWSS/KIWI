#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_addmenu.h — RADIANT_UX_DESIGN §16b.4: the ADD MENU.
//
// USER DIRECTIVE (shakeout C): "I want ALL the line tools (shift-a to create a
// line, go look all of them up and dont half ass it)."
//
// ── SHAKEOUT F: IT LOST SHIFT+A ─────────────────────────────────────────────
// The follow-up directive was blunt: "the shift-A menu is unacceptable.  Shift-A
// is for LINES."  Shift+A now starts the Line tool and the other seven Plasticity
// creation chords are bound alongside it (kiwi_keymap.h has the whole chain).
//
// This file is UNCHANGED and the menu is UNBOUND, not deleted.  It is still a
// palette row ("Add Menu (create)", KIWI_CMD_ADD_MENU 34027), still a button in
// the KIWI panel's Construct block, still remappable by name from radiant.ini
// [Commands] — and it is still the only single place that lists all thirteen
// creators including the four that no longer have a chord (Polyline, Arc, Circle
// (2-point), Polygon).  Deleting it would have made those four palette-only.
//
// ── WHAT THIS IS, AND WHAT IT IS NOT ────────────────────────────────────────
// It is a CURSOR-ANCHORED, CATEGORISED VIEW over the creation commands that
// already exist in `g_radiantCommands` — the §3 rule (one registry, never two)
// applies here exactly as it does to the §15 palette.  The only thing this file
// owns is the ORDER and the GROUPING, which is editorial, not state.
//
// It is NOT a Plasticity feature.  Plasticity reaches creation through eight
// direct Shift+letter bindings and a selection-contextual toolbar; its Electron
// menu has no create menu at all (RADIANT_UX_DESIGN §16b.2 cites the files).
// The add menu is the BLENDER shape, chosen because KIWI's letter budget (§11)
// cannot pay for eight creator chords and because the directive asks for one key
// that reaches every line tool.  The CONTENTS are the Plasticity inventory.
//
// ── HOW A ROW RUNS ──────────────────────────────────────────────────────────
// Deferred, through `PostMessage(WM_COMMAND)` — identical to the palette's
// Run(), and for the identical reason (kiwi_palette.cpp: dispatching inside the
// ImGui frame would nest a modal message pump inside the compositing bracket).
//
// ── KEYBOARD ────────────────────────────────────────────────────────────────
// While it is open it OWNS the keyboard: KiwiUX_KeyFunnel (kiwi_command.cpp)
// swallows keys for it the same way it does for the palette, so a stray hotkey
// cannot fire underneath the menu.  Its own keys are read from ImGui inside the
// draw, where the filter field holds focus.
// ─────────────────────────────────────────────────────────────────────────────

void KiwiAdd_Open();
void KiwiAdd_Close();
void KiwiAdd_Toggle();
bool KiwiAdd_IsOpen();

// Drawn from ImGuiPanels_Draw, at TOP LEVEL (not inside a viewport window) so it
// paints above everything — same placement rule as the palette.
void KiwiAdd_Draw();

// The entry in the shell's KIWI settings block, so the CLASSIC keymap (which
// binds no Shift+A) still reaches it.
void KiwiAdd_MenuItem();

// §3 registration + the instant-dispatch arm for KIWI_CMD_ADD_MENU.
void KiwiAdd_RegisterCommands();
bool KiwiAdd_DispatchInstant( unsigned int cmdId );

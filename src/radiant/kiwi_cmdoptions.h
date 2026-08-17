#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_cmdoptions.h — RADIANT_UX_DESIGN §62.3: the IN-COMMAND OPTIONS PANEL.
//
// USER DIRECTIVE (round AI, item 3), verbatim: "The lofting is pretty cool, but
// make the options clickable buttons like in plasticity.  Open a temp lofting
// panel (still allows enter/rightclick completion)."
//
// ── WHAT IT IS ───────────────────────────────────────────────────────────────
// A small floating ImGui window that exists ONLY while the live modal command
// declares options (KiwiEditorCommand::CommandOptions, kiwi_command.h).  It has no
// state of its own: every widget READS the command's own accessors and WRITES
// through the command's own handlers, which are the same ones the keyboard path
// calls.  There is exactly one copy of the state and it lives in the command.
//
// ── IT IS GENERIC, AND THAT WAS THE CHEAPER OPTION ───────────────────────────
// The reasoning is written out on CommandOptions itself.  Short form: the numeric
// fields and the keycaps were already declarative; the only thing a command could
// not describe was a BOOLEAN or an ENUM, and three virtuals fixed that for every
// command at once.  A loft-only panel would have been the same amount of code
// with none of the reuse.
//
// ── PLASTICITY ───────────────────────────────────────────────────────────────
// The model is `<plasticity-dialog>`, and three facts about it were copied:
//
//   1. IT IS SCOPED TO THE VIEWPORT, NOT THE APP WINDOW.  Dialog.tsx:32 renders
//      `<div class="absolute bottom-2 left-2 w-96 ...">` and index.html:31 mounts
//      the host INSIDE `<plasticity-viewport view="3d">`, so "absolute" resolves
//      against the 3D view's rect.  KIWI anchors to the camera IMAGE rect
//      (ImGuiShell_CameraImageRect), which is the same rectangle.
//   2. IT NEVER TAKES KEYBOARD FOCUS.  Dialog.tsx:45-47 puts `tabIndex={-1}` on
//      both footer buttons, precisely so the viewport keeps the keyboard while the
//      dialog is up.  KIWI's equivalent is NoNavFocus | NoFocusOnAppearing plus a
//      widget set with no text field in it, so ImGuiShell_WantsKeyboard stays false
//      and Enter / Esc / RMB keep meaning what they meant.
//   3. THE FOOTER IS Cancel + OK (Dialog.tsx:44-47) and they call the SAME
//      `dialog.cancel()` / `dialog.finish()` the keyboard does.  KIWI's two buttons
//      call KiwiCmd_Cancel / KiwiCmd_Confirm — i.e. Esc and Enter.
//
// KIWI's one departure: Plasticity's dialog sits BOTTOM-LEFT, which in this editor
// is where kiwi_hints.cpp already puts the prompt chips (KHINT_PROMPTS_LEFT).  The
// panel takes the viewport's LEFT edge at upper-third height instead — clear of
// the chips (bottom), of the numeric HUD (bottom-centre) and of the view cube
// (top-right), and never under the cursor, which is normally in the middle of the
// image mid-gesture.
//
// ── WHAT IT DOES NOT DO ──────────────────────────────────────────────────────
// It does not confirm, cancel or change anything by existing.  Every keyboard path
// is exactly as it was: Enter and RMB confirm, Esc cancels (or walks a stage back
// where the command says so), Tab still cycles the numeric fields and typing still
// wins.  Closing the panel is not offered, because it is not a window the user
// owns — it appears with the gesture and leaves with it.
// ─────────────────────────────────────────────────────────────────────────────

// Per-frame draw.  MUST be called at TOP-LEVEL ImGui window scope (imgui_shell.cpp
// calls it beside KiwiOutliner_Draw), never from inside KiwiVP_DrawCameraOverlay.
//
// KIWI-UX (CLEANUP, C-37): the reason is NOT "the overlay is ImDrawList-only" —
// DrawChips emits five real ImGui::Buttons in there.  THE INVARIANT the overlay
// keeps is: no trailing SetCursorScreenPos with no item after it, and no item that
// changes the window's CONTENT EXTENT after the image (kiwi_viewport.cpp's
// DrawChips carries the canonical statement).  THIS panel is its own ImGui window
// with its own SetNextWindowPos/Size, which is exactly what a nested Begin inside
// the viewport window would break — so it stays at top level.
//
// No-op when no command is live or the live one declares no options.
void KiwiCmdOpts_Draw();

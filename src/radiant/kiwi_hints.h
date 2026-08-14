#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_hints.h — the contextual hotkey panel (shakeout A, user request:
// "when selecting a point / edge / face there need to be hotkey indicators on
// the side that help tell you what you can do").
//
// ── SHAKEOUT I: ONE PANEL BECAME TWO STRIPS ─────────────────────────────────
// USER REPORT: "this popup box is not very helpful, also it should be in the
// bottom left and bottom right like modern plasticity."  The right-edge panel is
// GONE.  In its place:
//   BOTTOM-LEFT   the KEY PROMPTS — what the keyboard does right now.  A live
//                 command's grammar (confirm / cancel / axis / Tab / digits) plus
//                 the command's OWN keys via KiwiEditorCommand::HudPrompts; with
//                 nothing running, the selection's universal keys, or — with
//                 nothing selected — the CREATE chords.
//   BOTTOM-RIGHT  the VERB MENU for what is selected (Face: Extrude / Match /
//                 Join / Split …; Object: Move / Rotate / Scale / Cut / Delete;
//                 construction: Join / Trim / Move / Delete).  EMPTY while a
//                 command runs and empty with nothing selected.
// Both are compact chips (keycap + label) on one or two rows hugging the bottom
// edge, ImDrawList only, and both are governed by the single toggle below.
// The Plasticity components these mirror are cited in kiwi_hints.cpp's header.
//
// ── EVERY KEY IS READ LIVE FROM g_radiantCommands ───────────────────────────
// A hint that lies is worse than no hint, so nothing here hard-codes a letter.
// Each row names a COMMAND ID and the binding is looked up in the live table
// (Radiant_GetCommandTable — the mutable copy that LoadCommandMap patches from
// radiant.ini and that kiwi_keymap.cpp's profiles rewrite), formatted through
// mainfrm's own CommandList_KeyName.  Remap G, switch to the classic keymap, or
// edit radiant.ini and the panel says the truth on the very next frame.
//
// A command with NO binding in the current profile is shown as "(<palette key>)",
// the palette's own live binding — because the palette IS how it is reached.
//
// ── DRAWN WITH ImDrawList ONLY ──────────────────────────────────────────────
// Same rule as the chips and the view-cube: no ImGui items inside the camera
// image (the End() cursor-extent assert class fixed in eead8b7).  The panel is
// not interactive, so it does not claim the image's hover either.
// ─────────────────────────────────────────────────────────────────────────────

// Draw, from KiwiVP_DrawCameraOverlay.  Emits nothing when the toggle is off or
// there is nothing to say.
void KiwiHints_Draw( float imgMinX, float imgMinY, float imgW, float imgH );

// Own on/off switch (persisted, default ON) — a pure addition, so it is not under
// the modern-input master toggle.
bool KiwiHints_Show();
void KiwiHints_SetShow( bool on );

// ── ROUND Z, ITEM 5: THE ONE BOTTOM BAND ─────────────────────────────────────
// USER REPORT (with a screenshot): the command's chip row, the §26 texture readout
// and the §13 numeric HUD were all drawn on top of one another along the bottom of
// the camera view — three boxes, one anchor, unreadable.
//
// EACH HAD ITS OWN COPY OF THE SAME LINE.  kiwi_numeric.cpp's
// `y = imgMinY + imgH - boxH - 12`, kiwi_uv.cpp's identical one, and this file's
// `bottomY = imgMinY + imgH - KHINT_EDGE`.  Their comments each claimed a
// different CORNER (centre / left / left), which is true horizontally and worth
// nothing vertically: a wide status line and a wide chip strip overlap whatever
// corner they are nominally hugging.
//
// SO THE VERTICAL ANCHOR IS ALLOCATED, ONCE, IN DRAW ORDER.  KiwiVP_DrawCameraOverlay
// opens the band for the frame and every bottom-anchored overlay TAKES a slot for
// its own height; the band grows upward, so the first taker hugs the edge and each
// later one sits above the last with a fixed gap.  Nothing has to know what the
// others are or how tall they got, and adding a fourth row later costs one call.
//
// The ORDER is the call order in KiwiVP_DrawCameraOverlay: the chip strip is lowest
// (it is the persistent grammar and the thing a user looks down for), the numeric
// HUD above it, the texture readout above that.
void  KiwiHud_BandBegin( float imgMinY, float imgH );

// The TOP y at which to draw a box `boxH` tall, and reserve it.  `fallbackTop` is
// returned unchanged when no band is open this frame (a caller reached outside
// KiwiVP_DrawCameraOverlay keeps exactly its old geometry).
float KiwiHud_BandTake( float boxH, float fallbackTop );

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// Contextual ImDrawList-only hint strips: current key grammar at bottom-left and
// selection verbs at bottom-right, suppressed while a command is active.
// Command-backed chips read the mutable command table live; unbound commands use
// the palette's live key. Literal input-funnel gestures have no table row.
// Draw-list output neither extends ImGui content bounds nor claims image hover.
// ─────────────────────────────────────────────────────────────────────────────

// Draw, from KiwiVP_DrawCameraOverlay.  Emits nothing when the toggle is off or
// there is nothing to say.
void KiwiHints_Draw( float imgMinX, float imgMinY, float imgW, float imgH );

// Persisted, default-on switch independent of the modern-input master toggle.
bool KiwiHints_Show();
void KiwiHints_SetShow( bool on );

// The frame-local bottom band prevents overlay collisions and grows upward in
// caller order: hints lowest, numeric HUD above, texture readout above that.
void  KiwiHud_BandBegin( float imgMinY, float imgH );

// Return and reserve a box's top y; use fallbackTop if no band opened this frame.
float KiwiHud_BandTake( float boxH, float fallbackTop );

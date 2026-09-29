#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Floating options panel for live modal commands. Widgets use the command's own
// accessors and handlers, so panel and keyboard input share one state.
// Anchor it to the camera image's upper-left area, clear of prompts, numeric HUD,
// and view cube. The gesture owns its lifetime, and command keys remain available.

// Draw at top-level ImGui scope. Its Begin/SetNextWindowPos/Size would violate
// KiwiVP_DrawCameraOverlay's post-image content-extent invariant if called there.
// No-op when no live command declares options.
void KiwiCmdOpts_Draw();

// KIWI (2026-09-28, user: "tab should navigate between the fields and allow typing a number,
// also double clicking on the slider should allow typing as well"): Tab (Shift+Tab back) moves
// to the next numeric row of the live command's panel and opens it for typing on the next
// frame.  False when the live command shows no panel with a numeric row (the caller then
// keeps its own Tab meaning).
bool KiwiCmdOpts_TabFocus( bool backwards );

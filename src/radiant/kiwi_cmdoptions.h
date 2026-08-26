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

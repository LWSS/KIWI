#pragma once

// One persisted state shared by the Light tab, Sun tab, and View menu.
bool KiwiSunPreview_Enabled();
void KiwiSunPreview_SetEnabled( bool enabled );

// Stable one-line UI/status explanation for the current preview state.
const char *KiwiSunPreview_Status();

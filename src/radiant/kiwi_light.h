#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

struct selbrush_t;
struct Material;

void KiwiLight_Draw();
void KiwiLight_RegisterCommands();

// False preserves retail preview of every selected/pinned light.
bool KiwiLight_PreviewPrimaryOnly();
bool KiwiLight_GameWillRender( selbrush_t *brush );

// Missing editor-only materials make the faithful additive pass unsafe; camwnd only asks policy.
bool KiwiLight_PerPixelPreviewReady( const Material *multiplyMaterial,
                                     const Material *clearMaterial );

// Preserve missing additive techniques so the queue gate skips them instead of falling back to UNLIT.
bool KiwiLight_KeepMissingTechnique( int technique );

// Camera hook precedes the sun helper; the orthographic hook stays beside Ed_DrawSelectedRadius.
void KiwiLight_DrawWorld();
void KiwiLight_DrawXY( selbrush_t *brush, int viewType );

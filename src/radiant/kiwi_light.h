#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

struct selbrush_t;
struct Material;

// Dock helper and its unbound window command.
void KiwiLight_Draw();
void KiwiLight_RegisterCommands();

// KIWI preview policy layered over the binary path.  Retail still previews every
// selected/pinned light when this is false.
bool KiwiLight_PreviewPrimaryOnly();
bool KiwiLight_GameWillRender( selbrush_t *brush );

// Missing editor-only materials make the faithful additive pass unsafe.  Keep
// the detection and one-shot diagnostic in KIWI code; camwnd only asks policy.
bool KiwiLight_PerPixelPreviewReady( const Material *multiplyMaterial,
                                     const Material *clearMaterial );

// Missing per-light techniques must reach the existing editor queue gate unchanged;
// falling back to UNLIT would turn an absent additive contribution into a base pass.
bool KiwiLight_KeepMissingTechnique( int technique );

// Selected-light extent overlays.  Camera hook is immediately before the sun
// helper; the orthographic hook sits beside Ed_DrawSelectedRadius.
void KiwiLight_DrawWorld();
void KiwiLight_DrawXY( selbrush_t *brush, int viewType );

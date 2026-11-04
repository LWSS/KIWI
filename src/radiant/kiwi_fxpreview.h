#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI (2026-11-02, user: "Ideally it would have live preview as well when FX are selected"):
// plays effects in the camera view.  A small CPU particle system reads the .efx document
// (kiwi_efx.h) and follows the game's rules for spawning (looping interval/count, one-shot
// count, spawn delay, life), spawn origin/offset, run-relative frames, the velocity graphs
// in local and world frames, gravity, rotation, size/colour/alpha graphs, distance fade and
// texture atlases (src/EffectsCore fx_convert.cpp / fx_update.cpp / fx_draw.cpp), then draws
// sprites, tails and clouds with the effect's own materials through R_AddRenderCmdDrawTris.
// Runners play their child effects.  Not simulated: models, lights, sounds, decals, trails,
// collision, emission and impact/death effects (the editor says so per element).
//
// What plays: the selected kiwi_fx entities (default), every FX entity near the camera, or
// none (the FX window's Preview setting), plus the FX editor's test instance.  The FX editor
// pushes its unsaved document as an override, so edits show at once.

#include <string>

struct kefxDoc_t;

void KiwiFxPreview_DrawWorld();                 // camera world hook (kiwi_hover.cpp)
void KiwiFxPreview_ResetForNewMap();

enum { KFXP_OFF = 0, KFXP_SELECTED = 1, KFXP_ALL = 2 };
int  KiwiFxPreview_Mode();
void KiwiFxPreview_SetMode( int mode );         // persisted
bool KiwiFxPreview_Paused();
void KiwiFxPreview_SetPaused( bool paused );
float KiwiFxPreview_TimeScale();
void KiwiFxPreview_SetTimeScale( float scale ); // 0.05 .. 4

// The FX editor's working copy of `fxName` (nullptr doc clears it).  Instances of that
// effect restart with the new definition.
void KiwiFxPreview_SetOverride( const char *fxName, const kefxDoc_t *doc );
// Forget the cached definition (after a save / revert) and restart its instances.
void KiwiFxPreview_Invalidate( const char *fxName );
// Restart every instance (nullptr) or the instances of one effect.
void KiwiFxPreview_Restart( const char *fxName );

// The editor's free-standing test instance (when no placed FX uses the effect).
void KiwiFxPreview_PlayTest( const char *fxName, const float origin[3], const float forward[3] );
void KiwiFxPreview_StopTest();
bool KiwiFxPreview_TestPlaying();

// Diagnostics for the panels and the test DSL.
int  KiwiFxPreview_ActiveInstances();
int  KiwiFxPreview_LiveParticles();
int  KiwiFxPreview_DrawnQuads();                // last frame
// Material names the preview could not load for `fxName` ("" when all resolved).
std::string KiwiFxPreview_MissingMaterials( const char *fxName );
// KIWI (2026-11-02, user: "a browser like the models menu"): the FX browser's pictures.
// Tiles are snapshots of the effect rendered into the shared thumbnail RT (the Models
// browser's RTT_BeginThumb path) with the same simulation, framed to its particles; the
// pane is one effect playing (~30 fps).  Both render in KiwiFxPreview_ThumbTick, before the
// ImGui frame; the UI only reads textures.
struct IDirect3DTexture9;
// The tile picture of `fxName`; null while pending.  `mayRequest` = the tile is visible
// (one render per tick, the first visible miss wins).
IDirect3DTexture9 *KiwiFxPreview_Thumb( const char *fxName, bool mayRequest );
// True when the effect drew nothing (models / sounds / lights only, missing materials).
bool KiwiFxPreview_ThumbEmpty( const char *fxName );
// The pane: asks for `fxName` to play (call every frame it is shown); returns the latest
// frame or null.  An empty name stops it.
IDirect3DTexture9 *KiwiFxPreview_Pane( const char *fxName );
void KiwiFxPreview_PaneRestart();
// Drops cached tiles (all when fxName is null) so they re-render (after an edit or save).
void KiwiFxPreview_ThumbInvalidate( const char *fxName );
void KiwiFxPreview_ThumbTick();                 // imgui_shell.cpp, beside KiwiEntThumb_Tick

// Test DSL: counts, last frame's quads, and each material's techset / UNLIT technique.
std::string KiwiFxPreview_Describe( const char *fxName );
// Test DSL: advance every instance by `msec` without drawing (deterministic checks).
void KiwiFxPreview_Step( int msec );

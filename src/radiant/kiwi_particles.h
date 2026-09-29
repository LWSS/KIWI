#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Particles (KIWI 2026-09-24, user: "I want a new tab for Particles.  Show all the available
// particle effects and allow them to be placed in the world as Particles."  Selecting them:
// the "Particles only" sub-mode of Object mode, in the [4 Object] chip's dropdown beside
// Models only - the user rejected a separate mode-7 chip.)
//
// A placed particle is an ordinary point entity of class `kiwi_fx` (registered after the
// def scan, orange 16-unit box), so undo, G/R/S, copy/paste, delete, groups and the .map
// all handle it like any other entity.  Keys:
//     "fx"      effect path under raw/fx, no extension ("fire/firelp_med_pm") - loadfx()'s name
//     "angles"  pitch yaw roll; the effect's forward axis ("270 0 0" = up, the retail default)
//     "delay"   start time in seconds; negative = already running at map start (retail -15)
//
// THE GAME SIDE is the stock CoD4 MP client-side effects path, no engine change:
// cgame_mp/cg_client_side_effects_mp.cpp reads maps/mp/<map>_fx.gsc for the
// `level._effect["id"] = loadfx("path");` table (32 entries max) and
// maps/createfx/<map>_fx.gsc for `createOneshotEffect` blocks, and spawns every effect
// itself.  linker_pc64 packs both scripts and every loadfx'd effect into the map's fastfile
// (native_manifest.cpp QueueMapDependencies).  So on every map save the editor writes those
// two files from the kiwi_fx entities - but only files it wrote itself (they carry a
// "KIWI-Radiant" line); a hand-written script is never overwritten, the console says what
// to add to it instead.  cod4map drops kiwi_fx entities from the BSP (map.cpp), so they
// cost no game entity slots.

#include "kiwi_pick.h"

#define KIWI_FX_CLASSNAME "kiwi_fx"
#define KFX_PAYLOAD       "KIWI_FX"          // ImGui drag payload: the effect path

struct selbrush_t;
struct entity_s;

void KiwiParticles_Draw();                   // the "Particles" dock window (KIWI_WIN_PARTICLES)
void KiwiParticles_RegisterCommands();
bool KiwiParticles_DispatchInstant( unsigned int cmdId );
bool KiwiParticles_CameraDropTarget( float imgMinX, float imgMinY );
void KiwiParticles_DrawWorld();              // camera: forward arrows + the drag ghost
void KiwiParticles_ResetForNewMap();

// Registers the kiwi_fx entity class; Radiant_LoadEclassDefs calls it after every def scan
// (the scan frees the class list).
void KiwiParticles_RegisterEclass();

// A node of a kiwi_fx entity (not inside a prefab).
bool KiwiParticles_IsParticleNode( const selbrush_t *b );

// Particles-only picking.  Nearest particle under the camera-image pixel: its box along the ray,
// or failing that the one whose origin projects within 10 px.  Geometry never occludes.
bool KiwiParticles_PickAt( int imgX, int imgY, selbrush_t **outNode, float *outDist );

// Writes maps/mp/<map>_fx.gsc + maps/createfx/<map>_fx.gsc under raw\ for `mapPath`.
// Quiet on a map with no particles and no previously generated files.  `fromSave` = the
// Map_SaveFile hook (autosaves and region saves never export).
void KiwiParticles_ExportForMap( const char *mapPath, bool fromSave );

// Test DSL: place `fx` at `origin`, its forward along `normal`.  False = not placed.
bool KiwiParticles_PlaceAt( const char *fx, const float origin[3], const float normal[3] );

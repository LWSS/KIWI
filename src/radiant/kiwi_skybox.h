#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Skyboxes are ordinary sky-material brushes; this UI writes only stock brush geometry.
// SURF_SKY is authoritative, with a leaf-name fallback only when the flags word is zero.
// Real sky color maps are cubemaps, so flat ImGui tiles own MANAGED 2D copies of one face.
// See-through is decided per face against the orbit pivot so a near wall cannot erase the
// grid while far walls remain textured.

struct qtexture_s;
struct brush_t;

// Docked sky-material browser and its palette/instant-command integration.
void KiwiSky_Draw();
void KiwiSky_RegisterCommands();
bool KiwiSky_DispatchInstant( unsigned int cmdId );

// Null-safe canonical material predicate; do not add another sky-name spelling.
bool KiwiSky_IsSkyMaterial( const qtexture_s *q );

// Tests face 0 at the current edit layer; null is not a sky brush.
bool KiwiSky_IsSkyBrush( const brush_t *def );

// `facePoint` is a world-space point on the face.  Null uses the cached sky-union
// containment fallback; normal per-face calls compare view depth with the orbit pivot.
bool KiwiSky_SeeThroughFace( const float *facePoint );

// Geometry mutations can invalidate the cached sky-brush union.
void KiwiSky_InvalidateBounds();

// Releases the MANAGED cube-face copies; called by RTT_ReleaseForReset and idempotent.
void KiwiSky_ReleaseForReset();

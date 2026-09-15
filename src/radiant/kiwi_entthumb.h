#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Real 3D thumbnails use a dedicated RTT, orthographic scene setup, guarded model
// registration, and a shared memory/disk cache. Class requests require default_model_name;
// entity-specific models remain isometric bbox tiles.
// Registration finishes before RTT_BeginThumb so ERR_DROP cannot leave the target bound.
// Opaque BGRA pixels copy through reusable SYSTEMMEM into managed textures and .kthumb files.

struct eclass_t;
struct IDirect3DTexture9;

// Return the cached class thumbnail, or null when pending, unsupported, or failed.
// Only a visible miss (`mayRequest`) may spend a disk-load slot or enqueue the one request.
IDirect3DTexture9 *KiwiEntThumb_Get( const eclass_t *ec, bool mayRequest );

// Direct xmodel-name access shares entries and request budget with class access.
IDirect3DTexture9 *KiwiEntThumb_GetModel( const char *xmodelName, bool mayRequest );

// Bounds come from source hashing and are confirmed by guarded rendering on a miss.
bool KiwiEntThumb_GetModelBounds( const char *xmodelName,
                                  float outMins[3], float outMaxs[3] );
bool KiwiEntThumb_ModelFailed( const char *xmodelName );

// KIWI (2026-09-13, user: "show tri-count and details when mousing over xmodels"):
// the resident model's numbers for the browser tooltip.  Registers through the same
// guarded path the thumbnail uses (a hit on an already-loaded model is a hash lookup;
// callers only ask once the tile's preview is ready, so nothing loads on hover).
struct kiwiModelStats_t
{
    int  lods;
    int  bones;
    int  lod0Surfs;
    int  lod0Tris;
    int  lod0Verts;
    int  totalTris;          // every LOD
    int  collSurfs;
    int  materialCount;      // LOD 0 skins, deduplicated
    char materials[8][64];
};
bool KiwiEntThumb_ModelStats( const char *xmodelName, kiwiModelStats_t *out );

// Render at most one pending thumbnail before ImGui composition. Model loading is
// process-global and synchronous, so fixed and cost-based gaps space the work.
void KiwiEntThumb_Tick();

// Drop managed textures and readback state during RTT reset; disk files survive.
// Idempotent because reset recovery may run the release list twice.
void KiwiEntThumb_ReleaseForReset();

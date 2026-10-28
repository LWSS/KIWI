#pragma once
// Per-brush cast memo plus shared model-local geometry in CPU heap (no device-reset hook).
// Map_NewMap drops it because its keys point into brush and model asset memory.

struct selbrush_t;
struct orientation_t;
struct XModel;

// Advance the shared walk/cast/light-cache epoch; safe even during a walk.
void KiwiShadowCache_Invalidate();

// Layer-0 casting is an exact per-brush-def property, independent of light and frame.
// Keyed on the def pointer + the def's own version + the structural epoch (KIWI
// 2026-09-09: no longer the general walk epoch, which every drag frame bumps).
// Returns -1 = unknown, 0 = no, 1 = yes.
int  KiwiShadowCache_BrushCasts( const void *brushDef, unsigned defVersion );
void KiwiShadowCache_SetBrushCasts( const void *brushDef, unsigned defVersion, bool casts );

// Shared XModel-local geometry; false requires a caller-owned extraction fallback.
bool KiwiShadowCache_ModelGeo( XModel *model, const float **verts,
                               const unsigned short **indices, int *indexCount );

// Free cache storage during Map_NewMap teardown.
void KiwiShadowCache_Shutdown();

// KIWI (2026-10-27): one shadow-volume batch exactly as SunLightPreview_PolyOffsetShadows
// issued it (xyzw = 4 floats per vertex).  shadowvolume.cpp appends every flushed batch to
// the recording set by Radiant_ShadVol_SetRecording; Radiant_ShadVol_Replay re-issues them
// through the same stencil-shadow command.  The light preview's per-light cache (camwnd.cpp)
// is the client.
#include <vector>
struct KiwiShadVolBatch
{
    std::vector<short> indices;
    std::vector<float> xyzw;
};
typedef std::vector<KiwiShadVolBatch> KiwiShadVolRecording;
void Radiant_ShadVol_SetRecording( KiwiShadVolRecording *rec );   // nullptr = stop
void Radiant_ShadVol_Replay( const KiwiShadVolRecording &rec );

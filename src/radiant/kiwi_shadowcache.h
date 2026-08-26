#pragma once
// Per-brush cast memo plus shared model-local geometry in CPU heap (no device-reset hook).
// Map_NewMap drops it because its keys point into brush and model asset memory.

struct selbrush_t;
struct orientation_t;
struct XModel;

// Advance the shared walk/cast/light-cache epoch; safe even during a walk.
void KiwiShadowCache_Invalidate();

// Layer-0 casting is an exact per-brush-def property, independent of light and frame.
// Returns -1 = unknown, 0 = no, 1 = yes.
int  KiwiShadowCache_BrushCasts( const void *brushDef );
void KiwiShadowCache_SetBrushCasts( const void *brushDef, bool casts );

// Shared XModel-local geometry; false requires a caller-owned extraction fallback.
bool KiwiShadowCache_ModelGeo( XModel *model, const float **verts,
                               const unsigned short **indices, int *indexCount );

// Free cache storage during Map_NewMap teardown.
void KiwiShadowCache_Shutdown();

int KiwiShadowCache_ResidentKB();
int KiwiShadowCache_CachedModels();

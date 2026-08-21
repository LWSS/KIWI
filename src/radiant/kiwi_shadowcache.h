#pragma once
//  kiwi_shadowcache.h - the two SUN-PREVIEW-ONLY caches: a per-brush "can this cast at
//  all?" memo and per-XModel extracted geometry.  Plain heap, so no device-reset story;
//  Map_NewMap drops it because the keys point into brush and model memory.

struct selbrush_t;
struct orientation_t;
struct XModel;

// The one invalidation funnel.  Cheap (a counter flip); safe to call from any edit
// path, including from inside a walk.  The counter lives in kiwi_walkcache.cpp and is
// the editor's SINGLE cache epoch, so one call cannot leave the other cache stale.
void KiwiShadowCache_Invalidate();

// "Can this convex brush cast at all?"  Whether a face's layer-0 material can cast is a
// property of the ASSET, not of the light, camera or frame, so the memo is exact rather
// than approximate.  Keyed on brush DEF.  -1 = unknown (do the work), 0 = no, 1 = yes.
int  KiwiShadowCache_BrushCasts( const void *brushDef );
void KiwiShadowCache_SetBrushCasts( const void *brushDef, bool casts );

// The cached result of Editor_ExtractXModelGeo, per XModel.  False = not cached and not
// cacheable now: the caller falls back to its own scratch-buffer extraction.
bool KiwiShadowCache_ModelGeo( XModel *model, const float **verts,
                               const unsigned short **indices, int *indexCount );

// Free everything.  Called from Map_NewMap, before the brushes and models die.
void KiwiShadowCache_Shutdown();

int KiwiShadowCache_ResidentKB();
int KiwiShadowCache_CachedModels();

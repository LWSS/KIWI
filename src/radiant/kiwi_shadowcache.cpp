#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <gfx_d3d/r_xsurface.h>
#include "qe3.h"
#include "kiwi_shadowcache.h"
#include "kiwi_walkcache.h"
#include "kiwi_lightcache.h"

extern int Sys_Printf( const char *fmt, ... );

// Per-XModel caps; callers must not treat a cache miss as empty geometry.
#define KIWI_SV_MAX_MODEL_MB       96
#define KIWI_SV_MAX_MODELS       4096

// KIWI (2026-09-09) — DRAG LAG, cast-memo half.  The memo used to be keyed on the GENERAL
// walk epoch, which every drag frame bumps (each epair write and each model-pose stamp),
// so with the sun-preview shadows on, EVERY brush in the map re-ran its per-face material
// walk on EVERY frame of a drag.  "Does this def cast" depends only on the def's own faces
// and materials, so the memo is now keyed on the def pointer + the def's own version
// (bumped by every geometry/texture edit of that brush) + the STRUCTURAL epoch (a freed
// def whose address is re-used).  A dragged brush still recomputes (its version moves);
// everything else keeps its answer.
static inline unsigned Sv_Epoch() { return KiwiWalkCache_StructEpoch(); }

void KiwiShadowCache_Invalidate()
{
    KiwiWalkCache_Invalidate();
}

// Per-brush cast memo; stale or exhausted probes return unknown.
#define KIWI_SV_CAST_SLOTS 0x20000      // 1.5 MB on 32-bit; power of two for masked probing

struct SvCastSlot
{
    const void *key;
    unsigned    epoch;      // structural epoch at the memo
    unsigned    version;    // the def's own version at the memo
    int         casts;
};
static SvCastSlot s_castTable[KIWI_SV_CAST_SLOTS];

static int Sv_CastSlot( const void *key )
{
    // Pointer hash: drop the always-zero low bits of an allocation, then Knuth.
    unsigned h = (unsigned)( (uintptr_t)key >> 4 ) * 2654435761u;
    return (int)( h & ( KIWI_SV_CAST_SLOTS - 1 ) );
}

int KiwiShadowCache_BrushCasts( const void *brushDef, unsigned defVersion )
{
    if ( !brushDef )
        return -1;
    int slot = Sv_CastSlot( brushDef );
    for ( int probe = 0; probe < 32; ++probe )
    {
        const SvCastSlot *e = &s_castTable[slot];
        if ( !e->key )
            return -1;
        if ( e->key == brushDef )
            return ( e->epoch == Sv_Epoch() && e->version == defVersion ) ? e->casts : -1;
        slot = ( slot + 1 ) & ( KIWI_SV_CAST_SLOTS - 1 );
    }
    return -1;
}

void KiwiShadowCache_SetBrushCasts( const void *brushDef, unsigned defVersion, bool casts )
{
    if ( !brushDef )
        return;
    int slot = Sv_CastSlot( brushDef );
    for ( int probe = 0; probe < 32; ++probe )
    {
        SvCastSlot *e = &s_castTable[slot];
        if ( !e->key || e->key == brushDef || e->epoch != Sv_Epoch() )
        {
            e->key     = brushDef;
            e->epoch   = Sv_Epoch();
            e->version = defVersion;
            e->casts   = casts ? 1 : 0;
            return;
        }
        slot = ( slot + 1 ) & ( KIWI_SV_CAST_SLOTS - 1 );
    }
    // 32 probes deep and every slot live for another brush: give up rather than evict.
}

// Extracted model-local geometry keyed by asset pointer.
struct SvModelGeo
{
    XModel         *model;
    float          *verts;       // 3 floats per vertex, model-local (stride 12)
    unsigned short *indices;
    int             indexCount;
    int             bytes;
};

static SvModelGeo *s_models     = nullptr;
static int         s_modelCount = 0;
static int         s_modelCap   = 0;
static int         s_modelBytes = 0;
static bool        s_modelFull  = false;

// XModel* -> s_models index; avoids a linear scan per instance.
#define KIWI_SV_MODEL_SLOTS 0x2000          // 8,192 slots, >= 2x KIWI_SV_MAX_MODELS
static XModel *s_modelKey[KIWI_SV_MODEL_SLOTS];
static int     s_modelIdx[KIWI_SV_MODEL_SLOTS];
static bool    s_modelIndexInit = false;

static int Sv_ModelSlot( XModel *model )
{
    unsigned h = (unsigned)( (uintptr_t)model >> 4 ) * 2654435761u;
    int slot = (int)( h & ( KIWI_SV_MODEL_SLOTS - 1 ) );
    while ( s_modelKey[slot] && s_modelKey[slot] != model )
        slot = ( slot + 1 ) & ( KIWI_SV_MODEL_SLOTS - 1 );
    return slot;
}

bool KiwiShadowCache_ModelGeo( XModel *model, const float **verts,
                               const unsigned short **indices, int *indexCount )
{
    if ( !model )
        return false;
    if ( !s_modelIndexInit )
    {
        s_modelIndexInit = true;
        memset( s_modelKey, 0, sizeof( s_modelKey ) );
    }
    const int slot = Sv_ModelSlot( model );
    if ( s_modelKey[slot] == model )
    {
        const SvModelGeo *e = &s_models[s_modelIdx[slot]];
        if ( e->indexCount <= 0 )
            return false;                     // empty entry: caller falls back
        *verts      = e->verts;
        *indices    = e->indices;
        *indexCount = e->indexCount;
        return true;
    }
    if ( s_modelFull || s_modelCount >= KIWI_SV_MAX_MODELS )
        return false;

    // Match the direct caller's limits so cached and fallback extraction agree.
    static float          s_vbuf[0x4000 * 3];
    static unsigned short s_ibuf[0x10000];
    const int ic = Editor_ExtractXModelGeo( model, s_vbuf, 0x4000, s_ibuf, 0x10000 );

    if ( s_modelCount >= s_modelCap )
    {
        const int cap = s_modelCap ? s_modelCap * 2 : 256;
        SvModelGeo *p = (SvModelGeo *)realloc( s_models, (size_t)cap * sizeof( SvModelGeo ) );
        if ( !p )
        {
            s_modelFull = true;
            return false;
        }
        s_models   = p;
        s_modelCap = cap;
    }

    SvModelGeo *e = &s_models[s_modelCount];
    e->model      = model;
    e->verts      = nullptr;
    e->indices    = nullptr;
    e->indexCount = 0;
    e->bytes      = 0;

    if ( ic > 0 )
    {
        // Extraction packs LOD-0 surfaces consecutively and biases their indices, so max+1 = vertex count.
        int maxIdx = -1;
        for ( int k = 0; k < ic; ++k )
            if ( (int)s_ibuf[k] > maxIdx )
                maxIdx = (int)s_ibuf[k];
        const int vc = maxIdx + 1;

        const int need = vc * 3 * (int)sizeof( float ) + ic * (int)sizeof( unsigned short );
        if ( vc > 0 && s_modelBytes + need <= KIWI_SV_MAX_MODEL_MB * 1024 * 1024 )
        {
            float          *vcopy = (float *)malloc( (size_t)vc * 3 * sizeof( float ) );
            unsigned short *icopy = (unsigned short *)malloc( (size_t)ic * sizeof( unsigned short ) );
            if ( vcopy && icopy )
            {
                memcpy( vcopy, s_vbuf, (size_t)vc * 3 * sizeof( float ) );
                memcpy( icopy, s_ibuf, (size_t)ic * sizeof( unsigned short ) );
                e->verts      = vcopy;
                e->indices    = icopy;
                e->indexCount = ic;
                e->bytes      = need;
                s_modelBytes += need;
            }
            else
            {
                free( vcopy );
                free( icopy );
                s_modelFull = true;
            }
        }
        else if ( vc > 0 )
        {
            static bool s_reported = false;
            if ( !s_reported )
            {
                s_reported = true;
                Sys_Printf( "KIWI sun-preview model cache: %d MB cap reached - the "
                            "remaining models re-extract per frame.\n", KIWI_SV_MAX_MODEL_MB );
            }
            s_modelFull = true;
        }
    }

    // Publish empty entries too so this cache does not retry failed extractions.
    s_modelKey[slot] = model;
    s_modelIdx[slot] = s_modelCount;
    ++s_modelCount;
    if ( e->indexCount <= 0 )
        return false;
    *verts      = e->verts;
    *indices    = e->indices;
    *indexCount = e->indexCount;
    return true;
}

void KiwiShadowCache_Shutdown()
{
    KiwiLightCache_Shutdown();
    for ( int i = 0; i < s_modelCount; ++i )
    {
        free( s_models[i].verts );
        free( s_models[i].indices );
    }
    free( s_models );
    s_models     = nullptr;
    s_modelCount = 0;
    s_modelCap   = 0;
    s_modelBytes = 0;
    s_modelFull  = false;
    memset( s_modelKey, 0, sizeof( s_modelKey ) );
    s_modelIndexInit = true;
    memset( s_castTable, 0, sizeof( s_castTable ) );
    KiwiWalkCache_Invalidate();   // age every shared-epoch memo
}

int KiwiShadowCache_ResidentKB()
{
    int bytes = s_modelBytes;
    bytes += (int)sizeof( s_castTable );
    bytes += (int)sizeof( s_modelKey ) + (int)sizeof( s_modelIdx );
    return bytes / 1024;
}

int KiwiShadowCache_CachedModels()      { return s_modelCount; }


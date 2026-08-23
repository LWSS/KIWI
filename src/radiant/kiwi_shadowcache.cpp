#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_shadowcache.cpp - mechanism for kiwi_shadowcache.h.  Nothing here computes geometry.

#include "stdafx.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <gfx_d3d/r_xsurface.h>   // XModel, Editor_ExtractXModelGeo (r_xsurface.h:14)
#include "qe3.h"                  // selbrush_t (qe3.h:429), orientation_t
#include "kiwi_shadowcache.h"
#include "kiwi_walkcache.h"       // the ONE epoch lives there
#include "kiwi_lightcache.h"      // cached per-light 52-byte caster records

// win_qe3.cpp:123 — int Sys_Printf( const char *fmt, ... )
extern int Sys_Printf( const char *fmt, ... );

// Caps, per UNIQUE XModel.  Past either, the caller runs the original code.
#define KIWI_SV_MAX_MODEL_MB       96
#define KIWI_SV_MAX_MODELS       4096

// The epoch: ONE counter for the whole editor, owned by the walk cache.
static inline unsigned Sv_Epoch() { return KiwiWalkCache_Epoch(); }

void KiwiShadowCache_Invalidate()
{
    KiwiWalkCache_Invalidate();
}

// The "can this brush cast at all?" memo.  Epoch stored per entry; a full table = "unknown".
#define KIWI_SV_CAST_SLOTS 0x20000      // 131,072 slots x 12 B = 1.5 MB

struct SvCastSlot
{
    const void *key;
    unsigned    epoch;
    int         casts;
};
static SvCastSlot s_castTable[KIWI_SV_CAST_SLOTS];

static int Sv_CastSlot( const void *key )
{
    // Pointer hash: drop the always-zero low bits of an allocation, then Knuth.
    unsigned h = (unsigned)( (uintptr_t)key >> 4 ) * 2654435761u;
    return (int)( h & ( KIWI_SV_CAST_SLOTS - 1 ) );
}

int KiwiShadowCache_BrushCasts( const void *brushDef )
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
            return ( e->epoch == Sv_Epoch() ) ? e->casts : -1;
        slot = ( slot + 1 ) & ( KIWI_SV_CAST_SLOTS - 1 );
    }
    return -1;
}

void KiwiShadowCache_SetBrushCasts( const void *brushDef, bool casts )
{
    if ( !brushDef )
        return;
    int slot = Sv_CastSlot( brushDef );
    for ( int probe = 0; probe < 32; ++probe )
    {
        SvCastSlot *e = &s_castTable[slot];
        if ( !e->key || e->key == brushDef || e->epoch != Sv_Epoch() )
        {
            e->key   = brushDef;
            e->epoch = Sv_Epoch();
            e->casts = casts ? 1 : 0;
            return;
        }
        slot = ( slot + 1 ) & ( KIWI_SV_CAST_SLOTS - 1 );
    }
    // 32 probes deep and every slot live for another brush: give up rather than evict.
}

// Per-XModel extracted geometry, keyed on the XModel pointer (asset memory).
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

// XModel* -> s_models index (a linear scan would be O(models) per instance).  -1 = empty.
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
            return false;                     // known-bad model: extraction yielded nothing
        *verts      = e->verts;
        *indices    = e->indices;
        *indexCount = e->indexCount;
        return true;
    }
    if ( s_modelFull || s_modelCount >= KIWI_SV_MAX_MODELS )
        return false;

    // Extract ONCE, into the same limits the faithful call site uses, then keep it.
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

    // Publish the index BEFORE the early return, so a model that extracted to nothing
    // is remembered as such and never re-extracted.
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
    KiwiWalkCache_Invalidate();   // the shared epoch: every memo entry above is now stale
}

int KiwiShadowCache_ResidentKB()
{
    int bytes = s_modelBytes;
    bytes += (int)sizeof( s_castTable );
    bytes += (int)sizeof( s_modelKey ) + (int)sizeof( s_modelIdx );
    return bytes / 1024;
}

int KiwiShadowCache_CachedModels()      { return s_modelCount; }


#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_instcache.cpp - mechanism for kiwi_instcache.h.  The per-vertex transform is the
// game's own R_SkinXSurfaceStaticVerts, writing GfxPackedVertex (VERTDECL_PACKED).

#include "stdafx.h"
#include <d3d9.h>
#include <string.h>
#include <stdlib.h>
#include <gfx_d3d/r_init.h>        // dx — the D3D9 device (r_init.h:463)
#include <gfx_d3d/r_gfx.h>         // GfxPackedVertex (r_gfx.h:45), GfxScaledPlacement (r_gfx.h:98)
#include <gfx_d3d/r_xsurface.h>    // XSurfaceGetNumVerts / XSurfaceGetNumTris (r_xsurface.h:7-8)
#include <gfx_d3d/r_dobj_skin.h>   // -> r_scene.h -> xanim.h: the full XSurface (xanim.h:1175)
// DObjSkelMat is only a CAST TARGET here, so xanim/dobj.h (and <ode/ode.h>) stays out.
#include <universal/com_math.h>    // UnitQuatToAxis (com_math.h:440), R_TransformSkelMat (com_math.h:551), float4 (com_math.h:66)
#include "kiwi_instcache.h"
#include "kiwi_shadowcache.h"    // KiwiShadowCache_Invalidate

// win_qe3.cpp:122 — int Sys_Printf( const char *fmt, ... )
extern int Sys_Printf( const char *fmt, ... );

// r_staticmodelcache.cpp:593 — void __cdecl SetupTransformUnitVec(const float4 *mtx, int (*fixedMtx)[3])
extern void __cdecl SetupTransformUnitVec( const float4 *mtx, int ( *fixedMtx )[3] );
// r_staticmodelcache.cpp:606 — PackedUnitVec __cdecl LocalTransformUnitVec(PackedUnitVec in, const int (*fixedMtx)[3])
extern PackedUnitVec __cdecl LocalTransformUnitVec( PackedUnitVec in, const int ( *fixedMtx )[3] );

namespace
{

// 65,536 packed verts per page: indices are 16-bit, so a merge needs one 16-bit window.
const unsigned KIWI_IC_PAGE_VERTS = 65536u;
const unsigned KIWI_IC_PAGE_BYTES = KIWI_IC_PAGE_VERTS * 32u;      // 2 MB

// Caps.  Past either, Get() answers false and the caller keeps the per-instance path.
const int      KIWI_IC_MAX_PAGES  = 48;
const int      KIWI_IC_MAX_IDXBLK = 48;                            // x 1 MB
const unsigned KIWI_IC_IDXBLK_BYTES = 1024u * 1024u;

// Open-addressed (instance, surface) -> entry table.  Never grows; full = Get() false.
const int      KIWI_IC_TABLE_SIZE = 32768;
const int      KIWI_IC_TABLE_MASK = KIWI_IC_TABLE_SIZE - 1;

// Instance-surfaces transformed per front-end frame, so a map's first sight spreads out.
const int      KIWI_IC_BUILD_BUDGET = 1024;

struct Entry
{
    const GfxScaledPlacement *inst;    // key 1; null = free slot
    const XSurface           *xsurf;   // key 2
    KiwiInstGeo               geo;
    bool                      built;   // geo holds a real allocation
    bool                      stale;   // allocation valid, contents need re-transform
    bool                      failed;  // refused once — never retried
};

Entry                    s_table[KIWI_IC_TABLE_SIZE];
int                      s_entryCount;
int                      s_okCount;

IDirect3DVertexBuffer9  *s_page[KIWI_IC_MAX_PAGES];
unsigned                 s_pageUsedVerts[KIWI_IC_MAX_PAGES];
int                      s_pageCount;

unsigned char           *s_idxBlk[KIWI_IC_MAX_IDXBLK];
unsigned                 s_idxUsed[KIWI_IC_MAX_IDXBLK];
int                      s_idxBlkCount;

// The device every page above was created on - the recreate guard.
IDirect3DDevice9        *s_device;

unsigned                 s_residentBytes;
int                      s_builtThisFrame;
bool                     s_reportedFull;
// Once a pool has refused, STOP CLAIMING SLOTS, or every later miss walks the table.
bool                     s_poolFull;

unsigned IC_Hash( const GfxScaledPlacement *inst, const XSurface *xsurf )
{
    // Both keys are array elements, so their low bits are far from uniform; fold both.
    unsigned h = (unsigned)(uintptr_t)inst * 0x9E3779B1u;
    h ^= (unsigned)(uintptr_t)xsurf * 0x85EBCA6Bu;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return h;
}

// The entry for (inst, xsurf), or the free slot it would take; null = table full.  No
// tombstones - invalidation marks stale, so probe chains can never break.
Entry *IC_Slot( const GfxScaledPlacement *inst, const XSurface *xsurf )
{
    unsigned i = IC_Hash( inst, xsurf ) & KIWI_IC_TABLE_MASK;
    for ( int probe = 0; probe < KIWI_IC_TABLE_SIZE; ++probe )
    {
        Entry *e = &s_table[i];
        if ( !e->inst || ( e->inst == inst && e->xsurf == xsurf ) )
            return e;
        i = ( i + 1 ) & KIWI_IC_TABLE_MASK;
    }
    return nullptr;
}

void IC_Forget()
{
    memset( s_table, 0, sizeof( s_table ) );
    memset( s_page, 0, sizeof( s_page ) );
    memset( s_pageUsedVerts, 0, sizeof( s_pageUsedVerts ) );
    memset( s_idxBlk, 0, sizeof( s_idxBlk ) );
    memset( s_idxUsed, 0, sizeof( s_idxUsed ) );
    s_entryCount    = 0;
    s_okCount       = 0;
    s_pageCount     = 0;
    s_idxBlkCount   = 0;
    s_residentBytes = 0;
    s_reportedFull  = false;
    s_poolFull      = false;
}

// Reserve verts in a page.  A run may NEVER straddle pages; a bad fit starts a new one.
bool IC_AllocVerts( unsigned vertCount, unsigned *outPage, unsigned *outBaseVert )
{
    if ( vertCount > KIWI_IC_PAGE_VERTS )
        return false;                        // impossible: XSurface::vertCount is uint16
    for ( int i = 0; i < s_pageCount; ++i )
    {
        if ( KIWI_IC_PAGE_VERTS - s_pageUsedVerts[i] >= vertCount )
        {
            *outPage     = (unsigned)i;
            *outBaseVert = s_pageUsedVerts[i];
            s_pageUsedVerts[i] += vertCount;
            return true;
        }
    }
    if ( s_pageCount == KIWI_IC_MAX_PAGES || !dx.device )
        return false;
    IDirect3DVertexBuffer9 *vb = nullptr;
    // MANAGED, WRITEONLY, no FVF (the editor binds VERTDECL_PACKED).
    HRESULT hr = dx.device->CreateVertexBuffer( KIWI_IC_PAGE_BYTES, D3DUSAGE_WRITEONLY, 0,
                                                D3DPOOL_MANAGED, &vb, nullptr );
    if ( hr < 0 || !vb )
        return false;
    s_page[s_pageCount]          = vb;
    s_pageUsedVerts[s_pageCount] = vertCount;
    *outPage     = (unsigned)s_pageCount;
    *outBaseVert = 0;
    ++s_pageCount;
    return true;
}

// Reserve a pre-offset index run in heap - the bytes a merged draw memcpys into the DIB.
unsigned short *IC_AllocIndices( unsigned indexCount )
{
    const unsigned bytes = ( ( indexCount * 2u ) + 3u ) & ~3u;
    if ( bytes > KIWI_IC_IDXBLK_BYTES )
        return nullptr;
    for ( int i = 0; i < s_idxBlkCount; ++i )
    {
        if ( KIWI_IC_IDXBLK_BYTES - s_idxUsed[i] >= bytes )
        {
            unsigned short *p = (unsigned short *)( s_idxBlk[i] + s_idxUsed[i] );
            s_idxUsed[i] += bytes;
            return p;
        }
    }
    if ( s_idxBlkCount == KIWI_IC_MAX_IDXBLK )
        return nullptr;
    unsigned char *blk = (unsigned char *)malloc( KIWI_IC_IDXBLK_BYTES );
    if ( !blk )
        return nullptr;
    s_idxBlk[s_idxBlkCount] = blk;
    s_idxUsed[s_idxBlkCount] = bytes;
    ++s_idxBlkCount;
    return (unsigned short *)blk;
}

// The transform: R_SkinXSurfaceStaticVerts with a GfxPackedVertex destination.
bool IC_Transform( const GfxScaledPlacement *pl, const XSurface *xsurf,
                   IDirect3DVertexBuffer9 *vb, unsigned baseVert, unsigned vertCount )
{
    mat3x3 axis;
    UnitQuatToAxis( pl->base.quat, axis );

    // normAxis = pure rotation (normals), useAxis = rotation * scale.  Normals are NOT scaled.
    float4 normAxis[4];
    float4 useAxis[4];
    for ( int r = 0; r < 3; ++r )
    {
        normAxis[r].v[0] = axis[r][0];
        normAxis[r].v[1] = axis[r][1];
        normAxis[r].v[2] = axis[r][2];
        normAxis[r].v[3] = 0.0f;
        useAxis[r].v[0]  = pl->scale * axis[r][0];
        useAxis[r].v[1]  = pl->scale * axis[r][1];
        useAxis[r].v[2]  = pl->scale * axis[r][2];
        useAxis[r].v[3]  = 0.0f;
    }
    normAxis[3].v[0] = normAxis[3].v[1] = normAxis[3].v[2] = normAxis[3].v[3] = 0.0f;
    // ABSOLUTE world origin, not eye-relative: the merged draw runs under the editor's
    // eye-relative world matrix, whose row 3 already carries -eyeOffset.
    useAxis[3].v[0] = pl->base.origin[0];
    useAxis[3].v[1] = pl->base.origin[1];
    useAxis[3].v[2] = pl->base.origin[2];
    useAxis[3].v[3] = 0.0f;

    int fixedNormAxis[3][3];
    SetupTransformUnitVec( normAxis, fixedNormAxis );

    void *dst = nullptr;
    // MANAGED buffers may not use DISCARD/NOOVERWRITE; a plain lock hits sysmem.
    if ( vb->Lock( baseVert * 32u, vertCount * 32u, &dst, 0 ) < 0 || !dst )
        return false;

    const GfxPackedVertex *src = xsurf->verts0;
    GfxPackedVertex       *out = (GfxPackedVertex *)dst;
    for ( unsigned v = 0; v < vertCount; ++v )
    {
        R_TransformSkelMat( src[v].xyz, (const DObjSkelMat *)useAxis, out[v].xyz );
        out[v].binormalSign    = src[v].binormalSign;
        out[v].color.packed    = src[v].color.packed;
        out[v].texCoord.packed = src[v].texCoord.packed;
        out[v].normal          = LocalTransformUnitVec( src[v].normal,  fixedNormAxis );
        out[v].tangent         = LocalTransformUnitVec( src[v].tangent, fixedNormAxis );
    }
    vb->Unlock();
    return true;
}

} // namespace

bool KiwiInstCache_Get( const GfxScaledPlacement *placement, const XSurface *xsurf,
                        KiwiInstGeo *out )
{
    if ( !placement || !xsurf || !out || !dx.device )
        return false;

    if ( s_device != dx.device )
    {
        IC_Forget();
        s_device = dx.device;
    }

    Entry *e = IC_Slot( placement, xsurf );
    if ( !e )
        return false;                                  // table full

    if ( e->inst == placement && e->xsurf == xsurf )
    {
        if ( e->failed )
            return false;
        if ( !e->stale )
        {
            *out = e->geo;
            return true;
        }
        // Edited: re-transform IN PLACE; the pair's vertex count cannot change, so the allocation stands.
        if ( s_builtThisFrame >= KIWI_IC_BUILD_BUDGET )
            return false;                              // BY3 path for this frame
        ++s_builtThisFrame;
        if ( !IC_Transform( placement, xsurf, e->geo.vb, e->geo.baseVert, e->geo.vertCount ) )
        {
            e->failed = true;
            return false;
        }
        e->stale = false;
        *out = e->geo;
        return true;
    }

    // Refuse BEFORE claiming when a pool has refused or the table is 3/4 full (probe decay).
    if ( s_poolFull || s_entryCount >= ( KIWI_IC_TABLE_SIZE * 3 ) / 4 )
        return false;

    // Claim the slot even on failure, so an uncacheable surface is asked exactly once.
    const int vertCount = XSurfaceGetNumVerts( xsurf );
    const int triCount  = XSurfaceGetNumTris( xsurf );

    e->inst   = placement;
    e->xsurf  = xsurf;
    e->built  = false;
    e->stale  = false;
    e->failed = true;
    ++s_entryCount;

    if ( vertCount <= 0 || triCount <= 0 || !xsurf->verts0 || !xsurf->triIndices )
        return false;
    if ( s_builtThisFrame >= KIWI_IC_BUILD_BUDGET )
    {
        // Not a refusal, just "not this frame": release the slot so the next frame retries.
        e->inst = nullptr;
        --s_entryCount;
        return false;
    }

    unsigned page = 0, baseVert = 0;
    if ( !IC_AllocVerts( (unsigned)vertCount, &page, &baseVert ) )
    {
        s_poolFull = true;
        if ( !s_reportedFull )
        {
            s_reportedFull = true;
            Sys_Printf( "KIWI instance geometry cache: vertex pool full (%d instance surfaces, "
                        "%d KB resident) - the rest keep the per-instance draw path.\n",
                        s_okCount, (int)( s_residentBytes / 1024u ) );
        }
        return false;
    }

    unsigned short *idx = IC_AllocIndices( (unsigned)( 3 * triCount ) );
    if ( !idx )
    {
        s_poolFull = true;
        if ( !s_reportedFull )
        {
            s_reportedFull = true;
            Sys_Printf( "KIWI instance geometry cache: index pool full (%d instance surfaces, "
                        "%d KB resident) - the rest keep the per-instance draw path.\n",
                        s_okCount, (int)( s_residentBytes / 1024u ) );
        }
        return false;                                  // page space stays reserved: harmless
    }

    // Pre-biased by baseVert ONCE so a merged draw is a pure memcpy; every biased index fits in 16 bits.
    {
        const unsigned short *srcIdx = xsurf->triIndices;
        const unsigned n = (unsigned)( 3 * triCount );
        for ( unsigned k = 0; k < n; ++k )
            idx[k] = (unsigned short)( srcIdx[k] + baseVert );
    }

    ++s_builtThisFrame;
    if ( !IC_Transform( placement, xsurf, s_page[page], baseVert, (unsigned)vertCount ) )
        return false;

    e->geo.vb                 = s_page[page];
    e->geo.indices            = idx;
    e->geo.page               = page;
    e->geo.baseVert           = baseVert;
    e->geo.vertCount          = (unsigned)vertCount;
    e->geo.triCount           = (unsigned)triCount;
    e->built  = true;
    e->failed = false;
    s_residentBytes += (unsigned)vertCount * 32u + (unsigned)triCount * 6u;
    ++s_okCount;
    *out = e->geo;
    return true;
}

void KiwiInstCache_InvalidateInstance( const GfxScaledPlacement *placement )
{
    // KIWI: this is also the sun preview's model-pose edge - its recorded caster walk
    // stores these poses as composed orientations, so it invalidates here too.
    KiwiShadowCache_Invalidate();
    if ( !placement )
        return;
    // One linear scan.  This runs on EDIT paths, never on the draw path.
    for ( int i = 0; i < KIWI_IC_TABLE_SIZE; ++i )
    {
        Entry *e = &s_table[i];
        if ( e->inst != placement )
            continue;
        if ( e->built )
            e->stale = true;       // re-transform in place on the next draw
        else
            e->failed = true;      // never held geometry; keep refusing
    }
}

void KiwiInstCache_BeginFrame()
{
    s_builtThisFrame = 0;
}

void KiwiInstCache_Shutdown()
{
    for ( int i = 0; i < s_pageCount; ++i )
        if ( s_page[i] )
            s_page[i]->Release();
    for ( int i = 0; i < s_idxBlkCount; ++i )
        if ( s_idxBlk[i] )
            free( s_idxBlk[i] );
    IC_Forget();
    s_device = nullptr;
}

int KiwiInstCache_ResidentSurfs()  { return s_okCount; }
int KiwiInstCache_ResidentKB()     { return (int)( s_residentBytes / 1024u ); }
int KiwiInstCache_BuiltThisFrame() { return s_builtThisFrame; }

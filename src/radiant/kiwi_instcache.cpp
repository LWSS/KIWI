#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Caches R_SkinXSurfaceStaticVerts-style output in the packed editor vertex layout.

#include "stdafx.h"
#include <d3d9.h>
#include <string.h>
#include <stdlib.h>
#include <gfx_d3d/r_init.h>        // dx.device
#include <gfx_d3d/r_gfx.h>         // packed vertex and placement types
#include <gfx_d3d/r_xsurface.h>    // surface count helpers
#include <gfx_d3d/r_dobj_skin.h>   // XSurface definition
// DObjSkelMat is only a cast target, avoiding the heavier xanim/dobj.h include.
#include <universal/com_math.h>    // transform helpers
#include <universal/profile.h>     // PROF_SCOPED: the re-transform shows up in Tracy
#include "kiwi_instcache.h"
#include "kiwi_shadowcache.h"    // sun-preview invalidation

extern int Sys_Printf( const char *fmt, ... );

extern void __cdecl SetupTransformUnitVec( const float4 *mtx, int ( *fixedMtx )[3] );
extern PackedUnitVec __cdecl LocalTransformUnitVec( PackedUnitVec in, const int ( *fixedMtx )[3] );

namespace
{

// 65,536 packed verts per page: indices are 16-bit, so a merge needs one 16-bit window.
const unsigned KIWI_IC_PAGE_VERTS = 65536u;
const unsigned KIWI_IC_PAGE_BYTES = KIWI_IC_PAGE_VERTS * 32u;      // 2 MB

// Hard caps bound backing allocations at 96 MB of vertices and 48 MB of indices.
const int      KIWI_IC_MAX_PAGES  = 48;
const int      KIWI_IC_MAX_IDXBLK = 48;                            // x 1 MB
const unsigned KIWI_IC_IDXBLK_BYTES = 1024u * 1024u;

// Fixed open-addressed table; saturation falls back to per-instance drawing.
const int      KIWI_IC_TABLE_SIZE = 32768;
const int      KIWI_IC_TABLE_MASK = KIWI_IC_TABLE_SIZE - 1;

// Limit first-seen transforms per frame to spread map warmup.
const int      KIWI_IC_BUILD_BUDGET = 1024;

// KIWI (2026-09-09) — THE MODEL DRAG LAG.  A moved or rotated model re-stamps its
// placement every frame of the gesture; each stamp marked every cached surface of the
// instance stale, and the very next draw re-transformed ALL of its vertices on the CPU
// and Lock()ed the shared 2 MB vertex-buffer page (flags 0: a pipeline stall on a buffer
// the GPU is still reading) — every frame, for every surface, for as long as the
// operator dragged.  Brushes never touch this cache, which is why only models lagged.
// A stale entry is now re-transformed only once its pose has been unchanged for this
// long; until then Get refuses it and the caller takes the ordinary per-instance draw
// (GPU world matrix), which is exactly the path a full pool uses.
// 2026-09-10 (Tracy lagwhiledrag.tracy, 46 ms per drag frame inside
// RB_DrawEditorSkinnedCached with the frame-counted version built in): the settle
// clock was KiwiInstCache_BeginFrame, which runs once per FRONT-END frame — and every
// RTT view (camera, XY, texture) is its own front-end frame, so a drag that redraws the
// XY view advanced the clock 2-3 times per editor frame and the "settled" rebuild ran
// every frame anyway.  The clock is wall time now; view count cannot fool it.
const unsigned long long KIWI_IC_SETTLE_MS = 150;

struct Entry
{
    const GfxScaledPlacement *inst;    // key 1; null = free slot
    const XSurface           *xsurf;   // key 2
    KiwiInstGeo               geo;
    bool                      built;   // geo holds a real allocation
    bool                      stale;   // allocation valid, contents need re-transform
    bool                      failed;  // refused once — never retried
    unsigned long long        staleTick;  // GetTickCount64 when it was last marked stale
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

// Device used to create the pages, for detecting recreation.
IDirect3DDevice9        *s_device;

unsigned                 s_residentBytes;
int                      s_builtThisFrame;
bool                     s_reportedFull;
// Stop claiming table slots after either pool refuses an allocation.
bool                     s_poolFull;

unsigned IC_Hash( const GfxScaledPlacement *inst, const XSurface *xsurf )
{
    // Fold pointer alignment with odd avalanche constants; low address bits are nonuniform.
    unsigned h = (unsigned)(uintptr_t)inst * 0x9E3779B1u;
    h ^= (unsigned)(uintptr_t)xsurf * 0x85EBCA6Bu;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return h;
}

// No tombstones: invalidation marks entries stale so probe chains remain intact.
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

// Vertex runs cannot straddle pages because indices share one 16-bit window.
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

// Reserve a heap-backed index run for direct copying into the dynamic index buffer.
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

// Mirrors R_SkinXSurfaceStaticVerts into GfxPackedVertex.
bool IC_Transform( const GfxScaledPlacement *pl, const XSurface *xsurf,
                   IDirect3DVertexBuffer9 *vb, unsigned baseVert, unsigned vertCount )
{
    PROF_SCOPED( "instcache transform" );
    mat3x3 axis;
    UnitQuatToAxis( pl->base.quat, axis );

    // Rotate normals without scale; positions use rotation * scale.
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
    // Store absolute world origins; the eye-relative draw matrix already applies -eyeOffset.
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
        // A pose still in motion is NOT rebuilt (KIWI_IC_SETTLE_MS): the per-instance
        // draw carries it until it has stayed put, so a drag costs no CPU re-transform
        // and no vertex-buffer lock per frame.
        if ( ::GetTickCount64() - e->staleTick < KIWI_IC_SETTLE_MS )
            return false;                              // per-instance fallback while moving
        // Re-transform stale geometry in place; an XSurface's vertex count is immutable.
        if ( s_builtThisFrame >= KIWI_IC_BUILD_BUDGET )
            return false;                              // per-instance fallback this frame
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

    // Refuse before claiming after pool exhaustion or 75% table load to limit probe decay.
    if ( s_poolFull || s_entryCount >= ( KIWI_IC_TABLE_SIZE * 3 ) / 4 )
        return false;

    // Claim even on failure so permanently uncacheable surfaces are tested once.
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
        // Budget exhaustion is temporary; release the slot for next frame's retry.
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

    // Pre-bias once for memcpy batching; page bounds keep every result within uint16.
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
    // Model-pose edits also invalidate the sun preview's recorded caster geometry.
    KiwiShadowCache_Invalidate();
    if ( !placement )
        return;
    // The linear scan is confined to edit paths.
    for ( int i = 0; i < KIWI_IC_TABLE_SIZE; ++i )
    {
        Entry *e = &s_table[i];
        if ( e->inst != placement )
            continue;
        if ( e->built )
        {
            e->stale     = true;   // re-transform in place once the pose settles
            e->staleTick = ::GetTickCount64();
        }
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

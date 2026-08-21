#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_modelcache.cpp - mechanism for kiwi_modelcache.h.  Single-threaded.

#include "stdafx.h"
#include <d3d9.h>
#include <string.h>
#include <gfx_d3d/r_init.h>        // dx — the D3D9 device (r_init.h:463)
#include <gfx_d3d/r_xsurface.h>    // XSurfaceGetNumVerts / XSurfaceGetNumTris (r_xsurface.h:7-8)
#include <gfx_d3d/r_dobj_skin.h>   // -> r_scene.h -> xanim.h: the full XSurface (xanim.h:1175)
#include "kiwi_modelcache.h"

// win_qe3.cpp:122 — int Sys_Printf( const char *fmt, ... )
extern int Sys_Printf( const char *fmt, ... );

namespace
{

// One chunk holds 131,072 verts; an XSurface can never need more than 65,536 (uint16).
const unsigned KMC_VB_CHUNK_BYTES = 4u * 1024u * 1024u;
const unsigned KMC_IB_CHUNK_BYTES = 1u * 1024u * 1024u;   // >= 65,535 tris * 6 B
const int      KMC_MAX_VB_CHUNKS  = 32;                   // 128 MB cap
const int      KMC_MAX_IB_CHUNKS  = 32;                   //  32 MB cap

// Open-addressed pointer -> entry table.  Never grows; full = Get() false.
const int      KMC_TABLE_SIZE     = 4096;
const int      KMC_TABLE_MASK     = KMC_TABLE_SIZE - 1;

struct Entry
{
    const XSurface *xsurf;      // key; null = free slot
    KiwiModelGeo    geo;
    bool            failed;     // build attempted and refused — never retry
};

Entry                    s_table[KMC_TABLE_SIZE];
int                      s_entryCount;   // slots claimed (successes AND refusals)
int                      s_okCount;      // slots that really hold geometry — the plotted number

IDirect3DVertexBuffer9  *s_vbChunk[KMC_MAX_VB_CHUNKS];
unsigned                 s_vbUsed[KMC_MAX_VB_CHUNKS];
int                      s_vbChunkCount;

IDirect3DIndexBuffer9   *s_ibChunk[KMC_MAX_IB_CHUNKS];
unsigned                 s_ibUsed[KMC_MAX_IB_CHUNKS];
int                      s_ibChunkCount;

// The device every chunk was created on - the guard against a device recreate.
IDirect3DDevice9        *s_device;

unsigned                 s_residentBytes;

// One "the pool is full" line per session — a diagnostic, not a log.
bool                     s_reportedFull;

unsigned KMC_HashPtr( const XSurface *p )
{
    // XSurfaces sit in 56-byte arrays, so the pointer's low bits are far from uniform.
    unsigned h = (unsigned)(uintptr_t)p;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return h;
}

// The slot for `xsurf`: its entry, or the free slot it would take.  Null = table full.
Entry *KMC_Slot( const XSurface *xsurf )
{
    unsigned i = KMC_HashPtr( xsurf ) & KMC_TABLE_MASK;
    for ( int probe = 0; probe < KMC_TABLE_SIZE; ++probe )
    {
        Entry *e = &s_table[i];
        if ( !e->xsurf || e->xsurf == xsurf )
            return e;
        i = ( i + 1 ) & KMC_TABLE_MASK;
    }
    return nullptr;
}

// Drop every record without touching D3D - for the device-changed guard.
void KMC_Forget()
{
    memset( s_table, 0, sizeof( s_table ) );
    memset( s_vbChunk, 0, sizeof( s_vbChunk ) );
    memset( s_vbUsed, 0, sizeof( s_vbUsed ) );
    memset( s_ibChunk, 0, sizeof( s_ibChunk ) );
    memset( s_ibUsed, 0, sizeof( s_ibUsed ) );
    s_entryCount    = 0;
    s_okCount       = 0;
    s_vbChunkCount  = 0;
    s_ibChunkCount  = 0;
    s_residentBytes = 0;
    s_reportedFull  = false;
}

// Bump-allocate `bytes` of vertex space; returns the chunk + byte offset, or false.
bool KMC_AllocVerts( unsigned bytes, IDirect3DVertexBuffer9 **outVb, unsigned *outOffset )
{
    if ( bytes > KMC_VB_CHUNK_BYTES )
        return false;                                  // impossible for a uint16 vertCount
    for ( int i = 0; i < s_vbChunkCount; ++i )
    {
        if ( KMC_VB_CHUNK_BYTES - s_vbUsed[i] >= bytes )
        {
            *outVb     = s_vbChunk[i];
            *outOffset = s_vbUsed[i];
            s_vbUsed[i] += bytes;
            return true;
        }
    }
    if ( s_vbChunkCount == KMC_MAX_VB_CHUNKS )
        return false;
    IDirect3DVertexBuffer9 *vb = nullptr;
    // MANAGED, WRITEONLY: write-once geometry, no FVF (the editor binds VERTDECL_PACKED).
    HRESULT hr = dx.device->CreateVertexBuffer( KMC_VB_CHUNK_BYTES, D3DUSAGE_WRITEONLY, 0,
                                                D3DPOOL_MANAGED, &vb, nullptr );
    if ( hr < 0 || !vb )
        return false;                                  // out of memory: caller keeps uploading
    s_vbChunk[s_vbChunkCount] = vb;
    s_vbUsed[s_vbChunkCount]  = bytes;
    *outVb     = vb;
    *outOffset = 0;
    ++s_vbChunkCount;
    return true;
}

bool KMC_AllocIndices( unsigned bytes, IDirect3DIndexBuffer9 **outIb, unsigned *outOffset )
{
    bytes = ( bytes + 3u ) & ~3u;                      // keep every offset dword-aligned
    if ( bytes > KMC_IB_CHUNK_BYTES )
        return false;
    for ( int i = 0; i < s_ibChunkCount; ++i )
    {
        if ( KMC_IB_CHUNK_BYTES - s_ibUsed[i] >= bytes )
        {
            *outIb     = s_ibChunk[i];
            *outOffset = s_ibUsed[i];
            s_ibUsed[i] += bytes;
            return true;
        }
    }
    if ( s_ibChunkCount == KMC_MAX_IB_CHUNKS )
        return false;
    IDirect3DIndexBuffer9 *ib = nullptr;
    HRESULT hr = dx.device->CreateIndexBuffer( KMC_IB_CHUNK_BYTES, D3DUSAGE_WRITEONLY,
                                               D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, nullptr );
    if ( hr < 0 || !ib )
        return false;
    s_ibChunk[s_ibChunkCount] = ib;
    s_ibUsed[s_ibChunkCount]  = bytes;
    *outIb     = ib;
    *outOffset = 0;
    ++s_ibChunkCount;
    return true;
}

// Copied VERBATIM, so a cached draw is bit-identical to a dynamic one.
bool KMC_Build( const XSurface *xsurf, KiwiModelGeo *geo )
{
    const int vertCount = XSurfaceGetNumVerts( xsurf );
    const int triCount  = XSurfaceGetNumTris( xsurf );
    if ( vertCount <= 0 || triCount <= 0 || !xsurf->verts0 || !xsurf->triIndices )
        return false;

    const unsigned vbBytes = 32u * (unsigned)vertCount;
    const unsigned ibBytes =  6u * (unsigned)triCount;

    IDirect3DVertexBuffer9 *vb = nullptr;
    unsigned vbOffset = 0;
    if ( !KMC_AllocVerts( vbBytes, &vb, &vbOffset ) )
        return false;

    IDirect3DIndexBuffer9 *ib = nullptr;
    unsigned ibOffset = 0;
    if ( !KMC_AllocIndices( ibBytes, &ib, &ibOffset ) )
        return false;                                  // vertex space stays allocated: harmless

    void *dst = nullptr;
    // MANAGED buffers may not use DISCARD/NOOVERWRITE; a plain lock writes the sysmem copy.
    // This runs inside backend execution, so the runtime may stall here ONCE per surface.
    if ( vb->Lock( vbOffset, vbBytes, &dst, 0 ) < 0 || !dst )
        return false;
    memcpy( dst, xsurf->verts0, vbBytes );
    vb->Unlock();

    dst = nullptr;
    if ( ib->Lock( ibOffset, ibBytes, &dst, 0 ) < 0 || !dst )
        return false;
    memcpy( dst, xsurf->triIndices, ibBytes );
    ib->Unlock();

    geo->vb                = vb;
    geo->ib                = ib;
    geo->vertexOffsetBytes = vbOffset;
    geo->startIndex        = ibOffset / 2u;            // StartIndex counts INDICES
    geo->vertCount         = (unsigned)vertCount;
    geo->triCount          = (unsigned)triCount;
    s_residentBytes += vbBytes + ibBytes;
    return true;
}

} // namespace

bool KiwiModelCache_Get( const XSurface *xsurf, KiwiModelGeo *out )
{
    if ( !xsurf || !out || !dx.device )
        return false;

    // Device-identity guard.  One pointer compare on the hot path.
    if ( s_device != dx.device )
    {
        KMC_Forget();
        s_device = dx.device;
    }

    Entry *e = KMC_Slot( xsurf );
    if ( !e )
        return false;                                  // table full
    if ( e->xsurf == xsurf )
    {
        if ( e->failed )
            return false;
        *out = e->geo;
        return true;
    }

    // Cold: claim the slot (even on failure, so an uncacheable surface is asked once) and build.
    e->xsurf  = xsurf;
    e->failed = true;
    ++s_entryCount;
    if ( !KMC_Build( xsurf, &e->geo ) )
    {
        if ( !s_reportedFull )
        {
            s_reportedFull = true;
            Sys_Printf( "KIWI model geometry cache: refused a surface (%d cached, %d KB "
                        "resident) - those models keep the per-frame upload path.\n",
                        s_entryCount - 1, (int)( s_residentBytes / 1024u ) );
        }
        return false;
    }
    e->failed = false;
    ++s_okCount;
    *out = e->geo;
    return true;
}

void KiwiModelCache_Shutdown()
{
    for ( int i = 0; i < s_vbChunkCount; ++i )
        if ( s_vbChunk[i] )
            s_vbChunk[i]->Release();
    for ( int i = 0; i < s_ibChunkCount; ++i )
        if ( s_ibChunk[i] )
            s_ibChunk[i]->Release();
    KMC_Forget();
    s_device = nullptr;
}

int KiwiModelCache_ResidentSurfs() { return s_okCount; }
int KiwiModelCache_ResidentKB()    { return (int)( s_residentBytes / 1024u ); }

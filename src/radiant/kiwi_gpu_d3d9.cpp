#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_gpu.h on Direct3D 9.  MANAGED index buffers: the runtime keeps a system copy, so they
// survive a device Reset and need no release/recreate around one.  The editor issues its
// render commands on the main thread before the next frame is built, so a buffer is never
// read by the GPU after the front end replaces or releases it: rewriting in place is safe.

#include "stdafx.h"
#include <gfx_d3d/r_init.h>        // dx
#include <string.h>
#include "kiwi_gpu_d3d9.h"

struct KiwiGpuIndexBuffer
{
    IDirect3DIndexBuffer9 *ib;
    int                    capacity;   // in indices
};

static bool KiwiGpu_Write( IDirect3DIndexBuffer9 *ib, const uint16_t *indices, int count )
{
    void *dst = nullptr;
    // D3DLOCK_DISCARD is illegal on a MANAGED buffer; a plain lock is the documented rewrite.
    if ( ib->Lock( 0, (unsigned)( 2 * count ), &dst, 0 ) < 0 || !dst )
        return false;
    memcpy( dst, indices, (size_t)count * 2 );
    ib->Unlock();
    return true;
}

KiwiGpuIndexBuffer *KiwiGpu_CreateIndexBuffer( const uint16_t *indices, int count )
{
    if ( !dx.device || count <= 0 || !indices )
        return nullptr;
    IDirect3DIndexBuffer9 *ib = nullptr;
    if ( dx.device->CreateIndexBuffer( (unsigned)( 2 * count ), D3DUSAGE_WRITEONLY, D3DFMT_INDEX16,
                                       D3DPOOL_MANAGED, &ib, 0 ) < 0 || !ib )
        return nullptr;
    if ( !KiwiGpu_Write( ib, indices, count ) )
    {
        ib->Release();
        return nullptr;
    }
    KiwiGpuIndexBuffer *out = new KiwiGpuIndexBuffer;
    out->ib       = ib;
    out->capacity = count;
    return out;
}

KiwiGpuIndexBuffer *KiwiGpu_ReplaceIndexBuffer( KiwiGpuIndexBuffer *buf, const uint16_t *indices,
                                                int count )
{
    if ( buf && count > 0 && count <= buf->capacity && KiwiGpu_Write( buf->ib, indices, count ) )
        return buf;
    KiwiGpu_ReleaseIndexBuffer( buf );
    return KiwiGpu_CreateIndexBuffer( indices, count );
}

void KiwiGpu_ReleaseIndexBuffer( KiwiGpuIndexBuffer *buf )
{
    if ( !buf )
        return;
    if ( buf->ib )
        buf->ib->Release();
    delete buf;
}

IDirect3DIndexBuffer9 *KiwiGpu_D3D9IndexBuffer( const KiwiGpuIndexBuffer *buf )
{
    return buf ? buf->ib : nullptr;
}

struct KiwiGpuVertexBuffer
{
    IDirect3DVertexBuffer9 *vb;
    unsigned                bytes;
};

KiwiGpuVertexBuffer *KiwiGpu_CreateVertexBuffer( const void *verts, int stride, int count )
{
    if ( !dx.device || stride <= 0 || count <= 0 )
        return nullptr;
    const unsigned bytes = (unsigned)stride * (unsigned)count;
    IDirect3DVertexBuffer9 *vb = nullptr;
    if ( dx.device->CreateVertexBuffer( bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, 0 ) < 0
      || !vb )
        return nullptr;
    KiwiGpuVertexBuffer *out = new KiwiGpuVertexBuffer;
    out->vb    = vb;
    out->bytes = bytes;
    if ( verts && !KiwiGpu_WriteVertexBuffer( out, 0, verts, (int)bytes ) )
    {
        KiwiGpu_ReleaseVertexBuffer( out );
        return nullptr;
    }
    return out;
}

KiwiGpuVertexBuffer *KiwiGpu_WriteVertexBuffer( KiwiGpuVertexBuffer *buf, int byteOffset,
                                                const void *data, int bytes )
{
    if ( !buf || !data || byteOffset < 0 || bytes <= 0
      || (unsigned)byteOffset + (unsigned)bytes > buf->bytes )
        return nullptr;
    // MANAGED: a plain lock of the range, written in place (see the file comment).
    void *dst = nullptr;
    if ( buf->vb->Lock( (unsigned)byteOffset, (unsigned)bytes, &dst, 0 ) < 0 || !dst )
        return nullptr;
    memcpy( dst, data, (size_t)bytes );
    buf->vb->Unlock();
    return buf;
}

void KiwiGpu_ReleaseVertexBuffer( KiwiGpuVertexBuffer *buf )
{
    if ( !buf )
        return;
    if ( buf->vb )
        buf->vb->Release();
    delete buf;
}

IDirect3DVertexBuffer9 *KiwiGpu_D3D9VertexBuffer( const KiwiGpuVertexBuffer *buf )
{
    return buf ? buf->vb : nullptr;
}

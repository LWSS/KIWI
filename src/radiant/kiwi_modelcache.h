#pragma once
// Static rigid XModel LOD-0 geometry cached per XSurface in D3DPOOL_MANAGED buffers.
// Managed buffers survive reset; shutdown or device replacement drops the cache.

struct XSurface;
struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;

// One surface's vertex and index runs within the pool.
struct KiwiModelGeo
{
    IDirect3DVertexBuffer9 *vb;                // pool chunk holding the verts
    IDirect3DIndexBuffer9  *ib;                // pool chunk holding the indices
    unsigned                vertexOffsetBytes; // SetStreamSource offset (stride 32)
    unsigned                startIndex;        // DrawIndexedPrimitive StartIndex
    unsigned                vertCount;         // == XSurfaceGetNumVerts( xsurf )
    unsigned                triCount;          // == XSurfaceGetNumTris( xsurf )
};

// Build or retrieve one rigid surface. False requires the caller's per-frame dynamic
// upload fallback (invalid input, pool/table exhaustion, or any earlier failure).
bool KiwiModelCache_Get( const XSurface *xsurf, KiwiModelGeo *out );

// Release every pooled buffer.  The device must still be alive.  Idempotent.
void KiwiModelCache_Shutdown();

// Resident-cache metrics for diagnostics.
int KiwiModelCache_ResidentSurfs();
int KiwiModelCache_ResidentKB();

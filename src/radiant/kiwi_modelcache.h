#pragma once
//  kiwi_modelcache.h - static geometry for rigid XModel LOD-0 surfaces, uploaded once
//  per unique XSurface and drawn by stream offset thereafter.  MANAGED: nothing to
//  free on device reset; only shutdown and a device change drop it.

struct XSurface;
struct IDirect3DVertexBuffer9;
struct IDirect3DIndexBuffer9;

// One cached surface: where its verts and its indices live inside the pool.
struct KiwiModelGeo
{
    IDirect3DVertexBuffer9 *vb;                // pool chunk holding the verts
    IDirect3DIndexBuffer9  *ib;                // pool chunk holding the indices
    unsigned                vertexOffsetBytes; // SetStreamSource offset (stride 32)
    unsigned                startIndex;        // DrawIndexedPrimitive StartIndex
    unsigned                vertCount;         // == XSurfaceGetNumVerts( xsurf )
    unsigned                triCount;          // == XSurfaceGetNumTris( xsurf )
};

// Look up - and, the first time, BUILD - the static geometry for one rigid model
// surface.  False = not cacheable (no device, no verts0, pool full, earlier failure):
// the caller MUST fall back to the per-frame dynamic upload.  Never asserts.
bool KiwiModelCache_Get( const XSurface *xsurf, KiwiModelGeo *out );

// Release every pooled buffer.  The device must still be alive.  Idempotent.
void KiwiModelCache_Shutdown();

// Tracy magnitudes, published once per front-end frame by r_ed_scene.cpp.
int KiwiModelCache_ResidentSurfs();
int KiwiModelCache_ResidentKB();

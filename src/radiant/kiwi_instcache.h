#pragma once
//  kiwi_instcache.h - pre-transformed world-space geometry per model INSTANCE, so
//  many instances of one model surface merge into one draw call.  A page is exactly
//  65,536 verts because indices are 16-bit.  MANAGED: nothing to free on device reset.

struct XSurface;
struct GfxScaledPlacement;
struct IDirect3DVertexBuffer9;

// One cached instance surface: a world-space vertex run inside a page plus its index
// run, already biased by baseVert so a merged draw is a straight memcpy.
struct KiwiInstGeo
{
    IDirect3DVertexBuffer9 *vb;        // the page (bind at offset 0, stride 32)
    const unsigned short   *indices;   // 3 * triCount entries, pre-offset by baseVert
    unsigned                page;      // page id — instances merge only within one page
    unsigned                baseVert;  // first vertex within the page
    unsigned                vertCount; // == XSurfaceGetNumVerts( xsurf ) — the re-transform span
    unsigned                triCount;
};

// Look up - and, the first time or the first after an edit, BUILD - the world-space
// geometry for one instance of one rigid model surface.  False = unavailable (table
// or pool full, budget spent, no device): the caller MUST use the per-instance path.
bool KiwiInstCache_Get( const GfxScaledPlacement *placement, const XSurface *xsurf,
                        KiwiInstGeo *out );

// The dirty signal.  Called from the three writers of edMapGlobals.modelInst[].
// Marks every cached surface of that instance stale; the next Get re-transforms.
void KiwiInstCache_InvalidateInstance( const GfxScaledPlacement *placement );

// Per-front-end-frame hook: resets the build budget.
void KiwiInstCache_BeginFrame();

// Release every page and index block.  The device must still be alive.  Idempotent.
void KiwiInstCache_Shutdown();

// Tracy magnitudes, published once per front-end frame by r_ed_scene.cpp.
int KiwiInstCache_ResidentSurfs();
int KiwiInstCache_ResidentKB();
int KiwiInstCache_BuiltThisFrame();

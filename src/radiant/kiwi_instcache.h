#pragma once
// Pre-transformed world-space geometry lets instances of one surface share a draw.
// Pages hold one 16-bit index window; D3DPOOL_MANAGED survives device reset.

struct XSurface;
struct GfxScaledPlacement;
struct IDirect3DVertexBuffer9;

// World-space vertex run plus indices already biased by baseVert.
struct KiwiInstGeo
{
    IDirect3DVertexBuffer9 *vb;        // the page (bind at offset 0, stride 32)
    const unsigned short   *indices;   // 3 * triCount entries, pre-offset by baseVert
    unsigned                page;      // page id — instances merge only within one page
    unsigned                baseVert;  // first vertex within the page
    unsigned                vertCount; // == XSurfaceGetNumVerts( xsurf ) — the re-transform span
    unsigned                triCount;
};

// Build or retrieve one rigid instance surface. False requires the per-instance fallback
// (table/pool full, budget exhausted, or device unavailable).
bool KiwiInstCache_Get( const GfxScaledPlacement *placement, const XSurface *xsurf,
                        KiwiInstGeo *out );

// Called by all three placement writers; the next Get re-transforms stale geometry.
void KiwiInstCache_InvalidateInstance( const GfxScaledPlacement *placement );

// Per-front-end-frame hook: resets the build budget.
void KiwiInstCache_BeginFrame();

// Release every page and index block.  The device must still be alive.  Idempotent.
void KiwiInstCache_Shutdown();

// Per-frame Tracy counters.
int KiwiInstCache_ResidentSurfs();
int KiwiInstCache_ResidentKB();
int KiwiInstCache_BuiltThisFrame();

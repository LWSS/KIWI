#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_entthumb.h — real 3D model thumbnails for the entity browser: a standalone RTT
// slot, an ortho camera through R_Ed_SetSceneParms, and its own model registration
// (AddModelToModelInstBuff — no entity_s / brush_t needed).  One thumbnail per tick.
//
// Only classes with a CLASS-level model (`defaultmdl=` -> eclass_t::default_model_name)
// get one; misc_model / script_model / misc_prefab name their model per ENTITY and keep
// the isometric bbox tile.
//
// One shared 128x128 D3DPOOL_DEFAULT RT is rendered into, then copied per entry through a
// reused SYSTEMMEM surface into a D3DPOOL_MANAGED texture.  The CPU hop also persists the
// same opaque BGRA bytes under kiwi_cache/thumbs/.  A matching disk entry uploads directly
// to a MANAGED texture and skips the render.
//
// A failed model load is cached as FAILED and never retried.  The load runs inside the
// editor's ERR_DROP + SEH bracket and BEFORE RTT_BeginThumb, never between Begin and End:
// a longjmp out of a bound render target is a D3DERR_INVALIDCALL.

struct eclass_t;
struct IDirect3DTexture9;

// The cached thumbnail for `ec`, or null when there is none.  Null means "draw the
// isometric bbox instead" and covers three cases the caller does not need to tell apart:
// not rendered yet, no class-level model, and load failed.  On a miss it records at most
// ONE pending render request for the next tick.  `mayRequest` is the caller's "this tile
// is on screen" answer: a memory hit is served either way, but only a visible tile may
// spend one of the shared eight disk-load slots or enqueue a render miss.
IDirect3DTexture9 *KiwiEntThumb_Get( const eclass_t *ec, bool mayRequest );

// The same cache and renderer, addressed directly by xmodel name for the Models
// browser.  Entity-class and direct-name requests for the same xmodel share one
// texture and one pending-request budget.
IDirect3DTexture9 *KiwiEntThumb_GetModel( const char *xmodelName, bool mayRequest );

// Direct-name metadata populated while hashing the xmodel header (and confirmed by
// a guarded render on a miss).  Thus disk hits retain true placement bounds without
// registering the model; hard load/render failures remain cached for the session.
bool KiwiEntThumb_GetModelBounds( const char *xmodelName,
                                  float outMins[3], float outMaxs[3] );
bool KiwiEntThumb_ModelFailed( const char *xmodelName );

// One bounded render step: at most one thumbnail, only when one is pending.  Called from
// ImGuiShell_RenderViewportsToRT, i.e. outside the compositing bracket and before
// ImGuiShell_BeginFrame, under the RTT_DeviceHealthy() gate that function already makes.
// R_RegisterModel cannot be threaded (process-global fs/material/image state, and a
// Com_Error longjmp has no legal landing on a worker), so loads are SPACED instead: one
// per KENTT_MIN_GAP_MS and additionally KENTT_COST_FACTOR x the previous one's cost.
void KiwiEntThumb_Tick();

// Drop every cached texture and the readback surface.  Called from RTT_ReleaseForReset
// (radiant_rtt.cpp) so this feature has no device-reset hook of its own to forget.  Disk
// files survive and are lazily uploaded again after reset.
// Idempotent — the INVALIDCALL retry arm runs the release list twice.
void KiwiEntThumb_ReleaseForReset();

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_entthumb.h — KIWI-UX (ROUND AV, ITEM 3): REAL 3D MODEL THUMBNAILS FOR THE
//  ENTITY BROWSER.
// ═════════════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: *"I would really like 3d previews in the entities viewer,
// try to get that to work."*  Round AU scoped this out (kiwi_entbrowser.h:34-57) on four
// objections; every one of them turned out to have a clean answer, and this file is those
// four answers.
//
//  1. "THE RTT LAYER HAS ONLY FOUR FIXED SLOTS."  It has four VIEWPORT slots.  A fifth
//     STANDALONE slot (RTT_BeginThumb / RTT_EndThumb / RTT_ThumbSurface, radiant_rtt.h)
//     reuses the same pool discipline without becoming a `rttViewport_t` — which matters,
//     because `RTT_COUNT` also sizes imgui_shell.cpp's per-viewport INPUT arrays.
//
//  2. "IT WOULD NEED ITS OWN MODEL REGISTRATION."  It does, and that registration turns
//     out not to need an entity at all.  `edMapGlobals.modelInst[]` (r_ed_scene.cpp:237)
//     holds `{ quat, origin, scale, colorOverride, XModel*, inuse }` — no `entity_s`, no
//     `brush_t`, no `selbrush_t`.  `AddModelToModelInstBuff( XModel*, float axis[12],
//     float scale )` (r_ed_scene.cpp:256) is the whole registration, and
//     `Entity_UpdateModelInst` (entity.cpp:696-699) is literally just that call with an
//     entity's fields dug out first.  The entity-coupled helpers — `SetupModelInst`,
//     `Model_SetModel`, `Eclass_01` — are all AVOIDED; none of them is on this path.
//
//  3. "IT WOULD NEED ITS OWN CAMERA."  `R_Ed_SetSceneParms( org, axis, proj )`
//     (r_scene.h:320) is the only camera API the editor has, the 2D views already drive
//     an ORTHOGRAPHIC GfxMatrix through it, and ortho is what a thumbnail wants anyway.
//
//  4. "IT WOULD NEED A PLACE IN ImGuiShell_RenderViewportsToRT's PER-TICK BUDGET."  It
//     gets one: KiwiEntThumb_Tick() runs at the END of that function, so it inherits the
//     `!s_primary` and `RTT_DeviceHealthy()` gates the four viewports already pass
//     through, and it renders AT MOST ONE thumbnail per tick.  With a warm cache it is an
//     early return on a null request slot — literally zero draw cost.
//
// ── SCOPE, STATED PLAINLY ────────────────────────────────────────────────────────────
// A thumbnail is rendered for a class that names a model AT THE CLASS LEVEL, i.e. one
// with `defaultmdl=` -> `eclass_t::default_model_name` (eclass.cpp:915).  That is exactly
// the set round AU badged `M`: the `actor_*` AI spawners synthesized from
// main/aitype/*.gsc and the `weapon_*` classes synthesized by Init_ScanWeapons
// (eclass.cpp:1645/:1676).  It is NOT `misc_model` / `script_model` / `misc_prefab`:
// those carry no class-level model at all — their model is a per-ENTITY `model` key, so
// there is nothing for a per-CLASS tile to preview and the isometric bbox tile stays
// correct for them.  Every class with no resolvable model keeps round AU's ImDrawList
// isometric bbox, unchanged, on the same code path it has today.
//
// ── THE CACHE IS NOT THE RENDER TARGET ───────────────────────────────────────────────
// One shared 128×128 D3DPOOL_DEFAULT RT is rendered into, then copied out per entry:
//     RT (DEFAULT) --GetRenderTargetData--> one reused SYSTEMMEM surface --memcpy-->
//     a per-entry D3DPOOL_MANAGED texture
// which is the simplest correct D3D9 route to N cache entries from ONE render target.
// It adds exactly ONE default-pool object to the build (the RT, already swept by
// RTT_ReleaseForReset) instead of one per entry, and the CPU hop is what lets the copy
// force alpha to 0xFF: the RT is A8R8G8B8 and the scene's written alpha is not
// guaranteed opaque, which is the hazard imgui_shell.cpp:786 + :654 needs a per-draw
// ALPHABLENDENABLE callback for.  Baking the alpha means the tiles need no draw callback
// at all — and an ImDrawList callback splits the draw list, which in a grid of tiles is
// exactly what you do not want.
//
// ── FAILURE IS CACHED, NOT RETRIED ───────────────────────────────────────────────────
// A class whose model will not load is marked FAILED once and never attempted again, so
// a missing asset costs one tick, not one tick per frame forever.  The badge fallback
// stays for those.  The model LOAD runs inside the editor's ERR_DROP + SEH bracket
// (the same pair Editor_InstanceAndSkinModel installs, camwnd.cpp:1565) and — critically
// — it runs BEFORE RTT_BeginThumb, never between Begin and End: a longjmp out of a bound
// render target is the D3DERR_INVALIDCALL failure mode kiwi_devicereset.cpp:143 exists to
// report.
// ═════════════════════════════════════════════════════════════════════════════════════

struct eclass_t;
struct IDirect3DTexture9;

// The cached thumbnail for `ec`, or null when there is none.  Null means "draw the
// isometric bbox instead" and covers three cases the caller does not need to tell apart:
// not rendered yet, no class-level model, and load failed.  Cheap and side-effect-light:
// on a miss it records at most ONE pending request per frame for the next tick.
//
// KIWI-UX (ROUND AX, ITEM 3): `mayRequest` is the caller's "this tile is actually on
// screen" answer (ImGui::IsRectVisible).  A cache HIT is served either way — a visible
// tile and a scrolled-out one both draw from the same map — but only a visible tile is
// allowed to enqueue a load.  The browser draws every tile of every open group with no
// clipper, so without this the queue order was list order, not reading order, and the
// row under the cursor filled last.  Pair it with the budget in KiwiEntThumb_Tick.
IDirect3DTexture9 *KiwiEntThumb_Get( const eclass_t *ec, bool mayRequest );

// One bounded render step: at most one thumbnail, only when one is pending.  Called from
// ImGuiShell_RenderViewportsToRT, i.e. outside the compositing bracket and before
// ImGuiShell_BeginFrame, under the RTT_DeviceHealthy() gate that function already makes.
//
// ── KIWI-UX (ROUND AX, ITEM 3): WHY THIS IS A BUDGET AND NOT A THREAD ────────────────
// USER REQUEST: "It lags while loading the model previews (can you put it on a thread?)".
// The honest answer is no, and it is not a matter of effort.  The expensive call is
// R_RegisterModel, and everything under it is process-global, single-threaded state that
// the port inherits from the engine:
//   * the filesystem (fs_* globals, the searchpath list, one open-handle set);
//   * the material and image hash tables (imageGlobals.imageHashTable, Material_Add's
//     rgp.needSortMaterials) — no locks anywhere;
//   * the parse-thread info this file already has to save/restore (Com_GetParseThreadInfo);
//   * image upload, which is dx.device->CreateTexture on the SAME device the frame is
//     drawing with;
//   * and Com_Error, whose failure path is a longjmp to a setjmp frame owned by THIS
//     thread — an ERR_DROP raised on a worker has nowhere legal to land.
// A worker that touched any of those would be a rewrite of the asset layer, not a
// threading change.  A read-ahead thread that only pulled bytes into memory was also
// rejected: the file layer IS one of the shared globals, so the "clean ownership
// boundary" would have to be a private reader duplicating searchpath/iwd resolution — a
// second, silently divergent asset lookup, which is a worse bug than a stall.
// What is done instead is spacing: at most one registration per KENTT_MIN_GAP_MS, and
// additionally KENTT_COST_FACTOR x the previous one's measured cost.  Expected effect,
// stated plainly: the total time to fill a large group gets LONGER, and the editor stops
// being unresponsive while it happens.  Splitting register-from-render across two ticks
// was considered and rejected: registration dominates the cost so the win is small, and
// it would mean holding an XModel* across a tick boundary — across which a device reset
// can run Material_ReleaseAll (r_init.cpp:4652).
void KiwiEntThumb_Tick();

// Drop every cached texture and the readback surface.  Called from RTT_ReleaseForReset
// (radiant_rtt.cpp) so this feature has no device-reset hook of its own to forget.
// Idempotent — the INVALIDCALL retry arm runs the release list twice.
void KiwiEntThumb_ReleaseForReset();

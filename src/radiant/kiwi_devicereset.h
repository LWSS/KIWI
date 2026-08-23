#pragma once
// kiwi_devicereset.h — KIWI-UX ROUND AB, ITEM 1: surviving a D3D9 device RESET.
//
// ── THE BUG THIS FILE EXISTS FOR ────────────────────────────────────────────
// Monitor sleeps, monitor wakes, the D3D9 device is lost and then successfully
// reset by round V's machinery — and the NEXT healthy frame crashes:
//
//     RB_EndSurfacePrologue()                rb_shade.cpp:202
//     RB_EndTessSurface()                    rb_shade.cpp:189
//     RB_SetMaterialColorCmd(...)            rb_backend.cpp:1464
//     RB_ExecuteRenderCommandsLoop(...)      rb_backend.cpp:2736
//     RB_CallExecuteRenderCommands()         rb_backend.cpp:2820
//     R_IssueRenderCommands(...)             r_rendercmds.cpp:298
//     CamWnd_RenderToRT( w, h )              camwnd.cpp:4659
//     ImGuiShell_RenderViewportsToRT()       imgui_shell.cpp:759
//     Radiant_RunMessageLoop()               radiant_main.cpp:802
//
// `rb_shade.cpp:202` is `g_primStats->dynamicIndexCount += tess.indexCount;`.
// Only two operands, both file-scope globals, and `g_primStats` is only ever
// either NULL or `&g_viewStats->primStats[target]` (a static address,
// rb_stats.cpp:69-75).  So THE NULL IS `g_primStats`, and the state that
// produces the fault is `tess.indexCount != 0` while `g_primStats == 0`.
// The `iassert(g_primStats)` on the line above does fire — it is NON-FATAL in
// this build (assertive.cpp logs and falls through), so line 202 runs anyway.
//
// EXACTLY ONE PATH produces that pair.  `RB_DrawEditorSkinnedCached_Sub`
// (r_ed_scene.cpp:858) drives `tess` itself and never calls `R_TrackPrims`, so
// `g_primStats` is 0 for its whole duration; its final flush is guarded
// `if (haveBatch && boundVb)` (r_ed_scene.cpp:959).  An `ED_SURF_MESH` surf
// whose vertex buffer resolves to NULL leaves `boundVb == 0` while its indices
// were still copied into `tess` (r_ed_scene.cpp:941-943), so the flush is
// skipped and `tess.indexCount` LEAKS out of the command handler.  The very
// next `RC_SET_MATERIAL_COLOR` — the editor emits one per brush, per face, per
// entity — hits `if (tess.indexCount) RB_EndTessSurface();` and dies.
//
// And a NULL vertex buffer after a reset is guaranteed.  `Editor_VB_ReleaseForReset`
// (r_ed_vertbuf.cpp:125, called from r_init.cpp:4358) releases and NULLs every
// `editorGlobals.vb[]` and frees the whole pool — correctly, they are
// D3DPOOL_DEFAULT and Reset() fails otherwise.  But the per-face and per-patch
// `vertHandle`s CACHED in the editor surf cache (brush.cpp:2676 for faces,
// pmesh.cpp:9768 for patches) are never invalidated, and the faceVis rebuild
// only triggers on `b->version != b->def->version` (brush.cpp:200), which a
// device reset does not touch.  The comment at r_init.cpp:4356 claiming
// "faceVis visCount stays 0 in all build modes, so nothing dangles" was true
// when the surf cache was device-gated off; `Radiant_FaceVisGpuReady()` is
// `dx.device != nullptr` (camwnd.cpp:1194), so it is not true any more.
//
// A stale handle is worse than a crash once the pool refills, too: `vbCount`
// restarts at 0, so buffer index 1 becomes a DIFFERENT live buffer and the
// stale handle silently draws another brush's vertices.
//
// ── THE FIX ────────────────────────────────────────────────────────────────
// Invalidate the editor surf cache in the SAME breath as the pool release, so
// no cached handle can outlive the pool it indexes.  The invalidation must NOT
// hand the handles back (`Visuals_VisArray`/`R_Ed_FreeVertices` would push runs
// into freshly-recreated pools that never contained them) — it must simply drop
// them and arm the rebuild.  The binary already has that exact primitive:
// `Brush_InvalidateVis` (brush.cpp:1478, 0x478340) frees `b->faces` outright,
// runs `PMESH_22_Indices` for a patch (the deliberate no-R_Ed_FreeVertices twin,
// pmesh.cpp:9826) and sets `b->version = def->version - 1`.
//
// See RADIANT_UX_DESIGN.md §58.1.

// Drop every cached editor surf-cache vertex-buffer handle in the map and arm the
// rebuild.  Called from R_ReleaseForShutdownOrReset (r_init.cpp) immediately
// before Editor_VB_ReleaseForReset.  Safe to call with no map loaded and safe to
// call twice.
void KiwiDevice_InvalidateEditorSurfCache();

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX ROUND AD — THE DEVICE THAT NEVER CAME BACK
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: *"I had it hang while loading a map, we need to fix
// this"* — and, after Break All in the debugger: the main thread parked in
// `MsgWaitForMultipleObjects` at `Radiant_RunMessageLoop` (radiant_main.cpp:844),
// *"screen all black, never loads.  all other threads are just nvidia."*
//
// THE APP WAS NOT HUNG.  radiant_main.cpp:762-846 ticks at least every 16 ms
// whatever happens, and :843 is that tick's normal idle park.  Every tick ran
// `ImGuiShell_RenderViewportsToRT` → `ImGuiShell_BeginFrame` → `InvalidateRect` +
// `UpdateWindow` → the frame WM_PAINT (radiant_main.cpp:164-209).  The screen was
// black because that paint's one gate —
// `R_SetupRendertarget_CheckDevice` (r_init.cpp:4793) — answered FALSE on every
// tick, forever, WITHOUT PRINTING ANYTHING.  A device loss during a heavy map load
// (VRAM churn / a driver TDR — "all other threads are just nvidia") is exactly the
// shape that produces it.
//
// ── WHY THE RECOVERY COULD NEVER FINISH (the concrete bug) ──────────────────
// `Reset()` fails `D3DERR_INVALIDCALL` while ANY app-created D3DPOOL_DEFAULT
// resource is still alive.  Round V + round AB released everything the editor was
// known to own (swap chains, render targets, dynamic buffers, RTT textures, the
// editor VB pool, ImGui's buffers, queries).  ONE CLASS WAS MISSED, and it is
// map-content-dependent, which is why "it happened during a map load":
//
//     UNMANAGED GfxImages — `image->category >= IMG_CATEGORY_FIRST_UNMANAGED (5)`.
//
// A water image (`IMG_CATEGORY_WATER`, category 5) is built by `Image_BuildWaterMap`
// (r_image_load_obj.cpp:309) with imageFlags 0x10001, so `Image_GetUsage`
// (r_image.h:239) returns 512 = `D3DUSAGE_DYNAMIC`, and
// `Image_Create2DTexture_PC` (r_image.cpp:1355-1363) passes the pool as
// `(_D3DPOOL)(usage == 0)` — i.e. usage != 0 ⇒ **D3DPOOL_DEFAULT**.
// The engine releases these on the reset path in `R_ReleaseLostImages`
// (r_image.cpp:1134) — which is `DB_EnumXAssets(ASSET_TYPE_IMAGE, R_FreeLostImage,
// ...)`, and `DB_EnumXAssets` IS A NO-OP STUB IN THE EDITOR
// (engine_stubs.cpp:537; db_registry.cpp is not in radiant_files.cmake).  So in
// Radiant `R_ReleaseLostImages`, `R_ReloadLostImages` and `Material_ReleaseAll`
// all do NOTHING, and every unmanaged image stays alive across the whole reset
// cascade.  Load a map that references water, lose the device, and `Reset()`
// returns `D3DERR_INVALIDCALL` for the rest of the session.
//
// The editor's images are not in the DB at all: they live in the open-addressed
// `imageGlobals.imageHashTable` (r_image.h:111, 32768 slots — the editor size),
// which `Image_FindExisting_LoadObj` (r_image_load_obj.cpp:240) and `Image_Alloc`
// index.  That table is the enumeration the editor has, so it is the one the
// release/rebuild passes below walk.
//
// ── AND WHY A RETRY COULD NEVER FIX IT EITHER ──────────────────────────────
// Round V's reset-retry latch (`s_releasedForReset`, r_init.cpp:4381) exists
// because the release cascade is destructive and non-idempotent, so a retry
// "skips straight to Reset()".  That is right for the failure it was written for
// (`D3DERR_DEVICELOST` — the device is simply not ready yet).  It is exactly wrong
// for `D3DERR_INVALIDCALL`, which means *the device state is not acceptable*: a
// retry that releases nothing and unbinds nothing will get the same answer every
// time until the process dies.  Round AD gives that failure its own arm.
//
// See RADIANT_UX_DESIGN.md §59.

// D3DPOOL_DEFAULT image release / rebuild — the editor's stand-in for
// R_ReleaseLostImages / R_ReloadLostImages, which are dead in this build (see
// above).  Both walk imageGlobals.imageHashTable and reproduce the engine's own
// per-image rules verbatim (R_FreeLostImage r_image.cpp:1024, R_RebuildLostImage
// r_image.cpp:1101).  Both are IDEMPOTENT and safe with no map loaded.
void KiwiDevice_ReleaseUnmanagedImages();   // before Reset()
void KiwiDevice_RebuildUnmanagedImages();   // after a Reset() that succeeded

// Diagnostics.  The recovery chain must never fail silently again: these record
// the two HRESULTs that decide everything, and KiwiDevice_FrameHealthWatch turns a
// run of black frames into ONE throttled line (console + OutputDebugStringA).
void KiwiDevice_NoteCoopLevel( long hr );                    // R_TestDevice
void KiwiDevice_NoteResetResult( long hr, int releasePass ); // R_ResetDevice

// KIWI-UX (ROUND AX, ITEM 1) — name the LOSS, not just the failed recovery.
// Round AD made the recovery chain loud and left the loss itself silent, so a report
// could establish that Reset() kept failing without establishing what took the device
// down in the first place.  Called from BOTH sites that set dx.deviceLost
// (r_init.cpp R_CheckLostDevice and R_TestDevice) with the site name and the
// TestCooperativeLevel HRESULT that decided it.  ONE line per loss episode — the first
// HRESULT is the discriminator (DEVICELOST / DRIVERINTERNALERROR is the driver; anything
// else points back at the editor) and the ones after it are noise.  Re-arms when a
// Reset() finally succeeds.
void KiwiDevice_NoteLoss( const char *where, long hr );
                                                             // releasePass: 0 none, 1 full, 2 second-chance

// Called from the frame WM_PAINT, AFTER ::EndPaint.
//   `authorized` — the pump asked for THIS paint (ImGuiShell_FrameAuthorized).  A
//                  stray paint (OS repaint, a nested TrackPopupMenu/MessageBox
//                  loop) draws nothing BY DESIGN since round U and is no evidence
//                  of anything; those ticks are ignored outright, which is what
//                  keeps a file dialog left open for a minute from tripping the
//                  escape hatch below.
//   `painted`    — the scene bracket actually ran.
// Throttled reporting while an authorized tick renders nothing, and the
// permanent-loss escape hatch (rescue save + one message box) at ~15 s.
void KiwiDevice_FrameHealthWatch( struct HWND__ *frame, bool authorized, bool painted );

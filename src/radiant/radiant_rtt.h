#pragma once
// radiant_rtt.h — Phase 5: render each editor viewport into an offscreen D3D9 texture
// that ImGui samples as an image, replacing the native-child-HWND-per-viewport scheme.
// See RADIANT_UI_REWORK_PLAN.md (PHASE 5 — RTT VIEWPORTS).
struct IDirect3DTexture9;
struct IDirect3DSurface9;

enum rttViewport_t
{
    RTT_CAMERA = 0,
    RTT_XY,
    RTT_Z,
    RTT_TEXTURE,
    RTT_COUNT
};

// Ensure viewport `id`'s render-target texture exists at w×h (recreates on size change),
// point the engine's FRAME_BUFFER at it, and suppress Present. Returns false if the target
// couldn't be set up (skip the viewport's draw). Pair every true with RTT_End().
bool RTT_Begin( rttViewport_t id, int w, int h );
void RTT_End();

// The sampleable texture for ImGui::Image( (ImTextureID)RTT_GetTexture(id), ... ). May be
// null before the first successful RTT_Begin for that id.
IDirect3DTexture9 *RTT_GetTexture( rttViewport_t id );

// Device-loss bracket: release every RT texture (recreated lazily on the next RTT_Begin).
// Called from R_ReleaseForShutdownOrReset (r_init.cpp).
void RTT_ReleaseForReset();

// KIWI-UX (ROUND AB, ITEM 1): the same three-part test RTT_Begin makes, hoisted so the
// whole viewport-render pass can be skipped as ONE decision instead of four independent
// ones.  See RADIANT_UX_DESIGN.md §58.1.
bool RTT_DeviceHealthy();

// ═══════════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AV, ITEM 3) — THE ENTITY-BROWSER THUMBNAIL SURFACE.
// ═══════════════════════════════════════════════════════════════════════════════════
// A FIFTH render target, deliberately NOT a fifth `rttViewport_t`.  `RTT_COUNT` is not
// just the slot-array bound: `imgui_shell.cpp` sizes its per-viewport input arrays
// (s_cellW / s_cellH / s_hovered / s_wheel / s_imgMin) by it and
// `ImGuiShell_DispatchViewportInput` loops over all of them.  A new enumerator would
// silently enrol the thumbnail surface in the viewport INPUT dispatch, which is a frame
// loop rounds V and AB were spent stabilising.  A standalone slot touches none of that
// while reusing this file's EnsureSlot / FreeSlot pool discipline verbatim — and, most
// importantly, it is freed by the SAME `RTT_ReleaseForReset()` that is already
// registered in BOTH device-release sites (`r_init.cpp:4276` and the INVALIDCALL
// second-chance list at `r_init.cpp:4524`).  So the new default-pool object inherits
// complete reset coverage with no new hook anywhere.
//
// ── KIWI-UX (ROUND AX, ITEM 1): THAT LAST SENTENCE WAS THE INTENT, NOT THE CODE. ────
// `RTT_ReleaseForReset` released the slots with a range-`for` over the ARRAY `s_slots`,
// and a STANDALONE slot is not in an array.  The thumbnail's D3DPOOL_DEFAULT render
// target therefore survived every release pass, and the first device loss after the
// first thumbnail ever rendered latched `Reset() == D3DERR_INVALIDCALL` for the rest of
// the session — the "editor black forever while loading a map" report.  The DESIGN above
// is unchanged and still right; the release function now frees the slot EXPLICITLY
// (radiant_rtt.cpp), and `RTT_DescribeLiveSlots` below exists so that the next such
// omission is NAMED in the device-health line instead of being silent.  §75.
// It is small (128×128 A8R8G8B8) and needs no depth-stencil of its own: like every
// other RTT slot it borrows the engine's shared display-sized one, which is strictly
// larger.  Pair every `true` with RTT_EndThumb() — an unpaired Begin parks an
// app-owned surface as the active render target and the next Reset() fails
// D3DERR_INVALIDCALL (kiwi_devicereset.cpp:143 reports exactly that).
bool RTT_BeginThumb( int w, int h );
void RTT_EndThumb();
// The level-0 surface of the thumbnail RT, for GetRenderTargetData(). Null until the
// first successful RTT_BeginThumb.
IDirect3DSurface9 *RTT_ThumbSurface();

// KIWI-UX (ROUND AX, ITEM 1) — the audit that would have made this round a one-liner.
// Writes a comma-separated list of the RTT slots that STILL hold a D3DPOOL_DEFAULT
// texture ("cam,xy,z,tex,thumb"), or "none", into `out`, and returns how many.  Called
// from the device-health reporter after a Reset() that failed D3DERR_INVALIDCALL, which
// is exactly the failure "an app-created default-pool object is still alive" produces.
// Pure reads of the slot handles: safe on any device state, including a dead device.
int RTT_DescribeLiveSlots( char *out, int outSize );

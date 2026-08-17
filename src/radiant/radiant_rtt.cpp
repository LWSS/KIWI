// radiant_rtt.cpp — Phase 5 RTT viewport texture pool. See radiant_rtt.h.
#include "stdafx.h"
#include "radiant_rtt.h"

#include <d3d9.h>
#include <string.h>                  // KIWI-UX (ROUND AX): strncpy in RTT_DescribeLiveSlots
#include <gfx_d3d/r_init.h>          // dx, R_SetupRenderTargetTexture

// Present-suppress flag read by RB_SwapBuffers (rb_backend.cpp): true while a viewport
// renders into its RT so the offscreen render does not Present. Cleared for the final
// compositing frame (the ImGui frame on the real window), which is the only Present.
bool g_rbSuppressPresent = false;

namespace
{
    struct rtSlot_t
    {
        IDirect3DTexture9 *tex     = nullptr;
        IDirect3DSurface9 *surface = nullptr;
        int                w       = 0;
        int                h       = 0;
    };
    rtSlot_t s_slots[RTT_COUNT];

    // KIWI-UX (ROUND AV, ITEM 3) — the entity-browser thumbnail RT.  A standalone slot,
    // NOT a fifth rttViewport_t; see radiant_rtt.h for why that distinction matters.
    rtSlot_t s_thumbSlot;

    void FreeSlot( rtSlot_t &s )
    {
        if ( s.surface ) { s.surface->Release(); s.surface = nullptr; }
        if ( s.tex )     { s.tex->Release();     s.tex     = nullptr; }
        s.w = s.h = 0;
    }

    // (Re)create slot `s` at w×h if needed. A8R8G8B8 + D3DUSAGE_RENDERTARGET in the default
    // pool matches the frame-buffer format (r_init.cpp d3dpp.BackBufferFormat) so the engine's
    // FRAME_BUFFER bind is format-compatible. Returns false on failure.
    bool EnsureSlot( rtSlot_t &s, int w, int h )
    {
        if ( w < 1 ) w = 1;
        if ( h < 1 ) h = 1;
        if ( s.tex && s.w == w && s.h == h )
            return true;
        FreeSlot( s );
        if ( !dx.device )
            return false;
        HRESULT hr = dx.device->CreateTexture( (UINT)w, (UINT)h, 1, D3DUSAGE_RENDERTARGET,
                                               D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s.tex, nullptr );
        if ( hr < 0 || !s.tex )
        {
            s.tex = nullptr;
            return false;
        }
        if ( s.tex->GetSurfaceLevel( 0, &s.surface ) < 0 || !s.surface )
        {
            FreeSlot( s );
            return false;
        }
        s.w = w;
        s.h = h;
        return true;
    }
}

// KIWI-UX (ROUND AB, ITEM 1).  RTT_Begin's device test, callable on its own so the pump
// can skip the WHOLE viewport pass rather than have four viewports each discover the loss
// and each leave a half-built state behind.  Two reasons it is worth hoisting:
//   * ONE TestCooperativeLevel per frame instead of four.
//   * A partially-rendered pass is worse than none: CamWnd_RenderToRT can succeed and
//     XYWnd_RenderToRT then fail, leaving three stale RT images composited against one
//     fresh one.  The white-flicker guard (ImGuiShell_FrameAuthorized, imgui_shell.cpp:1101)
//     already tolerates a tick with no scene render at all, so skipping is free.
// It does NOT replace RTT_Begin's own check — the device can be lost between this call and
// any one viewport's Begin, and that check is the one that keeps a lost-device reset from
// running with an app-created D3DPOOL_DEFAULT surface as the active render target.
bool RTT_DeviceHealthy()
{
    if ( !dx.device || dx.deviceLost )
        return false;
    return dx.device->TestCooperativeLevel() == D3D_OK;
}

bool RTT_Begin( rttViewport_t id, int w, int h )
{
    if ( id < 0 || id >= RTT_COUNT )
        return false;

    // NEVER render into an RTT while the D3D device is lost / awaiting reset. The viewport draw
    // calls R_IssueRenderCommands, whose R_CheckLostDevice would run R_ResetDevice MID-RTT — i.e.
    // with this app-created D3DPOOL_DEFAULT surface as the device's ACTIVE render target. The
    // device holds an internal ref on its current RT, so Reset() then fails D3DERR_INVALIDCALL
    // ("Couldn't reset a lost Direct3D device"). Bailing here defers recovery to the compositing
    // frame, whose active RT is the window backbuffer (implicit to a swap chain) — the reset path
    // that already works for alt-tab. Loading a huge map (blackout) can transiently lose the
    // device (VRAM pressure), which is exactly when this fired.
    if ( !dx.device || dx.deviceLost )
        return false;
    if ( dx.device->TestCooperativeLevel() != D3D_OK )
        return false;

    rtSlot_t &s = s_slots[id];
    if ( !EnsureSlot( s, w, h ) )
        return false;
    g_rbSuppressPresent = true;                    // this render must not Present
    R_SetupRenderTargetTexture( s.surface, s.w, s.h );
    return true;
}

void RTT_End()
{
    g_rbSuppressPresent = false;
    // Let the next R_Setup* (the compositing frame's window setup, or the next viewport)
    // rebind cleanly — R_SetupRendertarget_CheckDevice early-outs if still "targeting a
    // window", so clear the index the RTT render parked.
    dx.targetWindowIndex = -1;
}

IDirect3DTexture9 *RTT_GetTexture( rttViewport_t id )
{
    if ( id < 0 || id >= RTT_COUNT )
        return nullptr;
    return s_slots[id].tex;
}

// ── KIWI-UX (ROUND AV, ITEM 3) — the thumbnail RT ────────────────────────────────────
// Same three device gates and the same lazy EnsureSlot as RTT_Begin; the ONLY difference
// is which slot it targets.  Written out rather than folded into RTT_Begin so the enum
// range check there stays a pure `id < RTT_COUNT` and no caller can reach the thumbnail
// surface by passing an out-of-range viewport id.
bool RTT_BeginThumb( int w, int h )
{
    if ( !dx.device || dx.deviceLost )
        return false;
    if ( dx.device->TestCooperativeLevel() != D3D_OK )
        return false;
    if ( !EnsureSlot( s_thumbSlot, w, h ) )
        return false;
    g_rbSuppressPresent = true;
    R_SetupRenderTargetTexture( s_thumbSlot.surface, s_thumbSlot.w, s_thumbSlot.h );
    return true;
}

void RTT_EndThumb()
{
    RTT_End();          // identical teardown: clear the present-suppress + the parked index
}

IDirect3DSurface9 *RTT_ThumbSurface()
{
    return s_thumbSlot.surface;
}

// ── KIWI-UX (ROUND AX, ITEM 1) — THE LEAK THAT MADE EVERY Reset() FAIL FOREVER ─────
// USER REPORT: "hit this while loading a new map ... consistently reproduce this hang
// while loading kisak_trash.map.  Unusable!"  The round-AD escape hatch fired with
// TestCooperativeLevel = D3DERR_DEVICENOTRESET and Reset = D3DERR_INVALIDCALL after 876
// attempts, i.e. the exact signature of an app-created D3DPOOL_DEFAULT resource that is
// still alive when Reset() runs.
//
// IT WAS THIS FUNCTION, AND IT WAS ONE LINE.  Round AV added the thumbnail RT as a
// STANDALONE slot (`s_thumbSlot`, :26) rather than a fifth `rttViewport_t`, for the good
// reasons radiant_rtt.h gives — but the release loop below is
//     for ( rtSlot_t &s : s_slots )
// and `s_slots` is the RTT_COUNT-element ARRAY.  A standalone object outside that array
// is not "in the loop above" no matter what the comment said; `s_thumbSlot` held a
// D3DUSAGE_RENDERTARGET / D3DPOOL_DEFAULT texture AND its level-0 surface across every
// release pass, so the very first device loss after the first thumbnail ever rendered
// latched the editor black for the rest of the session.  Round AD's whole recovery chain
// was working correctly and reporting correctly; it simply could never win.
//
// The fix is the explicit FreeSlot below.  It needs no new hook anywhere: BOTH device-
// release sites already call this function (r_init.cpp:4276 full pass, r_init.cpp:4524
// INVALIDCALL second-chance pass), and the slot is recreated LAZILY by EnsureSlot on the
// next RTT_BeginThumb, exactly like the four viewport slots.  FreeSlot null-checks both
// handles, so the double call the retry arm makes is a no-op (the idempotency rule stated
// at r_init.cpp:4500).
//
// ORDER IS ALSO CORRECT AND IS LOAD-BEARING.  R_SetupRenderTargetTexture ADDREFS the
// surface into gfxRenderTargets[FRAME_BUFFER].surface.color (r_init.cpp:4895-4899), so
// after a thumbnail render there are TWO refs on it.  This function runs BEFORE
// R_ShutdownRenderTargets in both passes (r_init.cpp:4276 vs :4307, and :4524 vs :4527),
// so ours drops first and the frame-buffer target's drops second — the surface is gone
// before Reset() either way.
void RTT_ReleaseForReset()
{
    for ( rtSlot_t &s : s_slots )
        FreeSlot( s );
    FreeSlot( s_thumbSlot );        // ROUND AX: NOT covered by the loop — see above.
    // KIWI-UX (ROUND AV, ITEM 3).  The thumbnail RT is released just above, and the
    // thumbnail CACHE (managed textures + the SYSTEMMEM readback surface, kiwi_entthumb.cpp)
    // is dropped with it.  Neither pool strictly requires it — MANAGED and SYSTEMMEM both
    // survive a Reset by the D3D9 contract — but dropping them here buys two things worth
    // more than the re-render: this whole feature's device-reset story becomes ONE already
    // registered function (so it is covered by the INVALIDCALL second-chance list for free,
    // r_init.cpp:4524), and no ImTextureID can outlive the
    // ImGuiShell_InvalidateDeviceObjects that runs immediately before us (r_init.cpp:4272).
    // Cost: after an alt-tab the visible tiles re-render one per frame.  Idempotent, so it
    // tolerates the double call the retry arm makes — the rule stated at r_init.cpp:4500.
    extern void KiwiEntThumb_ReleaseForReset();   // kiwi_entthumb.cpp
    KiwiEntThumb_ReleaseForReset();
    // KIWI-UX (ROUND BA).  The Sky tab's tiles are copies of one face of the sky
    // CUBEMAP (kiwi_skybox.h), also D3DPOOL_MANAGED, and they join this list for
    // exactly the two reasons stated above: one registered teardown for the whole
    // feature, and no ImTextureID outliving ImGuiShell_InvalidateDeviceObjects.
    extern void KiwiSky_ReleaseForReset();        // kiwi_skybox.cpp
    KiwiSky_ReleaseForReset();
}

// KIWI-UX (ROUND AX, ITEM 1).  See radiant_rtt.h.  This round cost a full investigation
// only because "which app-created D3DPOOL_DEFAULT object is still alive?" had no answer
// inside the editor — round AD's health line could say Reset() returned D3DERR_INVALIDCALL
// but not why.  Every slot this file owns is now nameable in one call.  Pure handle reads:
// no D3D call, so it is safe on a dead device and inside the reset path itself.
int RTT_DescribeLiveSlots( char *out, int outSize )
{
    // KIWI-UX (CLEANUP, C-17): both tables below are SIZED from RTT_COUNT and
    // INITIALISED by hand.  Adding a fifth viewport would compile clean, leave a
    // nullptr in s_all, and the loop below would deref it — a null read inside the
    // device-loss diagnostic, i.e. exactly when you least want one.
    static_assert( RTT_COUNT == 4, "RTT_DescribeLiveSlots' name/pointer tables are "
                                   "hand-written — extend both when RTT_COUNT moves" );
    static const char *const s_names[RTT_COUNT + 1] = { "cam", "xy", "z", "tex", "thumb" };
    const rtSlot_t *const    s_all[RTT_COUNT + 1]   = { &s_slots[RTT_CAMERA], &s_slots[RTT_XY],
                                                        &s_slots[RTT_Z],      &s_slots[RTT_TEXTURE],
                                                        &s_thumbSlot };
    int    live = 0;
    size_t used = 0;
    if ( out && outSize > 0 )
        out[0] = '\0';

    for ( int i = 0; i < RTT_COUNT + 1; ++i )
    {
        if ( !s_all[i]->tex && !s_all[i]->surface )
            continue;
        ++live;
        if ( !out || outSize <= 0 )
            continue;
        // Manual append rather than _snprintf accumulation: a truncating _snprintf
        // returns -1 on MSVC, and an offset advanced by -1 is a buffer underrun.
        const char *piece = s_names[i];
        if ( used && used + 1 < (size_t)outSize )
            out[used++] = ',';
        while ( *piece && used + 1 < (size_t)outSize )
            out[used++] = *piece++;
        out[used] = '\0';
    }
    if ( out && outSize > 0 && !live )
    {
        strncpy( out, "none", (size_t)outSize - 1 );
        out[outSize - 1] = '\0';
    }
    return live;
}

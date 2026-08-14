// radiant_rtt.cpp — Phase 5 RTT viewport texture pool. See radiant_rtt.h.
#include "stdafx.h"
#include "radiant_rtt.h"

#include <d3d9.h>
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

void RTT_ReleaseForReset()
{
    for ( rtSlot_t &s : s_slots )
        FreeSlot( s );
}

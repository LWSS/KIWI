#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Dirty-gates the XY and Z RTTs; unknown state renders.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include "xywnd.h"
#include "kiwi_command.h"
#include "kiwi_viewdirty.h"

namespace
{
    struct viewDirty_t
    {
        bool dirty     = true;    // force the initial render
        bool everDrawn = false;
        int  w         = -1;
        int  h         = -1;
        unsigned lastTick = 0;
        unsigned long long lastRenderMs = 0;   // GetTickCount64 at the last render
    };
    viewDirty_t s_view[KIWI_DIRTYVIEW_COUNT];

    // KIWI (2026-09-10, Tracy lagwhiledrag.tracy): while a modal command is live the 2D
    // views were re-rendered EVERY editor frame (OverlayForcesRender), and the XY pass of a
    // big map costs ~15 ms — a quarter of the 64 ms drag frame.  The camera is what the
    // operator is looking at during a drag; the 2D views now follow at this cadence and
    // catch up on the frame the gesture ends (the dirty flag is left set meanwhile).
    const unsigned long long KIWI_VIEWDIRTY_GESTURE_MS = 100;

    unsigned s_tick        = 0;
    // Shared across views to stagger heartbeat renders; -1 leaves tick zero available.
    unsigned s_lastBeatTick = (unsigned)-1;

    // Inputs that change overlays without setting g_nUpdateBits.
    bool OverlayForcesRender( kiwiDirtyView_t view )
    {
        // Active commands can animate previews in both 2D views without invalidating them.
        if ( KiwiCmd_Active() )
            return true;
        if ( view == KIWI_DIRTYVIEW_XY )
            return XYWnd_OverlayIsLive();
        return false;
    }
}

void KiwiViewDirty_Mark( kiwiDirtyView_t view )
{
    if ( (int)view >= 0 && (int)view < (int)KIWI_DIRTYVIEW_COUNT )
        s_view[view].dirty = true;
}

void KiwiViewDirty_MarkAll()
{
    for ( int i = 0; i < KIWI_DIRTYVIEW_COUNT; ++i )
        s_view[i].dirty = true;
}

void KiwiViewDirty_MarkFromUpdateBits( int bits )
{
    if ( bits & ( W_XY | W_XY_OVERLAY ) )
        s_view[KIWI_DIRTYVIEW_XY].dirty = true;
    if ( bits & ( W_Z | W_Z_OVERLAY ) )
        s_view[KIWI_DIRTYVIEW_Z].dirty = true;
}

bool KiwiViewDirty_ShouldRender( kiwiDirtyView_t view, int w, int h )
{
    if ( (int)view < 0 || (int)view >= (int)KIWI_DIRTYVIEW_COUNT )
        return true;                                   // unknown view: fail open
    viewDirty_t &s = s_view[view];

    // Correctness, not optimisation: a size change makes RTT recreate the texture.
    bool render = s.dirty || !s.everDrawn || w != s.w || h != s.h;

    if ( !render && KIWI_VIEWDIRTY_HEARTBEAT_TICKS > 0
         && s_lastBeatTick != s_tick
         && ( s_tick - s.lastTick ) >= (unsigned)KIWI_VIEWDIRTY_HEARTBEAT_TICKS )
    {
        render         = true;
        s_lastBeatTick = s_tick;
    }

    if ( !render && OverlayForcesRender( view ) )
        render = true;

    // Live-gesture cadence (KIWI_VIEWDIRTY_GESTURE_MS): a size change or a never-drawn
    // view still renders at once; everything else waits, and stays dirty so the first
    // frame after the gesture repaints it.
    const unsigned long long nowMs = ::GetTickCount64();
    if ( render && KiwiCmd_Active() && s.everDrawn && w == s.w && h == s.h
      && ( nowMs - s.lastRenderMs ) < KIWI_VIEWDIRTY_GESTURE_MS )
        return false;

    if ( render )
    {
        s.dirty        = false;
        s.everDrawn    = true;
        s.w            = w;
        s.h            = h;
        s.lastTick     = s_tick;
        s.lastRenderMs = nowMs;
    }
    return render;
}

void KiwiViewDirty_EndTick()
{
    ++s_tick;
}

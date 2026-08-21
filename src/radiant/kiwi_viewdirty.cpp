#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_viewdirty.cpp - mechanism for kiwi_viewdirty.h.  Every uncertain answer is "no".

#include "stdafx.h"
#include "qe3.h"                  // qedefs.h -> W_XY / W_XY_OVERLAY / W_Z / W_Z_OVERLAY
#include "mainfrm.h"              // camera_s — kiwi_command.h's prerequisite (as kiwi_boxselect.cpp:31)
#include "xywnd.h"                // XYWnd_OverlayIsLive (xywnd.h:35)
#include "kiwi_command.h"         // KiwiCmd_Active (kiwi_command.h:1133)
#include "kiwi_viewdirty.h"

namespace
{
    struct viewDirty_t
    {
        bool dirty     = true;    // nothing has been rendered yet — render once
        bool everDrawn = false;
        int  w         = -1;
        int  h         = -1;
        unsigned lastTick = 0;
    };
    viewDirty_t s_view[KIWI_DIRTYVIEW_COUNT];

    unsigned s_tick        = 0;   // pump ticks, advanced by KiwiViewDirty_EndTick
    // The tick the heartbeat last granted a render on: at most one view per tick.
    // s_tick starts at 0, so seed this out of band.
    unsigned s_lastBeatTick = (unsigned)-1;

    // The force-dirty predicate: state that no g_nUpdateBits site announces.
    bool OverlayForcesRender( kiwiDirtyView_t view )
    {
        // A modal KIWI tool previews every frame without invalidating anything.  BOTH views.
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
    // The SAME masks Radiant_UpdateWindows dispatches its RedrawWindow calls on.
    if ( bits & ( W_XY | W_XY_OVERLAY ) )
        s_view[KIWI_DIRTYVIEW_XY].dirty = true;
    if ( bits & ( W_Z | W_Z_OVERLAY ) )
        s_view[KIWI_DIRTYVIEW_Z].dirty = true;
}

bool KiwiViewDirty_ShouldRender( kiwiDirtyView_t view, int w, int h )
{
    if ( (int)view < 0 || (int)view >= (int)KIWI_DIRTYVIEW_COUNT )
        return true;                                   // unknown view -> render
    viewDirty_t &s = s_view[view];

    // Correctness, not optimisation: a size change makes RTT recreate the texture.
    bool render = s.dirty || !s.everDrawn || w != s.w || h != s.h;

    if ( !render && KIWI_VIEWDIRTY_HEARTBEAT_TICKS > 0
         && s_lastBeatTick != s_tick
         && ( s_tick - s.lastTick ) >= (unsigned)KIWI_VIEWDIRTY_HEARTBEAT_TICKS )
    {
        render         = true;                         // the missed-source insurance
        s_lastBeatTick = s_tick;                       // ...one view per tick, staggered
    }

    if ( !render && OverlayForcesRender( view ) )
        render = true;

    if ( render )
    {
        s.dirty     = false;
        s.everDrawn = true;
        s.w         = w;
        s.h         = h;
        s.lastTick  = s_tick;
    }
    return render;
}

void KiwiViewDirty_EndTick()
{
    ++s_tick;
}

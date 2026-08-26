#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Dirty gate for XY/Z RTTs, fed from the g_nUpdateBits drain. Camera and texture
// rendering are deliberately outside this gate.

enum kiwiDirtyView_t
{
    KIWI_DIRTYVIEW_XY = 0,
    KIWI_DIRTYVIEW_Z  = 1,
    KIWI_DIRTYVIEW_COUNT
};

// Missed-invalidation backstop in pump ticks; 120 is about 2 s at 60 Hz, 0 disables.
#define KIWI_VIEWDIRTY_HEARTBEAT_TICKS 120

// Mark from the g_nUpdateBits snapshot drained by Radiant_RoutineProcessing.
void KiwiViewDirty_MarkFromUpdateBits( int bits );

// Re-arm after invalidation outside g_nUpdateBits, such as an RTT reset or viewport reopen.
void KiwiViewDirty_MarkAll();
void KiwiViewDirty_Mark( kiwiDirtyView_t view );

// Return and consume the gate; a dock-cell size change forces an RTT redraw.
bool KiwiViewDirty_ShouldRender( kiwiDirtyView_t view, int w, int h );

// Advance heartbeat time once per healthy viewport-rendering tick.
void KiwiViewDirty_EndTick();

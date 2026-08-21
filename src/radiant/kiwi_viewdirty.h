#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
//  kiwi_viewdirty.h - the XY and Z views render only when their content can have
//  changed.  The dirty source is g_nUpdateBits, marked at its single drain in
//  Radiant_RoutineProcessing; the camera and texture views are deliberately not gated.

enum kiwiDirtyView_t
{
    KIWI_DIRTYVIEW_XY = 0,
    KIWI_DIRTYVIEW_Z  = 1,
    KIWI_DIRTYVIEW_COUNT
};

// Ticks between forced re-renders of a view nothing has marked dirty.  The pump caps at
// 60 Hz, so 120 == 2 s.  0 disables the heartbeat.
#define KIWI_VIEWDIRTY_HEARTBEAT_TICKS 120

// The drain hook.  `bits` is what Radiant_RoutineProcessing just took out of
// g_nUpdateBits; W_XY|W_XY_OVERLAY mark the XY view, W_Z|W_Z_OVERLAY the Z view.
void KiwiViewDirty_MarkFromUpdateBits( int bits );

// Unconditional mark, for events the bit currency does not express: the RTT pool being
// released for a device reset, a viewport window being reopened.
void KiwiViewDirty_MarkAll();
void KiwiViewDirty_Mark( kiwiDirtyView_t view );

// The gate: true = render this view now (and the flag is consumed).  `w`/`h` are the
// dock-cell size the render would use; a change in them is a render, because the RT is
// destroyed and recreated.
bool KiwiViewDirty_ShouldRender( kiwiDirtyView_t view, int w, int h );

// Advance the tick counter the heartbeat measures against.  Once per pump tick.
void KiwiViewDirty_EndTick();

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Directional box selection: left-to-right contains; right-to-left crosses.
// Shift adds, Ctrl removes and wins over Shift, and plain replaces.  Drags below
// KBOX_CLICK_PIXELS are clicks with the same modifiers.
// Granularity resolves once per rect from KiwiSel_GetModeMask(), in object, face,
// edge, then vertex priority.  Coordinates are camera-RTT image pixels with a
// top-left origin.  The modern-input dispatch gate leaves legacy camera drag intact.

// Absorb ordinary hand wobble without consuming a useful marquee-sized drag.
#define KBOX_CLICK_PIXELS 8

void KiwiBox_Begin ( int imgX, int imgY, bool shift, bool ctrl );
void KiwiBox_Update( int imgX, int imgY );

// Resolve the marquee (or the click) into KiwiSel(), then Sel_SyncToLegacy() and
// invalidate the views.  Ends the gesture either way.
void KiwiBox_End   ( int imgX, int imgY );

// Force-abort: drop the marquee, change nothing.  Used by the shell's stuck-drag
// teardown, which must run the owner's OWN teardown and never pre-empt a normal
// release edge.
void KiwiBox_Cancel();

// The live rect for the screen-space overlay.  x0/y0 is the PRESS point and x1/y1
// the current cursor (not normalised — the caller draws the direction), and
// `crossing` is true for a right-to-left drag.
bool KiwiBox_Rect( int *x0, int *y0, int *x1, int *y1, bool *crossing );

// True while the live marquee will remove its candidates.  The screen rectangle
// uses this to share the hover layer's warm remove/warning palette.
bool KiwiBox_RemovePreview();

// Draw read-only live candidates; emit nothing while inactive or below the click
// threshold.  The collection throttle and rendering limits live in the definition.
void KiwiBox_DrawPreview();

// Export the same click arbitration for paused commands that permit reselect.
// The caller must end its gesture first; auto-enter refuses while a command is live.
void KiwiBox_ClickSelectAt( int imgX, int imgY, bool shift, bool ctrl );

// Collect whole-brush candidates without changing selection or command state.
// Granularity is forced to SEL_OBJECT for solid-level tools.  Endpoints are camera-
// RTT image pixels in either order; crossing chooses touch over containment and
// includes center-ray rescue.  Returns at most maxOut pointers; recheck each with
// Sel_BrushLive before a later dereference.
struct selbrush_t;                // qe3.h:429 (the 56-byte brush INSTANCE / list node)
int KiwiBox_CollectBrushes( int x0, int y0, int x1, int y1, bool crossing,
                            selbrush_t **out, int maxOut );

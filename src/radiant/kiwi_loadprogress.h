#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_loadprogress.h — feedback for synchronous loads, via the frame's WINDOW TITLE.
// No ImGui frame may be nested from inside a load (the shell's s_beginFrame/s_inFrame
// guards reject it), and nothing here pumps; SetWindowTextA repaints anyway.

// Open a bracket.  `what` is the short subject shown in the caption; it is copied.
// Brackets nest — only the OUTERMOST Begin/End pair touches the caption.  Safe before
// the frame window exists (no-op).
void KiwiLoadProgress_Begin( const char *what );

// One log line.  Ignored unless a bracket is open.  Throttled internally and trimmed to
// a single line's worth of text.
void KiwiLoadProgress_Note( const char *line );

// Set the caption a loader wants the window to END UP with.  Outside a bracket this is a
// plain SetWindowTextA; inside one it is remembered and applied by End(), so a loader that
// renames the frame midway through its own work is not reverted when the bracket closes.
void KiwiLoadProgress_SetTitle( const char *title );

// Close the bracket and settle the caption.
void KiwiLoadProgress_End();

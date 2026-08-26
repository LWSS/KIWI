#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Native-title feedback for synchronous loads. Nothing here pumps or nests an ImGui frame;
// same-thread SetWindowTextA updates the caption directly.

// Open a nested bracket and copy its subject. Only the outermost pair owns the caption;
// calls made before the frame exists are harmless.
void KiwiLoadProgress_Begin( const char *what );

// Show one trimmed, throttled log line while a bracket is open.
void KiwiLoadProgress_Note( const char *line );

// Set the final caption: apply it directly outside a bracket, or defer it to End() inside one.
void KiwiLoadProgress_SetTitle( const char *title );

// Close the bracket and settle the caption.
void KiwiLoadProgress_End();

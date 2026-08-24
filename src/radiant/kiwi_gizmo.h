#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Modal transform manipulators.  The public entry points stay deliberately small:
// kiwi_viewport owns input routing and camwnd owns the single world-draw hook.
// Coordinates are camera-image pixels with a top-left origin.

void KiwiGizmo_Hover( int imgX, int imgY, bool over );
bool KiwiGizmo_MouseDown( int imgX, int imgY );
void KiwiGizmo_Drag( int imgX, int imgY );
void KiwiGizmo_Release();
void KiwiGizmo_Abort();
bool KiwiGizmo_Grabbed();

void KiwiGizmo_DrawWorld();

bool KiwiGizmo_Show();
void KiwiGizmo_SetShow( bool on );

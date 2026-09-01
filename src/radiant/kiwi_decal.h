#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Decals — paint decal patches onto brush faces from the "Decals" dock window.
//
// A decal is an ordinary 3x3 patch (so the .map, the compiler and the game need
// nothing new) laid flat on the picked face, pushed off it along the face normal by
// an offset, textured one-repeat-fit with a decal material, and given a vertex alpha
// for opacity / edge fade.  The window owns the material palette and the placement
// parameters; a selected 3x3 planar patch can be re-sized, rotated, re-layered,
// faded and flipped live from the same window.

void KiwiDecal_Draw();                 // the "Decals" dock window (KIWI_WIN_DECALS)

bool KiwiDecal_IsArmed();
bool KiwiDecal_HandleDown( int imgX, int imgY );
void KiwiDecal_HandleUp();
void KiwiDecal_HandleAbort();
bool KiwiDecal_HandleEscape();
bool KiwiDecal_HandleWheel( float steps, bool shift, bool ctrl );   // Ctrl+wheel size, Shift+wheel rotate

void KiwiDecal_Hover( int imgX, int imgY, bool over );
void KiwiDecal_DrawWorld();

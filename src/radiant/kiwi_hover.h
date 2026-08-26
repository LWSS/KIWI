#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Camera hover adds bounded world accents for one picked item and the active fine
// item without replacing ported selection passes. Hover is light cyan; active is
// a warm accent.

#include "kiwi_pick.h"

// Forward declarations keep qe3.h out of this header.
struct selbrush_t;
struct entity_s;

// Re-pick under the cursor (camera-image coords, TOP-LEFT origin).  Uses the
// current KiwiSel_GetModeMask() so hover granularity always matches what a click
// would select.  No-op when the hover toggle is off.
void KiwiHover_Update( int imgX, int imgY, bool ctrl );

// Cursor left the camera image / a drag started — drop the highlight.
void KiwiHover_Clear();

// The last hover result (invalid when nothing is hovered).
const pick_result_t &KiwiHover_Get();

// True when the hovered target is selected and Ctrl would remove it.  Fine-tool
// marker passes use the same state as the central hover renderer.
bool KiwiHover_RemovePreview();

// The shared selection-preview palette: cyan for add/replace, warm for remove.
void KiwiHover_PreviewColor( bool remove, float outRgb[3] );

// Cam_Draw tail hook: hover outline + active accent.
void KiwiHover_DrawWorld();

// Outliner rows use a separate latch because raycast hover stores only one item.
// Clear runs before each outliner draw and hovered rows republish for one frame.
// Brush pointers are liveness-checked at draw time because deletion can follow
// publication. Entity targets are bounded and degrade to a partial highlight.
// If the outliner follows the camera pass, the plain latch displays next frame.
void KiwiHover_OutlinerClear();
void KiwiHover_OutlinerBrush( selbrush_t *b );          // one row
void KiwiHover_OutlinerEntity( entity_s *e );           // a group / entity row
void KiwiHover_OutlinerCon( int conIndex );             // one construction object
void KiwiHover_OutlinerConGroup( int conGroup );        // a construction folder

// Read by kiwi_construct.cpp's line pass; -1 / 0 mean "nothing".
int  KiwiHover_OutlinerConIndex();
int  KiwiHover_OutlinerConGroupId();

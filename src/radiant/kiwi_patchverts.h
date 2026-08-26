#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Patch-vertex mode uses the modern vertex pick, marquee, and transform paths.
// V remains command-local for a modal pivot; while idle it toggles this mode for
// a selected patch and otherwise falls through to classic 33005 Drag Vertices.
//
// The mode sets SEL_MASK_VERTEX and draws from a latched patch list. Point-only
// selection removes its patch from selected_brushes, so the ported selected-patch
// control-point draw would disappear after the first click. The modern transform
// path supplies gizmo movement, grid snap, exact cancel restore, and per-drag undo.

struct selbrush_t;

// Handles V when active or a patch is selected; false falls through to classic 33005.
bool KiwiPatchVerts_ToggleForSelection();

// Is patch vertex mode live?
bool KiwiPatchVerts_Active();

// Leave safely, restoring the previous pick mask; a no-op when inactive.
void KiwiPatchVerts_Exit();

// Per-frame exit/adoption check, called outside Cam_Draw because full exit may
// relink brush lists. Empty selection stays in mode; mask change, an unowned item,
// or no live patch exits. A picked point on another live patch is adopted.
// Selection walks are generation-gated.
void KiwiPatchVerts_Update();

// Escape, offered from the idle key funnel.  True = the mode took the key.
bool KiwiPatchVerts_HandleEscape();

// Whether `b` is in this mode's draw scope; used by marquee preview.
bool KiwiPatchVerts_OwnsPatch( const selbrush_t *b );

// Cam_Draw tail hook for scoped control lattices and markers; inert when inactive.
void KiwiPatchVerts_DrawWorld();

// Bottom-of-screen status line for the mode, or NULL.  Same contract as a
// command's HudStatus (static storage, read every frame).
const char *KiwiPatchVerts_Status();

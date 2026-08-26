#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Camera focus commands do not mutate map geometry or open an undo record.
//
// "/" mirrors Plasticity's viewport:focus: frame selected brush owners and
// construction objects, or fall back to the visible world without changing view
// direction. Explicit selections remain frameable when hidden.
// KIWI_CMD_FOCUS_SELECTION 34108, modern key "/".

// Returns the AABB "/" would frame, or false when no eligible geometry exists.
bool KiwiFocus_SelectionBounds( float mins[3], float maxs[3] );

// Cheap palette predicate; avoids calculating fallback bounds every frame.
bool KiwiFocus_CanFocus();

// Space mirrors viewport:navigate:selection: aim down -face normal, frame the
// winding AABB, and make that face's construction plane explicit without changing
// projection. Face priority is active selection, another selection, then hover;
// no face clears an explicit plane. Modern Clone moves to Shift+Space.
// KIWI_CMD_VIEW_FACE 34132, modern key Space.
bool KiwiFocus_CanViewFace();      // Palette predicate.

void KiwiFocus_RegisterCommands();
bool KiwiFocus_DispatchInstant( unsigned int cmdId );

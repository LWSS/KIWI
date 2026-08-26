#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Ctrl+1..4 convert typed selection to points, edges, faces, or objects.
// These commands mutate no geometry and open no undo record; successful results
// sync to the legacy selection stores and repaint.
//
// Plasticity's keymap binds all four targets, but its Ctrl+1 command is not
// registered. SelectionConversionStrategy.ts:15-111 supplies the topology map:
// face-to-edge uses the boundary; object-to-edge/face enumerates all geometry;
// edge-to-face selects adjacent faces; edge/face-to-object selects the owner.
//
// KIWI deliberately differs:
// - Ctrl+1 implements Plasticity's unregistered binding as winding vertices or
//   patch controls. Winding references deduplicate in world space at the ported
//   FindPoint/SetupVertexSelection 0.1-unit tolerance.
// - Successful conversions also set KIWI's single-kind picker mask so the next
//   click preserves the result; Plasticity leaves its multi-toggle mode unchanged.
// - Vertices convert to every incident winding face or edge.
// - Patches convert only to points or objects. An edge/face conversion of a
//   patch-only selection is kept because selection changes are not undoable.

// Registers unbound command rows; the modern keymap supplies Ctrl+1..4.
void KiwiSelConv_RegisterCommands();

// Dispatches the four conversion ids; false for anything else.
bool KiwiSelConv_DispatchInstant( unsigned int commandId );

// Enabled for any non-empty selection; unsupported patch-only conversions report
// at run time.
bool KiwiSelConv_CanConvert();

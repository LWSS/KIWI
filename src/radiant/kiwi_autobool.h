#pragma once
// Greedily consolidate the current brush selection in one undo record: a forward
// pairwise fixed point, guarded touching components of at least three, then a reverse
// pairwise fixed point to recover some order-dependent misses. The command is unbound.
//
// Brush_MergeList (csg.cpp:409, 0x47D600) alone decides geometry and accepts only a
// convex result. Surviving faces keep manual CSG Merge materials; the new brush takes
// g_activeLayer_string and the merge-list head's owner (csg.cpp:541, 548-555).
//
// Candidates use the epsilon-padded world-space bounds test from Select_Touching_R
// (select.cpp:1678-1679), because mergeable neighbours abut rather than overlap.
// This is bounds contact, not proof that the face windings touch.
//
// Brush_MergeList marks flipped-equal planes interior without testing winding contact.
// Cluster results therefore must conserve total winding volume. Measurement uses fan
// triangles relative to the cluster centre with a double accumulator; tolerance is
// 0.1% of the input volume or one cubic world unit, whichever is larger. Originals
// stay alive until the guard accepts the result; pairwise attempts are unguarded.
//
// The scalar guard can miss volume-neutral shape errors and rejects valid overlapping
// inputs because their overlap is double-counted. It does not validate materials,
// contents, or layer selection. Unmeasurable cluster inputs/results are rejected.
//
// Other limits: same owning entity only; no patches or fixed-size entity brushes;
// greedy and still order-dependent; one cluster pass with no rejected-subset search.
// See RADIANT_UX_DESIGN.md §58.3.

// Palette predicate: >= 2 usable, same-entity, non-patch brushes selected.
bool KiwiAutoBool_CanExecute();

// The instant-command hook.  Returns false for any id that is not Auto Bool.
bool KiwiAutoBool_DispatchInstant( unsigned int commandId );

// Registration (called from KiwiCmd_RegisterCommands).
void KiwiAutoBool_RegisterCommands();

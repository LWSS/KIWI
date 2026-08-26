#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Trim follows Plasticity's fragment/complement behavior, but computes spans on
// hover instead of maintaining a fragment database. It edits construction curves
// only and remains active until RMB/Enter.
//
// Chains use cumulative world-space arc length. Crossings are 3D closest approaches
// within KCON_ISECT_DIST, matching construction intersection snapping.
//
// LINE/POLYLINE/RECT removal is always bounded first by the hovered stored edge;
// crossings may only narrow it. CIRCLE/ARC tessellation is not user-drawn, so those
// types retain the arc-between-neighboring-crossings rule.
//
// Trimming a parametric object freezes its current tessellation as KCON_POLYLINE;
// the HUD warns before the click. An interior open-chain cut may leave two objects,
// while a closed-chain cut leaves one open complement.
//
// Each click gets its own store-undo snapshot. PrepareTrim must finish before
// KiwiCon_UndoPush: the push also creates a unified-journal ticket, and there is no
// safe discard operation for a failed commit.
//
// The modern keymap binds bare T; classic command registration remains unbound.

class KiwiEditorCommand;

// Palette predicate: at least one trimmable object exists.
bool KiwiTrim_CanTrim();

void               KiwiTrim_RegisterCommands();
KiwiEditorCommand *KiwiTrim_CommandForId( int commandId );

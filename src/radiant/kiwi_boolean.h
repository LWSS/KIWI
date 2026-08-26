#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Q starts with the selected usable brushes as targets. Click, Shift+click, or
// Shift+drag names tools; difference is the default and Q toggles union. W and E
// retain their editor bindings, so this command intentionally has no intersection mode.
//
// Difference is the classic convex brush subtract built over the ported two-half
// splitter; this port has no subtract core of its own:
//
//     pieces = { target }
//     for each tool and each current piece:
//         for each outward tool plane:
//             keep the front half as an output piece
//             carry the back half as the possible intersection
//         discard the final back half (piece ∩ tool)
//
// This computes `target \ (t0 ∪ t1 ∪ …)` without requiring the tools' union to be
// convex. Each surviving piece is convex, and one tool produces at most one piece
// per tool face.
//
// KiwiSplit_DefByPlaneCarve applies §19 to every half that could be landed. Sliver
// fronts are dropped, qualifying refused backs may be carried only as unlanded
// intermediates, and a refused individual cut leaves that piece whole; each is counted.
// A target swallowed completely is left untouched instead of being silently deleted.
//
// All replacement defs remain unlinked until every target carve has been computed.
// The undo record covers selected targets plus the separately picked tools, lands
// every replacement first, then frees the targets and tools so owners never go empty.
//
// Union delegates command 32927 to the classic CSG merge handler, which owns undo
// and rejects patches, fixed-size or differently owned brushes, and non-convex hulls.

class KiwiEditorCommand;

void KiwiBool_RegisterCommands();
KiwiEditorCommand *KiwiBool_CommandForId( int commandId );

// Palette predicate: at least one selected brush can be consumed by CSG.
bool KiwiBool_CanBoolean();

// World-space signed-distance tolerance retained as the normative on-plane epsilon
// for sibling modelling tools.
#define KBOOL_ONPLANE_EPS 0.01f

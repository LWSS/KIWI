#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
#include "kiwi_construct.h"   // KCON_WELD_2D

// Offset Curve (modern O) adds a parallel construction object; the source survives.
// Plasticity then transfers selection to the new curve (OffsetCurveCommand.ts:34).
// It mirrors Plasticity's curve arm and active-plane fallback
// (OffsetCurveCommand.ts:8-34); face and edge inputs are not implemented.
//
// The first selected construction object is used at any selection granularity.
// LINE/POLYLINE/RECT are solved in plane space. CIRCLE/ARC remain parametric:
// only radius changes, preserving centre, plane, and sweep.
//
// Prefer the object's fitted plane. A straight chain has none, so the active
// construction-plane normal supplies sideways after orthogonalisation; a chain
// parallel to that normal is refused.
//
// Closed positive distances go outward according to winding; open positive distances
// go right of travel. Dragging reads the nearest edge's signed side. Distance is a
// delta from the gesture start; Rebase biases the latch to preserve the current value.
//
// Joints are mitered; nearly parallel or 180-degree turns and over-limit miters bevel.
// Refuse self-intersections, winding flips, collapsed radii, and point-count failures.
// Preview never mutates the store; Commit owns the construction-domain undo snapshot.

class KiwiEditorCommand;

// Four is the usual stroke miter limit, beveling corners sharper than about 29 degrees.
#define KOFF_MITER_LIMIT     4.0f
// Match plane-fit slop; smaller gestures would create near-duplicate geometry.
#define KOFF_MIN_DIST        0.5f       // == KCON_PLANE_FIT_DIST, the layer's one slop
// Prevent a circle or arc from collapsing to a point.
#define KOFF_MIN_RADIUS      1.0f
// Fixed plane-space duplicate tolerance shared by construction solvers.
#define KOFF_WELD_2D         KCON_WELD_2D

// The first selected construction object must be offsettable.
bool KiwiOffset_CanOffset();

void KiwiOffset_RegisterCommands();
KiwiEditorCommand *KiwiOffset_CommandForId( int commandId );

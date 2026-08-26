#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Match Face (Z) is KIWI-specific; Plasticity has no equivalent face-plane verb.
// With one selected brush face, click a target face to copy its plane. The same
// brush is allowed, but the source face itself is not a target.
//
// Preserve the source's outward sense by flipping an opposed target normal.
// Plane-point order (c + u*r, c, c + v*r) is required because Face_MakePlane uses
// cross(p0 - p1, p2 - p1); face-radius spacing avoids a weak plane definition.
//
// Validity must run before undo: apply an unrecorded trial, restore the baseline,
// then open one undo bracket and repeat with the ported texture-lock ordering:
// restore texdef/planepts -> Face_MakePlane -> save -> write -> reproject.
// Restoring first prevents texture lock from reprojecting an accumulated texdef.
// If V5 finds a duplicate half-space, gate without the source plus V8 closure,
// then remove the source; no surviving plane moves, so no texture reproject runs.
//
// With no face and one patch object selected, project each control point along the
// patch's Newell normal: P' = P + n * (D - P·N) / (n·N).
// Sum every control-net cell because fillet caps can have zero spanning edges; a
// planar Bézier control net keeps the entire surface planar.
// Refuse residuals over 0.1 world units (the editor point-dedup tolerance), Newell
// length at or below 1e-3 (2*area is round-off), |n·N| below 0.1 (>10x projection
// amplification), coordinates outside KVALID_MAX_COORD, or travel over
// KVALID_MAX_SPAN.
// Scratch projection makes a trial unnecessary; only after all gates pass does
// one undo record write the grid, rebuild the patch, and mark the map modified.
// Only the selected cap moves; adjacent arc/chamfer geometry does not follow.
//
// Z is the modern binding; classic CameraAngleDown moved to Shift+Alt+Z
// (mainfrm.cpp:1032; full audit in kiwi_keymap.h).

class KiwiEditorCommand;

void KiwiMatch_RegisterCommands();
KiwiEditorCommand *KiwiMatch_CommandForId( int commandId );

// Palette predicate: one face, or no faces and one patch object.
bool KiwiMatch_CanMatch();

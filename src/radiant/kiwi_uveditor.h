#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// UV editor; portions of this file and kiwi_uveditor.cpp are derived from TrenchBroom
// (https://github.com/TrenchBroom/TrenchBroom): UvViewHelper, UvOffsetTool, UvScaleTool,
// UvRotateTool, UvShearTool, UvOriginTool, UvCameraTool, and UvEditor.
//
// Copyright (C) 2010 Kristian Duske
//
// TrenchBroom is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// TrenchBroom is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// KIWI Radiant is likewise GPL; retain this notice and the derivation note above.

// Coordinate model
// ----------------
// This is a true 2D UV canvas, not TrenchBroom's world-space face-plane camera. Its axes
// are S and T in texture-repeat units: 1.0 is one tile of the active face's material.
// Face_MoveTexture already produces repeat-space ST (brush.cpp:2618), so no conversion is
// performed. +S points right and +T points down, matching the image V axis.
//
// The texture and grid stay fixed while displayed wireframes move. Thus the grabbed point
// follows the cursor, while the texture appears to move in the opposite direction on the
// fixed 3D face. View pan and wheel zoom are the only operations that move the background.
// TrenchBroom constants expressed as `px / zoom` become plain pixel constants here.

// UV writes
// ---------
// Rotate, scale, skew, and the toolbar transforms produce a displayed-space 2x3 affine A:
//   face: texMat = Face_MoveTexture(start texdef, normal)
//         row0' = A00*row0 + A01*row1; m3' = A00*m3 + A01*m7 + A02
//         row1' = A10*row0 + A11*row1; m7' = A10*m3 + A11*m7 + A12
//         texturevecs_02(texMat', normal, dist) -> size/shift/rotate/crossterm
//   patch: st' = A*st in ctrl[i][j].texCoord.st[2*layer + {0,1}], the slots written by
//          Patch_ShiftTexture (pmesh.cpp:3232-3238).
// Face_MoveTexture's rows remain in the base-axis plane under these combinations, so the
// texturevecs_02 decomposition is well posed (texturevecs.cpp:132/136).
//
// texturevecs_02 returns positive size[0]; transforming a negative-size[0] face can change
// its numeric parameterization (180-degree rotate plus re-signed size[1]) without moving
// pixels. Its internal sub_4AAD00 snapping is also not idempotent across repeated
// decompose/recompose cycles. Every live frame therefore rebuilds from the gesture-start
// texdef snapshot, never from the previous frame's output.
//
// Pure translation bypasses decomposition. Face_MoveTexture uses m3=-shift[0]/sx and
// m7=-shift[1]/sy, so `shift = shift0 - deltaST*size0`, including its zero-size -> 128
// substitution (texturevecs.cpp:108-109, 140-141). Brush_ShiftTexture instead adds to
// shift and therefore decreases ST: that command drags the texture, while this canvas
// drags the UV footprint. The opposite signs are intentional.

// Display layout and coordinate conversion
// ----------------------------------------
// Chained faces carry a display-only rigid transform D_f (rotation, optionally reflection).
// The active face is the identity anchor. No D_f reaches a texdef except through:
//   A_true = D_f^-1 * A_display * D_f
// A displayed translation d similarly becomes the true-frame vector M_f^-1*d. Reflection
// therefore reverses a face's true-frame rotation while preserving its on-screen motion,
// and the conjugated pivot remains the pivot seen by the user.
//
// The chain is a BFS over shared world edges welded into KUVE_WELD_EPS buckets. A neighbor
// is rotated to the displayed shared-edge direction, pinned at one endpoint, and reflected
// across that edge when its centroid would overlap the placed face. Different UV edge
// lengths are deliberately not scaled; unreachable faces and heavily overlapping patches
// are shelved. Bucket-boundary misses also fall back to shelving. The build is capped by
// KUVE_CHAIN_MAX_FACES/KUVE_CHAIN_MAX_EDGES.
//
// 2026-09-21: a shared edge need not be a WHOLE edge. Collinear edges overlapping by a
// world unit or more (T-junctions between separate brushes) fold along the overlap, after
// the welded pairs. Two faces of one plane keep their world sides instead of always
// landing opposite. A face still unreached but coplanar with a placed one is positioned
// through that face's world->display map (rigid: one corner pinned, longest diagonal
// turned to its mapped direction). Only what is neither adjacent nor coplanar is shelved.
//
// A chain layout is latched on gathered row order, active anchor, and the Chain toggle—not
// texdef/ST values or the local target set. Otherwise a completed edit would immediately
// re-solve and rearrange the displayed shapes. "Re-fold" requests a new solve; until then,
// heavy edits may create display overlaps. Turning Chain off shows true ST positions.

// Interaction contracts
// ---------------------
// The target set is local to this window and never changes the 3D selection. A 3D selection
// change re-seeds it from explicit selected faces; whole-brush-only selections target all.
// Targets draw gold, non-targets retain their face/patch hue at KUVE_DIM_ALPHA. The active
// face separately supplies the background material and chain anchor.
//
// One displayed-space box encloses the target set. Corners scale both axes (Shift makes the
// scale uniform); edge midpoints scale one axis; the opposite corner/side stays fixed.
// The corner's outside annulus rotates, Alt+edge midpoint skews, and the body moves. The
// pivot defaults to the target bbox center until dragged, then stays user-placed until the
// 3D selection changes. Rotate, skew, Rot 90, and Flip use that pivot. With +T down:
//   CW [[0,-1],[1,0]], CCW [[0,1],[-1,0]], Flip U [[-1,0],[0,1]], Flip V [[1,0],[0,-1]].
//
// Clicking a shape targets and drags it; repeated presses cycle topmost-first through
// stacked shapes. Shift-click adds and Ctrl-click removes. A press on one of several
// targets defers collapse until release so a drag can move the whole set. Empty-space drag
// marquees by displayed-outline intersection; an empty click or Escape restores all.
// Grid lines are guides/snap targets, never handles.
//
// Ctrl engages snapping during transforms. Candidate points and edges are captured once at
// the press in displayed space: target outlines are the moving set; grid/whole texels and
// other displayed shapes are destinations. Live reads would chase the ST values being
// rewritten. Move/scale/pivot snaps use an 8-pixel absorb; point/axis absorbs show a marker,
// while rotate reports its edge-angle lock. The pivot also considers both point sets and the
// target centroid. The TB issue #1350 near-axis guard refuses singular shear handles
// (UvShearTool.cpp:284-289).

// Integration contracts
// ---------------------
// ImGui's D3D9 backend forces CLAMP addressing, so the background is drawn as capped unit
// quads rather than one wrapped image (imgui_impl_dx9.cpp:149-150). The editor only borrows
// an engine-owned MAPTYPE_2D texture and owns no D3D resource or reset registration.
//
// Patch brushes contribute their control grid, not six derived box-face texdefs;
// Radiant_PatchGetTexdef reconstructs those from the grid (brush.cpp:2231). Numeric fields
// remain face-only. Patch Reset uses Patch_NaturalizeSelected's own undo bracket, so a mixed
// Reset is two undo steps (pmesh.cpp:2755-2757).
//
// Each gesture opens one undo record at its first mutation. Face-selected brushes are not
// in `selected_brushes`, so UndoOpen covers every gathered brush before writing. Cancel
// restores the exact gesture-start snapshot and cancels the record. A parked face gesture
// must be ended before the first UV write or its MaterialDef baseline can overwrite the edit.

// Tuning. All screen dimensions are pixels; this canvas does not use TB's `/zoom` units.
#define KUVE_ZOOM_MIN          4.0f     // canvas pixels per texture repeat, floor
#define KUVE_ZOOM_MAX       4096.0f     // ...and ceiling
#define KUVE_ZOOM_STEP         1.1f     // wheel factor (TB UvCameraTool.cpp:87/95)
#define KUVE_SNAP_PX           8.0f     // TB snapDelta absorb radius (UvViewHelper.cpp:206)
#define KUVE_ORIGIN_PICK_RAD   8.0f     // pivot disc pick radius; intentionally > draw radius
#define KUVE_ORIGIN_RADIUS_PX  5.0f     // TB UvOriginTool::OriginHandleRadius (:309)
#define KUVE_ORIGIN_CROSS_PX   7.0f     // arm length of the S/T direction cross
#define KUVE_SHEAR_MIN_PX      6.0f     // TB issue #1350 guard (UvShearTool.cpp:286)
#define KUVE_MAX_TILES         1600     // background unit-quad cap
#define KUVE_MAX_FACES         8192     // gather ceilings: a UV canvas is not a bulk op
#define KUVE_MAX_PATCHES        512
#define KUVE_CHAIN_MAX_FACES    256     // past this the chain is not built at all
#define KUVE_CHAIN_MAX_EDGES   4096     // ...and neither is the O(E^2) weld
#define KUVE_WELD_EPS          0.25f    // world-unit bucket the edge weld quantises to
#define KUVE_SHELF_GAP         0.15f    // repeats between shelved (non-adjacent) shapes
#define KUVE_DIM_ALPHA         0.40f    // non-target shapes
#define KUVE_SHAPE_EDGE_PX     6.0f     // edge-proximity slack on the shape hit-test
#define KUVE_BOX_INFLATE_PX    6.0f     // box outset from the target set's displayed bbox
#define KUVE_BOX_MIN_PX       24.0f     // below this the box carries NO handles: they would
                                        // cover it and eat the body drag (BoxHasHandles)
#define KUVE_HANDLE_PX         4.5f     // half-extent of a handle SQUARE, as DRAWN
#define KUVE_HANDLE_PICK_PX    7.0f     // ...and as PICKED (Chebyshev half-extent)
#define KUVE_ROTATE_ZONE_PX   14.0f     // annulus beyond a corner square, outside the box
#define KUVE_CYCLE_PX          4.0f     // repeat-press radius that advances the stack cycle
#define KUVE_MARQUEE_MIN_PX    3.0f     // shorter than this and a drag counts as a CLICK
#define KUVE_SNAP_MAX_PTS      1024     // cap on each gesture-start snap candidate set

// Owns its ImGui Begin/End and early-outs on the window-open flag.
void KiwiUvEd_Draw();

// Registers the palette row; KiwiWindows_DispatchInstant owns the window toggle.
void KiwiUvEd_RegisterCommands();

// Scope is latched once per frame while the window is visible and focused/hovered, or while
// a gesture is live (so leaving the window during a drag does not flicker overlays). Cam_Draw
// cannot query this window's ImGui state directly and may observe at most one frame of lag.
// Only selection overlays for gathered shapes are suppressed; selection state and unrelated
// overlays remain intact. Selected brushes still render their textured pass with neutral
// material color rather than disappearing. faceIndex < 0 asks about the whole brush/patch;
// faceIndex >= 0 asks about one face.
struct brush_t;                          // qe3.h:30 — this header takes no includes
bool KiwiUvEd_InScope();
bool KiwiUvEd_OverlaySuppressed( const brush_t *def, int faceIndex );

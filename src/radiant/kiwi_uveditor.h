#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_uveditor.h — KIWI-UX (ROUND BD): THE UV EDITOR WINDOW.
// ═════════════════════════════════════════════════════════════════════════════════════
//
//  Copyright (C) 2010 Kristian Duske
//
//  Portions of this file and of kiwi_uveditor.cpp are derived from TrenchBroom
//  (https://github.com/TrenchBroom/TrenchBroom) — specifically UvViewHelper.cpp,
//  UvOffsetTool.cpp, UvScaleTool.cpp, UvRotateTool.cpp, UvShearTool.cpp,
//  UvOriginTool.cpp, UvCameraTool.cpp and UvEditor.cpp.
//
//  TrenchBroom is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  TrenchBroom is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  KIWI Radiant is likewise GPL, so the adaptation is licensed; this notice and the
//  derivation note above are the condition of that.
// ═════════════════════════════════════════════════════════════════════════════════════
//
// USER DIRECTIVE (verbatim intent): a new UV system like TrenchBroom's UV editor, which
// shows EVERY SELECTED FACE as a UV wireframe over the tiled texture, offers advanced
// controls to scale / skew (crossterm) / rotate / offset so the texture shows up properly
// across all of them, and SUPPORTS PATCHES AND CURVES AS WELL AS BRUSH FACES.
//
// ── D-BD-A — THIS IS A 2D UV CANVAS, NOT TRENCHBROOM'S WORLD-SPACE VIEW ─────────────
// TrenchBroom's UV view is an ORTHOGRAPHIC CAMERA parked on the face plane looking down
// −normal (UvViewHelper.cpp:301-314).  The FACE is fixed on screen and the TEXTURE moves
// under it; every handle radius is `const / zoom` because one world unit is `zoom` pixels.
//
// KIWI's window is a true 2D canvas in UV space instead, for one reason that decides the
// whole design: the directive asks for EVERY SELECTED FACE at once, and there is no single
// face plane to park a camera on.  UV space is the one frame all of them share.  So:
//
//   * the axes are (S, T) in TEXTURE-REPEAT units — 1.0 == one tile of the ACTIVE face's
//     material.  KIWI's own ST are already in repeat units (Face_MoveTexture builds the
//     matrix and brush.cpp:2618 emits `st = row·p + m3`), so nothing is converted;
//   * the TEXTURE and its grid are FIXED in the canvas; the WIREFRAMES move.  That is the
//     Blender / Quake UV-editor metaphor and it is the inverse of TrenchBroom's, where the
//     face is fixed.  Consequence, stated once and true of every gesture below: **the
//     thing under the cursor when the drag starts stays under the cursor**, and the
//     texture therefore appears to move the OPPOSITE way on the 3D face.  TB's UvOffsetTool
//     writes `offset − snapped` (UvOffsetTool.cpp:105) precisely to get the same felt
//     behaviour out of the opposite frame; ours is `+delta` for the same reason.
//   * every TB constant that was `px / zoom` in world units is a PLAIN PIXEL constant here
//     (SURVEY_TRENCHBROOM §7 item 4): 5 px grid pick, 5 px origin line/circle, 8 px snap
//     absorb, 32 px ring / 5 px width, 32 px axes, 2.5 px active outline.
//
// ── D-BD-B — HOW A GESTURE REACHES A texdef: ONE 2×3 AFFINE, THEN THE BINARY'S OWN INVERSE
// Rotate / Scale / Skew all produce a 2×3 affine `A` on UV space and apply it identically:
//
//   FACES   texMat = Face_MoveTexture(gesture-start texdef, face normal)   [8 floats]
//           row0' = A00·row0 + A01·row1 ,  m3' = A00·m3 + A01·m7 + A02
//           row1' = A10·row0 + A11·row1 ,  m7' = A10·m3 + A11·m7 + A12
//           texturevecs_02(texMat', normal, dist)  →  size / shift / rotate / crossterm
//
//   PATCHES st' = A · st, per control point (ctrl[w][h].texCoord.st[2·layer(+1)] — the
//           same slot Patch_ShiftTexture writes, pmesh.cpp:3232).
//
// Both rows of a Face_MoveTexture matrix lie in the plane spanned by the base axes (the
// `r` component is written 0, texturevecs.cpp:132/136), and a linear combination of two
// such rows stays in it — so texturevecs_02's Phase-B orthogonalize is a no-op on our
// output and its decomposition is well posed.  It is a 4-dof factorisation (rotate, two
// sizes, crossterm) of a 2×2, i.e. general: KIWI's crossterm gives us REAL SKEW, which is
// exactly what TrenchBroom cannot do on Quake-style faces (ParaxialUvCoordSystem::shear is
// an empty function, ParaxialUvCoordSystem.cpp:721-725).
//
// TWO CONSEQUENCES THAT ARE NOT BUGS:
//   1. texturevecs_02 always returns a POSITIVE size[0] (Phase E is `1/|row0|`).  A face
//      that was carrying a NEGATIVE size[0] (a "Flip U") therefore comes back out of the
//      first rotate/scale/skew frame as positive size[0] with rotate turned 180° and
//      size[1] re-signed — a DIFFERENT PARAMETERISATION OF THE SAME MAPPING.  The pixels do
//      not move; the numeric readout jumps once.  Flip U itself does not go through the
//      affine path (it is `size[0] = −size[0]`), so it stays exact.
//   2. sub_4AAD00 snapping inside texturevecs_02 (size/shift gran 8 eps 0.001; rotate gran
//      4 eps 0.005; crossterm gran 1000 eps 1e-5) cleans values that are already within a
//      thousandth of a grid step.  It is near-identity, but it is NOT idempotent under
//      repeated decompose→recompose, which is why every live frame rebuilds from the
//      GESTURE-START texdef snapshot and never from the previous frame's output.
//
// ── D-BD-C — OFFSET DOES NOT GO THROUGH THE AFFINE, AND THAT IS EXACT ──────────────
// Face_MoveTexture writes `m3 = −shift[0]/sx` and `m7 = −shift[1]/sy` (texturevecs.cpp:140-
// 141) and NOTHING ELSE reads shift.  So a pure UV translation by (dS,dT) is exactly
// `shift[i] = shift0[i] − d[i]·size0[i]` (with the zero-size→128 substitution the binary
// itself makes at texturevecs.cpp:108-109).  No decompose, no snapping, bit-stable.
//
// SIGN NOTE, because two UI paths now shift textures and they read OPPOSITE:
// `Brush_ShiftTexture` ADDS to shift (select.cpp:3118), which DECREASES S — the modal
// "Texture Shift" command drags THE TEXTURE.  This window drags THE UV FOOTPRINT, so a
// drag to the right RAISES S and therefore LOWERS shift.  Both are correct in their own
// metaphor; neither is reachable from the other's surface.
//
// ── D-BD-D — THE BACKGROUND TILES BY HAND, BECAUSE THE IMGUI BACKEND CLAMPS ────────
// The plan was one AddImage whose uv0/uv1 span the visible UV rect, relying on D3D9's
// default WRAP addressing.  MEASURED, NOT ASSUMED: `deps/imgui/backends/imgui_impl_dx9.cpp`
// lines 149-150 set `D3DSAMP_ADDRESSU/V = D3DTADDRESS_CLAMP` on every ImGui draw, and the
// only way to change that mid-list is an ImDrawList CALLBACK, which SPLITS THE DRAW LIST
// (kiwi_entbrowser.cpp:429-434's finding) — the one thing not to do in a tiled fill.
// So the background is a bounded GRID of unit-quad AddImage calls, capped at
// KUVE_MAX_TILES; past the cap the canvas falls back to a flat plate and says so.
//
// ── D-BD-E — THIS WINDOW OWNS NO D3D OBJECT, SO IT HAS NO RESET STORY ──────────────
// The Sky tab needs a MANAGED per-face copy because a sky colorMap is a CUBEMAP
// (kiwi_skybox.cpp:301-346).  An ORDINARY material's colorMap is MAPTYPE_2D, and the
// round-AZ 2D arm (kiwi_skybox.cpp:469) hands ImGui the ENGINE'S OWN `img->texture.map`
// and stores nothing.  This file only ever takes that arm: a colormap that is not
// MAPTYPE_2D draws no background at all (and the toolbar says why).  Nothing is allocated,
// so there is no kiwiTexCache_t and no RTT_ReleaseForReset registration to forget.
//
// ── D-BD-F — PATCH BRUSHES CONTRIBUTE THEIR PATCH, NOT THEIR SIX BOX FACES ─────────
// `Brush_ShiftTexture` shifts BOTH a patch brush's face texdefs and its control STs
// (select.cpp:3095-3140).  This window draws and transforms the CONTROL GRID only.  The
// face texdefs of a patch brush are a derived cache — Radiant_PatchGetTexdef (brush.cpp:
// 2231) RECOVERS one from the grid whenever anything needs it — and drawing six bounding
// box outlines per patch would bury the grid the user came here to edit.
//
// ── D-BD-G — UNDO: ONE BRACKET PER GESTURE, OPENED AT THE FIRST MUTATION ───────────
// `KiwiCmd_UndoBegin` clones `selected_brushes`; face-selected brushes are NOT on that
// list (kiwi_selection.h DESIGN NOTE 2), so each is covered by `KiwiCmd_UndoCoverBrush`
// BEFORE the first write — the kiwi_uv.cpp:160 pattern.  Live frames then mutate directly;
// the mouse release calls `KiwiCmd_UndoCommit`.  Esc (and app focus loss) restores the
// gesture-start SNAPSHOT — which is exact, unlike kiwi_uv.cpp's inverse-delta leg — and
// then `KiwiCmd_UndoCancel`, which is the same belt-and-braces shape KiwiTexShiftCommand
// ::Cancel uses (kiwi_uv.cpp:345-353).
//
// ── D-BD-H — WHAT v1 DOES NOT DO ──────────────────────────────────────────────────
// The numeric fields in the toolbar are FACE-ONLY (a patch has no texdef to type into —
// it carries per-control-point ST directly, qe3.h:71-81).  The five gesture verbs DO work
// on patches, which is the part the directive asked for.  "Reset" on a selection holding
// patches re-naturalises them through `Patch_NaturalizeSelected`, which opens its OWN undo
// bracket (pmesh.cpp:2755-2757) — so a mixed selection's Reset is TWO Ctrl+Z, and the
// console says so.  Listed in RADIANT_KNOWN_ISSUES.
// ═════════════════════════════════════════════════════════════════════════════════════
//
// ═════════════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BG) — the user-feedback batch.  D-BG-A..E amend the above.
// ═════════════════════════════════════════════════════════════════════════════════════
//
// ── D-BG-A — "PAPER ANGELS": A DISPLAY-ONLY RIGID TRANSFORM PER FACE ───────────────
// USER REPORT, verbatim: "when selecting multiple faces, they should show up as different
// shapes in the UV editor.  Since they would stack on top of each other — make it so they
// are chained together like paper angels."
//
// Faces that share a material and a projection have IDENTICAL STs, so BD drew N outlines
// on top of each other and the user saw one.  Each gathered face f now carries a DISPLAY
// TRANSFORM D_f — a 2×3 affine whose linear part is orthonormal (a rotation, optionally
// composed with a reflection: det ±1) — applied ONLY when drawing that face's outline and
// when hit-testing it.  The active face is the anchor and its D is IDENTITY, always.
// True STs, texdefs and every snap candidate are untouched: D_f exists nowhere in the
// write path except as the conjugation below.
//
// CHAIN CONSTRUCTION (BFS from the active face).  Two selected faces are NEIGHBOURS when
// their windings share a WORLD EDGE — both endpoints landing in the same KUVE_WELD_EPS
// world-unit bucket, in either order (a QUANTISED weld, so the whole thing stays O(P log P)
// instead of O(P²); a vertex sitting within a thousandth of a bucket boundary can be missed,
// and the face is then shelved instead of folded, which is a cosmetic fallback, not a
// wrong answer).  For an anchor a already placed and a neighbour b:
//     A0 = D_a·st_a(E0),  A1 = D_a·st_a(E1)      the shared edge as DISPLAYED by a
//     b0 =      st_b(E0),  b1 =      st_b(E1)      the same world edge in b's OWN ST frame
//     R  = the rotation taking dir(b1−b0) onto dir(A1−A0)
//     D_b = T(A0)·R·T(−b0), then, IF b's centroid would land on the same side of the edge
//           as a's, composed with the REFLECTION across the edge line — the paper-doll
//           fold: the neighbour opens out onto the FAR side instead of overlapping.
// The two edges can have different LENGTHS (the faces may carry different texture scales);
// a rigid transform cannot fix that, so the fold pins the shared endpoint E0 and the edge
// DIRECTION and lets the far end run long or short.  That is stated rather than hidden.
// Faces (and patches, see below) that reach nothing already placed are SHELVED: translated
// into a row to the right of the occupied bounding box with a KUVE_SHELF_GAP gap.
//
// GESTURE CORRECTNESS UNDER D — THE CONJUGATION.  A canvas gesture produces ONE 2×3 affine
// A in DISPLAYED space (D-BD-B).  A displayed point is x_disp = D_f·x_true, and the gesture
// must move the DISPLAYED outline, so the true-frame affine that reaches the texdef is
//     A_true = D_f⁻¹ · A · D_f            (identity for the active face — nothing changes)
// Offsets are the vector case of the same rule: D=(M,t) ⇒ D⁻¹·T(d)·D = T(M⁻¹d).
// For a face whose D carries a REFLECTION the algebra flips the sign for free: with M a
// reflection (M = Mᵀ = M⁻¹, det M = −1) and A a rotation R(θ) about the handle,
//     M⁻¹·R(θ)·M = M·R(θ)·M = R(−θ)
// so the folded-over face turns the opposite way in its own frame and the SAME way as its
// neighbour on screen, which is what "chained like paper angels" has to mean.  The pivot
// conjugates with it: D⁻¹·T(o)·L·T(−o)·D = T(D⁻¹o)·(M⁻¹LM)·T(−D⁻¹o), i.e. every face turns
// about the ORIGIN HANDLE AS THE USER SEES IT.
//
// LIMIT, stated once: the snap candidates (vertex, edge-angle, edge-slope) are read from
// the ACTIVE face only — D = identity there, so the snaps are exact where they are read
// and are simply not offered for the folded neighbours.  RADIANT_KNOWN_ISSUES carries it.
//
// The chain is rebuilt only when the gathered set or its texdefs change AND no gesture is
// live: a layout that re-solved mid-drag would creep under the cursor.  Past
// KUVE_CHAIN_MAX_FACES faces it is not built at all (every D stays identity) — a fold-out
// of a thousand faces is not a diagram, and the O(E²) weld is not free.
//
// ── D-BG-B — PER-SHAPE TARGETING ("individual shape UV'ing") ──────────────────────
// A canvas click INSIDE a shape's displayed outline makes that shape the UV-EDIT TARGET
// SET; Shift+click toggles membership; a click on empty canvas restores "all".  The 3D
// selection is NEVER touched — this is a UV-editor-local sub-selection over the gathered
// rows, which is exactly why D-BG-A matters: you cannot click one of five stacked outlines.
// Non-target shapes draw at KUVE_DIM_ALPHA.
//
// ONE DELIBERATE DEVIATION from the brief's ordering: the shape hit-test runs AFTER the
// ring / origin / grid rungs, not before them.  Those three are GLOBAL canvas objects (the
// brief says so itself), and a scale grab lands on a grid line that is usually outside
// every outline — resolving targeting first would silently reset the target set on every
// scale.  So: a handle press leaves the target set alone; only a press that would have been
// an OFFSET resolves targeting, and it still both targets and starts the gesture in ONE
// click.
//
// ── D-BG-C — ROT 90 AND FLIP PIVOT ON THE ORIGIN HANDLE ───────────────────────────
// USER REPORT: "When flipping it 90 degrees, the shape flies around really far off the
// centre point."  BD's toolbar verbs wrote the texdef FIELDS directly (rotate ±90,
// size negate).  Both of those pivot on the TEXTURE-SPACE ORIGIN (0,0) — an ST footprint
// sitting at s≈37 swings 74 repeats away.  They are now the SAME affine path the gestures
// use, pivoted at the origin handle and conjugated per face:
//     Rot 90 CW  A = T(o)·[[0,−1],[1,0]]·T(−o)      (+T is DOWN, so this is CW ON SCREEN)
//     Rot 90 CCW A = T(o)·[[0, 1],[−1,0]]·T(−o)
//     Flip U     A = T(o)·[[−1,0],[0,1]]·T(−o)      (mirror across the origin's V axis)
//     Flip V     A = T(o)·[[1,0],[0,−1]]·T(−o)
// and the DEFAULT origin moved from TB's ST-bbox MIN corner to the active face's ST-bbox
// CENTRE — "off the centre point" is the user saying they expect a centre pivot.  The
// handle is still draggable for anyone who wants a corner.
//
// ── D-BG-D — "THE SHAPES MOVE, NOT THE BACKGROUND" ────────────────────────────────
// USER DIRECTIVE, verbatim: "its really unintuitive, for most operations the shapes should
// be moving, not the background."  This is D-BD-A's metaphor promoted to a LAW, and every
// operation was audited against it (the table is in the round-BG report).  The invariant,
// per gesture frame: the drawn outline of an affected shape moves exactly with the gesture
// — grab a point on the shape and that point stays under the cursor — and the tiled texture
// and its grid never move except during an explicit view pan / wheel zoom.  Offset, rotate,
// scale and skew already conformed (they all apply their affine to ST, D-BD-B).  Rot 90 and
// Flip did not, on BOTH counts: the face arm pivoted on the texture origin (D-BG-C) and the
// patch arm's "CW" matrix was [[0,1],[−1,0]], which turns a screen-right vector to
// screen-UP, i.e. COUNTER-clockwise.  Both fixed by the single affine above.
// The consequence the law implies and which is not a bug: the texture appears to rotate the
// OPPOSITE way on the 3D face, because a texdef rotation moves the TEXTURE under a fixed
// face and this canvas fixes the texture instead (D-BD-A).
//
// ── D-BG-E — HOVER ANNOUNCES THE HANDLE, AND THE MOUSE CURSOR SAYS WHICH ──────────
// USER REPORT: "The lines of the UV aren't easily clickable/draggable."  Every pick
// tolerance below is widened, and the ladder was factored into ONE probe function that both
// the press and the hover run, so what lights up under the cursor is by construction what
// the press will grab.  The probe also picks the ImGui mouse cursor — which doubles as the
// standing regression check for the round-BG invisible-cursor fix: the cursor is recomputed
// from scratch every frame and is never latched.
// (ROUND BI: the GRID-LINE half of this rung is gone with the rest of the grid-line tools;
// the probe and its one-answer-for-hover-and-press contract are exactly what BI keeps.)
// ═════════════════════════════════════════════════════════════════════════════════════
//
// ═════════════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BI) — THE CONTROLS COME OFF THE GRID AND ONTO THE SHAPES.
//  D-BI-A..F supersede the TrenchBroom grid-line interaction model above.  Everything
//  BELOW the gesture layer — D-BD-B/C (the affine and its inverse), D-BD-D..G, D-BG-A
//  (the fold-out and its conjugation) — is untouched and still governs.
// ═════════════════════════════════════════════════════════════════════════════════════
//
// ── D-BI-A — WHAT WAS REJECTED, AND WHY IT DESERVED IT ────────────────────────────
// USER VERDICT, verbatim intent: *"UV editor is still hard to use.  Take the controls off
// the grid lines.  Keep them on the shapes.  Allow selection and manipulation of each
// sub-shape in a grouped selection.  Again, I want to move the shapes, not the texture."*
//
// TrenchBroom hangs SCALE and SHEAR on the UV GRID LINES (UvScaleTool.cpp / UvShearTool.cpp
// pick `pickUvGrid`), which works there because TB's view holds ONE face and the grid is the
// only global object in it.  BD copied that; BG widened its pick bands; the model still
// failed, and the arithmetic of the failure is the point:
//
//   the grid band was min(10 px, 0.35 × stripe spacing) on EACH side of EVERY line, so at
//   the auto-fit zoom of a multi-face selection **up to 70% of the canvas answered SCALE**.
//   The origin's two INFINITE axis lines added two full-canvas 16 px-wide bands, and BG had
//   just moved the pivot default to the middle of the shapes, so those bands ran straight
//   through them.  The rotate ring added a 40 px disc in the same place.  And the shape
//   hit-test was the LAST rung of the ladder (D-BG-B's deliberate deviation), reached only
//   when all three of those missed.
//
// So on the exact selection the user was complaining about — a group, folded out, zoomed to
// fit — most presses inside a shape resolved to a canvas-global grid/origin/ring handle and
// never reached per-shape targeting at all.  THAT is why BG's sub-shape targeting "still"
// underdelivered: not a missing gather (whole-brush faces and patches were always gathered
// and always hit-tested — Gather() steps 1-2, PickShape's two loops), but a priority ladder
// that put the global furniture above the shapes and let the furniture cover the canvas.
// The second half of the same gap: PickShape ranked with a STRICT `<` over a rank that is
// 0.0 for every shape containing the point, so of N stacked shapes only the lowest-indexed
// one was EVER reachable — no cycling, no way down the stack.
//
// ── D-BI-B — THE TRANSFORM BOX: ONE PER TARGET SET, ON THE SHAPES ─────────────────
// The target set (D-BG-B, now first-class) carries ONE axis-aligned box: the canvas-space
// bbox of every targeted shape's DISPLAYED outline, inflated by KUVE_BOX_INFLATE_PX.  On it:
//   * 4 CORNER squares  — scale both axes about the pivot; Shift = uniform (aspect-locked,
//     the least-squares scalar ((h1−o)·(h0−o))/|h0−o|², so the drag's along-diagonal
//     component decides the factor);
//   * 4 EDGE-MID squares — single-axis scale about the pivot;
//   * the ROTATE ANNULUS — KUVE_ROTATE_ZONE_PX beyond a corner square AND OUTSIDE the box,
//     so it can never steal a body press;  Alt + an EDGE-MID square = SKEW along that edge's
//     axis (the crossterm route for faces, the ST shear for patches — BG's math, new grab
//     site, and TB issue #1350's near-axis guard still refuses the degenerate case);
//   * the BODY — a press inside a targeted shape's outline MOVES the target set.
// Ctrl is "disable snapping" and NOTHING ELSE now: BD's "Ctrl anywhere rotates" is gone
// (it made every Ctrl+drag meant to suppress a snap into a rotation), and so is the 32 px
// rotate RING around the pivot, whose whole job the corner annulus now does without sitting
// on top of the shapes.
//
// ── D-BI-C — SUB-SHAPE SELECTION IS FIRST-CLASS, INCLUDING DOWN A STACK ───────────
//   * A press on a shape targets it and begins the drag in the SAME press (BG's rule, kept).
//   * REPEATED presses within KUVE_CYCLE_PX of the previous one CYCLE through every shape
//     under the cursor, topmost first (patches draw over faces, later rows over earlier, so
//     "topmost" is reverse draw order).  This is the answer to D-BI-A's strict-`<` finding;
//     it is the standard alt-click-through of any 2D editor and it needs no modifier.
//   * Shift+press toggles membership and starts nothing.
//   * A press on a shape that is ALREADY one of several targets moves the WHOLE set — a
//     multi-selection may not collapse just because you grabbed it by one member; the NEXT
//     press at the same spot collapses to that shape (cycle index resets to 0 for exactly
//     that case, so the first repeat gives the topmost shape and not the second one).
//   * MARQUEE: an LMB drag from empty canvas rubber-bands, and every shape whose displayed
//     outline INTERSECTS the band (vertex in band, edge crossing it, or band inside the
//     outline) becomes the target set; Shift extends.  This REPLACES BD's "drag anywhere =
//     offset everything" fallback, which is the single easiest way to move geometry you did
//     not mean to touch.
//   * A drag that starts in EMPTY SPACE INSIDE the box moves the set when a sub-selection is
//     live, and marquees when it is not.  That is the exact reconciliation of the brief's
//     two sentences ("inside the box but on no handle" drags; "empty canvas" marquees): with
//     a real target set the box is an object you can grab anywhere; with no sub-selection
//     there is no such object and empty space is empty canvas.  A zero-distance press in
//     either of those places restores "all", which is the way back from any sub-selection.
//
// ── D-BI-D — THE PIVOT IS THE TARGET SET'S CENTRE UNTIL YOU SAY OTHERWISE ─────────
// The origin handle keeps its circle and gains a small cross; its two INFINITE axis lines
// are deleted (grid-line-era furniture that ate two full-canvas bands of picks — D-BI-A).
// It now defaults to the TARGET SET's displayed bbox centre and is re-derived every frame
// no gesture is live, so it follows the target set — UNTIL the user drags it, which latches
// `user placed` for the lifetime of the 3D selection (the latch clears on the same
// Sel_Generation change that re-frames the view).  Rot 90 / Flip U/V pivot on this handle
// (D-BG-C) and therefore now turn the target set about ITS OWN centre by default.
//
// ── D-BI-E — THE GRID IS FURNITURE ────────────────────────────────────────────────
// `pickUvGrid` and everything that hung off it are DELETED, not disabled: the grid-line
// hit test, its hover highlight, the scale-handle-on-a-stripe rendering, KUVE_GRID_PICK_PX
// and KUVE_GRID_PICK_FRAC.  The subdivision spinners stay — they still decide the drawn
// grid and the snapping quantum.  Grid intersections remain a SNAP TARGET for move / scale
// (with whole texels and the vertices of the shapes that are NOT being dragged), which is
// the BG candidate set retargeted to the new gestures: 8 px absorb, Ctrl disables.
//
// ── D-BI-F — THE SNAP CANDIDATES ARE CAPTURED AT THE PRESS, IN DISPLAYED SPACE ────
// BG read every snap candidate from the ACTIVE face's own ST frame.  The active face need
// not be in the target set at all now, so the candidates are the TARGET SET's displayed
// outline points and edges (what is being dragged) measured against the grid, whole texels
// and the OTHER shapes' displayed vertices (what it is being dragged onto).  Both sets are
// captured ONCE at the press — during the drag the STs they would be read from are exactly
// what the drag is rewriting — and both are capped at KUVE_SNAP_MAX_PTS.
// ═════════════════════════════════════════════════════════════════════════════════════
//
// ═════════════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BJ) — the second user-feedback batch on the BI model.
//  D-BJ-D..G amend §82.2 above; the gesture ALGEBRA (D-BD-B/C), the fold-out and its
//  conjugation (D-BG-A), the undo bracket (D-BD-G) and the BH handshake are untouched.
// ═════════════════════════════════════════════════════════════════════════════════════
//
// ── D-BJ-D — THE TARGET SET STARTS AS THE 3D FACE PICK, AND EVERY TARGET IS GOLD ──
// USER REPORTS, verbatim: *"When faces are shift-selected in 3D, they also need to be
// shift-selected in the UV editor by default."* and *"Everything selected in the UV
// editor should be yellow (not just 1 chunk)."*
//
// BI's default target set was "everything gathered".  For a WHOLE-BRUSH selection that
// is right — the user picked solids and every face of them is fair game — but for a
// selection that carries EXPLICIT faces it meant the first canvas gesture moved rows the
// user had not picked in 3D.  Gather() now derives the default from `g_SelectedFaces`
// when it is non-empty: exactly those faces are targeted, patches are not, and a
// whole-brush-only selection keeps "all".  The derivation is a DEFAULT, not a per-frame
// override — it is gated on a SELECTION-CHANGE STAMP read against `s_selGen`, so a
// canvas-local sub-selection the user built with clicks or a marquee survives the
// every-frame re-gather, and any real 3D selection change (including the shift-click
// that adds the fourth face) re-derives.  One deliberate exception: when the explicit
// faces already ARE every shape on the canvas the state is left as plain "all", because
// `s_targetAll` is read as a MODE by the empty-space arm of the probe (D-BI-C) and a
// redundant sub-selection would change what a press on empty canvas does.
//
// The GOLD is the target treatment now.  BI gave `KUVE_COL_ACTIVE` to the active face
// alone and used alpha for targeting, so a five-face target set drew one gold outline and
// four dim blue ones — "not just 1 chunk" is exactly that.  Targeted shapes (faces AND
// patches) draw gold at 2.5 px; non-targets keep their family colour at KUVE_DIM_ALPHA.
// The ACTIVE face keeps no outline of its own: it survives only as (a) the material that
// paints the background and (b) the fold anchor, both of which are named in the readout.
//
// ── D-BJ-E — SCALE ANCHORS THE OPPOSITE SIDE / CORNER ────────────────────────────
// USER REPORT, verbatim: *"When dragging the sides, dont expand both sides, just expand
// the side that is being dragged."*  BI's corner and edge-mid scale were PIVOT-anchored,
// and the pivot defaults to the box centre (D-BI-D), so a right-edge drag moved the left
// edge out by the same amount.  The anchor is now the handle ACROSS THE BOX —
// NW<->SE, NE<->SW, N<->S, E<->W — captured at the press from the DEFLATED box beside
// `s_gHandle*`, and the factor is `(cursor − anchor) / (handle − anchor)` per armed axis.
// Only the dragged side moves.  Shift on a corner is still the aspect-locked least-squares
// scalar, now about that anchor.
//
// THE PIVOT KEEPS ITS JOBS: rotate, skew, Rot 90 / Flip U / Flip V and the numeric ops.
// No modifier was added for pivot-anchored scaling: Alt is already SKEW on the edge mids
// (D-BI-B), so an Alt rule would have applied to corners only and a half-rule in a
// direct-manipulation surface is worse than no rule.  With the pivot dragged onto the
// opposite corner the two behaviours coincide anyway.
//
// ── D-BJ-F — THE SNAP SAYS SO ────────────────────────────────────────────────────
// USER REPORT, verbatim: *"The snapping isn't very intuitive."*  The MECHANISM was
// already D-BI-F's — what snaps is the thing being held (every displayed vertex of the
// target set on a move, nearest-wins; the dragged HANDLE per armed axis on a scale; the
// edge-angle set on a rotate), 8 px absorb, Ctrl disables — and it was invisible.  An
// 8 px absorb with no feedback reads as the cursor being ignored.
//
// So an engaged absorb now leaves a MARK, and it is the established KIWI snap-accent
// language transcribed to 2D: the tick-plus-dot-plus-separated-ring of kiwi_snap.cpp's
// EmitDotAndRing, drawn bright instead of the 3D pass's near-black because this canvas is
// dark.  The TICK runs along the line that was locked (a locked S draws a vertical tick),
// so "it stuck to THIS line" reads without a legend; the pivot's 2D point snap marks both
// axes and therefore reads as a cross.  The status line names what was hit — grid, vertex,
// grid + vertex, or edge angle.  The TEXEL rounding is deliberately NOT marked: it is
// sub-pixel at any normal zoom and marking it would light the accent permanently.
// One correctness fix rode along: the PIVOT's own snap candidates were read from the
// ACTIVE face's own ST frame, which since BI need not be targeted and, with the chain on,
// is not drawn where its STs say — so a pivot dropped "on that corner" landed elsewhere.
// It reads the same captured DISPLAYED-space sets as everything else now, plus the target
// set's centroid.
//
// ── D-BJ-G — THE FOLD LAYOUT IS LATCHED, NOT RE-SOLVED ──────────────────────────
// USER REPORT, verbatim: *"When rotating them all as a group, dont scramble them, keep
// them in their orientation."*
//
// THE MECHANISM was the rebuild trigger.  BG keyed the fold-out on a signature that
// hashed every gathered face's size / shift / rotate / crossterm and every patch's ST —
// the very values a gesture writes.  Mid-drag it was frozen (the `s_gesture == UVG_NONE`
// clause), but the instant the mouse came up the signature had moved and the whole layout
// was re-solved FROM THE NEW STs: a fresh BFS, fresh FoldRigid rotations, possibly
// different mirror decisions (the SideOf test flips as the ensemble turns) and a shelf
// laid out from a moved occupied box.  A group rotate reshuffled on release — and so did
// every other write: dragging one shape out of the chain snapped it back, and a numeric
// field edit re-solved the lot.
//
// A DISPLAY LAYOUT IS NOT A FUNCTION OF THE TEXDEFS.  D_f is solved once per (gathered
// rows and their order, the active face as fold ANCHOR, the Chain toggle) — which is what
// `ChainStructSig` already hashed — and never again because a texdef changed.  The
// conjugation then does the rest for free: a displayed outline is D_f·st_f and a gesture
// writes A_true = D_f⁻¹·A·D_f, so the displayed outline moves by exactly the canvas affine
// A whatever D_f is, and a group rotate rigidly turns the whole displayed ensemble about
// the pivot.  `ChainSignature` and `HashF` are DELETED, not disabled.
//
// THE TARGET SET IS NOT PART OF THE KEY.  BuildChain never reads it — the anchor is the
// active face — so keying on it would add re-solve points, i.e. scramble points, without
// changing the answer.  Re-solving is a TOOLBAR BUTTON ("Re-fold") instead, because it is
// a decision now and not a side effect.  AUTO-FRAME is unaffected: it runs off
// `s_zoomValid`, cleared by the Sel_Generation change — that is the VIEW, not the layout.
// KNOWN CONSEQUENCE, stated: after heavy editing the latched arrangement can drift into
// overlaps that a fresh solve would have avoided.  That is what Re-fold is for, and it is
// strictly better than the arrangement moving on its own.
// ═════════════════════════════════════════════════════════════════════════════════════

// ── tuning (one place, the kiwi_transform.cpp / kiwi_uv.h convention) ───────────────
// Every one of these is a PIXEL constant; see D-BD-A for why TB's `/zoom` is gone.
#define KUVE_ZOOM_MIN          4.0f     // canvas pixels per texture repeat, floor
#define KUVE_ZOOM_MAX       4096.0f     // ...and ceiling
#define KUVE_ZOOM_STEP         1.1f     // wheel factor (TB UvCameraTool.cpp:87/95)
// ROUND BG (item 3a) widened TB's pick bands; ROUND BI deleted the two that were attached
// to grid lines (KUVE_GRID_PICK_PX / _FRAC) and the rotate ring (KUVE_RING_*) with the
// gestures that used them — D-BI-A/B.  The TB value each survivor started from is kept in
// the comment: these are QoL adaptations of TB's numbers, not TB's numbers.
#define KUVE_SNAP_PX           8.0f     // TB snapDelta absorb radius (UvViewHelper.cpp:206)
#define KUVE_ORIGIN_PICK_RAD   8.0f     // BG: 5 -> 7, BI: 7 -> 8.  The disc is the ONLY
                                        // pivot pick now (the axis LINES are gone).
#define KUVE_ORIGIN_RADIUS_PX  5.0f     // TB UvOriginTool::OriginHandleRadius (:309) — DRAW
#define KUVE_ORIGIN_CROSS_PX   7.0f     // BI: arm length of the small cross that replaced
                                        // TB's two infinite axis lines
#define KUVE_SHEAR_MIN_PX      6.0f     // TB issue #1350 guard (UvShearTool.cpp:286)
#define KUVE_MAX_TILES         1600     // background quad cap — see D-BD-D
#define KUVE_MAX_FACES         8192     // gather ceilings: a UV canvas is not a bulk op
#define KUVE_MAX_PATCHES        512
// ── ROUND BG: the fold-out (D-BG-A) and the targeting dim (D-BG-B) ─────────────────
#define KUVE_CHAIN_MAX_FACES    256     // past this the chain is not built at all
#define KUVE_CHAIN_MAX_EDGES   4096     // ...and neither is the O(E^2) weld
#define KUVE_WELD_EPS          0.25f    // world-unit bucket the edge weld quantises to
#define KUVE_SHELF_GAP         0.15f    // repeats between shelved (non-adjacent) shapes
#define KUVE_DIM_ALPHA         0.40f    // non-target shapes (item 6)
#define KUVE_SHAPE_EDGE_PX     6.0f     // edge-proximity slack on the shape hit-test
// ── ROUND BI: the transform box (D-BI-B) and the selection gestures (D-BI-C) ───────
#define KUVE_BOX_INFLATE_PX    6.0f     // box outset from the target set's displayed bbox
#define KUVE_BOX_MIN_PX       24.0f     // below this the box carries NO handles: they would
                                        // cover it and eat the body drag (BoxHasHandles)
#define KUVE_HANDLE_PX         4.5f     // half-extent of a handle SQUARE, as DRAWN
#define KUVE_HANDLE_PICK_PX    7.0f     // ...and as PICKED (Chebyshev half-extent)
#define KUVE_ROTATE_ZONE_PX   14.0f     // annulus beyond a corner square, outside the box
#define KUVE_CYCLE_PX          4.0f     // repeat-press radius that advances the stack cycle
#define KUVE_MARQUEE_MIN_PX    3.0f     // shorter than this and a drag counts as a CLICK
#define KUVE_SNAP_MAX_PTS      1024     // cap on each gesture-start snap candidate set

// The dock window.  Begins/Ends itself and early-outs on its §9 flag, exactly like
// KiwiOutliner_Draw / KiwiEntBrowser_Draw / KiwiSky_Draw (imgui_shell.cpp:1484-1495).
void KiwiUvEd_Draw();

// §15 palette + §3 registration.  The window TOGGLE itself dispatches through
// KiwiWindows_DispatchInstant (the §9 table owns the flag) — this only names the row, the
// same split kiwi_skybox.cpp:1381-1390 makes.
void KiwiUvEd_RegisterCommands();

// ═════════════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BN, ITEM 3) — "IN SCOPE", AND WHAT THE 3D VIEW STOPS DRAWING
// ═════════════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: *"UV Editor — after a manipulation is done in the UV editor,
// hide the wiremesh rendering in the 3D cam so it's easier to see the texture being moved
// across the shape.  Restore it whenever the UV editor is no longer in 'scope'."*
//
// ── WHAT "IN SCOPE" IS, EXACTLY ─────────────────────────────────────────────────────
// The UV editor window is OPEN and VISIBLE and (focused OR hovered), or a UV gesture is
// live.  Three conditions, one OR, because all three mean the same thing to the user: "I
// am working in the UV editor right now."  A live gesture is included on its own terms —
// a drag that leaves the window's rect (which a scale or a rotate routinely does) must not
// make the overlays flicker back on mid-drag.
//
// IT IS A LATCH, computed ONCE per frame inside KiwiUvEd_Draw and read afterwards.  The
// camera image is rendered from Cam_Draw, which does not run inside the UV window's ImGui
// scope, so the ImGui focus/hover predicates are not askable from there.  Cost of the
// latch: at most ONE FRAME of latency on entering or leaving scope — invisible, and the
// alternative (asking ImGui from the render pass) is not answerable at all.
//
// ── WHAT IS SUPPRESSED, AND WHAT IS DELIBERATELY NOT ────────────────────────────────
// ONLY the SELECTION OVERLAYS, and ONLY for shapes the UV editor actually gathered:
//   * the ported selected-face FILL (camwnd.cpp Cam_DrawSelectedFaceFill) and the classic
//     magenta face outline (Cam_DrawSelectedFaces),
//   * the KIWI face fills and the selected-item wire accents (kiwi_hover.cpp),
//   * the tech-29 WHITE OUTLINE pass, for a gathered WHOLE brush / patch,
//   * the selected-brush RED TINT — the brush still draws (that pass is the only thing
//     that draws a selected brush's textured surface at all; skipping it would make the
//     brush VANISH, not un-highlight it), it simply draws at neutral MATERIAL_COLOR.
// The SELECTION ITSELF is untouched — nothing here writes selection state — and every
// overlay returns on the first frame after scope ends.  A brush or face that is NOT in the
// UV gather keeps all of its overlays, in scope or out.
//
// `faceIndex` < 0 asks the WHOLE-object question ("is this brush/patch gathered"), which is
// what the brush-level passes need; >= 0 asks about one face.
struct brush_t;                          // qe3.h:30 — this header takes no includes
bool KiwiUvEd_InScope();
bool KiwiUvEd_OverlaySuppressed( const brush_t *def, int faceIndex );

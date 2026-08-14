#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_matchface.h — SHAKEOUT G: MATCH FACE (Z).
//
// USER DIRECTIVE, verbatim: "Add a 'match face' feature (Z).  First you select a
// face, then press Z, then press the Face you want to match.  Doing this copies
// the exact angle and extension of that face."
//
// ── PLASTICITY HAS NO SUCH VERB (checked, not assumed) ──────────────────────
// There is no MatchFace / match-face / align-face command anywhere in
// plasticity/src.  The whole direct-face family lives in
// plasticity/src/commands/modifyface/ModifyFaceFactory.ts:71-130 — Remove,
// Create, Action, Fillet, Supple, Purify, Merger, United — and
// ModifyFaceCommand.ts:22-26 dispatches only between Refillet and Offset; two of
// the factories (MergerFaceCommand, ModifyFaceCommand.ts:139/:141) are literally
// empty command bodies.  The nearest thing in spirit is the face-neighbourhood
// walker `FaceCollector` (OffsetFaceFactory.ts:170-233), which is a draft-angle
// helper and not a plane copy.
//
// So this verb is KIWI'S OWN, defined by the directive rather than ported.  What
// makes it well-posed here is that a classic brush IS a set of planes: "copy the
// exact angle and extension" of a face has one unambiguous meaning — make the
// source face's PLANE the target face's plane — and §19 already knows how to
// decide whether the resulting solid survives.
//
// ── THE OPERATION ───────────────────────────────────────────────────────────
// With EXACTLY ONE face selected, Z starts a modal pick.  The next face CLICK
// (pick mask FACE) is the target; the source's three planepts are rewritten onto
// the target's plane and the brush is rebuilt and gated.
//
//   * THE SAME BRUSH IS ALLOWED.  Only the SOURCE FACE ITSELF is excluded, not
//     its brush: matching one face of a brush to another face of the same brush
//     is a legitimate (if usually self-destructive) request, and §19 is what
//     decides whether the result is a solid — not a pre-emptive rule here.
//
//   * THE SOURCE KEEPS ITS OUTWARD SENSE.  If the target's normal points the
//     opposite way, the copied plane is flipped before it is written:
//         n = targetNormal * sign( dot( targetNormal, sourceNormal ) )
//     Without that, matching a floor to a ceiling would turn the brush inside
//     out every time, which is a "swap two words" bug that §19 would only ever
//     report as "planes crossed".
//
//   * THE THREE PLANEPTS are built on the TARGET's plane, well spread: the
//     target winding's centroid c, plus c + u*r and c + v*r where (u, v, n) is a
//     right-handed in-plane basis and r is the target winding's own radius.  The
//     order is (c + u*r, c, c + v*r) so that Face_MakePlane's normal —
//     cross(p0 - p1, p2 - p1) = (u x v) * r² — comes out as n exactly.  Three
//     points a whole face-radius apart is the difference between a plane and a
//     numerically worthless one (the same reason kiwi_extrude.h picks spread cap
//     points).
//
//   * TEXTURE LOCK, the §20 bracket verbatim: restore the baseline texdef AND
//     planepts, Face_MakePlane, Ed_FaceTexLockSave, write the new planepts,
//     Ed_FaceTexLockReproject.  Exactly what face push/pull does per frame
//     (kiwi_transform.cpp ApplyFaces), for the same reason: without restoring the
//     baseline texdef first the lock would reproject an already-reprojected
//     texdef.
//
// ── WHY THE APPLY RUNS TWICE (the TRIAL) ────────────────────────────────────
// §19's rule is that a validity rejection must not touch the undo stack at all
// (kiwi_validity.h "WHY A BASELINE AND NOT JUST UNDO"), and the undo bracket's
// rule is that Undo_AddBrush must run BEFORE the first mutation.  Those two pull
// in opposite directions for a ONE-SHOT op: opening the bracket first means a
// rejection has to be unwound with KiwiCmd_UndoCancel, whose Undo_Undo
// deselects everything and re-links brush CLONES — i.e. it would free the very
// node this command is holding, mid-gesture, for a pick the user may simply want
// to retry.
//
// So the plane is applied as a TRIAL first, with NO bracket and no texture lock:
// rebuild, gate, and roll straight back to the baseline either way.  Only when
// the trial passes does the bracket open (cloning the untouched original) and the
// real, texture-locked apply run.  A rejected pick therefore costs one rebuild
// and changes nothing anywhere; a good pick costs two and leaves ONE undo record.
//
// ── ROUND AA, ITEM 4: MATCHING A CHAMFER ONTO ITS NEIGHBOUR ─────────────────
// USER REPORT, verbatim: "(see pic) match face should support this operation.
// You still can't delete a chamfer that was made on a brush."  The picture is a
// skewed chamfer face with a neighbouring face outlined as the match target.
//
// It was refused, and NOT for any reason to do with the source face being skewed
// or non-axial — there is no axiality assumption in this verb anywhere.  A
// chamfer face that takes its neighbour's plane BECOMES that neighbour's plane,
// and §19's V5 ("duplicate plane", kiwi_validity.cpp:280) rejects a brush that
// carries the same half-space twice.  V5 is right and stays: such a brush is
// ambiguous to the compiler.  What was wrong is that the TRIAL gated the
// intermediate brush instead of the one the user asked for.
//
// A duplicated half-space clips nothing its twin did not already clip, so the
// requested result is that same geometry with the now-redundant face DROPPED —
// which is an ordinary solid, and is "delete the chamfer" reached from the other
// side.  So the trial now:
//   * asks whether the copied plane coincides with another face's (the V5
//     predicate, at the V5 thresholds);
//   * if it does, gates the brush MINUS the source face
//     (KiwiValid_CheckBrushIgnoringFace) and adds V8 (KiwiValid_BrushCloses),
//     because dropping a half-space is exactly the edit that can open a solid;
//   * and on the apply, removes the SOURCE face — never the twin, which was in
//     the brush before this gesture and is not this verb's to delete.
// Every surviving face keeps its planepts and all four material layers: the
// removal is Brush_RemoveFace's memmove of whole face_t records, and no
// surviving PLANE moves, so there is no texdef to re-project.
//
// ── ROUND AN (deferred): Z ON A CURVE — PUSHING A FILLET CAP ONTO A FACE ────
// USER DIRECTIVE, verbatim: "the cap needs to be adjustable, I would use the (Z)
// match face command on the curve itself."
//
// The bevel/fillet tool lands three patches per edge (kiwi_patchfillet.cpp): the
// arc, and the two flat END CAPS that seal it (LandCaps).  A cap is a lens whose
// outline is the arc and its own projection onto the chamfer plane, and it is
// where the fillet MEETS whatever the edge runs into — so it is exactly the piece
// a mapper needs to be able to slide.  Nothing could: the caps land at commit and
// the only edit afterwards was dragging their control points by hand.
//
// ── WHAT THE VERB MEANS ON A PATCH ──────────────────────────────────────────
// With NO face selected and exactly ONE patch selected as an object, Z starts the
// same modal pick.  The clicked face's PLANE is then the target, and every control
// point P of the patch moves to
//
//     P' = P + n * t ,   t = ( planeDist - P.planeNormal ) / ( n.planeNormal )
//
// where n is THE PATCH'S OWN NORMAL.  That direction is the whole design:
//   * ALONG THE PATCH NORMAL, the projection is a pure TRANSLATION OF THE PLANE
//     the patch lives in.  Every point travels the same signed distance along n
//     (t varies only with P.planeNormal, and all P share one plane), so the cap
//     arrives on the target plane with its SHAPE, its spacing and its texturing
//     unchanged — it moved, it did not shear.  That is what "adjustable" has to
//     mean for a cap: the same cap, somewhere else.
//   * Along the TARGET's normal instead, the cap would flatten onto the face and
//     stop being a cap.  Along a screen or world axis it would shear.  Neither is
//     the operation the directive describes.
//   * THE SIGN OF n IS IRRELEVANT.  Flip n and the numerator's plane constant
//     flips with the denominator, so t is unchanged; there is no outward sense to
//     preserve here, unlike the face arm.
//
// ── THE PLANE FIT: NEWELL OVER THE CONTROL NET'S CELLS ──────────────────────
// The obvious two-edge cross product is degenerate on the very patch this feature
// exists for: a cap's arc is TANGENT to the chamfer plane at both ends, so at
// column 0 and column w-1 the profile point and its projection are the SAME POINT
// (kiwi_patchfillet.cpp:1676-1679 states this and dodges it by measuring at the
// middle column), which makes one spanning edge the zero vector.  Newell's method
// sums a signed area contribution over every edge of every cell of the grid, so
// no single degenerate edge decides anything, and for a planar net the sum is
// exactly 2*Area*n — the true normal, not a fit.  Cells are walked in one
// consistent (col,row) parametric order so contributions add rather than cancel.
//
// IT IS THE CONTROL NET THAT IS TESTED, NOT THE TESSELLATION, and that is the
// point: a bezier surface lies in the convex hull of its control points, so a
// planar control net IS a flat patch — projecting the control points is exactly
// projecting the surface, with no sampling and no error term.
//
// ── THE THREE REFUSALS, WITH THEIR NUMBERS ─────────────────────────────────
//   * NOT PLANAR — max |signed distance| of a control point from the net's own
//     centroid plane exceeds KMATCH_PLANAR_EPS = 0.1 world units.  0.1 is the
//     tolerance this tree already uses to call two points identical (the ported
//     FindPoint dedup tolerance, kiwi_patchfillet.cpp:74); it is ~40x above the
//     float round-off a 4096-unit patch accumulates, so a genuinely flat cap is
//     never refused for arithmetic reasons; and it is under the finest grid step,
//     so a cap bent by one quantum is refused out loud instead of being silently
//     flattened onto a plane it was never on.  A CURVED patch — the fillet ARC
//     itself — fails this by design: there is no single translation that puts a
//     curved surface on a plane, so approximating one would be inventing an
//     answer to a question with none.
//   * DEGENERATE NORMAL — the Newell sum's length (2*Area) is under
//     KMATCH_MIN_AREA2 = 1e-3, i.e. the net encloses under 5e-4 square units.
//     Below that the direction is round-off and everything downstream is noise.
//   * EDGE-ON TO THE TARGET — |n.planeNormal| under KMATCH_MIN_COS = 0.1.  That
//     denominator IS the amplification: at 0.1 a point one unit off the plane
//     slides ten units to reach it, and at zero there is no solution at all.  10x
//     is the outer edge of the result still meaning "the cap moved onto that
//     face"; past it one grazing pick flings the cap out of the level.
// Plus §19's own map bound on the RESULT (KVALID_MAX_COORD per axis and
// KVALID_MAX_SPAN of travel), because the cosine gate bounds amplification but
// not distance and a target plane a map away is still a legal plane.
//
// ── NO TRIAL, AND WHY THAT IS NOT AN INCONSISTENCY ─────────────────────────
// The face arm mutates to find out (see above): a copied plane re-clips the whole
// solid and only Brush_BuildWindings can say whether a brush survives.  A patch is
// not clipped against anything — it IS its control points — so every refusal above
// is answerable on a scratch copy of the grid with nothing touched.  So the patch
// arm solves all points into scratch, gates the RESULT, and opens the bracket only
// once it is going to commit.  Same promise ("a rejected pick changes nothing and
// touches no undo record"), reached the short way.
//
// The apply is then the ported control-point-edit sequence, verbatim from the one
// other place in the UX layer that moves control points (kiwi_transform.cpp's
// ApplyVerts): write the grid, ONE Patch_Rebuild( p, 1 ) for the whole net — the
// bounds/Brush_RebuildBrush/curveDef/version tail Patch_UpdateSelected_0 runs —
// then MarkMapModified.  ONE undo record: KiwiCmd_UndoBegin's head clones every
// brush on selected_brushes (which a patch OBJECT selection is on), UndoCoverBrush
// covers it again for free (Undo_AddBrush early-outs on Undo_BrushInUndo,
// undo.cpp:502), and the framework closes the bracket after Commit().
//
// ── HONEST LIMIT ────────────────────────────────────────────────────────────
// THIS MOVES THE CAP AND ONLY THE CAP.  The arc patch beside it and the chamfer
// face under it do not follow, so pushing a cap far from its arc opens a gap
// rather than stretching the fillet.  That is the operation as asked for — "the
// cap needs to be adjustable" — and it is the right one for the case it is for
// (seating a cap on the wall the filleted edge runs into), but it is not a
// re-solve of the fillet.
//
// ── KEYS ────────────────────────────────────────────────────────────────────
// Z in the modern profile.  Bare Z was CameraAngleDown 33062 (mainfrm.cpp:1032),
// one of the classic tank-camera keys; the full audit and its displacement to
// Shift+Alt+Z are in kiwi_keymap.h.
// ─────────────────────────────────────────────────────────────────────────────

class KiwiEditorCommand;

void KiwiMatch_RegisterCommands();
KiwiEditorCommand *KiwiMatch_CommandForId( int commandId );

// §3 palette predicate: exactly one FACE is selected.
bool KiwiMatch_CanMatch();

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_join.h — SHAKEOUT G: JOIN (J), the CONTEXT verb.
//
// USER DIRECTIVE, verbatim: "Add a 'join' feature (J).  This works on faces and
// lines primarily.  2 Faces can be joined if they have the exact same angle (If
// it makes sense logically).  This is for simplifying and rejoining things to be
// extruded again.  Lines can join to form advanced lines that makeup full shapes
// like squares."
//
// ── WHAT PLASTICITY'S `j` ACTUALLY IS (checked, not assumed) ────────────────
// CURVES ONLY.  `j` -> `command:join-curves` (src/startup/default-keymap.ts:258)
// -> JoinCurvesCommand.ts -> JoinCurvesFactory.ts:22-34, which refuses fewer than
// two curves (`if (models.length < 2) throw new Error("not enough curves");`) and
// chains them with `c3d.ActionCurve3D.CreateContours(models, 10)`.
//
// FACES CANNOT BE JOINED THERE AT ALL.  `MergerFaceFactory` and
// `UnitedFaceFactory` exist (ModifyFaceFactory.ts:124-130) but the command that
// would drive the first is an EMPTY BODY —
// `export class MergerFaceCommand extends Command { async execute(): Promise<void> { } }`
// (ModifyFaceCommand.ts:141) — and the second has no reference outside its own
// definition.  Solid-level joining in Plasticity is a boolean union, not a face
// verb.
//
// So the LINE arm ports a real Plasticity verb and the FACE arm is KIWI's own
// reading of the directive.  In a classic PLANE-DEFINED editor that reading has an
// exact and already-ported meaning: two brushes that meet across a shared plane
// are ONE convex solid if their union is convex, and `CSG_Merge` (csg.cpp:572,
// 0x47DA40) is the ported core that decides exactly that — Brush_MergeList's
// Phase 1 rejects any outer-face pair whose planes are concave and its Phase 2
// keeps every outer face.  "Simplifying and rejoining things to be extruded
// again" IS a CSG merge.
//
// ── THE THREE ARMS ──────────────────────────────────────────────────────────
//   (a) exactly TWO selected FACE items, on DIFFERENT brushes, whose planes are
//       THE SAME INFINITE PLANE  ->  merge the two owning brushes.
//   (b) a construction-line selection (and no brush faces)  ->  the shakeout-F
//       JoinLines core, KiwiConSel_Join().
//   (c) anything else  ->  a console message naming both arms.
//
// COPLANARITY IS TESTED AT §19'S OWN TOLERANCES, and it is deliberately the
// SAME-INFINITE-PLANE test rather than the same-normal one:
//
//     |dot(n1, n2)| > KVALID_PLANE_DOT   and   |d1 - sign(dot) * d2| < KVALID_PLANE_DIST
//
// Two brushes stacked against each other share a plane whose outward normals point
// AT each other — the "flipped equal" case Brush_MergeList itself calls an INNER
// face (`Plane_Equal(p2, p1, 1)` negates its first argument, csg.cpp's note).
// Requiring dot > 0 would reject the one arrangement the verb exists for, so the
// magnitude is what is tested and the constant is matched with the sign.
//
// ── HOW THE MERGE IS DRIVEN (no new CSG math, and no new undo shape) ────────
// CSG_Merge's contract is "merge everything on `selected_brushes`" — it takes no
// arguments (csg.cpp:572).  So this file SELECTS exactly the two owning brushes
// through the ported funnels (Select_Deselect then Select_Brush, select.cpp:884)
// and then runs the CLASSIC command id:
//
//     Radiant_ExecCommand( 32927 )   ->  mainfrm.cpp Cmd_OnSelectionCsgmerge
//
// which is already the undo-wrapped merge (Undo_ClearRedo / Undo_GeneralStart(
// "CSG merge" ) / Undo_AddBrushList / CSG_Merge / Undo_EndBrushList / Undo_End,
// mainfrm.cpp:2314-2321) plus the KiwiCsg_NoteBefore/NoteAfter console report.
// That is the same rule kiwi_csg.h already states for the §24 CSG workflow —
// there is exactly ONE handler and ONE undo record per operation, and this verb
// is a new way to REACH it, not a second copy of it.  A refusal (non-convex hull,
// different entities, a patch) is CSG_Merge's own, printed by CSG_Merge, with the
// originals put straight back on the selection by its own failure path.
//
// ── KEYS ────────────────────────────────────────────────────────────────────
// Bare J in the modern profile.  Shakeout F deliberately did NOT claim it
// (kiwi_keymap.h: "displacing ToggleOutlineDraw to buy a chord Ctrl+J gives away
// free is a bad trade") and put Join Lines on Ctrl+J.  The directive names the
// bare key, so shakeout G claims it and displaces ToggleOutlineDraw 33103 with the
// house two-step; CTRL+J STAYS BOUND to KIWI_CMD_CONSTRUCT_JOIN as the lines-only
// alias, so nothing that was reachable stops being reachable and the muscle memory
// shakeout F taught still works.  Full audit in kiwi_keymap.h.
// ─────────────────────────────────────────────────────────────────────────────

void KiwiJoin_RegisterCommands();
bool KiwiJoin_DispatchInstant( unsigned int commandId );

// §3 palette predicate: one of the two real arms is available.
bool KiwiJoin_CanJoin();

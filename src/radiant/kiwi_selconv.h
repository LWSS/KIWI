#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_selconv.h — shakeout D: SELECTION CONVERSION, Ctrl+1..Ctrl+4.
//
// USER DIRECTIVE: "in plasticity there is a feature where when you have a solid
// selected, you can use Ctrl+2 to convert the selection to edges, Ctrl+3 convert
// it to faces, that doesn't work here."
//
// ── WHAT PLASTICITY ACTUALLY DOES (read, not guessed) ───────────────────────
// From the LGPLv3 tree at F:\gitshit\plasticity:
//
//   src/startup/default-keymap.ts:247-250   the bindings, in the
//       `body:not([gizmo])` context — note they sit right under the PLAIN
//       1..4 at :240-243, which SET the selection MODE:
//           "ctrl-1": "selection:convert:control-point"
//           "ctrl-2": "selection:convert:edge"
//           "ctrl-3": "selection:convert:face"
//           "ctrl-4": "selection:convert:solid"
//   src/selection/CommandRegistrar.ts:30-32  only THREE of the four are
//       registered — edge / face / solid.  `selection:convert:control-point` is
//       bound to a command that does not exist, so Ctrl+1 is DEAD in Plasticity.
//   src/selection/SelectionConversionStrategy.ts:15-111  the semantics:
//       convert(to) switches on the target mode and, for each selected item of
//       the OTHER kinds, adds the converted items and REMOVES the source:
//         to CurveEdge :  face2edge  (:97  GetOuterEdges of the face)
//                         solid2edge (:89  every edge of the solid)
//         to Face      :  edge2face  (:64  the edge's GetFacePlus / GetFaceMinus
//                                     — i.e. the two faces adjacent to it)
//                         solid2face (:56  every face of the solid)
//         to Solid     :  edge2solid (:50) / face2solid (:44) — both add
//                         `view.parentItem`, the owning solid.
//   src/selection/ChangeSelectionExecutor.ts:140  onConvert is a straight
//       pass-through; the conversion does NOT touch the selection MODE MASK.
//
// ── HOW KIWI'S MAPPING DIFFERS, AND WHY ─────────────────────────────────────
//  1. Ctrl+1 IS IMPLEMENTED here (Plasticity's is a dead binding).  A brush has
//     no "control point" notion, so it converts to the winding VERTICES —
//     deduped at 0.1 units, the same tolerance the ported FindPoint /
//     SetupVertexSelection dedup and this layer's box select already use, so one
//     world corner shared by three faces becomes ONE point item and not three
//     (kiwi_selection.h DESIGN NOTE 3).
//  2. THE MODE MASK IS SET TOO.  Plasticity keeps mode and conversion separate
//     because its modes are a multi-select toggle set; KIWI's §11 mask is a
//     single radio-style filter driven by the 1..5 chips, and converting a
//     selection to edges while the picker is still in Object mode would mean the
//     very next click threw the result away.  So Ctrl+N also does what plain N
//     does.  This is the one deliberate divergence, and it is the useful one.
//  3. Vertices participate in BOTH directions.  Plasticity has no
//     control-point→anything conversion at all; here a vertex converts to the
//     faces that contain it (the natural counterpart of the edge→face rule) and
//     to the winding edges that touch it.
//  4. Patches convert to POINTS (their control net) and to OBJECTS only.  A
//     patch has no plane faces and no winding edges in this port, so Ctrl+2 /
//     Ctrl+3 on a patch-only selection is the documented EMPTY case: the
//     selection is KEPT and the console says why, rather than silently clearing.
//
// ── THE COMMANDS ────────────────────────────────────────────────────────────
// Four INSTANT commands in the NEW 34100.. block (kiwi_command.h explains the
// id-space move — the original 34001..34029 instant block is full):
//     KIWI_CMD_SELCONV_POINT   34100   Ctrl+1   "Convert Selection to Points"
//     KIWI_CMD_SELCONV_EDGE    34101   Ctrl+2   "Convert Selection to Edges"
//     KIWI_CMD_SELCONV_FACE    34102   Ctrl+3   "Convert Selection to Faces"
//     KIWI_CMD_SELCONV_OBJECT  34103   Ctrl+4   "Convert Selection to Objects"
//
// All four are pure SELECTION changes: they mutate no geometry, so per
// kiwi_command.h they open NO undo bracket (an empty record would make one
// Ctrl+Z do nothing), and they end with Sel_SyncToLegacy + a repaint.
// ─────────────────────────────────────────────────────────────────────────────

// Registers the four rows into the shared `g_radiantCommands` table (unbound —
// kiwi_keymap.cpp lays Ctrl+1..4 on top in the MODERN profile only).
void KiwiSelConv_RegisterCommands();

// The instant-dispatch tail for the four ids.  False for anything else.
bool KiwiSelConv_DispatchInstant( unsigned int commandId );

// §3 canExecute: anything at all is selected.  (Every conversion is meaningful
// for at least one kind, and the EMPTY-RESULT case is reported at run time with
// the reason, which is more useful than a greyed row that will not say why.)
bool KiwiSelConv_CanConvert();

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_caulk.h — KIWI-UX (ROUND BH, ITEMS 1 + 2): THE CAULK WORKFLOW.
// ═════════════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVES, verbatim:
//   1. *"Make (End) key on a solid face = set texture to caulk (caulk is a flag to the
//      compiler to tell it to optimize out the face)."*
//   2. *"Also make it so caulk is always stretched (fit?).  I can't see caulk when I
//      apply it manually atm."*
//
// ── WHY THIS IS ITS OWN TU AND NOT A BLOCK IN kiwi_uv.cpp ─────────────────────────
// kiwi_uv.cpp owns the three MODAL texdef gestures and their shared constraint / HUD /
// numeric state; every symbol in it is either a KiwiEditorCommand member or a helper for
// one.  Caulk is an INSTANT verb over the browser apply funnel with no gesture state at
// all, and its second half (the auto-fit) is a hook that a PORTED file calls back into.
// Folding it into kiwi_uv.cpp would have put a texwnd/select dependency into the file
// that owns the modal UV commands, which is the direction the cleanup pass spent a wave
// undoing.  One small TU, three exported functions.
//
// ── D-BH-A: THE MATERIAL IS RESOLVED THE WAY THE PORTED AUTOCAULK RESOLVES IT ────
// csg.cpp's Brush_AutoCaulkFace (the ported 0x47E080) writes the caulk material with
// `SetMaterial( "caulk", &mat )` (csg.cpp:665 and :679) and Brush_AutoCaulk compares a
// face's current material with `_stricmp( name, "caulk" )` (csg.cpp:722).  So the
// registered editor name of the shipped caulk material is the bare leaf `caulk`, and
// Texture_GetHandle( "caulk" ) (texwnd.cpp:320) is the same lookup the browser uses —
// it lowercases, walks the registered list and, on a first reference, registers
// "wc/caulk" through Register_WorldMaterial (texwnd.cpp:245-248).  In the shipped asset
// set the bulk Load_Materials pass has already registered it, so the lookup is a list
// walk and nothing is loaded here.
//
// ── D-BH-B: THE APPLY GOES THROUGH THE BROWSER FUNNEL, NOT THROUGH A SECOND PATH ──
// The verb resolves caulk to its INDEX in the browser's sorted_materials and calls
// TexWnd_ApplyMaterialAtIndex (texwnd.cpp:1252).  That is round AZ's D-AZ-D argument
// applied a second time: the funnel already carries
//   * the face-selection freshen + validate ("monkey hardening", texwnd.cpp:1296-1316),
//   * TexWnd_BuildClickedMaterialDef -> Texture_SetTexture -> Brush_SetTexture, whose
//     ONE undo record covers both `selected_brushes` and `g_SelectedFaces`
//     (select.cpp:1814-1879),
//   * round AK's Patch_KiwiReNaturalizeSelected fence (texwnd.cpp:1314),
//   * ROUND BH ITEM 3's KiwiUv_EndGestureBeforeApply at its head, and
//   * ROUND BH ITEM 2's auto-fit at its tail.
// A second apply written here would have been a second undo shape and five things to
// forget.  It also means the browser highlights caulk afterwards (the funnel writes
// m_selIndex), so the End key and a thumbnail click leave the editor in one state.
//
// ── D-BH-C: "ALWAYS FIT" IS Brush_FitTexture( 1, 1, 0 ), AND IT IS TWO UNDO STEPS ──
// The fit is the Surface Inspector's own funnel — Brush_FitTexture (select.cpp:3667,
// IDB 0x4939E0), the exact call OnFit and mainfrm.cpp:4813 make — so a caulk face ends
// up with the texdef the "Fit" button would have produced: one repeat across the face.
// Its brush pass routes a PATCH def through sub_47C950 -> Patch_FitTexturing
// (brush.cpp:2445-2446), which is the patch equivalent asked for; patch DENSITY is
// separately re-laid by the funnel's own Patch_KiwiReNaturalizeSelected.
//
// IT COSTS A SECOND UNDO RECORD AND THAT IS STATED RATHER THAN HIDDEN.  Both cores
// bracket THEMSELVES — Brush_SetTexture opens "set face textures"/"set brush textures"
// (select.cpp:1814-1815, closed :1879) and Brush_FitTexture opens "Fit texture"
// (select.cpp:3675-3676, closed :3730) — and Undo_GeneralStart ALWAYS allocates a fresh
// record (undo.cpp:367), so there is no nesting to borrow: an outer
// KiwiCmd_UndoBegin would be orphaned by the first core's own Undo_GeneralStart, which
// is precisely the trap kiwi_uv.h's UNDO RULING documents.  The alternatives were
// re-spelling Brush_FitTexture's body around the raw Texture_Fit / sub_47C950
// primitives (a duplicate of a ported function, i.e. the drift trap) or teaching
// kiwi_undo.cpp to coalesce tickets (which its own header rules out by name).  So the
// caulk apply is TWO Ctrl+Z — undo the fit, then undo the material — and it says so
// once per apply on the console.  Logged in RADIANT_KNOWN_ISSUES.
//
// ── D-BH-D: ONLY CAULK IS AUTO-FIT ───────────────────────────────────────────────
// The directive names caulk specifically, and surprise-fitting an ordinary material
// would destroy a texdef the mapper aligned by hand.  The detection is a LEAF-name
// case-insensitive compare against "caulk" (KiwiCaulk_IsCaulkName), so "wc/caulk" and
// "caulk" both match while "caulk_something" does not — deliberately narrower than
// Cam_EditorMaterialColor's substring table (camwnd.cpp:639-640), which may class a
// whole family as tool-coloured but must never make this fit fire on one.
//
// ── D-BH-E: ITEM 4 IS WHAT MAKES ITEM 2 VISIBLE ─────────────────────────────────
// "I can't see caulk when I apply it manually" had two causes.  The fit is one (a
// default-scaled caulk tile on a large face is a smear).  The other is that the
// unselected camera pass painted caulk FLAT — camwnd.cpp's ROUND BH ITEM 4 block has
// the decode.  Neither fix is sufficient alone.
// ═════════════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BJ, ITEM 1) — END SPLITS INTO PLAIN CAULK AND AUTO CAULK.
//  D-BJ-A..C amend D-BH-B above; the material resolution (D-BH-A), the fit
//  (D-BH-C), the leaf-name rule (D-BH-D) and the camera colouring (D-BH-E) are
//  unchanged and still govern the FACE arm.
// ═════════════════════════════════════════════════════════════════════════════════════
//
// USER REPORT, verbatim: *"The End bind for caulking needs to work on faces.  Currently
// it only works for solids.  When pressed on a solid (or group), it should instead
// AUTO-caulk too.  When used on a face (or faces) it just caulks."*
//
// ── D-BJ-A: WHY THE KEY NEVER ARRIVED ON A FACE SELECTION ────────────────────────
// Nothing in this file was ever reached.  In Face mode a face selection and a live
// PAUSED push/pull are THE SAME STATE — clicking a face auto-enters KIWI_CMD_MOVE and
// pauses it (kiwi_boxselect.cpp:884-891) — and the key funnel's ladder ends with
//     if ( g_activeCommand ) return KiwiCmd_KeyDown( vk, mods );   kiwi_command.cpp
// whose own tail is "Everything else is SWALLOWED" (kiwi_command.cpp:1615-1619).  End
// (VK_END) is not a bare modifier, not in PreemptVerb (kiwi_command.cpp:278-317), not
// in SwapVerb (:334-345, G/R/S only) and not DELETE, so it died there on EVERY press —
// in exactly the state the verb exists for.  With whole brushes selected nothing
// auto-enters, g_activeCommand is NULL, the chord reaches Radiant_TryHotkey and the
// verb ran: "it only works for solids", precisely.  It is the same trap round N found
// for Ctrl+R, round U for Ctrl+1..4 and round AA for DELETE, and the fix is the same
// shape: one rung above the swallow, gated on PreemptIdle.  It does NOT cancel, unlike
// round N's — the browser funnel already runs KiwiUv_EndGestureBeforeApply /
// …RestoreGestureAfterApply around the apply (texwnd.cpp:1261/:1330), which cancels
// AND re-arms the parked push with a fresh baseline, so cancelling in the funnel would
// cost the user the face gizmo on every End press.
//
// ── D-BJ-B: THE THREE ARMS ──────────────────────────────────────────────────────
//   FACE selection (g_SelectedFaces non-empty, no whole brushes)
//       TexWnd_ApplyMaterialAtIndex( caulk ) — plain caulk on exactly those faces,
//       plus D-BH-C's fit.  Unchanged from round BH; it simply runs now.
//   SOLID / GROUP (whole brushes, no explicit faces)
//       Radiant_ExecCommand( 33220 ) — Selection->CSG->Auto Caulk, i.e.
//       Cmd_OnSelectionAutoCaulk (mainfrm.cpp:2817-2824) -> Brush_AutoCaulk
//       (csg.cpp:697, IDB 0x47E0F0).  The PORTED feature, not a re-spelling of it.
//   MIXED
//       Both, split through the typed selection layer, because Brush_SetTexture walks
//       g_SelectedFaces AND selected_brushes in ONE record (select.cpp:1820-1876) —
//       so a mixed plain apply would caulk every face of every selected solid, which
//       is the opposite of what the auto half is for.
//
// ── D-BJ-C: WHAT THE AUTO ARM IS, AND WHAT IT DOES NOT COMPOSE WITH ─────────────
// FACTS about the ported entry point, read off csg.cpp:
//   * SCOPE — `Brush_AutoCaulk` walks `selected_brushes` only (csg.cpp:700-735).  It
//     never looks at g_SelectedFaces, so it is a WHOLE-BRUSH operation by construction.
//   * WHAT IT CAULKS — for each non-fixed, non-patch brush whose accumulated face
//     flags lack 0x80, every face whose current-layer material is not already "caulk"
//     (`_stricmp( name, "caulk" )`, csg.cpp:722) and which `CSG_FaceVisible`
//     (csg.cpp:210, IDB 0x47DE60) proves is fully covered by the OTHER SELECTED
//     brushes.  So it caulks the faces hidden by adjacent solid geometry — the stock
//     feature — and it only sees geometry that is itself selected.
//   * UNDO — it does NOT self-bracket.  Its one caller does: Cmd_OnSelectionAutoCaulk
//     wraps it in Undo_ClearRedo / Undo_GeneralStart("Auto caulk") / Undo_AddBrushList
//     / Undo_EndBrushList / Undo_End.  Reaching it through Radiant_ExecCommand( 33220 )
//     therefore gets the bracket for free and keeps ONE Auto Caulk in the editor.
//   * IT IS NOT A STUB.  Both it and CSG_FaceVisible are complete ports with the IDB
//     addresses in their headers.
//   * COUNT — it prints "%i faces caulked" but returns void, and csg.cpp is a ported
//     file this round does not touch, so the number this verb reports is MEASURED
//     either side of the call (NonCaulkFaceCount).
//   * NO FIT ON THE AUTO ARM, and this is the honest half of the brief's escape
//     hatch: Brush_AutoCaulkFace is the only thing that knows WHICH faces it
//     converted, and Brush_FitTexture( 1, 1, 0 ) takes the SELECTION, not a face list
//     — so fitting after an auto-caulk would re-fit every face of every selected
//     solid, destroying texdefs the mapper aligned by hand on the faces that are still
//     visible.  A caulk face the compiler removes has no visible texdef to fit, so the
//     omission costs nothing the fit exists to buy (D-BH-C's argument was about
//     SEEING caulk; an auto-caulked face is by definition one you cannot see).
//
// ── UNDO COST, stated rather than hidden (D-BH-C, extended) ─────────────────────
// FACE arm: two records (the apply, then the fit), unchanged.  SOLID arm: ONE
// ("Auto caulk").  MIXED: three, plus the selection swaps, which are not records.
// Logged in RADIANT_KNOWN_ISSUES.
//
// One more consequence of the MIXED arm: the face gizmo is not re-armed afterwards.
// The handshake runs ONCE at the head of that arm (a parked push holds a face index
// into a selection the arm replaces twice), so the funnel's own re-arm at the tail
// finds nothing to give back.  The FACE arm — the common case, and the one reported —
// is unaffected and keeps its gizmo.
// ═════════════════════════════════════════════════════════════════════════════════════

// Registers KIWI_CMD_CAULK_FACES ("Caulk Selection"), UNBOUND.  The modern keymap binds
// it to End (kiwi_keymap.cpp); the classic profile is untouched.  Called by
// KiwiCmd_RegisterCommands beside the other feature registrars.
void KiwiCaulk_RegisterCommands();

// The instant-command arm: KIWI_CMD_CAULK_FACES caulks the selection.
bool KiwiCaulk_DispatchInstant( unsigned int cmdId );

// §3 canExecute for the palette row.  Deliberately KiwiUv_CanEdit() itself (kiwi_uv.h)
// rather than a second spelling of it: the condition is "Brush_SetTexture would do
// something", i.e. at least one whole brush selected or at least one face in
// `g_SelectedFaces`, which is the same guard the three UV cores open with.
bool KiwiCaulk_CanExecute();

// TRUE when `name` names the shipped caulk material — leaf-name, case-insensitive.
// NULL and empty answer false.  Used by the auto-fit hook below and exported so nothing
// re-spells the comparison.
bool KiwiCaulk_IsCaulkName( const char *name );

// ── the ITEM 2 hook, called from the TAIL of TexWnd_ApplyMaterialAtIndex ─────────
// `appliedName` is the name of the material the funnel just applied (q->name).  When it
// is caulk this runs Brush_FitTexture( 1, 1, 0 ) over the same selection the apply
// walked and prints the two-undo-steps note; for anything else it returns immediately.
// Safe with an empty selection: the core early-returns (select.cpp:3672-3673).
void KiwiCaulk_AutoFitApplied( const char *appliedName );

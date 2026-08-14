#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_csg.h — RADIANT_UX_DESIGN §24: the CSG *workflow*.
//
// §24 says the math is already ported and "the work is WORKFLOW, not math".  This
// file writes NO CSG math whatsoever.  It adds three things over the ported cores:
//
//   1. §3 canExecute predicates that test the cores' REAL preconditions, so the
//      palette greys a row that would only print an error;
//   2. a before/after observation pair (KiwiCsg_NoteBefore / KiwiCsg_NoteAfter)
//      that reports "N brushes in → M brushes out" on the console, because
//      CSG_MakeHollow is completely silent;
//   3. the "Modeling" block in the shell panel, so CSG is reachable in BOTH
//      keymap profiles (§11) without a new key.
//
// ── WHAT IS ACTUALLY IN csg.cpp (inventory, read not assumed) ────────────────
//   CSG_MakeHollow      0x47D3C0   csg.cpp:318   — command id 32982
//   Brush_MergeList     0x47D600   csg.cpp:409   — CSG_Merge's helper
//   CSG_Merge           0x47DA40   csg.cpp:572   — command id 32927
//   Brush_AutoCaulkFace 0x47E080   csg.cpp:659   — command id 33220 (Auto Caulk)
//   Brush_AutoCaulk     0x47E0F0   csg.cpp:697
//   CSG_FaceVisible     0x47DE60   csg.cpp:210   — AutoCaulk's visibility test
//   + the four brush-classification helpers (Brush_GetFaceFlags,
//     Brush_IsFixedOrPatch, Brush_FaceFlags_Check80, Brush_IsFiltered,
//     Brush_IsCulled).
//
// ── THERE IS NO SUBTRACT ────────────────────────────────────────────────────
// CoD4Radiant has no CSG subtract: csg.cpp has no such core, `Subtract` appears
// nowhere in the radiant sources except unrelated vector maths, there is no menu
// id for it in the ~313-case Radiant_DispatchCommandDirect switch, and
// res/resource.h has no matching #define.  (GtkRadiant's `CSG_Subtract` /
// `Brush_Subtract` were removed from the CoD branch; what survives of that
// machinery is `Brush_SplitBrushByFace` (brush.cpp 0x471960), which is the
// clipper's and MakeHollow's per-plane split — a subtract would have to be
// WRITTEN on top of it.)
//
// Writing it is explicitly out of scope for this phase ("do NOT write new CSG
// math"), so "CSG: Subtract" is NOT registered, is NOT in the palette, and is
// logged in RADIANT_KNOWN_ISSUES instead of being faked.
//
// ── ROUND L UPDATE: THE FINDING STANDS; THE VERB EXISTS ─────────────────────
// Everything above is still true of csg.cpp — no core was found, none was added,
// and this file still dispatches only the three classic ids.  What ROUND L did is
// exactly the thing this note said a subtract "would have to be": WRITTEN on top
// of Brush_SplitBrushByFace, as a sequence of N plane splits that keeps the
// OUTSIDE half at each of the tool's face planes.  It lives in kiwi_boolean.h /
// .cpp, on the Q key, with its own §19 gate and its own undo bracket — NOT here,
// because this file's contract is "no CSG math, only dispatch" and that is worth
// keeping true.
//
// ── NO PREVIEW IN v1 ────────────────────────────────────────────────────────
// §24 asks for an overlay preview "where practical".  It is not practical here:
// both cores mutate the live brush lists in place (CSG_Merge unlinks every
// selected instance before it even tries, and re-adds them on failure;
// CSG_MakeHollow frees the source brush as it goes), so a faithful preview means
// running the whole operation on CLONES of the selection and drawing the result —
// a clone-and-run harness plus a way to draw unlinked defs.  The second half of
// that harness now exists (kiwi_bevel.cpp draws an unlinked def for Inset), so
// this is a real follow-up rather than a dead end.  Logged as future work.
//
// ── PRECONDITIONS, TAKEN FROM THE CORES ─────────────────────────────────────
//   Hollow (mainfrm.cpp Cmd_OnSelectionMakehollow, 0x425570): refuses more than
//     ONE selected brush outright ("Can't hollow more than 1 brush at a time.").
//     CSG_MakeHollow itself then skips fixed-size entities, patches and
//     xx5-flagged instances.  → canExecute: exactly one selected brush, not a
//     patch, not a fixed-size entity.
//   Merge (csg.cpp:572): needs >= 2 selected, none fixed-size, none a patch, all
//     owned by the SAME entity.  → canExecute tests all four.
//   Auto Caulk (csg.cpp:697): any selection.
// The predicates are cheap list walks over `selected_brushes`; the palette calls
// them once per visible row per frame while it is open.
// ─────────────────────────────────────────────────────────────────────────────

// ── §3 palette predicates over the CLASSIC ids ──────────────────────────────
// These attach to the EXISTING metadata rows for 32982 / 32927 / 33220 — no new
// command id is minted, so the palette still shows exactly one row per operation
// and the classic menu / hotkey / palette routes all reach the same handler.
bool KiwiCsg_CanHollow();     // 32982
bool KiwiCsg_CanMerge();      // 32927
bool KiwiCsg_CanAutoCaulk();  // 33220

// ── the console feedback pair (// KIWI-UX hooks in mainfrm.cpp) ─────────────
// NoteBefore snapshots the selected-brush count; NoteAfter reports the delta with
// `label` and re-reports the surviving selection.  They observe only — neither
// touches geometry, the selection or the undo stack — so they are legal at any
// point inside a ported handler.  NoteAfter with no matching NoteBefore is a
// no-op (that is what makes the hollow handler's early "more than 1 brush" return
// safe to leave alone).
void KiwiCsg_NoteBefore();
void KiwiCsg_NoteAfter( const char *label );

// ── the shell panel block ───────────────────────────────────────────────────
void KiwiCsg_MenuItems();

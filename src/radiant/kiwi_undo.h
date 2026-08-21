#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_undo.h — RADIANT_UX_DESIGN §39: ONE undo timeline over BOTH domains.
//
// USER DIRECTIVE (shakeout I), verbatim: "Redo the whole undo/redo system so that
// it works with every action."
//
// ── WHAT WAS BROKEN ─────────────────────────────────────────────────────────
// There were TWO undo stacks and no relationship between them:
//   * the PORTED one (undo.cpp) — brush/entity snapshots, 64 records, driven by
//     Undo_Start/Undo_End and rolled back by Undo_Undo/Undo_Redo;
//   * the CONSTRUCTION one (kiwi_construct.cpp) — whole-store snapshots of the
//     construction geometry, 32 deep, with NO redo at all.
// kiwi_construct.h "SCOPE RULING 2" wrote the split down honestly and logged it
// in RADIANT_KNOWN_ISSUES, and the ruling's own words were "a split undo is
// exactly the kind of thing that surprises a user once".  It surprised them.
// Concretely, before this file:
//   * Ctrl+Z INSIDE a drawing tool popped the construction stack; Ctrl+Z outside
//     one popped the legacy stack.  The user had to know which world they were in.
//   * drawing a line then moving a brush then pressing Ctrl+Z twice undid the
//     brush move and then... the brush move before that.  The line stayed.
//   * there was no construction REDO in any spelling — Ctrl+Y after a
//     construction undo silently redid a legacy record instead.
//
// ── THE JOURNAL ─────────────────────────────────────────────────────────────
// This file adds NO new snapshot format and copies NO geometry.  It is a list of
// TICKETS — {domain, label} — appended in the order records are actually closed,
// and Ctrl+Z pops the newest ticket and forwards it to whichever domain owns it:
//
//      s_undo (oldest .. newest)          s_redo (oldest .. newest)
//      [LEGACY "drag selection"]          [CONSTRUCTION "draw line"]
//      [CONSTRUCTION "draw rect"]   <->   ...
//      [LEGACY "extrude region"]
//
//   Ctrl+Z  pop s_undo.back()  ->  LEGACY: Undo_Undo()     CONSTRUCTION: KiwiCon_UndoPop()
//                              ->  push that ticket onto s_redo
//   Ctrl+Y  pop s_redo.back()  ->  LEGACY: Undo_Redo()     CONSTRUCTION: KiwiCon_RedoPop()
//                              ->  push that ticket back onto s_undo
//
// The two domains keep their own storage, their own depth limits and their own
// restore code.  ONLY the ORDER is unified — which is the whole of what was wrong.
//
// ── WHERE THE TICKETS COME FROM (both hooks are one line at a tail) ─────────
//   LEGACY        undo.cpp's Undo_End tail.  Undo_End is THE single close point:
//                 it is the only function that writes `done = 1`, every one of
//                 the ~40 Undo_End call sites in the tree is a bracket close, and
//                 both Undo_Undo and Undo_Redo consume records only in the
//                 `done` state.  A record that is never closed can never be
//                 undone, so a ticket for it would be a lie.
//   CONSTRUCTION  kiwi_construct.cpp's KiwiCon_UndoPush.  The store pushes a
//                 whole-store snapshot BEFORE mutating, so the push IS the
//                 record, and there is no separate close.
//
// ── EVICTION SYNC (the one thing that can desync a journal) ─────────────────
// The legacy stack is CAPPED (g_undoMaxSize 64 records, g_undoMaxMemorySize 2 MB)
// and drops its OLDEST record when either cap is hit.  A journal that did not
// hear about that would hold a ticket for a record that no longer exists, and
// enough Ctrl+Z presses would eventually forward a LEGACY ticket to an empty
// legacy stack — "Nothing left to undo" printed while the journal still claimed
// there was something.
//
// MECHANISM, and it is a HOOK rather than an inference: undo.cpp has EXACTLY ONE
// eviction point, Undo_FreeFirstUndo (undo.cpp:316), called from Undo_GeneralStart
// (the record-count cap, undo.cpp:444) and from Undo_End's trim loop (the memory
// cap, undo.cpp:701).  Its tail calls KiwiUndo_NoteLegacyEvicted(), which drops
// the OLDEST LEGACY ticket — the one for exactly the record that just died.
// Undo_Clear (map new / map load / a max-size change) frees the whole list in its
// own loop instead, so it gets its own hook, KiwiUndo_NoteLegacyCleared, which
// drops every LEGACY ticket from BOTH stacks.
//
// BELT AND BRACES: KiwiUndo_Undo also VERIFIES before it forwards.  A LEGACY
// ticket is only forwarded when g_lastundo is non-null (and a LEGACY redo ticket
// only when Undo_RedoAvailable() says so, undo.cpp:99);
// otherwise the ticket is discarded as stale and the next one is tried.  So even
// if a future eviction path is added without a hook, the journal self-heals
// instead of pretending.
//
// ── CANCEL SUPPRESSION ──────────────────────────────────────────────────────
// KiwiCmd_UndoCancel (kiwi_command.cpp) rolls a gesture back by CLOSING its own
// bracket and immediately undoing it: Undo_EndBrushList -> Undo_End -> Undo_Undo
// -> Undo_ClearRedo.  That Undo_End is a real close and would mint a ticket for a
// record that is destroyed three lines later — one phantom Ctrl+Z per cancelled
// gesture.  So KiwiCmd_UndoCancel brackets itself in
// KiwiUndo_SuppressBegin/End (kiwi_command.cpp:1064), which stops the APPEND
// hooks only.  The Undo_ClearRedo hook still fires inside the suppression window,
// because the legacy redo list genuinely IS cleared there and the journal's redo
// stack must follow.
//
// ── REDO RULES (Undo_ClearRedo's rule, generalised to two domains) ──────────
// undo.cpp's rule is "any new record destroys the redo list" (Undo_Start =
// Undo_ClearRedo + Undo_GeneralStart).  Generalised:
//   * a NEW ticket in EITHER domain clears the WHOLE journal redo stack and the
//     construction store's own redo stack with it;
//   * undo.cpp's Undo_ClearRedo clears it too (that is where KiwiCmd_UndoBegin,
//     Undo_Start and the cancel path all destroy the legacy redo);
//   * a REPLAY (KiwiUndo_Redo forwarding a ticket) does neither — it is not new
//     work.  Undo_Redo internally runs Undo_GeneralStart + Undo_End, so without
//     the replay flag a single Ctrl+Y would wipe the rest of the redo stack.
// Conservative on purpose: the worst case is losing a redo you might have kept,
// and the failure mode it rules out is forwarding a ticket into an empty stack.
//
// ── SELECTION SAFETY AFTER A CONSTRUCTION UNDO ──────────────────────────────
// KiwiCon_UndoPop replaces s_objects WHOLESALE, so every index the construction
// selection holds is meaningless.  It already calls KiwiConSel_NoteStoreReplaced
// (kiwi_construct.cpp:723) and the new KiwiCon_RedoPop does the same; those are
// the only two places the store is swapped out from under the selection outside
// Add/RemoveAt/ClearAll, which notify already.  Nothing else caches construction
// indices across a frame — kiwi_region re-derives on the generation counter, the
// drawing tools hold their own in-progress point list (not indices) and
// kiwi_snap.cpp walks the store live.
//
// ── WHAT IS DELIBERATELY *NOT* HERE ────────────────────────────────────────
// No merge of the two snapshot formats, no cross-domain compound records ("this
// extrude consumed that line" is still two tickets), and no coalescing of
// adjacent tickets.  All three are real features and none of them is what the
// user asked for.
// ─────────────────────────────────────────────────────────────────────────────

// The domains a ticket can name.
//
// == ROUND AG, ITEM 7: THE THIRD DOMAIN ====================================
// USER DIRECTIVE, verbatim: "making something hidden should be an un-doable
// action."
//
// It could not be a LEGACY record, and that is a fact about the data rather than
// a preference.  The hidden bit is `selbrush_t::brushFlags & 4` plus the depth
// counter `selbrush_t::xx5` (select.cpp:4168-4180), and BOTH live on the
// INSTANCE.  The legacy undo snapshots DEFS: Undo_AddBrush clones through
// Brush_FullClone_sub475E80 (undo.cpp:510 -> brush.cpp:7424), which allocates a
// 0x58 `brush_t` that has no brushFlags member at all.  Worse, Undo_Undo's
// re-create phase goes through Brush_AddToList (brush.cpp:667), whose
// `memset( b, 0, 0x38u )` ZEROES brushFlags and xx5 -- so a legacy record does
// not merely fail to carry the hidden bit, it destroys it.  (Brush_AddToList2,
// brush.cpp:942, clears the low five bits on every add-to-selection for the same
// structural reason.)
//
// So hide gets a domain of its own with its own tiny store, exactly the shape
// KUNDO_CONSTRUCTION has: a whole-state snapshot, push/pop/redo/clear, keyed on
// the stable `brush_t *` DEF pointer rather than on the instance.  See
// kiwi_visibility.h for the store and for what it does and does not restore.
enum kundoDomain_t
{
    KUNDO_LEGACY = 0,           // undo.cpp's brush/entity record stack
    KUNDO_CONSTRUCTION,         // kiwi_construct.cpp's whole-store snapshots
    KUNDO_VISIBILITY,           // kiwi_visibility.cpp's hidden-bit snapshots
};

// ── the two Ctrl+Z / Ctrl+Y entry points ────────────────────────────────────
// Both return false when their stack is empty, which is what lets the caller fall
// through to the CLASSIC handler (mainfrm.cpp's Cmd_OnEditUndo / Cmd_OnEditRedo)
// so a legacy record minted before this layer existed still undoes.
bool KiwiUndo_Undo();
bool KiwiUndo_Redo();

// Depths, for the KIWI panel readout and the palette's canExecute.
int  KiwiUndo_UndoDepth();
int  KiwiUndo_RedoDepth();
// The newest ticket's label ("drag selection", "draw line"), or NULL.
const char *KiwiUndo_UndoLabel();

// ── the record hooks (called from the two domains' tails) ───────────────────
// `operation` is the undo record's own operation string, which undo.cpp already
// requires to be a literal/static (undo.cpp:410 stores the POINTER), so keeping
// the pointer here is exactly as safe as the record itself.
void KiwiUndo_NoteLegacyRecord( const char *operation );
void KiwiUndo_NoteConstructionRecord( const char *operation );
void KiwiUndo_NoteVisibilityRecord( const char *operation );   // ROUND AG, ITEM 7

// ── the consistency hooks (undo.cpp) ────────────────────────────────────────
void KiwiUndo_NoteLegacyEvicted();      // Undo_FreeFirstUndo tail — oldest record died
void KiwiUndo_NoteLegacyCleared();      // Undo_Clear tail — the whole legacy stack died
void KiwiUndo_NoteLegacyRedoCleared();  // Undo_ClearRedo tail — the legacy redo died

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BJ, ITEM 4) — THE FACE-GRANULAR RESTORE
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: *"When ctrl-z'ing, dont select the whole solid (Bug!)."*
//
// ── THE ROOT CAUSE, end to end ──────────────────────────────────────────────
// A UV-editor gesture (and every other face-scoped KIWI op) opens ONE legacy
// record and covers the face-selected brushes by hand, because a brush named only
// by a FACE is not on `selected_brushes` at all (kiwi_selection.h DESIGN NOTE 2):
//     kiwi_uveditor.cpp UndoOpen -> KiwiCmd_UndoBegin + KiwiCmd_UndoCoverBrush
//                                -> Undo_AddBrush( node->def )
// Ctrl+Z then reaches Undo_Undo (undo.cpp:736) through this journal, and the
// legacy restore destroys the face granularity in TWO places, neither of which
// this layer could see:
//   1. undo.cpp:808  `Select_Deselect( 1 )` — and a1 != 0 means
//      `g_SelectedFaces.SetSize( 0 )` (select.cpp:1466-1468).  The whole face
//      selection is thrown away outright.
//   2. undo.cpp:932-982, Phase 4 — every brush DEF the record covered is
//      re-created and handed to `Brush_AddToList2( newInst )` (undo.cpp:974),
//      whose body TAIL-INSERTS the instance into `selected_brushes` and clears the
//      low five brushFlags bits (brush.cpp:933-943).  So every covered brush comes
//      back SELECTED AS A WHOLE OBJECT.
// undo.cpp:1011's `Sel_InvalidateFromLegacy()` then makes the typed layer re-derive
// from exactly that state, so KiwiSel() ends up holding SEL_OBJECT items where the
// user had SEL_FACE ones.  Both legacy behaviours are FAITHFUL and are not touched:
// face selection is a KIWI-side concept and the binary's undo has no idea it exists.
//
// ── THE FIX: A VALUE SNAPSHOT ON THE TICKET ─────────────────────────────────
// KiwiCmd_UndoBegin (the ONE bracket opener every KIWI gesture uses; ported code
// calls Undo_Start instead and is therefore unaffected) ARMS a snapshot of
// KiwiSel() before the first mutation.  The next ticket Append mints carries it.
// After a LEGACY ticket has been forwarded — undo AND redo, because a face-scoped
// gesture does not change the selection, so the state that was right at gesture
// begin is right at both ends — the snapshot is re-applied through the KIWI
// selection API + Sel_SyncToLegacy (which opens with its own Select_Deselect(1),
// so the whole-brush selection Phase 4 left behind is cleared for free).
//
// KEYED BY VALUE, NEVER BY POINTER, because Phase 4 hands back the record's CLONE
// of the def and a fresh selbrush_t instance: an entry is
// {brush ordinal in the snapshot's own brush list, def bounds + faceCount as the
// fingerprint, face plane, and the item's own indices}.  On restore the live
// brushes are walked in list order, each snapshot ordinal is matched to the first
// UNUSED brush whose fingerprint agrees, and a face entry additionally checks the
// plane at that face index.  A snapshot that resolves nothing is DISCARDED and the
// legacy selection is left exactly as the ported undo made it.
//
// SCOPE, deliberately narrow: the restore only runs when the snapshot contained at
// least one FACE item.  An object-only gesture already ends up with its objects
// selected, and re-writing the selection there would be a behaviour change for a
// case that has no bug.  It is also skipped outright while a modal command is live
// — a re-select underneath a parked face push is the trap round N documented, and
// the same discipline the ROUND BH handshake uses (kiwi_uv.cpp
// KiwiUv_EndGestureBeforeApply) applies here: do not hand a gesture a selection it
// did not ask for.  (Ctrl+Z cannot reach this with a command live anyway — the key
// funnel's own swallow rung sees to that — so the guard costs nothing.)
void KiwiUndo_ArmSelectionSnapshot();

// ── append suppression (KiwiCmd_UndoCancel) ─────────────────────────────────
// Re-entrant (a depth counter): a cancel that runs inside another cancel cannot
// leave the journal deaf.
void KiwiUndo_SuppressBegin();
void KiwiUndo_SuppressEnd();

// Drop everything (map new / map load).  Called from the same places the two
// stores are themselves reset.
void KiwiUndo_Reset();

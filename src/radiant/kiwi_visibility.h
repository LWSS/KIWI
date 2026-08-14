#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_visibility.h — ROUND J: the HIDE / ISOLATE family, and the one member of
// it Radiant never had.
//
// ── WHAT PLASTICITY BINDS (read, not guessed) ───────────────────────────────
// plasticity/src/startup/default-keymap.ts:296-299, the `body:not([gizmo])`
// scope, and the same four again in src/Menu.ts:48-51 with their menu labels:
//
//     h        command:hide-selected     "Hide selected"
//     shift-h  command:hide-unselected   "Hide everything other than selected"
//     alt-h    command:unhide-all        "Unhide all"
//     ctrl-h   command:invert-hidden     "Invert hidden"
//
// There is NO separate "isolate" verb and no isolate MODE with its own exit key:
// in Plasticity, ISOLATE IS `hide-unselected`, and you leave it with `alt-h`.
// That is the whole design, and it is why this round ships the family rather
// than inventing a mode — a mode would need its own state, its own restore set
// and its own way of behaving when the selection changes underneath it, none of
// which Plasticity has and none of which anybody asked for.
//
// ── WHAT RADIANT ALREADY HAD, AND THE ONE-LINE DIFFERENCE ───────────────────
// Three of the four are ALREADY PORTED AND ALREADY WIRED, on the same letter:
//
//   select.cpp:4160  Select_Hide()            hide the SELECTED brushes
//   select.cpp:4185  Select_HideUnselected()  hide the ACTIVE (unselected) brushes
//   select.cpp:4245  ShowHidden()             clear every hidden bit and depth
//   mainfrm.cpp:5554-5556  ids 32923 / 32934 / 32924
//   mainfrm.cpp:992-994    H / Alt+H / Shift+H  (the DEFAULT bindings)
//
// Classic Radiant's shift/alt are the OTHER WAY ROUND from Plasticity's:
//   classic   H = hide selected · Shift+H = SHOW HIDDEN   · Alt+H = hide unselected
//   modern    H = hide selected · Shift+H = hide unselected · Alt+H = SHOW HIDDEN
// So the modern profile SWAPS mods 1 and 2 on vk 0x48 and nothing else moves —
// no displacement, no new id, no new core.  The full audit is in kiwi_keymap.h.
// Shift+H then reads as ISOLATE, which is what a mapper actually presses it for.
//
// ── THE MISSING FOURTH: INVERT HIDDEN (KIWI_CMD_HIDE_INVERT, Ctrl+H) ────────
// Radiant has no `invert-hidden` in any spelling — there is no command id, no
// menu item and no core (the only nearby thing is HideByClassname 32925, which
// is a different verb).  So this file adds ONE, and it is written against the
// SAME TWO FIELDS the three ported cores use and nothing else:
//
//     brushFlags & 4   the hidden bit
//     xx5              the hide DEPTH (Select_Hide deepens it, ShowLastHidden
//                      peels one level, ShowHidden zeroes it)
//
// KiwiVis_InvertHidden walks BOTH display lists exactly as the ported cores do
// (selected first, then active — the order Select_Hide uses) and, per brush:
//     hidden  ->  clear the bit, depth 0     (what ShowHidden does)
//     visible ->  set the bit,   depth 1     (what Select_Hide's THIRD pass does)
//
// DELIBERATE, DOCUMENTED CONSEQUENCE: an invert FLATTENS the hide depth to a
// single level, so a "Show Last Hidden" (33246) after an invert peels everything
// rather than one layer.  That is the same thing Select_Hide's own third pass
// does to its targets (select.cpp:4177-4181 writes `xx5 = 1` unconditionally),
// so it is the house behaviour rather than a new rule — and the alternative,
// inventing a depth arithmetic for a fused hide+show, would be a rule with no
// source anywhere.
//
// ── UNDO: NONE, AND THAT IS FAITHFUL (ROUND J — OVERTURNED IN ROUND AG) ─────
// None of the three ported hide handlers opens an undo bracket
// (mainfrm.cpp:4963-4975 — each is a bare call plus the ported g_nUpdateBits
// invalidation), because hiding changes no geometry: it is a VIEW state, exactly
// as it is in Plasticity, whose hide commands go through the Scene's visibility
// database and not through its history.  So KiwiVis_InvertHidden opens no bracket
// either, and per kiwi_command.h ("a command that mutates nothing must NOT open a
// bracket at all") that is the correct answer rather than an omission: an empty
// legacy record would make one Ctrl+Z do nothing, and a non-empty one would make
// Ctrl+Z restore brush GEOMETRY the user never changed.  Ctrl+H is its own
// inverse, which is the undo Plasticity gives it too.
//
// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AG, ITEM 7 — THE RULING ABOVE IS OVERTURNED BY THE USER
// ═════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: "making something hidden should be an un-doable
// action."
//
// The round-J argument had one true half and one false half.  TRUE: a hide must
// not go into a LEGACY record — the paragraph above is right that a legacy
// record restores GEOMETRY, and kiwi_undo.h now writes out the further, worse
// reason (the legacy snapshot is of DEFS, brushFlags lives on the INSTANCE, and
// Undo_Undo's re-create phase memsets it to zero).  FALSE: "therefore it is not
// undoable at all".  The CONSTRUCTION store's hide has been journalled since
// round U by exactly the right mechanism — its own snapshot, its own domain —
// and the asymmetry was visible to the user as "Ctrl+Z undoes hiding a LINE but
// not hiding a BRUSH".  One editor, one Ctrl+Z.
//
// ── THE STORE ───────────────────────────────────────────────────────────────
// KUNDO_VISIBILITY, modelled on KiwiCon_UndoPush / UndoPop / RedoPop / ClearRedo
// one for one.  A snapshot is one entry per live brush instance:
//     { brush_t *def;  bool hidden;  int depth; }
// keyed on the DEF pointer, because the instance does not survive an unrelated
// legacy undo and the def does (Undo_Undo relinks the same def to a fresh
// selbrush_t).  Restoring walks both display lists and writes back to every live
// instance whose ->def matches.
//
// WHAT IT RESTORES, AND WHAT IT DELIBERATELY DOES NOT.  ONLY bit 2 of brushFlags
// and xx5.  Never the whole word: bit 0 is FilterBrush's cache, bit 1 and bit 5
// are the LAYER system's, bit 7 is SELECTED and bits 3/4 are the filter-list
// accumulators (filters.cpp:632-634, layers.cpp:524-530, qedefs.h:29).  Writing
// the word back would make Ctrl+Z on a hide silently re-select brushes and stomp
// layer visibility, which is precisely the class of over-reach the round-J note
// was right to be afraid of.
//
// WHAT IT CANNOT DO.  A brush created SINCE the snapshot is not in it and is left
// exactly as it is; a brush destroyed since is skipped.  Both are the "dropped a
// stale step" case the journal already handles, and neither can corrupt anything
// — the worst outcome is that one brush keeps its current visibility.
//
// ONE RECORD PER GESTURE.  The push happens once, at the top of each of H,
// Shift+H, Alt+H, Shift+Ctrl+H, Ctrl+H and each outliner eye click — never per
// brush.  KiwiVis_UndoPush is a no-op when the resulting snapshot would be
// identical to the last one pushed, so a hide that changes nothing does not cost
// the user a Ctrl+Z.
// ─────────────────────────────────────────────────────────────────────────────

// ── ROUND AG, ITEM 7: the hide/unhide undo domain (kiwi_undo.h KUNDO_VISIBILITY)
// Snapshot the CURRENT hidden state and mint one journal ticket, then mutate.
// `label` must be a literal / static — the journal keeps the pointer, exactly as
// undo.cpp's own records do.  Call it BEFORE the first write of a gesture.
void KiwiVis_UndoPush( const char *label );
// The journal's forwards.  Mirror KiwiCon_UndoPop / RedoPop / ClearRedo.
bool KiwiVis_UndoPop();
bool KiwiVis_RedoPop();
void KiwiVis_ClearRedo();
void KiwiVis_UndoReset();

// Apply `hidden` to ONE brush instance, writing the same two fields the ported
// family writes.  Exported so the outliner's eye stops carrying its own copy.
void KiwiVis_SetHidden( selbrush_t *b, bool hidden );

// ROUND AG, ITEM 7: named by KiwiVis_SetHidden below.
struct selbrush_t;

// The KIWI-owned fourth member of the family (Ctrl+H).
void KiwiVis_InvertHidden();

// ── §3 palette predicates ────────────────────────────────────────────────────
// Hide / Isolate need something selected (both ported cores return immediately on
// an empty selected list — select.cpp:4162 / 4187).
bool KiwiVis_CanHide();
// Show Hidden / Invert Hidden are only meaningful when something IS hidden; the
// scan is over both display lists and stops at the first hit.
bool KiwiVis_HasHidden();
// Invert is meaningful whenever there is ANY brush at all — with nothing hidden it
// hides everything, which is a legitimate (and instantly reversible) act.
bool KiwiVis_CanInvert();

// How many brushes are currently hidden — the HUD / console readout.
int  KiwiVis_HiddenCount();

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AO — HIDDEN SOLIDS SURVIVE SAVE / LOAD
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: *"Hidden solids are not respected on save and unhide on
// load."*
//
// ── WHY IT WAS LOST, AND WHY THE .map CANNOT CARRY IT ───────────────────────
// The hidden bit is `brushFlags & 4` on the selbrush_t INSTANCE (see the top of
// this file).  MapFile_WriteEntity (map.cpp:1390) writes the brush DEFINITION —
// planes, materials, layer — and nothing instance-side, so nothing about
// visibility reaches the file; a reload builds every instance from scratch with
// brushFlags == 0, i.e. fully visible.  That is CORRECT for the .map and must
// stay that way: the .map is compiler input and stock Radiant input, and an
// editor-only "this one is hidden" attribute has no business in it (RADIANT_UX_
// DESIGN §7 / decision D-5 — the same argument that put construction geometry in
// a sidecar rather than in entity keys).
//
// So the state goes where every other editor-only fact already goes: the KIWI2
// SIDECAR, `<mapname>.kiwi`, written by KiwiCon_SaveSidecar (kiwi_construct.cpp:
// 4333) and read by KiwiCon_LoadSidecar (:4439).  NO VERSION BUMP — the lines are
// optional and top-level, and the reader's `if (!inObject) continue;` gate
// (kiwi_construct.cpp:4546) means every build that shipped before this round
// silently skips them.  A map with nothing hidden writes not one extra byte.
//
// ── THE ORDINAL SPACE (the whole difficulty) ────────────────────────────────
// There is no persistent brush identity in this codebase — a brush_t has a
// pointer and nothing else that survives a process, and the .map records no id.
// So the association is POSITIONAL: `hiddenbrush N`, where N is the brush's index
// in the map's SAVE ENUMERATION.  That enumeration is a verbatim copy of
// Map_SaveFile's own walk and must stay one:
//
//     Map_SaveFile      map.cpp:693-706   entity loop  (entities.next → &entities,
//                                         gated on `def-list non-empty || worldspawn`)
//     MapFile_WriteEntity map.cpp:1515-1517  per-entity DEF-list loop
//                                         (b = eDef->brushes.prev; b != &eDef->def;
//                                          b = b->onext)
//
// The def-list walk is INSERTION ORDER, not reverse.  Entity_LinkBrush
// (entity.cpp:432-453) looks like a head insert but writes `old_head->onext = b`
// and only ever sets the +0x0C "first element" slot on the FIRST link, so
// `brushes.prev` stays the OLDEST brush and `onext` walks forward to the newest.
// (Entity_Free_R's `b->oprev == &e->brushes` assert at entity.cpp:400 is the same
// fact stated from the other end.)  The classic "Radiant lists come back reversed"
// trap therefore does NOT apply here — verified, not assumed.
//
// The LOAD side lines up for the same reason: ParseEntity (entity.cpp:1147) calls
// Entity_LinkBrush once per `{` block in FILE ORDER, and Map_LoadEntities
// (map.cpp:2125-2132) IncRefs each parsed entity onto the TAIL of `entities`
// (entity.cpp:325-332), also in file order.  Re-running the save walk after a load
// therefore reproduces the file's order exactly, which is what makes an ordinal
// mean the same brush on both sides.
//
// PATCHES ARE IN THE SAME ORDINAL SPACE, INTERLEAVED.  A patch is a brush_t
// carrying a patchMesh_t: Brush_Parse (brush.cpp:3905) consumes a `patchDef5` /
// `patchTerrainDef3` block and returns an ordinary brush_t that ParseEntity links
// like any other, and Brush_Write emits it from the same def-list loop.  There is
// no second numbering and no need for one.
//
// FIXED-SIZE ENTITIES ARE INCLUDED.  Their bbox brush is never WRITTEN to the .map
// (MapFile_WriteEntity:1511 gates brush output on `!fixedsize`), but ParseEntity's
// fixed-size tail (entity.cpp:1250) recreates exactly one of them per entity, in
// entity order — so counting it costs nothing and buys the case a mapper actually
// hits: hiding a misc_model or a light and having it stay hidden.
//
// ── HONEST LIMITS (documented, guarded, never fatal) ────────────────────────
// A positional association is only as good as the two walks agreeing:
//   • the .map edited outside KIWI (by hand, by another Radiant, by a merge tool)
//     shifts every ordinal after the edit;
//   • a future divergence between the save walk and the load walk does the same;
//   • a fixed-size entity that somehow owns more than one brush def loses the
//     extras on reload (ParseEntity:1205-1213 discards them).
// Every one of those is caught or harmless:
//   • `hiddenbrushtotal N` is written beside the ordinals; if the post-load
//     enumeration does not have exactly N brushes the whole set is DROPPED with a
//     console warning, because a shifted set is worse than none;
//   • an ordinal outside [0, total) is skipped, never applied and never an error;
//   • a wrong hide is cosmetic and Alt+H (Unhide All / ShowHidden, select.cpp:4245)
//     clears it in one keystroke.
// Nothing here can fail a map load: the sidecar is read after the map is fully
// live and every arm of the parse returns rather than throwing.
//
// ── THE API (all brush walking stays in kiwi_visibility.cpp) ────────────────
// SAVE.  KiwiCon_SaveSidecar calls Build once, then reads the count/total/ordinals
// back out.  Deliberately NOT a FILE*-taking writer: kiwi_visibility.h would then
// have to drag <stdio.h> into every one of its includers for two integers.
//
// Rebuild the enumeration and return how many of its brushes are hidden.
int  KiwiVis_SidecarBuild();
// Total brushes in the enumeration Build just took (the `hiddenbrushtotal` value).
int  KiwiVis_SidecarTotal();
// The i'th hidden ordinal, 0 <= i < the count Build returned; -1 out of range.
int  KiwiVis_SidecarOrdinal( int i );
//
// LOAD.  Begin at the head of KiwiCon_LoadSidecar (so a missing / older / hidden-
// less sidecar reliably clears the pending set), Note per `hiddenbrush` line,
// Total per `hiddenbrushtotal` line, and Apply from map.cpp AFTER the sidecar read
// — the flags are written to live INSTANCES, which only exist once the map load
// has finished building them.
void KiwiVis_SidecarLoadBegin();
void KiwiVis_SidecarLoadTotal( int total );
void KiwiVis_SidecarLoadNote( int ordinal );
void KiwiVis_SidecarLoadApply();

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AF, ITEM 9 — "WHY IS THAT CURVE NOT SELECTABLE?", ANSWERED IN ONE LINE
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "When hiding selected objects, q3 curves don't hide.
// actually they aren't selectable at all. Fix this."
//
// The HIDE half was a missing `FilterBrush` in the camera's patch pass and is
// fixed at the defect (camwnd.cpp, fenced).  The SELECTABILITY half has FOUR
// separate gates, every one of them a legitimate feature, every one of them
// SILENT, and any one of them alone produces "I click the curve and nothing
// happens":
//
//   1. `g_PrefsDlg->m_bSelectCurves == 0` — the ported "Don't select curves"
//      toggle (command 32852, mainfrm.cpp:5820).  select.cpp:698 drops every
//      patch from the ray walk while it is off.  It is ALSO a one-way trap
//      within a session: the toggle calls Prefs_SavePrefs, which deliberately
//      does not write this key (prefs.cpp:279, a faithful port of the binary's
//      own omission), so the state cannot be inspected anywhere.
//   2. A `Misc curve` FILTER entry with isShown == 0 — filters.cpp:509 answers
//      "filtered" for every patch whose `type != 64`, and the ROUND Q fillet
//      stamps PATCH_BEVEL (kiwi_patchfillet.cpp:1189), i.e. squarely inside it.
//      `FilterBrush` then removes the patch from all three pick entries
//      (select.cpp:672, kiwi_pick.cpp:167, kiwi_boxselect.cpp).
//   3. `Misc terrain` likewise, for a type-64 patch.
//   4. SELECTION MODE 3 (Face).  A patch has no brush face to resolve to, so
//      kiwi_pick.cpp:584-599 answers "no hit" by design.  Modes 4 and 5 select
//      the whole patch; modes 1 and 2 take its control points.
//
// This prints ONE line naming every gate that is CURRENTLY ON.  It reads state
// and changes nothing, which is what makes it safe to hang off a key.
//
// `force` false — the H key funnel's call, i.e. the exact moment the user
//     noticed.  At most ONCE per session, and only when the map actually contains
//     a patch, so a curve-free map never sees a word of it.
// `force` true — Ctrl+H (Invert Hidden), the family's "why is my visibility
//     weird" verb.  Always prints, including the ALL CLEAR line, whose value is
//     that it rules the four gates out and names what is left to suspect (the
//     patch's own `curveDef`, which PMESH_51 needs — pmesh.cpp:4257).
void KiwiVis_ReportPatchGates( bool force );

// ── commands ─────────────────────────────────────────────────────────────────
void KiwiVis_RegisterCommands();
bool KiwiVis_DispatchInstant( unsigned int cmdId );

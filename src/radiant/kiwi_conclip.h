#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_conclip.h — KIWI-UX ROUND AR, ITEM 1: COPY / PASTE FOR CONSTRUCTION
//                  GEOMETRY, and the Move that follows a paste.
//
// USER DIRECTIVE, verbatim: *"allow copy pasting of construction lines! after a
// paste, it should automatically go into G(move) mode."*
//
// ═════════════════════════════════════════════════════════════════════════════
//  WHAT ALREADY EXISTED, AND WHAT THIS ADDS
// ═════════════════════════════════════════════════════════════════════════════
// The BRUSH clipboard is entirely ported: Ctrl+C is classic id 33039
// (mainfrm.cpp:1191's binding -> Cmd_OnEditCopybrush -> XYWnd_CopyClip) and
// Ctrl+V is 33040 (-> Cmd_OnEditPastebrush -> XYWnd_PasteClip ->
// RadiantClipboard_Paste -> Map_ImportBuffer), and shakeout G already hung
// `KiwiCmd_AfterPaste` on the paste id so the pasted solids land SELECTED and in a
// PAUSED Move (kiwi_command.cpp).  Construction geometry reached none of it: the
// store is KIWI's own and never enters `selected_brushes`, so Ctrl+C over a set of
// lines copied nothing and Ctrl+V pasted nothing.
//
// This file is the construction half, wired to the SAME two ids so there is one
// Copy and one Paste in the editor and no third key to learn.
//
// ═════════════════════════════════════════════════════════════════════════════
//  WHY NOT THE OS CLIPBOARD
// ═════════════════════════════════════════════════════════════════════════════
// The brush path does use it (`RadiantClipboard_Copy` puts a .map fragment on the
// Windows clipboard, entity.cpp), so "do not abuse the OS clipboard unless the
// brush path does" would permit it.  It is still the wrong home here, for a reason
// that has nothing to do with taste:
//
//   A MIXED COPY NEEDS BOTH PAYLOADS AT ONCE.  A selection can legitimately hold
//   brushes AND construction objects (kiwi_conselect.h: the two selections are
//   PARALLEL, both may be non-empty).  One OS clipboard slot means the second
//   writer stomps the first, so a mixed Ctrl+C would silently lose one half.  Two
//   independent stores lose nothing and need no format negotiation.
//
// ACCEPTED COST, stated: the construction clipboard is PROCESS-LOCAL, so
// construction geometry does not travel between two running copies of the editor.
// Brushes still do, because their half is unchanged.
//
// ═════════════════════════════════════════════════════════════════════════════
//  WHAT A PASTED OBJECT IS
// ═════════════════════════════════════════════════════════════════════════════
// A VALUE COPY of the stored `kconObject_t`, in place (no offset — the brush paste
// does not offset either, and the Move that follows is how you place it), with two
// fields deliberately reset:
//
//   * `hidden`  -> false.  A paste is new scaffolding and new scaffolding you
//     cannot see is a paste that appears to have failed.  Hidden objects cannot be
//     selected in the first place (KiwiConSel_HideSelected clears the selection of
//     what it hid), so this only ever matters for a clipboard that outlived a Hide.
//   * `group`   -> -1.  ROUND W's group id is a HANDLE into the store's own table
//     (kiwi_construct.h); copying it would put a paste into a folder the user was
//     not looking at.  Ungrouped is the honest default and the outliner can move it.
//
// Everything else — type, points, closed, the parametric block, `segs`, `name` —
// is carried verbatim.  The store's own KiwiCon_Add then normalises and refits the
// plane exactly as it does for a drawn object, so a pasted object is
// indistinguishable from one that was drawn there.
//
// SIDECAR: nothing special.  A pasted object is an ordinary store object and
// persists through KiwiCon_SaveSidecar like any other (the directive's "they
// persist like any other object — no special casing").
//
// UNDO: ONE KiwiCon_UndoPush for the whole paste, before anything is added, per
// kiwi_construct.h ruling 2 — so one construction Ctrl+Z removes the whole paste
// and not one object of it.
//
// ═════════════════════════════════════════════════════════════════════════════
//  THE MOVE, AND THE MIXED CASE
// ═════════════════════════════════════════════════════════════════════════════
// A construction-only paste enters Move through the EXISTING path and no new one:
// `KiwiCmd_AfterPaste` starts KIWI_CMD_MOVE paused, `KiwiXform_CanMove` already
// answers true for a pure construction selection (kiwi_transform.cpp:3565 ->
// KiwiConSel_CanMove), and the Move command already has a construction arm
// (kiwi_transform.cpp:1146, `KiwiConSel_MoveBegin`).  So "paste auto-enters Move"
// is the same sentence for both kinds.
//
// A MIXED paste CANNOT be one Move gesture, and this is a framework fact rather
// than a decision made here: `KiwiConSel_CanMove()` requires the brush-side
// selection to be EMPTY, and the Move command reaches its construction arm only
// when `DominantKind` fails — i.e. only when there is nothing brush-side to move
// (kiwi_transform.cpp's arm states it in full).  Teaching Move to carry both would
// mean one gesture holding two baselines, two undo mechanisms (the ported brush
// snapshot and the construction store's own) and two cancel paths, which is a
// round of its own and not a paste's business.
//
// So a mixed paste PASTES BOTH — nothing is lost — and then REFUSES the auto-Move
// with one console line naming the two counts and telling the user which key gets
// them a gesture.  Refusing loudly is the only honest option: silently moving one
// half would leave the other half behind at the paste position, which is worse
// than not starting.
//
// See RADIANT_UX_DESIGN.md §70.
// ─────────────────────────────────────────────────────────────────────────────

// Copy every WHOLE selected construction object into the KIWI-side clipboard.
// REPLACES whatever was there.  Returns how many objects were stored; 0 means
// nothing construction-side was copied, and there are TWO such cases:
//   - the ported Copy took SOLIDS (`selected_brushes` non-empty) — the construction
//     clipboard is CLEARED so the two clipboards agree about what was copied;
//   - nothing at all was selected — BOTH clipboards are left untouched, the same
//     rule KiwiCmd_ClipCut applies brush-side.
// The body's reasoning is at kiwi_conclip.cpp:88-105.
// KIWI-UX (CLEANUP, A-1)
int KiwiConClip_Copy();

// Paste them into the store: one KiwiCon_UndoPush, visible and ungrouped, and the
// construction selection is REPLACED with exactly the pasted set.  Returns how many
// landed (0 when the clipboard is empty, which is a silent no-op so that an
// ordinary brush Ctrl+V says nothing about construction geometry).
int KiwiConClip_Paste();

// How many objects the LAST KiwiConClip_Paste landed, and CLEARS the count.
//
// It is a one-shot latch rather than a query of the live selection because
// `KiwiCmd_AfterPaste` — the only caller — is the tail on CLONE (33001) as well as
// on Paste, and Clone never touches this clipboard.  "Is anything selected
// construction-side" would make a stale line selection turn every Clone into the
// mixed refusal; "did THIS paste land lines" is the question that was meant, and
// only the paste knows the answer.
int KiwiConClip_TakeJustPasted();

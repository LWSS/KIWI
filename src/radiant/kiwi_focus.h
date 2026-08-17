#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_focus.h — ROUND J: FOCUS THE CAMERA ON THE SELECTION.
//
//     KIWI_CMD_FOCUS_SELECTION  34108   INSTANT, modern key "/"
//
// ── THE PLASTICITY SOURCE, AND THE KEY, READ RATHER THAN GUESSED ────────────
// Two DIFFERENT verbs in Plasticity are easy to confuse, and only one of them is
// "frame the selection":
//
//   "/"     `viewport:focus`                default-keymap.ts:327, Menu.ts:53
//           Viewport.tsx:568-571 —
//               focus() {
//                   const { solids, curves, regions, controlPoints, faces, edges }
//                       = this.editor.selection.selected;
//                   this.navigationControls.focus([...], visibleObjects);
//               }
//           i.e. it hands the ORBIT CONTROLS the selected objects and lets them
//           re-target and dolly.  The view DIRECTION is untouched.  Menu.ts:53
//           labels it, in full: "Focus camera on selected".
//
//   space   `viewport:navigate:selection`   default-keymap.ts:231
//           Viewport.tsx:156 → `this._navigate(cplanes.constructionPlaneForSelection(...))`
//           → _navigate (Viewport.tsx:558-566) REORIENTS the camera onto the
//           selection's construction plane AND swaps the active construction plane
//           AND may transition to ortho.  That is "look at this face square on",
//           not "frame this".
//
// KIWI SHIPS THE FIRST ONE, on Plasticity's own key.  The second is a separate
// verb with a separate cost (it owns the construction plane, which KIWI already
// has five commands for — KIWI_CMD_CPLANE_*), and mapping it onto the SPACE key
// would collide head-on with classic Radiant's Clone (33001, mainfrm.cpp:1055),
// which is one of the most-pressed keys in the whole editor.  It is not shipped
// and it is not half-shipped; see the round's report.
//
// ── WHAT GETS FRAMED ────────────────────────────────────────────────────────
// Plasticity's focus() takes EVERY selected thing regardless of kind, and falls
// back to the whole visible scene when nothing is selected (NavigationControls
// receives `visibleObjects` as its second argument for exactly that).  KIWI does
// the same, over the three things this editor can have selected:
//
//   1. the LEGACY brush selection — Select_GetBounds (select.cpp:2056), the AABB
//      over every selected brush def.  This is the common case and it is one call.
//   2. the KIWI CONSTRUCTION selection (kiwi_conselect.h) — the selected objects'
//      tessellated vertices.  Construction geometry lives outside selection_t
//      (kiwi_construct.h scope ruling 1), so it has to be asked separately, and
//      leaving it out would mean "/" did nothing at all in a pure construction
//      workflow.
//   3. NOTHING SELECTED → the whole visible world: every brush in either display
//      list that is not hidden, plus every construction object.  That is
//      Plasticity's `visibleObjects` fallback, and it makes "/" the "where IS
//      everything" key as well as the "look at this" key.
//
// FACE / EDGE / VERTEX selections are framed through their OWNING BRUSH's box
// rather than through the sub-element, because Select_GetBounds is per-brush and
// a sub-element selection always syncs its owner into selected_brushes
// (kiwi_selection.h Sel_SyncToLegacy).  Framing the whole brush when one of its
// faces is selected is also the more useful answer — the face is on screen either
// way, and its neighbours are the context you wanted.
//
// ── HIDDEN BRUSHES ARE EXCLUDED FROM THE FALLBACK, NOT FROM THE SELECTION ───
// The no-selection fallback skips brushes carrying the hidden bit
// (kiwi_visibility.h), because framing geometry you cannot see is framing
// nothing.  A brush that is BOTH hidden and selected is still framed, because the
// user named it explicitly and Select_GetBounds — the ported core — does not
// filter either.
//
// ── UNDO: NONE ─────────────────────────────────────────────────────────────
// It moves the camera.  Per kiwi_command.h ("a command that mutates nothing must
// NOT open a bracket at all") there is no bracket and no journal ticket, exactly
// as for the view-cube and the fly.  Plasticity's focus is likewise not a Command
// and never enters its history (it is a `viewport:` action on the Viewport, not a
// `command:` on the executor — Viewport.tsx:157).
// ─────────────────────────────────────────────────────────────────────────────

// The AABB the command would frame right now.  False when there is nothing at all
// to look at (an empty map with no construction geometry).  Exposed because the
// palette predicate and the command share it and a second copy would drift.
bool KiwiFocus_SelectionBounds( float mins[3], float maxs[3] );

// §3 palette predicate — greys the row on an empty world.
bool KiwiFocus_CanFocus();

// ── KIWI-UX (ROUND BK, ITEM 7): SPACE — THE CAMERA GOES IN FRONT OF A FACE ──
//
//     KIWI_CMD_VIEW_FACE  34132   INSTANT, modern key Space
//
// USER DIRECTIVE, verbatim: *"Pressing [Space] on a face should mimic what
// Plasticity does.  It moves the camera in front of that face (similar to /)."*
//
// THIS IS THE VERB THE HEADER ABOVE SAYS WAS NOT SHIPPED.  Round J identified it
// correctly — Plasticity's `viewport:navigate:selection`, space,
// default-keymap.ts:231 — and declined it because Space was classic Radiant's
// Clone (33001).  The user has asked for the key by name, so the MODERN keymap
// moves Clone to Shift+Space (kiwi_keymap.cpp carries the audit) and the classic
// profile is untouched.  KIWI ships the CAMERA half only: unlike Plasticity's, it
// does not swap the construction plane (KIWI has five explicit commands for that)
// and does not change the projection.
//
// THE MATH IS `/`'S.  The face's winding AABB goes through the same
// KiwiCam_FrameBounds fit (margin, radius floor, the smaller of the two tangents),
// after a KiwiCam_LookAlong that points the view axis down -normal.  So a face
// whose normal is a world axis lands on that axis view exactly, and an angled face
// is viewed square-on.
//
// WHICH FACE: the ACTIVE selected face, else the first selected face, else the one
// under the cursor.  Nothing selected and nothing hovered prints a line naming
// FACE mode.
bool KiwiFocus_CanViewFace();      // §3 palette predicate

void KiwiFocus_RegisterCommands();
bool KiwiFocus_DispatchInstant( unsigned int cmdId );

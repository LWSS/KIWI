#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_boxselect.h — RADIANT_UX_DESIGN §12: directional box selection.
//
//   left -> right : CONTAINMENT (only what is fully enclosed)
//   right -> left : CROSSING    (anything the rect touches)
//   Shift adds, Ctrl removes, plain replaces — the same modifiers as a click.
//   A drag under KBOX_CLICK_PIXELS is a CLICK: one Pick() with the same modifiers.
//
// Granularity comes from KiwiSel_GetModeMask() and follows the same
// point-beats-line-beats-area ranking kiwi_pick.cpp applies, resolved ONCE for the
// whole rect (mixing kinds inside one marquee would select a brush and its own
// faces and its own verts at the same time):
//   OBJECT bit set  -> whole objects  (modes 4 and 5)
//   else FACE       -> faces          (mode 3)
//   else EDGE       -> edges          (mode 2)
//   else            -> vertices       (mode 1)
//
// Everything is camera-RTT-image space, TOP-LEFT origin (kiwi_pick.h's convention).
// This layer is gated by the modern-input master toggle at its ONE dispatch point
// in the shell; with the toggle off, camera LMB is the ported drag-select verbatim.
// ─────────────────────────────────────────────────────────────────────────────

// ── ROUND N: 4 -> 8 ─────────────────────────────────────────────────────────
// USER REPORT, verbatim: "it's not possible to select multiple solid faces at once
// with shift-clicking."  One of the two mechanisms behind that is a HAND WOBBLE,
// and it is silent by construction: KiwiBox_End treats a press-to-release as a
// CLICK only while BOTH axes stay under this number, so a Shift+click that drifted
// five pixels LEFT-TO-RIGHT became a five-pixel CONTAINMENT marquee — which by
// definition wants a whole face inside it, found nothing, and (because Shift
// suppresses the clear) changed nothing at all.  The user sees a click that did
// not work.  A right-to-left wobble was fine, because a CROSSING rect has the
// centre-ray fallback; the containment direction has no such net, and giving it one
// would change what "containment" means for real marquees.
//
// 8 px is still well inside a deliberate drag (nobody box-selects an 8-pixel
// region — the smallest useful marquee is tens of pixels) and it is the ordinary
// desktop click-slop figure: Win32's own SM_CXDRAG default is 4 px in EACH
// direction from the press, i.e. an 8-px box, which is exactly this.
#define KBOX_CLICK_PIXELS 8

void KiwiBox_Begin ( int imgX, int imgY, bool shift, bool ctrl );
void KiwiBox_Update( int imgX, int imgY );

// Resolve the marquee (or the click) into KiwiSel(), then Sel_SyncToLegacy() and
// invalidate the views.  Ends the gesture either way.
void KiwiBox_End   ( int imgX, int imgY );

// Force-abort: drop the marquee, change nothing.  Used by the shell's stuck-drag
// teardown, which must run the owner's OWN teardown and never pre-empt a normal
// release edge.
void KiwiBox_Cancel();

bool KiwiBox_Active();

// The live rect for the screen-space overlay.  x0/y0 is the PRESS point and x1/y1
// the current cursor (not normalised — the caller draws the direction), and
// `crossing` is true for a right-to-left drag.
bool KiwiBox_Rect( int *x0, int *y0, int *x1, int *y1, bool *crossing );

// ── ROUND AG, ITEM 6: the LIVE marquee preview ──────────────────────────────
// USER DIRECTIVE: "While box selecting, it should highlight the items in
// realtime as the box goes over them (quality of life)."  Cam_Draw tail hook
// (// KIWI-UX in camwnd.cpp).  Emits nothing when no marquee is live or the drag
// is still inside KBOX_CLICK_PIXELS.  It READS state only — it cannot change
// what the release selects.  The re-collect throttle, the skipped centre-ray
// rescue, why it draws outlines rather than fills, and the Plasticity source it
// mirrors are all written out at the definition in kiwi_boxselect.cpp.
void KiwiBox_DrawPreview();

// ── ROUND K: THE CLICK GRAMMAR, EXPORTED ────────────────────────────────────
// KiwiBox_End's click path, callable directly.  It exists because ROUND K gives
// the auto-entered face push/pull a plain-click rung of its own
// (kiwi_command.h IdlePressReselect, kiwi_transform.cpp): while that gesture is
// PAUSED and has moved nothing, an LMB press means "select that instead", and
// "that" has to go through the SAME arbitration a click anywhere else does —
// construction geometry vs regions vs brush geometry, the shift/ctrl grammar, and
// the mode-3 face auto-enter at the end of it.
//
// Re-deriving any part of that at the transform's call site would have been a
// second click grammar in the editor, and the two would disagree the first time
// either changed.  The caller is responsible for having ENDED its own gesture
// first: the auto-enter at the tail of this refuses while a command is live.
void KiwiBox_ClickSelectAt( int imgX, int imgY, bool shift, bool ctrl );

// ── ROUND AA, ITEM 9: THE RECT'S BRUSH PASS, EXPORTED — WITHOUT THE SELECTION ─
// USER REPORT, verbatim: "the difference command needs to accept multiple
// shift-clicked brushes. […]  Also support shift box select."
//
// A modal command that BUILDS A SET (kiwi_boolean.cpp's tool set) needs the one
// question this file already answers — "which brushes are in this rectangle" —
// and needs NOTHING else this file does: it must not clear KiwiSel(), must not
// Sel_SyncToLegacy, must not auto-enter push/pull and must not touch construction
// geometry or regions.  KiwiBox_End is all of those things wrapped around the one
// question, so the question is exported instead of the gesture.
//
// The granularity is FORCED TO WHOLE OBJECTS, unlike KiwiBox_End's, which follows
// KiwiSel_GetModeMask().  The caller is a solid-level verb: in Face or Edge mode
// the mode-driven answer would be faces and edges, which such a verb cannot use at
// all — and a mapper in face mode who drags a box around three brushes to boolean
// them means the three brushes.
//
// x0/y0 and x1/y1 are camera-RTT image space in EITHER order (normalised inside);
// `crossing` picks touch-tests over containment, the same §12 rule the marquee
// uses, INCLUDING the centre-ray rescue for a rect that lies wholly inside one big
// face.  Returns how many pointers were written, clamped to maxOut.  Every
// returned node is live at the moment of the call and must still be run through
// Sel_BrushLive before a later deref (kiwi_selection.h LIVENESS).
struct selbrush_t;                // qe3.h:429 (the 56-byte brush INSTANCE / list node)
int KiwiBox_CollectBrushes( int x0, int y0, int x1, int y1, bool crossing,
                            selbrush_t **out, int maxOut );

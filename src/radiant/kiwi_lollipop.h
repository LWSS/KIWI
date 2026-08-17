#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_lollipop.h — ROUND K: the Plasticity EXTRUDE HANDLE.
//
// USER DIRECTIVE, verbatim: "when clicking a face on a solid(brush), it should
// auto enter the extrusion mode and it should look like a lollipop (see pic).
// The lollipop should also stay external to the face, it should move with the
// face so it doesn't get buried after a grab.  Also hide the move gizmo when
// extruding.  After confirming an action, the part should be de-selected as
// well."  The picture is Plasticity's extrude gizmo: a thin white circle sitting
// on the face, a short stem coming straight out of its centre along the normal,
// and a yellow ball on the end of the stem.
//
// ── WHAT IT REPLACES, AND WHERE ─────────────────────────────────────────────
// The three-arrow translate gizmo (kiwi_gizmo.cpp), for exactly three contexts:
// face push/pull (Move over a face selection), region extrude and face extrude.
// Those are the gestures with ONE degree of freedom, and a six-handle translate
// gizmo on a one-axis push is five handles that do nothing plus one — the amber
// normal arrow shakeout G had to add — that does the job badly.
//
// The gizmo does not merely go unused: kiwi_gizmo.cpp's GizmoUsable() refuses
// outright while a lollipop is wanted, so it is neither DRAWN nor HIT-TESTED.
// That second half is load-bearing — kiwi_viewport.cpp offers an LMB press to the
// gizmo BEFORE the command (kiwi_viewport.cpp's ordering note), so a gizmo that
// stayed hit-testable would swallow presses aimed at the ball.
//
// ── THE HANDLE RIDES THE FACE ───────────────────────────────────────────────
// "it should move with the face so it doesn't get buried after a grab".  The
// anchor is recomputed FROM THE COMMAND EVERY FRAME — the command answers with
// where its face/region centroid is NOW, i.e. including the push applied so far
// (KiwiEditorCommand::LollipopHandle) — so the circle stays on the moving face
// and the stem stays the same length in front of it.
//
// AND IT FLIPS.  A push/pull that goes NEGATIVE moves the face into the solid; a
// stem that kept pointing along the outward normal would then be sticking out of
// the far side, and the ball would end up inside the brush the user is looking at.
// So the command reports the DIRECTION with the push's sign already folded in,
// and the stem always leaves the face on the side the user is dragging toward.
//
// ── THE GRAB DOES NOT MOVE ANYTHING ─────────────────────────────────────────
// Taking hold of the ball calls Rebase() on the command (through the ordinary
// PAUSED→HOT resume, or explicitly when the gesture was already hot), so the
// mapping is re-latched AT THE PRESS PIXEL and the geometry does not jump by
// however far the cursor wandered since the command started.  That is the same
// guarantee kiwi_gizmo.cpp's shakeout-G arming sequence gives the move handles;
// the general case for the three-arrow gizmo is round L's.
//
// ── SIZES (screen-constant, like every other handle in this layer) ───────────
// KIWI-UX (CLEANUP, C-46): synced to the SHIPPED values.  These were the pre-round-U
// numbers (18 / 48 / 7); round U restyled the glyph and kiwi_lollipop.cpp's
// KLOL_*_PIX block records size by size why.
//   ring   14 px radius, drawn IN THE FACE PLANE (not camera-facing) so it reads
//          as lying on the surface, which is what the picture shows.
//   stem   42 px along the (signed) normal.
//   ball    5 px radius — the directive's "~10 px" across.
//   pick   12 px around the ball's projected centre (unchanged by round U — it is
//          a fingertip target, not a drawn thing).
// ─────────────────────────────────────────────────────────────────────────────

// True when the ACTIVE command wants a lollipop; fills the live anchor (world)
// and the signed outward direction (unit).  This is the ONE predicate — the draw,
// the hit test and kiwi_gizmo.cpp's stand-down all ask it.
bool KiwiLollipop_Wanted( float outAnchor[3], float outDir[3] );

// Convenience for the gizmo's gate: "is a lollipop wanted at all".
bool KiwiLollipop_Active();

// Cursor tracking, from kiwi_viewport.cpp's hover arm.  `over` false clears.
void KiwiLollipop_Hover( int imgX, int imgY, bool over );

// The LMB press.  True = the ball was taken (the caller owns the gesture until
// the release).  Resumes a PAUSED command and Rebase()s it first — see above.
bool KiwiLollipop_MouseDown( int imgX, int imgY );

void KiwiLollipop_Release();      // release edge: ungrab, then PAUSE the gesture
void KiwiLollipop_Abort();        // lost-capture edge: ungrab, then CANCEL it

// Cam_Draw tail hook (called from the same block as KiwiGizmo_DrawWorld).
void KiwiLollipop_DrawWorld();

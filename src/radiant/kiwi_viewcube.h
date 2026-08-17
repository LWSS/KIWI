#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_viewcube.h — the orientation widget (shakeout A, rebuilt in shakeout D).
//
// A Blender/Plasticity-style view cube in the TOP-RIGHT of the camera image: an
// actual CUBE — eight corners projected through the CURRENT camera basis, six
// filled quads painter-sorted by face normal, the visible faces labelled
// (X / -X / Y / -Y / TOP / BOT).  Clicking a visible FACE snaps the view to look
// along that axis; clicking a visible CORNER snaps to the isometric view from
// that corner.  Either way the orbit pivot and distance are kept — the camera
// moves onto the new direction, it does not teleport to the origin.
//
// ── IT USED TO BE SIX BALLS ─────────────────────────────────────────────────
// Shakeout A shipped six balls on spokes and argued a real cube was not worth the
// back-face ordering and the hit regions.  USER DIRECTIVE, shakeout D: "the
// 'blender cube' you gave me is ugly, needs to be an actual cube."  It is one
// now, and the argument turned out to be wrong on both counts: the sort is six
// dot products (kiwi_viewcube.cpp THE PAINTER SORT) and the corner views ARE
// reachable — a yaw/pitch camera with no roll expresses every corner view fine,
// which the general snap derivation makes free.
//
// ── SO THERE ARE 14 SNAP TARGETS, NOT 6 ─────────────────────────────────────
// Six faces plus eight corners.  EDGE targets (the twelve 45-degree views) are
// deliberately NOT wired: they would need a third hit region between the corner
// discs and the face quads on a 104-px widget, and an edge view is the one thing
// nobody reaches for.  Logged as future work rather than crammed in.
//
// ── DRAWN WITH ImDrawList ONLY ──────────────────────────────────────────────
// KIWI-UX (CLEANUP, C-37): the rule this used to state — "no ImGui items" — is
// not the rule, and the chips it cited as the precedent break it: DrawChips emits
// five real ImGui::Buttons inside the camera image.  THE INVARIANT IS: no trailing
// SetCursorScreenPos with no item after it, and no item that changes the window's
// CONTENT EXTENT after the image.  Either trips ImGui::End's
// ErrorCheckUsingSetCursorPosToExtendParentBoundaries (the assert class fixed in
// eead8b7) or feeds a scrollbar loop.  kiwi_viewport.cpp's DrawChips carries the
// canonical statement, in its "NO cursor restore here" block.
//
// This widget goes further than the invariant needs and emits NO items at all, so
// it does its OWN hover and click resolution against io.MousePos, and reports
// hover back so the shell can drop the image's hover for the frame (exactly what
// DrawChips returns for).
//
// Clicks are resolved DURING the frame rather than in the post-present dispatch
// because setting camera angles pops no modal and touches no message pump — the
// post-present rule exists for TrackPopupMenu, not for this.  It is the same
// place the chips resolve theirs.
//
// ── ROUND R: THE ONE EXCEPTION, AND WHY IT DOES NOT BREAK THE RULE ──────────
// The grid pill's TYPE-IN (the directive "let me type in the grid spacing
// manually") is a real ImGui InputText.  The rule above is about items IN THE
// CAMERA WINDOW — an item there moves that window's cursor, and the End() that
// follows the overlay is what turns a moved cursor into the
// ErrorCheckUsingSetCursorPosToExtendParentBoundaries assert.  The type-in lives
// in its OWN top-level window (a nested Begin/End pair, which ImGui saves and
// restores per window), so the camera window's cursor is untouched and the assert
// class stays fixed.  Everything else in the cluster — the cube, the projection
// pill, the grid pill's three zones — is still ImDrawList-only.
//
// ── ROUND M: THE PROJECTION PILL SHARES THIS WIDGET'S RULES ─────────────────
// The stated known limit here used to be "there is no orthographic projection in
// this camera, so the persp label is a readout, not a toggle."  There is one now
// (kiwi_camera.h ORTHOGRAPHIC / PERSPECTIVE TOGGLE), and the control is a small
// ORTHO/PERSP pill drawn directly under the cube's backdrop by this same
// function.  It follows every rule above — ImDrawList only, own hover, own click,
// hover reported back — and it is drawn OUTSIDE the KiwiViewCube_Show() gate, so
// turning the orientation cube off does not also hide the projection control.
// ─────────────────────────────────────────────────────────────────────────────

// Draw + resolve, from KiwiVP_DrawCameraOverlay.  Returns true when the cursor is
// over the widget OR over the projection pill, so the caller can take the image's
// hover for this frame.
bool KiwiViewCube_Draw( float imgMinX, float imgMinY, float imgW, float imgH );

// Own on/off switch (persisted, default ON).  A pure addition; not gated by the
// modern-input master toggle.
bool KiwiViewCube_Show();
void KiwiViewCube_SetShow( bool on );

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND P — ALT + MIDDLE CLICK: STEP THROUGH THE CUBE'S SIX FACE VIEWS
// ═════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: "add the alt-middle click shortcut that plasticity has
// to cycle through the stages of the camera cube. (look it up)."
//
// ── LOOKED IT UP.  PLASTICITY HAS NO SUCH BINDING, AND HERE IS THE PROOF ────
// Middle-click is `mouse1` there (KeyboardEventManager.ts:131-143 builds the
// keystroke from MouseEvent.button, and modifiers() at :166-174 prefixes "alt-"),
// and the DEFAULT keymap binds exactly two mouse chords in the orbit scope:
//     "orbit-controls": { "mouse1": "orbit:rotate", "mouse2": "orbit:pan" }
//                                              (default-keymap.ts:330-333)
// OrbitControls.onMouseDown (OrbitControls.ts:362-392) looks the keystroke up in
// that table and falls to `default: state = {tag:'none'}` for anything else — so
// bare "alt-mouse1" does NOTHING in stock Plasticity.  Nor can it reach a command:
// KeyboardEventManager.ts:56 converts ONLY RMB into a command keystroke, and
// ViewportControl.ts:97 refuses every button but LMB.  `alt-mouse1` acquires a
// meaning only in the optional orbit PRESETS the user can switch to — pan in the
// Maya preset, rotate in the 3ds Max one (ConfigFiles.ts:101-122).
//
// What Plasticity DOES have is the six named axis views the nav cube and the
// numpad share — `viewport:navigate:front|right|top|back|left|bottom`
// (Viewport.tsx:150-156, bound at default-keymap.ts:218-231, and called by the
// cube itself at ViewportNavigator.ts:136).  That IS "the stages of the camera
// cube", so the directive is honoured with those six, on the chord the user asked
// for — which is free in the default keymap, exactly as the trace above shows.
//
// ── WHAT THE CHORD DOES ─────────────────────────────────────────────────────
// Alt + MMB CLICK (a press that never travels past the drag threshold — a drag is
// still an orbit, and the orbit is deferred until that threshold so the click can
// never nudge the view):
//   * not currently on a face view  ->  snap to the NEAREST one to where you are
//     already looking (so the first press never spins the view somewhere unrelated)
//   * already on one                ->  advance to the NEXT in the ring
//                                       front -> right -> back -> left -> top ->
//                                       bottom -> front
// Either way the pivot and the reference distance are kept: this routes through
// the SAME LookAlongDirection the cube's own face clicks use, so the yaw lattice,
// the pole handling and the round-N spin reset are shared code, not a second copy.
void KiwiViewCube_StepAxisView();

// ── ROUND Y, ITEM 4: is the camera sitting ON one of the six axis views? ────
// USER DIRECTIVE: "the line tool is still way out of whack.  IT doesn't respect
// the camera angle.  When snapping to top or bottom it is IMPOSSIBLE for me to
// represent a Z direction.  … (see how plasticity does it)."
//
// Plasticity binds the construction plane to the view when you navigate to an
// axis view and un-binds it on the first orbit (Viewport.tsx:558-566 /
// :411-417).  KIWI reads the same fact live off the camera instead of latching
// it — the full argument, with the Plasticity citations, is on the definition in
// kiwi_viewcube.cpp.  Threshold is KVC_VIEW_ALIGNED, this file's own cos(3 deg).
//
//   outAxis  the WORLD AXIS the camera looks along: 0 = X, 1 = Y, 2 = Z.  That is
//            also the NORMAL of the plane the user is facing, which is exactly
//            what KiwiCon_SetPlaneAxis wants.
//   outSign  +1 / -1: which way along it.  Top and bottom are the same PLANE and
//            different views, as they are in Plasticity (PlaneDatabase.XY vs
//            _XY, PlaneDatabase.ts:17-22), so callers that only want the plane
//            can ignore this.
// Either pointer may be null.  False = the camera is not on an axis view.
bool KiwiViewCube_ViewAxis( int *outAxis, float *outSign );

// The name of that view ("front" / "right" / "back" / "left" / "top" / "bottom"),
// or NULL when the camera is not on one.  For console lines only.
const char *KiwiViewCube_ViewAxisName();

// ── ROUND S: ALT+MMB IS A SWIPE ─────────────────────────────────────────────
// USER DIRECTIVE, verbatim: "The alt-MMB shortcut should be more of a swipe, it's
// hard to actually do a standstill press of mmb."
//
// Round P made Alt+MMB a CLICK (travel under 4 px) and gave the drag back to the
// orbit, which is exactly the standstill press the directive is about.  ROUND S
// gives the WHOLE Alt+MMB gesture to the view instead: it never orbits (bare MMB
// still does, unchanged), and the RELEASE reads the travel:
//
//   travel  <  KVP_SWIPE_PIXELS   ->  KiwiViewCube_StepAxisView()  (round P's
//                                     nearest-then-ring behaviour, unchanged)
//   travel  >= KVP_SWIPE_PIXELS   ->  KiwiViewCube_SwipeAxisView( dominant axis )
//
// THE MAPPING, and it is the ORBIT's own directions quantised to 90 degrees — so a
// swipe is literally "the drag I was about to do, snapped".  KiwiCam_OrbitDrag
// (kiwi_camera.cpp) uses dyaw = -dx * k and pitch = pitch0 - dy * k, i.e. the model
// follows the mouse on both axes, which is also what Blender's turntable does.
// Starting from the face view NEAREST the current camera (so a swipe from an
// orbited view lands somewhere predictable):
//
//   SWIPE RIGHT   yaw -90    front -> left -> back -> right -> front
//   SWIPE LEFT    yaw +90    front -> right -> back -> left -> front
//   SWIPE DOWN    pitch -90  see the WRAP below
//   SWIPE UP      pitch +90  the same cycle, walked backwards
//
// ── ROUND U: THE VERTICAL SWIPE WRAPS; THE POLES ARE NOT TERMINAL ──────────
// USER DIRECTIVE, verbatim: "fix the viewport swiping (alt - mmb) so you can wrap
// around however you like."  Round S's vertical mapping (above, as shipped) was a
// LADDER with two ends, and the console said so: "already bottom - swipe the other
// way to come back".
//
// It is a GREAT CIRCLE now, walked indefinitely in either direction:
//
//     ref side --down--> TOP --down--> opposite side (yaw+180)
//              --down--> BOTTOM --down--> ref side --down--> ...
//
// i.e. from front:  front -> top -> back -> bottom -> front -> ...
// and UP is that read backwards, so a swipe and its opposite always undo each
// other.  The HORIZONTAL ring is unchanged.
//
// It carries two ints of phase state, and it has to: the quantity that
// distinguishes "front, having come over the top" from "back, clicked on the cube"
// is ROLL, and this camera has none (pitch + yaw only, pitch clamped to +-89).  The
// phase is VALIDATED against the live camera on every swipe and resynced from it on
// any disagreement, so a cube click / orbit / horizontal swipe / pole spin can
// never leave it pointing at a circle the user has left.  kiwi_viewcube.cpp's
// ValidateVertPhase carries the full argument.
//
// A horizontal swipe while ALREADY on a pole SPINS that pole view by 90 degrees
// (the pitch is kept, the yaw steps), which is what the same flick does in Blender
// and is the only reading that is not a no-op.
//
// `dir`: 0 = left, 1 = right, 2 = up, 3 = down.
void KiwiViewCube_SwipeAxisView( int dir );

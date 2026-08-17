#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_section.h — KIWI-UX (ROUND BM, ITEM 1b): SECTION ANALYSIS.
//                  KIWI-UX (ROUND BP, ITEM 1): rebuilt on the PROJECTION route.
//
//     KIWI_CMD_SECTION_TOGGLE  34133   INSTANT, unbound (the button + the palette)
//
// USER DIRECTIVE, verbatim: *"Get rid of this arrow key cross section feature,
// it's awful.  Instead, copy the Z down cross section feature from Plasticity.
// It's a button above the cube and it allows you to set a cross section Z level by
// selecting something (usually a plane), and then pushing/pulling a lollipop until
// it's where you want it.  You can remove the cross section effect by hitting the
// button again."*
// ROUND BP, verbatim: *"Section view still broke, NO PROBES went off.  Honestly
// you had it working earlier before I told you to do the arrow key refactor.  I
// just wanted it on the Z axis instead of far plane."*
//
// ── WHY THE D3D9 USER CLIP PLANE IS GONE (the round-BP autopsy, in one place) ─
// Round BO instrumented the whole chain and the evidence finally came back, in
// %TEMP%\radiant_firstlight.log, from the user's own session:
//
//   SECTION latch: n=(0.000 0.000 1.000) d=170.001 anchor=(29.6 -1343.5 170.0)
//   SECTIONPROBE: world=(-0 -0 -1 170.001) clip=(1.87615e-05 -691.191 668649
//                 -335355) SetClipPlane=0x00000000 SetRS=0x00000000
//                 GetRS=0x00000000 readback=1 gfxMetrics.maxClipPlanes=6
//
// Every HRESULT is S_OK, D3DRS_CLIPPLANEENABLE READS BACK AS D3DCLIPPLANE0, and
// the device advertises six user clip planes — and the view was not cut.  That is
// the D3D9 verdict on user clip planes under a PROGRAMMABLE vertex shader: the
// runtime accepts the state and the driver declines to patch the shader for it.
// There is nothing left to fix on that route, so the section does not use it.
// RC_SET_CLIP_PLANE, RB_SetClipPlaneCmd and the BO probe all stay in the tree
// (they are correct, and the probe is the only proof of the above), but nothing
// calls them any more.
//
// ── THE MECHANISM NOW: AN OBLIQUE NEAR PLANE IN THE PROJECTION MATRIX ────────
// The user's other data point is the one that chose this: round BK's ORTHO SLAB —
// a projection-matrix near-plane manipulation — visibly worked on their device.
// So the cut is folded into the projection, the standard Lengyel oblique-near
// technique, derived for THIS tree's row-vector convention in
// KiwiSection_ObliqueDepthColumn's own comment block and in RADIANT_UX_DESIGN §36b.
// It needs no device feature at all: it is four floats in the matrix every draw
// already multiplies by.
//
// ── THE PLANE IS ALWAYS A HORIZONTAL Z LEVEL ────────────────────────────────
// The directive said "Z down" and the re-report said "on the Z axis".  So there is
// exactly one number: `level`.  Everything with world z > level is cut away;
// clicking anything sets the level to that point's z; the lollipop slides it.  No
// arbitrary-normal planes — the internal maths keeps a general plane form because
// the derivation is written that way, but the only plane the UX can produce is
// (0,0,1, level).
//
// ── THE THREE STATES ────────────────────────────────────────────────────────
//   OFF      nothing is drawn, nothing is cut, nothing is clamped.
//   PICKING  the button was pressed: the next LMB in the camera image sets the
//            level.  Escape cancels back to OFF.
//   ON       the level is live.
//
// ── ONE STATE FOR THE CUT AND FOR THE PICK CLAMP (ROUND BP, ITEM 3) ─────────
// The round-BP autopsy of the "snap teleport" found that the section's PICK CLAMP
// (KiwiSection_ClampRayStart) was live whenever the mode was ON, while the cut
// itself depended on a device feature that did nothing — so a section could be
// armed, invisible, and silently advancing every pick ray in the editor.  That
// cannot happen again by construction: BOTH the clamp and PointVisible are gated
// on KiwiSection_Cutting(), which is true only when THIS FRAME'S CamWnd_SetupScene
// actually installed the oblique projection.  If the cut is not happening, the
// picker does not know the section exists.
//
// ── WHEN THE FOLD IS REFUSED, AND WHY THAT IS HONEST ────────────────────────
// An oblique near plane replaces the depth function with "distance from the
// section plane", so depth ORDERING survives only while that function is monotonic
// along a pixel's ray.  Both conditions fall straight out of the algebra (the
// derivation shows the working):
//   * PERSPECTIVE: z_ndc = const(pixel) + a*Cw/f, so ordering needs Cw < 0 — THE
//     EYE MUST BE ABOVE THE CUT.  Which is what a section is for.
//   * ORTHO: z_clip is affine in f with slope a*Cf, so ordering needs Cf > 0 — the
//     camera must be looking DOWNWARD at all.  An exactly level ortho view (FRONT /
//     SIDE) has Cf == 0 and no depth at all, and is the one refusal.
// A refused frame draws uncut, says so once, and — per the paragraph above — also
// turns the pick clamp off, so the editor is never in a half-sectioned state.
// ─────────────────────────────────────────────────────────────────────────────

struct ray_t;          // kiwi_pick.h — forward-declared to keep this header light

// ── the command ─────────────────────────────────────────────────────────────
void KiwiSection_RegisterCommands();
bool KiwiSection_DispatchInstant( unsigned int cmdId );

// The one mutator the view-cube button drives.  OFF -> PICKING -> (a click) -> ON,
// and ON -> OFF.  Same entry point as the command, so the button and the palette
// row can never diverge.
void KiwiSection_Toggle();

// ── state ───────────────────────────────────────────────────────────────────
bool  KiwiSection_Active();     // ON: a level is set and the camera wants to cut
bool  KiwiSection_Picking();    // PICKING: waiting for the click that sets the level
float KiwiSection_Level();      // the Z level (meaningless while OFF)

// THE gate for everything downstream of the render: true only while the oblique
// projection was actually installed for the frame being drawn / picked into.  The
// pick clamp, PointVisible and the "SECTION" readout all key on this.
bool KiwiSection_Cutting();

// Reset to OFF and drop every derived flag.  Called by the toggle, by Escape, and
// by the new-map path — a section is view state and must not survive a document.
void KiwiSection_Reset();

// Escape while PICKING cancels the activation and consumes the key.  Returns false
// in every other state, so Escape keeps its ordinary meaning.
bool KiwiSection_HandleEscape();

// ── input, from kiwi_viewport.cpp ───────────────────────────────────────────
// The LMB press while PICKING: sets the level from whatever is under the cursor and
// goes to ON.  True = the press was consumed.
bool KiwiSection_ClickPick( int imgX, int imgY );

// The lollipop ball.  MouseDown returns true when the ball was taken (the caller
// owns the gesture until the release), and re-bases the drag at the press pixel so
// the level does not jump by however far the cursor wandered — the same grab-rebase
// discipline kiwi_lollipop.h states for the command handles.
bool KiwiSection_HandleDown( int imgX, int imgY );
void KiwiSection_HandleDrag( int imgX, int imgY );
void KiwiSection_HandleUp();
void KiwiSection_HandleAbort();          // lost-capture: restore the slide at the grab

// Cursor tracking for the ball's hover highlight.  `over` false clears.
void KiwiSection_Hover( int imgX, int imgY, bool over );

// ── render ──────────────────────────────────────────────────────────────────
// ROUND BP: the clip-plane bracket is RETIRED.  Both entry points survive as
// no-ops so camwnd.cpp's call sites need no edit and so the RC_SET_CLIP_PLANE
// route stays reachable for a future round; neither can arm anything.
void KiwiSection_EmitClipBegin();
void KiwiSection_EmitClipEnd();

// THE PROJECTION FOLD.  Called from CamWnd_SetupScene with the numbers that built
// the base projection; writes the four floats of the projection's DEPTH COLUMN
// (m[0][2], m[1][2], m[2][2], m[3][2]) and returns true when it did.
//
// False means "leave the base projection alone": the section is off, or the
// ordering conditions above are not met (in which case it has already said so).
// The caller must ALSO report the answer through KiwiSection_NoteFrameCut so the
// pick clamp tracks the render exactly.
//
//   ortho       which arm built the base projection
//   guardC      the 0.99951171875 guard-band constant both arms carry
//   tanX/tanY   perspective half-extents at unit depth   (perspective only)
//   zNear       the perspective near plane                (perspective only)
//   halfW/halfH the ortho half-extents                    (ortho only)
//   depthHalf   the ortho slab half-depth                 (ortho only)
bool KiwiSection_ObliqueDepthColumn( const float origin[3], const float vpn[3],
                                     const float vright[3], const float vup[3],
                                     bool ortho, float guardC,
                                     float tanX, float tanY, float zNear,
                                     float halfW, float halfH, float depthHalf,
                                     float outCol[4] );

// The frame's verdict, recorded by CamWnd_SetupScene: true when the fold went in.
void KiwiSection_NoteFrameCut( bool cutting );

// The plane outline + the lollipop, from the camera draw's overlay tail.
void KiwiSection_DrawWorld();

// ── picking (from kiwi_pick.cpp) ────────────────────────────────────────────
// "Clicks in section mode should not select geometry the cut removed."
//
// TWO HALVES, because the picker has two passes and they fail differently:
//   * THE AREA PASS marches a ray, so the sound fix is to move its START to the
//     section plane whenever the ray begins above the level.
//   * THE SCREEN-SPACE VERTEX/EDGE PASS ranks candidates by pixel distance; its
//     winner is tested with KiwiSection_PointVisible and dropped when it is above
//     the level, so the pick falls through to the (clamped) area pass.
// BOTH are no-ops unless KiwiSection_Cutting() — see the header note above.
void KiwiSection_ClampRayStart( float *start, const float *dir );
bool KiwiSection_PointVisible( const float *p );

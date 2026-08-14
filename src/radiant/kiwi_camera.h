#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_camera.h — RADIANT_UX_DESIGN §10: the orbit-camera layer.
//
// A THIN layer that produces the same `camera_s.origin` / `camera_s.angles` the
// ported Cam_Draw already consumes — the render path does not change, and
// CamWnd_BuildMatrix keeps deriving vpn/vright/vup from those angles exactly as
// before.  Nothing here touches the ported camera code.
//
// Spec mapping (D-1 RESOLVED, shakeout A — see the appendix):
//   MMB drag        -> KiwiCam_OrbitBegin + KiwiCam_OrbitDrag  (orbit about the
//                      pivot LATCHED at the press — whatever the cursor ray hit)
//   Alt+MMB click   -> KiwiViewCube_StepAxisView (ROUND P; a DRAG is still an orbit)
//   wheel           -> KiwiCam_Dolly                        (along the CURSOR ray)
//   RMB drag        -> KiwiCam_PanDrag    (TRUCK — shakeout E user directive:
//                      "The camera should be pan on right click (not shift-right
//                      click)".  Shift+RMB stays pan, so nothing was taken away.)
//   Alt+RMB drag    -> KiwiCam_LookDrag   (MOUSELOOK: angles only, origin fixed —
//                      this is where the bare-RMB look moved in shakeout E)
//   RMB click       -> mid-command CONFIRM, else the CLASSIC context menu
//                      (kiwi_viewport.cpp decides; neither reaches this file)
//   Shift+MMB drag  -> KiwiCam_PanDrag
//   W/A/S/D/Q/E     -> KiwiCam_FlyTick    (while RMB is held over the image)
//   arrow keys      -> KiwiCam_FlyTick    (no RMB needed)
//
// All input coordinates are camera-RTT-IMAGE relative with a TOP-LEFT origin —
// the same space ImGuiShell_CameraPaintCursor and kiwi_pick.h use.
// ─────────────────────────────────────────────────────────────────────────────

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND P — THE MMB JUMP.  DIAGNOSIS AND THE LATCH RULE.
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "having a bug where the camera jumps while using mmb.
// Mmb shouldn't jump the camera, just smoothly rotate it with the mouse.  Has to
// do with raytrace against the object and the mouse moving off the object while
// the camera rotates.  Keep the pos fixed so it doesn't jump the camera."
//
// TWO independent jumps, both of them re-derivations, and the user's instinct
// ("the raytrace") named the source of both.
//
// JUMP 1 — THE FIRST DRAG FRAME RE-SEATED THE EYE ON THE PIVOT'S AXIS.
//   OrbitBegin picked the surface under the CURSOR, which is by definition OFF the
//   view axis, and stored it as look_at.  OrbitDrag then did
//       origin = look_at - forward * dist
//   i.e. it ASSUMED the eye was already exactly `dist` back along the view axis
//   from look_at, which it was not.  The very first frame with a non-zero delta
//   therefore slid the eye across the sphere of radius `dist` until the pivot was
//   dead-centre — a rotation of the whole view by the pivot's off-axis angle, in
//   ONE frame, with no input to justify it.  Its size is exactly how far off
//   centre the cursor was: with the default FOV the half-image is 0.75*tan(fov/2)
//   of the depth, so an MMB press near the top of the viewport swung the camera by
//   ~35-40 degrees instantly.  Grab a corner of the screen and it is violent;
//   grab the middle and it is invisible — which is why it read as intermittent.
//
// JUMP 2 — THE PRESS RE-LATCHED s_dist, AND IN ORTHO s_dist IS THE ZOOM.
//   OrbitBegin also set s_dist to the picked point's distance.  Since round M the
//   camera is ORTHOGRAPHIC BY DEFAULT and KiwiCam_OrthoHalfHeight is
//   s_dist * tan(fov/2) * 0.75 — so pressing MMB on a near wall instead of the far
//   floor instantly rescaled the entire image.  No drag needed: this one fired on
//   the press edge alone, which is the "consecutive presses re-tracing to wildly
//   different depths" case.
//
// THE RULE NOW, and it is the round-L grab-rebase discipline applied to the
// camera:
//   * OrbitBegin LATCHES the whole frame once — pivot, the eye's OFFSET from that
//     pivot, and the angles the gesture started at — and NOTHING re-derives any of
//     it until KiwiCam_OrbitEnd.  (`PivotUsable` still runs, but only to decide
//     what the pivot IS on a miss, before the latch.)
//   * OrbitDrag applies the TOTAL accumulated pixel delta to those latched values
//     as one rigid rotation of the offset (see OrbitRotate in the .cpp), so a zero
//     delta reproduces the camera EXACTLY — the first frame cannot move anything —
//     and the grabbed point keeps its screen position for the whole gesture.
//   * s_dist is NOT TOUCHED BY THE ORBIT AT ALL.  It is the view's REFERENCE
//     DISTANCE (ortho zoom, dolly reference, pan fallback), and an orbit is not a
//     zoom.  It stays consistent by construction anyway: every modern path that
//     moves the eye (pan, fly, translate) moves look_at with it.
//   * The one path that can still reach in mid-gesture is the WHEEL (navigation
//     never stops for a gesture, §4).  KiwiCam_Dolly therefore leaves the pivot
//     alone while an orbit is latched and RE-LATCHES the offset afterwards, so the
//     dolly composes with the orbit instead of teleporting it.
//
// NO DISTANCE SMOOTHING IS NEEDED, and that is a finding rather than an omission:
// under the new formulation the camera's translation per pixel is PROPORTIONAL to
// the orbit radius, so a pivot picked very close makes the gesture gentler, not
// more violent.  The old formulation's violence came entirely from the one-frame
// re-seat, which is gone.
void KiwiCam_OrbitBegin( int imgX, int imgY );

// Rotate the camera around the LATCHED pivot by a pixel delta (accumulated since
// the press).  Pitch clamped to +-89 degrees; the eye's offset from the pivot is
// rotated by the same rigid rotation the angles undergo, so the orbit radius and
// the pivot's screen position are both preserved.
void KiwiCam_OrbitDrag( int dx, int dy );

// Drop the latch.  MUST be called on the gesture's release AND abort edges.
void KiwiCam_OrbitEnd();
bool KiwiCam_OrbitLive();

// Dolly along the cursor ray.  `wheelSteps` is positive toward the scene; each
// step scales the orbit distance by 0.85 and the camera moves the difference
// along the ray, so zooming converges on whatever is under the cursor.
void KiwiCam_Dolly( float wheelSteps, int imgX, int imgY );

// The current orbit pivot (world space) — for HUD/gizmo consumers later.
const float *KiwiCam_LookAt();
float        KiwiCam_Distance();

// ─── shakeout A: RMB mouselook, truck-pan, keyboard fly ─────────────────────
//
// MOUSELOOK.  Rotates camera.angles from a pixel delta at the same 0.35 deg/px
// and the same signs as the orbit drag and as the ported free-look
// (CamWnd_Rotate2, camwnd.cpp:2598 `angles[1] -= dx*0.35; angles[0] -= dy*0.35`),
// pitch clamped to +-89.  The camera POSITION is not touched; the orbit pivot is
// re-seated in front so a following MMB orbit is still sane.
//
// It deliberately does NOT hide or re-centre the cursor: the ported free-look
// paths pair ShowCursor(FALSE) with Cam_MouseUp's ShowCursor(TRUE)-until-visible
// loop, and this layer never enters that path at all, so there is no counter to
// keep balanced (kiwi_viewport.cpp GESTURE OWNERSHIP).  Consequence, accepted and
// logged: a mouselook drag stops at the screen edge instead of spinning forever.
void KiwiCam_LookDrag( int dx, int dy );

// TRUCK-PAN.  Moves origin AND look_at by -vright*dx*k + vup*dy*k, where k is
// KiwiCam_WorldPerPixel at the pan's REFERENCE POINT — so the world tracks the
// cursor 1:1 at that depth.  (vright is screen-right and vup is screen-up in this
// basis; see kiwi_pick.cpp ProjectRaw, which projects with exactly those two dots.)
//
// ── THE SHAKEOUT-E SENSITIVITY FIX ──────────────────────────────────────────
// USER DIRECTIVE: "the sensitivity is wonky and changes a lot with zoom.  Feels
// bad".  Diagnosed, not guessed: k was KiwiCam_WorldPerPixel( s_lookAt ), and
// s_lookAt is the ORBIT PIVOT, which is only re-seated by an orbit, a dolly or a
// fly.  Pan the camera sideways along a wall and the pivot stays parked at its
// old depth; dolly toward a surface and PivotOnAxis puts the pivot at s_dist,
// which after the dolly's own clamps can be a small fraction of — or several
// times — the real depth of whatever the cursor is over.  k is LINEAR in that
// depth, so the pan speed swung by the same factor, and it swung on a quantity
// the user cannot see.
//
// The fix anchors k on WHAT IS UNDER THE CURSOR at the moment the pan starts:
// KiwiCam_PanBegin does ONE area pick and caches k at the hit point (falling back
// to the pivot on a miss), and the whole gesture uses that one number.  The
// grabbed point then tracks the cursor ~1:1, which is the Plasticity feel, and it
// cannot drift mid-drag because the cache is per-gesture.
void KiwiCam_PanBegin( int imgX, int imgY );
void KiwiCam_PanDrag ( int dx, int dy );
void KiwiCam_PanEnd  ();

// Translate the camera and the orbit pivot together (what the fly keys and the
// pan share).  No-op for a zero delta.
void KiwiCam_Translate( const float *delta );

// Point the camera along `pitch`/`yaw` (camera.angles[0]/[1] degrees) while
// keeping the current look_at and orbit distance — the view-cube's one mutator.
void KiwiCam_LookAlong( float pitch, float yaw );

// ── shakeout I: the modern MAP-NEW / MAP-LOAD placement ─────────────────────
// Put the camera at (0,-160,96) looking at the world origin and seat the orbit
// pivot there.  USER DIRECTIVE: "Scale of the map (3d view) is still way too big.
// Tone it down by about a factor of 10 in terms of zoom and such." — the ported
// placements leave the camera AT the origin (entity.cpp Map_New: (0,0,48) looking
// down +X; map.cpp's start-entity-less load: (0,0,0)), i.e. inside the world with
// nothing in frame, which is half of why the world reads as unbounded.
// The CALLER gates it on KiwiUX_ModernInput: the classic profile keeps the ported
// placement byte for byte.
void KiwiCam_DefaultSpawn();

// ── ROUND J: FRAME A BOUNDING BOX (Plasticity's `viewport:focus`) ───────────
// Keep the current view DIRECTION, put the orbit pivot on the box centre and pull
// the camera back far enough that the box's bounding SPHERE fits inside the
// smaller of the two half-FOVs, with a small margin.  This is the one mutator the
// §28 Focus command (kiwi_focus.h) needs; the command itself owns "what is the
// selection's box", which is not a camera question.
//
// The projection constants are NOT re-derived: the half-angle tangents come from
// the SAME `tan(fov/2) * 0.75` that KiwiCam_WorldPerPixel and kiwi_pick.cpp's
// MakeProjCtx use (which is CameraCalcRayDir's own per-pixel scale times half the
// image), so a frame is exactly tight in the axis that actually clips.
//
// A degenerate box (a single point, an empty selection's inverted sentinel box)
// is NOT a special case here — the caller must not hand one over; the command
// checks first and says so.  A radius below KCAM_FRAME_MIN_RADIUS is floored so
// framing one tiny brush does not put the near plane inside it.
void KiwiCam_FrameBounds( const float mins[3], const float maxs[3] );

// World units per screen pixel at `world`, i.e. the scale that makes a marker
// screen-constant at any distance.  Identical to CameraCalcRayDir's per-pixel
// scale times the eye-space depth.  THE CALLER MUST HAVE RUN CamWnd_BuildMatrix()
// this frame (it reads camera.vpn).
//
// Lifted here from kiwi_hover.cpp in shakeout A: the gizmo, the hover marker and
// the pan all size themselves with it, and three copies of one projection
// constant is exactly how a screen-scale drifts.
float KiwiCam_WorldPerPixel( const float *world );

// ── keyboard fly ────────────────────────────────────────────────────────────
// ONE poll per input tick, from the shell's post-present camera arm.  Both arms
// are decided by the CALLER (kiwi_viewport.cpp) because the conditions are input
// arbitration, not camera state:
//   wasd   — RMB is held in the camera image (mouselook is live)
//   arrows — the cursor is over the camera image, no modal command is running and
//            ImGui wants no text input
// Keys are read with GetAsyncKeyState, so they never enter the message queue and
// cannot double-fire the hotkey table.  dt comes from a QPC delta measured here
// (the pump is 60 fps-capped but the tick is not guaranteed, so 1/60 would be a
// lie); a stall is clamped to 0.1 s so an alt-tab cannot teleport the camera.
//
// Speed = g_PrefsDlg->m_nMoveSpeed (prefs.h:38, "MoveSpeed", default 350) units
// per second, times KiwiCam_FlySpeedScale(), times 3 while Shift is held.
// USER DIRECTIVE: the fly is ARROW KEYS ONLY (WASD clashed with the modern
// bindings — S = Scale).  Param 1 (`wasd`) now means "RMB-look held": arrows fly
// with modifiers ignored (Shift = boost) and are swallowed from the message
// path; param 2 = hover-arrows, bare arrows only.
void KiwiCam_FlyTick( bool wasd, bool arrows );

// True while the RMB look owns this vk (the four ARROWS) — the key funnel
// swallows it so look + arrow cannot also fire a hotkey-table binding.
bool KiwiCam_FlySwallowKey( unsigned int vk );

// Persisted fly-speed multiplier over the classic MoveSpeed pref (default 10 —
// user directive: 1x was "incredibly slow").
float KiwiCam_FlySpeedScale();
void  KiwiCam_SetFlySpeedScale( float mul );

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND M — ORTHOGRAPHIC / PERSPECTIVE TOGGLE
// ═════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: "while at the perfect axis align plane angle, it
// doesn't show a box as just a square.  I think this is due to the renderer.  We
// need an orthographic and perspective camera toggle.  Add that in the top right
// somewhere as a button in the 3d camera viewport."
//
// The diagnosis is the user's and it is right: the 3D view builds a PERSPECTIVE
// projection (camwnd.cpp CamWnd_SetupScene), so a cube seen down +X still shows
// its side faces receding.  This is a REAL ortho projection, not a long lens —
// verified swappable before it was written, because the 2D views already push an
// orthographic GfxMatrix through the SAME R_Ed_SetSceneParms entry point
// (xywnd.cpp XY_SetupScene / XY_SetupProjectionMtx).  See camwnd.cpp for the
// matrix and kiwi_pick.cpp for the picking inverse.
//
// ── THE ONE INVARIANT: SCALE-CONTINUITY AT THE PIVOT ────────────────────────
// The ortho half-height is
//       H = KiwiCam_Distance() * tan(fov/2) * 0.75
// i.e. exactly the half-height the perspective frustum has AT THE ORBIT PIVOT
// (0.75 is the same vertical-FOV factor CameraCalcRayDir, KiwiCam_WorldPerPixel
// and kiwi_pick.cpp's MakeProjCtx all carry).  Toggling therefore does not change
// the on-screen size of anything at the pivot depth — only of things nearer and
// further.  It also means the WHEEL still zooms: KiwiCam_Dolly already changes
// s_dist, so H follows it, and the ortho image scales the way the perspective one
// did.  (What the dolly's origin translation stops doing in ortho is changing the
// image at all — in an orthographic view sliding the eye along the view axis is a
// no-op.  The zoom comes entirely from s_dist.)
bool  KiwiCam_Ortho();
void  KiwiCam_SetOrtho( bool on );

// The ortho view volume's half-HEIGHT in world units, > 0.  Every consumer of the
// ortho projection (the matrix, the ray builder, the projection inverse and the
// screen scale) derives from THIS ONE function so they cannot drift apart.
float KiwiCam_OrthoHalfHeight();

// ── KIWI-UX (ROUND AI, ITEM 2): CAN THIS VIEW PORTRAY MOTION ALONG AN AXIS? ──
// USER REPORT, verbatim: "when creating a box (or other shape) with the camera
// perfectly aligned to TOP, when it's time to do the Height(Z), i move the camera
// and the height is already set to a huge negative number… It's impossible to
// portray Z movement while at top/bottom camera lock."
//
// EVERY one-axis gesture in this editor maps the cursor to a scalar the same way:
// `RayAxis( ray, pt, axis )` — the closest point on the world line (pt, axis) to
// the cursor ray.  Its denominator is
//         den = 1 - dot(axis, ray.dir)^2 = sin^2(theta)
// where theta is the angle between the axis and the ray.  The solve is therefore
// amplified by 1/sin^2(theta): a one-pixel cursor move at ground distance d moves
// the answer by roughly  d / (f * sin^2 theta)  world units, where f ~ 940 is
// CameraCalcRayDir's own pixel scale (camwnd.cpp:3090).  At theta = 5 degrees that
// is 70 units per pixel at d = 500 — and the local `den < 1e-4` guards in the six
// RayAxis copies only refuse at theta < 0.6 degrees, so everything between 0.6 and
// ~10 degrees SOLVES, loudly and wrongly.  That is the -140 yd: it is not a stale
// value, it is a real solve of a degenerate system, latched into the command's
// scalar and only made visible when the user orbits away.
//
// TWO GATES, and they answer different questions:
//
//   THE VIEW GATE (this function).  Reads the CAMERA's vpn, not the cursor ray,
//   so it is ONE stable boolean per frame: it does not flicker as the cursor
//   crosses the screen, and the HUD can name the remedy ("orbit to set height").
//   A gesture that fails it HOLDS its scalar and REBASES when the gate re-opens —
//   the round-L grab-rebase discipline, applied to a view change instead of a
//   pause/resume edge.
//
//   THE SAMPLE GATE (KCAM_RAYAXIS_MIN_DEN).  The view gate cannot be the whole
//   answer: with a 65-degree FOV a ray at the edge of the image is up to 32
//   degrees off vpn, so even at a legal vpn SOME pixel on screen still looks
//   straight down the axis.  Each RayAxis copy therefore refuses its own sample
//   at sin^2(theta) < KCAM_RAYAXIS_MIN_DEN.  The cost of a refusal is that the
//   scalar does not move for those pixels — a small dead disc around wherever the
//   axis is end-on on screen — which is strictly better than a jump.
//
// THE THRESHOLD.  0.97 is 14.1 degrees, sin^2 = 0.0594, i.e. the answer is
// amplified ~17x — about 9 world units per pixel at d = 500, which the grid quantise
// then absorbs.  Tighter (0.99 / 8 degrees) and the amplification is 51x and the
// drag is unusable; looser and a legitimately steep-but-workable view is refused.
// KCAM_RAYAXIS_MIN_DEN is the SAME angle expressed as sin^2 so the two gates
// cannot drift apart.
#define KCAM_AXIS_PORTRAY_DOT   0.97f
#define KCAM_RAYAXIS_MIN_DEN    0.0594f

// True when the current view can portray motion along the unit vector `axis`.
// `outDot`, when given, receives |vpn . axis| so a caller can drive a meter or a
// message without recomputing it.  ORTHO is not special-cased: an ortho top view
// has the same vpn and the same degeneracy (worse, in fact — every ray is exactly
// parallel to Z there), so it is refused by the same test.
bool  KiwiCam_AxisPortrayable( const float *axis, float *outDot = 0 );

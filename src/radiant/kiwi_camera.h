#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// RADIANT_UX_DESIGN §10 orbit-camera interface. It writes the same camera_s origin
// and angles consumed by the ported draw; CamWnd_BuildMatrix still owns the basis.
// Input positions are camera-RTT pixels with a top-left origin.
// MMB orbits, wheel dollies (ortho; perspective's wheel sets fly speed), RMB/Shift+MMB
// pans, Alt+RMB looks, and arrows fly.

// Orbit gesture.
// Latch pivot, eye offset, and angles at press, then rotate that offset by total
// drag delta. Do not relatch s_dist from a pick: in ortho it controls image scale.
// A mid-orbit wheel keeps the pivot fixed and relatches the offset afterward.
void KiwiCam_OrbitBegin( int imgX, int imgY );

// View > Debug: Orbit Pivot Probe - world-space rays of the last orbit press's pivot
// probe (camwnd.cpp world-overlay tail); nothing while the toggle is off.
void KiwiCam_DrawOrbitDebug();

// Orbit uses 360/viewport-height degrees per pixel on both axes. Pitch stops at
// ±89, widened only on a pole-exact starting side; clamping rewinds refused pixels.
// The rigid eye-offset rotation preserves orbit radius and pivot screen position.
void KiwiCam_OrbitDrag( int dx, int dy );

// Must be called on gesture release and abort.
void KiwiCam_OrbitEnd();

// Wheel zoom; positive wheelSteps moves toward the scene. Ortho scales s_dist and
// pans in closed form to keep the cursor's world point fixed. Perspective scales
// the eye about the current cursor-ray hit. Both use a flat 0.95 factor per notch.
void KiwiCam_Dolly( float wheelSteps, int imgX, int imgY );

// Current orbit pivot in world space and its reference distance.
const float *KiwiCam_LookAt();
float        KiwiCam_Distance();

// Mouselook matches CamWnd_Rotate2's 0.35 deg/px and signs (camwnd.cpp:4245), with
// one-sided ±89 pitch bounds. It changes angles only, then re-seats the pivot.
// The cursor is not recentered here, so an Alt+RMB drag stops at the screen edge; the
// mouse-forward fly (kiwi_viewport.cpp) reads raw mouse motion, so it never does.
void KiwiCam_LookDrag( int dx, int dy );
// KIWI (2026-10-27): the same look in fractional pixels (the fly look's raw, sensitivity-
// scaled motion), so slow sensitivities keep their sub-pixel remainder.
void KiwiCam_LookDelta( float dx, float dy );

// Truck-pan moves origin and pivot by -vright*dx*k + vup*dy*k. Cache k at the
// pivot when the gesture begins; moving eye and pivot together keeps it invariant.
void KiwiCam_PanBegin( int imgX, int imgY );
void KiwiCam_PanDrag ( int dx, int dy );
void KiwiCam_PanEnd  ();

// Translate the camera and the orbit pivot together (what the fly keys and the
// pan share).  No-op for a zero delta.
void KiwiCam_Translate( const float *delta );

// Aim along pitch/yaw while keeping look_at and distance. Absolute snaps allow
// exact ±90; using the drags' ±89 would leak side faces in top/bottom views.
void KiwiCam_LookAlong( float pitch, float yaw );

// The nearest of the six axis views (view cube), turned about the surface at the screen
// centre (else the pivot); the yaw snaps to 90 degrees so top / bottom stay axis-aligned.
// Returns the view's name for the console.
const char *KiwiCam_SnapToNearestAxis();

// 2D marker anchor: the eye in perspective; in ortho, the on-axis point at the
// standoff plane. The ortho origin is a zoom-dependent pseudo-eye, not a world place.
void KiwiCam_MarkerViewpoint( float out3[3] );

// Consume whether origin or angles changed since the previous call. Call once per
// camera tick; this drives 2D view dirtiness only and invalidates no derived data.
bool KiwiCam_PoseChangedSinceLastTick();

// Plane-facing sign for handle presentation: perspective uses the eye's side;
// ortho uses view direction because its pseudo-eye has no world-space meaning.
float KiwiCam_FacingSign( const float point[3], const float normal[3] );

// Modern-profile spawn at (0,-160,96), looking at and pivoting around the origin.
// The caller leaves classic-profile ported placement unchanged.
void KiwiCam_DefaultSpawn();

// Preserve view direction, pivot on the box center, and fit its bounding sphere to
// the smaller half-FOV using the shared tan(fov/2)*0.75 projection constant.
// Caller rejects degenerate boxes; tiny radii are floored for near-plane clearance.
void KiwiCam_FrameBounds( const float mins[3], const float maxs[3] );

// KIWI: perspective only, after a head-on frame of a surface: trace from `surface` along
// `normal` (toward the eye) and bring the eye in front of the first thing nearer than it.
// True when the eye moved.
bool KiwiCam_PullInFrontOf( const float surface[3], const float normal[3] );

// World units per screen pixel at `world`. Caller must have run CamWnd_BuildMatrix
// this frame because perspective scaling reads camera.vpn.
float KiwiCam_WorldPerPixel( const float *world );

// Poll arrow keys once per input tick. `lookHeld` permits modifiers and Shift boost;
// `arrows` is the bare-arrow hover arm. QPC supplies dt, clamped to 0.1 seconds.
// Perspective flies in world units; ortho pans along vright/vup at a screen-pixel
// rate because moving along the view axis is invisible in a parallel projection.
// KIWI (2026-10-21, reworked 2026-10-27): `wasd` = the fly keys are live (the mouse-forward
// fly is held): W/S forward/back, A/D strafe, Q/E down/up (camera up, or world Z with
// KiwiCam_FlyWorldUp); Shift faster, Ctrl slower.  Perspective motion eases in and out.
void KiwiCam_FlyTick( bool lookHeld, bool arrows, bool wasd );

// True while the camera owns this vk - the four ARROWS during a look, and W/A/S/D/Q/E
// while the fly keys are live - so the key funnel swallows it before the hotkey table.
bool KiwiCam_FlySwallowKey( unsigned int vk );

// Persisted multiplier over classic MoveSpeed; default 4x.
float KiwiCam_FlySpeedScale();
void  KiwiCam_SetFlySpeedScale( float mul );
// Wheel while flying: positive steps speed the fly keys up (1.25x per notch).
void  KiwiCam_WheelFlySpeed( float wheelSteps );
// KIWI (2026-10-27, user: "show the movespeed changes when scroll is activated (with some kind
// of speed bar). also have sensible mins and maxs"): the perspective fly speed in WORLD UNITS
// per second, KCAM_FLY_UPS_MIN .. KCAM_FLY_UPS_MAX (no longer a multiple of the classic
// MoveSpeed pref, which put the range at 35 .. 17500 u/s with MoveSpeed 350).
#define KCAM_FLY_UPS_MIN  20.0f            // KIWI (2026-10-27, user: "the minimum should be 20, anything lower is too slow")
#define KCAM_FLY_UPS_MAX  4096.0f          // ~340 ft/s: across a whole map in a few seconds
float KiwiCam_FlySpeed();
void  KiwiCam_SetFlySpeed( float ups );
// Seconds since the wheel last changed the fly speed (a large value when it never has):
// the viewport's speed bar shows while this is fresh.
double KiwiCam_FlySpeedChangedAge();
// KIWI (2026-10-27, user: "a better mouselook option"): fly-look settings (Settings panel).
// Raw mouse counts (WM_INPUT) -> look, with the sensitivity and invert-Y settings applied.
void  KiwiCam_FlyLookRaw( float countsX, float countsY );
float KiwiCam_FlyLookSens();               // multiplier over 0.12 deg per raw count, default 1
void  KiwiCam_SetFlyLookSens( float s );
bool  KiwiCam_FlyInvertY();
void  KiwiCam_SetFlyInvertY( bool on );
bool  KiwiCam_FlyWorldUp();                // Q / E along world Z instead of the camera's up
void  KiwiCam_SetFlyWorldUp( bool on );

// Orthographic/perspective toggle.
// Real orthographic projection with pivot-scale continuity:
// H = KiwiCam_Distance() * tan(fov/2) * 0.75, shared with rays and screen scale.
// Moving the pseudo-eye is image-invisible but changes its eye-centered depth slab,
// so ortho standoff is derived from this half-depth. Perspective instead keeps a
// separate inverse-VP-safe limit (camwnd.cpp:230-235).
// CamWnd_SetupScene and all derived camera limits consume this single definition.
#define KCAM_ORTHO_DEPTH_HALF 524288.0f

bool  KiwiCam_Ortho();
void  KiwiCam_SetOrtho( bool on );

// KIWI: a tool handle's display anchor, kept at least marginPx inside the camera image.
// Copies `world` and returns false when it is already there; otherwise writes the pinned
// point (same view depth) and returns true.  Presentation only: drags rebase at the press.
bool  KiwiCam_KeepOnScreen( const float *world, float marginPx, float *out );

// Positive ortho half-height in world units; matrix, rays, inverse, and screen
// scale all derive from this function.
float KiwiCam_OrthoHalfHeight();

// A one-axis closest-point solve has den = 1-dot(axis,ray.dir)² = sin²(theta), so
// error is amplified by 1/den near an end-on view. The stable view gate reads vpn;
// the per-sample gate still checks each cursor ray because edge rays diverge from vpn.
// |dot|=0.97 is 14.1 degrees; sin²=0.0594 caps amplification near 17x. Both macros
// encode the same cone and must move together.
#define KCAM_AXIS_PORTRAY_DOT   0.97f
#define KCAM_RAYAXIS_MIN_DEN    0.0594f
// KIWI: "extrude a construction face at a very flat camera angle ... wont work" - the
// exact closest-point solve is used only while the eye->anchor line is at least 28.4
// degrees off the axis (twice the sample cone, so the vanishing-point refusal zone stays
// clear of the anchor); closer to end-on, KiwiCam_RayAxis maps cursor travel in the
// anchor's screen plane instead of refusing.
#define KCAM_AXIS_EXACT_DOT     0.88f
#define KCAM_AXIS_PORTRAY_SIN   0.2431f     // sqrt(1 - KCAM_AXIS_PORTRAY_DOT²)
#define KCAM_AXIS_UPCONV_SIN    0.035f      // ~2 degrees: below this the axis is end-on

// Point on unit-axis line (pt,axis) driven by a unit-direction cursor ray.  Off-axis
// views: the exact closest point (false only for a ray inside the sample cone around
// the axis's vanishing point).  Near end-on views (KIWI: "very flat camera angle"): the
// cursor's offset from pt in the screen plane along the axis's screen direction, gain
// eased to 1:1 at a dead end-on view where screen-up pulls toward the viewer; never
// false.  Forward-declare ray_t to keep this header lightweight.
struct ray_t;
bool  KiwiCam_RayAxis( const ray_t &ray, const float *pt, const float *axis, float *out );

// outDot receives |vpn·axis|.  KIWI: "extrude a construction face at a very flat camera
// angle ... wont work" - every non-zero axis is portrayable now (KiwiCam_RayAxis has the
// end-on fallback), so the tools' "ORBIT to pull" holds no longer engage.
bool  KiwiCam_AxisPortrayable( const float *axis, float *outDot = 0 );

// KIWI (2026-10-24, user: "start saving the camera position/settings/mode/sectionview when
// the map is closed so you dont have to re-orient each time you re-open a map.  Save it in
// the kiwi custom stuff"): the per-map view, as `view ...` records in the .kiwi sidecar -
// camera pose, orbit pivot + distance, ortho/perspective, and the section cut.  Render
// method, x-ray and edge outline stay editor-wide profile settings.
void KiwiCamView_WriteSidecar( void *file );              // FILE *: the `view` records
bool KiwiCamView_ParseSidecarLine( const char *line );   // true = a `view` record, consumed
void KiwiCamView_ResetPending();                          // KiwiCon_LoadSidecar's start
void KiwiCamView_ApplyPending();                          // tail of the map load
bool KiwiCamView_Restored();      // the last load restored a saved camera (skip the load fit)

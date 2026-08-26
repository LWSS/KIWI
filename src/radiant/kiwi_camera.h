#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// RADIANT_UX_DESIGN §10 orbit-camera interface. It writes the same camera_s origin
// and angles consumed by the ported draw; CamWnd_BuildMatrix still owns the basis.
// Input positions are camera-RTT pixels with a top-left origin.
// MMB orbits, wheel dollies, RMB/Shift+MMB pans, Alt+RMB looks, and arrows fly.

// Orbit gesture.
// Latch pivot, eye offset, and angles at press, then rotate that offset by total
// drag delta. Do not relatch s_dist from a pick: in ortho it controls image scale.
// A mid-orbit wheel keeps the pivot fixed and relatches the offset afterward.
void KiwiCam_OrbitBegin( int imgX, int imgY );

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
// The cursor is not recentered, so a drag stops at the screen edge.
void KiwiCam_LookDrag( int dx, int dy );

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

// World units per screen pixel at `world`. Caller must have run CamWnd_BuildMatrix
// this frame because perspective scaling reads camera.vpn.
float KiwiCam_WorldPerPixel( const float *world );

// Poll arrow keys once per input tick. `lookHeld` permits modifiers and Shift boost;
// `arrows` is the bare-arrow hover arm. QPC supplies dt, clamped to 0.1 seconds.
// Perspective flies in world units; ortho pans along vright/vup at a screen-pixel
// rate because moving along the view axis is invisible in a parallel projection.
void KiwiCam_FlyTick( bool lookHeld, bool arrows );

// True while the RMB look owns this vk (the four ARROWS) — the key funnel
// swallows it so look + arrow cannot also fire a hotkey-table binding.
bool KiwiCam_FlySwallowKey( unsigned int vk );

// Persisted multiplier over classic MoveSpeed; default 4x.
float KiwiCam_FlySpeedScale();
void  KiwiCam_SetFlySpeedScale( float mul );

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

// Zoom meter.
// outFrac is 0 at maximum zoom-in and 1 at the projection ceiling, logarithmic so
// it is linear in multiplicative wheel notches. outWpp reports pivot scale.
// Either output may be null; false means camera size/range is unusable.
bool  KiwiCam_ZoomMeter( float *outFrac, float *outWpp );

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

// Closest point on unit-axis line (pt,axis) to a unit-direction ray. False inside
// the sample-refusal cone. Forward-declare ray_t to keep this header lightweight.
struct ray_t;
bool  KiwiCam_RayAxis( const ray_t &ray, const float *pt, const float *axis, float *out );

// Whether the view can portray motion along `axis`; outDot receives |vpn·axis|.
// Ortho uses the same test because its end-on rays have the same degeneracy.
bool  KiwiCam_AxisPortrayable( const float *axis, float *outDot = 0 );

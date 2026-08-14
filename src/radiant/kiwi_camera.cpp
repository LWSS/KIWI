#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_camera.cpp — RADIANT_UX_DESIGN §10 implementation.  See kiwi_camera.h.
//
// ── ANGLE CONVENTION (verified against CamWnd_BuildMatrix, camwnd.cpp 0x403470) ─
// The editor calls AngleVectors with the pitch NEGATED:
//     a = { -angles[0], angles[1], angles[2] };  AngleVectors( a, vpn, vright, vup )
// so with AngleVectors' forward = (cos p cos y, cos p sin y, -sin p):
//     vpn = ( cos(pitch) cos(yaw), cos(pitch) sin(yaw), sin(pitch) )
// i.e. a POSITIVE angles[0] looks UP.  Every derivation below uses that same
// expression through the same AngleVectors call, so the basis this layer assumes
// and the basis the draw computes can never drift.
//
// ── DRAG SIGNS ───────────────────────────────────────────────────────────────
// Same signs as the ported free-look (Cam_Rotate 0x403700: yaw -= dx, pitch -= dy),
// which for an ORBIT gives the turntable feel: dragging right swings the camera
// clockwise seen from above (the world appears to follow the drag), dragging down
// lifts the camera and brings the top of the pivot into view.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                 // camera_s
#include "prefs.h"                   // g_PrefsDlg (m_bCamXYUpdate, m_nMoveSpeed, camera_fov)
#include <universal/com_math.h>      // AngleVectors
#include "kiwi_camera.h"
#include "kiwi_pick.h"
#include "radiant_registry.h"        // Radiant_Profile* (the fly-speed multiplier)

#include <math.h>

// ── ported entry points (verified against their definitions) ────────────────
extern camera_s *Ed_Camera();          // camwnd.cpp
extern void      CamWnd_BuildMatrix(); // camwnd.cpp 0x403470
extern int       g_nUpdateBits;        // 0x25D5A74 (mainfrm.cpp)

namespace
{
    const float KCAM_DEG_PER_PX  = 0.35f;      // Cam_Rotate2's rate (0x4037c0)
    // ── SHAKEOUT I: THE WORLD FELT 10x TOO BIG ──────────────────────────────
    // USER DIRECTIVE, verbatim: "Scale of the map (3d view) is still way too big.
    // Tone it down by about a factor of 10 in terms of zoom and such."
    //
    // Nothing about the WORLD changed — a unit is still an inch and the default
    // grid is still 10 in (kiwi_units.h; the directive did not ask for a grid
    // change and a grid that no longer matches the map format would be a much
    // bigger lie than a camera that starts too far out).  What was wrong is every
    // number that decides HOW MUCH OF IT YOU SEE AT ONCE and how fast you move
    // through it, and there are three of them:
    //
    //   KCAM_DEF_DIST   256 -> 96   the orbit radius the camera starts with and
    //                               falls back to.  256 in is 21 feet of standoff
    //                               from a pivot before you have touched anything,
    //                               which is why a fresh map reads as a huge empty
    //                               plain.  96 in (8 ft) is a body-height standoff:
    //                               a 128-unit brush fills the frame instead of
    //                               sitting in the middle of it.
    //   KCAM_DOLLY_STEP 0.85 -> 0.90  15% of the reference distance per wheel notch
    //                               overshoots badly once the reference is small —
    //                               two notches used to more than halve it.  10%
    //                               takes ~7 notches to halve instead of ~4.3,
    //                               which is the "finer" the directive asked for.
    //   the FLY multiplier 10x -> 4x  (KiwiCam_FlySpeedScale below.)
    const float KCAM_DOLLY_STEP  = 0.90f;      // spec §10: fractional per wheel step
    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AJ, ITEM 3) — THE NEAR-FIELD DOLLY EASE
    // ═══════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: "When zooming in, the camera gets squirrely.  The
    // sensitivity is too high.  Need to try and remedy that slightly."
    //
    // PLASTICITY, MEASURED (its fork of three.js OrbitControls, and every number
    // here is from it):
    //   * the wheel is MULTIPLICATIVE and the constant is
    //     `zoomScale = Math.pow(0.95, zoomSpeed)` with `zoomSpeed = 1`
    //     (OrbitControls.ts:648; the default is default-settings.js:9-13), i.e.
    //     FIVE percent a notch — half of this editor's ten;
    //   * `Math.sign(deltaY)` throws the wheel delta's MAGNITUDE away
    //     (OrbitControls.ts:436), so a fast flick is not a bigger step;
    //   * there is NO soft minimum and no easing: `spherical.radius` is clamped
    //     hard against `minDistance = 0.1` (OrbitControls.ts:42, :222-225).  The
    //     only damping is the geometric series itself — the WORLD step is
    //     `radius * 0.05`, so it shrinks as the camera closes in;
    //   * and — the one that matters most here — **there is no zoom-to-cursor at
    //     all.**  `onMouseWheel` (OrbitControls.ts:423-443) never reads
    //     `clientX/Y`, and `dolly()` (:650-657) only scales the radius about
    //     `this.target`.  The eye moves along the VIEW AXIS, always.
    //
    // SO "SQUIRRELY" HAS TWO SOURCES HERE AND ONLY ONE OF THEM IS THE STEP SIZE.
    // KiwiCam_Dolly moves the eye along the CURSOR RAY (the "zoom to mouse" this
    // editor deliberately has and Plasticity does not).  Decompose that motion:
    // the along-view part is the zoom, the perpendicular part is a SIDEWAYS SLIDE
    // of `move * sin(angle between the cursor ray and vpn)`.  Far out, `move` is a
    // small fraction of what is framed and the slide is invisible.  Close in,
    // `move` is a large fraction of the framed extent — so every notch also shoves
    // the view sideways by a visible slice of the screen, which is exactly what
    // "squirrely" describes and is NOT what a smaller step alone would fix.
    //
    // THE REMEDY IS DELIBERATELY SLIGHT AND IS TWO EASES ON ONE PARAMETER,
    // `t = clamp( reference / KCAM_DOLLY_NEAR_DIST, 0, 1 )`:
    //   1. the STEP eases 0.90 -> 0.96 as t -> 0.  At the bottom that is a hair
    //      gentler than Plasticity's flat 0.95, which is the right end to land on
    //      for detail work; at t == 1 it is byte-for-byte the old number.
    //   2. the AIM eases from the full cursor ray toward the VIEW AXIS as t -> 0
    //      (never all the way — KCAM_DOLLY_AIM_MIN keeps a third of it, so zoom-to-
    //      mouse still works up close, it just stops dominating).  At t == 1 the
    //      direction is the cursor ray exactly as before.
    // NEAR_DIST is half the default standoff (KCAM_DEF_DIST 96), i.e. "closer than
    // arm's length to the thing you are working on"; beyond it nothing changes at
    // all, which is the "slightly" the report asked for.
    //
    // ORBIT AND PAN WERE CHECKED AND ARE LEFT ALONE, because they already match
    // Plasticity: pan is per-pixel world-proportional through KiwiCam_WorldPerPixel
    // with a floor at KCAM_MIN_DIST (the same shape as its
    // `2 * targetDistance * tan(fov/2) / clientHeight`, OrbitControls.ts:263-282),
    // and orbit is a flat KCAM_DEG_PER_PX with NO distance term, exactly as
    // `2*PI*dx/clientHeight` has none (OrbitControls.ts:672-677).
    // ── KIWI-UX (ROUND AN, ITEM 5): "the zoomed in sensitivity is still too
    // high, you mighta made it worse."  Round AL's numbers, retuned harder in the
    // same shape: the ease starts from FURTHER OUT (96 in — a whole default
    // framing, so the taper is felt before the camera is already on top of the
    // geometry), lands SLOWER (0.985 ≈ 1.5%/notch at the bottom vs 4%), and the
    // cursor-aim lateral pull — the "squirrely" half per round AL's diagnosis —
    // fades out COMPLETELY up close instead of keeping a 35% floor (Plasticity
    // has no zoom-to-cursor at all; near the subject neither do we).
    const float KCAM_DOLLY_STEP_NEAR = 0.985f; // the per-notch factor at t == 0
    const float KCAM_DOLLY_NEAR_DIST = 96.0f;  // where the ease begins (KCAM_DEF_DIST)
    const float KCAM_DOLLY_AIM_MIN   = 0.0f;   // cursor-aim fully off at t == 0
    // USER DIRECTIVE (shakeout C): "after a certain point, you can't zoom in
    // further" — 8 units (8 inches) was the wall.  1 inch lets the dolly get
    // close enough to detail work; the surface-referenced step already makes the
    // approach slow down smoothly instead of punching through.
    const float KCAM_MIN_DIST    = 1.0f;
    const float KCAM_MAX_DIST    = 262144.0f;
    const float KCAM_DEF_DIST    = 96.0f;      // shakeout I: was 256

    // ── SHAKEOUT I: the modern MAP-NEW / MAP-LOAD camera placement ──────────
    // The ported placement puts a fresh map's camera AT (0,0,48) looking down +X
    // (entity.cpp Map_New) and a start-entity-less load at (0,0,0) (map.cpp).
    // Both are INSIDE the world origin looking at nothing, which is the other half
    // of "the scale is way too big": you begin outside any frame of reference and
    // the first thing you have to do is navigate.  A three-quarter view from
    // (0,-160,96) puts the origin, the ground plane and the grid all on screen at a
    // human standoff — the same shape every modern DCC opens with.
    //   yaw   = atan2( 0-(-160), 0-0 )  = +90 deg   (look toward +Y)
    //   pitch = atan2( 0-96, 160 )      = -31 deg   (look down; +pitch is UP here,
    //                                                see the ANGLE CONVENTION note)
    // ── ROUND J: the FRAME (Plasticity `/`) tuning ──────────────────────────
    // The margin is what stops a framed box touching the image edge; 1.15 leaves
    // roughly a 7% border on the tight axis, which is what Plasticity's own focus
    // looks like.  The radius floor stops "frame one 8-unit brush" from parking
    // the eye 9 units away, inside the near plane's comfort zone.
    const float KCAM_FRAME_MARGIN     = 1.15f;
    const float KCAM_FRAME_MIN_RADIUS = 16.0f;

    const float KCAM_SPAWN_POS[3] = { 0.0f, -160.0f, 96.0f };
    const float KCAM_SPAWN_PITCH  = -31.0f;
    const float KCAM_SPAWN_YAW    =  90.0f;

    // Fly tuning (shakeout A).
    const float  KCAM_FLY_SHIFT   = 3.0f;      // Shift boost
    const double KCAM_FLY_MAX_DT  = 0.1;       // a stall must not teleport the camera
    const char  *KCAM_SECTION     = "KiwiUX";
    const char  *KCAM_FLY_ENTRY   = "FlySpeedScale3";   // v3: default moved 10x -> 4x
    // ROUND N: renamed "CameraOrtho" -> "CameraOrtho2" so the NEW DEFAULT actually
    // reaches everyone.  USER DIRECTIVE, verbatim: "Make orthogonal 3d cam view the
    // default."  A default alone would not have done it: round M's key is already
    // written into every profile that has ever toggled the pill, and
    // Radiant_ProfileGetInt returns the stored 0 in preference to any default we
    // pass.  Renaming the entry is the FlySpeedScale precedent, one line up
    // (KCAM_FLY_ENTRY carries the same note for the same reason) — the old key is
    // simply orphaned, and a user who wants perspective back clicks the pill once,
    // which writes the new key.
    const char  *KCAM_ORTHO_ENTRY = "CameraOrtho2";     // ROUND N: default ON

    // -1 = not read from the profile yet.  DEFAULT ON since round N (above).
    int s_ortho = -1;

    float s_lookAt[3] = { 0.0f, 0.0f, 0.0f };
    float s_dist      = KCAM_DEF_DIST;
    bool  s_have      = false;

    // ── ROUND P: THE ORBIT LATCH (kiwi_camera.h "THE MMB JUMP") ─────────────
    // Everything an orbit needs is frozen at the PRESS and nothing re-derives it
    // until the release: the pivot, the eye's offset FROM that pivot, and the
    // angles the gesture started at.  Each drag frame then applies the TOTAL
    // accumulated pixel delta to those latched values — the grab-rebase discipline
    // round L gave the gizmo, applied to the camera.  See the header for why the
    // old "re-seat the eye on the pivot's axis" formulation could not not jump.
    bool  s_orbActive   = false;
    float s_orbPivot[3] = { 0.0f, 0.0f, 0.0f };
    float s_orbRel[3]   = { 0.0f, 0.0f, 0.0f };   // begin origin - pivot
    float s_orbPitch0   = 0.0f;
    float s_orbYaw0     = 0.0f;
    int   s_orbAccX     = 0;
    int   s_orbAccY     = 0;

    // ── shakeout E: the pan's per-gesture world-per-pixel anchor ────────────
    // See kiwi_camera.h "THE SHAKEOUT-E SENSITIVITY FIX".  Cached at PanBegin and
    // held for the whole drag so the scale cannot swing under the user's hand.
    float s_panK      = 0.0f;
    bool  s_panHave   = false;

    // ── shakeout E: the dolly's SMOOTHED reference distance ─────────────────
    // The step used to be "distance to the surface under the cursor, else the
    // orbit distance", which STEPS the moment the cursor crosses a silhouette
    // edge mid-scroll: one notch moves 15% of 40 units, the next 15% of 4000.
    // Consecutive notches inside KCAM_DOLLY_WINDOW seconds now blend the new
    // reference into the previous one, so an edge crossing ramps over two or
    // three notches instead of jumping.  A dolly the user has stopped and
    // restarted takes the new reference outright — that IS a new intent.
    const double KCAM_DOLLY_WINDOW = 0.35;     // seconds
    const float  KCAM_DOLLY_BLEND  = 0.5f;     // fraction of the NEW reference kept
    double s_dollyPrev = 0.0;
    float  s_dollyRef  = 0.0f;
    bool   s_dollyHave = false;

    // Fly state.  s_flyPrev is the previous tick's QPC reading in seconds;
    // s_flyWasd records whether the last tick had the RMB (WASD) arm live, which
    // is what KiwiCam_FlySwallowKey answers with.
    double s_flyPrev  = 0.0;
    bool   s_flyHave  = false;
    bool   s_flyWasd  = false;
    float  s_flyScale = -1.0f;                  // <0 = not read from the profile yet

    bool KeyDown( int vk )
    {
        return ( ::GetAsyncKeyState( vk ) & 0x8000 ) != 0;
    }

    // Wall-clock seconds from the same counter the pump's frame gate uses
    // (radiant_main.cpp's QueryPerformanceCounter cadence).  The pump caps frames
    // at 60 fps but does NOT guarantee them, so the fly must measure its own dt
    // rather than assume 1/60.
    double NowSeconds()
    {
        LARGE_INTEGER f, n;
        ::QueryPerformanceFrequency( &f );
        ::QueryPerformanceCounter( &n );
        return ( f.QuadPart > 0 ) ? (double)n.QuadPart / (double)f.QuadPart : 0.0;
    }

    inline float Dot3( const float *a, const float *b )
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    // The view forward for the CURRENT angles, through the editor's own basis call.
    void ViewForward( const camera_s *c, float *out )
    {
        float a[3] = { -c->angles[0], c->angles[1], c->angles[2] };
        float right[3], up[3];
        AngleVectors( a, out, right, up );
    }

    void PivotOnAxis( camera_s *c )
    {
        float f[3];
        ViewForward( c, f );
        for ( int i = 0; i < 3; ++i )
            s_lookAt[i] = c->origin[i] + f[i] * s_dist;
        s_have = true;
    }

    // Is the stored pivot still a sane thing to orbit around?  The legacy free-look
    // / fly paths move the camera without telling this layer, so a pivot from ten
    // minutes ago can be behind the camera or far off-axis; orbiting that would
    // teleport the view.  Anything in front and roughly on the view axis is kept
    // (spec §10: "miss -> keep current"); anything else is re-derived at the SAME
    // distance, which is the closest honest reading of the intent.
    bool PivotUsable( const camera_s *c )
    {
        if ( !s_have )
            return false;
        float f[3];
        ViewForward( c, f );
        const float rel[3] = { s_lookAt[0] - c->origin[0],
                               s_lookAt[1] - c->origin[1],
                               s_lookAt[2] - c->origin[2] };
        const float z = Dot3( rel, f );
        if ( z < 1.0f )
            return false;                        // behind / at the eye
        const float off[3] = { rel[0] - f[0] * z, rel[1] - f[1] * z, rel[2] - f[2] * z };
        const float lateral = sqrtf( Dot3( off, off ) );
        return lateral <= z * 0.5f;              // within ~26 degrees of the view axis
    }

    void Commit( camera_s *c )
    {
        CamWnd_BuildMatrix();                    // keep vpn/vright/vup in step immediately
        // The invalidation the ported camera moves use (CamWnd_PositionDrag 0x4035f0).
        g_nUpdateBits |= 4 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
    }

    // ── ROUND P: Rodrigues about an arbitrary unit axis ─────────────────────
    void RotateAbout( const float *v, const float *k, float deg, float *out )
    {
        const double th = DEG2RAD( deg );
        const float  cs = (float)cos( th );
        const float  sn = (float)sin( th );
        const float  kv = Dot3( k, v );
        const float  cx[3] = { k[1] * v[2] - k[2] * v[1],
                               k[2] * v[0] - k[0] * v[2],
                               k[0] * v[1] - k[1] * v[0] };
        for ( int i = 0; i < 3; ++i )
            out[i] = v[i] * cs + cx[i] * sn + k[i] * kv * ( 1.0f - cs );
    }

    // THE ORBIT ROTATION, and the reason it is derived rather than tabulated.
    //
    // With the file's own ANGLE CONVENTION the view forward is
    //     f(p,y) = ( cos p cos y, cos p sin y, sin p )
    // and CamWnd_BuildMatrix's roll-free RIGHT vector is
    //     r(y)   = ( sin y, -cos y, 0 )                       (horizontal, always)
    // Differentiating f by p gives exactly r(y) x f, so rotating ANY vector about
    // r(y0) by +dp degrees is the same rotation that takes the camera's pitch from
    // p0 to p0+dp; and rotating about +Z by +dy takes its yaw from y0 to y0+dy.
    //
    // Applying the two IN THAT ORDER to the eye's offset from the pivot therefore
    // moves the eye by the SAME rigid rotation the angles just underwent — so the
    // pivot (and everything at its depth) stays at exactly the same screen position
    // for the whole gesture, in perspective and in ortho alike.  That is what
    // "orbit around what I grabbed" means, and it is what the old formulation could
    // not do: it discarded the eye's real offset and re-seated the eye ON the
    // pivot's view axis instead (see kiwi_camera.h "THE MMB JUMP").
    void OrbitRotate( const float *v, float dpDeg, float dyDeg, float yaw0Deg,
                      float *out )
    {
        const double y0 = DEG2RAD( yaw0Deg );
        const float  ax[3] = { (float)sin( y0 ), (float)-cos( y0 ), 0.0f };
        const float  zax[3] = { 0.0f, 0.0f, 1.0f };
        float t[3];
        RotateAbout( v, ax, dpDeg, t );
        RotateAbout( t, zax, dyDeg, out );
    }

    // Freeze the whole orbit frame at the CURRENT camera + the CURRENT s_lookAt.
    // Also the re-latch a mid-gesture dolly runs (see KiwiCam_Dolly).
    void LatchOrbit( camera_s *c )
    {
        float rel[3] = { c->origin[0] - s_lookAt[0],
                         c->origin[1] - s_lookAt[1],
                         c->origin[2] - s_lookAt[2] };
        if ( sqrtf( Dot3( rel, rel ) ) < KCAM_MIN_DIST )
        {
            // The pivot is ON the eye: there is no offset to rotate.  Re-derive it
            // in front at the reference distance — the only honest reading left.
            PivotOnAxis( c );
            rel[0] = c->origin[0] - s_lookAt[0];
            rel[1] = c->origin[1] - s_lookAt[1];
            rel[2] = c->origin[2] - s_lookAt[2];
        }
        for ( int i = 0; i < 3; ++i )
        {
            s_orbPivot[i] = s_lookAt[i];
            s_orbRel[i]   = rel[i];
        }
        s_orbPitch0 = c->angles[0];
        s_orbYaw0   = c->angles[1];
        s_orbAccX   = 0;
        s_orbAccY   = 0;
        s_orbActive = true;
        s_have      = true;
    }
}

// ─── orbit ───────────────────────────────────────────────────────────────────
//
// ── ROUND P: THE PIVOT IS LATCHED ONCE AND NOTHING RE-DERIVES IT ────────────
// USER REPORT, verbatim: "having a bug where the camera jumps while using mmb.
// Mmb shouldn't jump the camera, just smoothly rotate it with the mouse.  Has to
// do with raytrace against the object and the mouse moving off the object while
// the camera rotates.  Keep the pos fixed so it doesn't jump the camera."
//
// The full diagnosis is in kiwi_camera.h.  The rule this function and the drag
// below now implement, in one sentence: EVERYTHING the orbit reads is frozen here
// at the press — the pivot, the eye's offset from it and the angles — and the
// drag applies the TOTAL accumulated pixel delta to those frozen values, so the
// first frame with a zero delta reproduces the camera exactly (the grab-rebase
// discipline, round L).  s_dist is NOT touched: in an ORTHO view it IS the zoom
// (KiwiCam_OrthoHalfHeight), and re-latching it from a surface pick was a second,
// independent jump — a press on a near wall instantly rescaled the whole image.
void KiwiCam_OrbitBegin( int imgX, int imgY )
{
    camera_s *c = Ed_Camera();

    bool havePivot = false;
    ray_t ray;
    if ( Pick_RayFromImagePos( imgX, imgY, &ray ) )
    {
        // Area granularity only: an orbit pivot wants the SURFACE point under the
        // cursor, not the nearest vertex/edge snap the mode mask might prefer.
        const pick_result_t r = Pick( ray, SEL_MASK_OBJECT | SEL_MASK_FACE );
        if ( r.valid )
        {
            s_lookAt[0] = r.point[0];
            s_lookAt[1] = r.point[1];
            s_lookAt[2] = r.point[2];
            s_have      = true;
            havePivot   = true;
        }
    }
    if ( !havePivot && !PivotUsable( c ) )
        PivotOnAxis( c );

    LatchOrbit( c );
}

void KiwiCam_OrbitDrag( int dx, int dy )
{
    camera_s *c = Ed_Camera();
    if ( !s_orbActive )
    {
        // A drag whose press never reached this file (the entry point is public).
        if ( !s_have )
            PivotOnAxis( c );
        LatchOrbit( c );
    }
    if ( dx == 0 && dy == 0 )
        return;

    // ACCUMULATED, and re-derived from the latch every frame rather than applied
    // incrementally: the pitch CLAMP must not desynchronise the eye from the
    // angles (an incremental form would keep rotating the eye after the pitch had
    // stopped moving), and a long orbit cannot drift.
    s_orbAccX += dx;
    s_orbAccY += dy;

    float pitch = s_orbPitch0 - (float)s_orbAccY * KCAM_DEG_PER_PX;
    if ( pitch >  89.0f ) pitch =  89.0f;
    if ( pitch < -89.0f ) pitch = -89.0f;
    const float dp   = pitch - s_orbPitch0;          // what the clamp ACTUALLY allowed
    const float dyaw = -(float)s_orbAccX * KCAM_DEG_PER_PX;

    c->angles[0] = pitch;
    // Keep yaw bounded so a long orbit session cannot drift into float mush.
    c->angles[1] = (float)fmod( (double)( s_orbYaw0 + dyaw ), 360.0 );

    float rel[3];
    OrbitRotate( s_orbRel, dp, dyaw, s_orbYaw0, rel );
    for ( int i = 0; i < 3; ++i )
    {
        s_lookAt[i]  = s_orbPivot[i];
        c->origin[i] = s_orbPivot[i] + rel[i];
    }
    s_have = true;

    Commit( c );
}

// Release the latch.  Called from the gesture's RELEASE and ABORT arms
// (kiwi_viewport.cpp) — until it runs, nothing may re-derive the pivot.
void KiwiCam_OrbitEnd()
{
    s_orbActive = false;
}

bool KiwiCam_OrbitLive()
{
    return s_orbActive;
}

// ─── dolly ───────────────────────────────────────────────────────────────────
void KiwiCam_Dolly( float wheelSteps, int imgX, int imgY )
{
    if ( wheelSteps == 0.0f )
        return;
    camera_s *c = Ed_Camera();
    // ROUND P: a wheel notch DURING a live orbit must not move the orbit's pivot —
    // that is the "nothing re-derives it until the release" rule, and this was the
    // one path that could reach in from outside the gesture (the wheel is
    // dispatched above the gesture arms in kiwi_viewport.cpp, deliberately: §4
    // says camera navigation never stops).  The pivot stands; the eye's offset from
    // it is re-latched at the bottom, so the dolly COMPOSES with the orbit instead
    // of fighting it.
    if ( s_orbActive )
    {
        for ( int i = 0; i < 3; ++i )
            s_lookAt[i] = s_orbPivot[i];
        s_have = true;
    }
    else if ( !s_have || !PivotUsable( c ) )
    {
        PivotOnAxis( c );
    }

    // Cursor ray; a viewport with no size yet falls back to the view axis.
    float dir[3];
    ray_t ray;
    bool  haveRay = Pick_RayFromImagePos( imgX, imgY, &ray );
    if ( haveRay )
    {
        dir[0] = ray.dir[0]; dir[1] = ray.dir[1]; dir[2] = ray.dir[2];
    }
    else
    {
        ViewForward( c, dir );
    }

    // The step is a fraction of the distance to whatever is UNDER THE CURSOR when
    // something is (Plasticity/Blender "zoom to mouse"), and of the orbit distance
    // otherwise.  Deriving it from the surface — not from the clamped orbit radius —
    // is what stops the dolly from creeping to a halt a few units short of a wall
    // and then crawling back out one notch at a time.
    float reference = s_dist;
    float surface   = 0.0f;                                // TRUE hit distance
    bool  haveHit   = false;
    if ( haveRay )
    {
        const pick_result_t r = Pick( ray, SEL_MASK_OBJECT | SEL_MASK_FACE );
        if ( r.valid )
        {
            const float rel[3] = { r.point[0] - c->origin[0],
                                   r.point[1] - c->origin[1],
                                   r.point[2] - c->origin[2] };
            reference = sqrtf( Dot3( rel, rel ) );
            surface   = reference;
            haveHit   = true;
        }
    }
    if ( reference < KCAM_MIN_DIST ) reference = KCAM_MIN_DIST;
    if ( reference > KCAM_MAX_DIST ) reference = KCAM_MAX_DIST;

    // KIWI-UX (shakeout E): blend consecutive notches so crossing a silhouette
    // edge mid-scroll ramps instead of stepping.  See the constants above.  This
    // smooths the STEP SIZE only — `surface` below stays the true hit distance,
    // because a blended reference can sit BEYOND the wall and the punch-through
    // clamp must never be measured against a number the geometry did not give it.
    {
        const double now = NowSeconds();
        if ( s_dollyHave && ( now - s_dollyPrev ) <= KCAM_DOLLY_WINDOW )
            reference = s_dollyRef + ( reference - s_dollyRef ) * KCAM_DOLLY_BLEND;
        s_dollyPrev = now;
        s_dollyRef  = reference;
        s_dollyHave = true;
        if ( reference < KCAM_MIN_DIST ) reference = KCAM_MIN_DIST;
        if ( reference > KCAM_MAX_DIST ) reference = KCAM_MAX_DIST;
    }

    // ── KIWI-UX (ROUND AJ, ITEM 3): the near-field ease ─────────────────────
    // One parameter drives both halves; see the constants for the derivation and
    // for the Plasticity measurements it is calibrated against.  At t == 1 (the
    // reference at or beyond KCAM_DOLLY_NEAR_DIST) every line below is exactly
    // what it was: step == KCAM_DOLLY_STEP and dir == the cursor ray.
    float t = reference / KCAM_DOLLY_NEAR_DIST;
    if ( t > 1.0f ) t = 1.0f;
    if ( !( t > 0.0f ) ) t = 0.0f;                         // also catches NaN

    if ( haveRay && t < 1.0f )
    {
        float fwd[3];
        ViewForward( c, fwd );
        const float aim = KCAM_DOLLY_AIM_MIN + ( 1.0f - KCAM_DOLLY_AIM_MIN ) * t;
        float eased[3];
        for ( int i = 0; i < 3; ++i )
            eased[i] = fwd[i] + ( dir[i] - fwd[i] ) * aim;
        const float len = sqrtf( Dot3( eased, eased ) );
        if ( len > 1.0e-4f )                               // degenerate blend: keep the ray
            for ( int i = 0; i < 3; ++i )
                dir[i] = eased[i] / len;
    }

    const float step = KCAM_DOLLY_STEP_NEAR
                     + ( KCAM_DOLLY_STEP - KCAM_DOLLY_STEP_NEAR ) * t;
    const float k = powf( step, wheelSteps );              // < 1 when zooming in
    float move = reference * ( 1.0f - k );                 // > 0 when zooming in
    if ( haveHit && move > surface - KCAM_MIN_DIST )
        move = surface - KCAM_MIN_DIST;                    // never punch through the surface

    for ( int i = 0; i < 3; ++i )
        c->origin[i] += dir[i] * move;

    s_dist -= move;
    if ( s_dist < KCAM_MIN_DIST ) s_dist = KCAM_MIN_DIST;
    if ( s_dist > KCAM_MAX_DIST ) s_dist = KCAM_MAX_DIST;

    Commit( c );
    if ( s_orbActive )
        LatchOrbit( c );   // ROUND P: pivot STANDS; re-latch the eye offset + angles
    else
        PivotOnAxis( c );  // re-seat the pivot in front at the new distance
}

// ─── accessors ───────────────────────────────────────────────────────────────
const float *KiwiCam_LookAt()
{
    if ( !s_have )
        PivotOnAxis( Ed_Camera() );
    return s_lookAt;
}

float KiwiCam_Distance()
{
    return s_dist;
}

// ═════════════════════════════════════════════════════════════════════════════
//  shakeout A — D-1 resolved: RMB mouselook, truck-pan, keyboard fly.
// ═════════════════════════════════════════════════════════════════════════════

// ─── ROUND M: the ORTHO toggle (kiwi_camera.h) ───────────────────────────────
bool KiwiCam_Ortho()
{
    if ( s_ortho < 0 )
        s_ortho = Radiant_ProfileGetInt( KCAM_SECTION, KCAM_ORTHO_ENTRY, 1 ) ? 1 : 0;
    return s_ortho != 0;
}

void KiwiCam_SetOrtho( bool on )
{
    const int v = on ? 1 : 0;
    if ( KiwiCam_Ortho() == ( v != 0 ) )
        return;
    s_ortho = v;
    Radiant_ProfileSetInt( KCAM_SECTION, KCAM_ORTHO_ENTRY, v );
    g_nUpdateBits = -1;                 // repaint: the projection changed
    // Same shape as KiwiUX_SetShowGrid/Axes (kiwi_ux.cpp:88-99): the SETTER syncs
    // the native View menu's check mark, so the view-cube pill, the palette and
    // the menu itself all leave it correct whichever one was used.  The sync
    // self-gates on the menu having been built, so this is a no-op headless.
    extern void KiwiWindows_SyncViewMenu();   // kiwi_windows.cpp
    KiwiWindows_SyncViewMenu();
}

// THE ortho half-height.  Deliberately the only place the number exists — the
// projection matrix (camwnd.cpp), the ray builder (camwnd.cpp), the projection
// inverse (kiwi_pick.cpp) and the screen scale (below) all call this, so a change
// here cannot leave one of the four behind.  See kiwi_camera.h for why it is
// s_dist * tan(fov/2) * 0.75 and not something simpler.
float KiwiCam_OrthoHalfHeight()
{
    float d = s_dist;
    if ( !( d > KCAM_MIN_DIST ) ) d = KCAM_MIN_DIST;
    if ( d > KCAM_MAX_DIST )      d = KCAM_MAX_DIST;
    const double t = tan( DEG2RAD( g_PrefsDlg->camera_fov ) * 0.5 );
    float h = (float)( (double)d * t * 0.75 );
    if ( !( h > 1.0e-3f ) )
        h = 1.0e-3f;
    return h;
}

// ─── KIWI-UX (ROUND AI, ITEM 2): the one-axis view gate ──────────────────────
// See kiwi_camera.h for the whole derivation.  `axis` is expected unit, but the
// test is written so a non-unit axis merely tightens it rather than passing
// something it should not, and a zero axis is refused.
bool KiwiCam_AxisPortrayable( const float *axis, float *outDot )
{
    if ( outDot )
        *outDot = 1.0f;                       // "worst case" if we cannot answer
    if ( !axis )
        return false;
    const float len2 = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
    if ( !( len2 > 1.0e-8f ) )
        return false;
    const camera_s *c = Ed_Camera();
    // CamWnd_BuildMatrix is what refreshes vpn from the angles; every caller of
    // this function runs inside a live gesture, i.e. after the frame's build, so
    // reading vpn directly here is the same value the pick ray was built from.
    float d = c->vpn[0] * axis[0] + c->vpn[1] * axis[1] + c->vpn[2] * axis[2];
    d /= sqrtf( len2 );
    if ( d < 0.0f )
        d = -d;
    if ( outDot )
        *outDot = d;
    return d < KCAM_AXIS_PORTRAY_DOT;
}

// ─── screen scale ────────────────────────────────────────────────────────────
float KiwiCam_WorldPerPixel( const float *world )
{
    const camera_s *c = Ed_Camera();
    if ( !world || c->height < 1 )
        return 1.0f;
    // ROUND M (// KIWI-UX): in an ORTHOGRAPHIC view the scale does not depend on
    // depth at all — one pixel is 2H/height world units everywhere, which is what
    // makes every screen-constant marker in the editor stay screen-constant.
    // Consistent with the perspective arm by construction: at the pivot depth
    // z = s_dist the two expressions are the same number (kiwi_camera.h).
    if ( KiwiCam_Ortho() )
        return 2.0f * KiwiCam_OrthoHalfHeight() / (float)c->height;
    // Identical to CameraCalcRayDir / kiwi_pick.cpp MakeProjCtx: s = (t*0.75 + t*0.75) / height.
    const double t = tan( DEG2RAD( g_PrefsDlg->camera_fov ) * 0.5 );
    const float  s = (float)( ( t * 0.75 + t * 0.75 ) / (double)c->height );
    const float rel[3] = { world[0] - c->origin[0],
                           world[1] - c->origin[1],
                           world[2] - c->origin[2] };
    float z = Dot3( rel, c->vpn );
    if ( z < 1.0f )
        z = 1.0f;
    return z * s;
}

// ─── mouselook (RMB drag) ────────────────────────────────────────────────────
void KiwiCam_LookDrag( int dx, int dy )
{
    if ( dx == 0 && dy == 0 )
        return;
    camera_s *c = Ed_Camera();

    // Same rate and same signs as CamWnd_Rotate2 (camwnd.cpp:2598) and as the
    // orbit above, so mouselook and orbit never feel like two different cameras.
    c->angles[1] -= (float)dx * KCAM_DEG_PER_PX;
    c->angles[0] -= (float)dy * KCAM_DEG_PER_PX;

    if ( c->angles[0] >  89.0f ) c->angles[0] =  89.0f;
    if ( c->angles[0] < -89.0f ) c->angles[0] = -89.0f;
    c->angles[1] = (float)fmod( (double)c->angles[1], 360.0 );

    Commit( c );
    // The camera POSITION did not move, so the old pivot is now off to one side.
    // Re-seat it in front at the same distance: an orbit started right after a
    // look must turn around what the user is now looking at, not around whatever
    // was in front before the look.
    PivotOnAxis( c );
}

// ─── truck-pan (Shift+RMB / Shift+MMB drag) ──────────────────────────────────
void KiwiCam_Translate( const float *delta )
{
    if ( !delta || ( delta[0] == 0.0f && delta[1] == 0.0f && delta[2] == 0.0f ) )
        return;
    camera_s *c = Ed_Camera();
    for ( int i = 0; i < 3; ++i )
    {
        c->origin[i] += delta[i];
        s_lookAt[i]  += delta[i];
        // ROUND P: a translation that happens while an orbit is latched carries the
        // WHOLE frame with it — pivot and eye move together, so the latched offset
        // is unchanged and the orbit does not jump.  (No gesture can reach here
        // during an orbit today — the shell owns one gesture at a time — but the
        // entry point is public and this is one line.)
        if ( s_orbActive )
            s_orbPivot[i] += delta[i];
    }
    s_have = true;
    Commit( c );
}

// KIWI-UX (shakeout E): anchor the pan's world-per-pixel scale on WHAT IS UNDER
// THE CURSOR, once, at the press.  kiwi_camera.h carries the diagnosis of the
// "sensitivity changes a lot with zoom" complaint this fixes.
void KiwiCam_PanBegin( int imgX, int imgY )
{
    camera_s *c = Ed_Camera();
    if ( !s_have )
        PivotOnAxis( c );
    CamWnd_BuildMatrix();                        // WorldPerPixel reads camera.vpn

    s_panHave = false;

    ray_t ray;
    if ( Pick_RayFromImagePos( imgX, imgY, &ray ) )
    {
        // Area granularity only — a pan wants the SURFACE the user has their
        // finger on, not the nearest vertex the mode mask might prefer.  Same
        // rule, and the same one call, as KiwiCam_OrbitBegin.
        const pick_result_t r = Pick( ray, SEL_MASK_OBJECT | SEL_MASK_FACE );
        if ( r.valid )
        {
            s_panK    = KiwiCam_WorldPerPixel( r.point );
            s_panHave = ( s_panK > 0.0f );
        }
    }
    if ( !s_panHave )
    {
        // Nothing under the cursor: the orbit pivot is the only honest reference
        // left, which is exactly what every pan used before shakeout E.
        s_panK    = KiwiCam_WorldPerPixel( s_lookAt );
        s_panHave = ( s_panK > 0.0f );
    }
}

void KiwiCam_PanEnd()
{
    s_panHave = false;
    s_panK    = 0.0f;
}

void KiwiCam_PanDrag( int dx, int dy )
{
    if ( dx == 0 && dy == 0 )
        return;
    camera_s *c = Ed_Camera();
    if ( !s_have )
        PivotOnAxis( c );
    CamWnd_BuildMatrix();                        // vright/vup for this frame's angles

    // k at the GESTURE'S anchor depth (the surface the press landed on), so the
    // grabbed point tracks the cursor ~1:1.  The pivot fallback is what a pan
    // begun outside this file's PanBegin — none today, but the entry point is
    // public — would otherwise silently get wrong.
    const float k = s_panHave ? s_panK : KiwiCam_WorldPerPixel( s_lookAt );
    float d[3];
    for ( int i = 0; i < 3; ++i )
        d[i] = -c->vright[i] * ( (float)dx * k ) + c->vup[i] * ( (float)dy * k );
    KiwiCam_Translate( d );
}

// ─── view-cube mutator ───────────────────────────────────────────────────────
void KiwiCam_LookAlong( float pitch, float yaw )
{
    camera_s *c = Ed_Camera();
    // ROUND P: this is an ABSOLUTE re-aim, so any latched orbit frame is stale the
    // moment it runs.  Dropping it here means a snap that somehow lands mid-gesture
    // cannot be undone by the next drag frame re-applying the old latch.
    s_orbActive = false;
    if ( !s_have )
        PivotOnAxis( c );

    c->angles[0] = pitch;
    c->angles[1] = yaw;
    c->angles[2] = 0.0f;
    if ( c->angles[0] >  89.0f ) c->angles[0] =  89.0f;
    if ( c->angles[0] < -89.0f ) c->angles[0] = -89.0f;

    float f[3];
    ViewForward( c, f );
    for ( int i = 0; i < 3; ++i )
        c->origin[i] = s_lookAt[i] - f[i] * s_dist;

    Commit( c );
}

// ─── SHAKEOUT I: the modern map-new / map-load camera placement ──────────────
// See KCAM_SPAWN_* above for the numbers and the reasoning.  Called as a
// // KIWI-UX post-init hook from the two ported placements that leave the camera
// at the world origin, and ONLY when the modern input profile is on — the classic
// profile must keep the ported placement byte for byte.
void KiwiCam_DefaultSpawn()
{
    camera_s *c = Ed_Camera();
    c->origin[0] = KCAM_SPAWN_POS[0];
    c->origin[1] = KCAM_SPAWN_POS[1];
    c->origin[2] = KCAM_SPAWN_POS[2];
    c->angles[0] = KCAM_SPAWN_PITCH;
    c->angles[1] = KCAM_SPAWN_YAW;
    c->angles[2] = 0.0f;

    // Seat the ORBIT PIVOT on the world origin at the true standoff, so the very
    // first MMB drag turntables around what the user is looking at instead of
    // re-deriving a pivot at KCAM_DEF_DIST along the view axis (which for this
    // placement would land short of the origin and swing the view on the first
    // gesture).
    s_lookAt[0] = 0.0f;
    s_lookAt[1] = 0.0f;
    s_lookAt[2] = 0.0f;
    const float rel[3] = { -KCAM_SPAWN_POS[0], -KCAM_SPAWN_POS[1], -KCAM_SPAWN_POS[2] };
    float d = sqrtf( Dot3( rel, rel ) );
    if ( d < KCAM_MIN_DIST ) d = KCAM_MIN_DIST;
    if ( d > KCAM_MAX_DIST ) d = KCAM_MAX_DIST;
    s_dist    = d;
    s_have    = true;
    s_panHave = false;                    // the cached pan scale belonged to the old view
    s_dollyHave = false;
    s_orbActive = false;                  // ROUND P: …and so did any latched orbit frame

    Commit( c );
}

// ─── ROUND J: frame a bounding box (Plasticity `viewport:focus`) ─────────────
// See kiwi_camera.h.  Direction is preserved; only the standoff and the pivot
// move, which is exactly what Plasticity's NavigationControls.focus does (it
// re-targets the orbit controls and dollies, it never re-orients).
void KiwiCam_FrameBounds( const float mins[3], const float maxs[3] )
{
    if ( !mins || !maxs )
        return;

    camera_s *c = Ed_Camera();

    const float centre[3] = { ( mins[0] + maxs[0] ) * 0.5f,
                              ( mins[1] + maxs[1] ) * 0.5f,
                              ( mins[2] + maxs[2] ) * 0.5f };
    const float half[3]   = { ( maxs[0] - mins[0] ) * 0.5f,
                              ( maxs[1] - mins[1] ) * 0.5f,
                              ( maxs[2] - mins[2] ) * 0.5f };
    float radius = sqrtf( Dot3( half, half ) );
    if ( radius < KCAM_FRAME_MIN_RADIUS )
        radius = KCAM_FRAME_MIN_RADIUS;

    // THE SAME projection constant the ray builder and WorldPerPixel use — see
    // KiwiCam_WorldPerPixel above: the per-pixel scale is (t*0.75 + t*0.75)/height,
    // so the half-image height in world units at depth z is z * 0.75 * t, i.e.
    // tan(halfVFov) == 0.75 * tan(fov/2).  The horizontal half-angle follows from
    // the aspect.  Fitting the SMALLER of the two is what makes the box fit both.
    const double t     = tan( DEG2RAD( g_PrefsDlg->camera_fov ) * 0.5 );
    float        tanV  = (float)( t * 0.75 );
    if ( tanV < 1.0e-3f )
        tanV = 1.0e-3f;
    float tanH = tanV;
    if ( c->height > 0 && c->width > 0 )
        tanH = tanV * ( (float)c->width / (float)c->height );
    const float tanFit = ( tanH < tanV ) ? tanH : tanV;

    float dist = ( radius / tanFit ) * KCAM_FRAME_MARGIN;
    if ( dist < KCAM_MIN_DIST ) dist = KCAM_MIN_DIST;
    if ( dist > KCAM_MAX_DIST ) dist = KCAM_MAX_DIST;

    float f[3];
    ViewForward( c, f );
    for ( int i = 0; i < 3; ++i )
    {
        s_lookAt[i]  = centre[i];
        c->origin[i] = centre[i] - f[i] * dist;
    }
    s_dist      = dist;
    s_have      = true;
    s_panHave   = false;                  // the cached pan scale belonged to the old view
    s_dollyHave = false;                  // …and so did the dolly's smoothed reference
    s_orbActive = false;                  // ROUND P: …and so did any latched orbit frame

    Commit( c );
}

// ─── fly-speed multiplier (persisted) ────────────────────────────────────────
float KiwiCam_FlySpeedScale()
{
    if ( s_flyScale < 0.0f )
    {
        // Stored x100 as an int so it round-trips through the int profile helper
        // (the float entries in kiwi_units.cpp use the string helper because they
        //  must survive arbitrary precision; a speed multiplier does not).
        //
        // SHAKEOUT I: 10x -> 4x.  The 10x was set in shakeout A when the fly at 1x
        // (350 units/s = 29 ft/s) felt dead-slow, and it felt dead-slow because the
        // camera was starting 21 feet out with a 15%-per-notch dolly — i.e. the fly
        // was compensating for the two numbers above.  With those fixed, 10x
        // (3500 u/s, 290 ft/s, 3x that on Shift) overshoots anything you aim at.
        // 4x = 1400 u/s ~ 117 ft/s, which crosses a 4096-unit map in ~3 s.
        // THE SLIDER IS UNCHANGED (KiwiCam_SetFlySpeedScale, 0.01x..100x): this is a
        // DEFAULT, and a user who liked 10x sets it back in one drag.
        // (Entry renamed FlySpeedScale2 -> FlySpeedScale3 so a persisted 1000 from
        //  the shakeout-A build cannot pin the old default — the same trick that
        //  rename made when it moved 1x -> 10x.)
        const int pct = Radiant_ProfileGetInt( KCAM_SECTION, KCAM_FLY_ENTRY, 400 );
        s_flyScale = (float)pct * 0.01f;
        if ( !( s_flyScale > 0.01f ) ) s_flyScale = 0.01f;
        if ( s_flyScale > 100.0f )     s_flyScale = 100.0f;
    }
    return s_flyScale;
}

void KiwiCam_SetFlySpeedScale( float mul )
{
    if ( !( mul > 0.01f ) ) mul = 0.01f;
    if ( mul > 100.0f )     mul = 100.0f;
    s_flyScale = mul;
    Radiant_ProfileSetInt( KCAM_SECTION, KCAM_FLY_ENTRY, (int)( mul * 100.0f + 0.5f ) );
}

// ─── keyboard fly ────────────────────────────────────────────────────────────
// USER DIRECTIVE (shakeout A follow-up): ARROW KEYS ONLY.  WASD clashes with the
// Plasticity-style bindings this overhaul exists for (S = Scale, and W/A/D are
// one remap away from mattering) — so the fly is arrows, full stop.  While
// RMB-look is held the arrow WM_KEYDOWNs are swallowed here so the classic
// modified-arrow bindings cannot also fire; while merely hovering, only BARE
// arrows fly (Shift/Ctrl/Alt+arrow stay classic bindings in both profiles).
bool KiwiCam_FlySwallowKey( unsigned int vk )
{
    if ( !s_flyWasd )                        // = RMB-look held (see FlyTick)
        return false;
    return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT;
}

void KiwiCam_FlyTick( bool wasd, bool arrows )
{
    const double now = NowSeconds();
    double dt = s_flyHave ? ( now - s_flyPrev ) : 0.0;
    s_flyPrev = now;
    s_flyHave = true;
    s_flyWasd = wasd;

    if ( !wasd && !arrows )
        return;
    if ( !( dt > 0.0 ) )
        return;
    if ( dt > KCAM_FLY_MAX_DT )
        dt = KCAM_FLY_MAX_DT;

    // GetAsyncKeyState reports the PHYSICAL key regardless of focus, so gate on
    // this thread owning the foreground: typing WASD in another app must not fly
    // the editor's camera.
    if ( ::GetActiveWindow() == nullptr )
        return;

    // ARROWS ONLY (user directive — WASD clashed with the modern bindings).
    // While RMB-look is held (`wasd` param = lookHeld) modifiers are ignored so
    // Shift can boost; while merely hovering, bare arrows only — Shift/Ctrl/Alt +
    // arrow are live classic bindings (TexShift*, TexRotate*, SelectNudge* —
    // mainfrm.cpp:1109-1143) that keep their meaning in both keymap profiles.
    // (kiwi_keymap.cpp unbinds the classic bare-arrow CameraForward/Back/Left/
    // Right tank hops in the modern profile.)
    int fwd = 0, side = 0, vert = 0;
    const bool anyMod = KeyDown( VK_SHIFT ) || KeyDown( VK_CONTROL ) || KeyDown( VK_MENU );
    if ( wasd || ( arrows && !anyMod ) )
    {
        if ( KeyDown( VK_UP    ) ) ++fwd;
        if ( KeyDown( VK_DOWN  ) ) --fwd;
        if ( KeyDown( VK_RIGHT ) ) ++side;
        if ( KeyDown( VK_LEFT  ) ) --side;
    }
    if ( !fwd && !side && !vert )
        return;

    camera_s *c = Ed_Camera();
    CamWnd_BuildMatrix();                        // vpn/vright/vup for this frame's angles

    // prefs.h:38 `int m_nMoveSpeed;  // "MoveSpeed"  (350)` — the classic camera
    // paths use it as a rate (Cam_MouseControl: units/s at full joystick
    // deflection is m_nMoveSpeed * 6; CamWnd_Scroll: units per wheel notch is
    // m_nMoveSpeed * 0.7 * modifier), so reading it as units/SECOND here puts the
    // fly at a familiar 350 u/s with the stock pref.
    float speed = (float)g_PrefsDlg->m_nMoveSpeed * KiwiCam_FlySpeedScale();
    if ( wasd && KeyDown( VK_SHIFT ) )
        speed *= KCAM_FLY_SHIFT;
    const float step = speed * (float)dt;

    float d[3];
    for ( int i = 0; i < 3; ++i )
        d[i] = c->vpn[i]    * ( (float)fwd  * step )
             + c->vright[i] * ( (float)side * step )
             + c->vup[i]    * ( (float)vert * step );

    // The pivot rides along, so a fly followed by an orbit does not swing the view
    // around a point the user has flown past.
    KiwiCam_Translate( d );
}

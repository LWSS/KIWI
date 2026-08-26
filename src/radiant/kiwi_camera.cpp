#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// RADIANT_UX_DESIGN §10 camera implementation.
// CamWnd_BuildMatrix negates pitch before AngleVectors, so positive pitch looks up
// (camwnd.cpp 0x403470). Drag signs match Cam_Rotate at 0x403700.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                 // camera_s
#include "prefs.h"                   // g_PrefsDlg (m_bCamXYUpdate, m_nMoveSpeed, camera_fov)
#include <universal/com_math.h>      // AngleVectors
#include "kiwi_camera.h"
#include "kiwi_pick.h"
#include "radiant_registry.h"        // Radiant_Profile* (the fly-speed multiplier)
#include "kiwi_vec.h"     // shared Dot3/Sub3/... helpers

#include <math.h>

// Ported entry points.
extern camera_s *Ed_Camera();          // camwnd.cpp
extern void      CamWnd_BuildMatrix(); // camwnd.cpp 0x403470
extern int       g_nUpdateBits;        // 0x25D5A74 (mainfrm.cpp)

namespace
{
    const float KCAM_DEG_PER_PX  = 0.35f;      // Cam_Rotate2's rate (0x4037c0)
    // One viewport-height drag makes one full turn on both axes. Before sizing,
    // fall back to Cam_Rotate2's 0.35 degrees per pixel.
    float KCam_OrbitDegPerPx( const camera_s *c )
    {
        if ( !c || c->height < 16 )
            return KCAM_DEG_PER_PX;
        return 360.0f / (float)c->height;
    }

    // Axis snaps may use exact ±90: the roll-free basis remains defined at a pole.
    // Drags stop at ±89 to avoid crossing the pole and mirroring the view.
    const float KCAM_PITCH_DRAG_LIMIT = 89.0f;
    const float KCAM_PITCH_MAX        = 90.0f;   // the pole, and the absolute bound

    float KCam_PitchDragLimit( float pitch0 )
    {
        const float a = fabsf( pitch0 );
        if ( !( a > KCAM_PITCH_DRAG_LIMIT ) )    // also catches a NaN latch
            return KCAM_PITCH_DRAG_LIMIT;
        return ( a > KCAM_PITCH_MAX ) ? KCAM_PITCH_MAX : a;
    }

    // Widen only the starting pole's side, avoiding a first-pixel jump without
    // making the opposite pole reachable during the drag.
    void KCam_PitchDragBounds( float pitch0, float *outLo, float *outHi )
    {
        float lo = -KCAM_PITCH_DRAG_LIMIT;
        float hi =  KCAM_PITCH_DRAG_LIMIT;
        const float w = KCam_PitchDragLimit( pitch0 );
        if ( w > KCAM_PITCH_DRAG_LIMIT )
        {
            if ( pitch0 > 0.0f ) hi =  w;
            else                 lo = -w;
        }
        *outLo = lo;
        *outHi = hi;
    }
    // Map units remain inches; these constants tune only view scale and navigation.
    // A flat five-percent wheel factor matches Plasticity. Ortho cursor anchoring
    // is closed-form, so it needs no distance-dependent easing.
    const float KCAM_DOLLY_STEP  = 0.95f;      // multiplicative per wheel step
    // One inch permits detail work; perspective surface anchoring prevents punch-through.
    const float KCAM_MIN_DIST    = 1.0f;

    // Equal and opposite wheel deltas must use reciprocal factors.  Computing the
    // zoom-out arm as the reciprocal of the same positive-magnitude powf keeps the
    // algebra symmetric instead of asking two independently rounded powf calls to
    // happen to be exact inverses.
    float KCam_DollyFactor( float wheelSteps )
    {
        if ( wheelSteps > 0.0f )
            return powf( KCAM_DOLLY_STEP, wheelSteps );
        return 1.0f / powf( KCAM_DOLLY_STEP, -wheelSteps );
    }
    // Ortho clips against a symmetric ±KCAM_ORTHO_DEPTH_HALF slab about its
    // pseudo-eye. Limit standoff to half that half-depth, reserving equal depth
    // behind the pivot; 524288 also keeps 24-bit linear depth at 1/16-unit steps.
    const float KCAM_MAX_DIST_ORTHO   = KCAM_ORTHO_DEPTH_HALF * 0.5f; // 262144
    // Perspective has an infinite far plane, but its float32 inverse-VP fails on
    // far-from-origin cameras; 65536 stays clear of the observed ~-175000 failure
    // (camwnd.cpp:230-235).
    const float KCAM_MAX_DIST_PERSP   = 65536.0f;   // inverse-VP-safe standoff
    // The ortho pseudo-eye needs the wider leash to realize its standoff around a
    // map; perspective stays inside the engine's ±131072 world bound.
    const float KCAM_ORIGIN_BOUND_ORTHO = KCAM_ORTHO_DEPTH_HALF;   // 524288
    const float KCAM_ORIGIN_BOUND_PERSP = 131072.0f;   // engine world bound
    // Extreme FOV cannot expand the ortho half-height independently of standoff.
    const float KCAM_ORTHO_HALF_MAX = KCAM_ORTHO_DEPTH_HALF * 0.25f;  // 131072
    const float KCAM_DEF_DIST    = 96.0f;      // eight-foot default standoff

    // Select the projection-specific ceiling and fold the half-height cap into the
    // distance driver; clamping only the derived height creates wheel dead zones.
    float KCam_MaxDist()
    {
        if ( !KiwiCam_Ortho() )
            return KCAM_MAX_DIST_PERSP;
        float d = KCAM_MAX_DIST_ORTHO;
        // The same tan the half-height uses (KiwiCam_OrthoHalfHeight below), so the
        // two cannot disagree about where the ceiling is.
        const double t = tan( DEG2RAD( g_PrefsDlg->camera_fov ) * 0.5 ) * 0.75;
        if ( t > 1.0e-6 )
        {
            const float byHeight = (float)( (double)KCAM_ORTHO_HALF_MAX / t );
            if ( byHeight < d )
                d = byHeight;
        }
        if ( !( d > KCAM_MIN_DIST ) )       // a pathological fov must not invert the range
            d = KCAM_MIN_DIST;
        return d;
    }

    float KCam_OriginBound()
    {
        return KiwiCam_Ortho() ? KCAM_ORIGIN_BOUND_ORTHO : KCAM_ORIGIN_BOUND_PERSP;
    }

    // The modern profile starts at (0,-160,96), yaw +90 and pitch -31: a human-scale
    // three-quarter view of the origin. Frame margin leaves about 7% on the tight
    // axis; the radius floor avoids parking inside the near plane.
    const float KCAM_FRAME_MARGIN     = 1.15f;
    const float KCAM_FRAME_MIN_RADIUS = 16.0f;

    const float KCAM_SPAWN_POS[3] = { 0.0f, -160.0f, 96.0f };
    const float KCAM_SPAWN_PITCH  = -31.0f;
    const float KCAM_SPAWN_YAW    =  90.0f;

    // Fly tuning.
    const float  KCAM_FLY_SHIFT   = 3.0f;      // Shift boost
    const double KCAM_FLY_MAX_DT  = 0.1;       // a stall must not teleport the camera

    // Ortho arrows pan at a fixed screen-pixel rate. Normalize the persisted speed
    // by its default so the multiplier is applied once in both projections.
    const float KCAM_FLY_PX_PER_SEC = 600.0f;  // ortho arrows: screen px per second at 1x
    const float KCAM_FLY_SCALE_DEF  = 4.0f;    // the shipped KiwiCam_FlySpeedScale default
    const char  *KCAM_SECTION     = "KiwiUX";
    const char  *KCAM_FLY_ENTRY   = "FlySpeedScale3";   // v3: default moved 10x -> 4x
    // Versioned profile keys let changed defaults reach profiles with old values.
    const char  *KCAM_ORTHO_ENTRY = "CameraOrtho2";     // default ON

    // -1 = not read from the profile yet.
    int s_ortho = -1;

    float s_lookAt[3] = { 0.0f, 0.0f, 0.0f };
    float s_dist      = KCAM_DEF_DIST;
    bool  s_have      = false;

    // Freeze pivot, eye offset, and angles at press; every drag frame applies total
    // accumulated delta to this latch, preventing a first-frame re-seat jump.
    bool  s_orbActive   = false;
    float s_orbPivot[3] = { 0.0f, 0.0f, 0.0f };
    float s_orbRel[3]   = { 0.0f, 0.0f, 0.0f };   // begin origin - pivot
    float s_orbPitch0   = 0.0f;
    float s_orbYaw0     = 0.0f;
    int   s_orbAccX     = 0;
    // Float because a pitch-clamp rewind can land on a fractional pixel count.
    float s_orbAccY     = 0.0f;

    // Cache world-per-pixel for the gesture so pan scale cannot change mid-drag.
    float s_panK      = 0.0f;
    bool  s_panHave   = false;

    // QPC timestamp and the last tick's RMB-look ownership.
    double s_flyPrev     = 0.0;
    bool   s_flyHave     = false;
    bool   s_flyLookHeld = false;
    float  s_flyScale = -1.0f;                  // <0 = not read from the profile yet

    bool KeyDown( int vk )
    {
        return ( ::GetAsyncKeyState( vk ) & 0x8000 ) != 0;
    }

    // Measure fly dt because the 60-fps pump cadence is a cap, not a guarantee.
    double NowSeconds()
    {
        // QueryPerformanceFrequency is constant for the process lifetime.
        static const LONGLONG freq = []() -> LONGLONG
        {
            LARGE_INTEGER f;
            ::QueryPerformanceFrequency( &f );
            return f.QuadPart;
        }();
        LARGE_INTEGER n;
        ::QueryPerformanceCounter( &n );
        return ( freq > 0 ) ? (double)n.QuadPart / (double)freq : 0.0;
    }


}   // Pause the namespace: KiwiCam_RayAxis is declared with external linkage.

// Shared closest-point-on-an-axis solve.
bool KiwiCam_RayAxis( const ray_t &ray, const float *pt, const float *axis, float *out )
{
    float w0[3];
    Sub3( pt, ray.origin, w0 );
    const float b = Dot3( axis, ray.dir );
    const float d = Dot3( axis, w0 );
    const float e = Dot3( ray.dir, w0 );
    const float den = 1.0f - b * b;
    // den is sin²(theta); reject the same 14-degree cone as the view gate.
    if ( fabsf( den ) < KCAM_RAYAXIS_MIN_DEN )
        return false;
    const float s = ( b * e - d ) / den;
    Mad3( pt, axis, s, out );
    return true;
}

namespace
{
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

    // Legacy camera paths can stale the pivot. Keep it only when it remains in
    // front and roughly on-axis; otherwise rederive it at the same distance.
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

    // Clamp the scale factor itself so distance and cursor correction remain one
    // transform instead of Commit truncating only the eye afterward.
    float ScaleFactorInsideWorld( const camera_s *c, const float *toAnchor, float factor )
    {
        if ( !c || !toAnchor || !( factor > 0.0f ) )
            return 1.0f;                         // also rejects NaN

        const float travel = 1.0f - factor;
        if ( travel == 0.0f )
            return factor;

        const float bound = KCam_OriginBound();
        float allowed = 1.0f;                    // fraction of the requested eye delta
        for ( int i = 0; i < 3; ++i )
        {
            const float delta = toAnchor[i] * travel;
            float room = 0.0f;
            if ( delta > 0.0f )
                room = bound - c->origin[i];
            else if ( delta < 0.0f )
                room = c->origin[i] + bound;
            else
                continue;

            if ( !( room > 0.0f ) )
            {
                allowed = 0.0f;
                break;
            }
            const float need = fabsf( delta );
            if ( room < need )
            {
                const float a = room / need;
                if ( a < allowed )
                    allowed = a;
            }
        }
        return 1.0f - travel * allowed;
    }

    // Carry the pivot by the origin's clamp correction so the eye/pivot pair stays
    // rigid. The bound is projection-specific.
    void ClampToWorld( camera_s *c )
    {
        const float bound = KCam_OriginBound();      // projection-specific eye leash
        for ( int i = 0; i < 3; ++i )
        {
            float v = c->origin[i];
            if ( !( v == v ) ) v = 0.0f;                       // NaN: the only honest answer
            if ( v >  bound ) v =  bound;
            if ( v < -bound ) v = -bound;
            const float fix = v - c->origin[i];
            if ( fix == 0.0f )
                continue;
            c->origin[i] = v;
            s_lookAt[i] += fix;
            if ( s_orbActive )
                s_orbPivot[i] += fix;
        }
    }

    void Commit( camera_s *c )
    {
        ClampToWorld( c );                       // enforce the eye leash
        CamWnd_BuildMatrix();                    // keep vpn/vright/vup in step immediately
        // The invalidation the ported camera moves use (CamWnd_PositionDrag 0x4035f0).
        g_nUpdateBits |= 4 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
    }

    // Rodrigues rotation about a unit axis.
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

    // Apply pitch about roll-free right r(y)=(sin y,-cos y,0), then yaw about +Z.
    // This is the same rigid rotation as the angles, preserving pivot screen position.
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
        s_orbAccY   = 0.0f;
        s_orbActive = true;
        s_have      = true;
    }
}

// Orbit: latch pivot, eye offset, and angles once, then apply total drag delta.
// Do not relatch s_dist from the surface: in ortho it is the zoom driver.
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

    // On a cursor miss, use the framed surface's center-pick depth. Keep a stored
    // pivot only within one viewport-height of that depth; mismatch makes orbit
    // slew grow as ortho zoom narrows.
    if ( !havePivot )
    {
        ray_t cray;
        pick_result_t centre;                    // .valid defaults false (kiwi_pick.h:82)
        if ( Pick_RayFromImagePos( c->width / 2, c->height / 2, &cray ) )
            centre = Pick( cray, SEL_MASK_OBJECT | SEL_MASK_FACE );
        if ( centre.valid )
        {
            float f[3];
            ViewForward( c, f );
            const float relC[3] = { centre.point[0] - c->origin[0],
                                    centre.point[1] - c->origin[1],
                                    centre.point[2] - c->origin[2] };
            const float zc = Dot3( relC, f );
            bool keepStored = false;
            if ( PivotUsable( c ) )
            {
                const float relP[3] = { s_lookAt[0] - c->origin[0],
                                        s_lookAt[1] - c->origin[1],
                                        s_lookAt[2] - c->origin[2] };
                const float zp   = Dot3( relP, f );
                const float band = (float)c->height * KiwiCam_WorldPerPixel( centre.point );
                keepStored = ( fabsf( zp - zc ) <= band );
            }
            if ( !keepStored )
            {
                s_lookAt[0] = centre.point[0];
                s_lookAt[1] = centre.point[1];
                s_lookAt[2] = centre.point[2];
                s_have      = true;
            }
            havePivot = true;
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

    // Recompute from the latch and total delta so the pitch clamp cannot
    // desynchronize eye and angles, and long drags cannot accumulate drift.
    s_orbAccX += dx;
    s_orbAccY += (float)dy;

    // Resolution-relative rate: one viewport height is one full turn on both axes.
    const float degPerPx = KCam_OrbitDegPerPx( c );
    // ±89, widened on ONE side to the latched pitch when the orbit began on a
    // pole-exact axis view — see KCam_PitchDragLimit / KCam_PitchDragBounds.
    float lo = 0.0f, hi = 0.0f;
    KCam_PitchDragBounds( s_orbPitch0, &lo, &hi );
    float pitch = s_orbPitch0 - s_orbAccY * degPerPx;
    // Rewind refused travel at the clamp; otherwise pixels banked against a pole
    // must all be dragged back before the view moves.
    if ( pitch > hi )
    {
        pitch     = hi;
        s_orbAccY = ( s_orbPitch0 - hi ) / degPerPx;
    }
    else if ( pitch < lo )
    {
        pitch     = lo;
        s_orbAccY = ( s_orbPitch0 - lo ) / degPerPx;
    }
    const float dp   = pitch - s_orbPitch0;          // what the clamp ACTUALLY allowed
    const float dyaw = -(float)s_orbAccX * degPerPx;

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

// Release on both gesture release and abort; until then the pivot stays latched.
void KiwiCam_OrbitEnd()
{
    s_orbActive = false;
}

// Dolly.
void KiwiCam_Dolly( float wheelSteps, int imgX, int imgY )
{
    if ( !( wheelSteps > 0.0f || wheelSteps < 0.0f ) )       // zero or NaN
        return;
    camera_s *c = Ed_Camera();
    // A wheel notch during orbit keeps the world-space pivot and relatches the eye
    // offset afterward, so dolly composes with the active gesture.
    if ( s_orbActive )
    {
        for ( int i = 0; i < 3; ++i )
            s_lookAt[i] = s_orbPivot[i];
        s_have = true;
    }

    // Ortho zoom scales s_dist and pans so the cursor's world point is invariant:
    // for screen offset q, C' = C + q*wpp*(1-k). The pick-ray origin supplies q.
    // Derive C from the current eye, not a possibly off-axis orbit pivot, and clamp
    // s_dist before deriving the one effective scale factor.
    if ( KiwiCam_Ortho() )
    {
        const float maxDist = KCam_MaxDist();
        float d0 = s_dist;
        if ( !( d0 > KCAM_MIN_DIST ) ) d0 = KCAM_MIN_DIST;
        if ( d0 > maxDist )            d0 = maxDist;

        float newDist = d0 * KCam_DollyFactor( wheelSteps );        // < d0 zooming IN
        if ( !( newDist > KCAM_MIN_DIST ) ) newDist = KCAM_MIN_DIST;   // also catches NaN
        if ( newDist > maxDist )            newDist = maxDist;
        float kEff = newDist / d0;            // what the distance clamps ACTUALLY allowed

        float f[3];
        ViewForward( c, f );
        float toAnchor[3];
        for ( int i = 0; i < 3; ++i )
            toAnchor[i] = f[i] * d0;          // old eye -> old view centre

        ray_t oray;
        if ( Pick_RayFromImagePos( imgX, imgY, &oray ) )
        {
            const float rel[3] = { oray.origin[0] - c->origin[0],
                                   oray.origin[1] - c->origin[1],
                                   oray.origin[2] - c->origin[2] };
            const float offR = Dot3( rel, c->vright );
            const float offU = Dot3( rel, c->vup    );
            for ( int i = 0; i < 3; ++i )
                toAnchor[i] += c->vright[i] * offR + c->vup[i] * offU;
        }

        // Apply the leash to the same factor before axial and cursor motion diverge.
        kEff   = ScaleFactorInsideWorld( c, toAnchor, kEff );
        newDist = d0 * kEff;

        // A clamped notch must not re-seat an off-axis pivot behind the user's back.
        if ( kEff == 1.0f )
            return;

        float move[3];
        for ( int i = 0; i < 3; ++i )
        {
            move[i] = toAnchor[i] * ( 1.0f - kEff );
            c->origin[i] += move[i];
        }

        // Outside orbit, rebase from the eye; during orbit, keep the world pivot.
        if ( !s_orbActive )
        {
            for ( int i = 0; i < 3; ++i )
                s_lookAt[i] = c->origin[i] + f[i] * newDist;
        }
        s_dist = newDist;
        s_have = true;

        Commit( c );
        if ( s_orbActive )
            LatchOrbit( c );                  // pivot stands; refresh eye offset + angles
        return;
    }

    // Cursor ray; a viewport with no size yet falls back to the view axis.
    float dir[3];
    ray_t ray;
    const bool haveRay = Pick_RayFromImagePos( imgX, imgY, &ray );
    if ( haveRay )
    {
        dir[0] = ray.dir[0]; dir[1] = ray.dir[1]; dir[2] = ray.dir[2];
    }
    else
    {
        ViewForward( c, dir );
    }

    // One factor drives distance and cursor correction; differing factors cause a
    // sideways move at a zoom boundary.
    const float maxDist = KCam_MaxDist();
    float d0 = s_dist;
    if ( !( d0 > KCAM_MIN_DIST ) ) d0 = KCAM_MIN_DIST;
    if ( d0 > maxDist )            d0 = maxDist;

    float newDist = d0 * KCam_DollyFactor( wheelSteps );
    if ( !( newDist > KCAM_MIN_DIST ) ) newDist = KCAM_MIN_DIST;
    if ( newDist > maxDist )            newDist = maxDist;
    float kEff = newDist / d0;

    // Anchor at the cursor-ray hit, or d0 down that ray on a miss; retain no stale
    // value from a previous notch.
    float toAnchor[3] = { dir[0] * d0, dir[1] * d0, dir[2] * d0 };
    float surface = d0;
    bool haveHit = false;
    if ( haveRay )
    {
        const pick_result_t r = Pick( ray, SEL_MASK_OBJECT | SEL_MASK_FACE );
        if ( r.valid )
        {
            const float rel[3] = { r.point[0] - c->origin[0],
                                   r.point[1] - c->origin[1],
                                   r.point[2] - c->origin[2] };
            const float hitDist = sqrtf( Dot3( rel, rel ) );
            if ( hitDist > 0.0f )                       // also rejects NaN
            {
                Copy3( rel, toAnchor );
                surface = hitDist;
                haveHit = true;
            }
        }
    }

    // Clamp the factor so punch-through guard, distance, and lateral correction
    // describe one transform; inside the guard, an inward notch is a no-op.
    if ( haveHit && kEff < 1.0f )
    {
        if ( !( surface > KCAM_MIN_DIST ) )
            kEff = 1.0f;
        else
        {
            const float hitFloor = KCAM_MIN_DIST / surface;
            if ( kEff < hitFloor )
                kEff = hitFloor;
        }
    }

    // The eye leash is the final effective-factor clamp.
    kEff   = ScaleFactorInsideWorld( c, toAnchor, kEff );
    newDist = d0 * kEff;
    if ( newDist < KCAM_MIN_DIST ) newDist = KCAM_MIN_DIST;
    if ( newDist > maxDist )       newDist = maxDist;

    // A saturated notch changes neither pose nor pivot.  Otherwise this is the
    // literal scale-about-anchor identity O' = A + kEff*(O-A).
    if ( kEff == 1.0f )
        return;
    for ( int i = 0; i < 3; ++i )
        c->origin[i] += toAnchor[i] * ( 1.0f - kEff );

    s_dist = newDist;

    Commit( c );
    if ( s_orbActive )
        LatchOrbit( c );   // pivot stands; relatch eye offset and angles
    else
        PivotOnAxis( c );  // re-seat the pivot in front at the new distance
}

// Accessors.
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

// Orthographic/perspective toggle.
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

    // If the new projection has a lower distance ceiling, preserve the current
    // eye-derived view center and shorten standoff on-axis; an off-axis cached
    // pivot must not add lateral motion to this clamp.
    {
        camera_s   *c    = Ed_Camera();
        const float maxd = KCam_MaxDist();
        if ( s_dist > maxd )
        {
            float f[3];
            ViewForward( c, f );
            for ( int i = 0; i < 3; ++i )
                c->origin[i] += f[i] * ( s_dist - maxd );
            s_dist = maxd;
            PivotOnAxis( c );
            s_orbActive = false;        // the latched offset belonged to the old standoff
            s_panHave   = false;        // …and so did the cached pan scale
            Commit( c );
        }
    }

    g_nUpdateBits = -1;                 // repaint: the projection changed
    // Keep the native View menu synchronized; the helper self-gates headless use.
    extern void KiwiWindows_SyncViewMenu();   // kiwi_windows.cpp
    KiwiWindows_SyncViewMenu();
}

// Single ortho half-height source for matrix, rays, inverse, and screen scale:
// s_dist * tan(fov/2) * 0.75.
float KiwiCam_OrthoHalfHeight()
{
    float d = s_dist;
    const float maxDist = KCam_MaxDist();
    if ( !( d > KCAM_MIN_DIST ) ) d = KCAM_MIN_DIST;
    if ( d > maxDist )            d = maxDist;
    const double t = tan( DEG2RAD( g_PrefsDlg->camera_fov ) * 0.5 );
    float h = (float)( (double)d * t * 0.75 );
    if ( !( h > 1.0e-3f ) )
        h = 1.0e-3f;
    // KCam_MaxDist normally makes this cap unreachable; retain it as a last guard
    // against an unclamped direct s_dist write.
    if ( h > KCAM_ORTHO_HALF_MAX )
        h = KCAM_ORTHO_HALF_MAX;
    return h;
}

// Zoom meter uses the same min and projection-specific max as the wheel, so its
// endpoints coincide with the clamps.
bool KiwiCam_ZoomMeter( float *outFrac, float *outWpp )
{
    if ( outFrac ) *outFrac = 0.0f;
    if ( outWpp )  *outWpp  = 0.0f;

    const camera_s *c = Ed_Camera();
    if ( c->height < 1 )
        return false;

    const float lo = KCAM_MIN_DIST;
    const float hi = KCam_MaxDist();
    if ( !( hi > lo ) )
        return false;                        // a pathological fov collapsed the range

    float d = s_dist;
    if ( !( d > lo ) ) d = lo;               // also catches NaN
    if ( d > hi )      d = hi;

    // log(d/lo) / log(hi/lo).  Both logs are of numbers >= 1 and the denominator is
    // > 0 by the guard above, so this cannot divide by zero or take a log of <= 0.
    const double f = log( (double)d / (double)lo ) / log( (double)hi / (double)lo );
    float frac = (float)f;
    if ( !( frac > 0.0f ) ) frac = 0.0f;
    if ( frac > 1.0f )      frac = 1.0f;

    if ( outFrac )
        *outFrac = frac;
    if ( outWpp )
    {
        if ( !s_have )
            PivotOnAxis( Ed_Camera() );
        CamWnd_BuildMatrix();                // WorldPerPixel reads camera.vpn
        *outWpp = KiwiCam_WorldPerPixel( s_lookAt );
    }
    return true;
}

// One-axis view gate; non-unit input is normalized and a zero axis is refused.
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
    // Callers run after the frame build, so vpn matches the basis used by picking.
    float d = c->vpn[0] * axis[0] + c->vpn[1] * axis[1] + c->vpn[2] * axis[2];
    d /= sqrtf( len2 );
    if ( d < 0.0f )
        d = -d;
    if ( outDot )
        *outDot = d;
    return d < KCAM_AXIS_PORTRAY_DOT;
}

// Screen scale.
float KiwiCam_WorldPerPixel( const float *world )
{
    const camera_s *c = Ed_Camera();
    if ( !world || c->height < 1 )
        return 1.0f;
    // Ortho scale is depth-independent: 2H/height. At z=s_dist it equals the
    // perspective expression by construction.
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

// Mouselook.
void KiwiCam_LookDrag( int dx, int dy )
{
    if ( dx == 0 && dy == 0 )
        return;
    camera_s *c = Ed_Camera();

    // Rate and signs match CamWnd_Rotate2 (camwnd.cpp:4245). Incremental one-sided
    // bounds let a pole-exact view leave smoothly, then shrink back to ±89 without
    // making the opposite pole reachable.
    float lo = 0.0f, hi = 0.0f;
    KCam_PitchDragBounds( c->angles[0], &lo, &hi );
    c->angles[1] -= (float)dx * KCAM_DEG_PER_PX;
    c->angles[0] -= (float)dy * KCAM_DEG_PER_PX;

    if ( c->angles[0] > hi ) c->angles[0] = hi;
    if ( c->angles[0] < lo ) c->angles[0] = lo;
    c->angles[1] = (float)fmod( (double)c->angles[1], 360.0 );

    Commit( c );
    // Re-seat the unchanged-position camera's pivot on its new view axis.
    PivotOnAxis( c );
}

// Truck-pan.
void KiwiCam_Translate( const float *delta )
{
    if ( !delta || ( delta[0] == 0.0f && delta[1] == 0.0f && delta[2] == 0.0f ) )
        return;
    camera_s *c = Ed_Camera();
    for ( int i = 0; i < 3; ++i )
    {
        c->origin[i] += delta[i];
        s_lookAt[i]  += delta[i];
        // Carry a live orbit pivot so translation preserves the latched eye offset.
        if ( s_orbActive )
            s_orbPivot[i] += delta[i];
    }
    s_have = true;
    Commit( c );
}

// Cache the pivot's world-per-pixel scale once per pan gesture.
void KiwiCam_PanBegin( int imgX, int imgY )
{
    camera_s *c = Ed_Camera();
    if ( !s_have )
        PivotOnAxis( c );
    CamWnd_BuildMatrix();                        // WorldPerPixel reads camera.vpn

    // Pan uses world-per-pixel at the pivot, matching Plasticity's target. Translation
    // moves eye and pivot together, so this scale is invariant for the gesture.
    (void)imgX;
    (void)imgY;
    s_panK    = KiwiCam_WorldPerPixel( s_lookAt );
    s_panHave = ( s_panK > 0.0f );
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

    // Use the cached gesture scale, or pivot scale for a direct public call.
    const float k = s_panHave ? s_panK : KiwiCam_WorldPerPixel( s_lookAt );
    float d[3];
    for ( int i = 0; i < 3; ++i )
        d[i] = -c->vright[i] * ( (float)dx * k ) + c->vup[i] * ( (float)dy * k );
    KiwiCam_Translate( d );
}

// 2D marker anchor.
void KiwiCam_MarkerViewpoint( float out3[3] )
{
    if ( !out3 )
        return;
    const camera_s *c = Ed_Camera();
    Copy3( c->origin, out3 );
    if ( !KiwiCam_Ortho() )
        return;                              // perspective: the eye IS the place
    // Walk the ortho pseudo-eye to its standoff plane; during an off-axis orbit,
    // this remains the on-axis point at the same depth.
    float f[3];
    ViewForward( c, f );
    Mad3( out3, f, s_dist, out3 );
}

// Pose signature.
namespace
{
    float s_poseOrigin[3] = { 0.0f, 0.0f, 0.0f };
    float s_poseAngles[3] = { 0.0f, 0.0f, 0.0f };
    bool  s_poseHave      = false;
}

bool KiwiCam_PoseChangedSinceLastTick()
{
    const camera_s *c = Ed_Camera();
    bool changed = !s_poseHave;
    for ( int i = 0; i < 3; ++i )
    {
        // Exact compare, not an epsilon: the question is "did anything write the
        // pose", and a mutator that moved the camera by a hair still has to show.
        if ( c->origin[i] != s_poseOrigin[i] || c->angles[i] != s_poseAngles[i] )
            changed = true;
        s_poseOrigin[i] = c->origin[i];
        s_poseAngles[i] = c->angles[i];
    }
    s_poseHave = true;
    return changed;
}

// Camera-facing side of a plane.
float KiwiCam_FacingSign( const float point[3], const float normal[3] )
{
    if ( !point || !normal )
        return 1.0f;
    const camera_s *c = Ed_Camera();
    float d;
    if ( KiwiCam_Ortho() )
    {
        // A parallel projection has no meaningful eye point; facing is determined
        // by view direction, derived through the draw's AngleVectors path.
        float f[3];
        ViewForward( c, f );
        d = -Dot3( f, normal );
    }
    else
    {
        const float rel[3] = { c->origin[0] - point[0],
                               c->origin[1] - point[1],
                               c->origin[2] - point[2] };
        d = Dot3( rel, normal );
    }
    // Exactly edge-on (d == 0, and NaN) keeps the normal's own side, so the answer
    // is total and a degenerate frame changes nothing.
    return ( d < 0.0f ) ? -1.0f : 1.0f;
}

// View-cube mutator.
void KiwiCam_LookAlong( float pitch, float yaw )
{
    camera_s *c = Ed_Camera();
    // An absolute re-aim invalidates any latched orbit frame.
    s_orbActive = false;
    if ( !s_have )
        PivotOnAxis( c );

    c->angles[0] = pitch;
    c->angles[1] = yaw;
    c->angles[2] = 0.0f;
    // Absolute axis snaps use exact ±90 so top/bottom views do not leak side faces;
    // no caller may aim past a pole.
    if ( c->angles[0] >  KCAM_PITCH_MAX ) c->angles[0] =  KCAM_PITCH_MAX;
    if ( c->angles[0] < -KCAM_PITCH_MAX ) c->angles[0] = -KCAM_PITCH_MAX;

    float f[3];
    ViewForward( c, f );
    for ( int i = 0; i < 3; ++i )
        c->origin[i] = s_lookAt[i] - f[i] * s_dist;

    Commit( c );
}

// Modern-profile map-new/map-load placement; the classic profile keeps the ported pose.
void KiwiCam_DefaultSpawn()
{
    camera_s *c = Ed_Camera();
    c->origin[0] = KCAM_SPAWN_POS[0];
    c->origin[1] = KCAM_SPAWN_POS[1];
    c->origin[2] = KCAM_SPAWN_POS[2];
    c->angles[0] = KCAM_SPAWN_PITCH;
    c->angles[1] = KCAM_SPAWN_YAW;
    c->angles[2] = 0.0f;

    // Seat the pivot at the origin and record its true standoff so the first orbit
    // does not swing around a nearer KCAM_DEF_DIST fallback.
    s_lookAt[0] = 0.0f;
    s_lookAt[1] = 0.0f;
    s_lookAt[2] = 0.0f;
    const float rel[3] = { -KCAM_SPAWN_POS[0], -KCAM_SPAWN_POS[1], -KCAM_SPAWN_POS[2] };
    float d = sqrtf( Dot3( rel, rel ) );
    if ( d < KCAM_MIN_DIST )  d = KCAM_MIN_DIST;
    if ( d > KCam_MaxDist() ) d = KCam_MaxDist();
    s_dist    = d;
    s_have    = true;
    s_panHave = false;                    // the cached pan scale belonged to the old view
    s_orbActive = false;                  // discard any latched orbit frame

    Commit( c );
}

// Frame a box without changing view direction; move only pivot and standoff.
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

    // Use the ray builder's tan(halfVFov)=0.75*tan(fov/2); fit the smaller of the
    // vertical and aspect-derived horizontal half-angles.
    const double t     = tan( DEG2RAD( g_PrefsDlg->camera_fov ) * 0.5 );
    float        tanV  = (float)( t * 0.75 );
    if ( tanV < 1.0e-3f )
        tanV = 1.0e-3f;
    float tanH = tanV;
    if ( c->height > 0 && c->width > 0 )
        tanH = tanV * ( (float)c->width / (float)c->height );
    const float tanFit = ( tanH < tanV ) ? tanH : tanV;

    float dist = ( radius / tanFit ) * KCAM_FRAME_MARGIN;
    if ( dist < KCAM_MIN_DIST )  dist = KCAM_MIN_DIST;
    if ( dist > KCam_MaxDist() ) dist = KCam_MaxDist();

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
    s_orbActive = false;                  // discard any latched orbit frame

    Commit( c );
}

// Persisted fly-speed multiplier.
float KiwiCam_FlySpeedScale()
{
    if ( s_flyScale < 0.0f )
    {
        // Store x100 through the integer profile helper. The versioned key prevents
        // an old 10x default from overriding the current 4x default.
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

// Keyboard fly uses arrows only. During RMB look they are swallowed from the
// message path; while hovering, only bare arrows fly so modified bindings remain.
bool KiwiCam_FlySwallowKey( unsigned int vk )
{
    if ( !s_flyLookHeld )
        return false;
    return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT;
}

void KiwiCam_FlyTick( bool lookHeld, bool arrows )
{
    const double now = NowSeconds();
    double dt = s_flyHave ? ( now - s_flyPrev ) : 0.0;
    s_flyPrev     = now;
    s_flyHave     = true;
    s_flyLookHeld = lookHeld;

    if ( !lookHeld && !arrows )
        return;
    if ( !( dt > 0.0 ) )
        return;
    if ( dt > KCAM_FLY_MAX_DT )
        dt = KCAM_FLY_MAX_DT;

    // GetAsyncKeyState ignores focus, so require this thread to own the foreground.
    if ( ::GetActiveWindow() == nullptr )
        return;

    // RMB look ignores modifiers so Shift can boost; hover requires bare arrows,
    // preserving classic modified-arrow bindings. There is deliberately no Q/E
    // vertical term.
    int fwd = 0, side = 0;
    const bool anyMod = KeyDown( VK_SHIFT ) || KeyDown( VK_CONTROL ) || KeyDown( VK_MENU );
    if ( lookHeld || ( arrows && !anyMod ) )
    {
        if ( KeyDown( VK_UP    ) ) ++fwd;
        if ( KeyDown( VK_DOWN  ) ) --fwd;
        if ( KeyDown( VK_RIGHT ) ) ++side;
        if ( KeyDown( VK_LEFT  ) ) --side;
    }
    if ( !fwd && !side )
        return;

    camera_s *c = Ed_Camera();
    CamWnd_BuildMatrix();                        // vpn/vright/vup for this frame's angles

    // MoveSpeed is a units-per-second rate here, matching classic camera use.
    float speed = (float)g_PrefsDlg->m_nMoveSpeed * KiwiCam_FlySpeedScale();
    // In ortho arrows pan along vup/vright at a constant screen-pixel rate; motion
    // along vpn would be invisible in a parallel projection. Normalize the speed
    // slider by its default so it is counted once.
    const bool ortho = KiwiCam_Ortho();
    float d[3];
    if ( ortho )
    {
        float mul = KiwiCam_FlySpeedScale() / KCAM_FLY_SCALE_DEF;
        if ( !( mul > 0.0f ) ) mul = 1.0f;            // also catches NaN
        if ( lookHeld && KeyDown( VK_SHIFT ) )
            mul *= KCAM_FLY_SHIFT;
        // The pivot is the reference for the same reason the pan uses it — and in
        // ortho this function is depth-independent anyway, so any point would do.
        const float step = KCAM_FLY_PX_PER_SEC * mul
                         * KiwiCam_WorldPerPixel( s_lookAt ) * (float)dt;
        for ( int i = 0; i < 3; ++i )
            d[i] = c->vup[i]     * ( (float)fwd  * step )
                 + c->vright[i]  * ( (float)side * step );
    }
    else
    {
        if ( lookHeld && KeyDown( VK_SHIFT ) )
            speed *= KCAM_FLY_SHIFT;
        const float step = speed * (float)dt;
        for ( int i = 0; i < 3; ++i )
            d[i] = c->vpn[i]    * ( (float)fwd  * step )
                 + c->vright[i] * ( (float)side * step );
    }

    // The pivot rides along, so a fly followed by an orbit does not swing the view
    // around a point the user has flown past.
    KiwiCam_Translate( d );
}

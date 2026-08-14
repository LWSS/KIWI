#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_grid.cpp — RADIANT_UX_DESIGN §17 implementation.
//
// ROUND M: this file is a REWRITE.  The whole diagnosis (why the frustum
// footprint collapsed at shallow pitch), the v3 design, the budget arithmetic and
// the honest account of what "anti aliasing" means on this path live in
// kiwi_grid.h.  Read that first — this file is the mechanical half.
//
// What is GONE, deliberately, and each of these was a defect source in its own
// right rather than a feature this round is trading away:
//   * the four corner rays and the Z=0 intersection (the pitch coupling),
//   * the span-driven auto-coarsen loop (the density-with-angle coupling),
//   * the six distance bands and their majors-only outer half (lines that
//     appeared and disappeared as the camera moved),
//   * the projected-pixel-gap cull and the 512-slot per-line Schmitt table with
//     its slot-ownership bookkeeping (a whole subsystem whose only job was to
//     stop the previous item flickering).
// What is UNCHANGED: the API, both kiwi_ux.h toggles, KiwiGrid_Snap, the axes,
// and the discipline of declaring a hard segment budget up front.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "kiwi_grid.h"
#include "kiwi_lines.h"
#include "kiwi_units.h"
#include "kiwi_ux.h"
#include "kiwi_camera.h"    // ROUND AJ, ITEM 5 — KiwiCam_Ortho (no lattice in P mode)
#include "radiant_registry.h"   // ROUND AJ, ITEM 5 — the grid-snap preference

#include <math.h>

// ── ported entry points (verified against their definitions) ────────────────
extern camera_s *Ed_Camera();          // camwnd.cpp:151
extern void      CamWnd_BuildMatrix(); // camwnd.cpp:162 (0x403470)
extern int       Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:112
extern int       g_nUpdateBits;                        // 0x25D5A74 (mainfrm.cpp)
// KIWI-UX (ROUND AG, ITEM 10): the far-ring probe.  Same forwarder the unified
// picker uses (kiwi_pick.cpp:27), so the ortho branch is handled for free.
extern void      Ed_CameraCalcRayDir( int x, int y, float *dir ); // camwnd.cpp:3144

namespace
{
    // Axis half-length — the engine's own world bound (kiwi_grid.h).
    const float KGRID_AXIS_LEN = KGRID_MAX_EXTENT;

    // [band][major] grey.  Three DISTANCE bands, two BRIGHTNESS levels.  Both
    // levels exist in every band: a band never removes a line, it only dims it.
    const float KGRID_BANDS[KGRID_BAND_COUNT][2][3] =
    {
        { { 0.30f, 0.30f, 0.32f }, { 0.48f, 0.48f, 0.52f } },   // 0 near
        { { 0.24f, 0.24f, 0.26f }, { 0.39f, 0.39f, 0.43f } },   // 1 mid
        { { 0.17f, 0.17f, 0.19f }, { 0.28f, 0.28f, 0.31f } },   // 2 far
    };

    // Emission order for the six (band, major) colour runs: nearest and brightest
    // first, so a (budget-impossible, see kiwi_grid.h) exhaustion would drop the
    // least informative lines.  Encoded band*2 + major.
    const int KGRID_PASS_ORDER[KGRID_BAND_COUNT * 2] = { 1, 0,  3, 2,  5, 4 };

    // ── the LOD latch ───────────────────────────────────────────────────────
    // ONE integer for the whole grid (kiwi_grid.h THE LOD).  Reset when the user
    // changes the base spacing, because the thresholds are all relative to it.
    int   s_lod        = 0;
    float s_lodBase    = 0.0f;
    // KIWI-UX (ROUND AK): which REGIME last drove the latch — 0 = the perspective
    // altitude ladder, 1 = the ortho pixel ladder (kiwi_grid.h THE ORTHOGRAPHIC
    // GRID).  The two ladders measure different quantities against different
    // thresholds, so carrying `k` across a P-toggle would hand the new regime a
    // tier its own test never chose.  -1 = nothing has driven it yet.
    int   s_lodMode    = -1;

    // ROUND AG: the far ring's own Schmitt latch (kiwi_grid.h THE FAR RING).  One
    // bool for the whole grid, exactly as the LOD is one integer.
    bool  s_farOn      = false;

    inline float Dot3( const float *a, const float *b )
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    // Cheap behind-the-eye cull: drop a segment only when BOTH ends and the
    // midpoint are behind the eye plane.  Conservative — a segment that crosses
    // the plane is kept whole and clipped by the renderer.
    bool BehindEye( const camera_s *c, const float *a, const float *b )
    {
        const float mid[3] = { ( a[0] + b[0] ) * 0.5f, ( a[1] + b[1] ) * 0.5f, ( a[2] + b[2] ) * 0.5f };
        const float ra[3]  = { a[0] - c->origin[0], a[1] - c->origin[1], a[2] - c->origin[2] };
        const float rb[3]  = { b[0] - c->origin[0], b[1] - c->origin[1], b[2] - c->origin[2] };
        const float rm[3]  = { mid[0] - c->origin[0], mid[1] - c->origin[1], mid[2] - c->origin[2] };
        return Dot3( ra, c->vpn ) <= 0.0f
            && Dot3( rb, c->vpn ) <= 0.0f
            && Dot3( rm, c->vpn ) <= 0.0f;
    }

    // Shakeout A / C: the three world axes, each SPLIT AT THE ORIGIN (six
    // segments) and both halves at full brightness — the dimmed negative half
    // shakeout A introduced read as a rendering bug, so shakeout C undid it and
    // kept only the split (which costs nothing and keeps the per-half cull).
    // Colours are desaturated so the axes do not compete with SELECTED geometry
    // (pure white) or with the transform accents (kiwi_transform.cpp KX_AXIS_COL,
    // the saturated versions of these same hues).
    void DrawAxes( const camera_s *c )
    {
        static const float axisCol[3][3] =
        {
            { 0.72f, 0.28f, 0.30f },   // X red
            { 0.30f, 0.66f, 0.34f },   // Y green
            { 0.32f, 0.44f, 0.78f },   // Z blue
        };
        const float origin[3] = { 0.0f, 0.0f, 0.0f };
        for ( int ax = 0; ax < 3; ++ax )
        {
            float pos[3] = { 0.0f, 0.0f, 0.0f };
            float neg[3] = { 0.0f, 0.0f, 0.0f };
            pos[ax] =  KGRID_AXIS_LEN;
            neg[ax] = -KGRID_AXIS_LEN;

            KiwiLines_Color( axisCol[ax][0], axisCol[ax][1], axisCol[ax][2] );
            if ( !BehindEye( c, origin, pos ) )
                KiwiLines_Add( origin, pos );
            if ( !BehindEye( c, origin, neg ) )
                KiwiLines_Add( origin, neg );
        }
    }

    // ── the frame's lattice window ──────────────────────────────────────────
    // Everything the two emission passes need, and NOTHING in it reads pitch.
    struct window_t
    {
        float spacing;      // world units per cell (base * 2^lod)
        // KIWI-UX (ROUND AK): PER AXIS.  The perspective path sets both to the
        // same R = spacing * KGRID_HALF_CELLS and is bit-for-bit what it was; the
        // ortho path derives an EXACT, generally anisotropic footprint (a 16:9
        // view rect projected onto Z=0 is wider than it is deep), and forcing that
        // through one scalar would either clip the sides or overdraw the depth.
        float reach[2];     // half-extent in X and in Y
        float centre[2];    // window centre in XY
        float ground[2];    // the camera's own ground projection (band distances)
        bool  ok;
    };

    // Pick the LOD exponent from the camera's HEIGHT above Z=0, with the Schmitt
    // band documented in kiwi_grid.h.  Bounded: each iteration moves `k` by one
    // and the two tests cannot both be true (KGRID_LOD_DOWN < KGRID_LOD_UP), so
    // the guard is belt-and-braces against a NaN height rather than a real limit.
    int PickLod( float base, float height )
    {
        // KIWI-UX (ROUND AK): …or the REGIME changed (kiwi_grid.h).
        if ( s_lodBase != base || s_lodMode != 0 )
        {
            s_lodBase = base;
            s_lodMode = 0;
            s_lod     = 0;
        }
        int k = s_lod;
        if ( k < 0 )              k = 0;
        if ( k > KGRID_LOD_MAX )  k = KGRID_LOD_MAX;

        for ( int guard = 0; guard < KGRID_LOD_MAX * 2 + 4; ++guard )
        {
            const float cellK = base * (float)( 1 << k );
            if ( k < KGRID_LOD_MAX
              && height > cellK * KGRID_CELLS_PER_HEIGHT * KGRID_LOD_UP )
            {
                ++k;
                continue;
            }
            if ( k > 0 )
            {
                const float cellDn = base * (float)( 1 << ( k - 1 ) );
                if ( height < cellDn * KGRID_CELLS_PER_HEIGHT * KGRID_LOD_DOWN )
                {
                    --k;
                    continue;
                }
            }
            break;
        }
        s_lod = k;
        return k;
    }

    bool BuildWindow( const camera_s *c, window_t &w )
    {
        w.ok = false;
        const float base = KiwiUnits_GridSpacingWorld();
        if ( !( base > 0.0f ) )
            return false;

        // HEIGHT, not distance-to-anything: the one quantity the LOD is allowed
        // to depend on.  fabsf so a camera below the plane behaves the same.
        float height = fabsf( c->origin[2] );
        if ( !( height >= 0.0f ) )      // NaN
            height = 0.0f;

        const int k = PickLod( base, height );
        w.spacing = base * (float)( 1 << k );
        if ( !( w.spacing > 0.0f ) )
            return false;
        float R = w.spacing * (float)KGRID_HALF_CELLS;
        // CLAMPED, not rejected.  Past ~20000 units of altitude the height-driven
        // LOD asks for a reach beyond the engine's world bound; clamping there
        // keeps drawing (the window simply holds fewer than 2*KGRID_HALF_CELLS
        // cells, which only tightens the budget) where rejecting would make the
        // grid vanish at high altitude — the exact class of bug this round exists
        // to remove.
        if ( R > KGRID_MAX_EXTENT )
            R = KGRID_MAX_EXTENT;
        if ( !( R > 0.0f ) )
            return false;
        w.reach[0] = R;                 // ROUND AK: isotropic here, by design
        w.reach[1] = R;

        w.ground[0] = c->origin[0];
        w.ground[1] = c->origin[1];

        // The forward bias.  camera.forward is the YAW-PLANE forward
        // CamWnd_BuildMatrix builds from { 0, yaw, 0 } (camwnd.cpp:169-171), so it
        // is a unit vector in the XY plane and carries NO pitch — which is the
        // whole reason it is used here rather than vpn.  A degenerate basis (never
        // seen: AngleVectors always normalises) falls back to no bias.
        const float fwd[2] = { c->forward[0], c->forward[1] };
        const float fl     = sqrtf( fwd[0] * fwd[0] + fwd[1] * fwd[1] );
        const float bias   = ( fl > 1.0e-3f ) ? ( R * KGRID_FWD_BIAS / fl ) : 0.0f;
        w.centre[0] = w.ground[0] + fwd[0] * bias;
        w.centre[1] = w.ground[1] + fwd[1] * bias;

        w.ok = true;
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AK, ITEM 2) — THE ORTHOGRAPHIC PATH
    // ═══════════════════════════════════════════════════════════════════════
    // The design, the diagnosis of what altitude-driven LOD does to an ortho
    // image, the 1/sin decision and the declared budget are all in kiwi_grid.h
    // (THE ORTHOGRAPHIC GRID IS DERIVED FROM WORLD-PER-PIXEL).  Read that first.
    // Nothing below reads camera altitude, the pivot, or any pitch term except the
    // one closed-form footprint projection — and that one only ever sets REACH.

    // The ortho LOD ladder, in PIXELS.  Same shape as PickLod: one integer, one
    // Schmitt band, bounded iteration.  The two tests cannot both fire because
    // KGRID_PX_REFINE > 2 * KGRID_PX_MIN (coarsening doubles px, refining halves
    // it), so the guard is belt-and-braces against a NaN wpp.
    int PickLodOrtho( float base, float wpp )
    {
        if ( s_lodBase != base || s_lodMode != 1 )
        {
            s_lodBase = base;
            s_lodMode = 1;
            s_lod     = 0;
        }
        int k = s_lod;
        if ( k < 0 )              k = 0;
        if ( k > KGRID_LOD_MAX )  k = KGRID_LOD_MAX;
        if ( !( wpp > 0.0f ) )
            return k;

        for ( int guard = 0; guard < KGRID_LOD_MAX * 2 + 4; ++guard )
        {
            const float px = base * (float)( 1 << k ) / wpp;
            if ( k < KGRID_LOD_MAX && px < KGRID_PX_MIN )
            {
                ++k;
                continue;
            }
            if ( k > 0 && px > KGRID_PX_REFINE )
            {
                --k;
                continue;
            }
            break;
        }
        s_lod = k;
        return k;
    }

    // The visible ground footprint's half-extent on each world axis, EXACTLY, plus
    // the screen-centre ground hit it is centred on.  Ortho only — every step here
    // is false under a perspective divide.
    //
    //   halfW / halfH   the view rect in world units (wpp is constant, so this is
    //                   the whole rect and not a local approximation)
    //   vright          screen-right.  CamWnd_BuildMatrix builds the basis from
    //                   { 0, yaw, 0 } for forward and the full angles for the rest;
    //                   with no roll, vright is horizontal, so its own projection
    //                   onto Z = 0 is itself.
    //   g               screen-up, projected onto Z = 0 ALONG THE VIEW DIRECTION:
    //                   a point p + h*vup lands at p + h*(vup - vpn*(vup2/vpn2)).
    //                   This is where the 1/sin lives, and it is the ONLY place.
    void OrthoFootprint( const camera_s *c, float halfW, float halfH,
                         float *needOut, float *hitOut )
    {
        // vpn[2] floored at KGRID_ORTHO_MIN_SIN, SIGN PRESERVED.  Below that angle
        // round S's edge ramp already owns the frame (kiwi_grid.h).
        float vpnz = c->vpn[2];
        if ( vpnz >= 0.0f && vpnz <  KGRID_ORTHO_MIN_SIN ) vpnz =  KGRID_ORTHO_MIN_SIN;
        if ( vpnz <  0.0f && vpnz > -KGRID_ORTHO_MIN_SIN ) vpnz = -KGRID_ORTHO_MIN_SIN;

        const float t    = c->vup[2] / vpnz;
        const float g[2] = { c->vup[0] - c->vpn[0] * t,
                             c->vup[1] - c->vpn[1] * t };

        for ( int a = 0; a < 2; ++a )
            needOut[a] = halfW * fabsf( c->vright[a] ) + halfH * fabsf( g[a] );

        // The screen centre's own ground hit: origin + vpn * (-origin[2]/vpn[2]).
        const float tc = -c->origin[2] / vpnz;
        hitOut[0] = c->origin[0] + c->vpn[0] * tc;
        hitOut[1] = c->origin[1] + c->vpn[1] * tc;
    }

    // Apply the per-axis cell cap to one axis of a footprint.  When the axis fits,
    // the window is the footprint EXACTLY (centre = the hit).  When it does not,
    // the retained span is pulled PROPORTIONALLY back toward the eye's own ground
    // point — so a grazing view keeps the lattice under the viewer rather than
    // parked on a horizon it cannot resolve — and the caller is told, because that
    // is precisely the condition the far ring exists for.
    //
    // Continuous at the boundary by construction: at need == cap the pull factor
    // is 1 and the anchor IS the hit.
    void ClampAxis( float ground, float hit, float need, float cap,
                    float *centreOut, float *reachOut )
    {
        if ( !( need > cap ) )                     // also catches a NaN `need`
        {
            *centreOut = hit;
            *reachOut  = ( need > 0.0f ) ? need : cap;
            return;                                // nothing was taken away
        }
        const float pull = cap / need;             // < 1
        *centreOut = ground + ( hit - ground ) * pull;
        *reachOut  = cap;
    }

    bool BuildWindowOrtho( const camera_s *c, window_t &w, float *needOut, float *hitOut )
    {
        w.ok = false;
        const float base = KiwiUnits_GridSpacingWorld();
        if ( !( base > 0.0f ) )
            return false;

        // THE one input.  KiwiCam_WorldPerPixel's ortho arm is 2*H/height and does
        // not read its argument (kiwi_camera.cpp:668-673) — c->origin is passed so
        // the call reads as what it is, not because the position matters.
        const float wpp = KiwiCam_WorldPerPixel( c->origin );
        if ( !( wpp > 0.0f ) )
            return false;

        // ── KIWI-UX (ROUND AL, ITEM 3): THE LADDER MEASURES THE FORESHORTENED
        //    CELL, NOT THE FLAT ONE ───────────────────────────────────────────
        // USER REPORT, verbatim: "Grid ugliness still there. not fixed."  The
        // screenshot is a TILTED ortho view (the cube reads its TOP corner with
        // -X) at 6 in spacing, with dense anisotropic line bands in the middle
        // distance.  Round AK's ladder held `spacing / wpp` — the cell's
        // HORIZONTAL screen size — above KGRID_PX_MIN, and on a tilted ground
        // plane that is the size of the axis that does NOT collapse.
        //
        // THE DERIVATION (kiwi_grid.h THE FORESHORTENED FLOOR carries it in full).
        // Under a parallel projection a world displacement d lands on screen at
        // (d·vright, d·vup) / wpp.  With no roll, CamWnd_BuildMatrix's basis at
        // pitch phi and yaw psi is
        //     vpn    = ( cos phi cos psi,  cos phi sin psi, -sin phi )
        //     vright = (        -sin psi,          cos psi,        0 )
        //     vup    = ( sin phi cos psi,  sin phi sin psi,  cos phi )
        // Split a GROUND displacement d (d_z = 0) into d = a*vright + b*h with
        // h = (cos psi, sin psi, 0) the ground-forward direction.  Then
        //     d · vright = a                    (vright is horizontal: unchanged)
        //     d · vup    = b * sin phi          (h · vup = sin phi)
        // so the ground plane is compressed by EXACTLY |sin phi| = |vpn·Z| along
        // the screen's vertical axis and not at all along vright.  The two line
        // families therefore have screen pitches
        //     lines parallel to h       :  spacing / wpp                (full)
        //     lines parallel to vright  :  spacing * |vpn·Z| / wpp      (compressed)
        // and it is the second family that piles up into the reported bands.  The
        // floor must be applied to the SMALLER of the two, i.e. to
        //     px_eff = spacing * |vpn·Z| / wpp = spacing / (wpp / |vpn·Z|).
        // Feeding wppEff = wpp / |vpn·Z| to the unchanged ladder is that, exactly.
        //
        // WHY THIS IS NOT THE HISTORIC PITCH-COUPLING DISEASE.  That disease was
        // SPATIAL: a per-line or per-row factor under a perspective divide, so
        // the lattice's density varied across one image and crawled inside one
        // frame.  |vpn·Z| under a parallel projection is a single frame-level
        // scalar, constant over every pixel of the view by construction (there is
        // no divide), and it feeds the SAME Schmitt-latched integer s_lod that
        // zoom already feeds.  Orbiting can now move the tier — but unlike round
        // AK's altitude ladder, which moved it while nothing on screen changed
        // scale, the cells genuinely ARE compressing when it does, so coarsening
        // is the correct response and not a spurious one.  KGRID_PX_REFINE /
        // KGRID_PX_MIN = 22/9 = 2.44 > 2, so the latch still cannot oscillate.
        //
        // THE FLOOR IS THE ONE THAT ALREADY EXISTS.  Below KGRID_ORTHO_MIN_SIN
        // (= KGRID_EDGE_FULL, 0.08, ~4.6 deg) round S's EdgeFade is already
        // ramping the whole lattice out and OrthoFootprint already clamps there,
        // so the ladder stops coarsening at exactly the angle the fade takes over:
        // the two hand off at one constant rather than fighting over the frames
        // in between.  Bounded coarsening: 1/0.08 = 12.5x, under four tiers, and
        // KGRID_LOD_MAX still caps it.
        float sinPitch = fabsf( c->vpn[2] );
        if ( !( sinPitch > KGRID_ORTHO_MIN_SIN ) )      // also catches a NaN vpn
            sinPitch = KGRID_ORTHO_MIN_SIN;
        const float wppEff = wpp / sinPitch;
        if ( !( wppEff > 0.0f ) )
            return false;

        const int k = PickLodOrtho( base, wppEff );
        w.spacing = base * (float)( 1 << k );
        if ( !( w.spacing > 0.0f ) )
            return false;

        const float halfW = 0.5f * wpp * (float)c->width;
        const float halfH = 0.5f * wpp * (float)c->height;

        float need[2], hit[2];
        OrthoFootprint( c, halfW, halfH, need, hit );

        float cap = w.spacing * (float)KGRID_HALF_CELLS;
        if ( cap > KGRID_MAX_EXTENT )
            cap = KGRID_MAX_EXTENT;
        if ( !( cap > 0.0f ) )
            return false;

        w.ground[0] = c->origin[0];
        w.ground[1] = c->origin[1];
        for ( int a = 0; a < 2; ++a )
        {
            ClampAxis( w.ground[a], hit[a], need[a], cap, &w.centre[a], &w.reach[a] );
            if ( !( w.reach[a] > 0.0f ) )
                return false;
            if ( needOut ) needOut[a] = need[a];
            if ( hitOut  ) hitOut[a]  = hit[a];
        }

        w.ok = true;
        return true;
    }

    // The ortho far ring.  NO FRUSTUM PROBE: `need[a] > reach[a]` already IS the
    // exact statement "there is visible ground the near window does not cover",
    // and it is the same two numbers the near window was clamped with.  Same
    // 1.30 / 1.05 Schmitt latch as the perspective ring (and the same s_farOn, so
    // toggling P cannot leave two latches disagreeing), same majors-only rule.
    bool BuildFarWindowOrtho( const window_t &w, const float *need, const float *hit,
                              window_t &f )
    {
        f.ok = false;

        // How much of the visible ground the near window is missing, as a ratio.
        float ratio = 1.0f;
        for ( int a = 0; a < 2; ++a )
        {
            if ( !( w.reach[a] > 0.0f ) )
                return false;
            const float r = need[a] / w.reach[a];
            if ( r > ratio )
                ratio = r;
        }

        if ( s_farOn )
        {
            if ( ratio < KGRID_FAR_OFF )
                s_farOn = false;
        }
        else if ( ratio > KGRID_FAR_ON )
        {
            s_farOn = true;
        }
        if ( !s_farOn )
            return false;

        f.spacing = w.spacing * (float)( 1 << KGRID_FAR_STEP );
        if ( !( f.spacing > 0.0f ) )                // overflow / NaN at absurd LODs
            return false;

        float cap = f.spacing * (float)KGRID_HALF_CELLS;
        if ( cap > KGRID_MAX_EXTENT )
            cap = KGRID_MAX_EXTENT;

        f.ground[0] = w.ground[0];
        f.ground[1] = w.ground[1];
        bool bigger = false;
        for ( int a = 0; a < 2; ++a )
        {
            // The SAME footprint hit the near window used, not the near window's
            // (already pulled-back) centre.  The two tiers cannot separate: the
            // lattice is world-anchored at multiples of `spacing` and the far
            // spacing is a power-of-two multiple of the near one, so the window
            // centre sets the extent and never the phase.
            ClampAxis( f.ground[a], hit[a], need[a], cap, &f.centre[a], &f.reach[a] );
            if ( !( f.reach[a] > 0.0f ) )
                return false;
            if ( f.reach[a] > w.reach[a] )
                bigger = true;
        }
        if ( !bigger )                              // clamped back onto the near ring
            return false;

        f.ok = true;
        return true;
    }

    // ── ROUND AG: HOW FAR THE VISIBLE GROUND RUNS ───────────────────────────
    // Returns the ground distance (from the camera's own ground point) at which
    // the Z=0 plane leaves the frustum, or KGRID_FAR_INF when the HORIZON is on
    // screen and the answer is unbounded, or 0 when no ground is in view at all.
    //
    // Read kiwi_grid.h "HOW IS THERE VISIBLE GROUND OUT THERE" before touching
    // this: it is frustum math, which is what v2 died of, and the ONLY thing it is
    // permitted to decide is whether one fixed extra ring is drawn.  It must never
    // reach the near lattice's spacing or reach.
    const float KGRID_FAR_INF = 1.0e30f;

    float FarGroundDistance( const camera_s *c )
    {
        const float h = fabsf( c->origin[2] );
        if ( !( h > 0.0f ) || c->width < 2 || c->height < 2 )
            return 0.0f;

        // `dz > 0` means "this ray travels TOWARD the ground plane", whichever
        // side of it the eye is on.
        const float toward = ( c->origin[2] >= 0.0f ) ? -1.0f : 1.0f;

        // The bottom-centre ray first: if even THAT misses the plane, the view has
        // no ground in it and the far ring would be 482 wasted segments.
        {
            float dir[3];
            Ed_CameraCalcRayDir( c->width / 2, 0, dir );     // y is bottom-left origin
            if ( !( dir[2] * toward > 1.0e-4f ) )
                return 0.0f;
        }

        // The three rays along the TOP edge (y = height-1) are the shallowest the
        // frustum has; the corners are shallower than the centre because vright is
        // horizontal, so a corner adds horizontal length without adding any dz.
        const int yTop  = c->height - 1;
        const int xs[3] = { 0, c->width / 2, c->width - 1 };
        float     best  = 0.0f;
        for ( int i = 0; i < 3; ++i )
        {
            float dir[3];
            Ed_CameraCalcRayDir( xs[i], yTop, dir );
            const float dz = dir[2] * toward;
            if ( !( dz > 1.0e-4f ) )
                return KGRID_FAR_INF;      // this ray is at or above the horizon
            const float horiz = sqrtf( dir[0] * dir[0] + dir[1] * dir[1] );
            const float d     = h * horiz / dz;
            if ( d > best )
                best = d;
        }
        return best;
    }

    // Build the FAR window from the near one, or return false when the ring is not
    // wanted this frame.  Everything here is derived from `w` and one boolean, so
    // the near lattice cannot be perturbed by it.
    bool BuildFarWindow( const camera_s *c, const window_t &w, window_t &f )
    {
        f.ok = false;

        // ROUND AK: `reach` is per axis now; the PERSPECTIVE window is isotropic by
        // construction (BuildWindow writes R to both), so this arm reads [0] and is
        // arithmetically what it always was.
        const float R = w.reach[0];

        float dFar = FarGroundDistance( c );
        const float cap = R * KGRID_FAR_MAX_MUL;
        if ( dFar > cap )
            dFar = cap;

        if ( s_farOn )
        {
            if ( dFar < R * KGRID_FAR_OFF )
                s_farOn = false;
        }
        else if ( dFar > R * KGRID_FAR_ON )
        {
            s_farOn = true;
        }
        if ( !s_farOn )
            return false;

        f.spacing = w.spacing * (float)( 1 << KGRID_FAR_STEP );
        float fR  = R         * (float)( 1 << KGRID_FAR_STEP );
        if ( !( f.spacing > 0.0f ) )                // overflow / NaN at absurd LODs
            return false;
        if ( fR > KGRID_MAX_EXTENT )
            fR = KGRID_MAX_EXTENT;
        if ( !( fR > R ) )                          // clamped back onto the near ring
            return false;
        f.reach[0] = fR;
        f.reach[1] = fR;

        f.ground[0] = w.ground[0];
        f.ground[1] = w.ground[1];

        // Same pitch-free forward bias as the near window, scaled to the far reach.
        const float fwd[2] = { c->forward[0], c->forward[1] };
        const float fl     = sqrtf( fwd[0] * fwd[0] + fwd[1] * fwd[1] );
        const float bias   = ( fl > 1.0e-3f ) ? ( fR * KGRID_FWD_BIAS / fl ) : 0.0f;
        f.centre[0] = f.ground[0] + fwd[0] * bias;
        f.centre[1] = f.ground[1] + fwd[1] * bias;

        f.ok = true;
        return true;
    }

    // Which of the three brightness bands a line at `pos` on `axis` belongs to,
    // by its WORLD distance from the camera's own ground point.  Purely cosmetic:
    // every band draws every line it owns.
    inline int BandOf( const window_t &w, int axis, float pos )
    {
        const float d = fabsf( pos - w.ground[axis] ) / w.reach[axis];
        if ( d < KGRID_BAND_NEAR ) return 0;
        if ( d < KGRID_BAND_MID )  return 1;
        return 2;
    }

    // First / last lattice index on `axis` inside the window.  The window is
    // 2R wide, so (i1 - i0 + 1) <= 2*KGRID_HALF_CELLS + 1 — the budget bound.
    inline void IndexRange( const window_t &w, int axis, int *i0, int *i1 )
    {
        *i0 = (int)ceilf ( ( w.centre[axis] - w.reach[axis] ) / w.spacing );
        *i1 = (int)floorf( ( w.centre[axis] + w.reach[axis] ) / w.spacing );
    }

    // Emit one (band, major) colour run into the OPEN batch.  The colour has
    // already been set by the caller — one KiwiLines_Color per run, which is one
    // SetMaterialColor + DrawLines pair (kiwi_lines.h "Colour runs").  Returns
    // false once the batch budget is spent.
    //
    // The index MAGNITUDE is unbounded (a camera at world X = 200000 sees indices
    // near 20000), but the COUNT is not: the window is 2R wide whatever it is
    // centred on, so i1 - i0 + 1 <= 2*KGRID_HALF_CELLS + 1 always.  That, and not
    // any cull below, is what holds the budget.
    //
    // ROUND AG: `band < 0` means "every band" — the far ring is one flat tier and
    // has no distance banding of its own (kiwi_grid.h THE FAR RING).
    bool EmitRun( const camera_s *c, const window_t &w, int band, int major,
                  bool axesOn )
    {
        for ( int axis = 0; axis < 2; ++axis )     // 0: lines at constant X, 1: constant Y
        {
            int i0, i1;
            IndexRange( w, axis, &i0, &i1 );
            for ( int i = i0; i <= i1; ++i )
            {
                if ( axesOn && i == 0 )
                    continue;                      // the world axis owns that line
                if ( ( ( i % 10 == 0 ) ? 1 : 0 ) != major )
                    continue;
                const float pos = (float)i * w.spacing;
                if ( band >= 0 && BandOf( w, axis, pos ) != band )
                    continue;

                float a[3], b[3];
                a[2] = b[2] = 0.0f;
                a[axis]     = b[axis]     = pos;
                a[axis ^ 1] = w.centre[axis ^ 1] - w.reach[axis ^ 1];
                b[axis ^ 1] = w.centre[axis ^ 1] + w.reach[axis ^ 1];
                if ( BehindEye( c, a, b ) )
                    continue;
                if ( !KiwiLines_Add( a, b ) )
                    return false;                  // budget spent (kiwi_grid.h)
            }
        }
        return true;
    }

    // ── ROUND S: the EDGE-ON fade (kiwi_grid.h THE EDGE-ON FADE) ────────────
    // ONE number for the whole frame: how much of the ground lattice's brightness
    // survives at this view angle.  0 means "draw none of it".  Costs one dot and
    // one divide per frame; nothing per line.
    float EdgeFade( const camera_s *c )
    {
        const float s = fabsf( c->vpn[2] );          // |vpn · Z|
        if ( !( s > KGRID_EDGE_OFF ) )               // also catches NaN
            return 0.0f;
        if ( s >= KGRID_EDGE_FULL )
            return 1.0f;
        return ( s - KGRID_EDGE_OFF ) / ( KGRID_EDGE_FULL - KGRID_EDGE_OFF );
    }

    // The width-2 dim under-pass for the MAJOR lines only — the fake AA.  Its own
    // batch because KiwiLines_Begin fixes one width per batch.  Emitted BEFORE the
    // width-1 pass so the bright core lands on top ($line is depthTest LESSEQUAL,
    // depthWrite ON, so the coplanar core passes the depth the halo wrote).
    //
    // ROUND AK: `bands` is false in ORTHO — a parallel projection has no distance
    // attenuation to model, so one flat tier is drawn with band 0's colour and the
    // three-band bullseye that centred itself on the viewer is gone (kiwi_grid.h
    // DISTANCE BANDS ARE OFF IN ORTHO).  EmitRun's `band < 0` arm already meant
    // "every line, whatever its distance", so this costs no new emitter code.
    void DrawMajorHalo( const camera_s *c, const window_t &w, bool axesOn, float fade,
                        bool bands )
    {
        KiwiLines_Begin( KGRID_HALO_SEGMENTS, 2 );
        if ( !bands )
        {
            const float mul = KGRID_HALO_MUL * fade;
            KiwiLines_Color( KGRID_BANDS[0][1][0] * mul,
                             KGRID_BANDS[0][1][1] * mul,
                             KGRID_BANDS[0][1][2] * mul );
            EmitRun( c, w, /*band*/ -1, /*major*/ 1, axesOn );
            KiwiLines_Flush();
            return;
        }
        for ( int band = 0; band < KGRID_BAND_COUNT; ++band )
        {
            const float mul = KGRID_HALO_MUL * fade;
            KiwiLines_Color( KGRID_BANDS[band][1][0] * mul,
                             KGRID_BANDS[band][1][1] * mul,
                             KGRID_BANDS[band][1][2] * mul );
            if ( !EmitRun( c, w, band, /*major*/ 1, axesOn ) )
                break;
        }
        KiwiLines_Flush();
    }

    // ROUND AG: the far ring, emitted into the OPEN width-1 batch BEFORE the near
    // lattice so the brighter near lines land on top of the coincident far ones
    // (kiwi_grid.h THE FAR RING).  No distance banding, no halo.
    //
    // ── ROUND AH (THE BLACK SLAB): MAJORS ONLY, UNCONDITIONALLY ─────────────
    // USER REPORT, verbatim: "grid problem is worse." — screenshot: the whole
    // ground plane a solid BLACK slab at a shallow angle.
    //
    // Round AG emitted the far ring's MINOR run too (a second colour pass over
    // all 241 lines per axis).  Arithmetic kills that idea on its own terms: the
    // ring's on-latch fires at (visible ground) > 1.30 * R, and R tracks height
    // at roughly 9-10x, so farOn is structurally pitch < ~5 deg — the exact
    // regime where round S's edge fade already rules that minors cannot be drawn
    // for the NEAR window (241 lines across a heavily foreshortened span is more
    // lines than pixels; they merge into a slab).  The far window is EIGHT TIMES
    // that span through the same few pixels, and its colour is
    // band2 * KGRID_FAR_MUL * fade — at fade ~0.1 that is ~0.02 grey, i.e. lines
    // DARKER than the viewport clear.  Denser than pixels + darker than the
    // background = the solid black slab, precisely.  And those 482 segments
    // spent HALF the shared batch before the near lattice drew a line.
    //
    // The ring's job is "the plane continues", and at the only angles that
    // summon it, minors can never resolve as lines at ANY brightness — so this
    // is not a tuning knob, majors are simply all there is to draw.  <=25 lines
    // per axis (i % 10 == 0 inside 241 indices), ~50 segments total.
    // ── KIWI-UX (ROUND AM, ITEM 3): HOW MANY LINES THE RING WOULD DRAW ──────
    // The same arithmetic EmitRun does, without emitting: the index range on each
    // axis, then how many multiples of 10 (the major stride) fall inside it, less
    // index 0 when the world axis owns that line.  No BehindEye term — in ortho
    // nothing is behind the eye, and over-counting could only ever keep a ring the
    // gate would otherwise drop, which is the safe direction.  kiwi_grid.h carries
    // why a count floor is the right shape of gate for this pass.
    int FarMajorCount( const window_t &f, bool axesOn )
    {
        int n = 0;
        for ( int axis = 0; axis < 2; ++axis )
        {
            int i0, i1;
            IndexRange( f, axis, &i0, &i1 );
            if ( i1 < i0 )
                continue;
            const int m0 = (int)ceilf ( (float)i0 / 10.0f );
            const int m1 = (int)floorf( (float)i1 / 10.0f );
            if ( m1 < m0 )
                continue;
            int here = m1 - m0 + 1;
            if ( axesOn && m0 <= 0 && 0 <= m1 )
                --here;                            // index 0 is the world axis
            n += here;
        }
        return n;
    }

    void DrawFarRing( const camera_s *c, const window_t &f, bool axesOn, float fade )
    {
        const float mul = KGRID_FAR_MUL * fade;
        KiwiLines_Color( KGRID_BANDS[2][1][0] * mul,
                         KGRID_BANDS[2][1][1] * mul,
                         KGRID_BANDS[2][1][2] * mul );
        EmitRun( c, f, /*band*/ -1, /*major*/ 1, axesOn );
    }

    void DrawGround( const camera_s *c, const window_t &w, bool axesOn, float fade,
                     bool bands )
    {
        // ROUND S: below the half-way point of the ramp the MINOR lines are the
        // ones aliasing into the moiré slab (10x denser than the majors), so they
        // go first and the majors carry the lattice on their own.
        const bool minorsOn = ( fade >= KGRID_EDGE_MINOR );

        // ROUND AK: ortho is ONE flat tier — majors, then minors, both at band 0.
        if ( !bands )
        {
            for ( int major = 1; major >= 0; --major )
            {
                if ( !major && !minorsOn )
                    continue;
                KiwiLines_Color( KGRID_BANDS[0][major][0] * fade,
                                 KGRID_BANDS[0][major][1] * fade,
                                 KGRID_BANDS[0][major][2] * fade );
                if ( !EmitRun( c, w, /*band*/ -1, major, axesOn ) )
                    return;
            }
            return;
        }

        for ( int p = 0; p < KGRID_BAND_COUNT * 2; ++p )
        {
            const int pass  = KGRID_PASS_ORDER[p];
            const int band  = pass >> 1;
            const int major = pass & 1;
            if ( !major && !minorsOn )
                continue;
            KiwiLines_Color( KGRID_BANDS[band][major][0] * fade,
                             KGRID_BANDS[band][major][1] * fade,
                             KGRID_BANDS[band][major][2] * fade );
            if ( !EmitRun( c, w, band, major, axesOn ) )
                return;
        }
    }
}

// ─── Cam_Draw hook (ROUND S: now BEFORE the world — see camwnd.cpp) ──────────
void KiwiGrid_Draw()
{
    const bool axesOn = KiwiUX_ShowAxes();
    // ── KIWI-UX (ROUND AJ, ITEM 5): NO GROUND LATTICE IN PERSPECTIVE ────────
    // USER DIRECTIVE, verbatim: "Disable drawing the grid in 'P' Camera mode
    // because the bugs are even worse."
    //
    // THIS IS A WORKAROUND THE USER CHOSE, NOT THE END STATE, and it is written
    // down as one (RADIANT_KNOWN_ISSUES round AJ).  The perspective projection is
    // where every one of the lattice's open problems is at its worst — the
    // horizon-plane aliasing the round-S edge-on ramp (EdgeFade) only softens, the
    // band seams the round-AG far ring (BuildFarWindow) only pushes further out,
    // and the near-plane crawl a screen-space window cannot fix — and none of them
    // exist in ortho, where the lattice is a fixed-pitch pattern at a fixed scale.
    // Suppressing the draw is not a fix for any of them; it removes the surface
    // they show up on until they ARE fixed.
    //
    // THE AXES STAY.  They are three lines, they do not alias, and they are the
    // one thing that still says which way is which when the lattice has gone —
    // exactly the argument the edge-on ramp already makes for keeping them
    // outside its own fade.
    //
    // SNAPPING IS UNTOUCHED.  KiwiGrid_Snap is a different function with a
    // different switch (ITEM 5's other half), so the lattice you cannot see is
    // still the lattice you land on.
    const bool gridOn = KiwiUX_ShowGrid() && KiwiCam_Ortho();
    if ( !axesOn && !gridOn )
        return;

    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;
    CamWnd_BuildMatrix();               // vpn (cull + edge fade) + forward (bias)

    // ROUND S: the edge-on ramp.  Read BEFORE the window is built so a fully faded
    // frame costs nothing but the dot product.  The AXES are deliberately outside
    // it — they are three lines, they do not alias, and they are the one thing that
    // still says which way is which when the lattice has gone.
    const float fade = EdgeFade( c );

    // ── KIWI-UX (ROUND AK, ITEM 2): WHICH LADDER BUILDS THE WINDOW ──────────
    // ORTHO takes the world-per-pixel path: spacing from ZOOM alone, reach from
    // the closed-form footprint of the view rect on Z = 0, one flat brightness
    // tier.  PERSPECTIVE keeps round M's altitude ladder byte-for-byte — it is
    // not drawn at all right now (the ITEM 5 workaround above), and changing a
    // path nobody can see would be a change nobody can test.
    // Everything the ortho arm decides is in kiwi_grid.h THE ORTHOGRAPHIC GRID.
    const bool ortho = KiwiCam_Ortho();

    window_t w = {};                    // zero-init: nothing reads it unless haveWindow
    float    need[2] = { 0.0f, 0.0f };  // the ortho footprint before the cell cap
    float    hit [2] = { 0.0f, 0.0f };  // the screen centre's own ground point
    const bool haveWindow = gridOn && fade > 0.0f
                          && ( ortho ? BuildWindowOrtho( c, w, need, hit )
                                     : BuildWindow( c, w ) );

    // ROUND AG (ITEM 10): the far ring.  Derived ENTIRELY from `w` plus one
    // boolean — out of the frustum probe in perspective, out of the EXACT
    // "need > reach" comparison in ortho (ROUND AK: the probe's ray math is
    // meaningless under a parallel projection, kiwi_grid.h).  Either way it can
    // add a coarse distant tier and cannot touch the near lattice's spacing,
    // reach or centre.
    window_t wf = {};
    bool haveFar = haveWindow
                 && ( ortho ? BuildFarWindowOrtho( w, need, hit, wf )
                            : BuildFarWindow( c, w, wf ) );

    // ── KIWI-UX (ROUND AM, ITEM 3): THE RING STANDS DOWN NEAR THE HORIZON ───
    // USER REPORT, verbatim: "Grid ugliness still there. not fixed."  The strays
    // in the screenshot are this pass — the identification, the four properties
    // that pin it and the exact hole between EdgeFade's ceiling and the
    // reach-clamp regime are all in kiwi_grid.h WHEN THE FAR RING STOPS BEING A
    // GRID.  ORTHO ONLY: the perspective ring is unreachable today (the item-5
    // suppression) and a path nobody can see is a path nobody can test.
    //
    // Neither gate can touch the near lattice (which keeps its own EdgeFade ramp
    // and its own ladder) or the AXES (exempt from every fade by design), and a
    // dropped ring only ever RETURNS budget to the shared width-1 batch.
    if ( haveFar && ortho )
    {
        if ( fabsf( c->vpn[2] ) < KGRID_FAR_MIN_SIN )
            haveFar = false;                       // too near the horizon to read
        else if ( FarMajorCount( wf, axesOn ) < KGRID_FAR_MIN_LINES )
            haveFar = false;                       // too few lines to be a grid
    }

    // Pass 1 (width 2): the major-line halo, under everything.  NEAR ONLY — the
    // far ring is dim context and is not worth doubling (kiwi_grid.h THE BUDGET).
    //
    // ── KIWI-UX (ROUND AN, ITEM 11): NO HALO IN ORTHO ───────────────────────
    // USER REPORT, verbatim: "the main grey lines keep turning dark with some
    // weird aliasing."  The halo is the PERSPECTIVE fake-AA: a width-2 pass at
    // KGRID_HALO_MUL brightness under the width-1 bright core.  Under the ortho
    // foreshortening (round AL) a tilted view compresses the majors, the 1-px
    // core rasterises onto different pixels frame to frame, and the 2-px dim
    // halo always covers them — wherever the core misses, the major shows at
    // halo brightness: "turning dark, weird aliasing", exactly.  The core alone
    // is crisp; ortho tilted views are working views, not beauty shots.
    if ( haveWindow && !ortho )
        DrawMajorHalo( c, w, axesOn, fade, /*bands*/ !ortho );

    // Pass 2 (width 1): the far ring, the grid proper, then the axes on top.
    KiwiLines_Begin( KGRID_MAX_SEGMENTS, 1 );
    if ( haveFar )
        DrawFarRing( c, wf, axesOn, fade );
    if ( haveWindow )
        DrawGround( c, w, axesOn, fade, /*bands*/ !ortho );
    if ( axesOn )
        DrawAxes( c );
    KiwiLines_Flush();
}

// ─── ROUND AJ, ITEM 5: the grid-snap master switch (kiwi_grid.h) ─────────────
namespace
{
    int s_snapOn = -1;                      // -1 = not read from the profile yet
}

bool KiwiGrid_SnapEnabled()
{
    if ( s_snapOn < 0 )
        s_snapOn = Radiant_ProfileGetInt( "KiwiUX", "GridSnap", 1 ) ? 1 : 0;
    return s_snapOn != 0;
}

void KiwiGrid_SetSnapEnabled( bool on )
{
    const int v = on ? 1 : 0;
    if ( KiwiGrid_SnapEnabled() == ( v != 0 ) )
        return;
    s_snapOn = v;
    Radiant_ProfileSetInt( "KiwiUX", "GridSnap", v );
    Sys_Printf( "Grid snapping: %s.\n", v ? "ON" : "OFF (geometry snaps still work)" );
    g_nUpdateBits |= 1;
}

// ─── snap (SnapManager consumer) ─────────────────────────────────────────────
bool KiwiGrid_Snap( const float in[3], float out[3] )
{
    if ( !in || !out )
        return false;
    // ROUND AJ, ITEM 5: the master switch.  Reported as a REFUSAL rather than as
    // an identity snap, because "false, and `in` copied through" is the contract
    // every consumer already implements for an unusable spacing — kiwi_snap.cpp's
    // arm 9 demotes to SNAP_NONE on it, so the HUD stops claiming a grid too.
    if ( !KiwiGrid_SnapEnabled() )
    {
        out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
        return false;
    }
    // ── KIWI-UX (ROUND AK, ITEM 2): THE USER'S BASE SPACING, NOT THE DISPLAY
    //    TIER — and this became load-bearing this round ────────────────────────
    // Before round AK the ortho lattice's spacing did not move with zoom, so the
    // display tier and the snap tier agreed most of the time by accident.  Now the
    // tier IS a function of zoom (kiwi_grid.h THE ORTHOGRAPHIC GRID), so the two
    // must be held apart deliberately: zooming out to a coarse DISPLAY tier may
    // never move a vertex.  It cannot — `s_lod` is a file-static of this file's
    // anonymous namespace and is exported nowhere, and both quantisers
    // (this one and KiwiSnap_LightGridAxis, kiwi_snap.cpp:1111) read
    // KiwiUnits_GridSpacingWorld().  You land on the spacing the units pill says.
    const float s = KiwiUnits_GridSpacingWorld();
    if ( !( s > 0.0f ) )
    {
        out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
        return false;
    }
    for ( int i = 0; i < 3; ++i )
        out[i] = floorf( in[i] / s + 0.5f ) * s;
    return true;
}

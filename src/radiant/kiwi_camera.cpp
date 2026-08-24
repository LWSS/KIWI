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
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <math.h>

// ── ported entry points (verified against their definitions) ────────────────
extern camera_s *Ed_Camera();          // camwnd.cpp
extern void      CamWnd_BuildMatrix(); // camwnd.cpp 0x403470
extern int       g_nUpdateBits;        // 0x25D5A74 (mainfrm.cpp)

namespace
{
    const float KCAM_DEG_PER_PX  = 0.35f;      // Cam_Rotate2's rate (0x4037c0)
    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BM, ITEM 3) — THE ORBIT RATE IS RESOLUTION-RELATIVE
    // ═══════════════════════════════════════════════════════════════════════
    // Plasticity: `left = 2*PI*rotateDelta.x / domElement.clientHeight` and
    // `up = 2*PI*rotateDelta.y / clientHeight` — height for BOTH axes, their own
    // "// yes, height" comment included (plasticity/src/components/viewport/
    // OrbitControls.ts:673-674), with rotateSpeed defaulting to 1
    // (plasticity/src/startup/default-settings.js:11).  In degrees per pixel that
    // is 360/height: ONE VIEWPORT HEIGHT OF DRAG IS ONE FULL TURN, whatever the
    // window size.  KCAM_DEG_PER_PX (0.35) is the same number at height 1029 —
    // which is roughly the shipped camera image — so this changes nothing for the
    // common case and stops the orbit being twice as fast on a small viewport.
    //
    // The FALLBACK is KCAM_DEG_PER_PX, so a camera with no size yet (or a
    // pathological height) behaves exactly as it did.  BC discipline: the driver
    // (`height`) is what is checked, not the derived rate.
    float KCam_OrbitDegPerPx( const camera_s *c )
    {
        if ( !c || c->height < 16 )
            return KCAM_DEG_PER_PX;
        return 360.0f / (float)c->height;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  THE PITCH LIMITS: ±90 IS EXACT, ±89 IS ONLY FOR DRAGS
    // ═══════════════════════════════════════════════════════════════════════
    // The basis at a pole is NOT degenerate under this file's angle convention.
    // CamWnd_BuildMatrix passes { -pitch, yaw, 0 } to AngleVectors, and with roll
    // zero that gives  vright = ( sin y, -cos y, 0 )  and  vup = ( sp cy, sp sy,
    // cp )  with sp = sin(-pitch) — both unit, both perpendicular, and both fully
    // determined by the YAW at pitch = ±90 (com_math_anglevectors.cpp:34-43).  The
    // yaw-plane forward/right pair is built from { 0, yaw, 0 } and never sees the
    // pitch at all (camwnd.cpp:214-218).  So an axis-locked view may be pole-EXACT:
    // an ortho TOP view at ±89 is a 1-degree tilt that leaks the side faces of
    // axis-aligned geometry, and which sides it leaks depends on the approach yaw.
    //
    // KCAM_PITCH_DRAG_LIMIT is what an interactive DRAG may reach — unchanged at
    // 89, because a drag through the pole would have the view pass over the top
    // and come back mirrored, which is not what a pitch drag means.
    //
    // A drag that STARTS pole-exact is the one case that needs more than the flat
    // limit.  The orbit's clamp is absolute-from-latch, so with pitch0 = -90 the
    // first moved pixel would clamp to -89 and jump the view a whole degree — in
    // the WRONG direction when the drag is INTO the pole (-90.2 clamps up to -89).
    // Widening the limit to the latched pitch when that is already outside the
    // range makes the first pixel smooth, keeps the zero-delta frame exact, and is
    // bit-for-bit the old ±89 for every drag that starts inside it.
    //
    // the limit was never the "stuck at the top" the user then hit —
    // the ORBIT's accumulator was, by banking the pixels the clamp refused.  See
    // KiwiCam_OrbitDrag; this pair of numbers is unchanged.
    const float KCAM_PITCH_DRAG_LIMIT = 89.0f;
    const float KCAM_PITCH_MAX        = 90.0f;   // the pole, and the absolute bound

    float KCam_PitchDragLimit( float pitch0 )
    {
        const float a = fabsf( pitch0 );
        if ( !( a > KCAM_PITCH_DRAG_LIMIT ) )    // also catches a NaN latch
            return KCAM_PITCH_DRAG_LIMIT;
        return ( a > KCAM_PITCH_MAX ) ? KCAM_PITCH_MAX : a;
    }

    // ── THE WIDENING IS ONE-SIDED ───────────────────────────────────────────
    // The band a drag starting at `pitch0` may use.  ±89 normally; a drag that
    // BEGINS outside that (only a pole-exact axis snap puts it there) keeps its
    // own side widened to the latched pitch so the first moved pixel is smooth,
    // and the OPPOSITE side stays at 89 — a pole is a place you snap to, not a
    // place a drag can arrive at, and the symmetric form let one long drag from
    // TOP run all the way onto BOTTOM.
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
    // ── KIWI-UX (ROUND BM, ITEM 3): …AND IT IS PLASTICITY'S NUMBER NOW ──────
    // 0.95 is `Math.pow(0.95, zoomSpeed)` at the default zoomSpeed = 1
    // (plasticity/src/components/viewport/OrbitControls.ts:648 and
    //  plasticity/src/startup/default-settings.js:10) — FIVE percent a notch, flat,
    // at every distance and in both projections.  "Copy Plasticity more" is the
    // directive and this is the one number the whole zoom feel hangs off.
    // Plasticity also throws the wheel delta's MAGNITUDE away (`Math.sign(deltaY)`,
    // OrbitControls.ts:439); KIWI keeps the magnitude because `wheelSteps` is
    // already an integral notch count from the shell, so a flick is N notches and
    // not one giant one — the same result by a different route.
    const float KCAM_DOLLY_STEP  = 0.95f;      // spec §10: fractional per wheel step
    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BM, ITEM 3) — THE NEAR-FIELD EASES ARE GONE.  HERE IS WHY.
    // ═══════════════════════════════════════════════════════════════════════
    // Rounds AJ and AN answered "when zooming in, the camera gets squirrely" with
    // two eases on one parameter t = clamp( reference / 96, 0, 1 ): the per-notch
    // STEP eased 0.90 -> 0.985 as t fell, and the AIM eased from the cursor ray
    // toward the view axis until zoom-to-cursor switched off entirely up close.
    // Both are deleted this round, because they were treating a symptom.
    //
    // THE CAUSE, found by reading the ORTHO arm rather than the step size.  In an
    // orthographic view the image scale is s_dist and NOTHING ELSE (the half-height
    // is s_dist * tan(fov/2) * 0.75).  The dolly's step, however, was a fraction of
    // `reference` = THE DISTANCE TO THE SURFACE UNDER THE CURSOR.  Those two
    // quantities are unrelated in a parallel projection: zoom in until s_dist is 20
    // (a ~10-unit half-height) with a wall 500 units away under the cursor, and one
    // notch moved the eye 5% of 500 = 25 units — TWO AND A HALF FRAMED HEIGHTS —
    // most of it laterally, because the eye travels along the cursor ray.  That is
    // "squirrely", exactly, and it gets worse the further you zoom in, which is
    // exactly what the two reports described.  A step ease can only shrink it; it
    // cannot make it proportional, because the wrong quantity is being scaled.
    //
    // ROUND BM'S ORTHO ARM SCALES s_dist AND ANCHORS THE CURSOR IN CLOSED FORM, so
    // the lateral motion is not approximated at all — the world point under the
    // cursor is INVARIANT — and the residual is zero at every zoom.  With the cause
    // gone the eases have nothing left to do, and one flat KCAM_DOLLY_STEP is what
    // the brief asks for: one consistent curve, no dead zones, no lateral surprise.
    //
    // PLASTICITY, RE-MEASURED (unchanged from the AJ block this replaces):
    //   * multiplicative, `zoomScale = Math.pow(0.95, zoomSpeed)`, zoomSpeed = 1
    //     (OrbitControls.ts:648, default-settings.js:10);
    //   * no soft minimum and no easing — `spherical.radius` is clamped hard at
    //     `minDistance = 0.1` (OrbitControls.ts:42, :225).  The only damping is the
    //     geometric series itself;
    //   * NO zoom-to-cursor: `onMouseWheel` never reads clientX/Y
    //     (OrbitControls.ts:423-443) and `dolly()` only scales the radius about
    //     `this.target` (:650-657).  KIWI keeps zoom-to-cursor deliberately — it is
    //     a mapping editor, not a part modeller — but it is now EXACT in ortho
    //     rather than an easement, which is the only way to keep it and still have
    //     no lateral surprise.
    // USER DIRECTIVE (shakeout C): "after a certain point, you can't zoom in
    // further" — 8 units (8 inches) was the wall.  1 inch lets the dolly get
    // close enough to detail work; the surface-referenced step already makes the
    // approach slow down smoothly instead of punching through.
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
    // ══ KIWI-UX (ROUND AZ, ITEM 2): THE ZOOM-OUT CEILING, DERIVED ═══════════
    // USER DIRECTIVE, verbatim: "when zooming out extremely far, it breaks the
    // 3d camera completely."  The screenshot is an empty viewport with a sliver
    // of grid along the bottom edge and no geometry at all.  That is not a draw
    // bug — it is THE ORTHOGRAPHIC FAR PLANE, and the old ceiling put the pivot
    // exactly twice as far away as it.
    //
    //   THE SLAB.  KIWI's 3D camera is ORTHOGRAPHIC BY DEFAULT (KCAM_ORTHO_ENTRY
    //   below, "default ON" since round N), and CamWnd_SetupScene's ortho arm
    //   builds a depth range that is SYMMETRIC ABOUT THE EYE:
    //       zNear = -KCAM_ORTHO_DEPTH , zFar = +KCAM_ORTHO_DEPTH
    //   with KCAM_ORTHO_DEPTH = 131072 (camwnd.cpp:193 — the engine's own world
    //   bound, the ±131072 = 2^17 sentinel Brush_BuildWindings seeds its AABB at,
    //   brush.cpp:1470-1471).  Anything whose view-space depth from the EYE
    //   exceeds 131072 is clipped away by the projection.  There is no such
    //   plane in the perspective arm (m[2][2]=C, m[2][3]=1 is an INFINITE far
    //   plane), which is why this only bites the default profile.
    //
    //   THE ARITHMETIC THAT BROKE.  The dolly holds the pivot still and walks the
    //   EYE backwards, so the framed geometry sits at view depth exactly s_dist
    //   and anything behind it at s_dist + extent.  The depth budget behind the
    //   pivot is therefore  131072 - s_dist.  At the old ceiling of 262144 that
    //   budget is -131072: the pivot ITSELF is twice as far as the far plane, so
    //   the entire map clips out.  What survives is only what is within 131072 of
    //   the eye — for a camera looking down at a map from far above, the near
    //   corner of the ground grid, which is the sliver at the bottom edge of the
    //   screenshot, pixel for pixel.
    //
    //   THE NEW NUMBER.  65536 == KCAM_ORTHO_DEPTH / 2, which is the statement
    //   "half the slab is reserved for whatever is BEHIND what you framed":
    //       depth budget behind the pivot = 131072 - 65536 = 65536 units.
    //   In framing terms, at the default fov of 65 the half-image height is
    //   s_dist * tan(32.5deg) * 0.75 = s_dist * 0.47780 (KiwiCam_OrthoHalfHeight),
    //   so the ceiling frames 2 * 65536 * 0.47780 = 62,636 units vertically and
    //   ~111,000 horizontally at 16:9.  A CoD4 map lives inside ±16384; this is
    //   still just under four times a full-radius one, top to bottom, in one
    //   screen.  Nothing a mapper does is lost and the far plane cannot be
    //   reached by the wheel any more.
    //
    //   IT ALSO FIXES THE PERSPECTIVE ARM, which has a different failure at the
    //   same ceiling: camwnd.cpp:230-235 records that the float32 MatrixInverse44
    //   loses the sign of the inverse-VP's m[3][3] on far-from-origin cameras
    //   (observed blacking out at world X ~ -175000), so CamWnd_SetupScene's
    //   R_Ed_ProjectionWouldBeValid guard starts DROPPING WHOLE FRAMES.  The old
    //   262144 sailed straight past that on its own; 65536 plus the origin box
    //   clamp (KCAM_ORIGIN_BOUND, ClampToWorld below) keeps the eye inside
    //   ±131072, i.e. 25% clear of the observed failure.
    // ══ KIWI-UX (ROUND BC, ITEM 1): …AND THE SLAB ITSELF IS A CHOICE ════════
    // USER REPORT, verbatim: "The zoom out limit is too restrictive.  Allow more
    // zoom out."
    //
    // ROUND AZ WAS RIGHT ABOUT THE MECHANISM AND WRONG ABOUT WHICH END TO MOVE.
    // Everything in the block above is still true: in ortho the framed geometry
    // sits at view depth s_dist and the projection clips it at KCAM_ORTHO_DEPTH,
    // so the ceiling MUST be derived from that slab.  What AZ did not question is
    // that KCAM_ORTHO_DEPTH (camwnd.cpp:193) is itself a KIWI number, not an
    // engine one — it was picked as "the engine world bound" because that is the
    // largest coordinate a BRUSH can have, and the slab is not measured in brush
    // coordinates.  It is measured from the EYE, and an ortho eye is a pseudo-eye
    // parked s_dist behind the pivot: it is not in the world and nothing is
    // stored there.  So the whole ladder moves up two powers of two:
    //
    //     KCAM_ORTHO_DEPTH   131072 -> 524288   (camwnd.cpp:193, this round)
    //     the ortho ceiling   65536 -> 262144   (= depth/2, unchanged RELATION)
    //     KCAM_ORTHO_HALF_MAX 32768 -> 131072   (= depth/4, unchanged relation)
    //     the eye leash      131072 -> 524288   in ortho (see KCam_OriginBound)
    //
    // 4x THE RANGE, and the SAME derivation that produced AZ's number produces
    // this one: half the slab is still reserved for whatever is behind what you
    // framed (524288 - 262144 = 262144 units of depth budget), and the ceiling
    // frames 2 * 262144 * 0.47780 = 250,500 units vertically / ~445,000
    // horizontally at 16:9.  A CoD4 map lives inside ±16384, so that is the whole
    // 32,768-unit world FIFTEEN times over in one screen.
    //
    // WHY IT STOPS THERE — the four things that actually bound it, each checked:
    //   1. THE DEPTH BUFFER.  2*524288 units of LINEAR ortho depth over a 24-bit
    //      buffer is 1/16 unit per step (AZ's slab gave 1/64).  Brush coordinates
    //      are integers and coplanar faces tie either way, so 1/16 still resolves
    //      everything the editor draws; another doubling would start to matter.
    //   2. THE LATTICE'S REACH.  KGRID_MAX_EXTENT (kiwi_grid.h) is raised to the
    //      same 524288 this round so the ground grid can still cover the frame at
    //      the ceiling — see ITEM 3.  The two numbers are a pair.
    //   3. FLOAT32.  At 524288 = 2^19 the ulp is 2^(19-23) = 2^-4 = 0.0625 units,
    //      and the projection is built in floats.  (KIWI-UX (CLEANUP, C-39): this
    //      line said 0.03125, which is off by 2x — and it is load-bearing, because
    //      it is the term that grows with the slab.  One more doubling of the
    //      ceiling makes it 0.125.)  A grid line placed at a multiple of its
    //      spacing is exact (both are powers of two times the base); a brush vertex
    //      at ±16384 is exact.  Nothing in the pipeline needs sub-1/16 precision at
    //      that range.
    //   4. THE PERSPECTIVE ARM, which is bounded by something else entirely and
    //      therefore keeps its own ceiling — see KCAM_MAX_DIST_PERSP.
    // The three ortho numbers are WRITTEN as the relations rather than as literals,
    // so raising the slab cannot leave one of them behind — the failure mode round
    // AZ's own KCAM_ORTHO_HALF_MAX comment warned about, made structural.
    // KIWI-UX (CLEANUP, C-24): KCAM_ORTHO_DEPTH_HALF is defined ONCE, in
    // kiwi_camera.h, and camwnd.cpp's clip slab reads the same one.
    const float KCAM_MAX_DIST_ORTHO   = KCAM_ORTHO_DEPTH_HALF * 0.5f; // 262144
    // The PERSPECTIVE ceiling is NOT the slab — that arm has an infinite far plane
    // (m[2][2]=C, m[2][3]=1) and cannot clip on depth at all.  What bounds it is
    // the float32 MatrixInverse44 degeneracy camwnd.cpp:230-235 records (the
    // inverse-VP loses the sign of m[3][3] on far-from-origin cameras; observed
    // blacking out at world X ~ -175000), which makes R_Ed_ProjectionWouldBeValid
    // start DROPPING WHOLE FRAMES.  AZ's 65536 keeps the eye inside ±131072 for a
    // map-centred pivot, i.e. 25% clear of the observed failure, and there is no
    // evidence on which to raise it — so it does not move.  The grid is not drawn
    // in perspective either (ROUND AJ, ITEM 5), so the extra range would buy an
    // empty viewport.  KiwiCam_SetOrtho pulls the eye in when the user leaves
    // ortho above this, so the two ceilings can never be silently mixed.
    const float KCAM_MAX_DIST_PERSP   = 65536.0f;   // ROUND AZ's number, deliberately kept
    // The eye's own leash.  The dolly used to keep pushing the ORIGIN outward
    // after s_dist had already saturated — `move` is derived from `reference`,
    // which is clamped, but the origin write was not, so every further notch flung
    // the eye ~29,000 units further out for ever.  ROUND AZ stops that at source
    // (the zoom-out saturation in KiwiCam_Dolly) and boxes it here as well, so the
    // keyboard fly and any future mover inherit the same leash.  ±131072 is the
    // engine world bound (brush.cpp:1470); a camera outside the world is looking
    // at nothing by definition, and staying inside it is what keeps the inverse-VP
    // guard above from firing.
    //
    // ── KIWI-UX (ROUND BC, ITEM 1): PER PROJECTION, like the ceiling ────────
    // The ortho eye is a pseudo-eye and has to reach the new ceiling: with the
    // pivot anywhere inside a ±16384 map, `lookAt - forward * 262144` needs
    // 278,528 units of room, so a 131072 leash would silently pin the eye and
    // ITEM 2's whole failure mode (the pivot marching away while the eye stands
    // still) would fire on every zoom-out.  The perspective leash does NOT move:
    // it is the number that keeps the inverse-VP guard from firing.
    const float KCAM_ORIGIN_BOUND_ORTHO = KCAM_ORTHO_DEPTH_HALF;   // 524288
    const float KCAM_ORIGIN_BOUND_PERSP = 131072.0f;   // ROUND AZ's number, kept
    // The ortho half-height's own ceiling, so an extreme "Fov" pref cannot blow the
    // projection up independently of the distance clamp.  KCAM_ORTHO_DEPTH / 4 =
    // 131072 (ROUND BC: was 32768) is deliberately just ABOVE the 125,270 that
    // KCAM_MAX_DIST_ORTHO produces at the default fov of 65 — i.e. it is INACTIVE
    // for every default-fov user and only bites a fov pushed past ~68 degrees.
    const float KCAM_ORTHO_HALF_MAX = KCAM_ORTHO_DEPTH_HALF * 0.25f;  // 131072
    const float KCAM_DEF_DIST    = 96.0f;      // shakeout I: was 256

    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BC, ITEM 2) — THE TWO CEILINGS, AS FUNCTIONS
    // ═══════════════════════════════════════════════════════════════════════
    // Every clamp in this file used to read one constant.  There are two now (the
    // slab bounds ortho, the inverse-VP bounds perspective), so they are read
    // through these and nowhere else — the AZ precedent that put the half-height
    // in exactly one function, applied to the ceiling.
    //
    // AND THE HALF-HEIGHT CAP IS FOLDED IN HERE RATHER THAN APPLIED AFTERWARDS.
    // THAT IS ITEM 2'S SECOND BUG, and it is a dead zone in the wheel: round AZ
    // clamped the DERIVED half-height (KiwiCam_OrthoHalfHeight) while leaving
    // s_dist free to keep growing past the point where the clamp bit.  At any fov
    // above ~68 the image therefore STOPPED ZOOMING OUT while the recorded
    // distance kept climbing — and then zooming back IN did nothing at all until
    // s_dist had fallen back below the clamp, which is "zoom is broken after
    // zooming out all the way, can't zoom back in" exactly.  A clamp on a derived
    // quantity whose driver is unclamped is always a one-way state; the fix is to
    // bound the DRIVER, so every notch of the wheel changes the image.
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

    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BM, ITEM 1a + 3) — THE ORTHO ARROW RATE, REPLACED
    // ═══════════════════════════════════════════════════════════════════════
    // ROUND BK SHIPPED  speed = KiwiCam_OrthoHalfHeight() * 1.0f *
    //                          KiwiCam_FlySpeedScale()
    // and that is the bug the user felt.  KCAM_ORTHO_FLY_RATE was documented as
    // "one half-height per second at 1x", i.e. HALF A SCREEN A SECOND — a fine
    // number — but KiwiCam_FlySpeedScale() defaults to 4.0 (400 in the profile,
    // below), so the shipped rate was FOUR HALF-HEIGHTS, i.e. TWO WHOLE SCREENS,
    // per second, tripling to six on Shift.  The multiplier exists to scale the
    // PERSPECTIVE world rate (m_nMoveSpeed units/s); applying it again to a rate
    // that was already expressed in screens double-counts it.
    //
    // THE REPLACEMENT is a SCREEN-PIXEL rate through the same world-per-pixel
    // Plasticity's orthographic pan uses (OrbitControls.ts:276-279), so the arrows
    // move the image at a fixed pixels-per-second at every zoom:
    //     world step = KCAM_FLY_PX_PER_SEC * (scale / KCAM_FLY_SCALE_DEF)
    //                * KiwiCam_WorldPerPixel(pivot) * dt
    // 600 px/s is about 0.6 of a 1000-px-tall viewport a second: brisk, and you can
    // still stop on a doorway.  Dividing by KCAM_FLY_SCALE_DEF is what makes the
    // user's existing slider still mean something without double-counting it — at
    // the shipped default the factor is exactly 1, and a user who doubles the
    // slider doubles both the ortho and the perspective fly, which is the only
    // reading of "fly speed" that is the same promise in both projections.
    const float KCAM_FLY_PX_PER_SEC = 600.0f;  // ortho arrows: screen px per second at 1x
    const float KCAM_FLY_SCALE_DEF  = 4.0f;    // the shipped KiwiCam_FlySpeedScale default
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
    // FLOAT, and only because the pitch clamp REWINDS it (KiwiCam_OrbitDrag): the
    // rewind lands on the exact pixel count the limit allows, which is fractional.
    // A sum of integer pixel deltas is exact in float far past any drag length, so
    // every frame that does not touch the clamp computes the same pitch it did as
    // an int accumulator.
    float s_orbAccY     = 0.0f;

    // ── shakeout E: the pan's per-gesture world-per-pixel anchor ────────────
    // See kiwi_camera.h "THE SHAKEOUT-E SENSITIVITY FIX".  Cached at PanBegin and
    // held for the whole drag so the scale cannot swing under the user's hand.
    float s_panK      = 0.0f;
    bool  s_panHave   = false;

    // Fly state.  s_flyPrev is the previous tick's QPC reading in seconds;
    // s_flyLookHeld records whether the last tick had the RMB-look arm live, which
    // is what KiwiCam_FlySwallowKey answers with.
    double s_flyPrev     = 0.0;
    bool   s_flyHave     = false;
    bool   s_flyLookHeld = false;
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
        // KIWI-UX (CLEANUP, C-33): the FREQUENCY is fixed for the process lifetime
        // by the API contract (QueryPerformanceFrequency: "the frequency cannot
        // change while the system is running"), so it is read once.  Only the
        // COUNTER is sampled per call — and this runs on every fly tick.
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


}   // anonymous namespace PAUSED: the function below is declared in kiwi_camera.h
    // and must have EXTERNAL linkage — defined inside the anonymous namespace it
    // mangles as ?A0x...@ and every other TU's call fails to link (LNK2019).

// KIWI-UX (CLEANUP, RayAxis): the ONE closest-point-on-an-axis solve.  Body
// copied verbatim from kiwi_transform.cpp's, which the other three mirrored.
// (KCAM_RAYAXIS_MIN_DEN stays reachable — anonymous-namespace names are visible
// to the rest of this TU.)
bool KiwiCam_RayAxis( const ray_t &ray, const float *pt, const float *axis, float *out )
{
    float w0[3];
    Sub3( pt, ray.origin, w0 );
    const float b = Dot3( axis, ray.dir );
    const float d = Dot3( axis, w0 );
    const float e = Dot3( ray.dir, w0 );
    const float den = 1.0f - b * b;
    // ── KIWI-UX (ROUND AI, ITEM 2): THE SAMPLE GATE ─────────────────────────
    // den is sin^2(theta) and the solve is amplified by 1/den, so the old
    // 1.0e-4 only refused theta < 0.6 degrees and happily returned a point
    // hundreds of units away for anything steeper.  KCAM_RAYAXIS_MIN_DEN is the
    // same 14-degree cone the view gate uses (kiwi_camera.h).
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

    // Clamp a scale-about-point transform to the eye leash BEFORE it is written.
    // `toAnchor` is anchor-origin and the requested eye delta is
    // `toAnchor * (1-factor)`.  Returning a factor between the request and 1 keeps
    // the whole transform — distance and cursor correction alike — on one
    // effective factor instead of letting Commit truncate the eye afterwards.
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

    // ── KIWI-UX (ROUND AZ, ITEM 2): the eye's leash ─────────────────────────
    // Box the camera origin into the engine world bound and carry the orbit pivot
    // with it by the SAME correction, so the eye/pivot pair stays rigid — a clamp
    // that moved only one of the two would silently change the orbit radius and
    // the dolly reference.  Returns nothing and is a no-op for every camera that
    // was already inside the world, which is every camera that has not been flung
    // out by a runaway.  ROUND BC: the bound is per projection now — see
    // KCam_OriginBound / KCAM_ORIGIN_BOUND_ORTHO.
    void ClampToWorld( camera_s *c )
    {
        const float bound = KCam_OriginBound();      // KIWI-UX (ROUND BC, ITEM 1)
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
        ClampToWorld( c );                       // KIWI-UX (ROUND AZ, ITEM 2)
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
        s_orbAccY   = 0.0f;
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
    s_orbAccY += (float)dy;

    // KIWI-UX (ROUND BM, ITEM 3): the rate is 360/height degrees per pixel — one
    // viewport height of drag is one full turn, on both axes, which is Plasticity's
    // own `2*PI*delta/clientHeight` (OrbitControls.ts:673-674).  See
    // KCam_OrbitDegPerPx.  The signs, the clamp and the latch are unchanged.
    const float degPerPx = KCam_OrbitDegPerPx( c );
    // ±89, widened on ONE side to the latched pitch when the orbit began on a
    // pole-exact axis view — see KCam_PitchDragLimit / KCam_PitchDragBounds.
    float lo = 0.0f, hi = 0.0f;
    KCam_PitchDragBounds( s_orbPitch0, &lo, &hi );
    float pitch = s_orbPitch0 - s_orbAccY * degPerPx;
    // ── THE CLAMP MUST NOT BANK TRAVEL IT REFUSED ("stuck at the top") ───────
    // This rig is absolute-from-latch, so a clamped frame used to leave the
    // refused pixels sitting in the accumulator: push into a pole for 200 px and
    // the view did not move again until 200 px had been dragged back out.  From
    // a pole-exact TOP view that is the FIRST thing a drag does, and it reads as
    // the camera being stuck there.  Rewinding the accumulator to the exact
    // travel the limit allowed makes the next pixel in the other direction move
    // by exactly one pixel's worth, which is what the incremental mouselook has
    // always done.  A frame that does not clamp does not touch it.
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

// Release the latch.  Called from the gesture's RELEASE and ABORT arms
// (kiwi_viewport.cpp) — until it runs, nothing may re-derive the pivot.
void KiwiCam_OrbitEnd()
{
    s_orbActive = false;
}

// ─── dolly ───────────────────────────────────────────────────────────────────
void KiwiCam_Dolly( float wheelSteps, int imgX, int imgY )
{
    if ( !( wheelSteps > 0.0f || wheelSteps < 0.0f ) )       // zero or NaN
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

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BM, ITEM 3) — THE ORTHOGRAPHIC ARM, IN CLOSED FORM
    // ═════════════════════════════════════════════════════════════════════════
    // USER REPORT (the round's third item): "This last update really fucked the
    // camera up even more.  Do a camera properness pass ... Copy Plasticity more."
    //
    // WHAT AN ORTHO ZOOM ACTUALLY IS.  The image scale is set by ONE number —
    // KiwiCam_OrthoHalfHeight() = s_dist * tan(fov/2) * 0.75 — and sliding the eye
    // along the view axis changes nothing you can see.  So an ortho wheel notch is
    // exactly two things: SCALE s_dist, and PAN so the world point under the cursor
    // does not move.  Everything else the perspective arm does (a cursor ray, a
    // surface reference, a punch-through clamp, cross-notch reference state) is either
    // meaningless here or, as rounds AJ/AN discovered the hard way, actively wrong:
    // see the KIWI-UX (ROUND BM, ITEM 3) block by KCAM_DOLLY_STEP for how a
    // surface-distance step made zooming in "squirrely".
    //
    // THE ANCHOR, derived.  A pixel at screen offset (u,v) from the image centre
    // views the world point  C + (u*vright + v*vup) * wpp,  where C is the view
    // centre on the eye plane and wpp = 2H/height is the world-per-pixel
    // (KiwiCam_WorldPerPixel's ortho arm, and Plasticity's own ortho pan factor —
    // OrbitControls.ts:276-279).  Scale H by k and wpp scales by k too, so for the
    // SAME pixel to keep viewing the SAME world point:
    //     C' + (u*vright + v*vup) * k*wpp  ==  C + (u*vright + v*vup) * wpp
    //     C' = C + (u*vright + v*vup) * wpp * (1 - k)
    // and `(u*vright + v*vup) * wpp` is precisely the cursor ray's ORIGIN minus the
    // eye, projected onto the two screen axes — which the pick ray already gives us
    // exactly (Ed_CameraCalcRayOrigin's ortho arm builds it, camwnd.cpp; its
    // -vpn*lead term is along vpn and so contributes nothing to either dot).  No
    // iteration, no easement, no residual: the grabbed point is INVARIANT.
    //
    // THE WHEEL FRAME COMES FROM THE EYE, NOT FROM THE CACHED ORBIT PIVOT.  An
    // orbit may deliberately leave its picked pivot off the view axis.  Feeding
    // that pivot into `origin = lookAt - forward * newDist` on the next notch
    // silently centres it and moves the eye sideways before any requested scale
    // is applied.  Instead, derive the old view centre as
    // `origin + forward * d0`, pan that centre by the exact cursor correction,
    // and only then derive the new eye.  The old eye is therefore the k=1 case by
    // construction, regardless of what the preceding camera gesture did.
    //
    // BC DISCIPLINE: the DRIVER (s_dist) is clamped against KCam_MaxDist /
    // KCAM_MIN_DIST and the effective factor is re-derived FROM the clamped value,
    // so at either end the notch is a clean no-op.  For a moving notch, the new
    // eye is derived from the old eye's view centre rather than from cached pivot
    // state, so the only displacement is the requested scale plus its exact pan.
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

        // The leash is another clamp on the SAME factor.  Recompute newDist from
        // it before deriving either the axial move or the lateral cursor pan.
        kEff   = ScaleFactorInsideWorld( c, toAnchor, kEff );
        newDist = d0 * kEff;

        // A clamped notch is a true no-op.  In particular, do not use it to
        // re-seat an off-axis orbit pivot; that hidden state change would make a
        // later gesture discontinuous even though this notch moved no pixels.
        if ( kEff == 1.0f )
            return;

        float move[3];
        for ( int i = 0; i < 3; ++i )
        {
            move[i] = toAnchor[i] * ( 1.0f - kEff );
            c->origin[i] += move[i];
        }

        // Outside a live orbit, rebase the target FROM the new eye.  During a live
        // orbit its world-space pivot stays fixed and the new eye offset is
        // re-latched after Commit below.
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

    // One factor drives BOTH the distance and the cursor correction.  Clamping the
    // former while applying the latter with the requested (unclamped) factor is a
    // sideways move at a zoom boundary, not a dolly.
    const float maxDist = KCam_MaxDist();
    float d0 = s_dist;
    if ( !( d0 > KCAM_MIN_DIST ) ) d0 = KCAM_MIN_DIST;
    if ( d0 > maxDist )            d0 = maxDist;

    float newDist = d0 * KCam_DollyFactor( wheelSteps );
    if ( !( newDist > KCAM_MIN_DIST ) ) newDist = KCAM_MIN_DIST;
    if ( newDist > maxDist )            newDist = maxDist;
    float kEff = newDist / d0;

    // The anchor is the actual cursor-ray surface hit.  On a miss, the point d0
    // units down that same ray gives the old fallback while preserving the same
    // scale-about-point algebra.  No value from a previous notch is retained.
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

    // Scaling about the hit leaves `surface * kEff` in front of the eye.  Clamp
    // the factor itself so the punch-through guard, distance driver, and lateral
    // correction all describe the same transform.  If the eye is already inside
    // the guard, an inward notch is a no-op rather than a recoil.
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

    // The eye leash is the final clamp and therefore the final effective factor.
    // Applying it here prevents Commit from truncating only the origin after the
    // distance has already recorded a larger move.
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

    // ── KIWI-UX (ROUND BC, ITEM 1): THE TWO CEILINGS HAND OVER HERE ─────────
    // ORTHO reaches 262144 (the slab) and PERSPECTIVE stops at 65536 (the
    // inverse-VP degeneracy) — see KCAM_MAX_DIST_ORTHO / _PERSP.  A user who
    // zooms right out in ortho and then presses P would otherwise land the eye
    // 262144 units back with a projection that starts dropping frames, and every
    // clamp downstream reads the NEW ceiling, so s_dist and the eye would also
    // disagree by 200,000 units.  Pull the pair in RIGIDLY instead: keep the
    // current eye-derived view centre fixed and shorten the standoff along the
    // view axis.  A cached off-axis orbit pivot must not add lateral motion to a
    // projection clamp.
    // A camera already inside the new ceiling is untouched, which is every camera
    // going the other way (ortho's ceiling is the larger of the two).
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
    const float maxDist = KCam_MaxDist();
    if ( !( d > KCAM_MIN_DIST ) ) d = KCAM_MIN_DIST;
    if ( d > maxDist )            d = maxDist;
    const double t = tan( DEG2RAD( g_PrefsDlg->camera_fov ) * 0.5 );
    float h = (float)( (double)d * t * 0.75 );
    if ( !( h > 1.0e-3f ) )
        h = 1.0e-3f;
    // KIWI-UX (ROUND AZ, ITEM 2): the fov is a user pref and it multiplies the
    // distance, so the distance clamp alone does not bound this.  See
    // KCAM_ORTHO_HALF_MAX — inactive at the default fov, by construction.
    //
    // KIWI-UX (ROUND BC, ITEM 2): …AND NOW UNREACHABLE BY CONSTRUCTION, because
    // KCam_MaxDist folds the same limit into the DISTANCE ceiling above, so
    // `d * t * 0.75 <= KCAM_ORTHO_HALF_MAX` holds for every fov before this line
    // runs.  It is kept as a floor-of-last-resort for a direct s_dist write that
    // some future path forgets to clamp — but it can no longer create the wheel
    // dead zone it created in AZ, and that is the point.
    if ( h > KCAM_ORTHO_HALF_MAX )
        h = KCAM_ORTHO_HALF_MAX;
    return h;
}

// ─── KIWI-UX (ROUND BM, ITEM 2): the zoom meter ──────────────────────────────
// See kiwi_camera.h for what the two outputs mean and why the scale is log.  The
// range is the SAME pair of numbers every clamp in this file uses — KCAM_MIN_DIST
// and KCam_MaxDist(), the latter already per-projection and already folding in the
// fov-driven half-height cap — so the bar reaches its ends exactly when the wheel
// stops, by construction rather than by agreement.
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

    // Same rate and same signs as CamWnd_Rotate2 (camwnd.cpp:4245) and as the
    // orbit above, so mouselook and orbit never feel like two different cameras.
    // This form is INCREMENTAL, so the limit is taken from the pitch BEFORE the
    // move: from a pole-exact view the first frame either holds at ±90 (looking
    // further into the pole) or leaves it smoothly, instead of snapping a degree.
    // The limit then shrinks back to ±89 as the view re-enters the range, and is
    // exactly ±89 for every look that started inside it.  The widening is ONE-
    // SIDED (KCam_PitchDragBounds): leaving a pole shrinks the band behind you, so
    // the pole is a one-way door out and the OPPOSITE pole is never reachable by
    // looking.  Incremental, so there is nothing for a clamped frame to bank —
    // the orbit's rewind has no counterpart here.
    float lo = 0.0f, hi = 0.0f;
    KCam_PitchDragBounds( c->angles[0], &lo, &hi );
    c->angles[1] -= (float)dx * KCAM_DEG_PER_PX;
    c->angles[0] -= (float)dy * KCAM_DEG_PER_PX;

    if ( c->angles[0] > hi ) c->angles[0] = hi;
    if ( c->angles[0] < lo ) c->angles[0] = lo;
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

    // ── KIWI-UX (ROUND BM, ITEM 3): THE REFERENCE IS THE PIVOT, LIKE THEIRS ──
    // Shakeout E's per-gesture area Pick is gone.  Plasticity pans at the world-
    // per-pixel of `this.target` in both projections and nothing else
    // (plasticity/src/components/viewport/OrbitControls.ts:265-279), and the reason
    // KIWI moved off that — a pivot that wandered because the old dolly derived it
    // from a changing surface reference — no longer exists: the current dolly
    // derives its target from the resulting eye at the scaled standoff.  In ORTHO,
    // which is the default projection, KiwiCam_WorldPerPixel is depth-independent
    // (2H/height), so the surface pick could never have changed the answer there in
    // the first place.
    //
    // The cache stays per-gesture because it is FREE and exact: KiwiCam_Translate
    // moves the eye and the pivot together, so the pivot's world-per-pixel is
    // invariant for the whole drag — the same invariance Plasticity relies on when
    // it adds `panOffset` to both (OrbitControls.ts:228-234).
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

// ─── the 2D marker anchor (kiwi_camera.h WHERE THE CAMERA IS) ────────────────
void KiwiCam_MarkerViewpoint( float out3[3] )
{
    if ( !out3 )
        return;
    const camera_s *c = Ed_Camera();
    Copy3( c->origin, out3 );
    if ( !KiwiCam_Ortho() )
        return;                              // perspective: the eye IS the place
    // Walk the pseudo-eye forward to the standoff plane.  s_dist is the ortho
    // zoom driver, and the dolly keeps `origin = lookAt - f * s_dist`, so this is
    // the pivot whenever that invariant holds and the on-axis point at the same
    // depth when a live orbit has taken the pivot off-axis.
    float f[3];
    ViewForward( c, f );
    Mad3( out3, f, s_dist, out3 );
}

// ─── the pose signature (kiwi_camera.h THE POSE SIGNATURE) ───────────────────
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

// ─── which side of a face the camera is on (kiwi_camera.h) ───────────────────
float KiwiCam_FacingSign( const float point[3], const float normal[3] )
{
    if ( !point || !normal )
        return 1.0f;
    const camera_s *c = Ed_Camera();
    float d;
    if ( KiwiCam_Ortho() )
    {
        // No eye point means anything in a parallel projection — the eye is parked
        // KiwiCam_Distance() behind the pivot and moves with the ZOOM.  The view
        // DIRECTION is the whole answer: the camera is on the normal's side when
        // it looks against the normal.  Derived from the angles through the same
        // AngleVectors call the draw uses, so it can never be a frame behind.
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
    // ±90, NOT ±89.  This is the ABSOLUTE re-aim every axis snap goes through
    // (the view cube's faces and corners, the Alt+MMB step and swipe, View Face),
    // and clamping it to 89 was what made a TOP view a 1-degree tilt that leaked
    // side faces — the pole is well defined here (KCam_PitchDragLimit).  The clamp
    // stays as the entry point's sanity bound: no caller may aim past a pole.
    if ( c->angles[0] >  KCAM_PITCH_MAX ) c->angles[0] =  KCAM_PITCH_MAX;
    if ( c->angles[0] < -KCAM_PITCH_MAX ) c->angles[0] = -KCAM_PITCH_MAX;

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
    if ( d < KCAM_MIN_DIST )  d = KCAM_MIN_DIST;
    if ( d > KCam_MaxDist() ) d = KCam_MaxDist();
    s_dist    = d;
    s_have    = true;
    s_panHave = false;                    // the cached pan scale belonged to the old view
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

    // GetAsyncKeyState reports the PHYSICAL key regardless of focus, so gate on
    // this thread owning the foreground: typing WASD in another app must not fly
    // the editor's camera.
    if ( ::GetActiveWindow() == nullptr )
        return;

    // ARROWS ONLY (user directive — WASD clashed with the modern bindings).
    // While RMB-look is held modifiers are ignored so Shift can boost; while
    // merely hovering, bare arrows only — Shift/Ctrl/Alt + arrow are live classic
    // bindings (TexShift*, TexRotate*, SelectNudge* — mainfrm.cpp:1109-1143) that
    // keep their meaning in both keymap profiles.  (kiwi_keymap.cpp unbinds the
    // classic bare-arrow CameraForward/Back/Left/Right tank hops in the modern
    // profile.)
    //
    // KIWI-UX (CLEANUP, C-29): there is NO vertical term.  The Q/E up-down fly was
    // removed with WASD, so the `vert` residue and the always-zero `c->vup` term it
    // fed are gone.  Restoring a vertical binding is a feature decision, not a
    // cleanup one.
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

    // prefs.h:38 `int m_nMoveSpeed;  // "MoveSpeed"  (350)` — the classic camera
    // paths use it as a rate (Cam_MouseControl: units/s at full joystick
    // deflection is m_nMoveSpeed * 6; CamWnd_Scroll: units per wheel notch is
    // m_nMoveSpeed * 0.7 * modifier), so reading it as units/SECOND here puts the
    // fly at a familiar 350 u/s with the stock pref.
    float speed = (float)g_PrefsDlg->m_nMoveSpeed * KiwiCam_FlySpeedScale();
    // ══════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BM, ITEM 1a + 3) — IN ORTHO THE ARROWS PAN, AT A PIXEL RATE
    // ══════════════════════════════════════════════════════════════════════════
    // Round BK's arm read
    //     speed = KiwiCam_OrthoHalfHeight() * 1.0f * KiwiCam_FlySpeedScale();
    // which at the shipped 4x default is TWO SCREEN HEIGHTS PER SECOND (six on
    // Shift) — the multiplier applied twice, once inside the rate's own units and
    // once outside.  And the direction was `vpn`, which in a parallel projection
    // moves nothing you can see; BK made that visible by clipping the slab against
    // the eye, which is the feature this round deletes (kiwi_camera.h ITEM 1a).
    //
    // WHAT THE ARROWS DO IN ORTHO NOW: they PAN.  Up/Down along vup, Left/Right
    // along vright, at KCAM_FLY_PX_PER_SEC screen pixels per second converted
    // through KiwiCam_WorldPerPixel — the same world-per-pixel Plasticity's
    // orthographic pan uses (OrbitControls.ts:276-279).  Constant screen speed at
    // every zoom, which is the property BK was after, and the user's fly-speed
    // slider still scales it (normalised by its own default so it is counted once).
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

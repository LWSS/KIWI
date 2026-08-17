#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_section.cpp — KIWI-UX (ROUND BM, ITEM 1b): SECTION ANALYSIS.
//                    KIWI-UX (ROUND BP, ITEM 1): the projection route.
// The directive, the autopsy, the state machine, the "always a Z level" ruling and
// the ordering conditions are all on kiwi_section.h.  This file is the
// implementation, the derivation and the numbers.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "kiwi_section.h"
#include "kiwi_camera.h"
#include "kiwi_command.h"
#include "kiwi_lines.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_ux.h"
#include "kiwi_units.h"
#include "kiwi_vec.h"       // the one spelling of Dot3/Sub3/Mad3/...
#include "radiant_frame.h"  // Radiant_RegisterCommand

#include <math.h>

// ── ported / sibling entry points (each verified against its DEFINITION) ─────
extern camera_s *Ed_Camera();                          // camwnd.cpp:156
extern int       g_nUpdateBits;                        // engine_stubs.cpp:773
extern int       Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:112
extern void      Radiant_FL_Log( const char *fmt, ... );   // mainfrm.cpp:137

namespace
{
    // ── the state machine (kiwi_section.h "THE THREE STATES") ───────────────
    enum secMode_t { SEC_OFF = 0, SEC_PICKING, SEC_ON };
    secMode_t s_mode = SEC_OFF;

    // ── KIWI-UX (ROUND BP, ITEM 1): ONE NUMBER ──────────────────────────────
    // The plane is ALWAYS (0,0,1) with offset s_level; the surviving half is
    // z <= s_level.  s_anchor is only where the HANDLE is drawn — it carries the
    // x/y the user clicked so the lollipop appears where they pointed, and its z
    // is always s_level.
    float s_level     = 0.0f;
    float s_anchor[3] = { 0.0f, 0.0f, 0.0f };

    // ── THE ONE STATE THE PICKER IS ALLOWED TO SEE (kiwi_section.h) ─────────
    // Set by CamWnd_SetupScene through KiwiSection_NoteFrameCut once per camera
    // frame.  ClampRayStart and PointVisible key on THIS, never on s_mode, so a
    // section that is armed but not cutting cannot bend a single pick ray.
    bool s_frameCut = false;

    // The drag.  s_grabScalar is the z the cursor mapped to at the press and
    // s_grabLevel is s_level at that moment — the round-L grab-rebase pair, so
    // taking hold of the ball never moves the plane by itself.
    bool  s_grabbed    = false;
    bool  s_hot        = false;         // the ball is under the cursor
    float s_grabScalar = 0.0f;
    float s_grabLevel  = 0.0f;

    // One-shot refusal reporting, so a level ortho view does not print per frame.
    int  s_refusedKind = 0;             // 0 none, 1 perspective, 2 ortho

    // The fixed plane normal, spelled once.
    const float KSEC_N[3] = { 0.0f, 0.0f, 1.0f };

    // ── the handle, in PIXELS (screen-constant, like every other handle) ────
    const float KSEC_RING_PIX  = 18.0f;
    const float KSEC_STEM_PIX  = 42.0f;
    const float KSEC_BALL_PIX  = 5.0f;
    const float KSEC_PICK_PIX  = 12.0f;
    const int   KSEC_RING_SEGS = 24;
    const int   KSEC_BALL_SEGS = 12;
    const float KSEC_PLANE_PIX = 160.0f;

    const float KSEC_COL_PLANE[3] = { 0.36f, 0.68f, 0.95f };   // the cut, cool blue
    const float KSEC_COL_RING [3] = { 0.86f, 0.74f, 0.28f };
    const float KSEC_COL_BALL [3] = { 1.00f, 0.90f, 0.30f };
    const float KSEC_COL_HOT  [3] = { 1.00f, 0.98f, 0.72f };

    // ── THE TWO ORDERING FLOORS (kiwi_section.h "WHEN THE FOLD IS REFUSED") ──
    // PERSPECTIVE wants the eye strictly ABOVE the cut.  One world unit is the
    // floor rather than an epsilon because the depth RESOLUTION of the folded
    // projection is proportional to |Cw|: at |Cw| = 1 a 1000-unit-deep view still
    // resolves ~0.14 units, which is finer than anything the editor draws, and
    // below that the camera is sitting on the cut and nothing useful is hidden.
    const float KSEC_PERSP_EYE_MIN = 1.0f;
    // ORTHO wants the camera looking DOWN at all.  0.02 is ~1.1 degrees off level;
    // the arithmetic for why the floor can be this small is in the derivation
    // (`a` scales up as Cf shrinks, so the depth slope a*Cf stays comparable to
    // the base slab's right down to about half a degree).
    const float KSEC_ORTHO_DOWN_MIN = 0.02f;

    // A basis spanning the horizontal plane.  With a fixed +Z normal this is just
    // world X and Y, written out rather than derived so the handle's square is
    // axis-aligned and reads as a floor plan.
    void PlaneBasis( float *e0, float *e1 )
    {
        e0[0] = 1.0f; e0[1] = 0.0f; e0[2] = 0.0f;
        e1[0] = 0.0f; e1[1] = 1.0f; e1[2] = 0.0f;
    }

    void HandleTip( float *outTip, float *outWpp )
    {
        const float wpp = KiwiCam_WorldPerPixel( s_anchor );
        if ( outWpp )
            *outWpp = wpp;
        Mad3( s_anchor, KSEC_N, KSEC_STEM_PIX * wpp, outTip );
    }

    bool BallHit( int imgX, int imgY )
    {
        float tip[3];
        HandleTip( tip, nullptr );
        float tx, ty;
        if ( !Pick_WorldToImage( tip, &tx, &ty ) )
            return false;                                   // behind the eye
        const float dx = tx - (float)imgX;
        const float dy = ty - (float)imgY;
        return sqrtf( dx * dx + dy * dy ) <= KSEC_PICK_PIX;
    }

    // Camera-facing polygon, filled by its long diagonals when `solid` — lifted in
    // shape from kiwi_lollipop.cpp's EmitDisc.
    void EmitDisc( const camera_s *c, const float p[3], float r, int segs, bool solid )
    {
        float prev[3], pt[3];
        for ( int i = 0; i <= segs; ++i )
        {
            const float th = ( 6.283185307179586f * (float)( i % segs ) ) / (float)segs;
            const float cx = cosf( th ) * r;
            const float cy = sinf( th ) * r;
            for ( int k = 0; k < 3; ++k )
                pt[k] = p[k] + c->vright[k] * cx + c->vup[k] * cy;
            if ( i > 0 )
                KiwiLines_Add( prev, pt );
            Copy3( pt, prev );
        }
        if ( !solid )
            return;
        for ( int i = 0; i < segs / 2; ++i )
        {
            const float th0 = ( 6.283185307179586f * (float)i ) / (float)segs;
            const float th1 = th0 + 3.14159265358979f;
            float a[3], b[3];
            for ( int k = 0; k < 3; ++k )
            {
                a[k] = p[k] + c->vright[k] * cosf( th0 ) * r + c->vup[k] * sinf( th0 ) * r;
                b[k] = p[k] + c->vright[k] * cosf( th1 ) * r + c->vup[k] * sinf( th1 ) * r;
            }
            KiwiLines_Add( a, b );
        }
    }

    void Repaint()
    {
        g_nUpdateBits |= 1;
    }

    void SetLevel( float z )
    {
        s_level     = z;
        s_anchor[2] = z;
    }

    // What the click landed on, resolved into a Z LEVEL.  Any hit at all gives one
    // — a face, a patch, a model, a prefab — because the answer is only ever the
    // hit point's height (kiwi_section.h "THE PLANE IS ALWAYS A HORIZONTAL Z
    // LEVEL").  Nothing under the cursor falls back to the view pivot's height.
    bool LevelFromClick( int imgX, int imgY )
    {
        ray_t ray;
        if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
            return false;

        // Area granularity only: a section wants the SURFACE the user pointed at,
        // not the nearest vertex the mode mask might prefer.  Same one call, and the
        // same argument, as KiwiCam_OrbitBegin's pivot pick.
        const pick_result_t r = Pick( ray, SEL_MASK_FACE | SEL_MASK_OBJECT );
        if ( r.valid )
        {
            s_anchor[0] = r.point[0];
            s_anchor[1] = r.point[1];
            SetLevel( r.point[2] );
            return true;
        }

        // NOTHING UNDER THE CURSOR.  Where the cursor ray crosses the horizontal
        // plane through the orbit pivot: that is the depth the user is already
        // working at, and it is the same reference every other "clicked empty
        // space" path in this editor falls back to.
        const float *pivot = KiwiCam_LookAt();
        float p[3] = { pivot[0], pivot[1], pivot[2] };
        if ( fabsf( ray.dir[2] ) > 1.0e-4f )
        {
            const float t = ( pivot[2] - ray.origin[2] ) / ray.dir[2];
            if ( t > 0.0f )
                Mad3( ray.origin, ray.dir, t, p );
        }
        s_anchor[0] = p[0];
        s_anchor[1] = p[1];
        SetLevel( p[2] );
        return true;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  the command / the toggle
// ═════════════════════════════════════════════════════════════════════════════
void KiwiSection_RegisterCommands()
{
    // Unbound in both profiles: it is reached by the button above the view cube and
    // by name from the §15 palette.
    Radiant_RegisterCommand( "KiwiSectionAnalysis", 0, 0, KIWI_CMD_SECTION_TOGGLE );
}

// ── KIWI-UX (ROUND BP, ITEM 3): TOGGLE-OFF PROVABLY CLEARS EVERYTHING ────────
// The round-BP autopsy's second finding: a section could be armed with nothing to
// show for it, and its pick clamp went on bending rays.  There is now exactly ONE
// function that leaves the feature, every exit calls it, and it clears the frame
// verdict as well as the mode — so the very next pick sees no section at all
// without waiting for a frame to be drawn.
void KiwiSection_Reset()
{
    s_mode        = SEC_OFF;
    s_frameCut    = false;
    s_grabbed     = false;
    s_hot         = false;
    s_grabScalar  = 0.0f;
    s_grabLevel   = 0.0f;
    s_refusedKind = 0;
    Repaint();
}

void KiwiSection_Toggle()
{
    switch ( s_mode )
    {
    case SEC_OFF:
        s_mode = SEC_PICKING;
        Sys_Printf( "Section analysis: click anywhere to cut at that point's HEIGHT "
                    "(everything above it disappears).  Escape cancels.\n" );
        Repaint();
        break;
    case SEC_PICKING:
        KiwiSection_Reset();
        Sys_Printf( "Section analysis: cancelled.\n" );
        break;
    default:
        KiwiSection_Reset();
        Sys_Printf( "Section analysis: OFF - the whole model is back, and picking is "
                    "unclamped again.\n" );
        Radiant_FL_Log( "SECTION off (cleared)" );
        break;
    }
}

bool KiwiSection_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned)KIWI_CMD_SECTION_TOGGLE )
        return false;
    KiwiSection_Toggle();
    return true;
}

bool  KiwiSection_Active()  { return s_mode == SEC_ON; }
bool  KiwiSection_Picking() { return s_mode == SEC_PICKING; }
bool  KiwiSection_Cutting() { return s_mode == SEC_ON && s_frameCut; }
float KiwiSection_Level()   { return s_level; }

bool KiwiSection_HandleEscape()
{
    if ( s_mode != SEC_PICKING )
        return false;                       // ON is left with the button, not Escape:
                                            // Escape means "drop the selection" and a
                                            // section is not one.
    KiwiSection_Reset();
    Sys_Printf( "Section analysis: cancelled.\n" );
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  input
// ═════════════════════════════════════════════════════════════════════════════
bool KiwiSection_ClickPick( int imgX, int imgY )
{
    if ( s_mode != SEC_PICKING )
        return false;
    if ( !LevelFromClick( imgX, imgY ) )
    {
        // No ray at all means the viewport has no size yet — leave the pick state
        // armed rather than silently dropping the mode the user just asked for.
        return true;
    }
    s_mode        = SEC_ON;
    s_grabbed     = false;
    s_refusedKind = 0;
    // THE LATCH SAYS SO, OUT LOUD, ALWAYS (round BO) — and both copies survive,
    // because the console line is what the user reads live and the first-light
    // line is what an autopsy reads back afterwards.  Round BP's own autopsy was
    // settled by exactly these two lines being in the log.
    char zb[64];
    Sys_Printf( "[KIWI] SECTION ON at Z = %s - everything above that height is cut "
                "away in the 3D view.  Drag the lollipop to slide it; press the "
                "SECTION button again to clear.\n",
                KiwiUnits_Format( zb, sizeof( zb ), s_level ) );
    Radiant_FL_Log( "SECTION latch: level=%.3f anchor=(%.1f %.1f %.1f)",
                    s_level, s_anchor[0], s_anchor[1], s_anchor[2] );
    Repaint();
    return true;
}

bool KiwiSection_HandleDown( int imgX, int imgY )
{
    if ( s_mode != SEC_ON )
        return false;
    if ( !BallHit( imgX, imgY ) )
        return false;

    ray_t ray;
    if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
        return false;
    float hit[3];
    if ( !KiwiCam_RayAxis( ray, s_anchor, KSEC_N, hit ) )
    {
        // Straight down the Z axis the solve is degenerate and the
        // KCAM_RAYAXIS_MIN_DEN gate refused it (kiwi_camera.h "THE SAMPLE GATE").
        Sys_Printf( "Section: orbit a little - from straight overhead the slide "
                    "cannot be aimed.\n" );
        return false;
    }
    // GRAB-REBASE: record where the cursor mapped to and what the level was, and
    // drive the drag from the DIFFERENCE.  A press with no movement is therefore
    // exactly a no-op (kiwi_lollipop.h "THE GRAB DOES NOT MOVE ANYTHING").
    s_grabScalar = hit[2];
    s_grabLevel  = s_level;
    s_grabbed    = true;
    s_hot        = true;
    return true;
}

void KiwiSection_HandleDrag( int imgX, int imgY )
{
    if ( s_mode != SEC_ON || !s_grabbed )
        return;
    ray_t ray;
    if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
        return;
    float hit[3];
    if ( !KiwiCam_RayAxis( ray, s_anchor, KSEC_N, hit ) )
        return;                             // refused sample: HOLD, do not jump
    SetLevel( s_grabLevel + ( hit[2] - s_grabScalar ) );
    Repaint();
}

void KiwiSection_HandleUp()
{
    if ( !s_grabbed )
        return;
    s_grabbed = false;
    char buf[64];
    Sys_Printf( "Section: cutting at Z = %s.\n",
                KiwiUnits_Format( buf, sizeof( buf ), s_level ) );
}

void KiwiSection_HandleAbort()
{
    if ( !s_grabbed )
        return;
    s_grabbed = false;
    SetLevel( s_grabLevel );                // lost capture: put it back at the grab
    Repaint();
}

void KiwiSection_Hover( int imgX, int imgY, bool over )
{
    const bool was = s_hot;
    s_hot = ( s_mode == SEC_ON ) && over && BallHit( imgX, imgY );
    if ( s_hot != was )
        Repaint();
}

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND BP, ITEM 1b — THE OBLIQUE NEAR PLANE, DERIVED IN THIS TREE
// ═════════════════════════════════════════════════════════════════════════════
// CONVENTIONS, read off the code rather than assumed:
//   * MatrixForViewer (com_math.cpp:3080) builds a ROW-VECTOR view matrix whose
//     components are ( dot(p-o, vright), dot(p-o, vup), dot(p-o, vpn), 1 ) — the
//     first column is -axis[1] == +vright, the second is axis[2] == vup, the third
//     is axis[0] == vpn.  Call that v = ( r, u, f, 1 ).
//   * CamWnd_SetupScene composes clip = v * P, so clip.z = v . COLUMN 2 of P and
//     clip.w = v . COLUMN 3 of P.  (Perspective sets m[2][3] = 1, which is exactly
//     "view depth becomes clip w".)
//   * D3D keeps a fragment when 0 <= clip.z <= clip.w.  The NEAR plane is therefore
//     nothing but "column 2 of P, tested >= 0" — which is the whole opportunity:
//     replace column 2 and the near plane becomes whatever plane you like.
//     (This is Lengyel's oblique-frustum technique; in the GL convention the near
//     plane is row3+row4 and he must subtract the w row, which is where the "+M4"
//     in the published formula comes from.  In D3D's 0..w range there is no such
//     term and the substitution is direct.)
//
// THE SECTION PLANE, IN VIEW SPACE.  The surviving half is world z <= level, i.e.
// the world plane W = ( 0, 0, -1, level ) tested W.(p,1) >= 0.  Substituting
// p = o + r*vright + u*vup + f*vpn gives the view-space covector C with
// C.v == W.(p,1):
//       Cr = -vright.z      Cu = -vup.z      Cf = -vpn.z      Cw = level - o.z
// (each is just -dot( (0,0,1), basisVector ), and Cw is -dot(n,o) + level.)
//
// COLUMN 2 = a * C for a scale a > 0, which leaves the zero set — the cut — exactly
// the section plane whatever a is.  a is chosen so the FAR test survives:
//
//   PERSPECTIVE (clip.w = f, and the base far plane is effectively at infinity):
//     the far test is f - a*(C.v) >= 0.  Over the frustum |r| <= f*tanX and
//     |u| <= f*tanY, so C.v <= K*f + Cw with K = |Cr|*tanX + |Cu|*tanY + Cf.
//     Requiring a*(K*f + Cw) <= f for every f >= zNear gives, in one expression
//     that also covers K <= 0 and Cw <= 0,
//         a = zNear / ( max(K,0)*zNear + max(Cw,0) )
//     (with Cw <= 0 this is exactly 1/K, the largest a that can never clip.)
//
//   ORTHO (clip.w = 1, so the far test is a*(C.v) <= 1):
//     the view volume is the box |r| <= halfW, |u| <= halfH, |f| <= depthHalf, so
//         M = |Cr|*halfW + |Cu|*halfH + |Cf|*depthHalf + Cw
//     is the largest C.v that can occur and a = guardC / M puts that at the guard
//     band exactly, the same 2047/2048 headroom both base projections carry.
//
// DEPTH, AND THE TWO ORDERING CONDITIONS (kiwi_section.h states them; here is why):
//     perspective  z_ndc = clip.z/clip.w = a*( Cr*px*tanX + Cu*py*tanY + Cf )
//                                        + a*Cw/f      for the pixel (px,py)
//                  — the first term is CONSTANT down a pixel's ray, so
//                    d(z_ndc)/df = -a*Cw/f^2, which is positive (farther == larger,
//                    the D3D LESS test) if and only if Cw < 0: THE EYE ABOVE THE CUT.
//     ortho        clip.w = 1 and r,u do not vary with f at a fixed pixel, so
//                  z_clip is affine in f with slope a*Cf: ordering needs Cf > 0,
//                  i.e. the camera looking DOWNWARD.
// Outside those the fold is refused and the frame draws uncut (and the pick clamp
// goes with it — kiwi_section.h).
//
// THE COST, STATED.  Depth stops being the camera's hyperbolic z and becomes
// distance from the cut.  That is not a loss here: at 10000 units the base
// perspective projection resolves ~1.5 units per depth step and the folded one
// resolves ~0.03; the ortho slab's 1/16-unit step is matched or bettered for every
// Cf above about half a degree.  What IS lost is the base near plane — geometry
// nearer than r_znear is now drawn — but everything that close to the eye is above
// the cut in any useful configuration and is removed by the section test itself,
// and anything behind the eye has clip.w < 0 and fails 0 <= z <= w regardless.
bool KiwiSection_ObliqueDepthColumn( const float origin[3], const float vpn[3],
                                     const float vright[3], const float vup[3],
                                     bool ortho, float guardC,
                                     float tanX, float tanY, float zNear,
                                     float halfW, float halfH, float depthHalf,
                                     float outCol[4] )
{
    if ( s_mode != SEC_ON || !origin || !vpn || !vright || !vup || !outCol )
        return false;

    const float Cr = -vright[2];
    const float Cu = -vup[2];
    const float Cf = -vpn[2];
    const float Cw = s_level - origin[2];

    if ( ortho )
    {
        if ( !( Cf >= KSEC_ORTHO_DOWN_MIN ) )
        {
            if ( s_refusedKind != 2 )
            {
                s_refusedKind = 2;
                Sys_Printf( "Section: a level (front/side) ORTHOGRAPHIC view has no depth "
                            "along a Z cut, so the cut is held off and picking is "
                            "unclamped.  Orbit downward, or switch to PERSP.\n" );
            }
            return false;
        }
        const float M = fabsf( Cr ) * halfW + fabsf( Cu ) * halfH
                      + fabsf( Cf ) * depthHalf + Cw;
        if ( !( M > 1.0e-6f ) )
            return false;                   // the whole slab is above the cut
        const float a = guardC / M;
        if ( !( a > 0.0f ) )
            return false;
        outCol[0] = a * Cr;
        outCol[1] = a * Cu;
        outCol[2] = a * Cf;
        outCol[3] = a * Cw;
        s_refusedKind = 0;
        return true;
    }

    if ( !( Cw <= -KSEC_PERSP_EYE_MIN ) )
    {
        if ( s_refusedKind != 1 )
        {
            s_refusedKind = 1;
            Sys_Printf( "Section: the camera is at or below the cut, so there is nothing "
                        "above it to remove and depth could not be sorted.  The cut is "
                        "held off (and picking is unclamped) until you rise above "
                        "it.\n" );
        }
        return false;
    }

    const float K   = fabsf( Cr ) * tanX + fabsf( Cu ) * tanY + Cf;
    const float den = ( K > 0.0f ? K * zNear : 0.0f ) + ( Cw > 0.0f ? Cw : 0.0f );
    const float a   = ( den > 1.0e-6f ) ? ( zNear / den ) : 1.0f;
    if ( !( a > 0.0f ) )
        return false;
    outCol[0] = a * Cr;
    outCol[1] = a * Cu;
    outCol[2] = a * Cf;
    outCol[3] = a * Cw;
    s_refusedKind = 0;
    return true;
}

void KiwiSection_NoteFrameCut( bool cutting )
{
    s_frameCut = cutting;
}

// ═════════════════════════════════════════════════════════════════════════════
//  the retired D3D9 clip-plane bracket
// ═════════════════════════════════════════════════════════════════════════════
// ROUND BP: both are no-ops.  The user's own first-light log proved the device
// accepts SetClipPlane + D3DRS_CLIPPLANEENABLE and cuts nothing (kiwi_section.h
// quotes it), so the section does not go through that route any more.  The command,
// its backend handler and the BO probe all stay in the tree — they are correct and
// the probe is the evidence — but nothing arms them, and these two survive only so
// camwnd.cpp's matched-pair call sites need no edit.
void KiwiSection_EmitClipBegin() {}
void KiwiSection_EmitClipEnd()   {}

// ═════════════════════════════════════════════════════════════════════════════
//  render
// ═════════════════════════════════════════════════════════════════════════════
void KiwiSection_DrawWorld()
{
    if ( s_mode != SEC_ON )
        return;
    if ( !KiwiUX_ModernInput() )
        return;
    const camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;

    float wpp = 0.0f;
    float tip[3];
    HandleTip( tip, &wpp );
    if ( !( wpp > 0.0f ) )
        return;

    KiwiLines_Begin( KSEC_RING_SEGS + KSEC_BALL_SEGS * 2 + 12, 2 );

    // ── the cut itself: a square lying IN the level, so an invisible plane has a
    //    visible extent.  Screen-constant, like every handle in this layer.
    {
        float e0[3], e1[3];
        PlaneBasis( e0, e1 );
        const float h = KSEC_PLANE_PIX * wpp;
        float q[4][3];
        for ( int k = 0; k < 3; ++k )
        {
            q[0][k] = s_anchor[k] - e0[k] * h - e1[k] * h;
            q[1][k] = s_anchor[k] + e0[k] * h - e1[k] * h;
            q[2][k] = s_anchor[k] + e0[k] * h + e1[k] * h;
            q[3][k] = s_anchor[k] - e0[k] * h + e1[k] * h;
        }
        KiwiLines_Color( KSEC_COL_PLANE[0], KSEC_COL_PLANE[1], KSEC_COL_PLANE[2] );
        for ( int i = 0; i < 4; ++i )
            if ( !KiwiLines_Add( q[i], q[( i + 1 ) & 3] ) )
                break;
    }

    // ── the ring, drawn IN the plane so it reads as lying on the cut.
    {
        float e0[3], e1[3];
        PlaneBasis( e0, e1 );
        const float r = KSEC_RING_PIX * wpp;
        KiwiLines_Color( KSEC_COL_RING[0], KSEC_COL_RING[1], KSEC_COL_RING[2] );
        float prev[3];
        for ( int i = 0; i <= KSEC_RING_SEGS; ++i )
        {
            const float th = ( 6.283185307179586f * (float)( i % KSEC_RING_SEGS ) )
                           / (float)KSEC_RING_SEGS;
            float p[3];
            for ( int k = 0; k < 3; ++k )
                p[k] = s_anchor[k] + e0[k] * cosf( th ) * r + e1[k] * sinf( th ) * r;
            if ( i > 0 && !KiwiLines_Add( prev, p ) )
                break;
            Copy3( p, prev );
        }
    }

    // ── the stem and the ball.
    {
        const float *col = ( s_hot || s_grabbed ) ? KSEC_COL_HOT : KSEC_COL_BALL;
        KiwiLines_Color( col[0], col[1], col[2] );
        float base[3];
        Mad3( s_anchor, KSEC_N, KSEC_RING_PIX * wpp, base );
        KiwiLines_Add( base, tip );
        EmitDisc( c, tip, KSEC_BALL_PIX * wpp, KSEC_BALL_SEGS, true );
    }

    KiwiLines_Flush();
}

// ═════════════════════════════════════════════════════════════════════════════
//  picking (see kiwi_section.h for why there are two halves — and for why BOTH
//  are gated on KiwiSection_Cutting() and never on the mode)
// ═════════════════════════════════════════════════════════════════════════════
void KiwiSection_ClampRayStart( float *start, const float *dir )
{
    if ( !KiwiSection_Cutting() || !start || !dir )
        return;
    // Only a ray that BEGINS above the level needs moving.
    const float sd = start[2] - s_level;
    if ( sd <= 0.0f )
        return;
    if ( dir[2] >= -1.0e-6f )
        return;                             // parallel, or heading further up: the
                                            // ray never reaches visible geometry and
                                            // Test_Ray's own miss is the right answer
    const float t = -sd / dir[2];           // > 0 by the two tests above
    Mad3( start, dir, t, start );
}

bool KiwiSection_PointVisible( const float *p )
{
    if ( !KiwiSection_Cutting() || !p )
        return true;
    return p[2] <= s_level;
}

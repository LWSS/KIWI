#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Horizontal-Z section analysis using an oblique projection near plane.

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
#include "kiwi_vec.h"       // Dot3/Sub3/Mad3/...
#include "radiant_frame.h"  // Radiant_RegisterCommand

#include <math.h>

// Ported/sibling entry points.
extern camera_s *Ed_Camera();                          // camwnd.cpp:161
extern int       g_nUpdateBits;                        // engine_stubs.cpp:773
extern int       Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:118
extern void      Radiant_FL_Log( const char *fmt, ... );   // mainfrm.cpp:145

namespace
{
    // OFF -> PICKING -> ON.
    enum secMode_t { SEC_OFF = 0, SEC_PICKING, SEC_ON };
    secMode_t s_mode = SEC_OFF;

    // World z <= s_level survives; s_anchor x/y only positions the handle.
    float s_level     = 0.0f;
    float s_anchor[3] = { 0.0f, 0.0f, 0.0f };

    // Set once per camera frame; picking keys on this render verdict, not s_mode.
    bool s_frameCut = false;

    // The cursor-Z/level pair rebases a drag so pressing alone cannot move the cut.
    bool  s_grabbed    = false;
    bool  s_hot        = false;         // the ball is under the cursor
    float s_grabScalar = 0.0f;
    float s_grabLevel  = 0.0f;

    // One-shot refusal reporting, so a level ortho view does not print per frame.
    int  s_refusedKind = 0;             // 0 none, 1 perspective, 2 ortho

    const float KSEC_N[3] = { 0.0f, 0.0f, 1.0f };

    // Handle dimensions are screen pixels.
    const float KSEC_RING_PIX  = 18.0f;
    const float KSEC_STEM_PIX  = 42.0f;
    const float KSEC_BALL_PIX  = 5.0f;
    const float KSEC_PICK_PIX  = 12.0f;
    const int   KSEC_RING_SEGS = 24;
    const int   KSEC_BALL_SEGS = 12;
    const float KSEC_PLANE_PIX = 160.0f;

    const float KSEC_COL_PLANE[3] = { 0.36f, 0.68f, 0.95f };   // cut plane
    const float KSEC_COL_RING [3] = { 0.86f, 0.74f, 0.28f };
    const float KSEC_COL_BALL [3] = { 1.00f, 0.90f, 0.30f };
    const float KSEC_COL_HOT  [3] = { 1.00f, 0.98f, 0.72f };

    // One world unit keeps folded perspective depth useful and the eye off the cut.
    const float KSEC_PERSP_EYE_MIN = 1.0f;
    // 0.02 (~1.1 degrees) avoids near-level ortho depth instability.
    const float KSEC_ORTHO_DOWN_MIN = 0.02f;

    // World X/Y keeps the horizontal handle square axis-aligned.
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

    // Camera-facing polygon, optionally filled by long diagonals.
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

    // Any hit supplies only its world Z; empty space uses the view-pivot level.
    bool LevelFromClick( int imgX, int imgY )
    {
        ray_t ray;
        if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
            return false;

        // Use area granularity so the surface, not a nearby vertex, sets the level.
        const pick_result_t r = Pick( ray, SEL_MASK_FACE | SEL_MASK_OBJECT );
        if ( r.valid )
        {
            s_anchor[0] = r.point[0];
            s_anchor[1] = r.point[1];
            SetLevel( r.point[2] );
            return true;
        }

        // Empty space inherits the working depth from the horizontal pivot plane.
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

// Command and state.
void KiwiSection_RegisterCommands()
{
    // Unbound: reached by the view-cube button or command palette.
    Radiant_RegisterCommand( "KiwiSectionAnalysis", 0, 0, KIWI_CMD_SECTION_TOGGLE );
}

// Every exit clears the mode and frame verdict before the next pick.
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
        return false;                       // Escape keeps selection semantics while ON.
    KiwiSection_Reset();
    Sys_Printf( "Section analysis: cancelled.\n" );
    return true;
}

// Input.
bool KiwiSection_ClickPick( int imgX, int imgY )
{
    if ( s_mode != SEC_PICKING )
        return false;
    if ( !LevelFromClick( imgX, imgY ) )
    {
        // A missing viewport ray leaves the requested pick armed.
        return true;
    }
    s_mode        = SEC_ON;
    s_grabbed     = false;
    s_refusedKind = 0;
    // Keep live user feedback and the diagnostic latch record.
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
        // A straight-down view is degenerate for the Z-axis cursor solve.
        Sys_Printf( "Section: orbit a little - from straight overhead the slide "
                    "cannot be aimed.\n" );
        return false;
    }
    // Drive from the cursor-Z delta so a press without movement is a no-op.
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

// Oblique near plane in this tree's row-vector, D3D 0..w convention.
// MatrixForViewer (com_math.cpp:3080) gives v=(r,u,f,1), and clip=v*P, so D3D's
// clip.z >= 0 near test is projection column 2 rather than GL's row3+row4 form.
// For world z <= level, substitute p=o+r*vright+u*vup+f*vpn to obtain:
//   C = (-vright.z, -vup.z, -vpn.z, level-o.z), tested C.v >= 0.
// Replacing column 2 with a*C for positive a installs the cut; a must also preserve
// clip.z <= clip.w and monotonic depth along each pixel ray.
//
// Perspective: K=|Cr|*tanX+|Cu|*tanY+Cf and
//   a=zNear/(max(K,0)*zNear+max(Cw,0)).
// A nonpositive denominator needs no far-bound scale, so any positive a works.
// Since dz_ndc/df=-a*Cw/f^2, D3D LESS ordering requires Cw<0 (eye above cut).
//
// Ortho: M=|Cr|*halfW+|Cu|*halfH+|Cf|*depthHalf+Cw is max(C.v), so
//   a=guardC/M preserves the base projection's 2047/2048 far guard band.
// Its depth slope is a*Cf, requiring Cf>0 (camera looking downward).
// Refused folds also disable the pick clamp. Replacing the near plane admits the
// near-eye region, but useful cuts remove it; behind-eye geometry still has w<0.
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

// Compatibility no-ops preserve camwnd.cpp's matched pair; the tested D3D9 clip
// state was accepted but did not cut under the programmable vertex path.
void KiwiSection_EmitClipBegin() {}
void KiwiSection_EmitClipEnd()   {}

// Render.
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

    // Screen-constant square makes the otherwise invisible cut plane readable.
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

    // The ring lies in the cut plane.
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

    // Stem and ball.
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

// Picking follows KiwiSection_Cutting(), never merely the armed mode.
void KiwiSection_ClampRayStart( float *start, const float *dir )
{
    if ( !KiwiSection_Cutting() || !start || !dir )
        return;
    // Only rays beginning in the hidden half need clamping.
    const float sd = start[2] - s_level;
    if ( sd <= 0.0f )
        return;
    if ( dir[2] >= -1.0e-6f )
        return;                             // This ray cannot reach visible geometry.
    const float t = -sd / dir[2];           // > 0 by the two tests above
    Mad3( start, dir, t, start );
}

bool KiwiSection_PointVisible( const float *p )
{
    if ( !KiwiSection_Cutting() || !p )
        return true;
    return p[2] <= s_level;
}

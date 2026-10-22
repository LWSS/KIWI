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
#include "kiwi_ported.h"
#include "kiwi_selection.h"
#include "kiwi_ux.h"
#include "kiwi_units.h"
#include "kiwi_vec.h"       // Dot3/Sub3/Mad3/...
#include "kiwi_windows.h"   // KiwiWindows_SyncViewMenu - the View menu check marks
#include "prefs.h"          // g_PrefsDlg->camera_fov (the edge pass's frustum)
#include "radiant_frame.h"  // Radiant_RegisterCommand
#include "radiant_registry.h"   // Radiant_Profile* - the view-mode toggles persist
#include <gfx_d3d/r_rendercmds.h>  // R_AddCmd_Line3D / R_AddCmd_Line3DNoDepth, GfxPointVertex

#include <math.h>
#include <algorithm>
#include <unordered_map>
#include <vector>

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
            const float th = ( (float)( 2.0 * KIWI_PI ) * (float)( i % segs ) ) / (float)segs;
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
            const float th0 = ( (float)( 2.0 * KIWI_PI ) * (float)i ) / (float)segs;
            const float th1 = th0 + (float)KIWI_PI;
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
    // The two view modes beside it: unbound too (view-cube chips, palette, View menu).
    Radiant_RegisterCommand( "KiwiViewEdgeOutline", 0, 0, KIWI_CMD_VIEW_EDGE_OUTLINE );
    Radiant_RegisterCommand( "KiwiViewXRay",        0, 0, KIWI_CMD_VIEW_XRAY );
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
    if ( cmdId == (unsigned)KIWI_CMD_VIEW_EDGE_OUTLINE )
    {
        KiwiView_SetEdgeOutline( !KiwiView_EdgeOutline() );
        Sys_Printf( "Edge outlines: %s.\n", KiwiView_EdgeOutline() ? "ON" : "OFF" );
        return true;
    }
    if ( cmdId == (unsigned)KIWI_CMD_VIEW_XRAY )
    {
        KiwiView_SetXRay( !KiwiView_XRay() );
        Sys_Printf( KiwiView_XRay()
                    ? "X-ray: ON - edges draw through walls and every snap point is offered.\n"
                    : "X-ray: OFF - snapping offers only points the camera can see.\n" );
        return true;
    }
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
            const float th = ( (float)( 2.0 * KIWI_PI ) * (float)( i % KSEC_RING_SEGS ) )
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

// ═════════════════════════════════════════════════════════════════════════════
//  CAMERA VIEW MODES - EDGE OUTLINE + X-RAY (RADIANT_UX_DESIGN D-EDGEOUTLINE / D-XRAY)
// ═════════════════════════════════════════════════════════════════════════════
// User (2026-10-21): "a new visualization mode enabled by a button next to the O/P camera
// that adds an outline shader to all edges (Similar to plasticity).  This is NOT x-ray
// mode, just show them for the visible edges or near-visible."  And: "x-ray mode, I want
// this too ... the snapping points will only snap to points visible unless x-ray is
// turned on, then anything."
//
// Both draw from the brush data itself (face windings, patch render grids), not from the
// render triangulation, through the editor's own 3D line command.  `$line` tests depth
// LESSEQUAL and writes it; `$line_nodepth` skips the test (and still writes).  Neither
// blends, which decides the x-ray look - see KiwiView_DrawEdges.
namespace
{
    int s_edgeOutline = -1;              // -1 = not read from the profile yet
    int s_xray        = -1;

    const char *KVIEW_PROFILE = "KiwiUX";

    // $line's MATERIAL_COLOR .w is a flat override (r_rendercmds.cpp Ed_EmitLineBatch), so
    // these are the colours on screen.
    const float KEDGE_COL_DARK[3] = { 0.04f, 0.04f, 0.05f };   // visible edges
    const float KEDGE_COL_XRAY[3] = { 0.55f, 0.80f, 1.00f };   // the through-wall pass

    // Depth pull.  An endpoint moves toward the eye ALONG ITS OWN VIEW RAY - same pixel,
    // nearer depth - by this fraction of its distance plus a floor in WORLD UNITS.  The
    // perspective projection has an infinite far plane over znear 4, so a 24-bit depth step
    // is ~0.25 unit at 4000 and ~4 units at 16000; 0.3% of the distance stays several steps
    // ahead of that, and of the sub-pixel slope of a face seen at a grazing angle.  It is
    // also the "near-visible" slack: an edge less than that far behind a surface still shows.
    const float KEDGE_PULL_FRAC     = 0.003f;
    const float KEDGE_PULL_MIN      = 0.25f;
    const float KEDGE_PULL_MAX_FRAC = 0.25f;
    // Ortho depth is linear (1/16 unit per step): a constant pull worth two pixels.
    const float KEDGE_PULL_ORTHO_PX = 2.0f;
    // A brush smaller than this on screen would be a smudge of lines; it is skipped.
    const float KEDGE_MIN_PX        = 3.0f;
    // Per-frame segment cap (32 bytes each in the 48 MB editor command buffer).
    const int   KEDGE_MAX_SEGS      = 262144;
    // One RC_DRAW_LINES command holds at most 0xFFFF bytes, i.e. 2047 segments.
    enum { KEDGE_BATCH = 2000 };
    // Coincident-edge matching between brushes: endpoints quantised to 1/8 WORLD UNIT.
    const float KEDGE_WELD_Q        = 8.0f;
    // Within one brush, two windings share an edge when both ends agree this closely.
    const float KEDGE_SAME_PT       = 0.01f;
    // Brush face normals: the same plane.  Patch normals: a smooth continuation (~14 deg).
    const float KEDGE_COPLANAR      = 0.9995f;
    const float KEDGE_SMOOTH        = 0.97f;
    // Milliseconds between seam passes while the scene keeps changing (a drag).
    const unsigned KEDGE_SEAM_THROTTLE_MS = 400;

    struct kEdge_t
    {
        float a[3], b[3];
        float n1[3], n2[3];      // the faces it leaves (n2 == n1 unless `two`)
        bool  two;               // shared by two windings of the brush
        bool  patch;             // a patch border: normals are surface normals
    };

    // One brush or patch def.  The key fields are compared, never dereferenced, so an entry
    // that outlives its def is harmless until the next seam pass evicts it.
    struct kEdgeEntry_t
    {
        bool        built     = false;
        short       version   = 0;
        int         faceCount = 0;
        const void *faces     = nullptr;
        const void *w0        = nullptr;
        float       mins[3]   = { 0.0f, 0.0f, 0.0f };
        float       maxs[3]   = { 0.0f, 0.0f, 0.0f };
        const void *curve     = nullptr;
        short       pmVersion = 0;
        int         cw = 0, ch = 0;
        unsigned    seamGen   = 0;       // == s_seamGen: `seam` describes `edges`
        std::vector<kEdge_t>       edges;
        std::vector<unsigned char> seam; // 1 = merged away (a coplanar seam)
    };

    std::unordered_map<const brush_t *, kEdgeEntry_t> s_edges;
    unsigned           s_seamGen     = 1;
    unsigned long long s_seamSig     = 0;
    bool               s_seamPending = true;
    unsigned           s_seamTick    = 0;
    bool               s_seamEver    = false;

    GfxPointVertex s_ev[KEDGE_BATCH * 2];
    int            s_evCount = 0;

    void FillKey( kEdgeEntry_t &e, const brush_t *def )
    {
        e.version   = def->version;
        e.faceCount = def->faceCount;
        e.faces     = def->faces;
        e.w0        = ( def->faces && def->faceCount > 0 ) ? (const void *)def->faces[0].w : nullptr;
        Copy3( def->mins, e.mins );
        Copy3( def->maxs, e.maxs );
        const patchMesh_t *pm = def->patch;
        const curvePatchDef_t *g = pm ? pm->curveDef : nullptr;
        e.curve     = g;
        e.pmVersion = pm ? pm->version : 0;
        e.cw        = g ? g->width  : 0;
        e.ch        = g ? g->height : 0;
    }

    // def->version moves on every geometry edit (Brush_Build, Brush_RebuildAfterEdit, the
    // terrain tools); the rest catches a rebuilt winding set or a re-tessellated patch.
    bool KeyMatches( const kEdgeEntry_t &e, const brush_t *def )
    {
        if ( !e.built || e.version != def->version || e.faceCount != def->faceCount
          || e.faces != (const void *)def->faces )
            return false;
        const void *w0 = ( def->faces && def->faceCount > 0 ) ? (const void *)def->faces[0].w : nullptr;
        if ( e.w0 != w0 )
            return false;
        for ( int k = 0; k < 3; ++k )
            if ( e.mins[k] != def->mins[k] || e.maxs[k] != def->maxs[k] )
                return false;
        const patchMesh_t *pm = def->patch;
        const curvePatchDef_t *g = pm ? pm->curveDef : nullptr;
        return e.curve == (const void *)g
            && e.pmVersion == ( pm ? pm->version : 0 )
            && e.cw == ( g ? g->width : 0 ) && e.ch == ( g ? g->height : 0 );
    }

    inline bool SamePoint( const float *a, const float *b )
    {
        return fabsf( a[0] - b[0] ) <= KEDGE_SAME_PT
            && fabsf( a[1] - b[1] ) <= KEDGE_SAME_PT
            && fabsf( a[2] - b[2] ) <= KEDGE_SAME_PT;
    }

    // Each brush edge appears in the two windings it separates; keep it once, with both
    // face normals.  Quadratic in the brush's edge count, once per edit of that brush.
    void AddWindingEdge( std::vector<kEdge_t> &out, const float *a, const float *b, const float *n )
    {
        float d[3];
        Sub3( b, a, d );
        if ( Dot3( d, d ) < 1.0e-4f )
            return;                                  // degenerate sliver edge
        for ( kEdge_t &e : out )
        {
            if ( e.two )
                continue;
            if ( ( SamePoint( e.a, b ) && SamePoint( e.b, a ) )
              || ( SamePoint( e.a, a ) && SamePoint( e.b, b ) ) )
            {
                Copy3( n, e.n2 );
                e.two = true;
                return;
            }
        }
        kEdge_t e;
        Copy3( a, e.a );
        Copy3( b, e.b );
        Copy3( n, e.n1 );
        Copy3( n, e.n2 );
        e.two   = false;
        e.patch = false;
        out.push_back( e );
    }

    void AddPatchEdge( std::vector<kEdge_t> &out, const curveVert_t &va, const curveVert_t &vb )
    {
        float d[3];
        Sub3( vb.xyz, va.xyz, d );
        if ( Dot3( d, d ) < 1.0e-4f )
            return;
        kEdge_t e;
        Copy3( va.xyz, e.a );
        Copy3( vb.xyz, e.b );
        for ( int k = 0; k < 3; ++k )
            e.n1[k] = va.normal[k] + vb.normal[k];
        if ( !Norm3( e.n1 ) )
        {
            e.n1[0] = 0.0f; e.n1[1] = 0.0f; e.n1[2] = 1.0f;
        }
        Copy3( e.n1, e.n2 );
        e.two   = false;
        e.patch = true;
        out.push_back( e );
    }

    void BuildEdges( kEdgeEntry_t &e, const brush_t *def )
    {
        e.edges.clear();
        e.seam.clear();
        e.seamGen = 0;                               // the next seam pass describes it
        const patchMesh_t *pm = def->patch;
        if ( pm )
        {
            // Plasticity shows a surface's boundary, not its tessellation: the four border
            // rows/columns of the render grid (verts[y * width + x]).
            const curvePatchDef_t *g = pm->curveDef;
            if ( g && g->verts && g->width > 1 && g->height > 1 )
            {
                const int w = g->width, h = g->height;
                for ( int x = 0; x + 1 < w; ++x )
                {
                    AddPatchEdge( e.edges, g->verts[x], g->verts[x + 1] );
                    AddPatchEdge( e.edges, g->verts[( h - 1 ) * w + x], g->verts[( h - 1 ) * w + x + 1] );
                }
                for ( int y = 0; y + 1 < h; ++y )
                {
                    AddPatchEdge( e.edges, g->verts[y * w], g->verts[( y + 1 ) * w] );
                    AddPatchEdge( e.edges, g->verts[y * w + w - 1], g->verts[( y + 1 ) * w + w - 1] );
                }
            }
        }
        else if ( def->faces )
        {
            for ( int f = 0; f < def->faceCount; ++f )
            {
                const winding_t *w = def->faces[f].w;
                if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
                    continue;
                const float *n = def->faces[f].plane.normal;
                for ( int i = 0; i < w->numpoints; ++i )
                    AddWindingEdge( e.edges, w->p[i], w->p[( i + 1 ) % w->numpoints], n );
            }
            // A seam between two coplanar faces of one brush (duplicate planes) is no edge.
            size_t keep = 0;
            for ( size_t i = 0; i < e.edges.size(); ++i )
                if ( !( e.edges[i].two && Dot3( e.edges[i].n1, e.edges[i].n2 ) > KEDGE_COPLANAR ) )
                    e.edges[keep++] = e.edges[i];
            e.edges.resize( keep );
        }
        FillKey( e, def );
        e.built = true;
    }

    kEdgeEntry_t *EdgeEntry( const brush_t *def )
    {
        kEdgeEntry_t &e = s_edges[def];
        if ( !KeyMatches( e, def ) )
            BuildEdges( e, def );
        return &e;
    }

    // What the camera draws as brush geometry: unfiltered world and brush-entity brushes,
    // and patches.  Point entities (lights, spawns, models, prefabs) are boxes or meshes,
    // not edges.  Hidden geometry is filtered (FilterBrush folds brushFlags & 4 in).
    bool EdgeDrawable( selbrush_t *b )
    {
        if ( !b || !b->def || FilterBrush( b, 0 ) )
            return false;
        entity_s *owner = b->owner;
        if ( !owner )
            return false;
        if ( owner != world_entity )
        {
            const entity_s *od = owner->def;
            if ( !od || !od->eclass || od->eclass->fixedsize
              || ( od->eclass->classtype & ECLASS_PREFAB ) != 0 )
                return false;
        }
        const brush_t *def = b->def;
        if ( def->patch )
        {
            const curvePatchDef_t *g = def->patch->curveDef;
            return g && g->verts && g->width > 1 && g->height > 1;
        }
        return def->faces && def->faceCount > 0;
    }

    // Order-independent (selection moves nodes between lists without changing this), so
    // only an edit, a create/delete, or a hide/filter change asks for a new seam pass.
    unsigned long long SceneSignature()
    {
        unsigned long long sig = 0;
        selbrush_t *lists[] = { &active_brushes, &selected_brushes };
        for ( selbrush_t *head : lists )
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( !EdgeDrawable( b ) )
                continue;
            const brush_t *def = b->def;
            unsigned long long h = (unsigned long long)(uintptr_t)def;
            h ^= (unsigned long long)(unsigned short)def->version << 48;
            if ( def->patch )
                h ^= (unsigned long long)(unsigned short)def->patch->version << 32;
            h *= 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
            sig += h;
        }
        return sig;
    }

    struct kSeamRec_t
    {
        int           q[6];          // quantised endpoints, lower end first
        kEdgeEntry_t *e;
        int           idx;
    };

    bool SeamLess( const kSeamRec_t &x, const kSeamRec_t &y )
    {
        for ( int k = 0; k < 6; ++k )
            if ( x.q[k] != y.q[k] )
                return x.q[k] < y.q[k];
        return false;
    }

    // A run of coincident edges from different brushes/patches is a SEAM when the faces it
    // leaves behind, after cancelling buried face pairs, are all one plane.  Two floor tiles:
    // (up, +x) and (up, -x) -> the touching sides cancel -> {up, up} -> seam.  Two stacked
    // boxes: (up, +x) and (down, +x) -> {+x, +x} -> seam.  A box on a floor at a matching
    // edge keeps its line, because nothing cancels.  Two planes through one line with
    // opposite normals ARE one plane, so the normal test is the whole coplanarity test.
    bool SeamRun( const kSeamRec_t *r, int n )
    {
        enum { KMAX = 32 };
        float N[KMAX][3];
        bool  used[KMAX];
        int   c = 0;
        bool  anyPatch = false;
        for ( int i = 0; i < n; ++i )
        {
            const kEdge_t &e = r[i].e->edges[r[i].idx];
            anyPatch = anyPatch || e.patch;
            if ( c + 2 > KMAX )
                return false;                        // a pathological pile-up: keep the line
            Copy3( e.n1, N[c] ); used[c] = false; ++c;
            if ( e.two )
            {
                Copy3( e.n2, N[c] ); used[c] = false; ++c;
            }
        }
        const float thr = anyPatch ? KEDGE_SMOOTH : KEDGE_COPLANAR;
        for ( int i = 0; i < c; ++i )
        {
            if ( used[i] )
                continue;
            for ( int j = i + 1; j < c; ++j )
                if ( !used[j] && Dot3( N[i], N[j] ) < -thr )
                {
                    used[i] = used[j] = true;
                    break;
                }
        }
        int first = -1;
        for ( int i = 0; i < c; ++i )
        {
            if ( used[i] )
                continue;
            if ( first < 0 )
                first = i;
            else if ( Dot3( N[first], N[i] ) < thr )
                return false;
        }
        return true;                                 // one plane left (or all buried)
    }

    inline int Quant( float v )
    {
        return (int)floorf( v * KEDGE_WELD_Q + 0.5f );
    }

    // Every drawable brush and patch takes part (an off-screen neighbour still merges a
    // seam), so this also refreshes the whole cache.  Entries it does not reach are no
    // longer drawable (deleted, hidden, filtered) and are evicted.
    void SeamPass()
    {
        ++s_seamGen;
        if ( s_seamGen == 0 )
            s_seamGen = 1;
        static std::vector<kSeamRec_t> recs;
        recs.clear();
        selbrush_t *lists[] = { &active_brushes, &selected_brushes };
        for ( selbrush_t *head : lists )
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( !EdgeDrawable( b ) )
                continue;
            kEdgeEntry_t *e = EdgeEntry( b->def );
            if ( e->seamGen == s_seamGen )
                continue;                            // one def on two nodes
            e->seamGen = s_seamGen;
            e->seam.assign( e->edges.size(), 0 );
            for ( size_t i = 0; i < e->edges.size(); ++i )
            {
                const kEdge_t &ed = e->edges[i];
                kSeamRec_t r;
                int qa[3], qb[3];
                for ( int k = 0; k < 3; ++k )
                {
                    qa[k] = Quant( ed.a[k] );
                    qb[k] = Quant( ed.b[k] );
                }
                const bool aFirst = qa[0] != qb[0] ? qa[0] < qb[0]
                                  : qa[1] != qb[1] ? qa[1] < qb[1] : qa[2] <= qb[2];
                for ( int k = 0; k < 3; ++k )
                {
                    r.q[k]     = aFirst ? qa[k] : qb[k];
                    r.q[k + 3] = aFirst ? qb[k] : qa[k];
                }
                r.e   = e;
                r.idx = (int)i;
                recs.push_back( r );
            }
        }
        std::sort( recs.begin(), recs.end(), SeamLess );
        for ( size_t i = 0; i < recs.size(); )
        {
            size_t j = i + 1;
            while ( j < recs.size() && !SeamLess( recs[i], recs[j] ) )
                ++j;
            if ( j - i >= 2 && SeamRun( &recs[i], (int)( j - i ) ) )
                for ( size_t k = i; k < j; ++k )
                    recs[k].e->seam[recs[k].idx] = 1;
            i = j;
        }
        for ( auto it = s_edges.begin(); it != s_edges.end(); )
        {
            if ( it->second.seamGen != s_seamGen )
                it = s_edges.erase( it );
            else
                ++it;
        }
    }

    struct kView_t
    {
        bool  ortho;
        float eye[3], fwd[3], right[3], up[3];
        float tanX, tanY;            // perspective half-angle tangents
        float halfW, halfH;          // ortho half extents, WORLD UNITS
        float wpp;                   // ortho WORLD UNITS per pixel
        float heightPx;
    };

    bool ViewSetup( kView_t &v )
    {
        const camera_s *c = Ed_Camera();
        if ( c->width < 1 || c->height < 1 )
            return false;
        v.ortho = KiwiCam_Ortho();
        Copy3( c->origin, v.eye );
        Copy3( c->vpn,    v.fwd );
        Copy3( c->vright, v.right );
        Copy3( c->vup,    v.up );
        v.heightPx = (float)c->height;
        // CamWnd_SetupScene's own frustum: tanY = tan(fov/2) * 0.75, tanX by aspect.
        v.tanY  = tanf( (float)DEG2RAD( g_PrefsDlg->camera_fov ) * 0.5f ) * 0.75f;
        v.tanX  = v.tanY * (float)c->width / (float)c->height;
        v.halfH = KiwiCam_OrthoHalfHeight();
        v.halfW = v.halfH * (float)c->width / (float)c->height;
        v.wpp   = 2.0f * v.halfH / v.heightPx;
        return v.tanY > 1.0e-6f && ( !v.ortho || v.halfH > 0.0f );
    }

    // Conservative sphere-vs-frustum plus the on-screen size floor.
    bool BoxInView( const kView_t &v, const float *mins, const float *maxs, float *outNear )
    {
        float c[3], ext[3];
        for ( int k = 0; k < 3; ++k )
        {
            c[k]   = ( mins[k] + maxs[k] ) * 0.5f;
            ext[k] = ( maxs[k] - mins[k] ) * 0.5f;
        }
        const float r = sqrtf( Dot3( ext, ext ) );
        float d[3];
        Sub3( c, v.eye, d );
        const float x = fabsf( Dot3( d, v.right ) );
        const float y = fabsf( Dot3( d, v.up ) );
        if ( v.ortho )
        {
            if ( x - r > v.halfW || y - r > v.halfH )
                return false;
            if ( 2.0f * r < KEDGE_MIN_PX * v.wpp )
                return false;
            *outNear = 0.0f;
            return true;
        }
        const float z = Dot3( d, v.fwd );
        if ( z < -r )
            return false;                            // wholly behind the eye
        if ( x - r * sqrtf( 1.0f + v.tanX * v.tanX ) > z * v.tanX
          || y - r * sqrtf( 1.0f + v.tanY * v.tanY ) > z * v.tanY )
            return false;
        if ( z > r && r * v.heightPx < KEDGE_MIN_PX * z * v.tanY )
            return false;                            // a few pixels across: noise, not edges
        const float dist = sqrtf( Dot3( d, d ) ) - r;
        *outNear = dist > 1.0f ? dist : 1.0f;
        return true;
    }

    void EdgeFlush( bool depthTest, int width )
    {
        if ( s_evCount >= 2 )
        {
            if ( depthTest )
                R_AddCmd_Line3D( (short)( s_evCount / 2 ), (char)width, s_ev );
            else
                R_AddCmd_Line3DNoDepth( (short)( s_evCount / 2 ), (char)width, s_ev );
        }
        s_evCount = 0;
    }

    inline void EdgeEmit( const float *a, const float *b, unsigned packed, bool depthTest, int width )
    {
        if ( s_evCount + 2 > KEDGE_BATCH * 2 )
            EdgeFlush( depthTest, width );
        GfxPointVertex *v = &s_ev[s_evCount];
        Copy3( a, v[0].xyz );
        Copy3( b, v[1].xyz );
        *(unsigned *)v[0].color = packed;
        *(unsigned *)v[1].color = packed;
        s_evCount += 2;
    }

    // One pass over the unselected brushes and patches (the selected ones keep the white
    // outline the camera draws after its depth clear).  Returns the segments emitted.
    int EdgePass( const kView_t &v, bool depthTest, int width, const float *rgb, int budget )
    {
        float rgba[4] = { rgb[0], rgb[1], rgb[2], 1.0f };
        GfxColor col;
        Byte4PackPixelColor( rgba, &col );
        const unsigned packed = col.packed;

        int segs = 0;
        s_evCount = 0;
        for ( selbrush_t *b = active_brushes.next; b && b != &active_brushes && segs < budget; b = b->next )
        {
            if ( !EdgeDrawable( b ) )
                continue;
            const brush_t *def = b->def;
            float nearDist = 0.0f;
            if ( !BoxInView( v, def->mins, def->maxs, &nearDist ) )
                continue;
            const kEdgeEntry_t *e = EdgeEntry( def );
            const bool seamOk = e->seamGen == s_seamGen && e->seam.size() == e->edges.size();

            // The pull (see KEDGE_PULL_*): perspective along each point's eye ray, ortho
            // along -vpn.  Neither moves the point on screen.
            float k = 0.0f;
            float off[3] = { 0.0f, 0.0f, 0.0f };
            if ( depthTest )
            {
                if ( v.ortho )
                    for ( int i = 0; i < 3; ++i )
                        off[i] = -v.fwd[i] * ( KEDGE_PULL_ORTHO_PX * v.wpp + KEDGE_PULL_MIN );
                else
                {
                    k = KEDGE_PULL_FRAC + KEDGE_PULL_MIN / nearDist;
                    if ( k > KEDGE_PULL_MAX_FRAC )
                        k = KEDGE_PULL_MAX_FRAC;
                }
            }
            const int n = (int)e->edges.size();
            for ( int i = 0; i < n && segs < budget; ++i )
            {
                if ( seamOk && e->seam[i] )
                    continue;
                const kEdge_t &ed = e->edges[i];
                float pa[3], pb[3];
                for ( int c = 0; c < 3; ++c )
                {
                    pa[c] = ed.a[c] + ( v.eye[c] - ed.a[c] ) * k + off[c];
                    pb[c] = ed.b[c] + ( v.eye[c] - ed.b[c] ) * k + off[c];
                }
                EdgeEmit( pa, pb, packed, depthTest, width );
                ++segs;
            }
        }
        EdgeFlush( depthTest, width );
        return segs;
    }

    void ViewRepaint()
    {
        g_nUpdateBits |= 1;
    }
}

bool KiwiView_EdgeOutline()
{
    if ( s_edgeOutline < 0 )
        s_edgeOutline = Radiant_ProfileGetInt( KVIEW_PROFILE, "EdgeOutline", 0 ) ? 1 : 0;
    return s_edgeOutline != 0;
}

void KiwiView_SetEdgeOutline( bool on )
{
    const int v = on ? 1 : 0;
    if ( KiwiView_EdgeOutline() == on )
        return;
    s_edgeOutline = v;
    Radiant_ProfileSetInt( KVIEW_PROFILE, "EdgeOutline", v );
    KiwiWindows_SyncViewMenu();
    ViewRepaint();
}

bool KiwiView_XRay()
{
    if ( s_xray < 0 )
        s_xray = Radiant_ProfileGetInt( KVIEW_PROFILE, "XRay", 0 ) ? 1 : 0;
    return s_xray != 0;
}

void KiwiView_SetXRay( bool on )
{
    const int v = on ? 1 : 0;
    if ( KiwiView_XRay() == on )
        return;
    s_xray = v;
    Radiant_ProfileSetInt( KVIEW_PROFILE, "XRay", v );
    KiwiWindows_SyncViewMenu();
    ViewRepaint();
}

// Two passes at most, both line commands between the world passes and the selected-outline
// depth clear (camwnd.cpp):
//   1. VISIBLE: $line (depth LESSEQUAL), endpoints pulled toward the eye, dark ink.
//   2. X-RAY:   $line_nodepth, light ink, every edge.
// Neither line material blends and $line_nodepth still WRITES depth, so the hidden-edge pass
// cannot be told to draw only where pass 1 did not: whichever runs second covers the other,
// and running x-ray first would stamp its own (deeper) depth over the walls and let pass 1
// through everywhere.  So with x-ray on pass 1 is drawn 3 px wide and pass 2 1 px over it:
// a visible edge reads light with a dark rim, a hidden edge light alone.
void KiwiView_DrawEdges()
{
    const bool xray  = KiwiView_XRay();
    const bool edges = KiwiView_EdgeOutline();
    if ( !xray && !edges )
        return;
    kView_t v;
    if ( !ViewSetup( v ) )
        return;

    // Seams: re-merge when the scene changes, at most every KEDGE_SEAM_THROTTLE_MS during a
    // drag; a repaint is requested so the last change of a drag still gets its pass.
    const unsigned long long sig = SceneSignature();
    if ( sig != s_seamSig )
    {
        s_seamSig     = sig;
        s_seamPending = true;
    }
    if ( s_seamPending )
    {
        const unsigned now = (unsigned)::GetTickCount();
        if ( !s_seamEver || now - s_seamTick >= KEDGE_SEAM_THROTTLE_MS )
        {
            SeamPass();
            s_seamPending = false;
            s_seamEver    = true;
            s_seamTick    = now;
        }
        else
            ViewRepaint();
    }

    int budget = KEDGE_MAX_SEGS;
    budget -= EdgePass( v, true, xray ? 3 : 1, KEDGE_COL_DARK, budget );
    if ( xray && budget > 0 )
        EdgePass( v, false, 1, KEDGE_COL_XRAY, budget );
    // The selected-outline pass (and its model wireframe surfs) inherit MATERIAL_COLOR.
    KiwiTris_FillNeutral();
}

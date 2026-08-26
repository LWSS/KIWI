#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Presentation and input for the active command's one-axis handle; it never
// mutates geometry itself.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s

#include "kiwi_lollipop.h"
#include "kiwi_camera.h"
#include "kiwi_command.h"
#include "kiwi_lines.h"
#include "kiwi_pick.h"
#include "kiwi_ux.h"
#include "kiwi_vec.h"     // vector helpers

#include <math.h>

// Ported entry points: CamWnd_BuildMatrix 0x403470; g_nUpdateBits 0x25d5a74.
extern camera_s *Ed_Camera();
extern void      CamWnd_BuildMatrix();
extern int       g_nUpdateBits;

namespace
{
    // Screen-constant marker: face-plane ring, short stem, and solid endpoint.
    // The invisible pick radius stays larger than the drawn ball for usability.
    const float KLOL_RING_PIX  = 14.0f;
    const float KLOL_STEM_PIX  = 42.0f;
    const float KLOL_BALL_PIX  = 5.0f;      // 10 px diameter
    const float KLOL_PICK_PIX  = 12.0f;
    const int   KLOL_RING_SEGS = 20;        // 20 chords read as round at 14 px
    const int   KLOL_BALL_SEGS = 12;

    // The dim ring is context; the brighter ball is the interactive target.
    const float KLOL_COL_BALL[3] = { 1.00f, 0.90f, 0.30f };
    const float KLOL_COL_HOT [3] = { 1.00f, 0.98f, 0.72f };   // hovered / held
    const float KLOL_COL_RING[3] = { 0.86f, 0.74f, 0.28f };
    // HudInvalid uses the same red as invalid command previews.
    const float KLOL_COL_BAD [3] = { 1.00f, 0.30f, 0.25f };

    bool s_hot     = false;
    bool s_grabbed = false;


    struct lolGeo_t
    {
        float anchor[3];        // the LIVE face / region centroid
        float dir[3];           // unit, sign already folded in by the command
        float ring;             // world radius
        float stem;             // world length
        float ball;             // world radius
        float tip[3];           // anchor + dir * stem
        float e0[3], e1[3];     // an orthonormal pair spanning the face plane
    };

    // The least-aligned world axis is a nondegenerate seed perpendicular to unit n.
    void PerpBasis( const float n[3], float e0[3], float e1[3] )
    {
        int least = 0;
        for ( int k = 1; k < 3; ++k )
            if ( fabsf( n[k] ) < fabsf( n[least] ) )
                least = k;
        float seed[3] = { 0.0f, 0.0f, 0.0f };
        seed[least] = 1.0f;
        const float d = Dot3( n, seed );
        for ( int k = 0; k < 3; ++k )
            e0[k] = seed[k] - n[k] * d;
        float l = sqrtf( Dot3( e0, e0 ) );
        if ( l < 1.0e-5f )
        {
            e0[0] = 1.0f; e0[1] = 0.0f; e0[2] = 0.0f;
            l = 1.0f;
        }
        for ( int k = 0; k < 3; ++k )
            e0[k] /= l;
        e1[0] = n[1]*e0[2] - n[2]*e0[1];
        e1[1] = n[2]*e0[0] - n[0]*e0[2];
        e1[2] = n[0]*e0[1] - n[1]*e0[0];
    }

    // Builds screen-constant world geometry; false when disabled or unavailable.
    bool BuildGeo( lolGeo_t *g )
    {
        if ( !KiwiUX_ModernInput() )
            return false;
        const camera_s *c = Ed_Camera();
        if ( c->width < 1 || c->height < 1 )
            return false;
        if ( !KiwiLollipop_Wanted( g->anchor, g->dir ) )
            return false;

        const float wpp = KiwiCam_WorldPerPixel( g->anchor );
        g->ring = KLOL_RING_PIX * wpp;
        g->stem = KLOL_STEM_PIX * wpp;
        g->ball = KLOL_BALL_PIX * wpp;
        Mad3( g->anchor, g->dir, g->stem, g->tip );
        PerpBasis( g->dir, g->e0, g->e1 );
        return true;
    }

    bool BallHit( const lolGeo_t &g, int imgX, int imgY )
    {
        float tx, ty;
        if ( !Pick_WorldToImage( g.tip, &tx, &ty ) )
            return false;                       // behind the eye
        const float dx = tx - (float)imgX;
        const float dy = ty - (float)imgY;
        return sqrtf( dx * dx + dy * dy ) <= KLOL_PICK_PIX;
    }

    // Camera-facing outline; solid adds diameters because the line renderer has no fill.
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
}

float KiwiLollipop_FaceSide( const float anchor[3], const float normal[3], float push )
{
    if ( push < 0.0f ) return -1.0f;         // travelling: keep the ride/flip rule
    if ( push > 0.0f ) return  1.0f;
    return KiwiCam_FacingSign( anchor, normal );   // at rest: the camera picks
}

bool KiwiLollipop_Wanted( float outAnchor[3], float outDir[3] )
{
    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd )
        return false;
    float a[3] = { 0.0f, 0.0f, 0.0f };
    float d[3] = { 0.0f, 0.0f, 1.0f };
    if ( !cmd->LollipopHandle( a, d ) )
        return false;
    // A zero direction is a command that answered true without a usable normal —
    // refuse rather than draw a stem of length zero the user cannot grab.
    const float l = sqrtf( d[0]*d[0] + d[1]*d[1] + d[2]*d[2] );
    if ( l < 1.0e-4f )
        return false;
    if ( outAnchor )
        Copy3( a, outAnchor );
    if ( outDir )
        for ( int k = 0; k < 3; ++k )
            outDir[k] = d[k] / l;
    return true;
}

bool KiwiLollipop_Active()
{
    return KiwiLollipop_Wanted( 0, 0 );
}

void KiwiLollipop_Hover( int imgX, int imgY, bool over )
{
    const bool was = s_hot;
    s_hot = false;
    if ( over && !s_grabbed )
    {
        CamWnd_BuildMatrix();
        lolGeo_t g;
        if ( BuildGeo( &g ) )
            s_hot = BallHit( g, imgX, imgY );
    }
    if ( s_hot != was )
        g_nUpdateBits |= 1;                     // repaint so the brighten shows
}

bool KiwiLollipop_MouseDown( int imgX, int imgY )
{
    KiwiEditorCommand *cmd = KiwiCmd_Active();
    if ( !cmd )
        return false;

    CamWnd_BuildMatrix();
    lolGeo_t g;
    if ( !BuildGeo( &g ) )
        return false;
    if ( !BallHit( g, imgX, imgY ) )
        return false;

    // Shared grab path rebases at the press pixel before movement, preventing jumps.
    if ( !KiwiCmd_HandleGrab( imgX, imgY ) )
        return false;
    s_grabbed = true;
    s_hot     = true;
    g_nUpdateBits |= 1;
    return true;
}

void KiwiLollipop_Release()
{
    if ( !s_grabbed )
        return;
    s_grabbed = false;
    KiwiCmd_HandleRelease();                    // close the shared command gate
    // A handle release pauses; it does not commit.
    if ( KiwiCmd_Active() )
        KiwiCmd_Pause();
    g_nUpdateBits |= 1;
}

void KiwiLollipop_Abort()
{
    if ( !s_grabbed )
        return;
    s_grabbed = false;
    KiwiCmd_HandleRelease();                    // close the shared command gate
    if ( KiwiCmd_Active() )
        KiwiCmd_Cancel();
    g_nUpdateBits |= 1;
}

void KiwiLollipop_DrawWorld()
{
    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;
    CamWnd_BuildMatrix();

    lolGeo_t g;
    if ( !BuildGeo( &g ) )
        return;

    // Budget includes slack; ring-first ordering keeps the two colour runs contiguous.
    KiwiLines_Begin( KLOL_RING_SEGS + KLOL_BALL_SEGS * 2 + 8, 2 );

    // The ring lies in the face plane so it reads as attached, not as a floating halo.
    // HudInvalid colors the whole glyph red; mixing red and yellow resembles hover.
    KiwiEditorCommand *lolCmd = KiwiCmd_Active();
    const bool bad = lolCmd && lolCmd->HudInvalid();
    if ( bad )
        KiwiLines_Color( KLOL_COL_BAD[0], KLOL_COL_BAD[1], KLOL_COL_BAD[2] );
    else
        KiwiLines_Color( KLOL_COL_RING[0], KLOL_COL_RING[1], KLOL_COL_RING[2] );
    {
        float prev[3];
        for ( int i = 0; i <= KLOL_RING_SEGS; ++i )
        {
            const float th = ( 6.283185307179586f * (float)( i % KLOL_RING_SEGS ) )
                           / (float)KLOL_RING_SEGS;
            float p[3];
            for ( int k = 0; k < 3; ++k )
                p[k] = g.anchor[k] + g.e0[k] * cosf( th ) * g.ring
                                   + g.e1[k] * sinf( th ) * g.ring;
            if ( i > 0 && !KiwiLines_Add( prev, p ) )
                break;
            Copy3( p, prev );
        }
    }

    const float *col = bad ? KLOL_COL_BAD                                  // invalid overrides hover
                           : ( ( s_hot || s_grabbed ) ? KLOL_COL_HOT : KLOL_COL_BALL );
    KiwiLines_Color( col[0], col[1], col[2] );
    // Starting at 0.55 ring radii clears the ring and leaves a visible gap.
    float base[3];
    Mad3( g.anchor, g.dir, g.ring * 0.55f, base );
    KiwiLines_Add( base, g.tip );
    EmitDisc( c, g.tip, g.ball, KLOL_BALL_SEGS, true );

    KiwiLines_Flush();
}

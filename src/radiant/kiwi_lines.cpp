#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // camera_s
#include <gfx_d3d/r_rendercmds.h>   // R_AddCmd_Line3D, GfxPointVertex, GfxColor
#include "kiwi_lines.h"
#include "kiwi_camera.h"            // KiwiCam_Ortho

// Ported entry points.
extern int   R_Add3DLine( GfxPointVertex *verts, const orientation_t *orient,
                          const float *p1, const float *p2, const unsigned int *color,
                          char width, int vertCount, int maxVertCount );   // draw.cpp 0x40c110
extern char  Byte4PackPixelColor( float *from, GfxColor *out );            // 0x402ac0
extern float world_orient_matrix[4][3];                                    // entity.cpp 0x6DE290
extern camera_s *Ed_Camera();                                              // camwnd.cpp

namespace
{
    // Stage 256 segments per flush; R_Add3DLine auto-flushes on overflow.
    // The caller's budget, not this buffer, caps the full batch.
    enum { KLINES_VERTS = 512 };

    GfxPointVertex s_verts[KLINES_VERTS];
    int            s_vertCount = 0;
    int            s_remaining = 0;
    char           s_width     = 1;
    unsigned int   s_color     = 0xFFFFFFFFu;
}

void KiwiLines_Begin( int maxSegments, int width )
{
    s_vertCount = 0;
    s_remaining = ( maxSegments > 0 ) ? maxSegments : 0;
    s_width     = (char)( ( width > 0 ) ? width : 1 );
    s_color     = 0xFFFFFFFFu;
}

void KiwiLines_Color( float r, float g, float b )
{
    float rgba[4] = { r, g, b, 1.0f };      // line alpha stays opaque; fades use RGB
    GfxColor c;
    Byte4PackPixelColor( rgba, &c );
    s_color = c.packed;
}

bool KiwiLines_Add( const float *a, const float *b )
{
    if ( s_remaining <= 0 || !a || !b )
        return false;
    --s_remaining;
    s_vertCount = R_Add3DLine( s_verts, (const orientation_t *)world_orient_matrix,
                               a, b, &s_color, s_width, s_vertCount, KLINES_VERTS );
    return true;
}

void KiwiLines_Flush()
{
    if ( s_vertCount > 0 )
        R_AddCmd_Line3D( (short)( s_vertCount / 2 ), s_width, s_verts );
    s_vertCount = 0;
    s_remaining = 0;
}

int KiwiLines_Remaining()
{
    return s_remaining;
}

// `.w = 1` selects the flat RGB override used by Ed_EmitLineBatch colour runs.
void KiwiTris_FillFlatColor( const float rgba[4] )
{
    if ( !rgba )
        return;
    const float flat[4] = { rgba[0], rgba[1], rgba[2], 1.0f };
    R_AddCmdSetMaterialColor( flat );
}

void KiwiTris_FillNeutral()
{
    static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    R_AddCmdSetMaterialColor( s_neutral );
}

// white_tools backface-culls, so orient each fill triangle toward the viewer.
void KiwiTris_OrientToEye( const float *xyz, int stride,
                           unsigned short *idx, int idxCount, const float *eye )
{
    if ( !xyz || !idx || !eye || stride < 3 || idxCount < 3 )
        return;

    // Parallel projection winding uses n·vpn, not camera position. `dir` points
    // toward the viewer, matching `eye - a` in the perspective arm.
    const bool ortho = KiwiCam_Ortho();                 // direction test in parallel projection
    float dir[3] = { 0.0f, 0.0f, 0.0f };
    if ( ortho )
    {
        const camera_s *cam = Ed_Camera();              // editor camera is never null
        dir[0] = -cam->vpn[0];
        dir[1] = -cam->vpn[1];
        dir[2] = -cam->vpn[2];
    }

    for ( int t = 0; t + 2 < idxCount; t += 3 )
    {
        const float *a = xyz + (size_t)idx[t + 0] * (size_t)stride;
        const float *b = xyz + (size_t)idx[t + 1] * (size_t)stride;
        const float *c = xyz + (size_t)idx[t + 2] * (size_t)stride;

        float ab[3], ac[3], n[3];
        for ( int k = 0; k < 3; ++k ) { ab[k] = b[k] - a[k]; ac[k] = c[k] - a[k]; }
        n[0] = ab[1] * ac[2] - ab[2] * ac[1];
        n[1] = ab[2] * ac[0] - ab[0] * ac[2];
        n[2] = ab[0] * ac[1] - ab[1] * ac[0];

        // A near-zero world-space area has no stable winding to correct.
        if ( n[0] * n[0] + n[1] * n[1] + n[2] * n[2] <= 1e-12f )
            continue;

        const float toEye = ortho
                          ? ( n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2] )
                          : ( n[0] * ( eye[0] - a[0] )
                            + n[1] * ( eye[1] - a[1] )
                            + n[2] * ( eye[2] - a[2] ) );
        if ( toEye < 0.0f )
        {
            const unsigned short tmp = idx[t + 1];
            idx[t + 1] = idx[t + 2];
            idx[t + 2] = tmp;
        }
    }
}

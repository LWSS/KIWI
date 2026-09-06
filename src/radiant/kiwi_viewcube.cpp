#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Neutral view cube with coloured axis stubs; see kiwi_viewcube.h.
// Corners use an orthographic projection in the live camera basis:
//     screen.x = centre.x + dot( v, vright ) * KVC_HALF
//     screen.y = centre.y - dot( v, vup    ) * KVC_HALF     (screen y is down)
//     depth    = dot( v, vpn )                              (+ = away from the eye)
// v is a (±1,±1,±1) corner, so sqrt(3)*KVC_HALF must fit the backdrop radius.
// Faces sort by descending dot(outward normal, vpn): back faces draw first, while
// a negative dot marks a visible, labelled and clickable face.
//
// Face snaps use desired vpn=-normal. With positive pitch looking up,
// pitch=asin(vpn.z) and yaw=atan2(vpn.y,vpn.x). Exact ±90-degree pole snaps are
// intentional; only interactive drags clamp to ±89. Pole yaw is retained and
// quantized because it still rotates the plan view in its own plane.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include <imgui/imgui.h>

#include "kiwi_viewcube.h"
#include "kiwi_camera.h"
#include "kiwi_fmt.h"
#include "kiwi_command.h"            // shared grid stepping and clamp
#include "kiwi_grid.h"               // snap and perspective-grid state
#include "kiwi_numeric.h"            // grid expression parser
#include "kiwi_units.h"              // grid spacing and display units
#include "kiwi_ux.h"                 // KiwiUX_ShowTriCount - View > Show Triangle Count
#include "kiwi_section.h"            // section-analysis state
#include "kiwi_pick.h"               // ray_t + Pick_RayFromImagePos — the cursor-distance readout
#include "kiwi_droptrace.h"          // kiwiDropHit_t + KiwiDrop_Trace — likewise
#include <xanim/xanim.h>             // XSurface (complete type: triCount)
#include <xanim/xmodel.h>            // XModelGetSurfaces — the triangle-count readout
#include "radiant_registry.h"
#include "kiwi_vec.h"     // Dot3

#include <math.h>
#include <stdio.h>                   // _snprintf — the grid readout
#include <stdlib.h>
#include <string.h>                  // strlen — the triangle-count formatter

extern camera_s *Ed_Camera();          // camwnd.cpp
extern void      CamWnd_BuildMatrix(); // camwnd.cpp 0x403470
extern int       Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:118
// File scope on purpose: an extern inside an anonymous-namespace function gets
// internal linkage under MSVC (the cursor-distance readout's mousespace source).
extern bool      ImGuiShell_CameraPaintCursor( int *x, int *y, int *w, int *h );   // imgui_shell.cpp
extern selbrush_t selected_brushes;    // map.cpp — the triangle-count walk
extern selbrush_t active_brushes;      // map.cpp

namespace
{
    const float KVC_BOX    = 104.0f;   // backdrop diameter, pixels
    const float KVC_MARGIN = 10.0f;    // image inset, pixels
    const float KVC_HALF   = 29.0f;    // projected cube half-size, pixels
    const float KVC_CORNER = 8.0f;     // corner grab radius, pixels

    // Fixed slots keep the cluster's position independent of font metrics; their
    // heights and gaps sum exactly to KVC_TOP_STRIP.
    const float KVC_ZOOM_W    = 90.0f;   // bar width, pixels
    const float KVC_ZOOM_H    = 8.0f;
    const float KVC_ZOOM_GAP  = 2.0f;    // bar -> label
    const float KVC_ZOOM_TEXT = 14.0f;   // the label's slot (one default-font line)
    const float KVC_SEC_W     = 104.0f;  // aligns with the cube backdrop
    const float KVC_SEC_H     = 20.0f;   // button height, pixels
    const float KVC_SEC_GAP   = 4.0f;    // label -> button
    const float KVC_STRIP_GAP = 6.0f;    // button -> cube
    // Two text rows under the bar: the zoom / triangle label and the cursor-distance
    // line (the latter used to overlap the SECTION button).
    const float KVC_TOP_STRIP = KVC_ZOOM_H + KVC_ZOOM_GAP + KVC_ZOOM_TEXT + KVC_ZOOM_TEXT
                              + KVC_SEC_GAP + KVC_SEC_H + KVC_STRIP_GAP;

    // Omit the strip when it would push the cube below the image.
    float KVC_StripH( float imgH )
    {
        return ( imgH >= KVC_BOX * 1.5f + KVC_TOP_STRIP ) ? KVC_TOP_STRIP : 0.0f;
    }
    // Shared origin keeps the cube and its pills aligned below the optional strip.
    float KVC_ClusterTop( float imgMinY, float imgH )
    {
        return imgMinY + KVC_MARGIN + KVC_StripH( imgH );
    }
    int s_show = -1;                   // -1 = not read from the profile yet

    // The modal grid editor makes io.WantTextInput gate editor hotkeys; Enter and
    // Escape close its separate top-level window.
    bool  s_gridEdit      = false;
    bool  s_gridEditFocus = false;     // request keyboard focus on the frame it opens
    char  s_gridEditBuf[32] = { 0 };
    float s_gridEditX     = 0.0f;      // where to anchor the popup: the pill's own
    float s_gridEditY     = 0.0f;      // bottom-left, latched when it opens

    // Corner sign bits: bit0=X, bit1=Y, bit2=Z.
    inline void CornerVec( int i, float *out )
    {
        out[0] = ( i & 1 ) ? 1.0f : -1.0f;
        out[1] = ( i & 2 ) ? 1.0f : -1.0f;
        out[2] = ( i & 4 ) ? 1.0f : -1.0f;
    }

    struct face_t2
    {
        int         axis;      // 0 = X, 1 = Y, 2 = Z
        float       sign;      // +1 / -1  (the outward normal is sign * axis)
        const char *label;
    };

    // TOP/BOT avoid requiring the Z-label convention on common plan views.
    const face_t2 KVC_FACES[6] =
    {
        { 0,  1.0f, "X"   },
        { 0, -1.0f, "-X"  },
        { 1,  1.0f, "Y"   },
        { 1, -1.0f, "-Y"  },
        { 2,  1.0f, "TOP" },
        { 2, -1.0f, "BOT" },
    };

    // Axis-stub colours match the world axes; the cube body remains neutral.
    const float KVC_AXIS_RGB[3][3] =
    {
        { 0.84f, 0.31f, 0.32f },
        { 0.36f, 0.77f, 0.39f },
        { 0.38f, 0.54f, 0.89f },
    };

    // Back, front and hover values preserve depth without competing with axis hues.
    const float KVC_GREY_FRONT = 0.62f;    // visible face
    const float KVC_GREY_HOVER = 0.78f;
    const float KVC_GREY_BACK  = 0.30f;    // rear face

    // 1.32 clears the face but keeps axis stubs inside the 52-pixel backdrop radius.
    const float KVC_STUB      = 1.32f;
    const char *KVC_STUB_TEXT[3] = { "X", "Y", "Z" };

    // Projection pill aligns below the cube and claims clicks from the image.
    const float KVC_PROJ_W   = 40.0f;
    const float KVC_PROJ_H   = 20.0f;
    const float KVC_PROJ_GAP = 6.0f;    // below the disc

    // Perspective-grid control sits left of the projection pill. Ortho always has
    // its lattice, and narrow viewports omit this auxiliary control.
    const float KVC_PGRID_W   = 20.0f;
    const float KVC_PGRID_GAP = 5.0f;

    // Grid pill zones are [-], editable value and [+]. Steppers use the shared
    // nice-number ladder; only the separate edit popup creates an ImGui item.
    const float KVC_GRID_W    = 104.0f;
    const float KVC_GRID_H    = 20.0f;
    const float KVC_GRID_STEP = 22.0f;   // width of each stepper zone
    // Snap control shares the grid row and is omitted when it crosses the image inset.
    const float KVC_SNAP_W   = 20.0f;
    const float KVC_SNAP_GAP = 5.0f;
    // Fixed width fits the popup's hint line.
    const float KVC_GRID_EDIT_W = 250.0f;


    inline ImU32 Grey( float v, int alpha )
    {
        int g = (int)( v * 255.0f + 0.5f );
        if ( g < 0 )   g = 0;
        if ( g > 255 ) g = 255;
        return IM_COL32( g, g, g, alpha );
    }

    inline ImU32 AxisCol( int axis, float mul, int alpha )
    {
        const float *c = KVC_AXIS_RGB[axis];
        return IM_COL32( (int)( c[0] * mul * 255.0f ),
                         (int)( c[1] * mul * 255.0f ),
                         (int)( c[2] * mul * 255.0f ),
                         alpha );
    }

    // Walk (+u+v), (-u+v), (-u-v), (+u-v) around the face for convex drawing.
    void FaceCorners( const face_t2 &f, int *out4 )
    {
        const int u = ( f.axis + 1 ) % 3;
        const int v = ( f.axis + 2 ) % 3;
        const int nb = 1 << f.axis;
        const int ub = 1 << u;
        const int vb = 1 << v;
        const int base = ( f.sign > 0.0f ) ? nb : 0;
        out4[0] = base | ub | vb;
        out4[1] = base |      vb;
        out4[2] = base;
        out4[3] = base | ub;
    }

    // Point-in-projected-convex-quad by consistent cross-product sign.
    bool PointInQuad( const ImVec2 *q, float px, float py )
    {
        int pos = 0, neg = 0;
        for ( int i = 0; i < 4; ++i )
        {
            const ImVec2 &a = q[i];
            const ImVec2 &b = q[( i + 1 ) & 3];
            const float cross = ( b.x - a.x ) * ( py - a.y ) - ( b.y - a.y ) * ( px - a.x );
            if ( cross >  0.001f ) ++pos;
            if ( cross < -0.001f ) ++neg;
        }
        return !( pos && neg );
    }

    // Quantize derived yaw so face views land on 90-degree multiples and corners
    // on 45-degree multiples; pole yaw still controls in-plane orientation.
    float SnapAngle( float deg, float quantum )
    {
        float q = deg / quantum;
        q = ( q >= 0.0f ) ? floorf( q + 0.5f ) : ceilf( q - 0.5f );
        float out = q * quantum;
        // Match atan2's (-180, 180] range for direct comparisons.
        while ( out >  180.0f ) out -= 360.0f;
        while ( out <= -180.0f ) out += 360.0f;
        return out;
    }

    // Point the camera along dir, the desired vpn.
    void LookAlongDirection( const camera_s *c, const float *dir )
    {
        float d[3] = { dir[0], dir[1], dir[2] };
        const float len = sqrtf( Dot3( d, d ) );
        if ( !( len > 1.0e-6f ) )
            return;
        d[0] /= len; d[1] /= len; d[2] /= len;

        float z = d[2];
        if ( z >  1.0f ) z =  1.0f;
        if ( z < -1.0f ) z = -1.0f;
        float pitch = (float)RAD2DEG( asinf( z ) );
        // Exact poles prevent plan views leaking side faces; ±89 applies only to drags.
        if ( pitch >  90.0f ) pitch =  90.0f;
        if ( pitch < -90.0f ) pitch = -90.0f;
        // Preserve the 35.264-degree isometric pitch, but flatten numerical near-zero.
        if ( fabsf( pitch ) < 1.0e-3f )
            pitch = 0.0f;

        // Two horizontal components identify the 45-degree corner lattice.
        const bool diagonal = ( fabsf( d[0] ) > 1.0e-4f && fabsf( d[1] ) > 1.0e-4f );
        const float quantum = diagonal ? 45.0f : 90.0f;

        // At a pole, snap the existing yaw because atan2(0,0) has no direction.
        const bool degenerate = ( fabsf( d[0] ) < 1.0e-4f && fabsf( d[1] ) < 1.0e-4f );
        const float raw = degenerate ? c->angles[1]
                                     : (float)RAD2DEG( atan2f( d[1], d[0] ) );
        KiwiCam_LookAlong( pitch, SnapAngle( raw, quantum ) );
    }

    // Desired vpn rows: compass order, then the two poles.
    struct axisView_t
    {
        float       vpn[3];
        const char *name;
    };
    const axisView_t KVC_VIEWS[6] =
    {
        { {  0.0f,  1.0f,  0.0f }, "front"  },   // stand at -Y, look toward +Y
        { { -1.0f,  0.0f,  0.0f }, "right"  },
        { {  0.0f, -1.0f,  0.0f }, "back"   },
        { {  1.0f,  0.0f,  0.0f }, "left"   },
        { {  0.0f,  0.0f, -1.0f }, "top"    },
        { {  0.0f,  0.0f,  1.0f }, "bottom" },
    };
    // cos(3 degrees) tolerates angles -> basis -> dot round trips.
    const float KVC_VIEW_ALIGNED = 0.99863f;

    // Triangle-count preview for the label slot: every visible brush (winding fan),
    // patch (tessellated grid cells x 2) and model entity (lod 0 surfaces), over the
    // active and selected lists, hidden geometry excluded.  Recounted twice a second;
    // the walk is a few thousand nodes at most.
    int   s_triCount     = 0;
    float s_triNextTime  = 0.0f;

    int CountTriangles()
    {
        int tris = 0;
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass == 0 ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( !b->def || ( b->brushFlags & 4 ) != 0 )     // hidden (select.cpp:4168)
                    continue;
                if ( KiwiDrop_IsModelEntity( b ) )
                {
                    float mins[3], maxs[3], angles[3], origin[3], scale;
                    XModel *model = 0;
                    if ( KiwiDrop_GetModelInfo( b, mins, maxs, angles, &scale, origin, &model ) && model )
                    {
                        XSurface *surfs = 0;
                        const int n = XModelGetSurfaces( model, &surfs, 0 );
                        for ( int s = 0; surfs && s < n; ++s )
                            tris += surfs[s].triCount;
                    }
                    continue;
                }
                if ( b->def->patch )
                {
                    const curvePatchDef_t *mesh = b->def->patch->curveDef;
                    if ( mesh && mesh->width > 1 && mesh->height > 1 )
                        tris += ( mesh->width - 1 ) * ( mesh->height - 1 ) * 2;
                    continue;
                }
                if ( !b->def->faces )
                    continue;
                for ( int f = 0; f < b->def->faceCount; ++f )
                {
                    const winding_t *w = b->def->faces[f].w;
                    if ( w && w->numpoints >= 3 )
                        tris += w->numpoints - 2;
                }
            }
        }
        return tris;
    }

    void FormatThousands( char *out, int outSize, int v )
    {
        char raw[32];
        _snprintf( raw, sizeof( raw ), "%d", v );
        raw[sizeof( raw ) - 1] = 0;
        const int len = (int)strlen( raw );
        int o = 0;
        for ( int i = 0; i < len && o < outSize - 1; ++i )
        {
            if ( i && ( ( len - i ) % 3 ) == 0 )
                out[o++] = ',';
            out[o++] = raw[i];
        }
        out[o] = 0;
    }

    // The "1px = ..." zoom meter shows only while the zoom is fresh (the units per
    // pixel changed within the last KVC_ZOOM_LINGER seconds); the rest of the time
    // the slot carries the triangle count.
    const float KVC_ZOOM_LINGER = 1.5f;
    float s_zoomLastWpp  = -1.0f;
    float s_zoomLastTime = -1000.0f;

    // Log-scaled fill is linear in multiplicative wheel steps. Colour maps the same
    // fraction from green to red as world-units-per-pixel sensitivity increases;
    // the formatted number uses current display units. The 8-pixel bar is read-only
    // to avoid accidental zoom jumps, but still claims hover from the image.
    bool DrawZoomBar( float imgMinX, float imgMinY, float imgW, float imgH )
    {
        if ( KVC_StripH( imgH ) <= 0.0f )
            return false;                    // no room: the strip is not drawn at all

        float frac = 0.0f, wpp = 0.0f;
        if ( !KiwiCam_ZoomMeter( &frac, &wpp ) )
            return false;

        const float x1 = imgMinX + imgW - KVC_MARGIN;
        const float x0 = x1 - KVC_ZOOM_W;
        const float y0 = imgMinY + KVC_MARGIN;
        const float y1 = y0 + KVC_ZOOM_H;

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool   hot   = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows )
                          && mouse.x >= x0 && mouse.x <= x1
                          && mouse.y >= y0 && mouse.y <= y1 + KVC_ZOOM_GAP + KVC_ZOOM_TEXT;

        ImDrawList *dl = ImGui::GetWindowDrawList();

        // Fresh zoom?  A changed units-per-pixel restarts the linger clock.
        const float now = (float)ImGui::GetTime();
        if ( s_zoomLastWpp < 0.0f )
            s_zoomLastWpp = wpp;                 // first frame: nothing to announce
        else if ( fabsf( wpp - s_zoomLastWpp ) > wpp * 1.0e-4f )
        {
            s_zoomLastWpp  = wpp;
            s_zoomLastTime = now;
        }
        const bool zoomFresh = ( now - s_zoomLastTime ) < KVC_ZOOM_LINGER || hot;

        if ( zoomFresh )
        {
            dl->AddRectFilled( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                               IM_COL32( 18, 18, 22, 175 ), 2.0f );

            // Two linear legs preserve a true amber midpoint.
            float r, g, b;
            if ( frac < 0.5f )
            {
                const float t = frac * 2.0f;
                r = 0.36f + ( 0.98f - 0.36f ) * t;
                g = 0.80f + ( 0.75f - 0.80f ) * t;
                b = 0.36f + ( 0.22f - 0.36f ) * t;
            }
            else
            {
                const float t = ( frac - 0.5f ) * 2.0f;
                r = 0.98f + ( 0.95f - 0.98f ) * t;
                g = 0.75f + ( 0.24f - 0.75f ) * t;
                b = 0.22f + ( 0.20f - 0.22f ) * t;
            }
            const ImU32 col = IM_COL32( (int)( r * 255.0f ), (int)( g * 255.0f ),
                                        (int)( b * 255.0f ), 235 );

            // Keep a visible sliver at the minimum.
            float fillW = ( x1 - x0 ) * frac;
            if ( fillW < 2.0f ) fillW = 2.0f;
            dl->AddRectFilled( ImVec2( x0, y0 ), ImVec2( x0 + fillW, y1 ), col, 2.0f );
            dl->AddRect( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                         IM_COL32( 80, 84, 96, 150 ), 2.0f, 0, 1.0f );

            char num[64];
            char label[96];
            KiwiUnits_Format( num, sizeof( num ), wpp );
            _snprintf( label, sizeof( label ), "1px = %s", num );
            label[sizeof( label ) - 1] = 0;
            const ImVec2 ts = ImGui::CalcTextSize( label );
            dl->AddText( ImVec2( x1 - ts.x, y1 + KVC_ZOOM_GAP ),
                         hot ? IM_COL32( 255, 226, 110, 255 ) : IM_COL32( 190, 196, 208, 225 ),
                         label );
        }
        // Idle: the slot stays empty; the triangle count lives in the bottom-right
        // corner (DrawTriCount) where View > Show Triangle Count controls it.

        // KIWI-UX: live camera→cursor distance in RAW ENGINE UNITS (inches), under the
        // zoom label.  This is the number distance-based systems compare against (model
        // cull/LOD distances, fog, sound ranges), so a mapper can zoom to where detail
        // should drop and read the threshold straight off the view.  One extra ray only
        // while the cursor is over the camera image.
        {
            int cpx, cpy;
            ray_t ray;
            kiwiDropHit_t hit;
            if ( ImGuiShell_CameraPaintCursor( &cpx, &cpy, nullptr, nullptr )
              && Pick_RayFromImagePos( cpx, cpy, &ray )
              && KiwiDrop_Trace( ray, false, &hit ) )
            {
                const float *eye = Ed_Camera()->origin;
                const float dx = hit.point[0] - eye[0];
                const float dy = hit.point[1] - eye[1];
                const float dz = hit.point[2] - eye[2];
                const float dist = sqrtf( dx * dx + dy * dy + dz * dz );
                char dline[96];
                _snprintf( dline, sizeof( dline ), "cursor %.0f units", dist );
                dline[sizeof( dline ) - 1] = 0;
                const ImVec2 dts = ImGui::CalcTextSize( dline );
                dl->AddText( ImVec2( x1 - dts.x, y1 + KVC_ZOOM_GAP + KVC_ZOOM_TEXT ),
                             IM_COL32( 190, 196, 208, 225 ), dline );
            }
        }
        return hot;
    }

    // Three-state section control: off, waiting for a height, or active. It shares
    // KiwiSection_Toggle with the palette and claims its own hover/click rectangle.
    bool DrawSectionButton( float imgMinX, float imgMinY, float imgW, float imgH )
    {
        if ( KVC_StripH( imgH ) <= 0.0f )
            return false;

        const float x1 = imgMinX + imgW - KVC_MARGIN;
        const float x0 = x1 - KVC_SEC_W;
        const float y0 = imgMinY + KVC_MARGIN + KVC_ZOOM_H + KVC_ZOOM_GAP
                       + KVC_ZOOM_TEXT + KVC_ZOOM_TEXT + KVC_SEC_GAP;   // below the cursor line
        const float y1 = y0 + KVC_SEC_H;

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool   hot   = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows )
                          && mouse.x >= x0 && mouse.x <= x1
                          && mouse.y >= y0 && mouse.y <= y1;

        const bool on   = KiwiSection_Active();
        const bool pick = KiwiSection_Picking();

        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImU32 fill = hot ? IM_COL32( 62, 66, 78, 225 ) : IM_COL32( 18, 18, 22, 175 );
        if ( on )
            fill = hot ? IM_COL32( 150, 110, 30, 235 ) : IM_COL32( 118, 86, 22, 215 );
        else if ( pick )
            fill = hot ? IM_COL32( 96, 84, 44, 235 ) : IM_COL32( 74, 64, 34, 205 );
        dl->AddRectFilled( ImVec2( x0, y0 ), ImVec2( x1, y1 ), fill, 4.0f );
        dl->AddRect( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                     ( on || pick ) ? IM_COL32( 240, 200, 90, 200 )
                                    : IM_COL32( 80, 84, 96, 150 ), 4.0f, 0, 1.0f );

        // Expose the level and whether the active section currently produces a cut.
        char label[64];
        if ( on )
        {
            char zb[32];
            KiwiUnits_Format( zb, sizeof( zb ), KiwiSection_Level() );
            _snprintf( label, sizeof( label ),
                       KiwiSection_Cutting() ? "SECTION  Z %s" : "SECTION (no cut)", zb );
        }
        else
        {
            _snprintf( label, sizeof( label ), "%s", pick ? "CLICK A HEIGHT" : "SECTION" );
        }
        label[sizeof( label ) - 1] = 0;
        const ImVec2 ts = ImGui::CalcTextSize( label );
        dl->AddText( ImVec2( ( x0 + x1 ) * 0.5f - ts.x * 0.5f,
                             ( y0 + y1 ) * 0.5f - ts.y * 0.5f ),
                     ( hot || on || pick ) ? IM_COL32( 255, 234, 150, 255 )
                                           : IM_COL32( 210, 214, 224, 235 ),
                     label );

        if ( hot )
            ImGui::SetTooltip( "Section analysis - cut the 3D view at a Z LEVEL.\n"
                               "Click, then click anything: everything above that\n"
                               "point's height disappears.  Drag the lollipop to\n"
                               "slide the level.  Click again to clear." );

        if ( hot && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
            KiwiSection_Toggle();
        return hot;
    }

    // Projection pill; returned hover is claimed from the camera image.
    bool DrawProjButton( float imgMinX, float imgMinY, float imgW, float imgH )
    {
        const float x1 = imgMinX + imgW - KVC_MARGIN;
        const float x0 = x1 - KVC_PROJ_W;
        const float y0 = KVC_ClusterTop( imgMinY, imgH ) + KVC_BOX + KVC_PROJ_GAP;
        const float y1 = y0 + KVC_PROJ_H;

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool   hot   = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows )
                          && mouse.x >= x0 && mouse.x <= x1
                          && mouse.y >= y0 && mouse.y <= y1;

        const bool  ortho = KiwiCam_Ortho();
        ImDrawList *dl    = ImGui::GetWindowDrawList();
        dl->AddRectFilled( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                           hot ? IM_COL32( 62, 66, 78, 225 ) : IM_COL32( 18, 18, 22, 175 ),
                           4.0f );
        dl->AddRect( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                     IM_COL32( 80, 84, 96, 150 ), 4.0f, 0, 1.0f );

        // Label the current mode, not the click action.
        const char  *label = ortho ? "ORTHO" : "PERSP";
        const ImVec2 ts    = ImGui::CalcTextSize( label );
        // Fall back to one letter when the current font does not fit.
        if ( ts.x + 6.0f > KVC_PROJ_W )
        {
            label = ortho ? "O" : "P";
        }
        const ImVec2 ts2 = ImGui::CalcTextSize( label );
        dl->AddText( ImVec2( ( x0 + x1 ) * 0.5f - ts2.x * 0.5f,
                             ( y0 + y1 ) * 0.5f - ts2.y * 0.5f ),
                     hot ? IM_COL32( 255, 226, 110, 255 ) : IM_COL32( 210, 214, 224, 235 ),
                     label );

        if ( hot && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
            KiwiCam_SetOrtho( !ortho );

        // This auxiliary control is positioned by, and visible only with, perspective.
        bool pgHot = false;
        if ( !ortho )
        {
            const float gx1 = x0 - KVC_PGRID_GAP;
            const float gx0 = gx1 - KVC_PGRID_W;
            if ( gx0 >= imgMinX + KVC_MARGIN )
            {
                const bool on   = KiwiGrid_PerspGrid();
                const bool over = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows )
                               && mouse.x >= gx0 && mouse.x <= gx1
                               && mouse.y >= y0  && mouse.y <= y1;
                pgHot = over;

                dl->AddRectFilled( ImVec2( gx0, y0 ), ImVec2( gx1, y1 ),
                                   over ? IM_COL32( 62, 66, 78, 225 )
                                        : IM_COL32( 18, 18, 22, 175 ), 4.0f );
                dl->AddRect( ImVec2( gx0, y0 ), ImVec2( gx1, y1 ),
                             IM_COL32( 80, 84, 96, 150 ), 4.0f, 0, 1.0f );

                // A lattice glyph communicates the control in its 20-pixel box.
                const ImU32 ink = on ? IM_COL32( 255, 226, 110, 255 )
                                     : IM_COL32( 120, 126, 138, 220 );
                const float gx  = ( gx0 + gx1 ) * 0.5f;
                const float gy  = ( y0  + y1  ) * 0.5f;
                const float r   = 5.0f;
                for ( int i = -1; i <= 1; ++i )
                {
                    const float f = (float)i * r;
                    dl->AddLine( ImVec2( gx - r, gy + f ), ImVec2( gx + r, gy + f ), ink, 1.0f );
                    dl->AddLine( ImVec2( gx + f, gy - r ), ImVec2( gx + f, gy + r ), ink, 1.0f );
                }

                if ( over && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
                    KiwiGrid_SetPerspGrid( !on );
                if ( over )
                    ImGui::SetTooltip( "Ground grid in PERSPECTIVE (off by default).\n"
                                       "Spacing is chosen at the ORBIT PIVOT's depth and the\n"
                                       "lattice stops where its own cells stop resolving, so\n"
                                       "near the horizon you get the major lines and then\n"
                                       "nothing.  Ortho is unaffected." );
            }
        }
        return hot || pgHot;
    }

    // Grid readout and stepper; returned hover is claimed from the camera image.
    bool DrawGridButton( float imgMinX, float imgMinY, float imgW, float imgH )
    {
        const float x1 = imgMinX + imgW - KVC_MARGIN;
        const float x0 = x1 - KVC_GRID_W;
        const float y0 = KVC_ClusterTop( imgMinY, imgH ) + KVC_BOX + KVC_PROJ_GAP
                       + KVC_PROJ_H + KVC_PROJ_GAP;   // below the optional strip
        const float y1 = y0 + KVC_GRID_H;
        // Omit the third row rather than drawing below a short image.
        if ( y1 > imgMinY + imgH - KVC_MARGIN )
            return false;

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool   inWin = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows );
        const bool   over  = inWin
                          && mouse.x >= x0 && mouse.x <= x1
                          && mouse.y >= y0 && mouse.y <= y1;
        const bool hotMinus = over && mouse.x <  x0 + KVC_GRID_STEP;
        const bool hotPlus  = over && mouse.x >= x1 - KVC_GRID_STEP;

        // Omit the snap box rather than drawing across a narrow image's left inset.
        const float sx1 = x0 - KVC_SNAP_GAP;
        const float sx0 = sx1 - KVC_SNAP_W;
        const bool  haveSnapBox = ( sx0 >= imgMinX + KVC_MARGIN );
        const bool  overSnap = haveSnapBox && inWin
                            && mouse.x >= sx0 && mouse.x <= sx1
                            && mouse.y >= y0  && mouse.y <= y1;

        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                           over ? IM_COL32( 62, 66, 78, 225 ) : IM_COL32( 18, 18, 22, 175 ),
                           4.0f );
        dl->AddRect( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                     IM_COL32( 80, 84, 96, 150 ), 4.0f, 0, 1.0f );
        // Dividers distinguish the two stepper zones from the editable value.
        dl->AddLine( ImVec2( x0 + KVC_GRID_STEP, y0 + 3.0f ),
                     ImVec2( x0 + KVC_GRID_STEP, y1 - 3.0f ),
                     IM_COL32( 80, 84, 96, 120 ), 1.0f );
        dl->AddLine( ImVec2( x1 - KVC_GRID_STEP, y0 + 3.0f ),
                     ImVec2( x1 - KVC_GRID_STEP, y1 - 3.0f ),
                     IM_COL32( 80, 84, 96, 120 ), 1.0f );

        const ImU32 dim = IM_COL32( 210, 214, 224, 235 );
        const ImU32 lit = IM_COL32( 255, 226, 110, 255 );

        {
            const ImVec2 ts = ImGui::CalcTextSize( "-" );
            dl->AddText( ImVec2( x0 + KVC_GRID_STEP * 0.5f - ts.x * 0.5f,
                                 ( y0 + y1 ) * 0.5f - ts.y * 0.5f ),
                         hotMinus ? lit : dim, "-" );
        }
        {
            const ImVec2 ts = ImGui::CalcTextSize( "+" );
            dl->AddText( ImVec2( x1 - KVC_GRID_STEP * 0.5f - ts.x * 0.5f,
                                 ( y0 + y1 ) * 0.5f - ts.y * 0.5f ),
                         hotPlus ? lit : dim, "+" );
        }

        // Spacing is already inches; KiwiUnits_Format would convert it twice.
        // KiwiFmt_Num strips trailing zeroes across the supported range.
        char buf[40], value[32];
        _snprintf( buf, sizeof( buf ), "%s in",
                   KiwiFmt_Num( value, sizeof( value ),
                                KiwiUnits_GridSpacingInches(), 6 ) );
        buf[sizeof( buf ) - 1] = '\0';
        const ImVec2 ts = ImGui::CalcTextSize( buf );
        dl->AddText( ImVec2( ( x0 + x1 ) * 0.5f - ts.x * 0.5f,
                             ( y0 + y1 ) * 0.5f - ts.y * 0.5f ),
                     over ? IM_COL32( 236, 240, 248, 250 ) : dim, buf );

        if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            // Shared stepping owns the ladder, clamp and console report.
            if ( hotPlus )       KiwiCmd_StepGrid( true );
            else if ( hotMinus ) KiwiCmd_StepGrid( false );
            else if ( over )
            {
                // Seed the middle-zone editor with the current spacing.
                KiwiFmt_Num( s_gridEditBuf, sizeof( s_gridEditBuf ),
                             KiwiUnits_GridSpacingInches(), 6 );
                s_gridEdit      = true;
                s_gridEditFocus = true;
                // Right-align the wider popup, then clamp it to the image inset.
                s_gridEditX     = x1 - KVC_GRID_EDIT_W;
                if ( s_gridEditX < imgMinX + KVC_MARGIN )
                    s_gridEditX = imgMinX + KVC_MARGIN;
                s_gridEditY     = y1 + 4.0f;
            }
        }

        // A struck-through lattice communicates disabled snapping without a label.
        if ( haveSnapBox )
        {
            const bool on = KiwiGrid_SnapEnabled();
            dl->AddRectFilled( ImVec2( sx0, y0 ), ImVec2( sx1, y1 ),
                               overSnap ? IM_COL32( 62, 66, 78, 225 )
                                        : IM_COL32( 18, 18, 22, 175 ), 4.0f );
            dl->AddRect( ImVec2( sx0, y0 ), ImVec2( sx1, y1 ),
                         IM_COL32( 80, 84, 96, 150 ), 4.0f, 0, 1.0f );

            const ImU32 glyph = on ? IM_COL32( 255, 226, 110, 255 )
                                   : IM_COL32( 120, 124, 136, 220 );
            const float gx0 = sx0 + 5.0f, gx1 = sx1 - 5.0f;
            const float gy0 = y0  + 5.0f, gy1 = y1  - 5.0f;
            for ( int i = 1; i <= 2; ++i )
            {
                const float fx = gx0 + ( gx1 - gx0 ) * ( (float)i / 3.0f );
                const float fy = gy0 + ( gy1 - gy0 ) * ( (float)i / 3.0f );
                dl->AddLine( ImVec2( fx, gy0 ), ImVec2( fx, gy1 ), glyph, 1.0f );
                dl->AddLine( ImVec2( gx0, fy ), ImVec2( gx1, fy ), glyph, 1.0f );
            }
            if ( !on )
                dl->AddLine( ImVec2( sx0 + 3.0f, y1 - 3.0f ),
                             ImVec2( sx1 - 3.0f, y0 + 3.0f ),
                             IM_COL32( 244, 120, 110, 255 ), 2.0f );

            if ( overSnap && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
                KiwiGrid_SetSnapEnabled( !on );
        }

        return over || overSnap;
    }

    // InputText needs a separate top-level ImGui window so it cannot move the camera
    // window's cursor. Claim only this popup's rectangle; claiming the whole viewport
    // while it is open would discard unrelated camera clicks.
    bool DrawGridEditPopup()
    {
        if ( !s_gridEdit )
            return false;
        bool overPopup = false;

        ImGui::SetNextWindowPos( ImVec2( s_gridEditX, s_gridEditY ), ImGuiCond_Always );
        // Height auto-fits while the fixed width reserves space for the hint.
        ImGui::SetNextWindowSize( ImVec2( KVC_GRID_EDIT_W, 0.0f ), ImGuiCond_Always );
        if ( s_gridEditFocus )
            ImGui::SetNextWindowFocus();     // opening frame only

        bool keepOpen = true;
        if ( !ImGui::Begin( "Grid spacing##kiwigridedit", &keepOpen,
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize ) )
        {
            ImGui::End();
            return false;                    // no visible content to claim
        }
        // Include the field while still limiting the claim to this window.
        overPopup = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows
                                          | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem );

        if ( s_gridEditFocus )
        {
            ImGui::SetKeyboardFocusHere();
            s_gridEditFocus = false;
        }
        ImGui::SetNextItemWidth( -1.0f );
        const bool entered = ImGui::InputText( "##kiwigridvalue", s_gridEditBuf,
                                               sizeof( s_gridEditBuf ),
                                               ImGuiInputTextFlags_EnterReturnsTrue
                                             | ImGuiInputTextFlags_AutoSelectAll );
        // Do not use CharsDecimal: the shared grammar accepts operators and units.
        ImGui::TextDisabled( "inches — math and units ok (1/8, 6in, 1ft)  ·  "
                             "Enter applies  ·  Esc cancels" );

        // Cancellation precedes apply so Escape cannot commit.
        if ( ImGui::IsKeyPressed( ImGuiKey_Escape, false ) )
            keepOpen = false;
        else if ( entered )
        {
            // Typed positive values are not restricted to the stepper ladder; invalid
            // shared-parser input leaves the spacing unchanged and the editor open.
            float v = 0.0f;
            if ( KiwiNum_EvalDisplay( s_gridEditBuf, &v ) && KiwiCmd_SetGridSpacing( v ) )
                keepOpen = false;
            else
                s_gridEditFocus = true;      // re-focus and let them try again
        }

        ImGui::End();
        if ( !keepOpen )
        {
            s_gridEdit      = false;
            s_gridEditFocus = false;
            return false;                    // closed this frame: claim nothing
        }
        return overPopup;                    // this popup's rectangle only
    }
}

bool KiwiViewCube_Show()
{
    if ( s_show < 0 )
        s_show = Radiant_ProfileGetInt( "KiwiUX", "ShowViewCube", 1 ) ? 1 : 0;
    return s_show != 0;
}

void KiwiViewCube_SetShow( bool on )
{
    const int v = on ? 1 : 0;
    if ( s_show == v )
        return;
    s_show = v;
    Radiant_ProfileSetInt( "KiwiUX", "ShowViewCube", v );
}

// Read alignment from the live camera instead of maintaining a latch across every
// camera-motion path. outAxis is the faced plane's world normal; outSign is the
// direction along it.
bool KiwiViewCube_ViewAxis( int *outAxis, float *outSign )
{
    camera_s *c = Ed_Camera();
    if ( !c )
        return false;
    CamWnd_BuildMatrix();                        // vpn for THIS frame's angles
    for ( int i = 0; i < 6; ++i )
    {
        if ( Dot3( KVC_VIEWS[i].vpn, c->vpn ) < KVC_VIEW_ALIGNED )
            continue;
        int   axis = 2;
        float sign = 1.0f;
        for ( int k = 0; k < 3; ++k )
            if ( KVC_VIEWS[i].vpn[k] != 0.0f ) { axis = k; sign = KVC_VIEWS[i].vpn[k]; }
        if ( outAxis ) *outAxis = axis;
        if ( outSign ) *outSign = sign;
        return true;
    }
    return false;
}

const char *KiwiViewCube_ViewAxisName()
{
    camera_s *c = Ed_Camera();
    if ( !c )
        return 0;
    CamWnd_BuildMatrix();
    for ( int i = 0; i < 6; ++i )
        if ( Dot3( KVC_VIEWS[i].vpn, c->vpn ) >= KVC_VIEW_ALIGNED )
            return KVC_VIEWS[i].name;
    return 0;
}

void KiwiViewCube_StepAxisView()
{
    camera_s *c = Ed_Camera();
    CamWnd_BuildMatrix();                        // vpn for THIS frame's angles

    // Largest dot is the nearest face view.
    int   best     = 0;
    float bestDot  = -2.0f;
    for ( int i = 0; i < 6; ++i )
    {
        const float d = Dot3( KVC_VIEWS[i].vpn, c->vpn );
        if ( d > bestDot )
        {
            bestDot = d;
            best    = i;
        }
    }

    // Advance only when aligned; otherwise the first press stays near the current view.
    if ( bestDot >= KVC_VIEW_ALIGNED )
        best = ( best + 1 ) % 6;

    LookAlongDirection( c, KVC_VIEWS[best].vpn );
    Sys_Printf( "View: %s.\n", KVC_VIEWS[best].name );
}

// Vertical swipes wrap: reference side -> top -> opposite side -> bottom. A
// pitch/yaw camera has no roll to distinguish how it crossed a pole, so reference
// and phase are remembered, then validated against the live camera before use.
namespace
{
    // Reference side is 0..3; phase is reference, top, opposite, bottom.
    int s_vertRef  = -1;                  // -1 = no memory yet
    int s_vertStep = 0;

    int VertExpected( int ref, int step )
    {
        switch ( step )
        {
        case 1:  return 4;                       // top
        case 2:  return ( ref + 2 ) & 3;         // the opposite side
        case 3:  return 5;                       // bottom
        default: return ref;
        }
    }

    // At a pole, retained yaw must name one of this circle's two side views.
    void ValidateVertPhase( int base, int yawView )
    {
        const bool onPole = ( base == 4 || base == 5 );
        if ( s_vertRef >= 0 && s_vertRef <= 3
          && VertExpected( s_vertRef, s_vertStep ) == base
          && ( !onPole || yawView == s_vertRef || yawView == ( ( s_vertRef + 2 ) & 3 ) ) )
            return;                              // phase still matches the camera

        if ( base <= 3 )
        {
            s_vertRef  = base;
            s_vertStep = 0;
        }
        else
        {
            s_vertRef  = yawView;
            s_vertStep = ( base == 4 ) ? 1 : 3;
        }
    }
}

// Alt+MMB swipe uses the same snap path as cube clicks.
void KiwiViewCube_SwipeAxisView( int dir )
{
    camera_s *c = Ed_Camera();
    CamWnd_BuildMatrix();                        // vpn for THIS frame's angles

    // Start from the nearest face so an orbited view produces a local step.
    int   base    = 0;
    float bestDot = -2.0f;
    for ( int i = 0; i < 6; ++i )
    {
        const float d = Dot3( KVC_VIEWS[i].vpn, c->vpn );
        if ( d > bestDot ) { bestDot = d; base = i; }
    }
    const bool onPole = ( base == 4 || base == 5 );

    // Horizontal compass yaw: left=0, front=90, right=180, back=-90.
    // Right walks front->left->back->right; left reverses it.
    static const int RING_RIGHT[4] = { 3, 0, 1, 2 };   // indexed BY view id 0..3
    static const int RING_LEFT [4] = { 1, 2, 3, 0 };
    // Convert retained pole yaw back to a side-view id.
    const float yawSnapped = SnapAngle( c->angles[1], 90.0f );
    int         yawView    = 3;                        // yaw 0 == left
    if      ( yawSnapped >   45.0f && yawSnapped <=  135.0f ) yawView = 0;   // 90  front
    else if ( yawSnapped >  135.0f || yawSnapped <= -135.0f ) yawView = 1;   // 180 right
    else if ( yawSnapped <  -45.0f )                          yawView = 2;   // -90 back

    int target = -1;
    switch ( dir )
    {
    case 0:                                            // LEFT  — yaw +90
    case 1:                                            // RIGHT — yaw -90
        if ( onPole )
        {
            // Pole spins have no face row, so preserve exact pitch and step yaw directly.
            const float step = ( dir == 1 ) ? -90.0f : 90.0f;
            KiwiCam_LookAlong( ( base == 4 ) ? -90.0f : 90.0f,   // pole-EXACT
                               SnapAngle( yawSnapped + step, 90.0f ) );
            Sys_Printf( "View: %s (spun %s).\n", KVC_VIEWS[base].name,
                        ( dir == 1 ) ? "right" : "left" );
            return;
        }
        target = ( dir == 1 ) ? RING_RIGHT[base] : RING_LEFT[base];
        break;

    case 2:                                            // UP   — pitch +90
    default:                                           // DOWN — pitch -90
        // Add one phase for down or subtract one for up around the four-step cycle.
        ValidateVertPhase( base, yawView );
        s_vertStep = ( s_vertStep + ( ( dir == 3 ) ? 1 : 3 ) ) & 3;
        target     = VertExpected( s_vertRef, s_vertStep );
        break;
    }

    // A horizontal step establishes a new vertical circle immediately.
    if ( dir == 0 || dir == 1 )
    {
        s_vertRef  = target;
        s_vertStep = 0;
    }

    // Keep the table-index guard audible even though current paths produce 0..5.
    if ( target < 0 || target > 5 )
    {
        Sys_Printf( "View cube: refused an out-of-range view index (%i).\n", target );
        return;
    }

    LookAlongDirection( c, KVC_VIEWS[target].vpn );
    Sys_Printf( "View: %s.\n", KVC_VIEWS[target].name );
}

// Bottom-right corner: the scene triangle count (View > Show Triangle Count).  Counts
// every 0.5 s; brushes as winding fans, patches as tessellated cells x 2, model
// entities at lod 0, hidden geometry excluded.  Never claims hover.
static void DrawTriCount( float imgMinX, float imgMinY, float imgW, float imgH )
{
    if ( !KiwiUX_ShowTriCount() || imgW < 160.0f || imgH < 80.0f )
        return;
    const float now = (float)ImGui::GetTime();
    if ( now >= s_triNextTime )
    {
        s_triCount    = CountTriangles();
        s_triNextTime = now + 0.5f;
    }
    char num[32];
    char label[64];
    FormatThousands( num, sizeof( num ), s_triCount );
    _snprintf( label, sizeof( label ), "%s tris", num );
    label[sizeof( label ) - 1] = 0;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImVec2 ts = ImGui::CalcTextSize( label );
    const float x1 = imgMinX + imgW - KVC_MARGIN;
    const float y1 = imgMinY + imgH - KVC_MARGIN;
    dl->AddRectFilled( ImVec2( x1 - ts.x - 8.0f, y1 - ts.y - 6.0f ), ImVec2( x1, y1 ),
                       IM_COL32( 18, 18, 22, 175 ), 3.0f );
    dl->AddText( ImVec2( x1 - ts.x - 4.0f, y1 - ts.y - 3.0f ), IM_COL32( 190, 196, 208, 235 ), label );
}

bool KiwiViewCube_Draw( float imgMinX, float imgMinY, float imgW, float imgH )
{
    if ( imgW < KVC_BOX * 1.5f || imgH < KVC_BOX * 1.5f )
        return false;                            // too small a viewport to be useful

    // Cube visibility does not hide independent camera, section or grid controls.
    DrawTriCount( imgMinX, imgMinY, imgW, imgH );      // corner readout, no hover claim
    const bool zoomHot = DrawZoomBar( imgMinX, imgMinY, imgW, imgH );
    const bool secHot  = DrawSectionButton( imgMinX, imgMinY, imgW, imgH );
    const bool projHot = DrawProjButton( imgMinX, imgMinY, imgW, imgH );
    const bool gridHot = DrawGridButton( imgMinX, imgMinY, imgW, imgH );
    // Draw the editor after its owner and include its rectangle in the hover claim.
    const bool gridEditHot = DrawGridEditPopup();

    if ( !KiwiViewCube_Show() )
        return zoomHot || secHot || projHot || gridHot || gridEditHot;

    camera_s *c = Ed_Camera();
    CamWnd_BuildMatrix();                        // vpn/vright/vup for THIS frame's angles

    const float cx = imgMinX + imgW - KVC_MARGIN - KVC_BOX * 0.5f;
    const float cy = KVC_ClusterTop( imgMinY, imgH ) + KVC_BOX * 0.5f;   // optional strip offset

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool   winHovered = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows );
    const float  mdx = mouse.x - cx, mdy = mouse.y - cy;
    const bool   overWidget = winHovered
                           && ( mdx * mdx + mdy * mdy ) <= ( KVC_BOX * 0.5f ) * ( KVC_BOX * 0.5f );

    ImDrawList *dl = ImGui::GetWindowDrawList();

    // Soft backdrop keeps the cube legible over bright geometry.
    dl->AddCircleFilled( ImVec2( cx, cy ), KVC_BOX * 0.5f, IM_COL32( 18, 18, 22, 140 ), 32 );
    dl->AddCircle( ImVec2( cx, cy ), KVC_BOX * 0.5f, IM_COL32( 80, 84, 96, 120 ), 32, 1.0f );

    // Project the eight corners once.
    ImVec2 pt[8];
    float  cdepth[8];
    for ( int i = 0; i < 8; ++i )
    {
        float v[3];
        CornerVec( i, v );
        pt[i]     = ImVec2( cx + Dot3( v, c->vright ) * KVC_HALF,
                            cy - Dot3( v, c->vup    ) * KVC_HALF );
        cdepth[i] = Dot3( v, c->vpn );
    }

    // Order faces back-to-front.
    struct laidFace_t { float depth; int idx; };
    laidFace_t order[6];
    for ( int i = 0; i < 6; ++i )
    {
        float n[3] = { 0.0f, 0.0f, 0.0f };
        n[ KVC_FACES[i].axis ] = KVC_FACES[i].sign;
        order[i].depth = Dot3( n, c->vpn );
        order[i].idx   = i;
    }
    for ( int i = 0; i < 6; ++i )                // six elements: simple in-place sort
        for ( int j = i + 1; j < 6; ++j )
            if ( order[j].depth > order[i].depth )
            {
                const laidFace_t t = order[i]; order[i] = order[j]; order[j] = t;
            }

    // Corners win overlapping face hits and are clickable only at depth < 0.
    int hotCorner = -1;
    int hotFace   = -1;
    if ( overWidget )
    {
        float best = KVC_CORNER;
        for ( int i = 0; i < 8; ++i )
        {
            if ( cdepth[i] >= 0.0f )
                continue;
            const float dx = mouse.x - pt[i].x, dy = mouse.y - pt[i].y;
            const float d  = sqrtf( dx * dx + dy * dy );
            if ( d <= best )
            {
                best      = d;
                hotCorner = i;
            }
        }
        if ( hotCorner < 0 )
        {
            // Front-to-back over the painter order, so the nearest face wins.
            for ( int k = 5; k >= 0 && hotFace < 0; --k )
            {
                if ( order[k].depth >= 0.0f )
                    continue;                    // back face: not clickable
                int ci[4];
                FaceCorners( KVC_FACES[ order[k].idx ], ci );
                const ImVec2 quad[4] = { pt[ci[0]], pt[ci[1]], pt[ci[2]], pt[ci[3]] };
                if ( PointInQuad( quad, mouse.x, mouse.y ) )
                    hotFace = order[k].idx;
            }
        }
    }

    // Project stubs through the cube basis; draw rear stubs before faces and front
    // stubs afterward so the body occludes them correctly.
    ImVec2 stubPt[3];
    float  stubDepth[3];
    for ( int a = 0; a < 3; ++a )
    {
        float v[3] = { 0.0f, 0.0f, 0.0f };
        v[a] = 1.0f;
        stubPt[a]    = ImVec2( cx + Dot3( v, c->vright ) * KVC_HALF * KVC_STUB,
                               cy - Dot3( v, c->vup    ) * KVC_HALF * KVC_STUB );
        stubDepth[a] = Dot3( v, c->vpn );
    }
    for ( int a = 0; a < 3; ++a )
        if ( stubDepth[a] >= 0.0f )              // behind the cube
            dl->AddLine( ImVec2( cx, cy ), stubPt[a], AxisCol( a, 0.55f, 150 ), 2.0f );

    // Draw faces back-to-front.
    for ( int k = 0; k < 6; ++k )
    {
        const face_t2 &f     = KVC_FACES[ order[k].idx ];
        const bool     front = ( order[k].depth < 0.0f );
        const bool     isHot = ( hotFace == order[k].idx );

        int ci[4];
        FaceCorners( f, ci );
        ImVec2 quad[4] = { pt[ci[0]], pt[ci[1]], pt[ci[2]], pt[ci[3]] };

        // Grey levels distinguish back, front and hover without axis hues.
        const ImU32 fill = Grey( isHot ? KVC_GREY_HOVER
                                       : ( front ? KVC_GREY_FRONT : KVC_GREY_BACK ),
                                 245 );
        dl->AddConvexPolyFilled( quad, 4, fill );

        // Darker edges preserve the silhouette on a light body.
        const ImU32 edge = front ? Grey( 0.34f, 190 ) : Grey( 0.20f, 130 );
        for ( int e = 0; e < 4; ++e )
            dl->AddLine( quad[e], quad[( e + 1 ) & 3], edge, 1.0f );

        if ( !front )
            continue;                            // labels on the visible side only

        const ImVec2 mid( ( quad[0].x + quad[2].x ) * 0.5f,
                          ( quad[0].y + quad[2].y ) * 0.5f );
        const ImVec2 ts = ImGui::CalcTextSize( f.label );
        // Suppress labels that do not fit an edge-on face.
        const float extent = sqrtf( ( quad[0].x - quad[2].x ) * ( quad[0].x - quad[2].x )
                                  + ( quad[0].y - quad[2].y ) * ( quad[0].y - quad[2].y ) );
        if ( extent < ts.x + 4.0f )
            continue;
        dl->AddText( ImVec2( mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f ),
                     Grey( isHot ? 0.10f : 0.16f, 250 ), f.label );
    }

    // Draw front stubs and labels in world-axis colours.
    for ( int a = 0; a < 3; ++a )
    {
        if ( stubDepth[a] >= 0.0f )
            continue;
        dl->AddLine( ImVec2( cx, cy ), stubPt[a], AxisCol( a, 1.0f, 235 ), 2.0f );
        const ImVec2 ts = ImGui::CalcTextSize( KVC_STUB_TEXT[a] );
        dl->AddText( ImVec2( stubPt[a].x - ts.x * 0.5f, stubPt[a].y - ts.y * 0.5f ),
                     AxisCol( a, 1.0f, 245 ), KVC_STUB_TEXT[a] );
    }

    // Corner targets appear only on hover to avoid eight persistent dots.
    if ( hotCorner >= 0 )
        dl->AddCircleFilled( pt[hotCorner], 4.0f, IM_COL32( 255, 226, 110, 255 ), 12 );

    if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
    {
        if ( hotCorner >= 0 )
        {
            // Stand off the corner and look back for its isometric view.
            float v[3];
            CornerVec( hotCorner, v );
            const float back[3] = { -v[0], -v[1], -v[2] };
            LookAlongDirection( c, back );
        }
        else if ( hotFace >= 0 )
        {
            const face_t2 &f = KVC_FACES[hotFace];
            float back[3] = { 0.0f, 0.0f, 0.0f };
            back[f.axis] = -f.sign;              // vpn = -normal
            LookAlongDirection( c, back );
            // Publish the faced construction plane immediately; auto-plane would
            // otherwise wait until the next tool. Live tools capture their plane in
            // Begin(), so changing the active plane cannot move existing points.
            {
                // axis 2=XY, 1=XZ, 0=YZ; preserve position along the new normal.
                extern void KiwiCon_SetPlaneAxis( int axis );
                KiwiCon_SetPlaneAxis( f.axis );
            }
        }
    }

    // Every visible control claims hover so clicks cannot start a marquee behind it.
    return overWidget || zoomHot || secHot || projHot || gridHot || gridEditHot;
}

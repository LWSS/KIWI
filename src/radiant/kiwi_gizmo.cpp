#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>

#include "kiwi_gizmo.h"
#include "kiwi_camera.h"
#include "kiwi_command.h"
#include "kiwi_lollipop.h"
#include "kiwi_pick.h"
#include "kiwi_transform.h"
#include "kiwi_ux.h"
#include "radiant_registry.h"
#include "kiwi_vec.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

extern camera_s *Ed_Camera();
extern void      CamWnd_BuildMatrix();
extern int       g_nUpdateBits;
extern char      Byte4PackPixelColor( float *from, GfxColor *out );
extern void __cdecl R_AddRenderCmdDrawTris(
    Material *material, MaterialTechniqueType techType, short indexCount,
    const uint16_t *indices, short vertexCount,
    const float ( *xyzw )[4], const float ( *normal )[3], float *color,
    const float ( *st )[2] );

namespace
{
    const float KGZ_GROW          = 1.15f;
    const float KGZ_AXIS_PIX      = 70.0f * KGZ_GROW;
    const float KGZ_HEAD_PIX      = KGZ_AXIS_PIX * 0.20f;
    const float KGZ_HEAD_RAD_PIX  = KGZ_AXIS_PIX * 0.10f;
    const float KGZ_PLANE_OFF_PIX = KGZ_AXIS_PIX * 0.50f;
    const float KGZ_PLANE_RAD_PIX = KGZ_AXIS_PIX * 0.10f;
    const float KGZ_CENTER_PIX    = 10.0f * KGZ_GROW;
    const float KGZ_RING_PIX      = 55.0f * KGZ_GROW;
    const float KGZ_VIEW_RING_MUL = 0.80f / 0.70f;
    const float KGZ_SCALE_BOX_PIX = KGZ_AXIS_PIX * 0.10f;
    // KIWI (2026-09-15, user: "uniform scale should be the default, I don't see a way to
    // do it with the gizmo"): the uniform ball is the scale gizmo's main handle, so it
    // is drawn nearly twice as large as the axis boxes.
    const float KGZ_SCALE_BALL_PIX = KGZ_AXIS_PIX * 0.17f;

    const float KGZ_LINE_PIX       = 2.0f;
    const float KGZ_LINE_HOVER_PIX = 3.0f;
    const float KGZ_NORMAL_ALPHA   = 0.75f;
    const float KGZ_HOVER_ALPHA    = 1.0f;
    const float KGZ_SECTOR_ALPHA   = 0.22f;

    const float KGZ_CENTER_PICK_PIX = 12.0f * KGZ_GROW;
    const float KGZ_SHAFT_PICK_PIX  = 6.0f;
    const float KGZ_SOLID_PICK_PAD  = 3.0f;
    const float KGZ_RING_PICK_PIX   = KGZ_RING_PIX * 0.15f;
    const float KGZ_AXIS_MIN_T      = 0.20f;

    const int KGZ_CONE_SEGMENTS   = 12;
    const int KGZ_CIRCLE_SEGMENTS = 64;
    const int KGZ_SPHERE_SEGMENTS = 16;
    const float KGZ_PI      = 3.141592653589793f;
    const float KGZ_TWO_PI  = 6.283185307179586f;
    const float KGZ_DEGREES = 57.29577951308232f;

    enum handle_t
    {
        KGZ_NONE = -1,
        KGZ_AXIS_X = 0, KGZ_AXIS_Y, KGZ_AXIS_Z,
        KGZ_PLANE_X, KGZ_PLANE_Y, KGZ_PLANE_Z,
        KGZ_CENTER,
        KGZ_NORMAL
    };

    enum linearKind_t
    {
        KGZ_MOVE,
        KGZ_SCALE
    };

    // Plasticity default-theme 600 ramp, with its 400 ramp for hover.
    const float KGZ_AXIS_RGB[3][3] =
    {
        { 0.811765f, 0.066667f, 0.141176f },
        { 0.098039f, 0.580392f, 0.450980f },
        { 0.145098f, 0.388235f, 0.921569f }
    };
    const float KGZ_AXIS_HOVER_RGB[3][3] =
    {
        { 0.937255f, 0.305882f, 0.305882f },
        { 0.243137f, 0.741176f, 0.576471f },
        { 0.376471f, 0.647059f, 0.980392f }
    };
    // Cyan and magenta are the linear-space midpoints used by lerpColors,
    // encoded back to display RGB for this editor's packed-color path.
    const float KGZ_PLANE_RGB[3][3] =
    {
        { 0.123744f, 0.496106f, 0.736085f }, // YZ, cyan
        { 0.602294f, 0.283187f, 0.683034f }, // XZ, magenta
        { 0.992157f, 0.878431f, 0.278431f }  // XY, yellow
    };
    const float KGZ_PLANE_HOVER_RGB[3][3] =
    {
        { 0.318191f, 0.696183f, 0.811714f },
        { 0.728774f, 0.512950f, 0.745850f },
        { 0.996078f, 0.941176f, 0.541176f }
    };
    const float KGZ_WHITE_RGB[3]       = { 0.980392f, 0.980392f, 0.980392f };
    const float KGZ_WHITE_HOVER_RGB[3] = { 1.0f, 1.0f, 1.0f };
    const float KGZ_NORMAL_RGB[3]      = { 0.992157f, 0.878431f, 0.278431f };
    const float KGZ_NORMAL_HOVER_RGB[3]= { 0.996078f, 0.941176f, 0.541176f };

    int   s_hot       = KGZ_NONE;
    int   s_grabbed   = KGZ_NONE;
    int   s_ringHot   = -1;
    int   s_ringGrab  = -1;
    int   s_show      = -1;
    float s_ringStart = 0.0f;
    float s_ringPrev  = 0.0f;
    float s_ringTotal = 0.0f;

    struct gizmoGeo_t
    {
        linearKind_t kind;
        float anchor[3];
        float wpp;
        float axisLen;
        float headLen;
        float headRad;
        float planeOff;
        float planeRad;
        float centreRad;
        bool  hasNormal;
        float normal[3];
    };

    struct ringGeo_t
    {
        float pivot[3];
        float radius;
    };

    float Length3( const float *v )
    {
        return sqrtf( Dot3( v, v ) );
    }

    bool Normalize3( float *v )
    {
        const float len = Length3( v );
        if ( len <= 0.000001f )
            return false;
        v[0] /= len; v[1] /= len; v[2] /= len;
        return true;
    }

    void Cross3Local( const float *a, const float *b, float *out )
    {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }

    void AxisVector( int axis, float *out )
    {
        out[0] = out[1] = out[2] = 0.0f;
        out[axis] = 1.0f;
    }

    bool ScaleActive()
    {
        KiwiEditorCommand *cmd = KiwiCmd_Active();
        return cmd && cmd->Name() && strcmp( cmd->Name(), "Scale" ) == 0;
    }

    bool GizmoUsable()
    {
        if ( !KiwiUX_ModernInput() || !KiwiGizmo_Show() )
            return false;
        if ( KiwiXform_PivotPlacing() || KiwiLollipop_Active() )
            return false;
        const camera_s *c = Ed_Camera();
        return c->width >= 1 && c->height >= 1;
    }

    bool BuildLinear( gizmoGeo_t *g )
    {
        if ( !g || !GizmoUsable() )
            return false;

        if ( KiwiXform_IsMoveActive() )
        {
            g->kind = KGZ_MOVE;
            if ( !KiwiXform_ActivePivot( g->anchor ) )
                return false;
        }
        else if ( ScaleActive() )
        {
            g->kind = KGZ_SCALE;
            KiwiEditorCommand *cmd = KiwiCmd_Active();
            if ( !cmd || !cmd->BubbleAnchor( g->anchor ) )
                return false;
        }
        else
        {
            return false;
        }

        g->wpp       = KiwiCam_WorldPerPixel( g->anchor );
        g->axisLen   = KGZ_AXIS_PIX      * g->wpp;
        g->headLen   = KGZ_HEAD_PIX      * g->wpp;
        g->headRad   = KGZ_HEAD_RAD_PIX  * g->wpp;
        g->planeOff  = KGZ_PLANE_OFF_PIX * g->wpp;
        g->planeRad  = KGZ_PLANE_RAD_PIX * g->wpp;
        g->centreRad = ( g->kind == KGZ_SCALE ? KGZ_SCALE_BALL_PIX : KGZ_CENTER_PIX ) * g->wpp;
        g->hasNormal = g->kind == KGZ_MOVE && KiwiXform_ActivePushDir( g->normal );
        return g->wpp > 0.0f;
    }

    bool BuildRings( ringGeo_t *g )
    {
        if ( !g || !GizmoUsable() || !KiwiXform_IsRotateActive() )
            return false;
        if ( !KiwiXform_ActivePivot( g->pivot ) )
            return false;
        g->radius = KGZ_RING_PIX * KiwiCam_WorldPerPixel( g->pivot );
        return g->radius > 0.0f;
    }

    void AxisTip( const gizmoGeo_t &g, int axis, float *out )
    {
        Copy3( g.anchor, out );
        out[axis] += g.axisLen;
    }

    void NormalTip( const gizmoGeo_t &g, float *out )
    {
        Mad3( g.anchor, g.normal, g.axisLen * 1.25f, out );
    }

    void PlaneSquare( const gizmoGeo_t &g, int normalAxis, float q[4][3] )
    {
        const int i = ( normalAxis + 1 ) % 3;
        const int j = ( normalAxis + 2 ) % 3;
        const float si[4] = { -1.0f, 1.0f, 1.0f, -1.0f };
        const float sj[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
        for ( int k = 0; k < 4; ++k )
        {
            Copy3( g.anchor, q[k] );
            q[k][i] += g.planeOff + si[k] * g.planeRad;
            q[k][j] += g.planeOff + sj[k] * g.planeRad;
        }
    }

    void RingPointAngle( const ringGeo_t &g, int axis, float angle, float radiusMul, float *out )
    {
        const int i = ( axis + 1 ) % 3;
        const int j = ( axis + 2 ) % 3;
        Copy3( g.pivot, out );
        out[i] += cosf( angle ) * g.radius * radiusMul;
        out[j] += sinf( angle ) * g.radius * radiusMul;
    }

    void RingPoint( const ringGeo_t &g, int axis, int k, float *out )
    {
        RingPointAngle( g, axis, KGZ_TWO_PI * (float)k / (float)KGZ_CIRCLE_SEGMENTS, 1.0f, out );
    }

    bool RayPlane( const ray_t &ray, const float *point, const float *normal, float *out )
    {
        const float den = Dot3( ray.dir, normal );
        if ( fabsf( den ) < 0.00001f )
            return false;
        float rel[3];
        Sub3( point, ray.origin, rel );
        const float t = Dot3( rel, normal ) / den;
        if ( t <= 0.0f || t > 1000000.0f )
            return false;
        Mad3( ray.origin, ray.dir, t, out );
        return true;
    }

    bool RingCursorAngle( const ringGeo_t &g, int axis, int imgX, int imgY, float *outDegrees )
    {
        ray_t ray;
        if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
            return false;
        float normal[3];
        AxisVector( axis, normal );
        float hit[3];
        if ( !RayPlane( ray, g.pivot, normal, hit ) )
            return false;
        float rel[3];
        Sub3( hit, g.pivot, rel );
        const int i = ( axis + 1 ) % 3;
        const int j = ( axis + 2 ) % 3;
        if ( fabsf( rel[i] ) < g.radius * 0.001f && fabsf( rel[j] ) < g.radius * 0.001f )
            return false;
        *outDegrees = atan2f( rel[j], rel[i] ) * KGZ_DEGREES;
        return true;
    }

    const float *HandleRgb( int handle, const float *normal, const float *hover )
    {
        return ( s_hot == handle || s_grabbed == handle ) ? hover : normal;
    }

    float HandleAlpha( int handle )
    {
        return ( s_hot == handle || s_grabbed == handle ) ? KGZ_HOVER_ALPHA : KGZ_NORMAL_ALPHA;
    }

    float HandleWidth( int handle )
    {
        return ( s_hot == handle || s_grabbed == handle ) ? KGZ_LINE_HOVER_PIX : KGZ_LINE_PIX;
    }

    const float *AxisRgb( int axis )
    {
        return HandleRgb( KGZ_AXIS_X + axis, KGZ_AXIS_RGB[axis], KGZ_AXIS_HOVER_RGB[axis] );
    }

    const float *PlaneRgb( int normalAxis )
    {
        return HandleRgb( KGZ_PLANE_X + normalAxis,
                          KGZ_PLANE_RGB[normalAxis], KGZ_PLANE_HOVER_RGB[normalAxis] );
    }

    const float *CentreRgb()
    {
        return HandleRgb( KGZ_CENTER, KGZ_WHITE_RGB, KGZ_WHITE_HOVER_RGB );
    }

    const float *NormalRgb()
    {
        return HandleRgb( KGZ_NORMAL, KGZ_NORMAL_RGB, KGZ_NORMAL_HOVER_RGB );
    }

    const float *RingRgb( int axis )
    {
        const bool emph = s_ringGrab == axis || ( s_ringGrab < 0 && s_ringHot == axis );
        return emph ? KGZ_AXIS_HOVER_RGB[axis] : KGZ_AXIS_RGB[axis];
    }

    float RingAlpha( int axis )
    {
        return ( s_ringGrab == axis || ( s_ringGrab < 0 && s_ringHot == axis ) )
             ? KGZ_HOVER_ALPHA : KGZ_NORMAL_ALPHA;
    }

    float RingWidth( int axis )
    {
        return ( s_ringGrab == axis || ( s_ringGrab < 0 && s_ringHot == axis ) )
             ? KGZ_LINE_HOVER_PIX : KGZ_LINE_PIX;
    }

    // white_tools is alpha blended but back-face culled.  Each triangle is emitted
    // in both windings; only one winding survives, so translucency is not doubled.
    enum { KGZ_MESH_MAX_VERTS = 4096, KGZ_MESH_MAX_INDICES = 12288 };
    float    s_xyzw[KGZ_MESH_MAX_VERTS][4];
    float    s_normal[KGZ_MESH_MAX_VERTS][3];
    float    s_st[KGZ_MESH_MAX_VERTS][2];
    float    s_color[KGZ_MESH_MAX_VERTS];
    uint16_t s_index[KGZ_MESH_MAX_INDICES];
    int      s_vertexCount = 0;
    int      s_indexCount  = 0;
    float    s_packedColor = 0.0f;
    float    s_meshRgb[3]  = { 1.0f, 1.0f, 1.0f };

    void MeshBegin( const float *rgb, float alpha )
    {
        s_vertexCount = 0;
        s_indexCount  = 0;
        s_meshRgb[0] = rgb[0]; s_meshRgb[1] = rgb[1]; s_meshRgb[2] = rgb[2];
        float rgba[4] = { rgb[0], rgb[1], rgb[2], alpha };
        GfxColor packed;
        Byte4PackPixelColor( rgba, &packed );
        memcpy( &s_packedColor, &packed.packed, sizeof( s_packedColor ) );
    }

    int MeshVertex( const float *point )
    {
        if ( s_vertexCount >= KGZ_MESH_MAX_VERTS )
            return -1;
        const int i = s_vertexCount++;
        s_xyzw[i][0] = point[0]; s_xyzw[i][1] = point[1]; s_xyzw[i][2] = point[2]; s_xyzw[i][3] = 1.0f;
        s_normal[i][0] = 0.0f; s_normal[i][1] = 0.0f; s_normal[i][2] = 1.0f;
        s_st[i][0] = 0.0f; s_st[i][1] = 0.0f;
        s_color[i] = s_packedColor;
        return i;
    }

    void MeshTriangle( int a, int b, int c )
    {
        if ( a < 0 || b < 0 || c < 0 || s_indexCount + 6 > KGZ_MESH_MAX_INDICES )
            return;
        s_index[s_indexCount++] = (uint16_t)a;
        s_index[s_indexCount++] = (uint16_t)b;
        s_index[s_indexCount++] = (uint16_t)c;
        s_index[s_indexCount++] = (uint16_t)a;
        s_index[s_indexCount++] = (uint16_t)c;
        s_index[s_indexCount++] = (uint16_t)b;
    }

    void MeshFlush()
    {
        if ( s_vertexCount < 3 || s_indexCount < 6 )
            return;
        const float flat[4] = { s_meshRgb[0], s_meshRgb[1], s_meshRgb[2], 1.0f };
        R_AddCmdSetMaterialColor( flat );
        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                (short)s_indexCount, s_index, (short)s_vertexCount,
                                s_xyzw, s_normal, s_color, s_st );
    }

    void MeshPassEnd()
    {
        static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        R_AddCmdSetMaterialColor( white );
    }

    void PerpendicularBasis( const float *direction, const camera_s *c, float *u, float *v )
    {
        const float alongRight = Dot3( c->vright, direction );
        for ( int k = 0; k < 3; ++k )
            u[k] = c->vright[k] - direction[k] * alongRight;
        if ( !Normalize3( u ) )
        {
            const float alongUp = Dot3( c->vup, direction );
            for ( int k = 0; k < 3; ++k )
                u[k] = c->vup[k] - direction[k] * alongUp;
        }
        if ( !Normalize3( u ) )
        {
            const float fallback[3] = { fabsf( direction[0] ) < 0.8f ? 1.0f : 0.0f,
                                        fabsf( direction[0] ) < 0.8f ? 0.0f : 1.0f,
                                        0.0f };
            Cross3Local( direction, fallback, u );
            Normalize3( u );
        }
        Cross3Local( direction, u, v );
        Normalize3( v );
    }

    void DrawCone( const float *basePoint, const float *direction, float length, float radius,
                   const camera_s *c, const float *rgb, float alpha )
    {
        float tip[3];
        Mad3( basePoint, direction, length, tip );
        float u[3], v[3];
        PerpendicularBasis( direction, c, u, v );

        MeshBegin( rgb, alpha );
        const int tipIndex = MeshVertex( tip );
        const int baseIndex = MeshVertex( basePoint );
        int rim[KGZ_CONE_SEGMENTS];
        for ( int k = 0; k < KGZ_CONE_SEGMENTS; ++k )
        {
            const float angle = KGZ_TWO_PI * (float)k / (float)KGZ_CONE_SEGMENTS;
            float point[3];
            for ( int a = 0; a < 3; ++a )
                point[a] = basePoint[a] + u[a] * cosf( angle ) * radius + v[a] * sinf( angle ) * radius;
            rim[k] = MeshVertex( point );
        }
        for ( int k = 0; k < KGZ_CONE_SEGMENTS; ++k )
        {
            const int n = ( k + 1 ) % KGZ_CONE_SEGMENTS;
            MeshTriangle( tipIndex, rim[k], rim[n] );
            MeshTriangle( baseIndex, rim[n], rim[k] );
        }
        MeshFlush();
    }

    void DrawBox( const float *centre, float size, const float *rgb, float alpha )
    {
        const float half = size * 0.5f;
        MeshBegin( rgb, alpha );
        int v[8];
        for ( int i = 0; i < 8; ++i )
        {
            const float point[3] =
            {
                centre[0] + ( ( i & 1 ) ? half : -half ),
                centre[1] + ( ( i & 2 ) ? half : -half ),
                centre[2] + ( ( i & 4 ) ? half : -half )
            };
            v[i] = MeshVertex( point );
        }
        const int faces[6][4] =
        {
            { 0, 2, 3, 1 }, { 4, 5, 7, 6 },
            { 0, 1, 5, 4 }, { 2, 6, 7, 3 },
            { 0, 4, 6, 2 }, { 1, 3, 7, 5 }
        };
        for ( int f = 0; f < 6; ++f )
        {
            MeshTriangle( v[faces[f][0]], v[faces[f][1]], v[faces[f][2]] );
            MeshTriangle( v[faces[f][0]], v[faces[f][2]], v[faces[f][3]] );
        }
        MeshFlush();
    }

    void DrawSphere( const float *centre, float radius, const float *rgb, float alpha )
    {
        MeshBegin( rgb, alpha );
        float point[3] = { centre[0], centre[1], centre[2] + radius };
        const int top = MeshVertex( point );
        int ring[KGZ_SPHERE_SEGMENTS - 1][KGZ_SPHERE_SEGMENTS];
        for ( int lat = 1; lat < KGZ_SPHERE_SEGMENTS; ++lat )
        {
            const float phi = KGZ_PI * (float)lat / (float)KGZ_SPHERE_SEGMENTS;
            const float z = cosf( phi ) * radius;
            const float radial = sinf( phi ) * radius;
            for ( int lon = 0; lon < KGZ_SPHERE_SEGMENTS; ++lon )
            {
                const float angle = KGZ_TWO_PI * (float)lon / (float)KGZ_SPHERE_SEGMENTS;
                point[0] = centre[0] + cosf( angle ) * radial;
                point[1] = centre[1] + sinf( angle ) * radial;
                point[2] = centre[2] + z;
                ring[lat - 1][lon] = MeshVertex( point );
            }
        }
        point[0] = centre[0]; point[1] = centre[1]; point[2] = centre[2] - radius;
        const int bottom = MeshVertex( point );

        for ( int lon = 0; lon < KGZ_SPHERE_SEGMENTS; ++lon )
        {
            const int next = ( lon + 1 ) % KGZ_SPHERE_SEGMENTS;
            MeshTriangle( top, ring[0][next], ring[0][lon] );
            for ( int lat = 0; lat < KGZ_SPHERE_SEGMENTS - 2; ++lat )
            {
                MeshTriangle( ring[lat][lon], ring[lat][next], ring[lat + 1][next] );
                MeshTriangle( ring[lat][lon], ring[lat + 1][next], ring[lat + 1][lon] );
            }
            MeshTriangle( ring[KGZ_SPHERE_SEGMENTS - 2][lon],
                          ring[KGZ_SPHERE_SEGMENTS - 2][next], bottom );
        }
        MeshFlush();
    }

    void DrawQuad( float points[4][3], const float *rgb, float alpha )
    {
        MeshBegin( rgb, alpha );
        const int a = MeshVertex( points[0] );
        const int b = MeshVertex( points[1] );
        const int c = MeshVertex( points[2] );
        const int d = MeshVertex( points[3] );
        MeshTriangle( a, b, c );
        MeshTriangle( a, c, d );
        MeshFlush();
    }

    enum { KGZ_RIBBON_MAX_POINTS = 128 };
    void DrawRibbon( const float points[][3], int count, float widthPixels, bool closed,
                     const camera_s *c, const float *rgb, float alpha )
    {
        if ( count < 2 || count > KGZ_RIBBON_MAX_POINTS )
            return;
        MeshBegin( rgb, alpha );
        int left[KGZ_RIBBON_MAX_POINTS], right[KGZ_RIBBON_MAX_POINTS];
        for ( int i = 0; i < count; ++i )
        {
            const int prev = i > 0 ? i - 1 : ( closed ? count - 1 : 0 );
            const int next = i + 1 < count ? i + 1 : ( closed ? 0 : count - 1 );
            float tangent[3];
            Sub3( points[next], points[prev], tangent );
            const float sx = Dot3( tangent, c->vright );
            const float sy = Dot3( tangent, c->vup );
            const float screenLen = sqrtf( sx * sx + sy * sy );
            float side[3];
            if ( screenLen > 0.000001f )
            {
                const float px = -sy / screenLen;
                const float py =  sx / screenLen;
                for ( int k = 0; k < 3; ++k )
                    side[k] = c->vright[k] * px + c->vup[k] * py;
            }
            else
            {
                Copy3( c->vright, side );
            }
            const float half = KiwiCam_WorldPerPixel( points[i] ) * widthPixels * 0.5f;
            float a[3], b[3];
            Mad3( points[i], side,  half, a );
            Mad3( points[i], side, -half, b );
            left[i] = MeshVertex( a );
            right[i] = MeshVertex( b );
        }
        const int segments = closed ? count : count - 1;
        for ( int i = 0; i < segments; ++i )
        {
            const int next = ( i + 1 ) % count;
            MeshTriangle( left[i], right[i], right[next] );
            MeshTriangle( left[i], right[next], left[next] );
        }
        MeshFlush();
    }

    void DrawAxisHandle( const gizmoGeo_t &g, const camera_s *c, int axis )
    {
        const int handle = KGZ_AXIS_X + axis;
        const float *rgb = AxisRgb( axis );
        const float alpha = HandleAlpha( handle );
        float direction[3];
        AxisVector( axis, direction );
        float end[3];
        if ( g.kind == KGZ_MOVE )
            Mad3( g.anchor, direction, g.axisLen - g.headLen, end );
        else
            Mad3( g.anchor, direction, g.axisLen - KGZ_SCALE_BOX_PIX * g.wpp * 0.5f, end );
        float shaft[2][3];
        Copy3( g.anchor, shaft[0] ); Copy3( end, shaft[1] );
        DrawRibbon( shaft, 2, HandleWidth( handle ), false, c, rgb, alpha );

        if ( g.kind == KGZ_MOVE )
            DrawCone( end, direction, g.headLen, g.headRad, c, rgb, alpha );
        else
        {
            float tip[3];
            AxisTip( g, axis, tip );
            DrawBox( tip, KGZ_SCALE_BOX_PIX * g.wpp, rgb, alpha );
        }
    }

    void DrawNormalHandle( const gizmoGeo_t &g, const camera_s *c )
    {
        float tip[3];
        NormalTip( g, tip );
        float base[3];
        Mad3( tip, g.normal, -g.headLen, base );
        float shaft[2][3];
        Copy3( g.anchor, shaft[0] ); Copy3( base, shaft[1] );
        DrawRibbon( shaft, 2, HandleWidth( KGZ_NORMAL ), false, c,
                    NormalRgb(), HandleAlpha( KGZ_NORMAL ) );
        DrawCone( base, g.normal, g.headLen, g.headRad, c,
                  NormalRgb(), HandleAlpha( KGZ_NORMAL ) );
    }

    void DrawCentre( const gizmoGeo_t &g, const camera_s *c )
    {
        if ( g.kind == KGZ_SCALE )
        {
            DrawSphere( g.anchor, g.centreRad, CentreRgb(), HandleAlpha( KGZ_CENTER ) );
            return;
        }
        float points[KGZ_CIRCLE_SEGMENTS][3];
        for ( int k = 0; k < KGZ_CIRCLE_SEGMENTS; ++k )
        {
            const float angle = KGZ_TWO_PI * (float)k / (float)KGZ_CIRCLE_SEGMENTS;
            for ( int a = 0; a < 3; ++a )
                points[k][a] = g.anchor[a]
                             + c->vright[a] * cosf( angle ) * g.centreRad
                             + c->vup[a] * sinf( angle ) * g.centreRad;
        }
        DrawRibbon( points, KGZ_CIRCLE_SEGMENTS, HandleWidth( KGZ_CENTER ), true,
                    c, CentreRgb(), HandleAlpha( KGZ_CENTER ) );
    }

    bool RingSegmentVisible( const ringGeo_t &g, const camera_s *c, int axis, int segment )
    {
        float a[3], b[3], mid[3];
        RingPoint( g, axis, segment, a );
        RingPoint( g, axis, ( segment + 1 ) % KGZ_CIRCLE_SEGMENTS, b );
        for ( int k = 0; k < 3; ++k )
            mid[k] = ( a[k] + b[k] ) * 0.5f - g.pivot[k];
        return Dot3( mid, c->vpn ) <= 0.0f;
    }

    void DrawAxisRing( const ringGeo_t &g, const camera_s *c, int axis )
    {
        bool visible[KGZ_CIRCLE_SEGMENTS];
        int visibleCount = 0;
        for ( int k = 0; k < KGZ_CIRCLE_SEGMENTS; ++k )
        {
            visible[k] = RingSegmentVisible( g, c, axis, k );
            if ( visible[k] )
                ++visibleCount;
        }
        float points[KGZ_CIRCLE_SEGMENTS][3];
        for ( int k = 0; k < KGZ_CIRCLE_SEGMENTS; ++k )
            RingPoint( g, axis, k, points[k] );

        if ( visibleCount == KGZ_CIRCLE_SEGMENTS )
        {
            DrawRibbon( points, KGZ_CIRCLE_SEGMENTS, RingWidth( axis ), true,
                        c, RingRgb( axis ), RingAlpha( axis ) );
            return;
        }

        for ( int start = 0; start < KGZ_CIRCLE_SEGMENTS; ++start )
        {
            const int prev = ( start + KGZ_CIRCLE_SEGMENTS - 1 ) % KGZ_CIRCLE_SEGMENTS;
            if ( !visible[start] || visible[prev] )
                continue;
            float arc[KGZ_CIRCLE_SEGMENTS + 1][3];
            int count = 0;
            int segment = start;
            Copy3( points[segment], arc[count++] );
            while ( visible[segment] && count <= KGZ_CIRCLE_SEGMENTS )
            {
                segment = ( segment + 1 ) % KGZ_CIRCLE_SEGMENTS;
                Copy3( points[segment], arc[count++] );
            }
            DrawRibbon( arc, count, RingWidth( axis ), false,
                        c, RingRgb( axis ), RingAlpha( axis ) );
        }
    }

    void DrawViewRing( const ringGeo_t &g, const camera_s *c )
    {
        float points[KGZ_CIRCLE_SEGMENTS][3];
        const float radius = g.radius * KGZ_VIEW_RING_MUL;
        for ( int k = 0; k < KGZ_CIRCLE_SEGMENTS; ++k )
        {
            const float angle = KGZ_TWO_PI * (float)k / (float)KGZ_CIRCLE_SEGMENTS;
            for ( int a = 0; a < 3; ++a )
                points[k][a] = g.pivot[a]
                             + c->vright[a] * cosf( angle ) * radius
                             + c->vup[a] * sinf( angle ) * radius;
        }
        DrawRibbon( points, KGZ_CIRCLE_SEGMENTS, KGZ_LINE_PIX, true,
                    c, KGZ_WHITE_RGB, KGZ_NORMAL_ALPHA );
    }

    void DrawAngleFeedback( const ringGeo_t &g, const camera_s *c )
    {
        if ( s_ringGrab < 0 || fabsf( s_ringTotal ) < 0.05f )
            return;
        float sweep = fmodf( s_ringTotal, 360.0f );
        if ( fabsf( sweep ) < 0.05f )
            sweep = s_ringTotal < 0.0f ? -360.0f : 360.0f;
        int segments = (int)ceilf( fabsf( sweep ) * (float)KGZ_CIRCLE_SEGMENTS / 360.0f );
        if ( segments < 1 ) segments = 1;
        if ( segments > KGZ_CIRCLE_SEGMENTS ) segments = KGZ_CIRCLE_SEGMENTS;

        MeshBegin( KGZ_AXIS_HOVER_RGB[s_ringGrab], KGZ_SECTOR_ALPHA );
        const int centre = MeshVertex( g.pivot );
        float point[3];
        RingPointAngle( g, s_ringGrab, s_ringStart / KGZ_DEGREES, 1.0f, point );
        int previous = MeshVertex( point );
        for ( int k = 1; k <= segments; ++k )
        {
            const float degrees = s_ringStart + sweep * (float)k / (float)segments;
            RingPointAngle( g, s_ringGrab, degrees / KGZ_DEGREES, 1.0f, point );
            const int next = MeshVertex( point );
            MeshTriangle( centre, previous, next );
            previous = next;
        }
        MeshFlush();

        const float ticks[2] = { s_ringStart, s_ringStart + sweep };
        for ( int t = 0; t < 2; ++t )
        {
            float line[2][3];
            RingPointAngle( g, s_ringGrab, ticks[t] / KGZ_DEGREES, 0.72f, line[0] );
            RingPointAngle( g, s_ringGrab, ticks[t] / KGZ_DEGREES, 1.08f, line[1] );
            DrawRibbon( line, 2, KGZ_LINE_HOVER_PIX, false, c,
                        KGZ_AXIS_HOVER_RGB[s_ringGrab], KGZ_HOVER_ALPHA );
        }
    }

    void SortDescending( const float score[3], int order[3] )
    {
        order[0] = 0; order[1] = 1; order[2] = 2;
        for ( int i = 0; i < 2; ++i )
            for ( int j = i + 1; j < 3; ++j )
                if ( score[order[j]] > score[order[i]] )
                {
                    const int swap = order[i]; order[i] = order[j]; order[j] = swap;
                }
    }

    void GizmoDepthClear()
    {
        static const float unusedColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        R_AddCmdClearScreen( 6, unusedColor, 1.0f, 0 );
    }

    void DrawLinear( const gizmoGeo_t &g, const camera_s *c )
    {
        GizmoDepthClear();

        float planeScore[3];
        for ( int n = 0; n < 3; ++n )
        {
            const int i = ( n + 1 ) % 3;
            const int j = ( n + 2 ) % 3;
            planeScore[n] = c->vpn[i] + c->vpn[j];
        }
        int planeOrder[3];
        SortDescending( planeScore, planeOrder );
        for ( int k = 0; k < 3; ++k )
        {
            const int n = planeOrder[k];
            float q[4][3];
            PlaneSquare( g, n, q );
            DrawQuad( q, PlaneRgb( n ), HandleAlpha( KGZ_PLANE_X + n ) );
        }

        float axisScore[3] = { c->vpn[0], c->vpn[1], c->vpn[2] };
        int axisOrder[3];
        SortDescending( axisScore, axisOrder );
        for ( int k = 0; k < 3; ++k )
            DrawAxisHandle( g, c, axisOrder[k] );

        if ( g.hasNormal )
            DrawNormalHandle( g, c );
        DrawCentre( g, c );
        MeshPassEnd();
    }

    void DrawRotate( const ringGeo_t &g, const camera_s *c )
    {
        GizmoDepthClear();
        DrawAngleFeedback( g, c );

        for ( int axis = 0; axis < 3; ++axis )
            if ( axis != s_ringGrab )
                DrawAxisRing( g, c, axis );
        if ( s_ringGrab >= 0 )
            DrawAxisRing( g, c, s_ringGrab );
        DrawViewRing( g, c );
        MeshPassEnd();
    }

    float Distance2D( float ax, float ay, float bx, float by )
    {
        const float x = ax - bx, y = ay - by;
        return sqrtf( x * x + y * y );
    }

    float SegmentDistance2D( float px, float py, float ax, float ay,
                             float bx, float by, float *outT )
    {
        return Pick_SegDist2D( px, py, ax, ay, bx, by, outT );
    }

    float Cross2D( float px, float py, float ax, float ay, float bx, float by )
    {
        return ( px - bx ) * ( ay - by ) - ( ax - bx ) * ( py - by );
    }

    bool PointInTriangle2D( float px, float py, const float x[3], const float y[3] )
    {
        const float area = ( x[1] - x[0] ) * ( y[2] - y[0] ) - ( y[1] - y[0] ) * ( x[2] - x[0] );
        if ( fabsf( area ) < 0.01f )
            return false;
        const float a = Cross2D( px, py, x[0], y[0], x[1], y[1] );
        const float b = Cross2D( px, py, x[1], y[1], x[2], y[2] );
        const float c = Cross2D( px, py, x[2], y[2], x[0], y[0] );
        const bool hasNegative = a < 0.0f || b < 0.0f || c < 0.0f;
        const bool hasPositive = a > 0.0f || b > 0.0f || c > 0.0f;
        return !( hasNegative && hasPositive );
    }

    bool PointInProjectedQuad( float px, float py, float x[4], float y[4] )
    {
        const float x0[3] = { x[0], x[1], x[2] };
        const float y0[3] = { y[0], y[1], y[2] };
        const float x1[3] = { x[0], x[2], x[3] };
        const float y1[3] = { y[0], y[2], y[3] };
        if ( PointInTriangle2D( px, py, x0, y0 ) || PointInTriangle2D( px, py, x1, y1 ) )
            return true;
        for ( int k = 0; k < 4; ++k )
        {
            const int next = ( k + 1 ) & 3;
            if ( SegmentDistance2D( px, py, x[k], y[k], x[next], y[next], 0 ) <= KGZ_SOLID_PICK_PAD )
                return true;
        }
        return false;
    }

    int LinearHitTest( const gizmoGeo_t &g, int imgX, int imgY )
    {
        const float px = (float)imgX, py = (float)imgY;
        float anchorX, anchorY;
        if ( !Pick_WorldToImage( g.anchor, &anchorX, &anchorY ) )
            return KGZ_NONE;

        const float centrePick = g.kind == KGZ_SCALE
                               ? KGZ_SCALE_BALL_PIX + KGZ_SOLID_PICK_PAD
                               : KGZ_CENTER_PICK_PIX;
        if ( Distance2D( px, py, anchorX, anchorY ) <= centrePick )
            return KGZ_CENTER;

        for ( int normalAxis = 0; normalAxis < 3; ++normalAxis )
        {
            float q[4][3], x[4], y[4];
            PlaneSquare( g, normalAxis, q );
            bool projected = true;
            for ( int k = 0; k < 4; ++k )
                projected = projected && Pick_WorldToImage( q[k], &x[k], &y[k] );
            if ( projected && PointInProjectedQuad( px, py, x, y ) )
                return KGZ_PLANE_X + normalAxis;
        }

        int best = KGZ_NONE;
        float bestDistance = 1000000.0f;
        if ( g.hasNormal )
        {
            float tip[3], base[3], bx, by, tx, ty;
            NormalTip( g, tip );
            Mad3( tip, g.normal, -g.headLen, base );
            if ( Pick_WorldToImage( base, &bx, &by ) && Pick_WorldToImage( tip, &tx, &ty ) )
            {
                const float d = SegmentDistance2D( px, py, bx, by, tx, ty, 0 );
                if ( d <= KGZ_HEAD_RAD_PIX + KGZ_SOLID_PICK_PAD )
                {
                    best = KGZ_NORMAL;
                    bestDistance = d;
                }
            }
        }

        for ( int axis = 0; axis < 3; ++axis )
        {
            float direction[3]; AxisVector( axis, direction );
            float tip[3]; AxisTip( g, axis, tip );
            const float solidBack = g.kind == KGZ_MOVE
                                  ? g.axisLen - g.headLen
                                  : g.axisLen - KGZ_SCALE_BOX_PIX * g.wpp * 0.5f;
            float base[3]; Mad3( g.anchor, direction, solidBack, base );
            float bx, by, tx, ty;
            if ( !Pick_WorldToImage( base, &bx, &by ) || !Pick_WorldToImage( tip, &tx, &ty ) )
                continue;

            const float solidRadius = g.kind == KGZ_MOVE
                                    ? KGZ_HEAD_RAD_PIX + KGZ_SOLID_PICK_PAD
                                    : KGZ_SCALE_BOX_PIX * 0.866025f + KGZ_SOLID_PICK_PAD;
            const float solidDistance = SegmentDistance2D( px, py, bx, by, tx, ty, 0 );
            if ( solidDistance <= solidRadius && solidDistance <= bestDistance )
            {
                best = KGZ_AXIS_X + axis;
                bestDistance = solidDistance;
            }

            float t = 0.0f;
            const float shaftDistance = SegmentDistance2D( px, py, anchorX, anchorY, bx, by, &t );
            if ( t >= KGZ_AXIS_MIN_T && shaftDistance <= KGZ_SHAFT_PICK_PIX && shaftDistance <= bestDistance )
            {
                best = KGZ_AXIS_X + axis;
                bestDistance = shaftDistance;
            }
        }
        return best;
    }

    int RingHitTest( const ringGeo_t &g, int imgX, int imgY )
    {
        const float px = (float)imgX, py = (float)imgY;
        int best = -1;
        float bestDistance = KGZ_RING_PICK_PIX;
        for ( int axis = 0; axis < 3; ++axis )
        {
            float a[3]; RingPoint( g, axis, 0, a );
            float ax, ay;
            bool haveA = Pick_WorldToImage( a, &ax, &ay );
            for ( int k = 1; k <= KGZ_CIRCLE_SEGMENTS; ++k )
            {
                float b[3]; RingPoint( g, axis, k % KGZ_CIRCLE_SEGMENTS, b );
                float bx, by;
                const bool haveB = Pick_WorldToImage( b, &bx, &by );
                if ( haveA && haveB )
                {
                    const float distance = SegmentDistance2D( px, py, ax, ay, bx, by, 0 );
                    if ( distance <= bestDistance )
                    {
                        bestDistance = distance;
                        best = axis;
                    }
                }
                ax = bx; ay = by; haveA = haveB;
            }
        }
        return best;
    }

    // The scale command's constraint, read back from its HUD line: 0 = uniform,
    // 1 = one axis, 2 = the plane across `*axis` (the two other axes).
    int ScaleHudConstraint( int *axis )
    {
        *axis = -1;
        KiwiEditorCommand *cmd = KiwiCmd_Active();
        const char *hud = cmd ? cmd->HudStatus() : 0;
        if ( !hud )
            return 0;
        int mode = 1;
        const char *tag = strstr( hud, "axis " );
        if ( !tag )
        {
            tag  = strstr( hud, "plane " );
            mode = 2;
        }
        if ( !tag )
            return 0;
        tag = strchr( tag, ' ' ) + 1;
        if      ( *tag == 'X' ) *axis = 0;
        else if ( *tag == 'Y' ) *axis = 1;
        else if ( *tag == 'Z' ) *axis = 2;
        else return 0;
        return mode;
    }

    // X/Y/Z key = that axis, Shift (mods bit 0) + key = the plane across it; the same
    // constraint again releases to uniform (HandleAxisKey in kiwi_transform.cpp).
    void AimScaleAxis( int axis )
    {
        KiwiEditorCommand *cmd = KiwiCmd_Active();
        int cur;
        if ( !cmd || axis < 0 || axis > 2 || ( ScaleHudConstraint( &cur ) == 1 && cur == axis ) )
            return;
        cmd->KeyDown( 0x58 + axis, 0 );
    }

    void AimScalePlane( int axis )
    {
        KiwiEditorCommand *cmd = KiwiCmd_Active();
        int cur;
        if ( !cmd || axis < 0 || axis > 2 || ( ScaleHudConstraint( &cur ) == 2 && cur == axis ) )
            return;
        cmd->KeyDown( 0x58 + axis, 1 );
    }

    void AimScaleUniform()
    {
        KiwiEditorCommand *cmd = KiwiCmd_Active();
        int cur;
        const int mode = ScaleHudConstraint( &cur );
        if ( cmd && mode != 0 && cur >= 0 )
            cmd->KeyDown( 0x58 + cur, mode == 2 ? 1u : 0u );   // repeat = release
    }

    void PushRingAngle()
    {
        KiwiXform_FeedRotateDegrees( true, -s_ringTotal );
    }
}

bool KiwiGizmo_Show()
{
    if ( s_show < 0 )
        s_show = Radiant_ProfileGetInt( "KiwiUX", "ShowGizmo", 1 ) ? 1 : 0;
    return s_show != 0;
}

void KiwiGizmo_SetShow( bool on )
{
    const int value = on ? 1 : 0;
    if ( s_show == value )
        return;
    s_show = value;
    Radiant_ProfileSetInt( "KiwiUX", "ShowGizmo", value );
    g_nUpdateBits |= 1;
}

void KiwiGizmo_Hover( int imgX, int imgY, bool over )
{
    const int oldHot = s_hot;
    const int oldRingHot = s_ringHot;
    s_hot = KGZ_NONE;
    s_ringHot = -1;

    if ( over && s_grabbed == KGZ_NONE && s_ringGrab < 0 )
    {
        CamWnd_BuildMatrix();
        gizmoGeo_t linear;
        if ( BuildLinear( &linear ) )
            s_hot = LinearHitTest( linear, imgX, imgY );
        else
        {
            ringGeo_t rings;
            if ( BuildRings( &rings ) )
                s_ringHot = RingHitTest( rings, imgX, imgY );
        }
    }
    if ( oldHot != s_hot || oldRingHot != s_ringHot )
        g_nUpdateBits |= 1;
}

bool KiwiGizmo_MouseDown( int imgX, int imgY )
{
    if ( !KiwiCmd_Active() )
        return false;
    CamWnd_BuildMatrix();

    gizmoGeo_t linear;
    if ( BuildLinear( &linear ) )
    {
        const int hit = LinearHitTest( linear, imgX, imgY );
        if ( hit == KGZ_NONE || !KiwiCmd_HandleGrab( imgX, imgY ) )
            return false;

        if ( linear.kind == KGZ_MOVE )
        {
            if ( hit >= KGZ_AXIS_X && hit <= KGZ_AXIS_Z )
                KiwiXform_PresetMoveConstraint( KIWI_XCON_AXIS, hit - KGZ_AXIS_X );
            else if ( hit >= KGZ_PLANE_X && hit <= KGZ_PLANE_Z )
                KiwiXform_PresetMoveConstraint( KIWI_XCON_PLANE, hit - KGZ_PLANE_X );
            else
                KiwiXform_PresetMoveConstraint( KIWI_XCON_FREE, 0 );
        }
        else
        {
            if ( hit >= KGZ_AXIS_X && hit <= KGZ_AXIS_Z )
                AimScaleAxis( hit - KGZ_AXIS_X );          // one axis
            else if ( hit >= KGZ_PLANE_X && hit <= KGZ_PLANE_Z )
                AimScalePlane( hit - KGZ_PLANE_X );        // two axes (the square's plane)
            else
                AimScaleUniform();                         // the centre ball
        }

        s_grabbed = hit;
        s_hot = hit;
        g_nUpdateBits |= 1;
        return true;
    }

    ringGeo_t rings;
    if ( BuildRings( &rings ) )
    {
        const int axis = RingHitTest( rings, imgX, imgY );
        float angle = 0.0f;
        if ( axis < 0 || !RingCursorAngle( rings, axis, imgX, imgY, &angle ) )
            return false;
        if ( !KiwiCmd_HandleGrab( imgX, imgY ) )
            return false;

        KiwiXform_PresetRotateAxis( axis );
        s_ringGrab  = axis;
        s_ringHot   = axis;
        s_ringStart = angle;
        s_ringPrev  = angle;
        s_ringTotal = 0.0f;
        PushRingAngle();
        g_nUpdateBits |= 1;
        return true;
    }
    return false;
}

void KiwiGizmo_Drag( int imgX, int imgY )
{
    if ( s_ringGrab < 0 )
        return;
    if ( !KiwiXform_IsRotateActive() )
    {
        s_ringGrab = -1;
        return;
    }

    CamWnd_BuildMatrix();
    ringGeo_t rings;
    float angle = 0.0f;
    if ( !BuildRings( &rings ) || !RingCursorAngle( rings, s_ringGrab, imgX, imgY, &angle ) )
        return;
    float delta = angle - s_ringPrev;
    while ( delta > 180.0f ) delta -= 360.0f;
    while ( delta < -180.0f ) delta += 360.0f;
    s_ringTotal += delta;
    s_ringPrev = angle;
    PushRingAngle();
    g_nUpdateBits |= 1;
}

bool KiwiGizmo_Grabbed()
{
    return s_grabbed != KGZ_NONE || s_ringGrab >= 0;
}

void KiwiGizmo_Release()
{
    if ( !KiwiGizmo_Grabbed() )
        return;
    const bool wasRing = s_ringGrab >= 0;
    s_grabbed = KGZ_NONE;
    s_ringGrab = -1;
    KiwiCmd_HandleRelease();
    if ( wasRing )
        KiwiXform_FeedRotateDegrees( false, 0.0f );
    if ( KiwiCmd_Active() )
        KiwiCmd_Pause();
}

void KiwiGizmo_Abort()
{
    if ( !KiwiGizmo_Grabbed() )
        return;
    const bool wasRing = s_ringGrab >= 0;
    s_grabbed = KGZ_NONE;
    s_ringGrab = -1;
    KiwiCmd_HandleRelease();
    if ( wasRing )
        KiwiXform_FeedRotateDegrees( false, 0.0f );
    if ( KiwiCmd_Active() )
        KiwiCmd_Cancel();
}

void KiwiGizmo_DrawWorld()
{
    camera_s *c = Ed_Camera();
    if ( !c || c->width < 1 || c->height < 1 )
        return;
    CamWnd_BuildMatrix();

    gizmoGeo_t linear;
    if ( BuildLinear( &linear ) )
    {
        DrawLinear( linear, c );
        return;
    }

    ringGeo_t rings;
    if ( BuildRings( &rings ) )
        DrawRotate( rings, c );
}

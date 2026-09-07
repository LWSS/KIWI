#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Editor-only construction geometry, drawing tools, construction planes, and
// `.kiwi` sidecar persistence. Point objects store WORLD-SPACE xyz triples;
// circles/arcs store a parametric plane.
//
// KIWI2 records `object`, optional `plane` for circle/arc, `closed`, WORLD-SPACE
// `wpt`, parametric `arc`, and `end`. Unknown fields/types are skipped.
// KIWI1 `pt u v` records still load by conversion through the stored plane.
// Malformed sidecars warn and are ignored without blocking map load.
#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "prefs.h"          // g_PrefsDlg (camera_fov — the screen-scaled point marker)

#include <imgui/imgui.h>

#include "kiwi_camera.h"
#include "kiwi_construct.h"
#include "kiwi_command.h"
#include "kiwi_conselect.h"
#include "kiwi_fillet.h"
#include "kiwi_grid.h"
#include "kiwi_hover.h"
#include "kiwi_lines.h"
#include "kiwi_offset.h"
#include "kiwi_numeric.h"
#include "kiwi_pick.h"
#include "kiwi_region.h"
#include "kiwi_refimage.h"
#include "kiwi_snap.h"
#include "kiwi_trim.h"
#include "kiwi_undo.h"
#include "kiwi_units.h"
#include "kiwi_viewcube.h"
#include "kiwi_visibility.h"
#include "radiant_registry.h"
#include "kiwi_vec.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Ported entry points, with definition anchors.
extern int       Sys_Printf( const char *fmt, ... );          // win_qe3.cpp:118
extern camera_s *Ed_Camera();                                 // camwnd.cpp:161
extern void      CamWnd_BuildMatrix();                        // camwnd.cpp:210 (0x403470)
extern int       g_nUpdateBits;                               // engine_stubs.cpp:773 (0x25D5A74)
extern bool      ImGuiShell_CameraPaintCursor( int *x, int *y, int *w, int *h );  // imgui_shell.cpp:297
extern bool      Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );  // mainfrm.cpp:1358
extern void      Radiant_ExecCommand( unsigned int cmdId );   // mainfrm.cpp:4054

namespace
{
    const char *KCON_SECTION = "KiwiUX";
    int         s_showConstruction = -1;          // -1 = not loaded yet

    // Rose distinguishes editor-only construction geometry from other overlays.
    const float KCON_COL_LINE  [3] = { 1.00f, 0.35f, 0.70f };
    const float KCON_COL_POINT [3] = { 1.00f, 0.62f, 0.85f };
    const float KCON_COL_ACTIVE[3] = { 1.00f, 0.85f, 0.45f };   // the in-progress chain
    // Selection brightens the rose palette toward white.
    const float KCON_COL_SEL_LINE [3] = { 1.00f, 0.88f, 0.94f };
    const float KCON_COL_SEL_POINT[3] = { 1.00f, 1.00f, 1.00f };
    // Match the shared outliner hover cyan.
    const float KCON_COL_HOVER[3] = { 0.35f, 0.95f, 1.00f };
    // Construction store and whole-store undo snapshots.
    std::vector<kconObject_t>                s_objects;
    std::vector< std::vector<kconObject_t> > s_undo;
    // Redo storage is driven by the unified journal.
    std::vector< std::vector<kconObject_t> > s_redo;
    unsigned                                 s_generation = 1;

    // Group ids are stable handles, not vector indices.
    const int KCON_GROUPNAME_MAX = 64;
    struct kconGroup_t
    {
        int  id;
        char name[KCON_GROUPNAME_MAX];
    };
    std::vector<kconGroup_t> s_groups;
    int                      s_nextGroupId = 1;

    int GroupSlot( int id )
    {
        if ( id < 0 )
            return -1;
        for ( size_t i = 0; i < s_groups.size(); ++i )
            if ( s_groups[i].id == id )
                return (int)i;
        return -1;
    }

    kconPlane_t s_plane;                          // the ACTIVE construction plane
    bool        s_planeSeeded = false;
    // Only user-made gestures latch an explicit plane; otherwise tool starts
    // recompute a memoryless default. s_planeDesc feeds the visible plane chip.
    bool        s_planeExplicit = false;
    char        s_planeDesc[64] = { 0 };
    // A selected face becomes explicit only through this one-shot creation arm;
    // routine Face-mode selection must not silently latch a working plane.
    bool        s_selFacePlaneArmed = false;

    void Touch()
    {
        ++s_generation;
        KiwiRegion_Invalidate();
        g_nUpdateBits |= 1;
    }

    // Round full-circle counts up to a multiple of four so every 90-degree
    // cardinal is a vertex; never reduce a requested smoothness.
    int CardinalSegs( int n )
    {
        if ( n < 4 )
            n = 4;
        n = ( ( n + 3 ) / 4 ) * 4;
        if ( n > KCON_SEGS_MAX )
            n = ( KCON_SEGS_MAX / 4 ) * 4;
        return n;
    }

    // How many segments a full circle of this radius gets (kiwi_construct.h).
    int CircleSegs( float radius )
    {
        int n = (int)floorf( fabsf( radius ) * KCON_SEGS_PER_UNIT + 0.5f );
        if ( n < KCON_SEGS_MIN ) n = KCON_SEGS_MIN;
        if ( n > KCON_SEGS_MAX ) n = KCON_SEGS_MAX;
        return CardinalSegs( n );
    }

    // Shared tessellated preview walk; false when the line budget is exhausted.
    bool DrawObjectPreview( const kconObject_t &preview )
    {
        const int segs = KiwiCon_SegmentCount( preview );
        for ( int i = 0; i < segs; ++i )
        {
            float a[3], b[3];
            if ( !KiwiCon_SegmentWorld( preview, i, a, b ) )
                break;
            if ( !KiwiLines_Add( a, b ) )
                return false;
        }
        return true;
    }

    // Use the longest WORLD-SPACE edge as the plane-u hint. The scan must wrap
    // through the closing edge; false means no edge exceeds the degeneracy floor.
    bool LongestEdgeHint( const float *pts, int count, float out[3] )
    {
        out[0] = out[1] = out[2] = 0.0f;
        if ( !pts || count < 2 )
            return false;
        float bestLen = 0.0f;
        for ( int i = 0; i < count; ++i )
        {
            float e[3];
            Sub3( &pts[(size_t)( ( i + 1 ) % count ) * 3], &pts[(size_t)i * 3], e );
            const float l = Len3( e );
            if ( l > bestLen ) { bestLen = l; Copy3( e, out ); }
        }
        return bestLen > 1.0e-3f;
    }

    // Shared CCW plane-space rectangle emission and drawing.
    void EmitRectQuad( const kconPlane_t &plane, const float uv[4][2], kconObject_t *o )
    {
        for ( int i = 0; i < 4; ++i )
        {
            float w[3];
            KiwiCon_PlaneToWorld( plane, uv[i], w );
            o->pts.push_back( w[0] );  o->pts.push_back( w[1] );  o->pts.push_back( w[2] );
        }
    }

    void DrawRectQuad( const kconPlane_t &plane, const float uv[4][2] )
    {
        for ( int i = 0; i < 4; ++i )
        {
            float w0[3], w1[3];
            KiwiCon_PlaneToWorld( plane, uv[i], w0 );
            KiwiCon_PlaneToWorld( plane, uv[( i + 1 ) & 3], w1 );
            if ( !KiwiLines_Add( w0, w1 ) )
                return;
        }
    }

    // Circle/arc override bounds; polygon bounds remain intentionally separate.
    int ClampSides( int n )
    {
        if ( n < KCON_SIDES_MIN ) n = KCON_SIDES_MIN;
        if ( n > KCON_SEGS_MAX  ) n = KCON_SEGS_MAX;
        return n;
    }

    // Per-object circle/arc segment override; otherwise use the radius rule.
    int CircleSegsFor( const kconObject_t &o )
    {
        if ( o.segs <= 0 )
            return CircleSegs( o.radius );
        int n = ClampSides( o.segs );
        // Round the full-circle density before an arc prorates it by sweep.
        return CardinalSegs( n );
    }

    // An arc gets the same density pro-rata over its own sweep, never below 2.
    int ArcSegs( const kconObject_t &o )
    {
        const float sweep = fabsf( o.ang1 - o.ang0 );
        int n = (int)floorf( (float)CircleSegsFor( o ) * ( sweep / 360.0f ) + 0.5f );
        if ( n < 2 ) n = 2;
        if ( n > KCON_SEGS_MAX ) n = KCON_SEGS_MAX;
        return n;
    }

    void ArcPointUV( const kconObject_t &o, float t, float outUV[2] )
    {
        const float deg = o.ang0 + ( o.ang1 - o.ang0 ) * t;
        const float rad = deg * KCON_DEG2RAD;
        outUV[0] = o.centre[0] + cosf( rad ) * o.radius;
        outUV[1] = o.centre[1] + sinf( rad ) * o.radius;
    }

    // Uniform Catmull-Rom in WORLD SPACE. Open endpoints clamp, closed endpoints
    // wrap; the store receives a straight-segment KCON_POLYLINE tessellation.
    void SplinePoint3( const float *p0, const float *p1, const float *p2,
                       const float *p3, float t, float out[3] )
    {
        const float t2 = t * t;
        const float t3 = t2 * t;
        for ( int k = 0; k < 3; ++k )
        {
            out[k] = 0.5f * ( ( 2.0f * p1[k] )
                            + ( -p0[k] + p2[k] ) * t
                            + ( 2.0f * p0[k] - 5.0f * p1[k] + 4.0f * p2[k] - p3[k] ) * t2
                            + ( -p0[k] + 3.0f * p1[k] - 3.0f * p2[k] + p3[k] ) * t3 );
        }
    }

    // WORLD-SPACE xyz controls; coarsen rather than truncate at KCON_MAX_POINTS.
    void TessellateSpline( const std::vector<float> &ctrl, bool closed,
                           std::vector<float> *out )
    {
        out->clear();
        const int n = (int)( ctrl.size() / 3 );
        if ( n < 2 )
            return;
        if ( n == 2 && !closed )                  // two points is a line, not a curve
        {
            out->assign( ctrl.begin(), ctrl.end() );
            return;
        }

        const int spans = closed ? n : ( n - 1 );
        int segs = KCON_SPLINE_SEGS;
        while ( segs > 1 && spans * segs + 1 > KCON_MAX_POINTS )
            --segs;

        for ( int s = 0; s < spans; ++s )
        {
            const int i0 = closed ? ( ( s - 1 + n ) % n ) : ( ( s - 1 < 0 ) ? 0 : s - 1 );
            const int i1 = s;
            const int i2 = closed ? ( ( s + 1 ) % n ) : ( ( s + 1 > n - 1 ) ? n - 1 : s + 1 );
            const int i3 = closed ? ( ( s + 2 ) % n ) : ( ( s + 2 > n - 1 ) ? n - 1 : s + 2 );
            for ( int k = 0; k < segs; ++k )
            {
                float w[3];
                SplinePoint3( &ctrl[(size_t)i0 * 3], &ctrl[(size_t)i1 * 3],
                              &ctrl[(size_t)i2 * 3], &ctrl[(size_t)i3 * 3],
                              (float)k / (float)segs, w );
                out->push_back( w[0] );
                out->push_back( w[1] );
                out->push_back( w[2] );
            }
        }
        if ( !closed )                            // land exactly on the last click
        {
            out->push_back( ctrl[( (size_t)n - 1 ) * 3 + 0] );
            out->push_back( ctrl[( (size_t)n - 1 ) * 3 + 1] );
            out->push_back( ctrl[( (size_t)n - 1 ) * 3 + 2] );
        }
    }
}

// Plane math.
void KiwiCon_PlaneToWorld( const kconPlane_t &p, const float uv[2], float out[3] )
{
    for ( int k = 0; k < 3; ++k )
        out[k] = p.origin[k] + p.u[k] * uv[0] + p.v[k] * uv[1];
}

void KiwiCon_WorldToPlane( const kconPlane_t &p, const float world[3], float outUV[2] )
{
    float rel[3];
    Sub3( world, p.origin, rel );
    outUV[0] = Dot3( rel, p.u );
    outUV[1] = Dot3( rel, p.v );
}

bool KiwiCon_RayPlane( const kconPlane_t &p, const ray_t &ray, float outWorld[3] )
{
    const float den = Dot3( ray.dir, p.normal );
    if ( fabsf( den ) < 1.0e-5f )
        return false;
    float rel[3];
    Sub3( p.origin, ray.origin, rel );
    const float t = Dot3( rel, p.normal ) / den;
    if ( !( t > 0.0f ) || t > 1.0e6f )
        return false;
    Mad3( ray.origin, ray.dir, t, outWorld );
    return true;
}

// Finite ray/plane placement; see the declaration for the clamp contract.
bool KiwiCon_RayPlaneBounded( const kconPlane_t &p, const ray_t &ray, float outWorld[3] )
{
    // A degenerate plane basis has no (u,v) to bound anything in.
    if ( !( Dot3( p.normal, p.normal ) > 0.5f ) )
        return false;

    float hit[3];
    bool  have = false;

    const float den = Dot3( ray.dir, p.normal );
    if ( fabsf( den ) >= 1.0e-5f )
    {
        float rel[3];
        Sub3( p.origin, ray.origin, rel );
        const float t = Dot3( rel, p.normal ) / den;
        if ( t > 0.0f )
        {
            Mad3( ray.origin, ray.dir, t, hit );
            have = true;
        }
    }

    if ( !have )
    {
        // Project the eye onto the plane, follow the ray's in-plane bearing,
        // then let the common quad clamp pin the result to the finite edge.
        float rel[3];
        Sub3( ray.origin, p.origin, rel );
        const float off = Dot3( rel, p.normal );
        for ( int k = 0; k < 3; ++k )
            hit[k] = ray.origin[k] - p.normal[k] * off;

        float d[3];
        const float dn = Dot3( ray.dir, p.normal );
        for ( int k = 0; k < 3; ++k )
            d[k] = ray.dir[k] - p.normal[k] * dn;
        if ( Norm3( d ) )
            for ( int k = 0; k < 3; ++k )
                hit[k] += d[k] * ( KCON_PLANE_REACH * 2.0f );   // past the edge; clamped below
    }

    // THE QUAD.  Clamp in the plane's own basis, so the bound is a square centred on
    // the plane origin — Plasticity's PlaneGeometry(10000, 10000), in plane space.
    float uv[2];
    KiwiCon_WorldToPlane( p, hit, uv );
    for ( int k = 0; k < 2; ++k )
    {
        if ( uv[k] >  KCON_PLANE_REACH ) uv[k] =  KCON_PLANE_REACH;
        if ( uv[k] < -KCON_PLANE_REACH ) uv[k] = -KCON_PLANE_REACH;
        if ( !( uv[k] == uv[k] ) )       uv[k] = 0.0f;          // NaN from a huge t
    }
    KiwiCon_PlaneToWorld( p, uv, outWorld );
    return true;
}

bool KiwiCon_MakePlane( const float origin[3], const float normal[3],
                        const float *hintU, kconPlane_t *out )
{
    if ( !out )
        return false;
    float n[3];
    Copy3( normal, n );
    if ( !Norm3( n ) )
        return false;

    // Fall back to the world axis least aligned with the normal; this keeps
    // axis-aligned plane grids on world axes.
    float u[3];
    bool haveU = false;
    if ( hintU )
    {
        Copy3( hintU, u );
        Mad3( u, n, -Dot3( u, n ), u );          // strip the out-of-plane part
        haveU = Norm3( u );
    }
    if ( !haveU )
    {
        int least = 0;
        for ( int k = 1; k < 3; ++k )
            if ( fabsf( n[k] ) < fabsf( n[least] ) )
                least = k;
        float ax[3] = { 0.0f, 0.0f, 0.0f };
        ax[least] = 1.0f;
        Cross3( n, ax, u );
        if ( !Norm3( u ) )
            return false;
    }

    float v[3];
    Cross3( n, u, v );                            // right-handed: u × v == n
    if ( !Norm3( v ) )
        return false;

    Copy3( origin, out->origin );
    Copy3( n, out->normal );
    Copy3( u, out->u );
    Copy3( v, out->v );
    return true;
}

// Canonical bearing basis derives from the normal alone; see the header.
void KiwiCon_BearingBasis( const float normal[3], float outU[3], float outV[3] )
{
    float n[3];
    Copy3( normal, n );
    if ( !Norm3( n ) )
    {
        n[0] = 0.0f;  n[1] = 0.0f;  n[2] = 1.0f;     // degenerate: treat it as ground
    }

    // The tolerant ground test (RectangleFactory.ts:81) — it covers a downward normal,
    // which PlaneSnap's exact `=== 1` does not.
    if ( fabsf( n[2] ) > 1.0f - 1.0e-4f )
    {
        outU[0] = 1.0f;  outU[1] = 0.0f;  outU[2] = 0.0f;      // world +X
    }
    else
    {
        const float worldZ[3] = { 0.0f, 0.0f, 1.0f };
        Cross3( worldZ, n, outU );                              // the in-plane horizontal
        if ( !Norm3( outU ) )
        {
            outU[0] = 1.0f;  outU[1] = 0.0f;  outU[2] = 0.0f;
        }
    }
    // Remove any residual normal component before deriving the vertical axis.
    Mad3( outU, n, -Dot3( outU, n ), outU );
    if ( !Norm3( outU ) )
    {
        outU[0] = 1.0f;  outU[1] = 0.0f;  outU[2] = 0.0f;
    }
    Cross3( n, outU, outV );                                    // 90 deg = UP the surface
    if ( !Norm3( outV ) )
    {
        outV[0] = 0.0f;  outV[1] = 1.0f;  outV[2] = 0.0f;
    }
}

float KiwiCon_WrapDeg( float deg )
{
    if ( !( deg == deg ) )                    // NaN in, 0 out
        return 0.0f;
    deg = fmodf( deg, 360.0f );
    if ( deg < 0.0f )
        deg += 360.0f;
    if ( deg >= 360.0f )                      // fmodf can land exactly on 360 after the add
        deg = 0.0f;
    return deg;
}

bool KiwiCon_BearingOf( const float normal[3], const float dir[3], float *outDeg,
                        float *inPlaneOut, float *outOfPlaneOut )
{
    float u[3], v[3];
    KiwiCon_BearingBasis( normal, u, v );
    const float du = Dot3( dir, u );
    const float dv = Dot3( dir, v );
    const float dn = Dot3( dir, normal );
    const float inPlane = sqrtf( du * du + dv * dv );
    if ( inPlaneOut )    *inPlaneOut    = inPlane;
    if ( outOfPlaneOut ) *outOfPlaneOut = dn;
    if ( inPlane < 1.0e-4f )
        return false;
    if ( outDeg )
        *outDeg = KiwiCon_WrapDeg( atan2f( dv, du ) * KCON_RAD2DEG );
    return true;
}

void KiwiCon_BearingDir( const float normal[3], float deg, float out[3] )
{
    float u[3], v[3];
    KiwiCon_BearingBasis( normal, u, v );
    const float rad = deg * KCON_DEG2RAD;
    const float cu  = cosf( rad ), sv = sinf( rad );
    for ( int k = 0; k < 3; ++k )
        out[k] = u[k] * cu + v[k] * sv;
}

// Snap against the WORLD-ORIGIN lattice, not the arbitrary plane origin.
void KiwiCon_SnapUV( const kconPlane_t &p, float uv[2] )
{
    const float s = KiwiUnits_GridSpacingWorld();
    if ( !( s > 0.0f ) )
        return;
    // Convert origin-relative uv to absolute plane coordinates, snap, then
    // convert back. This leaves the plane-normal component untouched.
    const float ou = Dot3( p.origin, p.u );
    const float ov = Dot3( p.origin, p.v );
    uv[0] = floorf( ( uv[0] + ou ) / s + 0.5f ) * s - ou;
    uv[1] = floorf( ( uv[1] + ov ) / s + 0.5f ) * s - ov;
}

// Derive a plane from WORLD-SPACE points.
bool KiwiCon_FitPlane( const float *worldPts, int count, kconPlane_t *out )
{
    if ( !worldPts || count < 3 || !out )
        return false;

    // Newell's area normal remains stable when early vertices are collinear.
    float n[3] = { 0.0f, 0.0f, 0.0f };
    float c[3] = { 0.0f, 0.0f, 0.0f };
    for ( int i = 0; i < count; ++i )
    {
        const float *a = &worldPts[(size_t)i * 3];
        const float *b = &worldPts[(size_t)( ( i + 1 ) % count ) * 3];
        n[0] += ( a[1] - b[1] ) * ( a[2] + b[2] );
        n[1] += ( a[2] - b[2] ) * ( a[0] + b[0] );
        n[2] += ( a[0] - b[0] ) * ( a[1] + b[1] );
        c[0] += a[0];  c[1] += a[1];  c[2] += a[2];
    }
    if ( !Norm3( n ) )
        return false;                             // collinear / zero area
    for ( int k = 0; k < 3; ++k )
        c[k] /= (float)count;

    // Longest-edge basis hint includes the closing edge.
    float longest[3];
    const bool haveHint = LongestEdgeHint( worldPts, count, longest );

    kconPlane_t p;
    if ( !KiwiCon_MakePlane( c, n, haveHint ? longest : 0, &p ) )
        return false;

    // Refuse genuinely non-planar point sets before region/extrusion consumers.
    for ( int i = 0; i < count; ++i )
    {
        float rel[3];
        Sub3( &worldPts[(size_t)i * 3], p.origin, rel );
        if ( fabsf( Dot3( rel, p.normal ) ) > KCON_PLANE_FIT_DIST )
            return false;
    }
    *out = p;
    return true;
}

bool KiwiCon_ObjectPlane( const kconObject_t &o, kconPlane_t *out )
{
    if ( !out )
        return false;
    if ( KiwiCon_IsParametric( o ) || o.planeValid )
    {
        *out = o.plane;
        return true;
    }
    return false;
}

float KiwiCon_SegSegClosest( const float a0[3], const float a1[3],
                             const float b0[3], const float b1[3],
                             float *outTa, float *outTb, float outMid[3] )
{
    // Clamped segment/segment closest approach (Ericson, RTCD §5.1.9).
    float d1[3], d2[3], r[3];
    Sub3( a1, a0, d1 );
    Sub3( b1, b0, d2 );
    Sub3( a0, b0, r );
    const float aa = Dot3( d1, d1 );
    const float e  = Dot3( d2, d2 );
    const float f  = Dot3( d2, r );

    float s = 0.0f, t = 0.0f;
    if ( aa <= 1.0e-8f && e <= 1.0e-8f )
    {
        s = t = 0.0f;                             // both degenerate: point vs point
    }
    else if ( aa <= 1.0e-8f )
    {
        t = f / e;
        if ( t < 0.0f ) t = 0.0f;  if ( t > 1.0f ) t = 1.0f;
    }
    else
    {
        const float cc = Dot3( d1, r );
        if ( e <= 1.0e-8f )
        {
            s = -cc / aa;
            if ( s < 0.0f ) s = 0.0f;  if ( s > 1.0f ) s = 1.0f;
        }
        else
        {
            const float b    = Dot3( d1, d2 );
            const float den  = aa * e - b * b;
            if ( den > 1.0e-8f )
            {
                s = ( b * f - cc * e ) / den;
                if ( s < 0.0f ) s = 0.0f;  if ( s > 1.0f ) s = 1.0f;
            }
            t = ( b * s + f ) / e;
            if ( t < 0.0f )
            {
                t = 0.0f;
                s = -cc / aa;
                if ( s < 0.0f ) s = 0.0f;  if ( s > 1.0f ) s = 1.0f;
            }
            else if ( t > 1.0f )
            {
                t = 1.0f;
                s = ( b - cc ) / aa;
                if ( s < 0.0f ) s = 0.0f;  if ( s > 1.0f ) s = 1.0f;
            }
        }
    }

    float pa[3], pb[3], diff[3];
    Mad3( a0, d1, s, pa );
    Mad3( b0, d2, t, pb );
    Sub3( pa, pb, diff );
    if ( outTa ) *outTa = s;
    if ( outTb ) *outTb = t;
    if ( outMid )
        for ( int k = 0; k < 3; ++k )
            outMid[k] = ( pa[k] + pb[k] ) * 0.5f;
    return Len3( diff );
}

// Tessellated geometry.
int KiwiCon_VertCount( const kconObject_t &o )
{
    if ( o.type == KCON_CIRCLE )
        return CircleSegsFor( o );                // closed: the wrap is implicit
    if ( o.type == KCON_ARC )
        return ArcSegs( o ) + 1;
    return (int)( o.pts.size() / 3 );                 // three floats per WORLD point
}

bool KiwiCon_VertWorld( const kconObject_t &o, int i, float out[3] )
{
    const int n = KiwiCon_VertCount( o );
    if ( i < 0 || i >= n )
        return false;
    if ( o.type == KCON_CIRCLE )
    {
        // A full ring regardless of ang0/ang1 — a circle IS 360°, and storing the
        // pair keeps one parametric block for both types.
        const float rad = ( (float)i / (float)n ) * KCON_TWO_PI;
        const float uv[2] = { o.centre[0] + cosf( rad ) * o.radius,
                              o.centre[1] + sinf( rad ) * o.radius };
        KiwiCon_PlaneToWorld( o.plane, uv, out );
        return true;
    }
    if ( o.type == KCON_ARC )
    {
        float uv[2];
        ArcPointUV( o, (float)i / (float)( n - 1 ), uv );
        KiwiCon_PlaneToWorld( o.plane, uv, out );
        return true;
    }
    Copy3( &o.pts[(size_t)i * 3], out );          // world, verbatim
    return true;
}

int KiwiCon_SegmentCount( const kconObject_t &o )
{
    const int n = KiwiCon_VertCount( o );
    if ( n < 2 )
        return 0;
    const bool closed = ( o.type == KCON_CIRCLE ) || ( o.type == KCON_RECT ) || o.closed;
    return closed ? n : ( n - 1 );
}

// Read text between the first and last quote; spaces are allowed, quotes are not.
// Missing quotes leave the field unnamed without failing the whole sidecar.
static bool KiwiCon_QuotedField( const char *line, char *out, int cap )
{
    if ( !out || cap < 1 )
        return false;
    out[0] = '\0';
    if ( !line )
        return false;
    const char *q0 = strchr( line, '"' );
    const char *q1 = q0 ? strrchr( line, '"' ) : 0;
    if ( !q0 || !q1 || q1 <= q0 )
        return false;
    int n = (int)( q1 - q0 - 1 );
    if ( n > cap - 1 )
        n = cap - 1;
    memcpy( out, q0 + 1, (size_t)n );
    out[n] = '\0';
    return true;
}

// Shared construction preview palette.
extern const float KCON_PREVIEW_OK [3] = { 1.00f, 0.55f, 0.72f };
extern const float KCON_PREVIEW_BAD[3] = { 1.00f, 0.22f, 0.18f };

bool KiwiCon_SegmentWorld( const kconObject_t &o, int i, float a[3], float b[3] )
{
    const int segs = KiwiCon_SegmentCount( o );
    if ( i < 0 || i >= segs )
        return false;
    const int n = KiwiCon_VertCount( o );
    return KiwiCon_VertWorld( o, i, a ) && KiwiCon_VertWorld( o, ( i + 1 ) % n, b );
}

// Selection-mode-independent construction segment scan.
bool KiwiCon_PickSegmentAt( int imgX, int imgY, float outA[3], float outB[3],
                            int *outObj, int *outSeg, float *outPixels )
{
    if ( !KiwiCon_ShowConstruction() )
        return false;                           // hidden geometry is not clickable

    const float curX = (float)imgX;
    const float curY = (float)imgY;
    float bestA[3] = { 0.0f, 0.0f, 0.0f };
    float bestB[3] = { 0.0f, 0.0f, 0.0f };
    float bestDist = 0.0f;
    int   bestObj  = -1;
    int   bestSeg  = -1;
    bool  have     = false;

    const int count = KiwiCon_Count();
    for ( int i = 0; i < count; ++i )
    {
        const kconObject_t *o = KiwiCon_At( i );
        // A hidden object is INERT, not merely invisible (kiwi_construct.h
        // HIDDEN) — it is not drawn, so it must not be pickable either.
        if ( !o || o->hidden )
            continue;
        const int segs = KiwiCon_SegmentCount( *o );
        for ( int s = 0; s < segs; ++s )
        {
            float wa[3], wb[3], ax, ay, bx, by;
            if ( !KiwiCon_SegmentWorld( *o, s, wa, wb ) )
                break;
            if ( !Pick_WorldToImage( wa, &ax, &ay ) || !Pick_WorldToImage( wb, &bx, &by ) )
                continue;                       // an end behind the eye — skip whole
            const float d = Pick_SegDist2D( curX, curY, ax, ay, bx, by, 0 );
            if ( d > KCON_LINE_PIXELS )
                continue;
            if ( have && d >= bestDist )
                continue;
            have = true;  bestDist = d;
            Copy3( wa, bestA );  Copy3( wb, bestB );
            bestObj = i;  bestSeg = s;
        }
    }
    if ( !have )
        return false;
    if ( outA )      Copy3( bestA, outA );
    if ( outB )      Copy3( bestB, outB );
    if ( outObj )    *outObj = bestObj;
    if ( outSeg )    *outSeg = bestSeg;
    if ( outPixels ) *outPixels = bestDist;
    return true;
}

int KiwiCon_AnchorCount( const kconObject_t &o )
{
    if ( o.type == KCON_CIRCLE ) return 5;        // centre + 4 quadrants
    if ( o.type == KCON_ARC )    return 3;        // centre + both ends
    return (int)( o.pts.size() / 3 );
}

bool KiwiCon_AnchorWorld( const kconObject_t &o, int i, float out[3] )
{
    if ( i < 0 || i >= KiwiCon_AnchorCount( o ) )
        return false;

    float uv[2];
    if ( o.type == KCON_CIRCLE )
    {
        if ( i == 0 ) { uv[0] = o.centre[0]; uv[1] = o.centre[1]; }
        else
        {
            const float rad = (float)( i - 1 ) * 1.570796327f;
            uv[0] = o.centre[0] + cosf( rad ) * o.radius;
            uv[1] = o.centre[1] + sinf( rad ) * o.radius;
        }
    }
    else if ( o.type == KCON_ARC )
    {
        if ( i == 0 ) { uv[0] = o.centre[0]; uv[1] = o.centre[1]; }
        else            ArcPointUV( o, ( i == 1 ) ? 0.0f : 1.0f, uv );
    }
    else
    {
        Copy3( &o.pts[(size_t)i * 3], out );
        return true;
    }
    KiwiCon_PlaneToWorld( o.plane, uv, out );
    return true;
}

// Construction store.
int KiwiCon_Count()
{
    return (int)s_objects.size();
}

const kconObject_t *KiwiCon_At( int index )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return 0;
    return &s_objects[index];
}

namespace
{
    // Refresh a point object's cached fit; parametric planes are authoritative.
    void RefitPlane( kconObject_t &o )
    {
        if ( KiwiCon_IsParametric( o ) )
        {
            o.planeValid = true;
            return;
        }
        const int n = (int)( o.pts.size() / 3 );
        o.planeValid = ( n >= 3 ) && KiwiCon_FitPlane( &o.pts[0], n, &o.plane );
    }

    void RefitAll()
    {
        for ( size_t i = 0; i < s_objects.size(); ++i )
            RefitPlane( s_objects[i] );
    }

    // Normalize on add/load only: collapse consecutive duplicates and a repeated
    // closed seam. Do not normalize mid-move, when coincident points may be transient.
    void NormalizePoints( kconObject_t &o )
    {
        if ( KiwiCon_IsParametric( o ) )
            return;
        const int n = (int)( o.pts.size() / 3 );
        if ( n < 2 )
            return;

        std::vector<float> out;
        out.reserve( o.pts.size() );
        for ( int i = 0; i < n; ++i )
        {
            const float *p = &o.pts[(size_t)i * 3];
            const int have = (int)( out.size() / 3 );
            if ( have > 0 )
            {
                const float dx = p[0] - out[(size_t)( have - 1 ) * 3 + 0];
                const float dy = p[1] - out[(size_t)( have - 1 ) * 3 + 1];
                const float dz = p[2] - out[(size_t)( have - 1 ) * 3 + 2];
                if ( sqrtf( dx * dx + dy * dy + dz * dz ) <= KREG_JOIN_DIST )
                    continue;
            }
            out.push_back( p[0] );  out.push_back( p[1] );  out.push_back( p[2] );
        }

        // Closed wraps are implicit, so remove only a closed chain's repeated seam;
        // an open chain may legally return to its start.
        const int m = (int)( out.size() / 3 );
        if ( o.closed && m >= 2 )
        {
            const float dx = out[0] - out[(size_t)( m - 1 ) * 3 + 0];
            const float dy = out[1] - out[(size_t)( m - 1 ) * 3 + 1];
            const float dz = out[2] - out[(size_t)( m - 1 ) * 3 + 2];
            if ( sqrtf( dx * dx + dy * dy + dz * dz ) <= KREG_JOIN_DIST )
                out.resize( (size_t)( m - 1 ) * 3 );
        }

        if ( (int)( out.size() / 3 ) != n )
            o.pts.swap( out );
    }

    // Reset document-global plane state directly to world-ground defaults.
    void ResetActivePlaneToGround()
    {
        const float o[3] = { 0.0f, 0.0f, 0.0f };
        const float n[3] = { 0.0f, 0.0f, 1.0f };
        if ( KiwiCon_MakePlane( o, n, 0, &s_plane ) )
            s_planeSeeded = true;
        // Explicit planes cannot outlive their document.
        s_planeExplicit = false;
        s_planeDesc[0]  = '\0';
    }
}

// Normalize and validate before an undo ticket is minted.
static bool AcceptForStore( const kconObject_t &in, kconObject_t *out )
{
    // Budgets apply to the normalized representation that will be stored.
    kconObject_t obj = in;
    NormalizePoints( obj );
    // A tool must never be able to push a runaway object into the store — the
    // draw pass, the snap scan and the region walk are all sized off these counts.
    if ( (int)( obj.pts.size() / 3 ) > KCON_MAX_POINTS )
    {
        Sys_Printf( "Construction: object rejected (over %i points).\n", KCON_MAX_POINTS );
        return false;
    }
    if ( KiwiCon_VertCount( obj ) < 2 )
    {
        Sys_Printf( "Construction: object rejected (fewer than 2 points).\n" );
        return false;
    }
    if ( out )
        *out = obj;
    return true;
}

int KiwiCon_Add( const kconObject_t &o )
{
    kconObject_t obj;
    if ( !AcceptForStore( o, &obj ) )
        return -1;
    s_objects.push_back( obj );
    RefitPlane( s_objects.back() );               // refresh the derived plane
    Touch();
    return (int)s_objects.size() - 1;
}

// Validate before pushing so a refused object creates no empty undo step.
int KiwiCon_AddWithUndo( const kconObject_t &o )
{
    kconObject_t obj;
    if ( !AcceptForStore( o, &obj ) )
        return -1;
    KiwiCon_UndoPush();
    return KiwiCon_Add( obj );
}

bool KiwiCon_RemoveAt( int index )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return false;
    s_objects.erase( s_objects.begin() + index );
    KiwiConSel_NoteStoreReplaced();       // later object indices shifted
    Touch();
    return true;
}

void KiwiCon_ClearAll()
{
    // Reset the hidden plane state even when the object/group stores are empty.
    ResetActivePlaneToGround();

    // Empty groups are persistent objects and must participate in the early-out.
    if ( s_objects.empty() && s_groups.empty() )
        return;
    s_objects.clear();
    // A scene clear removes its construction group table too.
    s_groups.clear();
    s_nextGroupId = 1;
    KiwiConSel_NoteStoreReplaced();
    Touch();
}

// A new document also discards construction/reference-image state and unified
// journal tickets; palette ClearAll intentionally keeps its just-pushed undo snapshot.
void KiwiCon_ResetForNewMap()
{
    KiwiCon_ClearAll();
    KiwiRefImage_ResetForNewMap(); // KIWI (REFIMG): File->New must discard the old map's planes.
    s_undo.clear();
    s_redo.clear();
    KiwiUndo_Reset();
}

// Hidden-state access.
bool KiwiCon_Hidden( int index )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return true;             // out of range reads as "not visible" — see the header
    return s_objects[index].hidden;
}

void KiwiCon_SetHidden( int index, bool hidden )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return;
    if ( s_objects[index].hidden == hidden )
        return;
    s_objects[index].hidden = hidden;
    // Geometry is unchanged, but region caches must drop hidden boundaries.
    Touch();
    // Hidden state must invalidate the cached camera image at the store boundary.
    g_nUpdateBits = -1;
}

// Per-object sidecar label.
const char *KiwiCon_Name( int index )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return "";
    return s_objects[index].name.c_str();
}

void KiwiCon_SetName( int index, const char *name )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return;
    std::string v = name ? name : "";
    if ( (int)v.size() > KCON_NAME_MAX - 1 )
        v.resize( KCON_NAME_MAX - 1 );
    // Strip quotes because the sidecar field has no escape syntax.
    for ( size_t i = 0; i < v.size(); )
        if ( v[i] == '"' ) v.erase( i, 1 ); else ++i;
    if ( s_objects[index].name == v )
        return;
    s_objects[index].name = v;
    Touch();
    g_nUpdateBits = -1;
}

bool KiwiCon_HasHidden()
{
    for ( size_t i = 0; i < s_objects.size(); ++i )
        if ( s_objects[i].hidden )
            return true;
    return false;
}

int KiwiCon_UnhideAll()
{
    int n = 0;
    for ( size_t i = 0; i < s_objects.size(); ++i )
        if ( s_objects[i].hidden )
            ++n;
    if ( !n )
        return 0;
    KiwiCon_UndoPush();                    // its own bracket — see the header
    for ( size_t i = 0; i < s_objects.size(); ++i )
        s_objects[i].hidden = false;
    Touch();
    g_nUpdateBits = -1;               // redraw all views
    return n;
}

// Construction group access.
int KiwiCon_Group( int index )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return -1;                       // out of range reads as UNGROUPED
    return s_objects[index].group;
}

void KiwiCon_SetGroup( int index, int group )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return;
    // An id nobody declared is not a group; treat it as "ungroup" rather than
    // letting a stray write mint a folder the name table knows nothing about.
    if ( group >= 0 && GroupSlot( group ) < 0 )
        group = -1;
    if ( s_objects[index].group == group )
        return;
    s_objects[index].group = group;
    // Only derived/outliner views changed; no plane refit is needed.
    Touch();
}

int KiwiCon_NewGroup( const char *name )
{
    kconGroup_t g;
    memset( &g, 0, sizeof( g ) );
    g.id = s_nextGroupId++;
    if ( name && name[0] )
        _snprintf( g.name, sizeof( g.name ), "%s", name );
    else
        _snprintf( g.name, sizeof( g.name ), "Group %i", g.id );
    g.name[sizeof( g.name ) - 1] = '\0';
    s_groups.push_back( g );
    Touch();
    return g.id;
}

int KiwiCon_GroupCount()
{
    return (int)s_groups.size();
}

int KiwiCon_GroupIdAt( int i )
{
    if ( i < 0 || i >= (int)s_groups.size() )
        return -1;
    return s_groups[i].id;
}

bool KiwiCon_GroupExists( int group )
{
    return group >= 0 && GroupSlot( group ) >= 0;
}

const char *KiwiCon_GroupName( int group )
{
    const int slot = GroupSlot( group );
    return ( slot < 0 ) ? "" : s_groups[slot].name;
}

void KiwiCon_SetGroupName( int group, const char *name )
{
    const int slot = GroupSlot( group );
    if ( slot < 0 || !name || !name[0] )
        return;                          // an empty rename is a cancel, not a blank
    _snprintf( s_groups[slot].name, KCON_GROUPNAME_MAX, "%s", name );
    s_groups[slot].name[KCON_GROUPNAME_MAX - 1] = '\0';
    Touch();
}

bool KiwiCon_RemoveGroup( int group )
{
    const int slot = GroupSlot( group );
    if ( slot < 0 )
        return false;
    for ( size_t i = 0; i < s_objects.size(); ++i )
        if ( s_objects[i].group == group )
            s_objects[i].group = -1;
    s_groups.erase( s_groups.begin() + slot );
    Touch();
    return true;
}

int KiwiCon_GroupMemberCount( int group )
{
    if ( group < 0 )
        return 0;
    int n = 0;
    for ( size_t i = 0; i < s_objects.size(); ++i )
        if ( s_objects[i].group == group )
            ++n;
    return n;
}

// In-place store edit access.
kconObject_t *KiwiCon_MutableAt( int index )
{
    if ( index < 0 || index >= (int)s_objects.size() )
        return 0;
    return &s_objects[index];
}

void KiwiCon_NoteMutated()
{
    // Callers may move several objects; refresh every cached point-plane fit.
    RefitAll();
    Touch();
}

unsigned KiwiCon_Generation()
{
    return s_generation;
}

bool KiwiCon_HasObjects()
{
    return !s_objects.empty();
}

// Whole-store snapshots remain local; kiwi_undo owns their global ordering.
void KiwiCon_UndoPush()
{
    s_undo.push_back( s_objects );
    if ( (int)s_undo.size() > KCON_UNDO_DEPTH )
        s_undo.erase( s_undo.begin() );
    // A new edit invalidates both local and journal redo history.
    s_redo.clear();
    // Mint the unified journal ticket after storing the snapshot.
    KiwiUndo_NoteConstructionRecord( "construction edit" );
}

bool KiwiCon_UndoPop()
{
    if ( s_undo.empty() )
        return false;
    // Preserve the current state before restoring the undo snapshot.
    s_redo.push_back( s_objects );
    if ( (int)s_redo.size() > KCON_UNDO_DEPTH )
        s_redo.erase( s_redo.begin() );

    s_objects = s_undo.back();
    s_undo.pop_back();
    // Whole-store replacement invalidates every index-based construction selection.
    KiwiConSel_NoteStoreReplaced();
    Touch();
    return true;
}

// Mirror UndoPop without calling UndoPush, which would clear this redo path.
bool KiwiCon_RedoPop()
{
    if ( s_redo.empty() )
        return false;
    s_undo.push_back( s_objects );
    if ( (int)s_undo.size() > KCON_UNDO_DEPTH )
        s_undo.erase( s_undo.begin() );

    s_objects = s_redo.back();
    s_redo.pop_back();
    KiwiConSel_NoteStoreReplaced();       // same wholesale-replacement rule as UndoPop
    Touch();
    return true;
}

void KiwiCon_ClearRedo()
{
    s_redo.clear();
}

int KiwiCon_UndoDepth()
{
    return (int)s_undo.size();
}

// Active construction plane.
const kconPlane_t &KiwiCon_ActivePlane()
{
    if ( !s_planeSeeded )
    {
        // Initial default is world-ground XY through the origin.
        const float o[3] = { 0.0f, 0.0f, 0.0f };
        const float n[3] = { 0.0f, 0.0f, 1.0f };
        KiwiCon_MakePlane( o, n, 0, &s_plane );
        s_planeSeeded = true;
    }
    return s_plane;
}

void KiwiCon_SetActivePlane( const kconPlane_t &p )
{
    s_plane       = p;
    s_planeSeeded = true;
    g_nUpdateBits |= 1;
}

// Plane installation and explicit user intent are separate. Transient tool
// plane updates must never create a persistent latch.
void KiwiCon_MarkPlaneExplicit( const char *desc )
{
    s_planeExplicit = true;
    _snprintf( s_planeDesc, sizeof( s_planeDesc ), "%s", desc ? desc : "custom" );
    s_planeDesc[sizeof( s_planeDesc ) - 1] = '\0';
    g_nUpdateBits |= 1;
}

bool KiwiCon_PlaneIsExplicit() { return s_planeExplicit; }

const char *KiwiCon_PlaneDesc()
{
    return s_planeExplicit ? s_planeDesc : "";
}

void KiwiCon_ClearPlaneToDefault()
{
    const bool was = s_planeExplicit;
    s_planeExplicit = false;
    s_planeDesc[0]  = '\0';
    KiwiCon_DefaultPlaneForView();
    if ( was )
        Sys_Printf( "Construction plane: back to the default (%s).\n",
                    KiwiCon_PlaneDescLive() );
    g_nUpdateBits |= 1;
}

// Defaults are memoryless: an axis view uses its zero-height world-axis plane;
// other views use world ground. Never inherit an ambient plane elevation.
void KiwiCon_DefaultPlaneForView()
{
    int   axis = 2;
    float sign = 1.0f;
    if ( !KiwiViewCube_ViewAxis( &axis, &sign ) )
        axis = 2;                                   // free view -> world ground
    (void)sign;                                     // the plane has no facing

    float o[3] = { 0.0f, 0.0f, 0.0f };
    float n[3] = { 0.0f, 0.0f, 0.0f };
    n[axis] = 1.0f;

    kconPlane_t p;
    if ( KiwiCon_MakePlane( o, n, 0, &p ) )
        KiwiCon_SetActivePlane( p );
}

// What the chip and the console say about the plane in force RIGHT NOW.
const char *KiwiCon_PlaneDescLive()
{
    if ( s_planeExplicit )
        return s_planeDesc;
    const kconPlane_t &p = KiwiCon_ActivePlane();
    if ( fabsf( p.normal[2] ) > KCON_PLANE_PARALLEL ) return "ground XY (default)";
    if ( fabsf( p.normal[1] ) > KCON_PLANE_PARALLEL ) return "XZ (default)";
    if ( fabsf( p.normal[0] ) > KCON_PLANE_PARALLEL ) return "YZ (default)";
    return "default";
}

// A named major-plane command may inherit only a parallel EXPLICIT plane's
// offset; otherwise it uses world zero. Grid-snap the inherited working height
// so arbitrary face-hit coordinates cannot poison later plane placement.
static float KiwiCon_MajorPlaneOffset( int axis )
{
    if ( axis < 0 || axis > 2 )
        return 0.0f;
    const kconPlane_t &cur = KiwiCon_ActivePlane();
    float h = 0.0f;
    // Ambient/default planes never donate an offset.
    if ( s_planeExplicit && fabsf( cur.normal[axis] ) > KCON_PLANE_PARALLEL )   // rule 1
        h = cur.origin[axis];
    float in[3]  = { 0.0f, 0.0f, 0.0f };
    float out[3] = { 0.0f, 0.0f, 0.0f };
    in[axis] = h;
    KiwiGrid_Snap( in, out );                                    // rule 2
    return out[axis];
}

// Preserve off-axis coordinates and replace only the major-plane offset.
static void KiwiCon_MajorPlaneOrigin( int axis, float out[3] )
{
    const kconPlane_t &cur = KiwiCon_ActivePlane();
    Copy3( cur.origin, out );
    if ( axis >= 0 && axis <= 2 )
        out[axis] = KiwiCon_MajorPlaneOffset( axis );
}

void KiwiCon_SetPlaneAxis( int axis )
{
    if ( axis < 0 || axis > 2 )
        return;
    // Inherit height only from a parallel explicit plane; otherwise use zero.
    float o[3];
    KiwiCon_MajorPlaneOrigin( axis, o );
    float n[3] = { 0.0f, 0.0f, 0.0f };
    n[axis] = 1.0f;

    kconPlane_t p;
    if ( !KiwiCon_MakePlane( o, n, 0, &p ) )
        return;
    KiwiCon_SetActivePlane( p );
    Sys_Printf( "Construction plane: %s at %g\n",
                ( axis == 2 ) ? "XY" : ( axis == 1 ) ? "XZ" : "YZ", (double)o[axis] );
}

void KiwiCon_SetPlaneFromView()
{
    CamWnd_BuildMatrix();
    camera_s *c = Ed_Camera();

    // A view plane needs a DEPTH: put it through the current plane's origin so
    // "from view" re-aims the plane without teleporting it to the eye.
    const kconPlane_t &cur = KiwiCon_ActivePlane();
    float n[3];
    Copy3( c->vpn, n );
    n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];     // face the camera

    kconPlane_t p;
    if ( !KiwiCon_MakePlane( cur.origin, n, c->vright, &p ) )
        return;
    KiwiCon_SetActivePlane( p );
    Sys_Printf( "Construction plane: from view.\n" );
}

namespace
{
    // A selected/cursor face must face the ray; selected faces must also be within
    // their own bounds plus extent-scaled slack. Rejection falls through to the
    // default plane rather than extending a stale face across open space.
    bool FaceFacesTheRay( const face_t *f, const ray_t &ray )
    {
        const float den = Dot3( ray.dir, f->plane.normal );
        return ( fabsf( den ) >= KCON_PLANE_FACING_MIN );
    }

    bool RayReachesFace( const face_t *f, const winding_t *w, const ray_t &ray )
    {
        if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
            return false;
        const float den = Dot3( ray.dir, f->plane.normal );
        if ( fabsf( den ) < KCON_PLANE_FACING_MIN )
            return false;

        float rel[3];
        Sub3( w->p[0], ray.origin, rel );
        const float t = Dot3( rel, f->plane.normal ) / den;
        if ( !( t > 0.0f ) || t > KCON_PLANE_REACH )
            return false;
        float hit[3];
        Mad3( ray.origin, ray.dir, t, hit );

        float mins[3], maxs[3];
        Copy3( w->p[0], mins );
        Copy3( w->p[0], maxs );
        for ( int i = 1; i < w->numpoints; ++i )
            for ( int k = 0; k < 3; ++k )
            {
                if ( w->p[i][k] < mins[k] ) mins[k] = w->p[i][k];
                if ( w->p[i][k] > maxs[k] ) maxs[k] = w->p[i][k];
            }
        float extent = 0.0f;
        for ( int k = 0; k < 3; ++k )
            if ( maxs[k] - mins[k] > extent )
                extent = maxs[k] - mins[k];
        float grow = extent * KCON_FACE_PLANE_SLACK;
        if ( grow < KCON_FACE_PLANE_MINGROW )
            grow = KCON_FACE_PLANE_MINGROW;

        for ( int k = 0; k < 3; ++k )
            if ( hit[k] < mins[k] - grow || hit[k] > maxs[k] + grow )
                return false;
        return true;
    }

    // Face-hit plane: WORLD-SPACE hit origin, face normal, longest-edge u hint.
    bool PlaneFromFacePick( const pick_result_t &pick, const ray_t &ray, kconPlane_t *out )
    {
        if ( !pick.valid || pick.item.kind != SEL_FACE )
            return false;
        selbrush_t *node = pick.item.brush;
        if ( !node || !node->def || !node->def->faces )
            return false;
        const int fi = pick.item.faceIndex;
        if ( fi < 0 || fi >= node->def->faceCount )
            return false;
        face_t *f = &node->def->faces[fi];

        // Do not return a face whose screen-to-world scale is near singular.
        if ( !FaceFacesTheRay( f, ray ) )
            return false;

        const float *hint = 0;
        float longest[3];
        winding_t *w = f->w;
        if ( w && w->numpoints >= 2 && w->numpoints <= MAX_POINTS_ON_WINDING
          && LongestEdgeHint( &w->p[0][0], w->numpoints, longest ) )
            hint = longest;
        return KiwiCon_MakePlane( pick.point, f->plane.normal, hint, out );
    }
}

bool KiwiCon_SetPlaneFromCursorFace()
{
    ray_t ray;
    if ( !Pick_RayFromCursor( &ray ) )
        return false;
    const pick_result_t pick = Pick( ray, SEL_MASK_FACE );
    kconPlane_t p;
    if ( !PlaneFromFacePick( pick, ray, &p ) )
        return false;
    KiwiCon_SetActivePlane( p );
    return true;
}

// Exactly one live selected face can seed the armed creation flow; multiple
// faces have no unambiguous plane.
bool KiwiCon_SetPlaneFromSelectedFace()
{
    const selection_t &sel = KiwiSel();
    const sel_item_t  *face = 0;
    for ( size_t i = 0; i < sel.items.size(); ++i )
    {
        if ( sel.items[i].kind != SEL_FACE )
            continue;
        if ( face )
            return false;                     // more than one: no single answer
        face = &sel.items[i];
    }
    if ( !face || !Sel_BrushLive( face->brush ) )
        return false;

    return KiwiCon_SetPlaneFromFace( face->brush, face->faceIndex, true );
}

// Shared selected-face/[Space] construction. `requireReach` applies only where
// the preexisting selection could be stale relative to the cursor.
bool KiwiCon_SetPlaneFromFace( selbrush_t *node, int fi, bool requireReach )
{
    if ( !node || !Sel_BrushLive( node ) )
        return false;
    if ( !node->def || !node->def->faces )
        return false;
    if ( fi < 0 || fi >= node->def->faceCount )
        return false;
    face_t    *f = &node->def->faces[fi];
    winding_t *w = f->w;
    if ( !w || w->numpoints < 3 || w->numpoints > MAX_POINTS_ON_WINDING )
        return false;

    // A sticky selected face must still be within cursor reach when a ray exists.
    // Menu invocation has no ray to test and retains the selected-face behavior.
    ray_t ray;
    if ( requireReach && Pick_RayFromCursor( &ray ) && !RayReachesFace( f, w, ray ) )
        return false;

    float centre[3] = { 0.0f, 0.0f, 0.0f };
    for ( int i = 0; i < w->numpoints; ++i )
        for ( int k = 0; k < 3; ++k )
            centre[k] += w->p[i][k];
    for ( int k = 0; k < 3; ++k )
        centre[k] /= (float)w->numpoints;

    const float *hint = 0;
    float longest[3];
    if ( LongestEdgeHint( &w->p[0][0], w->numpoints, longest ) )
        hint = longest;

    kconPlane_t p;
    if ( !KiwiCon_MakePlane( centre, f->plane.normal, hint, &p ) )
        return false;
    KiwiCon_SetActivePlane( p );
    return true;
}

// Quantize camera direction to the nearest world-axis plane; arbitrary view
// planes are unsuitable for axis-bound brush primitives. This is a named
// operation, not an automatic tool-start inference.
bool KiwiCon_SetPlaneFromViewDominantAxis()
{
    CamWnd_BuildMatrix();
    const camera_s *c = Ed_Camera();
    if ( !c )
        return false;

    int   axis = 2;
    float best = fabsf( c->vpn[2] );
    for ( int k = 0; k < 2; ++k )
        if ( fabsf( c->vpn[k] ) > best ) { best = fabsf( c->vpn[k] ); axis = k; }
    if ( !( best > 1.0e-4f ) )
        return false;                       // a degenerate view matrix; leave it alone

    // Reuse only a parallel explicit, grid-quantized working height.
    float o[3];
    KiwiCon_MajorPlaneOrigin( axis, o );
    float n[3] = { 0.0f, 0.0f, 0.0f };
    n[axis] = 1.0f;

    kconPlane_t p;
    if ( !KiwiCon_MakePlane( o, n, 0, &p ) )
        return false;
    KiwiCon_SetActivePlane( p );
    Sys_Printf( "Construction plane: %s (from the view) at %g\n",
                ( axis == 2 ) ? "XY" : ( axis == 1 ) ? "XZ" : "YZ", (double)o[axis] );
    return true;
}

// Auto-plane resolution runs once at tool start. Ambient cursor/object/view
// inference remains deliberately absent; mid-gesture camera motion cannot reseat it.
void KiwiCon_AutoPlaneForTool()
{
    // Consume the one-shot selected-face arm even if an explicit plane wins.
    // Invariant: explicit plane; else armed single face; else memoryless default.
    // Never let ordinary selection mint an invisible latch that filters later snaps.
    const bool selFaceArmed = s_selFacePlaneArmed;
    s_selFacePlaneArmed = false;

    if ( s_planeExplicit )
    {
        // Preserve and announce the explicit plane at every tool start.
        Sys_Printf( "Construction plane: %s (explicit - press Space over empty space "
                    "to clear it).\n", KiwiCon_PlaneDescLive() );
        return;
    }
    if ( selFaceArmed && KiwiCon_SetPlaneFromSelectedFace() )
    {
        KiwiCon_MarkPlaneExplicit( "the SELECTED face" );
        Sys_Printf( "Construction plane: from the SELECTED face.\n" );
        return;
    }
    KiwiCon_DefaultPlaneForView();
}

// Arm selected-face adoption for exactly the next tool start.
void KiwiCon_ArmSelectedFacePlane()
{
    s_selFacePlaneArmed = true;
}

// Drawing tools share gesture, numeric, cancel, and undo plumbing; only planar
// subclasses own construction-plane placement.
namespace
{
    class KiwiDrawTool;
    KiwiDrawTool *s_activeTool = 0;
    // Latched PlanarOnly state is visible to snap code before class definitions.
    bool          s_activeToolPlanar = false;

    // WORLD-GROUND fallback frame; it is never installed as an active plane.
    void WorldGroundBasis( kconPlane_t *out )
    {
        const float o[3] = { 0.0f, 0.0f, 0.0f };
        const float n[3] = { 0.0f, 0.0f, 1.0f };
        const float u[3] = { 1.0f, 0.0f, 0.0f };
        KiwiCon_MakePlane( o, n, u, out );
    }

    // Seat the cursor face's orientation at the resolved WORLD-SPACE point;
    // if no usable face exists, the caller keeps world ground.
    bool PlaneFromCursorFaceAt( const float origin[3], kconPlane_t *out )
    {
        ray_t ray;
        if ( !Pick_RayFromCursor( &ray ) )
            return false;
        const pick_result_t pick = Pick( ray, SEL_MASK_FACE );
        kconPlane_t p;
        if ( !PlaneFromFacePick( pick, ray, &p ) )
            return false;
        Copy3( origin, p.origin );
        *out = p;
        return true;
    }

    // Static numeric-field labels; line tools add bearing, round tools add sides.
    const kiwiNumField_t KCON_FIELDS_LEN[1] =
    { { "length", KNUM_LENGTH, false } };
    const kiwiNumField_t KCON_FIELDS_LEN_ANG[2] =
    { { "length", KNUM_LENGTH, false },
      { "angle",  KNUM_ANGLE,  false } };
    // Round-tool side count shares the Tab field and polygon bracket-key state.
    const kiwiNumField_t KCON_FIELDS_LEN_SIDES[2] =
    { { "length", KNUM_LENGTH, false },
      { "sides",  KNUM_COUNT,  false } };

    class KiwiDrawTool : public KiwiEditorCommand
    {
    public:
        // A drawing tool must NEVER auto-commit on a click (kiwi_command.h's
        // WantsClicks contract) — placing a point is what a click means here.
        bool WantsClicks() const override { return true; }

        const char *HudStatus() const override { return m_hud[0] ? m_hud : 0; }

        // Advertise only tool-owned keys; framework prompts are added elsewhere.
        // Z is absent for intrinsically planar tools.
        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t s_free[] = {
                { "Z",   "Vertical" },
                { "Esc", "Clear chain" },
            };
            static const kiwiPrompt_t s_planar[] = {
                { "Esc", "Clear chain" },
            };
            if ( PlanarOnly() )
            {
                *out = s_planar;
                return (int)( sizeof( s_planar ) / sizeof( s_planar[0] ) );
            }
            *out = s_free;
            return (int)( sizeof( s_free ) / sizeof( s_free[0] ) );
        }

        // Only line-like tools expose an unambiguous bearing field.
        virtual bool WantsAngleField() const { return false; }

        // Tessellated round tools use the otherwise-unused second field for sides.
        virtual bool WantsSidesField() const { return false; }

        int NumericFields( const kiwiNumField_t **out ) const override
        {
            if ( WantsAngleField() )
            {
                *out = KCON_FIELDS_LEN_ANG;
                return 2;
            }
            if ( WantsSidesField() )
            {
                *out = KCON_FIELDS_LEN_SIDES;
                return 2;
            }
            *out = KCON_FIELDS_LEN;
            return 1;
        }

        // Report the count the shape will actually use; 0 means no answer yet.
        virtual int ToolSides() const { return m_sidesOverride; }

        bool NumericFieldValue( int field, float *out ) const override
        {
            if ( !out )
                return false;
            // Side count is tool state and is available before point placement.
            if ( field == 1 && WantsSidesField() )
            {
                const int n = ToolSides();
                if ( n <= 0 )
                    return false;                 // AUTO and nothing to report yet
                *out = (float)n;
                return true;
            }
            if ( !m_haveCur || m_pts.empty() )
                return false;
            float a[3], d[3];
            LastPoint( a );
            Sub3( m_cur, a, d );
            if ( field == 0 )
            {
                *out = Len3( d );                         // raw world units, 3D
                return true;
            }
            if ( field == 1 && WantsAngleField() )
            {
                // Bearings use the canonical basis of the resolved surface and are
                // omitted for Z-lock or mostly out-of-plane segments.
                if ( m_zLock )
                    return false;
                float deg = 0.0f, inPlane = 0.0f, dn = 0.0f;
                if ( !KiwiCon_BearingOf( BearingNormal(), d, &deg, &inPlane, &dn ) )
                    return false;                         // no meaningful bearing
                if ( fabsf( dn ) > inPlane )
                    return false;                         // mostly out of plane
                *out = deg;
                return true;
            }
            return false;
        }

        bool BubbleAnchor( float *out3 ) const override
        {
            if ( !out3 || !m_haveCur )
                return false;
            // The MOVING END of the segment — where Plasticity pins it.
            Copy3( m_cur, out3 );
            return true;
        }

        void NumericFieldChanged( int field, bool has, float world ) override
        {
            if ( field == 1 && WantsAngleField() )
            {
                m_hasAngle = has;
                // Undo the numeric layer's unconditional display-unit conversion.
                m_angleDeg = Units_ToDisplay( world );
                ApplyAngleOverride();
                Recompute();
                g_nUpdateBits |= 1;
                return;
            }
            // Counts also undo the numeric layer's length conversion.
            if ( field == 1 && WantsSidesField() )
            {
                if ( has )
                {
                    const int n = ClampSides(
                        (int)floorf( Units_ToDisplay( world ) + 0.5f ) );
                    m_sidesOverride = n;
                    KiwiCon_SetToolSides( n );    // remembered for the next gesture
                }
                else
                {
                    m_sidesOverride = 0;          // cleared: back to AUTO
                }
                Recompute();
                g_nUpdateBits |= 1;
                return;
            }
            KiwiEditorCommand::NumericFieldChanged( field, has, world );
        }

        bool Begin() override
        {
            // Seed the per-gesture override from the remembered tool setting.
            m_sidesOverride = WantsSidesField() ? KiwiCon_ToolSides() : 0;
            m_pts.clear();
            m_haveCur   = false;
            m_hasNum    = false;
            m_numWorld  = 0.0f;
            m_hasAngle  = false;
            m_angleDeg  = 0.0f;
            // World ground is only the fallback bearing surface.
            m_bearingNormal[0] = 0.0f;
            m_bearingNormal[1] = 0.0f;
            m_bearingNormal[2] = 1.0f;
            m_zLock     = false;
            m_lastClickMs = 0;
            m_lastClickX  = -9999;
            m_lastClickY  = -9999;
            m_hud[0]    = '\0';

            // Free line/polyline/spline tools keep verbatim WORLD points and never
            // touch the active plane. Planar tools resolve or derive one.
            WorldGroundBasis( &m_plane );
            if ( PlanarOnly() )
            {
                KiwiCon_AutoPlaneForTool();
                m_plane = KiwiCon_ActivePlane();
            }
            // Restore the planar gesture's entry plane on Leave.
            m_planeOnEntry     = m_plane;
            m_planeFromSurface = false;
            s_activeTool       = this;
            s_activeToolPlanar = PlanarOnly();

            // Latch a first cursor point so the very first frame already draws.
            LatchFromCursor();
            UpdateHud();
            Sys_Printf( "%s: %s\n", Name(), BeginHint() );
            return true;
        }

        // Tool-specific opening grammar.
        virtual const char *BeginHint() const
        { return "click to place points, Esc cancels."; }

        // Intrinsically planar shapes project; free curves accept resolved WORLD points.
        virtual bool PlanarOnly() const { return true; }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;
            m_snap = snap;
            if ( snap.valid )
            {
                // Snap results are already WORLD SPACE; store them verbatim.
                Copy3( snap.position, m_cur );
                m_haveCur = true;

                // Only planar tools project after their first point, or immediately
                // when the user supplied an explicit plane.
                if ( PlanarOnly() && ( PointCount() >= 1 || KiwiCon_PlaneIsExplicit() ) )
                    ProjectCurOntoPlane();
            }
            // Explicit Z constraint outranks the snap, while geometry snaps donate height.
            ApplyZLock();
            // Reapply typed bearing after pointer resolution and before recomputation.
            ApplyAngleOverride();
            Recompute();
            g_nUpdateBits |= 1;
        }

        void NumericChanged( bool has, float world ) override
        {
            m_hasNum   = has && KiwiNum_HasValue();
            m_numWorld = world;
            ApplyAngleOverride();
            Recompute();
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            // Consume unified undo/redo here so legacy accelerators cannot fire twice.
            if ( vk == 0x5A && ( mods & 4 ) != 0 )      // Ctrl+Z / Ctrl+Shift+Z
            {
                if ( ( mods & 1 ) != 0 )
                {
                    if ( !KiwiUndo_Redo() )
                        Sys_Printf( "Nothing left to redo.\n" );
                }
                else if ( !KiwiUndo_Undo() )
                {
                    Sys_Printf( "Nothing left to undo.\n" );
                }
                return true;
            }
            if ( vk == 0x59 && ( mods & 4 ) != 0 )      // Ctrl+Y
            {
                if ( !KiwiUndo_Redo() )
                    Sys_Printf( "Nothing left to redo.\n" );
                return true;
            }

            // Z toggles a vertical constraint for free tools. It is a toggle because
            // command input has no key-up path; planar tools consume and refuse it.
            if ( vk == 0x5A && !mods )                  // Z
            {
                if ( PlanarOnly() )
                {
                    Sys_Printf( "%s: this shape is planar — Z does nothing here "
                                "(use the line, polyline or spline tool).\n", Name() );
                    return true;
                }
                m_zLock = !m_zLock;
                if ( m_zLock && m_pts.empty() )
                {
                    m_zLock = false;                    // nothing to go up FROM yet
                    Sys_Printf( "%s: place a point first — Z constrains the NEXT segment.\n",
                                Name() );
                }
                else
                {
                    Sys_Printf( "%s: vertical constraint %s.\n",
                                Name(), m_zLock ? "ON" : "off" );
                }
                UpdateHud();
                g_nUpdateBits |= 1;
                return true;
            }

            // First Esc clears a live chain; the next leaves the tool.
            if ( vk == 0x1B && !m_pts.empty() )         // VK_ESCAPE
            {
                m_pts.clear();
                m_zLock = false;
                // Let subclasses reset stage state when the base clears the chain.
                OnChainCleared();
                UpdateHud();
                g_nUpdateBits |= 1;
                Sys_Printf( "%s: chain cleared (Esc again to exit).\n", Name() );
                return true;
            }

            // Enter ENDS a multi-point tool that already has enough points; for the
            // fixed-arity tools the framework's Enter-commits is right as-is.
            if ( vk == 0x0D && WantsEnterFinish() )     // VK_RETURN
            {
                Finish();
                return true;
            }
            return false;
        }

        void Commit() override
        {
            // Enter / an ending click arrive here.  Finish() is idempotent: a tool
            // that already stored its object on the ending click stores nothing.
            Finish();
            Leave();
        }

        void Cancel() override
        {
            m_pts.clear();
            Leave();
            g_nUpdateBits |= 1;
        }

        void DrawWorld() override
        {
            // Do not advertise a plane for free tools that do not use one.
            if ( PlanarOnly() && ( PointCount() >= 1 || KiwiCon_PlaneIsExplicit() ) )
                DrawWorkingPlane();
            DrawChain();
        }

        // Framework click hook: place a point.  Returning false COMMITS.
        bool Click() override
        {
            if ( !m_haveCur )
                return true;

            int cx = 0, cy = 0;
            const bool haveCur = KiwiCmd_LastCursor( &cx, &cy );
            const unsigned nowMs = (unsigned)::GetTickCount();
            const bool dbl = haveCur
                          && ( nowMs - m_lastClickMs ) <= KCON_DBLCLICK_MS
                          && abs( cx - m_lastClickX ) <= KCON_DBLCLICK_SLOP_PX
                          && abs( cy - m_lastClickY ) <= KCON_DBLCLICK_SLOP_PX;
            m_lastClickMs = nowMs;
            m_lastClickX  = cx;
            m_lastClickY  = cy;

            return OnClick( dbl );
        }

        // Tool-specific behavior.
        virtual bool OnClick( bool doubleClick ) = 0;
        virtual void Recompute() = 0;
        virtual void UpdateHud() = 0;
        virtual bool WantsEnterFinish() const { return false; }
        // Store any storable result. Enter/click provenance is intentionally absent.
        virtual void Finish() {}
        // Reset tool-specific stage state after Esc clears the chain.
        virtual void OnChainCleared() {}

        // Shared helpers.
        // Seed preview through the same snap/surface/ground resolver as MouseMove.
        bool LatchFromCursor()
        {
            ray_t ray;
            if ( !Pick_RayFromCursor( &ray ) )
                return false;
            int cx = 0, cy = 0;
            (void)KiwiCmd_LastCursor( &cx, &cy ); // label anchor only; the ray drives
            if ( !KiwiSnap_ResolvePoint( ray, cx, cy, m_cur ) )
                return false;
            if ( PlanarOnly() && KiwiCon_PlaneIsExplicit() )
                ProjectCurOntoPlane();
            m_haveCur = true;
            return true;
        }

        void Leave()
        {
            if ( s_activeTool == this )
            {
                s_activeTool       = 0;
                s_activeToolPlanar = false;
            }
            // Only planar tools can alter the active plane; restore their entry state.
            if ( PlanarOnly() )
                KiwiCon_SetActivePlane( m_planeOnEntry );
            m_pts.clear();
            m_haveCur = false;
            m_zLock   = false;
            m_hud[0]  = '\0';
            OnChainCleared();
        }

        // Plane-space helpers for intrinsically planar tools.
        void CurUV( float uv[2] ) const   { KiwiCon_WorldToPlane( m_plane, m_cur, uv ); }
        void SetCurUV( const float uv[2] ){ KiwiCon_PlaneToWorld( m_plane, uv, m_cur ); }
        void LastPointUV( float uv[2] ) const
        {
            float w[3];
            LastPoint( w );
            KiwiCon_WorldToPlane( m_plane, w, uv );
        }
        void ProjectCurOntoPlane()
        {
            float uv[2];
            CurUV( uv );
            SetCurUV( uv );
        }

        // Constrain to the vertical through the last point. Geometry snaps supply
        // height; otherwise use the closest point to the cursor ray.
        void ApplyZLock()
        {
            if ( !m_zLock || m_pts.empty() )
                return;
            float a[3];
            LastPoint( a );

            if ( m_snap.valid && KiwiSnap_IsGeometry( m_snap.type ) )
            {
                m_cur[0] = a[0];
                m_cur[1] = a[1];
                m_cur[2] = m_snap.position[2];
                m_haveCur = true;
                return;
            }

            ray_t ray;
            if ( !Pick_RayFromCursor( &ray ) )
                return;
            const float rayEnd[3] = { ray.origin[0] + ray.dir[0] * 1.0e5f,
                                      ray.origin[1] + ray.dir[1] * 1.0e5f,
                                      ray.origin[2] + ray.dir[2] * 1.0e5f };
            // A generous vertical span both ways so the closest-approach solve is
            // never clamped by the segment's own ends.
            const float vlo[3] = { a[0], a[1], a[2] - 1.0e5f };
            const float vhi[3] = { a[0], a[1], a[2] + 1.0e5f };
            float t = 0.0f;
            KiwiCon_SegSegClosest( vlo, vhi, ray.origin, rayEnd, &t, 0, 0 );
            float z = vlo[2] + ( vhi[2] - vlo[2] ) * t;
            // Respect both the grid master switch and construction Ctrl-to-unsnap.
            if ( KiwiCmd_SnapEngaged() )
            {
                const float zin [3] = { a[0], a[1], z };
                float       zout[3];
                KiwiGrid_Snap( zin, zout );
                z = zout[2];
            }
            m_cur[0] = a[0];
            m_cur[1] = a[1];
            m_cur[2] = z;
            m_haveCur = true;
        }

        // Rotate only the in-plane component into the typed canonical bearing;
        // preserve length and the out-of-plane component. Z lock wins.
        void ApplyAngleOverride()
        {
            if ( !m_hasAngle || !m_haveCur || m_pts.empty() || m_zLock )
                return;
            float a[3], d[3];
            LastPoint( a );
            Sub3( m_cur, a, d );
            const float *n = BearingNormal();
            float len = 0.0f, dn = 0.0f;
            KiwiCon_BearingOf( n, d, nullptr, &len, &dn );   // the split, not the bearing
            if ( !( len > 1.0e-4f ) )
                return;                       // no in-plane length: nothing to swing
            float dir[3];
            KiwiCon_BearingDir( n, m_angleDeg, dir );
            for ( int k = 0; k < 3; ++k )
                m_cur[k] = a[k] + dir[k] * len + n[k] * dn;
        }

        // Free tools never install a plane. A non-explicit planar tool derives its
        // plane once from the first point's face, or world ground in void; the
        // resolved WORLD-SPACE first point becomes that plane's origin.
        void PushPoint( const float w[3] )
        {
            const bool first = m_pts.empty();
            m_pts.push_back( w[0] );
            m_pts.push_back( w[1] );
            m_pts.push_back( w[2] );

            // Free tools retain only a fallback bearing normal from each placed point;
            // BearingNormal prefers the current snap resolution's plane.
            if ( !PlanarOnly() )
            {
                kconPlane_t bp;
                if ( PlaneFromCursorFaceAt( w, &bp ) )
                    Copy3( bp.normal, m_bearingNormal );
                else
                {
                    m_bearingNormal[0] = 0.0f;   // void: the world ground
                    m_bearingNormal[1] = 0.0f;
                    m_bearingNormal[2] = 1.0f;
                }
            }

            if ( !first || !PlanarOnly() || KiwiCon_PlaneIsExplicit() )
                return;

            kconPlane_t p;
            m_planeFromSurface = PlaneFromCursorFaceAt( w, &p );
            if ( !m_planeFromSurface )
            {
                WorldGroundBasis( &p );
                Copy3( w, p.origin );
            }
            m_plane = p;
            KiwiCon_SetActivePlane( m_plane );
            // Announce the derived plane when it becomes fixed for the gesture.
            Sys_Printf( "%s: plane taken from %s.\n", Name(),
                        m_planeFromSurface ? "the surface under the first point"
                                           : "the world ground at the first point" );
        }

        int PointCount() const { return (int)( m_pts.size() / 3 ); }

        void LastPoint( float out[3] ) const
        {
            Copy3( &m_pts[m_pts.size() - 3], out );
        }

        void FirstPoint( float out[3] ) const
        {
            Copy3( &m_pts[0], out );
        }

        // Guard the size_t stride here so short chains cannot underflow.
        void PrevPoint( float out[3] ) const
        {
            if ( m_pts.size() < 6 )
            {
                out[0] = out[1] = out[2] = 0.0f;
                return;
            }
            Copy3( &m_pts[m_pts.size() - 6], out );
        }

        // Loop-close proximity is measured in screen pixels, never world units.
        bool NearFirstPoint() const
        {
            if ( PointCount() < 3 )
                return false;
            float fw[3];
            FirstPoint( fw );
            float fx, fy, cx2, cy2;
            if ( !Pick_WorldToImage( fw, &fx, &fy ) || !Pick_WorldToImage( m_cur, &cx2, &cy2 ) )
                return false;
            const float dx = fx - cx2, dy = fy - cy2;
            return sqrtf( dx * dx + dy * dy ) <= KCON_JOIN_PIXELS;
        }

        void SetHud( const char *fmt, ... )
        {
            va_list ap;
            va_start( ap, fmt );
            _vsnprintf( m_hud, sizeof( m_hud ), fmt, ap );
            va_end( ap );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        // Bounded screen-space indicator for the active planar gesture. Fade at
        // grazing angles and draw it before geometry so budget loss drops it first.
        void DrawWorkingPlane()
        {
            const camera_s *c = Ed_Camera();
            if ( !c )
                return;

            // Squared grazing fade; suppress a near-edge-on screen smear.
            const float d   = Dot3( m_plane.normal, c->vpn );
            const float fade = d * d;
            if ( fade < KCON_PLANE_FADE_MIN )
                return;

            // Anchor at the first point, or the explicit plane origin before placement.
            float o[3];
            if ( PointCount() >= 1 )
                FirstPoint( o );
            else
                Copy3( m_plane.origin, o );

            // Keep a stable screen size at every camera distance.
            const float perPx = KiwiCam_WorldPerPixel( o );
            if ( !( perPx > 0.0f ) )
                return;
            const float half = perPx * KCON_PLANE_HALF_PIXELS;

            // The colour: the active-chain amber, dimmed by the grazing fade.  A
            // DIMMER COLOUR and not a lower alpha — kiwi_lines.h TRAP 2.
            const float k = 0.35f + 0.45f * fade;
            KiwiLines_Color( KCON_COL_ACTIVE[0] * k,
                             KCON_COL_ACTIVE[1] * k,
                             KCON_COL_ACTIVE[2] * k );

            // Square plus cross conveys orientation and height within a small budget.
            float corner[4][3];
            for ( int i = 0; i < 4; ++i )
            {
                const float su = ( i == 0 || i == 3 ) ? -half : half;
                const float sv = ( i < 2 )            ? -half : half;
                for ( int kk = 0; kk < 3; ++kk )
                    corner[i][kk] = o[kk] + m_plane.u[kk] * su + m_plane.v[kk] * sv;
            }
            for ( int i = 0; i < 4; ++i )
                if ( !KiwiLines_Add( corner[i], corner[( i + 1 ) % 4] ) )
                    return;

            const float cross = half * KCON_PLANE_CROSS_FRAC;
            float a[3], b[3];
            for ( int kk = 0; kk < 3; ++kk ) { a[kk] = o[kk] - m_plane.u[kk] * cross;
                                               b[kk] = o[kk] + m_plane.u[kk] * cross; }
            if ( !KiwiLines_Add( a, b ) )
                return;
            for ( int kk = 0; kk < 3; ++kk ) { a[kk] = o[kk] - m_plane.v[kk] * cross;
                                               b[kk] = o[kk] + m_plane.v[kk] * cross; }
            KiwiLines_Add( a, b );
        }

        // The rubber band + already-placed chain, inside the framework's batch.
        void DrawChain()
        {
            if ( m_pts.empty() && !m_haveCur )
                return;
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            const int n = PointCount();
            for ( int i = 0; i + 1 < n; ++i )
                if ( !KiwiLines_Add( &m_pts[(size_t)i * 3], &m_pts[( (size_t)i + 1 ) * 3] ) )
                    return;
            if ( n >= 1 && m_haveCur )
                KiwiLines_Add( &m_pts[( (size_t)n - 1 ) * 3], m_cur );
        }

        // Shape plane for planar tools; an inert ground placeholder for free tools.
        kconPlane_t        m_plane;
        // Planar gesture entry plane, restored on Leave.
        kconPlane_t        m_planeOnEntry;
        // Records whether a planar tool derived its plane from a surface.
        bool               m_planeFromSurface = false;
        // BearingNormal uses the planar shape plane, else the current resolved snap
        // plane, else this last-point fallback normal.
        float              m_bearingNormal[3] = { 0.0f, 0.0f, 1.0f };
        const float       *BearingNormal() const
        {
            if ( PlanarOnly() )
                return m_plane.normal;
            if ( m_snap.valid && m_snap.havePlane )
                return m_snap.planeNormal;
            return m_bearingNormal;
        }
        std::vector<float> m_pts;                 // placed points, 3 floats, WORLD
        float              m_cur[3] = { 0.0f, 0.0f, 0.0f };
        bool               m_haveCur = false;
        bool               m_zLock   = false;
        bool               m_hasNum  = false;
        int                m_sidesOverride = 0;   // 0 = automatic
        float              m_numWorld = 0.0f;
        // Typed bearing in the canonical surface basis.
        bool               m_hasAngle = false;
        float              m_angleDeg = 0.0f;
        snap_result_t      m_snap;
        unsigned           m_lastClickMs = 0;
        int                m_lastClickX  = -9999;
        int                m_lastClickY  = -9999;
        char               m_hud[192] = { 0 };
    };

    // Chained curve grammar: LMB adds points, near-first closes, double-click/Enter
    // finishes, and Esc drops the last point. RMB places-and-finishes only before
    // any LMB point; afterward it finishes without adding a tail. Two points store
    // as KCON_LINE, longer chains as KCON_POLYLINE; both command ids use this tool.
    class KiwiCurveTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Curve"; }
        // Tab cycles length to bearing angle for each segment.
        bool WantsAngleField() const override { return true; }
        // Free curves store the resolved WORLD-space snap position.
        bool PlanarOnly() const override { return false; }

        // Base Enter handling finishes and leaves the chained command.
        const char *BeginHint() const override
        {
            // Entry hint states the pre-LMB RMB behavior; the live HUD updates it.
            return "LMB chains points, click the first point to close, "
                   "RMB places a point and ends (once you have left-clicked, it ends "
                   "WITHOUT placing), Enter finishes at the cursor, "
                   "Esc drops the last point.";
        }

        // RMB is click-source-sensitive: before any LMB it adds the cursor point;
        // after any LMB it ends without a tail. RMB drags remain camera input.
        int HudPrompts( const kiwiPrompt_t **out ) const override
        {
            static const kiwiPrompt_t s_curve[] = {
                { "aim",  "Z guide = vertical" },
                { "Z",    "Vertical lock" },
                { "Esc",  "Drop last point" },
            };
            *out = s_curve;
            return (int)( sizeof( s_curve ) / sizeof( s_curve[0] ) );
        }

        bool Begin() override
        {
            m_wantClosed   = false;
            m_noTailPoint  = false;
            m_anyLmbPoint  = false;
            return KiwiDrawTool::Begin();
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            // Record RMB provenance but let the common Enter/confirm path finish.
            // The latch asks whether LMB was ever used in this chain, not its size.
            if ( vk == 0x0D && KiwiCmd_ConfirmIsRmb() && m_anyLmbPoint )
                m_noTailPoint = true;

            // Remove one point; an empty-chain Esc falls through to command cancel.
            if ( vk == 0x1B && !m_pts.empty() )         // VK_ESCAPE
            {
                m_pts.resize( m_pts.size() - 3 );
                if ( m_pts.empty() )
                    m_zLock = false;                    // nothing to go up FROM
                UpdateHud();
                g_nUpdateBits |= 1;
                Sys_Printf( "Curve: removed the last point (%i left%s).\n",
                            PointCount(),
                            m_pts.empty() ? ", Esc again to exit" : "" );
                return true;
            }
            return KiwiDrawTool::KeyDown( vk, mods );
        }

        bool OnClick( bool doubleClick ) override
        {
            // Pixel-space proximity closes reliably even when another equal-distance
            // snap candidate outranks the construction anchor.
            if ( PointCount() >= 3 && NearFirstPoint() )
            {
                m_wantClosed = true;
                Finish();
                return false;                      // closing the loop ENDS the object
            }
            if ( doubleClick && PointCount() >= 2 )
            {
                Finish();
                return false;
            }
            if ( PointCount() >= KCON_MAX_POINTS )
            {
                Sys_Printf( "Curve: %i point limit reached — ending here.\n", KCON_MAX_POINTS );
                Finish();
                return false;
            }
            PushPoint( m_cur );
            m_anyLmbPoint = true;
            UpdateHud();
            return true;
        }

        void Recompute() override
        {
            // Typed length sizes the segment leaving the last placed point.
            if ( m_hasNum && PointCount() >= 1 )
            {
                float a[3], d[3];
                LastPoint( a );
                Sub3( m_cur, a, d );
                const float l = Len3( d );
                if ( l > 1.0e-4f )
                    Mad3( a, d, m_numWorld / l, m_cur );
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            // State the next click and current RMB meaning on every frame.
            const int n = PointCount();
            if ( n == 0 )
            {
                SetHud( "curve  ·  click: first point  ·  keep clicking to chain  ·  %s",
                        m_anyLmbPoint ? "RMB: end (no point)"
                                      : "RMB: place a point and end" );
                return;
            }
            float a[3], d[3];
            LastPoint( a );
            Sub3( m_cur, a, d );
            char b[32], dz[32];
            KiwiUnits_Format( b,  sizeof( b ),  Len3( d ) );
            KiwiUnits_Format( dz, sizeof( dz ), d[2] );
            // Advertise both the aimable Z guide and the explicit Z lock.
            SetHud( "curve  %i pts  seg %s  dz %s%s  ·  %s  ·  %s  ·  "
                    "Enter: finish here  ·  "
                    "Esc: drop last  ·  vertical: aim the Z guide%s",
                    n, b, dz, m_zLock ? "  [Z LOCK]" : "",
                    ( n >= 3 && NearFirstPoint() )
                        ? "click: CLOSE the loop"
                        : "click: next point",
                    m_anyLmbPoint ? "RMB: end (no point)"
                                  : "RMB: place a point and end",
                    m_zLock ? ", or Z (LOCKED)" : ", or press Z" );
        }

        void Finish() override
        {
            // Closed chains omit the cursor tail to avoid a duplicate seam.
            std::vector<float> pts = m_pts;
            // Append a tail only below the cap and never for post-LMB RMB finish.
            if ( !m_wantClosed && m_haveCur && !m_noTailPoint
              && PointCount() < KCON_MAX_POINTS )
            {
                // Avoid a duplicate tail at the last committed point.
                float last[3], d[3];
                bool  take = true;
                if ( !m_pts.empty() )
                {
                    LastPoint( last );
                    Sub3( m_cur, last, d );
                    take = ( Len3( d ) >= 1.0e-3f );
                }
                if ( take )
                {
                    pts.push_back( m_cur[0] );
                    pts.push_back( m_cur[1] );
                    pts.push_back( m_cur[2] );
                }
            }
            if ( (int)( pts.size() / 3 ) < 2 )
            {
                // Only worth saying when the user HAD started a chain: finishing a
                // tool that was opened and immediately dismissed is not an error.
                if ( !m_pts.empty() )
                    Sys_Printf( "Curve: needs at least two points — nothing placed.\n" );
                m_pts.clear();
                m_wantClosed  = false;
                m_noTailPoint = false;
                m_anyLmbPoint = false;
                return;
            }
            kconObject_t o;
            // The tree's own convention for "a chain that came out at two points"
            // (kiwi_trim.cpp:558, kiwi_offset.cpp:577, kiwi_conselect.cpp:826).
            o.type   = ( pts.size() == 6 ) ? KCON_LINE : KCON_POLYLINE;
            o.plane  = m_plane;                    // seed only; refit on Add
            o.pts    = pts;
            o.closed = m_wantClosed;
            // Announce success only when the store accepted the object.
            if ( KiwiCon_AddWithUndo( o ) >= 0 )
                Sys_Printf( "Curve: %i points%s.\n", (int)( pts.size() / 3 ),
                            m_wantClosed ? ", closed" : "" );
            m_pts.clear();
            m_wantClosed  = false;
            m_noTailPoint = false;
            m_anyLmbPoint = false;
        }

    private:
        bool m_wantClosed  = false;
        // One-gesture RMB finish-without-tail latch.
        bool m_noTailPoint = false;
        // One-chain LMB-history latch controlling RMB grammar and hints.
        bool m_anyLmbPoint = false;
    };

    // ── RECT — two corners, axis-aligned IN PLANE ───────────────────────────
    class KiwiRectTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Rectangle"; }

        bool OnClick( bool ) override
        {
            if ( PointCount() == 0 )
            {
                PushPoint( m_cur );
                UpdateHud();
                return true;
            }
            Finish();
            return false;
        }

        void Recompute() override
        {
            // A typed value sizes the rect SQUARE (one scalar, grammar v1): it is
            // the extent along both in-plane axes, signed by the drag direction.
            if ( m_hasNum && PointCount() == 1 )
            {
                float a[2], c[2];
                LastPointUV( a );
                CurUV( c );
                const float su = ( c[0] >= a[0] ) ? 1.0f : -1.0f;
                const float sv = ( c[1] >= a[1] ) ? 1.0f : -1.0f;
                c[0] = a[0] + su * m_numWorld;
                c[1] = a[1] + sv * m_numWorld;
                SetCurUV( c );
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            if ( PointCount() == 0 )
            {
                SetHud( "rect  ·  click: first corner" );
                return;
            }
            float a[2], c[2];
            LastPointUV( a );
            CurUV( c );
            char bu[32], bv[32];
            KiwiUnits_Format( bu, sizeof( bu ), fabsf( c[0] - a[0] ) );
            KiwiUnits_Format( bv, sizeof( bv ), fabsf( c[1] - a[1] ) );
            SetHud( "rect  %s x %s  ·  click: opposite corner  ·  type = square size", bu, bv );
        }

        void Finish() override
        {
            if ( PointCount() != 1 || !m_haveCur )
                return;
            float a[2], c[2];
            LastPointUV( a );
            CurUV( c );
            if ( fabsf( c[0] - a[0] ) < 1.0e-3f || fabsf( c[1] - a[1] ) < 1.0e-3f )
            {
                Sys_Printf( "Rect: zero extent — nothing placed.\n" );
                m_pts.clear();
                return;
            }

            kconObject_t o;
            o.type   = KCON_RECT;
            o.plane  = m_plane;
            o.closed = true;
            // CCW in plane space so a region derived from it needs no rewind; then
            // straight out to WORLD, which is what the store holds now.
            const float u0 = ( a[0] < c[0] ) ? a[0] : c[0];
            const float u1 = ( a[0] < c[0] ) ? c[0] : a[0];
            const float v0 = ( a[1] < c[1] ) ? a[1] : c[1];
            const float v1 = ( a[1] < c[1] ) ? c[1] : a[1];
            const float uv[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
            EmitRectQuad( m_plane, uv, &o );
            KiwiCon_AddWithUndo( o );
            m_pts.clear();
        }

        void DrawWorld() override
        {
            if ( PointCount() != 1 || !m_haveCur )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            float a[2], cu[2];
            LastPointUV( a );
            CurUV( cu );
            const float c[4][2] = { { a[0], a[1] }, { cu[0], a[1] },
                                    { cu[0], cu[1] }, { a[0], cu[1] } };
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            DrawRectQuad( m_plane, c );
        }
    };

    // ── CIRCLE — centre, then radius (click or typed) ───────────────────────
    class KiwiCircleTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Circle"; }

        bool WantsSidesField() const override { return true; }

        // Always expose the effective AUTO/override count so the sides field is
        // visible before typing and agrees with generated geometry.
        int ToolSides() const override
        {
            return ( m_sidesOverride > 0 ) ? CardinalSegs( m_sidesOverride )
                                           : CircleSegs( m_radius );
        }

        bool OnClick( bool ) override
        {
            if ( PointCount() == 0 )
            {
                PushPoint( m_cur );
                UpdateHud();
                return true;
            }
            Finish();
            return false;
        }

        void Recompute() override
        {
            // The base owns the per-gesture side override.
            m_radius = 0.0f;
            if ( PointCount() == 1 )
            {
                float c[2], cur[2];
                LastPointUV( c );
                CurUV( cur );
                const float d0 = cur[0] - c[0], d1 = cur[1] - c[1];
                m_radius = m_hasNum ? m_numWorld : sqrtf( d0 * d0 + d1 * d1 );
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            if ( PointCount() == 0 )
            {
                SetHud( "circle  ·  click: centre" );
                return;
            }
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), m_radius );
            // HUD and Tab field report the same cardinal-rounded count as geometry.
            const int hudSegs = ToolSides();
            SetHud( "circle  radius %s  (%i segs, x4 for the quadrants)  ·  "
                    "click: rim  ·  type = radius  ·  Tab = sides",
                    b, hudSegs );
        }

        void Finish() override
        {
            if ( PointCount() != 1 )
                return;
            if ( !( m_radius > 1.0e-3f ) )
            {
                Sys_Printf( "Circle: zero radius — nothing placed.\n" );
                m_pts.clear();
                return;
            }
            float c[2];
            LastPointUV( c );
            kconObject_t o;
            o.type      = KCON_CIRCLE;
            o.plane     = m_plane;
            o.closed    = true;
            o.centre[0] = c[0];
            o.centre[1] = c[1];
            o.radius    = m_radius;
            o.ang0      = 0.0f;
            o.ang1      = 360.0f;
            o.segs      = m_sidesOverride;    // 0 = automatic
            KiwiCon_AddWithUndo( o );
            m_pts.clear();
        }

        void DrawWorld() override
        {
            if ( PointCount() != 1 || !( m_radius > 1.0e-3f ) )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            float c[2];
            LastPointUV( c );
            kconObject_t preview;
            preview.type      = KCON_CIRCLE;
            preview.plane     = m_plane;
            preview.closed    = true;
            preview.centre[0] = c[0];
            preview.centre[1] = c[1];
            preview.radius    = m_radius;
            // Propagate the per-gesture count into the throwaway preview object.
            preview.segs      = m_sidesOverride;
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            DrawObjectPreview( preview );
        }

    private:
        float m_radius = 0.0f;
    };

    // ── ARC — centre, then radius+start angle, then end angle ───────────────
    class KiwiArcTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Arc"; }

        bool WantsSidesField() const override { return true; }

        // Report full-circle density; ArcSegs prorates it over the sweep.
        int ToolSides() const override
        {
            // Match the cardinal-rounded density used by arc tessellation.
            return ( m_sidesOverride > 0 ) ? CardinalSegs( m_sidesOverride )
                                           : CircleSegs( m_radius );
        }

        bool Begin() override
        {
            m_stage  = 0;
            m_radius = 0.0f;
            m_a0     = 0.0f;
            m_a1     = 0.0f;
            return KiwiDrawTool::Begin();
        }

        bool OnClick( bool ) override
        {
            if ( m_stage == 0 )                    // centre
            {
                PushPoint( m_cur );
                m_stage = 1;
                UpdateHud();
                return true;
            }
            if ( m_stage == 1 )                    // radius + start angle
                return AdvanceToSweep( "Arc: zero radius — pick a point away "
                                       "from the centre." );
            Finish();                              // third click closes the sweep
            return false;
        }

        // Enter advances radius to sweep, then confirms at the final stage.
        bool AdvanceStage() override
        {
            if ( m_stage == 0 )
            {
                PushPoint( m_cur );
                m_stage = 1;
                UpdateHud();
                return true;
            }
            if ( m_stage == 1 )
                return AdvanceToSweep( "Arc: zero radius — type one, or pick a point "
                                       "away from the centre." );
            return false;                          // stage 2 is final — Enter confirms
        }

        // Shared radius-to-sweep transition. Clear the numeric entry because its
        // meaning changes from WORLD-SPACE radius to degrees; preserve field tables.
        bool AdvanceToSweep( const char *refusalMsg )
        {
            if ( !( m_radius > 1.0e-3f ) )
            {
                Sys_Printf( "%s\n", refusalMsg );
                return true;                       // consumed; do NOT fall through to Finish
            }
            m_lockRadius = m_radius;
            m_lockA0     = m_a0;
            m_stage      = 2;
            KiwiNum_ClearEntry();
            m_hasNum   = false;
            m_numWorld = 0.0f;
            UpdateHud();
            return true;
        }

        void Recompute() override
        {
            if ( m_stage >= 1 && PointCount() == 1 )
            {
                float c[2], cur[2];
                LastPointUV( c );
                CurUV( cur );
                const float d0 = cur[0] - c[0], d1 = cur[1] - c[1];
                const float ang = atan2f( d1, d0 ) * KCON_RAD2DEG;
                if ( m_stage == 1 )
                {
                    m_radius = m_hasNum ? m_numWorld : sqrtf( d0 * d0 + d1 * d1 );
                    m_a0     = ang;
                    m_a1     = ang;
                }
                else
                {
                    m_radius = m_lockRadius;
                    m_a0     = m_lockA0;
                    // Typed sweep is degrees; undo the numeric layer's length conversion.
                    float sweep = m_hasNum ? Units_ToDisplay( m_numWorld ) : ( ang - m_a0 );
                    while ( sweep <    0.0f ) sweep += 360.0f;
                    while ( sweep >  360.0f ) sweep -= 360.0f;
                    m_a1 = m_a0 + sweep;
                }
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            if ( m_stage == 0 )
            {
                SetHud( "arc  ·  click: centre" );
                return;
            }
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), m_radius );
            if ( m_stage == 1 )
                SetHud( "arc  radius %s  start %.1f deg  ·  click: start point  ·  type = radius",
                        b, (double)m_a0 );
            else
                SetHud( "arc  radius %s  sweep %.1f deg  ·  click: end point  ·  type = sweep (deg)",
                        b, (double)( m_a1 - m_a0 ) );
        }

        void Finish() override
        {
            if ( m_stage < 2 || PointCount() != 1 )
            {
                m_pts.clear();
                return;
            }
            if ( fabsf( m_a1 - m_a0 ) < 0.5f || !( m_radius > 1.0e-3f ) )
            {
                Sys_Printf( "Arc: zero sweep — nothing placed.\n" );
                m_pts.clear();
                return;
            }
            float c[2];
            LastPointUV( c );
            kconObject_t o;
            o.type      = KCON_ARC;
            o.plane     = m_plane;
            o.closed    = false;
            o.centre[0] = c[0];
            o.centre[1] = c[1];
            o.radius    = m_radius;
            o.ang0      = m_a0;
            o.ang1      = m_a1;
            o.segs      = m_sidesOverride;    // 0 = automatic
            KiwiCon_AddWithUndo( o );
            m_pts.clear();
            m_stage = 0;
        }

        void DrawWorld() override
        {
            if ( m_stage < 1 || PointCount() != 1 || !( m_radius > 1.0e-3f ) )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            float c[2];
            LastPointUV( c );
            kconObject_t preview;
            preview.type      = ( m_stage == 1 ) ? KCON_LINE : KCON_ARC;
            preview.plane     = m_plane;
            preview.centre[0] = c[0];
            preview.centre[1] = c[1];
            preview.radius    = m_radius;
            preview.ang0      = m_a0;
            preview.ang1      = ( m_stage == 1 ) ? m_a0 : m_a1;
            preview.segs      = m_sidesOverride;

            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            if ( m_stage == 1 )
            {
                // Stage 1 shows the radius handle, not a zero-length arc.
                float w0[3];
                KiwiCon_PlaneToWorld( m_plane, c, w0 );
                KiwiLines_Add( w0, m_cur );
                return;
            }
            DrawObjectPreview( preview );
        }

    private:
        int   m_stage      = 0;
        float m_radius     = 0.0f;
        float m_a0         = 0.0f;
        float m_a1         = 0.0f;
        float m_lockRadius = 0.0f;
        float m_lockA0     = 0.0f;
    };

    // Additional construction curve tools.
    // Center rectangle: center first, then a mirrored corner.
    class KiwiRectCenterTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Rectangle (center)"; }

        bool OnClick( bool ) override
        {
            if ( PointCount() == 0 )
            {
                PushPoint( m_cur );
                UpdateHud();
                return true;
            }
            Finish();
            return false;
        }

        void Recompute() override
        {
            // Typed value is center-to-edge half-size, yielding a 2N x 2N rectangle.
            if ( m_hasNum && PointCount() == 1 )
            {
                float c[2], cur[2];
                LastPointUV( c );
                CurUV( cur );
                const float su = ( cur[0] >= c[0] ) ? 1.0f : -1.0f;
                const float sv = ( cur[1] >= c[1] ) ? 1.0f : -1.0f;
                cur[0] = c[0] + su * m_numWorld;
                cur[1] = c[1] + sv * m_numWorld;
                SetCurUV( cur );
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            if ( PointCount() == 0 )
            {
                SetHud( "rect (center)  ·  click: centre" );
                return;
            }
            float c[2], cur[2];
            LastPointUV( c );
            CurUV( cur );
            char bu[32], bv[32];
            KiwiUnits_Format( bu, sizeof( bu ), fabsf( cur[0] - c[0] ) * 2.0f );
            KiwiUnits_Format( bv, sizeof( bv ), fabsf( cur[1] - c[1] ) * 2.0f );
            SetHud( "rect (center)  %s x %s  ·  click: corner  ·  type = half-size", bu, bv );
        }

        void Finish() override
        {
            if ( PointCount() != 1 || !m_haveCur )
                return;
            float c[2], cur[2];
            LastPointUV( c );
            CurUV( cur );
            const float hu = fabsf( cur[0] - c[0] );
            const float hv = fabsf( cur[1] - c[1] );
            if ( hu < 1.0e-3f || hv < 1.0e-3f )
            {
                Sys_Printf( "Rect (center): zero extent — nothing placed.\n" );
                m_pts.clear();
                return;
            }

            kconObject_t o;
            o.type   = KCON_RECT;
            o.plane  = m_plane;
            o.closed = true;
            // CCW, same convention as KiwiRectTool; out to WORLD as the store wants.
            const float uv[4][2] = { { c[0] - hu, c[1] - hv }, { c[0] + hu, c[1] - hv },
                                     { c[0] + hu, c[1] + hv }, { c[0] - hu, c[1] + hv } };
            EmitRectQuad( m_plane, uv, &o );
            KiwiCon_AddWithUndo( o );
            m_pts.clear();
        }

        void DrawWorld() override
        {
            if ( PointCount() != 1 || !m_haveCur )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            float c[2], cur[2];
            LastPointUV( c );
            CurUV( cur );
            const float hu = fabsf( cur[0] - c[0] );
            const float hv = fabsf( cur[1] - c[1] );
            const float q[4][2] = { { c[0] - hu, c[1] - hv }, { c[0] + hu, c[1] - hv },
                                    { c[0] + hu, c[1] + hv }, { c[0] - hu, c[1] + hv } };
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            DrawRectQuad( m_plane, q );
        }
    };

    // Two-point circle: clicks are diameter endpoints; center is their midpoint.
    class KiwiCircle2PtTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Circle (2-point)"; }

        bool WantsSidesField() const override { return true; }

        // Use the same cardinal-rounded density as other circles.
        int ToolSides() const override
        {
            return ( m_sidesOverride > 0 ) ? CardinalSegs( m_sidesOverride )
                                           : CircleSegs( m_radius );
        }

        bool OnClick( bool ) override
        {
            if ( PointCount() == 0 )
            {
                PushPoint( m_cur );
                UpdateHud();
                return true;
            }
            Finish();
            return false;
        }

        void Recompute() override
        {
            m_radius = 0.0f;
            if ( PointCount() == 1 )
            {
                float a[2], cur[2];
                LastPointUV( a );
                CurUV( cur );
                float d[2] = { cur[0] - a[0], cur[1] - a[1] };
                const float len = sqrtf( d[0] * d[0] + d[1] * d[1] );
                // Typed value sizes the diameter along the current direction.
                if ( m_hasNum && len > 1.0e-4f )
                {
                    cur[0] = a[0] + d[0] / len * m_numWorld;
                    cur[1] = a[1] + d[1] / len * m_numWorld;
                    SetCurUV( cur );
                    d[0] = cur[0] - a[0];
                    d[1] = cur[1] - a[1];
                }
                m_radius = 0.5f * sqrtf( d[0] * d[0] + d[1] * d[1] );
                m_centre[0] = ( a[0] + cur[0] ) * 0.5f;
                m_centre[1] = ( a[1] + cur[1] ) * 0.5f;
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            if ( PointCount() == 0 )
            {
                SetHud( "circle (2-point)  ·  click: first diameter end" );
                return;
            }
            char br[32], bd[32];
            KiwiUnits_Format( br, sizeof( br ), m_radius );
            KiwiUnits_Format( bd, sizeof( bd ), m_radius * 2.0f );
            SetHud( "circle (2-point)  dia %s  r %s  (%i segs, x4 for the quadrants)  ·  "
                    "click: second end  ·  type = diameter  ·  Tab = sides",
                    bd, br, ToolSides() );
        }

        void Finish() override
        {
            if ( PointCount() != 1 )
                return;
            if ( !( m_radius > 1.0e-3f ) )
            {
                Sys_Printf( "Circle (2-point): zero diameter — nothing placed.\n" );
                m_pts.clear();
                return;
            }
            kconObject_t o;
            o.type      = KCON_CIRCLE;
            o.plane     = m_plane;
            o.closed    = true;
            o.centre[0] = m_centre[0];
            o.centre[1] = m_centre[1];
            o.radius    = m_radius;
            o.ang0      = 0.0f;
            o.ang1      = 360.0f;
            o.segs      = m_sidesOverride;    // 0 = automatic
            KiwiCon_AddWithUndo( o );
            m_pts.clear();
        }

        void DrawWorld() override
        {
            if ( PointCount() != 1 || !( m_radius > 1.0e-3f ) )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            kconObject_t preview;
            preview.type      = KCON_CIRCLE;
            preview.plane     = m_plane;
            preview.closed    = true;
            preview.centre[0] = m_centre[0];
            preview.centre[1] = m_centre[1];
            preview.radius    = m_radius;
            preview.segs      = m_sidesOverride;
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            if ( !DrawObjectPreview( preview ) )
                return;
            // The diameter handle, so the gesture reads as "two ends", not "rim".
            float first[3];
            FirstPoint( first );
            KiwiLines_Add( first, m_cur );
        }

    private:
        float m_radius    = 0.0f;
        float m_centre[2] = { 0.0f, 0.0f };
    };

    // Polygon: center then radius. Digits remain the radius field; [ and ] adjust
    // the shared, clamped side count while the modal tool owns those keys.
    class KiwiPolygonTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Polygon"; }

        bool Begin() override
        {
            // Begin first seeds the base override, then derives the local side count.
            m_radius = 0.0f;
            m_ang0   = 0.0f;
            // The base seeds m_sidesOverride from the remembered value, so it has to
            // run FIRST — PolySides reads what it wrote.
            const bool ok = KiwiDrawTool::Begin();
            m_sides = PolySides();
            return ok;
        }

        bool KeyDown( int vk, unsigned int mods ) override
        {
            if ( !mods && ( vk == 0xDB || vk == 0xDD ) )   // VK_OEM_4 '[' / VK_OEM_6 ']'
            {
                m_sides += ( vk == 0xDD ) ? 1 : -1;
                if ( m_sides < KCON_POLY_SIDES_MIN ) m_sides = KCON_POLY_SIDES_MIN;
                if ( m_sides > KCON_POLY_SIDES_MAX ) m_sides = KCON_POLY_SIDES_MAX;
                // Keep the numeric field and persisted session value in sync.
                m_sidesOverride = m_sides;
                KiwiCon_SetToolSides( m_sides );
                UpdateHud();
                g_nUpdateBits |= 1;
                return true;
            }
            return KiwiDrawTool::KeyDown( vk, mods );
        }

        bool OnClick( bool ) override
        {
            if ( PointCount() == 0 )
            {
                PushPoint( m_cur );
                UpdateHud();
                return true;
            }
            Finish();
            return false;
        }

        void Recompute() override
        {
            // Recompute local sides from the base override so typed fields, keys,
            // preview, and committed vertices cannot diverge.
            m_sides  = PolySides();
            m_radius = 0.0f;
            if ( PointCount() == 1 )
            {
                float c[2], cur[2];
                LastPointUV( c );
                CurUV( cur );
                const float d0 = cur[0] - c[0], d1 = cur[1] - c[1];
                // Cursor bearing rotates the first vertex; typed input changes radius only.
                m_ang0   = atan2f( d1, d0 );
                m_radius = m_hasNum ? m_numWorld : sqrtf( d0 * d0 + d1 * d1 );
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            if ( PointCount() == 0 )
            {
                SetHud( "polygon  %i sides  ·  click: centre  ·  [ ] change sides", m_sides );
                return;
            }
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), m_radius );
            SetHud( "polygon  %i sides  r %s  ·  click: radius  ·  type = radius  ·  [ ] sides",
                    m_sides, b );
        }

        void Finish() override
        {
            if ( PointCount() != 1 )
                return;
            if ( !( m_radius > 1.0e-3f ) )
            {
                Sys_Printf( "Polygon: zero radius — nothing placed.\n" );
                m_pts.clear();
                return;
            }
            float c[2];
            LastPointUV( c );

            // Store a CCW WORLD-SPACE closed polyline; no sidecar type is needed.
            kconObject_t o;
            o.type   = KCON_POLYLINE;
            o.plane  = m_plane;
            o.closed = true;
            o.pts.reserve( (size_t)m_sides * 3 );
            for ( int i = 0; i < m_sides; ++i )
            {
                const float a = m_ang0 + KCON_TWO_PI * (float)i / (float)m_sides;
                const float uv[2] = { c[0] + cosf( a ) * m_radius,
                                      c[1] + sinf( a ) * m_radius };
                float w[3];
                KiwiCon_PlaneToWorld( m_plane, uv, w );
                o.pts.push_back( w[0] );  o.pts.push_back( w[1] );  o.pts.push_back( w[2] );
            }
            KiwiCon_AddWithUndo( o );
            m_pts.clear();
        }

        void DrawWorld() override
        {
            if ( PointCount() != 1 || !( m_radius > 1.0e-3f ) )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            float c[2];
            LastPointUV( c );
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            for ( int i = 0; i < m_sides; ++i )
            {
                const float a0 = m_ang0 + KCON_TWO_PI * (float)i / (float)m_sides;
                const float a1 = m_ang0 + KCON_TWO_PI * (float)( i + 1 ) / (float)m_sides;
                const float uv0[2] = { c[0] + cosf( a0 ) * m_radius, c[1] + sinf( a0 ) * m_radius };
                const float uv1[2] = { c[0] + cosf( a1 ) * m_radius, c[1] + sinf( a1 ) * m_radius };
                float w0[3], w1[3];
                KiwiCon_PlaneToWorld( m_plane, uv0, w0 );
                KiwiCon_PlaneToWorld( m_plane, uv1, w1 );
                if ( !KiwiLines_Add( w0, w1 ) )
                    return;
            }
        }

    private:
        // Polygon always reports a definite local side count.
        bool WantsSidesField() const override { return true; }
        int  ToolSides() const override       { return m_sides; }
        int  PolySides() const
        {
            int n = ( m_sidesOverride > 0 ) ? m_sidesOverride : KCON_POLY_SIDES_DEF;
            if ( n < KCON_POLY_SIDES_MIN ) n = KCON_POLY_SIDES_MIN;
            if ( n > KCON_POLY_SIDES_MAX ) n = KCON_POLY_SIDES_MAX;
            return n;
        }

        int   m_sides  = KCON_POLY_SIDES_DEF;
        float m_radius = 0.0f;
        float m_ang0   = 0.0f;
    };

    // Spline control clicks tessellate to a WORLD-SPACE KCON_POLYLINE; Enter
    // finishes and clicking the first control closes.
    class KiwiSplineTool : public KiwiDrawTool
    {
    public:
        const char *Name() const override { return "Construct Spline"; }
        bool WantsEnterFinish() const override { return PointCount() >= 2; }
        bool PlanarOnly() const override { return false; }

        bool Begin() override
        {
            m_wantClosed = false;
            return KiwiDrawTool::Begin();
        }

        bool OnClick( bool doubleClick ) override
        {
            if ( PointCount() >= 3 && NearFirstPoint() )
            {
                m_wantClosed = true;
                Finish();
                return false;
            }
            if ( doubleClick && PointCount() >= 2 )
            {
                Finish();
                return false;
            }
            // Cap controls early enough that their tessellation fits the store budget.
            if ( PointCount() >= KCON_MAX_POINTS / KCON_SPLINE_SEGS )
            {
                Sys_Printf( "Spline: control-point limit reached — ending here.\n" );
                Finish();
                return false;
            }
            PushPoint( m_cur );
            UpdateHud();
            return true;
        }

        void Recompute() override
        {
            if ( m_hasNum && PointCount() >= 1 )
            {
                float a[3], d[3];
                LastPoint( a );
                Sub3( m_cur, a, d );
                const float l = Len3( d );
                if ( l > 1.0e-4f )
                    Mad3( a, d, m_numWorld / l, m_cur );
            }
            UpdateHud();
        }

        void UpdateHud() override
        {
            const int n = PointCount();
            if ( n == 0 )
            {
                SetHud( "spline  ·  click: first control point" );
                return;
            }
            SetHud( "spline  %i pts  (%i segs/span)%s  ·  %s  ·  Z: vertical %s",
                    n, KCON_SPLINE_SEGS, m_zLock ? "  [Z LOCK]" : "",
                    ( n >= 3 && NearFirstPoint() )
                        ? "click: CLOSE the curve"
                        : "click: next point  ·  RMB/Enter: finish",
                    m_zLock ? "ON" : "off" );
        }

        void Finish() override
        {
            std::vector<float> ctrl = m_pts;
            if ( !m_wantClosed && m_haveCur )
            {
                ctrl.push_back( m_cur[0] );
                ctrl.push_back( m_cur[1] );
                ctrl.push_back( m_cur[2] );
            }
            if ( (int)( ctrl.size() / 3 ) < 2 )
            {
                // An immediate dismissal is silent; a started invalid chain reports.
                if ( !m_pts.empty() )
                    Sys_Printf( "Spline: needs at least two control points — nothing placed.\n" );
                m_pts.clear();
                return;
            }

            kconObject_t o;
            o.type   = KCON_POLYLINE;
            o.plane  = m_plane;
            o.closed = m_wantClosed;
            TessellateSpline( ctrl, m_wantClosed, &o.pts );
            if ( (int)( o.pts.size() / 3 ) < 2 )
            {
                // Coincident controls can collapse tessellation entirely.
                if ( !m_pts.empty() )
                    Sys_Printf( "Spline: %i control points tessellated to nothing — nothing placed.\n",
                                (int)( ctrl.size() / 3 ) );
                m_pts.clear();
                m_wantClosed = false;
                return;
            }
            KiwiCon_AddWithUndo( o );
            m_pts.clear();
            m_wantClosed = false;
        }

        void DrawWorld() override
        {
            std::vector<float> ctrl = m_pts;
            if ( m_haveCur )
            {
                ctrl.push_back( m_cur[0] );
                ctrl.push_back( m_cur[1] );
                ctrl.push_back( m_cur[2] );
            }
            if ( (int)( ctrl.size() / 3 ) < 2 )
            {
                KiwiDrawTool::DrawWorld();
                return;
            }
            std::vector<float> tess;
            TessellateSpline( ctrl, false, &tess );
            KiwiLines_Color( KCON_COL_ACTIVE[0], KCON_COL_ACTIVE[1], KCON_COL_ACTIVE[2] );
            const int n = (int)( tess.size() / 3 );
            for ( int i = 0; i + 1 < n; ++i )
                if ( !KiwiLines_Add( &tess[(size_t)i * 3], &tess[( (size_t)i + 1 ) * 3] ) )
                    return;
        }

    private:
        bool m_wantClosed = false;
    };

    // Line and Polyline command ids share one tool instance.
    KiwiCurveTool      s_curveTool;
    KiwiRectTool       s_rectTool;
    KiwiCircleTool     s_circleTool;
    KiwiArcTool        s_arcTool;
    KiwiRectCenterTool s_rectCenterTool;
    KiwiCircle2PtTool  s_circle2PtTool;
    KiwiPolygonTool    s_polygonTool;
    KiwiSplineTool     s_splineTool;
}

bool KiwiCon_ToolActive()
{
    return s_activeTool != 0;
}

namespace
{
    bool s_planePlacement = false;
}

void KiwiCon_SetPlanePlacement( bool on )
{
    s_planePlacement = on;
}

bool KiwiCon_PlanePlacement()
{
    // True only for intrinsically planar draw tools or an announced planar
    // primitive; free curves must bypass every plane-borne snap branch.
    return ( s_activeTool != 0 && s_activeToolPlanar ) || s_planePlacement;
}

bool KiwiCon_ToolAnchor( float out[3] )
{
    if ( !s_activeTool || s_activeTool->PointCount() < 1 )
        return false;
    s_activeTool->LastPoint( out );
    return true;
}

bool KiwiCon_ToolLoopStart( float out[3] )
{
    // Fewer than three points cannot form a loop-closing snap target.
    if ( !s_activeTool || s_activeTool->PointCount() < 3 )
        return false;
    s_activeTool->FirstPoint( out );
    return true;
}

bool KiwiCon_ToolPrevAnchor( float out[3] )
{
    if ( !s_activeTool || s_activeTool->PointCount() < 2 )
        return false;
    s_activeTool->PrevPoint( out );
    return true;
}

// Camera draw tail.
namespace
{
    // Screen-space filled-dot anchors use the shared ortho-aware world-per-pixel
    // scale: 2.5 px normally, 3.5 px when selected, 12 line segments each.
    const int   KCON_DOT_SEGS      = 8;
    const float KCON_DOT_PIX       = 2.5f;
    const float KCON_DOT_PIX_SEL   = 3.5f;

    void DrawFilledDot( const camera_s *c, const float *p, float pixRadius )
    {
        const float r = KiwiCam_WorldPerPixel( p ) * pixRadius;
        float prev[3], pt[3];
        for ( int i = 0; i <= KCON_DOT_SEGS; ++i )
        {
            const float th = ( KCON_TWO_PI * (float)( i % KCON_DOT_SEGS ) )
                           / (float)KCON_DOT_SEGS;
            const float cx = cosf( th ) * r;
            const float cy = sinf( th ) * r;
            for ( int k = 0; k < 3; ++k )
                pt[k] = p[k] + c->vright[k] * cx + c->vup[k] * cy;
            if ( i > 0 )
                KiwiLines_Add( prev, pt );
            Copy3( pt, prev );
        }
        // The long diagonals: chord i to chord i+segs/2, which is what fills it.
        for ( int i = 0; i < KCON_DOT_SEGS / 2; ++i )
        {
            const float th0 = ( KCON_TWO_PI * (float)i ) / (float)KCON_DOT_SEGS;
            const float th1 = th0 + KCON_PI;
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

void KiwiCon_DrawWorld()
{
    if ( !KiwiCon_ShowConstruction() )
        return;

    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;

    // Region fills draw in Cam_Draw's command-overlay slot under the same toggle.
    KiwiLines_Begin( KCON_DRAW_SEGMENTS, 1 );

    // Draw unselected then selected-on-top. Two color runs avoid fragmenting the
    // line batch per object, and white matches legacy brush selection.
    for ( int pass = 0; pass < 2; ++pass )
    {
        const bool wantSel = ( pass == 1 );
        if ( wantSel ) KiwiLines_Color( KCON_COL_SEL_LINE[0],  KCON_COL_SEL_LINE[1],  KCON_COL_SEL_LINE[2] );
        else           KiwiLines_Color( KCON_COL_LINE[0],      KCON_COL_LINE[1],      KCON_COL_LINE[2] );

        for ( size_t i = 0; i < s_objects.size(); ++i )
        {
            const kconObject_t &o = s_objects[i];
            if ( o.hidden )                   // hidden objects are inert
                continue;
            // Skip whole unselected objects in the selected pass; only objects with
            // partial selection need per-segment membership checks.
            const bool anySel = KiwiConSel_ObjectSelected( (int)i );
            if ( wantSel && !anySel )
                continue;
            const int segs = KiwiCon_SegmentCount( o );
            for ( int s = 0; s < segs; ++s )
            {
                if ( anySel && KiwiConSel_SegmentSelected( (int)i, s ) != wantSel )
                    continue;
                float a[3], b[3];
                if ( !KiwiCon_SegmentWorld( o, s, a, b ) )
                    break;
                if ( !KiwiLines_Add( a, b ) )
                {
                    KiwiLines_Flush();
                    return;
                }
            }
        }
    }

    // Retrace hovered object/group curves in shared hover cyan after selection.
    {
        const int hovObj = KiwiHover_OutlinerConIndex();
        const int hovGrp = KiwiHover_OutlinerConGroupId();
        if ( hovObj >= 0 || hovGrp != 0 )
        {
            KiwiLines_Color( KCON_COL_HOVER[0], KCON_COL_HOVER[1], KCON_COL_HOVER[2] );
            for ( size_t i = 0; i < s_objects.size(); ++i )
            {
                const kconObject_t &o = s_objects[i];
                if ( o.hidden )
                    continue;
                if ( (int)i != hovObj && !( hovGrp != 0 && o.group == hovGrp ) )
                    continue;
                const int segs = KiwiCon_SegmentCount( o );
                for ( int s = 0; s < segs; ++s )
                {
                    float a[3], b[3];
                    if ( !KiwiCon_SegmentWorld( o, s, a, b ) )
                        break;
                    if ( !KiwiLines_Add( a, b ) )
                    {
                        KiwiLines_Flush();
                        return;
                    }
                }
            }
        }
    }

    for ( int pass = 0; pass < 2; ++pass )
    {
        const bool wantSel = ( pass == 1 );
        if ( wantSel ) KiwiLines_Color( KCON_COL_SEL_POINT[0], KCON_COL_SEL_POINT[1], KCON_COL_SEL_POINT[2] );
        else           KiwiLines_Color( KCON_COL_POINT[0],     KCON_COL_POINT[1],     KCON_COL_POINT[2] );

        for ( size_t i = 0; i < s_objects.size(); ++i )
        {
            const kconObject_t &o = s_objects[i];
            if ( o.hidden )                   // hidden objects are inert
                continue;
            const bool anySel = KiwiConSel_ObjectSelected( (int)i );
            if ( wantSel && !anySel )
                continue;
            const int anchors = KiwiCon_AnchorCount( o );
            for ( int a = 0; a < anchors; ++a )
            {
                if ( anySel && KiwiConSel_PointSelected( (int)i, a ) != wantSel )
                    continue;
                float p[3];
                if ( !KiwiCon_AnchorWorld( o, a, p ) )
                    break;
                // Reserve enough budget to avoid a partially drawn 12-segment dot.
                if ( KiwiLines_Remaining() < 16 )
                {
                    KiwiLines_Flush();
                    return;
                }
                DrawFilledDot( c, p, wantSel ? KCON_DOT_PIX_SEL : KCON_DOT_PIX );
            }
        }
    }

    KiwiLines_Flush();
}

// Settings.
// One lazy-loaded, persisted side count is shared by all round tools; 0=AUTO.
static int s_toolSides = -1;      // -1 = not loaded yet

int KiwiCon_ToolSides()
{
    if ( s_toolSides < 0 )
    {
        int n = Radiant_ProfileGetInt( KCON_SECTION, "RoundToolSides", 0 );
        if ( n != 0 )
            n = ClampSides( n );
        s_toolSides = n;
    }
    return s_toolSides;
}

void KiwiCon_SetToolSides( int sides )
{
    int n = sides;
    if ( n != 0 )
        n = ClampSides( n );
    if ( n == s_toolSides )
        return;
    s_toolSides = n;
    Radiant_ProfileSetInt( KCON_SECTION, "RoundToolSides", n );
}

bool KiwiCon_ShowConstruction()
{
    if ( s_showConstruction < 0 )
        s_showConstruction = Radiant_ProfileGetInt( KCON_SECTION, "ShowConstruction", 1 ) ? 1 : 0;
    return s_showConstruction != 0;
}

void KiwiCon_SetShowConstruction( bool on )
{
    const int v = on ? 1 : 0;
    if ( s_showConstruction == v )
        return;
    s_showConstruction = v;
    Radiant_ProfileSetInt( KCON_SECTION, "ShowConstruction", v );
    g_nUpdateBits |= 1;
}

// Sidecar persistence.
namespace
{
    // Replace only the filename extension; dots in directory names are preserved.
    bool SidecarPath( const char *mapPath, char *out, int outSize )
    {
        if ( !mapPath || !mapPath[0] || outSize < 8 )
            return false;
        _snprintf( out, outSize, "%s", mapPath );
        out[outSize - 1] = '\0';

        char *lastSep = 0;
        for ( char *p = out; *p; ++p )
            if ( *p == '/' || *p == '\\' )
                lastSep = p;
        char *dot = 0;
        for ( char *p = ( lastSep ? lastSep + 1 : out ); *p; ++p )
            if ( *p == '.' )
                dot = p;
        if ( dot )
            *dot = '\0';

        const int len = (int)strlen( out );
        if ( len + 6 >= outSize )
            return false;
        strcat( out, ".kiwi" );
        return true;
    }

    const char *TypeName( kconType_t t )
    {
        switch ( t )
        {
        case KCON_LINE:     return "line";
        case KCON_POLYLINE: return "polyline";
        case KCON_RECT:     return "rect";
        case KCON_CIRCLE:   return "circle";
        case KCON_ARC:      return "arc";
        default:            return "line";
        }
    }

    bool TypeFromName( const char *s, kconType_t *out )
    {
        if ( !strcmp( s, "line" )     ) { *out = KCON_LINE;     return true; }
        if ( !strcmp( s, "polyline" ) ) { *out = KCON_POLYLINE; return true; }
        if ( !strcmp( s, "rect" )     ) { *out = KCON_RECT;     return true; }
        if ( !strcmp( s, "circle" )   ) { *out = KCON_CIRCLE;   return true; }
        if ( !strcmp( s, "arc" )      ) { *out = KCON_ARC;      return true; }
        return false;
    }
}

bool KiwiCon_SaveSidecar( const char *mapPath )
{
    char path[1100];
    if ( !SidecarPath( mapPath, path, sizeof( path ) ) )
        return false;

    // Enumerate hidden solids before deciding whether a sidecar is needed.
    const int hiddenCount = KiwiVis_SidecarBuild();

    if ( s_objects.empty() && hiddenCount == 0 && s_groups.empty()
      && KiwiRefImage_Count() == 0 ) // KIWI (REFIMG): sidecar-only planes keep the file.
    {
        // With no objects, hidden solids or groups, remove any existing sidecar.
        // (Groups are serialized below, so an empty-groups-only map must keep its file.)
        if ( !::DeleteFileA( path ) )
        {
            const DWORD error = ::GetLastError();
            if ( error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND )
            {
                Sys_Printf( "WARNING: could not remove empty construction sidecar %s (%lu)\n", path, error );
                return false;
            }
        }
        return true;
    }

    // Write to a temporary file so failure cannot destroy the previous sidecar.
    char tmp[1120];
    _snprintf( tmp, sizeof( tmp ), "%s.tmp", path );
    tmp[sizeof( tmp ) - 1] = '\0';

    FILE *f = fopen( tmp, "wb" );
    if ( !f )
    {
        Sys_Printf( "WARNING: could not write construction sidecar %s\n", tmp );
        return false;
    }

    fprintf( f, "KIWI2\n" );
    fprintf( f, "# KIWI construction geometry (RADIANT_UX_DESIGN section 7).\n" );
    fprintf( f, "# Editor-only: nothing here is map data and nothing here is compiled.\n" );

    // Hidden-brush ordinals index the adjacent map's save-order walk.
    // `hiddenbrushtotal` guards against stale ordinals after external map edits;
    // invalid ordinals are dropped at apply time.  Optional top-level records keep
    // KIWI2 backward compatible, and absent records mean nothing is hidden.
    if ( hiddenCount > 0 )
    {
        fprintf( f, "# Hidden solids (editor-only).  Ordinals index the .map save order;\n" );
        fprintf( f, "# they are only valid for the .map written beside this file.\n" );
        fprintf( f, "hiddenbrushtotal %i\n", KiwiVis_SidecarTotal() );
        for ( int i = 0; i < hiddenCount; ++i )
        {
            const int ord = KiwiVis_SidecarOrdinal( i );
            if ( ord >= 0 )
                fprintf( f, "hiddenbrush %i\n", ord );
        }
    }

    // Declare all groups before objects, including empty groups.  Quoted names may
    // contain spaces but not quotes.
    for ( size_t g = 0; g < s_groups.size(); ++g )
        fprintf( f, "congroup %i \"%s\"\n", s_groups[g].id, s_groups[g].name );
    // KIWI (REFIMG): independent editor-only blocks precede construction objects.
    KiwiRefImage_WriteSidecar( f );
    for ( size_t i = 0; i < s_objects.size(); ++i )
    {
        const kconObject_t &o = s_objects[i];
        fprintf( f, "object %s\n", TypeName( o.type ) );
        // Parametric planes are authoritative; point-object planes are derived caches.
        if ( KiwiCon_IsParametric( o ) )
        {
            fprintf( f, "plane %g %g %g  %g %g %g  %g %g %g  %g %g %g\n",
                     (double)o.plane.origin[0], (double)o.plane.origin[1], (double)o.plane.origin[2],
                     (double)o.plane.normal[0], (double)o.plane.normal[1], (double)o.plane.normal[2],
                     (double)o.plane.u[0],      (double)o.plane.u[1],      (double)o.plane.u[2],
                     (double)o.plane.v[0],      (double)o.plane.v[1],      (double)o.plane.v[2] );
        }
        fprintf( f, "closed %i\n", o.closed ? 1 : 0 );
        // Optional fields are omitted at their defaults for KIWI2 compatibility.
        if ( o.hidden )
            fprintf( f, "hidden 1\n" );
        if ( o.group >= 0 )
            fprintf( f, "group %i\n", o.group );
        if ( !o.name.empty() )
            fprintf( f, "name \"%s\"\n", o.name.c_str() );
        if ( KiwiCon_IsParametric( o ) )
        {
            // Omit AUTO tessellation so older KIWI2 output remains stable.
            if ( o.segs > 0 )
                fprintf( f, "segs %i\n", o.segs );
            fprintf( f, "arc %g %g %g %g %g\n",
                     (double)o.centre[0], (double)o.centre[1], (double)o.radius,
                     (double)o.ang0, (double)o.ang1 );
        }
        else
        {
            for ( size_t k = 0; k + 2 < o.pts.size(); k += 3 )
                fprintf( f, "wpt %g %g %g\n",
                         (double)o.pts[k], (double)o.pts[k + 1], (double)o.pts[k + 2] );
        }
        fprintf( f, "end\n" );
    }

    // Check both accumulated writes and buffered close before promoting the temp.
    // Sequence these calls: ferror must not read a FILE that fclose already freed.
    const bool writeFailed = ferror( f ) != 0;
    const bool closeFailed = fclose( f ) != 0;
    if ( writeFailed || closeFailed )
    {
        Sys_Printf( "WARNING: could not write construction sidecar %s (disk full?) — "
                    "the existing sidecar was left untouched.\n", tmp );
        ::DeleteFileA( tmp );
        return false;
    }

    if ( !::MoveFileExA( tmp, path, MOVEFILE_REPLACE_EXISTING ) )
    {
        Sys_Printf( "WARNING: could not replace construction sidecar %s\n", path );
        ::DeleteFileA( tmp );
        return false;
    }
    if ( hiddenCount > 0 )
        Sys_Printf( "Saved %i construction object(s) and %i hidden brush(es) to %s\n",
                    (int)s_objects.size(), hiddenCount, path );
    else
        Sys_Printf( "Saved %i construction object(s) to %s\n", (int)s_objects.size(), path );
    return true;
}

bool KiwiCon_LoadSidecar( const char *mapPath )
{
    KiwiRefImage_ResetForNewMap(); // KIWI (REFIMG): map replacement owns this store too.
    s_objects.clear();
    s_groups.clear();
    s_nextGroupId = 1;
    s_undo.clear();
    s_redo.clear();
    // A map load replaces both construction snapshots and unified-journal tickets.
    KiwiUndo_Reset();
    // Clear pending hidden ordinals before every early return to prevent cross-map state.
    KiwiVis_SidecarLoadBegin();
    Touch();

    char path[1100];
    if ( !SidecarPath( mapPath, path, sizeof( path ) ) )
        return false;

    FILE *f = fopen( path, "rb" );
    if ( !f )
        return false;                    // no sidecar is the normal case, not an error

    char line[512];
    // KIWI1 stores plane-space `pt u v`; KIWI2 stores WORLD-space `wpt x y z`.
    // Both load so existing editor scaffolding remains usable.
    if ( !fgets( line, sizeof( line ), f )
      || ( strncmp( line, "KIWI1", 5 ) != 0 && strncmp( line, "KIWI2", 5 ) != 0 ) )
    {
        fclose( f );
        Sys_Printf( "WARNING: %s is not a KIWI construction sidecar — ignored.\n", path );
        return false;
    }

    std::vector<kconObject_t> loaded;
    kconObject_t cur;
    std::vector<float> legacyUV;      // KIWI1 `pt` points, converted at `end`
    bool inObject = false;
    bool bad      = false;

    while ( fgets( line, sizeof( line ), f ) )
    {
        char kw[32] = { 0 };
        if ( sscanf( line, "%31s", kw ) != 1 )
            continue;
        if ( kw[0] == '#' )
            continue;

        // KIWI (REFIMG): its block parser consumes only refimage records/extensions.
        if ( KiwiRefImage_ParseSidecarLine( line ) )
            continue;

        if ( !strcmp( kw, "object" ) )
        {
            char tn[32] = { 0 };
            cur = kconObject_t();
            legacyUV.clear();
            inObject = true;
            if ( sscanf( line, "%*s %31s", tn ) != 1 || !TypeFromName( tn, &cur.type ) )
            {
                // Unknown TYPE: keep parsing so a newer file's extra object kinds
                // do not cost the user the objects this build does understand.
                inObject = false;
                bad      = true;
            }
            continue;
        }
        // Parse top-level group declarations before the object-content gate.
        if ( !strcmp( kw, "congroup" ) )
        {
            int gid = -1;
            if ( sscanf( line, "%*s %i", &gid ) == 1 && gid >= 0 )
            {
                char nm[KCON_GROUPNAME_MAX] = { 0 };
                KiwiCon_QuotedField( line, nm, KCON_GROUPNAME_MAX );
                if ( GroupSlot( gid ) < 0 )
                {
                    kconGroup_t g;
                    memset( &g, 0, sizeof( g ) );
                    g.id = gid;
                    if ( nm[0] )
                        _snprintf( g.name, sizeof( g.name ), "%s", nm );
                    else
                        _snprintf( g.name, sizeof( g.name ), "Group %i", gid );
                    g.name[sizeof( g.name ) - 1] = '\0';
                    s_groups.push_back( g );
                    // Mint new ids above every id declared by the file.
                    if ( gid >= s_nextGroupId )
                        s_nextGroupId = gid + 1;
                }
            }
            else bad = true;
            continue;
        }
        // Hidden-solid records are top-level.  Malformed visibility data is ignored
        // without marking construction geometry unreadable.
        if ( !strcmp( kw, "hiddenbrushtotal" ) )
        {
            int n = -1;
            if ( sscanf( line, "%*s %i", &n ) == 1 && n >= 0 )
                KiwiVis_SidecarLoadTotal( n );
            continue;
        }
        if ( !strcmp( kw, "hiddenbrush" ) )
        {
            int n = -1;
            if ( sscanf( line, "%*s %i", &n ) == 1 )
                KiwiVis_SidecarLoadNote( n );      // range-checked at apply time
            continue;
        }
        if ( !inObject )
            continue;

        if ( !strcmp( kw, "plane" ) )
        {
            float o[3], n[3], u[3], v[3];
            if ( sscanf( line, "%*s %f %f %f %f %f %f %f %f %f %f %f %f",
                         &o[0], &o[1], &o[2], &n[0], &n[1], &n[2],
                         &u[0], &u[1], &u[2], &v[0], &v[1], &v[2] ) == 12 )
            {
                // Rebuild the serialized basis so all solvers receive an orthonormal frame.
                if ( !KiwiCon_MakePlane( o, n, u, &cur.plane ) )
                    bad = true;
            }
            else bad = true;
        }
        else if ( !strcmp( kw, "closed" ) )
        {
            int c = 0;
            if ( sscanf( line, "%*s %i", &c ) == 1 ) cur.closed = ( c != 0 );
            else                                     bad = true;
        }
        else if ( !strcmp( kw, "hidden" ) )              // absent = visible
        {
            int h = 0;
            if ( sscanf( line, "%*s %i", &h ) == 1 ) cur.hidden = ( h != 0 );
            else                                     bad = true;
        }
        else if ( !strcmp( kw, "group" ) )               // absent = ungrouped
        {
            int g = -1;
            if ( sscanf( line, "%*s %i", &g ) == 1 )
            {
                // Undeclared group ids load as ungrouped.
                cur.group = ( GroupSlot( g ) >= 0 ) ? g : -1;
            }
            else bad = true;
        }
        else if ( !strcmp( kw, "name" ) )                // absent = unnamed
        {
            // A malformed optional label leaves the object unnamed.
            char nm[KCON_NAME_MAX] = { 0 };
            if ( KiwiCon_QuotedField( line, nm, KCON_NAME_MAX ) )
                cur.name = nm;
        }
        else if ( !strcmp( kw, "wpt" ) )                 // KIWI2 — WORLD, verbatim
        {
            float a, b, c;
            if ( sscanf( line, "%*s %f %f %f", &a, &b, &c ) == 3 )
            {
                if ( (int)( cur.pts.size() / 3 ) < KCON_MAX_POINTS )
                {
                    cur.pts.push_back( a );
                    cur.pts.push_back( b );
                    cur.pts.push_back( c );
                }
                else bad = true;
            }
            else bad = true;
        }
        else if ( !strcmp( kw, "pt" ) )                  // KIWI1 — plane space
        {
            float a, b;
            if ( sscanf( line, "%*s %f %f", &a, &b ) == 2 )
            {
                if ( (int)( legacyUV.size() / 2 ) < KCON_MAX_POINTS )
                {
                    legacyUV.push_back( a );
                    legacyUV.push_back( b );
                }
                else bad = true;
            }
            else bad = true;
        }
        else if ( !strcmp( kw, "segs" ) )
        {
            // Clamp hand-edited segment counts before they reach tessellation.
            int n = 0;
            if ( sscanf( line, "%*s %i", &n ) == 1 && n > 0 )
            {
                cur.segs = ClampSides( n );
            }
        }
        else if ( !strcmp( kw, "arc" ) )
        {
            float cu, cv, r, a0, a1;
            if ( sscanf( line, "%*s %f %f %f %f %f", &cu, &cv, &r, &a0, &a1 ) == 5 )
            {
                cur.centre[0] = cu; cur.centre[1] = cv;
                cur.radius = r; cur.ang0 = a0; cur.ang1 = a1;
            }
            else bad = true;
        }
        else if ( !strcmp( kw, "end" ) )
        {
            inObject = false;
            // Convert KIWI1 plane-space points through the object's serialized plane;
            // a missing plane intentionally falls back to the default XY basis.
            if ( !legacyUV.empty() )
            {
                for ( size_t k = 0; k + 1 < legacyUV.size(); k += 2 )
                {
                    float w[3];
                    KiwiCon_PlaneToWorld( cur.plane, &legacyUV[k], w );
                    cur.pts.push_back( w[0] );
                    cur.pts.push_back( w[1] );
                    cur.pts.push_back( w[2] );
                }
                legacyUV.clear();
            }
            // Loaded data bypasses KiwiCon_Add, so normalize repeated closing vertices here.
            NormalizePoints( cur );
            if ( KiwiCon_IsParametric( cur ) ? ( cur.radius > 1.0e-3f )
                                             : ( (int)( cur.pts.size() / 3 ) >= 2 ) )
                loaded.push_back( cur );
            else
                bad = true;
        }
        // Any other keyword is a newer file's addition — skipped silently.
    }
    if ( inObject )
        bad = true;               // EOF inside an object: the file is truncated, say so
    fclose( f );

    s_objects.swap( loaded );
    s_undo.clear();
    s_redo.clear();
    RefitAll();                   // refresh every point object's cached plane
    Touch();

    if ( bad )
        Sys_Printf( "WARNING: %s had unreadable entries — %i object(s) loaded.\n",
                    path, (int)s_objects.size() );
    else if ( !s_objects.empty() )
        Sys_Printf( "Loaded %i construction object(s) from %s\n", (int)s_objects.size(), path );
    return true;
}

// Commands.
void KiwiCon_RegisterCommands()
{
    // Register unbound; keymap profiles own the shortcuts.
    Radiant_RegisterCommand( "KiwiConstructLine",      0, 0, KIWI_CMD_DRAW_LINE );
    Radiant_RegisterCommand( "KiwiConstructPolyline",  0, 0, KIWI_CMD_DRAW_POLYLINE );
    Radiant_RegisterCommand( "KiwiConstructRect",      0, 0, KIWI_CMD_DRAW_RECT );
    Radiant_RegisterCommand( "KiwiConstructCircle",    0, 0, KIWI_CMD_DRAW_CIRCLE );
    Radiant_RegisterCommand( "KiwiConstructArc",       0, 0, KIWI_CMD_DRAW_ARC );
    Radiant_RegisterCommand( "KiwiConstructRectCenter", 0, 0, KIWI_CMD_DRAW_RECT_CENTER );
    Radiant_RegisterCommand( "KiwiConstructCircle2Pt",  0, 0, KIWI_CMD_DRAW_CIRCLE_2PT );
    Radiant_RegisterCommand( "KiwiConstructPolygon",    0, 0, KIWI_CMD_DRAW_POLYGON );
    Radiant_RegisterCommand( "KiwiConstructSpline",     0, 0, KIWI_CMD_DRAW_SPLINE );
    Radiant_RegisterCommand( "KiwiConstructPlaneXY",   0, 0, KIWI_CMD_CPLANE_XY );
    Radiant_RegisterCommand( "KiwiConstructPlaneXZ",   0, 0, KIWI_CMD_CPLANE_XZ );
    Radiant_RegisterCommand( "KiwiConstructPlaneYZ",   0, 0, KIWI_CMD_CPLANE_YZ );
    Radiant_RegisterCommand( "KiwiConstructPlaneFace", 0, 0, KIWI_CMD_CPLANE_FACE );
    Radiant_RegisterCommand( "KiwiConstructPlaneView", 0, 0, KIWI_CMD_CPLANE_VIEW );
    Radiant_RegisterCommand( "KiwiConstructClear",     0, 0, KIWI_CMD_CONSTRUCT_CLEAR );
    Radiant_RegisterCommand( "KiwiConstructUndo",      0, 0, KIWI_CMD_CONSTRUCT_UNDO );
}

KiwiEditorCommand *KiwiCon_CommandForId( int commandId )
{
    switch ( commandId )
    {
    // Line and Polyline are aliases for the same chained-curve tool.
    case KIWI_CMD_DRAW_LINE:     return &s_curveTool;
    case KIWI_CMD_DRAW_POLYLINE: return &s_curveTool;
    case KIWI_CMD_DRAW_RECT:     return &s_rectTool;
    case KIWI_CMD_DRAW_CIRCLE:   return &s_circleTool;
    case KIWI_CMD_DRAW_ARC:      return &s_arcTool;
    case KIWI_CMD_DRAW_RECT_CENTER: return &s_rectCenterTool;
    case KIWI_CMD_DRAW_CIRCLE_2PT:  return &s_circle2PtTool;
    case KIWI_CMD_DRAW_POLYGON:     return &s_polygonTool;
    case KIWI_CMD_DRAW_SPLINE:      return &s_splineTool;
    default:                     return 0;
    }
}

bool KiwiCon_DispatchInstant( unsigned int commandId )
{
    switch ( commandId )
    {
    // Named plane commands create an EXPLICIT persistent plane for planar tools.
    // Free line/polyline/spline tools ignore this plane and keep WORLD-space points.
    case KIWI_CMD_CPLANE_XY:
        KiwiCon_SetPlaneAxis( 2 );  KiwiCon_MarkPlaneExplicit( "XY (chosen)" );  return true;
    case KIWI_CMD_CPLANE_XZ:
        KiwiCon_SetPlaneAxis( 1 );  KiwiCon_MarkPlaneExplicit( "XZ (chosen)" );  return true;
    case KIWI_CMD_CPLANE_YZ:
        KiwiCon_SetPlaneAxis( 0 );  KiwiCon_MarkPlaneExplicit( "YZ (chosen)" );  return true;
    case KIWI_CMD_CPLANE_VIEW:
        KiwiCon_SetPlaneFromView(); KiwiCon_MarkPlaneExplicit( "the view plane" ); return true;
    case KIWI_CMD_CPLANE_FACE:
        if ( KiwiCon_SetPlaneFromCursorFace() )
        {
            KiwiCon_MarkPlaneExplicit( "a face (chosen)" );
            Sys_Printf( "Construction plane: from face under cursor.\n" );
        }
        else
        {
            Sys_Printf( "Construction plane: no face under the cursor.\n" );
        }
        return true;
    case KIWI_CMD_CONSTRUCT_CLEAR:
        if ( KiwiCon_HasObjects() )
        {
            KiwiCon_UndoPush();
            KiwiCon_ClearAll();
            Sys_Printf( "Construction: cleared.\n" );
        }
        return true;
    case KIWI_CMD_CONSTRUCT_UNDO:
        // Route through the unified journal; direct snapshot pops desynchronize tickets.
        if ( !KiwiUndo_Undo() )
            Sys_Printf( "Nothing left to undo.\n" );
        return true;
    default:
        return false;
    }
}

// Panel buttons provide a profile-independent route to every construction command.
void KiwiCon_MenuItems()
{
    ImGui::SeparatorText( "Construct" );

    struct row_t { const char *label; int id; };
    static const row_t KTOOLS[9] =
    {
        // Keep both discoverable names for the shared chained-curve tool.
        { "Line",       KIWI_CMD_DRAW_LINE        },
        { "Poly(=Line)",KIWI_CMD_DRAW_POLYLINE    },
        { "Spline",     KIWI_CMD_DRAW_SPLINE      },
        { "Rect",       KIWI_CMD_DRAW_RECT        },
        { "Rect(ctr)",  KIWI_CMD_DRAW_RECT_CENTER },
        { "Circle",     KIWI_CMD_DRAW_CIRCLE      },
        { "Circle(2p)", KIWI_CMD_DRAW_CIRCLE_2PT  },
        { "Arc",        KIWI_CMD_DRAW_ARC         },
        { "Polygon",    KIWI_CMD_DRAW_POLYGON     },
    };
    for ( int i = 0; i < 9; ++i )
    {
        if ( i % 3 )                              // 3 per row — nine buttons on one
            ImGui::SameLine();                    // line would run off the panel
        if ( ImGui::Button( KTOOLS[i].label ) )
            Radiant_ExecCommand( (unsigned int)KTOOLS[i].id );
    }

    ImGui::BeginDisabled( !KiwiTrim_CanTrim() );
    if ( ImGui::Button( "Trim Lines (T)" ) )
        Radiant_ExecCommand( KIWI_CMD_TRIM );
    ImGui::EndDisabled();
    // AllowWhenDisabled is required for the unavailable-action tooltip.
    if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) && !KiwiTrim_CanTrim() )
        ImGui::SetTooltip( "Draw a line or a polyline first.\n"
                           "Trim removes the piece between two crossings." );
    ImGui::SameLine();

    const bool canExtrude = !KiwiRegion_All().empty();
    ImGui::BeginDisabled( !canExtrude );
    if ( ImGui::Button( "Extrude Region" ) )
        Radiant_ExecCommand( KIWI_CMD_EXTRUDE_REGION );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) && !canExtrude )
        ImGui::SetTooltip( "Draw a closed loop first (a rect, a circle,\n"
                           "or a polyline clicked back onto its first point)." );

    const bool canOffset = KiwiOffset_CanOffset();
    ImGui::BeginDisabled( !canOffset );
    if ( ImGui::Button( "Offset (O)" ) )
        Radiant_ExecCommand( KIWI_CMD_OFFSET_CURVE );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) && !canOffset )
        ImGui::SetTooltip( "Select a construction line, polyline, rect,\n"
                           "circle or arc first (click it in any mode).\n"
                           "Offset makes a parallel COPY; the original stays." );
    ImGui::SameLine();

    const bool canFillet = KiwiFillet_CanFillet();
    ImGui::BeginDisabled( !canFillet );
    if ( ImGui::Button( "Fillet (B)" ) )
    {
        // B's generic dispatch prefers solid fillet when brush edges are selected;
        // this construction-labelled button must start curve fillet directly.
        KiwiCmd_Start( KIWI_CMD_FILLET_CURVE );
    }
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) && !canFillet )
        ImGui::SetTooltip( "Select a planar polyline or rect with corners.\n"
                           "Every corner rounds; select individual anchors\n"
                           "in Point mode (1) to round only those." );

    static const row_t KPLANES[5] =
    {
        { "XY",        KIWI_CMD_CPLANE_XY   },
        { "XZ",        KIWI_CMD_CPLANE_XZ   },
        { "YZ",        KIWI_CMD_CPLANE_YZ   },
        { "From face", KIWI_CMD_CPLANE_FACE },
        { "From view", KIWI_CMD_CPLANE_VIEW },
    };
    ImGui::TextDisabled( "Construction plane" );
    for ( int i = 0; i < 5; ++i )
    {
        if ( i )
            ImGui::SameLine();
        ImGui::PushID( 900 + i );
        if ( ImGui::Button( KPLANES[i].label ) )
            Radiant_ExecCommand( (unsigned int)KPLANES[i].id );
        ImGui::PopID();
    }

    ImGui::TextDisabled( "Selection (%i item(s))", KiwiConSel_Count() );
    ImGui::BeginDisabled( !KiwiConSel_CanJoin() );
    if ( ImGui::Button( "Join Lines" ) )
        Radiant_ExecCommand( KIWI_CMD_CONSTRUCT_JOIN );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) && !KiwiConSel_CanJoin() )
        ImGui::SetTooltip( "Select two or more open construction lines whose\n"
                           "ends meet (click them, or drag a marquee)." );
    ImGui::SameLine();
    ImGui::BeginDisabled( !KiwiConSel_CanDelete() );
    if ( ImGui::Button( "Delete Selected" ) )
        Radiant_ExecCommand( KIWI_CMD_CONSTRUCT_DELETE );
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled( KiwiConSel_Empty() );
    if ( ImGui::Button( "Deselect" ) )
        KiwiConSel_Clear();
    ImGui::EndDisabled();

    // Availability follows the unified journal because that is what the command pops.
    ImGui::BeginDisabled( KiwiUndo_UndoDepth() <= 0 );
    if ( ImGui::Button( "Undo (unified)" ) )
        Radiant_ExecCommand( KIWI_CMD_CONSTRUCT_UNDO );
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled( !KiwiCon_HasObjects() );
    if ( ImGui::Button( "Clear all" ) )
        Radiant_ExecCommand( KIWI_CMD_CONSTRUCT_CLEAR );
    ImGui::EndDisabled();

    bool show = KiwiCon_ShowConstruction();
    if ( ImGui::Checkbox( "Show construction geometry", &show ) )
        KiwiCon_SetShowConstruction( show );

    ImGui::TextDisabled( "%i object(s), %i region(s), %i construction snapshot(s)",
                         KiwiCon_Count(), (int)KiwiRegion_All().size(), KiwiCon_UndoDepth() );
    ImGui::TextDisabled( "timeline: %i undo / %i redo%s%s",
                         KiwiUndo_UndoDepth(), KiwiUndo_RedoDepth(),
                         KiwiUndo_UndoLabel() ? "   next: " : "",
                         KiwiUndo_UndoLabel() ? KiwiUndo_UndoLabel() : "" );
}

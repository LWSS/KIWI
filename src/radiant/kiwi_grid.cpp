#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// World-axis and ground-lattice implementation.
// Display LOD is independent of snap spacing; rendering is bounded up front.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "kiwi_grid.h"
#include "kiwi_lines.h"
#include "kiwi_units.h"
#include "kiwi_ux.h"
#include "kiwi_camera.h"    // projection mode and world-per-pixel
#include "radiant_registry.h"   // grid-snap preference
#include "kiwi_vec.h"     // Dot3

#include <math.h>

// Ported entry points, verified against their definitions.
extern camera_s *Ed_Camera();          // camwnd.cpp:161
extern void      CamWnd_BuildMatrix(); // camwnd.cpp:210 (0x403470)
extern int       Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:118
extern int       g_nUpdateBits;                        // 0x25D5A74 (mainfrm.cpp)

namespace
{
    // Axes use the same finite reach as the grid ruler.
    const float KGRID_AXIS_LEN = KGRID_MAX_EXTENT;

    // [tier][major] RGB. Near draws both runs; the far ring draws majors only.
    enum { KGRID_TIER_NEAR = 0, KGRID_TIER_FAR = 1, KGRID_TIER_COUNT = 2 };
    const float KGRID_BANDS[KGRID_TIER_COUNT][2][3] =
    {
        { { 0.30f, 0.30f, 0.32f }, { 0.48f, 0.48f, 0.52f } },   // 0 near lattice
        { { 0.17f, 0.17f, 0.19f }, { 0.28f, 0.28f, 0.31f } },   // 1 far ring
    };

    // One LOD latch; changing the user base spacing resets it.
    int   s_lod        = 0;
    float s_lodBase    = 0.0f;

    // The ortho far ring has its own Schmitt latch.
    bool  s_farOn      = false;


    // Cull only when both ends and the midpoint are behind the eye; crossing
    // segments remain for renderer clipping.
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

    // Split each desaturated axis at the origin so halves cull independently.
    // Keep axes full brightness to orient views when the ground lattice fades;
    // desaturation avoids competing with selections and transform accents.
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

    // Per-frame lattice window; reach, centre, and ground are world-space XY.
    struct window_t
    {
        float spacing;      // world units per cell (base * 2^lod)
        // The projected ortho footprint is anisotropic, so reach stays per-axis.
        float reach[2];     // half-extent in X and in Y
        float centre[2];    // window centre in XY
        float ground[2];    // the camera's own ground projection
        bool  ok;
    };

    // Ortho uses zoom-derived world-per-pixel; pitch supplies only the uniform
    // foreshortening factor and projected footprint, never camera altitude.

    // One pixel-space Schmitt LOD. REFINE > 2*MIN prevents one step from
    // satisfying both transitions; bounded iteration prevents runaway.
    int PickLodOrtho( float base, float wpp )
    {
        if ( s_lodBase != base )
        {
            s_lodBase = base;
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

    // Exact ortho ground footprint. With no roll, vright is horizontal and
    // ground-projected screen-up is g = vup - vpn*(vup.z/vpn.z). Each world-XY
    // half-extent is halfW*abs(vright) + halfH*abs(g); hitOut is the
    // screen-centre ground hit.
    void OrthoFootprint( const camera_s *c, float halfW, float halfH,
                         float *needOut, float *hitOut )
    {
        // Preserve vpn[2]'s sign; EdgeFade owns angles below this floor.
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

    // Clamp one footprint axis to the cell budget. Pull a clamped centre toward
    // the camera ground projection in the same ratio, keeping grazing coverage
    // near the viewer and continuity at need == cap.
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

    // Per-frame failures would spam: print only each cause's 0 -> 1 transition.
    // A successful build re-arms every cause.
    enum
    {
        KGRID_BAIL_BASE = 0,    // the grid spacing preference is not positive
        KGRID_BAIL_WPP,         // the camera's world-per-pixel is not positive
        KGRID_BAIL_WPPEFF,      // the foreshortened world-per-pixel is not positive
        KGRID_BAIL_SPACING,     // the chosen LOD tier's spacing is not positive
        KGRID_BAIL_CAP,         // the cell-count cap is not positive
        KGRID_BAIL_REACH,       // a clamped axis reach is not positive
    };
    unsigned s_bailLatch = 0;   // exactly one cause is live at a time

    bool GridBail( int cause, const char *why )
    {
        const unsigned bit = 1u << cause;
        if ( !( s_bailLatch & bit ) )
            Sys_Printf( "Grid: not drawn — %s.\n", why );
        s_bailLatch = bit;
        return false;
    }

    bool BuildWindowOrtho( const camera_s *c, window_t &w, float *needOut, float *hitOut )
    {
        w.ok = false;
        const float base = KiwiUnits_GridSpacingWorld();
        if ( !( base > 0.0f ) )
            return GridBail( KGRID_BAIL_BASE, "the grid spacing is not a positive "
                                              "number (kiwi_units)" );

        // Ortho world-per-pixel ignores position; origin identifies the active camera.
        const float wpp = KiwiCam_WorldPerPixel( c->origin );
        if ( !( wpp > 0.0f ) )
            return GridBail( KGRID_BAIL_WPP, "the camera's world-per-pixel is not "
                                             "positive (zoom or viewport height)" );

        // Roll-free ortho compresses one ground line family uniformly: its pitch
        // is spacing*abs(vpn.z)/wpp. Feed wpp/abs(vpn.z) to the shared pixel LOD.
        // This is one frame-level scalar, not a per-line perspective correction.
        // Floor it at EDGE_FULL, where edge fading takes over.
        float sinPitch = fabsf( c->vpn[2] );
        if ( !( sinPitch > KGRID_ORTHO_MIN_SIN ) )      // also catches a NaN vpn
            sinPitch = KGRID_ORTHO_MIN_SIN;
        const float wppEff = wpp / sinPitch;
        if ( !( wppEff > 0.0f ) )
            return GridBail( KGRID_BAIL_WPPEFF, "the foreshortened world-per-pixel "
                                                "is not positive (camera basis)" );

        const int k = PickLodOrtho( base, wppEff );
        w.spacing = base * (float)( 1 << k );
        if ( !( w.spacing > 0.0f ) )
            return GridBail( KGRID_BAIL_SPACING, "the LOD tier's spacing is not "
                                                 "positive (spacing overflowed)" );

        const float halfW = 0.5f * wpp * (float)c->width;
        const float halfH = 0.5f * wpp * (float)c->height;

        float need[2], hit[2];
        OrthoFootprint( c, halfW, halfH, need, hit );

        float cap = w.spacing * (float)KGRID_HALF_CELLS;
        if ( cap > KGRID_MAX_EXTENT )
            cap = KGRID_MAX_EXTENT;
        if ( !( cap > 0.0f ) )
            return GridBail( KGRID_BAIL_CAP, "the cell-count cap is not positive" );

        w.ground[0] = c->origin[0];
        w.ground[1] = c->origin[1];
        for ( int a = 0; a < 2; ++a )
        {
            ClampAxis( w.ground[a], hit[a], need[a], cap, &w.centre[a], &w.reach[a] );
            if ( !( w.reach[a] > 0.0f ) )
                return GridBail( KGRID_BAIL_REACH, ( a == 0 )
                                 ? "the window's X reach is not positive (footprint)"
                                 : "the window's Y reach is not positive (footprint)" );
            if ( needOut ) needOut[a] = need[a];
            if ( hitOut  ) hitOut[a]  = hit[a];
        }

        s_bailLatch = 0;                // built: re-arm every cause, silently
        w.ok = true;
        return true;
    }

    // Perspective evaluates world-per-pixel at the orbit pivot, shares the pixel
    // LOD and emitters, and produces a pivot-centered window_t.
    bool BuildWindowPersp( const camera_s *c, window_t &w )
    {
        w.ok = false;
        const float base = KiwiUnits_GridSpacingWorld();
        if ( !( base > 0.0f ) )
            return GridBail( KGRID_BAIL_BASE, "the grid spacing is not a positive "
                                              "number (kiwi_units)" );

        // Perspective wpp is z*s; dividing by the camera-clamped pivot depth
        // recovers s without duplicating the camera's FOV calculation.
        const float *pivot = KiwiCam_LookAt();
        const float  wpp   = KiwiCam_WorldPerPixel( pivot );
        if ( !( wpp > 0.0f ) )
            return GridBail( KGRID_BAIL_WPP, "the camera's world-per-pixel is not "
                                             "positive (fov or viewport height)" );

        float zPivot = ( pivot[0] - c->origin[0] ) * c->vpn[0]
                     + ( pivot[1] - c->origin[1] ) * c->vpn[1]
                     + ( pivot[2] - c->origin[2] ) * c->vpn[2];
        if ( zPivot < 1.0f )
            zPivot = 1.0f;                     // the same floor the camera applies

        // Use the ortho floor so LOD hands off to EdgeFade at one threshold.
        float sinPitch = fabsf( c->vpn[2] );
        if ( !( sinPitch > KGRID_ORTHO_MIN_SIN ) )      // also catches a NaN vpn
            sinPitch = KGRID_ORTHO_MIN_SIN;

        const float wppEff = wpp / sinPitch;
        if ( !( wppEff > 0.0f ) )
            return GridBail( KGRID_BAIL_WPPEFF, "the foreshortened world-per-pixel "
                                                "is not positive (camera basis)" );

        const int k = PickLodOrtho( base, wppEff );
        w.spacing = base * (float)( 1 << k );
        if ( !( w.spacing > 0.0f ) )
            return GridBail( KGRID_BAIL_SPACING, "the LOD tier's spacing is not "
                                                 "positive (spacing overflowed)" );

        // Projection-independent cell cap holds the segment budget.
        float cap = w.spacing * (float)KGRID_HALF_CELLS;
        if ( cap > KGRID_MAX_EXTENT )
            cap = KGRID_MAX_EXTENT;
        if ( !( cap > 0.0f ) )
            return GridBail( KGRID_BAIL_CAP, "the cell-count cap is not positive" );

        // At view depth z, compressed cell pitch is spacing*sinPitch/(z*sPix).
        // zMax is the depth where that pitch reaches KGRID_PX_MIN.
        const float sPix = wpp / zPivot;                 // radians-ish per pixel
        if ( sPix > 0.0f )
        {
            const float zMax = w.spacing * sinPitch / ( sPix * KGRID_PX_MIN );
            if ( zMax > 0.0f && zMax < cap )
                cap = zMax;
        }

        w.ground[0] = c->origin[0];
        w.ground[1] = c->origin[1];
        for ( int a = 0; a < 2; ++a )
        {
            // Center on pivot XY; perspective has no finite footprint to pull.
            w.centre[a] = pivot[a];
            w.reach[a]  = cap;
            if ( !( w.reach[a] > 0.0f ) )
                return GridBail( KGRID_BAIL_REACH, ( a == 0 )
                                 ? "the window's X reach is not positive (footprint)"
                                 : "the window's Y reach is not positive (footprint)" );
        }

        s_bailLatch = 0;                // built: re-arm every cause, silently
        w.ok = true;
        return true;
    }

    // Ortho far ring: Schmitt-latch the exact footprint-need/reach ratio.
    bool BuildFarWindowOrtho( const window_t &w, const float *need, const float *hit,
                              window_t &f )
    {
        f.ok = false;

        // Maximum missing-ground ratio across world XY.
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
        if ( !( f.spacing > 0.0f ) )                // rejects non-positive and NaN
            return false;

        float cap = f.spacing * (float)KGRID_HALF_CELLS;
        if ( cap > KGRID_MAX_EXTENT )
            cap = KGRID_MAX_EXTENT;

        f.ground[0] = w.ground[0];
        f.ground[1] = w.ground[1];
        bool bigger = false;
        for ( int a = 0; a < 2; ++a )
        {
            // Use the original footprint hit, not the pulled-back near centre.
            // Power-of-two spacing keeps near and far world phases aligned.
            ClampAxis( f.ground[a], hit[a], need[a], cap, &f.centre[a], &f.reach[a] );
            if ( !( f.reach[a] > 0.0f ) )
                return false;
            if ( f.reach[a] > w.reach[a] )
                bigger = true;
        }
        if ( !bigger )                              // no extension beyond near window
            return false;

        f.ok = true;
        return true;
    }

    // First / last lattice index on `axis` inside the window.  The window is
    // 2R wide, so (i1 - i0 + 1) <= 2*KGRID_HALF_CELLS + 1 — the budget bound.
    inline void IndexRange( const window_t &w, int axis, int *i0, int *i1 )
    {
        *i0 = (int)ceilf ( ( w.centre[axis] - w.reach[axis] ) / w.spacing );
        *i1 = (int)floorf( ( w.centre[axis] + w.reach[axis] ) / w.spacing );
    }

    // Emit one major/minor colour run into the open batch. Index magnitude is
    // unbounded, but a 2R window contains at most 2*KGRID_HALF_CELLS+1 lines
    // per axis. Return false when the shared segment budget is spent.
    bool EmitRun( const camera_s *c, const window_t &w, int major, bool axesOn )
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

                float a[3], b[3];
                // KIWI-UX: the grid sits a hair BELOW the Z=0 plane.  Terrain snapped to
                // exactly Z=0 (Set height's default target) is coplanar with it and the
                // depth-tested lines z-fight through the surface as dashes; -0.125 puts
                // every Z=0 surface in front when seen from above.
                a[2] = b[2] = -0.125f;
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

    // Frame-wide ground brightness at this pitch; zero skips lattice, not axes.
    float EdgeFade( const camera_s *c )
    {
        const float s = fabsf( c->vpn[2] );          // |vpn · Z|
        if ( !( s > KGRID_EDGE_OFF ) )               // also catches NaN
            return 0.0f;
        if ( s >= KGRID_EDGE_FULL )
            return 1.0f;
        return ( s - KGRID_EDGE_OFF ) / ( KGRID_EDGE_FULL - KGRID_EDGE_OFF );
    }

    // Emit far majors before near lines so brighter coincident near lines win.
    // Minors are omitted because the ring appears where their density aliases.
    // FarMajorCount implements the minimum-readable-grid gate. In ortho no
    // segment is behind the eye; over-counting can only preserve a marginal ring.
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
        KiwiLines_Color( KGRID_BANDS[KGRID_TIER_FAR][1][0] * mul,
                         KGRID_BANDS[KGRID_TIER_FAR][1][1] * mul,
                         KGRID_BANDS[KGRID_TIER_FAR][1][2] * mul );
        EmitRun( c, f, /*major*/ 1, axesOn );
    }

    // A flat near tier avoids camera-centered distance bands in ortho.
    void DrawGround( const camera_s *c, const window_t &w, bool axesOn, float fade )
    {
        // Minors fade first because they are ten times denser than majors.
        const bool minorsOn = ( fade >= KGRID_EDGE_MINOR );

        for ( int major = 1; major >= 0; --major )
        {
            if ( !major && !minorsOn )
                continue;
            KiwiLines_Color( KGRID_BANDS[KGRID_TIER_NEAR][major][0] * fade,
                             KGRID_BANDS[KGRID_TIER_NEAR][major][1] * fade,
                             KGRID_BANDS[KGRID_TIER_NEAR][major][2] * fade );
            if ( !EmitRun( c, w, major, axesOn ) )
                return;
        }
    }
}

// Cam_Draw hook runs before the world so later geometry covers the grid.
void KiwiGrid_Draw()
{

    const bool axesOn = KiwiUX_ShowAxes();
    // Ortho follows the grid overlay switch. Perspective additionally requires
    // its session opt-in; axes and snapping use independent switches.
    const bool ortho  = KiwiCam_Ortho();
    const bool gridOn = KiwiUX_ShowGrid() && ( ortho || KiwiGrid_PerspGrid() );
    if ( !axesOn && !gridOn )
        return;

    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;
    // Build the basis for culling, edge fade, footprint, and foreshortening.
    CamWnd_BuildMatrix();

    // Read fade before building a window; axes deliberately remain outside it.
    const float fade = EdgeFade( c );

    // Both projections produce window_t for shared emitters. Only ortho computes
    // exact footprint need/hit; perspective centers its window on the pivot.
    window_t w = {};                    // zero-init: nothing reads it unless haveWindow
    float    need[2] = { 0.0f, 0.0f };  // the ortho footprint before the cell cap
    float    hit [2] = { 0.0f, 0.0f };  // the screen centre's own ground point
    const bool haveWindow = gridOn && fade > 0.0f
                          && ( ortho ? BuildWindowOrtho( c, w, need, hit )
                                     : BuildWindowPersp( c, w ) );

    // The ortho-only far ring derives from the exact unclamped footprint and
    // cannot change the near lattice's spacing, reach, or centre.
    window_t wf = {};
    bool haveFar = haveWindow && ortho && BuildFarWindowOrtho( w, need, hit, wf );

    // Suppress the far ring when its angle or remaining major count cannot read
    // as a grid. These gates do not affect the near lattice or axes.
    if ( haveFar )
    {
        if ( fabsf( c->vpn[2] ) < KGRID_FAR_MIN_SIN )
            haveFar = false;                       // too near the horizon to read
        else if ( FarMajorCount( wf, axesOn ) < KGRID_FAR_MIN_LINES )
            haveFar = false;                       // too few lines to be a grid
    }

    // One width-1 batch: far ring, near grid, then axes on top.
    KiwiLines_Begin( KGRID_MAX_SEGMENTS, 1 );
    if ( haveFar )
        DrawFarRing( c, wf, axesOn, fade );
    if ( haveWindow )
        DrawGround( c, w, axesOn, fade );
    if ( axesOn )
        DrawAxes( c );
    KiwiLines_Flush();
}

// Grid-snap preference and perspective-grid session state.
namespace
{
    int s_snapOn = -1;                      // -1 = not read from the profile yet

    // Perspective lattice is opt-in session state and is not persisted.
    bool s_perspGrid = false;
}

bool KiwiGrid_PerspGrid()
{
    return s_perspGrid;
}

void KiwiGrid_SetPerspGrid( bool on )
{
    if ( s_perspGrid == on )
        return;
    s_perspGrid = on;
    Sys_Printf( "Perspective grid: %s.\n",
                on ? "ON (pivot-depth lattice; majors only near the horizon)"
                   : "off" );
    g_nUpdateBits |= 1;
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

// SnapManager consumer: quantize raw world units at the user base spacing.
bool KiwiGrid_Snap( const float in[3], float out[3] )
{
    if ( !in || !out )
        return false;
    // Disabled snapping uses the established copy-through refusal contract.
    if ( !KiwiGrid_SnapEnabled() )
    {
        out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
        return false;
    }
    // Snap to user base spacing, never display LOD; zoom must not move geometry.
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

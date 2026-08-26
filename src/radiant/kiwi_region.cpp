#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Regions come from closed objects, welded open-object cycles, and bounded cells
// in a planar arrangement.  Every route shares AcceptLoop; arrangement duplicates
// are matched by world position and area.
// Chain points pass through world space into the fitted group plane, so differing
// object bases and non-axial planes use the same loop and picking conventions.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // camera_s
#include <imgui/imgui.h>            // camera-overlay fill
#include <gfx_d3d/r_gfx.h>          // GfxColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT

#include "kiwi_region.h"
#include "kiwi_arrange.h"           // planar-arrangement pass
#include "kiwi_camera.h"            // KiwiCam_Ortho winding-side rule
#include "kiwi_construct.h"
#include "kiwi_lines.h"             // shared fill-rendering constraints
#include "kiwi_pick.h"
#include "kiwi_validity.h"          // KVALID_PLANE_DOT
#include "kiwi_units.h"             // grid-scaled tolerances
#include "kiwi_vec.h"     // Dot3

#include <math.h>
#include <vector>           // scratch vectors
#include <string.h>

// Ported entry points.
extern int   Sys_Printf( const char *fmt, ... );                         // win_qe3.cpp
extern int   g_nUpdateBits;                                              // 0x25D5A74 (mainfrm.cpp)
// Returns the persistent g_camwndState.camera and never null.
extern camera_s *Ed_Camera();                                            // camwnd.cpp:161
extern char  Byte4PackPixelColor( float *from, GfxColor *out );          // 0x402ac0
// R_AddCmdSetMaterialColor comes from r_rendercmds.h (declared __cdecl there).
extern void  __cdecl R_AddRenderCmdDrawTris(
                 Material *material, MaterialTechniqueType techType, short indexCount,
                 const uint16_t *indices, short vertexCount,
                 const float ( *xyzw )[4], const float ( *normal )[3], float *color,
                 const float ( *st )[2] );                               // 0x4fd1c0
// Names command-buffer drops before the void renderer call can hide them.
extern int   __cdecl R_Ed_CmdBufferHeadroom();                           // r_rendercmds.cpp
// Confirmed-visible translucent world-space polygon emitter; push is along n.
extern void  Cam_DrawWindingTinted( const float ( *pts )[3], int nv, const float *n,
                                    Material *mtl, uint bgra, float push,
                                    MaterialTechniqueType tech );        // camwnd.cpp:1221

namespace
{
    std::vector<kregion_t> s_regions;
    unsigned               s_builtFor = 0;        // the KiwiCon_Generation() it was built for
    bool                   s_dirty    = true;

    // Keep the resting fill weaker than the 0.25-alpha 3D marquee.
    const float KREG_FILL[4] = { 0.45f, 0.70f, 1.00f, 0.22f };
    const float KREG_HILITE[4] = { 0.62f, 0.84f, 1.00f, 0.38f };
    // New regions fade from a conspicuous but still translucent near-white.
    const float KREG_FLASH[4] = { 0.90f, 0.97f, 1.00f, 0.62f };
    // Selection stays weaker than the formation flash so both states remain visible.
    const float KREG_SELECTED[4] = { 0.82f, 0.92f, 1.00f, 0.46f };


    inline float Cross2( const float *o, const float *a, const float *b )
    {
        return ( a[0] - o[0] ) * ( b[1] - o[1] ) - ( a[1] - o[1] ) * ( b[0] - o[0] );
    }

    inline const float *Pt( const std::vector<float> &pts, int i )
    {
        return &pts[(size_t)i * 2];
    }

    inline int PtCount( const std::vector<float> &pts )
    {
        return (int)( pts.size() / 2 );
    }

    // Strict sign-change segment test; callers separately skip adjacent loop edges.
    bool SegCross( const float *a, const float *b, const float *c, const float *d )
    {
        const float d1 = Cross2( a, b, c );
        const float d2 = Cross2( a, b, d );
        const float d3 = Cross2( c, d, a );
        const float d4 = Cross2( c, d, b );
        return ( ( d1 > 0.0f ) != ( d2 > 0.0f ) ) && ( ( d3 > 0.0f ) != ( d4 > 0.0f ) );
    }

    bool PointInTriangle( const float *p, const float *a, const float *b, const float *c )
    {
        const float d1 = Cross2( a, b, p );
        const float d2 = Cross2( b, c, p );
        const float d3 = Cross2( c, a, p );
        const bool neg = ( d1 < 0.0f ) || ( d2 < 0.0f ) || ( d3 < 0.0f );
        const bool pos = ( d1 > 0.0f ) || ( d2 > 0.0f ) || ( d3 > 0.0f );
        return !( neg && pos );
    }

    void RewindCCW( std::vector<float> &pts )
    {
        if ( KiwiRegion_SignedArea( pts ) >= 0.0f )
            return;
        const int n = PtCount( pts );
        for ( int i = 0; i < n / 2; ++i )
        {
            const int j = n - 1 - i;
            float tu = pts[i * 2 + 0], tv = pts[i * 2 + 1];
            pts[i * 2 + 0] = pts[j * 2 + 0];
            pts[i * 2 + 1] = pts[j * 2 + 1];
            pts[j * 2 + 0] = tu;
            pts[j * 2 + 1] = tv;
        }
    }

    // Drop consecutive duplicates (and the wrap duplicate), so a loop built by
    // welding chain endpoints never carries a zero-length edge into the ear clip.
    void DedupLoop( std::vector<float> &pts, float tol )
    {
        std::vector<float> out;
        const int n = PtCount( pts );
        for ( int i = 0; i < n; ++i )
        {
            if ( !out.empty() )
            {
                const int m = PtCount( out );
                const float du = pts[i * 2 + 0] - out[( m - 1 ) * 2 + 0];
                const float dv = pts[i * 2 + 1] - out[( m - 1 ) * 2 + 1];
                if ( sqrtf( du * du + dv * dv ) <= tol )
                    continue;
            }
            out.push_back( pts[i * 2 + 0] );
            out.push_back( pts[i * 2 + 1] );
        }
        const int m = PtCount( out );
        if ( m >= 2 )
        {
            const float du = out[0] - out[( m - 1 ) * 2 + 0];
            const float dv = out[1] - out[( m - 1 ) * 2 + 1];
            if ( sqrtf( du * du + dv * dv ) <= tol )
                out.resize( ( m - 1 ) * 2 );
        }
        pts.swap( out );
    }

    // Remove successive edges that §19 would turn into duplicate prism side planes.
    // Reuse KVALID_PLANE_DOT and repeat because one removal can expose another.
    void DropCollinear( std::vector<float> &pts )
    {
        bool changed = true;
        while ( changed )
        {
            changed = false;
            const int n = PtCount( pts );
            if ( n < 4 )                     // a triangle has no redundant vertex
                return;
            for ( int i = 0; i < n; ++i )
            {
                const float *a = Pt( pts, ( i + n - 1 ) % n );
                const float *b = Pt( pts, i );
                const float *c = Pt( pts, ( i + 1 ) % n );
                const float e0[2] = { b[0] - a[0], b[1] - a[1] };
                const float e1[2] = { c[0] - b[0], c[1] - b[1] };
                const float l0 = sqrtf( e0[0] * e0[0] + e0[1] * e0[1] );
                const float l1 = sqrtf( e1[0] * e1[0] + e1[1] * e1[1] );
                if ( !( l0 > 1.0e-6f ) || !( l1 > 1.0e-6f ) )
                    continue;                // a zero-length edge; DedupLoop owns it
                const float dot = ( e0[0] * e1[0] + e0[1] * e1[1] ) / ( l0 * l1 );
                if ( dot <= KVALID_PLANE_DOT )
                    continue;
                pts.erase( pts.begin() + (size_t)i * 2, pts.begin() + (size_t)i * 2 + 2 );
                changed = true;
                break;
            }
        }
    }

    // Closed plane-space specialization of the shared finest-edge weld bound.
    float FinestLoopEdge( const std::vector<float> &pts )
    {
        return KiwiRegion_FinestEdge( pts.empty() ? 0 : &pts[0],
                                      PtCount( pts ), 2, true );
    }

    // Joined and unjoined outlines must sanitize identically: bounded dedupe,
    // collinear removal, then dedupe again because removal can expose a new weld.
    // Winding remains an acceptance decision in AcceptLoop.
    void SanitizeRing( std::vector<float> &pts )
    {
        DedupLoop( pts, KiwiRegion_WeldFor( FinestLoopEdge( pts ) ) );
        DropCollinear( pts );
        DedupLoop( pts, KiwiRegion_WeldFor( FinestLoopEdge( pts ) ) );
    }

    // Shared acceptance gate for every derivation route.
    bool AcceptLoop( kregion_t &r )
    {
        // Use the same bounded weld and collinear cleanup for all three passes.
        SanitizeRing( r.pts );
        const int n = PtCount( r.pts );
        if ( n < 3 )
            return false;
        if ( n > KREG_MAX_LOOP )
        {
            // Keep the fill/extrude cap failure visible, once per store generation.
            static unsigned s_lastCapGen = 0xFFFFFFFFu;
            if ( s_lastCapGen != KiwiCon_Generation() )
            {
                s_lastCapGen = KiwiCon_Generation();
                Sys_Printf( "Construction: an enclosed area has %i corners and the "
                            "region cap is %i, so it is neither filled nor "
                            "extrudable.  Redraw its round parts with a lower "
                            "'sides' count (Tab while drawing).\n",
                            n, KREG_MAX_LOOP );
            }
            return false;
        }
        RewindCCW( r.pts );
        if ( KiwiRegion_SignedArea( r.pts ) < KREG_MIN_AREA )
            return false;
        if ( KiwiRegion_SelfIntersects( r.pts ) )
            return false;
        return true;
    }

    // Endpoint nodes and weld distances are world-space so plane fitting can follow.
    struct chainEdge_t
    {
        int   objIndex;
        int   node0, node1;
        bool  used;
    };

    struct chainNode_t
    {
        float w[3];
        int   degree;
    };

    inline float Dist3( const float *a, const float *b )
    {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return sqrtf( dx * dx + dy * dy + dz * dz );
    }

    int NodeFor( std::vector<chainNode_t> &nodes, const float w[3], float tol )
    {
        for ( size_t i = 0; i < nodes.size(); ++i )
            if ( Dist3( nodes[i].w, w ) <= tol )
                return (int)i;
        chainNode_t n;
        n.w[0] = w[0];  n.w[1] = w[1];  n.w[2] = w[2];
        n.degree = 0;
        nodes.push_back( n );
        return (int)nodes.size() - 1;
    }

    // Membership requires every tessellated point to fit the plane band.
    bool ObjectOnPlane( const kconObject_t &o, const kconPlane_t &plane )
    {
        const int n = KiwiCon_VertCount( o );
        if ( n < 1 )
            return false;
        for ( int i = 0; i < n; ++i )
        {
            float w[3];
            if ( !KiwiCon_VertWorld( o, i, w ) )
                return false;
            const float rel[3] = { w[0] - plane.origin[0],
                                   w[1] - plane.origin[1],
                                   w[2] - plane.origin[2] };
            if ( fabsf( Dot3( rel, plane.normal ) ) > KiwiRegion_PlaneBand() )   // grid-aware band
                return false;
        }
        return true;
    }

    // Bookkeeping-only plane comparison; membership still tests every point.
    // Normals are sign-agnostic, and a false negative only repeats arrangement.
    bool SamePlaneApprox( const kconPlane_t &a, const kconPlane_t &b )
    {
        if ( fabsf( Dot3( a.normal, b.normal ) ) < 0.999f )
            return false;
        const float rel[3] = { b.origin[0] - a.origin[0],
                               b.origin[1] - a.origin[1],
                               b.origin[2] - a.origin[2] };
        return fabsf( Dot3( rel, a.normal ) ) <= KiwiRegion_PlaneBand();
    }

    // Use the same world-space endpoints that ChainWalk welds.
    bool SeedEndpoints( int object, float outA[3], float outB[3] )
    {
        const kconObject_t *o = KiwiCon_At( object );
        if ( !o )
            return false;
        const int n = KiwiCon_VertCount( *o );
        if ( n < 2 )
            return false;
        return KiwiCon_VertWorld( *o, 0, outA ) && KiwiCon_VertWorld( *o, n - 1, outB );
    }

    // Does either of `o`'s ends land on either of (a, b), within the weld tolerance?
    bool TouchesEnds( const kconObject_t &o, const float a[3], const float b[3] )
    {
        const int n = KiwiCon_VertCount( o );
        if ( n < 2 )
            return false;
        float e[2][3];
        if ( !KiwiCon_VertWorld( o, 0, e[0] ) || !KiwiCon_VertWorld( o, n - 1, e[1] ) )
            return false;
        for ( int i = 0; i < 2; ++i )
            if ( Dist3( e[i], a ) <= KiwiRegion_WeldDist()
              || Dist3( e[i], b ) <= KiwiRegion_WeldDist() )   // shared weld distance
                return true;
        return false;
    }

    // Append `o`'s world points to a scratch buffer (3 floats each) — the raw
    // material for a plane fit over a candidate set.
    void AppendWorldPoints( const kconObject_t &o, std::vector<float> *out )
    {
        const int n = KiwiCon_VertCount( o );
        for ( int i = 0; i < n; ++i )
        {
            float w[3];
            if ( !KiwiCon_VertWorld( o, i, w ) )
                return;
            out->push_back( w[0] );  out->push_back( w[1] );  out->push_back( w[2] );
        }
    }

    // Append `o`'s tessellated vertices to `loop` in walk order.  The shared
    // endpoint between two consecutive members is appended twice; AcceptLoop's
    // DedupLoop is what collapses it (one rule, one place).
    void AppendObjectPoints( const kconObject_t &o, const kconPlane_t &plane,
                             bool forward, std::vector<float> &loop )
    {
        const int n = KiwiCon_VertCount( o );
        for ( int i = 0; i < n; ++i )
        {
            float w[3], uv[2];
            if ( !KiwiCon_VertWorld( o, forward ? i : ( n - 1 - i ), w ) )
                return;
            // Through WORLD and back into the GROUP's plane: the objects in a chain
            // are coplanar within tolerance but each carries its own u/v basis, so
            // their raw plane-space coordinates are not comparable.
            KiwiCon_WorldToPlane( plane, w, uv );
            loop.push_back( uv[0] );
            loop.push_back( uv[1] );
        }
    }

    // Arrangement can rediscover pass-1/2 faces with different split vertices.
    // Match world position plus absolute area to prevent double fill/pick/extrude.
    bool DuplicateRegion( const kregion_t &r )
    {
        const int n = PtCount( r.pts );
        if ( n < 3 )
            return true;
        float c[2] = { 0.0f, 0.0f };
        for ( int i = 0; i < n; ++i )
        {
            c[0] += r.pts[i * 2 + 0];
            c[1] += r.pts[i * 2 + 1];
        }
        c[0] /= (float)n;
        c[1] /= (float)n;
        float cw[3];
        KiwiCon_PlaneToWorld( r.plane, c, cw );
        const float area = fabsf( KiwiRegion_SignedArea( r.pts ) );

        for ( size_t k = 0; k < s_regions.size(); ++k )
        {
            const kregion_t &o = s_regions[k];
            const int on = PtCount( o.pts );
            if ( on < 3 )
                continue;
            float oc[2] = { 0.0f, 0.0f };
            for ( int i = 0; i < on; ++i )
            {
                oc[0] += o.pts[i * 2 + 0];
                oc[1] += o.pts[i * 2 + 1];
            }
            oc[0] /= (float)on;
            oc[1] /= (float)on;
            float ow[3];
            KiwiCon_PlaneToWorld( o.plane, oc, ow );
            if ( Dist3( cw, ow ) > KiwiRegion_WeldDist() )      // shared weld distance
                continue;
            const float oa = fabsf( KiwiRegion_SignedArea( o.pts ) );
            const float bigger = ( oa > area ) ? oa : area;
            if ( bigger <= 0.0f || fabsf( oa - area ) <= bigger * 0.01f )
                return true;
        }
        return false;
    }

    void BuildRegions()
    {
        s_regions.clear();

        const int count = KiwiCon_Count();

        // ── PASS 1: a closed object IS a region ─────────────────────────────
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            // Hidden geometry cannot define a face the user can see or repair.
            if ( !o || o->hidden )
                continue;
            const bool closed = ( o->type == KCON_CIRCLE ) || ( o->type == KCON_RECT ) || o->closed;
            if ( !closed )
                continue;

            // Closed polylines still need a derived plane; a non-planar loop is not
            // a face.  Circles can answer with their stored plane.
            kconPlane_t plane;
            if ( !KiwiCon_ObjectPlane( *o, &plane ) )
                continue;

            kregion_t r;
            r.plane        = plane;
            r.sourceObject = i;
            const int n = KiwiCon_VertCount( *o );
            for ( int k = 0; k < n; ++k )
            {
                float w[3], uv[2];
                if ( !KiwiCon_VertWorld( *o, k, w ) )
                    break;
                KiwiCon_WorldToPlane( plane, w, uv );
                r.pts.push_back( uv[0] );
                r.pts.push_back( uv[1] );
            }
            if ( AcceptLoop( r ) )
                s_regions.push_back( r );
        }

        // ── PASS 2: chains of OPEN objects sharing a plane ──────────────────
        std::vector<int> open;
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( !o || o->hidden )        // hidden geometry is inert
                continue;
            const bool closed = ( o->type == KCON_CIRCLE ) || ( o->type == KCON_RECT ) || o->closed;
            if ( !closed && KiwiCon_VertCount( *o ) >= 2 )
                open.push_back( i );
        }

        // Planes already handled by the open-object arrangement pass.
        std::vector<kconPlane_t> arranged;

        std::vector<char> grouped( open.size(), 0 );
        for ( size_t g = 0; g < open.size(); ++g )
        {
            if ( grouped[g] )
                continue;

            // Fit the group plane from world points; a two-point seed needs a
            // partner before it determines a plane.  Later membership is point-to-plane.
            std::vector<float> seedPts;
            AppendWorldPoints( *KiwiCon_At( open[g] ), &seedPts );
            grouped[g] = 1;                  // consumed either way — do not retry it
            if ( seedPts.empty() )
                continue;                    // an object that could not answer at all

            std::vector<int> members;
            members.push_back( open[g] );

            kconPlane_t plane;
            bool havePlane = KiwiCon_FitPlane( &seedPts[0], (int)( seedPts.size() / 3 ),
                                               &plane );

            // Prefer a touching partner when a straight seed needs a plane, then
            // fall back to any fitting partner.  Ambiguous touching partners on
            // different planes still resolve by store order.
            if ( !havePlane )
            {
                float sa[3], sb[3];
                const bool haveSeedEnds = SeedEndpoints( open[g], sa, sb );
                for ( int pass = 0; pass < 2 && !havePlane; ++pass )
                {
                    if ( pass == 0 && !haveSeedEnds )
                        continue;
                    for ( size_t k = g + 1; k < open.size() && !havePlane; ++k )
                    {
                        if ( grouped[k] )
                            continue;
                        const kconObject_t *o = KiwiCon_At( open[k] );
                        if ( !o )
                            continue;
                        if ( pass == 0 && !TouchesEnds( *o, sa, sb ) )
                            continue;
                        // The fit rejects a candidate whose points stray from the plane.
                        std::vector<float> trial = seedPts;
                        AppendWorldPoints( *o, &trial );
                        if ( trial.size() == seedPts.size() )
                            continue;        // the candidate contributed nothing
                        kconPlane_t p;
                        if ( !KiwiCon_FitPlane( &trial[0], (int)( trial.size() / 3 ), &p ) )
                            continue;
                        plane     = p;
                        havePlane = true;
                        seedPts.swap( trial );
                        grouped[k] = 1;
                        members.push_back( open[k] );
                    }
                }
            }

            // Once fixed, the plane makes later membership independent of order.
            if ( havePlane )
            {
                for ( size_t k = g + 1; k < open.size(); ++k )
                {
                    if ( grouped[k] )
                        continue;
                    const kconObject_t *o = KiwiCon_At( open[k] );
                    if ( !o || !ObjectOnPlane( *o, plane ) )
                        continue;
                    grouped[k] = 1;
                    members.push_back( open[k] );
                }
            }
            // Keep one-member groups for arrangement; a self-crossing polyline can
            // bound a cell even though ChainWalk needs at least two members.
            if ( !havePlane )
                continue;

            // Region detection and Join Lines share this endpoint-graph walk.
            if ( members.size() >= 2 )
            {
                std::vector<kchainStep_t> steps;
                bool closed = false;
                if ( KiwiRegion_ChainWalk( &members[0], (int)members.size(),
                                           &steps, &closed )
                  && closed
                  // Two curved objects can enclose a loop; the area gate rejects
                  // the degenerate two-straight-segment case.
                  && steps.size() >= 2 )
                {
                    std::vector<float> loop;
                    for ( size_t s = 0; s < steps.size(); ++s )
                        AppendObjectPoints( *KiwiCon_At( steps[s].object ), plane,
                                            steps[s].forward, loop );

                    kregion_t r;
                    r.plane        = plane;
                    r.sourceObject = -1;
                    r.pts.swap( loop );
                    if ( AcceptLoop( r ) )
                        s_regions.push_back( r );
                }
            }

            // Arrangement runs even after a chain succeeds so mid-span crossings
            // also produce cells.  Add coplanar closed objects here because they can
            // subdivide a pass-1 face; DuplicateRegion removes repeated faces.
            {
                std::vector<int> arrMembers( members );
                for ( int k = 0; k < count; ++k )
                {
                    const kconObject_t *o = KiwiCon_At( k );
                    if ( !o || o->hidden )    // hidden geometry is inert
                        continue;
                    const bool isClosed = ( o->type == KCON_CIRCLE ) || ( o->type == KCON_RECT )
                                       || o->closed;
                    if ( !isClosed || !ObjectOnPlane( *o, plane ) )
                        continue;
                    arrMembers.push_back( k );
                }
                arranged.push_back( plane );   // prevents the closed-only sweep repeating it

                std::vector<karrCell_t> cells;
                if ( KiwiArrange_Cells( &arrMembers[0], (int)arrMembers.size(),
                                        plane, &cells ) )
                {
                    for ( size_t c = 0; c < cells.size(); ++c )
                    {
                        kregion_t r;
                        r.plane        = plane;
                        r.sourceObject = -1;
                        r.pts.swap( cells[c].pts );
                        if ( !AcceptLoop( r ) )
                            continue;
                        if ( DuplicateRegion( r ) )
                            continue;
                        s_regions.push_back( r );
                    }
                }
            }
        }

        // Closed-only planes never seed the open-object pass.  Sweep each remaining
        // such plane over all coplanar objects, with the same duplicate guard.
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            if ( !o || o->hidden )
                continue;
            const bool isClosed = ( o->type == KCON_CIRCLE ) || ( o->type == KCON_RECT )
                               || o->closed;
            if ( !isClosed )
                continue;

            kconPlane_t plane;
            if ( !KiwiCon_ObjectPlane( *o, &plane ) )
                continue;

            bool already = false;
            for ( size_t a = 0; a < arranged.size() && !already; ++a )
                already = SamePlaneApprox( arranged[a], plane );
            if ( already )
                continue;
            arranged.push_back( plane );

            std::vector<int> arrMembers;
            for ( int k = 0; k < count; ++k )
            {
                const kconObject_t *ok = KiwiCon_At( k );
                if ( !ok || ok->hidden || !ObjectOnPlane( *ok, plane ) )
                    continue;
                arrMembers.push_back( k );
            }
            // A single closed object is assumed to be pass-1-only, so arrangement
            // starts at two members on a closed-only plane.
            if ( arrMembers.size() < 2 )
                continue;

            std::vector<karrCell_t> cells;
            if ( !KiwiArrange_Cells( &arrMembers[0], (int)arrMembers.size(),
                                     plane, &cells ) )
                continue;
            for ( size_t c = 0; c < cells.size(); ++c )
            {
                kregion_t r;
                r.plane        = plane;
                r.sourceObject = -1;
                r.pts.swap( cells[c].pts );
                if ( !AcceptLoop( r ) )
                    continue;
                if ( DuplicateRegion( r ) )
                    continue;
                s_regions.push_back( r );
            }
        }

        // Cache each region's world-space match key for flash and selection carry.
        for ( size_t i = 0; i < s_regions.size(); ++i )
        {
            kregion_t &r = s_regions[i];
            const int n = PtCount( r.pts );
            if ( n < 1 )
                continue;
            float c[2] = { 0.0f, 0.0f };
            for ( int k = 0; k < n; ++k )
            {
                c[0] += r.pts[k * 2 + 0];
                c[1] += r.pts[k * 2 + 1];
            }
            c[0] /= (float)n;
            c[1] /= (float)n;
            KiwiCon_PlaneToWorld( r.plane, c, r.centroid );
        }
    }

    // Carry flash state by world position rather than unstable derived indices.
    // The generous radius tolerates small rebuild motion.
    const float KREG_FLASH_MATCH = 2.0f;

    void CarryFlash( const std::vector<kregion_t> &prev, bool announce )
    {
        // Require count growth before declaring an unmatched region new; otherwise
        // a moving region can miss the position match and re-flash every frame.
        const bool grew  = s_regions.size() > prev.size();
        int        fresh = 0;
        for ( size_t i = 0; i < s_regions.size(); ++i )
        {
            int carried = -1;
            for ( size_t p = 0; p < prev.size() && carried < 0; ++p )
            {
                const float dx = prev[p].centroid[0] - s_regions[i].centroid[0];
                const float dy = prev[p].centroid[1] - s_regions[i].centroid[1];
                const float dz = prev[p].centroid[2] - s_regions[i].centroid[2];
                if ( sqrtf( dx * dx + dy * dy + dz * dz ) <= KREG_FLASH_MATCH )
                    carried = prev[p].flash;
            }
            if ( carried >= 0 )
            {
                s_regions[i].flash = carried;
                continue;
            }
            if ( !grew )
            {
                s_regions[i].flash = 0;      // moved, reshaped — not new
                continue;
            }
            s_regions[i].flash = KREG_FLASH_FRAMES;
            ++fresh;
        }
        if ( announce && fresh > 0 )
            Sys_Printf( "Construction: %i region%s closed (%i total).\n",
                        fresh, ( fresh == 1 ) ? "" : "s", (int)s_regions.size() );
    }

    void EnsureBuilt()
    {
        const unsigned gen = KiwiCon_Generation();
        if ( !s_dirty && gen == s_builtFor )
            return;
        // Loaded sidecar regions are not newly closed during the first build.
        const bool first = ( s_builtFor == 0 );
        std::vector<kregion_t> prev;
        prev.swap( s_regions );
        BuildRegions();
        CarryFlash( prev, !first );
        if ( first )
            for ( size_t i = 0; i < s_regions.size(); ++i )
                s_regions[i].flash = 0;
        s_builtFor = gen;
        s_dirty    = false;
    }
}

// Region list.
const std::vector<kregion_t> &KiwiRegion_All()
{
    EnsureBuilt();
    return s_regions;
}

void KiwiRegion_Invalidate()
{
    s_dirty = true;
}

// Shared coplanarity and chain helpers.
bool KiwiRegion_ObjectOnPlane( const kconObject_t &o, const kconPlane_t &plane )
{
    return ObjectOnPlane( o, plane );
}

// Shared sanitizer preserves the joined/unjoined region invariant.
void KiwiRegion_SanitizeRing( std::vector<float> &pts )
{
    SanitizeRing( pts );
}

bool KiwiRegion_ChainWalk( const int *objects, int count,
                           std::vector<kchainStep_t> *outSteps, bool *outClosed )
{
    if ( outSteps )
        outSteps->clear();
    if ( outClosed )
        *outClosed = false;
    if ( !objects || count < 1 || !outSteps )
        return false;

    // Weld every member's two world-space ends into shared nodes.
    std::vector<chainNode_t> nodes;
    std::vector<chainEdge_t> edges;
    for ( int m = 0; m < count; ++m )
    {
        const kconObject_t *o = KiwiCon_At( objects[m] );
        if ( !o )
            continue;
        const int n = KiwiCon_VertCount( *o );
        if ( n < 2 )
            continue;
        float w0[3], w1[3];
        if ( !KiwiCon_VertWorld( *o, 0, w0 ) || !KiwiCon_VertWorld( *o, n - 1, w1 ) )
            continue;

        // Self-closing objects belong to pass 1; drop them before creating nodes.
        if ( Dist3( w0, w1 ) <= KiwiRegion_WeldDist() )         // pass-1 object, not a link
            continue;

        chainEdge_t e;
        e.objIndex = objects[m];
        e.node0    = NodeFor( nodes, w0, KiwiRegion_WeldDist() );   // shared weld distance
        e.node1    = NodeFor( nodes, w1, KiwiRegion_WeldDist() );
        e.used     = false;
        if ( e.node0 == e.node1 )
            continue;                        // welded onto one node — same case
        ++nodes[e.node0].degree;
        ++nodes[e.node1].degree;
        edges.push_back( e );
    }
    if ( edges.empty() )
        return false;

    // §8 read strictly: a node of degree 3+ is a T-junction and the whole set is
    // ambiguous.  Degree 1 is fine here (it is how an OPEN chain ends) — a REGION
    // additionally requires the walk to come back, which the `closed` flag says.
    for ( size_t i = 0; i < nodes.size(); ++i )
        if ( nodes[i].degree > 2 )
            return false;

    // Start at an END when there is one, so an open chain comes back in its
    // natural order; otherwise anywhere, because a cycle has no natural start.
    int startNode = edges[0].node0;
    for ( size_t i = 0; i < nodes.size(); ++i )
        if ( nodes[i].degree == 1 )
        {
            startNode = (int)i;
            break;
        }

    int    node     = startNode;
    size_t consumed = 0;
    for ( size_t guard = 0; guard < edges.size() + 1; ++guard )
    {
        int pick = -1;
        for ( size_t e = 0; e < edges.size() && pick < 0; ++e )
        {
            if ( edges[e].used )
                continue;
            if ( edges[e].node0 == node || edges[e].node1 == node )
                pick = (int)e;
        }
        if ( pick < 0 )
            break;
        chainEdge_t &e = edges[pick];
        const bool forward = ( e.node0 == node );
        e.used = true;
        ++consumed;

        kchainStep_t st;
        st.object  = e.objIndex;
        st.forward = forward;
        outSteps->push_back( st );

        node = forward ? e.node1 : e.node0;
        if ( node == startNode )
            break;                           // back to the start: a closed cycle
    }

    // ONE walk must consume EVERYTHING.  A set that splits into two chains fails
    // rather than silently returning half of itself.
    if ( consumed != edges.size() )
    {
        outSteps->clear();
        return false;
    }
    if ( outClosed )
        *outClosed = ( node == startNode );
    return true;
}

int KiwiRegion_PickAt( const ray_t &ray )
{
    const std::vector<kregion_t> &regions = KiwiRegion_All();
    int   best     = -1;
    float bestDist = 0.0f;

    for ( size_t i = 0; i < regions.size(); ++i )
    {
        float w[3];
        if ( !KiwiCon_RayPlane( regions[i].plane, ray, w ) )
            continue;
        float uv[2];
        KiwiCon_WorldToPlane( regions[i].plane, w, uv );

        // Even-odd crossing test in plane space.
        bool inside = false;
        const std::vector<float> &pts = regions[i].pts;
        const int n = PtCount( pts );
        for ( int a = 0, b = n - 1; a < n; b = a++ )
        {
            const float *pa = Pt( pts, a );
            const float *pb = Pt( pts, b );
            if ( ( ( pa[1] > uv[1] ) != ( pb[1] > uv[1] ) )
              && ( uv[0] < ( pb[0] - pa[0] ) * ( uv[1] - pa[1] ) / ( pb[1] - pa[1] ) + pa[0] ) )
                inside = !inside;
        }
        if ( !inside )
            continue;

        const float d = sqrtf( ( w[0] - ray.origin[0] ) * ( w[0] - ray.origin[0] )
                             + ( w[1] - ray.origin[1] ) * ( w[1] - ray.origin[1] )
                             + ( w[2] - ray.origin[2] ) * ( w[2] - ray.origin[2] ) );
        if ( best < 0 || d < bestDist )
        {
            best     = (int)i;
            bestDist = d;
        }
    }
    return best;
}

bool KiwiRegion_HitDistance( const ray_t &ray, int index, float *outDist )
{
    const std::vector<kregion_t> &regions = KiwiRegion_All();
    if ( index < 0 || index >= (int)regions.size() )
        return false;
    float w[3];
    if ( !KiwiCon_RayPlane( regions[index].plane, ray, w ) )
        return false;
    if ( outDist )
        *outDist = sqrtf( ( w[0] - ray.origin[0] ) * ( w[0] - ray.origin[0] )
                        + ( w[1] - ray.origin[1] ) * ( w[1] - ray.origin[1] )
                        + ( w[2] - ray.origin[2] ) * ( w[2] - ray.origin[2] ) );
    return true;
}

// Region selection is stored as world-space match keys, not derived indices.
namespace
{
    struct selCentroid_t { float c[3]; };
    std::vector<selCentroid_t> s_selCentroids;

    // Reuse the flash carry tolerance for the same cross-derive identity test.
    int ResolveCentroid( const float *c )
    {
        const std::vector<kregion_t> &regions = KiwiRegion_All();
        int   best  = -1;
        float bestD = KREG_FLASH_MATCH;
        for ( size_t i = 0; i < regions.size(); ++i )
        {
            const float d = Dist3( regions[i].centroid, c );
            if ( d <= bestD )
            {
                bestD = d;
                best  = (int)i;
            }
        }
        return best;
    }
}

void KiwiRegion_Select( int index )
{
    const std::vector<kregion_t> &regions = KiwiRegion_All();
    if ( index < 0 || index >= (int)regions.size() )
    {
        KiwiRegion_ClearSelection();
        return;
    }
    s_selCentroids.clear();
    selCentroid_t e;
    for ( int k = 0; k < 3; ++k )
        e.c[k] = regions[index].centroid[k];
    s_selCentroids.push_back( e );
    g_nUpdateBits |= 1;
}

// Shift+click uses the same toggle grammar as other selectable geometry.
void KiwiRegion_ToggleSelect( int index )
{
    const std::vector<kregion_t> &regions = KiwiRegion_All();
    if ( index < 0 || index >= (int)regions.size() )
        return;

    for ( size_t i = 0; i < s_selCentroids.size(); ++i )
    {
        if ( ResolveCentroid( s_selCentroids[i].c ) == index )
        {
            s_selCentroids.erase( s_selCentroids.begin() + i );
            g_nUpdateBits |= 1;
            return;
        }
    }
    selCentroid_t e;
    for ( int k = 0; k < 3; ++k )
        e.c[k] = regions[index].centroid[k];
    s_selCentroids.push_back( e );
    g_nUpdateBits |= 1;
}

void KiwiRegion_ClearSelection()
{
    if ( s_selCentroids.empty() )
        return;
    s_selCentroids.clear();
    g_nUpdateBits |= 1;
}

int KiwiRegion_SelectedIndex()
{
    // The first still-live member is the primary selection for single-target callers.
    for ( size_t i = 0; i < s_selCentroids.size(); ++i )
    {
        const int idx = ResolveCentroid( s_selCentroids[i].c );
        if ( idx >= 0 )
            return idx;
    }
    return -1;
}

int KiwiRegion_SelectedCount()
{
    int n = 0;
    for ( size_t i = 0; i < s_selCentroids.size(); ++i )
        if ( ResolveCentroid( s_selCentroids[i].c ) >= 0 )
            ++n;
    return n;
}

int KiwiRegion_SelectedAt( int which )
{
    if ( which < 0 )
        return -1;
    int n = 0;
    for ( size_t i = 0; i < s_selCentroids.size(); ++i )
    {
        const int idx = ResolveCentroid( s_selCentroids[i].c );
        if ( idx < 0 )
            continue;                    // a region that stopped existing
        if ( n == which )
            return idx;
        ++n;
    }
    return -1;
}

bool KiwiRegion_IsSelected( int index )
{
    if ( index < 0 )
        return false;
    for ( size_t i = 0; i < s_selCentroids.size(); ++i )
        if ( ResolveCentroid( s_selCentroids[i].c ) == index )
            return true;
    return false;
}

bool KiwiRegion_HasSelection()
{
    return KiwiRegion_SelectedIndex() >= 0;
}

// Translucent engine-route fill.
void KiwiRegion_DrawFills( int highlightIndex )
{
    // This is the only per-frame path allowed to age region flash counters.
    EnsureBuilt();
    std::vector<kregion_t> &regions = s_regions;

    // Track the first failed derivation/render gate and submitted triangle count.
    // Technique availability and command-buffer room are checked before the void
    // renderer call can silently discard a draw.
    const char *gate      = nullptr;              // the FIRST gate that closed
    int         nRegions  = (int)regions.size();
    int         nFills    = 0;                    // convex-piece draw calls submitted
    int         nTrisSub  = 0;                    // triangles handed to the renderer
    if ( nRegions == 0 )
        gate = "DERIVATION (no region was built from the construction store)";

    if ( regions.empty() )
    {
        // Report an unclosed store once per generation, with its nearest gap.
        if ( KiwiCon_Count() > 0 )
        {
            static unsigned s_lastEmptyGen = 0xFFFFFFFFu;
            if ( s_lastEmptyGen != KiwiCon_Generation() )
            {
                s_lastEmptyGen = KiwiCon_Generation();
                Sys_Printf( "Construction: %i object%s in the store and NO region derived "
                            "- gate %s.  Nothing can fill or extrude until a loop closes.\n",
                            KiwiCon_Count(), ( KiwiCon_Count() == 1 ) ? "" : "s", gate );
                KiwiRegion_ReportGaps( true );
            }
        }
        return;
    }

    // CamWnd_BuildMatrix has already produced this frame's view state.
    const camera_s *c = Ed_Camera();

    // Match the selected-face material-color bracket and restore white afterwards.
    // 0x408106
    static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
    R_AddCmdSetMaterialColor( s_neutral );

    // Resolve the selection set once into flags.  The accessors still make this
    // O(sel² * regions); any combined resolver must preserve the last-equal match.
    std::vector<char> selFlags( regions.size(), 0 );
    {
        const int selN = KiwiRegion_SelectedCount();
        for ( int s = 0; s < selN; ++s )
        {
            const int idx = KiwiRegion_SelectedAt( s );
            if ( idx >= 0 && idx < (int)selFlags.size() )
                selFlags[(size_t)idx] = 1;
        }
    }

    // Regions are bounded by KREG_MAX_LOOP; donor pieces have a tighter checked cap.
    const int KREG_DONOR_MAX_PTS = 64;           // == CAM_MAXFACEVERTS, camwnd.cpp:532
    static float s_world[KREG_MAX_LOOP][3];      // the loop, in world space
    static float s_piece[KREG_MAX_LOOP][3];      // one convex piece of it

    // stateBitsEntry[TECHNIQUE_UNLIT] == 0xFF predicts Material_GetTechnique's
    // silent null-technique drop; the material cannot change within this pass.
    if ( !gate )
    {
        if ( !g_qeglobals.d_white )
        {
            gate = "MATERIAL (g_qeglobals.d_white is null — white_tools never registered)";
        }
        else
        {
            const Material *wm = Material_FromHandle( g_qeglobals.d_white );
            if ( !wm || !wm->stateBitsTable )
                gate = "MATERIAL (white_tools has no state-bits table)";
            else if ( wm->stateBitsEntry[TECHNIQUE_UNLIT] == 0xFF )
                gate = "TECHNIQUE (white_tools has no TECHNIQUE_UNLIT — the draw is dropped "
                       "silently inside R_AddRenderCmdDrawTris)";
        }
    }

    // A pre-loop gate invalidates every apparent submit, so preserve it for reporting.
    const bool passGateClosed = ( gate != nullptr );

    // Count per-region rejection causes for the once-per-generation anomaly report.
    int nTooFew = 0, nTooMany = 0, nNoTris = 0;

    // One submitted sample for the once-per-generation route diagnostic.
    int   emitSample       = -1;
    int   emitSamplePieces = 0;
    int   emitSampleVerts  = 0;
    float emitSampleV[3]    = { 0.0f, 0.0f, 0.0f };
    float emitSampleRgba[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    for ( size_t r = 0; r < regions.size(); ++r )
    {
        kregion_t &reg = regions[r];
        const int n = PtCount( reg.pts );

        // Age before skip paths so an undrawable region cannot remain flashed forever.
        const int flash = reg.flash;
        if ( reg.flash > 0 )
        {
            --reg.flash;
            // Repaint until the frame-counted flash completes.
            g_nUpdateBits |= 1;
        }

        if ( n < 3 || n > KREG_MAX_LOOP )
        {
            if ( n < 3 ) ++nTooFew; else ++nTooMany;
            if ( !gate )
                gate = ( n < 3 ) ? "CORNERS (a region has fewer than 3 corners)"
                                 : "CORNERS (a region is over the KREG_MAX_LOOP cap)";
            continue;
        }

        // Cam_DrawWindingTinted fans one convex polygon, so concave loops use the
        // same ear-clip/Hertel-Mehlhorn decomposition as extrusion.
        std::vector< std::vector<int> > pieces;
        if ( !KiwiRegion_ConvexPieces( reg.pts, &pieces ) || pieces.empty() )
        {
            ++nNoTris;
            if ( !gate ) gate = "TRIANGULATE (the ear-clip refused the loop)";
            continue;
        }

        // Persistent selection outranks the caller's transient preview highlight.
        float rgba[4];
        memcpy( rgba, selFlags[r]                   ? KREG_SELECTED
                    : ( (int)r == highlightIndex )  ? KREG_HILITE
                                                    : KREG_FILL, sizeof( rgba ) );
        // Lerp from flash to resting color instead of snapping between them.
        if ( flash > 0 )
        {
            const float t = (float)flash / (float)KREG_FLASH_FRAMES;
            for ( int k = 0; k < 4; ++k )
                rgba[k] += ( KREG_FLASH[k] - rgba[k] ) * t;
        }
        GfxColor packed;
        Byte4PackPixelColor( rgba, &packed );

        // Match the visible sky-film bracket: MATERIAL_COLOR supplies flat RGB at
        // alpha 1, while the packed per-vertex color supplies translucency.
        const float flat[4] = { rgba[0], rgba[1], rgba[2], 1.0f };
        R_AddCmdSetMaterialColor( flat );

        // The donor applies push along n, so orient the region normal toward the
        // viewer before applying KREG_FILL_NUDGE.
        float nrm[3] = { reg.plane.normal[0], reg.plane.normal[1], reg.plane.normal[2] };
        {
            float mid[3];
            KiwiCon_PlaneToWorld( reg.plane, Pt( reg.pts, 0 ), mid );
            const float toEye[3] = { c->origin[0] - mid[0],
                                     c->origin[1] - mid[1],
                                     c->origin[2] - mid[2] };
            // Ortho has no eye point; choose the side from view direction instead.
            const float side = KiwiCam_Ortho() ? -Dot3( nrm, c->vpn ) : Dot3( nrm, toEye );
            if ( side < 0.0f )
                for ( int k = 0; k < 3; ++k )
                    nrm[k] = -nrm[k];
        }

        for ( int i = 0; i < n; ++i )
        {
            // The donor applies nudge, normals, and planar ST from these world points.
            float w[3];
            KiwiCon_PlaneToWorld( reg.plane, Pt( reg.pts, i ), w );
            s_world[i][0] = w[0];
            s_world[i][1] = w[1];
            s_world[i][2] = w[2];
        }
        // Region-plane normals match Face_AddWindingToTriBatch.
        // 0x47b86a
        // Reverse viewer-away pieces because white_tools culls back faces.
        // The byte formula mirrors R_GetCommandBuffer and is checked per piece.
        const bool flip = ( Dot3( nrm, reg.plane.normal ) < 0.0f );
        for ( size_t pi = 0; pi < pieces.size(); ++pi )
        {
            const std::vector<int> &poly = pieces[pi];
            const int pn = (int)poly.size();
            // Enforce Cam_DrawWindingTinted's silent CAM_MAXFACEVERTS stack bound.
            if ( pn < 3 || pn > KREG_DONOR_MAX_PTS )
            {
                ++nNoTris;
                if ( !gate ) gate = ( pn < 3 )
                    ? "TRIANGULATE (a convex piece has fewer than 3 corners)"
                    : "TRIANGULATE (a convex piece is over the donor route's 64-vertex cap)";
                continue;
            }
            bool badIndex = false;
            for ( int i = 0; i < pn; ++i )
            {
                const int src = poly[ flip ? ( pn - 1 - i ) : i ];
                if ( src < 0 || src >= n )
                {
                    badIndex = true;
                    break;
                }
                s_piece[i][0] = s_world[src][0];
                s_piece[i][1] = s_world[src][1];
                s_piece[i][2] = s_world[src][2];
            }
            if ( badIndex )
            {
                ++nNoTris;
                if ( !gate ) gate = "TRIANGULATE (a convex piece indexed off the loop)";
                continue;
            }

            const int ic    = ( pn - 2 ) * 3;
            const int bytes = 16 + ( 16 + 12 + 4 + 8 ) * pn + 2 * ( ( ic + 1 ) & ~1 );
            if ( R_Ed_CmdBufferHeadroom() < bytes )
            {
                if ( !gate )
                    gate = "CMDBUF (the render command buffer is full — the draw is "
                           "dropped silently inside R_GetCommandBuffer)";
                continue;
            }

            // Use the sky-film donor's material, technique, planar ST, normal, and fan.
            Cam_DrawWindingTinted( (const float (*)[3])s_piece, pn, nrm,
                                   g_qeglobals.d_white, (uint)packed.packed,
                                   KREG_FILL_NUDGE, TECHNIQUE_UNLIT );
            ++nFills;
            nTrisSub += pn - 2;
        }

        // Capture the first submitted sample for stable diagnostics.
        if ( emitSample < 0 && nFills > 0 )
        {
            emitSample       = (int)r;
            emitSamplePieces = (int)pieces.size();
            emitSampleVerts  = n;
            emitSampleV[0]   = s_world[0][0];
            emitSampleV[1]   = s_world[0][1];
            emitSampleV[2]   = s_world[0][2];
            for ( int k = 0; k < 4; ++k )
                emitSampleRgba[k] = rgba[k];
        }
    }

    R_AddCmdSetMaterialColor( s_white );

    // Once per store generation, confirm the route and one submitted sample.
    if ( emitSample >= 0 )
    {
        static unsigned s_lastEmitGen = 0xFFFFFFFFu;
        if ( s_lastEmitGen != KiwiCon_Generation() )
        {
            s_lastEmitGen = KiwiCon_Generation();
            Sys_Printf( "Region fill: %i region%s -> Cam_DrawWindingTinted (the SKY FILM "
                        "route).  Sample region %i: %i convex piece%s, %i loop verts, "
                        "first vert (%.1f %.1f %.1f), rgba %.2f/%.2f/%.2f/%.2f, "
                        "MATERIAL_COLOR.w 1, TECHNIQUE_UNLIT on white_tools.\n",
                        nFills, ( nFills == 1 ) ? "" : "s",
                        emitSample, emitSamplePieces,
                        ( emitSamplePieces == 1 ) ? "" : "s", emitSampleVerts,
                        emitSampleV[0], emitSampleV[1], emitSampleV[2],
                        emitSampleRgba[0], emitSampleRgba[1],
                        emitSampleRgba[2], emitSampleRgba[3] );
        }
    }

    // Report zero submitted triangles or a pass-level silent-drop gate once per
    // generation.  With both internal gates open, loss is downstream in blending
    // or shading rather than derivation or command submission.
    if ( nTrisSub == 0 || passGateClosed )
    {
        static unsigned s_lastFillGen = 0xFFFFFFFFu;
        if ( s_lastFillGen != KiwiCon_Generation() )
        {
            s_lastFillGen = KiwiCon_Generation();
            Sys_Printf( "Construction: %i region%s derived, %i fill%s emitted, %i triangles "
                        "submitted - first closed gate: %s.  (%i under 3 corners, %i over "
                        "the %i cap, %i would not triangulate.)\n",
                        nRegions, ( nRegions == 1 ) ? "" : "s",
                        nFills,   ( nFills   == 1 ) ? "" : "s",
                        nTrisSub,
                        gate ? gate : "NONE - every gate in the pass is open, so the loss is "
                                      "downstream of R_AddRenderCmdDrawTris (blend state or "
                                      "the vertcol_shaded vertex term)",
                        nTooFew, nTooMany, KREG_MAX_LOOP, nNoTris );
        }
    }
    else if ( gate )
    {
        // A partial fill is also anomalous and worth one throttled report.
        static unsigned s_lastPartialGen = 0xFFFFFFFFu;
        if ( s_lastPartialGen != KiwiCon_Generation() )
        {
            s_lastPartialGen = KiwiCon_Generation();
            Sys_Printf( "Construction: %i of %i regions filled (%i triangles) - the rest "
                        "stopped at gate: %s.\n",
                        nFills, nRegions, nTrisSub, gate );
        }
    }
}

// The ImGui overlay is a guaranteed-visible fallback for the engine-route fill.
// It has no depth buffer, so occluded regions show through world geometry.
// The engine route remains responsible for flash aging and renderer diagnostics.
// Pick_WorldToImage returns top-left image pixels; perspective loops with any
// vertex at/behind the eye plane are dropped whole rather than near-plane clipped.
namespace
{
    // Bound overlay work independently of the per-region vertex cap.
    const int   KREG_OVERLAY_MAX_LOOPS = 96;
    const ImU32 KREG_OVERLAY_FILL      = IM_COL32( 120, 180, 255,  77 );  // ~0.30 alpha
    const ImU32 KREG_OVERLAY_EDGE      = IM_COL32( 165, 210, 255, 150 );
    const ImU32 KREG_OVERLAY_FILL_SEL  = IM_COL32( 200, 230, 255, 105 );
    const ImU32 KREG_OVERLAY_EDGE_SEL  = IM_COL32( 235, 245, 255, 200 );
}

void KiwiRegion_DrawFillsOverlay( float imgMinX, float imgMinY, float imgW, float imgH )
{
    if ( !KiwiCon_ShowConstruction() )        // the same toggle the lines obey
        return;
    if ( !( imgW > 1.0f ) || !( imgH > 1.0f ) )
        return;

    // Read only; the engine route alone ages the flash once per frame.
    const std::vector<kregion_t> &regions = KiwiRegion_All();
    if ( regions.empty() )
        return;

    // Resolve selection once for the pass, as in the engine route.
    std::vector<char> selFlags( regions.size(), 0 );
    {
        const int selN = KiwiRegion_SelectedCount();
        for ( int s = 0; s < selN; ++s )
        {
            const int idx = KiwiRegion_SelectedAt( s );
            if ( idx >= 0 && idx < (int)selFlags.size() )
                selFlags[(size_t)idx] = 1;
        }
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    if ( !dl )
        return;
    const ImVec2 clipMin( imgMinX, imgMinY );
    const ImVec2 clipMax( imgMinX + imgW, imgMinY + imgH );
    dl->PushClipRect( clipMin, clipMax, true );

    ImVec2 scr[KREG_MAX_LOOP];
    int    drawn = 0;

    for ( size_t r = 0; r < regions.size() && drawn < KREG_OVERLAY_MAX_LOOPS; ++r )
    {
        const kregion_t &reg = regions[r];
        const int n = PtCount( reg.pts );
        if ( n < 3 || n > KREG_MAX_LOOP )
            continue;

        // Drop the whole loop if any vertex is at or behind the eye plane.
        bool  ok      = true;
        bool  onScreen = false;
        for ( int i = 0; i < n && ok; ++i )
        {
            float w[3], sx = 0.0f, sy = 0.0f;
            KiwiCon_PlaneToWorld( reg.plane, Pt( reg.pts, i ), w );
            if ( !Pick_WorldToImage( w, &sx, &sy ) )
            {
                ok = false;                    // at/behind the eye plane
                break;
            }
            scr[i].x = imgMinX + sx;
            scr[i].y = imgMinY + sy;
            // Cheap padded vertex-near-image test; the clip rect handles final coverage.
            if ( scr[i].x >= clipMin.x - imgW && scr[i].x <= clipMax.x + imgW
              && scr[i].y >= clipMin.y - imgH && scr[i].y <= clipMax.y + imgH )
                onScreen = true;
        }
        if ( !ok || !onScreen )
            continue;

        // Draw both sides, matching picking.  Concave loops use the shared convex
        // decomposition because AddConvexPolyFilled would otherwise fill the hull.
        std::vector< std::vector<int> > pieces;
        const bool sel = ( selFlags[r] != 0 );
        if ( KiwiRegion_ConvexPieces( reg.pts, &pieces ) && !pieces.empty() )
        {
            ImVec2 poly[KREG_MAX_LOOP];
            for ( size_t pi = 0; pi < pieces.size(); ++pi )
            {
                const std::vector<int> &idx = pieces[pi];
                const int pn = (int)idx.size();
                if ( pn < 3 || pn > KREG_MAX_LOOP )
                    continue;
                bool bad = false;
                for ( int i = 0; i < pn && !bad; ++i )
                {
                    const int src = idx[i];
                    if ( src < 0 || src >= n ) { bad = true; break; }
                    poly[i] = scr[src];
                }
                if ( bad )
                    continue;
                dl->AddConvexPolyFilled( poly, pn,
                                         sel ? KREG_OVERLAY_FILL_SEL : KREG_OVERLAY_FILL );
            }
        }
        else
        {
            // Preserve the boundary when triangulation refuses the fill.
        }

        // Draw the boundary over any fill.
        dl->AddPolyline( scr, n, sel ? KREG_OVERLAY_EDGE_SEL : KREG_OVERLAY_EDGE,
                         ImDrawFlags_Closed, 1.5f );
        ++drawn;
    }

    dl->PopClipRect();
}

// The 2D toolkit expects CCW plane-space loops.
float KiwiRegion_SignedArea( const std::vector<float> &pts )
{
    const int n = PtCount( pts );
    if ( n < 3 )
        return 0.0f;
    float a = 0.0f;
    for ( int i = 0, j = n - 1; i < n; j = i++ )
        a += Pt( pts, j )[0] * Pt( pts, i )[1] - Pt( pts, i )[0] * Pt( pts, j )[1];
    return a * 0.5f;
}

bool KiwiRegion_SelfIntersects( const std::vector<float> &pts )
{
    const int n = PtCount( pts );
    if ( n < 4 )
        return false;
    for ( int i = 0; i < n; ++i )
    {
        const int i2 = ( i + 1 ) % n;
        for ( int j = i + 1; j < n; ++j )
        {
            const int j2 = ( j + 1 ) % n;
            if ( i == j || i2 == j || i == j2 )
                continue;                    // adjacent segments share a point by design
            if ( SegCross( Pt( pts, i ), Pt( pts, i2 ), Pt( pts, j ), Pt( pts, j2 ) ) )
                return true;
        }
    }
    return false;
}

bool KiwiRegion_IsConvex( const std::vector<float> &pts )
{
    const int n = PtCount( pts );
    if ( n < 3 )
        return false;
    int sign = 0;
    for ( int i = 0; i < n; ++i )
    {
        const float c = Cross2( Pt( pts, i ), Pt( pts, ( i + 1 ) % n ), Pt( pts, ( i + 2 ) % n ) );
        // Tolerate collinear turns from tessellation and grid snapping.
        if ( fabsf( c ) < 1.0e-4f )
            continue;
        const int s = ( c > 0.0f ) ? 1 : -1;
        if ( sign == 0 )      sign = s;
        else if ( sign != s ) return false;
    }
    return true;
}

bool KiwiRegion_Triangulate( const std::vector<float> &pts, std::vector<int> *outTris )
{
    if ( !outTris )
        return false;
    outTris->clear();

    const int n = PtCount( pts );
    if ( n < 3 )
        return false;
    if ( KiwiRegion_SelfIntersects( pts ) )
        return false;

    std::vector<int> poly;
    poly.reserve( n );
    for ( int i = 0; i < n; ++i )
        poly.push_back( i );

    // Bound ear-clipping scans so pathological input cannot stall the editor.
    int guard = n * n + 8;
    while ( (int)poly.size() > 3 && guard-- > 0 )
    {
        bool clipped = false;
        const int m = (int)poly.size();
        for ( int i = 0; i < m; ++i )
        {
            const int ia = poly[( i + m - 1 ) % m];
            const int ib = poly[i];
            const int ic = poly[( i + 1 ) % m];
            const float *a = Pt( pts, ia );
            const float *b = Pt( pts, ib );
            const float *c = Pt( pts, ic );

            const float cross = Cross2( a, b, c );
            if ( cross < 0.0f )
                continue;                    // reflex on a CCW loop — not an ear

            if ( fabsf( cross ) < 1.0e-5f )
            {
                // Drop a collinear zero-area ear without emitting a triangle.
                poly.erase( poly.begin() + i );
                clipped = true;
                break;
            }

            bool contains = false;
            for ( int k = 0; k < m && !contains; ++k )
            {
                const int ik = poly[k];
                if ( ik == ia || ik == ib || ik == ic )
                    continue;
                contains = PointInTriangle( Pt( pts, ik ), a, b, c );
            }
            if ( contains )
                continue;

            outTris->push_back( ia );
            outTris->push_back( ib );
            outTris->push_back( ic );
            poly.erase( poly.begin() + i );
            clipped = true;
            break;
        }
        if ( !clipped )
            return false;                    // no ear anywhere: the loop is not simple
    }

    if ( poly.size() == 3 )
    {
        outTris->push_back( poly[0] );
        outTris->push_back( poly[1] );
        outTris->push_back( poly[2] );
    }
    return !outTris->empty();
}

namespace
{
    // Is the merged polygon (CCW index ring) still convex?
    bool RingIsConvex( const std::vector<float> &pts, const std::vector<int> &ring )
    {
        std::vector<float> flat;
        flat.reserve( ring.size() * 2 );
        for ( size_t i = 0; i < ring.size(); ++i )
        {
            flat.push_back( Pt( pts, ring[i] )[0] );
            flat.push_back( Pt( pts, ring[i] )[1] );
        }
        return KiwiRegion_IsConvex( flat );
    }

    // Merge `b` into `a` across the shared edge (s0,s1), producing the CCW ring of
    // the union.  False when they do not in fact share that edge.
    bool MergeAcross( const std::vector<int> &a, const std::vector<int> &b,
                      int s0, int s1, std::vector<int> *out )
    {
        // Find s0→s1 in `a` (it runs one way in a and the other way in b).
        int ai = -1;
        for ( size_t i = 0; i < a.size(); ++i )
            if ( a[i] == s0 && a[( i + 1 ) % a.size()] == s1 )
                ai = (int)i;
        int bi = -1;
        for ( size_t i = 0; i < b.size(); ++i )
            if ( b[i] == s1 && b[( i + 1 ) % b.size()] == s0 )
                bi = (int)i;
        if ( ai < 0 || bi < 0 )
            return false;

        out->clear();
        // a: start at s1 and run the whole way round, ending on s0.
        for ( size_t k = 0; k < a.size(); ++k )
            out->push_back( a[( ai + 1 + k ) % a.size()] );
        // Continue through b after s0, omitting the shared pair.
        for ( size_t k = 2; k < b.size(); ++k )
            out->push_back( b[( bi + k ) % b.size()] );
        if ( out->size() < 3 )
            return false;
        // Reject repeated vertices from multi-edge adjacency; they pinch the union.
        for ( size_t i = 0; i < out->size(); ++i )
            for ( size_t j = i + 1; j < out->size(); ++j )
                if ( ( *out )[i] == ( *out )[j] )
                    return false;
        return true;
    }
}

bool KiwiRegion_ConvexPieces( const std::vector<float> &pts,
                              std::vector< std::vector<int> > *outPieces )
{
    if ( !outPieces )
        return false;
    outPieces->clear();

    std::vector<int> tris;
    if ( !KiwiRegion_Triangulate( pts, &tris ) )
        return false;

    std::vector< std::vector<int> > pieces;
    for ( size_t t = 0; t + 2 < tris.size(); t += 3 )
    {
        std::vector<int> p;
        p.push_back( tris[t + 0] );
        p.push_back( tris[t + 1] );
        p.push_back( tris[t + 2] );
        pieces.push_back( p );
    }

    // Hertel-Mehlhorn greedily removes diagonals whose adjacent pieces remain convex.
    // The bounded profiles make the O(pieces²) passes acceptable.
    bool merged = true;
    int  guard  = (int)pieces.size() * (int)pieces.size() + 8;
    while ( merged && guard-- > 0 )
    {
        merged = false;
        for ( size_t i = 0; i < pieces.size() && !merged; ++i )
        {
            for ( size_t j = i + 1; j < pieces.size() && !merged; ++j )
            {
                const std::vector<int> &A = pieces[i];
                const std::vector<int> &B = pieces[j];
                for ( size_t ea = 0; ea < A.size() && !merged; ++ea )
                {
                    const int s0 = A[ea];
                    const int s1 = A[( ea + 1 ) % A.size()];
                    std::vector<int> ring;
                    if ( !MergeAcross( A, B, s0, s1, &ring ) )
                        continue;
                    if ( !RingIsConvex( pts, ring ) )
                        continue;
                    pieces[i].swap( ring );
                    pieces.erase( pieces.begin() + j );
                    merged = true;
                }
            }
        }
    }

    outPieces->swap( pieces );
    return !outPieces->empty();
}

// Central grid-aware tolerances keep welding and chaining on the same distances.
float KiwiRegion_WeldDist()
{
    const float g = KiwiUnits_GridSpacingWorld();
    float t = ( g > 0.0f ) ? g * 0.25f : KREG_JOIN_DIST;
    if ( t < KREG_JOIN_DIST ) t = KREG_JOIN_DIST;
    if ( t > KREG_TOL_MAX   ) t = KREG_TOL_MAX;
    return t;
}

// Find the shortest nondegenerate edge for the geometry-bounded weld.
// Zero means no edge supplied a bound, so the grid-derived distance wins.
float KiwiRegion_FinestEdge( const float *pts, int count, int stride, bool closed )
{
    if ( !pts || count < 2 || ( stride != 2 && stride != 3 ) )
        return 0.0f;
    float best = 0.0f;
    const int last = closed ? count : ( count - 1 );
    for ( int i = 0; i < last; ++i )
    {
        const float *a = pts + (size_t)i * stride;
        const float *b = pts + (size_t)( ( i + 1 ) % count ) * stride;
        const float dx = b[0] - a[0];
        const float dy = b[1] - a[1];
        const float dz = ( stride == 3 ) ? ( b[2] - a[2] ) : 0.0f;
        const float d  = sqrtf( dx * dx + dy * dy + dz * dz );
        if ( d <= KREG_JOIN_DIST )
            continue;                        // degenerate — the weld's own business
        if ( best <= 0.0f || d < best )
            best = d;
    }
    return best;
}

float KiwiRegion_WeldFor( float finestEdge )
{
    float t = KiwiRegion_WeldDist();
    if ( finestEdge > 0.0f )
    {
        const float bound = finestEdge * KREG_WELD_EDGE_FRAC;
        if ( bound < t )
            t = bound;
    }
    if ( t < KREG_JOIN_DIST ) t = KREG_JOIN_DIST;
    if ( t > KREG_TOL_MAX   ) t = KREG_TOL_MAX;
    return t;
}

float KiwiRegion_PlaneBand()
{
    const float g = KiwiUnits_GridSpacingWorld();
    float t = ( g > 0.0f ) ? g * 0.25f : KCON_PLANE_FIT_DIST;
    if ( t < KCON_PLANE_FIT_DIST ) t = KCON_PLANE_FIT_DIST;
    if ( t > KREG_TOL_MAX        ) t = KREG_TOL_MAX;
    return t;
}

// Report the nearest unwelded visible open endpoint using ChainWalk's distance.
// This is read-only and throttled unless the caller forces a report.
void KiwiRegion_ReportGaps( bool force )
{
    static unsigned s_reportedGen = 0xFFFFFFFFu;
    const unsigned  gen = KiwiCon_Generation();
    if ( !force && gen == s_reportedGen )
        return;
    s_reportedGen = gen;

    const float tol = KiwiRegion_WeldDist();

    // Closed objects have no dangling endpoints to diagnose.
    std::vector<float> ends;
    const int count = KiwiCon_Count();
    for ( int i = 0; i < count; ++i )
    {
        const kconObject_t *o = KiwiCon_At( i );
        if ( !o || o->hidden )
            continue;
        const bool closed = ( o->type == KCON_CIRCLE ) || ( o->type == KCON_RECT ) || o->closed;
        if ( closed )
            continue;
        const int n = KiwiCon_VertCount( *o );
        if ( n < 2 )
            continue;
        float a[3], b[3];
        if ( !KiwiCon_VertWorld( *o, 0, a ) || !KiwiCon_VertWorld( *o, n - 1, b ) )
            continue;
        ends.push_back( a[0] ); ends.push_back( a[1] ); ends.push_back( a[2] );
        ends.push_back( b[0] ); ends.push_back( b[1] ); ends.push_back( b[2] );
    }

    const int m = (int)( ends.size() / 3 );
    if ( m < 2 )
    {
        if ( force )
            Sys_Printf( "Construction: nothing open to close a loop with.\n" );
        return;
    }

    // Find each endpoint's nearest peer; O(m²) runs once per store generation.
    int   nearestAt  = -1;
    float nearestGap = 0.0f;
    int   dangling   = 0;
    for ( int i = 0; i < m; ++i )
    {
        float best = 1.0e30f;
        for ( int j = 0; j < m; ++j )
        {
            if ( j == i )
                continue;
            const float d = Dist3( &ends[(size_t)i * 3], &ends[(size_t)j * 3] );
            if ( d < best )
                best = d;
        }
        if ( best <= tol )
            continue;                        // welded: not a gap
        ++dangling;
        // Prefer the smallest real gap over an isolated endpoint across the map.
        if ( nearestAt < 0 || best < nearestGap )
        {
            nearestAt  = i;
            nearestGap = best;
        }
    }

    if ( nearestAt < 0 )
    {
        if ( force )
            Sys_Printf( "Construction: every open end is joined (weld %.2f) — if a "
                        "region still is not forming, the loop is not PLANAR enough "
                        "(band %.2f).\n", (double)tol, (double)KiwiRegion_PlaneBand() );
        return;
    }

    const float *p = &ends[(size_t)nearestAt * 3];
    Sys_Printf( "Construction: loop gap %.2f at (%.0f %.0f %.0f)%s — %i open end(s) "
                "are not joined.  The weld is %.2f (a quarter of the grid); raise the "
                "grid or move that point closer.\n",
                (double)nearestGap, (double)p[0], (double)p[1], (double)p[2],
                ( nearestGap <= tol * 4.0f ) ? " — nearly closed" : "",
                dangling, (double)tol );
}

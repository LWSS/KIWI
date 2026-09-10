#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Builds a throwaway planar view from construction segments without modifying
// map, selection, or construction data.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_arrange.h"
#include "kiwi_region.h"        // KREG_JOIN_DIST / KREG_MIN_AREA / KREG_MAX_LOOP + SignedArea

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <vector>

extern int Sys_Printf( const char *fmt, ... );

namespace
{
    // Merge cuts by the world-distance tolerance that recognized the intersection.
    const float KARRG_CUT_MERGE = KCON_ISECT_DIST;

    // Bound the grid-aware weld by the shortest input segment so tessellated edges
    // cannot collapse. `world` stores independent segments, not a point chain.
    float ArrangeWeld( const std::vector<float> &world )
    {
        const int n = (int)( world.size() / 6 );
        float finest = 0.0f;
        for ( int i = 0; i < n; ++i )
        {
            const float *w = &world[(size_t)i * 6];
            const float dx = w[3] - w[0], dy = w[4] - w[1], dz = w[5] - w[2];
            const float d  = sqrtf( dx * dx + dy * dy + dz * dz );
            if ( d <= KREG_JOIN_DIST )
                continue;                       // degenerate — not a bound
            if ( finest <= 0.0f || d < finest )
                finest = d;
        }
        return KiwiRegion_WeldFor( finest );
    }

    struct seg2_t
    {
        float a[2], b[2];
    };

    struct edge_t
    {
        int n0, n1;             // node indices
    };

    struct halfEdge_t
    {
        int   from, to;         // node indices
        float angle;            // atan2 of (to - from), for the per-node sort
        int   twin;             // index of the opposite half-edge
        int   next;             // filled by BuildNext()
        bool  used;
    };

    inline float Len2( const float a[2], const float b[2] )
    {
        const float dx = b[0] - a[0], dy = b[1] - a[1];
        return sqrtf( dx * dx + dy * dy );
    }

    // Test original 3D segments so projection cannot invent crossings in a merely
    // near-coplanar group. This matches Trim's closest-point test and tolerance.
    void GatherCuts( const std::vector<float> &world,      // 6 floats per segment
                     std::vector< std::vector<float> > *cuts )
    {
        const int n = (int)( world.size() / 6 );
        cuts->assign( (size_t)n, std::vector<float>() );

        for ( int i = 0; i < n; ++i )
        {
            const float *ai = &world[(size_t)i * 6];
            for ( int j = i + 1; j < n; ++j )
            {
                const float *aj = &world[(size_t)j * 6];
                float ta = 0.0f, tb = 0.0f;
                const float d = KiwiCon_SegSegClosest( ai, ai + 3, aj, aj + 3,
                                                       &ta, &tb, 0 );
                if ( d > KCON_ISECT_DIST )
                    continue;
                // Keep endpoint hits: a T-junction still needs a cut on its mid-span side.
                // SortAndMerge discards the endpoint cut before fragmentation.
                ( *cuts )[(size_t)i].push_back( ta );
                ( *cuts )[(size_t)j].push_back( tb );
            }
        }
    }

    void SortAndMerge( std::vector<float> *t, float lengthWorld )
    {
        if ( t->empty() )
            return;
        // Cut lists are small enough for insertion sort.
        for ( size_t i = 1; i < t->size(); ++i )
        {
            const float v = ( *t )[i];
            size_t k = i;
            while ( k > 0 && ( *t )[k - 1] > v )
            {
                ( *t )[k] = ( *t )[k - 1];
                --k;
            }
            ( *t )[k] = v;
        }
        // Convert the world-distance merge tolerance to a segment parameter.
        const float tol = ( lengthWorld > 1.0e-4f ) ? ( KARRG_CUT_MERGE / lengthWorld ) : 1.0f;
        std::vector<float> out;
        for ( size_t i = 0; i < t->size(); ++i )
        {
            const float v = ( *t )[i];
            if ( v <= tol || v >= 1.0f - tol )
                continue;                       // an END cut: the weld owns it
            if ( !out.empty() && v - out.back() <= tol )
                continue;
            out.push_back( v );
        }
        t->swap( out );
    }

    int NodeFor( std::vector<float> *nodes, const float p[2], float weld )
    {
        const int n = (int)( nodes->size() / 2 );
        for ( int i = 0; i < n; ++i )
        {
            const float dx = ( *nodes )[(size_t)i * 2 + 0] - p[0];
            const float dy = ( *nodes )[(size_t)i * 2 + 1] - p[1];
            if ( sqrtf( dx * dx + dy * dy ) <= weld )
                return i;
        }
        if ( n >= KARRG_MAX_NODES )
            return -1;
        nodes->push_back( p[0] );
        nodes->push_back( p[1] );
        return n;
    }

    // At h=(a->b), take the edge immediately clockwise from twin (b->a).
    // Angle-sorted buckets then trace each face with its interior on the left.
    void BuildNext( std::vector<halfEdge_t> &he, int nodeCount )
    {
        // Flat per-node buckets avoid one vector allocation per node.
        std::vector<int> counts( (size_t)nodeCount, 0 );
        for ( size_t i = 0; i < he.size(); ++i )
            ++counts[(size_t)he[i].from];

        std::vector<int> start( (size_t)nodeCount + 1, 0 );
        for ( int i = 0; i < nodeCount; ++i )
            start[(size_t)i + 1] = start[(size_t)i] + counts[(size_t)i];

        std::vector<int> fill( start.begin(), start.end() - 1 );
        std::vector<int> bucket( he.size(), -1 );
        for ( size_t i = 0; i < he.size(); ++i )
            bucket[(size_t)fill[(size_t)he[i].from]++] = (int)i;

        // Node degrees are small enough for insertion sort.
        for ( int nIdx = 0; nIdx < nodeCount; ++nIdx )
        {
            const int lo = start[(size_t)nIdx];
            const int hi = start[(size_t)nIdx + 1];
            for ( int i = lo + 1; i < hi; ++i )
            {
                const int   v = bucket[(size_t)i];
                const float a = he[(size_t)v].angle;
                int k = i;
                while ( k > lo && he[(size_t)bucket[(size_t)k - 1]].angle > a )
                {
                    bucket[(size_t)k] = bucket[(size_t)k - 1];
                    --k;
                }
                bucket[(size_t)k] = v;
            }
        }

        // Record each half-edge's bucket slot for O(1) twin lookup.
        std::vector<int> slot( he.size(), 0 );
        for ( int nIdx = 0; nIdx < nodeCount; ++nIdx )
            for ( int i = start[(size_t)nIdx]; i < start[(size_t)nIdx + 1]; ++i )
                slot[(size_t)bucket[(size_t)i]] = i;

        for ( size_t i = 0; i < he.size(); ++i )
        {
            const int twin = he[i].twin;
            const int b    = he[i].to;                  // == he[twin].from
            const int lo   = start[(size_t)b];
            const int hi   = start[(size_t)b + 1];
            const int deg  = hi - lo;
            if ( deg <= 0 )
            {
                he[i].next = twin;                      // cannot happen: the twin is
                continue;                               // itself outgoing from b
            }
            const int rel  = slot[(size_t)twin] - lo;
            const int prev = ( rel - 1 + deg ) % deg;   // clockwise neighbour
            he[i].next = bucket[(size_t)( lo + prev )];
        }
    }
}

bool KiwiArrange_Cells( const int *objects, int count, const kconPlane_t &plane,
                        std::vector<karrCell_t> *outCells )
{
    if ( outCells )
        outCells->clear();
    if ( !objects || count <= 0 || !outCells )
        return false;

    // Gather world-space segments.
    std::vector<float> world;                   // 6 floats per segment
    for ( int i = 0; i < count; ++i )
    {
        const kconObject_t *o = KiwiCon_At( objects[i] );
        // Public entry points enforce the store-wide rule that hidden geometry is inert.
        // Spline spans (KIWI 2026-09-10) never become a cell edge either.
        if ( !o || o->hidden || KiwiCon_HasSmooth( *o ) )
            continue;
        const int segs = KiwiCon_SegmentCount( *o );
        for ( int s = 0; s < segs; ++s )
        {
            float a[3], b[3];
            if ( !KiwiCon_SegmentWorld( *o, s, a, b ) )
                break;
            if ( (int)( world.size() / 6 ) >= KARRG_MAX_SEGS )
            {
                Sys_Printf( "Construction: %i+ coplanar segments — the region "
                            "arrangement is capped at %i and this group is skipped.\n",
                            KARRG_MAX_SEGS, KARRG_MAX_SEGS );
                return false;
            }
            for ( int k = 0; k < 3; ++k ) world.push_back( a[k] );
            for ( int k = 0; k < 3; ++k ) world.push_back( b[k] );
        }
    }
    const int nSeg = (int)( world.size() / 6 );
    if ( nSeg < 3 )
        return true;                            // fewer than three segments bound nothing

    // Use one geometry-bounded weld throughout this call.
    const float weld = ArrangeWeld( world );

    // Fragment at crossings.
    std::vector< std::vector<float> > cuts;
    GatherCuts( world, &cuts );

    std::vector<seg2_t> frag;
    for ( int i = 0; i < nSeg; ++i )
    {
        const float *w = &world[(size_t)i * 6];
        float a2[2], b2[2];
        KiwiCon_WorldToPlane( plane, w,     a2 );
        KiwiCon_WorldToPlane( plane, w + 3, b2 );

        const float len = Len2( a2, b2 );
        SortAndMerge( &cuts[(size_t)i], len );

        float prev[2] = { a2[0], a2[1] };
        for ( size_t k = 0; k <= cuts[(size_t)i].size(); ++k )
        {
            float cur[2];
            if ( k == cuts[(size_t)i].size() )
            {
                cur[0] = b2[0];  cur[1] = b2[1];
            }
            else
            {
                const float t = cuts[(size_t)i][k];
                cur[0] = a2[0] + ( b2[0] - a2[0] ) * t;
                cur[1] = a2[1] + ( b2[1] - a2[1] ) * t;
            }
            if ( Len2( prev, cur ) > weld )
            {
                if ( (int)frag.size() >= KARRG_MAX_EDGES )
                {
                    Sys_Printf( "Construction: the region arrangement produced more "
                                "than %i fragments — this group is skipped.\n",
                                KARRG_MAX_EDGES );
                    return false;
                }
                seg2_t f;
                f.a[0] = prev[0];  f.a[1] = prev[1];
                f.b[0] = cur[0];   f.b[1] = cur[1];
                frag.push_back( f );
            }
            prev[0] = cur[0];  prev[1] = cur[1];
        }
    }
    if ( frag.size() < 3 )
        return true;

    // Weld fragments into a deduplicated undirected edge set.
    std::vector<float>  nodes;                  // 2 floats per node
    std::vector<edge_t> edges;
    for ( size_t i = 0; i < frag.size(); ++i )
    {
        const int n0 = NodeFor( &nodes, frag[i].a, weld );
        const int n1 = NodeFor( &nodes, frag[i].b, weld );
        if ( n0 < 0 || n1 < 0 )
        {
            Sys_Printf( "Construction: the region arrangement needs more than %i "
                        "nodes — this group is skipped.\n", KARRG_MAX_NODES );
            return false;
        }
        if ( n0 == n1 )
            continue;                           // welded to a point
        bool dup = false;
        for ( size_t k = 0; k < edges.size() && !dup; ++k )
            dup = ( ( edges[k].n0 == n0 && edges[k].n1 == n1 )
                 || ( edges[k].n0 == n1 && edges[k].n1 == n0 ) );
        if ( dup )
            continue;                           // two lines drawn on top of each other
        edge_t e;
        e.n0 = n0;  e.n1 = n1;
        edges.push_back( e );
    }
    const int nodeCount = (int)( nodes.size() / 2 );
    if ( edges.size() < 3 || nodeCount < 3 )
        return true;

    // Build angularly ordered half-edges and walk faces.
    std::vector<halfEdge_t> he;
    he.reserve( edges.size() * 2 );
    for ( size_t i = 0; i < edges.size(); ++i )
    {
        for ( int dir = 0; dir < 2; ++dir )
        {
            halfEdge_t h;
            h.from  = dir ? edges[i].n1 : edges[i].n0;
            h.to    = dir ? edges[i].n0 : edges[i].n1;
            h.twin  = (int)( i * 2 + ( dir ? 0 : 1 ) );
            h.next  = -1;
            h.used  = false;
            h.angle = atan2f( nodes[(size_t)h.to * 2 + 1] - nodes[(size_t)h.from * 2 + 1],
                              nodes[(size_t)h.to * 2 + 0] - nodes[(size_t)h.from * 2 + 0] );
            he.push_back( h );
        }
    }
    BuildNext( he, nodeCount );

    for ( size_t s = 0; s < he.size(); ++s )
    {
        if ( he[s].used )
            continue;

        karrCell_t cell;
        int  h    = (int)s;
        bool ok   = true;
        // A valid cycle cannot visit more half-edges than the graph contains.
        for ( size_t guard = 0; guard <= he.size(); ++guard )
        {
            if ( he[(size_t)h].used )
            {
                ok = ( h == (int)s );           // closed on the start = a real face
                break;
            }
            he[(size_t)h].used = true;
            cell.pts.push_back( nodes[(size_t)he[(size_t)h].from * 2 + 0] );
            cell.pts.push_back( nodes[(size_t)he[(size_t)h].from * 2 + 1] );
            h = he[(size_t)h].next;
            if ( h < 0 || h >= (int)he.size() )
            {
                ok = false;
                break;
            }
        }
        if ( !ok || (int)( cell.pts.size() / 2 ) < 3 )
            continue;
        if ( (int)( cell.pts.size() / 2 ) > KREG_MAX_LOOP )
        {
            // This cell is rejected before AcceptLoop, so report the cap here.
            Sys_Printf( "Construction: an enclosed cell has %i corners and the "
                        "region cap is %i — it is neither filled nor extrudable.  "
                        "Lower the 'sides' count on its round parts.\n",
                        (int)( cell.pts.size() / 2 ), KREG_MAX_LOOP );
            continue;
        }

        // Interior-left walks make bounded faces positive and outer faces negative.
        // A dangling spur is traversed out and back, contributing zero signed area.
        const float area = KiwiRegion_SignedArea( cell.pts );
        if ( area < KREG_MIN_AREA )
            continue;

        if ( (int)outCells->size() >= KARRG_MAX_CELLS )
        {
            Sys_Printf( "Construction: more than %i enclosed regions on one plane — "
                        "the rest are ignored.\n", KARRG_MAX_CELLS );
            break;
        }
        outCells->push_back( cell );
    }
    return true;
}

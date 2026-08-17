#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_arrange.cpp — ROUND K: the planar arrangement.  See kiwi_arrange.h for the
// user directive, the two Plasticity files this mirrors (with line cites), why the
// existing endpoint passes cannot answer the # case, and every cap.
//
// NEW code.  It touches NO map data and NO store data: it reads the construction
// store through the const accessors and builds a throwaway view.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_arrange.h"
#include "kiwi_region.h"        // KREG_JOIN_DIST / KREG_MIN_AREA / KREG_MAX_LOOP + SignedArea

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <vector>

// ── ported entry points (each verified against its DEFINITION) ────────────
//   win_qe3.cpp:112   int Sys_Printf( const char *fmt, ... )
// (nothing else: this file reads the construction store through its const
//  accessors and never touches map data, the renderer or the selection.)
extern int Sys_Printf( const char *fmt, ... );

namespace
{
    // Two split parameters closer than this along one segment are the SAME cut.
    // Deliberately tighter than the weld tolerance: a parameter is dimensionless
    // and this is applied after multiplying back by the segment length, so it is a
    // world distance and it is KCON_ISECT_DIST — the same number that decided the
    // crossing existed in the first place.
    const float KARRG_CUT_MERGE = KCON_ISECT_DIST;

    // Node welding.  KREG_JOIN_DIST is §8's ONE slop (kiwi_construct.h says so out
    // loud), and using anything else here would mean a loop that welds for the
    // chain walker does not weld for the arrangement.
    // ROUND AF, ITEM 5: …and KREG_JOIN_DIST is a FLOOR now, not the value.  The
    // arrangement welds at exactly the distance the chain walker welds at, which
    // is the whole point of there being one slop — so it asks the same accessor.
    //
    // ── ROUND AG, ITEM 2: AND IT IS BOUNDED BY THE INPUT ────────────────────
    // The grid-scaled weld is the right number for USER-PLACED endpoints and the
    // wrong number for TESSELLATED ones (kiwi_region.h A WELD MAY NEVER EXCEED THE
    // GEOMETRY IT IS WELDING).  At grid 16 the unbounded weld is 4.0, and a
    // radius-32 arc has 3.1-unit edges — so NodeFor welded each arc vertex onto
    // its predecessor, every one of that arc's fragments came back with
    // n0 == n1 and was dropped, and the arc left the arrangement entirely.  The
    // cells it bounded then did not exist, silently.  That was the unfilled arch.
    //
    // So the weld is computed ONCE per call from the actual input — it is no
    // longer a free function, because "the weld" is now a property of the group
    // being arranged rather than of the editor's global state.  It is threaded
    // through as a parameter to the two places that consume it.
    // KIWI-UX (CLEANUP, A-11): deliberately NOT KiwiRegion_FinestEdge.  That
    // helper walks a CHAIN of consecutive points; `world` here is a list of
    // INDEPENDENT segments, six floats per entry with both endpoints inside the
    // entry, so the enumeration is a different question even though the floor,
    // the tie-break and the KiwiRegion_WeldFor tail below are identical.  Folding
    // it in would mean measuring one segment's end against the next segment's
    // start, which is not an edge of anything.
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

    // ── STEP 1: FRAGMENT ────────────────────────────────────────────────────
    // Every mutual crossing of `segs` becomes a cut parameter on BOTH segments —
    // which is precisely PlanarCurveDatabase's "this intersection cuts the curve
    // in two […] then we process the next curve, since it also has been cut in
    // two" (kiwi_arrange.h cites the docstring).
    //
    // The crossing test itself is KiwiCon_SegSegClosest, in 3D, on the ORIGINAL
    // world segments rather than on the projected 2D ones.  That is not a detour:
    // the group is coplanar only to within KCON_PLANE_FIT_DIST, so two segments
    // that are half a unit apart along the normal would cross EXACTLY in the
    // projection and not at all in space, and the projection is the one place the
    // editor could invent a crossing the user cannot see.  Trim asks the question
    // the same way, at the same tolerance.
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
                // A crossing AT an end of either segment is already a node once the
                // endpoints weld, so recording it as a cut costs one degenerate
                // fragment that the weld then collapses.  Recorded anyway: a
                // T-junction (one segment's END landing mid-span on the other) is
                // exactly the case where ONE side needs the cut and the other does
                // not, and testing which is which here would be a second rule.
                ( *cuts )[(size_t)i].push_back( ta );
                ( *cuts )[(size_t)j].push_back( tb );
            }
        }
    }

    void SortAndMerge( std::vector<float> *t, float lengthWorld )
    {
        if ( t->empty() )
            return;
        // Insertion sort: a segment with more than a handful of cuts is already
        // rare, and this keeps the file free of <algorithm> for four lines.
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
        // Merge cuts that land within KARRG_CUT_MERGE of each other IN WORLD UNITS.
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

    // ── STEP 2: the node set ────────────────────────────────────────────────
    // ROUND AG, ITEM 2: `weld` is passed in (ArrangeWeld above) rather than read
    // from the global grid.
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

    // ── STEP 3: the face walk ───────────────────────────────────────────────
    // For half-edge h = (a -> b): at b, find h's TWIN (b -> a) in b's outgoing list
    // sorted by angle, and take the entry BEFORE it, wrapping.  "Before it" in
    // ascending-angle order is the CLOCKWISE neighbour, and taking the clockwise
    // neighbour of the twin at every node is the standard rule that traces each
    // face with its interior on the LEFT.
    //
    // Worked, not asserted — triangle A(0,0) B(1,0) C(0,1):
    //   h = A->B.  At B the outgoing are B->C (135 deg) and B->A (180 deg); the
    //   twin B->A is index 1, so next = index 0 = B->C.
    //   h = B->C.  At C the outgoing are C->A (-90) and C->B (-45); twin C->B is
    //   index 1, so next = index 0 = C->A.
    //   h = C->A.  At A the outgoing are A->B (0) and A->C (90); twin A->C is
    //   index 1, so next = index 0 = A->B — closed.
    // The cycle is A->B->C, whose signed area is POSITIVE, i.e. the bounded cell.
    // The complementary cycle A->C->B comes out negative and is the outer face.
    void BuildNext( std::vector<halfEdge_t> &he, int nodeCount )
    {
        // One angle-sorted outgoing list per node.  Built as a flat bucket array so
        // the whole thing is two allocations rather than `nodeCount` vectors.
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

        // Sort each node's bucket by angle (insertion sort — a node's degree is a
        // handful in every real arrangement, and 4 for a plain crossing).
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

        // Where each half-edge sits inside its own node's bucket, so the twin can
        // be located in O(1) rather than by a scan per step.
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

    // ── gather the group's world segments ───────────────────────────────────
    std::vector<float> world;                   // 6 floats per segment
    for ( int i = 0; i < count; ++i )
    {
        const kconObject_t *o = KiwiCon_At( objects[i] );
        // ROUND X, ITEM 11: belt-and-braces.  kiwi_region.cpp already filters the
        // member list it hands over, but this is a public entry point and "hidden
        // bounds nothing" is a store-wide rule, not a caller's responsibility.
        if ( !o || o->hidden )
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

    // ROUND AG, ITEM 2: ONE weld for this call, bounded by this group's own
    // finest segment.  Everything below asks `weld`; nothing below reads the grid.
    const float weld = ArrangeWeld( world );

    // ── STEP 1: cut parameters, then fragments ──────────────────────────────
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

    // ── STEP 2: weld into a node set, then a deduped undirected edge set ─────
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

    // ── STEP 3: half-edges, the angular order, and the face walk ────────────
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
        // The guard is the half-edge count: a well-formed cycle visits each half-
        // edge at most once, so exceeding it means the `next` links are not a
        // permutation and the walk must be abandoned rather than looped forever.
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
            // ROUND AG, ITEM 2: LOUD.  This was a silent drop and it is half of
            // "the arch has no fill" — an arch cell carries the whole tessellated
            // arc, so it blows a cap that a rectangle never gets near.  The cap
            // itself moved to 2 * KCON_SEGS_MAX (kiwi_region.h); this says so when
            // even that is not enough, because AcceptLoop's own copy of the test
            // is never reached for a cell dropped here.
            Sys_Printf( "Construction: an enclosed cell has %i corners and the "
                        "region cap is %i — it is neither filled nor extrudable.  "
                        "Lower the 'sides' count on its round parts.\n",
                        (int)( cell.pts.size() / 2 ), KREG_MAX_LOOP );
            continue;
        }

        // SIGN IS THE BOUNDED/UNBOUNDED TEST.  The walk rule puts the interior on
        // the left, so a bounded cell is CCW (positive signed area) and the one
        // outer face of each connected component is CW (negative).  A dangling
        // "spur" — a line sticking out past a corner, which is EXACTLY what the
        // user's # picture is full of — is walked out and straight back along the
        // same edge pair, contributing zero area, so it neither creates a cell nor
        // corrupts the one it hangs off.
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

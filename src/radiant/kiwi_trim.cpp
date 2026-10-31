#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Trim operates only on editor construction geometry; it never mutates map data.
// See kiwi_trim.h for fragment semantics, conversion, undo, and binding contracts.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_command.h"
#include "kiwi_construct.h"
#include "kiwi_lines.h"
#include "kiwi_pick.h"
#include "kiwi_ported.h"
#include "kiwi_trim.h"
#include "kiwi_units.h"
#include "kiwi_vec.h"     // Shared Dot3/Sub3 helpers.

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <vector>

namespace
{
    // Red uniquely identifies construction geometry that the click will delete.
    const float KTRIM_COL_SPAN[3] = { 1.00f, 0.22f, 0.18f };

    // AABB rejection keeps normal scans cheap. Exhausting this safety cap invalidates
    // the entire hover rather than using a partial cut list that could widen removal.
    // 40,000 approximates a 200-segment chain against 200 nearby segments.
    const int KTRIM_MAX_PAIRS = 40000;

    // Every store type that can be walked as segments is trimmable. Parametric
    // shapes freeze to their current visible tessellation; the HUD warns first.
    bool Trimmable( const kconObject_t &o )
    {
        if ( KiwiCon_HasSmooth( o ) )                    // spline spans: layout curves only
            return false;
        if ( o.type == KCON_LINE || o.type == KCON_POLYLINE )
            return (int)( o.pts.size() / 3 ) >= 2;
        if ( o.type == KCON_CIRCLE || o.type == KCON_ARC || o.type == KCON_RECT )
            return KiwiCon_VertCount( o ) >= 2;
        return false;
    }

    // True when trimming converts the source to a polyline; used only by the HUD.
    // KIWI (2026-10-31): circles and arcs now leave true arcs (TrimArc below).
    bool IsParametricShape( const kconObject_t &o )
    {
        return o.type == KCON_RECT;
    }

    bool IsRound( const kconObject_t &o )
    {
        return o.type == KCON_CIRCLE || o.type == KCON_ARC;
    }

    // Stored-point segments are user-drawn edges and bound removal. Circle/arc
    // segments are tessellation, so they retain arc-between-crossings behavior.
    bool StoredSegments( const kconObject_t &o )
    {
        return o.type == KCON_LINE || o.type == KCON_POLYLINE || o.type == KCON_RECT;
    }

    // Flattened world points with cumulative arc length. Closed chains append the
    // wrap point so `total` is the perimeter and the complement is contiguous.
    struct chain_t
    {
        std::vector<float> p;        // 3 floats per point
        std::vector<float> cum;      // one per point; cum[0] == 0
        bool               closed = false;

        int   Count() const { return (int)( p.size() / 3 ); }
        float Total() const { return cum.empty() ? 0.0f : cum[cum.size() - 1]; }
    };

    bool BuildChain( const kconObject_t &o, chain_t *c )
    {
        c->p.clear();
        c->cum.clear();
        // Match the store's closed rule: circles and rectangles wrap regardless of
        // the writer-provided `closed` bit.
        c->closed = ( o.type == KCON_CIRCLE ) || ( o.type == KCON_RECT ) || o.closed;
        const int n = KiwiCon_VertCount( o );
        if ( n < 2 )
            return false;
        for ( int i = 0; i < n; ++i )
        {
            float w[3];
            if ( !KiwiCon_VertWorld( o, i, w ) )
                return false;
            c->p.push_back( w[0] );  c->p.push_back( w[1] );  c->p.push_back( w[2] );
        }
        if ( c->closed )                       // append the wrap point explicitly
        {
            c->p.push_back( c->p[0] );  c->p.push_back( c->p[1] );  c->p.push_back( c->p[2] );
        }

        const int m = c->Count();
        c->cum.push_back( 0.0f );
        for ( int i = 0; i + 1 < m; ++i )
        {
            float d[3];
            Sub3( &c->p[( (size_t)i + 1 ) * 3], &c->p[(size_t)i * 3], d );
            c->cum.push_back( c->cum[i] + Len3( d ) );
        }
        return c->Total() > 1.0e-4f;
    }

    // The world point at arc-length parameter `s`.
    void PointAt( const chain_t &c, float s, float out[3] )
    {
        const int m = c.Count();
        if ( s <= 0.0f )              { Copy3( &c.p[0], out ); return; }
        if ( s >= c.Total() )         { Copy3( &c.p[( (size_t)m - 1 ) * 3], out ); return; }
        for ( int i = 0; i + 1 < m; ++i )
        {
            if ( s > c.cum[i + 1] )
                continue;
            const float span = c.cum[i + 1] - c.cum[i];
            const float t    = ( span > 1.0e-6f ) ? ( ( s - c.cum[i] ) / span ) : 0.0f;
            for ( int k = 0; k < 3; ++k )
                out[k] = c.p[(size_t)i * 3 + k]
                       + ( c.p[( (size_t)i + 1 ) * 3 + k] - c.p[(size_t)i * 3 + k] ) * t;
            return;
        }
        Copy3( &c.p[( (size_t)m - 1 ) * 3], out );
    }

    // Include interpolated endpoints and original interior vertices so trimming
    // preserves the chain's corners (Plasticity TrimFactory.ts:65-82).
    void ExtractRange( const chain_t &c, float a, float b, std::vector<float> *out )
    {
        out->clear();
        if ( !( b > a + 1.0e-4f ) )
            return;
        float w[3];
        PointAt( c, a, w );
        out->push_back( w[0] );  out->push_back( w[1] );  out->push_back( w[2] );
        const int m = c.Count();
        for ( int i = 0; i < m; ++i )
        {
            if ( c.cum[i] <= a + 1.0e-4f || c.cum[i] >= b - 1.0e-4f )
                continue;
            out->push_back( c.p[(size_t)i * 3 + 0] );
            out->push_back( c.p[(size_t)i * 3 + 1] );
            out->push_back( c.p[(size_t)i * 3 + 2] );
        }
        PointAt( c, b, w );
        out->push_back( w[0] );  out->push_back( w[1] );  out->push_back( w[2] );
    }

    // Reject sub-tolerance cut artifacts rather than storing them as geometry.
    bool WorthKeeping( const std::vector<float> &pts )
    {
        const int n = (int)( pts.size() / 3 );
        if ( n < 2 )
            return false;
        float len = 0.0f;
        for ( int i = 0; i + 1 < n; ++i )
        {
            float d[3];
            Sub3( &pts[( (size_t)i + 1 ) * 3], &pts[(size_t)i * 3], d );
            len += Len3( d );
        }
        return len > KCON_PLANE_FIT_DIST;
    }

    // AABB rejection for the bounded crossing scan.
    struct box_t { float lo[3], hi[3]; };

    void BoxSeed( box_t *b, const float *p )
    {
        for ( int k = 0; k < 3; ++k ) { b->lo[k] = p[k]; b->hi[k] = p[k]; }
    }
    void BoxAdd( box_t *b, const float *p )
    {
        for ( int k = 0; k < 3; ++k )
        {
            if ( p[k] < b->lo[k] ) b->lo[k] = p[k];
            if ( p[k] > b->hi[k] ) b->hi[k] = p[k];
        }
    }
    void BoxGrow( box_t *b, float r )
    {
        for ( int k = 0; k < 3; ++k ) { b->lo[k] -= r; b->hi[k] += r; }
    }
    bool BoxOverlap( const box_t &a, const box_t &b )
    {
        for ( int k = 0; k < 3; ++k )
            if ( a.hi[k] < b.lo[k] || b.hi[k] < a.lo[k] )
                return false;
        return true;
    }
    void SegBox( box_t *b, const float *p0, const float *p1, float grow )
    {
        BoxSeed( b, p0 );
        BoxAdd ( b, p1 );
        BoxGrow( b, grow );
    }

    // Collect world-space crossings from other objects and nonadjacent segments of
    // this chain. Self/neighbor pairs share endpoints and are not crossings; the
    // first and last segments of a closed chain are neighbors too.
    // False means the pair cap left an incomplete list, which callers must discard
    // because any missing cut could widen the removed span.
    bool GatherCuts( const chain_t &c, int self, std::vector<float> *cuts )
    {
        cuts->clear();
        const int m     = c.Count();
        const int count = KiwiCon_Count();
        const int nseg  = m - 1;            // chain segment k joins point k to k+1
        int budget = KTRIM_MAX_PAIRS;

        // The hovered chain's own box, grown by the crossing tolerance: nothing
        // outside it can possibly meet the chain.
        box_t chainBox;
        BoxSeed( &chainBox, &c.p[0] );
        for ( int k = 1; k < m; ++k )
            BoxAdd( &chainBox, &c.p[(size_t)k * 3] );
        BoxGrow( &chainBox, KCON_ISECT_DIST );

        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            // Hidden construction is inert.
            if ( !o || o->hidden )
                continue;
            const bool isSelf = ( i == self );   // nonadjacent self-crossings count
            // Crossing objects need not themselves be trimmable; only the cut object
            // must be a chain.
            const int segs = KiwiCon_SegmentCount( *o );
            for ( int s = 0; s < segs; ++s )
            {
                float wa[3], wb[3];
                if ( !KiwiCon_SegmentWorld( *o, s, wa, wb ) )
                    break;

                // Reject candidates outside the whole chain before the inner walk.
                box_t segBox;
                SegBox( &segBox, wa, wb, 0.0f );
                if ( !BoxOverlap( chainBox, segBox ) )
                    continue;
                BoxGrow( &segBox, KCON_ISECT_DIST );

                for ( int k = 0; k + 1 < m; ++k )
                {
                    // Shared-endpoint self/neighbor pairs are not crossings.
                    if ( isSelf )
                    {
                        if ( s == k || s == k - 1 || s == k + 1 )
                            continue;
                        if ( c.closed && nseg >= 2
                          && ( ( k == 0 && s == nseg - 1 ) || ( k == nseg - 1 && s == 0 ) ) )
                            continue;
                    }

                    // Pay for closest approach only when this pair's boxes overlap.
                    box_t linkBox;
                    SegBox( &linkBox, &c.p[(size_t)k * 3],
                            &c.p[( (size_t)k + 1 ) * 3], 0.0f );
                    if ( !BoxOverlap( segBox, linkBox ) )
                        continue;

                    if ( --budget <= 0 )
                        return false;             // the valve — see KTRIM_MAX_PAIRS

                    float t = 0.0f;
                    const float d = KiwiCon_SegSegClosest( &c.p[(size_t)k * 3],
                                                           &c.p[( (size_t)k + 1 ) * 3],
                                                           wa, wb, &t, 0, 0 );
                    if ( d > KCON_ISECT_DIST )
                        continue;
                    const float sPar = c.cum[k] + ( c.cum[k + 1] - c.cum[k] ) * t;
                    // Merge coincident corner hits to avoid zero-length spans.
                    bool dup = false;
                    for ( size_t q = 0; q < cuts->size() && !dup; ++q )
                        dup = ( fabsf( (*cuts)[q] - sPar ) <= KCON_ISECT_DIST );
                    if ( !dup )
                        cuts->push_back( sPar );
                }
            }
        }

        for ( size_t a = 0; a + 1 < cuts->size(); ++a )      // insertion sort; tiny
            for ( size_t b = a + 1; b < cuts->size(); ++b )
                if ( (*cuts)[b] < (*cuts)[a] )
                {
                    const float t = (*cuts)[a];  (*cuts)[a] = (*cuts)[b];  (*cuts)[b] = t;
                }
        return true;
    }

    // Hover state and the exact span a click would remove.
    struct hover_t
    {
        bool  valid  = false;
        int   object = -1;
        float lo     = 0.0f;      // span start, arc length
        float hi     = 0.0f;      // span end
        bool  wrap   = false;     // closed object: the span runs hi -> total -> lo
        int   cuts   = 0;         // how many crossings the object has at all
        // True only when a complete scan derives a span covering the whole object.
        bool  whole  = false;
        // HUD-only: span is clamped to a user-drawn stored segment.
        bool  segment = false;
    };

    // Per-object warning latches prevent console spam during mouse movement.
    int s_truncWarnedObj = -1;

    // Separate so each refusal can still be reported in one session.
    int s_loopWarnedObj = -1;

    // Find the nearest trimmable object at the ordinary screen-space edge radius.
    bool HoverAt( int imgX, int imgY, hover_t *out )
    {
        *out = hover_t();
        const float curX = (float)imgX, curY = (float)imgY;

        int   bestObj  = -1;
        float bestDist = 0.0f;
        float bestPar  = 0.0f;
        // Latch bounds with bestPar so they always describe the same segment.
        float bestSegLo = 0.0f;
        float bestSegHi = 0.0f;
        chain_t bestChain;

        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            // Hidden construction is inert.
            if ( !o || o->hidden || !Trimmable( *o ) )
                continue;
            chain_t c;
            if ( !BuildChain( *o, &c ) )
                continue;

            const int m = c.Count();
            for ( int k = 0; k + 1 < m; ++k )
            {
                float ax, ay, bx, by;
                if ( !Pick_WorldToImage( &c.p[(size_t)k * 3], &ax, &ay )
                  || !Pick_WorldToImage( &c.p[( (size_t)k + 1 ) * 3], &bx, &by ) )
                    continue;                    // an end behind the eye — skip whole
                // Shared screen-space segment distance.
                float       t = 0.0f;
                const float d = Pick_SegDist2D( curX, curY, ax, ay, bx, by, &t );
                // Match the construction-line hover radius.
                if ( d > KCON_LINE_PIXELS )
                    continue;
                if ( bestObj >= 0 && d >= bestDist )
                    continue;
                bestObj   = i;
                bestDist  = d;
                bestPar   = c.cum[k] + ( c.cum[k + 1] - c.cum[k] ) * t;
                bestSegLo = c.cum[k];                    // hovered segment start
                bestSegHi = c.cum[k + 1];
                bestChain = c;
            }
        }
        if ( bestObj < 0 )
            return false;

        std::vector<float> cuts;
        if ( !GatherCuts( bestChain, bestObj, &cuts ) )
        {
            // Never derive a span from incomplete cuts; a missing boundary would
            // widen removal. The per-object latch prevents per-frame console spam.
            *out = hover_t();
            if ( s_truncWarnedObj != bestObj )
            {
                s_truncWarnedObj = bestObj;
                Sys_Printf( "Trim: too much geometry near this line to check safely — "
                            "it will not be trimmed.  Move or delete some of it "
                            "and try again.\n" );
            }
            return false;
        }

        out->valid  = true;
        out->object = bestObj;
        out->cuts   = (int)cuts.size();
        const float total = bestChain.Total();

        // Stored-point types are always bounded by the hovered segment; crossings
        // strictly inside may only narrow it. This also protects open chains that
        // return to their start, whose endpoint self-cuts cannot widen the click to
        // the whole object.
        const kconObject_t *hoverObj = KiwiCon_At( bestObj );
        if ( hoverObj && StoredSegments( *hoverObj ) )
        {
            out->wrap    = false;              // a stored edge never wraps the seam
            out->segment = true;               // use stored-edge HUD wording
            out->lo      = bestSegLo;
            out->hi      = bestSegHi;
            for ( size_t i = 0; i < cuts.size(); ++i )
            {
                const float c = cuts[i];
                if ( c <= bestSegLo + 1.0e-4f || c >= bestSegHi - 1.0e-4f )
                    continue;                  // outside this segment: not our business
                if ( c <= bestPar && c > out->lo ) out->lo = c;
                if ( c >  bestPar && c < out->hi ) out->hi = c;
            }
            // Mark whole only when this edge spans the entire object.
            out->whole = ( out->lo <= 1.0e-4f ) && ( out->hi >= total - 1.0e-4f );
            return true;
        }

        // Parametric tessellation is not a user edge: use neighboring crossings,
        // while a complete zero-cut scan marks the whole object as a stray.
        if ( cuts.empty() )
        {
            out->wrap  = false;
            out->lo    = 0.0f;
            out->hi    = total;
            out->whole = true;
            return true;
        }

        if ( !bestChain.closed )
        {
            // Open arcs use nearest cuts, falling back to their own endpoints.
            out->lo = 0.0f;
            out->hi = total;
            for ( size_t i = 0; i < cuts.size(); ++i )
            {
                if ( cuts[i] <= bestPar && cuts[i] > out->lo ) out->lo = cuts[i];
                if ( cuts[i] >  bestPar && cuts[i] < out->hi ) out->hi = cuts[i];
            }
            return true;
        }

        // A closed parametric loop needs two distinct cuts; one cut gives coincident
        // fragment boundaries and no well-defined removable arc. Zero cuts were
        // handled as a whole-object stray above. Warn once per object.
        if ( cuts.size() < 2 )
        {
            if ( s_loopWarnedObj != bestObj )
            {
                s_loopWarnedObj = bestObj;
                // The empty case returned above, so this is exactly one cut.
                Sys_Printf( "Trim: that is a CLOSED loop and only one line crosses "
                            "it — a loop needs TWO crossings before an arc between "
                            "them exists.  Draw a second line across it (a diameter "
                            "gives you two halves).\n" );
            }
            out->valid = false;
            return false;
        }
        out->wrap = true;
        out->lo   = cuts[cuts.size() - 1];        // default: the wrap interval
        out->hi   = cuts[0];
        for ( size_t i = 0; i + 1 < cuts.size(); ++i )
        {
            if ( bestPar >= cuts[i] && bestPar <= cuts[i + 1] )
            {
                out->lo   = cuts[i];
                out->hi   = cuts[i + 1];
                out->wrap = false;
                break;
            }
        }
        return true;
    }

    // Prepare every fallible read before pushing undo. KiwiCon_UndoPush also creates
    // a unified-journal ticket with no safe discard operation, so CommitTrim is
    // required to finish once the snapshot exists.
    struct trimPlan_t
    {
        int                object = -1;
        kconPlane_t        seedPlane;            // seed only; KiwiCon_Add refits
        std::vector<float> a, b;                 // the pieces to keep, world points
        bool               keepA = false, keepB = false;
        // KIWI (2026-10-31, user: improve Trim so "half circles and arches" are easier): a
        // circle / arc keeps its pieces as true arcs, not a frozen tessellation.
        bool               round = false;
        kconObject_t       src;                  // the circle / arc the pieces come from
        float              angA[2] = { 0.0f, 0.0f }, angB[2] = { 0.0f, 0.0f };   // degrees
    };

    // The angle (degrees, the curve's own plane) of chain parameter `s`.  A circle's or arc's
    // vertices sit at even angles with equal chords, so this is linear in s: exact at every
    // vertex, within a chord's sag in between.
    float AngleAtParam( const kconObject_t &o, const chain_t &c, float s )
    {
        const float f = ( c.Total() > 0.0f ) ? ( s / c.Total() ) : 0.0f;
        if ( o.type == KCON_CIRCLE )
            return 360.0f * f;
        return o.ang0 + ( o.ang1 - o.ang0 ) * f;
    }

    // A cut is where another object crosses a CHORD.  The arc's end belongs where that
    // object's line meets the circle itself, or the arc stops short of it and the two never
    // close into a face.  `guess` is AngleAtParam's answer; a cutter off the curve's plane,
    // or none near, leaves it as it is.
    float ExactCutAngle( const kconObject_t &o, int self, const float at[3], float guess )
    {
        float best = guess, bestAbs = 360.0f;
        auto consider = [&]( float px, float py ) {   // a candidate point, centre-relative
            const float deg = atan2f( py, px ) / KCON_DEG2RAD;
            float delta = fmodf( deg - guess, 360.0f );
            if ( delta >  180.0f ) delta -= 360.0f;
            if ( delta < -180.0f ) delta += 360.0f;
            if ( fabsf( delta ) < bestAbs )
            {
                bestAbs = fabsf( delta );
                best    = guess + delta;
            }
        };
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *cut = KiwiCon_At( i );
            if ( i == self || !cut || cut->hidden )
                continue;
            const int segs = KiwiCon_SegmentCount( *cut );
            for ( int s = 0; s < segs; ++s )
            {
                float wa[3], wb[3];
                if ( !KiwiCon_SegmentWorld( *cut, s, wa, wb ) )
                    break;
                float t = 0.0f;
                if ( KiwiCon_SegSegClosest( at, at, wa, wb, &t, 0, 0 ) > KCON_ISECT_DIST )
                    continue;
                // The cutter must lie in the curve's plane to meet the circle at all.
                float ra[3], rb[3];
                Sub3( wa, o.plane.origin, ra );
                Sub3( wb, o.plane.origin, rb );
                if ( fabsf( Dot3( ra, o.plane.normal ) ) > KCON_PLANE_FIT_DIST
                  || fabsf( Dot3( rb, o.plane.normal ) ) > KCON_PLANE_FIT_DIST )
                    continue;
                float a[2], b[2];
                KiwiCon_WorldToPlane( o.plane, wa, a );
                KiwiCon_WorldToPlane( o.plane, wb, b );
                const float d[2] = { b[0] - a[0], b[1] - a[1] };
                const float f[2] = { a[0] - o.centre[0], a[1] - o.centre[1] };
                const float qa = d[0] * d[0] + d[1] * d[1];
                const float qb = 2.0f * ( f[0] * d[0] + f[1] * d[1] );
                const float qc = f[0] * f[0] + f[1] * f[1] - o.radius * o.radius;
                const float disc = qb * qb - 4.0f * qa * qc;
                if ( !( qa > 1.0e-8f ) )
                    continue;
                // KIWI (2026-10-31, user: trimming the bottom of a circle whose sides were lines
                // ending ON it broke the arc): a cutter that ends on the circle counts its end ...
                const float ends[2][2] = { { f[0], f[1] }, { f[0] + d[0], f[1] + d[1] } };
                for ( int e = 0; e < 2; ++e )
                    if ( fabsf( sqrtf( ends[e][0] * ends[e][0] + ends[e][1] * ends[e][1] ) - o.radius )
                         <= KCON_ISECT_DIST )
                        consider( ends[e][0], ends[e][1] );
                // ... a line that only touches it (a double root, which float noise turns
                // negative) its touch point, and a crossing must lie on the cutter itself.
                const float tolU = KCON_ISECT_DIST / sqrtf( qa );
                if ( disc < 0.0f )
                {
                    const float u = -qb / ( 2.0f * qa );
                    const float px = f[0] + d[0] * u, py = f[1] + d[1] * u;
                    if ( u >= -tolU && u <= 1.0f + tolU
                      && sqrtf( px * px + py * py ) <= o.radius + KCON_ISECT_DIST )
                        consider( px, py );
                    continue;
                }
                const float root = sqrtf( disc );
                for ( int r = 0; r < 2; ++r )
                {
                    const float u = ( -qb + ( r ? root : -root ) ) / ( 2.0f * qa );
                    if ( u >= -tolU && u <= 1.0f + tolU )
                        consider( f[0] + d[0] * u, f[1] + d[1] * u );
                }
            }
        }
        // Only a correction within a couple of chords is the same crossing - plus the arc a
        // touching line stays within KCON_ISECT_DIST of, where the chord crossings smear out.
        const float chord = 720.0f / (float)( KiwiCon_VertCount( o ) > 1 ? KiwiCon_VertCount( o ) : 2 );
        const float touch = o.radius > KCON_ISECT_DIST
                          ? acosf( 1.0f - KCON_ISECT_DIST / o.radius ) / KCON_DEG2RAD : 180.0f;
        return ( bestAbs <= chord + touch ) ? best : guess;
    }

    // Fill plan's arc pieces from the hovered span; false leaves the polyline plan.
    bool PlanRoundTrim( const kconObject_t &src, int self, const chain_t &c, const hover_t &h,
                        trimPlan_t *plan )
    {
        const float total = c.Total();
        float at[3];
        auto cutAngle = [&]( float s ) {
            PointAt( c, s, at );
            return ExactCutAngle( src, self, at, AngleAtParam( src, c, s ) );
        };
        plan->round = true;
        plan->src   = src;
        if ( src.type == KCON_CIRCLE )
        {
            // The kept arc runs from the span's end round to its start (across the seam
            // unless the span itself wraps).
            const float a0 = cutAngle( h.hi );
            float       a1 = cutAngle( h.lo );
            const float sweep = ( h.wrap ? ( h.lo - h.hi ) : ( total - h.hi + h.lo ) ) / total * 360.0f;
            while ( a1 - a0 < sweep - 180.0f ) a1 += 360.0f;
            while ( a1 - a0 > sweep + 180.0f ) a1 -= 360.0f;
            plan->angA[0] = a0;
            plan->angA[1] = a1;
            plan->keepA   = fabsf( a1 - a0 ) >= 0.5f && fabsf( a1 - a0 ) < 359.5f;
            plan->keepB   = false;
            return true;
        }
        // An arc keeps [start, lo] and [hi, end]; its own ends are exact already.
        plan->angA[0] = src.ang0;
        plan->angA[1] = ( h.lo > 0.0f ) ? cutAngle( h.lo ) : src.ang0;
        plan->angB[0] = ( h.hi < total ) ? cutAngle( h.hi ) : src.ang1;
        plan->angB[1] = src.ang1;
        plan->keepA   = fabsf( plan->angA[1] - plan->angA[0] ) >= 0.5f;
        plan->keepB   = fabsf( plan->angB[1] - plan->angB[0] ) >= 0.5f;
        return true;
    }

    // No mutation.  False = the trim cannot proceed and NOTHING has been touched.
    bool PrepareTrim( const hover_t &h, trimPlan_t *plan )
    {
        const kconObject_t *src = KiwiCon_At( h.object );
        if ( !src )
            return false;
        chain_t c;
        if ( !BuildChain( *src, &c ) )
            return false;

        plan->object    = h.object;
        plan->seedPlane = src->plane;
        const float total = c.Total();

        // Keep the complement of [lo,hi], matching Plasticity Interval.trim.
        if ( c.closed )
        {
            // A ring minus one arc is one open chain; concatenate across the seam
            // when the removed span does not wrap.
            if ( h.wrap )
            {
                ExtractRange( c, h.hi, h.lo, &plan->a );  // the interior remainder
            }
            else
            {
                std::vector<float> tail, head;
                ExtractRange( c, h.hi, total, &tail );
                ExtractRange( c, 0.0f, h.lo, &head );
                plan->a = tail;
                // Skip head's seam point only when a nonempty tail already ends there;
                // otherwise the seam vertex is still required.
                const size_t first = tail.empty() ? 0u : 3u;
                for ( size_t k = first; k + 2 < head.size(); k += 3 )
                {
                    plan->a.push_back( head[k]     );
                    plan->a.push_back( head[k + 1] );
                    plan->a.push_back( head[k + 2] );
                }
            }
        }
        else
        {
            ExtractRange( c, 0.0f, h.lo, &plan->a );
            ExtractRange( c, h.hi, total, &plan->b );
        }

        plan->keepA = WorthKeeping( plan->a );
        plan->keepB = WorthKeeping( plan->b );
        if ( IsRound( *src ) )
            PlanRoundTrim( *src, h.object, c, h, plan );
        return true;
    }

    // After the caller's single undo push, this path is required to complete.
    // Returns the number of remainder objects, including zero for full deletion.
    int CommitTrim( const trimPlan_t &plan )
    {
        KiwiCon_RemoveAt( plan.object );

        int made = 0;
        for ( int pass = 0; pass < 2; ++pass )
        {
            const std::vector<float> &pts = ( pass == 0 ) ? plan.a : plan.b;
            if ( pass == 0 ? !plan.keepA : !plan.keepB )
                continue;
            if ( plan.round )
            {
                // KIWI (2026-10-31): a true arc on the source's own circle.
                const float *ang = ( pass == 0 ) ? plan.angA : plan.angB;
                kconObject_t o;
                o.type      = KCON_ARC;
                o.plane     = plan.src.plane;
                o.closed    = false;
                o.centre[0] = plan.src.centre[0];
                o.centre[1] = plan.src.centre[1];
                o.radius    = plan.src.radius;
                o.ang0      = ang[0];
                o.ang1      = ang[1];
                // A circle's piece keeps the circle's smoothness (its full-circle sample
                // count, which an arc prorates by its sweep); an arc's keeps the arc's.
                o.segs      = ( plan.src.type == KCON_ARC ) ? plan.src.segs : KiwiCon_VertCount( plan.src );
                if ( KiwiCon_Add( o ) >= 0 )
                    ++made;
                continue;
            }
            kconObject_t o;
            // Preserve the semantic LINE type for two-point remainders; store
            // consumers branch on it.
            o.type   = ( (int)( pts.size() / 3 ) == 2 ) ? KCON_LINE : KCON_POLYLINE;
            o.plane  = plan.seedPlane;
            o.pts    = pts;
            o.closed = false;                    // a trimmed chain is never closed
            if ( KiwiCon_Add( o ) >= 0 )
                ++made;
        }
        return made;
    }

    // Multi-click command implementation.
    class KiwiTrimCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Trim"; }
        bool CanExecute() override        { return KiwiTrim_CanTrim(); }

        // Each click is an edit event; RMB/Enter ends the session.
        bool WantsClicks() const override { return true; }
        const char *HudStatus() const override { return m_hud; }

        bool Begin() override
        {
            m_hover = hover_t();
            m_trims = 0;
            s_truncWarnedObj = -1;
            s_loopWarnedObj  = -1;         // independent warning latch
            // Advertise stored-edge versus parametric-arc behavior before hover.
            SetHud( "trim  ·  hover a line: click removes the ONE SEGMENT under the "
                    "cursor (a crossing inside it shortens the piece)  ·  circles "
                    "lose the arc between crossings  ·  RMB/Enter: done" );
            Sys_Printf( "Trim: click a segment to remove it — a triangle loses that "
                        "one edge and stays a chain, a lone line goes whole.  A line "
                        "crossing the segment shortens what goes.  Circles and arcs "
                        "lose the arc between two crossings.  RMB or Enter when "
                        "finished.\n" );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;  (void)snap;
            int cx = 0, cy = 0;
            if ( !KiwiCmd_LastCursor( &cx, &cy ) )
            {
                // Clear stale hover so an invisible span cannot remain clickable.
                m_hover = hover_t();
                g_nUpdateBits |= 1;
                return;
            }
            HoverAt( cx, cy, &m_hover );
            UpdateHud();
            g_nUpdateBits |= 1;
        }

        bool Click() override
        {
            if ( !m_hover.valid )
            {
                // HoverAt already prints the specific one-cut closed-loop refusal.
                Sys_Printf( "Trim: nothing to trim there — no construction line is "
                            "under the cursor.  Hovering one removes the segment "
                            "under the cursor (an arc between crossings on a circle "
                            "or arc).\n" );
                return true;
            }
            // Validate before the per-click snapshot so disappearance creates no
            // empty undo step.
            if ( m_hover.whole )
            {
                const kconObject_t *o = KiwiCon_At( m_hover.object );
                if ( !o )
                {
                    Sys_Printf( "Trim: the object went away under the cursor.\n" );
                    m_hover = hover_t();
                    return true;
                }
                KiwiCon_UndoPush();               // one snapshot per click
                KiwiCon_RemoveAt( m_hover.object );// validated above
                ++m_trims;
                // `whole` means the derived span covers the entire object.
                Sys_Printf( "Trim: stray removed (the piece under the cursor WAS the "
                            "whole object, so all of it went).\n" );
                m_hover = hover_t();
                UpdateHud();
                g_nUpdateBits |= 1;
                return true;                      // keep trimming
            }

            // Plan first, push second, commit third; a pushed journal ticket cannot
            // be safely discarded.
            trimPlan_t plan;
            if ( !PrepareTrim( m_hover, &plan ) )
            {
                Sys_Printf( "Trim: the object went away under the cursor.\n" );
                m_hover = hover_t();
                return true;                      // nothing pushed, nothing changed
            }
            // One snapshot per click; commit is required to complete from here.
            KiwiCon_UndoPush();
            const int made = CommitTrim( plan );
            ++m_trims;
            Sys_Printf( "Trim: span removed (%s).\n",
                        ( made == 0 ) ? "object deleted"
                                      : ( made == 1 ) ? "one piece left" : "split in two" );
            m_hover = hover_t();
            UpdateHud();
            g_nUpdateBits |= 1;
            return true;                          // keep trimming
        }

        void Commit() override
        {
            Sys_Printf( "Trim: %i span%s removed.\n", m_trims, ( m_trims == 1 ) ? "" : "s" );
            m_hover = hover_t();
        }

        void Cancel() override
        {
            // Each click already committed its own snapshot; Esc only ends the
            // session, while Ctrl+Z removes edits one at a time.
            m_hover = hover_t();
            g_nUpdateBits |= 1;
        }

        void DrawWorld() override
        {
            if ( !m_hover.valid )
                return;
            const kconObject_t *o = KiwiCon_At( m_hover.object );
            if ( !o )
                return;
            chain_t c;
            if ( !BuildChain( *o, &c ) )
                return;

            KiwiLines_Color( KTRIM_COL_SPAN[0], KTRIM_COL_SPAN[1], KTRIM_COL_SPAN[2] );
            if ( m_hover.wrap )
            {
                DrawRange( c, m_hover.lo, c.Total() );
                DrawRange( c, 0.0f, m_hover.hi );
            }
            else
            {
                DrawRange( c, m_hover.lo, m_hover.hi );
            }
        }

    private:
        void DrawRange( const chain_t &c, float lo, float hi )
        {
            std::vector<float> pts;
            ExtractRange( c, lo, hi, &pts );
            const int n = (int)( pts.size() / 3 );
            for ( int i = 0; i + 1 < n; ++i )
                if ( !KiwiLines_Add( &pts[(size_t)i * 3], &pts[( (size_t)i + 1 ) * 3] ) )
                    return;
        }

        void UpdateHud()
        {
            if ( !m_hover.valid )
            {
                SetHud( "trim  ·  %i removed  ·  hover a line  ·  RMB/Enter: done", m_trims );
                return;
            }
            // Whole-object highlights say DELETE rather than "span".
            if ( m_hover.whole )
            {
                SetHud( "trim  ·  STRAY (one segment, nothing crosses it)  ·  "
                        "click: DELETE WHOLE  ·  %i removed  ·  RMB/Enter: done", m_trims );
                return;
            }
            // Stored-edge spans say SEGMENT; crossings can only shorten them.
            if ( m_hover.segment )
            {
                char sb[32];
                KiwiUnits_Format( sb, sizeof( sb ), m_hover.hi - m_hover.lo );
                // Rectangles also convert to polylines, so warn before the click.
                const kconObject_t *so = KiwiCon_At( m_hover.object );
                SetHud( "trim  SEGMENT %s  (%i crossing%s on this line)%s  ·  "
                        "click: REMOVE that segment  ·  %i removed  ·  RMB/Enter: done",
                        sb, m_hover.cuts, ( m_hover.cuts == 1 ) ? "" : "s",
                        ( so && IsParametricShape( *so ) ) ? "  ·  becomes a polyline" : "",
                        m_trims );
                return;
            }
            const float span = m_hover.wrap ? 0.0f : ( m_hover.hi - m_hover.lo );
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), span );
            // Warn before freezing parametric tessellation as a polyline.
            const kconObject_t *ho = KiwiCon_At( m_hover.object );
            const bool para = ho && IsParametricShape( *ho );
            SetHud( "trim  span %s  (%i crossing%s)%s  ·  click: REMOVE  ·  RMB/Enter: done",
                    m_hover.wrap ? "wrap" : b,
                    m_hover.cuts, ( m_hover.cuts == 1 ) ? "" : "s",
                    para ? "  ·  becomes a polyline" : "" );
        }

        void SetHud( const char *fmt, ... )
        {
            va_list ap;
            va_start( ap, fmt );
            _vsnprintf( m_hud, sizeof( m_hud ), fmt, ap );
            va_end( ap );
            m_hud[sizeof( m_hud ) - 1] = '\0';
        }

        hover_t m_hover;
        int     m_trims = 0;
        char    m_hud[192] = { 0 };
    };

    KiwiTrimCommand s_trim;
}

// Public surface.
bool KiwiTrim_CanTrim()
{
    const int count = KiwiCon_Count();
    for ( int i = 0; i < count; ++i )
    {
        const kconObject_t *o = KiwiCon_At( i );
        if ( o && !o->hidden && Trimmable( *o ) )   // hidden construction is inert
            return true;
    }
    return false;
}

void KiwiTrim_RegisterCommands()
{
    // Unbound here — this is the CLASSIC-profile row; kiwi_keymap.cpp puts Trim on
    // the bare T in the modern profile, with the ViewTextures displacement.
    Radiant_RegisterCommand( "KiwiTrim", 0, 0, KIWI_CMD_TRIM );
}

KiwiEditorCommand *KiwiTrim_CommandForId( int commandId )
{
    return ( commandId == KIWI_CMD_TRIM ) ? &s_trim : 0;
}

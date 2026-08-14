#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_trim.cpp — SHAKEOUT H: the TRIM command (T).  See kiwi_trim.h for the
// Plasticity source this ports, what is and is not trimmable, and the keymap
// audit behind the bare T.
//
// NEW code over the ported cores.  It touches NO map data at all — the whole file
// works on the construction store, which is editor-only scaffolding.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_command.h"
#include "kiwi_construct.h"
#include "kiwi_lines.h"
#include "kiwi_pick.h"
#include "kiwi_trim.h"
#include "kiwi_units.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <vector>

// ── ported entry points (each verified against its DEFINITION) ─────────────
//   win_qe3.cpp:112        int  Sys_Printf( const char *fmt, ... )
//   engine_stubs.cpp:693   int  g_nUpdateBits = 0;   // 0x25d5a74
//   mainfrm.cpp:1246       bool Radiant_RegisterCommand( const char *name, byte vk,
//                                                        byte mods, int commandId )
extern int  Sys_Printf( const char *fmt, ... );
extern int  g_nUpdateBits;
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

namespace
{
    // §18: the trim highlight is RED, and it is the only red line this layer
    // draws.  "This is about to be deleted" is the one message that must not be
    // confusable with hover cyan, active amber or construction rose.
    const float KTRIM_COL_SPAN[3] = { 1.00f, 0.22f, 0.18f };

    // ── SHAKEOUT H FIX: THE BUDGET CANNOT BE ALLOWED TO CHANGE THE ANSWER ───
    // The first cut of this file spent ONE 512-segment budget across the whole
    // store IN STORE ORDER, and that is the worst possible shape for a budget: a
    // couple of 64-segment circles early in the list exhausted it, later objects
    // were never tested, a real crossing went MISSING, `hi` fell back to `total`
    // and the click deleted a span the user never pointed at.  A budget that makes
    // the tool WRONG is not a budget, it is a bug.
    //
    // Two changes, and the first is what actually pays for the second:
    //
    //   1. AABB REJECTION, at two levels.  A candidate segment whose box does not
    //      overlap the hovered chain's box (grown by KCON_ISECT_DIST) cannot cross
    //      it, and rejecting it costs six compares instead of the whole inner walk
    //      over the chain's own segments.  A per-PAIR box test then does the same
    //      for the inner loop.  In every realistic store this reduces the pair
    //      count to the handful of segments actually near the line, so the cap
    //      below is never approached.
    //   2. THE CAP IS A SAFETY VALVE, NOT A TRUNCATION.  If it is ever hit, the
    //      scan reports TRUNCATED and the hover is thrown away entirely — the tool
    //      shows nothing and refuses the click, with a console line saying why.
    //      So the failure mode is "Trim declines", never "Trim removes the wrong
    //      piece".  Raised to 40000 pair tests, which at ~50 flops each is well
    //      inside one mouse-move's budget and is roughly a 200-point polyline
    //      against 200 nearby segments.
    const int KTRIM_MAX_PAIRS = 40000;

    inline void  Copy3( const float *a, float *o ) { o[0]=a[0]; o[1]=a[1]; o[2]=a[2]; }
    inline void  Sub3 ( const float *a, const float *b, float *o )
    { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
    inline float Dot3 ( const float *a, const float *b )
    { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
    inline float Len3 ( const float *a ) { return sqrtf( Dot3( a, a ) ); }

    // ══════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AF, ITEM 4) — THE TYPE GATE WAS THE WHOLE BLOCKER
    // ══════════════════════════════════════════════════════════════════════════
    // USER DIRECTIVE, verbatim: "Allow the trim tool to chop into completed lines
    // (like a circle to make a half circle by eating half the links)."
    //
    // THE SPAN MACHINERY ALREADY DOES THIS.  Round T built the closed-chain arm in
    // full: BuildChain appends the wrap point so the parameterisation covers the
    // whole ring, GatherCuts excludes the wrap-adjacent segment pair, hover_t::wrap
    // expresses the interval that crosses the seam, SpanAt has a dedicated closed
    // branch that picks the arc between the two crossings the cursor sits between,
    // and PrepareTrim's closed arm walks the complement all the way round.  A
    // closed POLYLINE has trimmed correctly since that round.
    //
    // What a CIRCLE could never get past is THIS PREDICATE, which refused it on
    // TYPE — twice over, since a parametric object also has an empty `pts` and so
    // failed the point count as well.  Shakeout H argued the refusal in
    // kiwi_trim.h ("trimming one means converting it to a polyline, which silently
    // destroys the thing the user drew"), and the directive overrules exactly that:
    // half a circle IS a polyline, there is nothing else for it to be, and refusing
    // to make one does not preserve the circle — it preserves the inability to cut
    // it.
    //
    // So the gate now asks the only question that matters — "can this be walked as
    // a chain of segments" — which KiwiCon_VertCount / KiwiCon_VertWorld answer for
    // every type, parametric ones included.  The conversion is no longer silent:
    // the HUD says the shape will become a polyline BEFORE the click, and the
    // tessellation it is frozen at is the one the object was carrying — which round
    // AF item 7 turned into a number the user chooses, and is why the two items
    // shipped together.
    bool Trimmable( const kconObject_t &o )
    {
        if ( o.type == KCON_LINE || o.type == KCON_POLYLINE )
            return (int)( o.pts.size() / 3 ) >= 2;
        if ( o.type == KCON_CIRCLE || o.type == KCON_ARC || o.type == KCON_RECT )
            return KiwiCon_VertCount( o ) >= 2;
        return false;
    }

    // ROUND AF, ITEM 4: does trimming this object destroy a PARAMETRIC shape?
    // Used for the warning only — the trim itself does not care.
    bool IsParametricShape( const kconObject_t &o )
    {
        return o.type == KCON_CIRCLE || o.type == KCON_ARC || o.type == KCON_RECT;
    }

    // ── the flattened object: world points + cumulative arc length ───────────
    // A CLOSED polyline is flattened with its wrap segment appended, so the
    // parameterisation covers the whole ring and `total` is its perimeter.  That
    // is what lets the closed case below express "keep everything except the span
    // the user clicked" as one contiguous walk.
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
        // ROUND AF, ITEM 4: the CANONICAL closed predicate, the one kiwi_region.cpp
        // (:487/:524/:695), kiwi_construct.cpp (:628) and kiwi_conselect.cpp
        // (:531/:550) all spell.  `o.closed` alone is true for the circle and rect
        // this tool now accepts — both set it at creation — but relying on that is
        // relying on a writer's habit rather than on the store's rule, and this file
        // had no reason to know either way until this round.
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

    // Every point of the chain between parameters `a` and `b`, INCLUDING the two
    // interpolated cuts and every original vertex in between.  This is what keeps
    // a trimmed polyline's interior corners (Plasticity's IntervalWithPoints does
    // the same job for the same reason — TrimFactory.ts:65-82).
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

    // A piece worth keeping: two real points and some length.  Below this it is a
    // rounding artefact of the cut, not geometry.
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

    // ── AABB helpers for the crossing scan's rejection (see KTRIM_MAX_PAIRS) ──
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

    // ── the crossings ───────────────────────────────────────────────────────
    // Every parameter along `c` at which some object's segment comes within
    // KCON_ISECT_DIST.
    //
    // ── ROUND T: **SELF-CROSSINGS COUNT**, AND THAT WAS THE BUG ─────────────
    // USER REPORT, verbatim: "when using the trim tool on an oddly drawn shape,
    // it tries to trim the entire curve instead of just the parts that extend
    // past a colliding line.  Fix this so it works how it does in plasticity."
    //
    // THE FAILING CASE, found: an "oddly drawn shape" is ONE object.  Since round
    // P the line and polyline tools are a single CHAINED curve tool (one
    // while-loop picker), so a shape a user sketches in one go — including the
    // stray tail that overshoots where the outline closed on itself — is a single
    // KCON_POLYLINE.  This function skipped `self` WHOLE, on the argument that "a
    // polyline that crosses itself is a knot".  So for that shape it found ZERO
    // crossings, the open-chain branch below fell back to lo = 0 / hi = total,
    // and the click removed THE ENTIRE CURVE.  Exactly the report.
    //
    // Plasticity does not do that.  TrimFactory feeds its interval solver every
    // intersection the curve has, its own included — `curve.intersect(curve)` is
    // in the same list as the crossings against other curves — and trims the
    // FRAGMENT between neighbouring parameters.
    //
    // So `self` is scanned like anything else, minus the pairs that are not real
    // crossings: a segment against ITSELF, and a segment against either NEIGHBOUR
    // (they share an endpoint by construction, which would seed a cut at every
    // vertex of the chain and cut it into its own segments).  On a closed chain
    // the first and last segments are neighbours too, and the wrap pair is
    // excluded for the same reason.
    //
    // FALSE means the pair cap tripped and the answer would be INCOMPLETE — see
    // KTRIM_MAX_PAIRS.  An incomplete cut list is not a smaller answer, it is a
    // WRONG one (a missing cut silently widens the span a click would remove), so
    // the only honest thing to return is "I could not tell you".
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
            // ROUND X, ITEM 11: hidden is inert — an invisible line must not trim.
            if ( !o || o->hidden )
                continue;
            const bool isSelf = ( i == self );   // ROUND T: no longer skipped
            // Deliberately ALL objects, not only the trimmable ones: a line may be
            // perfectly reasonably trimmed against a circle it runs into.  Only the
            // object being CUT has to be a chain.
            const int segs = KiwiCon_SegmentCount( *o );
            for ( int s = 0; s < segs; ++s )
            {
                float wa[3], wb[3];
                if ( !KiwiCon_SegmentWorld( *o, s, wa, wb ) )
                    break;

                // LEVEL 1: this candidate segment against the whole chain's box.
                // Six compares, and it skips the entire inner walk — which is what
                // makes the circles-early-in-the-store case free instead of fatal.
                box_t segBox;
                SegBox( &segBox, wa, wb, 0.0f );
                if ( !BoxOverlap( chainBox, segBox ) )
                    continue;
                BoxGrow( &segBox, KCON_ISECT_DIST );

                for ( int k = 0; k + 1 < m; ++k )
                {
                    // ROUND T: the SELF adjacency exclusion.  A segment against
                    // itself or against either neighbour shares an endpoint by
                    // construction and is not a crossing; everything else on the
                    // same object is.  On a closed chain the ends are neighbours
                    // too (segment 0 begins where segment nseg-1 finished).
                    if ( isSelf )
                    {
                        if ( s == k || s == k - 1 || s == k + 1 )
                            continue;
                        if ( c.closed && nseg >= 2
                          && ( ( k == 0 && s == nseg - 1 ) || ( k == nseg - 1 && s == 0 ) ) )
                            continue;
                    }

                    // LEVEL 2: the same test per PAIR, so a long chain only pays
                    // the closest-approach solve where the two are actually near.
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
                    // Drop a duplicate: two objects meeting at the same corner, or
                    // one object crossing at a shared vertex, would otherwise seed
                    // two cuts a float apart and produce a zero-length span.
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

    // ── the hover ───────────────────────────────────────────────────────────
    struct hover_t
    {
        bool  valid  = false;
        int   object = -1;
        float lo     = 0.0f;      // span start, arc length
        float hi     = 0.0f;      // span end
        bool  wrap   = false;     // closed object: the span runs hi -> total -> lo
        int   cuts   = 0;         // how many crossings the object has at all
        // ── KIWI-UX (ROUND AQ, ITEM 5): THE STRAY ────────────────────────────
        // USER REPORT, verbatim: "Allow the trim tool to delete whole lines (some
        // become stray lines and it's easy to just mash click with T on)."
        // When the hovered object has NO crossings at all there is no span to
        // remove — round T established that (see the long note in HoverAt) and
        // refused outright, which is the safe answer to "I could not find the
        // boundary" but the wrong answer to "there IS no boundary".  The two are
        // distinguishable: `cuts == 0` from a COMPLETE scan is a stray, whereas an
        // incomplete scan (the KTRIM_MAX_PAIRS cap) still refuses before it gets
        // here, so the destructive fallback round T removed cannot come back.
        // A stray highlights WHOLE and a click deletes the object outright.
        bool  whole  = false;
    };

    // Which object the "too much geometry" refusal was last announced for, so the
    // console line is printed once per object rather than once per frame.  Reset by
    // the command's Begin().
    int s_truncWarnedObj = -1;

    // ROUND AF, ITEM 4: the same latch for the "a loop needs two crossings"
    // refusal.  Separate from the one above so a ring that hits both reasons in
    // one session still gets told about both.
    int s_loopWarnedObj = -1;

    // The nearest trimmable object under the cursor pixel, and the SPAN of it that
    // a click would remove.  Screen-space, at the ordinary edge radius, so what
    // trims is what the user would have clicked in Edge mode.
    bool HoverAt( int imgX, int imgY, hover_t *out )
    {
        *out = hover_t();
        const float curX = (float)imgX, curY = (float)imgY;

        int   bestObj  = -1;
        float bestDist = 0.0f;
        float bestPar  = 0.0f;
        chain_t bestChain;

        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            // ROUND X, ITEM 11: hidden is inert — not hoverable, not trimmable.
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
                const float ex = bx - ax, ey = by - ay;
                const float len2 = ex * ex + ey * ey;
                float t = 0.0f;
                if ( len2 > 1.0e-6f )
                {
                    t = ( ( curX - ax ) * ex + ( curY - ay ) * ey ) / len2;
                    if ( t < 0.0f ) t = 0.0f;
                    if ( t > 1.0f ) t = 1.0f;
                }
                const float dx = ax + ex * t - curX, dy = ay + ey * t - curY;
                const float d  = sqrtf( dx * dx + dy * dy );
                // KIWI-UX (ROUND K): KCON_LINE_PIXELS — Trim aims at the same
                // hairline the click and the hover aim at (kiwi_construct.h).
                if ( d > KCON_LINE_PIXELS )
                    continue;
                if ( bestObj >= 0 && d >= bestDist )
                    continue;
                bestObj   = i;
                bestDist  = d;
                bestPar   = c.cum[k] + ( c.cum[k + 1] - c.cum[k] ) * t;
                bestChain = c;
            }
        }
        if ( bestObj < 0 )
            return false;

        std::vector<float> cuts;
        if ( !GatherCuts( bestChain, bestObj, &cuts ) )
        {
            // SHAKEOUT H FIX: the pair cap tripped, so the cut list is INCOMPLETE
            // and every span derived from it would be too wide.  Refusing is the
            // only safe answer; the console says so rather than leaving the user to
            // discover it by losing geometry.  Latched per OBJECT because this runs
            // on every mouse-move frame and an un-latched print would be a hundred
            // lines a second.
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

        if ( !bestChain.closed )
        {
            // ── ROUND T: NO CROSSINGS IS NOT A REASON TO EAT THE WHOLE CURVE ──
            // USER REPORT: "it tries to trim the entire curve instead of just the
            // parts that extend past a colliding line."
            //
            // Shakeout H made a zero-crossing chain trim to lo = 0 / hi = total,
            // i.e. a click deleted the object, and said so out loud: "Plasticity
            // makes it a no-op (TrimFactory.ts:52).  KIWI deletes, because a line
            // with nothing crossing it has no excess bit to remove."  That reading
            // is wrong in exactly the way the user found: whenever a crossing was
            // MISSED for any reason (and the self-crossing skip above was missing
            // them wholesale), the fallback turned a trim into a delete — the most
            // destructive possible answer to "I could not find the boundary".
            //
            // KIWI now agrees with Plasticity: no crossings, no trim.  The console
            // says why and points at the key that does mean "get rid of this one".
            //
            // ── KIWI-UX (ROUND AQ, ITEM 5): …EXCEPT THAT IT IS NOW A DELETE ──
            // The round-T argument above is about a MISSED crossing being turned
            // into a delete, and every one of its examples is a chain that HAS
            // crossings the scan failed to find.  A chain that genuinely has none
            // is a different object: a stray, left over from trimming everything
            // around it, and the user's answer to it was "select it, press
            // Delete" — three inputs and a mode change for something they are
            // already hovering with the tool that removes geometry.  So the
            // zero-crossing case becomes a WHOLE-OBJECT delete rather than a
            // refusal.  The distinction round T needed is preserved intact,
            // because the INCOMPLETE-scan case (GatherCuts returning false) is
            // refused above this point and never reaches here — a missed crossing
            // still cannot become a delete.
            if ( cuts.empty() )
            {
                out->whole = true;
                out->lo    = 0.0f;
                out->hi    = total;
                return true;
            }

            // OPEN: bounded by the nearest cut below and above, falling back to the
            // object's own ends — which is now only ever ONE end, because the
            // zero-crossing case is refused above.
            out->lo = 0.0f;
            out->hi = total;
            for ( size_t i = 0; i < cuts.size(); ++i )
            {
                if ( cuts[i] <= bestPar && cuts[i] > out->lo ) out->lo = cuts[i];
                if ( cuts[i] >  bestPar && cuts[i] < out->hi ) out->hi = cuts[i];
            }
            return true;
        }

        // CLOSED: the ring needs at least TWO cuts before a span is well defined —
        // with one, "the piece between the crossings" is the whole ring minus a
        // point, which is not a trim, it is an open-the-loop with no bounds.
        //
        // ── KIWI-UX (ROUND AF, ITEM 4): AND IT SAYS SO NOW ───────────────────
        // PLASTICITY REACHES THE SAME ANSWER, by arithmetic rather than by rule.
        // PlanarCurveDatabase.ts:121-123 gives a closed curve no synthetic end
        // points (an OPEN one gets both of its own injected at :110-119) and only
        // appends `crosses.push(crosses[0])` to close the seam.  With ONE crossing
        // that array is `[c0, c0]`, so the emission loop's
        // `if (Math.abs(start - stop) > 10e-6)` (:135) never fires and the curve
        // ends up with ZERO fragments — nothing to pick, i.e. untrimmable.  Two
        // crossings give exactly two fragments, one of which wraps the seam.
        //
        // The difference is that KIWI has a console and can explain itself, and
        // "I hover the circle and nothing lights up" is precisely the failure this
        // item is about.  Throttled to one line per object, the same latch the
        // truncation warning above uses, so sweeping the cursor over a badly
        // crossed ring does not fill the console.
        // KIWI-UX (ROUND AQ, ITEM 5): a closed loop that NOTHING crosses is a stray
        // in exactly the same sense as an open one — a circle drawn and orphaned —
        // so it takes the whole-object delete too.  A loop with EXACTLY ONE
        // crossing is NOT a stray: something does cross it, the span is simply
        // ill-defined, and that keeps the round-AF explanation below.
        if ( cuts.empty() )
        {
            out->whole = true;
            out->wrap  = false;
            out->lo    = 0.0f;
            out->hi    = total;
            return true;
        }
        if ( cuts.size() < 2 )
        {
            if ( s_loopWarnedObj != bestObj )
            {
                s_loopWarnedObj = bestObj;
                Sys_Printf( "Trim: that is a CLOSED loop and %s crosses it — a loop "
                            "needs TWO crossings before an arc between them exists. "
                            " Draw a second line across it (a diameter gives you two "
                            "halves).\n",
                            cuts.empty() ? "nothing" : "only one line" );
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

    // ── the removal, in TWO halves ──────────────────────────────────────────
    // ── SHAKEOUT H FIX: THE STORE-UNDO SNAPSHOT MUST COME AFTER THE ONLY PATH
    //    THAT CAN STILL FAIL ────────────────────────────────────────────────
    // The first cut of this file pushed the snapshot and THEN discovered that the
    // hovered object had gone away, leaving a snapshot of a store nothing had
    // changed — a Ctrl+Z that appears to do nothing.  Its own attempted repair,
    // calling KiwiCon_UndoPop() on failure, was worse: since shakeout I every
    // KiwiCon_UndoPush ALSO mints a ticket in the unified journal (kiwi_undo.h),
    // and UndoPop is driven BY the journal rather than minting anything, so popping
    // here would leave an orphan CONSTRUCTION ticket that a later Ctrl+Z would
    // forward into somebody ELSE's snapshot.
    //
    // So there is no discard and no new store API: the work is split so that
    // NOTHING CAN FAIL AFTER THE PUSH.  PrepareTrim reads the store and computes
    // the two keep-lists without touching anything; CommitTrim does the removal and
    // the adds, and every one of those is unconditional by then (the index was
    // validated, and each piece is a SUB-RANGE of an object that already fitted
    // KCON_MAX_POINTS, so KiwiCon_Add cannot reject it either).
    struct trimPlan_t
    {
        int                object = -1;
        kconPlane_t        seedPlane;            // seed only; KiwiCon_Add refits
        std::vector<float> a, b;                 // the pieces to keep, world points
        bool               keepA = false, keepB = false;
    };

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

        // The KEEP intervals — the complement of [lo,hi], exactly as Plasticity's
        // Interval.trim computes it (src/commands/curve/Interval.ts:6-42).
        if ( c.closed )
        {
            // A ring minus one arc is ONE open chain, walked from the far cut all
            // the way round to the near one.  Built as two ranges and concatenated
            // when the removed span itself wraps.
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
                // SHAKEOUT H FIX: `head`'s first point is the ring's SEAM, and it is
                // a duplicate only because `tail` already ended on it.  When h.hi
                // lands within ExtractRange's own epsilon of `total` the tail comes
                // back EMPTY, and skipping head[0] unconditionally then threw the
                // seam vertex away — a closed square trimmed right at its seam lost
                // a corner.  Skip it only when there is a tail to have ended on.
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
        return true;
    }

    // Mutates.  Returns how many objects the trim left behind (0 = the whole object
    // went, which is still a real change).  The caller has already pushed exactly
    // one store-undo snapshot, and nothing here can decline to run.
    int CommitTrim( const trimPlan_t &plan )
    {
        KiwiCon_RemoveAt( plan.object );

        int made = 0;
        for ( int pass = 0; pass < 2; ++pass )
        {
            const std::vector<float> &pts = ( pass == 0 ) ? plan.a : plan.b;
            if ( pass == 0 ? !plan.keepA : !plan.keepB )
                continue;
            kconObject_t o;
            // A two-point remainder is a LINE again, anything longer a polyline.
            // Naming it honestly matters: the store's own type is what the region
            // walker, Join and the next trim all read.
            o.type   = ( (int)( pts.size() / 3 ) == 2 ) ? KCON_LINE : KCON_POLYLINE;
            o.plane  = plan.seedPlane;
            o.pts    = pts;
            o.closed = false;                    // a trimmed chain is never closed
            if ( KiwiCon_Add( o ) >= 0 )
                ++made;
        }
        return made;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  The command.
    // ═══════════════════════════════════════════════════════════════════════
    class KiwiTrimCommand : public KiwiEditorCommand
    {
    public:
        const char *Name() const override { return "Trim"; }
        bool CanExecute() override        { return KiwiTrim_CanTrim(); }

        // Multi-click, exactly like the drawing tools: a click is an EVENT (one
        // trim), never a commit.  RMB / Enter end the command — the same confirm
        // Plasticity gives trim by re-enqueueing until Escape.
        bool WantsClicks() const override { return true; }
        const char *HudStatus() const override { return m_hud; }

        bool Begin() override
        {
            m_hover = hover_t();
            m_trims = 0;
            s_truncWarnedObj = -1;
            s_loopWarnedObj  = -1;         // ROUND AF, ITEM 4
            // KIWI-UX (ROUND AQ, ITEM 5): the stray delete is advertised here, in
            // the one string the tool shows before anything is hovered.
            SetHud( "trim  ·  hover a line: click removes the span between crossings, "
                    "or the WHOLE object when nothing crosses it  ·  RMB/Enter: done" );
            Sys_Printf( "Trim: click the overhanging piece of a line.  "
                        "RMB or Enter when finished.\n" );
            return true;
        }

        void MouseMove( const pick_result_t &pick, const snap_result_t &snap ) override
        {
            (void)pick;  (void)snap;
            int cx = 0, cy = 0;
            if ( !KiwiCmd_LastCursor( &cx, &cy ) )
            {
                // SHAKEOUT H FIX: no cursor means no hover, and the PREVIOUS hover
                // must not survive it.  Returning early left the red span drawn and
                // m_hover.valid true, so a click — which does not need a cursor —
                // would still trim a span the user could no longer see.
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
                // ROUND T: name the LIKELY reason.  A hover that found an object
                // but no crossing is now refused (HoverAt), and "nothing under the
                // cursor" would be a lie about the commonest case.
                // KIWI-UX (ROUND AQ, ITEM 5): the last sentence used to read "To
                // remove a whole line, select it and press Delete."  Trim does that
                // itself now, so what is left here is genuinely "nothing under the
                // cursor" plus the one remaining refusal (a loop with exactly one
                // crossing), which prints its own line from HoverAt.
                Sys_Printf( "Trim: nothing to trim there — no construction line is "
                            "under the cursor.  Hovering one removes the span between "
                            "its crossings, or the whole object when nothing crosses "
                            "it.\n" );
                return true;
            }
            // ── KIWI-UX (ROUND AQ, ITEM 5): THE STRAY, DELETED WHOLE ────────
            // Same ordering discipline as the trim below — the only thing that can
            // still fail is "the object went away", so that is checked BEFORE the
            // snapshot and nothing is pushed on the failing path.  ONE undo record
            // per click, which is what makes mashing T-click over a field of
            // leftovers undoable one leftover at a time.
            if ( m_hover.whole )
            {
                const kconObject_t *o = KiwiCon_At( m_hover.object );
                if ( !o )
                {
                    Sys_Printf( "Trim: the object went away under the cursor.\n" );
                    m_hover = hover_t();
                    return true;
                }
                KiwiCon_UndoPush();               // kiwi_construct.h:636
                KiwiCon_RemoveAt( m_hover.object );// kiwi_construct.h:549
                ++m_trims;
                Sys_Printf( "Trim: stray removed (nothing crossed it, so the whole "
                            "object went).\n" );
                m_hover = hover_t();
                UpdateHud();
                g_nUpdateBits |= 1;
                return true;                      // keep trimming
            }

            // SHAKEOUT H FIX: plan FIRST (reads only), push SECOND, commit THIRD.
            // See the note on PrepareTrim for why there is no push-then-discard —
            // a discarded push would strand its journal ticket (kiwi_undo.h).
            trimPlan_t plan;
            if ( !PrepareTrim( m_hover, &plan ) )
            {
                Sys_Printf( "Trim: the object went away under the cursor.\n" );
                m_hover = hover_t();
                return true;                      // nothing pushed, nothing changed
            }
            // ONE snapshot per CLICK (kiwi_trim.h UNDO), and by here the trim
            // cannot decline.
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
            // NOT an undo: every click already committed its own snapshot, so Esc
            // means "stop trimming", not "put them all back".  Ctrl+Z is how a user
            // takes one back, one at a time, which is what a per-click snapshot is
            // for in the first place.
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
            // KIWI-UX (ROUND AQ, ITEM 5): a stray says DELETE, not "span", because
            // the highlight covers the whole object and the click is not a trim.
            if ( m_hover.whole )
            {
                SetHud( "trim  ·  STRAY (nothing crosses it)  ·  click: DELETE WHOLE  "
                        "·  %i removed  ·  RMB/Enter: done", m_trims );
                return;
            }
            const float span = m_hover.wrap ? 0.0f : ( m_hover.hi - m_hover.lo );
            char b[32];
            KiwiUnits_Format( b, sizeof( b ), span );
            // ROUND AF, ITEM 4: a parametric shape does not survive the trim — it
            // becomes the polyline it was being drawn as.  Said BEFORE the click,
            // which is the whole difference between a documented conversion and a
            // surprise.
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

// ─── the public surface ──────────────────────────────────────────────────────
bool KiwiTrim_CanTrim()
{
    const int count = KiwiCon_Count();
    for ( int i = 0; i < count; ++i )
    {
        const kconObject_t *o = KiwiCon_At( i );
        if ( o && !o->hidden && Trimmable( *o ) )   // ROUND X, ITEM 11 — hidden is inert
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

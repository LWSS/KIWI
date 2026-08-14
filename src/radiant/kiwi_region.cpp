#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_region.cpp — RADIANT_UX_DESIGN §8 implementation plus the 2D polygon
// toolkit §23's extrusion consumes.  See kiwi_region.h for the loop rules, the
// tolerances and the note on the fill's render path.
//
// NEW code over the ported cores.  The only ported thing it touches is the
// immediate-mode triangle emitter the editor already draws its own translucent
// overlays with.
//
// ── HOW A REGION IS FOUND ───────────────────────────────────────────────────
// THREE passes since ROUND K, in this order, so the cheap and unambiguous cases
// never pay for the expensive one:
//
//   PASS 1  every CLOSED object is a region on its own (rect, closed polyline,
//           circle).  Its tessellated vertices are the loop.
//   PASS 2  the OPEN objects are grouped by plane; inside a group their
//           endpoints are welded into shared nodes and the result is walked as a
//           graph.  A connected component is a region only when EVERY node in it
//           has degree exactly 2 — that is §8's "each endpoint shared by exactly
//           two segments" read strictly, and it is what makes the answer unique.
//           A T-junction (degree 3) rejects its whole component rather than
//           guessing a branch.
//   PASS 3  ROUND K - the PLANAR ARRANGEMENT over the same coplanar group plus
//           any coplanar CLOSED objects: every segment split at every mutual
//           crossing, then a minimal-face walk of the resulting planar graph, and
//           every BOUNDED cell is a region.  This is the pass that answers "lines
//           close off a section even if they extend further" - the # case, which
//           passes 1 and 2 are structurally blind to, because no endpoint touches
//           any other endpoint there.  It runs UNCONDITIONALLY and its duplicates
//           are dropped by DuplicateRegion (world centroid + area).  The algorithm,
//           the Plasticity sources it mirrors and every cap live in kiwi_arrange.h.
//
// Both passes then apply the same acceptance gate: no self-intersection, area
// above KREG_MIN_AREA, at most KREG_MAX_LOOP vertices, and the loop is rewound
// CCW so everything downstream (fill triangulation, extrusion cap winding) can
// assume one orientation.
//
// ── SHAKEOUT F ──────────────────────────────────────────────────────────────
// Three changes, all in service of "a face has to appear when a loop closes, on
// ANY plane, and I have to SEE it happen":
//
//   1. PASS 2's endpoint walk is EXPORTED as KiwiRegion_ChainWalk (kiwi_region.h)
//      so "Join Lines" (kiwi_conselect.cpp) chains by exactly this rule.  The
//      degree audit is now "no node above 2" plus "the walk closed", which is the
//      same condition written so that an OPEN chain is also expressible — Join
//      needs that, a region does not.
//   2. The two-object floor.  PASS 2 used to require THREE graph edges, which
//      silently rejected every loop made of two objects (two arcs, two polylines,
//      a polyline and a line back).  Two is enough; the area gate below is what
//      actually rejects the degenerate cases.
//   3. THE FLASH.  A newly formed region's fill fades from near-white over
//      KREG_FLASH_FRAMES Cam_Draw frames, and one console line announces it.
//      Frame-counted, gated on the region COUNT growing — see CarryFlash.
//
// AUDITED AND FOUND CORRECT (recorded so the next round does not re-check it):
// the plane keying works on ARBITRARY planes.  SamePlane derives the plane
// constant from the origin on every call rather than storing a `d`, so two
// objects whose planes were built from the SAME brush face at DIFFERENT cursor
// points compare EQUAL — same normal, same n·origin.  The chain's loop is
// re-projected through WORLD into the group's plane (AppendObjectPoints), so
// differing u/v bases are handled too, and KiwiRegion_PickAt is a plain
// ray∩plane plus an even-odd test in plane space with no axis assumption
// anywhere.  What WAS broken lived in kiwi_construct.cpp's
// KiwiCon_AutoPlaneForTool — see the note there.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                // camera_s
#include <gfx_d3d/r_gfx.h>          // GfxColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT

#include "kiwi_region.h"
#include "kiwi_arrange.h"           // ROUND K — PASS 3, the planar arrangement
#include "kiwi_construct.h"
#include "kiwi_lines.h"             // ROUND AA — KiwiTris_OrientToEye (TRAP 3)
#include "kiwi_pick.h"
#include "kiwi_validity.h"          // ROUND R — KVALID_PLANE_DOT, the §19 V5 threshold
#include "kiwi_units.h"             // ROUND AF, ITEM 5 — KiwiUnits_GridSpacingWorld

#include <math.h>
#include <vector>           // ROUND AF, ITEM 5 - the gap report scratch buffer
#include <string.h>

// ── ported entry points (verified against their definitions) ────────────────
extern int   Sys_Printf( const char *fmt, ... );                         // win_qe3.cpp
extern int   g_nUpdateBits;                                              // 0x25D5A74 (mainfrm.cpp)
// ROUND R — the fill's eye nudge needs the view normal.  camwnd.cpp:152
// `camera_s *Ed_Camera()`; `camera_s` comes from mainfrm.h, included above.
extern camera_s *Ed_Camera();                                            // camwnd.cpp:152
extern char  Byte4PackPixelColor( float *from, GfxColor *out );          // 0x402ac0
// R_AddCmdSetMaterialColor comes from r_rendercmds.h (declared __cdecl there).
extern void  __cdecl R_AddRenderCmdDrawTris(
                 Material *material, MaterialTechniqueType techType, short indexCount,
                 const uint16_t *indices, short vertexCount,
                 const float ( *xyzw )[4], const float ( *normal )[3], float *color,
                 const float ( *st )[2] );                               // 0x4fd1c0
// KIWI-UX (ROUND AM, ITEM 1): the render-command buffer headroom probe, so the
// fill pass can NAME the silent drop instead of counting past it.  Definition at
// r_rendercmds.cpp (KISAK_RADIANT block immediately above R_GetCommandBuffer).
extern int   __cdecl R_Ed_CmdBufferHeadroom();                           // r_rendercmds.cpp

namespace
{
    std::vector<kregion_t> s_regions;
    unsigned               s_builtFor = 0;        // the KiwiCon_Generation() it was built for
    bool                   s_dirty    = true;

    // §18: regions are "translucent light blue".  Alpha 0.22 sits under the
    // ported 3D-marquee quad's 0.25 so a marquee dragged over a region still
    // reads as the stronger of the two.
    const float KREG_FILL[4] = { 0.45f, 0.70f, 1.00f, 0.22f };
    const float KREG_HILITE[4] = { 0.62f, 0.84f, 1.00f, 0.38f };
    // SHAKEOUT F: what a region fades FROM when it has just formed.  Near-white
    // and much more opaque than either resting colour, so "a face appeared" is
    // unmissable even on a pale wall; the alpha still stays under 1 so the
    // geometry behind it is never hidden outright.
    const float KREG_FLASH[4] = { 0.90f, 0.97f, 1.00f, 0.62f };
    // ROUND K: the SELECTED region.  Near-white and stronger than either resting
    // colour, which is exactly what a SELECTED construction line already looks like
    // (kiwi_construct.cpp KCON_COL_SEL_LINE) — "selected" means one thing to the
    // eye on every kind of construction geometry.  Weaker than KREG_FLASH so a
    // region that forms AND is selected still reads its formation flash first.
    const float KREG_SELECTED[4] = { 0.82f, 0.92f, 1.00f, 0.46f };

    inline float Dot3( const float *a, const float *b )
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

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

    // Do segments (a,b) and (c,d) properly cross?  "Properly" excludes shared
    // endpoints, which is what makes it usable on a closed loop where every
    // consecutive pair touches by construction.
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

    // ── KIWI-UX (ROUND R): DROP REDUNDANT COLLINEAR VERTICES ────────────────
    // USER REPORT, verbatim: "Join: 5 lines -> one CLOSED polyline (5 points)"
    // followed by "Extrude: rejected — duplicate plane", on what the user drew as a
    // SQUARE.  A square drawn out of five lines has one side split in two, so the
    // ring carries a mid-edge vertex — and a mid-edge vertex is exactly what §23's
    // prism builder turns into TWO SIDE FACES SHARING ONE PLANE (both are "through
    // this edge, parallel to the extrusion axis", and the two edges are the same
    // line).  §19's V5 gate then says "duplicate plane", correctly, about geometry
    // that was never wrong — the profile was.
    //
    // The test is the V5 test, run one step earlier and in 2D: two consecutive
    // edges whose UNIT DIRECTIONS agree to within KVALID_PLANE_DOT produce two side
    // planes that V5 will call the same, because they also share the vertex between
    // them and so have the same distance.  Using §19's own constant rather than a
    // second tolerance is the point — a profile this accepts is a profile the
    // validity gate accepts, by construction, instead of by coincidence.
    //
    // Nothing legitimate is eaten.  KVALID_PLANE_DOT is 0.999 = 2.56°, and the
    // densest thing this layer produces is a KCON_SEGS_MAX (64) circle, whose
    // consecutive segments turn 5.6° — more than twice the threshold.  The pass
    // repeats until it makes no change, because removing one vertex can leave its
    // two neighbours collinear (three points along one split edge).  A ring that
    // collapses below 3 points is left to AcceptLoop's own n < 3 refusal.
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

    // ROUND AG, ITEM 2: the shortest edge of a plane-space loop that is still
    // above KREG_JOIN_DIST — the bound the weld may not exceed (kiwi_region.h
    // A WELD MAY NEVER EXCEED THE GEOMETRY IT IS WELDING).  Returns 0 when the
    // loop has no such edge at all, which the accessor reads as "no bound".
    float FinestLoopEdge( const std::vector<float> &pts )
    {
        const int n = PtCount( pts );
        float best = 0.0f;
        for ( int i = 0, j = n - 1; i < n; j = i++ )
        {
            const float du = Pt( pts, i )[0] - Pt( pts, j )[0];
            const float dv = Pt( pts, i )[1] - Pt( pts, j )[1];
            const float d  = sqrtf( du * du + dv * dv );
            if ( d <= KREG_JOIN_DIST )
                continue;                    // degenerate — the weld's own business
            if ( best <= 0.0f || d < best )
                best = d;
        }
        return best;
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AR, ITEM 2) — ONE RING SANITIZER, AND NOTHING MAY SKIP IT
    // ═════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: *"why does this bool diff fail? see the 2 pics.  The
    // 2nd pic fails while the 1st pic works.  Why?  The only different is I joined
    // the polyline on the 2nd one.  When i bool it into the pyramid it fails!"*
    //
    // A JOINED outline reaches this layer as PASS 1 (one closed object); an
    // UNJOINED one reaches it as PASS 2 (a chain) or PASS 3 (an arrangement cell).
    // The STANDING REGION INVARIANT is that those three routes derive the SAME
    // fill, the SAME extrude and therefore the SAME cut — "join is a bookkeeping
    // act, not a geometry act".  That invariant was true only by CONVENTION: all
    // three passes happened to funnel through AcceptLoop, and the two cleaning
    // steps were spelled out inline where a fourth caller could quietly acquire
    // one of them and not the other.
    //
    // It is a FUNCTION now, exported (kiwi_region.h), so that the invariant is a
    // thing the code states rather than a thing a reader has to re-derive.  The
    // three steps, in the order they must run:
    //
    //   1. DEDUPE at the weld, BOUNDED by the loop's own finest edge (round AG,
    //      ITEM 2 — a weld may never exceed the geometry it is welding);
    //   2. DROP COLLINEAR at §19's own KVALID_PLANE_DOT (round R — two consecutive
    //      edges on one line become two identical prism side planes, which is the
    //      "duplicate plane" refusal);
    //   3. …and repeat 1 once more, because dropping a collinear vertex can bring
    //      its two neighbours within the weld of each other.  That third step is
    //      NEW this round and it is the one shape neither pass used to remove.
    //
    // Winding is deliberately NOT part of this: RewindCCW is an ACCEPTANCE
    // decision (it needs the signed area the gate then re-reads), so it stays in
    // AcceptLoop where the other gates are.
    void SanitizeRing( std::vector<float> &pts )
    {
        DedupLoop( pts, KiwiRegion_WeldFor( FinestLoopEdge( pts ) ) );
        DropCollinear( pts );
        DedupLoop( pts, KiwiRegion_WeldFor( FinestLoopEdge( pts ) ) );
    }

    // The one acceptance gate both passes run (kiwi_region.cpp header note).
    bool AcceptLoop( kregion_t &r )
    {
        // Deduped at the JOIN tolerance, not at float epsilon: PASS 2 welds two
        // endpoints that are up to KREG_JOIN_DIST apart into one node, so the
        // stitched loop can carry a sub-tolerance edge that the ear clip would
        // then have to survive.
        //
        // ROUND AF, ITEM 5 made that tolerance grid-scaled.  ROUND AG, ITEM 2
        // BOUNDS it by the loop's own finest edge: the sentence that used to stand
        // here — "no real construction segment is that short (the densest circle
        // this store makes has ~6-unit segments at its minimum radius)" — is FALSE
        // for a LARGE round object, whose edge length is 0.098 * radius and has
        // nothing to do with the grid.  At grid 16 the unbounded weld was 4.0 and
        // it ate a radius-32 arc whole.  See kiwi_region.h for the full report.
        //
        // KIWI-UX (ROUND R): …then the collinear pass, so all THREE region passes
        // get it from one place — PASS 1 (a closed object drawn with a split side),
        // PASS 2 (a chain welded out of several lines) and PASS 3 (the arrangement,
        // whose face walk visits every T-junction node on an edge and so produces
        // collinear runs by construction).  See the note on DropCollinear for why
        // the tolerance is §19's own.
        //
        // KIWI-UX (ROUND AR, ITEM 2): both steps, plus the second dedupe pass, are
        // now SanitizeRing — one function, so a joined outline and an unjoined one
        // cannot be cleaned differently.  See the note on it above.
        SanitizeRing( r.pts );
        const int n = PtCount( r.pts );
        if ( n < 3 )
            return false;
        if ( n > KREG_MAX_LOOP )
        {
            // ROUND AG, ITEM 2: LOUD.  "extrudable if and only if filled" is only
            // a usable invariant if the editor says when it cannot hold it — a
            // silent `continue` here is exactly what made the unfilled arch
            // unanswerable from inside the editor.  Throttled by the store
            // generation like the gap report, so a stuck loop does not spam.
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

    // ── PASS 2's endpoint graph ─────────────────────────────────────────────
    // SHAKEOUT H: nodes are WORLD points and welding is a 3D distance — see the
    // note on KiwiRegion_ChainWalk in kiwi_region.h for why the plane had to go.
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

    // SHAKEOUT H: every tessellated point of `o` within KCON_PLANE_FIT_DIST of
    // `plane`.  Body here; KiwiRegion_ObjectOnPlane below is the exported spelling.
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
            if ( fabsf( Dot3( rel, plane.normal ) ) > KiwiRegion_PlaneBand() )   // ROUND AF, ITEM 5
                return false;
        }
        return true;
    }

    // ROUND AG, ITEM 2: "have we already arranged this plane".  NOT a revival of
    // the deleted KiwiRegion_SamePlane (which decided MEMBERSHIP, and rightly went
    // when membership became a point-vs-plane test).  This asks a bookkeeping
    // question about two planes the code itself produced, and a false negative
    // costs one redundant arrangement whose cells DuplicateRegion then drops —
    // i.e. it cannot produce a wrong region, only a wasted pass.  Sign-agnostic on
    // the normal: a rect and a circle drawn on the same wall from opposite sides
    // are the same plane for this purpose.
    bool SamePlaneApprox( const kconPlane_t &a, const kconPlane_t &b )
    {
        if ( fabsf( Dot3( a.normal, b.normal ) ) < 0.999f )
            return false;
        const float rel[3] = { b.origin[0] - a.origin[0],
                               b.origin[1] - a.origin[1],
                               b.origin[2] - a.origin[2] };
        return fabsf( Dot3( rel, a.normal ) ) <= KiwiRegion_PlaneBand();
    }

    // SHAKEOUT H FIX: an object's two chain ENDS in world space, which is what the
    // "prefer a partner that touches the seed" rule in PASS 2 compares.  Same two
    // vertices KiwiRegion_ChainWalk welds on, so "touches" means there exactly what
    // it means here.
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
              || Dist3( e[i], b ) <= KiwiRegion_WeldDist() )   // ROUND AF, ITEM 5
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

    // ── ROUND K: the PASS-3 duplicate gate ──────────────────────────────────
    // PASS 3 re-finds, as an arrangement cell, every loop PASS 1 and PASS 2 found
    // by their own routes — a drawn rectangle IS a bounded cell of its own four
    // segments.  Two regions in the same place would fill twice (visibly darker),
    // pick ambiguously and extrude twice, so the arrangement's output is matched
    // against everything already emitted.
    //
    // The key is WORLD CENTROID + |AREA|, not the point list: PASS 2 stitches a
    // loop out of whole objects and PASS 3 out of split fragments, so the same
    // quadrilateral legitimately comes back with a different vertex COUNT (a
    // corner that is a T-junction is a vertex for one and not the other).  Centroid
    // within the store's one weld tolerance and area within 1% is "the same face"
    // by any reading; two genuinely different cells that agree on both are two
    // loops drawn on top of each other, where either answer is the same answer.
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
            if ( Dist3( cw, ow ) > KiwiRegion_WeldDist() )      // ROUND AF, ITEM 5
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
            // ROUND U: a HIDDEN object bounds nothing.  kiwi_construct.h argues the
            // ruling in full; the short form is that a region held together by a
            // line nobody can see is a region nobody can fix.
            if ( !o || o->hidden )
                continue;
            const bool closed = ( o->type == KCON_CIRCLE ) || ( o->type == KCON_RECT ) || o->closed;
            if ( !closed )
                continue;

            // SHAKEOUT H: the object's plane is DERIVED now, and a closed polyline
            // that does not fit one is simply not a region — the loop is real, it
            // just does not bound a face.  KiwiCon_ObjectPlane answers both cases
            // (a circle's stored plane, a polyline's cached fit) and says no when
            // there is nothing honest to answer with.
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
            if ( !o || o->hidden )        // ROUND U — hidden is inert
                continue;
            const bool closed = ( o->type == KCON_CIRCLE ) || ( o->type == KCON_RECT ) || o->closed;
            if ( !closed && KiwiCon_VertCount( *o ) >= 2 )
                open.push_back( i );
        }

        // ROUND AG, ITEM 2: every plane PASS 3 has already arranged.  PASS 3b below
        // covers the planes it did not reach; see the note there.
        std::vector<kconPlane_t> arranged;

        std::vector<char> grouped( open.size(), 0 );
        for ( size_t g = 0; g < open.size(); ++g )
        {
            if ( grouped[g] )
                continue;

            // ── SHAKEOUT H: THE GROUP'S PLANE IS FITTED, NOT COPIED ─────────────
            // The seed used to hand over its own stored plane.  A world-space store
            // has none to hand over, and — more to the point — a seed that IS a
            // two-point line never determines a plane at all, which is the commonest
            // case there is (draw four lines round a corner and the first one is a
            // line).  So the seed's points are accumulated and A PARTNER IS ADDED
            // UNTIL THE FIT SUCCEEDS — which partner is the question the FIX note
            // below answers — and from then on membership is the plain
            // point-vs-plane test.
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

            // ── SHAKEOUT H FIX: WHICH PARTNER FIXES THE PLANE MATTERS ──────────
            // When the seed is a straight line it determines no plane on its own, so
            // some OTHER object has to co-determine one — and the greedy version
            // took whichever came first IN STORE ORDER.  A stray line that merely
            // happens to be coplanar with the seed (say, lying in the same floor)
            // could therefore fix the group's plane to the WRONG one and the real
            // loop, sitting on a wall through the same seed line, was then rejected
            // object by object and never became a region.
            //
            // The cheap fix is the rule a user would state: prefer a partner that
            // TOUCHES the seed.  Pass 1 only considers candidates one of whose ends
            // is within the weld tolerance of one of the seed's ends — i.e. a
            // candidate that could actually be the next link of the chain we are
            // looking for.  Pass 2 falls back to any coplanar candidate, so nothing
            // that used to group stops grouping; it just stops going FIRST.
            //
            // This narrows the failure, it does not close it: two lines that both
            // touch the seed end and lie on different planes still resolve by store
            // order.  Logged in RADIANT_KNOWN_ISSUES rather than papered over — the
            // real answer is to seed the plane from the CHAIN the walker finds, which
            // is a re-order of the whole pass and not this round's business.
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
                        // The fit itself is the test — KiwiCon_FitPlane refuses when
                        // any point strays, so a candidate on a different plane is
                        // rejected here rather than poisoning the group.
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

            // With the plane fixed, membership is the plain point-vs-plane test and
            // order no longer matters at all.
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
            // ROUND K: WAS `if ( !havePlane || members.size() < 2 ) continue;`.
            // The member floor moved DOWN to the chain walk, which is the only thing
            // that ever needed it — PASS 3 below can bound a cell out of a single
            // self-crossing polyline, and dropping the whole group here would have
            // taken the arrangement with it.
            if ( !havePlane )
                continue;

            // SHAKEOUT F: the endpoint-graph walk is now KiwiRegion_ChainWalk (see
            // kiwi_region.h) so "Join Lines" chains by the SAME rule a region does.
            // The semantics are unchanged, with ONE deliberate relaxation recorded
            // below at the `steps.size() < 2` gate.
            if ( members.size() >= 2 )
            {
                std::vector<kchainStep_t> steps;
                bool closed = false;
                if ( KiwiRegion_ChainWalk( &members[0], (int)members.size(),
                                           &steps, &closed )
                  && closed
                  // WAS `edges.size() < 3`.  A loop of TWO objects is perfectly real
                  // — two arcs, two polylines, or a polyline and a line back to its
                  // start — and the old floor of three rejected every one of them,
                  // which is exactly the "my loop closed and no face appeared"
                  // report shakeout F was answering.  Nothing unsafe is admitted:
                  // two STRAIGHT segments between the same two nodes are collinear,
                  // so they enclose no area and AcceptLoop's KREG_MIN_AREA gate
                  // drops them.
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

            // ── ROUND K, PASS 3: THE PLANAR ARRANGEMENT ─────────────────────────
            // USER DIRECTIVE: a region must form "whenever lines close off a section
            // even if they extend further".  The passes above are ENDPOINT passes
            // and structurally cannot see a mid-span crossing — the full argument,
            // the Plasticity sources this mirrors and every cap are in
            // kiwi_arrange.h.  It runs UNCONDITIONALLY over the group rather than
            // "only when the chain walk found nothing", and the duplicates that
            // produces are dropped by DuplicateRegion.  Correctness first: making
            // the passes exclusive would mean a store where four lines chain AND a
            // fifth crosses them shows only half its faces.
            //
            // CLOSED objects join the group HERE and nowhere else.  They are not in
            // `open` (PASS 1 owns them) and they cannot chain, but a rectangle with
            // a line drawn across it encloses two cells and the arrangement is the
            // only pass that can say so.  Membership is the same point-vs-plane test
            // the open members passed.
            {
                std::vector<int> arrMembers( members );
                for ( int k = 0; k < count; ++k )
                {
                    const kconObject_t *o = KiwiCon_At( k );
                    if ( !o || o->hidden )    // ROUND U — hidden is inert
                        continue;
                    const bool isClosed = ( o->type == KCON_CIRCLE ) || ( o->type == KCON_RECT )
                                       || o->closed;
                    if ( !isClosed || !ObjectOnPlane( *o, plane ) )
                        continue;
                    arrMembers.push_back( k );
                }
                arranged.push_back( plane );   // ROUND AG, ITEM 2 — see PASS 3b

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

        // ── ROUND AG, ITEM 2, PASS 3b: THE PLANES WITH NO OPEN OBJECT ON THEM ───
        // PASS 3 lives INSIDE the open-object group loop, so a plane that carries
        // only CLOSED objects never reaches the arrangement at all.  Two
        // overlapping rectangles, or a circle sitting inside a rect, enclose real
        // cells and produced NO fill — while the same picture with one stray line
        // across it produced all of them, because the line seeded a group.  That is
        // a second, independent source of "it only does it sometimes", and it is
        // the same defect shape as the first: a pass that is conditional on
        // something the user has no reason to connect it to.
        //
        // The cheapest honest fix is a second sweep, not a restructure of the group
        // loop: for every visible CLOSED object whose own plane no group already
        // arranged, arrange that plane over everything coplanar with it.  Costs
        // nothing when the store has open geometry on every plane (the `arranged`
        // test skips immediately) and it cannot double-emit — DuplicateRegion is
        // the same guard PASS 3 uses.
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
            // One closed object on its own plane IS pass 1's business and pass 1
            // has already had it; the arrangement only earns its cost from two.
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

        // ── SHAKEOUT F: the world centroid every region is keyed by ─────────────
        // Used only by the flash carry below; computed once here so nothing has to
        // re-derive it per frame.
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

    // SHAKEOUT F: carry the flash counters across a re-derive.  A region whose
    // world centroid matches one from the previous build is the SAME region and
    // keeps whatever flash it had left; a region with no match is NEW and gets a
    // full KREG_FLASH_FRAMES.  Centroid matching (rather than index matching) is
    // what makes this survive the re-derive's arbitrary ordering — PASS 1 and
    // PASS 2 both append, so adding one closed object can renumber everything a
    // chain produced.
    //
    // KREG_FLASH_MATCH is generous on purpose: it only has to tell "the same loop"
    // from "a different loop", and two DIFFERENT regions whose centroids are 2
    // units apart are two loops drawn on top of each other, where mis-carrying a
    // flash is invisible.
    const float KREG_FLASH_MATCH = 2.0f;

    void CarryFlash( const std::vector<kregion_t> &prev, bool announce )
    {
        // A region is only ever treated as NEW when the region COUNT actually
        // GREW.  Without that gate a MOVE gesture on construction geometry
        // (kiwi_conselect.h) would re-derive every frame with every centroid in a
        // slightly different place, the match would miss, and the same untouched
        // region would "form" sixty times a second — sixty flashes and sixty
        // console lines.  Count growth is the honest signal: a loop closing is
        // exactly "there is one more region than there was".
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
        // The console line is the half of the feedback that survives the user
        // looking somewhere else at the instant the loop closed.
        if ( announce && fresh > 0 )
            Sys_Printf( "Construction: %i region%s closed (%i total).\n",
                        fresh, ( fresh == 1 ) ? "" : "s", (int)s_regions.size() );
    }

    void EnsureBuilt()
    {
        const unsigned gen = KiwiCon_Generation();
        if ( !s_dirty && gen == s_builtFor )
            return;
        // SHAKEOUT F: the FIRST build of a session must not announce or flash every
        // region a loaded sidecar brought with it — only a build that follows a
        // real store change is a "a loop just closed" event.
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

// ─── the region list ─────────────────────────────────────────────────────────
const std::vector<kregion_t> &KiwiRegion_All()
{
    EnsureBuilt();
    return s_regions;
}

void KiwiRegion_Invalidate()
{
    s_dirty = true;
}

// ─── SHAKEOUT F: the exported coplanarity test + chain walker ────────────────
bool KiwiRegion_ObjectOnPlane( const kconObject_t &o, const kconPlane_t &plane )
{
    return ObjectOnPlane( o, plane );
}

// KIWI-UX (ROUND AR, ITEM 2) — see kiwi_region.h for the invariant this serves.
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

    // ── weld every member's two ENDS into shared nodes ───────────────────────
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

        // Ends already coincident: the object closes on itself and is NOT a chain
        // link.  Dropped BEFORE NodeFor so it cannot leave a stray node behind and
        // fail the degree audit for everyone else.
        if ( Dist3( w0, w1 ) <= KiwiRegion_WeldDist() )         // ROUND AF, ITEM 5
            continue;

        chainEdge_t e;
        e.objIndex = objects[m];
        e.node0    = NodeFor( nodes, w0, KiwiRegion_WeldDist() );   // ROUND AF, ITEM 5
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

// ─── ROUND K: region selection (see kiwi_region.h for why it is a centroid) ──
// ROUND AG, ITEM 1: …and why it is now a LIST of centroids rather than one.
namespace
{
    struct selCentroid_t { float c[3]; };
    std::vector<selCentroid_t> s_selCentroids;

    // Resolve ONE stored centroid to a live region index, or -1.
    // KREG_FLASH_MATCH is the tolerance the flash carry already uses to answer
    // "is this the same region across a re-derive"; asking the question twice with
    // two numbers is how the two answers drift apart.
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

// ROUND AG, ITEM 1: Shift+click.  In if it was out, out if it was in — the same
// grammar Shift+click already has on brushes, faces and construction segments,
// which is the whole point of the directive ("I should be able to shift click
// construction faces").
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
    // The PRIMARY: the first stored centroid that still resolves.  Every existing
    // caller wants "the one region the gesture is about", and for a single
    // selection this is bit-for-bit what it always was.
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

// ─── the translucent fill ────────────────────────────────────────────────────
void KiwiRegion_DrawFills( int highlightIndex )
{
    // NOT the const accessor: the flash counters live in the regions and this is
    // the one call per drawn frame that is allowed to age them (kiwi_region.h).
    EnsureBuilt();
    std::vector<kregion_t> &regions = s_regions;

    // ── KIWI-UX (ROUND AM, ITEM 1) — THE INSTRUMENT THAT NAMES THE GATE ─────
    // Fourth round on "the light blue face never shows", and three single-suspect
    // fixes have now missed.  The whole chain Cam_Draw -> KiwiCon_DrawWorld ->
    // here was walked gate by gate this round and every gate is open in code
    // (the table is in RADIANT_UX_DESIGN §66), so the remaining question can only
    // be answered ON THE USER'S MACHINE — which means the instrument has to be
    // the deliverable.
    //
    // WHAT WAS WRONG WITH THE OLD ONE (D-AL6, and the brief's own complaint):
    // `nDrawn` incremented AFTER R_AddRenderCmdDrawTris, which returns void and
    // drops silently.  It could only ever see derivation-side failures, and it
    // reported nothing for a store with NO regions at all — the one state with
    // zero instrumentation anywhere in the subsystem.
    //
    // WHAT THIS ONE DOES INSTEAD: it counts TRIANGLES SUBMITTED, computed BEFORE
    // the submit call, and it threads a GATE NAME through every early-out so the
    // line it prints says which one closed FIRST.  The two gates that live inside
    // R_AddRenderCmdDrawTris are probed here rather than inferred: the technique
    // through the same stateBitsEntry read camwnd.cpp's Cam_MaterialWritesDepth
    // uses (:589), and the command-buffer headroom through R_Ed_CmdBufferHeadroom
    // (r_rendercmds.cpp).  A healthy frame costs one 0xFF compare, one subtract
    // and a handful of increments, and prints nothing ever.
    const char *gate      = nullptr;              // the FIRST gate that closed
    int         nRegions  = (int)regions.size();
    int         nFills    = 0;                    // regions that survived to a submit
    int         nTrisSub  = 0;                    // triangles handed to the renderer
    if ( nRegions == 0 )
        gate = "DERIVATION (no region was built from the construction store)";

    if ( regions.empty() )
    {
        // The store has objects but nothing closes: say so, once per store change,
        // and hand the user the gap report that already knows how to say WHERE.
        // This path printed NOTHING for four rounds.
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

    // ROUND R: the view normal for the coplanar-fill nudge (kiwi_region.h
    // KREG_FILL_NUDGE).  Read ONCE for the whole pass — it cannot change inside it.
    // The caller is Cam_Draw's tail, so CamWnd_BuildMatrix has already run for this
    // frame and c->vpn is this frame's.
    const camera_s *c = Ed_Camera();

    // Same bracket the ported selected-face fill uses (camwnd.cpp 0x408106):
    // MATERIAL_COLOR neutral so the PER-VERTEX colour drives the draw, and back to
    // white afterwards so no later pass inherits it.
    static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
    R_AddCmdSetMaterialColor( s_neutral );

    // ROUND K: resolved ONCE for the whole pass — KiwiRegion_SelectedIndex walks
    // every region's centroid, and doing it per region would make the fill draw
    // quadratic in the region count for a value that cannot change mid-pass.
    //
    // ROUND AG, ITEM 1: the selection is a SET now, so this resolves the whole set
    // once into a flag array rather than asking KiwiRegion_IsSelected per region
    // (which would reintroduce exactly the quadratic the line above avoids).
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

    // One draw per region.  A region is capped at KREG_MAX_LOOP vertices, so the
    // fan is bounded and these buffers can be exact rather than guarded.
    static float    s_xyzw  [KREG_MAX_LOOP][4];
    static float    s_normal[KREG_MAX_LOOP][3];
    static float    s_st    [KREG_MAX_LOOP][2];
    static float    s_color [KREG_MAX_LOOP];
    static uint16_t s_idx   [( KREG_MAX_LOOP - 2 ) * 3];

    // ── KIWI-UX (ROUND AM, ITEM 1): GATE 4/5 — THE MATERIAL AND ITS TECHNIQUE.
    // R_AddRenderCmdDrawTris' FIRST silent drop: `Material_GetTechnique(handle,
    // techType)` null -> `return` with no diagnostic (r_rendercmds.cpp:2153).
    // Read it here the way camwnd.cpp:589 Cam_MaterialWritesDepth already reads
    // it — stateBitsEntry[tech] == 0xFF is "this material has no such technique",
    // which is what Material_GetTechnique bottoms out on.  Probed ONCE per pass:
    // it cannot change inside one.
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

    // A gate set BEFORE the loop is a pass-level failure: every region is lost to
    // it, so the report must lead with it even though the loop still runs and the
    // triangle count comes out nonzero (R_AddRenderCmdDrawTris takes the geometry
    // and drops it inside).  Latched here so the tail can tell the two apart.
    const bool passGateClosed = ( gate != nullptr );

    // ── KIWI-UX (ROUND AK, ITEM 1): THE PASS SAYS WHEN IT DREW NOTHING ──────
    // Round AG established the rule for this subsystem — "both drops now print" —
    // because a silent `continue` is what made the unfilled arch unanswerable from
    // inside the editor.  The DRAW had no such account, so "the fill never shows"
    // could not be told apart from "the region never derived" without a rebuild.
    // It reports only the ANOMALY (regions exist, none of them reached the
    // renderer) and only once per store generation, so a normal frame costs two
    // increments and a healthy editor never prints at all.
    int nTooFew = 0, nTooMany = 0, nNoTris = 0, nDrawn = 0;

    for ( size_t r = 0; r < regions.size(); ++r )
    {
        kregion_t &reg = regions[r];
        const int n = PtCount( reg.pts );

        // SHAKEOUT F: age the flash HERE, before any of the skip paths below, so a
        // region that cannot be triangulated this frame still ages out instead of
        // staying lit forever.
        const int flash = reg.flash;
        if ( reg.flash > 0 )
        {
            --reg.flash;
            // Keep repainting while the flash runs, otherwise it would freeze on
            // whatever frame the editor last happened to draw.
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

        std::vector<int> tris;
        if ( !KiwiRegion_Triangulate( reg.pts, &tris ) )
        {
            ++nNoTris;
            if ( !gate ) gate = "TRIANGULATE (the ear-clip refused the loop)";
            continue;
        }
        if ( tris.empty() || (int)tris.size() > ( KREG_MAX_LOOP - 2 ) * 3 )
        {
            ++nNoTris;
            if ( !gate ) gate = "TRIANGULATE (empty or over the index cap)";
            continue;
        }

        // ROUND K: the SELECTED region outranks the caller's highlight — a caller
        // passes `highlightIndex` for a transient (the extrude preview's own
        // region), and a selection is a state the user put the editor into.
        float rgba[4];
        memcpy( rgba, selFlags[r]                   ? KREG_SELECTED
                    : ( (int)r == highlightIndex )  ? KREG_HILITE
                                                    : KREG_FILL, sizeof( rgba ) );
        // …then lerp toward the flash colour by however much of the flash is left.
        // A LERP rather than a swap: the fill fades back to its resting blue over
        // the whole KREG_FLASH_FRAMES instead of snapping off, which is what makes
        // it read as "that just happened" rather than as a render glitch.
        if ( flash > 0 )
        {
            const float t = (float)flash / (float)KREG_FLASH_FRAMES;
            for ( int k = 0; k < 4; ++k )
                rgba[k] += ( KREG_FLASH[k] - rgba[k] ) * t;
        }
        GfxColor packed;
        Byte4PackPixelColor( rgba, &packed );
        const float packedAsFloat = *(float *)&packed.packed;   // bit-cast, as the ported batcher does

        // ── KIWI-UX (ROUND AM, ITEM 1) — THE ONE TRANSLUCENT PROBE ──────────
        // USER REPORT, verbatim: "Still no light blue face on extrudable line
        // clusters."  Fourth round.  Rounds AK and AL both moved the fill's
        // NORMAL and both missed, and this round found the report is not alone:
        // item 2 ("hover highlighting when hovering a face in mode 3") and item 4
        // ("gizmo arrows are still not filled in") are the SAME defect on two
        // other shapes — a face's only channel IS a fill (kiwi_hover.cpp's empty
        // `case SEL_FACE:`), and the gizmo heads have had real triangles since
        // round AK.  Three shapes, one call, one bracket.
        //
        // The measurement that names it is r_rendercmds.cpp's own (:1913-1930):
        // under a NEUTRAL MATERIAL_COLOR this editor's tools shaders came back at
        // "~0.32x".  A dim line is still a line; a 0.22-alpha fill at 0.32x is
        // nothing.  kiwi_lines.h TRAP 5 carries the derivation, the risk and why
        // exactly ONE translucent fill takes the override this round — which is
        // what RADIANT_KNOWN_ISSUES round AL asked for in as many words.
        //
        // Per REGION rather than per pass because the colour is per region (the
        // selection state and the formation flash both move it), and one extra
        // RC_SET_MATERIAL_COLOR per region is the same cost the ported line path
        // pays per colour run.
        //
        // ══════════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND AQ, ITEM 2) — THE PROBE CAME BACK, AND IT SAID NO.
        // ══════════════════════════════════════════════════════════════════════
        // The KiwiTris_FillFlatColor( rgba ) call that used to be on this line was
        // round AM's ONE TRANSLUCENT PROBE, taken on the "~0.32x neutral bracket"
        // model (kiwi_lines.h TRAP 5).  The probe has now been run by the user and
        // it FALSIFIES that model twice over:
        //   * the fill is still not visible, so the neutral bracket was not what
        //     was suppressing it; and
        //   * it did not come back as an opaque light-blue slab either, which is
        //     the OTHER outcome TRAP 5 predicted.  It came back as nothing at all.
        // And the control experiment was in the tree the whole time: the BOOLEAN's
        // red operand preview (kiwi_boolean.cpp FillBrush, :449) is confirmed
        // visible across many sessions, and it draws on the NEUTRAL bracket, with
        // per-vertex colour, at alpha 0.22 — the SAME alpha this fill uses
        // (KREG_FILL[3], :116 == KBOOL_DIFF_RGBA[3], kiwi_boolean.cpp:77).  A route
        // that carries a visible red fill at 0.22 cannot be the reason a light-blue
        // one at 0.22 is invisible.
        //
        // So this round stops probing and COPIES THE CONTROL.  The flat override is
        // withdrawn and the pass-level neutral bracket opened at the top of this
        // function (the one the ported selected-face fill uses) now covers every
        // region, which is byte-for-byte the boolean's submission state.  The
        // remaining differences between the two emitters, and the pass-location one
        // this round also removes, are tabulated in RADIANT_UX_DESIGN §69.
        // ══════════════════════════════════════════════════════════════════════

        for ( int i = 0; i < n; ++i )
        {
            float w[3];
            KiwiCon_PlaneToWorld( reg.plane, Pt( reg.pts, i ), w );
            // KIWI-UX (ROUND R): pull the fan toward the eye by KREG_FILL_NUDGE, so
            // a region drawn ON a brush face is not decided pixel-by-pixel against
            // that face's own depth.  See kiwi_region.h for the report and for why
            // this is kiwi_hover.cpp's number.  Along the VIEW normal rather than
            // the region normal: a region seen from BEHIND (its normal pointing
            // away) would otherwise be nudged further under the surface.
            // KIWI-UX (ROUND AA, ITEM 2): the tail of this sentence used to read
            // "and the fill is visible from both sides".  It was NOT — see the
            // KiwiTris_OrientToEye call below.  The nudge argument is unaffected
            // and stands as written.
            s_xyzw[i][0] = w[0] - c->vpn[0] * KREG_FILL_NUDGE;
            s_xyzw[i][1] = w[1] - c->vpn[1] * KREG_FILL_NUDGE;
            s_xyzw[i][2] = w[2] - c->vpn[2] * KREG_FILL_NUDGE;
            s_xyzw[i][3] = 1.0f;
            // ── KIWI-UX (ROUND AK, ITEM 1) — THE FILL'S NORMAL IS THE VIEW'S ──
            // USER REPORT, verbatim: "construction faces that can be extruded from
            // still lack their light blue background on the face.  It worked a few
            // days ago sometimes, but now never shows - not even from the backside."
            //
            // This line used to read `reg.plane.normal[]`, and THAT WAS THE ONLY
            // THING SEPARATING THIS FILL FROM EVERY OTHER FILL IN THE KIWI LAYER.
            // The whole rest of the path was audited end to end first and is sound:
            // KiwiRegion_Triangulate cannot be failing on a region that extrudes
            // (a CONVEX region skips it in kiwi_extrude.cpp:934-939 but cannot fail
            // it either — AcceptLoop:336 already ran the identical self-intersect
            // test, and a convex CCW loop always has an ear; a CONCAVE one reaches
            // it through KiwiRegion_ConvexPieces:1607 and has already proven it
            // succeeds).  EnsureBuilt is not re-entrant here (it commits s_builtFor
            // and s_dirty BEFORE returning, so the nested call from ResolveCentroid
            // short-circuits).  KiwiCon_Generation only moves inside Touch(), never
            // from a draw.  KiwiHover_DrawWorld's MATERIAL_COLOR brackets are
            // balanced and this pass re-seeds its own anyway.  The arrangement's
            // cells arrive CCW (kiwi_arrange.cpp:459-469 keeps only positive area).
            // So the geometry was reaching R_AddRenderCmdDrawTris and not appearing.
            //
            // WHAT THE NORMAL DOES: RB_DrawTriangles_Internal packs it per vertex
            // through R_SetVertex4dWithNormal (rb_backend.cpp:186-217), and the
            // editor's tools techniques are the vertcol_SHADED family
            // (r_rendercmds.cpp:1944-1947 names the resolved technique).  A fill
            // whose normal is locked to its own plane therefore presents the same
            // fixed normal from BOTH sides — which is exactly the reported
            // symptom, and exactly what round AA's KiwiTris_OrientToEye CANNOT fix,
            // because that call reorders INDICES and never touches this array.
            // A plane-locked normal also explains "only sometimes": it made the
            // result a function of which way the sketch's plane happened to face.
            //
            // kiwi_hover.cpp:206-209 wrote the rule down when it hit the same
            // question and answered it the other way: "the fan is a screen-facing
            // decal ... reading face->plane would need the def here for no gain".
            // kiwi_extrude.cpp:213-215 agrees.  This file is now the third.
            //
            // ── KIWI-UX (ROUND AL, ITEM 1) — AND `-vpn` WAS ALSO WRONG ──────
            // USER REPORT, verbatim: "the blue face only shows up at steep
            // angles.  Fix this!"  Round AK's half of the diagnosis was right —
            // the normal is the channel — and its choice of REPLACEMENT was not.
            // `-vpn` makes the shading term a function of the camera's PITCH, so
            // the fill lights up looking down and goes out as the view levels,
            // which is the report word for word, including the part depth cannot
            // explain (the fill is missing where the region hangs off the wall
            // over open air).  The two rounds' evidence together pin the term
            // down to a fixed, roughly world-UP fake light; the whole derivation
            // and why a CONSTANT is the only safe answer is kiwi_lines.h TRAP 4,
            // and the binary's own fill batcher (brush.cpp:5915) writes a fixed
            // world normal for the same reason.
            KiwiTris_FillNormal( s_normal[i] );
            s_st[i][0] = 0.0f;
            s_st[i][1] = 0.0f;
            s_color[i] = packedAsFloat;
        }
        for ( size_t k = 0; k < tris.size(); ++k )
            s_idx[k] = (uint16_t)tris[k];

        // KIWI-UX (ROUND AA, ITEM 2) — THE REPORTED BUG.
        // USER REPORT, verbatim: "the light blue construction lineface isn't
        // rendering unless you're facing the other way (see pics).  Fix this."
        // KiwiRegion_Triangulate's contract is "the input must be CCW"
        // (kiwi_region.h), and the loop is CCW in the REGION PLANE's basis — so
        // the emitted winding is locked to reg.plane and has nothing to do with
        // where the eye is.  White_tools culls back faces (kiwi_lines.h TRAP 3),
        // so exactly one side of every region drew.  Orient the fan at the eye,
        // the same answer round Y gave the cut disc.
        KiwiTris_OrientToEye( &s_xyzw[0][0], 4, s_idx, (int)tris.size(), c->origin );

        // ── KIWI-UX (ROUND AM, ITEM 1): GATE 8 — THE COMMAND BUFFER ─────────
        // R_AddRenderCmdDrawTris' SECOND silent drop.  Its byte cost is the exact
        // arithmetic r_rendercmds.cpp:2156-2163 does: a 16-byte header, then
        // 16+12+4+8 bytes of vertex streams per vertex, then a 2-byte-per-index
        // list rounded up to an even count.  Compared against the same sizeLimit
        // R_GetCommandBuffer will compute, BEFORE the call, so the drop is named
        // instead of inferred.
        {
            const int ic    = (int)tris.size();
            const int bytes = 16 + ( 16 + 12 + 4 + 8 ) * n + 2 * ( ( ic + 1 ) & ~1 );
            if ( R_Ed_CmdBufferHeadroom() < bytes )
            {
                if ( !gate )
                    gate = "CMDBUF (the render command buffer is full — the draw is "
                           "dropped silently inside R_GetCommandBuffer)";
                continue;
            }
        }

        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                (short)tris.size(), s_idx, (short)n,
                                s_xyzw, s_normal, s_color, s_st );
        ++nDrawn;
        ++nFills;
        nTrisSub += (int)tris.size() / 3;      // counted from the SUBMITTED index list
    }

    R_AddCmdSetMaterialColor( s_white );

    // ── KIWI-UX (ROUND AM, ITEM 1): THE ONE LINE ────────────────────────────
    // The condition the brief asked for, exactly: a NONZERO region count coexisting
    // with ZERO submitted triangles.  Throttled on the store generation the same way
    // the loop-cap report at :322 is, so a sketch stuck in this state says so once
    // rather than sixty times a second.
    //
    // WHEN IT PRINTS NOTHING, THAT IS ALSO THE ANSWER, and it is a sharper one than
    // round AK's was: triangles were counted from the SUBMITTED index list and both
    // in-renderer drops were probed BEFORE the call, so silence now means the
    // geometry genuinely reached RB_DrawTriangles_Internal with a technique and room
    // to hold it.  Everything left after that is BLEND STATE or SHADING — the
    // vertcol_shaded fakelight term of kiwi_lines.h TRAP 4, or the alpha the
    // MATERIAL_COLOR probe named in RADIANT_KNOWN_ISSUES round AL.
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
        // Some regions drew and some did not: still worth one line, because a
        // PARTIALLY filled sketch is the state that reads as "it works sometimes".
        static unsigned s_lastPartialGen = 0xFFFFFFFFu;
        if ( s_lastPartialGen != KiwiCon_Generation() )
        {
            s_lastPartialGen = KiwiCon_Generation();
            Sys_Printf( "Construction: %i of %i regions filled (%i triangles) - the rest "
                        "stopped at gate: %s.\n",
                        nFills, nRegions, nTrisSub, gate );
        }
    }
    (void)nDrawn;
}

// ═════════════════════════════════════════════════════════════════════════════
//  The 2D toolkit (§23).  Everything here works on a CCW plane-space loop.
// ═════════════════════════════════════════════════════════════════════════════
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
        // Exactly-collinear turns are TOLERATED: a tessellated arc segment pair or
        // a snapped-to-grid corner routinely produces one, and calling that
        // concave would send perfectly good profiles down the decomposition path.
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

    // Ear clipping.  `guard` bounds the outer loop at the theoretical maximum
    // number of scans (one full sweep per remaining vertex) so a pathological
    // input can stall the algorithm but never the editor.
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
                // A COLLINEAR vertex.  It is a zero-area "ear": drop the vertex
                // without emitting a degenerate triangle.  Handling this here is
                // what lets grid-snapped and tessellated profiles through at all.
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
        // b: continue from the vertex AFTER s0 and stop BEFORE s1 — b[bi] is s1 and
        // b[bi+1] is s0, so the shared pair is exactly what k = 2 .. size-1 skips.
        for ( size_t k = 2; k < b.size(); ++k )
            out->push_back( b[( bi + k ) % b.size()] );
        if ( out->size() < 3 )
            return false;
        // Two pieces that share MORE than the one edge would produce a ring with a
        // repeated vertex — a pinched polygon whose collinear turns the convexity
        // test tolerates.  Reject it outright rather than merge into a shape the
        // extruder cannot turn into planes.
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

    // HERTEL-MEHLHORN: repeatedly remove an INESSENTIAL diagonal — one whose two
    // adjacent pieces merge into a still-convex polygon.  Greedy and O(pieces²)
    // per pass, which is fine at this scale (a 64-vertex profile yields at most
    // 62 triangles) and is the standard formulation; it guarantees at most 4x the
    // optimal piece count.
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

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AF, ITEM 5 — THE GRID-AWARE TOLERANCES AND THE GAP REPORT
// ═════════════════════════════════════════════════════════════════════════════
// The reasoning — including why a quarter of a grid step provably cannot fuse two
// points the user meant to keep apart — is in kiwi_region.h.  These are the two
// numbers themselves, in one place, so nothing in this layer can weld at one
// distance and chain at another.
float KiwiRegion_WeldDist()
{
    const float g = KiwiUnits_GridSpacingWorld();
    float t = ( g > 0.0f ) ? g * 0.25f : KREG_JOIN_DIST;
    if ( t < KREG_JOIN_DIST ) t = KREG_JOIN_DIST;
    if ( t > KREG_TOL_MAX   ) t = KREG_TOL_MAX;
    return t;
}

// ROUND AG, ITEM 2 — see kiwi_region.h A WELD MAY NEVER EXCEED THE GEOMETRY IT IS
// WELDING for the report and the argument.  `finestEdge <= 0` means "the caller
// found no edge above the degenerate floor", i.e. no bound, so the grid wins.
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

// ── 5(c): "loop gap 0.8 at (x y z)" ─────────────────────────────────────────
// Every rejection inside BuildRegions is a silent `continue`, which is why "it
// will not detect my face" has never had an answer the editor itself could give.
// This is that answer: weld every visible OPEN object's two ends at exactly the
// distance the chain walker uses, then report the ends that are still DANGLING
// together with how far the nearest other dangling end is.
//
// A dangling end whose partner sits just past the weld distance IS the bug class
// the user is describing, and the number printed is what has to be closed — either
// by moving the point or by raising the grid, which raises the weld.
//
// It reads the store and prints.  It changes nothing, so it is safe to call from
// anywhere, including a draw path.
void KiwiRegion_ReportGaps( bool force )
{
    static unsigned s_reportedGen = 0xFFFFFFFFu;
    const unsigned  gen = KiwiCon_Generation();
    if ( !force && gen == s_reportedGen )
        return;
    s_reportedGen = gen;

    const float tol = KiwiRegion_WeldDist();

    // Collect every OPEN object's two ends.  A closed shape has nothing dangling by
    // definition, and is the thing this is trying to help the user MAKE.
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

    // For each end, the distance to the NEAREST other end.  At or under `tol` it is
    // already welded and is not a gap; over it, it is exactly what has to close.
    // O(m^2) over the ends of the OPEN objects only, run once per store change.
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
        // Report the SMALLEST real gap: the near miss is the one the user meant to
        // close, and a line end genuinely on its own across the map is not news.
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

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
#include <imgui/imgui.h>            // ROUND BL — the camera-overlay fill route
#include <gfx_d3d/r_gfx.h>          // GfxColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT

#include "kiwi_region.h"
#include "kiwi_arrange.h"           // ROUND K — PASS 3, the planar arrangement
#include "kiwi_camera.h"            // ROUND BK — KiwiCam_Ortho (the winding-side rule)
#include "kiwi_construct.h"
// KIWI-UX (ROUND BK, ITEM 4): kept for the TRAP notes this file cites, not for a
// call — the fill left the kiwi_lines emitter family for camwnd.cpp's own
// (Cam_DrawWindingTinted, externed below).
#include "kiwi_lines.h"             // TRAPs 3-5 (the fill's four-round history)
#include "kiwi_pick.h"
#include "kiwi_validity.h"          // ROUND R — KVALID_PLANE_DOT, the §19 V5 threshold
#include "kiwi_units.h"             // ROUND AF, ITEM 5 — KiwiUnits_GridSpacingWorld
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <math.h>
#include <vector>           // ROUND AF, ITEM 5 - the gap report scratch buffer
#include <string.h>

// ── ported entry points (verified against their definitions) ────────────────
extern int   Sys_Printf( const char *fmt, ... );                         // win_qe3.cpp
extern int   g_nUpdateBits;                                              // 0x25D5A74 (mainfrm.cpp)
// ROUND R — the fill's eye nudge needs the view normal.  `camera_s` comes from
// mainfrm.h, included above.
// KIWI-UX (CLEANUP, B-6): it returns &g_camwndState.camera and NEVER returns NULL
// (contract stated at camwnd.cpp:154-160), so the unguarded deref in the fill pass
// is correct and a `!c` guard there would be dead code.
extern camera_s *Ed_Camera();                                            // camwnd.cpp:161
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
// ── KIWI-UX (ROUND BK, ITEM 4): THE DONOR EMITTER ───────────────────────────
// camwnd.cpp's own translucent world-space polygon draw — the function the SKY
// FILM is drawn with (camwnd.cpp's sky see-through arm calls Cam_DrawFaceTinted,
// which is now three lines over this).  Signature copied VERBATIM from the
// definition; `pts` is the polygon's world points, `n` its plane normal, `bgra`
// the packed per-vertex colour, `push` the displacement along `n`.
//   camwnd.cpp:1221  void Cam_DrawWindingTinted( const float (*pts)[3], int nv,
//                        const float *n, Material *mtl, uint bgra, float push,
//                        MaterialTechniqueType tech )
extern void  Cam_DrawWindingTinted( const float ( *pts )[3], int nv, const float *n,
                                    Material *mtl, uint bgra, float push,
                                    MaterialTechniqueType tech );        // camwnd.cpp:1221

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
    // KIWI-UX (CLEANUP, A-11): the body is KiwiRegion_FinestEdge now — this is
    // the plane-space CLOSED-loop spelling of it, and kiwi_conselect.cpp's Join
    // is the world-space open-or-closed one.
    float FinestLoopEdge( const std::vector<float> &pts )
    {
        return KiwiRegion_FinestEdge( pts.empty() ? 0 : &pts[0],
                                      PtCount( pts ), 2, true );
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
    // WHAT WAS WRONG WITH THE OLD ONE (D-AL6, and the brief's own complaint): it
    // counted submits AFTER R_AddRenderCmdDrawTris, which returns void and drops
    // silently.  It could only ever see derivation-side failures, and it reported
    // nothing for a store with NO regions at all — the one state with zero
    // instrumentation anywhere in the subsystem.  (KIWI-UX (CLEANUP, B-25): that
    // counter outlived the fix as a write-only duplicate of `nFills` and is gone.)
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
    // ROUND AG, ITEM 1: the selection is a SET now, so this resolves it into a flag
    // array once rather than asking KiwiRegion_IsSelected per region.
    //
    // KIWI-UX (CLEANUP, B-4) — WHAT THIS DOES AND DOES NOT COST.  It removes the
    // per-REGION factor only.  KiwiRegion_SelectedCount (:1281) and every
    // KiwiRegion_SelectedAt (:1290) call still run ResolveCentroid (:1199) over
    // every stored centroid, and ResolveCentroid walks every region — so the block
    // below is O(sel^2 * regions) per drawn frame, not linear.  Collapsing it needs
    // ONE accessor that resolves the whole centroid list in a single
    // O(sel * regions) pass (and that pass must keep ResolveCentroid's `d <= bestD`
    // tie-break, which takes the LAST equal-distance match).
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

    // ROUND BK, ITEM 4: the loop's world points, handed to the donor route one
    // CONVEX PIECE at a time.  A region is capped at KREG_MAX_LOOP vertices, so
    // both buffers are bounded and can be exact rather than guarded.  The PIECE
    // buffer is bounded by the DONOR's own array size instead — see the cap check
    // in the piece loop below.
    const int KREG_DONOR_MAX_PTS = 64;           // == CAM_MAXFACEVERTS, camwnd.cpp:532
    static float s_world[KREG_MAX_LOOP][3];      // the loop, in world space
    static float s_piece[KREG_MAX_LOOP][3];      // one convex piece of it

    // ── KIWI-UX (ROUND AM, ITEM 1): GATE 4/5 — THE MATERIAL AND ITS TECHNIQUE.
    // R_AddRenderCmdDrawTris' FIRST silent drop: `Material_GetTechnique(handle,
    // techType)` null -> `return` with no diagnostic (r_rendercmds.cpp:2173).
    // Read it here the way camwnd.cpp:595 Cam_MaterialWritesDepth already reads
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
    int nTooFew = 0, nTooMany = 0, nNoTris = 0;

    // KIWI-UX (ROUND BK, ITEM 4): the sample the success line below reports.
    int   emitSample       = -1;
    int   emitSamplePieces = 0;
    int   emitSampleVerts  = 0;
    float emitSampleV[3]    = { 0.0f, 0.0f, 0.0f };
    float emitSampleRgba[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

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

        // ── KIWI-UX (ROUND BK, ITEM 4): CONVEX PIECES, NOT AN INDEX LIST ────
        // The donor route (camwnd.cpp Cam_DrawWindingTinted) takes ONE CONVEX
        // polygon and fans it, because that is what a brush face is.  The region
        // walker's loop may be concave, so it is decomposed with the machinery the
        // EXTRUDE already trusts for exactly this reason (KiwiRegion_ConvexPieces,
        // Hertel-Mehlhorn over the same ear-clip).  A convex loop comes back as one
        // piece, so the common case is still one draw per region.
        std::vector< std::vector<int> > pieces;
        if ( !KiwiRegion_ConvexPieces( reg.pts, &pieces ) || pieces.empty() )
        {
            ++nNoTris;
            if ( !gate ) gate = "TRIANGULATE (the ear-clip refused the loop)";
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

        // ══════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND BK, ITEM 4) — THE SKY FILM'S BRACKET, VERBATIM.
        // ══════════════════════════════════════════════════════════════════
        // The rule this replaces (CLEANUP B-19) was "submit on the pass-level
        // NEUTRAL bracket, take no per-region override", copied from the boolean
        // operand preview.  It has now been shipped for three rounds and the fill
        // has never appeared, so this round takes its state from the OTHER
        // confirmed-visible translucent fill instead — the round-BC sky film.
        //
        // WHAT THE SKY FILM ACTUALLY RUNS UNDER, read off camwnd.cpp rather than
        // assumed: the world face loop reaches it with `ecol` from
        // Cam_EditorMaterialColor, which SEEDS `out[3] = 1.0f` (camwnd.cpp:618-620)
        // and whose "sky" row overwrites rgb only — so MATERIAL_COLOR is
        // { r, g, b, **1.0** }, a FLAT COLOUR OVERRIDE, and the film's translucency
        // comes entirely from the packed PER-VERTEX alpha (0.30).
        //
        // THAT SETTLES kiwi_lines.h TRAP 5's ONE OPEN QUESTION, by demonstration
        // rather than by probe: `.w == 1` does NOT make a fill opaque.  The sky
        // film is a flat-override draw at vertex alpha 0.30 and it is see-through
        // on the user's machine — which is the whole reason round BC shipped it.
        // So the region fill takes the same recipe: rgb into MATERIAL_COLOR at
        // w == 1, the same rgb into the per-vertex colour with the REGION's alpha.
        const float flat[4] = { rgba[0], rgba[1], rgba[2], 1.0f };
        R_AddCmdSetMaterialColor( flat );

        // The nudge is along the REGION PLANE's normal now, not the view axis,
        // because the donor route applies it as `p + n*push` (its `push` argument,
        // the ported selected-face overlay's own displacement).  Signed at the eye
        // so a region viewed from behind is still lifted TOWARD the viewer — which
        // is what round R's `-vpn` was reaching for and is the same answer without
        // making the geometry a camera quantity.
        float nrm[3] = { reg.plane.normal[0], reg.plane.normal[1], reg.plane.normal[2] };
        {
            float mid[3];
            KiwiCon_PlaneToWorld( reg.plane, Pt( reg.pts, 0 ), mid );
            const float toEye[3] = { c->origin[0] - mid[0],
                                     c->origin[1] - mid[1],
                                     c->origin[2] - mid[2] };
            // In ORTHO there is no eye POINT (kiwi_lines.h, round AL, ITEM 2), so
            // the side is decided by the view DIRECTION exactly as the winding rule
            // is: the face pointing at the viewer is the one with n . vpn < 0.
            const float side = KiwiCam_Ortho() ? -Dot3( nrm, c->vpn ) : Dot3( nrm, toEye );
            if ( side < 0.0f )
                for ( int k = 0; k < 3; ++k )
                    nrm[k] = -nrm[k];
        }

        for ( int i = 0; i < n; ++i )
        {
            // KIWI-UX (ROUND BK, ITEM 4): the bare world point.  The KREG_FILL_NUDGE
            // displacement is now the donor route's own `push` argument (applied
            // along `nrm`, at the call below), and the per-vertex NORMAL and ST are
            // the donor's too.
            float w[3];
            KiwiCon_PlaneToWorld( reg.plane, Pt( reg.pts, i ), w );
            s_world[i][0] = w[0];
            s_world[i][1] = w[1];
            s_world[i][2] = w[2];
        }
        // ── THE THREE CHANNELS THIS ROUND HANDED TO THE DONOR, AND THE ROUNDS
        //    THAT OWNED THEM (kept short; the full arguments are in
        //    RADIANT_UX_DESIGN §64/§65/§66/§69 and kiwi_lines.h TRAPs 3-5) ──────
        //   * POSITION.  Round R nudged the fan back along `-vpn` by
        //     KREG_FILL_NUDGE so a region drawn ON a brush face is not decided
        //     pixel-by-pixel against that face's depth.  The nudge survives; it is
        //     along the (viewer-oriented) PLANE normal now, which is what the
        //     donor's `push` means and is no longer a camera quantity.
        //   * NORMAL.  Round AK moved it from the region plane to `-vpn`, round AL
        //     to KiwiTris_FillNormal's constant +Z, and the fill was invisible
        //     under all three.  It is the region's own plane normal again — the
        //     value BOTH visible fills in this editor write (kiwi_boolean.cpp
        //     FillBrush, camwnd.cpp's sky film) and the value the binary's own
        //     batcher writes (brush.cpp Face_AddWindingToTriBatch, 0x47b86a).
        //   * ST.  Was (0,0) on every kiwi fill; the donor writes a real planar
        //     1/128 projection, so a colorMap sample has somewhere to land.

        // ── KIWI-UX (ROUND AA, ITEM 2) — WINDING, and where it lives now ────
        // USER REPORT, verbatim: "the light blue construction lineface isn't
        // rendering unless you're facing the other way (see pics).  Fix this."
        // The loop is CCW in the REGION PLANE's basis, so the emitted winding was
        // locked to reg.plane and had nothing to do with where the eye is, and
        // white_tools culls back faces (kiwi_lines.h TRAP 3) — exactly one side of
        // every region drew.  Round AA answered that with KiwiTris_OrientToEye,
        // which rewrites the INDEX buffer.  The donor route owns its own fan, so
        // the answer moves to the VERTEX ORDER instead: a piece whose plane normal
        // points away from the viewer is emitted REVERSED, which is the same fix
        // one level up and needs no index surgery.
        //
        // ── KIWI-UX (ROUND AM, ITEM 1): GATE 8 — THE COMMAND BUFFER ─────────
        // R_AddRenderCmdDrawTris' SECOND silent drop.  Its byte cost is the exact
        // arithmetic r_rendercmds.cpp:2176-2183 does: a 16-byte header, then
        // 16+12+4+8 bytes of vertex streams per vertex, then a 2-byte-per-index
        // list rounded up to an even count.  Compared against the same sizeLimit
        // R_GetCommandBuffer will compute, BEFORE the call, so the drop is named
        // instead of inferred.  Asked per PIECE now, because a concave region is
        // more than one draw.
        const bool flip = ( Dot3( nrm, reg.plane.normal ) < 0.0f );
        for ( size_t pi = 0; pi < pieces.size(); ++pi )
        {
            const std::vector<int> &poly = pieces[pi];
            const int pn = (int)poly.size();
            // KREG_DONOR_MAX_PTS is the donor's own stack-array bound
            // (CAM_MAXFACEVERTS, camwnd.cpp:532).  Over it Cam_DrawWindingTinted
            // returns SILENTLY, which is precisely the class of drop this pass
            // exists to name, so it is checked HERE with a gate name of its own.
            // Hertel-Mehlhorn merges only while the result stays convex, so a piece
            // this large needs a 64-corner convex region and is not reachable from
            // the KCON tools — but "not reachable" is how the last five rounds of
            // this bug were argued.
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

            // ── THE DONOR CALL.  Literally the function the SKY FILM is drawn
            //    with (camwnd.cpp Cam_DrawWindingTinted, reached from
            //    Cam_DrawFaceTinted at camwnd.cpp's sky see-through arm): same
            //    material, same TECHNIQUE_UNLIT, same planar 1/128 ST, same plane
            //    normal per vertex, same fan, same one R_AddRenderCmdDrawTris.
            Cam_DrawWindingTinted( (const float (*)[3])s_piece, pn, nrm,
                                   g_qeglobals.d_white, (uint)packed.packed,
                                   KREG_FILL_NUDGE, TECHNIQUE_UNLIT );
            ++nFills;
            nTrisSub += pn - 2;
        }

        // The FIRST region that reached the renderer this pass, kept for the one
        // loud line below (printed after the loop, so it cannot flip-flop between
        // regions frame after frame).
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

    // ══════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BK, ITEM 4) — THE ONE LOUD LINE THE BRIEF ASKED FOR
    // ══════════════════════════════════════════════════════════════════════════
    // Six rounds of instrumentation have produced no report because every one of
    // them printed only on FAILURE, and this pass has never failed — the geometry
    // has always reached the renderer.  So this one prints on SUCCESS: once per
    // store generation (i.e. once per sketch edit, not sixty times a second) it
    // names the route, the piece count, the vertex count, the first world vertex
    // and the colour.  If the fill is still invisible after the route swap, the
    // user's next report carries the numbers that pin it — and if the line does not
    // appear at all, then the pass is not running, which is a different bug and one
    // this line finally distinguishes.
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
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BL, ITEM 3) — THE FILL, DRAWN WHERE IT CANNOT FAIL TO DRAW
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: *"There is still no light blue plane where a construction
// face can be extruded from.  You've failed again!"*
//
// SIX rounds (AA, AK, AL, AM, AQ, BK) have been spent on the ENGINE route: the
// winding order, the vertex normal, the ST, the material colour, the pass location,
// and finally BK's transplant onto the confirmed-visible sky film's own emitter.
// Every gate in that chain is open when read (the instrumentation above proves the
// geometry reaches R_AddRenderCmdDrawTris with a technique and buffer headroom),
// and the fill has still never appeared on the user's machine.  Whatever is eating
// it is downstream of everything this tree can read.
//
// So this round stops arguing with the renderer and draws the fills on the ONE
// surface that is demonstrably painted every single frame in the user's build: the
// ImGui camera overlay, the same ImDrawList that carries the HUD chips, the snap
// label, the value bubble, the view cube and the marquee — all of which the user
// can see in the very screenshots that report the missing fill.
//
// ── THE KNOWN LIMITATION, STATED LOUDLY ─────────────────────────────────────
// An overlay has NO DEPTH BUFFER.  A region behind a wall still shows through, and
// a region is drawn over any world geometry in front of it.  That is the accepted
// price of a fill that is guaranteed to exist; a §8 region is editor scaffolding
// that exists to be seen and clicked, not shaded world surface.  If the engine
// route is ever proven to work, this can be demoted to a fallback in one line — its
// call site is KiwiVP_DrawCameraOverlay and nothing else calls it.
//
// ── WHAT IS KEPT ────────────────────────────────────────────────────────────
// KiwiRegion_DrawFills (the engine route) and its once-per-store-generation console
// line stay exactly as round BK shipped them.  They cost nothing, they still age
// the flash counters (the one thing this pass must NOT do — it would double-age
// them), and the line is the only instrument that will ever tell us if the engine
// route starts working.
//
// ── PROJECTION ──────────────────────────────────────────────────────────────
// Pick_WorldToImage (kiwi_pick.cpp:500) — the editor's ONE world->camera-image
// projection, exact inverse of CameraCalcRayDir, ortho-aware since round M, and the
// helper every other screen-space consumer already uses.  Image pixels are
// TOP-LEFT origin and the overlay's own origin is (imgMinX, imgMinY), so screen =
// imgMin + image — the identical two lines KiwiNum_DrawBubble uses to pin the value
// bubble at world geometry (kiwi_numeric.cpp:822 + :878-879).  It returns FALSE for
// a point at or behind the eye plane in perspective (and never fails in ortho), so
// "any vertex refused" is exactly the near-plane case, and the loop is dropped
// whole rather than clipped — cheap, honest, and noted here rather than hidden.
namespace
{
    // Bounded per frame on both axes.  KREG_MAX_LOOP is the loop cap the derivation
    // already enforces (kiwi_region.h:249); the loop cap here is this pass's own —
    // a store that derives hundreds of regions is a sketch the user cannot read
    // anyway, and the overlay must never be able to cost more than the HUD.
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

    // The CONST accessor on purpose: KiwiRegion_DrawFills owns the flash counters
    // (kiwi_region.h:349-353) and ageing them from a second per-frame call would
    // halve the flash.  This pass reads and draws; it changes nothing.
    const std::vector<kregion_t> &regions = KiwiRegion_All();
    if ( regions.empty() )
        return;

    // The selection, resolved ONCE for the pass — same reason as the engine route's
    // copy at :1416-1425, and the same accessors.
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

        // Project the whole loop first: a single refusal drops the loop, so a
        // partially-behind polygon can never be drawn wrapped around the viewport.
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
            // A cheap "is any of this anywhere near the image" test, generously
            // padded: a triangle whose corners are all off one edge can still cover
            // the view, so this only rejects loops entirely outside a padded rect.
            if ( scr[i].x >= clipMin.x - imgW && scr[i].x <= clipMax.x + imgW
              && scr[i].y >= clipMin.y - imgH && scr[i].y <= clipMax.y + imgH )
                onScreen = true;
        }
        if ( !ok || !onScreen )
            continue;

        // BOTH SIDES.  No back-face test: the user asked to see these faces, the
        // click machinery (KiwiRegion_PickAt) has always accepted either side, and
        // an overlay has no winding rule to obey in the first place.
        //
        // The loop may be CONCAVE, and AddConvexPolyFilled would fill its hull.  The
        // decomposition is the one the extruder already trusts (Hertel-Mehlhorn over
        // the shared ear clip) — the same call the engine route makes at :1517.
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
            // The ear clip refused the loop (it self-intersects, or it is degenerate
            // in plane space).  The outline below still draws, so the user gets the
            // boundary rather than nothing at all.
        }

        // …and the boundary, always, over the fill.
        dl->AddPolyline( scr, n, sel ? KREG_OVERLAY_EDGE_SEL : KREG_OVERLAY_EDGE,
                         ImDrawFlags_Closed, 1.5f );
        ++drawn;
    }

    dl->PopClipRect();
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
// KIWI-UX (CLEANUP, A-11): see kiwi_region.h for why this is exported and which
// three sites it replaced.  The body is FinestLoopEdge's, generalised over stride
// and over the wrap edge; both were already exact in the copies.
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

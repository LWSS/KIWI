#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_arrange.h — ROUND K: the PLANAR ARRANGEMENT behind §8's regions.
//
// USER DIRECTIVE, verbatim: "When creating a closed off line shape, it currently
// only closes off when the points are perfectly at corners, it should do it
// whenever lines close off a section even if they extend further (see pic)".
// The picture is four lines crossing like a hash — # — and the quad in the middle
// must become a region even though every one of the four lines runs past the
// corner it helps make.
//
// ── WHY THE EXISTING PASSES CANNOT DO IT ────────────────────────────────────
// kiwi_region.cpp's two passes are both ENDPOINT passes.  PASS 1 accepts an
// object that is already closed; PASS 2 chain-walks an endpoint graph in which
// "these two objects are joined" means "an END of one is within KREG_JOIN_DIST of
// an END of the other" (KiwiRegion_ChainWalk).  In the # case NO endpoint touches
// any other endpoint — every meeting is a mid-span CROSSING — so the graph is
// four disjoint degree-1 pairs, the walk fails, and the enclosed quad is invisible
// to the whole of §8.  That is not a tolerance problem and no tolerance fixes it.
//
// ── WHAT PLASTICITY DOES (read, not assumed) ────────────────────────────────
// Exactly this, in two files:
//
//   plasticity/src/editor/curves/PlanarCurveDatabase.ts — its `add()` docstring
//   states the algorithm in as many words (:24-30): "to add a new curve, first
//   find its intersections with all other curves.  Now, suppose there's one
//   intersection along the parameter of the curve at i.  This intersection cuts
//   the curve in two.  Thus we trim curve from [0,i], and [i,1] […] Then we
//   process the next curve (the one we intersected with earlier), since it also
//   has been cut in two."  The implementation collects every coplanar curve on the
//   same placement (:50-57), intersects the current one against ALL of them
//   (`c3d.CurveEnvelope.IntersectWithAll`, :77), sorts the crossings by parameter
//   (:100) and emits one TRIMMED FRAGMENT per consecutive pair (:122-134).  The
//   store therefore holds a FRAGMENT SET, not the user's curves.
//
//   plasticity/src/editor/curves/RegionManager.ts — `updatePlacement()` takes the
//   coplanar curves for a placement (:16), decomposes any contour back into its
//   segments (:20-31, with the kernel's own note that "contours need to be turned
//   into segments before calling OuterContoursBuilder"), and hands the flat
//   segment list to `c3d.ContourGraph.OuterContoursBuilder` followed by
//   `c3d.ActionRegion.GetCorrectRegions` (:33-35).  Those two are the kernel's
//   planar-graph face finder: contours out of a soup of segments, then the regions
//   those contours bound.  Every region Plasticity shows is produced that way —
//   there is no endpoint-chaining path in it at all.
//
// KIWI mirrors that shape with the two steps written out, because there is no
// c3d kernel here:
//
//   1. FRAGMENT.  Project the group's world segments into the group's plane, then
//      split EVERY segment at EVERY mutual crossing (proper crossings and
//      T-junctions both).  This is PlanarCurveDatabase's trim loop.
//   2. FACE-WALK.  Weld the fragment endpoints into nodes, sort each node's
//      outgoing half-edges by angle, and trace minimal cycles by always taking the
//      CLOCKWISE-NEXT half-edge after the twin.  That rule traces every face with
//      its interior on the LEFT, so a BOUNDED cell comes out counter-clockwise
//      (positive signed area) and the one unbounded outer face per connected
//      component comes out clockwise (negative) and is dropped by sign.  This is
//      OuterContoursBuilder + GetCorrectRegions.
//
// The store is NEVER touched.  Fragments are a DERIVED VIEW built inside one call
// and thrown away; the user's lines keep their identity, their selection and their
// undo history, which is the one place KIWI deliberately differs from Plasticity
// (whose database really does replace the curves with their fragments).
//
// ── THE SPLITTING MATH IS THE TRIM TOOL'S ───────────────────────────────────
// KiwiCon_SegSegClosest (kiwi_construct.cpp:430 — the clamped closest-approach
// solve) is what kiwi_trim.cpp's GatherCuts uses to decide two segments cross, and
// it is what this file uses, at the SAME tolerance (KCON_ISECT_DIST).  One
// definition of "these two lines meet" in the editor, not two.
//
// ── CAPS, AND WHY EACH ONE IS WHERE IT IS ───────────────────────────────────
// The pass runs once per store CHANGE (kiwi_region.cpp re-derives on a generation
// bump), never per frame, so the budget is generous — but it is a hard budget, and
// an overflow LOGS and yields NOTHING for that group rather than yielding a
// partial arrangement, because a partial arrangement is a wrong arrangement.
//
//   KARRG_MAX_SEGS    256   input segments per coplanar group.  Splitting is the
//                          O(n^2) step: 256^2 = 65k pair tests, each a handful of
//                          dot products.  A store big enough to exceed this is a
//                          store where a 64-segment circle count has been drawn
//                          four times over on one plane.
//   KARRG_MAX_EDGES   1024  fragments after splitting.  n segments with k crossings
//                          each produce n + (total crossings) fragments; the cap is
//                          4x the segment cap, i.e. an average of three crossings
//                          per segment.
//   KARRG_MAX_NODES   1024  welded fragment endpoints.  Welding is O(nodes^2) in
//                          the worst case (1M compares, once per store change).
//   KARRG_MAX_CELLS    64   bounded cells emitted per group.
// A cell with more than KREG_MAX_LOOP vertices, or less than KREG_MIN_AREA of
// area, is dropped individually (it is not an overflow — it is a cell §8 already
// says is not a region).
// ─────────────────────────────────────────────────────────────────────────────

#include <vector>

#include "kiwi_construct.h"

// KIWI-UX (CLEANUP, B-26): the prefix is KARRG_ ("arrangement"), not KARR_.
// KARR_ was claimed by TWO unrelated families — kiwi_dupe.h's Array-duplicate
// tuning (KARR_MIN_COUNT / KARR_MAX_COUNT / KARR_DEF_LINEAR / KARR_DEF_RADIAL /
// KARR_MIN_OFFSET / KARR_MAX_SOURCE / KARR_LINE_BREAK_PIXELS, plus its KARR_FIELDS
// table) and this one.  No TU includes both headers today, so nothing collided —
// but grepping KARR_MAX_ returned two unrelated caps, and kiwi_dupe is the older,
// larger and more public claimant, so THIS family moved.  Four constants, one
// non-public consumer (kiwi_region.cpp reaches them through this header).
#define KARRG_MAX_SEGS    256
#define KARRG_MAX_EDGES   1024
#define KARRG_MAX_NODES   1024
#define KARRG_MAX_CELLS   64

// One bounded cell of the arrangement: a CCW plane-space loop, first point NOT
// repeated — exactly the shape kregion_t::pts carries, so kiwi_region.cpp can put
// it straight into a region and run its ordinary AcceptLoop gate over it.
struct karrCell_t
{
    std::vector<float> pts;
};

// Build the arrangement of `objects` (indices into the construction store) in
// `plane`, and return every BOUNDED cell it encloses.
//
// `objects` may mix open and closed objects freely — the arrangement does not care
// which is which, only where the segments are.  Objects whose points do not lie on
// `plane` are the caller's problem: it has already grouped by coplanarity
// (kiwi_region.cpp), and anything off-plane would be silently flattened here.
//
// False = an overflow (already logged) or nothing to do; `outCells` is cleared
// either way.  A true return with an empty list is the ordinary "these lines
// enclose nothing" answer.
bool KiwiArrange_Cells( const int *objects, int count, const kconPlane_t &plane,
                        std::vector<karrCell_t> *outCells );

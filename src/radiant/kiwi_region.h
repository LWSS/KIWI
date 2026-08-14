#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_region.h — RADIANT_UX_DESIGN §8: closed-loop Region detection over the
// construction store, the translucent light-blue fill, and the 2D polygon
// toolkit §23's extrusion needs (ear-clip triangulation + Hertel-Mehlhorn
// convex decomposition).
//
// A REGION is a closed, coplanar loop of construction geometry.  It renders as a
// translucent light-blue fill, and it is what "Extrude Region" consumes.
// Regions are DERIVED, never stored: KiwiRegion_All() re-derives whenever
// KiwiCon_Generation() moves, so a loop that opens loses its fill on the next
// frame with no bookkeeping anywhere.
//
// ── WHAT COUNTS AS A LOOP (v1) ──────────────────────────────────────────────
//   1. ONE OBJECT, already closed — a closed polyline, a rect, a circle.  Its
//      tessellated vertices ARE the loop, and they must FIT A PLANE.
//   2. A CHAIN of separate open objects (line / polyline / arc) whose endpoints
//      meet, forming exactly one closed cycle, and whose points ALL FIT ONE PLANE.
// Tolerances, all in one place so they can be argued with:
//   * endpoints meet   within KREG_JOIN_DIST (0.5 world units), measured in 3D.
//   * a vertex used by three or more segment ends is AMBIGUOUS and the whole
//      component is rejected — §8's "each endpoint shared by exactly two
//      segments" read strictly.  Guessing which branch to take is how a region
//      silently becomes the wrong region.
//   * planarity   every point within KCON_PLANE_FIT_DIST (0.5 world units) of the
//      fitted plane (KiwiCon_FitPlane, Newell).
//
// ── SHAKEOUT H: PLANARITY IS TESTED, NOT ASSUMED ────────────────────────────
// Before this round the construction store was plane-space (kiwi_construct.h
// ruling 3 as it stood), so every object CARRIED a plane and grouping was a
// plane-vs-plane comparison — KREG_PLANE_DOT / KREG_PLANE_DIST, applied to the two
// objects' stored bases.  With a world-space store there is no stored plane to
// compare: an object's plane is DERIVED and a two-point line does not determine
// one at all.  So grouping is now a POINT-vs-PLANE test — "do all of this object's
// points lie on the group's fitted plane" — which is strictly better:
//   * it handles the 2-point line, which has no plane of its own but is happy to
//     lie on anybody's;
//   * it cannot be fooled by two objects whose stored bases agreed while their
//     points had drifted apart, which the old comparison could not see;
//   * it is the SAME question the extruder needs answered, asked once.
// KiwiRegion_SamePlane, the old plane-vs-plane test, has no callers left and is
// deleted with its two tolerance macros — see the note at its former declaration.
//
// ── ROUND K: A THIRD SOURCE OF LOOPS (kiwi_arrange.h) ───────────────────────
// The two rules above are ENDPOINT rules, and a user drawing four lines that cross
// like a # produces no matching endpoints at all — every meeting is a mid-span
// crossing, ChainWalk sees four disjoint degree-1 pairs, and the quad in the middle
// was invisible to the whole of §8.  A third pass now splits every coplanar segment
// at every mutual crossing and walks the resulting planar graph for its BOUNDED
// cells; each cell is a region on exactly the same terms as one of the above (same
// AcceptLoop gate, same fill, same extrude).  Duplicates against passes 1 and 2 are
// dropped by world centroid + area.  Caps, complexity and the Plasticity sources
// this mirrors are all in kiwi_arrange.h; the loop rules above are unchanged and
// still own the cheap, unambiguous cases.
//
// v1 has NO holes and NO nested loops (spec decision D-3, still open): two
// concentric closed circles produce TWO regions, not a ring.  A region whose
// loop self-intersects is rejected outright — an extrusion of it would produce
// brushes that fold through themselves, and §19 says reject > repair.
//
// ── THE FILL ────────────────────────────────────────────────────────────────
// Emitted through the editor's own immediate-mode triangle path,
// R_AddRenderCmdDrawTris(d_white, TECHNIQUE_UNLIT, ...) — the SAME call the
// ported selected-face fill (Cam_DrawSelectedFaceFill, camwnd.cpp 0x408106) and
// the ported 3D marquee quad (Ed_DrawSelectionBoxQuad, xywnd.cpp 0x40CC50) use.
// Ed_DrawSelectionBoxQuad is the proof that per-vertex ALPHA works on that path
// (it draws blue at 0.25), which is what makes a real translucent fill possible
// instead of the hatch fallback.  MATERIAL_COLOR is set neutral for the draw and
// back to white afterwards, exactly as the two ported passes bracket themselves.
//
// ── THE 2D TOOLKIT (used by §23) ────────────────────────────────────────────
// All of it works on a PLANE-SPACE point list (2 floats per vertex, CCW), which
// is why kiwi_construct.h stores points that way.  Ear clipping handles
// collinear points (a zero-area ear is clipped, not rejected) and bails on a
// self-intersecting or degenerate loop; Hertel-Mehlhorn then removes every
// diagonal whose two triangles merge into a still-convex polygon, which is the
// decomposition §23 fixes as the algorithm choice.
// ─────────────────────────────────────────────────────────────────────────────

#include <vector>

#include "kiwi_construct.h"

struct ray_t;                       // kiwi_pick.h

// SHAKEOUT H: KREG_PLANE_DOT / KREG_PLANE_DIST are GONE with the plane-vs-plane
// test they served.  §8's coplanarity tolerance now has exactly one spelling,
// KCON_PLANE_FIT_DIST (kiwi_construct.h), because there is now exactly one way to
// ask the question: does this point lie on that plane.
#define KREG_JOIN_DIST   0.5f       // world units, endpoint chaining (3D) — the FLOOR

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AF, ITEM 5 — THE TOLERANCES ARE GRID-AWARE NOW
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "Curve closed loop detection still needs a bit more work.
// I find myself having to re-trace the line points myself to get it to detect a
// construction face."
//
// ROUND AA fixed the SAME-CHAIN case at the source: point 2 onward is projected
// onto the plane point 1 latched (kiwi_construct.cpp:1889-1917), and its note is
// still right — "loosening the region tolerance would take a genuinely skew ring
// and pretend it was flat".  What that fix cannot reach is a loop assembled from
// SEVERAL objects drawn at different times, on different working planes, possibly
// before the latch existed.  Those meet the acceptance path with endpoints that
// nearly coincide and points that nearly share a plane, and "nearly" was measured
// against a HARD 0.5 world units everywhere — a number chosen when the editor
// worked at grid 1 and 2.
//
// THE ARGUMENT FOR SCALING, and it is what keeps this from being "loosen it and
// hope".  Construction points SNAP TO THE GRID (§17, absolute snapping since round
// P).  Two endpoints the user meant to be DIFFERENT therefore land on different
// grid nodes, i.e. at least ONE FULL GRID STEP apart.  Two endpoints the user
// meant to be the SAME land on the same node — exactly, when both were snapped,
// and within a hair when one was not.  So any weld tolerance strictly below HALF a
// grid step is incapable of fusing two points the grid put in different places,
// and a QUARTER of a step is that with a factor of two in hand.
//
// The FLOOR is the old 0.5, so nothing gets tighter than it has ever been.  The
// CEILING is 16 units, because the argument above assumes snapped points and a
// mapper working at grid 256 with snapping off would otherwise get a 64-unit weld,
// which is a real distance in a real map.
//
//   weld  = clamp( grid * 0.25, KREG_JOIN_DIST,        KREG_TOL_MAX )
//   band  = clamp( grid * 0.25, KCON_PLANE_FIT_DIST,   KREG_TOL_MAX )
//
// The BAND is the point-to-plane distance a candidate loop's vertices may sit at
// and still count as coplanar.  It is not a new mechanism: the acceptance path
// ALREADY projects every member's points onto the group's best-fit plane
// (AppendObjectPoints → KiwiCon_WorldToPlane), and the plane itself already comes
// from Newell's method (KiwiCon_FitPlane).  All that changes is how far out a
// point may be and still be admitted to that projection.
#define KREG_TOL_MAX     16.0f      // world units — the ceiling on both of the above

// The endpoint weld distance to use RIGHT NOW.  Every chaining test in the region
// and arrangement layers asks this rather than reading KREG_JOIN_DIST directly.
float KiwiRegion_WeldDist();

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AG, ITEM 2 — A WELD MAY NEVER EXCEED THE GEOMETRY IT IS WELDING
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "Construction faces are lacking their light blue color.
// It should show light blue whenever I can extrude from a closed off set of
// construction lines - but it only does it sometimes."  The picture is an ARCH
// profile — straight lines plus a tessellated arc — with several enclosed cells
// unfilled.
//
// ── THE DIAGNOSIS: ROUND AF'S TOLERANCE WAS RIGHT AND WAS APPLIED TOO WIDELY ─
// Round AF item 5 scaled the weld to the grid, and its argument is sound AS FAR
// AS IT GOES: construction points SNAP, so two points the user meant to be
// different are at least one grid step apart, and a quarter step therefore cannot
// fuse them.  **That argument is about USER-PLACED ENDPOINTS.**  It is false for
// TESSELLATED vertices, and every consumer below eats tessellated vertices:
//
//     a circle/arc of radius r at the KCON_SEGS_MAX cap has an edge length of
//     2*pi*r/64 = 0.098*r, which has NOTHING to do with the grid.  A radius-32
//     arc has 3.1-unit edges.  At grid 16 the weld is 4.0.
//
// So at grid 16 and coarser:
//   * kiwi_arrange.cpp's NodeFor welded each arc vertex onto the previous one, so
//     every fragment came back with n0 == n1 and was `continue`d — the arc left
//     the arrangement entirely and the cells it bounded stopped existing;
//   * the same file's fragment-length floor (`Len2(prev,cur) > weld`) dropped
//     those fragments a second time;
//   * AcceptLoop's DedupLoop chewed the arc's vertices out of any loop that DID
//     survive, collapsing it below three points.
// A rectangle drawn at grid 16 has 16-unit edges and sails through all three.
// THAT is "it only does it sometimes": it is a function of the grid and of how
// finely the round objects in the loop are tessellated, and of nothing else.
//
// ── THE RULE ────────────────────────────────────────────────────────────────
// A weld may never exceed KREG_WELD_EDGE_FRAC of the FINEST edge it is being
// applied to.  Below that fraction it cannot collapse the finest edge present, so
// no legitimate vertex can be eaten, whatever the grid says.  Above the finest
// edge the grid's number still wins, so round AF's fix is intact everywhere it
// was actually about: a sketch of straight lines drawn at grid 64 still welds at
// 16, because its finest edge is 64.
//
//   weld = clamp( min( KiwiRegion_WeldDist(), finestEdge * FRAC ),
//                 KREG_JOIN_DIST, KREG_TOL_MAX )
//
// `finestEdge` is the shortest edge in the input that is ABOVE KREG_JOIN_DIST —
// edges at or below the old hard floor are the degenerate ones the weld exists to
// remove, and letting them set the bound would make the bound zero.
//
// 0.4 rather than 0.5 so the bound still holds after the projection into plane
// space and the float arithmetic either side of it; there is nothing deeper in
// the number than "comfortably under a half".
#define KREG_WELD_EDGE_FRAC  0.4f

float KiwiRegion_WeldFor( float finestEdge );

// The point-to-plane band to use RIGHT NOW, for the same reason.
float KiwiRegion_PlaneBand();

// ── ROUND AF, ITEM 5(c): SAY WHY THE LOOP DID NOT CLOSE ─────────────────────
// Region detection has never printed a word on failure — every rejection in
// BuildRegions is a silent `continue` — so "it did not detect my face" has been
// unanswerable from the editor and the user's only move was to re-trace the
// points.  This walks the visible OPEN construction objects, welds their ends at
// KiwiRegion_WeldDist, and reports every DANGLING end together with how far the
// nearest other dangling end is: "loop gap 0.8 at (128 64 0)".
//
// Throttled to once per store generation, so it speaks when the geometry changes
// and not once a frame.  `force` true is the palette / explicit route and always
// prints, including the "everything is joined" answer.
void KiwiRegion_ReportGaps( bool force );
#define KREG_MIN_AREA    1.0f       // square world units — below this it is a scratch

// ── ROUND AG, ITEM 2: THE LOOP CAP IS NOT THE TESSELLATION CAP ──────────────
// This used to read `KCON_SEGS_MAX`, and that conflation was the SECOND half of
// the unfilled-arch report.  They are different quantities:
//   * KCON_SEGS_MAX (64) is how many segments ONE round object is tessellated to.
//   * KREG_MAX_LOOP is how many vertices a DERIVED loop may carry — and a derived
//     loop is bounded by PARTS OF SEVERAL OBJECTS.  A full 64-segment circle with
//     one chord drawn across it produces two cells whose vertex counts are 64+2
//     and 2+2; the first was over the cap and was dropped, SILENTLY, in two
//     places (kiwi_arrange.cpp's cell emitter and AcceptLoop).  An arch profile
//     built on a tessellated arc is exactly that shape.
// 128 = 2 * KCON_SEGS_MAX, i.e. a cell bounded by TWO fully tessellated round
// objects, which is past anything drawn by hand.  §23's KEXT_MAX_PROFILE tracks
// it, because "extrudable if and only if filled" is the invariant this round is
// about and a profile cap below the loop cap breaks it by construction.
// The fill's static buffers scale with this: 128*(4+3+2+1) floats = 5 KB and a
// 378-entry index array, both still trivial and both still bounded.
#define KREG_MAX_LOOP    128        // vertices per derived region loop

// ── SHAKEOUT F: the NEW-REGION FLASH ────────────────────────────────────────
// USER DIRECTIVE (paraphrased from the round's brief): a region forming has to be
// SEEN.  When a loop closes, the new region's fill is boosted for this many
// CAM_DRAW FRAMES and then settles to the ordinary translucent blue.  Frame
// counted, never timed: this layer has no timer, the Cam_Draw tail is the only
// clock it is allowed to read, and a wall-clock flash would keep animating while
// the editor is idle and not redrawing (i.e. it would "flash" invisibly and be
// over by the time the user's next input caused a repaint).
#define KREG_FLASH_FRAMES 60

// ── ROUND R: THE FILL'S NUDGE TOWARD THE EYE ────────────────────────────────
// USER REPORT, verbatim: "I kinda got it to work, but the blue line plane was
// invisible".  The picture is lines drawn ON THE TOP FACE OF A BRUSH, so the
// region's plane and that face's plane are the SAME plane — and the fill was being
// emitted at exactly the region plane, i.e. exactly coplanar with geometry already
// in the depth buffer.  The editor's $line/$default path is depthTest LESSEQUAL,
// so a coplanar fan is decided by float rounding per pixel: some of it passes,
// most of it does not, and what the user sees is nothing at all.
//
// This is a solved problem in this tree and the solution is copied, not invented:
// kiwi_hover.cpp's face fill nudges its vertices back along the VIEW normal
// (KHOVER_FILL_NUDGE 0.5, itself following the ported patch overlay's own
// -0.125 * camera.vpn at pmesh.cpp:4773).  The same number is used here for the
// same reason — one fill-vs-coplanar-face problem should not have two answers —
// and it is the FILL distance rather than the outline's 0.25, because a fan spans
// far more of the face than an outline does and so needs the larger margin.
#define KREG_FILL_NUDGE   0.50f     // world units, straight back along camera.vpn

struct kregion_t
{
    kconPlane_t        plane;
    std::vector<float> pts;         // plane-space loop, CCW, first point NOT repeated
    int                sourceObject = -1;   // >= 0 when one closed object IS the region
    // SHAKEOUT F: frames of flash left (see KREG_FLASH_FRAMES).  Carried across a
    // re-derive by centroid matching, so a region that merely SURVIVES a store
    // change does not re-flash and a region that is genuinely NEW does.
    int                flash = 0;
    float              centroid[3] = { 0.0f, 0.0f, 0.0f };   // world, the match key
};

// The current regions.  Re-derived when the store generation moved; cheap to
// call every frame.
const std::vector<kregion_t> &KiwiRegion_All();

// Force a re-derive (the store calls this on a change; callers rarely need it).
void KiwiRegion_Invalidate();

// The region under a cursor ray — nearest plane hit whose in-plane point is
// inside the loop.  -1 for none.
int KiwiRegion_PickAt( const ray_t &ray );

// The DISTANCE from the ray origin to that hit, for the click arbitration in
// kiwi_boxselect.cpp: a region only wins a click when no brush face is closer.
// False when the index is not a region or the ray misses its plane.
bool KiwiRegion_HitDistance( const ray_t &ray, int index, float *outDist );

// ── ROUND K: REGION SELECTION ───────────────────────────────────────────────
// USER DIRECTIVE, verbatim: "Also I can't grab the light blue part as if its a
// face.  It should be extrudable into a new solid(brush)."
//
// A region is now a SELECTABLE thing, on the same footing as construction
// geometry: a KIWI-owned selection that never enters selection_t (a region is not
// a brush, a face, an edge or a vertex, and giving it a sel_item_t would mean
// teaching the whole ported selection layer about a thing the map cannot hold).
//
// THE STATE IS A WORLD CENTROID, NOT AN INDEX, and it has to be: regions are
// DERIVED and re-derived whenever KiwiCon_Generation() moves (kiwi_region.h's
// opening note), so an index goes stale the moment a line is drawn, moved or
// trimmed — and PASS 1 / PASS 2 / PASS 3 all append, so adding one object can
// renumber everything.  The centroid is the same key CarryFlash already matches
// on, at the same tolerance, so "is this the same region" has one answer in this
// file.  A region that stops existing simply stops resolving, and the selection
// quietly empties — which is exactly what should happen when the loop it named is
// opened up.
// ── ROUND AG, ITEM 1: THE SELECTION IS A SET ────────────────────────────────
// USER DIRECTIVE, verbatim: "you fixed shift-clicking face extending (good job).
// But I want it on all extrusions.  I should be able to shift click construction
// faces.  and shift click extrude from solid faces."
//
// The state is still a WORLD CENTROID per member and for the same reason (regions
// are DERIVED and re-derived on every store change, so an index goes stale the
// moment a line moves) — there is simply a LIST of them now.  Every stored
// centroid is resolved independently, so a region that stops existing drops out of
// the set on its own and takes nothing else with it.
//
// The single-selection API is UNCHANGED in meaning: KiwiRegion_Select replaces the
// whole set with one member, and KiwiRegion_SelectedIndex answers the PRIMARY (the
// first member that still resolves), which for a one-member set is exactly what it
// always returned.  Nothing that predates this round has to know the set exists.
void KiwiRegion_Select( int index );        // -1 clears; REPLACES the set
void KiwiRegion_ToggleSelect( int index );  // Shift+click: in if out, out if in
void KiwiRegion_ClearSelection();
int  KiwiRegion_SelectedIndex();            // -1 = nothing selected / it is gone
bool KiwiRegion_HasSelection();
int  KiwiRegion_SelectedCount();            // members that still resolve
int  KiwiRegion_SelectedAt( int which );    // 0..count-1 -> live region index, or -1
bool KiwiRegion_IsSelected( int index );

// The translucent fill, from the Cam_Draw tail (KiwiCon_DrawWorld calls it so
// the whole construction pass is one hook).  This is ALSO where the shakeout-F
// flash counters are decremented — one call per drawn frame, which is exactly
// what KREG_FLASH_FRAMES counts.
void KiwiRegion_DrawFills( int highlightIndex );

// ── SHAKEOUT F: the chain walker, EXPORTED ──────────────────────────────────
// PASS 2's endpoint-graph walk, lifted out of BuildRegions so "Join Lines"
// (kiwi_conselect.cpp) chains exactly the way a region does.  There is one
// chaining rule in this codebase and this is it; a second copy would drift, and
// then a set of lines would join into a polyline that does NOT become a region,
// which is the worst possible failure mode for a feature whose entire point is
// that closing a loop makes a face.
//
// SHAKEOUT H: the walker no longer takes a PLANE.  It welded endpoints in plane
// space, which meant it needed a plane before it could tell whether two lines even
// met — and with a world-space store that plane may not exist yet (it is derived
// FROM the chain the walker is being asked to find).  Welding in 3D world distance
// removes the circularity, is the same number (KREG_JOIN_DIST), and is strictly
// tighter: two ends that projected onto the same plane point while sitting 40
// units apart along the normal used to weld, and now do not.
//
// Semantics, otherwise unchanged from the code it replaces:
//   * only the FIRST and LAST tessellated vertex of each object are nodes;
//   * endpoints within KREG_JOIN_DIST of each other (in 3D) weld into ONE node;
//   * an object whose two ends already coincide is DROPPED from the graph (it
//     closes on itself — that is PASS 1's business, not a chain link) and so it
//     never appears in `outSteps`;
//   * a node of degree 3+ is AMBIGUOUS and fails the whole walk — guessing a
//     branch is how a loop silently becomes the wrong loop;
//   * success additionally requires that ONE walk consumes EVERY graph edge, so
//     a set that splits into two separate chains fails rather than returning
//     half of itself.
// The walk starts at a degree-1 node when there is one (so an OPEN chain comes
// back in its natural end-to-end order) and at any node otherwise.
struct kchainStep_t
{
    int  object;        // index into the construction store
    bool forward;       // false = walk this object's vertices in reverse
};

bool KiwiRegion_ChainWalk( const int *objects, int count,
                           std::vector<kchainStep_t> *outSteps, bool *outClosed );

// SHAKEOUT H: KiwiRegion_SamePlane IS GONE.  It was the §8 plane-vs-plane
// coplanarity test, and its two callers — PASS 2's grouping and Join's precondition
// — have both become point-vs-plane questions (see the note above and the JOIN note
// in kiwi_conselect.cpp).  With no callers left it was deleted rather than kept as
// a documented primitive nobody uses.  KREG_PLANE_DOT / KREG_PLANE_DIST went with
// it; §8's coplanarity tolerance is KCON_PLANE_FIT_DIST now, in one place.

// SHAKEOUT H: do ALL of `o`'s tessellated points lie on `plane`, within
// KCON_PLANE_FIT_DIST?  This is the grouping test PASS 2 now uses (see the note
// above), exported because Join asks the same question about a candidate set.
bool KiwiRegion_ObjectOnPlane( const kconObject_t &o, const kconPlane_t &plane );

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AR, ITEM 2 — THE ONE RING SANITIZER (the standing region invariant)
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "why does this bool diff fail? see the 2 pics.  The 2nd
// pic fails while the 1st pic works.  Why?  The only different is I joined the
// polyline on the 2nd one.  When i bool it into the pyramid it fails!"
//
// THE INVARIANT, stated once so it can be checked: **a joined outline and the
// unjoined outline it was made from must derive the SAME region — the same fill,
// the same extrude and the same cut.**  Join moves bookkeeping, not geometry.
//
// The three derivation routes reach different code: a JOINED closed polyline is
// PASS 1, an unjoined chain is PASS 2, and a set that only meets at crossings is
// PASS 3's arrangement.  Before this round they shared their cleaning only because
// all three happened to call AcceptLoop, and the two cleaning steps were written
// out inline there where a fourth caller could acquire one and not the other.
// This is that cleaning as ONE function:
//
//   1. dedupe at the weld, bounded by the ring's own finest edge (round AG);
//   2. drop collinear runs at §19's own KVALID_PLANE_DOT (round R) — the shape
//      that becomes TWO IDENTICAL PRISM SIDE PLANES and is refused as "duplicate
//      plane";
//   3. dedupe again, because step 2 can bring two survivors within the weld.
//      Step 3 is NEW: neither pass removed that shape before, and it is exactly
//      what a near-tangent arc/line junction leaves behind.
//
// Winding is NOT done here — RewindCCW is an ACCEPTANCE decision (it needs the
// signed area the gate re-reads) and stays with the other gates.
//
// The ring is plane-space, 2 floats per vertex, first point NOT repeated.  It may
// come back with fewer than 3 points, which every caller must treat as "not a
// region"; that refusal is the caller's, not this function's.
void KiwiRegion_SanitizeRing( std::vector<float> &pts );

// ── the 2D toolkit (§23) ─────────────────────────────────────────────────────
// Signed area of a plane-space loop; > 0 == CCW.
float KiwiRegion_SignedArea( const std::vector<float> &pts );

// True when the loop has a self-intersection (non-adjacent segments crossing).
bool KiwiRegion_SelfIntersects( const std::vector<float> &pts );

// True when the loop is convex (all cross products of consecutive edges share a
// sign; exactly-collinear turns are tolerated).
bool KiwiRegion_IsConvex( const std::vector<float> &pts );

// Ear-clip triangulation.  `outTris` gets 3 vertex indices per triangle.  False
// on a degenerate or self-intersecting loop.  The input must be CCW.
bool KiwiRegion_Triangulate( const std::vector<float> &pts, std::vector<int> *outTris );

// Triangulate, then Hertel-Mehlhorn-merge into convex pieces.  Each piece is a
// CCW index list into `pts`.  False when triangulation failed.
bool KiwiRegion_ConvexPieces( const std::vector<float> &pts,
                              std::vector< std::vector<int> > *outPieces );

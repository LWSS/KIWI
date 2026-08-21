#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_autobool.cpp — KIWI-UX ROUND AB, ITEM 3.  See kiwi_autobool.h for what
// this is, what decides a merge (Brush_MergeList, not this file) and the limits.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include <vector>
#include <math.h>              // fabs — the cluster phase's volume comparison

#include "kiwi_autobool.h"
#include "kiwi_csg.h"                 // KIWI-UX (CLEANUP, B-10): KiwiCsg_BrushUsable
#include "kiwi_command.h"      // KIWI_CMD_AUTO_BOOL + KiwiCmd_UndoBegin / KiwiCmd_UndoCommit
#include "kiwi_selection.h"    // KiwiSel / Sel_Clear / Sel_BrushLive
#include "kiwi_selext.h"       // KSELX_TOUCH_EPS

// ── ported entry points (each verified against its definition) ───────────────
extern int         Sys_Printf( const char *fmt, ... );                        // win_qe3.cpp
extern int         g_nUpdateBits;                                             // 0x25D5A74 (mainfrm.cpp)
// The merge CORE.  Takes a NULL-terminated list threaded through selbrush_t.next
// (NOT the circular display list) and returns a brand-new selbrush_t for the merged
// brush, or nullptr when the union would not be convex.  Does not free the inputs.
extern selbrush_t *Brush_MergeList( selbrush_t *brushList );                  // csg.cpp:409   (0x47D600)
extern void        Brush_RemoveFromList( selbrush_t *b );                     // brush.cpp:972 (0x476680)
extern void        Brush_AddToList2( selbrush_t *b );                         // brush.cpp:927 (0x4765A0)
extern void        Brush_Free( selbrush_t *b );                               // brush.cpp:1002 (0x475BA0)
// KIWI-UX (CLEANUP, B-28): FILE SCOPE, not block scope.  Round AI shipped a link
// error from a block-scope extern that MSVC mangled with its enclosing namespace;
// kiwi_uv.cpp carries the full account.  This is the declaration that used to sit
// inside KiwiAutoBool_RegisterCommands.
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods,
                                            int commandId );                  // mainfrm.cpp:1340
// selected_brushes is the qe3.h sentinel (qe3.h:1054); iterate it as
// `for ( b = selected_brushes.next; b != &selected_brushes; b = b->next )` (qe3.h:374).

namespace
{
    // A cascade cannot run forever — Brush_MergeList strictly reduces the brush count
    // on every success, so the loop terminates on its own; this only bounds the
    // WASTED work if a future change ever broke that.  N passes over N brushes is
    // already far more than the greedy walk can need.
    const int KAB_MAX_PASSES = 256;

    // ── ROUND AN: the cluster phase ──────────────────────────────────────────
    // The smallest set the cluster phase will hand to the core.  TWO is not a
    // cluster: the pairwise fixed point has already tried every touching pair and
    // refused the ones it refused, so re-offering a pair would only re-run the same
    // decision with a volume guard bolted on.  THREE is the first size where
    // Brush_MergeList can see something the pairwise walk structurally cannot.
    const size_t KAB_MIN_CLUSTER = 3;

    // Volume-conservation tolerance, RELATIVE to the pre-merge total.  0.1% is
    // deliberately loose on the noise side and still absurdly tight on the signal
    // side: the drift it has to forgive is a few float ULPs per winding vertex
    // (~1e-6 relative once the sum is shifted to the cluster centre and accumulated
    // in double, see BrushVolume), while the thing it has to catch is a hull that
    // swallowed the gap between two brushes — a whole room's worth of volume, tens
    // of percent, never tenths of one.  There is a wide empty band between the two,
    // and sitting in the middle of it is worth more than a tight number that might
    // reject a legitimate merge on a rebuilt winding.
    const double KAB_VOLUME_REL_EPS = 0.001;

    // ...and the floor under it, in cubic world units, for the degenerate case where
    // the pre-merge total is itself near zero and a relative test means nothing.  One
    // cubic unit is far below anything that can be built on the grid and far above
    // the float noise the relative term exists to absorb.
    const double KAB_VOLUME_ABS_EPS = 1.0;

    // ── validation: CSG_Merge's own, per brush (csg.cpp:589-603) ─────────────
    // KIWI-UX (CLEANUP, B-10): this body was byte-identical to kiwi_csg.cpp's
    // CsgUsable; both are KiwiCsg_BrushUsable (kiwi_csg.h) now.  The forwarder
    // keeps this file's own name at its own call sites.
    inline bool Usable( const selbrush_t *b ) { return KiwiCsg_BrushUsable( b ); }

    const entity_s *OwnerDef( const selbrush_t *b )
    {
        return ( b && b->owner ) ? b->owner->def : 0;
    }

    // Epsilon-padded bounds overlap — the ported Select_Touching_R test
    // (select.cpp:1678-1679, and kiwi_selext.cpp's BoundsTouch, which is file-local
    // there).  A STRICT overlap test would be wrong here and not merely slower: the
    // pairs Brush_MergeList accepts are the ones that share a face, i.e. that abut
    // exactly, and a strict test rejects every single one of them.
    // def->[mins,maxs] is kept current by Brush_BuildWindings (brush.cpp:1459-1463),
    // so this is a read and not a computation.
    bool BoundsTouch( const brush_t *a, const brush_t *b )
    {
        for ( int i = 0; i < 3; ++i )
        {
            if ( a->maxs[i] + KSELX_TOUCH_EPS < b->mins[i] ) return false;
            if ( a->mins[i] - KSELX_TOUCH_EPS > b->maxs[i] ) return false;
        }
        return true;
    }

    // ── ROUND AN: winding volume, the cluster phase's ONLY guard ─────────────
    // The cluster phase hands Brush_MergeList three or more brushes at once, and that
    // is precisely the case the header's last limit warns about (and the round AB
    // finding recorded there): the core calls a face INNER whenever ANY OTHER brush in
    // the set carries a flipped-equal plane, WITHOUT checking that the two faces
    // geometrically touch.  Two brushes sitting on the same wall plane but a room
    // apart therefore cancel each other's walls; both faces are dropped from the outer
    // set; and the "merged" brush is a hull that has SWALLOWED the empty space between
    // them.  Winding_PlanesConcave never sees it, because the faces it would have
    // rejected on are exactly the ones that got classified away.  For a PAIR that
    // misfire is rare enough that the pairwise phase ships without a guard.  For a SET
    // it is the normal failure, not the exotic one, so the cluster phase does not get
    // to merge anything it cannot first prove.
    //
    // The proof is TOTAL VOLUME CONSERVATION.  A legitimate union of solids that meet
    // only at shared faces has exactly the sum of their volumes (the shared faces are
    // measure zero); a hull that swallowed a void is strictly bigger.  So: sum the
    // members before, measure the merged brush after, accept only if they agree.
    //
    // Volume comes from the windings by the divergence theorem on the closed surface:
    //
    //     V = (1/3) * SUM over faces of ( centroid(face) . normal(face) ) * area(face)
    //
    // accumulated one FAN TRIANGLE at a time from w->p[0] — exact for the convex
    // windings Brush_BuildWindings produces, and the same fan kiwi_validity.cpp:42's
    // WindingArea walks.  Each triangle's term is the integral of (x . n) over that
    // triangle and integrals add, so the fan sum IS the face's term; there is no
    // separate centroid pass.
    //
    // TWO NUMERICAL PRECAUTIONS, both load-bearing:
    //  * every point is taken RELATIVE TO `ref`.  V is translation invariant for a
    //    closed surface (the n dA integral over one is zero), so this changes no
    //    answer — but the UNSHIFTED sum multiplies world coordinates (up to the
    //    editor's own 131072 sentinel box) by face areas and then cancels the products
    //    back down to the brush's own comparatively tiny volume, which is textbook
    //    catastrophic cancellation in float.  Shifting to the cluster's centre keeps
    //    every magnitude at the cluster's own scale, where float has digits to spare.
    //  * the accumulator is DOUBLE.  The inputs are floats; the running sum is not.
    //
    // Returns false — read as "cannot be measured, so refuse the merge" — for a brush
    // with fewer than 4 faces or with any missing or degenerate face winding.  An open
    // surface does not make the sum inaccurate, it makes it meaningless, and the safe
    // reading of "I cannot verify this" is "then do not do it".
    bool BrushVolume( const brush_t *def, const float ref[3], double *outVolume )
    {
        *outVolume = 0.0;
        if ( !def || !def->faces || def->faceCount < 4 )
            return false;                      // not a closed solid; nothing to measure

        double total = 0.0;                    // == 3V when the loop finishes
        for ( int f = 0; f < def->faceCount; ++f )
        {
            const face_t   *fc = &def->faces[f];
            const winding_t *w = fc->w;
            if ( !w || w->numpoints < 3 )
                return false;                  // a hole in the surface

            const float *n = fc->plane.normal;
            const double p0[3] = { (double)w->p[0][0] - ref[0],
                                   (double)w->p[0][1] - ref[1],
                                   (double)w->p[0][2] - ref[2] };

            for ( int i = 1; i + 1 < w->numpoints; ++i )
            {
                const double pi[3] = { (double)w->p[i][0] - ref[0],
                                       (double)w->p[i][1] - ref[1],
                                       (double)w->p[i][2] - ref[2] };
                const double pj[3] = { (double)w->p[i + 1][0] - ref[0],
                                       (double)w->p[i + 1][1] - ref[1],
                                       (double)w->p[i + 1][2] - ref[2] };

                const double u[3] = { pi[0] - p0[0], pi[1] - p0[1], pi[2] - p0[2] };
                const double v[3] = { pj[0] - p0[0], pj[1] - p0[1], pj[2] - p0[2] };
                const double c[3] = { u[1] * v[2] - u[2] * v[1],
                                      u[2] * v[0] - u[0] * v[2],
                                      u[0] * v[1] - u[1] * v[0] };

                // UNSIGNED triangle area (half the cross-product magnitude), so the
                // term does not depend on which way round the winding was wound —
                // only on the face plane's own outward normal, which is the thing the
                // divergence theorem actually asks for.
                const double area = 0.5 * sqrt( c[0] * c[0] + c[1] * c[1] + c[2] * c[2] );
                if ( area <= 0.0 )
                    continue;                  // degenerate sliver; contributes nothing

                const double cx = ( p0[0] + pi[0] + pj[0] ) / 3.0;
                const double cy = ( p0[1] + pi[1] + pj[1] ) / 3.0;
                const double cz = ( p0[2] + pi[2] + pj[2] ) / 3.0;
                total += ( cx * n[0] + cy * n[1] + cz * n[2] ) * area;
            }
        }

        // fabs, not a sign assumption: the magnitude is the volume whether the face
        // normals of this port's brushes point out of the solid or into it, and this
        // routine only ever compares one magnitude against another.
        *outVolume = fabs( total ) / 3.0;
        return true;
    }

    // ── ROUND AN: touching-graph components ──────────────────────────────────
    // Union-find with path HALVING (one extra store per step, no recursion, no second
    // pass).  The graph is tiny — one node per selected brush — so the constant factor
    // is irrelevant and the readability is not.
    int UF_Find( std::vector<int> &parent, int x )
    {
        while ( parent[x] != x )
        {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }

    // ── ONE pair attempt ─────────────────────────────────────────────────────
    // A faithful transcription of CSG_Merge's body (csg.cpp:605-648) restricted to
    // two brushes: unlink both into a NULL-terminated merge list threaded through
    // their own `.next`, hand it to the core, then either free both and link the
    // result, or link both back untouched.  Returns the merged instance, or nullptr
    // when the core refused — in which case `a` and `b` are still live, still on
    // selected_brushes, and completely unmodified.
    selbrush_t *TryMergePair( selbrush_t *a, selbrush_t *b )
    {
        // Built by PREPENDING in selection order, byte for byte as CSG_Merge does
        // (csg.cpp:607-616) — so the LATER brush ends up at the head, which is where
        // Brush_MergeList reads the new brush's owner from (csg.cpp:554-555).  The
        // owner is the same for both by this point (validated in RunAutoBool), but
        // the head also fixes the face-iteration order the core's duplicate-plane
        // skip depends on, so the direction is reproduced rather than assumed
        // irrelevant.
        selbrush_t *mergeList = nullptr;
        selbrush_t *pair[2] = { a, b };
        for ( int k = 0; k < 2; ++k )
        {
            selbrush_t *n = pair[k];
            Brush_RemoveFromList( n );
            n->next = mergeList;
            n->prev = nullptr;
            mergeList = n;
        }

        selbrush_t *merged = Brush_MergeList( mergeList );

        if ( merged )
        {
            for ( selbrush_t *cur = mergeList; cur; )
            {
                selbrush_t *next = cur->next;
                cur->next = nullptr;
                cur->prev = nullptr;
                Brush_Free( cur );
                cur = next;
            }
            // Brush_AddToList2 hard-errors on an already-linked node (brush.cpp:923);
            // Brush_MergeList's Brush_AddToList leaves the new instance UNLINKED from
            // the display lists, so this is the link.
            Brush_AddToList2( merged );
            return merged;
        }

        for ( selbrush_t *cur = mergeList; cur; )
        {
            selbrush_t *next = cur->next;
            cur->next = nullptr;
            cur->prev = nullptr;
            Brush_AddToList2( cur );
            cur = next;
        }
        return nullptr;
    }

    // ── ROUND AN: ONE cluster attempt ────────────────────────────────────────
    // TryMergePair widened to N members and given the volume guard.  The surgery is
    // the same and in the same order, with ONE deliberate difference: the originals
    // are NOT freed until the guard has passed.  That is what makes "restore the
    // pre-merge state" trivial rather than impossible — on either kind of rejection
    // every member is still a live, unmodified brush that only has to be linked back
    // onto selected_brushes, exactly as the core-refusal path already does.
    //
    // Returns  1  accepted   (*outMerged is the new brush, already on the list)
    //          0  refused by Brush_MergeList itself (non-convex hull)
    //         -1  rejected by the volume guard (or unmeasurable, which counts as a
    //             rejection — see BrushVolume)
    // In every non-1 case the members are back on selected_brushes untouched.
    int TryMergeCluster( selbrush_t *const *members, size_t count, selbrush_t **outMerged )
    {
        *outMerged = nullptr;
        if ( !members || count < 2 )
            return 0;

        // 1. Reference point + pre-merge volume, computed BEFORE anything is
        //    unlinked, so an unmeasurable member costs nothing to back out of.
        //    `ref` is the centre of the cluster's own bounding box; see BrushVolume
        //    for why the sum is taken relative to it.
        float mins[3] = {  1.0e30f,  1.0e30f,  1.0e30f };
        float maxs[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
        for ( size_t m = 0; m < count; ++m )
        {
            const brush_t *d = members[m] ? members[m]->def : 0;
            if ( !d )
                return -1;
            for ( int i = 0; i < 3; ++i )
            {
                if ( d->mins[i] < mins[i] ) mins[i] = d->mins[i];
                if ( d->maxs[i] > maxs[i] ) maxs[i] = d->maxs[i];
            }
        }
        const float ref[3] = { 0.5f * ( mins[0] + maxs[0] ),
                               0.5f * ( mins[1] + maxs[1] ),
                               0.5f * ( mins[2] + maxs[2] ) };

        double volBefore = 0.0;
        for ( size_t m = 0; m < count; ++m )
        {
            double v = 0.0;
            if ( !BrushVolume( members[m]->def, ref, &v ) )
                return -1;                     // unmeasurable: refuse, having changed nothing
            volBefore += v;
        }

        // 2. Unlink every member into a NULL-terminated merge list threaded through
        //    its own .next, PREPENDING in order exactly as CSG_Merge does
        //    (csg.cpp:607-616) and for the same two reasons TryMergePair spells out:
        //    the head decides the merged brush's owner (csg.cpp:554-555) and fixes the
        //    face-iteration order the core's duplicate-plane skip depends on.
        selbrush_t *mergeList = nullptr;
        for ( size_t m = 0; m < count; ++m )
        {
            selbrush_t *n = members[m];
            Brush_RemoveFromList( n );
            n->next = mergeList;
            n->prev = nullptr;
            mergeList = n;
        }

        selbrush_t *merged = Brush_MergeList( mergeList );

        // Which of the two refusals this was, remembered before `merged` is cleared:
        // the core's own non-convex verdict, or the volume guard's.  They restore
        // identically but they are reported separately, because "your selection is not
        // convex" and "the merge would have swallowed a void" are different facts about
        // the map and only the second one is a limitation of Brush_MergeList.
        const bool coreRefused = ( merged == nullptr );

        // 3. The guard.  Brush_MergeList already ran Brush_BuildWindings on the new
        //    brush (csg.cpp:561), so its face windings are current and measurable.
        bool accept = false;
        if ( merged )
        {
            double volAfter = 0.0;
            if ( BrushVolume( merged->def, ref, &volAfter ) )
            {
                double tol = volBefore * KAB_VOLUME_REL_EPS;
                if ( tol < KAB_VOLUME_ABS_EPS )
                    tol = KAB_VOLUME_ABS_EPS;
                accept = ( fabs( volAfter - volBefore ) <= tol );
            }
        }

        if ( merged && !accept )
        {
            // Discard the hull.  Brush_Free skips its Brush_RemoveFromList head when
            // the instance has no links (brush.cpp:1001), and Brush_MergeList's
            // Brush_AddToList leaves the new instance UNLINKED from the display lists,
            // so this is the whole cleanup: the instance goes, its def's refCount
            // drops through Entity_UnlinkBrush_def to zero, and Brush_Free_R takes the
            // def.  Nothing outside this function ever saw it, and it was created and
            // destroyed inside the undo bracket, so there is nothing for an undo to
            // restore either (the same argument RunAutoBool makes for intermediates).
            Brush_Free( merged );
            merged = nullptr;
        }

        if ( !merged )
        {
            for ( selbrush_t *cur = mergeList; cur; )
            {
                selbrush_t *next = cur->next;
                cur->next = nullptr;
                cur->prev = nullptr;
                Brush_AddToList2( cur );
                cur = next;
            }
            return coreRefused ? 0 : -1;
        }

        for ( selbrush_t *cur = mergeList; cur; )
        {
            selbrush_t *next = cur->next;
            cur->next = nullptr;
            cur->prev = nullptr;
            Brush_Free( cur );
            cur = next;
        }
        Brush_AddToList2( merged );
        *outMerged = merged;
        return 1;
    }

    // ── the greedy pairwise cascade, in a given index ORDER ──────────────────
    // ROUND AN: this is the original step-4 loop verbatim, with the two index walks
    // taken through `order` instead of straight through `set`.  Passing the identity
    // order reproduces the old behaviour byte for byte; passing the reversed order is
    // the reverse sweep.
    //
    // The reverse sweep is not decoration.  Brush_MergeList's greedy result is
    // ORDER-DEPENDENT (the header's second limit): merging A+B first can make A+B+C
    // impossible where B+C first would have allowed it.  A second sweep from the other
    // end therefore reaches pairs the forward one had already spoiled — and because
    // every merge is still individually validated by the core, a second sweep can only
    // ever find MORE merges, never a wrong one.  It also flips the argument order into
    // TryMergePair, so the merge list's head (and with it the face-iteration order the
    // core's duplicate-plane skip depends on) is the other brush.
    int PairwiseFixedPoint( std::vector<selbrush_t *> &set, bool reverse )
    {
        const size_t n = set.size();
        if ( n < 2 )
            return 0;

        std::vector<size_t> order( n );
        for ( size_t k = 0; k < n; ++k )
            order[k] = reverse ? ( n - 1 - k ) : k;

        // ONE O(n^2) sweep per pass, repeated while the previous pass changed
        // anything.  A successful merge does NOT restart the sweep: `a` simply
        // becomes the merged brush and the inner loop carries on from `oj`, so a run
        // of collinear boxes collapses inside a single pass instead of costing one
        // pass each.  The outer repeat is still needed because a merge can make an
        // EARLIER pair viable that was refused before it.
        //
        // Sel_BrushLive (rule 11) is swept ONCE PER PASS rather than per compare: it
        // is a linear walk of the whole map's display lists, and calling it inside
        // the inner loop would make the run O(n^2 * mapsize).  Sweeping per pass is
        // the same guarantee — nothing outside this loop can free a brush while the
        // bracket is open — at a cost that stays bounded.
        int merges = 0;
        const int maxPasses = ( (int)n < KAB_MAX_PASSES ) ? (int)n + 1 : KAB_MAX_PASSES;
        std::vector<bool> live( n, false );
        for ( int pass = 0; pass < maxPasses; ++pass )
        {
            for ( size_t k = 0; k < n; ++k )
                live[k] = ( set[k] != 0 && Sel_BrushLive( set[k] ) && set[k]->def != 0 );

            bool changed = false;
            for ( size_t oi = 0; oi < n; ++oi )
            {
                const size_t i = order[oi];
                if ( !live[i] )
                    continue;
                for ( size_t oj = oi + 1; oj < n; ++oj )
                {
                    const size_t j = order[oj];
                    if ( !live[j] )
                        continue;
                    selbrush_t *a = set[i];
                    selbrush_t *b = set[j];
                    if ( !BoundsTouch( a->def, b->def ) )
                        continue;              // cannot possibly share a face

                    selbrush_t *merged = TryMergePair( a, b );
                    if ( !merged )
                        continue;              // refused; both are untouched

                    // Both inputs are freed.  Retire j's slot and put the result in
                    // i's, so it is immediately a candidate for the next merge in
                    // this same sweep — that cascade IS the feature.
                    set[i]  = merged;
                    set[j]  = 0;
                    live[j] = false;
                    ++merges;
                    changed = true;
                }
            }
            if ( !changed )
                break;                          // fixed point
        }
        return merges;
    }

    // ── ROUND AN: the cluster phase ──────────────────────────────────────────
    // USER REQUEST, verbatim: *"Make it try combining all the solids at once along
    // with more combinations."*
    //
    // After the pairwise fixed point has taken everything it can two at a time, build
    // the TOUCHING GRAPH of what is left — one node per surviving brush, an edge
    // wherever BoundsTouch says two of them abut within the ported Select_Touching_R
    // epsilon — and hand each connected component of 3 or more members to
    // Brush_MergeList AS A WHOLE.  That is the "all at once" the request asks for, and
    // it is strictly more than the pairwise walk can reach: a plus-shaped set of five
    // boxes has no two members whose union is convex, but all five together are one
    // convex brush.  The pairwise phase refuses every pair and leaves five; the cluster
    // phase returns one.
    //
    // Components are the right unit of work rather than "the whole selection at once"
    // because two brushes that do not even touch can never be part of one convex solid,
    // so putting them in the same set can only ever poison it — and poisoning it is not
    // hypothetical, it is exactly the inner-face misclassification BrushVolume exists
    // to catch.  Splitting on the touching graph removes the easiest way to trigger it
    // before the guard is ever consulted.
    //
    // ONE pass, not a fixed point: components are disjoint by construction, so merging
    // one cannot make another viable.  What a REJECTED component might still yield is
    // left to the reverse sweep that follows, which is cheaper than a search and cannot
    // be surprising.
    // Returns the BRUSH-COUNT reduction (members - 1 per accepted component); the three
    // out-params are the per-component tally the report prints — how many components of
    // 3+ existed at all, how many merged, and how many the volume guard threw out.  A
    // component that is neither accepted nor volume-rejected was refused by
    // Brush_MergeList itself on convexity, which is not a KIWI decision and not news.
    int ClusterPhase( std::vector<selbrush_t *> &set,
                      int *outTried, int *outAccepted, int *outRejected )
    {
        *outTried    = 0;
        *outAccepted = 0;
        *outRejected = 0;

        const size_t n = set.size();
        if ( n < KAB_MIN_CLUSTER )
            return 0;

        // Rule 11 again: one Sel_BrushLive sweep for the whole phase, for the same
        // reason PairwiseFixedPoint sweeps once per pass — nothing outside this run
        // can free a brush while the bracket is open, and the check is a walk of the
        // map's display lists.  The slots this phase retires are cleared in `live`
        // as it goes, so a member consumed by one component is never offered to a
        // later one.
        std::vector<bool> live( n, false );
        for ( size_t k = 0; k < n; ++k )
            live[k] = ( set[k] != 0 && Sel_BrushLive( set[k] ) && set[k]->def != 0 );

        std::vector<int> parent( n );
        for ( size_t k = 0; k < n; ++k )
            parent[k] = (int)k;

        for ( size_t i = 0; i < n; ++i )
        {
            if ( !live[i] )
                continue;
            for ( size_t j = i + 1; j < n; ++j )
            {
                if ( !live[j] )
                    continue;
                if ( !BoundsTouch( set[i]->def, set[j]->def ) )
                    continue;
                const int ri = UF_Find( parent, (int)i );
                const int rj = UF_Find( parent, (int)j );
                // Union by SMALLEST INDEX, not by rank: it costs nothing at this size
                // and it makes the root of every component its lowest member, so the
                // component's representative slot below is also its first slot and the
                // merged brush lands where a reader would expect it to.
                if ( ri != rj )
                {
                    if ( ri < rj ) parent[rj] = ri;
                    else           parent[ri] = rj;
                }
            }
        }

        int merges = 0;
        std::vector<selbrush_t *> members;
        std::vector<size_t>       slots;
        for ( size_t r = 0; r < n; ++r )
        {
            if ( !live[r] || (size_t)UF_Find( parent, (int)r ) != r )
                continue;                       // component ROOTS only

            members.clear();
            slots.clear();
            for ( size_t k = 0; k < n; ++k )
            {
                if ( live[k] && (size_t)UF_Find( parent, (int)k ) == r )
                {
                    members.push_back( set[k] );
                    slots.push_back( k );
                }
            }
            if ( members.size() < KAB_MIN_CLUSTER )
                continue;                       // pairs are the pairwise phase's job

            ++( *outTried );
            selbrush_t *merged = nullptr;
            const int outcome = TryMergeCluster( &members[0], members.size(), &merged );
            if ( outcome == 1 )
            {
                ++( *outAccepted );
                // The whole component became one brush.  Keep it in the FIRST slot so
                // the reverse sweep sees it in the position the component occupied,
                // and retire the rest.
                set[slots[0]] = merged;
                for ( size_t k = 1; k < slots.size(); ++k )
                {
                    set[slots[k]]  = 0;
                    live[slots[k]] = false;
                }
                merges += (int)members.size() - 1;
            }
            else if ( outcome < 0 )
            {
                ++( *outRejected );
            }
        }
        return merges;
    }

    // ── the greedy cascade ───────────────────────────────────────────────────
    void RunAutoBool()
    {
        // 1. Gather + validate the candidate set BEFORE anything mutates.
        std::vector<selbrush_t *> set;
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            set.push_back( b );

        if ( set.size() < 2 )
        {
            Sys_Printf( "Auto-bool: select 2 or more brushes.\n" );
            return;
        }
        const entity_s *ownerDef0 = OwnerDef( set[0] );
        if ( !ownerDef0 )
        {
            Sys_Printf( "Auto-bool: the selection has no owning entity.\n" );
            return;
        }
        for ( size_t i = 0; i < set.size(); ++i )
        {
            if ( !Usable( set[i] ) )
            {
                Sys_Printf( "Auto-bool: the selection contains a patch or a fixed-size "
                            "entity; CSG merge cannot consume those.\n" );
                return;
            }
            if ( OwnerDef( set[i] ) != ownerDef0 )
            {
                Sys_Printf( "Auto-bool: the selection spans more than one entity; "
                            "merge only works within one.\n" );
                return;
            }
        }

        const int before = (int)set.size();

        // 2. Drop the TYPED selection before any brush is freed.  Every stored
        //    sel_item_t holds a selbrush_t* that this run will invalidate; the rule
        //    is stated at kiwi_boolean.cpp:954-957 and kiwi_join.cpp:124-127.  The
        //    LEGACY list stays exactly as it is — it is both the undo bracket's
        //    subject and the working set.
        Sel_Clear( KiwiSel() );

        // 3. ONE bracket for the whole consolidation.  Undo_AddBrushList inside
        //    KiwiCmd_UndoBegin deep-clones every def on selected_brushes, i.e. every
        //    original, which is exactly the set this run consumes.  Merged brushes
        //    created DURING the bracket need no Undo_AddBrush of their own:
        //     * an INTERMEDIATE (merged in pass k, consumed in pass k+1) is created
        //       and destroyed inside the record and did not exist before it, so
        //       there is nothing for an undo to restore;
        //     * a FINAL merged brush is on selected_brushes at commit, so
        //       Undo_EndBrushList stamps it with the record id and the undo removes
        //       it (undo.cpp:576-593).
        //    This is the difference from kiwi_boolean.cpp's carve, which DOES need
        //    manual covers (kiwi_boolean.cpp:970-999) — its tools are freed while
        //    OFF the list the bracket head walked.  Nothing here is.
        //    The literal is required to be static: Undo_GeneralStart stores the
        //    POINTER (kiwi_command.h:139-164).
        KiwiCmd_UndoBegin( "auto bool" );

        // 4. THREE PHASES, all inside the one bracket (rule: one gesture, one undo).
        //    They run in this order for a reason and not merely by accretion:
        //
        //     A. PAIRWISE, forward.  Cheapest, safest, needs no guard, and it is the
        //        phase that does the bulk of the work on ordinary selections.  It also
        //        makes the cluster phase's job smaller and its graph sparser.
        //     B. CLUSTERS.  Only what pairs could not take gets offered whole, so the
        //        risky path sees the fewest brushes it possibly can.
        //     C. PAIRWISE, reversed.  Picks up what A spoiled by its own greed and
        //        what B's rejected components still had in them.
        //
        //    Every phase leaves `set` in the same shape: surviving brushes in their
        //    slots, retired slots nulled.  No raw brush_t* survives a phase boundary
        //    unrevalidated — each phase re-runs its own Sel_BrushLive sweep on entry
        //    (rule 11), which is what makes them safe to chain in any order.
        const int pairMerges = PairwiseFixedPoint( set, false );

        int clustersTried    = 0;
        int clustersAccepted = 0;
        int clusterRejected  = 0;
        const int clusterMerges = ClusterPhase( set, &clustersTried,
                                                &clustersAccepted, &clusterRejected );

        const int reverseMerges = PairwiseFixedPoint( set, true );

        const int merges = pairMerges + clusterMerges + reverseMerges;

        // 5. Close the ONE record.
        KiwiCmd_UndoCommit();

        int after = 0;
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            ++after;

        g_nUpdateBits = -1;                     // CSG_Merge's own success dirty (csg.cpp:634)

        // Per-phase reporting, because the phases fail in different ways and a single
        // total hides which one earned its keep on this selection.  The rejection count
        // is the interesting number: a non-zero one means Brush_MergeList offered a hull
        // that would have swallowed empty space and the guard caught it, which is worth
        // seeing rather than silently discarding.
        if ( clustersTried > 0 )
            Sys_Printf( "Auto-bool: pairwise merged %i, clusters merged %i of %i "
                        "component%s (%i rejected on volume), reverse sweep merged %i; "
                        "%i brushes -> %i.\n",
                        pairMerges, clustersAccepted, clustersTried,
                        ( clustersTried == 1 ) ? "" : "s", clusterRejected,
                        reverseMerges, before, after );
        else
            Sys_Printf( "Auto-bool: pairwise merged %i, no cluster of 3+ touching "
                        "brushes was left to try, reverse sweep merged %i; "
                        "%i brushes -> %i.\n",
                        pairMerges, reverseMerges, before, after );

        if ( !merges )
            Sys_Printf( "Auto-bool: no pair and no cluster formed a single convex "
                        "brush. Nothing was changed.\n" );
    }
}

// ─── §3 palette predicate ────────────────────────────────────────────────────
bool KiwiAutoBool_CanExecute()
{
    // KIWI-UX (CLEANUP, B-10): this was the same ">= 2 / all usable / one owner"
    // walk KiwiCsg_CanMerge runs, with the owner read spelled through a local
    // OwnerDef() instead of inline.  Same answer, one implementation.
    return KiwiCsg_SelectionMergeable();
}

// ─── dispatch + registration ─────────────────────────────────────────────────
bool KiwiAutoBool_DispatchInstant( unsigned int commandId )
{
    if ( commandId != (unsigned int)KIWI_CMD_AUTO_BOOL )
        return false;
    RunAutoBool();
    return true;
}

void KiwiAutoBool_RegisterCommands()
{
    // UNBOUND on purpose (vk 0), like KIWI_CMD_MATINFO and KIWI_CMD_FILLET_EDGE.
    // The user's own framing was "might be a bad idea, but we can try it": a verb
    // that rewrites a whole selection's topology does not get a key it can be hit
    // by accident.  Palette / CSG panel only.
    Radiant_RegisterCommand( "KiwiAutoBool", 0, 0, KIWI_CMD_AUTO_BOOL );
}

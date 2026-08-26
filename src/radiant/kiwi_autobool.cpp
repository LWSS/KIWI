#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Auto-bool orchestration; Brush_MergeList remains the geometry authority.

#include "stdafx.h"
#include "qe3.h"
#include <vector>
#include <math.h>              // fabs — the cluster phase's volume comparison

#include "kiwi_autobool.h"
#include "kiwi_csg.h"                 // shared CSG merge eligibility
#include "kiwi_command.h"      // KIWI_CMD_AUTO_BOOL + KiwiCmd_UndoBegin / KiwiCmd_UndoCommit
#include "kiwi_selection.h"    // KiwiSel / Sel_Clear / Sel_BrushLive
#include "kiwi_selext.h"       // KSELX_TOUCH_EPS

// Ported entry points.
extern int         Sys_Printf( const char *fmt, ... );                        // win_qe3.cpp
extern int         g_nUpdateBits;                                             // 0x25D5A74 (mainfrm.cpp)
// Takes a NULL-terminated selbrush_t.next list; returns a new unlinked instance or
// nullptr and never frees the inputs.
extern selbrush_t *Brush_MergeList( selbrush_t *brushList );                  // csg.cpp:409   (0x47D600)
extern void        Brush_RemoveFromList( selbrush_t *b );                     // brush.cpp:972 (0x476680)
extern void        Brush_AddToList2( selbrush_t *b );                         // brush.cpp:927 (0x4765A0)
extern void        Brush_Free( selbrush_t *b );                               // brush.cpp:1002 (0x475BA0)
// Keep at file scope; MSVC gave the former block-scope declaration the wrong linkage.
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods,
                                            int commandId );                  // mainfrm.cpp:1340
// selected_brushes is a circular sentinel (qe3.h:374, 1054).

namespace
{
    // Cap worst-case rescans; selections above 257 brushes can stop before a true
    // fixed point if each pass exposes only one earlier pair.
    const int KAB_MAX_PASSES = 256;

    // Pairs were already exhausted; three is the first set size with a new outcome.
    const size_t KAB_MIN_CLUSTER = 3;

    // 0.1% is intended to clear shifted float-winding noise while catching void fill.
    const double KAB_VOLUME_REL_EPS = 0.001;

    // Near zero, allow one cubic world unit to absorb float noise.
    const double KAB_VOLUME_ABS_EPS = 1.0;

    // CSG_Merge's per-brush eligibility (csg.cpp:589-603).
    inline bool Usable( const selbrush_t *b ) { return KiwiCsg_BrushUsable( b ); }

    const entity_s *OwnerDef( const selbrush_t *b )
    {
        return ( b && b->owner ) ? b->owner->def : 0;
    }

    // Ported Select_Touching_R bounds test (select.cpp:1678-1679). Padding is required
    // because mergeable neighbours abut; Brush_BuildWindings maintains these bounds.
    bool BoundsTouch( const brush_t *a, const brush_t *b )
    {
        for ( int i = 0; i < 3; ++i )
        {
            if ( a->maxs[i] + KSELX_TOUCH_EPS < b->mins[i] ) return false;
            if ( a->mins[i] - KSELX_TOUCH_EPS > b->maxs[i] ) return false;
        }
        return true;
    }

    // Flipped-equal planes can be classified interior without winding contact, so a
    // cluster is accepted only when total winding volume is conserved.
    // V = (1/3) * Σ( triangle centroid · face normal ) * triangle area.
    // Coordinates are relative to the cluster centre to avoid world-coordinate
    // cancellation; the accumulator is double. Missing windings reject measurement,
    // while zero-area fan triangles currently contribute nothing.
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

                // Unsigned area; orientation comes from the face plane's normal.
                const double area = 0.5 * sqrt( c[0] * c[0] + c[1] * c[1] + c[2] * c[2] );
                if ( area <= 0.0 )
                    continue;                  // degenerate sliver; contributes nothing

                const double cx = ( p0[0] + pi[0] + pj[0] ) / 3.0;
                const double cy = ( p0[1] + pi[1] + pj[1] ) / 3.0;
                const double cz = ( p0[2] + pi[2] + pj[2] ) / 3.0;
                total += ( cx * n[0] + cy * n[1] + cz * n[2] ) * area;
            }
        }

        // Only the magnitude is compared, independent of the port's normal convention.
        *outVolume = fabs( total ) / 3.0;
        return true;
    }

    // Touching-component union-find with path halving.
    int UF_Find( std::vector<int> &parent, int x )
    {
        while ( parent[x] != x )
        {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }

    // CSG_Merge's list surgery (csg.cpp:605-648) restricted to two brushes. Returns
    // the linked result, or relinks both inputs and returns nullptr on refusal.
    selbrush_t *TryMergePair( selbrush_t *a, selbrush_t *b )
    {
        // Prepending matches csg.cpp:607-616. The head selects the result owner and
        // fixes the face order used by the core's duplicate-plane skip.
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
            // Brush_MergeList returns an instance unlinked from the display lists.
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

    // N-way merge with a volume guard. Inputs remain live until acceptance, so either
    // refusal can relink them. Returns 1 accepted, 0 core-refused, or -1 unmeasurable/
    // volume-rejected; *outMerged is linked only on acceptance.
    int TryMergeCluster( selbrush_t *const *members, size_t count, selbrush_t **outMerged )
    {
        *outMerged = nullptr;
        if ( !members || count < 2 )
            return 0;

        // Measure before unlinking; use the cluster bounds centre as the stable origin.
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

        // Prepend exactly as CSG_Merge does; list-head owner and face order are observable.
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

        // Restoration is identical, but reporting distinguishes core and guard refusal.
        const bool coreRefused = ( merged == nullptr );

        // Brush_MergeList built the result windings at csg.cpp:561.
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
            // The result is unlinked and never escaped this undo bracket; freeing the
            // instance here also releases its transient def.
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

    // Greedy results depend on order, so the reverse pass can recover pairs the forward
    // pass spoiled. Reversal also flips the list-head and duplicate-plane face order.
    int PairwiseFixedPoint( std::vector<selbrush_t *> &set, bool reverse )
    {
        const size_t n = set.size();
        if ( n < 2 )
            return 0;

        std::vector<size_t> order( n );
        for ( size_t k = 0; k < n; ++k )
            order[k] = reverse ? ( n - 1 - k ) : k;

        // Keep each result in slot i and continue the sweep; repeat because a merge can
        // expose an earlier pair. Check liveness once per pass to avoid an O(n²*map)
        // display-list walk; nothing external can free a brush inside the undo bracket.
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
                        continue;              // refused; both were relinked

                    // The result stays in i and can merge again during this sweep.
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

    // Group pairwise survivors by bounds contact and offer each component of at least
    // three whole. This can merge convex sets no pair can, while excluding unrelated
    // brushes that would only poison the attempt. Rejected subsets are not searched;
    // the reverse pairwise pass gets one more chance. Returns brush-count reduction;
    // out-parameters tally attempted, accepted, and guard-rejected components.
    int ClusterPhase( std::vector<selbrush_t *> &set,
                      int *outTried, int *outAccepted, int *outRejected )
    {
        *outTried    = 0;
        *outAccepted = 0;
        *outRejected = 0;

        const size_t n = set.size();
        if ( n < KAB_MIN_CLUSTER )
            return 0;

        // One display-list liveness sweep; retired slots are cleared below.
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
                // Smallest-index roots make the representative and result slot stable.
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
                // Keep the result in the component's first slot for the reverse sweep.
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

    void RunAutoBool()
    {
        // Validate the complete candidate set before mutation.
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

        // Typed items would retain freed selbrush_t pointers. Keep the legacy list as
        // both the undo subject and working set.
        Sel_Clear( KiwiSel() );

        // One bracket clones every original selected def. Intermediates exist only
        // inside it; final results remain selected for Undo_EndBrushList. The literal
        // is required because Undo_GeneralStart stores the pointer.
        KiwiCmd_UndoBegin( "auto bool" );

        // Forward pairs shrink the risky cluster input; guarded clusters see survivors;
        // reverse pairs recover order-dependent misses. Each phase preserves slots and
        // revalidates brush liveness.
        const int pairMerges = PairwiseFixedPoint( set, false );

        int clustersTried    = 0;
        int clustersAccepted = 0;
        int clusterRejected  = 0;
        const int clusterMerges = ClusterPhase( set, &clustersTried,
                                                &clustersAccepted, &clusterRejected );

        const int reverseMerges = PairwiseFixedPoint( set, true );

        const int merges = pairMerges + clusterMerges + reverseMerges;

        KiwiCmd_UndoCommit();

        int after = 0;
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            ++after;

        g_nUpdateBits = -1;                     // CSG_Merge's own success dirty (csg.cpp:634)

        // Keep guard rejections visible; they indicate a core result failed conservation.
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

bool KiwiAutoBool_CanExecute()
{
    // Same >=2, usable, one-owner gate as the handler.
    return KiwiCsg_SelectionMergeable();
}

bool KiwiAutoBool_DispatchInstant( unsigned int commandId )
{
    if ( commandId != (unsigned int)KIWI_CMD_AUTO_BOOL )
        return false;
    RunAutoBool();
    return true;
}

void KiwiAutoBool_RegisterCommands()
{
    // Intentionally unbound: this topology-changing command is palette/CSG-panel only.
    Radiant_RegisterCommand( "KiwiAutoBool", 0, 0, KIWI_CMD_AUTO_BOOL );
}

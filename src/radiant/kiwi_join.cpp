#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_join.cpp — SHAKEOUT G implementation.  See kiwi_join.h for the Plasticity
// finding (its `j` is CURVES ONLY; its face-merge command is an empty body), the
// three arms, the same-infinite-plane coplanarity test and why the face arm
// reaches CSG_Merge through the CLASSIC command id rather than calling it.
//
// NEW code over the ported cores.  It selects and dispatches; it computes no
// geometry and owns no undo bracket.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_join.h"
#include "kiwi_command.h"
#include "kiwi_conselect.h"
#include "kiwi_selection.h"
#include "kiwi_validity.h"          // KVALID_PLANE_DOT / KVALID_PLANE_DIST (§19's own)
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <math.h>

// ── ported entry points (each verified against its definition) ──────────────
extern int  Sys_Printf( const char *fmt, ... );                        // win_qe3.cpp
extern void Select_Deselect( int bAlsoFreeFaces );                     // select.cpp:1445 (0x48E800)
extern void Select_Brush( selbrush_t *brush, char some_overwrite,
                          char bStatus, char center_grid_on_selection ); // select.cpp:884
extern void Radiant_ExecCommand( unsigned int cmdId );                 // mainfrm.cpp:4083

namespace
{

    // The two selected FACE items, when there are EXACTLY two of them.  Returns
    // false for any other count, which is the whole precondition of arm (a).
    bool TwoFaces( sel_item_t *a, sel_item_t *b )
    {
        const selection_t &sel = KiwiSel();
        int n = 0;
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            const sel_item_t &it = sel.items[i];
            if ( it.kind != SEL_FACE || !Sel_BrushLive( it.brush ) )
                continue;
            if ( n == 0 )      *a = it;
            else if ( n == 1 ) *b = it;
            ++n;
            if ( n > 2 )
                return false;
        }
        return n == 2;
    }

    const face_t *FaceOf( const sel_item_t &it )
    {
        if ( !it.brush || !it.brush->def || !it.brush->def->faces )
            return 0;
        if ( it.faceIndex < 0 || it.faceIndex >= it.brush->def->faceCount )
            return 0;
        return &it.brush->def->faces[it.faceIndex];
    }

    // ── THE SAME-INFINITE-PLANE TEST (kiwi_join.h spells out why) ───────────
    // |dot(n1,n2)| > KVALID_PLANE_DOT AND |d1 - sign(dot)*d2| < KVALID_PLANE_DIST.
    // The MAGNITUDE is what is tested, not the signed dot: two brushes stacked
    // against each other share a plane whose outward normals point AT each other,
    // which is precisely the arrangement this verb exists to simplify.  The
    // constant is matched with the sign so the distance comparison stays honest
    // for the flipped case.
    //
    // Face planes carry (normal, dist) with the interior at n·p <= dist — the same
    // convention §19's V5/V6 checks read (kiwi_validity.h), so the two tests agree
    // by construction rather than by coincidence.
    bool SameInfinitePlane( const face_t *f1, const face_t *f2 )
    {
        if ( !f1 || !f2 )
            return false;
        const float d = Dot3( f1->plane.normal, f2->plane.normal );
        if ( fabsf( d ) <= KVALID_PLANE_DOT )
            return false;
        const float sign = ( d < 0.0f ) ? -1.0f : 1.0f;
        return fabsf( f1->plane.dist - sign * f2->plane.dist ) < KVALID_PLANE_DIST;
    }

    // ── ARM (a): two coplanar faces on two brushes -> merge the owners ──────
    // WHAT THIS DOES NOT DO: any CSG.  CSG_Merge's contract is "merge everything
    // on selected_brushes" and it takes no arguments (csg.cpp:572-579), so the
    // only thing to decide here is WHAT IS SELECTED when it runs.  Driving that
    // through the ported funnels (Select_Deselect, then Select_Brush per brush —
    // select.cpp:884) and then running the CLASSIC id keeps exactly one handler,
    // one undo record and one console report per merge:
    //
    //   Radiant_ExecCommand( 32927 ) -> Cmd_OnSelectionCsgmerge (mainfrm.cpp:2312)
    //     KiwiCsg_NoteBefore / Undo_ClearRedo / Undo_GeneralStart("CSG merge") /
    //     Undo_AddBrushList / CSG_Merge / Undo_EndBrushList / Undo_End /
    //     KiwiCsg_NoteAfter
    //
    // Every refusal (fixed-size entities, patches, different owner entities, a
    // non-convex hull) is CSG_Merge's own and is printed by CSG_Merge, which also
    // re-adds the originals on its own failure path (csg.cpp:620+).  This file
    // therefore refuses only the things CSG_Merge cannot see: fewer or more than
    // two faces, both faces on one brush, and non-coplanar planes.
    bool JoinFaces()
    {
        sel_item_t a, b;
        if ( !TwoFaces( &a, &b ) )
            return false;
        if ( a.brush == b.brush )
        {
            Sys_Printf( "Join: those two faces are on the SAME brush — select one "
                        "face on each of two brushes.\n" );
            return true;                     // the arm MATCHED; it just refused
        }
        if ( !SameInfinitePlane( FaceOf( a ), FaceOf( b ) ) )
        {
            Sys_Printf( "Join: those two faces are not on the same plane, so the "
                        "brushes do not meet across one.\n" );
            return true;
        }

        // The typed selection is about to become meaningless (the two brushes
        // become one), so it is dropped BEFORE the merge rather than left naming
        // faces of a freed def.
        Sel_Clear( KiwiSel() );
        Select_Deselect( 1 );
        Select_Brush( a.brush, 0, 0, 0 );
        Select_Brush( b.brush, 0, 0, 0 );
        Radiant_ExecCommand( 32927 );        // the one CSG-merge handler
        return true;
    }
}

// ─── §3 canExecute ───────────────────────────────────────────────────────────
bool KiwiJoin_CanJoin()
{
    sel_item_t a, b;
    if ( TwoFaces( &a, &b ) && a.brush != b.brush
      && SameInfinitePlane( FaceOf( a ), FaceOf( b ) ) )
        return true;
    return KiwiConSel_CanJoin();
}

// ─── the CONTEXT verb ────────────────────────────────────────────────────────
// ORDER: faces first, lines second.  The two selections are parallel and can both
// be non-empty (kiwi_conselect.h), and a brush-side selection is what every other
// command in the editor reads first, so J agrees with the rest of the editor
// rather than inventing a second precedence.
bool KiwiJoin_DispatchInstant( unsigned int commandId )
{
    if ( commandId != KIWI_CMD_JOIN )
        return false;

    if ( JoinFaces() )
        return true;
    if ( KiwiConSel_CanJoin() )
    {
        KiwiConSel_Join();                   // the shakeout-F chain walker
        return true;
    }

    Sys_Printf( "Join: select either TWO coplanar faces on two brushes (they merge "
                "into one solid) or two or more construction lines (they chain into "
                "one polyline).\n" );
    return true;
}

// ─── registration ────────────────────────────────────────────────────────────
void KiwiJoin_RegisterCommands()
{
    extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );
    Radiant_RegisterCommand( "KiwiJoin", 0, 0, KIWI_CMD_JOIN );
}

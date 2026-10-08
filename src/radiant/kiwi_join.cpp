#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Context-sensitive Join dispatch; geometry and undo remain in existing cores.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_join.h"
#include "kiwi_command.h"
#include "kiwi_conselect.h"
#include "kiwi_ported.h"
#include "kiwi_selection.h"
#include "kiwi_terrain.h"
#include "kiwi_validity.h"          // plane-comparison tolerances
#include "kiwi_vec.h"     // vector helpers

#include <math.h>

namespace
{

    // Return exactly two live face items.
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

    // Opposed normals may share a plane; compare |dot| and sign-adjusted distances
    // (world units, n·p <= dist) at the validity tolerances.
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

    // Two faces, one on each of two brushes, on one plane.
    bool CanJoinFaces( sel_item_t *a, sel_item_t *b )
    {
        return TwoFaces( a, b ) && a->brush != b->brush
            && SameInfinitePlane( FaceOf( *a ), FaceOf( *b ) );
    }

    // Why two selected faces cannot merge; false when two faces are not selected.
    bool FaceReason()
    {
        sel_item_t a, b;
        if ( !TwoFaces( &a, &b ) )
            return false;
        if ( a.brush == b.brush )
            Sys_Printf( "Join: those two faces are on the SAME brush — select one "
                        "face on each of two brushes.\n" );
        else
            Sys_Printf( "Join: those two faces are not on the same plane, so the "
                        "brushes do not meet across one.\n" );
        return true;
    }

    // CSG_Merge consumes selected_brushes; command 32927 owns its undo/reporting
    // and restores the originals on refusal (csg.cpp:572, 0x47DA40).
    void JoinFaces( const sel_item_t &a, const sel_item_t &b )
    {
        // Drop typed face references before the merge replaces their brush defs.
        Sel_Clear( KiwiSel() );
        Select_Deselect( 1 );
        Select_Brush( a.brush, 0, 0, 0 );
        Select_Brush( b.brush, 0, 0, 0 );
        Radiant_ExecCommand( 32927 );        // classic undo-wrapped CSG merge
    }
}

// KIWI: the predicate and the dispatch try the same arms in the same order - faces that
// can merge, terrain sheets that can join, construction lines - so an enabled Join always
// joins something.
bool KiwiJoin_CanJoin()
{
    sel_item_t a, b;
    if ( CanJoinFaces( &a, &b ) )
        return true;
    if ( KiwiTerrain_CanJoinSelected() )
        return true;
    return KiwiConSel_CanJoin();
}

bool KiwiJoin_DispatchInstant( unsigned int commandId )
{
    if ( commandId != KIWI_CMD_JOIN )
        return false;

    sel_item_t a, b;
    if ( CanJoinFaces( &a, &b ) )
    {
        JoinFaces( a, b );
        return true;
    }
    // Terrain: two or more selected sheet patches that share full edges merge into one
    // grid (kiwi_terrain.cpp), as long as the result stays within the 16-point cap.
    if ( KiwiTerrain_CanJoinSelected() )
    {
        KiwiTerrain_JoinSelected();
        return true;
    }
    if ( KiwiConSel_CanJoin() )
    {
        KiwiConSel_Join();                   // chain selected construction lines
        return true;
    }

    // Nothing joins: the specific reason (two faces, then terrain sheets), else the help.
    if ( FaceReason() )
        return true;
    if ( KiwiTerrain_JoinSelected() >= 0 )   // joins nothing here; prints the sheets' reason
        return true;
    Sys_Printf( "Join: select either TWO coplanar faces on two brushes (they merge "
                "into one solid), two or more edge-adjacent terrain patches (they merge "
                "into one grid), or two or more construction lines (they chain into "
                "one polyline).\n" );
    return true;
}

void KiwiJoin_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiJoin", 0, 0, KIWI_CMD_JOIN );
}

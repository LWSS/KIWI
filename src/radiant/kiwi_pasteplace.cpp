#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// See kiwi_pasteplace.h.

#include "stdafx.h"
#include "qe3.h"
#include "kiwi_pasteplace.h"
#include "kiwi_command.h"
#include "kiwi_droptrace.h"            // KiwiDrop_IsModelEntity / KiwiDrop_GetModelInfo
#include "kiwi_selection.h"            // Sel_InvalidateFromLegacy

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// Ported entry points (verified against their definitions).
extern int         Sys_Printf( const char *fmt, ... );
extern int         g_nUpdateBits;
extern selbrush_t  selected_brushes;                                          // map.cpp
extern selbrush_t  active_brushes;                                            // map.cpp
extern bool        g_kiwiImportInCallerUndo;                                  // map.cpp (KIWI hook)
extern bool        RadiantClipboard_HasMapText();                             // entity.cpp (KIWI tail)
extern void        RadiantClipboard_Paste();                                  // entity.cpp
extern void        Select_Deselect( int a1 );                                 // select.cpp:1444
extern void        Select_Brush( selbrush_t *brush, char some_overwrite, char bStatus, char center ); // select.cpp:904
extern void        Select_Delete();                                           // select.cpp:1539
extern void        Select_ApplyMatrix_SelectedBrushes( int bSnap, float *mat, float deg, char bSwap ); // select.cpp:2260
extern entity_s   *Brush_Move( const float *move, brush_t *def, char snap );  // brush.cpp 0x4782A0
extern void        SetKeyValue( entity_s_def *e, const char *key, const char *value ); // entity.cpp:213
extern void        sub_47B940( brush_t *def );                                // brush.cpp Brush_UpdateSpecialMaterialFlag
extern void        Undo_ClearRedo();
extern void        Undo_GeneralStart( const char *operation );
extern void        Undo_AddBrushList( selbrush_t *list );
extern void        Undo_EndBrushList( selbrush_t *list );
extern void        Undo_AddEntity_W( entity_s *e );
extern void        Undo_End();
extern void        MarkMapModified();
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

namespace
{
    struct ppPose_t
    {
        float       origin[3];
        float       angles[3];
        float       scale;
        std::string modelName;          // the "model" key, lower case, forward slashes
        float       size;               // bounds volume - the fallback "primary model" measure
        std::string angleText;          // the target's own "angles" string, reused verbatim
    };

    const char *KeyOf( const entity_s_def *def, const char *key )
    {
        if ( def )
            for ( const epair_t *ep = def->epairs; ep; ep = ep->next )
                if ( ep->key && !_stricmp( ep->key, key ) )
                    return ep->value ? ep->value : "";
        return "";
    }

    // (forward, left, up) rows from pitch / yaw / roll - AnglesToAxis.
    void AnglesAxis( const float *ang, float out[3][3] )
    {
        const float k = 3.14159265358979323846f / 180.0f;
        const float sp = sinf( ang[0] * k ), cp = cosf( ang[0] * k );
        const float sy = sinf( ang[1] * k ), cy = cosf( ang[1] * k );
        const float sr = sinf( ang[2] * k ), cr = cosf( ang[2] * k );
        out[0][0] = cp * cy;                out[0][1] = cp * sy;                out[0][2] = -sp;
        out[1][0] = sr * sp * cy - cr * sy; out[1][1] = sr * sp * sy + cr * cy; out[1][2] = sr * cp;
        out[2][0] = cr * sp * cy + sr * sy; out[2][1] = cr * sp * sy - sr * cy; out[2][2] = cr * cp;
    }

    // KIWI (2026-09-18, user: "the wall vanishes and I see nothing ... do a comparison via
    // name, and if that doesn't work take the biggest one by size"): the first version read
    // the pose through KiwiDrop_GetModelInfo, which needs the XModel RESIDENT with valid
    // bounds.  A model pasted a microsecond ago is not, so every pasted model failed the read,
    // no anchor was found, the group stayed where it was copied (invisible, on top of its
    // originals) - and the target was already deleted.  The pose now comes straight from
    // the entity: origin, the `angles` / `angle` keys, `modelscale`, and the `model` NAME.
    bool PoseOf( selbrush_t *node, ppPose_t *out )
    {
        if ( !node || !node->owner || !KiwiDrop_IsModelEntity( node ) )
            return false;
        const entity_s_def *def = (const entity_s_def *)node->owner->def;
        if ( !def )
            return false;
        for ( int k = 0; k < 3; ++k )
            out->origin[k] = def->origin[k];
        out->angles[0] = out->angles[1] = out->angles[2] = 0.0f;
        out->angleText = KeyOf( def, "angles" );
        if ( out->angleText.empty()
          || sscanf( out->angleText.c_str(), "%f %f %f", &out->angles[0], &out->angles[1], &out->angles[2] ) != 3 )
        {
            out->angles[0] = out->angles[2] = 0.0f;
            out->angles[1] = (float)atof( KeyOf( def, "angle" ) );     // legacy yaw-only key
        }
        out->scale = 1.0f;
        {
            const float s = (float)atof( KeyOf( def, "modelscale" ) );
            if ( s > 0.0f )
                out->scale = s;
        }
        out->modelName = KeyOf( def, "model" );
        for ( size_t i = 0; i < out->modelName.size(); ++i )
        {
            char &c = out->modelName[i];
            if ( c == '\\' ) c = '/';
            else if ( c >= 'A' && c <= 'Z' ) c = (char)( c - 'A' + 'a' );
        }
        // size: the real model bounds when it is loaded, else the proxy brush
        float mins[3], maxs[3], ang[3], sc, org[3];
        if ( KiwiDrop_GetModelInfo( node, mins, maxs, ang, &sc, org ) )
            out->size = ( maxs[0] - mins[0] ) * ( maxs[1] - mins[1] ) * ( maxs[2] - mins[2] ) * sc * sc * sc;
        else if ( node->def )
            out->size = ( node->def->maxs[0] - node->def->mins[0] ) * ( node->def->maxs[1] - node->def->mins[1] )
                      * ( node->def->maxs[2] - node->def->mins[2] );
        else
            out->size = 0.0f;
        return true;
    }

    int SelectedModelCount()
    {
        int n = 0;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            if ( KiwiDrop_IsModelEntity( b ) )
                ++n;
        return n;
    }

    void PasteInPlace()
    {
        // ── the targets: every selected model, by pose (the nodes die below) ─────────
        std::vector<selbrush_t *> targetNodes;
        std::vector<ppPose_t>     targets;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            ppPose_t p;
            if ( PoseOf( b, &p ) )
            {
                targetNodes.push_back( b );
                targets.push_back( p );
            }
        }
        if ( targets.empty() )
        {
            Sys_Printf( "Paste Group In Place: select the model(s) to replace first.\n" );
            return;
        }

        Undo_ClearRedo();
        Undo_GeneralStart( "paste group in place" );     // stores the POINTER - literals only

        // Only the targets stay selected; save them into the record.  They are DELETED LAST,
        // and only those whose replacement really landed: the first version deleted up front,
        // so a paste that could not be lined up cost the user the wall and showed nothing.
        Select_Deselect( 1 );
        for ( size_t i = 0; i < targetNodes.size(); ++i )
            Select_Brush( targetNodes[i], 0, 0, 0 );
        Undo_AddBrushList( &selected_brushes );
        for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
            Undo_AddEntity_W( (entity_s *)i->owner->def );

        std::vector<selbrush_t *> created, doomed;
        int replaced = 0, scaleMismatch = 0, noAnchor = 0, bySize = 0;
        for ( size_t t = 0; t < targets.size(); ++t )
        {
            const ppPose_t &T = targets[t];

            g_kiwiImportInCallerUndo = true;             // the paste stamps OUR record
            RadiantClipboard_Paste();                    // deselects, pastes, selects the paste
            g_kiwiImportInCallerUndo = false;

            // The anchor = the pasted model that STANDS FOR the target:
            //   1. the same `model` name (the biggest, should the group hold it twice);
            //   2. else the biggest model of the group - the wall, not one of its arms.
            selbrush_t *anchor = nullptr;
            ppPose_t    A;
            bool        anchorByName = false;
            int         pastedModels = 0;
            for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            {
                ppPose_t p;
                if ( !PoseOf( b, &p ) )
                    continue;
                ++pastedModels;
                const bool byName = !T.modelName.empty() && p.modelName == T.modelName;
                const bool better = !anchor
                                 || ( byName && !anchorByName )
                                 || ( byName == anchorByName && p.size > A.size );
                if ( better )
                {
                    anchor = b;
                    A = p;
                    anchorByName = byName;
                }
            }
            if ( !anchor )
            {
                // Nothing to line up with: take this paste back out and KEEP the target.
                ++noAnchor;
                Select_Delete();
                continue;
            }
            if ( !anchorByName )
                ++bySize;                                // reported once, in the summary line
            (void)pastedModels;
            doomed.push_back( targetNodes[t] );
            {
                // rotation M (row-vector convention, v' = v * M) with  Aaxis * M = Taxis
                float ax[3][3], tx[3][3];
                AnglesAxis( A.angles, ax );
                AnglesAxis( T.angles, tx );
                float rot_around[4][3];
                for ( int k = 0; k < 3; ++k )
                    rot_around[0][k] = A.origin[k];      // pivot = the anchor's origin
                bool identity = true;
                for ( int a = 0; a < 3; ++a )
                    for ( int b = 0; b < 3; ++b )
                    {
                        float m = 0.0f;
                        for ( int k = 0; k < 3; ++k )
                            m += ax[k][a] * tx[k][b];
                        rot_around[1 + a][b] = m;
                        if ( fabsf( m - ( a == b ? 1.0f : 0.0f ) ) > 1.0e-6f )
                            identity = false;
                    }
                if ( !identity )
                    Select_ApplyMatrix_SelectedBrushes( 0, rot_around[0], 1.0f, 0 );   // deg != 0: fixed-size entities turn too

                // then carry the anchor's (unmoved: it was the pivot) origin onto the target's
                ppPose_t now;
                const float *from = PoseOf( anchor, &now ) ? now.origin : A.origin;
                const float move[3] = { T.origin[0] - from[0], T.origin[1] - from[1], T.origin[2] - from[2] };
                if ( move[0] != 0.0f || move[1] != 0.0f || move[2] != 0.0f )
                    for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
                        Brush_Move( move, b->def, 0 );

                // the replacement wall is EXACTLY the old one: reuse its angle string
                entity_s_def *adef = anchor->owner ? (entity_s_def *)anchor->owner->def : nullptr;
                if ( adef && !T.angleText.empty() )
                {
                    SetKeyValue( adef, "angles", T.angleText.c_str() );
                    ++anchor->def->version;
                }
                if ( fabsf( A.scale - T.scale ) > 1.0e-4f )
                    ++scaleMismatch;
                ++replaced;
            }
            for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
            {
                sub_47B940( b->def );
                created.push_back( b );
            }
        }

        // now the replaced targets go (saved in the record above)
        Select_Deselect( 1 );
        for ( size_t i = 0; i < doomed.size(); ++i )
            Select_Brush( doomed[i], 0, 0, 0 );
        if ( !doomed.empty() )
            Select_Delete();
        targetNodes.clear();

        // every pasted object selected, stamped, record closed
        Select_Deselect( 1 );
        for ( size_t i = 0; i < created.size(); ++i )
            Select_Brush( created[i], 0, 0, 0 );
        Undo_EndBrushList( &selected_brushes );
        Undo_End();
        MarkMapModified();
        Sel_InvalidateFromLegacy();
        g_nUpdateBits = -1;

        Sys_Printf( "Paste Group In Place: replaced %i model%s with the clipboard group (%i object%s).%s%s%s\n",
                    replaced, replaced == 1 ? "" : "s", (int)created.size(), created.size() == 1 ? "" : "s",
                    bySize ? "  NOTE: no model name in the clipboard matched some targets - the group's biggest model was lined up instead." : "",
                    scaleMismatch ? "  NOTE: some targets had a different modelscale; the clipboard's scale was kept." : "",
                    noAnchor ? "  NOTE: the clipboard holds no model to line up with - those targets were left untouched." : "" );
    }
}

bool KiwiPastePlace_CanExecute()
{
    return RadiantClipboard_HasMapText() && SelectedModelCount() > 0;
}

void KiwiPastePlace_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiPasteInPlace", 0, 0, KIWI_CMD_PASTE_IN_PLACE );
}

bool KiwiPastePlace_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned int)KIWI_CMD_PASTE_IN_PLACE )
        return false;
    if ( KiwiCmd_Active() )
    {
        Sys_Printf( "Paste Group In Place: finish the current command first.\n" );
        return true;
    }
    if ( !RadiantClipboard_HasMapText() )
    {
        Sys_Printf( "Paste Group In Place: copy the group (the model and what belongs to it) first.\n" );
        return true;
    }
    PasteInPlace();
    return true;
}

void KiwiPastePlace_BuildMenu( void *frameMenu )
{
    HMENU menu = (HMENU)frameMenu;
    HMENU edit = menu ? ::GetSubMenu( menu, 1 ) : nullptr;
    if ( !edit || ::GetMenuState( edit, KIWI_CMD_PASTE_IN_PLACE, MF_BYCOMMAND ) != 0xFFFFFFFFu )
        return;
    ::AppendMenuA( edit, MF_STRING, KIWI_CMD_PASTE_IN_PLACE, "Paste Group In Place (replace selected models)\tCtrl+Shift+V" );
}

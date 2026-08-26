#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Material and texdef writes stay in the browser-apply and Surface Inspector funnels.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_caulk.h"
#include "kiwi_command.h"
#include "kiwi_uv.h"        // shared availability and gesture handoff
#include "kiwi_selection.h" // mixed-selection split

#include <string.h>
#include <vector>

// Cross-file entry points.
extern int         Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp
extern int         g_nUpdateBits;                                            // engine_stubs.cpp
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp
// texwnd_s is TU-local, so browser materials are exposed through these accessors.
extern int         TexWnd_MaterialCount();                                   // texwnd.cpp
extern qtexture_s *TexWnd_MaterialAt( int idx );                             // texwnd.cpp
extern void        TexWnd_ApplyMaterialAtIndex( int idx );                   // texwnd.cpp
extern qtexture_s *Texture_GetHandle( const char *name );                    // texwnd.cpp
extern void        Brush_FitTexture( float x, float y, int a4 );             // 0x4939E0
// Command 33220 reaches the static, undo-bracketed Cmd_OnSelectionAutoCaulk.
extern void        Radiant_ExecCommand( unsigned int cmdId );                // mainfrm.cpp
// Materialdef_GetName asserts exactly one of lyrMtl/radMtl, so callers guard it.
extern LayerMaterialDef *Materialdef_GetName( MaterialDef *mtlDef );         // materialdef.cpp

namespace
{
    // Matches Brush_AutoCaulkFace (0x47E080) and Brush_AutoCaulk (0x47E0F0).
    const char *const KCAULK_NAME = "caulk";

    // Registered names may be bare or folder-qualified.
    const char *LeafName( const char *name )
    {
        const char *slash = strrchr( name, '/' );
        if ( slash )
            return slash + 1;
        const char *back = strrchr( name, '\\' );
        return back ? back + 1 : name;
    }

    // Register a first reference before scanning; pointer identity avoids ambiguous names.
    int CaulkIndex()
    {
        qtexture_s *q = Texture_GetHandle( KCAULK_NAME );
        if ( !q )
            return -1;
        const int n = TexWnd_MaterialCount();
        for ( int i = 0; i < n; ++i )
            if ( TexWnd_MaterialAt( i ) == q )
                return i;
        return -1;
    }
}

bool KiwiCaulk_IsCaulkName( const char *name )
{
    if ( !name || !name[0] )
        return false;
    return _stricmp( LeafName( name ), KCAULK_NAME ) == 0;
}

bool KiwiCaulk_CanExecute()
{
    return KiwiUv_CanEdit();
}

void KiwiCaulk_AutoFitApplied( const char *appliedName )
{
    if ( !KiwiCaulk_IsCaulkName( appliedName ) )
        return;
    // One repeat per face; a4 == 0 matches Surface Inspector Fit (0x4939E0).
    Brush_FitTexture( 1.0f, 1.0f, 0 );
    Sys_Printf( "Caulk: fitted one repeat per face (the apply and the fit are separate "
                "undo records - two Ctrl+Z).\n" );
    g_nUpdateBits = -1;
}

namespace
{
    // Brush_AutoCaulk skips fixed-size entities and patches.
    int SelectedBrushCount( int *outSolids )
    {
        int n = 0, solids = 0;
        for ( selbrush_t *b = selected_brushes.next;
              b && b != &selected_brushes; b = b->next )
        {
            ++n;
            brush_t  *def   = b->def;
            entity_s *owner = def ? def->owner : nullptr;
            const bool fixed = owner && owner->eclass && owner->eclass->fixedsize;
            if ( def && !def->patch && !fixed )
                ++solids;
        }
        if ( outSolids )
            *outSolids = solids;
        return n;
    }

    // Brush_AutoCaulk reports its count but returns void, so measure around the call.
    int NonCaulkFaceCount()
    {
        const int layer = g_qeglobals.current_edit_layer;
        int n = 0;
        for ( selbrush_t *b = selected_brushes.next;
              b && b != &selected_brushes; b = b->next )
        {
            brush_t *def = b->def;
            if ( !def || !def->faces || def->patch )
                continue;
            for ( int fi = 0; fi < def->faceCount; ++fi )
            {
                MaterialDef *md = &def->faces[fi].mtldef[layer];
                // Materialdef_GetName asserts exactly one active material representation.
                if ( ( ( md->lyrMtl != 0 ) + ( md->radMtl != 0 ) ) != 1 )
                    continue;
                const char *name = (const char *)Materialdef_GetName( md );
                if ( !KiwiCaulk_IsCaulkName( name ) )
                    ++n;
            }
        }
        return n;
    }

    // Use the command id so Cmd_OnSelectionAutoCaulk retains undo ownership.
    int RunAutoCaulk()
    {
        const int before = NonCaulkFaceCount();
        Radiant_ExecCommand( 33220u );          // Cmd_OnSelectionAutoCaulk
        const int after  = NonCaulkFaceCount();
        return ( before > after ) ? ( before - after ) : 0;
    }

    // Sel_SyncToLegacy replaces both legacy selection sets.
    void SetSelection( const std::vector<sel_item_t> &items, const sel_item_t &active )
    {
        selection_t &sel = KiwiSel();
        Sel_Clear( sel );
        for ( size_t i = 0; i < items.size(); ++i )
            Sel_Add( sel, items[i] );
        if ( Sel_ItemValid( active ) )
            sel.active = active;
        Sel_SyncToLegacy();
    }
}

static bool KiwiCaulk_ApplyToSelection()
{
    if ( !KiwiCaulk_CanExecute() )
    {
        Sys_Printf( "Caulk: nothing selected - select brushes or faces first.\n" );
        return true;
    }

    const int nFaces = g_SelectedFaces.GetSize();
    int       nSolids = 0;
    const int nBrushes = SelectedBrushCount( &nSolids );

    // AutoCaulk owns its material writes and does not expose the changed-face subset;
    // fitting the whole selection would damage aligned UVs on visible faces.
    if ( nFaces == 0 && nSolids > 0 )
    {
        // End or reject a live UV gesture before the external material write.
        if ( !KiwiUv_EndGestureBeforeApply( "Caulk" ) )
            return true;
        const int did = RunAutoCaulk();
        KiwiUv_RestoreGestureAfterApply();
        Sys_Printf( "Caulk: auto-caulked %d hidden face%s on %d selected solid%s "
                    "(faces concealed by neighbouring geometry only).\n",
                    did, did == 1 ? "" : "s", nSolids, nSolids == 1 ? "" : "s" );
        g_nUpdateBits = -1;
        return true;
    }

    const int idx = CaulkIndex();
    if ( idx < 0 )
    {
        Sys_Printf( "Caulk: no \"%s\" material in the registry - this asset set is "
                    "missing main/materials/%s.\n", KCAULK_NAME, KCAULK_NAME );
        return true;
    }

    // An all-ineligible object selection uses plain apply+fit because patches and
    // fixed-size entities have no AutoCaulk visibility test.
    if ( nBrushes == 0 || nSolids == 0 )
    {
        TexWnd_ApplyMaterialAtIndex( idx );
        if ( nFaces > 0 )
            Sys_Printf( "Caulk: caulked %d selected face%s and fitted %s.\n",
                        nFaces, nFaces == 1 ? "" : "s", nFaces == 1 ? "it" : "them" );
        else
            Sys_Printf( "Caulk: caulked and fitted %d selected patch%s / entit%s "
                        "(nothing here has hidden faces to auto-caulk).\n",
                        nBrushes, nBrushes == 1 ? "" : "es", nBrushes == 1 ? "y" : "ies" );
        g_nUpdateBits = -1;
        return true;
    }

    // Brush_SetTexture walks both legacy sets, so split mixed input or it would
    // plain-caulk every face of each selected solid. Capture live typed items first.
    // Stop a parked face gesture before its indexed selection is replaced.
    if ( !KiwiUv_EndGestureBeforeApply( "Caulk" ) )
        return true;

    std::vector<sel_item_t> faceItems, objItems, allItems;
    const sel_item_t savedActive = KiwiSel().active;
    {
        const selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            const sel_item_t &it = sel.items[i];
            if ( !Sel_ItemValid( it ) || !Sel_BrushLive( it.brush ) )
                continue;
            allItems.push_back( it );
            if ( it.kind == SEL_FACE )       faceItems.push_back( it );
            else if ( it.kind == SEL_OBJECT ) objItems.push_back( it );
        }
    }

    // Keep the browser funnel as the final material mutation before restoration.
    int autoDid = 0;
    if ( !objItems.empty() )
    {
        SetSelection( objItems, sel_item_t() );
        autoDid = RunAutoCaulk();
    }
    if ( !faceItems.empty() )
    {
        SetSelection( faceItems, sel_item_t() );
        TexWnd_ApplyMaterialAtIndex( idx );
    }
    SetSelection( allItems, savedActive );

    Sys_Printf( "Caulk: %d face%s caulked and fitted; %d hidden face%s auto-caulked "
                "on %d solid%s.\n",
                (int)faceItems.size(), faceItems.size() == 1 ? "" : "s",
                autoDid, autoDid == 1 ? "" : "s",
                (int)objItems.size(), objItems.size() == 1 ? "" : "s" );
    g_nUpdateBits = -1;
    return true;
}

void KiwiCaulk_RegisterCommands()
{
    // Modern: End; View->Center moves to Shift+End. Classic stays unchanged.
    Radiant_RegisterCommand( "KiwiCaulkSelection", 0, 0, KIWI_CMD_CAULK_FACES );
}

bool KiwiCaulk_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned int)KIWI_CMD_CAULK_FACES )
        return false;
    return KiwiCaulk_ApplyToSelection();
}

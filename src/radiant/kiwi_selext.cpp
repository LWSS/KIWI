#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Selection expansion writes typed state and syncs it through Sel_SyncToLegacy.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_selext.h"
#include "kiwi_command.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_vec.h"     // Shared vector helpers.

#include <imgui/imgui.h>
#include <math.h>
#include <string.h>
#include <vector>

// Ported entry points.
extern int  Sys_Printf( const char *fmt, ... );          // win_qe3.cpp:118
extern int  g_nUpdateBits;                               // 0x25D5A74 (mainfrm.cpp)
extern void Radiant_ExecCommand( unsigned int cmdId );   // mainfrm.cpp:4054
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

namespace
{

    int EditLayer()
    {
        int l = g_qeglobals.current_edit_layer;
        if ( l < 0 || l > 3 )
            l = 0;
        return l;
    }

    // The registered material of one face on the current edit layer.
    qtexture_s *FaceMaterial( const brush_t *def, int faceIndex )
    {
        if ( !def || !def->faces || faceIndex < 0 || faceIndex >= def->faceCount )
            return 0;
        return def->faces[faceIndex].mtldef[EditLayer()].radMtl;
    }

    // Registry pointer first (materials are singletons through Texture_GetHandle);
    // a case-insensitive name compare covers a re-registered handle.
    bool SameMaterial( const qtexture_s *a, const qtexture_s *b )
    {
        if ( !a || !b )
            return false;
        if ( a == b )
            return true;
        if ( a->name && b->name )
            return _stricmp( a->name, b->name ) == 0;
        return false;
    }

    // Every candidate brush instance, both display lists, filtered exactly as a
    // click pick filters them (kiwi_pick.h Pick_BrushPickable).
    void Candidates( std::vector<selbrush_t *> *out )
    {
        for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
            if ( Pick_BrushPickable( b ) )
                out->push_back( b );
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            if ( Pick_BrushPickable( b ) )
                out->push_back( b );
    }

    // Every DISTINCT live brush the TYPED selection names — faces included,
    // because a face-selected brush is not on selected_brushes (kiwi_selection.h
    // DESIGN NOTE 2), so walking the legacy list would miss it.
    void SelectedBrushes( std::vector<selbrush_t *> *out )
    {
        const selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            selbrush_t *b = sel.items[i].brush;
            if ( !Sel_BrushLive( b ) || !b->def )
                continue;
            bool dup = false;
            for ( size_t k = 0; k < out->size() && !dup; ++k )
                dup = ( (*out)[k] == b );
            if ( !dup )
                out->push_back( b );
        }
    }

    // Matches Select_Touching_R (select.cpp, 0x490520): overlap on all
    // axes with a 1-unit epsilon. Test each brush, not the selection's union box,
    // which would span gaps in a scattered selection.
    bool BoundsTouch( const brush_t *a, const brush_t *b )
    {
        for ( int i = 0; i < 3; ++i )
        {
            if ( a->maxs[i] + KSELX_TOUCH_EPS < b->mins[i] ) return false;
            if ( a->mins[i] - KSELX_TOUCH_EPS > b->maxs[i] ) return false;
        }
        return true;
    }

    bool TouchesRange( const std::vector<selbrush_t *> &set, size_t from, size_t to,
                       const brush_t *def )
    {
        if ( to > set.size() )
            to = set.size();
        for ( size_t i = from; i < to; ++i )
            if ( set[i]->def && BoundsTouch( set[i]->def, def ) )
                return true;
        return false;
    }

    bool TouchesAny( const std::vector<selbrush_t *> &set, const brush_t *def )
    {
        return TouchesRange( set, 0, set.size(), def );
    }

    void Finish( const char *what, int added )
    {
        Sel_SyncToLegacy();
        g_nUpdateBits = -1;
        Sys_Printf( "%s: %i item(s) added, %i selected.\n",
                    what, added, (int)KiwiSel().items.size() );
    }

    // Marquee paths may not set active; fall back to the first suitable live item.
    bool ActiveFace( sel_item_t *out )
    {
        const selection_t &sel = KiwiSel();
        if ( sel.active.kind == SEL_FACE && Sel_BrushLive( sel.active.brush ) )
        {
            *out = sel.active;
            return true;
        }
        for ( size_t i = 0; i < sel.items.size(); ++i )
            if ( sel.items[i].kind == SEL_FACE && Sel_BrushLive( sel.items[i].brush ) )
            {
                *out = sel.items[i];
                return true;
            }
        return false;
    }

    bool ActiveAny( sel_item_t *out )
    {
        const selection_t &sel = KiwiSel();
        if ( Sel_BrushLive( sel.active.brush ) )
        {
            *out = sel.active;
            return true;
        }
        for ( size_t i = 0; i < sel.items.size(); ++i )
            if ( Sel_BrushLive( sel.items[i].brush ) )
            {
                *out = sel.items[i];
                return true;
            }
        return false;
    }

    // Selection operations.
    void SelectCoplanar()
    {
        sel_item_t act;
        if ( !ActiveFace( &act ) )
        {
            Sys_Printf( "Select Coplanar: select a FACE first (mode 3).\n" );
            return;
        }
        const brush_t *adef = act.brush->def;
        if ( !adef || !adef->faces || act.faceIndex < 0 || act.faceIndex >= adef->faceCount )
        {
            Sys_Printf( "Select Coplanar: the active face is stale — reselect it.\n" );
            return;
        }

        float n0[3];
        n0[0] = adef->faces[act.faceIndex].plane.normal[0];
        n0[1] = adef->faces[act.faceIndex].plane.normal[1];
        n0[2] = adef->faces[act.faceIndex].plane.normal[2];
        const float d0 = adef->faces[act.faceIndex].plane.dist;

        std::vector<selbrush_t *> cand;
        Candidates( &cand );

        selection_t &sel = KiwiSel();
        int added = 0;
        for ( size_t c = 0; c < cand.size() && added < KSELX_MAX_FACES; ++c )
        {
            selbrush_t *b = cand[c];
            if ( b->patch )                       // patches have no plane faces
                continue;
            brush_t *def = b->def;
            if ( !def || !def->faces )
                continue;
            for ( int f = 0; f < def->faceCount && added < KSELX_MAX_FACES; ++f )
            {
                if ( Dot3( def->faces[f].plane.normal, n0 ) < KSELX_PLANE_DOT )
                    continue;                     // same ORIENTATION required, not just same plane
                if ( fabsf( def->faces[f].plane.dist - d0 ) > KSELX_PLANE_DIST )
                    continue;
                if ( Sel_Add( sel, Sel_MakeFace( b, f ) ) )
                    ++added;
            }
        }
        if ( added >= KSELX_MAX_FACES )
            Sys_Printf( "Select Coplanar: hit the %i-face cap.\n", KSELX_MAX_FACES );
        Finish( "Select Coplanar", added );
    }

    void SelectTouching()
    {
        std::vector<selbrush_t *> seed;
        SelectedBrushes( &seed );
        if ( seed.empty() )
        {
            Sys_Printf( "Select Touching: nothing is selected.\n" );
            return;
        }

        std::vector<selbrush_t *> cand;
        Candidates( &cand );

        selection_t &sel = KiwiSel();
        int added = 0;
        for ( size_t c = 0; c < cand.size(); ++c )
        {
            if ( !cand[c]->def || !TouchesAny( seed, cand[c]->def ) )
                continue;
            if ( Sel_Add( sel, Sel_MakeObject( cand[c] ) ) )
                ++added;
        }
        Finish( "Select Touching", added );
    }

    void SelectSameMaterial()
    {
        sel_item_t act;
        if ( !ActiveAny( &act ) )
        {
            Sys_Printf( "Select Same Material: nothing is selected.\n" );
            return;
        }
        const bool faceMode = ( act.kind == SEL_FACE );
        const int  refFace  = faceMode ? act.faceIndex : 0;
        qtexture_s *ref     = FaceMaterial( act.brush->def, refFace );
        if ( !ref )
        {
            Sys_Printf( "Select Same Material: the active item has no material on "
                        "edit layer %i.\n", EditLayer() );
            return;
        }

        std::vector<selbrush_t *> cand;
        Candidates( &cand );

        selection_t &sel = KiwiSel();
        int added = 0;
        for ( size_t c = 0; c < cand.size() && added < KSELX_MAX_FACES; ++c )
        {
            selbrush_t *b = cand[c];
            if ( b->patch || !b->def || !b->def->faces )
                continue;
            if ( faceMode )
            {
                for ( int f = 0; f < b->def->faceCount && added < KSELX_MAX_FACES; ++f )
                    if ( SameMaterial( FaceMaterial( b->def, f ), ref )
                         && Sel_Add( sel, Sel_MakeFace( b, f ) ) )
                        ++added;
            }
            else
            {
                bool hit = false;
                for ( int f = 0; f < b->def->faceCount && !hit; ++f )
                    hit = SameMaterial( FaceMaterial( b->def, f ), ref );
                if ( hit && Sel_Add( sel, Sel_MakeObject( b ) ) )
                    ++added;
            }
        }
        Sys_Printf( "Select Same Material: '%s' (edit layer %i).\n",
                    ref->name ? ref->name : "?", EditLayer() );
        Finish( "Select Same Material", added );
    }

    // Fixed-point touching; growth plus the brush/pass caps guarantees termination.
    void SelectConnected()
    {
        std::vector<selbrush_t *> set;
        SelectedBrushes( &set );
        if ( set.empty() )
        {
            Sys_Printf( "Select Connected: nothing is selected.\n" );
            return;
        }

        std::vector<selbrush_t *> cand;
        Candidates( &cand );

        // `taken` excludes seed brushes from later frontier scans.
        std::vector<bool> taken( cand.size(), false );
        for ( size_t c = 0; c < cand.size(); ++c )
            for ( size_t s = 0; s < set.size() && !taken[c]; ++s )
                taken[c] = ( set[s] == cand[c] );

        // Test only the prior frontier so every final set member is scanned once.
        int    passes       = 0;
        bool   grew         = true;
        bool   capped       = false;
        size_t frontierFrom = 0;

        while ( grew && passes < KSELX_MAX_PASSES )
        {
            grew = false;
            ++passes;
            const size_t frontierTo = set.size();
            for ( size_t c = 0; c < cand.size(); ++c )
            {
                if ( taken[c] || !cand[c]->def )
                    continue;
                if ( (int)set.size() >= KSELX_MAX_CONNECTED )
                {
                    capped = true;
                    break;
                }
                if ( !TouchesRange( set, frontierFrom, frontierTo, cand[c]->def ) )
                    continue;
                taken[c] = true;
                set.push_back( cand[c] );
                grew = true;
            }
            frontierFrom = frontierTo;
            if ( capped )
                break;
        }

        selection_t &sel = KiwiSel();
        int added = 0;
        for ( size_t i = 0; i < set.size(); ++i )
            if ( Sel_Add( sel, Sel_MakeObject( set[i] ) ) )
                ++added;

        if ( capped )
            Sys_Printf( "Select Connected: hit the %i-brush cap — stopped.\n",
                        KSELX_MAX_CONNECTED );
        else if ( passes >= KSELX_MAX_PASSES )
            Sys_Printf( "Select Connected: hit the %i-pass cap — stopped.\n",
                        KSELX_MAX_PASSES );
        Finish( "Select Connected", added );
    }
}

// Command availability.
bool KiwiSelExt_CanCoplanar()
{
    const selection_t &sel = KiwiSel();
    for ( size_t i = 0; i < sel.items.size(); ++i )
        if ( sel.items[i].kind == SEL_FACE && sel.items[i].brush )
            return true;
    return false;
}

bool KiwiSelExt_CanTouching()  { return !KiwiSel().items.empty(); }
bool KiwiSelExt_CanConnected() { return !KiwiSel().items.empty(); }

bool KiwiSelExt_CanMaterial()
{
    const selection_t &sel = KiwiSel();
    return !sel.items.empty();
}

// Registration and dispatch.
void KiwiSelExt_RegisterCommands()
{
    // Deliberately unbound; the panel exposes all four commands.
    Radiant_RegisterCommand( "KiwiSelectCoplanar",  0, 0, KIWI_CMD_SELECT_COPLANAR );
    Radiant_RegisterCommand( "KiwiSelectTouching",  0, 0, KIWI_CMD_SELECT_TOUCHING );
    Radiant_RegisterCommand( "KiwiSelectMaterial",  0, 0, KIWI_CMD_SELECT_MATERIAL );
    Radiant_RegisterCommand( "KiwiSelectConnected", 0, 0, KIWI_CMD_SELECT_CONNECTED );
}

bool KiwiSelExt_DispatchInstant( unsigned int commandId )
{
    switch ( commandId )
    {
    case KIWI_CMD_SELECT_COPLANAR:  SelectCoplanar();     return true;
    case KIWI_CMD_SELECT_TOUCHING:  SelectTouching();     return true;
    case KIWI_CMD_SELECT_MATERIAL:  SelectSameMaterial(); return true;
    case KIWI_CMD_SELECT_CONNECTED: SelectConnected();    return true;
    default:                        return false;
    }
}

// Camera double-click selection.
bool KiwiSelExt_CameraDoubleClick( int imgX, int imgY )
{
    ray_t ray;
    if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
        return false;

    // Connected is brush-level, so ignore the current component mode.
    const pick_result_t hit = Pick( ray, SEL_MASK_OBJECT );
    if ( !hit.valid || !Sel_BrushLive( hit.item.brush ) )
        return false;

    // ImGui detects double-clicks by time/distance, not target. Require the hit
    // brush to be represented already; brush matching lets component clicks qualify
    // while different nearby targets fall back to ordinary click handling.
    {
        const selection_t &cur = KiwiSel();
        bool sameThing = false;
        for ( size_t i = 0; i < cur.items.size() && !sameThing; ++i )
            sameThing = ( cur.items[i].brush == hit.item.brush );
        if ( !sameThing )
            return false;
    }

    selection_t &sel = KiwiSel();
    Sel_Clear( sel );
    Sel_Add( sel, Sel_MakeObject( hit.item.brush ) );
    sel.active = Sel_MakeObject( hit.item.brush );
    Sel_SyncToLegacy();

    SelectConnected();
    return true;
}

// Selection block in the shell panel.
void KiwiSelExt_MenuItems()
{
    ImGui::SeparatorText( "Select more" );

    const bool canCo   = KiwiSelExt_CanCoplanar();
    const bool canTo   = KiwiSelExt_CanTouching();
    const bool canMat  = KiwiSelExt_CanMaterial();

    ImGui::BeginDisabled( !canCo );
    if ( ImGui::Button( "Coplanar faces" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_SELECT_COPLANAR );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() && !canCo )
        ImGui::SetTooltip( "Select a FACE first (mode 3)." );

    ImGui::SameLine();
    ImGui::BeginDisabled( !canTo );
    if ( ImGui::Button( "Touching" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_SELECT_TOUCHING );
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled( !canMat );
    if ( ImGui::Button( "Same material" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_SELECT_MATERIAL );
    ImGui::EndDisabled();

    ImGui::BeginDisabled( !canTo );
    if ( ImGui::Button( "Connected (touching)" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_SELECT_CONNECTED );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Also on DOUBLE-CLICK in the 3D view.\n"
                           "Capped at %i brushes.\n"
                           "(Not the classic Select Connected, which walks\n"
                           "target/targetname entity links.)", KSELX_MAX_CONNECTED );
}

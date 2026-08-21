#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_selext.cpp — RADIANT_UX_DESIGN §25 selection expansion.  See kiwi_selext.h
// for the inventory of what the port already has, why the ported region-select
// cluster is NOT the operation §25 asks for, and the granularity rules.
//
// NEW code over the ported cores.  Every result is written into the typed
// selection and pushed down through its ONE crossing (Sel_SyncToLegacy), so the
// legacy lists are still only ever driven by Select_Deselect / Select_Brush.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_selext.h"
#include "kiwi_command.h"
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <imgui/imgui.h>
#include <math.h>
#include <string.h>
#include <vector>

// ── ported entry points (each verified against its definition) ──────────────
extern int  Sys_Printf( const char *fmt, ... );          // win_qe3.cpp:118
extern int  g_nUpdateBits;                               // 0x25D5A74 (mainfrm.cpp)
extern void Radiant_ExecCommand( unsigned int cmdId );   // mainfrm.cpp:4054
// KIWI-UX (CLEANUP, C-55): at FILE scope, like every peer (kiwi_selconv.cpp,
// kiwi_visibility.cpp, kiwi_outliner.cpp) — it was declared inside
// KiwiSelExt_RegisterCommands' body with no cite.
// mainfrm.cpp — the shared command table's append hook (// KIWI-UX there).
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

// active_brushes / selected_brushes are the qe3.h sentinels (qe3.h:1053-1054).

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

    // The ported Select_Touching_R test (select.cpp:1678-1679): overlap on ALL
    // THREE axes with a +1 unit epsilon, so brushes that merely abut still count.
    //
    // DELIBERATE REFINEMENT of the brief's "intersects the selection's bounds":
    // the test is PER SELECTED BRUSH, not against the selection's union box.  A
    // union box over a scattered selection spans everything between its members
    // and would select the whole map; per-brush is what "touching" means to a
    // modeller and is the only version that makes the recursive form terminate
    // anywhere useful.
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

    // ── the ACTIVE item, resolved ───────────────────────────────────────────
    // The active item is what every one of these keys off.  When it is unset or
    // stale, the FIRST live item of the wanted kind stands in — otherwise a
    // marquee (which sets no active item on every path) would make the commands
    // silently unusable.
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

    // ═════════════════════════════════════════════════════════════════════════
    //  the four operations
    // ═════════════════════════════════════════════════════════════════════════
    void SelectCoplanar()
    {
        sel_item_t act;
        if ( !ActiveFace( &act ) )
        {
            Sys_Printf( "Select Coplanar: select a FACE first (mode 3).\n" );
            return;
        }
        const brush_t *adef = act.brush->def;
        // KIWI-UX (CLEANUP, C-54): audible, same early-out.  The arm right above
        // reports its refusal; this one refused the same command in silence.
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

    // TOUCHING, iterated to a fixed point.  The set only ever grows, so the loop
    // terminates; the two caps bound the cost on a dense map.
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

        // Classic flood fill over the candidate set: each pass tests every
        // not-yet-taken candidate against the CURRENT member set (per brush — see
        // BoundsTouch), and stops when a pass adds nothing.  `taken` mirrors
        // `set` so the inner test never rescans a member.
        std::vector<bool> taken( cand.size(), false );
        for ( size_t c = 0; c < cand.size(); ++c )
            for ( size_t s = 0; s < set.size() && !taken[c]; ++s )
                taken[c] = ( set[s] == cand[c] );

        // BFS by FRONTIER, not by whole set: a candidate is tested against each
        // member exactly once over the whole run, so the cost is O(candidates ×
        // final set) rather than O(passes × candidates × set).  On a big map the
        // difference is two orders of magnitude, and this runs on a double-click.
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

// ─── §3 canExecute ───────────────────────────────────────────────────────────
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

// ─── registration + dispatch ─────────────────────────────────────────────────
void KiwiSelExt_RegisterCommands()
{
    // Unbound: the CLASSIC-profile bindings, and the modern profile deliberately
    // claims NO new key this phase.  KEY CANDIDATES, logged rather than taken:
    //   Select Coplanar Faces      — Shift+C (0x43 mods 1; C alone is unbound)
    //   Select Touching            — Ctrl+T  (0x54 mods 4 is free)
    //   Select Same Material       — Shift+M (0x4D mods 1 is free)
    //   Select Connected (touching)— the double-click IS the binding.
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

// ─── the camera double-click (§25 "double-click = connected") ───────────────
bool KiwiSelExt_CameraDoubleClick( int imgX, int imgY )
{
    ray_t ray;
    if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
        return false;

    // Always an OBJECT pick, whatever the current mode mask says: "connected" is
    // a brush-level notion, and a double-click in point/edge mode asking for
    // connected brushes is unambiguous.
    const pick_result_t hit = Pick( ray, SEL_MASK_OBJECT );
    if ( !hit.valid || !Sel_BrushLive( hit.item.brush ) )
        return false;

    // ── KIWI-UX (ROUND Y, ITEM 6): A DOUBLE-CLICK IS TWO CLICKS ON ONE THING ──
    // USER REPORT: "Clicks are still ignored sometimes.  Makes it really annoying
    // to work fast."
    //
    // ImGui's double-click detector is TIME AND DISTANCE ONLY (MouseDoubleClickTime
    // 0.30 s, MouseDoubleClickMaxDist 6 px) — it has no idea what is under the
    // cursor.  So two quick clicks 6 px apart on DIFFERENT objects fired this, and
    // the second click's plain select was replaced by a Sel_Clear + select-connected
    // on whatever the second pick found.  The user's own second object never got
    // selected: from their side the click was ignored.
    //
    // Worse, it CASCADES with the other eaters this round fixes: a user whose first
    // click was swallowed re-clicks immediately, within 300 ms and within a few
    // pixels — the exact input the detector calls a double-click.  So one lost
    // click turned into a select-connected nobody asked for.
    //
    // THE GATE: the BRUSH under the second click must already be represented in
    // the selection, i.e. it must be what the first click selected.  Tested by
    // brush rather than by item so it holds in every mode — the first click may
    // have selected a FACE, an EDGE or a VERTEX of that brush and the expansion is
    // still the one the user is asking for.  A genuine double-click on one thing
    // always passes; a second click on something else falls straight back to the
    // ordinary click path, which is what returning false does.
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

// ─── the "Selection" block in the shell's panel window ──────────────────────
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

#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// CSG workflow UI over the ported geometry cores; no CSG math lives here.
// Buttons dispatch command ids so the shared handlers own execution.

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_csg.h"
#include "kiwi_autobool.h"
#include "kiwi_command.h"                      // KIWI_CMD_AUTO_BOOL

extern int  Sys_Printf( const char *fmt, ... );            // win_qe3.cpp
extern void Radiant_ExecCommand( unsigned int cmdId );     // mainfrm.cpp

namespace
{
    // Classic mainfrm.cpp dispatch ids: Hollow, Merge, and Auto Caulk.
    const int KCSG_ID_HOLLOW     = 32982;
    const int KCSG_ID_MERGE      = 32927;
    const int KCSG_ID_AUTO_CAULK = 33220;

    int  s_beforeCount = -1;          // -1 = no NoteBefore is pending

    int SelectedCount()
    {
        int n = 0;
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            ++n;
        return n;
    }

    // Baseline patch/fixed-owner gate shared by the CSG-family tools.
    inline bool CsgUsable( const selbrush_t *b ) { return KiwiCsg_BrushUsable( b ); }
}

bool KiwiCsg_BrushUsable( const selbrush_t *b )
{
    if ( !b || !b->def )
        return false;
    if ( b->patch )                                    // instance patch @0x20
        return false;
    const entity_s *owner = b->owner;
    if ( !owner || !owner->def )
        return false;
    const entity_s *ownerDef = owner->def;
    if ( !ownerDef->eclass || ownerDef->eclass->fixedsize )
        return false;
    return true;
}

bool KiwiCsg_SelectionMergeable()
{
    selbrush_t *first = selected_brushes.next;
    if ( first == &selected_brushes || first->next == &selected_brushes )
        return false;
    const entity_s *ownerDef0 = first->owner ? first->owner->def : 0;
    if ( !ownerDef0 )
        return false;
    for ( selbrush_t *b = first; b != &selected_brushes; b = b->next )
    {
        if ( !KiwiCsg_BrushUsable( b ) )
            return false;
        if ( b->owner->def != ownerDef0 )
            return false;
    }
    return true;
}

// Palette predicates.
bool KiwiCsg_CanHollow()
{
    selbrush_t *first = selected_brushes.next;
    if ( first == &selected_brushes )
        return false;
    if ( first->next != &selected_brushes )
        return false;
    return CsgUsable( first );
}

bool KiwiCsg_CanMerge()
{
    return KiwiCsg_SelectionMergeable();
}

bool KiwiCsg_CanAutoCaulk()
{
    return selected_brushes.next != &selected_brushes;
}

// Hollow is silent and Merge omits the selection delta, so report it here.
void KiwiCsg_NoteBefore()
{
    s_beforeCount = SelectedCount();
}

void KiwiCsg_NoteAfter( const char *label )
{
    if ( s_beforeCount < 0 )
        return;                                            // no pending observation
    const int before = s_beforeCount;
    const int after  = SelectedCount();
    s_beforeCount = -1;

    if ( before == after )
        Sys_Printf( "%s: %i brush(es) unchanged.\n", label ? label : "CSG", before );
    else
        Sys_Printf( "%s: %i brush(es) in, %i out (selection is the result).\n",
                    label ? label : "CSG", before, after );
}

// The caller is already inside ImGui::Begin, so this block must use buttons.
void KiwiCsg_MenuItems()
{
    ImGui::SeparatorText( "CSG" );

    const bool canMerge  = KiwiCsg_CanMerge();
    const bool canHollow = KiwiCsg_CanHollow();
    const bool canCaulk  = KiwiCsg_CanAutoCaulk();

    ImGui::BeginDisabled( !canMerge );
    if ( ImGui::Button( "Merge" ) )
        Radiant_ExecCommand( (unsigned int)KCSG_ID_MERGE );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() && !canMerge )
        ImGui::SetTooltip( "Select 2+ brushes of the SAME entity\n"
                           "(no patches, no fixed-size entities).\n"
                           "The union must stay convex or the merge is refused." );

    ImGui::SameLine();
    ImGui::BeginDisabled( !canHollow );
    if ( ImGui::Button( "Hollow" ) )
        Radiant_ExecCommand( (unsigned int)KCSG_ID_HOLLOW );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() && !canHollow )
        ImGui::SetTooltip( "Select exactly ONE brush (not a patch,\n"
                           "not a fixed-size entity).  Wall thickness is\n"
                           "the CLASSIC grid size, not the modern one." );

    ImGui::SameLine();
    ImGui::BeginDisabled( !canCaulk );
    if ( ImGui::Button( "Auto Caulk" ) )
        Radiant_ExecCommand( (unsigned int)KCSG_ID_AUTO_CAULK );
    ImGui::EndDisabled();

    // Auto Bool runs pair sweeps plus guarded 3+ clusters; it is intentionally unbound.
    ImGui::SameLine();
    ImGui::BeginDisabled( !KiwiAutoBool_CanExecute() );
    if ( ImGui::Button( "Auto Bool" ) )
        Radiant_ExecCommand( (unsigned int)KIWI_CMD_AUTO_BOOL );
    ImGui::EndDisabled();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Greedily merge every PAIR of selected brushes whose union\n"
                           "is exactly ONE convex brush, until no pair is left.\n"
                           "Lossless: no face is deleted and no plane is moved, so\n"
                           "the volume cannot change.  Greedy, so the result depends\n"
                           "on order.  One Ctrl+Z undoes the whole run." );

    // csg.cpp has no subtract core; Boolean Difference composes the splitter instead.
    ImGui::TextDisabled( "Subtract: press Q (Boolean -> difference), kiwi_boolean.h" );
}

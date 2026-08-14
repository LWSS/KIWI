#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_csg.cpp — RADIANT_UX_DESIGN §24 implementation.  See kiwi_csg.h for the
// csg.cpp inventory, the "there is no subtract" finding and the preview ruling.
//
// NEW code over the ported cores.  It contains no CSG math: every operation runs
// through Radiant_ExecCommand with the CLASSIC command id, i.e. the same handler
// the menu and the hotkey reach.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_csg.h"
#include "kiwi_autobool.h"                     // ROUND AB — the Auto Bool button
#include "kiwi_command.h"                      // KIWI_CMD_AUTO_BOOL

// ── ported entry points (each verified against its definition) ──────────────
extern int  Sys_Printf( const char *fmt, ... );            // win_qe3.cpp
extern void Radiant_ExecCommand( unsigned int cmdId );     // mainfrm.cpp:3894

// selected_brushes / active_brushes are the qe3.h sentinels (qe3.h:1053-1054).

namespace
{
    // The three classic ids this file fronts (mainfrm.cpp's dispatch switch:
    // 32982 -> Cmd_OnSelectionMakehollow, 32927 -> Cmd_OnSelectionCsgmerge,
    // 33220 -> Cmd_OnSelectionAutoCaulk).
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

    // A brush instance CSG can legally consume: not a patch, not the brush of a
    // fixed-size entity.  Both tests are the cores' own (csg.cpp:572's validation
    // loop and CSG_MakeHollow's per-node skip).  `entity_s_def` IS `entity_s`
    // (qe3.h:1000 `typedef entity_s entity_s_def;`), so the ported spelling
    // `((entity_s_def*)b->owner->def)->eclass->fixedsize` and the one below are
    // the same read.
    bool CsgUsable( const selbrush_t *b )
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
}

// ─── §3 palette predicates ───────────────────────────────────────────────────
bool KiwiCsg_CanHollow()
{
    selbrush_t *first = selected_brushes.next;
    if ( first == &selected_brushes )
        return false;                                      // nothing selected
    if ( first->next != &selected_brushes )
        return false;                                      // the handler refuses >1
    return CsgUsable( first );
}

bool KiwiCsg_CanMerge()
{
    selbrush_t *first = selected_brushes.next;
    if ( first == &selected_brushes || first->next == &selected_brushes )
        return false;                                      // the core needs >= 2
    const entity_s *ownerDef0 = first->owner ? first->owner->def : 0;
    if ( !ownerDef0 )
        return false;
    for ( selbrush_t *b = first; b != &selected_brushes; b = b->next )
    {
        if ( !CsgUsable( b ) )
            return false;
        if ( b->owner->def != ownerDef0 )                  // "different entities"
            return false;
    }
    return true;
}

bool KiwiCsg_CanAutoCaulk()
{
    return selected_brushes.next != &selected_brushes;
}

// ─── the console feedback pair ───────────────────────────────────────────────
// Observation only.  CSG_Merge is already chatty ("Merging..." / "done." / its
// four refusal messages) but never says how many brushes it ate; CSG_MakeHollow
// says NOTHING AT ALL, which is the case this exists for — a hollow of a 6-sided
// box silently replaces one brush with six and the user gets no confirmation.
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

// ─── the "Modeling" block in the shell's panel window ───────────────────────
// NOT an ImGui menu — ImGuiPanels_Menu draws inside an ordinary ImGui::Begin
// window, so BeginMenu/MenuItem would be illegal there (the same ruling
// KiwiCon_MenuItems documents).  Buttons in a labelled section.
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

    // ── KIWI-UX (ROUND AB, ITEM 3): AUTO BOOL ──────────────────────────────
    // Same predicate as Merge — it is Merge, applied greedily to PAIRS until no
    // pair is left that forms one convex brush.  Sits here rather than in a new
    // section because it belongs to exactly the same family and reads against
    // exactly the same selection.  Deliberately given no hotkey; the panel and the
    // command palette are its only two routes.
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

    // ROUND L: there is still no subtract CORE in this port (kiwi_csg.h's
    // inventory is unchanged) — but there is now a subtract VERB, built out of the
    // two-halves splitter rather than out of csg.cpp.  The line points at it
    // instead of dead-ending, because "not in this port" now reads as false.
    ImGui::TextDisabled( "Subtract: press Q (Boolean -> difference), kiwi_boolean.h" );
}

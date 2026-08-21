// imgui_panel_layers.cpp — UI-rework Phase 3: the LAYERS panel over the Phase-1
// action/read functions in layersdlg.cpp (declared in radiant_ui_actions.h and as
// externs below). New KISAK code; visible only under -imgui.
//
// Panel semantics vs the MFC CLayerDlg popup: the SAME core calls in the SAME order as
// the MFC handlers —
//   * list rows    : LayerList_Gather → "* " active marker + the [hidden]/[prefab]/
//                    [frozen] flag tags, decoded exactly like CLayerDlg::Refresh
//                    (layersdlg.cpp:293-327)
//   * "New"        : name edit + picked row as parent → LayerNew_Apply    (OnNew)
//   * "Delete"     : Layers_CanDeleteLayer guard, THEN the confirm, THEN
//                    LayerDelete_Apply                                   (OnDelete)
//   * "Rename"     : picked row + name edit → LayerRename_Apply          (OnRename)
//   * "Assign Sel" : Layers_AssignSelectionToLayer                       (OnAssign)
//   * "Select All" : Select_BrushByLayer                                 (OnSelectAll)
//   * "Make Active": Layers_SetActive                                    (OnMakeActive)
//   * "Hide"/"Show": Layers_SetHidden( name, true/false )                (OnHide/OnShow)
// The panel-flow differences are the sanctioned Phase-3 divergences noted in
// RADIANT_UI_REWORK_PLAN.md: the panel stays open, the list is re-gathered every frame
// instead of being push-populated by Refresh()/LayersDlg_RefreshIfOpen (so no explicit
// refresh call follows an Apply), the MessageBoxA confirm becomes an ImGui modal, and the
// selection-requiring buttons are DISABLED rather than early-returning from a click.
//
// The MENU233 right-click ops (Hide/Show/Freeze/Collapse/Expand, each with an "and
// Children" variant) are NOT here: LayerSubtreeOp_Apply is reachable, but the five
// function-pointer helpers it is called with (LY_ApplyHide/Show/Freeze/Collapse/Expand,
// layersdlg.cpp:135-139) are file-static, so no other TU can name them.  Orchestrator
// to-do — see the report / the header comment there.
//
// NO HWND/MFC anywhere in this file: every read is a *_Gather and every write a *_Apply
// (or one of the UI-independent Layers_* cores).
#include "stdafx.h"
#include "qe3.h"
#include "radiant_ui_actions.h"
#include <imgui/imgui.h>
#include <string>
#include <utility>
#include <vector>

// ── layersdlg.cpp bindings ────────────────────────────────────────────────────
// The (full path, flags) rows + the active layer's name — the read behind the MFC
// listbox population (layersdlg.cpp:221).  Signature must match exactly; the layer
// backend has no shared header for the dialog-side entry points yet.
extern void LayerList_Gather( std::vector<std::pair<std::string,int>> &layers,
                              const char *&active );                    // layersdlg.cpp:221
extern void Layers_AssignSelectionToLayer( const char *layerName );     // layersdlg.cpp:32  (0x41C850)
extern bool Layers_SetHidden( const char *layerName, bool hidden );     // layersdlg.cpp:46  (0x41CA60/0x41CB80)
// LayerNew_Apply / Layers_CanDeleteLayer / LayerDelete_Apply / LayerRename_Apply come
// from radiant_ui_actions.h; Layers_SetActive comes from qe3.h:1068.

// ── select.cpp binding ────────────────────────────────────────────────────────
// Takes a NON-const char* (it only reads it — select.cpp:1524-1554), so the picked
// name is handed over through a mutable copy, exactly as the MFC handler does with its
// LY_SelectedName stack buffer.
extern void Select_BrushByLayer( char *layer_str );                     // select.cpp:1547 (0x48EE10)

// ── panel state ───────────────────────────────────────────────────────────────
static bool s_showLayers = false;

// The list cursor is the picked layer's FULL PATH, not a row index: New/Rename/Delete
// reorder and re-key the sorted layerMap, so an index means a different layer after the
// next gather.  (CLayerDlg::Refresh restores the cursor by index — layersdlg.cpp:297 +
// 317-318 — which is the restore-by-index bug this panel deliberately does not
// reproduce.)  1100 bytes = the MFC handlers' layer-path buffers (layersdlg.cpp:451 etc).
static char s_sel[1100] = { 0 };

// The Name edit, used by New (leaf under the picked parent) and Rename (new leaf).  260
// bytes = the MFC edit's read buffer (layersdlg.cpp:437/468).
static char s_name[260] = { 0 };

static void Panel_CopyField( char *dst, size_t dstSz, const char *src )
{
    dst[0] = '\0';
    if ( src )
    {
        strncpy( dst, src, dstSz - 1 );
        dst[dstSz - 1] = '\0';
    }
}

// ── the layer list ────────────────────────────────────────────────────────────
// Row text is composed exactly as CLayerDlg::Refresh composes its LB_ADDSTRING line
// (layersdlg.cpp:303-316): mask 0x10 off the flags, then " [hidden]" (1) / " [prefab]"
// (2) / " [frozen]" (8) in that order, behind a "* " marker for the active layer and two
// spaces for everything else.  Bit 4 (expanded) is not annotated there and is not here.
static void Panel_DrawLayerList( const std::vector<std::pair<std::string,int>> &layers,
                                 const char *active )
{
    ImGui::SeparatorText( "Layers" );

    if ( ImGui::BeginChild( "##layerlist", ImVec2( 0.0f, 240.0f ), ImGuiChildFlags_Borders ) )
    {
        for ( size_t i = 0; i < layers.size(); ++i )
        {
            const int flags = layers[i].second & ~0x10;
            char      tag[64] = { 0 };
            if ( flags & 1 ) strcat( tag, " [hidden]" );
            if ( flags & 2 ) strcat( tag, " [prefab]" );
            if ( flags & 8 ) strcat( tag, " [frozen]" );

            const bool isActive = !strcmp( active, layers[i].first.c_str() );

            char line[1100];
            _snprintf( line, sizeof( line ) - 1, "%s%s%s",
                       isActive ? "* " : "  ", layers[i].first.c_str(), tag );
            line[sizeof( line ) - 1] = '\0';

            // The row's SELECTION VALUE is the bare full path — the panel never has to
            // strip a marker or a tag back off the display text (the MFC side does that
            // in LY_SelectedName, layersdlg.cpp:331-352).
            ImGui::PushID( (int)i );
            if ( ImGui::Selectable( line, !strcmp( s_sel, layers[i].first.c_str() ) ) )
                Panel_CopyField( s_sel, sizeof( s_sel ), layers[i].first.c_str() );
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

// ── exports ───────────────────────────────────────────────────────────────────
// The panel toggle, drawn inside the shell window's panel menu (imgui_shell.cpp →
// ImGuiPanels_Menu).
void ImGuiPanel_Layers_MenuItem()
{
    ImGui::Checkbox( "Layers", &s_showLayers );
}

// U-CMD-2: the menu/accelerator route.  Radiant_DispatchCommandDirect (mainfrm.cpp) calls
// this for command 33954, where the MFC handler called CLayerDlg::Toggle() — the same
// show/hide flip, over the flag the checkbox above drives.
void ImGuiPanel_Layers_Toggle()
{
    s_showLayers = !s_showLayers;
}

void ImGuiPanel_Layers_Draw()
{
    if ( !s_showLayers )
        return;

    if ( ImGui::Begin( "Layers", &s_showLayers ) )
    {
        std::vector<std::pair<std::string,int>> layers;
        const char                             *active = nullptr;
        LayerList_Gather( layers, active );
        if ( !active )                          // g_activeLayer_string is an array, but the
            active = "";                        // gather hands it out as a pointer

        // Drop a cursor whose layer is gone (Delete, or a Rename that re-pathed it, or a
        // map load).  This is what the name-keyed cursor buys: no stale row survives.
        if ( s_sel[0] )
        {
            bool found = false;
            for ( size_t i = 0; i < layers.size() && !found; ++i )
                found = !strcmp( s_sel, layers[i].first.c_str() );
            if ( !found )
                s_sel[0] = '\0';
        }

        Panel_DrawLayerList( layers, active );
        ImGui::Text( "Active: %s", active );    // = the s_lyActive status label

        ImGui::SetNextItemWidth( -1.0f );
        ImGui::InputText( "Name", s_name, sizeof( s_name ) );

        const bool haveSel = ( s_sel[0] != '\0' );

        // New is the one command that works with NO selection: the picked layer is only
        // the PARENT path, and "" means a root layer (LayerNew_Apply's contract, and what
        // OnNew's empty LY_SelectedName buffer passes — layersdlg.cpp:440-443).
        if ( ImGui::Button( "New" ) )
            LayerNew_Apply( s_name, s_sel );
        ImGui::SameLine();

        // Everything below acts ON the picked layer; the MFC handlers early-return when
        // nothing is picked, so the buttons are simply disabled here.
        ImGui::BeginDisabled( !haveSel );

        // = OnDelete's order: the built-ins are not deletable and get NO prompt either
        // (layersdlg.cpp:454-455), so the guard runs BEFORE the confirm opens.
        if ( ImGui::Button( "Delete" ) && Layers_CanDeleteLayer( s_sel ) )
            ImGui::OpenPopup( "Delete Layer?" );
        ImGui::SameLine();

        if ( ImGui::Button( "Rename" ) )
            LayerRename_Apply( s_sel, s_name );

        if ( ImGui::Button( "Assign Sel" ) )
            Layers_AssignSelectionToLayer( s_sel );
        ImGui::SameLine();

        if ( ImGui::Button( "Select All" ) )
        {
            char mutableName[1100];             // Select_BrushByLayer takes char*
            Panel_CopyField( mutableName, sizeof( mutableName ), s_sel );
            Select_BrushByLayer( mutableName );
        }
        ImGui::SameLine();

        if ( ImGui::Button( "Make Active" ) )
            Layers_SetActive( s_sel );

        if ( ImGui::Button( "Hide" ) )
            Layers_SetHidden( s_sel, true );
        ImGui::SameLine();
        if ( ImGui::Button( "Show" ) )
            Layers_SetHidden( s_sel, false );

        ImGui::EndDisabled();

        // The MB_YESNO confirm, as a modal.  Submitted OUTSIDE the disabled block so the
        // popup's own buttons stay live, and the layer is re-guarded on Yes in case the
        // cursor moved between the click and the answer.
        if ( ImGui::BeginPopupModal( "Delete Layer?", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::Text( "Delete Layer?\n\n%s", s_sel );
            ImGui::Separator();
            if ( ImGui::Button( "Yes" ) )
            {
                LayerDelete_Apply( s_sel );     // re-layers the victim's brushes onto active
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ImGui::Button( "No" ) )
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        ImGui::TextDisabled( "New nests under the picked layer; Rename replaces its path leaf" );
    }
    ImGuiShell_CloseOnFocusLoss( &s_showLayers );
    ImGui::End();
}

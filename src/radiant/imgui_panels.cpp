// imgui_panels.cpp — UI-rework Phase 3: ImGui panels over the Phase-1 action
// functions (radiant_ui_actions.h). New KISAK code; visible only under -imgui.
// Panel semantics vs the MFC dialogs: same core calls in the same order; the
// modal-flow differences (panels stay open, undo brackets open on click rather
// than around a modal) are the sanctioned Phase-3 divergences noted in
// RADIANT_UI_REWORK_PLAN.md.
#include "stdafx.h"
#include "qe3.h"
#include "radiant_ui_actions.h"
#include <imgui/imgui.h>

// The undo API has no shared header — every TU declares it locally (the
// mainfrm.cpp:6216 pattern). Used by the Thicken panel's click bracket.
extern void Undo_ClearRedo();               // undo.cpp
extern void Undo_GeneralStart( const char *op );
extern void Undo_AddBrushList( selbrush_t *list );
extern void Undo_EndBrushList( selbrush_t *list );
extern void Undo_End();

static bool s_showGoTo      = false;
static bool s_showArbRotate = false;
static bool s_showFindBrush = false;
static bool s_showScale     = false;
static bool s_showThicken   = false;
static bool s_showDemo      = false;

// Panel toggles, drawn inside the shell window (imgui_shell.cpp).
void ImGuiPanels_Menu()
{
    ImGui::SeparatorText( "Panels" );
    ImGui::Checkbox( "Go to position",     &s_showGoTo );
    ImGui::Checkbox( "Arbitrary rotation", &s_showArbRotate );
    ImGui::Checkbox( "Find brush",         &s_showFindBrush );
    ImGui::Checkbox( "Scale selection",    &s_showScale );
    ImGui::Checkbox( "Thicken patch",      &s_showThicken );
    {
        extern void ImGuiPanel_Surface_MenuItem();   // imgui_panel_surface.cpp
        extern void ImGuiPanel_Entity_MenuItem();    // imgui_panel_entity.cpp
        extern void ImGuiPanel_FindTex_MenuItem();   // imgui_panel_findtex.cpp
        extern void ImGuiPanel_Layers_MenuItem();    // imgui_panel_layers.cpp
        extern void ImGuiPanel_DynEnt_MenuItem();    // imgui_panel_dynent.cpp
        extern void ImGuiPanel_Vehicle_MenuItem();   // imgui_panel_vehicle.cpp
        extern void ImGuiPanel_Model_MenuItem();     // imgui_panel_model.cpp
        extern void ImGuiPanel_VertEdit_MenuItem();  // imgui_panel_vertedit.cpp
        extern void ImGuiPanel_KVSelect_MenuItem();  // imgui_panel_kvselect.cpp
        extern void ImGuiPanel_MapInfo_MenuItem();   // imgui_panel_mapinfo.cpp
        extern void ImGuiPanel_Prefs_MenuItem();     // imgui_panel_prefs.cpp
        extern void ImGuiPanel_Patch_MenuItem();     // imgui_panel_patch.cpp
        extern void ImGuiPanel_Commands_MenuItem();  // imgui_panel_commands.cpp
        extern void ImGuiPanel_AdvPatch_MenuItem();  // imgui_panel_advpatch.cpp
        ImGuiPanel_Surface_MenuItem();
        ImGuiPanel_Entity_MenuItem();
        ImGuiPanel_FindTex_MenuItem();
        ImGuiPanel_Layers_MenuItem();
        ImGuiPanel_DynEnt_MenuItem();
        ImGuiPanel_Vehicle_MenuItem();
        ImGuiPanel_Model_MenuItem();
        ImGuiPanel_VertEdit_MenuItem();
        ImGuiPanel_KVSelect_MenuItem();
        ImGuiPanel_MapInfo_MenuItem();
        ImGuiPanel_Prefs_MenuItem();
        ImGuiPanel_Patch_MenuItem();
        ImGuiPanel_Commands_MenuItem();
        ImGuiPanel_AdvPatch_MenuItem();
    }
    ImGui::Checkbox( "ImGui demo",         &s_showDemo );
}

// ─────────────────────────────────────────────────────────────────────────────
// U-CMD-2: the menu/accelerator route for the five warm-up panels.  Radiant_Dispatch-
// CommandDirect (mainfrm.cpp) calls these where the MFC handlers popped the matching
// dialog — the same show/hide flip, over the flags the checkboxes above drive:
//   33107 OnMiscGoToPosition            CGoToDlg::Show()
//   33033 OnSelectionArbitraryrotation  CArbRotateDlg::Show()
//   33023 OnMiscFindbrush               CFindBrushDlg::Show()
//   32809 OnSelectScale                 CScaleDialog::DoModal()
//   32904 OnCurveThicken                CDialogThick::DoModal()
// The two MODAL ones (Scale / Thicken) keep the Phase-3 divergence documented in the
// panel bodies below: the panel stays open and brackets undo around the click.
// ─────────────────────────────────────────────────────────────────────────────
void ImGuiPanel_GoTo_Toggle()
{
    s_showGoTo = !s_showGoTo;
}

void ImGuiPanel_ArbRotate_Toggle()
{
    s_showArbRotate = !s_showArbRotate;
}

void ImGuiPanel_FindBrush_Toggle()
{
    s_showFindBrush = !s_showFindBrush;
}

void ImGuiPanel_Scale_Toggle()
{
    s_showScale = !s_showScale;
}

void ImGuiPanel_Thicken_Toggle()
{
    s_showThicken = !s_showThicken;
}

void ImGuiPanels_Draw()
{
    if ( s_showDemo )
        ImGui::ShowDemoWindow( &s_showDemo );

    {
        extern void ImGuiPanel_Surface_Draw();       // imgui_panel_surface.cpp
        extern void ImGuiPanel_Entity_Draw();        // imgui_panel_entity.cpp
        extern void ImGuiPanel_FindTex_Draw();       // imgui_panel_findtex.cpp
        extern void ImGuiPanel_Layers_Draw();        // imgui_panel_layers.cpp
        extern void ImGuiPanel_DynEnt_Draw();        // imgui_panel_dynent.cpp
        extern void ImGuiPanel_Vehicle_Draw();       // imgui_panel_vehicle.cpp
        extern void ImGuiPanel_Model_Draw();         // imgui_panel_model.cpp
        extern void ImGuiPanel_VertEdit_Draw();      // imgui_panel_vertedit.cpp
        extern void ImGuiPanel_KVSelect_Draw();      // imgui_panel_kvselect.cpp
        extern void ImGuiPanel_MapInfo_Draw();       // imgui_panel_mapinfo.cpp
        extern void ImGuiPanel_Prefs_Draw();         // imgui_panel_prefs.cpp
        extern void ImGuiPanel_Patch_Draw();         // imgui_panel_patch.cpp
        extern void ImGuiPanel_PatchDensity_Draw();  // imgui_panel_patchdensity.cpp
        extern void ImGuiPanel_Filters_Draw();       // imgui_panel_filters.cpp
        extern void ImGuiPanel_Commands_Draw();      // imgui_panel_commands.cpp
        extern void ImGuiPanel_AdvPatch_Draw();      // imgui_panel_advpatch.cpp
        ImGuiPanel_Surface_Draw();
        ImGuiPanel_Entity_Draw();
        ImGuiPanel_FindTex_Draw();
        ImGuiPanel_Layers_Draw();
        ImGuiPanel_DynEnt_Draw();
        ImGuiPanel_Vehicle_Draw();
        ImGuiPanel_Model_Draw();
        ImGuiPanel_VertEdit_Draw();
        ImGuiPanel_KVSelect_Draw();
        ImGuiPanel_MapInfo_Draw();
        ImGuiPanel_Prefs_Draw();
        ImGuiPanel_Patch_Draw();
        ImGuiPanel_PatchDensity_Draw();
        ImGuiPanel_Filters_Draw();
        ImGuiPanel_Commands_Draw();
        ImGuiPanel_AdvPatch_Draw();
    }

    if ( s_showGoTo )
    {
        if ( ImGui::Begin( "Go to position", &s_showGoTo, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            static char coords[128] = { 0 };
            ImGui::TextUnformatted( "Position (e.g.  256 256 16  or  (256, 256, 16)):" );
            bool go = ImGui::InputText( "##gotocoords", coords, sizeof( coords ),
                                        ImGuiInputTextFlags_EnterReturnsTrue );
            go |= ImGui::Button( "Go" );
            if ( go )
                GoTo_Apply( coords );
        }
        ImGuiShell_CloseOnFocusLoss( &s_showGoTo );
        ImGui::End();
    }

    if ( s_showArbRotate )
    {
        if ( ImGui::Begin( "Arbitrary rotation", &s_showArbRotate, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            static float deg[3] = { 0.0f, 0.0f, 0.0f };
            ImGui::InputFloat( "X axis (deg)", &deg[0] );
            ImGui::InputFloat( "Y axis (deg)", &deg[1] );
            ImGui::InputFloat( "Z axis (deg)", &deg[2] );
            if ( ImGui::Button( "OK" ) )
                ArbRotate_Apply( deg[0], deg[1], deg[2] );
        }
        ImGuiShell_CloseOnFocusLoss( &s_showArbRotate );
        ImGui::End();
    }

    if ( s_showFindBrush )
    {
        if ( ImGui::Begin( "Find brush", &s_showFindBrush, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            static int entIdx = 0, brushIdx = 0;
            ImGui::InputInt( "Entity number", &entIdx );
            ImGui::InputInt( "Brush number", &brushIdx );
            if ( ImGui::Button( "Find" ) )
                FindBrush_Apply( brushIdx, entIdx );
        }
        ImGuiShell_CloseOnFocusLoss( &s_showFindBrush );
        ImGui::End();
    }

    if ( s_showScale )
    {
        if ( ImGui::Begin( "Scale selection", &s_showScale, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            static float sc[3] = { 1.0f, 1.0f, 1.0f };
            ImGui::InputFloat( "X", &sc[0] );
            ImGui::InputFloat( "Y", &sc[1] );
            ImGui::InputFloat( "Z", &sc[2] );
            if ( ImGui::Button( "Scale" ) )
                SelectScale_Apply( sc[0], sc[1], sc[2] );   // validations + undo bracket inside
        }
        ImGuiShell_CloseOnFocusLoss( &s_showScale );
        ImGui::End();
    }

    if ( s_showThicken )
    {
        if ( ImGui::Begin( "Thicken patch", &s_showThicken, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            static int  amount = 8;
            static bool seam   = true;
            ImGui::InputInt( "Amount", &amount );
            ImGui::Checkbox( "Seam", &seam );
            if ( ImGui::Button( "Thicken" ) )
            {
                // The MFC caller brackets around the MODAL; panel flow brackets
                // around the click (sanctioned divergence — no empty undo record
                // on cancel).
                Undo_ClearRedo();
                Undo_GeneralStart( "curve thicken" );
                Undo_AddBrushList( &selected_brushes );
                CurveThicken_Apply( amount, seam );
                Undo_EndBrushList( &selected_brushes );
                Undo_End();
            }
        }
        ImGuiShell_CloseOnFocusLoss( &s_showThicken );
        ImGui::End();
    }
}

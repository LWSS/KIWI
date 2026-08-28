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
        extern void KiwiGrass_MenuItem();            // kiwi_grass.cpp
        extern void ImGuiPanel_LyrMtl_MenuItem();       // imgui_panel_lyrmtl.cpp
        extern void ImGuiPanel_ScriptGroup_MenuItem();  // imgui_panel_scriptgroup.cpp
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
        KiwiGrass_MenuItem();
        ImGuiPanel_LyrMtl_MenuItem();
        ImGuiPanel_ScriptGroup_MenuItem();
    }
    ImGui::Checkbox( "ImGui demo",         &s_showDemo );

    // KIWI-UX: RADIANT_UX_DESIGN Phase-1b switches — the modern-input master toggle,
    // the grid/axes/hover additions and the §17 units + grid-spacing prefs.  Phase 2 adds
    // the §11 keymap-profile switcher and the §6 snap-marker toggle inside the same block,
    // plus the §15 palette entry so CLASSIC-keymap users reach it without the modern F.
    {
        extern void KiwiUX_DrawSettings();   // kiwi_ux.cpp
        extern void KiwiPalette_MenuItem();  // kiwi_palette.cpp
        KiwiUX_DrawSettings();
        KiwiPalette_MenuItem();
        // Phase 4 (§7/§16/§23): the "Construct" submenu — the drawing tools, the
        // construction planes and Extrude Region.  None of them binds a key this
        // phase (kiwi_construct.h KEYS note), so this and the palette ARE the route
        // in BOTH keymap profiles.
        extern void KiwiCon_MenuItems();     // kiwi_construct.cpp
        KiwiCon_MenuItems();
        // Shakeout C (§16b): the four SOLID primitives — the one-gesture answer
        // to "how do I make a brush in the 3D view" — and the Shift+A add menu
        // that lists every creator, so the CLASSIC keymap reaches it too.
        extern void KiwiPrim_MenuItems();    // kiwi_primitive.cpp
        extern void KiwiAdd_MenuItem();      // kiwi_addmenu.cpp
        KiwiPrim_MenuItems();
        KiwiAdd_MenuItem();
        // Phase 5 (§24/§25): CSG workflow, bevel/inset, mirror + arrays and the
        // selection-expansion helpers.  Same ruling as the Construct block — none
        // of them binds a key this phase, so this block and the command palette
        // ARE the route in BOTH keymap profiles.
        extern void KiwiCsg_MenuItems();     // kiwi_csg.cpp
        extern void KiwiBevel_MenuItems();   // kiwi_bevel.cpp
        extern void KiwiDupe_MenuItems();    // kiwi_dupe.cpp
        extern void KiwiSelExt_MenuItems();  // kiwi_selext.cpp
        KiwiCsg_MenuItems();
        KiwiBevel_MenuItems();
        KiwiDupe_MenuItems();
        KiwiSelExt_MenuItems();
        // Phase 6 (§26): the UV workflow v1 — texture shift/rotate/scale + the
        // texture pick.  Same ruling again: no new key bindings this phase, so
        // this block and the command palette ARE the route in BOTH profiles (the
        // classic middle-button pick over the 3D view still works too).
        extern void KiwiUv_MenuItems();      // kiwi_uv.cpp
        KiwiUv_MenuItems();
    }
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
        extern void KiwiGrass_Draw();                // kiwi_grass.cpp
        extern void ImGuiPanel_LyrMtl_Draw();        // imgui_panel_lyrmtl.cpp
        extern void ImGuiPanel_ScriptGroup_Draw();   // imgui_panel_scriptgroup.cpp
        extern void ImGuiPanel_Project_Draw();       // imgui_panel_project.cpp
        extern void ImGuiPanel_Sides_Draw();         // imgui_panel_sides.cpp
        ImGuiPanel_Surface_Draw();
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
        KiwiGrass_Draw();
        ImGuiPanel_LyrMtl_Draw();
        ImGuiPanel_ScriptGroup_Draw();
        ImGuiPanel_Project_Draw();
        ImGuiPanel_Sides_Draw();
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

    // KIWI-UX (RADIANT_UX_DESIGN §15, Phase 2): the command palette.  Drawn LAST and at TOP
    // LEVEL (not inside a viewport window) so it centres over the whole dockspace and paints
    // above everything else.  Deliberately NOT run through ImGuiShell_CloseOnFocusLoss — the
    // palette closes on Esc / on running a row, and a click-off close would fight its own
    // auto-focus (the panel auto-close latch is for docked tool panels, not a transient popup).
    {
        extern void KiwiPalette_Draw();   // kiwi_palette.cpp
        KiwiPalette_Draw();
        // KIWI-UX (§16b, shakeout C): the Shift+A add menu, same placement rule —
        // top level, drawn last, so it paints above every docked window.  It is
        // cursor-anchored rather than centred, which is the only difference.
        extern void KiwiAdd_Draw();       // kiwi_addmenu.cpp
        KiwiAdd_Draw();
    }
}

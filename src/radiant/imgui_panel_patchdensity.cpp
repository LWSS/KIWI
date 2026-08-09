// imgui_panel_patchdensity.cpp — UI-rework Phase 5: ImGui "Patch density" prompt over the
// patchdialog.cpp core (PatchDensity_Apply / PatchDensity_Commit). New KISAK code.
//
// Replaces CPatchDensityDlg (IDD_PATCH_DENSITY 157), the MODAL CDialog that Curve → Simple
// Patch Mesh (cmd 32856) and Curve → Simple Terrain Patch (cmd 32939) ran. That dialog was
// left a NO-OP stub in the no-MFC shell (patchdialog.cpp DoSimpleTerrainPatchMesh), so those
// two menu items opened an empty undo bracket and made nothing — this panel wires them back.
//
// Two dropdowns pick the patch density; "Create" commits (undo-bracketed) via
// PatchDensity_Commit. Same dimension tables as the binary: patch = {3,5,7,9,11,13,15}
// (combo index → PatchDensity_Apply's s_patchDensityDims[sel]); terrain = width sel+2, so
// 2..16 across the 15 valid selections (Create_Terrain(sel+2, ...)). No HWND/MFC here — the
// only binds are the two UI-independent free functions in patchdialog.cpp.
#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

extern void ImGuiShell_CloseOnFocusLoss( bool *p_open );          // imgui_shell.cpp
extern void PatchDensity_Commit( bool terrain, int wSel, int hSel ); // patchdialog.cpp

static bool s_show    = false;
static bool s_terrain = false;

// Remembered across opens (the panel-side twin of g_patchDensity/terrainLastWidthSel). Kept
// separate per mode so switching Simple ↔ Terrain restores each mode's last pick.
static int  s_patchW = 0, s_patchH = 0;    // index into {3,5,7,9,11,13,15}
static int  s_terrW  = 0, s_terrH  = 0;    // index into {2 .. 16}

// Combo labels. Patch: the binary's 7-entry density table. Terrain: 15 entries 2..16.
static const char *const s_patchItems[7]  = { "3", "5", "7", "9", "11", "13", "15" };
static const char *const s_terrItems[15]  = { "2", "3", "4", "5", "6", "7", "8", "9",
                                              "10", "11", "12", "13", "14", "15", "16" };

// The menu/command route (patchdialog.cpp DoSimplePatchMesh / DoSimpleTerrainPatchMesh call
// this once QE_SingleBrush passes). Opening does NOT reset the remembered selection.
void ImGuiPanel_PatchDensity_Open( bool terrain )
{
    s_terrain = terrain;
    s_show    = true;
}

void ImGuiPanel_PatchDensity_Draw()
{
    if ( !s_show )
        return;

    const char *title = s_terrain ? "Simple Terrain Patch###PatchDensity"
                                   : "Simple Patch Mesh###PatchDensity";
    ImGui::SetNextWindowSize( ImVec2( 260.0f, 0.0f ), ImGuiCond_FirstUseEver );
    if ( ImGui::Begin( title, &s_show, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        int  *pw    = s_terrain ? &s_terrW : &s_patchW;
        int  *ph    = s_terrain ? &s_terrH : &s_patchH;
        const char *const *items = s_terrain ? s_terrItems : s_patchItems;
        const int   count = s_terrain ? 15 : 7;

        ImGui::TextUnformatted( "Patch density (control points):" );
        ImGui::Spacing();
        ImGui::SetNextItemWidth( 120.0f );
        ImGui::Combo( "Width", pw, items, count );
        ImGui::SetNextItemWidth( 120.0f );
        ImGui::Combo( "Height", ph, items, count );

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if ( ImGui::Button( "Create", ImVec2( 110.0f, 0.0f ) ) )
        {
            PatchDensity_Commit( s_terrain, *pw, *ph );
            s_show = false;
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
            s_show = false;
    }
    ImGuiShell_CloseOnFocusLoss( &s_show );
    ImGui::End();
}

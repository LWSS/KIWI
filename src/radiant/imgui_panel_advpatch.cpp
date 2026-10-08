// imgui_panel_advpatch.cpp — the Advanced Patch Editor (Y / cmd 33130) entry points.
//
// The dialog itself is superseded by Terrain Sculpt (kiwi_terrain.cpp): the Y key, the
// panel-menu checkbox and the ImGuiPanels draw pass all forward there.  KIWI: the ported
// dialog's paint path (and the CurvEditDlg slot table it seeded) is gone.
#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>
#include "kiwi_terrain.h"

void ImGuiPanel_AdvPatch_MenuItem()
{
    KiwiTerrain_MenuItem();
}

// U-CMD-2: cmd 33130 (Patch -> Advanced Edit, key Y) — Radiant_DispatchCommandDirect.
// KIWI: Y arms / disarms the terrain tool (the panel toggle stays on the Windows menu).
void ImGuiPanel_AdvPatch_Toggle()
{
    KiwiTerrain_ToggleArmed();
}

void ImGuiPanel_AdvPatch_Draw()
{
    KiwiTerrain_Draw();
}

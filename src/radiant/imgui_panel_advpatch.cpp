// imgui_panel_advpatch.cpp — the Advanced Patch Editor (Y / cmd 33130) entry points.
//
// The dialog itself is superseded by Terrain Sculpt (kiwi_terrain.cpp): the Y key, the
// panel-menu checkbox and the ImGuiPanels draw pass all forward there.  What survives
// here is the one piece of the ported dialog other code still depends on: seeding the
// CurvEditDlg control-table slots (inner / outer radius, amplitude) at startup, which
// the legacy soft-select vertex drag (mode 1, sub_43DA20) and its radius getters
// sub_401BB0 / sub_401C00 / sub_401C50 read.  Terrain Sculpt pushes its own radii into
// those slots whenever the legacy soft-select option is enabled.
#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>
#include "kiwi_terrain.h"

// pmesh.cpp — the CurvEditDlg control table (OnInitDialog's bind half, patchdialog.cpp:101-103).
extern void  CurveEdit_BindData( int slot, int trackbarId, int editId, float defVal,
                                 float mn, float mx, float step );   // pmesh.cpp:4942

// The 3 param slots exactly as the dialog's OnInitDialog bound them; the defaults are the
// KIWI ones (64 / 256) so a typical CoD4 terrain has control points inside the radius.
struct advPatchSlot_t
{
    int   trackbarId, editId;
    float defVal, mn, mx, step;
};
static const advPatchSlot_t s_slotDef[3] =
{
    { 1424, 1428,  64.0f, 0.0f, 1024.0f, 16.0f },   // inner radius
    { 1425, 1429, 256.0f, 0.0f, 1024.0f, 16.0f },   // outer radius
    { 1426, 1430,  11.0f, 0.0f,   16.0f,  1.0f },   // amplitude (stored log2+8 -> 2^3 = 8)
};

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
    static bool s_bound = false;   // one-shot per process, like OnInitDialog per CREATE
    if ( !s_bound )
    {
        s_bound = true;
        for ( int slot = 0; slot < 3; ++slot )
        {
            const advPatchSlot_t &d = s_slotDef[slot];
            CurveEdit_BindData( slot, d.trackbarId, d.editId, d.defVal, d.mn, d.mx, d.step );
        }
    }
    KiwiTerrain_Draw();
}

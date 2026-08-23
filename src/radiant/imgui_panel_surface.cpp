// imgui_panel_surface.cpp — UI-rework Phase 3 unit 4: ImGui Surface inspector over
// the surfacedlg.cpp core actions. New KISAK code; visible only under -imgui.
// Panel semantics vs CSurfaceDlg: same core calls in the same order (Gather → edit →
// Apply/Spin, and the Fit/Natural/CAP/Lmap/Set buttons); the panel-stays-open flow and
// the explicit Refresh button are the sanctioned Phase-3 divergences noted in
// RADIANT_UI_REWORK_PLAN.md. No HWND path here: the dialog's Surf_RefreshFields and the
// SetTexMods/Wnd02 scratch transaction are all gated on surfDlgGlob.hwnd, so the panel
// re-Gathers its own fields instead.
#include "stdafx.h"
#include "qe3.h"
#include "kiwi_fmt.h"
#include "radiant_ui_actions.h"
#include <imgui/imgui.h>

// MUST MATCH surfacedlg.cpp verbatim (shared-header consolidation pending)
struct surfaceDlgState_t
{
    char  currTex[132];    // IDC_SURFACE_INSP_CURR_TEX (in; the resolved name comes back out)
    float horzShift;       // IDC_SURFACE_INSP_HORZ_SHIFT_IN   (texels)
    float vertShift;       // IDC_SURFACE_INSP_VERT_SHIFT_IN
    float horzStretch;     // IDC_SURFACE_INSP_HORZ_STRETCH    ("stretch" = size/width; raw for a patch)
    float vertStretch;     // IDC_SURFACE_INSP_VERT_STRETCH
    float rotate;          // IDC_SURFACE_INSP_ROTATE          (degrees)
    float sampleSize;      // IDC_SURFACE_INSP_SAMPLE_SIZE     (shared g_patch_texdef.sample_size)
    float repeatsX;        // IDC_SURFACE_INSP_TXT_REPEATS_X   (raw texel size[0])
    float repeatsY;        // IDC_SURFACE_INSP_TXT_REPEATS_Y   (raw texel size[1])
    int   bPatchMode;      // m_bPatchMode
    char  texdefDirty;     // m_texdefDirty
    char  sampleDirty;     // m_sampleDirty
};

// ── surfacedlg.cpp — the struct-coupled inspector core (not in radiant_ui_actions.h) ──
extern bool SurfaceDlg_Gather( surfaceDlgState_t &out );                     // Select_SetTexture_2 read pass
extern void SurfaceDlg_Apply( surfaceDlgState_t &st );                       // GetTexMods commit
extern void SurfaceDlg_Spin( surfaceDlgState_t &st, int idFrom, bool up );   // UpdateSpinners
extern void SurfaceFit_Apply();                                              // "Fit" button
extern void SurfaceNaturalize_Apply();                                       // "Natural" button
extern void SurfaceCap_Apply( float xSize, float ySize );                    // "CAP" button
extern void SurfaceLightmap_Apply();                                         // "Lmap" button
extern void SurfaceSet_Apply( float v5, float v6, int checked );             // "Set..." button

// The control ids SurfaceDlg_Spin / SurfaceSet_Apply dispatch on (res/resource.h values,
// copied so this panel needs no MFC resource header).
static const int SI_ID_HORZ_SHIFT_SPIN   = 1248;   // IDC_SURFACE_INSP_HORZ_SHIFT_SPIN
static const int SI_ID_VERT_SHIFT_SPIN   = 1251;   // IDC_SURFACE_INSP_VERT_SHIFT_SPIN
static const int SI_ID_VERT_STRETCH_SPIN = 1254;   // IDC_SURFACE_INSP_VERT_STRETCH_SPIN
static const int SI_ID_HORZ_STRETCH_SPIN = 1257;   // IDC_SURFACE_INSP_HORZ_STRETCH_SPIN
static const int SI_ID_ROTATE_SPIN       = 1259;   // IDC_SURFACE_INSP_ROTATE_SPIN
static const int SI_ID_SAMPLE_SIZE_SPIN  = 1260;   // IDC_SURFACE_INSP_SAMPLE_SIZE_SPIN
static const int SI_ID_PATCH_2D          = 1477;   // IDC_SURFACE_INSP_PATCH_2D    (radio "Terrain uses 2D distance")
static const int SI_ID_PATCH_3D          = 1478;   // IDC_SURFACE_INSP_PATCH_3D    (radio "Terrain uses 3d distance")
static const int SI_ID_PATCH_CURVE       = 1479;   // IDC_SURFACE_INSP_PATCH_CURVE (radio "Terrain emulates curves")

static bool s_showSurface = false;

// The panel IS the dialog's control set, so the state struct persists across frames
// (the two dirty flags stand in for m_texdefDirty / m_sampleDirty).
static surfaceDlgState_t s_state;
static bool  s_opened       = false;                // first-open latch (OnInitDialog equivalent)
static float s_texRepeat[2] = { 1.0f, 1.0f };       // IDC_SURFACE_INSP_TEX_REP_X / _Y (CAP + Set)
static int   s_setMode      = 0;                    // Set's terrain-distance radio (0 = none)

// Re-read the selection into the panel fields; Gather returning false means "nothing
// editable", and its contract is to leave the fields as-is.
static void SI_Refresh()
{
    SurfaceDlg_Gather( s_state );
}

// One texdef row: the edit field (EN_CHANGE → dirty flag) plus the up-down arrows
// (UDN_DELTAPOS → SurfaceDlg_Spin with this row's control id, then the dialog's
// trailing field refresh).
static void SI_Row( const char *label, float *value, int spinId, char *dirtyFlag )
{
    ImGui::PushID( spinId );
    ImGui::SetNextItemWidth( 90.0f );
    if ( ImGui::InputFloat( "##value", value ) )
        *dirtyFlag = 1;
    ImGui::SameLine();
    if ( ImGui::SmallButton( "-" ) )
    {
        SurfaceDlg_Spin( s_state, spinId, false );
        SI_Refresh();
    }
    ImGui::SameLine();
    if ( ImGui::SmallButton( "+" ) )
    {
        SurfaceDlg_Spin( s_state, spinId, true );
        SI_Refresh();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted( label );
    ImGui::PopID();
}

// Panel toggle, drawn inside the shell window (imgui_shell.cpp).
void ImGuiPanel_Surface_MenuItem()
{
    ImGui::Checkbox( "Surface inspector", &s_showSurface );
}

// Hotkey/menu route (S — Cmd_OnTexturesInspector; was the MFC Surf_OpenInspector).
void ImGuiPanel_Surface_Toggle()
{
    s_showSurface = !s_showSurface;
}

void ImGuiPanel_Surface_Draw()
{
    if ( !s_showSurface )
    {
        s_opened = false;      // a re-open re-seeds + re-reads the selection
        return;
    }

    if ( !s_opened )
    {
        // OnInitDialog: seed the tex-repeat edits + the terrain-mode radio from the cached
        // g_qeglobals values, clear the dirty flags, then read the selection.
        s_opened = true;
        if ( !g_qeglobals.surfInsp_tex_repeatx ) g_qeglobals.surfInsp_tex_repeatx = 1;
        if ( !g_qeglobals.surfInsp_tex_repeaty ) g_qeglobals.surfInsp_tex_repeaty = 1;
        s_texRepeat[0] = (float)g_qeglobals.surfInsp_tex_repeatx;
        s_texRepeat[1] = (float)g_qeglobals.surfInsp_tex_repeaty;
        if ( !g_qeglobals.surfInsp_nIDButton ) g_qeglobals.surfInsp_nIDButton = SI_ID_PATCH_2D;
        s_setMode = g_qeglobals.surfInsp_nIDButton;
        s_state.texdefDirty = 0;
        s_state.sampleDirty = 0;
        SI_Refresh();
    }

    if ( ImGui::Begin( "Surface inspector", &s_showSurface, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        ImGui::SetNextItemWidth( 280.0f );
        // The MFC name edit has no EN_CHANGE handler; flagging it here is a panel-flow
        // choice so a typed name reaches SetMaterial through the normal Apply path.
        if ( ImGui::InputText( "Material", s_state.currTex, sizeof( s_state.currTex ) ) )
            s_state.texdefDirty = 1;

        ImGui::SeparatorText( "Alignment" );
        SI_Row( "Horizontal shift",   &s_state.horzShift,   SI_ID_HORZ_SHIFT_SPIN,   &s_state.texdefDirty );
        SI_Row( "Vertical shift",     &s_state.vertShift,   SI_ID_VERT_SHIFT_SPIN,   &s_state.texdefDirty );
        SI_Row( "Horizontal stretch", &s_state.horzStretch, SI_ID_HORZ_STRETCH_SPIN, &s_state.texdefDirty );
        SI_Row( "Vertical stretch",   &s_state.vertStretch, SI_ID_VERT_STRETCH_SPIN, &s_state.texdefDirty );
        SI_Row( "Rotate (deg)",       &s_state.rotate,      SI_ID_ROTATE_SPIN,       &s_state.texdefDirty );
        SI_Row( "Sample size",        &s_state.sampleSize,  SI_ID_SAMPLE_SIZE_SPIN,  &s_state.sampleDirty );

        // Read-only "Repeats in" readouts (raw texel size), matching the dialog.
        char repeatsX[48], repeatsY[48];
        ImGui::Text( "Repeats in   x: %s   y: %s",
                     KiwiFmt_Num( repeatsX, sizeof( repeatsX ), s_state.repeatsX, 6 ),
                     KiwiFmt_Num( repeatsY, sizeof( repeatsY ), s_state.repeatsY, 6 ) );
        ImGui::Text( "Patch mode: %s", s_state.bPatchMode ? "yes" : "no" );

        if ( ImGui::Button( "Apply" ) )
        {
            SurfaceDlg_Apply( s_state );   // no-op unless a dirty flag is up (GetTexMods)
            SI_Refresh();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Refresh" ) )
            SI_Refresh();

        ImGui::SeparatorText( "Fit / patch texturing" );
        if ( ImGui::Button( "Fit" ) )
        {
            SurfaceFit_Apply();
            SI_Refresh();                  // OnFit is the one button path that ends in a field refresh
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Natural" ) )
            SurfaceNaturalize_Apply();
        ImGui::SameLine();
        if ( ImGui::Button( "Lmap" ) )
            SurfaceLightmap_Apply();

        ImGui::SetNextItemWidth( 90.0f );
        ImGui::InputFloat( "Texture x", &s_texRepeat[0] );
        ImGui::SetNextItemWidth( 90.0f );
        ImGui::InputFloat( "Texture y", &s_texRepeat[1] );

        if ( ImGui::Button( "CAP" ) )
            SurfaceCap_Apply( s_texRepeat[0], s_texRepeat[1] );   // bails inside if either reads 0

        // Set's terrain-distance mode (GetCheckedRadioButton equivalent; 0 = nothing
        // checked, which SurfaceSet_Apply answers with the "select a mode" hint).
        ImGui::RadioButton( "Terrain uses 2D distance", &s_setMode, SI_ID_PATCH_2D );
        ImGui::RadioButton( "Terrain uses 3d distance", &s_setMode, SI_ID_PATCH_3D );
        ImGui::RadioButton( "Terrain emulates curves",  &s_setMode, SI_ID_PATCH_CURVE );
        if ( ImGui::Button( "Set" ) )
            SurfaceSet_Apply( s_texRepeat[0], s_texRepeat[1], s_setMode );
    }
    ImGuiShell_CloseOnFocusLoss( &s_showSurface );
    ImGui::End();
}

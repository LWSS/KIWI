// imgui_panel_findtex.cpp — UI-rework Phase 3: ImGui "Find / Replace texture" panel over
// the findtexture.cpp core actions. New KISAK code; visible only under -imgui.
//
// Panel semantics vs CFindTextureDlg: the SAME core call the MFC OK/Apply handlers make,
// with the same widget reads —
//   * Find / Replace edits (IDC_FR_FIND 1097 / IDC_FR_REPLACE 1101), 256-byte buffers like
//     FR_DoReplace's findBuf/replBuf.
//   * the four checkboxes, verbatim OnCreate labels, driving the flag byte bits
//     FindTexture_Apply assembles: 1 = selected-only, 2 = force, 4 = recurse-prefabs,
//     8 = live.  Live defaults ON, as OnCreate's BM_SETCHECK does.
//   * "Replace" = OnFindReplaceApply — FR_DoReplace WITHOUT the trailing DestroyWindow.
// The panel-stays-open flow is the sanctioned Phase-3 divergence noted in
// RADIANT_UI_REWORK_PLAN.md: OK (apply + DestroyWindow) and Close (DestroyWindow) collapse
// into "Replace" plus the window's own close box, and FR_DoReplace's
// "Radiant::TextureFindWindow" GetWindowRect/SaveRegistryInfo pass is dropped — both are
// HWND-lifetime plumbing with no core-action content (ImGui persists its own window rect).
//
// NO HWND/MFC anywhere in this file: the only binds are the two UI-independent functions in
// radiant_ui_actions.h.
#include "stdafx.h"
#include "qe3.h"
#include "radiant_ui_actions.h"
#include <imgui/imgui.h>

static bool s_showFindTex = false;

// The dialog's two edit controls.  256 bytes each, matching FR_DoReplace's stack buffers
// (findtexture.cpp:403) — ImGui reserves the NUL, so the same 255-char capacity as the
// MFC ::GetWindowTextA( ..., sizeof(buf) - 1 ) reads.
static char s_find[256]    = { 0 };
static char s_replace[256] = { 0 };

// The four BS_AUTOCHECKBOX states (findtexture.cpp:371-380).  OnCreate leaves the first
// three clear and checks Live.
static bool s_bSelectedOnly   = false;   // IDC_FR_SELONLY  1105 → flag&1
static bool s_bForce          = false;   // IDC_FR_FORCE    1109 → flag&2
static bool s_bRecursePrefabs = false;   // IDC_FR_RECURSE  1525 → flag&4
static bool s_bLive           = true;    // IDC_FR_LIVE     1684 → flag&8

static bool s_opened = false;            // first-open latch (OnCreate equivalent)

// Panel toggle, drawn inside the shell window (imgui_shell.cpp).
void ImGuiPanel_FindTex_MenuItem()
{
    ImGui::Checkbox( "Find / replace texture", &s_showFindTex );
}

// U-CMD-2: the menu/accelerator route.  Radiant_DispatchCommandDirect (mainfrm.cpp) calls
// this for command 32812, where the MFC handler called CFindTextureDlg::show() — the same
// show/hide flip, over the flag the checkbox above drives.
void ImGuiPanel_FindTex_Toggle()
{
    s_showFindTex = !s_showFindTex;
}

void ImGuiPanel_FindTex_Draw()
{
    if ( !s_showFindTex )
    {
        // Hiding the panel is the DestroyWindow equivalent, so a re-open re-runs the
        // OnCreate pre-fill (CFindTextureDlg::show only skips it for a STILL-ALIVE
        // singleton, which a hidden panel is not).
        s_opened = false;
        return;
    }

    if ( !s_opened )
    {
        // OnCreate tail (findtexture.cpp:386-389): pre-fill Find with the current-texture
        // window's material name.  It returns nullptr when the active layer has no single
        // valid material — leave the field as-is, exactly like the `nm && nm[0]` guard.
        s_opened = true;
        const char *nm = FindTexture_GetCurrentMaterialName();
        if ( nm && nm[0] )
        {
            strncpy( s_find, nm, sizeof( s_find ) - 1 );
            s_find[sizeof( s_find ) - 1] = 0;
        }
    }

    if ( ImGui::Begin( "Find / Replace Texture(s)", &s_showFindTex, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        // Enter in either edit fires the replace — the IDOK BS_DEFPUSHBUTTON equivalent,
        // minus OK's DestroyWindow.
        ImGui::SetNextItemWidth( 260.0f );
        bool go = ImGui::InputText( "Find", s_find, sizeof( s_find ),
                                   ImGuiInputTextFlags_EnterReturnsTrue );
        ImGui::SetNextItemWidth( 260.0f );
        go |= ImGui::InputText( "Replace", s_replace, sizeof( s_replace ),
                                ImGuiInputTextFlags_EnterReturnsTrue );

        ImGui::Checkbox( "Use selected brushes only", &s_bSelectedOnly );
        ImGui::Checkbox( "Replace everywhere (don't test against Find)", &s_bForce );
        ImGui::Checkbox( "Recurse into prefabs (re-saves referenced .map files)", &s_bRecursePrefabs );
        ImGui::Checkbox( "Live: also copy the current texture mapping", &s_bLive );

        go |= ImGui::Button( "Replace" );
        if ( go )
        {
            // FR_DoReplace's tail call.  An empty Replace field is a no-op INSIDE the
            // action (findtexture.cpp:251), so the button needs no guard of its own; the
            // visited-set reset and the g_nUpdateBits stamp are in there too.
            FindTexture_Apply( s_find, s_replace, s_bSelectedOnly, s_bForce,
                               s_bRecursePrefabs, s_bLive );
        }
    }
    ImGuiShell_CloseOnFocusLoss( &s_showFindTex );
    ImGui::End();
}

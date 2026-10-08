// imgui_panel_cap.cpp — the open-patch "special cap" type prompt, over pmesh.cpp's Cap_Commit.
// KIWI: replaces CCapDialog (sub_40A8A0, IDD 0xA1) that Patch_CapCurrent popped when Curve >
// Cap met a patch with no closed edge: four radios (Bevel / Endcap / Inverted Bevel / Inverted
// Endcap), OK builds the two caps.  The Endcap types need a patch at least 5 columns wide.
#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

extern void Cap_Commit( int nType );   // pmesh.cpp

static bool s_show  = false;
static bool s_arm   = false;           // open the popup on the next draw
static int  s_type  = 0;               // DDX_Radio m_nType: 0 Bevel .. 3 Inverted Endcap
static int  s_width = 0;               // the patch's width when the prompt opened

void ImGuiPanel_Cap_Open( int patchWidth )
{
    s_width = patchWidth;
    if ( s_width < 5 && ( s_type == 1 || s_type == 3 ) )
        s_type = 0;
    s_show = true;
    s_arm  = true;
}

void ImGuiPanel_Cap_Draw()
{
    if ( !s_show )
        return;

    if ( s_arm )
    {
        ImGui::OpenPopup( "Cap type###CapType" );
        s_arm = false;
    }
    if ( ImGui::BeginPopupModal( "Cap type###CapType", &s_show, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        static const char *const s_names[4] = { "Bevel", "Endcap", "Inverted Bevel", "Inverted Endcap" };
        for ( int t = 0; t < 4; ++t )
        {
            const bool endcap = ( t == 1 || t == 3 );
            ImGui::BeginDisabled( endcap && s_width < 5 );
            ImGui::RadioButton( s_names[t], &s_type, t );
            ImGui::EndDisabled();
        }
        if ( s_width < 5 )
            ImGui::TextDisabled( "Endcaps need a patch at least 5 columns wide." );
        ImGui::Separator();
        const bool ok = ImGui::Button( "OK", ImVec2( 100.0f, 0.0f ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 100.0f, 0.0f ) ) )
        {
            s_show = false;
            ImGui::CloseCurrentPopup();
        }
        else if ( ok )
        {
            Cap_Commit( s_type );
            s_show = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    else
    {
        s_show = false;   // dismissed (Esc / the popup's close box)
    }
}

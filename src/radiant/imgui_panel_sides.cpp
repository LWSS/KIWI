// imgui_panel_sides.cpp — the "number of sides" prompt, over mainfrm.cpp's Sides_Commit.
// Replaces the IDD_ARBITRARY_SIDES modal (SidesDlgProc 0x495F00) that Brush -> Primitives
// Arbitrary sided (33034) / Cone (32833) / Sphere (32892) ran.
#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>
#include <stdlib.h>
#include <string.h>

extern void Sides_Commit( int sides );   // mainfrm.cpp:2513  void Sides_Commit(int)

static bool s_show = false;
static bool s_arm  = false;                      // open the popup on the next draw
static char s_title[64] = { 0 };                 // which primitive the caller armed
static char s_sides[256] = { 0 };                // the IDC_ARB_SIDES_IN edit text

// The dialog shipped NO default (an empty field atol's to 0 and the core rejects it with
// "Bad sides number"), so the buffer starts empty; it then holds whatever was typed last,
// which is what the template's edit did across opens within a session.
void ImGuiPanel_Sides_Open( const char *title )
{
    strncpy( s_title, title ? title : "Sides", sizeof( s_title ) - 1 );
    s_title[sizeof( s_title ) - 1] = '\0';
    s_show = true;
    s_arm  = true;
}

void ImGuiPanel_Sides_Draw()
{
    if ( !s_show )
        return;

    if ( s_arm )
    {
        ImGui::OpenPopup( "Number of sides###ArbSides" );
        s_arm = false;
    }
    if ( ImGui::BeginPopupModal( "Number of sides###ArbSides", &s_show,
                                 ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        ImGui::TextUnformatted( s_title );
        if ( ImGui::IsWindowAppearing() )
            ImGui::SetKeyboardFocusHere();       // WM_INITDIALOG's SetFocus on the edit
        ImGui::SetNextItemWidth( 160.0f );
        bool ok = ImGui::InputText( "Sides", s_sides, sizeof( s_sides ),
                                    ImGuiInputTextFlags_CharsDecimal |
                                    ImGuiInputTextFlags_EnterReturnsTrue );
        ImGui::Separator();
        ok |= ImGui::Button( "OK", ImVec2( 100.0f, 0.0f ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 100.0f, 0.0f ) ) )
        {
            s_show = false;                      // IDCANCEL: build nothing
            ImGui::CloseCurrentPopup();
        }
        else if ( ok )
        {
            Sides_Commit( (int)atol( s_sides ) );   // IDOK's atol of the field text
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

// imgui_panel_project.cpp — Project Settings, over the mainfrm.cpp pair
// (ProjectSettings_Get / ProjectSettings_Apply).  Replaces the IDD_PROJECT_SETTINGS
// modal DialogBoxParamA that File -> Project settings ran.
#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>
#include <string.h>

extern const char *ProjectSettings_Get( const char *key );                 // mainfrm.cpp:1741  const char *ProjectSettings_Get(const char*)
extern void        ProjectSettings_Apply( const char *basepath, const char *mapspath,
                                          const char *entitypath, const char *game,
                                          const char *basegame );          // mainfrm.cpp:1748  void ProjectSettings_Apply(const char*,const char*,const char*,const char*,const char*)

static bool s_show = false;
static bool s_arm  = false;      // open the popup on the next draw

// The five edits, 1024-byte buffers like the dialog proc's GetDlgItemTextA reads.
static char s_basepath[1024]   = { 0 };
static char s_mapspath[1024]   = { 0 };
static char s_entitypath[1024] = { 0 };
static char s_game[1024]       = { 0 };
static char s_basegame[1024]   = { 0 };

static void SeedField( char *dst, size_t cap, const char *key )
{
    const char *v = ProjectSettings_Get( key );
    strncpy( dst, v ? v : "", cap - 1 );
    dst[cap - 1] = '\0';
}

// WM_INITDIALOG.
static void Seed()
{
    SeedField( s_basepath,   sizeof( s_basepath ),   "basepath" );
    SeedField( s_mapspath,   sizeof( s_mapspath ),   "mapspath" );
    SeedField( s_entitypath, sizeof( s_entitypath ), "entitypath" );
    SeedField( s_game,       sizeof( s_game ),       "game" );
    SeedField( s_basegame,   sizeof( s_basegame ),   "basegame" );
}

void ImGuiPanel_Project_Open()
{
    Seed();
    s_show = true;
    s_arm  = true;
}

void ImGuiPanel_Project_Draw()
{
    if ( !s_show )
        return;

    if ( s_arm )
    {
        ImGui::OpenPopup( "Project Settings" );
        s_arm = false;
    }
    ImGui::SetNextWindowSize( ImVec2( 520.0f, 0.0f ), ImGuiCond_Appearing );
    if ( ImGui::BeginPopupModal( "Project Settings", &s_show,
                                 ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        ImGui::SetNextItemWidth( 380.0f ); ImGui::InputText( "Base path",   s_basepath,   sizeof( s_basepath ) );
        ImGui::SetNextItemWidth( 380.0f ); ImGui::InputText( "Maps path",   s_mapspath,   sizeof( s_mapspath ) );
        ImGui::SetNextItemWidth( 380.0f ); ImGui::InputText( "Entity path", s_entitypath, sizeof( s_entitypath ) );
        ImGui::SetNextItemWidth( 380.0f ); ImGui::InputText( "Game",        s_game,       sizeof( s_game ) );
        ImGui::SetNextItemWidth( 380.0f ); ImGui::InputText( "Base game",   s_basegame,   sizeof( s_basegame ) );

        ImGui::Separator();
        if ( ImGui::Button( "OK", ImVec2( 110.0f, 0.0f ) ) )
        {
            // IDOK: the five SetKeyValue writes then Project_Write.
            ProjectSettings_Apply( s_basepath, s_mapspath, s_entitypath, s_game, s_basegame );
            s_show = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
        {
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

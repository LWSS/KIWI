// imgui_panel_scriptgroup.cpp — the Script Group tool, over scriptgroup.cpp.
// Replaces the modeless MFC dialog on IDD_SCRIPT_GROUP_NAME (217) that AssociateEntities
// created into g_qeglobals.d_hwndMedia.  Menu "Script group" (200) / Shift+Alt+G (33150).
#include "stdafx.h"
#include "qe3.h"
#include "prefs.h"                  // prefData_t / g_PrefsDlg — the fields the DlgProc seeded from
#include <imgui/imgui.h>
#include <string.h>
#include <string>
#include <vector>

// ── scriptgroup.cpp bindings — the same functions the DlgProc's WM_COMMAND cases called ──
extern void ScriptGroupFlags_Gather( const char *key, std::vector<std::string> &rows ); // scriptgroup.cpp:1281  void ScriptGroupFlags_Gather(const char*,std::vector<std::string>&)
extern void ScriptGroupKey_Apply( const char *key );                                    // scriptgroup.cpp:1343  void ScriptGroupKey_Apply(const char*)
extern void ScriptGroupAddSubKey_Apply( const char *subKey, const char *subValue );     // scriptgroup.cpp:1357  void ScriptGroupAddSubKey_Apply(const char*,const char*)
extern void ScriptGroupRemoveSubKey_Apply( const char *subKey, const char *subValue );  // scriptgroup.cpp:1374  void ScriptGroupRemoveSubKey_Apply(const char*,const char*)
extern void ScriptGroupTeam_Apply( const char *teamKey );                               // scriptgroup.cpp:1405  void ScriptGroupTeam_Apply(const char*)
extern void ScriptGroupColorCode_Apply( const char *code );                             // scriptgroup.cpp:1414  void ScriptGroupColorCode_Apply(const char*)
extern void ScriptGroupKeyPreset_Apply( const char *key );                              // scriptgroup.cpp:1544  void ScriptGroupKeyPreset_Apply(const char*)
extern void ScriptGroupSubKeyPreset_Apply( const char *subKey );                        // scriptgroup.cpp:1559  void ScriptGroupSubKeyPreset_Apply(const char*)
extern void ScriptGroupDisassociate_Apply();                                            // scriptgroup.cpp:1391  void ScriptGroupDisassociate_Apply()
extern void ScriptGroupTurretShare_Apply();                                             // scriptgroup.cpp:1431  void ScriptGroupTurretShare_Apply()
extern void ScriptGroupTurretKey_Apply( const char *turretKey );                        // scriptgroup.cpp:1477  void ScriptGroupTurretKey_Apply(const char*)
extern void ScriptGroup_SyncGroupKeyToTeam();                                           // scriptgroup.cpp:1326  void ScriptGroup_SyncGroupKeyToTeam()
extern void ScriptGroup_AddColorToSelection();                                          // scriptgroup.cpp:1216  void ScriptGroup_AddColorToSelection()
extern int  ScriptGroup_AddKeyToSelectedTriggers( const char *key, const char *value ); // scriptgroup.cpp:195  int ScriptGroup_AddKeyToSelectedTriggers(const char*,const char*)
extern int  ScriptGroup_RemoveKeyFromSelectedTriggers( const char *key, const char *value ); // scriptgroup.cpp:228  int ScriptGroup_RemoveKeyFromSelectedTriggers(const char*,const char*)

static bool s_show = false;

// The dialog's five edits.  256-byte buffers, matching the DlgProc's char[256] reads.
static char s_groupKey[256] = { 0 };   // 1441
static char s_subKey[256]   = { 0 };   // 1631
static char s_subValue[256] = { 0 };   // 1635
static char s_flagTrue[256] = { 0 };   // 1636
static char s_flagFalse[256]= { 0 };   // 1639

// WM_INITDIALOG's seeding, and the re-seed after any action that writes those prefs
// behind the fields' back (the binary did the same with SetWindowTextA).
static void SeedFromPrefs()
{
    strncpy( s_groupKey, g_PrefsDlg->ScriptGroupKey.c_str(),     sizeof( s_groupKey ) - 1 );
    strncpy( s_subKey,   g_PrefsDlg->ScriptSubKey_key.c_str(),   sizeof( s_subKey ) - 1 );
    strncpy( s_subValue, g_PrefsDlg->ScriptSubValue_key.c_str(), sizeof( s_subValue ) - 1 );
    s_groupKey[sizeof( s_groupKey ) - 1] = '\0';
    s_subKey[sizeof( s_subKey ) - 1]     = '\0';
    s_subValue[sizeof( s_subValue ) - 1] = '\0';
}

// One flag list (1671 script_flag_true / 1298 script_flag_false).  ScriptGroup_HasFlag
// refilled these on every selection change and on every add/remove; gathering per frame
// is the same content with no push path to keep in sync.
static void DrawFlagList( const char *id, const char *key )
{
    std::vector<std::string> rows;
    ScriptGroupFlags_Gather( key, rows );
    if ( ImGui::BeginChild( id, ImVec2( 0.0f, 80.0f ), ImGuiChildFlags_Borders ) )
    {
        for ( size_t i = 0; i < rows.size(); ++i )
            ImGui::TextUnformatted( rows[i].c_str() );
    }
    ImGui::EndChild();
}

void ImGuiPanel_ScriptGroup_Toggle()
{
    s_show = !s_show;
    if ( s_show )
        SeedFromPrefs();
}

void ImGuiPanel_ScriptGroup_MenuItem()
{
    ImGui::Checkbox( "Script group", &s_show );
}

void ImGuiPanel_ScriptGroup_Draw()
{
    if ( !s_show )
        return;

    ImGui::SetNextWindowSize( ImVec2( 380.0f, 560.0f ), ImGuiCond_FirstUseEver );
    if ( ImGui::Begin( "Script Group", &s_show ) )
    {
        // ── group key (1441) + OK (id 1) ──────────────────────────────────────
        ImGui::SeparatorText( "Group key" );
        ImGui::SetNextItemWidth( 220.0f );
        bool ok = ImGui::InputText( "##groupkey", s_groupKey, sizeof( s_groupKey ),
                                    ImGuiInputTextFlags_EnterReturnsTrue );
        ImGui::SameLine();
        ok |= ImGui::Button( "Assign" );
        if ( ok )
        {
            // The DlgProc's OK: commit the key, assign the next group number, close.  The
            // panel stays open (the sanctioned Phase-3 divergence).
            ScriptGroupKey_Apply( s_groupKey );
            SeedFromPrefs();
        }
        if ( ImGui::Button( "script_health" ) )          // 1442
        {
            ScriptGroupKeyPreset_Apply( "script_health" );
            SeedFromPrefs();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "script_killspawner" ) )     // 0x5A7
        {
            ScriptGroupKeyPreset_Apply( "script_killspawner" );
            SeedFromPrefs();
        }

        // ── sub key / value (1631 / 1635) + Add (4) / Remove (6) ──────────────
        ImGui::SeparatorText( "Sub key" );
        ImGui::SetNextItemWidth( 220.0f );
        ImGui::InputText( "Key##sub", s_subKey, sizeof( s_subKey ) );
        ImGui::SetNextItemWidth( 220.0f );
        ImGui::InputText( "Value##sub", s_subValue, sizeof( s_subValue ) );
        if ( ImGui::Button( "Add##sub" ) )
        {
            ScriptGroupAddSubKey_Apply( s_subKey, s_subValue );
            SeedFromPrefs();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Remove##sub" ) )
        {
            ScriptGroupRemoveSubKey_Apply( s_subKey, s_subValue );
            SeedFromPrefs();
        }
        if ( ImGui::Button( "script_objective_active" ) )    // 0x669
        {
            ScriptGroupSubKeyPreset_Apply( "script_objective_active" );
            SeedFromPrefs();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "script_objective_inactive" ) )  // 0x66A
        {
            ScriptGroupSubKeyPreset_Apply( "script_objective_inactive" );
            SeedFromPrefs();
        }

        // ── script_flag_true (1636 edit, 5 add / 7 remove, 1671 list) ─────────
        ImGui::SeparatorText( "script_flag_true" );
        ImGui::SetNextItemWidth( 220.0f );
        ImGui::InputText( "##flagtrue", s_flagTrue, sizeof( s_flagTrue ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Add##ft" ) )
            ScriptGroup_AddKeyToSelectedTriggers( "script_flag_true", s_flagTrue );
        ImGui::SameLine();
        if ( ImGui::Button( "Remove##ft" ) )
            ScriptGroup_RemoveKeyFromSelectedTriggers( "script_flag_true", s_flagTrue );
        DrawFlagList( "##flagtruelist", "script_flag_true" );

        // ── script_flag_false (1639 edit, 0xE add / 0xF remove, 1298 list) ────
        ImGui::SeparatorText( "script_flag_false" );
        ImGui::SetNextItemWidth( 220.0f );
        ImGui::InputText( "##flagfalse", s_flagFalse, sizeof( s_flagFalse ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Add##ff" ) )
            ScriptGroup_AddKeyToSelectedTriggers( "script_flag_false", s_flagFalse );
        ImGui::SameLine();
        if ( ImGui::Button( "Remove##ff" ) )
            ScriptGroup_RemoveKeyFromSelectedTriggers( "script_flag_false", s_flagFalse );
        DrawFlagList( "##flagfalselist", "script_flag_false" );

        // ── colour radios 1661..1667 (r,b,y,c,g,p,o) ─────────────────────────
        ImGui::SeparatorText( "Colour" );
        static const struct { const char *code; const char *label; } kColors[7] = {
            { "r", "Red" }, { "b", "Blue" }, { "y", "Yellow" }, { "c", "Cyan" },
            { "g", "Green" }, { "p", "Purple" }, { "o", "Orange" } };
        const char *curColor = g_PrefsDlg->ScriptColorKey.c_str();
        for ( int i = 0; i < 7; ++i )
        {
            if ( i )
                ImGui::SameLine();
            if ( ImGui::RadioButton( kColors[i].label, strcmp( curColor, kColors[i].code ) == 0 ) )
            {
                ScriptGroupColorCode_Apply( kColors[i].code );
                SeedFromPrefs();       // it re-syncs the group key to the team key
            }
        }

        // ── team radios 1668 allies / 1670 axis ──────────────────────────────
        const char *curTeam = g_PrefsDlg->ScriptColorTeamKey.c_str();
        if ( ImGui::RadioButton( "Allies", strcmp( curTeam, "script_color_allies" ) == 0 ) )
        {
            ScriptGroupTeam_Apply( "script_color_allies" );
            SeedFromPrefs();
        }
        ImGui::SameLine();
        if ( ImGui::RadioButton( "Axis", strcmp( curTeam, "script_color_axis" ) == 0 ) )
        {
            ScriptGroupTeam_Apply( "script_color_axis" );
            SeedFromPrefs();
        }

        if ( ImGui::Button( "Add colour" ) )      // 9
        {
            ScriptGroup_SyncGroupKeyToTeam();
            ScriptGroup_AddColorToSelection();
            SeedFromPrefs();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Disassociate" ) )    // 0xA
        {
            ScriptGroupDisassociate_Apply();
            SeedFromPrefs();
        }

        // ── turret rows 0x5A9 / 0x5AA / 0x5AB ────────────────────────────────
        ImGui::SeparatorText( "Turrets" );
        if ( ImGui::Button( "Turret share key" ) )        // 0x5A9
        {
            ScriptGroupTurretKey_Apply( "script_turret_share" );
            SeedFromPrefs();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Turret ambush key" ) )       // 0x5AB
        {
            ScriptGroupTurretKey_Apply( "script_turret_ambush" );
            SeedFromPrefs();
        }
        if ( ImGui::Button( "Share turret exports" ) )    // 0x5AA
        {
            ScriptGroupTurretShare_Apply();
            SeedFromPrefs();
        }
    }
    // NOT run through ImGuiShell_CloseOnFocusLoss: every button here acts on the CURRENT
    // selection and both flag lists are read off it, so the operator picks entities in a
    // viewport with this open — the same ruling imgui_panel_advpatch.cpp makes.
    ImGui::End();
}

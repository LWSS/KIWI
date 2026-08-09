// imgui_panel_vehicle.cpp — UI-rework Phase 3: ImGui "Vehicle" panel over the
// vehicledlg.cpp core actions. New KISAK code; visible only under -imgui.
//
// Panel semantics vs CVehicleDlg: the SAME core call each MFC handler makes, with the same
// widget reads and the same key strings / value constants —
//   * the four EDIT rows, label text verbatim from OnCreate (vehicledlg.cpp:287-290):
//       "startinghealth:"  → VehSetKey_Apply( text, "script_startinghealth" )   (OnSetHealth)
//       "accuracy(0-100):" → VehSetAccuracy_Apply( text )                       (OnSetAccuracy)
//       "speed:"           → VehSetKey_Apply( text, "speed" )                   (OnSetSpeed)
//       "lookahead:"       → VehSetKey_Apply( text, "lookahead" )               (OnSetLookahead)
//     Clear on each row is VehClearKey_Apply with that row's key, exactly the key its
//     OnClear* handler passes (vehicledlg.cpp:383-386).  An EMPTY edit is a RemovePair
//     inside VehSetKey_Apply (vehicledlg.cpp:114-121) and inside VehSetAccuracy_Apply
//     (vehicledlg.cpp:126-133), so Set needs no guard of its own — and the atol/100.0
//     "%.2f" accuracy scaling stays where the MFC side has it, in the action.
//   * "crashtype:" — the 3-entry combo in OnCreate's CB_ADDSTRING order (default / plane /
//     forced, vehicledlg.cpp:309-311), which is also VehSetCrashType_Apply's index mapping
//     (0 → "default", 1 → "plane", 2 → "forced").  Set passes the current index, exactly
//     what OnSetCrashType's CB_GETCURSEL hands over; Clear removes "script_crashtype".
//   * the seven on/off pushbutton PAIRS, each button a fixed value/key constant copied from
//     OnToggle's switch (vehicledlg.cpp:401-418) → VehSetToggle_Apply.
//   * the ten script-group buttons, keys copied from OnScriptGroup's switch
//     (vehicledlg.cpp:426-438) → VehScriptGroup_Apply, laid out 2 columns x 5 rows as
//     OnCreate does (vehicledlg.cpp:349-356).
// The panel-flow differences are the sanctioned Phase-3 divergences noted in
// RADIANT_UI_REWORK_PLAN.md: the panel stays open (the actions' EndDialog/SetFocus tail is
// already dropped in the Phase-1 cores — vehicledlg.cpp:83-87), OnClose's ShowWindow(
// SW_HIDE ) and CVehicleDlg::Toggle's create/show/hide dance collapse into the checkbox
// plus the window's own close box, and the ctrl-id plumbing (IDC_VEH_*, ON_CONTROL_RANGE)
// has no analogue — a button binds its constants directly instead of routing an id through
// a switch.
//
// The dialog is WRITE-ONLY: it never reads the selection's current values back, so nothing
// here pre-fills or reflects state (no read-side *_Gather exists, and inventing one would
// be new behaviour).  That is also why the toggles are On/Off BUTTON pairs rather than
// checkboxes: a checkbox would advertise a current state the dialog cannot know.
//
// NO HWND/MFC anywhere in this file: the only binds are the six UI-independent functions
// declared in radiant_ui_actions.h.
#include "stdafx.h"
#include "qe3.h"
#include "radiant_ui_actions.h"
#include <imgui/imgui.h>

static bool s_showVehicle = false;

// The dialog's four edit controls.  1024 bytes for the three plain rows = VEH_Commit's
// stack buffer (vehicledlg.cpp:365) and 256 for accuracy = OnSetAccuracy's
// (vehicledlg.cpp:378) — ImGui reserves the NUL, so the same capacity as the MFC
// ::GetWindowTextA( ..., sizeof( buf ) - 1 ) reads.  All start empty, as OnCreate leaves
// them (it pre-fills nothing).
static char s_health[1024] = { 0 };
static char s_accuracy[256] = { 0 };
static char s_speed[1024] = { 0 };
static char s_lookahead[1024] = { 0 };

// The crash-type combo's cursor.  OnCreate's CB_SETCURSEL seeds it to row 0
// (vehicledlg.cpp:312).
static int s_crashType = 0;

// ── the plain edit rows ───────────────────────────────────────────────────────
// label + edit + [Set] [Clear].  Enter in the edit fires Set, the BS_DEFPUSHBUTTON
// gesture the hand-built popup has no equivalent of.  `key` is the row's key for BOTH
// buttons: OnSet*/OnClear* pass the same string (vehicledlg.cpp:370-372 vs 383-386).
static void Panel_VehEditRow( const char *label, char *buf, size_t bufSz, const char *key )
{
    ImGui::PushID( key );

    ImGui::SetNextItemWidth( 150.0f );                  // = OnCreate's inW
    bool set = ImGui::InputText( label, buf, bufSz, ImGuiInputTextFlags_EnterReturnsTrue );

    ImGui::SameLine();
    set |= ImGui::Button( "Set" );
    if ( set )
        VehSetKey_Apply( buf, key );                    // empty text → RemovePair, inside

    ImGui::SameLine();
    if ( ImGui::Button( "Clear" ) )
        VehClearKey_Apply( key );

    ImGui::PopID();
}

// ── the on/off pushbutton pairs ───────────────────────────────────────────────
// Every value/key constant here is copied from OnToggle's switch (vehicledlg.cpp:401-418);
// the button captions come from OnCreate's toggles[] table (vehicledlg.cpp:319-327), which
// is where "team:" gets Allies/Axis instead of On/Off.
static void Panel_VehToggleRow( const char *label, const char *key,
                                const char *onTxt, const char *onVal,
                                const char *offTxt, const char *offVal )
{
    ImGui::PushID( key );

    if ( ImGui::Button( onTxt, ImVec2( 64.0f, 0.0f ) ) )
        VehSetToggle_Apply( onVal, key );
    ImGui::SameLine();
    if ( ImGui::Button( offTxt, ImVec2( 64.0f, 0.0f ) ) )
        VehSetToggle_Apply( offVal, key );

    ImGui::SameLine();
    ImGui::TextUnformatted( label );

    ImGui::PopID();
}

// ── exports ───────────────────────────────────────────────────────────────────
// The panel toggle, drawn inside the shell window's panel menu (imgui_shell.cpp →
// ImGuiPanels_Menu).
void ImGuiPanel_Vehicle_MenuItem()
{
    ImGui::Checkbox( "Vehicle", &s_showVehicle );
}

// U-CMD-2: the menu/accelerator route.  Radiant_DispatchCommandDirect (mainfrm.cpp) calls
// this for command 33221, where the MFC handler called CVehicleDlg::Toggle() — the same
// show/hide flip, over the flag the checkbox above drives.
void ImGuiPanel_Vehicle_Toggle()
{
    s_showVehicle = !s_showVehicle;
}

void ImGuiPanel_Vehicle_Draw()
{
    if ( !s_showVehicle )
        return;

    // "Vehicle Group" = the popup's caption (vehicledlg.cpp:483).
    if ( ImGui::Begin( "Vehicle Group", &s_showVehicle, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        // Every action below walks selected_brushes and skips world_entity
        // (vehicledlg.cpp:47-65), so with nothing selected it is simply a no-op — the MFC
        // buttons behave the same way and are not disabled either.
        ImGui::SeparatorText( "Keys" );

        Panel_VehEditRow( "startinghealth:", s_health, sizeof( s_health ), "script_startinghealth" );

        // Accuracy is the one row with its own action: 0..100 in the edit, stored as the
        // 0.00..1.00 float VehSetAccuracy_Apply formats (vehicledlg.cpp:135).  Clear still
        // goes through VehClearKey_Apply, the key OnClearAccuracy passes.
        ImGui::PushID( "script_accuracy" );
        ImGui::SetNextItemWidth( 150.0f );
        bool setAccur = ImGui::InputText( "accuracy(0-100):", s_accuracy, sizeof( s_accuracy ),
                                          ImGuiInputTextFlags_EnterReturnsTrue );
        ImGui::SameLine();
        setAccur |= ImGui::Button( "Set" );
        if ( setAccur )
            VehSetAccuracy_Apply( s_accuracy );         // atol/100.0 "%.2f" scaling inside
        ImGui::SameLine();
        if ( ImGui::Button( "Clear" ) )
            VehClearKey_Apply( "script_accuracy" );
        ImGui::PopID();

        Panel_VehEditRow( "speed:",     s_speed,     sizeof( s_speed ),     "speed" );
        Panel_VehEditRow( "lookahead:", s_lookahead, sizeof( s_lookahead ), "lookahead" );

        // crash type.  The item order IS the value mapping — index is what Set hands to
        // VehSetCrashType_Apply, unchanged, like OnSetCrashType's raw CB_GETCURSEL.
        {
            static const char *const kCrashTypes[] = { "default", "plane", "forced" };

            ImGui::PushID( "script_crashtype" );
            ImGui::SetNextItemWidth( 150.0f );
            ImGui::Combo( "crashtype:", &s_crashType, kCrashTypes, IM_ARRAYSIZE( kCrashTypes ) );
            ImGui::SameLine();
            if ( ImGui::Button( "Set" ) )
                VehSetCrashType_Apply( s_crashType );
            ImGui::SameLine();
            if ( ImGui::Button( "Clear" ) )
                VehClearKey_Apply( "script_crashtype" );
            ImGui::PopID();
        }

        // ── the toggle pairs ──────────────────────────────────────────────────
        ImGui::SeparatorText( "Toggles" );

        Panel_VehToggleRow( "deathroll:",     "script_deathroll",     "On", "1", "Off", "0" );
        Panel_VehToggleRow( "turret:",        "script_turret",        "On", "1", "Off", "0" );
        Panel_VehToggleRow( "turretmg:",      "script_turretmg",      "On", "1", "Off", "0" );
        Panel_VehToggleRow( "badplace:",      "script_badplace",      "On", "1", "Off", "0" );
        Panel_VehToggleRow( "avoidvehicles:", "script_avoidvehicles", "On", "1", "Off", "0" );
        Panel_VehToggleRow( "attackai:",      "script_attackai",      "On", "1", "Off", "0" );
        Panel_VehToggleRow( "team:",          "script_team",          "Allies", "allies", "Axis", "axis" );

        // ── the script-group buttons ──────────────────────────────────────────
        // Clicking one stores its key NAME then assigns the next free group number to the
        // selection (VehScriptGroup_Apply → VehicleDlg_SetScriptGroupKey, vehicledlg.cpp:169).
        // Captions are OnCreate's short sg[] labels; the KEYS are OnScriptGroup's, verbatim.
        ImGui::SeparatorText( "Script groups (assign next number to selection)" );
        {
            static const struct { const char *lbl; const char *key; } kScriptGroups[] = {
                { "spawngroup",  "script_vehiclespawngroup"     }, { "startmove",   "script_vehiclestartmove"      },
                { "groupdelete", "script_vehiclegroupdelete"    }, { "ride",        "script_vehicleride"           },
                { "walk",        "script_vehiclewalk"           }, { "attackgroup", "script_vehicleattackgroup"    },
                { "focusfire",   "script_vehiclefocusfiregroup" }, { "detour",      "script_vehicledetour"          },
                { "gatetrigger", "script_gatetrigger"           }, { "attackorgs",  "script_attackorgs"            },
            };

            const ImVec2 sgSize( 170.0f, 0.0f );        // = OnCreate's sgW
            for ( int n = 0; n < IM_ARRAYSIZE( kScriptGroups ); ++n )
            {
                if ( n & 1 )                            // 2 columns x 5 rows, as OnCreate lays out
                    ImGui::SameLine();
                if ( ImGui::Button( kScriptGroups[n].lbl, sgSize ) )
                    VehScriptGroup_Apply( kScriptGroups[n].key );
            }
        }

        ImGui::TextDisabled( "Write-only, like the dialog: an empty field's Set removes the key" );
    }
    ImGuiShell_CloseOnFocusLoss( &s_showVehicle );
    ImGui::End();
}

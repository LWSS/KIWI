// imgui_panel_commands.cpp — UI-rework Phase 3 unit P-8: ImGui "Command list" over the
// editor hotkey command map. New KISAK code; visible only under -imgui.
//
// Panel semantics vs CCommandsDlg (mainfrm.cpp:5306, IDD_DLG_COMMANDLIST 132, reached from
// Help→Command list... 32790): the SAME lines the listbox gets, built by the SAME two
// helpers, in the SAME order —
//   * one row per g_radiantCommands entry, in TABLE ORDER. The dialog template's listbox
//     1036 has no LBS_SORT (radiant.rc:893), so AddString order == table order, duplicates
//     included ("Patch TAB" is in the table twice, mainfrm.cpp:1802-1803 — faithful, the
//     binary's std::map keeps the first but the listbox lists both).
//   * each line is OnInitDialog's `"%s \t%s%s"` (mainfrm.cpp:5337) = name, TAB, then
//     CommandList_Mods' modifier prefix followed by CommandList_KeyName's key name. Here
//     the TAB becomes the table's column break, so the name column holds the `%s` and the
//     binding column holds `mods + key`; the single space the format puts before the TAB is
//     dropped with it (it exists only to keep the listbox glyphs off the tab stop).
//   * built ONCE on first open, like OnInitDialog — the binding table is only rewritten by
//     LoadCommandMap at startup (mainfrm.cpp:1990), so there is nothing to re-poll.
//
// Sanctioned Phase-3 divergences (RADIANT_UI_REWORK_PLAN.md):
//   * the panel stays open; IDOK "Close" collapses into the window's own close box.
//   * the LB_SETTABSTOPS 96 (mainfrm.cpp:5322) becomes a fixed first-column width. Those are
//     dialog-template quarter-average-character units, so 96 == 24 average characters; spent
//     here as 24 character widths rather than as 96 pixels.
//   * THE c:/commandlist.txt WRITE IS INTENTIONALLY DROPPED. OnInitDialog also mirrors every
//     line into "c:/commandlist.txt" (CFile modeCreate|modeWrite, mainfrm.cpp:5324-5348) — a
//     silent side effect of merely OPENING the dialog, and a hardcoded write to the root of
//     C:. It is faithful in the MFC path and STAYS THERE; a panel that repaints has no
//     business touching the disk. Anyone who wants the dump still has Help→Command list...
//   * the name filter below is a PANEL CONVENIENCE with no dialog counterpart (187 rows in a
//     209-unit listbox is a lot of scrolling). It only hides rows; it never reorders them.
//
// NO HWND/MFC anywhere in this file: the two bound helpers were made non-static during the
// rework precisely so this panel could reach them without the CDialog.
#include "stdafx.h"
#include "qe3.h"
#include <universal/q_shared.h>      // I_stristr — case-insensitive filter match
#include <imgui/imgui.h>
#include <string>
#include <vector>
#include "radiant_ui_actions.h"

// MUST MATCH mainfrm.cpp:1764 verbatim (shared-header consolidation pending). Layout-identical
// is not optional: this is the parameter type of the two extern helpers below, so both the
// mangled names and the field offsets have to agree with the definition in mainfrm.cpp.
//   `byte` is universal/q_shared.h's typedef (stdafx.h:78), the same one mainfrm.cpp sees.
struct RadiantCommand { const char *name; byte vk; byte mods; int commandId; };

// ── mainfrm.cpp bindings ──────────────────────────────────────────────────────
// The two UI-independent halves of a command-list line, split out of sub_40BBC0. Both
// non-static (mainfrm.cpp:5274 / 5284) and both pure formatters — no listbox, no CFile.
//   KeyName: the g_radiantKeys entry for c.vk, else the raw character written into keybuf.
//   Mods:    "" or "Shift[ + Alt][ + Control][ + Left Win] + " — note the TRAILING " + ",
//            which is why the binding cell is a plain mods-then-key concatenation.
extern const char *CommandList_KeyName( const RadiantCommand &c, char keybuf[8] );  // mainfrm.cpp:5274
extern void        CommandList_Mods( const RadiantCommand &c, char mods[64] );      // mainfrm.cpp:5284

// ── BLOCKED BINDING — ORCHESTRATOR TO-DO (plan P-8) ───────────────────────────
// The rows themselves cannot be reached yet. All three symbols OnInitDialog uses to walk the
// table are TU-static in mainfrm.cpp and therefore have no external linkage:
//     static RadiantCommand g_radiantCommands[...]   mainfrm.cpp:1957
//     static void Radiant_SeedCommandTable()         mainfrm.cpp:1959
//     static const RadiantKeyName g_radiantKeys[]    mainfrm.cpp:1971  (reached via KeyName)
// The two options that would avoid an edit outside this file were both rejected:
//   * copying g_radiantCommandsDefault (mainfrm.cpp:1765-1953, 187 verbatim-from-the-exe
//     entries) is not just the usual duplicate-table drift risk — it would be WRONG. The
//     dialog lists g_radiantCommands, the MUTABLE copy that LoadCommandMap patches in place
//     from radiant.ini [Commands] (mainfrm.cpp:1990). A panel reading the defaults would
//     show stale bindings to every user who has ever rebound a key.
//   * going through CMainFrame is worse: ShowMenuItemKeyBindings (mainfrm.cpp:2069) is the
//     only non-static reader of the table and it emits into an HMENU, not to a caller.
// So this needs ONE accessor in mainfrm.cpp, next to Radiant_SeedCommandTable:
//     int Radiant_GetCommandTable( const RadiantCommand **out )
//     {
//         Radiant_SeedCommandTable();          // MANDATORY — see below
//         *out = g_radiantCommands;
//         return (int)ARRAYSIZE( g_radiantCommands );
//     }
// plus its declaration (mainfrm.h or radiant_ui_actions.h, wherever RadiantCommand lands when
// the type moves to a shared header) — then this stub becomes a one-line forward and the row
// builder below runs unchanged.
// THE SEED CALL INSIDE THE ACCESSOR IS LOAD-BEARING, not defensive: g_radiantCommands is a
// zero-initialised BSS array until Radiant_SeedCommandTable memcpy's the defaults into it, so
// an unseeded walk reads 187 null `name` pointers. That seed is exactly what CCommandsDlg::
// OnInitDialog does first (mainfrm.cpp:5328) and what the panel's first-open must inherit;
// keeping it inside the accessor is what makes this panel independent of whether CMainFrame
// has run LoadCommandMap yet (it will not have, once Phase 4 drops CMainFrame).
extern int Radiant_GetCommandTable( const RadiantCommand **out );   // mainfrm.cpp (seeds inside)
static int CommandList_GetTable( const RadiantCommand **out )
{
    return Radiant_GetCommandTable( out );
}

// ── panel state ───────────────────────────────────────────────────────────────
static bool s_showCommands = false;
static bool s_opened       = false;      // first-open latch (OnInitDialog equivalent)

// One built row = the two halves of one listbox line. Prebuilt rather than formatted per
// frame because the source table does not change after startup, and because the filter has to
// match against the same text the row shows.
struct CommandRow { std::string name, binding; };
static std::vector<CommandRow> s_rows;

// Filter text (panel-only). 64 chars is well past the longest command name in the table.
static char s_filter[64] = { 0 };

// The OnInitDialog body (mainfrm.cpp:5328-5349) minus the listbox and minus the CFile.
static void CommandList_Build()
{
    s_rows.clear();

    const RadiantCommand *table = nullptr;
    const int count = CommandList_GetTable( &table );

    for ( int i = 0; i < count; ++i )
    {
        const RadiantCommand &c = table[i];

        // Same call order and same buffer sizes as OnInitDialog: keybuf is only written when
        // the VK has no g_radiantKeys name, so it must stay alive as long as keyName does.
        char        keybuf[8];
        const char *keyName = CommandList_KeyName( c, keybuf );
        char        mods[64];
        CommandList_Mods( c, mods );

        // The `"%s \t%s%s"` line, split at the TAB. Kept in the 320-byte-per-line budget of
        // OnInitDialog's `line[320]` so a name long enough to be truncated there is truncated
        // here too rather than silently displaying wider than the dialog would.
        char cell[320];
        _snprintf( cell, sizeof( cell ) - 1, "%s", c.name );
        cell[sizeof( cell ) - 1] = '\0';
        CommandRow row;
        row.name = cell;

        _snprintf( cell, sizeof( cell ) - 1, "%s%s", mods, keyName );
        cell[sizeof( cell ) - 1] = '\0';
        row.binding = cell;

        s_rows.push_back( row );
    }
}

// Panel toggle, drawn inside the shell window (imgui_shell.cpp).
void ImGuiPanel_Commands_MenuItem()
{
    ImGui::Checkbox( "Command list", &s_showCommands );
}

// U-CMD-2: the menu/accelerator route.  Radiant_DispatchCommandDirect (mainfrm.cpp) calls
// this for command 32790, where the MFC handler called CCommandsDlg::DoModal() — the same
// show/hide flip, over the flag the checkbox above drives.
void ImGuiPanel_Commands_Toggle()
{
    s_showCommands = !s_showCommands;
}

void ImGuiPanel_Commands_Draw()
{
    if ( !s_showCommands )
    {
        s_opened = false;      // a re-open rebuilds, like every CCommandsDlg::DoModal
        return;
    }

    if ( !s_opened )
    {
        s_opened = true;
        CommandList_Build();   // = OnInitDialog's fill (the seed rides inside the accessor)
    }

    // Plain Begin (no AlwaysAutoResize): the row table is a scrolling region of the available
    // size, and auto-resize would fight it every frame.
    if ( ImGui::Begin( "Mapped Commands", &s_showCommands ) )     // the dialog's CAPTION
    {
        // Panel-only. Filters by NAME, the column the dialog's tab stop separates on.
        ImGui::SetNextItemWidth( 220.0f );
        ImGui::InputTextWithHint( "##cmdfilter", "filter by command name", s_filter,
                                  sizeof( s_filter ) );
        ImGui::SameLine();
        if ( ImGui::Button( "Clear" ) )
            s_filter[0] = '\0';

        const bool filtering = ( s_filter[0] != '\0' );
        int        shown     = 0;

        // ScrollY with a 0 height would eat the whole remaining region and push the footer
        // readout off the bottom, so reserve its line(s) explicitly.
        const int   footerLines = s_rows.empty() ? 2 : 1;
        const ImVec2 tableSize( 0.0f,
                                -ImGui::GetTextLineHeightWithSpacing() * (float)footerLines );

        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                    | ImGuiTableFlags_ScrollY;
        if ( ImGui::BeginTable( "##commandlist", 2, flags, tableSize ) )
        {
            // 96 LB_SETTABSTOPS units = 24 average characters (mainfrm.cpp:5322).
            ImGui::TableSetupColumn( "Command", ImGuiTableColumnFlags_WidthFixed,
                                     ImGui::CalcTextSize( "0" ).x * 24.0f );
            ImGui::TableSetupColumn( "Binding", ImGuiTableColumnFlags_WidthStretch );
            ImGui::TableSetupScrollFreeze( 0, 1 );
            ImGui::TableHeadersRow();

            for ( size_t i = 0; i < s_rows.size(); ++i )
            {
                if ( filtering && !I_stristr( s_rows[i].name.c_str(), s_filter ) )
                    continue;
                ++shown;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex( 0 );
                ImGui::TextUnformatted( s_rows[i].name.c_str() );
                ImGui::TableSetColumnIndex( 1 );
                // Unbound commands (table vk 0x00, e.g. "SaveLayeredMaterials"
                // mainfrm.cpp:1952) legitimately produce an empty binding cell — see the
                // KeyName note in the report; the dialog shows an empty column there too.
                ImGui::TextUnformatted( s_rows[i].binding.c_str() );
            }
            ImGui::EndTable();
        }

        // Row-count readout: not a dialog control, just the scrollbar's information made
        // explicit — and, while the table binding is blocked, the one visible symptom.
        if ( s_rows.empty() )
        {
            ImGui::TextDisabled( "No commands: the binding table is not reachable from this"
                                 " translation unit yet." );
            ImGui::TextDisabled( "See the BLOCKED BINDING note at the top of"
                                 " imgui_panel_commands.cpp (plan P-8)." );
        }
        else if ( filtering )
        {
            ImGui::TextDisabled( "%d of %d command%s", shown, (int)s_rows.size(),
                                 s_rows.size() == 1 ? "" : "s" );
        }
        else
        {
            ImGui::TextDisabled( "%d command%s (table order, as the listbox lists them)",
                                 (int)s_rows.size(), s_rows.size() == 1 ? "" : "s" );
        }
    }
    ImGuiShell_CloseOnFocusLoss( &s_showCommands );
    ImGui::End();
}

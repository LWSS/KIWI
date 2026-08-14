#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_palette.cpp — RADIANT_UX_DESIGN §15 implementation.  See kiwi_palette.h
// for the "no second registry" rule and the key-capture contract.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_palette.h"
#include "kiwi_command.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// MUST MATCH mainfrm.cpp's definition verbatim — same reasoning as
// imgui_panel_commands.cpp:46 (this is the parameter type of the two extern
// formatters below, so field offsets and mangled names both have to agree).
//   `byte` is universal/q_shared.h's typedef (stdafx.h), the same one mainfrm sees.
struct RadiantCommand { const char *name; byte vk; byte mods; int commandId; };

// ── mainfrm.cpp bindings (verified against their definitions) ───────────────
extern int         Radiant_GetCommandTable( const RadiantCommand **out );
extern const char *CommandList_KeyName( const RadiantCommand &c, char keybuf[8] );
extern void        CommandList_Mods( const RadiantCommand &c, char mods[64] );
extern void        Radiant_ExecCommand( unsigned int cmdId );

namespace
{
    bool  s_open        = false;
    bool  s_focusNext   = false;
    bool  s_scrollToSel = false;
    char  s_filter[128] = { 0 };
    int   s_selected    = 0;

    struct row_t
    {
        int         commandId;
        const char *display;
        const char *category;
        char        shortcut[80];
        bool        enabled;
    };

    std::vector<row_t> s_rows;

    // Subsequence fuzzy match, case-insensitive: every character of `pat` must
    // appear in `str` in order.  Deliberately the simplest thing that works — the
    // rows are ~200, and a ranking model is not what makes a palette feel good at
    // this size.
    bool FuzzyMatch( const char *str, const char *pat )
    {
        if ( !pat || !*pat )
            return true;
        const char *s = str;
        for ( const char *p = pat; *p; ++p )
        {
            if ( *p == ' ' )
                continue;                    // spaces are separators, not literals
            const int pc = tolower( (unsigned char)*p );
            for ( ;; )
            {
                if ( !*s )
                    return false;
                const int sc = tolower( (unsigned char)*s );
                ++s;
                if ( sc == pc )
                    break;
            }
        }
        return true;
    }

    // Rebuild the row list from the LIVE table + the §3 metadata.  Cheap enough to
    // do every frame the palette is open (≈200 rows, no allocation churn after the
    // first frame) — and doing it every frame is what makes a keymap-profile switch
    // or a canExecute() change show up instantly.
    void Rebuild()
    {
        s_rows.clear();

        const RadiantCommand *table = nullptr;
        const int count = Radiant_GetCommandTable( &table );
        if ( !table )
            return;
        s_rows.reserve( (size_t)count );

        char haystack[256];
        for ( int i = 0; i < count; ++i )
        {
            const RadiantCommand &c = table[i];
            if ( !c.name || !c.commandId )
                continue;

            // The table legitimately holds one id twice ("Patch TAB" 33089, two bindings —
            // faithful to the binary).  The command-list panel shows both because it mirrors
            // a listbox; a PALETTE lists commands, so the first binding wins.
            bool dupe = false;
            for ( int j = 0; j < i; ++j )
                if ( table[j].commandId == c.commandId ) { dupe = true; break; }
            if ( dupe )
                continue;

            const kiwiCommandInfo_t *info = KiwiCmd_Info( c.commandId );
            row_t r;
            r.commandId = c.commandId;
            r.display   = info ? info->displayName : c.name;
            r.category  = info ? info->category    : "Classic";
            r.enabled   = KiwiCmd_CanExecute( c.commandId );

            // Shortcut text from the LIVE binding, through mainfrm's own formatters
            // (CommandList_Mods leaves a trailing " + ", hence the plain concat).
            r.shortcut[0] = '\0';
            if ( c.vk )
            {
                char mods[64];
                char keybuf[8];
                CommandList_Mods( c, mods );
                const char *key = CommandList_KeyName( c, keybuf );
                _snprintf( r.shortcut, sizeof( r.shortcut ), "%s%s", mods, key ? key : "" );
                r.shortcut[sizeof( r.shortcut ) - 1] = '\0';
            }

            _snprintf( haystack, sizeof( haystack ), "%s %s %s",
                       r.display, r.category, c.name );
            haystack[sizeof( haystack ) - 1] = '\0';
            if ( !FuzzyMatch( haystack, s_filter ) )
                continue;

            s_rows.push_back( r );
        }

        if ( s_selected >= (int)s_rows.size() )
            s_selected = (int)s_rows.size() - 1;
        if ( s_selected < 0 )
            s_selected = 0;
    }

    void Run( int commandId )
    {
        KiwiPalette_Close();
        // §3: ONE dispatcher — but DEFERRED.  Run() executes during the ImGui frame
        // (ImGuiPanels_Draw), inside the compositing scene bracket; a command that
        // opens a modal Win32 dialog (DoColor, file dialogs, MessageBox) would nest
        // its message pump in the bracket — the exact crash class the shell's
        // context-menu dispatch was moved post-present to avoid.  PostMessage routes
        // the id through the frame's WM_COMMAND case (radiant_main.cpp: lParam==0,
        // LOWORD(wParam)=id → Radiant_ExecCommand), i.e. the pump-side path every
        // MENU command already takes — KIWI ids included (WM_COMMAND →
        // DispatchCommandDirect → KiwiCmd_Dispatch).  Ids fit LOWORD: max is 60767.
        ::PostMessageA( g_qeglobals.d_hwndMain, WM_COMMAND,
                        (WPARAM)(unsigned int)commandId, 0 );
    }
}

// ─── open / close ────────────────────────────────────────────────────────────
void KiwiPalette_Open()
{
    s_open      = true;
    s_focusNext = true;
    s_selected  = 0;
    s_filter[0] = '\0';
}

void KiwiPalette_Close()
{
    s_open = false;
}

void KiwiPalette_Toggle()
{
    if ( s_open )
        KiwiPalette_Close();
    else
        KiwiPalette_Open();
}

bool KiwiPalette_IsOpen()
{
    return s_open;
}

// ─── menu hook ───────────────────────────────────────────────────────────────
// Drawn inside the shell's panel block (a plain window, not a menu bar — hence a
// button, matching the Checkbox-style entries the other panels use there).
void KiwiPalette_MenuItem()
{
    if ( ImGui::Button( "Open command palette" ) )
        KiwiPalette_Open();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Every command, searchable (modern keymap: F).\n"
                           "This is how the classic keymap reaches new features." );
}

// ─── draw ────────────────────────────────────────────────────────────────────
void KiwiPalette_Draw()
{
    if ( !s_open )
        return;

    // A changed filter re-ranks everything under the cursor, so put the highlight
    // back on the first row rather than leaving it on an unrelated index.
    static char s_lastFilter[sizeof( s_filter )] = { 0 };
    if ( strcmp( s_lastFilter, s_filter ) != 0 )
    {
        strncpy( s_lastFilter, s_filter, sizeof( s_lastFilter ) - 1 );
        s_lastFilter[sizeof( s_lastFilter ) - 1] = '\0';
        s_selected    = 0;
        s_scrollToSel = true;
    }

    Rebuild();

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const ImVec2 size( 620.0f, 420.0f );
    ImGui::SetNextWindowPos( ImVec2( vp->WorkPos.x + ( vp->WorkSize.x - size.x ) * 0.5f,
                                     vp->WorkPos.y + vp->WorkSize.y * 0.16f ),
                             ImGuiCond_Always );
    ImGui::SetNextWindowSize( size, ImGuiCond_Always );
    if ( s_focusNext )
        ImGui::SetNextWindowFocus();   // on OPEN only — forcing it every frame would
                                       // make every other window unclickable

    if ( !ImGui::Begin( "Command palette", &s_open,
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize ) )
    {
        ImGui::End();
        return;
    }

    // ── the filter field ────────────────────────────────────────────────────
    // Auto-focused, so io.WantTextInput is true and the pump's WantsKeyboard gate
    // keeps typed characters out of the hotkey table (see the header).
    if ( s_focusNext )
    {
        ImGui::SetKeyboardFocusHere();
        s_focusNext = false;
    }
    ImGui::SetNextItemWidth( -1.0f );
    const bool entered = ImGui::InputTextWithHint( "##kiwipalettefilter", "Search commands...",
                                                   s_filter, sizeof( s_filter ),
                                                   ImGuiInputTextFlags_EnterReturnsTrue );

    // ── keyboard navigation, read from ImGui (never from Win32) ─────────────
    // InputText does not consume the arrows on a single-line field, so these are
    // safe to read while it holds focus.
    const int rowCount = (int)s_rows.size();
    if ( rowCount > 0 )
    {
        const int before = s_selected;
        if ( ImGui::IsKeyPressed( ImGuiKey_DownArrow, true ) )
            s_selected = ( s_selected + 1 ) % rowCount;
        if ( ImGui::IsKeyPressed( ImGuiKey_UpArrow, true ) )
            s_selected = ( s_selected + rowCount - 1 ) % rowCount;
        if ( ImGui::IsKeyPressed( ImGuiKey_PageDown, true ) )
            s_selected = ( s_selected + 10 < rowCount ) ? s_selected + 10 : rowCount - 1;
        if ( ImGui::IsKeyPressed( ImGuiKey_PageUp, true ) )
            s_selected = ( s_selected - 10 > 0 ) ? s_selected - 10 : 0;
        if ( s_selected != before )
            s_scrollToSel = true;            // keyboard moved it — follow it (see below)
    }
    if ( ImGui::IsKeyPressed( ImGuiKey_Escape, false ) )
    {
        ImGui::End();
        KiwiPalette_Close();
        return;
    }

    int runId = 0;
    if ( entered && rowCount > 0 && s_selected >= 0 && s_selected < rowCount
      && s_rows[s_selected].enabled )
        runId = s_rows[s_selected].commandId;

    ImGui::Separator();

    // ── the rows ────────────────────────────────────────────────────────────
    if ( ImGui::BeginChild( "##kiwipaletterows", ImVec2( 0.0f, 0.0f ), 0,
                            ImGuiWindowFlags_NoSavedSettings ) )
    {
        for ( int i = 0; i < rowCount; ++i )
        {
            const row_t &r = s_rows[i];
            ImGui::PushID( i );

            // Captured BEFORE the full-width Selectable, because SameLine takes an
            // ABSOLUTE offset from the window's content edge, not a relative one.
            const float rowW = ImGui::GetContentRegionAvail().x;

            if ( !r.enabled )
                ImGui::PushStyleColor( ImGuiCol_Text, ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );

            char label[224];
            _snprintf( label, sizeof( label ), "%s   [%s]", r.display, r.category );
            label[sizeof( label ) - 1] = '\0';

            const bool sel = ( i == s_selected );
            if ( ImGui::Selectable( label, sel, ImGuiSelectableFlags_AllowDoubleClick ) )
            {
                s_selected = i;
                if ( r.enabled )
                    runId = r.commandId;
            }
            if ( !r.enabled && ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Unavailable with the current selection." );

            // Shortcut, RIGHT-ALIGNED on the same line (§15).
            if ( r.shortcut[0] )
            {
                float x = rowW - ImGui::CalcTextSize( r.shortcut ).x - 8.0f;
                if ( x < 0.0f )
                    x = 0.0f;
                ImGui::SameLine( x );
                ImGui::TextDisabled( "%s", r.shortcut );
            }

            if ( !r.enabled )
                ImGui::PopStyleColor();

            // Follow the keyboard cursor, but never fight the mouse wheel: only the
            // frame an arrow/page key actually moved the selection.
            if ( sel && s_scrollToSel )
                ImGui::SetScrollHereY( 0.5f );

            ImGui::PopID();
        }
        s_scrollToSel = false;
    }
    ImGui::EndChild();

    ImGui::End();

    if ( runId )
        Run( runId );
}

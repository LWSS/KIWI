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
#include "radiant_frame.h"          // KIWI-UX (CLEANUP, C-6): struct RadiantCommand + the table API
#include "kiwi_cmdui.h"             // KIWI-UX (CLEANUP, C-72): the palette/add-menu shared bits
#include "kiwi_command.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <vector>

// KIWI-UX (CLEANUP, C-6): the struct AND these declarations now live in
// radiant_frame.h, included above.  This file used to carry a verbatim copy of
// `struct RadiantCommand` plus its own externs, as six other TUs did.

// ── mainfrm.cpp bindings (verified against their definitions) ───────────────
extern void        Radiant_ExecCommand( unsigned int cmdId );

namespace
{
    // ── KIWI-UX (CLEANUP, C-73): window layout, named file-locally ──────────────
    // Same convention kiwi_cmdoptions.cpp's KOPT_WIDTH / KOPT_INSET / KOPT_TOPFRAC
    // follows.  The palette is a fixed-size centred sheet (NoResize), so these ARE
    // the window, not a starting size.
    const float KPAL_WIDTH   = 620.0f;
    const float KPAL_HEIGHT  = 420.0f;
    const float KPAL_TOPFRAC = 0.16f;   // down from the viewport's work top
    const int   KPAL_PAGE    = 10;      // rows PageUp/PageDown moves

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

    // KIWI-UX (CLEANUP, C-72): the fuzzy match, the shortcut formatter and the
    // right-aligned hint moved to kiwi_cmdui.h — the add menu carried a
    // character-identical copy of all three, and the two lists have to accept the
    // same queries and render the same rows or the difference is a bug nobody can
    // report.  This body IS the one that moved.

    // Rebuild the row list from the LIVE table + the §3 metadata.  Runs once per
    // frame the palette is VISIBLE (KIWI-UX CLEANUP, C-62 moved the call below
    // ImGui::Begin's early-out — it used to run even on a frame the window was
    // clipped away).  It stays per-frame rather than being cached on the filter
    // string, because `enabled` (KiwiCmd_CanExecute) and the shortcut text are
    // LIVE: caching the rows would freeze a keymap-profile switch or a selection
    // change out of the list, which is the behaviour this file was written for.
    void Rebuild()
    {
        s_rows.clear();

        const RadiantCommand *table = nullptr;
        const int count = Radiant_GetCommandTable( &table );
        if ( !table )
            return;
        s_rows.reserve( (size_t)count );

        // KIWI-UX (CLEANUP, C-62): the duplicate-id test was `for ( j = 0; j < i; )`
        // — O(n²) over ~200 rows, ~20 000 compares every frame.  Same answer, one
        // pass: a bit per command id, set for EVERY row as it is passed (including
        // the ones skipped below, which is what the old inner loop compared
        // against), so "have I already emitted this id" is one test.  Ids fit
        // LOWORD (the Run() note below), and anything outside that range falls
        // back to the linear scan rather than indexing out of the map.
        const int KPAL_ID_CAP = 65536;
        static unsigned char seen[KPAL_ID_CAP / 8];
        memset( seen, 0, sizeof( seen ) );

        char haystack[256];
        for ( int i = 0; i < count; ++i )
        {
            const RadiantCommand &c = table[i];
            const int  id      = c.commandId;
            const bool inRange = ( id > 0 && id < KPAL_ID_CAP );
            bool dupe = false;
            if ( inRange )
            {
                dupe = ( seen[id >> 3] & ( 1u << ( id & 7 ) ) ) != 0;
                seen[id >> 3] |= (unsigned char)( 1u << ( id & 7 ) );
            }
            else if ( id != 0 )
            {
                for ( int j = 0; j < i; ++j )
                    if ( table[j].commandId == id ) { dupe = true; break; }
            }

            if ( !c.name || !id )
                continue;

            // The table legitimately holds one id twice ("Patch TAB" 33089, two bindings —
            // faithful to the binary).  The command-list panel shows both because it mirrors
            // a listbox; a PALETTE lists commands, so the first binding wins.
            if ( dupe )
                continue;

            const kiwiCommandInfo_t *info = KiwiCmd_Info( c.commandId );
            row_t r;
            r.commandId = c.commandId;
            r.display   = info ? info->displayName : c.name;
            r.category  = info ? info->category    : "Classic";
            r.enabled   = KiwiCmd_CanExecute( c.commandId );

            // Shortcut text from the LIVE binding (KIWI-UX CLEANUP, C-72).
            KiwiCmdUI_ShortcutText( c, r.shortcut, (int)sizeof( r.shortcut ) );

            _snprintf( haystack, sizeof( haystack ), "%s %s %s",
                       r.display, r.category, c.name );
            haystack[sizeof( haystack ) - 1] = '\0';
            if ( !KiwiCmdUI_Fuzzy( haystack, s_filter ) )
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

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const ImVec2 size( KPAL_WIDTH, KPAL_HEIGHT );
    ImGui::SetNextWindowPos( ImVec2( vp->WorkPos.x + ( vp->WorkSize.x - size.x ) * 0.5f,
                                     vp->WorkPos.y + vp->WorkSize.y * KPAL_TOPFRAC ),
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

    // KIWI-UX (CLEANUP, C-62): BELOW the early-out.  This used to run above
    // SetNextWindowPos, so a frame on which Begin returned false still paid for a
    // full rebuild of ~200 rows.  Nothing between the old call site and here has a
    // side effect the rebuild depends on: SetNextWindowPos / SetNextWindowSize /
    // SetNextWindowFocus are pure ImGui state, and the ONE latch — the filter-change
    // detector above, which zeroes s_selected and arms s_scrollToSel — deliberately
    // stays where it was, ahead of the early-out.  It cannot mis-fire there: s_filter
    // is only written by the InputText BELOW, inside this same Begin body, so it
    // cannot change on a frame the window is not drawn.  The rebuild still reads
    // LAST frame's filter, exactly as it did from the old position.
    Rebuild();

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
            s_selected = ( s_selected + KPAL_PAGE < rowCount ) ? s_selected + KPAL_PAGE : rowCount - 1;
        if ( ImGui::IsKeyPressed( ImGuiKey_PageUp, true ) )
            s_selected = ( s_selected - KPAL_PAGE > 0 ) ? s_selected - KPAL_PAGE : 0;
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

            // Shortcut, RIGHT-ALIGNED on the same line (§15; CLEANUP, C-72).
            KiwiCmdUI_RightAlignedHint( r.shortcut, rowW );

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

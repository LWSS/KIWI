#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Command palette implementation; see kiwi_palette.h for registry and key-capture invariants.

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_palette.h"
#include "radiant_frame.h"          // RadiantCommand and command-table API
#include "kiwi_cmdui.h"             // shared palette/add-menu UI helpers
#include "kiwi_command.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <vector>

extern void        Radiant_ExecCommand( unsigned int cmdId );

namespace
{
    // Final dimensions for the fixed-size, centered sheet.
    const float KPAL_WIDTH   = 620.0f;
    const float KPAL_HEIGHT  = 420.0f;
    const float KPAL_TOPFRAC = 0.16f;   // fraction down from the viewport work top
    const int   KPAL_PAGE    = 10;      // PageUp/PageDown step in rows

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

    // Rebuild every visible frame: enabled state and live bindings change independently of the filter.
    void Rebuild()
    {
        s_rows.clear();

        const RadiantCommand *table = nullptr;
        const int count = Radiant_GetCommandTable( &table );
        if ( !table )
            return;
        s_rows.reserve( (size_t)count );

        // Mark every encountered ID before filtering so duplicate detection follows table order.
        // Valid IDs fit LOWORD; out-of-range IDs use a linear scan instead of indexing the bitmap.
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

            // The table intentionally has two bindings for "Patch TAB" (33089); a palette lists
            // commands rather than bindings, so the first table entry wins.
            if ( dupe )
                continue;

            const kiwiCommandInfo_t *info = KiwiCmd_Info( c.commandId );
            row_t r;
            r.commandId = c.commandId;
            r.display   = info ? info->displayName : c.name;
            r.category  = info ? info->category    : "Classic";
            r.enabled   = KiwiCmd_CanExecute( c.commandId );

            // Use live binding text so profile and radiant.ini changes appear immediately.
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
        // Defer through WM_COMMAND because commands may open modal pumps while ImGui is compositing.
        // The frame dispatches the post-present message through the same path as menu commands;
        // command IDs fit LOWORD (maximum 60767).
        ::PostMessageA( g_qeglobals.d_hwndMain, WM_COMMAND,
                        (WPARAM)(unsigned int)commandId, 0 );
    }
}

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

// Use a button because the shell's panel block is a plain window, not a menu bar.
void KiwiPalette_MenuItem()
{
    if ( ImGui::Button( "Open command palette" ) )
        KiwiPalette_Open();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Every command, searchable (modern keymap: F).\n"
                           "This is how the classic keymap reaches new features." );
}

void KiwiPalette_Draw()
{
    if ( !s_open )
        return;

    // Filtering reorders rows, so do not reuse a highlight that now names another command.
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
        ImGui::SetNextWindowFocus();   // only on open; forcing it every frame blocks other windows

    if ( !ImGui::Begin( "Command palette", &s_open,
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize ) )
    {
        ImGui::End();
        return;
    }

    // Rebuild only after a successful Begin. The filter latch stays above so selection resets
    // before this rebuild consumes the previous frame's InputText value.
    Rebuild();

    // Auto-focus makes io.WantTextInput keep typed characters out of Radiant hotkeys.
    if ( s_focusNext )
    {
        ImGui::SetKeyboardFocusHere();
        s_focusNext = false;
    }
    ImGui::SetNextItemWidth( -1.0f );
    const bool entered = ImGui::InputTextWithHint( "##kiwipalettefilter", "Search commands...",
                                                   s_filter, sizeof( s_filter ),
                                                   ImGuiInputTextFlags_EnterReturnsTrue );

    // Single-line InputText leaves these navigation keys available to the palette.
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
            s_scrollToSel = true;            // follow keyboard navigation without fighting mouse scrolling
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

    if ( ImGui::BeginChild( "##kiwipaletterows", ImVec2( 0.0f, 0.0f ), 0,
                            ImGuiWindowFlags_NoSavedSettings ) )
    {
        for ( int i = 0; i < rowCount; ++i )
        {
            const row_t &r = s_rows[i];
            ImGui::PushID( i );

            // SameLine uses an absolute offset, so capture the row width before Selectable.
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

            // Align the shortcut against the row width captured before Selectable.
            KiwiCmdUI_RightAlignedHint( r.shortcut, rowW );

            if ( !r.enabled )
                ImGui::PopStyleColor();

            // Scroll only after keyboard navigation so mouse-wheel scrolling is not overridden.
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

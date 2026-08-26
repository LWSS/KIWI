#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Shared helpers keep command-palette and add-menu matching and shortcut UI consistent.

#include "radiant_frame.h"      // RadiantCommand and live command-table formatters
#include <imgui/imgui.h>
#include <ctype.h>
#include <stdio.h>

// Case-insensitive subsequence match; an empty pattern matches all.
inline bool KiwiCmdUI_Fuzzy( const char *str, const char *pat )
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

// Use live formatters so keymap changes propagate; CommandList_Mods includes the trailing " + ".
// Valid buffers are always terminated and empty for unbound commands.
inline void KiwiCmdUI_ShortcutText( const RadiantCommand &c, char *out, int outSize )
{
    if ( !out || outSize < 1 )
        return;
    out[0] = '\0';
    if ( !c.vk )
        return;
    char mods[64];
    char keybuf[8];
    CommandList_Mods( c, mods );
    const char *key = CommandList_KeyName( c, keybuf );
    _snprintf( out, outSize, "%s%s", mods, key ? key : "" );
    out[outSize - 1] = '\0';
}

// Resolve add-menu command IDs in the live table; absent or unbound IDs leave `out` empty.
inline void KiwiCmdUI_ShortcutForId( int commandId, char *out, int outSize )
{
    if ( !out || outSize < 1 )
        return;
    out[0] = '\0';
    const RadiantCommand *table = 0;
    const int count = Radiant_GetCommandTable( &table );
    if ( !table )
        return;
    for ( int i = 0; i < count; ++i )
    {
        if ( table[i].commandId != commandId || !table[i].vk )
            continue;
        KiwiCmdUI_ShortcutText( table[i], out, outSize );
        return;
    }
}

// Call immediately after Selectable; `rowW` is its content width and 8 px is right padding.
// Empty text is ignored.
inline void KiwiCmdUI_RightAlignedHint( const char *text, float rowW )
{
    if ( !text || !text[0] )
        return;
    float x = rowW - ImGui::CalcTextSize( text ).x - 8.0f;
    if ( x < 0.0f )
        x = 0.0f;
    ImGui::SameLine( x );
    ImGui::TextDisabled( "%s", text );
}

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_cmdui.h — KIWI-UX (CLEANUP, C-72): the three things the §15 command
// palette and the §16b add menu were each doing their own way.
//
// kiwi_addmenu.cpp:88 wrote the excuse down: the fuzzy match was "duplicated
// rather than exported because it is nine lines and exporting it would mean a
// header for one predicate".  It is three things now (match, shortcut text,
// right-aligned hint), both files also needed one copy of `struct RadiantCommand`
// each (C-6), and the two lists have to FEEL the same — a filter that accepts a
// query in one and rejects it in the other is a bug the user cannot report.  So
// the header exists, and this is what is in it.
//
// Header-only, all `inline`, no .cpp.  The bodies are the palette's verbatim —
// the add menu's copies were character-identical apart from comments.
// ─────────────────────────────────────────────────────────────────────────────

#include "radiant_frame.h"      // struct RadiantCommand + CommandList_KeyName / _Mods
#include <imgui/imgui.h>
#include <ctype.h>
#include <stdio.h>

// Subsequence fuzzy match, case-insensitive: every character of `pat` must appear
// in `str` in order.  Spaces in the pattern are separators, not literals.  An
// empty pattern matches everything.  Deliberately the simplest thing that works —
// both lists are ~200 rows and a ranking model is not what makes them feel good
// at this size.
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

// One row's binding as display text, through mainfrm's OWN formatters, so a
// keymap-profile switch shows up with no work here.  `out` is always terminated
// and is EMPTY when the row carries no key.  (CommandList_Mods leaves a trailing
// " + ", which is why this is a plain concatenation.)
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

// The same, found BY ID in the live table (the add menu's rows are ids, not table
// entries).  Empty when the id is unbound or absent.
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

// The shortcut hint, RIGHT-ALIGNED on the row just submitted (§15).  Call it
// straight after the row's Selectable; `rowW` is the list's content width.  No-op
// for empty text, so callers do not need their own guard.
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

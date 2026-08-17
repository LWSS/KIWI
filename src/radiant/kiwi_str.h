#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_str.h — KIWI-UX (CLEANUP, C-66): the one case-insensitive substring test.
//
// There were three ContainsNoCase implementations with three different answers:
//   kiwi_entbrowser.cpp  empty needle -> TRUE  (an empty filter matches all)
//   kiwi_material.cpp    empty needle -> FALSE (the opposite, same name)
//   kiwi_skybox.cpp      hand-inlined into GatherSky, folding case with
//                        `( *a | 32 ) == ( *b | 32 )`, which is not ASCII-correct:
//                        it makes '[' == '{', '\' == '|' and ']' == '}' compare
//                        equal, and material paths contain backslashes.
//
// THE ANSWER KEPT, stated once: an EMPTY OR NULL NEEDLE MATCHES (it is a search
// filter, and an empty filter shows everything).  A NULL HAYSTACK never matches.
// That is the safest of the three because it is the one every live call site
// already wanted — kiwi_material.cpp's needles are the non-empty KMTL_TOOL_NAMES
// literals and its haystack is guarded non-empty, so its opposite answer was
// unreachable; the skybox's copy only ran behind `if ( s_search[0] )`.
//
// Header-only, all `inline`, no .cpp, no includes.
// ─────────────────────────────────────────────────────────────────────────────

// ASCII-only case fold.  Deliberately not tolower(): this walks material paths
// and eclass names, where the locale must not change the answer.
inline char KiwiStr_LowerAscii( char c )
{
    return ( c >= 'A' && c <= 'Z' ) ? (char)( c + ( 'a' - 'A' ) ) : c;
}

// Does `hay` contain `needle`, ignoring ASCII case?  See the empty/null rule above.
inline bool KiwiStr_ContainsNoCase( const char *hay, const char *needle )
{
    if ( !needle || !needle[0] )
        return true;
    if ( !hay )
        return false;
    for ( const char *h = hay; *h; ++h )
    {
        const char *a = h, *b = needle;
        while ( *a && *b && KiwiStr_LowerAscii( *a ) == KiwiStr_LowerAscii( *b ) )
            ++a, ++b;
        if ( !*b )
            return true;
    }
    return false;
}

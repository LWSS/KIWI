#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// ASCII-only folding keeps path/name matching locale-independent and avoids
// punctuation aliases such as backslash matching '|' under the `c | 32` shortcut.
inline char KiwiStr_LowerAscii( char c )
{
    return ( c >= 'A' && c <= 'Z' ) ? (char)( c + ( 'a' - 'A' ) ) : c;
}

// Empty/null needles match any haystack so an empty filter shows everything;
// otherwise, matching is ASCII case-insensitive and a null haystack does not match.
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

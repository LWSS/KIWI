#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include "qe3.h"
#include "kiwi_entinspect.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <map>

extern eclass_t *g_eclass; // eclass.cpp:117

namespace
{
    struct SchemaCacheEntry
    {
        SchemaCacheEntry() : initialized( false ) {}

        bool          initialized;
        std::string   comments;
        KiwiEntSchema schema;
    };

    std::map<const eclass_t *, SchemaCacheEntry> s_schemaCache;
    size_t s_cachedClassCount = (size_t)-1;
    const eclass_t *s_cachedClassHead = nullptr;

    char LowerAscii( char c )
    {
        return ( c >= 'A' && c <= 'Z' ) ? (char)( c + ( 'a' - 'A' ) ) : c;
    }

    std::string LowerString( const std::string &value )
    {
        std::string out = value;
        for ( size_t i = 0; i < out.size(); ++i )
            out[i] = LowerAscii( out[i] );
        return out;
    }

    bool EqualNoCase( const char *a, const char *b )
    {
        if ( !a || !b )
            return a == b;
        while ( *a && *b )
        {
            if ( LowerAscii( *a ) != LowerAscii( *b ) )
                return false;
            ++a;
            ++b;
        }
        return *a == *b;
    }

    bool StartsNoCase( const char *value, const char *prefix )
    {
        if ( !value || !prefix )
            return false;
        while ( *prefix )
        {
            if ( !*value || LowerAscii( *value ) != LowerAscii( *prefix ) )
                return false;
            ++value;
            ++prefix;
        }
        return true;
    }

    std::string Trim( const std::string &value )
    {
        size_t first = 0;
        while ( first < value.size() && isspace( (unsigned char)value[first] ) )
            ++first;
        size_t last = value.size();
        while ( last > first && isspace( (unsigned char)value[last - 1] ) )
            --last;
        return value.substr( first, last - first );
    }

    bool ContainsNoCase( const std::string &value, const char *needle )
    {
        return LowerString( value ).find( LowerString( needle ? needle : "" ) ) != std::string::npos;
    }

    int ParseNumberCount( const std::string &value )
    {
        const char *p = value.c_str();
        int count = 0;
        while ( 1 )
        {
            while ( *p && isspace( (unsigned char)*p ) )
                ++p;
            if ( !*p )
                return count;

            char *end = nullptr;
            strtod( p, &end );
            if ( end == p )
                return -1;
            ++count;
            p = end;
        }
    }

    bool IsSpawnFlagName( const eclass_t *eclass, const std::string &name )
    {
        if ( EqualNoCase( name.c_str(), "spawnflags" ) )
            return true;
        if ( !eclass )
            return false;

        const char *const flags[9] =
        {
            eclass->flagname0, eclass->flagname1, eclass->flagname2,
            eclass->flagname3, eclass->flagname4, eclass->flagname5,
            eclass->flagname6, eclass->flagname7, eclass->flagname8
        };
        for ( int i = 0; i < 9; ++i )
            if ( flags[i][0] && EqualNoCase( flags[i], name.c_str() ) )
                return true;
        return false;
    }

    int FindParameter( const KiwiEntSchema &schema, const std::string &name )
    {
        for ( size_t i = 0; i < schema.parameters.size(); ++i )
            if ( EqualNoCase( schema.parameters[i].name.c_str(), name.c_str() ) )
                return (int)i;
        return -1;
    }

    bool ReadQuoted( const std::string &text, size_t *cursor, std::string *out )
    {
        size_t p = *cursor;
        while ( p < text.size() && isspace( (unsigned char)text[p] ) )
            ++p;
        if ( p >= text.size() || text[p] != '"' )
            return false;
        const size_t first = ++p;
        while ( p < text.size() && text[p] != '"' )
            ++p;
        if ( p >= text.size() )
            return false;
        *out = text.substr( first, p - first );
        *cursor = p + 1;
        return true;
    }

    bool ReadToken( const std::string &text, size_t *cursor, std::string *out )
    {
        size_t p = *cursor;
        while ( p < text.size() && isspace( (unsigned char)text[p] ) )
            ++p;
        if ( p >= text.size() )
            return false;
        if ( text[p] == '"' )
        {
            *cursor = p;
            return ReadQuoted( text, cursor, out );
        }

        const size_t first = p;
        while ( p < text.size() && !isspace( (unsigned char)text[p] ) )
            ++p;
        *out = text.substr( first, p - first );
        *cursor = p;
        return !out->empty();
    }

    struct ParsedDefault
    {
        std::string name;
        std::string value;
    };

    void ParseDefaults( const std::string &comments,
                        std::map<std::string, std::string> *defaults,
                        std::vector<ParsedDefault> *orderedDefaults )
    {
        size_t marker = 0;
        while ( ( marker = comments.find( "default:", marker ) ) != std::string::npos )
        {
            size_t cursor = marker + 8;
            std::string key;
            std::string value;
            if ( ReadToken( comments, &cursor, &key ) && ReadToken( comments, &cursor, &value ) )
            {
                ( *defaults )[LowerString( key )] = value;
                bool found = false;
                for ( size_t i = 0; i < orderedDefaults->size(); ++i )
                {
                    if ( EqualNoCase( ( *orderedDefaults )[i].name.c_str(), key.c_str() ) )
                    {
                        ( *orderedDefaults )[i].value = value;
                        found = true;
                        break;
                    }
                }
                if ( !found )
                {
                    ParsedDefault parsed;
                    parsed.name = key;
                    parsed.value = value;
                    orderedDefaults->push_back( parsed );
                }
            }
            marker = cursor > marker ? cursor : marker + 8;
        }
    }

    bool IsUpperIdentifier( const std::string &name )
    {
        bool haveLetter = false;
        for ( size_t i = 0; i < name.size(); ++i )
        {
            const unsigned char c = (unsigned char)name[i];
            if ( c >= 'a' && c <= 'z' )
                return false;
            if ( c >= 'A' && c <= 'Z' )
                haveLetter = true;
        }
        return haveLetter;
    }

    bool ParseDocumentedLine( const std::string &line, std::string *name,
                              std::string *description )
    {
        name->clear();
        description->clear();
        if ( line.empty() )
            return false;

        if ( line[0] == '"' )
        {
            const size_t quote = line.find( '"', 1 );
            if ( quote == std::string::npos )
                return false;
            *name = line.substr( 1, quote - 1 );
            *description = Trim( line.substr( quote + 1 ) );
            if ( !description->empty() && ( ( *description )[0] == '-' ||
                                             ( *description )[0] == ':' ) )
                *description = Trim( description->substr( 1 ) );
            return !name->empty();
        }

        if ( StartsNoCase( line.c_str(), "Specify a \"" ) )
        {
            const size_t firstQuote = line.find( '"' );
            const size_t lastQuote = line.find( '"', firstQuote + 1 );
            if ( lastQuote != std::string::npos )
            {
                *name = line.substr( firstQuote + 1, lastQuote - firstQuote - 1 );
                *description = Trim( line.substr( lastQuote + 1 ) );
                return !name->empty();
            }
        }

        size_t keyEnd = 0;
        while ( keyEnd < line.size() && !isspace( (unsigned char)line[keyEnd] ) &&
                line[keyEnd] != ':' )
            ++keyEnd;
        if ( keyEnd == 0 )
            return false;

        std::string candidate = line.substr( 0, keyEnd );
        bool attachedDash = !candidate.empty() && candidate[candidate.size() - 1] == '-';
        if ( attachedDash )
            candidate.erase( candidate.size() - 1 );
        if ( candidate.empty() )
            return false;
        for ( size_t i = 0; i < candidate.size(); ++i )
        {
            const unsigned char c = (unsigned char)candidate[i];
            if ( !( isalnum( c ) || c == '_' ) )
                return false;
        }

        size_t cursor = keyEnd;
        size_t whitespace = 0;
        while ( cursor < line.size() && isspace( (unsigned char)line[cursor] ) )
        {
            ++cursor;
            ++whitespace;
        }

        bool separated = attachedDash || whitespace >= 2;
        if ( cursor < line.size() && ( line[cursor] == '-' || line[cursor] == ':' ) )
        {
            separated = true;
            ++cursor;
            while ( cursor < line.size() && isspace( (unsigned char)line[cursor] ) )
                ++cursor;
        }
        if ( !separated )
            return false;

        *name = candidate;
        *description = Trim( line.substr( cursor ) );
        return true;
    }

    int AddDocumentedParameter( KiwiEntSchema *schema,
                                const std::map<std::string, std::string> &defaults,
                                const std::string &name, const std::string &description )
    {
        int index = FindParameter( *schema, name );
        if ( index < 0 )
        {
            const std::map<std::string, std::string>::const_iterator found =
                defaults.find( LowerString( name ) );
            const char *defaultValue = found != defaults.end() ? found->second.c_str() : "";
            schema->parameters.push_back(
                KiwiEntInspect_InferParameter( name.c_str(), description.c_str(), defaultValue ) );
            return (int)schema->parameters.size() - 1;
        }

        if ( !description.empty() )
        {
            std::string &existing = schema->parameters[index].description;
            if ( !existing.empty() )
                existing += " ";
            existing += description;
        }
        return index;
    }

    KiwiEntSchema ParseSchema( const eclass_t *eclass, const std::string &comments )
    {
        KiwiEntSchema schema;
        std::map<std::string, std::string> defaults;
        std::vector<ParsedDefault> orderedDefaults;
        ParseDefaults( comments, &defaults, &orderedDefaults );

        int activeParameter = -1;
        size_t lineStart = 0;
        while ( lineStart <= comments.size() )
        {
            size_t lineEnd = comments.find( '\n', lineStart );
            if ( lineEnd == std::string::npos )
                lineEnd = comments.size();
            std::string line = comments.substr( lineStart, lineEnd - lineStart );
            if ( !line.empty() && line[line.size() - 1] == '\r' )
                line.erase( line.size() - 1 );
            const std::string trimmed = Trim( line );
            const bool indentedQuoted = !line.empty() &&
                isspace( (unsigned char)line[0] ) && !trimmed.empty() && trimmed[0] == '"';

            if ( trimmed.empty() )
            {
                activeParameter = -1;
            }
            else if ( StartsNoCase( trimmed.c_str(), "default:" ) )
            {
                activeParameter = -1;
            }
            else
            {
                std::string name;
                std::string description;
                if ( ParseDocumentedLine( trimmed, &name, &description ) )
                {
                    if ( indentedQuoted )
                    {
                        if ( activeParameter >= 0 )
                        {
                            std::string &existing = schema.parameters[activeParameter].description;
                            if ( !existing.empty() )
                                existing += " ";
                            existing += trimmed;
                        }
                    }
                    else if ( name.empty() || IsSpawnFlagName( eclass, name ) ||
                          IsUpperIdentifier( name ) )
                        activeParameter = -1;
                    else
                        activeParameter = AddDocumentedParameter(
                            &schema, defaults, name, description );
                }
                else if ( activeParameter >= 0 )
                {
                    std::string &existing = schema.parameters[activeParameter].description;
                    if ( !existing.empty() )
                        existing += " ";
                    existing += trimmed;
                }
            }

            if ( lineEnd == comments.size() )
                break;
            lineStart = lineEnd + 1;
        }

        // KIWI: a default line documents a usable key even when no prose row names it.
        for ( size_t i = 0; i < orderedDefaults.size(); ++i )
        {
            const ParsedDefault &parsed = orderedDefaults[i];
            if ( FindParameter( schema, parsed.name ) < 0 &&
                 !IsSpawnFlagName( eclass, parsed.name ) &&
                 !IsUpperIdentifier( parsed.name ) )
            {
                schema.parameters.push_back( KiwiEntInspect_InferParameter(
                    parsed.name.c_str(), "", parsed.value.c_str() ) );
            }
        }

        for ( size_t i = 0; i < schema.parameters.size(); ++i )
        {
            const KiwiEntParameter parsed = schema.parameters[i];
            schema.parameters[i] = KiwiEntInspect_InferParameter(
                parsed.name.c_str(), parsed.description.c_str(), parsed.defaultValue.c_str() );
        }

        // KIWI: cod4.def describes light targeting in prose, not with a quoted key line.
        if ( eclass && StartsNoCase( eclass->name, "light" ) && FindParameter( schema, "target" ) < 0 )
        {
            schema.parameters.push_back( KiwiEntInspect_InferParameter(
                "target", "Points the light at the entity with this targetname.", "" ) );
        }

        return schema;
    }

    void SetRange( KiwiEntParameter *parameter, float speed, float minimum, float maximum )
    {
        parameter->dragSpeed = speed;
        parameter->dragMin = minimum;
        parameter->dragMax = maximum;
        parameter->hasDragRange = true;
    }

    // KIWI: cod4.def documents many defaults in prose rather than with a "default:"
    // marker, e.g. misc_model "modelscale  scale multiplier (defaults to 1x, ...)".
    // Without this the Inspector showed 0.000 for an absent modelscale, which reads
    // as "scale zero / not editable" when the engine default is 1. Accepts
    // "defaults to <num>", "default <num>", "default is <num>", "default of <num>";
    // a trailing unit letter such as the "x" in "1x" is dropped.
    bool DefaultFromProse( const std::string &description, std::string *value )
    {
        static const char *const markers[] = { "defaults to ", "default is ", "default of ", "default " };
        const std::string lowered = LowerString( description );
        for ( size_t m = 0; m < sizeof( markers ) / sizeof( markers[0] ); ++m )
        {
            size_t at = lowered.find( markers[m] );
            if ( at == std::string::npos )
                continue;
            size_t cursor = at + strlen( markers[m] );
            while ( cursor < lowered.size() && isspace( (unsigned char)lowered[cursor] ) )
                ++cursor;
            size_t end = cursor;
            if ( end < lowered.size() && ( lowered[end] == '-' || lowered[end] == '+' ) )
                ++end;
            bool digits = false;
            while ( end < lowered.size() && ( isdigit( (unsigned char)lowered[end] ) || lowered[end] == '.' ) )
            {
                digits = digits || isdigit( (unsigned char)lowered[end] );
                ++end;
            }
            if ( !digits )
                continue;
            *value = lowered.substr( cursor, end - cursor );
            return true;
        }
        return false;
    }
}

KiwiEntParameter KiwiEntInspect_InferParameter( const char *name,
                                                const char *description,
                                                const char *defaultValue )
{
    KiwiEntParameter parameter;
    parameter.name = name ? name : "";
    parameter.description = description ? description : "";
    parameter.defaultValue = defaultValue ? defaultValue : "";
    parameter.widget = KIWI_ENT_WIDGET_TEXT;
    parameter.dragSpeed = 0.1f;
    parameter.dragMin = 0.0f;
    parameter.dragMax = 0.0f;
    parameter.hasDragRange = false;

    const char *key = parameter.name.c_str();

    // KIWI: fill a missing default from the prose, then from the engine's own
    // hard defaults for keys whose absence has a well-defined meaning.
    if ( parameter.defaultValue.empty() )
    {
        std::string prose;
        if ( DefaultFromProse( parameter.description, &prose ) )
            parameter.defaultValue = prose;
        else if ( EqualNoCase( key, "modelscale" ) )
            parameter.defaultValue = "1";   // entity.cpp: absent or <= 0 -> 1.0
    }

    // KIWI: explicit well-known keys take precedence over prose/default heuristics.
    if ( EqualNoCase( key, "_color" ) || EqualNoCase( key, "suncolor" ) ||
         EqualNoCase( key, "sundiffusecolor" ) || EqualNoCase( key, "color" ) )
    {
        parameter.widget = KIWI_ENT_WIDGET_COLOR3;
        return parameter;
    }
    if ( EqualNoCase( key, "origin" ) || EqualNoCase( key, "angles" ) )
    {
        parameter.widget = KIWI_ENT_WIDGET_VEC3;
        SetRange( &parameter, EqualNoCase( key, "origin" ) ? 1.0f : 0.5f,
                  EqualNoCase( key, "origin" ) ? -65536.0f : -360.0f,
                  EqualNoCase( key, "origin" ) ?  65536.0f :  360.0f );
        return parameter;
    }

    struct KnownNumber
    {
        const char       *name;
        KiwiEntWidgetType widget;
        float             speed;
        float             minimum;
        float             maximum;
    };
    static const KnownNumber numbers[] =
    {
        { "radius",          KIWI_ENT_WIDGET_FLOAT, 1.00f,    0.0f,   4096.0f },
        { "intensity",       KIWI_ENT_WIDGET_FLOAT, 0.05f,    0.0f,     10.0f },
        { "fov_inner",       KIWI_ENT_WIDGET_FLOAT, 0.50f,    0.0f,    180.0f },
        { "fov_outer",       KIWI_ENT_WIDGET_FLOAT, 0.50f,    0.0f,    180.0f },
        { "exponent",        KIWI_ENT_WIDGET_FLOAT, 0.05f,    0.0f,     10.0f },
        { "modelscale",      KIWI_ENT_WIDGET_FLOAT, 0.01f,    0.0f,    100.0f },
        { "maxturn",         KIWI_ENT_WIDGET_FLOAT, 0.50f,    0.0f,    360.0f },
        { "maxmove",         KIWI_ENT_WIDGET_FLOAT, 1.00f,    0.0f,   4096.0f },
        { "sunlight",        KIWI_ENT_WIDGET_FLOAT, 0.05f,    0.0f,     10.0f },
        { "diffusefraction", KIWI_ENT_WIDGET_FLOAT, 0.01f,    0.0f,      1.0f },
        { "ambient",         KIWI_ENT_WIDGET_FLOAT, 0.01f,    0.0f,     10.0f },
        { "speed",           KIWI_ENT_WIDGET_FLOAT, 1.00f,    0.0f,   4096.0f },
        { "wait",            KIWI_ENT_WIDGET_FLOAT, 0.05f,   -1.0f,   3600.0f },
        { "delay",           KIWI_ENT_WIDGET_FLOAT, 0.05f,    0.0f,   3600.0f },
        { "health",          KIWI_ENT_WIDGET_INT,   1.00f,    0.0f, 100000.0f },
        { "dmg",             KIWI_ENT_WIDGET_INT,   1.00f,    0.0f, 100000.0f },
        { "count",           KIWI_ENT_WIDGET_INT,   1.00f,    0.0f, 100000.0f },
        { "random",          KIWI_ENT_WIDGET_FLOAT, 0.05f,    0.0f,   3600.0f }
    };
    for ( size_t i = 0; i < sizeof( numbers ) / sizeof( numbers[0] ); ++i )
    {
        if ( EqualNoCase( key, numbers[i].name ) )
        {
            parameter.widget = numbers[i].widget;
            SetRange( &parameter, numbers[i].speed, numbers[i].minimum, numbers[i].maximum );
            return parameter;
        }
    }

    if ( EqualNoCase( key, "target" ) || EqualNoCase( key, "targetname" ) ||
         EqualNoCase( key, "script_noteworthy" ) || EqualNoCase( key, "script_linkname" ) ||
         EqualNoCase( key, "killtarget" ) )
    {
        parameter.widget = KIWI_ENT_WIDGET_TARGET;
        return parameter;
    }
    if ( EqualNoCase( key, "model" ) )
    {
        parameter.widget = KIWI_ENT_WIDGET_MODEL;
        return parameter;
    }
    if ( EqualNoCase( key, "def" ) )
    {
        parameter.widget = KIWI_ENT_WIDGET_DEF;
        return parameter;
    }

    // KIWI: unit-specific matches must precede broad boolean words such as "enable".
    if ( ContainsNoCase( parameter.description, "degrees" ) )
    {
        parameter.widget = KIWI_ENT_WIDGET_ANGLE;
        SetRange( &parameter, 0.5f, -360.0f, 360.0f );
    }
    else if ( ContainsNoCase( parameter.description, "units" ) )
    {
        parameter.widget = KIWI_ENT_WIDGET_DISTANCE;
        SetRange( &parameter, 1.0f, -65536.0f, 65536.0f );
    }
    else if ( ContainsNoCase( parameter.description, "0 or 1" ) ||
              ContainsNoCase( parameter.description, "true" ) ||
              ContainsNoCase( parameter.description, "false" ) ||
              ContainsNoCase( parameter.description, "enable" ) )
    {
        parameter.widget = KIWI_ENT_WIDGET_BOOL;
    }
    else
    {
        const int numberCount = ParseNumberCount( parameter.defaultValue );
        if ( numberCount == 3 )
        {
            parameter.widget = KIWI_ENT_WIDGET_VEC3;
            SetRange( &parameter, 0.1f, -65536.0f, 65536.0f );
        }
        else if ( numberCount == 1 )
        {
            parameter.widget = KIWI_ENT_WIDGET_FLOAT;
        }
    }

    return parameter;
}

const KiwiEntSchema &KiwiEntInspect_GetSchema( const eclass_t *eclass, size_t eclassCount )
{
    static const KiwiEntSchema emptySchema;
    if ( !eclass )
        return emptySchema;

    if ( s_cachedClassCount != eclassCount || s_cachedClassHead != g_eclass )
    {
        s_schemaCache.clear();
        s_cachedClassCount = eclassCount;
        s_cachedClassHead = g_eclass;
    }

    const std::string comments = eclass->comments ? eclass->comments : "";
    SchemaCacheEntry &entry = s_schemaCache[eclass];
    if ( !entry.initialized || entry.comments != comments )
    {
        entry.initialized = true;
        entry.comments = comments;
        entry.schema = ParseSchema( eclass, comments );
    }
    return entry.schema;
}

#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// KIWI (2026-11-02): .efx documents - see kiwi_efx.h.  Field names, flag table, curve
// rules and defaults mirror src/EffectsCore/fx_load_obj.cpp (the game's parser); the
// layout of the writer matches the shipped raw/fx files.

#include "stdafx.h"

#include "kiwi_efx.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    // s_allFlagDefs, fx_load_obj.cpp:19 - order is also the writer's order.
    const kefxFlagDef_t s_flagDefs[] =
    {
        { "looping",            0, 0x1, 0x1 },
        { "useRandColor",       0, 0x2, 0x2 },
        { "useRandAlpha",       0, 0x4, 0x4 },
        { "useRandSize0",       0, 0x8, 0x8 },
        { "useRandSize1",       0, 0x10, 0x10 },
        { "useRandScale",       0, 0x20, 0x20 },
        { "useRandRotDelta",    0, 0x40, 0x40 },
        { "modColorByAlpha",    0, 0x80, 0x80 },
        { "useRandVel0",        0, 0x100, 0x100 },
        { "useRandVel1",        0, 0x200, 0x200 },
        { "useBackCompatVel",   0, 0x400, 0x400 },
        { "absVel0",            0, 0x800, 0x800 },
        { "absVel1",            0, 0x1000, 0x1000 },
        { "playOnTouch",        0, 0x2000, 0x2000 },
        { "playOnDeath",        0, 0x4000, 0x4000 },
        { "playOnRun",          0, 0x8000, 0x8000 },
        { "boundingSphere",     0, 0x10000, 0x10000 },
        { "useItemClip",        0, 0x20000, 0x20000 },
        { "disabled",           0, 0x80000000u, 0x80000000u },
        { "spawnRelative",      1, 0x2, 0x2 },
        { "spawnFrustumCull",   1, 0x4, 0x4 },
        { "runnerUsesRandRot",  1, 0x8, 0x8 },
        { "spawnOffsetNone",    1, 0x30, 0x0 },
        { "spawnOffsetSphere",  1, 0x30, 0x10 },
        { "spawnOffsetCylinder",1, 0x30, 0x20 },
        { "runRelToWorld",      1, 0xC0, 0x0 },
        { "runRelToSpawn",      1, 0xC0, 0x40 },
        { "runRelToEffect",     1, 0xC0, 0x80 },
        { "runRelToOffset",     1, 0xC0, 0xC0 },
        { "useCollision",       1, 0x100, 0x100 },
        { "dieOnTouch",         1, 0x200, 0x200 },
        { "drawPastFog",        1, 0x400, 0x400 },
        { "drawWithViewModel",  1, 0x800, 0x800 },
        { "blocksSight",        1, 0x1000, 0x1000 },
        { "modelUsesPhysics",   1, 0x8000000, 0x8000000 },
        { "nonUniformScale",    1, 0x10000000, 0x10000000 },
        { "startFixed",         2, 0x3, 0x0 },
        { "startRandom",        2, 0x3, 0x1 },
        { "startIndexed",       2, 0x3, 0x2 },
        { "playOverLife",       2, 0x4, 0x4 },
        { "loopOnlyNTimes",     2, 0x8, 0x8 },
        { nullptr, 0, 0, 0 }
    };

    const char *const s_typeNames[KEFX_TYPE_COUNT] =
    {
        "billboardSprite", "orientedSprite", "tail", "trail", "cloud", "model",
        "light", "spotLight", "sound", "decal", "runner"
    };
    const char *const s_typeLabels[KEFX_TYPE_COUNT] =
    {
        "Billboard sprite", "Oriented sprite", "Tail", "Trail", "Cloud", "Model",
        "Light", "Spot light", "Sound", "Decal", "Runner (child effect)"
    };

    // ── tokenizer (Com_Parse with space-delimiting off: { } ; split words) ─────
    struct lexer_t
    {
        const char *p;
        int         line;
        std::string tok;
        bool        quoted;
        bool        have;      // an unread token in tok (Unget)

        explicit lexer_t( const char *text ) : p( text ), line( 1 ), quoted( false ), have( false ) {}

        bool Next()
        {
            if ( have )
            {
                have = false;
                return true;
            }
            quoted = false;
            tok.clear();
            for ( ;; )
            {
                while ( *p && isspace( (unsigned char)*p ) )
                {
                    if ( *p == '\n' ) ++line;
                    ++p;
                }
                if ( p[0] == '/' && p[1] == '/' )
                {
                    while ( *p && *p != '\n' ) ++p;
                    continue;
                }
                if ( p[0] == '/' && p[1] == '*' )
                {
                    p += 2;
                    while ( *p && !( p[0] == '*' && p[1] == '/' ) )
                    {
                        if ( *p == '\n' ) ++line;
                        ++p;
                    }
                    if ( *p ) p += 2;
                    continue;
                }
                break;
            }
            if ( !*p )
                return false;
            if ( *p == '"' )
            {
                ++p;
                while ( *p && *p != '"' )
                {
                    if ( *p == '\n' ) ++line;
                    tok += *p++;
                }
                if ( *p == '"' ) ++p;
                quoted = true;
                return true;
            }
            if ( *p == '{' || *p == '}' || *p == ';' )
            {
                tok = *p++;
                return true;
            }
            while ( *p && !isspace( (unsigned char)*p ) && *p != '{' && *p != '}' && *p != ';' && *p != '"' )
                tok += *p++;
            return true;
        }
        void Unget() { have = true; }
        bool Is( const char *s ) const { return !quoted && tok == s; }
    };

    struct parser_t
    {
        lexer_t     lx;
        std::string err;

        explicit parser_t( const char *text ) : lx( text ) {}

        bool Fail( const char *msg, const char *detail = nullptr )
        {
            if ( err.empty() )
            {
                char buf[320];
                _snprintf( buf, sizeof( buf ), "line %i: %s%s%s", lx.line, msg, detail ? " " : "", detail ? detail : "" );
                buf[sizeof( buf ) - 1] = '\0';
                err = buf;
            }
            return false;
        }
        bool Expect( const char *s )
        {
            if ( !lx.Next() || !lx.Is( s ) )
                return Fail( "expected", s );
            return true;
        }
        bool Float( float *v )
        {
            if ( !lx.Next() || lx.Is( ";" ) || lx.Is( "{" ) || lx.Is( "}" ) )
                return Fail( "expected a number" );
            *v = (float)atof( lx.tok.c_str() );
            return true;
        }
        bool Int( int *v )
        {
            if ( !lx.Next() || lx.Is( ";" ) || lx.Is( "{" ) || lx.Is( "}" ) )
                return Fail( "expected an integer" );
            *v = atoi( lx.tok.c_str() );
            return true;
        }
        bool Range( kefxRange_t *r ) { return Float( &r->base ) && Float( &r->amp ); }
        bool IntRange( kefxRange_t *r )
        {
            int a = 0, b = 0;
            if ( !Int( &a ) || !Int( &b ) )
                return false;
            r->base = (float)a;
            r->amp  = (float)b;
            return true;
        }
        bool String( std::string *s )
        {
            if ( !lx.Next() || ( !lx.quoted && ( lx.Is( ";" ) || lx.Is( "{" ) || lx.Is( "}" ) ) ) )
                return Fail( "expected a string" );
            *s = lx.tok;
            return true;
        }
        // Flag words up to ';' (left unread), FX_ParseSingleFlag semantics: clear the mask, set the value.
        bool Flags( unsigned *word0, unsigned *word1, unsigned *word2 )
        {
            for ( ;; )
            {
                if ( !lx.Next() )
                    return Fail( "unterminated flags" );
                if ( lx.Is( ";" ) )
                {
                    lx.Unget();
                    return true;
                }
                const kefxFlagDef_t *d = s_flagDefs;
                unsigned *out = nullptr;
                for ( ; d->name; ++d )
                {
                    unsigned *w = d->word == 0 ? word0 : d->word == 1 ? word1 : word2;
                    if ( w && !strcmp( lx.tok.c_str(), d->name ) )
                    {
                        out = w;
                        break;
                    }
                }
                if ( !out )
                    return Fail( "unknown flag", lx.tok.c_str() );
                *out = ( *out & ~d->mask ) | d->value;
            }
        }
        // FX_ParseCurve: keys until '}', clamped values, >= 2 keys from t=0 to t=1, increasing.
        bool Curve( kefxCurve_t *c, int dims, float lo, float hi )
        {
            if ( !Expect( "{" ) )
                return false;
            c->keys.clear();
            for ( ;; )
            {
                if ( !lx.Next() )
                    return Fail( "unterminated curve" );
                if ( lx.Is( "}" ) )
                    break;
                float v = (float)atof( lx.tok.c_str() );
                if ( c->keys.size() % ( dims + 1 ) )
                    v = v < lo ? lo : ( v > hi ? hi : v );
                c->keys.push_back( v );
                if ( (int)c->keys.size() > 512 )
                    return Fail( "curve has too many keys" );
            }
            if ( c->keys.size() % ( dims + 1 ) )
                return Fail( "curve has a partial key" );
            const int n = c->Count( dims );
            if ( n < 2 )
                return Fail( "curves need at least 2 keys" );
            if ( c->keys[0] != 0.0f || c->keys[( dims + 1 ) * ( n - 1 )] != 1.0f )
                return Fail( "curves must start at time 0 and end at time 1" );
            for ( int k = 1; k < n; ++k )
                if ( c->keys[( dims + 1 ) * ( k - 1 )] >= c->keys[( dims + 1 ) * k] )
                    return Fail( "curve times must increase" );
            return true;
        }
        bool Graph( kefxGraph_t *g, int dims, float lo, float hi )
        {
            return Float( &g->scale ) && Expect( "{" ) && Curve( &g->curve[0], dims, lo, hi )
                && Curve( &g->curve[1], dims, lo, hi ) && Expect( "}" );
        }
        bool Visuals( kefxElem_t *e, int type, bool block )
        {
            if ( e->type != KEFX_NONE )
                return Fail( "more than one type of visuals in an element" );
            e->type = type;
            e->visuals.clear();
            if ( !block )
                return true;
            if ( !Expect( "{" ) )
                return false;
            for ( ;; )
            {
                if ( !lx.Next() )
                    return Fail( "unterminated visuals" );
                if ( lx.Is( "}" ) )
                    return true;
                if ( (int)e->visuals.size() >= ( type == KEFX_DECAL ? 16 : KEFX_MAX_VISUALS ) )
                    return Fail( "too many visuals" );
                e->visuals.push_back( lx.tok );
            }
        }
        bool TrailDef( kefxElem_t *e )
        {
            e->hasTrailDef = true;
            e->trailVerts.clear();
            e->trailInds.clear();
            if ( !Expect( "{" ) )
                return false;
            for ( ;; )
            {
                if ( !lx.Next() )
                    return Fail( "unterminated trailDef" );
                if ( lx.Is( "}" ) )
                    break;
                lx.Unget();
                float v[3];
                if ( !Float( &v[0] ) || !Float( &v[1] ) || !Float( &v[2] ) )
                    return false;
                if ( e->trailVerts.size() >= 64 * 3 )
                    return Fail( "too many trail vertices" );
                e->trailVerts.insert( e->trailVerts.end(), v, v + 3 );
            }
            if ( !Expect( "{" ) )
                return false;
            for ( ;; )
            {
                if ( !lx.Next() )
                    return Fail( "unterminated trailDef" );
                if ( lx.Is( "}" ) )
                    return true;
                if ( e->trailInds.size() >= 128 )
                    return Fail( "too many trail indices" );
                e->trailInds.push_back( atoi( lx.tok.c_str() ) );
            }
        }

        bool Field( kefxElem_t *e, const std::string &key )
        {
            const char *k = key.c_str();
            float dummy = 0.0f;
            if ( !strcmp( k, "name" ) )                    return String( &e->name );
            if ( !strcmp( k, "editorFlags" ) || !strcmp( k, "flags" ) )
                return Flags( &e->editorFlags, &e->flags, nullptr );
            if ( !strcmp( k, "spawnRange" ) )              return Range( &e->spawnRange );
            if ( !strcmp( k, "fadeInRange" ) )             return Range( &e->fadeInRange );
            if ( !strcmp( k, "fadeOutRange" ) )            return Range( &e->fadeOutRange );
            if ( !strcmp( k, "spawnFrustumCullRadius" ) )  return Float( &e->spawnFrustumCullRadius );
            if ( !strcmp( k, "spawnLooping" ) )            return Int( &e->loopIntervalMsec ) && Int( &e->loopCount );
            if ( !strcmp( k, "spawnOneShot" ) )            return IntRange( &e->oneShotCount );
            if ( !strcmp( k, "spawnDelayMsec" ) )          return IntRange( &e->spawnDelayMsec );
            if ( !strcmp( k, "lifeSpanMsec" ) )            return IntRange( &e->lifeSpanMsec );
            if ( !strcmp( k, "spawnOrgX" ) )               return Range( &e->spawnOrg[0] );
            if ( !strcmp( k, "spawnOrgY" ) )               return Range( &e->spawnOrg[1] );
            if ( !strcmp( k, "spawnOrgZ" ) )               return Range( &e->spawnOrg[2] );
            if ( !strcmp( k, "spawnOffsetRadius" ) )       return Range( &e->spawnOffsetRadius );
            if ( !strcmp( k, "spawnOffsetHeight" ) )       return Range( &e->spawnOffsetHeight );
            if ( !strcmp( k, "spawnAnglePitch" ) )         return Range( &e->spawnAngles[0] );
            if ( !strcmp( k, "spawnAngleYaw" ) )           return Range( &e->spawnAngles[1] );
            if ( !strcmp( k, "spawnAngleRoll" ) )          return Range( &e->spawnAngles[2] );
            if ( !strcmp( k, "angleVelPitch" ) )           return Range( &e->angleVel[0] );
            if ( !strcmp( k, "angleVelYaw" ) )             return Range( &e->angleVel[1] );
            if ( !strcmp( k, "angleVelRoll" ) )            return Range( &e->angleVel[2] );
            if ( !strcmp( k, "initialRot" ) )              return Range( &e->initialRot );
            if ( !strcmp( k, "gravity" ) )                 return Range( &e->gravity );
            if ( !strcmp( k, "elasticity" ) )              return Range( &e->elasticity );
            if ( !strcmp( k, "atlasBehavior" ) )           return Flags( nullptr, nullptr, &e->atlasBehavior );
            if ( !strcmp( k, "atlasIndex" ) )              return Int( &e->atlasIndex );
            if ( !strcmp( k, "atlasFps" ) )                return Int( &e->atlasFps );
            if ( !strcmp( k, "atlasLoopCount" ) )          return Int( &e->atlasLoopCount );
            if ( !strcmp( k, "atlasColIndexBits" ) )       return Int( &e->atlasColIndexBits );
            if ( !strcmp( k, "atlasRowIndexBits" ) )       return Int( &e->atlasRowIndexBits );
            if ( !strcmp( k, "atlasEntryCount" ) )         return Int( &e->atlasEntryCount );
            if ( !strncmp( k, "velGraph", 8 ) && ( k[8] == '0' || k[8] == '1' ) && k[9] >= 'X' && k[9] <= 'Z' && !k[10] )
                return Graph( &e->vel[k[8] - '0'][k[9] - 'X'], 1, -0.5f, 0.5f );
            if ( !strcmp( k, "rotGraph" ) )                return Graph( &e->rot, 1, -0.5f, 0.5f );
            if ( !strcmp( k, "sizeGraph0" ) )              return Graph( &e->size[0], 1, 0.0f, 1.0f );
            if ( !strcmp( k, "sizeGraph1" ) )              return Graph( &e->size[1], 1, 0.0f, 1.0f );
            if ( !strcmp( k, "scaleGraph" ) )              return Graph( &e->scale, 1, 0.0f, 1.0f );
            if ( !strcmp( k, "colorGraph" ) )              return Graph( &e->color, 3, 0.0f, 1.0f );
            if ( !strcmp( k, "alphaGraph" ) )              return Graph( &e->alpha, 1, 0.0f, 1.0f );
            if ( !strcmp( k, "lightingFrac" ) )            return Float( &e->lightingFrac );
            if ( !strcmp( k, "collOffset" ) )              return Float( &e->collOffset[0] ) && Float( &e->collOffset[1] ) && Float( &e->collOffset[2] );
            if ( !strcmp( k, "collRadius" ) )              return Float( &e->collRadius );
            if ( !strcmp( k, "fxOnImpact" ) )              return String( &e->fxOnImpact );
            if ( !strcmp( k, "fxOnDeath" ) )               return String( &e->fxOnDeath );
            if ( !strcmp( k, "sortOrder" ) )               return Int( &e->sortOrder );
            if ( !strcmp( k, "emission" ) )                return String( &e->emission );
            if ( !strcmp( k, "emitDist" ) )                return Range( &e->emitDist );
            if ( !strcmp( k, "emitDistVariance" ) )        return Range( &e->emitDistVariance );
            if ( !strcmp( k, "trailRepeatTime" ) )         return Float( &dummy );      // deprecated (FX_ParseTrailRepeatTime)
            if ( !strcmp( k, "trailSplitDist" ) )          return Int( &e->trailSplitDist );
            if ( !strcmp( k, "trailScrollTime" ) )         return Float( &e->trailScrollTime );
            if ( !strcmp( k, "trailRepeatDist" ) )         return Int( &e->trailRepeatDist );
            if ( !strcmp( k, "trailDef" ) )                return TrailDef( e );
            for ( int t = 0; t < KEFX_TYPE_COUNT; ++t )
                if ( !strcmp( k, s_typeNames[t] ) )
                    return Visuals( e, t, t != KEFX_LIGHT && t != KEFX_SPOTLIGHT );
            return Fail( "unknown field", k );
        }

        bool Elem( int version, kefxElem_t *e )
        {
            *e = kefxElem_t();
            // FX_ParseEditorElem defaults: zeroed, sortOrder 5, v1 files back-compat velocity.
            e->loopIntervalMsec = 0;
            for ( int f = 0; f < 2; ++f )
                for ( int a = 0; a < 3; ++a )
                    e->vel[f][a].scale = 0.0f;
            e->rot.scale = e->size[0].scale = e->size[1].scale = e->scale.scale = 0.0f;
            e->color.scale = e->alpha.scale = 0.0f;
            if ( version < 2 )
                e->editorFlags = KEFX_E_BACKCOMPATVEL;
            for ( ;; )
            {
                if ( !lx.Next() )
                    return Fail( "unterminated element" );
                if ( lx.Is( "}" ) )
                    break;
                const std::string key = lx.tok;
                if ( !Field( e, key ) || !Expect( ";" ) )
                    return false;
            }
            if ( e->type == KEFX_NONE )
                return Fail( "no visual type specified in element", e->name.c_str() );
            // Graph curves missing from an old file: flat ones, so the element still writes.
            kefxGraph_t *graphs[] = { &e->vel[0][0], &e->vel[0][1], &e->vel[0][2], &e->vel[1][0], &e->vel[1][1],
                                      &e->vel[1][2], &e->rot, &e->size[0], &e->size[1], &e->scale, &e->color, &e->alpha };
            for ( size_t g = 0; g < sizeof( graphs ) / sizeof( graphs[0] ); ++g )
                for ( int v = 0; v < 2; ++v )
                    if ( graphs[g]->curve[v].keys.empty() )
                        KiwiEfx_FlatCurve( graphs[g]->curve[v], graphs[g] == &e->color ? 3 : 1,
                                           ( graphs[g] == &e->color || graphs[g] == &e->alpha ) ? 1.0f : 0.0f );
            return true;
        }
    };

    // ── writer ─────────────────────────────────────────────────────────────
    void Num( std::string &out, float v )
    {
        char buf[48];
        _snprintf( buf, sizeof( buf ), "%g", (double)v );
        buf[sizeof( buf ) - 1] = '\0';
        if ( !strcmp( buf, "-0" ) )
            strcpy( buf, "0" );
        out += buf;
    }
    void IntStr( std::string &out, int v )
    {
        char buf[24];
        _snprintf( buf, sizeof( buf ), "%i", v );
        buf[sizeof( buf ) - 1] = '\0';
        out += buf;
    }
    void LineRange( std::string &out, const char *key, const kefxRange_t &r )
    {
        out += "\t"; out += key; out += " ";
        Num( out, r.base ); out += " "; Num( out, r.amp ); out += ";\n";
    }
    void LineIntRange( std::string &out, const char *key, const kefxRange_t &r )
    {
        out += "\t"; out += key; out += " ";
        IntStr( out, (int)floorf( r.base + 0.5f ) ); out += " "; IntStr( out, (int)floorf( r.amp + 0.5f ) ); out += ";\n";
    }
    void LineInt( std::string &out, const char *key, int v )
    {
        out += "\t"; out += key; out += " "; IntStr( out, v ); out += ";\n";
    }
    void LineFloat( std::string &out, const char *key, float v )
    {
        out += "\t"; out += key; out += " "; Num( out, v ); out += ";\n";
    }
    void LineString( std::string &out, const char *key, const std::string &s )
    {
        out += "\t"; out += key; out += " \""; out += s; out += "\";\n";
    }
    void LineFlags( std::string &out, const char *key, int w0, int w1, unsigned v0, unsigned v1 )
    {
        out += "\t"; out += key;
        for ( const kefxFlagDef_t *d = s_flagDefs; d->name; ++d )
        {
            if ( d->word != w0 && d->word != w1 )
                continue;
            const unsigned v = d->word == w0 ? v0 : v1;
            if ( ( v & d->mask ) == d->value && ( d->value || d->mask != d->value ) )
            {
                out += " ";
                out += d->name;
            }
        }
        out += ";\n";
    }
    void LineGraph( std::string &out, const char *key, const kefxGraph_t &g, int dims )
    {
        out += "\t"; out += key; out += " "; Num( out, g.scale ); out += "\n\t{\n";
        for ( int v = 0; v < 2; ++v )
        {
            out += "\t\t{\n";
            const kefxCurve_t &c = g.curve[v];
            const int n = c.Count( dims );
            for ( int k = 0; k < n; ++k )
            {
                out += "\t\t\t";
                for ( int d = 0; d <= dims; ++d )
                {
                    if ( d ) out += " ";
                    Num( out, c.keys[k * ( dims + 1 ) + d] );
                }
                out += "\n";
            }
            out += "\t\t}\n";
        }
        out += "\t};\n";
    }

    std::string ExeDir()
    {
        char exe[MAX_PATH] = { 0 };
        ::GetModuleFileNameA( nullptr, exe, MAX_PATH );
        char *slash = strrchr( exe, '\\' );
        if ( slash )
            *slash = '\0';
        return exe;
    }
}

const kefxFlagDef_t *KiwiEfx_FlagDefs()
{
    return s_flagDefs;
}

const char *KiwiEfx_TypeName( int type )
{
    return ( type >= 0 && type < KEFX_TYPE_COUNT ) ? s_typeNames[type] : "none";
}

const char *KiwiEfx_TypeLabel( int type )
{
    return ( type >= 0 && type < KEFX_TYPE_COUNT ) ? s_typeLabels[type] : "(no visuals)";
}

void KiwiEfx_GraphRange( const kefxGraph_t *g, const kefxElem_t &e, float *lo, float *hi )
{
    const bool signedRange = g == &e.rot || ( g >= &e.vel[0][0] && g <= &e.vel[1][2] );
    *lo = signedRange ? -0.5f : 0.0f;
    *hi = signedRange ? 0.5f : 1.0f;
}

int KiwiEfx_GraphDims( const kefxGraph_t *g, const kefxElem_t &e )
{
    return g == &e.color ? 3 : 1;
}

bool KiwiEfx_Parse( const char *text, kefxDoc_t &out, std::string &err )
{
    out.elems.clear();
    err.clear();
    if ( !text )
    {
        err = "no text";
        return false;
    }
    parser_t ps( text );
    if ( !ps.lx.Next() || _stricmp( ps.lx.tok.c_str(), "iwfx" ) )
    {
        err = "not an iwfx effect (legacy format?)";
        return false;
    }
    int version = 0;
    if ( !ps.Int( &version ) )
    {
        err = ps.err;
        return false;
    }
    if ( version > 2 )
    {
        err = "version is too high (2 is the newest the game reads)";
        return false;
    }
    while ( ps.lx.Next() )
    {
        if ( !ps.lx.Is( "{" ) )
        {
            ps.Fail( "expected '{' to start an element, found", ps.lx.tok.c_str() );
            break;
        }
        if ( (int)out.elems.size() >= KEFX_MAX_ELEMS )
        {
            ps.Fail( "more than 32 elements" );
            break;
        }
        out.elems.push_back( kefxElem_t() );
        if ( !ps.Elem( version, &out.elems.back() ) )
            break;
    }
    if ( !ps.err.empty() )
    {
        err = ps.err;
        out.elems.clear();
        return false;
    }
    return true;
}

std::string KiwiEfx_Write( const kefxDoc_t &doc )
{
    std::string o = "iwfx 2\n\n";
    for ( size_t i = 0; i < doc.elems.size(); ++i )
    {
        const kefxElem_t &e = doc.elems[i];
        o += "{\n";
        LineString( o, "name", e.name );
        LineFlags( o, "editorFlags", 0, -1, e.editorFlags, 0 );
        LineFlags( o, "flags", 1, -1, e.flags, 0 );
        LineRange( o, "spawnRange", e.spawnRange );
        LineRange( o, "fadeInRange", e.fadeInRange );
        LineRange( o, "fadeOutRange", e.fadeOutRange );
        LineFloat( o, "spawnFrustumCullRadius", e.spawnFrustumCullRadius );
        o += "\tspawnLooping "; IntStr( o, e.loopIntervalMsec ); o += " "; IntStr( o, e.loopCount ); o += ";\n";
        LineIntRange( o, "spawnOneShot", e.oneShotCount );
        LineIntRange( o, "spawnDelayMsec", e.spawnDelayMsec );
        LineIntRange( o, "lifeSpanMsec", e.lifeSpanMsec );
        LineRange( o, "spawnOrgX", e.spawnOrg[0] );
        LineRange( o, "spawnOrgY", e.spawnOrg[1] );
        LineRange( o, "spawnOrgZ", e.spawnOrg[2] );
        LineRange( o, "spawnOffsetRadius", e.spawnOffsetRadius );
        LineRange( o, "spawnOffsetHeight", e.spawnOffsetHeight );
        LineRange( o, "spawnAnglePitch", e.spawnAngles[0] );
        LineRange( o, "spawnAngleYaw", e.spawnAngles[1] );
        LineRange( o, "spawnAngleRoll", e.spawnAngles[2] );
        LineRange( o, "angleVelPitch", e.angleVel[0] );
        LineRange( o, "angleVelYaw", e.angleVel[1] );
        LineRange( o, "angleVelRoll", e.angleVel[2] );
        LineRange( o, "initialRot", e.initialRot );
        LineRange( o, "gravity", e.gravity );
        LineRange( o, "elasticity", e.elasticity );
        LineFlags( o, "atlasBehavior", 2, -1, e.atlasBehavior, 0 );
        LineInt( o, "atlasIndex", e.atlasIndex );
        LineInt( o, "atlasFps", e.atlasFps );
        LineInt( o, "atlasLoopCount", e.atlasLoopCount );
        LineInt( o, "atlasColIndexBits", e.atlasColIndexBits );
        LineInt( o, "atlasRowIndexBits", e.atlasRowIndexBits );
        LineInt( o, "atlasEntryCount", e.atlasEntryCount );
        static const char *const kVel[2][3] = { { "velGraph0X", "velGraph0Y", "velGraph0Z" },
                                                { "velGraph1X", "velGraph1Y", "velGraph1Z" } };
        for ( int f = 0; f < 2; ++f )
            for ( int a = 0; a < 3; ++a )
                LineGraph( o, kVel[f][a], e.vel[f][a], 1 );
        LineGraph( o, "rotGraph", e.rot, 1 );
        LineGraph( o, "sizeGraph0", e.size[0], 1 );
        LineGraph( o, "sizeGraph1", e.size[1], 1 );
        LineGraph( o, "scaleGraph", e.scale, 1 );
        LineGraph( o, "colorGraph", e.color, 3 );
        LineGraph( o, "alphaGraph", e.alpha, 1 );
        LineFloat( o, "lightingFrac", e.lightingFrac );
        o += "\tcollOffset "; Num( o, e.collOffset[0] ); o += " "; Num( o, e.collOffset[1] ); o += " ";
        Num( o, e.collOffset[2] ); o += ";\n";
        LineFloat( o, "collRadius", e.collRadius );
        LineString( o, "fxOnImpact", e.fxOnImpact );
        LineString( o, "fxOnDeath", e.fxOnDeath );
        LineInt( o, "sortOrder", e.sortOrder );
        LineString( o, "emission", e.emission );
        LineRange( o, "emitDist", e.emitDist );
        LineRange( o, "emitDistVariance", e.emitDistVariance );
        LineInt( o, "trailSplitDist", e.trailSplitDist );
        LineFloat( o, "trailScrollTime", e.trailScrollTime );
        LineInt( o, "trailRepeatDist", e.trailRepeatDist );
        if ( e.hasTrailDef )
        {
            o += "\ttrailDef\n\t{\n";
            for ( size_t v = 0; v + 2 < e.trailVerts.size(); v += 3 )
            {
                o += "\t\t"; Num( o, e.trailVerts[v] ); o += " "; Num( o, e.trailVerts[v + 1] ); o += " ";
                Num( o, e.trailVerts[v + 2] ); o += "\n";
            }
            o += "\t} {\n";
            for ( size_t k = 0; k < e.trailInds.size(); ++k )
            {
                o += "\t\t"; IntStr( o, e.trailInds[k] ); o += "\n";
            }
            o += "\t};\n";
        }
        if ( e.type == KEFX_LIGHT || e.type == KEFX_SPOTLIGHT )
        {
            o += "\t"; o += s_typeNames[e.type]; o += ";\n";
        }
        else if ( e.type >= 0 && e.type < KEFX_TYPE_COUNT )
        {
            o += "\t"; o += s_typeNames[e.type]; o += "\n\t{\n";
            for ( size_t v = 0; v < e.visuals.size(); ++v )
            {
                o += "\t\t\""; o += e.visuals[v]; o += "\"\n";
            }
            o += "\t};\n";
        }
        o += "}\n";
    }
    return o;
}

std::string KiwiEfx_PathFor( const char *fxName )
{
    std::string rel = fxName ? fxName : "";
    for ( size_t i = 0; i < rel.size(); ++i )
        if ( rel[i] == '/' ) rel[i] = '\\';
    return ExeDir() + "\\raw\\fx\\" + rel + ".efx";
}

bool KiwiEfx_Load( const char *fxName, kefxDoc_t &out, std::string &err )
{
    out.elems.clear();
    const std::string path = KiwiEfx_PathFor( fxName );
    FILE *f = fopen( path.c_str(), "rb" );
    if ( !f )
    {
        err = "cannot open " + path;
        return false;
    }
    std::string text;
    char buf[16384];
    size_t n;
    while ( ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 )
        text.append( buf, n );
    fclose( f );
    if ( !KiwiEfx_Parse( text.c_str(), out, err ) )
    {
        err = std::string( fxName ) + ".efx " + err;
        return false;
    }
    return true;
}

bool KiwiEfx_Save( const char *fxName, const kefxDoc_t &doc, std::string &err )
{
    if ( !fxName || !fxName[0] )
    {
        err = "no effect name";
        return false;
    }
    const std::string path = KiwiEfx_PathFor( fxName );
    // Folders under raw\fx for a new "folder/name".
    for ( size_t i = ExeDir().size() + 1; i < path.size(); ++i )
        if ( path[i] == '\\' )
            ::CreateDirectoryA( path.substr( 0, i ).c_str(), nullptr );
    const std::string text = KiwiEfx_Write( doc );
    const std::string tmp = path + ".kiwitmp";
    FILE *f = fopen( tmp.c_str(), "wb" );
    if ( !f )
    {
        err = "cannot write " + tmp;
        return false;
    }
    const bool ok = fwrite( text.data(), 1, text.size(), f ) == text.size();
    if ( fclose( f ) != 0 || !ok || !::MoveFileExA( tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING ) )
    {
        ::DeleteFileA( tmp.c_str() );
        err = "cannot write " + path;
        return false;
    }
    return true;
}

unsigned long long KiwiEfx_FileStamp( const char *fxName )
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if ( !::GetFileAttributesExA( KiwiEfx_PathFor( fxName ).c_str(), GetFileExInfoStandard, &a ) )
        return 0;
    return ( (unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32 ) | a.ftLastWriteTime.dwLowDateTime;
}

void KiwiEfx_FlatCurve( kefxCurve_t &c, int dims, float v )
{
    c.keys.clear();
    for ( int k = 0; k < 2; ++k )
    {
        c.keys.push_back( (float)k );
        for ( int d = 0; d < dims; ++d )
            c.keys.push_back( v );
    }
}

void KiwiEfx_InitElem( kefxElem_t &e, int type )
{
    e = kefxElem_t();
    e.name = "new_element";
    e.type = type;
    e.editorFlags = KEFX_E_LOOPING | KEFX_E_RANDROTDELTA | KEFX_E_RANDVEL0;
    e.flags = KEFX_F_SPAWNRELATIVE | KEFX_F_RUN_SPAWN;
    e.loopIntervalMsec = 200;
    e.loopCount = 0;
    e.oneShotCount.base = 1.0f;
    e.lifeSpanMsec.base = 1500.0f;
    e.lifeSpanMsec.amp = 500.0f;
    e.initialRot.base = 0.0f;
    e.initialRot.amp = 360.0f;
    e.spawnFrustumCullRadius = 100.0f;
    for ( int f = 0; f < 2; ++f )
        for ( int a = 0; a < 3; ++a )
        {
            e.vel[f][a].scale = 0.0f;
            KiwiEfx_FlatCurve( e.vel[f][a].curve[0], 1, 0.0f );
            KiwiEfx_FlatCurve( e.vel[f][a].curve[1], 1, 0.0f );
        }
    // Rise along the effect's forward axis: 60 units/s, some spread.
    e.vel[0][0].scale = 120.0f;
    KiwiEfx_FlatCurve( e.vel[0][0].curve[0], 1, 0.5f );
    KiwiEfx_FlatCurve( e.vel[0][0].curve[1], 1, 0.25f );
    e.rot.scale = 0.0f;
    KiwiEfx_FlatCurve( e.rot.curve[0], 1, 0.0f );
    KiwiEfx_FlatCurve( e.rot.curve[1], 1, 0.0f );
    e.size[0].scale = 32.0f;
    KiwiEfx_FlatCurve( e.size[0].curve[0], 1, 0.5f );
    e.size[0].curve[0].keys[3] = 1.0f;
    KiwiEfx_FlatCurve( e.size[0].curve[1], 1, 1.0f );
    e.size[1] = e.size[0];
    e.scale.scale = 1.0f;
    KiwiEfx_FlatCurve( e.scale.curve[0], 1, 1.0f );
    KiwiEfx_FlatCurve( e.scale.curve[1], 1, 1.0f );
    e.color.scale = 1.0f;
    KiwiEfx_FlatCurve( e.color.curve[0], 3, 1.0f );
    KiwiEfx_FlatCurve( e.color.curve[1], 3, 1.0f );
    e.alpha.scale = 1.0f;
    KiwiEfx_FlatCurve( e.alpha.curve[0], 1, 1.0f );
    e.alpha.curve[0].keys[3] = 0.0f;            // fade out over life
    KiwiEfx_FlatCurve( e.alpha.curve[1], 1, 1.0f );
    e.sortOrder = 5;
    if ( type == KEFX_BILLBOARD || type == KEFX_ORIENTED || type == KEFX_TAIL || type == KEFX_CLOUD
      || type == KEFX_TRAIL || type == KEFX_DECAL )
        e.visuals.push_back( "gfx_smk_light" );
}

float KiwiEfx_Sample( const kefxCurve_t &c, int dims, float t, int dim )
{
    const int stride = dims + 1;
    const int n = c.Count( dims );
    if ( n <= 0 )
        return 0.0f;
    if ( n == 1 || t <= c.keys[0] )
        return c.keys[1 + dim];
    for ( int k = 1; k < n; ++k )
    {
        const float t1 = c.keys[k * stride];
        if ( t <= t1 )
        {
            const float t0 = c.keys[( k - 1 ) * stride];
            const float f = t1 > t0 ? ( t - t0 ) / ( t1 - t0 ) : 1.0f;
            const float v0 = c.keys[( k - 1 ) * stride + 1 + dim], v1 = c.keys[k * stride + 1 + dim];
            return v0 + ( v1 - v0 ) * f;
        }
    }
    return c.keys[( n - 1 ) * stride + 1 + dim];
}

void KiwiEfx_Validate( const kefxElem_t &e, std::vector<std::string> &out )
{
    if ( e.type == KEFX_NONE )
        out.push_back( "no visual type" );
    const bool needVisuals = e.type != KEFX_LIGHT && e.type != KEFX_SPOTLIGHT && e.type != KEFX_NONE;
    if ( needVisuals && e.visuals.empty() )
        out.push_back( "no visuals - the element draws nothing" );
    // Lights loop at 0 ms in the shipped effects (one light, kept alive); only particles care.
    if ( needVisuals && e.Looping() && e.loopIntervalMsec <= 0 )
        out.push_back( "looping interval is 0 ms (the game uses 1 ms)" );
    if ( !e.Looping() && e.oneShotCount.base + e.oneShotCount.amp <= 0.0f )
        out.push_back( "one-shot count is 0 - nothing spawns" );
    if ( e.lifeSpanMsec.base + e.lifeSpanMsec.amp <= 0.0f && e.type != KEFX_RUNNER && e.type != KEFX_SOUND
      && e.type != KEFX_DECAL )
        out.push_back( "life span is 0 ms" );
    if ( e.type == KEFX_TRAIL && !e.hasTrailDef )
        out.push_back( "a trail needs a trailDef cross-section" );
    if ( e.name.size() >= 48 )
        out.push_back( "name is longer than 47 characters (the game truncates it)" );
}

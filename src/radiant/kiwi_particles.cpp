#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Particles - see kiwi_particles.h.  The browser lists every raw/fx/**/*.efx; a drag into
// the camera (or a double-click) places a kiwi_fx point entity; the particles-only Object
// sub-mode picks them; every
// map save writes the two createfx scripts the MP client plays them from.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"            // camera_s (the double-click drop's centre pixel)

#include <imgui/imgui.h>
#include <imgui_internal.h>     // IsMouseDragPastThreshold (tile click vs drag, as the Models browser)

#include "kiwi_particles.h"
#include "kiwi_fxeditor.h"      // KIWI (2026-11-02): Edit opens the FX Editor
#include "kiwi_fxpreview.h"     // KIWI (2026-11-02): camera preview of effects
#include "kiwi_command.h"
#include "kiwi_construct.h"     // KiwiCon_ActivePlane / KiwiCon_RayPlane (drop fallback)
#include "kiwi_fmt.h"
#include "kiwi_lines.h"
#include "kiwi_pick.h"
#include "kiwi_ported.h"
#include "kiwi_selection.h"
#include "kiwi_str.h"
#include "kiwi_transform.h"     // KiwiXform_CommandForId (chained drops)
#include "kiwi_vec.h"
#include "kiwi_windows.h"
#include "radiant_registry.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

extern eclass_t   *g_eclass;                                                 // eclass.cpp:117
extern eclass_t   *Eclass_InitFromText( void *textPtr );                     // eclass.cpp:792
extern void        Eclass_InsertAlphabetized( eclass_t **list, eclass_t *ec ); // eclass.cpp
extern void        Undo_KiwiMarkCreated( brush_t *def );                     // undo.cpp (KIWI tail)

namespace
{
    const float KFX_PI = (float)KIWI_PI;
    const char *KFX_PROFILE = "KiwiParticles";
    const char *KFX_OWNER_MARK = "KIWI-Radiant";          // generated-file ownership line
    const int   KFX_CLIENT_MAX_DEFS = 32;                 // g_effectDefMap[32] (cg_client_side_effects_mp.cpp)
    const int   KFX_ARROW_CAP = 1024;                     // camera arrows per frame

    // ── the effect list ──────────────────────────────────────────────────────
    struct fxRow_t
    {
        std::string path;       // "fire/firelp_med_pm" (loadfx name)
        std::string leaf;       // "firelp_med_pm"
        std::string folder;     // "fire" ("" at the top of raw/fx)
        long long   bytes = 0;
    };
    std::vector<fxRow_t>     s_rows;
    std::vector<std::string> s_folders;
    bool        s_enumerated = false;

    // KIWI (2026-11-03): the tile browser is drawn by two hosts - the FX window and the FX
    // Editor - with one code path and a state each.
    struct browserHost_t
    {
        char        filter[128] = { 0 };
        std::string folder;                 // "" = all folders
        std::string picked;                 // last clicked tile
        bool        editor = false;         // a click opens the effect in the FX Editor
    };
    browserHost_t s_winHost;
    browserHost_t s_edHost;
    char        ( &s_filter )[128] = s_winHost.filter;
    std::string &s_folder = s_winHost.folder;
    std::string &s_picked = s_winHost.picked;  // the FX window's pick (pane, Replace)

    // Tooltip summaries, read from the .efx on first hover.
    struct fxInfo_t
    {
        int  elements = 0;
        int  looping  = 0;
        std::vector<std::string> names;
        std::set<std::string>    kinds;
        bool read = false;
    };
    std::map<std::string, fxInfo_t> s_info;

    // ── settings ─────────────────────────────────────────────────────────────
    bool  s_loaded   = false;
    int   s_aim      = 0;                   // 0 = world up, 1 = surface normal
    float s_delay    = -15.0f;              // retail createfx default for ambient loops

    // ── drag / drop ──────────────────────────────────────────────────────────
    bool        s_pendHave = false;
    std::string s_pendFx;
    int         s_pendX = -1, s_pendY = -1;
    bool        s_pendPlacement = false;
    float       s_pendOrigin[3] = { 0, 0, 0 };
    float       s_pendNormal[3] = { 0, 0, 1 };

    bool  s_ghostHave = false;
    std::string s_ghostFx;
    float s_ghostOrigin[3] = { 0, 0, 0 };
    float s_ghostNormal[3] = { 0, 0, 1 };

    char  s_exportStatus[256] = "";

    // ── helpers ──────────────────────────────────────────────────────────────
    void Load()
    {
        if ( s_loaded )
            return;
        s_loaded = true;
        s_aim = Radiant_ProfileGetInt( KFX_PROFILE, "Aim", 0 ) ? 1 : 0;
        const std::string d = Radiant_ProfileGetString( KFX_PROFILE, "Delay", "-15" );
        s_delay = (float)atof( d.c_str() );
        if ( !_finite( s_delay ) ) s_delay = -15.0f;
    }

    void Save()
    {
        char text[64];
        Radiant_ProfileSetInt( KFX_PROFILE, "Aim", s_aim );
        Radiant_ProfileSetString( KFX_PROFILE, "Delay", KiwiFmt_Exact( text, sizeof( text ), s_delay + 0.0f, 0 ) );
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

    bool NameLess( const fxRow_t &a, const fxRow_t &b )
    {
        return _stricmp( a.path.c_str(), b.path.c_str() ) < 0;
    }

    void Scan( const std::string &dir, const std::string &rel, int depth )
    {
        if ( depth > 8 )
            return;
        WIN32_FIND_DATAA fd;
        HANDLE h = ::FindFirstFileA( ( dir + "\\*" ).c_str(), &fd );
        if ( h == INVALID_HANDLE_VALUE )
            return;
        do
        {
            const char *n = fd.cFileName;
            if ( !strcmp( n, "." ) || !strcmp( n, ".." ) )
                continue;
            if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
            {
                Scan( dir + "\\" + n, rel.empty() ? std::string( n ) : rel + "/" + n, depth + 1 );
                continue;
            }
            const size_t len = strlen( n );
            if ( len <= 4 || _stricmp( n + len - 4, ".efx" ) )
                continue;
            fxRow_t r;
            r.leaf   = std::string( n, len - 4 );
            r.folder = rel;
            r.path   = rel.empty() ? r.leaf : rel + "/" + r.leaf;
            r.bytes  = ( (long long)fd.nFileSizeHigh << 32 ) | fd.nFileSizeLow;
            s_rows.push_back( r );
        }
        while ( ::FindNextFileA( h, &fd ) );
        ::FindClose( h );
    }

    void Enumerate()
    {
        s_rows.clear();
        s_folders.clear();
        Scan( ExeDir() + "\\raw\\fx", "", 0 );
        std::sort( s_rows.begin(), s_rows.end(), NameLess );
        std::set<std::string> folders;
        for ( size_t i = 0; i < s_rows.size(); ++i )
        {
            const std::string &f = s_rows[i].folder;
            folders.insert( f.substr( 0, f.find( '/' ) ) );    // top-level folder
        }
        s_folders.assign( folders.begin(), folders.end() );
        if ( !s_folder.empty() && !folders.count( s_folder ) )
            s_folder.clear();
        if ( !s_edHost.folder.empty() && !folders.count( s_edHost.folder ) )
            s_edHost.folder.clear();
        s_enumerated = true;
    }

    const fxInfo_t &Info( const fxRow_t &r )
    {
        fxInfo_t &info = s_info[r.path];
        if ( info.read )
            return info;
        info.read = true;
        const std::string file = ExeDir() + "\\raw\\fx\\" + r.path + ".efx";
        FILE *f = fopen( file.c_str(), "rb" );
        if ( !f )
            return info;
        char line[512];
        static const char *const kKinds[] = { "billboardSprite", "orientedSprite", "tail", "trail",
                                              "cloud", "model", "light", "spotLight", "sound", "decal",
                                              "runner" };
        while ( fgets( line, sizeof( line ), f ) )
        {
            const char *p = line;
            while ( *p == ' ' || *p == '\t' ) ++p;
            if ( !strncmp( p, "name \"", 6 ) )
            {
                ++info.elements;
                const char *e = strchr( p + 6, '"' );
                if ( e && info.names.size() < 10 )
                    info.names.push_back( std::string( p + 6, e - ( p + 6 ) ) );
            }
            else if ( !strncmp( p, "editorFlags", 11 ) && strstr( p, " looping" ) )
                ++info.looping;
            else
            {
                for ( size_t k = 0; k < sizeof( kKinds ) / sizeof( kKinds[0] ); ++k )
                {
                    const size_t n = strlen( kKinds[k] );
                    if ( !strncmp( p, kKinds[k], n ) && ( p[n] == ';' || p[n] == '\r' || p[n] == '\n' || !p[n] ) )
                        info.kinds.insert( kKinds[k] );
                }
            }
        }
        fclose( f );
        return info;
    }

    // Forward axis <-> entity angles (pitch yaw roll, Quake convention: +pitch looks down).
    void DirToAngles( const float d[3], float out[3] )
    {
        const float flat = sqrtf( d[0] * d[0] + d[1] * d[1] );
        float yaw   = ( flat > 1e-6f ) ? atan2f( d[1], d[0] ) * 180.0f / KFX_PI : 0.0f;
        float pitch = atan2f( -d[2], flat ) * 180.0f / KFX_PI;
        if ( yaw < 0.0f )   yaw += 360.0f;
        if ( pitch < 0.0f ) pitch += 360.0f;
        out[0] = floorf( pitch * 100.0f + 0.5f ) / 100.0f;
        out[1] = floorf( yaw * 100.0f + 0.5f ) / 100.0f;
        out[2] = 0.0f;
    }

    void AnglesToDir( const float a[3], float out[3] )
    {
        const float p = a[0] * KFX_PI / 180.0f, y = a[1] * KFX_PI / 180.0f;
        out[0] = cosf( p ) * cosf( y );
        out[1] = cosf( p ) * sinf( y );
        out[2] = -sinf( p );
    }

    eclass_t *FxEclass()
    {
        for ( eclass_t *e = g_eclass; e; e = e->next )
            if ( e->name && !_stricmp( e->name, KIWI_FX_CLASSNAME ) )
                return e;
        return nullptr;
    }

    entity_s_def *NodeDef( const selbrush_t *b )
    {
        if ( !b || !b->owner || b->owner == world_entity || !b->owner->def )
            return nullptr;
        return (entity_s_def *)b->owner->def;
    }

    bool IsFxDef( const entity_s_def *def, const eclass_t *ec )
    {
        if ( !def || !def->eclass )
            return false;
        if ( ec )
            return def->eclass == ec;
        return def->eclass->name && !_stricmp( def->eclass->name, KIWI_FX_CLASSNAME );
    }

    // The particle's angles: "angles", else "angle" (yaw), else pointing up.
    void FxAngles( const entity_s_def *def, float out[3] )
    {
        out[0] = 270.0f; out[1] = 0.0f; out[2] = 0.0f;
        const char *a = ValueForKey2( def, "angles" );
        if ( a && a[0] && sscanf( a, "%f %f %f", &out[0], &out[1], &out[2] ) >= 2 )
            return;
        const char *y = ValueForKey2( def, "angle" );
        if ( y && y[0] )
        {
            out[0] = 0.0f;
            out[1] = (float)atof( y );
        }
    }

    // The "origin" key (what the .map carries), else the entity's cached origin.  A point
    // entity's live origin wins: its key is only rewritten on copy, so an export after a
    // move wrote where the effect WAS.
    void FxOrigin( const entity_s_def *def, float out[3] )
    {
        memcpy( out, def->origin, sizeof( float ) * 3 );
        if ( def->eclass && def->eclass->fixedsize )
            return;
        const char *o = ValueForKey2( def, "origin" );
        float v[3];
        if ( o && o[0] && sscanf( o, "%f %f %f", &v[0], &v[1], &v[2] ) == 3 )
            memcpy( out, v, sizeof( v ) );
    }

    float FxDelay( const entity_s_def *def )
    {
        const char *d = ValueForKey2( def, "delay" );
        return ( d && d[0] ) ? (float)atof( d ) : -15.0f;
    }

    // ── placement ────────────────────────────────────────────────────────────
    // Surface under the camera pixel: the camera's solid trace (brushes, patches, model
    // meshes), else the working plane, else 256 units down the ray.
    bool ResolvePlacement( int imgX, int imgY, float origin[3], float normal[3] )
    {
        ray_t ray;
        if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
            return false;
        float dist = 0.0f, n[3] = { 0.0f, 0.0f, 1.0f };
        if ( Pick_TraceSolid( ray.origin, ray.dir, &dist, n ) )
        {
            for ( int k = 0; k < 3; ++k )
                origin[k] = ray.origin[k] + ray.dir[k] * dist;
            if ( n[0] * ray.dir[0] + n[1] * ray.dir[1] + n[2] * ray.dir[2] > 0.0f )
                for ( int k = 0; k < 3; ++k ) n[k] = -n[k];
            memcpy( normal, n, sizeof( n ) );
            return true;
        }
        float hit[3];
        if ( KiwiCon_RayPlane( KiwiCon_ActivePlane(), ray, hit ) )
        {
            memcpy( origin, hit, sizeof( hit ) );
            memcpy( normal, KiwiCon_ActivePlane().normal, sizeof( float ) * 3 );
            return true;
        }
        for ( int k = 0; k < 3; ++k )
            origin[k] = ray.origin[k] + ray.dir[k] * 256.0f;
        normal[0] = normal[1] = 0.0f;
        normal[2] = 1.0f;
        return true;
    }

    // The native CreateEntityBrush (0x466290) placeholder sequence (kiwi_modelbrowser.cpp DropPlaceholder).
    bool DropPlaceholder( eclass_t *ec, const float origin[3] )
    {
        float mins[3], maxs[3];
        for ( int i = 0; i < 3; ++i )
        {
            mins[i] = origin[i] + ec->mins[i];
            maxs[i] = origin[i] + ec->maxs[i];
            if ( maxs[i] - mins[i] < 1.0f )
                maxs[i] = mins[i] + 1.0f;
        }
        Ed_EnsureCurrentMaterial_Kiwi();
        brush_t *def = Brush_Alloc( g_qeglobals.random_texture_stuff, nullptr );
        if ( !def )
            return false;
        Brush_Create( mins, maxs, def, nullptr );
        Brush_BuildWindings( def, 1 );
        KiwiExtrude_LandDef( def );
        // Created inside the open record: Undo must not restore it as pre-existing
        // (the Models-browser lesson, kiwi_modelbrowser.cpp DropPlaceholder).
        Undo_KiwiMarkCreated( def );
        return true;
    }

    bool Place( const char *fx, const float origin[3], const float normal[3] )
    {
        if ( !fx || !fx[0] )
            return false;
        if ( KiwiEditorCommand *live = KiwiCmd_Active() )
        {
            if ( live->PreemptIdle() )
                KiwiCmd_Cancel();
            else if ( live == KiwiXform_CommandForId( KIWI_CMD_MOVE )
                   || live == KiwiXform_CommandForId( KIWI_CMD_ROTATE )
                   || live == KiwiXform_CommandForId( KIWI_CMD_SCALE ) )
                KiwiCmd_Commit();           // the previous drop's paused Move (Models' chained-drop rule)
            else
            {
                Sys_Printf( "Particles: finish or cancel \"%s\" before placing an effect.\n", live->Name() );
                return false;
            }
        }
        eclass_t *ec = FxEclass();
        if ( !ec )
        {
            KiwiParticles_RegisterEclass();
            ec = FxEclass();
        }
        if ( !ec || !ec->fixedsize )
        {
            Sys_Printf( "Particles: the %s entity class is not available.\n", KIWI_FX_CLASSNAME );
            return false;
        }

        float angles[3];
        if ( s_aim == 1 && normal )
            DirToAngles( normal, angles );
        else
        {
            angles[0] = 270.0f; angles[1] = 0.0f; angles[2] = 0.0f;
        }

        Select_Deselect( 1 );
        Undo_ClearRedo();
        Undo_GeneralStart( "place particle" );
        if ( !DropPlaceholder( ec, origin ) )
        {
            Undo_End();
            Sys_Printf( "Particles: brush allocation failed.\n" );
            return false;
        }
        CreateEntityFromName( KIWI_FX_CLASSNAME );

        entity_s_def *created = nullptr;
        selbrush_t *sel = selected_brushes.next;
        if ( sel && sel != &selected_brushes && sel->next == &selected_brushes )
        {
            entity_s_def *cand = NodeDef( sel );
            if ( cand && cand->eclass == ec )
                created = cand;
        }
        if ( created )
        {
            char a[96], p[32], y[32], r[32], d[48];
            _snprintf( a, sizeof( a ), "%s %s %s", KiwiFmt_Num( p, sizeof( p ), angles[0], 2 ),
                       KiwiFmt_Num( y, sizeof( y ), angles[1], 2 ), KiwiFmt_Num( r, sizeof( r ), angles[2], 2 ) );
            a[sizeof( a ) - 1] = '\0';
            SetKeyValue( created, "fx", fx );
            SetKeyValue( created, "angles", a );   // authored from a normal: 0.01 deg is enough
            SetKeyValue( created, "delay", KiwiFmt_Exact( d, sizeof( d ), s_delay + 0.0f, 0 ) );
        }
        Undo_End();
        if ( !created )
        {
            Sys_Printf( "Particles: entity creation failed; nothing placed.\n" );
            return false;
        }
        g_nUpdateBits = -1;
        char x[32], yy[32], z[32];
        Sys_Printf( "Placed particle '%s' at %s %s %s.\n", fx, KiwiFmt_Num( x, sizeof( x ), origin[0] ),
                    KiwiFmt_Num( yy, sizeof( yy ), origin[1] ), KiwiFmt_Num( z, sizeof( z ), origin[2] ) );
        KiwiCmd_AfterPaste();
        return true;
    }

    bool QueueDrop( const char *fx, int imgX, int imgY, const float *origin, const float *normal )
    {
        if ( !fx || !fx[0] )
            return false;
        s_pendFx = fx;
        s_pendX  = imgX;
        s_pendY  = imgY;
        s_pendPlacement = origin != nullptr;
        if ( origin )
        {
            memcpy( s_pendOrigin, origin, sizeof( s_pendOrigin ) );
            memcpy( s_pendNormal, normal, sizeof( s_pendNormal ) );
        }
        s_pendHave = true;
        ::PostMessageA( g_qeglobals.d_hwndMain, WM_COMMAND, (WPARAM)(unsigned int)KIWI_CMD_PARTICLE_DROP, 0 );
        return true;
    }

    // ── selection helpers ────────────────────────────────────────────────────
    void SelectedFx( std::vector<entity_s_def *> &out )
    {
        const eclass_t *ec = FxEclass();
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            entity_s_def *def = NodeDef( b );
            if ( !IsFxDef( def, ec ) || ( b->owner && b->owner->prefab ) )
                continue;
            if ( std::find( out.begin(), out.end(), def ) == out.end() )
                out.push_back( def );
        }
    }

    // One undo record over every selected particle.
    void SetOnSelected( const char *op, const char *key, const char *value )
    {
        std::vector<entity_s_def *> fx;
        SelectedFx( fx );
        if ( fx.empty() )
            return;
        Undo_ClearRedo();
        Undo_GeneralStart( op );
        for ( size_t i = 0; i < fx.size(); ++i )
        {
            Undo_AddEntity_W( (entity_s *)fx[i] );
            SetKeyValue( fx[i], key, value );
        }
        Undo_End();
        g_nUpdateBits = -1;
    }

    // ── export ───────────────────────────────────────────────────────────────
    std::string Sanitize( const std::string &s )
    {
        std::string o;
        for ( size_t i = 0; i < s.size() && o.size() < 60; ++i )
        {
            const char c = (char)tolower( (unsigned char)s[i] );
            o += ( ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_' ) ? c : '_';
        }
        return o.empty() ? std::string( "fx" ) : o;
    }

    // 1 = ours, 0 = absent, -1 = somebody else's (hand-written)
    int FileOwnership( const std::string &path )
    {
        FILE *f = fopen( path.c_str(), "rb" );
        if ( !f )
            return 0;
        char buf[1024];
        const size_t n = fread( buf, 1, sizeof( buf ) - 1, f );
        fclose( f );
        buf[n] = '\0';
        return strstr( buf, KFX_OWNER_MARK ) ? 1 : -1;
    }

    bool WriteText( const std::string &path, const std::string &text )
    {
        const std::string tmp = path + ".kiwitmp";
        FILE *f = fopen( tmp.c_str(), "wb" );
        if ( !f )
            return false;
        const bool ok = fwrite( text.data(), 1, text.size(), f ) == text.size();
        if ( fclose( f ) != 0 || !ok )
        {
            ::DeleteFileA( tmp.c_str() );
            return false;
        }
        if ( !::MoveFileExA( tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING ) )
        {
            ::DeleteFileA( tmp.c_str() );
            return false;
        }
        return true;
    }

    struct fxEnt_t
    {
        std::string fx;
        float origin[3];
        float angles[3];
        float delay;
    };

    std::string MapBaseName( const char *mapPath )
    {
        if ( !mapPath || !mapPath[0] )
            return std::string();
        const char *s = mapPath + strlen( mapPath );
        while ( s > mapPath && s[-1] != '\\' && s[-1] != '/' )
            --s;
        std::string name( s );
        const size_t dot = name.rfind( '.' );
        if ( dot != std::string::npos )
            name.erase( dot );
        for ( size_t i = 0; i < name.size(); ++i )
            name[i] = (char)tolower( (unsigned char)name[i] );
        return name;
    }
}

// ── eclass ─────────────────────────────────────────────────────────────────────
void KiwiParticles_RegisterEclass()
{
    if ( FxEclass() )
        return;
    // Eclass_InitFromText reads from "/*QUAKED" + 9; the comment runs to "*/".
    static char text[] =
        "/*QUAKED kiwi_fx (1 0.55 0.1) (-6 -6 -6) (6 6 6)\n"
        "KIWI particle effect, placed from the Particles tab.\n"
        "\"fx\" effect path under raw/fx without .efx (fire/firelp_med_pm).\n"
        "\"angles\" pitch yaw roll of the effect's forward axis; 270 0 0 = up.\n"
        "\"delay\" start time in seconds; negative = already running at map start.\n"
        "Saved into maps/createfx/<map>_fx.gsc on every map save; the map compiler drops the entity.\n"
        "*/";
    eclass_t *ec = Eclass_InitFromText( text );
    if ( ec )
        Eclass_InsertAlphabetized( &g_eclass, ec );
}

bool KiwiParticles_IsParticleNode( const selbrush_t *b )
{
    if ( !b || ( b->owner && b->owner->prefab ) )
        return false;
    return IsFxDef( NodeDef( b ), nullptr );
}

// KIWI (2026-11-02): the FX editor / camera preview hooks - see kiwi_particles.h.
void KiwiParticles_Collect( std::vector<kiwiFxEnt_t> &out, bool selectedOnly )
{
    out.clear();
    const eclass_t *ec = FxEclass();
    if ( !ec )
        return;
    std::set<const entity_s_def *> seen;
    for ( int pass = selectedOnly ? 1 : 0; pass < 2; ++pass )
    {
        selbrush_t *head = pass == 0 ? &active_brushes : &selected_brushes;
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            entity_s_def *def = NodeDef( b );
            if ( !IsFxDef( def, ec ) || ( b->owner && b->owner->prefab ) || !Pick_BrushPickable( b )
              || !seen.insert( def ).second )
                continue;
            kiwiFxEnt_t e;
            e.key = def;
            const char *fx = ValueForKey2( def, "fx" );
            e.fx = fx ? fx : "";
            FxOrigin( def, e.origin );
            FxAngles( def, e.angles );
            e.selected = pass == 1;
            out.push_back( e );
        }
    }
}

std::string KiwiParticles_SelectedEffect()
{
    std::vector<entity_s_def *> fx;
    SelectedFx( fx );
    std::string path;
    for ( size_t i = 0; i < fx.size(); ++i )
    {
        const char *p = ValueForKey2( fx[i], "fx" );
        const std::string s = p ? p : "";
        if ( i == 0 )
            path = s;
        else if ( _stricmp( s.c_str(), path.c_str() ) )
            return std::string();
    }
    return path;
}

bool KiwiParticles_SetSelectedEffect( const char *fx )
{
    std::vector<entity_s_def *> sel;
    SelectedFx( sel );
    if ( sel.empty() || !fx || !fx[0] )
        return false;
    SetOnSelected( "set particle effect", "fx", fx );
    return true;
}

bool KiwiParticles_BrowserSet( const char *what, const char *value )
{
    const std::string v = value ? value : "";
    // The FX Editor's browser: editor_pick = a tile click there (opens the effect).
    if ( !_stricmp( what, "editor_pick" ) )
    {
        s_edHost.picked = v;
        return KiwiFxEditor_Open( v.c_str() );
    }
    if ( !_stricmp( what, "editor_search" ) )
    {
        _snprintf( s_edHost.filter, sizeof( s_edHost.filter ), "%s", v.c_str() );
        s_edHost.filter[sizeof( s_edHost.filter ) - 1] = '\0';
        return true;
    }
    if ( !_stricmp( what, "editor_folder" ) )
    {
        s_edHost.folder = v;
        return true;
    }
    if ( !_stricmp( what, "pick" ) )
    {
        s_picked = v;
        KiwiFxPreview_PaneRestart();
        return true;
    }
    if ( !_stricmp( what, "search" ) )
    {
        _snprintf( s_filter, sizeof( s_filter ), "%s", v.c_str() );
        s_filter[sizeof( s_filter ) - 1] = '\0';
        return true;
    }
    if ( !_stricmp( what, "folder" ) )
    {
        s_folder = v;
        return true;
    }
    return false;
}

void KiwiParticles_Rescan()
{
    s_info.clear();
    s_enumerated = false;
}

// ── particles-only picking ─────────────────────────────────────────────────────
bool KiwiParticles_PickAt( int imgX, int imgY, selbrush_t **outNode, float *outDist )
{
    if ( outNode ) *outNode = nullptr;
    ray_t ray;
    if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
        return false;
    const eclass_t *ec = FxEclass();
    if ( !ec )
        return false;

    selbrush_t *best = nullptr, *near_ = nullptr;
    float bestT = FLT_MAX, nearPx = 10.0f, nearT = FLT_MAX;
    for ( int pass = 0; pass < 2; ++pass )
    {
        selbrush_t *head = pass == 0 ? &active_brushes : &selected_brushes;
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( ( b->owner && b->owner->prefab ) || !IsFxDef( NodeDef( b ), ec ) || !b->def
              || !Pick_BrushPickable( b ) )
                continue;
            // Slab test against the box, grown 2 units so a small box is easy to hit.
            float t0 = 0.0f, t1 = FLT_MAX;
            bool  hit = true;
            for ( int k = 0; k < 3 && hit; ++k )
            {
                const float lo = b->def->mins[k] - 2.0f, hi = b->def->maxs[k] + 2.0f;
                if ( fabsf( ray.dir[k] ) < 1e-8f )
                {
                    hit = ray.origin[k] >= lo && ray.origin[k] <= hi;
                    continue;
                }
                float a = ( lo - ray.origin[k] ) / ray.dir[k], c = ( hi - ray.origin[k] ) / ray.dir[k];
                if ( a > c ) { const float s = a; a = c; c = s; }
                if ( a > t0 ) t0 = a;
                if ( c < t1 ) t1 = c;
                hit = t0 <= t1;
            }
            if ( hit && t0 < bestT )
            {
                bestT = t0;
                best  = b;
            }
            if ( best )
                continue;
            // Screen fallback: a distant box is a few pixels wide.
            float c[3], sx, sy;
            for ( int k = 0; k < 3; ++k )
                c[k] = ( b->def->mins[k] + b->def->maxs[k] ) * 0.5f;
            if ( Pick_WorldToImage( c, &sx, &sy ) )
            {
                const float d = sqrtf( ( sx - imgX ) * ( sx - imgX ) + ( sy - imgY ) * ( sy - imgY ) );
                const float t = ( c[0] - ray.origin[0] ) * ray.dir[0] + ( c[1] - ray.origin[1] ) * ray.dir[1]
                              + ( c[2] - ray.origin[2] ) * ray.dir[2];
                if ( d < nearPx || ( d == nearPx && t < nearT ) )
                {
                    nearPx = d;
                    nearT  = t;
                    near_  = b;
                }
            }
        }
    }
    selbrush_t *pick = best ? best : near_;
    if ( !pick )
        return false;
    if ( outNode ) *outNode = pick;
    if ( outDist ) *outDist = best ? bestT : nearT;
    return true;
}

// ── export ─────────────────────────────────────────────────────────────────────
void KiwiParticles_ExportForMap( const char *mapPath, bool fromSave )
{
    const std::string name = MapBaseName( mapPath );
    if ( name.empty() || name.find( "autosave" ) != std::string::npos )
        return;
    const eclass_t *ec = FxEclass();

    std::vector<fxEnt_t> ents;
    for ( entity_s *e = entities.next; e && e != &entities; e = e->next )
    {
        entity_s_def *def = (entity_s_def *)e;
        if ( !IsFxDef( def, ec ) )
            continue;
        const char *fx = ValueForKey2( def, "fx" );
        if ( !fx || !fx[0] )
            continue;
        fxEnt_t r;
        r.fx = fx;
        FxOrigin( def, r.origin );
        FxAngles( def, r.angles );
        r.delay = FxDelay( def );
        ents.push_back( r );
    }

    const std::string root = ExeDir() + "\\raw\\maps";
    const std::string mapFile = root + "\\mp\\" + name + "_fx.gsc";
    const std::string fxFile  = root + "\\createfx\\" + name + "_fx.gsc";
    const int ownMap = FileOwnership( mapFile ), ownFx = FileOwnership( fxFile );
    if ( ents.empty() && ownMap != 1 && ownFx != 1 )
    {
        if ( !fromSave )
            _snprintf( s_exportStatus, sizeof( s_exportStatus ), "No particles in %s - nothing written.", name.c_str() );
        return;                             // never had particles: stay silent, write nothing
    }

    // fxid per effect: the leaf name, the whole path where two folders share a leaf.
    std::map<std::string, std::string> idOf;
    {
        std::map<std::string, std::set<std::string> > byLeaf;
        for ( size_t i = 0; i < ents.size(); ++i )
        {
            const size_t sl = ents[i].fx.rfind( '/' );
            byLeaf[Sanitize( sl == std::string::npos ? ents[i].fx : ents[i].fx.substr( sl + 1 ) )].insert( ents[i].fx );
        }
        for ( std::map<std::string, std::set<std::string> >::iterator it = byLeaf.begin(); it != byLeaf.end(); ++it )
            for ( std::set<std::string>::iterator p = it->second.begin(); p != it->second.end(); ++p )
                idOf[*p] = it->second.size() == 1 ? it->first : Sanitize( *p );
    }

    char num[3][32];
    std::string mapText =
        "// " + std::string( KFX_OWNER_MARK ) + " particles: generated from " + name
        + ".map on every save - edits here are overwritten.\n"
        "// Remove this comment block to take the file over by hand (the editor then leaves it alone).\n"
        "main()\n{\n";
    for ( std::map<std::string, std::string>::iterator it = idOf.begin(); it != idOf.end(); ++it )
        mapText += "\tlevel._effect[ \"" + it->second + "\" ] = loadfx( \"" + it->first + "\" );\n";
    mapText += "}\n";

    char header[256];
    _snprintf( header, sizeof( header ), "// CreateFX entities placed: %i (%s particles from %s.map, rewritten on save)\n",
               (int)ents.size(), KFX_OWNER_MARK, name.c_str() );
    header[sizeof( header ) - 1] = '\0';
    // The layout is what cg_client_side_effects_mp.cpp CG_ParseClientEffects expects,
    // line for line: the first line exact, one "//" line after "{", then the blocks.
    std::string fxText = "//_createfx generated. Do not touch!!\nmain()\n{\n";
    fxText += header;
    for ( size_t i = 0; i < ents.size(); ++i )
    {
        const fxEnt_t &r = ents[i];
        const std::string &id = idOf[r.fx];
        fxText += "\tent = maps\\mp\\_utility::createOneshotEffect( \"" + id + "\" );\n";
        // Exact: 3 decimals moved every effect off its entity (100.12345 -> 100.123).
        fxText += std::string( "\tent.v[ \"origin\" ] = ( " ) + KiwiFmt_Exact( num[0], 32, r.origin[0] ) + ", "
                + KiwiFmt_Exact( num[1], 32, r.origin[1] ) + ", " + KiwiFmt_Exact( num[2], 32, r.origin[2] ) + " );\n";
        fxText += std::string( "\tent.v[ \"angles\" ] = ( " ) + KiwiFmt_Exact( num[0], 32, r.angles[0] ) + ", "
                + KiwiFmt_Exact( num[1], 32, r.angles[1] ) + ", " + KiwiFmt_Exact( num[2], 32, r.angles[2] ) + " );\n";
        fxText += "\tent.v[ \"fxid\" ] = \"" + id + "\";\n";
        fxText += std::string( "\tent.v[ \"delay\" ] = " ) + KiwiFmt_Exact( num[0], 32, r.delay ) + ";\n\n";
    }
    fxText += "}\n";

    ::CreateDirectoryA( root.c_str(), nullptr );
    ::CreateDirectoryA( ( root + "\\mp" ).c_str(), nullptr );
    ::CreateDirectoryA( ( root + "\\createfx" ).c_str(), nullptr );

    bool mapOk = false, fxOk = false;
    if ( ownMap == -1 )
    {
        Sys_Printf( "Particles: maps/mp/%s_fx.gsc is hand-written, so it was left alone.  For the "
                    "particles to load, it needs these lines inside main():\n", name.c_str() );
        for ( std::map<std::string, std::string>::iterator it = idOf.begin(); it != idOf.end(); ++it )
            Sys_Printf( "    level._effect[ \"%s\" ] = loadfx( \"%s\" );\n", it->second.c_str(), it->first.c_str() );
    }
    else if ( !( mapOk = WriteText( mapFile, mapText ) ) )
        Sys_Printf( "ERROR: Particles: could not write %s.\n", mapFile.c_str() );
    if ( ownFx == -1 )
        Sys_Printf( "Particles: maps/createfx/%s_fx.gsc is hand-written (no \"%s\" line), so the %i placed "
                    "particle(s) were NOT written to it.\n", name.c_str(), KFX_OWNER_MARK, (int)ents.size() );
    else if ( !( fxOk = WriteText( fxFile, fxText ) ) )
        Sys_Printf( "ERROR: Particles: could not write %s.\n", fxFile.c_str() );

    if ( (int)idOf.size() > KFX_CLIENT_MAX_DEFS )
        Sys_Printf( "WARNING: Particles: %s uses %i different effects; the client loads only the first %i "
                    "(MAX_CLIENT_EFFECT_DEFS in cg_client_side_effects_mp.cpp).\n",
                    name.c_str(), (int)idOf.size(), KFX_CLIENT_MAX_DEFS );

    SYSTEMTIME st;
    ::GetLocalTime( &st );
    _snprintf( s_exportStatus, sizeof( s_exportStatus ), "%02i:%02i  %s: %i particle(s), %i effect(s)%s%s%s",
               st.wHour, st.wMinute, name.c_str(), (int)ents.size(), (int)idOf.size(),
               ( ownMap == -1 || ownFx == -1 ) ? " - a hand-written script was left alone (see console)" : "",
               ( ( ownMap != -1 && !mapOk ) || ( ownFx != -1 && !fxOk ) ) ? " - WRITE FAILED" : "",
               (int)idOf.size() > KFX_CLIENT_MAX_DEFS ? " - over the 32-effect client limit" : "" );
    s_exportStatus[sizeof( s_exportStatus ) - 1] = '\0';
    if ( !fromSave || !ents.empty() )
        Sys_Printf( "Particles: %s\n", s_exportStatus );
}

bool KiwiParticles_PlaceAt( const char *fx, const float origin[3], const float normal[3] )
{
    Load();
    return Place( fx, origin, normal );
}

// ── dock window ────────────────────────────────────────────────────────────────
namespace
{
    int SelectedCount()
    {
        std::vector<entity_s_def *> fx;
        SelectedFx( fx );
        return (int)fx.size();
    }

    // KIWI (2026-11-02): the FX preview's test instance where a drop at the view centre would land.
    void PreviewInFront( const char *fx )
    {
        camera_s *cam = Ed_Camera();
        float origin[3], normal[3] = { 0.0f, 0.0f, 1.0f };
        if ( !cam || !ResolvePlacement( cam->width / 2, cam->height / 2, origin, normal ) )
            return;
        const float up[3] = { 0.0f, 0.0f, 1.0f };
        KiwiFxPreview_PlayTest( fx, origin, s_aim == 1 ? normal : up );
    }

    void DrawSelectedSection()
    {
        std::vector<entity_s_def *> fx;
        SelectedFx( fx );
        if ( fx.empty() )
            return;
        ImGui::SeparatorText( "Selected" );
        if ( fx.size() == 1 )
        {
            const char *path = ValueForKey2( fx[0], "fx" );
            ImGui::Text( "%s", path && path[0] ? path : "(no effect set)" );
        }
        else
            ImGui::Text( "%i particles", (int)fx.size() );
        // KIWI (2026-11-02): open the selection's effect in the FX Editor.
        const std::string shared = KiwiParticles_SelectedEffect();
        if ( !shared.empty() )
        {
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Edit" ) )
                KiwiFxEditor_Open( shared.c_str() );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Open %s in the FX Editor.", shared.c_str() );
        }

        ImGui::BeginDisabled( s_picked.empty() );
        if ( ImGui::Button( "Replace effect with picked" ) )
            SetOnSelected( "replace particle effect", "fx", s_picked.c_str() );
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( s_picked.empty() ? "Click an effect in the list first."
                                                : "Every selected particle plays '%s' instead.  Undoable.",
                               s_picked.c_str() );

        // Delay and aim, applied to all selected on release (one undo record each).
        static float s_editDelay = -15.0f;
        static const void *s_editFor = nullptr;
        if ( s_editFor != fx[0] || !ImGui::IsAnyItemActive() )
        {
            s_editFor   = fx[0];
            s_editDelay = FxDelay( fx[0] );
        }
        ImGui::SetNextItemWidth( 90.0f );
        ImGui::InputFloat( "Start delay (s)", &s_editDelay, 1.0f, 5.0f, "%.2f" );
        if ( ImGui::IsItemDeactivatedAfterEdit() )
        {
            char d[48];
            SetOnSelected( "particle delay", "delay", KiwiFmt_Exact( d, sizeof( d ), s_editDelay + 0.0f, 0 ) );
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "When the effect starts, in seconds from map start.\n"
                               "Negative = already running when the map loads (retail -15)." );
        if ( ImGui::SmallButton( "Aim up" ) )
            SetOnSelected( "aim particle", "angles", "270 0 0" );
        ImGui::SameLine();
        ImGui::TextDisabled( "rotate (R) to aim; the arrow is the effect's forward axis" );
    }

    // KIWI (2026-11-02, user: "a browser like the models menu where I can easily select some
    // and drag them into the world"): tiles like the Models browser (kiwi_modelbrowser.cpp
    // DrawTile) showing a rendered snapshot of each effect (KiwiFxPreview_Thumb).
    const float KFX_TILE   = 92.0f;
    const float KFX_TILE_PAD = 6.0f;

    void DrawTile( const fxRow_t &r, int index, browserHost_t &host )
    {
        std::string &s_picked = host.picked;     // this host's pick (the window's or the editor's)
        ImGui::PushID( index );
        const float labelH = ImGui::GetTextLineHeight();
        const ImVec2 cell( KFX_TILE, KFX_TILE + labelH + 2.0f );
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton( "##fxtile", cell );
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const ImVec2 p1( p0.x + cell.x, p0.y + cell.y );
        const ImVec2 boxMax( p0.x + KFX_TILE, p0.y + KFX_TILE );
        IDirect3DTexture9 *thumb = KiwiFxPreview_Thumb( r.path.c_str(), ImGui::IsRectVisible( p0, p1 ) );
        const bool empty = KiwiFxPreview_ThumbEmpty( r.path.c_str() );
        const bool picked = !s_picked.empty() && !_stricmp( s_picked.c_str(), r.path.c_str() );

        if ( ImGui::BeginDragDropSource( ImGuiDragDropFlags_SourceNoHoldToOpenOthers ) )
        {
            ImGui::SetDragDropPayload( KFX_PAYLOAD, r.path.c_str(), r.path.size() + 1 );
            ImGui::TextUnformatted( r.path.c_str() );
            if ( thumb )
                ImGui::Image( (ImTextureID)(intptr_t)thumb, ImVec2( KFX_TILE * 0.75f, KFX_TILE * 0.75f ) );
            ImGui::TextDisabled( "drop in the 3D view" );
            ImGui::EndDragDropSource();
        }
        // A plain click (no drag travel) picks the tile: the FX window's pane plays it, the FX
        // Editor opens it.
        if ( hovered && ImGui::IsMouseReleased( ImGuiMouseButton_Left ) && !ImGui::IsMouseDragPastThreshold( ImGuiMouseButton_Left ) )
        {
            if ( host.editor )
                KiwiFxEditor_Open( r.path.c_str() );
            else if ( !picked )
                KiwiFxPreview_PaneRestart();
            s_picked = r.path;
        }
        if ( hovered && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
            QueueDrop( r.path.c_str(), -1, -1, nullptr, nullptr );

        ImDrawList *dl = ImGui::GetWindowDrawList();
        if ( hovered || active )
            dl->AddRectFilled( p0, p1, ImGui::GetColorU32( ImGuiCol_FrameBgHovered ), 3.0f );
        if ( thumb )
            dl->AddImage( (ImTextureID)(intptr_t)thumb, ImVec2( p0.x + 2.0f, p0.y + 2.0f ), ImVec2( boxMax.x - 2.0f, boxMax.y - 2.0f ) );
        else
        {
            dl->AddRectFilled( ImVec2( p0.x + 2.0f, p0.y + 2.0f ), ImVec2( boxMax.x - 2.0f, boxMax.y - 2.0f ), IM_COL32( 23, 26, 31, 255 ), 2.0f );
            // Not drawable in the preview (models / lights / sounds only) or still pending.
            const char *tag = empty ? "no preview" : "...";
            const ImVec2 ts = ImGui::CalcTextSize( tag );
            dl->AddText( ImVec2( ( p0.x + boxMax.x - ts.x ) * 0.5f, ( p0.y + boxMax.y - ts.y ) * 0.5f ),
                         ImGui::GetColorU32( ImGuiCol_TextDisabled ), tag );
        }
        if ( picked )
            dl->AddRect( p0, p1, ImGui::GetColorU32( ImGuiCol_ButtonActive ), 3.0f, 0, 2.0f );
        dl->PushClipRect( ImVec2( p0.x, boxMax.y ), p1, true );
        dl->AddText( ImVec2( p0.x + 2.0f, boxMax.y + 1.0f ),
                     ImGui::GetColorU32( hovered || picked ? ImGuiCol_Text : ImGuiCol_TextDisabled ), r.leaf.c_str() );
        dl->PopClipRect();

        // KIWI (2026-11-02): play / edit / place from the list.
        if ( ImGui::BeginPopupContextItem( "##fxrow" ) )
        {
            s_picked = r.path;
            ImGui::TextDisabled( "%s", r.path.c_str() );
            if ( ImGui::MenuItem( "Preview in front of camera" ) )
                PreviewInFront( r.path.c_str() );
            if ( ImGui::MenuItem( "Edit in FX Editor" ) )
                KiwiFxEditor_Open( r.path.c_str() );
            if ( ImGui::MenuItem( "Place at view centre" ) )
                QueueDrop( r.path.c_str(), -1, -1, nullptr, nullptr );
            ImGui::BeginDisabled( KiwiParticles_SelectedEffect().empty() && !SelectedCount() );
            if ( ImGui::MenuItem( "Use for selected FX" ) )
                SetOnSelected( "replace particle effect", "fx", r.path.c_str() );
            ImGui::EndDisabled();
            if ( ImGui::MenuItem( "Copy path" ) )
                ImGui::SetClipboardText( r.path.c_str() );
            ImGui::EndPopup();
        }

        if ( hovered && !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
        {
            const fxInfo_t &info = Info( r );
            ImGui::BeginTooltip();
            if ( thumb )
                ImGui::Image( (ImTextureID)(intptr_t)thumb, ImVec2( 160.0f, 160.0f ) );
            ImGui::TextUnformatted( r.path.c_str() );
            ImGui::TextDisabled( "raw/fx/%s.efx  (%.1f KB)", r.path.c_str(), (double)r.bytes / 1024.0 );
            if ( info.elements )
            {
                ImGui::Text( "%i element%s, %s", info.elements, info.elements == 1 ? "" : "s",
                             info.looping == 0 ? "one-shot (plays once, then stops)"
                           : info.looping == info.elements ? "looping" : "partly looping" );
                std::string kinds;
                for ( std::set<std::string>::const_iterator k = info.kinds.begin(); k != info.kinds.end(); ++k )
                    kinds += ( kinds.empty() ? "" : ", " ) + *k;
                if ( !kinds.empty() )
                    ImGui::TextDisabled( "%s", kinds.c_str() );
                std::string names;
                for ( size_t k = 0; k < info.names.size(); ++k )
                    names += ( k ? ", " : "" ) + info.names[k];
                if ( !names.empty() )
                    ImGui::TextDisabled( "%s%s", names.c_str(), info.elements > (int)info.names.size() ? ", ..." : "" );
            }
            ImGui::TextDisabled( "drag into the 3D view or double-click to place" );
            ImGui::EndTooltip();
        }
        ImGui::PopID();
    }

    // Search box + folder combo of one browser host (no trailing SameLine).
    void DrawBrowserFilters( browserHost_t &host )
    {
        ImGui::PushID( host.editor ? "edbrowse" : "winbrowse" );
        ImGui::SetNextItemWidth( 140.0f );
        ImGui::InputTextWithHint( "##fxsearch", "search", host.filter, sizeof( host.filter ) );
        if ( host.filter[0] )
        {
            ImGui::SameLine();
            if ( ImGui::SmallButton( "x##fxsearchclear" ) )
                host.filter[0] = '\0';
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 120.0f );
        if ( ImGui::BeginCombo( "##fxfolder", host.folder.empty() ? "All folders" : host.folder.c_str() ) )
        {
            if ( ImGui::Selectable( "All folders", host.folder.empty() ) )
                host.folder.clear();
            for ( size_t i = 0; i < s_folders.size(); ++i )
            {
                const char *f = s_folders[i].empty() ? "(top level)" : s_folders[i].c_str();
                if ( ImGui::Selectable( f, !host.folder.empty() && host.folder == s_folders[i] ) )
                    host.folder = s_folders[i];
            }
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }

    // The effect count and the tile grid of one host, `height` tall (ImGui child sizing:
    // negative = the remaining height minus that much).
    void DrawBrowserGrid( browserHost_t &host, float height )
    {
        std::vector<int> visible;
        visible.reserve( s_rows.size() );
        for ( size_t i = 0; i < s_rows.size(); ++i )
        {
            const fxRow_t &r = s_rows[i];
            if ( !host.folder.empty() )
            {
                const std::string top = r.folder.substr( 0, r.folder.find( '/' ) );
                if ( _stricmp( top.c_str(), host.folder.c_str() ) )
                    continue;
            }
            if ( host.filter[0] && !KiwiStr_ContainsNoCase( r.path.c_str(), host.filter ) )
                continue;
            visible.push_back( (int)i );
        }
        ImGui::TextDisabled( "%i of %i effects", (int)visible.size(), (int)s_rows.size() );

        ImGui::BeginChild( host.editor ? "##fxedlist" : "##fxlist", ImVec2( 0.0f, height ), ImGuiChildFlags_Borders );
        {
            // Rows of tiles; the clipper keeps only the visible rows (and so only their
            // thumbnail requests) alive.
            const float avail = ImGui::GetContentRegionAvail().x;
            const int cols = ( std::max )( 1, (int)( ( avail + KFX_TILE_PAD ) / ( KFX_TILE + KFX_TILE_PAD ) ) );
            const int rows = ( (int)visible.size() + cols - 1 ) / cols;
            const float rowH = KFX_TILE + ImGui::GetTextLineHeight() + 2.0f + KFX_TILE_PAD;
            ImGuiListClipper clipper;
            clipper.Begin( rows, rowH );
            ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( KFX_TILE_PAD, KFX_TILE_PAD ) );
            while ( clipper.Step() )
                for ( int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row )
                    for ( int c = 0; c < cols; ++c )
                    {
                        const int i = row * cols + c;
                        if ( i >= (int)visible.size() )
                            break;
                        if ( c )
                            ImGui::SameLine();
                        DrawTile( s_rows[visible[i]], visible[i], host );
                    }
            ImGui::PopStyleVar();
        }
        if ( visible.empty() )
            ImGui::TextDisabled( s_rows.empty() ? "No .efx files under raw\\fx next to the editor."
                                                : "No effect matches the filters." );
        ImGui::EndChild();
    }

    // The picked effect playing (iw3xo-radiant's effects browser has the same idea: the
    // selection plays in its own view, src/ggui/effects_browser.cpp).
    void DrawPane()
    {
        const float side = 150.0f;
        if ( s_picked.empty() )
        {
            KiwiFxPreview_Pane( "" );
            return;
        }
        IDirect3DTexture9 *tex = KiwiFxPreview_Pane( s_picked.c_str() );
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton( "##fxpane", ImVec2( side, side ) );
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if ( tex )
            dl->AddImage( (ImTextureID)(intptr_t)tex, p, ImVec2( p.x + side, p.y + side ) );
        else
            dl->AddRectFilled( p, ImVec2( p.x + side, p.y + side ), IM_COL32( 23, 26, 31, 255 ) );
        dl->AddRect( p, ImVec2( p.x + side, p.y + side ), IM_COL32( 255, 255, 255, 40 ) );
        if ( ImGui::BeginDragDropSource( ImGuiDragDropFlags_SourceNoHoldToOpenOthers ) )
        {
            ImGui::SetDragDropPayload( KFX_PAYLOAD, s_picked.c_str(), s_picked.size() + 1 );
            ImGui::TextUnformatted( s_picked.c_str() );
            ImGui::TextDisabled( "drop in the 3D view" );
            ImGui::EndDragDropSource();
        }
        if ( ImGui::IsItemHovered() && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
            KiwiFxPreview_PaneRestart();
        if ( ImGui::IsItemHovered() && !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            ImGui::SetTooltip( "Click to replay; drag into the 3D view to place." );

        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted( s_picked.c_str() );
        for ( size_t i = 0; i < s_rows.size(); ++i )
            if ( !_stricmp( s_rows[i].path.c_str(), s_picked.c_str() ) )
            {
                const fxInfo_t &info = Info( s_rows[i] );
                ImGui::TextDisabled( "%i element%s, %s", info.elements, info.elements == 1 ? "" : "s",
                                     info.looping == 0 ? "one-shot" : info.looping == info.elements ? "looping" : "partly looping" );
                break;
            }
        if ( ImGui::Button( "Place" ) )
            QueueDrop( s_picked.c_str(), -1, -1, nullptr, nullptr );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "At the centre of the 3D view (or drag the picture / a tile in)." );
        ImGui::SameLine();
        if ( ImGui::Button( "Edit" ) )
            KiwiFxEditor_Open( s_picked.c_str() );
        ImGui::BeginDisabled( !SelectedCount() );
        if ( ImGui::Button( "Use for selected" ) )
            SetOnSelected( "replace particle effect", "fx", s_picked.c_str() );
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "Every selected FX entity plays this effect.  Undoable." );
        if ( ImGui::SmallButton( "Preview in 3D view" ) )
            PreviewInFront( s_picked.c_str() );
        ImGui::SameLine();
        if ( ImGui::SmallButton( "x##unpick" ) )
            s_picked.clear();
        ImGui::EndGroup();
    }
}

void KiwiParticles_Draw()
{
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_PARTICLES );
    if ( !open || !*open )
        return;
    Load();
    if ( KiwiWindows_JustOpened( KIWI_WIN_PARTICLES ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );
    if ( !s_enumerated )
        Enumerate();

    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_PARTICLES ), open ) )
    {
        DrawBrowserFilters( s_winHost );
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Refresh" ) )
        {
            s_info.clear();
            Enumerate();
        }
        ImGui::SameLine();
        if ( ImGui::SmallButton( "New" ) )
            KiwiFxEditor_New();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Create a new effect in the FX Editor." );

        // KIWI (2026-11-02, user: "live preview ... when FX are selected"): what the camera plays.
        {
            static const char *const kModes[] = { "Off", "Selected", "All nearby" };
            int mode = KiwiFxPreview_Mode();
            ImGui::TextUnformatted( "Preview:" );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 100.0f );
            if ( ImGui::Combo( "##fxpreviewmode", &mode, kModes, 3 ) )
                KiwiFxPreview_SetMode( mode );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Effects playing in the 3D view: the selected FX, every FX within\n"
                                   "3000 units of the camera, or none.  Sprites, tails, clouds and\n"
                                   "runners play; models, lights, sounds, decals and trails do not." );
            ImGui::SameLine();
            bool paused = KiwiFxPreview_Paused();
            if ( ImGui::Checkbox( "Pause", &paused ) )
                KiwiFxPreview_SetPaused( paused );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Restart" ) )
                KiwiFxPreview_Restart( nullptr );
            if ( KiwiFxPreview_TestPlaying() )
            {
                ImGui::SameLine();
                if ( ImGui::SmallButton( "Stop test" ) )
                    KiwiFxPreview_StopTest();
            }
        }

        ImGui::TextUnformatted( "Aim:" );
        ImGui::SameLine();
        if ( ImGui::RadioButton( "World up", s_aim == 0 ) ) { s_aim = 0; Save(); }
        ImGui::SameLine();
        if ( ImGui::RadioButton( "Surface normal", s_aim == 1 ) ) { s_aim = 1; Save(); }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "The effect's forward axis at placement.  Most ambient effects (fire,\n"
                               "smoke, dust) rise along it, so World up suits floors and slopes;\n"
                               "Surface normal points sparks and drips out of walls and ceilings." );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 80.0f );
        if ( ImGui::InputFloat( "Delay##new", &s_delay, 0.0f, 0.0f, "%.1f" ) )
            Save();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Start delay for new particles, seconds.  Negative = already running\n"
                               "when the map loads (retail uses -15 for ambient loops)." );

        DrawSelectedSection();
        ImGui::Separator();
        DrawPane();

        DrawBrowserGrid( s_winHost, -( ImGui::GetFrameHeightWithSpacing() * 2.0f + 4.0f ) );

        if ( ImGui::Button( "Write game scripts now" ) )
            KiwiParticles_ExportForMap( currentmap, false );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "raw/maps/mp/<map>_fx.gsc (the loadfx table) and raw/maps/createfx/<map>_fx.gsc\n"
                               "(one block per particle).  Also written on every map save.  The MP client\n"
                               "plays them (clientSideEffects), and the Build panel's FASTFILE stage packs\n"
                               "both scripts and every effect they name.  Hand-written scripts are never\n"
                               "overwritten.  At most 32 different effects per map (client limit)." );
        ImGui::SameLine();
        ImGui::TextDisabled( "%s", s_exportStatus[0] ? s_exportStatus : "written on every map save" );
    }
    ImGui::End();
}

// KIWI (2026-11-03, user: "the FX editor needs a browser"): the same tile browser inside
// the FX Editor; a click opens the effect there, drag / double-click still place it.
void KiwiParticles_DrawEditorBrowser( const char *currentFx, float height )
{
    Load();
    if ( !s_enumerated )
        Enumerate();
    if ( currentFx && currentFx[0] )
        s_edHost.picked = currentFx;            // the open effect's tile is outlined
    s_edHost.editor = true;
    DrawBrowserFilters( s_edHost );
    ImGui::SameLine();
    if ( ImGui::SmallButton( "Refresh##fxed" ) )
    {
        s_info.clear();
        Enumerate();
    }
    DrawBrowserGrid( s_edHost, height );
}

// ── camera drop target + world overlay ─────────────────────────────────────────
bool KiwiParticles_CameraDropTarget( float imgMinX, float imgMinY )
{
    if ( ImGui::GetDragDropPayload() == nullptr )
        s_ghostHave = false;
    if ( !ImGui::BeginDragDropTarget() )
    {
        s_ghostHave = false;
        return false;
    }
    bool took = false;
    const ImGuiPayload *p = ImGui::AcceptDragDropPayload(
        KFX_PAYLOAD, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect );
    if ( p && p->Data && p->DataSize > 0 )
    {
        std::string fx( (const char *)p->Data, strnlen( (const char *)p->Data, (size_t)p->DataSize ) );
        const ImVec2 m = ImGui::GetIO().MousePos;
        const int x = (int)( m.x - imgMinX ), y = (int)( m.y - imgMinY );
        float origin[3], normal[3];
        if ( ResolvePlacement( x, y, origin, normal ) )
        {
            memcpy( s_ghostOrigin, origin, sizeof( origin ) );
            memcpy( s_ghostNormal, normal, sizeof( normal ) );
            s_ghostFx   = fx;
            s_ghostHave = true;
            g_nUpdateBits |= W_CAMERA;
        }
        if ( p->Delivery )
        {
            took = QueueDrop( fx.c_str(), x, y, s_ghostHave ? s_ghostOrigin : nullptr, s_ghostNormal );
            s_ghostHave = false;
        }
    }
    else
        s_ghostHave = false;
    ImGui::EndDragDropTarget();
    return took;
}

void KiwiParticles_DrawWorld()
{
    const eclass_t *ec = FxEclass();
    if ( !ec && !s_ghostHave )
        return;

    struct arrow_t { float o[3], d[3]; bool sel; };
    std::vector<arrow_t> arrows;
    if ( ec )
    {
        for ( int pass = 0; pass < 2 && (int)arrows.size() < KFX_ARROW_CAP; ++pass )
        {
            selbrush_t *head = pass == 0 ? &active_brushes : &selected_brushes;
            for ( selbrush_t *b = head->next; b && b != head && (int)arrows.size() < KFX_ARROW_CAP; b = b->next )
            {
                entity_s_def *def = NodeDef( b );
                if ( !IsFxDef( def, ec ) || ( b->owner && b->owner->prefab ) || !Pick_BrushPickable( b ) )
                    continue;
                arrow_t a;
                float ang[3];
                FxAngles( def, ang );
                AnglesToDir( ang, a.d );
                FxOrigin( def, a.o );
                a.sel = pass == 1;
                arrows.push_back( a );
            }
        }
    }
    if ( arrows.empty() && !s_ghostHave )
        return;

    // The arrow is the effect's forward axis: a 24-unit shaft and a 4-line head.
    auto emit = []( const float o[3], const float d[3] )
    {
        float tip[3], side[3], up[3] = { 0.0f, 0.0f, 1.0f };
        for ( int k = 0; k < 3; ++k ) tip[k] = o[k] + d[k] * 24.0f;
        if ( fabsf( d[2] ) > 0.9f ) { up[0] = 1.0f; up[2] = 0.0f; }
        side[0] = d[1] * up[2] - d[2] * up[1];
        side[1] = d[2] * up[0] - d[0] * up[2];
        side[2] = d[0] * up[1] - d[1] * up[0];
        const float sl = sqrtf( side[0] * side[0] + side[1] * side[1] + side[2] * side[2] );
        if ( sl > 1e-6f ) for ( int k = 0; k < 3; ++k ) side[k] /= sl;
        float up2[3] = { d[1] * side[2] - d[2] * side[1], d[2] * side[0] - d[0] * side[2], d[0] * side[1] - d[1] * side[0] };
        KiwiLines_Add( o, tip );
        for ( int s = -1; s <= 1; s += 2 )
        {
            float h1[3], h2[3];
            for ( int k = 0; k < 3; ++k )
            {
                h1[k] = tip[k] - d[k] * 6.0f + side[k] * 3.0f * (float)s;
                h2[k] = tip[k] - d[k] * 6.0f + up2[k] * 3.0f * (float)s;
            }
            KiwiLines_Add( tip, h1 );
            KiwiLines_Add( tip, h2 );
        }
    };

    KiwiLines_Begin( (int)arrows.size() * 5 + 20, 2 );
    KiwiLines_Color( 1.0f, 0.55f, 0.1f );
    for ( size_t i = 0; i < arrows.size(); ++i )
        if ( !arrows[i].sel )
            emit( arrows[i].o, arrows[i].d );
    KiwiLines_Color( 1.0f, 0.95f, 0.35f );
    for ( size_t i = 0; i < arrows.size(); ++i )
        if ( arrows[i].sel )
            emit( arrows[i].o, arrows[i].d );
    if ( s_ghostHave )
    {
        // Where the drop will land: a small box and the aim it will get.
        float mins[3], maxs[3], corners[KIWI_BOX_CORNERS][3];
        for ( int k = 0; k < 3; ++k ) { mins[k] = s_ghostOrigin[k] - 6.0f; maxs[k] = s_ghostOrigin[k] + 6.0f; }
        KiwiBox_Corners( mins, maxs, corners );
        KiwiLines_Color( 0.45f, 0.85f, 1.0f );
        for ( int e = 0; e < KIWI_BOX_EDGES; ++e )
            KiwiLines_Add( corners[KIWI_BOX_EDGE[e][0]], corners[KIWI_BOX_EDGE[e][1]] );
        float d[3] = { 0.0f, 0.0f, 1.0f };
        if ( s_aim == 1 )
            memcpy( d, s_ghostNormal, sizeof( d ) );
        emit( s_ghostOrigin, d );
    }
    KiwiLines_Flush();
}

// ── commands ───────────────────────────────────────────────────────────────────
void KiwiParticles_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiWindowParticles",      0, 0, KIWI_CMD_WINDOW_PARTICLES );
    Radiant_RegisterCommand( "KiwiWriteParticleScripts", 0, 0, KIWI_CMD_PARTICLE_EXPORT );
}

bool KiwiParticles_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId == (unsigned int)KIWI_CMD_PARTICLE_EXPORT )
    {
        KiwiParticles_ExportForMap( currentmap, false );
        return true;
    }
    if ( cmdId != (unsigned int)KIWI_CMD_PARTICLE_DROP )
        return false;
    if ( !s_pendHave )
        return true;                         // ours, already consumed
    s_pendHave = false;
    Load();
    const std::string fx = s_pendFx;
    float origin[3], normal[3];
    bool have = s_pendPlacement;
    if ( have )
    {
        memcpy( origin, s_pendOrigin, sizeof( origin ) );
        memcpy( normal, s_pendNormal, sizeof( normal ) );
    }
    else
    {
        int x = s_pendX, y = s_pendY;
        if ( x < 0 || y < 0 )
        {
            camera_s *cam = Ed_Camera();
            x = cam ? cam->width / 2 : 0;
            y = cam ? cam->height / 2 : 0;
        }
        have = ResolvePlacement( x, y, origin, normal );
    }
    if ( !have )
    {
        Sys_Printf( "Particles: no place to put '%s' under the cursor.\n", fx.c_str() );
        return true;
    }
    Place( fx.c_str(), origin, normal );
    return true;
}

void KiwiParticles_ResetForNewMap()
{
    s_pendHave  = false;
    s_ghostHave = false;
    s_exportStatus[0] = '\0';
    KiwiFxPreview_ResetForNewMap();   // KIWI (2026-11-02): drop the camera preview's instances
}

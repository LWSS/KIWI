#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// KIWI (2026-10-26, user: "material properties panel") - the Material window; see
// kiwi_matprops.h for the save / backup / revert contract.

#include "stdafx.h"
#include "qe3.h"

#include <imgui/imgui.h>
#include <imgui_internal.h>             // FindWindowByName / DockId (dock beside the 2D View)

#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_gfx.h>
#include <d3d9.h>
#include <universal/com_files.h>
#include <universal/surfaceflags.h>

#include "kiwi_matprops.h"
#include "kiwi_matwriter.h"             // KiwiFS_WriteRawAtomic
#include "kiwi_ported.h"                // Texture_GetHandle, BrushList_RebuildDisplay, ImGuiShell_DockRoot
#include "kiwi_walkcache.h"             // KiwiWalkCache_MarkStructural
#include "kiwi_windows.h"

#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

extern qtexture_s *TexWnd_RegisterMaterialByName( const char *name );   // texwnd.cpp:621

namespace
{
    // The three header fields Save patches, by the loader's own struct (r_material.h:625/644).
    static_assert( sizeof( MaterialRaw ) == 0x40, "MaterialRaw must be 64 bytes" );
    static_assert( sizeof( MaterialTextureDefRaw ) == 0x0C, "MaterialTextureDefRaw must be 12 bytes" );
    static_assert( offsetof( MaterialRaw, info ) == 0x00, "MaterialRaw.info at +0x00" );
    static_assert( offsetof( MaterialInfoRaw, sortKey ) == 0x09, "sortKey at +0x09" );
    static_assert( offsetof( MaterialInfoRaw, surfaceFlags ) == 0x20, "surfaceFlags at +0x20" );
    static_assert( offsetof( MaterialInfoRaw, contents ) == 0x24, "contents at +0x24" );

    const size_t KMP_OFS_SORT     = offsetof( MaterialInfoRaw, sortKey );
    const size_t KMP_OFS_SURF     = offsetof( MaterialInfoRaw, surfaceFlags );
    const size_t KMP_OFS_CONTENTS = offsetof( MaterialInfoRaw, contents );

    // kiwi_matconvert.cpp keeps its moved originals in the same folder as plain <name>[.N];
    // the suffix keeps ours apart.  FS_ListFiles( "materials" ) does not descend into it.
    const char *KMP_BACKUP_FMT = "materials/_kiwi_backup/%s.matprops";

    const float  KMP_SWATCH = 64.0f;     // colour-map swatch, pixels
    const float  KMP_COL    = 112.0f;    // checkbox grid column
    const float  KMP_BAR    = 140.0f;    // penetration bar
    const float  KMP_BTN    = 72.0f;     // Save / Discard / Revert
    const ImVec4 KMP_CHANGED( 1.0f, 0.78f, 0.30f, 1.0f );
    const ImVec4 KMP_ERROR  ( 1.0f, 0.45f, 0.35f, 1.0f );

    // Bit names: infoParms spellings (universal/surfaceflags.cpp:4) where one entry owns the
    // bit, else the q_shared.h / surfaceflags.h macro name.
    struct BitName
    {
        const char *name;
        int         bit;
        const char *tip;
    };

    const BitName kSurfBits[] = {
        { "nonsolid",     SURF_NONSOLID,     "Not solid" },
        { "nodraw",       SURF_NODRAW,       "Not drawn" },
        { "nolightmap",   SURF_NOLIGHTMAP,   "No lightmap" },
        { "nocastshadow", SURF_NOCASTSHADOW, "Casts no shadow" },
        { "nodlight",     SURF_NODLIGHT,     "No dynamic light" },
        { "noimpact",     SURF_NOIMPACT,     "No impact effects" },
        { "nomarks",      SURF_NOMARKS,      "No bullet marks or decals" },
        { "nopenetrate",  SURF_NOPENETRATE,  "Bullets stop here" },
        { "nosteps",      SURF_NOSTEPS,      "No footstep sounds" },
        { "nodamage",     SURF_NODAMAGE,     "No falling damage" },
        { "slick",        SURF_SLICK,        "Slippery" },
        { "ladder",       SURF_LADDER,       "Climbable" },
        { "mantleOn",     SURF_MANTLEON,     "Mantle on top" },
        { "mantleOver",   SURF_MANTLEOVER,   "Mantle over" },
        { "sky",          SURF_SKY,          "Sky surface" },
        { "portal",       SURF_PORTAL,       "Portal" },
        { "hdrportal",    SURF_HDRPORTAL,    "HDR portal" },
    };

    const BitName kContentBits[] = {
        { "solid",        CONTENTS_SOLID,        "Blocks players and bullets" },
        { "detail",       CONTENTS_DETAIL,       "Detail: does not split the BSP" },
        { "structural",   CONTENTS_STRUCTURAL,   "Structural" },
        { "translucent",  CONTENTS_TRANSLUCENT,  "Translucent" },
        { "glass",        CONTENTS_GLASS,        "Glass" },
        { "water",        CONTENTS_WATER,        "Water volume" },
        { "foliage",      CONTENTS_FOLIAGE,      "Foliage" },
        { "noncolliding", CONTENTS_NONCOLLIDING, "Non-colliding" },
        { "playerclip",   CONTENTS_PLAYERCLIP,   "Blocks players" },
        { "monsterclip",  CONTENTS_MONSTERCLIP,  "Blocks AI" },
        { "vehicleclip",  CONTENTS_VEHICLECLIP,  "Blocks vehicles" },
        { "itemclip",     CONTENTS_ITEMCLIP,     "Blocks items" },
        { "clipmissile",  CONTENTS_MISSILECLIP,  "Blocks missiles" },
        { "clipshot",     CONTENTS_CLIPSHOT,     "Blocks bullets" },
        { "canshootclip", CONTENTS_CANSHOOTCLIP, "Can-shoot clip" },
        { "ai_nosight",   CONTENTS_AI_NOSIGHT,   "Blocks AI sight" },
        { "sky",          CONTENTS_SKY,          "Sky" },
        { "mantle",       CONTENTS_MANTLE,       "Mantle volume" },
        { "nodrop",       (int)CONTENTS_NODROP,  "No drop" },   // 0x80000000u: cast, no narrowing
    };

    struct TexRow
    {
        std::string   slot;      // "colorMap", "normalMap", ...
        std::string   image;
        unsigned char semantic;
    };

    // The file as last read plus the staged edits.
    struct Loaded
    {
        std::string                name;           // browser name ("" = nothing loaded yet)
        bool                       ok = false;
        std::string                error;
        std::vector<unsigned char> bytes;
        std::string                embeddedName;
        std::string                techSet;
        std::string                refImage;
        unsigned short             toolFlags    = 0;
        int                        diskSurf     = 0;
        int                        diskContents = 0;
        int                        diskSort     = 0;
        int                        surf         = 0;   // staged
        int                        contents     = 0;   // staged
        int                        sort         = 0;   // staged
        std::vector<TexRow>        textures;
        bool                       rawCopy       = false;   // raw/materials/<name> exists
        bool                       backup        = false;   // our backup exists
        bool                       backupDiffers = false;   // ... and is not the current file
    };

    Loaded      s_cur;
    std::string s_status;
    bool        s_statusErr = false;
    std::string s_thumbTried;               // one Texture_GetHandle per selected name

    std::map<std::string, float> s_pen;     // "small_brick" -> 6
    bool                         s_penLoaded = false;
    float                        s_penMax    = 72.0f;
    std::vector<int>             s_typeOrder;

    void SetStatus( bool err, const char *fmt, ... )
    {
        char buf[768];
        va_list ap;
        va_start( ap, fmt );
        _vsnprintf( buf, sizeof( buf ) - 1, fmt, ap );
        va_end( ap );
        buf[sizeof( buf ) - 1] = '\0';
        s_status    = buf;
        s_statusErr = err;
    }

    // ── files ──────────────────────────────────────────────────────────────────────
    // Through the engine filesystem: the bytes the editor and Material_Load actually see.
    bool ReadQpath( const char *qpath, std::vector<unsigned char> *out )
    {
        out->clear();
        int fh = 0;
        com_fileAccessed = 1;
        const int size = (int)FS_FOpenFileRead( qpath, &fh );
        if ( !fh )
            return false;
        if ( size <= 0 )
        {
            FS_FCloseFile( fh );
            return false;
        }
        out->assign( (size_t)size, 0 );
        const unsigned got = FS_Read( &( *out )[0], (unsigned)size, fh );
        FS_FCloseFile( fh );
        return got == (unsigned)size;
    }

    // raw/<qpath> under fs_homepath: where KiwiFS_WriteRawAtomic writes.
    void RawOsPath( const char *qpath, char *out )
    {
        FS_BuildOSPath( fs_homepath->current.string, "raw", qpath, out );
    }

    bool ReadOsFile( const char *path, std::vector<unsigned char> *out )
    {
        out->clear();
        FILE *f = fopen( path, "rb" );
        if ( !f )
            return false;
        fseek( f, 0, SEEK_END );
        const long n = ftell( f );
        fseek( f, 0, SEEK_SET );
        bool ok = n > 0;
        if ( ok )
        {
            out->assign( (size_t)n, 0 );
            ok = fread( &( *out )[0], 1, (size_t)n, f ) == (size_t)n;
        }
        fclose( f );
        return ok;
    }

    bool OsFileExists( const char *path )
    {
        const DWORD a = ::GetFileAttributesA( path );
        return a != INVALID_FILE_ATTRIBUTES && !( a & FILE_ATTRIBUTE_DIRECTORY );
    }

    void MaterialQpath( const std::string &name, char *out, size_t n )
    {
        _snprintf( out, n, "materials/%s", name.c_str() );
        out[n - 1] = '\0';
    }

    void BackupQpath( const std::string &name, char *out, size_t n )
    {
        _snprintf( out, n, KMP_BACKUP_FMT, name.c_str() );
        out[n - 1] = '\0';
    }

    // ── parse (Material_LoadRaw's offsets, as kiwi_matwriter.cpp ParseMaterial) ───────
    bool Str( const std::vector<unsigned char> &b, unsigned off, std::string *out )
    {
        if ( off == 0 || off >= b.size() )
            return false;
        const char *base = (const char *)&b[0];
        for ( size_t i = off; i < b.size(); ++i )
            if ( base[i] == '\0' )
            {
                out->assign( base + off, i - off );
                return true;
            }
        return false;
    }

    bool Parse( const std::vector<unsigned char> &b, Loaded *m, std::string *why )
    {
        if ( b.size() < sizeof( MaterialRaw ) )
        {
            *why = "File is shorter than a material header";
            return false;
        }
        MaterialRaw raw;
        memcpy( &raw, &b[0], sizeof( raw ) );
        if ( !Str( b, raw.info.nameOffset, &m->embeddedName ) || !Str( b, raw.techSetNameOffset, &m->techSet ) )
        {
            *why = "Material file does not parse";
            return false;
        }
        if ( !Str( b, raw.info.refImageNameOffset, &m->refImage ) )
            m->refImage.clear();

        const size_t texEnd = (size_t)raw.textureTableOffset + (size_t)raw.textureCount * sizeof( MaterialTextureDefRaw );
        if ( raw.textureCount && texEnd > b.size() )
        {
            *why = "Texture table runs past the end of the file";
            return false;
        }
        m->textures.clear();
        for ( unsigned i = 0; i < raw.textureCount; ++i )
        {
            MaterialTextureDefRaw t;
            memcpy( &t, &b[0] + raw.textureTableOffset + sizeof( MaterialTextureDefRaw ) * i, sizeof( t ) );
            TexRow row;
            row.semantic = t.semantic;
            if ( !Str( b, t.nameOffset, &row.slot ) )
                row.slot = "?";
            if ( t.semantic == 11 )                       // TS_WATER_MAP: a water def, not a name
                row.image = "(water)";
            else if ( !Str( b, t.u.imageNameOffset, &row.image ) )
                row.image = "?";
            m->textures.push_back( row );
        }

        m->toolFlags    = raw.info.toolFlags;
        m->diskSurf     = raw.info.surfaceFlags;
        m->diskContents = raw.info.contents;
        m->diskSort     = raw.info.sortKey;
        return true;
    }

    void Load( const std::string &name )
    {
        s_cur      = Loaded();
        s_cur.name = name;

        char qpath[160];
        MaterialQpath( name, qpath, sizeof( qpath ) );
        if ( !ReadQpath( qpath, &s_cur.bytes ) )
        {
            s_cur.error = "No material file";
            return;
        }
        std::string why;
        if ( !Parse( s_cur.bytes, &s_cur, &why ) )
        {
            s_cur.error = why;
            return;
        }
        s_cur.surf     = s_cur.diskSurf;
        s_cur.contents = s_cur.diskContents;
        s_cur.sort     = s_cur.diskSort;

        char os[MAX_OSPATH];
        RawOsPath( qpath, os );
        s_cur.rawCopy = OsFileExists( os );

        char bq[192];
        BackupQpath( name, bq, sizeof( bq ) );
        RawOsPath( bq, os );
        std::vector<unsigned char> bak;
        s_cur.backup        = ReadOsFile( os, &bak );
        s_cur.backupDiffers = s_cur.backup && bak != s_cur.bytes;
        s_cur.ok            = true;
    }

    // ── live refresh (kiwi_matconvert.cpp RefreshLiveMaterial) ─────────────────────────
    // Re-run Material_LoadRaw on the new file, copy it over the registered Material so every
    // existing pointer sees the new surfaceTypeBits / sortKey / cast-shadow flag, re-sort, and
    // re-stamp the browser entry (filters read its surfaceFlags / contents copy).
    bool ReloadLive( const std::string &name, char *err, size_t errSz )
    {
        qtexture_s *browser = Texture_GetHandle( name.c_str() );
        Material   *old     = browser ? browser->handle : nullptr;

        char full[96];
        _snprintf( full, sizeof( full ), "wc/%s", name.c_str() );
        full[sizeof( full ) - 1] = '\0';
        Material *fresh = Material_Load( full, 0 );
        bool ok = true;
        if ( !fresh || Material_IsDefault( fresh ) )
        {
            _snprintf( err, errSz, "the renderer could not reload '%s' (see the console)", name.c_str() );
            err[errSz - 1] = '\0';
            ok = false;
        }
        else if ( old && !Material_IsDefault( old ) )   // never overwrite $default
        {
            const unsigned short hashIndex = old->info.hashIndex;
            *old = *fresh;
            old->info.hashIndex = hashIndex;
            Material_Sort();
        }

        if ( TexWnd_RegisterMaterialByName( name.c_str() ) )
            browser = Texture_GetHandle( name.c_str() );
        if ( browser && old && !Material_IsDefault( old ) )
            browser->handle = old;

        BrushList_RebuildDisplay( &active_brushes );
        BrushList_RebuildDisplay( &selected_brushes );
        BrushList_RebuildDisplay( &filtered_brushes );
        KiwiWalkCache_MarkStructural();             // nocastshadow changes what brushes cast
        g_nUpdateBits = -1;
        return ok;
    }

    // Atomic write, then prove it: the OS file holds `data`, and the engine filesystem (what
    // the editor and the compilers resolve) reads the same bytes back.
    bool WriteVerified( const std::string &name, const std::vector<unsigned char> &data )
    {
        char qpath[160];
        MaterialQpath( name, qpath, sizeof( qpath ) );
        char err[512] = { 0 };
        if ( !KiwiFS_WriteRawAtomic( qpath, &data[0], (unsigned)data.size(), err, sizeof( err ) ) )
        {
            SetStatus( true, "Save failed: %s", err );
            return false;
        }
        char os[MAX_OSPATH];
        RawOsPath( qpath, os );
        std::vector<unsigned char> back;
        if ( !ReadOsFile( os, &back ) || back != data )
        {
            SetStatus( true, "Save did not verify: %s does not hold the written bytes", os );
            return false;
        }
        if ( !ReadQpath( qpath, &back ) || back != data )
        {
            SetStatus( true, "Wrote %s, but the editor reads another copy of materials/%s (a searchpath shadows it)",
                       os, name.c_str() );
            return false;
        }
        return true;
    }

    void PutI32( std::vector<unsigned char> &b, size_t ofs, int v )
    {
        memcpy( &b[ofs], &v, sizeof( v ) );
    }

    void Save()
    {
        const std::string name = s_cur.name;
        char qpath[160];
        MaterialQpath( name, qpath, sizeof( qpath ) );

        std::vector<unsigned char> now;
        if ( !ReadQpath( qpath, &now ) || now != s_cur.bytes )
        {
            Load( name );
            SetStatus( true, "The file changed on disk and was reloaded; nothing was written" );
            return;
        }

        // Backup before the first overwrite; an existing one is the older original, kept.
        char bq[192], bos[MAX_OSPATH];
        BackupQpath( name, bq, sizeof( bq ) );
        RawOsPath( bq, bos );
        if ( !OsFileExists( bos ) )
        {
            char err[512] = { 0 };
            std::vector<unsigned char> check;
            if ( !KiwiFS_WriteRawAtomic( bq, &now[0], (unsigned)now.size(), err, sizeof( err ) ) )
            {
                SetStatus( true, "Backup failed, nothing was written: %s", err );
                return;
            }
            if ( !ReadOsFile( bos, &check ) || check != now )
            {
                SetStatus( true, "Backup %s did not verify; nothing was written", bos );
                return;
            }
            Sys_Printf( "Material '%s': backup %s\n", name.c_str(), bos );
        }

        // Byte-patch the three fields; every other byte is the file as read.
        std::vector<unsigned char> out = now;
        PutI32( out, KMP_OFS_SURF, s_cur.surf );
        PutI32( out, KMP_OFS_CONTENTS, s_cur.contents );
        out[KMP_OFS_SORT] = (unsigned char)s_cur.sort;

        const int oldSurf = s_cur.diskSurf, oldContents = s_cur.diskContents, oldSort = s_cur.diskSort;
        if ( !WriteVerified( name, out ) )
        {
            Load( name );
            return;
        }
        char err[256] = { 0 };
        const bool live = ReloadLive( name, err, sizeof( err ) );
        Sys_Printf( "Material '%s': surfaceFlags 0x%08X -> 0x%08X, contents 0x%08X -> 0x%08X, sort %d -> %d\n",
                    name.c_str(), (unsigned)oldSurf, (unsigned)s_cur.surf,
                    (unsigned)oldContents, (unsigned)s_cur.contents, oldSort, s_cur.sort );
        Load( name );
        if ( live )
            SetStatus( false, "Saved. Recompile maps to see it in game." );
        else
            SetStatus( true, "Saved, but %s", err );
    }

    void Revert()
    {
        const std::string name = s_cur.name;
        char bq[192], bos[MAX_OSPATH];
        BackupQpath( name, bq, sizeof( bq ) );
        RawOsPath( bq, bos );
        std::vector<unsigned char> bak;
        Loaded probe;
        std::string why;
        if ( !ReadOsFile( bos, &bak ) || !Parse( bak, &probe, &why ) )
        {
            SetStatus( true, "Backup %s is missing or unreadable", bos );
            return;
        }
        if ( !WriteVerified( name, bak ) )
        {
            Load( name );
            return;
        }
        char err[256] = { 0 };
        const bool live = ReloadLive( name, err, sizeof( err ) );
        Sys_Printf( "Material '%s': reverted from %s\n", name.c_str(), bos );
        Load( name );
        if ( live )
            SetStatus( false, "Reverted. Recompile maps to see it in game." );
        else
            SetStatus( true, "Reverted, but %s", err );
    }

    // ── bullet penetration (BG_LoadPenetrationDepthTable, bg_weapons.cpp:46) ─────────────
    // "BULLET_PEN_TABLE\small_bark\20\..." - the game keys each row "<size>_" +
    // Com_SurfaceTypeToName( type ) and reads a missing row as 0.
    void LoadPen()
    {
        if ( s_penLoaded )
            return;
        s_penLoaded = true;
        void *buf = nullptr;
        const int len = FS_ReadFile( "info/bullet_penetration_mp", &buf );
        if ( len <= 0 || !buf )
        {
            if ( buf )
                FS_FreeFile( (char *)buf );
            return;
        }
        std::string text( (const char *)buf, (size_t)len );
        FS_FreeFile( (char *)buf );

        std::vector<std::string> tok;
        std::string cur;
        for ( size_t i = 0; i <= text.size(); ++i )
        {
            const char c = i < text.size() ? text[i] : '\\';
            if ( c == '\\' )
            {
                tok.push_back( cur );
                cur.clear();
            }
            else if ( c != '\r' && c != '\n' && c != ' ' && c != '\t' )
                cur += (char)tolower( (unsigned char)c );
        }
        size_t i = 0;
        if ( !tok.empty() && ( tok[0].empty() || tok[0] == "bullet_pen_table" ) )
            i = 1;
        float maxV = 0.0f;
        for ( ; i + 1 < tok.size(); i += 2 )
        {
            if ( tok[i].empty() )
                continue;
            const float v = (float)atof( tok[i + 1].c_str() );
            s_pen[tok[i]] = v;
            maxV = (std::max)( maxV, v );
        }
        if ( maxV > 0.0f )
            s_penMax = maxV;
    }

    // ── surface types ──────────────────────────────────────────────────────────────────
    // Display spelling; infoParms spells foliage "foilage" (surfaceflags.cpp:13).
    const char *TypeName( int t )
    {
        if ( t <= 0 || t >= SURF_TYPECOUNT )
            return "default";
        const char *n = Com_SurfaceTypeToName( t );
        return !_stricmp( n, "foilage" ) ? "foliage" : n;
    }

    void BuildTypeOrder()
    {
        if ( !s_typeOrder.empty() )
            return;
        for ( int t = 1; t < SURF_TYPECOUNT; ++t )
            s_typeOrder.push_back( t );
        std::sort( s_typeOrder.begin(), s_typeOrder.end(),
                   []( int a, int b ) { return _stricmp( TypeName( a ), TypeName( b ) ) < 0; } );
        s_typeOrder.insert( s_typeOrder.begin(), 0 );
    }

    // ── widgets ────────────────────────────────────────────────────────────────────────
    IDirect3DTexture9 *ColorMap( qtexture_s *q )   // kiwi_glass.cpp ColorMap
    {
        if ( !q || !q->handle )
            return nullptr;
        Material *mtl = q->handle;
        if ( !mtl->textureTable )
            return nullptr;
        for ( int i = 0; i < (int)mtl->textureCount; ++i )
        {
            if ( mtl->textureTable[i].semantic != 2 )        // TS_COLOR_MAP
                continue;
            GfxImage *img = mtl->textureTable[i].u.image;
            if ( !img || img->delayLoadPixels )
                continue;
            if ( img->mapType == MAPTYPE_2D && img->texture.map )
                return img->texture.map;
        }
        return nullptr;
    }

    void Swatch( qtexture_s *q )
    {
        if ( q && !q->handle && s_thumbTried != q->name )
        {
            s_thumbTried = q->name;
            Texture_GetHandle( q->name );                     // shows from the next frame
        }
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy( ImVec2( KMP_SWATCH, KMP_SWATCH ) );
        const ImVec2 p1( p.x + KMP_SWATCH, p.y + KMP_SWATCH );
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if ( IDirect3DTexture9 *tex = ColorMap( q ) )
            dl->AddImage( (ImTextureID)(intptr_t)tex, p, p1 );
        else
            dl->AddRectFilled( p, p1, IM_COL32( 60, 60, 66, 255 ) );
        dl->AddRect( p, p1, IM_COL32( 90, 90, 100, 255 ) );
        if ( ImGui::IsItemHovered() && !s_cur.refImage.empty() )
            ImGui::SetTooltip( "%s", s_cur.refImage.c_str() );
    }

    void BitBox( const BitName &b, int *staged, int disk )
    {
        bool on = ( *staged & b.bit ) != 0;
        const bool changed = ( ( *staged ^ disk ) & b.bit ) != 0;
        if ( changed )
            ImGui::PushStyleColor( ImGuiCol_Text, KMP_CHANGED );
        const bool hit = ImGui::Checkbox( b.name, &on );
        if ( changed )
            ImGui::PopStyleColor();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "%s\n0x%08X", b.tip, (unsigned)b.bit );
        if ( hit )
            *staged = on ? ( *staged | b.bit ) : ( *staged & ~b.bit );
    }

    void BitGrid( const char *id, const BitName *bits, int count, int *staged, int disk, int alsoKnown )
    {
        ImGui::PushID( id );
        int cols = (int)( ImGui::GetContentRegionAvail().x / KMP_COL );
        cols = cols < 1 ? 1 : ( cols > 4 ? 4 : cols );
        if ( ImGui::BeginTable( "##bits", cols, ImGuiTableFlags_SizingFixedFit ) )
        {
            for ( int c = 0; c < cols; ++c )
                ImGui::TableSetupColumn( nullptr, ImGuiTableColumnFlags_WidthFixed, KMP_COL );
            for ( int i = 0; i < count; ++i )
            {
                ImGui::TableNextColumn();
                BitBox( bits[i], staged, disk );
            }
            ImGui::EndTable();
        }
        int known = alsoKnown;
        for ( int i = 0; i < count; ++i )
            known |= bits[i].bit;
        if ( const int other = *staged & ~known )
            ImGui::TextDisabled( "+ unnamed bits 0x%08X (kept)", (unsigned)other );
        ImGui::PopID();
    }

    void DrawTypeAndSort()
    {
        BuildTypeOrder();
        const int type = SURF_TYPEINDEX( s_cur.surf );
        const bool typeChanged = type != SURF_TYPEINDEX( s_cur.diskSurf );
        ImGui::SetNextItemWidth( 150.0f );
        if ( typeChanged )
            ImGui::PushStyleColor( ImGuiCol_Text, KMP_CHANGED );
        const bool open = ImGui::BeginCombo( "Surface", TypeName( type ) );
        if ( typeChanged )
            ImGui::PopStyleColor();
        if ( !open && ImGui::IsItemHovered() )
        {
            const char *game = type > 0 ? Com_SurfaceTypeToName( type ) : "default";
            ImGui::SetTooltip( "Footsteps step_*_%s\nImpacts bullet_*_%s\nPenetration below", game, game );
        }
        if ( open )
        {
            for ( size_t i = 0; i < s_typeOrder.size(); ++i )
            {
                const int t = s_typeOrder[i];
                if ( ImGui::Selectable( TypeName( t ), t == type ) )
                    s_cur.surf = ( s_cur.surf & ~SURF_TYPE_MASK ) | ( ( t << SURF_TYPE_SHIFT ) & SURF_TYPE_MASK );
                if ( t == type )
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        const bool sortChanged = s_cur.sort != s_cur.diskSort;
        ImGui::SetNextItemWidth( 100.0f );
        if ( sortChanged )
            ImGui::PushStyleColor( ImGuiCol_Text, KMP_CHANGED );
        int sort = s_cur.sort;
        if ( ImGui::InputInt( "Sort", &sort, 1, 4 ) )
            s_cur.sort = sort < 0 ? 0 : ( sort > 63 ? 63 : sort );      // bcassert( sortKey, 0x40 )
        if ( sortChanged )
            ImGui::PopStyleColor();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Draw order, 0..63\nShipped: 4 opaque, 12 decal, 43 glass / blend" );
    }

    void DrawPenetration()
    {
        LoadPen();
        ImGui::SeparatorText( "Bullet penetration" );
        if ( s_pen.empty() )
        {
            ImGui::TextDisabled( "No info/bullet_penetration_mp" );
            return;
        }
        const int  type    = SURF_TYPEINDEX( s_cur.surf );
        const bool blocked = ( s_cur.surf & SURF_NOPENETRATE ) != 0;
        static const char *const kSizes[3] = { "small", "medium", "large" };
        ImDrawList *dl = ImGui::GetWindowDrawList();
        for ( int i = 0; i < 3; ++i )
        {
            std::string key = std::string( kSizes[i] ) + "_" + ( type > 0 ? Com_SurfaceTypeToName( type ) : "default" );
            for ( size_t k = 0; k < key.size(); ++k )
                key[k] = (char)tolower( (unsigned char)key[k] );
            std::map<std::string, float>::const_iterator it = s_pen.find( key );
            const bool  found = it != s_pen.end();
            const float v     = found ? it->second : 0.0f;
            const float x0    = ImGui::GetCursorPosX();

            ImGui::TextUnformatted( kSizes[i] );
            ImGui::SameLine( x0 + 60.0f );
            const float  h = ImGui::GetTextLineHeight();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Dummy( ImVec2( KMP_BAR, h ) );
            const bool hovered = ImGui::IsItemHovered();
            dl->AddRectFilled( p, ImVec2( p.x + KMP_BAR, p.y + h ), IM_COL32( 50, 50, 56, 255 ) );
            if ( !blocked && v > 0.0f )
            {
                const float w = KMP_BAR * ( v < s_penMax ? v / s_penMax : 1.0f );
                dl->AddRectFilled( p, ImVec2( p.x + w, p.y + h ), IM_COL32( 110, 170, 230, 255 ) );
            }
            ImGui::SameLine();
            if ( blocked || v <= 0.0f )
                ImGui::TextDisabled( "stops" );
            else
                ImGui::Text( "%g in", v );
            if ( hovered )
            {
                if ( found )
                    ImGui::SetTooltip( "%s weapons: %g units into %s", kSizes[i], v, TypeName( type ) );
                else
                    ImGui::SetTooltip( "No '%s' row: the game reads 0", key.c_str() );
            }
        }
        if ( blocked )
            ImGui::TextDisabled( "nopenetrate: bullets stop here" );
    }

    void DrawTextures()
    {
        if ( s_cur.textures.empty() )
        {
            ImGui::TextDisabled( "No textures" );
            return;
        }
        if ( ImGui::BeginTable( "##tex", 2, ImGuiTableFlags_SizingFixedFit ) )
        {
            ImGui::TableSetupColumn( nullptr, ImGuiTableColumnFlags_WidthFixed, 96.0f );
            ImGui::TableSetupColumn( nullptr, ImGuiTableColumnFlags_WidthFixed, 240.0f );
            for ( size_t i = 0; i < s_cur.textures.size(); ++i )
            {
                const TexRow &t = s_cur.textures[i];
                ImGui::TableNextColumn();
                ImGui::TextDisabled( "%s", t.slot.c_str() );
                ImGui::TableNextColumn();
                ImGui::TextUnformatted( t.image.c_str() );
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "%s\nsemantic %u", t.image.c_str(), (unsigned)t.semantic );
            }
            ImGui::EndTable();
        }
    }

    void DrawActions()
    {
        const bool dirty = s_cur.surf != s_cur.diskSurf || s_cur.contents != s_cur.diskContents
                        || s_cur.sort != s_cur.diskSort;
        ImGui::BeginDisabled( !dirty );
        if ( ImGui::Button( "Save", ImVec2( KMP_BTN, 0.0f ) ) )
            Save();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Write raw/materials/%s", s_cur.name.c_str() );
        ImGui::SameLine();
        if ( ImGui::Button( "Discard", ImVec2( KMP_BTN, 0.0f ) ) )
        {
            s_cur.surf     = s_cur.diskSurf;
            s_cur.contents = s_cur.diskContents;
            s_cur.sort     = s_cur.diskSort;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled( !s_cur.backupDiffers );
        if ( ImGui::Button( "Revert", ImVec2( KMP_BTN, 0.0f ) ) )
            Revert();
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "%s", s_cur.backup ? "Restore the file from before the first Save" : "No backup yet" );

        if ( !s_status.empty() )
        {
            ImGui::PushTextWrapPos( 0.0f );
            if ( s_statusErr )
                ImGui::TextColored( KMP_ERROR, "%s", s_status.c_str() );
            else
                ImGui::TextDisabled( "%s", s_status.c_str() );
            ImGui::PopTextWrapPos();
        }
    }

    void DrawBody()
    {
        int layer = g_qeglobals.current_edit_layer;
        layer = layer < 0 ? 0 : ( layer > 2 ? 2 : layer );
        qtexture_s *q = g_qeglobals.random_texture_stuff[layer].mtl.radMtl;
        if ( !q || !q->name || !q->name[0] || q->name[0] == '$' )
        {
            ImGui::TextDisabled( "No Material Selected" );
            return;
        }
        if ( s_cur.name != q->name )
        {
            Load( q->name );           // staged edits belong to the previous material
            s_status.clear();
        }

        Swatch( q );
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted( s_cur.name.c_str() );
        if ( s_cur.ok )
        {
            ImGui::TextDisabled( "%s", s_cur.techSet.c_str() );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Techset%s", s_cur.rawCopy ? "" : "\nPacked: Save writes a raw/materials copy" );
            DrawTypeAndSort();
        }
        ImGui::EndGroup();
        if ( !s_cur.ok )
        {
            ImGui::TextColored( KMP_ERROR, "%s", s_cur.error.c_str() );
            return;
        }
        if ( !s_cur.embeddedName.empty() && _stricmp( s_cur.embeddedName.c_str(), s_cur.name.c_str() ) )
            ImGui::TextColored( KMP_ERROR, "File names itself '%s'", s_cur.embeddedName.c_str() );

        DrawPenetration();
        ImGui::SeparatorText( "Surface flags" );
        BitGrid( "surf", kSurfBits, (int)( sizeof( kSurfBits ) / sizeof( kSurfBits[0] ) ),
                 &s_cur.surf, s_cur.diskSurf, SURF_TYPE_MASK );
        ImGui::SeparatorText( "Contents" );
        BitGrid( "contents", kContentBits, (int)( sizeof( kContentBits ) / sizeof( kContentBits[0] ) ),
                 &s_cur.contents, s_cur.diskContents, 0 );
        ImGui::SeparatorText( "Textures" );
        DrawTextures();
        ImGui::Separator();
        DrawActions();
    }
}

void KiwiMatProps_Draw()
{
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_MATPROPS );
    if ( !open || !*open )
        return;
    if ( KiwiWindows_JustOpened( KIWI_WIN_MATPROPS ) )
    {
        // Reopened: a tab beside the 2D View (above the Textures browser), else the main node.
        ImGuiWindow *xy = ImGui::FindWindowByName( KiwiWindows_Title( KIWI_WIN_XY ) );
        ImGui::SetNextWindowDockID( xy && xy->DockId ? xy->DockId : ImGuiShell_DockRoot(), ImGuiCond_Always );
    }
    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_MATPROPS ), open ) )
        DrawBody();
    ImGui::End();
}

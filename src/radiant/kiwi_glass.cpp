#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Breakable glass - see kiwi_glass.h.

#include "stdafx.h"
#include "qe3.h"

#include <imgui/imgui.h>

#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_gfx.h>
#include <d3d9.h>
#include <universal/com_files.h>
#include <universal/surfaceflags.h>

#include <glass/glass_kiwi_defaults.h>
#include <glass/glass_kiwi_slab.h>

#include "kiwi_glass.h"
#include "kiwi_command.h"
#include "kiwi_conselect.h"
#include "kiwi_matwriter.h"
#include "kiwi_selection.h"
#include "kiwi_windows.h"
#include "radiant_registry.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#ifndef CONTENTS_GLASS
#define CONTENTS_GLASS 0x10
#endif

extern int         Sys_Printf( const char *fmt, ... );
extern int         g_nUpdateBits;
extern entity_s   *world_entity;                                             // map.cpp
extern entity_s    entities;                                                 // entity.cpp (definition list)
extern entity_s    entityInsts;                                              // entity.cpp (instance list)
extern selbrush_t  selected_brushes;
extern selbrush_t  active_brushes;
extern eclass_t   *g_eclass;                                                 // eclass.cpp:117
extern eclass_t   *Eclass_InitFromText( void *textPtr );                     // eclass.cpp:792
extern void        Eclass_InsertAlphabetized( const char ***a1, const char **a2 ); // eclass.cpp:947
extern eclass_t   *Eclass_ForName( int has_brushes, const char *name );      // eclass.cpp:1090
extern entity_s   *Entity_Create( eclass_t *eclass );                        // entity.cpp:1660
extern void        Entity_Free( char *a1 );                                  // entity.cpp:1519
extern void        Entity_LinkBrush( brush_t *b, entity_s *world_ent );      // entity.cpp:469
extern void        Entity_UnlinkBrush( brush_t *b );                         // entity.cpp:503
extern selbrush_t *Entity_LinkBrush_0_extern( entity_s *e, entity_brush_s *b );// brush.cpp:639
extern void        Brush_BuildWindings( brush_t *def, int bFull );
extern void        SetupVertexSelection();
extern void        MarkMapModified();
extern void        sub_476330( selbrush_t *b );                              // brush.cpp:858 Brush_Deselect_Helper
extern void        sub_476470( selbrush_t *b );                              // brush.cpp:973 Brush_Select_Helper
extern void        sub_47B940( brush_t *def );                               // brush.cpp Brush_UpdateSpecialMaterialFlag
extern void        SetKeyValue( entity_s_def *e, const char *key, const char *value ); // entity.cpp:234
extern char       *ValueForKey2( const entity_s *e, const char *key );       // entity.cpp:91; "" when absent
extern void        Undo_ClearRedo();                                         // undo.cpp:176
extern void        Undo_GeneralStart( const char *operation );               // undo.cpp:367 (keeps the pointer)
extern void        Undo_AddBrush( entity_brush_s *pBrushInst );              // undo.cpp:494; takes brush def
extern void        Undo_AddEntity_W( entity_s *a1 );                         // undo.cpp:634
extern void        Undo_SetIdForEntity( entity_s_def *ent );                 // undo.cpp:664
extern void        Undo_End();                                               // undo.cpp:687
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1481
extern ImGuiID     ImGuiShell_DockRoot();                                    // imgui_shell.cpp
extern qtexture_s *Texture_GetHandle( const char *name );                    // texwnd.cpp:318 (registers)
extern int         TexWnd_MaterialCount();                                   // texwnd.cpp (end)
extern qtexture_s *TexWnd_MaterialAt( int idx );                             // texwnd.cpp (end)
extern void        TexWnd_MaterialDefFor( qtexture_s *q, MaterialDef *out ); // texwnd.cpp (end)
extern LayerMaterialDef *Materialdef_GetName( MaterialDef *m );              // 0x431640: the name

namespace
{
    const char *KGL_PROFILE = "KiwiGlass";
    const char *KGL_UG_KEY  = "kiwi_group";      // kiwi_outliner.cpp KOUT_UG_KEY

    // ── texture sets: raw/glass/<name> ──────────────────────────────────────
    // The same keys and defaults GlassKiwi_LoadDefFile reads (glass_kiwi_load.cpp).
    struct glassSet_t
    {
        std::string name;
        int         maxHealth    = 120;        // BO1's numbers, as GlassKiwi_SetDefDefaults
        float       minShardSize = 40.0f;
        float       maxShardSize = 200.0f;
        float       keep         = 0.5f;       // shardLifeProbability
        int         maxShards    = 0;
        float       uvScale      = 1.0f / 64.0f;
        int         frameEdges   = 1;
        std::string pristine, cracked, shard;  // blank = default (pristine: the pane's material)
        std::string crackSound, shatterSound, autoShatterSound, crackEffect, shatterEffect;
    };

    std::vector<glassSet_t> s_sets;              // sorted by name
    bool        s_scanned   = false;
    bool        s_loaded    = false;
    std::string s_active;                        // profile ActiveSet
    bool        s_focusNext = false;             // KiwiGlass_Focus: front the window next draw
    glassSet_t  s_edit;                          // the panel's working copy of the active set
    bool        s_editValid = false;
    char        s_filter[64] = "";
    char        s_newName[64] = "";
    char        s_status[256] = "";

    std::string ExeDir()
    {
        char exe[MAX_PATH] = { 0 };
        ::GetModuleFileNameA( nullptr, exe, MAX_PATH );
        char *slash = strrchr( exe, '\\' );
        if ( slash )
            *slash = '\0';
        return exe;
    }

    std::string SetPath( const std::string &name )
    {
        std::string rel = name;
        for ( size_t i = 0; i < rel.size(); ++i )
            if ( rel[i] == '/' )
                rel[i] = '\\';
        return ExeDir() + "\\raw\\glass\\" + rel;
    }

    void SetStatus( const char *fmt, ... )
    {
        va_list ap;
        va_start( ap, fmt );
        _vsnprintf( s_status, sizeof( s_status ) - 1, fmt, ap );
        va_end( ap );
        s_status[sizeof( s_status ) - 1] = '\0';
    }

    bool ParseSet( const std::string &text, glassSet_t &s )
    {
        if ( text.compare( 0, 8, "GLASSDEF" ) != 0 )
            return false;
        const char *p = text.c_str() + 8;
        while ( *p == '\\' )
        {
            const char *k = ++p;
            while ( *p && *p != '\\' ) ++p;
            const std::string key( k, p - k );
            if ( *p != '\\' )
                break;
            const char *v = ++p;
            while ( *p && *p != '\\' && *p != '\r' && *p != '\n' ) ++p;
            const std::string val( v, p - v );
            if      ( key == "maxHealth" )            s.maxHealth    = atoi( val.c_str() );
            else if ( key == "minShardSize" )         s.minShardSize = (float)atof( val.c_str() );
            else if ( key == "maxShardSize" )         s.maxShardSize = (float)atof( val.c_str() );
            else if ( key == "shardLifeProbability" ) s.keep         = (float)atof( val.c_str() );
            else if ( key == "maxShards" )            s.maxShards    = atoi( val.c_str() );
            else if ( key == "uvScale" )              s.uvScale      = (float)atof( val.c_str() );
            else if ( key == "frameEdges" )           s.frameEdges   = atoi( val.c_str() );
            else if ( key == "pristineMaterial" )     s.pristine     = val;
            else if ( key == "crackedMaterial" )      s.cracked      = val;
            else if ( key == "shardMaterial" )        s.shard        = val;
            else if ( key == "crackSound" )           s.crackSound   = val;
            else if ( key == "shatterSound" )         s.shatterSound = val;
            else if ( key == "autoShatterSound" )     s.autoShatterSound = val;
            else if ( key == "crackEffect" )          s.crackEffect  = val;
            else if ( key == "shatterEffect" )        s.shatterEffect = val;
        }
        return true;
    }

    std::string FormatSet( const glassSet_t &s )
    {
        char buf[64];
        std::string t = "GLASSDEF";
        auto num  = [&]( const char *k, const char *fmt, double v ) { _snprintf( buf, sizeof( buf ) - 1, fmt, v ); buf[sizeof( buf ) - 1] = '\0'; t += "\\"; t += k; t += "\\"; t += buf; };
        auto inum = [&]( const char *k, int v ) { _snprintf( buf, sizeof( buf ) - 1, "%i", v ); buf[sizeof( buf ) - 1] = '\0'; t += "\\"; t += k; t += "\\"; t += buf; };
        auto str  = [&]( const char *k, const std::string &v ) { if ( !v.empty() ) { t += "\\"; t += k; t += "\\"; t += v; } };
        inum( "maxHealth", s.maxHealth );
        num( "minShardSize", "%g", s.minShardSize );
        num( "maxShardSize", "%g", s.maxShardSize );
        num( "shardLifeProbability", "%g", s.keep );
        inum( "maxShards", s.maxShards );
        num( "uvScale", "%g", s.uvScale );
        inum( "frameEdges", s.frameEdges );
        str( "pristineMaterial", s.pristine );
        str( "crackedMaterial", s.cracked );
        str( "shardMaterial", s.shard );
        str( "crackSound", s.crackSound );
        str( "shatterSound", s.shatterSound );
        str( "autoShatterSound", s.autoShatterSound );
        str( "crackEffect", s.crackEffect );
        str( "shatterEffect", s.shatterEffect );
        return t;
    }

    bool ReadText( const std::string &path, std::string &out )
    {
        FILE *f = fopen( path.c_str(), "rb" );
        if ( !f )
            return false;
        char buf[4096];
        size_t n;
        out.clear();
        while ( ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 && out.size() < 65536 )
            out.append( buf, n );
        fclose( f );
        return true;
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

    void ScanDir( const std::string &dir, const std::string &rel, int depth )
    {
        if ( depth > 4 )
            return;
        WIN32_FIND_DATAA fd;
        HANDLE h = ::FindFirstFileA( ( dir + "\\*" ).c_str(), &fd );
        if ( h == INVALID_HANDLE_VALUE )
            return;
        do
        {
            const char *n = fd.cFileName;
            if ( n[0] == '.' )
                continue;
            const std::string name = rel.empty() ? std::string( n ) : rel + "/" + n;
            if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
            {
                ScanDir( dir + "\\" + n, name, depth + 1 );
                continue;
            }
            if ( strchr( n, '.' ) )                     // defs have no extension (.kiwitmp, notes)
                continue;
            std::string text;
            glassSet_t s;
            if ( !ReadText( dir + "\\" + n, text ) || !ParseSet( text, s ) )
                continue;
            s.name = name;
            s_sets.push_back( s );
        }
        while ( ::FindNextFileA( h, &fd ) );
        ::FindClose( h );
    }

    bool SetLess( const glassSet_t &a, const glassSet_t &b ) { return _stricmp( a.name.c_str(), b.name.c_str() ) < 0; }

    const glassSet_t *FindSet( const char *name )
    {
        if ( !name || !name[0] )
            name = GLASS_KIWI_DEFAULT_DEF;
        for ( size_t i = 0; i < s_sets.size(); ++i )
            if ( !_stricmp( s_sets[i].name.c_str(), name ) )
                return &s_sets[i];
        return nullptr;
    }

    void Load()
    {
        if ( s_loaded )
            return;
        s_loaded = true;
        s_active  = Radiant_ProfileGetString( KGL_PROFILE, "ActiveSet", GLASS_KIWI_DEFAULT_DEF );
    }

    void SaveProfile()
    {
        Radiant_ProfileSetString( KGL_PROFILE, "ActiveSet", s_active.c_str() );
    }

    void EditActive()
    {
        const glassSet_t *s = FindSet( s_active.c_str() );
        s_editValid = s != nullptr;
        s_edit = s ? *s : glassSet_t();
    }

    void Rescan()
    {
        s_sets.clear();
        ScanDir( ExeDir() + "\\raw\\glass", "", 0 );
        std::sort( s_sets.begin(), s_sets.end(), SetLess );
        s_scanned = true;
        EditActive();
    }

    void EnsureScanned()
    {
        Load();
        if ( !s_scanned )
            Rescan();
    }

    const char *ActiveName()
    {
        EnsureScanned();
        return s_active.empty() ? GLASS_KIWI_DEFAULT_DEF : s_active.c_str();
    }

    bool SameSet( const glassSet_t &a, const glassSet_t &b )
    {
        return FormatSet( a ) == FormatSet( b );
    }

    // ── entities ────────────────────────────────────────────────────────────
    eclass_t *GlassEclass()
    {
        for ( eclass_t *e = g_eclass; e; e = e->next )
            if ( e->name && !_stricmp( e->name, KIWI_GLASS_CLASSNAME ) )
                return e;
        return nullptr;
    }

    const char *DefClass( const entity_s_def *def )
    {
        return ( def && def->eclass && def->eclass->name ) ? def->eclass->name : "";
    }

    bool IsGlassDef( const entity_s_def *def )       { return !_stricmp( DefClass( def ), KIWI_GLASS_CLASSNAME ); }
    bool IsFuncGroupDef( const entity_s_def *def )   { return !_stricmp( DefClass( def ), "func_group" ); }

    entity_s_def *NodeDef( const selbrush_t *b )
    {
        if ( !b || !b->owner || !b->owner->def )
            return nullptr;
        return (entity_s_def *)b->owner->def;
    }

    bool IsPaneNode( const selbrush_t *b )
    {
        return b && b->owner && b->owner != world_entity && !b->owner->prefab && IsGlassDef( NodeDef( b ) );
    }

    int InstBrushCount( entity_s *inst )
    {
        int n = 0;
        for ( selbrush_t *b = inst->brushes.ownerNext; b && b != &inst->brushes; b = b->ownerNext )
            ++n;
        return n;
    }

    std::string Key( const entity_s_def *def, const char *key )
    {
        const char *v = def ? ValueForKey2( (const entity_s *)def, key ) : "";
        return v ? std::string( v ) : std::string();
    }

    void SetKeyIfChanged( entity_s_def *def, const char *key, const std::string &value )
    {
        if ( Key( def, key ) != value )
            SetKeyValue( def, key, value.c_str() );
    }

    // The outliner's relink recipe (kiwi_outliner.cpp RelinkBrush).
    void RelinkBrush( selbrush_t *sb, entity_s *targetInst, entity_s_def *targetDef )
    {
        const bool wasSelected = ( (unsigned)sb->brushFlags & 0x80u ) != 0;
        if ( wasSelected )
            sub_476330( sb );
        brush_t *bDef = sb->def;
        Entity_UnlinkBrush( bDef );
        Entity_LinkBrush( bDef, (entity_s *)targetDef );
        Entity_LinkBrush_0_extern( targetInst, sb );
        Brush_BuildWindings( bDef, 1 );
        if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
            SetupVertexSelection();
        MarkMapModified();
        ++bDef->version;
        if ( wasSelected )
            sub_476470( sb );
    }

    // ── slab geometry (the game's routine) ──────────────────────────────────
    const int KGL_MAX_FACES = 64;     // far past the game's 32 sides; CheckPane rejects more

    GlassKiwiSlabResult BrushSlab( const brush_t *def, GlassKiwiSlab *slab, int *nonAxial )
    {
        GlassKiwiSlabFace faces[KGL_MAX_FACES];
        const int n = (std::min)( def->faceCount, KGL_MAX_FACES );
        int axialLess = 0;
        for ( int i = 0; i < n; ++i )
        {
            const face_t *f = &def->faces[i];
            faces[i].pts      = f->w ? &f->w->p[0][0] : nullptr;
            faces[i].ptCount  = f->w ? (unsigned)f->w->numpoints : 0u;
            faces[i].ptStride = 3;
            faces[i].plane[0] = f->plane.normal[0];
            faces[i].plane[1] = f->plane.normal[1];
            faces[i].plane[2] = f->plane.normal[2];
            faces[i].plane[3] = f->plane.dist;
            const float *nrm = f->plane.normal;
            const bool axial = fabsf( nrm[0] ) > 0.99999f || fabsf( nrm[1] ) > 0.99999f || fabsf( nrm[2] ) > 0.99999f;
            if ( !axial && faces[i].ptCount >= 3 )
                ++axialLess;
        }
        if ( nonAxial )
            *nonAxial = axialLess;
        return GlassKiwi_BuildSlab( faces, (unsigned)n, nullptr, slab );
    }

    // The front face's horizontal texture repeat as the game's shard UV scale (one scalar for
    // u and v, glass_shard.cpp; size = world units per repeat, 64 = the sets' 1/64).
    bool FrontUvScale( const brush_t *def, std::string &out )
    {
        GlassKiwiSlab slab;
        if ( BrushSlab( def, &slab, nullptr ) == GLASS_SLAB_NO_FACES )
            return false;
        const texdef_sub_t &td = def->faces[slab.frontFace].mtldef[0].mat_texDef;
        const float sx = td.size[0] != 0.0f ? fabsf( td.size[0] ) : 128.0f;
        char buf[32];
        _snprintf( buf, sizeof( buf ) - 1, "%g", 1.0 / (double)sx );
        buf[sizeof( buf ) - 1] = '\0';
        out = buf;
        return true;
    }

    const brush_t *FirstBrush( const entity_s_def *def )
    {
        const brush_t *sentinel = (const brush_t *)&def->def;
        const brush_t *b = (const brush_t *)def->brushes.prev;
        return ( b && b != sentinel ) ? b : nullptr;
    }

    int DefBrushCount( const entity_s_def *def, int *patches )
    {
        int n = 0;
        if ( patches )
            *patches = 0;
        const brush_t *sentinel = (const brush_t *)&def->def;
        for ( const brush_t *b = (const brush_t *)def->brushes.prev; b && b != sentinel; b = b->onext )
        {
            ++n;
            if ( b->patch && patches )
                ++*patches;
        }
        return n;
    }

    // ── materials ───────────────────────────────────────────────────────────
    struct matInfo_t
    {
        bool found  = false;
        bool glass  = false;     // CONTENTS_GLASS + glass surface type
        bool world  = false;     // techsets/wc_<ts> exists (pane / cracked)
        bool model  = false;     // techsets/mc_<ts> exists (shards)
    };
    std::map<std::string, matInfo_t> s_matInfo;

    bool TechsetExists( const char *prefix, const char *techSet )
    {
        char path[256];
        _snprintf( path, sizeof( path ) - 1, "techsets/%s%s.techset", prefix, techSet );
        path[sizeof( path ) - 1] = '\0';
        int h = 0;
        FS_FOpenFileRead( path, &h );
        if ( !h )
            return false;
        FS_FCloseFile( h );
        return true;
    }

    const matInfo_t &MatInfo( const std::string &name )
    {
        std::map<std::string, matInfo_t>::iterator it = s_matInfo.find( name );
        if ( it != s_matInfo.end() )
            return it->second;
        matInfo_t &mi = s_matInfo[name];
        kiwiMatSource_t src;
        char err[256];
        if ( !KiwiMat_ReadSource( name.c_str(), &src, err, sizeof( err ) ) )
            return mi;
        mi.found = true;
        mi.glass = ( src.contents & CONTENTS_GLASS ) && ( src.surfaceFlags & SURF_TYPE_MASK ) == SURF_TYPE_GLASS;
        mi.world = TechsetExists( "wc_", src.techSet );    // the runtime's "wc/" + name -> "wc_" + techset
        mi.model = TechsetExists( "mc_", src.techSet );
        return mi;
    }

    std::string FaceMaterial( const face_t *f )
    {
        const char *n = (const char *)Materialdef_GetName( (MaterialDef *)&f->mtldef[0] );
        return n ? std::string( n ) : std::string();
    }

    // name -> browser entry, rebuilt when the browser's list changes
    std::map<std::string, qtexture_s *> s_browser;
    int s_browserCount = -1;

    qtexture_s *BrowserMaterial( const std::string &name )
    {
        const int count = TexWnd_MaterialCount();
        if ( count != s_browserCount )
        {
            s_browser.clear();
            for ( int i = 0; i < count; ++i )
                if ( qtexture_s *q = TexWnd_MaterialAt( i ) )
                    if ( q->name )
                        s_browser[q->name] = q;
            s_browserCount = count;
        }
        std::map<std::string, qtexture_s *>::iterator it = s_browser.find( name );
        return it != s_browser.end() ? it->second : nullptr;
    }

    // The colormap as a D3D texture, or null (kiwi_decal.cpp ColorMap).
    IDirect3DTexture9 *ColorMap( qtexture_s *q )
    {
        if ( !q || !q->next )
            return nullptr;
        Material *mtl = q->next;
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

    bool FaceIsGlass( const face_t *f )
    {
        return MatInfo( FaceMaterial( f ) ).glass;
    }

    // Retexture the base layer.  A face that already wears a glass material keeps its own
    // alignment; any other face ($default, caulk, a wall texture) takes the material's
    // natural scale, as a Textures browser click would give it.  onlyNonGlass = the
    // fallback pane material, which leaves glass faces alone.
    void Retexture( brush_t *def, qtexture_s *q, bool onlyNonGlass )
    {
        MaterialDef md;
        TexWnd_MaterialDefFor( q, &md );
        for ( int i = 0; i < def->faceCount; ++i )
        {
            MaterialDef &face = def->faces[i].mtldef[0];
            if ( FaceIsGlass( &def->faces[i] ) )
            {
                if ( onlyNonGlass )
                    continue;
                face.lyrMtl = md.lyrMtl;
                face.radMtl = md.radMtl;
            }
            else
                face = md;
        }
        Brush_BuildWindings( def, 1 );
        MarkMapModified();
        ++def->version;
        sub_47B940( def );
    }

    // The editor's pane material for faces that are not glass when the set names none
    // (the game then keeps the compiled faces' material).
    qtexture_s *FallbackPaneMaterial()
    {
        qtexture_s *q = BrowserMaterial( GLASS_KIWI_DEFAULT_PANE_MATERIAL );
        if ( !q || !q->next )
            q = Texture_GetHandle( GLASS_KIWI_DEFAULT_PANE_MATERIAL );
        return q && q->next ? q : nullptr;
    }

    // The set's pane material, loaded; null = keep the faces' materials.
    qtexture_s *PristineFor( const glassSet_t *set )
    {
        if ( !set || set->pristine.empty() )
            return nullptr;
        qtexture_s *q = BrowserMaterial( set->pristine );
        if ( !q || !q->next )
            q = Texture_GetHandle( set->pristine.c_str() );
        if ( !q || !q->next )
        {
            Sys_Printf( "Glass: pane material %s does not load; faces keep their materials.\n", set->pristine.c_str() );
            return nullptr;
        }
        return q;
    }

    // A pane entity takes `set`: glassdef, the set's pane material on every face (else
    // `fallback` on the faces that are not glass: bullets only see a pane through a glass
    // material), uvscale from its front face.
    void ApplyToPane( entity_s_def *def, const char *setName, qtexture_s *pristine, qtexture_s *fallback )
    {
        SetKeyIfChanged( def, "glassdef", setName );
        brush_t *sentinel = (brush_t *)&def->def;
        for ( brush_t *b = (brush_t *)def->brushes.prev; b && b != sentinel; b = b->onext )
        {
            if ( b->patch )
                continue;
            if ( pristine )
                Retexture( b, pristine, false );
            else if ( fallback )
                Retexture( b, fallback, true );
        }
        if ( const brush_t *b = FirstBrush( def ) )
        {
            std::string uv;
            if ( FrontUvScale( b, uv ) )
                SetKeyIfChanged( def, "uvscale", uv );
        }
    }

    // ── selection ───────────────────────────────────────────────────────────
    struct glassPick_t
    {
        std::vector<selbrush_t *> brushes;   // unique, selection order
        int         panes   = 0;             // brushes of glass entities
        int         plain   = 0;             // worldspawn / func_group brushes
        int         other   = 0;             // brushes of other brush entities
        const char *blocked = nullptr;       // why Make is unavailable
    };

    void Gather( glassPick_t &p )
    {
        const selection_t &sel = KiwiSel();
        std::set<selbrush_t *> seen;
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            selbrush_t *b = sel.items[i].brush;
            if ( !b || !Sel_BrushLive( b ) || seen.count( b ) )
                continue;
            seen.insert( b );
            entity_s_def *def = NodeDef( b );
            if ( ( b->def && b->def->patch ) || b->patch )
            {
                p.blocked = "Patches cannot become glass";
                continue;
            }
            if ( b->owner && b->owner->prefab )
            {
                p.blocked = "Prefab contents cannot become glass";
                continue;
            }
            if ( def && def->eclass && def->eclass->fixedsize )
            {
                p.blocked = "Models, lights and other point entities cannot become glass";
                continue;
            }
            p.brushes.push_back( b );
            if ( b->owner == world_entity || IsFuncGroupDef( def ) )
                ++p.plain;
            else if ( IsGlassDef( def ) )
                ++p.panes;
            else
                ++p.other;
        }
        if ( KiwiConSel_Count() > 0 )
            p.blocked = "Construction lines cannot become glass";
    }

    void SelectNodes( const std::vector<selbrush_t *> &nodes )
    {
        selection_t &sel = KiwiSel();
        Sel_Clear( sel );
        for ( size_t i = 0; i < nodes.size(); ++i )
            if ( Sel_BrushLive( nodes[i] ) )
                Sel_Add( sel, Sel_MakeObject( nodes[i] ) );
        Sel_SyncToLegacy();
        g_nUpdateBits = -1;
    }

    void RestoreSelection( const std::vector<sel_item_t> &items );

    // Empty the selection for Entity_Create (it merges into / refuses over selected brushes).
    bool ParkSelection( const std::vector<sel_item_t> &keep )
    {
        Sel_Clear( KiwiSel() );
        Sel_SyncToLegacy();
        if ( selected_brushes.next == &selected_brushes )
            return true;
        Sys_Printf( "Glass: could not clear the selection; nothing changed.\n" );
        RestoreSelection( keep );
        return false;
    }

    void RestoreSelection( const std::vector<sel_item_t> &items )
    {
        Sel_InvalidateFromLegacy();
        selection_t &sel = KiwiSel();
        Sel_Clear( sel );
        for ( size_t i = 0; i < items.size(); ++i )
            if ( Sel_BrushLive( items[i].brush ) )
                Sel_Add( sel, items[i] );
        Sel_SyncToLegacy();
        g_nUpdateBits = -1;
    }

    // Every pane node on screen (selected or not, not hidden or filtered out).
    void PaneNodes( std::vector<selbrush_t *> &out )
    {
        selbrush_t *lists[2] = { &active_brushes, &selected_brushes };
        for ( int l = 0; l < 2; ++l )
            for ( selbrush_t *b = lists[l]->next; b && b != lists[l]; b = b->next )
                if ( IsPaneNode( b ) )
                    out.push_back( b );
    }

    entity_s *InstanceOf( entity_s_def *def )
    {
        for ( entity_s *e = entityInsts.next; e && e != &entityInsts; e = e->next )
            if ( e->def == (entity_s *)def && !e->prefab )
                return e;
        return nullptr;
    }

    // ── validation ──────────────────────────────────────────────────────────
    struct paneStatus_t
    {
        entity_s_def *def = nullptr;
        int           errors = 0;
        int           warnings = 0;
        std::string   text;
    };

    void Problem( paneStatus_t &st, bool error, const char *fmt, ... )
    {
        char buf[256];
        va_list ap;
        va_start( ap, fmt );
        _vsnprintf( buf, sizeof( buf ) - 1, fmt, ap );
        va_end( ap );
        buf[sizeof( buf ) - 1] = '\0';
        if ( error ) ++st.errors; else ++st.warnings;
        if ( !st.text.empty() )
            st.text += "; ";
        st.text += buf;
    }

    void CheckMaterial( paneStatus_t &st, const char *role, const std::string &name, bool model )
    {
        const matInfo_t &mi = MatInfo( name );
        if ( !mi.found )
            Problem( st, true, "%s material %s not found", role, name.c_str() );
        else if ( model ? !mi.model : !mi.world )
            Problem( st, true, "%s material %s has no %s technique set", role, name.c_str(), model ? "model (mc_)" : "world (wc_)" );
    }

    void CheckPane( entity_s_def *def, paneStatus_t &st )
    {
        st = paneStatus_t();
        st.def = def;
        int patches = 0;
        const int count = DefBrushCount( def, &patches );
        if ( count != 1 || patches )
        {
            Problem( st, true, patches ? "holds a patch" : "has %i brushes, a pane is one brush", count );
            return;
        }
        const brush_t *b = FirstBrush( def );
        if ( b->faceCount > KGL_MAX_FACES )
        {
            Problem( st, true, "too many sides (%i, max %i)", b->faceCount, GLASS_KIWI_MAX_POLYS - 6 );
            return;
        }
        GlassKiwiSlab slab;
        int nonAxial = 0;
        switch ( BrushSlab( b, &slab, &nonAxial ) )
        {
        case GLASS_SLAB_OK:
            break;
        case GLASS_SLAB_NO_FACES:
            Problem( st, true, "brush has no faces" );
            return;
        case GLASS_SLAB_NOT_SLAB:
            Problem( st, true, "not a flat slab: back face %.1f deg off parallel (max %.0f)", slab.wedgeDeg, GLASS_KIWI_MAX_WEDGE_DEG );
            return;
        case GLASS_SLAB_THICKNESS:
            Problem( st, true, "%.2f units thick, must be %.1f..%.0f", slab.thickness, GLASS_KIWI_MIN_THICKNESS, GLASS_KIWI_MAX_THICKNESS );
            return;
        case GLASS_SLAB_POINT_COUNT:
            Problem( st, true, "front face has %u points, must be 3..%d", slab.numPts, GLASS_KIWI_MAX_OUTLINE );
            return;
        case GLASS_SLAB_NO_AREA:
            Problem( st, true, "front face has no area" );
            return;
        }
        if ( nonAxial + 6 > GLASS_KIWI_MAX_POLYS )
            Problem( st, true, "too many sides (%i, max %i)", nonAxial, GLASS_KIWI_MAX_POLYS - 6 );
        if ( slab.frontArea < 4.0f )
            Problem( st, false, "front face is only %.1f square units", slab.frontArea );

        const std::string setName = Key( def, "glassdef" );
        const glassSet_t *set = FindSet( setName.c_str() );
        if ( !set )
            Problem( st, true, "glass set %s not found in raw/glass", setName.empty() ? GLASS_KIWI_DEFAULT_DEF : setName.c_str() );

        const std::string face = FaceMaterial( &b->faces[slab.frontFace] );
        const matInfo_t &fi = MatInfo( face );
        if ( !fi.found )
            Problem( st, true, "face material %s not found", face.c_str() );
        else if ( !fi.glass )
            Problem( st, true, "face material %s is not glass (bullets and players ignore the pane)", face.c_str() );
        if ( set )
        {
            if ( !set->pristine.empty() && _stricmp( set->pristine.c_str(), face.c_str() ) )
                Problem( st, false, "faces use %s, the set's pane material is %s", face.c_str(), set->pristine.c_str() );
            CheckMaterial( st, "pane", set->pristine.empty() ? face : set->pristine, false );
            CheckMaterial( st, "cracked", set->cracked.empty() ? std::string( GLASS_KIWI_DEFAULT_CRACKED_MATERIAL ) : set->cracked, false );
            CheckMaterial( st, "shard", set->shard.empty() ? std::string( GLASS_KIWI_DEFAULT_SHARD_MATERIAL ) : set->shard, true );
        }
        float ang[3] = { 0.0f, 0.0f, 0.0f };
        const std::string angles = Key( def, "angles" );
        if ( !angles.empty() && sscanf( angles.c_str(), "%f %f %f", &ang[0], &ang[1], &ang[2] ) == 3
          && ( ang[0] != 0.0f || ang[1] != 0.0f || ang[2] != 0.0f ) )
            Problem( st, false, "has an angles key (ignored)" );   // the game's test
    }

    struct mapCheck_t
    {
        std::vector<paneStatus_t> panes;
        int    errors = 0, warnings = 0, bad = 0;
        int    brushEntities = 0;          // non-dyn_ submodels cod4map will make
        double time = -1.0;
    };
    mapCheck_t s_map;

    void CheckMap( bool force )
    {
        const double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
        if ( !force && s_map.time >= 0.0 && now - s_map.time < 0.5 )
            return;
        EnsureScanned();
        s_map = mapCheck_t();
        s_map.time = now;
        for ( entity_s *e = entities.next; e && e != &entities; e = e->next )
        {
            entity_s_def *def = (entity_s_def *)e;
            if ( !def->eclass || def->eclass->fixedsize || !FirstBrush( def ) )
                continue;
            const char *cn = DefClass( def );
            if ( _stricmp( cn, "worldspawn" ) && _stricmp( cn, "func_group" ) && _strnicmp( cn, "dyn_", 4 ) )
                ++s_map.brushEntities;
            if ( !IsGlassDef( def ) )
                continue;
            paneStatus_t st;
            CheckPane( def, st );
            s_map.errors += st.errors;
            s_map.warnings += st.warnings;
            if ( st.errors )
                ++s_map.bad;
            s_map.panes.push_back( st );
        }
        if ( (int)s_map.panes.size() > GLASS_KIWI_MAX_PANES - 1 )
            ++s_map.errors;
        if ( s_map.brushEntities > 1023 )
            ++s_map.errors;
    }

    const char *MapLimitText()
    {
        static char buf[160];
        buf[0] = '\0';
        if ( (int)s_map.panes.size() > GLASS_KIWI_MAX_PANES - 1 )
            _snprintf( buf, sizeof( buf ) - 1, "%i panes, the game takes %i", (int)s_map.panes.size(), GLASS_KIWI_MAX_PANES - 1 );
        else if ( s_map.brushEntities > 1023 )
            _snprintf( buf, sizeof( buf ) - 1, "%i brush entities, cod4map takes 1023", s_map.brushEntities );
        return buf;
    }

    void PaneLabel( const entity_s_def *def, char *out, size_t outSize )
    {
        const brush_t *b = FirstBrush( def );
        float c[3] = { 0, 0, 0 };
        if ( b )
            for ( int k = 0; k < 3; ++k )
                c[k] = 0.5f * ( b->mins[k] + b->maxs[k] );
        _snprintf( out, outSize - 1, "glass %i (%.0f %.0f %.0f)", def->numberId, c[0], c[1], c[2] );
        out[outSize - 1] = '\0';
    }

    // Right after Make / Apply: say which panes the game will drop, and why.
    void ReportPanes( const std::vector<entity_s_def *> &defs )
    {
        int bad = 0;
        for ( size_t i = 0; i < defs.size(); ++i )
        {
            paneStatus_t st;
            CheckPane( defs[i], st );
            if ( !st.errors && !st.warnings )
                continue;
            char label[96];
            PaneLabel( defs[i], label, sizeof( label ) );
            Sys_Printf( "Glass: %s: %s%s\n", label, st.text.c_str(),
                        st.errors ? " - it will not break in game" : "" );
            bad += st.errors ? 1 : 0;
        }
        if ( bad )
            Sys_Printf( "Glass: %i of %i pane(s) have errors; the Glass window lists them.\n", bad, (int)defs.size() );
    }

    void SelectPanes( bool (*want)( const entity_s_def * ) )
    {
        std::vector<selbrush_t *> all, nodes;
        PaneNodes( all );
        for ( size_t i = 0; i < all.size(); ++i )
            if ( want( NodeDef( all[i] ) ) )
                nodes.push_back( all[i] );
        SelectNodes( nodes );
    }

    bool UsesActive( const entity_s_def *def )
    {
        const std::string k = Key( def, "glassdef" );
        return !_stricmp( k.empty() ? GLASS_KIWI_DEFAULT_DEF : k.c_str(), ActiveName() );
    }

    bool IsInvalid( const entity_s_def *def )
    {
        for ( size_t i = 0; i < s_map.panes.size(); ++i )
            if ( s_map.panes[i].def == def )
                return s_map.panes[i].errors > 0;
        return false;
    }

    // Retexture every pane that uses `name` (its pane material changed), one record.
    void RetextureSetUsers( const glassSet_t &set )
    {
        qtexture_s *q = PristineFor( &set );
        if ( !q )
            return;
        std::vector<entity_s_def *> defs;
        for ( entity_s *e = entities.next; e && e != &entities; e = e->next )
        {
            entity_s_def *def = (entity_s_def *)e;
            const std::string k = Key( def, "glassdef" );
            if ( IsGlassDef( def ) && FirstBrush( def ) && !_stricmp( k.empty() ? GLASS_KIWI_DEFAULT_DEF : k.c_str(), set.name.c_str() ) )
                defs.push_back( def );
        }
        if ( defs.empty() )
            return;
        static const char *op = "retexture glass set";
        Undo_ClearRedo();
        Undo_GeneralStart( op );
        for ( size_t i = 0; i < defs.size(); ++i )
            Undo_AddEntity_W( (entity_s *)defs[i] );
        for ( size_t i = 0; i < defs.size(); ++i )
            ApplyToPane( defs[i], set.name.c_str(), q, nullptr );
        Undo_End();
        g_nUpdateBits = -1;
        Sys_Printf( "Glass: retextured %i pane(s) using %s\n", (int)defs.size(), set.name.c_str() );
    }
}

// ── eclass ─────────────────────────────────────────────────────────────────────
void KiwiGlass_RegisterEclass()
{
    if ( GlassEclass() )
        return;
    // Eclass_InitFromText reads from "/*QUAKED" + 9; "?" = a brush entity.
    static char text[] =
        "/*QUAKED glass (0.45 0.8 1) ?\n"
        "Breakable glass pane (KIWI): one convex slab brush, 0.1 to 16 units thick (back face within 3 degrees of parallel), at most 22 points on its largest face.\n"
        "\"glassdef\" texture set under raw/glass (pane, cracked and shard materials, health, shard sizes); default default_clear.\n"
        "\"uvscale\" shard texture scale; written from the largest face on every save.\n"
        "\"frameedges\" 1 = the edges are held by a frame (edge pieces stick, then drop), 0 = everything falls.\n"
        "Made with Make Breakable Glass; the Glass window edits the sets.\n"
        "*/";
    eclass_t *ec = Eclass_InitFromText( text );
    if ( ec )
        Eclass_InsertAlphabetized( (const char ***)&g_eclass, (const char **)ec );
}

// ── commands ───────────────────────────────────────────────────────────────────
bool KiwiGlass_CanMake()
{
    glassPick_t p;
    Gather( p );
    return !p.blocked && p.plain + p.panes > 0;
}

bool KiwiGlass_CanUnmake()
{
    glassPick_t p;
    Gather( p );
    return p.panes > 0;
}

bool KiwiGlass_CanApply()
{
    return KiwiGlass_CanUnmake();
}

bool KiwiGlass_Make()
{
    glassPick_t p;
    Gather( p );
    if ( p.blocked )
    {
        Sys_Printf( "Glass: %s.\n", p.blocked );
        return false;
    }
    eclass_t *ec = GlassEclass();
    if ( !ec )
    {
        Sys_Printf( "Glass: the glass entity class is not registered.\n" );
        return false;
    }
    const std::string setName = ActiveName();
    const glassSet_t *set = FindSet( setName.c_str() );
    if ( !set )
        Sys_Printf( "Glass: set %s is not in raw/glass; the game will use its defaults.\n", setName.c_str() );
    qtexture_s *pristine = PristineFor( set );       // registers before the record opens
    qtexture_s *fallback = pristine ? nullptr : FallbackPaneMaterial();

    std::vector<selbrush_t *> make;
    std::vector<entity_s_def *> reset;
    int skipped = 0;
    for ( size_t i = 0; i < p.brushes.size(); ++i )
    {
        selbrush_t *b = p.brushes[i];
        entity_s_def *def = NodeDef( b );
        if ( b->owner == world_entity || IsFuncGroupDef( def ) )
            make.push_back( b );
        else if ( IsGlassDef( def ) )
        {
            if ( std::find( reset.begin(), reset.end(), def ) == reset.end() )
                reset.push_back( def );
        }
        else
            ++skipped;
    }
    if ( skipped )
        Sys_Printf( "Glass: skipped %i brush(es) of other entities; only worldspawn and func_group brushes become glass.\n", skipped );
    if ( make.empty() && reset.empty() )
        return false;

    // Entity_Create reads the legacy selection, so each pane is born on an empty one.
    const std::vector<sel_item_t> keep = KiwiSel().items;
    if ( !ParkSelection( keep ) )
        return false;

    static const char *op = "make breakable glass";
    Undo_ClearRedo();
    Undo_GeneralStart( op );

    // Record before any change: func_groups this drains whole (freed after), panes that
    // only take the set whole, every other moving brush on its own.
    std::vector<entity_s *> drained;
    for ( size_t i = 0; i < make.size(); ++i )
    {
        entity_s *owner = make[i]->owner;
        if ( owner == world_entity || std::find( drained.begin(), drained.end(), owner ) != drained.end() )
            continue;
        int leaving = 0;
        for ( size_t k = 0; k < make.size(); ++k )
            if ( make[k]->owner == owner )
                ++leaving;
        if ( leaving == InstBrushCount( owner ) )
            drained.push_back( owner );
    }
    for ( size_t i = 0; i < drained.size(); ++i )
        Undo_AddEntity_W( drained[i]->def );
    for ( size_t i = 0; i < reset.size(); ++i )
        Undo_AddEntity_W( (entity_s *)reset[i] );
    for ( size_t i = 0; i < make.size(); ++i )
        if ( std::find( drained.begin(), drained.end(), make[i]->owner ) == drained.end() )
            Undo_AddBrush( (entity_brush_s *)make[i]->def );

    int made = 0;
    std::vector<entity_s_def *> panes = reset;
    for ( size_t i = 0; i < make.size(); ++i )
    {
        selbrush_t *b = make[i];
        const std::string group = b->owner != world_entity ? Key( NodeDef( b ), KGL_UG_KEY ) : std::string();
        entity_s *inst = Entity_Create( ec );        // empty selection + brush class = empty entity
        entity_s_def *def = inst ? (entity_s_def *)inst->def : nullptr;
        if ( !def )
            continue;
        Undo_SetIdForEntity( def );
        if ( !group.empty() )
            SetKeyValue( def, KGL_UG_KEY, group.c_str() );   // stays in its outliner group
        RelinkBrush( b, inst, def );
        ApplyToPane( def, setName.c_str(), pristine, fallback );
        panes.push_back( def );
        ++made;
    }
    for ( size_t i = 0; i < reset.size(); ++i )
        ApplyToPane( reset[i], setName.c_str(), pristine, fallback );
    for ( size_t i = 0; i < drained.size(); ++i )
        Entity_Free( (char *)drained[i] );
    Undo_End();

    RestoreSelection( keep );
    s_map.time = -1.0;
    Sys_Printf( "Glass: %i new pane(s), %i pane(s) set to %s.\n", made, (int)reset.size(), setName.c_str() );
    ReportPanes( panes );
    return made > 0 || !reset.empty();
}

bool KiwiGlass_Unmake()
{
    glassPick_t p;
    Gather( p );
    std::vector<selbrush_t *> move;
    for ( size_t i = 0; i < p.brushes.size(); ++i )
        if ( IsPaneNode( p.brushes[i] ) )
            move.push_back( p.brushes[i] );
    if ( move.empty() )
        return false;

    const std::vector<sel_item_t> keep = KiwiSel().items;
    if ( !ParkSelection( keep ) )
        return false;

    static const char *op = "make regular brush";
    Undo_ClearRedo();
    Undo_GeneralStart( op );

    // Panes this empties are recorded whole and freed; a brush leaving a pane that keeps
    // others (an invalid multi-brush pane) is recorded on its own.
    std::vector<entity_s *> drained;
    for ( size_t i = 0; i < move.size(); ++i )
    {
        entity_s *owner = move[i]->owner;
        if ( std::find( drained.begin(), drained.end(), owner ) != drained.end() )
            continue;
        int leaving = 0;
        for ( size_t k = 0; k < move.size(); ++k )
            if ( move[k]->owner == owner )
                ++leaving;
        if ( leaving == InstBrushCount( owner ) )
            drained.push_back( owner );
    }
    for ( size_t i = 0; i < drained.size(); ++i )
        Undo_AddEntity_W( drained[i]->def );
    for ( size_t i = 0; i < move.size(); ++i )
        if ( std::find( drained.begin(), drained.end(), move[i]->owner ) == drained.end() )
            Undo_AddBrush( (entity_brush_s *)move[i]->def );

    // A pane in an outliner group goes back into that group's func_group.
    std::map<std::string, entity_s *> wrappers;
    entity_s_def *worldDef = (entity_s_def *)world_entity->def;
    for ( size_t i = 0; i < move.size(); ++i )
    {
        selbrush_t *b = move[i];
        const std::string group = Key( NodeDef( b ), KGL_UG_KEY );
        entity_s *targetInst = world_entity;
        if ( !group.empty() )
        {
            entity_s *&w = wrappers[group];
            if ( !w )
            {
                for ( entity_s *e = entities.next; e && e != &entities && !w; e = e->next )
                    if ( IsFuncGroupDef( (entity_s_def *)e ) && Key( (entity_s_def *)e, KGL_UG_KEY ) == group )
                        w = InstanceOf( (entity_s_def *)e );
                if ( !w )
                {
                    eclass_t *fg = Eclass_ForName( 0, "func_group" );
                    w = fg ? Entity_Create( fg ) : nullptr;
                    if ( w && w->def )
                    {
                        Undo_SetIdForEntity( (entity_s_def *)w->def );
                        SetKeyValue( (entity_s_def *)w->def, KGL_UG_KEY, group.c_str() );
                    }
                }
            }
            if ( w && w->def )
                targetInst = w;
        }
        RelinkBrush( b, targetInst, targetInst == world_entity ? worldDef : (entity_s_def *)targetInst->def );
    }
    for ( size_t i = 0; i < drained.size(); ++i )
        Entity_Free( (char *)drained[i] );
    Undo_End();

    RestoreSelection( keep );
    s_map.time = -1.0;
    Sys_Printf( "Glass: %i brush(es) are regular brushes again.\n", (int)move.size() );
    return true;
}

bool KiwiGlass_ApplySetToSelection()
{
    glassPick_t p;
    Gather( p );
    std::vector<entity_s_def *> defs;
    for ( size_t i = 0; i < p.brushes.size(); ++i )
        if ( IsPaneNode( p.brushes[i] ) )
        {
            entity_s_def *def = NodeDef( p.brushes[i] );
            if ( std::find( defs.begin(), defs.end(), def ) == defs.end() )
                defs.push_back( def );
        }
    if ( defs.empty() )
        return false;
    const std::string setName = ActiveName();
    qtexture_s *pristine = PristineFor( FindSet( setName.c_str() ) );
    qtexture_s *fallback = pristine ? nullptr : FallbackPaneMaterial();

    static const char *op = "apply glass set";
    Undo_ClearRedo();
    Undo_GeneralStart( op );
    for ( size_t i = 0; i < defs.size(); ++i )
        Undo_AddEntity_W( (entity_s *)defs[i] );
    for ( size_t i = 0; i < defs.size(); ++i )
        ApplyToPane( defs[i], setName.c_str(), pristine, fallback );
    Undo_End();
    g_nUpdateBits = -1;
    s_map.time = -1.0;
    Sys_Printf( "Glass: %i pane(s) set to %s.\n", (int)defs.size(), setName.c_str() );
    ReportPanes( defs );
    return true;
}

void KiwiGlass_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiGlassMake",   0, 0, KIWI_CMD_GLASS_MAKE );
    Radiant_RegisterCommand( "KiwiGlassUnmake", 0, 0, KIWI_CMD_GLASS_UNMAKE );
    Radiant_RegisterCommand( "KiwiGlassApply",  0, 0, KIWI_CMD_GLASS_APPLY );
    Radiant_RegisterCommand( "KiwiWindowGlass", 0, 0, KIWI_CMD_WINDOW_GLASS );
}

bool KiwiGlass_DispatchInstant( unsigned int cmdId )
{
    switch ( cmdId )
    {
    case KIWI_CMD_GLASS_MAKE:   KiwiGlass_Make();   return true;
    case KIWI_CMD_GLASS_UNMAKE: KiwiGlass_Unmake(); return true;
    case KIWI_CMD_GLASS_APPLY:  KiwiGlass_ApplySetToSelection(); return true;
    default:                    return false;
    }
}

// ── test DSL ───────────────────────────────────────────────────────────────────
bool KiwiGlass_SetActiveSet( const char *name )
{
    Rescan();
    if ( !name || !FindSet( name ) )
        return false;
    s_active = FindSet( name )->name;
    SaveProfile();
    EditActive();
    return true;
}

int KiwiGlass_Validate( bool log, int *outWarnings, int *outPanes )
{
    s_matInfo.clear();
    CheckMap( true );
    if ( log )
    {
        for ( size_t i = 0; i < s_map.panes.size(); ++i )
        {
            const paneStatus_t &st = s_map.panes[i];
            if ( !st.errors && !st.warnings )
                continue;
            char label[96];
            PaneLabel( st.def, label, sizeof( label ) );
            Sys_Printf( "Glass: %s: %s\n", label, st.text.c_str() );
        }
        if ( MapLimitText()[0] )
            Sys_Printf( "Glass: %s\n", MapLimitText() );
        Sys_Printf( "Glass: %i pane(s), %i error(s), %i warning(s).\n", (int)s_map.panes.size(), s_map.errors, s_map.warnings );
    }
    if ( outWarnings )
        *outWarnings = s_map.warnings;
    if ( outPanes )
        *outPanes = (int)s_map.panes.size();
    return s_map.errors;
}

// ── map save ───────────────────────────────────────────────────────────────────
void KiwiGlass_BeforeMapSave()
{
    for ( entity_s *e = entities.next; e && e != &entities; e = e->next )
    {
        entity_s_def *def = (entity_s_def *)e;
        if ( !IsGlassDef( def ) )
            continue;
        const brush_t *b = FirstBrush( def );
        std::string uv;
        if ( b && !b->patch && FrontUvScale( b, uv ) )
            SetKeyIfChanged( def, "uvscale", uv );
    }
}

// ── panel ──────────────────────────────────────────────────────────────────────
namespace
{
    std::set<std::string> s_thumbTried;
    int s_thumbBudget = 0;

    // The material's colour map over the item just submitted (rect p .. p+size).  Only browser
    // entries load; `load` = false for shard (model) materials so the Glass window never
    // registers anything new into the Textures browser.
    void DrawSwatch( const ImVec2 &p, float size, const std::string &name, bool load )
    {
        qtexture_s *q = name.empty() ? nullptr : BrowserMaterial( name );
        IDirect3DTexture9 *tex = ColorMap( q );
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 q1( p.x + size, p.y + size );
        if ( tex )
            dl->AddImage( (ImTextureID)(intptr_t)tex, p, q1 );
        else
        {
            dl->AddRectFilled( p, q1, IM_COL32( 60, 60, 66, 255 ) );
            if ( load && q && !q->next && s_thumbBudget > 0 && ImGui::IsItemVisible() && s_thumbTried.insert( name ).second )
            {
                Texture_GetHandle( name.c_str() );   // shows from the next frame
                --s_thumbBudget;
            }
        }
    }

    void Swatch( const std::string &name, float size, bool load = true )
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy( ImVec2( size, size ) );
        DrawSwatch( p, size, name, load );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "%s", name.empty() ? "(each pane's own material)" : name.c_str() );
    }

    const float KGL_CARD = 96.0f;     // material card swatch, pixels

    // One material slot of the active set: the slot name, a large swatch (click = the
    // Textures browser's selected material, right-click = back to the default) and the
    // material's name under it.  `fallback` = what a blank slot means in game ("" = the
    // pane's own face material).
    void MaterialCard( const char *label, std::string &value, const char *fallback, bool load )
    {
        ImGui::BeginGroup();
        ImGui::PushID( label );
        ImGui::TextUnformatted( label );
        const std::string shown = value.empty() ? std::string( fallback ) : value;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton( "##swatch", ImVec2( KGL_CARD, KGL_CARD ) );
        const bool reset   = ImGui::IsItemClicked( ImGuiMouseButton_Right );
        const bool hovered = ImGui::IsItemHovered();
        DrawSwatch( p, KGL_CARD, shown, load );
        ImGui::GetWindowDrawList()->AddRect( p, ImVec2( p.x + KGL_CARD, p.y + KGL_CARD ),
                                             hovered ? IM_COL32( 230, 230, 240, 255 ) : IM_COL32( 90, 90, 100, 255 ) );
        if ( clicked )
        {
            qtexture_s *cur = g_qeglobals.random_texture_stuff[0].mtl.radMtl;
            if ( cur && cur->name )
                value = cur->name;
        }
        if ( reset )
            value.clear();
        if ( hovered )
            ImGui::SetTooltip( "%s\nClick: use the material selected in the Textures browser\nRight-click: back to the default",
                               shown.empty() ? "each pane's own face material" : shown.c_str() );
        ImGui::PushTextWrapPos( ImGui::GetCursorPosX() + KGL_CARD );
        if ( value.empty() )
            ImGui::TextDisabled( "%s", fallback[0] ? "default" : "pane's own" );
        else
            ImGui::TextWrapped( "%s", value.c_str() );
        ImGui::PopTextWrapPos();
        ImGui::PopID();
        ImGui::EndGroup();
    }

    void DrawSets()
    {
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::InputTextWithHint( "##glassfilter", "Search sets", s_filter, sizeof( s_filter ) );
        const float tile = 40.0f;
        if ( ImGui::BeginChild( "##glasssets", ImVec2( 0.0f, 260.0f ), ImGuiChildFlags_Borders ) )
        {
            std::vector<int> rows;
            for ( int i = 0; i < (int)s_sets.size(); ++i )
            {
                if ( s_filter[0] )
                {
                    std::string n = s_sets[i].name, f = s_filter;
                    std::transform( n.begin(), n.end(), n.begin(), ::tolower );
                    std::transform( f.begin(), f.end(), f.begin(), ::tolower );
                    if ( n.find( f ) == std::string::npos )
                        continue;
                }
                rows.push_back( i );
            }
            ImGuiListClipper clipper;
            clipper.Begin( (int)rows.size(), tile + ImGui::GetStyle().ItemSpacing.y );
            while ( clipper.Step() )
                for ( int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r )
                {
                    const glassSet_t &s = s_sets[rows[r]];
                    ImGui::PushID( rows[r] );
                    Swatch( s.pristine, tile );
                    ImGui::SameLine( 0.0f, 2.0f );
                    Swatch( s.cracked.empty() ? std::string( GLASS_KIWI_DEFAULT_CRACKED_MATERIAL ) : s.cracked, tile );
                    ImGui::SameLine( 0.0f, 2.0f );
                    Swatch( s.shard.empty() ? std::string( GLASS_KIWI_DEFAULT_SHARD_MATERIAL ) : s.shard, tile, false );
                    ImGui::SameLine();
                    const bool active = !_stricmp( s.name.c_str(), ActiveName() );
                    if ( ImGui::Selectable( s.name.c_str(), active, 0, ImVec2( 0.0f, tile ) ) && !active )
                    {
                        s_active = s.name;
                        SaveProfile();
                        EditActive();
                    }
                    ImGui::PopID();
                }
            if ( rows.empty() )
                ImGui::TextDisabled( s_sets.empty() ? "No sets in raw/glass" : "No set matches \"%s\"", s_filter );
        }
        ImGui::EndChild();

        if ( ImGui::Button( "New Set..." ) )
        {
            _snprintf( s_newName, sizeof( s_newName ) - 1, "%s_copy", ActiveName() );
            ImGui::OpenPopup( "New glass set" );
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Copy the active set under a new name" );
        ImGui::SameLine();
        if ( ImGui::Button( "Rescan" ) )
        {
            s_thumbTried.clear();
            s_matInfo.clear();
            Rescan();
            s_map.time = -1.0;
        }
        if ( ImGui::BeginPopup( "New glass set" ) )
        {
            ImGui::SetNextItemWidth( 220.0f );
            ImGui::InputText( "##newset", s_newName, sizeof( s_newName ), ImGuiInputTextFlags_CharsNoBlank );
            std::string name = s_newName;
            std::transform( name.begin(), name.end(), name.begin(), ::tolower );
            bool ok = !name.empty();
            for ( size_t i = 0; i < name.size() && ok; ++i )
                ok = isalnum( (unsigned char)name[i] ) || name[i] == '_';
            const bool exists = ok && FindSet( name.c_str() ) != nullptr;
            ImGui::BeginDisabled( !ok || exists );
            if ( ImGui::Button( "Create" ) )
            {
                glassSet_t s = s_editValid ? s_edit : glassSet_t();
                s.name = name;
                if ( WriteText( SetPath( name ), FormatSet( s ) ) )
                {
                    s_active = name;
                    SaveProfile();
                    Rescan();
                    SetStatus( "Created raw/glass/%s", name.c_str() );
                }
                else
                    SetStatus( "Could not write raw/glass/%s", name.c_str() );
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            if ( exists )
                ImGui::TextDisabled( "A set with that name exists" );
            else if ( !ok )
                ImGui::TextDisabled( "Letters, digits and _ only" );
            ImGui::EndPopup();
        }
    }

    void DrawEditor()
    {
        if ( !s_editValid )
        {
            ImGui::TextDisabled( "Set %s is not in raw/glass", ActiveName() );
            return;
        }
        const glassSet_t *disk = FindSet( s_edit.name.c_str() );
        const bool dirty = disk && !SameSet( *disk, s_edit );
        ImGui::Text( "%s%s", s_edit.name.c_str(), dirty ? " *" : "" );

        // The three materials side by side (sounds and the shatter effect stay as the file
        // has them; the window does not edit them).
        MaterialCard( "Pane", s_edit.pristine, "", true );
        ImGui::SameLine();
        MaterialCard( "Cracked", s_edit.cracked, GLASS_KIWI_DEFAULT_CRACKED_MATERIAL, true );
        ImGui::SameLine();
        MaterialCard( "Shards", s_edit.shard, GLASS_KIWI_DEFAULT_SHARD_MATERIAL, false );

        const float w = 96.0f;
        ImGui::SetNextItemWidth( w );
        ImGui::DragInt( "Health", &s_edit.maxHealth, 1.0f, 1, 100000 );
        ImGui::SetNextItemWidth( w );
        ImGui::DragInt( "Max shards", &s_edit.maxShards, 1.0f, 0, 400 );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "0 = the game's maxShardSplit" );
        float area[2] = { s_edit.minShardSize, s_edit.maxShardSize };
        ImGui::SetNextItemWidth( w );
        if ( ImGui::DragFloat2( "Shard area", area, 1.0f, 1.0f, 100000.0f, "%.0f" ) )
        {
            s_edit.minShardSize = (std::max)( 1.0f, area[0] );
            s_edit.maxShardSize = (std::max)( s_edit.minShardSize, area[1] );
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Smallest and largest shard, in square units" );
        ImGui::SetNextItemWidth( w );
        ImGui::SliderFloat( "Debris kept", &s_edit.keep, 0.0f, 1.0f, "%.2f" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Fraction of shards, largest first, that land and stay" );
        ImGui::SetNextItemWidth( w );
        ImGui::DragFloat( "UV scale", &s_edit.uvScale, 0.0001f, 0.0001f, 1.0f, "%.5f" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Shard texture scale; panes override it with their own uvscale" );
        bool frame = s_edit.frameEdges != 0;
        if ( ImGui::Checkbox( "Frame edges", &frame ) )
            s_edit.frameEdges = frame ? 1 : 0;
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Edge pieces stick in the frame, then drop" );

        ImGui::BeginDisabled( !dirty );
        if ( ImGui::Button( "Save" ) )
        {
            const bool paneChanged = disk && _stricmp( disk->pristine.c_str(), s_edit.pristine.c_str() );
            const glassSet_t saved = s_edit;
            if ( WriteText( SetPath( saved.name ), FormatSet( saved ) ) )
            {
                SetStatus( "Saved raw/glass/%s", saved.name.c_str() );
                s_matInfo.clear();
                Rescan();
                if ( paneChanged )
                    RetextureSetUsers( saved );
                s_map.time = -1.0;
            }
            else
                SetStatus( "Could not write raw/glass/%s", saved.name.c_str() );
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Revert" ) )
            EditActive();
        ImGui::EndDisabled();
    }

    void DrawSelection()
    {
        glassPick_t p;
        Gather( p );
        if ( p.brushes.empty() )
            ImGui::TextDisabled( "No glass selected" );
        else
            ImGui::Text( "%i pane brush(es), %i other brush(es) selected", p.panes, p.plain + p.other );

        const bool canMake = !p.blocked && p.plain + p.panes > 0;
        ImGui::BeginDisabled( !canMake );
        if ( ImGui::Button( "Make Breakable Glass" ) )
            KiwiGlass_Make();
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "%s", p.blocked ? p.blocked : canMake ? "Each selected brush becomes its own pane with the active set"
                                                                     : "Select worldspawn or func_group brushes" );
        ImGui::SameLine();
        ImGui::BeginDisabled( p.panes == 0 );
        if ( ImGui::Button( "Make Regular Brush" ) )
            KiwiGlass_Unmake();
        ImGui::SameLine();
        if ( ImGui::Button( "Apply Set" ) )
            KiwiGlass_ApplySetToSelection();
        if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "Selected panes take the active set and its pane material" );
        ImGui::EndDisabled();

        // per-pane status
        std::vector<entity_s_def *> defs;
        for ( size_t i = 0; i < p.brushes.size(); ++i )
            if ( IsPaneNode( p.brushes[i] ) )
            {
                entity_s_def *def = NodeDef( p.brushes[i] );
                if ( std::find( defs.begin(), defs.end(), def ) == defs.end() )
                    defs.push_back( def );
            }
        ImGui::PushTextWrapPos( 0.0f );               // status lines wrap to the window
        for ( size_t i = 0; i < defs.size() && i < 12; ++i )
        {
            paneStatus_t st;
            CheckPane( defs[i], st );
            char label[96];
            PaneLabel( defs[i], label, sizeof( label ) );
            const std::string set = Key( defs[i], "glassdef" );
            if ( st.errors )
                ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s: %s", label, st.text.c_str() );
            else if ( st.warnings )
                ImGui::TextColored( ImVec4( 1.0f, 0.75f, 0.3f, 1.0f ), "%s [%s]: %s", label, set.c_str(), st.text.c_str() );
            else
                ImGui::Text( "%s [%s]: OK", label, set.empty() ? GLASS_KIWI_DEFAULT_DEF : set.c_str() );
        }
        ImGui::PopTextWrapPos();
        if ( defs.size() > 12 )
            ImGui::TextDisabled( "and %i more", (int)defs.size() - 12 );

        if ( ImGui::Button( "Select Panes Using Set" ) )
            SelectPanes( UsesActive );
        ImGui::SameLine();
        ImGui::BeginDisabled( s_map.bad == 0 );
        if ( ImGui::Button( "Select Invalid Panes" ) )
            SelectPanes( IsInvalid );
        ImGui::EndDisabled();
    }
}

void KiwiGlass_Focus()
{
    KiwiWindows_Set( KIWI_WIN_GLASS, true );
    s_focusNext = true;
}

void KiwiGlass_Draw()
{
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_GLASS );
    if ( !open || !*open )
        return;
    if ( KiwiWindows_JustOpened( KIWI_WIN_GLASS ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );
    if ( s_focusNext )
    {
        ImGui::SetNextWindowFocus();             // fronts its dock tab too
        s_focusNext = false;
    }
    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_GLASS ), open ) )
    {
        EnsureScanned();
        CheckMap( false );
        s_thumbBudget = 4;
        ImGui::SeparatorText( "Sets" );
        DrawSets();
        ImGui::SeparatorText( "Active set" );
        DrawEditor();
        ImGui::SeparatorText( "Selection" );
        DrawSelection();
        ImGui::SeparatorText( "Map" );
        if ( s_map.panes.empty() )
            ImGui::TextDisabled( "No panes in this map" );
        else
            ImGui::Text( "%i pane(s), %i with errors, %i warning(s)", (int)s_map.panes.size(), s_map.bad, s_map.warnings );
        if ( MapLimitText()[0] )
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s", MapLimitText() );
        if ( s_status[0] )
            ImGui::TextDisabled( "%s", s_status );
    }
    ImGui::End();
}

// ── Build window ───────────────────────────────────────────────────────────────
void KiwiGlass_DrawBuildCheck()
{
    CheckMap( false );
    if ( s_map.panes.empty() )
        return;
    if ( !s_map.bad && !MapLimitText()[0] )
    {
        ImGui::TextDisabled( "Glass: %i pane(s) OK", (int)s_map.panes.size() );
        return;
    }
    ImGui::TextColored( ImVec4( 1.0f, 0.6f, 0.2f, 1.0f ), "Glass: %i of %i pane(s) will not break in game", s_map.bad, (int)s_map.panes.size() );
    if ( MapLimitText()[0] )
        ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "%s", MapLimitText() );
    ImGui::SameLine();
    if ( s_map.bad && ImGui::SmallButton( "Select##glassbad" ) )
        SelectPanes( IsInvalid );
}

// KIWI material health, in-place unlit conversion, and Shift+L diagnostics.
#include "stdafx.h"
#include "qe3.h"

#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>
#include <gfx_d3d/r_state.h>
#include <universal/com_files.h>
#include <imgui/imgui.h>

#include "kiwi_matconvert.h"
#include "kiwi_matwriter.h"

#include <algorithm>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

extern int         Sys_Printf( const char *fmt, ... );
extern qtexture_s *Texture_GetHandle( const char *name );
extern qtexture_s *TexWnd_RegisterMaterialByName( const char *name );
extern qtexture_s *MaterialDef_GetLayeredMaterial( MaterialDef *def );
extern bool        Materialdef_Realize( MaterialDef *def );
extern void        sub_47D060( selbrush_t *brushList );
extern int         g_nUpdateBits;
extern entity_s    entities;

namespace
{

const unsigned int KIWI_CONVERT_MENU_ID = 0xE501u;

struct HealthEntry
{
    std::string name;
    std::string techSet;
    int         faceCount;
};

struct SurfaceMaterial
{
    std::string name;
    std::string techSet;
};

struct MovedFile
{
    std::string source;
    std::string backup;
};

std::vector<HealthEntry> s_health;
int                      s_nonLitSurfaceCount = 0;
bool                     s_healthValid = false;

void SetErr( char *err, size_t errLen, const char *fmt, ... )
{
    if ( !err || !errLen )
        return;
    va_list ap;
    va_start( ap, fmt );
    _vsnprintf( err, errLen - 1, fmt, ap );
    va_end( ap );
    err[errLen - 1] = '\0';
}

const char *TechSetBase( const char *name )
{
    if ( !name )
        return "<missing>";
    if ( !_strnicmp( name, "wc_", 3 ) || !_strnicmp( name, "mc_", 3 ) )
        return name + 3;
    if ( !_strnicmp( name, "w_", 2 ) || !_strnicmp( name, "m_", 2 ) )
        return name + 2;
    return name;
}

bool IsExplicitlyNonLitTechSet( const char *name )
{
    const char *base = TechSetBase( name );
    return !_stricmp( base, "2d" ) || !_strnicmp( base, "unlit", 5 );
}

MaterialTechniqueSet *EffectiveTechSet( const Material *material )
{
    if ( !material || !material->techniqueSet )
        return nullptr;
    return material->techniqueSet->remappedTechniqueSet
         ? material->techniqueSet->remappedTechniqueSet : material->techniqueSet;
}

Material *LoadedMaterial( qtexture_s *q )
{
    if ( !q )
        return nullptr;
    if ( !q->handle && q->name && q->name[0] )
    {
        qtexture_s *loaded = Texture_GetHandle( q->name );
        if ( loaded )
            q = loaded;
    }
    return q->handle;
}

bool VisitMaterialDef( MaterialDef *def, SurfaceMaterial *out )
{
    if ( !def || ( !def->lyrMtl && !def->radMtl ) )
        return false;
    if ( !Materialdef_Realize( def ) )
        return false;
    qtexture_s *q = MaterialDef_GetLayeredMaterial( def );
    Material *material = LoadedMaterial( q );
    if ( !q || !material || !KiwiMatConvert_MaterialIsNonLit( material ) )
        return false;
    if ( out )
    {
        out->name = q->name ? q->name
                            : ( material->info.name ? material->info.name : "<unnamed>" );
        MaterialTechniqueSet *effective = EffectiveTechSet( material );
        out->techSet = TechSetBase( effective ? effective->name : nullptr );
    }
    return true;
}

void AddSurfaceToHealth( MaterialDef *def )
{
    SurfaceMaterial bad;
    if ( !VisitMaterialDef( def, &bad ) )
        return;

    ++s_nonLitSurfaceCount;
    size_t entry = 0;
    for ( ; entry < s_health.size(); ++entry )
        if ( !_stricmp( s_health[entry].name.c_str(), bad.name.c_str() ) )
            break;
    if ( entry == s_health.size() )
    {
        HealthEntry h;
        h.name = bad.name;
        h.techSet = bad.techSet;
        h.faceCount = 0;
        s_health.push_back( h );
    }
    ++s_health[entry].faceCount;
}

void ScanBrushDefinition( brush_t *brush )
{
    if ( !brush )
        return;
    if ( brush->patch )
    {
        AddSurfaceToHealth( (MaterialDef *)&brush->patch->texture );
        return;
    }
    for ( int face = 0; face < brush->faceCount; ++face )
        AddSurfaceToHealth( &brush->faces[face].mtldef[0] );
}

void ScanPrefabBrushList( selbrush_t *sentinel, int depth )
{
    if ( !sentinel || depth > 32 )
        return;
    for ( selbrush_t *instance = sentinel->next;
          instance && instance != sentinel; instance = instance->next )
    {
        // Brush-instance owners are entity instances: prefab identity is owner->prefab,
        // matching Walk_RecordList; owner->eclass is invalid here (use owner->def).
        entity_s *owner = instance->owner;
        if ( owner && owner->prefab )
        {
            ScanPrefabBrushList( &((prefab_s *)owner->prefab)->brushes, depth + 1 );
            continue;
        }
        ScanBrushDefinition( instance->def );
    }
}

void ScanPrefabRoots( selbrush_t *sentinel )
{
    if ( !sentinel )
        return;
    for ( selbrush_t *instance = sentinel->next;
          instance && instance != sentinel; instance = instance->next )
    {
        entity_s *owner = instance->owner;           // Prefab identity is owner->prefab; see ScanPrefabBrushList.
        if ( owner && owner->prefab )
            ScanPrefabBrushList( &((prefab_s *)owner->prefab)->brushes, 1 );
    }
}

void ScanMap( bool printDiagnostics )
{
    s_health.clear();
    s_nonLitSurfaceCount = 0;

    if ( entities.next )
    {
        for ( entity_s *entity = entities.next; entity && entity != &entities; entity = entity->next )
        {
            if ( entity->eclass && entity->eclass->fixedsize )
                continue;

            brush_t *sentinel = (brush_t *)&entity->def;
            for ( brush_t *brush = (brush_t *)entity->brushes.prev;
                  brush && brush != sentinel; brush = brush->onext )
                ScanBrushDefinition( brush );
        }
    }
    // Fixed-size prefab entities contribute a bounding-box brush to the entity list;
    // their rendered world surfaces live in the prefab instance's child display list.
    ScanPrefabRoots( &active_brushes );
    ScanPrefabRoots( &selected_brushes );
    ScanPrefabRoots( &filtered_brushes );

    std::sort( s_health.begin(), s_health.end(), []( const HealthEntry &a, const HealthEntry &b ) {
        return _stricmp( a.name.c_str(), b.name.c_str() ) < 0;
    } );
    s_healthValid = true;

    if ( !printDiagnostics )
        return;

    Sys_Printf( "\n---- Map material health ----\n" );
    for ( size_t i = 0; i < s_health.size(); ++i )
    {
        const HealthEntry &h = s_health[i];
        Sys_Printf( "Material '%s' uses techset '%s' - it will render UNLIT in-game and "
                    "cast NO runtime sun shadows (used by %d faces). Textures tab > "
                    "right-click > Convert to lit world material.\n",
                    h.name.c_str(), h.techSet.c_str(), h.faceCount );
    }
    Sys_Printf( "Map health: %d surfaces use non-lit materials (no runtime shadows).\n",
                s_nonLitSurfaceCount );
    Sys_Printf( "-----------------------------\n" );
}

bool ValidMaterialName( const char *name )
{
    if ( !name || !name[0] || name[0] == '$' || strlen( name ) > 50 )
        return false;
    if ( name[0] == '/' || name[0] == '\\' || strstr( name, ".." ) || strchr( name, ':' ) )
        return false;
    for ( const unsigned char *p = (const unsigned char *)name; *p; ++p )
    {
        const bool okay = ( *p >= 'a' && *p <= 'z' ) || ( *p >= 'A' && *p <= 'Z' )
                       || ( *p >= '0' && *p <= '9' ) || *p == '_' || *p == '-'
                       || *p == '.' || *p == '/';
        if ( !okay )
            return false;
    }
    return true;
}

bool HasRealArt( const char *image )
{
    return image && image[0] && image[0] != '$';
}

bool ContainsNoCase( const char *text, const char *needle )
{
    if ( !text || !needle || !needle[0] )
        return false;
    const size_t needleLen = strlen( needle );
    const size_t textLen = strlen( text );
    for ( size_t i = 0; i + needleLen <= textLen; ++i )
        if ( !_strnicmp( text + i, needle, needleLen ) )
            return true;
    return false;
}

char BlendFamily( const kiwiMatSource_t &source, const Material *loaded )
{
    const char *loadedTech = loaded && loaded->techniqueSet
                           ? TechSetBase( loaded->techniqueSet->name ) : source.techSet;
    if ( ( source.refStateBits[0] & GFXS0_ATEST_MASK ) != 0
      || ContainsNoCase( loadedTech, "alphatest" ) || ContainsNoCase( loadedTech, "_t0" )
      || ContainsNoCase( source.techSet, "alphatest" ) || ContainsNoCase( source.techSet, "_t0" ) )
        return 't';

    const bool namedBlend = ContainsNoCase( loadedTech, "add" )
                         || ContainsNoCase( loadedTech, "blend" )
                         || ContainsNoCase( loadedTech, "multiply" )
                         || ContainsNoCase( loadedTech, "falloff" )
                         || ContainsNoCase( loadedTech, "replace" )
                         || ContainsNoCase( loadedTech, "_b0" )
                         || ContainsNoCase( source.techSet, "add" )
                         || ContainsNoCase( source.techSet, "blend" )
                         || ContainsNoCase( source.techSet, "multiply" )
                         || ContainsNoCase( source.techSet, "falloff" )
                         || ContainsNoCase( source.techSet, "replace" )
                         || ContainsNoCase( source.techSet, "_b0" );
    // RGB blend 0x12 is Src=One, Dst=Zero, Op=Disable (opaque); any other nonzero
    // value preserves unnamed alpha/additive sources.
    const unsigned int rgbBlend = source.refStateBits[0] & GFXS0_BLEND_RGB_MASK;
    if ( namedBlend || ( rgbBlend && rgbBlend != 0x12 ) || source.sortKey != 4 )
        return 'b';
    return 'r';
}

void BuildFamilyName( char family, bool normal, bool specular, char *out, size_t outLen )
{
    _snprintf( out, outLen, "l_sm_%c0c0%s%s", family,
               normal ? "n0" : "", specular ? "s0" : "" );
    out[outLen - 1] = '\0';
}

int FindTemplate( const char *techSet )
{
    for ( int i = 0; i < KiwiMat_TemplateCount(); ++i )
    {
        const kiwiMatTemplateInfo_t *info = KiwiMat_TemplateInfo( i );
        if ( info && !_stricmp( info->techSet, techSet ) && KiwiMat_ResolveTemplate( i ) )
            return i;
    }
    return -1;
}

void AddRoot( std::vector<std::string> &roots, const char *root )
{
    if ( !root || !root[0] )
        return;
    for ( size_t i = 0; i < roots.size(); ++i )
        if ( !_stricmp( roots[i].c_str(), root ) )
            return;
    roots.push_back( root );
}

bool FileExists( const char *path )
{
    const DWORD attributes = GetFileAttributesA( path );
    return attributes != INVALID_FILE_ATTRIBUTES && !( attributes & FILE_ATTRIBUTE_DIRECTORY );
}

bool BackupFile( const std::string &root, const char *dir, const char *name,
                 std::vector<MovedFile> &moved, char *err, size_t errLen )
{
    char qpath[128];
    _snprintf( qpath, sizeof( qpath ), "materials/%s", name );
    qpath[sizeof( qpath ) - 1] = '\0';

    char source[MAX_OSPATH];
    FS_BuildOSPath( root.c_str(), dir, qpath, source );
    if ( !FileExists( source ) )
        return true;

    char backupQpath[160];
    _snprintf( backupQpath, sizeof( backupQpath ), "materials/_kiwi_backup/%s", name );
    backupQpath[sizeof( backupQpath ) - 1] = '\0';

    char backup[MAX_OSPATH];
    FS_BuildOSPath( root.c_str(), dir, backupQpath, backup );
    for ( int suffix = 1; FileExists( backup ) && suffix < 1000; ++suffix )
    {
        _snprintf( backupQpath, sizeof( backupQpath ),
                   "materials/_kiwi_backup/%s.%d", name, suffix );
        backupQpath[sizeof( backupQpath ) - 1] = '\0';
        FS_BuildOSPath( root.c_str(), dir, backupQpath, backup );
    }
    if ( FileExists( backup ) )
    {
        SetErr( err, errLen, "could not choose a free backup name for '%s'", source );
        return false;
    }
    if ( FS_CreatePath( backup ) )
    {
        SetErr( err, errLen, "refused to create the backup path '%s'", backup );
        return false;
    }
    if ( !MoveFileExA( source, backup, MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH ) )
    {
        SetErr( err, errLen, "could not move '%s' to '%s' (Win32 error %lu)",
                source, backup, GetLastError() );
        return false;
    }

    MovedFile record;
    record.source = source;
    record.backup = backup;
    moved.push_back( record );
    return true;
}

void RollbackMoves( std::vector<MovedFile> &moved )
{
    for ( size_t i = moved.size(); i > 0; --i )
    {
        const MovedFile &record = moved[i - 1];
        if ( !MoveFileExA( record.backup.c_str(), record.source.c_str(),
                           MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING
                         | MOVEFILE_WRITE_THROUGH ) )
            Sys_Printf( "Material convert ROLLBACK FAILED: '%s' remains safely at '%s' "
                        "(Win32 error %lu).\n", record.source.c_str(),
                        record.backup.c_str(), GetLastError() );
    }
}

bool BackupActiveCopies( const char *name, std::vector<MovedFile> &moved,
                         char *err, size_t errLen )
{
    std::vector<std::string> roots;
    AddRoot( roots, fs_homepath ? fs_homepath->current.string : nullptr );
    AddRoot( roots, fs_basepath ? fs_basepath->current.string : nullptr );
    if ( roots.empty() )
    {
        SetErr( err, errLen, "fs_homepath/fs_basepath is empty" );
        return false;
    }

    for ( size_t i = 0; i < roots.size(); ++i )
    {
        if ( !BackupFile( roots[i], "raw", name, moved, err, errLen )
          || !BackupFile( roots[i], "main", name, moved, err, errLen ) )
        {
            RollbackMoves( moved );
            return false;
        }
    }
    if ( moved.empty() )
    {
        SetErr( err, errLen,
                "the material was readable, but no raw/main file could be found to back up" );
        return false;
    }
    return true;
}

bool ValidateFreshMaterial( Material *fresh, const char *expectedTechSet,
                            char *err, size_t errLen )
{
    if ( !fresh || Material_IsDefault( fresh ) )
    {
        SetErr( err, errLen, "the engine rejected the rewritten material" );
        return false;
    }
    const char *loadedTech = fresh->techniqueSet ? TechSetBase( fresh->techniqueSet->name ) : "";
    if ( _stricmp( loadedTech, expectedTechSet ) )
    {
        SetErr( err, errLen, "the engine loaded techset '%s', expected '%s'",
                loadedTech, expectedTechSet );
        return false;
    }
    if ( KiwiMatConvert_MaterialIsNonLit( fresh ) )
    {
        SetErr( err, errLen, "techset '%s' lacks lit-sun-shadow or build-shadowmap-depth",
                loadedTech );
        return false;
    }

    bool colorReady = false;
    for ( int i = 0; i < fresh->textureCount; ++i )
    {
        if ( fresh->textureTable[i].semantic != 2 )
            continue;
        GfxImage *image = fresh->textureTable[i].u.image;
        colorReady = image && image->texture.basemap;
        break;
    }
    if ( !colorReady )
    {
        SetErr( err, errLen, "the rewritten material's colorMap did not upload" );
        return false;
    }
    return true;
}

void RefreshLiveMaterial( const char *name, Material *oldMaterial, Material *fresh,
                          qtexture_s *browserMaterial )
{
    const unsigned short hashIndex = oldMaterial->info.hashIndex;
    *oldMaterial = *fresh;
    oldMaterial->info.hashIndex = hashIndex;
    Material_Sort();

    qtexture_s *registered = TexWnd_RegisterMaterialByName( name );
    if ( registered )
        browserMaterial = Texture_GetHandle( name );
    if ( browserMaterial )
        browserMaterial->handle = oldMaterial;

    sub_47D060( &active_brushes );
    sub_47D060( &selected_brushes );
    sub_47D060( &filtered_brushes );
    g_nUpdateBits = -1;
}

void PrintConversion( const char *name, const kiwiMatSource_t &source,
                      const char *family, const std::vector<MovedFile> &moved,
                      const kiwiMatSource_t &written )
{
    Sys_Printf( "Material convert: '%s'\n", name );
    Sys_Printf( "  family: %s -> %s\n", source.techSet, family );
    Sys_Printf( "  images: color='%s', normal='%s', specular='%s'\n",
                source.colorMapImage,
                HasRealArt( source.normalMapImage ) ? source.normalMapImage : "<none>",
                HasRealArt( source.specularMapImage ) ? source.specularMapImage : "<none>" );
    for ( size_t i = 0; i < moved.size(); ++i )
        Sys_Printf( "  backup: %s -> %s\n", moved[i].source.c_str(), moved[i].backup.c_str() );
    Sys_Printf( "  output: raw/materials/%s (gameFlags 0x%02X, sort %u)\n",
                name, (unsigned)written.gameFlags, (unsigned)written.sortKey );
    Sys_Printf( "  refresh: material, texture browser, brush faces and patches refreshed live\n" );
}

void ConvertAllFromSnapshot()
{
    std::vector<std::string> names;
    names.reserve( s_health.size() );
    for ( size_t i = 0; i < s_health.size(); ++i )
        names.push_back( s_health[i].name );

    int converted = 0;
    for ( size_t i = 0; i < names.size(); ++i )
    {
        char err[512] = { 0 };
        if ( KiwiMatConvert_ToLit( names[i].c_str(), err, sizeof( err ) ) )
            ++converted;
        else
            Sys_Printf( "Material convert FAILED: '%s': %s\n", names[i].c_str(), err );
    }
    ScanMap( true );
    Sys_Printf( "Material convert: %d of %d map material(s) converted.\n",
                converted, (int)names.size() );
}

} // namespace

bool KiwiMatConvert_MaterialIsNonLit( const Material *material )
{
    if ( !material || !material->techniqueSet )
        return true;
    if ( IsExplicitlyNonLitTechSet( material->techniqueSet->name ) )
        return true;
    MaterialTechniqueSet *techSet = EffectiveTechSet( material );
    if ( techSet && IsExplicitlyNonLitTechSet( techSet->name ) )
        return true;
    if ( !techSet || !techSet->techniques[TECHNIQUE_LIT_SUN_SHADOW] )
        return true;
    // l_sm_b0* has lit-sun-shadow but intentionally no depth pass; only opaque and
    // alpha-tested world families require both passes.
    if ( ContainsNoCase( TechSetBase( techSet->name ), "l_sm_b0" ) )
        return false;
    return !techSet->techniques[TECHNIQUE_BUILD_SHADOWMAP_DEPTH];
}

void KiwiMatConvert_ResetMapHealth()
{
    s_health.clear();
    s_nonLitSurfaceCount = 0;
    s_healthValid = false;
}

void KiwiMatConvert_OnMapLoaded()
{
    ScanMap( true );
}

void KiwiMatConvert_OnMaterialModeChanged( int mode )
{
    if ( mode == 1 )
        ScanMap( true );
}

bool KiwiMatConvert_ShouldShowBrushInLightmap( brush_t *brush )
{
    if ( !brush || g_qeglobals.current_edit_layer != 1 )
        return false;
    if ( brush->patch )
        return VisitMaterialDef( (MaterialDef *)&brush->patch->texture, nullptr );
    for ( int face = 0; face < brush->faceCount; ++face )
        if ( VisitMaterialDef( &brush->faces[face].mtldef[0], nullptr ) )
            return true;
    return false;
}

void KiwiMatConvert_ApplyFaceLightmapDiagnostic( face_t *face, EdLayerGeom *geom )
{
    if ( !face || !geom || g_qeglobals.current_edit_layer != 1
      || !VisitMaterialDef( &face->mtldef[0], nullptr ) )
        return;
    for ( int i = 0; i < geom->vertcount; ++i )
        geom->color[i] = 0xFFFF0000u;
    if ( g_qeglobals.d_opague )
        geom->material = g_qeglobals.d_opague;
}

void KiwiMatConvert_ApplyPatchLightmapDiagnostic( patchMesh_t *patch,
                                                  unsigned int *colors, int colorCount,
                                                  Material **material )
{
    if ( !patch || !colors || colorCount <= 0 || g_qeglobals.current_edit_layer != 1
      || !VisitMaterialDef( (MaterialDef *)&patch->texture, nullptr ) )
        return;
    for ( int i = 0; i < colorCount; ++i )
        colors[i] = 0xFFFF0000u;
    if ( material && g_qeglobals.d_opague )
        *material = g_qeglobals.d_opague;
}

bool KiwiMatConvert_ToLit( const char *materialName, char *errOut, size_t errLen )
{
    if ( errOut && errLen )
        errOut[0] = '\0';
    if ( !ValidMaterialName( materialName ) )
    {
        SetErr( errOut, errLen, "invalid material name" );
        return false;
    }

    kiwiMatSource_t source;
    if ( !KiwiMat_ReadSource( materialName, &source, errOut, errLen ) )
        return false;

    qtexture_s *browserMaterial = Texture_GetHandle( materialName );
    Material *oldMaterial = LoadedMaterial( browserMaterial );
    if ( !oldMaterial || Material_IsDefault( oldMaterial ) )
    {
        SetErr( errOut, errLen, "the existing material is not loaded successfully" );
        return false;
    }
    if ( !KiwiMatConvert_MaterialIsNonLit( oldMaterial ) )
    {
        SetErr( errOut, errLen, "material already has lit-sun-shadow and build-shadowmap-depth passes" );
        return false;
    }

    const bool haveNormal = HasRealArt( source.normalMapImage );
    const bool haveSpecular = HasRealArt( source.specularMapImage );
    char family[64];
    BuildFamilyName( BlendFamily( source, oldMaterial ), haveNormal, haveSpecular,
                     family, sizeof( family ) );
    const int templateIndex = FindTemplate( family );
    if ( templateIndex < 0 )
    {
        SetErr( errOut, errLen, "no healthy shipped template resolves for family '%s'", family );
        return false;
    }

    kiwiMatFields_t fields;
    memset( &fields, 0, sizeof( fields ) );
    _snprintf( fields.name, sizeof( fields.name ), "%s", materialName );
    _snprintf( fields.imageName, sizeof( fields.imageName ), "%s", source.colorMapImage );
    if ( haveNormal )
        _snprintf( fields.normalImageName, sizeof( fields.normalImageName ), "%s", source.normalMapImage );
    if ( haveSpecular )
        _snprintf( fields.specularImageName, sizeof( fields.specularImageName ), "%s", source.specularMapImage );
    fields.usage = source.usage ? source.usage
                 : browserMaterial && browserMaterial->usage_index
                 ? (unsigned char)browserMaterial->usage_index : 1;
    fields.locale = source.locale ? source.locale
                  : browserMaterial && browserMaterial->tex_num_or_localefilter
                  ? (unsigned int)browserMaterial->tex_num_or_localefilter : 1u;
    fields.autoTexScaleWidth = source.autoTexScaleWidth ? source.autoTexScaleWidth
                             : browserMaterial && browserMaterial->width > 0
                             ? (unsigned short)browserMaterial->width : 512;
    fields.autoTexScaleHeight = source.autoTexScaleHeight ? source.autoTexScaleHeight
                              : browserMaterial && browserMaterial->height > 0
                              ? (unsigned short)browserMaterial->height : 512;
    fields.surfaceType = source.surfaceFlags & 0x1F00000;

    std::vector<MovedFile> moved;
    if ( !BackupActiveCopies( materialName, moved, errOut, errLen ) )
        return false;

    if ( !KiwiMat_Write( templateIndex, &fields, errOut, errLen ) )
    {
        KiwiMat_DeleteWritten( materialName, nullptr, nullptr, nullptr );
        RollbackMoves( moved );
        return false;
    }

    kiwiMatSource_t written;
    char validationErr[512] = { 0 };
    const unsigned char expectedSort = family[5] == 'b' ? 12 : 4;
    if ( !KiwiMat_ReadSource( materialName, &written, validationErr, sizeof( validationErr ) )
      || _stricmp( written.techSet, family ) || written.gameFlags != 0x12
      || written.sortKey != expectedSort )
    {
        KiwiMat_DeleteWritten( materialName, nullptr, nullptr, nullptr );
        RollbackMoves( moved );
        SetErr( errOut, errLen,
                "rewritten header verification failed: %s (techset '%s', gameFlags 0x%02X, sort %u)",
                validationErr[0] ? validationErr : "unexpected header",
                written.techSet, (unsigned)written.gameFlags, (unsigned)written.sortKey );
        return false;
    }

    char fullName[80];
    _snprintf( fullName, sizeof( fullName ), "wc/%s", materialName );
    fullName[sizeof( fullName ) - 1] = '\0';
    Material *fresh = Material_Load( fullName, 0 );
    if ( !ValidateFreshMaterial( fresh, family, validationErr, sizeof( validationErr ) ) )
    {
        KiwiMat_DeleteWritten( materialName, nullptr, nullptr, nullptr );
        RollbackMoves( moved );
        SetErr( errOut, errLen, "rewritten material verification failed: %s", validationErr );
        return false;
    }

    RefreshLiveMaterial( materialName, oldMaterial, fresh, browserMaterial );
    // KIWI (2026-09-09): the sun-preview cast memo is keyed on brush versions and the
    // STRUCTURAL epoch (kiwi_shadowcache.cpp), and a converted material changes what its
    // brushes cast without touching any brush — bump the structural epoch so every memo
    // re-evaluates against the fresh material.
    {
        extern void KiwiWalkCache_MarkStructural();   // kiwi_walkcache.h
        KiwiWalkCache_MarkStructural();
    }
    ScanMap( false );
    PrintConversion( materialName, source, family, moved, written );
    return true;
}

void KiwiMatConvert_AppendTextureContextMenu( void *menu, qtexture_s *material )
{
    if ( !menu || !material )
        return;
    Material *loaded = LoadedMaterial( material );
    AppendMenuA( (HMENU)menu, MF_SEPARATOR, 0, nullptr );
    const UINT flags = loaded && !Material_IsDefault( loaded )
                    && ValidMaterialName( material->name )
                    && KiwiMatConvert_MaterialIsNonLit( loaded )
                     ? MF_STRING : MF_STRING | MF_GRAYED;
    AppendMenuA( (HMENU)menu, flags, KIWI_CONVERT_MENU_ID,
                 "Convert to lit world material" );
}

bool KiwiMatConvert_HandleTextureContextCommand( unsigned int command, qtexture_s *material )
{
    if ( command != KIWI_CONVERT_MENU_ID )
        return false;
    if ( !material || !material->name )
        return true;

    char err[512] = { 0 };
    if ( !KiwiMatConvert_ToLit( material->name, err, sizeof( err ) ) )
    {
        Sys_Printf( "Material convert FAILED: '%s': %s\n", material->name, err );
        MessageBoxA( g_qeglobals.d_hwndMain, err, "Convert to lit world material",
                     MB_OK | MB_ICONERROR );
    }
    return true;
}

void KiwiMatConvert_DrawMapHealth()
{
    if ( !s_healthValid )
        ScanMap( false );

    ImGui::Separator();
    ImGui::TextUnformatted( "Map health" );
    const ImVec4 statusColor = s_nonLitSurfaceCount
                             ? ImVec4( 1.0f, 0.55f, 0.25f, 1.0f )
                             : ImVec4( 0.45f, 0.85f, 0.52f, 1.0f );
    ImGui::TextColored( statusColor, "%d surfaces use non-lit materials (no runtime shadows)",
                        s_nonLitSurfaceCount );

    char button[160];
    _snprintf( button, sizeof( button ),
               "Convert all unlit materials used by this map (%d)", (int)s_health.size() );
    button[sizeof( button ) - 1] = '\0';
    ImGui::BeginDisabled( s_health.empty() );
    if ( ImGui::Button( button, ImVec2( -1.0f, 0.0f ) ) )
        ImGui::OpenPopup( "Convert map materials to lit?" );
    ImGui::EndDisabled();

    if ( ImGui::BeginPopupModal( "Convert map materials to lit?", nullptr,
                                 ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        ImGui::TextWrapped( "The following material definitions will be replaced in raw. "
                            "Every existing raw/main copy is moved to materials/_kiwi_backup first." );
        ImGui::BeginChild( "##unlit_material_list", ImVec2( 540.0f, 180.0f ),
                           ImGuiChildFlags_Borders );
        for ( size_t i = 0; i < s_health.size(); ++i )
            ImGui::BulletText( "%s  [%s, %d faces]", s_health[i].name.c_str(),
                               s_health[i].techSet.c_str(), s_health[i].faceCount );
        ImGui::EndChild();

        if ( ImGui::Button( "Convert all", ImVec2( 160.0f, 0.0f ) ) )
        {
            ConvertAllFromSnapshot();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 160.0f, 0.0f ) ) )
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

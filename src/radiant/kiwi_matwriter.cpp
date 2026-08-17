// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_matwriter.cpp — KIWI-UX (ROUND BE): THE BINARY MATERIAL WRITER (mechanical half).
// ═════════════════════════════════════════════════════════════════════════════════════
// Read kiwi_matwriter.h first — D-BE-E..I carry the whole argument (why cloning, the
// field-by-field derivation of the blob layout from Material_LoadRaw, why "wc/" is not a
// directory, where the files land, and why the browser gate is a hard requirement).  This
// file is the transcription of that derivation.
// ═════════════════════════════════════════════════════════════════════════════════════
#include "stdafx.h"
#include "qe3.h"

#include <gfx_d3d/r_material.h>     // MaterialRaw / MaterialInfoRaw / MaterialTextureDefRaw /
                                    // MaterialConstantDefRaw / Material / Material_RegisterHandle /
                                    // Material_IsDefault
#include <gfx_d3d/r_gfx.h>          // GfxImage / GfxTexture
#include <universal/com_files.h>    // FS_FOpenFileWrite / FS_Write / FS_FOpenFileRead / FS_Read /
                                    // FS_FCloseFile / FS_Delete / FS_ListFiles / FS_FreeFileList

#include "kiwi_matwriter.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

// ── ported / cross-file entry points (each verified against its definition) ─────────
extern int  Sys_Printf( const char *fmt, ... );          // win_qe3.cpp:112   int Sys_Printf(const char*,...)

// Every offset this file writes is `sizeof`-derived rather than hard-coded, so the ONLY way
// the layout can drift out from under it is a struct edit.  These four are the sizes the
// derivation in kiwi_matwriter.h D-BE-F assumes, and the sizes all 4353 shipped materials
// parse with.  r_material.h declares them without size asserts of its own.
static_assert( sizeof( MaterialInfoRaw )        == 0x28, "MaterialInfoRaw must be 40 bytes" );
static_assert( sizeof( MaterialRaw )            == 0x40, "MaterialRaw must be 64 bytes" );
static_assert( sizeof( MaterialTextureDefRaw )  == 0x0C, "MaterialTextureDefRaw must be 12 bytes" );
static_assert( sizeof( MaterialConstantDefRaw ) == 0x14, "MaterialConstantDefRaw must be 20 bytes" );

// ═════════════════════════════════════════════════════════════════════════════════════
//  THE TEMPLATE SET
// ═════════════════════════════════════════════════════════════════════════════════════
// Four families, each with a techset whose ONLY art dependency is a colormap: every other
// texture entry in the shipped members points at a BUILT-IN image ($identitynormalmap,
// which Image_Load routes to Image_LoadBuiltin on the leading '$' — r_image_load_obj.cpp:373).
// That is what makes them clonable from one dropped file: the clone substitutes the colormap
// and inherits a valid normal/spec binding it does not have to invent.
//
// The candidate lists were chosen by parsing all 4353 shipped materials in this data tree and
// keeping only members with a non-zero usage AND locale (i.e. ones that already pass the
// browser gate) whose non-colormap images are all built-ins.
namespace
{

struct TemplateRow
{
    kiwiMatTemplateInfo_t info;
    const char           *candidates[5];
    int                   expectSortKey;
    int                   expectTextureCount;
};

const TemplateRow kTemplates[] =
{
    { { "World - lit, opaque",       "l_sm_r0c0",
        "Standard lit world surface. The usual choice for walls, floors and terrain." },
      { "ch_concrete_01", "ch_brick_wall_03", "case256x32red", nullptr, nullptr }, 4, 2 },

    { { "World - lit, alpha test",   "l_sm_t0c0",
        "Lit, with hard cut-out transparency from the image's alpha. Fences, foliage, grates." },
      { "icbm_wirescreen", "me_curtains_1", "ch_windowslatbroke01_a", nullptr, nullptr }, 4, 2 },

    { { "World - lit, alpha blend",  "l_sm_b0c0",
        "Lit, with soft alpha blending and a decal sort key. Signs, posters, painted overlays." },
      { "ch_decal_mural", "ad_sign256x256", "ad_sign128x256", nullptr, nullptr }, 12, 2 },

    { { "World - unlit, opaque",     "unlit",
        "Fullbright, ignores lighting entirely. Pre-baked art and light-panel surfaces." },
      { "ac_building_wall01", "ac_building_door01", "ac_ground_base01", nullptr, nullptr }, 4, 1 },
};

const int kTemplateCount = (int)( sizeof( kTemplates ) / sizeof( kTemplates[0] ) );

// resolved-once cache, one slot per row (empty string == "asked and answered, none found")
std::string g_resolved[kTemplateCount];
bool        g_resolvedDone[kTemplateCount] = {};

void SetErr( char *err, size_t errSz, const char *fmt, ... )
{
    if ( !err || errSz == 0 )
        return;
    va_list ap;
    va_start( ap, fmt );
    _vsnprintf( err, errSz - 1, fmt, ap );
    va_end( ap );
    err[errSz - 1] = '\0';
}

// ── a parsed material blob ─────────────────────────────────────────────────────────
struct TexEntry
{
    std::string   name;         // "colorMap" / "normalMap" / …
    std::string   image;        // the image name this entry binds
    unsigned char samplerState;
    unsigned char semantic;
};
struct ConstEntry
{
    std::string name;
    float       literal[4];
};
struct ParsedMaterial
{
    MaterialInfoRaw         info;
    unsigned int            refStateBits[2];
    std::string             name;
    std::string             refImage;
    std::string             techSet;
    std::vector<TexEntry>   textures;
    std::vector<ConstEntry> constants;
};

// Read a NUL-terminated string that must lie entirely inside the blob.
bool ReadStr( const std::vector<unsigned char> &blob, unsigned int off, std::string *out )
{
    if ( off == 0 || off >= blob.size() )
        return false;
    const char *base = (const char *)&blob[0];
    for ( size_t i = off; i < blob.size(); ++i )
    {
        if ( base[i] == '\0' )
        {
            out->assign( base + off, i - off );
            return true;
        }
    }
    return false;
}

// Read the whole of materials/<name> into `blob` through the engine filesystem.
bool ReadMaterialFile( const char *name, std::vector<unsigned char> *blob )
{
    char qpath[128];
    _snprintf( qpath, sizeof( qpath ), "materials/%s", name );
    qpath[sizeof( qpath ) - 1] = '\0';

    int fh = 0;
    com_fileAccessed = 1;
    const int size = (int)FS_FOpenFileRead( qpath, &fh );
    if ( !fh )
        return false;
    if ( size < (int)sizeof( MaterialRaw ) )
    {
        FS_FCloseFile( fh );
        return false;
    }
    blob->assign( (size_t)size, 0 );
    const unsigned got = FS_Read( &( *blob )[0], (unsigned)size, fh );
    FS_FCloseFile( fh );
    return got == (unsigned)size;
}

// Parse a blob with EXACTLY the offsets Material_LoadRaw dereferences (kiwi_matwriter.h D-BE-F).
bool ParseMaterial( const std::vector<unsigned char> &blob, ParsedMaterial *out )
{
    if ( blob.size() < sizeof( MaterialRaw ) )
        return false;
    const MaterialRaw *raw = (const MaterialRaw *)&blob[0];

    out->info            = raw->info;
    out->refStateBits[0] = raw->refStateBits[0];
    out->refStateBits[1] = raw->refStateBits[1];

    if ( !ReadStr( blob, raw->info.nameOffset, &out->name ) )        // :5853
        return false;
    if ( !ReadStr( blob, raw->techSetNameOffset, &out->techSet ) )   // :5514
        return false;
    // refImageNameOffset is never dereferenced by the loader; tolerate a bad one.
    if ( !ReadStr( blob, raw->info.refImageNameOffset, &out->refImage ) )
        out->refImage.clear();

    const size_t texEnd = (size_t)raw->textureTableOffset + (size_t)raw->textureCount * 12;
    if ( raw->textureCount && texEnd > blob.size() )
        return false;
    for ( unsigned i = 0; i < raw->textureCount; ++i )               // :5908-5932
    {
        const MaterialTextureDefRaw *t =
            (const MaterialTextureDefRaw *)( &blob[0] + raw->textureTableOffset + 12 * i );
        TexEntry e;
        if ( !ReadStr( blob, t->nameOffset, &e.name ) )
            return false;
        if ( !ReadStr( blob, t->u.imageNameOffset, &e.image ) )
            return false;
        e.samplerState = t->samplerState;
        e.semantic     = t->semantic;
        out->textures.push_back( e );
    }

    const size_t cnstEnd = (size_t)raw->constantTableOffset + (size_t)raw->constantCount * 20;
    if ( raw->constantCount && cnstEnd > blob.size() )
        return false;
    for ( unsigned i = 0; i < raw->constantCount; ++i )              // :5939-5951
    {
        const MaterialConstantDefRaw *c =
            (const MaterialConstantDefRaw *)( &blob[0] + raw->constantTableOffset + 20 * i );
        ConstEntry e;
        if ( !ReadStr( blob, c->nameOffset, &e.name ) )              // must be non-zero :5508
            return false;
        for ( int k = 0; k < 4; ++k )
            e.literal[k] = c->literal[k];
        out->constants.push_back( e );
    }
    return true;
}

// Is this parsed material usable as a template for `row`?
bool TemplateShapeOk( const TemplateRow &row, const ParsedMaterial &m )
{
    if ( m.techSet != row.info.techSet )
        return false;
    if ( m.info.sortKey != (unsigned char)row.expectSortKey )
        return false;
    if ( (int)m.textures.size() != row.expectTextureCount )
        return false;
    if ( m.constants.empty() )
        return false;
    // exactly one colormap, and every OTHER image must be a built-in ('$…', which
    // Image_Load routes to Image_LoadBuiltin — r_image_load_obj.cpp:373), because the
    // clone replaces only the colormap and must not inherit somebody else's art.
    int colorMaps = 0;
    for ( size_t i = 0; i < m.textures.size(); ++i )
    {
        if ( m.textures[i].semantic == 2 )
        {
            ++colorMaps;
            continue;
        }
        if ( m.textures[i].semantic == 11 )        // a water def, not an image name
            return false;
        if ( m.textures[i].image.empty() || m.textures[i].image[0] != '$' )
            return false;
    }
    if ( colorMaps != 1 )
        return false;
    // the gate fields must already be sane — we copy toolFlags/refStateBits from here and
    // a template that could not itself appear in the browser is the wrong thing to clone.
    if ( !m.info.usage || !m.info.locale )
        return false;
    return true;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════════════
//  TEMPLATE RESOLUTION
// ═════════════════════════════════════════════════════════════════════════════════════
int KiwiMat_TemplateCount() { return kTemplateCount; }

const kiwiMatTemplateInfo_t *KiwiMat_TemplateInfo( int index )
{
    if ( index < 0 || index >= kTemplateCount )
        return nullptr;
    return &kTemplates[index].info;
}

const char *KiwiMat_ResolveTemplate( int index )
{
    if ( index < 0 || index >= kTemplateCount )
        return nullptr;
    if ( g_resolvedDone[index] )
        return g_resolved[index].empty() ? nullptr : g_resolved[index].c_str();

    const TemplateRow &row = kTemplates[index];
    g_resolvedDone[index] = true;

    std::vector<unsigned char> blob;
    ParsedMaterial             m;

    // 1) the named candidates, in order — one FS open each, no directory walk.
    for ( int i = 0; i < 5 && row.candidates[i]; ++i )
    {
        blob.clear();
        m = ParsedMaterial();
        if ( !ReadMaterialFile( row.candidates[i], &blob ) )
            continue;
        if ( !ParseMaterial( blob, &m ) || !TemplateShapeOk( row, m ) )
            continue;
        g_resolved[index] = row.candidates[i];
        return g_resolved[index].c_str();
    }

    // 2) fall back to scanning materials/ for the first file of the right shape.  Only
    //    reached in a data tree whose art differs from the shipped one, and only once.
    int          count = 0;
    const char **names = FS_ListFiles( "materials", "", FS_LIST_ALL, &count );
    for ( int i = 0; i < count; ++i )
    {
        if ( !names[i] || names[i][0] == '$' )      // '$' is a built-in, never a template
            continue;
        blob.clear();
        m = ParsedMaterial();
        if ( !ReadMaterialFile( names[i], &blob ) )
            continue;
        if ( !ParseMaterial( blob, &m ) || !TemplateShapeOk( row, m ) )
            continue;
        g_resolved[index] = names[i];
        break;
    }
    FS_FreeFileList( names );
    return g_resolved[index].empty() ? nullptr : g_resolved[index].c_str();
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  EXISTENCE PROBES
// ═════════════════════════════════════════════════════════════════════════════════════
bool KiwiMat_ExistsOnDisk( const char *name )
{
    if ( !name || !name[0] )
        return false;
    char qpath[128];
    _snprintf( qpath, sizeof( qpath ), "materials/%s", name );
    qpath[sizeof( qpath ) - 1] = '\0';
    int fh = 0;
    com_fileAccessed = 1;
    FS_FOpenFileRead( qpath, &fh );
    if ( !fh )
        return false;
    FS_FCloseFile( fh );
    return true;
}

bool KiwiIwi_ExistsOnDisk( const char *imageName )
{
    if ( !imageName || !imageName[0] )
        return false;
    char qpath[128];
    _snprintf( qpath, sizeof( qpath ), "images/%s.iwi", imageName );
    qpath[sizeof( qpath ) - 1] = '\0';
    int fh = 0;
    com_fileAccessed = 1;
    FS_FOpenFileRead( qpath, &fh );
    if ( !fh )
        return false;
    FS_FCloseFile( fh );
    return true;
}

void KiwiMat_DeleteWritten( const char *materialName, const char *imageName )
{
    char qpath[128];
    if ( materialName && materialName[0] )
    {
        _snprintf( qpath, sizeof( qpath ), "materials/%s", materialName );
        qpath[sizeof( qpath ) - 1] = '\0';
        FS_Delete( qpath );
    }
    if ( imageName && imageName[0] )
    {
        _snprintf( qpath, sizeof( qpath ), "images/%s.iwi", imageName );
        qpath[sizeof( qpath ) - 1] = '\0';
        FS_Delete( qpath );
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  THE WRITER
// ═════════════════════════════════════════════════════════════════════════════════════
bool KiwiMat_Write( int templateIndex, const kiwiMatFields_t *f, char *err, size_t errSz )
{
    if ( !f || !f->name[0] )
    {
        SetErr( err, errSz, "no material name" );
        return false;
    }
    // kiwi_matwriter.h D-BE-I: refuse to write a header the browser gate would reject.
    if ( !f->usage || !f->locale || !f->autoTexScaleWidth || !f->autoTexScaleHeight )
    {
        SetErr( err, errSz,
                "usage/locale/autoTexScale must all be non-zero or Load_Materials (texwnd.cpp:455) hides the material" );
        return false;
    }
    // Material_LoadFile builds "materials/%s" into a 64-byte buffer with Com_sprintf
    // (r_material_load_obj.cpp:5277-5279), and Image_LoadFromFileWithReader builds
    // "images/%s.iwi" into 64 as well (r_image_load_obj.cpp:403).  "images/" + ".iwi" is
    // 11 characters, so 52 is the hard ceiling for the image name.
    if ( strlen( f->name ) > 50 || strlen( f->imageName ) > 50 )
    {
        SetErr( err, errSz, "name too long (the engine's path buffers are 64 bytes)" );
        return false;
    }

    const char *tmplName = KiwiMat_ResolveTemplate( templateIndex );
    if ( !tmplName )
    {
        SetErr( err, errSz, "no usable template material for this type in this data tree" );
        return false;
    }

    std::vector<unsigned char> tblob;
    ParsedMaterial             t;
    if ( !ReadMaterialFile( tmplName, &tblob ) || !ParseMaterial( tblob, &t ) )
    {
        SetErr( err, errSz, "could not parse the template material '%s'", tmplName );
        return false;
    }

    // ── build the string block ─────────────────────────────────────────────────────
    // Shipped order (D-BE-F): techSetName, materialName, colormap image, texture entry
    // names, constant names.  Interned so a repeated string costs one copy.
    std::vector<char>        strings;
    std::vector<std::string> interned;
    std::vector<unsigned>    internedOff;

    const unsigned texTabOff   = (unsigned)sizeof( MaterialRaw );                             // 0x40
    const unsigned constTabOff = texTabOff + 12u * (unsigned)t.textures.size();
    const unsigned stringBase  = constTabOff + 20u * (unsigned)t.constants.size();

    // Append `s` to the string block once and hand back its blob-relative offset.
    auto intern = [&]( const std::string &s ) -> unsigned
    {
        for ( size_t i = 0; i < interned.size(); ++i )
            if ( interned[i] == s )
                return internedOff[i];
        const unsigned off = stringBase + (unsigned)strings.size();
        strings.insert( strings.end(), s.begin(), s.end() );
        strings.push_back( '\0' );
        interned.push_back( s );
        internedOff.push_back( off );
        return off;
    };

    const std::string newName  = f->name;
    const std::string newImage = f->imageName[0] ? std::string( f->imageName ) : newName;

    const unsigned offTech  = intern( t.techSet );
    const unsigned offName  = intern( newName );
    const unsigned offImage = intern( newImage );

    std::vector<unsigned> texNameOff( t.textures.size(), 0 );
    std::vector<unsigned> texImgOff( t.textures.size(), 0 );
    for ( size_t i = 0; i < t.textures.size(); ++i )
    {
        texNameOff[i] = intern( t.textures[i].name );
        texImgOff[i]  = ( t.textures[i].semantic == 2 ) ? offImage : intern( t.textures[i].image );
    }
    std::vector<unsigned> constNameOff( t.constants.size(), 0 );
    for ( size_t i = 0; i < t.constants.size(); ++i )
        constNameOff[i] = intern( t.constants[i].name );

    // ── assemble ───────────────────────────────────────────────────────────────────
    std::vector<unsigned char> out( (size_t)stringBase + strings.size(), 0 );

    MaterialRaw *raw = (MaterialRaw *)&out[0];
    raw->info                        = t.info;               // carry every field we do not own
    raw->info.nameOffset             = offName;
    raw->info.refImageNameOffset     = offImage;             // shipped files point it at the colormap
    raw->info.usage                  = f->usage;
    raw->info.locale                 = f->locale;
    raw->info.autoTexScaleWidth      = f->autoTexScaleWidth;
    raw->info.autoTexScaleHeight     = f->autoTexScaleHeight;
    // Keep every non-surface-type bit the template carries (nodraw/nonsolid/… live there
    // too — me_fence_chainlink ships 0xD00020) and replace only the 0x1F00000 field that
    // Material_LoadRaw:5878 turns into surfaceTypeBits and TexWnd_FilterAccept (texwnd.cpp:911) filters on.
    // surfaceType < 0 is the wizard's "(from template)" row: leave the whole field alone.
    if ( f->surfaceType >= 0 )
        raw->info.surfaceFlags = ( t.info.surfaceFlags & ~0x1F00000 ) | ( f->surfaceType & 0x1F00000 );
    raw->refStateBits[0]             = t.refStateBits[0];
    raw->refStateBits[1]             = t.refStateBits[1];
    raw->textureCount                = (unsigned short)t.textures.size();
    raw->constantCount               = (unsigned short)t.constants.size();
    raw->techSetNameOffset           = offTech;
    raw->textureTableOffset          = texTabOff;
    raw->constantTableOffset         = constTabOff;

    for ( size_t i = 0; i < t.textures.size(); ++i )
    {
        MaterialTextureDefRaw *d = (MaterialTextureDefRaw *)( &out[0] + texTabOff + 12 * i );
        d->nameOffset       = texNameOff[i];
        d->samplerState     = t.textures[i].samplerState;
        d->semantic         = t.textures[i].semantic;
        d->u.imageNameOffset = texImgOff[i];
    }
    for ( size_t i = 0; i < t.constants.size(); ++i )
    {
        MaterialConstantDefRaw *d = (MaterialConstantDefRaw *)( &out[0] + constTabOff + 20 * i );
        d->nameOffset = constNameOff[i];
        for ( int k = 0; k < 4; ++k )
            d->literal[k] = t.constants[i].literal[k];
    }
    if ( !strings.empty() )
        memcpy( &out[0] + stringBase, &strings[0], strings.size() );

    // ── write ──────────────────────────────────────────────────────────────────────
    char qpath[128];
    _snprintf( qpath, sizeof( qpath ), "materials/%s", f->name );
    qpath[sizeof( qpath ) - 1] = '\0';

    const int h = FS_FOpenFileWrite( qpath );
    if ( !h )
    {
        SetErr( err, errSz, "could not open '%s' for writing", qpath );
        return false;
    }
    const unsigned wrote = FS_Write( (const char *)&out[0], (unsigned)out.size(), h );
    FS_FCloseFile( h );
    if ( wrote != (unsigned)out.size() )
    {
        FS_Delete( qpath );
        SetErr( err, errSz, "short write to '%s' (%u of %u bytes)", qpath, wrote, (unsigned)out.size() );
        return false;
    }
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  ROUND-TRIP GATE, STAGE 2 + 3
// ═════════════════════════════════════════════════════════════════════════════════════
bool KiwiMat_Verify( const char *name, kiwiMatVerify_t *outInfo, char *err, size_t errSz )
{
    if ( outInfo )
        memset( outInfo, 0, sizeof( *outInfo ) );
    if ( !name || !name[0] )
    {
        SetErr( err, errSz, "no material name" );
        return false;
    }

    // ── STAGE 3a: read the header back and apply the Load_Materials gate ───────────
    std::vector<unsigned char> blob;
    ParsedMaterial             m;
    if ( !ReadMaterialFile( name, &blob ) )
    {
        SetErr( err, errSz, "the engine filesystem cannot find 'materials/%s' after writing it", name );
        return false;
    }
    if ( !ParseMaterial( blob, &m ) )
    {
        SetErr( err, errSz, "'materials/%s' does not parse with Material_LoadRaw's own offsets", name );
        return false;
    }
    if ( m.name != name )
    {
        // Not fatal to the loader (it uses whatever nameOffset says), but it would mean a
        // searchpath copy shadows ours — worth failing loudly, see D-BE-H.
        SetErr( err, errSz,
                "'materials/%s' reads back with the embedded name '%s' - another searchpath is shadowing the file",
                name, m.name.c_str() );
        return false;
    }
    if ( !m.info.usage || !m.info.locale || !m.info.autoTexScaleWidth || !m.info.autoTexScaleHeight )
    {
        SetErr( err, errSz,
                "browser gate FAILED (texwnd.cpp:455): usage=%u locale=%u autoTexScale=%ux%u",
                (unsigned)m.info.usage, (unsigned)m.info.locale,
                (unsigned)m.info.autoTexScaleWidth, (unsigned)m.info.autoTexScaleHeight );
        return false;
    }

    if ( outInfo )
    {
        outInfo->gameFlags          = m.info.gameFlags;
        outInfo->sortKey            = m.info.sortKey;
        outInfo->usage              = m.info.usage;
        outInfo->locale             = m.info.locale;
        outInfo->autoTexScaleWidth  = m.info.autoTexScaleWidth;
        outInfo->autoTexScaleHeight = m.info.autoTexScaleHeight;
        outInfo->surfaceFlags       = m.info.surfaceFlags;
        outInfo->contents           = m.info.contents;
        _snprintf( outInfo->techSet, sizeof( outInfo->techSet ), "%s", m.techSet.c_str() );
        outInfo->techSet[sizeof( outInfo->techSet ) - 1] = '\0';
        for ( size_t i = 0; i < m.textures.size(); ++i )
        {
            if ( m.textures[i].semantic != 2 )
                continue;
            _snprintf( outInfo->colorMapImage, sizeof( outInfo->colorMapImage ), "%s",
                       m.textures[i].image.c_str() );
            outInfo->colorMapImage[sizeof( outInfo->colorMapImage ) - 1] = '\0';
            break;
        }
    }

    // ── STAGE 2: the REAL engine load ─────────────────────────────────────────────
    // Material_RegisterHandle (r_material.cpp:584) -> Material_Register -> Material_Load
    // (r_material_load_obj.cpp:5974) -> Material_LoadRaw (:5826) -> Image_Register (:5929)
    // -> Image_Load -> Image_LoadFromFile -> Image_LoadFromFileWithReader (r_image_load_obj.cpp:389).
    // So this ONE call exercises the material reader AND the .iwi reader end to end, on
    // exactly the two files we just wrote.  "wc/" because that is how the editor registers a
    // world material (texwnd.cpp:238-242) and it is what selects the "wc_" techset prefix.
    //
    // CAVEAT, honestly stated: Material_Register_LoadObj (r_material.cpp:554) returns a
    // CACHED Material* when the name is already in rg.materialHashTable, so on an OVERWRITE
    // of a name this session has already registered, this stage re-validates the cached
    // material rather than re-reading the new file.  The header check ABOVE always reads the
    // new bytes off disk, so the browser gate is still proved either way; only the "the
    // engine can parse this blob" half is skipped in that one case, and the caller's
    // Image_Reload keeps the PIXELS current.  A failed load is NOT cached
    // (Material_MakeDefault, r_material.cpp:520-531, never calls Material_Add), so a retry
    // after a fix always re-reads.
    char full[128];
    _snprintf( full, sizeof( full ), "wc/%s", name );
    full[sizeof( full ) - 1] = '\0';

    Material *mtl = Material_RegisterHandle( full, 0 );
    if ( !mtl )
    {
        SetErr( err, errSz, "Material_RegisterHandle('%s') returned null", full );
        return false;
    }
    if ( Material_IsDefault( mtl ) )
    {
        SetErr( err, errSz,
                "the engine loader rejected 'materials/%s' and fell back to the default material "
                "(the console lines above say why)", name );
        return false;
    }

    // The colormap the loader actually bound, and whether its .iwi decoded far enough to
    // produce a D3D texture.  Image_LoadFromData sets texture.basemap (r_image_load_obj.cpp:541
    // clears it, then the format arm uploads); a null one means the reader took the file but
    // produced nothing.
    bool sawColorMap = false;
    for ( int i = 0; i < mtl->textureCount; ++i )
    {
        if ( mtl->textureTable[i].semantic != 2 )
            continue;
        GfxImage *img = mtl->textureTable[i].u.image;
        if ( !img )
            break;
        sawColorMap = true;
        if ( outInfo )
        {
            outInfo->colorMapWidth    = img->width;
            outInfo->colorMapHeight   = img->height;
            outInfo->colorMapUploaded = ( img->texture.basemap != nullptr );
        }
        if ( !img->texture.basemap )
        {
            SetErr( err, errSz,
                    "the engine read 'images/%s.iwi' but produced no texture", img->name ? img->name : "?" );
            return false;
        }
        break;
    }
    if ( !sawColorMap )
    {
        SetErr( err, errSz, "the loaded material exposes no colorMap" );
        return false;
    }
    return true;
}

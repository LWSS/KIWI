// kiwi_matwriter.cpp — the binary material writer (mechanical half).  The blob layout and
// the constraints are in kiwi_matwriter.h.
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
extern int  Sys_Printf( const char *fmt, ... );          // win_qe3.cpp:118   int Sys_Printf(const char*,...)

// Every offset this file writes is `sizeof`-derived, so only a struct edit can drift the
// layout.  r_material.h declares these without size asserts of its own.
static_assert( sizeof( MaterialInfoRaw )        == 0x28, "MaterialInfoRaw must be 40 bytes" );
static_assert( sizeof( MaterialRaw )            == 0x40, "MaterialRaw must be 64 bytes" );
static_assert( sizeof( MaterialTextureDefRaw )  == 0x0C, "MaterialTextureDefRaw must be 12 bytes" );
static_assert( sizeof( MaterialConstantDefRaw ) == 0x14, "MaterialConstantDefRaw must be 20 bytes" );

// ── THE TEMPLATE SET ───────────────────────────────────────────────────────────────
// Every candidate is a shipped material that already passes the browser gate and whose
// non-slot images are all built-ins ('$…'), so the clone substitutes only the slots
// `info.slots` declares and inherits valid bindings for the rest.
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
        "Standard lit world surface. The usual choice for walls, floors and terrain.",
        KIWI_MAT_SLOT_COLOR },
      { "ch_concrete_01", "ch_brick_wall_03", "case256x32red", nullptr, nullptr }, 4, 2 },

    { { "World - lit, alpha test",   "l_sm_t0c0",
        "Lit, with hard cut-out transparency from the image's alpha. Fences, foliage, grates.",
        KIWI_MAT_SLOT_COLOR },
      { "icbm_wirescreen", "me_curtains_1", "ch_windowslatbroke01_a", nullptr, nullptr }, 4, 2 },

    { { "World - lit, alpha blend",  "l_sm_b0c0",
        "Lit, with soft alpha blending and a decal sort key. Signs, posters, painted overlays.",
        KIWI_MAT_SLOT_COLOR },
      { "ch_decal_mural", "ad_sign256x256", "ad_sign128x256", nullptr, nullptr }, 12, 2 },

    { { "World - unlit, opaque",     "unlit",
        "Fullbright, ignores lighting entirely. Pre-baked art and light-panel surfaces.",
        KIWI_MAT_SLOT_COLOR },
      { "ac_building_wall01", "ac_building_door01", "ac_ground_base01", nullptr, nullptr }, 4, 1 },

    // ── families that consume a normal and/or specular slot ────────────────────────
    { { "World - lit, normal + specular",              "l_sm_r0c0n0s0",
        "Fully detailed lit surface. Shiny/detailed surfaces: metals, machined trim, wet stone.",
        KIWI_MAT_SLOT_COLOR | KIWI_MAT_SLOT_NORMAL | KIWI_MAT_SLOT_SPECULAR },
      { "ch_brick_wall_01", "ch_concrete_03", "ch_ceiling01", "ch_concrete_base_01",
        "ch_brick_wall_05" }, 4, 3 },

    { { "World - lit, alpha test, normal + specular",  "l_sm_t0c0n0s0",
        "Fully detailed with hard cut-out transparency. Metal grates, chain-link, railings.",
        KIWI_MAT_SLOT_COLOR | KIWI_MAT_SLOT_NORMAL | KIWI_MAT_SLOT_SPECULAR },
      { "ch_factory_floorgrate", "me_floor_metalgratingparallel_1side", "me_railing_plaster",
        "me_floor_metalgratingparallel", "me_fence_chainlink" }, 4, 3 },

    { { "World - lit, alpha blend, normal + specular", "l_sm_b0c0n0s0",
        "Fully detailed soft-blended overlay with a decal sort key. Relief decals, grime, wet patches.",
        KIWI_MAT_SLOT_COLOR | KIWI_MAT_SLOT_NORMAL | KIWI_MAT_SLOT_SPECULAR },
      { "ch_dec_concretewall01", "me_decal_brick", "ch_concrete_floor01_dec",
        "me_metal_peelingpaint_01", "ch_patchydirt01" }, 12, 3 },

    { { "World - lit, normal",                         "l_sm_r0c0n0",
        "Lit and bumped but not shiny. Plaster, rough concrete, fabric, carpet.",
        KIWI_MAT_SLOT_COLOR | KIWI_MAT_SLOT_NORMAL },
      { "case512", "case256", "ap_office_carpet", "case1024", "case" }, 4, 2 },

    { { "World - lit, alpha test, normal",             "l_sm_t0c0n0",
        "Bumped, with hard cut-out transparency and no specular. Rugs, cut-out signage.",
        KIWI_MAT_SLOT_COLOR | KIWI_MAT_SLOT_NORMAL },
      { "com_persian_rug_01", "ap_signage_interior_set_1", "com_ch_me_rug_01",
        "ap_signage_interior_set_2", "ch_factory_beambraces" }, 12, 2 },

    { { "World - lit, alpha blend, normal",            "l_sm_b0c0n0",
        "Bumped soft-blended overlay, decal sort key, no specular. Chipped paint, patches, tracks.",
        KIWI_MAT_SLOT_COLOR | KIWI_MAT_SLOT_NORMAL },
      { "ch_wallpaintchipped_1_dec", "me_decal_concretepatch", "me_decal_wall_chipped_1",
        "ch_dec_carpettorn", "me_decal_bullet_holes02" }, 12, 2 },

    { { "World - lit, specular",                       "l_sm_r0c0s0",
        "Shiny but flat: reflection without relief. Tile, painted metal, polished stone.",
        KIWI_MAT_SLOT_COLOR | KIWI_MAT_SLOT_SPECULAR },
      { "ch_reactor_metal_3", "ch_tile_floor01", "ch_tile_wall_01", "me_wallpaper2",
        "ch_wallwhiteblue01" }, 4, 3 },

    { { "World - glass (alpha blend + specular)",      "l_sm_b0c0s0",
        "What the shipped glass uses: blended, reflective, glass surface type and glass contents.",
        KIWI_MAT_SLOT_COLOR | KIWI_MAT_SLOT_SPECULAR },
      { "ap_glass_sanded", "com_glass_clear", "me_glass", "me_railingpanelsblueglass",
        "mtl_glass_clear_test" }, 43, 3 },
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

// Parse a blob with EXACTLY the offsets Material_LoadRaw dereferences.
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

// semantic <-> slot, the one place the mapping is written down.
unsigned SlotForSemantic( unsigned char semantic )
{
    if ( semantic == 2 ) return KIWI_MAT_SLOT_COLOR;
    if ( semantic == 5 ) return KIWI_MAT_SLOT_NORMAL;
    if ( semantic == 8 ) return KIWI_MAT_SLOT_SPECULAR;
    return 0;
}

// The built-in a slot's texture entry binds when the wizard has no image for it — the same
// fallbacks Image_AssignDefaultTexture uses for semantics 5 and 8.
const char *BuiltinForSlot( unsigned slot )
{
    if ( slot == KIWI_MAT_SLOT_NORMAL )   return "$identitynormalmap";
    if ( slot == KIWI_MAT_SLOT_SPECULAR ) return "$black";
    return nullptr;
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
    // Exactly one colormap; every entry whose slot this family CONSUMES must carry real art;
    // every other image must be a built-in ('$…'), because the clone replaces only the slots
    // it owns and must not inherit somebody else's art.
    int colorMaps = 0;
    for ( size_t i = 0; i < m.textures.size(); ++i )
    {
        if ( m.textures[i].semantic == 11 )        // a water def, not an image name
            return false;
        if ( m.textures[i].image.empty() )
            return false;
        const unsigned slot    = SlotForSemantic( m.textures[i].semantic );
        const bool     builtin = ( m.textures[i].image[0] == '$' );
        if ( m.textures[i].semantic == 2 )
            ++colorMaps;
        if ( slot != 0 && ( row.info.slots & slot ) != 0 )
        {
            if ( builtin )
                return false;                      // this family is supposed to supply art here
            continue;
        }
        if ( !builtin )
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

// ── TEMPLATE RESOLUTION ────────────────────────────────────────────────────────────
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

// ── EXISTENCE PROBES ───────────────────────────────────────────────────────────────
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

void KiwiMat_DeleteWritten( const char *materialName, const char *colorImage,
                            const char *normalImage, const char *specularImage )
{
    char qpath[128];
    if ( materialName && materialName[0] )
    {
        _snprintf( qpath, sizeof( qpath ), "materials/%s", materialName );
        qpath[sizeof( qpath ) - 1] = '\0';
        FS_DeleteInDir( qpath, (char *)"raw" );
    }
    const char *images[3] = { colorImage, normalImage, specularImage };
    for ( int i = 0; i < 3; ++i )
    {
        // A '$…' name is a built-in, never a file we wrote.
        if ( !images[i] || !images[i][0] || images[i][0] == '$' )
            continue;
        _snprintf( qpath, sizeof( qpath ), "images/%s.iwi", images[i] );
        qpath[sizeof( qpath ) - 1] = '\0';
        FS_DeleteInDir( qpath, (char *)"raw" );
    }
}

// ── THE WRITER ─────────────────────────────────────────────────────────────────────
bool KiwiMat_Write( int templateIndex, const kiwiMatFields_t *f, char *err, size_t errSz )
{
    if ( !f || !f->name[0] )
    {
        SetErr( err, errSz, "no material name" );
        return false;
    }
    // Refuse to write a header the browser gate would reject.
    if ( !f->usage || !f->locale || !f->autoTexScaleWidth || !f->autoTexScaleHeight )
    {
        SetErr( err, errSz,
                "usage/locale/autoTexScale must all be non-zero or Load_Materials (texwnd.cpp:455) hides the material" );
        return false;
    }
    // Both "materials/%s" and "images/%s.iwi" are built into 64-byte engine buffers, and
    // "images/" + ".iwi" is 11 characters.
    if ( strlen( f->name ) > 50 || strlen( f->imageName ) > 50
         || strlen( f->normalImageName ) > 50 || strlen( f->specularImageName ) > 50 )
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
    // Shipped order: techSetName, materialName, colormap image, texture entry names,
    // constant names.  Interned so a repeated string costs one copy.
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

    // The substitution table, one row per slot.  Everything not named here — samplerState,
    // semantic, the entry NAME, the constant table, the rest of MaterialInfoRaw — is carried
    // from the template verbatim, never synthesised.
    const TemplateRow &row = kTemplates[templateIndex];
    struct SlotSub { unsigned slot; const char *image; };
    const SlotSub subs[3] =
    {
        { KIWI_MAT_SLOT_COLOR,    newImage.c_str()       },
        { KIWI_MAT_SLOT_NORMAL,   f->normalImageName     },
        { KIWI_MAT_SLOT_SPECULAR, f->specularImageName   },
    };

    std::vector<unsigned> texNameOff( t.textures.size(), 0 );
    std::vector<unsigned> texImgOff( t.textures.size(), 0 );
    for ( size_t i = 0; i < t.textures.size(); ++i )
    {
        texNameOff[i] = intern( t.textures[i].name );

        const unsigned slot = SlotForSemantic( t.textures[i].semantic );
        const char    *bind = nullptr;
        if ( slot != 0 && ( row.info.slots & slot ) != 0 )
        {
            for ( int s = 0; s < 3; ++s )
                if ( subs[s].slot == slot )
                    bind = ( subs[s].image && subs[s].image[0] ) ? subs[s].image
                                                                 : BuiltinForSlot( slot );
            // COLOR is never optional: newImage falls back to f->name, which was already
            // checked non-empty, so `bind` is non-null on that arm by construction.
        }
        texImgOff[i] = bind ? intern( std::string( bind ) )
                            : intern( t.textures[i].image );   // not our slot: template verbatim
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
    // too) and replace only the 0x1F00000 surfaceTypeBits field.  surfaceType < 0 is the
    // wizard's "(from template)" row: leave the whole field alone.
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

    // raw/ rather than fs_gamedir ("main"): the map compilers' searchpaths are
    // raw/raw_shared/devraw only, so an import into main/ is invisible to them.
    const int h = FS_FOpenFileWriteToDir( qpath, "raw" );
    if ( !h )
    {
        SetErr( err, errSz, "could not open '%s' for writing", qpath );
        return false;
    }
    const unsigned wrote = FS_Write( (const char *)&out[0], (unsigned)out.size(), h );
    FS_FCloseFile( h );
    if ( wrote != (unsigned)out.size() )
    {
        FS_DeleteInDir( qpath, (char *)"raw" );
        SetErr( err, errSz, "short write to '%s' (%u of %u bytes)", qpath, wrote, (unsigned)out.size() );
        return false;
    }
    return true;
}

// ── ROUND-TRIP GATE, STAGE 2 + 3 ───────────────────────────────────────────────────
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
        // Not fatal to the loader, but it means a searchpath copy shadows ours.
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
        // All three slots, from the bytes that were just read back.
        for ( size_t i = 0; i < m.textures.size(); ++i )
        {
            char *dst = nullptr;
            if ( m.textures[i].semantic == 2 )      dst = outInfo->colorMapImage;
            else if ( m.textures[i].semantic == 5 ) dst = outInfo->normalMapImage;
            else if ( m.textures[i].semantic == 8 ) dst = outInfo->specularMapImage;
            if ( !dst || dst[0] )
                continue;
            _snprintf( dst, 64, "%s", m.textures[i].image.c_str() );
            dst[63] = '\0';
        }
    }

    // ── STAGE 2: the REAL engine load ─────────────────────────────────────────────
    // This one call drags the material reader and the .iwi reader down end to end.  "wc/" is
    // how the editor registers a world material and is what selects the "wc_" techset prefix.
    // CAVEAT: registration returns a CACHED Material* when the name is already in
    // rg.materialHashTable, so an OVERWRITE of a name registered this session re-validates
    // the cached material.  The header check ABOVE always reads the new bytes off disk.  A
    // failed load is never cached, so a retry after a fix always re-reads.
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

    // What the loader actually bound per slot, and whether each .iwi decoded far enough to
    // produce a D3D texture (a null texture.basemap means the reader took the file but
    // produced nothing).  A missing COLORMAP texture is fatal; a normal/specular entry that
    // failed to decode is only reported, because Image_AssignDefaultTexture has already
    // substituted a valid fallback and the material renders.
    bool sawColorMap = false;
    for ( int i = 0; i < mtl->textureCount; ++i )
    {
        const unsigned char sem = mtl->textureTable[i].semantic;
        if ( sem != 2 && sem != 5 && sem != 8 )
            continue;
        GfxImage *img = mtl->textureTable[i].u.image;
        if ( !img )
            continue;
        if ( outInfo )
        {
            int  *w  = ( sem == 2 ) ? &outInfo->colorMapWidth
                     : ( sem == 5 ) ? &outInfo->normalMapWidth : &outInfo->specularMapWidth;
            int  *h  = ( sem == 2 ) ? &outInfo->colorMapHeight
                     : ( sem == 5 ) ? &outInfo->normalMapHeight : &outInfo->specularMapHeight;
            bool *up = ( sem == 2 ) ? &outInfo->colorMapUploaded
                     : ( sem == 5 ) ? &outInfo->normalMapUploaded : &outInfo->specularMapUploaded;
            *w  = img->width;
            *h  = img->height;
            *up = ( img->texture.basemap != nullptr );
        }
        if ( sem != 2 )
            continue;
        sawColorMap = true;
        if ( !img->texture.basemap )
        {
            SetErr( err, errSz,
                    "the engine read 'images/%s.iwi' but produced no texture", img->name ? img->name : "?" );
            return false;
        }
    }
    if ( !sawColorMap )
    {
        SetErr( err, errSz, "the loaded material exposes no colorMap" );
        return false;
    }
    return true;
}

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Clone a shipped material around a new name and images. Cloning is required because
// refStateBits[2] is compiler output remapped by per-techset state maps with no documented
// source grammar.
//
// Material blob layout, little-endian:
//   +0x00  MaterialRaw (r_material.h:589, sizeof 0x40)
//     +0x00 u32 nameOffset             (char*)mtlRaw + it
//     +0x04 u32 refImageNameOffset     never dereferenced by the loader; kept valid anyway
//     +0x08 u8  gameFlags
//     +0x09 u8  sortKey                (bcassert < 0x40)
//     +0x0A u8  textureAtlasRowCount
//     +0x0B u8  textureAtlasColumnCount
//     +0x0C f32 maxDeformMove          not read by Material_LoadRaw; carried from the template
//     +0x10 u8  deformFlags            (ditto)
//     +0x11 u8  usage                  editor + browser gate
//     +0x12 u16 toolFlags
//     +0x14 u32 locale                 editor + browser gate
//     +0x18 u16 autoTexScaleWidth      EDITOR ONLY — texwnd.cpp:419
//     +0x1A u16 autoTexScaleHeight     EDITOR ONLY — texwnd.cpp:420
//     +0x1C f32 tessSize               not read by Material_LoadRaw; carried from the template
//     +0x20 s32 surfaceFlags           surfaceTypeBits = (surfaceFlags & 0x1F00000) >> 20
//     +0x24 s32 contents               EDITOR ONLY — texwnd.cpp:415
//     +0x28 u32 refStateBits[2]
//     +0x30 u16 textureCount
//     +0x32 u16 constantCount
//     +0x34 u32 techSetNameOffset      (char*)mtlRaw + it
//     +0x38 u32 textureTableOffset     (char*)mtlRaw + it
//     +0x3C u32 constantTableOffset    (char*)mtlRaw + it
//   +0x40  MaterialTextureDefRaw[textureCount] (r_material.h:146, sizeof 0xC)
//     +0x0 u32 nameOffset       hashed with R_HashString
//     +0x4 u8  samplerState     (state & 7) must be non-zero; masked &0x1F
//     +0x5 u8  semantic         2 = colorMap, 5 = normalMap, 8 = specularMap, 11 = water
//     +0x6     2 padding bytes
//     +0x8 u32 u.imageNameOffset
//   +0x40 + 0xC*textureCount  MaterialConstantDefRaw[constantCount] (r_material.h:156, 0x14)
//     +0x00 u32 nameOffset      must be non-zero; strncpy 12
//     +0x04 f32 literal[4]
//   NUL-terminated strings follow and are addressed only by the offsets above. The loader
//   qsorts texture and constant tables by entry-name hash; cloning preserves those names, so
//   serialized table order need not match the source.
//
// "wc/" is a registration prefix stripped by Material_Load; the disk path is materials/<name>.
// Writes target raw/ because map compilers search raw/raw_shared/devraw; the editor also
// searches raw/.
//
// Load_Materials skips zero usage, locale, or autoTexScale dimensions; TexWnd_FilterAccept also
// rejects usage_index 0, so KiwiMat_Write refuses such headers.

#include <stddef.h>

// Unfilled slots use $identitynormalmap (semantic 5) or $black (semantic 8). Written names use
// <name>, <name>_nml, and <name>_spc for color, normal, and specular maps.

// ── the shipped template set ────────────────────────────────────────────────────────
// Each family combines an `l_sm_<r|t|b>0c0[d0][n0][s0]` techset (opaque/test/blend plus
// color/detail/normal/specular) with its texture-entry shape. Named candidates are tried in
// order, then materials/ is scanned for a matching shape.
enum kiwiMatSlot_t
{
    KIWI_MAT_SLOT_COLOR    = 1,   // MaterialTextureDefRaw::semantic 2
    KIWI_MAT_SLOT_NORMAL   = 2,   //                                5
    KIWI_MAT_SLOT_SPECULAR = 4,   //                                8
};

struct kiwiMatTemplateInfo_t
{
    const char  *label;       // the wizard's dropdown text
    const char  *techSet;     // the techset name the file must carry
    const char  *blurb;       // one line of help under the dropdown
    unsigned int slots;       // KIWI_MAT_SLOT_* this family's techset consumes as real art
};

int                          KiwiMat_TemplateCount();
const kiwiMatTemplateInfo_t *KiwiMat_TemplateInfo( int index );
// Resolve to a cached shipped material name, or null when this data tree has no usable template.
const char                  *KiwiMat_ResolveTemplate( int index );

// ── the per-material fields the wizard collects ────────────────────────────────────
struct kiwiMatFields_t
{
    char          name[64];        // material + image base name (already sanitised)
    char          imageName[64];   // colormap image name (usually == name)
    // EMPTY means "do not substitute": the clone binds the built-in for a slot its family
    // consumes, or keeps the template's own binding for one it does not.
    char          normalImageName[64];
    char          specularImageName[64];
    unsigned char usage;           // filter_usage_array[..].index   (must be non-zero)
    unsigned int  locale;          // 1 << filter_locale_array[..].index (must be non-zero)
    unsigned short autoTexScaleWidth;   // must be non-zero
    unsigned short autoTexScaleHeight;  // must be non-zero
    int           surfaceType;     // filter_surfacetype_array[..].index, already << 20;
                                   // NEGATIVE = keep the template's surfaceFlags verbatim
};

// Clone into materials/<f->name>; failure leaves nothing behind and writes a reason to `err`.
bool KiwiMat_Write( int templateIndex, const kiwiMatFields_t *f, char *err, size_t errSz );

// Read through the writer's parser. Legacy 2D materials may use semantic 0 instead of a
// semantic-2 color map; colorMapImage then takes the first real non-water image and
// colorMapSemantic records its source.
struct kiwiMatSource_t
{
    unsigned char  gameFlags;
    unsigned char  sortKey;
    unsigned char  usage;
    unsigned short toolFlags;
    unsigned int   locale;
    unsigned short autoTexScaleWidth;
    unsigned short autoTexScaleHeight;
    int            surfaceFlags;
    int            contents;
    unsigned int   refStateBits[2];
    char           techSet[64];
    char           colorMapImage[64];
    unsigned char  colorMapSemantic;
    char           normalMapImage[64];
    char           specularMapImage[64];
};
bool KiwiMat_ReadSource( const char *name, kiwiMatSource_t *outInfo, char *err, size_t errSz );

// Verify the disk header/browser gate; an engine-loader cache miss also exercises .iwi loading.
struct kiwiMatVerify_t
{
    unsigned char  gameFlags;
    unsigned char  sortKey;
    unsigned char  usage;
    unsigned int   locale;
    unsigned short autoTexScaleWidth;
    unsigned short autoTexScaleHeight;
    int            surfaceFlags;
    int            contents;
    char           techSet[64];
    char           colorMapImage[64];
    int            colorMapWidth;
    int            colorMapHeight;
    bool           colorMapUploaded;   // GfxImage::texture.basemap != null after the load
    // `*Image` is empty for no entry and names the built-in for an unfilled entry.
    char           normalMapImage[64];
    int            normalMapWidth;
    int            normalMapHeight;
    bool           normalMapUploaded;
    char           specularMapImage[64];
    int            specularMapWidth;
    int            specularMapHeight;
    bool           specularMapUploaded;
};
bool KiwiMat_Verify( const char *name, kiwiMatVerify_t *outInfo, char *err, size_t errSz );

bool KiwiMat_ExistsOnDisk( const char *name );
bool KiwiIwi_ExistsOnDisk( const char *imageName );

// Roll back one import; null, empty, and built-in ('$…') names are skipped.
void KiwiMat_DeleteWritten( const char *materialName, const char *colorImage,
                            const char *normalImage, const char *specularImage );

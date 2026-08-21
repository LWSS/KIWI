#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_matwriter.h — the binary material writer: read a shipped material, rebuild it around
// a new name + new images, write it back into the library.  Pure I/O, no editor state.
// It CLONES rather than synthesises because refStateBits[2] is compiler output remapped
// through the techset's per-pass state maps, with no grammar documented anywhere in the tree.
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
//   then a string block of NUL-terminated strings, addressed only by the offsets above.  The
//   shipped order, reproduced here: techSetName, materialName, colormapImageName, each
//   texture entry's NAME, each constant's name.
//   The loader qsorts the texture and constant tables in place by R_HashString of the entry
//   NAME; cloning preserves every entry name, so that order needs no reproducing here.
//
// The "wc/" of Register_WorldMaterial is a registration prefix, not a directory: Material_Load
// strips it, so the file on disk is plain materials/<name>.  Files land in
// <gameroot>/raw/materials/<name> and <gameroot>/raw/images/<name>.iwi via
// FS_FOpenFileWriteToDir(…, "raw") — raw/ rather than fs_gamedir ("main") because the map
// compilers' searchpaths are raw/raw_shared/devraw only; the editor searches raw/ too.
//
// BROWSER GATE, hard requirement: Load_Materials (texwnd.cpp:479) skips any material whose
// usage, locale, autoTexScaleWidth or autoTexScaleHeight is zero, and TexWnd_FilterAccept
// (texwnd.cpp:925) rejects usage_index == 0.  KiwiMat_Write refuses to write such a header.

#include <stddef.h>

// Unfilled slots bind the engine's own semantic fallbacks: $identitynormalmap (semantic 5)
// and $black (semantic 8).  Written image names follow the tree's convention: the colour map
// keeps <name>, the normal map is <name>_nml, the specular map is <name>_spc.

// ── the shipped template set ────────────────────────────────────────────────────────
// Each row is a FAMILY: a techset name (`l_sm_<r|t|b>0c0[d0][n0][s0]` — r = opaque, t = alpha
// test, b = alpha blend, c = colour, d = detail, n = normal, s = specular) plus the
// texture-entry shape its members have.  The named candidates are tried in order; if none is
// present in this data tree, KiwiMat_ResolveTemplate falls back to scanning materials/ for
// the first file with the same techset name and a compatible shape.
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
// Resolve `index` to a shipped material file name (cached).  Null if this data tree has no
// usable template for that family — the wizard greys the row out.
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

// Clone `templateIndex` into materials/<f->name>.  Returns false with a reason in `err`;
// on failure nothing is left behind.
bool KiwiMat_Write( int templateIndex, const kiwiMatFields_t *f, char *err, size_t errSz );

// Round-trip gate stage 2 + 3: register the material through the engine's own loader (which
// proves the .iwi too), then re-read the header off disk and apply the browser gate to it.
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
    // `*Image` is empty when the loaded material has no such entry; it names the BUILT-IN
    // when the entry is present but unfilled.
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

// Does materials/<name> already exist anywhere on the searchpaths?
bool KiwiMat_ExistsOnDisk( const char *name );
// Does images/<name>.iwi already exist anywhere on the searchpaths?
bool KiwiIwi_ExistsOnDisk( const char *imageName );

// Delete what one import attempt wrote (rollback).  Every argument is optional — null, empty
// and built-in names ('$…') are skipped.
void KiwiMat_DeleteWritten( const char *materialName, const char *colorImage,
                            const char *normalImage, const char *specularImage );

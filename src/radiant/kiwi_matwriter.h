#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_matwriter.h — KIWI-UX (ROUND BE): THE BINARY MATERIAL WRITER.
// ═════════════════════════════════════════════════════════════════════════════════════
// Pure I/O.  No ImGui, no editor state.  Read a shipped material, rebuild it around a new
// name + a new colormap, write it back into the library.
//
// ── D-BE-E: WHY TEMPLATE CLONING AND NOT SYNTHESIS ──────────────────────────────────
// A material file is not self-contained: `Material_LoadRaw` (r_material_load_obj.cpp:5826)
// resolves the techset by NAME through `Material_RegisterTechniqueSet` (:5515) and then
// `Material_BuildStateBitsTable` (:5953) remaps `refStateBits[2]` through that techset's
// per-pass state maps.  Those two uint32s are compiler output — there is no documented
// grammar for them anywhere in this tree, and getting them wrong does not fail loudly, it
// produces a material that draws with the wrong blend/depth state.  A shipped material
// that already renders correctly carries a KNOWN-GOOD (techset name, refStateBits, sortKey,
// gameFlags, toolFlags, constant literals) tuple.  Cloning keeps that tuple intact and
// changes only the things that are genuinely per-material: the name, the colormap image,
// and the editor/browser metadata the wizard collects.
//
// ── D-BE-F: THE MATERIAL BLOB LAYOUT, DERIVED FIELD BY FIELD FROM THE LOADER ────────
// Every offset below is a line of Material_LoadRaw / Material_FinishLoadingInstance that
// dereferences it.  The layout was then confirmed against the shipped set (4353 files, all
// parse with these offsets).
//
//   +0x00  MaterialRaw (r_material.h:589, sizeof 0x40)
//     +0x00 u32 nameOffset            (char*)mtlRaw + it            r_material_load_obj.cpp:5853
//     +0x04 u32 refImageNameOffset    NEVER dereferenced by the loader (grep: r_material.h:573
//                                     is its only mention) — kept valid anyway, pointed at the
//                                     colormap image name exactly as the shipped files do.
//     +0x08 u8  gameFlags                                           :5862
//     +0x09 u8  sortKey                                             :5898  (bcassert < 0x40 :5495)
//     +0x0A u8  textureAtlasRowCount                                :5899
//     +0x0B u8  textureAtlasColumnCount                             :5900
//     +0x0C f32 maxDeformMove         (not read by Material_LoadRaw; carried from the template)
//     +0x10 u8  deformFlags           (ditto)
//     +0x11 u8  usage                 EDITOR: Material.editorUsage  :5875, and the browser gate
//     +0x12 u16 toolFlags             Material_BuildStateBitsTable  :5953; editorToolFlags :5874
//     +0x14 u32 locale                EDITOR: Material.editorLocale :5876, and the browser gate
//     +0x18 u16 autoTexScaleWidth     EDITOR ONLY — texwnd.cpp:412 (the browser thumbnail size)
//     +0x1A u16 autoTexScaleHeight    EDITOR ONLY — texwnd.cpp:413
//     +0x1C f32 tessSize              (not read by Material_LoadRaw; carried from the template)
//     +0x20 s32 surfaceFlags          :5869 (editor stash), :5878 (surfaceTypeBits =
//                                     (surfaceFlags & 0x1F00000) >> 20), :5965 (bit 0x40000)
//     +0x24 s32 contents              EDITOR ONLY — texwnd.cpp:415
//     +0x28 u32 refStateBits[2]                                     :5953
//     +0x30 u16 textureCount                                        :5901
//     +0x32 u16 constantCount                                       :5902
//     +0x34 u32 techSetNameOffset     (char*)mtlRaw + it            :5514
//     +0x38 u32 textureTableOffset    (char*)mtlRaw + it            :5907 / :5496
//     +0x3C u32 constantTableOffset   (char*)mtlRaw + it            :5938 / :5505
//
//   +0x40  MaterialTextureDefRaw[textureCount] (r_material.h:146, sizeof 0xC)
//     +0x0 u32 nameOffset       R_HashString((char*)mtlRaw + it)    :5910-5913
//     +0x4 u8  samplerState     assert (state & 7) != 0             :5915; masked &0x1F :5467
//     +0x5 u8  semantic         2 = colorMap, 5 = normalMap, 8 = specularMap, 11 = water :5922
//     +0x6      2 padding bytes
//     +0x8 u32 u.imageNameOffset  Image_Register((char*)mtlRaw+it)  :5929-5931
//
//   +0x40 + 0xC*textureCount  MaterialConstantDefRaw[constantCount] (r_material.h:156, 0x14)
//     +0x00 u32 nameOffset      must be non-zero :5508; strncpy 12  :5941-5944
//     +0x04 f32 literal[4]                                          :5946-5950
//
//   then a STRING BLOCK of NUL-terminated strings, addressed only by the offsets above.
//   The shipped order (verified by dumping the bytes of ac_building_wall01, 150 bytes) is:
//     techSetName, materialName, colormapImageName, then each texture entry's NAME, then
//     each constant's name.  This writer reproduces that order; the loader does not care,
//     but a diff against a shipped file should read as a diff and not as a rewrite.
//
//   NOTE — the loader SORTS IN PLACE.  Material_FinishLoadingInstance qsorts the texture
//   table (:5503) and the constant table (:5512) by R_HashString of the entry NAME, in the
//   caller's own buffer.  Because cloning preserves every entry NAME and only substitutes an
//   IMAGE name, the sort order the shipped file already has is preserved, so nothing here
//   has to reproduce that hash order.
//
// ── D-BE-G: THE "wc/" PREFIX IS A REGISTRATION PREFIX, NOT A DIRECTORY ──────────────
// `Register_WorldMaterial` (texwnd.cpp:238) registers "wc/<name>"; `Material_Load`
// (r_material_load_obj.cpp:5987-5995) then STRIPS the matching g_materialTypeInfo prefix
// before `Material_LoadFile` builds "materials/%s" (:5279).  So the file on disk is
// `materials/<name>` with no prefix — which is what the shipped set shows: every world
// material this editor browses (ch_concrete_01, ad_sign256x256, …) is a plain file directly
// under materials/, and texwnd.cpp:218's own header read opens `materials/<name>` too.
// The prefix survives only into the TECHSET name (`Material_FinishLoadingInstance` :5514
// prepends g_materialTypeInfo[4].techniqueSetPrefix "wc_"), which is why the template must
// be a material that already registers cleanly as a world material.
//
// ── D-BE-H: WHERE THE FILES LAND, AND WHY BOTH SIDES FIND THEM ─────────────────────
// `FS_FOpenFileWrite` (com_files.cpp:496) writes to <fs_homepath>/<fs_gamedir>/<qpath>.
//   * fs_homepath == fs_basepath in this build (com_files.cpp:1291-1293: homePath falls back
//     to fs_basepath->reset when the platform hook returns nothing), and fs_basepath is the
//     exe-derived game root (com_files.cpp:1234-1281, the KISAK_RADIANT block).
//   * fs_gamedir is the LAST non-localized folder FS_AddGameDirectory saw (com_files.cpp:1666).
//     FS_InitFilesystem calls FS_Startup("main") (com_files.cpp:2034) and FS_Startup's last
//     two adds are "main_shared" then "main" (com_files.cpp:1959-1961), and inside each add
//     FS_AddLocalizedGameDirectory (com_files.cpp:1689-1696) ends with the NON-localized
//     FS_AddGameDirectory — so fs_gamedir == "main".
//   * FS_AddSearchPath PREPENDS non-localized paths (com_files.cpp:1308-1309), so the last
//     directory added is the FIRST one searched: main beats raw beats devraw.
// Net: we write <gameroot>/main/materials/<name> and <gameroot>/main/images/<name>.iwi —
// literally the directory the 4353 shipped materials and 6268 shipped .iwi files live in,
// at the TOP of the search order, and on a searchpath the game builds identically (it calls
// the same FS_Startup("main")).  No direct path construction is needed and none is done.
//
// ── D-BE-I: THE BROWSER GATE IS A HARD REQUIREMENT ─────────────────────────────────
// Load_Materials (texwnd.cpp:455) skips any material whose usage, locale, autoTexScaleWidth
// or autoTexScaleHeight is zero, and TexWnd_FilterAccept (texwnd.cpp:896) independently
// rejects usage_index == 0.  KiwiMat_Write REFUSES to write a header that would fail that
// gate rather than silently producing an invisible material.
// ═════════════════════════════════════════════════════════════════════════════════════

#include <stddef.h>

// ── the shipped template set ────────────────────────────────────────────────────────
// Each row is a FAMILY, described by the techset name and the texture-entry shape its
// members have.  The named candidates are tried in order; if none of them is present in
// this data tree, KiwiMat_ResolveTemplate falls back to scanning materials/ for the first
// file with the same techset name and a compatible shape.
struct kiwiMatTemplateInfo_t
{
    const char *label;        // the wizard's dropdown text
    const char *techSet;      // the techset name the file must carry
    const char *blurb;        // one line of help under the dropdown
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

// ROUND-TRIP GATE, STAGE 2 + 3.  Registers the material through the ENGINE's own loader
// (Material_RegisterHandle -> Material_Load -> Material_LoadRaw -> Image_Register ->
// Image_LoadFromFileWithReader, so it proves the .iwi as well), then re-reads the 0x28
// header off disk and applies the Load_Materials browser gate to it.  `outInfo`, when
// given, receives the gate fields for the console summary.
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
};
bool KiwiMat_Verify( const char *name, kiwiMatVerify_t *outInfo, char *err, size_t errSz );

// Does materials/<name> already exist anywhere on the searchpaths?
bool KiwiMat_ExistsOnDisk( const char *name );
// Does images/<name>.iwi already exist anywhere on the searchpaths?
bool KiwiIwi_ExistsOnDisk( const char *imageName );

// Delete a written pair (used to roll back a half-completed import).
void KiwiMat_DeleteWritten( const char *materialName, const char *imageName );

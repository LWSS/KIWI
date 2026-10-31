#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\texwnd.cpp
//
// The texture browser.  It opens with the MATERIAL-REGISTRATION part (the window, palette
// draw and scroll/click follow further down).  These three functions are the back half of
// SetMaterial's real dispatch (materialdef.cpp):
// a named material → a cached qtexture_s wrapping a registered engine Material*.
//   Texture_GetHandle (0x45a8e0)      — name → cached qtexture_s (registers if new)
//   Register_WorldMaterial (0x45a7b0) — name → engine Material* ("wc/<name>")
//   Editor_AddRadiantMaterial (0x45a5b0) — alloc + fill a qtexture_s, add to the list
//
// CoD3/CoD4 DIVERGENCE ADAPTATION (P3 ruling — kisak engine is authoritative):
// the binary reads autoTexScaleWidth/contents/surfaceFlags/usage/locale out of a CoD4
// 56-byte MaterialInfo that kisak's runtime MaterialInfo (0x18) does not retain.  They
// are read from the material file's raw MaterialInfoRaw header instead
// (Radiant_ReadMaterialInfo); without one, the editor-filter fields come from the
// editor-widened Material (editorUsage/editorLocale/surfaceFlags, stashed in
// Material_LoadRaw) and width/height from the material's colormap image.  Either way
// TexWnd_FilterAccept's usage/locale/surfaceType browser filter works on real data.
// Register_WorldMaterial still follows the binary's wc/<name> then optional
// wc/<name>_editor registration path.

#include "stdafx.h"
#include <universal/surfaceflags.h>
#include "qe3.h"
#include "prefs.h"                  // g_PrefsDlg->m_nTextureWindowScale / m_bTextureScrollbar
#include "kiwi_matconvert.h"        // KIWI-UX: unlit conversion context menu + Shift+L scan hook
#include "kiwi_matprops.h"          // KIWI: right-click "Material properties"
#include "kiwi_mathdrcache.h"       // KIWI: boot-time material header cache (cold-start fix)
#include "kiwi_floatsnap.h"         // KIWI: KiwiRound_Fistp
#include "kiwi_ported.h"            // KIWI: shared engine/editor externs
#include "kiwi_texcolor.h"          // KIWI: colour filter + In Use & View filter
#include "kiwi_matwriter.h"         // KIWI: KiwiMat_ReadSource (colour filter's colour map name)
#include "kiwi_camera.h"            // KIWI: KiwiCam_Ortho / OrthoHalfHeight (In Use & View)
#include "kiwi_section.h"           // KIWI: a live section cut hides faces above it
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_font.h>         // Font_s::pixelHeight (TexWnd_SetupIter)
#include <universal/com_files.h>    // FS_ListFiles/FS_Read/FS_FOpenFileRead — bulk material load
#include <universal/profile.h>

// ── radiant-local helpers (home files) ────────────────────────────────────────
extern char *AllocMaterialString( const char *src );   // qe3.cpp     (0x48b030)
extern void  MainFrm_SetStatusText( int pane, const char *text ); // mainfrm.cpp (status pane sink)

// ─────────────────────────────────────────────────────────────────────────────
// texWndGlob_textureOffset (0x25d7990, texwnd_s, 0x1006c bytes).  The field offsets
// are the binary's.
// ─────────────────────────────────────────────────────────────────────────────
// nPos_s (16 bytes) — per-(simple|layered) texture-window scroll state.
struct nPos_s
{
    int nPos_current;          // 0x00  scroll offset (px)
    int nPos_max;              // 0x04  content extent (scrollbar range)
    int nPos_layered_current;  // 0x08  (the layered-material sub-view — parked)
    int nPos_layered_max;      // 0x0c
};

struct texwnd_s
{
    int          textureOffset;                 // 0x00
    int          usageCount;                    // 0x04
    int          localeCount;                   // 0x08
    // 1-byte INDEX fields (0..count-1; the filter handlers assert it fits a byte), not
    // the booleans the binary's types say; `usageFilter + 60000` recovers the menu item id.
    byte usageFilter;                  // 0x0c  (usage  filter index)
    byte localeFilter;                 // 0x0d  (locale filter index)
    byte surfaceTypeFilter;            // 0x0e  (surface-type filter index)
    bool         searchbar_filter;              // 0x0f
    const char  *searchbar_buffer;              // 0x10
    int          materialCount;                 // 0x14
    qtexture_s  *sorted_materials[16384];       // 0x18   (0x10000 bytes)
    qtexture_s  *qtextures;                      // 0x10018 (linked-list head, via ->prev)
    int          unk_8;                          // 0x1001c
    bool         m_bNeedRange;                   // 0x10020  scrollbar range valid
    char         _pad10021[3];                   // 0x10021
    int          m_nWidth;                       // 0x10024  pane client width  (px)
    int          m_nHeight;                      // 0x10028  pane client height (px)
    int          lastButtonDown;                 // 0x1002c
    int          m_ptDown[2];                    // 0x10030  CPoint
    bool         m_was_mouse_dragged;            // 0x10038
    bool         unk_bool;                        // 0x10039  (layered-material wnd active)
    char         _pad1003a[2];                   // 0x1003a
    nPos_s       nPos[3];                         // 0x1003c  per-layer scroll (current/max)
};
static_assert(sizeof(texwnd_s) == (sizeof(void *) == 8 ? 131192 : 0x1006c), "texwnd_s must match the IDB (0x1006c)");

texwnd_s texWndGlob_textureOffset = {};

// Head of the registered-material list (texWndGlob.qtextures @0x10018), walked via
// ->prev.  texwnd_s is TU-local, so out-of-file walkers (Map_LoadFile's in-use clear)
// go through this accessor.
qtexture_s *TexWnd_GetMaterialListHead() { return texWndGlob_textureOffset.qtextures; }

// Accessor for filters.cpp case-6 `Misc decal` seed (texWndGlob.unk_8 @0x1001c).  FillTextureMenu
// sets this to 1<<localeIndex("decal") from locale.txt — with the stock locale.txt
// ("case","test","tools","decal",...) "decal" is locale index 3, so unk_8 = 1<<3 = 8.
int TexWnd_GetDecalLocaleBit() { return texWndGlob_textureOffset.unk_8; }

// Loaded usage/locale filter counts (texwnd_s is TU-local) — used by CMainFrame::OnCreate's
// FillTextureMenu logging.
int texWndGlob_textureOffset_usageCount()  { return texWndGlob_textureOffset.usageCount; }
int texWndGlob_textureOffset_localeCount() { return texWndGlob_textureOffset.localeCount; }

// Layered-material window "live add" flag (texwnd_s.unk_bool @0x10039).  The layered-
// material tool palette (layeredmaterialwnd.cpp LayeredMaterialWnd_ToggleLiveAdd, 0x417440)
// sets this so the texture-window click-apply path can add a layer instead of replacing
// the face material.  Accessor avoids exporting the TU-local texwnd_s.
extern "C" void TexWnd_SetLayeredMaterialActive( int active )
{
    texWndGlob_textureOffset.unk_bool = ( active != 0 );
}

// ─────────────────────────────────────────────────────────────────────────────
// Editor material auto-texture scale: the binary uses the CoD4 MaterialInfo's
// autoTexScaleWidth/Height (≈ the colormap resolution). kisak does not retain it,
// so derive it from the material's first valid colormap image. Fallback 512.
// ─────────────────────────────────────────────────────────────────────────────
static int Radiant_OriginalImageDim( uint16_t dim, const GfxImage *img )
{
    int shift = img ? img->picmip.platform[0] : 0; // PICMIP_PLATFORM_USED
    int original = (int)dim;
    while ( shift-- > 0 && original < 32768 )
        original <<= 1;
    return original > 0 ? original : 512;
}

static void Radiant_MaterialTexScale( Material *mtl, int *outW, int *outH )
{
    *outW = 512;
    *outH = 512;
    if ( !mtl || !mtl->textureTable )
        return;

    // The binary's editor path reads autoTexScaleWidth/Height from the CoD4 material
    // header, i.e. the color-map texel density.  When we must fall back to the
    // runtime material, do not use textureTable[0]: the table is hash-sorted and
    // often starts with normalMap/specularMap.
    for ( int i = 0; i < mtl->textureCount; ++i )
    {
        if ( mtl->textureTable[i].semantic != TS_COLOR_MAP )
            continue;
        GfxImage *img = mtl->textureTable[i].u.image;
        if ( img && img->width && img->height )
        {
            *outW = Radiant_OriginalImageDim( img->width, img );
            *outH = Radiant_OriginalImageDim( img->height, img );
            return;
        }
    }

    for ( int i = 0; i < mtl->textureCount; ++i )
    {
        if ( mtl->textureTable[i].semantic == TS_WATER_MAP )
            continue;
        GfxImage *img = mtl->textureTable[i].u.image;
        if ( img && img->width && img->height )
        {
            *outW = Radiant_OriginalImageDim( img->width, img );
            *outH = Radiant_OriginalImageDim( img->height, img );
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Register_WorldMaterial (0x45a7b0) — register the editor world-material variant
// ("wc/<name>") and, when flagged, try the "_editor" world-material variant.
// ─────────────────────────────────────────────────────────────────────────────
struct RadiantMaterialInfo
{
    uint8_t  gameFlags;
    uint8_t  usage;
    uint16_t toolFlags;
    uint locale;
    uint16_t autoTexScaleWidth;
    uint16_t autoTexScaleHeight;
    int      surfaceFlags;
    int      contents;
};

static bool Material_ReadEditorVariant( const char *editorName, MaterialInfoRaw *out );

static void Radiant_InfoFromRaw( const MaterialInfoRaw *raw, RadiantMaterialInfo *out )
{
    out->gameFlags          = raw->gameFlags;
    out->usage              = raw->usage;
    out->toolFlags          = raw->toolFlags;
    out->locale             = raw->locale;
    out->autoTexScaleWidth  = raw->autoTexScaleWidth;
    out->autoTexScaleHeight = raw->autoTexScaleHeight;
    out->surfaceFlags       = raw->surfaceFlags;
    out->contents           = raw->contents;
}

static void Radiant_InfoFromMaterialFallback( Material *mtl, const MaterialInfo *info, RadiantMaterialInfo *out )
{
    int w, h;
    Radiant_MaterialTexScale( mtl, &w, &h );
    out->gameFlags          = info ? info->gameFlags : 0;
    out->usage              = mtl ? mtl->editorUsage : 0;
    out->toolFlags          = 0;
    out->locale             = mtl ? mtl->editorLocale : 0;
    out->autoTexScaleWidth  = (uint16_t)w;
    out->autoTexScaleHeight = (uint16_t)h;
    out->surfaceFlags       = mtl ? mtl->surfaceFlags : 0;
    out->contents           = 0;
}

static void Radiant_ReadMaterialInfo( const char *name, Material *mtl,
                                      const MaterialInfo *runtimeInfo,
                                      RadiantMaterialInfo *outInfo )
{
    MaterialInfoRaw raw;
    if ( Material_ReadEditorVariant( name, &raw ) )
    {
        if ( raw.toolFlags & 0x1000 )
        {
            char editorName[80];
            strncpy( editorName, name, 64 );
            editorName[64] = '\0';
            I_strncat( editorName, sizeof( editorName ), "_editor" );

            MaterialInfoRaw editorRaw;
            if ( Material_ReadEditorVariant( editorName, &editorRaw ) )
                raw = editorRaw;
        }
        Radiant_InfoFromRaw( &raw, outInfo );
        return;
    }

    Radiant_InfoFromMaterialFallback( mtl, runtimeInfo, outInfo );
}

static Material *Register_WorldMaterial( const char *name, RadiantMaterialInfo *outInfo )
{
    char full[256];
    Com_sprintf( full, sizeof( full ), "wc/%s", name );
    Material *mtl = Material_RegisterHandle( full, IMAGE_TRACK_MISC );
    MaterialInfo runtimeInfo;
    // The binary never falls back to the base "<name>" material here.
    Material_GetInfo( mtl, &runtimeInfo );

    MaterialInfoRaw raw;
    if ( Material_ReadEditorVariant( name, &raw ) && ( raw.toolFlags & 0x1000 ) )
    {
        char editorName[80];
        strncpy( editorName, name, 64 );
        editorName[64] = '\0';
        I_strncat( editorName, sizeof( editorName ), "_editor" );

        char editorFull[256];
        Com_sprintf( editorFull, sizeof( editorFull ), "wc/%s", editorName );
        Material *editorMtl = Material_RegisterHandle( editorFull, IMAGE_TRACK_MISC );
        if ( !Material_IsDefault( editorMtl ) )
        {
            mtl = editorMtl;
            Material_GetInfo( mtl, &runtimeInfo );
        }
    }

    Radiant_ReadMaterialInfo( name, mtl, &runtimeInfo, outInfo );
    return mtl;
}

// ─────────────────────────────────────────────────────────────────────────────
// Editor_AddRadiantMaterial (0x45a5b0) — allocate a qtexture_s, fill it from the
// registered Material*, and prepend it to texWndGlob's material list.
// ─────────────────────────────────────────────────────────────────────────────
static qtexture_s *Editor_AddRadiantMaterial( const RadiantMaterialInfo *mtlInfo,
                                              const char *name, Material *mtl )
{
    iassert( mtlInfo->autoTexScaleWidth != 0 && mtlInfo->autoTexScaleHeight != 0 );   // TexWnd.cpp:209

    qtexture_s *newRadMat = new qtexture_s();   // value-init → all zero
    iassert(newRadMat);

    // The binary fills these from the embedded CoD4 56-byte MaterialInfo (usage@+0x1C,
    // locale@+0x20, surfaceFlags); `mtlInfo` carries the same fields (Register_WorldMaterial:
    // the raw header, else the values stashed on the Material).  A default/missing material
    // has usage 0 → filtered out, as the binary rejects usage_index==0 in TexWnd_FilterAccept.
    newRadMat->next                        = mtl;                         // 0x00  THE rendered handle
    newRadMat->name                        = AllocMaterialString( name ); // 0x04
    newRadMat->is_in_use                   = true;                        // 0x08
    newRadMat->usage_index                 = (char)mtlInfo->usage;        // 0x0A
    newRadMat->tex_num_or_localefilter     = (int)mtlInfo->locale;        // 0x10
    newRadMat->unk_flags2                  = (uint16_t)mtlInfo->toolFlags;// 0x0C  LOWORD(toolFlags)
    newRadMat->unk1                        = (char)mtlInfo->gameFlags;    // 0x09
    newRadMat->width                       = mtlInfo->autoTexScaleWidth;  // 0x14
    newRadMat->height                      = mtlInfo->autoTexScaleHeight; // 0x18
    newRadMat->color_or_surfacetype_filter = mtlInfo->surfaceFlags;       // 0x1C  surfaceFlags (full; the
                                                                          //       filter ANDs with SURF_TYPE_MASK)
    newRadMat->in_use                      = mtlInfo->contents;           // 0x20

    newRadMat->prev = texWndGlob_textureOffset.qtextures;                // 0x24  list link
    texWndGlob_textureOffset.qtextures = newRadMat;
    texWndGlob_textureOffset.sorted_materials[texWndGlob_textureOffset.materialCount++] = newRadMat;
    return newRadMat;
}

// ─────────────────────────────────────────────────────────────────────────────
// Texture_GetHandle (0x45a8e0) — name → cached qtexture_s. Walks the registered
// list (linked via ->prev); registers + adds on first reference. The found-with-
// null-handle branch lazily registers the engine material.
// ─────────────────────────────────────────────────────────────────────────────
qtexture_s *Texture_GetHandle( const char *name )
{
    if ( strlen( name ) >= 0x40 )
        Com_PrintMessage( "texture name '%s' is too long", name );

    char lowercaseName[256];
    strncpy( lowercaseName, name, sizeof( lowercaseName ) - 1 );
    lowercaseName[sizeof( lowercaseName ) - 1] = '\0';
    _strlwr( lowercaseName );
    iassert(lowercaseName[0] != '(');

    RadiantMaterialInfo info;

    for ( qtexture_s *q = texWndGlob_textureOffset.qtextures; q; q = q->prev )
    {
        if ( !_stricmp( lowercaseName, q->name ) )
        {
            bool needHandle = ( q->next == nullptr );
            q->is_in_use = true;
            if ( needHandle )
                q->next = Register_WorldMaterial( lowercaseName, &info );
            return q;
        }
    }

    // Not in the list — register the engine material and add a wrapper.
    Material *mtl = Register_WorldMaterial( lowercaseName, &info );
    if ( Material_IsDefault( mtl ) && I_stricmp( name, "$default" ) )
        Sys_Printf( "\t\t\tWARNING: Could not find material '%s'\n", name );
    return Editor_AddRadiantMaterial( &info, lowercaseName, mtl );
}

// ═════════════════════════════════════════════════════════════════════════════
//  BULK MATERIAL LOAD — populate the texture browser at editor startup with EVERY
//  material in the searchpaths, not just the ones the loaded map references.
//    Load_Materials                   (0x45ae40) — FS_ListFilteredFiles("materials") loop
//    Material_ConvertToEditorMaterial (0x45aa30) — raw 0x28 header → editor qtexture
//    Material_IsEditorVariant         (0x45a6d0) — name ends with "_editor"
//    Editor_DoesMaterialExist         (0x45adb0) — dup check by name
//    Material_ReadEditorVariant       (0x45ade0) / Material_CopyEditorString (0x45a790)
//
//  Each material's on-disk 0x28 MaterialInfoRaw header carries autoTexScaleWidth/Height
//  + usage/locale/toolFlags/gameFlags/surfaceFlags/contents DIRECTLY, and the qtexture's
//  editor-filter fields are filled from it field-for-field as the binary's
//  Editor_AddRadiantMaterial does.  The render handle (q->next) is left
//  NULL: DrawMaterials lazily binds it via Texture_GetHandle when the thumbnail scrolls
//  into view, so startup stays cheap (header read + alloc, no 4000+ material registrations).
// ═════════════════════════════════════════════════════════════════════════════

// 0x45a6d0 — does `name` end with the "_editor" world-craft suffix?
static bool Material_IsEditorVariant( const char *name )
{
    size_t n = strlen( name );
    if ( n < 7 )
        return false;
    return strcmp( name + n - 7, "_editor" ) == 0;
}

// 0x45adb0 — find an already-registered qtexture by name (case-insensitive walk).
static qtexture_s *Editor_DoesMaterialExist( const char *name )
{
    for ( qtexture_s *q = texWndGlob_textureOffset.qtextures; q; q = q->prev )
        if ( !_stricmp( name, q->name ) )
            return q;
    return nullptr;
}

// 0x45ade0 — open "materials/<editorName>" and read its 0x28 header into `out`.
static bool Material_ReadEditorVariant( const char *editorName, MaterialInfoRaw *out )
{
    int h = 0;
    com_fileAccessed = 1;
    FS_FOpenFileRead( va( "materials/%s", editorName ), &h );
    if ( !h )
        return false;
    uint got = FS_Read( (uint8_t *)out, sizeof( MaterialInfoRaw ), h );
    FS_FCloseFile( h );
    return got == sizeof( MaterialInfoRaw );
}

// 0x45aa30 + 0x45a5b0 — raw header → browser qtexture (the binary's field assignments,
// sourced from MaterialInfoRaw).  Returns the existing entry if already registered.
static qtexture_s *Material_ConvertToEditorMaterial( const MaterialInfoRaw *raw,
                                                     const char *name )
{
    qtexture_s *existing = Editor_DoesMaterialExist( name );
    if ( existing )
        return existing;

    qtexture_s *q = new qtexture_s();                              // value-init → all zero
    q->next                        = nullptr;                      // 0x00  lazy render handle
    q->name                        = AllocMaterialString( name );  // 0x04
    q->is_in_use                   = true;                         // 0x08
    q->unk1                        = (char)raw->gameFlags;         // 0x09
    q->usage_index                 = (char)raw->usage;             // 0x0A
    q->unk_flags2                  = (uint16_t)raw->toolFlags;     // 0x0C  (LOWORD = toolFlags)
    q->tex_num_or_localefilter     = (int)raw->locale;            // 0x10
    q->width                       = raw->autoTexScaleWidth;       // 0x14
    q->height                      = raw->autoTexScaleHeight;      // 0x18
    q->color_or_surfacetype_filter = raw->surfaceFlags;           // 0x1C
    q->in_use                      = raw->contents;               // 0x20
    q->prev = texWndGlob_textureOffset.qtextures;                 // 0x24  list link
    texWndGlob_textureOffset.qtextures = q;
    texWndGlob_textureOffset.sorted_materials[texWndGlob_textureOffset.materialCount++] = q;
    return q;
}

// 0x45ae40 — enumerate every material in the searchpaths and register the renderable,
// editor-visible ones (valid usage/locale + a real auto-scale) into the browser.
void Load_Materials( void )
{

    if ( !fs_searchpaths )
    {
        Sys_Printf( "Load_Materials: filesystem not initialized\n" );
        return;
    }

    PROF_SCOPED( "Load_Materials" );
    int          numMaterials = 0;
    const char **names = FS_ListFiles( "materials", "", FS_LIST_ALL, &numMaterials );
    int          added = 0;

    // KIWI (2026-09-19): the 40-byte headers come from one cache file while the material
    // files are unchanged (size + write time) - kiwi_mathdrcache.h has the why.  A miss takes
    // the binary's own open / read / close below and is then cached.
    KiwiMatHdr_Begin( sizeof( MaterialInfoRaw ) );

    for ( int i = 0; i < numMaterials; ++i )
    {
        const char *name = names[i];
        if ( Material_IsEditorVariant( name ) )
            continue;

        MaterialInfoRaw raw;
        const kiwiMatHdr_t cached = KiwiMatHdr_Get( name, &raw );
        if ( cached == KMATHDR_BAD )
            continue;
        if ( cached == KMATHDR_MISS )
        {
            int h = 0;
            com_fileAccessed = 1;
            FS_FOpenFileRead( va( "materials/%s", name ), &h );
            if ( !h )
            {
                KiwiMatHdr_Put( name, nullptr );
                continue;
            }
            uint got = FS_Read( (uint8_t *)&raw, sizeof( raw ), h );
            FS_FCloseFile( h );
            if ( got != sizeof( raw ) )
            {
                KiwiMatHdr_Put( name, nullptr );
                continue;
            }
            KiwiMatHdr_Put( name, &raw );
        }

        // Binary's nested-non-zero gate: skip materials that aren't browsable
        // (no usage/locale or a degenerate auto-scale that would div-by-zero the grid).
        if ( !raw.usage || !raw.locale || !raw.autoTexScaleWidth || !raw.autoTexScaleHeight )
            continue;

        // toolFlags & 0x1000 → a separate "<name>_editor" header overrides this one.
        if ( raw.toolFlags & 0x1000 )
        {
            char editorName[80];
            strncpy( editorName, name, 64 );
            editorName[64] = '\0';
            I_strncat( editorName, sizeof( editorName ), "_editor" );
            MaterialInfoRaw editorRaw;
            const kiwiMatHdr_t editorCached = KiwiMatHdr_Get( editorName, &editorRaw );
            if ( editorCached == KMATHDR_HIT )
                raw = editorRaw;
            else if ( editorCached == KMATHDR_MISS )
            {
                const bool ok = Material_ReadEditorVariant( editorName, &editorRaw );
                KiwiMatHdr_Put( editorName, ok ? &editorRaw : nullptr );
                if ( ok )
                    raw = editorRaw;
            }
        }

        Material_ConvertToEditorMaterial( &raw, name );
        ++added;
    }

    KiwiMatHdr_End();
    FS_FreeFileList( names );
    Sys_Printf( "Load_Materials: registered %d of %d materials into the texture browser\n",
                added, numMaterials );
}

// ═════════════════════════════════════════════════════════════════════════════
//  Textures→Show In Use — mark/count the textures the
//  loaded map actually references, so the browser can show only them.
//    tex_set_is_in_use (0x45B6E0) / Texture_ForName (0x45B760) / Texture_ShowInuse (0x45B850).
//  Also runs at the END of every map load (map.cpp Map_LoadFromFile → Texture_ShowInuse),
//  which is why it must be real (the in-use flag drives the usage filter + the count line).
// ═════════════════════════════════════════════════════════════════════════════

// 0x45B6E0 — the Texture_ForName per-MaterialDef realize callback: flag the qtexture
// in-use. (Binary returns the qtexture*; MaterialDef_02 discards it — return value unused.)
static int tex_set_is_in_use( qtexture_s *q )
{
    q->is_in_use = true;
    return 0;
}

// 0x45A4D0 — qsort comparator for sorted_materials[] (each entry is a qtexture_s*):
// case-insensitive by material name.
static int __cdecl Tex_stricmp( const void *a1, const void *a2 )
{
    return _stricmp( ( *(qtexture_s **)a1 )->name, ( *(qtexture_s **)a2 )->name );
}

// 0x45B760 — Texture_ForName: walk a brush list and flag every referenced material's
// qtexture in-use (via MaterialDef_02 → tex_set_is_in_use). `layer` = the material
// channel (0 = base). Recurses into prefab child brush lists:
//   - fixedsize (point) entity → recurse owner->prefab->active_brushlist.
//   - else, patch brush       → flag &patch->texture[layer] (patchMesh_material stride 8).
//   - else, plain brush       → flag &def->faces[i].mtldef[layer] for the DEF faceCount
//     (the authoritative count).
void Texture_ForName( selbrush_t *list, int layer )
{
    for ( selbrush_t *b = list->next; b && b != list; b = b->next )
    {
        brush_t *def = b->def;
        if ( *(int *)&def->owner->eclass->fixedsize )
        {
            // Point/bbox entity: descend into its prefab's brush list (if any).
            void *prefab = b->owner->prefab;
            if ( prefab )
                Texture_ForName( &((prefab_s *)prefab)->brushes, layer );
        }
        else if ( def->patch )
        {
            // Patch: one MaterialDef per layer at &patch->texture[layer].
            MaterialDef_02( (MaterialDef *)( &def->patch->texture + layer ), tex_set_is_in_use );
        }
        else
        {
            for ( int i = 0; i < def->faceCount; ++i )
                MaterialDef_02( &def->faces[i].mtldef[layer], tex_set_is_in_use );
        }
    }
}

// 0x45B850 — Texture_ShowInuse: clear all in-use flags + the usage/locale/surface-type
// filters, re-flag the materials used by the active + selected brushes, re-sort the
// browser list, invalidate, and report the count.
LRESULT Texture_ShowInuse()
{
    int layer = g_qeglobals.current_edit_layer;
    texWndGlob_textureOffset.nPos[layer].nPos_current         = 0;
    texWndGlob_textureOffset.nPos[layer].nPos_layered_current = 0;
    texWndGlob_textureOffset.localeFilter      = 0;
    texWndGlob_textureOffset.usageFilter       = 0;
    texWndGlob_textureOffset.surfaceTypeFilter = 0;

    for ( qtexture_s *q = texWndGlob_textureOffset.qtextures; q; q = q->prev )
        q->is_in_use = false;

    SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                  (LPARAM)"Selecting active textures\n" );
    Texture_ForName( &active_brushes,   0 );
    Texture_ForName( &selected_brushes, 0 );

    qsort( texWndGlob_textureOffset.sorted_materials,
           texWndGlob_textureOffset.materialCount, sizeof( qtexture_s * ), Tex_stricmp );

    g_nUpdateBits |= W_TEXTURE;

    int n = 0;
    for ( qtexture_s *q = texWndGlob_textureOffset.qtextures; q; q = q->prev )
        if ( q->is_in_use )
            ++n;
    return Sys_Printf( "%d textures in use\n", n );
}

// 0x45B730 — Texture_ShowAll: mark EVERY registered qtexture in-use, so the browser shows the
// full registered set rather than only the selection's.  The Textures->Show All path (Ctrl-A,
// cmd 32973).  Unlike Show In Use it does NOT re-filter or re-sort — it just un-hides everything.
LRESULT Texture_ShowAll()
{
    for ( qtexture_s *q = texWndGlob_textureOffset.qtextures; q; q = q->prev )
        q->is_in_use = true;
    g_nUpdateBits |= W_TEXTURE;
    return Sys_Printf( "Showing all textures...\n" );
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BE): LIVE REGISTRATION OF ONE JUST-WRITTEN MATERIAL.
// ═════════════════════════════════════════════════════════════════════════════
// The texture-import wizard (kiwi_import.cpp) writes materials/<name> + images/<name>.iwi
// and then has to make the browser show it WITHOUT a restart.  That is exactly the
// Load_Materials per-file body above (:436-471) for a single name — header read, the
// non-zero gate, the toolFlags&0x1000 "_editor" override, Material_ConvertToEditorMaterial —
// so this reuses those four steps rather than duplicating them, which is also why it lives
// here: Material_ReadEditorVariant / Material_ConvertToEditorMaterial / Editor_DoesMaterial-
// Exist / Tex_stricmp and texwnd_s itself are all TU-local (the accessor pattern at :94).
//
// The one thing Load_Materials does NOT have to do is refresh an EXISTING entry.  An
// overwrite import re-uses the same name, so Material_ConvertToEditorMaterial (:400) would
// hand back the old qtexture with the old width/height/usage/locale still on it.  The
// refresh below re-stamps exactly the eleven fields :405-415 sets, in the same order, and
// drops the cached render handle so the next TexWnd_DrawMaterials re-registers the material
// through Texture_GetHandle.
qtexture_s *TexWnd_RegisterMaterialByName( const char *name )
{
    if ( !name || !name[0] )
        return nullptr;

    MaterialInfoRaw raw;
    int             h = 0;
    com_fileAccessed = 1;
    FS_FOpenFileRead( va( "materials/%s", name ), &h );
    if ( !h )
        return nullptr;
    uint got = FS_Read( (uint8_t *)&raw, sizeof( raw ), h );
    FS_FCloseFile( h );
    if ( got != sizeof( raw ) )
        return nullptr;

    // The same gate Load_Materials applies at :455.
    if ( !raw.usage || !raw.locale || !raw.autoTexScaleWidth || !raw.autoTexScaleHeight )
        return nullptr;

    if ( raw.toolFlags & 0x1000 )                                   // :459-468
    {
        char editorName[80];
        strncpy( editorName, name, 64 );
        editorName[64] = '\0';
        I_strncat( editorName, sizeof( editorName ), "_editor" );
        MaterialInfoRaw editorRaw;
        if ( Material_ReadEditorVariant( editorName, &editorRaw ) )
            raw = editorRaw;
    }

    qtexture_s *existing = Editor_DoesMaterialExist( name );
    qtexture_s *q        = Material_ConvertToEditorMaterial( &raw, name );
    if ( !q )
        return nullptr;

    if ( existing )
    {
        // Re-stamp, field for field, what :405-415 stamps on a fresh entry.
        q->next                        = nullptr;                   // drop the stale handle
        q->is_in_use                   = true;
        q->unk1                        = (char)raw.gameFlags;
        q->usage_index                 = (char)raw.usage;
        q->unk_flags2                  = (uint16_t)raw.toolFlags;
        q->tex_num_or_localefilter     = (int)raw.locale;
        q->width                       = raw.autoTexScaleWidth;
        q->height                      = raw.autoTexScaleHeight;
        q->color_or_surfacetype_filter = raw.surfaceFlags;
        q->in_use                      = raw.contents;
    }
    else
    {
        // Keep sorted_materials[] ordered the way Load_Materials leaves it, so the browser's
        // flow layout does not put the new tile at a random place in the grid.
        qsort( texWndGlob_textureOffset.sorted_materials,
               texWndGlob_textureOffset.materialCount, sizeof( qtexture_s * ), Tex_stricmp );
    }

    g_nUpdateBits |= W_TEXTURE;
    return q;
}

// (TexWnd_MakeMaterialCurrentByName, this round's second accessor, lives just below
//  TexWnd_BuildClickedMaterialDef — it needs that builder and the Texture_SetTexture
//  extern, both of which are declared further down this file.)

// The Usage/Locale/Surface-type filter name→index tables TexWnd_FilterAccept's chain reads.
// filter_usage_array/filter_locale_array (engine_stubs.cpp, loaded by FillTextureMenu);
// filter_surfacetype_array (defined below in this TU).  RadiantFilterEntry = the binary's
// filter_material_t {char* name; int index} (8B); the menu/shutdown code below reuses it.
// Declared here (ahead of TexWnd_FilterAccept, which reads these arrays).
struct RadiantFilterEntry { char *name; int index; };
static_assert(sizeof(RadiantFilterEntry) == (sizeof(void *) == 8 ? 16 : 8), "filter_material_t must be 8 bytes (IDB)");
extern RadiantFilterEntry filter_usage_array[256];       // 0x739F80
extern RadiantFilterEntry filter_locale_array[256];      // 0x73A780
extern RadiantFilterEntry filter_surfacetype_array[29];  // 0x73AF80 (this TU, below)


// ═════════════════════════════════════════════════════════════════════════════
// CTexWnd renders registered materials as a wrapping, scrollable thumbnail grid.
// Editor_AddRadiantMaterial and Material_ConvertToEditorMaterial populate the metadata
// consumed by its usage, locale, surface-type, and layer filters.
// ═════════════════════════════════════════════════════════════════════════════

#include "mainfrm.h"                  // CTexWnd
#include <gfx_d3d/r_init.h>           // dx, R_SetupRendertarget_CheckDevice, R_Hwnd_Resize, R_CheckTargetWindow
#include <gfx_d3d/r_rendercmds.h>     // R_BeginFrame/EndFrame, clear, ProjectionSet2D, Draw2DImage, DrawText
#include "radiant_rtt.h"             // P5 RTT: RTT_Begin/RTT_End, RTT_TEXTURE

extern void  Brush_SetTexture( MaterialDef *material, char planarProject );   // select.cpp (0x48f170)
extern char  Texture_SetTexture( const int *a1, MaterialDef *a2 ); // texwnd.cpp 0x45be50
extern int   MaterialDef_04( MaterialDef *mtlDef );              // materialdef.cpp 0x431740
extern int   MaterialDef_13( int visIndex, MaterialDef *mtlDef );// materialdef.cpp 0x431ba0
// Texture_SetTexture deps (the back half of the function — declared here, defined further down):
extern char  byte_73C380;                                        // findtexture.cpp (0x73C380) — 1=fill Find, 0=fill Replace on pick
extern LRESULT LayeredMaterialWnd_RadMtl( qtexture_s *radMtl );   // layeredmaterialwnd.cpp (0x4185C0) — add radMtl as a live layer

// Layered-material sub-view (defined below; the R_DrawTexWnd 0x45d0f0 dispatch in OnPaint
// selects it via this mode flag).  See the LAYERED-MATERIAL SUB-VIEW block near EOF.
extern int  g_texwnd_simple_layered_selection;                    // (defined below)
static int  TexWnd_DrawLayeredMaterials();

// ─── texwndState_t — the texture browser's shell state (U-VP-TEX) ─────────────────
// The texture window is a SINGLETON: radiant_main.cpp's Radiant_CreateRenderWindows creates
// exactly one (TexWnd_CreateRaw).  So this file-scope instance IS the texture browser: the
// raw-Win32 WndProc at the bottom of this file reads/writes these fields, and Ed_TexWnd()
// hands the same block to callers.
//
// Every field keeps its old CTexWnd member NAME.
//   m_hWnd   = the view's window handle, standing in for CWnd::m_hWnd / GetSafeHwnd() (the
//              scrollbar calls and Invalidate/UpdateWindow need it).  Latched
//              in TexWnd_OnCreate.  It is also the "is there a live window at all?" test.
//              CONTRACT: every repaint/scroll body below (TexWnd_CheckScroll /
//              TexWnd_UpdateScrollRange / TexWnd_ApplyMaterialAtIndex)
//              assumes a live m_hWnd, exactly as the CWnd members it replaces assumed a live
//              CWnd::m_hWnd — the two headless entry points (Texture_SetTexture's scroll tail
//              and Texture_ResetPosition) gate on it, and the handlers only run for a real
//              window.  Do NOT call them with m_hWnd == NULL: ::InvalidateRect(NULL, …) would
//              invalidate every window in the thread instead of asserting like MFC did.
// The per-window globals in texWndGlob_textureOffset (m_nWidth/m_nHeight/m_ptDown/
// lastButtonDown/m_was_mouse_dragged/nPos) stay exactly where they are — they are texwnd_s
// (0x25d7990) fields, and the handlers still feed them.
struct texwndState_t
{
    HWND m_hWnd     = nullptr;    // the browser HWND
    int  m_nWidth   = 0;          // pane client width  (px), updated in TexWnd_OnSize
    int  m_nHeight  = 0;          // pane client height (px)
    int  m_scrollY  = 0;          // vertical scroll offset (px) == the binary's nPos_current
    int  m_selIndex = -1;         // selected material (index into sorted_materials)
    int  m_contentH = 0;          // last laid-out content height (for the scroll clamp)
} g_texwndState;

// Ed_TexWnd — the shell-agnostic "texture browser" accessor (U-GLOBALS).  The binary's
// concept (g_pParentWnd->m_pTexWnd) collapses to the single browser window in this port, so
// this always returns the one state block.  Callers declare it themselves (`extern
// texwndState_t *Ed_TexWnd();`).
texwndState_t *Ed_TexWnd()
{
    return &g_texwndState;
}

// Handler/method free fns defined further down but used earlier in the file.
int  TexWnd_CheckScroll( int n );
void TexWnd_UpdateScrollRange();
void TexWnd_DrawMaterials();

static void TexWnd_BuildClickedMaterialDef( qtexture_s *q, MaterialDef *out )
{
    memset( out, 0, sizeof( *out ) );
    out->radMtl = q;

    qtexture_s *layered = MaterialDef_GetLayeredMaterial( out );
    const int width = layered ? layered->width : 512;
    const int height = layered ? layered->height : 512;
    const int layerCount = MaterialDef_04( out );
    const float sample = g_qeglobals.random_texture_stuff[g_qeglobals.current_edit_layer].sampleSize;

    for ( int i = 0; i < layerCount; ++i )
    {
        texdef_sub_t *td = &out->mat_texDef + i;
        td->size[0] = (float)width * sample;
        td->size[1] = (float)height * sample;
        td->shift[0] = 0.0f;
        td->shift[1] = 0.0f;
        td->rotate = 0.0f;
        td->unk3 = 0;
        *(int *)&td->sample_size = MaterialDef_13( i, out );
    }
}

// KIWI-UX (ROUND BE): make a registered material the CURRENT one, by name — the second of
// this round's two additive accessors (the first, TexWnd_RegisterMaterialByName, sits with
// the Load_Materials block it reuses).  The recipe is the click path's, verbatim:
// TexWnd_BuildClickedMaterialDef above builds the MaterialDef a thumbnail click would build,
// and Texture_SetTexture (defined at the bottom of this file, IDB 0x45be50) stamps it into
// random_texture_stuff and applies it.  Note the DOCUMENTED SIDE EFFECT — Texture_SetTexture's
// tail calls Brush_SetTexture(a2,1), i.e. it RETEXTURES THE SELECTION, exactly as clicking
// the thumbnail would.  The import wizard therefore puts this behind an opt-in checkbox
// rather than doing it on every import.
bool TexWnd_MakeMaterialCurrentByName( const char *name )
{
    if ( !name || !name[0] )
        return false;
    qtexture_s *q = Editor_DoesMaterialExist( name );
    if ( !q )
        return false;
    MaterialDef mat;
    TexWnd_BuildClickedMaterialDef( q, &mat );
    Texture_SetTexture( nullptr, &mat );
    return true;
}

// ── grid layout constants (pane pixel space) ─────────────────────────────────
static const int TEX_MARGIN = 8;     // = the binary's left/top start
// The per-thumbnail label band = font->pixelHeight (TexWnd_SetupIter) and the name label
// is drawn at scale 1.0 in colors[8] (TexWnd_DrawMaterials), as in the binary; a compact
// fixed band + 0.18 text scale renders the labels far too small to read.
static void R_DrawOutlineRect( int xL, int yT, int xR, int yB, const float *rgba );  // fwd (def below)

// ── MaterialIter_t (44 bytes) — the flow-layout iterator. ──────────────────────────
struct MaterialIter_t
{
    qtexture_s *radMtl;            // 0x00  current material
    int         sortedIndex;       // 0x04  index into sorted_materials (init -1)
    int         thumbX;            // 0x08  laid-out x of this thumbnail
    int         rowY;              // 0x0c  laid-out y of this thumbnail (UNSCROLLED)
    int         cursorX;           // 0x10  running x within the current row
    int         cursorY;           // 0x14  running y of the current row top
    int         thumbW;            // 0x18  this thumbnail's width  (px)
    int         thumbH;            // 0x1c  this thumbnail's height (px)
    int         rowMaxH;           // 0x20  tallest thumbnail in the current row
    int         labelBand;         // 0x24  text band height
    float       texwndscale;       // 0x28  m_nTextureWindowScale / 100
};

// Ed_RoundToInt (0x40A7D0) — fistp rounding.  KIWI: the old truncating cast made
// thumbnails up to a pixel small.
static inline int Ed_RoundToInt( float x ) { return (int)KiwiRound_Fistp( x ); }

// TexWnd_SetupIter (0x45ba00).
static int TexWnd_SetupIter( MaterialIter_t *it, Font_s *font )
{
    it->cursorX      = TEX_MARGIN;
    it->cursorY      = TEX_MARGIN;
    it->sortedIndex  = -1;
    it->rowY         = 0;
    it->rowMaxH      = 0;
    it->texwndscale  = (float)( (double)g_PrefsDlg->m_nTextureWindowScale / 100.0 );
    it->labelBand    = font->pixelHeight;
    return it->labelBand;
}

// TexWnd_FilterAccept — the per-material accept predicate (0x45ba70's filter chain).
// Dispatches on g_qeglobals.current_edit_layer:
//   layer 1 (Lightmap render-method): accept iff the name starts with "lightmap_"
//   layer 2 (Smoothing render-method): accept iff the name starts with "smoothing_"
//   layer 0 (Material): the usage/locale/surfaceType filter chain, then the search-bar,
//            else the "is_in_use" base predicate.
// The layer-1/2 sub-views + the search-bar are NAME-based (texture names are lowercased at
// registration — Texture_GetHandle's _strlwr — so case-insensitive _strnicmp == the
// binary's raw byte compare).
//
// The lightmap-prefix and smoothing-prefix tests run for EVERY material, and layer 0 FIRST
// rejects lightmap_*/smoothing_* names, BEFORE the usage chain.
//
// LAYER 0 CHAIN:
//   reject if  name has "lightmap_"  prefix
//   reject if  name has "smoothing_" prefix
//   reject if  tex->usage_index == 0
//   reject if  usageFilter!=0       && usage_index != filter_usage_array[usageFilter].index
//   reject if  localeFilter!=0      && (1<<filter_locale_array[localeFilter].index & tex->tex_num_or_localefilter)==0
//   reject if surfaceTypeFilter != 0 and the selected encoded surface type does not match
//   (tex->color_or_surfacetype_filter & SURF_TYPE_MASK)
//   then searchbar (prefix) if active; else accept iff tex->is_in_use
// The usage_index/locale/surfaceFlags metadata is populated for BOTH registration paths
// (Material_ConvertToEditorMaterial reads the raw header; Editor_AddRadiantMaterial gets it from
// Register_WorldMaterial: the raw header, else the editor-widened Material's
// editorUsage/editorLocale/surfaceFlags), so the chain filters real data.  With no filter
// selected (all indices 0 — the "all" default), the chain reduces to
// "reject lightmap_/smoothing_/usage_index==0, else accept iff is_in_use" — every in-use
// material with a real usage (i.e. every browsable world material) is accepted, so the default
// view is NOT emptied.
static bool TexWnd_FilterAcceptBase( const qtexture_s *tex )   // KIWI: the colour filter wraps it below
{
    if ( !tex || !tex->name )
        return false;
    if ( _strnicmp( tex->name, "kiwi_refimg_", 12 ) == 0 ) return false; // KIWI (REFIMG): generated editor assets never enter the browser.
    // KIWI (2026-10-24, user: "i want the kglass removed from the textures window. They're
    // useless and will never be chosen"): the glass system's own materials stay loadable.
    if ( _strnicmp( tex->name, "kglass_", 7 ) == 0 ) return false;

    const int layer = g_qeglobals.current_edit_layer;
    if ( layer == 1 )                                          // Lightmap render-method
        return _strnicmp( tex->name, "lightmap_", 9 ) == 0;
    if ( layer == 2 )                                          // Smoothing render-method
        return _strnicmp( tex->name, "smoothing_", 10 ) == 0;

    // layer 0 (Material) — the lightmap/smoothing prefix exclusion is computed for every
    // material and rejected first, then the usage/locale/surfaceType chain.
    if ( _strnicmp( tex->name, "lightmap_", 9 ) == 0 )
        return false;
    if ( _strnicmp( tex->name, "smoothing_", 10 ) == 0 )
        return false;

    const byte usage_index = (byte)tex->usage_index;
    if ( !usage_index )
        return false;

    const byte usageFilter = texWndGlob_textureOffset.usageFilter;
    if ( usageFilter && usage_index != (byte)filter_usage_array[usageFilter].index )
        return false;

    const byte localeFilter = texWndGlob_textureOffset.localeFilter;
    if ( localeFilter &&
         ( ( 1 << filter_locale_array[localeFilter].index ) & tex->tex_num_or_localefilter ) == 0 )
        return false;

    const byte surfaceTypeFilter = texWndGlob_textureOffset.surfaceTypeFilter;
    if ( surfaceTypeFilter &&
         filter_surfacetype_array[surfaceTypeFilter].index != ( tex->color_or_surfacetype_filter & SURF_TYPE_MASK ) )
        return false;

    // Search-bar (data-independent): when active, accept iff the material name matches the
    // search buffer as a case-insensitive PREFIX (_strnicmp(buffer, name, strlen(buffer))).
    if ( texWndGlob_textureOffset.searchbar_filter && texWndGlob_textureOffset.searchbar_buffer )
    {
        const char *q = texWndGlob_textureOffset.searchbar_buffer;
        if ( _strnicmp( q, tex->name, strlen( q ) ) == 0 )     // the binary's prefix match
            return true;
        // KIWI-UX (ROUND AE): SUBSTRING superset of the binary's prefix match.  The
        // prefix arm above is the faithful one and still runs first; this arm exists
        // because nearly every shipped material name starts with a family prefix
        // ("ch_", "ac_", "me_"), so a user typing "brick" means ch_brick_* and the
        // prefix rule alone would show nothing.  Case handling is free: names are
        // lowercased at registration (Texture_GetHandle's _strlwr) and
        // TexWnd_SetSearchFilter lowercases the query the same way.
        return strstr( tex->name, q ) != nullptr;
    }

    // base predicate: show the textures the map actually references.
    return tex->is_in_use;
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI (2026-10-22): COLOUR FILTER
// ═════════════════════════════════════════════════════════════════════════════
// User: "add color sort buttons on the textures window, so if I want a "green" texture, I
// click a green button and it shows only green.  Make it a radio style set of buttons."
//
// A material's colour is the mean of its colour map, read from the small mips at the FRONT
// of the .iwi (mips are stored smallest first), so the header and a few hundred bytes are
// all that is read.  The mean is classified into one bucket.  Classification is lazy: the
// Textures toolbar pumps it under a time budget while a colour is picked, and a material
// not classified yet is hidden until it is.  Results are cached by material name.
static const kiwiTexColor_t s_texColors[KTEXCOL_COUNT] = {
    { "All",    0.00f, 0.00f, 0.00f },
    { "Red",    0.85f, 0.18f, 0.16f },
    { "Orange", 0.95f, 0.55f, 0.12f },
    { "Yellow", 0.95f, 0.85f, 0.20f },
    { "Green",  0.30f, 0.70f, 0.25f },
    { "Cyan",   0.25f, 0.75f, 0.80f },
    { "Blue",   0.22f, 0.40f, 0.85f },
    { "Purple", 0.55f, 0.30f, 0.80f },
    { "Pink",   0.92f, 0.50f, 0.70f },
    { "Brown",  0.50f, 0.34f, 0.20f },
    { "White",  0.93f, 0.93f, 0.93f },
    { "Grey",   0.50f, 0.50f, 0.50f },
    { "Black",  0.08f, 0.08f, 0.08f },
};

static int s_texColorFilter = KTEXCOL_ALL;
static std::unordered_map<std::string, signed char> s_texColorOf;   // 0 = no readable colour map

const kiwiTexColor_t *TexWnd_ColorInfo( int bucket )
{
    return ( bucket >= 0 && bucket < KTEXCOL_COUNT ) ? &s_texColors[bucket] : nullptr;
}

int TexWnd_GetColorFilter() { return s_texColorFilter; }

void TexWnd_SetColorFilter( int bucket )
{
    if ( bucket < 0 || bucket >= KTEXCOL_COUNT || bucket == s_texColorFilter )
        return;
    s_texColorFilter = bucket;
    TexWnd_CheckScroll( 0 );
}

// Mean RGB (0..1) -> bucket.  Hue bands in degrees; dull oranges/yellows are Brown.
static int TexWnd_ClassifyMean( float r, float g, float b )
{
    const float mx = r > g ? ( r > b ? r : b ) : ( g > b ? g : b );
    const float mn = r < g ? ( r < b ? r : b ) : ( g < b ? g : b );
    const float c  = mx - mn;
    if ( mx < 0.12f )
        return KTEXCOL_BLACK;
    if ( c < 0.05f || c / mx < 0.12f )
        return mx > 0.78f ? KTEXCOL_WHITE : ( mx < 0.20f ? KTEXCOL_BLACK : KTEXCOL_GREY );
    float h;
    if ( mx == r )      h = 60.0f * fmodf( ( g - b ) / c + 6.0f, 6.0f );
    else if ( mx == g ) h = 60.0f * ( ( b - r ) / c + 2.0f );
    else                h = 60.0f * ( ( r - g ) / c + 4.0f );
    const float s = c / mx;
    if ( h >= 12.0f && h < 55.0f && ( mx < 0.65f || s < 0.45f ) )
        return KTEXCOL_BROWN;
    if ( h < 12.0f || h >= 340.0f ) return KTEXCOL_RED;
    if ( h < 42.0f )                return KTEXCOL_ORANGE;
    if ( h < 70.0f )                return KTEXCOL_YELLOW;
    if ( h < 165.0f )               return KTEXCOL_GREEN;
    if ( h < 200.0f )               return KTEXCOL_CYAN;
    if ( h < 255.0f )               return KTEXCOL_BLUE;
    if ( h < 290.0f )               return KTEXCOL_PURPLE;
    return KTEXCOL_PINK;
}

static void TexWnd_Rgb565( unsigned v, float *out )
{
    out[0] = (float)( ( v >> 11 ) & 31 ) / 31.0f;
    out[1] = (float)( ( v >> 5 ) & 63 ) / 63.0f;
    out[2] = (float)( v & 31 ) / 31.0f;
}

// Mean colour of one DXT colour block (the 8 colour bytes): its 16 palette picks.
static void TexWnd_DxtBlockSum( const unsigned char *p, bool dxt1, float *sum )
{
    const unsigned c0 = p[0] | ( p[1] << 8 ), c1 = p[2] | ( p[3] << 8 );
    float pal[4][3];
    TexWnd_Rgb565( c0, pal[0] );
    TexWnd_Rgb565( c1, pal[1] );
    const bool four = !dxt1 || c0 > c1;
    for ( int k = 0; k < 3; ++k )
    {
        pal[2][k] = four ? ( 2.0f * pal[0][k] + pal[1][k] ) / 3.0f : ( pal[0][k] + pal[1][k] ) * 0.5f;
        pal[3][k] = four ? ( pal[0][k] + 2.0f * pal[1][k] ) / 3.0f : 0.0f;
    }
    const unsigned idx = p[4] | ( p[5] << 8 ) | ( p[6] << 16 ) | ( (unsigned)p[7] << 24 );
    for ( int i = 0; i < 16; ++i )
        for ( int k = 0; k < 3; ++k )
            sum[k] += pal[( idx >> ( 2 * i ) ) & 3][k];
}

// Bucket of one material, 0 when its colour map cannot be read.
static int TexWnd_ClassifyMaterial( const char *name )
{
    kiwiMatSource_t src;
    char err[128];
    if ( !KiwiMat_ReadSource( name, &src, err, sizeof( err ) ) || src.colorMapImage[0] == '$' )
        return 0;

    char qpath[128];
    _snprintf( qpath, sizeof( qpath ), "images/%s.iwi", src.colorMapImage );
    qpath[sizeof( qpath ) - 1] = '\0';
    int fh = 0;
    com_fileAccessed = 1;
    const int fileSize = (int)FS_FOpenFileRead( qpath, &fh );
    if ( !fh )
        return 0;
    unsigned char hdr[28];
    if ( fileSize < 28 || FS_Read( hdr, 28, fh ) != 28 || hdr[0] != 'I' || hdr[1] != 'W' || hdr[2] != 'i'
      || ( hdr[5] & 0x0C ) )                                    // cube / volume
    {
        FS_FCloseFile( fh );
        return 0;
    }
    const int fmt = hdr[4];
    const int w   = hdr[6] | ( hdr[7] << 8 );
    const int h   = hdr[8] | ( hdr[9] << 8 );
    int bpb = 0, bpp = 0;                                       // bytes per DXT block / pixel
    switch ( fmt )
    {
    case 0x0B: bpb = 8;  break;                                 // DXT1
    case 0x0C: case 0x0D: bpb = 16; break;                      // DXT3 / DXT5
    case 0x01: bpp = 4;  break;                                 // ARGB8 (B,G,R,A in memory)
    case 0x02: bpp = 3;  break;                                 // RGB8  (B,G,R)
    case 0x05: bpp = 1;  break;                                 // L8
    default:
        FS_FCloseFile( fh );
        return 0;
    }
    if ( w < 1 || h < 1 )
    {
        FS_FCloseFile( fh );
        return 0;
    }
    int levels = 1;
    if ( !( hdr[5] & 0x02 ) )                                   // has mips: down to 1x1
        for ( int m = w > h ? w : h; m > 1; m >>= 1 )
            ++levels;
    auto levelSize = [&]( int l, int *lw, int *lh ) {
        *lw = ( w >> l ) > 0 ? ( w >> l ) : 1;
        *lh = ( h >> l ) > 0 ? ( h >> l ) : 1;
        return bpb ? bpb * ( ( *lw + 3 ) >> 2 ) * ( ( *lh + 3 ) >> 2 ) : bpp * *lw * *lh;
    };
    // The deepest level at least 8 px on its short side (or level 0 for a tiny image).
    int target = 0;
    for ( int l = levels - 1; l >= 0; --l )
    {
        int lw, lh;
        levelSize( l, &lw, &lh );
        if ( lw >= 8 && lh >= 8 )
        {
            target = l;
            break;
        }
    }
    int skip = 0, lw = 0, lh = 0;
    for ( int l = levels - 1; l > target; --l )
        skip += levelSize( l, &lw, &lh );
    const int need = levelSize( target, &lw, &lh );
    if ( 28 + skip + need > fileSize || need > ( 1 << 20 ) )
    {
        FS_FCloseFile( fh );
        return 0;
    }
    std::vector<unsigned char> data( (size_t)( skip + need ) );
    const unsigned got = FS_Read( data.data(), (unsigned)data.size(), fh );
    FS_FCloseFile( fh );
    if ( got != (unsigned)data.size() )
        return 0;

    const unsigned char *px = data.data() + skip;
    float sum[3] = { 0.0f, 0.0f, 0.0f };
    float n = 0.0f;
    if ( bpb )
    {
        const int blocks = need / bpb;
        for ( int i = 0; i < blocks; ++i )
            TexWnd_DxtBlockSum( px + i * bpb + ( bpb - 8 ), fmt == 0x0B, sum );
        n = 16.0f * (float)blocks;
    }
    else
    {
        for ( int i = 0; i < lw * lh; ++i, px += bpp )
        {
            if ( bpp == 1 )
            {
                const float v = (float)px[0] / 255.0f;
                sum[0] += v; sum[1] += v; sum[2] += v;
            }
            else
            {
                sum[0] += (float)px[2] / 255.0f;
                sum[1] += (float)px[1] / 255.0f;
                sum[2] += (float)px[0] / 255.0f;
            }
        }
        n = (float)( lw * lh );
    }
    if ( !( n > 0.0f ) )
        return 0;
    return TexWnd_ClassifyMean( sum[0] / n, sum[1] / n, sum[2] / n );
}

// Classify the not-yet-seen materials the other filters accept, for up to budgetMs.
// Reports how many of those are classified; true when the pass is complete.
bool TexWnd_PumpColorFilter( double budgetMs, int *outDone, int *outTotal )
{
    LARGE_INTEGER f, t0, t;
    ::QueryPerformanceFrequency( &f );
    ::QueryPerformanceCounter( &t0 );
    bool out = false;
    int  done = 0, total = 0;
    const int count = texWndGlob_textureOffset.materialCount;
    for ( int i = 0; i < count; ++i )
    {
        const qtexture_s *tex = texWndGlob_textureOffset.sorted_materials[i];
        if ( !TexWnd_FilterAcceptBase( tex ) )
            continue;
        ++total;
        if ( s_texColorOf.count( tex->name ) )
        {
            ++done;
            continue;
        }
        if ( out )
            continue;
        ::QueryPerformanceCounter( &t );
        if ( (double)( t.QuadPart - t0.QuadPart ) * 1000.0 / (double)f.QuadPart > budgetMs )
        {
            out = true;
            continue;
        }
        s_texColorOf[tex->name] = (signed char)TexWnd_ClassifyMaterial( tex->name );
        ++done;
    }
    if ( outDone )  *outDone  = done;
    if ( outTotal ) *outTotal = total;
    return done == total;
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI (2026-10-22): "IN USE & VIEW" FILTER
// ═════════════════════════════════════════════════════════════════════════════
// User: "add another fliter for textures that is (In Use & View) that shows textures that are
// currently visible in the 3d view."  Visible = a brush face or patch that is not filtered,
// faces the camera, lies (as a bounding sphere) inside the camera frustum and is not wholly
// above a section cut.  Nothing occludes: a face behind a wall still counts.  The set is
// refreshed a few times a second while the filter is on.
static bool s_viewFilter = false;
static std::unordered_set<const qtexture_s *> s_inView, s_inViewNext;
static DWORD s_inViewTick = 0;
static const unsigned KTEXVIEW_REFRESH_MS = 200;

bool TexWnd_GetViewFilter() { return s_viewFilter; }

void TexWnd_SetViewFilter( bool on )
{
    if ( s_viewFilter == on )
        return;
    s_viewFilter = on;
    s_inView.clear();
    s_inViewTick = 0;
    TexWnd_CheckScroll( 0 );
}

static int TexWnd_NoteInView( qtexture_s *q )
{
    if ( q )
        s_inViewNext.insert( q );
    return 0;
}

struct texViewCam_t
{
    bool  ortho;
    float eye[3], fwd[3], right[3], up[3];
    float tanX, tanY, halfW, halfH;
};

static bool TexWnd_SphereInView( const texViewCam_t &v, const float *c, float r )
{
    const float d[3] = { c[0] - v.eye[0], c[1] - v.eye[1], c[2] - v.eye[2] };
    const float x = fabsf( d[0] * v.right[0] + d[1] * v.right[1] + d[2] * v.right[2] );
    const float y = fabsf( d[0] * v.up[0] + d[1] * v.up[1] + d[2] * v.up[2] );
    if ( v.ortho )
        return x - r <= v.halfW && y - r <= v.halfH;
    const float z = d[0] * v.fwd[0] + d[1] * v.fwd[1] + d[2] * v.fwd[2];
    if ( z < -r )
        return false;
    return x - r * sqrtf( 1.0f + v.tanX * v.tanX ) <= z * v.tanX
        && y - r * sqrtf( 1.0f + v.tanY * v.tanY ) <= z * v.tanY;
}

// Faces the camera, inside the frustum, and (with a live section cut) not wholly above it.
static bool TexWnd_PolyInView( const texViewCam_t &v, const float (*pts)[3], int n, int stride,
                               const float *normal, float dist )
{
    if ( n < 1 )
        return false;
    if ( normal )
    {
        const float facing = v.ortho
            ? -( normal[0] * v.fwd[0] + normal[1] * v.fwd[1] + normal[2] * v.fwd[2] )
            : normal[0] * v.eye[0] + normal[1] * v.eye[1] + normal[2] * v.eye[2] - dist;
        if ( facing <= 0.0f )
            return false;
    }
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for ( int i = 0; i < n; ++i )
    {
        const float *p = (const float *)( (const char *)pts + (size_t)i * stride );
        for ( int k = 0; k < 3; ++k )
        {
            lo[k] = p[k] < lo[k] ? p[k] : lo[k];
            hi[k] = p[k] > hi[k] ? p[k] : hi[k];
        }
    }
    if ( KiwiSection_Cutting() && lo[2] > KiwiSection_Level() )
        return false;
    const float c[3] = { ( lo[0] + hi[0] ) * 0.5f, ( lo[1] + hi[1] ) * 0.5f, ( lo[2] + hi[2] ) * 0.5f };
    const float e[3] = { hi[0] - c[0], hi[1] - c[1], hi[2] - c[2] };
    return TexWnd_SphereInView( v, c, sqrtf( e[0] * e[0] + e[1] * e[1] + e[2] * e[2] ) );
}

static void TexWnd_CollectInView( const texViewCam_t &v, selbrush_t *list )
{
    for ( selbrush_t *b = list->next; b && b != list; b = b->next )
    {
        brush_t *def = b->def;
        if ( !def || FilterBrush( b, 0 ) )
            continue;
        if ( def->patch )
        {
            const curvePatchDef_t *g = def->patch->curveDef;
            if ( g && g->verts && g->width > 0 && g->height > 0
              && TexWnd_PolyInView( v, (const float (*)[3])&g->verts[0].xyz, g->width * g->height,
                                    (int)sizeof( curveVert_t ), nullptr, 0.0f ) )
                MaterialDef_02( (MaterialDef *)&def->patch->texture, TexWnd_NoteInView );
            continue;
        }
        if ( !def->faces )
            continue;
        for ( int i = 0; i < def->faceCount; ++i )
        {
            const face_t &f = def->faces[i];
            if ( !f.w || f.w->numpoints < 3 )
                continue;
            if ( TexWnd_PolyInView( v, (const float (*)[3])f.w->p[0], f.w->numpoints,
                                    (int)sizeof( f.w->p[0] ), f.plane.normal, f.plane.dist ) )
                MaterialDef_02( (MaterialDef *)&f.mtldef[0], TexWnd_NoteInView );
        }
    }
}

// Refreshes the visible set on its interval; re-clamps the scroll when it changes.
void TexWnd_PumpViewFilter()
{
    if ( !s_viewFilter )
        return;
    const DWORD now = ::GetTickCount();
    if ( s_inViewTick && now - s_inViewTick < KTEXVIEW_REFRESH_MS )
        return;
    s_inViewTick = now ? now : 1;

    const camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;
    texViewCam_t v;
    v.ortho = KiwiCam_Ortho();
    for ( int k = 0; k < 3; ++k )
    {
        v.eye[k]   = c->origin[k];
        v.fwd[k]   = c->vpn[k];
        v.right[k] = c->vright[k];
        v.up[k]    = c->vup[k];
    }
    // CamWnd_SetupScene's frustum: tanY = tan(fov/2) * 0.75, tanX by aspect.
    v.tanY  = tanf( (float)DEG2RAD( g_PrefsDlg->camera_fov ) * 0.5f ) * 0.75f;
    v.tanX  = v.tanY * (float)c->width / (float)c->height;
    v.halfH = KiwiCam_OrthoHalfHeight();
    v.halfW = v.halfH * (float)c->width / (float)c->height;

    s_inViewNext.clear();
    TexWnd_CollectInView( v, &active_brushes );
    TexWnd_CollectInView( v, &selected_brushes );
    if ( s_inViewNext != s_inView )
    {
        s_inView.swap( s_inViewNext );
        TexWnd_CheckScroll( Ed_TexWnd()->m_scrollY );
        g_nUpdateBits |= W_TEXTURE;
    }
}

static bool TexWnd_FilterAccept( const qtexture_s *tex )
{
    if ( !TexWnd_FilterAcceptBase( tex ) )
        return false;
    if ( s_viewFilter && !s_inView.count( tex ) )
        return false;
    if ( s_texColorFilter == KTEXCOL_ALL )
        return true;
    const auto it = s_texColorOf.find( tex->name );
    return it != s_texColorOf.end() && it->second == s_texColorFilter;
}

// TexWnd_IterateMaterials (0x45ba70) — advance to the next accepted material and
// compute its flow-layout slot. Returns false when the list is exhausted. The accept
// predicate is TexWnd_FilterAccept (above); the layout math below is the binary's.
static bool TexWnd_IterateMaterials( MaterialIter_t *it )
{
    ++it->sortedIndex;
    int count = texWndGlob_textureOffset.materialCount;
    qtexture_s *tex = nullptr;
    for ( ; it->sortedIndex < count; ++it->sortedIndex )
    {
        tex = texWndGlob_textureOffset.sorted_materials[it->sortedIndex];
        it->radMtl = tex;
        if ( TexWnd_FilterAccept( tex ) )
            break;
    }
    if ( it->sortedIndex >= count )
        return false;

    // ── flow layout ──
    double w = (double)tex->width  * it->texwndscale;
    double h = (double)tex->height * it->texwndscale;
    while ( w < 8.0 || h < 8.0 )   { w *= 2.0; h *= 2.0; }   // smaller side ≥ 8
    while ( w < 32.0 && h < 32.0 ) { w *= 2.0; h *= 2.0; }   // at least one side ≥ 32
    int thumbW = Ed_RoundToInt( (float)w );
    int thumbH = Ed_RoundToInt( (float)h );
    it->thumbW = thumbW;
    it->thumbH = thumbH;

    // wrap to the next row when this thumbnail would overflow the pane width
    if ( thumbW + it->cursorX > texWndGlob_textureOffset.m_nWidth - 8 )
    {
        if ( it->rowMaxH > 0 )
        {
            it->cursorY += it->labelBand + it->rowMaxH + 4;
            it->cursorX  = TEX_MARGIN;
            it->rowMaxH  = 0;
        }
    }
    it->thumbX = it->cursorX;
    it->rowY   = it->cursorY;
    if ( it->rowMaxH < thumbH ) it->rowMaxH = thumbH;
    it->cursorX += ( thumbW < 64 ? 64 : thumbW ) + 8;
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Shell-agnostic texture-browser handlers + methods (U-VP-TEX).  Every afx_msg body and
//  every CTexWnd method lives here as a free function on plain args; the raw-Win32 WndProc
//  at the bottom of this file feeds them with the SAME conventions MFC used (client coords, the
//  MK_* wParam flag word, LOWORD/HIWORD size, the ON_WM_VSCROLL wParam decomposition).  The
//  state writes and the scrollbar/invalidate calls are part of the handler bodies.
//  `Ed_TexWnd()` is the one texture browser (see texwndState_t).
//
//  Callers declare these themselves:
//      extern void TexWnd_OnCreate( HWND hwnd );
//      extern void TexWnd_OnSize( HWND hwnd, int cx, int cy );
//      extern void TexWnd_Paint( HWND hwnd );
//      extern void TexWnd_OnLButtonDown( int x, int y );
//      extern void TexWnd_OnRButtonDown( unsigned int nFlags, int x, int y );
//      extern void TexWnd_OnRButtonUp( unsigned int nFlags, int x, int y );
//      extern int  TexWnd_OnMouseWheel( short zDelta );
//      extern void TexWnd_OnVScroll( unsigned int nSBCode, unsigned int nPos );
//      extern void TexWnd_DrawMaterials();
//      extern int  TexWnd_HitTest( int px, int py );
//      extern void TexWnd_ShowMaterialStatus( int px, int py );
//      extern int  TexWnd_CheckScroll( int n );
//      extern void TexWnd_UpdateScrollRange();
//      extern void TexWnd_ApplyMaterialAtIndex( int idx );
//      extern void TexWnd_Scroll( short zDelta );
// ═════════════════════════════════════════════════════════════════════════════

// Latch the client size (CTexWnd::OnCreate tail, after the base-class create) + the HWND.
void TexWnd_OnCreate( HWND hwnd )
{
    texwndState_t *tex = Ed_TexWnd();
    tex->m_hWnd = hwnd;
    RECT rc; GetClientRect( hwnd, &rc );
    tex->m_nWidth  = rc.right - rc.left;
    tex->m_nHeight = rc.bottom - rc.top;
    texWndGlob_textureOffset.m_nWidth  = tex->m_nWidth;
    texWndGlob_textureOffset.m_nHeight = tex->m_nHeight;
}

void TexWnd_OnSize( HWND hwnd, int cx, int cy )
{
    texwndState_t *tex = Ed_TexWnd();
    tex->m_nWidth = cx; tex->m_nHeight = cy;
    texWndGlob_textureOffset.m_nWidth  = cx;
    texWndGlob_textureOffset.m_nHeight = cy;
    if ( dx.device && cx > 0 && cy > 0 )
        R_Hwnd_Resize( (HWND__ *)hwnd, cx, cy );
}

// HitTest — pane pixel (px,py) → material index in sorted_materials, or -1. Runs the
// SAME flow iterator as DrawMaterials and finds the thumbnail whose cell contains the
// click (TexWnd_SelectMaterial 0x45c520's hit-rect: x in [thumbX, thumbX+thumbW),
// y in [rowY, rowY+thumbH+labelBand]). The iterator works in UNSCROLLED coordinates,
// so the click is lifted by the scroll offset.
int TexWnd_HitTest( int px, int py )
{
    Font_s *font = (Font_s *)g_qeglobals.d_font_list;
    if ( !font ) return -1;
    int y = py + Ed_TexWnd()->m_scrollY;
    MaterialIter_t it;
    TexWnd_SetupIter( &it, font );
    while ( TexWnd_IterateMaterials( &it ) )
    {
        if ( px >= it.thumbX && px < it.thumbX + it.thumbW &&
             y  >= it.rowY   && y  <= it.rowY + it.thumbH + it.labelBand )
            return it.sortedIndex;
    }
    return -1;
}

// ShowMaterialStatus — the click status-bar readout (TexWnd_SelectMaterial 0x45c520).
// Runs the SAME flow iterator + hit-rect as HitTest; on a hit, write "<name> W: w H: h"
// into status pane 3 (the binary's get_m_strStatus(&m_strStatus[3]) + UpdateStatusText,
// here the decoupled MainFrm_SetStatusText sink); on a miss, "Did not select a texture"
// to d_hwndStatus (the binary's SendMessageA(d_hwndStatus, ...) branch).
void TexWnd_ShowMaterialStatus( int px, int py )
{
    Font_s *font = (Font_s *)g_qeglobals.d_font_list;
    if ( !font ) return;
    int y = py + Ed_TexWnd()->m_scrollY;
    MaterialIter_t it;
    TexWnd_SetupIter( &it, font );
    while ( TexWnd_IterateMaterials( &it ) )
    {
        iassert( it.radMtl );
        if ( px >= it.thumbX && px < it.thumbX + it.thumbW &&
             y  >= it.rowY   && y  <= it.rowY + it.thumbH + it.labelBand )
        {
            qtexture_s *q = it.radMtl;
            extern bool Cam_MaterialIsOverlay( Material *handle );   // camwnd.cpp (KIWI-UX decal badge)
            MainFrm_SetStatusText( 3, va( "%s W: %i H: %i%s", q->name, q->width, q->height,
                                          q->next && Cam_MaterialIsOverlay( q->next )
                                          ? "  [decal/overlay - needs a surface under it]" : "" ) );
            return;
        }
    }
    ::SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                    (LPARAM)"Did not select a texture\n" );  // global, not CWnd::SendMessageA
}

// DrawMaterials — the 2D THUMBNAIL grid (0x45cc40 TexWnd_DrawMaterials).
// Called between ProjectionSet2D and R_EndFrame (2D pane-pixel coordinates active).
// Each visible thumbnail = R_AddCmdDraw2DImage(world material colormap) with the
// material name above it; the selected thumbnail gets a colors[10] frame. Off-screen rows
// are laid out (for scroll extent + hit-test) but not drawn.
void TexWnd_DrawMaterials()
{
    texwndState_t *tex = Ed_TexWnd();
    texWndGlob_textureOffset.m_nWidth  = tex->m_nWidth;
    texWndGlob_textureOffset.m_nHeight = tex->m_nHeight;

    static const float s_white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    Font_s *font = (Font_s *)g_qeglobals.d_font_list;
    if ( !font ) return;

    MaterialIter_t it;
    TexWnd_SetupIter( &it, font );
    int contentBottom = 0;

    while ( TexWnd_IterateMaterials( &it ) )
    {
        qtexture_s *q = it.radMtl;
        int rowBottom = it.rowY + it.labelBand + it.thumbH;
        if ( rowBottom > contentBottom ) contentBottom = rowBottom;

        int screenY = it.rowY - tex->m_scrollY;                   // top of cell on screen
        if ( screenY >= tex->m_nHeight || screenY + it.labelBand + it.thumbH < 0 )
            continue;                                             // vertical cull

        // lazily ensure the engine handle is registered (already set for map materials).
        // The registered handle must be this material's.
        if ( !q->next )
        {
            qtexture_s *rv = Texture_GetHandle( (char *)q->name );
            iassert( rv == q );
            iassert( q->next );
        }
        Material *mtl = q->next;

        float x = (float)it.thumbX;
        float yImg = (float)( it.labelBand + screenY );           // image top (label band above)
        // RB_DrawStretchPic forces TECHNIQUE_UNLIT (which samples the colormap) — always
        // present for registered editor materials (the matsys invariant, see Cam_TechAvailable).
        // Guard a partially-loaded material (NULL techniqueSet) defensively: the stretch-pic
        // backend, unlike the camera's DrawTris, has no technique-missing skip.
        if ( mtl && mtl->techniqueSet )
            R_AddCmdDraw2DImage( x, yImg, (float)it.thumbW, (float)it.thumbH,
                                 0.0f, 0.0f, 1.0f, 1.0f, s_white, mtl );

        // KIWI-UX: decal badge - an orange corner square on materials made to sit on top of
        // something (blended, no depth write; camwnd.cpp Cam_MaterialIsOverlay).  As a brush
        // face's main material they show whatever is behind them.
        extern bool Cam_MaterialIsOverlay( Material *handle );   // camwnd.cpp
        if ( mtl && mtl->techniqueSet && rgp.whiteMaterial && Cam_MaterialIsOverlay( mtl ) )
        {
            static const float s_badge[4]  = { 1.0f, 0.55f, 0.08f, 1.0f };
            static const float s_border[4] = { 0.08f, 0.08f, 0.08f, 1.0f };
            int b = it.thumbW / 6;
            if ( b < 8 )  b = 8;
            if ( b > 14 ) b = 14;
            const int bx = it.thumbX + it.thumbW - b - 2, by = (int)yImg + 2;
            R_AddCmdDraw2DImage( (float)bx, (float)by, (float)b, (float)b,
                                 0.0f, 0.0f, 1.0f, 1.0f, s_badge, rgp.whiteMaterial );
            R_DrawOutlineRect( bx - 1, by - 1, bx + b, by + b, s_border );
        }

        // Selected thumbnail gets a 1px-outset frame in the saved "selected texture" colour
        // colors[10], via R_DrawOutlineRect (R_AddCmd_Line2D).
        if ( it.sortedIndex == tex->m_selIndex )
            R_DrawOutlineRect( it.thumbX - 1, (int)yImg - 1,
                               it.thumbX + it.thumbW + 1, (int)yImg + it.thumbH + 1,
                               g_qeglobals.d_savedinfo.colors[10] );

        // Name label at scale 1.0 in the saved text colour colors[8].
        R_AddCmdDrawText( q->name, 0x7FFFFFFF, font, x, yImg,
                          1.0f, 1.0f, 0.0f, g_qeglobals.d_savedinfo.colors[8], 0 );
    }
    tex->m_contentH = contentBottom + TEX_MARGIN;
}

// The CTexWnd::OnPaint pipeline (0x45db20).  The DC (CPaintDC / BeginPaint) belongs to
// the shell, not to this pipeline.
void TexWnd_Paint( HWND hwndArg )
{
    if ( !dx.device )
        return;
    HWND__ *hwnd = (HWND__ *)hwndArg;
    if ( !R_SetupRendertarget_CheckDevice( hwnd ) )
        return;

    R_BeginFrame();
    R_BeginSharedCmdList();
    // Clear to the saved COLOR_TEXTUREBACK (colors[0]), not a hardcoded dark grey.
    R_AddCmdClearScreen( 7, g_qeglobals.d_savedinfo.colors[0], 1.0f, 0 );
    // MATERIAL_COLOR for the thumbnail draw: the binary's CTexWnd::OnPaint does
    // NOT set it at all (BeginFrame->AddClearCmd->SetProjection2D->R_DrawTexWnd), leaving the
    // per-command-buffer default MATERIAL_COLOR = [0,0,0,0] (gfxCmdBufInput is memset 0). The
    // .w == 0 is the load-bearing part: the world/tool UNLIT pixel shader is a vertcol_shaded*
    // variant whose ps_3_0 body is  oC0.rgb = matColor.w*(matColor.rgb - vColor*colorMap)
    // + colorMap*vColor  (c1 = MATERIAL_COLOR). With matColor.w == 0 the flat-override term
    // vanishes and the sampled colorMap shows through; with [1,1,1,1] (w==1) it
    // FULLY replaces every thumbnail with flat white. We DO still emit the set (vs the binary's
    // omission) because kisak's editor fog bridge rides on RB_SetMaterialColorCmd (rb_backend.cpp)
    // to seed the no-fog CONST_SRC_CODE_FOG / FOG_COLOR defaults that the vertcol_simple_fog
    // thumbnail shaders read — the editor has no per-scene fog setup. So: w == 0 (the binary's
    // matColor state) + the fog bridge fires. Verified on real GPU pixels (mp_test browser,
    // GetRenderTargetData→LockRect): aa_default/_default_water 255,255,255→their real texture,
    // ac_building_* keep their texture, vs all-white at w == 1.
    { static const float s_matColor[4] = { 1.0f, 1.0f, 1.0f, 0.0f }; R_AddCmdSetMaterialColor( s_matColor ); }
    R_AddCmdProjectionSet2D();      // = SetProjection2D (RC_PROJECTION_SET / GFX_PROJECTION_2D)
    // R_DrawTexWnd (0x45d0f0) dispatch: the layered-material sub-view when the mode
    // flag is set, else the normal thumbnail grid (the default; the right-click menu
    // switches g_texwnd_simple_layered_selection).
    if ( g_texwnd_simple_layered_selection == 1 )
        TexWnd_DrawLayeredMaterials();
    else
        TexWnd_DrawMaterials();
    TexWnd_UpdateScrollRange();    // set the WS_VSCROLL range from the laid-out content

    R_EndFrame();
    R_IssueRenderCommands( (uint)-1 );
    R_SortMaterials();
    R_CheckTargetWindow( hwnd );
}

// P5 RTT: the same TexWnd_Paint pipeline, rendering into RTT_TEXTURE's offscreen texture (for
// ImGui to sample) instead of a native window.  The window-setup (R_SetupRendertarget_CheckDevice)
// is replaced by RTT_Begin, the tail R_CheckTargetWindow is dropped, and the frame ends with
// RTT_End.  `w`/`h` come from the ImGui dock cell.
//
// STOP/CAVEAT: the paint body ends with TexWnd_UpdateScrollRange(), which drives the
// native WS_VSCROLL bar via ::SetScrollInfo/::SetScrollPos( Ed_TexWnd()->m_hWnd, ... )
// (TexWnd_UpdateScrollRange and TexWnd_CheckScroll).  That is the ONLY HWND touch in this
// draw path (TexWnd_DrawMaterials/DrawLayeredMaterials themselves take no HWND).  It is kept here
// to mirror TexWnd_Paint exactly and is harmless while the child window still exists during the
// shell transition, but the texture child window CANNOT be deleted until scroll-range/position is
// migrated to ImGui.
void TexWnd_RenderToRT( int w, int h )
{
    PROF_SCOPED( "Texture RenderToRT" );
    if ( !dx.device || w < 1 || h < 1 )
        return;
    texwndState_t *tex = Ed_TexWnd();
    // Drive the viewport's own size state from the dock-cell size (was set by TexWnd_OnSize).
    tex->m_nWidth  = w;   tex->m_nHeight = h;
    texWndGlob_textureOffset.m_nWidth  = w;
    texWndGlob_textureOffset.m_nHeight = h;
    {
        if ( !RTT_Begin( RTT_TEXTURE, w, h ) )   // points FRAME_BUFFER at the RT + suppresses Present
            return;
    }

    {
        R_BeginFrame();
        R_BeginSharedCmdList();
        R_AddCmdClearScreen( 7, g_qeglobals.d_savedinfo.colors[0], 1.0f, 0 );
        { static const float s_matColor[4] = { 1.0f, 1.0f, 1.0f, 0.0f }; R_AddCmdSetMaterialColor( s_matColor ); }
        R_AddCmdProjectionSet2D();      // = SetProjection2D (RC_PROJECTION_SET / GFX_PROJECTION_2D)
    }
    if ( g_texwnd_simple_layered_selection == 1 )
        TexWnd_DrawLayeredMaterials();
    else
        TexWnd_DrawMaterials();
    TexWnd_UpdateScrollRange();    // set the WS_VSCROLL range from the laid-out content (see STOP above)

    {
        R_EndFrame();
    }
    R_IssueRenderCommands( (uint)-1 );
    {
        R_SortMaterials();
    }
    {
        RTT_End();
    }
}

// ApplyMaterialAtIndex — select material `idx` and apply it to the current face
// selection (the body of a thumbnail click), factored out of OnLButtonDown.
// KIWI-UX (ROUND AK, ITEM 3): the patch density re-lay that composes with the
// ported texture apply.  Defined in pmesh.cpp beside Patch_KiwiFinishNewLike,
// because it needs that file's static Patch_WidthDistanceTo / HeightDistanceTo.
extern void Patch_KiwiReNaturalizeSelected();                  // pmesh.cpp (ROUND AK)
// ── KIWI-UX (ROUND BH, ITEMS 2 + 3): the two hooks this funnel now carries ──────
// ITEM 3 at the HEAD: a face click in Face mode auto-enters KIWI_CMD_MOVE and parks
//   it, and that gesture's baseline covers the face's whole MaterialDef block — so a
//   material applied while it is parked is written back out by the gesture's Cancel
//   (kiwi_transform.cpp:1573) or by its next push frame (:2638).  That is the entire
//   "texture applications require a right-click/enter to confirm" report.  The full
//   chain is on KiwiUv_EndGestureBeforeApply (kiwi_uv.cpp).  It must run BEFORE the
//   apply — a Cancel afterwards would revert the apply it exists to protect.
// Declared at FILE scope for the same reason Patch_KiwiReNaturalizeSelected above is
// (round AI's MSVC namespace-mangling link error on a block-scope extern).
extern bool KiwiUv_EndGestureBeforeApply( const char *what );  // kiwi_uv.cpp (ROUND BH)
extern void KiwiUv_RestoreGestureAfterApply();                 // kiwi_uv.cpp (ROUND BH)

void TexWnd_ApplyMaterialAtIndex( int idx )
{
    texwndState_t *tex = Ed_TexWnd();
    if ( idx < 0 || idx >= texWndGlob_textureOffset.materialCount )
        return;
    // KIWI-UX (ROUND BH, ITEM 3) — see the note above.  A live gesture that refuses to
    // yield (one that has applied something and does not opt into the round-Z swap
    // protocol) blocks the apply and says so on the console, rather than letting the
    // apply land and be silently rolled back.
    if ( !KiwiUv_EndGestureBeforeApply( "Texture" ) )
        return;
    {
        tex->m_selIndex = idx;
        qtexture_s *q = texWndGlob_textureOffset.sorted_materials[idx];
        if ( q && q->name )
        {
            // Freshen + VALIDATE the face selection before applying. Under random input a
            // prior edit can bump a brush's def->version without rebuilding its faceVis
            // (so b->version != b->def->version), or change its faceCount (reallocating
            // b->faces so selFace.face dangles) — either trips Brush_SetTexture's selFace
            // asserts. Rebuild each selected brush's faceVis (Brush_CheckBuildFaceVis, no realloc when
            // faceCount is unchanged → selFace.face stays valid), then drop the whole face
            // selection if any entry is still inconsistent. (monkey hardening.)
            int n = g_SelectedFaces.GetSize();
            bool selOk = ( n >= 0 ) && ( n == 0 || g_SelectedFaces.m_pData != nullptr );
            for ( int i = 0; selOk && i < n; ++i )
            {
                selbrush_t *b = g_SelectedFaces.GetAt( i ).brush;
                if ( !b || !b->def ) { selOk = false; break; }
                Brush_CheckBuildFaceVis( b, (const float *)world_orient_matrix );
                int fi = g_SelectedFaces.GetAt( i ).index;
                if ( (unsigned)fi >= (unsigned)b->faceCount ||
                     b->version != b->def->version ||
                     g_SelectedFaces.GetAt( i ).face != &( (faceVis_s *)b->faces )[fi] )
                    selOk = false;
            }
            if ( !selOk ) { g_SelectedFaces.m_nSize = 0; n = 0; }

            // 0x45C0D0: a thumbnail click builds a fresh 36-byte MaterialDef from
            // the clicked qtexture, with texdef size = qtexture auto-scale * current
            // layer sampleSize. It does not preserve the previous face/current mapping.
            MaterialDef mat{};
            TexWnd_BuildClickedMaterialDef( q, &mat );
            Texture_SetTexture( nullptr, &mat );

            // ── KIWI-UX (ROUND AK, ITEM 3): PATCHES KEEP THEIR DENSITY ──────
            // USER REPORT, verbatim: "bevel is good, but I can't select the curve
            // parts to retexture them.  See how it's messed up?"
            //
            // Texture_SetTexture reaches a patch through Brush_SetAllFacesMaterial with materialOnly,
            // whose patch branch swaps the two material POINTERS and returns
            // (brush.cpp:2852-2871).  It never re-lays ctrl[][].texCoord, so the
            // world-units-per-repeat rescales by newWidth/oldWidth and the surface
            // comes out stretched — which is the second half of the report and is
            // faithful to 0x476ED0, so the ported function is NOT touched.  The
            // composition is done HERE instead, in the one funnel a thumbnail click
            // goes through, over the SAME selected_brushes list the apply walked.
            // The derivation (why nothing needs capturing before the apply) is on
            // Patch_KiwiReNaturalize itself.  Faces are unaffected: the sweep skips
            // every non-patch node.  (The declaration is at FILE scope above this
            // function on purpose — kiwi_ux round AI shipped a link error from a
            // block-scope extern that MSVC mangled with its enclosing namespace.)
            Patch_KiwiReNaturalizeSelected();

            // KIWI (2026-10-10, user: "you dont need to texture fit the caulking"): the
            // round-BH caulk auto-fit is gone; it was a second undo record per caulk.

            // ── KIWI-UX (ROUND BH, ITEM 3): give the face gizmo back ────────
            // AFTER every mutation, so the restarted gesture's BeginFaces
            // baseline captures the material AND the texdef this apply just
            // wrote.  No-op unless the head of this function actually ended an
            // auto-entered face push.
            KiwiUv_RestoreGestureAfterApply();
        }
        g_nUpdateBits = -1;          // redraw camera (the applied face) + this window
        ::InvalidateRect( tex->m_hWnd, nullptr, FALSE );   // == CWnd::Invalidate( FALSE )
    }
}

void TexWnd_OnLButtonDown( int x, int y )
{
    // Status-bar readout of the clicked material (the binary's OnButtonDown →
    // TexWnd_SelectMaterial_02 → TexWnd_SelectMaterial), then select + apply it to the face selection.
    TexWnd_ShowMaterialStatus( x, y );
    int idx = TexWnd_HitTest( x, y );
    if ( idx >= 0 )
        TexWnd_ApplyMaterialAtIndex( idx );
}

// 0x45c8d0  TexWnd_OnRightMouseContextMenu — the texture-window right-click popup:
// "View simple materials" (id 1, MF_CHECKED=8 when the view flag==0) /
// "View layered materials" (id 2, checked when ==1); in layered mode a
// MF_SEPARATOR (0x800) + "Save layered materials" (id 0x88C1).  TrackPopupMenu uses
// TPM_RETURNCMD (0x100) at the recorded screen-space down point.
// Picking 1/2 switches g_texwnd_simple_layered_selection (+g_nUpdateBits W_TEXTURE redraw,
// drained each idle by Radiant_RoutineProcessing → Radiant_UpdateWindows); 0x88C1 (35009)
// saves the library.
extern char LayeredMaterials_Save();      // layeredmaterials.cpp (0x416f40)
static void TexWnd_OnRightMouseContextMenu( int materialIndex ) // KIWI-UX: thread the hit material to the owned popup action
{
    HMENU PopupMenu = CreatePopupMenu();
    AppendMenuA( PopupMenu, g_texwnd_simple_layered_selection != 0 ? 0 : 8, 1u, "View simple materials" );
    AppendMenuA( PopupMenu, g_texwnd_simple_layered_selection != 1 ? 0 : 8, 2u, "View layered materials" );
    if ( g_texwnd_simple_layered_selection == 1 )
    {
        AppendMenuA( PopupMenu, 0x800u, 0, 0 );                              // MF_SEPARATOR
        AppendMenuA( PopupMenu, 0, 0x88C1u, "Save layered materials" );      // id 35009
    }
    qtexture_s *hitMaterial = materialIndex >= 0 ? texWndGlob_textureOffset.sorted_materials[materialIndex] : nullptr;
    // KIWI: the clicked material in the Material window, without changing the browser selection.
    const unsigned int KIWI_MATPROPS_MENU_ID = 0xE510u;
    if ( hitMaterial )
    {
        AppendMenuA( PopupMenu, MF_SEPARATOR, 0, nullptr );
        AppendMenuA( PopupMenu, MF_STRING, KIWI_MATPROPS_MENU_ID, "Material properties" );
    }
    KiwiMatConvert_AppendTextureContextMenu( PopupMenu, hitMaterial ); // KIWI-UX
    // P5 RTT: d_hwndTexture is the hidden child and a poor menu owner; repoint the owner to the
    // visible main frame.  (TPM_RETURNCMD returns the pick directly, so no WM_COMMAND routing.)
    int cmd = TrackPopupMenu( PopupMenu, 0x100u,                            // TPM_RETURNCMD
                              texWndGlob_textureOffset.m_ptDown[0],
                              texWndGlob_textureOffset.m_ptDown[1],
                              0, g_qeglobals.d_hwndMain, 0 );
    if ( (unsigned int)cmd == KIWI_MATPROPS_MENU_ID )
    {
        KiwiMatProps_Open( hitMaterial );
        return;
    }
    if ( KiwiMatConvert_HandleTextureContextCommand( (unsigned int)cmd, hitMaterial ) ) return; // KIWI-UX
    if ( (unsigned int)(cmd - 1) > 1 )
    {
        if ( cmd == 35009 )
            LayeredMaterials_Save();                                         // (return unused)
    }
    else if ( g_texwnd_simple_layered_selection != cmd - 1 )                 // cmd is 1 or 2 → 0 or 1
    {
        g_texwnd_simple_layered_selection = cmd - 1;
        g_nUpdateBits |= W_TEXTURE;
    }
}

// 0x45c9a0  CTexWnd::OnButtonDown (right-button path): record the screen-space down
// point (popup anchor) + the button mask.  The right button does not select a material.
void TexWnd_OnRButtonDown( unsigned int nFlags, int x, int y )
{
    POINT pt;
    GetCursorPos( &pt );
    texWndGlob_textureOffset.m_ptDown[0] = pt.x;
    texWndGlob_textureOffset.m_ptDown[1] = pt.y;
    texWndGlob_textureOffset.lastButtonDown = nFlags;
    (void)x; (void)y;   // the binary's OnButtonDown is standalone (records only); no base call
}

// 0x45ca30  CTexWnd::OnButtonUp: on right-button release, pop the view-mode context
// menu (a right-drag would set m_was_mouse_dragged and pan instead — that path is not
// yet ported, so m_was_mouse_dragged stays 0 and the click always opens the menu).
void TexWnd_OnRButtonUp( unsigned int nFlags, int x, int y )
{
    if ( ( texWndGlob_textureOffset.lastButtonDown & 2 ) != 0 )      // MK_RBUTTON
    {
        if ( texWndGlob_textureOffset.m_was_mouse_dragged )
        {
            texWndGlob_textureOffset.m_was_mouse_dragged = 0;
            ShowCursor( TRUE );
        }
        else
        {
            TexWnd_OnRightMouseContextMenu( TexWnd_HitTest( x, y ) ); // KIWI-UX: context action belongs to the released thumbnail
        }
    }
    (void)nFlags; (void)x; (void)y;   // the binary's OnButtonUp is standalone; no base call (avoids WM_CONTEXTMENU)
}

// 0x45c7c0  CTexWnd::CheckScroll — clamp `n` to [0, contentExtent - paneHeight + 16];
// on change, store it (m_scrollY == the binary's nPos_current), move the OS thumb, repaint.
// m_contentH is the binary's nPos_max (content extent, set by DrawMaterials).
// (CWnd::SetScrollPos/Invalidate/UpdateWindow ARE ::SetScrollPos(m_hWnd,…)/
// ::InvalidateRect(m_hWnd,NULL,FALSE)/::UpdateWindow(m_hWnd).)
int TexWnd_CheckScroll( int n )
{
    texwndState_t *tex = Ed_TexWnd();
    int result = ( tex->m_contentH - tex->m_nHeight ) + 16;
    if ( n > result )
        n = result;
    if ( n < 0 )
        n = 0;
    if ( n != tex->m_scrollY )
    {
        ::SetScrollPos( tex->m_hWnd, SB_VERT, n, TRUE );
        tex->m_scrollY = n;
        ::InvalidateRect( tex->m_hWnd, nullptr, FALSE );
        ::UpdateWindow( tex->m_hWnd );
    }
    return result;
}

// 0x45C830 — set the WS_VSCROLL range/page from the content extent, then clamp.
void TexWnd_UpdateScrollRange()
{
    texwndState_t *tex = Ed_TexWnd();
    SCROLLINFO si = { 0 };
    si.cbSize = sizeof( SCROLLINFO );
    si.fMask  = SIF_RANGE | SIF_PAGE;
    si.nMin   = 0;
    si.nMax   = tex->m_contentH + 16;     // nPos_max + 16
    si.nPage  = tex->m_nHeight;
    ::SetScrollInfo( tex->m_hWnd, SB_VERT, &si, TRUE );
    int result = si.nMax - si.nPage;
    if ( tex->m_scrollY > result )
    {
        tex->m_scrollY = result;
        TexWnd_CheckScroll( result );
    }
}

// UI-rework: shell-facing scroll accessors for the ImGui texture-tab scrollbar (the native
// WS_VSCROLL bar is hidden under RTT). GetScrollMax mirrors TexWnd_CheckScroll's clamp ceiling
// (contentExtent - paneHeight + 16); SetScroll routes through CheckScroll so the value is
// clamped and m_scrollY updates for the next RTT render.
int TexWnd_GetScroll()
{
    return Ed_TexWnd()->m_scrollY;
}

int TexWnd_GetScrollMax()
{
    texwndState_t *tex = Ed_TexWnd();
    int m = ( tex->m_contentH - tex->m_nHeight ) + 16;
    return m > 0 ? m : 0;
}

void TexWnd_SetScroll( int n )
{
    TexWnd_CheckScroll( n );
}

// ── KIWI-UX (ROUND AE): the search UI the searchbar arm was waiting for ─────
// USER DIRECTIVE, verbatim: "add a small searchbar in the textures bar next to
// the new buttons".  The BINARY's filter machinery is already ported and live —
// TexWnd_FilterAccept's searchbar arm (above, IDB 0x45bcdd) reads
// texWndGlob.searchbar_filter/searchbar_buffer, and the layout, hit-test and
// scroll range all flow through that predicate per paint — so the whole "wire a
// search UI" job is this setter plus an ImGui input box in the shell
// (imgui_shell.cpp, next to the round-AC Show All / In Use buttons).  The query
// is lowercased exactly as Texture_GetHandle lowercases names at registration
// (_strlwr), which is what makes the substring arm's plain strstr
// case-insensitive in practice.  Scroll resets to the top on every edit: the
// filtered content is almost always shorter than the previous view, and a stale
// offset would show an empty window below the results.
void TexWnd_SetSearchFilter( const char *q )
{
    static char s_buf[64];
    if ( q )
    {
        _snprintf( s_buf, sizeof( s_buf ), "%s", q );
        s_buf[sizeof( s_buf ) - 1] = '\0';
        _strlwr( s_buf );
    }
    else
        s_buf[0] = '\0';
    texWndGlob_textureOffset.searchbar_buffer = s_buf;
    texWndGlob_textureOffset.searchbar_filter = ( s_buf[0] != '\0' );
    TexWnd_CheckScroll( 0 );
}

// 0x45dc80  CTexWnd::OnVScroll — the WM_VSCROLL handler.  (CWnd::GetScrollInfo(nBar,lpsi,
// nMask) just stores nMask into lpsi->fMask and calls ::GetScrollInfo — the body already set
// the identical fMask, so the plain Win32 call is the same request.)
void TexWnd_OnVScroll( unsigned int nSBCode, unsigned int nPos )
{
    texwndState_t *tex = Ed_TexWnd();
    SCROLLINFO si = { 0 };
    si.cbSize = sizeof( SCROLLINFO );
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_TRACKPOS;   // 0x17
    ::GetScrollInfo( tex->m_hWnd, SB_VERT, &si );
    int n = si.nPos;
    switch ( nSBCode )
    {
        case SB_LINEUP:    TexWnd_CheckScroll( si.nPos - 15 );             break;
        case SB_LINEDOWN:  TexWnd_CheckScroll( si.nPos + 15 );             break;
        case SB_PAGEUP:    TexWnd_CheckScroll( si.nPos - tex->m_nHeight ); break;
        case SB_PAGEDOWN:  TexWnd_CheckScroll( si.nPos + tex->m_nHeight ); break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: n = si.nTrackPos;
            // fall through
        default:           TexWnd_CheckScroll( n );                       break;
    }
    (void)nPos;   // the binary's handler reads the live SCROLLINFO, not the message's 16-bit nPos
}

// 0x45dd80  CTexWnd::Scroll — mouse-wheel = half-page step, sign from the wheel direction.
void TexWnd_Scroll( short zDelta )
{
    texwndState_t *tex = Ed_TexWnd();
    int step = ( zDelta < 0 ) ? ( tex->m_nHeight / -2 ) : ( tex->m_nHeight / 2 );
    TexWnd_CheckScroll( tex->m_scrollY - step );
}

// CTexWnd::OnMouseWheel: scroll, then let the base class run (the MFC override's tail is
// `return CWnd::OnMouseWheel(...)`, i.e. the default processing).  Returns 0 = "not handled,
// chain on", which is what the MFC path effectively did.
int TexWnd_OnMouseWheel( short zDelta )
{
    TexWnd_Scroll( zDelta );
    return 0;
}

// ══════════════════════════════════════════════════════════════════════════════
//  0x45B910  Material_SetMode — switch the face DISPLAY MODE (Textures→Render Method:
//  Material=0 / Lightmap=1 / Smoothing=2).  Sets g_qeglobals.current_edit_layer (the
//  MaterialDef layer index every face-texture access keys off — brush.cpp/camwnd.cpp/
//  csg.cpp/filters.cpp), invalidates the texture-window scrollbar range, radio-checks the
//  three menu items, refreshes the open surface-inspector, rebuilds the
//  three brush lists' display state, copies selected-face values, and requests a redraw.
//  Menu IDs 33232/33233/36100 are the binary's ID_Material/ID_Lightmap/ID_Smoothing.
// ══════════════════════════════════════════════════════════════════════════════
// surfDlgGlob (surface inspector; .hwnd) comes from qe3.h
extern void  Surf_RefreshFields();                      // surfacedlg.cpp (Select_SetTexture_2 field refresh)
extern void  CopySelectedFaceValues();                  // brush.cpp 0x47d130

void Material_SetMode( int iMode )
{
    if ( iMode == g_qeglobals.current_edit_layer )
        return;

    g_qeglobals.current_edit_layer  = iMode;
    KiwiMatConvert_OnMaterialModeChanged( iMode ); // KIWI-UX: Shift+L reports loaded-techset health before the diagnostic rebuild
    texWndGlob_textureOffset.m_bNeedRange = false;        // scrollbar range now stale

    // Radio-check the three Render Method menu items (checked iff its mode is active).
    HMENU menu = ::GetMenu( g_qeglobals.d_hwndMain );
    if ( menu )
    {
        ::CheckMenuItem( menu, 33232 /*Material*/,  iMode != 0 ? MF_UNCHECKED : MF_CHECKED );
        ::CheckMenuItem( menu, 33233 /*Lightmap*/,  iMode != 1 ? MF_UNCHECKED : MF_CHECKED );
        ::CheckMenuItem( menu, 36100 /*Smoothing*/, iMode != 2 ? MF_UNCHECKED : MF_CHECKED );
    }

    if ( surfDlgGlob.hwnd )
        Surf_RefreshFields();                             // Select_SetTexture_2 — refresh the fields (KIWI: SetTexMods is gone)


    BrushList_RebuildDisplay( &active_brushes );
    BrushList_RebuildDisplay( &selected_brushes );
    BrushList_RebuildDisplay( &filtered_brushes );
    CopySelectedFaceValues();

    // 0x13 repaints camera, XY, and the texture browser.
    g_nUpdateBits |= ( W_CAMERA | W_XY | W_TEXTURE );
}

// ══════════════════════════════════════════════════════════════════════════════
//  0x45A520  Texture_SetMode — the Textures→Render Method RADIO (menu ids 32990..32994:
//  Wireframe / Fullbright / Normal-based Fake Lighting / View-based Fake Lighting /
//  Case textures).  Stores the picked menu id in d_savedinfo.iTextMenu.  The binary also
//  pushes the matching camera draw_mode 0..4 into the 3D view.
//  The default case asserts "unhandled case" at TexWnd.cpp:192 unless ASSERT_UNKOWN.
//  DISTINCT from Material_SetMode above (33232/33233/36100) — that one is the layer radio.
//  Reached via Cmd_OnRendermethodCaseTextures (mainfrm.cpp; the binary's
//  CMainFrame::OnRendermethodCaseTextures 0x4243D0, ON_COMMAND_RANGE).
// ══════════════════════════════════════════════════════════════════════════════
void Texture_SetMode( int iTexMenu )
{
    int mode;
    switch ( iTexMenu )
    {
    case 32990: mode = 0; break;      // ID_RenderMethod_Wireframe
    case 32991: mode = 1; break;      // ID_RenderMethod_Fullbright
    case 32992: mode = 2; break;      // ID_RenderMethod_Normal_basedFakeLighting
    case 32993: mode = 3; break;      // ID_RenderMethod_View_basedFakeLighting
    case 32994: mode = 4; break;      // ID_RenderMethod_Casetextures
    default:
        iassert( 0 && "unhandled case" );   // Assert(TexWnd.cpp, 192, 1, "unhandled case")
        return;
    }

    g_qeglobals.d_savedinfo.iTextMenu = iTexMenu;
    // KIWI: push the camera draw mode again - the ImGui port dropped it with the MFC camera
    // window, so the radio stored the choice and the 3D view ignored it
    camera_s *cam = Ed_Camera();
    if ( cam->draw_mode != mode )
    {
        cam->draw_mode = mode;
        g_nUpdateBits |= W_CAMERA;
    }
}

// KIWI: CTexWnd_Shutdown (0x45d1e0, the material-set teardown OnDestroy / QE_LoadProject ran)
// is gone - KIWI loads the project once and the process exit reclaims the heap;
// Editor_VB_ReleaseForReset releases the editor vertex buffers on shutdown and device reset.
extern struct TexWndGlob_t { void *materialNameRemap; } texWndGlob;   // engine_stubs.cpp

// ══════════════════════════════════════════════════════════════════════════════
//  TEXTURE-FILTER MENU SUBSYSTEM (Textures-menu Usage / Locale / Surface type submenus).
//    TexFilter_LoadMenuFile   0x45B010 (qe3.cpp — the line-by-line table loader)
//    AppendTextureFilterMenu  0x45AFB0 (builds one CreateMenu() popup + appends it)
//    FillTextureMenu          0x45B260 (loads usage.txt/locale.txt, appends the 3 submenus)
//    TexWnd_UsageFilter       0x45B3B0  → Cmd_OnFilterUsage       (ON_COMMAND_RANGE 60000..60255)
//    TexWnd_localFilter       0x45B490  → Cmd_OnFilterLocale      (ON_COMMAND_RANGE 60256..60511)
//    TexWnd_SurfaceTypeFilter 0x45B570  → Cmd_OnFilterSurfaceType (ON_COMMAND_RANGE 60512..60767)
//  FillTextureMenu runs from Radiant_BootFrame (radiant_main.cpp, the QE_LoadProject analog),
//  after the menu is installed — it needs the Textures popup (KTEX_TEXTURES_MENU) live.
// ══════════════════════════════════════════════════════════════════════════════
extern int TexFilter_LoadMenuFile( const char *txt, void *dest, int startId );  // qe3.cpp (0x45B010)
extern const char *Dvar_GetString( const char *dvarName );                       // qcommon (fs_basepath)

// filter_surfacetype_array (0x73AF80) — 29 STATIC entries {name, surfaceFlag}.  Unlike
// the usage/locale arrays (loaded from text), this is a compiled-in table; the `index` is the
// material surface-flag bit (e.g. asphalt = 22<<20) TexWnd_FilterAccept compares against
// surfaceFlags & SURF_TYPE_MASK; the menu id is the array position.
// The binary's .data table (the strings are string-literals, never freed).
RadiantFilterEntry filter_surfacetype_array[29] = {
    { (char *)"all", SURF_TYPE_DEFAULT },
    { (char *)"asphalt", SURF_TYPE_ASPHALT },
    { (char *)"bark", SURF_TYPE_BARK },
    { (char *)"brick", SURF_TYPE_BRICK },
    { (char *)"carpet", SURF_TYPE_CARPET },
    { (char *)"ceramic", SURF_TYPE_CERAMIC },
    { (char *)"cloth", SURF_TYPE_CLOTH },
    { (char *)"concrete", SURF_TYPE_CONCRETE },
    { (char *)"cushion", SURF_TYPE_CUSHION },
    { (char *)"dirt", SURF_TYPE_DIRT },
    { (char *)"flesh", SURF_TYPE_FLESH },
    { (char *)"fruit", SURF_TYPE_FRUIT },
    { (char *)"foliage", SURF_TYPE_FOLIAGE },
    { (char *)"glass", SURF_TYPE_GLASS },
    { (char *)"grass", SURF_TYPE_GRASS },
    { (char *)"gravel", SURF_TYPE_GRAVEL },
    { (char *)"ice", SURF_TYPE_ICE },
    { (char *)"metal", SURF_TYPE_METAL },
    { (char *)"mud", SURF_TYPE_MUD },
    { (char *)"painted metal", SURF_TYPE_PAINTEDMETAL },
    { (char *)"paper", SURF_TYPE_PAPER },
    { (char *)"plaster", SURF_TYPE_PLASTER },
    { (char *)"plastic", SURF_TYPE_PLASTIC },
    { (char *)"rock", SURF_TYPE_ROCK },
    { (char *)"rubber", SURF_TYPE_RUBBER },
    { (char *)"sand", SURF_TYPE_SAND },
    { (char *)"snow", SURF_TYPE_SNOW },
    { (char *)"water", SURF_TYPE_WATER },
    { (char *)"wood", SURF_TYPE_WOOD },
};

// AppendTextureFilterMenu (0x45AFB0) — build ONE submenu popup ("Usage"/"Locale"/"Surface type")
// from a filter array and append it to the parent Textures menu.  Each non-NULL entry → a menu
// item with id (idBase + arrayIndex); each NULL entry (a "<separator>" loaded by the loader) →
// MF_SEPARATOR.
static BOOL AppendTextureFilterMenu( HMENU hMenu, LPCSTR lpNewItem,
                                     const RadiantFilterEntry *array, int count, int idBase )
{
    HMENU sub = CreateMenu();
    for ( int v5 = 0; v5 < count; ++v5 )
    {
        if ( array[v5].name )
            AppendMenuA( sub, MF_STRING, v5 + idBase, array[v5].name );
        else
            AppendMenuA( sub, MF_SEPARATOR, 0, 0 );
    }
    return AppendMenuA( hMenu, MF_POPUP, (UINT_PTR)sub, lpNewItem );
}

// Resolve the binary's "../deffiles/materials/<x>.txt" path.  The editor runs from the install
// bin\, where "../deffiles/.." is the deffiles tree; this port can launch from anywhere, so try
// the binary's relative path first, then <fs_basepath>\deffiles\materials\<x>.txt.  Returns the
// resolved path in `out` (the caller passes it straight to TexFilter_LoadMenuFile/fopen).
static const char *ResolveDeffilePath( const char *relative, char *out, size_t outSize )
{
    // 1) the binary's relative path (cwd == install bin\).
    FILE *probe = fopen( relative, "rb" );
    if ( probe ) { fclose( probe ); _snprintf( out, outSize - 1, "%s", relative ); out[outSize - 1] = 0; return out; }
    // 2) <fs_basepath>\<relative-without-leading-../>  (the known install layout).
    const char *base = Dvar_GetString( "fs_basepath" );
    const char *tail = relative;
    while ( *tail == '.' || *tail == '/' || *tail == '\\' ) ++tail;   // strip leading "../"
    if ( base && base[0] )
    {
        _snprintf( out, outSize - 1, "%s\\%s", base, tail );
        out[outSize - 1] = 0;
        return out;
    }
    _snprintf( out, outSize - 1, "%s", relative ); out[outSize - 1] = 0;
    return out;                                                 // fall back to the relative path
}

// FillTextureMenu (0x45B260) — populate the Textures menu's Usage/Locale/Surface-type filter
// submenus.  On first call (both counts 0) it loads usage.txt → filter_usage_array (ids from
// 60000) and locale.txt → filter_locale_array (ids from 60256), then appends all three submenus
// and seeds the decal locale bit (texWndGlob.unk_8 = 1<<localeIndex("decal")) used by filters.cpp
// case-6 "Misc decal".  Finally radio-checks the active usage/locale/surfaceType item ("all" = 0).
// The binary's body, except the path resolution (see ResolveDeffilePath) and the menu position.
// KIWI (2026-09-27): the Textures popup's top-level position.  The binary's 5 counted the
// Grid popup, which KIWI removed (res/radiant.rc), so Textures is now 4.
static const int KTEX_TEXTURES_MENU = 4;

DWORD FillTextureMenu()
{
    HMENU menu    = GetMenu( g_qeglobals.d_hwndMain );
    HMENU subMenu = GetSubMenu( menu, KTEX_TEXTURES_MENU );     // (binary: 5)

    if ( !texWndGlob_textureOffset.usageCount && !texWndGlob_textureOffset.localeCount )
    {
        char path[1024];
        texWndGlob_textureOffset.usageCount =
            TexFilter_LoadMenuFile( ResolveDeffilePath( "../deffiles/materials/usage.txt", path, sizeof( path ) ),
                                    filter_usage_array, 1 );    // startId 1
        texWndGlob_textureOffset.localeCount =
            TexFilter_LoadMenuFile( ResolveDeffilePath( "../deffiles/materials/locale.txt", path, sizeof( path ) ),
                                    filter_locale_array, 0 );   // startId 0
    }

    AppendTextureFilterMenu( subMenu, "Usage",        filter_usage_array,       texWndGlob_textureOffset.usageCount,  60000 );
    AppendTextureFilterMenu( subMenu, "Locale",       filter_locale_array,      texWndGlob_textureOffset.localeCount, 60256 );
    AppendTextureFilterMenu( subMenu, "Surface type", filter_surfacetype_array, 29,                                   60512 );

    texWndGlob_textureOffset.usageFilter       = 0;
    texWndGlob_textureOffset.localeFilter      = 0;
    texWndGlob_textureOffset.surfaceTypeFilter = 0;
    for ( int v2 = 0; v2 < texWndGlob_textureOffset.localeCount; ++v2 )
    {
        const char *name = filter_locale_array[v2].name;
        if ( name && !_stricmp( name, "decal" ) )
            texWndGlob_textureOffset.unk_8 = 1 << filter_locale_array[v2].index;
    }

    CheckMenuItem( subMenu, texWndGlob_textureOffset.usageFilter       + 60000, MF_CHECKED );
    CheckMenuItem( subMenu, texWndGlob_textureOffset.localeFilter      + 60256, MF_CHECKED );
    return CheckMenuItem( subMenu, texWndGlob_textureOffset.surfaceTypeFilter + 60512, MF_CHECKED );
}

// ── Menu-item handlers (ON_COMMAND_RANGE) ──────────────────────────────────────
// All three: mark every registered material in-use (the "Showing all textures" reset — the
// actual per-filter predicate is applied by TexWnd_FilterAccept on the next paint), then radio-
// check the chosen submenu item and store the filter index.  The list is
// walked head→tail via qtexture_s::prev (texWndGlob.qtextures is the list head linked by ->prev).
static void TexWnd_ShowAllTextures()
{
    for ( qtexture_s *q = texWndGlob_textureOffset.qtextures; q; q = q->prev )
        q->is_in_use = 1;
    Sys_Printf( "Showing all textures...\n" );
    g_nUpdateBits |= W_TEXTURE;
}

void TexWnd_UsageFilter( int index )                           // 0x45B3B0
{
    TexWnd_ShowAllTextures();
    iassert( index >= 0 && index < texWndGlob_textureOffset.usageCount ); // TexWnd.cpp:618
    if ( index != texWndGlob_textureOffset.usageFilter )
    {
        HMENU subMenu = GetSubMenu( GetMenu( g_qeglobals.d_hwndMain ), KTEX_TEXTURES_MENU ); // (binary: 5)
        CheckMenuItem( subMenu, texWndGlob_textureOffset.usageFilter + 60000, MF_UNCHECKED );
        CheckMenuItem( subMenu, index + 60000, MF_CHECKED );
        texWndGlob_textureOffset.usageFilter = (byte)index;
        g_nUpdateBits |= W_TEXTURE;
    }
}

void TexWnd_localFilter( int index )                           // 0x45B490
{
    TexWnd_ShowAllTextures();
    iassert( index >= 0 && index < texWndGlob_textureOffset.localeCount ); // TexWnd.cpp:638
    if ( index != texWndGlob_textureOffset.localeFilter )
    {
        HMENU subMenu = GetSubMenu( GetMenu( g_qeglobals.d_hwndMain ), KTEX_TEXTURES_MENU ); // (binary: 5)
        CheckMenuItem( subMenu, texWndGlob_textureOffset.localeFilter + 60256, MF_UNCHECKED );
        CheckMenuItem( subMenu, index + 60256, MF_CHECKED );
        texWndGlob_textureOffset.localeFilter = (byte)index;
        g_nUpdateBits |= W_TEXTURE;
    }
}

void TexWnd_SurfaceTypeFilter( unsigned int index )            // 0x45B570
{
    TexWnd_ShowAllTextures();
    iassert( index <= 0x1C );                                  // TexWnd.cpp:658 (index < 29)
    if ( index != (unsigned int)texWndGlob_textureOffset.surfaceTypeFilter )
    {
        HMENU subMenu = GetSubMenu( GetMenu( g_qeglobals.d_hwndMain ), KTEX_TEXTURES_MENU ); // (binary: 5)
        CheckMenuItem( subMenu, texWndGlob_textureOffset.surfaceTypeFilter + 60512, MF_UNCHECKED );
        CheckMenuItem( subMenu, index + 60512, MF_CHECKED );
        texWndGlob_textureOffset.surfaceTypeFilter = (byte)index;
        g_nUpdateBits |= W_TEXTURE;
    }
}

// ══════════════════════════════════════════════════════════════════════════════
//  LAYERED-MATERIAL SUB-VIEW (R_DrawTexWnd's g_texwnd_simple_layered_selection==1 branch).
//  Draws the loaded layered-material library (lyrMtlGlob.Layers[], from layeredmaterials.cpp)
//  as a vertical list of outlined thumbnails: per entry a name label, then each layer's
//  texture fit into a 64×64 cell, framed by a 2D outline rect.
//    R_DrawOutlineRect            0x45cb10
//    TexWnd_DrawLayeredMaterialEntry  0x45cea0
//    TexWnd_DrawLayeredMaterials  0x45d080
//  The 2D line command R_AddCmd_Line2D (0x4fd180) is a gfx_d3d/r_rendercmds.cpp leaf.
// ══════════════════════════════════════════════════════════════════════════════

// The texture-window MODE flag (0x25E79FC).  0 = the
// normal thumbnail grid (DrawMaterials), 1 = this layered-material sub-view.  Read by the
// whole texwnd cluster in the binary (OnPaint/scroll/right-click/select); the right-click
// menu's View simple / View layered items switch it.  Defined here (the file that
// owns the texture-window draw dispatch).
int g_texwnd_simple_layered_selection = 0;

// 84-byte library entry view (mirrors layeredmaterials.cpp's offset enum + the authoring
// window's LyrMtlEntry).  A library entry holds at most ONE layer (the binary's cap), but the
// draw indexes layer i at handle offset 0x50 + 8*i, as the binary does.
typedef LyrEntry_t LyrMtlDrawEntry;
static_assert( sizeof( LyrMtlDrawEntry ) == sizeof(LyrEntry_t), "native library entry" );
// layer i's handle pointer lives at 0x50 + 8*i (the {id,handle} pair stride is 8).
static inline qtexture_s *LyrMtl_LayerHandle( const LyrMtlDrawEntry *e, int i )
{
    return e->layers[i].handle;
}

// R_DrawOutlineRect (0x45cb10) — draw a 2D rectangle outline (xL,yT)-(xR,yB) in
// colour `rgba` via one R_AddCmd_Line2D(4 lines): 8 GfxPointVertex (4 edges, z=0, every
// vertex carrying the same packed colour), built as an edge loop (top, right, bottom, left).
static void R_DrawOutlineRect( int xL, int yT, int xR, int yB, const float *rgba )
{
    GfxColor col;
    Byte4PackPixelColor( const_cast<float *>( rgba ), &col );

    // 4 line segments (2 verts each) = the rectangle perimeter, in the binary's vertex
    // order: 0:(xL,yT)->(xR,yT) top, 1:(xR,yT)->(xR,yB) right, 2:(xR,yB)->(xL,yB) bottom,
    // 3:(xL,yB)->(xL,yT) left.
    const int ex[8] = { xL, xR,  xR, xR,  xR, xL,  xL, xL };
    const int ey[8] = { yT, yT,  yT, yB,  yB, yB,  yB, yT };

    GfxPointVertex verts[8];
    for ( int i = 0; i < 8; ++i )
    {
        verts[i].xyz[0]  = (float)ex[i];
        verts[i].xyz[1]  = (float)ey[i];
        verts[i].xyz[2]  = 0.0f;
        *(unsigned int *)verts[i].color = col.packed;
    }
    R_AddCmd_Line2D( 4, 1, verts );

    // ── KIWI-UX (ROUND AI, ITEM 1): RESTORE THE WINDOW'S NEUTRAL MATERIAL_COLOR ──
    // USER REPORT, verbatim: "bug in textures view where when selecting a texture,
    // it sometimes turn the other textures in the viewport solid red".
    //
    // ROOT CAUSE.  `R_AddCmd_Line2D` is STATE-DESTRUCTIVE in this port and says so
    // at its own definition (r_rendercmds.cpp:2048-2055): under KISAK_RADIANT it
    // routes through `Ed_EmitLineBatch` (r_rendercmds.cpp:1996), which pushes the
    // run's first vertex colour as the MATERIAL colour (r_rendercmds.cpp:2024) and
    // NEVER puts it back — the binary never needed to, because it parks the neutral
    // {0,0,0,0} once per pass and lets the per-vertex colour drive $line.  The
    // selected-thumbnail frame is drawn in `colors[10]` = {1,0,0,1} (win_qe3.cpp:423),
    // so after this call MATERIAL_COLOR is RED WITH w == 1 — and w is exactly the
    // flat-override weight the thumbnail shader lerps by
    // (`rgb = w*(matColor.rgb - vColor*colorMap) + colorMap*vColor`, the derivation
    // written out at TexWnd_Paint's own seed above).  Every `R_AddCmdDraw2DImage`
    // emitted LATER IN THE SAME FRAME therefore collapses to flat red: the thumbnail
    // is not drawing its texture at all, it is drawing the border's colour over its
    // whole quad.
    //
    // WHY "SOMETIMES", and it is not random — it is POSITIONAL.  The per-frame seed
    // at TexWnd_Paint:1066 / TexWnd_RenderToRT:1110 re-neutralises MATERIAL_COLOR at
    // the top of every paint, so the damage never survives a frame; within a frame it
    // reaches exactly the thumbnails emitted AFTER the selected one.  Scroll the
    // selection off the top (the `continue` cull in TexWnd_DrawMaterials skips its
    // border entirely) or land it on the last visible cell and the grid looks
    // perfect.  That is why the same material appears correct-with-a-red-border in
    // one screenshot and solid red in the next: in the second it is not the selected
    // one, it is merely downstream of it.
    //
    // THE FIX IS THE BRACKET DISCIPLINE EVERY OTHER PASS IN THIS EDITOR ALREADY
    // FOLLOWS — set, draw, put it back: xywnd.cpp:1149/1190 ("restore for subsequent
    // passes/frames"), camwnd.cpp:1501/1439, kiwi_hover.cpp:497/521,
    // kiwi_region.cpp:1290/1409.  texwnd.cpp was the one drawing path in the repo
    // that set a non-neutral colour and never restored it.
    //
    // It is done HERE rather than at the two call sites because both callers
    // (TexWnd_DrawMaterials' selected frame and TexWnd_DrawLayeredMaterialEntry's
    // per-row frame) have the identical hazard, and the second is worse: its frame
    // colour is `colors[8]` = black for a non-active row, which would tint the NEXT
    // entry's layer thumbnails solid black by the same mechanism.  Restoring inside
    // the helper makes the helper safe for any future caller too.
    //
    // The value is the same {1,1,1,0} the two frame openers seed, and the w == 0 is
    // the load-bearing half (see the long note on TexWnd_Paint's seed).  COST: one
    // extra RC_SET_MATERIAL_COLOR per outline drawn — at most one per frame in the
    // grid (only the selected cell is framed), one per row in the layered sub-view.
    // `R_AddCmdSetMaterialColor` does NOT itself dedup (r_rendercmds.cpp:2221); it
    // updates `s_edLastMatColor`, which is what lets the NEXT line batch skip its
    // own push when the colour is unchanged, so the command count is a wash.
    { static const float s_restore[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
      R_AddCmdSetMaterialColor( s_restore ); }
}

// TexWnd_DrawLayeredMaterialEntry (0x45cea0) — draw ONE layered-material entry at
// the running Y cursor `*py`: a name label, each layer's texture (aspect-fit into a 64×64
// cell), and an outline rect framing the row (highlighted when this is the active entry).
// Advances *py by the row height.  `entry` is the 84-byte LyrMtlDrawEntry; its first 64
// bytes are the name string the binary passes directly to R_AddCmdDrawText.
static void TexWnd_DrawLayeredMaterialEntry( LyrMtlDrawEntry *entry, int *py )
{
    Font_s *font = (Font_s *)g_qeglobals.d_font_list;
    int     cursorX  = 8;                                 // per-layer x cursor
    int     fontH    = font ? font->pixelHeight : 0;

    int oldY = *py;
    *py += fontH;
    // label visible band check
    if ( !( fontH + oldY < 0 ) && *py - fontH < texWndGlob_textureOffset.m_nHeight )
        R_AddCmdDrawText( entry->name, 0x7FFFFFFF, font, 8.0f, (float)*py,
                          1.0f, 1.0f, 0.0f, g_qeglobals.d_savedinfo.colors[8], 0 );

    // thumbnail-row vertical cull
    if ( *py + 64 < 0 || *py >= texWndGlob_textureOffset.m_nHeight )
    {
        *py += 72;
        return;
    }

    int frameRight = cursorX;                             // final cursorX after layers
    int layerLast  = entry->layerCount - 1;
    if ( layerLast >= 0 )
    {
        // Walk layers HIGH->LOW, as the binary does.
        for ( int i = layerLast; i >= 0; --i )
        {
            qtexture_s *handle = LyrMtl_LayerHandle( entry, i );

            // An unrealized layer has no drawable technique; keep its layout slot.
            // In the live GUI the layer handle (Texture_GetHandle
            // at parse) and its render Material are always realized; off the realize path
            // (headless / pre-first-realize) handle->next can be NULL and RB_DrawStretchPic
            // has no technique-missing skip.  Skip the image when unrealized — geometry/
            // layout (the cursor advance + outline) stays identical.  (Behaviour-identical
            // in the GUI; matches DrawMaterials' own mtl/techniqueSet guard.)
            int w = handle ? handle->width  : 64;
            int h = handle ? handle->height : 64;

            int fitW = w, fitH = h;
            if ( w >= h )                                 // landscape
            {
                if ( w > 64 ) { fitW = 64; fitH = ( h << 6 ) / w; }   // (h*64)/w, truncated
            }
            else                                          // portrait
            {
                if ( h > 64 ) { fitW = ( w << 6 ) / h; fitH = 64; }   // (w*64)/h, truncated
            }

            float ix = (float)( cursorX + ( 64 - fitW ) / 2 );        // centered in cell
            float iy = (float)( *py    + ( 64 - fitH ) / 2 );
            if ( handle && handle->next && handle->next->techniqueSet )
                R_AddCmdDraw2DImage( ix, iy, (float)fitW, (float)fitH,
                                     0.0f, 0.0f, 1.0f, 1.0f, colorWhite, handle->next );

            cursorX += 72;
        }
        frameRight = cursorX;
    }

    // outline colour: active entry = colors[10] (highlight), else colors[8].
    const float *frameCol =
        ( entry == lyrMtlWndGlob.activeLyrMtl )
            ? g_qeglobals.d_savedinfo.colors[10]
            : g_qeglobals.d_savedinfo.colors[8];
    if ( entry->layerCount )
        frameRight -= 8;                                  // trim the trailing +72 gap to +64
    R_DrawOutlineRect( 7, *py - 1, frameRight + 1, *py + 65, frameCol );
    *py += 72;
}

// TexWnd_DrawLayeredMaterials (0x45d080) — the layered-material sub-view: lay the
// library entries out top-to-bottom (scrolled by nPos[layer].nPos_layered_current),
// drawing each via TexWnd_DrawLayeredMaterialEntry, and record the laid-out content
// extent into nPos[layer].nPos_layered_max.  Returns the content height.
static int TexWnd_DrawLayeredMaterials()
{
    int layer = g_qeglobals.current_edit_layer;
    int y     = 8 - texWndGlob_textureOffset.nPos[layer].nPos_layered_current;

    if ( lyrMtlGlob.entryCount > 0 )
    {
        LyrMtlDrawEntry *e = (LyrMtlDrawEntry *)lyrMtlGlob.Layers;
        for ( int i = 0; i < lyrMtlGlob.entryCount; ++i, ++e )
            TexWnd_DrawLayeredMaterialEntry( e, &y );
        layer = g_qeglobals.current_edit_layer;           // (the binary re-reads after the loop)
    }

    texWndGlob_textureOffset.nPos[layer].nPos_layered_max =
        y + texWndGlob_textureOffset.nPos[layer].nPos_layered_current;
    return y;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Texture_SetTexture (0x45be50) — "make a MaterialDef the current editor
//  texture".  Copies it into random_texture_stuff[layer], realizes its handles,
//  optionally stores a patch-vertex texcoord projection, APPLIES it to the
//  selection (Brush_SetTexture), updates the find/layered-material windows, and
//  scrolls the browser to it (CheckScroll).  Driven by Drag_Begin's middle-button
//  texture pick and CreateEntityFromName's trigger/volume default material.
// ═════════════════════════════════════════════════════════════════════════════

// TexWnd_02 (0x45b690) — the MaterialDef_02 realize callback int(*)(qtexture_s*).
// Lazily registers the radMtl's engine handle.  radMtl->next is the
// rendered Material*; radMtl->name is the material name.
static int TexWnd_02( qtexture_s *radMtl )
{
    iassert( radMtl );                                     // TexWnd.cpp:688
    if ( !radMtl->next )
    {
        RadiantMaterialInfo localRaw;
        radMtl->next = Register_WorldMaterial( radMtl->name, &localRaw );
    }
    return radMtl->next != NULL;   // MaterialDef_02 discards the callback result.
}

// The binary's 0x415ce0 / 0x415d40 push the picked material name into the find dialog's Find
// / Replace field, selected by byte_73C380 (which edit last had focus; toggled by the
// EN_SETFOCUS handlers 0x415DF0/0x415E00).  The find-dialog-visible branch below is empty.

// Is the find/replace dialog on screen?  The binary tests it twice in Texture_SetTexture (once to
// choose the Find/Replace fill, once to decide whether to APPLY the material to the selection),
// so it is one predicate here.
// U-GLOBALS / U-GUARD: CFindTextureDlg (MFC) is gone and the ImGui find panel is not wired
// here, so the predicate is false — which is exactly the "no find dialog open" behaviour (the
// material gets applied via Brush_SetTexture, the field-fill is skipped).
static bool TexWnd_FindDlgVisible()
{
    return false;
}

// Texture_SetTexture: a1 = the picked patch (patchMesh_t, or null);
// a2 = the MaterialDef* to make current.  Returns the CheckScroll result / 0.
//   random_texture_stuff stride is 0x834 (2100) bytes/layer on x86.  The patch projection
//   stores each control point's current-layer ST pair in cur->st[col][row].
char Texture_SetTexture( const int *a1, MaterialDef *a2 )
{
    const int layer = g_qeglobals.current_edit_layer;
    curTexWndLayer_t *cur = &g_qeglobals.random_texture_stuff[layer];

    // 1) Stamp the MaterialDef into the per-layer current-texture template (36 bytes).
    memcpy( cur, a2, 36u );

    // 2) Realize its handles (radMtl->cb once / lyrMtl->cb per layer).
    MaterialDef_02( &cur->mtl, TexWnd_02 );

    // 3) Optional patch-vertex texcoord projection (a1 != 0).
    if ( a1 )
    {
        // KIWI: typed copy.  The binary's raw +17 ints (ctrl@0x38 + 12 on x86) read 32 bytes
        // short on x64 (ctrl@0x58): a pick stored pointer bits and zeros as the patch's ST.
        const patchMesh_t *p = (const patchMesh_t *)a1;
        cur->hasProjection = 1;
        cur->gridWidth     = p->width;
        cur->gridHeight    = p->height;
        for ( int col = 0; col < p->width; ++col )
            for ( int row = 0; row < p->height; ++row )
            {
                cur->st[col][row][0] = p->ctrl[col][row].texCoord.st[2 * layer];
                cur->st[col][row][1] = p->ctrl[col][row].texCoord.st[2 * layer + 1];
            }
    }
    else
    {
        cur->hasProjection = 0;
    }

    g_nUpdateBits |= W_TEXTURE;                            // W_TEXTURE redraw

    // 4) the binary inlines Materialdef_GetName here (MaterialDef.cpp:85 lives in it)
    const char *name = (const char *)Materialdef_GetName( a2 );

    // 5) Find-dialog: push `name` into the field that last had focus (byte_73C380 picks
    //    Find vs Replace).
    if ( TexWnd_FindDlgVisible() )
    {
    }

    // 6) Either add the radMtl as a live layered-material layer, or APPLY to the selection.
    if ( texWndGlob_textureOffset.unk_bool )              // layered-material "live add" armed
    {
        LayeredMaterialWnd_RadMtl( a2->radMtl );
    }
    else if ( !TexWnd_FindDlgVisible() )
    {
        Brush_SetTexture( a2, 1 );                        // THE APPLY
    }

    // 7) Iterate the browser to the chosen material and scroll it into view.
    MaterialIter_t iter;
    TexWnd_SetupIter( &iter, (Font_s *)g_qeglobals.d_font_list );
    if ( !TexWnd_IterateMaterials( &iter ) )
        return 0;
    while ( 1 )
    {
        iassert( iter.radMtl );                           // TexWnd.cpp:1027
        if ( !_stricmp( name, iter.radMtl->name ) )
            break;
        if ( !TexWnd_IterateMaterials( &iter ) )
            return 0;
    }

    // scroll target: keep the matched thumbnail visible.
    int nPos = texWndGlob_textureOffset.nPos[layer].nPos_current;
    if ( iter.labelBand + iter.thumbH + iter.rowY + 8 > nPos + texWndGlob_textureOffset.m_nHeight )
        nPos = iter.thumbH + iter.labelBand - texWndGlob_textureOffset.m_nHeight + iter.rowY + 8;
    if ( iter.rowY < nPos )
        nPos = iter.rowY;
    // CTexWnd::CheckScroll(nPos) works entirely on globals (texWndGlob/d_hwndTexture), hence
    // the free fn TexWnd_CheckScroll over Ed_TexWnd(); the m_hWnd test is the liveness gate
    // (NULL headless → nothing to scroll).
    if ( Ed_TexWnd()->m_hWnd )
        return (char)TexWnd_CheckScroll( nPos );
    return 0;
}

// 0x45B650 — Texture_ResetPosition: scroll the texture browser back to the top and force a
// re-layout.  The binary walks texWndGlob.nPos[0..2] zeroing each nPos_current (the per-layer
// scroll offset), then (simple mode: g_texwnd_simple_layered_selection==0) calls
// TexWnd_SelectMaterial_02(9,9).  That helper reaches TexWnd_SelectMaterial, which calls
// 0x45C0D0 -> Texture_SetTexture for the thumbnail under (9,9), making the first visible
// material the current brush texture.
// Called by Radiant_CheckTextureScale (mainfrm.cpp, texture-window-scale change): after the
// thumbnails re-lay-out at the new scale the old scroll offset is meaningless, so reset it.
//
// PORT NOTE: this port keeps the live scroll offset on the browser state's m_scrollY (the
// binary's nPos_current lives in the global nPos[]).  So this zeros the global
// nPos[] AND resets the live window's m_scrollY to 0, then runs the same
// hit-test/apply path at (9,9) before repainting.  Headless (no window) -> the global reset
// alone; the m_hWnd test is the liveness gate.
void Texture_ResetPosition()
{
    for ( int layer = 0; layer < 3; ++layer )
        texWndGlob_textureOffset.nPos[layer].nPos_current = 0;

    texwndState_t *tex = Ed_TexWnd();
    if ( tex->m_hWnd )
    {
        tex->m_scrollY = 0;
        if ( g_texwnd_simple_layered_selection == 0 )
        {
            int hit = TexWnd_HitTest( 9, 9 );                     // TexWnd_SelectMaterial_02(9,9)
            if ( hit >= 0 )
            {
                TexWnd_ShowMaterialStatus( 9, 9 );
                TexWnd_ApplyMaterialAtIndex( hit );               // 0x45C0D0 -> Texture_SetTexture
            }
        }
        TexWnd_CheckScroll( 0 );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  RELOCATED HOME — this function's embedded Assert() calls name THIS file as
//  their source (see the brush.cpp relocation protocol / line-uniqueness test).
// ═════════════════════════════════════════════════════════════════════════════
#include <universal/q_parse.h>  // Com_BeginParseSession/EndParseSession/SetCSV/ParseExt/
                                // SkipRestOfLine + parseInfo_t (Get_MaterialNames)
extern void  Com_PrintMessage( const char *fmt, ... );             // 0x40A960

// ─────────────────────────────────────────────────────────────────────────────
// 0x45aaa0  Get_MaterialNames - load "MaterialNames.csv" into
// texWndGlob.materialNameRemap, a list of {shaderName, materialName(lowercased), next}
// triples.  Lines are "<shader>,<material>"; '#' and blank lines are skipped.  Consumed by
// Material_BaseNameRemap (engine_stubs.cpp) on the legacy version-0 .map parse path; lives
// for the process (the binary's CTexWnd_Shutdown freed it).
// Its caller Load_Textures (0x45d140) is not ported: Radiant_SeedCurrentTexdefs (mainfrm.cpp)
// calls this and Radiant_BootFrame (radiant_main.cpp) calls Load_Materials.
// ─────────────────────────────────────────────────────────────────────────────
struct MaterialNameRemap_qe3 { char *key; char *value; MaterialNameRemap_qe3 *next; };
// (TexWndGlob_t/texWndGlob declared earlier in this file — engine_stubs.cpp)

void Get_MaterialNames( void )
{
    // texWndGlob.materialNameRemap must be empty when called.
    iassert( texWndGlob.materialNameRemap == NULL );   // TexWnd.cpp:360

    void *data = nullptr;
    if ( LoadFile( "MaterialNames.csv", &data ) < 0 )   // absent → nothing to do
        return;

    Com_BeginParseSession( "MaterialNames.csv" );
    Com_SetCSV( 1 );                                    // parseInfo[..].csv = 1

    const char *text = (const char *)data;
    while ( 1 )
    {
        // first CSV field on the line = shader name (allowLineBreaks=1).
        parseInfo_t *pi = Com_ParseExt( &text, 1 );
        if ( !text )                                    // buffer exhausted
            break;

        const char *shader = pi->token;
        if ( shader[0] == '#' || shader[0] == 0 )       // comment or blank line
        {
            // skip to (and past) the next newline, counting it for line tracking.
            const char *p = text;
            int c = (signed char)*p;
            if ( c )
            {
                while ( 1 )
                {
                    ++p;
                    if ( c == '\n' )
                    {
                        ParseThreadInfo *pt = Com_GetParseThreadInfo();
                        ++pt->parseInfo[pt->parseInfoNum].lines;
                        break;
                    }
                    c = (signed char)*p;
                    if ( !c ) break;
                }
            }
            text = p;
            continue;
        }

        // dup the shader name.
        size_t slen = strlen( shader ) + 1;
        char  *shaderDup = (char *)malloc( slen );
        memcpy( shaderDup, shader, slen );

        // second CSV field on the SAME line = material name (allowLineBreaks=0).
        parseInfo_t *pi2 = Com_ParseExt( &text, 0 );
        const char  *matName = pi2->token;
        if ( matName[0] == 0 )                          // no matching material name
            Com_PrintMessage( "shader name '%s' doesn't have a matching material name in MaterialNames.csv", shaderDup );

        // dup (the binary inlines AllocMaterialString, qe3.cpp:56 head-check included; the
        // empty-name branch above only prints) + lowercase.
        char *matDup = AllocMaterialString( matName );
        _strlwr( matDup );

        // push {shaderDup, matDup, head} onto the remap list.
        MaterialNameRemap_qe3 *node = (MaterialNameRemap_qe3 *)malloc( sizeof( MaterialNameRemap_qe3 ) );
        node->key   = shaderDup;
        node->value = matDup;
        node->next  = (MaterialNameRemap_qe3 *)texWndGlob.materialNameRemap;
        texWndGlob.materialNameRemap = node;

        Com_SkipRestOfLine( &text );
    }

    Com_EndParseSession();                              // underflow guard + pop frame
    free( data );
}

// ═════════════════════════════════════════════════════════════════════════════
//  Raw-Win32 shell — CTexWnd's message map (U-VP-TEX).  Same free fns, same conventions:
//    WM_CREATE      → TexWnd_OnCreate                        (CTexWnd::OnCreate tail; also
//                     latches the HWND into the state — see texwndState_t::m_hWnd)
//    WM_SIZE        → DefWindowProc, then TexWnd_OnSize       (MFC chains CWnd::OnSize FIRST)
//                     cx/cy = LOWORD/HIWORD(lParam), as MFC's ON_WM_SIZE thunk extracts them
//    WM_ERASEBKGND  → return 1                                (CTexWnd::OnEraseBkgnd returns TRUE)
//    WM_PAINT       → BeginPaint + TexWnd_Paint + EndPaint    (CPaintDC's job in the MFC shell)
//    WM_LBUTTONDOWN → TexWnd_OnLButtonDown, THEN DefWindowProc (the MFC handler tail-calls
//                     CWnd::OnLButtonDown, which is Default() → DefWindowProc)
//    WM_RBUTTONDOWN → TexWnd_OnRButtonDown, return 0.  The binary's handler (0x45c9a0) is standalone
//                     (records the popup anchor only) and the MFC override does NOT chain the
//                     base class, so MFC never reaches DefWindowProc either.
//    WM_RBUTTONUP   → TexWnd_OnRButtonUp, return 0.  Same: standalone (0x45ca30); no base call
//                     — deliberately, so the shell never synthesizes WM_CONTEXTMENU on top of
//                     the handler's own TrackPopupMenu.
//    WM_MOUSEWHEEL  → TexWnd_OnMouseWheel, THEN DefWindowProc (the MFC override returns
//                     CWnd::OnMouseWheel(...), i.e. the default processing)
//    WM_VSCROLL     → DefWindowProc, then TexWnd_OnVScroll    (MFC chains CWnd::OnVScroll FIRST)
//                     nSBCode = LOWORD(wParam), nPos = HIWORD(wParam) — MFC's ON_WM_VSCROLL
//                     thunk decomposition; the handler itself re-reads the live SCROLLINFO, and
//                     pScrollBar was always NULL here (a window's own standard scrollbar).
//    x/y = (short)LOWORD/HIWORD(lParam) — client coords, exactly CPoint(lParam);
//    nFlags = wParam — the MK_* word MFC passes as UINT nFlags.
//  No focus/capture entries: CTexWnd's message map has none, so the browser never took focus or
//  capture in the MFC shell either.
//
//  CALLER OBLIGATIONS (radiant_main.cpp's Radiant_CreateRenderWindows):
//    • set g_qeglobals.d_hwndTexture from the new HWND — gfxwrapper.cpp's
//      R_BeginRegistrationInternal asserts it and calls R_InitRendererForWindow on it;
//    • create the browser AFTER the XY window (which is created first because
//      R_BeginRegistrationInternal attaches the device to all five panes in order);
//    • create it WITH WS_VSCROLL (TexWnd_CreateRaw does) — CheckScroll/OnVScroll/UpdateScrollRange
//      drive that scrollbar;
//    • route the central wheel dispatcher's texture-pane branch
//      (CMainFrame::OnScroll 0x42b850; here Radiant_OnMouseWheel) to TexWnd_Scroll();
//    • the W_TEXTURE repaint bit is drained by Radiant_RoutineProcessing, whose
//      KiwiViewDirty_MarkFromUpdateBits feeds the RTT render (the window itself is hidden).
// ═════════════════════════════════════════════════════════════════════════════

static const char *const TEXWND_CLASS_NAME = "KIWITexWnd";

LRESULT CALLBACK TexWnd_WndProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    switch ( msg )
    {
    case WM_CREATE:
        TexWnd_OnCreate( hwnd );
        return 0;

    case WM_SIZE:
    {
        LRESULT r = DefWindowProcA( hwnd, msg, wParam, lParam );
        TexWnd_OnSize( hwnd, (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return r;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint( hwnd, &ps );
        TexWnd_Paint( hwnd );
        EndPaint( hwnd, &ps );
        return 0;
    }

    case WM_LBUTTONDOWN:
        TexWnd_OnLButtonDown( (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        break;      // → DefWindowProc, like CWnd::OnLButtonDown

    case WM_RBUTTONDOWN:
        TexWnd_OnRButtonDown( (unsigned int)wParam,
                              (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return 0;

    case WM_RBUTTONUP:
        TexWnd_OnRButtonUp( (unsigned int)wParam,
                            (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return 0;

    case WM_MOUSEWHEEL:
        // wParam: HIWORD = zDelta, LOWORD = the MK_* flags (the wheel point in lParam is
        // SCREEN-space and unused here — CTexWnd::Scroll steps by half a page).
        TexWnd_OnMouseWheel( (short)HIWORD( wParam ) );
        break;      // → DefWindowProc, like CWnd::OnMouseWheel

    case WM_VSCROLL:
    {
        LRESULT r = DefWindowProcA( hwnd, msg, wParam, lParam );
        TexWnd_OnVScroll( (unsigned int)LOWORD( wParam ), (unsigned int)HIWORD( wParam ) );
        return r;
    }
    }
    return DefWindowProcA( hwnd, msg, wParam, lParam );
}

// The CTexWnd::PreCreateWindow class (CS_OWNDC + no background brush: we present via D3D, so
// the shell must not paint the client area) + the binary's CWnd::Create style, plus
// the browser's own WS_VSCROLL (PreCreateWindow adds it there).
HWND TexWnd_CreateRaw( HWND parent, int x, int y, int w, int h )
{
    static bool s_classRegistered = false;
    HINSTANCE   inst = GetModuleHandleA( nullptr );

    if ( !s_classRegistered )
    {
        WNDCLASSA wc = {};
        wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = TexWnd_WndProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursorA( nullptr, IDC_ARROW );
        wc.hbrBackground = nullptr;
        wc.lpszClassName = TEXWND_CLASS_NAME;
        if ( !RegisterClassA( &wc ) )
            return nullptr;
        s_classRegistered = true;
    }

    // CREATED HIDDEN — see the same note in XYWnd_CreateRaw (xywnd.cpp).
    return CreateWindowExA( 0, TEXWND_CLASS_NAME, nullptr,
                            WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VSCROLL,
                            x, y, w, h, parent, nullptr, inst, nullptr );
}


// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AZ, ITEM 3): THE SORTED-ARRAY ACCESSORS, FOR THE SKY TAB
// ═════════════════════════════════════════════════════════════════════════════
// `TexWnd_GetMaterialListHead` (:94) walks the registry as an UNSORTED linked list; the
// browser itself indexes `sorted_materials`, which `Load_Materials` sorts with Tex_stricmp,
// and the Sky tab needs that same INDEX because its apply goes through
// `TexWnd_ApplyMaterialAtIndex` — the identical funnel a thumbnail click takes, so it
// inherits the ported Brush_SetTexture apply (one undo record over both selected_brushes
// and g_SelectedFaces) and round AK's patch re-naturalize fence, rather than growing a
// second spelling of either.  `texwnd_s` is TU-local, hence accessors — the same pattern
// the listhead one at :94 uses.  See kiwi_skybox.h D-AZ-C / D-AZ-D.
//
// AT THE END OF THE FILE ON PURPOSE.  These first went in beside their sibling at :95 and
// that shifted every line in this file below it by 32, which silently invalidated every
// `texwnd.cpp:NNN` citation in the rest of the tree — and this codebase's comments are
// load-bearing citations.  New non-ported helpers in a ported file belong at the bottom.
int TexWnd_MaterialCount() { return texWndGlob_textureOffset.materialCount; }

qtexture_s *TexWnd_MaterialAt( int idx )
{
    if ( idx < 0 || idx >= texWndGlob_textureOffset.materialCount )
        return nullptr;
    return texWndGlob_textureOffset.sorted_materials[idx];
}

// The MaterialDef a thumbnail CLICK would build for `q` (sub_45C0D0's fresh 36-byte def:
// texdef size = the qtexture auto-scale * the current layer's sampleSize).  Exposed so the
// Sky tab's shell brushes are created with exactly the def a click would have applied,
// instead of a hand-rolled one that would drift from it.  The builder stays static.
void TexWnd_MaterialDefFor( qtexture_s *q, MaterialDef *out )
{
    if ( !q || !out )
        return;
    TexWnd_BuildClickedMaterialDef( q, out );
}

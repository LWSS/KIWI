// Drag/drop plumbing and import-wizard orchestration.
#include "stdafx.h"
#include "qe3.h"

#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <d3d9.h>
#include <imgui/imgui.h>

#include "kiwi_command.h"
#include "kiwi_fmt.h"
#include "kiwi_import.h"
#include "kiwi_iwi.h"
#include "kiwi_matwriter.h"
#include "kiwi_texgrave.h"          // deferred release / deferred reload

#include <gfx_d3d/r_image.h>
#include <gfx_d3d/r_gfx.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

// Ported/cross-file entry points.
extern int  Sys_Printf( const char *fmt, ... );                                          // win_qe3.cpp:118
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1358
extern int  g_nUpdateBits;                                                               // engine_stubs.cpp:773
// texwnd.cpp exposes no public header for these accessors.
extern qtexture_s *TexWnd_RegisterMaterialByName( const char *name );                    // texwnd.cpp:601
extern bool        TexWnd_MakeMaterialCurrentByName( const char *name );                 // texwnd.cpp:792
extern int  texWndGlob_textureOffset_usageCount();                                       // texwnd.cpp:106
extern int  texWndGlob_textureOffset_localeCount();                                      // texwnd.cpp:107

// FillTextureMenu loads usage/locale; surface types are compiled in.
struct RadiantFilterEntry { char *name; int index; };
static_assert( sizeof( RadiantFilterEntry ) == (sizeof(void *) == 8 ? 16 : 8), "filter_material_t must be 8 bytes (IDB)" );
extern RadiantFilterEntry filter_usage_array[256];        // engine_stubs.cpp:821  (IDB 0x739F80)
extern RadiantFilterEntry filter_locale_array[256];       // engine_stubs.cpp:822  (IDB 0x73A780)
extern RadiantFilterEntry filter_surfacetype_array[29];   // texwnd.cpp:1736       (IDB 0x73AF80)

// State.
namespace
{

// Slot index order is the wizard's row order, NOT the material's semantic order.
enum
{
    SLOT_COLOR = 0,
    SLOT_NORMAL,
    SLOT_SPECULAR,
    SLOT_COUNT
};

const char *const kSlotLabel[SLOT_COUNT] = { "Color map", "Normal map", "Specular map" };

// The tree's own naming: the colour map keeps the bare name.
const char *const kSlotWriteSuffix[SLOT_COUNT] = { "", "_nml", "_spc" };

const unsigned kSlotMask[SLOT_COUNT] =
    { KIWI_MAT_SLOT_COLOR, KIWI_MAT_SLOT_NORMAL, KIWI_MAT_SLOT_SPECULAR };

const int kSlotEncoding[SLOT_COUNT] =
    { KIWI_IWI_ENC_COLOR, KIWI_IWI_ENC_NORMAL, KIWI_IWI_ENC_SPECULAR };

struct ImportGroup
{
    std::string path[SLOT_COUNT];
    std::string pairKey;            // the stripped basename the pairing agreed on ("" == none)
    bool        paired = false;     // derive and display the stripped paired name
};

// Pending queue.
std::vector<ImportGroup> s_queue;      // front == the group being edited
bool                     s_openRequest = false;   // an ImGui::OpenPopup is owed
bool                     s_popupOpen   = false;

// Per-file wizard state.
char  s_name[64]        = { 0 };
int   s_templateIndex   = 0;
int   s_usageRow        = 0;      // row in filter_usage_array
int   s_localeRow       = 0;      // row in filter_locale_array
int   s_surfaceRow      = 0;      // row in filter_surfacetype_array, 0 == "(from template)"
int   s_tileW           = 512;
int   s_tileH           = 512;
bool  s_compress        = true;
bool  s_resampleToPot   = true;
bool  s_overwrite       = false;
bool  s_makeCurrent     = false;
bool  s_applyToRest     = false;
char  s_dupCheckedName[80] = { 0 };   // "<templateIndex>/<name>"
bool  s_dupOnDisk          = false;

// Probed sources for the queue front.
kiwiIwiSource_t s_src[SLOT_COUNT]         = {};
bool            s_srcOk[SLOT_COUNT]       = {};
char            s_srcErr[SLOT_COUNT][256] = {};
char            s_status[512]             = { 0 };
bool            s_statusBad               = false;

// autoTexScale and world units per repeat.
// Convert through the sample size of the current layer that a browser click stamps.
float ImportSampleSize()
{
    const float s = g_qeglobals.random_texture_stuff[g_qeglobals.current_edit_layer].sampleSize;
    return ( s > 0.0f ) ? s : 0.25f;   // mainfrm.cpp:734 seeds 0.25f (IDB 0x45d14e)
}

// Refuse zero autoTexScale because Load_Materials treats it as "hide this".
int ImportUnitsFromTexScale( int texScale )
{
    const int units = (int)( (float)texScale * ImportSampleSize() + 0.5f );
    return units > 0 ? units : 1;
}

int ImportTexScaleFromUnits( int units )
{
    const int ts = (int)( (float)units / ImportSampleSize() + 0.5f );
    if ( ts < 1 )     return 1;
    if ( ts > 65535 ) return 65535;
    return ts;
}

// At the default 0.25 sample size, 512 autoTexScale repeats every 128 world units.
const int kImportTexScaleModalMax = 512;

// Previews use managed textures, so no device-reset hook is needed (kiwi_iwi.cpp).
IDirect3DTexture9 *s_preview[SLOT_COUNT]     = {};
std::string        s_previewPath[SLOT_COUNT];

// Batch carry-over.
bool s_batch = false;

// Defer release until the next frame because ImGui may have recorded this pointer already.
void ReleasePreview( int slot )
{
    KiwiTexGrave_Release( s_preview[slot] );   // null-tolerant, so no guard here
    s_preview[slot] = nullptr;
    s_previewPath[slot].clear();
}

void ReleaseAllPreviews()
{
    for ( int i = 0; i < SLOT_COUNT; ++i )
        ReleasePreview( i );
}

// Name derivation.
void SanitiseName( const char *stem, char *out, size_t outSz )
{
    size_t w = 0;
    for ( size_t i = 0; stem[i] && w + 1 < outSz; ++i )
    {
        char c = stem[i];
        if ( c >= 'A' && c <= 'Z' )
            c = (char)( c - 'A' + 'a' );
        if ( c == ' ' || c == '.' || c == '-' )
            c = '_';
        const bool ok = ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_';
        if ( !ok )
            continue;
        if ( c == '_' && w == 0 )       // never lead with an underscore
            continue;
        out[w++] = c;
    }
    out[w] = '\0';
    if ( !out[0] )
        _snprintf( out, outSz, "imported_texture" );
    // The writer's 50-character ceiling preserves the engine's 64-byte path buffers.
    if ( strlen( out ) > 50 )
        out[50] = '\0';
}

void BaseStem( const char *path, char *out, size_t outSz )
{
    out[0] = '\0';
    if ( !path || !path[0] )
        return;
    const char *base = path;
    for ( const char *c = path; *c; ++c )
        if ( *c == '\\' || *c == '/' )
            base = c + 1;
    _snprintf( out, outSz, "%s", base );
    out[outSz - 1] = '\0';
    char *dot = strrchr( out, '.' );
    if ( dot )
        *dot = '\0';
    for ( size_t i = 0; out[i]; ++i )
        if ( out[i] >= 'A' && out[i] <= 'Z' )
            out[i] = (char)( out[i] - 'A' + 'a' );
}

void DeriveName( const char *path, char *out, size_t outSz )
{
    char stem[256];
    BaseStem( path, stem, sizeof( stem ) );
    SanitiseName( stem, out, outSz );
}

// Pairing.
// Group multi-file drops by stripped basename; whole-suffix tests need no longest-first order.
struct SuffixRow { const char *suffix; int slot; };
const SuffixRow kSuffixes[] =
{
    // normal
    { "_nml",      SLOT_NORMAL },   { "_bump",     SLOT_NORMAL },
    { "_norm",     SLOT_NORMAL },   { "_n",        SLOT_NORMAL },
    { "_nm",       SLOT_NORMAL },   { "_nrm",      SLOT_NORMAL },
    { "_normal",   SLOT_NORMAL },   { "_bmp",      SLOT_NORMAL },
    // specular
    { "_spc",      SLOT_SPECULAR }, { "_spec",     SLOT_SPECULAR },
    { "_s",        SLOT_SPECULAR }, { "_specular", SLOT_SPECULAR },
    { "_gloss",    SLOT_SPECULAR },
    // colour
    { "_col",      SLOT_COLOR },    { "_color",    SLOT_COLOR },
    { "_colour",   SLOT_COLOR },    { "_c",        SLOT_COLOR },
    { "_diffuse",  SLOT_COLOR },    { "_diff",     SLOT_COLOR },
    { "_albedo",   SLOT_COLOR },    { "_d",        SLOT_COLOR },
};
const int kSuffixCount = (int)( sizeof( kSuffixes ) / sizeof( kSuffixes[0] ) );

// Unsuffixed stems default to color and keep their full key, pairing name.png with name_n.png.
void ClassifyStem( const char *stem, std::string *key, int *slot )
{
    const size_t len = strlen( stem );
    for ( int i = 0; i < kSuffixCount; ++i )
    {
        const size_t sl = strlen( kSuffixes[i].suffix );
        if ( len <= sl )                      // a stem that is ONLY a suffix has no key
            continue;
        if ( _stricmp( stem + len - sl, kSuffixes[i].suffix ) != 0 )
            continue;
        key->assign( stem, len - sl );
        *slot = kSuffixes[i].slot;
        return;
    }
    key->assign( stem, len );
    *slot = SLOT_COLOR;
}

const char *PairingRuleText()
{
    return "Paired by name: <base> / <base>_col / _c / _d / _diff = color,"
           " _n / _nm / _nml / _nrm / _norm / _normal / _bump = normal,"
           " _s / _spc / _spec / _specular / _gloss = specular.";
}

// Vocabulary helpers.
// Rows with a null name are "<separator>" entries; they are not selectable values.
int FindUsageRowByName( const char *want )
{
    const int n = texWndGlob_textureOffset_usageCount();
    for ( int i = 1; i < n && i < 256; ++i )
        if ( filter_usage_array[i].name && _stricmp( filter_usage_array[i].name, want ) == 0 )
            return i;
    return 0;
}
int FindLocaleRowByName( const char *want )
{
    const int n = texWndGlob_textureOffset_localeCount();
    for ( int i = 1; i < n && i < 256; ++i )
        if ( filter_locale_array[i].name && _stricmp( filter_locale_array[i].name, want ) == 0 )
            return i;
    return 0;
}
int FirstRealRow( const RadiantFilterEntry *arr, int count )
{
    for ( int i = 1; i < count && i < 256; ++i )
        if ( arr[i].name )
            return i;
    return 0;
}

// Empty boot vocabularies would write usage 0, which Load_Materials hides.
bool VocabularyReady()
{
    return texWndGlob_textureOffset_usageCount() > 1 && texWndGlob_textureOffset_localeCount() > 1;
}

int SelectedUsageValue()
{
    if ( s_usageRow > 0 && s_usageRow < 256 && filter_usage_array[s_usageRow].name )
        return filter_usage_array[s_usageRow].index;
    return 0;
}
unsigned SelectedLocaleMask()
{
    // The material's locale field is a BITMASK over locale indices, not an index.
    if ( s_localeRow > 0 && s_localeRow < 256 && filter_locale_array[s_localeRow].name )
    {
        const int bit = filter_locale_array[s_localeRow].index;
        if ( bit >= 0 && bit < 32 )
            return 1u << bit;
    }
    return 0;
}
int SelectedSurfaceType()
{
    // Row 0 inherits; writing the table's {"all", 0} entry would clear the surface type.
    if ( s_surfaceRow <= 0 || s_surfaceRow >= 29 )
        return -1;
    return filter_surfacetype_array[s_surfaceRow].index;
}

// Queue management.
void SetStatus( bool bad, const char *fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    _vsnprintf( s_status, sizeof( s_status ) - 1, fmt, ap );
    va_end( ap );
    s_status[sizeof( s_status ) - 1] = '\0';
    s_statusBad = bad;
}

// Keep probe state synchronized with Browse/[x] path changes.
void ProbeSlot( int slot )
{
    s_srcOk[slot]     = false;
    s_srcErr[slot][0] = '\0';
    memset( &s_src[slot], 0, sizeof( s_src[slot] ) );
    if ( s_queue.empty() || s_queue.front().path[slot].empty() )
        return;
    s_srcOk[slot] = KiwiIwi_Probe( s_queue.front().path[slot].c_str(), &s_src[slot],
                                   s_srcErr[slot], sizeof( s_srcErr[slot] ) );
}

// Prefer an exact resolvable slot match, then the family consuming the most filled slots.
int DefaultTemplateForFilledSlots()
{
    unsigned filled = 0;
    for ( int i = 0; i < SLOT_COUNT; ++i )
        if ( !s_queue.empty() && !s_queue.front().path[i].empty() )
            filled |= kSlotMask[i];

    int best = 0;
    int bestScore = -1;
    for ( int i = 0; i < KiwiMat_TemplateCount(); ++i )
    {
        const kiwiMatTemplateInfo_t *ti = KiwiMat_TemplateInfo( i );
        if ( !ti || !KiwiMat_ResolveTemplate( i ) )
            continue;
        // Built-in fallback maps require an explicit family choice, not an automatic default.
        if ( ( ti->slots & ~filled ) != 0 )
            continue;
        const unsigned used = ti->slots & filled;
        int score = 0;
        for ( int b = 0; b < SLOT_COUNT; ++b )
            if ( used & kSlotMask[b] )
                ++score;
        if ( score > bestScore )
        {
            bestScore = score;
            best      = i;
        }
    }
    return best;
}

// Batch mode preserves user settings but re-derives each file's name and tiling.
void BeginFile( bool keepSettings )
{
    s_overwrite  = false;
    s_dupCheckedName[0] = '\1';   // force the duplicate probe to re-run for the new name
    s_dupOnDisk         = false;
    ReleaseAllPreviews();
    for ( int i = 0; i < SLOT_COUNT; ++i )
    {
        s_srcOk[i]     = false;
        s_srcErr[i][0] = '\0';
        memset( &s_src[i], 0, sizeof( s_src[i] ) );
    }
    if ( s_queue.empty() )
        return;

    const ImportGroup &g = s_queue.front();
    // Strip a suffix only for a real paired group; otherwise use the color filename.
    if ( g.paired && !g.pairKey.empty() )
        SanitiseName( g.pairKey.c_str(), s_name, sizeof( s_name ) );
    else
        DeriveName( g.path[SLOT_COLOR].c_str(), s_name, sizeof( s_name ) );

    // Reserve four of the writer's 50-character image-name budget for optional-map suffixes.
    if ( !g.path[SLOT_NORMAL].empty() || !g.path[SLOT_SPECULAR].empty() )
        if ( strlen( s_name ) > 46 )
            s_name[46] = '\0';

    for ( int i = 0; i < SLOT_COUNT; ++i )
        ProbeSlot( i );

    if ( s_srcOk[SLOT_COLOR] )
    {
        const int w = s_src[SLOT_COLOR].isPowerOfTwo ? s_src[SLOT_COLOR].width  : s_src[SLOT_COLOR].potWidth;
        const int h = s_src[SLOT_COLOR].isPowerOfTwo ? s_src[SLOT_COLOR].height : s_src[SLOT_COLOR].potHeight;
        // Preserve aspect while capping source-derived autoTexScale at a 512-pixel long edge.
        const int longEdge = ( w > h ) ? w : h;
        if ( longEdge > kImportTexScaleModalMax )
        {
            // Integer halving preserves power-of-two dimensions and exact aspect.
            int cw = w, ch = h;
            while ( ( ( cw > ch ) ? cw : ch ) > kImportTexScaleModalMax && cw > 1 && ch > 1 )
            {
                cw >>= 1;
                ch >>= 1;
            }
            s_tileW = cw;
            s_tileH = ch;
        }
        else
        {
            s_tileW = w;
            s_tileH = h;
        }
    }

    if ( !keepSettings )
    {
        s_templateIndex = DefaultTemplateForFilledSlots();
        // Name-based defaults follow loaded vocabularies; otherwise use the first real row.
        s_usageRow  = FindUsageRowByName( "exterior wall" );
        if ( !s_usageRow )
            s_usageRow = FirstRealRow( filter_usage_array, texWndGlob_textureOffset_usageCount() );
        s_localeRow = FindLocaleRowByName( "Generic" );
        if ( !s_localeRow )
            s_localeRow = FirstRealRow( filter_locale_array, texWndGlob_textureOffset_localeCount() );
        s_surfaceRow    = 0;       // "(from template)"
        s_compress      = true;
        s_resampleToPot = true;
        s_makeCurrent   = false;
        s_applyToRest   = false;
    }

    s_status[0] = '\0';
    for ( int i = 0; i < SLOT_COUNT; ++i )
    {
        if ( !s_srcOk[i] || s_src[i].isPowerOfTwo )
            continue;
        SetStatus( false, "%s is %dx%d, not a power of two - it will be resampled to %dx%d.",
                   kSlotLabel[i], s_src[i].width, s_src[i].height,
                   s_src[i].potWidth, s_src[i].potHeight );
        break;
    }
}

void PopFront()
{
    if ( !s_queue.empty() )
        s_queue.erase( s_queue.begin() );
}

void CloseWizard()
{
    ReleaseAllPreviews();
    s_queue.clear();
    s_batch     = false;
    s_popupOpen = false;
    ImGui::CloseCurrentPopup();
}

// Planned output names.
// Centralize names so UI, writes, duplicate checks, reloads, and rollback cannot diverge.
struct PlannedNames
{
    char image[SLOT_COUNT][64];
    bool active[SLOT_COUNT];           // filled by the user AND consumed by the family
    bool droppedBySlot[SLOT_COUNT];    // filled by the user, refused by the family
};

void PlanNames( PlannedNames *out )
{
    memset( out, 0, sizeof( *out ) );
    const kiwiMatTemplateInfo_t *ti = KiwiMat_TemplateInfo( s_templateIndex );
    const unsigned slots = ti ? ti->slots : (unsigned)KIWI_MAT_SLOT_COLOR;
    for ( int i = 0; i < SLOT_COUNT; ++i )
    {
        const bool have     = !s_queue.empty() && !s_queue.front().path[i].empty();
        const bool consumed = ( slots & kSlotMask[i] ) != 0;
        out->active[i]        = have && consumed;
        out->droppedBySlot[i] = have && !consumed;
        if ( out->active[i] )
        {
            _snprintf( out->image[i], sizeof( out->image[i] ), "%s%s", s_name, kSlotWriteSuffix[i] );
            out->image[i][sizeof( out->image[i] ) - 1] = '\0';
        }
    }
}

// Import.
bool PerformImport( char *err, size_t errSz )
{
    if ( s_queue.empty() )
    {
        _snprintf( err, errSz - 1, "nothing queued" );
        err[errSz - 1] = '\0';
        return false;
    }
    const ImportGroup group = s_queue.front();

    PlannedNames plan;
    PlanNames( &plan );

    bool overwriting = KiwiMat_ExistsOnDisk( s_name );
    for ( int i = 0; i < SLOT_COUNT && !overwriting; ++i )
        if ( plan.active[i] )
            overwriting = KiwiIwi_ExistsOnDisk( plan.image[i] );

    // Fresh imports roll back; overwrites keep partial replacements because their originals
    // are already gone, and deleting them would turn corruption into missing assets.
    const bool rollback = !overwriting;

    // Record only images confirmed written for fresh-import rollback.
    char wroteImage[SLOT_COUNT][64] = {};

    // Write and verify each .iwi before writing the material.
    kiwiIwiResult_t res[SLOT_COUNT] = {};
    for ( int i = 0; i < SLOT_COUNT; ++i )
    {
        if ( !plan.active[i] )
            continue;

        char imgQPath[128];
        _snprintf( imgQPath, sizeof( imgQPath ), "images/%s.iwi", plan.image[i] );
        imgQPath[sizeof( imgQPath ) - 1] = '\0';

        kiwiIwiOptions_t opt = {};
        opt.compress      = s_compress;          // COLOR only; the writer ignores it otherwise
        opt.resampleToPot = s_resampleToPot;
        opt.encoding      = kSlotEncoding[i];

        // Writer failures leave no output; only post-write verification can leave a bad file.
        if ( !KiwiIwi_WriteFromFile( group.path[i].c_str(), imgQPath, &opt, &res[i], err, errSz ) )
        {
            if ( rollback )
                KiwiMat_DeleteWritten( nullptr, wroteImage[SLOT_COLOR],
                                       wroteImage[SLOT_NORMAL], wroteImage[SLOT_SPECULAR] );
            return false;
        }
        _snprintf( wroteImage[i], sizeof( wroteImage[i] ), "%s", plan.image[i] );
        wroteImage[i][sizeof( wroteImage[i] ) - 1] = '\0';

        if ( !KiwiIwi_VerifyOnDisk( imgQPath, err, errSz ) )
        {
            if ( rollback )
                KiwiMat_DeleteWritten( nullptr, wroteImage[SLOT_COLOR],
                                       wroteImage[SLOT_NORMAL], wroteImage[SLOT_SPECULAR] );
            else
                Sys_Printf( "Import: '%s' was OVERWRITTEN and the replacement is bad - it was left in place.\n",
                            plan.image[i] );
            return false;
        }
    }

    // Queue cache reloads for next frame: Image_Reload destroys live D3D textures that an
    // earlier panel may already have placed in this frame's ImGui draw list. Logged dimensions
    // therefore remain pre-reload until then.
    if ( overwriting )
        for ( int i = 0; i < SLOT_COUNT; ++i )
            if ( wroteImage[i][0] )
                KiwiTexGrave_ReloadImage( wroteImage[i] );

    // Write the material only after all images verify.
    kiwiMatFields_t f = {};
    _snprintf( f.name, sizeof( f.name ), "%s", s_name );
    _snprintf( f.imageName, sizeof( f.imageName ), "%s", plan.image[SLOT_COLOR] );
    _snprintf( f.normalImageName, sizeof( f.normalImageName ), "%s", plan.image[SLOT_NORMAL] );
    _snprintf( f.specularImageName, sizeof( f.specularImageName ), "%s", plan.image[SLOT_SPECULAR] );
    f.name[sizeof( f.name ) - 1]                           = '\0';
    f.imageName[sizeof( f.imageName ) - 1]                 = '\0';
    f.normalImageName[sizeof( f.normalImageName ) - 1]     = '\0';
    f.specularImageName[sizeof( f.specularImageName ) - 1] = '\0';
    f.usage              = (unsigned char)SelectedUsageValue();
    f.locale             = SelectedLocaleMask();
    f.autoTexScaleWidth  = (unsigned short)( s_tileW > 0 ? s_tileW : res[SLOT_COLOR].width );
    f.autoTexScaleHeight = (unsigned short)( s_tileH > 0 ? s_tileH : res[SLOT_COLOR].height );
    f.surfaceType        = SelectedSurfaceType();

    if ( !KiwiMat_Write( s_templateIndex, &f, err, errSz ) )
    {
        if ( rollback )
            KiwiMat_DeleteWritten( nullptr, wroteImage[SLOT_COLOR],
                                   wroteImage[SLOT_NORMAL], wroteImage[SLOT_SPECULAR] );
        return false;
    }

    // Verify through the engine loader and browser gate.
    kiwiMatVerify_t v = {};
    if ( !KiwiMat_Verify( s_name, &v, err, errSz ) )
    {
        if ( rollback )
            KiwiMat_DeleteWritten( s_name, wroteImage[SLOT_COLOR],
                                   wroteImage[SLOT_NORMAL], wroteImage[SLOT_SPECULAR] );
        else
            Sys_Printf( "Import: '%s' was OVERWRITTEN and the replacement does not verify - it was left in place.\n", s_name );
        return false;
    }

    // Register immediately so the browser needs no restart.
    qtexture_s *q = TexWnd_RegisterMaterialByName( s_name );
    if ( !q )
    {
        if ( rollback )
            KiwiMat_DeleteWritten( s_name, wroteImage[SLOT_COLOR],
                                   wroteImage[SLOT_NORMAL], wroteImage[SLOT_SPECULAR] );
        _snprintf( err, errSz - 1, "the material loaded but the texture browser refused to register it" );
        err[errSz - 1] = '\0';
        return false;
    }
    if ( s_makeCurrent )
        TexWnd_MakeMaterialCurrentByName( s_name );

    static const char *kFormatName[] = { "", "ARGB8888", "", "", "", "", "", "", "", "", "", "DXT1", "DXT3", "DXT5" };

    Sys_Printf( "Import: -> materials/%s%s\n", s_name,
                overwriting ? "  (overwrote existing files)" : "" );
    for ( int i = 0; i < SLOT_COUNT; ++i )
    {
        if ( !plan.active[i] )
            continue;
        const char *fmtName = ( res[i].format >= 0 && res[i].format < 14 )
                                ? kFormatName[res[i].format] : "?";
        Sys_Printf( "        %-12s '%s' -> images/%s.iwi  [%s %dx%d, %d mips, %d bytes%s]\n",
                    kSlotLabel[i], group.path[i].c_str(), plan.image[i], fmtName,
                    res[i].width, res[i].height, res[i].mipCount, res[i].fileSize,
                    res[i].resampled ? ", resampled to POT" : "" );
    }
    Sys_Printf( "        techset '%s'  sortKey %u  usage %u  locale 0x%X  autoTexScale %ux%u "
                "(repeats every %dx%d units)  surfaceFlags 0x%X  contents 0x%X\n",
                v.techSet, (unsigned)v.sortKey, (unsigned)v.usage, (unsigned)v.locale,
                (unsigned)v.autoTexScaleWidth, (unsigned)v.autoTexScaleHeight,
                ImportUnitsFromTexScale( (int)v.autoTexScaleWidth ),
                ImportUnitsFromTexScale( (int)v.autoTexScaleHeight ),
                (unsigned)v.surfaceFlags, (unsigned)v.contents );
    Sys_Printf( "        verified through the engine: colorMap '%s' %dx%d %s",
                v.colorMapImage, v.colorMapWidth, v.colorMapHeight,
                v.colorMapUploaded ? "uploaded" : "MISSING" );
    if ( v.normalMapImage[0] )
        Sys_Printf( ", normalMap '%s' %dx%d %s", v.normalMapImage, v.normalMapWidth,
                    v.normalMapHeight, v.normalMapUploaded ? "uploaded" : "MISSING" );
    if ( v.specularMapImage[0] )
        Sys_Printf( ", specularMap '%s' %dx%d %s", v.specularMapImage, v.specularMapWidth,
                    v.specularMapHeight, v.specularMapUploaded ? "uploaded" : "MISSING" );
    Sys_Printf( ".\n" );

    g_nUpdateBits |= W_TEXTURE;
    return true;
}

// Queueing.
// Multi-file drops group stripped-basename matches; a single file is always color-only.
int QueuePaths( const std::vector<std::string> &paths )
{
    std::vector<std::string> ok;
    for ( size_t i = 0; i < paths.size(); ++i )
    {
        if ( !KiwiIwi_IsAcceptedExtension( paths[i].c_str() ) )
        {
            Sys_Printf( "Import: refused '%s' - only %s are supported.\n",
                        paths[i].c_str(), KiwiIwi_AcceptedExtensionList() );
            continue;
        }
        ok.push_back( paths[i] );
    }
    if ( ok.empty() )
        return 0;

    if ( ok.size() == 1 )
    {
        ImportGroup g;
        g.path[SLOT_COLOR] = ok[0];
        g.paired           = false;
        s_queue.push_back( g );
        return 1;
    }

    const size_t firstNew = s_queue.size();
    int          pairs    = 0;
    for ( size_t i = 0; i < ok.size(); ++i )
    {
        char stem[256];
        BaseStem( ok[i].c_str(), stem, sizeof( stem ) );

        std::string key;
        int         slot = SLOT_COLOR;
        ClassifyStem( stem, &key, &slot );

        size_t at = s_queue.size();
        for ( size_t g = firstNew; g < s_queue.size(); ++g )
        {
            if ( s_queue[g].pairKey != key )
                continue;
            if ( !s_queue[g].path[slot].empty() )
                continue;                     // two files claim the same slot: second stands alone
            at = g;
            break;
        }
        if ( at == s_queue.size() )
        {
            ImportGroup g;
            g.pairKey = key;
            g.paired  = false;
            s_queue.push_back( g );
        }
        else
        {
            s_queue[at].paired = true;
            ++pairs;
        }
        s_queue[at].path[slot] = ok[i];
    }

    // Promote the first optional map when a group has no color instead of dropping the group.
    for ( size_t g = firstNew; g < s_queue.size(); ++g )
    {
        if ( !s_queue[g].path[SLOT_COLOR].empty() )
            continue;
        for ( int s = 1; s < SLOT_COUNT; ++s )
        {
            if ( s_queue[g].path[s].empty() )
                continue;
            Sys_Printf( "Import: '%s' arrived without a color map for '%s' - queued as a color map.\n",
                        s_queue[g].path[s].c_str(), s_queue[g].pairKey.c_str() );
            s_queue[g].path[SLOT_COLOR] = s_queue[g].path[s];
            s_queue[g].path[s].clear();
            s_queue[g].paired = false;
            break;
        }
    }

    if ( pairs )
        Sys_Printf( "Import: %d file%s grouped into %d material%s by name. %s\n",
                    (int)ok.size(), ok.size() == 1 ? "" : "s",
                    (int)( s_queue.size() - firstNew ),
                    ( s_queue.size() - firstNew ) == 1 ? "" : "s", PairingRuleText() );
    return (int)ok.size();
}

}  // namespace

// WM_DROPFILES.
bool KiwiImport_HandleDropFiles( void *hDropOpaque )
{
    HDROP hDrop = (HDROP)hDropOpaque;
    if ( !hDrop )
        return false;

    // Copy every path before DragFinish invalidates the HDROP.
    std::vector<std::string> paths;
    const UINT n = ::DragQueryFileA( hDrop, 0xFFFFFFFF, nullptr, 0 );
    for ( UINT i = 0; i < n; ++i )
    {
        char buf[MAX_PATH];
        buf[0] = '\0';
        if ( ::DragQueryFileA( hDrop, i, buf, (UINT)sizeof( buf ) ) && buf[0] )
            paths.push_back( buf );
    }
    ::DragFinish( hDrop );

    const int accepted = QueuePaths( paths );
    if ( !accepted )
        return false;
    return true;
}

// Commands.
void KiwiImport_RegisterCommands()
{
    // DROPPED is an internal continuation and must remain non-bindable.
    Radiant_RegisterCommand( "KiwiImportTextures", 0, 0, KIWI_CMD_IMPORT_BROWSE );
}

bool KiwiImport_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId == (unsigned int)KIWI_CMD_IMPORT_DROPPED )
    {
        if ( s_queue.empty() )
            return true;               // ours, already consumed (a duplicate post)
        if ( !s_popupOpen )
        {
            s_batch = false;
            BeginFile( false );
            s_openRequest = true;
        }
        return true;
    }

    if ( cmdId == (unsigned int)KIWI_CMD_IMPORT_BROWSE )
    {
        // Multi-select returns one full path or a directory plus NUL-separated filenames.
        static char buf[16384];
        buf[0] = '\0';

        // COMMDLG filters are NUL-terminated pairs followed by a final NUL.
        char   filter[256];
        size_t fp = 0;
        {
            const char *exts = KiwiIwi_AcceptedExtensionList();
            char        label[128];
            _snprintf( label, sizeof( label ), "Images (%s)", exts );
            label[sizeof( label ) - 1] = '\0';
            const char *parts[4] = { label, exts, "All files (*.*)", "*.*" };
            for ( int i = 0; i < 4; ++i )
            {
                const size_t n = strlen( parts[i] );
                if ( fp + n + 2 >= sizeof( filter ) )
                    break;
                memcpy( filter + fp, parts[i], n );
                fp += n;
                filter[fp++] = '\0';
            }
            filter[fp++] = '\0';
        }

        OPENFILENAMEA ofn = { 0 };
        ofn.lStructSize = sizeof( ofn );
        ofn.hwndOwner   = ::GetActiveWindow();
        ofn.lpstrFilter = filter;
        ofn.lpstrFile   = buf;
        ofn.nMaxFile    = (DWORD)sizeof( buf );
        ofn.Flags       = OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_ALLOWMULTISELECT;
        if ( !::GetOpenFileNameA( &ofn ) )
            return true;

        std::vector<std::string> paths;
        const char *dir = buf;
        const char *p   = dir + strlen( dir ) + 1;
        if ( !*p )
        {
            paths.push_back( dir );                    // single selection: buf IS the path
        }
        else
        {
            for ( ; *p; p += strlen( p ) + 1 )
            {
                char full[MAX_PATH];
                _snprintf( full, sizeof( full ), "%s\\%s", dir, p );
                full[sizeof( full ) - 1] = '\0';
                paths.push_back( full );
            }
        }

        if ( QueuePaths( paths ) && !s_popupOpen )
        {
            s_batch = false;
            BeginFile( false );
            s_openRequest = true;
        }
        return true;
    }

    return false;
}

// Wizard.
void KiwiImport_Draw()
{
    // Import at most one batch item per frame to keep pumping messages.
    if ( s_batch && !s_queue.empty() )
    {
        const std::string src = s_queue.front().path[SLOT_COLOR];
        char err[512];
        err[0] = '\0';
        if ( !PerformImport( err, sizeof( err ) ) )
            Sys_Printf( "Import FAILED for '%s': %s\n", src.c_str(), err );
        PopFront();
        if ( !s_queue.empty() )
            BeginFile( true );
        else
            s_batch = false;
        return;
    }

    if ( s_queue.empty() && !s_popupOpen )
        return;

    if ( s_openRequest )
    {
        ImGui::OpenPopup( "Import texture" );
        s_openRequest = false;
        s_popupOpen   = true;
    }
    if ( !s_popupOpen )
        return;

    if ( !ImGui::BeginPopupModal( "Import texture", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        // Treat external dismissal as Cancel All; nothing has been written.
        if ( s_popupOpen )
        {
            ReleaseAllPreviews();
            s_queue.clear();
            s_popupOpen = false;
        }
        return;
    }

    if ( s_queue.empty() )
    {
        CloseWizard();
        ImGui::EndPopup();
        return;
    }

    const std::string src = s_queue.front().path[SLOT_COLOR];

    // Source.
    ImGui::TextDisabled( "Source" );
    if ( s_queue.front().paired )
        ImGui::TextWrapped( "%s", PairingRuleText() );
    if ( s_queue.size() > 1 )
        ImGui::Text( "%d more material%s in this drop.", (int)s_queue.size() - 1,
                     s_queue.size() == 2 ? "" : "s" );

    if ( !s_srcOk[SLOT_COLOR] )
    {
        ImGui::Separator();
        ImGui::TextWrapped( "%s", src.c_str() );
        ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "Cannot read this file: %s",
                            s_srcErr[SLOT_COLOR] );
        ImGui::Separator();
        if ( ImGui::Button( "Skip", ImVec2( 120.0f, 0.0f ) ) )
        {
            PopFront();
            if ( s_queue.empty() )
                CloseWizard();
            else
                BeginFile( true );
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel all", ImVec2( 120.0f, 0.0f ) ) )
            CloseWizard();
        ImGui::EndPopup();
        return;
    }

    // Name.
    ImGui::Separator();
    ImGui::SetNextItemWidth( 320.0f );
    ImGui::InputText( "Material name", s_name, sizeof( s_name ) );
    ImGui::SameLine();
    ImGui::TextDisabled( "(?)" );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Written as materials/<name> and images/<name>.iwi.\n"
                           "A normal map is written as images/<name>_nml.iwi and a\n"
                           "specular map as images/<name>_spc.iwi (the shipped tree's own\n"
                           "suffixes). Registered in the editor as wc/<name>." );

    // Material type.
    // Keep material type before slot rows; their compatibility depends on its selection.
    const int tmplCount = KiwiMat_TemplateCount();
    if ( s_templateIndex < 0 || s_templateIndex >= tmplCount )
        s_templateIndex = 0;
    const kiwiMatTemplateInfo_t *cur = KiwiMat_TemplateInfo( s_templateIndex );
    ImGui::SetNextItemWidth( 320.0f );
    if ( ImGui::BeginCombo( "Material type", cur ? cur->label : "" ) )
    {
        for ( int i = 0; i < tmplCount; ++i )
        {
            const kiwiMatTemplateInfo_t *ti = KiwiMat_TemplateInfo( i );
            const char *resolved = KiwiMat_ResolveTemplate( i );
            ImGui::BeginDisabled( resolved == nullptr );
            if ( ImGui::Selectable( ti->label, i == s_templateIndex ) )
                s_templateIndex = i;
            ImGui::EndDisabled();
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "%s\n\ntechset '%s'\nuses: %s%s%s\ntemplate: %s",
                                   ti->blurb, ti->techSet,
                                   "color",
                                   ( ti->slots & KIWI_MAT_SLOT_NORMAL )   ? " + normal"   : "",
                                   ( ti->slots & KIWI_MAT_SLOT_SPECULAR ) ? " + specular" : "",
                                   resolved ? resolved : "NOT AVAILABLE in this data tree" );
        }
        ImGui::EndCombo();
    }
    if ( cur )
        ImGui::TextDisabled( "%s", cur->blurb );
    const char *curTemplate = KiwiMat_ResolveTemplate( s_templateIndex );
    if ( !curTemplate )
        ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ),
                            "No shipped material of this type was found to clone." );

    // Slots.
    // Release changed previews through the graveyard; ImGui may hold this frame's pointer.
    const unsigned curSlots = cur ? cur->slots : (unsigned)KIWI_MAT_SLOT_COLOR;

    for ( int slot = 0; slot < SLOT_COUNT; ++slot )
    {
        ImGui::PushID( slot );
        ImGui::Separator();

        const std::string &path = s_queue.front().path[slot];
        if ( !path.empty() && s_previewPath[slot] != path )
        {
            ReleasePreview( slot );
            s_preview[slot]     = KiwiIwi_CreatePreview( path.c_str() );
            s_previewPath[slot] = path;
        }

        if ( s_preview[slot] )
        {
            const float side   = 128.0f;
            const float aspect = ( s_src[slot].height > 0 )
                                   ? ( (float)s_src[slot].width / (float)s_src[slot].height ) : 1.0f;
            ImVec2 size = ( aspect >= 1.0f ) ? ImVec2( side, side / aspect )
                                             : ImVec2( side * aspect, side );
            ImGui::Image( (ImTextureID)(intptr_t)s_preview[slot], size );  // imgui_shell.cpp:939's spelling
            ImGui::SameLine();
        }

        ImGui::BeginGroup();
        ImGui::Text( "%s%s", kSlotLabel[slot], slot == SLOT_COLOR ? " (required)" : " (optional)" );
        if ( path.empty() )
        {
            ImGui::TextDisabled( "(none)" );
        }
        else
        {
            ImGui::TextWrapped( "%s", path.c_str() );
            if ( !s_srcOk[slot] )
                ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "Cannot read this file: %s",
                                    s_srcErr[slot] );
            else
            {
                ImGui::Text( "%d x %d, %s", s_src[slot].width, s_src[slot].height,
                             s_src[slot].hasAlpha ? "has alpha" : "no alpha" );
                if ( !s_src[slot].isPowerOfTwo )
                    ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.3f, 1.0f ),
                                        "Not a power of two. The engine's mip counter (Image_CountMipmaps)\n"
                                        "assumes POT, so this must be resampled to %d x %d.",
                                        s_src[slot].potWidth, s_src[slot].potHeight );
                if ( slot == SLOT_SPECULAR && !s_src[slot].hasAlpha )
                    ImGui::TextDisabled( "No alpha: gloss reads as 1, i.e. the sharpest reflection." );
            }
        }

        if ( ImGui::Button( "Browse..." ) )
        {
            char       buf[MAX_PATH];
            char       filter[256];
            size_t     fp   = 0;
            const char *exts = KiwiIwi_AcceptedExtensionList();
            char        label[128];
            buf[0] = '\0';
            _snprintf( label, sizeof( label ), "Images (%s)", exts );
            label[sizeof( label ) - 1] = '\0';
            {
                const char *parts[4] = { label, exts, "All files (*.*)", "*.*" };
                for ( int i = 0; i < 4; ++i )
                {
                    const size_t n = strlen( parts[i] );
                    if ( fp + n + 2 >= sizeof( filter ) )
                        break;
                    memcpy( filter + fp, parts[i], n );
                    fp += n;
                    filter[fp++] = '\0';
                }
                filter[fp++] = '\0';
            }
            OPENFILENAMEA ofn = { 0 };
            ofn.lStructSize = sizeof( ofn );
            ofn.hwndOwner   = ::GetActiveWindow();
            ofn.lpstrFilter = filter;
            ofn.lpstrFile   = buf;
            ofn.nMaxFile    = (DWORD)sizeof( buf );
            ofn.Flags       = OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_EXPLORER;
            if ( ::GetOpenFileNameA( &ofn ) && buf[0] )
            {
                ReleasePreview( slot );
                s_queue.front().path[slot] = buf;
                ProbeSlot( slot );
            }
        }
        if ( !path.empty() && slot != SLOT_COLOR )
        {
            ImGui::SameLine();
            if ( ImGui::Button( "x" ) )
            {
                ReleasePreview( slot );
                s_queue.front().path[slot].clear();
                ProbeSlot( slot );
            }
        }

        const bool consumed = ( curSlots & kSlotMask[slot] ) != 0;
        if ( !path.empty() && !consumed )
            ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.3f, 1.0f ),
                                "This material type has no %s - this file will be SKIPPED.",
                                kSlotLabel[slot] );
        else if ( path.empty() && consumed && slot != SLOT_COLOR )
            ImGui::TextDisabled( "(flat - no %s supplied; binds the built-in %s)",
                                 kSlotLabel[slot],
                                 slot == SLOT_NORMAL ? "$identitynormalmap" : "$black" );
        ImGui::EndGroup();
        ImGui::PopID();
    }

    ImGui::Separator();

    // Planned output and duplicate check.
    // Cache this all-searchpath/.iwd probe until the typed name or family changes.
    PlannedNames plan;
    PlanNames( &plan );

    char dupKey[80];
    _snprintf( dupKey, sizeof( dupKey ), "%d/%s", s_templateIndex, s_name );
    dupKey[sizeof( dupKey ) - 1] = '\0';
    if ( strcmp( s_dupCheckedName, dupKey ) != 0 )
    {
        _snprintf( s_dupCheckedName, sizeof( s_dupCheckedName ), "%s", dupKey );
        s_dupCheckedName[sizeof( s_dupCheckedName ) - 1] = '\0';
        s_dupOnDisk = KiwiMat_ExistsOnDisk( s_name );
        for ( int i = 0; i < SLOT_COUNT && !s_dupOnDisk; ++i )
            if ( plan.active[i] )
                s_dupOnDisk = KiwiIwi_ExistsOnDisk( plan.image[i] );
        // Overwrite consent is per name; every new collision must ask again.
        s_overwrite = false;
    }
    const bool dupOnDisk = s_dupOnDisk;

    ImGui::TextDisabled( "Will write:" );
    ImGui::BulletText( "materials/%s", s_name );
    for ( int i = 0; i < SLOT_COUNT; ++i )
        if ( plan.active[i] )
            ImGui::BulletText( "images/%s.iwi   (%s)", plan.image[i], kSlotLabel[i] );
    for ( int i = 0; i < SLOT_COUNT; ++i )
        if ( plan.droppedBySlot[i] )
            ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.3f, 1.0f ),
                                "Not written: the %s (this material type has no such slot).",
                                kSlotLabel[i] );

    // "images/" + ".iwi" leaves 50 chars in the engine's 64-byte path buffer; suffixes
    // consume four more.
    int nameCeiling = 50;
    for ( int i = 0; i < SLOT_COUNT; ++i )
        if ( plan.active[i] )
        {
            const int room = 50 - (int)strlen( kSlotWriteSuffix[i] );
            if ( room < nameCeiling )
                nameCeiling = room;
        }
    const bool nameFits = ( (int)strlen( s_name ) <= nameCeiling );
    if ( !nameFits )
        ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ),
                            "The name must be %d characters or shorter for these maps "
                            "(the engine's path buffers are 64 bytes).", nameCeiling );

    if ( dupOnDisk )
    {
        ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.3f, 1.0f ),
                            "A material or image with one of these names already exists." );
        ImGui::Checkbox( "Overwrite the existing files", &s_overwrite );
    }
    else
    {
        s_overwrite = false;
    }

    // Usage, locale, and surface type.
    const int usageCount  = texWndGlob_textureOffset_usageCount();
    const int localeCount = texWndGlob_textureOffset_localeCount();

    ImGui::SetNextItemWidth( 320.0f );
    if ( ImGui::BeginCombo( "Usage",
                            ( s_usageRow > 0 && filter_usage_array[s_usageRow].name )
                                ? filter_usage_array[s_usageRow].name : "<none>" ) )
    {
        for ( int i = 1; i < usageCount && i < 256; ++i )
        {
            if ( !filter_usage_array[i].name )   // "<separator>" row
            {
                ImGui::Separator();
                continue;
            }
            if ( ImGui::Selectable( filter_usage_array[i].name, i == s_usageRow ) )
                s_usageRow = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled( "(?)" );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "deffiles/materials/usage.txt - the same vocabulary the\n"
                           "Textures > Usage filter menu is built from. Must not be\n"
                           "empty: Load_Materials hides any material with usage 0." );

    ImGui::SetNextItemWidth( 320.0f );
    if ( ImGui::BeginCombo( "Locale",
                            ( s_localeRow > 0 && filter_locale_array[s_localeRow].name )
                                ? filter_locale_array[s_localeRow].name : "<none>" ) )
    {
        for ( int i = 1; i < localeCount && i < 256; ++i )
        {
            if ( !filter_locale_array[i].name )
            {
                ImGui::Separator();
                continue;
            }
            if ( ImGui::Selectable( filter_locale_array[i].name, i == s_localeRow ) )
                s_localeRow = i;
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled( "(?)" );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "deffiles/materials/locale.txt. Stored as a BIT MASK\n"
                           "(1 << index), which is how TexWnd_FilterAccept tests it." );

    ImGui::SetNextItemWidth( 320.0f );
    if ( ImGui::BeginCombo( "Surface type",
                            ( s_surfaceRow > 0 ) ? filter_surfacetype_array[s_surfaceRow].name
                                                 : "(from template)" ) )
    {
        if ( ImGui::Selectable( "(from template)", s_surfaceRow == 0 ) )
            s_surfaceRow = 0;
        for ( int i = 1; i < 29; ++i )
            if ( ImGui::Selectable( filter_surfacetype_array[i].name, i == s_surfaceRow ) )
                s_surfaceRow = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextDisabled( "(?)" );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "The compiled-in surface-type table (texwnd.cpp:1725).\n"
                           "Drives footstep/impact sounds in game and the\n"
                           "Textures > Surface type browser filter." );

    // Tiling and output.
    // Spinner units are world units per repeat; stored autoTexScale uses the live sample size.
    ImGui::Separator();
    int tile[2] = { ImportUnitsFromTexScale( s_tileW ), ImportUnitsFromTexScale( s_tileH ) };
    ImGui::SetNextItemWidth( 200.0f );
    if ( ImGui::InputInt2( "Tiling (world units per repeat)", tile ) )
    {
        if ( tile[0] > 0 && tile[0] < 65536 ) s_tileW = ImportTexScaleFromUnits( tile[0] );
        if ( tile[1] > 0 && tile[1] < 65536 ) s_tileH = ImportTexScaleFromUnits( tile[1] );
    }
    ImGui::SameLine();
    ImGui::TextDisabled( "(?)" );
    if ( ImGui::IsItemHovered() )
    {
        char sampleSize[32];
        ImGui::SetTooltip( "How much WORLD SPACE one copy of the texture covers.\n"
                           "Every wall in the shipped maps repeats every 128 units.\n\n"
                           "Stored as autoTexScaleWidth/Height = units / sample size\n"
                           "(%s here), which the texture browser also uses as the\n"
                           "thumbnail size.  A fresh face's texdef is seeded with\n"
                           "autoTexScale * sample size, i.e. with this number.",
                           KiwiFmt_Num( sampleSize, sizeof( sampleSize ),
                                        ImportSampleSize() ) );
    }
    {
        const bool haveSrc = s_srcOk[SLOT_COLOR];
        const int  srcW    = haveSrc ? ( s_src[SLOT_COLOR].isPowerOfTwo ? s_src[SLOT_COLOR].width
                                                                       : s_src[SLOT_COLOR].potWidth )
                                     : 0;
        const int  srcH    = haveSrc ? ( s_src[SLOT_COLOR].isPowerOfTwo ? s_src[SLOT_COLOR].height
                                                                       : s_src[SLOT_COLOR].potHeight )
                                     : 0;
        const int  units   = ImportUnitsFromTexScale( s_tileW );
        ImGui::TextDisabled( "= autoTexScale %dx%d", s_tileW, s_tileH );
        if ( haveSrc && units > 0 )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "   %.1f texels/unit (shipped walls: 4.0)",
                                 (double)srcW / (double)units );
            if ( s_tileW != srcW || s_tileH != srcH )
            {
                ImGui::SameLine();
                ImGui::TextDisabled( "   source %dx%d", srcW, srcH );
            }
        }
    }

    ImGui::Checkbox( "Compress (DXT1 without alpha, DXT5 with)", &s_compress );
    ImGui::SameLine();
    ImGui::TextDisabled( "(?)" );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "On: about 1/8 (DXT1) or 1/4 (DXT5) of the uncompressed size,\n"
                           "and what nearly every shipped .iwi uses.\n"
                           "Off: lossless 32-bit ARGB.\n\n"
                           "Color map only. Every shipped normal map (1292 of 1293) and\n"
                           "every shipped specular map (1437 of 1437) is DXT5, so those\n"
                           "two slots are always written as DXT5." );
    bool allPot = true;
    for ( int i = 0; i < SLOT_COUNT; ++i )
        if ( plan.active[i] && s_srcOk[i] && !s_src[i].isPowerOfTwo )
            allPot = false;
    ImGui::BeginDisabled( allPot );
    ImGui::Checkbox( "Resample to power of two", &s_resampleToPot );
    ImGui::EndDisabled();
    ImGui::Checkbox( "Make current (also retextures the selection)", &s_makeCurrent );

    if ( s_queue.size() > 1 )
        ImGui::Checkbox( "Apply these settings to the remaining materials", &s_applyToRest );

    // Validation and status.
    const bool vocabOk   = VocabularyReady();
    const bool gateOk    = SelectedUsageValue() != 0 && SelectedLocaleMask() != 0
                           && s_tileW > 0 && s_tileH > 0;
    const bool nameOk    = s_name[0] != '\0' && nameFits;
    const bool dupOk     = !dupOnDisk || s_overwrite;
    bool srcOk = true;
    bool potOk = true;
    for ( int i = 0; i < SLOT_COUNT; ++i )
    {
        if ( !plan.active[i] )
            continue;
        if ( !s_srcOk[i] )
            srcOk = false;
        else if ( !s_src[i].isPowerOfTwo && !s_resampleToPot )
            potOk = false;
    }
    const bool canImport = vocabOk && gateOk && nameOk && dupOk && potOk && srcOk
                           && curTemplate != nullptr;

    ImGui::Separator();
    if ( !vocabOk )
        ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ),
                            "usage.txt / locale.txt were not loaded (FillTextureMenu). Without them\n"
                            "the material would be written with usage 0 and stay invisible." );
    else if ( !gateOk )
        ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ),
                            "Usage, locale and tiling must all be non-zero (texwnd.cpp:455)." );
    else if ( !srcOk )
        ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ),
                            "One of the maps this material would use cannot be read - clear it "
                            "or pick another file." );
    else if ( s_status[0] )
        ImGui::TextColored( s_statusBad ? ImVec4( 1.0f, 0.45f, 0.35f, 1.0f )
                                        : ImVec4( 0.75f, 0.75f, 0.75f, 1.0f ),
                            "%s", s_status );

    ImGui::Separator();
    ImGui::BeginDisabled( !canImport );
    if ( ImGui::Button( "Import", ImVec2( 120.0f, 0.0f ) ) )
    {
        char err[512];
        err[0] = '\0';
        const bool ok = PerformImport( err, sizeof( err ) );
        if ( !ok )
        {
            Sys_Printf( "Import FAILED for '%s': %s\n", src.c_str(), err );
            SetStatus( true, "FAILED: %s", err );
        }
        else
        {
            const bool batch = s_applyToRest;
            PopFront();
            if ( s_queue.empty() )
            {
                CloseWizard();
                ImGui::EndDisabled();
                ImGui::EndPopup();
                return;
            }
            if ( batch )
            {
                s_batch = true;
                ReleaseAllPreviews();
                s_popupOpen = false;
                BeginFile( true );
                ImGui::CloseCurrentPopup();
                ImGui::EndDisabled();
                ImGui::EndPopup();
                return;
            }
            BeginFile( true );
        }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if ( ImGui::Button( "Skip", ImVec2( 120.0f, 0.0f ) ) )
    {
        Sys_Printf( "Import: skipped '%s'.\n", src.c_str() );
        PopFront();
        if ( s_queue.empty() )
            CloseWizard();
        else
            BeginFile( true );
    }
    ImGui::SameLine();
    if ( ImGui::Button( "Cancel all", ImVec2( 120.0f, 0.0f ) ) )
    {
        Sys_Printf( "Import: cancelled (%d material%s not imported).\n",
                    (int)s_queue.size(), s_queue.size() == 1 ? "" : "s" );
        CloseWizard();
    }

    ImGui::EndPopup();
}

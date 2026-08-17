// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_import.cpp — KIWI-UX (ROUND BE): the drop plumbing, the wizard, the orchestration.
// ═════════════════════════════════════════════════════════════════════════════════════
// Read kiwi_import.h first (D-BE-J..M: why the drop is Win32, what the drop target is in
// v1, how the queue and "apply to the rest" work, and why nothing counts as imported until
// the engine has read it back).  The two file formats are derived in kiwi_iwi.h (D-BE-A..D)
// and kiwi_matwriter.h (D-BE-E..I).
// ═════════════════════════════════════════════════════════════════════════════════════
#include "stdafx.h"
#include "qe3.h"

#include <windows.h>
#include <shellapi.h>               // DragQueryFileA / DragFinish (shell32.lib is already linked)
#include <commdlg.h>                // GetOpenFileNameA (comdlg32.lib is already linked)
#include <d3d9.h>
#include <imgui/imgui.h>

#include "kiwi_command.h"
#include "kiwi_import.h"
#include "kiwi_iwi.h"
#include "kiwi_matwriter.h"

#include <gfx_d3d/r_image.h>        // Image_FindExisting / Image_Reload (the overwrite refresh)
#include <gfx_d3d/r_gfx.h>          // GfxImage

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

// ── ported / cross-file entry points (each verified against its definition) ─────────
extern int  Sys_Printf( const char *fmt, ... );                                          // win_qe3.cpp:112       int Sys_Printf(const char*,...)
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1340   bool Radiant_RegisterCommand(const char*,byte,byte,int)
extern int  g_nUpdateBits;                                                               // engine_stubs.cpp:773  int g_nUpdateBits
// texwnd.cpp — the two ADDITIVE accessors this round adds beside Load_Materials, both in
// the TU-local texwnd_s pattern (texwnd.cpp:94).  Declared here until U-GLOBALS gives the
// texture browser a header.
extern qtexture_s *TexWnd_RegisterMaterialByName( const char *name );                    // texwnd.cpp:599        qtexture_s *TexWnd_RegisterMaterialByName(const char*)
extern bool        TexWnd_MakeMaterialCurrentByName( const char *name );                 // texwnd.cpp:790        bool TexWnd_MakeMaterialCurrentByName(const char*)
// texwnd.cpp:105 / :106 — the loaded usage/locale filter counts (texwnd_s is TU-local).
extern int  texWndGlob_textureOffset_usageCount();                                       // texwnd.cpp:105        int texWndGlob_textureOffset_usageCount()
extern int  texWndGlob_textureOffset_localeCount();                                      // texwnd.cpp:106        int texWndGlob_textureOffset_localeCount()

// The three filter vocabularies.  Same struct + same extern spelling texwnd.cpp:666-672
// uses; filter_usage_array / filter_locale_array are defined in engine_stubs.cpp:821-822
// and filled from deffiles/materials/{usage,locale}.txt by FillTextureMenu (texwnd.cpp:1802),
// filter_surfacetype_array is the compiled-in table at texwnd.cpp:1723.
struct RadiantFilterEntry { char *name; int index; };
static_assert( sizeof( RadiantFilterEntry ) == 8, "filter_material_t must be 8 bytes (IDB)" );
extern RadiantFilterEntry filter_usage_array[256];        // engine_stubs.cpp:821  (IDB 0x739F80)
extern RadiantFilterEntry filter_locale_array[256];       // engine_stubs.cpp:822  (IDB 0x73A780)
extern RadiantFilterEntry filter_surfacetype_array[29];   // texwnd.cpp:1723       (IDB 0x73AF80)

// ═════════════════════════════════════════════════════════════════════════════════════
//  STATE
// ═════════════════════════════════════════════════════════════════════════════════════
namespace
{

// ── the pending queue (D-BE-L) ─────────────────────────────────────────────────────
std::vector<std::string> s_queue;      // absolute source paths, front == the one being edited
bool                     s_openRequest = false;   // an ImGui::OpenPopup is owed
bool                     s_popupOpen   = false;

// ── per-file wizard state ──────────────────────────────────────────────────────────
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
// the cached duplicate probe (see the wizard's name field)
char  s_dupCheckedName[64] = { 0 };
bool  s_dupOnDisk          = false;

// ── the probed source, refreshed whenever the queue front changes ──────────────────
kiwiIwiSource_t s_src        = {};
bool            s_srcOk      = false;
char            s_srcErr[256] = { 0 };
char            s_status[512] = { 0 };
bool            s_statusBad  = false;

// ── preview (MANAGED, so no device-reset hook is needed — kiwi_iwi.cpp) ────────────
IDirect3DTexture9 *s_preview     = nullptr;
std::string        s_previewPath;

// ── "apply to the rest" carry-over ─────────────────────────────────────────────────
bool s_batch = false;

void ReleasePreview()
{
    if ( s_preview )
        s_preview->Release();
    s_preview = nullptr;
    s_previewPath.clear();
}

// ── name derivation (D-BE-L) ───────────────────────────────────────────────────────
void DeriveName( const char *path, char *out, size_t outSz )
{
    out[0] = '\0';
    if ( !path || !path[0] )
        return;
    const char *base = path;
    for ( const char *c = path; *c; ++c )
        if ( *c == '\\' || *c == '/' )
            base = c + 1;

    char stem[256];
    _snprintf( stem, sizeof( stem ), "%s", base );
    stem[sizeof( stem ) - 1] = '\0';
    char *dot = strrchr( stem, '.' );
    if ( dot )
        *dot = '\0';

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
    // 50 is the writer's ceiling (kiwi_matwriter.cpp: the engine's 64-byte path buffers).
    if ( strlen( out ) > 50 )
        out[50] = '\0';
}

// ── vocabulary helpers ─────────────────────────────────────────────────────────────
// Rows with a null name are the "<separator>" entries TexFilter_LoadMenuFile writes
// (qe3.cpp:113-117); they are not selectable values.
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

// Both filter tables are loaded by FillTextureMenu at boot (radiant_main.cpp:605).  If that
// somehow did not run, the counts are 0 and every dropdown would be empty — which would let
// the wizard write usage == 0 and produce an invisible material.  Report it instead.
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
    // TexWnd_FilterAccept (texwnd.cpp:906) tests `(1 << filter_locale_array[row].index) & tex->locale`,
    // so the material's locale field is a BITMASK over locale indices, not an index.
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
    // Row 0 is the wizard's "(from template)" sentinel — filter_surfacetype_array[0] is
    // {"all", 0}, which as a WRITE value would mean "clear the surface type", not "any".
    if ( s_surfaceRow <= 0 || s_surfaceRow >= 29 )
        return -1;
    return filter_surfacetype_array[s_surfaceRow].index;
}

// ── queue management ───────────────────────────────────────────────────────────────
void SetStatus( bool bad, const char *fmt, ... )
{
    va_list ap;
    va_start( ap, fmt );
    _vsnprintf( s_status, sizeof( s_status ) - 1, fmt, ap );
    va_end( ap );
    s_status[sizeof( s_status ) - 1] = '\0';
    s_statusBad = bad;
}

// (Re)seed the per-file fields from the queue front.  `keepSettings` is the batch carry-over:
// only the NAME and the tiling defaults are re-derived (D-BE-L).
void BeginFile( bool keepSettings )
{
    s_srcOk      = false;
    s_srcErr[0]  = '\0';
    s_overwrite  = false;
    s_dupCheckedName[0] = '\1';   // force the duplicate probe to re-run for the new name
    s_dupOnDisk         = false;
    ReleasePreview();
    if ( s_queue.empty() )
        return;

    const std::string &path = s_queue.front();
    DeriveName( path.c_str(), s_name, sizeof( s_name ) );
    s_srcOk = KiwiIwi_Probe( path.c_str(), &s_src, s_srcErr, sizeof( s_srcErr ) );

    if ( s_srcOk )
    {
        const int w = s_src.isPowerOfTwo ? s_src.width  : s_src.potWidth;
        const int h = s_src.isPowerOfTwo ? s_src.height : s_src.potHeight;
        s_tileW = w;
        s_tileH = h;
    }

    if ( !keepSettings )
    {
        s_templateIndex = 0;
        // Defaults chosen by NAME so they track whatever usage.txt / locale.txt this data
        // tree ships, with a first-real-row fallback when the name is absent.
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

    if ( s_srcOk && !s_src.isPowerOfTwo )
        SetStatus( false, "%dx%d is not a power of two - it will be resampled to %dx%d.",
                   s_src.width, s_src.height, s_src.potWidth, s_src.potHeight );
    else
        s_status[0] = '\0';
}

void PopFront()
{
    if ( !s_queue.empty() )
        s_queue.erase( s_queue.begin() );
}

void CloseWizard()
{
    ReleasePreview();
    s_queue.clear();
    s_batch     = false;
    s_popupOpen = false;
    ImGui::CloseCurrentPopup();
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  THE IMPORT ITSELF (D-BE-M)
// ═════════════════════════════════════════════════════════════════════════════════════
bool PerformImport( const char *srcPath, char *err, size_t errSz )
{
    char imgQPath[128];
    _snprintf( imgQPath, sizeof( imgQPath ), "images/%s.iwi", s_name );
    imgQPath[sizeof( imgQPath ) - 1] = '\0';

    const bool overwriting = KiwiMat_ExistsOnDisk( s_name ) || KiwiIwi_ExistsOnDisk( s_name );

    // ROLLBACK POLICY.  On a fresh import, any failure deletes whatever we managed to write
    // (D-BE-M).  On an OVERWRITE it must NOT: the previous files are already gone, and
    // deleting the half-written replacement would turn a bad import into a missing asset
    // that every map referencing that name would then load as $default.  So an overwrite
    // failure leaves the partial file in place and says so, loudly.
    const bool rollback = !overwriting;

    // 1) the .iwi
    kiwiIwiOptions_t opt = {};
    opt.compress      = s_compress;
    opt.resampleToPot = s_resampleToPot;
    kiwiIwiResult_t res = {};
    if ( !KiwiIwi_WriteFromFile( srcPath, imgQPath, &opt, &res, err, errSz ) )
        return false;

    // 2) STAGE 1 of the round trip: the engine's own header functions over our own file.
    if ( !KiwiIwi_VerifyOnDisk( imgQPath, err, errSz ) )
    {
        if ( rollback )
            KiwiMat_DeleteWritten( nullptr, s_name );
        else
            Sys_Printf( "Import: '%s' was OVERWRITTEN and the replacement is bad - it was left in place.\n", s_name );
        return false;
    }

    // An overwrite has to invalidate the engine's cached copy of the image, or the browser
    // and the 3D view keep drawing the old pixels until a restart.  Image_Reload
    // (r_image.cpp:1301) is the editor's own F5 path per image.
    if ( overwriting )
    {
        if ( GfxImage *old = Image_FindExisting( s_name ) )
            Image_Reload( old );
    }

    // 3) the material
    kiwiMatFields_t f = {};
    _snprintf( f.name, sizeof( f.name ), "%s", s_name );
    _snprintf( f.imageName, sizeof( f.imageName ), "%s", s_name );
    f.name[sizeof( f.name ) - 1]           = '\0';
    f.imageName[sizeof( f.imageName ) - 1] = '\0';
    f.usage              = (unsigned char)SelectedUsageValue();
    f.locale             = SelectedLocaleMask();
    f.autoTexScaleWidth  = (unsigned short)( s_tileW > 0 ? s_tileW : res.width );
    f.autoTexScaleHeight = (unsigned short)( s_tileH > 0 ? s_tileH : res.height );
    f.surfaceType        = SelectedSurfaceType();

    if ( !KiwiMat_Write( s_templateIndex, &f, err, errSz ) )
    {
        if ( rollback )
            KiwiMat_DeleteWritten( nullptr, s_name );
        return false;
    }

    // 4) STAGE 2 + 3 of the round trip: the real engine loader, then the browser gate.
    kiwiMatVerify_t v = {};
    if ( !KiwiMat_Verify( s_name, &v, err, errSz ) )
    {
        if ( rollback )
            KiwiMat_DeleteWritten( s_name, s_name );
        else
            Sys_Printf( "Import: '%s' was OVERWRITTEN and the replacement does not verify - it was left in place.\n", s_name );
        return false;
    }

    // 5) into the browser, now, without a restart.
    qtexture_s *q = TexWnd_RegisterMaterialByName( s_name );
    if ( !q )
    {
        if ( rollback )
            KiwiMat_DeleteWritten( s_name, s_name );
        _snprintf( err, errSz - 1, "the material loaded but the texture browser refused to register it" );
        err[errSz - 1] = '\0';
        return false;
    }
    if ( s_makeCurrent )
        TexWnd_MakeMaterialCurrentByName( s_name );

    static const char *kFormatName[] = { "", "ARGB8888", "", "", "", "", "", "", "", "", "", "DXT1", "DXT3", "DXT5" };
    const char *fmtName = ( res.format >= 0 && res.format < 14 ) ? kFormatName[res.format] : "?";

    Sys_Printf( "Import: '%s' -> materials/%s + images/%s.iwi  [%s %dx%d, %d mips, %d bytes%s]\n",
                srcPath, s_name, s_name, fmtName, res.width, res.height, res.mipCount,
                res.fileSize, res.resampled ? ", resampled to POT" : "" );
    Sys_Printf( "        techset '%s'  sortKey %u  usage %u  locale 0x%X  autoTexScale %ux%u  "
                "surfaceFlags 0x%X  contents 0x%X%s\n",
                v.techSet, (unsigned)v.sortKey, (unsigned)v.usage, (unsigned)v.locale,
                (unsigned)v.autoTexScaleWidth, (unsigned)v.autoTexScaleHeight,
                (unsigned)v.surfaceFlags, (unsigned)v.contents,
                overwriting ? "  (overwrote an existing pair)" : "" );
    Sys_Printf( "        verified through the engine: colorMap '%s' %dx%d, texture %s.\n",
                v.colorMapImage, v.colorMapWidth, v.colorMapHeight,
                v.colorMapUploaded ? "uploaded" : "MISSING" );

    g_nUpdateBits |= W_TEXTURE;
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  QUEUEING
// ═════════════════════════════════════════════════════════════════════════════════════
int QueuePaths( const std::vector<std::string> &paths )
{
    int accepted = 0;
    for ( size_t i = 0; i < paths.size(); ++i )
    {
        if ( !KiwiIwi_IsAcceptedExtension( paths[i].c_str() ) )
        {
            Sys_Printf( "Import: refused '%s' - only %s are supported.\n",
                        paths[i].c_str(), KiwiIwi_AcceptedExtensionList() );
            continue;
        }
        s_queue.push_back( paths[i] );
        ++accepted;
    }
    return accepted;
}

}  // namespace

// ═════════════════════════════════════════════════════════════════════════════════════
//  WM_DROPFILES (D-BE-J)
// ═════════════════════════════════════════════════════════════════════════════════════
bool KiwiImport_HandleDropFiles( void *hDropOpaque )
{
    HDROP hDrop = (HDROP)hDropOpaque;
    if ( !hDrop )
        return false;

    // The HDROP is only valid until DragFinish, so the WHOLE list is copied out here, in
    // the message handler, before anything else happens (D-BE-J).
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

// ═════════════════════════════════════════════════════════════════════════════════════
//  COMMANDS
// ═════════════════════════════════════════════════════════════════════════════════════
void KiwiImport_RegisterCommands()
{
    // Only the BROWSE verb is registered.  KIWI_CMD_IMPORT_DROPPED is an internal
    // continuation of a finished gesture and is never bindable, exactly like round AU's
    // KIWI_CMD_ENT_DROP (kiwi_entbrowser.cpp:1004).
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
        // OFN_ALLOWMULTISELECT + OFN_EXPLORER: on success the buffer is either a single
        // full path, or a directory followed by NUL-separated file names and a double NUL.
        static char buf[16384];
        buf[0] = '\0';

        // A COMMDLG filter is a run of NUL-terminated pairs ended by a second NUL, so it
        // is assembled byte by byte rather than through a formatter.
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

// ═════════════════════════════════════════════════════════════════════════════════════
//  THE WIZARD
// ═════════════════════════════════════════════════════════════════════════════════════
void KiwiImport_Draw()
{
    // BATCH MODE: "apply these settings to the remaining files" turns the rest of the queue
    // into silent imports, one per frame so the console line order matches the queue order
    // and a long batch never blocks the pump for more than one file's work.
    if ( s_batch && !s_queue.empty() )
    {
        const std::string src = s_queue.front();
        char err[512];
        err[0] = '\0';
        if ( !PerformImport( src.c_str(), err, sizeof( err ) ) )
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
        // The popup was dismissed by something other than our buttons (Escape).  Treat it
        // as Cancel All: nothing has been written, so there is nothing to roll back.
        if ( s_popupOpen )
        {
            ReleasePreview();
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

    const std::string src = s_queue.front();

    // ── the source ────────────────────────────────────────────────────────────────
    ImGui::TextDisabled( "Source" );
    ImGui::TextWrapped( "%s", src.c_str() );
    if ( s_queue.size() > 1 )
        ImGui::Text( "%d more file%s in this drop.", (int)s_queue.size() - 1,
                     s_queue.size() == 2 ? "" : "s" );

    if ( !s_srcOk )
    {
        ImGui::Separator();
        ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ), "Cannot read this file: %s", s_srcErr );
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

    // ── preview (lazy, one MANAGED texture at a time) ─────────────────────────────
    if ( s_previewPath != src )
    {
        ReleasePreview();
        s_preview     = KiwiIwi_CreatePreview( src.c_str() );
        s_previewPath = src;
    }

    ImGui::Separator();
    if ( s_preview )
    {
        const float side = 128.0f;
        const float aspect = ( s_src.height > 0 ) ? ( (float)s_src.width / (float)s_src.height ) : 1.0f;
        ImVec2 size = ( aspect >= 1.0f ) ? ImVec2( side, side / aspect ) : ImVec2( side * aspect, side );
        ImGui::Image( (ImTextureID)(intptr_t)s_preview, size );   // imgui_shell.cpp:889's spelling
        ImGui::SameLine();
    }
    ImGui::BeginGroup();
    ImGui::Text( "%d x %d, %s", s_src.width, s_src.height, s_src.hasAlpha ? "has alpha" : "no alpha" );
    if ( !s_src.isPowerOfTwo )
        ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.3f, 1.0f ),
                            "Not a power of two. The engine's mip counter (Image_CountMipmaps)\n"
                            "assumes POT, so this must be resampled to %d x %d.",
                            s_src.potWidth, s_src.potHeight );
    ImGui::EndGroup();

    ImGui::Separator();

    // ── name ──────────────────────────────────────────────────────────────────────
    ImGui::SetNextItemWidth( 320.0f );
    ImGui::InputText( "Material name", s_name, sizeof( s_name ) );
    ImGui::SameLine();
    ImGui::TextDisabled( "(?)" );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Written as materials/<name> and images/<name>.iwi.\n"
                           "Registered in the editor as wc/<name>." );

    // The duplicate probe is two FS_FOpenFileRead calls, each of which walks every searchpath
    // and every .iwd hash — far too much to redo on all 60 frames a second the popup is up.
    // Re-run it only when the typed name actually changes.
    if ( strcmp( s_dupCheckedName, s_name ) != 0 )
    {
        _snprintf( s_dupCheckedName, sizeof( s_dupCheckedName ), "%s", s_name );
        s_dupCheckedName[sizeof( s_dupCheckedName ) - 1] = '\0';
        s_dupOnDisk = KiwiMat_ExistsOnDisk( s_name ) || KiwiIwi_ExistsOnDisk( s_name );
        // The overwrite opt-in is per NAME, not per session: typing a different name that
        // also collides must ask again rather than inherit the previous name's consent.
        s_overwrite = false;
    }
    const bool dupOnDisk = s_dupOnDisk;
    if ( dupOnDisk )
    {
        ImGui::TextColored( ImVec4( 1.0f, 0.8f, 0.3f, 1.0f ), "A material or image with this name already exists." );
        ImGui::Checkbox( "Overwrite the existing files", &s_overwrite );
    }
    else
    {
        s_overwrite = false;
    }

    // ── material type ─────────────────────────────────────────────────────────────
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
                ImGui::SetTooltip( "%s\n\ntechset '%s'\ntemplate: %s", ti->blurb, ti->techSet,
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

    // ── usage / locale / surface type ─────────────────────────────────────────────
    const int usageCount  = texWndGlob_textureOffset_usageCount();
    const int localeCount = texWndGlob_textureOffset_localeCount();

    ImGui::SetNextItemWidth( 320.0f );
    if ( ImGui::BeginCombo( "Usage",
                            ( s_usageRow > 0 && filter_usage_array[s_usageRow].name )
                                ? filter_usage_array[s_usageRow].name : "<none>" ) )
    {
        for ( int i = 1; i < usageCount && i < 256; ++i )
        {
            if ( !filter_usage_array[i].name )   // "<separator>" row (qe3.cpp:113-117)
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
        ImGui::SetTooltip( "The compiled-in surface-type table (texwnd.cpp:1723).\n"
                           "Drives footstep/impact sounds in game and the\n"
                           "Textures > Surface type browser filter." );

    // ── tiling / output ───────────────────────────────────────────────────────────
    ImGui::Separator();
    int tile[2] = { s_tileW, s_tileH };
    ImGui::SetNextItemWidth( 200.0f );
    if ( ImGui::InputInt2( "Tiling (world units per repeat)", tile ) )
    {
        s_tileW = ( tile[0] > 0 && tile[0] < 65536 ) ? tile[0] : s_tileW;
        s_tileH = ( tile[1] > 0 && tile[1] < 65536 ) ? tile[1] : s_tileH;
    }
    ImGui::SameLine();
    ImGui::TextDisabled( "(?)" );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "autoTexScaleWidth/Height. The texture browser uses it as the\n"
                           "thumbnail size and a fresh face's texdef is derived from it,\n"
                           "so the image resolution is the natural default." );

    ImGui::Checkbox( "Compress (DXT1 without alpha, DXT5 with)", &s_compress );
    ImGui::SameLine();
    ImGui::TextDisabled( "(?)" );
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "On: about 1/8 (DXT1) or 1/4 (DXT5) of the uncompressed size,\n"
                           "and what nearly every shipped .iwi uses.\n"
                           "Off: lossless 32-bit ARGB." );
    ImGui::BeginDisabled( s_src.isPowerOfTwo );
    ImGui::Checkbox( "Resample to power of two", &s_resampleToPot );
    ImGui::EndDisabled();
    ImGui::Checkbox( "Make current (also retextures the selection)", &s_makeCurrent );

    if ( s_queue.size() > 1 )
        ImGui::Checkbox( "Apply these settings to the remaining files", &s_applyToRest );

    // ── validation + status ───────────────────────────────────────────────────────
    const bool vocabOk   = VocabularyReady();
    const bool gateOk    = SelectedUsageValue() != 0 && SelectedLocaleMask() != 0
                           && s_tileW > 0 && s_tileH > 0;
    const bool nameOk    = s_name[0] != '\0';
    const bool dupOk     = !dupOnDisk || s_overwrite;
    const bool potOk     = s_src.isPowerOfTwo || s_resampleToPot;
    const bool canImport = vocabOk && gateOk && nameOk && dupOk && potOk && curTemplate != nullptr;

    ImGui::Separator();
    if ( !vocabOk )
        ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ),
                            "usage.txt / locale.txt were not loaded (FillTextureMenu). Without them\n"
                            "the material would be written with usage 0 and stay invisible." );
    else if ( !gateOk )
        ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.35f, 1.0f ),
                            "Usage, locale and tiling must all be non-zero (texwnd.cpp:455)." );
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
        const bool ok = PerformImport( src.c_str(), err, sizeof( err ) );
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
                ReleasePreview();
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
        Sys_Printf( "Import: cancelled (%d file%s not imported).\n",
                    (int)s_queue.size(), s_queue.size() == 1 ? "" : "s" );
        CloseWizard();
    }

    ImGui::EndPopup();
}

// kiwi_modelimport.cpp - the Model Import window.  See kiwi_modelimport.h.
//
// One recipe is open at a time.  The property column is the s_sections table below:
// a later model property is a new Section_* function and one more row, nothing else.
// Everything slow - the Blender FBX load, hull generation, builds, folder scans and
// texture swatches - runs on a kimport::ImportJob worker over COPIES of the recipe
// and the source state, copied when the job STARTS; the UI swaps the results in when it
// finishes, and the recipe stays read-only while a job that hands one back is queued or
// running.

#include "stdafx.h"
#include "qe3.h"

#include <imgui/imgui.h>
#include <commdlg.h>                // GetOpenFileNameA
#include <shlobj.h>                 // SHBrowseForFolderA (folder pickers)

#include <gfx_d3d/r_gfx.h>          // GfxMatrix, GfxColor
#include <gfx_d3d/r_init.h>         // dx
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // R_BeginFrame / R_EndFrame / clear / material colour
#include <gfx_d3d/r_scene.h>        // R_Ed_SetSceneParms / R_Ed_ProjectionWouldBeValid
#include <universal/com_math.h>     // AngleVectors

#include "modelimport/ki_build.h"
#include "modelimport/ki_job.h"
#include "modelimport/ki_pick.h"

#include "kiwi_command.h"
#include "kiwi_hints.h"             // KiwiHints_DrawKeys: the view's keycap strip
#include "kiwi_lines.h"
#include "kiwi_modelbrowser.h"      // KiwiModelBrowser_RequestRefresh after a build
#include "kiwi_modelimport.h"
#include "kiwi_windows.h"
#include "radiant_frame.h"          // Radiant_RegisterCommand
#include "radiant_rtt.h"            // RTT_BeginPreview / RTT_EndPreview / RTT_PreviewTexture

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

extern int  Sys_Printf( const char *fmt, ... );
extern char Byte4PackPixelColor( float *from, GfxColor *out );                     // 0x402ac0
extern void R_SortMaterials();                                                     // r_ed_scene.cpp:2118
extern int  __cdecl R_Ed_CmdBufferHeadroom();                                      // r_rendercmds.cpp
extern void __cdecl R_AddRenderCmdDrawTris(
    Material *material, MaterialTechniqueType techType, short indexCount,
    const uint16_t *indices, short vertexCount,
    const float ( *xyzw )[4], const float ( *normal )[3], float *color,
    const float ( *st )[2] );

namespace ki = kimport;
namespace fs = std::filesystem;

namespace
{
    // ── preview ──────────────────────────────────────────────────────────────────
    const float KMI_FOV_Y     = 50.0f;            // vertical field of view, degrees
    const float KMI_GUARD     = 0.99951171875f;   // the camera's guard-band constant (CamWnd_SetupScene)
    const float KMI_BG[4]     = { 0.13f, 0.14f, 0.17f, 1.0f };
    const float KMI_ORBIT_DEG = 0.4f;             // degrees per dragged pixel
    const float KMI_ZOOM_STEP = 0.85f;            // distance factor per wheel notch
    const float KMI_GHOST_A   = 0.30f;            // "Ghost" model alpha
    const float KMI_HULL_A    = 0.20f;            // hull face fill alpha
    const float KMI_DEG2RAD   = 0.017453292f;
    // One R_AddRenderCmdDrawTris command must stay under 64 KB: GfxCmdHeader::byteCount
    // is a uint16_t that R_GetCommandBuffer stores unchecked, and the backend walks the
    // list by it, so a bigger command corrupts the stream.  The backend tess also caps a
    // draw at 5450 vertices (RB_CheckTessOverflow).  Each triangle is 3 vertices of
    // xyzw/normal/colour/st (40 bytes) plus both windings' 6 indices.
    enum { KMI_BATCH_TRIS = 480 };
    const int KMI_TRI_BYTES   = 3 * ( 16 + 12 + 4 + 8 ) + 6 * 2;
    static_assert( 16 + KMI_BATCH_TRIS * KMI_TRI_BYTES < 0x10000, "a DrawTris command must fit its 16-bit byteCount" );
    static_assert( KMI_BATCH_TRIS * 3 <= 5450, "a DrawTris command must fit the backend tess" );
    const int KMI_CMD_RESERVE = 1 << 20;          // command buffer left for the rest of the frame
    enum { KMI_LOG_MAX = 500 };
    enum { KMI_SWATCH = 64 };                     // texture swatch size, pixels

    // One colour per CoD4 surface type (cod4::SurfaceTypeName order), for the preview
    // and the swatches beside every surface picker.
    const float KMI_SURFACE_RGB[cod4::kSurfaceTypeCount][3] =
    {
        { 0.60f, 0.60f, 0.60f },   // default
        { 0.45f, 0.30f, 0.18f },   // bark
        { 0.70f, 0.32f, 0.25f },   // brick
        { 0.55f, 0.25f, 0.45f },   // carpet
        { 0.75f, 0.55f, 0.75f },   // cloth
        { 0.62f, 0.62f, 0.58f },   // concrete
        { 0.50f, 0.38f, 0.25f },   // dirt
        { 0.90f, 0.65f, 0.55f },   // flesh
        { 0.30f, 0.62f, 0.25f },   // foliage
        { 0.55f, 0.80f, 0.95f },   // glass
        { 0.40f, 0.75f, 0.30f },   // grass
        { 0.55f, 0.52f, 0.48f },   // gravel
        { 0.80f, 0.92f, 1.00f },   // ice
        { 0.55f, 0.62f, 0.72f },   // metal
        { 0.40f, 0.30f, 0.20f },   // mud
        { 0.95f, 0.93f, 0.85f },   // paper
        { 0.88f, 0.85f, 0.78f },   // plaster
        { 0.48f, 0.47f, 0.45f },   // rock
        { 0.88f, 0.80f, 0.55f },   // sand
        { 0.97f, 0.97f, 1.00f },   // snow
        { 0.25f, 0.45f, 0.85f },   // water
        { 0.70f, 0.50f, 0.28f },   // wood
        { 0.28f, 0.28f, 0.30f },   // asphalt
        { 0.92f, 0.88f, 0.80f },   // ceramic
        { 0.90f, 0.80f, 0.25f },   // plastic
        { 0.18f, 0.18f, 0.20f },   // rubber
        { 0.70f, 0.40f, 0.35f },   // cushion
        { 0.90f, 0.35f, 0.20f },   // fruit
        { 0.35f, 0.55f, 0.75f },   // paintedmetal
    };
    const float KMI_SELECT_RGB[3] = { 1.00f, 0.55f, 0.10f };
    const float KMI_HULL_RGB[3]   = { 0.20f, 0.85f, 1.00f };
    const float KMI_HAND_RGB[3]   = { 0.95f, 0.35f, 0.85f };   // hulls read from a hand-edited collmap

    // ── jobs ─────────────────────────────────────────────────────────────────────
    enum jobKind_t
    {
        JOB_LOAD,           // open a recipe file
        JOB_RELOAD,         // re-read the source (units / scale / origin / winding changed, or re-exported)
        JOB_HULLS,          // the hulls for the current settings (preview only, nothing written)
        JOB_SAVE,           // write the recipe (+ the master collmap in automatic mode)
        JOB_RESET_HULLS,    // hand-edited -> automatic: regenerate the master and save
        JOB_BUILD,          // save, then build every enabled variant
        JOB_CREATE,         // a new recipe from a source file
        JOB_SCAN,           // list the sources in a folder
    };

    struct job_t
    {
        jobKind_t            kind = JOB_LOAD;
        bool                 locks = false;       // hands a recipe back: read-only while queued or running
        bool                 fromWindow = false;  // works on the open recipe: copied when it STARTS
        ki::GameRoot         root;
        fs::path             recipePath;
        ki::Recipe           recipe;
        ki::SourceState      state;
        fs::path             source;         // JOB_CREATE: the model file; JOB_SCAN: the folder
        ki::NewRecipeOptions options;        // JOB_CREATE
        double               sinceHours = 0.0;
        // results
        std::vector<ki::Hull>        hulls;
        ki::TextureScan              scan;
        std::string                  setName;
        std::string                  textureError;
        ki::BuildReport              report;
        std::vector<ki::SourceEntry> sources;
        std::vector<char>            sourceHandMade;   // JOB_SCAN: per source, a hand-made collmap exists
        bool                         piecesChanged = false;   // LOAD / RELOAD: the piece table moved
    };

    struct swatchJob_t
    {
        std::vector<std::string>                paths;
        std::vector<std::vector<unsigned char>> rgba;
        std::vector<int>                        w, h;
    };

    struct swatch_t
    {
        IDirect3DTexture9 *tex    = nullptr;
        bool               failed = false;
    };

    // ── window state ─────────────────────────────────────────────────────────────
    bool                          s_init = false;
    ki::GameRoot                  s_root;
    std::vector<std::string>      s_recipeNames;
    bool                          s_listStale = true;

    bool                          s_open = false;       // a recipe is loaded
    ki::Recipe                    s_recipe;             // the edited copy
    fs::path                      s_recipePath;
    bool                          s_dirty = false;      // edits not yet saved
    ki::SourceState               s_state;
    bool                          s_haveMesh = false;
    long long                     s_srcSize = -1;       // the source file as loaded, for "changed on disk"
    long long                     s_srcStamp = 0;
    bool                          s_srcChanged = false;
    std::vector<ki::Hull>         s_hulls;
    std::vector<std::vector<std::vector<std::array<double, 3>>>> s_windings;   // per hull
    double                        s_hullVolume = 0.0;
    ki::TextureScan               s_scan;
    std::string                   s_setName;
    std::string                   s_textureError;
    std::string                   s_lastError;
    ki::BuildReport               s_lastBuild;
    bool                          s_haveBuild = false;
    bool                          s_masterHandMade = false;   // collmaps\<asset>.map lacks the marker
    int                           s_previewTris = 0;    // triangles the last preview drew

    std::set<int>                 s_sel;                // selected pieces
    int                           s_hover = -1;         // piece under the mouse in the view
    int                           s_scrollTo = -1;      // piece list row to bring into view once

    // Both job runners are leaked on purpose: ~ImportJob joins its worker, and a Blender
    // FBX load can take minutes, so quitting Radiant mid-import must not wait for it.
    // Process exit ends the thread; a worker touches only its own heap job data.
    ki::ImportJob &Job()
    {
        static ki::ImportJob *job = new ki::ImportJob();
        return *job;
    }
    ki::ImportJob &SwatchJob()
    {
        static ki::ImportJob *job = new ki::ImportJob();
        return *job;
    }

    std::shared_ptr<job_t>        s_jobData;            // the running job
    std::deque<std::shared_ptr<job_t>> s_queue;
    bool                          s_hullsAgain = false; // settings changed while hulls were computing

    std::shared_ptr<swatchJob_t>  s_swatchData;
    bool                          s_swatchAgain = false;
    std::map<std::string, swatch_t> s_swatches;         // by base-colour path

    std::deque<std::string>       s_log;
    bool                          s_logScroll = false;

    // preview camera + view
    float s_yaw       = 225.0f;
    float s_pitch     = 22.0f;                          // positive looks down
    float s_dist      = 128.0f;
    float s_target[3] = { 0.0f, 0.0f, 0.0f };
    int   s_viewW     = 0;
    int   s_viewH     = 0;
    int   s_viewFrame = -100;                           // ImGui frame that last showed the view
    bool  s_viewDirty = true;
    int   s_rtW       = 0;
    int   s_rtH       = 0;
    int   s_modelMode = 0;                              // 0 solid, 1 ghost, 2 hidden
    bool  s_showHulls = true;
    bool  s_showGrid  = true;
    bool  s_lmbPress  = false;
    bool  s_lmbDrag   = false;
    float s_propWidth = 430.0f;
    float s_logHeight = 120.0f;

    // popups
    bool        s_reqNew = false, s_reqScan = false, s_reqDiscard = false;
    bool        s_reqDelete = false, s_reqLeave = false, s_reqHand = false;
    std::function<void()> s_pendingAction;              // runs once "discard changes?" is answered
    std::string s_leaveTo;                              // "auto" / "none": leaving Hand-edited
    jobKind_t   s_handKind = JOB_SAVE;                  // save / build waiting on "hand-made collision?"
    std::function<void()> s_handThen;
    std::string s_newSource, s_newAsset, s_newFolder, s_newPrefix;
    std::string s_newSurface = "concrete";
    std::string s_newUnits = "in";
    bool        s_newReplace = false;
    std::string s_scanFolder;
    float       s_scanHours = 0.0f;
    std::vector<ki::SourceEntry> s_sources;
    std::vector<char>            s_sourceHandMade;      // per source: importing would replace hand-made collision
    bool        s_haveScan = false;

    // ── small helpers ────────────────────────────────────────────────────────────
    void Log( const std::string &line )
    {
        s_log.push_back( line );
        while ( s_log.size() > KMI_LOG_MAX )
            s_log.pop_front();
        s_logScroll = true;
    }

    bool SameText( const std::string &a, const std::string &b )
    {
        return _stricmp( a.c_str(), b.c_str() ) == 0;
    }

    // ImGui text field over a std::string (imgui_stdlib is not compiled into Radiant).
    bool InputString( const char *label, std::string &value, ImGuiInputTextFlags flags = 0 )
    {
        char buf[1024];
        _snprintf( buf, sizeof( buf ), "%s", value.c_str() );
        buf[sizeof( buf ) - 1] = '\0';
        if ( !ImGui::InputText( label, buf, sizeof( buf ), flags ) )
            return false;
        value = buf;
        return true;
    }

    bool StringCombo( const char *id, std::string &value, const char *const *values,
                      const char *const *labels, int count )
    {
        int cur = -1;
        for ( int i = 0; i < count; ++i )
            if ( SameText( value, values[i] ) )
                cur = i;
        bool changed = false;
        if ( ImGui::BeginCombo( id, cur >= 0 ? labels[cur] : value.c_str() ) )
        {
            for ( int i = 0; i < count; ++i )
                if ( ImGui::Selectable( labels[i], i == cur ) && i != cur )
                {
                    value = values[i];
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    }

    int SurfaceIndex( const std::string &name )
    {
        int index = 0;
        return ( !name.empty() && cod4::ParseSurfaceType( name, index ) ) ? index : 0;
    }

    ImVec4 SurfaceColor( int index )
    {
        if ( index < 0 || index >= cod4::kSurfaceTypeCount )
            index = 0;
        const float *c = KMI_SURFACE_RGB[index];
        return ImVec4( c[0], c[1], c[2], 1.0f );
    }

    void SurfaceDot( int index )
    {
        const float s = ImGui::GetFrameHeight() * 0.6f;
        ImGui::ColorButton( "##surfdot", SurfaceColor( index ),
                            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop |
                            ImGuiColorEditFlags_NoBorder, ImVec2( s, s ) );
    }

    // A surface picker.  `inherit` non-null adds an "inherit the default" entry (value "").
    bool SurfaceCombo( const char *id, std::string &value, const char *inherit )
    {
        const int cur = SurfaceIndex( value );
        const char *preview = value.empty() && inherit ? inherit : cod4::SurfaceTypeName( cur );
        bool changed = false;
        if ( ImGui::BeginCombo( id, preview ) )
        {
            if ( inherit && ImGui::Selectable( inherit, value.empty() ) && !value.empty() )
            {
                value.clear();
                changed = true;
            }
            for ( int i = 1; i < cod4::kSurfaceTypeCount; ++i )
            {
                ImGui::PushID( i );
                SurfaceDot( i );
                ImGui::SameLine();
                const bool selected = !value.empty() && cur == i;
                if ( ImGui::Selectable( cod4::SurfaceTypeName( i ), selected ) && !selected )
                {
                    value = cod4::SurfaceTypeName( i );
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    int PieceSurface( int piece )
    {
        if ( piece >= 0 && piece < (int)s_recipe.pieces.size() && s_recipe.pieces[piece].index == piece &&
             !s_recipe.pieces[piece].surface.empty() )
            return SurfaceIndex( s_recipe.pieces[piece].surface );
        return SurfaceIndex( s_recipe.surface );
    }

    const std::vector<ki::TextureVariant> *FoundVariants()
    {
        const auto it = s_scan.sets.find( s_setName );
        return it == s_scan.sets.end() ? nullptr : &it->second;
    }

    void ScanTextures( const ki::Recipe &recipe, ki::TextureScan &scan, std::string &setName,
                       std::string &error )
    {
        scan = ki::TextureScan();
        setName.clear();
        error.clear();
        const std::vector<ki::TextureVariant> *variants = nullptr;
        if ( !ki::ResolveTextures( recipe, scan, variants, setName, error ) )
            setName.clear();
    }

    bool StatFile( const fs::path &path, long long &size, long long &stamp )
    {
        std::error_code ec;
        const auto bytes = fs::file_size( path, ec );
        if ( ec )
            return false;
        const auto time = fs::last_write_time( path, ec );
        if ( ec )
            return false;
        size  = (long long)bytes;
        stamp = (long long)time.time_since_epoch().count();
        return true;
    }

    void RefreshRecipeList()
    {
        s_listStale = false;
        s_recipeNames.clear();
        std::error_code ec;
        for ( fs::directory_iterator it( s_root.ModelSource(), ec ), end; !ec && it != end; it.increment( ec ) )
            if ( SameText( it->path().extension().string(), ".kimport" ) )
                s_recipeNames.push_back( it->path().stem().string() );
        std::sort( s_recipeNames.begin(), s_recipeNames.end(),
                   []( const std::string &a, const std::string &b ) { return _stricmp( a.c_str(), b.c_str() ) < 0; } );
    }

    void Init()
    {
        if ( s_init )
            return;
        s_init = true;
        s_root = ki::DefaultGameRoot();
        s_scanFolder = ki::GetImportSetting( s_root, "sourceFolder" );
        const std::string surface = ki::GetImportSetting( s_root, "defaultSurface" );
        if ( SurfaceIndex( surface ) > 0 )
            s_newSurface = surface;
        const std::string units = ki::GetImportSetting( s_root, "defaultUnits" );
        if ( ki::UnitsFactor( units ) > 0.0 )
            s_newUnits = units;
    }

    void RememberSetting( const char *key, const std::string &value )
    {
        std::string err;
        if ( !ki::SetImportSetting( s_root, key, value, err ) )
            Log( "Warning: " + err );
    }

    // ── native pickers ───────────────────────────────────────────────────────────
    int CALLBACK BrowseFolderInit( HWND hwnd, UINT msg, LPARAM, LPARAM data )
    {
        if ( msg == BFFM_INITIALIZED && data )
            ::SendMessageA( hwnd, BFFM_SETSELECTIONA, TRUE, data );
        return 0;
    }

    bool BrowseFolder( const char *title, std::string &folder )
    {
        const HRESULT com = ::CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED );
        BROWSEINFOA bi = {};
        bi.hwndOwner = g_qeglobals.d_hwndMain;
        bi.lpszTitle = title;
        bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        bi.lpfn      = BrowseFolderInit;
        bi.lParam    = (LPARAM)( folder.empty() ? nullptr : folder.c_str() );
        bool ok = false;
        if ( PIDLIST_ABSOLUTE pidl = ::SHBrowseForFolderA( &bi ) )
        {
            char path[MAX_PATH] = {};
            if ( ::SHGetPathFromIDListA( pidl, path ) && path[0] )
            {
                folder = path;
                ok = true;
            }
            ::CoTaskMemFree( pidl );
        }
        if ( SUCCEEDED( com ) )
            ::CoUninitialize();
        return ok;
    }

    bool BrowseModelFile( std::string &path, const std::string &initialDir )
    {
        char file[MAX_PATH] = {};
        OPENFILENAMEA ofn = {};
        ofn.lStructSize     = sizeof( ofn );
        ofn.hwndOwner       = g_qeglobals.d_hwndMain;
        ofn.lpstrFilter     = "3D models (*.fbx;*.obj)\0*.fbx;*.obj\0All files (*.*)\0*.*\0\0";
        ofn.lpstrTitle      = "Import a model";
        ofn.lpstrFile       = file;
        ofn.nMaxFile        = MAX_PATH;
        ofn.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
        ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
        if ( !::GetOpenFileNameA( &ofn ) || !file[0] )
            return false;
        path = file;
        return true;
    }

    // ── swatches ─────────────────────────────────────────────────────────────────
    IDirect3DTexture9 *CreateSwatch( const std::vector<unsigned char> &rgba, int w, int h )
    {
        if ( !dx.device || w <= 0 || h <= 0 || rgba.size() < (size_t)w * h * 4 )
            return nullptr;
        IDirect3DTexture9 *tex = nullptr;
        if ( dx.device->CreateTexture( (UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                       &tex, nullptr ) < 0 || !tex )
            return nullptr;
        D3DLOCKED_RECT lr = {};
        if ( tex->LockRect( 0, &lr, nullptr, 0 ) < 0 )
        {
            tex->Release();
            return nullptr;
        }
        for ( int y = 0; y < h; ++y )
        {
            unsigned *dst = (unsigned *)( (unsigned char *)lr.pBits + (size_t)y * lr.Pitch );
            const unsigned char *src = &rgba[(size_t)y * w * 4];
            for ( int x = 0; x < w; ++x )   // opaque: the swatch shows colour, not the opacity map
                dst[x] = 0xFF000000u | ( (unsigned)src[x * 4] << 16 ) | ( (unsigned)src[x * 4 + 1] << 8 ) |
                         (unsigned)src[x * 4 + 2];
        }
        tex->UnlockRect( 0 );
        return tex;
    }

    // Safe only before this frame's ImGui::Image calls (PollJobs runs first in Draw).
    void ReleaseSwatches()
    {
        for ( auto &entry : s_swatches )
            if ( entry.second.tex )
                entry.second.tex->Release();
        s_swatches.clear();
    }

    void RequestSwatches()
    {
        if ( SwatchJob().Running() )
        {
            s_swatchAgain = true;
            return;
        }
        const std::vector<ki::TextureVariant> *found = FoundVariants();
        if ( !found )
            return;
        std::shared_ptr<swatchJob_t> data = std::make_shared<swatchJob_t>();
        for ( const ki::TextureVariant &v : *found )
        {
            const std::string path = v.files.baseColor.string();
            if ( !path.empty() && !s_swatches.count( path ) )
            {
                s_swatches[path] = swatch_t();      // pending: asked once
                data->paths.push_back( path );
            }
        }
        if ( data->paths.empty() )
            return;
        s_swatchData = data;
        SwatchJob().Start( "Texture swatches", [data]( const ki::LogFn &, std::string & ) {
            const size_t n = data->paths.size();
            data->rgba.resize( n );
            data->w.assign( n, 0 );
            data->h.assign( n, 0 );
            for ( size_t i = 0; i < n; ++i )
            {
                std::string err;
                if ( !ki::LoadThumbnail( data->paths[i], KMI_SWATCH, data->rgba[i], data->w[i], data->h[i], err ) )
                    data->rgba[i].clear();
            }
            return true;
        } );
    }

    void PollSwatches()
    {
        SwatchJob().TakeLog();
        bool ok = false;
        std::string err;
        if ( SwatchJob().TakeFinished( ok, err ) && s_swatchData )
        {
            const swatchJob_t &data = *s_swatchData;
            for ( size_t i = 0; i < data.paths.size() && i < data.rgba.size(); ++i )
            {
                swatch_t &s = s_swatches[data.paths[i]];
                if ( s.tex )
                    continue;
                s.tex    = data.rgba[i].empty() ? nullptr : CreateSwatch( data.rgba[i], data.w[i], data.h[i] );
                s.failed = !s.tex;
            }
            s_swatchData.reset();
        }
        if ( !SwatchJob().Running() && s_swatchAgain )
        {
            s_swatchAgain = false;
            RequestSwatches();
        }
    }

    // ── hulls / camera ───────────────────────────────────────────────────────────
    void SetHulls( std::vector<ki::Hull> hulls )
    {
        s_hulls = std::move( hulls );
        s_windings.clear();
        s_hullVolume = 0.0;
        for ( const ki::Hull &hull : s_hulls )
        {
            s_windings.push_back( ki::HullWindings( hull ) );
            s_hullVolume += ki::HullVolume( hull );
        }
        s_viewDirty = true;
    }

    void FrameModel()
    {
        const ki::ModelSpaceMesh &m = s_state.modelMesh;
        float radius = 0.0f;
        for ( int k = 0; k < 3; ++k )
        {
            s_target[k] = ( m.boundsMin[k] + m.boundsMax[k] ) * 0.5f;
            const float e = ( m.boundsMax[k] - m.boundsMin[k] ) * 0.5f;
            radius += e * e;
        }
        radius = sqrtf( radius );
        if ( !( radius > 0.01f ) )
            radius = 16.0f;
        const float aspect = s_viewH > 0 ? (float)s_viewW / (float)s_viewH : 1.0f;
        const float halfY  = KMI_FOV_Y * 0.5f * KMI_DEG2RAD;
        const float halfX  = atanf( tanf( halfY ) * aspect );
        s_dist = radius / sinf( ( std::min )( halfX, halfY ) ) * 1.05f;
        s_viewDirty = true;
    }

    // ── the job runner (worker thread: touches only `j`) ─────────────────────────
    bool SamePieces( const std::vector<ki::RecipePiece> &a, const std::vector<ki::RecipePiece> &b )
    {
        if ( a.size() != b.size() )
            return false;
        for ( size_t i = 0; i < a.size(); ++i )
            if ( a[i].index != b[i].index || a[i].triangles != b[i].triangles || a[i].name != b[i].name ||
                 a[i].surface != b[i].surface )
                return false;
        return true;
    }

    // collmaps\<asset>.map exists without the importer's marker: it was made or edited by
    // hand, and nothing automatic may overwrite it unasked.  (Pure file I/O: any thread.)
    bool HandMadeCollmap( const ki::GameRoot &root, const std::string &asset )
    {
        ki::Recipe named;
        named.asset = asset;
        const fs::path path = ki::MasterCollmapPath( root, named );
        std::error_code ec;
        return !asset.empty() && fs::exists( path, ec ) && !ki::IsGeneratedCollmap( path );
    }

    bool RunJob( job_t &j, const ki::LogFn &log, std::string &err )
    {
        switch ( j.kind )
        {
        case JOB_SCAN:
            j.sources = ki::ScanSources( j.root, j.source, j.sinceHours, err );
            for ( const ki::SourceEntry &e : j.sources )
                j.sourceHandMade.push_back( !e.hasRecipe && HandMadeCollmap( j.root, e.asset ) );
            return err.empty();
        case JOB_HULLS:
            return ki::CurrentHulls( j.recipe, j.root, j.state, j.hulls, log, err );
        case JOB_SAVE:
            if ( !ki::SaveRecipe( j.recipePath, j.recipe, err ) )
                return false;
            if ( j.recipe.collision.mode == "auto" )
                return ki::RegenerateCollision( j.recipe, j.root, j.state, false, j.hulls, log, err );
            return ki::CurrentHulls( j.recipe, j.root, j.state, j.hulls, log, err );
        case JOB_RESET_HULLS:
            if ( !ki::RegenerateCollision( j.recipe, j.root, j.state, true, j.hulls, log, err ) )
                return false;
            return ki::SaveRecipe( j.recipePath, j.recipe, err );
        case JOB_BUILD:
            if ( !ki::SaveRecipe( j.recipePath, j.recipe, err ) )
                return false;
            if ( !ki::BuildRecipe( j.recipe, j.root, j.state, log, j.report, err ) )
                return false;
            return ki::CurrentHulls( j.recipe, j.root, j.state, j.hulls, log, err );
        case JOB_CREATE:
            if ( !ki::NewRecipe( j.source, j.options, j.root, j.recipe, j.state, log, err ) )
                return false;
            j.recipePath = ki::RecipePath( j.root, j.recipe.asset );
            if ( !ki::SaveRecipe( j.recipePath, j.recipe, err ) )
                return false;
            break;
        case JOB_LOAD:
            if ( !ki::LoadRecipe( j.recipePath, j.recipe, err ) )
                return false;
            [[fallthrough]];
        case JOB_RELOAD:
        {
            if ( !ki::InspectSource( j.recipe, j.state, log, err ) )
                return false;
            const std::vector<ki::RecipePiece> before = j.recipe.pieces;
            ki::SyncPieces( j.recipe, j.state, log );
            j.piecesChanged = !SamePieces( before, j.recipe.pieces );
            break;
        }
        }
        // LOAD / RELOAD / CREATE: everything the window shows.
        if ( !ki::CurrentHulls( j.recipe, j.root, j.state, j.hulls, log, err ) )
            return false;
        ScanTextures( j.recipe, j.scan, j.setName, j.textureError );
        return true;
    }

    const char *JobTitle( jobKind_t kind )
    {
        switch ( kind )
        {
        case JOB_LOAD:        return "Loading";
        case JOB_RELOAD:      return "Reloading the source";
        case JOB_HULLS:       return "Collision";
        case JOB_SAVE:        return "Saving";
        case JOB_RESET_HULLS: return "Regenerating collision";
        case JOB_BUILD:       return "Building";
        case JOB_CREATE:      return "Importing";
        case JOB_SCAN:        return "Scanning";
        }
        return "Working";
    }

    std::shared_ptr<job_t> MakeJob( jobKind_t kind )
    {
        std::shared_ptr<job_t> j = std::make_shared<job_t>();
        j->kind       = kind;
        j->locks      = kind != JOB_HULLS && kind != JOB_SCAN;
        j->fromWindow = kind != JOB_CREATE && kind != JOB_SCAN;
        j->root       = s_root;
        return j;
    }

    // While any queued or running job will hand a recipe back, the recipe is read-only:
    // an edit made in between would be overwritten or saved from a stale copy.
    bool Locked()
    {
        if ( s_jobData && s_jobData->locks )
            return true;
        for ( const std::shared_ptr<job_t> &j : s_queue )
            if ( j->locks )
                return true;
        return false;
    }

    void StartJob( std::shared_ptr<job_t> j )
    {
        if ( Job().Running() || s_jobData )
        {
            s_queue.push_back( j );
            return;
        }
        if ( j->fromWindow )
        {
            // The open recipe as it is NOW, not when the job was queued.  A LOAD keeps the
            // path it was given and only borrows the state (same source = no reload).
            if ( !s_open && j->kind != JOB_LOAD )
                return;                                 // its recipe was closed meanwhile
            if ( s_open )
            {
                j->recipe = s_recipe;
                j->state  = s_state;
            }
            if ( j->kind != JOB_LOAD )
                j->recipePath = s_recipePath;
        }
        s_jobData = j;
        if ( j->locks )
            Log( std::string( JobTitle( j->kind ) ) + "..." );
        if ( !Job().Start( JobTitle( j->kind ), [j]( const ki::LogFn &log, std::string &err ) {
                 return RunJob( *j, log, err );
             } ) )
        {
            s_jobData.reset();
            s_queue.push_front( j );
        }
    }

    void RequestHulls()
    {
        if ( !s_open || !s_haveMesh )
            return;
        if ( s_jobData )
        {
            s_hullsAgain = true;
            return;
        }
        StartJob( MakeJob( JOB_HULLS ) );
    }

    void OpenRecipe( const std::string &asset )
    {
        std::shared_ptr<job_t> j = MakeJob( JOB_LOAD );
        j->recipePath = ki::RecipePath( s_root, asset );
        StartJob( j );
    }

    // Save / Build.  Automatic collision rewrites collmaps\<asset>.map at both, and a build
    // with collision off deletes it, so when that file was made by hand the "Hand-made
    // collision" popup decides first.  `then` runs once the job is queued (the "unsaved
    // changes" popup's save-and-continue).
    void SaveOrBuild( jobKind_t kind, std::function<void()> then = nullptr )
    {
        const std::string &mode = s_recipe.collision.mode;
        if ( ( mode == "auto" || ( mode == "none" && kind == JOB_BUILD ) ) &&
             HandMadeCollmap( s_root, s_recipe.asset ) )
        {
            s_handKind = kind;
            s_handThen = std::move( then );
            s_reqHand  = true;
            return;
        }
        StartJob( MakeJob( kind ) );
        if ( then )
            then();
    }

    void CloseRecipe()
    {
        s_open = false;
        s_haveMesh = false;
        s_dirty = false;
        s_recipe = ki::Recipe();
        s_recipePath.clear();
        s_state = ki::SourceState();
        SetHulls( {} );
        s_scan = ki::TextureScan();
        s_setName.clear();
        s_textureError.clear();
        s_sel.clear();
        s_hover = -1;
        s_haveBuild = false;
    }

    void Finish( job_t &j )
    {
        s_lastError.clear();
        switch ( j.kind )
        {
        case JOB_SCAN:
            s_sources         = std::move( j.sources );
            s_sourceHandMade  = std::move( j.sourceHandMade );
            s_sourceHandMade.resize( s_sources.size(), 0 );
            s_haveScan = true;
            return;
        case JOB_HULLS:
            if ( s_open && j.recipePath == s_recipePath )
                SetHulls( std::move( j.hulls ) );
            return;
        case JOB_BUILD:
        {
            s_state = std::move( j.state );
            SetHulls( std::move( j.hulls ) );
            s_lastBuild = std::move( j.report );
            s_haveBuild = true;
            for ( const auto &b : s_lastBuild.built )
                Log( "Built " + b.asset + " (" + std::to_string( b.hulls ) + " hulls)" );
            for ( const std::string &w : s_lastBuild.warnings )
                Log( "Warning: " + w );
            s_dirty = false;
            s_masterHandMade = HandMadeCollmap( s_root, s_recipe.asset );
            KiwiModelBrowser_RequestRefresh();
            Sys_Printf( "Model Import: built %i model(s) from %s.\n", (int)s_lastBuild.built.size(),
                        s_recipe.asset.c_str() );
            return;
        }
        case JOB_SAVE:
        case JOB_RESET_HULLS:
            // The recipe was read-only from the moment this was queued, so the window's copy
            // is what was saved; RESET only switched the collision back to automatic.
            if ( j.kind == JOB_RESET_HULLS )
                s_recipe.collision.mode = j.recipe.collision.mode;
            s_state = std::move( j.state );
            SetHulls( std::move( j.hulls ) );
            s_dirty = false;
            s_masterHandMade = HandMadeCollmap( s_root, s_recipe.asset );
            Log( "Saved " + s_recipePath.string() );
            return;
        default:
            break;
        }
        // LOAD / RELOAD / CREATE
        const bool fresh = j.kind != JOB_RELOAD;
        if ( fresh )
        {
            ReleaseSwatches();
            s_sel.clear();
            s_haveBuild = false;
            s_dirty = false;
        }
        s_recipe       = std::move( j.recipe );
        s_recipePath   = j.recipePath;
        s_state        = std::move( j.state );
        s_open         = true;
        s_haveMesh     = true;
        s_hover        = -1;
        s_scan         = std::move( j.scan );
        s_setName      = j.setName;
        s_textureError = j.textureError;
        SetHulls( std::move( j.hulls ) );
        s_srcChanged = false;
        if ( !StatFile( s_recipe.source, s_srcSize, s_srcStamp ) )
            s_srcSize = -1;
        if ( j.piecesChanged )
            s_dirty = true;                     // the piece table on disk no longer matches the mesh
        s_masterHandMade = HandMadeCollmap( s_root, s_recipe.asset );
        if ( s_masterHandMade && s_recipe.collision.mode == "auto" )
            Log( "Warning: collmaps\\" + s_recipe.asset + ".map was edited by hand; saving or building with "
                 "automatic collision would replace it" );
        if ( j.kind == JOB_CREATE )
        {
            s_listStale = true;
            Log( "Imported " + s_recipe.asset + " (" + std::to_string( s_state.pieceSet.pieces.size() ) +
                 " pieces)" );
        }
        if ( fresh )
            FrameModel();
        RequestSwatches();
    }

    void PollJobs()
    {
        for ( const std::string &line : Job().TakeLog() )
            Log( line );
        bool ok = false;
        std::string err;
        if ( Job().TakeFinished( ok, err ) && s_jobData )
        {
            std::shared_ptr<job_t> j = std::move( s_jobData );
            s_jobData.reset();
            if ( ok )
                Finish( *j );
            else
            {
                s_lastError = err;
                Log( "Error: " + err );
                Sys_Printf( "Model Import: %s\n", err.c_str() );
            }
        }
        if ( !s_jobData )
        {
            if ( s_hullsAgain )
            {
                s_hullsAgain = false;
                RequestHulls();
            }
            else if ( !s_queue.empty() )
            {
                std::shared_ptr<job_t> next = s_queue.front();
                s_queue.pop_front();
                StartJob( next );
            }
        }
        PollSwatches();

        // A re-export of the open source shows a Reload prompt (checked about once a second).
        if ( s_open && s_srcSize >= 0 && !s_srcChanged && ( ImGui::GetFrameCount() % 60 ) == 0 )
        {
            long long size = 0, stamp = 0;
            if ( StatFile( s_recipe.source, size, stamp ) && ( size != s_srcSize || stamp != s_srcStamp ) )
                s_srcChanged = true;
        }
    }

    void MarkDirty()
    {
        s_dirty = true;
    }

    // ── preview rendering ────────────────────────────────────────────────────────
    struct view_t
    {
        float eye[3], fwd[3], right[3], up[3];
        float axis[3][3];
        float tanX, tanY, zNear;
    };

    void ViewFor( int w, int h, view_t &v )
    {
        const float ang[3] = { s_pitch, s_yaw, 0.0f };
        AngleVectors( ang, v.fwd, v.right, v.up );
        for ( int k = 0; k < 3; ++k )
        {
            v.eye[k]     = s_target[k] - v.fwd[k] * s_dist;
            v.axis[0][k] = v.fwd[k];
            v.axis[1][k] = -v.right[k];     // R_Ed_SetSceneParms wants { forward, left, up }
            v.axis[2][k] = v.up[k];
        }
        v.tanY  = tanf( KMI_FOV_Y * 0.5f * KMI_DEG2RAD );
        v.tanX  = v.tanY * (float)w / (float)( h > 0 ? h : 1 );
        v.zNear = ( std::max )( 0.05f, ( std::min )( 16.0f, s_dist * 0.01f ) );
    }

    // CamWnd_SetupScene's perspective (infinite far plane), at our field of view.
    void Projection( const view_t &v, GfxMatrix &proj )
    {
        memset( &proj, 0, sizeof( proj ) );
        proj.m[0][0] = KMI_GUARD / v.tanX;
        proj.m[1][1] = KMI_GUARD / v.tanY;
        proj.m[2][2] = KMI_GUARD;
        proj.m[2][3] = 1.0f;
        proj.m[3][2] = KMI_GUARD * -v.zNear;
    }

    float    s_xyzw[KMI_BATCH_TRIS * 3][4];
    float    s_normal[KMI_BATCH_TRIS * 3][3];
    float    s_st[KMI_BATCH_TRIS * 3][2];
    float    s_color[KMI_BATCH_TRIS * 3];
    uint16_t s_index[KMI_BATCH_TRIS * 6];
    int      s_vc = 0;
    int      s_ic = 0;

    void TriFlush( Material *mtl )
    {
        if ( s_ic >= 6 )
            R_AddRenderCmdDrawTris( mtl, TECHNIQUE_UNLIT, (short)s_ic, s_index, (short)s_vc,
                                    s_xyzw, s_normal, s_color, s_st );
        s_vc = 0;
        s_ic = 0;
    }

    // One triangle in both windings: $opaque and white_tools both cull back faces.
    void TriAdd( Material *mtl, const float *a, const float *b, const float *c, const float *n, float packed )
    {
        if ( s_vc + 3 > KMI_BATCH_TRIS * 3 )
            TriFlush( mtl );
        const float *p[3] = { a, b, c };
        for ( int k = 0; k < 3; ++k )
        {
            const int i = s_vc + k;
            s_xyzw[i][0] = p[k][0]; s_xyzw[i][1] = p[k][1]; s_xyzw[i][2] = p[k][2]; s_xyzw[i][3] = 1.0f;
            s_normal[i][0] = n[0]; s_normal[i][1] = n[1]; s_normal[i][2] = n[2];
            s_st[i][0] = 0.0f; s_st[i][1] = 0.0f;
            s_color[i] = packed;
        }
        const uint16_t v0 = (uint16_t)s_vc;
        s_index[s_ic++] = v0;     s_index[s_ic++] = v0 + 1; s_index[s_ic++] = v0 + 2;
        s_index[s_ic++] = v0;     s_index[s_ic++] = v0 + 2; s_index[s_ic++] = v0 + 1;
        s_vc += 3;
    }

    float PackColor( float r, float g, float b, float a )
    {
        float rgba[4] = { ( std::min )( 1.0f, ( std::max )( 0.0f, r ) ), ( std::min )( 1.0f, ( std::max )( 0.0f, g ) ),
                          ( std::min )( 1.0f, ( std::max )( 0.0f, b ) ), ( std::min )( 1.0f, ( std::max )( 0.0f, a ) ) };
        GfxColor c;
        Byte4PackPixelColor( rgba, &c );
        float out;
        memcpy( &out, &c.packed, sizeof( out ) );
        return out;
    }

    void Lerp3( float *rgb, const float *to, float t )
    {
        for ( int k = 0; k < 3; ++k )
            rgb[k] += ( to[k] - rgb[k] ) * t;
    }

    void DrawMesh( const view_t &v )
    {
        const ki::ModelSpaceMesh &m = s_state.modelMesh;
        const size_t tris = m.trianglePiece.size();
        if ( !tris || s_modelMode == 2 || m.positions.size() < tris * 3 )
            return;
        const bool ghost = s_modelMode == 1;
        Material *mtl = ghost || !g_qeglobals.d_opague ? g_qeglobals.d_white : g_qeglobals.d_opague;
        if ( !mtl )
            return;
        // The command buffer is shared with the whole frame: a mesh too big for it is cut
        // short (the view says so), never overflowed.
        const int room = R_Ed_CmdBufferHeadroom() - KMI_CMD_RESERVE;
        if ( room <= KMI_TRI_BYTES )
            return;
        const size_t drawn = ( std::min )( tris, (size_t)room / KMI_TRI_BYTES );
        s_previewTris = (int)drawn;
        KiwiTris_FillNeutral();                  // vertex colours: no flat RGB override

        // A key light over the viewer's left shoulder; both sides lit alike.
        float light[3];
        for ( int k = 0; k < 3; ++k )
            light[k] = -v.fwd[k] * 0.55f + v.up[k] * 0.65f - v.right[k] * 0.35f;
        const float ll = sqrtf( light[0] * light[0] + light[1] * light[1] + light[2] * light[2] );
        for ( int k = 0; k < 3; ++k )
            light[k] /= ll;

        const size_t pieceCount = s_state.pieceSet.pieces.size();
        std::vector<std::array<float, 3>> pieceRgb( pieceCount + 1 );
        for ( size_t p = 0; p <= pieceCount; ++p )
        {
            const int surface = p < pieceCount ? PieceSurface( (int)p ) : 0;
            float *rgb = pieceRgb[p].data();
            memcpy( rgb, KMI_SURFACE_RGB[surface], sizeof( float ) * 3 );
            if ( s_sel.count( (int)p ) )
                Lerp3( rgb, KMI_SELECT_RGB, 0.65f );
            if ( (int)p == s_hover )
            {
                static const float white[3] = { 1.0f, 1.0f, 1.0f };
                Lerp3( rgb, white, 0.35f );
            }
        }

        const float alpha = ghost ? KMI_GHOST_A : 1.0f;
        for ( size_t t = 0; t < drawn; ++t )
        {
            const float *a = m.positions[t * 3 + 0].data();
            const float *b = m.positions[t * 3 + 1].data();
            const float *c = m.positions[t * 3 + 2].data();
            const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const float e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            const float len = sqrtf( n[0] * n[0] + n[1] * n[1] + n[2] * n[2] );
            if ( !( len > 0.0f ) )
                continue;
            n[0] /= len; n[1] /= len; n[2] /= len;
            const float shade = 0.28f + 0.72f * fabsf( n[0] * light[0] + n[1] * light[1] + n[2] * light[2] );
            const uint32_t piece = m.trianglePiece[t];
            const float *rgb = pieceRgb[piece < pieceCount ? piece : pieceCount].data();
            TriAdd( mtl, a, b, c, n, PackColor( rgb[0] * shade, rgb[1] * shade, rgb[2] * shade, alpha ) );
        }
        TriFlush( mtl );
    }

    void DrawGrid()
    {
        if ( !s_showGrid )
            return;
        const ki::ModelSpaceMesh &m = s_state.modelMesh;
        float ext = 0.0f;
        for ( int k = 0; k < 2; ++k )
            ext = ( std::max )( ext, ( std::max )( fabsf( m.boundsMin[k] ), fabsf( m.boundsMax[k] ) ) );
        ext = ext * 1.25f + 8.0f;
        float step = 1.0f;
        while ( ext / step > 24.0f )
            step *= 2.0f;
        const int n = (int)ceilf( ext / step );
        ext = n * step;

        KiwiLines_Begin( 4 * n + 8, 1 );
        KiwiLines_Color( 0.27f, 0.29f, 0.34f );
        for ( int i = -n; i <= n; ++i )
        {
            if ( i == 0 )
                continue;
            const float c = i * step;
            const float a0[3] = { c, -ext, 0.0f }, a1[3] = { c, ext, 0.0f };
            const float b0[3] = { -ext, c, 0.0f }, b1[3] = { ext, c, 0.0f };
            KiwiLines_Add( a0, a1 );
            KiwiLines_Add( b0, b1 );
        }
        const float x0[3] = { -ext, 0.0f, 0.0f }, x1[3] = { ext, 0.0f, 0.0f };
        const float y0[3] = { 0.0f, -ext, 0.0f }, y1[3] = { 0.0f, ext, 0.0f };
        KiwiLines_Color( 0.62f, 0.26f, 0.26f );
        KiwiLines_Add( x0, x1 );
        KiwiLines_Color( 0.28f, 0.55f, 0.28f );
        KiwiLines_Add( y0, y1 );
        KiwiLines_Flush();
    }

    void HullRgb( size_t i, float *rgb )
    {
        const ki::Hull &hull = s_hulls[i];
        memcpy( rgb, hull.piece < 0 || hull.kind == "manual" ? KMI_HAND_RGB : KMI_HULL_RGB, sizeof( float ) * 3 );
        if ( hull.piece >= 0 && s_sel.count( hull.piece ) )
            memcpy( rgb, KMI_SELECT_RGB, sizeof( float ) * 3 );
    }

    void DrawHulls( const view_t &v )
    {
        if ( !s_showHulls || s_windings.empty() )
            return;

        // Faces: translucent, front faces only (a convex hull's back faces would double the tint).
        // The grid's line batches left a flat-RGB material colour (w = 1) behind: reset it.
        if ( Material *mtl = g_qeglobals.d_white )
        {
            KiwiTris_FillNeutral();
            for ( size_t i = 0; i < s_windings.size(); ++i )
            {
                const auto &faces = s_windings[i];
                double hc[3] = { 0.0, 0.0, 0.0 };
                int count = 0;
                for ( const auto &face : faces )
                    for ( const auto &p : face )
                    {
                        hc[0] += p[0]; hc[1] += p[1]; hc[2] += p[2];
                        ++count;
                    }
                if ( !count )
                    continue;
                for ( int k = 0; k < 3; ++k )
                    hc[k] /= count;
                float rgb[3];
                HullRgb( i, rgb );
                const float packed = PackColor( rgb[0], rgb[1], rgb[2], KMI_HULL_A );
                for ( const auto &face : faces )
                {
                    if ( face.size() < 3 )
                        continue;
                    double fc[3] = { 0.0, 0.0, 0.0 }, n[3] = { 0.0, 0.0, 0.0 };
                    for ( size_t a = 0; a < face.size(); ++a )
                    {
                        const auto &p = face[a];
                        const auto &q = face[( a + 1 ) % face.size()];
                        n[0] += ( p[1] - q[1] ) * ( p[2] + q[2] );   // Newell
                        n[1] += ( p[2] - q[2] ) * ( p[0] + q[0] );
                        n[2] += ( p[0] - q[0] ) * ( p[1] + q[1] );
                        fc[0] += p[0]; fc[1] += p[1]; fc[2] += p[2];
                    }
                    for ( int k = 0; k < 3; ++k )
                        fc[k] /= (double)face.size();
                    if ( n[0] * ( fc[0] - hc[0] ) + n[1] * ( fc[1] - hc[1] ) + n[2] * ( fc[2] - hc[2] ) < 0.0 )
                    {
                        n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2];
                    }
                    if ( n[0] * ( v.eye[0] - fc[0] ) + n[1] * ( v.eye[1] - fc[1] ) + n[2] * ( v.eye[2] - fc[2] ) <= 0.0 )
                        continue;
                    const float nf[3] = { 0.0f, 0.0f, 1.0f };
                    const float p0[3] = { (float)face[0][0], (float)face[0][1], (float)face[0][2] };
                    for ( size_t a = 1; a + 1 < face.size(); ++a )
                    {
                        const float p1[3] = { (float)face[a][0], (float)face[a][1], (float)face[a][2] };
                        const float p2[3] = { (float)face[a + 1][0], (float)face[a + 1][1], (float)face[a + 1][2] };
                        TriAdd( mtl, p0, p1, p2, nf, packed );
                    }
                }
            }
            TriFlush( mtl );
        }

        // Edges.
        int edges = 0;
        for ( const auto &faces : s_windings )
            for ( const auto &face : faces )
                edges += (int)face.size();
        KiwiLines_Begin( ( std::min )( edges, 32000 ), 1 );
        for ( size_t i = 0; i < s_windings.size(); ++i )
        {
            float rgb[3];
            HullRgb( i, rgb );
            KiwiLines_Color( rgb[0], rgb[1], rgb[2] );
            for ( const auto &face : s_windings[i] )
                for ( size_t a = 0; a < face.size(); ++a )
                {
                    const auto &p = face[a];
                    const auto &q = face[( a + 1 ) % face.size()];
                    const float pf[3] = { (float)p[0], (float)p[1], (float)p[2] };
                    const float qf[3] = { (float)q[0], (float)q[1], (float)q[2] };
                    KiwiLines_Add( pf, qf );
                }
        }
        KiwiLines_Flush();
    }

    // ── picking ──────────────────────────────────────────────────────────────────
    int PickAt( float mx, float my )
    {
        if ( !s_haveMesh || s_viewW < 1 || s_viewH < 1 )
            return -1;
        view_t v;
        ViewFor( s_viewW, s_viewH, v );
        const float ndcX = 2.0f * mx / (float)s_viewW - 1.0f;
        const float ndcY = 1.0f - 2.0f * my / (float)s_viewH;
        float dir[3];
        for ( int k = 0; k < 3; ++k )
            dir[k] = v.fwd[k] + v.right[k] * ( ndcX * v.tanX / KMI_GUARD ) + v.up[k] * ( ndcY * v.tanY / KMI_GUARD );
        int piece = -1, triangle = -1;
        float t = 0.0f;
        return ki::PickPiece( s_state.modelMesh, v.eye, dir, piece, triangle, t ) ? piece : -1;
    }

    // ── ImGui pieces ─────────────────────────────────────────────────────────────
    void OpaqueImageCb( const ImDrawList *, const ImDrawCmd * )
    {
        if ( dx.device )
            dx.device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
    }

    void Swatch( const std::string &path, float size )
    {
        const auto it = s_swatches.find( path );
        if ( it != s_swatches.end() && it->second.tex )
            ImGui::Image( (ImTextureID)(intptr_t)it->second.tex, ImVec2( size, size ) );
        else
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled( p, ImVec2( p.x + size, p.y + size ), IM_COL32( 50, 54, 64, 255 ) );
            ImGui::Dummy( ImVec2( size, size ) );
        }
    }

    // The view's keys, on the same keycap strip the camera uses (kiwi_hints.cpp).
    const char *const KMI_VIEW_KEYS[][2] =
    {
        { "LMB", "Pick piece" }, { "Ctrl+LMB", "Add / remove" }, { "LMB/MMB drag", "Orbit" },
        { "RMB drag", "Pan" }, { "Shift+MMB", "Pan" }, { "Wheel", "Zoom" },
    };

    // `fromView`: picked in the 3D view, so the piece list scrolls to it once.
    void ClickPiece( int piece, bool toggle, bool fromView )
    {
        if ( fromView && piece >= 0 )
            s_scrollTo = piece;
        if ( toggle )
        {
            if ( piece >= 0 && !s_sel.erase( piece ) )
                s_sel.insert( piece );
        }
        else
        {
            s_sel.clear();
            if ( piece >= 0 )
                s_sel.insert( piece );
        }
        s_viewDirty = true;
    }

    void DrawView( float width, float height )
    {
        const ImVec2 limit = ImGui::GetMainViewport()->Size;
        const int w = (int)( std::min )( width, limit.x );
        const int h = (int)( std::min )( height, limit.y );
        if ( w < 8 || h < 8 )
            return;
        if ( w != s_viewW || h != s_viewH )
            s_viewDirty = true;
        s_viewW = w;
        s_viewH = h;
        s_viewFrame = ImGui::GetFrameCount();

        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 p1( p0.x + w, p0.y + h );
        ImGui::InvisibleButton( "##kmiview", ImVec2( (float)w, (float)h ),
                                ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                ImGuiButtonFlags_MouseButtonMiddle );
        const bool hovered = ImGui::IsItemHovered();
        const bool active  = ImGui::IsItemActive();
        const bool clicked = ImGui::IsItemClicked( ImGuiMouseButton_Left );

        ImDrawList *dl = ImGui::GetWindowDrawList();
        IDirect3DTexture9 *tex = RTT_PreviewTexture();
        if ( s_haveMesh && tex && s_rtW > 0 )
        {
            dl->AddCallback( OpaqueImageCb, nullptr );
            dl->AddImage( (ImTextureID)(intptr_t)tex, p0, p1 );
            dl->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
        }
        else
        {
            dl->AddRectFilled( p0, p1, IM_COL32( 33, 36, 43, 255 ) );
            const char *msg = s_jobData ? "Loading..." : "No model open";
            const ImVec2 ts = ImGui::CalcTextSize( msg );
            dl->AddText( ImVec2( p0.x + ( w - ts.x ) * 0.5f, p0.y + ( h - ts.y ) * 0.5f ),
                         IM_COL32( 150, 156, 170, 255 ), msg );
        }
        if ( s_jobData && s_haveMesh )
            dl->AddText( ImVec2( p0.x + 10.0f, p0.y + 8.0f ), IM_COL32( 236, 200, 120, 255 ), Job().Title().c_str() );
        if ( !s_haveMesh )
            return;
        const int total = (int)s_state.modelMesh.trianglePiece.size();
        if ( s_modelMode != 2 && s_previewTris > 0 && s_previewTris < total )
        {
            char note[96];
            _snprintf( note, sizeof( note ), "Preview shows %i of %i triangles", s_previewTris, total );
            note[sizeof( note ) - 1] = '\0';
            dl->AddText( ImVec2( p0.x + 10.0f, p0.y + 8.0f + ImGui::GetTextLineHeightWithSpacing() ),
                         IM_COL32( 236, 200, 120, 255 ), note );
        }
        if ( hovered )
            KiwiHints_DrawKeys( p0.x, p0.y, (float)w, (float)h, KMI_VIEW_KEYS,
                                (int)( sizeof( KMI_VIEW_KEYS ) / sizeof( KMI_VIEW_KEYS[0] ) ) );

        ImGuiIO &io = ImGui::GetIO();
        if ( clicked )
        {
            s_lmbPress = true;
            s_lmbDrag  = false;
        }
        if ( s_lmbPress && ImGui::IsMouseDragging( ImGuiMouseButton_Left, 4.0f ) )
            s_lmbDrag = true;
        if ( s_lmbPress && ImGui::IsMouseReleased( ImGuiMouseButton_Left ) )
        {
            if ( !s_lmbDrag && hovered )
                ClickPiece( PickAt( io.MousePos.x - p0.x, io.MousePos.y - p0.y ), io.KeyCtrl, true );
            s_lmbPress = false;
        }

        if ( active && ( io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f ) )
        {
            const bool mmb = ImGui::IsMouseDown( ImGuiMouseButton_Middle );
            const bool rmb = ImGui::IsMouseDown( ImGuiMouseButton_Right );
            const bool lmb = s_lmbPress && s_lmbDrag;
            if ( rmb || ( mmb && io.KeyShift ) )
            {
                view_t v;
                ViewFor( w, h, v );
                const float scale = 2.0f * s_dist * v.tanY / (float)h;
                for ( int k = 0; k < 3; ++k )
                    s_target[k] += ( -v.right[k] * io.MouseDelta.x + v.up[k] * io.MouseDelta.y ) * scale;
                s_viewDirty = true;
            }
            else if ( mmb || lmb )
            {
                s_yaw   -= io.MouseDelta.x * KMI_ORBIT_DEG;
                s_pitch += io.MouseDelta.y * KMI_ORBIT_DEG;
                s_pitch  = ( std::max )( -89.0f, ( std::min )( 89.0f, s_pitch ) );
                s_viewDirty = true;
            }
        }
        if ( hovered && io.MouseWheel != 0.0f )
        {
            s_dist *= powf( KMI_ZOOM_STEP, io.MouseWheel );
            s_dist  = ( std::max )( 0.5f, ( std::min )( 200000.0f, s_dist ) );
            s_viewDirty = true;
        }

        // Hover highlight + tooltip.  The ray test walks every triangle, so it reruns only
        // when the mouse moved or the camera did (s_viewDirty), not every frame.
        int hover = -1;
        if ( hovered && !active )
            hover = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || s_viewDirty
                  ? PickAt( io.MousePos.x - p0.x, io.MousePos.y - p0.y )
                  : s_hover;
        if ( hover != s_hover )
        {
            s_hover = hover;
            s_viewDirty = true;
        }
        if ( s_hover >= 0 && s_hover < (int)s_state.pieceSet.pieces.size() )
        {
            const auto &piece = s_state.pieceSet.pieces[s_hover];
            ImGui::BeginTooltip();
            ImGui::Text( "%s", piece.name.c_str() );
            SurfaceDot( PieceSurface( s_hover ) );
            ImGui::SameLine();
            ImGui::Text( "%s  -  %i triangles", cod4::SurfaceTypeName( PieceSurface( s_hover ) ),
                         (int)piece.triangleCount );
            ImGui::EndTooltip();
        }
    }

    // ── property sections ────────────────────────────────────────────────────────
    const char *const KMI_UNITS[]        = { "in", "m", "cm", "mm" };
    const char *const KMI_UNIT_LABELS[]  = { "Inches", "Meters", "Centimeters", "Millimeters" };
    const char *const KMI_ORIGINS[]      = { "base", "center", "none" };
    const char *const KMI_ORIGIN_LABELS[] = { "Base (bottom centre on the ground)", "Bounds centre", "Keep the source origin" };
    const char *const KMI_GREEN[]        = { "auto", "flip", "keep" };
    const char *const KMI_GREEN_LABELS[] = { "Auto-detect", "Flip (OpenGL export)", "Keep (DirectX export)" };
    const char *const KMI_SHAPES[]       = { "auto", "box", "kdop" };
    const char *const KMI_SHAPE_LABELS[] = { "Auto (box or 26-sided)", "Box", "26-sided (k-DOP)" };
    struct clipMaterial_t { const char *name; const char *what; };
    const clipMaterial_t KMI_CLIP[] =
    {
        { "clip",               "Players, AI and vehicles" },
        { "clip_metal",         "Also stops bullets, with metal impacts" },
        { "clip_nosight",       "Also blocks AI sight" },
        { "clip_nosight_metal", "Blocks AI sight; metal impacts" },
        { "clip_player",        "Players only" },
        { "clip_ai",            "AI only" },
        { "clip_vehicle",       "Vehicles only" },
        { "clip_foliage",       "Foliage clip" },
    };

    const float KMI_LABEL_W = 120.0f;   // the label column of every property row

    void Label( const char *text )
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( text );
        ImGui::SameLine( KMI_LABEL_W );
    }

    void StartReload()
    {
        MarkDirty();
        StartJob( MakeJob( JOB_RELOAD ) );
    }

    void Section_Model()
    {
        const ki::ModelSpaceMesh &m = s_state.modelMesh;
        Label( "Asset" );
        ImGui::TextUnformatted( s_recipe.asset.c_str() );
        ImGui::SetItemTooltip( "Set at import.  Texture variant 2 builds %s, and so on.",
                               ki::VariantAssetName( s_recipe.asset, "2" ).c_str() );

        Label( "Source" );
        const std::string file = fs::path( s_recipe.source ).filename().string();
        ImGui::TextUnformatted( file.c_str() );
        ImGui::SetItemTooltip( "%s", s_recipe.source.c_str() );
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Show" ) )
            KiwiWindows_RevealInExplorer( s_recipe.source.c_str() );
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Reload" ) )
            StartJob( MakeJob( JOB_RELOAD ) );
        ImGui::SetItemTooltip( "Read the source file again (after a re-export)." );
        if ( s_srcChanged )
        {
            ImGui::TextColored( ImVec4( 1.0f, 0.78f, 0.35f, 1.0f ), "The source changed on disk." );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Reload now" ) )
                StartJob( MakeJob( JOB_RELOAD ) );
        }

        Label( "Geometry" );
        ImGui::Text( "%i pieces, %i triangles", (int)s_state.pieceSet.pieces.size(), (int)m.trianglePiece.size() );
        Label( "Size" );
        ImGui::Text( "%.1f x %.1f x %.1f in", m.boundsMax[0] - m.boundsMin[0], m.boundsMax[1] - m.boundsMin[1],
                     m.boundsMax[2] - m.boundsMin[2] );

        ImGui::Spacing();
        Label( "Units" );
        ImGui::SetNextItemWidth( -1.0f );
        if ( StringCombo( "##units", s_recipe.units, KMI_UNITS, KMI_UNIT_LABELS, 4 ) )
            StartReload();
        ImGui::SetItemTooltip( "The unit the source file was modelled in (Plasticity exports: usually inches)." );
        Label( "Scale" );
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::InputDouble( "##scale", &s_recipe.scale, 0.0, 0.0, "%.4g" );
        if ( ImGui::IsItemDeactivatedAfterEdit() )
        {
            if ( !( s_recipe.scale > 0.0 ) )
                s_recipe.scale = 1.0;
            StartReload();
        }
        Label( "Origin" );
        ImGui::SetNextItemWidth( -1.0f );
        if ( StringCombo( "##origin", s_recipe.origin, KMI_ORIGINS, KMI_ORIGIN_LABELS, 3 ) )
            StartReload();
        Label( "Winding" );
        if ( ImGui::Checkbox( "Flip faces", &s_recipe.flipWinding ) )
            StartReload();
        ImGui::SetItemTooltip( "Turn every face inside out (a mirrored export draws inside-out in game)." );
        Label( "Texture V" );
        if ( ImGui::Checkbox( "Flip", &s_recipe.uvFlip ) )
            MarkDirty();
        ImGui::SetItemTooltip( "Flip texture V (on for Plasticity / Blender / OBJ exports)." );
        Label( "Normal green" );
        ImGui::SetNextItemWidth( -1.0f );
        if ( StringCombo( "##green", s_recipe.normalGreen, KMI_GREEN, KMI_GREEN_LABELS, 3 ) )
            MarkDirty();
        ImGui::SetItemTooltip( "The normal map's green channel convention." );
    }

    void Section_Surfaces()
    {
        Label( "Default" );
        SurfaceDot( SurfaceIndex( s_recipe.surface ) );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( -1.0f );
        if ( SurfaceCombo( "##defsurf", s_recipe.surface, nullptr ) )
        {
            MarkDirty();
            s_viewDirty = true;
            RequestHulls();
        }
        ImGui::SetItemTooltip( "Bullet impacts, footsteps and penetration for every piece without its own type." );

        const int count = (int)s_recipe.pieces.size();
        char inherit[64];
        _snprintf( inherit, sizeof( inherit ), "default (%s)", cod4::SurfaceTypeName( SurfaceIndex( s_recipe.surface ) ) );
        inherit[sizeof( inherit ) - 1] = '\0';

        if ( !s_sel.empty() )
        {
            ImGui::Text( "%i selected", (int)s_sel.size() );
            ImGui::SameLine();
            std::string pick;
            ImGui::SetNextItemWidth( 170.0f );
            if ( SurfaceCombo( "##selsurf", pick, inherit ) )
            {
                for ( int piece : s_sel )
                    if ( piece >= 0 && piece < count )
                        s_recipe.pieces[piece].surface = pick;
                MarkDirty();
                s_viewDirty = true;
                RequestHulls();
            }
            ImGui::SetItemTooltip( "Set the surface of every selected piece." );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Clear" ) )
            {
                s_sel.clear();
                s_viewDirty = true;
            }
        }

        const float rowH   = ImGui::GetFrameHeightWithSpacing();
        const float height = ( std::min )( (float)count, 12.0f ) * rowH + rowH + 4.0f;
        if ( !ImGui::BeginTable( "##pieces", 3,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                                 ImGuiTableFlags_SizingStretchProp,
                                 ImVec2( 0.0f, height ) ) )
            return;
        ImGui::TableSetupScrollFreeze( 0, 1 );
        ImGui::TableSetupColumn( "Piece", ImGuiTableColumnFlags_WidthStretch, 1.0f );
        ImGui::TableSetupColumn( "Tris", ImGuiTableColumnFlags_WidthFixed, 50.0f );
        ImGui::TableSetupColumn( "Surface", ImGuiTableColumnFlags_WidthFixed, 150.0f );
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin( count );
        if ( s_scrollTo >= count )
            s_scrollTo = -1;
        if ( s_scrollTo >= 0 )
            clipper.IncludeItemByIndex( s_scrollTo );
        while ( clipper.Step() )
            for ( int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i )
            {
                ki::RecipePiece &piece = s_recipe.pieces[i];
                ImGui::PushID( i );
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex( 0 );
                const bool selected = s_sel.count( i ) != 0;
                ImGui::AlignTextToFramePadding();
                if ( ImGui::Selectable( piece.name.empty() ? "(unnamed)" : piece.name.c_str(), selected,
                                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap ) )
                    ClickPiece( i, ImGui::GetIO().KeyCtrl, false );
                if ( i == s_scrollTo )
                {
                    ImGui::SetScrollHereY( 0.5f );
                    s_scrollTo = -1;
                }
                ImGui::TableSetColumnIndex( 1 );
                ImGui::Text( "%i", piece.triangles );
                ImGui::TableSetColumnIndex( 2 );
                SurfaceDot( PieceSurface( i ) );
                ImGui::SameLine();
                ImGui::SetNextItemWidth( -1.0f );
                if ( SurfaceCombo( "##surf", piece.surface, inherit ) )
                {
                    MarkDirty();
                    s_viewDirty = true;
                    RequestHulls();
                }
                ImGui::PopID();
            }
        ImGui::EndTable();
    }

    ki::RecipeVariant *FindVariant( const std::string &id )
    {
        for ( ki::RecipeVariant &v : s_recipe.textures.variants )
            if ( SameText( v.id, id ) )
                return &v;
        return nullptr;
    }

    void RescanTextures()
    {
        ScanTextures( s_recipe, s_scan, s_setName, s_textureError );
        ReleaseSwatches();
        RequestSwatches();
    }

    void MapLetters( const ki::TextureFiles &f )
    {
        const struct { const char *letter; bool have; const char *name; } maps[] =
        {
            { "C", !f.baseColor.empty(), "Base colour" }, { "N", !f.normal.empty(), "Normal" },
            { "M", !f.metalness.empty(), "Metalness" },   { "R", !f.roughness.empty(), "Roughness" },
            { "O", !f.opacity.empty(), "Opacity" },
        };
        for ( int i = 0; i < 5; ++i )
        {
            if ( i )
                ImGui::SameLine( 0.0f, 3.0f );
            ImGui::TextColored( maps[i].have ? ImVec4( 0.85f, 0.90f, 1.0f, 1.0f ) : ImVec4( 0.4f, 0.42f, 0.46f, 1.0f ),
                                "%s", maps[i].letter );
            ImGui::SetItemTooltip( "%s: %s", maps[i].name, maps[i].have ? "found" : "missing (a neutral default is used)" );
        }
    }

    void Section_Textures()
    {
        ki::Recipe::Textures &t = s_recipe.textures;
        const float buttonW = ImGui::CalcTextSize( "Guess" ).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        Label( "Folder" );
        ImGui::SetNextItemWidth( -( buttonW + ImGui::GetStyle().ItemSpacing.x ) );
        InputString( "##texfolder", t.folder );
        if ( ImGui::IsItemDeactivatedAfterEdit() )
        {
            MarkDirty();
            RescanTextures();
        }
        ImGui::SetItemTooltip( "The InstaMAT export folder." );
        ImGui::SameLine();
        if ( ImGui::Button( "...##texfolder", ImVec2( buttonW, 0.0f ) ) &&
             BrowseFolder( "InstaMAT export folder", t.folder ) )
        {
            MarkDirty();
            RescanTextures();
        }

        Label( "Prefix" );
        ImGui::SetNextItemWidth( -( buttonW + ImGui::GetStyle().ItemSpacing.x ) );
        InputString( "##texprefix", t.prefix );
        if ( ImGui::IsItemDeactivatedAfterEdit() )
        {
            MarkDirty();
            RescanTextures();
        }
        ImGui::SetItemTooltip( "The name InstaMAT put in front of every map: <prefix>_Default_..._BaseColor.png" );
        ImGui::SameLine();
        if ( ImGui::Button( "Guess", ImVec2( buttonW, 0.0f ) ) )
        {
            const std::string guess = ki::GuessTexturePrefix( s_recipe.source, t.folder );
            if ( guess.empty() )
                Log( "No texture prefix in " + t.folder + " matches " + fs::path( s_recipe.source ).filename().string() );
            else if ( guess != t.prefix )
            {
                t.prefix = guess;
                MarkDirty();
                RescanTextures();
            }
        }

        if ( s_scan.sets.size() > 1 )
        {
            Label( "Texture set" );
            ImGui::SetNextItemWidth( -1.0f );
            if ( ImGui::BeginCombo( "##texset", s_setName.c_str() ) )
            {
                for ( const auto &set : s_scan.sets )
                    if ( ImGui::Selectable( set.first.c_str(), set.first == s_setName ) && set.first != s_setName )
                    {
                        t.set = set.first;
                        MarkDirty();
                        RescanTextures();
                    }
                ImGui::EndCombo();
            }
        }

        if ( ImGui::SmallButton( "Rescan" ) )
            RescanTextures();
        ImGui::SetItemTooltip( "Look for new or re-exported maps." );
        if ( !s_textureError.empty() )
        {
            ImGui::PushTextWrapPos( 0.0f );
            ImGui::TextColored( ImVec4( 1.0f, 0.78f, 0.35f, 1.0f ), "%s", s_textureError.c_str() );
            ImGui::PopTextWrapPos();
        }

        const std::vector<ki::TextureVariant> *found = FoundVariants();
        const float swatch = 40.0f;
        if ( ImGui::BeginTable( "##variants", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp ) )
        {
            ImGui::TableSetupColumn( "Build", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() );
            ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed, swatch );
            ImGui::TableSetupColumn( "Variant", ImGuiTableColumnFlags_WidthStretch, 1.0f );
            ImGui::TableSetupColumn( "Maps", ImGuiTableColumnFlags_WidthFixed, 70.0f );
            ImGui::TableHeadersRow();
            if ( found )
                for ( const ki::TextureVariant &v : *found )
                {
                    ImGui::PushID( v.id.c_str() );
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex( 0 );
                    ki::RecipeVariant *rv = FindVariant( v.id );
                    bool on = rv && rv->enabled;
                    if ( ImGui::Checkbox( "##on", &on ) )
                    {
                        if ( rv )
                            rv->enabled = on;
                        else
                            t.variants.push_back( { v.id, on } );
                        MarkDirty();
                    }
                    ImGui::SetItemTooltip( "Build this variant." );
                    ImGui::TableSetColumnIndex( 1 );
                    Swatch( v.files.baseColor.string(), swatch );
                    ImGui::TableSetColumnIndex( 2 );
                    ImGui::Text( "%s", ki::VariantAssetName( s_recipe.asset, v.id ).c_str() );
                    if ( v.width > 0 )
                        ImGui::TextDisabled( "variant %s, %i x %i", v.id.c_str(), v.width, v.height );
                    else
                        ImGui::TextDisabled( "variant %s", v.id.c_str() );
                    ImGui::TableSetColumnIndex( 3 );
                    MapLetters( v.files );
                    ImGui::PopID();
                }
            // Variants the recipe asks for that the folder no longer has.
            for ( ki::RecipeVariant &rv : t.variants )
            {
                bool present = false;
                if ( found )
                    for ( const ki::TextureVariant &v : *found )
                        present |= SameText( v.id, rv.id );
                if ( present )
                    continue;
                ImGui::PushID( rv.id.c_str() );
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex( 0 );
                if ( ImGui::Checkbox( "##on", &rv.enabled ) )
                    MarkDirty();
                ImGui::TableSetColumnIndex( 2 );
                ImGui::Text( "%s", ki::VariantAssetName( s_recipe.asset, rv.id ).c_str() );
                ImGui::TextColored( ImVec4( 1.0f, 0.55f, 0.45f, 1.0f ), "variant %s: no maps found", rv.id.c_str() );
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        Label( "Glass opacity" );
        ImGui::SetNextItemWidth( -1.0f );
        const double lo = 0.0, hi = 1.0;
        if ( ImGui::SliderScalar( "##glass", ImGuiDataType_Double, &t.glassOpacity, &lo, &hi, "%.2f" ) )
            MarkDirty();
        ImGui::SetItemTooltip( "How see-through glass pieces are when the variant has no opacity map." );
    }

    void Section_Collision()
    {
        ki::Recipe::Collision &c = s_recipe.collision;
        const bool isAuto = c.mode == "auto", isManual = c.mode == "manual", isNone = c.mode == "none";
        Label( "Mode" );
        if ( ImGui::RadioButton( "Automatic", isAuto ) && !isAuto )
        {
            if ( isManual )
            {
                s_leaveTo  = "auto";                // would discard the hand edits: ask
                s_reqLeave = true;
            }
            else
            {
                c.mode = "auto";
                MarkDirty();
                RequestHulls();
            }
        }
        ImGui::SetItemTooltip( "Hulls are generated from the mesh; every save and build rewrites\n"
                               "collmaps\\%s.map with them.", s_recipe.asset.c_str() );
        ImGui::SameLine();
        if ( ImGui::RadioButton( "Hand-edited", isManual ) && !isManual )
        {
            c.mode = "manual";
            MarkDirty();
            RequestHulls();
        }
        ImGui::SetItemTooltip( "The build uses collmaps\\%s.map as it is: open it as a map to edit the\n"
                               "hulls.  It starts from the hulls last saved.", s_recipe.asset.c_str() );
        ImGui::SameLine();
        if ( ImGui::RadioButton( "None", isNone ) && !isNone )
        {
            if ( isManual )
            {
                s_leaveTo  = "none";
                s_reqLeave = true;
            }
            else
            {
                c.mode = "none";
                MarkDirty();
                RequestHulls();
            }
        }
        ImGui::SetItemTooltip( "No player, AI or vehicle collision: building deletes this model's collmaps." );

        if ( ( isAuto || isNone ) && s_masterHandMade )
        {
            ImGui::PushTextWrapPos( 0.0f );
            ImGui::TextColored( ImVec4( 1.0f, 0.78f, 0.35f, 1.0f ),
                                isAuto ? "collmaps\\%s.map was edited by hand; saving or building replaces it."
                                       : "collmaps\\%s.map was edited by hand; building deletes it.",
                                s_recipe.asset.c_str() );
            ImGui::PopTextWrapPos();
            if ( ImGui::SmallButton( "Keep it (Hand-edited)" ) )
            {
                c.mode = "manual";
                MarkDirty();
                RequestHulls();
            }
        }
        if ( isNone )
            return;

        // Enabled in Hand-edited mode too: the build writes the variant copies with it.
        Label( "Material" );
        ImGui::SetNextItemWidth( -1.0f );
        if ( ImGui::BeginCombo( "##clipmat", c.material.c_str() ) )
        {
            for ( const clipMaterial_t &m : KMI_CLIP )
            {
                if ( ImGui::Selectable( m.name, c.material == m.name ) && c.material != m.name )
                {
                    c.material = m.name;
                    MarkDirty();
                }
                ImGui::SameLine( 170.0f );
                ImGui::TextDisabled( "%s", m.what );
            }
            ImGui::EndCombo();
        }
        if ( isManual )
            ImGui::SetItemTooltip( "The clip material of texture variants 2 and up; collmaps\\%s.map keeps its own.",
                                   s_recipe.asset.c_str() );
        else
            ImGui::SetItemTooltip( "The clip material the hull brushes are written with." );

        ImGui::BeginDisabled( isManual );
        bool changed = false;
        Label( "Shape" );
        ImGui::SetNextItemWidth( -1.0f );
        changed |= StringCombo( "##shape", c.settings.shape, KMI_SHAPES, KMI_SHAPE_LABELS, 3 );
        ImGui::SetItemTooltip( "Auto uses the 26-sided hull when it hugs a piece noticeably tighter than a box." );

        const double zero = 0.0, one = 1.0, inches = 512.0;
        Label( "Smallest piece" );
        ImGui::SetNextItemWidth( -1.0f );
        changed |= ImGui::DragScalar( "##minpiece", ImGuiDataType_Double, &c.settings.minPieceSize, 0.1f, &zero, &inches, "%.1f in" );
        ImGui::SetItemTooltip( "Pieces whose longest side is shorter get no hull (bolts, trim)." );
        Label( "Min thickness" );
        ImGui::SetNextItemWidth( -1.0f );
        changed |= ImGui::DragScalar( "##minthick", ImGuiDataType_Double, &c.settings.minThickness, 0.05f, &zero, &inches, "%.2f in" );
        ImGui::SetItemTooltip( "Thinner hulls are thickened about their middle (sheet metal, glass)." );
        Label( "26-sided ratio" );
        ImGui::SetNextItemWidth( -1.0f );
        changed |= ImGui::SliderScalar( "##kdop", ImGuiDataType_Double, &c.settings.kdopRatio, &zero, &one, "%.2f" );
        ImGui::SetItemTooltip( "Auto shape: the 26-sided hull is used when its volume is below this share of the box." );
        Label( "Split gain" );
        ImGui::SetNextItemWidth( -1.0f );
        changed |= ImGui::SliderScalar( "##split", ImGuiDataType_Double, &c.settings.splitGain, &zero, &one, "%.2f" );
        ImGui::SetItemTooltip( "Bent pieces (an L pipe) are split while that shrinks the hulls below this share\n"
                               "of one hull.  Lower = fewer splits." );
        Label( "Max splits" );
        ImGui::SetNextItemWidth( -1.0f );
        changed |= ImGui::SliderInt( "##depth", &c.settings.maxDepth, 0, 6 );
        ImGui::SetItemTooltip( "Split depth: at most 2^n hulls per piece." );
        ImGui::EndDisabled();
        if ( changed )
        {
            MarkDirty();
            RequestHulls();
        }

        ImGui::Spacing();
        ImGui::Text( "%i hulls, %.0f cubic in", (int)s_hulls.size(), s_hullVolume );
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Show file" ) )
            KiwiWindows_RevealInExplorer( ki::MasterCollmapPath( s_root, s_recipe ).string().c_str() );
        if ( isManual )
        {
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Regenerate" ) )
            {
                s_leaveTo  = "auto";
                s_reqLeave = true;
            }
            ImGui::SetItemTooltip( "Replace the hand-edited hulls with automatic ones." );
        }
    }

    void Section_Build()
    {
        std::vector<std::string> targets;
        const std::vector<ki::TextureVariant> *found = FoundVariants();
        for ( const ki::RecipeVariant &rv : s_recipe.textures.variants )
        {
            if ( !rv.enabled || !found )
                continue;
            for ( const ki::TextureVariant &v : *found )
                if ( SameText( v.id, rv.id ) && !v.files.baseColor.empty() )
                    targets.push_back( ki::VariantAssetName( s_recipe.asset, v.id ) );
        }
        if ( targets.empty() )
            ImGui::TextColored( ImVec4( 1.0f, 0.78f, 0.35f, 1.0f ), "No texture variant is ticked." );
        for ( const std::string &name : targets )
            ImGui::BulletText( "%s", name.c_str() );

        ImGui::BeginDisabled( targets.empty() );
        char label[64];
        _snprintf( label, sizeof( label ), "Build %i model%s", (int)targets.size(), targets.size() == 1 ? "" : "s" );
        label[sizeof( label ) - 1] = '\0';
        if ( ImGui::Button( label, ImVec2( -1.0f, 0.0f ) ) )
            SaveOrBuild( JOB_BUILD );
        ImGui::EndDisabled();
        ImGui::SetItemTooltip( "Saves the recipe, then writes the xmodels, materials, images and collision.\n"
                               "New models appear in the Models browser.  A model this session already\n"
                               "loaded may keep its old look until Radiant restarts." );

        if ( s_haveBuild )
        {
            ImGui::Spacing();
            ImGui::TextDisabled( "Last build" );
            for ( const auto &b : s_lastBuild.built )
                ImGui::BulletText( "%s  (%i hulls)", b.asset.c_str(), (int)b.hulls );
            for ( const std::string &w : s_lastBuild.warnings )
            {
                ImGui::PushTextWrapPos( 0.0f );
                ImGui::TextColored( ImVec4( 1.0f, 0.78f, 0.35f, 1.0f ), "%s", w.c_str() );
                ImGui::PopTextWrapPos();
            }
        }
    }

    // The property column.  A new group of model properties = a Section_* function + a row.
    struct section_t
    {
        const char *title;
        void ( *draw )();
    };
    const section_t KMI_SECTIONS[] =
    {
        { "Model",     Section_Model },
        { "Surfaces",  Section_Surfaces },
        { "Textures",  Section_Textures },
        { "Collision", Section_Collision },
        { "Build",     Section_Build },
    };

    void DrawProperties()
    {
        if ( !s_open )
        {
            ImGui::TextDisabled( s_jobData ? "Loading..." : "No model open" );
            return;
        }
        for ( const section_t &section : KMI_SECTIONS )
        {
            ImGui::PushID( section.title );
            if ( ImGui::CollapsingHeader( section.title, ImGuiTreeNodeFlags_DefaultOpen ) )
            {
                section.draw();
                ImGui::Spacing();
            }
            ImGui::PopID();
        }
    }

    // ── toolbar + popups ─────────────────────────────────────────────────────────
    // Runs `action` now, or after "discard changes?" when the open recipe has edits.
    void AfterDiscard( std::function<void()> action )
    {
        if ( s_open && s_dirty )
        {
            s_pendingAction = std::move( action );
            s_reqDiscard = true;
            return;
        }
        action();
    }

    void StartCreate( const fs::path &source, const ki::NewRecipeOptions &options )
    {
        std::shared_ptr<job_t> j = MakeJob( JOB_CREATE );
        j->source  = source;
        j->options = options;
        StartJob( j );
    }

    ki::NewRecipeOptions NewOptions( const std::string &asset, const std::string &folder, const std::string &prefix )
    {
        ki::NewRecipeOptions o;
        o.asset         = asset;
        o.surface       = s_newSurface;
        o.units         = s_newUnits;
        o.textureFolder = folder;
        o.texturePrefix = prefix;
        return o;
    }

    void StartScan()
    {
        if ( s_scanFolder.empty() )
            return;
        RememberSetting( "sourceFolder", s_scanFolder );
        std::shared_ptr<job_t> j = MakeJob( JOB_SCAN );
        j->source     = s_scanFolder;
        j->sinceHours = s_scanHours;
        s_haveScan = false;
        StartJob( j );
    }

    void BeginNew()
    {
        std::string path;
        if ( !BrowseModelFile( path, ki::GetImportSetting( s_root, "sourceFolder" ) ) )
            return;
        s_scanFolder = fs::path( path ).parent_path().string();
        RememberSetting( "sourceFolder", s_scanFolder );
        s_newSource  = path;
        s_newAsset   = ki::NewRecipeAssetName( path, ki::NewRecipeOptions() );
        s_newFolder  = ki::DefaultTextureFolder( s_root );
        s_newPrefix  = s_newFolder.empty() ? std::string() : ki::GuessTexturePrefix( path, s_newFolder );
        s_newReplace = false;
        s_reqNew = true;
    }

    void DeleteRecipe()
    {
        // The built models keep their collmaps: collmaps\<asset>.map is also variant 1's.
        std::error_code ec;
        fs::remove( s_recipePath, ec );
        if ( ec )
        {
            Log( "Error: cannot delete " + s_recipePath.string() + ": " + ec.message() );
            return;
        }
        Log( "Deleted " + s_recipePath.string() );
        CloseRecipe();
        s_listStale = true;
    }

    void DrawStatus()
    {
        if ( s_jobData )
        {
            static const char spin[] = "|/-\\";
            ImGui::TextColored( ImVec4( 1.0f, 0.82f, 0.5f, 1.0f ), "%s %c", Job().Title().c_str(),
                                spin[( ImGui::GetFrameCount() / 8 ) & 3] );
        }
        else if ( !s_lastError.empty() )
        {
            ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.4f, 1.0f ), "%s", s_lastError.c_str() );
            ImGui::SetItemTooltip( "%s", s_lastError.c_str() );
        }
        else if ( s_open && s_dirty )
            ImGui::TextColored( ImVec4( 1.0f, 0.82f, 0.5f, 1.0f ), "Unsaved changes" );
        else if ( s_open )
            ImGui::TextDisabled( "Saved" );
    }

    void DrawToolbar()
    {
        ImGui::BeginDisabled( Locked() );
        if ( ImGui::Button( "Import model..." ) )
            BeginNew();
        ImGui::SetItemTooltip( "Turn an FBX or OBJ into an xmodel." );
        ImGui::SameLine();
        if ( ImGui::Button( "Import folder..." ) )
        {
            s_reqScan = true;
            if ( !s_haveScan )
                StartScan();
        }
        ImGui::SetItemTooltip( "List the FBX / OBJ files in a folder and import the new ones." );

        ImGui::SameLine( 0.0f, 20.0f );
        if ( s_listStale )
            RefreshRecipeList();
        ImGui::SetNextItemWidth( 260.0f );
        if ( ImGui::BeginCombo( "##recipe", s_open ? s_recipe.asset.c_str() : "Open a model..." ) )
        {
            if ( ImGui::IsWindowAppearing() )
                RefreshRecipeList();
            for ( const std::string &name : s_recipeNames )
                if ( ImGui::Selectable( name.c_str(), s_open && SameText( name, s_recipe.asset ) ) )
                    AfterDiscard( [name]() { OpenRecipe( name ); } );
            if ( s_recipeNames.empty() )
                ImGui::TextDisabled( "Nothing imported yet" );
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip( "Imported models (model_source\\*.kimport)." );

        ImGui::SameLine();
        ImGui::BeginDisabled( !s_open || !s_dirty );
        if ( ImGui::Button( "Save" ) )
            SaveOrBuild( JOB_SAVE );
        ImGui::EndDisabled();
        ImGui::SetItemTooltip( "Save the recipe; with automatic collision, also collmaps\\<asset>.map." );
        ImGui::SameLine();
        ImGui::BeginDisabled( !s_open );
        if ( ImGui::Button( "Build" ) )
            SaveOrBuild( JOB_BUILD );
        ImGui::EndDisabled();
        ImGui::SetItemTooltip( "Save, then build every ticked texture variant." );
        ImGui::SameLine();
        ImGui::BeginDisabled( !s_open || !s_dirty );
        if ( ImGui::Button( "Revert" ) )
            OpenRecipe( s_recipe.asset );
        ImGui::EndDisabled();
        ImGui::SetItemTooltip( "Throw away the unsaved changes." );
        ImGui::SameLine();
        ImGui::BeginDisabled( !s_open );
        if ( ImGui::Button( "Delete..." ) )
            s_reqDelete = true;
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        ImGui::SameLine( 0.0f, 20.0f );
        DrawStatus();
    }

    void DrawViewToolbar()
    {
        bool changed = false;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( "Model" );
        ImGui::SameLine();
        changed |= ImGui::RadioButton( "Solid", &s_modelMode, 0 );
        ImGui::SameLine();
        changed |= ImGui::RadioButton( "Ghost", &s_modelMode, 1 );
        ImGui::SameLine();
        changed |= ImGui::RadioButton( "Hidden", &s_modelMode, 2 );
        ImGui::SameLine( 0.0f, 16.0f );
        changed |= ImGui::Checkbox( "Collision", &s_showHulls );
        ImGui::SameLine();
        changed |= ImGui::Checkbox( "Grid", &s_showGrid );
        ImGui::SameLine( 0.0f, 16.0f );
        ImGui::BeginDisabled( !s_haveMesh );
        if ( ImGui::Button( "Frame" ) )
            FrameModel();
        ImGui::EndDisabled();
        if ( changed )
            s_viewDirty = true;
    }

    void DrawLog( float height )
    {
        ImGui::BeginChild( "##kmilog", ImVec2( 0.0f, height ), ImGuiChildFlags_Borders );
        for ( const std::string &line : s_log )
        {
            if ( !line.compare( 0, 6, "Error:" ) )
                ImGui::TextColored( ImVec4( 1.0f, 0.45f, 0.4f, 1.0f ), "%s", line.c_str() );
            else if ( !line.compare( 0, 8, "Warning:" ) )
                ImGui::TextColored( ImVec4( 1.0f, 0.78f, 0.35f, 1.0f ), "%s", line.c_str() );
            else
                ImGui::TextUnformatted( line.c_str() );
        }
        if ( s_logScroll )
        {
            ImGui::SetScrollHereY( 1.0f );
            s_logScroll = false;
        }
        ImGui::EndChild();
    }

    // A draggable bar between two panes; returns the drag delta along its axis.
    float Splitter( const char *id, bool vertical, float length )
    {
        const float thick = 6.0f;
        ImGui::InvisibleButton( id, vertical ? ImVec2( thick, length ) : ImVec2( length, thick ) );
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if ( hot )
            ImGui::SetMouseCursor( vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS );
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled( vertical ? ImVec2( a.x + 2.0f, a.y ) : ImVec2( a.x, a.y + 2.0f ),
                                                   vertical ? ImVec2( a.x + 4.0f, b.y ) : ImVec2( b.x, a.y + 4.0f ),
                                                   hot ? IM_COL32( 110, 130, 170, 255 ) : IM_COL32( 55, 60, 72, 255 ) );
        if ( !ImGui::IsItemActive() )
            return 0.0f;
        return vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
    }

    void PopupNew()
    {
        if ( !ImGui::BeginPopupModal( "Import model##kmi", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            return;
        ImGui::TextUnformatted( s_newSource.c_str() );
        ImGui::Separator();

        // Importing writes model_source\<asset>.kimport and collmaps\<asset>.map: replacing
        // either one is opt-in.  The name is the one NewRecipe will really use (shortened to
        // fit the variants it finds), so this scans the texture folder and reads the collmap:
        // only when the name, folder or prefix changed.
        static std::string s_checkedKey, s_planned;
        static bool        s_exists = false, s_handMade = false;
        const std::string key = s_newSource + '|' + s_newAsset + '|' + s_newFolder + '|' + s_newPrefix;
        if ( key != s_checkedKey || ImGui::IsWindowAppearing() )
        {
            std::error_code ec;
            s_checkedKey = key;
            s_planned  = ki::PlannedAssetName( s_newSource, NewOptions( s_newAsset, s_newFolder, s_newPrefix ), s_root );
            s_exists   = fs::exists( ki::RecipePath( s_root, s_planned ), ec );
            s_handMade = HandMadeCollmap( s_root, s_planned );
        }
        const std::string &asset = s_planned;

        Label( "Asset name" );
        ImGui::SetNextItemWidth( 340.0f );
        InputString( "##asset", s_newAsset );
        if ( asset != s_newAsset )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "-> %s", asset.c_str() );
        }
        Label( "Surface" );
        SurfaceDot( SurfaceIndex( s_newSurface ) );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 316.0f );
        SurfaceCombo( "##surface", s_newSurface, nullptr );
        ImGui::SetItemTooltip( "Every piece starts as this; change single pieces later." );
        Label( "Units" );
        ImGui::SetNextItemWidth( 340.0f );
        StringCombo( "##units", s_newUnits, KMI_UNITS, KMI_UNIT_LABELS, 4 );
        Label( "Textures" );
        ImGui::SetNextItemWidth( 300.0f );
        InputString( "##folder", s_newFolder );
        ImGui::SameLine();
        if ( ImGui::Button( "...##folder", ImVec2( 34.0f, 0.0f ) ) )
            BrowseFolder( "InstaMAT export folder", s_newFolder );
        Label( "Prefix" );
        ImGui::SetNextItemWidth( 300.0f );
        InputString( "##prefix", s_newPrefix );
        ImGui::SetItemTooltip( "The name InstaMAT put in front of every map." );
        ImGui::SameLine();
        if ( ImGui::Button( "Guess##prefix" ) )
            s_newPrefix = ki::GuessTexturePrefix( s_newSource, s_newFolder );

        if ( s_exists )
            ImGui::TextColored( ImVec4( 1.0f, 0.78f, 0.35f, 1.0f ), "%s is already imported.", asset.c_str() );
        if ( s_handMade )
            ImGui::TextColored( ImVec4( 1.0f, 0.78f, 0.35f, 1.0f ),
                                "collmaps\\%s.map has hand-made collision; importing replaces it.", asset.c_str() );
        if ( s_exists || s_handMade )
            ImGui::Checkbox( "Replace them", &s_newReplace );
        ImGui::Separator();
        ImGui::BeginDisabled( asset.empty() || ( ( s_exists || s_handMade ) && !s_newReplace ) );
        if ( ImGui::Button( "Import", ImVec2( 120.0f, 0.0f ) ) )
        {
            const ki::NewRecipeOptions options = NewOptions( s_newAsset, s_newFolder, s_newPrefix );
            const std::string source = s_newSource;
            RememberSetting( "defaultSurface", s_newSurface );
            RememberSetting( "defaultUnits", s_newUnits );
            AfterDiscard( [source, options]() { StartCreate( source, options ); } );
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 120.0f, 0.0f ) ) )
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    std::string AgeText( double hours )
    {
        char buf[32];
        if ( hours < 1.0 )
            _snprintf( buf, sizeof( buf ), "%.0f min", hours * 60.0 );
        else if ( hours < 48.0 )
            _snprintf( buf, sizeof( buf ), "%.1f h", hours );
        else
            _snprintf( buf, sizeof( buf ), "%.0f days", hours / 24.0 );
        buf[sizeof( buf ) - 1] = '\0';
        return buf;
    }

    void PopupScan()
    {
        const ImVec2 work = ImGui::GetMainViewport()->WorkSize;
        ImGui::SetNextWindowSize( ImVec2( ( std::min )( 900.0f, work.x * 0.9f ), ( std::min )( 520.0f, work.y * 0.8f ) ),
                                  ImGuiCond_Appearing );
        if ( !ImGui::BeginPopupModal( "Import folder##kmi" ) )
            return;
        Label( "Folder" );
        ImGui::SetNextItemWidth( -150.0f );
        InputString( "##folder", s_scanFolder );
        ImGui::SameLine();
        if ( ImGui::Button( "...##scanfolder", ImVec2( 34.0f, 0.0f ) ) && BrowseFolder( "Model export folder", s_scanFolder ) )
            StartScan();
        ImGui::SameLine();
        ImGui::BeginDisabled( s_jobData != nullptr || s_scanFolder.empty() );
        if ( ImGui::Button( "Scan", ImVec2( 90.0f, 0.0f ) ) )
            StartScan();
        ImGui::EndDisabled();
        Label( "Changed within" );
        ImGui::SetNextItemWidth( 120.0f );
        if ( ImGui::InputFloat( "##hours", &s_scanHours, 0.0f, 0.0f, "%.0f hours" ) && s_scanHours < 0.0f )
            s_scanHours = 0.0f;
        ImGui::SetItemTooltip( "0 lists every file." );
        Label( "New imports" );
        SurfaceDot( SurfaceIndex( s_newSurface ) );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 160.0f );
        SurfaceCombo( "##surface", s_newSurface, nullptr );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 130.0f );
        StringCombo( "##units", s_newUnits, KMI_UNITS, KMI_UNIT_LABELS, 4 );
        ImGui::Separator();

        // A source whose collmap was made by hand is never batch-imported over it.
        auto importable = []( size_t i ) {
            return !s_sources[i].hasRecipe && !( i < s_sourceHandMade.size() && s_sourceHandMade[i] );
        };
        int fresh = 0;
        for ( size_t i = 0; i < s_sources.size(); ++i )
            fresh += importable( i ) ? 1 : 0;
        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        if ( s_jobData && s_jobData->kind == JOB_SCAN )
            ImGui::TextDisabled( "Scanning..." );
        else if ( s_haveScan && s_sources.empty() )
            ImGui::TextDisabled( "No FBX or OBJ files here." );
        if ( ImGui::BeginTable( "##sources", 5,
                                ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                                ImGuiTableFlags_SizingStretchProp,
                                ImVec2( 0.0f, -footer ) ) )
        {
            ImGui::TableSetupScrollFreeze( 0, 1 );
            ImGui::TableSetupColumn( "File", ImGuiTableColumnFlags_WidthStretch, 1.4f );
            ImGui::TableSetupColumn( "Age", ImGuiTableColumnFlags_WidthFixed, 70.0f );
            ImGui::TableSetupColumn( "Asset", ImGuiTableColumnFlags_WidthStretch, 1.2f );
            ImGui::TableSetupColumn( "Textures", ImGuiTableColumnFlags_WidthStretch, 1.0f );
            ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthFixed, 70.0f );
            ImGui::TableHeadersRow();
            for ( size_t i = 0; i < s_sources.size(); ++i )
            {
                const ki::SourceEntry &e = s_sources[i];
                ImGui::PushID( (int)i );
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex( 0 );
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted( e.source.filename().string().c_str() );
                ImGui::SetItemTooltip( "%s", e.source.string().c_str() );
                ImGui::TableSetColumnIndex( 1 );
                ImGui::TextUnformatted( AgeText( e.ageHours ).c_str() );
                ImGui::TableSetColumnIndex( 2 );
                ImGui::TextUnformatted( e.asset.c_str() );
                ImGui::TableSetColumnIndex( 3 );
                if ( e.texturePrefixGuess.empty() )
                    ImGui::TextDisabled( "none found" );
                else
                    ImGui::TextUnformatted( e.texturePrefixGuess.c_str() );
                ImGui::TableSetColumnIndex( 4 );
                if ( e.hasRecipe )
                {
                    if ( ImGui::SmallButton( "Open" ) )
                    {
                        const std::string asset = e.asset;
                        AfterDiscard( [asset]() { OpenRecipe( asset ); } );
                        ImGui::CloseCurrentPopup();
                    }
                }
                else if ( !importable( i ) )
                {
                    ImGui::TextDisabled( "Skipped" );
                    ImGui::SetItemTooltip( "collmaps\\%s.map has hand-made collision.  Use Import model... and tick\n"
                                           "Replace to import over it.", e.asset.c_str() );
                }
                else if ( ImGui::SmallButton( "Import" ) )
                {
                    const fs::path source = e.source;
                    const ki::NewRecipeOptions options = NewOptions( "", "", e.texturePrefixGuess );
                    AfterDiscard( [source, options]() { StartCreate( source, options ); } );
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        char label[48];
        _snprintf( label, sizeof( label ), "Import %i new", fresh );
        label[sizeof( label ) - 1] = '\0';
        ImGui::BeginDisabled( fresh == 0 || s_jobData != nullptr );
        if ( ImGui::Button( label, ImVec2( 140.0f, 0.0f ) ) )
        {
            std::vector<std::pair<fs::path, ki::NewRecipeOptions>> batch;
            for ( size_t i = 0; i < s_sources.size(); ++i )
                if ( importable( i ) )
                    batch.push_back( { s_sources[i].source, NewOptions( "", "", s_sources[i].texturePrefixGuess ) } );
            RememberSetting( "defaultSurface", s_newSurface );
            RememberSetting( "defaultUnits", s_newUnits );
            AfterDiscard( [batch]() {
                for ( const auto &item : batch )
                    StartCreate( item.first, item.second );
            } );
            s_haveScan = false;                     // the next open re-lists with the new recipes
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip( "Import every file without a recipe, one after another, with the surface and\n"
                               "units above and the texture prefix each one matched.  Files whose collmap\n"
                               "was made by hand are skipped." );
        ImGui::SameLine();
        if ( ImGui::Button( "Close", ImVec2( 120.0f, 0.0f ) ) )
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void PopupDiscard()
    {
        if ( !ImGui::BeginPopupModal( "Unsaved changes##kmi", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            return;
        ImGui::Text( "%s has unsaved changes.", s_recipe.asset.c_str() );
        ImGui::Separator();
        if ( ImGui::Button( "Save", ImVec2( 110.0f, 0.0f ) ) )
        {
            // Queued jobs run in order: the save lands before whatever comes next.
            SaveOrBuild( JOB_SAVE, std::move( s_pendingAction ) );
            s_pendingAction = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Discard", ImVec2( 110.0f, 0.0f ) ) )
        {
            s_dirty = false;
            if ( s_pendingAction )
                s_pendingAction();
            s_pendingAction = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
        {
            s_pendingAction = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void PopupDelete()
    {
        if ( !ImGui::BeginPopupModal( "Delete recipe##kmi", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            return;
        ImGui::Text( "Delete the import recipe for %s?", s_recipe.asset.c_str() );
        ImGui::TextDisabled( "The built models and their collision stay." );
        ImGui::Separator();
        if ( ImGui::Button( "Delete", ImVec2( 110.0f, 0.0f ) ) )
        {
            DeleteRecipe();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Leaving Hand-edited collision: both ways out can cost the hand edits.
    void PopupLeave()
    {
        if ( !ImGui::BeginPopupModal( "Hand-edited collision##kmi", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            return;
        const bool toAuto = s_leaveTo == "auto";
        if ( toAuto )
        {
            ImGui::Text( "Replace the hand-edited hulls in collmaps\\%s.map with automatic ones?",
                         s_recipe.asset.c_str() );
            ImGui::TextDisabled( "This also saves the recipe." );
        }
        else
        {
            ImGui::Text( "Turn collision off for %s?", s_recipe.asset.c_str() );
            ImGui::TextDisabled( "The next build deletes collmaps\\%s.map and its hand edits.", s_recipe.asset.c_str() );
        }
        ImGui::Separator();
        if ( ImGui::Button( toAuto ? "Regenerate" : "Turn off", ImVec2( 110.0f, 0.0f ) ) )
        {
            if ( toAuto )
                StartJob( MakeJob( JOB_RESET_HULLS ) );
            else
            {
                s_recipe.collision.mode = "none";
                MarkDirty();
                RequestHulls();
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Save / Build with automatic collision over a hand-made collmaps\<asset>.map.
    void PopupHand()
    {
        if ( !ImGui::BeginPopupModal( "Hand-made collision##kmi", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
            return;
        ImGui::Text( "collmaps\\%s.map was edited by hand.", s_recipe.asset.c_str() );
        if ( s_recipe.collision.mode == "none" )
            ImGui::TextDisabled( "With collision off, building deletes it." );
        else
            ImGui::TextDisabled( "Automatic collision would replace it when you %s.",
                                 s_handKind == JOB_BUILD ? "build" : "save" );
        ImGui::Separator();
        if ( ImGui::Button( "Keep it", ImVec2( 110.0f, 0.0f ) ) )
        {
            s_recipe.collision.mode = "manual";
            MarkDirty();
            StartJob( MakeJob( s_handKind ) );
            if ( s_handThen )
                s_handThen();
            s_handThen = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemTooltip( "Switch this model to Hand-edited collision, then %s.",
                               s_handKind == JOB_BUILD ? "build" : "save" );
        ImGui::SameLine();
        if ( ImGui::Button( "Replace it", ImVec2( 110.0f, 0.0f ) ) )
        {
            StartJob( MakeJob( s_handKind ) );
            if ( s_handThen )
                s_handThen();
            s_handThen = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if ( ImGui::Button( "Cancel", ImVec2( 110.0f, 0.0f ) ) )
        {
            s_handThen = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void KiwiModelImport_Draw()
{
    if ( s_init )
        PollJobs();                     // a job finishes even while the window is closed
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_MODELIMPORT );
    if ( !open || !*open )
        return;
    Init();

    // First use: a large floating window over the editor; it docks like any other.
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos( ImVec2( vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f ),
                             ImGuiCond_FirstUseEver, ImVec2( 0.5f, 0.5f ) );
    ImGui::SetNextWindowSize( ImVec2( ( std::min )( 1280.0f, vp->WorkSize.x * 0.9f ),
                                      ( std::min )( 820.0f, vp->WorkSize.y * 0.9f ) ),
                              ImGuiCond_FirstUseEver );
    if ( !ImGui::Begin( KiwiWindows_Title( KIWI_WIN_MODELIMPORT ), open,
                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse ) )
    {
        ImGui::End();
        return;
    }

    DrawToolbar();
    ImGui::Separator();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    s_propWidth = ( std::max )( 260.0f, ( std::min )( s_propWidth, avail.x - 240.0f ) );
    ImGui::BeginChild( "##kmiprops", ImVec2( s_propWidth, avail.y ) );
    ImGui::BeginDisabled( Locked() );
    DrawProperties();
    ImGui::EndDisabled();
    ImGui::EndChild();
    ImGui::SameLine( 0.0f, 0.0f );
    s_propWidth += Splitter( "##kmisplit", true, avail.y );
    ImGui::SameLine( 0.0f, 0.0f );

    ImGui::BeginChild( "##kmiright", ImVec2( 0.0f, avail.y ), ImGuiChildFlags_None,
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse );
    DrawViewToolbar();
    const ImVec2 right = ImGui::GetContentRegionAvail();
    s_logHeight = ( std::max )( 40.0f, ( std::min )( s_logHeight, right.y - 160.0f ) );
    DrawView( right.x, right.y - s_logHeight - 6.0f );
    s_logHeight -= Splitter( "##kmilogsplit", false, right.x );
    DrawLog( 0.0f );
    ImGui::EndChild();

    // Popups live on the window's own ID stack, so they open here.
    if ( s_reqNew )     { ImGui::OpenPopup( "Import model##kmi" );        s_reqNew = false; }
    if ( s_reqScan )    { ImGui::OpenPopup( "Import folder##kmi" );       s_reqScan = false; }
    if ( s_reqDiscard ) { ImGui::OpenPopup( "Unsaved changes##kmi" );     s_reqDiscard = false; }
    if ( s_reqDelete )  { ImGui::OpenPopup( "Delete recipe##kmi" );       s_reqDelete = false; }
    if ( s_reqLeave )   { ImGui::OpenPopup( "Hand-edited collision##kmi" ); s_reqLeave = false; }
    if ( s_reqHand )    { ImGui::OpenPopup( "Hand-made collision##kmi" ); s_reqHand = false; }
    PopupNew();
    PopupScan();
    PopupDiscard();
    PopupDelete();
    PopupLeave();
    PopupHand();
    ImGui::End();
}

void KiwiModelImport_RenderPreview()
{
    if ( !s_init || !s_haveMesh || !KiwiWindows_IsOpen( KIWI_WIN_MODELIMPORT ) )
        return;
    // A hidden tab or collapsed window shows no image, so it needs no render.
    if ( ImGui::GetFrameCount() - s_viewFrame > 2 )
        return;
    const int w = s_viewW, h = s_viewH;
    if ( w < 8 || h < 8 )
        return;
    if ( !s_viewDirty && RTT_PreviewTexture() && w == s_rtW && h == s_rtH )
        return;

    view_t v;
    ViewFor( w, h, v );
    GfxMatrix proj;
    Projection( v, proj );
    if ( !R_Ed_ProjectionWouldBeValid( v.eye, (const float ( * )[3])v.axis, &proj ) )
        return;
    if ( !RTT_BeginPreview( w, h ) )
        return;

    // kiwi_entthumb.cpp RenderThumb's frame, with immediate triangles instead of a model.
    static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    R_BeginFrame();
    R_BeginSharedCmdList();
    R_AddCmdClearScreen( 7, KMI_BG, 1.0f, 0 );     // colour+depth+stencil
    R_AddCmdSetMaterialColor( s_neutral );          // vertex colours, no flat override
    R_Ed_SetSceneParms( v.eye, (const float ( * )[3])v.axis, &proj );
    R_SortMaterials();
    DrawMesh( v );
    DrawGrid();
    DrawHulls( v );
    R_AddCmdSetMaterialColor( s_neutral );
    R_EndFrame();
    R_IssueRenderCommands( (uint)-1 );
    R_SortMaterials();
    RTT_EndPreview();

    s_viewDirty = false;
    s_rtW = w;
    s_rtH = h;
}

void KiwiModelImport_ReleaseForReset()
{
    // The preview RT itself is freed by RTT_ReleaseForReset; the swatches are MANAGED.
    s_viewDirty = true;
    s_rtW = 0;
    s_rtH = 0;
}

void KiwiModelImport_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiWindowModelImport", 0, 0, KIWI_CMD_WINDOW_MODELIMPORT );
}

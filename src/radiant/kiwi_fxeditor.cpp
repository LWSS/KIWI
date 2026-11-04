#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// KIWI (2026-11-02): the FX Editor dock window - see kiwi_fxeditor.h.
// Tabs follow iw3xo-radiant's effects editor (Generation, Size, Velocity, Rotation, Physics,
// Color, Visuals); field meanings follow the game's FX code (src/EffectsCore).

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"            // camera_s

#include <imgui/imgui.h>
#include <imgui_internal.h>     // FindWindowByName / DockId (dock beside the Particles browser)
#include <d3d9.h>
#include <gfx_d3d/r_gfx.h>
#include <gfx_d3d/r_image.h>    // TS_COLOR_MAP
#include <gfx_d3d/r_material.h> // material swatches
#include <map>

#include "kiwi_fxeditor.h"
#include "kiwi_efx.h"
#include "kiwi_fxpreview.h"
#include "kiwi_command.h"
#include "kiwi_particles.h"
#include "kiwi_pick.h"
#include "kiwi_ported.h"
#include "kiwi_str.h"
#include "kiwi_windows.h"
#include "radiant_registry.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

extern void ImGuiShell_FocusTab( const char *title );                       // imgui_shell.cpp:226

namespace
{
    const char *KFE_PROFILE   = "KiwiFxEditor";
    const char *KFE_NEW_NAME  = "__kiwi_fxedit_new";     // preview key of an unsaved new effect
    const int   KFE_UNDO_MAX  = 64;
    const float KFE_NUM_W     = 64.0f;                   // one number field
    const float KFE_CURVE_W   = 300.0f;
    const float KFE_CURVE_H   = 88.0f;

    // ── document state ────────────────────────────────────────────────────────
    std::string s_name;                 // "" = new, unsaved
    kefxDoc_t   s_doc;
    kefxDoc_t   s_committed;            // the document before the current edit gesture
    bool        s_dirty    = false;
    bool        s_gesture  = false;
    bool        s_pushPreview = false;
    int         s_sel      = 0;
    std::vector<kefxDoc_t> s_undo, s_redo;
    std::string s_status;
    std::string s_lastFollowed;
    bool        s_follow   = true;
    bool        s_loaded   = false;
    int         s_velFrame = 0, s_velAxis = 0;
    int         s_forceTab = -1;        // test DSL: select this tab next frame
    int         s_gradStop[2] = { 0, 0 };
    std::string s_pendingOpen;          // waits for "discard changes?"
    bool        s_askDiscard = false;
    char        s_saveAs[128] = "";
    bool        s_overwriteArmed = false;
    char        s_renameBuf[48] = "";
    int         s_renaming = -1;
    bool        s_renameFocus = false;

    void LoadPrefs()
    {
        if ( s_loaded )
            return;
        s_loaded = true;
        s_follow = Radiant_ProfileGetInt( KFE_PROFILE, "FollowSelection", 1 ) != 0;
    }

    const char *PreviewName()
    {
        return s_name.empty() ? KFE_NEW_NAME : s_name.c_str();
    }

    void PushPreview()
    {
        KiwiFxPreview_SetOverride( PreviewName(), &s_doc );
        s_pushPreview = false;
    }

    // Every widget that changes s_doc calls this.  The first change of a gesture snapshots
    // the document for Undo; EndFrame closes the gesture once no widget is active.
    void Changed()
    {
        if ( !s_gesture )
        {
            s_undo.push_back( s_committed );
            if ( (int)s_undo.size() > KFE_UNDO_MAX )
                s_undo.erase( s_undo.begin() );
            s_redo.clear();
            s_gesture = true;
        }
        s_dirty = true;
        s_pushPreview = true;
    }

    void EndFrame()
    {
        if ( s_gesture && !ImGui::IsAnyItemActive() )
        {
            s_gesture = false;
            s_committed = s_doc;
        }
        if ( s_pushPreview )
            PushPreview();
    }

    void ClampSel()
    {
        if ( s_sel >= (int)s_doc.elems.size() ) s_sel = (int)s_doc.elems.size() - 1;
        if ( s_sel < 0 ) s_sel = 0;
    }

    void Adopt( const std::string &name, const kefxDoc_t &doc )
    {
        // The old effect's preview goes back to its file.
        if ( !s_name.empty() || !s_doc.elems.empty() )
            KiwiFxPreview_SetOverride( PreviewName(), nullptr );
        s_name = name;
        s_doc = doc;
        s_committed = doc;
        s_dirty = false;
        s_gesture = false;
        s_undo.clear();
        s_redo.clear();
        s_sel = 0;
        s_renaming = -1;
        s_overwriteArmed = false;
        _snprintf( s_saveAs, sizeof( s_saveAs ), "%s", name.c_str() );
        s_saveAs[sizeof( s_saveAs ) - 1] = '\0';
        PushPreview();
    }

    bool LoadNamed( const std::string &name )
    {
        kefxDoc_t doc;
        std::string err;
        if ( !KiwiEfx_Load( name.c_str(), doc, err ) )
        {
            Sys_Printf( "FX Editor: %s\n", err.c_str() );
            s_status = err;
            return false;
        }
        Adopt( name, doc );
        s_status = "Opened raw/fx/" + name + ".efx";
        return true;
    }

    std::string CleanName( const char *in )
    {
        std::string s = in ? in : "";
        for ( size_t i = 0; i < s.size(); ++i )
            if ( s[i] == '\\' ) s[i] = '/';
        while ( !s.empty() && ( s[0] == '/' || s[0] == ' ' ) ) s.erase( 0, 1 );
        while ( !s.empty() && ( s[s.size() - 1] == ' ' || s[s.size() - 1] == '/' ) ) s.erase( s.size() - 1 );
        if ( s.size() > 4 && !_stricmp( s.c_str() + s.size() - 4, ".efx" ) ) s.erase( s.size() - 4 );
        for ( size_t i = 0; i < s.size(); ++i )
        {
            const char c = s[i];
            if ( !( ( c >= 'a' && c <= 'z' ) || ( c >= 'A' && c <= 'Z' ) || ( c >= '0' && c <= '9' ) || c == '_' || c == '-' || c == '/' ) )
                return std::string();
        }
        if ( s.find( "//" ) != std::string::npos )
            return std::string();
        return s;
    }

    void PlayTest();

    bool SaveTo( const std::string &name )
    {
        std::string err;
        if ( !KiwiEfx_Save( name.c_str(), s_doc, err ) )
        {
            s_status = "Save failed: " + err;
            Sys_Printf( "FX Editor: %s\n", s_status.c_str() );
            return false;
        }
        bool replay = false;
        if ( name != s_name )
        {
            // The test instance played the old name; it restarts under the new one.
            replay = KiwiFxPreview_TestPlaying();
            KiwiFxPreview_SetOverride( PreviewName(), nullptr );
            s_name = name;
        }
        s_dirty = false;
        KiwiParticles_Rescan();
        KiwiFxPreview_ThumbInvalidate( name.c_str() );   // its browser tile re-renders
        PushPreview();
        if ( replay )
            PlayTest();
        s_status = "Saved raw/fx/" + name + ".efx";
        Sys_Printf( "FX Editor: saved raw/fx/%s.efx (%i element%s).\n", name.c_str(), (int)s_doc.elems.size(),
                    s_doc.elems.size() == 1 ? "" : "s" );
        return true;
    }

    void Undo()
    {
        if ( s_undo.empty() )
            return;
        s_redo.push_back( s_doc );
        s_doc = s_undo.back();
        s_undo.pop_back();
        s_committed = s_doc;
        s_dirty = true;
        ClampSel();
        PushPreview();
    }

    void Redo()
    {
        if ( s_redo.empty() )
            return;
        s_undo.push_back( s_doc );
        s_doc = s_redo.back();
        s_redo.pop_back();
        s_committed = s_doc;
        s_dirty = true;
        ClampSel();
        PushPreview();
    }

    // Plays the document in front of the camera (where a drop at the view centre would land).
    void PlayTest()
    {
        camera_s *cam = Ed_Camera();
        if ( !cam )
            return;
        float origin[3];
        ray_t ray;
        float dist = 0.0f, n[3];
        if ( Pick_RayFromImagePos( cam->width / 2, cam->height / 2, &ray ) && Pick_TraceSolid( ray.origin, ray.dir, &dist, n )
          && dist < 2048.0f )
            for ( int k = 0; k < 3; ++k ) origin[k] = ray.origin[k] + ray.dir[k] * dist;
        else
            for ( int k = 0; k < 3; ++k ) origin[k] = cam->origin[k] + cam->vpn[k] * 200.0f;
        const float up[3] = { 0.0f, 0.0f, 1.0f };
        PushPreview();
        KiwiFxPreview_PlayTest( PreviewName(), origin, up );
    }

    // ── asset lists (effects, effect materials, models) ───────────────────────
    enum { KIND_FX, KIND_MATERIAL, KIND_MODEL, KIND_COUNT };
    std::vector<std::string> s_assets[KIND_COUNT];
    bool s_assetsScanned[KIND_COUNT] = { false, false, false };
    char s_pickFilter[96] = "";
    bool s_pickAllMaterials = false;

    std::string ExeDir()
    {
        char exe[MAX_PATH] = { 0 };
        ::GetModuleFileNameA( nullptr, exe, MAX_PATH );
        char *slash = strrchr( exe, '\\' );
        if ( slash )
            *slash = '\0';
        return exe;
    }

    void ScanDir( const std::string &dir, const std::string &rel, const char *ext, int depth, std::vector<std::string> &out )
    {
        if ( depth > 8 )
            return;
        WIN32_FIND_DATAA fd;
        HANDLE h = ::FindFirstFileA( ( dir + "\\*" ).c_str(), &fd );
        if ( h == INVALID_HANDLE_VALUE )
            return;
        do
        {
            const char *n = fd.cFileName;
            if ( !strcmp( n, "." ) || !strcmp( n, ".." ) )
                continue;
            if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
            {
                if ( ext )
                    ScanDir( dir + "\\" + n, rel.empty() ? std::string( n ) : rel + "/" + n, ext, depth + 1, out );
                continue;
            }
            std::string name = n;
            if ( ext )
            {
                const size_t len = strlen( ext );
                if ( name.size() <= len || _stricmp( name.c_str() + name.size() - len, ext ) )
                    continue;
                name.erase( name.size() - len );
            }
            out.push_back( rel.empty() ? name : rel + "/" + name );
        }
        while ( ::FindNextFileA( h, &fd ) );
        ::FindClose( h );
    }

    bool NoCaseLess( const std::string &a, const std::string &b ) { return _stricmp( a.c_str(), b.c_str() ) < 0; }

    // An effect material's colour map for the panel (null = not found).  Material handles live
    // as long as the editor, so only they are cached; the texture is read every draw (device
    // resets rebuild textures).
    IDirect3DTexture9 *ColorMapOf( const std::string &name )
    {
        static std::map<std::string, Material *> s_mats;
        std::map<std::string, Material *>::iterator it = s_mats.find( name );
        if ( it == s_mats.end() )
        {
            Material *m = name.empty() ? nullptr : Material_RegisterHandle( name.c_str(), IMAGE_TRACK_FX );
            if ( m && Material_IsDefault( m ) )
                m = nullptr;
            it = s_mats.insert( std::make_pair( name, m ) ).first;
        }
        const Material *mt = it->second ? Material_FromHandle( it->second ) : nullptr;
        for ( int t = 0; mt && mt->textureTable && t < (int)mt->textureCount; ++t )
            if ( mt->textureTable[t].semantic == TS_COLOR_MAP && mt->textureTable[t].u.image )
                return mt->textureTable[t].u.image->texture.map;
        return nullptr;
    }

    // A swatch item: the colour map over a checkerboard (effect textures are mostly alpha).
    void Swatch( const std::string &name, float size )
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy( ImVec2( size, size ) );
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const float c = size / 4.0f;
        for ( int y = 0; y < 4; ++y )
            for ( int x = 0; x < 4; ++x )
                dl->AddRectFilled( ImVec2( p.x + x * c, p.y + y * c ), ImVec2( p.x + ( x + 1 ) * c, p.y + ( y + 1 ) * c ),
                                   ( ( x + y ) & 1 ) ? IM_COL32( 70, 70, 70, 255 ) : IM_COL32( 45, 45, 45, 255 ) );
        if ( IDirect3DTexture9 *tex = ColorMapOf( name ) )
            dl->AddImage( (ImTextureID)(intptr_t)tex, p, ImVec2( p.x + size, p.y + size ) );
        else
            dl->AddText( ImVec2( p.x + 4.0f, p.y + size * 0.5f - 7.0f ), IM_COL32( 255, 120, 80, 255 ), "?" );
        dl->AddRect( p, ImVec2( p.x + size, p.y + size ), IM_COL32( 255, 255, 255, 40 ) );
    }

    const std::vector<std::string> &Assets( int kind )
    {
        if ( !s_assetsScanned[kind] )
        {
            s_assetsScanned[kind] = true;
            std::vector<std::string> &v = s_assets[kind];
            v.clear();
            const std::string raw = ExeDir() + "\\raw\\";
            if ( kind == KIND_FX )            ScanDir( raw + "fx", "", ".efx", 0, v );
            else if ( kind == KIND_MATERIAL ) ScanDir( raw + "materials", "", nullptr, 0, v );
            else                              ScanDir( raw + "xmodel", "", nullptr, 0, v );
            std::sort( v.begin(), v.end(), NoCaseLess );
        }
        return s_assets[kind];
    }

    // A popup list with a search box; true when `value` was picked.  Effect materials are the
    // "gfx_*" ones unless "All materials" is ticked.
    bool AssetPicker( const char *popupId, int kind, std::string &value )
    {
        bool picked = false;
        ImGui::SetNextWindowSize( ImVec2( 340.0f, 380.0f ), ImGuiCond_Appearing );
        if ( !ImGui::BeginPopup( popupId ) )
            return false;
        if ( ImGui::IsWindowAppearing() )
        {
            s_pickFilter[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth( 200.0f );
        ImGui::InputTextWithHint( "##pickfilter", "search", s_pickFilter, sizeof( s_pickFilter ) );
        if ( kind == KIND_MATERIAL )
        {
            ImGui::SameLine();
            ImGui::Checkbox( "All materials", &s_pickAllMaterials );
        }
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Rescan" ) )
            s_assetsScanned[kind] = false;
        const std::vector<std::string> &all = Assets( kind );
        std::vector<int> rows;
        for ( size_t i = 0; i < all.size(); ++i )
        {
            const std::string &n = all[i];
            if ( kind == KIND_MATERIAL && !s_pickAllMaterials && _strnicmp( n.c_str(), "gfx", 3 ) )
                continue;
            if ( s_pickFilter[0] && !KiwiStr_ContainsNoCase( n.c_str(), s_pickFilter ) )
                continue;
            rows.push_back( (int)i );
        }
        ImGui::BeginChild( "##picklist", ImVec2( 0.0f, 0.0f ), ImGuiChildFlags_Borders );
        ImGuiListClipper clip;
        clip.Begin( (int)rows.size() );
        while ( clip.Step() )
            for ( int r = clip.DisplayStart; r < clip.DisplayEnd; ++r )
            {
                const std::string &n = all[rows[r]];
                if ( ImGui::Selectable( n.c_str(), !_stricmp( n.c_str(), value.c_str() ) ) )
                {
                    value = n;
                    picked = true;
                    ImGui::CloseCurrentPopup();
                }
                if ( kind == KIND_MATERIAL && ImGui::IsItemHovered() )
                {
                    ImGui::BeginTooltip();      // loads only the hovered material
                    Swatch( n, 128.0f );
                    ImGui::EndTooltip();
                }
            }
        if ( rows.empty() )
            ImGui::TextDisabled( "Nothing matches." );
        ImGui::EndChild();
        ImGui::EndPopup();
        return picked;
    }

    // ── small widgets ─────────────────────────────────────────────────────────
    void Tip( const char *text )
    {
        if ( text && ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            ImGui::SetTooltip( "%s", text );
    }

    // "label   [base] + [random]": value = base + random 0..1 * amp, the file's two numbers.
    bool RangeRow( const char *label, kefxRange_t &r, bool isInt, float speed, const char *tip, float lo = -1e9f, float hi = 1e9f )
    {
        bool ch = false;
        ImGui::PushID( label );
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( label );
        Tip( tip );
        ImGui::SameLine( 132.0f );
        ImGui::SetNextItemWidth( KFE_NUM_W );
        if ( isInt )
        {
            int b = (int)floorf( r.base + 0.5f ), a = (int)floorf( r.amp + 0.5f );
            if ( ImGui::DragInt( "##b", &b, speed, (int)lo, (int)hi ) ) { r.base = (float)b; ch = true; }
            Tip( tip );
            ImGui::SameLine();
            ImGui::TextDisabled( "+" );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( KFE_NUM_W );
            if ( ImGui::DragInt( "##a", &a, speed, -1000000, 1000000 ) ) { r.amp = (float)a; ch = true; }
        }
        else
        {
            if ( ImGui::DragFloat( "##b", &r.base, speed, lo, hi, "%g" ) ) ch = true;
            Tip( tip );
            ImGui::SameLine();
            ImGui::TextDisabled( "+" );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( KFE_NUM_W );
            if ( ImGui::DragFloat( "##a", &r.amp, speed, -1e6f, 1e6f, "%g" ) ) ch = true;
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Random extra, 0 to this value per particle." );
        ImGui::PopID();
        if ( ch ) Changed();
        return ch;
    }

    bool IntRow( const char *label, int &v, int lo, int hi, const char *tip )
    {
        ImGui::PushID( label );
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( label );
        Tip( tip );
        ImGui::SameLine( 132.0f );
        ImGui::SetNextItemWidth( KFE_NUM_W );
        const bool ch = ImGui::DragInt( "##v", &v, 0.5f, lo, hi );
        Tip( tip );
        ImGui::PopID();
        if ( ch ) Changed();
        return ch;
    }

    bool FloatRow( const char *label, float &v, float speed, float lo, float hi, const char *tip )
    {
        ImGui::PushID( label );
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( label );
        Tip( tip );
        ImGui::SameLine( 132.0f );
        ImGui::SetNextItemWidth( KFE_NUM_W );
        const bool ch = ImGui::DragFloat( "##v", &v, speed, lo, hi, "%g" );
        Tip( tip );
        ImGui::PopID();
        if ( ch ) Changed();
        return ch;
    }

    bool Flag( const char *label, unsigned &word, unsigned bit, const char *tip )
    {
        bool on = ( word & bit ) != 0;
        const bool ch = ImGui::Checkbox( label, &on );
        Tip( tip );
        if ( ch )
        {
            word = on ? ( word | bit ) : ( word & ~bit );
            Changed();
        }
        return ch;
    }

    // One flag group (mask) as a combo of its values.
    bool FlagCombo( const char *label, unsigned &word, unsigned mask, const char *const *names, const unsigned *values,
                    int count, const char *tip, float width = 130.0f )
    {
        int cur = 0;
        for ( int i = 0; i < count; ++i )
            if ( ( word & mask ) == values[i] ) cur = i;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( label );
        Tip( tip );
        ImGui::SameLine( 132.0f );
        ImGui::SetNextItemWidth( width );
        ImGui::PushID( label );
        const bool ch = ImGui::Combo( "##c", &cur, names, count );
        ImGui::PopID();
        Tip( tip );
        if ( ch )
        {
            word = ( word & ~mask ) | values[cur];
            Changed();
        }
        return ch;
    }

    // ── curve editor ──────────────────────────────────────────────────────────
    // Keys are squares; drag to move (the end keys stay at t = 0 / 1), double-click empty
    // space to add a key to the bright curve, right-click a key to delete it.  The dim
    // curve is the random-range partner (curve 1).
    kefxCurve_t *s_dragCurve = nullptr;
    int          s_dragKey   = -1;

    bool CurveEditor( const char *id, kefxCurve_t *c0, kefxCurve_t *c1, float lo, float hi )
    {
        ImGui::PushID( id );
        const ImVec2 size( KFE_CURVE_W, KFE_CURVE_H );
        const ImVec2 mn = ImGui::GetCursorScreenPos();
        const ImVec2 mx( mn.x + size.x, mn.y + size.y );
        ImGui::InvisibleButton( "##area", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight );
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled( mn, mx, IM_COL32( 24, 26, 30, 255 ), 3.0f );
        for ( int g = 1; g < 4; ++g )
        {
            const float x = mn.x + size.x * (float)g / 4.0f, y = mn.y + size.y * (float)g / 4.0f;
            dl->AddLine( ImVec2( x, mn.y ), ImVec2( x, mx.y ), IM_COL32( 255, 255, 255, 18 ) );
            dl->AddLine( ImVec2( mn.x, y ), ImVec2( mx.x, y ), IM_COL32( 255, 255, 255, 18 ) );
        }
        if ( lo < 0.0f && hi > 0.0f )
        {
            const float y = mx.y - ( 0.0f - lo ) / ( hi - lo ) * size.y;
            dl->AddLine( ImVec2( mn.x, y ), ImVec2( mx.x, y ), IM_COL32( 255, 255, 255, 50 ) );
        }
        dl->AddRect( mn, mx, IM_COL32( 255, 255, 255, 40 ), 3.0f );

        auto toScreen = [&]( float t, float v ) -> ImVec2
        {
            return ImVec2( mn.x + t * size.x, mx.y - ( v - lo ) / ( hi - lo ) * size.y );
        };
        kefxCurve_t *curves[2] = { c0, c1 };
        const ImU32 cols[2] = { IM_COL32( 255, 170, 60, 255 ), IM_COL32( 120, 180, 255, 200 ) };
        for ( int ci = 1; ci >= 0; --ci )
        {
            kefxCurve_t *c = curves[ci];
            if ( !c ) continue;
            const int n = c->Count( 1 );
            for ( int k = 1; k < n; ++k )
                dl->AddLine( toScreen( c->keys[( k - 1 ) * 2], c->keys[( k - 1 ) * 2 + 1] ),
                             toScreen( c->keys[k * 2], c->keys[k * 2 + 1] ), cols[ci], 1.5f );
            for ( int k = 0; k < n; ++k )
            {
                const ImVec2 p = toScreen( c->keys[k * 2], c->keys[k * 2 + 1] );
                const bool drag = s_dragCurve == c && s_dragKey == k;
                dl->AddRectFilled( ImVec2( p.x - 3, p.y - 3 ), ImVec2( p.x + 3, p.y + 3 ), drag ? IM_COL32( 255, 255, 255, 255 ) : cols[ci] );
            }
        }

        bool changed = false;
        const ImVec2 m = ImGui::GetIO().MousePos;
        const float mt = ( std::max )( 0.0f, ( std::min )( 1.0f, ( m.x - mn.x ) / size.x ) );
        const float mv = ( std::max )( lo, ( std::min )( hi, lo + ( mx.y - m.y ) / size.y * ( hi - lo ) ) );
        // Nearest key under the mouse.
        kefxCurve_t *hitC = nullptr;
        int hitK = -1;
        float best = 7.0f * 7.0f;
        for ( int ci = 0; ci < 2; ++ci )
        {
            kefxCurve_t *c = curves[ci];
            if ( !c ) continue;
            for ( int k = 0; k < c->Count( 1 ); ++k )
            {
                const ImVec2 p = toScreen( c->keys[k * 2], c->keys[k * 2 + 1] );
                const float d = ( p.x - m.x ) * ( p.x - m.x ) + ( p.y - m.y ) * ( p.y - m.y );
                if ( d < best ) { best = d; hitC = c; hitK = k; }
            }
        }
        if ( hovered && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            s_dragCurve = hitC;
            s_dragKey = hitK;
            if ( !hitC && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) && c0
              && c0->Count( 1 ) < KEFX_MAX_CURVE_KEYS && mt > 0.0f && mt < 1.0f )
            {
                int at = 0;
                while ( at < c0->Count( 1 ) && c0->keys[at * 2] < mt ) ++at;
                if ( at > 0 && at < c0->Count( 1 ) && c0->keys[at * 2] - mt > 1e-3f && mt - c0->keys[( at - 1 ) * 2] > 1e-3f )
                {
                    c0->keys.insert( c0->keys.begin() + at * 2, 2, 0.0f );
                    c0->keys[at * 2] = mt;
                    c0->keys[at * 2 + 1] = mv;
                    s_dragCurve = c0;
                    s_dragKey = at;
                    changed = true;
                }
            }
        }
        if ( active && s_dragCurve && ( s_dragCurve == c0 || s_dragCurve == c1 ) && s_dragKey >= 0
          && ImGui::IsMouseDragging( ImGuiMouseButton_Left, 0.0f ) )
        {
            kefxCurve_t *c = s_dragCurve;
            const int n = c->Count( 1 ), k = s_dragKey;
            if ( k < n )
            {
                float t = c->keys[k * 2];
                if ( k > 0 && k < n - 1 )
                    t = ( std::max )( c->keys[( k - 1 ) * 2] + 0.001f, ( std::min )( c->keys[( k + 1 ) * 2] - 0.001f, mt ) );
                if ( c->keys[k * 2] != t || c->keys[k * 2 + 1] != mv )
                {
                    c->keys[k * 2] = t;
                    c->keys[k * 2 + 1] = mv;
                    changed = true;
                }
            }
        }
        if ( !ImGui::IsMouseDown( ImGuiMouseButton_Left ) && ( s_dragCurve == c0 || s_dragCurve == c1 ) && !active )
        {
            s_dragCurve = nullptr;
            s_dragKey = -1;
        }
        if ( hovered && ImGui::IsMouseClicked( ImGuiMouseButton_Right ) && hitC && hitK > 0 && hitK < hitC->Count( 1 ) - 1 )
        {
            hitC->keys.erase( hitC->keys.begin() + hitK * 2, hitC->keys.begin() + hitK * 2 + 2 );
            changed = true;
        }
        if ( hovered && hitC && hitK >= 0 )
            ImGui::SetTooltip( "t %.3f   value %.3f", hitC->keys[hitK * 2], hitC->keys[hitK * 2 + 1] );
        else if ( hovered )
            ImGui::SetTooltip( "Double-click to add a key; drag keys; right-click deletes.\nOrange = curve, blue = random-range partner." );
        ImGui::PopID();
        if ( changed ) Changed();
        return changed;
    }

    // A graph: the scale, the random toggle that brings in curve 1, the curve editor.
    void GraphBlock( const char *label, kefxGraph_t &g, kefxElem_t &e, unsigned *randWord, unsigned randBit,
                     const char *scaleTip )
    {
        ImGui::PushID( label );
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( label );
        ImGui::SameLine( 132.0f );
        ImGui::SetNextItemWidth( KFE_NUM_W );
        if ( ImGui::DragFloat( "##scale", &g.scale, 0.5f, -1e6f, 1e6f, "%g" ) )
            Changed();
        Tip( scaleTip );
        bool rnd = false;
        if ( randWord )
        {
            ImGui::SameLine();
            rnd = ( *randWord & randBit ) != 0;
            if ( ImGui::Checkbox( "Random range", &rnd ) )
            {
                *randWord = rnd ? ( *randWord | randBit ) : ( *randWord & ~randBit );
                Changed();
            }
            Tip( "Each particle picks a value between the orange and the blue curve." );
        }
        float lo, hi;
        KiwiEfx_GraphRange( &g, e, &lo, &hi );
        CurveEditor( "##curve", &g.curve[0], rnd ? &g.curve[1] : nullptr, lo, hi );
        ImGui::PopID();
    }

    // Colour gradient over life: stops below the bar; click selects, drag moves, double-click
    // adds, right-click deletes (not the end stops).
    void GradientBlock( const char *id, kefxCurve_t &c, int &selStop )
    {
        ImGui::PushID( id );
        const ImVec2 size( KFE_CURVE_W, 22.0f );
        const ImVec2 mn = ImGui::GetCursorScreenPos();
        const ImVec2 mx( mn.x + size.x, mn.y + size.y );
        ImGui::InvisibleButton( "##grad", ImVec2( size.x, size.y + 10.0f ), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight );
        const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const int n = c.Count( 3 );
        auto colAt = [&]( int k ) -> ImU32
        {
            return ImGui::ColorConvertFloat4ToU32( ImVec4( c.keys[k * 4 + 1], c.keys[k * 4 + 2], c.keys[k * 4 + 3], 1.0f ) );
        };
        for ( int k = 1; k < n; ++k )
        {
            const float x0 = mn.x + c.keys[( k - 1 ) * 4] * size.x, x1 = mn.x + c.keys[k * 4] * size.x;
            dl->AddRectFilledMultiColor( ImVec2( x0, mn.y ), ImVec2( x1, mx.y ), colAt( k - 1 ), colAt( k ), colAt( k ), colAt( k - 1 ) );
        }
        dl->AddRect( mn, mx, IM_COL32( 255, 255, 255, 60 ) );
        if ( selStop >= n ) selStop = n - 1;
        int hit = -1;
        const ImVec2 m = ImGui::GetIO().MousePos;
        for ( int k = 0; k < n; ++k )
        {
            const float x = mn.x + c.keys[k * 4] * size.x;
            const ImU32 edge = k == selStop ? IM_COL32( 255, 255, 255, 255 ) : IM_COL32( 140, 140, 140, 255 );
            dl->AddTriangleFilled( ImVec2( x, mx.y ), ImVec2( x - 5, mx.y + 9 ), ImVec2( x + 5, mx.y + 9 ), colAt( k ) );
            dl->AddTriangle( ImVec2( x, mx.y ), ImVec2( x - 5, mx.y + 9 ), ImVec2( x + 5, mx.y + 9 ), edge );
            if ( fabsf( m.x - x ) < 6.0f && m.y > mn.y && m.y < mx.y + 10.0f )
                hit = k;
        }
        bool changed = false;
        const float mt = ( std::max )( 0.0f, ( std::min )( 1.0f, ( m.x - mn.x ) / size.x ) );
        if ( hovered && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            if ( hit >= 0 )
                selStop = hit;
            else if ( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) && n < KEFX_MAX_CURVE_KEYS )
            {
                int at = 0;
                while ( at < n && c.keys[at * 4] < mt ) ++at;
                if ( at > 0 && at < n && c.keys[at * 4] - mt > 1e-3f && mt - c.keys[( at - 1 ) * 4] > 1e-3f )
                {
                    float v[4] = { mt, KiwiEfx_Sample( c, 3, mt, 0 ), KiwiEfx_Sample( c, 3, mt, 1 ), KiwiEfx_Sample( c, 3, mt, 2 ) };
                    c.keys.insert( c.keys.begin() + at * 4, v, v + 4 );
                    selStop = at;
                    changed = true;
                }
            }
        }
        if ( active && ImGui::IsMouseDragging( ImGuiMouseButton_Left, 1.0f ) && selStop > 0 && selStop < c.Count( 3 ) - 1 )
        {
            const float t = ( std::max )( c.keys[( selStop - 1 ) * 4] + 0.001f, ( std::min )( c.keys[( selStop + 1 ) * 4] - 0.001f, mt ) );
            if ( t != c.keys[selStop * 4] ) { c.keys[selStop * 4] = t; changed = true; }
        }
        if ( hovered && ImGui::IsMouseClicked( ImGuiMouseButton_Right ) && hit > 0 && hit < n - 1 )
        {
            c.keys.erase( c.keys.begin() + hit * 4, c.keys.begin() + hit * 4 + 4 );
            if ( selStop >= hit && selStop > 0 ) --selStop;
            changed = true;
        }
        if ( hovered && hit < 0 )
            ImGui::SetTooltip( "Double-click to add a colour stop; drag stops; right-click deletes." );
        if ( selStop >= 0 && selStop < c.Count( 3 ) )
        {
            ImGui::SetNextItemWidth( 200.0f );
            if ( ImGui::ColorEdit3( "##stop", &c.keys[selStop * 4 + 1], ImGuiColorEditFlags_Float ) )
                changed = true;
            ImGui::SameLine();
            ImGui::TextDisabled( "at %.2f", c.keys[selStop * 4] );
        }
        ImGui::PopID();
        if ( changed ) Changed();
    }

    // ── tabs ──────────────────────────────────────────────────────────────────
    void TabGeneration( kefxElem_t &e )
    {
        ImGui::SeparatorText( "Spawning" );
        int mode = e.Looping() ? 1 : 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( "Mode" );
        ImGui::SameLine( 132.0f );
        if ( ImGui::RadioButton( "One-shot", mode == 0 ) ) { e.editorFlags &= ~KEFX_E_LOOPING; Changed(); }
        ImGui::SameLine();
        if ( ImGui::RadioButton( "Looping", mode == 1 ) ) { e.editorFlags |= KEFX_E_LOOPING; Changed(); }
        if ( e.Looping() )
        {
            IntRow( "Interval (ms)", e.loopIntervalMsec, 1, 600000, "One particle every this many milliseconds." );
            IntRow( "Count", e.loopCount, 0, 1000000, "How many particles in all; 0 = forever." );
        }
        else
            RangeRow( "Count", e.oneShotCount, true, 0.2f, "Particles spawned at once.", 0.0f, 1000.0f );
        RangeRow( "Life (ms)", e.lifeSpanMsec, true, 5.0f, "How long each particle lives.", 0.0f, 600000.0f );
        RangeRow( "Delay (ms)", e.spawnDelayMsec, true, 5.0f, "Wait after the effect starts before each spawn.", 0.0f, 600000.0f );

        ImGui::SeparatorText( "Origin" );
        Flag( "Relative to effect axis", e.flags, KEFX_F_SPAWNRELATIVE,
              "On: Forward/Left/Up follow the effect's aim (the FX entity's arrow).\nOff: world X/Y/Z." );
        const bool rel = ( e.flags & KEFX_F_SPAWNRELATIVE ) != 0;
        RangeRow( rel ? "Forward" : "X", e.spawnOrg[0], false, 0.5f, "Spawn offset along the first axis." );
        RangeRow( rel ? "Left" : "Y", e.spawnOrg[1], false, 0.5f, "Spawn offset along the second axis." );
        RangeRow( rel ? "Up" : "Z", e.spawnOrg[2], false, 0.5f, "Spawn offset along the third axis." );
        {
            static const char *const names[] = { "None", "Sphere", "Cylinder" };
            static const unsigned values[] = { 0u, (unsigned)KEFX_F_OFFSET_SPHERE, (unsigned)KEFX_F_OFFSET_CYLINDER };
            FlagCombo( "Offset", e.flags, KEFX_F_OFFSET_MASK, names, values, 3,
                       "Scatter spawns over a sphere, or a cylinder around the forward axis." );
        }
        if ( e.flags & KEFX_F_OFFSET_MASK )
        {
            RangeRow( "Radius", e.spawnOffsetRadius, false, 0.5f, "Distance from the origin." );
            if ( ( e.flags & KEFX_F_OFFSET_MASK ) == KEFX_F_OFFSET_CYLINDER )
                RangeRow( "Height", e.spawnOffsetHeight, false, 0.5f, "Along the forward axis." );
        }
        {
            static const char *const names[] = { "World", "Spawn", "Effect", "Offset" };
            static const unsigned values[] = { 0u, (unsigned)KEFX_F_RUN_SPAWN, (unsigned)KEFX_F_RUN_EFFECT, (unsigned)KEFX_F_RUN_OFFSET };
            FlagCombo( "Run relative to", e.flags, KEFX_F_RUN_MASK, names, values, 4,
                       "The frame particles move in.  Effect: they follow a moving effect.\n"
                       "Offset: forward points away from the origin (bursts, sparks)." );
        }

        ImGui::SeparatorText( "Culling and fading" );
        RangeRow( "Spawn range", e.spawnRange, false, 5.0f, "Camera distance band in which particles spawn (start, length)." );
        RangeRow( "Fade in", e.fadeInRange, false, 5.0f, "Fade in from this camera distance over the length (0 = off)." );
        RangeRow( "Fade out", e.fadeOutRange, false, 5.0f, "Fade out from this camera distance over the length (0 = off)." );
        Flag( "Frustum cull", e.flags, KEFX_F_FRUSTUMCULL, "Skip spawning when this radius is off screen." );
        if ( e.flags & KEFX_F_FRUSTUMCULL )
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth( KFE_NUM_W );
            if ( ImGui::DragFloat( "##cullr", &e.spawnFrustumCullRadius, 1.0f, 0.0f, 100000.0f, "%g" ) )
                Changed();
            Tip( "Cull radius." );
        }

        ImGui::SeparatorText( "Misc" );
        IntRow( "Sort order", e.sortOrder, 0, 255, "Draw order among effects; higher draws first (behind)." );
        unsigned &f = e.flags;
        Flag( "Draw past fog", f, 0x400, "Not hidden by the far plane / fog." );
        ImGui::SameLine();
        Flag( "Blocks sight", f, 0x1000, "AI cannot see through it." );
        ImGui::SameLine();
        Flag( "With view model", f, 0x800, "Draws with the weapon view model." );
    }

    void TabSize( kefxElem_t &e )
    {
        GraphBlock( "Size", e.size[0], e, &e.editorFlags, KEFX_E_RANDSIZE0,
                    "Diameter in units at curve value 1 (half of it each side of the centre)." );
        Flag( "Separate height", e.flags, KEFX_F_NONUNIFORMSCALE, "Width and height from two graphs (non-uniform scale)." );
        if ( ( e.flags & KEFX_F_NONUNIFORMSCALE ) || e.type == KEFX_TAIL || e.type == KEFX_CLOUD )
            GraphBlock( "Height", e.size[1], e, &e.editorFlags, KEFX_E_RANDSIZE1,
                        e.type == KEFX_TAIL ? "Tail length in units at curve value 1." : "Height in units at curve value 1." );
        if ( e.type == KEFX_CLOUD || e.type == KEFX_MODEL )
            GraphBlock( "Scale", e.scale, e, &e.editorFlags, KEFX_E_RANDSCALE, "Model / cloud scale at curve value 1." );
    }

    void TabVelocity( kefxElem_t &e )
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( "Graph" );
        ImGui::SameLine( 132.0f );
        ImGui::RadioButton( "Velocity 1", &s_velFrame, 0 );
        ImGui::SameLine();
        ImGui::RadioButton( "Velocity 2", &s_velFrame, 1 );
        const unsigned absBit = s_velFrame ? KEFX_E_ABSVEL1 : KEFX_E_ABSVEL0;
        const unsigned rndBit = s_velFrame ? KEFX_E_RANDVEL1 : KEFX_E_RANDVEL0;
        Flag( "World axes", e.editorFlags, absBit,
              "Off: along the particle's frame (forward / left / up of the run-relative frame).\nOn: world X / Y / Z." );
        const bool world = ( e.editorFlags & absBit ) != 0;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( "Axis" );
        ImGui::SameLine( 132.0f );
        ImGui::RadioButton( world ? "X" : "Forward", &s_velAxis, 0 );
        ImGui::SameLine();
        ImGui::RadioButton( world ? "Y" : "Left", &s_velAxis, 1 );
        ImGui::SameLine();
        ImGui::RadioButton( world ? "Z" : "Up", &s_velAxis, 2 );
        GraphBlock( "Speed", e.vel[s_velFrame][s_velAxis], e, &e.editorFlags, rndBit,
                    "Units per second at curve value 1 (the curve runs -0.5 .. 0.5 over the life)." );
        Flag( "Legacy velocity", e.editorFlags, KEFX_E_BACKCOMPATVEL,
              "Old effects' randomness rule for the second graph (keeps shipped effects moving as before)." );
    }

    void TabRotation( kefxElem_t &e )
    {
        RangeRow( "Initial rotation", e.initialRot, false, 1.0f, "Sprite roll at birth, degrees." );
        GraphBlock( "Spin", e.rot, e, &e.editorFlags, KEFX_E_RANDROTDELTA, "Degrees per second at curve value 1." );
        ImGui::SeparatorText( "Orientation (oriented sprites, models, runners)" );
        RangeRow( "Pitch", e.spawnAngles[0], false, 1.0f, "Degrees." );
        RangeRow( "Yaw", e.spawnAngles[1], false, 1.0f, "Degrees." );
        RangeRow( "Roll", e.spawnAngles[2], false, 1.0f, "Degrees." );
        RangeRow( "Pitch speed", e.angleVel[0], false, 1.0f, "Degrees per second." );
        RangeRow( "Yaw speed", e.angleVel[1], false, 1.0f, "Degrees per second." );
        RangeRow( "Roll speed", e.angleVel[2], false, 1.0f, "Degrees per second." );
        Flag( "Runner random rotation", e.flags, KEFX_F_RUNNERRANDROT, "Child effects start at a random rotation." );
    }

    void EffectRef( const char *label, std::string &value, const char *popup )
    {
        ImGui::PushID( label );
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( label );
        ImGui::SameLine( 132.0f );
        if ( ImGui::Button( value.empty() ? "(none)" : value.c_str(), ImVec2( 200.0f, 0.0f ) ) )
            ImGui::OpenPopup( popup );
        if ( AssetPicker( popup, KIND_FX, value ) )
            Changed();
        if ( !value.empty() )
        {
            ImGui::SameLine();
            if ( ImGui::SmallButton( "x" ) ) { value.clear(); Changed(); }
        }
        ImGui::PopID();
    }

    void TabPhysics( kefxElem_t &e )
    {
        RangeRow( "Gravity", e.gravity, false, 0.5f, "Percent of world gravity; negative rises (smoke)." );
        RangeRow( "Bounce", e.elasticity, false, 0.01f, "Elasticity on impact, 0..1." );
        ImGui::SeparatorText( "Collision" );
        Flag( "Collide", e.flags, 0x100, "Particles collide with the world (not simulated in the editor)." );
        ImGui::SameLine();
        Flag( "Die on touch", e.flags, 0x200, "A particle ends when it hits something." );
        ImGui::SameLine();
        Flag( "Item clip", e.editorFlags, 0x20000, "Also collide with item clip." );
        Flag( "Bounding sphere", e.editorFlags, 0x10000, "Collide with a sphere instead of a point." );
        if ( e.editorFlags & 0x10000 )
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( "Sphere offset" );
            ImGui::SameLine( 132.0f );
            ImGui::SetNextItemWidth( KFE_NUM_W * 3.0f );
            if ( ImGui::DragFloat3( "##coll", e.collOffset, 0.5f, -10000.0f, 10000.0f, "%g" ) ) Changed();
            FloatRow( "Sphere radius", e.collRadius, 0.5f, 0.0f, 10000.0f, "Collision sphere radius." );
        }
        ImGui::SeparatorText( "Effects it starts" );
        Flag( "On impact", e.editorFlags, KEFX_E_PLAYONTOUCH, "Play an effect where a particle hits." );
        if ( e.editorFlags & KEFX_E_PLAYONTOUCH ) EffectRef( "Impact effect", e.fxOnImpact, "##pickimpact" );
        Flag( "On death", e.editorFlags, KEFX_E_PLAYONDEATH, "Play an effect where a particle dies." );
        if ( e.editorFlags & KEFX_E_PLAYONDEATH ) EffectRef( "Death effect", e.fxOnDeath, "##pickdeath" );
        Flag( "While moving", e.editorFlags, KEFX_E_PLAYONRUN, "Emit an effect along the particle's path." );
        if ( e.editorFlags & KEFX_E_PLAYONRUN )
        {
            EffectRef( "Emitted effect", e.emission, "##pickemit" );
            RangeRow( "Emit distance", e.emitDist, false, 0.5f, "Units travelled between emissions." );
            RangeRow( "Emit variance", e.emitDistVariance, false, 0.5f, "Random change of that distance." );
        }
    }

    void TabColor( kefxElem_t &e )
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( "Colour" );
        ImGui::SameLine( 132.0f );
        bool rc = ( e.editorFlags & KEFX_E_RANDCOLOR ) != 0;
        if ( ImGui::Checkbox( "Random range##col", &rc ) )
        {
            e.editorFlags = rc ? ( e.editorFlags | KEFX_E_RANDCOLOR ) : ( e.editorFlags & ~KEFX_E_RANDCOLOR );
            Changed();
        }
        Tip( "Each particle picks a colour between the two gradients." );
        GradientBlock( "##g0", e.color.curve[0], s_gradStop[0] );
        if ( rc )
            GradientBlock( "##g1", e.color.curve[1], s_gradStop[1] );
        GraphBlock( "Alpha", e.alpha, e, &e.editorFlags, KEFX_E_RANDALPHA, "The game ignores this scale; the curve is the alpha (0..1)." );
        Flag( "Colour by alpha", e.editorFlags, KEFX_E_MODCOLORBYALPHA,
              "Multiply colour by alpha (for additive materials; alpha becomes 1)." );
        FloatRow( "Lighting", e.lightingFrac, 0.01f, 0.0f, 1.0f, "How much world lighting tints it, 0..1 (not previewed)." );
    }

    void TabVisuals( kefxElem_t &e )
    {
        int type = e.type < KEFX_TYPE_COUNT ? e.type : 0;
        const char *items[KEFX_TYPE_COUNT];
        for ( int t = 0; t < KEFX_TYPE_COUNT; ++t ) items[t] = KiwiEfx_TypeLabel( t );
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted( "Type" );
        ImGui::SameLine( 132.0f );
        ImGui::SetNextItemWidth( 180.0f );
        if ( ImGui::Combo( "##type", &type, items, KEFX_TYPE_COUNT ) && type != e.type )
        {
            const bool wasMaterial = e.type <= KEFX_CLOUD || e.type == KEFX_DECAL;
            const bool isMaterial = type <= KEFX_CLOUD || type == KEFX_DECAL;
            e.type = type;
            if ( wasMaterial != isMaterial || type == KEFX_LIGHT || type == KEFX_SPOTLIGHT )
                e.visuals.clear();
            Changed();
        }
        if ( e.type == KEFX_MODEL || e.type == KEFX_LIGHT || e.type == KEFX_SPOTLIGHT || e.type == KEFX_SOUND
          || e.type == KEFX_DECAL || e.type == KEFX_TRAIL )
            ImGui::TextDisabled( "Saved and played in game; the editor preview does not draw this type." );

        if ( e.type != KEFX_LIGHT && e.type != KEFX_SPOTLIGHT )
        {
            const int kind = e.type == KEFX_RUNNER ? KIND_FX : e.type == KEFX_MODEL ? KIND_MODEL : KIND_MATERIAL;
            ImGui::SeparatorText( e.type == KEFX_RUNNER ? "Effects (one picked per particle)"
                                : e.type == KEFX_MODEL  ? "Models (one picked per particle)"
                                : e.type == KEFX_SOUND  ? "Sound aliases"
                                                        : "Materials (one picked per particle)" );
            int removeAt = -1;
            for ( size_t v = 0; v < e.visuals.size(); ++v )
            {
                ImGui::PushID( (int)v );
                if ( e.type == KEFX_SOUND )
                {
                    char buf[128];
                    _snprintf( buf, sizeof( buf ), "%s", e.visuals[v].c_str() );
                    buf[sizeof( buf ) - 1] = '\0';
                    ImGui::SetNextItemWidth( 240.0f );
                    if ( ImGui::InputText( "##snd", buf, sizeof( buf ) ) ) { e.visuals[v] = buf; Changed(); }
                }
                else
                {
                    if ( kind == KIND_MATERIAL )
                    {
                        Swatch( e.visuals[v], 40.0f );
                        if ( ImGui::IsItemHovered() )
                        {
                            ImGui::BeginTooltip();
                            Swatch( e.visuals[v], 160.0f );
                            ImGui::EndTooltip();
                        }
                        ImGui::SameLine();
                    }
                    if ( ImGui::Button( e.visuals[v].c_str(), ImVec2( 200.0f, kind == KIND_MATERIAL ? 40.0f : 0.0f ) ) )
                        ImGui::OpenPopup( "##pickvis" );
                    if ( AssetPicker( "##pickvis", kind, e.visuals[v] ) )
                        Changed();
                }
                ImGui::SameLine();
                if ( ImGui::SmallButton( "x" ) ) removeAt = (int)v;
                ImGui::PopID();
            }
            if ( removeAt >= 0 )
            {
                e.visuals.erase( e.visuals.begin() + removeAt );
                Changed();
            }
            const int cap = e.type == KEFX_DECAL ? 16 : KEFX_MAX_VISUALS;
            ImGui::BeginDisabled( (int)e.visuals.size() >= cap );
            if ( ImGui::SmallButton( "Add" ) )
            {
                if ( e.type == KEFX_SOUND )
                {
                    e.visuals.push_back( "" );
                    Changed();
                }
                else
                    ImGui::OpenPopup( "##pickadd" );
            }
            ImGui::EndDisabled();
            std::string added;
            if ( AssetPicker( "##pickadd", kind, added ) && !added.empty() )
            {
                e.visuals.push_back( added );
                Changed();
            }
            if ( kind == KIND_MATERIAL )
            {
                const std::string missing = KiwiFxPreview_MissingMaterials( PreviewName() );
                if ( !missing.empty() )
                    ImGui::TextColored( ImVec4( 1.0f, 0.55f, 0.3f, 1.0f ), "Not found: %s", missing.c_str() );
            }
        }

        if ( e.type <= KEFX_CLOUD )
        {
            ImGui::SeparatorText( "Texture atlas" );
            static const char *const names[] = { "Fixed", "Random", "Indexed" };
            static const unsigned values[] = { 0u, 1u, 2u };
            FlagCombo( "Start frame", e.atlasBehavior, KEFX_A_START_MASK, names, values, 3,
                       "Fixed: the frame below.  Random: per particle.  Indexed: by spawn order." );
            IntRow( "Frame", e.atlasIndex, 0, 255, "Start frame for Fixed." );
            IntRow( "Frames / sec", e.atlasFps, 0, 1000, "Animation speed (0 = still)." );
            Flag( "Play over life", e.atlasBehavior, KEFX_A_PLAYOVERLIFE, "Run through the atlas once over the particle's life." );
            ImGui::SameLine();
            Flag( "Loop N times", e.atlasBehavior, KEFX_A_LOOPONLYN, "Stop on the last frame after N loops." );
            if ( e.atlasBehavior & KEFX_A_LOOPONLYN )
                IntRow( "Loops", e.atlasLoopCount, 0, 1000, "Extra loops before holding the last frame." );
        }
        if ( e.type == KEFX_TRAIL )
        {
            ImGui::SeparatorText( "Trail" );
            IntRow( "Split distance", e.trailSplitDist, 0, 100000, "Units between trail segments." );
            FloatRow( "Scroll time", e.trailScrollTime, 0.01f, 0.0f, 1000.0f, "Texture scroll seconds." );
            IntRow( "Repeat distance", e.trailRepeatDist, 0, 100000, "Units per texture repeat." );
            ImGui::TextDisabled( e.hasTrailDef ? "Cross-section: %i vertices (kept as loaded)." : "No cross-section (trailDef) - the game rejects it.",
                                 (int)e.trailVerts.size() / 3 );
        }
    }

    // ── element list ──────────────────────────────────────────────────────────
    ImU32 TypeColor( int type )
    {
        switch ( type )
        {
        case KEFX_BILLBOARD: case KEFX_ORIENTED: case KEFX_CLOUD: return IM_COL32( 255, 150, 50, 255 );
        case KEFX_TAIL: case KEFX_TRAIL:                          return IM_COL32( 255, 220, 80, 255 );
        case KEFX_MODEL:                                          return IM_COL32( 140, 200, 255, 255 );
        case KEFX_LIGHT: case KEFX_SPOTLIGHT:                     return IM_COL32( 255, 255, 190, 255 );
        case KEFX_SOUND:                                          return IM_COL32( 170, 255, 170, 255 );
        case KEFX_DECAL:                                          return IM_COL32( 200, 160, 120, 255 );
        case KEFX_RUNNER:                                         return IM_COL32( 230, 130, 255, 255 );
        default:                                                  return IM_COL32( 128, 128, 128, 255 );
        }
    }

    void ElementList( float width, float height )
    {
        ImGui::BeginChild( "##elems", ImVec2( width < 0.0f ? 0.0f : width, height ), ImGuiChildFlags_Borders );
        int moveFrom = -1, moveTo = -1;
        for ( int i = 0; i < (int)s_doc.elems.size(); ++i )
        {
            kefxElem_t &e = s_doc.elems[i];
            ImGui::PushID( i );
            bool on = !e.Disabled();
            if ( ImGui::Checkbox( "##on", &on ) )
            {
                e.editorFlags = on ? ( e.editorFlags & ~(unsigned)KEFX_E_DISABLED ) : ( e.editorFlags | (unsigned)KEFX_E_DISABLED );
                Changed();
            }
            Tip( "Enabled (disabled elements are saved but not played)." );
            ImGui::SameLine();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float h = ImGui::GetFrameHeight();
            ImGui::GetWindowDrawList()->AddCircleFilled( ImVec2( p.x + 4.0f, p.y + h * 0.5f ), 4.0f, TypeColor( e.type ) );
            ImGui::Dummy( ImVec2( 10.0f, h ) );
            ImGui::SameLine();
            if ( s_renaming == i )
            {
                ImGui::SetNextItemWidth( -1.0f );
                if ( s_renameFocus )
                {
                    ImGui::SetKeyboardFocusHere();
                    s_renameFocus = false;
                }
                const bool done = ImGui::InputText( "##rename", s_renameBuf, sizeof( s_renameBuf ),
                                                    ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll );
                if ( done || ImGui::IsItemDeactivated() )
                {
                    if ( strcmp( s_renameBuf, e.name.c_str() ) )
                    {
                        Changed();
                        e.name = s_renameBuf;
                    }
                    s_renaming = -1;
                }
            }
            else
            {
                ImGui::BeginDisabled( e.Disabled() );
                if ( ImGui::Selectable( e.name.empty() ? "(unnamed)" : e.name.c_str(), s_sel == i,
                                        ImGuiSelectableFlags_AllowDoubleClick, ImVec2( 0.0f, h ) ) )
                {
                    s_sel = i;
                    if ( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                    {
                        s_renaming = i;
                        s_renameFocus = true;
                        _snprintf( s_renameBuf, sizeof( s_renameBuf ), "%s", e.name.c_str() );
                        s_renameBuf[sizeof( s_renameBuf ) - 1] = '\0';
                    }
                }
                ImGui::EndDisabled();
                if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
                    ImGui::SetTooltip( "%s, %s\nDouble-click to rename; drag to reorder.", KiwiEfx_TypeLabel( e.type ),
                                       e.Looping() ? "looping" : "one-shot" );
                if ( ImGui::BeginDragDropSource() )
                {
                    ImGui::SetDragDropPayload( "KIWI_FXELEM", &i, sizeof( i ) );
                    ImGui::TextUnformatted( e.name.c_str() );
                    ImGui::EndDragDropSource();
                }
                if ( ImGui::BeginDragDropTarget() )
                {
                    if ( const ImGuiPayload *pl = ImGui::AcceptDragDropPayload( "KIWI_FXELEM" ) )
                    {
                        moveFrom = *(const int *)pl->Data;
                        moveTo = i;
                    }
                    ImGui::EndDragDropTarget();
                }
            }
            ImGui::PopID();
        }
        if ( s_doc.elems.empty() )
            ImGui::TextDisabled( "No elements" );
        ImGui::EndChild();
        if ( moveFrom >= 0 && moveTo >= 0 && moveFrom != moveTo && moveFrom < (int)s_doc.elems.size() )
        {
            Changed();
            kefxElem_t e = s_doc.elems[moveFrom];
            s_doc.elems.erase( s_doc.elems.begin() + moveFrom );
            s_doc.elems.insert( s_doc.elems.begin() + moveTo, e );
            s_sel = moveTo;
        }
    }

    void ElementButtons()
    {
        ImGui::BeginDisabled( (int)s_doc.elems.size() >= KEFX_MAX_ELEMS );
        if ( ImGui::SmallButton( "Add" ) )
            ImGui::OpenPopup( "##addelem" );
        ImGui::EndDisabled();
        if ( ImGui::BeginPopup( "##addelem" ) )
        {
            for ( int t = 0; t < KEFX_TYPE_COUNT; ++t )
                if ( ImGui::MenuItem( KiwiEfx_TypeLabel( t ) ) )
                {
                    Changed();
                    kefxElem_t e;
                    KiwiEfx_InitElem( e, t );
                    char nm[48];
                    _snprintf( nm, sizeof( nm ), "%s_%i", KiwiEfx_TypeName( t ), (int)s_doc.elems.size() + 1 );
                    nm[sizeof( nm ) - 1] = '\0';
                    e.name = nm;
                    const int at = s_doc.elems.empty() ? 0 : s_sel + 1;
                    s_doc.elems.insert( s_doc.elems.begin() + at, e );
                    s_sel = at;
                }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled( s_doc.elems.empty() || (int)s_doc.elems.size() >= KEFX_MAX_ELEMS );
        if ( ImGui::SmallButton( "Duplicate" ) )
        {
            Changed();
            kefxElem_t e = s_doc.elems[s_sel];
            e.name += "_copy";
            s_doc.elems.insert( s_doc.elems.begin() + s_sel + 1, e );
            ++s_sel;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled( s_doc.elems.empty() );
        if ( ImGui::SmallButton( "Delete" ) )
        {
            Changed();
            s_doc.elems.erase( s_doc.elems.begin() + s_sel );
            ClampSel();
        }
        ImGui::EndDisabled();
    }

    void Toolbar()
    {
        // Line 1: the document.
        ImGui::AlignTextToFramePadding();
        if ( s_name.empty() )
            ImGui::TextUnformatted( s_doc.elems.empty() ? "(no effect)" : "(new effect)" );
        else
            ImGui::Text( "%s", s_name.c_str() );
        if ( s_dirty )
        {
            ImGui::SameLine( 0.0f, 2.0f );
            ImGui::TextColored( ImVec4( 1.0f, 0.7f, 0.3f, 1.0f ), "*" );
        }
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Open" ) )
            ImGui::OpenPopup( "##openfx" );
        std::string pick = s_name;
        if ( AssetPicker( "##openfx", KIND_FX, pick ) )
            KiwiFxEditor_Open( pick.c_str() );
        ImGui::SameLine();
        if ( ImGui::SmallButton( "New" ) )
            KiwiFxEditor_New();
        ImGui::SameLine();
        ImGui::BeginDisabled( s_name.empty() || !s_dirty );
        if ( ImGui::SmallButton( "Save" ) )
            SaveTo( s_name );
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled( s_doc.elems.empty() );
        if ( ImGui::SmallButton( "Save As" ) )
        {
            s_overwriteArmed = false;
            ImGui::OpenPopup( "##saveas" );
        }
        ImGui::EndDisabled();
        if ( ImGui::BeginPopup( "##saveas" ) )
        {
            ImGui::TextUnformatted( "raw/fx/" );
            ImGui::SameLine( 0.0f, 0.0f );
            ImGui::SetNextItemWidth( 220.0f );
            if ( ImGui::IsWindowAppearing() )
                ImGui::SetKeyboardFocusHere();
            if ( ImGui::InputText( "##path", s_saveAs, sizeof( s_saveAs ) ) )
                s_overwriteArmed = false;
            ImGui::SameLine( 0.0f, 0.0f );
            ImGui::TextUnformatted( ".efx" );
            const std::string clean = CleanName( s_saveAs );
            const bool exists = !clean.empty() && KiwiEfx_FileStamp( clean.c_str() ) != 0;
            if ( clean.empty() )
                ImGui::TextDisabled( "folder/name - letters, digits, _ - /" );
            else if ( exists )
                ImGui::TextColored( ImVec4( 1.0f, 0.6f, 0.3f, 1.0f ), s_overwriteArmed ? "Press again to overwrite." : "That effect exists." );
            ImGui::BeginDisabled( clean.empty() );
            if ( ImGui::Button( exists ? "Overwrite" : "Save" ) )
            {
                if ( exists && !s_overwriteArmed && clean != s_name )
                    s_overwriteArmed = true;
                else if ( SaveTo( clean ) )
                    ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled( s_name.empty() || !s_dirty );
        if ( ImGui::SmallButton( "Revert" ) )
            LoadNamed( s_name );
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled( s_undo.empty() );
        if ( ImGui::SmallButton( "Undo" ) ) Undo();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled( s_redo.empty() );
        if ( ImGui::SmallButton( "Redo" ) ) Redo();
        ImGui::EndDisabled();

        // Line 2: playback.
        ImGui::BeginDisabled( s_doc.elems.empty() );
        if ( ImGui::SmallButton( KiwiFxPreview_TestPlaying() ? "Replay" : "Play" ) )
            PlayTest();
        Tip( "Play the effect where the view centre meets the world.  Selected FX entities\n"
             "using this effect play the edited version too." );
        ImGui::EndDisabled();
        if ( KiwiFxPreview_TestPlaying() )
        {
            ImGui::SameLine();
            if ( ImGui::SmallButton( "Stop" ) )
                KiwiFxPreview_StopTest();
        }
        ImGui::SameLine();
        bool paused = KiwiFxPreview_Paused();
        if ( ImGui::Checkbox( "Pause", &paused ) )
            KiwiFxPreview_SetPaused( paused );
        ImGui::SameLine();
        float speed = KiwiFxPreview_TimeScale();
        ImGui::SetNextItemWidth( 90.0f );
        if ( ImGui::SliderFloat( "##speed", &speed, 0.05f, 2.0f, "speed %.2fx", ImGuiSliderFlags_Logarithmic ) )
            KiwiFxPreview_SetTimeScale( speed );
        ImGui::SameLine();
        if ( ImGui::Checkbox( "Follow selection", &s_follow ) )
            Radiant_ProfileSetInt( KFE_PROFILE, "FollowSelection", s_follow ? 1 : 0 );
        Tip( "Selecting an FX entity opens its effect here (unless there are unsaved edits)." );
        ImGui::SameLine();
        ImGui::BeginDisabled( s_name.empty() );
        if ( ImGui::SmallButton( "Use for selected" ) )
        {
            if ( !KiwiParticles_SetSelectedEffect( s_name.c_str() ) )
                s_status = "Select FX entities first.";
        }
        Tip( "Every selected FX entity plays this effect (undoable)." );
        ImGui::EndDisabled();
    }

    void FollowSelection()
    {
        if ( !s_follow )
            return;
        const std::string sel = KiwiParticles_SelectedEffect();
        if ( sel.empty() || sel == s_lastFollowed )
            return;
        s_lastFollowed = sel;
        if ( !_stricmp( sel.c_str(), s_name.c_str() ) || s_dirty )
            return;
        LoadNamed( sel );
    }

    void DiscardPrompt()
    {
        if ( s_askDiscard )
        {
            ImGui::OpenPopup( "Discard FX changes?" );
            s_askDiscard = false;
        }
        if ( ImGui::BeginPopupModal( "Discard FX changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize ) )
        {
            ImGui::Text( "%s has unsaved changes.", s_name.empty() ? "The new effect" : s_name.c_str() );
            if ( ImGui::Button( "Discard" ) )
            {
                const std::string next = s_pendingOpen;
                s_dirty = false;
                if ( next.empty() )
                    KiwiFxEditor_New();
                else
                    LoadNamed( next );
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if ( ImGui::Button( "Cancel" ) )
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
}

// ── public ─────────────────────────────────────────────────────────────────────
bool KiwiFxEditor_Open( const char *fxName )
{
    LoadPrefs();
    const std::string name = CleanName( fxName );
    if ( name.empty() )
        return false;
    KiwiWindows_Set( KIWI_WIN_FXEDITOR, true );
    ImGuiShell_FocusTab( KiwiWindows_Title( KIWI_WIN_FXEDITOR ) );
    if ( s_dirty && name != s_name )
    {
        s_pendingOpen = name;
        s_askDiscard = true;
        return true;
    }
    s_lastFollowed = name;
    return LoadNamed( name );
}

void KiwiFxEditor_New()
{
    LoadPrefs();
    KiwiWindows_Set( KIWI_WIN_FXEDITOR, true );
    ImGuiShell_FocusTab( KiwiWindows_Title( KIWI_WIN_FXEDITOR ) );
    if ( s_dirty )
    {
        s_pendingOpen.clear();
        s_askDiscard = true;
        return;
    }
    kefxDoc_t doc;
    kefxElem_t e;
    KiwiEfx_InitElem( e, KEFX_BILLBOARD );
    e.name = "smoke";
    doc.elems.push_back( e );
    Adopt( std::string(), doc );
    s_dirty = true;
    s_status = "New effect - Save As to write it under raw/fx.";
}

void KiwiFxEditor_Draw()
{
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_FXEDITOR );
    if ( !open || !*open )
        return;
    LoadPrefs();
    if ( KiwiWindows_JustOpened( KIWI_WIN_FXEDITOR ) )
    {
        // Beside the Particles browser (right column), never over the camera: the point of
        // editing is watching the preview.  The main node is the fallback.
        ImGuiWindow *sib = ImGui::FindWindowByName( KiwiWindows_Title( KIWI_WIN_PARTICLES ) );
        if ( !sib || !sib->DockId )
            sib = ImGui::FindWindowByName( KiwiWindows_Title( KIWI_WIN_INSPECTOR ) );
        ImGui::SetNextWindowDockID( sib && sib->DockId ? sib->DockId : ImGuiShell_DockRoot(), ImGuiCond_Always );
    }
    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_FXEDITOR ), open ) )
    {
        FollowSelection();
        Toolbar();
        ImGui::Separator();
        const bool noEffect = s_doc.elems.empty() && s_name.empty();
        // KIWI (2026-11-03, user: "the FX editor needs a browser"): the FX window's tile
        // browser, same code (kiwi_particles.cpp): click = open here, drag / double-click = place.
        if ( ImGui::CollapsingHeader( "Browse effects", ImGuiTreeNodeFlags_DefaultOpen ) )
        {
            const float rowH = 92.0f + ImGui::GetTextLineHeight() + 8.0f;   // a tile row (KFX_TILE + label + pad)
            KiwiParticles_DrawEditorBrowser( s_name.c_str(),
                                             noEffect ? -ImGui::GetTextLineHeightWithSpacing() : rowH * 2.0f + 8.0f );
        }
        if ( noEffect )
        {
            ImGui::TextDisabled( "No effect open" );
        }
        else
        {
            ClampSel();
            const float status = ImGui::GetTextLineHeightWithSpacing();
            // A narrow dock (the right column) stacks the element list over the tabs.
            const bool narrow = ImGui::GetContentRegionAvail().x < 640.0f;
            if ( narrow )
            {
                const float rows = (float)( std::min )( (int)s_doc.elems.size(), 6 );
                ElementList( -1.0f, ( std::max )( 2.0f, rows ) * ImGui::GetFrameHeightWithSpacing() + 8.0f );
                ElementButtons();
            }
            else
            {
                ImGui::BeginGroup();
                ElementList( 190.0f, -( ImGui::GetFrameHeightWithSpacing() + status ) );
                ElementButtons();
                ImGui::EndGroup();
                ImGui::SameLine();
            }
            ImGui::BeginChild( "##props", ImVec2( 0.0f, -status ), ImGuiChildFlags_None );
            if ( !s_doc.elems.empty() )
            {
                kefxElem_t &e = s_doc.elems[s_sel];
                std::vector<std::string> problems;
                KiwiEfx_Validate( e, problems );
                for ( size_t i = 0; i < problems.size(); ++i )
                    ImGui::TextColored( ImVec4( 1.0f, 0.6f, 0.3f, 1.0f ), "%s", problems[i].c_str() );
                if ( ImGui::BeginTabBar( "##fxtabs" ) )
                {
                    static const char *const kTabs[] = { "Generation", "Size", "Velocity", "Rotation", "Physics", "Color", "Visuals" };
                    for ( int t = 0; t < 7; ++t )
                    {
                        // s_forceTab: the test DSL's `fxedit ui` picks a page.
                        const ImGuiTabItemFlags fl = s_forceTab == t ? ImGuiTabItemFlags_SetSelected : 0;
                        if ( !ImGui::BeginTabItem( kTabs[t], nullptr, fl ) )
                            continue;
                        switch ( t )
                        {
                        case 0: TabGeneration( e ); break;
                        case 1: TabSize( e );       break;
                        case 2: TabVelocity( e );   break;
                        case 3: TabRotation( e );   break;
                        case 4: TabPhysics( e );    break;
                        case 5: TabColor( e );      break;
                        default: TabVisuals( e );   break;
                        }
                        ImGui::EndTabItem();
                    }
                    s_forceTab = -1;
                    ImGui::EndTabBar();
                }
            }
            ImGui::EndChild();
            ImGui::TextDisabled( "%s", s_status.c_str() );
        }
        DiscardPrompt();
    }
    ImGui::End();
    EndFrame();
}

void KiwiFxEditor_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiWindowFxEditor", 0, 0, KIWI_CMD_WINDOW_FXEDITOR );
}

bool KiwiFxEditor_DispatchInstant( unsigned int cmdId )
{
    (void)cmdId;
    return false;                               // the window toggle goes through KiwiWindows
}

// ── test DSL ───────────────────────────────────────────────────────────────────
// fxedit open <fx> | new | add <typeName> | count | name <i> <name> | flag <i> <flagName> 0|1
//        | set <i> <field> <values...> | visual <i> <typeName> [names...] | saveas <fx> | save
//        | undo | redo | play | elem <i>
bool KiwiFxEditor_TestVerb( const std::vector<std::string> &w, std::string &out, std::string &err )
{
    out.clear();
    err.clear();
    if ( w.size() < 2 )
    {
        err = "usage: fxedit <verb> ...";
        return false;
    }
    const std::string &v = w[1];
    auto elemAt = [&]( size_t argi ) -> kefxElem_t *
    {
        if ( w.size() <= argi )
            return nullptr;
        const int i = atoi( w[argi].c_str() );
        if ( i < 0 || i >= (int)s_doc.elems.size() )
            return nullptr;
        return &s_doc.elems[i];
    };
    if ( v == "open" && w.size() == 3 )
    {
        s_dirty = false;                         // tests never stop at the discard prompt
        if ( !KiwiFxEditor_Open( w[2].c_str() ) || s_name != CleanName( w[2].c_str() ) )
        {
            err = "could not open " + w[2] + ": " + s_status;
            return false;
        }
        out = s_name;
        return true;
    }
    if ( v == "new" )
    {
        s_dirty = false;
        KiwiFxEditor_New();
        return true;
    }
    if ( v == "count" )
    {
        out = std::to_string( s_doc.elems.size() );
        return true;
    }
    if ( v == "add" && w.size() == 3 )
    {
        for ( int t = 0; t < KEFX_TYPE_COUNT; ++t )
            if ( !_stricmp( w[2].c_str(), KiwiEfx_TypeName( t ) ) )
            {
                Changed();
                kefxElem_t e;
                KiwiEfx_InitElem( e, t );
                e.name = KiwiEfx_TypeName( t );
                s_doc.elems.push_back( e );
                s_sel = (int)s_doc.elems.size() - 1;
                s_gesture = false;
                s_committed = s_doc;
                PushPreview();
                return true;
            }
        err = "unknown element type " + w[2];
        return false;
    }
    if ( v == "name" && w.size() == 4 )
    {
        kefxElem_t *e = elemAt( 2 );
        if ( !e ) { err = "no element " + w[2]; return false; }
        Changed();
        e->name = w[3];
        s_gesture = false;
        s_committed = s_doc;
        return true;
    }
    if ( v == "flag" && w.size() == 5 )
    {
        kefxElem_t *e = elemAt( 2 );
        if ( !e ) { err = "no element " + w[2]; return false; }
        for ( const kefxFlagDef_t *d = KiwiEfx_FlagDefs(); d->name; ++d )
        {
            if ( _stricmp( d->name, w[3].c_str() ) )
                continue;
            unsigned &word = d->word == 0 ? e->editorFlags : d->word == 1 ? e->flags : e->atlasBehavior;
            Changed();
            if ( atoi( w[4].c_str() ) )
                word = ( word & ~d->mask ) | d->value;
            else if ( d->mask == d->value )
                word &= ~d->mask;
            s_gesture = false;
            s_committed = s_doc;
            PushPreview();
            return true;
        }
        err = "unknown flag " + w[3];
        return false;
    }
    if ( v == "set" && w.size() >= 5 )
    {
        // Re-parse the element with the field appended: the game's parser keeps the last value.
        kefxElem_t *e = elemAt( 2 );
        if ( !e ) { err = "no element " + w[2]; return false; }
        kefxDoc_t one;
        one.elems.push_back( *e );
        std::string text = KiwiEfx_Write( one );
        const size_t close = text.rfind( '}' );
        std::string field = "\t" + w[3];
        for ( size_t k = 4; k < w.size(); ++k )
            field += " " + w[k];
        field += ";\n";
        text.insert( close, field );
        kefxDoc_t parsed;
        std::string perr;
        if ( !KiwiEfx_Parse( text.c_str(), parsed, perr ) || parsed.elems.size() != 1 )
        {
            err = "set " + w[3] + ": " + perr;
            return false;
        }
        Changed();
        *e = parsed.elems[0];
        s_gesture = false;
        s_committed = s_doc;
        PushPreview();
        return true;
    }
    if ( v == "visual" && w.size() >= 4 )
    {
        kefxElem_t *e = elemAt( 2 );
        if ( !e ) { err = "no element " + w[2]; return false; }
        int type = -1;
        for ( int t = 0; t < KEFX_TYPE_COUNT; ++t )
            if ( !_stricmp( w[3].c_str(), KiwiEfx_TypeName( t ) ) ) type = t;
        if ( type < 0 ) { err = "unknown visual type " + w[3]; return false; }
        Changed();
        e->type = type;
        e->visuals.assign( w.begin() + 4, w.end() );
        s_gesture = false;
        s_committed = s_doc;
        PushPreview();
        return true;
    }
    if ( v == "elem" && w.size() == 3 )
    {
        kefxElem_t *e = elemAt( 2 );
        if ( !e ) { err = "no element " + w[2]; return false; }
        kefxDoc_t one;
        one.elems.push_back( *e );
        out = KiwiEfx_Write( one );
        return true;
    }
    if ( v == "saveas" && w.size() == 3 )
    {
        const std::string clean = CleanName( w[2].c_str() );
        if ( clean.empty() || !SaveTo( clean ) )
        {
            err = "save failed: " + s_status;
            return false;
        }
        out = clean;
        return true;
    }
    if ( v == "save" )
    {
        if ( s_name.empty() || !SaveTo( s_name ) )
        {
            err = "save failed: " + s_status;
            return false;
        }
        return true;
    }
    if ( v == "delete_saved" && w.size() == 3 )
    {
        // Test cleanup: only effects the harness wrote under raw/fx/kiwitest/.
        const std::string clean = CleanName( w[2].c_str() );
        if ( clean.compare( 0, 9, "kiwitest/" ) || !::DeleteFileA( KiwiEfx_PathFor( clean.c_str() ).c_str() ) )
        {
            err = "delete_saved only removes an existing raw/fx/kiwitest/ effect";
            return false;
        }
        const std::string dir = KiwiEfx_PathFor( "kiwitest/x" );
        ::RemoveDirectoryA( dir.substr( 0, dir.rfind( '\\' ) ).c_str() );   // only when empty
        KiwiParticles_Rescan();
        return true;
    }
    if ( v == "ui" && w.size() >= 3 )
    {
        // fxedit ui <elem> [tab 0..6]   select an element (and a property page) in the window
        kefxElem_t *e = elemAt( 2 );
        if ( !e ) { err = "no element " + w[2]; return false; }
        s_sel = atoi( w[2].c_str() );
        if ( w.size() >= 4 )
            s_forceTab = atoi( w[3].c_str() );
        return true;
    }
    if ( v == "undo" ) { Undo(); return true; }
    if ( v == "redo" ) { Redo(); return true; }
    if ( v == "dirty" ) { out = s_dirty ? "1" : "0"; return true; }
    if ( v == "current" ) { out = s_name; return true; }    // the open effect's path
    if ( v == "play" )
    {
        PlayTest();
        return true;
    }
    err = "unknown fxedit verb " + v;
    return false;
}

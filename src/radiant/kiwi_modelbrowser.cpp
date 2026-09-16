#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"

#include <imgui/imgui.h>
#include <universal/com_files.h>

#include "kiwi_modelbrowser.h"
#include "kiwi_command.h"
#include "kiwi_droptrace.h"     // KiwiDrop_IsModelEntity - "Swap in place" targets
#include "kiwi_entthumb.h"
#include "kiwi_thumbcache.h"
#include "kiwi_fmt.h"
#include "kiwi_lines.h"
#include "kiwi_pick.h"
#include "kiwi_str.h"
#include "kiwi_transform.h"
#include "kiwi_windows.h"

#include <algorithm>
#include <float.h>
#include <string>
#include <vector>
#include <string.h>
#include <commdlg.h>
#include <imgui_internal.h>

extern int         Sys_Printf( const char *fmt, ... );
extern int         g_nUpdateBits;
extern entity_s   *world_entity;          // map.cpp
extern selbrush_t  selected_brushes;      // map.cpp
extern eclass_t   *Eclass_ForName( int has_brushes, const char *name );
extern brush_t    *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls );
extern void        Brush_Create( float *mins, float *maxs, brush_t *b, eclass_t *ecls );
extern void        Brush_BuildWindings( brush_t *def, int bFull );
extern void        Select_Deselect( int a1 );
extern void        CreateEntityFromName( const char *str );
extern void        SetKeyValue( entity_s_def *e, const char *key, const char *value );
extern void        Undo_ClearRedo();
extern void        Undo_GeneralStart( const char *operation );
extern void        Undo_AddEntity_W( entity_s *e );                       // undo.cpp 0x45E990
extern void        Undo_End();
extern void        MarkMapModified();
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods,
                                             int commandId );
extern void        Ed_EnsureCurrentMaterial_Kiwi();
extern selbrush_t *KiwiExtrude_LandDef( brush_t *def );
extern ImGuiID     ImGuiShell_DockRoot();
extern camera_s   *Ed_Camera();
extern void        Radiant_SetProjectMapsPath( const char *mapPath );

namespace
{
    const float KMODEL_TILE_W       = 84.0f;
    const float KMODEL_TILE_H       = 64.0f;
    const float KMODEL_TILE_PAD     = 6.0f;

    struct modelRow_t
    {
        std::string name;
        std::string prefix;
    };

    std::vector<modelRow_t> s_models;
    std::vector<std::string> s_prefixes;
    bool        s_enumerated = false;
    char        s_filter[128] = { 0 };
    std::string s_prefix;

    bool        s_pendHave = false;
    std::string s_pendModel;
    int         s_pendX = 0;
    int         s_pendY = 0;
    bool        s_pendPlacement = false;
    float       s_pendOrigin[3] = { 0.0f, 0.0f, 0.0f };

    bool  s_ghostHave = false;
    std::string s_ghostModel;
    float s_ghostOrigin[3] = { 0.0f, 0.0f, 0.0f };
    float s_ghostMins[3] = { 0.0f, 0.0f, 0.0f };
    float s_ghostMaxs[3] = { 0.0f, 0.0f, 0.0f };

    bool NameLess( const std::string &a, const std::string &b )
    {
        return _stricmp( a.c_str(), b.c_str() ) < 0;
    }

    bool NameEqual( const std::string &a, const std::string &b )
    {
        return _stricmp( a.c_str(), b.c_str() ) == 0;
    }

    void NormalizeListedName( std::string &name )
    {
        for ( size_t i = 0; i < name.size(); ++i )
            if ( name[i] == '\\' )
                name[i] = '/';
        if ( name.size() >= 7 && _strnicmp( name.c_str(), "xmodel/", 7 ) == 0 )
            name.erase( 0, 7 );
        while ( !name.empty() && name[0] == '/' )
            name.erase( 0, 1 );
        while ( !name.empty() && name[name.size() - 1] == '/' )
            name.erase( name.size() - 1 );
    }

    std::string PrefixForName( const std::string &name )
    {
        const size_t underscore = name.find( '_' );
        if ( underscore == std::string::npos || underscore == 0 )
            return "(other)";
        return name.substr( 0, underscore + 1 );
    }

    void EnumerateModels()
    {
        std::vector<std::string> names;
        int count = 0;
        // FS_LIST_ALL covers loose and loaded-IWD paths; listing deduplicates case-insensitively.
        const char **files = FS_ListFiles( "xmodel", "", FS_LIST_ALL, &count );
        names.reserve( count > 0 ? (size_t)count : 0 );
        for ( int i = 0; files && i < count; ++i )
        {
            if ( !files[i] || !files[i][0] )
                continue;
            std::string name( files[i] );
            NormalizeListedName( name );
            if ( !name.empty() )
                names.push_back( name );
        }
        if ( files )
            FS_FreeFileList( files );

        std::sort( names.begin(), names.end(), NameLess );
        names.erase( std::unique( names.begin(), names.end(), NameEqual ), names.end() );

        s_models.clear();
        s_prefixes.clear();
        s_models.reserve( names.size() );
        s_prefixes.reserve( names.size() );
        for ( size_t i = 0; i < names.size(); ++i )
        {
            modelRow_t row;
            row.name = names[i];
            row.prefix = PrefixForName( row.name );
            s_prefixes.push_back( row.prefix );
            s_models.push_back( row );
        }
        std::sort( s_prefixes.begin(), s_prefixes.end(), NameLess );
        s_prefixes.erase( std::unique( s_prefixes.begin(), s_prefixes.end(), NameEqual ),
                          s_prefixes.end() );

        if ( !s_prefix.empty() )
        {
            bool found = false;
            for ( size_t i = 0; i < s_prefixes.size(); ++i )
                if ( NameEqual( s_prefix, s_prefixes[i] ) )
                {
                    found = true;
                    break;
                }
            if ( !found )
                s_prefix.clear();
        }
        s_enumerated = true;
    }

    bool BoundsValid( const float mins[3], const float maxs[3] )
    {
        for ( int i = 0; i < 3; ++i )
            if ( !_finite( mins[i] ) || !_finite( maxs[i] ) || maxs[i] < mins[i] )
                return false;
        return true;
    }

    void TileBounds( const char *modelName, float mins[3], float maxs[3] )
    {
        if ( KiwiEntThumb_GetModelBounds( modelName, mins, maxs ) &&
             BoundsValid( mins, maxs ) )
            return;
        for ( int i = 0; i < 3; ++i )
        {
            mins[i] = -8.0f;
            maxs[i] =  8.0f;
        }
    }

    void ProjectIso( const float p[3], float *x, float *y )
    {
        *x = ( p[0] - p[1] ) * 0.86602540f;
        *y = ( p[0] + p[1] ) * 0.5f - p[2];
    }

    ImU32 ModelColor( bool failed, bool hovered, float mul, float alpha )
    {
        float r = failed ? 0.42f : 0.34f;
        float g = failed ? 0.42f : 0.68f;
        float b = failed ? 0.42f : 0.95f;
        if ( hovered && !failed )
        {
            r *= 1.2f;
            g *= 1.2f;
            b *= 1.2f;
        }
        r *= mul;
        g *= mul;
        b *= mul;
        if ( r > 1.0f ) r = 1.0f;
        if ( g > 1.0f ) g = 1.0f;
        if ( b > 1.0f ) b = 1.0f;
        return ImGui::GetColorU32( ImVec4( r, g, b, alpha ) );
    }

    void DrawIsoBox( ImDrawList *dl, const ImVec2 &cellMin, const ImVec2 &cellMax,
                     const float inMins[3], const float inMaxs[3],
                     bool failed, bool hovered )
    {
        float mins[3], maxs[3];
        for ( int i = 0; i < 3; ++i )
        {
            mins[i] = inMins[i];
            maxs[i] = inMaxs[i];
            if ( maxs[i] - mins[i] < 0.001f )
            {
                mins[i] -= 0.5f;
                maxs[i] += 0.5f;
            }
        }

        float corners[KIWI_BOX_CORNERS][3];
        KiwiBox_Corners( mins, maxs, corners );
        float px[KIWI_BOX_CORNERS], py[KIWI_BOX_CORNERS];
        float lowX = FLT_MAX, lowY = FLT_MAX;
        float highX = -FLT_MAX, highY = -FLT_MAX;
        for ( int i = 0; i < KIWI_BOX_CORNERS; ++i )
        {
            ProjectIso( corners[i], &px[i], &py[i] );
            if ( px[i] < lowX ) lowX = px[i];
            if ( px[i] > highX ) highX = px[i];
            if ( py[i] < lowY ) lowY = py[i];
            if ( py[i] > highY ) highY = py[i];
        }

        const float availW = cellMax.x - cellMin.x - KMODEL_TILE_PAD * 2.0f;
        const float availH = cellMax.y - cellMin.y - KMODEL_TILE_PAD * 2.0f;
        const float spanX = highX - lowX > 0.001f ? highX - lowX : 1.0f;
        const float spanY = highY - lowY > 0.001f ? highY - lowY : 1.0f;
        float scale = availW / spanX;
        if ( availH / spanY < scale )
            scale = availH / spanY;
        const float centerX = ( cellMin.x + cellMax.x ) * 0.5f;
        const float centerY = ( cellMin.y + cellMax.y ) * 0.5f;
        const float modelX = ( lowX + highX ) * 0.5f;
        const float modelY = ( lowY + highY ) * 0.5f;

        ImVec2 screen[KIWI_BOX_CORNERS];
        for ( int i = 0; i < KIWI_BOX_CORNERS; ++i )
            screen[i] = ImVec2( centerX + ( px[i] - modelX ) * scale,
                                centerY + ( py[i] - modelY ) * scale );

        dl->AddQuadFilled( screen[4], screen[5], screen[7], screen[6],
                           ModelColor( failed, hovered, 1.05f, 0.75f ) );
        dl->AddQuadFilled( screen[0], screen[1], screen[5], screen[4],
                           ModelColor( failed, hovered, 0.75f, 0.75f ) );
        dl->AddQuadFilled( screen[1], screen[3], screen[7], screen[5],
                           ModelColor( failed, hovered, 0.55f, 0.75f ) );
        const ImU32 edge = ModelColor( failed, hovered, 1.25f, 1.0f );
        for ( int i = 0; i < KIWI_BOX_EDGES; ++i )
            dl->AddLine( screen[KIWI_BOX_EDGE[i][0]], screen[KIWI_BOX_EDGE[i][1]],
                         edge, 1.0f );
    }

    void DrawTooltip( const modelRow_t &row, bool failed, bool ready )
    {
        if ( !ImGui::BeginTooltip() )
            return;
        ImGui::TextUnformatted( row.name.c_str() );
        ImGui::Separator();

        float mins[3], maxs[3];
        if ( KiwiEntThumb_GetModelBounds( row.name.c_str(), mins, maxs ) )
        {
            char a[32], b[32], c[32], d[32], e[32], f[32];
            ImGui::Text( "bounds: %s %s %s  to  %s %s %s",
                         KiwiFmt_Num( a, sizeof( a ), mins[0] ),
                         KiwiFmt_Num( b, sizeof( b ), mins[1] ),
                         KiwiFmt_Num( c, sizeof( c ), mins[2] ),
                         KiwiFmt_Num( d, sizeof( d ), maxs[0] ),
                         KiwiFmt_Num( e, sizeof( e ), maxs[1] ),
                         KiwiFmt_Num( f, sizeof( f ), maxs[2] ) );
        }
        else
        {
            ImGui::TextDisabled( "bounds: loading" );
        }

        // KIWI (2026-09-13): tri-count and details.  Only a READY tile asks (its model
        // is resident by then), so hovering never loads a model.
        kiwiModelStats_t stats;
        if ( ready && !failed && KiwiEntThumb_ModelStats( row.name.c_str(), &stats ) )
        {
            ImGui::Text( "LOD 0: %i tris  %i verts  %i surface%s",
                         stats.lod0Tris, stats.lod0Verts,
                         stats.lod0Surfs, stats.lod0Surfs == 1 ? "" : "s" );
            if ( stats.lods > 1 )
                ImGui::Text( "%i LODs, %i tris in all", stats.lods, stats.totalTris );
            ImGui::Text( "%i bone%s, %i collision surface%s", stats.bones, stats.bones == 1 ? "" : "s",
                         stats.collSurfs, stats.collSurfs == 1 ? "" : "s" );
            for ( int k = 0; k < stats.materialCount; ++k )
                ImGui::TextDisabled( "%s %s", k == 0 ? "materials:" : "          ", stats.materials[k] );
        }

        if ( failed )
            ImGui::TextDisabled( "preview failed; this model cannot be placed" );
        else if ( ready )
            ImGui::TextDisabled( "preview ready" );
        else
            ImGui::TextDisabled( "preview loading" );
        if ( !failed )
            ImGui::TextDisabled( "drag into the 3D view or double-click to place" );
        ImGui::TextDisabled( "right-click: open in Explorer / delete from disk" );
        ImGui::EndTooltip();
    }

    bool QueueDrop( const char *modelName, int imgX, int imgY,
                    const float *placement = 0 )
    {
        if ( !modelName || !modelName[0] || KiwiEntThumb_ModelFailed( modelName ) )
            return false;
        s_pendModel = modelName;
        s_pendX = imgX;
        s_pendY = imgY;
        s_pendPlacement = placement != 0;
        if ( placement )
            for ( int k = 0; k < 3; ++k )
                s_pendOrigin[k] = placement[k];
        s_pendHave = true;
        ::PostMessageA( g_qeglobals.d_hwndMain, WM_COMMAND,
                        (WPARAM)(unsigned int)KIWI_CMD_MODEL_DROP, 0 );
        return true;
    }

    // Permanent delete (KIWI 2026-09-13).  Confirms with the full file list, deletes
    // the loose files, drops the thumbnail, and rescans the list on the NEXT frame
    // (s_models is being walked by the tile loop right now).
    bool s_rescanPending = false;

    // KIWI (2026-09-15, user: "Swap in place with new model ... mainly to facilitate
    // reskins"): a single click picks a tile (accent frame); the button rewrites the
    // "model" key of every selected misc_model to the picked name.  Every other key -
    // origin, angles, modelscale, targetname, spawnflags - is untouched, so the new
    // model lands exactly where the old one stood; Checkkey_Model / SetupModelInst
    // rebuild the instance and its proxy box, and one undo record covers the batch.
    std::string s_pickedModel;

    int SelectedModelEntities( std::vector<entity_s_def *> *out )
    {
        if ( out )
            out->clear();
        int n = 0;
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            if ( !b->owner || !b->owner->def || !KiwiDrop_IsModelEntity( b ) )
                continue;
            entity_s_def *def = (entity_s_def *)b->owner->def;
            if ( def->eclass && ( def->eclass->classtype & 0x10 ) )
                continue;                               // native prefabs are not xmodels
            bool dup = false;
            if ( out )
                for ( size_t i = 0; i < out->size() && !dup; ++i )
                    dup = ( ( *out )[i] == def );
            if ( dup )
                continue;
            if ( out )
                out->push_back( def );
            ++n;
        }
        return n;
    }

    bool SwapSelectedModels( const char *modelName, char *err, size_t errSz )
    {
        if ( err && errSz ) err[0] = '\0';
        if ( !modelName || !modelName[0] )
        {
            if ( err ) _snprintf( err, errSz, "no replacement model picked" );
            return false;
        }
        std::vector<entity_s_def *> defs;
        if ( SelectedModelEntities( &defs ) == 0 )
        {
            if ( err ) _snprintf( err, errSz, "no misc_model is selected in the 3D view" );
            return false;
        }
        std::string name = modelName;
        for ( size_t i = 0; i < name.size(); ++i )
            if ( name[i] == '\\' ) name[i] = '/';
        if ( !_strnicmp( name.c_str(), "xmodel/", 7 ) )
            name.erase( 0, 7 );

        Undo_ClearRedo();
        Undo_GeneralStart( "swap model in place" );
        int swapped = 0;
        for ( size_t i = 0; i < defs.size(); ++i )
        {
            const char *old = nullptr;
            for ( const epair_t *ep = defs[i]->epairs; ep && !old; ep = ep->next )
                if ( ep->key && !_stricmp( ep->key, "model" ) )
                    old = ep->value ? ep->value : "";
            if ( old && !_stricmp( old, name.c_str() ) )
                continue;                               // already that model
            Undo_AddEntity_W( (entity_s *)defs[i] );
            SetKeyValue( defs[i], "model", name.c_str() );   // Checkkey_Model + EntityAssignModel
            ++swapped;
        }
        Undo_End();
        MarkMapModified();
        g_nUpdateBits = -1;
        Sys_Printf( "Models browser: swapped %i of %i selected model(s) to '%s' in place.\n",
                    swapped, (int)defs.size(), name.c_str() );
        if ( swapped == 0 && err )
            _snprintf( err, errSz, "the selected model(s) already use '%s'", name.c_str() );
        return swapped > 0;
    }

    void DeleteModelFromDisk( const char *modelName )
    {
        std::vector<std::string> loose, packed;
        if ( !KiwiThumbCache_ResolveModelFiles( modelName, &loose, &packed ) )
        {
            Sys_Printf( "Models browser: '%s' is not on the search path; nothing to delete.\n", modelName );
            return;
        }
        if ( loose.empty() )
        {
            Sys_Printf( "Models browser: '%s' has no loose files (packed in an IWD); nothing deleted.\n", modelName );
            return;
        }
        std::string text = "PERMANENTLY delete the xmodel \"";
        text += modelName;
        text += "\" from disk?\n\nThese files will be deleted:\n";
        for ( size_t i = 0; i < loose.size(); ++i )
            text += "  " + loose[i] + "\n";
        if ( !packed.empty() )
        {
            text += "\nLeft alone (packed in an IWD):\n";
            for ( size_t i = 0; i < packed.size() && i < 8; ++i )
                text += "  " + packed[i] + "\n";
        }
        text += "\nThis cannot be undone.";
        const int answer = ::MessageBoxA( g_qeglobals.d_hwndMain, text.c_str(),
                                          "Delete xmodel from disk",
                                          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 );
        if ( answer != IDYES )
            return;
        int deleted = 0;
        for ( size_t i = 0; i < loose.size(); ++i )
        {
            if ( ::DeleteFileA( loose[i].c_str() ) )
            {
                ++deleted;
                Sys_Printf( "Models browser: deleted \"%s\".\n", loose[i].c_str() );
            }
            else
                Sys_Printf( "Models browser: could not delete \"%s\" (Win32 error %lu).\n",
                            loose[i].c_str(), (unsigned long)::GetLastError() );
        }
        Sys_Printf( "Models browser: '%s' - %i of %i file(s) deleted. Placed instances keep "
                    "their resident copy until the editor restarts.\n",
                    modelName, deleted, (int)loose.size() );
        KiwiThumbCache_Invalidate( modelName );
        s_rescanPending = true;
    }

    void DrawTile( const modelRow_t &row )
    {
        const float labelH = ImGui::GetTextLineHeight();
        const ImVec2 cellSize( KMODEL_TILE_W, KMODEL_TILE_H + labelH + 2.0f );
        ImGui::PushID( row.name.c_str() );
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton( "##model", cellSize );
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();
        const ImVec2 p1( p0.x + cellSize.x, p0.y + cellSize.y );

        IDirect3DTexture9 *thumb = KiwiEntThumb_GetModel(
            row.name.c_str(), ImGui::IsRectVisible( p0, p1 ) );
        const bool failed = KiwiEntThumb_ModelFailed( row.name.c_str() );
        float mins[3], maxs[3];
        TileBounds( row.name.c_str(), mins, maxs );

        if ( !failed && ImGui::BeginDragDropSource(
                ImGuiDragDropFlags_SourceNoHoldToOpenOthers ) )
        {
            ImGui::SetDragDropPayload( KMODEL_PAYLOAD, row.name.c_str(),
                                       row.name.size() + 1 );
            ImGui::TextUnformatted( row.name.c_str() );
            const ImVec2 gp = ImGui::GetCursorScreenPos();
            ImGui::Dummy( ImVec2( KMODEL_TILE_W, KMODEL_TILE_H ) );
            const ImVec2 gmax( gp.x + KMODEL_TILE_W, gp.y + KMODEL_TILE_H );
            ImDrawList *gdl = ImGui::GetWindowDrawList();
            if ( thumb )
            {
                const float side = KMODEL_TILE_H;
                const float cx = ( gp.x + gmax.x ) * 0.5f;
                const float cy = ( gp.y + gmax.y ) * 0.5f;
                gdl->AddImage( (ImTextureID)(intptr_t)thumb,
                               ImVec2( cx - side * 0.5f, cy - side * 0.5f ),
                               ImVec2( cx + side * 0.5f, cy + side * 0.5f ) );
            }
            else
            {
                DrawIsoBox( gdl, gp, gmax, mins, maxs, false, true );
            }
            ImGui::TextDisabled( "drop in the 3D view" );
            ImGui::EndDragDropSource();
        }

        // Right-click reveals the asset on disk. Resolved through the same
        // search-path walk the loader uses, so a shadowed copy is never shown and an
        // IWD member (nothing loose to open) offers the containing .iwd instead.
        // Must follow the InvisibleButton: BeginPopupContextItem tests the last item.
        if ( ImGui::BeginPopupContextItem( "##modelctx" ) )
        {
            char container[512];
            bool loose = false;
            const bool found = KiwiThumbCache_ResolveModelSource(
                row.name.c_str(), container, sizeof( container ), &loose );
            if ( found && loose )
            {
                if ( ImGui::MenuItem( "Open in Explorer" ) &&
                     !KiwiWindows_RevealInExplorer( container ) )
                    Sys_Printf( "Models browser: could not open Explorer for '%s'.\n",
                                container );
            }
            else if ( found )
            {
                ImGui::MenuItem( "Open in Explorer", nullptr, false, false );
                ImGui::TextDisabled( "packed in an IWD; no loose file" );
                if ( ImGui::MenuItem( "Reveal IWD in Explorer" ) &&
                     !KiwiWindows_RevealInExplorer( container ) )
                    Sys_Printf( "Models browser: could not open Explorer for '%s'.\n",
                                container );
            }
            else
            {
                ImGui::MenuItem( "Open in Explorer", nullptr, false, false );
                ImGui::TextDisabled( "not found on the search path" );
            }
            // KIWI (2026-09-13, user: "right click permanent delete on xmodels ... with a
            // confirm prompt"): the xmodel and the parts/surfs its header names, loose
            // files only (IWD members are listed as untouchable), behind a Yes/No box.
            ImGui::Separator();
            if ( ImGui::MenuItem( "Delete from disk...", nullptr, false, found && loose ) )
                DeleteModelFromDisk( row.name.c_str() );
            ImGui::EndPopup();
        }

        // A plain click (no drag travel) picks the tile for "Swap in place".
        if ( hovered && !failed && ImGui::IsMouseReleased( ImGuiMouseButton_Left )
          && !ImGui::IsMouseDragPastThreshold( ImGuiMouseButton_Left ) )
            s_pickedModel = row.name;
        const bool picked = !s_pickedModel.empty() && NameEqual( s_pickedModel, row.name );

        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 boxMax( p0.x + KMODEL_TILE_W, p0.y + KMODEL_TILE_H );
        if ( hovered || active )
            dl->AddRectFilled( p0, p1, ImGui::GetColorU32( ImGuiCol_FrameBgHovered ), 3.0f );
        if ( picked )
            dl->AddRect( p0, p1, ImGui::GetColorU32( ImGuiCol_ButtonActive ), 3.0f, 0, 2.0f );
        if ( thumb && !failed )
        {
            const float side = KMODEL_TILE_H;
            const float cx = ( p0.x + boxMax.x ) * 0.5f;
            const float cy = ( p0.y + boxMax.y ) * 0.5f;
            dl->AddImage( (ImTextureID)(intptr_t)thumb,
                          ImVec2( cx - side * 0.5f, cy - side * 0.5f ),
                          ImVec2( cx + side * 0.5f, cy + side * 0.5f ) );
        }
        else
        {
            DrawIsoBox( dl, p0, boxMax, mins, maxs, failed, hovered );
        }
        if ( failed )
        {
            dl->AddRectFilled( p0, boxMax, ImGui::GetColorU32( ImVec4( 0.1f, 0.1f, 0.1f, 0.5f ) ) );
            dl->AddText( ImVec2( p0.x + 3.0f, p0.y + 2.0f ),
                         ImGui::GetColorU32( ImGuiCol_TextDisabled ), "BAD" );
        }

        dl->PushClipRect( ImVec2( p0.x, boxMax.y ), p1, true );
        const ImGuiCol nameStyle = failed || !hovered
                                 ? ImGuiCol_TextDisabled : ImGuiCol_Text;
        dl->AddText( ImVec2( p0.x + 2.0f, boxMax.y + 1.0f ),
                     ImGui::GetColorU32( nameStyle ), row.name.c_str() );
        dl->PopClipRect();

        if ( hovered && !failed && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
            QueueDrop( row.name.c_str(), -1, -1 );
        if ( hovered && !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            DrawTooltip( row, failed, thumb != nullptr );
        ImGui::PopID();
    }

    bool PlacementBounds( const char *modelName, eclass_t *miscModel,
                          float mins[3], float maxs[3] )
    {
        if ( KiwiEntThumb_GetModelBounds( modelName, mins, maxs ) &&
             BoundsValid( mins, maxs ) )
            return true;
        if ( !miscModel )
            return false;
        for ( int i = 0; i < 3; ++i )
        {
            mins[i] = miscModel->mins[i];
            maxs[i] = miscModel->maxs[i];
        }
        return BoundsValid( mins, maxs );
    }

    bool ResolvePlacement( const char *modelName, int imgX, int imgY,
                           float outOrigin[3], float outMins[3], float outMaxs[3] )
    {
        ray_t ray;
        if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
            return false;

        eclass_t *miscModel = Eclass_ForName( 0, "misc_model" );
        float modelMins[3], modelMaxs[3];
        if ( !PlacementBounds( modelName, miscModel, modelMins, modelMaxs ) )
            return false;
        const float angles[3] = { 0.0f, 0.0f, 0.0f };
        return KiwiDrop_ComputePlacement( ray, modelMins, modelMaxs,
                                          angles, 1.0f, outOrigin,
                                          outMins, outMaxs );
    }

    bool DropPlaceholder( eclass_t *miscModel, const float origin[3] )
    {
        // Entity_Create derives origin from placeholder mins minus eclass mins, so use
        // the eclass box here; real model bounds only drive resting and the ghost.
        float mins[3], maxs[3];
        for ( int i = 0; i < 3; ++i )
        {
            mins[i] = origin[i] + miscModel->mins[i];
            maxs[i] = origin[i] + miscModel->maxs[i];
            if ( maxs[i] - mins[i] < 1.0f )
                maxs[i] = mins[i] + 1.0f;
        }

        Ed_EnsureCurrentMaterial_Kiwi();
        brush_t *def = Brush_Alloc( g_qeglobals.random_texture_stuff, nullptr );
        if ( !def )
        {
            Sys_Printf( "Models browser: brush allocation failed.\n" );
            return false;
        }
        Brush_Create( mins, maxs, def, nullptr );
        Brush_BuildWindings( def, 1 );
        KiwiExtrude_LandDef( def );
        return true;
    }

    bool PerformDrop( const char *modelName, int imgX, int imgY,
                      const float *latchedOrigin, bool prefab = false )
    {
        if ( !modelName || !modelName[0] )
        {
            Sys_Printf( "Models browser: the drop carried no model name.\n" );
            return false;
        }
        if ( !prefab && KiwiEntThumb_ModelFailed( modelName ) )
        {
            Sys_Printf( "Models browser: '%s' failed its preview load and was not placed.\n",
                        modelName );
            return false;
        }

        if ( KiwiEditorCommand *live = KiwiCmd_Active() )
        {
            if ( live->PreemptIdle() )
                KiwiCmd_Cancel();
            else if ( live == KiwiXform_CommandForId( KIWI_CMD_MOVE )
                   || live == KiwiXform_CommandForId( KIWI_CMD_ROTATE )
                   || live == KiwiXform_CommandForId( KIWI_CMD_SCALE ) )
            {
                // KIWI (2026-09-09, user: "I should be able to chain drag out models"):
                // the previous drop left its model in a PAUSED Move (the placement gesture);
                // a new drag from the browser confirms that placement where it stands and
                // carries on, instead of refusing until the operator presses Enter.
                KiwiCmd_Commit();
            }
            else
            {
                Sys_Printf( "Models browser: finish or cancel \"%s\" before placing a model.\n",
                            live->Name() );
                return false;
            }
        }

        const char *className = prefab ? "misc_prefab" : "misc_model";
        eclass_t *miscModel = Eclass_ForName( 0, className );
        if ( !miscModel || *(int *)&miscModel->fixedsize == 0 )
        {
            Sys_Printf( "Models browser: %s is not available.\n", className );
            return false;
        }

        float origin[3];
        if ( latchedOrigin )
        {
            for ( int k = 0; k < 3; ++k )
                origin[k] = latchedOrigin[k];
        }
        else
        {
            float modelMins[3], modelMaxs[3];
            if ( !ResolvePlacement( modelName, imgX, imgY,
                                    origin, modelMins, modelMaxs ) )
            {
                Sys_Printf( "Models browser: the drop ray has no valid placement; nothing placed.\n" );
                return false;
            }
        }

        Select_Deselect( 1 );
        // Keep the ported placeholder/entity sequence and model epair in one undo record.
        Undo_ClearRedo();
        Undo_GeneralStart( "create entity" );
        if ( !DropPlaceholder( miscModel, origin ) )
        {
            Undo_End();
            return false;
        }
        CreateEntityFromName( className );

        entity_s_def *created = nullptr;
        selbrush_t *selected = selected_brushes.next;
        if ( selected && selected != &selected_brushes &&
             selected->next == &selected_brushes && selected->owner &&
             selected->owner != world_entity )
        {
            entity_s_def *candidate = (entity_s_def *)selected->owner->def;
            if ( candidate && candidate->eclass == miscModel )
                created = candidate;
        }
        // No angles epair is authored; absence means zero rotation.
        if ( created )
            SetKeyValue( created, "model", modelName );
        Undo_End();

        if ( !created )
        {
            Sys_Printf( "Models browser: %s creation failed; no model key was written.\n", className );
            return false;
        }

        g_nUpdateBits = -1;
        char x[32], y[32], z[32];
        Sys_Printf( "Placed %s '%s' at %s %s %s.\n", className, modelName,
                    KiwiFmt_Num( x, sizeof( x ), origin[0] ),
                    KiwiFmt_Num( y, sizeof( y ), origin[1] ),
                    KiwiFmt_Num( z, sizeof( z ), origin[2] ) );
        KiwiCmd_AfterPaste();
        return true;
    }

    bool PayloadName( const ImGuiPayload *payload, std::string &out )
    {
        out.clear();
        if ( !payload || !payload->Data || payload->DataSize <= 0 )
            return false;
        const char *data = (const char *)payload->Data;
        int length = 0;
        while ( length < payload->DataSize && data[length] )
            ++length;
        if ( length <= 0 )
            return false;
        out.assign( data, (size_t)length );
        return true;
    }
}

void KiwiModelBrowser_Draw()
{
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_MODELS );
    if ( !open || !*open )
        return;
    if ( KiwiWindows_JustOpened( KIWI_WIN_MODELS ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );
    if ( !s_enumerated )
        EnumerateModels();

    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_MODELS ), open ) )
    {
        ImGui::SetNextItemWidth( 140.0f );
        ImGui::InputTextWithHint( "##modelsearch", "search", s_filter, sizeof( s_filter ) );
        if ( s_filter[0] )
        {
            ImGui::SameLine();
            if ( ImGui::SmallButton( "x##modelsearchclear" ) )
                s_filter[0] = '\0';
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 120.0f );
        const char *prefixPreview = s_prefix.empty() ? "All prefixes" : s_prefix.c_str();
        if ( ImGui::BeginCombo( "##modelprefix", prefixPreview ) )
        {
            if ( ImGui::Selectable( "All prefixes", s_prefix.empty() ) )
                s_prefix.clear();
            for ( size_t i = 0; i < s_prefixes.size(); ++i )
            {
                const bool selected = !s_prefix.empty() && NameEqual( s_prefix, s_prefixes[i] );
                if ( ImGui::Selectable( s_prefixes[i].c_str(), selected ) )
                    s_prefix = s_prefixes[i];
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if ( ImGui::SmallButton( "Refresh" ) )
            EnumerateModels();
        if ( ImGui::SmallButton( "Clear thumbnail cache" ) )
            KiwiThumbCache_InvalidateAll();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Delete cached model/entity thumbnails; visible tiles reload lazily" );

        {
            const int selectedModels = SelectedModelEntities( nullptr );
            const bool canSwap = selectedModels > 0 && !s_pickedModel.empty();
            ImGui::BeginDisabled( !canSwap );
            if ( ImGui::Button( "Swap in place with new model" ) )
            {
                char err[256];
                if ( !SwapSelectedModels( s_pickedModel.c_str(), err, sizeof( err ) ) )
                    Sys_Printf( "Models browser: swap failed - %s.\n", err );
            }
            ImGui::EndDisabled();
            if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            {
                if ( canSwap )
                    ImGui::SetTooltip( "Replace the %i selected model(s) with '%s' at the same origin,\n"
                                       "angles and scale; every other key is kept. Undoable.",
                                       selectedModels, s_pickedModel.c_str() );
                else
                    ImGui::SetTooltip( "Select the model(s) to replace in the 3D view, then click a tile\n"
                                       "here to pick the replacement." );
            }
            if ( !s_pickedModel.empty() )
            {
                ImGui::SameLine();
                ImGui::TextDisabled( "picked: %s", s_pickedModel.c_str() );
                ImGui::SameLine();
                if ( ImGui::SmallButton( "x##unpick" ) )
                    s_pickedModel.clear();
            }
        }
        ImGui::Separator();

        if ( s_rescanPending )
        {
            s_rescanPending = false;
            EnumerateModels();               // a delete happened last frame
        }

        std::vector<int> visible;
        visible.reserve( s_models.size() );
        for ( size_t i = 0; i < s_models.size(); ++i )
        {
            if ( !s_prefix.empty() && !NameEqual( s_prefix, s_models[i].prefix ) )
                continue;
            if ( !KiwiStr_ContainsNoCase( s_models[i].name.c_str(), s_filter ) )
                continue;
            visible.push_back( (int)i );
        }

        ImGui::TextDisabled( "%i of %i models", (int)visible.size(), (int)s_models.size() );
        ImGui::BeginChild( "##modeltiles", ImVec2( 0.0f, 0.0f ), 0,
                           ImGuiWindowFlags_HorizontalScrollbar );
        const float cellW = KMODEL_TILE_W + ImGui::GetStyle().ItemSpacing.x;
        int perRow = (int)( ImGui::GetContentRegionAvail().x / cellW );
        if ( perRow < 1 )
            perRow = 1;
        const int rowCount = ( (int)visible.size() + perRow - 1 ) / perRow;
        const float rowH = KMODEL_TILE_H + ImGui::GetTextLineHeight() + 2.0f +
                           ImGui::GetStyle().ItemSpacing.y;
        ImGuiListClipper clipper;
        clipper.Begin( rowCount, rowH );
        while ( clipper.Step() )
        {
            for ( int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row )
            {
                const int first = row * perRow;
                for ( int col = 0; col < perRow; ++col )
                {
                    const int visibleIndex = first + col;
                    if ( visibleIndex >= (int)visible.size() )
                        break;
                    if ( col > 0 )
                        ImGui::SameLine();
                    DrawTile( s_models[visible[visibleIndex]] );
                }
            }
        }
        if ( visible.empty() )
        {
            if ( s_models.empty() )
                ImGui::TextDisabled( "No xmodels were returned by the editor filesystem." );
            else
                ImGui::TextDisabled( "No model matches the current filters." );
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

bool KiwiModelBrowser_CameraDropTarget( float imgMinX, float imgMinY )
{
    if ( ImGui::GetDragDropPayload() == nullptr )
    {
        s_ghostHave = false;
        s_ghostModel.clear();
    }
    if ( !ImGui::BeginDragDropTarget() )
    {
        s_ghostHave = false;
        s_ghostModel.clear();
        return false;
    }

    const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(
        KMODEL_PAYLOAD,
        ImGuiDragDropFlags_AcceptBeforeDelivery |
        ImGuiDragDropFlags_AcceptNoDrawDefaultRect );
    bool took = false;
    std::string modelName;
    if ( payload && PayloadName( payload, modelName ) &&
         !KiwiEntThumb_ModelFailed( modelName.c_str() ) )
    {
        if ( _stricmp( s_ghostModel.c_str(), modelName.c_str() ) != 0 )
        {
            s_ghostHave = false;
            s_ghostModel = modelName;
        }
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const int x = (int)( mouse.x - imgMinX );
        const int y = (int)( mouse.y - imgMinY );
        float origin[3], mins[3], maxs[3];
        if ( ResolvePlacement( modelName.c_str(), x, y, origin, mins, maxs ) )
        {
            for ( int k = 0; k < 3; ++k )
            {
                s_ghostOrigin[k] = origin[k];
                s_ghostMins[k] = mins[k];
                s_ghostMaxs[k] = maxs[k];
            }
            s_ghostHave = true;
        }
        // Hold the last valid box through parallel/upward misses; delivery latches it.
        if ( payload->Delivery )
        {
            took = QueueDrop( modelName.c_str(), x, y,
                              s_ghostHave ? s_ghostOrigin : 0 );
            s_ghostHave = false;
            s_ghostModel.clear();
        }
    }
    else
    {
        s_ghostHave = false;
        s_ghostModel.clear();
    }
    ImGui::EndDragDropTarget();
    return took;
}

void KiwiModelBrowser_DrawGhost()
{
    if ( !s_ghostHave )
        return;
    float corners[KIWI_BOX_CORNERS][3];
    KiwiBox_Corners( s_ghostMins, s_ghostMaxs, corners );
    KiwiLines_Begin( KIWI_BOX_EDGES, 2 );
    KiwiLines_Color( 0.45f, 0.85f, 1.0f );
    for ( int i = 0; i < KIWI_BOX_EDGES; ++i )
        if ( !KiwiLines_Add( corners[KIWI_BOX_EDGE[i][0]],
                             corners[KIWI_BOX_EDGE[i][1]] ) )
            break;
    KiwiLines_Flush();
}

void KiwiModelBrowser_RegisterCommands()
{
    KiwiThumbCache_Init();
    Radiant_RegisterCommand( "KiwiWindowModels", 0, 0, KIWI_CMD_WINDOW_MODELS );
    Radiant_RegisterCommand( "KiwiInsertPrefab", 0, 0, KIWI_CMD_PREFAB_INSERT );
}

namespace
{
    std::string PrefabRoot()
    {
        if ( g_qeglobals.d_project_entity )
            for ( epair_t *ep = g_qeglobals.d_project_entity->epairs; ep; ep = ep->next )
                if ( !_stricmp( ep->key, "mapspath" ) && ep->value && ep->value[0] )
                    return ep->value;
        char exe[MAX_PATH] = {};
        ::GetModuleFileNameA( nullptr, exe, MAX_PATH );
        char *slash = strrchr( exe, '\\' );
        if ( slash ) *slash = '\0';
        return std::string( exe ) + "\\map_source";
    }

    std::string FullPrefabPath( const std::string &path )
    {
        char full[MAX_PATH] = {};
        DWORD n = ::GetFullPathNameA( path.c_str(), MAX_PATH, full, nullptr );
        if ( !n || n >= MAX_PATH ) return std::string();
        for ( char *p = full; *p; ++p ) if ( *p == '\\' ) *p = '/';
        return full;
    }
}

bool KiwiModelBrowser_SwapSelected( const char *modelName, char *err, size_t errSz )
{
    return SwapSelectedModels( modelName, err, errSz );
}

bool KiwiModelBrowser_InsertPrefab( const char *relativePath )
{
    if ( !relativePath || !relativePath[0] ) return false;
    std::string root = FullPrefabPath( PrefabRoot() );
    if ( root.empty() ) return false;
    if ( root.back() != '/' ) root += '/';
    const std::string full = FullPrefabPath( root + relativePath );
    extern char currentmap[];
    const std::string current = FullPrefabPath( currentmap );
    if ( full.size() <= root.size() || _strnicmp( full.c_str(), root.c_str(), root.size() )
      || full.size() < 4 || _stricmp( full.c_str() + full.size() - 4, ".map" )
      || !_stricmp( full.c_str(), current.c_str() )
      || ::GetFileAttributesA( full.c_str() ) == INVALID_FILE_ATTRIBUTES )
    {
        Sys_Printf( "Insert Prefab: choose an existing .map inside map_source, other than the current map.\n" );
        return false;
    }
    // File commands execute outside the ImGui frame, so the native model loader is safe.
    const std::string name = full.substr( root.size() );
    // New/unsaved maps may not have a project yet. The native loader must use
    // exactly the same root that the chooser and path validation just used.
    bool haveProjectRoot = false;
    if ( g_qeglobals.d_project_entity )
        for ( epair_t *ep = g_qeglobals.d_project_entity->epairs; ep; ep = ep->next )
            if ( ep->key && !_stricmp( ep->key, "mapspath" ) && ep->value && ep->value[0] )
                haveProjectRoot = true;
    if ( !haveProjectRoot )
        Radiant_SetProjectMapsPath( ( root + "__prefab_insert.map" ).c_str() );
    ray_t ray;
    camera_s *camera = Ed_Camera();
    if ( !camera || !Pick_RayFromImagePos( camera->width / 2, camera->height / 2, &ray ) )
        return false;
    float origin[3], mins[3], maxs[3];
    const float boxMin[3] = { -16, -16, -16 }, boxMax[3] = { 16, 16, 16 };
    const float angles[3] = {};
    if ( !KiwiDrop_ComputePlacement( ray, boxMin, boxMax, angles, 1, origin, mins, maxs ) )
        return false;
    return PerformDrop( name.c_str(), -1, -1, origin, true );
}

void KiwiModelBrowser_BuildFileMenu( void *frameMenu )
{
    HMENU file = ::GetSubMenu( (HMENU)frameMenu, 0 );
    if ( !file || ::GetMenuState( file, KIWI_CMD_PREFAB_INSERT, MF_BYCOMMAND ) != (UINT)-1 ) return;
    if ( !::InsertMenuA( file, 32844, MF_BYCOMMAND | MF_STRING, KIWI_CMD_PREFAB_INSERT, "Insert &Prefab..." ) )
        ::AppendMenuA( file, MF_STRING, KIWI_CMD_PREFAB_INSERT, "Insert &Prefab..." );
}

bool KiwiModelBrowser_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId == KIWI_CMD_PREFAB_INSERT )
    {
        char file[MAX_PATH] = {};
        const std::string root = FullPrefabPath( PrefabRoot() );
        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof( ofn ); ofn.hwndOwner = g_qeglobals.d_hwndMain;
        ofn.lpstrFilter = "Prefab maps (*.map)\0*.map\0\0";
        ofn.lpstrTitle = "Insert Prefab - placed as one selectable object";
        ofn.lpstrInitialDir = root.c_str(); ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if ( ::GetOpenFileNameA( &ofn ) )
        {
            const std::string full = FullPrefabPath( file );
            const std::string prefix = root + '/';
            if ( !_strnicmp( full.c_str(), prefix.c_str(), prefix.size() ) )
                KiwiModelBrowser_InsertPrefab( full.c_str() + prefix.size() );
            else
                Sys_Printf( "Insert Prefab: place the prefab and its dependencies inside %s first.\n", root.c_str() );
        }
        return true;
    }
    if ( cmdId != (unsigned int)KIWI_CMD_MODEL_DROP )
        return false;
    if ( !s_pendHave )
        return true;

    const std::string modelName = s_pendModel;
    int x = s_pendX;
    int y = s_pendY;
    const bool havePlacement = s_pendPlacement;
    float placement[3] = { s_pendOrigin[0], s_pendOrigin[1], s_pendOrigin[2] };
    s_pendHave = false;
    s_pendPlacement = false;
    s_pendModel.clear();
    if ( x < 0 || y < 0 )
    {
        camera_s *camera = Ed_Camera();
        x = camera ? camera->width / 2 : 0;
        y = camera ? camera->height / 2 : 0;
    }
    PerformDrop( modelName.c_str(), x, y, havePlacement ? placement : 0 );
    return true;
}

void KiwiModelBrowser_ResetForNewMap()
{
    s_models.clear();
    s_prefixes.clear();
    s_enumerated = false;
    s_filter[0] = '\0';
    s_prefix.clear();
    s_pendHave = false;
    s_pendPlacement = false;
    s_pendModel.clear();
    s_ghostHave = false;
    s_ghostModel.clear();
}

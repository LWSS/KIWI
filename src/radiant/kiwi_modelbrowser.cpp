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
#include "kiwi_ported.h"
#include "kiwi_str.h"
#include "kiwi_transform.h"
#include "kiwi_windows.h"
#include "radiant_registry.h"   // the dyn_model checkbox's remembered state
#include <xanim/xmodel.h>       // XModel collision surfaces (hitbox view)
#include "modelimport/ki_autocollide.h"   // player hulls from the mesh (match collision)
#include "modelimport/ki_collision.h"     // WriteCollmap
#include <filesystem>

#include <algorithm>
#include <float.h>
#include <map>
#include <random>
#include <string>
#include <vector>
#include <string.h>
#include <commdlg.h>
#include <imgui_internal.h>

extern void        Undo_KiwiMarkCreated( brush_t *def );                  // undo.cpp (KIWI tail)
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

    // KIWI (2026-09-16, user: "a small icon/banner in top corner of the thumbnail if the
    // model supports physics collision"): the header's physics preset, looked up lazily
    // for tiles that are actually on screen and remembered for the session.  A few header
    // reads per frame at most, so scrolling a 4,600-model list never hitches.
    std::map<std::string, std::string> s_physPreset;      // name -> preset ("" = none)
    int s_physFrame  = -1;
    int s_physBudget = 0;
    const int KMODEL_PHYS_PER_FRAME = 12;

    // Null while unknown (not looked up yet); else the preset, "" when the model has none.
    const std::string *PhysPreset( const std::string &name, bool mayLoad )
    {
        std::map<std::string, std::string>::iterator it = s_physPreset.find( name );
        if ( it != s_physPreset.end() )
            return &it->second;
        if ( !mayLoad )
            return nullptr;
        const int frame = ImGui::GetFrameCount();
        if ( frame != s_physFrame )
        {
            s_physFrame  = frame;
            s_physBudget = KMODEL_PHYS_PER_FRAME;
        }
        if ( s_physBudget <= 0 )
            return nullptr;
        --s_physBudget;
        std::string preset;
        KiwiThumbCache_ModelPhysPreset( name.c_str(), &preset );
        return &s_physPreset.insert( std::make_pair( name, preset ) ).first->second;
    }

    void DrawTooltip( const modelRow_t &row, bool failed, bool ready )
    {
        if ( !ImGui::BeginTooltip() )
            return;
        ImGui::TextUnformatted( row.name.c_str() );
        ImGui::Separator();
        {
            const std::string *preset = PhysPreset( row.name, true );
            if ( preset && !preset->empty() )
                ImGui::TextColored( ImVec4( 1.0f, 0.72f, 0.25f, 1.0f ),
                                    "PHYSICS prop (preset \"%s\") - place it as a dyn_model to be shot around",
                                    preset->c_str() );
        }

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

    // KIWI (2026-10-15, user: "randomize variants with a certain name ... I copied and pasted a
    // lot of triwindows, but only 1 variant. Now I want to assort them so it doesn't look
    // uniform"): a variant family is <base> plus every listed <base>_<n> (the importer's naming:
    // variant 1 keeps the bare name).
    bool HasModel( const std::string &name )
    {
        for ( size_t i = 0; i < s_models.size(); ++i )
            if ( NameEqual( s_models[i].name, name ) )
                return true;
        return false;
    }

    bool AllDigits( const char *s )
    {
        if ( !*s )
            return false;
        for ( ; *s; ++s )
            if ( *s < '0' || *s > '9' )
                return false;
        return true;
    }

    std::string FamilyBase( const std::string &name )
    {
        const size_t u = name.rfind( '_' );
        if ( u != std::string::npos && u > 0 && AllDigits( name.c_str() + u + 1 ) && HasModel( name.substr( 0, u ) ) )
            return name.substr( 0, u );
        return name;
    }

    void FamilyMembers( const std::string &base, std::vector<std::string> *out )
    {
        out->clear();
        if ( HasModel( base ) )
            out->push_back( base );
        for ( size_t i = 0; i < s_models.size(); ++i )
        {
            const std::string &n = s_models[i].name;
            if ( n.size() > base.size() + 1 && !_strnicmp( n.c_str(), base.c_str(), base.size() )
              && n[base.size()] == '_' && AllDigits( n.c_str() + base.size() + 1 ) )
                out->push_back( n );
        }
    }

    std::string ModelKey( const entity_s_def *def )
    {
        for ( const epair_t *ep = def->epairs; ep; ep = ep->next )
            if ( ep->key && !_stricmp( ep->key, "model" ) )
            {
                std::string name = ep->value ? ep->value : "";
                NormalizeListedName( name );
                return name;
            }
        return std::string();
    }

    // Every placed model entity (selected or not), each once.
    void PlacedModelEntities( std::vector<entity_s_def *> *out )
    {
        out->clear();
        for ( selbrush_t *list : { &active_brushes, &selected_brushes } )
            for ( selbrush_t *b = list->next; b && b != list; b = b->next )
            {
                if ( !b->owner || !b->owner->def || !KiwiDrop_IsModelEntity( b ) )
                    continue;
                entity_s_def *def = (entity_s_def *)b->owner->def;
                if ( def->eclass && ( def->eclass->classtype & 0x10 ) )
                    continue;
                if ( std::find( out->begin(), out->end(), def ) == out->end() )
                    out->push_back( def );
            }
    }

    // The selected models, else every placed copy of the picked tile's family: each family gets
    // its variants dealt out evenly in a random order, so no variant dominates a run of copies.
    void RandomizeVariants( const std::string &picked )
    {
        std::vector<entity_s_def *> defs;
        const bool fromSelection = SelectedModelEntities( &defs ) > 0;
        if ( !fromSelection )
        {
            if ( picked.empty() )
            {
                Sys_Printf( "Models browser: select the models to assort, or click a tile to assort every copy of it.\n" );
                return;
            }
            const std::string base = FamilyBase( picked );
            std::vector<entity_s_def *> all;
            PlacedModelEntities( &all );
            for ( entity_s_def *d : all )
                if ( NameEqual( FamilyBase( ModelKey( d ) ), base ) )
                    defs.push_back( d );
        }

        std::map<std::string, std::vector<entity_s_def *>> families;
        for ( entity_s_def *d : defs )
        {
            std::string base = FamilyBase( ModelKey( d ) );
            std::transform( base.begin(), base.end(), base.begin(), ::tolower );
            if ( !base.empty() )
                families[base].push_back( d );
        }

        std::mt19937 rng( (unsigned)::GetTickCount() );
        Undo_ClearRedo();
        Undo_GeneralStart( "randomize model variants" );
        int changed = 0, assorted = 0, single = 0;
        for ( auto &fam : families )
        {
            std::vector<std::string> members;
            FamilyMembers( fam.first, &members );
            if ( members.size() < 2 )
            {
                single += (int)fam.second.size();
                continue;
            }
            std::vector<size_t> deal( fam.second.size() );
            for ( size_t i = 0; i < deal.size(); ++i )
                deal[i] = i % members.size();
            std::shuffle( deal.begin(), deal.end(), rng );
            for ( size_t i = 0; i < fam.second.size(); ++i )
            {
                entity_s_def *d = fam.second[i];
                ++assorted;
                const std::string &want = members[deal[i]];
                if ( NameEqual( ModelKey( d ), want ) )
                    continue;
                Undo_AddEntity_W( (entity_s *)d );
                SetKeyValue( d, "model", want.c_str() );   // Checkkey_Model + EntityAssignModel
                ++changed;
            }
        }
        Undo_End();
        if ( changed )
        {
            MarkMapModified();
            g_nUpdateBits = -1;
        }
        Sys_Printf( "Models browser: assorted %i model(s) across their variants (%i changed)%s.\n",
                    assorted, changed, fromSelection ? "" : " - every placed copy of the picked family" );
        if ( single )
            Sys_Printf( "Models browser: %i model(s) have no other variants and were left alone.\n", single );
    }

    // KIWI (2026-10-15, user: "can you show how many # of these variants is in-use on the map
    // when I select it"): the picked tile's family (else the first selected model's), each
    // variant's placed count; recounted a few times a second, not every frame.
    struct variantUsage_t
    {
        std::string              base;
        std::vector<std::string> members;
        std::vector<int>         placed;
        std::vector<int>         selected;
        double                   when = -1.0;
    };
    variantUsage_t s_usage;

    void DrawVariantUsage()
    {
        std::string source = s_pickedModel;
        if ( source.empty() )
        {
            std::vector<entity_s_def *> sel;
            if ( SelectedModelEntities( &sel ) > 0 )
                source = ModelKey( sel[0] );
        }
        if ( source.empty() )
            return;
        const std::string base = FamilyBase( source );
        const double now = ImGui::GetTime();
        if ( !NameEqual( base, s_usage.base ) || now - s_usage.when > 0.25 )
        {
            s_usage.base = base;
            s_usage.when = now;
            FamilyMembers( base, &s_usage.members );
            s_usage.placed.assign( s_usage.members.size(), 0 );
            s_usage.selected.assign( s_usage.members.size(), 0 );
            std::vector<entity_s_def *> all, sel;
            PlacedModelEntities( &all );
            SelectedModelEntities( &sel );
            for ( entity_s_def *d : all )
            {
                const std::string name = ModelKey( d );
                for ( size_t i = 0; i < s_usage.members.size(); ++i )
                    if ( NameEqual( name, s_usage.members[i] ) )
                    {
                        ++s_usage.placed[i];
                        if ( std::find( sel.begin(), sel.end(), d ) != sel.end() )
                            ++s_usage.selected[i];
                        break;
                    }
            }
        }
        if ( s_usage.members.size() < 2 )
            return;                                      // no variants to compare
        int total = 0, totalSel = 0;
        for ( size_t i = 0; i < s_usage.members.size(); ++i )
        {
            total    += s_usage.placed[i];
            totalSel += s_usage.selected[i];
        }
        ImGui::TextDisabled( "In map: %s  %i placed", s_usage.base.c_str(), total );
        if ( totalSel )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "(%i selected)", totalSel );
        }
        for ( size_t i = 0; i < s_usage.members.size(); ++i )
        {
            const std::string &m = s_usage.members[i];
            const char *label = m.size() > base.size() ? m.c_str() + base.size() : "base";
            char text[96];
            if ( s_usage.selected[i] )
                _snprintf( text, sizeof( text ), "%s: %i (%i sel)", label, s_usage.placed[i], s_usage.selected[i] );
            else
                _snprintf( text, sizeof( text ), "%s: %i", label, s_usage.placed[i] );
            text[sizeof( text ) - 1] = '\0';
            const float w = ImGui::CalcTextSize( text ).x;
            if ( i > 0 && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x * 2.0f + w
                              < ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x )
                ImGui::SameLine( 0.0f, ImGui::GetStyle().ItemSpacing.x * 2.0f );
            if ( s_usage.placed[i] == 0 )
                ImGui::TextDisabled( "%s", text );
            else
                ImGui::TextUnformatted( text );
        }
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
        else
        {
            // Physics badge, top-right corner.  Only visible tiles ask (and only a few per
            // frame), so the badge fills in a beat after a tile scrolls into view.
            const std::string *preset = PhysPreset( row.name, ImGui::IsRectVisible( p0, p1 ) );
            if ( preset && !preset->empty() )
            {
                const char  *tag = "PHYS";
                const ImVec2 ts  = ImGui::CalcTextSize( tag );
                const ImVec2 b1( boxMax.x - 1.0f, p0.y + 1.0f + ts.y + 2.0f );
                const ImVec2 b0( b1.x - ts.x - 6.0f, p0.y + 1.0f );
                dl->AddRectFilled( b0, b1, IM_COL32( 235, 150, 30, 235 ), 3.0f );
                dl->AddText( ImVec2( b0.x + 3.0f, b0.y + 1.0f ), IM_COL32( 20, 14, 4, 255 ), tag );
            }
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

    // The "Physics (dyn_model)" checkbox, remembered across runs.
    int s_placeDyn = -1;
    bool PlaceDynModel()
    {
        if ( s_placeDyn < 0 )
            s_placeDyn = Radiant_ProfileGetInt( "KiwiUX", "ModelsPlaceDynModel", 0 ) != 0 ? 1 : 0;
        return s_placeDyn != 0;
    }
    void SetPlaceDynModel( bool on )
    {
        s_placeDyn = on ? 1 : 0;
        Radiant_ProfileSetInt( "KiwiUX", "ModelsPlaceDynModel", s_placeDyn );
    }
    const std::string &PlacePreset();   // the physPreset combo, below

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
        // KIWI (2026-09-16, user: "undo'ing ... placing a model leaves behind square
        // brushes"): the placeholder is born INSIDE the open record, but CreateEntityFromName
        // opens with Undo_AddBrushList(selected) which cloned it as if it had existed before
        // - and Undo_Undo restored that clone after freeing the entity.  Stamping it as
        // created makes Undo_AddBrush skip it (no clone); Entity_Create's tail deletes the
        // placeholder itself, so nothing is left for either phase.
        Undo_KiwiMarkCreated( def );
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

        // KIWI (2026-10-21, user: "Add a checkbox in the Models Window that creates a model as
        // dyn_model (so it has physics)").  The game drops the whole map on a dyn_model whose
        // xmodel has no collision LOD (DynEntity_load_obj.cpp), so those stay misc_model.
        bool dyn = !prefab && PlaceDynModel();
        if ( dyn )
        {
            kiwiModelStats_t stats;
            if ( KiwiEntThumb_ModelStats( modelName, &stats ) && stats.collLod < 0 )
            {
                Sys_Printf( "Models browser: '%s' has no collision LOD, which a dyn_model needs - "
                            "placed as misc_model instead.\n", modelName );
                dyn = false;
            }
        }
        const char *className = prefab ? "misc_prefab" : dyn ? "dyn_model" : "misc_model";
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
        {
            SetKeyValue( created, "model", modelName );
            if ( dyn )
                SetKeyValue( created, "physPreset", PlacePreset().c_str() );   // else the game logs an error per prop
        }
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

    // ═════════════════════════════════════════════════════════════════════════════════
    //  SELECTED MODEL: info, physPreset, hitboxes  (KIWI 2026-10-21, user: "for dyn_models
    //  (and similar), I want a dropdown for the physPreset selector. I want more information
    //  accessible in the models tab when a model is selected, also a visualization of their
    //  3d hitboxes would be nice too.")
    // ═════════════════════════════════════════════════════════════════════════════════

    std::string ExeDir()
    {
        char path[MAX_PATH] = { 0 };
        ::GetModuleFileNameA( nullptr, path, MAX_PATH );
        char *slash = strrchr( path, '\\' );
        if ( slash )
            *slash = '\0';
        return path;
    }

    // physic\<name> presets: "PHYSIC\key\value\key\value..."; raw\ first, then main\.
    struct physPreset_t
    {
        std::string name;
        std::string text;
    };
    std::vector<physPreset_t> s_presets;
    bool                      s_presetsScanned = false;

    void ScanPresets()
    {
        s_presetsScanned = true;
        s_presets.clear();
        const char *dirs[] = { "raw\\physic", "main\\physic" };
        for ( int d = 0; d < 2; ++d )
        {
            const std::string dir = ExeDir() + "\\" + dirs[d];
            WIN32_FIND_DATAA fd;
            HANDLE h = ::FindFirstFileA( ( dir + "\\*" ).c_str(), &fd );
            if ( h == INVALID_HANDLE_VALUE )
                continue;
            do
            {
                if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
                    continue;
                bool dup = false;
                for ( size_t i = 0; i < s_presets.size() && !dup; ++i )
                    dup = !_stricmp( s_presets[i].name.c_str(), fd.cFileName );
                if ( dup || fd.nFileSizeLow > 4096 )
                    continue;
                physPreset_t p;
                p.name = fd.cFileName;
                if ( FILE *f = fopen( ( dir + "\\" + fd.cFileName ).c_str(), "rb" ) )
                {
                    char buf[4097];
                    const size_t n = fread( buf, 1, 4096, f );
                    buf[n] = '\0';
                    fclose( f );
                    p.text = buf;
                }
                if ( p.text.compare( 0, 6, "PHYSIC" ) != 0 )
                    continue;
                s_presets.push_back( p );
            } while ( ::FindNextFileA( h, &fd ) );
            ::FindClose( h );
        }
        std::sort( s_presets.begin(), s_presets.end(), []( const physPreset_t &a, const physPreset_t &b )
        { return _stricmp( a.name.c_str(), b.name.c_str() ) < 0; } );
    }

    const physPreset_t *PresetNamed( const std::string &name )
    {
        if ( !s_presetsScanned )
            ScanPresets();
        for ( size_t i = 0; i < s_presets.size(); ++i )
            if ( !_stricmp( s_presets[i].name.c_str(), name.c_str() ) )
                return &s_presets[i];
        return nullptr;
    }

    std::string PresetField( const physPreset_t *p, const char *key )
    {
        if ( !p )
            return std::string();
        std::vector<std::string> parts;
        size_t at = 0;
        for ( ;; )
        {
            const size_t bs = p->text.find( '\\', at );
            parts.push_back( p->text.substr( at, bs == std::string::npos ? std::string::npos : bs - at ) );
            if ( bs == std::string::npos )
                break;
            at = bs + 1;
        }
        for ( size_t i = 1; i + 1 < parts.size(); i += 2 )
            if ( !_stricmp( parts[i].c_str(), key ) )
                return parts[i + 1];
        return std::string();
    }

    const char *KeyOfDef( const entity_s_def *def, const char *key )
    {
        for ( const epair_t *ep = def ? def->epairs : nullptr; ep; ep = ep->next )
            if ( ep->key && !_stricmp( ep->key, key ) )
                return ep->value ? ep->value : "";
        return "";
    }

    // dyn_model, or anything already carrying a physPreset key.
    bool UsesPhysPreset( const entity_s_def *def )
    {
        if ( !def )
            return false;
        const char *cn = def->eclass && def->eclass->name ? def->eclass->name : "";
        return !_stricmp( cn, "dyn_model" ) || KeyOfDef( def, "physPreset" )[0];
    }

    // The preset newly placed dyn_models get (Models window combo).
    std::string s_placePreset;
    bool        s_placePresetLoaded = false;
    const std::string &PlacePreset()
    {
        if ( !s_placePresetLoaded )
        {
            s_placePresetLoaded = true;
            s_placePreset = Radiant_ProfileGetString( "KiwiUX", "ModelsPlacePhysPreset", "default" );
            if ( s_placePreset.empty() )
                s_placePreset = "default";
        }
        return s_placePreset;
    }

    // Preset picker; `current` "" = mixed.  Hovering a row shows its values.
    bool PresetCombo( const char *id, const std::string &current, std::string *picked )
    {
        if ( !s_presetsScanned )
            ScanPresets();
        bool changed = false;
        ImGui::SetNextItemWidth( 170.0f );
        if ( ImGui::BeginCombo( id, current.empty() ? "(mixed)" : current.c_str() ) )
        {
            for ( size_t i = 0; i < s_presets.size(); ++i )
            {
                const physPreset_t &p = s_presets[i];
                if ( ImGui::Selectable( p.name.c_str(), !_stricmp( p.name.c_str(), current.c_str() ) ) )
                {
                    *picked = p.name;
                    changed = true;
                }
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "mass %s, friction %s, bounce %s\nbullet force x%s, explosion force x%s%s%s",
                                       PresetField( &p, "mass" ).c_str(), PresetField( &p, "friction" ).c_str(),
                                       PresetField( &p, "bounce" ).c_str(), PresetField( &p, "bulletForceScale" ).c_str(),
                                       PresetField( &p, "explosiveForceScale" ).c_str(),
                                       PresetField( &p, "tempDefaultToCylinder" ) == "1" ? "\nshape: cylinder" : "",
                                       PresetField( &p, "sndAliasPrefix" ).empty() ? ""
                                           : ( "\nsounds: " + PresetField( &p, "sndAliasPrefix" ) ).c_str() );
            }
            if ( s_presets.empty() )
                ImGui::TextDisabled( "no presets in raw\\physic or main\\physic" );
            ImGui::EndCombo();
        }
        return changed;
    }

    void SetPresetOnSelected( const std::string &preset )
    {
        std::vector<entity_s_def *> defs;
        SelectedModelEntities( &defs );
        Undo_ClearRedo();
        Undo_GeneralStart( "set physPreset" );
        int n = 0;
        for ( size_t i = 0; i < defs.size(); ++i )
        {
            if ( !UsesPhysPreset( defs[i] ) || !_stricmp( KeyOfDef( defs[i], "physPreset" ), preset.c_str() ) )
                continue;
            Undo_AddEntity_W( (entity_s *)defs[i] );
            SetKeyValue( defs[i], "physPreset", preset.c_str() );
            ++n;
        }
        Undo_End();
        if ( n )
            MarkMapModified();
        g_nUpdateBits = -1;
        Sys_Printf( "Models browser: physPreset '%s' on %i model(s).\n", preset.c_str(), n );
    }

    // ── player collision hulls: collmaps\<model>.map, model space, edges cached ──
    struct hullEdges_t
    {
        bool               loaded = false;
        int                hulls  = 0;
        std::vector<float> seg;          // x0 y0 z0 x1 y1 z1 ...
    };
    std::map<std::string, hullEdges_t> s_hullCache;

    void HullEdgesOfBrush( const std::vector<double> &pl, hullEdges_t &out )
    {
        // pl: n0 n1 n2 d per plane (outward); corners = triples inside all, edges = 2 shared planes
        const size_t np = pl.size() / 4;
        struct corner_t { double p[3]; int a, b, c; };
        std::vector<corner_t> cs;
        for ( size_t i = 0; i < np; ++i )
            for ( size_t j = i + 1; j < np; ++j )
                for ( size_t k = j + 1; k < np; ++k )
                {
                    const double *A = &pl[i * 4], *B = &pl[j * 4], *C = &pl[k * 4];
                    const double bc[3] = { B[1] * C[2] - B[2] * C[1], B[2] * C[0] - B[0] * C[2], B[0] * C[1] - B[1] * C[0] };
                    const double ca[3] = { C[1] * A[2] - C[2] * A[1], C[2] * A[0] - C[0] * A[2], C[0] * A[1] - C[1] * A[0] };
                    const double ab[3] = { A[1] * B[2] - A[2] * B[1], A[2] * B[0] - A[0] * B[2], A[0] * B[1] - A[1] * B[0] };
                    const double det = A[0] * bc[0] + A[1] * bc[1] + A[2] * bc[2];
                    if ( fabs( det ) < 1e-9 )
                        continue;
                    corner_t c;
                    for ( int t = 0; t < 3; ++t )
                        c.p[t] = ( A[3] * bc[t] + B[3] * ca[t] + C[3] * ab[t] ) / det;
                    bool inside = true;
                    for ( size_t q = 0; q < np && inside; ++q )
                        inside = pl[q * 4] * c.p[0] + pl[q * 4 + 1] * c.p[1] + pl[q * 4 + 2] * c.p[2] - pl[q * 4 + 3] <= 0.01;
                    if ( !inside )
                        continue;
                    c.a = (int)i; c.b = (int)j; c.c = (int)k;
                    cs.push_back( c );
                }
        if ( cs.size() < 4 )
            return;
        ++out.hulls;
        for ( size_t i = 0; i < cs.size(); ++i )
            for ( size_t j = i + 1; j < cs.size(); ++j )
            {
                const double *p = cs[i].p, *q = cs[j].p;
                if ( fabs( p[0] - q[0] ) + fabs( p[1] - q[1] ) + fabs( p[2] - q[2] ) < 0.01 )
                    continue;
                const int ci[3] = { cs[i].a, cs[i].b, cs[i].c }, cj[3] = { cs[j].a, cs[j].b, cs[j].c };
                int shared = 0;
                for ( int a = 0; a < 3; ++a )
                    for ( int b = 0; b < 3; ++b )
                        shared += ( ci[a] == cj[b] );
                if ( shared < 2 )
                    continue;
                for ( int t = 0; t < 3; ++t ) out.seg.push_back( (float)p[t] );
                for ( int t = 0; t < 3; ++t ) out.seg.push_back( (float)q[t] );
            }
    }

    const hullEdges_t &HullEdges( const std::string &model )
    {
        hullEdges_t &h = s_hullCache[model];
        if ( h.loaded )
            return h;
        h.loaded = true;
        std::string file = model;
        for ( size_t i = 0; i < file.size(); ++i )
            if ( file[i] == '/' ) file[i] = '\\';
        FILE *f = fopen( ( ExeDir() + "\\collmaps\\" + file + ".map" ).c_str(), "rb" );
        if ( !f )
            return h;
        std::string text;
        char buf[8192];
        size_t n;
        while ( ( n = fread( buf, 1, sizeof( buf ), f ) ) > 0 )
            text.append( buf, n );
        fclose( f );

        // brushes are the depth-2 blocks; patches inside are skipped
        std::vector<double> planes;
        int depth = 0;
        bool patch = false;
        size_t at = 0;
        while ( at < text.size() )
        {
            size_t eol = text.find( '\n', at );
            if ( eol == std::string::npos ) eol = text.size();
            const std::string line = text.substr( at, eol - at );
            at = eol + 1;
            size_t s = line.find_first_not_of( " \t\r" );
            if ( s == std::string::npos ) continue;
            const char c = line[s];
            if ( c == '{' ) { if ( ++depth == 2 ) { planes.clear(); patch = false; } continue; }
            if ( c == '}' )
            {
                if ( depth == 2 && !patch && planes.size() >= 16 )
                    HullEdgesOfBrush( planes, h );
                --depth;
                continue;
            }
            if ( depth == 2 && ( !line.compare( s, 5, "curve" ) || !line.compare( s, 4, "mesh" ) ) )
                patch = true;
            if ( depth != 2 || c != '(' || patch )
                continue;
            double v[9];
            if ( sscanf( line.c_str() + s, " ( %lf %lf %lf ) ( %lf %lf %lf ) ( %lf %lf %lf )",
                         &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8] ) != 9 )
                continue;
            // outward normal: (p2 - p0) x (p1 - p0), the editor's planept winding
            const double e1[3] = { v[6] - v[0], v[7] - v[1], v[8] - v[2] };
            const double e2[3] = { v[3] - v[0], v[4] - v[1], v[5] - v[2] };
            double nn[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
            const double l = sqrt( nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2] );
            if ( l < 1e-9 ) continue;
            for ( int t = 0; t < 3; ++t ) nn[t] /= l;
            planes.push_back( nn[0] ); planes.push_back( nn[1] ); planes.push_back( nn[2] );
            planes.push_back( nn[0] * v[0] + nn[1] * v[1] + nn[2] * v[2] );
        }
        return h;
    }

    int s_showHitboxes = -1;
    bool ShowHitboxes()
    {
        if ( s_showHitboxes < 0 )
            s_showHitboxes = Radiant_ProfileGetInt( "KiwiUX", "ModelsShowHitboxes", 1 ) ? 1 : 0;
        return s_showHitboxes != 0;
    }
    void SetShowHitboxes( bool on )
    {
        s_showHitboxes = on ? 1 : 0;
        Radiant_ProfileSetInt( "KiwiUX", "ModelsShowHitboxes", s_showHitboxes );
    }

    // KIWI (2026-10-21, user: "add the ability to force match player physics to one of the
    // other hitboxes (bullet set or physics set)"): rewrite collmaps\<model>.map - the player
    // hulls cod4map stamps on EVERY copy of the model - from the mesh (the importer's automatic
    // hulls: what bullets hit) or from the physics shape (bounds box / upright 16-sided
    // cylinder).  The old file is kept as .map.bak.
    enum matchShape_t { MATCH_BULLET, MATCH_PHYSICS };

    std::string CollmapPath( const std::string &model )
    {
        std::string file = model;
        for ( size_t i = 0; i < file.size(); ++i )
            if ( file[i] == '/' ) file[i] = '\\';
        return ExeDir() + "\\collmaps\\" + file + ".map";
    }

    bool MatchPlayerCollision( const std::string &model, matchShape_t shape, bool cylinder )
    {
        std::vector<kimport::Hull> hulls;
        kimport::CollisionSettings settings;
        const kimport::LogFn log = []( const std::string &s ) { Sys_Printf( "  %s\n", s.c_str() ); };
        std::string note;
        if ( shape == MATCH_BULLET )
        {
            kimport::RawXModel raw;
            std::string err;
            if ( !kimport::ReadRawXModel( std::filesystem::path( ExeDir() ) / "raw", model, raw, err ) )
            {
                Sys_Printf( "Models: cannot read raw\\xmodel\\%s - %s\n", model.c_str(), err.c_str() );
                return false;
            }
            kimport::TrunkSettings trunk;
            kimport::AutoHullsForXModel( raw, kimport::AutoClass::Prop, settings, trunk, hulls, note, log );
            note = "matched to the bullet (mesh) shape";
        }
        else
        {
            float mins[3], maxs[3];
            if ( !KiwiEntThumb_GetModelBounds( model.c_str(), mins, maxs ) )
            {
                Sys_Printf( "Models: '%s' has no bounds to build a physics shape from.\n", model.c_str() );
                return false;
            }
            kimport::Hull h;
            h.kind = cylinder ? "kdop" : "box";
            if ( cylinder )
            {
                const double cx = 0.5 * ( mins[0] + maxs[0] ), cy = 0.5 * ( mins[1] + maxs[1] );
                const double r = 0.5 * ( ( maxs[0] - mins[0] ) > ( maxs[1] - mins[1] ) ? ( maxs[0] - mins[0] ) : ( maxs[1] - mins[1] ) );
                for ( int k = 0; k < 16; ++k )
                {
                    const double a = k * ( 6.283185307179586 / 16 ), nx = cos( a ), ny = sin( a );
                    h.planes.push_back( { nx, ny, 0.0, nx * cx + ny * cy + r } );
                }
            }
            else
            {
                h.planes.push_back( { 1, 0, 0, maxs[0] } );
                h.planes.push_back( { -1, 0, 0, -mins[0] } );
                h.planes.push_back( { 0, 1, 0, maxs[1] } );
                h.planes.push_back( { 0, -1, 0, -mins[1] } );
            }
            h.planes.push_back( { 0, 0, 1, maxs[2] } );
            h.planes.push_back( { 0, 0, -1, -mins[2] } );
            hulls.push_back( h );
            note = cylinder ? "matched to the physics cylinder" : "matched to the physics box";
        }
        if ( hulls.empty() )
        {
            Sys_Printf( "Models: no hull came out for '%s'; the collmap was left alone.\n", model.c_str() );
            return false;
        }
        const std::string path = CollmapPath( model );
        if ( GetFileAttributesA( path.c_str() ) != INVALID_FILE_ATTRIBUTES )
            CopyFileA( path.c_str(), ( path + ".bak" ).c_str(), FALSE );
        std::string err;
        if ( !kimport::WriteCollmap( path, hulls, "clip", err, note ) )
        {
            Sys_Printf( "Models: writing %s failed - %s\n", path.c_str(), err.c_str() );
            return false;
        }
        s_hullCache.erase( model );
        Sys_Printf( "Models: collmaps\\%s.map now holds %d hull%s (%s); previous kept as .map.bak. "
                    "Every copy of the model gets it at the next BSP compile.\n",
                    model.c_str(), (int)hulls.size(), hulls.size() == 1 ? "" : "s", note.c_str() );
        const std::string recipe = ExeDir() + "\\model_source\\" + model + ".kimport";
        if ( GetFileAttributesA( recipe.c_str() ) != INVALID_FILE_ATTRIBUTES )
            Sys_Printf( "Models: '%s' has an import recipe - set its collision mode to manual, or a "
                        "rebuild will write its own hulls over this.\n", model.c_str() );
        g_nUpdateBits = -1;
        return true;
    }

    // KIWI (2026-10-21, user: "a none button for player collision ... that removes all player
    // collision from a model"): the collmap goes to .map.bak, so cod4map stamps no hulls.
    bool RemovePlayerCollision( const std::string &model )
    {
        const std::string path = CollmapPath( model );
        if ( GetFileAttributesA( path.c_str() ) == INVALID_FILE_ATTRIBUTES )
        {
            Sys_Printf( "Models: '%s' already has no player collision.\n", model.c_str() );
            return false;
        }
        if ( !MoveFileExA( path.c_str(), ( path + ".bak" ).c_str(), MOVEFILE_REPLACE_EXISTING ) )
        {
            Sys_Printf( "Models: could not remove %s (error %lu).\n", path.c_str(), GetLastError() );
            return false;
        }
        s_hullCache.erase( model );
        Sys_Printf( "Models: collmaps\\%s.map removed (kept as .map.bak); players and AI walk "
                    "through every copy after the next BSP compile.\n", model.c_str() );
        const std::string recipe = ExeDir() + "\\model_source\\" + model + ".kimport";
        if ( GetFileAttributesA( recipe.c_str() ) != INVALID_FILE_ATTRIBUTES )
            Sys_Printf( "Models: '%s' has an import recipe - set its collision mode to none, or a "
                        "rebuild will write its hulls back.\n", model.c_str() );
        g_nUpdateBits = -1;
        return true;
    }

    // Colours: player hulls green, bullet collision orange, physics shape yellow.
    void DrawSelectedModelInfo( bool inspector )
    {
        std::vector<entity_s_def *> defs;
        const int count = SelectedModelEntities( &defs );
        if ( count == 0 )
            return;

        if ( inspector )
            ImGui::SeparatorText( "Model" );
        else
            ImGui::Separator();
        const entity_s_def *def = defs[0];
        const char *model = KeyOfDef( def, "model" );
        bool sameModel = true;
        for ( size_t i = 1; i < defs.size() && sameModel; ++i )
            sameModel = !_stricmp( KeyOfDef( defs[i], "model" ), model );
        if ( count == 1 )
            ImGui::Text( "Selected: %s  (%s)", model, def->eclass && def->eclass->name ? def->eclass->name : "?" );
        else
            ImGui::Text( "Selected: %i models%s%s", count, sameModel ? ", all " : "", sameModel ? model : "" );

        if ( count == 1 )
        {
            const char *ang = KeyOfDef( def, "angles" );
            const char *sc  = KeyOfDef( def, "modelscale" );
            ImGui::TextDisabled( "origin %.1f %.1f %.1f   angles %s   scale %s",
                                 def->origin[0], def->origin[1], def->origin[2],
                                 ang[0] ? ang : "0 0 0", sc[0] ? sc : "1" );
        }

        if ( sameModel )
        {
            kiwiModelStats_t st;
            if ( KiwiEntThumb_ModelStats( model, &st ) )
            {
                ImGui::TextDisabled( "%d LOD%s, LOD0 %d tris / %d verts, %d tris all LODs, %d bone%s",
                                     st.lods, st.lods == 1 ? "" : "s", st.lod0Tris, st.lod0Verts, st.totalTris,
                                     st.bones, st.bones == 1 ? "" : "s" );
                std::string mats;
                for ( int m = 0; m < st.materialCount && m < 8; ++m )
                    mats += ( m ? ", " : "" ) + std::string( st.materials[m] );
                ImGui::TextDisabled( "materials: %s", mats.empty() ? "-" : mats.c_str() );
                ImGui::TextDisabled( "bullet collision: %s", st.collLod < 0 ? "none (no collision LOD)"
                                     : ( std::to_string( st.collSurfs ) + " surface(s), LOD " + std::to_string( st.collLod ) ).c_str() );
            }
            const hullEdges_t &h = HullEdges( model );
            if ( h.hulls )
                ImGui::TextDisabled( "player collision: %d hull%s (collmaps\\%s.map, stamped at compile)",
                                     h.hulls, h.hulls == 1 ? "" : "s", model );
            else
                ImGui::TextDisabled( "player collision: none - players and AI walk through it" );

            // physics shape of this model: a cylinder preset on a dyn_model, else its box
            bool cylinder = false;
            if ( UsesPhysPreset( def ) )
            {
                const std::string pn = KeyOfDef( def, "physPreset" );
                const physPreset_t *p = PresetNamed( pn.empty() ? "default" : pn );
                cylinder = p && PresetField( p, "tempDefaultToCylinder" ) == "1";
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( "Player collision =" );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "bullet shape" ) )
                MatchPlayerCollision( model, MATCH_BULLET, false );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Rebuild collmaps\\%s.map from the mesh (convex hulls around its pieces).\n"
                                   "Applies to every copy of this model at the next compile.", model );
            ImGui::SameLine();
            if ( ImGui::SmallButton( cylinder ? "physics cylinder" : "physics box" ) )
                MatchPlayerCollision( model, MATCH_PHYSICS, cylinder );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Rebuild collmaps\\%s.map as the physics %s.\n"
                                   "Applies to every copy of this model at the next compile.",
                                   model, cylinder ? "cylinder" : "box" );
            ImGui::SameLine();
            ImGui::BeginDisabled( h.hulls == 0 );
            if ( ImGui::SmallButton( "none" ) )
                RemovePlayerCollision( model );
            ImGui::EndDisabled();
            if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
                ImGui::SetTooltip( "Remove collmaps\\%s.map (kept as .map.bak): no player collision.\n"
                                   "Applies to every copy of this model at the next compile.", model );
        }

        // physPreset for dyn_models (and anything already carrying the key)
        int physCount = 0;
        std::string cur;
        bool mixed = false;
        for ( size_t i = 0; i < defs.size(); ++i )
        {
            if ( !UsesPhysPreset( defs[i] ) )
                continue;
            std::string p = KeyOfDef( defs[i], "physPreset" );
            if ( p.empty() )
                p = "default";
            if ( physCount++ == 0 )
                cur = p;
            else if ( _stricmp( cur.c_str(), p.c_str() ) )
                mixed = true;
        }
        if ( physCount )
        {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( "physPreset" );
            ImGui::SameLine();
            std::string picked;
            if ( PresetCombo( "##selpreset", mixed ? std::string() : cur, &picked ) )
                SetPresetOnSelected( picked );
            if ( !mixed )
            {
                const physPreset_t *p = PresetNamed( cur );
                ImGui::SameLine();
                if ( p )
                    ImGui::TextDisabled( "mass %s  friction %s  bounce %s%s", PresetField( p, "mass" ).c_str(),
                                         PresetField( p, "friction" ).c_str(), PresetField( p, "bounce" ).c_str(),
                                         PresetField( p, "tempDefaultToCylinder" ) == "1" ? "  cylinder" : "" );
                else
                    ImGui::TextDisabled( "(not found - the game falls back to default)" );
            }
            ImGui::TextDisabled( "dyn_model: knocked around by bullets and explosions; players walk through it" );
        }

        if ( !ShowHitboxes() )
            return;
        ImGui::TextDisabled( "hitboxes:" );
        ImGui::SameLine();
        ImGui::TextColored( ImVec4( 0.3f, 1.0f, 0.3f, 1.0f ), "player" );
        ImGui::SameLine();
        ImGui::TextColored( ImVec4( 1.0f, 0.55f, 0.1f, 1.0f ), "bullets" );
        ImGui::SameLine();
        ImGui::TextColored( ImVec4( 1.0f, 0.95f, 0.2f, 1.0f ), "physics" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Drawn on the selected models in the 3D view.\n"
                               "player: the collmaps hulls cod4map stamps as clip (players / AI collide)\n"
                               "bullets: the xmodel's collision triangles (bullet impacts, surface type)\n"
                               "physics: a dyn_model's physics shape (box, or cylinder for cylinder presets)" );
    }

    // World point of a model-space point.
    void ModelToWorld( float axis[3][3], const float *origin, float scale, const float *v, float *out )
    {
        for ( int k = 0; k < 3; ++k )
            out[k] = origin[k] + scale * ( v[0] * axis[0][k] + v[1] * axis[1][k] + v[2] * axis[2][k] );
    }

    // XModelCollTri_s planes back to corners (cod4rad embree_trace.cpp ReconstructTriVerts).
    bool CollTriVerts( const XModelCollTri_s &t, float v[3][3] )
    {
        const float *n = t.plane, *a = t.svec, *b = t.tvec;
        const float det = n[0] * ( a[1] * b[2] - a[2] * b[1] ) - n[1] * ( a[0] * b[2] - a[2] * b[0] )
                        + n[2] * ( a[0] * b[1] - a[1] * b[0] );
        if ( fabsf( det ) < 1e-10f )
            return false;
        const float inv = 1.0f / det;
        const float i00 = ( a[1] * b[2] - a[2] * b[1] ) * inv, i01 = ( n[2] * b[1] - n[1] * b[2] ) * inv, i02 = ( n[1] * a[2] - n[2] * a[1] ) * inv;
        const float i10 = ( a[2] * b[0] - a[0] * b[2] ) * inv, i11 = ( n[0] * b[2] - n[2] * b[0] ) * inv, i12 = ( n[2] * a[0] - n[0] * a[2] ) * inv;
        const float i20 = ( a[0] * b[1] - a[1] * b[0] ) * inv, i21 = ( n[1] * b[0] - n[0] * b[1] ) * inv, i22 = ( n[0] * a[1] - n[1] * a[0] ) * inv;
        v[0][0] = i00 * n[3] + i01 * a[3] + i02 * b[3];
        v[0][1] = i10 * n[3] + i11 * a[3] + i12 * b[3];
        v[0][2] = i20 * n[3] + i21 * a[3] + i22 * b[3];
        v[1][0] = v[0][0] + i01; v[1][1] = v[0][1] + i11; v[1][2] = v[0][2] + i21;
        v[2][0] = v[0][0] + i02; v[2][1] = v[0][1] + i12; v[2][2] = v[0][2] + i22;
        return true;
    }

    void DrawHitboxes()
    {
        if ( !ShowHitboxes() || selected_brushes.next == &selected_brushes )
            return;
        const int budget = 24000;
        KiwiLines_Begin( budget, 2 );
        int models = 0;
        for ( int pass = 0; pass < 3; ++pass )
        {
            if ( pass == 0 ) KiwiLines_Color( 0.3f, 1.0f, 0.3f );     // player hulls
            if ( pass == 1 ) KiwiLines_Color( 1.0f, 0.55f, 0.1f );    // bullet collision
            if ( pass == 2 ) KiwiLines_Color( 1.0f, 0.95f, 0.2f );    // physics shape
            models = 0;
            for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes && models < 64; b = b->next )
            {
                if ( !b->owner || !b->owner->def || !KiwiDrop_IsModelEntity( b ) )
                    continue;
                const entity_s_def *def = (const entity_s_def *)b->owner->def;
                if ( def->eclass && ( def->eclass->classtype & 0x10 ) )
                    continue;                                       // prefabs: no xmodel
                float mins[3], maxs[3], ang[3], scale, org[3];
                XModel *xm = nullptr;
                if ( !KiwiDrop_GetModelInfo( b, mins, maxs, ang, &scale, org, &xm ) )
                    continue;
                ++models;
                float axis[3][3];
                AnglesToAxis( ang, axis );
                if ( pass == 0 )
                {
                    const hullEdges_t &h = HullEdges( KeyOfDef( def, "model" ) );
                    for ( size_t i = 0; i + 5 < h.seg.size(); i += 6 )
                    {
                        float a[3], c[3];
                        ModelToWorld( axis, org, scale, &h.seg[i], a );
                        ModelToWorld( axis, org, scale, &h.seg[i + 3], c );
                        if ( !KiwiLines_Add( a, c ) ) break;
                    }
                }
                else if ( pass == 1 && xm )
                {
                    for ( int s = 0; s < xm->numCollSurfs; ++s )
                    {
                        const XModelCollSurf_s &cs = xm->collSurfs[s];
                        if ( cs.numCollTris * 3 > KiwiLines_Remaining() )
                        {
                            // over budget: the surface's bounds instead of its triangles
                            float c8[KIWI_BOX_CORNERS][3], w8[KIWI_BOX_CORNERS][3];
                            KiwiBox_Corners( cs.mins, cs.maxs, c8 );
                            for ( int k = 0; k < KIWI_BOX_CORNERS; ++k )
                                ModelToWorld( axis, org, scale, c8[k], w8[k] );
                            for ( int e = 0; e < KIWI_BOX_EDGES; ++e )
                                KiwiLines_Add( w8[KIWI_BOX_EDGE[e][0]], w8[KIWI_BOX_EDGE[e][1]] );
                            continue;
                        }
                        for ( int t = 0; t < cs.numCollTris; ++t )
                        {
                            float v[3][3], w[3][3];
                            if ( !CollTriVerts( cs.collTris[t], v ) )
                                continue;
                            for ( int k = 0; k < 3; ++k )
                                ModelToWorld( axis, org, scale, v[k], w[k] );
                            KiwiLines_Add( w[0], w[1] );
                            KiwiLines_Add( w[1], w[2] );
                            KiwiLines_Add( w[2], w[0] );
                        }
                    }
                }
                else if ( pass == 2 && UsesPhysPreset( def ) )
                {
                    std::string pn = KeyOfDef( def, "physPreset" );
                    const physPreset_t *p = PresetNamed( pn.empty() ? "default" : pn );
                    if ( p && PresetField( p, "tempDefaultToCylinder" ) == "1" )
                    {
                        // upright cylinder fitted to the bounds
                        const float cx = 0.5f * ( mins[0] + maxs[0] ), cy = 0.5f * ( mins[1] + maxs[1] );
                        const float r = 0.5f * ( ( maxs[0] - mins[0] ) > ( maxs[1] - mins[1] ) ? ( maxs[0] - mins[0] ) : ( maxs[1] - mins[1] ) );
                        const int segs = 16;
                        for ( int k = 0; k < segs; ++k )
                        {
                            const float a0 = (float)k / segs * 6.2831853f, a1 = (float)( k + 1 ) / segs * 6.2831853f;
                            const float lo0[3] = { cx + r * cosf( a0 ), cy + r * sinf( a0 ), mins[2] };
                            const float lo1[3] = { cx + r * cosf( a1 ), cy + r * sinf( a1 ), mins[2] };
                            const float hi0[3] = { lo0[0], lo0[1], maxs[2] }, hi1[3] = { lo1[0], lo1[1], maxs[2] };
                            float A[3], B[3], C[3], D[3];
                            ModelToWorld( axis, org, scale, lo0, A ); ModelToWorld( axis, org, scale, lo1, B );
                            ModelToWorld( axis, org, scale, hi0, C ); ModelToWorld( axis, org, scale, hi1, D );
                            KiwiLines_Add( A, B ); KiwiLines_Add( C, D );
                            if ( k % 4 == 0 ) KiwiLines_Add( A, C );
                        }
                    }
                    else
                    {
                        float c8[KIWI_BOX_CORNERS][3], w8[KIWI_BOX_CORNERS][3];
                        KiwiBox_Corners( mins, maxs, c8 );
                        for ( int k = 0; k < KIWI_BOX_CORNERS; ++k )
                            ModelToWorld( axis, org, scale, c8[k], w8[k] );
                        for ( int e = 0; e < KIWI_BOX_EDGES; ++e )
                            KiwiLines_Add( w8[KIWI_BOX_EDGE[e][0]], w8[KIWI_BOX_EDGE[e][1]] );
                    }
                }
            }
        }
        KiwiLines_Flush();
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
        ImGui::SameLine();
        {
            bool show = ShowHitboxes();
            if ( ImGui::Checkbox( "Hitboxes", &show ) )
                SetShowHitboxes( show );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Draw the selected models' collision in the 3D view" );
        }
        ImGui::SameLine();
        {
            bool dyn = PlaceDynModel();
            if ( ImGui::Checkbox( "Physics (dyn_model)", &dyn ) )
                SetPlaceDynModel( dyn );
            if ( ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Place models as dyn_model: a physics prop that bullets and explosions\n"
                                   "knock around in game. It does not block players or AI (dynamic\n"
                                   "entities are client-side; player movement never traces them)." );
            if ( dyn )
            {
                ImGui::SameLine();
                std::string picked;
                if ( PresetCombo( "##placepreset", PlacePreset(), &picked ) )
                {
                    s_placePreset = picked;
                    Radiant_ProfileSetString( "KiwiUX", "ModelsPlacePhysPreset", picked.c_str() );
                }
                if ( ImGui::IsItemHovered() )
                    ImGui::SetTooltip( "physPreset for newly placed dyn_models (mass, friction, bounce, sounds)" );
            }
        }

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
            ImGui::SameLine();
            ImGui::BeginDisabled( selectedModels == 0 && s_pickedModel.empty() );
            if ( ImGui::Button( "Randomize variants" ) )
                RandomizeVariants( s_pickedModel );
            ImGui::EndDisabled();
            if ( ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
                ImGui::SetTooltip( "Deal each model's variants (name, name_2, name_3 ...) out evenly in a\n"
                                   "random order: the %s. Undoable.",
                                   selectedModels > 0 ? "selected models"
                                                      : "every placed copy of the picked tile's model" );
            if ( !s_pickedModel.empty() )
            {
                ImGui::SameLine();
                ImGui::TextDisabled( "picked: %s", s_pickedModel.c_str() );
                ImGui::SameLine();
                if ( ImGui::SmallButton( "x##unpick" ) )
                    s_pickedModel.clear();
            }
            DrawVariantUsage();
            DrawSelectedModelInfo( false );
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

void KiwiModelBrowser_DrawSelectedModelInfo( bool inspector )
{
    DrawSelectedModelInfo( inspector );
}

void KiwiModelBrowser_DrawGhost()
{
    DrawHitboxes();                     // KIWI: the selected models' collision, every frame
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

// KIWI (2026-10-01, model importer): re-list raw\xmodel on the next draw (a build just
// added models); the filter and picked prefix stay.
void KiwiModelBrowser_RequestRefresh()
{
    s_enumerated = false;
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

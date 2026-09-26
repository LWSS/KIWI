#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Paint-style misc_model scattering.  One Alt+LMB stroke owns one undo record;
// samples use parallel camera rays so an overhang does not change the painted layer.

#include "stdafx.h"
#include "qe3.h"

#include <imgui/imgui.h>

#include "kiwi_command.h"
#include "kiwi_droptrace.h"
#include "kiwi_extrude.h"
#include "kiwi_fmt.h"
#include "kiwi_grass.h"
#include "kiwi_lines.h"
#include "kiwi_modelbrowser.h"
#include "kiwi_pick.h"
#include "radiant_registry.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern int           Sys_Printf( const char *fmt, ... );
extern int           g_nUpdateBits;
extern entity_s     *world_entity;
extern selbrush_t    selected_brushes;
extern eclass_t     *Eclass_ForName( int has_brushes, const char *name );
extern brush_t      *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls );
extern void          Brush_Create( float *mins, float *maxs, brush_t *b, eclass_t *ecls );
extern void          Brush_BuildWindings( brush_t *def, int bFull );
extern void          Select_Deselect( int deselectFaces );
extern void          CreateEntityFromName( const char *name );
extern void          SetKeyValue( entity_s_def *entity, const char *key, const char *value );
extern void          Ed_EnsureCurrentMaterial_Kiwi();
extern void          Undo_ClearRedo();
extern void          Undo_GeneralStart( const char *operation );
extern void          Undo_End();
// Layer assignment (must live at file scope: an extern declared inside an
// anonymous-namespace function gets internal linkage under MSVC).
extern bool          Layers_Exists( const char *name );                      // qe3.h:1065
extern bool          LayerNew_Apply( const char *name, const char *parent ); // radiant_ui_actions.h
extern void          Layers_AssignSelectionToLayer( const char *layerName ); // layersdlg.cpp

namespace
{
    enum
    {
        KGRASS_SLOT_COUNT    = 6,
        KGRASS_MODEL_CHARS   = 256,
        KGRASS_RING_SEGMENTS = 48
    };

    const char  *KGRASS_PROFILE = "KiwiGrass";
    const float  KGRASS_PI      = 3.14159265358979323846f;

    struct grassSlot_t
    {
        char  model[KGRASS_MODEL_CHARS];
        float weight;
    };

    struct grassPoint_t
    {
        float xyz[3];
    };

    grassSlot_t s_slots[KGRASS_SLOT_COUNT];
    float s_radius   = 128.0f;
    int   s_density  = 6;
    float s_spacing  = 24.0f;
    bool  s_randomYaw = true;
    float s_scaleMin = 0.85f;
    float s_scaleMax = 1.25f;
    float s_maxSlope = 45.0f;
    char  s_layer[64] = "grass";   // map layer every placed model is assigned to ("" = none)

    bool s_loaded    = false;
    bool s_armed     = false;
    bool s_cursorHave = false;
    float s_cursorPoint[3] = { 0.0f, 0.0f, 0.0f };

    bool s_stroke       = false;
    bool s_undoOpen     = false;
    bool s_placeFailed  = false;
    bool s_haveLastStamp = false;
    float s_lastStamp[3] = { 0.0f, 0.0f, 0.0f };
    int   s_placedCount = 0;
    eclass_t *s_miscModel = 0;
    std::vector<grassPoint_t> s_placed;

    bool s_warnedNoModels     = false;
    bool s_warnedMissingClass = false;
    char s_status[160] = "Disarmed.";

    float ClampFloat( float value, float lo, float hi )
    {
        if ( !_finite( value ) )
            return lo;
        if ( value < lo ) return lo;
        if ( value > hi ) return hi;
        return value;
    }

    int ClampInt( int value, int lo, int hi )
    {
        if ( value < lo ) return lo;
        if ( value > hi ) return hi;
        return value;
    }

    void SetStatus( const char *fmt, ... )
    {
        va_list args;
        va_start( args, fmt );
        _vsnprintf( s_status, sizeof( s_status ), fmt, args );
        va_end( args );
        s_status[sizeof( s_status ) - 1] = '\0';
    }

    void CopyString( char *dst, int dstSize, const char *src )
    {
        if ( !dst || dstSize <= 0 )
            return;
        strncpy( dst, src ? src : "", (size_t)dstSize - 1 );
        dst[dstSize - 1] = '\0';
    }

    float ReadFloat( const char *entry, float defValue )
    {
        const std::string text = Radiant_ProfileGetString( KGRASS_PROFILE, entry, "" );
        if ( text.empty() )
            return defValue;
        char *end = 0;
        const double value = strtod( text.c_str(), &end );
        if ( end == text.c_str() || !_finite( value ) )
            return defValue;
        return (float)value;
    }

    void WriteFloat( const char *entry, float value )
    {
        char text[64];
        KiwiFmt_Num( text, sizeof( text ), value, 6 );
        Radiant_ProfileSetString( KGRASS_PROFILE, entry, text );
    }

    void SanitizeSettings()
    {
        s_radius   = ClampFloat( s_radius,   8.0f, 1024.0f );
        s_density  = ClampInt  ( s_density,  1,    64 );
        s_spacing  = ClampFloat( s_spacing,  0.0f, 1024.0f );
        s_scaleMin = ClampFloat( s_scaleMin, 0.10f, 4.0f );
        s_scaleMax = ClampFloat( s_scaleMax, 0.10f, 4.0f );
        if ( s_scaleMin > s_scaleMax )
        {
            const float swap = s_scaleMin;
            s_scaleMin = s_scaleMax;
            s_scaleMax = swap;
        }
        s_maxSlope = ClampFloat( s_maxSlope, 0.0f, 90.0f );
        for ( int i = 0; i < KGRASS_SLOT_COUNT; ++i )
        {
            if ( !_finite( s_slots[i].weight ) || s_slots[i].weight < 0.0f )
                s_slots[i].weight = 0.0f;
            else if ( s_slots[i].weight > 1000.0f )
                s_slots[i].weight = 1000.0f;
        }
    }

    void LoadSettings()
    {
        if ( s_loaded )
            return;
        s_loaded = true;

        for ( int i = 0; i < KGRASS_SLOT_COUNT; ++i )
        {
            char entry[32];
            _snprintf( entry, sizeof( entry ), "Model%i", i + 1 );
            entry[sizeof( entry ) - 1] = '\0';
            const std::string model = Radiant_ProfileGetString( KGRASS_PROFILE, entry, "" );
            CopyString( s_slots[i].model, sizeof( s_slots[i].model ), model.c_str() );

            _snprintf( entry, sizeof( entry ), "Weight%i", i + 1 );
            entry[sizeof( entry ) - 1] = '\0';
            s_slots[i].weight = ReadFloat( entry, 1.0f );
        }

        s_radius    = ReadFloat( "Radius", 128.0f );
        s_density   = Radiant_ProfileGetInt( KGRASS_PROFILE, "Density", 6 );
        s_spacing   = ReadFloat( "Spacing", 24.0f );
        s_randomYaw = Radiant_ProfileGetInt( KGRASS_PROFILE, "RandomYaw", 1 ) != 0;
        s_scaleMin  = ReadFloat( "ScaleMin", 0.85f );
        s_scaleMax  = ReadFloat( "ScaleMax", 1.25f );
        s_maxSlope  = ReadFloat( "MaxSlope", 45.0f );
        const std::string layer = Radiant_ProfileGetString( KGRASS_PROFILE, "Layer", "grass" );
        CopyString( s_layer, sizeof( s_layer ), layer.c_str() );
        SanitizeSettings();
    }

    void SaveSettings()
    {
        SanitizeSettings();
        for ( int i = 0; i < KGRASS_SLOT_COUNT; ++i )
        {
            char entry[32];
            _snprintf( entry, sizeof( entry ), "Model%i", i + 1 );
            entry[sizeof( entry ) - 1] = '\0';
            Radiant_ProfileSetString( KGRASS_PROFILE, entry, s_slots[i].model );

            _snprintf( entry, sizeof( entry ), "Weight%i", i + 1 );
            entry[sizeof( entry ) - 1] = '\0';
            WriteFloat( entry, s_slots[i].weight );
        }
        WriteFloat( "Radius", s_radius );
        Radiant_ProfileSetInt( KGRASS_PROFILE, "Density", s_density );
        WriteFloat( "Spacing", s_spacing );
        Radiant_ProfileSetInt( KGRASS_PROFILE, "RandomYaw", s_randomYaw ? 1 : 0 );
        WriteFloat( "ScaleMin", s_scaleMin );
        WriteFloat( "ScaleMax", s_scaleMax );
        WriteFloat( "MaxSlope", s_maxSlope );
        Radiant_ProfileSetString( KGRASS_PROFILE, "Layer", s_layer );
    }

    bool AnyNonEmptyModel()
    {
        for ( int i = 0; i < KGRASS_SLOT_COUNT; ++i )
            if ( s_slots[i].model[0] )
                return true;
        return false;
    }

    float UsableWeight()
    {
        float total = 0.0f;
        for ( int i = 0; i < KGRASS_SLOT_COUNT; ++i )
            if ( s_slots[i].model[0] && s_slots[i].weight > 0.0f )
                total += s_slots[i].weight;
        return total;
    }

    float UnitRandom()
    {
        return (float)rand() / ( (float)RAND_MAX + 1.0f );
    }

    const char *ChooseModel( float totalWeight )
    {
        float pick = UnitRandom() * totalWeight;
        const char *last = 0;
        for ( int i = 0; i < KGRASS_SLOT_COUNT; ++i )
        {
            if ( !s_slots[i].model[0] || !( s_slots[i].weight > 0.0f ) )
                continue;
            last = s_slots[i].model;
            if ( pick < s_slots[i].weight )
                return s_slots[i].model;
            pick -= s_slots[i].weight;
        }
        return last;
    }

    void ClearCursor()
    {
        if ( s_cursorHave )
        {
            s_cursorHave = false;
            g_nUpdateBits |= W_CAMERA;
        }
    }

    bool TraceCursor( int imgX, int imgY, ray_t *outRay, kiwiDropHit_t *outHit )
    {
        ray_t ray;
        kiwiDropHit_t hit;
        if ( !Pick_RayFromImagePos( imgX, imgY, &ray )
          || !KiwiDrop_Trace( ray, false, &hit ) )
            return false;
        if ( outRay ) *outRay = ray;
        if ( outHit ) *outHit = hit;
        return true;
    }

    bool UpdateCursor( int imgX, int imgY, ray_t *outRay, kiwiDropHit_t *outHit )
    {
        ray_t ray;
        kiwiDropHit_t hit;
        if ( !TraceCursor( imgX, imgY, &ray, &hit ) )
        {
            ClearCursor();
            return false;
        }

        const bool changed = !s_cursorHave
                          || fabsf( s_cursorPoint[0] - hit.point[0] ) > 0.01f
                          || fabsf( s_cursorPoint[1] - hit.point[1] ) > 0.01f
                          || fabsf( s_cursorPoint[2] - hit.point[2] ) > 0.01f;
        s_cursorHave = true;
        for ( int i = 0; i < 3; ++i )
            s_cursorPoint[i] = hit.point[i];
        if ( changed )
            g_nUpdateBits |= W_CAMERA;
        if ( outRay ) *outRay = ray;
        if ( outHit ) *outHit = hit;
        return true;
    }

    bool TooCloseToStroke( const float point[3] )
    {
        if ( !( s_spacing > 0.0f ) )
            return false;
        const float minDist2 = s_spacing * s_spacing;
        for ( size_t i = 0; i < s_placed.size(); ++i )
        {
            const float dx = point[0] - s_placed[i].xyz[0];
            const float dy = point[1] - s_placed[i].xyz[1];
            if ( dx * dx + dy * dy < minDist2 )
                return true;
        }
        return false;
    }

    bool DropPlaceholder( const float origin[3] )
    {
        float mins[3], maxs[3];
        for ( int i = 0; i < 3; ++i )
        {
            mins[i] = origin[i] + s_miscModel->mins[i];
            maxs[i] = origin[i] + s_miscModel->maxs[i];
            if ( maxs[i] - mins[i] < 1.0f )
                maxs[i] = mins[i] + 1.0f;
        }

        Ed_EnsureCurrentMaterial_Kiwi();
        brush_t *def = Brush_Alloc( g_qeglobals.random_texture_stuff, nullptr );
        if ( !def )
        {
            Sys_Printf( "Grass Scatter: brush allocation failed.\n" );
            return false;
        }
        Brush_Create( mins, maxs, def, nullptr );
        Brush_BuildWindings( def, 1 );
        KiwiExtrude_LandDef( def );
        return true;
    }

    void BeginUndo()
    {
        if ( s_undoOpen )
            return;
        Undo_ClearRedo();
        Undo_GeneralStart( "scatter grass" );
        s_undoOpen = true;
    }

    bool PlaceOne( const float origin[3], const char *modelName, float yaw, float scale )
    {
        Select_Deselect( 1 );
        BeginUndo();
        if ( !DropPlaceholder( origin ) )
            return false;
        CreateEntityFromName( "misc_model" );

        entity_s_def *created = 0;
        selbrush_t *selected = selected_brushes.next;
        if ( selected && selected != &selected_brushes
          && selected->next == &selected_brushes && selected->owner
          && selected->owner != world_entity )
        {
            entity_s_def *candidate = (entity_s_def *)selected->owner->def;
            if ( candidate && candidate->eclass == s_miscModel )
                created = candidate;
        }
        if ( !created )
        {
            Sys_Printf( "Grass Scatter: misc_model creation failed; no keys were written.\n" );
            return false;
        }

        SetKeyValue( created, "model", modelName );
        if ( s_randomYaw )
        {
            char yawText[32], angles[96];
            KiwiFmt_Num( yawText, sizeof( yawText ), yaw, 3 );
            _snprintf( angles, sizeof( angles ), "0 %s 0", yawText );
            angles[sizeof( angles ) - 1] = '\0';
            SetKeyValue( created, "angles", angles );
        }
        if ( fabsf( scale - 1.0f ) > 0.01f )
        {
            char scaleText[32];
            KiwiFmt_Num( scaleText, sizeof( scaleText ), scale, 4 );
            SetKeyValue( created, "modelscale", scaleText );
        }
        // Assign the freshly created (still selected) model to the grass layer, so
        // every scatter lands in one Layers-panel group ("grass" by default) that can
        // be hidden / selected / frozen together and round-trips in the .map.
        if ( s_layer[0] )
        {
            if ( !Layers_Exists( s_layer ) )
                LayerNew_Apply( s_layer, "" );
            if ( Layers_Exists( s_layer ) )
                Layers_AssignSelectionToLayer( s_layer );
        }
        return true;
    }

    void ScatterStamp( const ray_t &centerRay )
    {
        const float totalWeight = UsableWeight();
        if ( !( totalWeight > 0.0f ) )
            return;

        const float slopeZ = ( s_maxSlope >= 90.0f )
                           ? 0.0f
                           : cosf( s_maxSlope * KGRASS_PI / 180.0f );
        int placedThisStamp = 0;
        for ( int i = 0; i < s_density && !s_placeFailed; ++i )
        {
            const float r = s_radius * sqrtf( UnitRandom() );
            const float a = 2.0f * KGRASS_PI * UnitRandom();
            ray_t ray = centerRay;
            ray.origin[0] += r * cosf( a );
            ray.origin[1] += r * sinf( a );

            kiwiDropHit_t hit;
            if ( !KiwiDrop_Trace( ray, false, &hit ) )
                continue;
            if ( hit.normal[2] < slopeZ || TooCloseToStroke( hit.point ) )
                continue;

            const char *modelName = ChooseModel( totalWeight );
            if ( !modelName )
                continue;
            const float yaw = s_randomYaw ? UnitRandom() * 360.0f : 0.0f;
            const float scale = s_scaleMin + UnitRandom() * ( s_scaleMax - s_scaleMin );
            if ( !PlaceOne( hit.point, modelName, yaw, scale ) )
            {
                s_placeFailed = true;
                break;
            }

            grassPoint_t point;
            for ( int axis = 0; axis < 3; ++axis )
                point.xyz[axis] = hit.point[axis];
            s_placed.push_back( point );
            ++s_placedCount;
            ++placedThisStamp;
        }

        if ( placedThisStamp )
            g_nUpdateBits = -1;
    }

    void MaybeStamp( int imgX, int imgY, bool force )
    {
        if ( !s_stroke || s_placeFailed )
            return;

        ray_t ray;
        kiwiDropHit_t center;
        if ( !UpdateCursor( imgX, imgY, &ray, &center ) )
            return;

        const float stampStep = s_radius * 0.5f;
        if ( !force && s_haveLastStamp )
        {
            const float dx = center.point[0] - s_lastStamp[0];
            const float dy = center.point[1] - s_lastStamp[1];
            if ( dx * dx + dy * dy < stampStep * stampStep )
                return;
        }

        for ( int i = 0; i < 3; ++i )
            s_lastStamp[i] = center.point[i];
        s_haveLastStamp = true;
        ScatterStamp( ray );
    }

    void EndStroke()
    {
        if ( !s_stroke )
            return;

        if ( s_undoOpen )
            Undo_End();
        s_undoOpen = false;

        Select_Deselect( 1 );
        g_nUpdateBits = -1;

        if ( s_placedCount )
        {
            Sys_Printf( "Grass Scatter: placed %i misc_model%s in one stroke.\n",
                        s_placedCount, s_placedCount == 1 ? "" : "s" );
            SetStatus( "Armed. Last stroke placed %i model%s.",
                       s_placedCount, s_placedCount == 1 ? "" : "s" );
        }
        else if ( s_placeFailed )
        {
            SetStatus( "Armed. Placement failed; see the console." );
        }
        else
        {
            SetStatus( "Armed. No points passed the trace, slope, and spacing filters." );
        }

        s_stroke        = false;
        s_placeFailed   = false;
        s_haveLastStamp = false;
        s_placedCount   = 0;
        s_miscModel     = 0;
        s_placed.clear();
    }

    void SetArmed( bool armed )
    {
        if ( s_armed == armed )
            return;
        if ( !armed )
            EndStroke();
        s_armed = armed;
        ClearCursor();
        s_warnedNoModels = false;
        SetStatus( armed ? "Armed." : "Disarmed." );   // KIWI: keys on the camera hint strip
        g_nUpdateBits |= W_CAMERA;
    }

    bool AcceptModelDrop( char *dst, int dstSize )
    {
        if ( !ImGui::BeginDragDropTarget() )
            return false;
        bool changed = false;
        const ImGuiPayload *payload = ImGui::AcceptDragDropPayload( KMODEL_PAYLOAD );
        if ( payload && payload->IsDelivery() && payload->Data && payload->DataSize > 0 )
        {
            const char *data = (const char *)payload->Data;
            int length = 0;
            while ( length < payload->DataSize && data[length] )
                ++length;
            if ( length > 0 )
            {
                if ( length >= dstSize )
                    length = dstSize - 1;
                memcpy( dst, data, (size_t)length );
                dst[length] = '\0';
                changed = true;
            }
        }
        ImGui::EndDragDropTarget();
        return changed;
    }
}

// Grass Scatter is a MODE of the Terrain Sculpt panel (kiwi_terrain.cpp); every route
// that used to open its own window now opens that panel with the Grass tool selected.
extern void KiwiTerrain_OpenWithTool( int tool );   // kiwi_terrain.cpp
enum { KGRASS_TERRAIN_TOOL = 6 };                    // KTER_GRASS in kiwi_terrain.cpp

void KiwiGrass_MenuItem()
{
    LoadSettings();
    if ( ImGui::MenuItem( "Grass Scatter (Terrain Sculpt mode)" ) )
        KiwiTerrain_OpenWithTool( KGRASS_TERRAIN_TOOL );
}

// Windows-menu / palette route (KIWI_CMD_GRASS_PANEL, kiwi_windows.cpp).
void KiwiGrass_TogglePanel()
{
    LoadSettings();
    KiwiTerrain_OpenWithTool( KGRASS_TERRAIN_TOOL );
}

bool KiwiGrass_PanelVisible()
{
    extern bool KiwiTerrain_PanelVisible();
    return KiwiTerrain_PanelVisible();
}

// The terrain panel arms/disarms the scatter when its Grass mode is armed.
void KiwiGrass_SetArmed( bool armed )
{
    LoadSettings();
    SetArmed( armed );
}

void KiwiGrass_Draw()
{
    LoadSettings();
    // The settings now live inside the Terrain Sculpt panel (KiwiGrass_DrawSettings);
    // this per-frame call only keeps the arm state honest when the panel is gone.
    extern bool KiwiTerrain_PanelVisible();
    if ( s_armed && !KiwiTerrain_PanelVisible() )
        SetArmed( false );
}

// The palette + scatter parameters, drawn inside the Terrain Sculpt panel.
void KiwiGrass_DrawSettings()
{
    LoadSettings();
    {
        bool changed = false;
        ImGui::TextDisabled( "Drag model tiles from the Models browser into a slot." );
        ImGui::SeparatorText( "Model palette" );
        ImGui::TextDisabled( "Model name" );
        ImGui::SameLine( 294.0f );
        ImGui::TextDisabled( "Weight" );
        for ( int i = 0; i < KGRASS_SLOT_COUNT; ++i )
        {
            ImGui::PushID( i );
            ImGui::Text( "%i", i + 1 );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 260.0f );
            changed |= ImGui::InputText( "##model", s_slots[i].model,
                                         sizeof( s_slots[i].model ) );
            changed |= AcceptModelDrop( s_slots[i].model, sizeof( s_slots[i].model ) );
            ImGui::SameLine();
            ImGui::SetNextItemWidth( 72.0f );
            changed |= ImGui::InputFloat( "##weight", &s_slots[i].weight,
                                          0.1f, 1.0f, "%.2f" );
            ImGui::PopID();
        }

        ImGui::SeparatorText( "Scatter" );
        changed |= ImGui::SliderFloat( "Radius", &s_radius, 8.0f, 1024.0f, "%.0f" );
        changed |= ImGui::SliderInt( "Density", &s_density, 1, 64 );
        changed |= ImGui::SliderFloat( "Spacing", &s_spacing, 0.0f, 1024.0f, "%.0f" );
        changed |= ImGui::Checkbox( "Random yaw", &s_randomYaw );
        changed |= ImGui::DragFloatRange2( "Scale jitter", &s_scaleMin, &s_scaleMax,
                                           0.01f, 0.10f, 4.0f, "%.2f", "%.2f" );
        changed |= ImGui::SliderFloat( "Max slope", &s_maxSlope, 0.0f, 90.0f, "%.0f deg" );
        ImGui::SetNextItemWidth( 160.0f );
        changed |= ImGui::InputText( "Layer", s_layer, sizeof( s_layer ) );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Every placed model is assigned to this map layer\n"
                               "(Layers panel: hide / select / freeze the whole group).\n"
                               "Created on first use; empty = no layer assignment." );

        if ( changed )
        {
            SanitizeSettings();
            SaveSettings();
            s_warnedNoModels = false;
            g_nUpdateBits |= W_CAMERA;
        }
        if ( s_armed )
            ImGui::TextColored( ImVec4( 0.42f, 0.92f, 0.48f, 1.0f ), "%s", s_status );
        else
            ImGui::TextDisabled( "%s", s_status );
    }
}

bool KiwiGrass_IsArmed()
{
    return s_armed;
}

bool KiwiGrass_HandleDown( int imgX, int imgY )
{
    LoadSettings();
    if ( !s_armed )
        return false;

    if ( !AnyNonEmptyModel() )
    {
        if ( !s_warnedNoModels )
        {
            s_warnedNoModels = true;
            Sys_Printf( "Grass Scatter: add or drop at least one model into the palette.\n" );
        }
        SetStatus( "Armed, but the model palette is empty." );
        return false;
    }
    if ( !( UsableWeight() > 0.0f ) )
    {
        if ( !s_warnedNoModels )
        {
            s_warnedNoModels = true;
            Sys_Printf( "Grass Scatter: non-empty model slots need a positive weight.\n" );
        }
        SetStatus( "Armed, but no model slot has a positive weight." );
        return false;
    }

    s_miscModel = Eclass_ForName( 0, "misc_model" );
    if ( !s_miscModel || *(int *)&s_miscModel->fixedsize == 0 )
    {
        if ( !s_warnedMissingClass )
        {
            s_warnedMissingClass = true;
            Sys_Printf( "Grass Scatter: misc_model is not available.\n" );
        }
        SetStatus( "Armed, but misc_model is unavailable (check the loaded .def)." );
        s_miscModel = 0;
        return false;
    }

    if ( KiwiEditorCommand *live = KiwiCmd_Active() )
    {
        if ( live->PreemptIdle() )
            KiwiCmd_Cancel();
        else
        {
            Sys_Printf( "Grass Scatter: finish or cancel \"%s\" before painting.\n",
                        live->Name() );
            SetStatus( "Armed. Finish or cancel the active command before painting." );
            s_miscModel = 0;
            return false;
        }
    }

    ray_t ray;
    kiwiDropHit_t hit;
    if ( !UpdateCursor( imgX, imgY, &ray, &hit ) )
    {
        SetStatus( "Armed. No surface under the cursor." );
        s_miscModel = 0;
        return false;
    }

    s_stroke        = true;
    s_undoOpen      = false;
    s_placeFailed   = false;
    s_haveLastStamp = false;
    s_placedCount   = 0;
    s_placed.clear();
    s_placed.reserve( (size_t)s_density * 4 );
    SetStatus( "Painting grass..." );
    MaybeStamp( imgX, imgY, true );
    return true;
}

void KiwiGrass_HandleDrag( int imgX, int imgY )
{
    if ( s_stroke )
        MaybeStamp( imgX, imgY, false );
}

void KiwiGrass_HandleUp()
{
    EndStroke();
}

void KiwiGrass_HandleAbort()
{
    EndStroke();
}

bool KiwiGrass_HandleEscape()
{
    if ( !s_armed )
        return false;
    SetArmed( false );
    return true;
}

void KiwiGrass_Hover( int imgX, int imgY, bool over )
{
    if ( !s_armed || !over )
    {
        ClearCursor();
        return;
    }
    UpdateCursor( imgX, imgY, 0, 0 );
}

void KiwiGrass_DrawWorld()
{
    if ( !s_armed || !s_cursorHave )
        return;

    KiwiLines_Begin( KGRASS_RING_SEGMENTS, 2 );
    KiwiLines_Color( 0.30f, 0.95f, 0.42f );
    float first[3], prev[3];
    for ( int i = 0; i < KGRASS_RING_SEGMENTS; ++i )
    {
        const float a = 2.0f * KGRASS_PI * (float)i / (float)KGRASS_RING_SEGMENTS;
        float point[3] = { s_cursorPoint[0] + s_radius * cosf( a ),
                           s_cursorPoint[1] + s_radius * sinf( a ),
                           s_cursorPoint[2] + 1.0f };
        if ( i == 0 )
        {
            for ( int axis = 0; axis < 3; ++axis )
                first[axis] = prev[axis] = point[axis];
            continue;
        }
        if ( !KiwiLines_Add( prev, point ) )
            break;
        for ( int axis = 0; axis < 3; ++axis )
            prev[axis] = point[axis];
    }
    if ( KiwiLines_Remaining() > 0 )
        KiwiLines_Add( prev, first );
    KiwiLines_Flush();
}

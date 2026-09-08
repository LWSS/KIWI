#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include "qe3.h"
#include "prefs.h"
#include "xywnd.h"
#include "kiwi_light.h"
#include "kiwi_lightcache.h"
#include "kiwi_walkcache.h"
#include "kiwi_lines.h"
#include "kiwi_windows.h"
#include "kiwi_command.h"
#include "kiwi_boxselect.h"
#include "kiwi_extrude.h"

#include <gfx_d3d/r_init.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_rendercmds.h>
#include <universal/com_files.h>
#include <imgui/imgui.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <math.h>
#include <stdio.h>
#include <string.h>

extern selbrush_t selected_brushes;
extern entity_s   entities;
extern float      world_orient_matrix[4][3];
extern int        g_nUpdateBits;
extern entity_s_def *edit_entity;

extern char  *ValueForKey2( const entity_s *e, const char *key );
extern float  Entity_GetFloatValueForKey( const entity_s *e, const char *key );
extern int    Entity_GetIntValueForKey( const entity_s *e, const char *key );
extern int    Entity_GetVec3ForKey( entity_s_def *e, float *out, const char *key );
extern bool   HasKeyValuePair( entity_s_def *e, const char *key );
extern void   SetKeyValue( entity_s_def *e, const char *key, const char *value );
extern void   SpawnFlags_Apply( int flags );
extern void   MarkMapModified();
extern void   Undo_ClearRedo();
extern void   Undo_GeneralStart( const char *operation );
extern void   Undo_AddEntity_W( entity_s *e );
extern void   Undo_End();
extern void   Select_Deselect( int deselectFaces );
extern void   Select_Brush( selbrush_t *brush, char overwrite, char status, char center );
extern void   CreateEntityFromName( const char *classname );
extern eclass_t *Eclass_ForName( int hasBrushes, const char *name );
extern brush_t  *Brush_Alloc( const void *material, eclass_t *eclass );
extern void      Brush_Create( float *mins, float *maxs, brush_t *brush, eclass_t *eclass );
extern void      Brush_BuildWindings( brush_t *brush, int full );
extern void      Ed_EnsureCurrentMaterial_Kiwi();
extern int       Sys_Printf( const char *fmt, ... );
extern bool      Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );
extern void      Radiant_ExecCommand( unsigned int commandId );
extern void      CamWnd_AddLightPreview( selbrush_t *inst, selbrush_t *arg2,
                                         const orientation_t *orient );
extern void      ImGuiShell_FocusTab( const char *title );
extern ImGuiID   ImGuiShell_DockRoot();
extern bool KiwiSunPreview_Enabled();
extern void KiwiSunPreview_SetEnabled(bool);
extern const char *KiwiSunPreview_Status();

namespace
{
    const int   KLIGHT_MAX_OVERLAYS = 8;
    const int   KLIGHT_RING_SEGS    = 24;
    const float KLIGHT_PI           = 3.14159265358979323846f;
    const float KLIGHT_XY_DEPTH     = 131072.0f;

    const char *const KTIP_COLOR =
        "Sets RGB hue; the largest channel is scaled to 1, so 0.1 0.1 0.1 equals 1 1 1.\n"
        "The normalized color is multiplied by intensity for both the bake and runtime primary.\n"
        "Use intensity, rather than scaling every RGB channel, to change brightness.";
    const char *const KTIP_INTENSITY =
        "Brightness where the falloff curve is white: 1 is fullbright; smaller values are dimmer.\n"
        "The compiler multiplies it by max-normalized _color.\n"
        "Gotcha: cod4rad treats a non-positive value as default 1, while a runtime primary can be dark.";
    const char *const KTIP_RADIUS =
        "Hard cutoff distance in map units. The falloff image runs origin-to-radius and ends black.\n"
        "The bake samples distance x image width / radius, so radius stretches the same curve.\n"
        "Keep it positive: a missing or invalid radius makes the compiler ignore the light.";
    const char *const KTIP_FOV_OUTER =
        "Spotlight's total cone angle in degrees; 90 means 45 degrees to either side.\n"
        "A value of 0 derives a cone through a 64-unit-radius circle at the target.\n"
        "Keep it above fov_inner. A primary at 180 is ignored; above 120 requires NOSHADOWMAP.";
    const char *const KTIP_FOV_INNER =
        "Full-bright inner cone in degrees. The spotlight fades only between inner and outer FOV.\n"
        "Keep it smaller than fov_outer; the default is 0. It has no angular effect on an omni.";
    const char *const KTIP_EXPONENT =
        "Shapes a spotlight's angular fade between fov_inner and fov_outer.\n"
        "0 means no fade, 1 is linear, and larger integers make the edge fall off more steeply.\n"
        "Only spots use it; cod4map forces an omni's exponent to 0.";
    const char *const KTIP_DEF =
        "Selects raw/lights/<name>: a GfxLightDef falloff image plus lmapLookupStart slot.\n"
        "Default light_point_linear and light_dynamic use falloff_linear; light_point_quadratic uses falloff_quadratic.\n"
        "light_point_linear_nocenter uses falloff_lin_nocenter; light_point_dark_edges uses falloff_center.\n"
        "light_no_falloff uses falloff_none; candle/florescent use falloff_candle/falloff_florescent.\n"
        "red_light/tungsten_lamp use falloff_redlight/falloff_tungsten. Spots share these radial curves.\n"
        "The combo lists disk defs; FOV/exponent shape spots. Custom primary names: existing file, 63 chars max.";
    const char *const KTIP_PRIMARY_HEADER =
        "Primary lights are real-time game lights that can light moving objects.\n"
        "Use PRIMARY for the few lights that must cast dynamic shadows or light moving objects.\n"
        "Leave it off for fill lights: baked lights are cheaper, static, and contribute bounce.";
    const char *const KTIP_PRIMARY_OMNI =
        "Real-time omni: lights the world and moving objects without cone attenuation.\n"
        "Use it only for the few gameplay lights that need runtime lighting; bake fill lights.\n"
        "A resolvable target is still required for its stored spot/shadow direction.\n"
        "Only one primary may affect a world surface; overlaps are compile errors.\n"
        "Only spots get shadowmaps; if OMNI and SPOT are both set, SPOT wins.";
    const char *const KTIP_PRIMARY_SPOT =
        "Real-time cone light: inner/outer FOV and exponent control angular falloff.\n"
        "Use it for the few lights that must light moving objects or cast dynamic spot shadows.\n"
        "A resolvable target is required; only one primary may affect each world surface.\n"
        "Spot shadowmaps compete for four slots by default; fov_outer above 120 requires NOSHADOWMAP.\n"
        "If OMNI and SPOT are both set, SPOT wins.";
    const char *const KTIP_PRIMARY_SCRIPTABLE =
        "Makes an OMNI/SPOT primary survive as a script light entity.\n"
        "Scripts can change color, intensity, radius, FOV and exponent, then move or turn it.\n"
        "Use it for animated or moving lights; SCRIPTABLE alone does nothing without OMNI/SPOT.\n"
        "maxmove and maxturn bound movement from the compiled origin and direction.";
    const char *const KTIP_PRIMARY_NOSHADOWMAP =
        "Prevents this primary from ever receiving a shadowmap.\n"
        "Use it when dynamic shadows are unnecessary; it preserves the four-light default budget.\n"
        "It is required when fov_outer is above 120; without it cod4map drops the primary.\n"
        "The light still renders in real time and is still baked into lightmaps.";
    const char *const KTIP_MAXTURN =
        "Maximum degrees script may turn this light away from its compiled direction.\n"
        "cod4map clamps it from 0 to 180, and the game clamps motion to that cone; 0 locks turning.";
    const char *const KTIP_MAXMOVE =
        "Maximum map units script may move this light from its compiled origin.\n"
        "The game clamps motion to that radius; 0 locks movement. It applies only to SCRIPTABLE primaries.";
    const char *const KTIP_TARGET =
        "Chooses an entity, usually info_null, that the light points toward; for OMNI it supplies direction only.\n"
        "Every PRIMARY needs a resolvable target or cod4map drops it.\n"
        "With no target key, cod4rad still bakes a point light; a bad target makes the spotlight bake fail.\n"
        "fov_outer 0 derives a cone through a 64-unit-radius circle at the target.";
    const char *const KTIP_CREATE_TARGET =
        "Creates an info_null 64 units along the current direction and targets every selected light.\n"
        "With no existing direction it places the target straight down.";
    const char *const KTIP_ENABLE_PREVIEW =
        "Turns selected and pinned light rendering in the 3D camera on or off.";
    const char *const KTIP_MAX_INTENSITY =
        "On forces preview brightness to 10,000,000; off uses the light's real intensity key.";
    const char *const KTIP_PRIMARY_ONLY =
        "Hides baked-only fill lights so the preview shows only lights that exist in the game at runtime.";
    const char *const KTIP_SUN_PREVIEW =
        "Turns the directional sun preview on or off; the status line reports whether it can run.";
    const char *const KTIP_PIN =
        "Keeps this light in the 3D preview after it is deselected.";
    const char *const KTIP_EXTENTS =
        "Shows or hides radius spheres, spot cones, and target axes for the selected lights.";

    bool s_previewPrimaryOnly = false;
    unsigned s_gameEligibilityEpoch = ~0u;
    std::map<selbrush_t *, bool> s_gameEligibility;
    std::map<entity_s_def *, bool> s_showExtents;

    entity_s_def *s_observedLight = nullptr;
    entity_s_def *s_pendingLightFocus = nullptr;
    entity_s_def *s_uiEntity      = nullptr;

    enum FloatField
    {
        KF_INTENSITY,
        KF_RADIUS,
        KF_FOV_OUTER,
        KF_FOV_INNER,
        KF_MAXTURN,
        KF_MAXMOVE,
        KF_COUNT
    };
    struct FloatEdit
    {
        float value;
        bool  active;
    };
    FloatEdit s_floatEdit[KF_COUNT] = {};
    int   s_exponentEdit = 0;
    bool  s_exponentActive = false;
    float s_colorEdit[3] = { 1.0f, 1.0f, 1.0f };
    bool  s_colorActive = false;
    char  s_defEdit[128] = {};
    bool  s_defActive = false;

    struct LightInfo
    {
        entity_s_def *def;
        selbrush_t   *brush;
        float origin[3];
        float rawColor[3];
        float effectiveColor[3];
        float intensity;
        float radius;
        int   flags;
        bool  hasTargetKey;
        bool  hasTargetName;
        entity_s_def *target;
        float targetPos[3];
        float travelDir[3];
        float targetDistance;
        float fovOuter;
        float fovInner;
        float cosOuter;
        float cosInner;
        size_t defNameLength;
        bool  previewSpot;
    };

    enum GameLightReject
    {
        KLR_NONE,
        KLR_NOT_PRIMARY,
        KLR_NO_TARGET_NAME,
        KLR_UNRESOLVED_TARGET,
        KLR_DEF_NAME_TOO_LONG,
        KLR_NONPOSITIVE_RADIUS,
        KLR_OUTER_FOV_TOO_WIDE,
        KLR_SPOT_CONE_INVERTED,
        KLR_WIDE_CONE_NEEDS_NOSHADOW
    };

    float Clamp( float v, float lo, float hi )
    {
        if ( !( v >= lo ) ) return lo;
        if ( v > hi ) return hi;
        return v;
    }

    // Fixed-point labels retain map-key precision without scientific notation or trailing zeroes.
    void FormatFloat( char *out, size_t outSize, float value, int decimals = 3 )
    {
        if ( !out || !outSize )
            return;
        _snprintf( out, outSize, "%.*f", decimals, value );
        out[outSize - 1] = '\0';
        char *dot = strchr( out, '.' );
        if ( dot )
        {
            char *end = out + strlen( out );
            while ( end > dot + 1 && end[-1] == '0' ) --end;
            if ( end > dot && end[-1] == '.' ) --end;
            *end = '\0';
        }
        if ( !strcmp( out, "-0" ) )
        {
            out[0] = '0';
            out[1] = '\0';
        }
    }

    float Normalize3( float v[3] )
    {
        const float len = sqrtf( v[0] * v[0] + v[1] * v[1] + v[2] * v[2] );
        if ( len > 1.0e-6f )
        {
            const float inv = 1.0f / len;
            v[0] *= inv; v[1] *= inv; v[2] *= inv;
        }
        return len;
    }

    void Cross3( const float a[3], const float b[3], float out[3] )
    {
        out[0] = a[1] * b[2] - a[2] * b[1];
        out[1] = a[2] * b[0] - a[0] * b[2];
        out[2] = a[0] * b[1] - a[1] * b[0];
    }

    const char *Key( entity_s_def *def, const char *name )
    {
        return def ? ValueForKey2( def, name ) : "";
    }

    entity_s_def *FindTarget( const char *name )
    {
        if ( !name || !*name )
            return nullptr;
        for ( entity_s *e = entities.next; e && e != &entities; e = e->next )
        {
            const char *candidate = Key( (entity_s_def *)e, "targetname" );
            if ( candidate && !strcmp( candidate, name ) )
                return (entity_s_def *)e;
        }
        return nullptr;
    }

    void NormalizeMax( const float in[3], float out[3] )
    {
        float m = in[0];
        if ( in[1] > m ) m = in[1];
        if ( in[2] > m ) m = in[2];
        if ( m == 0.0f )
        {
            // cod4map PrimaryLight_ColorNormalize uses white for a zero vector.
            out[0] = out[1] = out[2] = 1.0f;
            return;
        }
        out[0] = in[0] / m;
        out[1] = in[1] / m;
        out[2] = in[2] / m;
    }

    bool ReadLight( selbrush_t *brush, LightInfo *out )
    {
        if ( !brush || !brush->owner || !brush->def || !out )
            return false;
        entity_s_def *def = (entity_s_def *)brush->owner->def;
        if ( !def || !def->eclass || !( def->eclass->classtype & 1 ) )
            return false;

        memset( out, 0, sizeof(*out) );
        out->def = def;
        out->brush = brush;
        out->origin[0] = 0.5f * ( brush->def->mins[0] + brush->def->maxs[0] );
        out->origin[1] = 0.5f * ( brush->def->mins[1] + brush->def->maxs[1] );
        out->origin[2] = 0.5f * ( brush->def->mins[2] + brush->def->maxs[2] );
        out->rawColor[0] = out->rawColor[1] = out->rawColor[2] = 1.0f;
        Entity_GetVec3ForKey( def, out->rawColor, "_color" );
        out->intensity = Entity_GetFloatValueForKey( def, "intensity" );
        out->radius = Entity_GetFloatValueForKey( def, "radius" );
        out->flags = Entity_GetIntValueForKey( def, "spawnflags" );
        out->fovOuter = Entity_GetFloatValueForKey( def, "fov_outer" );
        out->fovInner = Entity_GetFloatValueForKey( def, "fov_inner" );

        float hue[3];
        NormalizeMax( out->rawColor, hue );
        out->effectiveColor[0] = hue[0] * out->intensity;
        out->effectiveColor[1] = hue[1] * out->intensity;
        out->effectiveColor[2] = hue[2] * out->intensity;

        const char *targetName = Key( def, "target" );
        out->hasTargetKey = HasKeyValuePair( def, "target" );
        out->hasTargetName = targetName && *targetName;
        out->target = FindTarget( targetName );
        const char *defName = Key( def, "def" );
        out->defNameLength = defName ? strlen( defName ) : 0;
        if ( out->target )
        {
            memcpy( out->targetPos, out->target->origin, sizeof(out->targetPos) );
            out->travelDir[0] = out->targetPos[0] - out->origin[0];
            out->travelDir[1] = out->targetPos[1] - out->origin[1];
            out->travelDir[2] = out->targetPos[2] - out->origin[2];
            out->targetDistance = Normalize3( out->travelDir );
        }

        if ( out->fovOuter == 0.0f )
        {
            const float d = out->targetDistance;
            out->cosOuter = d / sqrtf( d * d + 4096.0f );
        }
        else
        {
            out->cosOuter = cosf( out->fovOuter * KLIGHT_PI / 360.0f );
        }
        out->cosInner = cosf( out->fovInner * KLIGHT_PI / 360.0f );

        // Ported Entity_Light classifies bit 1 and missing/invalid target cones as omni.
        out->previewSpot = !( out->flags & 1 ) && out->target
                        && out->cosOuter < out->cosInner;
        return true;
    }

    // Status/preview share per-entity gates; the BSP-only 255-primary cap/sort and surface assignment stay compile-time.
    GameLightReject GameLightRejection( const LightInfo &light )
    {
        if ( !( light.flags & 3 ) )
            return KLR_NOT_PRIMARY;
        if ( !light.hasTargetName )
            return KLR_NO_TARGET_NAME;
        if ( !light.target )
            return KLR_UNRESOLVED_TARGET;
        if ( light.defNameLength >= 64 )
            return KLR_DEF_NAME_TOO_LONG;
        if ( light.radius <= 0.0f )
            return KLR_NONPOSITIVE_RADIUS;
        if ( light.fovOuter != 0.0f && light.cosOuter <= 0.0f )
            return KLR_OUTER_FOV_TOO_WIDE;
        if ( ( light.flags & 2 ) && light.cosOuter > light.cosInner )
            return KLR_SPOT_CONE_INVERTED;
        if ( !( light.flags & 8 ) && light.cosOuter <= 0.4990000128746033f )
            return KLR_WIDE_CONE_NEEDS_NOSHADOW;
        return KLR_NONE;
    }

    void GatherSelected( std::vector<LightInfo> &lights,
                         std::vector<selbrush_t *> *allSelection = nullptr )
    {
        lights.clear();
        if ( allSelection ) allSelection->clear();
        std::set<entity_s_def *> seen;
        if ( !selected_brushes.next )
            return;
        for ( selbrush_t *brush = selected_brushes.next;
              brush != &selected_brushes; brush = brush->next )
        {
            if ( allSelection ) allSelection->push_back( brush );
            LightInfo info;
            if ( ReadLight( brush, &info ) && seen.insert( info.def ).second )
                lights.push_back( info );
        }
    }

    bool ShowExtents( entity_s_def *def )
    {
        std::map<entity_s_def *, bool>::iterator it = s_showExtents.find( def );
        return it == s_showExtents.end() ? true : it->second;
    }

    void WriteKeyAll( const std::vector<LightInfo> &lights, const char *key,
                      const char *value, const char *operation )
    {
        if ( lights.empty() || !key || !value )
            return;
        KiwiLightCache_BeginLightEdit();
        Undo_ClearRedo();
        Undo_GeneralStart( operation );
        for ( size_t i = 0; i < lights.size(); ++i )
        {
            Undo_AddEntity_W( (entity_s *)lights[i].def );
            SetKeyValue( lights[i].def, key, value );
        }
        Undo_End();
        MarkMapModified();
        g_nUpdateBits = -1;
        std::vector<entity_s_def *> editedDefs;
        editedDefs.reserve( lights.size() );
        for ( size_t i = 0; i < lights.size(); ++i )
            editedDefs.push_back( lights[i].def );
        KiwiLightCache_RetainUnaffected( &editedDefs[0], (int)editedDefs.size() );
    }

    void WriteFloatAll( const std::vector<LightInfo> &lights, const char *key,
                        float value, const char *operation )
    {
        char text[48];
        FormatFloat( text, sizeof(text), value, 6 );
        WriteKeyAll( lights, key, text, operation );
    }

    bool FloatWidget( FloatField field, const char *label, entity_s_def *def,
                      const char *key, float speed, float lo, float hi )
    {
        FloatEdit &edit = s_floatEdit[field];
        if ( !edit.active && def )
            edit.value = Entity_GetFloatValueForKey( def, key );
        ImGui::SetNextItemWidth( 116.0f );
        ImGui::DragFloat( label, &edit.value, speed, lo, hi, "%.3f",
                          ImGuiSliderFlags_AlwaysClamp );
        edit.active = ImGui::IsItemActive();
        return ImGui::IsItemDeactivatedAfterEdit();
    }

    void ItemTooltip( const char *text )
    {
        if ( !text || !ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenDisabled ) )
            return;
        if ( ImGui::BeginTooltip() )
        {
            ImGui::PushTextWrapPos( ImGui::GetFontSize() * 48.0f );
            ImGui::TextUnformatted( text );
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    void ResetEditors( entity_s_def *def )
    {
        if ( s_uiEntity == def )
            return;
        s_uiEntity = def;
        for ( int i = 0; i < KF_COUNT; ++i )
            s_floatEdit[i].active = false;
        s_exponentActive = false;
        s_colorActive = false;
        s_defActive = false;
        s_defEdit[0] = '\0';
    }

    bool ViewportGestureLive()
    {
        int x0, y0, x1, y1;
        bool crossing;
        return KiwiBox_Rect( &x0, &y0, &x1, &y1, &crossing )
            || KiwiCmd_Active()
            || ImGui::IsMouseDown( ImGuiMouseButton_Left )
            || ImGui::IsMouseDown( ImGuiMouseButton_Right )
            || ImGui::IsMouseDown( ImGuiMouseButton_Middle );
    }

    void PollAutoFocus( entity_s_def *first )
    {
        if ( first != s_observedLight )
        {
            s_observedLight = first;
            s_pendingLightFocus = first;
        }

        if ( !s_pendingLightFocus || ViewportGestureLive() )
            return;

        s_pendingLightFocus = nullptr;
        KiwiWindows_Set( KIWI_WIN_LIGHT, true );
        ImGuiShell_FocusTab( "Light" );
    }

    void GatherLightDefs( std::vector<std::string> &out )
    {
        static bool loaded = false;
        static std::vector<std::string> defs;
        if ( !loaded )
        {
            loaded = true;
            int count = 0;
            const char **files = FS_ListFiles( "lights", "", FS_LIST_ALL, &count );
            for ( int i = 0; files && i < count; ++i )
                if ( files[i] && files[i][0] ) defs.push_back( files[i] );
            if ( files ) FS_FreeFileList( files );
            defs.push_back( "light_point_linear" );
            std::sort( defs.begin(), defs.end() );
            defs.erase( std::unique( defs.begin(), defs.end() ), defs.end() );
        }
        out = defs;
    }

    void GatherTargetNames( std::vector<std::string> &out )
    {
        out.clear();
        for ( entity_s *e = entities.next; e && e != &entities; e = e->next )
        {
            const char *name = Key( (entity_s_def *)e, "targetname" );
            if ( name && *name ) out.push_back( name );
        }
        std::sort( out.begin(), out.end() );
        out.erase( std::unique( out.begin(), out.end() ), out.end() );
    }

    std::string UniqueTargetName()
    {
        for ( int suffix = 1; suffix < 100000; ++suffix )
        {
            char name[64];
            _snprintf( name, sizeof(name), "light_target_%d", suffix );
            name[sizeof(name) - 1] = '\0';
            if ( !FindTarget( name ) ) return name;
        }
        return "light_target";
    }

    bool DropTargetPlaceholder( const float point[3], eclass_t *eclass )
    {
        if ( !eclass )
            return false;
        float mins[3], maxs[3];
        for ( int i = 0; i < 3; ++i )
        {
            mins[i] = point[i] + eclass->mins[i];
            maxs[i] = point[i] + eclass->maxs[i];
            if ( maxs[i] - mins[i] < 1.0f ) maxs[i] = mins[i] + 1.0f;
        }
        Ed_EnsureCurrentMaterial_Kiwi();
        brush_t *def = Brush_Alloc( g_qeglobals.random_texture_stuff, nullptr );
        if ( !def ) return false;
        Brush_Create( mins, maxs, def, nullptr );
        Brush_BuildWindings( def, 1 );
        return KiwiExtrude_LandDef( def ) != nullptr;
    }

    void CreateTarget( const std::vector<LightInfo> &lights )
    {
        if ( lights.empty() )
            return;
        std::vector<selbrush_t *> selection;
        for ( selbrush_t *b = selected_brushes.next;
              b != &selected_brushes; b = b->next )
            selection.push_back( b );

        float dir[3] = { 0.0f, 0.0f, -1.0f };
        if ( lights[0].target && lights[0].targetDistance > 1.0e-6f )
            memcpy( dir, lights[0].travelDir, sizeof(dir) );
        float point[3] = {
            lights[0].origin[0] + dir[0] * 64.0f,
            lights[0].origin[1] + dir[1] * 64.0f,
            lights[0].origin[2] + dir[2] * 64.0f
        };
        eclass_t *infoNull = Eclass_ForName( 0, "info_null" );
        if ( !infoNull )
        {
            Sys_Printf( "Light helper: info_null eclass is unavailable; target not created.\n" );
            return;
        }

        const std::string targetName = UniqueTargetName();
        Select_Deselect( 1 );
        Undo_ClearRedo();
        Undo_GeneralStart( "create light target" );
        bool created = DropTargetPlaceholder( point, infoNull );
        if ( created )
        {
            CreateEntityFromName( "info_null" );
            selbrush_t *targetBrush = selected_brushes.next;
            created = targetBrush && targetBrush != &selected_brushes
                   && targetBrush->owner && targetBrush->owner->def;
            if ( created )
            {
                entity_s_def *targetDef = (entity_s_def *)targetBrush->owner->def;
                // CreateEntityFromName assigns the new undo ID; snapshot only pre-existing lights.
                SetKeyValue( targetDef, "targetname", targetName.c_str() );
                for ( size_t i = 0; i < lights.size(); ++i )
                {
                    Undo_AddEntity_W( (entity_s *)lights[i].def );
                    SetKeyValue( lights[i].def, "target", targetName.c_str() );
                }
            }
        }
        Undo_End();

        Select_Deselect( 1 );
        for ( size_t i = 0; i < selection.size(); ++i )
            Select_Brush( selection[i], 0, 0, 0 );
        if ( created )
        {
            MarkMapModified();
            g_nUpdateBits = -1;
            char x[48], y[48], z[48];
            FormatFloat( x, sizeof(x), point[0], 3 );
            FormatFloat( y, sizeof(y), point[1], 3 );
            FormatFloat( z, sizeof(z), point[2], 3 );
            Sys_Printf( "Light helper: created info_null target %s at %s %s %s.\n",
                        targetName.c_str(), x, y, z );
        }
        else
        {
            Sys_Printf( "Light helper: target creation failed.\n" );
        }
    }

    void BasisForDir( const float dir[3], float right[3], float up[3] )
    {
        const float ref[3] = { 0.0f, 0.0f, fabsf( dir[2] ) > 0.9f ? 0.0f : 1.0f };
        float alt[3] = { 0.0f, 1.0f, 0.0f };
        Cross3( fabsf( dir[2] ) > 0.9f ? alt : ref, dir, right );
        Normalize3( right );
        Cross3( dir, right, up );
        Normalize3( up );
    }

    void EmitRing( const float center[3], const float right[3], const float up[3],
                   float radius, int segments )
    {
        float first[3], prev[3];
        for ( int i = 0; i <= segments; ++i )
        {
            const float a = 2.0f * KLIGHT_PI * (float)( i % segments ) / (float)segments;
            float p[3];
            for ( int k = 0; k < 3; ++k )
                p[k] = center[k] + right[k] * cosf(a) * radius + up[k] * sinf(a) * radius;
            if ( i == 0 ) memcpy( first, p, sizeof(first) );
            else KiwiLines_Add( prev, ( i == segments ) ? first : p );
            memcpy( prev, p, sizeof(prev) );
        }
    }

    void EmitCone( const LightInfo &light, float cosHalf, float brightness )
    {
        if ( cosHalf <= 0.001f || cosHalf >= 1.0f || light.radius <= 0.0f )
            return;
        float right[3], up[3];
        BasisForDir( light.travelDir, right, up );
        const float ringRadius = light.radius * sqrtf( 1.0f - cosHalf * cosHalf );
        const float axialDistance = light.radius * cosHalf;
        float center[3];
        for ( int k = 0; k < 3; ++k )
            center[k] = light.origin[k] + light.travelDir[k] * axialDistance;
        KiwiLines_Color( Clamp(light.effectiveColor[0] * brightness, 0.0f, 1.0f),
                          Clamp(light.effectiveColor[1] * brightness, 0.0f, 1.0f),
                          Clamp(light.effectiveColor[2] * brightness, 0.0f, 1.0f) );
        EmitRing( center, right, up, ringRadius, 16 );
        for ( int i = 0; i < 4; ++i )
        {
            const float a = 0.5f * KLIGHT_PI * (float)i;
            float p[3];
            for ( int k = 0; k < 3; ++k )
                p[k] = center[k] + right[k] * cosf(a) * ringRadius
                                  + up[k] * sinf(a) * ringRadius;
            KiwiLines_Add( light.origin, p );
        }
    }

    void EmitWorldLight( const LightInfo &light )
    {
        KiwiLines_Color( Clamp(light.effectiveColor[0], 0.0f, 1.0f),
                          Clamp(light.effectiveColor[1], 0.0f, 1.0f),
                          Clamp(light.effectiveColor[2], 0.0f, 1.0f) );
        const float x[3] = { 1.0f, 0.0f, 0.0f };
        const float y[3] = { 0.0f, 1.0f, 0.0f };
        const float z[3] = { 0.0f, 0.0f, 1.0f };
        if ( light.radius > 0.0f )
        {
            EmitRing( light.origin, x, y, light.radius, KLIGHT_RING_SEGS );
            EmitRing( light.origin, x, z, light.radius, KLIGHT_RING_SEGS );
            EmitRing( light.origin, y, z, light.radius, KLIGHT_RING_SEGS );
        }
        if ( !light.previewSpot )
            return;
        EmitCone( light, light.cosOuter, 1.0f );
        EmitCone( light, light.cosInner, 0.45f );
        KiwiLines_Color( Clamp(light.effectiveColor[0], 0.0f, 1.0f),
                          Clamp(light.effectiveColor[1], 0.0f, 1.0f),
                          Clamp(light.effectiveColor[2], 0.0f, 1.0f) );
        KiwiLines_Add( light.origin, light.targetPos );
        if ( light.fovOuter == 0.0f )
        {
            float right[3], up[3];
            BasisForDir( light.travelDir, right, up );
            EmitRing( light.targetPos, right, up, 64.0f, 16 );
        }
    }

    bool IsWithinOverlayCap( selbrush_t *wanted )
    {
        int index = 0;
        std::set<entity_s_def *> seen;
        for ( selbrush_t *b = selected_brushes.next;
              b && b != &selected_brushes; b = b->next )
        {
            LightInfo info;
            if ( !ReadLight( b, &info ) || !seen.insert( info.def ).second )
                continue;
            if ( b == wanted ) return index < KLIGHT_MAX_OVERLAYS;
            ++index;
        }
        return false;
    }

    void EmitXYCone( const LightInfo &light, float cosHalf, float brightness, int viewType )
    {
        if ( cosHalf <= 0.001f || cosHalf >= 1.0f || light.radius <= 0.0f )
            return;
        const int h = ( viewType == ED_VIEW_YZ );
        const int v = ( viewType != ED_VIEW_XY ) + 1;
        const int d = 3 - h - v;
        KiwiLines_Color( Clamp(light.effectiveColor[0] * brightness, 0.0f, 1.0f),
                          Clamp(light.effectiveColor[1] * brightness, 0.0f, 1.0f),
                          Clamp(light.effectiveColor[2] * brightness, 0.0f, 1.0f) );
        float origin[3] = { light.origin[0], light.origin[1], light.origin[2] };
        const float axialDistance = light.radius * cosHalf;
        float end[3] = {
            light.origin[0] + light.travelDir[0] * axialDistance,
            light.origin[1] + light.travelDir[1] * axialDistance,
            light.origin[2] + light.travelDir[2] * axialDistance
        };
        origin[d] = end[d] = KLIGHT_XY_DEPTH;
        float ph = -light.travelDir[v], pv = light.travelDir[h];
        const float plen = sqrtf( ph * ph + pv * pv );
        if ( plen > 1.0e-6f ) { ph /= plen; pv /= plen; }
        else { ph = 1.0f; pv = 0.0f; }
        const float width = light.radius * sqrtf( 1.0f - cosHalf * cosHalf );
        float a[3] = { end[0], end[1], end[2] };
        float b[3] = { end[0], end[1], end[2] };
        a[h] += ph * width; a[v] += pv * width;
        b[h] -= ph * width; b[v] -= pv * width;
        KiwiLines_Add( origin, a );
        KiwiLines_Add( origin, b );
        KiwiLines_Add( a, b );
    }

    void DrawStatus( const LightInfo &light )
    {
        const bool primary = ( light.flags & 3 ) != 0;
        const bool gameSpot = ( light.flags & 2 ) != 0;
        const char *type = !primary ? "baked-only light"
                         : ( gameSpot ? ( ( light.flags & 1 )
                                             ? "primary spot (both type bits set)"
                                             : "primary spot" )
                                      : "primary omni" );
        ImGui::Text( "Game type: %s", type );
        if ( !primary )
            ImGui::TextDisabled( "Baked-only lights do not exist as runtime dynamic lights." );
        if ( ( light.flags & 3 ) == 3 )
            ImGui::TextColored( ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                                "Compiler chooses spot; the faithful editor preview classifies bit 1 as omni." );

        const GameLightReject reject = GameLightRejection( light );
        const char *compilerReject = nullptr;
        switch ( reject )
        {
        case KLR_NO_TARGET_NAME:          compilerReject = "no target name"; break;
        case KLR_UNRESOLVED_TARGET:       compilerReject = "target does not resolve"; break;
        case KLR_DEF_NAME_TOO_LONG:       compilerReject = "def name is 64+ bytes"; break;
        case KLR_NONPOSITIVE_RADIUS:      compilerReject = "radius must be positive"; break;
        case KLR_OUTER_FOV_TOO_WIDE:      compilerReject = "fov_outer must be below 180 degrees"; break;
        case KLR_SPOT_CONE_INVERTED:      compilerReject = "fov_inner exceeds fov_outer"; break;
        case KLR_WIDE_CONE_NEEDS_NOSHADOW:compilerReject = "wide cone requires PRIMARY_NOSHADOWMAP"; break;
        default: break;
        }
        if ( compilerReject )
            ImGui::TextColored( ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                                "Compiler: ignored (%s)", compilerReject );
        else if ( primary )
        {
            ImGui::Text( "Compiler: runtime primary light (per-light checks passed)" );
            ImGui::TextDisabled( "The 255-light cap/sort and per-surface assignment are compile-time only." );
        }

        if ( reject == KLR_NO_TARGET_NAME && !light.hasTargetKey )
            ImGui::TextColored( ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                                "cod4rad: no target key -> bakes as a point light; no runtime primary." );
        else if ( reject == KLR_NO_TARGET_NAME )
            ImGui::TextColored( ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                                "cod4rad: empty target value -> unresolved spotlight (nothing bakes)." );
        else if ( reject == KLR_UNRESOLVED_TARGET )
            ImGui::TextColored( ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                                "cod4rad: unresolved target -> spotlight is also ignored (nothing bakes)." );

        const char *shadowReason = "eligible; runtime budget may still omit it";
        bool shadow = primary && !compilerReject && !( light.flags & 8 );
        if ( !primary ) shadowReason = "baked-only";
        else if ( compilerReject ) shadowReason = "compiler ignored light";
        else if ( light.flags & 8 ) shadowReason = "PRIMARY_NOSHADOWMAP";
        ImGui::Text( "Shadowmap: %s (%s)", shadow ? "yes" : "no", shadowReason );

        char outer[48], inner[48], red[48], green[48], blue[48];
        float compilerCosInner = light.cosInner;
        if ( !gameSpot && light.cosOuter > compilerCosInner )
            compilerCosInner = light.cosOuter * 0.75f + 0.25f;
        FormatFloat( outer, sizeof(outer), light.cosOuter, 4 );
        FormatFloat( inner, sizeof(inner), compilerCosInner, 4 );
        FormatFloat( red, sizeof(red), light.effectiveColor[0], 3 );
        FormatFloat( green, sizeof(green), light.effectiveColor[1], 3 );
        FormatFloat( blue, sizeof(blue), light.effectiveColor[2], 3 );
        ImGui::Text( "cos outer %s   inner %s", outer, inner );
        ImGui::Text( "Effective colour %s %s %s", red, green, blue );
        if ( light.target )
        {
            char distance[48];
            FormatFloat( distance, sizeof(distance), light.targetDistance, 2 );
            ImGui::Text( "Target distance: %s", distance );
        }
        else ImGui::TextDisabled( "Target distance: no resolved target" );
        if ( light.fovOuter == 0.0f && light.target )
        {
            const float derived = 2.0f * acosf( Clamp(light.cosOuter, -1.0f, 1.0f) )
                                * 180.0f / KLIGHT_PI;
            char degrees[48];
            FormatFloat( degrees, sizeof(degrees), derived, 2 );
            ImGui::TextDisabled( "fov_outer 0 -> 64u target circle -> %s degrees", degrees );
        }
        if ( primary && !( light.flags & 8 ) && light.cosOuter <= 0.4990000128746033f )
            ImGui::TextColored( ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                                "Warning: cod4map requires PRIMARY_NOSHADOWMAP for this cone." );
        if ( gameSpot && light.cosOuter > light.cosInner )
            ImGui::TextColored( ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                                "Invalid cone: fov_inner must be smaller than fov_outer." );
        else if ( gameSpot && light.cosOuter == light.cosInner )
            ImGui::TextColored( ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                                "Degenerate cone: cod4map accepts equality, but the runtime shader requires outer < inner." );
    }
}

bool KiwiLight_PreviewPrimaryOnly()
{
    return s_previewPrimaryOnly;
}

bool KiwiLight_PerPixelPreviewReady( const Material *multiplyMaterial,
                                     const Material *clearMaterial )
{
    const bool multiplyMissing = !multiplyMaterial
                              || ( rgp.defaultMaterial
                                && Material_IsDefault( multiplyMaterial ) );
    const bool clearMissing = !clearMaterial
                           || ( rgp.defaultMaterial && Material_IsDefault( clearMaterial ) );
    const bool stencilMissing = !rgp.stencilShadowMaterial
                             || ( rgp.defaultMaterial
                               && Material_IsDefault( rgp.stencilShadowMaterial ) );
    if ( !multiplyMissing && !clearMissing && !stencilMissing )
        return true;

    static bool s_reported = false;
    if ( !s_reported )
    {
        s_reported = true;
        Sys_Printf( "Light preview: faithful per-light pass unavailable (%s missing) - "
                    "drawing glow spheres instead.\n",
                    multiplyMissing ? "white_multiply"
                  : clearMissing ? "clearAlphaStencil material" : "stencilshadow material" );
    }
    return false;
}

bool KiwiLight_KeepMissingTechnique( int technique )
{
    return technique == TECHNIQUE_LIGHT_SPOT
        || technique == TECHNIQUE_LIGHT_OMNI;
}

bool KiwiLight_GameWillRender( selbrush_t *brush )
{
    const unsigned epoch = KiwiWalkCache_Epoch();
    if ( s_gameEligibilityEpoch != epoch )
    {
        s_gameEligibility.clear();
        s_gameEligibilityEpoch = epoch;
    }
    std::map<selbrush_t *, bool>::const_iterator cached = s_gameEligibility.find( brush );
    if ( cached != s_gameEligibility.end() )
        return cached->second;

    LightInfo light;
    const bool accepted = ReadLight( brush, &light )
                       && GameLightRejection( light ) == KLR_NONE;
    s_gameEligibility[brush] = accepted;
    return accepted;
}

void KiwiLight_RegisterCommands()
{
    Radiant_RegisterCommand( "KiwiWindowLight", 0, 0, KIWI_CMD_WINDOW_LIGHT );
}

void KiwiLight_DrawWorld()
{
    std::vector<LightInfo> lights;
    GatherSelected( lights );
    if ( lights.empty() )
        return;
    // 72 sphere + 40 cone + 1 axis + 16 default-target-ring segments.
    KiwiLines_Begin( KLIGHT_MAX_OVERLAYS * 129, 2 );
    const int count = (int)lights.size() < KLIGHT_MAX_OVERLAYS
                    ? (int)lights.size() : KLIGHT_MAX_OVERLAYS;
    for ( int i = 0; i < count; ++i )
        if ( ShowExtents( lights[i].def ) ) EmitWorldLight( lights[i] );
    KiwiLines_Flush();
}

void KiwiLight_DrawXY( selbrush_t *brush, int viewType )
{
    if ( viewType != ED_VIEW_XY && viewType != ED_VIEW_XZ && viewType != ED_VIEW_YZ )
        return;
    LightInfo light;
    if ( !ReadLight( brush, &light ) || !IsWithinOverlayCap( brush )
      || !ShowExtents( light.def ) )
        return;
    KiwiLines_Begin( 8, 2 );
    KiwiLines_Color( Clamp(light.effectiveColor[0], 0.0f, 1.0f),
                      Clamp(light.effectiveColor[1], 0.0f, 1.0f),
                      Clamp(light.effectiveColor[2], 0.0f, 1.0f) );
    // Ed_DrawSelectedRadius supplies the view-plane circle; add only the spot cone and axis.
    if ( light.previewSpot )
    {
        EmitXYCone( light, light.cosOuter, 1.0f, viewType );
        EmitXYCone( light, light.cosInner, 0.45f, viewType );
        KiwiLines_Color( Clamp(light.effectiveColor[0], 0.0f, 1.0f),
                          Clamp(light.effectiveColor[1], 0.0f, 1.0f),
                          Clamp(light.effectiveColor[2], 0.0f, 1.0f) );
        const int h = ( viewType == ED_VIEW_YZ );
        const int v = ( viewType != ED_VIEW_XY ) + 1;
        const int d = 3 - h - v;
        float a[3] = { light.origin[0], light.origin[1], light.origin[2] };
        float b[3] = { light.targetPos[0], light.targetPos[1], light.targetPos[2] };
        a[d] = b[d] = KLIGHT_XY_DEPTH;
        KiwiLines_Add( a, b );
    }
    KiwiLines_Flush();
}

void KiwiLight_Draw()
{
    std::vector<LightInfo> lights;
    std::vector<selbrush_t *> allSelection;
    GatherSelected( lights, &allSelection );
    entity_s_def *first = lights.empty() ? nullptr : lights[0].def;
    entity_s_def *focusFirst = nullptr;
    if ( !allSelection.empty() )
    {
        LightInfo focusInfo;
        if ( ReadLight( allSelection[0], &focusInfo ) )
            focusFirst = focusInfo.def;
    }
    PollAutoFocus( focusFirst );
    ResetEditors( first );

    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_LIGHT );
    if ( !open || !*open )
        return;
    if ( KiwiWindows_JustOpened( KIWI_WIN_LIGHT ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );

    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_LIGHT ), open ) )
    {
        if ( lights.empty() )
        {
            ImGui::TextWrapped( "Select a light entity to edit its runtime and bake parameters." );
            ImGui::TextDisabled( "Non-primary lights are baked only. Primary omni/spot lights "
                                 "also exist in the game at runtime." );
        }
        else
        {
            LightInfo &light = lights[0];
            ImGui::Text( "%d selected light%s", (int)lights.size(), lights.size() == 1 ? "" : "s" );
            DrawStatus( light );
            ImGui::Separator();

            if ( !s_colorActive )
                memcpy( s_colorEdit, light.rawColor, sizeof(s_colorEdit) );
            ImGui::SetNextItemWidth( 220.0f );
            ImGui::ColorEdit3( "_color", s_colorEdit,
                               ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB
                             | ImGuiColorEditFlags_HDR );
            s_colorActive = ImGui::IsItemActive();
            const bool colorDeactivated = ImGui::IsItemDeactivatedAfterEdit();
            ItemTooltip( KTIP_COLOR );
            if ( colorDeactivated )
            {
                // HDR mode lets a channel be dragged BELOW zero; cod4rad degammas the
                // colour with powf, and a negative base is NaN (the "!IS_NAN_FLOAT(energy)"
                // sanity check on kisak_trash, 2026-09-05).  Above 1 is fine, below 0 is not.
                for ( int k = 0; k < 3; ++k )
                    if ( !( s_colorEdit[k] >= 0.0f ) )
                        s_colorEdit[k] = 0.0f;
                char text[96];
                char red[28], green[28], blue[28];
                FormatFloat( red, sizeof(red), s_colorEdit[0], 6 );
                FormatFloat( green, sizeof(green), s_colorEdit[1], 6 );
                FormatFloat( blue, sizeof(blue), s_colorEdit[2], 6 );
                _snprintf( text, sizeof(text), "%s %s %s", red, green, blue );
                text[sizeof(text) - 1] = '\0';
                WriteKeyAll( lights, "_color", text, "light colour" );
            }

            if ( FloatWidget( KF_INTENSITY, "intensity", first, "intensity", 0.05f, 0.0f, 10.0f ) )
                WriteFloatAll( lights, "intensity", s_floatEdit[KF_INTENSITY].value, "light intensity" );
            ItemTooltip( KTIP_INTENSITY );
            if ( FloatWidget( KF_RADIUS, "radius", first, "radius", 4.0f, 1.0f, 4096.0f ) )
                WriteFloatAll( lights, "radius", s_floatEdit[KF_RADIUS].value, "light radius" );
            ItemTooltip( KTIP_RADIUS );

            if ( FloatWidget( KF_FOV_OUTER, "fov_outer", first, "fov_outer", 0.25f, 0.0f, 180.0f ) )
            {
                float value = s_floatEdit[KF_FOV_OUTER].value;
                const float inner = Entity_GetFloatValueForKey( first, "fov_inner" );
                if ( value > 0.0f && value <= inner ) value = Clamp( inner + 0.1f, 0.1f, 180.0f );
                s_floatEdit[KF_FOV_OUTER].value = value;
                WriteFloatAll( lights, "fov_outer", value, "light outer fov" );
            }
            ItemTooltip( KTIP_FOV_OUTER );
            if ( FloatWidget( KF_FOV_INNER, "fov_inner", first, "fov_inner", 0.25f, 0.0f, 180.0f ) )
            {
                float outer = light.fovOuter;
                if ( outer == 0.0f )
                    outer = 2.0f * acosf( Clamp(light.cosOuter, -1.0f, 1.0f) ) * 180.0f / KLIGHT_PI;
                float value = s_floatEdit[KF_FOV_INNER].value;
                if ( outer > 0.0f && value >= outer ) value = Clamp( outer - 0.1f, 0.0f, 179.9f );
                s_floatEdit[KF_FOV_INNER].value = value;
                WriteFloatAll( lights, "fov_inner", value, "light inner fov" );
            }
            ItemTooltip( KTIP_FOV_INNER );
            if ( !s_exponentActive )
                s_exponentEdit = Entity_GetIntValueForKey( first, "exponent" );
            ImGui::SetNextItemWidth( 116.0f );
            ImGui::DragInt( "exponent", &s_exponentEdit, 1.0f, 0, 10, "%d",
                            ImGuiSliderFlags_AlwaysClamp );
            s_exponentActive = ImGui::IsItemActive();
            const bool exponentDeactivated = ImGui::IsItemDeactivatedAfterEdit();
            ItemTooltip( KTIP_EXPONENT );
            if ( exponentDeactivated )
            {
                char exponent[16];
                _snprintf( exponent, sizeof(exponent), "%d", s_exponentEdit );
                exponent[sizeof(exponent) - 1] = '\0';
                WriteKeyAll( lights, "exponent", exponent, "light exponent" );
            }

            std::vector<std::string> defs;
            GatherLightDefs( defs );
            // KIWI FIX: Key() hands back a pointer INTO the entity's epair value storage.
            // WriteKeyAll -> SetKeyValue -> sub_483500 free()s that exact block and
            // reallocates it, and WriteKeyAll is called from inside the combo below — so a
            // raw pointer dangles for the remaining Selectable compares, the empty-check and
            // the strncpy into s_defEdit.  Snapshot the value before anything can mutate it.
            const std::string liveDefValue( Key( first, "def" ) );
            const char *liveDef = liveDefValue.c_str();
            const char *defPreview = ( liveDef && *liveDef ) ? liveDef : "light_point_linear (compiler default)";
            const bool defOpen = ImGui::BeginCombo( "def", defPreview );
            ItemTooltip( KTIP_DEF );
            if ( defOpen )
            {
                if ( ImGui::Selectable( "light_point_linear (compiler default)", !liveDef || !*liveDef ) )
                    WriteKeyAll( lights, "def", "light_point_linear", "light falloff definition" );
                for ( size_t i = 0; i < defs.size(); ++i )
                    if ( ImGui::Selectable( defs[i].c_str(), liveDef && !_stricmp(liveDef, defs[i].c_str()) ) )
                        WriteKeyAll( lights, "def", defs[i].c_str(), "light falloff definition" );
                ImGui::EndCombo();
            }
            if ( !liveDef || !*liveDef )
                ImGui::TextDisabled( "Editor preview uses light_dynamic until this key is written." );
            if ( !s_defActive )
            {
                strncpy( s_defEdit, liveDef ? liveDef : "", sizeof(s_defEdit) - 1 );
                s_defEdit[sizeof(s_defEdit) - 1] = '\0';
            }
            ImGui::SetNextItemWidth( 220.0f );
            const bool defEnter = ImGui::InputText( "def (custom)", s_defEdit, sizeof(s_defEdit),
                                                    ImGuiInputTextFlags_EnterReturnsTrue );
            s_defActive = ImGui::IsItemActive();
            const bool defDeactivated = ImGui::IsItemDeactivatedAfterEdit();
            ItemTooltip( KTIP_DEF );
            if ( defEnter || defDeactivated )
                WriteKeyAll( lights, "def", s_defEdit[0] ? s_defEdit : "light_point_linear",
                             "light falloff definition" );

            ImGui::SeparatorText( "Primary light flags (?)" );
            ItemTooltip( KTIP_PRIMARY_HEADER );
            bool allAreLights = !allSelection.empty();
            for ( size_t i = 0; i < allSelection.size(); ++i )
            {
                entity_s_def *def = allSelection[i]->owner
                                  ? (entity_s_def *)allSelection[i]->owner->def : nullptr;
                if ( !def || !def->eclass || !( def->eclass->classtype & 1 ) )
                    allAreLights = false;
            }
            ImGui::BeginDisabled( !allAreLights || !edit_entity );
            int flags = light.flags;
            bool omni = ( flags & 1 ) != 0;
            bool spot = ( flags & 2 ) != 0;
            bool scriptable = ( flags & 4 ) != 0;
            bool noShadow = ( flags & 8 ) != 0;
            bool flagChanged = false;
            if ( ImGui::Checkbox( "PRIMARY_OMNI", &omni ) ) flagChanged = true;
            ItemTooltip( KTIP_PRIMARY_OMNI );
            if ( ImGui::Checkbox( "PRIMARY_SPOT", &spot ) ) flagChanged = true;
            ItemTooltip( KTIP_PRIMARY_SPOT );
            if ( ImGui::Checkbox( "PRIMARY_SCRIPTABLE", &scriptable ) ) flagChanged = true;
            ItemTooltip( KTIP_PRIMARY_SCRIPTABLE );
            if ( ImGui::Checkbox( "PRIMARY_NOSHADOWMAP", &noShadow ) ) flagChanged = true;
            ItemTooltip( KTIP_PRIMARY_NOSHADOWMAP );
            if ( flagChanged )
            {
                KiwiLightCache_BeginLightEdit();
                flags &= ~15;
                if ( omni ) flags |= 1;
                if ( spot ) flags |= 2;
                if ( scriptable ) flags |= 4;
                if ( noShadow ) flags |= 8;
                SpawnFlags_Apply( flags );
                MarkMapModified();
                g_nUpdateBits = -1;
                std::vector<entity_s_def *> editedDefs;
                editedDefs.reserve( lights.size() );
                for ( size_t i = 0; i < lights.size(); ++i )
                    editedDefs.push_back( lights[i].def );
                KiwiLightCache_RetainUnaffected( &editedDefs[0], (int)editedDefs.size() );
            }
            ImGui::EndDisabled();
            if ( !allAreLights )
                ImGui::TextDisabled( "Spawnflag editing is disabled for a mixed selection." );

            if ( scriptable )
            {
                if ( FloatWidget( KF_MAXTURN, "maxturn", first, "maxturn", 0.25f, 0.0f, 180.0f ) )
                    WriteFloatAll( lights, "maxturn", s_floatEdit[KF_MAXTURN].value, "light maxturn" );
                ItemTooltip( KTIP_MAXTURN );
                if ( FloatWidget( KF_MAXMOVE, "maxmove", first, "maxmove", 1.0f, 0.0f, 4096.0f ) )
                    WriteFloatAll( lights, "maxmove", s_floatEdit[KF_MAXMOVE].value, "light maxmove" );
                ItemTooltip( KTIP_MAXMOVE );
            }

            ImGui::SeparatorText( "Target (?)" );
            ItemTooltip( KTIP_TARGET );
            // KIWI FIX: same epair use-after-free as the "def" combo above — the "<none>"
            // and per-name Selectables call WriteKeyAll, which free()s this entity's
            // "target" value string while the loop still compares against it.
            const std::string liveTargetValue( Key( first, "target" ) );
            const char *liveTarget = liveTargetValue.c_str();
            std::vector<std::string> targets;
            GatherTargetNames( targets );
            const bool targetOpen = ImGui::BeginCombo( "target", liveTarget && *liveTarget ? liveTarget : "<none>" );
            ItemTooltip( KTIP_TARGET );
            if ( targetOpen )
            {
                if ( ImGui::Selectable( "<none>", !liveTarget || !*liveTarget ) )
                    WriteKeyAll( lights, "target", "", "light target" );
                for ( size_t i = 0; i < targets.size(); ++i )
                    if ( ImGui::Selectable( targets[i].c_str(), liveTarget && !strcmp(liveTarget, targets[i].c_str()) ) )
                        WriteKeyAll( lights, "target", targets[i].c_str(), "light target" );
                ImGui::EndCombo();
            }
            if ( ImGui::Button( "Create target" ) )
                CreateTarget( lights );
            ItemTooltip( KTIP_CREATE_TARGET );

            ImGui::SeparatorText( "Preview" );
            bool preview = g_PrefsDlg->enable_light_preview != 0;
            if ( ImGui::Checkbox( "Enable light preview", &preview ) )
                Radiant_ExecCommand( 33950 );
            ItemTooltip( KTIP_ENABLE_PREVIEW );
            // Retail's field name is inverted: false forces 10,000,000; true uses the key.
            bool maxIntensity = !g_qeglobals.preview_at_max_intensity;
            if ( ImGui::Checkbox( "Preview at max intensity", &maxIntensity ) )
                Radiant_ExecCommand( 36122 );
            ItemTooltip( KTIP_MAX_INTENSITY );
            if ( ImGui::Checkbox( "Preview only primary lights (as the game does)",
                                  &s_previewPrimaryOnly ) )
                g_nUpdateBits = -1;
            ItemTooltip( KTIP_PRIMARY_ONLY );
            ImGui::TextDisabled( "baked-only lights: bounce & radiosity not previewed" );
            bool sunPreview = KiwiSunPreview_Enabled();
            if ( ImGui::Checkbox( "Sun preview", &sunPreview ) )
            {
                KiwiSunPreview_SetEnabled( sunPreview );
                g_nUpdateBits = -1;
            }
            ItemTooltip( KTIP_SUN_PREVIEW );
            const char *sunStatus = KiwiSunPreview_Status();
            ImGui::TextDisabled( "Sun preview: %s",
                                 sunStatus && *sunStatus ? sunStatus : "status unavailable" );
            if ( ImGui::Button( "Pin this light" ) )
            {
                CamWnd_AddLightPreview( light.brush, 0,
                    (const orientation_t *)world_orient_matrix );
                g_nUpdateBits = -1;
            }
            ItemTooltip( KTIP_PIN );
            bool show = ShowExtents( first );
            if ( ImGui::Checkbox( "Show extents", &show ) )
            {
                for ( size_t i = 0; i < lights.size(); ++i ) s_showExtents[lights[i].def] = show;
                g_nUpdateBits = -1;
            }
            ItemTooltip( KTIP_EXTENTS );
        }
    }
    ImGui::End();
}

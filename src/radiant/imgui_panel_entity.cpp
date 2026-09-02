// KIWI: typed entity inspector over the existing win_ent read/write funnels.
#include "stdafx.h"
#include "qe3.h"
#include "kiwi_entinspect.h"
#include "kiwi_fmt.h"
#include "kiwi_windows.h"
#include "radiant_ui_actions.h"

#include <imgui/imgui.h>

#include <algorithm>
#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <map>
#include <set>
#include <stdlib.h>
#include <string>
#include <vector>

extern entity_s_def *edit_entity;              // win_ent.cpp:77
extern int           multiple_edit_entities;   // win_ent.cpp:78
extern entity_s      entities;                 // entity.cpp:295
extern entity_s      entityInsts;              // entity.cpp:302
extern entity_s     *world_entity;             // map.cpp:62
extern int           g_nUpdateBits;             // engine_stubs.cpp:773

struct eclassRow_t
{
    const char *name;
    eclass_t   *eclass;
};

extern void EclassList_Gather( std::vector<eclassRow_t> &rows );       // win_ent.cpp:167
extern int  SpawnFlags_Gather();                                      // win_ent.cpp:456
extern void SpawnFlagBit_Apply( int bit, int checked );                // win_ent.cpp:521
extern void EclassCreate_Apply( const char *name );                    // win_ent.cpp:572
extern void EclassSelect_Apply( int listIndex, eclass_t *pec );        // win_ent.cpp:778
extern void Entity_UpdateSelection();                                  // win_ent.cpp:791
extern void ImGuiShell_FocusTab( const char *title );                  // imgui_shell.cpp:205
extern ImGuiID ImGuiShell_DockRoot();                                  // imgui_shell.cpp:798
extern void MarkMapModified( void );                                   // win_qe3.cpp:195

namespace
{
    bool s_entityFocused = false;
    bool s_editWorldspawn = false;
    bool s_previousSelectionEmpty = true;
    entity_s_def *s_previousEntity = nullptr;

    int  s_selEclass = -1;
    char s_eclassFilter[64] = { 0 };
    char s_key[4096] = { 0 };
    char s_value[4096] = { 0 };

    struct PanelEditState
    {
        PanelEditState()
            : integerValue( 0 ), boolValue( false ), wasActive( false )
        {
            text[0] = '\0';
            floatValue[0] = floatValue[1] = floatValue[2] = 0.0f;
        }

        char  text[4096];
        float floatValue[3];
        int   integerValue;
        bool  boolValue;
        bool  wasActive;
    };

    std::map<std::string, PanelEditState> s_editStates;
    entity_s_def *s_editStateEntity = nullptr;

    struct OtherKeyRow
    {
        std::string key;
        std::string value;
    };

    void CopyField( char *dst, size_t dstSize, const char *src )
    {
        if ( !dst || dstSize == 0 )
            return;
        dst[0] = '\0';
        if ( src )
        {
            strncpy( dst, src, dstSize - 1 );
            dst[dstSize - 1] = '\0';
        }
    }

    char LowerAscii( char c )
    {
        return ( c >= 'A' && c <= 'Z' ) ? (char)( c + ( 'a' - 'A' ) ) : c;
    }

    std::string LowerString( const char *value )
    {
        std::string out = value ? value : "";
        for ( size_t i = 0; i < out.size(); ++i )
            out[i] = LowerAscii( out[i] );
        return out;
    }

    bool ContainsNoCase( const char *haystack, const char *needle )
    {
        if ( !needle || !needle[0] )
            return true;
        if ( !haystack )
            return false;
        for ( const char *h = haystack; *h; ++h )
        {
            const char *a = h;
            const char *b = needle;
            while ( *a && *b && LowerAscii( *a ) == LowerAscii( *b ) )
            {
                ++a;
                ++b;
            }
            if ( !*b )
                return true;
        }
        return false;
    }

    bool StartsNoCase( const char *value, const char *prefix )
    {
        if ( !value || !prefix )
            return false;
        while ( *prefix )
        {
            if ( !*value || LowerAscii( *value ) != LowerAscii( *prefix ) )
                return false;
            ++value;
            ++prefix;
        }
        return true;
    }

    epair_t *FindEpair( entity_s_def *entity, const char *key )
    {
        if ( !entity || !key )
            return nullptr;
        for ( epair_t *ep = entity->epairs; ep; ep = ep->next )
            if ( !_stricmp( ep->key, key ) )
                return ep;
        return nullptr;
    }

    const char *EntityClassname( entity_s_def *entity )
    {
        if ( entity && entity->eclass && entity->eclass->name )
            return entity->eclass->name;
        epair_t *classname = FindEpair( entity, "classname" );
        return classname ? classname->value : nullptr;
    }

    bool IsWorldspawn( entity_s_def *entity )
    {
        const char *classname = EntityClassname( entity );
        return classname && !_stricmp( classname, "worldspawn" );
    }

    bool IsLightClass( entity_s_def *entity )
    {
        const char *classname = EntityClassname( entity );
        return classname && StartsNoCase( classname, "light" );
    }

    bool IsTriggerClass( entity_s_def *entity )
    {
        const char *classname = EntityClassname( entity );
        return classname && StartsNoCase( classname, "trigger_" );
    }

    bool IsSunTabKey( const char *key )
    {
        return key && ( !_stricmp( key, "sundirection" ) ||
                        !_stricmp( key, "sunlight" ) ||
                        !_stricmp( key, "suncolor" ) );
    }

    bool IsLightTabKey( const char *key )
    {
        static const char *const owned[] =
        {
            "_color", "intensity", "radius", "fov_outer", "fov_inner",
            "exponent", "def", "maxturn", "maxmove", "target", "spawnflags"
        };
        if ( !key )
            return false;
        for ( size_t i = 0; i < sizeof( owned ) / sizeof( owned[0] ); ++i )
            if ( !_stricmp( key, owned[i] ) )
                return true;
        return false;
    }

    bool ParameterExists( const std::vector<KiwiEntParameter> &parameters, const char *name )
    {
        for ( size_t i = 0; i < parameters.size(); ++i )
            if ( !_stricmp( parameters[i].name.c_str(), name ) )
                return true;
        return false;
    }

    void AddParameter( std::vector<KiwiEntParameter> *parameters,
                       const char *name, const char *description )
    {
        if ( !name || !name[0] || ParameterExists( *parameters, name ) )
            return;
        parameters->push_back( KiwiEntInspect_InferParameter( name, description, "" ) );
    }

    void BuildProperties( entity_s_def *entity, const KiwiEntSchema &schema,
                          std::vector<KiwiEntParameter> *properties )
    {
        properties->clear();
        if ( !entity )
            return;

        const bool world = IsWorldspawn( entity );
        const bool light = IsLightClass( entity );
        const bool fixed = entity->eclass && entity->eclass->fixedsize;
        const bool hasAngles = FindEpair( entity, "angles" ) != nullptr;
        bool schemaHasAngles = false;

        for ( size_t i = 0; i < schema.parameters.size(); ++i )
        {
            const KiwiEntParameter &parameter = schema.parameters[i];
            const char *key = parameter.name.c_str();
            if ( !_stricmp( key, "classname" ) || !_stricmp( key, "spawnflags" ) )
                continue;
            if ( !_stricmp( key, "angles" ) )
            {
                schemaHasAngles = true;
                if ( ( !fixed || IsTriggerClass( entity ) ) && !hasAngles )
                    continue;
            }
            if ( !_stricmp( key, "origin" ) && !fixed )
                continue;
            if ( world && IsSunTabKey( key ) )
                continue;
            if ( light && IsLightTabKey( key ) )
                continue;
            if ( !ParameterExists( *properties, key ) )
                properties->push_back( parameter );
        }

        if ( world )
            return;

        AddParameter( properties, "targetname",
                      "Name other entities and scripts use to address this entity." );
        if ( !light )
            AddParameter( properties, "target",
                          "Targetname of the entity this entity activates or points at." );
        AddParameter( properties, "script_noteworthy",
                      "Optional script tag for map-specific behavior." );
        AddParameter( properties, "script_linkname",
                      "Optional script link group name." );

        if ( fixed )
            AddParameter( properties, "origin", "World position of this point entity." );

        const bool modelOrPoint = fixed || ( entity->eclass && ( entity->eclass->classtype & 8 ) );
        const bool brushOrTrigger = !fixed || IsTriggerClass( entity );
        const bool showAngles = hasAngles ||
                                ( !brushOrTrigger && ( schemaHasAngles || modelOrPoint ) );
        if ( showAngles )
            AddParameter( properties, "angles", "Pitch, yaw and roll orientation in degrees." );
    }

    entity_s *FindEntityInstance( entity_s_def *def )
    {
        if ( !def )
            return nullptr;
        for ( entity_s *instance = entityInsts.next;
              instance && instance != &entityInsts;
              instance = instance->next )
        {
            if ( instance->def == (entity_s *)def )
                return instance;
        }
        return nullptr;
    }

    int BrushCount( entity_s_def *def )
    {
        entity_s *instance = FindEntityInstance( def );
        if ( !instance )
            return 0;
        int count = 0;
        for ( selbrush_t *brush = instance->brushes.ownerNext;
              brush && brush != &instance->brushes;
              brush = brush->ownerNext )
            ++count;
        return count;
    }

    bool SelectionIsEmpty()
    {
        return !selected_brushes.next || selected_brushes.next == &selected_brushes;
    }

    void MarkEntityWrite()
    {
        MarkMapModified();
        g_nUpdateBits = -1;
    }

    void SetEntityKey( const char *key, const char *value )
    {
        if ( !edit_entity || !key || !key[0] || !value )
            return;
        EntSetKey_Apply( key, value );
        MarkEntityWrite();
    }

    void DeleteEntityKey( const char *key )
    {
        if ( !edit_entity || !key || !key[0] )
            return;
        EntDeleteKey_Apply( key );
        if ( _stricmp( key, "classname" ) )
            MarkEntityWrite();
    }

    void SetEntitySpawnFlags( int flags )
    {
        if ( !edit_entity )
            return;
        SpawnFlags_Apply( flags );
        MarkEntityWrite();
    }

    PanelEditState &EditState( const char *key )
    {
        if ( s_editStateEntity != edit_entity )
        {
            s_editStates.clear();
            s_editStateEntity = edit_entity;
        }
        return s_editStates[LowerString( key )];
    }

    bool ParseFloats( const char *text, float *out, int count )
    {
        if ( !text || !out || count <= 0 )
            return false;
        const char *cursor = text;
        for ( int i = 0; i < count; ++i )
        {
            while ( *cursor && isspace( (unsigned char)*cursor ) )
                ++cursor;
            char *end = nullptr;
            const double value = strtod( cursor, &end );
            if ( end == cursor || !_finite( value ) || value > FLT_MAX || value < -FLT_MAX )
                return false;
            out[i] = (float)value;
            cursor = end;
        }
        while ( *cursor && isspace( (unsigned char)*cursor ) )
            ++cursor;
        return *cursor == '\0';
    }

    bool ParseInteger( const char *text, int *out )
    {
        if ( !text || !out )
            return false;
        while ( *text && isspace( (unsigned char)*text ) )
            ++text;
        char *end = nullptr;
        const long value = strtol( text, &end, 10 );
        if ( end == text || value < INT_MIN || value > INT_MAX )
            return false;
        while ( *end && isspace( (unsigned char)*end ) )
            ++end;
        if ( *end )
            return false;
        *out = (int)value;
        return true;
    }

    bool ParseBool( const char *text, bool *out )
    {
        if ( !text || !out )
            return false;
        if ( !_stricmp( text, "true" ) )
        {
            *out = true;
            return true;
        }
        if ( !_stricmp( text, "false" ) )
        {
            *out = false;
            return true;
        }
        int value = 0;
        if ( !ParseInteger( text, &value ) )
            return false;
        *out = value != 0;
        return true;
    }

    void FormatVec3Epair( char *out, size_t outSize, const float value[3] )
    {
        char x[48], y[48], z[48];
        _snprintf( out, outSize, "%s %s %s",
                   KiwiFmt_Num( x, sizeof( x ), value[0], 6 ),
                   KiwiFmt_Num( y, sizeof( y ), value[1], 6 ),
                   KiwiFmt_Num( z, sizeof( z ), value[2], 6 ) );
        out[outSize - 1] = '\0';
    }

    void GatherSelectedEntities( std::vector<entity_s_def *> *out )
    {
        out->clear();
        if ( !selected_brushes.next )
            return;
        std::set<entity_s_def *> seen;
        for ( selbrush_t *brush = selected_brushes.next;
              brush != &selected_brushes;
              brush = brush->next )
        {
            entity_s_def *entity = brush->owner ? (entity_s_def *)brush->owner->def : nullptr;
            if ( entity && seen.insert( entity ).second )
                out->push_back( entity );
        }
    }

    bool IsMixedValue( const std::vector<entity_s_def *> &entitiesToCompare, const char *key )
    {
        if ( entitiesToCompare.size() < 2 || !key )
            return false;
        epair_t *first = FindEpair( entitiesToCompare[0], key );
        for ( size_t i = 1; i < entitiesToCompare.size(); ++i )
        {
            epair_t *other = FindEpair( entitiesToCompare[i], key );
            if ( ( first == nullptr ) != ( other == nullptr ) )
                return true;
            if ( first && strcmp( first->value, other->value ) )
                return true;
        }
        return false;
    }

    void AddUniqueSuggestion( std::vector<std::string> *suggestions, const char *value )
    {
        if ( !value || !value[0] )
            return;
        for ( size_t i = 0; i < suggestions->size(); ++i )
            if ( !_stricmp( ( *suggestions )[i].c_str(), value ) )
                return;
        suggestions->push_back( value );
    }

    bool SuggestionLess( const std::string &a, const std::string &b )
    {
        return _stricmp( a.c_str(), b.c_str() ) < 0;
    }

    void GatherTargetSuggestions( std::vector<std::string> *suggestions )
    {
        suggestions->clear();
        for ( entity_s *entity = entities.next;
              entity && entity != &entities;
              entity = entity->next )
        {
            for ( epair_t *ep = entity->epairs; ep; ep = ep->next )
            {
                if ( !_stricmp( ep->key, "targetname" ) || !_stricmp( ep->key, "target" ) )
                    AddUniqueSuggestion( suggestions, ep->value );
            }
        }
        std::sort( suggestions->begin(), suggestions->end(), SuggestionLess );
    }

    void DrawWrappedTooltip( const char *text )
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos( ImGui::GetFontSize() * 35.0f );
        ImGui::TextUnformatted( text ? text : "" );
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    void DrawParameterLabel( const KiwiEntParameter &parameter, bool mixed )
    {
        ImGui::TextUnformatted( parameter.name.c_str() );
        bool showTooltip = ImGui::IsItemHovered();
        ImGui::SameLine();
        ImGui::TextDisabled( "(?)" );
        showTooltip |= ImGui::IsItemHovered();
        if ( mixed )
        {
            ImGui::SameLine();
            ImGui::TextDisabled( "(mixed)" );
        }
        if ( showTooltip )
        {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos( ImGui::GetFontSize() * 35.0f );
            if ( parameter.description.empty() )
                ImGui::TextDisabled( "No description in the entity definition." );
            else
                ImGui::TextUnformatted( parameter.description.c_str() );
            ImGui::Separator();
            if ( parameter.defaultValue.empty() )
                ImGui::TextDisabled( "No .def default." );
            else
                ImGui::Text( "Default: %s", parameter.defaultValue.c_str() );
            if ( parameter.widget == KIWI_ENT_WIDGET_COLOR3 )
            {
                ImGui::Separator();
                ImGui::TextWrapped( "The engine scales the biggest colour component to 1; "
                                    "colour ratios are preserved." );
            }
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    bool DrawTextEditor( PanelEditState *state, const char *source, const char *hint,
                         float width, std::string *outValue )
    {
        if ( !state->wasActive )
            CopyField( state->text, sizeof( state->text ), source );
        ImGui::SetNextItemWidth( width );
        const bool enter = ImGui::InputTextWithHint(
            "##value", hint ? hint : "", state->text, sizeof( state->text ),
            ImGuiInputTextFlags_EnterReturnsTrue );
        const bool done = enter || ImGui::IsItemDeactivatedAfterEdit();
        state->wasActive = ImGui::IsItemActive();
        if ( done )
        {
            *outValue = state->text;
            return true;
        }
        return false;
    }

    bool DrawTargetEditor( PanelEditState *state, const char *source, const char *hint,
                           const std::vector<std::string> &suggestions, float width,
                           std::string *outValue )
    {
        const float comboWidth = ImGui::GetFrameHeight();
        const float available = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
        float textWidth = available - comboWidth - ImGui::GetStyle().ItemSpacing.x;
        if ( textWidth < 40.0f )
            textWidth = 40.0f;
        bool commit = DrawTextEditor( state, source, hint, textWidth, outValue );

        ImGui::SameLine();
        ImGui::SetNextItemWidth( comboWidth );
        if ( ImGui::BeginCombo( "##targets", "", ImGuiComboFlags_NoPreview ) )
        {
            if ( suggestions.empty() )
            {
                ImGui::TextDisabled( "No targetnames or targets in this map" );
            }
            else
            {
                for ( size_t i = 0; i < suggestions.size(); ++i )
                {
                    if ( ImGui::Selectable( suggestions[i].c_str() ) )
                    {
                        CopyField( state->text, sizeof( state->text ), suggestions[i].c_str() );
                        *outValue = suggestions[i];
                        commit = true;
                    }
                }
            }
            ImGui::EndCombo();
        }
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Choose an existing targetname or target from this map." );
        return commit;
    }

    bool DrawTypedEditor( const KiwiEntParameter &parameter, bool present,
                           const std::string &currentValue,
                           const std::vector<std::string> &suggestions,
                           float width, std::string *outValue )
    {
        PanelEditState &state = EditState( parameter.name.c_str() );
        const char *displayValue = present ? currentValue.c_str() : parameter.defaultValue.c_str();

        if ( parameter.widget == KIWI_ENT_WIDGET_TEXT ||
             parameter.widget == KIWI_ENT_WIDGET_MODEL ||
             parameter.widget == KIWI_ENT_WIDGET_DEF )
        {
            std::string hint;
            if ( !present && !parameter.defaultValue.empty() )
                hint = std::string( "default: " ) + parameter.defaultValue;
            else if ( parameter.widget == KIWI_ENT_WIDGET_MODEL )
                hint = "xmodel asset name / path";
            else if ( parameter.widget == KIWI_ENT_WIDGET_DEF )
                hint = "light definition asset";
            const char *source = present ? currentValue.c_str() : "";
            return DrawTextEditor( &state, source, hint.c_str(), width, outValue );
        }

        if ( parameter.widget == KIWI_ENT_WIDGET_TARGET )
        {
            std::string hint;
            if ( !present && !parameter.defaultValue.empty() )
                hint = std::string( "default: " ) + parameter.defaultValue;
            else
                hint = "targetname / target";
            const char *source = present ? currentValue.c_str() : "";
            return DrawTargetEditor( &state, source, hint.c_str(), suggestions, width, outValue );
        }

        const bool dimDefault = !present;
        if ( dimDefault )
            ImGui::PushStyleVar( ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.60f );

        // KIWI: epairs have no undo-free preview path, so drags stay local and commit on release.
        bool commit = false;
        if ( parameter.widget == KIWI_ENT_WIDGET_COLOR3 )
        {
            float parsed[3] = { 0.0f, 0.0f, 0.0f };
            if ( present && !ParseFloats( displayValue, parsed, 3 ) )
            {
                if ( dimDefault )
                    ImGui::PopStyleVar();
                return DrawTextEditor( &state, currentValue.c_str(), "invalid colour: edit raw value",
                                       width, outValue );
            }
            if ( !state.wasActive )
            {
                if ( !ParseFloats( displayValue, parsed, 3 ) )
                    parsed[0] = parsed[1] = parsed[2] = 0.0f;
                state.floatValue[0] = parsed[0];
                state.floatValue[1] = parsed[1];
                state.floatValue[2] = parsed[2];
            }
            ImGui::SetNextItemWidth( width );
            ImGui::ColorEdit3( "##value", state.floatValue,
                               ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB );
            commit = ImGui::IsItemDeactivatedAfterEdit();
            state.wasActive = ImGui::IsItemActive();
            if ( commit )
            {
                char value[96];
                FormatVec3Epair( value, sizeof( value ), state.floatValue );
                *outValue = value;
            }
        }
        else if ( parameter.widget == KIWI_ENT_WIDGET_VEC3 )
        {
            float parsed[3] = { 0.0f, 0.0f, 0.0f };
            if ( present && !ParseFloats( displayValue, parsed, 3 ) )
            {
                if ( dimDefault )
                    ImGui::PopStyleVar();
                return DrawTextEditor( &state, currentValue.c_str(), "invalid vector: edit raw value",
                                       width, outValue );
            }
            if ( !state.wasActive )
            {
                if ( !ParseFloats( displayValue, parsed, 3 ) )
                    parsed[0] = parsed[1] = parsed[2] = 0.0f;
                state.floatValue[0] = parsed[0];
                state.floatValue[1] = parsed[1];
                state.floatValue[2] = parsed[2];
            }
            ImGui::SetNextItemWidth( width );
            ImGui::DragFloat3( "##value", state.floatValue, parameter.dragSpeed,
                               parameter.hasDragRange ? parameter.dragMin : 0.0f,
                               parameter.hasDragRange ? parameter.dragMax : 0.0f,
                               KIWI_FMT_FLOAT );
            commit = ImGui::IsItemDeactivatedAfterEdit();
            state.wasActive = ImGui::IsItemActive();
            if ( commit )
            {
                char value[96];
                FormatVec3Epair( value, sizeof( value ), state.floatValue );
                *outValue = value;
            }
        }
        else if ( parameter.widget == KIWI_ENT_WIDGET_INT )
        {
            int parsed = 0;
            if ( present && !ParseInteger( displayValue, &parsed ) )
            {
                if ( dimDefault )
                    ImGui::PopStyleVar();
                return DrawTextEditor( &state, currentValue.c_str(), "invalid integer: edit raw value",
                                       width, outValue );
            }
            if ( !state.wasActive )
            {
                if ( !ParseInteger( displayValue, &parsed ) )
                    parsed = 0;
                state.integerValue = parsed;
            }
            ImGui::SetNextItemWidth( width );
            ImGui::DragInt( "##value", &state.integerValue, parameter.dragSpeed,
                            parameter.hasDragRange ? (int)parameter.dragMin : 0,
                            parameter.hasDragRange ? (int)parameter.dragMax : 0 );
            commit = ImGui::IsItemDeactivatedAfterEdit();
            state.wasActive = ImGui::IsItemActive();
            if ( commit )
            {
                char value[32];
                _snprintf( value, sizeof( value ), "%i", state.integerValue );
                value[sizeof( value ) - 1] = '\0';
                *outValue = value;
            }
        }
        else if ( parameter.widget == KIWI_ENT_WIDGET_BOOL )
        {
            bool parsed = false;
            if ( present && !ParseBool( displayValue, &parsed ) )
            {
                if ( dimDefault )
                    ImGui::PopStyleVar();
                return DrawTextEditor( &state, currentValue.c_str(), "invalid boolean: edit raw value",
                                       width, outValue );
            }
            if ( !ParseBool( displayValue, &parsed ) )
                parsed = false;
            state.boolValue = parsed;
            if ( ImGui::Checkbox( "##value", &state.boolValue ) )
            {
                *outValue = state.boolValue ? "1" : "0";
                commit = true;
            }
            state.wasActive = false;
        }
        else
        {
            float parsed = 0.0f;
            if ( present && !ParseFloats( displayValue, &parsed, 1 ) )
            {
                if ( dimDefault )
                    ImGui::PopStyleVar();
                return DrawTextEditor( &state, currentValue.c_str(), "invalid number: edit raw value",
                                       width, outValue );
            }
            if ( !state.wasActive )
            {
                if ( !ParseFloats( displayValue, &parsed, 1 ) )
                    parsed = 0.0f;
                state.floatValue[0] = parsed;
            }
            ImGui::SetNextItemWidth( width );
            ImGui::DragFloat( "##value", &state.floatValue[0], parameter.dragSpeed,
                              parameter.hasDragRange ? parameter.dragMin : 0.0f,
                              parameter.hasDragRange ? parameter.dragMax : 0.0f,
                              KIWI_FMT_FLOAT );
            commit = ImGui::IsItemDeactivatedAfterEdit();
            state.wasActive = ImGui::IsItemActive();
            if ( commit )
            {
                char value[64];
                KiwiFmt_Num( value, sizeof( value ), state.floatValue[0], 6 );
                *outValue = value;
            }
        }

        if ( dimDefault )
            ImGui::PopStyleVar();
        return commit;
    }

    bool DrawResetButton( bool visible )
    {
        if ( !visible )
            return false;
        const bool reset = ImGui::SmallButton( "\xC3\x97" );
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Remove this key and use the engine default." );
        return reset;
    }

    void DrawEditorAndReset( const KiwiEntParameter &parameter, bool present, bool mixed,
                             const std::string &currentValue, const char *writeKey,
                             const std::vector<std::string> &targetSuggestions )
    {
        const bool showReset = present || mixed;
        float editorWidth = ImGui::GetContentRegionAvail().x;
        if ( showReset )
            editorWidth -= ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
        if ( editorWidth < 40.0f )
            editorWidth = 40.0f;

        std::string newValue;
        const bool commit = DrawTypedEditor( parameter, present, currentValue,
                                              targetSuggestions, editorWidth, &newValue );
        bool reset = false;
        if ( showReset )
        {
            ImGui::SameLine();
            reset = DrawResetButton( true );
        }

        if ( reset )
            DeleteEntityKey( writeKey );
        else if ( commit )
            SetEntityKey( writeKey, newValue.c_str() );
    }

    void DrawProperties( const std::vector<KiwiEntParameter> &properties,
                         const std::vector<std::string> &targetSuggestions,
                         const std::vector<entity_s_def *> &selectedEntities )
    {
        ImGui::SeparatorText( "Properties" );
        if ( properties.empty() )
            return;

        const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        if ( !ImGui::BeginTable( "##properties", 2, flags ) )
            return;
        ImGui::TableSetupColumn( "Property", ImGuiTableColumnFlags_WidthFixed, 135.0f );
        ImGui::TableSetupColumn( "Value", ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableHeadersRow();

        for ( size_t i = 0; i < properties.size(); ++i )
        {
            const KiwiEntParameter &parameter = properties[i];
            epair_t *epair = FindEpair( edit_entity, parameter.name.c_str() );
            const bool present = epair != nullptr;
            const std::string currentValue = present ? epair->value : "";
            const std::string writeKey = present ? epair->key : parameter.name;
            const bool mixed = IsMixedValue( selectedEntities, parameter.name.c_str() );

            ImGui::PushID( parameter.name.c_str() );
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex( 0 );
            DrawParameterLabel( parameter, mixed );

            ImGui::TableSetColumnIndex( 1 );
            DrawEditorAndReset( parameter, present, mixed, currentValue, writeKey.c_str(),
                                targetSuggestions );
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void GatherOtherKeys( const std::vector<KiwiEntParameter> &properties,
                          std::vector<OtherKeyRow> *rows )
    {
        rows->clear();
        if ( !edit_entity )
            return;
        for ( epair_t *ep = edit_entity->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, "classname" ) || !_stricmp( ep->key, "spawnflags" ) )
                continue;
            if ( FindEpair( edit_entity, ep->key ) != ep )
                continue;
            if ( ParameterExists( properties, ep->key ) )
                continue;
            if ( IsWorldspawn( edit_entity ) && IsSunTabKey( ep->key ) )
                continue;
            if ( IsLightClass( edit_entity ) && IsLightTabKey( ep->key ) )
                continue;
            OtherKeyRow row;
            row.key = ep->key;
            row.value = ep->value;
            rows->push_back( row );
        }
    }

    void DrawOtherKeys( const std::vector<KiwiEntParameter> &properties,
                        const std::vector<std::string> &targetSuggestions,
                        const std::vector<entity_s_def *> &selectedEntities )
    {
        std::vector<OtherKeyRow> rows;
        GatherOtherKeys( properties, &rows );
        if ( rows.empty() )
            return;

        ImGui::SeparatorText( "Other keys" );

        const ImGuiTableFlags flags = ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
        if ( !ImGui::BeginTable( "##otherkeys", 2, flags ) )
            return;
        ImGui::TableSetupColumn( "Key", ImGuiTableColumnFlags_WidthFixed, 135.0f );
        ImGui::TableSetupColumn( "Value", ImGuiTableColumnFlags_WidthStretch );
        ImGui::TableHeadersRow();

        for ( size_t i = 0; i < rows.size(); ++i )
        {
            KiwiEntParameter inferred = KiwiEntInspect_InferParameter( rows[i].key.c_str(), "", "" );
            const bool mixed = IsMixedValue( selectedEntities, rows[i].key.c_str() );

            ImGui::PushID( (int)i );
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex( 0 );
            DrawParameterLabel( inferred, mixed );

            ImGui::TableSetColumnIndex( 1 );
            inferred.name = std::string( "other:" ) + rows[i].key;
            DrawEditorAndReset( inferred, true, mixed, rows[i].value, rows[i].key.c_str(),
                                targetSuggestions );
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    void DrawAddKey()
    {
        if ( !ImGui::CollapsingHeader( "Add key" ) )
            return;

        if ( ImGui::BeginTable( "##addkey", 3, ImGuiTableFlags_SizingStretchProp ) )
        {
            ImGui::TableSetupColumn( "Key", ImGuiTableColumnFlags_WidthStretch, 0.8f );
            ImGui::TableSetupColumn( "Value", ImGuiTableColumnFlags_WidthStretch, 1.2f );
            ImGui::TableSetupColumn( "Set", ImGuiTableColumnFlags_WidthFixed );
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex( 0 );
            ImGui::SetNextItemWidth( -1.0f );
            bool commit = ImGui::InputTextWithHint( "##key", "key", s_key, sizeof( s_key ),
                                                     ImGuiInputTextFlags_EnterReturnsTrue );
            ImGui::TableSetColumnIndex( 1 );
            ImGui::SetNextItemWidth( -1.0f );
            commit |= ImGui::InputTextWithHint( "##value", "value", s_value, sizeof( s_value ),
                                                ImGuiInputTextFlags_EnterReturnsTrue );
            ImGui::TableSetColumnIndex( 2 );
            if ( ImGui::Button( "Set" ) || commit )
                SetEntityKey( s_key, s_value );
            ImGui::EndTable();
        }
    }

    bool IsNamedSpawnFlag( const char *name )
    {
        return name && name[0] && _stricmp( name, "x" ) && strcmp( name, "-" );
    }

    void DrawSpawnFlags( eclass_t *eclass )
    {
        if ( !eclass )
            return;
        const char *const names[9] =
        {
            eclass->flagname0, eclass->flagname1, eclass->flagname2,
            eclass->flagname3, eclass->flagname4, eclass->flagname5,
            eclass->flagname6, eclass->flagname7, eclass->flagname8
        };
        int namedCount = 0;
        for ( int i = 0; i < 9; ++i )
            if ( IsNamedSpawnFlag( names[i] ) )
                ++namedCount;
        if ( namedCount == 0 )
            return;

        ImGui::SeparatorText( "Spawnflags" );
        const int originalFlags = SpawnFlags_Gather();
        int editedFlags = originalFlags;
        bool changed = false;
        int changedBit = -1;
        bool changedChecked = false;
        if ( ImGui::BeginTable( "##spawnflags", 2, ImGuiTableFlags_SizingStretchSame ) )
        {
            int slot = 0;
            for ( int i = 0; i < 9; ++i )
            {
                if ( !IsNamedSpawnFlag( names[i] ) )
                    continue;
                if ( ( slot & 1 ) == 0 )
                    ImGui::TableNextRow();
                ImGui::TableSetColumnIndex( slot & 1 );
                bool checked = ( editedFlags & ( 1 << i ) ) != 0;
                ImGui::PushID( i );
                if ( ImGui::Checkbox( names[i], &checked ) )
                {
                    if ( checked )
                        editedFlags |= 1 << i;
                    else
                        editedFlags &= ~( 1 << i );
                    changed = true;
                    changedBit = i;
                    changedChecked = checked;
                }
                ImGui::PopID();
                ++slot;
            }
            ImGui::EndTable();
        }

        if ( changed )
        {
            if ( multiple_edit_entities && changedBit >= 0 )
            {
                SpawnFlagBit_Apply( changedBit, changedChecked ? 1 : 0 );
                MarkEntityWrite();
            }
            else
            {
                SetEntitySpawnFlags( editedFlags );
            }
        }
    }

    void DrawEclassList( std::vector<eclassRow_t> &rows )
    {
        ImGui::SetNextItemWidth( -1.0f );
        ImGui::InputTextWithHint( "##eclassfilter", "filter...", s_eclassFilter,
                                  sizeof( s_eclassFilter ) );
        if ( ImGui::BeginChild( "##eclasslist", ImVec2( 0.0f, 250.0f ), ImGuiChildFlags_Borders ) )
        {
            for ( size_t i = 0; i < rows.size(); ++i )
            {
                if ( !ContainsNoCase( rows[i].name, s_eclassFilter ) )
                    continue;
                ImGui::PushID( (int)i );
                if ( ImGui::Selectable( rows[i].name ? rows[i].name : "(unnamed)",
                                        s_selEclass == (int)i,
                                        ImGuiSelectableFlags_AllowDoubleClick ) )
                {
                    s_selEclass = (int)i;
                    EclassSelect_Apply( (int)i, rows[i].eclass );
                    if ( ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left ) )
                    {
                        EclassCreate_Apply( rows[i].name );
                        ImGui::CloseCurrentPopup();
                    }
                }
                if ( ImGui::IsItemHovered() && rows[i].eclass && rows[i].eclass->comments &&
                     rows[i].eclass->comments[0] )
                    DrawWrappedTooltip( rows[i].eclass->comments );
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::TextDisabled( "single click: inspect class   double click: create / change entity" );
    }

    void DrawChangeClass( std::vector<eclassRow_t> &rows )
    {
        const bool canChange = edit_entity && !IsWorldspawn( edit_entity );
        ImGui::BeginDisabled( !canChange );
        if ( ImGui::SmallButton( "Change class \xE2\x96\xBE" ) )
            ImGui::OpenPopup( "##changeclass" );
        ImGui::EndDisabled();

        ImGui::SetNextWindowSize( ImVec2( 420.0f, 340.0f ), ImGuiCond_Appearing );
        if ( ImGui::BeginPopup( "##changeclass" ) )
        {
            if ( ImGui::IsWindowAppearing() )
                ImGui::SetKeyboardFocusHere();
            DrawEclassList( rows );
            ImGui::EndPopup();
        }
    }

    void DrawEntityHeader( std::vector<eclassRow_t> &eclasses,
                           const std::vector<entity_s_def *> &selectedEntities )
    {
        if ( !edit_entity )
            return;

        const char *classname = EntityClassname( edit_entity );
        ImGui::SetWindowFontScale( 1.25f );
        ImGui::TextUnformatted( classname ? classname : "(unknown class)" );
        const bool classHovered = ImGui::IsItemHovered();
        ImGui::SetWindowFontScale( 1.0f );
        if ( edit_entity->eclass && edit_entity->eclass->comments &&
             edit_entity->eclass->comments[0] && classHovered )
            DrawWrappedTooltip( edit_entity->eclass->comments );

        if ( selectedEntities.size() > 1 )
            ImGui::TextDisabled( "(%u entities selected)", (unsigned)selectedEntities.size() );
        if ( edit_entity->eclass && !edit_entity->eclass->fixedsize )
        {
            const int count = BrushCount( edit_entity );
            ImGui::TextDisabled( "%d brush%s", count, count == 1 ? "" : "es" );
        }
        DrawChangeClass( eclasses );
    }

    void DrawOwnedSettingsLink()
    {
        if ( IsWorldspawn( edit_entity ) )
        {
            if ( ImGui::SmallButton( "Sun settings \xE2\x86\x92 Sun tab" ) )
            {
                KiwiWindows_Set( KIWI_WIN_SUN, true );
                ImGuiShell_FocusTab( "Sun" );
            }
        }
        else if ( IsLightClass( edit_entity ) )
        {
            if ( ImGui::SmallButton( "Light settings \xE2\x86\x92 Light tab" ) )
            {
                KiwiWindows_Set( KIWI_WIN_LIGHT, true );
                ImGuiShell_FocusTab( "Light" );
            }
        }
    }

    void UpdateSelectionAndAutoFocus()
    {
        Entity_UpdateSelection();
        const bool selectionEmpty = SelectionIsEmpty();
        if ( selectionEmpty != s_previousSelectionEmpty )
        {
            s_editWorldspawn = false;
            s_previousSelectionEmpty = selectionEmpty;
        }
        if ( edit_entity == s_previousEntity )
            return;

        s_editStates.clear();
        s_editStateEntity = edit_entity;
        if ( selectionEmpty )
            s_editWorldspawn = false;

        const char *classname = EntityClassname( edit_entity );
        if ( edit_entity && !selectionEmpty && classname &&
             _stricmp( classname, "worldspawn" ) && !StartsNoCase( classname, "light" ) )
        {
            KiwiWindows_Set( KIWI_WIN_INSPECTOR, true );
            ImGuiShell_FocusTab( "Inspector" );
        }
        s_previousEntity = edit_entity;
    }
}

void ImGuiPanel_Entity_MenuItem()
{
    bool open = KiwiWindows_IsOpen( KIWI_WIN_INSPECTOR );
    if ( ImGui::Checkbox( "Inspector", &open ) )
        KiwiWindows_Set( KIWI_WIN_INSPECTOR, open );
}

void ImGuiPanel_Entity_Toggle()
{
    if ( KiwiWindows_IsOpen( KIWI_WIN_INSPECTOR ) && s_entityFocused )
    {
        KiwiWindows_Set( KIWI_WIN_INSPECTOR, false );
        return;
    }
    KiwiWindows_Set( KIWI_WIN_INSPECTOR, true );
    ImGuiShell_FocusTab( "Inspector" );
}

void ImGuiPanel_Entity_Draw()
{
    s_entityFocused = false;
    UpdateSelectionAndAutoFocus();
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_INSPECTOR );
    if ( !open || !*open )
        return;

    if ( KiwiWindows_JustOpened( KIWI_WIN_INSPECTOR ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );

    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_INSPECTOR ), open ) )
    {
        s_entityFocused = ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows );

        bool inspectEntity = !SelectionIsEmpty() || s_editWorldspawn;
        if ( !inspectEntity )
        {
            ImGui::TextUnformatted( "Nothing selected" );
            const bool haveWorldspawn = world_entity && world_entity->def;
            ImGui::BeginDisabled( !haveWorldspawn );
            if ( ImGui::Button( "Edit worldspawn" ) )
            {
                edit_entity = (entity_s_def *)world_entity->def;
                s_editWorldspawn = true;
                s_editStates.clear();
                s_editStateEntity = edit_entity;
                inspectEntity = true;
            }
            ImGui::EndDisabled();
        }

        if ( inspectEntity && edit_entity )
        {
            std::vector<eclassRow_t> eclasses;
            EclassList_Gather( eclasses );
            if ( s_selEclass >= (int)eclasses.size() )
                s_selEclass = -1;

            std::vector<entity_s_def *> selectedEntities;
            GatherSelectedEntities( &selectedEntities );
            DrawEntityHeader( eclasses, selectedEntities );
            // KIWI FIX: DrawEntityHeader -> DrawChangeClass -> EclassCreate_Apply
            // (win_ent.cpp:572) runs Entity_Create, which reparents the selected brushes
            // into a NEW entity and free()s every owner entity it empties — including the
            // defs already sitting in selectedEntities.  The rest of this frame still
            // hands that vector to IsMixedValue -> FindEpair, which walks ->epairs of a
            // freed entity_s_def.  Re-gather from the (re-selected) live brush list, and
            // re-check edit_entity, before anything downstream dereferences them.
            GatherSelectedEntities( &selectedEntities );
            if ( !edit_entity )
            {
                ImGui::End();
                return;
            }
            DrawOwnedSettingsLink();

            const KiwiEntSchema &schema = KiwiEntInspect_GetSchema(
                edit_entity->eclass, eclasses.size() );
            std::vector<KiwiEntParameter> properties;
            BuildProperties( edit_entity, schema, &properties );
            std::vector<std::string> targetSuggestions;
            GatherTargetSuggestions( &targetSuggestions );

            DrawProperties( properties, targetSuggestions, selectedEntities );
            if ( !IsLightClass( edit_entity ) )
                DrawSpawnFlags( edit_entity->eclass );
            DrawOtherKeys( properties, targetSuggestions, selectedEntities );
            DrawAddKey();
        }
    }
    ImGui::End();
}

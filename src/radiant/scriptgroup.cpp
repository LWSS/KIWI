#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Script-group editor: assigns color, trigger, team, and numbered-group epairs
// to selected entities.

#include "stdafx.h"     // pulls in qe3.h (g_qeglobals / brush lists / Com_Error / ERR_FATAL)
                       //   and res/resource.h (IDD_SCRIPT_GROUP_NAME = 217 / 0xD9)
#include "prefs.h"      // prefData_t / g_PrefsDlg (ScriptGroupKey / ScriptColorTeamKey CStrings)
#include <cstring>
#include <cstdlib>     // atol / _itoa
#include <set>
#include <string>
#include <vector>
#include "kiwi_ported.h"   // KIWI: shared engine/editor externs

// ── externs (per-TU, matching the established pattern in select.cpp/brush.cpp) ──
extern void         ImGuiPanel_ScriptGroup_Toggle();                      // imgui_panel_scriptgroup.cpp:76  void ImGuiPanel_ScriptGroup_Toggle()
// selected_brushes / active_brushes sentinels are declared in qe3.h.

// KIWI: the binary copied every epair value below into char[1024] / [N][16] stack buffers and
// split it with strtok - a long value, a 16+ character token or 512+ kept tokens overran them,
// and the nested strtok calls (SetKey -> ScriptGroup_01 -> HasKey) cut each other's loops short.
// These helpers keep the same strings on std::string.

// strtok( value, " " )'s tokens: runs of spaces split, no empty tokens.
static std::vector<std::string> SG_Tokens( const char *value )
{
    std::vector<std::string> tokens;
    const char *p = value;
    while ( *p )
    {
        while ( *p == ' ' )
            ++p;
        const char *start = p;
        while ( *p && *p != ' ' )
            ++p;
        if ( p > start )
            tokens.emplace_back( start, p );
    }
    return tokens;
}

// Each kept token followed by one space - the trailing space is load-bearing for the .map text.
static std::string SG_Join( const std::vector<std::string> &kept )
{
    std::string out;
    for ( const std::string &token : kept )
    {
        out += token;
        out += ' ';
    }
    return out;
}

// A colour token holds the code ANYWHERE (strstr); its number is read from token + strlen( code ).
static bool SG_HasCode( const std::string &token, const char *code )
{
    return strstr( token.c_str(), code ) != nullptr;
}
static int SG_TokenNumber( const std::string &token, const char *code )
{
    return atol( token.c_str() + strlen( code ) );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x451170  ScriptGroup_Unreachable
// Maps a script-colour code substring to its index 0..6 (r,b,y,c,g,p,o =
// red/blue/yellow/cyan/green/purple/orange).  Returns -1 if none match.
// KIWI: no assert on -1 (the binary's "Unreachable", ScriptGroup.cpp:39) - the tokens are map
// data, the three callers draw every frame and all skip -1.
// ─────────────────────────────────────────────────────────────────────────────
int ScriptGroup_Unreachable( const char *a1 )
{
    static const char *const codes[7] = { "r", "b", "y", "c", "g", "p", "o" };
    for ( int i = 0; i < 7; ++i )
    {
        if ( strstr( a1, codes[i] ) )
            return i;
    }
    return -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  KEY ADD/REMOVE subsystem
// ═════════════════════════════════════════════════════════════════════════════
//
// CoD editor design fact (relied on by every iterator's Assert and by the def
// dereferences): for a placed entity, entity_s and entity_s_def are the SAME struct
// and `def` is self-referential (entity IS its own def), so the
// `b->owner->def == b->def->owner` assert is `entity->def == entity`.  The
// epair list head lives at offset 0x74 on that def (entity_s::epairs).  The asserts
// pass type=0 (engine_stubs Assert only DebugBreak()s on type!=0), so they log-and-
// continue.

// 0x4537A0  ScriptGroup_AddKey — ADD a space-separated value-token to one
// entity's key: an empty key takes the value whole; otherwise, if the value isn't
// already a token, " <value>" is appended.
static void ScriptGroup_AddKey( const char *key, const char *value, entity_s_def *def )
{
    const std::string existing = ValueForKey2( def, key );   // copied: SetKeyValue frees the epair

    if ( existing.empty() )
    {
        Sys_Printf( "Added value %s to key %s\n", value, key );
        SetKeyValue( def, key, value );
        return;
    }

    for ( const std::string &token : SG_Tokens( existing.c_str() ) )
    {
        if ( token == value )
        {
            Sys_Printf( "Entity already has value %s on their key %s\n", value, key );
            return;
        }
    }

    const std::string joined = existing + " " + value;
    SetKeyValue( def, key, joined.c_str() );
    Sys_Printf( "Added value %s to key %s\n", value, key );
}

// 0x453440  ScriptGroup_HasKey — REMOVE a space-separated value-token from one
// entity's key (despite the name it is a REMOVE, not a query).  The kept
// tokens are re-joined each followed by a space (SG_Join); SetKeyValue if any are left,
// else DeleteKey.
static void ScriptGroup_HasKey( const char *value, const char *key, entity_s_def *def )
{
    const char *existing = ValueForKey2( def, key );
    if ( !existing[0] )
    {
        Sys_Printf( "Entity doesn't have key %s\n", key );
        return;
    }

    const std::vector<std::string> tokens = SG_Tokens( existing );
    if ( tokens.empty() )
    {
        Sys_Printf( "Entity doesn't have key %s value %s\n", key, value );
        return;
    }

    std::vector<std::string> kept;
    bool hasKey = false;
    for ( const std::string &token : tokens )
    {
        if ( token == value )
        {
            iassert( !hasKey );
            hasKey = true;
        }
        else
        {
            kept.push_back( token );
        }
    }

    if ( !hasKey )
    {
        Sys_Printf( "Entity doesn't have key %s value %s\n", key, value );
        return;
    }

    const std::string out = SG_Join( kept );
    if ( !out.empty() )
        SetKeyValue( def, key, out.c_str() );
    else
        DeleteKey( &def->epairs, key );
    Sys_Printf( "Removed value %s from key %s\n", value, key );
}

// ── selection iterators ───────────────────────────────────────────────────────
// All four walk selected_brushes, skip world/null-owner brushes, and carry
// the `b->owner->def == b->def->owner` self-ref assert; the *Triggers variants additionally
// gate on the entity being trigger_multiple/trigger_radius.  Each ends with UpdateSelection.

// 0x453A50  ScriptGroup_AddKeyToSelected — add `value` to `key` on every selected entity.
// Note the (value, key) argument order; ScriptGroupAddSubKey_Apply passes ScriptSubValue, ScriptSubKey.
int ScriptGroup_AddKeyToSelected( const char *value, const char *key )
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );
        ScriptGroup_AddKey( key, value, (entity_s_def *)owner->def );
    }
    return UpdateSelection( 0xFFFFFFFF, 0 );
}

// 0x4539A0  ScriptGroup_AddKeyToSelectedTriggers — add only to selected TRIGGER entities.
int ScriptGroup_AddKeyToSelectedTriggers( const char *key, const char *value )
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );
        entity_s_def *def = (entity_s_def *)owner->def;
        if ( Entity_HasEpairMatch( def, "classname", "trigger_multiple" )
          || Entity_HasEpairMatch( def, "classname", "trigger_radius" ) )
            ScriptGroup_AddKey( key, value, def );
    }
    return UpdateSelection( 0xFFFFFFFF, 0 );
}

// 0x453670  ScriptGroup_RemoveKeyFromSelected — remove `value` from `key` on every
// selected entity.  Here the order is (key, value); ScriptGroup_HasKey takes (value, key).
int ScriptGroup_RemoveKeyFromSelected( const char *key, const char *value )
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );
        ScriptGroup_HasKey( value, key, (entity_s_def *)owner->def );
    }
    return UpdateSelection( 0xFFFFFFFF, 0 );
}

// 0x4536F0  ScriptGroup_RemoveKeyFromSelectedTriggers — remove only from selected TRIGGER ents.
int ScriptGroup_RemoveKeyFromSelectedTriggers( const char *key, const char *value )
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );
        entity_s_def *def = (entity_s_def *)owner->def;
        if ( Entity_HasEpairMatch( def, "classname", "trigger_multiple" )
          || Entity_HasEpairMatch( def, "classname", "trigger_radius" ) )
            ScriptGroup_HasKey( value, key, def );
    }
    return UpdateSelection( 0xFFFFFFFF, 0 );
}

// ═════════════════════════════════════════════════════════════════════════════
//  SCRIPT-GROUP NUMBER ASSIGNMENT
// ═════════════════════════════════════════════════════════════════════════════
//
// This is the core both the Script-Group dialog's colour buttons AND the Vehicle
// dialog's 10 script-group buttons drive.  Each button stores a key NAME into
// g_PrefsDlg->ScriptGroupKey (VehicleDlg_SetScriptGroupKey) and then calls
// ScriptGroup_AssignNextNumber, which finds the highest existing group number stored
// under that key across all (non-selected) world entities and assigns max+1 to the
// current selection.
//
// SCOPE NOTE — this is NOT the colour-token machinery (ScriptGroup_RemoveColors[_02]
// / ScriptGroup_Color / the single-char ScriptColorKey "r/b/y/c/g/p/o" codes).  Those
// edit a DIFFERENT key (g_PrefsDlg->ScriptColorTeamKey, a space-separated list of
// <colourCode><number> tokens) and are implemented separately, with the Script-Group
// MFC dialog, further down this file.  AssignNextNumber writes a single SCALAR number under
// ScriptGroupKey via SetKeyValue (wholesale replace), so its inverse is a plain DeleteKey
// of that key (NOT ScriptGroup_RemoveColors, which is the colour-list subsystem).

// ScriptGroup_AssignNextNumber (0x479FF0) lives in brush.cpp (its asserts are
// brush.cpp:4293/4298).
extern void ScriptGroup_AssignNextNumber();   // brush.cpp 0x479FF0

// 0x45FBA0  VehicleDlg_SetScriptGroupKey — the Vehicle dialog's script-group button
// entry: store the chosen key NAME into g_PrefsDlg->ScriptGroupKey, then assign the next
// group number to the selection.  (The EndDialog/SetFocus/g_nUpdateBits tail is the
// modeless-dialog "applied, return focus" gesture, supplied by the caller in the UI.)
// vehicledlg.cpp's buttons call it.
void VehicleDlg_SetScriptGroupKey( const char *key )
{
    g_PrefsDlg->ScriptGroupKey = ( key ? key : "" );
    ScriptGroup_AssignNextNumber();
}

// ═════════════════════════════════════════════════════════════════════════════
//  COLOUR-GROUP / TRIGGER-NUMBER machinery (ScriptGroup_Type)
// ═════════════════════════════════════════════════════════════════════════════
#define MAX_COLORGROUPS 512

// 0x451200  ScriptGroup_Type  (ScriptGroup.cpp:62)
// Grow the selection by trigger COLOUR GROUP: gather the colour-group numbers of every
// selected trigger_multiple/trigger_radius entity (parsed from the ScriptColorTeamKey
// epair's ScriptColorKey-prefixed token), then select every active trigger whose own
// colour-group number is one of them.  Called by SelectedAssociated when
// ScriptGroupKey == ScriptColorTeamKey.  The active-brush walk saves the next node
// before Brush_RemoveFromList/AddToList2 relink the current one.
void ScriptGroup_Type()
{
    const char *scriptColorTeamKey = g_PrefsDlg->ScriptColorTeamKey.c_str();
    const char *scriptColorKey     = g_PrefsDlg->ScriptColorKey.c_str();

    std::vector<int> colorGroups;

    // Pass 1 — collect each selected trigger entity's colour-group number.
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;

        iassert( b->owner->def == b->def->owner );

        entity_s_def *def = (entity_s_def *)owner->def;
        if ( !Entity_HasEpairMatch( def, "classname", "trigger_multiple" )
             && !Entity_HasEpairMatch( def, "classname", "trigger_radius" ) )
            continue;

        const char *value = ValueForKey2( def, scriptColorTeamKey );
        if ( !value[0] || !strstr( value, scriptColorKey ) )
            continue;

        // The single ScriptColorKey token's number.
        int triggerNumber = -1;
        for ( const std::string &token : SG_Tokens( value ) )
        {
            if ( SG_HasCode( token, scriptColorKey ) )
            {
                vassert( (triggerNumber == -1), "(token) = %s", token.c_str() );   // ScriptGroup.cpp:98
                triggerNumber = SG_TokenNumber( token, scriptColorKey );
            }
        }
        if ( triggerNumber != -1 )
            iassert( triggerNumber < MAX_COLORGROUPS );
        else
            iassert( triggerNumber != -1 );

        colorGroups.push_back( triggerNumber );
    }

    // Pass 2 — select every active entity whose colour-group number is in the set.
    for ( selbrush_t *bAll = active_brushes.next; bAll != &active_brushes; )
    {
        selbrush_t *next = bAll->next;   // captured before Brush_RemoveFromList/AddToList2 relink
        entity_s *owner = bAll->owner;
        if ( owner && owner != world_entity )
        {
            iassert( bAll->owner->def == bAll->def->owner );

            entity_s_def *def = (entity_s_def *)bAll->owner->def;
            const char *value = ValueForKey2( def, scriptColorTeamKey );
            if ( value[0] && strstr( value, scriptColorKey ) )
            {
                for ( const std::string &token : SG_Tokens( value ) )
                {
                    if ( !SG_HasCode( token, scriptColorKey ) )
                        continue;
                    const int triggerNumber = SG_TokenNumber( token, scriptColorKey );
                    for ( int group : colorGroups )
                    {
                        if ( group == triggerNumber )
                        {
                            Brush_RemoveFromList( bAll );
                            Brush_AddToList2( bAll );
                            break;
                        }
                    }
                }
            }
        }
        bAll = next;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  COLOUR-GROUP / TRIGGER-NUMBER cluster
// ═════════════════════════════════════════════════════════════════════════════
//
// CONVENTIONS used throughout:
//  * selbrush_t (display-list node): owner@0x08, def@0x14.  The self-ref invariant
//    `b->owner->def == b->def->owner` (= entity->def == entity) is asserted throughout.
//  * the colour-team value is a space-separated list of "<colourCode><number>" tokens
//    stored under the epair key g_PrefsDlg->ScriptColorTeamKey; the single-char colour
//    code is g_PrefsDlg->ScriptColorKey.  An absent key reads as "" (ValueForKey2).
//  * values are split with SG_Tokens and kept tokens re-joined with SG_Join (each followed
//    by a space).

// ─────────────────────────────────────────────────────────────────────────────
// 0x453fd0  ScriptGroup_BrushIsTrigger (ScriptGroup.cpp:1150) — true if the brush's
// entity is trigger_multiple/trigger_radius.  Exported (non-static) so the
// render-decoration draws (brush.cpp DrawModels_Decorations / camwnd.cpp Cam_DrawTokens)
// can gate on it.
// ─────────────────────────────────────────────────────────────────────────────
bool ScriptGroup_BrushIsTrigger( selbrush_t *b )
{
    entity_s *owner = b->owner;
    if ( !owner || owner == world_entity )
        return false;
    iassert( b->owner->def == b->def->owner );
    entity_s_def *def = (entity_s_def *)owner->def;
    return Entity_HasEpairMatch( def, "classname", "trigger_multiple" )
        || Entity_HasEpairMatch( def, "classname", "trigger_radius" );
}

// 0x454260  ScriptGroup_IsValidColorCode — true if the current
// ScriptColorKey exactly matches one of the 7 single-char codes {r,b,y,c,g,p,o}.
static bool ScriptGroup_IsValidColorCode()
{
    static const char *const codes[7] = { "r", "b", "y", "c", "g", "p", "o" };
    const char *key = g_PrefsDlg->ScriptColorKey.c_str();
    for ( int i = 0; i < 7; ++i )
    {
        if ( !strcmp( key, codes[i] ) )
            return true;
    }
    return false;
}

// 0x451a40  ScriptGroup_TeamKeyContains — true if the entity DEF
// `def`'s ScriptColorTeamKey value contains the substring `code`.
static bool ScriptGroup_TeamKeyContains( const char *code, entity_s_def *def )
{
    const char *value = ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() );
    return value[0] && strstr( value, code ) != nullptr;
}

// 0x4540b0  ScriptGroup_TriggerColorNumber — read the FIRST
// ScriptColorKey-prefixed token's number from the brush entity's ScriptColorTeamKey
// value, or -1 if absent.  (Single-match: no `triggerNumber==-1` duplicate assert.)
static int ScriptGroup_TriggerColorNumber( selbrush_t *b )
{
    entity_s_def *def = (entity_s_def *)b->owner->def;
    const char *scriptColorKey = g_PrefsDlg->ScriptColorKey.c_str();

    const char *value = ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() );
    if ( !value[0] || !strstr( value, scriptColorKey ) )
        return -1;

    for ( const std::string &token : SG_Tokens( value ) )
    {
        if ( SG_HasCode( token, scriptColorKey ) )
            return SG_TokenNumber( token, scriptColorKey );
    }
    return -1;
}

// 0x454220  ScriptGroup_SelectionTriggerNumber — the LAST
// trigger-colour number across the selected trigger brushes (or -1).
static int ScriptGroup_SelectionTriggerNumber()
{
    int n = -1;
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( ScriptGroup_BrushIsTrigger( b ) )
            n = ScriptGroup_TriggerColorNumber( b );
    }
    return n;
}

// 0x453e50  ScriptGroup_SelectionHasTrigger — true if any selected entity is a
// trigger_multiple/trigger_radius.  Non-static — OnScriptGroup_Disassociate calls it.
bool ScriptGroup_SelectionHasTrigger()
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( ScriptGroup_BrushIsTrigger( b ) )
            return true;
    }
    return false;
}

// 0x452a50  ScriptGroup_SelectedHasColorTeam — true if the brush's NON-trigger entity
// (AI / node / goal volume) holds the substring `code` in its ScriptColorTeamKey value.
// (Returns false for trigger entities — the trigger side is handled separately.)
static bool ScriptGroup_SelectedHasColorTeam( selbrush_t *b, const char *code )
{
    entity_s *owner = b->owner;
    if ( !owner || owner == world_entity )
        return false;
    iassert( b->owner->def == b->def->owner );
    entity_s_def *def = (entity_s_def *)owner->def;
    if ( Entity_HasEpairMatch( def, "classname", "trigger_multiple" ) )
        return false;
    if ( Entity_HasEpairMatch( def, "classname", "trigger_radius" ) )
        return false;

    const char *value = ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() );
    return value[0] && strstr( value, code ) != nullptr;
}

// 0x451650  ScriptGroup_Color — pick the FIRST free colour-group number (0..511) not
// currently used by any active OR selected entity.  Builds a used[512] bitmap from every
// ScriptColorKey token's number, then returns the first index where used==0.
// KIWI: a number outside 0..511 (typed or map-loaded) is skipped - it wrote past used[].
static int ScriptGroup_Color()
{
    char used[MAX_COLORGROUPS];
    memset( used, 0, sizeof( used ) );
    const char *scriptColorKey = g_PrefsDlg->ScriptColorKey.c_str();

    // Pass 1 — active brushes, pass 2 — selected brushes.
    selbrush_t *const lists[2] = { &active_brushes, &selected_brushes };
    for ( selbrush_t *list : lists )
    {
        for ( selbrush_t *b = list->next; b != list; b = b->next )
        {
            entity_s *owner = b->owner;
            if ( !owner || owner == world_entity )
                continue;
            iassert( b->owner->def == b->def->owner );

            entity_s_def *def = (entity_s_def *)owner->def;
            const char *value = ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() );
            if ( !value[0] || !strstr( value, scriptColorKey ) )
                continue;
            for ( const std::string &token : SG_Tokens( value ) )
            {
                if ( !SG_HasCode( token, scriptColorKey ) )
                    continue;
                const int n = SG_TokenNumber( token, scriptColorKey );
                if ( n >= 0 && n < MAX_COLORGROUPS )
                    used[n] = 1;
            }
        }
    }

    // First free index; all 512 taken asserts "unreachable".
    int result = 0;
    while ( used[result] )
    {
        if ( ++result >= MAX_COLORGROUPS )
        {
            iassert( !"unreachable" );   // ScriptGroup.cpp:249
            return 0;
        }
    }
    return result;
}

// ── helpers used by ScriptGroup_SetKey2 / RemoveColors / RemoveColors_02 ──
// `def`'s ScriptColorTeamKey value without the `code` tokens numbered `colorNumber`, the
// others re-joined (SG_Join); written back with SetKeyValue, or DeleteKey when none are left.
static std::string ScriptGroup_StripColor( entity_s_def *def, const char *code, int colorNumber )
{
    const char *teamKey = g_PrefsDlg->ScriptColorTeamKey.c_str();
    std::vector<std::string> kept;
    for ( const std::string &token : SG_Tokens( ValueForKey2( def, teamKey ) ) )
    {
        if ( !SG_HasCode( token, code ) || SG_TokenNumber( token, code ) != colorNumber )
            kept.push_back( token );
    }
    const std::string out = SG_Join( kept );
    if ( !out.empty() )
        SetKeyValue( def, teamKey, out.c_str() );
    else
        DeleteKey( &def->epairs, teamKey );
    return out;
}

// 0x4520c0  ScriptGroup_SetKey2 (ScriptGroup.cpp:401) — remove the colour token whose
// number == `colorNumber` from ONE brush entity's ScriptColorTeamKey value (keep all
// other tokens, re-joined with trailing spaces).
static void ScriptGroup_SetKey2( selbrush_t *b, int colorNumber )
{
    entity_s *owner = b->owner;
    if ( !owner || owner == world_entity )
        return;
    iassert( b->owner->def == b->def->owner );
    entity_s_def *def = (entity_s_def *)owner->def;

    const char *scriptColorKey = g_PrefsDlg->ScriptColorKey.c_str();
    const char *value = ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() );
    if ( !value[0] || !strstr( value, scriptColorKey ) )
        return;

    const std::string out = ScriptGroup_StripColor( def, scriptColorKey, colorNumber );
    Sys_Printf( "Set entity key value to %s\n", out.c_str() );
}

// 0x452330  ScriptGroup_RemoveUnused (ScriptGroup.cpp:~447) — strip colour group
// `colorNumber` from EVERY selected then EVERY active brush entity (via SetKey2).
static void ScriptGroup_RemoveUnused( int colorNumber )
{
    Sys_Printf( "Removing unused %s %i from all entities:\n",
                g_PrefsDlg->ScriptColorKey.c_str(), colorNumber );
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        ScriptGroup_SetKey2( b, colorNumber );
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
        ScriptGroup_SetKey2( b, colorNumber );
}

// 0x452b50  ScriptGroup_RemoveUnusedIfOrphaned — remove colour
// group `colorNumber` everywhere ONLY IF no non-trigger (AI/node) entity still references
// it (scan selected then active brushes for the "<code><n> " token; bail if found).
static bool ScriptGroup_RemoveUnusedIfOrphaned( int colorNumber )
{
    const std::string token = va( "%s%i ", g_PrefsDlg->ScriptColorKey.c_str(), colorNumber );

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( ScriptGroup_SelectedHasColorTeam( b, token.c_str() ) )
            return true;       // still in use → keep
    }
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
    {
        if ( ScriptGroup_SelectedHasColorTeam( b, token.c_str() ) )
            return true;
    }
    ScriptGroup_RemoveUnused( colorNumber );
    return false;
}

// 0x4523a0  ScriptGroup_RemoveColors (ScriptGroup.cpp:~478) — strip colour token
// (code `colorCode`, number `colorNumber`) from every selected NON-trigger (AI/node)
// entity.  Returns true if any entity was modified.
static bool ScriptGroup_RemoveColors( int colorNumber, const char *colorCode )
{
    bool modified = false;
    Sys_Printf( "Removing colorNumber %s %i\n", colorCode, colorNumber );

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );
        entity_s_def *def = (entity_s_def *)owner->def;
        if ( Entity_HasEpairMatch( def, "classname", "trigger_multiple" )
          || Entity_HasEpairMatch( def, "classname", "trigger_radius" ) )
            continue;

        const char *value = ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() );
        if ( !value[0] || !strstr( value, colorCode ) )
            continue;

        const std::string out = ScriptGroup_StripColor( def, colorCode, colorNumber );
        modified = true;
        Sys_Printf( "Set entity key value %s\n", out.c_str() );
    }
    return modified;
}

// 0x452720  ScriptGroup_RemoveColors_02 (ScriptGroup.cpp:~553) — the TRIGGER-entity
// variant of the above (uses g_PrefsDlg->ScriptColorKey directly; no return value).
// Strips colour group `colorNumber` from selected trigger entities.
static void ScriptGroup_RemoveColors_02( int colorNumber )
{
    Sys_Printf( "Removing colorNumber %s %i\n",
                g_PrefsDlg->ScriptColorKey.c_str(), colorNumber );
    const char *scriptColorKey = g_PrefsDlg->ScriptColorKey.c_str();

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );
        entity_s_def *def = (entity_s_def *)owner->def;
        if ( !Entity_HasEpairMatch( def, "classname", "trigger_multiple" )
          && !Entity_HasEpairMatch( def, "classname", "trigger_radius" ) )
            continue;

        const char *value = ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() );
        if ( !value[0] || !strstr( value, scriptColorKey ) )
            continue;

        const std::string out = ScriptGroup_StripColor( def, scriptColorKey, colorNumber );
        Sys_Printf( "Set entity key value %s\n", out.c_str() );
    }
}

// 0x453170  ScriptGroup_TriggerNumber — the DISASSOCIATE core.  For each selected
// trigger's single colour number, try removing it from non-trigger entities; if NONE had
// it (RemoveColors returned false) fall back to removing it from the trigger entities;
// then RemoveUnusedIfOrphaned.
void ScriptGroup_TriggerNumber()
{
    const char *scriptColorKey = g_PrefsDlg->ScriptColorKey.c_str();

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );
        entity_s_def *def = (entity_s_def *)owner->def;
        if ( !Entity_HasEpairMatch( def, "classname", "trigger_multiple" )
          && !Entity_HasEpairMatch( def, "classname", "trigger_radius" ) )
            continue;

        const char *value = ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() );
        if ( !value[0] || !strstr( value, scriptColorKey ) )
            continue;

        int triggerNumber = -1;
        bool hasColor = false;
        for ( const std::string &token : SG_Tokens( value ) )
        {
            if ( SG_HasCode( token, scriptColorKey ) )
            {
                iassert( triggerNumber == -1 );   // scriptgroup.cpp:842
                triggerNumber = SG_TokenNumber( token, scriptColorKey );
                iassert( !hasColor );   // scriptgroup.cpp:846
                hasColor = true;
            }
        }
        if ( triggerNumber != -1 )
            iassert( triggerNumber < MAX_COLORGROUPS );
        else
            iassert( triggerNumber != -1 );

        if ( !ScriptGroup_RemoveColors( triggerNumber, scriptColorKey ) )
            ScriptGroup_RemoveColors_02( triggerNumber );
        ScriptGroup_RemoveUnusedIfOrphaned( triggerNumber );
    }
}

// 0x452c20  ScriptGroup_Trigger (ScriptGroup.cpp:~683) — reconcile a trigger COLOUR
// GROUP toward target `colorNumber`.  Two 512-bitmaps:
//   selSeen — written by PASS 1 (the SELECTED triggers)
//   actSeen — written by PASS 2 (the ACTIVE  triggers)
// PASS 1 marks selSeen[n] for every selected trigger's colour number n != target.
// PASS 2 marks actSeen[n] for every active  trigger's colour number n != target (read-
//   only; the active walk uses the same selbrush_t ->next link as the selected list).
// FINAL: for j in 0..511, if selSeen[j] && j!=target && !actSeen[j] →
//   RemoveUnused(j) — drop a colour group a selected trigger used that no active one does.
// KIWI: a number outside 0..511 (typed or map-loaded) is not marked - it wrote past the maps.
static void ScriptGroup_Trigger( int colorNumber )
{
    const char *scriptColorKey = g_PrefsDlg->ScriptColorKey.c_str();
    char selSeen[MAX_COLORGROUPS];   // PASS 1 (selected)
    char actSeen[MAX_COLORGROUPS];   // PASS 2 (active)
    memset( selSeen, 0, sizeof( selSeen ) );
    memset( actSeen, 0, sizeof( actSeen ) );

    // PASS 1 — selected triggers (single colour number each, asserted).
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );
        entity_s_def *def = (entity_s_def *)owner->def;
        if ( !Entity_HasEpairMatch( def, "classname", "trigger_multiple" )
          && !Entity_HasEpairMatch( def, "classname", "trigger_radius" ) )
            continue;

        const char *value = ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() );
        if ( !value[0] || !strstr( value, scriptColorKey ) )
            continue;

        int triggerNumber = -1;
        for ( const std::string &token : SG_Tokens( value ) )
        {
            if ( SG_HasCode( token, scriptColorKey ) )
            {
                iassert( triggerNumber == -1 );   // scriptgroup.cpp:725
                triggerNumber = SG_TokenNumber( token, scriptColorKey );
            }
        }
        iassert( triggerNumber != -1 );   // scriptgroup.cpp:729
        if ( triggerNumber != colorNumber && triggerNumber >= 0 && triggerNumber < MAX_COLORGROUPS )
            selSeen[triggerNumber] = 1;
    }

    // PASS 2 — active triggers (read-only; same ->next link as the selected list).
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );   // "bAll->owner->def == bAll->def->owner"
        entity_s_def *def = (entity_s_def *)owner->def;
        if ( !Entity_HasEpairMatch( def, "classname", "trigger_multiple" )
          && !Entity_HasEpairMatch( def, "classname", "trigger_radius" ) )
            continue;

        const char *value = ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() );
        if ( !value[0] || !strstr( value, scriptColorKey ) )
            continue;

        int triggerNumber = -1;
        bool hasColor = false;
        for ( const std::string &token : SG_Tokens( value ) )
        {
            if ( SG_HasCode( token, scriptColorKey ) )
            {
                triggerNumber = SG_TokenNumber( token, scriptColorKey );
                if ( triggerNumber == colorNumber )
                    break;                       // target found on an active trigger — stop scanning
                vassert( (!hasColor), "(token) = %s", token.c_str() );   // ScriptGroup.cpp:774
                hasColor = true;
            }
        }
        iassert( triggerNumber != -1 );   // scriptgroup.cpp:778
        if ( triggerNumber != colorNumber && triggerNumber >= 0 && triggerNumber < MAX_COLORGROUPS )
            actSeen[triggerNumber] = 1;
    }

    // FINAL — drop numbers a selected trigger uses (other than target) that no active
    // trigger still uses.
    for ( int j = 0; j < MAX_COLORGROUPS; ++j )
    {
        if ( selSeen[j] && j != colorNumber && !actSeen[j] )
            ScriptGroup_RemoveUnused( j );
    }
}

// 0x453ad0  ScriptGroup_ApplyColorToSelected (ScriptGroup.cpp:~1043) — set the colour
// token to `colorNumber` on every selected trigger entity, REPLACING any existing colour
// token (exactly one allowed: "!hasColor" assert) while keeping non-colour tokens.
static void ScriptGroup_ApplyColorToSelected( int colorNumber )
{
    const char *scriptColorKey = g_PrefsDlg->ScriptColorKey.c_str();

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );
        entity_s_def *def = (entity_s_def *)owner->def;
        if ( !Entity_HasEpairMatch( def, "classname", "trigger_multiple" )
          && !Entity_HasEpairMatch( def, "classname", "trigger_radius" ) )
            continue;

        const char *value = ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() );

        std::string out;
        bool hasColor = false;
        if ( value[0] )
        {
            std::vector<std::string> kept;
            for ( const std::string &token : SG_Tokens( value ) )
            {
                if ( SG_HasCode( token, scriptColorKey ) )
                {
                    iassert( !hasColor );   // scriptgroup.cpp:1076
                    hasColor = true;
                }
                else
                {
                    kept.push_back( token );
                }
            }
            // One extra trailing space after the kept block (the binary's format).
            out = SG_Join( kept ) + " ";
        }
        // Append the new colour token "<code><n> " (trailing space) - never empty.
        out += va( "%s%i ", scriptColorKey, colorNumber );

        SetKeyValue( def, g_PrefsDlg->ScriptColorTeamKey.c_str(), out.c_str() );
        Sys_Printf( "Set trigger key value to %s\n", out.c_str() );
    }
}

// 0x451af0  ScriptGroup_01 (ScriptGroup.cpp:~276) — colour-token reconciliation against
// one node/AI entity DEF (`def`) for colour number `colorNumber`.  For each selected
// trigger whose team key already holds "<code><n>", walk that trigger's team-key tokens
// and, for any token (other than "<code><n>") that `def` ALSO carries, remove it from
// `def`'s team key.  Keeps a node/AI entity from holding colour tokens a selected trigger
// now owns.  (Called once from ScriptGroup_SetKey after the token is written.)
// KIWI: the binary's nested strtok (this loop around HasKey's own) ended the loop after the
// first removal; every shared token goes now.  Its va( "%s%", team key ) key was the bare
// team key (_vsnprintf drops the lone '%'), passed directly here.
static void ScriptGroup_01( entity_s_def *def, int colorNumber )
{
    const char *teamKey = g_PrefsDlg->ScriptColorTeamKey.c_str();
    // colourToken = "<code><n>" (no trailing space here).
    const std::string colourToken = va( "%s%i", g_PrefsDlg->ScriptColorKey.c_str(), colorNumber );

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );
        entity_s_def *trigDef = (entity_s_def *)owner->def;
        if ( !Entity_HasEpairMatch( trigDef, "classname", "trigger_multiple" )
          && !Entity_HasEpairMatch( trigDef, "classname", "trigger_radius" ) )
            continue;

        // Only act when THIS trigger's team key already contains the colour token.
        if ( !ScriptGroup_TeamKeyContains( colourToken.c_str(), trigDef ) )
            continue;

        for ( const std::string &token : SG_Tokens( ValueForKey2( trigDef, teamKey ) ) )
        {
            // skip the colour token itself; remove any other token `def` also carries
            if ( token != colourToken && ScriptGroup_TeamKeyContains( token.c_str(), def ) )
                ScriptGroup_HasKey( token.c_str(), teamKey, def );
        }
    }
}

// 0x451d40  ScriptGroup_SetKey (ScriptGroup.cpp:~328) — write the colour token to every
// selected NODE/AI/info_volume entity's team key (append to existing list, or create it),
// then reconcile via ScriptGroup_01.  Skips entities whose classname isn't actor/node/
// info_volume; a "node" with no radius gets radius=64.
static void ScriptGroup_SetKey( int colorNumber )
{
    const char *scriptColorKey = g_PrefsDlg->ScriptColorKey.c_str();

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );
        entity_s_def *def = (entity_s_def *)owner->def;

        const char *classname = ValueForKey2( def, "classname" );

        // Filter: only actor / node / info_volume entities are eligible.
        if ( !strstr( classname, "actor" ) )
        {
            if ( strstr( classname, "node" ) )
            {
                if ( !ValueForKey2( def, "radius" )[0] )
                    SetKeyValue( def, "radius", "64" );
            }
            else if ( !strstr( classname, "info_volume" ) )
            {
                continue;
            }
        }

        const char *value = ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() );
        if ( value[0] )
        {
            // Append " <code><n> " (leading and trailing space).
            const std::string tempString = std::string( value ) + va( " %s%i ", scriptColorKey, colorNumber );
            SetKeyValue( def, g_PrefsDlg->ScriptColorTeamKey.c_str(), tempString.c_str() );
            Sys_Printf( "Set node/AI key value to %s\n", tempString.c_str() );
            // KIWI: once - the binary re-ran it per token of tempString, but _01's own strtok
            // ended that loop whenever _01 did anything
            ScriptGroup_01( def, colorNumber );
        }
        else
        {
            // Create the value as "<code><n> " (trailing space).
            const std::string tempString = va( "%s%i ", scriptColorKey, colorNumber );
            SetKeyValue( def, g_PrefsDlg->ScriptColorTeamKey.c_str(), tempString.c_str() );
            Sys_Printf( "Set node/AI key value to %s\n", tempString.c_str() );
        }
    }
}

// 0x4543b0  ScriptGroup_AddColorToSelection (ScriptGroup.cpp:~1243) — the top-level
// "add colour group to selection" command.  If the selection has triggers: pick/reconcile
// a colour number, apply it to the triggers, write the team token onto node/AI entities.
// Else if a valid 1-char colour code is selected: tag selected ACTOR entities with
// script_forcecolor.  Else prompt.
void ScriptGroup_AddColorToSelection()
{
    if ( ScriptGroup_SelectionHasTrigger() )
    {
        int triggerNumber = ScriptGroup_SelectionTriggerNumber();
        if ( triggerNumber == -1 )
        {
            triggerNumber = ScriptGroup_Color();
        }
        else
        {
            ScriptGroup_Trigger( triggerNumber );
        }
        iassert( triggerNumber < MAX_COLORGROUPS );   // scriptgroup.cpp:1284
        Sys_Printf( "Adding %s%i to entities:\n",
                    g_PrefsDlg->ScriptColorKey.c_str(), triggerNumber );
        ScriptGroup_ApplyColorToSelected( triggerNumber );
        ScriptGroup_SetKey( triggerNumber );
        UpdateSelection( 0xFFFFFFFF, 0 );
        g_nUpdateBits = -1;
    }
    else if ( ScriptGroup_IsValidColorCode() )
    {
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        {
            entity_s *owner = b->owner;
            if ( !owner || owner == world_entity )
                continue;
            iassert( b->owner->def == b->def->owner );
            entity_s_def *def = (entity_s_def *)owner->def;
            if ( strstr( ValueForKey2( def, "classname" ), "actor" ) )
                SetKeyValue( def, "script_forcecolor", g_PrefsDlg->ScriptColorKey.c_str() );
        }
    }
    else
    {
        Sys_Printf( "Select a color you want to add to the entity\n" );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  SCRIPT-GROUP TOOL (was the modeless MFC dialog on IDD_SCRIPT_GROUP_NAME = 217)
// ═════════════════════════════════════════════════════════════════════════════
//
// The dialog drove the cluster above through numeric control ids; imgui_panel_scriptgroup.cpp
// is the widget layer now and binds to the *_Apply functions below.  The control map the
// DlgProc used, kept for reference:
//   1441 group-key edit        1631 sub-key edit      1635 sub-value edit
//   1636 flag-value edit (true)  1639 flag-value edit (false)
//   1661..1667 colour radios r,b,y,c,g,p,o            1668 allies  1670 axis radio
//   1671 script_flag_true listbox    1298 script_flag_false listbox

// UI-independent read behind the two script-flag lists (ScriptGroup_HasFlag 0x454e40's
// selected-trigger token walk), deduplicated.
// KIWI: a std::set - the binary's seen[256][16] slots and buf[1024] overran on normal 16+
// character flag names and on long values, and this runs every frame while the Script group
// panel is open.
void ScriptGroupFlags_Gather( const char *key, std::vector<std::string> &rows )
{
    std::set<std::string> seen;

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );
        entity_s_def *def = (entity_s_def *)owner->def;
        if ( !Entity_HasEpairMatch( def, "classname", "trigger_multiple" )
          && !Entity_HasEpairMatch( def, "classname", "trigger_radius" ) )
            continue;

        for ( const std::string &token : SG_Tokens( ValueForKey2( def, key ) ) )
        {
            if ( seen.insert( token ).second )   // add only if not already seen
                rows.push_back( token );
        }
    }
}

// 0x454480  ScriptGroup_SyncGroupKeyToTeam — force the active script
// "group key" to be the colour-team key (so the colour-team path owns the selection) and
// save prefs.  The two menu handlers call this BEFORE the cluster ops, to switch the
// system into colour-team mode.  The binary also mirrored the new value into the dialog's
// group-key edit and returned focus to the camera; the panel re-reads the pref instead.
void ScriptGroup_SyncGroupKeyToTeam()
{
    g_PrefsDlg->ScriptGroupKey = g_PrefsDlg->ScriptColorTeamKey;
    Prefs_SavePrefs( g_PrefsDlg );
}

// ── button actions ────────────────────────────────────────────────────────────
// Each took its edit control's text and drove a cluster function; the panel passes the
// same strings straight in.

// UI-independent action behind the Script-Group dialog's OK button — commit the typed group
// key to prefs (empty text is a no-op) and assign the next group number to the selection.
void ScriptGroupKey_Apply( const char *key )
{
    if ( key[0] )
    {
        g_PrefsDlg->ScriptGroupKey = key;
        Prefs_SavePrefs( g_PrefsDlg );
        ScriptGroup_AssignNextNumber();
        UpdateSelection( 0xFFFFFFFF, 0 );
        g_nUpdateBits = -1;
    }
}

// UI-independent action behind the Script-Group dialog's sub-key Add button — store the
// sub key/value to prefs if non-empty, then add that key/value to every selected entity.
void ScriptGroupAddSubKey_Apply( const char *subKey, const char *subValue )
{
    if ( subKey[0] )
    {
        g_PrefsDlg->ScriptSubKey_key = subKey;
        Prefs_SavePrefs( g_PrefsDlg );
    }
    if ( subValue[0] )
    {
        g_PrefsDlg->ScriptSubValue_key = subValue;
        Prefs_SavePrefs( g_PrefsDlg );
    }
    ScriptGroup_AddKeyToSelected( subValue, subKey );   // (value, key)
}

// UI-independent action behind the Script-Group dialog's sub-key Remove button — same prefs
// store, but REMOVE the key/value from every selected entity.
void ScriptGroupRemoveSubKey_Apply( const char *subKey, const char *subValue )
{
    if ( subKey[0] )
    {
        g_PrefsDlg->ScriptSubKey_key = subKey;
        Prefs_SavePrefs( g_PrefsDlg );
    }
    if ( subValue[0] )
    {
        g_PrefsDlg->ScriptSubValue_key = subValue;
        Prefs_SavePrefs( g_PrefsDlg );
    }
    ScriptGroup_RemoveKeyFromSelected( subKey, subValue );   // (key, value)
}

// 0x455b20  ScriptGroupDlg_Disassociate — the dialog's "Disassociate" button (identical to
// CMainFrame::OnScriptGroup_Disassociate).
void ScriptGroupDisassociate_Apply()
{
    ScriptGroup_SyncGroupKeyToTeam();
    if ( !ScriptGroup_SelectionHasTrigger() )
    {
        Sys_Printf( "You must select a trigger_multiple or trigger_radius in combination with the nodes, AI, or goal volumes you with to disassociate.\n" );
        return;
    }
    ScriptGroup_TriggerNumber();
    UpdateSelection( 0xFFFFFFFF, 0 );
    g_nUpdateBits = -1;
}

// UI-independent action behind the Script-Group dialog's axis/allies team radios.
void ScriptGroupTeam_Apply( const char *teamKey )
{
    g_PrefsDlg->ScriptColorTeamKey = teamKey;
    ScriptGroup_SyncGroupKeyToTeam();
    g_nUpdateBits = -1;
    Prefs_SavePrefs( g_PrefsDlg );
}

// UI-independent action behind the Script-Group dialog's 7 colour radios.
void ScriptGroupColorCode_Apply( const char *code )
{
    g_PrefsDlg->ScriptColorKey = code;
    ScriptGroup_SyncGroupKeyToTeam();
    g_nUpdateBits = -1;
    Prefs_SavePrefs( g_PrefsDlg );
}

// ── turret-share button handlers ──────────────────────────────────────────────
// 0x455b60  ScriptGroupDlg_TurretShare — the "Share" turret-key button.
// Sets the group key to "token" + the token key to "script_turret_share", then for EACH
// selected misc_turret entity whose "export" key has atol>0, adds token key + that export
// number to every selected entity through ScriptGroupAddSubKey_Apply (which stores the
// pair to prefs, so the panel's fields end up holding the values).
void ScriptGroupTurretShare_Apply()
{
    std::string exportStr;

    g_PrefsDlg->ScriptGroupKey      = "token";
    g_PrefsDlg->ScriptGroupTokenKey = "script_turret_share";
    const std::string subKey = g_PrefsDlg->ScriptGroupTokenKey;

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s_def *def = (entity_s_def *)b->owner->def;
        if ( strcmp( ValueForKey2( def, "classname" ), "misc_turret" ) )
            continue;

        exportStr = ValueForKey2( def, "export" );
        if ( atol( exportStr.c_str() ) > 0 )
            ScriptGroupAddSubKey_Apply( subKey.c_str(), exportStr.c_str() );
    }

    BrushList_RebuildDisplay( &active_brushes );
    BrushList_RebuildDisplay( &selected_brushes );
    BrushList_RebuildDisplay( &filtered_brushes );
    g_nUpdateBits = -1;
}

// 0x455d80  ScriptGroupDlg_TurretKey — the "ambush"/"share" turret-key rows.
// Sets the group key to "token" + the token key to `turretKey`.  If the FIRST selected
// entity's "export" key has atol>0 (the turret being shared), it gathers the export numbers
// of every OTHER selected misc_turret, re-links the first brush to the FRONT of the
// selection (Select_Deselect(1) + Brush_RemoveFromList/AddToList2), then adds token key +
// each gathered export to every selected entity (via ScriptGroupAddSubKey_Apply, as in
// ScriptGroupTurretShare_Apply above).
void ScriptGroupTurretKey_Apply( const char *turretKey )
{
    std::string exportStr;
    // KIWI: the binary's String[32][16] export table took a strcpy per turret with only an
    // assert on the count - a 16+ character export or 32+ other turrets overran the stack.
    std::vector<std::string> exportList;

    g_PrefsDlg->ScriptGroupKey      = "token";
    g_PrefsDlg->ScriptGroupTokenKey = ( turretKey ? turretKey : "" );           // NULL-safe like MFC's CString=
    const std::string subKey = g_PrefsDlg->ScriptGroupTokenKey;

    selbrush_t *first = selected_brushes.next;
    if ( first == &selected_brushes )
        return;

    // the FIRST selected entity's "export" value.
    entity_s_def *firstDef = (entity_s_def *)first->owner->def;
    if ( atol( ValueForKey2( firstDef, "export" ) ) <= 0 )
        return;

    // gather every OTHER selected misc_turret's export number.
    for ( selbrush_t *b = first->next; b != &selected_brushes; b = b->next )
    {
        entity_s_def *def = (entity_s_def *)b->owner->def;
        if ( strcmp( ValueForKey2( def, "classname" ), "misc_turret" ) )
            continue;

        exportStr = ValueForKey2( def, "export" );
        if ( atol( exportStr.c_str() ) > 0 )
            exportList.push_back( exportStr );
    }

    // re-link the shared turret to the front of the selection, then propagate.
    Select_Deselect( 1 );
    Brush_RemoveFromList( first );
    Brush_AddToList2( first );
    for ( size_t i = 0; i < exportList.size(); ++i )
        ScriptGroupAddSubKey_Apply( subKey.c_str(), exportList[i].c_str() );

    BrushList_RebuildDisplay( &active_brushes );
    BrushList_RebuildDisplay( &selected_brushes );
    BrushList_RebuildDisplay( &filtered_brushes );
    g_nUpdateBits = -1;
}

// UI-independent action behind the dialog's fixed group-key rows (script_health 1442 /
// script_killspawner 0x5A7) — adopt the key, rebuild the brush-list displays, save prefs and
// assign the next group number to the selection.  (The row's EndDialog stays in the handler.)
void ScriptGroupKeyPreset_Apply( const char *key )
{
    g_PrefsDlg->ScriptGroupKey = key;
    BrushList_RebuildDisplay( &active_brushes );
    BrushList_RebuildDisplay( &selected_brushes );
    BrushList_RebuildDisplay( &filtered_brushes );
    Prefs_SavePrefs( g_PrefsDlg );
    ScriptGroup_AssignNextNumber();
    g_nUpdateBits = -1;
    UpdateSelection( 0xFFFFFFFF, 0 );
}

// UI-independent action behind the dialog's fixed sub-key rows (script_objective_active
// 0x669 / script_objective_inactive 0x66A) — adopt the sub key and save prefs.  (The
// widget mirror of the new value stays with the caller.)
void ScriptGroupSubKeyPreset_Apply( const char *subKey )
{
    g_PrefsDlg->ScriptSubKey_key = subKey;
    Prefs_SavePrefs( g_PrefsDlg );
}

// 0x455a80  AssociateEntities — toggle entry for the Script-Group tool (the CMainFrame
// "Associate Entities" menu).  The binary created the modeless dialog on first use and
// toggled its visibility afterwards, seeding the two flag listboxes on the way in; the
// panel gathers those lists itself every frame, so this is just the toggle.
void AssociateEntities()
{
    ImGuiPanel_ScriptGroup_Toggle();
}

// ═════════════════════════════════════════════════════════════════════════════
//  SCRIPT-GROUP single-char TEAM-COLOUR VISUALIZATION (the 2D-view marker)
//  The drawing itself (ScriptGroup_DrawTeamColorViz 0x46B110 and its two leaves) lives in
//  xywnd.cpp.  It is the XYWnd analog of camwnd.cpp's Cam_DrawTokens (CamWnd_Tokens
//  0x4076C0) - same token-parse + per-entity billboard machinery, but driven from
//  CXYWnd::XY_Draw with the view-rect AABB cull + the two view axes the 2D view supplies.
//  Called from XY_DrawBrushes (the binary's call position: XY_Draw 0x46D867), gated on a
//  selected trigger's non-empty ScriptColorTeamKey value.
// ═════════════════════════════════════════════════════════════════════════════
extern void  Ed_DrawScriptColorQuad( const entity_s *entDef, const float *color );// brush.cpp  0x46AE10

// 0x4560F0  PrefsDlg_ScriptTeamColorEnabled — true when the team-colour
// visualization is active: ScriptGroupKey != "token" AND ScriptGroupKey == the
// ScriptColorTeamKey.  XY_Draw
// (xywnd.cpp) gates the per-trigger team-colour capture on this; the camera path inlines
// the same test.
bool PrefsDlg_ScriptTeamColorEnabled()
{
    return strcmp( g_PrefsDlg->ScriptGroupKey.c_str(), "token" ) != 0
        && strcmp( g_PrefsDlg->ScriptGroupKey.c_str(),
                   g_PrefsDlg->ScriptColorTeamKey.c_str() ) == 0;
}


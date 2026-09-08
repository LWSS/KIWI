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
#include <string>
#include <vector>

extern void Assert( const char *file, int line, int type, const char *fmt, ... );

// ── externs (per-TU, matching the established pattern in select.cpp/brush.cpp) ──
extern int          Sys_Printf( const char *fmt, ... );
extern char        *va( const char *fmt, ... );
extern entity_s    *world_entity;                                         // 0x25D5B30 (map.cpp)
extern void         SetKeyValue( entity_s_def *e, const char *key, const char *value );  // 0x483690
extern void         DeleteKey( epair_t **head, const char *key );         // 0x483720
extern bool         Entity_HasEpairMatch( entity_s *e, const char *key, const char *val );// 0x483930
extern bool         HasKeyValuePair( entity_s_def *e, const char *key );  // 0x4838B0 (entity.cpp)
extern char        *ValueForKey2( const entity_s *defPtr, const char *key );          // 0x4825C0 (entity.cpp)
extern int          UpdateSelection( int wParam, eclass_t *cls );         // 0x497180 (win_ent.cpp)
extern void         ImGuiPanel_ScriptGroup_Toggle();                      // imgui_panel_scriptgroup.cpp:76  void ImGuiPanel_ScriptGroup_Toggle()
// selected_brushes / active_brushes sentinels are declared in qe3.h.

// ─────────────────────────────────────────────────────────────────────────────
// 0x451170  ScriptGroup_Unreachable  (70 bytes)   [PORTED — dependency-free]
// Maps a script-colour code substring to its index 0..6 (r,b,y,c,g,p,o =
// red/blue/yellow/cyan/green/purple/orange).  Returns -1 (and asserts) if none
// match.  The only scriptgroup.cpp function with no g_PrefsDlg/UpdateSelection
// dependency.  (off_73B07C in the binary; ASSERT_UNKOWN gate omitted — the assert
// message is literally "Unreachable", i.e. the not-found path is a programming
// error, so we always assert there.)
// ─────────────────────────────────────────────────────────────────────────────
int ScriptGroup_Unreachable( const char *a1 )
{
    static const char *const codes[7] = { "r", "b", "y", "c", "g", "p", "o" };
    for ( int i = 0; i < 7; ++i )
    {
        if ( strstr( a1, codes[i] ) )
            return i;
    }
    // KEEP_VERBOSE: prose condition string ("Unreachable").
    Assert( "C:\\trees\\cod3-pc\\cod3-modtools\\cod3src\\Radiant\\ScriptGroup.cpp",
            39, 0, "Unreachable" );
    return -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  KEY ADD/REMOVE subsystem (PORTED — real, gate-verified)
// ═════════════════════════════════════════════════════════════════════════════
//
// CoD editor design fact (relied on by every iterator's Assert and by the def
// dereferences): for a placed entity, entity_s and entity_s_def are the SAME struct
// and `def` is self-referential (entity IS its own def), so the
// binary's `b->owner->def == b->def->owner` assert is `entity->def == entity`.  The
// epair list head lives at offset 0x74 on that def (entity_s::epairs).  The asserts
// pass type=0 (engine_stubs Assert only DebugBreak()s on type!=0), so they log-and-
// continue — faithful and gate-safe.

// 0x4537A0  ScriptGroup_AddKey  (512) — ADD a space-separated value-token to one
// entity's key.  Binary: copy the existing key value into a 1024 buffer; if empty,
// the value becomes the whole string; else tokenise and, if the value isn't already
// a token, append " <value>".  (The decompile's v16/v17 contiguous-buffer + strtok-
// then-restore dance is exactly a strcpy(existing)+membership-test+strcat below.)
static void ScriptGroup_AddKey( const char *key, const char *value, entity_s_def *def )
{
    // Resolve the existing value for `key` (case-insensitive, like the binary's _stricmp walk).
    const char *existing = "";
    for ( epair_t *ep = def->epairs; ep; ep = ep->next )
    {
        if ( !_stricmp( ep->key, key ) ) { existing = ep->value; break; }
    }

    char buf[1024];
    strcpy( buf, existing );

    if ( !buf[0] )
    {
        // No existing value → the value becomes the whole string.
        Sys_Printf( "Added value %s to key %s\n", value, key );
        strcpy( buf, value );
        SetKeyValue( def, key, buf );
        return;
    }

    // Existing value present → test membership against a scratch copy (the binary
    // strtok's buf in place, then restores it from the saved value pointer; tokenising
    // a copy keeps buf intact so we can append directly — identical net string).
    char scratch[1024];
    strcpy( scratch, buf );
    for ( char *token = strtok( scratch, " " ); token; token = strtok( nullptr, " " ) )
    {
        iassert( token[0] );
        if ( !strcmp( token, value ) )
        {
            Sys_Printf( "Entity already has value %s on their key %s\n", value, key );
            return;
        }
    }

    // Not present → append " <value>" (binary: strcpy(buf,existing)+va(" %s",value)).
    strcat( buf, va( " %s", value ) );
    SetKeyValue( def, key, buf );
    Sys_Printf( "Added value %s to key %s\n", value, key );
}

// 0x453440  ScriptGroup_HasKey  (553) — REMOVE a space-separated value-token from one
// entity's key (the name is from the ID-pass; it is a REMOVE, not a query).  Binary:
// copy the existing value into v18; tokenise; rebuild the kept tokens (each followed by
// a trailing space) into the contiguous v19; then SetKeyValue(v19) if non-empty else
// DeleteKey.  The TRAILING SPACE on every kept token is preserved verbatim (the binary
// emits `va("%s ", token)`), which is load-bearing for the .map round-trip diff.
static void ScriptGroup_HasKey( const char *value, const char *key, entity_s_def *def )
{
    const char *existing = "";
    for ( epair_t *ep = def->epairs; ep; ep = ep->next )
    {
        if ( !_stricmp( ep->key, key ) ) { existing = ep->value; break; }
    }

    char buf[1024];
    strcpy( buf, existing );
    if ( !buf[0] )
    {
        Sys_Printf( "Entity doesn't have key %s\n", key );
        return;
    }

    char out[1024];
    out[0] = 0;
    bool hasKey = false;

    char *token = strtok( buf, " " );
    if ( !token )
    {
        Sys_Printf( "Entity doesn't have key %s value %s\n", key, value );
        return;
    }
    do
    {
        iassert( token[0] );
        if ( !strcmp( token, value ) )
        {
            iassert( !hasKey );
            hasKey = true;
        }
        else
        {
            // Keep this token — append "<token> " (TRAILING SPACE, matching va("%s ",tok)).
            strcat( out, va( "%s ", token ) );
        }
        token = strtok( nullptr, " " );
    }
    while ( token );

    if ( !hasKey )
    {
        Sys_Printf( "Entity doesn't have key %s value %s\n", key, value );
        return;
    }

    if ( strlen( out ) )
        SetKeyValue( def, key, out );
    else
        DeleteKey( &def->epairs, key );           // binary: DeleteKey(&def->epairs, key)
    Sys_Printf( "Removed value %s from key %s\n", value, key );
}

// ── selection iterators ───────────────────────────────────────────────────────
// All four walk selected_brushes, skip world/null-owner brushes, and (faithfully) carry
// the `b->owner->def == b->def->owner` self-ref assert; the *Triggers variants additionally
// gate on the entity being trigger_multiple/trigger_radius.  Each ends with UpdateSelection.

// 0x453A50  ScriptGroup_AddKeyToSelected — add `value` to `key` on every selected entity.
// (Binary calls ScriptGroup_AddKey(key, value, def); the dialog wrapper sub_454780 passes
//  value=field2(ScriptSubValue), key=field1(ScriptSubKey).)
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
// selected entity.  (Binary calls ScriptGroup_HasKey(value, key, def); wrapper sub_4549B0
// passes key=field1, value=field2.)
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
//  SCRIPT-GROUP NUMBER ASSIGNMENT (PORTED — real, scriptgrpcolor-gate verified)
// ═════════════════════════════════════════════════════════════════════════════
//
// This is the core both the Script-Group dialog's colour buttons AND the Vehicle
// dialog's 10 script-group buttons drive.  Each button stores a key NAME into
// g_PrefsDlg->ScriptGroupKey (VehicleDlg_SetScriptGroupKey 0x45FBA0 →
// str_set(&ScriptGroupKey, key)) and then calls ScriptGroup_AssignNextNumber, which
// finds the highest existing group number stored under that key across all
// (non-selected) world entities and assigns max+1 to the current selection.
//
// SCOPE NOTE — this is NOT the colour-token machinery (ScriptGroup_RemoveColors[_02]
// / ScriptGroup_Color / the single-char ScriptColorKey "r/b/y/c/g/p/o" codes).  Those
// edit a DIFFERENT key (g_PrefsDlg->ScriptColorTeamKey, a space-separated list of
// <colourCode><number> tokens) and are implemented separately, with the Script-Group
// MFC dialog, further down this file.  AssignNextNumber writes a single SCALAR number under
// ScriptGroupKey via SetKeyValue (wholesale replace), so its faithful inverse is a
// plain DeleteKey of that key — ScriptGroup_RemoveAssignedNumber below (NOT
// ScriptGroup_RemoveColors, which is the colour-list subsystem).

// ScriptGroup_AssignNextNumber (0x479FF0) now lives in brush.cpp — its asserts are
// brush.cpp:4293/4298, i.e. that is its source file.
extern void ScriptGroup_AssignNextNumber();   // brush.cpp 0x479FF0

// 0x45FBA0  VehicleDlg_SetScriptGroupKey — the Vehicle dialog's script-group button
// entry: store the chosen key NAME into g_PrefsDlg->ScriptGroupKey, then assign the next
// group number to the selection.  (The binary's str_set is a CString assignment; the
// EndDialog/SetFocus/g_nUpdateBits tail is the modeless-dialog "applied, return focus"
// gesture, supplied by the caller in the UI.)  Ported here so the assignment closure
// lives with the rest of the script-group machinery; vehicledlg.cpp's buttons call it.
void VehicleDlg_SetScriptGroupKey( const char *key )
{
    g_PrefsDlg->ScriptGroupKey = ( key ? key : "" );
    ScriptGroup_AssignNextNumber();
}

// ── INVERSE (the faithful undo of AssignNextNumber's scalar write) ─────────────
// AssignNextNumber does SetKeyValue(def, ScriptGroupKey, "<N>") — a wholesale scalar
// write — so removing it is a plain DeleteKey of that key on every selected non-world
// entity.  (This is the number subsystem's inverse; the colour-LIST inverse is the
// ScriptGroup_RemoveColors[_02] below, a different key + token-strip.)  Used by the
// scriptgrpcolor gate to verify assign → remove → gone.
void ScriptGroup_RemoveAssignedNumber( const char *key )
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        entity_s_def *def = (entity_s_def *)owner->def;
        // KEEP_VERBOSE: inlined brush.cpp:4313 owner-vs-def invariant (the carrier
        // iassert lives in brush.cpp; this copy's guard shape can't stringize to it).
        if ( b->def && def != (entity_s_def *)b->def->owner )
            Assert( "C:\\trees\\cod3-pc\\cod3-modtools\\cod3src\\Radiant\\brush.cpp",
                    4313, 0, "%s", "b->owner->def == b->def->owner" );
        DeleteKey( &def->epairs, key );      // entity_s::epairs @0x74
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  COLOUR-GROUP / TRIGGER-NUMBER machinery (partial — ScriptGroup_Type only)
// ═════════════════════════════════════════════════════════════════════════════
#define MAX_COLORGROUPS 512
// MAX_COLORENTREES is the per-turret export-token cap in the turret-share helper.
#define MAX_COLORENTREES 32

// IDB `zero` (0x6d58f0) is the shared empty-string global the colour fns default an absent
// epair value to.  Reproduced as a local empty string (value-identical to "").
static const char zero[] = "";

extern void Brush_RemoveFromList( selbrush_t *b );   // brush.cpp 0x476680
extern void Brush_AddToList2( selbrush_t *b );       // brush.cpp 0x4765a0
extern void Select_Deselect( int bDeselectFaces );   // select.cpp 0x48E800
extern int  g_nUpdateBits;                           // engine_stubs.cpp 0x25D5A74
extern void sub_47D060( selbrush_t *listSentinel );   // brush.cpp (brush-list display rebuild)

// 0x451200  ScriptGroup_Type  (ScriptGroup.cpp:62)
// Grow the selection by trigger COLOUR GROUP: gather the colour-group numbers of every
// selected trigger_multiple/trigger_radius entity (parsed from the ScriptColorTeamKey
// epair's ScriptColorKey-prefixed token), then select every active trigger whose own
// colour-group number is one of them.  Called by SelectedAssociated when
// ScriptGroupKey == ScriptColorTeamKey.  The binary builds the per-entity value string
// into a 1024 stack buffer (inlined strcpy of the `zero`-defaulted epair value) and
// parses it with strtok/strstr/atol; reproduced 1:1 below.  The active-brush walk saves
// `&match->next->prev` before Brush_RemoveFromList/AddToList2 relink the node.
void ScriptGroup_Type()
{
    const char *scriptColorTeamKey = g_PrefsDlg->ScriptColorTeamKey.c_str();
    const char *scriptColorKey     = g_PrefsDlg->ScriptColorKey.c_str();

    int colorGroups[MAX_COLORGROUPS];
    int colors = 0;

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

        const char *value = "";   // IDB `zero` — empty string when the key is absent
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, scriptColorTeamKey ) ) { value = ep->value; break; }
        }

        char buf[1024];
        strcpy( buf, value );
        if ( !buf[0] )
            continue;
        if ( !strstr( buf, scriptColorKey ) )
            continue;

        // Find the single ScriptColorKey-prefixed token and atol its numeric tail.
        int triggerNumber = -1;
        for ( char *token = strtok( buf, " " ); token; token = strtok( nullptr, " " ) )
        {
            iassert( token[0] );
            if ( strstr( token, scriptColorKey ) )
            {
                vassert( (triggerNumber == -1), "(token) = %s", token );   // ScriptGroup.cpp:98
                triggerNumber = atol( &token[strlen( scriptColorKey )] );
            }
        }
        if ( triggerNumber != -1 )
            iassert( triggerNumber < MAX_COLORGROUPS );
        else
            iassert( triggerNumber != -1 );

        colorGroups[colors++] = triggerNumber;
        iassert( colors < MAX_COLORGROUPS );
    }

    // Pass 2 — select every active trigger whose colour-group number is in the set.
    for ( selbrush_t *bAll = active_brushes.next; bAll != &active_brushes; )
    {
        selbrush_t *next = bAll->next;   // captured before Brush_RemoveFromList/AddToList2 relink
        entity_s *owner = bAll->owner;
        if ( owner && owner != world_entity )
        {
            iassert( bAll->owner->def == bAll->def->owner );

            entity_s_def *def = (entity_s_def *)bAll->owner->def;
            const char *value = "";
            for ( epair_t *ep = def->epairs; ep; ep = ep->next )
            {
                if ( !_stricmp( ep->key, scriptColorTeamKey ) ) { value = ep->value; break; }
            }

            char buf[1024];
            strcpy( buf, value );
            if ( buf[0] && strstr( buf, scriptColorKey ) )
            {
                for ( char *token = strtok( buf, " " ); token; token = strtok( nullptr, " " ) )
                {
                    iassert( token[0] );
                    if ( strstr( token, scriptColorKey ) )
                    {
                        int triggerNumber = atol( &token[strlen( scriptColorKey )] );
                        for ( int n = 0; n < colors; ++n )
                        {
                            if ( colorGroups[n] == triggerNumber )
                            {
                                Brush_RemoveFromList( bAll );
                                Brush_AddToList2( bAll );
                                break;
                            }
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
// CONVENTIONS used throughout (all verified against the disasm, NOT hex-rays):
//  * selbrush_t (display-list node): owner@0x08, def@0x14.  The self-ref invariant
//    `b->owner->def == b->def->owner` (= entity->def == entity) is carried verbatim.
//  * the colour-team value is a space-separated list of "<colourCode><number>" tokens
//    stored under the epair key g_PrefsDlg->ScriptColorTeamKey; the single-char colour
//    code is (const char*)g_PrefsDlg->ScriptColorKey (IDB `*(const char**)…ScriptColorKey`
//    is the CString-buffer-as-ptr idiom).  `zero` (IDB) → "".
//  * the binary copies an epair value into a 1024 stack buffer with an inlined strcpy
//    (`v5 = buf - src; src[v5] = *src; ++src`) — reproduced as strcpy below.
//  * the rebuild-into-a-fixed-16-byte-stride-scratch + rejoin-with-trailing-spaces dance
//    (`strcpy(scratch+16*n, tok)` then `qmemcpy(&buf[..],scratch+16*n); strcpy(end," ")`)
//    is reproduced as a real scratch[N][16] + strcat with a trailing space.

// ─────────────────────────────────────────────────────────────────────────────
// 0x453fd0  ScriptGroup_BrushIsTrigger (ScriptGroup.cpp:1150) — true if the brush's
// entity is trigger_multiple/trigger_radius.  (Leaf.)  Exported (non-static) so the
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

// 0x454050  ScriptGroup_BrushIsTurret (ScriptGroup.cpp:1162) — true if the brush's
// entity is misc_turret.  (Leaf; unused by the wired paths but ported for completeness.)
static bool ScriptGroup_BrushIsTurret( selbrush_t *b )
{
    entity_s *owner = b->owner;
    if ( !owner || owner == world_entity )
        return false;
    iassert( b->owner->def == b->def->owner );
    return Entity_HasEpairMatch( (entity_s_def *)owner->def, "classname", "misc_turret" );
}

// 0x454260  ScriptGroup_IsValidColorCode (already IDB-renamed) — true if the current
// ScriptColorKey exactly matches one of the 7 single-char codes {r,b,y,c,g,p,o}.
// (Binary walks off_73B07C with strcmp until the array-end sentinel flt_73B098.)
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

// 0x451a40  ScriptGroup_TeamKeyContains (already IDB-renamed) — true if the entity DEF
// `def`'s ScriptColorTeamKey value contains the substring `code`.  Walks the def's epairs
// directly (defaults to `zero`=""), copies into a 1024 buffer, returns buf[0]&&strstr.
static bool ScriptGroup_TeamKeyContains( const char *code, entity_s_def *def )
{
    const char *value = zero;       // IDB `zero` = ""
    for ( epair_t *ep = def->epairs; ep; ep = ep->next )
    {
        if ( !_stricmp( ep->key, g_PrefsDlg->ScriptColorTeamKey.c_str() ) ) { value = ep->value; break; }
    }
    char buf[1024];
    strcpy( buf, value );
    return buf[0] && strstr( buf, code ) != nullptr;
}

// 0x4540b0  ScriptGroup_TriggerColorNumber (already IDB-renamed) — read the FIRST
// ScriptColorKey-prefixed token's number from the brush entity's ScriptColorTeamKey
// value, or -1 if absent.  (Single-match: no `triggerNumber==-1` duplicate assert.)
static int ScriptGroup_TriggerColorNumber( selbrush_t *b )
{
    entity_s_def *def = (entity_s_def *)b->owner->def;
    const char *scriptColorKey = g_PrefsDlg->ScriptColorKey.c_str();

    const char *value = zero;
    for ( epair_t *ep = def->epairs; ep; ep = ep->next )
    {
        if ( !_stricmp( ep->key, g_PrefsDlg->ScriptColorTeamKey.c_str() ) ) { value = ep->value; break; }
    }
    char buf[1024];
    strcpy( buf, value );
    if ( !buf[0] || !strstr( buf, scriptColorKey ) )
        return -1;

    for ( char *token = strtok( buf, " " ); token; token = strtok( nullptr, " " ) )
    {
        iassert( token[0] );
        if ( strstr( token, scriptColorKey ) )
            return atol( &token[strlen( scriptColorKey )] );
    }
    return -1;
}

// 0x454220  ScriptGroup_SelectionTriggerNumber (already IDB-renamed) — the LAST
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

    char buf[1024];
    strcpy( buf, ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() ) );
    return buf[0] && strstr( buf, code ) != nullptr;
}

// 0x451650  ScriptGroup_Color — pick the FIRST free colour-group number (0..511) not
// currently used by any active OR selected trigger entity.  Builds a used[512] bitmap by
// parsing every entity's ScriptColorTeamKey token list (atol of each ScriptColorKey-
// prefixed token → used[n]=1), then returns the first index where used==0.
static int ScriptGroup_Color()
{
    char used[MAX_COLORGROUPS];
    memset( used, 0, sizeof( used ) );
    const char *scriptColorKey = g_PrefsDlg->ScriptColorKey.c_str();

    // Pass 1 — active brushes.
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );

        entity_s_def *def = (entity_s_def *)owner->def;
        const char *value = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, g_PrefsDlg->ScriptColorTeamKey.c_str() ) ) { value = ep->value; break; }
        }
        char buf[1024];
        strcpy( buf, value );
        if ( !buf[0] || !strstr( buf, scriptColorKey ) )
            continue;
        for ( char *token = strtok( buf, " " ); token; token = strtok( nullptr, " " ) )
        {
            iassert( token[0] );
            if ( strstr( token, scriptColorKey ) )
            {
                int n = atol( &token[strlen( scriptColorKey )] );
                iassert( n < MAX_COLORGROUPS );      // "colorNumber < MAX_COLORGROUPS"
                used[n] = 1;
            }
        }
    }

    // Pass 2 — selected brushes (identical parse).
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        iassert( b->owner->def == b->def->owner );

        entity_s_def *def = (entity_s_def *)owner->def;
        const char *value = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, g_PrefsDlg->ScriptColorTeamKey.c_str() ) ) { value = ep->value; break; }
        }
        char buf[1024];
        strcpy( buf, value );
        if ( !buf[0] || !strstr( buf, scriptColorKey ) )
            continue;
        for ( char *token = strtok( buf, " " ); token; token = strtok( nullptr, " " ) )
        {
            iassert( token[0] );
            if ( strstr( token, scriptColorKey ) )
            {
                int n = atol( &token[strlen( scriptColorKey )] );
                iassert( n < MAX_COLORGROUPS );
                used[n] = 1;
            }
        }
    }

    // First free index.  (Binary: while (used[result]) if (++result>=512) assert "unreachable".)
    int result = 0;
    while ( used[result] )
    {
        if ( ++result >= MAX_COLORGROUPS )
        {
            // KEEP_VERBOSE: prose condition string ("unreachable").
            Assert( "C:\\trees\\cod3-pc\\cod3-modtools\\cod3src\\Radiant\\ScriptGroup.cpp",
                    249, 0, "unreachable" );
            return 0;
        }
    }
    return result;
}

// ── helper used by ScriptGroup_SetKey/SetKey2/ApplyColorToSelected/RemoveColors* ──
// Rejoin a fixed 16-byte-stride scratch array of `count` tokens into `out` with a
// trailing space after EACH token (matching the binary's `qmemcpy + strcpy(" ")` dance).
// Net string is byte-identical (incl. the trailing space, load-bearing for the .map diff).
static void ScriptGroup_JoinTokens( char *out, const char ( *scratch )[16], int count )
{
    for ( int n = 0; n < count; ++n )
    {
        strcat( out, scratch[n] );
        strcat( out, " " );
    }
}

// 0x4520c0  ScriptGroup_SetKey2 (ScriptGroup.cpp:401) — remove the colour token whose
// number == `colorNumber` from ONE brush entity's ScriptColorTeamKey value (keep all
// other tokens, re-joined with trailing spaces).  Brush arrives in ecx (RemoveUnused
// passes it); colorNumber is the second arg.  SetKeyValue if non-empty else DeleteKey.
static void ScriptGroup_SetKey2( selbrush_t *b, int colorNumber )
{
    entity_s *owner = b->owner;
    if ( !owner || owner == world_entity )
        return;
    iassert( b->owner->def == b->def->owner );
    entity_s_def *def = (entity_s_def *)owner->def;

    const char *scriptColorKey = g_PrefsDlg->ScriptColorKey.c_str();
    char buf[1024];
    strcpy( buf, ValueForKey2( def, g_PrefsDlg->ScriptColorTeamKey.c_str() ) );
    if ( !buf[0] || !strstr( buf, scriptColorKey ) )
        return;

    char scratch[MAX_COLORGROUPS][16];
    int kept = 0;
    for ( char *token = strtok( buf, " " ); token; token = strtok( nullptr, " " ) )
    {
        iassert( token[0] );
        // Keep every token EXCEPT the one matching this colour code AND number.
        if ( !strstr( token, scriptColorKey ) || atol( &token[strlen( scriptColorKey )] ) != colorNumber )
        {
            strcpy( scratch[kept], token );
            ++kept;
        }
    }

    char out[1024];
    out[0] = 0;
    if ( kept > 0 )
        ScriptGroup_JoinTokens( out, scratch, kept );

    if ( strlen( out ) )
        SetKeyValue( def, g_PrefsDlg->ScriptColorTeamKey.c_str(), out );
    else
        DeleteKey( &def->epairs, g_PrefsDlg->ScriptColorTeamKey.c_str() );
    Sys_Printf( "Set entity key value to %s\n", out );
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

// 0x452a... forward decl needed by RemoveUnusedIfOrphaned.
// 0x452b50  ScriptGroup_RemoveUnusedIfOrphaned (already IDB-renamed) — remove colour
// group `colorNumber` everywhere ONLY IF no non-trigger (AI/node) entity still references
// it (scan selected then active brushes for the "<code><n> " token; bail if found).
static bool ScriptGroup_RemoveUnusedIfOrphaned( int colorNumber )
{
    char token[1024];
    strcpy( token, va( "%s%i ", g_PrefsDlg->ScriptColorKey.c_str(), colorNumber ) );

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( ScriptGroup_SelectedHasColorTeam( b, token ) )
            return true;       // still in use → keep
    }
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
    {
        if ( ScriptGroup_SelectedHasColorTeam( b, token ) )
            return true;
    }
    ScriptGroup_RemoveUnused( colorNumber );
    return false;
}

// 0x4523a0  ScriptGroup_RemoveColors (ScriptGroup.cpp:~478) — strip colour token
// (code `colorCode`, number `colorNumber`) from every selected NON-trigger (AI/node)
// entity.  Returns true if any entity was modified.  (The binary takes `colorCode` as a
// CString-by-value and releases it at the end; we take a plain const char* — it is only
// ever read via strstr/strlen — so the CString fork/release pair in the caller is dropped.)
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

        const char *value = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, g_PrefsDlg->ScriptColorTeamKey.c_str() ) ) { value = ep->value; break; }
        }
        char buf[1024];
        strcpy( buf, value );
        if ( !buf[0] || !strstr( buf, colorCode ) )
            continue;

        char scratch[MAX_COLORGROUPS][16];
        char out[1024];
        out[0] = 0;
        int kept = 0;
        for ( char *token = strtok( buf, " " ); token; token = strtok( nullptr, " " ) )
        {
            iassert( token[0] );
            if ( !strstr( token, colorCode ) || atol( &token[strlen( colorCode )] ) != colorNumber )
            {
                strcpy( scratch[kept], token );
                ++kept;
            }
        }
        if ( kept > 0 )
            ScriptGroup_JoinTokens( out, scratch, kept );

        if ( strlen( out ) )
            SetKeyValue( def, g_PrefsDlg->ScriptColorTeamKey.c_str(), out );
        else
            DeleteKey( &def->epairs, g_PrefsDlg->ScriptColorTeamKey.c_str() );
        modified = true;
        Sys_Printf( "Set entity key value %s\n", out );
    }
    return modified;
}

// 0x452720  ScriptGroup_RemoveColors_02 (ScriptGroup.cpp:~553) — the TRIGGER-entity
// variant of the above (uses g_PrefsDlg->ScriptColorKey directly; no return value, no
// CString release).  Strips colour group `colorNumber` from selected trigger entities.
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

        const char *value = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, g_PrefsDlg->ScriptColorTeamKey.c_str() ) ) { value = ep->value; break; }
        }
        char buf[1024];
        strcpy( buf, value );
        if ( !buf[0] || !strstr( buf, scriptColorKey ) )
            continue;

        char scratch[MAX_COLORGROUPS][16];
        char out[1024];
        out[0] = 0;
        int kept = 0;
        for ( char *token = strtok( buf, " " ); token; token = strtok( nullptr, " " ) )
        {
            iassert( token[0] );
            if ( !strstr( token, scriptColorKey ) || atol( &token[strlen( scriptColorKey )] ) != colorNumber )
            {
                strcpy( scratch[kept], token );
                ++kept;
            }
        }
        if ( kept > 0 )
            ScriptGroup_JoinTokens( out, scratch, kept );

        if ( strlen( out ) )
            SetKeyValue( def, g_PrefsDlg->ScriptColorTeamKey.c_str(), out );
        else
            DeleteKey( &def->epairs, g_PrefsDlg->ScriptColorTeamKey.c_str() );
        Sys_Printf( "Set entity key value %s\n", out );
    }
}

// 0x453170  ScriptGroup_TriggerNumber — the DISASSOCIATE core.  For each selected
// trigger's single colour number, try removing it from non-trigger entities; if NONE had
// it (RemoveColors returned false) fall back to removing it from the trigger entities;
// then RemoveUnusedIfOrphaned.  (The binary passes a CString COPY of ScriptColorKey to
// RemoveColors via sub_40F2D0 [CString fork] and RemoveColors releases it; we pass the
// const char* directly — behaviour-identical, the fork/release pair is dropped.)
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

        const char *value = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, g_PrefsDlg->ScriptColorTeamKey.c_str() ) ) { value = ep->value; break; }
        }
        char buf[1024];
        strcpy( buf, value );
        if ( !buf[0] || !strstr( buf, scriptColorKey ) )
            continue;

        int triggerNumber = -1;
        bool hasColor = false;
        for ( char *token = strtok( buf, " " ); token; token = strtok( nullptr, " " ) )
        {
            iassert( token[0] );
            if ( strstr( token, scriptColorKey ) )
            {
                iassert( triggerNumber == -1 );   // scriptgroup.cpp:842
                triggerNumber = atol( &token[strlen( scriptColorKey )] );
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
// GROUP toward target `colorNumber`.  Two 512-bitmaps (verified slot-for-slot vs disasm):
//   selSeen = var_604 (written by PASS 1, the SELECTED triggers, at 0x452ea2)
//   actSeen = var_804 (written by PASS 2, the ACTIVE  triggers, at 0x453100)
// PASS 1 marks selSeen[n] for every selected trigger's colour number n != target.
// PASS 2 marks actSeen[n] for every active  trigger's colour number n != target (read-
//   only; the active walk uses the SAME selbrush_t ->next link [esi+4], not an entity
//   onext — the hex-rays `entity_brush_s::onext`/`owner->owner` were type artifacts).
// FINAL (0x453130): for j in 0..511, if selSeen[j] && j!=target && !actSeen[j] →
//   RemoveUnused(j) — drop a colour group a selected trigger used that no active one does.
static void ScriptGroup_Trigger( int colorNumber )
{
    const char *scriptColorKey = g_PrefsDlg->ScriptColorKey.c_str();
    char selSeen[MAX_COLORGROUPS];   // var_604 — PASS 1 (selected)
    char actSeen[MAX_COLORGROUPS];   // var_804 — PASS 2 (active)
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

        const char *value = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, g_PrefsDlg->ScriptColorTeamKey.c_str() ) ) { value = ep->value; break; }
        }
        char buf[1024];
        strcpy( buf, value );
        if ( !buf[0] || !strstr( buf, scriptColorKey ) )
            continue;

        int triggerNumber = -1;
        for ( char *token = strtok( buf, " " ); token; token = strtok( nullptr, " " ) )
        {
            iassert( token[0] );
            if ( strstr( token, scriptColorKey ) )
            {
                iassert( triggerNumber == -1 );   // scriptgroup.cpp:725
                triggerNumber = atol( &token[strlen( scriptColorKey )] );
            }
        }
        iassert( triggerNumber != -1 );   // scriptgroup.cpp:729
        if ( triggerNumber != colorNumber )
        {
            iassert( triggerNumber < MAX_COLORGROUPS );
            selSeen[triggerNumber] = 1;          // var_604 (0x452ea2)
        }
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

        const char *value = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, g_PrefsDlg->ScriptColorTeamKey.c_str() ) ) { value = ep->value; break; }
        }
        char buf[1024];
        strcpy( buf, value );
        if ( !buf[0] || !strstr( buf, scriptColorKey ) )
            continue;

        int triggerNumber = -1;
        bool hasColor = false;
        for ( char *token = strtok( buf, " " ); token; token = strtok( nullptr, " " ) )
        {
            iassert( token[0] );
            if ( strstr( token, scriptColorKey ) )
            {
                triggerNumber = atol( &token[strlen( scriptColorKey )] );
                if ( triggerNumber == colorNumber )
                    break;                       // target found on an active trigger — stop scanning
                vassert( (!hasColor), "(token) = %s", token );   // ScriptGroup.cpp:774
                hasColor = true;
            }
        }
        iassert( triggerNumber != -1 );   // scriptgroup.cpp:778
        if ( triggerNumber != colorNumber )
        {
            iassert( triggerNumber < MAX_COLORGROUPS );
            actSeen[triggerNumber] = 1;          // var_804 (0x453100)
        }
    }

    // FINAL — drop numbers a selected trigger uses (other than target) that no active
    // trigger still uses.  (IDB 0x453130: if (selSeen[j] && j!=target && !actSeen[j]).)
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

        const char *value = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, g_PrefsDlg->ScriptColorTeamKey.c_str() ) ) { value = ep->value; break; }
        }
        char buf[1024];
        strcpy( buf, value );

        char out[1024];
        out[0] = 0;
        bool hasColor = false;
        if ( buf[0] )
        {
            char scratch[MAX_COLORGROUPS][16];
            int kept = 0;
            for ( char *token = strtok( buf, " " ); token; token = strtok( nullptr, " " ) )
            {
                iassert( token[0] );
                if ( strstr( token, scriptColorKey ) )
                {
                    iassert( !hasColor );   // scriptgroup.cpp:1076
                    hasColor = true;
                }
                else
                {
                    strcpy( scratch[kept], token );
                    ++kept;
                }
            }
            if ( kept > 0 )
                ScriptGroup_JoinTokens( out, scratch, kept );
            // The binary appends one extra trailing space after the kept block here.
            strcat( out, " " );
        }
        // Append the new colour token "<code><n> " (trailing space).
        strcat( out, va( "%s%i ", scriptColorKey, colorNumber ) );

        if ( strlen( out ) )
            SetKeyValue( def, g_PrefsDlg->ScriptColorTeamKey.c_str(), out );
        else
            DeleteKey( &def->epairs, g_PrefsDlg->ScriptColorTeamKey.c_str() );
        Sys_Printf( "Set trigger key value to %s\n", out );
    }
}

// 0x451af0  ScriptGroup_01 (ScriptGroup.cpp:~276) — colour-token reconciliation against
// one node/AI entity DEF (`def`) for colour number `colorNumber`.  For each selected
// trigger whose team key already holds "<code><n>", walk that trigger's team-key tokens
// and, for any token (other than "<code><n>") that `def` ALSO carries, remove it from
// `def`'s team key.  Keeps a node/AI entity from holding colour tokens a selected trigger
// now owns.  (Called from ScriptGroup_SetKey for each freshly-written token.)
static void ScriptGroup_01( entity_s_def *def, int colorNumber )
{
    const char *scriptColorKey = g_PrefsDlg->ScriptColorKey.c_str();

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

        // colourToken = "<code><n>" (no trailing space here — binary va("%s%i",code,n)).
        char colourToken[1024];
        strcpy( colourToken, va( "%s%i", scriptColorKey, colorNumber ) );

        // Only act when THIS trigger's team key already contains the colour token.
        if ( !ScriptGroup_TeamKeyContains( colourToken, trigDef ) )
            continue;

        char list[1024];
        strcpy( list, ValueForKey2( trigDef, g_PrefsDlg->ScriptColorTeamKey.c_str() ) );
        for ( char *token = strtok( list, " " ); token; token = strtok( nullptr, " " ) )
        {
            iassert( token[0] );
            if ( strcmp( colourToken, token ) )       // skip the colour token itself
            {
                if ( ScriptGroup_TeamKeyContains( token, def ) )
                {
                    // Remove `token` from def's team-key value.  The binary builds the key via
                    // va("%s%", ScriptColorTeamKey) — the format string is literally "%s%" with a
                    // trailing LONE '%'.  Both the binary and this port route va through _vsnprintf,
                    // which emits NOTHING for a malformed trailing '%', so the key is exactly the
                    // team key (e.g. "script_color_allies").  Passing the same "%s%" reproduces it
                    // byte-for-byte (do NOT write "%s%%" — that would append a literal '%').
                    char teamKeyArg[256];
                    strcpy( teamKeyArg, va( "%s%", g_PrefsDlg->ScriptColorTeamKey.c_str() ) );
                    ScriptGroup_HasKey( token, teamKeyArg, def );
                }
            }
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

        const char *classname = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, "classname" ) ) { classname = ep->value; break; }
        }

        // Filter: only actor / node / info_volume entities are eligible.
        if ( !strstr( classname, "actor" ) )
        {
            if ( strstr( classname, "node" ) )
            {
                if ( !strcmp( ValueForKey2( def, "radius" ), zero ) )
                    SetKeyValue( def, "radius", "64" );
            }
            else if ( !strstr( classname, "info_volume" ) )
            {
                continue;
            }
        }

        // Read existing team-key value.
        const char *value = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, g_PrefsDlg->ScriptColorTeamKey.c_str() ) ) { value = ep->value; break; }
        }
        char tempString[1024];
        strcpy( tempString, value );

        if ( tempString[0] )
        {
            // Append " <code><n> " (the binary's leading-and-trailing-space variant).
            strcat( tempString, va( " %s%i ", scriptColorKey, colorNumber ) );
            iassert( strlen( tempString ) );      // "strlen(tempString)"
            SetKeyValue( def, g_PrefsDlg->ScriptColorTeamKey.c_str(), tempString );
            Sys_Printf( "Set node/AI key value to %s\n", tempString );
            // Reconcile each token (binary strtok's tempString and calls ScriptGroup_01 per token).
            for ( char *token = strtok( tempString, " " ); token; token = strtok( nullptr, " " ) )
            {
                iassert( token[0] );
                ScriptGroup_01( def, colorNumber );
            }
        }
        else
        {
            // Create the value as "<code><n> " (trailing space).
            strcpy( tempString, va( "%s%i ", scriptColorKey, colorNumber ) );
            if ( strlen( tempString ) == 0 )
                DeleteKey( &def->epairs, g_PrefsDlg->ScriptColorTeamKey.c_str() );
            else
                SetKeyValue( def, g_PrefsDlg->ScriptColorTeamKey.c_str(), tempString );
            Sys_Printf( "Set node/AI key value to %s\n", tempString );
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

            const char *classname = zero;
            for ( epair_t *ep = def->epairs; ep; ep = ep->next )
            {
                if ( !_stricmp( ep->key, "classname" ) ) { classname = ep->value; break; }
            }
            if ( strstr( classname, "actor" ) )
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
// selected-trigger token walk).  Dedups via a 16-byte-stride scratch.
void ScriptGroupFlags_Gather( const char *key, std::vector<std::string> &rows )
{
    char seen[256][16];
    int  seenCount = 0;

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

        const char *value = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, key ) ) { value = ep->value; break; }
        }
        char buf[1024];
        strcpy( buf, value );
        if ( !buf[0] )
            continue;

        for ( char *token = strtok( buf, " " ); token; token = strtok( nullptr, " " ) )
        {
            iassert( token[0] );
            // Add only if not already seen.
            bool found = false;
            for ( int i = 0; i < seenCount; ++i )
            {
                if ( !strcmp( token, seen[i] ) ) { found = true; break; }
            }
            if ( !found )
            {
                rows.push_back( token );
                strcpy( seen[seenCount], token );
                ++seenCount;
            }
        }
    }
}

// 0x454480  ScriptGroup_SyncGroupKeyToTeam (was sub_454480) — force the active script
// "group key" to be the colour-team key (so the colour-team path owns the selection) and
// save prefs.  The two menu handlers call this BEFORE the cluster ops, to switch the
// system into colour-team mode.  The binary also mirrored the new value into the dialog's
// group-key edit and returned focus to the camera; the panel re-reads the pref instead.
void ScriptGroup_SyncGroupKeyToTeam()
{
    g_PrefsDlg->ScriptGroupKey = g_PrefsDlg->ScriptColorTeamKey;   // CString_Assign
    Prefs_SavePrefs( g_PrefsDlg );
}

// ── button actions (the sub_454*/455B* helpers) ───────────────────────────────
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
// Sets the group key to "token" + the token key to "script_turret_share" (which the
// binary mirrored into the sub-key edit 0x65F), then for EACH selected misc_turret entity
// whose "export" key has atol>0, adds token key + that export number to every selected
// entity.  The binary staged both through the two edit controls and read them back out
// via ScriptGroupDlg_AddSubKey; the same pair goes straight into ScriptGroupAddSubKey_Apply
// here, which is what that round trip amounted to (and which stores them to prefs, so the
// panel's fields still end up holding the values the edits did).
void ScriptGroupTurretShare_Apply()   // 0x455b60
{
    std::string exportStr;   // IDB local CString (str_set scratch); std::string here

    g_PrefsDlg->ScriptGroupKey      = "token";                  // str_set(&ScriptGroupKey,"token",5)
    g_PrefsDlg->ScriptGroupTokenKey = "script_turret_share";    // str_set(&ScriptGroupTokenKey,...)
    const std::string subKey = g_PrefsDlg->ScriptGroupTokenKey;

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s_def *def = (entity_s_def *)b->owner->def;

        // classname == "misc_turret"?
        const char *classname = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, "classname" ) ) { classname = ep->value; break; }
        }
        if ( strcmp( classname, "misc_turret" ) )
            continue;

        // its "export" key value.
        const char *value = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, "export" ) ) { value = ep->value ? ep->value : zero; break; }
        }
        exportStr = value;
        if ( atol( exportStr.c_str() ) > 0 )
            ScriptGroupAddSubKey_Apply( subKey.c_str(), exportStr.c_str() );
    }

    sub_47D060( &active_brushes );
    sub_47D060( &selected_brushes );
    sub_47D060( &filtered_brushes );
    g_nUpdateBits = -1;
}

// 0x455d80  ScriptGroupDlg_TurretKey — the "ambush"/"share" turret-key rows.
// Sets the group key to "token" + the token key to `turretKey` (which the binary mirrored
// into the sub-key edit 0x65F).  If the FIRST selected entity's "export" key has atol>0
// (the turret being shared), it gathers the export numbers of every OTHER selected
// misc_turret (up to MAX_COLORENTREES=32, packed into a [32][16] table), re-links the first
// brush to the FRONT of the selection (Select_Deselect(1) + Brush_RemoveFromList/AddToList2),
// then adds token key + each gathered export to every selected entity.  Same edit-control
// round trip collapsed as in ScriptGroupTurretShare_Apply above.
void ScriptGroupTurretKey_Apply( const char *turretKey )   // 0x455d80
{
    std::string exportStr;   // IDB local CString (str_set scratch); std::string here
    char    String[MAX_COLORENTREES][16];   // IDB String[512] = export-token table, stride 16
    int     exports = 0;

    g_PrefsDlg->ScriptGroupKey      = "token";                                  // str_set(...,"token",5)
    g_PrefsDlg->ScriptGroupTokenKey = ( turretKey ? turretKey : "" );           // str_set(&...,Src,strlen); NULL-safe like MFC's CString=
    const std::string subKey = g_PrefsDlg->ScriptGroupTokenKey;

    selbrush_t *first = selected_brushes.next;
    if ( first == &selected_brushes )
        return;

    // the FIRST selected entity's "export" value.
    entity_s_def *firstDef = (entity_s_def *)first->owner->def;
    const char *value = zero;
    for ( epair_t *ep = firstDef->epairs; ep; ep = ep->next )
    {
        if ( !_stricmp( ep->key, "export" ) ) { value = ep->value; break; }
    }
    if ( atol( value ) <= 0 )
        return;

    // gather every OTHER selected misc_turret's export number into the table.
    for ( selbrush_t *b = first->next; b != &selected_brushes; b = b->next )
    {
        entity_s_def *def = (entity_s_def *)b->owner->def;

        const char *classname = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, "classname" ) ) { classname = ep->value; break; }
        }
        if ( strcmp( classname, "misc_turret" ) )
            continue;

        const char *exp = zero;
        for ( epair_t *ep = def->epairs; ep; ep = ep->next )
        {
            if ( !_stricmp( ep->key, "export" ) ) { exp = ep->value ? ep->value : zero; break; }
        }
        exportStr = exp;
        if ( atol( exportStr.c_str() ) > 0 )
        {
            strcpy( String[exports], exportStr.c_str());
            ++exports;
            iassert( exports < MAX_COLORENTREES );   // ScriptGroup.cpp:1818
        }
    }

    // re-link the shared turret to the front of the selection, then propagate.
    Select_Deselect( 1 );
    Brush_RemoveFromList( first );
    Brush_AddToList2( first );
    for ( int i = 0; i < exports; ++i )
        ScriptGroupAddSubKey_Apply( subKey.c_str(), String[i] );

    sub_47D060( &active_brushes );
    sub_47D060( &selected_brushes );
    sub_47D060( &filtered_brushes );
    g_nUpdateBits = -1;
}

// UI-independent action behind the dialog's fixed group-key rows (script_health 1442 /
// script_killspawner 0x5A7) — adopt the key, rebuild the brush-list displays, save prefs and
// assign the next group number to the selection.  (The row's EndDialog stays in the handler.)
void ScriptGroupKeyPreset_Apply( const char *key )
{
    g_PrefsDlg->ScriptGroupKey = key;
    sub_47D060( &active_brushes );
    sub_47D060( &selected_brushes );
    sub_47D060( &filtered_brushes );
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
//  sub_46B110 (0x46B110) + its two leaves CamTokens_EntityGate (0x46AAE0) /
//  CamTokens_BrushMatchesToken (0x46AA80).  Ported VERBATIM from the disasm
// This is the XYWnd analog of camwnd.cpp's Cam_DrawTokens
//  (CamWnd_Tokens 0x4076C0) — same token-parse + per-entity billboard machinery,
//  but driven from CXYWnd::XY_Draw with the view-rect AABB cull + the two view
//  axes the 2D view supplies.  Wired into XY_DrawBrushes (xywnd.cpp) at the
//  binary's call position (XY_Draw 0x46D867, gated on a selected trigger's
//  non-empty ScriptColorTeamKey value).
// ═════════════════════════════════════════════════════════════════════════════
extern char  FilterBrush( selbrush_t *b, int updateFilters );        // filters.cpp 0x46A1F0
extern void  Ed_DrawScriptColorQuad( const entity_s *entDef, const float *color );// brush.cpp  0x46AE10

// 0x4560F0  PrefsDlg_ScriptTeamColorEnabled (sub_4560F0) — true when the team-colour
// visualization is active: ScriptGroupKey != "token" AND ScriptGroupKey == the
// ScriptColorTeamKey.  Disasm-faithful (the && of strcmp!=0 and strcmp==0).  XY_Draw
// (xywnd.cpp) gates the per-trigger team-colour capture on this; the camera path inlines
// the same test.
bool PrefsDlg_ScriptTeamColorEnabled()
{
    return strcmp( g_PrefsDlg->ScriptGroupKey.c_str(), "token" ) != 0
        && strcmp( g_PrefsDlg->ScriptGroupKey.c_str(),
                   g_PrefsDlg->ScriptColorTeamKey.c_str() ) == 0;
}


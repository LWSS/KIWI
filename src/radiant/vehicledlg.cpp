#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Vehicle and vehicle-path-node script-key authoring dialog. It applies to every
// selected non-world entity. The supported keys are:
//   EDIT fields (free value):
//     "script_startinghealth"  starting HP                          (edit 0x460140)
//     "script_accuracy"        0..100 → stored as a 0.00..1.00 float (edit 0x460250,
//                              value = atol(text)/100.0 formatted "%.2f"; default 44)
//     "speed"                  node drive speed                     (edit 0x460790)
//     "lookahead"              spline lookahead distance            (edit 0x4608A0)
//   ON/OFF radios (fixed value, a button per state):
//     "script_deathroll"   1/0   "script_team"  allies/axis
//     "script_turretmg"    1/0   "script_turret" 1/0
//     "script_badplace"    1/0   "script_avoidvehicles" 1/0
//     "script_attackai"    1/0   "script_crashtype" default/plane/forced (combo 0x460700)
//   SCRIPT-GROUP buttons (scriptgroup.cpp colour/number machinery — see below):
//     "script_vehiclespawngroup" "script_vehiclestartmove" "script_vehiclegroupdelete"
//     "script_vehicleride" "script_vehiclewalk" "script_vehicleattackgroup"
//     "script_vehiclefocusfiregroup" "script_vehicledetour" "script_gatetrigger"
//     "script_attackorgs"
//
// Like the binary dialog, it is write-only and empty edits remove their keys.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include <cstring>

extern void Assert( const char *file, int line, int type, const char *fmt, ... );

// ── externs (per-TU, matching dynentitydlg.cpp / select.cpp) ──────────────────
extern entity_s *world_entity;                                              // 0x25D5B30 (map.cpp)
extern void      SetKeyValue( entity_s_def *e, const char *key, const char *value ); // 0x483690 (entity.cpp)
extern void      DeleteKey( epair_t **head, const char *key );              // 0x483720 (entity.cpp)
extern void      Checkkey_Model( entity_s_def *e, const char *key );        // 0x482F70 (=Checkkey_Model_0)
extern void      Checkkey_Color( entity_s_def *e, const char *key );        // 0x483210 (entity.cpp)
extern void      SetKeyValuePairs();                                        // 0x496CF0 (win_ent.cpp)
extern int       g_nUpdateBits;                                            // 0x25D5A74 (engine_stubs.cpp)
extern void      VehicleDlg_SetScriptGroupKey( const char *key );          // 0x45FBA0 (scriptgroup.cpp)
// selected_brushes sentinel is declared in qe3.h.

// Is this selected-brush instance one we should edit?  Faithful to the SetPair/RemovePair
// loop guards: skip a null owner, skip world_entity.  (No eclass-name filter — the vehicle
// dialog applies to any selected vehicle entity / vehicle path node.)  When it returns
// true, *outDef is the entity DEF that carries the epairs.
static bool Veh_SelectedDef( selbrush_t *i, entity_s_def **outDef )
{
    *outDef = nullptr;
    entity_s *owner = i->owner;
    if ( !owner || owner == world_entity )
        return false;

    entity_s_def *def = (entity_s_def *)owner->def;
    if ( !def )
        return false;

    // The binary's invariant (type 0 → log+continue).  b = the binary's local.
    selbrush_t *b = i;
    if ( b->def )
        iassert( b->owner->def == b->def->owner );   // VehicleDlg.cpp:133

    *outDef = def;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45FC00  VehicleDlg_SetPair(value@<ebx>, key@<edi>) — SET `key`=`value` on every
// selected (non-world) entity's def.  The binary iterates selected_brushes.next and
// for each owner != world: assert the def invariant, then SetKeyValue(def, key, value);
// finally EndDialog(GetActiveWindow(),1) + SetFocus(camera) + g_nUpdateBits=-1.
// (No Undo bracket — transcribed verbatim.)
// ─────────────────────────────────────────────────────────────────────────────
void VehicleDlg_SetPair( const char *value, const char *key )
{
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
    {
        entity_s_def *def = nullptr;
        if ( !Veh_SelectedDef( i, &def ) )
            continue;
        SetKeyValue( def, key, value );
    }
    // The dialog refreshes the entity inspector + invalidates the views.  (The binary's
    // EndDialog/SetFocus tail is the modeless-dialog "field applied, return focus to the
    // camera" gesture; the headless gate has no HWNDs so this is just the data refresh.)
    SetKeyValuePairs();
    g_nUpdateBits = -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x45FC90  VehicleDlg_RemovePair(key@<eax>) — REMOVE `key` from every selected
// (non-world) entity's def.  DeleteKey(&def->epairs, key) then the post-delete
// revalidation Checkkey_Model_0 + Checkkey_Color (SetKeyValue fires those on a SET, but
// DeleteKey does not, so RemovePair calls them explicitly — verified at 0x45FCE4..0x45FCF4).
// ─────────────────────────────────────────────────────────────────────────────
void VehicleDlg_RemovePair( const char *key )
{
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
    {
        entity_s_def *def = nullptr;
        if ( !Veh_SelectedDef( i, &def ) )
            continue;
        DeleteKey( &def->epairs, key );      // entity_s::epairs @0x74
        Checkkey_Model( def, key );          // 0x482F70 (Checkkey_Model_0)
        Checkkey_Color( def, key );          // 0x483210
    }
    SetKeyValuePairs();
    g_nUpdateBits = -1;
}

// UI-independent action behind CVehicleDlg's per-field [Set] buttons.
// Faithful to each edit handler (0x460140 etc.): GetWindowText → empty? RemovePair :
// SetPair.  (The accuracy field additionally scales — handled in OnSetAccuracy.)
void VehSetKey_Apply( const char *value, const char *key )
{
    if ( value[0] )
        VehicleDlg_SetPair( value, key );
    else
        VehicleDlg_RemovePair( key );
    g_nUpdateBits |= 1;
}

// UI-independent action behind CVehicleDlg's accuracy [Set] button.
// Accuracy (0x460250): empty → RemovePair (binary resets the slider to 44 first); else
// value = atol(text)/100.0 formatted "%.2f" → SetPair("script_accuracy", value).
void VehSetAccuracy_Apply( const char *text )
{
    if ( !text[0] )
    {
        VehicleDlg_RemovePair( "script_accuracy" );
        g_nUpdateBits |= 1;
        return;
    }
    char val[64];
    _snprintf( val, sizeof( val ), "%.2f", (double)atol( text ) / 100.0 );
    VehicleDlg_SetPair( val, "script_accuracy" );
    g_nUpdateBits |= 1;
}

// UI-independent action behind CVehicleDlg's per-field [Clear] buttons.
void VehClearKey_Apply( const char *key )
{
    VehicleDlg_RemovePair( key );
    g_nUpdateBits |= 1;
}

// UI-independent action behind CVehicleDlg's crash-type [Set] button.
// Crash type (0x460700): CB_GETCURSEL → 0 default / 1 plane / 2 forced → SetPair.
void VehSetCrashType_Apply( int crashType )
{
    const char *v = ( crashType == 1 ) ? "plane" : ( crashType == 2 ) ? "forced" : "default";
    VehicleDlg_SetPair( v, "script_crashtype" );
    g_nUpdateBits |= 1;
}

// UI-independent action behind CVehicleDlg's on/off toggle pushbuttons.
// on/off toggle pushbuttons — each button is a fixed SetPair (the binary's per-button
// thunks at 0x45FDE0.. each SetKeyValue a constant value, e.g. script_turret 1/0).
void VehSetToggle_Apply( const char *value, const char *key )
{
    VehicleDlg_SetPair( value, key );
    g_nUpdateBits |= 1;
}

// UI-independent action behind CVehicleDlg's script-group buttons.
// script-group buttons (the binary's thunks @0x45fd50.. + OnAttackOrgs @0x460670): each
// is VehicleDlg_SetScriptGroupKey("<key>") → store the key NAME into ScriptGroupKey, then
// ScriptGroup_AssignNextNumber assigns the next free group number to the selection.
void VehScriptGroup_Apply( const char *key )
{
    VehicleDlg_SetScriptGroupKey( key );
    g_nUpdateBits |= 1;
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFC shell — CVehicleDlg, the hand-built modeless popup (CDynEntityDlg / CLayerDlg
//  pattern).  The ImGui panel (imgui_panel_vehicle.cpp) calls the Veh*_Apply cores
//  above; nothing outside this block is needed by the ON build.
// ══════════════════════════════════════════════════════════════════════════════


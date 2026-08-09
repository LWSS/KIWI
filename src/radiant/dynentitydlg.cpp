#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Dynamic/destructible entity authoring dialog. The dyn_model keys are:
//   "type"          - "clutter" (default) | "destruct"  (a CComboBox preset list)
//   "physPreset"    - the physics preset                (main_shared\physic\)
//   "health"        - destruct only: HP → triggers a destroy event on death
//   "destroyEfx"    - destruct only: the destroy FX     (raw_shared\fx\, *.efx)
//   "destroyPieces" - destruct only: the breakable model (main_shared\xmodelpieces\)
//
// The dialog is write-only: empty fields remove their keys.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include <cstring>

extern void Assert( const char *file, int line, int type, const char *fmt, ... );

// ── externs (per-TU, matching select.cpp / scriptgroup.cpp) ───────────────────
extern entity_s *world_entity;                                              // 0x25D5B30 (map.cpp)
extern void      SetKeyValue( entity_s_def *e, const char *key, const char *value ); // 0x483690 (entity.cpp)
extern void      DeleteKey( epair_t **head, const char *key );              // 0x483720 (entity.cpp)
extern void      Checkkey_Model( entity_s_def *e, const char *key );        // 0x482F70 (=Checkkey_Model_0)
extern void      Checkkey_Color( entity_s_def *e, const char *key );        // 0x483210 (entity.cpp)
extern void      Undo_ClearRedo();                                          // 0x45DF20 (undo.cpp)
extern void      Undo_GeneralStart( const char *op );                       // 0x45E3F0 (undo.cpp)
extern void      Undo_AddEntity_W( entity_s *e );                           // 0x45E990 (undo.cpp)
extern void      Undo_End();                                               // 0x45EA20 (undo.cpp)
extern void      SetKeyValuePairs();                                        // 0x496CF0 (win_ent.cpp)
extern int       g_nUpdateBits;                                            // 0x25D5A74 (engine_stubs.cpp)
// selected_brushes sentinel is declared in qe3.h.

// Is this selected-brush instance owned by a dyn_ entity we should edit?  Faithful to
// the SetPair/RemovePair loop guards (skip null owner, skip world_entity, require an
// eclass name starting with "dyn_"), with the §11 NULL-eclass/name guard added.  When
// it returns true, `*outDef` is the entity DEF that carries the epairs.
static bool DynEnt_SelectedDef( selbrush_t *i, entity_s_def **outDef )
{
    *outDef = nullptr;
    entity_s *owner = i->owner;
    if ( !owner || owner == world_entity )
        return false;

    entity_s_def *def = (entity_s_def *)owner->def;
    // The binary asserts (type 0 = log-and-continue) on a null eclass->name, then
    // dereferences it in strncmp.  Guard the whole chain (a real dyn_ entity always
    // has both) so a malformed selection never AVs on the never-run path.
    if ( !def || !def->eclass || !def->eclass->name )
    {
        if ( def && def->eclass )
        {
            selbrush_t *b = i;                   // the binary's local
            iassert( b->owner->def->eclass->name );   // DynEntityDlg.cpp:72
        }
        return false;
    }
    if ( strncmp( def->eclass->name, "dyn_", 4 ) != 0 )
        return false;

    // The binary's invariant (type 0 → log+continue).  b = the binary's local.
    if ( i->def )
    {
        selbrush_t *b = i;
        iassert( b->owner->def == b->def->owner );   // DynEntityDlg.cpp:76
    }

    *outDef = def;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x40DF20  DynEntityDlg_01_SetPair(value@<ebx>, key@<edi>) — SET `key`=`value` on
// every selected dyn_ entity's def (Undo-bracketed).  The binary asserts on a null
// value/key, then for each selected brush whose owner is a non-world dyn_ entity:
// Undo_AddEntity_W(def) + SetKeyValue(def, key, value).
// ─────────────────────────────────────────────────────────────────────────────
void DynEntityDlg_01_SetPair( const char *value, const char *key )
{
    iassert(key);
    iassert(value);

    Undo_ClearRedo();
    Undo_GeneralStart( "Set key pair" );
    for ( selbrush_t *i = selected_brushes.prev; i != &selected_brushes; i = i->prev )
    {
        entity_s_def *def = nullptr;
        if ( !DynEnt_SelectedDef( i, &def ) )
            continue;
        Undo_AddEntity_W( (entity_s *)def );
        SetKeyValue( def, key, value );
    }
    Undo_End();
    SetKeyValuePairs();
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x40E040  DynEntityDlg_02_RemovePair(key@<eax>) — REMOVE `key` from every selected
// dyn_ entity's def (Undo-bracketed).  DeleteKey(&def->epairs, key) then the post-
// delete revalidation Checkkey_Model_0 + Checkkey_Color (SetKeyValue fires those on a
// SET, but DeleteKey does not, so RemovePair calls them explicitly — verified vs the
// disasm at 0x40E11F..0x40E12F).
// ─────────────────────────────────────────────────────────────────────────────
void DynEntityDlg_02_RemovePair( const char *key )
{
    iassert(key);

    Undo_ClearRedo();
    Undo_GeneralStart( "Remove key pair" );
    for ( selbrush_t *i = selected_brushes.prev; i != &selected_brushes; i = i->prev )
    {
        entity_s_def *def = nullptr;
        if ( !DynEnt_SelectedDef( i, &def ) )
            continue;
        Undo_AddEntity_W( (entity_s *)def );
        DeleteKey( &def->epairs, key );      // entity_s::epairs @0x74
        Checkkey_Model( def, key );          // 0x482F70 (Checkkey_Model_0)
        Checkkey_Color( def, key );          // 0x483210
    }
    Undo_End();
    SetKeyValuePairs();
}

// UI-independent action behind CDynEntityDlg's per-field [Set] buttons.
// Faithful to each edit handler (0x40E760 etc.): GetWindowText → empty? RemovePair :
// SetPair.  (The binary's per-field control also has dedicated Clear buttons that
// always RemovePair; OnClear* below provide those.)
void DynEntSetKey_Apply( const char *value, const char *key )
{
    if ( value[0] )
        DynEntityDlg_01_SetPair( value, key );
    else
        DynEntityDlg_02_RemovePair( key );
    g_nUpdateBits |= 1;
}

// UI-independent action behind CDynEntityDlg's per-field [Clear] buttons.
void DynEntClearKey_Apply( const char *key )
{
    DynEntityDlg_02_RemovePair( key );
    g_nUpdateBits |= 1;
}

// UI-independent action behind CDynEntityDlg's type [Set] button (0x40E6C0's SetPair tail).
void DynEntSetType_Apply( const char *type )
{
    DynEntityDlg_01_SetPair( type, "type" );
    g_nUpdateBits |= 1;
}

// UI-independent lookup behind CDynEntityDlg's [Help] button (0x40E5C0's eclass walk).
const char *DynEntHelp_Gather()
{
    for ( selbrush_t *i = selected_brushes.prev; i != &selected_brushes; i = i->prev )
    {
        entity_s_def *def = nullptr;
        if ( !DynEnt_SelectedDef( i, &def ) )
            continue;
        if ( def->eclass && def->eclass->comments )
            return def->eclass->comments;
        return nullptr;
    }
    return nullptr;
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFC shell — CDynEntityDlg, the hand-built modeless popup (CLayerDlg / CSurfaceDlg
//  pattern).  Everything below is CWnd/CFileDialog/CString-Mid territory; the ImGui
//  panel (imgui_panel_dynent.cpp) drives the DynEnt*_Apply / DynEntHelp_Gather cores
//  above instead, so nothing outside this block is needed by the ON build.
// ══════════════════════════════════════════════════════════════════════════════


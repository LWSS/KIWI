#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\win_ent.cpp
//
// THE ENTITY WINDOW (CEntityWnd) — the entity inspector.  Two halves of the QE4
// lineage, faithful to the CoD4Radiant IDB cluster (0x496370..0x4981f0):
//   * the eclass LIST  (FillClassList 0x496800, from g_eclass)
//   * the KEY/VALUE editor (SetKeyValuePairs 0x496cf0 / AddProp 0x497490 /
//     DelProp 0x4975c0 / EditProp 0x4977b0 + the key/value edit fields)
//   * create-any-eclass (CreateEntity 0x497300 → Entity_Create)
//   * UpdateSelection (0x497180) — the selection→inspector refresh, driven from
//     Brush_AddToList2 / Brush_RemoveFromList / Select_Deselect.
//
// The binary builds the controls from a CreateDialog(IDD_ENTITY) template +
// SetParent (GetEntityControls 0x4964b0).  radiant.rc carries no dialog templates
// (only the reconstructed menu/accelerators), so the window PLUMBING here is MFC:
// CEntityWnd : CWnd creates the child controls (two listboxes + two edit fields +
// a Delete button + the 12 spawnflag checkboxes + the 10 angle buttons) directly and
// routes their notifications.  The DATA operations transcribe the IDB verbatim:
//   * the 12 SPAWNFLAG checkboxes (SetSpawnFlags 0x496e70 / _R 0x496f00 / _2 0x497040)
//     — each box toggles a bit in the entity's decimal "spawnflags" key; the first 8
//     labels come from the eclass flagname0..7.
//   * the ANGLE-button grid (Entity_SetAngles 0x494030, already in select.cpp) — the
//     8 compass buttons set the "angles" YAW, up/down toggle the PITCH to ∓90.
// The model/prefab picker (OpenDialog / IDC_E_ADD_MODEL) and the faithful SizeEntityDlg
// pixel layout both SHIPPED; only the binary's CModelFileDialog preview PANE is still out
// (a plain CFileDialog gives the identical pick — see CEntityWnd_OpenModelDialog).
//
// NO TAB STRIP.  The binary's inspector DOES carry a mode-switch tab (a CTabCtrl
// g_wndTabsEntWnd, subclassed from IDC_E_TAB_CONTROL in the IDD_ENTITY dialog resource —
// hence no "SysTabControl32" CreateWindowExA class string in the IDB), with the panes
// column-swapped in/out by SizeEntityDlg (0x4978a0)'s entwnd_col trick.  This port has no
// dialog resource to subclass, so an earlier revision INVENTED a raw WC_TABCONTROLA tab.
// That invented tab (created in every mode with a fixed top inset) is the "messed up / has
// tabs" UX the operator flagged.  It is REMOVED: mode switching stays on the N/O/T/F
// hotkeys + the View menu (CEntityWnd_SetInspectorMode), and each mode lays its own
// controls across the full client area from the top (no inset).  This matches both the
// operator's request and the binary's per-mode single-pane presentation as closely as a
// resource-less hand-built window allows.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include <string>
#include <vector>

// ── radiant-local deps (home files) ───────────────────────────────────────────
extern selbrush_t  selected_brushes;                 // engine_stubs (0x23F1864)
extern entity_s   *world_entity;                      // map.cpp      (0x25D5B30)
extern eclass_t   *g_eclass;                          // eclass.cpp   (0x25D5B20)
extern int         g_nUpdateBits;                     // engine_stubs (0x25D5A74)

extern char       *TranslateString( char *buf );      // win_qe3.cpp  (0x499CE0)
extern eclass_t   *Eclass_ForName( int has_brushes, const char *name ); // eclass.cpp (0x482190)
extern entity_s   *Entity_Create( eclass_t *eclass ); // entity.cpp   (0x484980)

extern void        SetKeyValue( entity_s_def *e, const char *key, const char *value ); // entity.cpp 0x483690
extern void        DeleteKey( epair_t **head, const char *key );                       // entity.cpp 0x483720
extern char       *ValueForKey2( int e, const char *key );                             // entity.cpp 0x4825C0
extern int         Entity_GetVec3ForKey( entity_s_def *e, float *out, const char *key );// entity.cpp 0x483860
extern void        Checkkey_Model( entity_s_def *e, const char *key );                 // entity.cpp 0x482F70
extern void        Checkkey_Color( entity_s_def *e, const char *key );                 // entity.cpp 0x483210

extern void        Select_Deselect( int a1 );                                          // select.cpp 0x48E800
extern void        Select_Brush( selbrush_t *b, char ovw, char status, char center );  // select.cpp 0x48DCC0

extern void        Undo_ClearRedo();                                                   // undo.cpp 0x45DF20
extern void        Undo_GeneralStart( const char *operation );                         // undo.cpp 0x45E3F0
extern void        Undo_AddEntity_W( entity_s *e );                                    // undo.cpp 0x45E990
extern void        Undo_End();                                                         // undo.cpp 0x45EA20

extern void        Entity_SetAngles( float a1, int axis );                             // select.cpp 0x494030

extern void        ScriptGroup_HasFlag( const char *key, int dlgItemID, HWND hDlg );   // scriptgroup.cpp 0x454E40


// ── entity-window globals (mirror the IDB file-scope state @ 0x240A1xx) ────────
entity_s_def *edit_entity            = nullptr;   // 0x240A108  currently-edited def
int           multiple_edit_entities = 0;         // 0x240A10C  >1 entity selected

// inspector_mode (IDB 0x240A110): the active inspector pane.  Initialised to
// INSPECTOR_ENTITY (the binary's default — the entity editor is shown at startup).
int           inspector_mode         = INSPECTOR_ENTITY;

static HWND   hwndEnt_entlist        = nullptr;   // 0x240A118  eclass listbox
static HWND   entwnd_comment         = nullptr;   // 0x240A11C  eclass description (read-only)
static HWND   entwnd_kvlist          = nullptr;   // 0x240A150  key/value listbox
static HWND   entwnd_keyfield        = nullptr;   // 0x240A184  key edit field
static HWND   entwnd_valuefield      = nullptr;   // 0x240A18C  value edit field
static HWND   entwnd_keylabel        = nullptr;   // "Key" static
static HWND   entwnd_valuelabel      = nullptr;   // "Value" static
static HWND   entwnd_delbtn          = nullptr;   // "Delete Key" button
static HWND   entwnd_addmodelbtn     = nullptr;   // "Add Model" button (IDC_E_ADD_MODEL)
static WNDPROC OldFieldWindowProc    = nullptr;   // 0x240A0FC  subclassed edit wndproc

// The 12 spawnflag checkbox HWNDs (IDB entwnd_entcheck_1..8 + _easy/_medium/_hard/
// _deathmatch, the contiguous slice 0x240A120..0x240A14C of the GetEntityControls
// HWND array, ENTITY_DEFINES EntCheck1..EntCheck12).  SetSpawnFlags* index this by
// bit (0..11); UpdateSelection labels the first 8 from the eclass flagname0..7.
static HWND   entwnd_entcheck[12]    = { nullptr };

// The 10 angle/direction button HWNDs (IDB entwnd_entdir0/45/90/135/180/225/270/315/
// up/down, ENTITY_DEFINES EntDir0..EntDirDown).  Each maps to an Entity_SetAngles call.
static HWND   entwnd_entdir[10]      = { nullptr };

// ── CFilterWnd controls (the Filters inspector pane — built into this window) ──
// Faithful to the binary's CFilterWnd: four category CHECKLISTS + six simple show-flag
// CHECKBOXES.  The four category lists are OWNER-DRAWN listboxes that render a real checkbox
// glyph (DrawFrameControl) per row and toggle on a SINGLE click (via a small subclass).  The
// binary uses MFC CCheckListBox, but that class's owner-draw state init asserts in this
// static-MFC/MBCS build — this hand-drawn equivalent gives the same look + single-click feel.
static HWND   filt_catlabel[4]       = { nullptr };   // "Geometry" / "Trigger" / "Entity" / "Other"
static HWND   filt_catlist[4]        = { nullptr };   // the four owner-drawn category filter lists
static HWND   filt_flagcheck[6]      = { nullptr };   // the six d_xyShowFlags checkboxes

// TASK 2 — the eclass name currently picked in the list (RMB create reads this; the
// hardcoded "light" default applies until the list has a selection).
static char   s_selectedEclass[256]  = "light";

// CEntityWnd control IDs (this hand-built window uses its own ID space — the binary's
// reconstructed resource IDs are not used for these MFC-created children).  Declared
// here (not in the class block below) because the model-picker helpers above
// reference IDC_ENT_ADD_MODEL_BTN.
enum
{
    IDC_ENT_ECLASS_LIST = 1801,
    IDC_ENT_COMMENT,
    IDC_ENT_KV_LIST,
    IDC_ENT_KEY_FIELD,
    IDC_ENT_VALUE_FIELD,
    IDC_ENT_DELETE_BTN,
    IDC_ENT_KEY_LABEL,
    IDC_ENT_VALUE_LABEL,
    IDC_ENT_ADD_MODEL_BTN,                              // the "Add Model" button (1809)
    // 12 contiguous spawnflag-checkbox IDs (index = ID - IDC_ENT_CHECK_FIRST = bit).
    IDC_ENT_CHECK_FIRST  = 1810,
    IDC_ENT_CHECK_LAST   = IDC_ENT_CHECK_FIRST + 11,   // 1821
    // 10 contiguous angle/direction button IDs (index = ID - IDC_ENT_DIR_FIRST,
    // matching ENTITY_DEFINES EntDir0..EntDirDown).
    IDC_ENT_DIR_FIRST    = 1822,
    IDC_ENT_DIR_LAST     = IDC_ENT_DIR_FIRST + 9,      // 1831

    // ── CFilterWnd controls (the Filters inspector pane) ──────────────────────
    // 4 contiguous category checklists (geometry/trigger/entity/other), index =
    // ID - IDC_FILT_LIST_FIRST = category (matching CFilterWnd_GetCategoryHead order).
    IDC_FILT_LIST_FIRST  = 1840,
    IDC_FILT_LIST_LAST   = IDC_FILT_LIST_FIRST + 3,    // 1843
    // 6 contiguous simple show-flag checkboxes (Angles/Connections/Names/Blocks/
    // Coordinates/Reverse Filter), index = ID - IDC_FILT_FLAG_FIRST.
    IDC_FILT_FLAG_FIRST  = 1844,
    IDC_FILT_FLAG_LAST   = IDC_FILT_FLAG_FIRST + 5,    // 1849
};

// ──────────────────────────────────────────────────────────────────────────────
//  Data operations — faithful ports of the IDB win_ent.cpp cluster.
// ──────────────────────────────────────────────────────────────────────────────

// One row of the eclass list: the name the row is labelled with and the eclass_t* the row
// carries as its item data (read back by the selchange / create paths).
struct eclassRow_t
{
    const char *name;
    eclass_t   *eclass;
};

// UI-independent read behind the eclass list's population (FillClassList 0x496800's
// g_eclass walk).
void EclassList_Gather( std::vector<eclassRow_t> &rows )
{
    for ( eclass_t *pec = g_eclass; pec; pec = pec->next )
    {
        eclassRow_t row;
        row.name   = pec->name;
        row.eclass = pec;
        rows.push_back( row );
    }
}

// FillClassList (0x496800) — reset + repopulate the eclass listbox from g_eclass,
// stashing each eclass_t* as the item data (so selchange/create can recover it).
static void FillClassList()
{
    if ( !hwndEnt_entlist )
        return;
    SendMessageA( hwndEnt_entlist, LB_RESETCONTENT, 0, 0 );

    std::vector<eclassRow_t> rows;
    EclassList_Gather( rows );

    for ( size_t i = 0; i < rows.size(); ++i )
    {
        LRESULT idx = SendMessageA( hwndEnt_entlist, LB_ADDSTRING, 0, (LPARAM)rows[i].name );
        SendMessageA( hwndEnt_entlist, LB_SETITEMDATA, idx, (LPARAM)rows[i].eclass );
    }
}

// ═══ ENTITY KEY/VALUE CLUSTER — AUDIT: all FAITHFUL vs IDA (2026-06-25) ════════════════════════
//  SetKeyValuePairs 0x496cf0: LB_SETCOLUMNWIDTH(width/2) + LB_RESETCONTENT; epair loop skip "origin",
//   strlen(key)<=8 → "%s\t\t%s" else "%s\t%s", LB_ADDSTRING; non-zero origin → "origin\t\t%g %g %g";
//   g_nUpdateBits |= W_CAMERA|W_XY. AddProp 0x497490: key/value WM_GETTEXT(4095) + Undo bracket("set
//   key value pair") + multi per-entity / single Undo_AddEntity_W+SetKeyValue (single + "origin" key →
//   Entity_GetVec3ForKey + ++version[16-bit]). DelProp 0x4975c0: WM_GETTEXT(0xFFF), "classname" →
//   MessageBox-block, else Undo bracket + Undo_AddEntity_W + DeleteKey(&def->epairs) + Checkkey_Model
//   (0x482F70) + Checkkey_Color (0x483210). EditProp 0x4977b0: LB_GETCURSEL/GETTEXT, split on first
//   '\t' → key + (skip 2nd '\t') value into the WM_SETTEXT fields. ALL EXACT (entwnd_kvlist null-guard
//   in SetKeyValuePairs = the only port-added defensive). The hand-built dialog plumbing is the only
//   reconstruction; the logic above is 1:1.
// ════════════════════════════════════════════════════════════════════════════════════════════
// One key/value row of the inspector's key/value list: the epair's key + value.  The
// synthesised "origin" row carries the def's origin vec3, pre-formatted, as its value.
struct entKvRow_t
{
    std::string key;
    std::string value;
};

// UI-independent read behind the key/value list's population (SetKeyValuePairs 0x496cf0's
// epair walk: "origin" is skipped, then appended from the def's origin vec3 when non-zero).
void EntityKeyValues_Gather( entity_s_def *def, std::vector<entKvRow_t> &rows )
{
    if ( !def )
        return;

    for ( epair_t *ep = def->epairs; ep; ep = ep->next )
    {
        if ( !strcmp( ep->key, "origin" ) )
            continue;
        entKvRow_t row;
        row.key   = ep->key;
        row.value = ep->value;
        rows.push_back( row );
    }

    if ( def->origin[0] != 0.0f || def->origin[1] != 0.0f || def->origin[2] != 0.0f )
    {
        char sz[4100];
        sprintf( sz, "%g %g %g", def->origin[0], def->origin[1], def->origin[2] );
        entKvRow_t row;
        row.key   = "origin";
        row.value = sz;
        rows.push_back( row );
    }
}

// SetKeyValuePairs (0x496cf0) — repopulate the key/value listbox from edit_entity's
// epairs (skipping "origin", which is appended specially from the def's origin vec3).
void SetKeyValuePairs()
{
    if ( !edit_entity || !entwnd_kvlist )
        return;

    RECT rc;
    GetWindowRect( entwnd_kvlist, &rc );
    SendMessageA( entwnd_kvlist, LB_SETCOLUMNWIDTH, ( rc.right - rc.left ) / 2, 0 );
    SendMessageA( entwnd_kvlist, LB_RESETCONTENT, 0, 0 );

    std::vector<entKvRow_t> rows;
    EntityKeyValues_Gather( edit_entity, rows );

    char sz[4100];
    for ( size_t i = 0; i < rows.size(); ++i )
    {
        // <=8-char keys get a second tab so the value column lines up.  ("origin" is 6, so
        // the synthesised origin row still comes out as the binary's "origin\t\t%g %g %g".)
        if ( rows[i].key.length() <= 8 )
            sprintf( sz, "%s\t\t%s", rows[i].key.c_str(), rows[i].value.c_str() );
        else
            sprintf( sz, "%s\t%s", rows[i].key.c_str(), rows[i].value.c_str() );
        SendMessageA( entwnd_kvlist, LB_ADDSTRING, 0, (LPARAM)sz );
    }

    g_nUpdateBits |= W_CAMERA | W_XY;
}

// UI-independent action behind the inspector's key/value commit (AddProp 0x497490's set
// half — the Enter-in-the-value-field path and the entity-colour picker both land here).
void EntSetKey_Apply( const char *key, const char *value )
{
    if ( !edit_entity )
        return;

    Undo_ClearRedo();
    Undo_GeneralStart( "set key value pair" );
    if ( multiple_edit_entities )
    {
        for ( selbrush_t *sb = selected_brushes.next; sb != &selected_brushes; sb = sb->next )
        {
            entity_s_def *def = (entity_s_def *)sb->owner->def;
            Undo_AddEntity_W( (entity_s *)def );
            SetKeyValue( def, key, value );
        }
    }
    else
    {
        Undo_AddEntity_W( (entity_s *)edit_entity );
        SetKeyValue( edit_entity, key, value );
        if ( !strcmp( "origin", key ) )
        {
            Entity_GetVec3ForKey( edit_entity, edit_entity->origin, "origin" );
            ++edit_entity->version;
        }
    }
    SetKeyValuePairs();
    Undo_End();
}

// AddProp (0x497490) — read the key/value fields, set the pair (Undo-bracketed) on the
// edited entity (or every selected entity when multiple), refresh the list.
void AddProp()
{
    if ( !edit_entity )
        return;

    char key[4100], value[4096];
    SendMessageA( entwnd_keyfield,   WM_GETTEXT, 4095, (LPARAM)key   );
    SendMessageA( entwnd_valuefield, WM_GETTEXT, 4095, (LPARAM)value );

    EntSetKey_Apply( key, value );
}

// Win_GetEntityKeyValueFields — copy the current text of the entity-window key/value
// edit fields.  This is the data the CKeyValueSelectDlg constructor (IDB sub_416760)
// pre-loads into its key/value CString members via SendMessage(...,WM_GETTEXT,...) on
// entwnd_keyfield / entwnd_valuefield.  Exposed so select.cpp's Select_ByKeyValue dialog
// (which lives in another TU) can seed its edit fields identically.  Buffers must hold
// at least 0x1000 bytes each (the binary reads up to 0xFFF chars per field).
void Win_GetEntityKeyValueFields( char *keyOut, char *valueOut )
{
    if ( keyOut )
    {
        keyOut[0] = 0;
        if ( entwnd_keyfield )
            SendMessageA( entwnd_keyfield, WM_GETTEXT, 0xFFF, (LPARAM)keyOut );
    }
    if ( valueOut )
    {
        valueOut[0] = 0;
        if ( entwnd_valuefield )
            SendMessageA( entwnd_valuefield, WM_GETTEXT, 0xFFF, (LPARAM)valueOut );
    }
}

// Win_SetEntityKeyValueFields — SetWindowTextA the entity-window key/value edit fields.
// The entity-color picker (CMainFrame::OnMiscSelectentitycolor, IDB 0x424C10) writes the
// chosen "_color"/"r g b" here before calling AddProp() to commit the pair.  entwnd_* are
// TU-static, so the caller reaches them through this accessor.
void Win_SetEntityKeyValueFields( const char *key, const char *value )
{
    if ( entwnd_valuefield && value )
        SetWindowTextA( entwnd_valuefield, value );
    if ( entwnd_keyfield && key )
        SetWindowTextA( entwnd_keyfield, key );
}

// UI-independent action behind the inspector's "Delete Key" button (DelProp 0x4975c0's
// delete half, including the classname block).
void EntDeleteKey_Apply( const char *key )
{
    if ( !edit_entity )
        return;

    if ( !_stricmp( key, "classname" ) )
    {
        MessageBoxA( GetActiveWindow(),
            "You can't delete the classname of an entity.  If you want to move this geometry to "
            "the world entity then select \"Ungroup Entity\" from the right-click menu.\n",
            "Radiant", MB_ICONEXCLAMATION );
        return;
    }

    Undo_ClearRedo();
    Undo_GeneralStart( "delete key value pair" );
    if ( multiple_edit_entities )
    {
        for ( selbrush_t *sb = selected_brushes.next; sb != &selected_brushes; sb = sb->next )
        {
            entity_s_def *def = (entity_s_def *)sb->owner->def;
            Undo_AddEntity_W( (entity_s *)def );
            DeleteKey( &def->epairs, key );
            Checkkey_Model( def, key );      // 0x482F70 (IDB stale-named "MapLoad_ParsePrefab")
            Checkkey_Color( def, key );      // 0x483210
        }
    }
    else
    {
        Undo_AddEntity_W( (entity_s *)edit_entity );
        DeleteKey( &edit_entity->epairs, key );
        Checkkey_Model( edit_entity, key );
        Checkkey_Color( edit_entity, key );
    }
    SetKeyValuePairs();
    Undo_End();
}

// DelProp (0x4975c0) — delete the key named in the key field (blocking "classname"),
// Undo-bracketed, with the binary's post-delete model/colour re-validation.
void DelProp()
{
    if ( !edit_entity )
        return;

    char key[4100];
    SendMessageA( entwnd_keyfield, WM_GETTEXT, 0xFFF, (LPARAM)key );

    EntDeleteKey_Apply( key );
}

// EditProp (0x4977b0) — the selected key/value list item → split on the tab into the
// key + value edit fields (so it can be edited and re-added).
void EditProp()
{
    if ( !edit_entity )
        return;

    LRESULT i = SendMessageA( entwnd_kvlist, LB_GETCURSEL, 0, 0 );
    if ( i < 0 )
        return;

    char sz[4096];
    SendMessageA( entwnd_kvlist, LB_GETTEXT, i, (LPARAM)sz );

    int j = 0;
    while ( sz[j] != '\t' )
        ++j;
    char *val = &sz[j + 1];
    sz[j] = '\0';
    if ( *val == '\t' )
        ++val;

    SendMessageA( entwnd_keyfield,   WM_SETTEXT, 0, (LPARAM)sz  );
    SendMessageA( entwnd_valuefield, WM_SETTEXT, 0, (LPARAM)val );
}

// ──────────────────────────────────────────────────────────────────────────────
//  SPAWNFLAGS — the 12 checkboxes.  The "spawnflags" key is a decimal int of OR'd
//  bits; checkbox i toggles bit i.  Three faithful IDB ports:
//   * SetSpawnFlags    (0x496e70) — flags → checkboxes (reflect on selection).
//   * SetSpawnFlags_R  (0x496f00) — one checkbox toggled → flags (per-entity, keeps
//                                   the OTHER bits when several entities are selected).
//   * SetSpawnFlags_2  (0x497040) — rebuild flags from ALL 12 checkbox states.
//  SetSpawnFlags_R(i) is what each checkbox click calls; it delegates the single-edit
//  case to SetSpawnFlags_2 (so a single entity is rebuilt wholesale, multi-edit is
//  per-bit).  The spawnflags string is a plain decimal atol/sprintf("%i"); the
//  bit math is (v & (1<<i)) / (1<<i)|v / ~(1<<i)&v — transcribed verbatim.

// Return entity e's "spawnflags" value as an int (0 if absent), without disturbing it.
static int SpawnFlags_Get( entity_s_def *e )
{
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
        if ( !_stricmp( ep->key, "spawnflags" ) )
            return (int)atol( ep->value );
    return 0;          // IDB: atol(zero) — the empty-CString sentinel
}

// UI-independent read behind the 12 spawnflag checkboxes' state (SetSpawnFlags 0x496e70's
// "spawnflags" read; 0 when nothing is being edited).
int SpawnFlags_Gather()
{
    return edit_entity ? SpawnFlags_Get( edit_entity ) : 0;
}

// SetSpawnFlags (0x496e70) — read edit_entity's spawnflags, push each bit into the
// matching checkbox's BM_SETCHECK state.
void SetSpawnFlags()
{
    if ( !edit_entity )
        return;
    int flags = SpawnFlags_Gather();
    for ( int i = 0; i < 12; ++i )
        SendMessageA( entwnd_entcheck[i], BM_SETCHECK, ( flags & ( 1 << i ) ) != 0, 0 );
}

// UI-independent action behind the 12 spawnflag checkboxes (SetSpawnFlags_2 0x497040's
// write half): `flags` becomes the entity's decimal "spawnflags" key.
void SpawnFlags_Apply( int flags )
{
    char sz[32];
    sprintf( sz, "%i", flags );

    Undo_ClearRedo();
    Undo_GeneralStart( "set spawnflags" );
    if ( multiple_edit_entities )
    {
        for ( selbrush_t *sb = selected_brushes.next; sb != &selected_brushes; sb = sb->next )
        {
            entity_s_def *def = (entity_s_def *)sb->owner->def;
            // IDB inlines Undo_AddEntity + the entity's brush-def walk here; that is
            // exactly Undo_AddEntity_W (already disasm-verified, the right field/sentinel).
            Undo_AddEntity_W( (entity_s *)def );
            SetKeyValue( def, "spawnflags", sz );
        }
    }
    else
    {
        Undo_AddEntity_W( (entity_s *)edit_entity );
        SetKeyValue( edit_entity, "spawnflags", sz );
    }
    SetKeyValuePairs();
    Undo_End();
}

// SetSpawnFlags_2 (0x497040) — collapse all 12 checkbox states into one int and write
// it as "spawnflags" on the edited entity (single) or every selected entity (multiple).
// 0x497040: vs IDA 0x497040 — flags = OR of (BM_GETCHECK<<i) for i in 0..11; sprintf "%i";
// Undo_ClearRedo + Undo_GeneralStart("set spawnflags"); multi → per-entity Undo_AddEntity_W (=
// the binary's inlined `if(g_lastundo) Undo_AddEntity + def->brushes.oprev brush-def walk /
// else Sys_Printf "no last undo"`, disasm-verified equiv) + SetKeyValue; single →
// Undo_AddEntity_W(edit_entity) + SetKeyValue; SetKeyValuePairs + Undo_End. SetSpawnFlags
// 0x496e70 (flags→12 BM_SETCHECK) + SpawnFlags_Get (atol of the spawnflags epair, ""
// sentinel→0) also match.
void SetSpawnFlags_2()
{
    int flags = 0;
    for ( int i = 0; i < 12; ++i )
        flags |= (int)SendMessageA( entwnd_entcheck[i], BM_GETCHECK, 0, 0 ) << i;

    SpawnFlags_Apply( flags );
}

// UI-independent action behind one spawnflag checkbox toggle in a MULTI-entity selection
// (SetSpawnFlags_R 0x496f00's per-entity bit math); `checked` is that box's BM_GETCHECK.
void SpawnFlagBit_Apply( int bit, int checked )
{
    Undo_ClearRedo();
    Undo_GeneralStart( "set spawnflags" );
    for ( selbrush_t *sb = selected_brushes.next; sb != &selected_brushes; sb = sb->next )
    {
        entity_s_def *def   = (entity_s_def *)sb->owner->def;
        int       flags = SpawnFlags_Get( def );
        int v8 = checked ? ( ( checked << bit ) | flags ) : ( ~( 1 << bit ) & flags );

        char sz[32];
        sprintf( sz, "%i", v8 );
        Undo_AddEntity_W( (entity_s *)def );
        SetKeyValue( def, "spawnflags", sz );
    }
    SetKeyValuePairs();
    Undo_End();
}

// SetSpawnFlags_R (0x496f00) — checkbox `bit` was toggled.  For a single entity this
// is just SetSpawnFlags_2 (rebuild from the boxes).  For multiple entities, only the
// toggled bit is changed PER ENTITY (their other bits, which can differ, are kept):
// read the entity's current flags, OR/AND-NOT bit `bit` from that checkbox's state.
// 0x496f00: vs IDA 0x496f00 — multi-edit per-entity bit math EXACT (v8 = BM_GETCHECK ?
// ((check<<bit)|flags) : (~(1<<bit)&flags); flags = atol(spawnflags epair) = SpawnFlags_Get;
// Undo_AddEntity_W + SetKeyValue per entity; SetKeyValuePairs + Undo_End). DIVERGENCE (port
// more correct than a latent quirk, documented): the binary calls Undo_ClearRedo +
// Undo_GeneralStart at the TOP (before the multiple/single split), so the single-entity path
// runs that outer bracket AND THEN SetSpawnFlags_2's own ClearRedo+GeneralStart+End — leaving
// the outer bracket ORPHANED (never Undo_End'd → an extra empty undo record). The port skips
// the outer bracket for the single case (early-return to SetSpawnFlags_2), producing the
// identical spawnflags result with clean undo bookkeeping. The multi-edit path's bracket is
// unchanged (matches the binary).
void SetSpawnFlags_R( int bit )
{
    if ( !multiple_edit_entities )
    {
        SetSpawnFlags_2();
        return;
    }

    // BM_GETCHECK on the toggled box: set the bit if checked, clear it otherwise.  (The
    // binary re-reads it inside the per-entity loop; the box cannot change mid-loop, so
    // hoisting the read to the caller side is value-identical.)
    int checked = (int)SendMessageA( entwnd_entcheck[bit], BM_GETCHECK, 0, 0 );
    SpawnFlagBit_Apply( bit, checked );
}


// UI-independent action behind the eclass list's double-click / the create-entity command
// (CreateEntity 0x497300's Entity_Create half).
void EclassCreate_Apply( const char *name )
{
    if ( !_stricmp( name, "worldspawn" ) )
    {
        MessageBoxA( GetActiveWindow(),
            "Failed to create entity.\n\nCan't create an entity with worldspawn.",
            "Radiant", MB_ICONEXCLAMATION );
        return;
    }

    eclass_t *pecNew = Eclass_ForName( 0, name );
    // Both POINT (fixed-size) and BRUSH entities are supported: Entity_Create's brush-
    // entity reparent loop reparents the selected world brushes into a
    // func_*/script_brushmodel entity.  (Was gated with a "not supported" message while
    // the reparent loop was a FATAL stub.)
    if ( Entity_Create( pecNew ) )
    {
        if ( selected_brushes.next == &selected_brushes )
        {
            edit_entity = (entity_s_def *)world_entity->def;
        }
        else
        {
            edit_entity = (entity_s_def *)selected_brushes.next->owner->def;
            selbrush_t *first = selected_brushes.next;
            Select_Deselect( 1 );
            Select_Brush( first, 1, 1, 0 );
        }
        SetKeyValuePairs();
        g_nUpdateBits = -1;
    }
    else
    {
        MessageBoxA( GetActiveWindow(), "Failed to create entity.\n", "Radiant", MB_ICONEXCLAMATION );
    }
}

// CreateEntity (0x497300) — create a new entity of the eclass selected in the list,
// using the currently-selected brush(es) as the proxy (Entity_Create), then re-select
// and refresh.  (Mirrors the binary; the GUI also reaches this via the eclass DBLCLK.)
void CreateEntity()
{
    if ( selected_brushes.next == &selected_brushes )
    {
        MessageBoxA( GetActiveWindow(),
            "Failed to create entity.\n\nYou must have a selected brush to create an entity",
            "Radiant", MB_ICONEXCLAMATION );
        return;
    }

    LRESULT i = SendMessageA( hwndEnt_entlist, LB_GETCURSEL, 0, 0 );
    if ( i < 0 )
    {
        MessageBoxA( GetActiveWindow(),
            "Failed to create entity.\n\nYou must have a selected class to create an entity",
            "Radiant", MB_ICONEXCLAMATION );
        return;
    }

    char name[1028];
    SendMessageA( hwndEnt_entlist, LB_GETTEXT, i, (LPARAM)name );

    EclassCreate_Apply( name );
}

// The eclass-driven inspector fields: the description-box text and the first 8 spawnflag
// checkbox labels (an empty name means that box is blanked + disabled).
struct entEclassInfo_t
{
    const char *comment;
    const char *flagname[8];
};

// UI-independent read behind the description box + the first 8 spawnflag checkbox labels
// (UpdateSelection 0x497180's comments + flag-name walk).
void EclassInfo_Gather( eclass_t *cls, entEclassInfo_t &out )
{
    out.comment = TranslateString( cls->comments );

    // The eclass flag names (flagname0..flagname7, 32-byte stride), exactly as the IDB
    // loop reads them (it stops at checkbox 8 = &entcheck_easy; the remaining 4 —
    // easy/medium/hard/deathmatch — keep their static labels).
    const char *flagname = cls->flagname0;
    for ( int i = 0; i < 8; ++i, flagname += 32 )
        out.flagname[i] = flagname;
}

// UpdateSelection (0x497180) — the selection→inspector refresh.  edit_entity becomes
// the selected entity's def (or worldspawn if nothing selected); the eclass list cursor
// + description follow; the key/value list is rebuilt.  wParam==-1 means "derive the
// eclass from edit_entity" (selection change); otherwise it is an explicit list index
// (the eclass-list selchange).  The 8 spawnflag checkboxes are PARKED.
int UpdateSelection( int wParam, eclass_t *cls )
{
    // ── KIWI-UX (ROUND AU): edit_entity IS NOT A GUI FIELD ───────────────────
    // USER DIRECTIVE: "make sure the legacy entity inspector (N) bind works so we
    // can change their properties."  It could not, and this guard is the whole of
    // why.  `hwndEnt_entlist` is the MFC CEntityWnd's eclass LISTBOX and it is
    // never assigned in this shell (win_ent.cpp:86 initialises it to nullptr and
    // nothing writes it — the MFC inspector is gone).  So this function returned
    // at line one on EVERY selection change, and `edit_entity` — the def every one
    // of the *_Apply editors writes through (EntSetKey_Apply :278, EntDeleteKey_
    // Apply :358, SpawnFlags_Apply :476, EntAngle_Apply :908) — stayed at whatever
    // EclassCreate_Apply last left it, i.e. nullptr for a session that never
    // created an entity.  The ImGui inspector therefore printed "(no entity
    // selected)" forever, with a perfectly good selection sitting in front of it.
    //
    // The three lines below are the SAME three the binary runs (they are just
    // below, unchanged, for the MFC path) — hoisted ABOVE the port guard so the
    // two globals track the selection with or without a listbox.  Nothing else is
    // hoisted: the LB_* calls that follow really do need the window, and the
    // remaining PARKED spawnflag work is untouched.  Ordering note: this is a
    // plain pair of global writes, not a subsystem setter, so hoisting it cannot
    // reach anything the way a ported setter replayed at a different phase would.
    //
    // The two NULL tests are the price of running BEFORE the guard rather than
    // after it: the binary reaches this code only from a live GUI, where the
    // sentinel is linked and worldspawn exists, and this path now also runs during
    // boot / Map_Free, where neither is guaranteed (map.cpp:264 nulls
    // world_entity).  Neither test can fire once a map is up.
    if ( selected_brushes.prev )
    {
        selbrush_t *sel = selected_brushes.prev;
        if ( sel == &selected_brushes )
        {
            edit_entity            = world_entity ? (entity_s_def *)world_entity->def
                                                  : nullptr;
            multiple_edit_entities = 0;
        }
        else
        {
            edit_entity            = (entity_s_def *)sel->owner->def;
            multiple_edit_entities = ( sel->prev != &selected_brushes );
        }
    }

    if ( !hwndEnt_entlist )       // the window is not up (headless / selftest) — PORT guard
        return 1;

    // IDA 0x497180 reads selected_brushes.prev (the TAIL = the LAST-selected brush), NOT .next
    // (the head / first-selected): the inspector reflects the MOST-RECENTLY-selected entity.
    // [the port read .next (first-selected) — for a multi-ENTITY selection the
    //  inspector showed the wrong entity. selected_brushes is appended at .prev (Brush_AddToList2),
    //  so .prev is the last selection.]  The binary's first statement is the 623 list-init assert.
    iassert( selected_brushes.prev );   // win_ent.cpp:623 (L0) — RESTORED
    selbrush_t *sel = selected_brushes.prev;
    entity_s_def *ent;
    if ( sel == &selected_brushes )
    {
        ent = (entity_s_def *)world_entity->def;
        multiple_edit_entities = 0;
    }
    else
    {
        ent = (entity_s_def *)sel->owner->def;
        multiple_edit_entities = ( sel->prev != &selected_brushes );
    }
    edit_entity = ent;

    if ( wParam != -1
         || ( cls = ent->eclass,
              wParam = (int)SendMessageA( hwndEnt_entlist, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)cls->name ),
              wParam != -1 ) )
    {
        SendMessageA( hwndEnt_entlist, LB_SETCURSEL, wParam, 0 );
    }

    if ( cls )
    {
        entEclassInfo_t info;
        EclassInfo_Gather( cls, info );

        if ( entwnd_comment )
            SendMessageA( entwnd_comment, WM_SETTEXT, 0, (LPARAM)info.comment );

        // Label + enable the first 8 checkboxes from the eclass flag names.  An empty
        // name → blank " " + disabled, exactly as the IDB loop.
        for ( int i = 0; i < 8; ++i )
        {
            HWND checkbox = entwnd_entcheck[i];
            const char *flagname = info.flagname[i];
            if ( flagname && *flagname )
            {
                EnableWindow( checkbox, TRUE );
                SendMessageA( checkbox, WM_SETTEXT, 0, (LPARAM)flagname );
            }
            else
            {
                SendMessageA( checkbox, WM_SETTEXT, 0, (LPARAM)" " );
                EnableWindow( checkbox, FALSE );
            }
        }
        SetSpawnFlags();                 // reflect this entity's spawnflags into the boxes
        SetKeyValuePairs();

        // IDA tail (0x4972a9) — RESTORED 2026-07-31. Reflects the selection's
        // script_flag_true/false keys into the script-group (media) window's two flag
        // listboxes. Was parked on two then-missing pieces; both landed with the
        // script-group unit: ScriptGroup_HasFlag (0x454e40, scriptgroup.cpp) and
        // d_hwndMedia (created by the script-group dialog's WM_INITDIALOG).
        // IsWindowVisible(NULL) is FALSE, so this stays inert headless, as in the binary.
        if ( ::IsWindowVisible( g_qeglobals.d_hwndMedia ) )
        {
            ScriptGroup_HasFlag( "script_flag_true",  1671, g_qeglobals.d_hwndMedia );
            ScriptGroup_HasFlag( "script_flag_false", 1298, g_qeglobals.d_hwndMedia );
        }
    }
    return 1;
}

// UI-independent action behind the eclass list's selection change (the entity WndProc's
// eclass LBN_SELCHANGE arm): remember the picked class name (the RMB create reads it),
// then refresh the inspector from that class.
void EclassSelect_Apply( int listIndex, eclass_t *pec )
{
    if ( pec && pec->name )
    {
        strncpy( s_selectedEclass, pec->name, sizeof( s_selectedEclass ) - 1 );
        s_selectedEclass[sizeof( s_selectedEclass ) - 1] = '\0';
    }
    UpdateSelection( listIndex, pec );        // description + this entity's key/values
}

// Public entry for the selection hooks (= UpdateSelection(-1, NULL)); a no-op when the
// entity window is not present (headless / selftest builds), so the round-trip gates
// keep their proven path.
void Entity_UpdateSelection()
{
    UpdateSelection( -1, nullptr );
}

// TASK 2 — the eclass name the entity window currently has picked (RMB create reads it).
const char *Ed_SelectedEclassName()
{
    return s_selectedEclass;
}

// ──────────────────────────────────────────────────────────────────────────────
//  THE MODEL / PREFAB PICKER — CEntityWnd_OpenModelDialog (0x497d20, IDB
//  "OpenDialog").  Wired to IDC_E_ADD_MODEL: when a misc_model/script_*/dyn_model
//  (or misc_prefab) entity is selected, the entity window pops a file dialog over
//  the model (or prefab) directory; the picked file is relativised to a bare xmodel
//  name and set as the entity's "model" epair (→ EntityAssignModel → the bbox).
//
//  The binary's flow (verified vs the disasm):
//   * read g_qeglobals.d_project_entity's `key` epair (basepath / mapspath) → the
//     initial directory = <value>\<subdir>;
//   * pop a CModelFileDialog (a CFileDialog subclass that adds a MODEL-PREVIEW pane
//     via a custom template — the preview pane is PARKED here; a plain CFileDialog
//     gives the identical pick result);
//   * on OK: lowercase the picked path + '\'→'/', then relativise — find the
//     `subdir`-tail token ("xmodel"/"prefabs") inside the path and drop everything
//     before it; for models (stripToBareName) also drop the leading "xmodel/" so the
//     stored value is the bare model name the asset system expects;
//   * SendMessage "model" → key field, name → value field, then AddProp()
//     (= SetKeyValue("model", name) on edit_entity), then bump version / clear
//     modelClass + the brush's modelFailed flag and refocus the XY view.
//
//  THE SPLIT (so the headless model_gate can exercise the real commit without a file
//  dialog — exactly as RunSetKeyTest drives SetKeyValue directly, not the GUI fields):
//   * Ed_RelativizeModelPath(full, subdir, stripToBareName) — the pure path math.
//   * Ed_CommitPickedModel(def, relName) — the picker's NET EFFECT on the entity
//     (SetKeyValue("model",name) + the version/modelClass/brush-flag tail).  This is
//     what the gate calls; the GUI picker calls it after the CFileDialog returns.
// ──────────────────────────────────────────────────────────────────────────────

// Ed_RelativizeModelPath — turn an absolute picked path into the stored "model"
// value, faithful to OpenDialog's lowercase + slashify + find-subdir-token + strip.
// `subdirToken` is the leaf of the search subdir ("xmodel" or "prefabs"); the binary
// searches for the LOWERCASE token, so callers pass it lowercase.  Returns the
// relative form (a copy in `out`, capped at outSz).  Mirrors the two cstr_find loops.
void Ed_RelativizeModelPath( const char *fullPath, const char *subdirToken,
                             bool stripToBareName, char *out, size_t outSz )
{
    char buf[1024];
    // lowercase + backslash→slash (the binary does both, on both the path and the
    // token, before cstr_find).
    size_t n = 0;
    for ( const char *p = fullPath; *p && n < sizeof( buf ) - 1; ++p, ++n )
        buf[n] = ( *p == '\\' ) ? '/' : (char)tolower( (byte)*p );
    buf[n] = '\0';

    char tok[64];
    size_t tn = 0;
    for ( const char *p = subdirToken; *p && tn < sizeof( tok ) - 1; ++p, ++tn )
        tok[tn] = ( *p == '\\' ) ? '/' : (char)tolower( (byte)*p );
    tok[tn] = '\0';

    // Relativise: drop everything up to (and including) the subdir token.  The binary
    // finds the token; if present (and not already at index 0) it Mid()s the string to
    // start at the token.  Here we point past the token + its trailing slash.
    const char *rel = buf;
    char *hit = ( tok[0] ? strstr( buf, tok ) : nullptr );
    if ( hit )
    {
        rel = hit + strlen( tok );
        if ( *rel == '/' ) ++rel;          // skip the slash after "xmodel"
        // stripToBareName (models): the value is the bare leaf the asset system wants
        // (e.g. "ac_car_part01"), so after stripping "xmodel/" we already have it —
        // but if the model lives in a deeper subdir the binary keeps the remainder.
        // (OpenDialog's a4 path additionally drops a leading dir segment if one
        // remains before the first '/'; we keep the post-"xmodel/" remainder, which is
        // the bare name for the flat raw\xmodel layout and the relative name otherwise.)
        (void)stripToBareName;
    }

    // Copy out.
    size_t i = 0;
    for ( ; rel[i] && i < outSz - 1; ++i )
        out[i] = rel[i];
    out[i] = '\0';
}

// Ed_CommitPickedModel — set `relName` as the entity's "model" key and run the
// picker's post-set tail (faithful to OpenDialog's AddProp + version/modelClass/
// brush-flag cleanup).  Undo-bracketed like AddProp.  Headless-safe (no HWNDs).
void Ed_CommitPickedModel( entity_s_def *def, const char *relName )
{
    if ( !def || !relName )
        return;

    Undo_ClearRedo();
    Undo_GeneralStart( "set key value pair" );      // AddProp's bracket
    Undo_AddEntity_W( (entity_s *)def );
    SetKeyValue( def, "model", relName );            // → Checkkey_Model + EntityAssignModel
    Undo_End();

    // OpenDialog's tail (0x498119..): SetKeyValue already ran Checkkey_Model's
    // version++/modelClass=NULL/brush.unk01=0 for the "model" key, so this is the
    // binary's belt-and-suspenders repeat — kept verbatim.
    ++def->version;
    def->modelClass = nullptr;
    brush_t *b = (brush_t *)def->def;   // brushes.oprev == the def-list brush
    if ( b != (brush_t *)&def->def )
        b->unk01 = 0;
}

// Ed_PostAddModelCommand — CreateEntityFromName's misc_model tail posts WM_COMMAND
// IDC_E_ADD_MODEL to the entity window (the binary uses the resource ID 1294; this
// hand-built window uses its own control ID IDC_ENT_ADD_MODEL_BTN).  HIWORD 0 =
// BN_CLICKED, so MFC routes it to CEntityWnd::OnAddModel.  No-op (headless / no
// entity window) — the model-less bbox stands and the gate drives the commit core.
void Ed_PostAddModelCommand()
{
    // wParam = MAKEWPARAM(id, BN_CLICKED); BN_CLICKED==0 so the HIWORD notification code
    // is 0 and wParam is just the control id.  (MAKEWPARAM/MAKELONG aren't reliably in
    // scope in this TU's macro set — build it explicitly.)
    if ( g_qeglobals.d_hwndEntity )
        PostMessageA( g_qeglobals.d_hwndEntity, WM_COMMAND,
                      (WPARAM)( (ushort)IDC_ENT_ADD_MODEL_BTN
                                | ( (unsigned long)( BN_CLICKED ) << 16 ) ), 0 );
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFC shell — the model picker + the whole CEntityWnd window class.  Everything above
//  (edit_entity / multiple_edit_entities / the *_Gather + *_Apply cores / SetKeyValuePairs /
//  Win_Get|SetEntityKeyValueFields / Ed_RelativizeModelPath / Ed_CommitPickedModel /
//  Ed_PostAddModelCommand / UpdateSelection / Ed_SelectedEclassName) stays COMMON —
//  imgui_panel_entity.cpp and imgui_panel_kvselect.cpp drive those.  The child-control
//  HWND statics also stay common (plain Win32; simply NULL in the ON build, which every
//  reader already guards).
// ══════════════════════════════════════════════════════════════════════════════

void Radiant_RefreshFilterPane()
{
    // NO-MFC: no-op — CEntityWnd is the MFC inspector; the ImGui filters/entity panels
    // re-gather from the filter lists themselves.
}


// An angle/direction button was clicked.  Faithful to the WndProc IDC_ANGLE* cases:
// the 8 compass buttons set the YAW component (axis 1) to a fixed heading; Up/Down
// toggle the PITCH component (axis 0) to -90 / +90 (CoD pitch convention: negative
// pitch looks up, positive looks down — NOT the GtkRadiant -1/-2 `angle` special
// values, which this CoD build does not use).  All cases refresh the kv list.
// UI-independent action behind the inspector's 10 angle/direction buttons.
void EntAngle_Apply( int idx )
{
    switch ( idx )
    {
        case 0: Entity_SetAngles(   0.0f, 1 ); break;   // E   (IDC_ANGLE360)
        case 1: Entity_SetAngles(  45.0f, 1 ); break;   // NE
        case 2: Entity_SetAngles(  90.0f, 1 ); break;   // N
        case 3: Entity_SetAngles( 135.0f, 1 ); break;   // NW
        case 4: Entity_SetAngles( 180.0f, 1 ); break;   // W
        case 5: Entity_SetAngles( 225.0f, 1 ); break;   // SW
        case 6: Entity_SetAngles( 270.0f, 1 ); break;   // S
        case 7: Entity_SetAngles( 315.0f, 1 ); break;   // SE
        case 8:                                         // Up — toggle pitch -90/0
        {
            // sub_485570 zeroes the vec on a missing key (Entity_GetVec3ForKey does
            // NOT), so pre-zero — else ang[0] is stack garbage for an angle-less entity.
            float ang[3] = { 0.0f, 0.0f, 0.0f };
            Entity_GetVec3ForKey( edit_entity, ang, "angles" );
            Entity_SetAngles( ( ang[0] == -90.0f ) ? 0.0f : -90.0f, 0 );
            break;
        }
        case 9:                                         // Down — toggle pitch +90/0
        {
            float ang[3] = { 0.0f, 0.0f, 0.0f };
            Entity_GetVec3ForKey( edit_entity, ang, "angles" );
            Entity_SetAngles( ( ang[0] == 90.0f ) ? 0.0f : 90.0f, 0 );
            break;
        }
        default:
            return;
    }
    SetKeyValuePairs();                                 // reflect the new "angles" key
}


// CEntityWnd_SetInspectorMode (0x496b00) — switch the right-column inspector between
// Entity / Textures / Console / Filters.  FAITHFUL state machine (matching the binary):
//   * the m_nCurrentStyle gate: in styles 0/3 (floating/4-pane) switching to Textures or
//     Console is a no-op (those are separate windows there).  This port is style 1
//     (integrated) so all switches apply.
//   * sets inspector_mode (0x240a110), the d_hwndEntity window title (Entity/Textures/
//     Console/Filters), and enables ID_MISC_SELECTENTITYCOLOR only in Entity mode
//     (EnableMenuItem MF_GRAYED otherwise — 3u = MF_DISABLED|MF_GRAYED).
//   * "show one pane-group at a time": LayoutForMode shows only the active mode's controls
//     (entity group in Entity mode, filter group in Filter mode) across the full client
//     area, mirroring the binary's column-swap.  The switch is driven by the N/O/T/F
//     hotkeys + the View menu (no tab control — the binary's tab lived in a dialog resource
//     this port cannot subclass; the invented WC_TABCONTROL was removed).
void CEntityWnd_SetInspectorMode( int mode )
{
    // NO-MFC: the style gate reads CMainFrame::m_nCurrentStyle (MFC); this shell is the
    // integrated style-1 layout, which is the value the MFC build also uses here, so the
    // gate below evaluates identically.
    const int style = 1;
    if ( ( style == 0 || style == 3 ) &&
         ( mode == INSPECTOR_TEXTURE || mode == INSPECTOR_CONSOLE ) )
        return;

    inspector_mode = mode;

    const char *title = "Entity";
    switch ( mode )
    {
    case INSPECTOR_TEXTURE: title = "Textures"; break;
    case INSPECTOR_CONSOLE: title = "Console";  break;
    case INSPECTOR_FILTER:  title = "Filters";  break;
    default:                title = "Entity";   break;
    }
    if ( g_qeglobals.d_hwndEntity )
        SetWindowTextA( g_qeglobals.d_hwndEntity, title );

    // Enable the "Select Entity Color" menu item only in Entity mode (binary: MF_ENABLED
    // for W_ENTITY, MF_GRAYED otherwise).  ID_MISC_SELECTENTITYCOLOR = 0x810C = 33036.
    if ( g_qeglobals.d_hwndMain )
    {
        HMENU menu = GetMenu( g_qeglobals.d_hwndMain );
        if ( menu )
            EnableMenuItem( menu, 33036,
                            ( mode == INSPECTOR_ENTITY ) ? MF_ENABLED
                                                         : ( MF_DISABLED | MF_GRAYED ) );
    }

    // Apply the show/hide: only the active mode's controls are visible (entity group in
    // Entity mode, filter group in Filter mode); the inspector body repaints.  This port
    // keeps the texture browser + console in their own always-visible fixed slots (the
    // working layout), so a non-Entity/non-Filter mode simply empties the inspector body +
    // flips the title/mode; their panes remain accessible in their slots.  NOTE: deliberately
    // NOT raising/focusing the D3D render panes here — BringWindowToTop/SetFocus on a live
    // swap-chain window destabilises the renderer (it forced a full-window texture render),
    // and we keep the proven layout stable over a faithful-but-risky reposition.
    // NO-MFC: per-mode show/hide skipped — LayoutForMode is a CEntityWnd method; the mode
    // itself (inspector_mode + the window title + the menu enable above) still updates, and
    // the ImGui panels are per-pane windows, so there is no column to swap.
}

// KISAK divergences in this file, all deliberate:
//   0x4978a0 CEntityWnd_SizeEntityDlg — the binary column-swaps whole panes (Entity / Texture /
//            Console / Filter = d_hwndGroup = CFilterWnd) in and out of the client area via
//            entwnd_col.  This port lays the controls out proportionally and show/hides only
//            the active mode's group over the full client area, keeping the texture browser and
//            console in their own fixed slots.
//   NO TAB CONTROL: the binary's mode tab is a CTabCtrl (g_wndTabsEntWnd) subclassed from
//            IDC_E_TAB_CONTROL in the IDD_ENTITY resource (GetEntityControls 0x4964b0), which a
//            resource-less port cannot reproduce.
//   CEntityWnd_SetInspectorMode does NOT raise/focus the D3D render panes — BringWindowToTop /
//            SetFocus on a live swap-chain window destabilises the renderer.
//   W_GROUP (0x800) has no body in the binary's SetInspectorMode either (the "Group" pane is
//            dead in this build; OnViewGroups 0x42b700 has no menu command).
//   The CModelFileDialog model-PREVIEW pane (custom template, tmpl id 2) is not reproduced;
//            the picker is a plain CFileDialog.

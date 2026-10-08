#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\win_ent.cpp
//
// THE ENTITY WINDOW (CEntityWnd) — the entity inspector.  Two halves of the QE4
// lineage (the 0x496370..0x4981f0 cluster):
//   * the eclass LIST  (FillClassList 0x496800, from g_eclass)
//   * the KEY/VALUE editor (AddProp 0x497490 / DelProp 0x4975c0 cores; KIWI: the
//     listbox + edit fields are the ImGui entity panel's)
//   * create-any-eclass (CreateEntity 0x497300 → Entity_Create)
//   * UpdateSelection (0x497180) — the selection→inspector refresh, driven from
//     Brush_AddToList2 / Brush_RemoveFromList / Select_Deselect.
//
// The binary builds the controls from a CreateDialog(IDD_ENTITY) template +
// SetParent (GetEntityControls 0x4964b0).  radiant.rc carries no dialog templates
// (only the reconstructed menu/accelerators), so the window PLUMBING here is MFC:
// CEntityWnd : CWnd creates the child controls (two listboxes + two edit fields +
// a Delete button + the 12 spawnflag checkboxes) directly and routes their
// notifications.  The DATA operations:
//   * the 12 SPAWNFLAG checkboxes (SetSpawnFlags 0x496e70 / _R 0x496f00 / _2 0x497040)
//     — each box toggles a bit in the entity's decimal "spawnflags" key; the first 8
//     labels come from the eclass flagname0..7.
// KIWI: the ANGLE-button grid (Entity_SetAngles 0x494030) is gone; the ImGui entity panel
// edits "angles" directly.
// The model/prefab picker (OpenDialog / IDC_E_ADD_MODEL) and the SizeEntityDlg pixel
// layout both SHIPPED; only the binary's CModelFileDialog preview PANE is still out
// (a plain CFileDialog gives the identical pick — see CEntityWnd_OpenModelDialog).
//
// NO TAB STRIP.  The binary's inspector carries a mode-switch tab (a CTabCtrl
// g_wndTabsEntWnd, subclassed from IDC_E_TAB_CONTROL in the IDD_ENTITY dialog resource),
// with the panes column-swapped in/out by SizeEntityDlg (0x4978a0)'s entwnd_col trick.
// This port has no dialog resource to subclass, so mode switching stays on the N/O/T/F
// hotkeys + the View menu (CEntityWnd_SetInspectorMode), and each mode lays its own
// controls across the full client area from the top (no inset).

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include "kiwi_ported.h"   // KIWI: shared engine/editor externs
#include <vector>

// ── radiant-local deps (home files) ───────────────────────────────────────────
extern eclass_t   *g_eclass;                          // eclass.cpp   (0x25D5B20)

extern void        Entity_RebuildBounds( entity_s *e );                                  // entity.cpp 0x485390

// ── entity-window globals (file-scope state @ 0x240A1xx) ───────────────────────
entity_s_def *edit_entity            = nullptr;   // 0x240A108  currently-edited def
int           multiple_edit_entities = 0;         // 0x240A10C  >1 entity selected

// inspector_mode (0x240A110): the active inspector pane.  Initialised to
// INSPECTOR_ENTITY (the binary's default — the entity editor is shown at startup).
int           inspector_mode         = INSPECTOR_ENTITY;

// KIWI: the MFC inspector's child-control HWNDs (eclass list, key/value list + fields, the 12
// spawnflag checkboxes, the CFilterWnd lists) are gone - nothing created them, so every
// reader saw NULL.  The ImGui entity / filter / key-value panels read and write through the
// *_Gather / *_Apply cores below.

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
//  Data operations
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

// ═══ ENTITY KEY/VALUE CLUSTER ════════════════════════════════════════════════════════════════
//  AddProp 0x497490's set half (EntSetKey_Apply) and DelProp 0x4975c0's delete half
//  (EntDeleteKey_Apply) are the cores; the ImGui entity panel draws the key/value list itself.
//  KIWI: SetKeyValuePairs 0x496cf0 (the listbox refill after every edit), AddProp's WM_GETTEXT
//  reads and the field accessors went with the MFC controls - they only ever saw NULL HWNDs.
// ════════════════════════════════════════════════════════════════════════════════════════════

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
            if ( !strcmp( "origin", key ) )     // KIWI: as the single edit - else save wrote the old origin
            {
                Entity_GetVec3ForKey( def, def->origin, "origin" );
                ++def->version;
                if ( def->eclass && def->eclass->fixedsize )
                    Entity_RebuildBounds( (entity_s *)def );   // KIWI: the box / picking follow
            }
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
            if ( edit_entity->eclass && edit_entity->eclass->fixedsize )
                Entity_RebuildBounds( (entity_s *)edit_entity );   // KIWI: the box / picking follow
        }
    }
    Undo_End();
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
            Checkkey_Model( def, key );      // 0x482F70
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
    Undo_End();
}

// ──────────────────────────────────────────────────────────────────────────────
//  SPAWNFLAGS — the 12 checkboxes.  The "spawnflags" key is a decimal int of OR'd
//  bits; checkbox i toggles bit i.
//   * SetSpawnFlags_R  (0x496f00) — one checkbox toggled → flags (per-entity, keeps
//                                   the OTHER bits when several entities are selected).
//   * SetSpawnFlags_2  (0x497040) — rebuild flags from ALL 12 checkbox states.
//  KIWI: the MFC checkboxes are gone (SetSpawnFlags 0x496e70 pushed the flags into them); the
//  panels read SpawnFlags_Gather and call SpawnFlagBit_Apply (_R's per-bit core) and
//  SpawnFlags_Apply (_2's write half) directly.  The spawnflags string is a plain decimal
//  atol/sprintf("%i"); the bit math is (v & (1<<i)) / (1<<i)|v / ~(1<<i)&v.

// Return entity e's "spawnflags" value as an int (0 if absent), without disturbing it.
static int SpawnFlags_Get( entity_s_def *e )
{
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
        if ( !_stricmp( ep->key, "spawnflags" ) )
            return (int)atol( ep->value );
    return 0;          // absent key
}

// UI-independent read behind the 12 spawnflag checkboxes' state (SetSpawnFlags 0x496e70's
// "spawnflags" read; 0 when nothing is being edited).
int SpawnFlags_Gather()
{
    return edit_entity ? SpawnFlags_Get( edit_entity ) : 0;
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
            // Undo_AddEntity_W covers the binary's inlined Undo_AddEntity + brush-def walk
            // (the right field/sentinel).
            Undo_AddEntity_W( (entity_s *)def );
            SetKeyValue( def, "spawnflags", sz );
        }
    }
    else
    {
        Undo_AddEntity_W( (entity_s *)edit_entity );
        SetKeyValue( edit_entity, "spawnflags", sz );
    }
    Undo_End();
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
    Undo_End();
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
    // func_*/script_brushmodel entity.
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
        g_nUpdateBits = -1;
    }
    else
    {
        MessageBoxA( GetActiveWindow(), "Failed to create entity.\n", "Radiant", MB_ICONEXCLAMATION );
    }
}

// UpdateSelection (0x497180) — the selection→inspector refresh: edit_entity becomes the
// LAST-selected entity's def (selected_brushes.prev, the tail - Brush_AddToList2 appends
// there), or worldspawn's when nothing is selected; multiple_edit_entities notes a multi-
// entity selection.  Every *_Apply editor writes through edit_entity.
// KIWI: the binary then moved the MFC eclass-list cursor, the description box, the 8
// spawnflag checkbox labels and the key/value list - those controls are gone (the ImGui
// panels gather their own each frame), so wParam / cls (the list index / class) are unused.
// The NULL tests cover boot and Map_Free, which the binary's GUI-only path never saw
// (map.cpp nulls world_entity).
int UpdateSelection( int wParam, eclass_t *cls )
{
    (void)wParam;
    (void)cls;
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
//  (edit_entity / multiple_edit_entities / the *_Gather + *_Apply cores /
//  Ed_PostAddModelCommand / UpdateSelection) stays COMMON — imgui_panel_entity.cpp drives
//  those.
// ══════════════════════════════════════════════════════════════════════════════

void Radiant_RefreshFilterPane()
{
    // NO-MFC: no-op — CEntityWnd is the MFC inspector; the ImGui filters/entity panels
    // re-gather from the filter lists themselves.
}



// CEntityWnd_SetInspectorMode (0x496b00) — switch the right-column inspector between
// Entity / Textures / Console / Filters.  State machine:
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
    // and the proven layout stays stable.
    // NO-MFC: per-mode show/hide skipped — LayoutForMode is a CEntityWnd method; the mode
    // itself (inspector_mode + the window title + the menu enable above) still updates, and
    // the ImGui panels are per-pane windows, so there is no column to swap.
}

// Deliberate divergences from the binary:
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

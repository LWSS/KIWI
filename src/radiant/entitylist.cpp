#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Entity browser: groups map entities by classname, shows definition epairs, and
// selects the chosen instance's brushes.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"
#include <commctrl.h>
#include <stdio.h>
#include <map>
#include <string>
#include <vector>

// ── map entity instance list (entity.cpp / map.cpp) ──────────────────────────
extern entity_s   entityInsts;        // 0x23F1748 — entity-instance list sentinel (ring)

// ── selection core (select.cpp) ──────────────────────────────────────────────
extern void       Select_Deselect( int bDeselectFaces );                    // 0x48E800
extern void       Select_Brush( selbrush_t *b, char overwrite, char status, char center ); // 0x48DCC0
extern int        g_nUpdateBits;      // engine_stubs (0x25D5A74)

// MFC shell — the hand-built popup instance (NULL until opened) + its control ids.

// ══════════════════════════════════════════════════════════════════════════════
//  DATA CORE — headless-safe walks of the live entity list (no HWNDs).  Used by
//  the dialog to fill the controls.
// ══════════════════════════════════════════════════════════════════════════════

// Resolve the definition that owns an instance's classname and epairs.
static inline entity_s_def *EL_Def( entity_s *inst )
{
    return inst ? (entity_s_def *)inst->def : nullptr;
}

// The classname shown in the tree for an entity (the binary uses def->eclass->name
// verbatim; empty entities can't reach here because entityInsts only holds realized
// instances, but guard anyway).
static const char *EL_ClassName( entity_s *inst )
{
    entity_s_def *def = EL_Def( inst );
    if ( def && def->eclass && def->eclass->name )
        return def->eclass->name;
    return "";
}

// One tree row: the classname the group node and its leaf are both labelled with, and the
// instance the leaf carries as its item data.
struct entListEntry_t
{
    std::string  className;
    entity_s    *inst;
};

// One key/value row of the epair list.
struct entListPair_t
{
    const char *key;
    const char *value;
};

// UI-independent read behind the entity tree's population (InsertItems 0x40F7D0's entityInsts walk).
void EntityList_Gather( std::vector<entListEntry_t> &rows )
{
    for ( entity_s *ent = entityInsts.next; ent && ent != &entityInsts; ent = ent->next )
    {
        entListEntry_t row;
        row.className = EL_ClassName( ent );
        row.inst      = ent;
        rows.push_back( row );
    }
}

// UI-independent read behind the K/V list's population (UpdateKeyValuePairs 0x40F990's epair walk).
void EntityEpairs_Gather( entity_s *inst, std::vector<entListPair_t> &rows )
{
    entity_s_def *def = EL_Def( inst );
    if ( !def )
        return;

    for ( epair_t *e = def->epairs; e; e = e->next )
    {
        entListPair_t row;
        row.key   = e->key   ? e->key   : "";
        row.value = e->value ? e->value : "";
        rows.push_back( row );
    }
}

// ══════════════════════════════════════════════════════════════════════════════
//  EntityListDlg::SelectItem (0x40F750) essence — select the picked entity's brushes.
//  `inst` is the entity_s* stored as the tree leaf's item data.  Deselect everything,
//  then add every brush in the entity's owner list to the selection.  g_nUpdateBits=-1.
//  Headless-safe (no HWNDs); the dialog passes the GetItemData result, the gate passes
//  an entity directly.
// ══════════════════════════════════════════════════════════════════════════════
// UI-independent action behind CEntityListDlg's Select button / tree double-click.
void EntListSelect_Apply( entity_s *inst )
{
    // IDA SelectItem 0x40F750: Select_Deselect + the brush-walk run ONLY when the tree item's
    // data (entity_s*) is non-null.  A caret on a classname GROUP node (inserted without
    // TVIF_PARAM, so lParam==0) is a no-op and must not wipe the selection.
    if ( inst )
    {
        Select_Deselect( 1 );
        // head = &inst->brushes (the embedded entity_brush_s sentinel @+0x0C); the owner brush
        // list is linked via ownerNext until it wraps back to the sentinel.
        selbrush_t *head = &inst->brushes;
        for ( selbrush_t *b = head->ownerNext; b && b != head; b = b->ownerNext )
            Select_Brush( b, 1, 1, 1 );
    }
    g_nUpdateBits = -1;
}

// ══════════════════════════════════════════════════════════════════════════════
//  MFC shell — THE HAND-BUILT POPUP (CWnd), the binary's CEntityListDlg; the data comes
//  from the cores above (faithful to InsertItems / UpdateKeyValuePairs), which stay
//  COMMON for the dock-tab entity list the ImGui shell will host.
// ══════════════════════════════════════════════════════════════════════════════


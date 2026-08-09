#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\MapInfo.cpp — Edit→Map Info (menu 32786, CMainFrame::OnEditMapinfo
// 0x426C60): a read-only 9-row x 3-column (World / Prefabs / Total) statistics grid plus a
// per-classname entity listbox.  MapInfo_01 0x42EEA0 counts, MapInfo_02 0x42F030 tallies
// classnames into a CMap, MapInfo_03_Item 0x42F1C0 writes one grid cell, MapInfoDialog
// 0x42F230 is the OnInitDialog body (here MapInfo_PopulateDialog).
//
// radiant.rc carries no IDD_DLG_MAPINFO template, so — like CVehicleDlg / CDynEntityDlg —
// the shell is HAND-BUILT: a CWnd popup whose children are the 27 count statics (ids
// 1490..1516) and the listbox (id 1013).  The binary's dialog is modal and recomputes on
// every DoModal; this one recomputes on every open.  A snapshot view with no input, so
// modal-vs-modeless is not observable.

#include "stdafx.h"
#include "qe3.h"
#include "qedefs.h"        // PATCH_TERRAIN
#include "mainfrm.h"
#include <map>             // classCounts tally (was MFC CMap before U-SHIM removal)
#include <string>
       // (already included by stdafx.h), so MapInfo_01/02 + mapInfoStats_t stay common.

// ── extern declarations for globals/functions from other .cpp files ──────────
extern void       Assert( const char *file, int line, int type, const char *fmt, ... );
extern bool       Eclass_hasModel( eclass_t *ec );   // eclass.cpp 0x481740
extern void       Select_Deselect( int bDeselectFaces ); // select.cpp 0x48E800
extern entity_s   entityInsts;                       // map.cpp 0x23F1748 (entity-inst list head)
// active_brushes / selected_brushes sentinels are declared in qe3.h.

// CLASS_PREFAB: eclass classtype bit that identifies a misc_prefab entity.
// IDA: (classtype & CLASS_PREFAB) != 0 at 0x42EFD7.  IDB/GtkRadiant value = 0x10.
#ifndef CLASS_PREFAB
#define CLASS_PREFAB 0x10
#endif

// ── 0x42EEA0  MapInfo_01 ──────────────────────────────────────────────────────
// Walk brush list a3 and entity list a4, accumulating into two 7-counter blocks:
//   [0] brushes [1] curves [2] terrain [3] brush_ents [4] box_ents [5] model_ents [6] prefabs
// a1 = the world block, a2 = the prefab-children block.  A fixed-size CLASS_PREFAB entity
// with a realized prefab recurses with a2 for BOTH blocks, so nested prefabs share counters.
void __cdecl MapInfo_01(
    int        *worldStats,   // a1 — output counters for the world
    int         prefabStats,  // a2 — output counters for prefab children (int* cast to int)
    selbrush_t *brushList,    // a3 — active_brushes or prefab active_brushlist sentinel
    entity_s   *entList )     // a4 — entityInsts sentinel
{
    iassert(worldStats);
    iassert(prefabStats);
    iassert(brushList);

    // Traverse the brush list (selbrush_t doubly-linked, iterated via ->next).
    for ( selbrush_t *b = brushList->next; b != brushList; b = b->next )
    {
        iassert( b->owner );   // MapInfo.cpp:60

        iassert(b->owner->def == b->def->owner);

        // Fixed-size entities are counted in the ENTITY loop; their bbox brushes are
        // skipped here.
        entity_s_def *ownerDef = (entity_s_def *)b->owner->def;
        eclass_t     *ec       = ownerDef->eclass;
        if ( !ec->fixedsize )
        {
            {
                // inlined counting-helper head check; its param was `stats`
                int *stats = worldStats;
                iassert( stats );   // MapInfo.cpp:31
            }

            // The terrain bit is read through the INSTANCE indirection: instance[0] is the
            // patch DEF (Brush_AddToList asserts `b->patch->def == b->def->patch`) and
            // patchMesh_t.type is at DEF+0x10 — NOT offset 0x10 of the instance.
            patch_t *patchInst = b->patch;
            if ( patchInst )
            {
                patchMesh_t *pm = patchInst->def;               // instance.def = patch DEF
                if ( ( (byte)pm->type & PATCH_TERRAIN ) != 0 )
                    ++worldStats[2];   // terrain
                else
                    ++worldStats[1];   // curve
            }
            else
            {
                ++worldStats[0];       // plain brush
            }
        }
    }

    // Traverse the entity list.
    for ( entity_s *j = entList->next; j != entList; j = j->next )
    {
        entity_s_def *def = (entity_s_def *)j->def;
        eclass_t     *ec  = def->eclass;

        if ( ec->fixedsize )
        {
            int classtype = ec->classtype;
            if ( ( classtype & CLASS_PREFAB ) != 0 )
            {
                ++worldStats[6];   // prefab count
                if ( j->prefab )
                {
                    // MapInfo_01(a2, a2, &prefab->active_brushlist, prefab): the prefab head's
                    // first fields alias an entity_s (brush-list head @0x0C, ent-list next @0x04).
                    struct prefab_s *pfab = (struct prefab_s *)j->prefab;
                    MapInfo_01( (int *)prefabStats,
                                prefabStats,
                                (selbrush_t *)&((entity_s *)pfab)->brushes,
                                (entity_s   *)pfab );
                }
            }
            else if ( ( classtype & 8 ) != 0 || Eclass_hasModel( ec ) )
            {
                ++worldStats[5];   // model entity
            }
            else
            {
                ++worldStats[4];   // box entity
            }
        }
        else
        {
            ++worldStats[3];       // brush entity (non-fixed-size)
        }
    }
}

// ── 0x42F030  MapInfo_02 ──────────────────────────────────────────────────────
// ++map[ entity->def->eclass->name ] over entList, recursing into prefab children exactly
// like MapInfo_01.  IDB offsets: i->def = i[2], def->eclass = +0x60, eclass->name = +0x04,
// eclass->classtype = +0x180, i->prefab = +0x48.
void __cdecl MapInfo_02( std::map<std::string, int> *classCounts, entity_s *entList )
{
    iassert(entList);

    for ( entity_s *i = entList->next; i != entList; i = i->next )
    {
        entity_s_def *def = (entity_s_def *)i->def;
        eclass_t     *ec  = def->eclass;
        const char   *name = ec->name;

        // ++count for this class name (std::map value-initializes a missing key to 0,
        // matching MFC's Lookup(0-if-absent) then ++-then-store).
        ++(*classCounts)[ name ];

        if ( ( ec->classtype & CLASS_PREFAB ) != 0 && i->prefab )
            MapInfo_02( classCounts, (entity_s *)i->prefab );
    }
}

// ── 0x42F1C0  MapInfo_03_Item — set dialog control `ctrlId` to decimal `value`.
// MFC shell only (CWnd-typed): the ImGui Map-Info panel formats the same numbers itself.

// One Map Info snapshot: the two 7-counter blocks MapInfo_01 fills, the four derived
// totals, and the per-classname tally MapInfo_02 builds for the listbox rows.
struct mapInfoStats_t
{
    int brushes, curves, terrain, brush_ents, box_ents, model_ents, prefabs_total;
    int prefab_brushes, prefab_curves, prefab_terrain, prefab_brush_ents,
        prefab_box_ents, prefab_model_ents, prefab_prefabs;
    int geo_world_total, prefab_geo_total, world_total, prefab_total_total;
    // was MFC CMap<CString,LPCSTR,int,int> before U-SHIM removal. NOTE: std::map is
    // sorted (alphabetical) where the CMap reproduced MFC hash-bucket order — the
    // listbox is a DISPLAY list, so sorted is acceptable (arguably better). std::map
    // is movable, so mapInfoStats_t is no longer forced non-copyable (the "fresh local
    // per gather" pattern in MapInfo_Gather / MI_Refresh still works, just no longer
    // required by the type).
    std::map<std::string, int> classCounts;
};

// ══════════════════════════════════════════════════════════════════════════════
//  UI-independent count + tally behind CMapInfo's grid/listbox population.
//  0x42F230  MapInfoDialog (CMapInfo::OnInitDialog) — count + display body: MapInfo_01
//  fills the 14 counters, 27 MapInfo_03_Item calls push the grid cells (ids 1490..1516),
//  then MapInfo_02 fills the listbox with "<classname>\t<count>" lines.
// ══════════════════════════════════════════════════════════════════════════════
void MapInfo_Gather( mapInfoStats_t &out )
{
    Select_Deselect( 1 );  // 0x42F28D — the binary deselects before counting

    // The binary's 14 zeroed stack ints, passed as two contiguous 7-counter blocks.
    int worldStats[7]  = { 0, 0, 0, 0, 0, 0, 0 };
    int prefabStats[7] = { 0, 0, 0, 0, 0, 0, 0 };
    MapInfo_01( worldStats, (int)prefabStats, &active_brushes, &entityInsts );

    out.brushes    = worldStats[0]; out.curves     = worldStats[1]; out.terrain    = worldStats[2];
    out.brush_ents = worldStats[3]; out.box_ents   = worldStats[4]; out.model_ents = worldStats[5];
    out.prefabs_total = worldStats[6];
    out.prefab_brushes    = prefabStats[0]; out.prefab_curves   = prefabStats[1];
    out.prefab_terrain    = prefabStats[2]; out.prefab_brush_ents = prefabStats[3];
    out.prefab_box_ents   = prefabStats[4]; out.prefab_model_ents = prefabStats[5];
    out.prefab_prefabs    = prefabStats[6];

    // Derived totals (0x42F2B9..0x42F2E6).
    out.geo_world_total    = out.brushes + out.terrain + out.curves;
    out.prefab_geo_total   = out.prefab_brushes + out.prefab_terrain + out.prefab_curves;
    out.world_total        = out.brush_ents + out.box_ents + out.prefabs_total + out.model_ents;
    out.prefab_total_total = out.prefab_brush_ents + out.prefab_box_ents + out.prefab_prefabs + out.prefab_model_ents;

    // ── Per-class entity tally (MapInfo_02) ──
    // The binary built a CMap<CString,LPCSTR,int,int>(10 buckets); a std::map needs no
    // bucket sizing, so the InitHashTable(11) call is dropped. `out` is a fresh local
    // whose map starts empty, so no .clear() is needed either.
    MapInfo_02( &out.classCounts, &entityInsts );
}

// MFC shell — the grid/listbox push pass (CWnd-typed).  MapInfo_Gather above is the
// COMMON core imgui_panel_mapinfo.cpp calls.


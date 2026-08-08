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
#include <afxtempl.h>      // CMap<CString,LPCSTR,int,int> (MapInfo_02)

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
void __cdecl MapInfo_02( CMap<CString, LPCSTR, int, int> *classCounts, entity_s *entList )
{
    iassert(entList);

    for ( entity_s *i = entList->next; i != entList; i = i->next )
    {
        entity_s_def *def = (entity_s_def *)i->def;
        eclass_t     *ec  = def->eclass;
        const char   *name = ec->name;

        // Lookup-then-store: ++count for this class name (0 if absent).
        int count = 0;
        classCounts->Lookup( name, count );
        ++count;
        (*classCounts)[ name ] = count;

        if ( ( ec->classtype & CLASS_PREFAB ) != 0 && i->prefab )
            MapInfo_02( classCounts, (entity_s *)i->prefab );
    }
}

// ── 0x42F1C0  MapInfo_03_Item — set dialog control `ctrlId` to decimal `value`.
void MapInfo_03_Item( CWnd *dlg, int ctrlId, int value )
{
    CWnd *item = dlg->GetDlgItem( ctrlId );
    iassert(item);
    if ( !item )
        return;
    char buf[32];
    _itoa( value, buf, 10 );
    item->SetWindowText( buf );
}

// One Map Info snapshot: the two 7-counter blocks MapInfo_01 fills, the four derived
// totals, and the per-classname tally MapInfo_02 builds for the listbox rows.
struct mapInfoStats_t
{
    int brushes, curves, terrain, brush_ents, box_ents, model_ents, prefabs_total;
    int prefab_brushes, prefab_curves, prefab_terrain, prefab_brush_ents,
        prefab_box_ents, prefab_model_ents, prefab_prefabs;
    int geo_world_total, prefab_geo_total, world_total, prefab_total_total;
    CMap<CString, LPCSTR, int, int> classCounts;
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
    // The binary builds a CMap<CString,LPCSTR,int,int>(10 buckets) and populates it.
    out.classCounts.InitHashTable( 11 );             // ~10 buckets (binary uses 10)
    MapInfo_02( &out.classCounts, &entityInsts );
}

void MapInfo_PopulateDialog( CWnd *dlg, HWND hListBox )
{
    mapInfoStats_t st;
    MapInfo_Gather( st );

    if ( !dlg )
        return;

    // ── World column (1490,1493,...) ──
    MapInfo_03_Item( dlg, 1490, st.brushes );           // Brushes
    MapInfo_03_Item( dlg, 1493, st.curves );            // Curves
    MapInfo_03_Item( dlg, 1496, st.terrain );           // Terrain
    MapInfo_03_Item( dlg, 1499, st.geo_world_total );   // geo total
    MapInfo_03_Item( dlg, 1502, st.brush_ents );
    MapInfo_03_Item( dlg, 1505, st.box_ents );
    MapInfo_03_Item( dlg, 1508, st.model_ents );
    MapInfo_03_Item( dlg, 1511, st.prefabs_total );
    MapInfo_03_Item( dlg, 1514, st.world_total );
    // ── Prefab column (1491,1494,...) ──
    MapInfo_03_Item( dlg, 1491, st.prefab_brushes );
    MapInfo_03_Item( dlg, 1494, st.prefab_curves );
    MapInfo_03_Item( dlg, 1497, st.prefab_terrain );
    MapInfo_03_Item( dlg, 1500, st.prefab_geo_total );
    MapInfo_03_Item( dlg, 1503, st.prefab_brush_ents );
    MapInfo_03_Item( dlg, 1506, st.prefab_box_ents );
    MapInfo_03_Item( dlg, 1509, st.prefab_model_ents );
    MapInfo_03_Item( dlg, 1512, st.prefab_prefabs );
    MapInfo_03_Item( dlg, 1515, st.prefab_total_total );
    // ── Totals column (1492,1495,...) = world + prefab ──
    MapInfo_03_Item( dlg, 1492, st.brushes + st.prefab_brushes );
    MapInfo_03_Item( dlg, 1495, st.curves + st.prefab_curves );
    MapInfo_03_Item( dlg, 1498, st.terrain + st.prefab_terrain );
    MapInfo_03_Item( dlg, 1501, st.brushes + st.curves + st.terrain + st.prefab_brushes + st.prefab_curves + st.prefab_terrain );
    MapInfo_03_Item( dlg, 1504, st.brush_ents + st.prefab_brush_ents );
    MapInfo_03_Item( dlg, 1507, st.box_ents + st.prefab_box_ents );
    MapInfo_03_Item( dlg, 1510, st.model_ents + st.prefab_model_ents );
    MapInfo_03_Item( dlg, 1513, st.prefabs_total + st.prefab_prefabs );
    MapInfo_03_Item( dlg, 1516, st.world_total + st.prefab_total_total );

    // ── Per-class entity list (tally → listbox) ──
    // The binary resets the listbox (LB_RESETCONTENT), sets one tab stop at 196
    // (LB_SETTABSTOPS), then adds "<name>\t<count>" per class (LB_ADDSTRING).
    if ( hListBox && ::IsWindow( hListBox ) )
    {
        ::SendMessageA( hListBox, LB_RESETCONTENT, 0, 0 );
        int tab = 196;                               // 0xC4 (IDA 0x42F50E)
        ::SendMessageA( hListBox, LB_SETTABSTOPS, 1, (LPARAM)&tab );

        POSITION pos = st.classCounts.GetStartPosition();
        while ( pos )
        {
            CString key;
            int     count = 0;
            st.classCounts.GetNextAssoc( pos, key, count );
            char line[512];
            _snprintf( line, sizeof( line ), "%s\t%i", (LPCSTR)key, count );
            ::SendMessageA( hListBox, LB_ADDSTRING, 0, (LPARAM)line );
        }
    }
}

// ══ CMapInfo — the hand-built dialog shell: a header row, 9 labelled rows x 3 numeric
//    statics (ids 1490..1516), then the class list box (id 1013). ═══════════════════

CMapInfo *g_dlgMapInfo = nullptr;     // the singleton (NULL until first opened)

static HFONT s_miFont    = nullptr;
static HWND  s_miListBox = nullptr;

// The 9 row labels in grid order (matches the binary's 1490.. control rows).
static const char *const kMapInfoRows[9] =
{
    "Brushes",  "Curves",  "Terrain",  "Geometry total",
    "Brush entities", "Box entities", "Model entities", "Prefabs", "Entity total",
};

static HWND MI_MakeChild( HWND parent, const char *cls, DWORD style, int id,
                          int x, int y, int w, int hgt, const char *text = nullptr )
{
    HWND h = CreateWindowExA( 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                              x, y, w, hgt, parent, (HMENU)(INT_PTR)id,
                              AfxGetInstanceHandle(), NULL );
    if ( h && s_miFont )
        SendMessageA( h, WM_SETFONT, (WPARAM)s_miFont, 0 );
    return h;
}

BEGIN_MESSAGE_MAP( CMapInfo, CWnd )
    ON_WM_CREATE()
    ON_WM_CLOSE()
END_MESSAGE_MAP()

CMapInfo::CMapInfo()
{
}

int CMapInfo::OnCreate( LPCREATESTRUCT lpCreateStruct )
{
    if ( CWnd::OnCreate( lpCreateStruct ) == -1 )
        return -1;
    if ( !s_miFont )
        s_miFont = (HFONT)GetStockObject( DEFAULT_GUI_FONT );

    HWND self = GetSafeHwnd();
    const int M = 10, lblW = 130, colW = 72, colGap = 6, rowH = 20, hdrH = 18;
    const int col0 = M + lblW + 8;                 // first number column x
    const int col1 = col0 + colW + colGap;
    const int col2 = col1 + colW + colGap;
    int y = M;

    // Column headers.
    MI_MakeChild( self, "static", SS_LEFT,  0, M,    y, lblW, hdrH, "Statistic" );
    MI_MakeChild( self, "static", SS_RIGHT, 0, col0, y, colW, hdrH, "World" );
    MI_MakeChild( self, "static", SS_RIGHT, 0, col1, y, colW, hdrH, "Prefabs" );
    MI_MakeChild( self, "static", SS_RIGHT, 0, col2, y, colW, hdrH, "Total" );
    y += hdrH + 4;

    // 9 rows × 3 numeric cells.  Control ids follow the binary: row r world cell =
    // 1490+3r, prefab cell = 1491+3r, total cell = 1492+3r.
    for ( int r = 0; r < 9; ++r )
    {
        MI_MakeChild( self, "static", SS_LEFT,  0,          M,    y, lblW, hdrH, kMapInfoRows[r] );
        MI_MakeChild( self, "static", SS_RIGHT, 1490 + 3*r, col0, y, colW, hdrH, "0" );
        MI_MakeChild( self, "static", SS_RIGHT, 1491 + 3*r, col1, y, colW, hdrH, "0" );
        MI_MakeChild( self, "static", SS_RIGHT, 1492 + 3*r, col2, y, colW, hdrH, "0" );
        y += rowH;
    }
    y += 8;

    // Entity-class list box (ctrl id 1013).
    MI_MakeChild( self, "static", SS_LEFT, 0, M, y, lblW, hdrH, "Entities by class:" );
    y += hdrH + 2;
    s_miListBox = MI_MakeChild( self, "listbox", WS_BORDER | WS_VSCROLL | LBS_USETABSTOPS | LBS_NOINTEGRALHEIGHT,
                                1013, M, y, col2 + colW - M, 140 );

    // Compute + display now (the binary does this in OnInitDialog).
    MapInfo_PopulateDialog( this, s_miListBox );
    return 0;
}

void CMapInfo::OnClose()
{
    ShowWindow( SW_HIDE );
}

void CMapInfo::PostNcDestroy()
{
    g_dlgMapInfo = nullptr;
    s_miListBox  = nullptr;
    delete this;
}

// 0x426C60  CMainFrame::OnEditMapinfo opens this; the binary makes a fresh modal dialog
// each time, so Show() recreates (or re-shows + recomputes) the popup on every invoke.
void CMapInfo::Show()
{
    if ( g_dlgMapInfo && ::IsWindow( g_dlgMapInfo->GetSafeHwnd() ) )
    {
        // Recompute against the current map, then re-show + foreground.
        MapInfo_PopulateDialog( g_dlgMapInfo, s_miListBox );
        g_dlgMapInfo->ShowWindow( SW_SHOW );
        g_dlgMapInfo->SetForegroundWindow();
        return;
    }

    CWnd *parent = AfxGetMainWnd();
    g_dlgMapInfo = new CMapInfo();

    const DWORD style   = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
    const DWORD exStyle = WS_EX_TOOLWINDOW;
    int px = 240, py = 160;
    if ( parent && parent->GetSafeHwnd() )
    {
        RECT r;  parent->GetWindowRect( &r );
        px = r.left + 160;  py = r.top + 110;
    }
    CRect rc( px, py, px + 392, py + 440 );
    if ( !g_dlgMapInfo->CreateEx( exStyle,
             AfxRegisterWndClass( CS_HREDRAW | CS_VREDRAW,
                 ::LoadCursor( NULL, IDC_ARROW ), (HBRUSH)( COLOR_BTNFACE + 1 ), NULL ),
             "Map Info", style, rc, parent, 0 ) )
    {
        delete g_dlgMapInfo;
        g_dlgMapInfo = nullptr;
    }
}


// imgui_panel_mapinfo.cpp â€” UI-rework Phase 3 unit P-6: ImGui "Map info" over the
// mapinfo.cpp count core (MapInfo_Gather). New KISAK code; visible only under -imgui.
//
// Panel semantics vs CMapInfo: the SAME core call producing the SAME 27 grid cells and the
// SAME per-classname list â€”
//   * the 9 x 3 grid : MapInfo_Gather's counters, arranged exactly as
//                      MapInfo_PopulateDialog's 27 MapInfo_03_Item calls arrange control
//                      ids 1490..1516 (mapinfo.cpp:218-247), Total column arithmetic
//                      included verbatim
//   * the class list : the CMap tally MapInfo_02 builds, walked with
//                      GetStartPosition/GetNextAssoc in CMap order â€” i.e. UNORDERED, the
//                      same order the MFC LB_ADDSTRING loop produces (mapinfo.cpp:258-267)
// Sanctioned Phase-3 divergences (RADIANT_UI_REWORK_PLAN.md): the panel stays open, and it
// gathers on FIRST OPEN plus on an explicit "Refresh" click instead of every frame â€” unlike
// the sibling panels, whose gathers are cheap reads. Two reasons this one must not re-gather
// per frame:
//   1) MapInfo_Gather calls Select_Deselect( 1 ) as its first statement (mapinfo.cpp:183 â€”
//      faithful to the binary at 0x42F28D), so EVERY gather CLEARS THE SELECTION. Per-frame
//      gathering would make the editor unselectable while this panel is up.
//   2) it walks the whole brush list, the whole entity list and recurses through every
//      realized prefab (MapInfo_01 / MapInfo_02) â€” an O(map) snapshot, not a field read.
// This matches the dialog's own behavior anyway: CMapInfo recomputes only on open
// (mapinfo.cpp:346 / 369), never continuously.
//
// NO HWND/MFC-dialog path here: the numbers are copied out into plain ints and the tally
// into a vector, so nothing in this file needs MapInfo_03_Item or the listbox messages.
// (MFC types still appear â€” see the mapInfoStats_t copy below.)
#include "stdafx.h"
#include "qe3.h"
#include "radiant_ui_actions.h"
#include <imgui/imgui.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

// MUST MATCH mapinfo.cpp:166 verbatim (shared-header consolidation pending)
// PHASE-4 SHIM FLAG: `classCounts` is an MFC CMap<CString,LPCSTR,int,int>. That compiles
// here only because stdafx.h pulls afxwin.h + afxtempl.h (stdafx.h:47/51) into every editor
// TU; it is exactly the mapinfo entry in the plan's "CString/CMap needs a decision" list
// (RADIANT_UI_REWORK_PLAN.md:114 / P-6 note at :232). When that shim lands, this copy and
// the GetStartPosition/GetNextAssoc walk below are the two places to retarget.
// NOTE the CMap member also makes mapInfoStats_t NON-COPYABLE (no copy ctor), hence: the
// struct is constructed in place as a local and handed to MapInfo_Gather BY REFERENCE, never
// stored in a static or copied into one.
struct mapInfoStats_t
{
    int brushes, curves, terrain, brush_ents, box_ents, model_ents, prefabs_miTotal;
    int prefab_brushes, prefab_curves, prefab_terrain, prefab_brush_ents,
        prefab_box_ents, prefab_model_ents, prefab_prefabs;
    int geo_world_total, prefab_geo_total, world_total, prefab_total_total;
    // was MFC CMap<CString,LPCSTR,int,int> before U-SHIM removal (see mapinfo.cpp copy):
    // std::map is sorted (alphabetical), where the CMap reproduced MFC hash-bucket order.
    // This is a DISPLAY list, so sorted is acceptable. std::map is movable, so this struct
    // is no longer forced non-copyable — the fresh-local-per-refresh pattern below still
    // works but is no longer required by the type.
    std::map<std::string, int> classCounts;
};

// â”€â”€ mapinfo.cpp binding â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// The UI-independent count + tally body of CMapInfo::OnInitDialog (0x42F230). Non-static
// (mapinfo.cpp:181), and the only symbol this panel needs â€” MapInfo_01/_02 are reached
// through it, and MapInfo_03_Item / MapInfo_PopulateDialog are HWND-side, so neither is
// bound here. Reminder: it DESELECTS before counting.
extern void MapInfo_Gather( mapInfoStats_t &out );                        // mapinfo.cpp:181

// â”€â”€ panel state â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static bool s_showMapInfo = false;
static bool s_opened      = false;      // first-open latch (OnInitDialog / Show equivalent)

// The 27 grid cells as plain ints, one triple per grid row â€” copied out of the (local,
// non-copyable) stats struct by MI_Refresh. Row r here is the MFC row whose control ids are
// 1490+3r (World), 1491+3r (Prefabs), 1492+3r (Total) â€” mapinfo.cpp:332-334.
static int s_miWorld[9]  = { 0 };
static int s_miPrefab[9] = { 0 };
static int s_miTotal[9]  = { 0 };

// The per-classname tally, copied out of the CMap in iteration order (see MI_Refresh).
static std::vector<std::pair<std::string, int>> s_classRows;

// The 9 row labels in grid order. Duplicated from kMapInfoRows (mapinfo.cpp:280-284)
// because that table is file-static there â€” no symbol to bind.
static const char *const kMapInfoRows[9] =
{
    "Brushes",  "Curves",  "Terrain",  "Geometry total",
    "Brush entities", "Box entities", "Model entities", "Prefabs", "Entity total",
};

// One snapshot = one MapInfo_Gather into a FRESH local struct, then the copy-out.
// The struct must be fresh per call: MapInfo_Gather ends with classCounts.InitHashTable(11)
// (mapinfo.cpp:206), and MFC's InitHashTable ASSERTs the map is still empty â€” so reusing one
// long-lived instance across refreshes would trip that assert on the second gather. Cheap
// either way, and it is what MapInfo_PopulateDialog does with its own local (mapinfo.cpp:212).
static void MI_Refresh()
{
    mapInfoStats_t st;
    MapInfo_Gather( st );          // NOTE: Select_Deselect( 1 ) happens in here

    // â”€â”€ World column = MapInfo_03_Item ids 1490,1493,...,1514 (mapinfo.cpp:219-227) â”€â”€
    s_miWorld[0] = st.brushes;
    s_miWorld[1] = st.curves;
    s_miWorld[2] = st.terrain;
    s_miWorld[3] = st.geo_world_total;      // = brushes + terrain + curves
    s_miWorld[4] = st.brush_ents;
    s_miWorld[5] = st.box_ents;
    s_miWorld[6] = st.model_ents;
    s_miWorld[7] = st.prefabs_miTotal;
    s_miWorld[8] = st.world_total;          // = brush_ents + box_ents + prefabs_miTotal + model_ents

    // â”€â”€ Prefabs column = ids 1491,1494,...,1515 (mapinfo.cpp:229-237) â”€â”€
    s_miPrefab[0] = st.prefab_brushes;
    s_miPrefab[1] = st.prefab_curves;
    s_miPrefab[2] = st.prefab_terrain;
    s_miPrefab[3] = st.prefab_geo_total;
    s_miPrefab[4] = st.prefab_brush_ents;
    s_miPrefab[5] = st.prefab_box_ents;
    s_miPrefab[6] = st.prefab_model_ents;
    s_miPrefab[7] = st.prefab_prefabs;
    s_miPrefab[8] = st.prefab_total_total;

    // â”€â”€ Total column = ids 1492,1495,...,1516 (mapinfo.cpp:239-247) â”€â”€
    // Reproduced as WRITTEN, not as "world + prefab": the grand geometry total (id 1501) is
    // RE-SUMMED from the six raw counters rather than adding the two precomputed geo totals.
    // Same value, but the expression is kept verbatim so a future divergence in either
    // derived total shows up here the way it shows up in the dialog.
    s_miTotal[0] = st.brushes + st.prefab_brushes;
    s_miTotal[1] = st.curves  + st.prefab_curves;
    s_miTotal[2] = st.terrain + st.prefab_terrain;
    s_miTotal[3] = st.brushes + st.curves + st.terrain
               + st.prefab_brushes + st.prefab_curves + st.prefab_terrain;
    s_miTotal[4] = st.brush_ents  + st.prefab_brush_ents;
    s_miTotal[5] = st.box_ents    + st.prefab_box_ents;
    s_miTotal[6] = st.model_ents  + st.prefab_model_ents;
    s_miTotal[7] = st.prefabs_miTotal + st.prefab_prefabs;
    s_miTotal[8] = st.world_total  + st.prefab_total_total;

    // ORDERING CHANGE (U-SHIM removal): the MFC CMap walk produced hash-bucket order;
    // std::map iterates sorted (alphabetical). This is a DISPLAY list, so sorted is
    // acceptable (arguably nicer). The "unsorted, in tally order" readout below is now
    // literally alphabetical.
    s_classRows.clear();
    for ( auto &kv : st.classCounts )
        s_classRows.push_back( std::make_pair( kv.first, kv.second ) );
}

// â”€â”€ exports â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// The panel toggle, drawn inside the shell window's panel menu (imgui_shell.cpp â†’
// ImGuiPanels_Menu).
void ImGuiPanel_MapInfo_MenuItem()
{
    ImGui::Checkbox( "Map info", &s_showMapInfo );
}

// U-CMD-2: the menu/accelerator route.  Radiant_DispatchCommandDirect (mainfrm.cpp) calls
// this for command 32786, where the MFC handler called CMapInfo::Show() — the same
// show/hide flip, over the flag the checkbox above drives.
void ImGuiPanel_MapInfo_Toggle()
{
    s_showMapInfo = !s_showMapInfo;
}

void ImGuiPanel_MapInfo_Draw()
{
    if ( !s_showMapInfo )
    {
        s_opened = false;      // a re-open recomputes, like every CMapInfo::Show
        return;
    }

    if ( !s_opened )
    {
        s_opened = true;
        MI_Refresh();          // = the OnInitDialog / Show recompute
    }

    // Plain Begin (no AlwaysAutoResize): the class list is a fixed-height child of
    // available width, and auto-resize would fight its own width every frame.
    if ( ImGui::Begin( "Map info", &s_showMapInfo ) )
    {
        // The only control the dialog does not have. Needed because the panel outlives the
        // count: without it the numbers would silently go stale as the map is edited.
        if ( ImGui::Button( "Refresh" ) )
            MI_Refresh();
        ImGui::SameLine();
        ImGui::TextDisabled( "(counting deselects everything, as in the dialog)" );

        // 4 columns = the dialog's header row: the "Statistic" label column plus the three
        // numeric columns World / Prefabs / Total (mapinfo.cpp:321-324).
        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg
                                    | ImGuiTableFlags_SizingFixedFit;
        if ( ImGui::BeginTable( "##mapinfogrid", 4, flags ) )
        {
            ImGui::TableSetupColumn( "Statistic", ImGuiTableColumnFlags_WidthFixed, 130.0f );
            ImGui::TableSetupColumn( "World",     ImGuiTableColumnFlags_WidthFixed,  72.0f );
            ImGui::TableSetupColumn( "Prefabs",   ImGuiTableColumnFlags_WidthFixed,  72.0f );
            ImGui::TableSetupColumn( "Total",     ImGuiTableColumnFlags_WidthFixed,  72.0f );
            ImGui::TableHeadersRow();

            for ( int r = 0; r < 9; ++r )
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex( 0 );
                ImGui::TextUnformatted( kMapInfoRows[r] );
                // The MFC cells are SS_RIGHT statics, so the numbers are right-aligned here
                // too (the labels stay left-aligned).
                for ( int c = 0; c < 3; ++c )
                {
                    const int  v      = ( c == 0 ) ? s_miWorld[r] : ( c == 1 ) ? s_miPrefab[r] : s_miTotal[r];
                    char       cell[32];
                    _snprintf( cell, sizeof( cell ) - 1, "%i", v );
                    cell[sizeof( cell ) - 1] = '\0';
                    ImGui::TableSetColumnIndex( c + 1 );
                    const float pad = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize( cell ).x;
                    if ( pad > 0.0f )
                        ImGui::SetCursorPosX( ImGui::GetCursorPosX() + pad );
                    ImGui::TextUnformatted( cell );
                }
            }
            ImGui::EndTable();
        }

        // â”€â”€ "Entities by class:" â€” the listbox (ctrl id 1013, mapinfo.cpp:342) â”€â”€
        ImGui::SeparatorText( "Entities by class" );
        if ( ImGui::BeginChild( "##mapinfoclasses", ImVec2( 0.0f, 200.0f ), ImGuiChildFlags_Borders ) )
        {
            // The listbox lines are "<classname>\t<count>" against a single tab stop of 196
            // (LB_SETTABSTOPS, mapinfo.cpp:255-256). ImGui does not lay out tabs, so the same
            // 196 is spent as a SameLine offset â€” visually the same two columns. (The Win32
            // units are dialog-template quarter-characters, not pixels, so the column lands
            // in a slightly different place than the dialog's; nothing but spacing.)
            for ( size_t i = 0; i < s_classRows.size(); ++i )
            {
                ImGui::TextUnformatted( s_classRows[i].first.c_str() );
                ImGui::SameLine( 196.0f );
                ImGui::Text( "%i", s_classRows[i].second );
            }
        }
        ImGui::EndChild();

        // Row-count readout: not a dialog control, just the "how many classes" the listbox
        // scrollbar implies.
        ImGui::TextDisabled( "%d class%s (sorted alphabetically)",
                             (int)s_classRows.size(), s_classRows.size() == 1 ? "" : "es" );
    }
    ImGuiShell_CloseOnFocusLoss( &s_showMapInfo );
    ImGui::End();
}

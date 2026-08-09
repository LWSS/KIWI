// imgui_panel_filters.cpp — UI-rework: ImGui "Filters" panel, the shell replacement for
// CFilterWnd (the entity window's INSPECTOR_FILTER pane). New KISAK code.
//
// The F hotkey (ViewFilters, cmd 33104 → Cmd_OnFilterDlg) used to toggle the native entity
// window into filter mode; with no native entity window in the ImGui shell that path was inert
// (Radiant_ToggleInspectorMode bails on a null d_hwndEntity), so F did nothing. This panel is
// the faithful equivalent of CFilterWnd: four category CHECKLISTS (Geometry / Trigger / Entity
// / Other) over the loaded filter_entry_s lists, plus the six d_xyShowFlags checkboxes. It
// drives the SAME shell-agnostic cores the native pane did (filters.cpp), so filtering behaves
// identically — no new filter logic here.
#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

extern void            ImGuiShell_CloseOnFocusLoss( bool *p_open );      // imgui_shell.cpp
extern filter_entry_s *CFilterWnd_GetCategoryHead( int category );       // filters.cpp
extern void            RadiantFilters_ToggleEntry( filter_entry_s *cb, bool show );  // filters.cpp
extern int             CFilterWnd_ShowFlagCount();                       // filters.cpp
extern const char     *CFilterWnd_ShowFlagLabel( int i );
extern bool            CFilterWnd_GetShowFlag( int i );
extern void            CFilterWnd_SetShowFlag( int i, bool checked );

static bool s_showFilters = false;

// U-CMD: the menu/accelerator route (Radiant_DispatchCommandDirect cmd 33104, the F hotkey).
void ImGuiPanel_Filters_Toggle()
{
    s_showFilters = !s_showFilters;
}

void ImGuiPanel_Filters_Draw()
{
    if ( !s_showFilters )
        return;

    // AlwaysAutoResize: the window grows to fit ALL controls so nothing is ever behind a
    // scrollbar (the user's requirement). The 4 category lists are laid out SIDE-BY-SIDE in a
    // table so the panel stays as SHORT as its tallest category instead of stacking into a long
    // strip; the 6 show-flags go in a 3-wide grid below for the same reason.
    if ( ImGui::Begin( "Filters", &s_showFilters, ImGuiWindowFlags_AlwaysAutoResize ) )
    {
        // The four category checklists, in CFilterWnd_GetCategoryHead order (0..3), columned.
        static const char *const s_catLabels[4] = { "Geometry", "Trigger", "Entity", "Other" };
        if ( ImGui::BeginTable( "##filtercats", 4,
                                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit ) )
        {
            ImGui::TableNextRow();
            for ( int cat = 0; cat < 4; ++cat )
            {
                ImGui::TableSetColumnIndex( cat );
                ImGui::SeparatorText( s_catLabels[cat] );
                ImGui::PushID( cat );
                int row = 0;
                for ( filter_entry_s *e = CFilterWnd_GetCategoryHead( cat ); e; e = e->next_filter )
                {
                    if ( !e->name )
                        continue;
                    ImGui::PushID( row++ );          // unique ID even for duplicate names
                    bool shown = e->isShown;
                    if ( ImGui::Checkbox( e->name, &shown ) )
                        RadiantFilters_ToggleEntry( e, shown );
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }

        // The six simple show-flag checkboxes (CFilterWnd::GetSettings): Angles / Connections /
        // Names / Blocks / Coordinates / Reverse Filter, each a d_xyShowFlags bit. 3-wide grid.
        ImGui::SeparatorText( "Show" );
        const int n = CFilterWnd_ShowFlagCount();
        if ( ImGui::BeginTable( "##showflags", 3, ImGuiTableFlags_SizingFixedFit ) )
        {
            for ( int i = 0; i < n; ++i )
            {
                ImGui::TableNextColumn();
                bool on = CFilterWnd_GetShowFlag( i );
                if ( ImGui::Checkbox( CFilterWnd_ShowFlagLabel( i ), &on ) )
                    CFilterWnd_SetShowFlag( i, on );
            }
            ImGui::EndTable();
        }
    }
    ImGuiShell_CloseOnFocusLoss( &s_showFilters );
    ImGui::End();
}

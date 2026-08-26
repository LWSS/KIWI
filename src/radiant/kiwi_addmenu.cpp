#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Cursor-anchored view of registered creation-related commands; see kiwi_addmenu.h.

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_addmenu.h"
#include "radiant_frame.h"          // RadiantCommand table API
#include "kiwi_cmdui.h"             // shared palette/add-menu UI helpers
#include "kiwi_command.h"       // command ids and KiwiCmd_CanExecute

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace
{
    bool   s_open       = false;
    bool   s_focusNext  = false;
    char   s_filter[96] = { 0 };
    int    s_selected   = 0;
    ImVec2 s_anchor( 0.0f, 0.0f );      // cursor position captured on first draw
    bool   s_anchorPending = false;     // capture through ImGui inside a frame

    // Editorial ordering and grouping; metadata stays in g_radiantCommands.
    struct addRow_t
    {
        const char *category;   // non-NULL only on the first row of a group
        const char *label;
        int         commandId;
    };

    const addRow_t KADD_ROWS[] =
    {
        // Both ids intentionally address one chained-curve tool; keep Polyline
        // as a discoverable alias.
        { "Curves", "Line (chained curve)",  KIWI_CMD_DRAW_LINE        },
        { 0,        "Polyline (= Line)",     KIWI_CMD_DRAW_POLYLINE    },
        { 0,        "Spline",                KIWI_CMD_DRAW_SPLINE      },
        { 0,        "Rectangle (corner)",    KIWI_CMD_DRAW_RECT        },
        { 0,        "Rectangle (center)",    KIWI_CMD_DRAW_RECT_CENTER },
        { 0,        "Circle (center)",       KIWI_CMD_DRAW_CIRCLE      },
        { 0,        "Circle (2-point)",      KIWI_CMD_DRAW_CIRCLE_2PT  },
        { 0,        "Arc (center)",          KIWI_CMD_DRAW_ARC         },
        { 0,        "Polygon (n-gon)",       KIWI_CMD_DRAW_POLYGON     },

        { "Solids", "Box (corner)",          KIWI_CMD_PRIM_BOX         },
        { 0,        "Box (centre)",          KIWI_CMD_PRIM_BOX_CENTER  },   // same Solids group
        { 0,        "Cylinder",              KIWI_CMD_PRIM_CYLINDER    },
        { 0,        "Sphere",                KIWI_CMD_PRIM_SPHERE      },
        { 0,        "Cone",                  KIWI_CMD_PRIM_CONE        },

        { "From construction", "Extrude Region", KIWI_CMD_EXTRUDE_REGION },

        // Place Sun edits worldspawn keys, so it remains outside curve/solid groups.
        { "Lighting", "Place Sun",              KIWI_CMD_PLACE_SUN        },

        // Keep drawing-plane controls adjacent to creation commands.
        { "Construction plane", "XY",        KIWI_CMD_CPLANE_XY        },
        { 0,        "XZ",                    KIWI_CMD_CPLANE_XZ        },
        { 0,        "YZ",                    KIWI_CMD_CPLANE_YZ        },
        { 0,        "From face under cursor",KIWI_CMD_CPLANE_FACE      },
        { 0,        "From view",             KIWI_CMD_CPLANE_VIEW      },
    };

    const int KADD_COUNT = (int)( sizeof( KADD_ROWS ) / sizeof( KADD_ROWS[0] ) );

    const float KADD_WIDTH   = 330.0f;
    const float KADD_HEIGHT  = 430.0f;
    const float KADD_CURSOR_OFF = 8.0f;   // down-right of the cursor anchor

    // Shortcut labels refresh on open; visibility refreshes once after filter
    // input each frame.
    char s_shortcut[KADD_COUNT][80];
    bool s_visible[KADD_COUNT];

    void ResolveShortcuts()
    {
        for ( int i = 0; i < KADD_COUNT; ++i )
            KiwiCmdUI_ShortcutForId( KADD_ROWS[i].commandId, s_shortcut[i],
                                     (int)sizeof( s_shortcut[i] ) );
    }

    bool RowMatches( int i )
    {
        char hay[128];
        _snprintf( hay, sizeof( hay ), "%s %s",
                   KADD_ROWS[i].label,
                   KADD_ROWS[i].category ? KADD_ROWS[i].category : "" );
        hay[sizeof( hay ) - 1] = '\0';
        return KiwiCmdUI_Fuzzy( hay, s_filter );
    }

    void RebuildVisible()
    {
        for ( int i = 0; i < KADD_COUNT; ++i )
            s_visible[i] = RowMatches( i );
    }

    bool RowVisible( int i ) { return s_visible[i]; }

    // Move the highlight to the next VISIBLE row in `dir`, wrapping.
    void StepSelection( int dir )
    {
        for ( int n = 0; n < KADD_COUNT; ++n )
        {
            s_selected = ( s_selected + dir + KADD_COUNT ) % KADD_COUNT;
            if ( RowVisible( s_selected ) )
                return;
        }
    }

    void Run( int commandId )
    {
        KiwiAdd_Close();
        // Postpone Win32 modal commands until after ImGui composition; ids fit
        // LOWORD.
        ::PostMessageA( g_qeglobals.d_hwndMain, WM_COMMAND,
                        (WPARAM)(unsigned int)commandId, 0 );
    }
}

void KiwiAdd_Open()
{
    s_open      = true;
    s_focusNext = true;
    s_filter[0] = '\0';
    s_selected  = 0;
    ResolveShortcuts();
    RebuildVisible();
    // Capture through ImGui on first draw because command dispatch can open the
    // menu outside a frame.
    s_anchorPending = true;
}

void KiwiAdd_Close()
{
    s_open = false;
}

void KiwiAdd_Toggle()
{
    if ( s_open )
        KiwiAdd_Close();
    else
        KiwiAdd_Open();
}

bool KiwiAdd_IsOpen()
{
    return s_open;
}

void KiwiAdd_MenuItem()
{
    if ( ImGui::Button( "Open add menu (create)" ) )
        KiwiAdd_Open();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Every creation command, at the cursor (modern keymap: Shift+A).\n"
                           "Curves, solid primitives, extrude, construction planes." );
}

void KiwiAdd_Draw()
{
    if ( !s_open )
        return;

    if ( s_anchorPending )
    {
        s_anchor = ImGui::GetMousePos();
        if ( s_anchor.x < 0.0f || s_anchor.y < 0.0f )
            s_anchor = ImVec2( 0.0f, 0.0f );
        s_anchorPending = false;
    }

    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const ImVec2 size( KADD_WIDTH, KADD_HEIGHT );

    // Keep the cursor-anchored window within the main viewport work area.
    float x = s_anchor.x + KADD_CURSOR_OFF;
    float y = s_anchor.y + KADD_CURSOR_OFF;
    if ( x + size.x > vp->WorkPos.x + vp->WorkSize.x ) x = vp->WorkPos.x + vp->WorkSize.x - size.x;
    if ( y + size.y > vp->WorkPos.y + vp->WorkSize.y ) y = vp->WorkPos.y + vp->WorkSize.y - size.y;
    if ( x < vp->WorkPos.x ) x = vp->WorkPos.x;
    if ( y < vp->WorkPos.y ) y = vp->WorkPos.y;

    ImGui::SetNextWindowPos( ImVec2( x, y ), ImGuiCond_Always );
    ImGui::SetNextWindowSize( size, ImGuiCond_Always );
    if ( s_focusNext )
        ImGui::SetNextWindowFocus();       // only on initial open

    if ( !ImGui::Begin( "Add", &s_open,
                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize ) )
    {
        ImGui::End();
        return;
    }

    if ( s_focusNext )
    {
        ImGui::SetKeyboardFocusHere();
        s_focusNext = false;
    }
    ImGui::SetNextItemWidth( -1.0f );
    const bool entered = ImGui::InputTextWithHint( "##kiwiaddfilter", "Filter...",
                                                   s_filter, sizeof( s_filter ),
                                                   ImGuiInputTextFlags_EnterReturnsTrue );

    // Rebuild here so navigation and drawing share the current filter state.
    RebuildVisible();

    // A single-line InputText leaves Up/Down available for row navigation.
    if ( ImGui::IsKeyPressed( ImGuiKey_DownArrow, true ) )
        StepSelection( 1 );
    if ( ImGui::IsKeyPressed( ImGuiKey_UpArrow, true ) )
        StepSelection( -1 );
    if ( ImGui::IsKeyPressed( ImGuiKey_Escape, false ) )
    {
        ImGui::End();
        KiwiAdd_Close();
        return;
    }

    // Never let Enter run a row hidden by the current filter.
    if ( !RowVisible( s_selected ) )
        StepSelection( 1 );

    int runId = 0;
    if ( entered && RowVisible( s_selected ) && KiwiCmd_CanExecute( KADD_ROWS[s_selected].commandId ) )
        runId = KADD_ROWS[s_selected].commandId;

    ImGui::Separator();

    if ( ImGui::BeginChild( "##kiwiaddrows", ImVec2( 0.0f, 0.0f ), 0,
                            ImGuiWindowFlags_NoSavedSettings ) )
    {
        for ( int i = 0; i < KADD_COUNT; ++i )
        {
            if ( !RowVisible( i ) )
                continue;

            if ( KADD_ROWS[i].category )
                ImGui::SeparatorText( KADD_ROWS[i].category );

            ImGui::PushID( i );
            const float rowW    = ImGui::GetContentRegionAvail().x;
            const bool  enabled = KiwiCmd_CanExecute( KADD_ROWS[i].commandId );
            if ( !enabled )
                ImGui::PushStyleColor( ImGuiCol_Text,
                                       ImGui::GetStyleColorVec4( ImGuiCol_TextDisabled ) );

            if ( ImGui::Selectable( KADD_ROWS[i].label, i == s_selected ) )
            {
                s_selected = i;
                if ( enabled )
                    runId = KADD_ROWS[i].commandId;
            }
            if ( !enabled && ImGui::IsItemHovered() )
                ImGui::SetTooltip( "Unavailable right now." );

            KiwiCmdUI_RightAlignedHint( s_shortcut[i], rowW );

            if ( !enabled )
                ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    ImGui::End();

    if ( runId )
        Run( runId );
}

void KiwiAdd_RegisterCommands()
{
    // Add Menu is unbound; modern Shift+A belongs to Line.
    Radiant_RegisterCommand( "KiwiAddMenu", 0, 0, KIWI_CMD_ADD_MENU );
}

bool KiwiAdd_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned int)KIWI_CMD_ADD_MENU )
        return false;
    KiwiAdd_Toggle();
    return true;
}

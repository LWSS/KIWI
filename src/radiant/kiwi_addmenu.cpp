#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_addmenu.cpp — RADIANT_UX_DESIGN §16b.4 implementation.  See
// kiwi_addmenu.h for what this is a view over and why it defers dispatch.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_addmenu.h"
#include "kiwi_command.h"       // the ids + KiwiCmd_CanExecute — the ONLY dependency

#include <ctype.h>
#include <stdio.h>
#include <string.h>

// MUST MATCH mainfrm.cpp's definition verbatim — same reasoning as
// kiwi_palette.cpp:25 (this is the parameter type of the two extern formatters
// below, so field offsets and mangled names both have to agree).
struct RadiantCommand { const char *name; byte vk; byte mods; int commandId; };

// ── mainfrm.cpp bindings (verified against their definitions) ───────────────
extern int         Radiant_GetCommandTable( const RadiantCommand **out );   // mainfrm.cpp
extern const char *CommandList_KeyName( const RadiantCommand &c, char keybuf[8] );
extern void        CommandList_Mods( const RadiantCommand &c, char mods[64] );
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

namespace
{
    bool   s_open       = false;
    bool   s_focusNext  = false;
    char   s_filter[96] = { 0 };
    int    s_selected   = 0;
    ImVec2 s_anchor( 0.0f, 0.0f );      // where the cursor was when it opened
    bool   s_anchorPending = false;     // read the cursor INSIDE the frame (see Open)

    // THE editorial table.  Order and grouping only — every row is an id that
    // already exists in g_radiantCommands (§3: no second registry).  A row with
    // a NULL label is a category header.
    struct addRow_t
    {
        const char *category;   // non-NULL only on the first row of a group
        const char *label;
        int         commandId;
    };

    const addRow_t KADD_ROWS[] =
    {
        // ROUND P: one CHAINED CURVE tool behind both rows — every click adds a
        // point, RMB/Enter finishes the whole chain as one object (see
        // KiwiCurveTool in kiwi_construct.cpp).  The Polyline row is kept, and
        // labelled as the alias it now is, so nobody hunts for a tool that moved.
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
        { "Solids", "Box (centre)",          KIWI_CMD_PRIM_BOX_CENTER  },   // ROUND AF, ITEM 8
        { 0,        "Cylinder",              KIWI_CMD_PRIM_CYLINDER    },
        { 0,        "Sphere",                KIWI_CMD_PRIM_SPHERE      },
        { 0,        "Cone",                  KIWI_CMD_PRIM_CONE        },

        { "From construction", "Extrude Region", KIWI_CMD_EXTRUDE_REGION },

        // The plane is the question this menu provokes ("on WHAT am I drawing?"),
        // so the answer is one scroll away instead of in another panel.
        { "Construction plane", "XY",        KIWI_CMD_CPLANE_XY        },
        { 0,        "XZ",                    KIWI_CMD_CPLANE_XZ        },
        { 0,        "YZ",                    KIWI_CMD_CPLANE_YZ        },
        { 0,        "From face under cursor",KIWI_CMD_CPLANE_FACE      },
        { 0,        "From view",             KIWI_CMD_CPLANE_VIEW      },
    };

    const int KADD_COUNT = (int)( sizeof( KADD_ROWS ) / sizeof( KADD_ROWS[0] ) );

    // Subsequence fuzzy match, case-insensitive — the same one the palette uses
    // (kiwi_palette.cpp FuzzyMatch), duplicated rather than exported because it
    // is nine lines and exporting it would mean a header for one predicate.
    bool FuzzyMatch( const char *str, const char *pat )
    {
        if ( !pat || !*pat )
            return true;
        const char *s = str;
        for ( const char *p = pat; *p; ++p )
        {
            if ( *p == ' ' )
                continue;
            const int pc = tolower( (unsigned char)*p );
            for ( ;; )
            {
                if ( !*s )
                    return false;
                const int sc = tolower( (unsigned char)*s );
                ++s;
                if ( sc == pc )
                    break;
            }
        }
        return true;
    }

    // The LIVE binding for an id, formatted through mainfrm's own formatters so
    // a keymap-profile switch shows up here with no work (CommandList_Mods leaves
    // a trailing " + ", hence the plain concat).
    void ShortcutFor( int commandId, char *out, int outSize )
    {
        out[0] = '\0';
        const RadiantCommand *table = 0;
        const int count = Radiant_GetCommandTable( &table );
        if ( !table )
            return;
        for ( int i = 0; i < count; ++i )
        {
            if ( table[i].commandId != commandId || !table[i].vk )
                continue;
            char mods[64];
            char keybuf[8];
            CommandList_Mods( table[i], mods );
            const char *key = CommandList_KeyName( table[i], keybuf );
            _snprintf( out, outSize, "%s%s", mods, key ? key : "" );
            out[outSize - 1] = '\0';
            return;
        }
    }

    bool RowVisible( int i )
    {
        char hay[128];
        _snprintf( hay, sizeof( hay ), "%s %s",
                   KADD_ROWS[i].label,
                   KADD_ROWS[i].category ? KADD_ROWS[i].category : "" );
        hay[sizeof( hay ) - 1] = '\0';
        return FuzzyMatch( hay, s_filter );
    }

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
        // DEFERRED dispatch — see kiwi_palette.cpp's Run() for the full reasoning
        // (an in-frame command that opens a modal Win32 dialog would nest its
        // message pump inside the compositing scene bracket).  Ids fit LOWORD.
        ::PostMessageA( g_qeglobals.d_hwndMain, WM_COMMAND,
                        (WPARAM)(unsigned int)commandId, 0 );
    }
}

// ─── open / close ────────────────────────────────────────────────────────────
void KiwiAdd_Open()
{
    s_open      = true;
    s_focusNext = true;
    s_filter[0] = '\0';
    s_selected  = 0;
    // Anchor at the cursor, the whole point of an ADD MENU as opposed to a
    // palette: the list appears where you are already looking.  The cursor is
    // read in the DRAW, not here: Open() runs on the pump thread out of a
    // WM_COMMAND (the palette / menu / hotkey routes all land there), which is
    // outside any ImGui frame.
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

// ─── the shell entry (classic keymap route) ──────────────────────────────────
void KiwiAdd_MenuItem()
{
    if ( ImGui::Button( "Open add menu (create)" ) )
        KiwiAdd_Open();
    if ( ImGui::IsItemHovered() )
        ImGui::SetTooltip( "Every creation command, at the cursor (modern keymap: Shift+A).\n"
                           "Curves, solid primitives, extrude, construction planes." );
}

// ─── draw ────────────────────────────────────────────────────────────────────
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
    const ImVec2 size( 330.0f, 430.0f );

    // Clamp so the menu is never born off-screen (opening it near the bottom-right
    // corner is exactly when a cursor anchor would otherwise put it there).
    float x = s_anchor.x + 8.0f;
    float y = s_anchor.y + 8.0f;
    if ( x + size.x > vp->WorkPos.x + vp->WorkSize.x ) x = vp->WorkPos.x + vp->WorkSize.x - size.x;
    if ( y + size.y > vp->WorkPos.y + vp->WorkSize.y ) y = vp->WorkPos.y + vp->WorkSize.y - size.y;
    if ( x < vp->WorkPos.x ) x = vp->WorkPos.x;
    if ( y < vp->WorkPos.y ) y = vp->WorkPos.y;

    ImGui::SetNextWindowPos( ImVec2( x, y ), ImGuiCond_Always );
    ImGui::SetNextWindowSize( size, ImGuiCond_Always );
    if ( s_focusNext )
        ImGui::SetNextWindowFocus();       // on OPEN only (kiwi_palette.cpp's rule)

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

    // Keyboard navigation, read from ImGui (never from Win32) — a single-line
    // InputText does not consume the arrows, so this is safe while it has focus.
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

    // A filter change can hide the highlighted row; walk to the next visible one
    // so Enter never runs something the user cannot see.
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

            // The header is drawn with its FIRST VISIBLE member, so filtering
            // never leaves an orphaned category label behind.
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

            char sc[80];
            ShortcutFor( KADD_ROWS[i].commandId, sc, sizeof( sc ) );
            if ( sc[0] )
            {
                float sx = rowW - ImGui::CalcTextSize( sc ).x - 8.0f;
                if ( sx < 0.0f )
                    sx = 0.0f;
                ImGui::SameLine( sx );
                ImGui::TextDisabled( "%s", sc );
            }

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

// ─── §3 registration + dispatch ──────────────────────────────────────────────
void KiwiAdd_RegisterCommands()
{
    // The CLASSIC-profile binding is none; kiwi_keymap.cpp lays Shift+A on top for
    // the modern profile (and displaces SelectAllOfType to Shift+Alt+A — the audit
    // is in kiwi_keymap.h).
    Radiant_RegisterCommand( "KiwiAddMenu", 0, 0, KIWI_CMD_ADD_MENU );
}

bool KiwiAdd_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned int)KIWI_CMD_ADD_MENU )
        return false;
    KiwiAdd_Toggle();
    return true;
}

#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

#include "stdafx.h"
#include "qe3.h"
#include "radiant_frame.h"         // Radiant_CheckMenu

#include "kiwi_windows.h"
#include "kiwi_command.h"          // KIWI_CMD_WINDOW_* (the reserved instant ids)
#include "kiwi_ux.h"               // KiwiUX_ShowGrid / KiwiUX_ShowAxes
#include "kiwi_camera.h"           // KiwiCam_Ortho / KiwiCam_SetOrtho
#include "radiant_registry.h"      // Radiant_ProfileGetInt/SetInt

#include <shellapi.h>              // ShellExecuteA (KiwiWindows_RevealInExplorer)
#include <stdio.h>                 // sprintf_s

// Keep Radiant_CheckMenu single-sourced in radiant_frame.h.
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp
extern int  Sys_Printf( const char *fmt, ... );                                             // win_qe3.cpp

namespace
{
    struct kwEntry_t
    {
        const char *title;      // the ImGui window title (== the dock ini key)
        const char *entry;      // kiwi_radiant.ini [KiwiWindows] key
        const char *menuText;   // the native menu caption
        int         commandId;  // the KIWI instant id the menu item posts
        int         defOpen;    // authoritative default-open state
    };

    // Order must match kiwiWindow_t.
    // 2D View and Textures stay open in the right column; Z and shell stay closed for 3D-first use.
    // Default-open tabs must also be placed by ImGuiShell_BuildDefaultDockLayout.
    const kwEntry_t s_def[KIWI_WIN_COUNT] = {
        { "2D View",          "XY",      "&2D View (XY / XZ / YZ)", KIWI_CMD_WINDOW_XY,      1 },
        { "Z",                "Z",       "&Z View",                 KIWI_CMD_WINDOW_Z,       0 },
        { "Textures",         "Texture", "&Texture Browser",        KIWI_CMD_WINDOW_TEXTURE, 1 },
        { "Console",          "Console", "&Console",                KIWI_CMD_WINDOW_CONSOLE, 1 },
        { "KIWI ImGui shell", "Shell",   "KIWI ImGui &Shell panel", KIWI_CMD_WINDOW_SHELL,   0 },
        { "Outliner",         "Outliner","&Outliner (scene list)",  KIWI_CMD_WINDOW_OUTLINER,1 },
        { "Entities",         "Entities","&Entity Browser",         KIWI_CMD_WINDOW_ENTITIES,1 },
        { "Models",           "Models",  "&Model Browser",          KIWI_CMD_WINDOW_MODELS,  1 },
        { "Sky",              "Sky",     "S&ky Browser",            KIWI_CMD_WINDOW_SKY,     1 },
        { "UV editor",        "UvEditor","&UV Editor",              KIWI_CMD_WINDOW_UVEDITOR,1 },
        { "Sun",              "Sun",     "S&un Helper",             KIWI_CMD_WINDOW_SUN,     1 },
        { "Light",            "Light",   "&Light Helper",           KIWI_CMD_WINDOW_LIGHT,   1 },
        { "Inspector",        "Inspector", "&Inspector",             KIWI_CMD_WINDOW_INSPECTOR, 1 },
        { "Decals",           "Decals",  "&Decals",                 KIWI_CMD_WINDOW_DECALS,  1 },
        { "Reference Images", "RefImages", "&Reference Images",     KIWI_CMD_WINDOW_REFIMAGES, 1 },
    };

    // Older profiles are reseeded once so visibility defaults match the rebuilt dock layout.
    // Advancing the shared version intentionally overwrites per-window choices once.
    const char *KW_SECTION  = "KiwiWindows";
    const char *KW_VER_KEY  = "DefaultsVersion";
    // Shared with kiwi_dock<N>.ini; bump for default-set or default-layout changes.
    const int   KW_VERSION  = KIWI_LAYOUT_VERSION;

    bool s_open[KIWI_WIN_COUNT];        // the LIVE flag (ImGui's p_open target)
    bool s_last[KIWI_WIN_COUNT];        // what we last persisted — the ✕-box detector
    bool s_justOpened[KIWI_WIN_COUNT];  // closed -> open THIS frame (re-dock trigger)
    bool s_loaded = false;
    HMENU s_menu  = nullptr;            // the frame menu the popup was appended to

    void Persist( int i );          // defined below; Load's re-seed pass needs it

    void Load()
    {
        if ( s_loaded )
            return;
        s_loaded = true;                // Set first in case profile access ever becomes re-entrant.
        const int stored  = Radiant_ProfileGetInt( KW_SECTION, KW_VER_KEY, 1 );
        const bool reseed = ( stored < KW_VERSION );
        for ( int i = 0; i < KIWI_WIN_COUNT; ++i )
        {
            s_open[i] = reseed
                      ? ( s_def[i].defOpen != 0 )
                      : ( Radiant_ProfileGetInt( KW_SECTION, s_def[i].entry,
                                                 s_def[i].defOpen ) != 0 );
            s_last[i] = s_open[i];
            s_justOpened[i] = false;
        }
        if ( reseed )
        {
            for ( int i = 0; i < KIWI_WIN_COUNT; ++i )
                Persist( i );
            Radiant_ProfileSetInt( KW_SECTION, KW_VER_KEY, KW_VERSION );
        }
    }

    void Persist( int i )
    {
        Radiant_ProfileSetInt( KW_SECTION, s_def[i].entry, s_open[i] ? 1 : 0 );
    }
}

const char *KiwiWindows_Title( kiwiWindow_t w )
{
    if ( (unsigned)w >= (unsigned)KIWI_WIN_COUNT )
        return "";
    return s_def[w].title;
}

bool KiwiWindows_IsOpen( kiwiWindow_t w )
{
    if ( (unsigned)w >= (unsigned)KIWI_WIN_COUNT )
        return false;
    Load();
    return s_open[w];
}

bool *KiwiWindows_OpenPtr( kiwiWindow_t w )
{
    if ( (unsigned)w >= (unsigned)KIWI_WIN_COUNT )
        return nullptr;
    Load();
    return &s_open[w];
}

void KiwiWindows_Set( kiwiWindow_t w, bool open )
{
    if ( (unsigned)w >= (unsigned)KIWI_WIN_COUNT )
        return;
    Load();
    if ( s_open[w] == open )
        return;
    if ( open && !s_open[w] )
        s_justOpened[w] = true;
    s_open[w] = open;
    s_last[w] = open;
    Persist( (int)w );
    KiwiWindows_SyncMenu();
}

bool KiwiWindows_JustOpened( kiwiWindow_t w )
{
    if ( (unsigned)w >= (unsigned)KIWI_WIN_COUNT )
        return false;
    Load();
    return s_justOpened[w];
}

void KiwiWindows_CommitPending()
{
    Load();
    for ( int i = 0; i < KIWI_WIN_COUNT; ++i )
    {
        // Clear only after Begin consumed the one-frame re-dock request.
        s_justOpened[i] = false;
        if ( s_open[i] == s_last[i] )
            continue;
        // ImGui's title-bar close is the only external writer today.
        s_last[i] = s_open[i];
        Persist( i );
        KiwiWindows_SyncMenu();
    }
}

// Native Windows popup. MF_POPUP takes the submenu HMENU in the UINT_PTR command slot.
// Item ids follow the ordinary WM_COMMAND -> Radiant_ExecCommand route.
// Radiant_CheckMenu uses MF_BYCOMMAND (mainfrm.cpp).
void KiwiWindows_BuildMenu( void *frameMenu )
{
    HMENU hMenu = (HMENU)frameMenu;
    if ( !hMenu )
        return;
    Load();

    HMENU popup = ::CreatePopupMenu();
    if ( !popup )
    {
        // Built once at startup, so reporting this failure cannot spam.
        Sys_Printf( "KIWI: CreatePopupMenu failed - the \"Windows\" menu is not "
                    "available this session (%i windows unreachable from the menu "
                    "bar; the palette still reaches them).\n", (int)KIWI_WIN_COUNT );
        return;
    }
    for ( int i = 0; i < KIWI_WIN_COUNT; ++i )
        ::AppendMenuA( popup, MF_STRING, (UINT_PTR)s_def[i].commandId, s_def[i].menuText );

    // Floating tool panels that are not KIWI_WIN_* dock windows but still belong here.
    ::AppendMenuA( popup, MF_SEPARATOR, 0, nullptr );
    ::AppendMenuA( popup, MF_STRING, (UINT_PTR)KIWI_CMD_GRASS_PANEL, "Grass Scatter" );
    ::AppendMenuA( popup, MF_STRING, (UINT_PTR)KIWI_CMD_TERRAIN_PANEL, "Terrain Sculpt	Y" );

    // Camera is always on; grayed id 0 prevents hiding the primary viewport.
    ::AppendMenuA( popup, MF_SEPARATOR, 0, nullptr );
    ::AppendMenuA( popup, MF_STRING | MF_GRAYED | MF_CHECKED, 0, "3D Camera (always on)" );

    ::AppendMenuA( hMenu, MF_POPUP | MF_STRING, (UINT_PTR)popup, "&Windows" );
    s_menu = hMenu;
    ::DrawMenuBar( g_qeglobals.d_hwndMain );

    KiwiWindows_SyncMenu();
}

void KiwiWindows_SyncMenu()
{
    if ( !s_menu )
        return;
    Load();
    for ( int i = 0; i < KIWI_WIN_COUNT; ++i )
        Radiant_CheckMenu( (UINT)s_def[i].commandId, s_open[i] );
    // Non-dock tool panels appended to the same popup.
    extern bool KiwiGrass_PanelVisible();   // kiwi_grass.cpp
    Radiant_CheckMenu( (UINT)KIWI_CMD_GRASS_PANEL, KiwiGrass_PanelVisible() );
    extern bool KiwiTerrain_PanelVisible();   // kiwi_terrain.cpp
    Radiant_CheckMenu( (UINT)KIWI_CMD_TERRAIN_PANEL, KiwiTerrain_PanelVisible() );
}

// Native View-popup toggles use KIWI ids and Radiant_CheckMenu check marks.
namespace
{
    const int KVIEW_POPUP_INDEX = 2;    // File 0, Edit 1, VIEW 2 (res/radiant.rc)
    bool      s_viewMenuBuilt = false;
}

void KiwiWindows_BuildViewMenu( void *frameMenu )
{
    HMENU hMenu = (HMENU)frameMenu;
    if ( !hMenu )
        return;
    HMENU view = ::GetSubMenu( hMenu, KVIEW_POPUP_INDEX );
    if ( !view )
        return;                          // no View popup — leave the bar untouched

    ::AppendMenuA( view, MF_SEPARATOR, 0, nullptr );
    ::AppendMenuA( view, MF_STRING, (UINT_PTR)KIWI_CMD_VIEW_SHOW_GRID, "Show &Grid" );
    ::AppendMenuA( view, MF_STRING, (UINT_PTR)KIWI_CMD_VIEW_SHOW_AXES, "Show &Axes" );
    // Checked means orthographic.
    ::AppendMenuA( view, MF_STRING, (UINT_PTR)KIWI_CMD_VIEW_ORTHO,     "&Orthographic Camera" );
    ::AppendMenuA( view, MF_STRING, (UINT_PTR)KIWI_CMD_VIEW_SHOW_TRIS, "Show &Triangle Count" );
    ::AppendMenuA( view, MF_STRING, (UINT_PTR)KIWI_CMD_VIEW_SHOW_FACING, "Show &Facing Arrows" );
    ::AppendMenuA( view, MF_STRING, (UINT_PTR)KIWI_CMD_VIEW_LEAK_BG, "&Leak Finder Background (strobing)" );
    s_viewMenuBuilt = true;
    ::DrawMenuBar( g_qeglobals.d_hwndMain );

    KiwiWindows_SyncViewMenu();
}

void KiwiWindows_SyncViewMenu()
{
    if ( !s_viewMenuBuilt )
        return;
    Radiant_CheckMenu( (UINT)KIWI_CMD_VIEW_SHOW_GRID, KiwiUX_ShowGrid() );
    Radiant_CheckMenu( (UINT)KIWI_CMD_VIEW_SHOW_AXES, KiwiUX_ShowAxes() );
    Radiant_CheckMenu( (UINT)KIWI_CMD_VIEW_ORTHO,     KiwiCam_Ortho() );   // projection check state
    Radiant_CheckMenu( (UINT)KIWI_CMD_VIEW_SHOW_TRIS, KiwiUX_ShowTriCount() );
    Radiant_CheckMenu( (UINT)KIWI_CMD_VIEW_SHOW_FACING, KiwiUX_ShowFacingArrows() );
    Radiant_CheckMenu( (UINT)KIWI_CMD_VIEW_LEAK_BG, KiwiUX_LeakBackground() );
}

// Command registration and dispatch.
void KiwiWindows_RegisterCommands()
{
    // Window and view toggles stay unbound but registered for palette/keymap discovery.
    Radiant_RegisterCommand( "KiwiWindowXY",      0, 0, KIWI_CMD_WINDOW_XY );
    Radiant_RegisterCommand( "KiwiWindowZ",       0, 0, KIWI_CMD_WINDOW_Z );
    Radiant_RegisterCommand( "KiwiWindowTexture", 0, 0, KIWI_CMD_WINDOW_TEXTURE );
    Radiant_RegisterCommand( "KiwiWindowConsole", 0, 0, KIWI_CMD_WINDOW_CONSOLE );
    Radiant_RegisterCommand( "KiwiWindowShell",   0, 0, KIWI_CMD_WINDOW_SHELL );
    Radiant_RegisterCommand( "KiwiWindowOutliner",0, 0, KIWI_CMD_WINDOW_OUTLINER );  // toggle registered here
    Radiant_RegisterCommand( "KiwiWindowInspector", 0, 0, KIWI_CMD_WINDOW_INSPECTOR );
    // Other helper windows register their rows in their owning modules; all toggles
    // still dispatch through s_def below.
    Radiant_RegisterCommand( "KiwiViewShowGrid",  0, 0, KIWI_CMD_VIEW_SHOW_GRID );
    Radiant_RegisterCommand( "KiwiViewShowAxes",  0, 0, KIWI_CMD_VIEW_SHOW_AXES );
    Radiant_RegisterCommand( "KiwiViewOrtho",     0, 0, KIWI_CMD_VIEW_ORTHO );
    Radiant_RegisterCommand( "KiwiGrassScatter",  0, 0, KIWI_CMD_GRASS_PANEL );
    Radiant_RegisterCommand( "KiwiTerrainSculpt", 0, 0, KIWI_CMD_TERRAIN_PANEL );
    Radiant_RegisterCommand( "KiwiWindowDecals",  0, 0, KIWI_CMD_WINDOW_DECALS );
    Radiant_RegisterCommand( "KiwiWindowRefImages", 0, 0, KIWI_CMD_WINDOW_REFIMAGES ); // KIWI (REFIMG)
    Radiant_RegisterCommand( "KiwiPerf",            0, 0, KIWI_CMD_PERF_HUD );         // perf HUD toggle
    Radiant_RegisterCommand( "KiwiViewShowTris",    0, 0, KIWI_CMD_VIEW_SHOW_TRIS );   // View > Show Triangle Count
    Radiant_RegisterCommand( "KiwiViewShowFacing",  0, 0, KIWI_CMD_VIEW_SHOW_FACING ); // View > Show Facing Arrows
    Radiant_RegisterCommand( "KiwiViewLeakBackground", 0, 0, KIWI_CMD_VIEW_LEAK_BG );  // View > Leak Finder Background
}

bool KiwiWindows_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId == (unsigned int)KIWI_CMD_VIEW_SHOW_GRID )
    {
        KiwiUX_SetShowGrid( !KiwiUX_ShowGrid() );
        KiwiWindows_SyncViewMenu();
        return true;
    }
    if ( cmdId == (unsigned int)KIWI_CMD_VIEW_SHOW_AXES )
    {
        KiwiUX_SetShowAxes( !KiwiUX_ShowAxes() );
        KiwiWindows_SyncViewMenu();
        return true;
    }
    // Share the view-cube setter so every projection control stays synchronized.
    if ( cmdId == (unsigned int)KIWI_CMD_VIEW_ORTHO )
    {
        KiwiCam_SetOrtho( !KiwiCam_Ortho() );
        KiwiWindows_SyncViewMenu();
        return true;
    }
    if ( cmdId == (unsigned int)KIWI_CMD_GRASS_PANEL )
    {
        extern void KiwiGrass_TogglePanel();   // kiwi_grass.cpp
        KiwiGrass_TogglePanel();
        return true;
    }
    if ( cmdId == (unsigned int)KIWI_CMD_TERRAIN_PANEL )
    {
        extern void KiwiTerrain_TogglePanel();   // kiwi_terrain.cpp
        KiwiTerrain_TogglePanel();
        return true;
    }
    if ( cmdId == (unsigned int)KIWI_CMD_PERF_HUD )
    {
        extern void KiwiPerf_Toggle();           // kiwi_perf.cpp
        KiwiPerf_Toggle();
        return true;
    }
    if ( cmdId == (unsigned int)KIWI_CMD_VIEW_SHOW_TRIS )
    {
        KiwiUX_SetShowTriCount( !KiwiUX_ShowTriCount() );
        KiwiWindows_SyncViewMenu();
        return true;
    }
    if ( cmdId == (unsigned int)KIWI_CMD_VIEW_LEAK_BG )
    {
        KiwiUX_SetLeakBackground( !KiwiUX_LeakBackground() );
        KiwiWindows_SyncViewMenu();
        return true;
    }
    if ( cmdId == (unsigned int)KIWI_CMD_VIEW_SHOW_FACING )
    {
        KiwiUX_SetShowFacingArrows( !KiwiUX_ShowFacingArrows() );
        KiwiWindows_SyncViewMenu();
        return true;
    }

    for ( int i = 0; i < KIWI_WIN_COUNT; ++i )
    {
        if ( (unsigned)s_def[i].commandId != cmdId )
            continue;
        const kiwiWindow_t w = (kiwiWindow_t)i;
        KiwiWindows_Set( w, !KiwiWindows_IsOpen( w ) );
        return true;
    }
    return false;
}

bool KiwiWindows_RevealInExplorer( const char *osPath )
{
    if ( !osPath || !osPath[0] )
        return false;
    // explorer.exe /select,"<path>" opens the parent folder with the file highlighted.
    char arguments[1024];
    sprintf_s( arguments, sizeof( arguments ), "/select,\"%s\"", osPath );
    const HINSTANCE result = ::ShellExecuteA( g_qeglobals.d_hwndMain, "open",
                                               "explorer.exe", arguments,
                                               nullptr, SW_SHOWNORMAL );
    return (INT_PTR)result > 32;
}

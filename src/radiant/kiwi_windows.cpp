#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_windows.cpp — RADIANT_UX_DESIGN §9 implementation (shakeout B).
// See kiwi_windows.h for the directive this serves and for why the flag lives
// here rather than inside imgui_shell.cpp.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "radiant_frame.h"         // Radiant_CheckMenu (radiant_frame.h:92)

#include "kiwi_windows.h"
#include "kiwi_command.h"          // KIWI_CMD_WINDOW_* (the reserved instant ids)
#include "kiwi_ux.h"               // §17 KiwiUX_ShowGrid / KiwiUX_ShowAxes (shakeout C)
#include "kiwi_camera.h"           // ROUND M: KiwiCam_Ortho / KiwiCam_SetOrtho
#include "radiant_registry.h"      // Radiant_ProfileGetInt/SetInt (radiant_registry.h:13/15)

// mainfrm.cpp:1690 — void Radiant_CheckMenu( UINT id, bool checked ) (radiant_frame.h:92).
// Declared by radiant_frame.h, which qe3.h already pulls in; re-stating it here would risk
// a signature drift, so it is used as declared.
// mainfrm.cpp:1236 — bool Radiant_RegisterCommand( const char *, byte, byte, int ).
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

namespace
{
    struct kwEntry_t
    {
        const char *title;      // the ImGui window title (== the dock ini key)
        const char *entry;      // kiwi_radiant.ini [KiwiWindows] key
        const char *menuText;   // the native menu caption
        int         commandId;  // the KIWI instant id the menu item posts
        int         defOpen;    // the shakeout-B default
    };

    // THE table.  Order == kiwiWindow_t.
    // KIWI-UX (shakeout I): 2D View and Textures default OPEN again.  USER
    // DIRECTIVE: "put the texture view by default under the 2d view on the right
    // middle dock."  Shakeout B closed all four because the 3D camera had just
    // become the primary surface and a four-pane grid was in the way; a RIGHT
    // COLUMN is not in the way, and a texture browser you have to go and find is a
    // texture browser nobody applies.  Z and the shell panel stay closed — the
    // directive named two windows and the 3D-first ruling still holds for the rest.
    // The dock LAYOUT that places them is ImGuiShell_BuildDefaultDockLayout; the two
    // have to change together or the windows open as floating tabs over the camera.
    const kwEntry_t s_def[KIWI_WIN_COUNT] = {
        { "2D View",          "XY",      "&2D View (XY / XZ / YZ)", KIWI_CMD_WINDOW_XY,      1 },
        { "Z",                "Z",       "&Z View",                 KIWI_CMD_WINDOW_Z,       0 },
        { "Textures",         "Texture", "&Texture Browser",        KIWI_CMD_WINDOW_TEXTURE, 1 },
        { "Console",          "Console", "&Console",                KIWI_CMD_WINDOW_CONSOLE, 1 },
        { "KIWI ImGui shell", "Shell",   "KIWI ImGui &Shell panel", KIWI_CMD_WINDOW_SHELL,   0 },
        // ROUND W: the outliner.  Default OPEN — the directive asks for the list to
        // be there, and KW_VERSION is bumped below so existing profiles get it once.
        { "Outliner",         "Outliner","&Outliner (scene list)",  KIWI_CMD_WINDOW_OUTLINER,1 },
    };

    // KIWI-UX (shakeout I): the defaults above only bite on a FRESH profile, and an
    // existing kiwi_radiant.ini carries the shakeout-B zeros — the same trap the
    // dock ini bump solves for the layout.  So the [KiwiWindows] section is
    // VERSIONED: when the stored version is older than KW_VERSION the per-window
    // entries are re-seeded from the table once and the version is written back.
    // A user who has since opened or closed a window deliberately loses that choice
    // exactly once, which is the same deal the dock-ini bump makes and the only way
    // a default change is ever visible to an existing install.
    const char *KW_SECTION  = "KiwiWindows";
    const char *KW_VER_KEY  = "DefaultsVersion";
    const int   KW_VERSION  = 5;          // 1 = shakeout B (all closed but Console)
                                          // 2 = shakeout I (2D View + Textures back on)
                                          // 3 = ROUND W    (the Outliner, on by default)
                                          // 4 = ROUND X    (full-width console strip; the
                                          //     window SET is unchanged, but the dock ini
                                          //     bump to kiwi_dock7 only rebuilds the layout
                                          //     — re-seeding here keeps the two in step so
                                          //     every window the new layout places is
                                          //     actually open when it is placed)
                                          // 5 = ROUND Y    (skinnier Outliner column; same
                                          //     deal — the dock ini goes to kiwi_dock8 and
                                          //     this keeps the window set in step with it)

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
        s_loaded = true;                // BEFORE the reads: Radiant_ProfileGetInt cannot
                                        // re-enter here, but the guard keeps one owner.
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
        // The re-dock request is one frame long: the shell consumed it in the Begin
        // it just ran, so clear it AFTER the frame rather than at the toggle.
        s_justOpened[i] = false;
        if ( s_open[i] == s_last[i] )
            continue;
        // Somebody else moved it — the title-bar ✕ is the only such writer today.
        s_last[i] = s_open[i];
        Persist( i );
        KiwiWindows_SyncMenu();
    }
}

// ─── the native "Windows" popup ──────────────────────────────────────────────
// Win32 usage mirrored from the two menu sites already in this shell:
//   * ::LoadMenuA + ::SetMenu on the frame  (radiant_main.cpp:482-484)
//   * ::CheckMenuItem( HMENU, id, MF_CHECKED|MF_UNCHECKED ) with MF_BYCOMMAND
//     (the default) — Radiant_CheckMenu, mainfrm.cpp:1690-1695.
// AppendMenuA with MF_POPUP takes the sub-menu handle in the UINT_PTR slot; the
// items use MF_STRING and carry the KIWI command id, so their WM_COMMAND lands in
// Radiant_FrameWndProc's `case WM_COMMAND` (radiant_main.cpp:196-207) exactly like
// every other menu item and routes through Radiant_ExecCommand.
void KiwiWindows_BuildMenu( void *frameMenu )
{
    HMENU hMenu = (HMENU)frameMenu;
    if ( !hMenu )
        return;
    Load();

    HMENU popup = ::CreatePopupMenu();
    if ( !popup )
        return;
    for ( int i = 0; i < KIWI_WIN_COUNT; ++i )
        ::AppendMenuA( popup, MF_STRING, (UINT_PTR)s_def[i].commandId, s_def[i].menuText );

    // The camera is NOT toggleable (kiwi_windows.h) — say so rather than leave the
    // user hunting for it.  MF_GRAYED so it can never be clicked; id 0 = no command.
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
}

// ─── shakeout C: Show Grid / Show Axes in the NATIVE View popup ──────────────
// See kiwi_windows.h for the index-safety argument.  Same Win32 vocabulary as
// KiwiWindows_BuildMenu above: AppendMenuA + MF_STRING carrying a KIWI instant
// id, and Radiant_CheckMenu (mainfrm.cpp:1690, MF_BYCOMMAND) for the check marks.
namespace
{
    const int KVIEW_POPUP_INDEX = 2;    // File 0, Edit 1, VIEW 2 (res/radiant.rc:65)
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
    // ROUND M: the 3D camera's projection.  Checked == orthographic.
    ::AppendMenuA( view, MF_STRING, (UINT_PTR)KIWI_CMD_VIEW_ORTHO,     "&Orthographic Camera" );
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
    Radiant_CheckMenu( (UINT)KIWI_CMD_VIEW_ORTHO,     KiwiCam_Ortho() );   // ROUND M
}

// ─── §3 registration + dispatch ──────────────────────────────────────────────
void KiwiWindows_RegisterCommands()
{
    // Unbound by default in BOTH keymap profiles: these are view toggles, and every
    // free key is worth more to an editing command.  Registering them anyway is what
    // makes them searchable in the §15 palette and remappable from radiant.ini.
    Radiant_RegisterCommand( "KiwiWindowXY",      0, 0, KIWI_CMD_WINDOW_XY );
    Radiant_RegisterCommand( "KiwiWindowZ",       0, 0, KIWI_CMD_WINDOW_Z );
    Radiant_RegisterCommand( "KiwiWindowTexture", 0, 0, KIWI_CMD_WINDOW_TEXTURE );
    Radiant_RegisterCommand( "KiwiWindowConsole", 0, 0, KIWI_CMD_WINDOW_CONSOLE );
    Radiant_RegisterCommand( "KiwiWindowShell",   0, 0, KIWI_CMD_WINDOW_SHELL );
    Radiant_RegisterCommand( "KiwiWindowOutliner",0, 0, KIWI_CMD_WINDOW_OUTLINER );  // ROUND W
    // Shakeout C: the two View-menu toggles.  Unbound for the same reason as the
    // five above — a view toggle is not worth a key — but registered so they are
    // searchable in the §15 palette and remappable from radiant.ini.
    Radiant_RegisterCommand( "KiwiViewShowGrid",  0, 0, KIWI_CMD_VIEW_SHOW_GRID );
    Radiant_RegisterCommand( "KiwiViewShowAxes",  0, 0, KIWI_CMD_VIEW_SHOW_AXES );
    // ROUND M: the projection toggle, unbound for the same reason.
    Radiant_RegisterCommand( "KiwiViewOrtho",     0, 0, KIWI_CMD_VIEW_ORTHO );
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
    // ROUND M: the same call the view-cube pill makes (kiwi_viewcube.cpp), so the
    // menu, the palette and the widget cannot disagree about the camera.
    if ( cmdId == (unsigned int)KIWI_CMD_VIEW_ORTHO )
    {
        KiwiCam_SetOrtho( !KiwiCam_Ortho() );
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

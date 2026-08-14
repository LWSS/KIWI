#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_windows.h — RADIANT_UX_DESIGN §9 (viewport layout), shakeout B.
//
// USER DIRECTIVE (verbatim):
//   "3d cam view IS the new primary way of working with the editor.  We want to
//    obsolete the 2d view and z views completely (but leave them openable in the
//    topbar somehow via a windows dropdown)"
//   "Hide the texture and 'KIWI Imgui shell' tabs by default but keep them in
//    the program as tabs somewhere."
//
// So: every dock window except the CAMERA gets an open/closed flag.  The flag is
// the single source of truth for three consumers, which is the whole point of
// putting it here rather than inside imgui_shell.cpp:
//
//   1. imgui_shell.cpp's draw — a closed window skips ImGui::Begin ENTIRELY, and
//      an open one passes &flag as Begin's p_open so the title-bar ✕ writes the
//      same variable the menu writes.
//   2. imgui_shell.cpp's RTT pass — ImGuiShell_RenderViewportsToRT renders from
//      the cell size the LAST draw recorded, so a window that stopped being drawn
//      would otherwise keep its stale size and keep costing a full off-screen
//      scene every tick forever.  The render is gated on the same flag, so a
//      hidden 2D / Z / texture view costs nothing at all.
//   3. the native "Windows" popup appended to the frame's IDR_MENU_QUAKE3 menu —
//      one check-marked item per window, dispatched through the ordinary
//      Radiant_ExecCommand -> Radiant_DispatchCommandDirect KIWI arm.
//
// The CAMERA is deliberately NOT in the list: it is the editor now, and a menu
// item that can hide the only remaining viewport is a trap.
//
// Persistence: kiwi_radiant.ini section "KiwiWindows" (Radiant_Profile*), one
// entry per window.  Defaults encode the directive — only the console is on.
// ─────────────────────────────────────────────────────────────────────────────

enum kiwiWindow_t
{
    KIWI_WIN_XY = 0,        // "2D View"          (RTT_XY)      default OFF
    KIWI_WIN_Z,             // "Z"                (RTT_Z)       default OFF
    KIWI_WIN_TEXTURE,       // "Textures"         (RTT_TEXTURE) default OFF
    KIWI_WIN_CONSOLE,       // "Console"                        default ON
    KIWI_WIN_SHELL,         // "KIWI ImGui shell"               default OFF
    // ROUND W: the Plasticity-style scene list (kiwi_outliner.h).  DEFAULT ON and
    // docked into a new LEFT column, because the directive asks for it "on the
    // left" and a scene list nobody can find is a scene list nobody uses.
    KIWI_WIN_OUTLINER,      // "Outliner"                       default ON
    KIWI_WIN_COUNT,
};

// The ImGui window title — the SAME string the dock ini and DockBuilderDockWindow
// key off, so there is exactly one spelling of each name in the build.
const char *KiwiWindows_Title( kiwiWindow_t w );

bool  KiwiWindows_IsOpen ( kiwiWindow_t w );
// Address of the live flag, for ImGui::Begin's p_open (the ✕ box writes it).
// Reconciled + persisted by KiwiWindows_CommitPending() at the end of the frame.
bool *KiwiWindows_OpenPtr( kiwiWindow_t w );
void  KiwiWindows_Set    ( kiwiWindow_t w, bool open );

// Call ONCE per ImGui frame, after every Begin/End: notices a flag that the ✕ box
// (or anything else) changed behind our back, persists it and re-syncs the menu
// check marks.  Also answers "did this window just re-open this frame", which the
// shell uses to re-dock it (SetNextWindowDockID) instead of letting it float off
// in whatever position the ini remembered.
void  KiwiWindows_CommitPending();
bool  KiwiWindows_JustOpened( kiwiWindow_t w );

// Boot: append the native "Windows" popup to the frame menu, then seed the check
// marks.  `frameMenu` is the HMENU (void * so this header stays windows.h-free).
void  KiwiWindows_BuildMenu( void *frameMenu );
void  KiwiWindows_SyncMenu();

// ── shakeout C: Show Grid / Show Axes in the NATIVE View menu ───────────────
// USER DIRECTIVE: "add a disable grid option in the View Dropdown at the topbar".
//
// Appended as a separator + two check items INTO the existing View popup rather
// than into a new top-level one, and that is safe: appending ITEMS to a popup
// changes nothing about the MENU BAR's popup indices, which is what the two
// index-based consumers in this build actually read —
//   * texwnd.cpp:1600 / :1651 / :1665 / :1679  GetSubMenu( menu, 5 )  = Textures
//   * radiant_main.cpp:529 / qe3.cpp:766       GetSubMenu( menu, 0 )  = File (MRU)
// Only adding or removing a POPUP would shift those, and the §9 "Windows" popup
// is appended at the END of the bar (after Help), so even that one cannot.
// View is popup index 2 (File 0, Edit 1, View 2, Selection 3, Grid 4, Textures 5
// — res/radiant.rc:27/48/65/146/212/228, and index 5 agreeing with texwnd is the
// cross-check that the counting is right).
void  KiwiWindows_BuildViewMenu( void *frameMenu );
void  KiwiWindows_SyncViewMenu();

// §3 registration + the instant-command tail (kiwi_command.cpp calls both).
void  KiwiWindows_RegisterCommands();
bool  KiwiWindows_DispatchInstant( unsigned int cmdId );

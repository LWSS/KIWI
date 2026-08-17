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
// entry per window.
//
// KIWI-UX (CLEANUP, C-58): THE DEFAULTS ARE THE `s_def` TABLE IN kiwi_windows.cpp,
// AND NOWHERE ELSE.  This line used to claim "only the console is on", which was
// shakeout B's set and has been wrong since shakeout I put 2D View and Textures
// back; five of the eight rows ship open today.  Read `s_def`'s `defOpen` column
// for the answer, and `KW_VERSION` (== KIWI_LAYOUT_VERSION below) for whether an
// existing profile gets re-seeded from it — those two are the whole mechanism.
// The `default ON/OFF` notes on the enum below are a convenience echo of that
// table; if they ever disagree with it, the table is right.
// ─────────────────────────────────────────────────────────────────────────────

// ── KIWI-UX (CLEANUP, C-8): ONE LAYOUT VERSION, TWO CONSUMERS ───────────────
// The window-defaults reseed (kiwi_windows.cpp, section "KiwiWindows" entry
// "DefaultsVersion") and the dock-layout ini filename (imgui_shell.cpp builds
// "kiwi_dock<N>.ini") always had to be bumped TOGETHER — a half-bump leaves the
// windows reseeded with the old layout pinned, or the reverse, with no
// diagnostic.  The two were coupled only by a comment ladder in each file.  Now
// there is ONE number: bump it here and both follow.  (It carries the dock ini's
// numbering, which was ahead of the window counter's; the changeover reseeds the
// window defaults exactly once, which is what the mechanism does anyway.)
// KIWI-UX (ROUND BD): 10 -> 11.  The UV editor joins the Textures dock node as a
// fourth tab, which needs BOTH halves of this number: the [KiwiWindows] reseed (so
// an existing profile actually gets the new window OPEN) and a fresh
// kiwi_dock11.ini (so ImGuiShell_BuildDefaultDockLayout runs again and PLACES it).
// Existing users get a one-time layout reseed — the same deal every default change
// has made since shakeout I, and the only way one is ever visible to an install
// that already has a profile.
#define KIWI_LAYOUT_VERSION  11

enum kiwiWindow_t
{
    // Shakeout I put these two back ON: "put the texture view by default under the
    // 2d view on the right middle dock."
    KIWI_WIN_XY = 0,        // "2D View"          (RTT_XY)      default ON
    KIWI_WIN_Z,             // "Z"                (RTT_Z)       default OFF
    KIWI_WIN_TEXTURE,       // "Textures"         (RTT_TEXTURE) default ON
    KIWI_WIN_CONSOLE,       // "Console"                        default ON
    KIWI_WIN_SHELL,         // "KIWI ImGui shell"               default OFF
    // ROUND W: the Plasticity-style scene list (kiwi_outliner.h).  DEFAULT ON and
    // docked into a new LEFT column, because the directive asks for it "on the
    // left" and a scene list nobody can find is a scene list nobody uses.
    KIWI_WIN_OUTLINER,      // "Outliner"                       default ON
    // ROUND AU: the TrenchBroom-style entity browser (kiwi_entbrowser.h).  USER
    // DIRECTIVE: "a new panel that's in the same viewport (tabbed) with the
    // textures tab".  DEFAULT ON, and docked into the SAME node as "Textures" by
    // ImGuiShell_BuildDefaultDockLayout — the two have to change together or the
    // window opens as a floating tab over the camera (the note on s_def below).
    KIWI_WIN_ENTITIES,      // "Entities"                       default ON
    // ROUND AZ: the SKY tab (kiwi_skybox.h).  USER DIRECTIVE: "Make this a separate
    // tab like the entity tab" — so it joins the SAME dock node as Textures and
    // Entities, and the same rule applies: this enum and
    // ImGuiShell_BuildDefaultDockLayout have to change together or it opens as a
    // floating tab over the camera.  DEFAULT ON, because a tab nobody can find is a
    // tab nobody uses and KW_VERSION is bumped so existing profiles get it once.
    KIWI_WIN_SKY,           // "Sky"                            default ON
    // ROUND BD: the TrenchBroom-style UV editor (kiwi_uveditor.h).  USER DIRECTIVE:
    // a UV window showing every selected face as a wireframe over the tiled texture.
    // DEFAULT ON, and docked into the SAME node as Textures / Entities / Sky by
    // ImGuiShell_BuildDefaultDockLayout — the fourth tab of that node, and the same
    // rule the three rows above it state applies: this enum and that function have to
    // change together or the window opens as a floating tab over the camera.
    KIWI_WIN_UVEDITOR,      // "UV editor"                      default ON
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
//   * texwnd.cpp:1638 / :1651 / :1665 / :1679  GetSubMenu( menu, 5 )  = Textures
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

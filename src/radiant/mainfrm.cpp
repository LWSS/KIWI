#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// The editor frame's shell-agnostic layer: the QE4 pane layout + splitter gutters, the boot
// steps, the idle pump / invalidation broadcast / hotkey map, and the id->action command
// table behind every menu + accelerator id.  Renderer bring-up is
// R_BeginRegistrationInternal (0x416510).
// U-RIP: the MFC shell (CMainFrame, its AFX_MSGMAP, the C*Dlg DDX classes, the toolbar +
// status bar + CTextureBar members and every CMainFrame::On* forwarder) is DELETED.  What
// is left is the free-function layer radiant_main.cpp drives; `was CMainFrame::X (0xADDR)`
// notes are PROVENANCE (the IDB body each function came from), not live code.

#include "stdafx.h"
#include <ctime>                       // clock() â€” RoutineProcessing camera-fly dtime (0x421a90)
#include "mainfrm.h"
#include "radiant_frame.h"             // U-BOOT: the shell-agnostic frame API declared for both shells
#include "qe3.h"                       // g_qeglobals, qeglobals_t, grid_sizes
#include "prefs.h"                     // g_PrefsDlg, Prefs_ShowDialog/SavePrefs
#include "radiant_registry.h"          // Radiant_ProfileGetString (was AfxGetApp()->GetProfileString)
#include "xywnd.h"                     // ED_VIEW_*
#include <qcommon/qcommon.h>           // Dvar_Init
#include <qcommon/cmd.h>               // Cbuf_Init, Cmd_Init
#include <qcommon/threads.h>           // THREAD_CONTEXT_COUNT
#include <win32/win_local.h>           // CRITSECT_COUNT (radiant-safe; cmd.cpp includes it too)
#include <universal/com_memory.h>      // Com_InitHunkMemory
#include <universal/com_files.h>       // FS_InitFilesystem
#include <gfx_d3d/r_init.h>            // R_InitEditor, R_InitRendererForWindow
#include <gfx_d3d/r_rendercmds.h>      // R_InitRenderCommands
#include <gfx_d3d/r_material.h>        // Material (R_BeginRegistrationInternal return)

extern void Radiant_RegisterGroupCDvars();   // engine_stubs.cpp
extern void Sys_InitializeCriticalSections();// universal/win_common.cpp (decl lives in win32/win_local.h, not radiant-safe)
extern void track_init();                    // qcommon/mem_track.h
extern void SL_Init();                        // script/scr_stringlist.cpp (also inits the script memory tree)

// The real renderer bootstrap (gfxwrapper.cpp, IDB 0x416510). Asserts d_hwndCamera/XY/Z/
// Texture + lyrMtlWndGlob.layerList, attaches the device to each, registers fonts/qerfont
// (g_qeglobals.d_font_list) + white_tools/$opaque/$additive.
extern Material *R_BeginRegistrationInternal();

// Map / status pipeline.
extern void       Load_Materials();                              // texwnd.cpp (0x45ae40) bulk material load
extern void       Get_MaterialNames();                            // qe3.cpp (0x45aaa0), Load_Textures head
extern unsigned long FillTextureMenu();                          // texwnd.cpp (0x45b260) Usage/Locale/Surface filter submenus
extern LRESULT    Texture_ShowAll();                             // texwnd.cpp (0x45b730) un-hide all materials
extern void       Texture_ResetPosition();                       // texwnd.cpp (0x45b650) reset texture-browser scroll
extern void       Map_LoadFromFile( const char *path );          // map.cpp (0x486680)
extern void       Map_NewMap();                                  // map.cpp (0x486110)
extern void       Map_New();                                     // entity.cpp (0x4870C0) Fileâ†’New
extern entity_s  *world_entity;                                  // map.cpp (0x25D5B30)
extern void       Prefab_LevelBack();                            // map.cpp (0x489D50)
extern void       Map_SaveFile( const char *path, char a1, char a2 ); // map.cpp
extern eclass_t  *Eclass_ForName( int has_brushes, const char *name ); // eclass.cpp
extern selbrush_t active_brushes;                                // map.cpp (0x23F189C)
extern void       QE_CountBrushesAndUpdateStatusBar();           // qe3.cpp
extern void       Entity_UpdateSelection();                      // win_ent.cpp (UpdateSelection(-1,NULL))
extern void       Z_CenterOnMap();                               // z.cpp
// (Cam_CenterOnMap(CCamWnd*) extern retired with U-RIP â€” the forwarder died with the class)
extern void       Undo_Undo();                                   // undo.cpp
extern void       Undo_Redo();                                   // undo.cpp
extern int        g_nUpdateBits;                                 // engine_stubs.cpp (0x25D5A74)

// â”€â”€â”€ U-GLOBALS: the shell-agnostic viewport entry points (camwnd.cpp) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Ed_Camera() / Ed_ActiveXY() (xywnd.h) never return NULL, so the command handlers below
// reach the camera + XY state without going through CMainFrame's child-window pointers.
extern camera_s  *Ed_Camera();                                   // camwnd.cpp
extern void       CamWnd_BuildMatrix();                          // was CCamWnd::Cam_BuildMatrix
extern void       CamWnd_MouseControl( float dtime );            // was Cam_MouseControl( cam, dt )
extern void       CamWnd_Scroll( float amount );                 // was CCamWnd_Scroll( frame, amt )
extern void       CamWnd_ChangeFloor( int a2 );                  // was CCamWnd::Cam_ChangeFloor( cam, a2 )
extern void       CamWnd_RegionsForSelected();                   // was Regions_ForSelected( cam )
extern void       CamWnd_AddLightPreview( selbrush_t *inst, int arg2, const orientation_t *orient );
extern int        CamWnd_RemoveLightPreview( selbrush_t *removed );
extern void       CamWnd_ClearLightPreviews();                   // was m_pCamWnd->light_preview_count = 0

// Console sink â€” the binary's CMainFrame::OnCreate installs console_print as the
// global console_stuff callback (console_stuff = &console_print, 0x420A54), so every
// editor-log Com_PrintMessage / Com_PrintError / R_Warn line lands in the console
// pane (d_hwndEdit).  cmdlib.cpp owns console_stuff + SetConsoleHandler (0x40A9E0);
// console_print lives in win_qe3.cpp.
extern void       console_print( const char *fmt, va_list args );          // win_qe3.cpp (0x499D80)
extern void       SetConsoleHandler( void ( *fn )( const char *, va_list ) );// cmdlib.cpp (0x40A9E0)
extern BOOL       LoadRegistryInfo( const char *pszName, void *pvBuf, long *plSize ); // win_qe3.cpp 0x4999C0

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  U-CMD-1 â€” the shell-agnostic frame layer.  Everything a command needs (the idle pump,
//  the invalidation broadcast, the hotkey lookup, the id->action dispatch) is a FREE
//  function.  Post-U-RIP the raw-Win32 shell (radiant_main.cpp) is their only caller.
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// U-BOOT: every declaration this block used to carry now lives in radiant_frame.h (included
// above), so the raw-Win32 shell and the MFC shell read the SAME prototypes â€” no per-TU
// re-declaration to drift.  The bodies are unchanged and still live in this file:
//   Radiant_UpdateWindows / RoutineProcessing / TryHotkey / ExecCommand /
//   DispatchCommandDirect, the XYWnd_* five, and the frame helpers
//   (Radiant_SetGridStatus / PicMip / CheckTextureScale / SetEntityCheck).

// radiantFrameState_t â€” the three frame members those free functions still need.  The frame
// is a singleton (one editor window), so this is the one file-scope instance.  U-RIP deleted
// the class that used to own them; the provenance cites below stay.
//   doLoop       = CMainFrame::m_bDoLoop       (RoutineProcessing gate, set at the OnCreate tail)
//   camPreview   = CMainFrame::m_bCamPreview   (ToggleCamera 0x423A50 / OnUpdateViewCameraupdate 0x4264B0)
//   currentStyle = CMainFrame::m_nCurrentStyle (the binary's QE4 window style; this port pins it
//                  at 1 and never writes it â€” win_ent.cpp:1690 still reads the MEMBER, same
//                  constant, so the member has to stay until that call site is swept)
// EXTERNAL READERS checked before redirecting (repo-wide grep): m_bDoLoop â€” none;
// m_bCamPreview â€” none; m_nCurrentStyle â€” win_ent.cpp:1690 only (reported: that call site
// still names the deleted member and is U-RIP-2's to sweep).
// U-BOOT: the STRUCT lives in radiant_frame.h (the shell's boot writes .doLoop); the single
// instance still lives here.
radiantFrameState_t g_radiantFrameState;

// The frame's menu bar as a raw HMENU â€” the binary's GetMenu()/CMenu::CheckMenuItem pairs are
// plain ::GetMenu/::CheckMenuItem calls, so every menu radio/check update below is
// shell-agnostic (the raw-Win32 shell owns the same IDR_MENU_QUAKE3 bar).
static HMENU Radiant_FrameMenu()
{
    return ::GetMenu( g_qeglobals.d_hwndMain );
}

// Show/hide one docked child view by HWND â€” the shell-agnostic form of the binary's
// IsWindowVisible + ShowWindow(SW_HIDE/SW_SHOW) pair in the Viewâ†’Toggleâ†’* handlers
// (0x426A40 / 0x426A90 / 0x426AE0 / 0x426B30).  NULL hwnd = no-op, as the MFC guards were.
static void Radiant_ToggleChildVisible( HWND hwnd )
{
    if ( !hwnd )
        return;
    ::ShowWindow( hwnd, ::IsWindowVisible( hwnd ) ? SW_HIDE : SW_SHOW );
}

// Editor trace log (%TEMP%\radiant_firstlight.log) - append+flush+close per call so the
// last line survives a crash inside a window callback.  Startup/error lines only.
void Radiant_FL_Log( const char *fmt, ... )
{
    char buf[1024];
    va_list ap; va_start( ap, fmt );
    _vsnprintf( buf, sizeof( buf ), fmt, ap );
    va_end( ap );
    char path[MAX_PATH], tmp[MAX_PATH];
    GetTempPathA( sizeof( tmp ), tmp );
    _snprintf( path, sizeof( path ), "%sradiant_firstlight.log", tmp );
    FILE *f = fopen( path, "a" );
    if ( f ) { fputs( buf, f ); fputc( '\n', f ); fclose( f ); }
}

// U-BOOT: de-static'd â€” the raw-Win32 boot opens the same trace file (radiant_main.cpp).
void Radiant_FL_LogReset()
{
    char path[MAX_PATH], tmp[MAX_PATH];
    GetTempPathA( sizeof( tmp ), tmp );
    _snprintf( path, sizeof( path ), "%sradiant_firstlight.log", tmp );
    FILE *f = fopen( path, "w" );
    if ( f ) { fputs( "=== radiant trace ===\n", f ); fclose( f ); }
}

// Resolve a code address to "module!0xoffset" - the module it belongs to (NOT necessarily
// the .exe) + its RVA within it.  scripts\radiant\symbolicate.ps1 resolves it via dbghelp.
static void Radiant_FL_ResolveAddr( void *addr, char *buf, size_t bufsz )
{
    HMODULE mod = NULL;
    if ( GetModuleHandleExA(
             GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
             (LPCSTR)addr, &mod ) && mod )
    {
        char modpath[MAX_PATH] = { 0 };
        const char *modname = "?";
        if ( GetModuleFileNameA( mod, modpath, sizeof( modpath ) ) )
        {
            const char *slash = strrchr( modpath, '\\' );
            modname = slash ? slash + 1 : modpath;
        }
        _snprintf( buf, bufsz, "%s!0x%X", modname,
            (unsigned)( (uintptr_t)addr - (uintptr_t)mod ) );
    }
    else
    {
        _snprintf( buf, bufsz, "%p!?? (no module)", addr );
    }
    buf[bufsz - 1] = 0;
}

// VEH: log the faulting address of hard exceptions (AV / illegal-instr / int3 / fastfail) as
// module!offset, the read/write target for AVs, and a CaptureStackBackTrace caller chain.
// U-BOOT: de-static'd â€” the raw-Win32 boot installs the same handler (radiant_main.cpp).
LONG WINAPI Radiant_FL_Veh( EXCEPTION_POINTERS *ep )
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if ( code == 0xC0000005 /*AV*/ || code == 0xC000001D /*illegal instr*/ ||
         code == 0x80000003 /*int3/breakpoint*/ || code == 0xC0000409 /*fastfail/stack overrun*/ )
    {
        void *addr = ep->ExceptionRecord->ExceptionAddress;
        char faultloc[320];
        Radiant_FL_ResolveAddr( addr, faultloc, sizeof( faultloc ) );

        if ( code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2 )
        {
            // ExceptionInformation[0]: 0=read, 1=write, 8=DEP/execute; [1]=target VA.
            ULONG_PTR op = ep->ExceptionRecord->ExceptionInformation[0];
            ULONG_PTR va = ep->ExceptionRecord->ExceptionInformation[1];
            Radiant_FL_Log( "*** EXCEPTION 0x%08X at %s  (%s 0x%p) ***",
                code, faultloc, op == 1 ? "write" : op == 8 ? "exec" : "read", (void *)va );
        }
        else
        {
            Radiant_FL_Log( "*** EXCEPTION 0x%08X at %s ***", code, faultloc );
        }

        // Short backtrace; the first 2-3 frames are this VEH + KiUserExceptionDispatcher.
        void *frames[16] = { 0 };
        USHORT n = CaptureStackBackTrace( 0, 16, frames, NULL );
        Radiant_FL_Log( "    --- backtrace (%u frames) ---", (unsigned)n );
        for ( USHORT i = 0; i < n; ++i )
        {
            char fl[320];
            Radiant_FL_ResolveAddr( frames[i], fl, sizeof( fl ) );
            Radiant_FL_Log( "    [%2u] %p  %s", (unsigned)i, frames[i], fl );
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}


// Default editor colours/grid so the views are legible without a registry/prefs load
// (Phase 6). colours: [1] XY background, [2] grid minor, [3] grid major, [4] camera
// background, [8] grid text, [9] brushes, [10] selected, [13] active view name.
// U-BOOT: de-static'd â€” both boots seed the same grid/palette defaults.
void Radiant_SetDefaultGridState()
{
    g_qeglobals.d_showgrid = true;
    // CreateQEChildren 0x4219d1: d_gridsize = 5 (grid_sizes[5] == 16.0) is the startup default.
    g_qeglobals.d_gridsize = 5;

    // Default palette, verbatim from MFCCreate (0x499600): WHITE XY background, grey grid,
    // BLACK text/brushes.  XY_DrawGrid draws each grid colour as its own
    // R_AddCmdSetMaterialColor batch, so the dark lines survive on the white background
    SavedInfo_t &si = g_qeglobals.d_savedinfo;
    si.iSize     = 0x2C4;
    si.iTextMenu = 32993;
    si.d_picmip  = 2;
    static const float kColors[23][4] = {
        { 0.25f, 0.25f, 0.25f, 1.0f },   //  0
        { 1.00f, 1.00f, 1.00f, 1.0f },   //  1  XY background (WHITE)
        { 0.75f, 0.75f, 0.75f, 1.0f },   //  2  grid minor (light grey)
        { 0.50f, 0.50f, 0.50f, 1.0f },   //  3  grid major (mid grey)
        { 0.25f, 0.25f, 0.25f, 1.0f },   //  4  camera background
        { 0.00f, 0.00f, 0.00f, 0.0f },   //  5  (unset by MFCCreate)
        { 0.00f, 0.00f, 0.00f, 0.0f },   //  6  (unset)
        { 0.00f, 0.00f, 1.00f, 1.0f },   //  7  block grid (blue)
        { 0.00f, 0.00f, 0.00f, 1.0f },   //  8  grid text (BLACK)
        { 0.00f, 0.00f, 0.00f, 1.0f },   //  9  brushes (BLACK)
        { 1.00f, 0.00f, 0.00f, 1.0f },   // 10  selected (red)
        { 1.00f, 0.25f, 0.25f, 0.25f },  // 11
        { 0.00f, 0.00f, 1.00f, 1.0f },   // 12  (blue)
        { 0.50f, 0.00f, 0.75f, 1.0f },   // 13  active view name (purple)
        { 0.00f, 0.60f, 0.00f, 1.0f },   // 14  (green)
        { 0.00f, 0.00f, 0.00f, 0.0f },   // 15  (unset)
        { 1.00f, 0.25f, 0.25f, 0.25f },  // 16
        { 0.75f, 0.75f, 0.75f, 1.0f },   // 17
        { 0.75f, 0.75f, 0.75f, 1.0f },   // 18
        { 0.50f, 0.60f, 0.00f, 1.0f },   // 19
        { 0.65f, 0.00f, 0.00f, 1.0f },   // 20
        { 0.85f, 0.00f, 0.85f, 1.0f },   // 21
        { 0.80f, 0.60f, 0.00f, 1.0f },   // 22
    };
    for ( int i = 0; i < 23; ++i )
        for ( int c = 0; c < 4; ++c )
            si.colors[i][c] = kColors[i][c];
}

// KISAK: the binary's OnCreateClient (0x422480) builds nested CSplitterWnds - outer 2 rows
// (views 85% / console CEdit 15%, whose HWND becomes d_hwndEdit), inner 3 columns.  The
// renderer multi-window path is driven by HWND, not splitter panes, so the port reproduces
// that arrangement by hand: a bottom console strip, then Z | XY | (Cam over Tex).
// U-BOOT: EdLayout moved to radiant_frame.h (plain RECT â€” the raw shell has no CRect).
// Width (px) of the splitter gutters left between adjacent panes (the draggable bars).
static const int GAP = 4;

// Splitter bars: the panes are laid out by hand with GAP-wide gutters (client area covered by
// no child pane); the WM_SETCURSOR / WM_LBUTTON* / WM_MOUSEMOVE handlers drag these fractions
// (defaults = the binary's OnCreateClient splits).
static float s_fracConsole = 0.15f;  // console height / inner (views+console) height
static float s_fracZ       = 0.05f;  // Z-strip width / cx (IDA OnCreateClient col0 = 5%)
static float s_fracRight   = 0.25f;  // right-column width / cx (IDA OnCreateClient col2 = 25%)
static float s_fracCam     = 0.60f;  // camera height / views height (IDA split3 row0 = 60%)
static float s_fracEnt     = 0.20f;  // entity-inspector height / views height
// Gutter geometry (client coords) cached by Radiant_ComputeLayout for the hit-test.
static int s_barV1x, s_barV2x;            // vertical gutters: Z|XY, XY|right
static int s_barH1y, s_barH2y, s_barH3y;  // horizontal gutters: cam|ent, ent|tex, views|console
static int s_barViewsTop, s_barViewsBot;  // y-span of the vertical gutters
static int s_barRightL, s_barRightR;      // x-span of the cam|ent / ent|tex gutters
static int s_barCx, s_barInnerH, s_barTopInset, s_barTopH;  // drag-math references
static int s_dragBar = 0;                 // gutter currently being dragged (0 = none)
static inline float Clampf( float v, float lo, float hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }

struct RadiantSplitInfo
{
    int min;
    int cur;
};

static bool Radiant_LoadSplitInfo( const char *name, RadiantSplitInfo *out )
{
    long size = sizeof( *out );
    return LoadRegistryInfo( name, out, &size ) && size >= (long)sizeof( *out ) && out->cur > 0;
}

// CMainFrame::OnCreateClient 0x422480 seeds the three splitter trees with 85/15,
// 5/70/25, 60/40, then LoadRegistryInfo overrides Row_*/Col_* with persisted
// CSplitterWnd row/column info.  The port's panes are manual HWNDs, so translate the
// same saved current sizes into the fraction state that Radiant_ComputeLayout consumes.
static void Radiant_LoadSavedSplitterLayout()
{
    static bool s_loaded = false;
    if ( s_loaded )
        return;
    s_loaded = true;

    RadiantSplitInfo row0, row1;
    if ( Radiant_LoadSplitInfo( "Radiant::Split::Row_0", &row0 ) &&
         Radiant_LoadSplitInfo( "Radiant::Split::Row_1", &row1 ) )
    {
        int total = row0.cur + row1.cur;
        if ( total > 0 )
            s_fracConsole = Clampf( (float)row1.cur / (float)total, 0.01f, 0.80f );
    }

    RadiantSplitInfo col0, col1, col2;
    if ( Radiant_LoadSplitInfo( "Radiant::Split2::Col_0", &col0 ) &&
         Radiant_LoadSplitInfo( "Radiant::Split2::Col_1", &col1 ) &&
         Radiant_LoadSplitInfo( "Radiant::Split2::Col_2", &col2 ) )
    {
        int total = col0.cur + col1.cur + col2.cur;
        if ( total > 0 )
        {
            s_fracZ     = Clampf( (float)col0.cur / (float)total, 0.005f, 0.40f );
            s_fracRight = Clampf( (float)col2.cur / (float)total, 0.01f, 0.85f );
        }
    }

    RadiantSplitInfo split3row0, split3row1;
    if ( Radiant_LoadSplitInfo( "Radiant::Split3::Row_0", &split3row0 ) &&
         Radiant_LoadSplitInfo( "Radiant::Split3::Row_1", &split3row1 ) )
    {
        int total = split3row0.cur + split3row1.cur;
        if ( total > 0 )
            s_fracCam = Clampf( (float)split3row0.cur / (float)total, 0.05f, 0.95f );
    }
}

// U-BOOT: de-static'd and re-typed to HWND (CWnd::SetWindowPlacement is a one-line wrapper
// over ::SetWindowPlacement, so the body is identical for both shells).
void Radiant_RestoreMainWindowPlacement( HWND frame )
{
    WINDOWPLACEMENT wp;
    memset( &wp, 0, sizeof( wp ) );
    wp.length = sizeof( wp );
    long size = sizeof( wp );
    if ( LoadRegistryInfo( "Radiant::MainWindowPlace", &wp, &size ) && size >= (long)sizeof( wp ) )
    {
        wp.length = sizeof( wp );
        ::SetWindowPlacement( frame, &wp );
    }
}

// topInset reserves a strip at the top of the frame for the docked texture bar (the QE4
// views + console lay out below it).  The layout is computed for the reduced height and then
// shifted down by topInset.
// U-BOOT: de-static'd (the raw shell's WM_SIZE recomputes the same layout); the topInset
// default argument now lives on the radiant_frame.h declaration.
EdLayout Radiant_ComputeLayout( int cx, int cy, int topInset )
{
    Radiant_LoadSavedSplitterLayout();

    if ( topInset < 0 ) topInset = 0;
    if ( topInset > cy - 64 ) topInset = 0;   // never starve the views
    cy -= topInset;
    if ( cx < 64 ) cx = 64;
    if ( cy < 64 ) cy = 64;

    // Bottom console strip (the binary's m_wndSplit row 1, 15% of the frame). Clamp so a
    // small window keeps usable views above it and never collapses the console to 0.
    int conH = (int)( cy * s_fracConsole );
    if ( conH < 5 ) conH = 5;              // IDA SetRowInfo row1 min = 5
    if ( conH > cy - 50 ) conH = cy - 50;  // IDA row0 min = 50
    if ( conH < 1 ) conH = 1;
    int topH = cy - conH;                   // the views occupy [0, topH)
    if ( topH < 64 ) { topH = cy; conH = 0; }

    // Binary OnCreateClient 0x422480 (nView 0, after the left/right pane-ID swap): Z strip 5%
    // left, XY grid 70% middle, right column 25% = Camera over texture browser.  (The binary
    // hides the Entity window in combined view; here it floats - see below.)
    int zW = (int)( cx * s_fracZ );         // Z strip (left)
    if ( zW < 10 ) zW = 10;                 // IDA split2 col0 min = 10
    int rW = (int)( cx * s_fracRight );     // right column (camera + inspector + textures)
    if ( rW < 25 ) rW = 25;                 // IDA split2 col2 min = 25
    // Cap only on the XY grid's MINIMUM width: the binary's CSplitterWnd has no maximum for
    // the right column, so let it grow to whatever the drag handler allows (0.85).
    const int xyMinW = 100;                 // IDA split2 col1 min = 100
    if ( rW > cx - zW - xyMinW ) rW = cx - zW - xyMinW;
    if ( rW < 25 ) rW = 25;                 // re-assert the min (tiny frames)
    int xyW = cx - zW - rW;                 // XY grid (the big middle pane)
    if ( xyW < 40 ) xyW = 40;               // absolute safety floor

    const int zRight  = zW;
    const int xyRight = zW + xyW;

    // Right column = Camera over Texture browser.  The Entity inspector FLOATS (toggled by
    // N/O/T/F), so s_fracEnt is unused.
    int rCamH = (int)( topH * s_fracCam );  // Camera on top, Texture browser below
    if ( rCamH < 15 ) rCamH = 15;           // IDA split3 min = 15/15
    if ( rCamH > topH - 15 ) rCamH = topH - 15;
    const int camBottom = rCamH;

    // U-BOOT: CRect::SetRect / ::OffsetRect became the Win32 ::SetRect / ::OffsetRect those
    // CRect members wrap (EdLayout is plain RECT now) â€” same arguments, same results.
    EdLayout L;
    ::SetRect( &L.z,   0,              0,               zRight,  topH      );
    ::SetRect( &L.xy,  zRight  + GAP,  0,               xyRight, topH      );
    ::SetRect( &L.cam, xyRight + GAP,  0,               cx,      camBottom );
    ::SetRect( &L.tex, xyRight + GAP,  camBottom + GAP, cx,      topH      );
    ::SetRect( &L.ent, 0, 0, 0, 0 );        // entity inspector floats (toggled) â€” not docked
    ::SetRect( &L.con, 0,              topH + GAP,      cx,      cy        );   // full-width console
    if ( topInset > 0 )
    {
        ::OffsetRect( &L.xy,  0, topInset );  ::OffsetRect( &L.cam, 0, topInset );
        ::OffsetRect( &L.z,   0, topInset );  ::OffsetRect( &L.tex, 0, topInset );
        ::OffsetRect( &L.ent, 0, topInset );  ::OffsetRect( &L.con, 0, topInset );
    }
    // Cache gutter geometry (client coords) for the splitter-bar hit-test (CMainFrame mouse
    // handlers).  cy here is the inner (views+console) height after the topInset subtraction.
    s_barCx = cx; s_barInnerH = cy; s_barTopInset = topInset; s_barTopH = topH;
    s_barV1x = L.z.right;   s_barV2x = L.xy.right;
    s_barH1y = L.cam.bottom; s_barH2y = -10000; s_barH3y = L.con.top;  // H1 = cam|tex; ent floats (no H2)
    s_barViewsTop = L.xy.top; s_barViewsBot = L.xy.bottom;
    s_barRightL = L.cam.left; s_barRightR = L.cam.right;
    return L;
}

// Set once init is fully up (device created). OnPaint only renders when true.
bool g_radiantFirstLightRendererReady = false;

// Engine subsystem bring-up (once, BEFORE the windows/renderer): the slice of InitInstance /
// Com_Init that precedes window creation.  NOT the renderer - R_BeginRegistrationInternal owns
// R_InitRenderCommands + Com_InitHunkMemory + R_InitEditor + the per-window attaches.
// U-BOOT: de-static'd â€” the raw-Win32 boot runs this identical subsystem bring-up first.
void Radiant_EngineInit()
{
    Radiant_FL_Log( "EngineInit: Com_InitThreadData(MAIN)" ); Com_InitThreadData( THREAD_CONTEXT_MAIN );
    Radiant_FL_Log( "EngineInit: Sys_InitializeCriticalSections" ); Sys_InitializeCriticalSections();
    Radiant_FL_Log( "EngineInit: track_init" );          track_init();
    Radiant_FL_Log( "EngineInit: SL_Init" );             SL_Init();
    Radiant_FL_Log( "EngineInit: Cbuf_Init" );           Cbuf_Init();
    Radiant_FL_Log( "EngineInit: Cmd_Init" );            Cmd_Init();
    Radiant_FL_Log( "EngineInit: Dvar_Init" );           Dvar_Init();
    Radiant_FL_Log( "EngineInit: Radiant_RegisterGroupCDvars" ); Radiant_RegisterGroupCDvars();

    // gfxCfg is zero-initialised in the radiant build (the client's SetupGfxConfig is not
    // compiled in). R_InitRenderCommands (inside R_BeginRegistrationInternal) sizes its
    // command/scene buffers by gfxCfg.maxClientViews â€” 0 would allocate nothing and fault.
    Radiant_FL_Log( "EngineInit: SetupGfxConfig" );
    gfxCfg.maxClientViews     = 1;
    gfxCfg.entCount           = 2208;
    gfxCfg.entnumNone         = ENTITYNUM_NONE;
    gfxCfg.entnumOrdinaryEnd  = ENTITYNUM_WORLD;
    gfxCfg.threadContextCount = THREAD_CONTEXT_COUNT;
    gfxCfg.critSectCount      = CRITSECT_COUNT;

    // FS before the hunk, as in the binary (Hunk_Init runs later inside
    // R_BeginRegistrationInternal - a second Com_InitHunkMemory here trips its !s_hunkData
    // assert).  useFastFile is already registered, so FS's IsFastFileLoad reads a valid dvar.
    Radiant_FL_Log( "EngineInit: FS_InitFilesystem" );   FS_InitFilesystem();
    Radiant_FL_Log( "EngineInit: done" );
}

// Status-bar sink (the real MainFrm_SetStatusText; replaces the engine_stubs no-op).
void MainFrm_SetStatusText( int pane, const char *text )
{
    // NO-MFC: no status bar yet (U-BOOT skipped it) â€” text goes nowhere, same as the
    // MFC no-frame arm. The ImGui shell window is the natural future sink.
    (void)pane; (void)text;
}

// â”€â”€ Map open (Fileâ†’Open + the cmdline startup share this) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static char s_currentMapPath[MAX_PATH] = "";

// (Radiant_CenterXYOnMap deleted with U-RIP: dead since HEAD per U-GLOBALS, and the
// last writer of the deleted CXYWnd class members.)

// U-BOOT: de-static'd â€” the raw shell gates its cmdline map on the same test.
bool Radiant_PathLooksLikeMap( const char *p )
{
    if ( !p ) return false;
    size_t n = strlen( p );
    return n > 4 && _stricmp( p + n - 4, ".map" ) == 0;
}

// SetKeyValue (entity.cpp 0x483690).  Its side-effect chain is inert for "mapspath":
// Checkkey_Model fires only for model/angles/modelscale, Checkkey_Color only for
// _color/targetname.
extern void SetKeyValue( entity_s_def *e, const char *key, const char *value );

// LayerdMatWnd (layeredmaterials.cpp 0x416D40) - load the library named by the project
// entity's "layeredmaterials" epair into lyrMtlGlob.Layers[] (the binary calls it from
// QE_LoadProject right after Load_Textures).
extern signed int LayerdMatWnd();

// Project "mapspath" seed for prefab loading on a GUI map-open.  A stock .map references its
// geometry + prefabs via misc_prefab "model" keys, which Prefab_Load resolves as
// <project mapspath>\<name>.  Seed mapspath = the opened map's own directory (where the stock
// prefab tree lives), matching the real editor's project mapspath (= the map_source dir).
static void Radiant_SetProjectMapsPath( const char *mapPath )
{
    if ( !mapPath )
        return;

    // Directory portion of mapPath (strip the trailing "\<file>.map").
    char dir[MAX_PATH];
    _snprintf( dir, sizeof( dir ), "%s", mapPath );
    dir[sizeof( dir ) - 1] = '\0';
    char *slash = strrchr( dir, '\\' );
    char *fslash = strrchr( dir, '/' );
    if ( fslash && ( !slash || fslash > slash ) )
        slash = fslash;
    if ( !slash )
        return;                       // no directory component â€” leave mapspath as is
    *slash = '\0';

    // Allocate the project entity DEF on first use (140-byte entity_s_def, brush-list
    // sentinels self-linked â€” matching Entity_Create's LABEL_10 def init).
    entity_s_def *proj = g_qeglobals.d_project_entity;
    if ( !proj )
    {
        proj = (entity_s_def *)operator new( 0x8Cu );
        memset( proj, 0, 0x8Cu );
        proj->brushes.prev         = (selbrush_t *)&proj->def;
        proj->def = (entity_s *)&proj->def;
        g_qeglobals.d_project_entity = proj;
    }

    SetKeyValue( proj, "mapspath", dir );

    // The binary's QE_LoadProject parses cod4.prj's
    //   "layeredmaterials" "cod4_layered_material_library.txt"
    // epair then calls LayerdMatWnd() to load that library.  Seed the same epair and run the
    // loader.  LayerdMatWnd's LoadFile is a plain fopen relative to the editor CWD (no FS
    // search paths), so the file must sit beside the exe; absent, the library stays empty.
    SetKeyValue( proj, "layeredmaterials", "cod4_layered_material_library.txt" );
    LayerdMatWnd();
}

// Startup project load: resolve cod4.prj and run the full QE_LoadProject_ParseFile parse
// (populate d_project_entity with every epair, resolve the search-path keys, register
// fs_basegame/game).  KISAK: the binary's Tier 1 — the registry "Prefs"/"LastProject" value
// (Set Startup Project) — is REMOVED: it let a stale registry key silently point the editor
// at a .prj (and through it a data directory) anywhere on disk, which was error-prone.  The
// editor now always loads the cod4.prj beside the exe; game data lives next to the exe
// (fs_basepath is exe-derived in com_files.cpp and is no longer overridable from a .prj).
static bool g_projectLoadedAtStartup = false;
// U-BOOT: de-static'd â€” both boots load the project file at the same point.
bool Radiant_LoadProjectAtStartup()
{
    extern signed int QE_LoadProject_ParseFile( const char *path );   // qe3.cpp 0x48BAB0
    extern const char *Dvar_GetString( const char *dvarName );        // qcommon (fs_basepath)

    // The stock cod4.prj beside the editor.  Try the CWD first (faithful: LoadFileNoCrash's
    // plain fopen), then EXE-anchored locations — the exe's own directory, then
    // <fs_basepath>\cod4.prj and <fs_basepath>\bin\cod4.prj (fs_basepath is exe-derived,
    // com_files.cpp).  The exe-dir attempt matters because the CWD is NOT the exe dir under
    // a debugger or a shortcut with a different "Start in".
    if ( QE_LoadProject_ParseFile( "cod4.prj" ) )
    {
        g_projectLoadedAtStartup = true;
        Radiant_FL_Log( "QE_LoadProject: loaded cod4.prj (cwd)" );
        return true;
    }
    {
        char exeDir[MAX_PATH];
        if ( ::GetModuleFileNameA( NULL, exeDir, sizeof( exeDir ) ) )
        {
            char *slash = strrchr( exeDir, '\\' );
            if ( slash ) *slash = 0;
            char prjPath[MAX_PATH];
            _snprintf( prjPath, sizeof( prjPath ), "%s\\cod4.prj", exeDir );
            prjPath[sizeof( prjPath ) - 1] = 0;
            if ( QE_LoadProject_ParseFile( prjPath ) )
            {
                g_projectLoadedAtStartup = true;
                Radiant_FL_Log( "QE_LoadProject: loaded %s", prjPath );
                return true;
            }
        }
    }
    const char *base = Dvar_GetString( "fs_basepath" );
    if ( base && *base )
    {
        static const char *const s_prjSubdirs[] = { "", "bin\\" };
        for ( int i = 0; i < 2; ++i )
        {
            char prjPath[MAX_PATH];
            _snprintf( prjPath, sizeof( prjPath ), "%s\\%scod4.prj", base, s_prjSubdirs[i] );
            prjPath[sizeof( prjPath ) - 1] = 0;
            if ( QE_LoadProject_ParseFile( prjPath ) )
            {
                g_projectLoadedAtStartup = true;
                Radiant_FL_Log( "QE_LoadProject: loaded %s", prjPath );
                return true;
            }
        }
    }

    Radiant_FL_Log( "QE_LoadProject: no .prj found â€” using default seed" );
    return false;
}

void Radiant_OpenMap( const char *path )
{
    if ( !Radiant_PathLooksLikeMap( path ) )
        return;
    Radiant_FL_Log( "OpenMap: %s", path );
    // Seed the project mapspath from the map's own directory so misc_prefab "model"
    // refs (the *_geo.map geometry + prefabs/) resolve in Prefab_Load on this load.
    Radiant_SetProjectMapsPath( path );
    Map_LoadFromFile( path );
    _snprintf( s_currentMapPath, sizeof( s_currentMapPath ), "%s", path );

    if ( g_qeglobals.d_hwndMain )        // NO-MFC: same liveness gate, HWND-based
    {
        Z_CenterOnMap();
        // NO Cam_CenterOnMap here: Map_LoadFromFile (0x486680) already placed camera.origin at
        // the info_player_start (else deathmatch, else origin) +60 Z and set camera.angles from
        // that entity, exactly as the binary does.  (Radiant_CenterXYOnMap / Z_CenterOnMap keep
        // the 2D views map-centered - a port convenience; the binary points the XY view at the
        // player start too, via the m_pXYWnd write in 0x486680.)
        // Title = "CoD4Radiant - <map>".
        char title[MAX_PATH + 32];
        _snprintf( title, sizeof( title ), "CoD4Radiant - %s", path );
        ::SetWindowTextA( g_qeglobals.d_hwndMain, title );   // was g_pParentWnd->SetWindowText (same HWND)
        // The eclass list grows as the map's entity classes are registered (Eclass_ForName
        // during parse) â€” refresh the entity-window list so they appear.
        // NO-MFC: no entity window; the ImGui entity panel re-gathers the eclass list
        // every frame, so only the selection snapshot needs the poke.
        Entity_UpdateSelection();
    }
    QE_CountBrushesAndUpdateStatusBar();
    g_nUpdateBits = W_ALL;

    // KISAK operator switch: RADIANT_STARTUP_CAM="x y z pitch yaw roll" places the 3D camera
    // after the load and rebuilds its view basis (a cmdline map bypasses radiantapp.cpp's
    // RADIANT_STARTUP_MAP path, so the hook is repeated here).
    if ( const char *camStr = getenv( "RADIANT_STARTUP_CAM" ) )
    {
        // U-GLOBALS: Ed_Camera() is never NULL, so the shell guard is gone.
        float x = 0, y = 0, z = 0, pi = 0, ya = 0, ro = 0;
        if ( sscanf( camStr, "%f %f %f %f %f %f", &x, &y, &z, &pi, &ya, &ro ) >= 3 )
        {
            camera_s *cam = Ed_Camera();
            cam->origin[0] = x;
            cam->origin[1] = y;
            cam->origin[2] = z;
            cam->angles[0] = pi;
            cam->angles[1] = ya;
            cam->angles[2] = ro;
            CamWnd_BuildMatrix();
            Radiant_FL_Log( "STARTUP_CAM: camera.origin=(%.1f %.1f %.1f) angles=(%.1f %.1f %.1f)",
                            x, y, z, pi, ya, ro );
        }
    }

}

// U-BOOT: Radiant_CheckGridMenu / Radiant_CheckMenu are declared in radiant_frame.h now
// (de-static'd â€” the raw shell's boot seeds the same menu check marks).

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  U-BOOT â€” three boot steps that used to be INLINE BLOCKS of CMainFrame::OnCreate.
//  They are free functions now so the raw-Win32 boot (radiant_main.cpp) runs the SAME
//  bodies in the same order instead of a second copy that could drift.  OnCreate calls
//  them at exactly the points the blocks sat (step numbers kept).
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// OnCreate step 6a-pre) The head of Load_Textures (0x45d140) that the port skips by calling
//     Load_Materials() directly: seed g_qeglobals.random_texture_stuff[0..2] (the per-edit-
//     layer "current texdef" a new brush's faces are stamped from) with the default
//     materials + sample sizes.  Without it the current texdef stays ZERO (radMtl == NULL),
//     so a brush drawn before the first texture-window click gets a degenerate MaterialDef -
//     it renders untextured AND is unpickable.
void Radiant_SeedCurrentTexdefs()
{
    extern void SetMaterial( const char *name, patchMesh_material *out );          // materialdef.cpp 0x4315c0
    extern int  Init_MaterialLayer( MaterialDef *channel, MaterialDef *src );      // materialdef.cpp 0x472c00
    curTexWndLayer_t *rts = g_qeglobals.random_texture_stuff;
    Get_MaterialNames();       // 0x45d143
    rts[0].sampleSize = 0.25f;   // 0x45d14e
    rts[1].sampleSize = 16.0f;   // 0x45d164
    g_qeglobals.current_edit_layer = 0;   // 0x45d16a
    rts[2].sampleSize = 0.25f;   // 0x45d174
    SetMaterial( "$default",       (patchMesh_material *)&rts[0].mtl );   // 0x45d17a
    SetMaterial( "lightmap_gray",  (patchMesh_material *)&rts[1].mtl );   // 0x45d189
    SetMaterial( "smoothing_hard", (patchMesh_material *)&rts[2].mtl );   // 0x45d198
    // Init_MaterialLayer's 2nd arg is the sample-size FLOAT BITS reinterpreted as a ptr
    // (cf. map.cpp / Face_InitMaterialChannel).
    float s0 = 0.25f, s1 = 16.0f, s2 = 0.25f;
    Init_MaterialLayer( &rts[0].mtl, *(MaterialDef **)&s0 );   // 0x45d1ac
    Init_MaterialLayer( &rts[1].mtl, *(MaterialDef **)&s1 );   // 0x45d1bf
    Init_MaterialLayer( &rts[2].mtl, *(MaterialDef **)&s2 );   // 0x45d1cf
}

// OnCreate step 6a-ter) Scan the AI-type + gametype def folders, then the weapon defs, into
//     the eclass list - the binary's QE_LoadProject runs Load_Defs("aitype") /
//     Load_Defs("maps/mp/gametypes") / ScanWeapAiFiles right after
//     Eclass_InitForSourceDirectory, so those entities appear in the entity RMB menu.
void Radiant_LoadEclassDefs()
{
    extern eclass_t *Eclass_InitForSourceDirectory( char *path );  // eclass.cpp (0x481B50)
    extern char     *ValueForKey2( int e, const char *key );       // entity.cpp 0x4825C0
    extern void      Load_Defs( const char *folder );              // eclass.cpp (0x48B6C0)
    extern void      ScanWeapAiFiles();                            // eclass.cpp (0x48BA40)
    extern const char *Dvar_GetString( const char *dvarName );     // qcommon (fs_basepath)
    // QE_LoadProject (0x48bab0) tail: the binary reads the project entity's "entitypath"
    // epair (stock cod4.prj: ".\cod4.def") and scans it FIRST via
    // Eclass_InitForSourceDirectory - which Eclass_FreeAll()s and resets g_eclass, so it
    // MUST precede the appending Load_Defs calls.
    // KISAK: the binary _findfirst's ".\cod4.def" relative to the process CWD, relying on
    // that being the install bin\.  The port's CWD is the build-output dir, so resolve
    // entitypath against <fs_basepath>\bin\ and only run the FreeAll-ing scan when the
    // resolved file EXISTS - else a missing cod4.def would WIPE the palette.
    char *entitypath = g_qeglobals.d_project_entity
        ? ValueForKey2( (int)(intptr_t)g_qeglobals.d_project_entity, "entitypath" )
        : (char *)"";
    char resolvedDef[MAX_PATH] = "";
    if ( entitypath && *entitypath )
    {
        const char *rel = entitypath;
        if ( rel[0] == '.' && ( rel[1] == '\\' || rel[1] == '/' ) )   // strip a leading ".\"
            rel += 2;
        const bool absolute = ( rel[0] == '\\' || rel[0] == '/' || ( rel[0] && rel[1] == ':' ) );
        const char *base = Dvar_GetString( "fs_basepath" );
        if ( absolute )
            _snprintf( resolvedDef, sizeof( resolvedDef ), "%s", entitypath );
        else if ( base && *base )
            _snprintf( resolvedDef, sizeof( resolvedDef ), "%s\\bin\\%s", base, rel );
        else
            _snprintf( resolvedDef, sizeof( resolvedDef ), "%s", entitypath );
        resolvedDef[sizeof( resolvedDef ) - 1] = 0;
    }
    if ( resolvedDef[0] && GetFileAttributesA( resolvedDef ) != INVALID_FILE_ATTRIBUTES )
        Eclass_InitForSourceDirectory( resolvedDef );   // loads cod4.def (base palette)
    else
        Radiant_FL_Log( "Eclass: base entitypath '%s' -> '%s' not found; keeping existing eclasses",
                        entitypath ? entitypath : "", resolvedDef );
    Load_Defs( "aitype" );
    Load_Defs( "maps/mp/gametypes" );
    ScanWeapAiFiles();
}

// OnCreate tail) SetButtonMenuStates 0x420000 runs CheckTextureScale after project load.  Its
//     Texture_ResetPosition tail selects the first visible material at (9,9), making that
//     material the current brush texture before the user draws the first brush.
void Radiant_ApplyStartupTextureScale()
{
    switch ( g_PrefsDlg->m_nTextureWindowScale )
    {
        case 10:  Radiant_CheckTextureScale( 32898 ); break;
        case 25:  Radiant_CheckTextureScale( 32897 ); break;
        case 50:  Radiant_CheckTextureScale( 32896 ); break;
        case 200: Radiant_CheckTextureScale( 32894 ); break;
        default:  Radiant_CheckTextureScale( 32895 ); break;
    }
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  U-BOOT â€” the RADIANT_STARTUP_MAP / RADIANT_STARTUP_CAM operator hooks, verbatim from
//  CRadiantApp::InitInstance (radiantapp.cpp:93-138).  They run at the very END of the boot
//  (after ShowWindow/UpdateWindow), which is where InitInstance has them.
//  The MFC shell still runs its own inline copy in radiantapp.cpp â€” that whole TU is on the
//  U-RIP kill list (plan item 2), so the duplication is transient BY DESIGN; the raw shell
//  calls THIS body.  It lives in mainfrm.cpp because camera_s is declared in mainfrm.h,
//  which the raw shell cannot include (it is MFC-laden until U-RIP folds it away).
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
void Radiant_ApplyStartupMapEnv()
{
    // radiantapp.cpp:96 â€” if RADIANT_STARTUP_MAP is set, load that map at startup (no effect
    // when unset).  Lets the first-light log capture a real map's applied camera origin +
    // render state for far-from-origin debugging (no GUI File->Open).
    const char *startupMap = getenv( "RADIANT_STARTUP_MAP" );
    if ( !startupMap )
        return;

    Map_LoadFromFile( startupMap );                                       // radiantapp.cpp:100

    // radiantapp.cpp:104 â€” RADIANT_STARTUP_CAM="x y z pitch yaw roll" repositions the 3D
    // camera after load AND rebuilds its view basis (Cam_BuildMatrix), so a headless run can
    // render a real vantage (origin alone leaves the map out-of-frustum).
    if ( const char *camStr = getenv( "RADIANT_STARTUP_CAM" ) )
    {
        float x = 0, y = 0, z = 0, pi = 0, ya = 0, ro = 0;
        if ( sscanf( camStr, "%f %f %f %f %f %f", &x, &y, &z, &pi, &ya, &ro ) >= 3 )
        {
            camera_s *cam = Ed_Camera();      // U-GLOBALS: never NULL, so no shell guard
            cam->origin[0] = x;
            cam->origin[1] = y;
            cam->origin[2] = z;
            cam->angles[0] = pi;
            cam->angles[1] = ya;
            cam->angles[2] = ro;
            CamWnd_BuildMatrix();   // rebuild vpn/vright/vup so Cam_Fov's frustum matches
        }
    }
    {
        const camera_s     *cam = Ed_Camera();
        const xywndState_t *xy  = Ed_ActiveXY();
        Radiant_FL_Log( "STARTUP_MAP %s: camera.origin=(%.1f %.1f %.1f) angles=(%.1f %.1f %.1f)  XY.origin=(%.1f %.1f %.1f) scale=%.4f",
            startupMap,
            cam->origin[0], cam->origin[1], cam->origin[2],
            cam->angles[0], cam->angles[1], cam->angles[2],
            xy->m_vOrigin[0], xy->m_vOrigin[1], xy->m_vOrigin[2],
            xy->m_fScale );
    }
}


// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  The gutter (splitter-bar) drag â€” U-CMD-2 extraction.  All four handlers only ever
//  touched the file-static gutter geometry + the fraction state above, so they are free
//  functions now and BOTH shells drive the same drag (CMainFrame's handlers below forward;
//  radiant_main.cpp's frame WndProc calls them from WM_SETCURSOR / WM_LBUTTON* / WM_MOUSEMOVE).
//  SPLIT POINT: the move handler updates the fractions and REPORTS the change instead of
//  relaying out itself â€” the relayout is the one shell-specific step (CMainFrame::Relayout-
//  Panes vs the raw shell's Radiant_RelayoutPanes), and both call it right after.
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•

// Splitter-bar hit-test on the GAP gutters (client coords): 0 = none; 1/2 = vertical
// (Z|XY, XY|right); 3/4 = horizontal (cam|ent, ent|tex) inside the right column; 5 =
// horizontal (views|console).  Geometry is cached by Radiant_ComputeLayout.
int Radiant_HitTestBar( int x, int y )
{
    const int TOL = GAP + 3;
    if ( y >= s_barViewsTop && y <= s_barViewsBot )
    {
        if ( abs( x - s_barV1x ) <= TOL ) return 1;
        if ( abs( x - s_barV2x ) <= TOL ) return 2;
    }
    if ( x >= s_barRightL && x <= s_barRightR )
    {
        if ( abs( y - s_barH1y ) <= TOL ) return 3;
        if ( abs( y - s_barH2y ) <= TOL ) return 4;
    }
    if ( abs( y - s_barH3y ) <= TOL && x >= 0 && x <= s_barCx ) return 5;
    return 0;
}

// WM_SETCURSOR over the frame's own client area: true = the resize cursor was set and the
// caller must NOT chain to its default handler.  `frame` is the window the cursor position
// is mapped against (CWnd::ScreenToClient's HWND).
bool Radiant_SplitterOnSetCursor( HWND frame )
{
    POINT p;
    ::GetCursorPos( &p );
    ::ScreenToClient( frame, &p );
    int b = Radiant_HitTestBar( p.x, p.y );
    if ( b == 1 || b == 2 ) { ::SetCursor( ::LoadCursorA( nullptr, IDC_SIZEWE ) ); return true; }
    if ( b >= 3 )           { ::SetCursor( ::LoadCursorA( nullptr, IDC_SIZENS ) ); return true; }
    return false;
}

// true = a gutter was grabbed (the mouse is captured; the caller must not chain).
bool Radiant_SplitterOnLButtonDown( HWND frame, int x, int y )
{
    int b = Radiant_HitTestBar( x, y );
    if ( !b )
        return false;
    s_dragBar = b;
    ::SetCapture( frame );
    return true;
}

// true = a drag is live and the fractions changed â€” the CALLER relays the panes out.
bool Radiant_SplitterOnMouseMove( unsigned int nFlags, int x, int y )
{
    if ( !s_dragBar || !( nFlags & MK_LBUTTON ) || s_barCx <= 0 || s_barTopH <= 0 || s_barInnerH <= 0 )
        return false;

    switch ( s_dragBar )
    {
        case 1: s_fracZ       = Clampf( (float)x / s_barCx, 0.02f, 0.40f ); break;
        case 2: s_fracRight   = Clampf( (float)( s_barCx - x ) / s_barCx, 0.12f, 0.85f ); break;  // max was 0.50 â€” that floored the XY view at ~50% width; binary's CSplitterWnd has no such cap
        case 3: s_fracCam     = Clampf( (float)( y - s_barTopInset ) / s_barTopH, 0.15f, 0.80f ); break;
        case 4: s_fracEnt     = Clampf( (float)( y - s_barH1y ) / s_barTopH, 0.08f, 0.70f ); break;
        case 5: s_fracConsole = Clampf( (float)( s_barInnerH - ( y - s_barTopInset ) ) / s_barInnerH, 0.05f, 0.80f ); break;  // max was 0.50 â€” let the XY view's height shrink past 50% too
    }
    return true;
}

// true = the drag ended here (capture released; the caller must not chain).
bool Radiant_SplitterOnLButtonUp()
{
    if ( !s_dragBar )
        return false;
    s_dragBar = 0;
    ::ReleaseCapture();
    return true;
}


// The texture bar's field refresh.  U-RIP deleted the MFC body (the bar was a CWnd embedded
// in CMainFrame); the docked strip is not up in this shell yet, so this is the no-op
// invalidation point the broadcast below still calls â€” it becomes the ImGui texture-bar
// refresh when that strip ships (U-BOOT v2).
static void Radiant_RefreshTextureBar()
{
}

// â”€â”€ The real invalidation broadcast (CMainFrame::UpdateWindows, IDB 0x427090) â”€â”€â”€â”€
// RedrawWindow each view whose W_* bit is set. RDW_UPDATENOW forces a synchronous repaint
// (so drags track the cursor). m_bCamPreview is implicit-true here.
// U-CMD-1: the four views are reached through g_qeglobals.d_hwnd{XY,Camera,Z,Texture} instead
// of CMainFrame's child pointers.  VERIFIED IDENTICAL at the assignment sites
// (each d_hwnd* WAS that child's GetSafeHwnd(), assigned once at creation and never rewritten
// anywhere in the repo; the raw creators in radiant_main.cpp assign the same HWNDs), so the
// ::RedrawWindow
// targets are byte-for-byte the same windows; the non-NULL test replaces the pointer+
// GetSafeHwnd() pair.  This is also what R_BeginRegistrationInternal attaches the device to.
void Radiant_UpdateWindows( int nBits )
{
    if ( ( nBits & ( W_XY | W_XY_OVERLAY ) ) && g_qeglobals.d_hwndXY )
        ::RedrawWindow( g_qeglobals.d_hwndXY, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW );
    if ( ( nBits & ( W_CAMERA | W_CAMERA_IFON ) ) && g_qeglobals.d_hwndCamera )
        ::RedrawWindow( g_qeglobals.d_hwndCamera, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW );
    if ( ( nBits & ( W_Z | W_Z_OVERLAY ) ) && g_qeglobals.d_hwndZ )
        ::RedrawWindow( g_qeglobals.d_hwndZ, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW );
    if ( ( nBits & W_TEXTURE ) && g_qeglobals.d_hwndTexture )
        ::RedrawWindow( g_qeglobals.d_hwndTexture, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW );

    // surfinsp: refresh the Surface Inspector edit fields whenever a view is invalidated
    // (selection / texdef edit / map load all set g_nUpdateBits).  No-op when it's closed.
    Surf_UpdateInspector();

    // texture bar: refresh its texdef readout + material name on the same invalidation
    // broadcast (the binary refreshes it from UpdateTextureBar on every current-texture
    // change).  No-op when the bar isn't up.
    Radiant_RefreshTextureBar();
}

// Idle pump (CMainFrame::RoutineProcessing 0x421a90), driven each idle from
// CRadiantApp::OnIdle: compute dtime from clock() into g_qeglobals.g_time/g_oldtime (clamp
// dt>2 -> 0.1, then dtime>0.2 -> 0.2), pump Cam_MouseControl for the RMB cursor-joystick fly,
// then flush g_nUpdateBits via UpdateWindows.  U-CMD-1: the raw-Win32 message pump (U-BOOT)
// calls this straight from its idle branch.
void Radiant_RoutineProcessing()
{
    if ( !g_radiantFrameState.doLoop )   // U-CMD-1: was m_bDoLoop
        return;

    // dtime = wall-clock seconds since last pump, clamped (binary 0x421ada..0x421b24).
    clock_t now = clock();
    double dtime = (double)now / 1000.0 - g_qeglobals.g_time;
    g_qeglobals.g_time = (double)now / 1000.0;
    if ( dtime > 2.0 )
        dtime = 0.1;
    g_qeglobals.g_oldtime = dtime;
    if ( dtime > 0.2 )
        dtime = 0.2;
    CamWnd_MouseControl( (float)dtime );   // U-GLOBALS: was Cam_MouseControl( m_pCamWnd, ... )

    if ( g_nUpdateBits )
    {
        int bits = g_nUpdateBits;
        g_nUpdateBits = 0;
        Radiant_UpdateWindows( bits );
    }
}

// Editor hotkey command map (CMainFrame::OnKeyDown 0x422370 + LoadCommands_stdmap 0x420140
// over g_Commands @0x73B240, 187 entries).  Radiant binds single-key + modified editor
// shortcuts through this map, NOT the MFC accelerator table: OnKeyDown builds a modifier mask
// (Shift=1, Alt=2, Ctrl=4, Win=8), looks the key up, and SendMessage(WM_COMMAND, commandId).
// std::map keeps the FIRST binding for a duplicate key, so a forward first-match search
// matches the binary.  g_radiantCommands is a MUTABLE copy of g_radiantCommandsDefault (the
// binary's table, verbatim): LoadCommandMap (0x421230) overrides individual entries BY NAME
// from radiant.ini [Commands]; ShowMenuItemKeyBindings (0x420460) annotates the menu items.
struct RadiantCommand { const char *name; byte vk; byte mods; int commandId; };
static const RadiantCommand g_radiantCommandsDefault[] = {
    { "ToggleOutlineDraw", 0x4A, 0, 33103 },
    { "ToggleTintDraw", 0x4A, 1, 33172 },
    { "CSGMerge", 0x55, 4, 32927 },
    { "AutoCaulk", 0x43, 2, 33220 },
    { "ViewFilters", 0x46, 0, 33104 },
    { "HideSelected", 0x48, 0, 32923 },
    { "HideUnSelected", 0x48, 2, 32934 },
    { "ShowHidden", 0x48, 1, 32924 },
    { "ShowLastHidden", 0x48, 5, 33246 },
    { "FitBrush", 0x42, 1, 33098 },
    { "AdvancedCurveEdit", 0x59, 0, 33130 },
    { "SelectionKeyValue", 0x46, 5, 33133 },
    { "ToggleLockPatchVertices", 0xBE, 4, 33140 },
    { "ToggleUnlockPatchVertices", 0xBE, 5, 33139 },
    { "ToggleTurnTerrainEdges", 0xBF, 4, 33141 },
    { "ShowTexturesInUse", 0x55, 0, 32974 },
    { "ViewTextures", 0x54, 0, 33018 },
    { "ThickenPatch", 0x54, 5, 32904 },
    { "AddTerrainRow", 0x41, 5, 33153 },
    { "ExtrudeTerrainRow", 0x4F, 2, 33192 },
    { "RemoveTerrainRow", 0x51, 5, 33154 },
    { "SplitPatch", 0x58, 5, 33158 },
    { "SurfaceInspector", 0x53, 0, 33041 },
    { "PatchInspector", 0x53, 1, 33092 },
    { "ApplyPatchCap", 0x50, 5, 35040 },
    { "TolerantWeld", 0x4A, 5, 33155 },
    { "RedisperseVertices", 0x46, 1, 33170 },
    { "RedisperseRows", 0x45, 1, 32888 },
    { "RedisperseCols", 0x45, 5, 32889 },
    { "InvertCurveTextureX", 0x49, 5, 32903 },
    { "InvertCurveTextureY", 0x49, 1, 32899 },
    { "InvertCurve", 0x49, 4, 32881 },
    { "IncPatchColumn", 0x6B, 5, 32868 },
    { "IncPatchRow", 0x6B, 4, 32867 },
    { "DecPatchColumn", 0x6D, 5, 32870 },
    { "DecPatchRow", 0x6D, 4, 32869 },
    { "Patch TAB", 0x09, 0, 33089 },
    { "Patch TAB", 0x09, 1, 33089 },
    { "TogglePatchWireframes", 0x57, 1, 32857 },
    { "SelectNudgeDown", 0x28, 2, 32850 },
    { "EntityColor", 0x4B, 0, 33036 },
    { "CameraForward", 0x26, 0, 33059 },
    { "CameraBack", 0x28, 0, 33060 },
    { "CameraLeft", 0x25, 0, 33057 },
    { "CameraRight", 0x27, 0, 33058 },
    { "Vertex Select Up", 0x26, 4, 33165 },
    { "Vertex Select Down", 0x28, 4, 33166 },
    { "CameraUp", 0x44, 0, 33055 },
    { "CameraDown", 0x43, 0, 33056 },
    { "CameraAngleUp", 0x41, 0, 33061 },
    { "CameraAngleDown", 0x5A, 0, 33062 },
    { "CameraStrafeRight", 0xBE, 0, 33064 },
    { "CameraStrafeLeft", 0xBC, 0, 33063 },
    { "ToggleGrid", 0x30, 0, 33065 },
    { "SetGridPointFive", 0xC0, 0, 35021 },
    { "SetGrid1", 0x31, 0, 35022 },
    { "SetGrid2", 0x32, 0, 35023 },
    { "SetGrid4", 0x33, 0, 35024 },
    { "SetGrid8", 0x34, 0, 35025 },
    { "SetGrid16", 0x35, 0, 35026 },
    { "SetGrid32", 0x36, 0, 35027 },
    { "SetGrid64", 0x37, 0, 35029 },
    { "SetGrid256", 0x38, 0, 35032 },
    { "SetGrid512", 0x39, 0, 35033 },
    { "DragEdges", 0x45, 0, 33006 },
    { "DragVertices", 0x56, 0, 33005 },
    { "ViewEntityInfo", 0x4E, 0, 33017 },
    { "ViewConsole", 0x4F, 0, 33016 },
    { "CloneSelection", 0x20, 0, 33001 },
    { "DeleteSelection", 0x08, 0, 33003 },
    { "UnSelectSelection", 0x1B, 0, 33002 },
    { "InvertSelection", 0x49, 0, 33101 },
    { "CenterView", 0x23, 0, 32953 },
    { "ZoomOut", 0x2D, 0, 32996 },
    { "ZoomIn", 0x2E, 0, 32995 },
    { "SelectPrev", 0xBC, 1, 33161 },
    { "SelectNext", 0xBE, 1, 33160 },
    { "UpFloor", 0x21, 0, 32954 },
    { "DownFloor", 0x22, 0, 32955 },
    { "LinkSelectionToggle", 0x4F, 1, 1085 },
    { "ToggleClipper", 0x58, 0, 32783 },
    { "ToggleCrosshairs", 0x58, 1, 33100 },
    { "ToggleTexMoveLock", 0x54, 1, 32785 },
    { "ToggleTexRotateLock", 0x52, 1, 32835 },
    { "ToggleLightmapLock", 0x00, 0, 33237 },
    { "RemoveColorNode", 0x52, 4, 10 },
    { "ToggleLayers", 0x4C, 0, 33954 },
    { "Preferences", 0x50, 0, 32784 },
    { "ToggleCamera", 0x43, 5, 33069 },
    { "ToggleView", 0x56, 5, 33071 },
    { "DropVertices", 0x44, 7, 33213 },
    { "RotateZ", 0x44, 1, 32961 },
    { "ToggleZ", 0x5A, 5, 33070 },
    { "SameTargetname", 0x42, 0, 36121 },
    { "SameTarget", 0x42, 4, 36123 },
    { "ConnectSelection", 0x57, 0, 33021 },
    { "SetViewToEntity", 0x75, 0, 33210 },
    { "SplaySelection", 0x57, 2, 33157 },
    { "SelectConnectedEntities", 0x45, 6, 33134 },
    { "SelectTargettedEntities", 0x45, 4, 36110 },
    { "PatchMatrixTranspose", 0x4D, 5, 32906 },
    { "MakeDetail", 0x44, 5, 33042 },
    { "MakeWeaponClip", 0x57, 5, 196 },
    { "MakeNonColliding", 0xBD, 5, 197 },
    { "MakeSplitCoplanarGeo", 0x00, 0, 33223 },
    { "MakeDontSplitCoplanarGeo", 0x00, 0, 33224 },
    { "MapInfo", 0x4D, 0, 32786 },
    { "NextLeakSpot", 0x4B, 5, 33024 },
    { "PrevLeakSpot", 0x4C, 5, 33025 },
    { "FileOpen", 0x4F, 4, 57601 },
    { "FileSave", 0x53, 4, 57603 },
    { "Quit", 0x00, 0, 32951 },
    { "NextView", 0x09, 4, 32789 },
    { "ClipSelected", 0x0D, 0, 32795 },
    { "SplitSelected", 0x0D, 1, 32794 },
    { "FlipClip", 0x0D, 4, 32796 },
    { "MouseRotate", 0x52, 0, 32810 },
    { "Copy", 0x43, 4, 33039 },
    { "Paste", 0x56, 4, 33040 },
    { "Undo", 0x5A, 4, 57643 },
    { "Redo", 0x59, 4, 57644 },
    { "ZZoomOut", 0x2D, 4, 33000 },
    { "ZZoomIn", 0x2E, 4, 32999 },
    { "TexDecrement", 0x6D, 1, 33072 },
    { "TexIncrement", 0x6B, 1, 33073 },
    { "TextureFit", 0x46, 4, 33074 },
    { "TextureFitAll", 0x46, 6, 33234 },
    { "TexRotateClock", 0x25, 4, 33075 },
    { "TexRotateCounter", 0x27, 4, 33076 },
    { "TexShiftLeft", 0x25, 1, 33079 },
    { "TexShiftRight", 0x27, 1, 33080 },
    { "TexShiftUp", 0x26, 1, 33081 },
    { "TexShiftDown", 0x28, 1, 33082 },
    { "TexLayerCycle", 0x4C, 1, 33238 },
    { "TexLayerMaterial", 0x00, 0, 33232 },
    { "TexLayerLightmap", 0x00, 0, 33233 },
    { "GridDown", 0xDB, 0, 33084 },
    { "GridUp", 0xDD, 0, 33083 },
    { "TexScaleLeft", 0x25, 4, 33085 },
    { "TexScaleRight", 0x27, 4, 33086 },
    { "LightShiftUp", 0xDD, 2, 33145 },
    { "LightShiftDown", 0xDB, 2, 33146 },
    { "CyclinderHeightUp", 0xBE, 2, 33176 },
    { "CyclinderHeightDown", 0xBC, 2, 33177 },
    { "AssociateEntities", 0x47, 1, 33150 },
    { "VehicleGroup", 0x56, 1, 33221 },
    { "DynEntities", 0x59, 1, 36106 },
    { "DisassociateEntities", 0x47, 5, 33151 },
    { "SelectedAssociated", 0x58, 4, 33152 },
    { "OverBrightShiftUp", 0xDD, 1, 33147 },
    { "OverBrightShiftDown", 0xDB, 1, 33148 },
    { "CubicClipZoomOut", 0xDD, 4, 32819 },
    { "CubicClipZoomIn", 0xDB, 4, 32820 },
    { "ToggleCubicClip", 0xDC, 4, 32817 },
    { "MoveSelectionDOWN", 0x6D, 0, 32829 },
    { "MoveSelectionUP", 0x6B, 0, 32831 },
    { "LinkSelected", 0x51, 2, 33211 },
    { "SelectNudgeLeft", 0x25, 2, 32847 },
    { "GetDistance", 0x70, 2, 33178 },
    { "AutoEdgeTurn", 0x71, 2, 33179 },
    { "SelectNudgeRight", 0x27, 2, 32848 },
    { "SelectNudgeUp", 0x26, 2, 32849 },
    { "CycleCapTexturePatch", 0x4E, 5, 32905 },
    { "NaturalizePatch", 0x4E, 4, 32890 },
    { "ToggleSnapToGrid", 0x47, 6, 32793 },
    { "SelectSnapPointsToGrid", 0x47, 4, 33091 },
    { "ShowAllTextures", 0x41, 4, 32973 },
    { "SelectAllOfType", 0x41, 1, 33093 },
    { "SelectAllOfTypeRecurse", 0x41, 2, 33212 },
    { "CapCurrentCurve", 0x43, 1, 32885 },
    { "MakeStructural", 0x53, 5, 33043 },
    { "Center2DOnCamera", 0x58, 2, 33108 },
    { "EnterPrefab", 0x22, 2, 33173 },
    { "LeavePrefab", 0x21, 2, 33174 },
    { "LightPreviewToggle", 0x77, 0, 33950 },
    { "LightPreviewStart", 0x77, 1, 33951 },
    { "LightPreviewStop", 0x77, 3, 33952 },
    { "LightPreviewClear", 0x77, 2, 33953 },
    { "LightPreviewSun", 0x77, 4, 36108 },
    { "LightPreviewRegions", 0x77, 5, 36125 },
    { "MaxLightIntensity", 0x77, 6, 36122 },
    { "VertEdit", 0x47, 0, 33199 },
    { "RefreshTextures", 0x74, 0, 33204 },
    { "TogglePreviewModels", 0x00, 0, 35005 },
    { "HideByClassname", 0x48, 7, 32925 },
    { "ToggleLayeredMaterialWnd", 0x73, 0, 35008 },
    { "SaveLayeredMaterials", 0x00, 0, 35009 },
};

// Mutable runtime binding table (the port's analog of the LoadCommands_stdmap std::map).
// Seeded from the defaults; LoadCommandMap patches vk/mods in place from radiant.ini.
static RadiantCommand g_radiantCommands[ ARRAYSIZE( g_radiantCommandsDefault ) ];
static bool           g_radiantCommandsInit = false;
static void Radiant_SeedCommandTable()
{
    if ( g_radiantCommandsInit )
        return;
    memcpy( g_radiantCommands, g_radiantCommandsDefault, sizeof( g_radiantCommands ) );
    g_radiantCommandsInit = true;
}

// Shell-agnostic read access to the LIVE command table (the mutable copy LoadCommandMap
// patches from radiant.ini) â€” the ImGui command-list panel's row source.  Seeding inside
// the accessor keeps callers independent of whether CMainFrame has run yet (Phase 4).
int Radiant_GetCommandTable( const RadiantCommand **out )
{
    Radiant_SeedCommandTable();
    *out = g_radiantCommands;
    return (int)ARRAYSIZE( g_radiantCommands );
}

// The named-key table (binary g_Keys @0x73BDF8, 47 entries, ends at g_KeysExceeded 0x73BF74).
// Maps a key NAME to its VK code â€” used by LoadCommandMap (parse an INI binding string) and by
// ShowMenuItemKeyBindings (VK â†’ name for the menu annotation).  Extracted verbatim from the exe.
struct RadiantKeyName { const char *name; unsigned int vk; };
static const RadiantKeyName g_radiantKeys[] = {
    { "Space", 0x20 }, { "Backspace", 0x08 }, { "Escape", 0x1B }, { "End", 0x23 },
    { "Insert", 0x2D }, { "Delete", 0x2E }, { "PageUp", 0x21 }, { "PageDown", 0x22 },
    { "Up", 0x26 }, { "Down", 0x28 }, { "Left", 0x25 }, { "Right", 0x27 },
    { "F1", 0x70 }, { "F2", 0x71 }, { "F3", 0x72 }, { "F4", 0x73 }, { "F5", 0x74 },
    { "F6", 0x75 }, { "F7", 0x76 }, { "F8", 0x77 }, { "F9", 0x78 }, { "F10", 0x79 },
    { "F11", 0x7A }, { "F12", 0x7B }, { "Tab", 0x09 }, { "Return", 0x0D },
    { "Comma", 0xBC }, { "Period", 0xBE }, { "Plus", 0x6B }, { "Multiply", 0x6A },
    { "Subtract", 0x6D }, { "NumPad0", 0x60 }, { "NumPad1", 0x61 }, { "NumPad2", 0x62 },
    { "NumPad3", 0x63 }, { "NumPad4", 0x64 }, { "NumPad5", 0x65 }, { "NumPad6", 0x66 },
    { "NumPad7", 0x67 }, { "NumPad8", 0x68 }, { "NumPad9", 0x69 }, { "Minus", 0xBD },
    { "[", 0xDB }, { "]", 0xDD }, { "\\", 0xDC }, { "~", 0xC0 }, { "LWin", 0x5B },
};

// â”€â”€ LoadCommandMap (0x421230) â€” override the default key bindings from radiant.ini â”€â”€â”€â”€â”€
//   For each command, read radiant.ini [Commands] <name>=<binding>; if present, parse the
//   modifier flags (+alt/+ctrl/+shift/+lwin) and the key (single alnum char or a g_Keys name)
//   and overwrite that command's binding.  A missing file / missing key leaves the default.
//   INI path: g_PrefsDlg->m_strUserIniPath if set, else <exe folder>\radiant.ini (g_strAppPath).
// U-BOOT: lifted out of CMainFrame verbatim â€” the body never touched the frame (only
// g_PrefsDlg + GetModuleFileNameA + the command table), so it is the same call in both
// shells.  CMainFrame::LoadCommandMap is a forwarder at the bottom of this file.
void Radiant_LoadCommandMap()
{
    Radiant_SeedCommandTable();

    char iniPath[MAX_PATH];
    if ( g_PrefsDlg && !g_PrefsDlg->m_strUserIniPath.empty() )
    {
        _snprintf( iniPath, sizeof( iniPath ), "%s", g_PrefsDlg->m_strUserIniPath.c_str() );
    }
    else
    {
        char exeDir[MAX_PATH];
        GetModuleFileNameA( nullptr, exeDir, sizeof( exeDir ) );
        char *slash = strrchr( exeDir, '\\' );
        if ( slash ) *slash = 0;
        _snprintf( iniPath, sizeof( iniPath ), "%s\\radiant.ini", exeDir );
    }

    for ( RadiantCommand &c : g_radiantCommands )
    {
        char buf[1024];
        if ( !GetPrivateProfileStringA( "Commands", c.name, "", buf, sizeof( buf ), iniPath ) )
            continue;   // no override for this command â†’ keep the default binding

        // Parse + strip the modifier flags (each is optional, any order).  The binary uses
        // CString::Find + Replace; a plain strstr/erase is faithful.
        byte mods = 0;
        struct { const char *tok; byte bit; } flags[] =
            { { "+alt", 2 }, { "+ctrl", 4 }, { "+shift", 1 }, { "+lwin", 8 } };
        for ( auto &fl : flags )
        {
            char *at = strstr( buf, fl.tok );
            if ( at )
            {
                mods |= fl.bit;
                size_t n = strlen( fl.tok );
                memmove( at, at + n, strlen( at + n ) + 1 );   // remove the token in place
            }
        }
        // trim whitespace
        char *s = buf;
        while ( *s == ' ' || *s == '\t' ) ++s;
        char *e = s + strlen( s );
        while ( e > s && ( e[-1] == ' ' || e[-1] == '\t' ) ) --e;
        *e = 0;
        _strupr( s );

        byte vk = 0;
        bool have = false;
        if ( strlen( s ) == 1 && isalnum( (byte)*s ) )
        {
            vk   = (byte)( *s & 0x7F );   // __toascii
            have = true;
        }
        else
        {
            for ( const RadiantKeyName &k : g_radiantKeys )
            {
                // g_Keys names compared case-insensitively (binary uses _mbsicmp)
                if ( !_stricmp( s, k.name ) )
                {
                    vk   = (byte)k.vk;
                    have = true;
                    break;
                }
            }
        }
        if ( have )
        {
            c.vk   = vk;
            c.mods = mods;
        }
    }
}

// â”€â”€ ShowMenuItemKeyBindings (0x420460) â€” annotate menu items with their key binding â”€â”€â”€â”€
//   For each command, if a menu item with that command ID exists (and is a text item), append
//   "\t[Shift-][Alt-][Ctrl-][LWin-]KeyName" to its caption.  KeyName comes from g_Keys, or the
//   raw char (%c) for a plain-key binding.  Faithful to the binary (walks g_Commands/g_Keys).
// U-BOOT: lifted out of CMainFrame verbatim â€” it takes the HMENU it annotates and touches
// nothing else, so both shells call it right after ::SetMenu.  CMainFrame::
// ShowMenuItemKeyBindings is a forwarder at the bottom of this file.
BOOL Radiant_ShowMenuItemKeyBindings( HMENU hMenu )
{
    Radiant_SeedCommandTable();
    BOOL result = FALSE;
    for ( const RadiantCommand &c : g_radiantCommands )
    {
        MENUITEMINFOA mii;
        char caption[1028];
        memset( &mii, 0, sizeof( mii ) );
        mii.cbSize     = sizeof( mii );
        mii.fMask      = MIIM_TYPE;
        mii.dwTypeData = caption;
        mii.cch        = 1024;
        result = GetMenuItemInfoA( hMenu, c.commandId, FALSE, &mii );
        if ( !result || mii.fType )        // no such item / not a string item (separator etc.)
            continue;

        // drop any existing "\t..." accelerator text, then rebuild it.
        char *tab = strchr( caption, '\t' );
        if ( tab ) *tab = 0;
        char *w = caption + strlen( caption );
        *w++ = '\t';
        *w   = 0;

        if ( c.mods )
        {
            if ( c.mods & 1 ) { strcpy( w, "Shift-" ); w += strlen( w ); }
            if ( c.mods & 2 ) { strcpy( w, "Alt-" );   w += strlen( w ); }
            if ( c.mods & 4 ) { strcpy( w, "Ctrl-" );  w += strlen( w ); }
            if ( c.mods & 8 ) { strcpy( w, "LWin-" );  w += strlen( w ); }
        }

        // VK â†’ key name (g_Keys); if not a named key, emit the raw character.
        const char *keyName = nullptr;
        for ( const RadiantKeyName &k : g_radiantKeys )
            if ( k.vk == c.vk ) { keyName = k.name; break; }
        if ( keyName )
            strcpy( w, keyName );
        else
            sprintf( caption + strlen( caption ), "%c", c.vk );

        memset( &mii, 0, sizeof( mii ) );
        mii.cbSize     = sizeof( mii );
        mii.fMask      = MIIM_TYPE;
        mii.fType      = 0;
        mii.dwTypeData = caption;
        mii.cch        = (UINT)strlen( caption );
        result = SetMenuItemInfoA( hMenu, c.commandId, FALSE, &mii );
    }
    return result;
}


// Command-map lookup + dispatch (factored out of OnKeyDown so the floating inspector popup can
// reach it too).  Returns true when a hotkey fired its command.
// U-CMD-1: this IS the whole lookup half of 0x422370, now a free function â€” the raw-Win32
// shell's per-view key handlers call it directly.  The dispatch TAIL is Radiant_ExecCommand,
// which is the single #ifdef'd fork for the whole file: MFC = SendMessage(WM_COMMAND) to the
// frame (byte-identical to the old ::SendMessageA(m_hWnd, ...) â€” d_hwndMain IS m_hWnd),
// KISAK_NO_MFC = Radiant_DispatchCommandDirect.
bool Radiant_TryHotkey( unsigned int vk )
{
    if ( !vk )
        return false;
    Radiant_SeedCommandTable();   // every other table reader seeds; without this an
                                  // early key (pre-LoadCommandMap) walks all-zero BSS
    byte mods = 0;
    if ( GetKeyState( VK_MENU )    < 0 ) mods |= 2;   // Alt
    if ( GetKeyState( VK_CONTROL ) < 0 ) mods |= 4;   // Ctrl
    if ( GetKeyState( VK_SHIFT )   < 0 ) mods |= 1;   // Shift
    if ( GetKeyState( VK_LWIN )    < 0 ) mods |= 8;   // Win
    for ( const RadiantCommand &c : g_radiantCommands )
    {
        if ( c.vk == vk && c.mods == mods )
        {
            Radiant_ExecCommand( (unsigned int)c.commandId );
            return true;
        }
    }
    return false;
}


// â”€â”€ File / Edit command wrappers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// U-CMD-2: the five File-menu map flows + the WM_CLOSE guard are FREE functions now.  Every
// one of these bodies already reached the shell through exactly two things â€” s_currentMapPath
// and the frame's own HWND â€” and the HWND is g_qeglobals.d_hwndMain in BOTH shells
// (mainfrm.cpp:1460 GetSafeHwnd() / radiant_main.cpp:312), so one body serves the MFC message
// map (via the forwarders below) and Radiant_DispatchCommandDirect.  The two prompts they
// guard on (Radiant_OkToDiscard / Radiant_ConfirmModified) and Radiant_FileSaveAs's picker are
// defined further down this file; radiant_frame.h declares all three for both shells.
void Radiant_FileNew()
{
    // Faithful OnFileNew (0x423AA0): guard on OkToDiscard() (the unsaved-changes prompt,
    // now shipped â€” was a parked stub), pop any prefab nesting, then run the real
    // Fileâ†’New = Map_New().  Map_New builds a valid worldspawn so a drag-created brush
    // can attach; the old Map_NewMap()-only path left world_entity NULL and the first
    // NewBrushDrag NULL-deref'd.
    if ( !Radiant_OkToDiscard() )
        return;                            // user chose Cancel â†’ keep the current map
    Prefab_LevelBack();
    Map_New();
    s_currentMapPath[0] = 0;
    ::SetWindowTextA( g_qeglobals.d_hwndMain, "CoD4Radiant - untitled" );
    QE_CountBrushesAndUpdateStatusBar();
    g_nUpdateBits = W_ALL;
}

void Radiant_FileOpen()
{
    // Faithful OnFileOpen (0x423ae0): guard on OkToDiscard() (then Prefab_LevelBack)
    // BEFORE popping the open dialog â€” so a dirty map prompts to save first.
    if ( !Radiant_OkToDiscard() )
        return;
    Prefab_LevelBack();
    char file[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    memset( &ofn, 0, sizeof( ofn ) );
    ofn.lStructSize = sizeof( ofn );
    ofn.hwndOwner   = g_qeglobals.d_hwndMain;
    ofn.lpstrFilter = "Map files (*.map)\0*.map\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = sizeof( file );
    ofn.lpstrTitle  = "Open Map";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if ( GetOpenFileNameA( &ofn ) )
        Radiant_OpenMap( file );
}

void Radiant_FileSave()
{
    if ( !s_currentMapPath[0] )
    {
        Radiant_FileSaveAs();
        return;
    }
    Map_SaveFile( s_currentMapPath, 0, 0 );
    Radiant_FL_Log( "saved: %s", s_currentMapPath );
}

void Radiant_FileExit()
{
    // Fileâ†’Exit posts WM_CLOSE â†’ the close guard runs there (faithful: the binary's exit
    // guard lives in OnClose 0x422220, not in the Exit command).  Both shells honour it:
    // the MFC frame in CMainFrame::OnClose, the raw frame in its WM_CLOSE case, and both
    // run the SAME Radiant_FrameCloseAllowed() body below.
    ::PostMessageA( g_qeglobals.d_hwndMain, WM_CLOSE, 0, 0 );
}

// The guard half of CMainFrame::OnClose (0x422220): true = let the frame close.  If the user
// cancels the prompt the close is swallowed (the caller must NOT chain to its default).
bool Radiant_FrameCloseAllowed()
{
    if ( !Radiant_OkToDiscard() )
        return false;
    Prefab_LevelBack();
    return true;
}


// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
//  PROJECT (.prj) menu handlers + the MRU (recent-files) menu.
//  Cores (Project_Write / QE_LoadProject_ParseFile / the MRU_* family) live in qe3.cpp.
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
extern signed int Project_Write( const char *path );                        // qe3.cpp 0x48BD90
extern const char *Project_GetCurrentPath();                                 // qe3.cpp (== dword_25D65AC)
extern LPMRUMENU  *CreateMruMenuDefault();                                    // qe3.cpp 0x48A150
extern void        MRU_NewItem( LPMRUMENU *mru, const char *lpString1 );      // qe3.cpp 0x48A2C0
extern void        MRU_InsertItem( LPMRUMENU *mru, HMENU hMenu );             // qe3.cpp 0x48A400
extern void        SaveMruInReg( LPMRUMENU *mru );                            // qe3.cpp 0x48A750
extern void        LoadMruInReg( LPMRUMENU *mru );                            // qe3.cpp 0x48A870
extern BOOL        DoMru( short nID, HWND hWnd );                             // qe3.cpp 0x4994B0
extern char       *ValueForKey2( int e, const char *key );                   // entity.cpp 0x4825C0
extern void        SetKeyValue( entity_s_def *e, const char *key, const char *value ); // entity.cpp

// â”€â”€ 0x495330  ProjectDlgProc â€” the IDD_PROJECT_SETTINGS dialog proc â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Verbatim from the binary: WM_INITDIALOG (272) fills the 5 edit controls from the
// project entity; WM_COMMAND (273) OK reads them back (SetKeyValue) + Project_Write,
// Cancel just closes.  Control ids: 1265 basepath / 1274 mapspath / 1253 entitypath /
// 1260 game / 1273 basegame.
static INT_PTR CALLBACK ProjectDlgProc( HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam )
{
    entity_s_def *proj = (entity_s_def *)g_qeglobals.d_project_entity;
    if ( msg == WM_INITDIALOG )
    {
        if ( !proj )
            return TRUE;
        SetDlgItemTextA( hDlg, 1265, ValueForKey2( (int)(intptr_t)proj, "basepath" ) );
        SetDlgItemTextA( hDlg, 1274, ValueForKey2( (int)(intptr_t)proj, "mapspath" ) );
        SetDlgItemTextA( hDlg, 1253, ValueForKey2( (int)(intptr_t)proj, "entitypath" ) );
        SetDlgItemTextA( hDlg, 1260, ValueForKey2( (int)(intptr_t)proj, "game" ) );
        SetDlgItemTextA( hDlg, 1273, ValueForKey2( (int)(intptr_t)proj, "basegame" ) );
        return TRUE;
    }
    if ( msg != WM_COMMAND )
        return FALSE;
    if ( LOWORD( wParam ) == IDOK )
    {
        char String[1024];
        GetDlgItemTextA( hDlg, 1265, String, 1024 ); SetKeyValue( proj, "basepath",   String );
        GetDlgItemTextA( hDlg, 1274, String, 1024 ); SetKeyValue( proj, "mapspath",   String );
        GetDlgItemTextA( hDlg, 1253, String, 1024 ); SetKeyValue( proj, "entitypath", String );
        GetDlgItemTextA( hDlg, 1260, String, 1024 ); SetKeyValue( proj, "game",       String );
        GetDlgItemTextA( hDlg, 1273, String, 1024 ); SetKeyValue( proj, "basegame",   String );
        EndDialog( hDlg, 1 );
        Project_Write( Project_GetCurrentPath() );
        return TRUE;
    }
    if ( LOWORD( wParam ) == IDCANCEL )
    {
        EndDialog( hDlg, 0 );
        return TRUE;
    }
    return FALSE;
}

// â”€â”€ 0x428DE0  CMainFrame::OnFileProjectsettings â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Pops the Project Settings dialog (edit the loaded project entity's epairs).  Faithful
// (binary DialogBoxParamA(IDD_DLG_PROJECT, ProjectDlgProc)).  No-op if no project loaded
// (the dialog would show blank fields and Project_Write would fault on a NULL entity).
static void Cmd_OnFileProjectsettings()
{
    if ( !g_qeglobals.d_project_entity )
    {
        Radiant_FL_Log( "OnFileProjectsettings: no project loaded" );
        return;
    }
    DialogBoxParamA( g_qeglobals.d_hInstance, MAKEINTRESOURCE( IDD_PROJECT_SETTINGS ),
                     g_qeglobals.d_hwndMain, ProjectDlgProc, 0 );
}


static void Cmd_OnEditUndo()
{
    Undo_Undo();
    QE_CountBrushesAndUpdateStatusBar();
    g_nUpdateBits = W_ALL;
}

static void Cmd_OnEditRedo()
{
    Undo_Redo();
    QE_CountBrushesAndUpdateStatusBar();
    g_nUpdateBits = W_ALL;
}

// â”€â”€ Grid menu (IDB OnGrid1 0x424ab0 + helpers) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// uIDCheckItemGrid (IDB 0x6ddf04): the 11 grid-size menu command IDs, parallel to
// grid_sizes (IDB 0x6dde5c = {0.5,1,2,4,8,16,32,64,128,256,512}). Clicking "Grid N"
// sets g_qeglobals.d_gridsize to the matching index.
extern float grid_sizes[];   // engine_stubs.cpp (0x6dde5c)
static const UINT s_gridMenuIDs[11] =
    { 35021, 35022, 35023, 35024, 35025, 35026, 35027, 35029, 35031, 35032, 35033 };

// CheckGridMenuSelection (IDB 0x428950): radio-check the active grid-size menu item.
void Radiant_CheckGridMenu()   // U-BOOT: de-static'd (both boots seed the radio check)
{
    HMENU m = Radiant_FrameMenu();
    if ( !m )
        return;
    for ( int i = 0; i < 11; ++i )
        ::CheckMenuItem( m, s_gridMenuIDs[i],
            ( i == g_qeglobals.d_gridsize ) ? MF_CHECKED : MF_UNCHECKED );
}

extern "C" int ClampGridSize();   // drag.cpp (0x463a80 â€” rotate/grid snap table)

// SetGridStatus (IDB 0x428a00) â€” the full "G:%.1f T:%i R:%i C:%i L:%c%c" status line.
// Now that g_PrefsDlg is the real settings object the texture/rotate-lock + cubic-scale
// fields are read directly (no more grid-size-only fallback): G=grid size,
// T=saved grid index, R=ClampGridSize, C=CubicScale, L=texLock('M')+rotLock('R').
void Radiant_SetGridStatus()
{
    const char texLockC = g_PrefsDlg->m_bTextureLock ? 'M' : ' ';
    const char rotLockC = g_PrefsDlg->m_bRotateLock  ? 'R' : ' ';
    char buf[64];
    // NB: d_savedinfo.d_gridsize is a float in the port's qe3.h; the binary passes it
    // to %i, so cast to int explicitly (a float in a %i vararg would misalign the rest).
    _snprintf( buf, sizeof( buf ), "G:%.1f T:%i R:%i C:%i L:%c%c",
               grid_sizes[g_qeglobals.d_gridsize],
               (int)g_qeglobals.d_savedinfo.d_gridsize,
               ClampGridSize(),
               g_PrefsDlg->m_nCubicScale,
               texLockC, rotLockC );
    MainFrm_SetStatusText( 4, buf );
}

// â”€â”€ Prefs command handlers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// A small CheckMenuItem helper (the binary inlines GetMenuâ†’FromHandleâ†’CheckMenuItem
// in each toggle); MF_CHECKED=8, MF_UNCHECKED=0.
void Radiant_CheckMenu( UINT id, bool checked )   // U-BOOT: de-static'd
{
    HMENU m = Radiant_FrameMenu();
    if ( m )
        ::CheckMenuItem( m, id, checked ? MF_CHECKED : MF_UNCHECKED );
}

extern void PMESH_49();              // pmesh.cpp (0x4495C0) â€” rebuild all active curveDefs


// OnSnaptogrid (IDB 0x428380) â€” toggle m_bNoClamp, persist, re-check the menu. This is
// the interactive persistence demo: toggling snap flips the grid-snap behaviour
// (drag.cpp / xywnd.cpp / Brush_SnapPlanepts all read m_bNoClamp) and SavePrefs writes
// it to the registry so it survives a restart.
static void Cmd_OnSnaptogrid()
{
    g_PrefsDlg->m_bNoClamp ^= 1;
    Prefs_SavePrefs( g_PrefsDlg );
    Radiant_CheckMenu( 32793, g_PrefsDlg->m_bNoClamp == 0 );     // snap = !NoClamp
    Radiant_SetGridStatus();
}

// Texture-Lock toggles (IDB OnToggleLockMoves 0x426b80 / OnToggleLockRotations
// 0x429230 / OnToggleLockLightmap 0x426bf0): flip the lock field, re-check the menu,
// SavePrefs, refresh the grid-status line (which shows the lock chars). The lock
// reprojection itself is still deferred in Brush_Move (see brush.cpp), so these
// currently only persist + display the flags.
static void Cmd_OnToggleLockMoves()
{
    g_PrefsDlg->m_bTextureLock = ( g_PrefsDlg->m_bTextureLock == 0 );
    Radiant_CheckMenu( 32785, g_PrefsDlg->m_bTextureLock != 0 );
    Prefs_SavePrefs( g_PrefsDlg );
    Radiant_SetGridStatus();
}
static void Cmd_OnToggleLockRotations()
{
    g_PrefsDlg->m_bRotateLock = ( g_PrefsDlg->m_bRotateLock == 0 );
    Radiant_CheckMenu( 32835, g_PrefsDlg->m_bRotateLock != 0 );
    Prefs_SavePrefs( g_PrefsDlg );
    Radiant_SetGridStatus();
}
static void Cmd_OnToggleLockLightmap()
{
    g_PrefsDlg->m_bLightmapLock = ( g_PrefsDlg->m_bLightmapLock == 0 );
    Radiant_CheckMenu( 33237, g_PrefsDlg->m_bLightmapLock != 0 );
    Prefs_SavePrefs( g_PrefsDlg );
    Radiant_SetGridStatus();
}

// â”€â”€ Viewâ†’Show overlay toggles (d_xyShowFlags; IDB 0x42ba40 / 0x42bac0 / 0x42bb00 /
// 0x42bbc0 / 0x42bb60 / 0x42bc20) â”€â”€
// Each handler flips its bit in d_savedinfo.d_xyShowFlags, re-checks the menu item
// (CHECKED == feature SHOWN == bit CLEAR â€” the binary's CheckMenuItem value is 8 when the
// bit is being cleared; Reverse Filter is the one INVERTED case), pushes the state into the
// filter window's checkboxes (CFilterWnd::GetSettings) and broadcasts a repaint.
extern void Map_BuildBrushData();    // map.cpp (0x485f00) â€” rebuilds the display lists

// CFilterWnd::GetSettings (IDB 0x4140f0) â€” push d_xyShowFlags into the filter pane's six
// BM_SETCHECK checkboxes.  The port's filter pane is the entity window's Filter mode, so
// this is Radiant_RefreshFilterPane (win_ent.cpp); it no-ops when that window is down.
static void Radiant_SyncFilterWnd()
{
    Radiant_RefreshFilterPane();
}

static void Cmd_OnSelectNames()              // IDB 0x42ba40
{
    bool nowShown = ( ( g_qeglobals.d_savedinfo.d_xyShowFlags ^ 8 ) & 8 ) == 0;
    g_qeglobals.d_savedinfo.d_xyShowFlags ^= 8u;       // 0x8 = names HIDDEN when set
    Radiant_CheckMenu( 33971 /*ID_Names*/, nowShown );
    Map_BuildBrushData();                              // binary rebuilds brush data here
    Radiant_SyncFilterWnd();
    g_nUpdateBits |= 2u;                               // W_XY
}

static void Cmd_OnSelectCoordinates()        // IDB 0x42bb60
{
    bool nowShown = ( ( g_qeglobals.d_savedinfo.d_xyShowFlags ^ 0x20 ) & 0x20 ) == 0;
    g_qeglobals.d_savedinfo.d_xyShowFlags ^= 0x20u;    // 0x20 = coordinate rulers HIDDEN when set
    Radiant_CheckMenu( 33975 /*ID_Coordinates*/, nowShown );
    Radiant_SyncFilterWnd();
    g_nUpdateBits |= 2u;                               // W_XY
}

static void Cmd_OnSelectReverseFilter()      // IDB 0x42bc20
{
    // NB the menu-check polarity is INVERTED vs the other toggles in the binary
    // (v5=0 when the bit is being cleared, v5=8 when set) â€” i.e. CHECKED == reverse ON
    // == bit SET.  Transcribed verbatim.
    bool nowOn = ( ( g_qeglobals.d_savedinfo.d_xyShowFlags ^ 0x40 ) & 0x40 ) != 0;
    g_qeglobals.d_savedinfo.d_xyShowFlags ^= 0x40u;    // 0x40 = reverse-filter mode
    Radiant_CheckMenu( 36127 /*ID_ReverseFilter*/, nowOn );
    Radiant_SyncFilterWnd();
    ++g_qeglobals.g_filtersUpdated;                    // invalidate FilterBrush's per-brush cache
    if ( g_qeglobals.d_hwndCamera )
        ::SetFocus( g_qeglobals.d_hwndCamera );
    g_nUpdateBits = -1;                                // full repaint (filter visibility changed)
}

static void Cmd_OnSelectConnections()        // IDB 0x42bbc0
{
    // d_xyShowFlags bit 0x4 = connection lines HIDDEN when SET (shown when clear).  Draw
    // consumers = Lines_AddLinkTo + Lines_AddLinkToScript (xywnd.cpp), both ported (the
    // connections unit) and invoked from OnPaint/Cam_Draw via Ed_DrawConnectionLines.
    bool nowShown = ( ( g_qeglobals.d_savedinfo.d_xyShowFlags ^ 4 ) & 4 ) == 0;
    g_qeglobals.d_savedinfo.d_xyShowFlags ^= 4u;
    Radiant_CheckMenu( 33974 /*ID_Connections*/, nowShown );
    Radiant_SyncFilterWnd();                           // mirror into the filter window's checkbox
    g_nUpdateBits |= 0xBu;                             // W_CAMERA|W_XY|W_Z (binary: |= 0xB)
}

static void Cmd_OnSelectAngles()             // IDB 0x42bac0
{
    // d_xyShowFlags bit 0x2 = entity angle arrows HIDDEN when SET (shown when clear).  Draw
    // consumer = DrawAngles (brush.cpp), invoked from DrawBrush for fixedsize "angles"-keyed
    // entities in BOTH the XY views and the camera â€” hence g_nUpdateBits |= 3 (W_XY|W_CAMERA).
    bool nowShown = ( ( g_qeglobals.d_savedinfo.d_xyShowFlags ^ 2 ) & 2 ) == 0;
    g_qeglobals.d_savedinfo.d_xyShowFlags ^= 2u;
    Radiant_CheckMenu( 33972 /*ID_Angles*/, nowShown );
    Radiant_SyncFilterWnd();
    g_nUpdateBits |= 3u;                               // W_XY|W_CAMERA (binary: |= 3)
}

static void Cmd_OnSelectBlocks()             // IDB 0x42bb00
{
    // d_xyShowFlags bit 0x10 = the 1024-unit block grid HIDDEN when SET (shown when clear).
    // Draw consumer = XY_DrawBlockGrid (xywnd.cpp), invoked from OnPaint when the bit is clear.
    bool nowShown = ( ( g_qeglobals.d_savedinfo.d_xyShowFlags ^ 0x10 ) & 0x10 ) == 0;
    g_qeglobals.d_savedinfo.d_xyShowFlags ^= 0x10u;
    Radiant_CheckMenu( 33973 /*ID_Blocks*/, nowShown );
    Radiant_SyncFilterWnd();
    g_nUpdateBits |= 2u;                               // W_XY (block grid is XY-only)
}

// OnTexturesInspector (IDB 0x424b60) â€” Texturesâ†’Surface Inspector â†’ DoSurface.  Opens (or
// re-focuses) the hand-built Surface Inspector popup; selecting a face then populates it,
// and edits re-project the texture live in the camera (surfacedlg.cpp).
static void Cmd_OnTexturesInspector()
{
    // Surf_OpenInspector() opened the MFC surface dialog and is a fenced no-op now —
    // "S" was dispatching into silence. The ImGui surface inspector is the dialog.
    extern void ImGuiPanel_Surface_Toggle();   // imgui_panel_surface.cpp
    ImGuiPanel_Surface_Toggle();
}

// OnPatchInspector (IDB 0x42b460, cmd 33092, Shift+S) â€” Patchâ†’Inspector â†’ DoPatchInspector.
// Opens (or re-focuses) the hand-built Patch Properties popup and populates it from the
// selected patch (patchdialog.cpp).
extern void DoPatchInspector();   // patchdialog.cpp (0x436d30)
static void Cmd_OnPatchInspector()
{
    DoPatchInspector();
}

// OnCurveSimplepatchmesh (IDB 0x429a20, cmd 32856 / 0x8058 â€” verified from the CMainFrame
// command table {cmdId,cmdId,0x38,pfn}) â€” Curveâ†’Simple Patch Mesh.  Runs the "Patch
// density" modal (CPatchDensityDlg) inside an Undo bracket; OK builds a flat NxM Bezier
// patch over the selected brush (patchdialog.cpp â†’ Patch_GenericMesh).
extern void DoSimplePatchMesh();   // patchdialog.cpp (0x429a20 body)
static void Cmd_OnCurveSimplepatchmesh()
{
    DoSimplePatchMesh();
}

extern void DoSimpleTerrainPatchMesh( bool terrain );   // patchdialog.cpp
static void Cmd_OnCurveSimpleterrainpatch()
{
    DoSimpleTerrainPatchMesh( true );
}


// OnSelectionAddToActiveLayer (IDB CXYWnd::OnSelectionAddToActiveLayer 0x466930,
// AFX msgmap nID 0x88B9 = 35001) â€” the right-click "Add selection to active layer"
// command.  The binary attaches it to the CXYWnd context-menu map; KIWI routes
// menu commands through CMainFrame, so this thin handler forwards to the ported
// core (brush.cpp), which sets every selected brush's parent_layer_string to
// g_activeLayer_string.  g_nUpdateBits|=1 â†’ redraw (CMainFrame::On* convention).
extern void CXYWnd_OnSelectionAddToActiveLayer();   // brush.cpp (0x466930 core)
static void Cmd_OnSelectionAddToActiveLayer()
{
    CXYWnd_OnSelectionAddToActiveLayer();
    g_nUpdateBits |= 1;
}


// OnEditEntityinfo (IDB 0x426D6F) â€” Editâ†’Entity Info... The MFC entity-list browser
// died with U-RIP (EntList_Open deleted with entitylist.cpp's popup); its cores
// (EntityList_Gather/EntityEpairs_Gather/EntListSelect_Apply) survive. Until the
// entity-list DOCK TAB lands (POST-RIP: plan "Panels NOT queued" list), route to the
// ImGui entity INSPECTOR panel â€” its eclass/K-V views cover the browse use case.
static void Cmd_OnEditEntityinfo()
{
    extern void ImGuiPanel_Entity_Toggle();   // imgui_panel_entity.cpp
    ImGuiPanel_Entity_Toggle();
}

// OnGrid1 (IDB 0x424ab0): set the grid size from the clicked menu item, refresh the
// menu radio check + status, redraw the XY/Z views. ON_COMMAND_EX passes the command ID.
static BOOL Cmd_OnGridSize( UINT nID )
{
    for ( int i = 0; i < 11; ++i )
        if ( nID == s_gridMenuIDs[i] ) { g_qeglobals.d_gridsize = i; break; }
    Radiant_CheckGridMenu();
    Radiant_SetGridStatus();
    extern void Sys_UpdateWindows( int bits );   // win_qe3.cpp
    Sys_UpdateWindows( W_XY | W_Z );
    return TRUE;
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// View + Selection menu wrappers - thin command handlers forwarding to ported cores.

extern selbrush_t selected_brushes;                      // engine_stubs (0x23F1864)
extern void    Undo_ClearRedo();                         // undo.cpp
extern void    Undo_GeneralStart( const char *operation );
extern void    Undo_AddBrushList( selbrush_t *sb );
extern void    Undo_EndBrushList( selbrush_t *sb );
extern void    Undo_End();
extern void    Select_Deselect( int bDeselectFaces );    // select.cpp (0x48E800)
extern void    CSG_MakeHollow();                          // csg.cpp    (0x47D3C0)
extern int     CSG_Merge();                               // csg.cpp    (0x47DA40)
extern void    Sys_UpdateWindows( int bits );             // win_qe3.cpp
extern int     Sys_Printf( const char *fmt, ... );        // win_qe3.cpp (0x499E90)
extern float   z_scale;                                   // z.cpp       (0x241A5B0)

// IDB global 0x25D5A90 â€” last committed XY zoom level (pixels/world unit). The zoom
// handlers mirror m_fScale into it; the overlay/print paths read it.
float g_zoomLevel = 1.0f;

// CXYWnd::OnViewZoomin (0x424750): zoom the active XY view in 1.25Ã—, clamp to 160,
// and zoom the Z view to match. (Viewâ†’Zoomâ†’XY Zoom In, ID 32995 / Delete.)
static void Cmd_OnViewZoomin()
{
    // U-GLOBALS: the view state comes from Ed_ActiveXY() (never NULL â€” no m_pXYWnd guard).
    if ( Ed_ActiveXY()->m_bActive )
    {
        Ed_ActiveXY()->m_fScale *= 1.25f;
        if ( Ed_ActiveXY()->m_fScale > 160.0f )
            Ed_ActiveXY()->m_fScale = 160.0f;
        g_zoomLevel = Ed_ActiveXY()->m_fScale;
    }
    Sys_UpdateWindows( W_XY | W_XY_OVERLAY | W_Z | W_Z_OVERLAY );
    z_scale *= 1.25f;
    if ( z_scale > 160.0f )
        z_scale = 160.0f;
}

// CXYWnd::OnViewZoomout (0x4247e0): zoom the active XY view out 0.8Ã—, clamp to
// 0.003125, Z view follows. (Viewâ†’Zoomâ†’XY Zoom Out, ID 32996 / Insert.)
static void Cmd_OnViewZoomout()
{
    // U-GLOBALS: the view state comes from Ed_ActiveXY() (never NULL â€” no m_pXYWnd guard).
    if ( Ed_ActiveXY()->m_bActive )
    {
        float s = Ed_ActiveXY()->m_fScale * 0.800000011920929f;
        if ( s < 0.003125000046566129f )
            s = 0.003125f;
        Ed_ActiveXY()->m_fScale = s;
        g_zoomLevel = s;
    }
    z_scale *= 0.800000011920929f;
    if ( z_scale < 0.003125000046566129f )
        z_scale = 0.003125f;
    Sys_UpdateWindows( W_XY | W_XY_OVERLAY | W_Z | W_Z_OVERLAY );
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// 0x42b850  CMainFrame::OnScroll â€” the CENTRAL mouse-wheel dispatcher every editor
// child wndproc funnels WM_MOUSEWHEEL into (`point` is in SCREEN coords).  Order:
//   1. Texture pane hover  -> CTexWnd::Scroll (half-page step).
//   2. Camera pane hover, and only with the CameraUseWheel pref  -> CCamWnd::Scroll
//      (dolly; wheel-forward passes -1.0, wheel-back +1.0).
//   3. Anything else       -> XY zoom in/out.
// The hover test is two-stage, exactly as the binary: with QE4StyleWindows (m_nView) == 1
// the pane must additionally be the window UNDER the cursor (and, for the texture pane,
// the inspector must currently be in Textures mode); otherwise only the screen-rect test
// runs.  Returns TRUE on every path (the wheel is always consumed).
// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// U-CMD-2: extracted whole.  The three MFC binds all had exact HWND twins, so this is ONE
// body both shells run (CMainFrame::OnScroll below is a forwarder):
//   m_pTexWnd / m_pCamWnd            -> g_qeglobals.d_hwndTexture / d_hwndCamera (the SAME
//        windows: mainfrm.cpp:974 / :963 register them from those very pointers)
//   CWnd::FromHandle(..) == m_pXWnd  -> the HWND compare that test always reduced to
//   GetClientRect + ClientToScreen   -> ::GetClientRect + ::MapWindowPoints( h, 0, .., 2 )
//        (CWnd::ClientToScreen(RECT*) maps both corners â€” that is this call)
//   m_pTexWnd->Scroll( zDelta )      -> TexWnd_Scroll (texwnd.cpp:2217 is its forwarder)
// `focusOrHover` is the window the WM_MOUSEWHEEL was delivered to.  The binary's hover test
// consults ::WindowFromPoint ONLY (the wheel is dispatched by CURSOR position, not focus), so
// it is deliberately not read here; it stays in the signature because the raw pump's call
// site has it and a focus-follows rule would land here, not at the call site.
extern void TexWnd_Scroll( short zDelta );                           // texwnd.cpp (CTexWnd::Scroll 0x45DD80)

bool Radiant_OnMouseWheel( HWND focusOrHover, short zDelta, POINT screenPt )
{
    (void)focusOrHover;

    if ( !zDelta )                                                   // 0x42b863
        return true;

    const HWND hTex = g_qeglobals.d_hwndTexture;
    const HWND hCam = g_qeglobals.d_hwndCamera;

    bool overTex = false;                                            // 0x42b879 (v13)
    bool overCam = false;                                            // 0x42b87e (v14)

    if ( hTex )                                                      // 0x42b883
    {
        // 0x42b8b2 â€” style gate + "the texture pane is the window under the cursor".
        if ( g_PrefsDlg->m_nView != 1
          || ( inspector_mode == INSPECTOR_TEXTURE
            && ::WindowFromPoint( screenPt ) == hTex ) )
        {
            RECT r;
            ::GetClientRect( hTex, &r );                             // 0x42b8bd
            ::MapWindowPoints( hTex, nullptr, (POINT *)&r, 2 );      // 0x42b8ce
            if ( ::PtInRect( &r, screenPt ) )                        // 0x42b8da
                overTex = true;                                      // 0x42b8e4
        }
    }
    if ( hCam && g_PrefsDlg->camera_use_wheel )                      // 0x42b8f1/0x42b8f8
    {
        if ( g_PrefsDlg->m_nView != 1
          || ::WindowFromPoint( screenPt ) == hCam )                 // 0x42b920
        {
            RECT r;
            ::GetClientRect( hCam, &r );                             // 0x42b92b
            ::MapWindowPoints( hCam, nullptr, (POINT *)&r, 2 );      // 0x42b93c
            if ( ::PtInRect( &r, screenPt ) )                        // 0x42b948
                overCam = true;                                      // 0x42b952
        }
    }

    if ( overTex )                                                   // 0x42b95c
    {
        TexWnd_Scroll( zDelta );                                     // 0x42b962
        return true;
    }
    if ( !overCam )                                                  // 0x42b97d
    {
        if ( zDelta > 0 )                                            // 0x42b9e9
            Cmd_OnViewZoomin();                                      // 0x42b9eb
        else
            Cmd_OnViewZoomout();                                     // 0x42b9fe
        return true;
    }
    iassert( g_qeglobals.d_hwndCamera );                             // MainFrm.cpp:6124 (was iassert(m_pCamWnd) â€” same window)
    CamWnd_Scroll( ( zDelta > 0 ) ? -1.0f : 1.0f );                  // 0x42b9b7 / 0x42b9cf
    return true;
}


// CMainFrame::OnView100 (0x423c30): reset the XY view to 1:1. (Viewâ†’Zoomâ†’XY 100%,
// ID 32968.)
static void Cmd_OnView100()
{
    Ed_ActiveXY()->m_fScale = 1.0f;   // U-GLOBALS: was m_pXYWnd->m_fScale (never NULL now)
    Sys_UpdateWindows( W_XY | W_XY_OVERLAY );
}

// CMainFrame::OnSelectionDeselect (0x425740): deselect every selected brush + face.
// (Selection->Deselect, ID 33002 / Esc.)  The surf-inspector / clip / rotate / scale /
// curve-point branches only fire when that mode is engaged; the binary otherwise falls
// through to Select_Deselect.
static void Cmd_OnSelectionDeselect()
{
    Select_Deselect( 1 );
    MainFrm_SetStatusText( 2, " " );
}

// â”€â”€ VERTEX-EDIT toggle (Selectionâ†’Drag Vertices, ID 33005) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// CMainFrame::OnSelectionDragVertices (0x425840): sel_vertex/sel_curvepoint â†’ back to
// sel_brush; otherwise route pure-patch selections into patch/terrain edit, else build the
// vertex handle list (SetupVertexSelection) and enter sel_vertex if there are any handles.
// The tail cleans up whichever patch sub-mode was active BEFORE the switch.
extern int  OnlyPatchesSelected();   // engine_stubs.cpp 0x447860
extern int  AnyPatchesSelected();    // engine_stubs.cpp 0x447890
extern void SetupVertexSelection();  // select.cpp 0x494bc0
extern void Patch_EditPatch();       // engine_stubs.cpp (patch editor, FATAL until pmesh)
extern void Terrain_Edit();          // pmesh.cpp (mixed patch+brush vert-snap entry, 0x442100)
extern void sub_43ECB0();            // engine_stubs.cpp (addpoint-mode cleanup, FATAL)
extern void CMainFrame_UpdatePatchToolbarButtons();   // select.cpp (0x42AA70)

static void Cmd_OnSelectionDragVertices()
{
    select_t prevMode = g_qeglobals.d_select_mode;
    if ( prevMode == sel_vertex || prevMode == sel_curvepoint )
    {
        g_qeglobals.d_select_mode = sel_brush;   // toggle OFF
    }
    else
    {
        if ( OnlyPatchesSelected() )             // pure-patch â†’ patch vertex edit
        {
            Patch_EditPatch();
            g_nUpdateBits = -1;
            return;
        }
        if ( AnyPatchesSelected() )              // mixed-with-patch â†’ terrain edit
        {
            Terrain_Edit();
            g_nUpdateBits = -1;
            return;
        }
        SetupVertexSelection();                  // build the point/edge handle lists
        if ( !g_qeglobals.d_numpoints )          // nothing to drag â†’ bail
        {
            g_nUpdateBits = -1;
            return;
        }
        // The binary RE-READS d_select_mode here (0x425898) â€” SetupVertexSelection runs first.
        prevMode = g_qeglobals.d_select_mode;
        g_qeglobals.d_select_mode = sel_vertex;  // toggle ON
    }

    // Clean up whichever patch sub-mode was active before the switch.
    if ( prevMode == sel_cycle_edge_direction_quad )
    {
        CMainFrame_UpdatePatchToolbarButtons();
        g_nUpdateBits = -1;
        return;
    }
    if ( prevMode == sel_addpoint )
    {
        sub_43ECB0();
    }
    g_nUpdateBits = -1;
}

// â”€â”€ EDGE-EDIT toggle (Selectionâ†’Drag Edges, ID 33006) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// CMainFrame::OnSelectionDragedges (0x4257d0): the sibling of the vertex toggle,
// but simpler â€” it does NOT route pure-patch selections (no OnlyPatchesSelected /
// AnyPatchesSelected branch). sel_edge â†’ back to sel_brush; otherwise build the
// handle lists (SetupVertexSelection) and enter sel_edge if there are any points.
static void Cmd_OnSelectionDragEdges()
{
    if ( g_qeglobals.d_select_mode == sel_edge )
    {
        g_qeglobals.d_select_mode = sel_brush;   // toggle OFF
        g_nUpdateBits = -1;
        return;
    }
    SetupVertexSelection();                       // build the point/edge handle lists
    if ( g_qeglobals.d_numpoints )
    {
        select_t prevMode = g_qeglobals.d_select_mode;
        g_qeglobals.d_select_mode = sel_edge;     // toggle ON
        if ( prevMode == sel_cycle_edge_direction_quad )
        {
            CMainFrame_UpdatePatchToolbarButtons();
            g_nUpdateBits = -1;
            return;
        }
        if ( prevMode == sel_addpoint )
            sub_43ECB0();
    }
    g_nUpdateBits = -1;
}

// â”€â”€ DROP SELECTION TO FLOOR (Selectionâ†’Drop to Floor, ID 33183) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// CMainFrame::OnDropSelected (0x425be0): for each selected brush/entity, trace a
// ray straight DOWN from its origin (fixed-size point entities start +16 up) and
// move it to the first surface below. Fixed-size entities optionally orient to the
// floor normal (OrientModel) and get a random pitch/roll/yaw + modelscale scatter
// (DropModel). The drop is raised by DropHeight unless ForceZeroDropHeight; non-
// fixed-size brushes snap to the grid unless NoClamp. The whole pass is one undo
// bracket, and (like the binary) prefs are reloaded from the registry at entry.
//   The grid-snap loop's apparent IDB off-by-one (reads pos@+0x54.., writes @+0x50..) is a
//   stack-shift display artifact, not a shifted write; both arrays are the same pos[3].
extern void          sub_47CBA0( selbrush_t *b, int axis, float deg );                       // select.cpp (0x47CBA0)
extern char         *va( const char *fmt, ... );                                             // q_shared
extern edTrace_t    *Trace_AllDirectionsIfFailed( float *cam_origin, edTrace_t *trace_result,
                                                  float *dir, int contents );                // select.cpp (0x48DAA0)
extern void          AlignEntityToFace_OrientToFloor( entity_s_def *ent, float *dir );        // entity.cpp (0x485AD0)
extern entity_s     *Brush_Move( const float *move, brush_t *def, char snap );               // brush.cpp (0x47BA40)

static void Cmd_OnDropSelected()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "drop selection" );
    Undo_AddBrushList( &selected_brushes );
    Prefs_LoadPrefs( g_PrefsDlg );                       // binary: CPrefsDlg::LoadPrefs (0x44e330)

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        // Cancel any in-progress patch sub-mode and force plain brush-select.
        select_t prevMode = g_qeglobals.d_select_mode;
        g_qeglobals.d_select_mode = sel_brush;
        if ( prevMode == sel_cycle_edge_direction_quad )
        {
            CMainFrame_UpdatePatchToolbarButtons();
        }
        else if ( prevMode == sel_addpoint )
        {
            sub_43ECB0();                                // Patch_FinishCurveDrag (0x43ecb0)
        }

        entity_s_def *def = (entity_s_def *)b->owner->def;

        // â”€â”€ random model scatter (fixed-size entities, when DropModel set) â”€â”€
        if ( *(int *)&def->eclass->fixedsize && g_PrefsDlg->m_bDropModel )
        {
            SetKeyValue( def, "angles", "0 0 0" );
            sub_47CBA0( b, 0, (float)( (double)rand() * 0.000030517578125 ) * 5.0f - 2.5f );   // pitch Â±2.5
            sub_47CBA0( b, 1, (float)( (double)rand() * 0.000030517578125 ) * 5.0f - 2.5f );   // roll  Â±2.5
            sub_47CBA0( b, 2, (float)( (double)rand() * 0.000030517578125 ) * 360.0f );        // yaw   0..360
            float base  = (float)g_PrefsDlg->scale_base  / 100.0f;
            float range = (float)g_PrefsDlg->scale_range / 100.0f;
            float scale = (float)( (double)rand() * 0.000030517578125 ) * ( range + range ) + base - range;
            SetKeyValue( def, "modelscale", va( "%f", scale ) );
        }

        // â”€â”€ trace straight down from the entity origin (fixed-size starts +16 up) â”€â”€
        float dir[3]        = { 0.0f, 0.0f, -1.0f };
        float cam_origin[3] = { def->origin[0], def->origin[1], def->origin[2] };
        if ( *(int *)&def->eclass->fixedsize )
            cam_origin[2] += 16.0f;

        edTrace_t tr;
        Trace_AllDirectionsIfFailed( cam_origin, &tr, dir, 4610 );
        if ( !tr.hit.brush )
            continue;

        if ( *(int *)&def->eclass->fixedsize && g_PrefsDlg->m_bOrientModel )
            AlignEntityToFace_OrientToFloor( def, tr.normal );

        float pos[3];
        pos[0] = dir[0] * tr.dist + cam_origin[0];
        pos[1] = dir[1] * tr.dist + cam_origin[1];
        pos[2] = dir[2] * tr.dist + cam_origin[2];
        if ( !g_PrefsDlg->m_bForceZeroDropHeight )
            pos[2] += (float)g_PrefsDlg->m_dropHeight;

        // Non-fixed-size brushes snap to the grid (unless NoClamp).
        if ( !*(int *)&def->eclass->fixedsize && !g_PrefsDlg->m_bNoClamp )
        {
            float gs = grid_sizes[g_qeglobals.d_gridsize];
            for ( int i = 0; i < 3; ++i )
                pos[i] = (float)floor( pos[i] / gs + 0.5 ) * gs;
        }

        float move_delta[3];
        move_delta[0] = pos[0] - def->origin[0];
        move_delta[1] = pos[1] - def->origin[1];
        move_delta[2] = pos[2] - def->origin[2];
        Brush_Move( move_delta, b->def, 0 );
    }

    g_nUpdateBits = -1;
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// CMainFrame::OnSelectionMakehollow (0x425570): undo-wrapped CSG hollow of the one
// selected brush. (Selectionâ†’CSGâ†’Hollow, ID 32982.) Refuses >1 brush like the binary.
static void Cmd_OnSelectionMakehollow()
{
    // Count the selection: the binary walks at most one extra node, then errors.
    selbrush_t *b = selected_brushes.next;
    int extra = 0;
    while ( b != &selected_brushes && ++extra <= 1 )
        b = b->next;
    if ( b != &selected_brushes )
    {
        Sys_Printf( "Can't hollow more than 1 brush at a time.\n" );
        return;
    }
    Undo_ClearRedo();
    Undo_GeneralStart( "hollow" );
    Undo_AddBrushList( &selected_brushes );
    CSG_MakeHollow();
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// CMainFrame::OnSelectionCsgmerge (0x4255d0): undo-wrapped CSG merge of the
// selection into one convex brush. (Selectionâ†’CSGâ†’Merge, ID 32927.)
static void Cmd_OnSelectionCsgmerge()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "CSG merge" );
    Undo_AddBrushList( &selected_brushes );
    CSG_Merge();
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// â”€â”€ CLIPPER command handlers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// The clipper cluster (xywnd.cpp). OnViewClipper toggles clip mode (cancelling mouse-rotate
// mode on the way IN) and check-marks the toolbar button; OnClipSelected commits the kept
// side (Enter), undo-bracketed; OnFlipClip swaps which side the clip keeps (X).
extern void Ed_SetClipMode( char bMode );    // xywnd.cpp (0x465430)
struct xywndState_t;
extern xywndState_t *Ed_ActiveXY();           // xywnd.cpp — the one 2D view (U-GLOBALS)
extern void Ed_Clip( xywndState_t *wnd );     // xywnd.cpp (0x46dc10) — xywndState_t* overload (U-RIP done)
extern void Ed_SplitClip();                   // xywnd.cpp (0x46dd30)
extern void Ed_FlipClip();                    // xywnd.cpp (0x46ddf0)
extern int  g_bClipMode;                      // engine_stubs.cpp 0x23f16d8
extern bool g_bRotateMode;                    // drag.cpp (0x23F16D9)
extern int  g_bPatchBendMode;                 // pmesh.cpp (0x25D5B04)
void Ed_InvalidateAllViews();                 // defined below (repaint broadcast)


extern void Patch_BendHandleEnter();          // pmesh.cpp (0x447B70)


// Region menu wrappers - 5-byte jmp thunks in the IDB (0x4252b0..0x4252f0) to the map.cpp
// cores; no undo wrapper (a selection change, not an edit).
extern void Map_RegionOff();              // map.cpp (0x487530)
extern void Map_RegionXY();               // map.cpp (0x4877d0)
extern void Map_RegionTallBrush();        // map.cpp (0x487860)
extern void Map_RegionBrush();            // map.cpp (0x4878e0)
extern void Map_RegionSelectedBrushes();  // map.cpp (0x487720)

static void Cmd_OnRegionOff()          { Map_RegionOff(); }            // 0x4252b0
static void Cmd_OnRegionSetxy()        { Map_RegionXY(); }            // 0x4252f0
static void Cmd_OnRegionSettallbrush() { Map_RegionTallBrush(); }     // 0x4252e0
static void Cmd_OnRegionSetbrush()     { Map_RegionBrush(); }         // 0x4252c0
static void Cmd_OnRegionSetselection() { Map_RegionSelectedBrushes(); } // 0x4252d0

// â”€â”€ BRUSH â†’ PRIMITIVES command handlers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Reshape the selected brush into an N-sided cylinder / cone / sphere.  The binary pops the
// IDD_ARBITRARY_SIDES modal (SidesDlgProc 0x495F00), which dispatches to
// Brush_MakeSided{,Cone,Sphere} via the g_bDoCone / g_bDoSphere flags.  All three handlers
// are undo-bracketed (0x424EE0 / 0x429170 / 0x42B630).
extern void Brush_MakeSided_Prolog( unsigned int sides, char snap ); // brush.cpp (0x4735E0)
extern void Brush_MakeSidedCone( int sides );                        // brush.cpp (0x47BC10)
extern void Brush_MakeSidedSphere( int sides );                      // brush.cpp (0x47BE90)

// IDB globals 0x25D5B38 / 0x25D5B39 â€” which primitive SidesDlgProc builds.
char g_bDoCone   = 0;
char g_bDoSphere = 0;

// SidesDlgProc (0x495F00) â€” the modal "number of sides" dialog procedure.  Reads
// the IDC_ARB_SIDES_IN edit field on OK and builds the primitive selected by the
// g_bDoCone / g_bDoSphere flags.  Faithful to the binary (atol of the field text).
static INT_PTR CALLBACK SidesDlgProc( HWND hDlg, UINT msg, WPARAM wParam, LPARAM )
{
    if ( msg == WM_INITDIALOG )
    {
        ::SetFocus( ::GetDlgItem( hDlg, IDC_ARB_SIDES_IN ) );
        return FALSE;       // we set focus ourselves (return 0 like the binary)
    }
    if ( msg != WM_COMMAND )
        return FALSE;

    WORD id = LOWORD( wParam );
    if ( id == IDCANCEL )
    {
        ::EndDialog( hDlg, 0 );
        return FALSE;
    }
    if ( id != IDOK )
        return FALSE;

    char text[256] = { 0 };
    ::GetWindowTextA( ::GetDlgItem( hDlg, IDC_ARB_SIDES_IN ), text, 255 );
    int sides = atol( text );
    if ( g_bDoCone )
        Brush_MakeSidedCone( sides );
    else if ( g_bDoSphere )
        Brush_MakeSidedSphere( sides );
    else
        Brush_MakeSided_Prolog( (unsigned int)sides, 1 );
    ::EndDialog( hDlg, 1 );
    return FALSE;
}

// Run the modal sides dialog, undo-bracketed (shared by all three handlers).
static void Radiant_RunSidesDialog( const char *undoName, char doCone, char doSphere )
{
    Undo_ClearRedo();
    Undo_GeneralStart( undoName );
    Undo_AddBrushList( &selected_brushes );
    g_bDoCone   = doCone;
    g_bDoSphere = doSphere;
    // The modal's owner is the frame HWND, and its module is d_hInstance (both set at
    // boot in radiant_main.cpp) — was g_pParentWnd->m_hWnd / AfxGetInstanceHandle().
    ::DialogBoxParamA( g_qeglobals.d_hInstance, MAKEINTRESOURCE( IDD_ARBITRARY_SIDES ),
                       g_qeglobals.d_hwndMain,
                       SidesDlgProc, 0 );
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}


// â”€â”€ SELECTION menu wrappers (batch â€” thin forwards to already-ported cores) â”€â”€â”€
// Each transcribed from the IDB; the Make Detail/Structural pair is undo-bracketed exactly
// as the binary.
extern void Select_Connected();                          // select.cpp (0x490EC0)
extern void Select_ByClass( const char *key );           // select.cpp (0x490A70)
extern LRESULT Select_Ungroup();                         // entity.cpp (0x490780)
extern void Select_ChangeBrushType( int contents, int mask ); // select.cpp (0x491790)

static void Cmd_OnSelectConneted()           // 0x425550 â€” Selectionâ†’Select Connected (33134)
{
    Select_Connected();
}

static void Cmd_OnSelectionTargetname()      // 0x426390 â€” Selectionâ†’Select Targetname (33132)
{
    Select_ByClass( "targetname" );
}

static void Cmd_OnSelectionClassname()       // 0x4263A0 â€” Selectionâ†’Select Classname (202)
{
    Select_ByClass( "classname" );
}

static void Cmd_OnSelectionUngroupentity()   // 0x426380 â€” Selectionâ†’Ungroup entity (33035)
{
    Select_Ungroup();
}

extern void Select_ByKeyValue();                         // select.cpp (0x490C00)

static void Cmd_OnSelectionKeyValue()        // 0x4263B0 â€” Selectionâ†’Select by Key/Value (33133)
{
    Select_ByKeyValue();
}

static void Cmd_OnSelectionMakeDetail()      // 0x4261C0 â€” Selectionâ†’Make Detail (33042)
{
    Undo_ClearRedo();
    Undo_GeneralStart( "make detail" );
    Undo_AddBrushList( &selected_brushes );
    Select_ChangeBrushType( 0x8000000, 8320 );
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

static void Cmd_OnSelectionMakeStructural()  // 0x426200 â€” Selectionâ†’Make Structural (33043)
{
    Undo_ClearRedo();
    Undo_GeneralStart( "make structural" );
    Undo_AddBrushList( &selected_brushes );
    Select_ChangeBrushType( 0, 134226052 );
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// â”€â”€ SCRIPT-GROUP / FIXED-SIZE LIGHT command handlers (0x428E10-0x428EE0) â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Thin CMainFrame methods the binary jumps through to the brush.cpp/pmesh.cpp cores,
// transcribed verbatim incl. the exact float factors.
extern void DisassociateEntities();          // brush.cpp 0x47a1b0
extern void SelectedAssociated();            // brush.cpp 0x47a330
extern void OverbrightShift( float a1 );     // brush.cpp 0x47a600
extern void Light_01( float a1 );            // brush.cpp 0x47a790
extern void Light_Height( float a1 );        // brush.cpp 0x47a910
extern void Patch_Subdivide( int decrease ); // pmesh.cpp 0x44cde0
// Script-Group colour command cores (scriptgroup.cpp).
extern void ScriptGroup_SyncGroupKeyToTeam();   // 0x454480 (force ScriptGroupKey = ScriptColorTeamKey)
extern void ScriptGroup_AddColorToSelection();  // 0x4543B0
extern void ScriptGroup_TriggerNumber();        // 0x453170
extern bool ScriptGroup_SelectionHasTrigger();  // (helper used by OnScriptGroup_Disassociate)
extern void AssociateEntities();                // 0x455A80 (Script-Group dialog toggle/open)
extern int  UpdateSelection( int wParam, eclass_t *cls );   // win_ent.cpp 0x497180 (declared again @2812)
extern int  Sys_Printf( const char *fmt, ... );             // win_qe3.cpp 0x499E90 (declared again @2021)

// 0x428E00 (thunk)  CMainFrame::OnAssociateEntities â€” Associate-Entities accelerator (Shift+G):
// open the Script-Group dialog.
static void Cmd_OnAssociateEntities()
{
    AssociateEntities();
}

// 0x424E20 (thunk)  CMainFrame::OnScriptGroup â€” the "Script group" menu item (id 200): same
// AssociateEntities core as OnAssociateEntities (a distinct thunk in the binary).
static void Cmd_OnScriptGroup()
{
    AssociateEntities();
}

static void Cmd_OnDisassociateEntities()   // 0x428E10 (thunk â†’ DisassociateEntities)
{
    DisassociateEntities();
}

static void Cmd_OnSelectedAssociated()     // 0x428E20 (thunk â†’ SelectedAssociated)
{
    SelectedAssociated();
}

// 0x4264A0  CMainFrame::OnScriptGroup_01 â€” "Add Color Group" command: force the colour-team
// key, then run ScriptGroup_AddColorToSelection.
static void Cmd_OnScriptGroup_01()
{
    ScriptGroup_SyncGroupKeyToTeam();
    ScriptGroup_AddColorToSelection();
}

// 0x426460  CMainFrame::OnScriptGroup_Disassociate â€” the Script-Group "Disassociate" command:
// force the colour-team key, require a trigger in the selection, then ScriptGroup_TriggerNumber.
static void Cmd_OnScriptGroup_Disassociate()
{
    ScriptGroup_SyncGroupKeyToTeam();
    if ( !ScriptGroup_SelectionHasTrigger() )
    {
        Sys_Printf( "You must select a trigger_multiple or trigger_radius in combination with the nodes, AI, or goal volumes you with to disassociate.\n" );
        return;
    }
    ScriptGroup_TriggerNumber();
    UpdateSelection( 0xFFFFFFFF, 0 );
    g_nUpdateBits = -1;
}

static void Cmd_OnLightShiftUp()           // 0x428E30
{
    Light_01( 1.1f );
    g_nUpdateBits = -1;
}

static void Cmd_OnLightShiftDown()         // 0x428E50
{
    Light_01( 0.9f );
    g_nUpdateBits = -1;
}

static void Cmd_OnCyclinderHeightUp()      // 0x428E70
{
    Light_Height( 1.1f );
    g_nUpdateBits = -1;
}

static void Cmd_OnCyclinderHeightDown()    // 0x428E90
{
    Light_Height( 0.9f );
    g_nUpdateBits = -1;
}

// â”€â”€ Camera raise/lower (D / C) â€” IDB OnCameraUp 0x426900 / OnCameraDown 0x426680 â”€â”€
//   Nudge the 3D camera up/down by 32 world units along Z, then redraw the camera (W_CAMERA)
//   and, if "camera updates XY" is on, the 2D view (W_XY).  Faithful to the binary's
//   `g_nUpdateBits |= 2*(m_bCamXYUpdate!=0)+1`.
static void Cmd_OnCameraUp()               // 0x426900 (cmd 33055, 'D')
{
    Ed_Camera()->origin[2] += 32.0f;   // U-GLOBALS
    g_nUpdateBits |= 2 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
}

static void Cmd_OnCameraDown()             // 0x426680 (cmd 33056, 'C')
{
    Ed_Camera()->origin[2] -= 32.0f;   // U-GLOBALS
    g_nUpdateBits |= 2 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
}

// â”€â”€ Camera pitch up/down (A / Z) â€” IDB OnCameraAngleUp 0x4265d0 / OnCameraAngleDown 0x426590 â”€â”€
//   Tilt the 3D camera pitch (angles[0]) by Â±22.5Â°, clamped to [-85, 85], then redraw the
//   camera (W_CAMERA).  Faithful to the binary.
static void Cmd_OnCameraAngleUp()          // 0x4265d0 (cmd 33061, 'A')
{
    g_nUpdateBits |= W_CAMERA;
    Ed_Camera()->angles[0] += 22.5f;   // U-GLOBALS
    if ( Ed_Camera()->angles[0] > 85.0f )
        Ed_Camera()->angles[0] = 85.0f;
}

static void Cmd_OnCameraAngleDown()        // 0x426590 (cmd 33062, 'Z')
{
    g_nUpdateBits |= W_CAMERA;
    Ed_Camera()->angles[0] -= 22.5f;   // U-GLOBALS
    if ( Ed_Camera()->angles[0] < -85.0f )
        Ed_Camera()->angles[0] = -85.0f;
}

static void Cmd_OnOverBrightShiftUp()      // 0x428EB0
{
    OverbrightShift( -0.05f );
    Patch_Subdivide( -1 );
    g_nUpdateBits = -1;
}

static void Cmd_OnOverBrightShiftDown()    // 0x428EE0
{
    OverbrightShift( 0.05f );
    Patch_Subdivide( 1 );
    g_nUpdateBits = -1;
}


// REGION SELECTION handlers â€” the IDB thunks (0x426340/0x426360/0x426370/0x426350)
// are 5-byte jmps straight to the cores (no undo wrapper: a selection change, not an
// edit). Each core takes the single selected brush as a box, deletes it, then
// (re)selects the brushes matching the box test (see select.cpp).
extern void Select_CompleteTall();   // select.cpp (0x490170)
extern void Select_PartialTall();    // select.cpp (0x4903D0)
extern void Select_Touching_R();     // select.cpp (0x490520)
extern void Select_Inside_R();       // select.cpp (0x490650)

static void Cmd_OnSelectionCompleteTall()   // 0x426340 â€” Select Complete Tall (32984)
{
    Select_CompleteTall();
}

static void Cmd_OnSelectionPartialTall()    // 0x426360 â€” Select Partial Tall (32983)
{
    Select_PartialTall();
}

static void Cmd_OnSelectionTouching()       // 0x426370 â€” Select Touching (32986)
{
    Select_Touching_R();
}

static void Cmd_OnSelectionInside()         // 0x426350 â€” Select Inside (33008)
{
    Select_Inside_R();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// MENU-WRAPPER BATCH - thin On* handlers forwarding to already-ported cores, each
// transcribed from the IDB (EAs below).
extern void    Select_Delete();                               // select.cpp (0x48E760)
extern void    Brush_AutoCaulk();                             // csg.cpp    (0x47E0F0)
extern void    Select_ChangeBrushToolflags( int set, int clear );// select.cpp (0x491890)
extern void    Undo_AddEntity_W( entity_s *def );            // undo.cpp   (0x45E990)
extern undo_s *g_lastundo;                                   // undo.cpp   (0x23F162C)

// CMainFrame::OnSelectionDelete (0x425690) - Edit->Delete (33003 / Backspace).  Undo-brackets
// the selection, then per selected brush adds its OWNER entity + that entity's whole brush-def
// list to undo (so a Delete that empties an entity can be restored), then Select_Delete().
// The per-iteration AddEntity + def-list walk IS the binary's inlined Undo_AddEntity_W.
static void Cmd_OnSelectionDelete()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "delete" );
    Undo_AddBrushList( &selected_brushes );
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
    {
        if ( g_lastundo )
            Undo_AddEntity_W( (entity_s *)i->owner->def );
        else
            Sys_Printf( "Undo_AddEntity: no last undo.\n" );
    }
    Select_Delete();
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// CMainFrame::OnSelectionAutoCaulk (0x425600) â€” Selectionâ†’CSGâ†’Auto Caulk (33220).
// Undo-bracketed Brush_AutoCaulk (caulks selected-brush faces hidden by neighbours).
static void Cmd_OnSelectionAutoCaulk()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "Auto caulk" );
    Undo_AddBrushList( &selected_brushes );
    Brush_AutoCaulk();
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// CMainFrame::OnSelectionMakeWeaponclip (0x426240) â€” Make Weapon Clip (196). Sibling
// of Make Detail/Structural â€” sets the weapon-clip contents bits via Select_ChangeBrushType.
static void Cmd_OnSelectionMakeWeaponclip()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "make weaponclip" );
    Undo_AddBrushList( &selected_brushes );
    Select_ChangeBrushType( 0x8002080, 0 );
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// CMainFrame::OnSelectionMakeNonColliding (0x426280) â€” Make Non-Colliding (197).
static void Cmd_OnSelectionMakeNonColliding()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "make noncolliding" );
    Undo_AddBrushList( &selected_brushes );
    Select_ChangeBrushType( 134217732, 0 );
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// CMainFrame::OnSelectionMakeSplitCoplanar (0x4262C0) â€” Make Split Coplanar Geo (33223).
// Sets brush-face toolflag bit 256 (split-coplanar) via Select_ChangeBrushToolflags.
static void Cmd_OnSelectionMakeSplitCoplanar()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "make split coplanar geo" );
    Undo_AddBrushList( &selected_brushes );
    Select_ChangeBrushToolflags( 256, 0 );
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// CMainFrame::OnSelectionMakeDontSplitCoplanar (0x426300) â€” Make Don't Split Coplanar (33224).
static void Cmd_OnSelectionMakeDontSplitCoplanar()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "make don't split coplanar geo" );
    Undo_AddBrushList( &selected_brushes );
    Select_ChangeBrushToolflags( 0, 256 );
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// CMainFrame::OnViewCenter (0x423C50) â€” Viewâ†’Center (32953 / End). Levels the camera
// (zero pitch/roll) and snaps its yaw to the nearest 22.5Â° increment, then repaints
// the camera + Z views. Pure inline camera math (no core dependency); transcribed
// verbatim â€” note floor((yaw+11)/22.5)*22.5 is the binary's snap (NOT a rounding helper).
static void Cmd_OnViewCenter()
{
    {
        camera_s *cam = Ed_Camera();   // U-GLOBALS: never NULL â€” m_pCamWnd guard dropped
        cam->angles[0] = 0.0f;
        cam->angles[2] = 0.0f;
        cam->angles[1] =
            (float)( floor( ( cam->angles[1] + 11.0 ) / 22.5 ) * 22.5 );
    }
    g_nUpdateBits |= ( W_CAMERA | W_XY_OVERLAY );   // binary: |= 5u (0x1 | 0x4)
}

extern LRESULT Texture_ShowInuse();   // texwnd.cpp (0x45B850) â€” mark/count in-use textures

// CMainFrame::OnViewUpfloor (0x424700) / OnViewDownfloor (0x423ED0) â€” Viewâ†’Up/Down Floor
// (32954/32955). Snap the camera onto the nearest brush surface above/below it
// (CCamWnd::Cam_ChangeFloor, now ported in camwnd.cpp). Verbatim one-line handlers.
static void Cmd_OnViewUpfloor()
{
    CamWnd_ChangeFloor( 1 );   // U-GLOBALS: was CCamWnd::Cam_ChangeFloor( m_pCamWnd, 1 )
}
static void Cmd_OnViewDownfloor()
{
    CamWnd_ChangeFloor( 0 );   // U-GLOBALS: was CCamWnd::Cam_ChangeFloor( m_pCamWnd, 0 )
}

// CMainFrame::OnTexturesShowinuse (0x424B20) â€” Texturesâ†’Show In Use (32974). Mark only
// the textures the map references, then redraw the texture browser. Faithful to the
// binary: wait-cursor â†’ Texture_ShowInuse() â†’ RedrawWindow(m_pTexWnd) if present.
static void Cmd_OnTexturesShowinuse()
{
    HCURSOR prev = SetCursor( LoadCursorA( 0, (LPCSTR)IDC_WAIT ) );
    Texture_ShowInuse();
    (void)prev;
    if ( g_qeglobals.d_hwndTexture )   // U-CMD-1: was m_pTexWnd->m_hWnd
        ::RedrawWindow( g_qeglobals.d_hwndTexture, 0, 0,
                        RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW );
}

// CMainFrame::OnTexturesShowall (0x42B440) â€” Texturesâ†’Show All (32973 / Ctrl-A).  The command
// is OVERLOADED in the binary: with a trigger in the selection it is the Script-Group "add
// colour" command instead, and only otherwise does it un-hide every registered material.
extern LRESULT Texture_ShowAll();   // texwnd.cpp (0x45b730)
static void Cmd_OnTexturesShowall()
{
    if ( ScriptGroup_SelectionHasTrigger() )
    {
        ScriptGroup_SyncGroupKeyToTeam();
        ScriptGroup_AddColorToSelection();
        return;
    }
    Texture_ShowAll();
    if ( g_qeglobals.d_hwndTexture )   // U-CMD-1: was m_pTexWnd->m_hWnd
        ::RedrawWindow( g_qeglobals.d_hwndTexture, 0, 0,
                        RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW );
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// TEXTURE REFRESH / RESOLUTION / WINDOW-SCALE cluster (Textures menu).  The image-reload
// backend (R_ReloadImages / R_UpdateMipMap) is in gfx_d3d/r_image.cpp under KISAK_RADIANT;
// kisak's imageGlobals.imageHashTable[32768] IS the binary's flat imageGlobals[32768].

void __cdecl R_ReloadImages();   // gfx_d3d/r_image.cpp (idb R_ReloadImages 0x513D70; decl in r_image.h)
void __cdecl R_UpdateMipMap();    // gfx_d3d/r_image.cpp (idb R_UpdateMipMap  0x5139A0; decl in r_image.h)

// CMainFrame::OnTextureRefresh (0x428B50) â€” Texturesâ†’Refresh Textures (33204, F5).  Reload
// every loose-file image from disk so edited TGA/DDS/IWI shows up without restarting, then
// invalidate all views.  Verbatim: R_ReloadImages(); g_nUpdateBits = -1.
static void Cmd_OnTextureRefresh()
{
    R_ReloadImages();
    g_nUpdateBits = -1;
}

// CMainFrame::PicMip (0x420860) â€” apply g_qeglobals.d_savedinfo.d_picmip to the r_picmip*
// dvars and check the matching Texture-Resolution radio item.  Verbatim from the binary,
// adapted to kisak's Dvar_SetIntByName(name, value) arg order (the binary lists them
// swapped; see gfxwrapper.cpp).  d_picmip is 0..3 (Maximum..Low); the menu ids are
// 36115+d_picmip.  (Bump/spec are pinned to 3, exactly as the binary.)
void Radiant_PicMip()
{
    int picmip = g_qeglobals.d_savedinfo.d_picmip;
    iassert( picmip >= 0 && picmip <= 3 );   // idb: menuID in [36115, 36118]
    Dvar_SetIntByName( "r_picmip",      picmip );
    Dvar_SetIntByName( "r_picmip_spec", 3 );
    Dvar_SetIntByName( "r_picmip_bump", 3 );
    // U-CMD-1: the binary's GetMenu()/CMenu::CheckMenuItem pair IS ::GetMenu/::CheckMenuItem,
    // so the radio update is shell-agnostic over the frame's raw HMENU.
    if ( HMENU menu = Radiant_FrameMenu() )
    {
        ::CheckMenuItem( menu, 36115, MF_BYCOMMAND | MF_UNCHECKED );
        ::CheckMenuItem( menu, 36116, MF_BYCOMMAND | MF_UNCHECKED );
        ::CheckMenuItem( menu, 36117, MF_BYCOMMAND | MF_UNCHECKED );
        ::CheckMenuItem( menu, 36118, MF_BYCOMMAND | MF_UNCHECKED );
        ::CheckMenuItem( menu, 36115 + picmip, MF_BYCOMMAND | MF_CHECKED );
    }
}

// CMainFrame::OnTextureResolution (0x424340) â€” Texturesâ†’Texture Resolution (36115..36118 =
// Maximum/High/Normal/Low).  If the picked level differs from the current d_picmip, store it,
// push the r_picmip dvars (PicMip), propagate them into imageGlobals.picmip* (R_UpdateMipMap),
// reload every loose-file image at the new mip level (R_ReloadImages), and invalidate all
// views (g_nUpdateBits = -1).  Verbatim from the binary (id âˆ’ 36115 == the picmip level).
static void Cmd_OnTextureResolution( UINT nID )
{
    int level = (int)nID - 36115;
    if ( level != g_qeglobals.d_savedinfo.d_picmip )
    {
        g_qeglobals.d_savedinfo.d_picmip = level;
        Radiant_PicMip();
        R_UpdateMipMap();
        R_ReloadImages();
        g_nUpdateBits = -1;
    }
}

// â”€â”€ Texturesâ†’Texture Filter submenu (33226..33230) â€” 0x424200 / 0x424250 / 0x4242A0 /
//    Each handler: find the malleable "r_textureMode" dvar and set its string from source
//    (the binary's 2-arg Dvar_SetStringFromSource passes source 0 == DVAR_SOURCE_INTERNAL),
//    or register it fresh with flags 0x4000 (DVAR_EXTERNAL) + description "External Dvar";
//    then `or g_nUpdateBits, 1` (== W_CAMERA).
//    KISAK: CoD4's renderer consults r_textureMode in a global sampler-filter override;
//    kisak's CoD3 renderer bakes min/mag/mip into each technique's GfxStateBits.samplerState
//    and has no such override, so the dvar is written faithfully but nothing consumes it.
static void Radiant_SetTextureMode( const char *mode )
{
    const dvar_s *v = Dvar_FindVar( "r_textureMode" );          // idb Dvar_FindMalleableVar 0x4B0F00
    if ( v )
        Dvar_SetStringFromSource( (dvar_s *)v, (char *)mode, DVAR_SOURCE_INTERNAL );
    else
        Dvar_RegisterString( "r_textureMode", mode, 0x4000, "External Dvar" );
    g_nUpdateBits |= W_CAMERA;
}
static void Cmd_OnTextureFilterNearest()     { Radiant_SetTextureMode( "nearest" ); }      // 33226 (0x424200)
static void Cmd_OnTextureFilterLinear()      { Radiant_SetTextureMode( "linear" ); }       // 33227 (0x424250)
static void Cmd_OnTextureFilterBilinear()    { Radiant_SetTextureMode( "bilinear" ); }     // 33228 (0x4242A0)
static void Cmd_OnTextureFilterTrilinear()   { Radiant_SetTextureMode( "trilinear" ); }    // 33229 (0x4242F0)
static void Cmd_OnTextureFilterAnisotropic() { Radiant_SetTextureMode( "anisotropic" ); }  // 33230 (0x424380)

// â”€â”€ Texturesâ†’Render Method (32990..32994) â€” the binary's ON_COMMAND_RANGE handler
//    OnRendermethodCaseTextures (0x4243D0) is a one-liner: `Texture_SetMode(nID)`.
//    Texture_SetMode (0x45A520, texwnd.cpp) maps the five menu ids onto camera draw modes
//    0..4 (Wireframe / Fullbright / Normal-based fake lighting / View-based fake lighting /
//    Case textures) and stores the picked id in d_savedinfo.iTextMenu.
//    NOTE this is a DIFFERENT feature from OnRenderMethod{Material,Lightmap,Smoothing}
//    (33232/33233/36100 â†’ Material_SetMode) â€” those stay exactly where they are.
extern void Texture_SetMode( int iTexMenu );                                // texwnd.cpp (0x45A520)
static void Cmd_OnRendermethodCaseTextures( UINT nID ) { Texture_SetMode( (int)nID ); }

// CMainFrame::CheckTextureScale (0x42AF50) â€” check the picked Texture-Window-Scale radio item
// (32894..32898 = 200/100/50/25/10%), persist prefs, reset the browser scroll, and repaint the
// texture window.  Verbatim; the binary's raw `g_nUpdateBits |= 0x10` is W_TEXTURE (qedefs
// macros are aligned to the binary values, so the symbol equals the raw 0x10).
void Radiant_CheckTextureScale( UINT uIDCheckItem )
{
    if ( HMENU menu = Radiant_FrameMenu() )   // U-CMD-1: raw HMENU == the binary's CMenu pair
    {
        ::CheckMenuItem( menu, 32898, MF_BYCOMMAND | MF_UNCHECKED );   // 10%
        ::CheckMenuItem( menu, 32897, MF_BYCOMMAND | MF_UNCHECKED );   // 25%
        ::CheckMenuItem( menu, 32896, MF_BYCOMMAND | MF_UNCHECKED );   // 50%
        ::CheckMenuItem( menu, 32895, MF_BYCOMMAND | MF_UNCHECKED );   // 100%
        ::CheckMenuItem( menu, 32894, MF_BYCOMMAND | MF_UNCHECKED );   // 200%
        ::CheckMenuItem( menu, uIDCheckItem, MF_BYCOMMAND | MF_CHECKED );
    }
    Prefs_SavePrefs( g_PrefsDlg );   // idb CPrefsDlg::SavePrefs(g_PrefsDlg) 0x44f280
    Texture_ResetPosition();
    g_nUpdateBits |= W_TEXTURE;   // idb `or g_nUpdateBits, 10h` (W_TEXTURE == 0x10)
}

// The five Texture-Window-Scale menu items (32894..32898).  Each stores the percentage in
// g_PrefsDlg->m_nTextureWindowScale then CheckTextureScale(id).  Verbatim (0x42B020..0x42AFE0).
static void Cmd_OnTexturesTexturewindowscale10()  { g_PrefsDlg->m_nTextureWindowScale = 10;  Radiant_CheckTextureScale( 32898 ); }
static void Cmd_OnTexturesTexturewindowscale25()  { g_PrefsDlg->m_nTextureWindowScale = 25;  Radiant_CheckTextureScale( 32897 ); }
static void Cmd_OnTexturesTexturewindowscale50()  { g_PrefsDlg->m_nTextureWindowScale = 50;  Radiant_CheckTextureScale( 32896 ); }
static void Cmd_OnTexturesTexturewindowscale100() { g_PrefsDlg->m_nTextureWindowScale = 100; Radiant_CheckTextureScale( 32895 ); }
static void Cmd_OnTexturesTexturewindowscale200() { g_PrefsDlg->m_nTextureWindowScale = 200; Radiant_CheckTextureScale( 32894 ); }

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// INSPECTOR TAB SWITCHING - each command toggles the right-column inspector to its mode (or
// hides the inspector if it is already in that mode + visible), then SetInspectorMode.

// (Radiant_ToggleInspectorMode removed — the native entity-window inspector modes are all
// ImGui panels/dock tabs now: Entity → ImGuiPanel_Entity_Toggle, Textures/Console →
// ImGuiShell_FocusTab, Filters → ImGuiPanel_Filters_Toggle. Nothing drives the old HWND path.)

// CMainFrame::OnViewEntity (0x423f00) â€” Viewâ†’Toggleâ†’Entity View (33017).
// Dock-world routing (the inspector-mode toggles drove the MFC tab strip, which
// early-returns without d_hwndEntity — N/O/texture-view were dispatching into silence):
// Entity -> the ImGui panel; Textures/Console -> focus the dock tab; Filters (F) stays
// on the inert old path until the filters panel exists (POST-RIP).
extern void ImGuiPanel_Entity_Toggle();                  // imgui_panel_entity.cpp
extern void ImGuiPanel_Filters_Toggle();                 // imgui_panel_filters.cpp
extern void ImGuiShell_FocusTab( const char *title );    // imgui_shell.cpp
static void Cmd_OnViewEntity()      { ImGuiPanel_Entity_Toggle(); }
// CMainFrame::OnViewTexture (0x424440) â€” Texturesâ†’Texture inspector tab (33018).
static void Cmd_OnViewTextureMode() { ImGuiShell_FocusTab( "Textures" ); }
// CMainFrame::OnViewConsole (0x423e10) â€” Viewâ†’Toggleâ†’Console View (33016, hotkey O).
static void Cmd_OnViewConsole()     { ImGuiShell_FocusTab( "Console" ); }
// CMainFrame::OnFilterDlg (0x42b7a0) â€” Viewâ†’Filter Settings (33104, hotkey F).  Routes to the
// ImGui Filters panel (imgui_panel_filters.cpp) — the CFilterWnd replacement — instead of the
// dead native entity-window filter mode (Radiant_ToggleInspectorMode no-ops with no d_hwndEntity).
static void Cmd_OnFilterDlg()       { ImGuiPanel_Filters_Toggle(); }

// â”€â”€ Textures-menu Usage / Locale / Surface-type filter command handlers â”€â”€â”€â”€â”€â”€â”€â”€â”€
// The FillTextureMenu-built submenus (ids base+arrayIndex) dispatch here.  Each thin
// wrapper subtracts the submenu's id base to recover the filter index and forwards to the
// texwnd.cpp core.  IDB CMainFrame::OnFilterUsage 0x4243e0 / OnFilterLocale 0x424400 /
// OnFilterSurfaceType 0x424420 (each: `add nID, -base; call TexWnd_*Filter`).
extern void TexWnd_UsageFilter( int index );             // texwnd.cpp (0x45B3B0)
extern void TexWnd_localFilter( int index );             // texwnd.cpp (0x45B490)
extern void TexWnd_SurfaceTypeFilter( unsigned int index ); // texwnd.cpp (0x45B570)
static void Cmd_OnFilterUsage( UINT nID )       { TexWnd_UsageFilter( (int)nID - 60000 ); }        // 0x4243ed
static void Cmd_OnFilterLocale( UINT nID )      { TexWnd_localFilter( (int)nID - 60256 ); }        // 0x42440d
static void Cmd_OnFilterSurfaceType( UINT nID ) { TexWnd_SurfaceTypeFilter( (unsigned int)( nID - 60512 ) ); } // 0x42442d

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// SELECTION TRANSFORMS - Clone + Flip/Rotate X/Y/Z; each transcribed from the IDB.
extern void Clone_Selection( float gridSize );                              // select.cpp (0x48F0D0)
extern void DoFlip( int axis, const char *opName );                         // (0x424F30) â€” body below
extern void Select_GetMid( float *mid );                                    // select.cpp (0x48FC70)
extern void Select_RotateAxis( int axis, float deg, float (*rot_around)[4][3] ); // select.cpp (0x48FF40)
extern void Select_FlipAxis( int axis );                                    // select.cpp (0x48FD50)
extern void Select_ApplyMatrix_SelectedBrushes( int bSnap, float *mat, float deg, char bSwap ); // select.cpp (0x48FD10)
extern void sub_47B940( brush_t *def );                                     // brush.cpp (Brush_UpdateSpecialMaterialFlag, real)
extern int  UpdateSelection( int wParam, eclass_t *cls );                   // win_ent.cpp (0x497180)

// CMainFrame::OnSelectionClone (0x425480) â€” Selectionâ†’Clone (33001). Clones the
// selection (in-memory clone, see select.cpp note), then refreshes render flags on
// every selected + active brush DEF (sub_47B940 = Brush_UpdateSpecialMaterialFlag, now
// a real port in brush.cpp â€” refreshes the 2D back-face-cull hint from the face materials).
static void Cmd_OnSelectionClone()
{
    Clone_Selection( grid_sizes[g_qeglobals.d_gridsize] );
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
        sub_47B940( i->def );
    for ( selbrush_t *j = active_brushes.next; j != &active_brushes; j = j->next )
        sub_47B940( j->def );
}

// CMainFrame::OnEditCopybrush (0x4286B0) â€” Editâ†’Copy (33039). Serialise the selection
// to the in-app clipboard via the active XY view.
static void Cmd_OnEditCopybrush()
{
    XYWnd_CopyClip();   // U-CMD-1: was m_pActiveXY->Copy()
}

// CMainFrame::OnEditPastebrush (0x4286D0) â€” Editâ†’Paste (33040). Re-parse the clipboard
// (placing + selecting), then refresh the special-material flag on every selected +
// active brush DEF (sub_47B940 = Brush_UpdateSpecialMaterialFlag, real port; identical
// tail to OnSelectionClone).
static void Cmd_OnEditPastebrush()
{
    XYWnd_PasteClip();   // U-CMD-1: was m_pActiveXY->Paste()
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
        sub_47B940( i->def );
    for ( selbrush_t *j = active_brushes.next; j != &active_brushes; j = j->next )
        sub_47B940( j->def );
}

// CMainFrame::OnBrushFlipx/y/z (0x4250A0/0x4250C0/0x4250E0) â€” Brushâ†’Flipâ†’X/Y/Z
// (32956/57/58). Each forwards to DoFlip (select.cpp), which undo-brackets the
// selection, mirrors it across the axis through the pivot (Select_FlipAxis), and for
// fixed-size entities also flips their `angles` key by 180Â°.
static void Cmd_OnBrushFlipx() { DoFlip( 0, "flip X" ); }
static void Cmd_OnBrushFlipy() { DoFlip( 1, "flip Y" ); }
static void Cmd_OnBrushFlipz() { DoFlip( 2, "flip Z" ); }

// CMainFrame::OnBrushRotatex/y/z (0x425100/0x425190/0x425220) â€” Brushâ†’Rotateâ†’X/Y/Z
// (32959/60/61). The canonical transform pattern: undo bracket â†’ Select_GetMid pivot â†’
// build the 90Â° rotation matrix (Select_RotateAxis) â†’ apply to every selected brush
// (Select_ApplyMatrix_SelectedBrushes) â†’ refresh selection â†’ undo end.
static void Radiant_RotateSelection( int axis, const char *opName )
{
    if ( selected_brushes.next == &selected_brushes )
        return;
    Undo_ClearRedo();
    Undo_GeneralStart( opName );
    Undo_AddBrushList( &selected_brushes );
    float rot_around[4][3];
    Select_GetMid( rot_around[0] );
    Select_RotateAxis( axis, 90.0f, (float (*)[4][3])rot_around );
    Select_ApplyMatrix_SelectedBrushes( 0, rot_around[0], 90.0f, 0 );
    g_nUpdateBits = -1;
    UpdateSelection( -1, 0 );
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

static void Cmd_OnBrushRotatex() { Radiant_RotateSelection( 0, "rotate X" ); }
static void Cmd_OnBrushRotatey() { Radiant_RotateSelection( 1, "rotate Y" ); }
static void Cmd_OnBrushRotatez() { Radiant_RotateSelection( 2, "rotate Z" ); }

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// TOOLBAR COMMAND HANDLERS (IDR_TOOLBAR152).  MFC auto-disables toolbar buttons whose command
// id has no ON_COMMAND handler, so every button needs one.  Each toggles a pref/mode and
// reflects it on its button via TB_CHECKBUTTON (the binary's SendMessage(TB_CHECKBUTTON,
// cmdId, state)); bodies match the IDB 1:1.
extern void Brush_FlipTexture( int axis );          // select.cpp (0x492280)
extern void Brush_RotateTexture( int deg );         // select.cpp (0x4929F0)
extern void Material_SetMode( int iMode );          // texwnd.cpp (0x45B910)
extern void CMainFrame_UpdatePatchToolbarButtons(); // select.cpp (0x42AA70)
extern void Patch_InsDelToggle();                   // pmesh.cpp  (0x447E50)
extern int  g_qeglobals_redispersePatchVerts;       // engine_stubs.cpp (0x25D5A6B)
extern char g_nScaleHow;                            // drag.cpp   (0x23F16DC)
extern bool g_bScaleMode;                           // drag.cpp   (0x23F16DA)
extern bool g_bRotateMode;                          // drag.cpp   (0x23F16D9)


// â”€â”€ Texturesâ†’Flip/Rotate (toolbar) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void Cmd_OnTextureFlipX()    { Brush_FlipTexture( 0 ); }      // 0x42BF40 (33135)
static void Cmd_OnTextureFlipY()    { Brush_FlipTexture( 1 ); }      // 0x42BF50 (33136)
static void Cmd_OnTextureRotate90() { Brush_RotateTexture( 90 ); }   // 0x42BF60 (33182)

// â”€â”€ Editâ†’Cycle Layer (0x424010, 33238) â€” advance the current material layer 0â†’1â†’2â†’0. â”€
static void Cmd_OnEditLayerCycle()
{
    Material_SetMode( ( g_qeglobals.current_edit_layer + 1 ) % 3 );
}


// â”€â”€ Viewâ†’Change (0x426400, 32781) â€” cycle the active XY view type XYâ†’XZâ†’YZâ†’XY.
//    (SetViewType is a stub in this port, so the rotation is cosmetic until it lands;
//    the handler is faithful and PositionView/repaint run.) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void Cmd_OnViewChange()
{
    if ( g_radiantFrameState.currentStyle == 2 )   // U-CMD-1 (Ed_ActiveXY() is never NULL)
        return;
    // m_nViewType is ED_VIEW (2=XY top, 1=XZ front, 0=YZ side).  Cycle XYâ†’XZâ†’YZâ†’XY,
    // calling SetViewType with the matching EViewType (XY=0/XZ=1/YZ=2).
    switch ( Ed_ActiveXY()->m_nViewType )   // U-GLOBALS: was m_pXYWnd->m_nViewType
    {
    case 2:  XYWnd_SetViewType( ED_VIEW_XZ ); break;   // XY  â†’ XZ
    case 1:  XYWnd_SetViewType( ED_VIEW_YZ ); break;   // XZ  â†’ YZ
    default: XYWnd_SetViewType( ED_VIEW_XY ); break;   // YZ  â†’ XY
    }
    XYWnd_PositionView();
    g_nUpdateBits |= 2u;                               // W_XY
}


// â”€â”€ Patch curve-edit vert-lock modes (0x42B4F0 / 0x42B510 / 0x42B530) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void Cmd_ToggleLockPatchVertMode()            // 33140
{
    g_qeglobals.bLockPatchVerts   = !g_qeglobals.bLockPatchVerts;
    g_qeglobals.bUnlockPatchVerts = 0;
    CMainFrame_UpdatePatchToolbarButtons();
}

static void Cmd_ToggleUnlockPatchVertMode()          // 33139
{
    g_qeglobals.bLockPatchVerts   = 0;
    g_qeglobals.bUnlockPatchVerts = !g_qeglobals.bUnlockPatchVerts;
    CMainFrame_UpdatePatchToolbarButtons();
}

static void Cmd_OnCycleTerrainEdge()                 // 0x42B530 (33141)
{
    g_qeglobals.bLockPatchVerts   = 0;
    g_qeglobals.bUnlockPatchVerts = 0;
    if ( g_qeglobals.d_select_mode == sel_cycle_edge_direction_quad )
    {
        g_qeglobals.d_select_mode = sel_brush;
        CMainFrame_UpdatePatchToolbarButtons();
    }
    else
    {
        select_t prev = g_qeglobals.d_select_mode;
        g_qeglobals.d_select_mode = sel_cycle_edge_direction_quad;
        CMainFrame_UpdatePatchToolbarButtons();
        if ( prev == sel_addpoint )
            sub_43ECB0();                              // Patch_FinishCurveDrag
    }
    CMainFrame_UpdatePatchToolbarButtons();
}


// â”€â”€ Miscâ†’Cycle Preview Models (0x42BDD0, 35005) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Advance w_cyclePreviewMode 0â†’1â†’2â†’3â†’4â†’0 (0 = off).  For each selected brush: clear any
// existing preview-model flag (brushFlags&0x100) and bump the entity-def version so its
// model re-instances; then, if the new mode is on AND the eclass has a model name for that
// mode slot (eclass->default_model_name[mode]), re-arm the preview flag.  Verbatim from the
// IDB (the dead `if(!v1)` re-test is omitted â€” v1 = old+1 is never 0 for the 0..4 range).
extern void Entity_RebuildBounds( entity_s *e );    // entity.cpp (0x485390)

// â”€â”€ Drop Selected Relative-Z (0x425940, 35042) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// In curve/terrain point-edit mode, drop the SELECTED control points to the floor while
// PRESERVING their relative Z offsets: find the lowest selected point, trace each point
// straight down, and shift it so the lowest sits on the surface and the rest keep their
// height above it.  Cancels mouse-rotate mode (m_pActiveXY, the IDB +0x814 member confirmed
// vs OnSelectMouserotate) and re-tessellates each selected patch.  One undo bracket.
extern void Patch_Rebuild( patchMesh_t *p, char doBounds );   // pmesh.cpp (0x438D80)


// 0x4241E0  CMainFrame::OnShowRegionsForSelected â€” build the per-pixel-light-preview
// CSG regions for every selected light (camwnd.cpp Regions_ForSelected), then flag a
// redraw.  The produced GfxLightRegionHull array (d_lightRegionHulls) is consumed by
// the parked per-pixel light draw (LightPreview_DrawLight, #26 layer C).
static void Cmd_OnShowRegionsForSelected()
{
    CamWnd_RegionsForSelected();   // U-GLOBALS: was Regions_ForSelected( m_pCamWnd )
    g_nUpdateBits |= 1u;
}

// 0x42BFD0  CMainFrame::OnSetAsActiveLayer (cmd 33955) â€” the Layers "Set as active layer"
// command.  The binary handler is a single `retn` (empty stub); ported verbatim as a no-op.
// Wired so 33955 no longer WRONGLY fires OnShowRegionsForSelected (that is now on 36125).
static void Cmd_OnSetAsActiveLayer()
{
}

// SetEntityCheck (0x42B1F0) â€” re-check the main-menu item matching the active show-state.
// The exact m_nEntityShowState values were read from the IDB handler immediates (default
// 0x10010 = Skinned).  No-op for items absent from the port's menu bar.
void Radiant_SetEntityCheck()
{
    HMENU menu = Radiant_FrameMenu();   // U-CMD-1: raw HMENU == the binary's CMenu pair
    if ( !menu )
        return;
    int s = g_PrefsDlg->m_nEntityShowState;
    ::CheckMenuItem( menu, 32909, ( s == 0x1000 )  ? MF_CHECKED : MF_UNCHECKED );  // Bounding box
    ::CheckMenuItem( menu, 32916, ( s == 0x10001 ) ? MF_CHECKED : MF_UNCHECKED );  // Wireframe
    ::CheckMenuItem( menu, 32911, ( s == 0x101 )   ? MF_CHECKED : MF_UNCHECKED );  // Selected Wireframe
    ::CheckMenuItem( menu, 32912, ( s == 0x110 )   ? MF_CHECKED : MF_UNCHECKED );  // Selected Skinned
    ::CheckMenuItem( menu, 32913, ( s == 0x10010 ) ? MF_CHECKED : MF_UNCHECKED );  // Skinned
    ::CheckMenuItem( menu, 32914, ( s == 0x11010 ) ? MF_CHECKED : MF_UNCHECKED );  // Skinned and Boxed
}

// The 6 entity-display-mode setters (0x42B320..0x42B3E0): set the show-state bits, re-check
// the menu, persist, repaint.  Bit layout (from the IDB immediates): WIREFRAME=0x1,
// SKIN_MODEL=0x10, SELECTED_ONLY=0x100, BOXED=0x1000, SKINNED=0x10000.
static void Cmd_OnViewEntitiesasBoundingbox()
{ g_PrefsDlg->m_nEntityShowState = 0x1000;  Radiant_SetEntityCheck(); Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; }
static void Cmd_OnViewEntitiesasWireframe()
{ g_PrefsDlg->m_nEntityShowState = 0x10001; Radiant_SetEntityCheck(); Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; }
static void Cmd_OnViewEntitiesasSelectedwireframe()
{ g_PrefsDlg->m_nEntityShowState = 0x101;   Radiant_SetEntityCheck(); Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; }
static void Cmd_OnViewEntitiesasSelectedskinned()
{ g_PrefsDlg->m_nEntityShowState = 0x110;   Radiant_SetEntityCheck(); Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; }
static void Cmd_OnViewEntitiesasSkinned()
{ g_PrefsDlg->m_nEntityShowState = 0x10010; Radiant_SetEntityCheck(); Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; }
static void Cmd_OnViewEntitiesasSkinnedandboxed()
{ g_PrefsDlg->m_nEntityShowState = 0x11010; Radiant_SetEntityCheck(); Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; }


// â”€â”€ Colors menu (DoColor is in win_dlg.cpp, IDB 0x499350) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Each thunk is DoColor(paletteIndex) then g_nUpdateBits = -1 (Sys_UpdateWindows).  DoColor
// already sets g_nUpdateBits = -1 on OK; the trailing assignment (kept verbatim from the
// binary) also forces a full repaint on cancel.  Palette indices decoded from each EA body.
static void Cmd_OnTextureBackground()         { DoColor(0);  g_nUpdateBits = -1; }  // 0x424E40
static void Cmd_OnColorsXyBackground()        { DoColor(1);  g_nUpdateBits = -1; }  // 0x424EC0
static void Cmd_OnColorsMinor()               { DoColor(2);  g_nUpdateBits = -1; }  // 0x424EA0
static void Cmd_OnColorsMajor()               { DoColor(3);  g_nUpdateBits = -1; }  // 0x424E80
static void Cmd_OnColorsCameraBack()          { DoColor(4);  g_nUpdateBits = -1; }  // 0x424E60
static void Cmd_OnColorsGridblock()           { DoColor(7);  g_nUpdateBits = -1; }  // 0x427320
static void Cmd_OnColorsGridText()            { DoColor(8);  g_nUpdateBits = -1; }  // 0x4272C0
static void Cmd_OnColorsBrush()               { DoColor(9);  g_nUpdateBits = -1; }  // 0x427240
static void Cmd_OnColorsSelectedbrush()       { DoColor(10); g_nUpdateBits = -1; }  // 0x4272E0
static void Cmd_OnColorsSelectedbrushCamera() { DoColor(11); g_nUpdateBits = -1; }  // 0x427300
static void Cmd_OnColorsClipper()             { DoColor(12); g_nUpdateBits = -1; }  // 0x427280
static void Cmd_OnColorsViewname()            { DoColor(13); g_nUpdateBits = -1; }  // 0x427340
static void Cmd_OnColorsDetailBrush()         { DoColor(14); g_nUpdateBits = -1; }  // 0x427360
static void Cmd_OnColorsToggleDrawSurfs()     { DoColor(15); g_nUpdateBits = -1; }  // 0x427380
static void Cmd_OnColorsSelfaceCamera()       { DoColor(16); g_nUpdateBits = -1; }  // 0x4273E0
static void Cmd_OnColorsFuncGroup()           { DoColor(17); g_nUpdateBits = -1; }  // 0x427400
static void Cmd_OnColorsFuncCullGroup()       { DoColor(18); g_nUpdateBits = -1; }  // 0x427420
static void Cmd_OnColorsWeaponclip()          { DoColor(19); g_nUpdateBits = -1; }  // 0x4273A0
static void Cmd_OnColorsSizeInfo()            { DoColor(20); g_nUpdateBits = -1; }  // 0x427440
static void Cmd_OnColorsModel()               { DoColor(21); g_nUpdateBits = -1; }  // 0x427460
static void Cmd_OnColorsUnknown208()          { DoColor(22); g_nUpdateBits = -1; }  // 0x4273C0
static void Cmd_OnColorsWireframe()           { DoColor(23); g_nUpdateBits = -1; }  // 0x4272A0
static void Cmd_OnColorsFrozenLayers()        { DoColor(24); g_nUpdateBits = -1; }  // 0x427260

// â”€â”€ Select Entity Color (33036 / K accel; IDB 0x424C10) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Seed a scratch palette slot from the edited entity's "_color" key (slot 6 if a LIGHT is
// selected, else slot 5), pop DoColor on that slot, and â€” while the entity inspector is
// shown â€” write the picked "r g b" back to the entity via the key/value fields + AddProp().
extern entity_s_def *edit_entity;                                    // win_ent.cpp (0x240A108)
extern int           inspector_mode;                                 // win_ent.cpp (0x240A110)
extern void          AddProp();                                      // win_ent.cpp (0x497490)
extern void          Win_SetEntityKeyValueFields( const char *key, const char *value ); // win_ent.cpp
static void Cmd_OnMiscSelectentitycolor()
{
    if ( !edit_entity )
        return;

    // A LIGHT selected (eclass->classtype & 1) uses slot 6 (normalized), else slot 5.
    int slot = 5;
    for ( selbrush_t *sb = selected_brushes.next; sb != &selected_brushes; sb = sb->next )
    {
        entity_s_def *def = (entity_s_def *)sb->owner->def;
        if ( ( def->eclass->classtype & 1 ) != 0 )
        {
            Sys_Printf( "Light selected, normalizing _color value.\n" );
            slot = 6;
            break;
        }
    }

    // Pre-seed colors[slot] from the entity's "_color" key (if present and 3-float).
    const char *colorVal = "";
    for ( epair_t *ep = edit_entity->epairs; ep; ep = ep->next )
    {
        if ( !_stricmp( ep->key, "_color" ) )
        {
            colorVal = ep->value;
            break;
        }
    }
    float cr, cg, cb;
    if ( colorVal[0] && sscanf( colorVal, "%f %f %f", &cr, &cg, &cb ) == 3 )
    {
        g_qeglobals.d_savedinfo.colors[slot][0] = cr;
        g_qeglobals.d_savedinfo.colors[slot][1] = cg;
        g_qeglobals.d_savedinfo.colors[slot][2] = cb;
        g_qeglobals.d_savedinfo.colors[slot][3] = 1.0f;
    }

    if ( inspector_mode == INSPECTOR_ENTITY && DoColor( slot ) )
    {
        char rgb[108];
        sprintf( rgb, "%f %f %f",
                 g_qeglobals.d_savedinfo.colors[slot][0],
                 g_qeglobals.d_savedinfo.colors[slot][1],
                 g_qeglobals.d_savedinfo.colors[slot][2] );
        Win_SetEntityKeyValueFields( "_color", rgb );
        AddProp();
    }
    g_nUpdateBits = -1;
}

// â”€â”€ Colorsâ†’Themes â€” whole-palette presets.  Every colors[i][c] transcribed verbatim from
// the binary (float constant-by-constant; the decompiler's shuffled store order is
// irrelevant â€” no slot is written twice within a theme).  Each ends g_nUpdateBits = -1. â”€â”€

// 0x427480 â€” "QE4 Original"
static void Cmd_OnThemeQ4()
{
    vec4_t *c = g_qeglobals.d_savedinfo.colors;
    c[0][0]=0.25f;  c[0][1]=0.25f;  c[0][2]=0.25f;  c[0][3]=1.0f;
    c[1][0]=1.0f;   c[1][1]=1.0f;   c[1][2]=1.0f;   c[1][3]=1.0f;
    c[2][0]=0.75f;  c[2][1]=0.75f;  c[2][2]=0.75f;  c[2][3]=1.0f;
    c[3][0]=0.5f;   c[3][1]=0.5f;   c[3][2]=0.5f;   c[3][3]=1.0f;
    c[4][0]=0.25f;  c[4][1]=0.25f;  c[4][2]=0.25f;  c[4][3]=1.0f;
    c[7][0]=0.0f;   c[7][1]=0.0f;   c[7][2]=1.0f;   c[7][3]=1.0f;
    c[8][0]=0.0f;   c[8][1]=0.0f;   c[8][2]=0.0f;   c[8][3]=1.0f;
    c[9][0]=0.0f;   c[9][1]=0.0f;   c[9][2]=0.0f;   c[9][3]=1.0f;
    c[10][0]=1.0f;  c[10][1]=0.0f;  c[10][2]=0.0f;  c[10][3]=1.0f;
    c[11][0]=1.0f;  c[11][1]=0.25f; c[11][2]=0.25f; c[11][3]=0.25f;
    c[12][0]=0.0f;  c[12][1]=0.0f;  c[12][2]=1.0f;  c[12][3]=1.0f;
    c[13][0]=0.5f;  c[13][1]=0.0f;  c[13][2]=0.75f; c[13][3]=1.0f;
    c[14][0]=0.0f;  c[14][1]=0.60000002f; c[14][2]=0.0f; c[14][3]=1.0f;
    c[15][0]=0.1f;  c[15][1]=0.40000001f; c[15][2]=1.0f; c[15][3]=1.0f;
    c[16][0]=1.0f;  c[16][1]=0.25f; c[16][2]=0.25f; c[16][3]=0.25f;
    c[17][0]=0.75f; c[17][1]=0.75f; c[17][2]=0.75f; c[17][3]=1.0f;
    c[18][0]=0.75f; c[18][1]=0.75f; c[18][2]=0.75f; c[18][3]=1.0f;
    c[19][0]=0.5f;  c[19][1]=0.60000002f; c[19][2]=0.0f; c[19][3]=1.0f;
    c[20][0]=0.64999998f; c[20][1]=0.0f; c[20][2]=0.0f; c[20][3]=1.0f;
    c[21][0]=0.85000002f; c[21][1]=0.0f; c[21][2]=0.85000002f; c[21][3]=1.0f;
    c[22][0]=0.80000001f; c[22][1]=0.60000002f; c[22][2]=0.0f; c[22][3]=1.0f;
    c[23][0]=0.2f;  c[23][1]=0.17f; c[23][2]=0.14f; c[23][3]=1.0f;
    c[24][0]=0.75f; c[24][1]=0.0f;  c[24][2]=0.0f;  c[24][3]=1.0f;
    c[25][0]=0.5f;  c[25][1]=0.0f;  c[25][2]=0.0f;  c[25][3]=1.0f;
    c[26][0]=1.0f;  c[26][1]=1.0f;  c[26][2]=1.0f;  c[26][3]=1.0f;
    g_nUpdateBits = -1;
}

// 0x427760 â€” "Q3Radiant Original"
static void Cmd_OnThemeQ3()
{
    vec4_t *c = g_qeglobals.d_savedinfo.colors;
    c[0][0]=0.25f;  c[0][1]=0.25f;  c[0][2]=0.25f;  c[0][3]=1.0f;
    c[1][0]=1.0f;   c[1][1]=1.0f;   c[1][2]=1.0f;   c[1][3]=1.0f;
    c[2][0]=1.0f;   c[2][1]=1.0f;   c[2][2]=1.0f;   c[2][3]=1.0f;
    c[3][0]=0.5f;   c[3][1]=0.5f;   c[3][2]=0.5f;   c[3][3]=1.0f;
    c[4][0]=0.25f;  c[4][1]=0.25f;  c[4][2]=0.25f;  c[4][3]=1.0f;
    c[7][0]=0.0f;   c[7][1]=0.0f;   c[7][2]=1.0f;   c[7][3]=1.0f;
    c[8][0]=0.0f;   c[8][1]=0.0f;   c[8][2]=0.0f;   c[8][3]=1.0f;
    c[9][0]=0.0f;   c[9][1]=0.0f;   c[9][2]=0.0f;   c[9][3]=1.0f;
    c[10][0]=1.0f;  c[10][1]=0.0f;  c[10][2]=0.0f;  c[10][3]=1.0f;
    c[11][0]=1.0f;  c[11][1]=0.25f; c[11][2]=0.25f; c[11][3]=0.25f;
    c[12][0]=0.0f;  c[12][1]=0.0f;  c[12][2]=1.0f;  c[12][3]=1.0f;
    c[13][0]=0.5f;  c[13][1]=0.0f;  c[13][2]=0.75f; c[13][3]=1.0f;
    c[14][0]=0.0f;  c[14][1]=0.60000002f; c[14][2]=0.0f; c[14][3]=1.0f;
    c[15][0]=0.40000001f; c[15][1]=0.60000002f; c[15][2]=1.0f; c[15][3]=1.0f;
    c[16][0]=1.0f;  c[16][1]=0.25f; c[16][2]=0.25f; c[16][3]=0.25f;
    c[17][0]=0.75f; c[17][1]=0.75f; c[17][2]=0.75f; c[17][3]=1.0f;
    c[18][0]=0.75f; c[18][1]=0.75f; c[18][2]=0.75f; c[18][3]=1.0f;
    c[19][0]=0.5f;  c[19][1]=0.60000002f; c[19][2]=0.0f; c[19][3]=1.0f;
    c[20][0]=0.64999998f; c[20][1]=0.0f; c[20][2]=0.0f; c[20][3]=1.0f;
    c[21][0]=0.85000002f; c[21][1]=0.0f; c[21][2]=0.85000002f; c[21][3]=1.0f;
    c[22][0]=0.80000001f; c[22][1]=0.60000002f; c[22][2]=0.0f; c[22][3]=1.0f;
    c[23][0]=0.2f;  c[23][1]=0.17f; c[23][2]=0.14f; c[23][3]=1.0f;
    c[24][0]=0.75f; c[24][1]=0.0f;  c[24][2]=0.0f;  c[24][3]=1.0f;
    c[25][0]=0.5f;  c[25][1]=0.0f;  c[25][2]=0.0f;  c[25][3]=1.0f;
    c[26][0]=1.0f;  c[26][1]=1.0f;  c[26][2]=1.0f;  c[26][3]=1.0f;
    g_nUpdateBits = -1;
}

// 0x427A40 â€” "Black and Green"
static void Cmd_OnThemeBlackGreen()
{
    vec4_t *c = g_qeglobals.d_savedinfo.colors;
    c[0][0]=0.25f;  c[0][1]=0.25f;  c[0][2]=0.25f;  c[0][3]=1.0f;
    c[1][0]=0.0f;   c[1][1]=0.0f;   c[1][2]=0.0f;   c[1][3]=1.0f;
    c[2][0]=0.0f;   c[2][1]=0.0f;   c[2][2]=0.0f;   c[2][3]=1.0f;
    c[3][0]=0.30000001f; c[3][1]=0.5f; c[3][2]=0.5f; c[3][3]=1.0f;
    c[4][0]=0.25f;  c[4][1]=0.25f;  c[4][2]=0.25f;  c[4][3]=1.0f;
    c[7][0]=0.0f;   c[7][1]=0.0f;   c[7][2]=1.0f;   c[7][3]=1.0f;
    c[8][0]=1.0f;   c[8][1]=1.0f;   c[8][2]=1.0f;   c[8][3]=1.0f;
    c[9][0]=1.0f;   c[9][1]=1.0f;   c[9][2]=1.0f;   c[9][3]=1.0f;
    c[10][0]=1.0f;  c[10][1]=0.0f;  c[10][2]=0.0f;  c[10][3]=1.0f;
    c[11][0]=1.0f;  c[11][1]=0.25f; c[11][2]=0.25f; c[11][3]=0.25f;
    c[12][0]=0.0f;  c[12][1]=0.0f;  c[12][2]=1.0f;  c[12][3]=1.0f;
    c[13][0]=0.69999999f; c[13][1]=0.69999999f; c[13][2]=0.69999999f; c[13][3]=1.0f;
    c[14][0]=0.0f;  c[14][1]=0.60000002f; c[14][2]=0.0f; c[14][3]=1.0f;
    c[15][0]=0.40000001f; c[15][1]=0.60000002f; c[15][2]=1.0f; c[15][3]=1.0f;
    c[16][0]=1.0f;  c[16][1]=0.25f; c[16][2]=0.25f; c[16][3]=0.25f;
    c[17][0]=0.75f; c[17][1]=0.75f; c[17][2]=0.75f; c[17][3]=1.0f;
    c[18][0]=0.75f; c[18][1]=0.75f; c[18][2]=0.75f; c[18][3]=1.0f;
    c[19][0]=0.5f;  c[19][1]=0.60000002f; c[19][2]=0.0f; c[19][3]=1.0f;
    c[20][0]=0.64999998f; c[20][1]=0.0f; c[20][2]=0.0f; c[20][3]=1.0f;
    c[21][0]=0.85000002f; c[21][1]=0.0f; c[21][2]=0.85000002f; c[21][3]=1.0f;
    c[22][0]=0.80000001f; c[22][1]=0.60000002f; c[22][2]=0.0f; c[22][3]=1.0f;
    c[23][0]=0.2f;  c[23][1]=0.17f; c[23][2]=0.14f; c[23][3]=1.0f;
    c[24][0]=0.75f; c[24][1]=0.0f;  c[24][2]=0.0f;  c[24][3]=1.0f;
    c[25][0]=0.25f; c[25][1]=0.0f;  c[25][2]=0.0f;  c[25][3]=1.0f;
    c[26][0]=0.75f; c[26][1]=0.75f; c[26][2]=0.75f; c[26][3]=1.0f;
    g_nUpdateBits = -1;
}

// 0x427D30 â€” "Inverted"
static void Cmd_OnThemeInverted()
{
    vec4_t *c = g_qeglobals.d_savedinfo.colors;
    c[0][0]=0.25f;  c[0][1]=0.25f;  c[0][2]=0.25f;  c[0][3]=1.0f;
    c[1][0]=0.0f;   c[1][1]=0.0f;   c[1][2]=0.0f;   c[1][3]=1.0f;
    c[2][0]=0.0f;   c[2][1]=0.0f;   c[2][2]=0.25f;  c[2][3]=1.0f;
    c[3][0]=0.0f;   c[3][1]=0.0f;   c[3][2]=0.5f;   c[3][3]=1.0f;
    c[4][0]=0.25f;  c[4][1]=0.25f;  c[4][2]=0.25f;  c[4][3]=1.0f;
    c[7][0]=0.0f;   c[7][1]=0.0f;   c[7][2]=1.0f;   c[7][3]=1.0f;
    c[8][0]=0.2f;   c[8][1]=0.69999999f; c[8][2]=0.55000001f; c[8][3]=1.0f;
    c[9][0]=0.5f;   c[9][1]=0.5f;   c[9][2]=0.5f;   c[9][3]=1.0f;
    c[10][0]=1.0f;  c[10][1]=0.0f;  c[10][2]=0.0f;  c[10][3]=1.0f;
    c[11][0]=1.0f;  c[11][1]=0.25f; c[11][2]=0.25f; c[11][3]=0.25f;
    c[12][0]=0.0f;  c[12][1]=0.0f;  c[12][2]=1.0f;  c[12][3]=1.0f;
    c[13][0]=0.69999999f; c[13][1]=0.69999999f; c[13][2]=0.0f; c[13][3]=1.0f;
    c[14][0]=0.0f;  c[14][1]=0.80000001f; c[14][2]=0.0f; c[14][3]=1.0f;
    c[15][0]=0.40000001f; c[15][1]=0.60000002f; c[15][2]=1.0f; c[15][3]=1.0f;
    c[16][0]=1.0f;  c[16][1]=0.25f; c[16][2]=0.25f; c[16][3]=0.25f;
    c[17][0]=0.75f; c[17][1]=0.75f; c[17][2]=0.75f; c[17][3]=1.0f;
    c[18][0]=0.75f; c[18][1]=0.75f; c[18][2]=0.75f; c[18][3]=1.0f;
    c[19][0]=0.5f;  c[19][1]=0.60000002f; c[19][2]=0.0f; c[19][3]=1.0f;
    c[20][0]=0.64999998f; c[20][1]=0.0f; c[20][2]=0.0f; c[20][3]=1.0f;
    c[21][0]=0.85000002f; c[21][1]=0.0f; c[21][2]=0.85000002f; c[21][3]=1.0f;
    c[22][0]=0.80000001f; c[22][1]=0.60000002f; c[22][2]=0.0f; c[22][3]=1.0f;
    c[23][0]=0.2f;  c[23][1]=0.17f; c[23][2]=0.14f; c[23][3]=1.0f;
    c[24][0]=0.75f; c[24][1]=0.0f;  c[24][2]=0.0f;  c[24][3]=1.0f;
    c[25][0]=0.25f; c[25][1]=0.0f;  c[25][2]=0.0f;  c[25][3]=1.0f;
    c[26][0]=0.2f;  c[26][1]=0.69999999f; c[26][2]=0.55000001f; c[26][3]=1.0f;
    g_nUpdateBits = -1;
}

// 0x428040 â€” "Gray"
static void Cmd_OnThemeGrey()
{
    vec4_t *c = g_qeglobals.d_savedinfo.colors;
    c[0][0]=0.63f;  c[0][1]=0.63f;  c[0][2]=0.63f;  c[0][3]=1.0f;
    c[1][0]=0.63f;  c[1][1]=0.63f;  c[1][2]=0.63f;  c[1][3]=1.0f;
    c[2][0]=0.56999999f; c[2][1]=0.56999999f; c[2][2]=0.56999999f; c[2][3]=1.0f;
    c[3][0]=0.5f;   c[3][1]=0.5f;   c[3][2]=0.5f;   c[3][3]=1.0f;
    c[4][0]=0.63f;  c[4][1]=0.63f;  c[4][2]=0.63f;  c[4][3]=1.0f;
    c[7][0]=0.0f;   c[7][1]=0.0f;   c[7][2]=1.0f;   c[7][3]=1.0f;
    c[8][0]=0.0f;   c[8][1]=0.27000001f; c[8][2]=0.1f; c[8][3]=1.0f;
    c[9][0]=0.0f;   c[9][1]=0.02f;  c[9][2]=0.38f;  c[9][3]=1.0f;
    c[10][0]=0.25999999f; c[10][1]=1.0f; c[10][2]=0.63999999f; c[10][3]=1.0f;
    c[11][0]=0.25999999f; c[11][1]=1.0f; c[11][2]=0.63999999f; c[11][3]=0.25f;
    c[12][0]=0.0f;  c[12][1]=0.0f;  c[12][2]=1.0f;  c[12][3]=1.0f;
    c[13][0]=0.0f;  c[13][1]=0.27000001f; c[13][2]=0.1f; c[13][3]=1.0f;
    c[14][0]=0.66000003f; c[14][1]=0.67000002f; c[14][2]=1.0f; c[14][3]=1.0f;
    c[15][0]=0.1f;  c[15][1]=0.40000001f; c[15][2]=1.0f; c[15][3]=1.0f;
    c[16][0]=0.25999999f; c[16][1]=1.0f; c[16][2]=0.63999999f; c[16][3]=0.25f;
    c[17][0]=0.75f; c[17][1]=0.75f; c[17][2]=0.75f; c[17][3]=1.0f;
    c[18][0]=0.0f;  c[18][1]=0.5f;  c[18][2]=0.5f;  c[18][3]=1.0f;
    c[19][0]=0.79000002f; c[19][1]=0.80000001f; c[19][2]=0.1f; c[19][3]=1.0f;
    c[20][0]=0.0f;  c[20][1]=0.27000001f; c[20][2]=0.1f; c[20][3]=1.0f;
    c[21][0]=0.88f; c[21][1]=0.88999999f; c[21][2]=1.0f; c[21][3]=1.0f;
    c[22][0]=0.80000001f; c[22][1]=0.60000002f; c[22][2]=0.0f; c[22][3]=1.0f;
    c[23][0]=0.2f;  c[23][1]=0.17f; c[23][2]=0.14f; c[23][3]=1.0f;
    c[24][0]=0.75f; c[24][1]=0.0f;  c[24][2]=0.0f;  c[24][3]=1.0f;
    c[25][0]=0.0f;  c[25][1]=0.27000001f; c[25][2]=0.1f; c[25][3]=1.0f;
    c[26][0]=0.63f; c[26][1]=0.63f; c[26][2]=0.63f; c[26][3]=1.0f;
    g_nUpdateBits = -1;
}

// â”€â”€ CMainFrame::OnDynamicLighting (0x429960) â€” "Dynamic Lighting" (menu 32854/0x8056) â”€â”€â”€â”€â”€â”€â”€â”€â”€
// Verbatim: allocate + Cam_Init a fresh CCamWnd (the port's ctor IS CCamWnd::Cam_Init
// 0x402c40) and Create it as a FLOATING top-level popup - class "QCamera", empty title,
// WS_OVERLAPPEDWINDOW, 200x200 at (100,100), parent = the desktop, child id 12345.
// The binary's feature is DEAD: its CCamWnd::OnCreate hijacks g_qeglobals.d_hwndCamera, but
// the popup's hwnd is never registered with the renderer and all 5 R_MAX_WINDOWS slots are
// taken at startup, so the popup renders nothing.  KISAK: the port omits the d_hwndCamera
// hijack (that would regress the real camera); CCamWnd::OnPaint no-ops for the unregistered
// hwnd, matching the "renders nothing" outcome.  See RADIANT_MISSING_FUNCTIONS.md.
//
// U-CMD-1 â€” DELIBERATE DIVERGENCE, the only behaviour change this unit makes to the MFC build:
// the second-CCamWnd construction is REMOVED and 32854 is an explicit no-op.
// Reason (the U-GUARD HAZARD block at camwnd.cpp ~85): U-VP-CAM moved every CCamWnd member into
// the file-scope camwndState_t singleton, so a SECOND CCamWnd is no longer a second camera â€”
// its WM_CREATE / WM_SIZE / mouse input write THE REAL camera's state (size, button state,
// cursor anchors).  The feature was already dead in the binary (the popup's hwnd is never
// registered with the renderer and all 5 R_MAX_WINDOWS slots are taken at startup, so it drew
// nothing), and the port had already dropped its d_hwndCamera hijack â€” so nothing observable is
// lost, while the singleton corruption it would now cause is real.  See
// RADIANT_MISSING_FUNCTIONS.md; do not "restore" this without first giving the popup its own
// state block.
static void Cmd_OnDynamicLighting()
{
}

// Misc->Maya Export (33186 -> ExportToMaya 0x491b20).  The binary first pops the
// ExportToMayaProc options dialog (0x496080) then calls ExportToMaya with those flags.
// KISAK: radiant.rc has no IDD_MAYA template, so the options dialog is not built; the export
// runs with that dialog's DEFAULTS (Merge ON, UVs OFF, Quads ON, units=Inch -> scale 2.54,
// name "radiantImport.mel") into "<exeDir>\Maya\".  The emit pipeline is in mayaexport.cpp.
extern "C" void ExportToMaya( const char *dir, const char *outName,
                              char emitUVs, char groupAsBrush, char polyList, float scale );
static void Cmd_OnMiscMayaExport()                                          // 0x423xxx â†’ 0x491b20
{
    // Output dir = the editor exe's folder (the binary uses g_strAppPath; same intent).
    char exePath[MAX_PATH];
    GetModuleFileNameA( nullptr, exePath, sizeof( exePath ) );
    char *slash = strrchr( exePath, '\\' );
    if ( slash ) *slash = 0;                       // strip the exe name â†’ directory
    char mayaDir[MAX_PATH];
    _snprintf( mayaDir, sizeof( mayaDir ), "%s\\Maya", exePath );
    CreateDirectoryA( mayaDir, nullptr );           // ensure "<exeDir>\Maya\" exists

    ExportToMaya( exePath, "radiantImport.mel",
                  /*emitUVs*/0, /*groupAsBrush*/1, /*polyList*/1, /*scale=inch*/2.54f );
}

// â”€â”€ Helpâ†’About + Fileâ†’Error file + Texturesâ†’Render Method â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
extern void Pointfile_Errorfile_Public();      // errorfile.cpp (0x4100B0 wrapper)
extern void Pointfile_Clear();                 // points.cpp (0x410600)
extern void Pointfile_ResetPoints();           // points.cpp (s_num_points = 0)
extern int  s_errLogCount; // points.cpp (== s_errLogCount @0x1814CE8)
extern void Material_SetMode( int iMode );     // texwnd.cpp (0x45B910)
extern void Texture_SetMode( int iTexMenu );   // texwnd.cpp (0x45A520)


// 0x423B40 â€” toggle the error-log display: if errors are loaded, clear them; else load
// the current map's .errlog (Pointfile_Errorfile parses + sorts + navigates to the first).
// The leading s_num_points = 0 clears any displayed leak path first (the binary does this).
static void Cmd_OnErrorFile()
{
    Pointfile_ResetPoints();
    if ( s_errLogCount )
        Pointfile_Clear();
    else
        Pointfile_Errorfile_Public();
}

// â”€â”€ Pointfile (leak trace) menu handlers â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
extern FILE *Pointfile_Check();        // points.cpp (0x48ACB0) â€” load currentmap's .lin
extern int   Pointfile_GetNumPoints(); // points.cpp (s_num_points)
extern void  Pointfile_Next();         // points.cpp (0x48AA60)
extern void  Pointfile_Prev();         // points.cpp (0x48AB90)
extern void  Errorfile_NextError();    // errorfile.cpp (0x410670)
extern void  Errorfile_PrevError();    // errorfile.cpp (0x410710)

// 0x423B20 â€” Fileâ†’Pointfile toggle: clear the error log, then toggle the .lin leak path
// (loaded â†’ hide; hidden â†’ load currentmap's .lin via Pointfile_Check).
static void Cmd_OnPointfileOpen()
{
    Pointfile_Clear();
    if ( Pointfile_GetNumPoints() )
        Pointfile_ResetPoints();
    else
        Pointfile_Check();
}

// 0x424BC0 â€” Miscâ†’Next leak spot: step forward through the pointfile if one is loaded,
// otherwise through the error log.
static void Cmd_OnMiscNextleakspot()
{
    if ( Pointfile_GetNumPoints() )
        Pointfile_Next();
    else if ( s_errLogCount )
        Errorfile_NextError();
}

// 0x424BE0 â€” Miscâ†’Previous leak spot: symmetric to OnMiscNextleakspot.
static void Cmd_OnMiscPreviousleakspot()
{
    if ( Pointfile_GetNumPoints() )
        Pointfile_Prev();
    else if ( s_errLogCount )
        Errorfile_PrevError();
}

// Texturesâ†’Render Method radio group â†’ Material_SetMode(0/1/2).
static void Cmd_OnRenderMethodMaterial()  { Material_SetMode( 0 ); }
static void Cmd_OnRenderMethodLightmap()  { Material_SetMode( 1 ); }
static void Cmd_OnRenderMethodSmoothing() { Material_SetMode( 2 ); }

// â”€â”€ LAYERED MATERIALS â€” the authoring tool palette (layeredmaterialwnd.cpp) â”€â”€â”€â”€â”€â”€
// OnToggleLayeredMaterials (0x42BFE0) show/hides the frame (body identical to
// LayeredMaterialWnd_ToggleVisibility 0x4176B0); OnSaveLayeredMaterials (0x42C020) flushes the
// library to disk (CRC-gated).  The real window is NOT auto-created at startup, so
// lyrMtlWndGlob.hwnd may be NULL - ShowWindow(NULL,...) is a harmless no-op.
extern "C" int LayeredMaterialWnd_UntoggleLiveAdd();  // layeredmaterialwnd.cpp (sub_417440)
extern char LayeredMaterials_Save();                  // layeredmaterials.cpp (0x416F40)

static void Cmd_OnToggleLayeredMaterials()
{
    if ( !::IsWindowVisible( lyrMtlWndGlob.hwnd ) )
    {
        ::ShowWindow( lyrMtlWndGlob.hwnd, SW_SHOW );
        return;
    }
    if ( (BYTE)lyrMtlWndGlob.liveAddActive )
        LayeredMaterialWnd_UntoggleLiveAdd();   // binary calls sub_417440 (un-toggle Live)
    ::ShowWindow( lyrMtlWndGlob.hwnd, SW_HIDE );
}

static void Cmd_OnSaveLayeredMaterials()
{
    LayeredMaterials_Save();
}

// â”€â”€ Brush_Print (0x47BB60) â€” debug-dump a brush's face planepts to the console â”€â”€â”€â”€â”€â”€
//   Faithful transcription incl. the binary's quirk: the loop counter (the "Face %i" label)
//   steps by 2 while the face index steps by 1 and the loop tests label < faceCount â€” so it
//   prints the first ceil(faceCount/2) faces labelled 0,2,4,...  (a harmless debug quirk).
//   The binary's face stride is 464B (CoD4 face_t); the port uses sizeof(face_t) (232B).
static void Brush_Print( brush_t *def )
{
    for ( int i = 0; 2 * i < def->faceCount; ++i )
    {
        const float *pp = &def->faces[i].planepts[0][0];
        Sys_Printf( "Face %i\n", 2 * i );
        Sys_Printf( "%g %g %g\n", pp[0], pp[1], pp[2] );
        Sys_Printf( "%g %g %g\n", pp[3], pp[4], pp[5] );
        Sys_Printf( "%g %g %g\n", pp[6], pp[7], pp[8] );
    }
}

// CMainFrame::OnSelectionPrint (0x429110, cmd 33087) â€” Brush_Print every selected brush's def.
static void Cmd_OnSelectionPrint()
{
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
        Brush_Print( i->def );
}

// â”€â”€ WXY_Print (0x463AE0) â€” print the XY viewport to the printer â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
//   PrintDlg (PD_RETURNDC) â†’ StartDoc/StartPage â†’ StretchBlt the XY window's client into the
//   printer's physical page â†’ EndPage/EndDoc, with a MessageBox on each failure step.  Verbatim
//   from the disasm (Win32 GDI printing).
static void WXY_Print()
{
    PRINTDLGA pd;
    memset( &pd, 0, sizeof( pd ) );
    pd.lStructSize = sizeof( PRINTDLGA );   // binary uses 66 (= sizeof on VC7.1)
    pd.hwndOwner   = g_qeglobals.d_hwndXY;
    pd.Flags       = PD_RETURNDC;
    pd.hInstance   = 0;
    if ( !PrintDlgA( &pd ) || !pd.hDC )
    {
        MessageBoxA( GetActiveWindow(), "Print Error!\n\nCould not PrintDlg()", "Radiant", MB_OK | MB_ICONERROR );
        return;
    }

    DOCINFOA di;
    memset( &di, 0, sizeof( di ) );
    di.cbSize      = sizeof( DOCINFOA );    // binary uses 20
    di.lpszDocName = "QE4";
    if ( StartDocA( pd.hDC, &di ) <= 0 )
    {
        MessageBoxA( GetActiveWindow(), "Print Error!\n\nCould not StartDoc()", "Radiant", MB_OK | MB_ICONERROR );
        return;
    }
    if ( StartPage( pd.hDC ) <= 0 )
    {
        MessageBoxA( GetActiveWindow(), "Print Error!\n\nCould not StartPage()", "Radiant", MB_OK | MB_ICONERROR );
        return;
    }

    RECT rc;
    GetWindowRect( g_qeglobals.d_hwndXY, &rc );
    int srcW = rc.right - rc.left;
    int srcH = rc.bottom - rc.top;
    int dstW = GetDeviceCaps( pd.hDC, PHYSICALWIDTH )  - GetDeviceCaps( pd.hDC, PHYSICALOFFSETX );
    int dstH = GetDeviceCaps( pd.hDC, PHYSICALHEIGHT ) - GetDeviceCaps( pd.hDC, PHYSICALOFFSETY );
    HDC src = GetDC( g_qeglobals.d_hwndXY );
    StretchBlt( pd.hDC, 0, 0, dstW, dstH, src, 0, 0, srcW, srcH, SRCCOPY );
    ReleaseDC( g_qeglobals.d_hwndXY, src );

    if ( EndPage( pd.hDC ) <= 0 )
    {
        MessageBoxA( GetActiveWindow(), "Print Error!\n\nCould not EndPage()", "Radiant", MB_OK | MB_ICONERROR );
        return;
    }
    if ( EndDoc( pd.hDC ) <= 0 )
        MessageBoxA( GetActiveWindow(), "Print Error!\n\nCould not EndDoc()", "Radiant", MB_OK | MB_ICONERROR );
}

// CMainFrame::OnMiscPrintxy (0x424C00, cmd 33037) â€” thunk to WXY_Print.
static void Cmd_OnMiscPrintxy()
{
    WXY_Print();
}

// â”€â”€ PATCH menu â€” Patch_BrushToMesh primitives + Patch_AdjustSelected grid edits â”€â”€
// Each handler is the binary's Undo-bracketed wrapper (OnCurvePatch* 0x42A360.. /
// OnCurveInsert*/Delete* 0x42A560..); the cores live in pmesh.cpp (data layer).
extern void Patch_BrushToMesh( char bCone, byte bBevel,
                               byte bEndcap, char bSquare );   // pmesh.cpp (0x43ACC0)
extern void Patch_AdjustSelected( char bInsert, char bColumn, char bFlag ); // pmesh.cpp (0x444550)
extern void Patch_ToggleInverted();                                         // pmesh.cpp (0x4465C0)
extern void Patch_Transpose();                                              // pmesh.cpp (0x4491D0)
extern void Patch_NaturalizeSelected( bool unk, bool cap, float x, float y );// pmesh.cpp (0x447FD0)
extern void Select_SetTexture( float *out );                                // select.cpp (0x456D70)
extern void Select_Invert();                                                // select.cpp (0x493F10)
extern void Patch_DisperseColumns();                                        // pmesh.cpp (0x4443A0)
extern void Patch_DisperseRows();                                           // pmesh.cpp (0x444200)
extern void Patch_InvertTexture( char axis );                               // pmesh.cpp (0x446680)

static void Radiant_PatchBrushToMesh( char cone, byte bevel,
                                      byte endcap, char square, const char *op )
{
    Undo_ClearRedo();
    Undo_GeneralStart( op );
    Undo_AddBrushList( &selected_brushes );
    Patch_BrushToMesh( cone, bevel, endcap, square );
    g_nUpdateBits = -1;
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}
static void Radiant_PatchAdjust( char ins, char col, char flag, const char *op )
{
    Undo_ClearRedo();
    Undo_GeneralStart( op );
    Undo_AddBrushList( &selected_brushes );
    Patch_AdjustSelected( ins, col, flag );
    g_nUpdateBits = -1;
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// Patchâ†’Primitivesâ†’Dense Cylinder (32883, 0x42AB90) / Very Dense Cylinder (32884,
// 0x42AC40).  Both are the plain cylinder followed by NESTED, individually-bracketed
// Patch_AdjustSelected row edits â€” reproduced verbatim, including the nesting (the outer
// "dense cylinder" record stays open across the inner "add/insert (2) rows" records) and
// the repeated g_nUpdateBits = -1 writes.  Dense = 1 add + 1 insert; Very Dense = 2 of each.
static void Radiant_PatchDenseRows( int pairs, const char *op )
{
    Undo_ClearRedo();
    Undo_GeneralStart( op );
    Undo_AddBrushList( &selected_brushes );
    Patch_BrushToMesh( 0, 0, 0, 0 );
    for ( int i = 0; i < pairs; ++i )
    {
        Radiant_PatchAdjust( 1, 0, 1, "add (2) rows" );      // 0x42ABC6 / 0x42AC76
        Radiant_PatchAdjust( 1, 0, 0, "insert (2) rows" );   // 0x42ABF6 / 0x42ACA6
    }
    g_nUpdateBits = -1;
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}
static void Cmd_OnCurvePatchdensetube()     { Radiant_PatchDenseRows( 1, "dense cylinder" ); }
static void Cmd_OnCurvePatchverydensetube() { Radiant_PatchDenseRows( 2, "very dense cylinder" ); }

// Patchâ†’Cycle Cap Texture (32905 / Shift+Ctrl+N, 0x42B1A0) â€” same shape as Naturalize
// but with the binary's (unk=1, cap=1) flags, i.e. it cycles the CAP texture projection.
static void Cmd_OnCurveCyclecap()
{
    float x[2];
    Select_SetTexture( x );
    Patch_NaturalizeSelected( 1, 1, x[0], x[1] );
    g_nUpdateBits = -1;
}

// Patchâ†’Insertâ†’Add Terrain Row / Column (33153 / Shift+Ctrl+A, 0x42B080) â€” if a PATCH is
// selected, insert at the selected vertex pair; otherwise fall through to the entity-chain
// arm (clone the source entity onto the pair's midpoint and re-link).  Verbatim.
extern void Patch_InsertRemoveFromVertPair();     // pmesh.cpp (0x44BB90)
extern void Select_InsertMidpointEntity();        // select.cpp (0x48EB30)
static void Cmd_OnAddTerrainRowColumn()
{
    if ( selected_brushes.next == &selected_brushes || !selected_brushes.next->patch )
        Select_InsertMidpointEntity();
    else
        Patch_InsertRemoveFromVertPair();
}

static void Cmd_OnCurvePatchtube()   { Radiant_PatchBrushToMesh( 0, 0, 0, 0, "make curve cylinder" ); }
static void Cmd_OnCurvePatchcone()   { Radiant_PatchBrushToMesh( 1, 0, 0, 0, "make curve cone" ); }
static void Cmd_OnCurvePatchendcap() { Radiant_PatchBrushToMesh( 0, 0, 1, 0, "make end cap" ); }
static void Cmd_OnCurvePatchbevel()  { Radiant_PatchBrushToMesh( 0, 1, 0, 0, "make bevel" ); }
static void Cmd_OnCurvePatchsquare() { Radiant_PatchBrushToMesh( 0, 0, 0, 1, "square cylinder" ); }
static void Cmd_OnCurveSquareBevel() { Radiant_PatchBrushToMesh( 0, 1, 0, 1, "square bevel" ); }
static void Cmd_OnCurveSquareEndcap(){ Radiant_PatchBrushToMesh( 0, 0, 1, 1, "square endcap" ); }

// Curve->Insert menu.  "Insert" FRONT-inserts (bFlag=0 = subdivide the first segment), "Add"
// BACK-inserts (bFlag=1 = subdivide the last) - they are NOT identical.  From the binary's
// message map: 32874 Insert (2) Columns -> Patch_AdjustSelected(1,1,0); 32873 Add (2) Columns
// -> (1,1,1); rows mirror at 32876/32875.  The binary's SAME-NAMED OnCurveInsertcolumn (nID
// 32868, op "insert colum", flag 1) is a DIFFERENT handler - do not match this one to it.
static void Cmd_OnCurveInsertcolumn()    { Radiant_PatchAdjust( 1, 1, 0, "insert (2) columns" ); } // 32874 front
static void Cmd_OnCurveInsertAddcolumn() { Radiant_PatchAdjust( 1, 1, 1, "add (2) columns" ); }    // 32873 back
static void Cmd_OnCurveInsertrow()       { Radiant_PatchAdjust( 1, 0, 0, "insert (2) rows" ); }    // 32876 front
static void Cmd_OnCurveInsertAddrow()    { Radiant_PatchAdjust( 1, 0, 1, "add (2) rows" ); }       // 32875 back
static void Cmd_OnCurveDeleteFirstcolumn(){ Radiant_PatchAdjust( 0, 1, 1, "delete first (2) columns" ); }
static void Cmd_OnCurveDeleteLastcolumn() { Radiant_PatchAdjust( 0, 1, 0, "delete last (2) columns" ); }
static void Cmd_OnCurveDeleteFirstrow()   { Radiant_PatchAdjust( 0, 0, 1, "delete first (2) rows" ); }
static void Cmd_OnCurveDeleteLastrow()    { Radiant_PatchAdjust( 0, 0, 0, "delete last (2) rows" ); }
// Patchâ†’Negative (32881): faithful thunk to Patch_ToggleInverted (IDB OnCurveNegative
// 0x42A7E0 is a pure thunk â€” NO Undo bracket, matching the binary).
static void Cmd_OnCurveNegative()         { Patch_ToggleInverted(); }
// Patchâ†’Matrixâ†’Transpose (32906): IDB OnCurveMatrixTranspose 0x42B1E0 = Patch_Transpose()
// then g_nUpdateBits=-1 (no Undo bracket).
static void Cmd_OnCurveMatrixTranspose()  { Patch_Transpose(); g_nUpdateBits = -1; }
// Patchâ†’Naturalize (32890): IDB OnPatchNaturalize 0x42AE10 â€” fetch the default tex-repeat
// scale (Select_SetTexture), Patch_NaturalizeSelected(unk=0,cap=0,...) lays linear S/T,
// then g_nUpdateBits=-1. (Patch_NaturalizeSelected does its own Undo bracket.)
static void Cmd_OnPatchNaturalize()       { float x[2]; Select_SetTexture( x ); Patch_NaturalizeSelected( 0, 0, x[0], x[1] ); g_nUpdateBits = -1; }
// Selectionâ†’Invert (33101 / Ctrl+I): IDB OnSelectionInvert 0x42B6F0 = Select_Invert()
// then g_nUpdateBits |= 0xB (swap active/selected lists + repaint XY/Z/camera).
static void Cmd_OnSelectionInvert()       { Select_Invert(); g_nUpdateBits |= 0xB; }
// Patchâ†’Redisperse Cols/Rows (32889/32888): IDB OnCurveRedisperse* 0x42AD80/0x42AD90 =
// Patch_Disperse{Columns,Rows}() then g_nUpdateBits = -1 (evenly redistribute control points).
static void Cmd_OnCurveRedisperseCols()   { Patch_DisperseColumns(); g_nUpdateBits = -1; }
static void Cmd_OnCurveRedisperseRows()   { Patch_DisperseRows();    g_nUpdateBits = -1; }
// Patchâ†’Negative Texture X/Y (32899/32903): IDB OnCurveNegativeTexture{X,Y} 0x42A7F0/0x42A800
// = Patch_InvertTexture(0/1) (the worker sets g_nUpdateBits itself; pure thunks, no Undo bracket).
static void Cmd_OnCurveNegativeTextureX() { Patch_InvertTexture( 0 ); }
static void Cmd_OnCurveNegativeTextureY() { Patch_InvertTexture( 1 ); }

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// PATCH/CURVE OPERATION CLUSTER + TERRAIN row/col handlers; cores in pmesh.cpp, ids from the
// binary's frame message map.
extern void        Patch_CapCurrent();                                   // pmesh.cpp 0x43AA20
extern void        Patch_Thicken( int amount, char bseam );              // pmesh.cpp 0x448700
extern void        SplitPatch();                                         // pmesh.cpp 0x44CC60
extern void        ExtrudeTerrainRow();                                  // pmesh.cpp 0x44BD40
extern void        RemoveTerrainRowCol();                                // pmesh.cpp 0x44BE10
extern brush_t    *PMESH_58( face_t *face, selbrush_t *ownerInst );      // pmesh.cpp 0x44D1F0
extern selbrush_t *PMESH_07_Width( selbrush_t *a1 );                     // pmesh.cpp 0x43B950
extern void        Patch_NaturalizeSelected( bool unk, bool cap, float x, float y );
extern void        Select_SetTexture( float *out );
extern selbrush_t  selected_brushes;
extern void        Brush_RemoveFromList( selbrush_t *b );                // brush.cpp
extern void        Brush_AddToList2( selbrush_t *b );                    // brush.cpp

// Curveâ†’Cap (32885, IDB OnCurveCap 0x42AD40): auto-cap the single selected patch.
static void Cmd_OnCurveCap()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "curve cap" );
    Undo_AddBrushList( &selected_brushes );
    Patch_CapCurrent();
    g_nUpdateBits = -1;
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// Patchâ†’Cap naturalize (35040, IDB OnPatchCap 0x42AE50): lay cap texturing on the selection.
static void Cmd_OnPatchCap()
{
    float x[2];
    Select_SetTexture( x );
    Patch_NaturalizeSelected( 1, 0, x[0], x[1] );
    g_nUpdateBits = -1;
}

// â”€â”€ CDialogThick (IDD 0xA4) â€” Curveâ†’Thicken parameters (thickness + seam) â”€â”€â”€â”€â”€
//   Hand-built like CPatchDensityDlg.  DDX (0x40C060): checkbox 1288 = "create seam"
//   (member @0x74, default 1), edit 1290 = thickness (member @0x78, default 8).
// UI-independent action behind Curveâ†’Thicken's OK â€” Patch_Thicken + full repaint.  (The
// undo bracket stays with the caller: IDB 0x42B0D0 opens it around the DIALOG, not the
// thicken â€” even a cancel opens/closes the bracket.)
void CurveThicken_Apply( int amount, bool seam )
{
    Patch_Thicken( amount, seam );
    g_nUpdateBits = -1;
}


extern void Select_Scale( float sx, float sy, float sz );   // select.cpp (0x48FDC0)
extern int  OnlyPatchesSelected();                          // engine_stubs.cpp (0x447860)

// UI-independent action behind Selectionâ†’Scale's OK â€” the two validations, then
// Select_Scale inside an undo bracket (IDB OnSelectScale 0x4283D0 body after DoModal).
// PRECEDENCE, verbatim from 0x42844D..0x428481: the negative test is X < 0 || (Y < 0 && Z < 0),
// NOT a three-way OR (`fcom X` + `jnp` -> error, then `fcom Y` + `jp` SKIPS the Z test).  An
// original quirk: a lone negative Y or Z slips through to Select_Scale.
void SelectScale_Apply( float x, float y, float z )
{
    if ( x < 0.0f || ( y < 0.0f && z < 0.0f ) )
    {
        Sys_Printf( "Cannot scale by a negative value." );
        return;
    }
    if ( ( x == 0.0f || y == 0.0f || z == 0.0f ) && !OnlyPatchesSelected() )
    {
        Sys_Printf( "Can only scale patches by zero." );
        return;
    }

    Undo_ClearRedo();
    Undo_GeneralStart( "scale" );
    Undo_AddBrushList( &selected_brushes );
    Select_Scale( x, y, z );
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
    g_nUpdateBits = -1;
}


// Selectionâ†’Clipperâ†’Split selected (32794 / Shift+Enter, IDB OnSplitSelected 0x4271D0) â€”
// commit the clip keeping BOTH halves, inside an undo bracket.  Gated on an active XY view
// exactly as the binary is.
extern void Ed_SplitClip();                                 // xywnd.cpp (CXYWnd::SplitClip 0x46DD30)

// Physicsâ†’Cylinder (36113, IDB OnMakePhysCylinder 0x4291D0) / Physicsâ†’Box (36120,
// IDB OnMakePhysBox 0x429200) â€” undo-bracketed wrappers over the brush.cpp cores.
extern void Brush_MakePhysCylinder();   // brush.cpp (0x47C310)
extern void Brush_MakePhysBox();        // brush.cpp (0x47C180)
static void Cmd_OnMakePhysCylinder()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "make physics cylinder" );
    Undo_AddBrushList( &selected_brushes );
    Brush_MakePhysCylinder();
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}
static void Cmd_OnMakePhysBox()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "make physics box" );
    Undo_AddBrushList( &selected_brushes );
    Brush_MakePhysBox();
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// The remaining one-off command bodies, each ported from its own IDB body; the ones the binary
// leaves EMPTY are reproduced as empty (with the address), never as a silent TODO.
extern void Delete_Exportables();                // entity.cpp 0x487C30
extern void Select_HideUnselected2_unused();     // select.cpp 0x48EF40
extern void Patch_BendToggle();                  // pmesh.cpp  0x4478E0
extern void Patch_BendHandleTAB();               // pmesh.cpp  0x447980
extern void Patch_InsDelHandleTAB();             // pmesh.cpp  0x447F40
extern void Patch_SnapVertToGrid();              // pmesh.cpp  0x449280
extern void Patch_RedistributeVerts();           // pmesh.cpp  0x449930
extern void DoRedistPatchPts();                  // pmesh.cpp  0x449A90
extern void DoTurnTerrainEdges();                // pmesh.cpp  0x44B260
extern void SelectTargettedEntity();             // select.cpp 0x491440
extern int  g_bPatchBendMode;                    // pmesh.cpp  0x25D5B04
extern int  g_qeglobals_redispersePatchVerts;    // engine_stubs.cpp 0x25D5A6B
extern char PMESH_10( char bAdd, int dCol, int dRow );            // pmesh.cpp  0x43CB80
extern void Select_Brush( selbrush_t *b, char some_overwrite,
                          char bStatus, char center_grid_on_selection ); // select.cpp 0x48DCC0
extern void Select_Deselect( char bResetMode );                   // select.cpp 0x48E800

// 206 â€” Miscâ†’Delete exportables (0x424E30), a pure thunk.
static void Cmd_OnDeleteExportables() { Delete_Exportables(); }

// â”€â”€ CCommandsDlg (IDD_DLG_COMMANDLIST 132) â€” Helpâ†’Command list... (32790) â”€â”€â”€â”€â”€â”€
//   CCommandsDlg overrides DoDataExchange (0x40B850: DDX_Control 1036) and OnInitDialog
//   (0x40B890).  OnInitDialog sets ONE tab stop at 96 dialog units, opens "c:/commandlist.txt"
//   (CFile modeCreate|modeWrite, no exception object - a silent no-op when C:\ is not writable)
//   and appends "<name> \t<modifiers><key>" for every command to both the listbox and the file.
// UI-independent halves of a command-list line (sub_40BBC0): key-name lookup and the
// modifier string, in the binary's order.
// key name: the g_Keys entry, else the raw character (sub_40BBC0's %c).
const char *CommandList_KeyName( const RadiantCommand &c, char keybuf[8] )
{
    for ( const RadiantKeyName &k : g_radiantKeys )
        if ( k.vk == c.vk )
            return k.name;
    keybuf[0] = (char)c.vk;
    keybuf[1] = '\0';
    return keybuf;
}

void CommandList_Mods( const RadiantCommand &c, char mods[64] )
{
    mods[0] = '\0';
    if ( c.mods & 1 ) strcat( mods, "Shift" );
    if ( c.mods & 2 ) strcat( mods, mods[0] ? " + Alt"      : "Alt" );
    if ( c.mods & 4 ) strcat( mods, mods[0] ? " + Control"  : "Control" );
    if ( c.mods & 8 ) strcat( mods, mods[0] ? " + Left Win" : "Left Win" );
    if ( mods[0] )    strcat( mods, " + " );
}

// Single shell-agnostic command entry point.  Every editor command is identified by its
// menu/accelerator id (the ids in g_radiantCommands and the ON_COMMAND maps).  This is THE
// one place the two shells diverge:
//   MFC          â†’ the frame's message map stays the authoritative id->handler table, so the
//                  id goes out as a WM_COMMAND exactly as the binary's TryHotkey tail does.
//   KISAK_NO_MFC â†’ Radiant_DispatchCommandDirect (the free-function command table at the
//                  bottom of this file) IS the table; there is no message map to post to.
// Callers (menus, accelerators, hotkeys, the ImGui shell) do not change either way.
void Radiant_ExecCommand( unsigned int cmdId )
{
    Radiant_DispatchCommandDirect( cmdId );
}


// 1085 â€” LinkSelectionToggle (Shift+O, 0x423EE0): flip the pref + persist it.
static void Cmd_OnLinkKeepSelection()
{
    g_PrefsDlg->linking_keeps_selection = ( g_PrefsDlg->linking_keeps_selection == 0 );
    Prefs_SavePrefs( g_PrefsDlg );      // idb CPrefsDlg::SavePrefs 0x44F280
}

// 32776 â€” ToggleCamera (0x423A50): flip the camera-update preview flag.  (Distinct from
// 33069 OnTogglecamera, which show/hides the camera VIEW.)
static void Cmd_ToggleCamera() { g_radiantFrameState.camPreview = !g_radiantFrameState.camPreview; }


// 32782 â€” Viewâ†’Camera update (0x426450).  Its ON_UPDATE_COMMAND_UI was already wired;
// the command itself was not, so the check mark drew but clicking did nothing.
static void Cmd_OnViewCameraupdate() { g_nUpdateBits |= W_CAMERA; }

// 32863 / 32864 â€” Patchâ†’Primitivesâ†’Inverted End Cap / Inverted Bevel.  GENUINELY EMPTY
// in the binary (0x42A500 / 0x42A4F0 are single `retn`s) â€” reproduced as empty.
static void Cmd_OnCurvePatchinvertedendcap() {}
static void Cmd_OnCurvePatchinvertedbevel()  {}

// 32867/32868/32869/32870 â€” the SINGLE-row/column patch grid edits (Ctrl+Num+/- and the
// Shift variants).  Note these are NOT the Patch menu's "(2) rows/columns" commands
// (32873..32876) â€” different ids, different undo strings, and all four pass bFlag = 1.
static void Cmd_OnCurveInsertrowSingle()    { Radiant_PatchAdjust( 1, 0, 1, "insert row" ); }     // 0x42A5B0
static void Cmd_OnCurveInsertcolumnSingle() { Radiant_PatchAdjust( 1, 1, 1, "insert colum" ); }   // 0x42A560 (idb typo kept)
static void Cmd_OnCurveDeleterowSingle()    { Radiant_PatchAdjust( 0, 0, 1, "delete row" ); }     // 0x42A650
static void Cmd_OnCurveDeletecolumnSingle() { Radiant_PatchAdjust( 0, 1, 1, "delete column" ); }  // 0x42A600


// 32925 â€” HideByClassname (Shift+Alt+Ctrl+H, 0x42B6B0), a thunk.
static void Cmd_OnHideUnselected2() { Select_HideUnselected2_unused(); }

// 32978 â€” Miscâ†’Benchmark (0x424B70).  EMPTY in the binary (the camera-spin benchmark is
// compiled out); reproduced as empty so the menu item stops being silently unroutable.
static void Cmd_OnMiscBenchmark() {}

// 33089 â€” Patch TAB (0x42A9E0).  In bend mode / redisperse mode the TAB steps that mode's
// state machine; otherwise it cycles to the NEXT brush of the selected brush's entity
// (worldspawn excluded).
static void Cmd_OnPatchTab()
{
    if ( g_bPatchBendMode )
    {
        Patch_BendHandleTAB();
        return;
    }
    if ( g_qeglobals_redispersePatchVerts )
    {
        Patch_InsDelHandleTAB();
        return;
    }

    selbrush_t *cur = selected_brushes.next;
    if ( cur == &selected_brushes )
        return;
    entity_s *owner = cur->owner;
    if ( !_stricmp( ( (entity_s *)owner->def )->eclass->name, "worldspawn" ) )
        return;

    Select_Deselect( 1 );
    selbrush_t *head = &owner->brushes;
    selbrush_t *scan = owner->brushes.ownerNext;
    if ( scan != head )
    {
        // Walk to the node AFTER cur (the binary's do/while: stop once the previous
        // node was cur, or once the list wraps).
        bool wasCur;
        do
        {
            wasCur = ( cur == scan );
            scan = scan->ownerNext;
        } while ( !wasCur && scan != head );
    }
    if ( scan == head )
        scan = scan->ownerNext;      // skip the sentinel

    Select_Brush( scan, 0, 1, 0 );
    g_nUpdateBits = -1;
}

// 33090 â€” Patch ENTER (0x42A9D0).  EMPTY in the binary.
static void Cmd_OnPatchEnter() {}

// 33091 â€” SelectSnapPointsToGrid (Ctrl+G, 0x42AE90).  With NOTHING selected this pops the
// "go to position" dialog; with a selection it snaps the patch control points to the grid
// inside an undo bracket.  (The odd pairing is the original's.)
// U-CMD-2: the WITH-a-selection half is a free function â€” it is the only half that is
// shell-agnostic, and keeping ONE body avoids the duplicate-function drift trap.  The
// no-selection half stays per-shell (MFC pops CGoToDlg; the command table toggles the
// Go-to-position panel), because the dialog IS the shell-specific part.
void Cmd_SnapPatchVertsToGrid()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "snap to grid" );
    Undo_AddBrushList( &selected_brushes );
    Patch_SnapVertToGrid();
    g_nUpdateBits = -1;
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}


// 33165..33168 â€” the four "vertex select" nudges (Ctrl+Up/Down + two unbound siblings).
// Each is a bare PMESH_10(1, dCol, dRow) â€” bAdd = 1 (EXTEND the vertex selection), unlike
// the already-wired OnSelectionDragVertices arrows which pass bAdd = 0.
static void Cmd_OnVertexSelectUp()    { PMESH_10( 1, 0, -1 ); }   // 0x426880
static void Cmd_OnVertexSelectDown()  { PMESH_10( 1, 0,  1 ); }   // 0x4268A0
static void Cmd_OnVertexSelectRight() { PMESH_10( 1, 1,  0 ); }   // 0x4268C0
static void Cmd_OnVertexSelectLeft()  { PMESH_10( 1, -1, 0 ); }   // 0x4268E0

// 33170 â€” RedisperseVertices (Shift+F, 0x42A270), a thunk.
static void Cmd_OnRedistPatchPoints() { DoRedistPatchPts(); }

// 33179 â€” AutoEdgeTurn (Alt+F2, 0x4294E0), a thunk.  (NOT the same command as the
// "Turn Terrain Edges" MENU item, which the original binds to 33142.)
static void Cmd_OnTurnTerrainEdges() { DoTurnTerrainEdges(); }

// 33213 â€” DropVertices (Shift+Alt+Ctrl+D, 0x42AE00).
static void Cmd_OnDropPatchVertices() { Patch_RedistributeVerts(); g_nUpdateBits = -1; }

// 36110 â€” SelectTargettedEntities (Ctrl+E, 0x425560), a thunk.
static void Cmd_OnSelectTargettedEntity() { SelectTargettedEntity(); }

// 57602 / 57607 / 57609 â€” Fileâ†’Close / Print / Print Preview.  All three are EMPTY in the
// binary (0x423A70 / 0x423B60 / 0x423B70) â€” Radiant does not implement them.
static void Cmd_OnFileClose() {}
static void Cmd_OnFilePrint() {}
static void Cmd_OnFilePrintPreview() {}

// Curveâ†’Split (33158, IDB OnSplitPatch 0x42B0C0): thunk.
static void Cmd_OnSplitPatch() { SplitPatch(); }

// Terrainâ†’Extrude Row/Col (33192, IDB 0x42B0A0): thunk.
static void Cmd_ExtrudeTerrainRow2() { ExtrudeTerrainRow(); }

// Terrainâ†’Remove Row/Col (33154, IDB 0x42B0B0): thunk.
static void Cmd_OnRemoveTerrainRowColumn() { RemoveTerrainRowCol(); }

// Curveâ†’Terrain (35041, IDB OnCurveToTerrain 0x429B30): convert selected patches to terrain.
static void Cmd_OnCurveToTerrain()
{
    int n = 0;
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( b->patch )
        {
            if ( !n )
            {
                Undo_ClearRedo();
                Undo_GeneralStart( "convert curve to terrain" );
                Undo_AddBrushList( &selected_brushes );
            }
            ++n;
            PMESH_07_Width( b );
        }
    }
    if ( !n )
    {
        Sys_Printf( "Curve to terrain: failed; no patches found in the selection.\n" );
        return;
    }
    Select_Delete();
    g_nUpdateBits = -1;
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
    Sys_Printf( va( "Curve to terrain: converted %i %s.\n", n, ( n <= 1 ) ? "curve" : "curves" ) );
}

// Selectionâ†’Connect (33021, IDB OnSelectionConnect 0x425510): in curve-point mode connect
//   patch vertices (ConnectVertices â€” patch-verts, still deferred); otherwise WeldMesh (now
//   ported), and if no weld happened, ConnectEntities_R.  Now wire-able because WeldMesh landed.
extern char WeldMesh();                                   // pmesh.cpp 0x44C8C0
extern void ConnectEntities_R();                          // select.cpp 0x48C530
extern void ConnectVertices();                            // pmesh.cpp 0x44A920
static void Cmd_OnSelectionConnect()
{
    if ( g_qeglobals.d_select_mode == sel_curvepoint )
    {
        // ConnectVertices (0x44A920) â€” patch control-vertex connect/weld (now ported).
        ConnectVertices();
        return;
    }
    if ( !WeldMesh() )
    {
        ConnectEntities_R();
        UpdateSelection( -1, 0 );
    }
}

// Faceâ†’Terrain (36102, IDB OnFaceToTerrain 0x429BE0): convert selected faces to terrain patches.
static void Cmd_OnFaceToTerrain()
{
    Undo_ClearRedo();
    Undo_GeneralStart( "convert faces to terrain" );
    Undo_AddBrushList( &selected_brushes );

    int faceCount = g_SelectedFaces.GetSize();
    // collect the new terrain brushes (the binary uses a CArray<BrushInst*>; a plain
    // bounded buffer is faithful â€” one new brush per selected face).
    selbrush_t *newBrushes[256];
    int newCount = 0;
    for ( int i = 0; i < faceCount && newCount < 256; ++i )
    {
        selbrush_t *brush = g_SelectedFaces.GetAt( i ).brush;
        face_t     *face  = &brush->def->faces[g_SelectedFaces.GetAt( i ).index];
        brush_t    *nb    = PMESH_58( face, brush );
        if ( nb )
            newBrushes[newCount++] = (selbrush_t *)nb;
    }

    g_nUpdateBits = -1;
    Select_Deselect( 1 );

    // relink the new brushes into the selection (Brush_RemoveFromList + Brush_AddToList2).
    for ( int i = 0; i < newCount; ++i )
    {
        Brush_RemoveFromList( newBrushes[i] );
        if ( newBrushes[i]->next || newBrushes[i]->prev )
            Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
        Brush_AddToList2( newBrushes[i] );
    }

    // undo id-stamp tail (identical idiom to the other cluster ops).
    if ( g_lastundo && !g_lastundo->done )
    {
        for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
        {
            i->def->ownerPrev = (entity_s *)(intptr_t)g_lastundo->id;
            entity_s *owner = (entity_s *)(intptr_t)i->def->owner;
            if ( *(int *)&owner->eclass->fixedsize )
                owner->epairEdits = g_lastundo->id;
        }
    }
    Undo_End();
}

// Legacy direct-invalidate entry, kept for the input handlers that call it - routed through
// the real Sys_UpdateWindows broadcast.
void Ed_InvalidateAllViews()
{
    extern void Sys_UpdateWindows( int bits );   // win_qe3.cpp
    Sys_UpdateWindows( W_ALL );
    if ( g_qeglobals.d_hwndMain )                 // U-RIP: same liveness gate, HWND-based
        Radiant_RoutineProcessing();              // flush now (keeps drags responsive)
}

// â”€â”€ ConfirmModified (IDB 0x49a030) â€” the unsaved-changes "Save changes?" prompt â”€â”€
// The binary's body is a single MessageBoxA with two shapes selected by the "DefaultSaveNo"
// pref (g_PrefsDlg->loose_changes):
//   loose_changes set  -> "Lose changes?"       MB_OKCANCEL         (IDOK -> discard)
//   loose_changes clear-> "Save changes first?" MB_YESNOCANCEL|ICONEXCLAMATION
//        IDYES -> save first (SaveAsDialog if untitled, else Map_SaveFile), proceed
//        IDNO  -> discard, proceed;  IDCANCEL -> abort (return false)
// ConfirmModified_Decide is the UI-INDEPENDENT decision core so the headless
// `confirmmodified` gate can drive every MessageBox outcome deterministically; ConfirmModified
// pops the box, runs the save, and delegates
// the proceed/abort verdict to the core.  Keeps the GUI and gate on one source of truth.
enum { CM_ABORT = 0, CM_PROCEED = 1, CM_SAVE_THEN_PROCEED = 2 };

// answer: for loose_changes -> MessageBox IDOK/IDCANCEL; else IDYES/IDNO/IDCANCEL.
int ConfirmModified_Decide( int answer, bool looseChanges )
{
    if ( looseChanges )
        return ( answer == IDOK ) ? CM_PROCEED : CM_ABORT;
    // "Save changes first?" â€” YES=save+proceed, NO=proceed, CANCEL/other=abort.
    if ( answer == IDYES )
        return CM_SAVE_THEN_PROCEED;
    if ( answer == IDNO )
        return CM_PROCEED;
    return CM_ABORT;
}

// U-CMD-2: the prompt is a FREE function â€” its whole body is a plain ::MessageBoxA over
// ::GetActiveWindow() plus the two save paths, so nothing in it was ever CMainFrame state.
// CMainFrame::ConfirmModified below forwards, and the raw shell (radiant_main.cpp's WM_CLOSE,
// the File flows, errorfile.cpp / qe3.cpp's NO-MFC arms) calls this same body.
bool Radiant_ConfirmModified()
{
    const bool looseChanges = ( g_PrefsDlg && g_PrefsDlg->loose_changes ) ? true : false;

    int answer;
    if ( looseChanges )
        answer = ::MessageBoxA( ::GetActiveWindow(), "Lose changes?", "Radiant", MB_OKCANCEL );
    else
        answer = ::MessageBoxA( ::GetActiveWindow(), "Save changes first?", "Radiant",
                                MB_YESNOCANCEL | MB_ICONEXCLAMATION );

    int verdict = ConfirmModified_Decide( answer, looseChanges );
    if ( verdict == CM_SAVE_THEN_PROCEED )
    {
        // Binary: strcmp(currentmap,"unnamed.map") â†’ SaveAsDialog; else Map_SaveFile(currentmap).
        // Port adaptation: the active path lives in s_currentMapPath (empty == untitled),
        // so this mirrors OnFileSave's branch exactly.
        if ( !s_currentMapPath[0] )
            return Radiant_FileSaveAs();   // SaveAsDialog(0): false if the user cancels Save-As
        Map_SaveFile( s_currentMapPath, 0, 0 );
        Radiant_FL_Log( "ConfirmModified: saved %s", s_currentMapPath );
        return true;
    }
    return ( verdict == CM_PROCEED );
}

// Radiant_OkToDiscard â€” the EXACT guard the binary places at the head of every
// destroy-the-map command (OnFileNew/OnFileOpen/OnClose/DoMru):
//   ( !HasUnsavedChangesOrInsidePrefab()
//     && CheckLayeredMaterial_Modifications(...) == lyrMtlGlob_crcToken )  ||  ConfirmModified()
// i.e. proceed silently when nothing is dirty, otherwise prompt.  The layered-material
// CRC half matches ErrorLog_01's already-shipped guard (errorfile.cpp).
extern int      modified;                 // map.cpp 0x23f179c
extern int      prefabStackLevel;         // map.cpp 0x25d5b34
extern unsigned int CheckLayeredMaterial_Modifications( uint8_t *a1, int a2, int a3 ); // layeredmaterials.cpp

static bool HasUnsavedChangesOrInsidePrefab_mf()   // mirror of errorfile.cpp's static (0x489d90)
{
    if ( modified )
        return true;
    if ( prefabStackLevel > 0 )
    {
        prefabLevel_t *p = g_prefabStack;
        for ( int n = 0; ; ++p )
        {
            if ( p->modified )
                return true;
            if ( ++n >= prefabStackLevel )
                return false;
        }
    }
    return false;
}

bool Radiant_OkToDiscard()
{
    if ( !HasUnsavedChangesOrInsidePrefab_mf()
         && CheckLayeredMaterial_Modifications( lyrMtlGlob.Layers,
                                                84 * lyrMtlGlob.entryCount, 0 ) == (unsigned)lyrMtlGlob.crcToken )
        return true;                       // nothing dirty â†’ proceed silently
    return Radiant_ConfirmModified();      // dirty â†’ prompt
}

// OnFileSaveAs that reports whether the user actually saved (vs cancelling the Save-As
// dialog) â€” ConfirmModified's IDYES-on-untitled path needs the cancel signal.
// U-CMD-2: free function (was CMainFrame::OnFileSaveAs_Confirmed).  GetSaveFileNameA is plain
// Win32, and the three frame touches become the HWND both shells register as d_hwndMain:
// ofn.hwndOwner, GetMenu()->GetSafeHmenu() â†’ ::GetMenu(), SetWindowText â†’ ::SetWindowTextA.
bool Radiant_FileSaveAs()
{
    char file[MAX_PATH];
    _snprintf( file, sizeof( file ), "%s", s_currentMapPath );
    OPENFILENAMEA ofn;
    memset( &ofn, 0, sizeof( ofn ) );
    ofn.lStructSize = sizeof( ofn );
    ofn.hwndOwner   = g_qeglobals.d_hwndMain;
    ofn.lpstrFilter = "Map files (*.map)\0*.map\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = sizeof( file );
    ofn.lpstrTitle  = "Save Map As";
    ofn.lpstrDefExt = "map";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if ( !GetSaveFileNameA( &ofn ) )
        return false;                      // user cancelled Save-As â†’ abort the parent op

    // SaveAsDialog (0x49A760) .map path: after the extension default, copy the chosen path
    // into currentmap, bump the MRU (MRU_NewItem promotes it to slot 0), and refresh the
    // File-menu recent-files list â€” verbatim from 0x49a800-0x49a84a â€” BEFORE Map_SaveFile.
    _snprintf( s_currentMapPath, sizeof( s_currentMapPath ), "%s", file );
    if ( g_qeglobals.d_lpMruMenu )
    {
        MRU_NewItem( g_qeglobals.d_lpMruMenu, file );
        MRU_InsertItem( g_qeglobals.d_lpMruMenu,
                        ::GetSubMenu( ::GetMenu( g_qeglobals.d_hwndMain ), 0 ) );
    }

    Map_SaveFile( file, 0, 0 );
    char title[MAX_PATH + 32];
    _snprintf( title, sizeof( title ), "CoD4Radiant - %s", file );
    ::SetWindowTextA( g_qeglobals.d_hwndMain, title );
    return true;
}


// CXYWnd clipboard ops.  KISAK: the binary mirrors the copied selection to the Win32 OLE
// clipboard (RegisterClipboardFormatA("RadiantClippings")) so brushes can be pasted between
// two running editors; that OLE + CMemFile layer is not ported.  These bodies use the in-app
// clipboard buffer (entity.cpp), which is the full single-editor round trip (Copy ->
// Entity_WriteSelected_R, Paste -> Map_ImportBuffer) and also carries the selection between
// maps (Map_Free's "Copy selection?" box on File->New/Open).
extern void RadiantClipboard_Copy();    // entity.cpp
extern void RadiantClipboard_Paste();   // entity.cpp

void XYWnd_PasteClip()
{
    RadiantClipboard_Paste();
}

void XYWnd_CopyClip()
{
    RadiantClipboard_Copy();
}

// CXYWnd::PositionView (0x46DE10) â€” recenter this XY view's origin.  The two in-plane
// axes depend on the view type (ED_VIEW_YZ=0/XZ=1/XY=2): nDim1 = (view==YZ), nDim2 =
// (view!=XY)+1 â†’ XY:{0,1} XZ:{0,2} YZ:{1,2}.  If a point-edit mode is active and there
// are move points, center on their average; otherwise center on the camera origin, and
// if exactly one brush is selected, on that brush's bbox centre.  Faithful to the disasm.
// U-GLOBALS: the body reads/writes the shell-agnostic view state (Ed_ActiveXY()) instead of
// this CXYWnd's own members â€” the XY view is a singleton, so they are the same view, and the
// state block is now the ONE authority the bridge no longer overwrites.
// U-CMD-1: extracted out of CXYWnd â€” the body never touched a CXYWnd member after U-GLOBALS,
// so it is a plain free function over Ed_ActiveXY() + Ed_Camera(); CXYWnd::PositionView is now
// a forwarder (the XY view is a singleton, so "this view" and "the active view" are the same).
void XYWnd_PositionView()
{
    xywndState_t *xy = Ed_ActiveXY();
    int nDim1 = ( xy->m_nViewType == ED_VIEW_YZ ) ? 1 : 0;
    int nDim2 = ( xy->m_nViewType != ED_VIEW_XY ) + 1;

    if ( ( g_qeglobals.d_select_mode == sel_vertex
        || g_qeglobals.d_select_mode == sel_curvepoint
        || g_qeglobals.d_select_mode == sel_area
        || g_qeglobals.d_select_mode == sel_terrainpoint )
      && g_qeglobals.d_num_move_points )
    {
        xy->m_vOrigin[nDim1] = 0.0f;
        xy->m_vOrigin[nDim2] = 0.0f;
        for ( int p = 0; p < g_qeglobals.d_num_move_points; ++p )
        {
            xy->m_vOrigin[nDim1] += g_qeglobals.d_move_points[p]->xyz[nDim1];
            xy->m_vOrigin[nDim2] += g_qeglobals.d_move_points[p]->xyz[nDim2];
        }
        xy->m_vOrigin[nDim1] = (float)( xy->m_vOrigin[nDim1] / (double)g_qeglobals.d_num_move_points );
        xy->m_vOrigin[nDim2] = (float)( xy->m_vOrigin[nDim2] / (double)g_qeglobals.d_num_move_points );
    }
    else
    {
        const camera_s *cam = Ed_Camera();
        xy->m_vOrigin[nDim1] = cam->origin[nDim1];
        xy->m_vOrigin[nDim2] = cam->origin[nDim2];
        for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
        {
            // The binary tests the LAST node (i->next == sentinel) â€” center on the single
            // (or last) selected brush's bbox.
            if ( i->next == &selected_brushes )
            {
                xy->m_vOrigin[nDim1] = ( i->def->maxs[nDim1] + i->def->mins[nDim1] ) * 0.5f;
                xy->m_vOrigin[nDim2] = ( i->def->maxs[nDim2] + i->def->mins[nDim2] ) * 0.5f;
            }
        }
    }
}

// â”€â”€â”€ CXYWnd::SetViewType (0x46DF90) â€” set the 2D-view axis (YZ/XZ/XY) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// The binary stores the EViewType (== m_nViewType value) directly, then â€” only in the
// floating single-XY layout (m_nCurrentStyle==1) â€” retitles the window "YZ Side" /
// "XZ Front" / "XY Top".  In the port's docked layout (m_nCurrentStyle!=1 by default)
// the title update is skipped exactly as the binary skips it.
// U-GLOBALS: writes the view type into the shell-agnostic state (Ed_ActiveXY()) â€” same
// singleton view, and the ONE authority now that the bridge no longer syncs it back in.
// U-CMD-1: extracted.  `vt` stays an int because ED_VIEW_YZ/XZ/XY (xywnd.h) ARE
// CXYWnd::EViewType's values (0/1/2) â€” verified against both enums.  The retitle is
// ::SetWindowTextA on the XY view's own HWND (d_hwndXY), which is what CWnd::SetWindowTextA
// resolved to; the style gate reads the shell-agnostic frame state.
void XYWnd_SetViewType( int vt )
{
    Ed_ActiveXY()->m_nViewType = vt;
    if ( g_radiantFrameState.currentStyle == 1 )
    {
        const char *title = "YZ Side";
        if ( Ed_ActiveXY()->m_nViewType == 2 )      title = "XY Top";
        else if ( Ed_ActiveXY()->m_nViewType == 1 ) title = "XZ Front";
        ::SetWindowTextA( g_qeglobals.d_hwndXY, title );
    }
}

// â”€â”€â”€ CXYWnd::SetRotateMode (0x46E090) â€” enter/leave free mouse-rotation mode â”€â”€â”€â”€â”€â”€
// Toolbar "Free rotation" button (OnSelectMouserotate).  Turning it ON requires a
// selection (else prints + stays off) and seats the rotate pivot at the selection
// centre (Select_GetTrueMid â†’ g_vRotateOrigin), zeroing the rotation accumulator.
// Returns the resulting g_bRotateMode so the caller can light the toolbar button.
extern void     Select_GetTrueMid( float *center );   // select.cpp (0x48FC20)
extern float    g_vRotateOrigin[3];                   // drag.cpp   (0x23F1658)
extern float    g_vRotation[3];                       // drag.cpp   (0x23F164C)
extern bool     g_bRotateMode;                        // drag.cpp   (0x23F16D9)

// U-CMD-1: extracted.  The trailing repaint was CWnd::RedrawWindow on the XY view itself,
// i.e. ::RedrawWindow(d_hwndXY, ...) â€” same window, same flags.
bool XYWnd_SetRotateMode( int bMode )
{
    if ( bMode && selected_brushes.next != &selected_brushes )
    {
        g_bRotateMode = true;
        Select_GetTrueMid( g_vRotateOrigin );
        g_vRotation[0] = g_vRotation[1] = g_vRotation[2] = 0.0f;
    }
    else
    {
        if ( bMode )
            Sys_Printf( "Need a brush selected to turn on Mouse Rotation mode\n" );
        g_bRotateMode = false;
    }
    ::RedrawWindow( g_qeglobals.d_hwndXY, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW );
    return g_bRotateMode;
}

// ─── CMainFrame::OnViewClipper (0x426510) + OnSelectMouserotate (0x428570) ────────
// The clip and mouse-rotate modes are mutually exclusive: entering EITHER cancels the OTHER
// (verbatim from the IW3xRadiant IDA — the earlier port dropped the cancel because rotate mode
// was unported; XYWnd_SetRotateMode above ships it now).  m_pActiveXY is always Ed_ActiveXY()
// here, so its null-guard falls away; the m_wndToolBar TB_CHECKBUTTON tail is the only DROPPED
// line (no toolbar in the shell yet — cosmetic button state).  Mutual recursion terminates:
// each cancels the other BEFORE flipping its own mode, so the re-entrant call sees the other
// mode still set and this one still clear.  Forward-declared so each can call the other.
static void Cmd_OnSelectMouserotate();

static void Cmd_OnViewClipper()      // 32783 (X)
{
    if ( g_bClipMode )
    {
        Ed_SetClipMode( 0 );                 // turn OFF (SetClipMode's leave path repaints)
    }
    else
    {
        if ( g_bRotateMode )
            Cmd_OnSelectMouserotate();        // cancel rotate mode first
        Ed_SetClipMode( 1 );                 // turn ON
    }
}

static void Cmd_OnSelectMouserotate()  // 32810 (R)
{
    if ( g_bClipMode )
        Cmd_OnViewClipper();                 // cancel clip mode first
    if ( g_bRotateMode )
    {
        g_bRotateMode = false;               // turn OFF
        Ed_InvalidateAllViews();             // was RedrawWindow(m_pActiveXY->m_hWnd, ...)
        // Binary tail: if EVERY selected brush is a patch, rebuild the display lists.
        selbrush_t *v3 = selected_brushes.next;
        if ( v3 != &selected_brushes )
        {
            while ( v3->patch )
            {
                v3 = v3->next;
                if ( v3 == &selected_brushes )
                    return;
            }
            Map_BuildBrushData();
        }
    }
    else
    {
        XYWnd_SetRotateMode( 1 );            // turn ON (needs a selection; prints if none)
    }
}

// CMainFrame::OnPatchWireframe (0x42a300) — Shift+W: cycle patch_wireframe 0→1→2→0, persist,
// repaint.  CMainFrame::OnTolerantWeld (0x42a130) — Shift+J: toggle m_bTolerantWeld, persist,
// repaint.  Both dropped only the m_wndToolBar TB_CHECKBUTTON tail (cosmetic; no toolbar yet).
extern void Prefs_SavePrefs( prefData_t *p );   // prefs.cpp (CPrefsDlg::SavePrefs 0x44f280)

static void Cmd_OnPatchWireframe()   // 32857 (Shift+W)
{
    if ( ++g_PrefsDlg->patch_wireframe > 2 )
        g_PrefsDlg->patch_wireframe = 0;
    Prefs_SavePrefs( g_PrefsDlg );
    g_nUpdateBits = -1;
}

static void Cmd_OnTolerantWeld()     // 33155 (Shift+J)
{
    g_PrefsDlg->m_bTolerantWeld ^= 1;
    Prefs_SavePrefs( g_PrefsDlg );
    g_nUpdateBits = -1;
}

// Patch mode toggles that call ported cores (0x42a990 / 0x42a950) + OnToggleDrawSurfs
// (0x42a040), which additionally re-flags every selected + active brush's special material.
// All three drop only the m_wndToolBar TB_CHECKBUTTON reflect-state SendMessage (cosmetic).
extern void       Patch_InsDelToggle();     // pmesh.cpp (0x447e50) — redisperse mode
extern void       Patch_BendToggle();       // pmesh.cpp (0x4478e0) — bend mode
extern void       sub_47B940( brush_t *b ); // brush.cpp (0x47b940) — Brush_UpdateSpecialMaterialFlag
extern selbrush_t active_brushes;           // map.cpp   (0x23F189C)

static void Cmd_OnPatchRedisperse() { Patch_InsDelToggle(); g_nUpdateBits = -1; }   // 32872 (0x42a990)
static void Cmd_OnPatchBend()       { Patch_BendToggle();   g_nUpdateBits = -1; }   // 32882 (0x42a950)

static void Cmd_OnToggleDrawSurfs()   // 33144 (0x42a040)
{
    g_PrefsDlg->draw_toggle ^= 1;
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
        sub_47B940( i->def );
    for ( selbrush_t *j = active_brushes.next;   j != &active_brushes;   j = j->next )
        sub_47B940( j->def );
    Prefs_SavePrefs( g_PrefsDlg );
    g_nUpdateBits = -1;
}

// â”€â”€â”€ CXYWnd forwarders (U-CMD-1).  The five bodies above are the real implementations;
// these keep mainfrm.h's CXYWnd declaration and every existing `m_pXYWnd->X()` /
// `m_pActiveXY->X()` call site (this file + map.cpp / entity.cpp / win_dlg.cpp) working
// unchanged.  U-GUARD deletes them with the class once those call sites are retargeted.

// CTextureBar::GetSurfaceAttributes + the rest of the bar now live in texturebar.cpp.

// â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•â•
// UI COMMAND-WIRING batch - CMainFrame WM_COMMAND thunks over already-ported cores, each
// transcribed verbatim from its binary handler (EA in the trailing comment).
extern int g_bCrossHairs;   // engine_stubs.cpp (0x25D5B06) â€” XY crosshair toggle

// â”€â”€ Camera fly keys (arrows / , . ) â€” Left/Right pitch-yaw, Forward/Back/Strafe move.
//    Each first tries PMESH_10 (patch-vertex move in vertex/curvepoint mode); if that
//    handles it (returns nonzero) the camera is left alone.  IDB 0x426720/70/0x4266B0/
//    0x426610/0x4267C0/0x426820.
extern char PMESH_10( char bAdd, int a2, int a3 );   // pmesh.cpp 0x43CB80

static void Cmd_OnCameraLeft()             // 0x426720 (cmd 33057, Left)
{
    if ( !PMESH_10( 0, 1, 0 ) )
    {
        Ed_Camera()->angles[1] += 22.5f;   // U-GLOBALS
        g_nUpdateBits |= 2 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
    }
}

static void Cmd_OnCameraRight()            // 0x426770 (cmd 33058, Right)
{
    if ( !PMESH_10( 0, -1, 0 ) )
    {
        Ed_Camera()->angles[1] -= 22.5f;   // U-GLOBALS
        g_nUpdateBits |= 2 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
    }
}

static void Cmd_OnCameraForward()          // 0x4266B0 (cmd 33059, Up)
{
    if ( !PMESH_10( 0, 0, -1 ) )
    {
        camera_s *cam = Ed_Camera();   // U-GLOBALS
        cam->origin[0] += cam->forward[0] * 32.0f;
        cam->origin[1] += cam->forward[1] * 32.0f;
        cam->origin[2] += 32.0f * cam->forward[2];
        g_nUpdateBits |= 2 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
    }
}

static void Cmd_OnCameraBack()             // 0x426610 (cmd 33060, Down)
{
    if ( !PMESH_10( 0, 0, 1 ) )
    {
        camera_s *cam = Ed_Camera();   // U-GLOBALS
        cam->origin[0] -= cam->forward[0] * 32.0f;
        cam->origin[1] -= cam->forward[1] * 32.0f;
        cam->origin[2] -= 32.0f * cam->forward[2];
        g_nUpdateBits |= 2 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
    }
}

static void Cmd_OnCameraStrafeleft()       // 0x4267C0 (cmd 33063, ',')
{
    camera_s *cam = Ed_Camera();   // U-GLOBALS
    cam->origin[0] -= cam->right[0] * 32.0f;
    cam->origin[1] -= cam->right[1] * 32.0f;
    cam->origin[2] -= 32.0f * cam->right[2];
    g_nUpdateBits |= 2 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
}

static void Cmd_OnCameraStraferight()      // 0x426820 (cmd 33064, '.')
{
    camera_s *cam = Ed_Camera();   // U-GLOBALS
    cam->origin[0] += cam->right[0] * 32.0f;
    cam->origin[1] += cam->right[1] * 32.0f;
    cam->origin[2] += 32.0f * cam->right[2];
    g_nUpdateBits |= 2 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
}

// â”€â”€ Grid toggle (0) â€” flip d_showgrid, redraw XY+Z.  IDB 0x426930. â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void Cmd_OnGridToggle()             // cmd 33065
{
    g_qeglobals.d_showgrid = !g_qeglobals.d_showgrid;
    g_nUpdateBits |= W_XY | W_Z;
}

// â”€â”€ Grid size next/prev ( ] / [ ) â€” step d_gridsize within [0,10].  IDB 0x4289A0/D0. â”€
static void Cmd_OnGridNext()               // cmd 33083 (])
{
    if ( g_qeglobals.d_gridsize < 10 )
        ++g_qeglobals.d_gridsize;
    Radiant_CheckGridMenu();
    g_nUpdateBits |= 0xAu;                   // W_XY | W_Z
    Radiant_SetGridStatus();
}

static void Cmd_OnGridPrev()               // cmd 33084 ([)
{
    if ( g_qeglobals.d_gridsize > 0 )
        --g_qeglobals.d_gridsize;
    Radiant_CheckGridMenu();
    g_nUpdateBits |= 0xAu;
    Radiant_SetGridStatus();
}

// â”€â”€ Texture-shift step inc/dec (Shift+KP-/KP+) â€” walk d_savedinfo.d_gridsize as an INT
//    counter (LODWORD bit-manip; the wrap-guard keeps it out of 0).  IDB 0x4287B0/830.
static void Cmd_OnSelectionTextureSnapDec()  // cmd 33072
{
    int *g = (int *)&g_qeglobals.d_savedinfo.d_gridsize;
    if ( !--*g )
        *g = -1;   // binary sets NAN bit-pattern (0xFFFFFFFF int) to avoid 0
    Radiant_SetGridStatus();
}

static void Cmd_OnSelectionTextureSnapInc()  // cmd 33073
{
    int *g = (int *)&g_qeglobals.d_savedinfo.d_gridsize;
    if ( !++*g )
        *g = 1;
    Radiant_SetGridStatus();
}

// â”€â”€ Texture fit / fit-all (Ctrl+F / Ctrl+Shift+F) â€” Brush_FitTexture 1Ã—1.  IDB
//    0x4287D0 / 0x428800. â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
extern void Brush_FitTexture( float x, float y, int a4 );      // select.cpp 0x4939E0
static void Cmd_OnSelectionTextureFitUnk()   // cmd 33074
{
    Brush_FitTexture( 1.0f, 1.0f, 0 );
    g_nUpdateBits = -1;
}
static void Cmd_OnTextureFitAll()            // cmd 33234
{
    Brush_FitTexture( 1.0f, 1.0f, 1 );
    g_nUpdateBits = -1;
}

// â”€â”€ Texture rotate CW / CCW (Ctrl+arrows) â€” Brush_RotateTexture(Â±ClampGridSize).  IDB
//    0x428850 / 0x428860. â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void Cmd_OnTexRotateClockwise()       // cmd 33075
{
    Brush_RotateTexture( ClampGridSize() );
}
static void Cmd_OnTexRotateCounterCW()       // cmd 33076
{
    Brush_RotateTexture( -ClampGridSize() );
}

// â”€â”€ Texture shift L/R/U/D (Shift+arrows) â€” Brush_ShiftTexture by Â±2Â·grid.  IDB
//    0x4288E0/910/870/8A0. â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
extern void Brush_ShiftTexture( float ds, float dt );          // select.cpp 0x491F20
static void Cmd_OnTexShiftLeft()             // cmd 33079
{
    Brush_ShiftTexture( grid_sizes[g_qeglobals.d_gridsize] + grid_sizes[g_qeglobals.d_gridsize], 0.0f );
}
static void Cmd_OnTexShiftRight()            // cmd 33080
{
    Brush_ShiftTexture( grid_sizes[g_qeglobals.d_gridsize] * -2.0f, 0.0f );
}
static void Cmd_OnTexShiftUp()               // cmd 33081
{
    Brush_ShiftTexture( 0.0f, grid_sizes[g_qeglobals.d_gridsize] + grid_sizes[g_qeglobals.d_gridsize] );
}
static void Cmd_OnTexShiftDown()             // cmd 33082
{
    Brush_ShiftTexture( 0.0f, grid_sizes[g_qeglobals.d_gridsize] * -2.0f );
}

// â”€â”€ Z-view zoom (Ctrl+Del / Ctrl+Ins) + Z 100% â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void Cmd_OnViewZ100()               // cmd 32998 â€” empty stub in the binary (0x424740)
{
}
static void Cmd_OnViewZzoomin()            // cmd 32999 (0x424A00)
{
    g_nUpdateBits |= 0x28u;                  // W_Z | W_Z_OVERLAY
    z_scale *= 1.25f;
    if ( z_scale > 160.0f )
        z_scale = 160.0f;
}
static void Cmd_OnViewZzoomout()           // cmd 33000 (0x424A40)
{
    g_nUpdateBits |= 0x28u;
    z_scale *= 0.800000011920929f;
    if ( z_scale < 0.003125000046566129f )
        z_scale = 0.003125f;
}

// â”€â”€ Cubic-clip zoom out / in (Ctrl+[ / ]) â€” Â±m_nCubicScale [1,220], persist.  IDB
//    0x428F50 / 0x428F10. â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void Cmd_OnViewCubeout()            // cmd 32819
{
    if ( ++g_PrefsDlg->m_nCubicScale > 220 )
        g_PrefsDlg->m_nCubicScale = 220;
    Prefs_SavePrefs( g_PrefsDlg );
    g_nUpdateBits |= 1u;
    Radiant_SetGridStatus();
}
static void Cmd_OnViewCubein()             // cmd 32820
{
    if ( --g_PrefsDlg->m_nCubicScale < 1 )
        g_PrefsDlg->m_nCubicScale = 1;
    Prefs_SavePrefs( g_PrefsDlg );
    g_nUpdateBits |= 1u;
    Radiant_SetGridStatus();
}

// â”€â”€ View layout XY / YZ / XZ (0x424710/0x423FB0/0x424A80) â€” set the active 2D-view axis.
//    m_nCurrentStyle==2 is the "no XY pane" style; skip there.  EViewType == m_nViewType. â”€
static void Cmd_OnViewXy()                 // cmd 32772
{
    if ( g_radiantFrameState.currentStyle != 2 )
    {
        XYWnd_SetViewType( ED_VIEW_XY );
        XYWnd_PositionView();
    }
    g_nUpdateBits |= W_XY;
}
static void Cmd_OnViewYz()                 // cmd 32774
{
    if ( g_radiantFrameState.currentStyle != 2 )
    {
        XYWnd_SetViewType( ED_VIEW_YZ );
        XYWnd_PositionView();
    }
    g_nUpdateBits |= W_XY;
}
static void Cmd_OnViewXz()                 // cmd 32773
{
    if ( g_radiantFrameState.currentStyle != 2 )
    {
        XYWnd_SetViewType( ED_VIEW_XZ );
        XYWnd_PositionView();
    }
    g_nUpdateBits |= W_XY;
}

// â”€â”€ Toggle YZ / XZ view (0x427230/0x427220) â€” EMPTY stubs in the binary. â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
static void Cmd_OnToggleviewYz() {}        // cmd 32797
static void Cmd_OnToggleviewXz() {}        // cmd 32798

// â”€â”€ Viewâ†’Toggleâ†’{Console,Camera,XY,Z} show/hide (0x426A90 / 0x426A40 / 0x426AE0 /
//    0x426B30) â€” the four SHOW/HIDE toggles (dead in the port until the U7 backfill).
//    Each is the same shape: gate on the layout style, then flip the child window's
//    visibility with IsWindowVisible + ShowWindow(SW_HIDE/SW_SHOW).  Ported verbatim.
//    NOTE the STYLE GATES differ per handler and are the binary's, not a typo:
//      console/camera : style > 0 && style < 3   (i.e. 1 or 2)
//      XY             : style == 1  ONLY
//      Z              : style == 1 || style == 2, ELSE tail-jumps to Undo_Redo (see below)
static void Cmd_OnToggleconsole()          // cmd 33068 (0x426A90)
{
    // The binary's m_pEditWnd is the docked console CEdit (g_qeglobals.d_hwndEdit); in this
    // port that is the embedded m_wndConsole child.
    if ( g_radiantFrameState.currentStyle > 0 && g_radiantFrameState.currentStyle < 3 )
    {
        Radiant_ToggleChildVisible( g_qeglobals.d_hwndEdit );   // U-CMD-1: was m_wndConsole
    }
}

static void Cmd_OnTogglecamera()           // cmd 33069 (0x426A40), Shift+Ctrl+C
{
    if ( g_radiantFrameState.currentStyle > 0 && g_radiantFrameState.currentStyle < 3 )
    {
        Radiant_ToggleChildVisible( g_qeglobals.d_hwndCamera );   // U-CMD-1: was m_pCamWnd
    }
}

static void Cmd_OnToggleview()             // cmd 33071 (0x426AE0), Shift+Ctrl+V
{
    if ( g_radiantFrameState.currentStyle == 1 )
    {
        Radiant_ToggleChildVisible( g_qeglobals.d_hwndXY );   // U-CMD-1: was m_pXYWnd
    }
}

static void Cmd_OnTogglez()                // cmd 33070 (0x426B30), Shift+Ctrl+Z
{
    // ORIGINAL QUIRK, reproduced verbatim: the style-mismatch path at 0x426B43 is
    // `pop esi ; jmp Undo_Redo` â€” i.e. in any layout other than 1/2 this command performs
    // a REDO instead of a Z-view toggle.  Not an ICF artifact (the else arm is reachable by
    // fall-through from the two `jz`s and the function already has its own `pop esi ; retn`
    // epilogue at 0x426B7D).  This port fixes m_nCurrentStyle at 1, so the arm is unreachable
    // here; it is kept so the code matches the binary.
    if ( g_radiantFrameState.currentStyle == 1 || g_radiantFrameState.currentStyle == 2 )
    {
        Radiant_ToggleChildVisible( g_qeglobals.d_hwndZ );   // U-CMD-1: was m_pZWnd
    }
    else
    {
        Undo_Redo();
    }
}

// â”€â”€ Next view (Ctrl+Tab, 0x426DB0) â€” cycle the active XY view type XYâ†’XZâ†’YZâ†’XY. â”€â”€â”€â”€â”€â”€
static void Cmd_OnViewNextview()           // cmd 32789
{
    if ( g_radiantFrameState.currentStyle != 2 )
    {
        int vt = Ed_ActiveXY()->m_nViewType;   // U-GLOBALS: was m_pXYWnd->m_nViewType
        if ( vt == 2 )      XYWnd_SetViewType( ED_VIEW_XZ );
        else if ( vt == 1 ) XYWnd_SetViewType( ED_VIEW_YZ );
        else                XYWnd_SetViewType( ED_VIEW_XY );
        XYWnd_PositionView();
        g_nUpdateBits |= 2u;
    }
}

// â”€â”€ Toolbar Main / Texture show-hide (0x4290F0/0x429100) â€” EMPTY stubs in the binary. â”€
static void Cmd_OnToolbarMain() {}         // cmd 32830
static void Cmd_OnToolbarTexture() {}      // cmd 32832

// â”€â”€ Center 2D on camera (Shift+C, 0x42A2D0) â€” copy the camera origin into the XY view. â”€
static void Cmd_OnCenter2DOnCamera()       // cmd 33108
{
    xywndState_t   *xy  = Ed_ActiveXY();   // U-GLOBALS: was g_pParentWnd->m_pActiveXY
    const camera_s *cam = Ed_Camera();     // U-GLOBALS: was g_pParentWnd->m_pCamWnd
    g_nUpdateBits |= 2u;
    xy->m_vOrigin[0] = cam->origin[0];
    xy->m_vOrigin[1] = cam->origin[1];
    xy->m_vOrigin[2] = cam->origin[2];
}

// â”€â”€ Selection cycle next / prev (Shift+, / Shift+.) â€” SelectNext/Prev + redraw. â”€â”€â”€â”€â”€â”€
extern void SelectNext();                   // select.cpp 0x494210
extern void SelectPrev();                   // select.cpp 0x494360
static void Cmd_OnSelectNext()             // cmd 33160 (0x423C90)
{
    SelectNext();
    g_nUpdateBits |= 5u;                     // W_XY | W_CAMERA
}
static void Cmd_OnSelectPrev()             // cmd 33161 (0x423CA0)
{
    SelectPrev();
    g_nUpdateBits |= 5u;
}

// â”€â”€ Move selection up / down (KP+ / KP-) â€” translate Â±grid along Z, wrapped in undo. â”€â”€
extern void Undo_ClearRedo();               // undo.cpp
extern void Undo_GeneralStart( const char *op );
extern void Undo_AddBrushList( selbrush_t *list );
extern void Undo_EndBrushList( selbrush_t *list );
extern void Undo_End();
static void Cmd_OnSelectionMovedown()      // cmd 32829 (0x429050)
{
    extern void Select_Move( const float *delta, char bSnap );
    Undo_ClearRedo();
    Undo_GeneralStart( "move up" );
    Undo_AddBrushList( &selected_brushes );
    float vAmt[3] = { 0.0f, 0.0f, -grid_sizes[g_qeglobals.d_gridsize] };
    Select_Move( vAmt, 1 );
    g_nUpdateBits |= 0xBu;                    // W_XY | W_CAMERA | W_Z
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}
static void Cmd_OnSelectionMoveup()        // cmd 32831 (0x4290B0)
{
    extern void Select_Move( const float *delta, char bSnap );
    float delta[3] = { 0.0f, 0.0f, grid_sizes[g_qeglobals.d_gridsize] };
    Select_Move( delta, 1 );
    g_nUpdateBits |= 0xBu;
}

// â”€â”€ Selection nudge L/R/U/D (Alt+arrows) â€” NudgeSelection(dir, this, grid).  IDB
//    0x429510/530/550/4F0. â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// U-CMD-2: the MFC-signature entry point (and these four members) is the MFC shell's only
// caller; the command table dispatches 32847..32850 straight at NudgeSelection_Apply
// (select.cpp:4541), the shell-agnostic core select.cpp already split out.

// â”€â”€ Splay / Set View To Entity / Link Selected / Get Distance / Select All Of Type â”€â”€
extern void DoSplay();                      // pmesh.cpp 0x44A430
extern void SetViewToEntity();              // select.cpp 0x48C3D0
extern void LinkSelected();                 // select.cpp 0x48C7B0
extern void Get_DistanceBetweenEnts();      // select.cpp 0x47A260
extern void Select_ByTexture( int recurse );      // select.cpp 0x4934F0
extern void Select_ByClassSimilar();              // select.cpp 0x493830
static void Cmd_OnSplay()                  // cmd 33157 (0x4254F0)
{
    DoSplay();
}
static void Cmd_OnSetViewToEntity()        // cmd 33210 (0x425540)
{
    SetViewToEntity();
}
static void Cmd_OnLinkSelected()           // cmd 33211 (0x425500)
{
    LinkSelected();
}
static void Cmd_OnDistanceBetweenEntities() // cmd 33178 (0x4294D0)
{
    Get_DistanceBetweenEnts();
}
static void Cmd_OnSelectAllOfType()        // cmd 33093 (0x42B470)
{
    if ( selected_brushes.next == &selected_brushes )
        Select_ByTexture( 0 );
    else
        Select_ByClassSimilar();
    g_nUpdateBits = -1;
}
static void Cmd_OnSelectAllOfTypeRecursive() // cmd 33212 (0x42B4B0)
{
    if ( selected_brushes.next == &selected_brushes )
        Select_ByTexture( 1 );
    else
        Select_ByClassSimilar();
    g_nUpdateBits = -1;
}

// â”€â”€ Hide/Show workflow (H / Shift+H / Alt+H / Ctrl+H) â€” select.cpp cores. â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
extern void Select_Hide();                  // select.cpp 0x493D50
extern void Select_HideUnselected();        // select.cpp 0x493DD0
extern void ShowHidden();                   // select.cpp 0x493E50
extern void ShowLastHidden();               // select.cpp 0x493EA0
static void Cmd_OnHideSelected()           // cmd 32923 (0x42B6A0)
{
    Select_Hide();
    Select_Deselect( 1 );
}
static void Cmd_OnHideUnselected()         // cmd 32934 (0x42B6C0)
{
    Select_HideUnselected();
}
static void Cmd_OnShowHidden()             // cmd 32924 (0x42B6D0)
{
    ShowHidden();
}
static void Cmd_OnShowLastHidden()         // cmd 33246 (0x42B6E0)
{
    ShowLastHidden();
}

// â”€â”€ Selection draw toggles: crosshair / no-outline / no-tint.  IDB 0x42B690/0x425630/50.
static void Cmd_OnViewCrosshair()          // cmd 33100
{
    g_bCrossHairs ^= 1u;
    g_nUpdateBits |= 2u;
}
static void Cmd_OnSelectionNoOutline()     // cmd 33103
{
    g_qeglobals.dontDrawSelectedOutlines = !g_qeglobals.dontDrawSelectedOutlines;
    g_nUpdateBits = -1;
}
static void Cmd_OnSelectionNoTint()        // cmd 33172
{
    g_qeglobals.dontDrawSelectedTint = !g_qeglobals.dontDrawSelectedTint;
    g_nUpdateBits = -1;
}

// â”€â”€ Select same target / targetname (B / Ctrl+B) â€” copy the FIRST selected entity's
//    key value onto all selected.  IDB 0x42ADA0 (targetname) / 0x42ADD0 (target). â”€â”€â”€â”€â”€â”€
extern void Select_AllByKeyValue( const char *key );   // select.cpp (sub_485B70 0x485B70)
static void Cmd_OnSelectSameTargetname()   // cmd 36121 (sub_42ADA0)
{
    Select_AllByKeyValue( "targetname" );
    g_nUpdateBits = -1;
}
static void Cmd_OnSelectSameTarget()       // cmd 36123 (sub_42ADD0)
{
    Select_AllByKeyValue( "target" );
    g_nUpdateBits = -1;
}

// â”€â”€ ON_UPDATE_COMMAND_UI handlers â€” keep menu items' enable/grey state live. â”€â”€â”€â”€â”€â”€â”€â”€â”€
extern undo_s *g_lastundo;              // undo.cpp (0x23F162C)
extern bool    Undo_RedoAvailable();    // undo.cpp â€” g_lastredo != nullptr
extern int     g_region_active;         // map.cpp (0x23F1744)


// â”€â”€ Light-preview submenu (F8 workflow) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// U-GLOBALS: CamWnd_AddLightPreview / CamWnd_RemoveLightPreview / CamWnd_ClearLightPreviews
// are declared with the other accessors at the top of this file.
extern float world_orient_matrix[4][3];                                       // entity.cpp 0x6DE290

static void Cmd_OnEnableLightPreview()     // cmd 33950 (0x4240C0)
{
    g_PrefsDlg->enable_light_preview = ( g_PrefsDlg->enable_light_preview == 0 );
    Prefs_SavePrefs( g_PrefsDlg );
    ::CheckMenuItem( Radiant_FrameMenu(), 33950, g_PrefsDlg->enable_light_preview ? MF_CHECKED : MF_UNCHECKED );
    g_nUpdateBits |= 1u;
}
static void Cmd_OnPreviewSun()             // cmd 36108 (0x424060)
{
    g_PrefsDlg->preview_sun_aswell = ( g_PrefsDlg->preview_sun_aswell == 0 );
    Prefs_SavePrefs( g_PrefsDlg );
    ::CheckMenuItem( Radiant_FrameMenu(), 36108, g_PrefsDlg->preview_sun_aswell ? MF_CHECKED : MF_UNCHECKED );
    g_nUpdateBits |= W_CAMERA;
}
static void Cmd_OnStartPreviewSelected()   // cmd 33951 (0x424120)
{
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
    {
        entity_s *owner = i->owner;
        if ( ( ((entity_s_def *)owner->def)->eclass->classtype & 1 ) != 0 )   // light
            CamWnd_AddLightPreview( i, 0, (const orientation_t *)world_orient_matrix );
    }
    g_nUpdateBits |= W_CAMERA;
}
static void Cmd_OnStopPreviewSelected()    // cmd 33952 (0x424170)
{
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
    {
        entity_s *owner = i->owner;
        if ( ( ((entity_s_def *)owner->def)->eclass->classtype & 1 ) != 0 )   // light
            CamWnd_RemoveLightPreview( i );
    }
    g_nUpdateBits |= 1u;
}
static void Cmd_OnClearPreviewList()       // cmd 33953 (0x4241C0)
{
    g_nUpdateBits |= W_CAMERA;
    CamWnd_ClearLightPreviews();   // U-GLOBALS: was m_pCamWnd->light_preview_count = 0
}
static void Cmd_OnPreviewAtMaxIntensity()  // cmd 36122 (0x425670)
{
    g_qeglobals.preview_at_max_intensity = !g_qeglobals.preview_at_max_intensity;
    g_nUpdateBits = -1;
}

// â”€â”€ Fileâ†’Load / Save-Selected / Save-Region â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
extern void Map_ImportFile( const char *path );                  // map.cpp 0x488C70
extern void DefaultExtension( char *path, const char *ext );     // cmdlib.cpp
extern void Map_SaveFile( const char *path, char bRegion, char a3 ); // map.cpp
typedef int WriteFunc_map_t( int ctx, const char *fmt, ... );
extern void Entity_WriteSelected_R( WriteFunc_map_t **writer );  // map.cpp 0x488DF0

// File-writing WriteFunc for OnFileExportmap_Sub (a 2-slot writer whose [1] is the FILE*).
static int Radiant_FileWriter( int ctx, const char *fmt, ... )
{
    WriteFunc_map_t **writer = (WriteFunc_map_t **)ctx;
    FILE *fp = (FILE *)writer[1];
    va_list ap; va_start( ap, fmt );
    int n = vfprintf( fp, fmt, ap );
    va_end( ap );
    return n;
}

// 0x488EB0  OnFileExportmap_Sub â€” write the selection to `pFilename` (iwmap 4 header).
static void OnFileExportmap_Sub( const char *pFilename )
{
    FILE *fp = fopen( pFilename, "w" );
    if ( !fp )
    {
        Sys_Printf( "ERROR!!!! Couldn't open %s\n", pFilename );
        return;
    }
    fprintf( fp, "iwmap %i\n", 4 );
    WriteFunc_map_t *writer[2];
    writer[0] = &Radiant_FileWriter;
    writer[1] = (WriteFunc_map_t *)fp;
    Entity_WriteSelected_R( writer );
    fclose( fp );
}

static void Cmd_OnFileImportmap()          // cmd 32844 â€” Fileâ†’Load (merge .map)  (0x429290)
{
    char file[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    memset( &ofn, 0, sizeof( ofn ) );
    ofn.lStructSize = sizeof( ofn );
    ofn.hwndOwner   = g_qeglobals.d_hwndMain;   // U-CMD-1: == the frame HWND
    ofn.lpstrFilter = "Map files (*.map)\0*.map\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = sizeof( file );
    ofn.lpstrTitle  = "Load Map";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if ( GetOpenFileNameA( &ofn ) )
        Map_ImportFile( file );
}

static void Cmd_OnFileExportmap()          // cmd 32845 â€” Fileâ†’Save Selected  (0x4293A0)
{
    char file[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    memset( &ofn, 0, sizeof( ofn ) );
    ofn.lStructSize = sizeof( ofn );
    ofn.hwndOwner   = g_qeglobals.d_hwndMain;   // U-CMD-1: == the frame HWND
    ofn.lpstrFilter = "Map files (*.map)\0*.map\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = sizeof( file );
    ofn.lpstrDefExt = "map";
    ofn.lpstrTitle  = "Save Selected";
    ofn.Flags       = OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
    if ( GetSaveFileNameA( &ofn ) )
        OnFileExportmap_Sub( file );
}

static void Cmd_OnFileSaveregion()         // cmd 32827 â€” Fileâ†’Save Region  (0x429020)
{
    // The binary calls SaveAsDialog(1); its region path (a1!=0) is just the Save dialog +
    // DefaultExtension(".reg") + Map_SaveFile(file, 1, 0) â€” the MRU management (MRU_* not
    // ported) only runs on the non-region (SaveAs) path, so it is faithfully omitted here.
    char file[MAX_PATH] = "";
    OPENFILENAMEA ofn;
    memset( &ofn, 0, sizeof( ofn ) );
    ofn.lStructSize = sizeof( ofn );
    ofn.hwndOwner   = g_qeglobals.d_hwndCamera;
    ofn.lpstrFilter = "Map file (*.map, *.reg)\0*.map\0*.reg\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = sizeof( file );
    ofn.Flags       = OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST; // binary Flags 6162
    if ( !GetSaveFileNameA( &ofn ) )
        return;
    DefaultExtension( file, ".reg" );
    Map_SaveFile( file, 1, 0 );
}

// â”€â”€ CEdBlankPane (Tex placeholder + laymat content render shell) â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
extern bool g_radiantFirstLightRendererReady;


// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
// 0x42c030  MainFrm_BrushList  (MainFrm.cpp:6372-6384)
// Not a window populate: a pure Assert-based integrity walk over one brush list,
// called by Map_LoadFile / the prefab enter-leave pair / Undo_GeneralStart+Undo_End
// with a label describing the moment.  Walks the DISPLAY list (prev/next) and checks
// the link back-pointers, the def/owner cross-link and the def refcount.
// (Its sibling MainFrm_EntList 0x42c1e0 is deliberately still a stub â€” see engine_stubs.cpp.)
// â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
extern void Assert( const char *file, int line, int type, const char *fmt, ... );  // engine_stubs.cpp

void MainFrm_BrushList( int message, selbrush_t *brushList )
{
    const char *msg = (const char *)(intptr_t)message;
    iassert( brushList );   // mainfrm.cpp:6372

    for ( selbrush_t *brush = brushList->next; brush != brushList; brush = brush->next )
    {
        vassert( (brush), "(message) = %s", msg );   // MainFrm.cpp:6375
        vassert( (brush->prev), "(message) = %s", msg );   // MainFrm.cpp:6376
        vassert( (brush->next), "(message) = %s", msg );   // MainFrm.cpp:6377
        vassert( (brush->prev->next == brush), "(message) = %s", msg );   // MainFrm.cpp:6378
        vassert( (brush->next->prev == brush), "(message) = %s", msg );   // MainFrm.cpp:6379
        vassert( (brush->def), "(message) = %s", msg );   // MainFrm.cpp:6381
        vassert( (brush->owner), "(message) = %s", msg );   // MainFrm.cpp:6382
        vassert( (brush->def->owner == brush->owner->def), "(message) = %s", msg );   // MainFrm.cpp:6383
        vassert( (brush->def->refCount >= 1), "(message) = %s", msg );   // MainFrm.cpp:6384
    }
}

// ============================================================================
//  U-CMD-1 - Radiant_DispatchCommandDirect: the shell-agnostic id->action table.
//
//  U-RIP: the frame's AFX_MSGMAP is deleted, so THIS is the only id->action table; every
//  caller (menus, accelerators, hotkeys, the ImGui shell) reaches it through
//  Radiant_ExecCommand.
//
//  Extraction rule: a handler is extracted when its body reaches the editor ONLY through
//  free functions + globals (cores, g_qeglobals, g_PrefsDlg, g_nUpdateBits, Ed_Camera(),
//  Ed_ActiveXY(), the frame's raw HMENU, the d_hwnd* views).  The handlers that were bound
//  to a now-deleted shell object (the toolbar's TB_CHECKBUTTON state machine, the HMENU
//  popups) return false with a POST-RIP marker; false means "not routable yet", never
//  "the command did nothing".
//
//  U-CMD-2 routed the deferred block: every dialog command now flips its ImGui panel, the
//  File-menu map flows call the extracted free bodies, and the MRU picks reach DoMru.  ONE
//  CAVEAT on the panel toggles: the panels only RENDER while the ImGui shell is up (it is
//  behind -imgui, and the overlay draws inside the camera-window / dockhost paint), so with
//  the overlay off the toggle still flips its bool silently and nothing appears.  That is
//  accepted for v1 - the flag is correct, only the presenter is missing - and it self-heals
//  the moment the shell is on.  What is left returns false with a POST-RIP marker: those are
//  the toolbar-state commands (there is no toolbar in the raw shell until an ImGui one
//  exists), the m_pActiveXY-gated clipper commands, the HMENU popup commands (U-MENU), and
//  the two project-file dialogs.
// ============================================================================
// The panel toggles the deferred dialog commands route to (imgui_panel_*.cpp +
// imgui_panels.cpp).  Each flips the file-static show flag its checkbox drives - the raw
// twin of the MFC handler's dialog Show/Toggle.
extern void ImGuiPanel_FindTex_Toggle();     // 32812  was CFindTextureDlg::show()
extern void ImGuiPanel_Layers_Toggle();      // 33954  was CLayerDlg::Toggle()
extern void ImGuiPanel_DynEnt_Toggle();      // 36106  was CDynEntityDlg::Toggle()
extern void ImGuiPanel_Vehicle_Toggle();     // 33221  was CVehicleDlg::Toggle()
extern void ImGuiPanel_Model_Toggle();       // 33240  was CModelDlg::Toggle()
extern void ImGuiPanel_VertEdit_Toggle();    // 33199  was CVertEditDlg::Toggle()
extern void ImGuiPanel_MapInfo_Toggle();     // 32786  was CMapInfo::Show()
extern void ImGuiPanel_Prefs_Toggle();       // 32784  was Prefs_ShowDialog( this )
extern void ImGuiPanel_Commands_Toggle();    // 32790  was CCommandsDlg::DoModal()
extern void ImGuiPanel_AdvPatch_Toggle();    // 33130  was AdvPatchEdit_Toggle( this )
extern void ImGuiPanel_GoTo_Toggle();        // 33107 / 33091's no-selection arm  was CGoToDlg::Show()
extern void ImGuiPanel_ArbRotate_Toggle();   // 33033  was CArbRotateDlg::Show()
extern void ImGuiPanel_FindBrush_Toggle();   // 33023  was CFindBrushDlg::Show()
extern void ImGuiPanel_Scale_Toggle();       // 32809  was CScaleDialog::DoModal()
extern void ImGuiPanel_Thicken_Toggle();     // 32904  was CDialogThick::DoModal()

// The two command bodies that live in other TUs next to their cores.
extern void NudgeSelection_Apply( int dir, float amt );   // select.cpp (0x429570 minus the frame arg)
extern void Radiant_PrefabEnter();                        // map.cpp (0x42BF70, with Prefab_NextLevel)
extern void Radiant_PrefabLeave();                        // map.cpp (0x42BF80, with Prefab_PrevLevel)

bool Radiant_DispatchCommandDirect( unsigned int cmdId )
{
    // FillTextureMenu-built filter submenus: id ranges, not discrete ids.
    if ( cmdId >= 60000 && cmdId <= 60255 ) { Cmd_OnFilterUsage( cmdId ); return true; }   // Textures->Usage filter submenu       0x4243E0
    if ( cmdId >= 60256 && cmdId <= 60511 ) { Cmd_OnFilterLocale( cmdId ); return true; }   // Textures->Locale filter submenu      0x424400
    if ( cmdId >= 60512 && cmdId <= 60767 ) { Cmd_OnFilterSurfaceType( cmdId ); return true; }   // Textures->Surface-type filter submenu 0x424420

    // File->Recent Files, the MRU_InsertItem-built menu items (ON_COMMAND_RANGE(8000,8009) ->
    // CMainFrame::OnMru 0x423FE0).  DoMru already took the frame HWND, and it rebuilds the
    // recent-files items in ::GetSubMenu( ::GetMenu( hWnd ), 0 ) itself.
    if ( cmdId >= 8000 && cmdId <= 8009 ) { DoMru( (short)cmdId, g_qeglobals.d_hwndMain ); return true; }

    switch ( cmdId )
    {
    case ID_EDIT_UNDO: Cmd_OnEditUndo(); return true;   // Edit->Undo
    case ID_EDIT_REDO: Cmd_OnEditRedo(); return true;   // Edit->Redo
    case 33039: Cmd_OnEditCopybrush(); return true;   // Edit->Copy            0x4286B0
    case 33040: Cmd_OnEditPastebrush(); return true;   // Edit->Paste           0x4286D0
    case 32818: Cmd_OnFileProjectsettings(); return true;   // File->Project Settings 0x428DE0
    case 32995: Cmd_OnViewZoomin(); return true;   // View->Zoom->XY Zoom In  0x424750
    case 32996: Cmd_OnViewZoomout(); return true;   // View->Zoom->XY Zoom Out 0x4247E0
    case 32968: Cmd_OnView100(); return true;   // View->Zoom->XY 100%     0x423C30
    case 33002: Cmd_OnSelectionDeselect(); return true;   // Selection->Deselect     0x425740
    case 33005: Cmd_OnSelectionDragVertices(); return true;   // Selection->Drag Vertices 0x425840
    case 33006: Cmd_OnSelectionDragEdges(); return true;   // Selection->Drag Edges    0x4257D0
    case 32982: Cmd_OnSelectionMakehollow(); return true;   // CSG->Hollow    0x425570
    case 32927: Cmd_OnSelectionCsgmerge(); return true;   // CSG->Merge     0x4255D0
    case 33183: Cmd_OnDropSelected(); return true;   // Drop to Floor  0x425BE0
    case 36113: Cmd_OnMakePhysCylinder(); return true;   // Physics->Cylinder 0x4291D0
    case 36120: Cmd_OnMakePhysBox(); return true;   // Physics->Box      0x429200
    case 32979: Cmd_OnRegionOff(); return true;   // Region->Off            0x4252B0
    case 32980: Cmd_OnRegionSetxy(); return true;   // Region->Set XY         0x4252F0
    case 33007: Cmd_OnRegionSettallbrush(); return true;   // Region->Set Tall Brush 0x4252E0
    case 32981: Cmd_OnRegionSetbrush(); return true;   // Region->Set Brush      0x4252C0
    case 33044: Cmd_OnRegionSetselection(); return true;   // Region->Set Selected   0x4252D0
    case 32793: Cmd_OnSnaptogrid(); return true;   // Grid->Snap to grid     0x428380
    case 32785: Cmd_OnToggleLockMoves(); return true;   // Texture Lock->Moves     0x426B80
    case 32835: Cmd_OnToggleLockRotations(); return true;   // Texture Lock->Rotations 0x429230
    case 33237: Cmd_OnToggleLockLightmap(); return true;   // Texture Lock->Lightmaps 0x426BF0
    case 33041: Cmd_OnTexturesInspector(); return true;   // Surface Inspector 0x424B60
    case 33092: Cmd_OnPatchInspector(); return true;   // Patch->Inspector  0x42B460
    case 35001: Cmd_OnSelectionAddToActiveLayer(); return true;   // ctx Add selection to active layer 0x466930
    case 32787: Cmd_OnEditEntityinfo(); return true;   // Edit->Entity Info 0x426D6F
    case 33134: Cmd_OnSelectConneted(); return true;   // Select Connected  0x425550
    case 33132: Cmd_OnSelectionTargetname(); return true;   // Select Targetname 0x426390
    case 202: Cmd_OnSelectionClassname(); return true;   // Select Classname  0x4263A0
    case 33035: Cmd_OnSelectionUngroupentity(); return true;   // Ungroup entity    0x426380
    case 33042: Cmd_OnSelectionMakeDetail(); return true;   // Make Detail       0x4261C0
    case 33043: Cmd_OnSelectionMakeStructural(); return true;   // Make Structural   0x426200
    case 33133: Cmd_OnSelectionKeyValue(); return true;   // Select by Key/Value 0x4263B0
    case 32984: Cmd_OnSelectionCompleteTall(); return true;   // Select Complete Tall 0x426340
    case 32983: Cmd_OnSelectionPartialTall(); return true;   // Select Partial Tall  0x426360
    case 32986: Cmd_OnSelectionTouching(); return true;   // Select Touching      0x426370
    case 33008: Cmd_OnSelectionInside(); return true;   // Select Inside        0x426350
    case 33003: Cmd_OnSelectionDelete(); return true;   // Edit->Delete         0x425690
    case 33220: Cmd_OnSelectionAutoCaulk(); return true;   // CSG->Auto Caulk      0x425600
    case 196: Cmd_OnSelectionMakeWeaponclip(); return true;   // Make Weapon Clip     0x426240
    case 197: Cmd_OnSelectionMakeNonColliding(); return true;   // Make Non-Colliding   0x426280
    case 33223: Cmd_OnSelectionMakeSplitCoplanar(); return true;   // Make Split Coplanar  0x4262C0
    case 33224: Cmd_OnSelectionMakeDontSplitCoplanar(); return true;   // Make Don't Split Coplanar 0x426300
    case 32953: Cmd_OnViewCenter(); return true;   // View->Center     0x423C50
    case 32954: Cmd_OnViewUpfloor(); return true;   // View->Up Floor   0x424700
    case 32955: Cmd_OnViewDownfloor(); return true;   // View->Down Floor 0x423ED0
    case 32974: Cmd_OnTexturesShowinuse(); return true;   // Textures->Show In Use 0x424B20
    case 32973: Cmd_OnTexturesShowall(); return true;   // Textures->Show All    0x42B440
    case 33017: Cmd_OnViewEntity(); return true;   // inspector->Entity  0x423F00
    case 33018: Cmd_OnViewTextureMode(); return true;   // inspector->Texture 0x424440
    case 33016: Cmd_OnViewConsole(); return true;   // inspector->Console 0x423E10
    case 33104: Cmd_OnFilterDlg(); return true;   // inspector->Filters 0x42B7A0
    case 33001: Cmd_OnSelectionClone(); return true;   // Selection->Clone 0x425480
    case 32956: Cmd_OnBrushFlipx(); return true;   // Brush->Flip->X  0x4250A0
    case 32957: Cmd_OnBrushFlipy(); return true;   // Brush->Flip->Y  0x4250C0
    case 32958: Cmd_OnBrushFlipz(); return true;   // Brush->Flip->Z  0x4250E0
    case 32959: Cmd_OnBrushRotatex(); return true;   // Brush->Rotate->X 0x425100
    case 32960: Cmd_OnBrushRotatey(); return true;   // Brush->Rotate->Y 0x425190
    case 32961: Cmd_OnBrushRotatez(); return true;   // Brush->Rotate->Z 0x425220
    case 33135: Cmd_OnTextureFlipX(); return true;   // Brush_FlipTexture(0)  0x42BF40
    case 33136: Cmd_OnTextureFlipY(); return true;   // Brush_FlipTexture(1)  0x42BF50
    case 33182: Cmd_OnTextureRotate90(); return true;   // Brush_RotateTexture   0x42BF60
    case 33238: Cmd_OnEditLayerCycle(); return true;   // cycle material layer  0x424010
    case 33140: Cmd_ToggleLockPatchVertMode(); return true;   // lock patch verts   0x42B4F0
    case 33139: Cmd_ToggleUnlockPatchVertMode(); return true;   // unlock patch verts 0x42B510
    case 33141: Cmd_OnCycleTerrainEdge(); return true;   // cycle terrain edge 0x42B530
    case 32781: Cmd_OnViewChange(); return true;   // View->Change (XY view cycle) 0x426400
    case 36125: Cmd_OnShowRegionsForSelected(); return true;   // light-preview regions 0x4241E0
    case 33955: Cmd_OnSetAsActiveLayer(); return true;   // Layers set-active (EMPTY) 0x42BFD0
    case 32909: Cmd_OnViewEntitiesasBoundingbox(); return true;   // entity display: bounding box
    case 32916: Cmd_OnViewEntitiesasWireframe(); return true;   // entity display: wireframe
    case 32911: Cmd_OnViewEntitiesasSelectedwireframe(); return true;   // entity display: sel wireframe
    case 32912: Cmd_OnViewEntitiesasSelectedskinned(); return true;   // entity display: sel skinned
    case 32913: Cmd_OnViewEntitiesasSkinned(); return true;   // entity display: skinned
    case 32914: Cmd_OnViewEntitiesasSkinnedandboxed(); return true;   // entity display: skinned+boxed
    case 33186: Cmd_OnMiscMayaExport(); return true;   // Misc->Maya Export 0x491B20
    case 36109: Cmd_OnErrorFile(); return true;   // File->Error file  0x423B40
    case 32967: Cmd_OnPointfileOpen(); return true;   // File->Pointfile   0x423B20
    case 33024: Cmd_OnMiscNextleakspot(); return true;   // Next leak spot    0x424BC0
    case 33025: Cmd_OnMiscPreviousleakspot(); return true;   // Prev leak spot    0x424BE0
    case 33037: Cmd_OnMiscPrintxy(); return true;   // Misc->Print XY    0x424C00
    case 33087: Cmd_OnSelectionPrint(); return true;   // Selection->Print  0x429110
    case 33232: Cmd_OnRenderMethodMaterial(); return true;   // Material_SetMode(0)
    case 33233: Cmd_OnRenderMethodLightmap(); return true;   // Material_SetMode(1)
    case 36100: Cmd_OnRenderMethodSmoothing(); return true;   // Material_SetMode(2)
    case 33226: Cmd_OnTextureFilterNearest(); return true;   // r_textureMode nearest     0x424200
    case 33227: Cmd_OnTextureFilterLinear(); return true;   // r_textureMode linear      0x424250
    case 33228: Cmd_OnTextureFilterBilinear(); return true;   // r_textureMode bilinear    0x4242A0
    case 33229: Cmd_OnTextureFilterTrilinear(); return true;   // r_textureMode trilinear   0x4242F0
    case 33230: Cmd_OnTextureFilterAnisotropic(); return true;   // r_textureMode anisotropic 0x424380
    case 33204: Cmd_OnTextureRefresh(); return true;   // Textures->Refresh (F5) 0x428B50
    case 32894: Cmd_OnTexturesTexturewindowscale200(); return true;   // tex window scale 200% 0x42B020
    case 32895: Cmd_OnTexturesTexturewindowscale100(); return true;   // 100% 0x42B000
    case 32896: Cmd_OnTexturesTexturewindowscale50(); return true;   // 50%  0x42B060
    case 32897: Cmd_OnTexturesTexturewindowscale25(); return true;   // 25%  0x42B040
    case 32898: Cmd_OnTexturesTexturewindowscale10(); return true;   // 10%  0x42AFE0
    case 35008: Cmd_OnToggleLayeredMaterials(); return true;   // layered materials show/hide 0x42BFE0
    case 35009: Cmd_OnSaveLayeredMaterials(); return true;   // layered materials save      0x42C020
    case 32856: Cmd_OnCurveSimplepatchmesh(); return true;   // Simple Patch Mesh     0x429A20
    case 32939: Cmd_OnCurveSimpleterrainpatch(); return true;   // Simple Terrain Patch
    case 32859: Cmd_OnCurvePatchtube(); return true;   // Patch primitives: cylinder
    case 32860: Cmd_OnCurvePatchcone(); return true;   // Patch primitives: cone
    case 32861: Cmd_OnCurvePatchendcap(); return true;   // Patch primitives: end cap
    case 32862: Cmd_OnCurvePatchbevel(); return true;   // Patch primitives: bevel
    case 32891: Cmd_OnCurvePatchsquare(); return true;   // Patch primitives: square cylinder
    case 32920: Cmd_OnCurveSquareBevel(); return true;   // Patch primitives: square bevel
    case 32921: Cmd_OnCurveSquareEndcap(); return true;   // Patch primitives: square end cap
    case 32874: Cmd_OnCurveInsertcolumn(); return true;   // Insert (2) Columns
    case 32873: Cmd_OnCurveInsertAddcolumn(); return true;   // Add (2) Columns
    case 32876: Cmd_OnCurveInsertrow(); return true;   // Insert (2) Rows
    case 32875: Cmd_OnCurveInsertAddrow(); return true;   // Add (2) Rows
    case 32877: Cmd_OnCurveDeleteFirstcolumn(); return true;   // Delete First (2) Columns
    case 32878: Cmd_OnCurveDeleteLastcolumn(); return true;   // Delete Last (2) Columns
    case 32879: Cmd_OnCurveDeleteFirstrow(); return true;   // Delete First (2) Rows
    case 32880: Cmd_OnCurveDeleteLastrow(); return true;   // Delete Last (2) Rows
    case 32881: Cmd_OnCurveNegative(); return true;   // Patch->Negative       0x42A7E0
    case 32906: Cmd_OnCurveMatrixTranspose(); return true;   // Patch->Transpose      0x42B1E0
    case 32890: Cmd_OnPatchNaturalize(); return true;   // Patch->Naturalize     0x42AE10
    case 33101: Cmd_OnSelectionInvert(); return true;   // Selection->Invert     0x42B6F0
    case 32889: Cmd_OnCurveRedisperseCols(); return true;   // Redisperse Columns    0x42AD80
    case 32888: Cmd_OnCurveRedisperseRows(); return true;   // Redisperse Rows       0x42AD90
    case 32899: Cmd_OnCurveNegativeTextureX(); return true;   // Negative Texture X    0x42A7F0
    case 32903: Cmd_OnCurveNegativeTextureY(); return true;   // Negative Texture Y    0x42A800
    case 32883: Cmd_OnCurvePatchdensetube(); return true;   // Dense Cylinder        0x42AB90
    case 32884: Cmd_OnCurvePatchverydensetube(); return true;   // Very Dense Cylinder   0x42AC40
    case 32905: Cmd_OnCurveCyclecap(); return true;   // Cycle Cap Texture     0x42B1A0
    case 33153: Cmd_OnAddTerrainRowColumn(); return true;   // Add Terrain Row/Column 0x42B080
    case 206: Cmd_OnDeleteExportables(); return true;   // Misc->Delete exportables 0x424E30
    case 1085: Cmd_OnLinkKeepSelection(); return true;   // LinkSelectionToggle      0x423EE0
    case 32776: Cmd_ToggleCamera(); return true;   // camera-update preview flip 0x423A50
    case 32782: Cmd_OnViewCameraupdate(); return true;   // View->Camera update        0x426450
    case 32863: Cmd_OnCurvePatchinvertedendcap(); return true;   // Inverted End Cap (EMPTY) 0x42A500
    case 32864: Cmd_OnCurvePatchinvertedbevel(); return true;   // Inverted Bevel   (EMPTY) 0x42A4F0
    case 32867: Cmd_OnCurveInsertrowSingle(); return true;   // IncPatchRow     0x42A5B0
    case 32868: Cmd_OnCurveInsertcolumnSingle(); return true;   // IncPatchColumn  0x42A560
    case 32869: Cmd_OnCurveDeleterowSingle(); return true;   // DecPatchRow     0x42A650
    case 32870: Cmd_OnCurveDeletecolumnSingle(); return true;   // DecPatchColumn  0x42A600
    case 32925: Cmd_OnHideUnselected2(); return true;   // HideByClassname 0x42B6B0
    case 32978: Cmd_OnMiscBenchmark(); return true;   // Misc->Benchmark (EMPTY) 0x424B70
    case 33089: Cmd_OnPatchTab(); return true;   // Patch TAB       0x42A9E0
    case 33090: Cmd_OnPatchEnter(); return true;   // Patch ENTER (EMPTY) 0x42A9D0
    case 33165: Cmd_OnVertexSelectUp(); return true;   // Vertex Select Up    0x426880
    case 33166: Cmd_OnVertexSelectDown(); return true;   // Vertex Select Down  0x4268A0
    case 33167: Cmd_OnVertexSelectRight(); return true;   // Vertex Select Right 0x4268C0
    case 33168: Cmd_OnVertexSelectLeft(); return true;   // Vertex Select Left  0x4268E0
    case 33170: Cmd_OnRedistPatchPoints(); return true;   // RedisperseVertices  0x42A270
    case 33179: Cmd_OnTurnTerrainEdges(); return true;   // AutoEdgeTurn        0x4294E0
    case 33213: Cmd_OnDropPatchVertices(); return true;   // DropVertices        0x42AE00
    case 36110: Cmd_OnSelectTargettedEntity(); return true;   // SelectTargettedEntities 0x425560
    case 57602: Cmd_OnFileClose(); return true;   // File->Close         (EMPTY) 0x423A70
    case 57607: Cmd_OnFilePrint(); return true;   // File->Print         (EMPTY) 0x423B60
    case 57609: Cmd_OnFilePrintPreview(); return true;   // File->Print Preview (EMPTY) 0x423B70
    case 32885: Cmd_OnCurveCap(); return true;   // Curve->Cap          0x42AD40
    case 35040: Cmd_OnPatchCap(); return true;   // Patch->Cap          0x42AE50
    case 33158: Cmd_OnSplitPatch(); return true;   // Curve->Split        0x42B0C0
    case 33192: Cmd_ExtrudeTerrainRow2(); return true;   // Extrude Row/Col     0x42B0A0
    case 33154: Cmd_OnRemoveTerrainRowColumn(); return true;   // Remove Row/Col      0x42B0B0
    case 35041: Cmd_OnCurveToTerrain(); return true;   // Curve->Terrain      0x429B30
    case 36102: Cmd_OnFaceToTerrain(); return true;   // Face->Terrain       0x429BE0
    case 33021: Cmd_OnSelectionConnect(); return true;   // Selection->Connect  0x425510
    case 33971: Cmd_OnSelectNames(); return true;   // View->Show->Names        0x42BA40
    case 33972: Cmd_OnSelectAngles(); return true;   // View->Show->Angles       0x42BAC0
    case 33973: Cmd_OnSelectBlocks(); return true;   // View->Show->Blocks       0x42BB00
    case 33974: Cmd_OnSelectConnections(); return true;   // View->Show->Connections  0x42BBC0
    case 33975: Cmd_OnSelectCoordinates(); return true;   // View->Show->Coordinates  0x42BB60
    case 36127: Cmd_OnSelectReverseFilter(); return true;   // View->Show->Reverse Filter 0x42BC20
    case 33151: Cmd_OnDisassociateEntities(); return true;   // DisassociateEntities 0x428E10
    case 33152: Cmd_OnSelectedAssociated(); return true;   // SelectedAssociated   0x428E20
    case 9: Cmd_OnScriptGroup_01(); return true;   // ScriptGroup Add Color   0x4264A0
    case 10: Cmd_OnScriptGroup_Disassociate(); return true;   // ScriptGroup Disassociate 0x426460
    case 200: Cmd_OnScriptGroup(); return true;   // Script group menu item  0x424E20
    case 33150: Cmd_OnAssociateEntities(); return true;   // AssociateEntities accel 0x428E00
    case 33145: Cmd_OnLightShiftUp(); return true;   // light value x1.1  0x428E30
    case 33146: Cmd_OnLightShiftDown(); return true;   // light value x0.9  0x428E50
    case 33176: Cmd_OnCyclinderHeightUp(); return true;   // cyl height x1.1   0x428E70
    case 33177: Cmd_OnCyclinderHeightDown(); return true;   // cyl height x0.9   0x428E90
    case 33055: Cmd_OnCameraUp(); return true;   // camera +32Z  0x426900
    case 33056: Cmd_OnCameraDown(); return true;   // camera -32Z  0x426680
    case 33061: Cmd_OnCameraAngleUp(); return true;   // pitch +22.5  0x4265D0
    case 33062: Cmd_OnCameraAngleDown(); return true;   // pitch -22.5  0x426590
    case 33147: Cmd_OnOverBrightShiftUp(); return true;   // overbright -0.05 0x428EB0
    case 33148: Cmd_OnOverBrightShiftDown(); return true;   // overbright +0.05 0x428EE0
    case 33057: Cmd_OnCameraLeft(); return true;   // strafe-yaw left  0x426720
    case 33058: Cmd_OnCameraRight(); return true;   // strafe-yaw right 0x426770
    case 33059: Cmd_OnCameraForward(); return true;   // move forward     0x4266B0
    case 33060: Cmd_OnCameraBack(); return true;   // move back        0x426610
    case 33063: Cmd_OnCameraStrafeleft(); return true;   // strafe left      0x4267C0
    case 33064: Cmd_OnCameraStraferight(); return true;   // strafe right     0x426820
    case 33065: Cmd_OnGridToggle(); return true;   // toggle grid      0x426930
    case 33083: Cmd_OnGridNext(); return true;   // grid size up     0x4289A0
    case 33084: Cmd_OnGridPrev(); return true;   // grid size down   0x4289D0
    case 33072: Cmd_OnSelectionTextureSnapDec(); return true;   // tex step dec 0x4287B0
    case 33073: Cmd_OnSelectionTextureSnapInc(); return true;   // tex step inc 0x428830
    case 33074: Cmd_OnSelectionTextureFitUnk(); return true;   // texture fit      0x4287D0
    case 33234: Cmd_OnTextureFitAll(); return true;   // texture fit all  0x428800
    case 33075: Cmd_OnTexRotateClockwise(); return true;   // tex rotate CW    0x428850
    case 33076: Cmd_OnTexRotateCounterCW(); return true;   // tex rotate CCW   0x428860
    case 33079: Cmd_OnTexShiftLeft(); return true;   // tex shift left   0x4288E0
    case 33080: Cmd_OnTexShiftRight(); return true;   // tex shift right  0x428910
    case 33081: Cmd_OnTexShiftUp(); return true;   // tex shift up     0x428870
    case 33082: Cmd_OnTexShiftDown(); return true;   // tex shift down   0x4288A0
    case 32998: Cmd_OnViewZ100(); return true;   // Z 100% (EMPTY)   0x424740
    case 32999: Cmd_OnViewZzoomin(); return true;   // Z zoom in        0x424A00
    case 33000: Cmd_OnViewZzoomout(); return true;   // Z zoom out       0x424A40
    case 32819: Cmd_OnViewCubeout(); return true;   // cubic-clip out   0x428F50
    case 32820: Cmd_OnViewCubein(); return true;   // cubic-clip in    0x428F10
    case 32772: Cmd_OnViewXy(); return true;   // View->Layout XY  0x424710
    case 32774: Cmd_OnViewYz(); return true;   // View->Layout YZ  0x423FB0
    case 32773: Cmd_OnViewXz(); return true;   // View->Layout XZ  0x424A80
    case 32797: Cmd_OnToggleviewYz(); return true;   // Toggle YZ (EMPTY) 0x427230
    case 32798: Cmd_OnToggleviewXz(); return true;   // Toggle XZ (EMPTY) 0x427220
    case 33068: Cmd_OnToggleconsole(); return true;   // View->Toggle->Console 0x426A90
    case 33069: Cmd_OnTogglecamera(); return true;   // View->Toggle->Camera  0x426A40
    case 33070: Cmd_OnTogglez(); return true;   // View->Toggle->Z       0x426B30
    case 33071: Cmd_OnToggleview(); return true;   // View->Toggle->XY      0x426AE0
    case 32789: Cmd_OnViewNextview(); return true;   // Next view (Ctrl+Tab)  0x426DB0
    case 32830: Cmd_OnToolbarMain(); return true;   // toolbar Main    (EMPTY) 0x4290F0
    case 32832: Cmd_OnToolbarTexture(); return true;   // toolbar Texture (EMPTY) 0x429100
    case 33108: Cmd_OnCenter2DOnCamera(); return true;   // center 2D on camera 0x42A2D0
    case 33160: Cmd_OnSelectNext(); return true;   // select next 0x423C90
    case 33161: Cmd_OnSelectPrev(); return true;   // select prev 0x423CA0
    case 32829: Cmd_OnSelectionMovedown(); return true;   // move selection down 0x429050
    case 32831: Cmd_OnSelectionMoveup(); return true;   // move selection up   0x4290B0
    case 33157: Cmd_OnSplay(); return true;   // Splay              0x4254F0
    case 33210: Cmd_OnSetViewToEntity(); return true;   // set view to entity 0x425540
    case 33211: Cmd_OnLinkSelected(); return true;   // link selected      0x425500
    case 33178: Cmd_OnDistanceBetweenEntities(); return true;   // get distance       0x4294D0
    case 33093: Cmd_OnSelectAllOfType(); return true;   // select all of type 0x42B470
    case 33212: Cmd_OnSelectAllOfTypeRecursive(); return true;   // select all recurse 0x42B4B0
    case 32923: Cmd_OnHideSelected(); return true;   // hide selected      0x42B6A0
    case 32934: Cmd_OnHideUnselected(); return true;   // hide unselected    0x42B6C0
    case 32924: Cmd_OnShowHidden(); return true;   // show hidden        0x42B6D0
    case 33246: Cmd_OnShowLastHidden(); return true;   // show last hidden   0x42B6E0
    case 33100: Cmd_OnViewCrosshair(); return true;   // toggle crosshairs  0x42B690
    case 33103: Cmd_OnSelectionNoOutline(); return true;   // no outline         0x425630
    case 33172: Cmd_OnSelectionNoTint(); return true;   // no tint            0x425650
    case 36121: Cmd_OnSelectSameTargetname(); return true;   // same targetname    0x42ADA0
    case 36123: Cmd_OnSelectSameTarget(); return true;   // same target        0x42ADD0
    case 32844: Cmd_OnFileImportmap(); return true;   // File->Load (merge) 0x429290
    case 32845: Cmd_OnFileExportmap(); return true;   // File->Save Selected 0x4293A0
    case 32827: Cmd_OnFileSaveregion(); return true;   // File->Save Region   0x429020
    case 33950: Cmd_OnEnableLightPreview(); return true;   // enable light preview  0x4240C0
    case 36108: Cmd_OnPreviewSun(); return true;   // preview sun as well   0x424060
    case 33951: Cmd_OnStartPreviewSelected(); return true;   // start preview selected 0x424120
    case 33952: Cmd_OnStopPreviewSelected(); return true;   // stop preview selected  0x424170
    case 33953: Cmd_OnClearPreviewList(); return true;   // clear preview list     0x4241C0
    case 36122: Cmd_OnPreviewAtMaxIntensity(); return true;   // preview at max intensity 0x425670
    case 33013: Cmd_OnTextureBackground(); return true;   // DoColor(0)  0x424E40
    case 33014: Cmd_OnColorsXyBackground(); return true;   // DoColor(1)  0x424EC0
    case 33020: Cmd_OnColorsMinor(); return true;   // DoColor(2)  0x424EA0
    case 33019: Cmd_OnColorsMajor(); return true;   // DoColor(3)  0x424E80
    case 209: Cmd_OnColorsCameraBack(); return true;   // DoColor(4)  0x424E60
    case 32803: Cmd_OnColorsGridblock(); return true;   // DoColor(7)  0x427320
    case 32799: Cmd_OnColorsGridText(); return true;   // DoColor(8)  0x4272C0
    case 32800: Cmd_OnColorsBrush(); return true;   // DoColor(9)  0x427240
    case 32801: Cmd_OnColorsSelectedbrush(); return true;   // DoColor(10) 0x4272E0
    case 33171: Cmd_OnColorsSelectedbrushCamera(); return true;   // DoColor(11) 0x427300
    case 32802: Cmd_OnColorsClipper(); return true;   // DoColor(12) 0x427280
    case 32804: Cmd_OnColorsViewname(); return true;   // DoColor(13) 0x427340
    case 33131: Cmd_OnColorsDetailBrush(); return true;   // DoColor(14) 0x427360
    case 33149: Cmd_OnColorsToggleDrawSurfs(); return true;   // DoColor(15) 0x427380
    case 33137: Cmd_OnColorsSelfaceCamera(); return true;   // DoColor(16) 0x4273E0
    case 189: Cmd_OnColorsFuncGroup(); return true;   // DoColor(17) 0x427400
    case 211: Cmd_OnColorsFuncCullGroup(); return true;   // DoColor(18) 0x427420
    case 195: Cmd_OnColorsWeaponclip(); return true;   // DoColor(19) 0x4273A0
    case 212: Cmd_OnColorsSizeInfo(); return true;   // DoColor(20) 0x427440
    case 213: Cmd_OnColorsModel(); return true;   // DoColor(21) 0x427460
    case 208: Cmd_OnColorsUnknown208(); return true;   // DoColor(22) 0x4273C0
    case 33194: Cmd_OnColorsWireframe(); return true;   // DoColor(23) 0x4272A0
    case 35020: Cmd_OnColorsFrozenLayers(); return true;   // DoColor(24) 0x427260
    case 33036: Cmd_OnMiscSelectentitycolor(); return true;   // Select Entity Color 0x424C10
    case 32805: Cmd_OnThemeQ4(); return true;   // Themes->QE4 Original       0x427480
    case 32806: Cmd_OnThemeQ3(); return true;   // Themes->Q3Radiant Original 0x427760
    case 32807: Cmd_OnThemeBlackGreen(); return true;   // Themes->Black and Green    0x427A40
    case 33143: Cmd_OnThemeInverted(); return true;   // Themes->Inverted           0x427D30
    case 210: Cmd_OnThemeGrey(); return true;   // Themes->Gray               0x428040
    case 32854: Cmd_OnDynamicLighting(); return true;   // Dynamic Lighting - U-CMD-1 no-op, see the hazard note above

    // Grid menu, 11 sizes (ON_COMMAND_EX) 0x424AB0
    case 35021:
    case 35022:
    case 35023:
    case 35024:
    case 35025:
    case 35026:
    case 35027:
    case 35029:
    case 35031:
    case 35032:
    case 35033:
        Cmd_OnGridSize( cmdId ); return true;
    // Textures->Render Method radio 0x4243D0
    case 32990:
    case 32991:
    case 32992:
    case 32993:
    case 32994:
        Cmd_OnRendermethodCaseTextures( cmdId ); return true;
    // Textures->Texture Resolution 0x424340
    case 36115:
    case 36116:
    case 36117:
    case 36118:
        Cmd_OnTextureResolution( cmdId ); return true;

    // ------------------------------------------------------------------------
    // U-CMD-2: the File-menu map flows.  The bodies are the free functions above
    // (mainfrm.cpp's "File / Edit command wrappers" block); the MFC members forward to the
    // same ones, so both shells run one flow.  Exit posts WM_CLOSE and the CLOSE handler
    // runs the discard prompt - faithful to 0x422220, where the guard lives.
    // ------------------------------------------------------------------------
    case ID_FILE_NEW: Radiant_FileNew(); return true;   // File->New            0x423AA0
    case ID_FILE_OPEN: Radiant_FileOpen(); return true;   // File->Open           0x423AE0
    case ID_FILE_SAVE: Radiant_FileSave(); return true;   // File->Save
    case ID_FILE_SAVEAS_RAD: Radiant_FileSaveAs(); return true;   // File->Save As        (SaveAsDialog 0x49A760)
    case ID_FILE_EXIT_RAD: Radiant_FileExit(); return true;   // File->Exit

    // ------------------------------------------------------------------------
    // U-CMD-2: the dialog commands, each onto its ImGui panel's toggle.  Where the MFC
    // Toggle/Show stamped g_nUpdateBits |= 1 (the repaint behind a dialog that had just
    // covered the views), the stamp is reproduced here.
    // ------------------------------------------------------------------------
    case 32812: ImGuiPanel_FindTex_Toggle(); return true;   // Textures->Replace All   0x428B40
    case 33954: ImGuiPanel_Layers_Toggle(); g_nUpdateBits |= 1; return true;   // Layers dialog           0x42BD10
    case 36106: ImGuiPanel_DynEnt_Toggle(); g_nUpdateBits |= 1; return true;   // Misc->Dyn Entities      0x42BD90
    case 33221: ImGuiPanel_Vehicle_Toggle(); g_nUpdateBits |= 1; return true;   // Misc->Vehicle Group     0x42BD50
    case 33240: ImGuiPanel_Model_Toggle(); g_nUpdateBits |= 1; return true;   // Replace Models          0x42BF00
    case 33199: ImGuiPanel_VertEdit_Toggle(); g_nUpdateBits |= 1; return true;   // Vertex Edit ('G')       0x42BCD0
    case 32786: ImGuiPanel_MapInfo_Toggle(); return true;   // Edit->Map Info          0x426C60
    case 33130: ImGuiPanel_AdvPatch_Toggle(); g_nUpdateBits |= 1; return true;   // Patch->Advanced Edit    0x42BC90
    case 32784: ImGuiPanel_Prefs_Toggle(); return true;   // Edit->Preferences       0x426950
    case 32790: ImGuiPanel_Commands_Toggle(); return true;   // Help->Command list
    case 33023: ImGuiPanel_FindBrush_Toggle(); return true;   // Misc->Find brush        0x424B96
    case 33107: ImGuiPanel_GoTo_Toggle(); return true;   // Misc->Go to position    0x424BB9
    case 33033: ImGuiPanel_ArbRotate_Toggle(); return true;   // Arbitrary rotation      0x425300
    case 32809: ImGuiPanel_Scale_Toggle(); return true;   // Selection->Scale        0x4283D0
    case 32904: ImGuiPanel_Thicken_Toggle(); return true;   // Curve->Thicken          0x42B0D0

    // Help->About (0x4264F0).  CAboutDlg is three static labels + an OK button, so the raw
    // twin is one plain-Win32 MessageBoxA carrying the same three strings (win_dlg.cpp:607-611)
    // under the same caption.
    case 33038:
        ::MessageBoxA( g_qeglobals.d_hwndMain,
                       "Kisak Radiant\n\n"
                       "AI Decompiled and based on Call of Duty 4's map editor (CoD4Radiant)\n\n"
                       "Special Thanks to xoxor4d for providing his IDA file, which had at least "
                       "60% of the functions named by hand!",
                       "About CoD4Radiant", MB_OK | MB_ICONINFORMATION );
        return true;

    // Edit->Enter / Leave Prefab (0x42BF70 / 0x42BF80).  Both bodies live in map.cpp beside
    // their Prefab_NextLevel / Prefab_PrevLevel cores; Leave prompts through
    // Radiant_ConfirmModified when the prefab is dirty.
    case 33173: Radiant_PrefabEnter(); return true;
    case 33174: Radiant_PrefabLeave(); return true;

    // 33091 SelectSnapPointsToGrid (Ctrl+G, 0x42AE90): with a selection, snap the patch
    // points; with NOTHING selected the original pops the go-to-position dialog - the panel
    // toggle is that dialog here.
    case 33091:
        if ( selected_brushes.next == &selected_brushes )
            ImGuiPanel_GoTo_Toggle();
        else
            Cmd_SnapPatchVertsToGrid();
        return true;

    // The four Alt+arrow nudges (0x429510/530/550/4F0).  NudgeSelection's CMainFrame* arg was
    // only ever the ->Nudge receiver, and that body now lives in NudgeSelection_Apply
    // (select.cpp:4541), so the frame drops out entirely.
    case 32847: NudgeSelection_Apply( 0, grid_sizes[g_qeglobals.d_gridsize] ); return true;   // left
    case 32848: NudgeSelection_Apply( 2, grid_sizes[g_qeglobals.d_gridsize] ); return true;   // right
    case 32849: NudgeSelection_Apply( 1, grid_sizes[g_qeglobals.d_gridsize] ); return true;   // up
    case 32850: NudgeSelection_Apply( 3, grid_sizes[g_qeglobals.d_gridsize] ); return true;   // down

    // ------------------------------------------------------------------------
    // POST-RIP: still UI-bound, and the widget each one needs does not exist in the raw
    // shell yet.  Reason per line; nothing here is a missing port - each body exists and
    // works in the MFC build, it just cannot run without its widget.  The TOOLBAR block is
    // the bulk of it and unblocks only when an ImGui toolbar ships (RADIANT_UI_REWORK_PLAN.md
    // U-BOOT v2); the m_pActiveXY clipper gates go with the XY-view sweep, the HandlePopup
    // trio with U-MENU.
    // ------------------------------------------------------------------------
    case 32791: return false;   // POST-RIP: OnFileNewproject               CFileDialog (MFC)
    case 35018: return false;   // POST-RIP: OnSetStartupProject            AfxGetApp()/CString profile access
    // CLIPPER + MOUSE-ROTATE — cores ported to Ed_ActiveXY()/xywndState_t (no MFC view).
    // X toggles clip mode (cancelling rotate); Enter / Shift+Enter / Ctrl+Enter commit/split/
    // flip; R toggles free mouse-rotation (cancelling clip). Ed_Clip self-gates on g_bClipMode;
    // Ed_SplitClip bails on empty split lists. (Toolbar check-button state lands with the ImGui toolbar.)
    case 32783: Cmd_OnViewClipper();       return true;   // ToggleClipper   (X)
    case 32795: Ed_Clip( Ed_ActiveXY() );  return true;   // ClipSelected    (Enter)
    case 32794: Ed_SplitClip();            return true;   // SplitSelected   (Shift+Enter)
    case 32796: Ed_FlipClip();             return true;   // FlipClip        (Ctrl+Enter)
    case 32810: Cmd_OnSelectMouserotate(); return true;   // MouseRotate     (R)
    case 33034: return false;   // POST-RIP: OnBrushArbitrarysided          AfxGetInstanceHandle + g_pParentWnd->m_hWnd modal
    case 32833: return false;   // POST-RIP: OnBrushMakecone                same modal sides dialog
    case 32892: return false;   // POST-RIP: OnBrushPrimitivesSphere        same modal sides dialog
    // ── TOOLBAR-STATE PREF TOGGLES — ported verbatim from the IW3xRadiant IDA (handler EA in
    //    each comment). Every one is a g_PrefsDlg field flip + Prefs_SavePrefs + the binary's own
    //    repaint; the ONLY dropped line is the m_wndToolBar TB_CHECKBUTTON reflect-state
    //    SendMessage (cosmetic — the button glyph lands with the ImGui toolbar). ──
    case 32936: if ( ++g_PrefsDlg->camera_mode > 2 ) g_PrefsDlg->camera_mode = 0;
                Prefs_SavePrefs( g_PrefsDlg ); return true;                              // ToggleCameraMovementMode (0x429eb0)
    case 32852: g_PrefsDlg->m_bSelectCurves ^= 1;      Prefs_SavePrefs( g_PrefsDlg ); return true;   // Dontselectcurve      (0x429920)
    case 32858: g_PrefsDlg->g_bPatchWeld ^= 1;         Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; return true;   // PatchWeld            (0x42a400)
    case 32865: g_PrefsDlg->patch_drill_down ^= 1;     Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; return true;   // PatchDrilldown       (0x42a510)
    case 33138: g_PrefsDlg->camera_masked ^= 1;        Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits |= 1; return true;   // ToggleTexAlphaRender (0x429f10)
    case 33142: g_PrefsDlg->entities_off ^= 1;         Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits |= 1; return true;   // DisableSelOfEntities (0x429f60)
    case 33169: g_PrefsDlg->sky_brush_off ^= 1;        Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits |= 1; return true;   // DisableSelOfSky      (0x429fb0)
    case 33156: g_PrefsDlg->m_bSelectableModels ^= 1;  Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; return true;   // SelectableModels     (0x42a280)
    case 33159: g_PrefsDlg->m_bDropModel ^= 1;         Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; return true;   // PlantModel           (0x42a0e0)
    case 33206: g_PrefsDlg->m_bForceZeroDropHeight ^= 1; Prefs_SavePrefs( g_PrefsDlg ); return true;                     // ForceZeroDropHeight  (0x42a000)
    case 33195: g_PrefsDlg->m_bOrientModel ^= 1;       Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; return true;   // OrientToFloor        (0x4258f0)
    case 33207: g_PrefsDlg->m_bVertSnapModel ^= 1;     Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; return true;   // VertSnapModel        (0x42a180)
    case 33208: g_PrefsDlg->m_bVertSnapBrush ^= 1;     Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; return true;   // VertSnapBrush        (0x42a1d0)
    case 33209: g_PrefsDlg->m_bVertSnapPrefab ^= 1;    Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits = -1; return true;   // VertSnapPrefab       (0x42a220)
    // Cubic clipping (0x428f90): flip + tick the (still-present) native menu item + repaint.
    case 32817: g_PrefsDlg->m_bCubicClipping ^= 1;
                Radiant_CheckMenu( 32817, g_PrefsDlg->m_bCubicClipping != 0 );
                Prefs_SavePrefs( g_PrefsDlg ); g_nUpdateBits |= 1; return true;          // ViewCubicclipping
    case 32857: Cmd_OnPatchWireframe();  return true;   // TogglePatchWireframes (Shift+W)
    case 33155: Cmd_OnTolerantWeld();    return true;   // TolerantWeld          (Shift+J)
    case 33144: Cmd_OnToggleDrawSurfs(); return true;   // ToggleDrawSurfs       (0x42a040)
    case 32872: Cmd_OnPatchRedisperse(); return true;   // PatchRedisperse       (0x42a990)
    case 32882: Cmd_OnPatchBend();       return true;   // PatchBend             (0x42a950)

    // STILL DEFERRED — genuinely need a widget/state-machine that does not exist yet:
    case 32813: return false;   // POST-RIP: OnSelectMousescale             mouse-scale mode + m_wndToolBar
    case 32814: return false;   // POST-RIP: OnScalelockX                   DoScaleLock toolbar state machine
    case 32815: return false;   // POST-RIP: OnScalelockY                   DoScaleLock toolbar state machine
    case 32816: return false;   // POST-RIP: OnScalelockZ                   DoScaleLock toolbar state machine
    case 35005: return false;   // POST-RIP: OnMiscCyclePreviewModels       CMenu (preview-models submenu)
    case 35042: return false;   // POST-RIP: OnDropSelectedRelativeZ        m_pActiveXY + m_wndToolBar
    case 32915: return false;   // POST-RIP: OnShowEntities                 HandlePopup( CWnd*, CMenu ) - needs U-MENU
    case 32779: return false;   // POST-RIP: OnPopupRenderMethod            HandlePopup( CWnd*, CMenu )
    case 32780: return false;   // POST-RIP: OnPopupSelection               HandlePopup( CWnd*, CMenu )
    }

    return false;   // unknown id - nothing dispatched
}


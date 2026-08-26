#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// radiant_main.cpp — the plain-Win32 application entry (Phase 4 unit U-BOOT).
//
// This file replaces CRadiantApp (radiantapp.cpp) AND the shell half of CMainFrame: WinMain,
// the frame window class + WndProc, and the message pump.  It contains NO editor behaviour —
// every step below is a call into a body that already exists and that the MFC shell calls
// too (mainfrm.cpp's free functions, the four *_CreateRaw viewport creators).  The unit's
// success criterion is ORDER: each step cites the MFC line it reproduces.
//
// ┌── BOOT ORDER (MFC source → raw equivalent) ────────────────────────────────────────┐
// │  radiantapp.cpp CRadiantApp::InitInstance                                          │
// │   :53  Com_Printf gate-P2 smoke line          → same call                          │
// │   :57  InitCommonControlsEx( ICC_* set )      → same call                          │
// │   :69  SetRegistryKey( "iw\\CoD4Radiant" )    → NO equivalent needed: the shim's    │
// │        (CWinApp profile registry key)            AfxGetApp()/KISAK_PROFILE_BASE     │
// │                                                  already points at that key         │
// │   :70  Prefs_Init( true )                     → same call                          │
// │   :72  new CMainFrame + Create(work rect)     → RegisterClassA + CreateWindowExA,   │
// │        (WM_CREATE → CMainFrame::OnCreate)        then Radiant_BootFrame() below     │
// │   :90  ShowWindow( m_nCmdShow )/UpdateWindow  → same calls                         │
// │   :96  RADIANT_STARTUP_MAP / _CAM             → Radiant_ApplyStartupMapEnv()        │
// │  mainfrm.cpp CMainFrame::OnCreate  (see Radiant_BootFrame for the per-step cites)   │
// │  CWinApp::Run + CRadiantApp::OnIdle           → Radiant_RunMessageLoop()            │
// └────────────────────────────────────────────────────────────────────────────────────┘
//
// DELIBERATE STRUCTURAL DIFFERENCE (documented, not a divergence in behaviour): MFC ran the
// whole OnCreate body from INSIDE WM_CREATE (CWnd::CreateEx dispatches it).  Here the frame
// window is created first and Radiant_BootFrame() runs immediately after CreateWindowExA
// returns.  Nothing in the sequence needs to happen before WM_CREATE completes, the HWND is
// the same, and the WM_SIZE storm that ::SetMenu triggers then lands on a frame whose child
// views already exist (in MFC it landed on RecalcLayout the same way).  The frame's WndProc
// tolerates messages arriving before the children exist (every handler null-checks).
#include "stdafx.h"
#include "kiwi_plastbridge.h"
#include "radiant_frame.h"             // the shell-agnostic frame API (this unit's header)
#include "xywnd.h"                     // Ed_ActiveXY / ED_VIEW_XY (U-GLOBALS)
#include "prefs.h"                     // Prefs_Init + g_PrefsDlg (the light-preview seeds)
#include <universal/profile.h>
                                       // NOTE (reported): prefs.h:131 still declares
                                       // Prefs_ShowDialog( CWnd * ), which does not compile
                                       // under KISAK_NO_MFC — it breaks EVERY includer, not
                                       // just this one.  U-GUARD owns the fence.

#include <shellapi.h>                  // KIWI-UX (ROUND BE): DragAcceptFiles / WM_DROPFILES

#include <qcommon/qcommon.h>           // Com_Printf
#include <gfx_d3d/r_init.h>            // dx (windowCount, for the first-light log)
#include <gfx_d3d/r_material.h>        // Material — R_BeginRegistrationInternal's return
#include <gfx_d3d/r_rendercmds.h>      // R_BeginFrame/.../R_IssueRenderCommands — the frame's
                                       // own paint (it is the ImGui dockspace surface, slot 6)

// ── ImGui shell (imgui_shell.cpp) — the frame IS the editor UI now ─────────────
extern bool ImGuiShell_HandleMessage( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam );
extern void ImGuiShell_SetPrimarySurface( HWND hwnd );
extern bool ImGuiShell_PrimaryActive();
extern void ImGuiShell_ApplyViewportDocks();
extern void ImGuiShell_RenderPlatformWindows();   // popped-out panel OS windows (post-present)
extern void ImGuiShell_BeginFrame();              // authorize this tick's single ImGui frame
extern bool ImGuiShell_FrameAuthorized();         // ROUND U: will this paint draw a scene?
extern void ImGuiShell_RenderViewportsToRT();     // Phase 5: render the 4 viewports to their RTs
extern void ImGuiShell_DispatchViewportInput();   // Phase 5: viewport mouse input (post-present)

// ── KIWI-UX (ROUND AD): device-health reporting for the frame's own paint ──────
// Declared against kiwi_devicereset.h (the definition is kiwi_devicereset.cpp); that
// header is not included here because it declares nothing else this file needs and
// carries a large analysis block.
extern void KiwiDevice_FrameHealthWatch( HWND frame, bool authorized, bool painted );  // kiwi_devicereset.cpp

// mainfrm.h is deliberately NOT included: it is the MFC class header (CMainFrame + the
// C*Wnd/C*Dlg family behind its own fences), and this shell must not depend on it —
// radiant_frame.h is the survivor U-RIP folds it into.  Everything else this boot drives is
// declared below against its owning file, the same way mainfrm.cpp declares its externs.
extern Material   *R_BeginRegistrationInternal();                // gfxwrapper.cpp (0x416510)
extern void        Load_Materials();                             // texwnd.cpp (0x45ae40)
extern unsigned long FillTextureMenu();                          // texwnd.cpp (0x45b260)
extern LRESULT     Texture_ShowAll();                            // texwnd.cpp (0x45b730)
extern void        Texture_SetMode( int iTexMenu );              // texwnd.cpp (0x45a520)
extern void        Map_NewMap();                                 // map.cpp (0x486110)
extern void        Map_New();                                    // entity.cpp (0x4870c0)
extern entity_s   *world_entity;                                 // map.cpp (0x25d5b30)
extern eclass_t   *Eclass_ForName( int has_brushes, const char *name ); // eclass.cpp
extern void        QE_CountBrushesAndUpdateStatusBar();          // qe3.cpp
extern void        Radiant_RefreshFilterPane();                  // win_ent.cpp (no-op in this shell)
extern void        console_print( const char *fmt, va_list args );          // win_qe3.cpp (0x499d80)
extern void        SetConsoleHandler( void ( *fn )( const char *, va_list ) ); // cmdlib.cpp (0x40a9e0)
extern LPMRUMENU  *CreateMruMenuDefault();                       // qe3.cpp (0x48a150)
extern void        LoadMruInReg( LPMRUMENU *mru );               // qe3.cpp (0x48a870)
extern void        MRU_InsertItem( LPMRUMENU *mru, HMENU hMenu );// qe3.cpp (0x48a400)
extern void        SaveMruInReg( LPMRUMENU *mru );               // qe3.cpp (0x48a920)

// ═════════════════════════════════════════════════════════════════════════════
//  Window/child identity
// ═════════════════════════════════════════════════════════════════════════════
static const char *const RADIANT_FRAME_CLASS = "KIWIRadiantFrame";
static const char *const RADIANT_BLANK_CLASS = "KIWIRadiantBlankPane";

// The child-window ids the MFC shell handed to CWnd::Create — literally afxres.h's
// AFX_IDW_PANE_FIRST (0xE900) + n, kept numerically identical so nothing that ever keyed
// off a pane id changes meaning.  Only the console EDIT actually needs one (its EN_*
// notifications arrive as WM_COMMAND on the frame).
enum { KIWI_IDW_PANE_FIRST = 0xE900 };   // == AFX_IDW_PANE_FIRST

static HWND   s_hwndFrame   = nullptr;
static HWND   s_hwndConsole = nullptr;   // g_qeglobals.d_hwndEdit
static HWND   s_hwndLayMat  = nullptr;   // lyrMtlWndGlob.layerList placeholder
static HACCEL s_hAccel      = nullptr;   // IDR_MAIN_ACCEL (was CMainFrame::m_hAccel)
static HMENU  s_hMenu       = nullptr;   // IDR_MENU_QUAKE3
static bool   s_bootDone    = false;     // gate the WndProc's layout/idle work during creation

// KIWI-UX (RADIANT_UX_DESIGN §11, Phase 2): re-annotate the menu bar after a keymap-profile
// switch.  s_hMenu is file-static, so kiwi_keymap.cpp cannot reach the ported
// Radiant_ShowMenuItemKeyBindings itself.  Pure forwarder — the annotation call is idempotent
// (it strips any existing "\t..." before rebuilding it, mainfrm.cpp).
void Radiant_RefreshMenuKeyBindings()
{
    if ( s_hMenu )
        Radiant_ShowMenuItemKeyBindings( s_hMenu );
}

// ═════════════════════════════════════════════════════════════════════════════
//  Layout — CMainFrame::RelayoutPanes (mainfrm.cpp:1662) minus the two docked MFC bars.
//  The MFC body subtracted the status-bar height and passed g_texBarHeight as the topInset;
//  neither bar exists in this shell (see the SKIPPED list at the bottom of this file), so
//  the full client area goes to the QE4 panes with topInset 0.  Radiant_ComputeLayout is
//  the SAME function the MFC shell calls, so the pane arrangement + the persisted splitter
//  fractions are identical.
// ═════════════════════════════════════════════════════════════════════════════
static void Radiant_RelayoutPanes()
{
    RECT rc;
    ::GetClientRect( s_hwndFrame, &rc );
    const int cx = rc.right - rc.left;
    const int cy = rc.bottom - rc.top;
    if ( cx < 64 || cy < 64 )
        return;                       // ignore minimize — keep the swap chains at their last real size

    const EdLayout L = Radiant_ComputeLayout( cx, cy, 0 );

    // mainfrm.cpp:1683-1688 — the same five MoveWindow calls, in the same order.
    // (m_pEntWnd is a FLOATING window in the MFC shell and not laid out by the dock; this
    //  shell has no entity window at all.)
    struct { HWND hwnd; const RECT *r; } panes[] = {
        { g_qeglobals.d_hwndXY,      &L.xy  },
        { g_qeglobals.d_hwndCamera,  &L.cam },
        { g_qeglobals.d_hwndZ,       &L.z   },
        { g_qeglobals.d_hwndTexture, &L.tex },
        { s_hwndConsole,             &L.con },
    };
    for ( const auto &p : panes )
    {
        if ( p.hwnd )
            ::MoveWindow( p.hwnd, p.r->left, p.r->top,
                          p.r->right - p.r->left, p.r->bottom - p.r->top, TRUE );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  The frame WndProc — the CMainFrame message map (mainfrm.cpp:219-230), minus the entries
//  that are MFC-object-bound (see the SKIPPED list).
// ═════════════════════════════════════════════════════════════════════════════
static LRESULT CALLBACK Radiant_FrameWndProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    // The ImGui shell gets first look — the frame's client area IS the dockspace (tab
    // bars, panels, splitter handles are all ImGui's). Consumed input never reaches the
    // legacy handlers below. (Same contract as the raw camera WndProc.)
    if ( ImGuiShell_HandleMessage( hwnd, msg, wParam, lParam ) )
        return 0;

    switch ( msg )
    {
    case WM_PAINT:
        // The frame's own present (slot-6 swap chain): the CZWnd::OnPaint skeleton with no
        // editor draw — the backend hook (rb_backend.cpp, pre-EndScene) submits the ImGui
        // frame while this window is the active target. Children are moved AFTER the
        // present (their WM_SIZE → R_Hwnd_Resize must not run inside the scene bracket).
        if ( ImGuiShell_PrimaryActive() )
        {
            // ── KIWI-UX (ROUND U): NEVER PRESENT A FRAME WITH NO SCENE ──────
            // USER REPORT: "There is a flicker on some actions that turns the
            // whole window white."  The clear below uses colors[1], which
            // win_qe3.cpp:415 sets to { 1, 1, 1, 1 } — pure white — and the ImGui
            // scene that is supposed to cover it is submitted by a DIFFERENT
            // function (ImGuiShell_DrawOverlay, pre-EndScene) that refuses to run
            // on any paint the pump did not authorize.  So an unauthorized
            // WM_PAINT — one caused by a nested TrackPopupMenu / MessageBox
            // message loop, a window move, an OS repaint — cleared the whole
            // client area to white and presented it empty.  The full chain is on
            // ImGuiShell_FrameAuthorized (imgui_shell.cpp).
            //
            // The fix is to do NOTHING on such a paint but validate the region:
            // under DWM the window keeps its last presented content, so the
            // correct frame simply stays on screen until the pump's next tick
            // (<= 16 ms away) draws a real one.  There is no case where clearing
            // to white and presenting is better than leaving the last good frame.
            //
            // s_framePresented is the ONE exception, and it is the boot instant:
            // before the pump has ever driven a frame there is no "last good
            // frame" to keep, so the first paint still clears — exactly the
            // pre-round-U behaviour, for exactly one paint.
            static bool s_framePresented = false;
            const bool  drawScene = ImGuiShell_FrameAuthorized() || !s_framePresented;

            PAINTSTRUCT ps;
            ::BeginPaint( hwnd, &ps );
            bool scenePainted = false;
            if ( drawScene && dx.device && R_SetupRendertarget_CheckDevice( hwnd ) )
            {
                R_BeginFrame();
                R_BeginSharedCmdList();
                R_AddCmdClearScreen( 7, g_qeglobals.d_savedinfo.colors[1], 1.0f, 0 );
                R_EndFrame();
                R_IssueRenderCommands( (uint)-1 );
                R_SortMaterials();
                R_CheckTargetWindow( hwnd );
                s_framePresented = true;
                scenePainted     = true;
            }
            ::EndPaint( hwnd, &ps );
            // ── KIWI-UX (ROUND AD): THE BLACK SCREEN NOW SAYS SOMETHING ─────
            // USER REPORT: *"I had it hang while loading a map"* — main thread
            // idle in MsgWaitForMultipleObjects, screen black, never recovers.
            // The pump was fine; the `if` above was answering FALSE on every
            // tick, forever, and printing nothing.  This is the one place that
            // knows a tick rendered nothing, so it is the one place that reports
            // it (throttled) and, after ~15 s of unbroken black, writes the
            // rescue save and tells the operator.  It MUST be after ::EndPaint:
            // the escape hatch pops a message box, whose nested pump would
            // otherwise re-deliver an unvalidated WM_PAINT forever.  `drawScene`
            // is passed so a paint the pump did not ask for — which draws nothing
            // by design since round U — is not mistaken for a black frame.
            KiwiDevice_FrameHealthWatch( hwnd, drawScene, scenePainted );
            ImGuiShell_ApplyViewportDocks();
            // NOTE: the pop-out platform windows are rendered by the PUMP (once per tick,
            // right after this synchronous paint), NOT here — calling UpdatePlatformWindows
            // from an arbitrary WM_PAINT (e.g. an OS-driven repaint, or one where the ImGui
            // frame's re-entrancy guard skipped Render) asserts inside ImGui
            // ("call Render() before UpdatePlatformWindows"). See Radiant_RunMessageLoop.
            // NO self-invalidate here — the pump drives one synchronous frame per tick
            // (see Radiant_RunMessageLoop). Re-invalidating from inside WM_PAINT keeps
            // this window permanently dirty, which starves every child's paint delivery
            // AND the pump's idle branch (the black-viewport / only-draws-while-dragging
            // failure the first build showed).
            return 0;
        }
        break;
    case WM_COMMAND:
        // The MFC shell's AFX_MSGMAP was the id->handler table; here Radiant_ExecCommand is
        // (mainfrm.cpp:5484 — it forks to Radiant_DispatchCommandDirect under KISAK_NO_MFC).
        // Menu + accelerator commands arrive with HIWORD(wParam) == 0 and lParam == 0; a
        // non-zero lParam means a CHILD CONTROL notification (the console EDIT's EN_*), which
        // is not a command id — same gate the raw camera WndProc documents (camwnd.cpp:4177).
        if ( lParam == 0 )
        {
            Radiant_ExecCommand( (unsigned int)LOWORD( wParam ) );
            return 0;
        }
        break;

    // ── KIWI-UX (ROUND BE): FILES DROPPED FROM EXPLORER ─────────────────────────
    // Armed by the DragAcceptFiles in Radiant_BootFrame.  The handler does exactly two
    // things: copy the HDROP's path list out (it is invalid the moment DragFinish returns,
    // so this cannot be deferred) and POST the work id.  Everything after that — opening the
    // wizard, decoding, writing, registering — happens on the pump's next tick, because a
    // WM_ handler can fire from inside the compositing scene bracket or a nested modal pump
    // and running an ImGui popup from there would re-enter the frame.  Same discipline as
    // round AU's entity drop (kiwi_entbrowser.cpp:946-949); the whole argument is in
    // kiwi_import.h D-BE-J.
    case WM_DROPFILES:
        {
            // Block-scope extern + a local id constant, the same shape every other KIWI
            // hook in this file takes: kiwi_command.h cannot be included here (it pulls in
            // kiwi_pick.h / kiwi_snap.h, which need qe3.h, which this shell file
            // deliberately does not take — see the mainfrm.h note above).
            extern bool      KiwiImport_HandleDropFiles( void *hDropOpaque );  // kiwi_import.h
            const unsigned int kImportDropped = 34128;                         // KIWI_CMD_IMPORT_DROPPED, kiwi_command.h
            if ( KiwiImport_HandleDropFiles( (void *)wParam ) )
                ::PostMessageA( hwnd, WM_COMMAND, (WPARAM)kImportDropped, 0 );
            return 0;
        }

    case WM_SIZE:
        // CMainFrame::OnSize (mainfrm.cpp:1750): chain the default first, then relayout.
        // The floating-window Transparent_Background tail (mainfrm.cpp:1762) is NOT
        // reproduced — it carves the frame region around m_wndStatusBar/m_wndToolBar, both
        // MFC control bars this shell does not have, and both prefs default OFF.
        {
            const LRESULT r = ::DefWindowProcA( hwnd, msg, wParam, lParam );
            if ( s_bootDone && wParam != SIZE_MINIMIZED && ImGuiShell_PrimaryActive() )
                R_Hwnd_Resize( hwnd, (int)LOWORD( lParam ), (int)HIWORD( lParam ) );
            // Dock nodes drive the children once the shell owns the frame; the EdLayout
            // relayout only serves the pre-first-ImGui-frame instant.
            if ( s_bootDone && !ImGuiShell_PrimaryActive() )
                Radiant_RelayoutPanes();
            return r;
        }

    case WM_TIMER:
        // CMainFrame::OnTimer (mainfrm.cpp:1838): timer 1 refreshes the brush/entity counts.
        // Flushing g_nUpdateBits stays RoutineProcessing's job (the idle branch of the pump).
        // Zoned because this is periodic work that lands in the message drain, between
        // frames, where an unzoned cost reads as an empty gap in the trace.  The handler is
        // change-gated (qe3.cpp), so the steady-state zone is two loads and a compare.
        if ( wParam == 1 )
        {
            PROF_SCOPED( "status count timer" );
            QE_CountBrushesAndUpdateStatusBar();
        }
        break;

    case WM_KEYDOWN:
        // CMainFrame::OnKeyDown (mainfrm.cpp:2299, IDB 0x422370) — ON_WM_KEYDOWN existed so
        // editor hotkeys still fire when the FRAME ITSELF has focus.  Keys pressed over a
        // view are pre-translated in the pump instead (see Radiant_PreTranslateMessage).
        if ( Radiant_TryHotkey( (unsigned int)wParam ) )
            return 0;
        break;

    case WM_MOUSEWHEEL:
        // U-CMD-2: the frame IS the central wheel dispatcher (CMainFrame::OnScroll 0x42B850).
        // lParam is already in SCREEN coords, which is what the hover test wants.  Children
        // that do not handle the wheel bubble it here through DefWindowProc, exactly as they
        // reached the MFC frame.  The dolly (the one wheel path only the frame served) is live
        // again; XY zoom and the texture scroll keep working through their own handlers.
        //
        // RTT: the native viewport children are HIDDEN, so this legacy hover-test dispatcher
        // can no longer find a window under the cursor (WindowFromPoint never returns a hidden
        // child) — it would fall through to the view-centered Cmd_OnViewZoomin/out.  Under the
        // ImGui primary the wheel is routed instead by ImGuiShell_DispatchViewportInput →
        // VP_Wheel (cursor-anchored, image-relative), so gate the legacy path off to avoid a
        // double zoom.  (Mirrors the WM_SETCURSOR / WM_LBUTTONDOWN splitter gates above.)
        if ( !ImGuiShell_PrimaryActive() )
        {
            POINT pt = { (LONG)(short)LOWORD( lParam ), (LONG)(short)HIWORD( lParam ) };
            if ( Radiant_OnMouseWheel( hwnd, (short)HIWORD( wParam ), pt ) )
                return 0;
        }
        break;

    // ── Legacy EdLayout gutter-splitters — dead while the dockspace owns the frame ──
    // (ImGui dock splitters replace them; these fire only in the pre-primary instant.)
    case WM_SETCURSOR:
        if ( !ImGuiShell_PrimaryActive()
             && (HWND)wParam == hwnd && LOWORD( lParam ) == HTCLIENT
             && Radiant_SplitterOnSetCursor( hwnd ) )
            return TRUE;
        break;

    case WM_LBUTTONDOWN:
        if ( !ImGuiShell_PrimaryActive()
             && Radiant_SplitterOnLButtonDown( hwnd, (short)LOWORD( lParam ), (short)HIWORD( lParam ) ) )
            return 0;
        break;

    case WM_MOUSEMOVE:
        if ( !ImGuiShell_PrimaryActive()
             && Radiant_SplitterOnMouseMove( (unsigned int)wParam,
                                             (short)LOWORD( lParam ), (short)HIWORD( lParam ) ) )
        {
            Radiant_RelayoutPanes();
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if ( !ImGuiShell_PrimaryActive() && Radiant_SplitterOnLButtonUp() )
            return 0;
        break;

    case WM_ERASEBKGND:
        return 1;                     // the panes own their client areas (D3D present / EDIT)

    case WM_CLOSE:
        // CMainFrame::OnClose (IDB 0x422220): guard on OkToDiscard() + Prefab_LevelBack()
        // before letting the frame go.  U-CMD-2 lifted that guard to
        // Radiant_FrameCloseAllowed(), so quitting with unsaved edits now prompts here too —
        // and File→Exit (Radiant_FileExit) just posts WM_CLOSE, so it goes through this one
        // guard rather than prompting twice.  User cancel = swallow the close.
        if ( !Radiant_FrameCloseAllowed() )
            return 0;
        ::DestroyWindow( hwnd );
        return 0;

    case WM_DESTROY:
        // CMainFrame::OnDestroy (mainfrm.cpp:2575, IDB 0x421C60): persist the MRU list.
        if ( g_qeglobals.d_lpMruMenu )
            SaveMruInReg( g_qeglobals.d_lpMruMenu );
        g_radiantFrameState.doLoop = false;   // stop the idle pump before the views go away
        ::PostQuitMessage( 0 );
        return 0;
    }
    return ::DefWindowProcA( hwnd, msg, wParam, lParam );
}

// The layered-material content placeholder's WndProc.  CEdBlankPane (mainfrm.h:258) exists
// only so R_BeginRegistrationInternal has a fifth HWND to assert on and attach a swap chain
// to (gfxwrapper.cpp:63-74 + layeredmaterialwnd.cpp:967); it is hidden and never painted, so
// the default processing is the whole behaviour.
static LRESULT CALLBACK Radiant_BlankPaneWndProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    if ( msg == WM_ERASEBKGND )
        return 1;
    return ::DefWindowProcA( hwnd, msg, wParam, lParam );
}

// ═════════════════════════════════════════════════════════════════════════════
//  Radiant_CreateRenderWindows — the raw twin of mainfrm.cpp:950 (same name, same order).
//  ORDER IS LOAD-BEARING: R_BeginRegistrationInternal asserts and attaches the device to all
//  five windows, and the XY view is created FIRST (xywnd.cpp:4603).  Each viewport's
//  caller-obligation block spells out the d_hwnd* registration this function owes it:
//    XY  — xywnd.cpp:4602   (d_hwndXY  + seed Ed_ActiveXY()->m_nViewType = ED_VIEW_XY)
//    Cam — camwnd.cpp:4104  (d_hwndCamera)
//    Z   — z.cpp:753        (d_hwndZ)
//    Tex — texwnd.cpp:2288  (d_hwndTexture, created AFTER XY, WS_VSCROLL supplied by the
//                            creator, W_TEXTURE drained by Radiant_UpdateWindows)
// ═════════════════════════════════════════════════════════════════════════════
static bool Radiant_CreateRenderWindows( HWND frame, const EdLayout &L )
{
    auto width  = []( const RECT &r ) { return r.right - r.left; };
    auto height = []( const RECT &r ) { return r.bottom - r.top; };

    // XY grid — first, per the caller obligation.
    g_qeglobals.d_hwndXY = XYWnd_CreateRaw( frame, L.xy.left, L.xy.top, width( L.xy ), height( L.xy ) );
    if ( !g_qeglobals.d_hwndXY )
        return false;
    Ed_ActiveXY()->m_nViewType = ED_VIEW_XY;      // mainfrm.cpp:957 (top-down at startup)

    g_qeglobals.d_hwndCamera = CamWnd_CreateRaw( frame, L.cam.left, L.cam.top, width( L.cam ), height( L.cam ) );
    if ( !g_qeglobals.d_hwndCamera )
        return false;

    g_qeglobals.d_hwndZ = ZWnd_CreateRaw( frame, L.z.left, L.z.top, width( L.z ), height( L.z ) );
    if ( !g_qeglobals.d_hwndZ )
        return false;

    g_qeglobals.d_hwndTexture = TexWnd_CreateRaw( frame, L.tex.left, L.tex.top, width( L.tex ), height( L.tex ) );
    if ( !g_qeglobals.d_hwndTexture )
        return false;

    // LayeredMaterialWnd content (mainfrm.cpp:979): a hidden minimal render shell.
    // R_BeginRegistrationInternal asserts + attaches it, so it needs a valid sized HWND;
    // keep it off-screen/hidden (WS_CHILD without WS_VISIBLE), exactly as CEdBlankPane is.
    s_hwndLayMat = ::CreateWindowExA( 0, RADIANT_BLANK_CLASS, nullptr, WS_CHILD,
                                      0, 0, 64, 64, frame, nullptr,
                                      ::GetModuleHandleA( nullptr ), nullptr );
    if ( !s_hwndLayMat )
        return false;
    lyrMtlWndGlob.layerList = s_hwndLayMat;

    // Entity inspector (d_hwndEntity): SKIPPED in this shell — CEntityWnd is the MFC
    // inspector and the ImGui panels replace it, so g_qeglobals.d_hwndEntity stays NULL.
    // Verified NULL-safe at every reader: mainfrm.cpp:3985 (Radiant_ToggleInspectorMode
    // early-returns), win_ent.cpp:885 + :1739 + :1765 (all guarded / MFC-fenced),
    // mainfrm.cpp's FL_Log line only prints it.  R_BeginRegistrationInternal does NOT
    // assert it (gfxwrapper.cpp:63-70 covers Camera/XY/Z/Texture + the layer list only).
    // CONSEQUENCE: the N/O/T/F inspector-mode hotkeys become no-ops (they toggle that
    // window's visibility).  REPORTED.

    // Console pane (d_hwndEdit) — mainfrm.cpp:1015.  The binary's is a CEditWnd whose
    // PreCreateWindow (0x40F5B0) gives it style 0x50200084: note NOT ES_READONLY, which is
    // why console_print's EM_REPLACESEL works.  CEdit::CreateEx is a wrapper over this exact
    // CreateWindowExA, so the raw form is the same window.
    {
        // CREATED HIDDEN — the binary's style MINUS WS_VISIBLE.  This is the QE3 console pane
        // (the system "EDIT" class); USER32 repaints it white on every EM_REPLACESEL, and the
        // only thing that ever hid it ran from the pump, so it was visible through every long
        // synchronous operation.  The window still exists, so console_print's EM_* sinks and
        // ImGuiShell_DrawConsoleTab keep working unchanged.
        const DWORD kConsoleStyle = WS_CHILD | WS_VSCROLL |
                                    ES_MULTILINE | ES_AUTOHSCROLL;   // 0x50200084 minus WS_VISIBLE
        s_hwndConsole = ::CreateWindowExA( WS_EX_CLIENTEDGE, "EDIT", nullptr, kConsoleStyle,
                                           L.con.left, L.con.top, width( L.con ), height( L.con ),
                                           frame, (HMENU)(uintptr_t)( KIWI_IDW_PANE_FIRST + 6 ),
                                           ::GetModuleHandleA( nullptr ), nullptr );
        if ( !s_hwndConsole )
            return false;
        g_qeglobals.d_hwndEdit = s_hwndConsole;
        // The binary sets the console font to DEFAULT_GUI_FONT (OnCreateClient tail 0x4232B0).
        ::SendMessageA( g_qeglobals.d_hwndEdit, WM_SETFONT,
                        (WPARAM)::GetStockObject( DEFAULT_GUI_FONT ), TRUE );
        // Install console_print as the editor-log sink (the binary's console_stuff =
        // &console_print), so Com_PrintMessage / Com_PrintError / R_Warn land in the pane.
        SetConsoleHandler( console_print );
    }

    Radiant_FL_Log( "windows: XY=%p Cam=%p Z=%p Tex=%p LayMat=%p Ent=%p Console=%p",
        (void *)g_qeglobals.d_hwndXY, (void *)g_qeglobals.d_hwndCamera,
        (void *)g_qeglobals.d_hwndZ, (void *)g_qeglobals.d_hwndTexture,
        (void *)lyrMtlWndGlob.layerList, (void *)g_qeglobals.d_hwndEntity,
        (void *)g_qeglobals.d_hwndEdit );
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Radiant_BootFrame — CMainFrame::OnCreate (mainfrm.cpp:1432, IDB 0x4209b0) step for step.
//  Every numbered comment is the MFC step number it reproduces; the two skipped steps keep
//  their numbers so the sequence stays diff-able against the MFC body.
// ═════════════════════════════════════════════════════════════════════════════
static bool Radiant_BootFrame( HWND frame )
{
    // (OnCreate head, mainfrm.cpp:1434-1436)
    Radiant_FL_LogReset();
    ::AddVectoredExceptionHandler( 1, Radiant_FL_Veh );
    Radiant_FL_Log( "Radiant_BootFrame begin (raw-Win32 shell)" );

    // mainfrm.cpp:1440 — restore the persisted frame placement.
    Radiant_RestoreMainWindowPlacement( frame );

    // mainfrm.cpp:1443-1446.  g_pParentWnd (a CMainFrame *) has no raw analog and is NOT set
    // here; every core reader of it is either MFC-fenced or already retargeted onto
    // g_qeglobals.d_hwndMain / the Ed_*() accessors (U-GLOBALS).
    g_qeglobals.d_hInstance = ::GetModuleHandleA( nullptr );   // was AfxGetInstanceHandle()
    g_qeglobals.d_hwndMain  = frame;                           // was GetSafeHwnd()
    Radiant_SetDefaultGridState();

    // 1) Engine subsystems (threads / SL / dvars / FS) — before windows + renderer.
    Radiant_EngineInit();                                      // mainfrm.cpp:1448

    // 1a) The FULL cod4.prj parse (QE_LoadProject 0x48BAB0).
    Radiant_LoadProjectAtStartup();                            // mainfrm.cpp:1460

    // 1a-bis) Create the MRU (recent-files) menu structure (OnCreate 0x4210b0).  The registry
    //     load + menu insertion happen after ::SetMenu (step 4c), per SetButtonMenuStates.
    g_qeglobals.d_lpMruMenu = CreateMruMenuDefault();          // mainfrm.cpp:1467

    // 1b) SKIPPED — the icon toolbar (mainfrm.cpp:1473, OnCreate 0x420ac6 CreateEx +
    //     LoadToolBar(152)) is a CToolBar, and every TB_CHECKBUTTON seed with it.
    // i)  SKIPPED — m_bAutoMenuEnable = FALSE (mainfrm.cpp:1513) is CCmdTarget state.
    // 2)  SKIPPED — the docked CTextureBar strip (mainfrm.cpp:1521); barH is therefore 0 and
    //     the QE4 panes get the whole client area.  g_PrefsDlg->m_bTextureBar defaults OFF,
    //     so the MFC shell hides it too (mainfrm.cpp:1526) — same geometry either way.

    // 3) The four render windows + the layer-list placeholder + the console pane, laid out
    //    QE4-style (mainfrm.cpp:1534-1535).
    RECT rc;
    ::GetClientRect( frame, &rc );
    int cx = rc.right - rc.left, cy = rc.bottom - rc.top;
    if ( cx < 64 || cy < 64 ) { cx = 1280; cy = 800; }         // mainfrm.cpp:1520 fallback
    const EdLayout L = Radiant_ComputeLayout( cx, cy, /*topInset=*/0 );
    if ( !Radiant_CreateRenderWindows( frame, L ) )
    {
        Radiant_FL_Log( "render-window creation FAILED" );
        return false;
    }

    // 3-bis) The real renderer bring-up: R_InitRenderCommands + hunk + R_InitEditor +
    //    R_InitRendererForWindow x5 + fonts/qerfont + utility materials (mainfrm.cpp:1546).
    Radiant_FL_Log( "R_BeginRegistrationInternal..." );
    R_BeginRegistrationInternal();
    g_radiantFirstLightRendererReady = true;
    Radiant_FL_Log( "renderer up (windowCount=%d, font=%p)",
        dx.windowCount, (void *)g_qeglobals.d_font_list );

    // 3-ter) FULL-IMGUI TRANSITION: the frame itself takes the sixth swap chain and
    //    becomes the ImGui dockspace surface — the editor UI from here on. The five
    //    children turn into dock tabs (imgui_shell.cpp parks them over their tab rects).
    R_InitRendererForWindow( frame );
    ImGuiShell_SetPrimarySurface( frame );
    ::InvalidateRect( frame, nullptr, FALSE );   // kick the first dockspace frame
    Radiant_FL_Log( "ImGui primary surface = frame (windowCount=%d)", dx.windowCount );

    // 4) Menu + accelerator — the real IDR_MENU_QUAKE3 / IDR_MAIN_ACCEL (mainfrm.cpp:1552).
    //    The menu bar IS the command source in this shell: its WM_COMMANDs land in the frame
    //    WndProc above and route through Radiant_ExecCommand.
    HINSTANCE inst = ::GetModuleHandleA( nullptr );
    s_hMenu = ::LoadMenuA( inst, MAKEINTRESOURCEA( IDR_MENU_QUAKE3 ) );
    if ( s_hMenu )
        ::SetMenu( frame, s_hMenu );
    s_hAccel = ::LoadAcceleratorsA( inst, MAKEINTRESOURCEA( IDR_MAIN_ACCEL ) );
    Radiant_FL_Log( "menu=%p accel=%p", (void *)s_hMenu, (void *)s_hAccel );

    // 4a) User keyboard command map (OnCreate 0x4210bf/0x4210c9): layer the radiant.ini
    //     [Commands] overrides onto the built-in defaults, then annotate the menu items with
    //     their current key bindings (mainfrm.cpp:1562-1564).
    Radiant_LoadCommandMap();
    // KIWI-UX (RADIANT_UX_DESIGN §11, Phase 2): the keymap PROFILE lands on top of the
    // radiant.ini overrides, so an explicit user remap sits under the profile and the
    // classic profile is literally "defaults + radiant.ini".  Must run BEFORE the menu
    // annotation below, or the menu would advertise the pre-profile bindings.
    {
        extern void KiwiKeymap_ApplyBoot();   // kiwi_keymap.cpp
        KiwiKeymap_ApplyBoot();
    }
    if ( s_hMenu )
        Radiant_ShowMenuItemKeyBindings( s_hMenu );

    // 4a-bis) KIWI-UX (RADIANT_UX_DESIGN §9, shakeout B): append the native "Windows"
    //     popup — one check-marked item per hideable dock window (2D View, Z, Textures,
    //     Console, the shell panel), each carrying a KIWI instant command id so its
    //     WM_COMMAND lands in this file's `case WM_COMMAND` and routes through the
    //     ordinary Radiant_ExecCommand -> Radiant_DispatchCommandDirect KIWI arm.
    //     Deliberately AFTER Radiant_ShowMenuItemKeyBindings: that annotator walks the
    //     command table and rewrites any menu item whose id it finds, appending a bare
    //     "\t" for an unbound command — which these five are by default.  Running it
    //     first means it cannot see them, so their captions stay clean.  (The same
    //     annotator runs again on a keymap-profile switch, when they DO exist; see
    //     RADIANT_KNOWN_ISSUES.)  KiwiWindows_BuildMenu seeds the check marks itself.
    if ( s_hMenu )
    {
        extern void KiwiWindows_BuildMenu( void *frameMenu );   // kiwi_windows.cpp
        KiwiWindows_BuildMenu( s_hMenu );

        // 4a-ter) KIWI-UX (RADIANT_UX_DESIGN §17, shakeout C, user directive "add a
        //     disable grid option in the View Dropdown at the topbar"): a separator
        //     plus "Show Grid" / "Show Axes" check items appended INTO the existing
        //     View popup (index 2).  Appending ITEMS to a popup cannot shift the
        //     menu bar's POPUP indices, which is all the index-based consumers read
        //     (texwnd.cpp:1640 index 5, radiant_main.cpp:530 index 0) — the argument
        //     is written out in kiwi_windows.h.  Same post-annotator placement as
        //     the Windows popup above, and for the same caption-cleanliness reason.
        extern void KiwiWindows_BuildViewMenu( void *frameMenu );
        KiwiWindows_BuildViewMenu( s_hMenu );
        // KIWI: append before the launcher below performs the final DrawMenuBar.
        KiwiPlastBridge_BuildMenu( s_hMenu );

        // 4a-quater) KIWI-UX (ROUND BH, ITEM 5), user directive "Build and run needs
        //     to be in the win32 toolbar somewhere": a top-level, popup-less
        //     "Build & Run (F9)" item at the RIGHT END of the menu bar, carrying
        //     KIWI_CMD_BUILD_RUN so its WM_COMMAND lands in this file's `case
        //     WM_COMMAND` and routes through the ordinary Radiant_ExecCommand KIWI
        //     arm.  There is no CToolBar in this shell (step 1b above), and the menu
        //     bar is this shell's stated command source; kiwi_launch.h has the full
        //     argument.  LAST of the menu builders so the popups keep the
        //     bar positions they have had since shakeout B/C, and appending after
        //     them cannot move any POPUP INDEX the index-based consumers read.
        extern void KiwiLaunch_BuildMenu( void *frameMenu );   // kiwi_launch.cpp
        KiwiLaunch_BuildMenu( s_hMenu );
    }

    // 4b) Build the Textures-menu Usage / Locale / Surface-type filter submenus.  MUST run
    //     AFTER ::SetMenu so GetMenu(d_hwndMain) is live (mainfrm.cpp:1571).
    FillTextureMenu();

    // 4c) Populate File→Recent Files from the registry (SetButtonMenuStates 0x420039):
    //     LoadMruInReg + MRU_InsertItem into the File popup (mainfrm.cpp:1577).
    if ( g_qeglobals.d_lpMruMenu && g_qeglobals.d_project_entity )
    {
        LoadMruInReg( g_qeglobals.d_lpMruMenu );
        MRU_InsertItem( g_qeglobals.d_lpMruMenu, ::GetSubMenu( ::GetMenu( frame ), 0 ) );
        Radiant_FL_Log( "MRU loaded: %d recent files", g_qeglobals.d_lpMruMenu->wNbItemFill );
    }

    // 5) SKIPPED — the six-pane CStatusBar (mainfrm.cpp:1587).  MainFrm_SetStatusText is the
    //    only writer and it is still MFC-bound (mainfrm.cpp:1066 reads
    //    g_pParentWnd->m_wndStatusBar), so every status line silently goes nowhere here.
    //    REPORTED.

    // 6) Bootstrap an empty map (sentinel lists) so File→Open / a cmdline load have a valid
    //    map state to free+reload (mainfrm.cpp:1591).
    Map_NewMap();
    Eclass_ForName( 1, "worldspawn" );

    // 6a-pre) Seed the per-edit-layer current texdefs (Load_Textures head 0x45d140).
    Radiant_SeedCurrentTexdefs();                              // mainfrm.cpp:1595

    // 6a-bis) Bulk-load every material in the searchpaths into the browser.
    Load_Materials();                                          // mainfrm.cpp:1600

    // 6a-ter) Scan the entity def folders into the eclass list (QE_LoadProject tail).
    Radiant_LoadEclassDefs();                                  // mainfrm.cpp:1603

    // 6b) Load the visibility filters (RadiantFilters.txt), then refresh the Filters pane
    //     (a no-op in this shell — the ImGui panel re-gathers itself).
    Load_RadiantFilters();                                     // mainfrm.cpp:1611
    Radiant_RefreshFilterPane();

    // Optional cmdline map (File→Open replaces this for interactive use) — mainfrm.cpp:1615.
    const char *mapPath = ( __argc > 1 ) ? __argv[1] : nullptr;
    if ( Radiant_PathLooksLikeMap( mapPath ) )
        Radiant_OpenMap( mapPath );

    // No map loaded → Map_NewMap left world_entity NULL and the first drag-created brush
    // would NULL-deref Entity_LinkBrush( world_entity->def ).  The binary ends startup with a
    // valid worldspawn (QE_LoadProject → Map_New) — match that (mainfrm.cpp:1626).
    if ( !world_entity )
        Map_New();

    // Default the texture browser to SHOW ALL on startup (mainfrm.cpp:1633).
    Texture_ShowAll();

    // The startup CheckTextureScale (SetButtonMenuStates 0x420000) — mainfrm.cpp:1636.
    Radiant_ApplyStartupTextureScale();

    // ── KIWI-UX (ROUND AA, ITEM 1): RE-ASSERT THE FULL LISTING, LAST ──────────────
    // USER REPORT: "you broke the textures window!  It only shows 4 materials now."
    // "Show all" is the intent of the call above (mainfrm.cpp:1633) but it is made
    // BEFORE the scale pass, and the scale pass is a whole subtree —
    // Radiant_CheckTextureScale -> Texture_ResetPosition -> TexWnd_HitTest /
    // TexWnd_ApplyMaterialAtIndex -> Texture_SetTexture -> Brush_SetTexture — every
    // level of which touches browser state (is_in_use via Texture_GetHandle, the
    // per-layer nPos, the scroll position, m_selIndex).  The base accept predicate
    // is `tex->is_in_use` (texwnd.cpp:820), so ANY of that leaving flags clear
    // leaves the browser showing a handful of materials, and nothing later in the
    // boot re-opens it.  Asking for "show all" AFTER the last thing that can narrow
    // it makes the boot state unconditional instead of order-dependent.  It is the
    // same ported call, it is idempotent (texwnd.cpp:574 — one flag write per
    // qtexture plus a W_TEXTURE dirty bit), and it changes nothing a user does
    // later: Textures->Show In Use still narrows the browser on demand, and a map
    // load still narrows it (map.cpp:546).
    Texture_ShowAll();

    // mainfrm.cpp:1638-1644 — counts, grid status line, grid radio-check, and the
    // Light-Preview submenu check marks seeded from the LOADED prefs.
    QE_CountBrushesAndUpdateStatusBar();
    Radiant_SetGridStatus();
    Radiant_CheckGridMenu();
    Radiant_CheckMenu( 33950, g_PrefsDlg->enable_light_preview != 0 );
    Radiant_CheckMenu( 36108, g_PrefsDlg->preview_sun_aswell   != 0 );
    // KIWI-UX (ROUND AV, ITEM 1): seed View->Entities-as... from the LOADED prefs too.
    // Radiant_SetEntityCheck (mainfrm.cpp:3275, the binary's SetEntityCheck 0x42B1F0) was
    // only ever called BY the six setters, so the submenu opened with nothing ticked and
    // the persisted mode was invisible until the user picked one.
    Radiant_SetEntityCheck();

    // CMainFrame::OnCreateClient tail, 0x4232E3: apply the persisted render mode after the
    // camera window exists (mainfrm.cpp:1650).
    Texture_SetMode( g_qeglobals.d_savedinfo.iTextMenu );

    // ── KIWI-UX (ROUND BE): ACCEPT FILES DROPPED FROM EXPLORER ───────────────────
    // The one-line half of the texture-import feature.  Nothing in src/ registered for
    // WM_DROPFILES before this round (kiwi_import.h D-BE-J), and this is the last point in
    // boot where the frame is fully built, so a drop can never arrive before the shell can
    // service it.  The frame is the only window that registers: the render "windows" are
    // hidden children the dockspace draws over, so a drop anywhere on the editor lands here
    // (D-BE-K).  Handled in Radiant_FrameWndProc's WM_DROPFILES case.
    ::DragAcceptFiles( frame, TRUE );

    // OnCreate tail (mainfrm.cpp:1653-1654): arm RoutineProcessing and the status timer.
    g_radiantFrameState.doLoop = true;   // was m_bDoLoop
    ::SetTimer( frame, 1, 250, nullptr );
    s_bootDone = true;
    Radiant_FL_Log( "Radiant_BootFrame done" );
    return true;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Pre-translate — CMainFrame::PreTranslateMessage (mainfrm.cpp:2342) plus the hotkey feed
//  the two view handlers lost.
//
//  Why the hotkey feed lives HERE: in the MFC shell every view forwarded its WM_KEYDOWN to
//  CMainFrame::OnKeyDown (CXYWnd::OnKeyDown 0x465c90 → xywnd.cpp:3369; CCamWnd::OnKeyDown
//  0x402f60 → camwnd.cpp:3781), and both of those forwards are compiled OUT under
//  KISAK_NO_MFC (the bodies are `#ifndef KISAK_NO_MFC ... #else (void)nChar;` — they say
//  outright "Raw shell: keys reach the frame's own pump instead").  So the pump does the
//  lookup, gated to EXACTLY the windows that fed the frame in MFC:
//     the frame itself      (ON_WM_KEYDOWN, mainfrm.cpp:229)
//     the XY grid           (xywnd.cpp:3369)
//     the camera view       (camwnd.cpp:3781)
//  Z and the texture browser are deliberately EXCLUDED: neither CZWnd nor CTexWnd has an
//  ON_WM_KEYDOWN entry (z.cpp:750 / texwnd.cpp:2285 both note the gap), so they swallowed
//  keys in the MFC shell and must keep swallowing them here.  The console EDIT is excluded
//  by the same rule, which is what keeps typed text out of the hotkey table (the equivalent
//  of CEntityWnd::PreTranslateMessage's "not an Edit control" gate, win_ent.cpp:1689).
// ═════════════════════════════════════════════════════════════════════════════
static bool Radiant_PreTranslateMessage( MSG *pMsg )
{
    // ImGui text input outranks accelerators/hotkeys: when a panel field owns the
    // keyboard, keys must reach it, not the command map (the pump-side twin of the MFC
    // shell's CEntityWnd::PreTranslateMessage "not an Edit control" gate).
    {
        extern bool ImGuiShell_WantsKeyboard();   // imgui_shell.cpp
        if ( ( pMsg->message == WM_KEYDOWN || pMsg->message == WM_CHAR ||
               pMsg->message == WM_KEYUP ||
               // KIWI-UX (ROUND AM, ITEM 7b): the SYS twins too, now that Alt
               // chords are routed below — a focused ImGui field still outranks
               // everything, and that must not become false for Alt+<key>.
               pMsg->message == WM_SYSKEYDOWN || pMsg->message == WM_SYSKEYUP )
             && ImGuiShell_WantsKeyboard() )
            return false;                          // TranslateMessage/Dispatch as normal
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AM, ITEM 7b) — ALT CHORDS REACH THE HOTKEY TABLE
    // ═════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: "the alt-s bind to open the surface inspector window
    // doesn't work because the win32 topbar steals the input and then locks alt
    // pressed down."  Exactly right, and it is the long-standing
    // RADIANT_KNOWN_ISSUES entry ("Alt-chords dead").
    //
    // THE MECHANISM.  Windows delivers a key pressed WITH Alt as WM_SYSKEYDOWN,
    // not WM_KEYDOWN.  Every hotkey path in this shell filtered on WM_KEYDOWN —
    // this function at the two tests below, and the frame WndProc's own
    // ON_WM_KEYDOWN arm — so an Alt chord reached NEITHER Radiant_TryHotkey NOR
    // KiwiUX_KeyFunnel.  It fell through to DefWindowProc, which is the menu-bar
    // mnemonic handler: no menu has an S mnemonic at top level, so the chord was
    // swallowed with a beep and the window was left in menu-activation mode with
    // Alt latched.  Alt+S (Surface Inspector, kiwi_keymap.cpp:97), Shift+Alt+S
    // (Patch Inspector, :85), Alt+V (centre rect, :372), Alt+F, Alt+R, Alt+H and
    // every Ctrl+Alt row were all unreachable for the same reason.
    //
    // THE FIX, AND WHY IT IS SAFE.  Route WM_SYSKEYDOWN through the SAME
    // Radiant_TryHotkey the WM_KEYDOWN path uses.  That function builds its
    // modifier mask from GetKeyState (mainfrm.cpp:1590-1614), so Alt is already
    // in the mask and a row only matches if it was BOUND with Alt — an UNBOUND
    // Alt chord returns false and falls straight through to the native menu,
    // unchanged.  Consuming a MATCHED chord (returning true) means the pump calls
    // neither TranslateMessage nor DispatchMessage, so the menu loop never sees
    // it and there is nothing to latch.
    //
    // THE THREE THINGS THAT MUST KEEP WORKING, each guarded explicitly:
    //   * BARE Alt and every other bare modifier fall through — round U's law
    //     (kiwi_command.cpp:1738-1788), reproduced here rather than assumed,
    //     because a swallowed modifier also starves ImGui's modifier state.
    //   * F10 opens the menu.  F10 arrives as WM_SYSKEYDOWN with NO Alt down, so
    //     the `alt` test below excludes it by construction — this path only ever
    //     looks at chords that actually hold Alt.
    //   * The SYSTEM chords stay the system's: Alt+F4 (close), Alt+Space (window
    //     menu), Alt+Tab, Alt+Enter, Alt+Esc.  Refused by name rather than by
    //     trusting that nothing is bound to them.
    //
    // AND THE LATCH ITSELF.  Consuming the chord is not enough on its own: the
    // Alt KEY-UP that follows is what DefWindowProc turns into "activate the menu
    // bar", so after Alt+S the menu would still drop and take the focus.  When a
    // chord is consumed, the trailing bare-Alt release is consumed with it — and
    // ONLY then, so an Alt tap with no chord still opens the menu (accessibility).
    // Alt+MMB is a MOUSE gesture and never enters this path at all.
    {
        static bool s_altChordConsumed = false;

        if ( pMsg->message == WM_SYSKEYUP || pMsg->message == WM_KEYUP )
        {
            const unsigned vkUp = (unsigned)pMsg->wParam;
            if ( vkUp == VK_MENU || vkUp == VK_LMENU || vkUp == VK_RMENU )
            {
                if ( s_altChordConsumed )
                {
                    s_altChordConsumed = false;
                    return true;               // swallow the release that opens the bar
                }
            }
        }

        if ( pMsg->message == WM_SYSKEYDOWN )
        {
            const unsigned vk  = (unsigned)pMsg->wParam;
            const bool     alt = ( ::GetKeyState( VK_MENU ) < 0 );

            bool routable = alt;
            switch ( vk )
            {
            // ROUND U's law — a bare modifier is never swallowed.
            case VK_SHIFT:   case VK_CONTROL: case VK_MENU:
            case VK_LSHIFT:  case VK_RSHIFT:
            case VK_LCONTROL:case VK_RCONTROL:
            case VK_LMENU:   case VK_RMENU:
            case VK_LWIN:    case VK_RWIN:
            // The system's own Alt chords, refused by name.
            case VK_F4:      case VK_SPACE:   case VK_TAB:
            case VK_RETURN:  case VK_ESCAPE:  case VK_F10:
                routable = false;
                break;
            default:
                break;
            }

            if ( routable && pMsg->hwnd != g_qeglobals.d_hwndEdit
              && Radiant_TryHotkey( vk ) )
            {
                s_altChordConsumed = true;     // …and eat the Alt release with it
                return true;
            }
        }
    }

    // KIWI-UX (RADIANT_UX_DESIGN §4/§5/§15, Phase 2): the modal-command + palette key
    // intercept.  Deliberately placed HERE — after the WantTextInput gate above (a focused
    // ImGui field still outranks everything, unchanged) and BEFORE both TranslateAccelerator
    // and Radiant_TryHotkey, so a live gesture's Esc/Enter/digits can never also fire an
    // editor hotkey and staple an unrelated undo record onto a half-finished edit.  Returns
    // false whenever no command is active and the palette is closed, i.e. the ordinary path
    // below is bit-for-bit what it was.  Swallowing the WM_KEYDOWN also suppresses its WM_CHAR
    // (the pump only calls TranslateMessage when this function returns false).
    {
        extern bool KiwiUX_KeyFunnel( unsigned int vk );   // kiwi_command.cpp
        if ( pMsg->message == WM_KEYDOWN && KiwiUX_KeyFunnel( (unsigned int)pMsg->wParam ) )
            return true;
    }

    // mainfrm.cpp:2344 — IDR_MAIN_ACCEL first.
    if ( s_hAccel && ::TranslateAcceleratorA( s_hwndFrame, s_hAccel, pMsg ) )
        return true;

    if ( pMsg->message != WM_KEYDOWN )
        return false;

    // mainfrm.cpp:2349-2353 — the command-map Ctrl+Z / Ctrl+Y undo/redo, handled frame-wide
    // so they work regardless of which child view has focus.  The MFC body called
    // OnEditUndo()/OnEditRedo() directly; those members are forwarders onto the same command
    // ids the table below dispatches (mainfrm.cpp:7203-7204).
    if ( ::GetAsyncKeyState( VK_CONTROL ) < 0 )
    {
        if ( pMsg->wParam == 'Z' ) { Radiant_ExecCommand( ID_EDIT_UNDO ); return true; }
        if ( pMsg->wParam == 'Y' ) { Radiant_ExecCommand( ID_EDIT_REDO ); return true; }
    }

    // The editor command map (g_radiantCommands) — APP-WIDE, matching MFC: CWinApp::
    // PreTranslateMessage ran for every message regardless of the focused window, so
    // hotkeys fired with focus on ANY view (Z and Texture included — their windows never
    // handled keys themselves in either shell). The only exclusions are typing sinks:
    // the console EDIT (CEntityWnd::PreTranslateMessage's "not an Edit control" gate)
    // and ImGui text input (already returned-false above via WantTextInput).
    if ( pMsg->hwnd != g_qeglobals.d_hwndEdit )
    {
        if ( Radiant_TryHotkey( (unsigned int)pMsg->wParam ) )
            return true;
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  The message pump — CWinApp::Run + CRadiantApp::OnIdle (radiantapp.cpp:38-47).
//
//  CHOSEN IDLE PATTERN: the PeekMessage-drain loop, i.e. MFC's own Run() structure —
//    for (;;) { while ( queue empty && OnIdle(n++) ) ;  do { PumpMessage(); } while ( queue non-empty ); }
//  NOT a ~10ms WM_TIMER.  Reasons, in order:
//    * it is what the MFC shell literally did, so the RoutineProcessing cadence (and hence
//      the camera-fly dtime sampling in mainfrm.cpp:1897) is unchanged;
//    * it does NOT busy-spin: the inner idle loop is bounded (OnIdle stops returning true),
//      and the pump then BLOCKS in GetMessage until real input arrives;
//    * a 10ms timer would add a message source the MFC build never had, waking the editor
//      100x/s (and re-entering RoutineProcessing) even with nothing pending.
//  The idle-pass COUNT: MFC's CWinApp/CWinThread::OnIdle returns TRUE for its first couple of
//  passes (its own cmd-UI update + temp-handle cleanup bookkeeping, neither of which exists
//  here) and FALSE after, so RoutineProcessing ran a small fixed number of times per idle
//  transition and then the pump blocked.  Radiant_OnIdle below reproduces that shape —
//  TRUE for passes 0 and 1, FALSE at 2, i.e. at most three RoutineProcessing calls per idle
//  transition — which is behaviourally equivalent because RoutineProcessing is idempotent:
//  it no-ops once g_nUpdateBits is 0 (mainfrm.cpp:1907).
//  Note the periodic SetTimer(1, 250) armed at the boot tail is what keeps the RMB
//  cursor-joystick camera fly advancing while the mouse is still — exactly as in MFC, where
//  that timer message was also the only thing re-entering the idle branch.
//  OBSERVATION (same in both shells, not a regression): while the ImGui dockhost window is
//  visible it self-invalidates every WM_PAINT (imgui_dockhost.cpp:68), so the queue is never
//  empty and the idle branch never runs — MFC's Run() stalled identically on a pending paint.
// ═════════════════════════════════════════════════════════════════════════════
static bool Radiant_OnIdle( long idleCount )
{
    // CRadiantApp::OnIdle (radiantapp.cpp:38): flush the pending window-update bits once per
    // idle pass.  RoutineProcessing no-ops when g_nUpdateBits == 0, so this cannot spin.
    if ( ::IsWindow( s_hwndFrame ) )
        Radiant_RoutineProcessing();
    return idleCount < 2;
}

static int Radiant_RunMessageLoop()
{
    // DETERMINISTIC TICK, not CWinApp::Run's idle-while-empty shape. The frame is a
    // continuously-rendered ImGui surface: if it re-invalidated itself from WM_PAINT
    // (the first attempt), its paint stayed permanently pending, which (a) made
    // `!PeekMessage` never true so OnIdle/RoutineProcessing never ran — the viewports'
    // g_nUpdateBits never drained — and (b) starved every child's WM_PAINT delivery.
    // Symptom: black viewports that only drew while the window was being dragged.
    // Here each tick: drain the queue → drain the editor's update bits (synchronous
    // child repaints via RDW_UPDATENOW) → exactly ONE synchronous ImGui frame →
    // sleep-until-input-or-10ms. UI runs ~60-100 Hz, input latency ≤ one tick, no
    // busy spin, and no window can starve another. (Free fix: the RMB camera fly now
    // advances every tick instead of MFC's message-driven ~4 Hz idle cadence.)
    // 60 FPS CAP, ZERO ADDED INPUT LATENCY: messages are drained on EVERY wake (input is
    // never held back by the frame gate), but a dockspace frame renders only when 1/60 s
    // has elapsed since the last one. The wait timeout is "time until the next frame is
    // due", so the loop sleeps precisely between frames yet wakes instantly for input
    // (QS_ALLINPUT). timeBeginPeriod(1) keeps the wait granularity ~1 ms — without it the
    // default 15.6 ms timer quantum would make a 16.7 ms cadence oscillate 30↔60.
    ::timeBeginPeriod( 1 );

    LARGE_INTEGER qpf, lastFrame;
    ::QueryPerformanceFrequency( &qpf );
    ::QueryPerformanceCounter( &lastFrame );
    const LONGLONG frameTicks = qpf.QuadPart / 60;

    MSG msg;
    for ( ;; )
    {
        // ATTRIBUTION: one zone around the whole drain, not one per message — pass-boundary
        // granularity.  Opened only when there is something to drain: the sub-millisecond
        // tail of every frame runs this loop many times with an empty queue, and zoning
        // those would bury the trace in empty scopes.
        if ( ::PeekMessageA( &msg, nullptr, 0, 0, PM_NOREMOVE ) )
        {
            PROF_SCOPED( "pump message drain" );
            while ( ::PeekMessageA( &msg, nullptr, 0, 0, PM_REMOVE ) )
            {
                if ( msg.message == WM_QUIT )
                {
                    ::timeEndPeriod( 1 );
                    return (int)msg.wParam;
                }
                if ( !Radiant_PreTranslateMessage( &msg ) )
                {
                    ::TranslateMessage( &msg );
                    ::DispatchMessageA( &msg );
                }
            }
        }

        Radiant_OnIdle( 0 );   // RoutineProcessing: Cam_MouseControl + g_nUpdateBits drain
                               // (event-driven viewport repaints — not frame-gated, so an
                               // edit still shows on the very next tick)

        LARGE_INTEGER now;
        ::QueryPerformanceCounter( &now );
        const LONGLONG sinceFrame = now.QuadPart - lastFrame.QuadPart;
        if ( sinceFrame >= frameTicks )
        {
            if ( s_hwndFrame && ::IsWindowVisible( s_hwndFrame ) && ImGuiShell_PrimaryActive() )
            {
                // The editor's frame: four RT viewport renders, one composited ImGui frame,
                // then the pop-out windows.  Capped at 60 Hz by the QPC arithmetic around it.
                PROF_SCOPED( "editor frame" );
                // Phase 5: render the four viewports into their RT textures FIRST — each does
                // its own scene + suppressed Present, so it must run OUTSIDE the compositing
                // frame's scene bracket. The compositing ImGui frame then samples them.
                ImGuiShell_RenderViewportsToRT();
                ImGuiShell_BeginFrame();         // authorize exactly one ImGui NewFrame for
                                                 // the paint we force next (stray paints won't)
                ::InvalidateRect( s_hwndFrame, nullptr, FALSE );
                ::UpdateWindow( s_hwndFrame );   // synchronous WM_PAINT — one dockspace frame
                // Render the pop-out panel OS windows now, once, immediately after the
                // frame rendered — decoupled from WM_PAINT so it can never run on a paint
                // that didn't render an ImGui frame (the AFK UpdatePlatformWindows assert).
                ImGuiShell_RenderPlatformWindows();
                // Viewport mouse input — AFTER present, outside the scene bracket (a
                // handler may pop a modal context menu). Uses the rects/hover the frame
                // just recorded.
                ImGuiShell_DispatchViewportInput();
                FrameMark;
            }
            // Advance by whole frame intervals (drift-free cadence); a long stall
            // resynchronizes instead of "catching up" with a burst.
            lastFrame.QuadPart += frameTicks * ( sinceFrame / frameTicks );
        }

        ::QueryPerformanceCounter( &now );
        LONGLONG ticksLeft = frameTicks - ( now.QuadPart - lastFrame.QuadPart );
        DWORD waitMs = 0;
        if ( ticksLeft > 0 )
            waitMs = (DWORD)( ( ticksLeft * 1000 ) / qpf.QuadPart );
        if ( waitMs > 16 ) waitMs = 16;
        ::MsgWaitForMultipleObjects( 0, nullptr, FALSE, waitMs, QS_ALLINPUT );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  WinMain — CRadiantApp::InitInstance (radiantapp.cpp:49) + CWinApp::Run.
// ═════════════════════════════════════════════════════════════════════════════
int APIENTRY WinMain( HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/,
                      LPSTR /*lpCmdLine*/, int nCmdShow )
{
    // radiantapp.cpp:53 — gate P2 smoke test: verify the engine subset links and basic
    // printing works before any real init.  Com_Printf is safe pre-init.
    Com_Printf( 0, "[Radiant] Gate P2 smoke: engine subset initialized, WinMain reached\n" );

    // Map loads are straight-line synchronous and never pump; without this the DWM
    // ghost paints a white copy over the frame once Windows marks it unresponsive.
    ::DisableProcessWindowsGhosting();

    // radiantapp.cpp:57 — register the common-control window classes the editor's child
    // controls need (the inspector tab strip uses SysTabControl32 — ICC_TAB_CLASSES).
    {
        INITCOMMONCONTROLSEX icc;
        icc.dwSize = sizeof( icc );
        icc.dwICC  = ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES |
                     ICC_UPDOWN_CLASS | ICC_STANDARD_CLASSES;
        ::InitCommonControlsEx( &icc );
    }

    // radiantapp.cpp:69 — SetRegistryKey( "iw\\CoD4Radiant" ) has no equivalent here: ALL
    // registry usage was removed — the Radiant_Profile* helpers persist to kiwi_radiant.ini
    // beside the exe (radiant_registry.h), so the editor is fully self-contained.
    // radiantapp.cpp:70 — bring the preference singleton up with the saved values BEFORE any
    // view reads g_PrefsDlg.
    Prefs_Init( /*loadFromRegistry=*/true );

    // The frame window class.  CS_DBLCLKS is NOT set: CMainFrame's message map has no
    // double-click entry either.  No background brush — WM_ERASEBKGND returns 1 and the
    // panes cover the client area.
    {
        WNDCLASSA wc = {};
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = Radiant_FrameWndProc;
        wc.hInstance     = hInstance;
        wc.hIcon         = ::LoadIconA( hInstance, MAKEINTRESOURCEA( IDI_RADIANT ) );
        wc.hCursor       = ::LoadCursorA( nullptr, IDC_ARROW );
        wc.hbrBackground = nullptr;
        wc.lpszClassName = RADIANT_FRAME_CLASS;
        if ( !::RegisterClassA( &wc ) )
            return 1;

        WNDCLASSA blank = {};
        blank.lpfnWndProc   = Radiant_BlankPaneWndProc;
        blank.hInstance     = hInstance;
        blank.hCursor       = ::LoadCursorA( nullptr, IDC_ARROW );
        blank.lpszClassName = RADIANT_BLANK_CLASS;
        if ( !::RegisterClassA( &blank ) )
            return 1;
    }

    // radiantapp.cpp:77-88 — the initial frame rect: the desktop work area inset by 1/40 on
    // each side (the fallback 1600x1000 stands in when SPI_GETWORKAREA fails).  Reproduced
    // verbatim, including the fact that the insets are applied inside the `if`.
    RECT work = { 0, 0, 1600, 1000 };
    if ( ::SystemParametersInfoA( SPI_GETWORKAREA, 0, &work, 0 ) )
    {
        int w = work.right - work.left;
        int h = work.bottom - work.top;
        work.left   += w / 40;
        work.top    += h / 40;
        work.right  -= w / 40;
        work.bottom -= h / 40;
    }

    // radiantapp.cpp:87 — the frame itself.  The MFC title was "CoD4Radiant"; this shell is
    // KIWI's, so the caption is "KIWI Radiant" (Radiant_OpenMap still rewrites it to
    // "CoD4Radiant - <map>" on a load — that writer is MFC-bound today, see the report).
    // WS_CLIPCHILDREN is LOAD-BEARING: the frame is itself a D3D present surface (the
    // dockspace), and without it the frame's full-client Present draws over the child
    // viewports' pixels every tick while the children present back — a two-way overdraw
    // fight that reads as constant flicker. With it, GDI clips the frame's present
    // region around the children, so each surface owns its pixels exclusively.
    s_hwndFrame = ::CreateWindowExA( 0, RADIANT_FRAME_CLASS, "KIWI Radiant",
                                     WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                     work.left, work.top,
                                     work.right - work.left, work.bottom - work.top,
                                     nullptr, nullptr, hInstance, nullptr );
    if ( !s_hwndFrame )
        return 1;

    // CMainFrame::OnCreate's body (see the step-for-step cites in Radiant_BootFrame).
    if ( !Radiant_BootFrame( s_hwndFrame ) )
    {
        ::DestroyWindow( s_hwndFrame );
        return 1;
    }

    // radiantapp.cpp:90-91.
    ::ShowWindow( s_hwndFrame, nCmdShow );
    ::UpdateWindow( s_hwndFrame );

    // radiantapp.cpp:96-138 — the RADIANT_STARTUP_MAP / RADIANT_STARTUP_CAM operator hooks
    // (body in mainfrm.cpp, where camera_s is in scope).
    Radiant_ApplyStartupMapEnv();

    // CWinApp::Run.
    return Radiant_RunMessageLoop();
}

// ═════════════════════════════════════════════════════════════════════════════
//  SKIPPED from the MFC boot, and what still points at each skip
//  (full analysis in the U-BOOT report; every item is a reported follow-up, not a silent gap):
//    * the icon TOOLBAR (CToolBar m_wndToolBar) and every TB_CHECKBUTTON seed — ~25 commands
//      in Radiant_DispatchCommandDirect still return false, now with a "POST-RIP: m_wndToolBar"
//      marker (the tail of the switch), so they are unroutable in BOTH shells today.  They
//      unblock when an ImGui toolbar ships (U-BOOT v2).
//    * the docked CTextureBar — Radiant_RefreshTextureBar (mainfrm.cpp:1848) is already
//      #ifndef-fenced to a no-op, so the invalidation broadcast is clean.
//    * the six-pane CStatusBar — MainFrm_SetStatusText (mainfrm.cpp:1066) still dereferences
//      g_pParentWnd->m_wndStatusBar, i.e. it does not COMPILE under KISAK_NO_MFC yet.
//    * the entity inspector (d_hwndEntity stays NULL) — every reader is NULL-guarded or
//      MFC-fenced (cites at its creation site above); the N/O/T/F mode hotkeys no-op.
//    * [FIXED by U-CMD-2] the splitter-bar drag and the frame's WM_MOUSEWHEEL dispatcher.
//      HitTestBar + the three gutter handlers and CMainFrame::OnScroll are free functions now
//      (Radiant_HitTestBar / Radiant_SplitterOn* / Radiant_OnMouseWheel), wired in the frame
//      WndProc above; the gutters drag and the camera dolly work in this shell.
//    * ON_UPDATE_COMMAND_UI (the 4 enable/grey handlers, mainfrm.cpp:683) — MFC idle-time
//      command UI; menu items are never greyed in this shell.
// ═════════════════════════════════════════════════════════════════════════════


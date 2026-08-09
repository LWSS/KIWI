#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ═════════════════════════════════════════════════════════════════════════════
//  radiant_frame.h — the editor-frame API (Phase 4 unit U-BOOT).
//
//  Everything here is plain C++ over HWND/RECT/globals.  Bodies live in mainfrm.cpp;
//  radiant_main.cpp (WinMain + the frame WndProc) is their caller.
//
//  U-RIP: the old MFC shell is deleted, so this is the ONE frame API — nothing is fenced
//  any more.  The `was <class>::<method> (0xADDR)` notes below are PROVENANCE (the IDA
//  function each free function was extracted from), not live declarations.
// ═════════════════════════════════════════════════════════════════════════════

#include "stdafx.h"     // HWND / RECT / UINT + qe3.h (g_qeglobals)

// ─── The QE4 pane layout ─────────────────────────────────────────────────────────
// Was `struct EdLayout { CRect ... }` at mainfrm.cpp:766.  Plain RECT now: the two members
// the layout code used (SetRect / OffsetRect) are the Win32 ::SetRect / ::OffsetRect the old
// CRect methods wrapped.
struct EdLayout { RECT xy, cam, z, tex, ent, con; };

// mainfrm.cpp:858 (was static).  Splits a client area into the QE4 arrangement
// (Z | XY | (Camera over Textures), console strip across the bottom) using the persisted
// splitter fractions, and caches the gutter geometry for the splitter hit-test.  topInset
// reserves the docked texture-bar strip at the top.
EdLayout Radiant_ComputeLayout( int cx, int cy, int topInset = 0 );

// ─── The frame's own state (U-CMD-1) ─────────────────────────────────────────────
// The three CMainFrame members the extracted free functions still need.  The editor frame
// is a singleton, so this is ONE instance (definition in mainfrm.cpp).
//   doLoop       = CMainFrame::m_bDoLoop       (RoutineProcessing gate, set at the boot tail)
//   camPreview   = CMainFrame::m_bCamPreview   (ToggleCamera 0x423A50 / OnUpdateViewCameraupdate)
//   currentStyle = CMainFrame::m_nCurrentStyle (the QE4 window style; pinned at 1, never written)
struct radiantFrameState_t
{
    bool doLoop       = false;
    bool camPreview   = false;
    int  currentStyle = 1;
};
extern radiantFrameState_t g_radiantFrameState;

// ─── The frame's shell-agnostic behaviour (mainfrm.cpp) ──────────────────────────
void Radiant_UpdateWindows( int nBits );                  // was CMainFrame::UpdateWindows     (0x427090)
void Radiant_RoutineProcessing();                         // was CMainFrame::RoutineProcessing (0x421a90)
bool Radiant_TryHotkey( unsigned int vk );                // was CMainFrame::TryHotkey (0x422370's lookup half)
void Radiant_ExecCommand( unsigned int cmdId );           // the single id->action entry point
bool Radiant_DispatchCommandDirect( unsigned int cmdId ); // the Phase-4 direct command table

// ─── The unsaved-changes prompts + the File-menu map flows (U-CMD-2) ─────────────
// All plain Win32: the prompts are MessageBoxA over ::GetActiveWindow(), and the file flows
// reach the shell only through s_currentMapPath + g_qeglobals.d_hwndMain (the frame HWND).
bool Radiant_ConfirmModified();                           // was CMainFrame::ConfirmModified   (0x49A030)
bool Radiant_OkToDiscard();                               // was CMainFrame::OkToDiscard (the per-command guard)
bool Radiant_FileSaveAs();                                // was CMainFrame::OnFileSaveAs_Confirmed (SaveAsDialog 0x49A760)
void Radiant_FileNew();                                   // was CMainFrame::OnFileNew         (0x423AA0)
void Radiant_FileOpen();                                  // was CMainFrame::OnFileOpen        (0x423AE0)
void Radiant_FileSave();                                  // was CMainFrame::OnFileSave
void Radiant_FileExit();                                  // was CMainFrame::OnFileExit (posts WM_CLOSE)
bool Radiant_FrameCloseAllowed();                         // the guard half of CMainFrame::OnClose (0x422220)

// ─── The frame's mouse input (U-CMD-2) ───────────────────────────────────────────
// The central wheel dispatcher every editor child funnels WM_MOUSEWHEEL into: texture pane
// hover → half-page scroll, camera hover (+CameraUseWheel) → dolly, otherwise XY zoom.
// `screenPt` is in SCREEN coords (WM_MOUSEWHEEL's lParam); always returns true (the wheel is
// consumed on every path, as the binary's OnScroll does).
bool Radiant_OnMouseWheel( HWND focusOrHover, short zDelta, POINT screenPt );

// The gutter (splitter-bar) drag.  Geometry is cached by Radiant_ComputeLayout; the move
// handler updates the persisted fractions and returns "changed" — the CALLER then relays the
// panes out (that step is the one shell-specific half).  Each returns true when it consumed
// the message, i.e. the caller must not chain to its default handler.
int  Radiant_HitTestBar( int x, int y );                  // 0 = none; 1/2 vertical, 3/4/5 horizontal
bool Radiant_SplitterOnSetCursor( HWND frame );
bool Radiant_SplitterOnLButtonDown( HWND frame, int x, int y );
bool Radiant_SplitterOnMouseMove( unsigned int nFlags, int x, int y );
bool Radiant_SplitterOnLButtonUp();

// The CXYWnd method bodies that live in mainfrm.cpp, as free functions over Ed_ActiveXY().
void XYWnd_PositionView();                                // was CXYWnd::PositionView   (0x46DE10)
void XYWnd_CopyClip();                                    // was CXYWnd::Copy
void XYWnd_PasteClip();                                   // was CXYWnd::Paste
void XYWnd_SetViewType( int vt );                         // was CXYWnd::SetViewType    (0x46DF90)
bool XYWnd_SetRotateMode( int bMode );                    // was CXYWnd::SetRotateMode  (0x46E090)

// Frame helpers each command body needs (were CMainFrame members with HWND-free bodies).
void Radiant_SetGridStatus();                             // was CMainFrame::SetGridStatus      (0x428a00)
void Radiant_PicMip();                                    // was CMainFrame::PicMip             (0x420860)
void Radiant_CheckTextureScale( UINT uIDCheckItem );      // was CMainFrame::CheckTextureScale  (0x42AF50)
void Radiant_SetEntityCheck();                            // was CMainFrame::SetEntityCheck     (0x42B1F0)
void Radiant_CheckMenu( UINT id, bool checked );          // mainfrm.cpp:2572 (was static)
void Radiant_CheckGridMenu();                             // mainfrm.cpp:2537 (was static)

// ─── The boot sequence's steps (all extracted from CMainFrame::OnCreate / InitInstance) ──
// Each was a static helper or an inline block of the MFC boot; the raw shell's WinMain runs
// the SAME bodies in the SAME order (see the step table at the head of radiant_main.cpp).
void Radiant_FL_Log( const char *fmt, ... );              // %TEMP%\radiant_firstlight.log trace
void Radiant_FL_LogReset();                               // mainfrm.cpp:164  (was static)
LONG WINAPI Radiant_FL_Veh( EXCEPTION_POINTERS *ep );     // mainfrm.cpp:201  (was static) — VEH sink
void Radiant_RestoreMainWindowPlacement( HWND frame );    // mainfrm.cpp:842  (was static, CMainFrame*)
void Radiant_SetDefaultGridState();                       // mainfrm.cpp:719  (was static)
void Radiant_EngineInit();                                // mainfrm.cpp:931  (was static)
bool Radiant_LoadProjectAtStartup();                      // mainfrm.cpp:1188 (was static)
void Radiant_LoadCommandMap();                            // 0x421230 — was CMainFrame::LoadCommandMap
BOOL Radiant_ShowMenuItemKeyBindings( HMENU hMenu );      // 0x420460 — was CMainFrame::ShowMenuItemKeyBindings
void Radiant_SeedCurrentTexdefs();                        // OnCreate step 6a-pre (Load_Textures head 0x45d140)
void Radiant_LoadEclassDefs();                            // OnCreate step 6a-ter (QE_LoadProject tail 0x48bab0)
void Radiant_ApplyStartupTextureScale();                  // OnCreate step 6b-bis (SetButtonMenuStates 0x420000)
bool Radiant_PathLooksLikeMap( const char *p );           // mainfrm.cpp:1122 (was static)
void Radiant_OpenMap( const char *path );                 // File->Open + the cmdline/startup load
void Radiant_ApplyStartupMapEnv();                        // RADIANT_STARTUP_MAP / _CAM debug hooks

// Set once init is fully up (device created).  Every view's paint gates on it.
extern bool g_radiantFirstLightRendererReady;

// ─── The four raw viewport creators (one per U-VP-* unit) ────────────────────────
// Each registers its own window class on first use and returns a WS_CHILD|WS_VISIBLE view
// parented to `parent`.  The CALLER owns the g_qeglobals.d_hwnd* registration and the
// creation ORDER — see the caller-obligation comment block at the head of each raw shell
// (xywnd.cpp:4595, camwnd.cpp:3880, z.cpp:750, texwnd.cpp:2245) and the boot table in
// radiant_main.cpp.
HWND XYWnd_CreateRaw ( HWND parent, int x, int y, int w, int h );   // xywnd.cpp:4694
HWND CamWnd_CreateRaw( HWND parent, int x, int y, int w, int h );   // camwnd.cpp:3987
HWND ZWnd_CreateRaw  ( HWND parent, int x, int y, int w, int h );   // z.cpp:814
HWND TexWnd_CreateRaw( HWND parent, int x, int y, int w, int h );   // texwnd.cpp:2326

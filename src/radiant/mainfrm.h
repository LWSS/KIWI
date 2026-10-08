#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// The editor's shared window-layer declarations.  Handler EAs come from the IDB
// message-map dump; bodies live in mainfrm.cpp and the per-window TUs.
//
// U-RIP: the MFC window/dialog classes that used to fill this header (CCamWnd / CXYWnd /
// CZWnd / CEdBlankPane / CTexWnd / CEntityWnd / CTextureBar / the C*Dlg family / the
// 387-method CMainFrame) are DELETED — the editor is a plain-Win32 + ImGui shell now.
// What is left is exactly what common code names: camera_s (the editor camera state,
// reached via Ed_Camera()), the free-function entry points and the shared globals.
// Nothing was hoisted out of class scope when the classes went: the only common-code user
// of the old CXYWnd::EViewType (map.cpp) goes through XYWnd_SetViewType(int) + xywnd.h's
// plain ED_VIEW_* enum, and CCamWnd::LightPreviewRec already had a common twin
// (camwnd.cpp's camLightPreviewRec_t).
// The frame's own API lives in radiant_frame.h; a follow-up folds these survivors in there.

#include "stdafx.h"

// ─── The 3D perspective camera view (camwnd.cpp) ───────────────────────────
// camera_s — IDB camera_s (0x7c). The basis vectors (vpn/vright/vup) are rebuilt
// each frame from angles by Cam_BuildMatrix; forward/right are the yaw-plane
// movement basis used by the WASD/arrow fly controls.
struct camera_s   // 0x7c
{
    int   width;          // 0x00  client width  (px)
    int   height;         // 0x04  client height (px)
    bool  timing;         // 0x08
    char  pad_timing[3];  // 0x09
    float origin[3];      // 0x0c  camera world position
    float angles[3];      // 0x18  pitch/yaw/roll
    int   draw_mode;      // 0x24  0=wire 1=fullbright 2=normal 3=view 4=textures
    float color[3];       // 0x28
    float forward[3];     // 0x34  yaw-plane forward (movement)
    float right[3];       // 0x40  yaw-plane right   (movement)
    float up[3];          // 0x4c
    float vup[3];         // 0x58  view up
    float vpn[3];         // 0x64  view forward (normal)
    float vright[3];      // 0x70  view right
};
static_assert(sizeof(camera_s) == 0x7c, "camera_s must be 0x7c (IDB)");


// CFilterWnd refresh entry — the Filters inspector pane re-reads the loaded filter
// lists + d_xyShowFlags (called on map load + when the inspector enters Filter mode).
void Radiant_RefreshFilterPane();

// SetInspectorMode (IDB CEntityWnd_SetInspectorMode 0x496b00): switch the right-column
// inspector between Entity / Textures / Console / Filters (INSPECTOR_* in qedefs.h).
// Faithful state machine (sets inspector_mode + window title + the ID_MISC_SELECTENTITYCOLOR
// menu enable) adapted to this port's separate panes: "show one pane-group at a time".
// Driven by the N/O/T/F hotkeys + View menu (no tab control — see win_ent.cpp).
void CEntityWnd_SetInspectorMode( int mode );
extern int inspector_mode;            // IDB 0x240a110 — the active inspector pane



// surfacedlg.cpp entry point (selection/load refresh).
void Surf_UpdateInspector();    // UpdateSurfaceDialog (0x458590) — refresh fields on change



// DoColor (win_dlg.cpp, IDB 0x499350) — the Colors-menu colour picker.  Seeds a CColorDialog
// from g_qeglobals.d_savedinfo.colors[index], on OK writes the chosen RGB back (alpha = 1)
// and flags a full window update.  Returns 1 on OK, 0 on cancel.
int DoColor( int index );


// Status-bar sink (the real MainFrm_SetStatusText; replaces the engine_stubs no-op).
void MainFrm_SetStatusText( int pane, const char *text );

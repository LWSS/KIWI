#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Editor XY-view math: free functions taking an explicit view-state struct, which the MFC
// CXYWnd fills from its members (m_nViewType / m_fScale / m_vOrigin / m_nWidth / m_nHeight).
// The originals are CXYWnd methods in the CoD4Radiant binary (IW3xRadiant.i64).

struct GfxMatrix;

// CoD4Radiant view-type values, recovered from CXYWnd::XY_DrawGrid's label logic
// (@0x4686a0: m_nViewType==2 -> "XY Top", ==1 -> "XZ Front", else "YZ Side").
enum { ED_VIEW_YZ = 0, ED_VIEW_XZ = 1, ED_VIEW_XY = 2 };

struct XYViewState
{
    int   viewType;      // m_nViewType (ED_VIEW_*)
    float scale;         // m_fScale (pixels per world unit)
    float origin[3];     // m_vOrigin (world-space view centre)
    int   width;         // m_nWidth  (client width,  px)
    int   height;        // m_nHeight (client height, px)
    bool  active;        // m_bActive (brighter view-name label when focused)
};

void XY_SetupProjectionMtx(GfxMatrix *mtx, float width, float height, float depth); // IDB 0x4a7980
bool XY_SetupScene(const XYViewState *v);                                           // IDB 0x5064c0 (false = degenerate-projection frame dropped)
void XY_DrawGrid(const XYViewState *v);                                             // IDB 0x4686a0 (grid lines only)
void XY_DrawBlockGrid(const XYViewState *v);                                        // IDB 0x4690f0 (1024-unit block grid + labels)
void XY_DrawBrushes(const XYViewState *v);                                          // IDB 0x46CE20 (brush + overlay body)

// ─── xywndState_t — the XY viewport's shell state (U-VP-XY / U-GLOBALS) ────────────
// The XY grid view is a SINGLETON in this port: Radiant_CreateRenderWindows (mainfrm.cpp
// :881) news exactly ONE CXYWnd and immediately aliases m_pActiveXY onto it; m_pActiveXY is
// never reassigned anywhere (the binary's multi-pane active-XY swap in OnLButtonDown/
// OnRButtonDown is already documented as MOOT here).  So the one file-scope instance in
// xywnd.cpp IS the XY view: the MFC CXYWnd handlers and the raw-Win32 WndProc twin in that
// file read/write these SAME fields, and Ed_ActiveXY() hands the same block to core callers.
//
// Every field keeps its CXYWnd member NAME so the handler / draw / clipper bodies are
// unchanged (verbatim) after the sweep — only the pointer TYPE changed (CXYWnd* → xywndState_t*).
//   m_hWnd   = the view's window handle, standing in for CWnd::m_hWnd / GetSafeHwnd() (the
//              clipper capture tests and CreateEntityFromClassname's GetClientRect need it).
//              Latched in XYWnd_OnCreate.
//   clip*    = the XY view-bounds / clip-plane block (binary CXYWnd +168..+256).  These are
//              STATE-ONLY: brush.cpp's prefab-content cull reaches them through
//              XYWnd_SetupClipPlanes + XYWnd_CullBrush, both operating on this block.
// mainfrm.h still DECLARES all of these as CXYWnd members (U-GUARD deletes them with the class)
// — see the unit report's dead-member list.
struct xywndState_t
{
    HWND  m_hWnd      = nullptr;      // (port) the view HWND — was CWnd::m_hWnd
    int   m_nViewType = 2;            // ED_VIEW_XY (top-down); 0=YZ, 1=XZ, 2=XY
    float m_fScale    = 1.0f;         // IDA CXYWnd::CXYWnd 0x4637bc
    float m_vOrigin[3] = { 0.0f, 20.0f, 46.0f }; // IDA CXYWnd::CXYWnd 0x463784..0x4637a8
    int   m_nWidth    = 0;            // client width  (px), updated in XYWnd_OnSize
    int   m_nHeight   = 0;            // client height (px)
    bool  m_bActive   = true;

    // Input state (mirrors the CoD4Radiant CXYWnd press members).
    int    m_nButtonstate    = 0;     // current MK_ button flags during a press
    int    m_nPressx         = 0;     // press pixel X
    int    m_nPressy         = 0;     // press pixel Y
    float  m_vPressdelta[3]  = { 0, 0, 0 };
    POINT  m_ptCursor        = { 0, 0 };  // screen cursor at press (for RMB scroll)
    bool   m_bPress_selection = false;    // something was selected at press time

    // ── XY view bounds + clip planes (binary CXYWnd fields +168..+256) ─────────
    int   m_clipDim1 = 0;             // +168 (x84)  horizontal world axis
    int   m_clipDim2 = 1;             // +172 (x85)  vertical world axis
    int   m_clipDim3 = 2;             // +176 (x86)  the out-of-plane axis
    float m_clipMin1 = 0.0f;          // +180 (x87)  view-rect min along dim1
    float m_clipMin2 = 0.0f;          // +184 (x88)  view-rect min along dim2
    float m_clipMax1 = 0.0f;          // +188 (x89)  view-rect max along dim1
    float m_clipMax2 = 0.0f;          // +192 (x90)  view-rect max along dim2
    float m_clipPlanes[4][4] = {};    // +196..+256  four {normal[3], dist} view-edge planes
};

// Ed_ActiveXY — the shell-agnostic "active XY view" accessor (U-GLOBALS).  The binary's
// concept (g_pParentWnd->m_pActiveXY) collapses to the single XY view in this port, so this
// always returns the one state block (never NULL); under the MFC shell m_pActiveXY stays an
// MFC-side alias of the same viewport.  Defined in xywnd.cpp.
xywndState_t *Ed_ActiveXY();

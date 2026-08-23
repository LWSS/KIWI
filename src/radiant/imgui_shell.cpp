// imgui_shell.cpp — the editor's shell: the Dear ImGui dockspace hosted on the main
// frame, its panels, and the five viewport dock tabs.  New KISAK code (not from the
// binary).  KIWI-UX (CLEANUP, C-10): the header used to say this was a Phase-2a
// overlay behind an `-imgui` flag with the MFC shell untouched.  All three claims
// are false — the shell IS the editor UI, unconditionally (user directive
// 2026-08-08), and the flag and its gate statics are gone.  See
// RADIANT_UI_REWORK_PLAN.md.
#include "stdafx.h"
#include "qe3.h"

#include <d3d9.h>
#include "radiant_rtt.h"           // RTT_GetTexture / rttViewport_t (viewport images)
#include "kiwi_windows.h"          // KIWI-UX §9 (shakeout B): per-window visibility flags
#include "kiwi_outliner.h"         // KIWI-UX ROUND W: the scene list dock window
#include "kiwi_entbrowser.h"       // KIWI-UX ROUND AU: the entity browser + camera drop target
#include "kiwi_modelbrowser.h"     // KIWI-UX: the Models tab + camera drop target
#include "kiwi_skybox.h"           // KIWI-UX ROUND AZ: the Sky tab (third tab of that node)
#include "kiwi_uveditor.h"         // KIWI-UX ROUND BD: the UV editor (fourth tab of it)
#include "kiwi_sun.h"              // KIWI-UX: the Sun helper tab (fifth tab of that node)
#include "kiwi_light.h"            // KIWI-UX: the Light helper tab (sixth tab of that node)
#include "kiwi_cmdoptions.h"       // KIWI-UX ROUND AI, ITEM 3: the in-command options panel
#include "kiwi_import.h"           // KIWI-UX ROUND BE: the texture-import wizard (modal popup)
#include "kiwi_launch.h"           // KIWI-UX ROUND BF: the Build & Run dialog + its child poll
#include "kiwi_texgrave.h"         // the deferred-release graveyard (D-BU-A..E)
#include "radiant_registry.h"      // KIWI-UX (CLEANUP, C-7): Radiant_IniPath — <exedir>\…
#include <universal/profile.h>
#include "kiwi_viewdirty.h"        // the 2D-view dirty-flag skip
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>     // DockBuilder API + GetCurrentWindow (auto-close)
#include <imgui/backends/imgui_impl_win32.h>
#include <imgui/backends/imgui_impl_dx9.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg,
                                                              WPARAM wParam, LPARAM lParam );

static bool s_shellInited  = false;
static bool s_frameRendered = false;  // a full ImGui frame was Render()'d this cycle —
                                      // gates the platform-window update (AFK assert fix)
static bool s_beginFrame    = false;  // the PUMP requests exactly one ImGui frame per tick.
                                      // NewFrame is driven by the frame's WM_PAINT, but stray
                                      // paints (boot InvalidateRect, OS repaints) must NOT run
                                      // a NewFrame — else NewFrame count and UpdatePlatform-
                                      // Windows count desync and NewFrame asserts. Only a
                                      // pump-requested paint clears this and runs the frame.
// KIWI-UX (ROUND Y, ITEM 6): "an ImGui frame really ran this tick".  Set by
// ImGuiShell_DrawOverlay once it is past ALL of its early-outs and has called
// NewFrame; consumed (and cleared) by ImGuiShell_DispatchViewportInput, which the
// pump calls unconditionally.  Without it, a skipped frame made the dispatch
// re-read LAST tick's io state and re-fire the same press/release edges — see the
// block on the early-out for what that did to a click.
static bool s_frameLive     = false;
static HWND s_backendHwnd  = nullptr; // the window the win32 backend is bound to

// (The Phase-2b dockhost window is retired — the main frame IS the dockspace surface
//  now; see ImGuiShell_SetPrimarySurface below.)

// ── KIWI-UX (CLEANUP, C-7 + C-8): the dock-layout ini, BESIDE THE EXE ───────────
// ImGui stores the pointer it is handed, so the storage has to outlive the call —
// hence the function-local static.  The directory comes from Radiant_IniPath()
// (radiant_registry.cpp:9), which is the tree's one exe-relative path builder; the
// filename carries KIWI_LAYOUT_VERSION so it cannot drift out of step with the
// window-defaults reseed (kiwi_windows.h).  A user whose old kiwi_dock10.ini sat in
// some other CWD gets one layout rebuild, which is the same first-run path the
// version bumps already take.
static const char *DockIniPath()
{
    static char s_path[MAX_PATH];
    if ( !s_path[0] )
    {
        _snprintf( s_path, sizeof( s_path ), "%s", Radiant_IniPath() );
        s_path[sizeof( s_path ) - 1] = '\0';
        char *slash = strrchr( s_path, '\\' );      // keep "<exedir>\"
        if ( slash )
            slash[1] = '\0';
        else
            s_path[0] = '\0';                       // no directory: land in the CWD
        const size_t used = strlen( s_path );
        _snprintf( s_path + used, sizeof( s_path ) - used, "kiwi_dock%d.ini",
                   KIWI_LAYOUT_VERSION );
        s_path[sizeof( s_path ) - 1] = '\0';
    }
    return s_path;
}

extern int Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:118

// ── KIWI-UX (CLEANUP, C-9): retire the previous versions' dock inis ────────────
// The layout-migration mechanism is "change the filename", so every superseded
// kiwi_dock<n>.ini stayed on disk forever.  This deletes them ONCE, on a run where
// the CURRENT version's ini already exists (i.e. the migration is complete and the
// old file can no longer be needed), and only in the directory DockIniPath() built
// -- never a scan, never the CWD, never a pattern this function did not write.
static void DockIniRetireOlder()
{
    const char *path = DockIniPath();
    const char *slash = strrchr( path, '\\' );
    const size_t dirLen = slash ? (size_t)( slash - path ) + 1 : 0;
    char old[MAX_PATH];
    int removed = 0;
    for ( int v = 1; v < KIWI_LAYOUT_VERSION; ++v )
    {
        if ( dirLen >= sizeof( old ) )
            break;
        memcpy( old, path, dirLen );
        _snprintf( old + dirLen, sizeof( old ) - dirLen, "kiwi_dock%d.ini", v );
        old[sizeof( old ) - 1] = '\0';
        if ( ::GetFileAttributesA( old ) == INVALID_FILE_ATTRIBUTES )
            continue;
        if ( ::DeleteFileA( old ) )
        {
            Sys_Printf( "Dock layout: removed superseded %s\n", old );
            ++removed;
        }
        else
            Sys_Printf( "Dock layout: could not remove superseded %s\n", old );
    }
    (void)removed;                       // nothing is printed when nothing was there
}

static bool ImGuiShell_Enabled()
{
    // Full-ImGui transition (user directive 2026-08-08): the shell IS the editor UI —
    // always on.  (The -imgui flag gated the Phase-2a overlay era; kept as a function
    // so the call sites read the same.)
    return true;
}

// ── Primary surface: the main frame hosts the dockspace ─────────────────────────
// radiant_main.cpp registers the frame's swap chain (slot 6) at boot and hands the HWND
// here. From then on the ImGui frame renders on the FRAME (dockspace + panels + the five
// viewport dock tabs below); the camera-window overlay is only the pre-boot fallback.
static HWND s_primary = nullptr;

void ImGuiShell_SetPrimarySurface( HWND hwnd )
{
    s_primary = hwnd;
}

bool ImGuiShell_PrimaryActive()
{
    return s_primary != nullptr;
}

// Pump-side gate: keys go to the focused ImGui text field, not the accelerator table.
// KIWI-UX (shakeout B) — AUDITED, DELIBERATELY UNCHANGED.  This gate is already the
// narrow one: WantTextInput is set only while an InputText/InputFloat actually owns the
// keyboard (imgui.cpp:5760 — it comes straight from WantTextInputNextFrame, which only
// the text widgets raise), so it covers the §15 palette's search field and the panel
// number fields and nothing else.  It is NOT what killed hotkeys over the 3D view; see
// the WantCaptureKeyboard note at the bottom of this file (ImGuiShell_HandleMessage).
bool ImGuiShell_WantsKeyboard()
{
    return s_shellInited && ImGui::GetIO().WantTextInput;
}

// Auto-close a tool panel when the user clicks off it — called by each panel BETWEEN its
// ImGui::Begin() and ImGui::End() (the window must be the current one). A per-window latch
// (focused-at-least-once) means a panel just opened by a hotkey does NOT self-close before
// it receives focus, and closing here clears *p_open so the SAME hotkey reopens it next
// press (no 2-press desync). Windows the user has docked are exempt — only free-floating
// pop-outs auto-close.
void ImGuiShell_CloseOnFocusLoss( bool *p_open )
{
    // Per-window state: absent = never focused yet (still appearing — must NOT close, or a
    // panel opened from a menu/hotkey flashes for one frame and vanishes); present = has held
    // focus, value counts CONSECUTIVE unfocused frames since. A freshly-opened FLOATING panel
    // (multi-viewport OS window) grabs focus on its appearing frame, then can bounce unfocused
    // for a frame or two while the OS window settles / the continuously-repainted frame briefly
    // reactivates. A short grace absorbs that bounce; a real click-off still closes in ~0.1 s.
    static std::unordered_map<ImGuiID, int> s_unfocused;
    static const int kGraceFrames = 8;    // ~130 ms at 60 fps — imperceptible for click-off

    if ( !p_open || !*p_open )
        return;
    ImGuiWindow *w = ImGui::GetCurrentWindow();
    if ( !w )
        return;
    if ( w->DockIsActive )          // docked → user parked it deliberately; don't auto-close
    {
        s_unfocused.erase( w->ID );
        return;
    }
    const bool focused = ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows );
    if ( focused )
    {
        s_unfocused[w->ID] = 0;     // has-been-focused, 0 unfocused frames
        return;
    }
    auto it = s_unfocused.find( w->ID );
    if ( it == s_unfocused.end() )  // never focused yet → still appearing → keep it open
        return;
    if ( ++it->second >= kGraceFrames )   // held focus, now lost it for real → close
    {
        *p_open = false;
        s_unfocused.erase( it );
    }
}

// Command-side tab focus (the dock-world twin of the old inspector-mode toggles: the
// "O"/texture-view hotkeys used to flip the MFC inspector tab strip). Consumed at the
// top of the next dockspace frame.
static char s_focusTab[64] = { 0 };

void ImGuiShell_FocusTab( const char *title )
{
    if ( title )
    {
        strncpy( s_focusTab, title, sizeof( s_focusTab ) - 1 );
        s_focusTab[sizeof( s_focusTab ) - 1] = '\0';
    }
}

// ── Phase 5: viewports as RTT images ────────────────────────────────────────────
// Each of the four render viewports is drawn as ImGui::Image(RTT texture) inside its dock
// window — NOT a native child parked over the surface. The image's content-region size is
// recorded here so the NEXT tick renders that viewport's RT at the right size (one-frame
// lag on resize, imperceptible). The console (an EDIT control) is still a native child.
extern void CamWnd_RenderToRT( int w, int h );   // camwnd.cpp
extern void XYWnd_RenderToRT( int w, int h );    // xywnd.cpp
// the non-drawing half of an XY tick, owed even when the
// dirty gate skips the render.
extern void XYWnd_TickSkipped( int w, int h );   // xywnd.cpp:4162
extern void ZWnd_RenderToRT( int w, int h );     // z.cpp
extern void TexWnd_RenderToRT( int w, int h );   // texwnd.cpp

static int s_cellW[RTT_COUNT] = { 0, 0, 0, 0 };
static int s_cellH[RTT_COUNT] = { 0, 0, 0, 0 };
static IDirect3DDevice9 *s_device = nullptr;   // stashed for the opaque-image draw callback

// ── Viewport input dispatch ──────────────────────────────────────────────────────
// The viewport image is an ImGui item; when hovered (or during a drag this viewport
// owns) we translate image-relative mouse coords into the extracted *_On* handlers.
// Software drag-capture: once a button goes down over a viewport, that viewport keeps
// receiving move/up until every button releases — so a drag that leaves the image cell
// still tracks (the OS-SetCapture the handlers used is neutered under RTT).
extern void CamWnd_OnLButtonDown( HWND, unsigned int, int, int );
extern void CamWnd_OnLButtonUp( unsigned int );
extern void CamWnd_OnRButtonDown( HWND, unsigned int, int, int );
extern void CamWnd_OnRButtonUp( HWND, unsigned int, int, int );
extern void CamWnd_OnMouseMove( HWND, unsigned int, int, int );
// KIWI-UX (ROUND BG, ITEM 2): the per-frame "should the cursor be hidden right now?"
// recomputation.  Signature copied from its definition, camwnd.cpp
// `void CamWnd_CursorReconcile( bool legacyCamDragLive )`.
extern void CamWnd_CursorReconcile( bool legacyCamDragLive );
extern void XYWnd_OnLButtonDown( HWND, unsigned int, int, int );
extern void XYWnd_OnLButtonUp( unsigned int, int, int );
extern void XYWnd_OnMouseMove( unsigned int, int, int );
extern void XYWnd_OnRButtonDown( HWND, unsigned int, int, int );
extern void XYWnd_OnRButtonUp( HWND, unsigned int );
extern void XYWnd_OnMButtonDown( HWND, unsigned int, int, int );
extern void XYWnd_OnMButtonUp();
extern int  XYWnd_OnMouseWheel( HWND, short, int, int );
extern void ZWnd_OnLButtonDown( HWND, unsigned int, int, int );
extern void ZWnd_OnRButtonDown( HWND, unsigned int, int, int );
extern void ZWnd_OnMouseMove( HWND, unsigned int, int, int );
extern void ZWnd_OnLButtonUp( unsigned int );
extern void ZWnd_OnRButtonUp( unsigned int );
extern void TexWnd_OnLButtonDown( int, int );
extern void TexWnd_OnRButtonDown( unsigned int, int, int );
extern void TexWnd_OnRButtonUp( unsigned int, int, int );
extern int  TexWnd_OnMouseWheel( short );
extern int  TexWnd_GetScroll();       // texwnd.cpp — shell scrollbar accessors (native bar hidden)
extern int  TexWnd_GetScrollMax();
extern void TexWnd_SetScroll( int n );

// KIWI-UX: RADIANT_UX_DESIGN Phase-1b camera layer (kiwi_viewport.cpp). Each input entry
// point returns true only when it CONSUMED the event, and returns false unconditionally
// while the modern-input master toggle is off — so with the toggle off the camera dispatch
// below is exactly the pre-Phase-1b legacy one. See kiwi_viewport.h for which legacy path
// each consumed event replaces.
extern bool KiwiVP_CameraButtonDown( int btn, int imgX, int imgY, bool shift, bool ctrl );
extern bool KiwiVP_CameraMouseMove ( int imgX, int imgY );
extern bool KiwiVP_CameraButtonUp  ( int btn, int imgX, int imgY );
extern bool KiwiVP_CameraWheel     ( float steps, int imgX, int imgY );
extern void KiwiVP_CameraHover     ( int imgX, int imgY, bool over );
extern void KiwiVP_CameraTick      ( bool cursorOver );
extern bool KiwiVP_CameraAbort     ();
extern bool KiwiVP_DrawCameraOverlay( float imgMinX, float imgMinY, float imgW, float imgH );

static rttViewport_t s_inputOwner = RTT_COUNT;   // RTT_COUNT == none capturing
static ImVec2        s_imgMin[RTT_COUNT];         // image top-left in screen coords
static bool          s_hovered[RTT_COUNT];        // image hovered this frame (recorded in Draw)
static float         s_wheel[RTT_COUNT];          // wheel delta captured DURING the frame — must
                                                  // be read before Render()/EndFrame() zeroes
                                                  // io.MouseWheel (imgui.cpp:6442); the post-present
                                                  // dispatch would otherwise always see 0.

// Camera-image mousespace for the terrain-paint cursor ring (see ImGuiShell_CameraPaintCursor).
static bool          s_camMouseOver = false;
static int           s_camMouseX = 0, s_camMouseY = 0;   // image-relative, top-left origin
static int           s_camImgW = 0, s_camImgH = 0;

// Bridge for CamWnd_Draw's terrain-paint brush ring: the cursor position + size RELATIVE to the
// camera RTT image (the 3D viewport panel). Returns false when the cursor isn't over the camera
// image (so the ring isn't drawn), replacing the old ScreenToClient on the hidden native child.
bool ImGuiShell_CameraPaintCursor( int *x, int *y, int *w, int *h )
{
    if ( !s_camMouseOver )
        return false;
    if ( x ) *x = s_camMouseX;
    if ( y ) *y = s_camMouseY;
    if ( w ) *w = s_camImgW;
    if ( h ) *h = s_camImgH;
    return true;
}

// ── KIWI-UX (ROUND AI, ITEM 3): the CAMERA IMAGE's rect, in SCREEN coords ────
// The in-command options panel (kiwi_cmdoptions.cpp) is a real ImGui window, not
// an ImDrawList overlay — kiwi_hints.h forbids ImGui ITEMS inside the camera image
// and the panel is interactive by definition — so it needs the image's rect to
// anchor itself to the viewport the way Plasticity's dialog anchors to its
// (plasticity/src/components/dialog/Dialog.tsx:32 `absolute bottom-2 left-2`,
// mounted INSIDE `<plasticity-viewport>`, index.html:31).  Everything here is
// already recorded for the terrain-paint bridge above; this only publishes it.
// False when the camera viewport has not been drawn this session.
bool ImGuiShell_CameraImageRect( float *x, float *y, float *w, float *h )
{
    if ( s_camImgW < 1 || s_camImgH < 1 )
        return false;
    if ( x ) *x = s_imgMin[RTT_CAMERA].x;
    if ( y ) *y = s_imgMin[RTT_CAMERA].y;
    if ( w ) *w = (float)s_camImgW;
    if ( h ) *h = (float)s_camImgH;
    return true;
}

static HWND VP_Hwnd( rttViewport_t id )
{
    switch ( id )
    {
    case RTT_CAMERA:  return g_qeglobals.d_hwndCamera;
    case RTT_XY:      return g_qeglobals.d_hwndXY;
    case RTT_Z:       return g_qeglobals.d_hwndZ;
    case RTT_TEXTURE: return g_qeglobals.d_hwndTexture;
    default:          return nullptr;
    }
}

// MK_* flags from the current ImGui mouse/keyboard state (what the Win32 handlers expect).
static unsigned int VP_Flags()
{
    const ImGuiIO &io = ImGui::GetIO();
    unsigned int f = 0;
    if ( io.MouseDown[0] ) f |= MK_LBUTTON;
    if ( io.MouseDown[1] ) f |= MK_RBUTTON;
    if ( io.MouseDown[2] ) f |= MK_MBUTTON;
    if ( io.KeyCtrl )      f |= MK_CONTROL;
    if ( io.KeyShift )     f |= MK_SHIFT;
    return f;
}

static void VP_Down( rttViewport_t id, HWND hw, int btn, unsigned int f, int x, int y )
{
    switch ( id )
    {
    case RTT_CAMERA:
        if ( btn == 0 ) CamWnd_OnLButtonDown( hw, f, x, y );
        else if ( btn == 1 ) CamWnd_OnRButtonDown( hw, f, x, y );
        break;
    case RTT_XY:
        if ( btn == 0 ) XYWnd_OnLButtonDown( hw, f, x, y );
        else if ( btn == 1 ) XYWnd_OnRButtonDown( hw, f, x, y );
        else if ( btn == 2 ) XYWnd_OnMButtonDown( hw, f, x, y );
        break;
    case RTT_Z:
        if ( btn == 0 ) ZWnd_OnLButtonDown( hw, f, x, y );
        else if ( btn == 1 ) ZWnd_OnRButtonDown( hw, f, x, y );
        break;
    case RTT_TEXTURE:
        if ( btn == 0 ) TexWnd_OnLButtonDown( x, y );
        else if ( btn == 1 ) TexWnd_OnRButtonDown( f, x, y );
        break;
    default: break;
    }
}

static void VP_Up( rttViewport_t id, HWND hw, int btn, unsigned int f, int x, int y )
{
    switch ( id )
    {
    case RTT_CAMERA:
        if ( btn == 0 ) CamWnd_OnLButtonUp( f );
        else if ( btn == 1 ) CamWnd_OnRButtonUp( hw, f, x, y );
        break;
    case RTT_XY:
        if ( btn == 0 ) XYWnd_OnLButtonUp( f, x, y );
        else if ( btn == 1 ) XYWnd_OnRButtonUp( hw, f );
        else if ( btn == 2 ) XYWnd_OnMButtonUp();
        break;
    case RTT_Z:
        if ( btn == 0 ) ZWnd_OnLButtonUp( f );
        else if ( btn == 1 ) ZWnd_OnRButtonUp( f );
        break;
    case RTT_TEXTURE:
        if ( btn == 1 ) TexWnd_OnRButtonUp( f, x, y );
        break;
    default: break;
    }
}

static void VP_Move( rttViewport_t id, HWND hw, unsigned int f, int x, int y )
{
    switch ( id )
    {
    case RTT_CAMERA: CamWnd_OnMouseMove( hw, f, x, y ); break;
    case RTT_XY:     XYWnd_OnMouseMove( f, x, y );       break;
    case RTT_Z:      ZWnd_OnMouseMove( hw, f, x, y );    break;
    default: break;   // texture browser has no mouse-move handler
    }
}

static void VP_Wheel( rttViewport_t id, HWND hw, short dz, int sx, int sy )
{
    switch ( id )
    {
    case RTT_XY:      XYWnd_OnMouseWheel( hw, dz, sx, sy ); break;
    case RTT_TEXTURE: TexWnd_OnMouseWheel( dz );            break;
    default: break;   // camera/Z: no wheel
    }
}

// Dispatch one viewport's input. Called from the PUMP (after the frame presents), NOT
// during the ImGui frame — a viewport handler may pop a modal TrackPopupMenu (RMB context
// menu) whose nested message loop must not run inside the compositing frame's D3D scene
// bracket. io mouse-click/release edges are still valid here (same tick, before the next
// NewFrame); hover was recorded during the frame (IsItemHovered needs the item context).
// [The physical-cursor helper is defined first, immediately below, because both this
//  function and ImGuiShell_ReleaseViewportInput need it.]

// ── KIWI-UX (ROUND AB, ITEM 2): the PHYSICAL cursor, mapped into image space ──
// Multi-viewport is deliberately OFF in this shell (see the note at the config-flag
// site below), so ImGui screen space IS the backend window's CLIENT space and
// s_imgMin[] is directly comparable to a ScreenToClient'd GetCursorPos.  Deliberately
// UNCLAMPED: a value left of / above the image is negative and a value past it
// overflows the image, and BOTH are correct — a drag that has left the cell is still
// pointing somewhere, and every consumer of these coordinates is a ray/plane
// projection that handles the whole plane, not an array index.
static bool VP_PhysicalImagePos( rttViewport_t id, int *x, int *y )
{
    if ( !s_backendHwnd )
        return false;
    POINT p;
    if ( !::GetCursorPos( &p ) || !::ScreenToClient( s_backendHwnd, &p ) )
        return false;
    *x = (int)( (float)p.x - s_imgMin[id].x );
    *y = (int)( (float)p.y - s_imgMin[id].y );
    return true;
}

static void ImGuiShell_ViewportInput( rttViewport_t id, bool hovered )
{
    const ImGuiIO &io = ImGui::GetIO();
    HWND hw = VP_Hwnd( id );
    const unsigned int flags = VP_Flags();
    int mx = (int)( io.MousePos.x - s_imgMin[id].x );
    int my = (int)( io.MousePos.y - s_imgMin[id].y );

    const bool owns = ( s_inputOwner == id );

    // ── KIWI-UX (ROUND AB, ITEM 2): A LIVE DRAG NEVER LOSES THE CURSOR ──────
    // WM_MOUSELEAVE makes the backend post io.AddMousePosEvent(-FLT_MAX, -FLT_MAX)
    // (imgui_impl_win32.cpp:789), and its GetCursorPos fallback (:389-398) only
    // re-supplies a position while the app is FOREGROUND.  Drag off the window, let
    // something else take the foreground, and io.MousePos stays at -FLT_MAX: the
    // subtraction above then produces garbage and the gesture is fed nonsense.
    // While this viewport OWNS the drag we ask the OS instead.  Only then — inside
    // the window io.MousePos is the authoritative, event-synced value, and the XY
    // RMB pan RE-CENTRES the cursor every move (xywnd.cpp), so preferring the
    // physical position unconditionally would fight it.
    if ( owns && !ImGui::IsMousePosValid() )
        VP_PhysicalImagePos( id, &mx, &my );

    // KIWI-UX: the Phase-1b camera layer gets first refusal on CAMERA input only
    // (orbit / dolly / marquee — RADIANT_UX_DESIGN §10/§12). This is the ONE gate:
    // every KiwiVP_Camera* returns false while the master toggle is off, so the
    // legacy arms below then run exactly as they did before.
    const bool kiwiCam = ( id == RTT_CAMERA );

    if ( s_inputOwner == RTT_COUNT && hovered )
    {
        bool kiwiTook = false;
        for ( int b = 0; b < 3; ++b )
            if ( ImGui::IsMouseClicked( b ) )
            {
                s_inputOwner = id;
                if ( kiwiCam && KiwiVP_CameraButtonDown( b, mx, my, io.KeyShift, io.KeyCtrl ) )
                {
                    kiwiTook = true;
                    continue;
                }
                VP_Down( id, hw, b, flags, mx, my );
            }
        if ( s_wheel[id] != 0.0f )
        {
            if ( !( kiwiCam && KiwiVP_CameraWheel( s_wheel[id], mx, my ) ) )
                VP_Wheel( id, hw, (short)( s_wheel[id] * 120.0f ), mx, my );   // image-relative
        }
        const bool anyDown = io.MouseDown[0] || io.MouseDown[1] || io.MouseDown[2];
        if ( kiwiCam )
        {
            // §18 hover pick: once per frame, cursor over the image, NO button down.
            KiwiVP_CameraHover( mx, my, !anyDown && !kiwiTook );
        }
        // ══ KIWI-UX (ROUND BG, ITEM 2) — THE INVISIBLE-CURSOR ROOT CAUSE ══════════
        // USER REPORT: "My mouse went invisible while using the UV editor."
        //
        // This is the UNOWNED branch: no viewport claimed a press.  It used to feed
        // VP_Move unconditionally with VP_Flags(), which is the LIVE MK_* mask — so a
        // drag that belongs to an ImGui WIDGET, merely passing over a viewport image,
        // arrived at the legacy move handler as a viewport drag.  It reaches the camera
        // image because round Y's IsItemHovered( AllowWhenBlockedByActiveItem ) (below,
        // in the Draw path) deliberately stops another item's ActiveId from cancelling
        // the image's hover — correct for its own purpose, and exactly what lets a UV
        // editor drag "hover" the 3D view.  The chain then was:
        //   VP_Move -> CamWnd_OnMouseMove -> CamWnd_MouseMoved( MK_RBUTTON, .. )
        //     -> CamWnd_PositionDrag (camera_mode 1, the stock pref, camwnd.cpp:4098)
        //        -> SetCursorPos( m_ptCursor ) + ShowCursor( FALSE )   camwnd.cpp:4117
        // every tick of the drag, and the ONLY restore is Cam_MouseUp's
        // ShowCursor(TRUE)-until-visible loop (camwnd.cpp:4032), which runs from VP_Up —
        // which needs s_inputOwner == this viewport, which was never set.  So the
        // ShowCursor counter stayed deeply negative and the cursor was gone APP-WIDE.
        //
        // The rule, and it is the move-direction twin of the one kiwi_viewport.cpp's
        // GESTURE OWNERSHIP note states for releases: a legacy move handler is
        // BUTTON-MASK DRIVEN, so it may only be fed a nonzero mask for a press it
        // actually saw.  With no button down this is the ordinary hover move and is
        // unchanged; a press claimed THIS TICK sets s_inputOwner above and still gets
        // its move.  (The belt-and-braces half — a per-frame recomputation of whether
        // the cursor should be hidden at all — is CamWnd_CursorReconcile, called from
        // the dispatch entry point below.)
        if ( !kiwiTook && ( s_inputOwner == id || !anyDown ) )
            VP_Move( id, hw, flags, mx, my );
    }
    else if ( owns )
    {
        if ( !( kiwiCam && KiwiVP_CameraMouseMove( mx, my ) ) )
            VP_Move( id, hw, flags, mx, my );
        for ( int b = 0; b < 3; ++b )
            if ( ImGui::IsMouseReleased( b ) )
            {
                if ( kiwiCam && KiwiVP_CameraButtonUp( b, mx, my ) )
                    continue;
                VP_Up( id, hw, b, flags, mx, my );
            }
        if ( !io.MouseDown[0] && !io.MouseDown[1] && !io.MouseDown[2] )
            s_inputOwner = RTT_COUNT;
    }
    else if ( kiwiCam )
    {
        KiwiVP_CameraHover( mx, my, false );           // cursor elsewhere → drop the highlight
    }
}

// Force-release any in-progress viewport drag and restore anything it left in an abnormal
// state (the camera free-look hidden cursor). Called when input ownership was lost without a
// clean button-up — a popup from another window stole the OS mouse, or the app deactivated —
// so the release edge never reached ImGui and s_inputOwner would otherwise stay stuck (and the
// ShowCursor counter stuck negative → invisible cursor app-wide). Gated on actually owning a
// drag so the camera cursor-restore only runs when a camera drag was live (no over-increment).
// Force-release the OWNING viewport's drag and restore anything it left abnormal. Called ONLY
// as a stuck-state safety (see the guard below): a popup ate the OS mouse or the app
// deactivated, so the release edge never reached ImGui and the drag would otherwise wedge —
// e.g. the XY RMB pan re-centres the cursor every move while m_nButtonstate==MK_RBUTTON, so a
// lost RMB-up "steals" the mouse forever; the camera free-look leaves the cursor hidden. Each
// viewport's up-handler resets its own drag/button state (context menus are stubbed in this
// shell), so synthesizing the release IS the clean teardown — except the camera, whose
// OnRButtonUp would pop a menu, so it routes through CamWnd_AbortDrag (Cam_MouseUp only).
void ImGuiShell_AbortViewportInput()
{
    if ( !s_shellInited || s_inputOwner == RTT_COUNT )
        return;
    const rttViewport_t id = s_inputOwner;
    HWND hw = VP_Hwnd( id );
    if ( id == RTT_CAMERA )
    {
        // KIWI-UX: a modern gesture (orbit / marquee) tears ITSELF down — it never went
        // through Drag_Begin, so CamWnd_AbortDrag would be a foreign teardown (and its
        // ShowCursor loop is only correct for the free-look the modern path doesn't use).
        // Fall through to the ported teardown only when the live drag really is legacy.
        if ( !KiwiVP_CameraAbort() )
        {
            extern void CamWnd_AbortDrag();   // camwnd.cpp — Cam_MouseUp restore (re-shows the cursor)
            CamWnd_AbortDrag();
        }
    }
    else
    {
        for ( int b = 0; b < 3; ++b )     // XY/Z/texture: up-handlers just reset drag/button state
            VP_Up( id, hw, b, 0, 0, 0 );
    }
    s_inputOwner = RTT_COUNT;
}

// ── KIWI-UX (ROUND AB, ITEM 2): THE MISSING RELEASE IS A RELEASE ────────────
// USER REPORT, verbatim: *"sometimes when using a tool and then dragging your mouse
// off, it just resets the operation. it's super annoying. it just says 'move
// selection undone.' or something similar to in console and snaps back to where it
// was."*
//
// That console line is `Sys_Printf( "%s undone.\n", ... )` (undo.cpp:984), i.e. a
// REAL Undo_Undo, and the chain that reaches it without a keystroke is exactly one:
//
//   ImGuiShell_ForceReleaseIfStuck  (below)
//     -> ImGuiShell_AbortViewportInput            (imgui_shell.cpp)
//        -> KiwiVP_CameraAbort                    (kiwi_viewport.cpp)
//           -> KiwiGizmo_Abort                    (kiwi_gizmo.cpp)
//              -> KiwiCmd_Cancel                  (kiwi_command.cpp)
//                 -> KiwiCmd_UndoCancel           (kiwi_command.cpp)
//                    -> Undo_Undo                 ("move selection undone.")
// KIWI-UX (CLEANUP, C-14): all five line numbers here were wrong (they named
// :405/:893/:685/:1262/:2039 for what were then :493/:996/:1336/:1398/:2244).  The
// SYMBOL is the durable half of a cite and the line number is not, so the numbers
// are dropped rather than re-fixed — see C-15 on the convention.
//
// The guard's TRIGGER was right and its ACTION was wrong.  It fires precisely when
// "this viewport owns a drag AND no physical mouse button is down" — which is not
// an ambiguous state at all: THE USER HAS LET GO.  The release edge simply never
// reached ImGui, because the WM_*BUTTONUP went somewhere else — capture was
// dropped (WM_CANCELMODE / another process taking it / an alt-tab), or the app lost
// the foreground while the cursor was outside the client area.  Round Z wrote the
// guard for the gestures that have no result to keep (the XY RMB pan, the camera
// free-look), where "tear it down" IS the clean answer; for a MODAL EDIT it throws
// the user's drag away and prints an undo line for a gesture they finished.
//
// So the guard now SYNTHESIZES THE RELEASE, at the real cursor position, through
// the very same up-handlers a release inside the image would have run — which is
// the behaviour the report asks for and the behaviour of every serious editor:
// a drag captures, and letting go anywhere commits it.
//
// PLASTICITY, VERIFIED: ViewportControl.onPointerDown
// (plasticity/src/components/viewport/ViewportControl.ts:95) registers the drag's
// pointerup and pointermove on `document`, not on the canvas (:111-112), and only
// unregisters them through the gesture's own Disposable (:113-114) — so a live drag
// hears every move and the release wherever the cursor is.  Its 'dragging' arms are
// a bare continueDrag (:167-169) and a bare endDrag (:210-216): no bounds test, no
// hover test, no timeout and NO CANCEL anywhere in that state machine.  Its
// navigation half uses the browser's real capture for the same reason —
// domElement.setPointerCapture(event.pointerId) at OrbitControls.ts:291, released
// at :329.  Cancellation there is an explicit `escape`, never an inference about
// where the cursor went.
//
// ABORT REMAINS, for the one case that is genuinely not a release: a live drag
// whose owning viewport can offer no release semantics (nothing in io still reads
// as down, so there is no button to release).  That falls through to the old
// teardown, so no state can wedge.
static void ImGuiShell_ReleaseViewportInput()
{
    const rttViewport_t id = s_inputOwner;
    if ( id == RTT_COUNT )
        return;
    HWND hw = VP_Hwnd( id );

    // The image-space position the release happened at.  Prefer the OS cursor: by
    // definition we got here because ImGui's own view of the mouse is stale.
    int mx = (int)( ImGui::GetIO().MousePos.x - s_imgMin[id].x );
    int my = (int)( ImGui::GetIO().MousePos.y - s_imgMin[id].y );
    VP_PhysicalImagePos( id, &mx, &my );

    ImGuiIO &io = ImGui::GetIO();
    bool released = false;
    for ( int b = 0; b < 3; ++b )
    {
        if ( !io.MouseDown[b] )
            continue;
        // Un-wedge ImGui itself.  Its MouseDown[] is stuck true because the up
        // message never arrived; leaving it that way means the NEXT genuine press
        // produces no IsMouseClicked edge.  Queued, applied at the next NewFrame.
        io.AddMouseButtonEvent( b, false );
        if ( id == RTT_CAMERA )
        {
            // Camera only: NEVER VP_Up here.  CamWnd_OnRButtonUp pops the classic
            // context menu (camwnd.cpp:3568) — the same reason
            // ImGuiShell_AbortViewportInput routes the camera around VP_Up.
            if ( KiwiVP_CameraButtonUp( b, mx, my ) )
                released = true;
        }
        else
        {
            VP_Up( id, hw, b, 0, mx, my );   // flags 0: every button is physically up
            released = true;
        }
    }

    if ( !released )
    {
        ImGuiShell_AbortViewportInput();     // clears s_inputOwner itself
        return;
    }
    s_inputOwner = RTT_COUNT;
}

// KIWI-UX (ROUND Y, ITEM 6): the stuck-drag guard, hoisted out of the dispatch tail
// so the skipped-frame early-out can still run it.  ROUND AB, ITEM 2 changed only
// what it DOES — see ImGuiShell_ReleaseViewportInput above.
static void ImGuiShell_ForceReleaseIfStuck()
{
    if ( s_inputOwner == RTT_COUNT )
        return;
    const bool anyDown = ( ::GetAsyncKeyState( VK_LBUTTON ) & 0x8000 ) ||
                         ( ::GetAsyncKeyState( VK_RBUTTON ) & 0x8000 ) ||
                         ( ::GetAsyncKeyState( VK_MBUTTON ) & 0x8000 );
    if ( !anyDown )
        ImGuiShell_ReleaseViewportInput();
}

// Pump entry (post-present): route input to all four viewports using the rects/hover the
// compositing frame recorded. Outside the scene bracket, so a context-menu modal is safe.
void ImGuiShell_DispatchViewportInput()
{
    if ( !s_shellInited || !s_primary )
        return;

    // ── KIWI-UX (ROUND BG, ITEM 2): THE PER-FRAME CURSOR RECOMPUTATION ─────────
    // FIRST in the tick, and OUTSIDE the skipped-frame early-out below, because a
    // skipped frame is one of the ways the release that would have restored the
    // cursor goes missing.  The predicate is physical (GetAsyncKeyState, the same
    // source the stuck-drag guard trusts for exactly this reason) and names the one
    // state in which a hidden cursor is CORRECT: this shell owns a live camera drag
    // that the LEGACY handlers are driving.  Everything else — a drag that was never
    // ours, a lost release, focus loss, a popup eating the mouse — restores.
    // See CamWnd_CursorReconcile (camwnd.cpp) for the latch it is undoing.
    {
        const bool physDown = ( ::GetAsyncKeyState( VK_LBUTTON ) & 0x8000 ) ||
                              ( ::GetAsyncKeyState( VK_RBUTTON ) & 0x8000 ) ||
                              ( ::GetAsyncKeyState( VK_MBUTTON ) & 0x8000 );
        CamWnd_CursorReconcile( s_inputOwner == RTT_CAMERA && physDown );
    }

    // ── KIWI-UX (ROUND Y, ITEM 6): ONLY DISPATCH FOR A FRAME THAT HAPPENED ──
    // USER REPORT: "Clicks are still ignored sometimes."
    //
    // The pump calls this every tick unconditionally, but ImGuiShell_DrawOverlay
    // has four early-outs (re-entrancy, no primary surface, wrong HWND, no
    // authorized frame) and any of them skips the NewFrame that produces the io
    // state read here.  When that happens io.MousePos, IsMouseClicked/Released and
    // s_hovered[] are all LAST tick's, and the same press/release EDGES are
    // dispatched a second time.  The press is protected by s_inputOwner; the
    // RELEASE is not, and re-firing it runs the legacy up-handler for a press the
    // viewport never saw — which leaves drag state inconsistent and is one of the
    // ways a following click goes nowhere.
    //
    // s_frameLive is set by DrawOverlay only once it is past every early-out and
    // has actually called NewFrame, and is consumed here.  Nothing else reads it,
    // and it cannot deadlock a gesture: the stuck-drag guard below still runs
    // (it is physical-key based), which is exactly the safety net for the case
    // where a release edge is genuinely lost.
    if ( !s_frameLive )
    {
        ImGuiShell_ForceReleaseIfStuck();
        return;
    }
    s_frameLive = false;

    for ( int id = 0; id < RTT_COUNT; ++id )
        ImGuiShell_ViewportInput( (rttViewport_t)id, s_hovered[id] );

    // KIWI-UX (shakeout A): the keyboard camera fly. ONE poll per tick, AFTER the
    // loop so the gesture state it reads (is the RMB mouselook live?) is this
    // tick's. Keys are read with GetAsyncKeyState inside, so nothing here touches
    // the message queue and no hotkey can double-fire. No-op while the modern-input
    // master toggle is off.
    KiwiVP_CameraTick( s_hovered[RTT_CAMERA] );

    // Stuck-drag guard — runs AFTER the loop so the NORMAL per-frame release path above always
    // wins (it calls the viewport's real up-handler and clears s_inputOwner). Only if we STILL
    // own a drag yet no physical mouse button is down (the release edge was eaten by a popup
    // that stole the OS mouse) do we force the teardown. Ordering matters: doing this before the
    // loop pre-empted the normal RMB-up, leaving the XY pan's m_nButtonstate stuck (cursor
    // "stolen"). GetAsyncKeyState reads the true physical state regardless of focus.
    ImGuiShell_ForceReleaseIfStuck();
}

// Draw the viewport image OPAQUE: the RT is A8R8G8B8 and the scene's written alpha is not
// guaranteed to be 1, so alpha-blending the image could make it transparent. This callback
// turns alpha-blend off for the image draw; ImDrawCallback_ResetRenderState restores it.
static void ImGuiShell_ImageOpaqueCb( const ImDrawList *, const ImDrawCmd * )
{
    if ( s_device )
        s_device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
}

// KIWI-UX (§9, shakeout B): the dockspace id of the CURRENT frame, recorded by
// DrawOverlay right after DockSpaceOverViewport.  A window the user re-opens from the
// Windows menu is docked into it (SetNextWindowDockID, imgui.h:994 — the one public
// docking-placement call this vendored ImGui exposes; DockBuilderDockWindow is
// imgui_internal and is reserved for the first-run layout build).  Without this a
// re-opened window reappears wherever the ini last saw it, which for a window that was
// closed before the layout rebuild means "floating in the top-left corner".
static ImGuiID s_dockRoot = 0;

// KIWI-UX (ROUND W): the same id, for a panel that Begins ITSELF in its own file.
// kiwi_outliner.cpp needs the re-dock target for the JustOpened latch, and the
// alternative — moving its Begin/End up here — would put a 500-line panel body in
// the shell.  One accessor is the smaller seam.
ImGuiID ImGuiShell_DockRoot()
{
    return s_dockRoot;
}

// Draw one RTT viewport as an image + record its cell size for next tick's RT render.
// `win` (KIWI-UX §9) names the kiwi_windows flag; KIWI_WIN_COUNT = always-on, which is
// the camera and only the camera.  A closed window is skipped entirely (no Begin, no
// End) and an open one passes its flag as Begin's p_open, so the title-bar ✕ writes the
// same state the Windows menu writes.
// ── KIWI-UX (CLEANUP, C-12): ONE SPELLING OF "THIS VIEWPORT TOOK NO INPUT" ─────
// s_imgMin / s_hovered / s_wheel and the four s_cam* are written ONLY inside the
// `if ( tex )` arm below.  When RTT_GetTexture returns null — the first frames
// after a device reset, since RTT_ReleaseForReset frees every slot and EnsureSlot
// is lazy — the shell kept LAST frame's rect and hover, and
// ImGuiShell_DispatchViewportInput dispatched clicks against them.  The
// closed-window path was always careful to zero exactly these; this is that same
// zeroing, named, so the two paths cannot drift.  Cell sizes are deliberately NOT
// touched here: the no-texture path still records them, and that is what makes the
// RT get created at the right size next tick.
static void ClearViewportInputState( rttViewport_t id )
{
    s_hovered[id] = false;
    s_wheel[id]   = 0.0f;
    s_imgMin[id]  = ImVec2( 0.0f, 0.0f );
    if ( id == RTT_CAMERA )
    {
        s_camMouseOver = false;
        s_camMouseX = 0;
        s_camMouseY = 0;
    }
}

static void ImGuiShell_DrawViewportImage( rttViewport_t id, kiwiWindow_t win )
{
    const bool  always = ( win == KIWI_WIN_COUNT );
    const char *title  = always ? "Camera" : KiwiWindows_Title( win );
    bool       *p_open = always ? nullptr  : KiwiWindows_OpenPtr( win );

    if ( p_open && !*p_open )
    {
        // NOT DRAWN AT ALL — no Begin, no End.  The stale cell size is cleared here so
        // ImGuiShell_RenderViewportsToRT cannot keep rendering this viewport off-screen
        // from last frame's dimensions (it is gated on the flag too, belt and braces).
        s_cellW[id] = 0;
        s_cellH[id] = 0;
        ClearViewportInputState( id );      // KIWI-UX (CLEANUP, C-12)
        return;
    }
    if ( !always && KiwiWindows_JustOpened( win ) )
        ImGui::SetNextWindowDockID( s_dockRoot, ImGuiCond_Always );

    ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
    const bool open = ImGui::Begin( title, p_open,
                                    ImGuiWindowFlags_NoScrollbar |
                                    ImGuiWindowFlags_NoScrollWithMouse |
                                    ImGuiWindowFlags_NoCollapse );
    ImGui::PopStyleVar();
    if ( open )
    {
        // ── KIWI-UX (ROUND AC): the ALL / IN-USE filter, ON the Textures panel ──
        // USER REPORT, verbatim: "when loading an existing map, the textures panel
        // only shows the textures in use.  How do I see all available textures so I
        // can add onto the map?  Seems like an oversight"
        //
        // Not an oversight in the port — it is the binary's own behaviour: every map
        // load ends in Texture_ShowInuse (map.cpp:546, faithful to 0x45B850), and
        // the way back is Textures→Show All (32973, Cmd_OnTexturesShowall).  What IS
        // an oversight is discoverability: that lives in the native menu bar, which
        // the 3D-first shell de-emphasised.  So the two filter verbs get buttons
        // right where the question arises.  They call the same texwnd primitives the
        // menu handlers call (mainfrm.cpp:2865/2879 — minus 32973's script-group
        // trigger overload, which is a selection act, not a filter, and would be a
        // baffling side effect on a browser button).  No RedrawWindow needed:
        // TexWnd_RenderToRT repaints every authorized frame.
        if ( id == RTT_TEXTURE )
        {
            extern LRESULT Texture_ShowAll();     // texwnd.cpp (0x45b730), as radiant_main.cpp:77
            extern LRESULT Texture_ShowInuse();   // texwnd.cpp (0x45B850), as map.cpp:166
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted( "Show:" );
            ImGui::SameLine();
            if ( ImGui::SmallButton( "All" ) )
                Texture_ShowAll();
            ImGui::SameLine();
            if ( ImGui::SmallButton( "In Use" ) )
                Texture_ShowInuse();
            // ── KIWI-UX (ROUND AE): the search bar ──────────────────────────
            // USER DIRECTIVE: "add a small searchbar in the textures bar next to
            // the new buttons".  The filter itself is the BINARY's own ported
            // machinery (TexWnd_FilterAccept's searchbar arm) — this box only
            // feeds it through TexWnd_SetSearchFilter (texwnd.cpp), which
            // lowercases and re-homes the scroll.  Every keystroke re-filters:
            // the browser repaints per authorized frame, so there is nothing to
            // refresh manually.  The field participates in the round-Y hover fix
            // (AllowWhenBlockedByActiveItem), so clicking from the focused box
            // straight into a viewport still lands.
            {
                extern void TexWnd_SetSearchFilter( const char *q );   // texwnd.cpp (ROUND AE)
                static char s_texSearch[64] = { 0 };
                ImGui::SameLine();
                ImGui::SetNextItemWidth( 150.0f );
                if ( ImGui::InputTextWithHint( "##texsearch", "search", s_texSearch,
                                               sizeof( s_texSearch ) ) )
                    TexWnd_SetSearchFilter( s_texSearch );
                if ( s_texSearch[0] )
                {
                    ImGui::SameLine();
                    if ( ImGui::SmallButton( "x##texsearchclr" ) )
                    {
                        s_texSearch[0] = '\0';
                        TexWnd_SetSearchFilter( "" );
                    }
                }
            }
        }

        ImVec2 avail = ImGui::GetContentRegionAvail();

        // Texture browser: reserve a column on the right for a real, draggable scrollbar
        // (the native WS_VSCROLL bar is hidden under RTT; the wheel alone left the list
        // un-navigable by mouse). Shrink the image so the scrollbar never overlaps textures.
        const bool  texScroll = ( id == RTT_TEXTURE ) && ( TexWnd_GetScrollMax() > 0 );
        const float sbW       = texScroll ? ImGui::GetStyle().ScrollbarSize : 0.0f;
        if ( sbW > 0.0f )
            avail.x -= sbW;

        const int w = (int)avail.x, h = (int)avail.y;
        if ( w >= 1 && h >= 1 )
        {
            s_cellW[id] = w;
            s_cellH[id] = h;
            IDirect3DTexture9 *tex = RTT_GetTexture( id );
            if ( tex )
            {
                ImDrawList *dl = ImGui::GetWindowDrawList();
                dl->AddCallback( ImGuiShell_ImageOpaqueCb, nullptr );
                // KIWI-UX (ROUND S): draw at the RT's OWN integer size, not at the
                // float `avail`.  The RT is created at ((int)avail.x, (int)avail.y)
                // (the two lines above), so blitting it across the fractional
                // remainder scaled it by avail.x/w horizontally and avail.y/h
                // vertically — two DIFFERENT factors, i.e. a real (if sub-pixel,
                // <=1 px on each axis) anisotropic stretch of everything in the
                // view.  Investigated as a candidate for the "cylinder looks
                // skewed" report; it is far too small to be that on its own (the
                // projection itself is isotropic — see camwnd.cpp's ortho block),
                // but it is a genuine mismatch and it costs nothing to remove.
                const ImVec2 imgSize( (float)w, (float)h );
                ImGui::Image( (ImTextureID)(intptr_t)tex, imgSize );
                dl->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
                // Record only — the actual input dispatch runs in the pump AFTER present
                // (ImGuiShell_DispatchViewportInput), so a context-menu modal can't nest
                // inside the compositing scene bracket.
                s_imgMin[id]   = ImGui::GetItemRectMin();
                // ── KIWI-UX (ROUND AU): THE CAMERA IMAGE IS A DROP TARGET ──
                // FIRST thing after the Image and nothing between, because
                // BeginDragDropTarget reads g.LastItemData: with the Image's id
                // of 0 it derives the target id from that item's RECT
                // (imgui.cpp:16032-16037), which is the documented pattern for
                // dropping onto an image.  It is a no-op unless a drag is live.
                if ( id == RTT_CAMERA )
                    KiwiEntBrowser_CameraDropTarget( s_imgMin[id].x, s_imgMin[id].y );
                if ( id == RTT_CAMERA ) KiwiModelBrowser_CameraDropTarget( s_imgMin[id].x, s_imgMin[id].y ); // KIWI-UX
                // ═══════════════════════════════════════════════════════════
                //  KIWI-UX (ROUND Y, ITEM 6) — A FOCUSED TEXT FIELD WAS
                //  SWALLOWING THE FIRST CLICK INTO THE VIEWPORT.
                // ═══════════════════════════════════════════════════════════
                // USER REPORT, verbatim: "Clicks are still ignored sometimes.
                // Makes it really annoying to work fast."
                //
                // ImGui::Image submits with id 0, and plain IsItemHovered()
                // CANCELS the hover whenever ANY other item owns g.ActiveId — the
                // only exemption is the window's own MoveId (imgui.cpp
                // IsItemHovered, the `cancel_is_hovered` block).  An InputText
                // holds ActiveId ACROSS FRAMES while it is focused, and the
                // viewport images are submitted BEFORE the panels, so on the frame
                // where the user clicks into the 3D view after typing anywhere —
                // the entity panel, prefs, the surface inspector, an outliner
                // rename, the view-cube grid type-in, the command palette's
                // auto-focused filter — s_hovered[] was false and
                // ImGuiShell_DispatchViewportInput never delivered the click at
                // all.  The field deactivates later in the same frame, so the
                // SECOND click works.  That is the whole "takes 2 tries" shape,
                // and it is why it looked random: it depended on whether the user
                // had typed since the last viewport click.
                //
                // AllowWhenBlockedByActiveItem removes exactly that cancellation
                // and nothing else.  Window OVERLAP is a separate test
                // (g.HoveredWindow) and is untouched, so a panel or a popup drawn
                // over the image still blocks it, which is what must keep
                // happening.  Round X's selUnmask fixed the OTHER "2 clicks"
                // report; this is the one it could not explain.
                bool imgHovered = ImGui::IsItemHovered( ImGuiHoveredFlags_AllowWhenBlockedByActiveItem );
                // ── KIWI-UX (ROUND AU): A LIVE DRAG-DROP OWNS THE MOUSE ─────
                // AllowWhenBlockedByActiveItem is exactly what makes an ImGui
                // DRAG read as "hovering the viewport": the source tile holds
                // ActiveId for the whole drag, and round Y's flag deliberately
                // removes that cancellation.  Without this the post-present
                // dispatch would feed CamWnd_OnMouseMove — and, on a drag that
                // BEGAN over the image, a press — while the user is only carrying
                // a payload across it.  The drop itself does not need the hover:
                // it arrives through BeginDragDropTarget above, inside the frame.
                // (compile fix: IsDragDropActive is imgui_internal.h-only in
                // 1.92.8; GetDragDropPayload() != NULL is the public spelling —
                // "returns NULL when drag and drop is finished or inactive",
                // imgui.h:1020.)
                if ( ImGui::GetDragDropPayload() != nullptr )
                    imgHovered = false;
                // KIWI-UX: the §11 selection-mode chip row + the §12 marquee rectangle, drawn
                // INSIDE the camera image (screen space; the world-space overlays go in the
                // Cam_Draw tail instead). A chip under the cursor takes the image's hover for
                // this frame so clicking one can never also start a marquee behind it.
                if ( id == RTT_CAMERA )
                {
                    // ROUND S: imgSize, not avail — the overlay must lay out over
                    // the pixels the image ACTUALLY occupies.
                    if ( KiwiVP_DrawCameraOverlay( s_imgMin[id].x, s_imgMin[id].y,
                                                   imgSize.x, imgSize.y ) )
                        imgHovered = false;
                }
                s_hovered[id]  = imgHovered;
                // Capture the wheel NOW (valid frame context, before EndFrame zeroes it). The
                // post-present dispatch consumes s_wheel[id]; reading io.MouseWheel there is
                // always 0. Item-hover keeps the wheel scoped to the viewport under the cursor.
                s_wheel[id]    = s_hovered[id] ? ImGui::GetIO().MouseWheel : 0.0f;

                // Terrain-paint cursor-ring bridge: CamWnd_Draw draws the brush ring during
                // RenderToRT, but it must position it in the CAMERA IMAGE's mousespace (the 3D
                // viewport panel), not via the hidden native camera child. Record the image-
                // relative cursor + image size here (top-left origin; the ring flips Y itself).
                if ( id == RTT_CAMERA )
                {
                    s_camMouseOver = s_hovered[id];
                    s_camMouseX = (int)( ImGui::GetIO().MousePos.x - s_imgMin[id].x );
                    s_camMouseY = (int)( ImGui::GetIO().MousePos.y - s_imgMin[id].y );
                    s_camImgW   = w;
                    s_camImgH   = h;
                }

                // The texture scrollbar itself. VSliderInt is inverted so the TOP of travel is
                // scroll 0; it re-reads TexWnd's live offset every frame, so the wheel path and
                // the bar stay in sync (both drive m_scrollY through TexWnd_CheckScroll).
                if ( sbW > 0.0f )
                {
                    const int maxS = TexWnd_GetScrollMax();
                    int inv = maxS - TexWnd_GetScroll();
                    ImGui::SameLine( 0.0f, 0.0f );
                    if ( ImGui::VSliderInt( "##texscroll", ImVec2( sbW, avail.y ),
                                            &inv, 0, maxS, "" ) )
                        TexWnd_SetScroll( maxS - inv );
                }
            }
            else
            {
                // KIWI-UX (CLEANUP, C-12): no RT this frame, so this viewport takes
                // no input this frame either.  (The bare ImGui::Dummy( avail ) that
                // used to reserve the space is gone — nothing needed it; End()
                // follows immediately.)
                ClearViewportInputState( id );
            }
        }
        else
        {
            ClearViewportInputState( id );   // KIWI-UX (CLEANUP, C-12): zero-area cell
        }
    }
    ImGui::End();
}

// ── Console ────────────────────────────────────────────────────────────────────
// The console is now ImGui-drawn text, NOT the native EDIT child. The old EDIT was parked
// over the dockspace via MoveWindow every frame; resizing thrashed it repainting over the
// D3D-composited frame → whole-screen flicker (the last instance of the native-child-over-D3D
// problem the RTT viewports already solved). console_print (win_qe3.cpp) tees its output here
// via ImGuiConsole_Append; the EDIT stays alive but is permanently hidden by
// ApplyViewportDocks (SW_HIDE), so nothing native paints over the scene.
static HWND        s_consoleHwnd    = nullptr;   // native EDIT — kept alive but always hidden

static std::string s_conText;
static bool        s_conScrollToBottom = false;

void ImGuiConsole_Append( const char *s )
{
    if ( !s || !*s )
        return;
    s_conText += s;
    // Bound the buffer (the EDIT trimmed at 400 lines); trim to the last ~96 KB on a line
    // boundary so the visible tail is never cut mid-line.
    const size_t cap = 96 * 1024;
    if ( s_conText.size() > cap )
    {
        size_t cut = s_conText.size() - cap;
        size_t nl  = s_conText.find( '\n', cut );
        if ( nl != std::string::npos )
            cut = nl + 1;
        s_conText.erase( 0, cut );
    }
    s_conScrollToBottom = true;
}

void ImGuiConsole_Clear()
{
    s_conText.clear();
    s_conScrollToBottom = true;
}

static void ImGuiShell_DrawConsoleTab( HWND child )
{
    s_consoleHwnd = child;      // ImGui owns the console now — the native EDIT stays hidden
    bool *p_open = KiwiWindows_OpenPtr( KIWI_WIN_CONSOLE );   // KIWI-UX §9
    if ( !p_open || !*p_open )
        return;                 // closed → no Begin at all (Windows menu / ✕ reopen it)
    if ( KiwiWindows_JustOpened( KIWI_WIN_CONSOLE ) )
        ImGui::SetNextWindowDockID( s_dockRoot, ImGuiCond_Always );
    if ( ImGui::Begin( "Console", p_open, ImGuiWindowFlags_NoCollapse ) )
    {
        // KIWI-UX (user directive): no Clear button, no chars readout — the
        // console is just the text.  ImGuiConsole_Clear stays callable.
        ImGui::BeginChild( "##conscroll", ImVec2( 0.0f, 0.0f ), false,
                           ImGuiWindowFlags_HorizontalScrollbar );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 4.0f, 1.0f ) );
        // TextUnformatted has a built-in clipper for long text inside a scroll region.
        ImGui::TextUnformatted( s_conText.c_str(), s_conText.c_str() + s_conText.size() );
        ImGui::PopStyleVar();
        // Snap to the newest line whenever new output arrived (the EDIT's EM_REPLACESEL-at-
        // caret behavior); when output is idle the flag stays clear so the user can scroll
        // back and read history freely.
        if ( s_conScrollToBottom )
            ImGui::SetScrollHereY( 1.0f );
        s_conScrollToBottom = false;
        ImGui::EndChild();
    }
    ImGui::End();
}

// Render all four viewports into their RT textures — called from the pump BEFORE the
// compositing frame (each does its own scene + suppressed Present; must be OUTSIDE the
// compositing frame's scene bracket). Uses the sizes recorded by last frame's images.
void ImGuiShell_RenderViewportsToRT()
{
    PROF_SCOPED( "RenderViewportsToRT" );
    if ( !s_primary )
        return;
    // KIWI-UX (ROUND AB, ITEM 1): NEVER build a scene while the device is lost or awaiting
    // reset.  The recovery this defers to is the compositing WM_PAINT's
    // R_SetupRendertarget_CheckDevice → R_TestDevice (radiant_main.cpp:198), whose active
    // render target is the window backbuffer — the reset path that already works.  Letting
    // the loop run instead means R_IssueRenderCommands' own R_CheckLostDevice
    // (r_rendercmds.cpp:278) runs the FULL R_RecoverLostDevice → R_ResetDevice cascade from
    // INSIDE the RTT bracket, i.e. with an app-created D3DPOOL_DEFAULT surface bound, and
    // R_ReleaseForShutdownOrReset tears down the editor VB pool halfway through a command
    // list that references it.  Composes with round V (r_init.cpp:4487 / :4803), which owns
    // the recreate side; this owns only "do not draw yet".
    if ( !RTT_DeviceHealthy() )
        return;
    // KIWI-UX (§9, shakeout B): a viewport whose dock window is CLOSED is not rendered.
    // Two independent guarantees, because getting this wrong costs a whole off-screen
    // scene per tick per hidden view: (a) the draw zeroes s_cell*[] the moment it stops
    // drawing a window, so the size these calls read is 0 and the RT render early-outs;
    // (b) the flag is checked here as well, so even a stale size can never resurrect a
    // hidden viewport.  The CAMERA has no flag — it is always rendered.
    CamWnd_RenderToRT( s_cellW[RTT_CAMERA],  s_cellH[RTT_CAMERA] );
    // The 2D views render only when dirty: when the gate says "clean" the RTT keeps its last
    // texture and the ImGui::Image below samples it as always.  Every uncertain answer is
    // "render" (kiwi_viewdirty.h), and a CLOSED view is marked dirty so reopening it can never
    // show a stale frame.  The CAMERA stays every-tick; the texture view is left alone.
    if ( KiwiWindows_IsOpen( KIWI_WIN_XY ) )
    {
        if ( KiwiViewDirty_ShouldRender( KIWI_DIRTYVIEW_XY, s_cellW[RTT_XY], s_cellH[RTT_XY] ) )
            XYWnd_RenderToRT ( s_cellW[RTT_XY],      s_cellH[RTT_XY] );
        else
            XYWnd_TickSkipped( s_cellW[RTT_XY],      s_cellH[RTT_XY] );
    }
    else
    {
        KiwiViewDirty_Mark( KIWI_DIRTYVIEW_XY );
    }
    if ( KiwiWindows_IsOpen( KIWI_WIN_Z ) )
    {
        if ( KiwiViewDirty_ShouldRender( KIWI_DIRTYVIEW_Z, s_cellW[RTT_Z], s_cellH[RTT_Z] ) )
            ZWnd_RenderToRT  ( s_cellW[RTT_Z],       s_cellH[RTT_Z] );
    }
    else
    {
        KiwiViewDirty_Mark( KIWI_DIRTYVIEW_Z );
    }
    if ( KiwiWindows_IsOpen( KIWI_WIN_TEXTURE ) )
        TexWnd_RenderToRT( s_cellW[RTT_TEXTURE], s_cellH[RTT_TEXTURE] );
    KiwiViewDirty_EndTick();

    // KIWI-UX (ROUND AV, ITEM 3): the entity-browser model thumbnails.  LAST, and inside
    // this function rather than beside its call site (radiant_main.cpp:938), so it
    // inherits both gates above for free — the `!s_primary` early-out and, far more
    // importantly, the round-AB `RTT_DeviceHealthy()` one.  It renders AT MOST ONE
    // thumbnail, only when the browser asked for one on the previous frame; with a warm
    // cache (and whenever the Entity Browser is closed, since a panel that does not draw
    // never requests) it is an early return on a null request slot.  Its render target is
    // a standalone RTT slot, freed by the same RTT_ReleaseForReset the four viewports use
    // — see kiwi_entthumb.h.
    extern void KiwiEntThumb_Tick();   // kiwi_entthumb.cpp
    KiwiEntThumb_Tick();
}

// Post-present (frame WM_PAINT): every native viewport child is a render-target IMAGE now, and
// the console is ImGui-drawn, so ALL of them stay permanently HIDDEN (kept alive only for the
// camera cursor-ring + texture scrollbar HWND uses, and the console's legacy SendMessage sinks).
// Nothing native is parked over the dockspace anymore — that was the source of the resize flicker.
void ImGuiShell_ApplyViewportDocks()
{
    if ( !s_primary )
        return;
    HWND hide[] = { g_qeglobals.d_hwndCamera, g_qeglobals.d_hwndXY,
                    g_qeglobals.d_hwndZ, g_qeglobals.d_hwndTexture, s_consoleHwnd };
    for ( HWND h : hide )
        if ( h && ::IsWindowVisible( h ) )
            ::ShowWindow( h, SW_HIDE );
}

// First-run dock layout (only when the dock ini did not exist).
//
// KIWI-UX (RADIANT_UX_DESIGN §9, shakeout B).  USER DIRECTIVE: "3d cam view IS the new
// primary way of working with the editor.  We want to obsolete the 2d view and z views
// completely (but leave them openable in the topbar somehow via a windows dropdown)" +
// "Hide the texture and 'KIWI Imgui shell' tabs by default".
//
// So the default layout is now TWO nodes: the camera fills the whole centre, and the
// console is an ~18% strip along the bottom.  The other four windows are CLOSED by
// default (kiwi_windows.cpp), so docking them here would only pre-place tabs nobody
// asked for — they are docked into the live dockspace root when the user re-opens them
// from the Windows menu (SetNextWindowDockID in the draw).
//
// ── KIWI-UX (shakeout I): THE RIGHT COLUMN ──────────────────────────────────
// USER DIRECTIVE, verbatim: "put the texture view by default under the 2d view on
// the right middle dock."  Two things had to change together:
//   * the LAYOUT gains a right-hand column, split in two — 2D View on top,
//     Textures under it;
//   * the two windows have to be OPEN by default for any of it to be visible
//     (kiwi_windows.cpp's defOpen), because shakeout B closed all four.
// Z and the KIWI shell panel stay CLOSED and stay undocked here, for exactly the
// reason above: pre-placing tabs nobody asked for is what the 3D-first round was
// undoing.
//
// ── KIWI-UX (ROUND X, ITEM 1): THE CONSOLE SPANS THE WHOLE BOTTOM ───────────
// USER DIRECTIVE, verbatim: "the default layout should look like this: (see pic,
// console takes up whole bottom)".  Shakeout I had the console under the CAMERA
// only, so it stopped at the outliner on one side and the right column on the
// other.  The picture is one uninterrupted strip under all three.
//
// SPLIT ORDER IS THE WHOLE FIX and it is the reverse of shakeout I's:
//   1. the console comes off the ROOT first, so it is full WIDTH;
//   2. then the left column off what remains, so it stops at the console;
//   3. then the right column, likewise;
//   4. the camera is whatever is left in the middle.
// A node split from the root is full-extent in the perpendicular axis; anything
// split later can only eat the node it was given.  Taking the columns first is
// exactly what made the console narrow before.
static bool s_dockLayoutPending = false;   // set at init from ini existence

static void ImGuiShell_BuildDefaultDockLayout( ImGuiID dockId )
{
    if ( !s_dockLayoutPending )
        return;
    s_dockLayoutPending = false;

    ImGui::DockBuilderRemoveNode( dockId );
    ImGui::DockBuilderAddNode( dockId, ImGuiDockNodeFlags_DockSpace );
    ImGui::DockBuilderSetNodeSize( dockId, ImGui::GetMainViewport()->WorkSize );

    // 1. the console, off the ROOT — a full-WIDTH strip along the bottom edge,
    //    under the outliner, the camera and the right column alike (ROUND X).
    ImGuiID upper = 0;
    const ImGuiID console =
        ImGui::DockBuilderSplitNode( dockId, ImGuiDir_Down, 0.20f, nullptr, &upper );

    // 2. the LEFT column — the outliner.  USER DIRECTIVE (ROUND W): "a collapsible
    //    giant list of all brushes ON THE LEFT".  Off the upper node, so it is full
    //    height OF THAT NODE, i.e. it stops at the console.
    //    ROUND Y, ITEM 2 — USER DIRECTIVE: "Make the outliner window skinnier by
    //    default."  0.18 -> 0.125.  The rows are one icon plus a name, so an eighth
    //    of the frame reads a full name at 1080p and gives the camera the width
    //    back; the splitter still drags, and the ini/KW_VERSION pair is bumped
    //    below so an existing profile actually rebuilds and sees it.
    ImGuiID midRow = 0;
    const ImGuiID left =
        ImGui::DockBuilderSplitNode( upper, ImGuiDir_Left, 0.125f, nullptr, &midRow );

    // 3. the RIGHT column, off what is left of the upper row.
    ImGuiID camera = 0;
    const ImGuiID right =
        ImGui::DockBuilderSplitNode( midRow, ImGuiDir_Right, 0.25f, nullptr, &camera );

    // 4. the column split in half: 2D View above, Textures below.
    ImGuiID rightTop = 0;
    const ImGuiID rightBottom =
        ImGui::DockBuilderSplitNode( right, ImGuiDir_Down, 0.5f, nullptr, &rightTop );

    ImGui::DockBuilderDockWindow( "Outliner", left );          // ROUND W — the scene list
    ImGui::DockBuilderDockWindow( "Camera",   camera );        // the editor
    ImGui::DockBuilderDockWindow( "Console",  console );       // full-width bottom strip
    ImGui::DockBuilderDockWindow( "2D View",  rightTop );      // top-right
    ImGui::DockBuilderDockWindow( "Textures", rightBottom );   // right-middle/bottom

    // ── KIWI-UX: the ENTITIES tab ───────────────────────────────────────────
    // USER DIRECTIVE, verbatim: "a new panel that's in the same viewport (tabbed)
    // with the textures tab".  Docking a second window into the SAME node id is
    // exactly what makes them tabs of one another — no extra split.
    ImGui::DockBuilderDockWindow( "Entities",         rightBottom );
    ImGui::DockBuilderDockWindow( "Models",           rightBottom ); // KIWI-UX

    // ── KIWI-UX (ROUND AZ, ITEM 3): the SKY tab, third of the same node ──────
    // USER DIRECTIVE, verbatim: "Make this a separate tab like the entity tab."
    // Same node id, so it is a tab of Textures and Entities and not a split —
    // the one-line mechanism the round-AU block above states.
    ImGui::DockBuilderDockWindow( "Sky",              rightBottom );

    // ── KIWI-UX (ROUND BD): the UV EDITOR, fourth of the same node ───────────
    // The directive's window sits beside the texture browser it edits the UVs of,
    // so it is the same one-line mechanism the round-AU block above states: the
    // same node id makes it a TAB, not a split.  KIWI_LAYOUT_VERSION went to 11
    // with it so an existing profile actually rebuilds and sees this line.
    ImGui::DockBuilderDockWindow( "UV editor",        rightBottom );

    // ── KIWI-UX: the SUN HELPER, fifth of the same node ──────────────────────
    // USER REPORT, verbatim: "where is the add menu?  I can't see it.  You need to
    // do a tab like the skybox helper."  Same one-line mechanism the round-AU block
    // above states — the same node id makes it a TAB, not a split — and
    // KIWI_LAYOUT_VERSION went to 12 with it, which is the half that makes this line
    // run at all for an install that already has a dock ini (kiwi_sun.h "THE DOCK
    // TAB IS THE DISCOVERY SURFACE").
    ImGui::DockBuilderDockWindow( "Sun",              rightBottom );
    // The selected-light helper is the sixth tab; layout version 13 makes this
    // placement take effect for profiles that already had the Sun layout.
    ImGui::DockBuilderDockWindow( "Light",            rightBottom );
    ImGui::DockBuilderDockWindow( "Inspector",         rightBottom ); // KIWI-UX

    ImGui::DockBuilderFinish( dockId );
}

// Backend hook (rb_backend.cpp, inside the scene bracket just before EndScene):
// one ImGui frame per camera-window paint, submitted while dx.inScene. Lazy init
// on the first call — the device exists by then, and imgui_impl_dx9 re-creates
// its device objects in NewFrame after an Invalidate (device reset).
// imgui_impl_dx9 wraps its drawing in a D3DSBT_ALL state block, so the engine's
// cached device state is untouched.
void ImGuiShell_DrawOverlay( IDirect3DDevice9 *device, HWND activeHwnd )
{
    // Re-entrancy guard: a panel action that pops a modal (MessageBoxA in
    // EclassCreate_Apply etc.) pumps messages mid-frame; a re-entrant paint
    // must skip its ImGui frame rather than corrupt the context.
    static bool s_inFrame = false;
    if ( s_inFrame )
        return;

    if ( !ImGuiShell_Enabled() || !device )
        return;
    // ImGui lives ONLY on the frame (the dockspace surface). It is initialized on the
    // frame and its multi-viewport main viewport is bound to the frame HWND — so DrawOverlay
    // must never run for any other window. The old camera-window fallback initialized ImGui
    // on the camera during boot, then boot switched the primary to the frame, leaving the
    // main viewport bound to the wrong HWND — which tripped NewFrame's sanity checks. No
    // fallback now: nothing draws until the frame is the registered primary surface.
    if ( !s_primary || activeHwnd != s_primary )
        return;
    // Only the pump's once-per-tick request drives a NewFrame. A WM_PAINT the pump did NOT
    // ask for (the boot InvalidateRect, an OS repaint) still clears+presents the frame, but
    // must not start an ImGui frame — see s_beginFrame. This keeps NewFrame paired 1:1 with
    // the pump's UpdatePlatformWindows call (line 11827 assert).
    if ( !s_beginFrame )
        return;
    s_beginFrame = false;
    s_device = device;               // for the opaque-image draw callback



    if ( !s_shellInited )
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        // Multi-viewport (ViewportsEnable) is DELIBERATELY OFF. It was only ever a workaround:
        // the editor viewports used to be native child HWNDs the OS composited ON TOP of the
        // frame's D3D surface, so an embedded ImGui panel was hidden behind them — making each
        // floating panel its own top-level OS window was the only way to get it above the
        // children. Phase-5 RTT removed that entirely: the viewports are now textures drawn
        // INTO the frame surface and every native child is permanently SW_HIDE'd
        // (ImGuiShell_ApplyViewportDocks), so nothing composites over the panels. Keeping
        // multi-viewport on now only bought bugs — pop-out OS windows with mis-mapped mouse
        // coordinates (clicks landing in the wrong place, widgets unclickable) and windows that
        // wouldn't stay open. Tool panels are regular IN-FRAME floating ImGui windows again:
        // they still float over the dockspace and are draggable, they just aren't separate OS
        // windows, so input uses the plain single-viewport path and Just Works.
        // KIWI-UX (CLEANUP, C-7 + C-8): an ABSOLUTE, exe-relative path built from
        // ONE version number.  ImGui resolves a bare IniFilename against the process
        // CWD, and the OPENFILENAMEA dialogs in this tree do not set OFN_NOCHANGEDIR
        // — so after the first File->Open the dock layout was written next to the
        // .map and the first-run test below read the wrong directory, which looks to
        // the user like the layout resetting itself.  radiant_registry.h:3-5 already
        // settles this: editor settings live NEXT TO THE EXE.
        io.IniFilename = DockIniPath();
                                              // (bumped 11 -> 12 so the SUN tab
                                              // actually appears as a fifth tab of
                                              // that node for an existing install.
                                              // Reported as "I can't see it" against
                                              // a long-lived layout, which is this
                                              // mechanism's entire purpose.)
                                              // (ROUND BD bumped 10 -> 11 for the
                                              // UV EDITOR, fourth tab of the same node.)
                                              // (ROUND AZ): bumped 9 -> 10 so the
                                              // SKY tab actually appears as a third tab
                                              // beside Textures and Entities for an
                                              // existing install — mandatory, not
                                              // cosmetic, for exactly the reason stated
                                              // three lines down.
                                              // (ROUND AU bumped 8 -> 9 so the
                                              // ENTITIES tab actually appears beside
                                              // Textures for an existing install.)
                                              // (ROUND Y bumped 7 -> 8 for the
                                              // SKINNIER OUTLINER, left split 0.18 -> 0.125.)
                                              // (ROUND X bumped 6 -> 7 for the
                                              // full-width console strip.)  Same trap every
                                              // time: an existing kiwi_dock6.ini pins the
                                              // previous layout forever and the rebuilt
                                              // default would never be seen.
                                              // (ROUND W bumped 5 -> 6 for the LEFT column,
                                              // the Outliner; shakeout I bumped 4 -> 5 for the
                                              // right column, 2D View over Textures; shakeout B
                                              // bumped 3 -> 4 for the 3D-first layout; the
                                              // pre-3 inis held popped-out multi-viewport
                                              // positions.)
        // First run ever (no ini) → build the classic-Radiant default dock layout.
        s_dockLayoutPending =
            ( ::GetFileAttributesA( io.IniFilename ) == INVALID_FILE_ATTRIBUTES );
        // KIWI-UX (CLEANUP, C-9): the current version's ini exists, so every earlier
        // kiwi_dock<n>.ini beside it is spent — drop them.  Deliberately NOT done on
        // the first-run path: there the migration has not happened yet.
        if ( !s_dockLayoutPending )
            DockIniRetireOlder();
        ImGui::StyleColorsDark();
        // When windows can be their own OS windows, square corners + opaque bg (rounded
        // corners would leave transparent gaps at the OS-window edges).
        {
            ImGuiStyle &style = ImGui::GetStyle();
            style.WindowRounding = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f;
        }
        ImGui_ImplWin32_Init( activeHwnd );
        ImGui_ImplDX9_Init( device );
        s_backendHwnd = activeHwnd;
        s_shellInited = true;
    }
    else if ( activeHwnd != s_backendHwnd )
    {
        // The overlay migrated (camera ↔ dockhost). The win32 backend's mouse mapping
        // is hwnd-relative, so rebind it; the DX9 backend is device-bound and stays.
        ImGui_ImplWin32_Shutdown();
        ImGui_ImplWin32_Init( activeHwnd );
        s_backendHwnd = activeHwnd;
    }

    // THE ONE DRAIN POINT, and it must stay HERE: after every early-out above (so it cannot
    // run on a tick that renders nothing) and before NewFrame (so nothing this frame has
    // recorded an ImTextureID yet).  Everything queued was retired during the PREVIOUS frame's
    // UI build and presented by its WM_PAINT, so this is the first instant a release is safe.
    // ABOVE `s_inFrame = true` deliberately: `s_beginFrame` was consumed at the top, so a
    // paint this drain pumps (Image_Reload can Com_Error) returns at that gate instead.
    KiwiTexGrave_Drain();

    s_inFrame = true;
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    // KIWI-UX (ROUND Y, ITEM 6): from here on, the io state the post-present
    // dispatch reads is THIS tick's.  See s_frameLive at the top of the file.
    s_frameLive = true;

    // KIWI-UX (CLEANUP, C-11): unconditional.  This was wrapped in `if ( dockMode )`
    // with `const bool dockMode = true;` twenty lines up — the "pre-boot overlay
    // fallback" that variable selected against was removed (see the note above the
    // primary-surface gate), so the false arm could not be taken.
    const ImGuiID dockId = ImGui::DockSpaceOverViewport();
    s_dockRoot = dockId;                  // KIWI-UX §9: re-dock target for re-opened windows
    ImGuiShell_BuildDefaultDockLayout( dockId );

    if ( s_focusTab[0] )                  // pending hotkey/menu tab focus (O, texture view…)
    {
        ImGui::SetWindowFocus( s_focusTab );
        s_focusTab[0] = '\0';
    }

    // The editor views as dock tabs — RTT textures drawn as images (Phase 5); the
    // console stays a native EDIT child parked over its cell.
    // KIWI-UX (§9, shakeout B): the camera is unconditional (NULL p_open = no ✕, no
    // menu entry); the other three are drawn only while their kiwi_windows flag is
    // set, and pass it as p_open so the title-bar ✕ toggles the SAME state the
    // Windows menu does.  The titles are read from kiwi_windows so there is exactly
    // one spelling of each per build (the dock ini keys off them).
    ImGuiShell_DrawViewportImage( RTT_CAMERA,  KIWI_WIN_COUNT );   // always on
    ImGuiShell_DrawViewportImage( RTT_XY,      KIWI_WIN_XY );
    ImGuiShell_DrawViewportImage( RTT_Z,       KIWI_WIN_Z );
    ImGuiShell_DrawViewportImage( RTT_TEXTURE, KIWI_WIN_TEXTURE );
    ImGuiShell_DrawConsoleTab( g_qeglobals.d_hwndEdit );

    // KIWI-UX (user directive): the FPS readout lives in the WIN32 TITLE BAR, not
    // the panel.  Every ~500ms: read the current title, strip OUR " | N FPS"
    // suffix if present (last " | " occurrence, and only when it really ends in
    // " FPS" — a map path containing " | " is left alone), re-append.  Map loads
    // that rewrite the title are self-healing: the next tick re-appends.
    {
        static double s_nextTitle = 0.0;
        const double now = ImGui::GetTime();
        if ( now >= s_nextTitle && g_qeglobals.d_hwndMain )
        {
            s_nextTitle = now + 0.5;
            char cur[512];
            cur[0] = '\0';
            ::GetWindowTextA( g_qeglobals.d_hwndMain, cur, sizeof( cur ) );
            char *m = strstr( cur, " | " );
            while ( m )
            {
                char *n = strstr( m + 3, " | " );
                if ( !n ) break;
                m = n;
            }
            if ( m && strstr( m, " FPS" ) )
                *m = '\0';
            char withFps[560];
            _snprintf( withFps, sizeof( withFps ), "%s | %.0f FPS",
                       cur, ImGui::GetIO().Framerate );
            withFps[sizeof( withFps ) - 1] = '\0';
            ::SetWindowTextA( g_qeglobals.d_hwndMain, withFps );
        }
    }

    // KIWI-UX (§9, shakeout B): the shell panel is OFF by default and skips Begin
    // entirely when closed.  ImGuiPanels_Draw is NOT gated with it — the tool panels it
    // draws have their own visibility and must keep working with the shell tab hidden.
    if ( bool *shellOpen = KiwiWindows_OpenPtr( KIWI_WIN_SHELL ) )
    {
        if ( *shellOpen )
        {
            if ( KiwiWindows_JustOpened( KIWI_WIN_SHELL ) )
                ImGui::SetNextWindowDockID( s_dockRoot, ImGuiCond_Always );
            if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_SHELL ), shellOpen ) )
            {
                ImGui::Text( "KIWI Radiant — ImGui shell." );
                extern void ImGuiPanels_Menu();   // imgui_panels.cpp
                ImGuiPanels_Menu();
            }
            ImGui::End();
        }
    }
    // KIWI-UX (ROUND W): the OUTLINER dock window.  It Begins/Ends itself (and
    // early-outs when its §9 flag is clear) exactly like the shell panel above, but
    // its body is large enough to deserve its own file — kiwi_outliner.cpp.
    KiwiOutliner_Draw();
    // KIWI-UX (ROUND AU): the ENTITY BROWSER dock window — same contract as the
    // outliner above (Begins/Ends itself, early-outs on its §9 flag).  Drawn AFTER
    // the viewport images so the camera image is already submitted when a tile's
    // drag starts, and at top-level window scope because it is a real window.
    KiwiEntBrowser_Draw();
    KiwiModelBrowser_Draw(); // KIWI-UX
    // KIWI-UX (ROUND AZ, ITEM 3): the SKY dock window — same contract again
    // (Begins/Ends itself, early-outs on its §9 flag).  It has no drag-to-camera
    // gesture, so unlike the entity browser it carries no ordering constraint
    // against the viewport images; it sits here to keep the three tabs of one dock
    // node adjacent in this function as well as on screen.
    KiwiSky_Draw();
    // KIWI-UX (ROUND BD): the UV EDITOR dock window — same contract again (Begins/Ends
    // itself, early-outs on its §9 flag).  Its canvas is pure ImDrawList over an
    // InvisibleButton, so like the Sky tab it carries no ordering constraint against the
    // viewport images; it sits here to keep the four tabs of one dock node adjacent in
    // this function as well as on screen.
    KiwiUvEd_Draw();
    // KIWI-UX: the SUN HELPER dock window — same contract again (Begins/Ends itself,
    // early-outs on its §9 flag).  Its body is plain ImGui items with no canvas and no
    // drag gesture, so like the Sky tab it carries no ordering constraint against the
    // viewport images; it sits here to keep the seven tabs of one dock node adjacent in
    // this function as well as on screen.
    KiwiSun_Draw();
    KiwiLight_Draw();
    extern void ImGuiPanel_Entity_Draw(); // imgui_panel_entity.cpp
    ImGuiPanel_Entity_Draw(); // KIWI-UX
    // KIWI-UX (ROUND BE): the TEXTURE IMPORT WIZARD.  Not a dock window — a MODAL POPUP
    // that exists only while the dropped-file queue is non-empty, which is why this round
    // adds no kiwiWindow_t row and does NOT bump KIWI_LAYOUT_VERSION.  It draws at
    // top-level window scope for the same reason KiwiCmdOpts_Draw below does: ImGui::Open-
    // Popup / BeginPopupModal must run outside any other window's Begin/End pair.  It
    // early-outs to nothing when there is no queue.
    KiwiImport_Draw();
    // KIWI-UX (ROUND BF): the BUILD & RUN dialog.  Also not a dock window — a FLOATING
    // window with its own file-scope open flag, so this round adds no kiwiWindow_t row and
    // does NOT bump KIWI_LAYOUT_VERSION either.  It is called UNCONDITIONALLY and POLLS ITS
    // CHILD PROCESS BEFORE it looks at that flag: the compilers run in real processes whose
    // stdout pipe this frame is the only thing draining, so closing the window mid-build
    // must not stall the pipe or freeze the BSP -> Light -> Run chain between stages
    // (kiwi_launch.h D-BF-D).  Top-level window scope for the same reason as the wizard
    // above: its Kill confirmation is an ImGui modal popup.
    KiwiLaunch_Draw();

    // KIWI-UX (ROUND AI, ITEM 3): the IN-COMMAND OPTIONS panel — a floating window
    // that exists only while the live modal command declares options.  Drawn HERE,
    // at top-level window scope, rather than from KiwiVP_DrawCameraOverlay: that
    // runs INSIDE the viewport window's Begin/End and its whole contract is
    // ImDrawList-only (kiwi_hints.h "no ImGui items inside the camera image").
    KiwiCmdOpts_Draw();

    extern void ImGuiPanels_Draw();       // imgui_panels.cpp
    ImGuiPanels_Draw();

    // KIWI-UX (§9): reconcile anything the ✕ boxes above changed — persist it and
    // re-sync the Windows-menu check marks.  Must run after every Begin/End of this
    // frame and before the next one, so the one-frame "just opened" latch is correct.
    KiwiWindows_CommitPending();

    {
        PROF_SCOPED( "ImGui render" );
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData( ImGui::GetDrawData() );   // main viewport (this surface)
    }
    s_frameRendered = true;   // gates ImGuiShell_RenderPlatformWindows (see below)
    // The extra platform windows (popped-out panels) are NOT rendered here — each does its
    // own BeginScene/EndScene/Present, which must not nest inside the frame's scene bracket
    // (DrawOverlay runs pre-EndScene). The frame's WM_PAINT calls ImGuiShell_RenderPlatform-
    // Windows() after the present instead.
    s_inFrame = false;
}

// Called from the frame's WM_PAINT AFTER the present (outside the scene bracket): draw +
// present every popped-out panel OS window. No-op unless multi-viewport is active.
// Pump calls this ONCE per tick, before it forces the frame's paint — it authorizes the
// single NewFrame that paint will run (see s_beginFrame).
void ImGuiShell_BeginFrame()
{
    s_beginFrame = true;
}

// ── KIWI-UX (ROUND U): "IS THIS PAINT GOING TO DRAW A SCENE?" ───────────────
// USER REPORT, verbatim: "There is a flicker on some actions that turns the whole
// window white.  You gotta fix that.  It's not super consistant."
//
// THE MECHANISM, read end to end rather than guessed:
//   * the frame's WM_PAINT (radiant_main.cpp) unconditionally ran
//     R_AddCmdClearScreen( 7, g_qeglobals.d_savedinfo.colors[1], ... ) and then
//     presented.  colors[1] is set to { 1, 1, 1, 1 } — PURE WHITE — in
//     win_qe3.cpp:415, because it is the classic 2D GRID BACKGROUND;
//   * the ImGui scene for that frame is submitted somewhere else entirely: the
//     backend hook ImGuiShell_DrawOverlay, pre-EndScene;
//   * that hook REFUSES to draw unless the pump authorized this exact paint
//     (s_beginFrame, set by ImGuiShell_BeginFrame immediately before the pump's
//     InvalidateRect + UpdateWindow) — and it also refuses on its own re-entrancy
//     guard when a panel action pops a modal mid-frame.
// So ANY WM_PAINT the pump did not ask for cleared the whole client area to WHITE
// and presented it with nothing drawn on top.  That is the flash, and it is
// "not super consistent" because the paints that trigger it are the ones OTHER
// code causes: a TrackPopupMenu or MessageBox nested message loop, a window
// move/resize, an OS-driven repaint, a modal dialog closing.
//
// THE GUARD (radiant_main.cpp's WM_PAINT): present ONLY a frame whose scene
// bracket is actually going to run.  This is the predicate it asks.  Re-authorizing
// instead was NOT an option — s_beginFrame exists to keep NewFrame paired 1:1 with
// the pump's UpdatePlatformWindows call, and breaking that pairing is the AFK
// assert this file already carries two notes about.
bool ImGuiShell_FrameAuthorized()
{
    return s_beginFrame;
}

void ImGuiShell_RenderPlatformWindows()
{
    if ( !s_shellInited )
        return;
    // Only after a frame was actually rendered this cycle — UpdatePlatformWindows asserts
    // (FrameCountPlatformEnded == FrameCount) if Render() wasn't called, which is exactly
    // what happened on the AFK crash when a paint skipped the ImGui frame.
    if ( !s_frameRendered )
        return;
    s_frameRendered = false;
    if ( ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable )
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

// Device-loss hook (R_ReleaseForShutdownOrReset): drop the DX9 objects before
// dx.device->Reset(). NewFrame re-creates them on the next painted frame.
void ImGuiShell_InvalidateDeviceObjects()
{
    if ( s_shellInited )
        ImGui_ImplDX9_InvalidateDeviceObjects();
    // Nothing app-owned may sit in the graveyard across the Reset() that follows.  RELEASES
    // ONLY: a lost device cannot create Image_Reload's replacement texture.  Unconditional and
    // idempotent — it tolerates the double call the INVALIDCALL second-chance arm makes.
    KiwiTexGrave_ReleaseForReset();
}

// Camera-window message hook (CCamWnd::WindowProc). Returns true when ImGui
// consumed the message — the camera handlers must not also react. Mouse/key
// traffic invalidates the camera window so the overlay repaints without a
// continuous frame loop (the editor renders per-WM_PAINT).
bool ImGuiShell_HandleMessage( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    if ( !s_shellInited || hwnd != s_backendHwnd )
        return false;

    // The backend's return value is LOAD-BEARING and must be respected. It returns 1 for
    // messages it fully handles — most importantly WM_SETCURSOR, where it has just set the
    // resize/move cursor. Discarding it (the old bug) let the message fall through to
    // DefWindowProc, which reset the cursor to the arrow the moment the mouse stopped
    // moving — and made resize borders impossible to see/grab. Returning here stops that.
    if ( ImGui_ImplWin32_WndProcHandler( hwnd, msg, wParam, lParam ) )
        return true;

    // Otherwise, swallow the message from the legacy handlers when ImGui owns the input
    // (hovering a panel / typing in a field).
    ImGuiIO &io = ImGui::GetIO();
    const bool mouseMsg = ( msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST );
    const bool keyMsg   = ( msg >= WM_KEYFIRST   && msg <= WM_KEYLAST );
    // KIWI-UX (shakeout B) — THE HOTKEY-IN-THE-3D-VIEW FIX.  The key arm used to test
    // io.WantCaptureKeyboard, which is FAR broader than "a field is being typed into":
    // imgui.cpp:5748-5754 sets it from `g.ActiveId != 0`, and imgui.cpp:5600-5619
    // (UpdateMouseMovingWindowEndFrame) sets an ActiveId — the hovered window's MoveId —
    // on EVERY left click that lands on window background rather than on a widget.  The
    // camera viewport IS window background (ImGui::Image submits an item with id 0), so
    // pressing LMB anywhere in the 3D view latched ActiveId for the whole drag; ImGui
    // then cancels the actual move for a docked window but deliberately does NOT clear
    // ActiveId (the comment at imgui.cpp:5612).  Result: for the entire duration of any
    // click or drag in the 3D view, io.WantCaptureKeyboard was true and this returned
    // true for every WM_KEY*, which killed the frame WndProc's own hotkey arm
    // (radiant_main.cpp:233-239 `case WM_KEYDOWN: Radiant_TryHotkey`) — the arm that is
    // the ONLY hotkey path whenever a nested modal loop (a viewport context menu,
    // DialogBoxParamA, MessageBoxA) is pumping instead of Radiant_RunMessageLoop, since
    // those loops never call Radiant_PreTranslateMessage.
    // Narrowed to WantTextInput, i.e. exactly the same rule the pump-side gate uses
    // (ImGuiShell_WantsKeyboard, top of this file): a focused TEXT FIELD still outranks
    // every hotkey, and nothing else does.  The MOUSE arm is untouched — WantCaptureMouse
    // is the correct test there and no part of this change touches it.
    return ( mouseMsg && io.WantCaptureMouse ) || ( keyMsg && io.WantTextInput );
}

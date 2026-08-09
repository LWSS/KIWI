// imgui_shell.cpp — UI-rework Phase 2a: Dear ImGui overlay on the camera window.
// New KISAK code (not from the binary). Behind the `-imgui` command-line flag; the
// MFC shell is untouched. The overlay proves device/backend/input integration —
// the dockspace shell is Phase 2b. See RADIANT_UI_REWORK_PLAN.md.
#include "stdafx.h"
#include "qe3.h"

#include <d3d9.h>
#include "radiant_rtt.h"           // RTT_GetTexture / rttViewport_t (viewport images)
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>     // DockBuilder API + GetCurrentWindow (auto-close)
#include <imgui/backends/imgui_impl_win32.h>
#include <imgui/backends/imgui_impl_dx9.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler( HWND hWnd, UINT msg,
                                                              WPARAM wParam, LPARAM lParam );

static bool s_shellChecked = false;
static bool s_shellEnabled = false;   // -imgui present on the command line
static bool s_shellInited  = false;
static bool s_frameRendered = false;  // a full ImGui frame was Render()'d this cycle —
                                      // gates the platform-window update (AFK assert fix)
static bool s_beginFrame    = false;  // the PUMP requests exactly one ImGui frame per tick.
                                      // NewFrame is driven by the frame's WM_PAINT, but stray
                                      // paints (boot InvalidateRect, OS repaints) must NOT run
                                      // a NewFrame — else NewFrame count and UpdatePlatform-
                                      // Windows count desync and NewFrame asserts. Only a
                                      // pump-requested paint clears this and runs the frame.
static HWND s_backendHwnd  = nullptr; // the window the win32 backend is bound to

// (The Phase-2b dockhost window is retired — the main frame IS the dockspace surface
//  now; see ImGuiShell_SetPrimarySurface below.)

static bool ImGuiShell_Enabled()
{
    // Full-ImGui transition (user directive 2026-08-08): the shell IS the editor UI —
    // always on. (The -imgui flag gated the Phase-2a overlay era; kept as a function so
    // the call sites read the same.)
    (void)s_shellChecked; (void)s_shellEnabled;
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
static void ImGuiShell_ViewportInput( rttViewport_t id, bool hovered )
{
    const ImGuiIO &io = ImGui::GetIO();
    HWND hw = VP_Hwnd( id );
    const unsigned int flags = VP_Flags();
    const int mx = (int)( io.MousePos.x - s_imgMin[id].x );
    const int my = (int)( io.MousePos.y - s_imgMin[id].y );

    const bool owns = ( s_inputOwner == id );

    if ( s_inputOwner == RTT_COUNT && hovered )
    {
        for ( int b = 0; b < 3; ++b )
            if ( ImGui::IsMouseClicked( b ) )
            {
                s_inputOwner = id;
                VP_Down( id, hw, b, flags, mx, my );
            }
        if ( s_wheel[id] != 0.0f )
            VP_Wheel( id, hw, (short)( s_wheel[id] * 120.0f ), mx, my );   // image-relative
        VP_Move( id, hw, flags, mx, my );
    }
    else if ( owns )
    {
        VP_Move( id, hw, flags, mx, my );
        for ( int b = 0; b < 3; ++b )
            if ( ImGui::IsMouseReleased( b ) )
                VP_Up( id, hw, b, flags, mx, my );
        if ( !io.MouseDown[0] && !io.MouseDown[1] && !io.MouseDown[2] )
            s_inputOwner = RTT_COUNT;
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
        extern void CamWnd_AbortDrag();   // camwnd.cpp — Cam_MouseUp restore (re-shows the cursor)
        CamWnd_AbortDrag();
    }
    else
    {
        for ( int b = 0; b < 3; ++b )     // XY/Z/texture: up-handlers just reset drag/button state
            VP_Up( id, hw, b, 0, 0, 0 );
    }
    s_inputOwner = RTT_COUNT;
}

// Pump entry (post-present): route input to all four viewports using the rects/hover the
// compositing frame recorded. Outside the scene bracket, so a context-menu modal is safe.
void ImGuiShell_DispatchViewportInput()
{
    if ( !s_shellInited || !s_primary )
        return;

    for ( int id = 0; id < RTT_COUNT; ++id )
        ImGuiShell_ViewportInput( (rttViewport_t)id, s_hovered[id] );

    // Stuck-drag guard — runs AFTER the loop so the NORMAL per-frame release path above always
    // wins (it calls the viewport's real up-handler and clears s_inputOwner). Only if we STILL
    // own a drag yet no physical mouse button is down (the release edge was eaten by a popup
    // that stole the OS mouse) do we force the teardown. Ordering matters: doing this before the
    // loop pre-empted the normal RMB-up, leaving the XY pan's m_nButtonstate stuck (cursor
    // "stolen"). GetAsyncKeyState reads the true physical state regardless of focus.
    if ( s_inputOwner != RTT_COUNT )
    {
        const bool anyDown = ( ::GetAsyncKeyState( VK_LBUTTON ) & 0x8000 ) ||
                             ( ::GetAsyncKeyState( VK_RBUTTON ) & 0x8000 ) ||
                             ( ::GetAsyncKeyState( VK_MBUTTON ) & 0x8000 );
        if ( !anyDown )
            ImGuiShell_AbortViewportInput();
    }
}

// Draw the viewport image OPAQUE: the RT is A8R8G8B8 and the scene's written alpha is not
// guaranteed to be 1, so alpha-blending the image could make it transparent. This callback
// turns alpha-blend off for the image draw; ImDrawCallback_ResetRenderState restores it.
static void ImGuiShell_ImageOpaqueCb( const ImDrawList *, const ImDrawCmd * )
{
    if ( s_device )
        s_device->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
}

// Draw one RTT viewport as an image + record its cell size for next tick's RT render.
static void ImGuiShell_DrawViewportImage( const char *title, rttViewport_t id )
{
    ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0.0f, 0.0f ) );
    const bool open = ImGui::Begin( title, nullptr,
                                    ImGuiWindowFlags_NoScrollbar |
                                    ImGuiWindowFlags_NoScrollWithMouse |
                                    ImGuiWindowFlags_NoCollapse );
    ImGui::PopStyleVar();
    if ( open )
    {
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
                ImGui::Image( (ImTextureID)(intptr_t)tex, avail );
                dl->AddCallback( ImDrawCallback_ResetRenderState, nullptr );
                // Record only — the actual input dispatch runs in the pump AFTER present
                // (ImGuiShell_DispatchViewportInput), so a context-menu modal can't nest
                // inside the compositing scene bracket.
                s_imgMin[id]   = ImGui::GetItemRectMin();
                s_hovered[id]  = ImGui::IsItemHovered();
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
                ImGui::Dummy( avail );
            }
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
    if ( ImGui::Begin( "Console", nullptr, ImGuiWindowFlags_NoCollapse ) )
    {
        if ( ImGui::SmallButton( "Clear" ) )
            ImGuiConsole_Clear();
        ImGui::SameLine();
        ImGui::TextDisabled( "(%d chars)", (int)s_conText.size() );
        ImGui::Separator();

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
    if ( !s_primary )
        return;
    CamWnd_RenderToRT( s_cellW[RTT_CAMERA],  s_cellH[RTT_CAMERA] );
    XYWnd_RenderToRT ( s_cellW[RTT_XY],      s_cellH[RTT_XY] );
    ZWnd_RenderToRT  ( s_cellW[RTT_Z],       s_cellH[RTT_Z] );
    TexWnd_RenderToRT( s_cellW[RTT_TEXTURE], s_cellH[RTT_TEXTURE] );
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

// First-run dock layout (only when kiwi_imgui.ini did not exist): the classic Radiant
// arrangement — Camera top-left (Z tabbed behind), 2D View right, Textures bottom-left
// (Console + the shell window tabbed behind). Everything is user-re-dockable; the ini
// persists whatever they make of it.
static bool s_dockLayoutPending = false;   // set at init from ini existence

static void ImGuiShell_BuildDefaultDockLayout( ImGuiID dockId )
{
    if ( !s_dockLayoutPending )
        return;
    s_dockLayoutPending = false;

    ImGui::DockBuilderRemoveNode( dockId );
    ImGui::DockBuilderAddNode( dockId, ImGuiDockNodeFlags_DockSpace );
    ImGui::DockBuilderSetNodeSize( dockId, ImGui::GetMainViewport()->WorkSize );

    ImGuiID top = 0, console = 0;
    ImGuiID left = 0, right = 0, leftTop = 0, leftBottom = 0, botTex = 0, botMisc = 0;

    // Carve a FULL-WIDTH console strip off the very bottom first, so it spans the entire
    // width beneath every editor pane. Everything else is arranged inside `top`.
    console = ImGui::DockBuilderSplitNode( dockId, ImGuiDir_Down, 0.20f, nullptr, &top );

    right   = ImGui::DockBuilderSplitNode( top, ImGuiDir_Right, 0.55f, nullptr, &left );
    leftTop = ImGui::DockBuilderSplitNode( left, ImGuiDir_Up, 0.55f, nullptr, &leftBottom );
    // Split the bottom-left again so the texture browser has its OWN visible pane rather
    // than sitting as a tab behind the shell.
    botTex  = ImGui::DockBuilderSplitNode( leftBottom, ImGuiDir_Left, 0.6f, nullptr, &botMisc );

    // ONLY the editor viewports + console + the shell toggle-list are docked into the
    // frame grid. Tool panels are deliberately NOT docked — they open as floating pop-out
    // OS windows (NoAutoMerge), which is what the user wants and what keeps them above the
    // native viewport children.
    ImGui::DockBuilderDockWindow( "Camera",   leftTop );
    ImGui::DockBuilderDockWindow( "Z",        leftTop );
    ImGui::DockBuilderDockWindow( "2D View",  right );
    ImGui::DockBuilderDockWindow( "Textures", botTex );          // own visible pane
    ImGui::DockBuilderDockWindow( "KIWI ImGui shell", botMisc );
    ImGui::DockBuilderDockWindow( "Console",  console );         // full-width bottom strip

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
    const bool dockMode = true;

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
        io.IniFilename = "kiwi_dock3.ini";  // bumped: the old ini holds popped-out (multi-
                                              // viewport) window positions that are meaningless
                                              // now — a clean name rebuilds the default layout.
        // First run ever (no ini) → build the classic-Radiant default dock layout.
        s_dockLayoutPending =
            ( ::GetFileAttributesA( io.IniFilename ) == INVALID_FILE_ATTRIBUTES );
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

    s_inFrame = true;
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if ( dockMode )
    {
        const ImGuiID dockId = ImGui::DockSpaceOverViewport();
        ImGuiShell_BuildDefaultDockLayout( dockId );

        if ( s_focusTab[0] )              // pending hotkey/menu tab focus (O, texture view…)
        {
            ImGui::SetWindowFocus( s_focusTab );
            s_focusTab[0] = '\0';
        }

        // The editor views as dock tabs — RTT textures drawn as images (Phase 5); the
        // console stays a native EDIT child parked over its cell.
        ImGuiShell_DrawViewportImage( "Camera",   RTT_CAMERA );
        ImGuiShell_DrawViewportImage( "2D View",  RTT_XY );
        ImGuiShell_DrawViewportImage( "Z",        RTT_Z );
        ImGuiShell_DrawViewportImage( "Textures", RTT_TEXTURE );
        ImGuiShell_DrawConsoleTab( g_qeglobals.d_hwndEdit );
    }

    if ( ImGui::Begin( "KIWI ImGui shell" ) )
    {
        ImGui::Text( dockMode ? "KIWI Radiant — ImGui shell." : "Overlay (pre-boot fallback)." );
        ImGui::Text( "%.1f ms/frame (%.0f FPS)",
                     1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate );
        extern void ImGuiPanels_Menu();   // imgui_panels.cpp
        ImGuiPanels_Menu();
    }
    ImGui::End();
    extern void ImGuiPanels_Draw();       // imgui_panels.cpp
    ImGuiPanels_Draw();

    ImGui::Render();
    ImGui_ImplDX9_RenderDrawData( ImGui::GetDrawData() );   // main viewport (this surface)
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
    return ( mouseMsg && io.WantCaptureMouse ) || ( keyMsg && io.WantCaptureKeyboard );
}

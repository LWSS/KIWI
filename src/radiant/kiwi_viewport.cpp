#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// kiwi_viewport.cpp — the shell bridge; kiwi_viewport.h defines its contract.
//
// A modern gesture owns its entire press-to-release cycle. Never pass legacy an
// unmatched release: its down handler did not establish the state that up tears down.
// KiwiVP_CameraAbort reports modern teardown so the shell does not also call CamWnd_AbortDrag.
//
// RMB is classified at release: drag navigates, near-click confirms, and the menu is disabled.
// If re-enabled, replay CamWnd_OnRButtonDown/Up (camwnd.cpp:3886/3892): down sets
// cam_was_not_dragged for CamWnd_ContextMenu (:3031); post-present keeps it outside the scene.
//
// Alt is latched at MMB press: that whole gesture is a view swipe, never an orbit.
// Net travel selects KiwiViewCube_SwipeAxisView or KiwiViewCube_StepAxisView; bare MMB
// still orbits from its first pixel.
//
// A replay feeds CamWnd_DropModelsToPlane zero dt to suppress its one-shot camera fly
// (camwnd.cpp:2550-2553), restoring g_oldtime afterward. CameraMode 1 returns earlier
// (camwnd.cpp:2497); this guard matters for CameraMode 0.

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_viewport.h"
#include "kiwi_boxselect.h"
#include "kiwi_camera.h"
#include "kiwi_command.h"
#include "kiwi_construct.h"
#include "kiwi_gizmo.h"
#include "kiwi_grass.h"
#include "kiwi_hints.h"
#include "kiwi_hover.h"
#include "kiwi_lollipop.h"
#include "kiwi_section.h"
#include "kiwi_sun.h"
#include "kiwi_transform.h"
#include "kiwi_numeric.h"
#include "kiwi_patchverts.h"
#include "kiwi_region.h"
#include "kiwi_selection.h"
#include "kiwi_selext.h"
#include "kiwi_snap.h"
#include "kiwi_ux.h"
#include "kiwi_units.h"
#include "kiwi_uv.h"
#include "kiwi_viewcube.h"
#include "kiwi_viewdirty.h"

#include <stdio.h>

// ── ported entry points (verified against their definitions) ────────────────
extern void CamWnd_OnRButtonDown( HWND hwnd, unsigned int nFlags, int x, int y ); // camwnd.cpp:5575
extern void CamWnd_OnRButtonUp  ( HWND hwnd, unsigned int nFlags, int x, int y ); // camwnd.cpp:5581
extern bool ImGuiShell_WantsKeyboard();                                           // imgui_shell.cpp:152
// Terrain-paint gate shared by Drag_Begin (drag.cpp:653) and Cam_Draw's cursor ring
// (camwnd.cpp:3189): patchdialog.cpp:229, IDB 0x401D50. Keep it outside the anonymous
// namespace to preserve the ported definition's linkage.
extern int  sub_401D50();                                                         // patchdialog.cpp:229

namespace
{
    // Tags own the full press/release cycle. COMMAND and CONSUMED distinguish a modal
    // owner from a press-only owner; CMD_MARQUEE borrows KiwiBox geometry without
    // mutating selection. SECTION and SUN need distinct release/abort paths.
    enum kgesture_t { KG_NONE = 0, KG_ORBIT, KG_MARQUEE, KG_COMMAND, KG_CONSUMED,
                      KG_LOOK, KG_PAN, KG_GIZMO, KG_CMD_MARQUEE, KG_SECTION,
                      KG_SUN, KG_DROP, KG_GRASS };

    // The sticky RMB menu-drag latch uses tighter slop than LMB selection so an
    // intended camera drag cannot become a menu click after returning near its press.
    const int KVP_RMB_CLICK_PIXELS = 4;

    // Confirm reuses the 8 px LMB click slop: losing an active command is costlier
    // than accepting a tiny pan. Do not "undo" that pan; KiwiCam_PanDrag is scale-
    // and depth-dependent, so negating its pixels would drift.
    const int KVP_RMB_CONFIRM_PIXELS = KBOX_CLICK_PIXELS;

    kgesture_t s_gesture = KG_NONE;
    int        s_gestureBtn = -1;
    int        s_lastX = 0, s_lastY = 0;

    // Alt is latched at MMB press: the whole gesture remains a view action, so
    // releasing Alt cannot turn it into an orbit. Net travel chooses swipe direction;
    // under 24 px it falls back to KiwiViewCube_StepAxisView. Bare MMB orbits immediately.
    const int KVP_SWIPE_PIXELS = 24;

    bool         s_mmbAlt   = false;
    int          s_mmbTravX = 0, s_mmbTravY = 0;
    bool         s_mmbDrag  = false;   // bare MMB only: the orbit is live

    // Signed deltas give net displacement; s_rmbDrag separately latches a larger excursion.
    int          s_rmbPressX = 0, s_rmbPressY = 0;
    unsigned int s_rmbFlags  = 0;
    int          s_rmbTravX  = 0, s_rmbTravY = 0;
    bool         s_rmbDrag   = false;
    // Latched at press; releasing Alt mid-drag must not switch look to pan.
    bool         s_rmbLook   = false;

    // Ctrl+sun click removes; a travelled drag retains its snapped orbit.
    bool         s_sunCtrl   = false;
    int          s_sunPressX = 0, s_sunPressY = 0;
    int          s_sunMaxX   = 0, s_sunMaxY = 0;

    void EndGesture()
    {
        s_gesture    = KG_NONE;
        s_gestureBtn = -1;
        s_sunCtrl    = false;
        s_sunMaxX    = s_sunMaxY = 0;
    }

    // A command marquee borrows only KiwiBox's rectangle. Sub-threshold travel is
    // returned to the command as Shift+click; a drag supplies the resolved rectangle.
    // Always Cancel, never End, so KiwiBox cannot mutate selection independently.
    void CommandMarqueeEnd( int imgX, int imgY )
    {
        KiwiBox_Update( imgX, imgY );
        int  x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool crossing = false;
        const bool have = KiwiBox_Rect( &x0, &y0, &x1, &y1, &crossing );
        KiwiBox_Cancel();
        if ( !have )
            return;

        const int dx  = x1 - x0;
        const int dy  = y1 - y0;
        const int adx = ( dx < 0 ) ? -dx : dx;
        const int ady = ( dy < 0 ) ? -dy : dy;
        if ( adx < KBOX_CLICK_PIXELS && ady < KBOX_CLICK_PIXELS )
        {
            KiwiCmd_MouseButton( 0, x0, y0, true );
            return;
        }
        KiwiCmd_Marquee( x0, y0, x1, y1, crossing, true );
    }

    // Classic click replay needs both ported halves; zero dt suppresses its fly side effect.
    void ReplayLegacyRightClick( int imgX, int imgY )
    {
        HWND hw = g_qeglobals.d_hwndCamera;
        if ( !hw )
            return;
        const double savedDt = g_qeglobals.g_oldtime;
        g_qeglobals.g_oldtime = 0.0;
        CamWnd_OnRButtonDown( hw, s_rmbFlags, imgX, imgY );
        g_qeglobals.g_oldtime = savedDt;
        CamWnd_OnRButtonUp( hw, s_rmbFlags, imgX, imgY );
    }

    // ── chips (spec §11) ────────────────────────────────────────────────────
    struct chip_t
    {
        const char *label;
        const char *tip;
        sel_mask_t  mask;
    };

    // Tooltips name the exclusions enforced by construction-line and region picking.
    const chip_t KCHIPS[5] =
    {
        { "1 Point",  "Point / vertex selection\nBrush vertices + construction points\n"
                      "Hotkey 1 (modern keymap)",                           SEL_MASK_VERTEX },
        { "2 Edge",   "Edge selection\nBrush edges + construction lines\n"
                      "Hotkey 2 (modern keymap)",                           SEL_MASK_EDGE   },
        // Patches have no faces, so Face mode names the whole patch.
        { "3 Face",   "Face selection\nBrush faces + construction REGION faces\n"
                      "A CURVE/PATCH picks as the whole patch (patches have no faces)\n"
                      "Construction lines are NOT clickable in this mode\n"
                      "Hotkey 3 (modern keymap)",                           SEL_MASK_FACE   },
        { "4 Object", "Object selection - BRUSHES AND ENTITIES ONLY\n"
                      "No construction geometry of any kind\n"
                      "Hotkey 4 (modern keymap)",                           SEL_MASK_OBJECT },
        { "5 All",    "Everything - brushes, faces, edges, points,\n"
                      "construction lines and region faces\n"
                      "Hotkey 5 (modern keymap)",                           SEL_MASK_EVERYTHING },
    };

    // The 1-5 shortcuts are modern-profile only; chips remain available in both profiles.

    // Expose non-default plane/section latches that otherwise silently affect picks.
    // ImDrawList output creates no item and therefore cannot claim viewport hover.
    void DrawStateBanner( float imgMinX, float imgMinY )
    {
        char rows[2][96];
        ImU32 cols[2];
        int   n = 0;

        // A construction plane is relevant only while a planar placement tool uses it.
        if ( KiwiCon_PlaneIsExplicit() && KiwiCon_PlanePlacement() )
        {
            _snprintf( rows[n], sizeof( rows[n] ), "PLANE: %s", KiwiCon_PlaneDescLive() );
            cols[n] = IM_COL32( 120, 210, 255, 255 );
            rows[n][sizeof( rows[n] ) - 1] = 0;
            ++n;
        }
        if ( KiwiSection_Active() )
        {
            char zb[32];
            KiwiUnits_Format( zb, sizeof( zb ), KiwiSection_Level() );
            _snprintf( rows[n], sizeof( rows[n] ),
                       KiwiSection_Cutting() ? "SECTION Z=%s" : "SECTION Z=%s  (not cutting)", zb );
            cols[n] = IM_COL32( 255, 206, 90, 255 );
            rows[n][sizeof( rows[n] ) - 1] = 0;
            ++n;
        }
        if ( !n )
            return;

        ImDrawList *dl = ImGui::GetWindowDrawList();
        float y = imgMinY + 8.0f + 26.0f;                  // just under the chip row
        for ( int i = 0; i < n; ++i )
        {
            const ImVec2 ts = ImGui::CalcTextSize( rows[i] );
            const ImVec2 a( imgMinX + 8.0f, y );
            const ImVec2 b( a.x + ts.x + 12.0f, a.y + ts.y + 6.0f );
            dl->AddRectFilled( a, b, IM_COL32( 18, 18, 22, 205 ), 3.0f );
            dl->AddRect( a, b, cols[i], 3.0f, 0, 1.0f );
            dl->AddText( ImVec2( a.x + 6.0f, a.y + 3.0f ), cols[i], rows[i] );
            y = b.y + 4.0f;
        }
    }

    bool DrawChips( float imgMinX, float imgMinY )
    {
        ImGui::SetCursorScreenPos( ImVec2( imgMinX + 8.0f, imgMinY + 8.0f ) );

        bool anyHovered = false;
        const sel_mask_t cur = KiwiSel_GetModeMask();

        ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 7.0f, 3.0f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing,  ImVec2( 4.0f, 4.0f ) );
        ImGui::PushID( "kiwichips" );
        for ( int i = 0; i < 5; ++i )
        {
            const bool on = ( cur == KCHIPS[i].mask );
            if ( i )
                ImGui::SameLine();
            if ( on )
            {
                ImGui::PushStyleColor( ImGuiCol_Button,        ImVec4( 0.20f, 0.48f, 0.72f, 0.95f ) );
                ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.26f, 0.56f, 0.82f, 0.95f ) );
                ImGui::PushStyleColor( ImGuiCol_ButtonActive,  ImVec4( 0.30f, 0.62f, 0.90f, 0.95f ) );
            }
            else
            {
                ImGui::PushStyleColor( ImGuiCol_Button,        ImVec4( 0.14f, 0.14f, 0.16f, 0.80f ) );
                ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.24f, 0.24f, 0.27f, 0.90f ) );
                ImGui::PushStyleColor( ImGuiCol_ButtonActive,  ImVec4( 0.30f, 0.30f, 0.34f, 0.95f ) );
            }
            if ( ImGui::Button( KCHIPS[i].label ) )
                KiwiSel_SetModeMask( KCHIPS[i].mask );
            if ( ImGui::IsItemHovered() )
            {
                anyHovered = true;
                ImGui::SetTooltip( "%s", KCHIPS[i].tip );
            }
            ImGui::PopStyleColor( 3 );
        }
        ImGui::PopID();
        ImGui::PopStyleVar( 2 );

        // Do not restore the cursor with a trailing SetCursorScreenPos: without a
        // following item ImGui::End asserts, while a Dummy would extend content and
        // risk scrollbar feedback. The next camera-window call is End().
        return anyHovered;
    }

    void DrawSunRemoveCue( float imgMinX, float imgMinY, float imgW, float imgH )
    {
        if ( !KiwiHover_RemovePreview() )
            return;
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const int x = (int)( mouse.x - imgMinX );
        const int y = (int)( mouse.y - imgMinY );
        if ( x < 0 || y < 0 || x >= (int)imgW || y >= (int)imgH
          || !KiwiSun_GlyphHit( x, y ) )
            return;
        float rgb[3];
        KiwiHover_PreviewColor( true, rgb );
        const ImU32 col = ImGui::ColorConvertFloat4ToU32(
            ImVec4( rgb[0], rgb[1], rgb[2], 0.96f ) );
        ImGui::GetWindowDrawList()->AddCircle( mouse, 14.0f, col, 24, 3.0f );
    }

    // Marquee modes share one filled, solid-border shape; hue alone distinguishes
    // warm crossing from cool containment. The legacy 3D quad is unreachable because
    // modern LMB ownership prevents its CamWnd_OnLButtonDown -> Drag_Begin path.
    void DrawMarquee( float imgMinX, float imgMinY )
    {
        int x0, y0, x1, y1;
        bool crossing = false;
        if ( !KiwiBox_Rect( &x0, &y0, &x1, &y1, &crossing ) )
            return;

        const ImVec2 a( imgMinX + (float)( x0 < x1 ? x0 : x1 ),
                        imgMinY + (float)( y0 < y1 ? y0 : y1 ) );
        const ImVec2 b( imgMinX + (float)( x0 < x1 ? x1 : x0 ),
                        imgMinY + (float)( y0 < y1 ? y1 : y0 ) );
        if ( b.x - a.x < 1.0f && b.y - a.y < 1.0f )
            return;
        // Match KiwiBox_End's click slop: sub-threshold hand wobble remains a click
        // and must not advertise a marquee that release will not perform.
        {
            const int dx = x1 - x0;
            const int dy = y1 - y0;
            if ( ( dx > -KBOX_CLICK_PIXELS && dx < KBOX_CLICK_PIXELS )
              && ( dy > -KBOX_CLICK_PIXELS && dy < KBOX_CLICK_PIXELS ) )
                return;                      // still a click
        }

        // Direction still distinguishes crossing from containment, except during
        // Ctrl-remove: then the action outranks direction and both the rectangle
        // and its 3D candidates use the hover layer's warm warning palette.
        ImU32 fill   = crossing ? IM_COL32( 240, 190,  70, 48 )
                                : IM_COL32(  90, 170, 240, 48 );
        ImU32 border = crossing ? IM_COL32( 250, 200,  80, 235 )
                                : IM_COL32( 120, 195, 255, 235 );
        if ( KiwiBox_RemovePreview() )
        {
            float rgb[3];
            KiwiHover_PreviewColor( true, rgb );
            fill = ImGui::ColorConvertFloat4ToU32(
                ImVec4( rgb[0], rgb[1], rgb[2], 48.0f / 255.0f ) );
            border = ImGui::ColorConvertFloat4ToU32(
                ImVec4( rgb[0], rgb[1], rgb[2], 235.0f / 255.0f ) );
        }

        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled( a, b, fill );
        dl->AddRect( a, b, border, 0.0f, 0, 1.0f );
    }
}

// ─── input (post-present) ────────────────────────────────────────────────────
bool KiwiVP_CameraButtonDown( int btn, int imgX, int imgY, bool shift, bool ctrl )
{
    if ( s_gesture != KG_NONE )
        return false;

    // KIWI-UX: an armed Grass Scatter owns bare Alt+LMB before every camera handle,
    // command, selection arm, and the legacy terrain-paint handoff below.  When it is
    // disarmed this gate is false, preserving the prior dispatch in effect.
    if ( btn == 0 && !shift && !ctrl && ImGui::GetIO().KeyAlt
      && KiwiGrass_IsArmed() )
    {
        KiwiHover_Clear();
        KiwiGrass_HandleDown( imgX, imgY );
        s_lastX      = imgX;
        s_lastY      = imgY;
        s_gesture    = KG_GRASS;        // own release even when prerequisites refuse a stamp
        s_gestureBtn = btn;
        return true;
    }

    // Section pick/handle input precedes commands and selection because its handle
    // exists without a command. Shift bypasses the handle for additive selection;
    // the consumed pick owns release so it cannot also select behind the plane.
    if ( btn == 0 && KiwiSection_ClickPick( imgX, imgY ) )
    {
        s_lastX      = imgX;
        s_lastY      = imgY;
        s_gesture    = KG_CONSUMED;      // own the release; change nothing on it
        s_gestureBtn = btn;
        return true;
    }
    if ( btn == 0 && !shift && KiwiSection_HandleDown( imgX, imgY ) )
    {
        s_lastX      = imgX;
        s_lastY      = imgY;
        s_gesture    = KG_SECTION;
        s_gestureBtn = btn;
        return true;
    }

    // The sun handle also exists without a command and must precede selection.
    // Shift bypasses it; Ctrl is resolved at release as remove-click versus orbit.
    // KiwiSun_HandleDown self-gates so an unselected helper still follows click selection.
    if ( btn == 0 && !shift && KiwiSun_HandleDown( imgX, imgY ) )
    {
        KiwiHover_Clear();
        s_lastX      = imgX;
        s_lastY      = imgY;
        s_sunCtrl    = ctrl;
        s_sunPressX  = imgX;
        s_sunPressY  = imgY;
        s_sunMaxX    = s_sunMaxY = 0;
        s_gesture    = KG_SUN;
        s_gestureBtn = btn;
        return true;
    }

    // Command handles precede the command's generic LMB arm, which would otherwise
    // resume/commit before hit testing. Lollipop precedes gizmo; their gates make
    // them mutually exclusive. Shift bypasses the lollipop for additive selection.
    if ( btn == 0 && !shift && KiwiCmd_Active() && KiwiLollipop_MouseDown( imgX, imgY ) )
    {
        s_lastX      = imgX;
        s_lastY      = imgY;
        s_gesture    = KG_GIZMO;
        s_gestureBtn = btn;
        return true;
    }

    if ( btn == 0 && KiwiCmd_Active() && KiwiGizmo_MouseDown( imgX, imgY ) )
    {
        s_lastX      = imgX;
        s_lastY      = imgY;
        s_gesture    = KG_GIZMO;         // commits on the RELEASE edge (kiwi_gizmo.h)
        s_gestureBtn = btn;
        return true;
    }

    // Shift+LMB lends KiwiBox to commands that explicitly WantsMarquee. Keep it
    // below handle hit tests and above generic command LMB; bare LMB retains each
    // command's resume/park meaning.
    if ( btn == 0 && shift && KiwiCmd_Active() && KiwiCmd_WantsMarquee() )
    {
        KiwiHover_Clear();
        KiwiBox_Begin( imgX, imgY, shift, ctrl );
        s_lastX      = imgX;
        s_lastY      = imgY;
        s_gesture    = KG_CMD_MARQUEE;
        s_gestureBtn = btn;
        return true;
    }

    // IdlePressReselect has no Ctrl parameter, so cancel the record-free idle command
    // and use the complete click grammar here. Shift+Ctrl retains Ctrl-wins/remove.
    if ( btn == 0 && ctrl && KiwiCmd_Active()
      && KiwiCmd_Active()->PreemptIdle() )
    {
        const char *name = KiwiCmd_Active()->Name();
        const bool wasFaceMove = name && strcmp( name, "Move" ) == 0;
        const bool wasRegion   = name && strcmp( name, "Extrude Region" ) == 0;
        KiwiCmd_Cancel();
        KiwiHover_Clear();
        KiwiBox_ClickSelectAt( imgX, imgY, shift, ctrl );

        // Removing an absent target (including empty space) is a true no-op, and
        // removing one member from a larger set leaves the same paused tool over
        // the survivors.  This mirrors the Shift re-entry in IdlePressReselect.
        if ( !KiwiCmd_Active() && wasRegion && KiwiRegion_HasSelection() )
        {
            if ( KiwiCmd_Start( KIWI_CMD_EXTRUDE_REGION ) )
                KiwiCmd_Pause();
        }
        else if ( !KiwiCmd_Active() && wasFaceMove && KiwiXform_CanMove() )
        {
            const selection_t &sel = KiwiSel();
            bool haveFace = false;
            for ( size_t i = 0; i < sel.items.size() && !haveFace; ++i )
                haveFace = ( sel.items[i].kind == SEL_FACE );
            if ( haveFace && KiwiCmd_Start( KIWI_CMD_MOVE ) )
                KiwiCmd_Pause();
        }
        s_lastX      = imgX;
        s_lastY      = imgY;
        s_gesture    = KG_CONSUMED;
        s_gestureBtn = btn;
        return true;
    }

    // Active commands get LMB before selection while MMB/wheel navigation stays live.
    // This remains above the master toggle: otherwise legacy Drag_Begin could start
    // over an already-live modern command.
    if ( KiwiCmd_Active() && KiwiCmd_MouseButton( btn, imgX, imgY, shift ) )
    {
        s_lastX      = imgX;
        s_lastY      = imgY;
        s_gesture    = KG_COMMAND;       // own the release too (see KG_COMMAND above)
        s_gestureBtn = btn;
        return true;
    }

    if ( !KiwiUX_ModernInput() )
        return false;

    s_lastX = imgX;
    s_lastY = imgY;

    if ( btn == 2 )                      // MMB — orbit, or truck with Shift (D-1)
    {
        KiwiHover_Clear();
        if ( shift )
        {
            KiwiCam_PanBegin( imgX, imgY );   // anchor pan scale for this gesture
            s_gesture = KG_PAN;
        }
        else
        {
            // Latch Alt from the same ImGui IO that supplied Shift/Ctrl.
            s_mmbAlt   = ImGui::GetIO().KeyAlt;
            s_mmbTravX = 0;
            s_mmbTravY = 0;
            s_mmbDrag  = !s_mmbAlt;           // a BARE MMB orbits from pixel one
            // Alt+MMB remains a swipe, so it never takes the orbit pivot latch.
            if ( !s_mmbAlt )
                KiwiCam_OrbitBegin( imgX, imgY );
            s_gesture = KG_ORBIT;
        }
        s_gestureBtn = btn;
        return true;
    }
    if ( btn == 1 )                      // RMB — pan-or-confirm, Alt = mouselook
    {
        KiwiHover_Clear();
        // Bare/Shift RMB pans; Alt+RMB looks. Alt is latched from the same ImGui IO
        // as Shift/Ctrl so releasing it mid-drag cannot change the gesture.
        // KG_LOOK owns both modes for common click classification; s_rmbLook chooses drag action.
        s_rmbLook = ImGui::GetIO().KeyAlt && !shift;
        // Save press flags for replay; MK_RBUTTON is gone by release (imgui_shell.cpp:226).
        s_rmbFlags  = MK_RBUTTON | ( ctrl ? MK_CONTROL : 0u ) | ( shift ? MK_SHIFT : 0u );
        s_rmbPressX = imgX;
        s_rmbPressY = imgY;
        s_rmbTravX  = 0;
        s_rmbTravY  = 0;
        s_rmbDrag   = false;
        s_gesture   = KG_LOOK;
        s_gestureBtn = btn;
        if ( !s_rmbLook )
            KiwiCam_PanBegin( imgX, imgY );   // anchor pan scale for this gesture
        return true;
    }
    if ( btn == 0 )                      // LMB — select only: marquee / click (§12, D-6)
    {
        // Armed bare Alt+LMB returns false so the shell hands the stroke to the ported chain:
        // CamWnd_OnLButtonDown (camwnd.cpp:4757) -> Drag_Begin (drag.cpp:650-653) ->
        // Patch_Paint_Start (pmesh.cpp:5994). Mirror mode/modifier gates; sub_401D50 gates the ring (:3189).
        const select_t selMode = g_qeglobals.d_select_mode;
        if ( ImGui::GetIO().KeyAlt && !shift && !ctrl
             && ( selMode == sel_brush || selMode == sel_addpoint )
             && sub_401D50() )
        {
            KiwiHover_Clear();
            return false;                // hand the press to the ported chain
        }

        // ImGui's double-click edge is valid here in the pump; the first click already
        // selected the seed. KG_CONSUMED keeps release from KiwiBox_End, which would
        // replace the expanded selection with a single click pick.
        if ( !shift && !ctrl
             && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left )
             && KiwiSelExt_CameraDoubleClick( imgX, imgY ) )
        {
            KiwiHover_Clear();
            s_gesture    = KG_CONSUMED;
            s_gestureBtn = btn;
            return true;
        }

        // A plain drag on an all-model selection uses the ground-contact solver;
        // KiwiDrop_BeginAt also proves the press hit a selected model.
        if ( !shift && !ctrl && !ImGui::GetIO().KeyAlt
             && KiwiDrop_BeginAt( imgX, imgY ) )
        {
            KiwiHover_Clear();
            s_lastX      = imgX;
            s_lastY      = imgY;
            s_gesture    = KG_DROP;
            s_gestureBtn = btn;
            return true;
        }

        // No idle gizmo exists; chips suppress image hover before input dispatch.
        KiwiHover_Clear();
        KiwiBox_Begin( imgX, imgY, shift, ctrl );
        s_gesture    = KG_MARQUEE;
        s_gestureBtn = btn;
        return true;
    }
    return false;
}

bool KiwiVP_CameraMouseMove( int imgX, int imgY )
{
    if ( s_gesture == KG_NONE )
        return false;

    if ( s_gesture == KG_GRASS )
    {
        s_lastX = imgX;
        s_lastY = imgY;
        KiwiGrass_HandleDrag( imgX, imgY );
        return true;
    }

    // Feed a modal preview first even during camera navigation; it does not consume
    // the move, so the owning camera gesture still receives its delta.
    if ( KiwiCmd_Active() )
        KiwiCmd_MouseMove( imgX, imgY );

    // A held rotate ring maps the absolute pixel after command preview so its angle wins.
    if ( s_gesture == KG_GIZMO )
        KiwiGizmo_Drag( imgX, imgY );
    // Section drag maps the absolute cursor along the plane normal from its press baseline.
    if ( s_gesture == KG_SECTION )
        KiwiSection_HandleDrag( imgX, imgY );
    // Sun orbit owns an absolute-pixel accumulator; camera arms still need local dx/dy.
    if ( s_gesture == KG_SUN )
    {
        KiwiSun_HandleDrag( imgX, imgY );
        const int sx = imgX - s_sunPressX;
        const int sy = imgY - s_sunPressY;
        const int ax = ( sx < 0 ) ? -sx : sx;
        const int ay = ( sy < 0 ) ? -sy : sy;
        if ( ax > s_sunMaxX ) s_sunMaxX = ax;
        if ( ay > s_sunMaxY ) s_sunMaxY = ay;
    }

    const int dx = imgX - s_lastX;
    const int dy = imgY - s_lastY;
    s_lastX = imgX;
    s_lastY = imgY;

    if ( s_gesture == KG_ORBIT )
    {
        // Alt+MMB tracks net swipe travel without rotating; bare MMB orbits immediately.
        if ( s_mmbDrag )
        {
            KiwiCam_OrbitDrag( dx, dy );
        }
        else
        {
            s_mmbTravX += dx;
            s_mmbTravY += dy;
            // The ImDrawList swipe hint redraws every ImGui tick; no dirty stamp needed.
        }
    }
    else if ( s_gesture == KG_MARQUEE || s_gesture == KG_CMD_MARQUEE )
        KiwiBox_Update( imgX, imgY );    // one rectangle, selection or command owner
    else if ( s_gesture == KG_PAN )
        KiwiCam_PanDrag( dx, dy );
    else if ( s_gesture == KG_LOOK )
    {
        // Defer motion until click slop is exceeded, then apply the accumulated delta
        // once so a click cannot nudge the view and a drag loses no motion.
        s_rmbTravX += dx;
        s_rmbTravY += dy;
        if ( s_rmbDrag )
        {
            if ( s_rmbLook ) KiwiCam_LookDrag( dx, dy );
            else             KiwiCam_PanDrag ( dx, dy );
        }
        else if ( s_rmbTravX >  KVP_RMB_CLICK_PIXELS || s_rmbTravX < -KVP_RMB_CLICK_PIXELS
               || s_rmbTravY >  KVP_RMB_CLICK_PIXELS || s_rmbTravY < -KVP_RMB_CLICK_PIXELS )
        {
            s_rmbDrag = true;
            if ( s_rmbLook ) KiwiCam_LookDrag( s_rmbTravX, s_rmbTravY );
            else             KiwiCam_PanDrag ( s_rmbTravX, s_rmbTravY );
        }
    }
    // KG_COMMAND/KG_GIZMO were already fed above; ownership is their remaining job.
    return true;
}

bool KiwiVP_CameraButtonUp( int btn, int imgX, int imgY )
{
    if ( s_gesture == KG_NONE )
        return false;
    if ( btn != s_gestureBtn )
        return true;                     // ignore a release whose press was not owned

    // Classify RMB before teardown because confirm may commit and re-enter this layer.
    // Confirm rechecks net travel against 8 px instead of the sticky 4 px drag latch;
    // any tiny pan remains. A menu, if enabled, still requires the tighter no-drag latch.
    const int  rmbAbsX = ( s_rmbTravX < 0 ) ? -s_rmbTravX : s_rmbTravX;
    const int  rmbAbsY = ( s_rmbTravY < 0 ) ? -s_rmbTravY : s_rmbTravY;
    const bool rmbNearClick = ( s_gesture == KG_LOOK )
                           && rmbAbsX <= KVP_RMB_CONFIRM_PIXELS
                           && rmbAbsY <= KVP_RMB_CONFIRM_PIXELS;
    const bool rmbClick = ( s_gesture == KG_LOOK && !s_rmbDrag );
    const bool wantConfirm = rmbNearClick && ( KiwiCmd_Active() != 0 );
    // Modern camera input intentionally disables CamWnd_ContextMenu (camwnd.cpp:4434)
    // without changing the ported handler. Any re-enable must use ReplayLegacyRightClick.
    const bool wantMenu    = false && rmbClick && !wantConfirm;
    // Alt+MMB always changes view: net travel selects directional swipe or ring step.
    const bool wantViewStep = ( s_gesture == KG_ORBIT && s_mmbAlt && !s_mmbDrag );
    int        swipeDir     = -1;
    if ( wantViewStep )
    {
        const int ax = ( s_mmbTravX < 0 ) ? -s_mmbTravX : s_mmbTravX;
        const int ay = ( s_mmbTravY < 0 ) ? -s_mmbTravY : s_mmbTravY;
        if ( ax >= KVP_SWIPE_PIXELS || ay >= KVP_SWIPE_PIXELS )
        {
            // A dead-even diagonal falls to horizontal, which has four destinations.
            if ( ax >= ay ) swipeDir = ( s_mmbTravX > 0 ) ? 1 : 0;   // right / left
            else            swipeDir = ( s_mmbTravY > 0 ) ? 3 : 2;   // down  / up
        }
    }

    if ( s_gesture == KG_GRASS )
        KiwiGrass_HandleUp();
    else if ( s_gesture == KG_MARQUEE )
        KiwiBox_End( imgX, imgY );
    else if ( s_gesture == KG_CMD_MARQUEE )
        CommandMarqueeEnd( imgX, imgY );
    else if ( s_gesture == KG_GIZMO )
    {
        // Each handle release self-guards; exactly one owner is active.
        KiwiLollipop_Release();
        KiwiGizmo_Release();             // drag-to-pause (kiwi_gizmo.h)
    }
    else if ( s_gesture == KG_SECTION )
        KiwiSection_HandleUp();          // ungrab; the plane stays put
    else if ( s_gesture == KG_SUN )
    {
        const int sx = imgX - s_sunPressX;
        const int sy = imgY - s_sunPressY;
        const int ax = ( sx < 0 ) ? -sx : sx;
        const int ay = ( sy < 0 ) ? -sy : sy;
        const bool ctrlClick = s_sunCtrl
                            && s_sunMaxX < KBOX_CLICK_PIXELS
                            && s_sunMaxY < KBOX_CLICK_PIXELS
                            && ax < KBOX_CLICK_PIXELS && ay < KBOX_CLICK_PIXELS;
        if ( ctrlClick )
        {
            KiwiSun_HandleAbort();
            KiwiBox_ClickSelectAt( s_sunPressX, s_sunPressY, false, true );
        }
        else
            KiwiSun_HandleUp();          // travelled drag: commit one undo record
    }
    else if ( s_gesture == KG_DROP )
        KiwiCmd_Commit();                 // live preview closes as one move record
    else if ( s_gesture == KG_LOOK || s_gesture == KG_PAN )
        KiwiCam_PanEnd();                // drop the gesture's cached pan scale
    else if ( s_gesture == KG_ORBIT )
        KiwiCam_OrbitEnd();              // drop the latched orbit frame
    EndGesture();

    // Apply the view step after closing ownership: re-aiming drops the orbit latch.
    if ( wantViewStep )
    {
        if ( swipeDir >= 0 )
            KiwiViewCube_SwipeAxisView( swipeDir );
        else
            KiwiViewCube_StepAxisView();
    }

    if ( wantConfirm )
        KiwiCmd_Confirm();

    // Replay last: TrackPopupMenu nests a message loop, so no gesture may remain live.
    if ( wantMenu )
        ReplayLegacyRightClick( s_rmbPressX, s_rmbPressY );
    return true;
}

bool KiwiVP_CameraWheel( float steps, int imgX, int imgY )
{
    if ( !KiwiUX_ModernInput() )
        return false;
    KiwiCam_Dolly( steps, imgX, imgY );
    return true;
}

void KiwiVP_CameraHover( int imgX, int imgY, bool over )
{
    // The stroke path updates its own cursor; every other gesture hides the armed ring.
    if ( s_gesture != KG_GRASS )
        KiwiGrass_Hover( imgX, imgY, over && s_gesture == KG_NONE );

    if ( !over || s_gesture != KG_NONE )
    {
        KiwiHover_Clear();
        KiwiGizmo_Hover( imgX, imgY, false );
        KiwiLollipop_Hover( imgX, imgY, false );
        KiwiSection_Hover( imgX, imgY, false );
        KiwiSun_Hover( imgX, imgY, false );
        return;
    }

    const ImGuiIO &io = ImGui::GetIO();
    const bool selectionAdd = io.KeyShift;

    // Section and sun handles exist without a command. Shift suppresses section
    // hover because its press will fall through to additive selection.
    KiwiSection_Hover( imgX, imgY, !selectionAdd );
    KiwiSun_Hover( imgX, imgY, true );

    // Idle movement previews a modal command. An unmoved auto-entered command yields
    // to selection hover because its next click may preempt it.
    if ( KiwiCmd_Active() )
    {
        if ( KiwiCmd_Active()->PreemptIdle() )
            KiwiHover_Update( imgX, imgY, io.KeyCtrl );
        else
            KiwiHover_Clear();
        // Gizmos exist only during modal move/rotate commands.
        KiwiGizmo_Hover( imgX, imgY, true );
        // A parked command may expose its lollipop handle.
        KiwiLollipop_Hover( imgX, imgY, !selectionAdd );
        KiwiCmd_MouseMove( imgX, imgY );
        return;
    }

    KiwiLollipop_Hover( imgX, imgY, false );
    KiwiHover_Update( imgX, imgY, io.KeyCtrl );
    // No command means no gizmo; this call clears stale hover state.
    KiwiGizmo_Hover( imgX, imgY, false );
}

bool KiwiVP_CameraAbort()
{
    if ( s_gesture == KG_NONE )
        return false;
    if ( s_gesture == KG_GRASS )
        KiwiGrass_HandleAbort();
    else if ( s_gesture == KG_MARQUEE || s_gesture == KG_CMD_MARQUEE )
        KiwiBox_Cancel();                // discard either owner's rect without selection changes
    else if ( s_gesture == KG_GIZMO )
    {
        KiwiLollipop_Abort();
        KiwiGizmo_Abort();               // exact restore through the command framework
    }
    else if ( s_gesture == KG_SECTION )
        KiwiSection_HandleAbort();       // restore the level at the grab
    else if ( s_gesture == KG_SUN )
        KiwiSun_HandleAbort();           // nothing was written: the worldspawn already
                                         // holds the pre-drag angles
    else if ( s_gesture == KG_DROP )
        KiwiCmd_Cancel();                 // restore the move command's captured baseline
    else if ( s_gesture == KG_LOOK || s_gesture == KG_PAN )
        KiwiCam_PanEnd();                // drop the cached pan scale
    else if ( s_gesture == KG_ORBIT )
        KiwiCam_OrbitEnd();              // drop the latched orbit frame
    EndGesture();
    KiwiHover_Clear();
    return true;
}

// ─── per-tick keyboard fly (post-present) ────────────────────────────────────
// Called after viewport dispatch. RMB look permits modified arrows and the key funnel
// swallows them; idle hover permits bare arrows only. Navigation remains available
// during modal commands, and GetAsyncKeyState does not dequeue command input.
void KiwiVP_CameraTick( bool cursorOver )
{
    // Patch-vertex exit may relink active/selected lists, so update outside Cam_Draw's
    // list walks. Keep it above the input toggle so disabling modern input cannot
    // strand the mode.
    KiwiPatchVerts_Update();

    // The pose signature catches modern and legacy camera mutations after dispatch.
    // Mark XY/Z RTTs directly; g_nUpdateBits would also schedule a redundant camera
    // render. Keep this above the toggle so classic-profile motion is tracked too.
    if ( KiwiCam_PoseChangedSinceLastTick() )
    {
        KiwiViewDirty_Mark( KIWI_DIRTYVIEW_XY );
        KiwiViewDirty_Mark( KIWI_DIRTYVIEW_Z );
    }

    if ( !KiwiUX_ModernInput() )
    {
        // Tick disabled arms to reset dt and clear the key-funnel ownership latch.
        KiwiCam_FlyTick( false, false );
        return;
    }
    const bool lookHeld = ( s_gesture == KG_LOOK );
    const bool arrows   = cursorOver && !ImGuiShell_WantsKeyboard()
                       && s_gesture == KG_NONE;
    KiwiCam_FlyTick( lookHeld, arrows );
}

// ─── overlay (during the ImGui frame) ────────────────────────────────────────
namespace
{
    // Alt+MMB arrows highlight the dominant direction only after swipe threshold.
    // Pure ImDrawList output cannot claim image hover.
    void DrawSwipeHint( float imgMinX, float imgMinY, float imgW, float imgH )
    {
        if ( s_gesture != KG_ORBIT || !s_mmbAlt || s_mmbDrag )
            return;

        const float cx = imgMinX + imgW * 0.5f;
        const float cy = imgMinY + imgH * 0.5f;
        const float r0 = 30.0f;            // inner gap
        const float r1 = 58.0f;            // arrow tip
        const float wg = 15.0f;            // half-width of the arrow base

        const int ax = ( s_mmbTravX < 0 ) ? -s_mmbTravX : s_mmbTravX;
        const int ay = ( s_mmbTravY < 0 ) ? -s_mmbTravY : s_mmbTravY;
        int hot = -1;
        if ( ax >= KVP_SWIPE_PIXELS || ay >= KVP_SWIPE_PIXELS )
        {
            if ( ax >= ay ) hot = ( s_mmbTravX > 0 ) ? 1 : 0;
            else            hot = ( s_mmbTravY > 0 ) ? 3 : 2;
        }

        // Matches KiwiViewCube_SwipeAxisView: left, right, up, down.
        static const float DIRV[4][2] = { { -1.0f, 0.0f }, { 1.0f, 0.0f },
                                          {  0.0f, -1.0f }, { 0.0f, 1.0f } };
        ImDrawList *dl = ImGui::GetWindowDrawList();
        for ( int d = 0; d < 4; ++d )
        {
            const float ux = DIRV[d][0], uy = DIRV[d][1];
            const float px = -uy,        py = ux;          // perpendicular
            const ImVec2 tip ( cx + ux * r1,             cy + uy * r1 );
            const ImVec2 baseL( cx + ux * r0 + px * wg,  cy + uy * r0 + py * wg );
            const ImVec2 baseR( cx + ux * r0 - px * wg,  cy + uy * r0 - py * wg );
            const ImU32  col = ( d == hot ) ? IM_COL32( 255, 226, 110, 235 )
                                            : IM_COL32( 190, 196, 210, 70 );
            dl->AddTriangleFilled( tip, baseL, baseR, col );
        }
    }
}

bool KiwiVP_DrawCameraOverlay( float imgMinX, float imgMinY, float imgW, float imgH )
{
    // Draw screen-space region fills first so all HUD remains above the scaffolding.
    // This ImDrawList-only fallback complements the engine route without claiming hover;
    // its depth limitation is documented by the declaration.
    KiwiRegion_DrawFillsOverlay( imgMinX, imgMinY, imgW, imgH );   // kiwi_region.h:377

    DrawSunRemoveCue( imgMinX, imgMinY, imgW, imgH );
    DrawMarquee( imgMinX, imgMinY );     // under the chips
    DrawSwipeHint( imgMinX, imgMinY, imgW, imgH );

    // Bottom-band slots grow upward in call order, preventing HUD overlap.
    KiwiHud_BandBegin( imgMinY, imgH );

    // Contextual hotkeys are non-interactive ImDrawList output.
    KiwiHints_Draw( imgMinX, imgMinY, imgW, imgH );

    // Snap and numeric HUD output cannot claim image hover.
    if ( KiwiCmd_Active() )
    {
        KiwiSnap_DrawLabel( KiwiCmd_LastSnap(), imgMinX, imgMinY, imgW, imgH );
        KiwiNum_DrawHud( imgMinX, imgMinY, imgW, imgH );
        // The value bubble is pinned to action geometry, not the bottom band.
        KiwiNum_DrawBubble( imgMinX, imgMinY, imgW, imgH );
    }

    // Unconditional: this also ticks the camera-cursor latch used by Pick Texture.
    KiwiUv_DrawReadout( imgMinX, imgMinY, imgW, imgH );

    // View cube and chips report their own hover so clicks cannot start a marquee behind them.
    const bool cubeHot = KiwiViewCube_Draw( imgMinX, imgMinY, imgW, imgH );
    const bool chipHot = DrawChips( imgMinX, imgMinY );
    // Draw after chips; the banner is non-interactive and does not affect the result.
    DrawStateBanner( imgMinX, imgMinY );
    return cubeHot || chipHot;
}

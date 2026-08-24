#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_viewport.cpp — the shell bridge.  See kiwi_viewport.h for the contract and
// for what each consumed event replaces.
//
// ── GESTURE OWNERSHIP ────────────────────────────────────────────────────────
// At most one modern gesture is live at a time (s_gesture).  Once one starts, the
// legacy CamWnd_* handlers see NOTHING for the rest of it — including strays from
// other buttons, whose press was never dispatched anywhere (the shell's dispatch
// only starts a gesture on the first click while unowned).  Feeding a legacy
// up-handler a release whose press it never saw is exactly how drag state wedges.
//
// ── ABORT ────────────────────────────────────────────────────────────────────
// KiwiVP_CameraAbort tears down OUR gesture only and tells the shell it did, so
// the shell's stuck-drag guard does not additionally run CamWnd_AbortDrag (the
// camera's own teardown, for the camera's own drags).  It never pre-empts the
// normal release edge — the shell only calls it after the per-frame release path.
//
// ── RMB: CLICK vs DRAG (spec appendix D-1 / D-2, RESOLVED in shakeout A / E) ─
// The modern layer owns the WHOLE RMB gesture and decides at the RELEASE edge
// which of the two meanings it had:
//
//   travel > KVP_RMB_CLICK_PIXELS   NAVIGATION.  Legacy saw nothing, start to
//                                   finish, so no free-look state was entered and
//                                   there is no hidden cursor to restore.
//                                   SHAKEOUT E, user directive: "The camera should
//                                   be pan on right click (not shift-right click)"
//                                   — a bare RMB drag TRUCKS, and the mouselook
//                                   moves to ALT+RMB.  Shift+RMB stays pan as it
//                                   was, so no muscle memory is broken.
//   travel <= that, COMMAND LIVE    CONFIRM (shakeout E, appendix D-2 reopened
//                                   and resolved).  KiwiCmd_Confirm runs the same
//                                   ladder Enter does; the legacy context menu is
//                                   NOT replayed, because a menu about the
//                                   selection is meaningless mid-edit to it.
//   travel <= that, NOTHING LIVE    CONTEXT MENU.  The legacy down+up PAIR is
//                                   REPLAYED here, in order, with the flags the
//                                   press carried:
//                                     CamWnd_OnRButtonDown (camwnd.cpp:3886)
//                                       -> CamWnd_DropModelsToPlane: sets
//                                          m_nCambuttonstate and, because
//                                          (nFlags & MK_RBUTTON), cam_was_not_dragged
//                                     CamWnd_OnRButtonUp   (camwnd.cpp:3892)
//                                       -> Cam_MouseUp, then CamWnd_ContextMenu,
//                                          whose FIRST gate is cam_was_not_dragged
//                                          (camwnd.cpp:3031) — which is exactly why
//                                          the down half cannot be skipped.
//
// Replaying the pair (rather than calling CamWnd_ContextMenu directly) keeps the
// menu's own preconditions, its ordering deviation and its state teardown in ONE
// place: the ported handler.  This runs in the post-present dispatch, so the
// TrackPopupMenu nested message loop is still outside the compositing frame's
// scene bracket — the rule the whole dispatch site exists for.
//
// (KIWI-UX shakeout E: the replay is now reached ONLY when no modal command is
// live.  With one live the same no-drag release is the CONFIRM instead.)
//
// ── ROUND P/S: ALT+MMB IS THE VIEW GESTURE, AND IT IS A SWIPE ───────────────
// Bare MMB is unchanged and orbits from pixel one.  With ALT HELD AT THE PRESS the
// gesture belongs to the VIEW for its whole life and never becomes an orbit
// (round P deferred the orbit by 4 px and then handed it over; the directive
// "should be more of a swipe, it's hard to actually do a standstill press of mmb"
// says that hand-over was the bug).  The release reads the NET travel: past
// KVP_SWIPE_PIXELS it is a directional 90-degree step
// (KiwiViewCube_SwipeAxisView), under it round P's nearest-then-ring
// KiwiViewCube_StepAxisView — whose header carries the Plasticity trace showing
// alt-MMB is unbound there.  Alt is latched at the press and never re-read, the
// same rule as s_rmbLook.
//
// The one thing the replay neutralises is CamWnd_DropModelsToPlane's one-shot
// cursor-joystick fly (camwnd.cpp:2550-2553): that is the CLASSIC meaning of RMB,
// and in the modern layer RMB is mouselook, so a context click must not also
// nudge the camera.  It is fed dt = 0 for the duration (g_qeglobals.g_oldtime
// saved and restored), which makes CamWnd_MouseControl's every term zero without
// touching a line of ported logic.  (With the stock CameraMode pref of 1 it
// returns before that anyway — camwnd.cpp:2497 — so this only matters at
// CameraMode 0.)
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_viewport.h"
#include "kiwi_boxselect.h"
#include "kiwi_camera.h"
#include "kiwi_command.h"
#include "kiwi_construct.h"      // ROUND BP, ITEM 3 — the PLANE chip reads the latch
#include "kiwi_gizmo.h"
#include "kiwi_hints.h"
#include "kiwi_hover.h"
#include "kiwi_lollipop.h"      // ROUND K — the extrude handle that replaces the gizmo
#include "kiwi_section.h"       // ROUND BM — section analysis (the plane + its lollipop)
#include "kiwi_sun.h"           // the sun helper — its glyph is a grabbable handle
#include "kiwi_transform.h"
#include "kiwi_numeric.h"
#include "kiwi_patchverts.h"    // ROUND AJ, ITEM 1 — the mode's per-frame lifecycle
#include "kiwi_region.h"        // ROUND BL, ITEM 3 — the overlay fill route
#include "kiwi_selection.h"
#include "kiwi_selext.h"
#include "kiwi_snap.h"
#include "kiwi_ux.h"
#include "kiwi_units.h"          // ROUND BP — KiwiUnits_Format (the banner's numbers)
#include "kiwi_uv.h"
#include "kiwi_viewcube.h"
#include "kiwi_viewdirty.h"      // the 2D views' RTT gate — the camera pose marks it

#include <stdio.h>               // ROUND BP — _snprintf (the banner's rows)

// ── ported entry points (verified against their definitions) ────────────────
extern void CamWnd_OnRButtonDown( HWND hwnd, unsigned int nFlags, int x, int y ); // camwnd.cpp:5575
extern void CamWnd_OnRButtonUp  ( HWND hwnd, unsigned int nFlags, int x, int y ); // camwnd.cpp:5581
extern bool ImGuiShell_WantsKeyboard();                                           // imgui_shell.cpp:152
// KIWI-UX (ROUND AO, ITEM Y): the terrain-paint ARMED gate — "a paint mode is
// picked (not Disabled) AND outer radius > inner radius".  Signature copied
// verbatim from its definition, `int sub_401D50()` at patchdialog.cpp:229 (IDB
// 0x401D50); it is the SAME function Drag_Begin (drag.cpp:653) and Cam_Draw
// (camwnd.cpp:3189, the cursor ring) already gate on, so this arm can never
// disagree with either the ring the user sees or the handler it defers to.
// File scope, next to the other ported externs — never at block scope inside the
// anonymous namespace below (that spelling caused the round-AI link failure).
extern int  sub_401D50();                                                         // patchdialog.cpp:229

namespace
{
    // KG_COMMAND (Phase 2): the press was eaten by an active modal command.  It is a real
    // gesture, not a bare "consumed" flag, because the shell hands ownership of the whole
    // press→release cycle to whoever took the press: without it, the release edge would
    // reach CamWnd_OnLButtonUp whose press it never saw — exactly the wedge the GESTURE
    // OWNERSHIP note above exists to prevent.  Its body does nothing; owning is the point.
    // KG_CONSUMED (Phase 5): the press did its whole job at press time (the §25
    // double-click = select-connected) and there is nothing left to do on move or
    // release — but the press→release cycle must still be OWNED, for exactly the
    // reason KG_COMMAND exists.  A separate value rather than reusing KG_COMMAND
    // because no command is active and the move arm must not pretend one is.
    // KG_LOOK / KG_PAN / KG_GIZMO (shakeout A): the RMB gesture (a pending
    // click-or-drag — see the RMB note at the top; shakeout E made the DRAG half
    // a truck unless Alt is held), the MMB/Shift truck, and a §14 gizmo drag.
    // KG_CMD_MARQUEE (ROUND AA, ITEM 9): a Shift+LMB drag LENT TO THE ACTIVE
    // COMMAND (kiwi_command.h WantsMarquee).  It uses the same KiwiBox_* rect —
    // so the §12 overlay draws unchanged — but it NEVER reaches KiwiBox_End, and
    // therefore never touches the selection: the release hands the resolved
    // rectangle to the command instead.  A separate value rather than a flag on
    // KG_MARQUEE, for the reason KG_CONSUMED gives: the release arm must be able to
    // tell the two apart at a glance, and they end differently.
    // KG_SECTION (ROUND BM, ITEM 1b): the SECTION-ANALYSIS lollipop is held.  Its
    // own tag rather than KG_GIZMO, because the section is NOT a command: KG_GIZMO's
    // release and abort arms call into kiwi_lollipop / kiwi_gizmo, which both route
    // through the active command's Rebase/Cancel, and there is no command here to
    // route through (kiwi_section.h states why a section is view state).
    // KG_SUN: the SUN HELPER's glyph is held (kiwi_sun.h).  Its own tag rather than
    // KG_SECTION, for the same reason KG_SECTION is not KG_GIZMO — the two are
    // different owners with different release and abort bodies, and a shared tag
    // would make the release arm guess which one it is holding.
    enum kgesture_t { KG_NONE = 0, KG_ORBIT, KG_MARQUEE, KG_COMMAND, KG_CONSUMED,
                      KG_LOOK, KG_PAN, KG_GIZMO, KG_CMD_MARQUEE, KG_SECTION,
                      KG_SUN, KG_DROP };

    // A press-to-release travel under this many pixels is a CLICK, not a drag.
    // Same MEANING as KBOX_CLICK_PIXELS on the LMB side; ROUND N deliberately left
    // this one at 4 when it raised that to 8.  The two measure different things:
    // this is ACCUMULATED travel (a wandering drag can never be read as a click),
    // and it decides between the context menu / confirm and a camera pan — where a
    // wrong answer costs the user a view, not a selection.  A tighter number is the
    // safer one there, and there is no silent-nothing-happened failure to cure.
    const int KVP_RMB_CLICK_PIXELS = 4;

    // ── KIWI-UX (ROUND Z, ITEM 6): THE CONFIRM GETS A WIDER SLOP ────────────
    // USER REPORT, verbatim: "There needs to be a tiny threshold where right
    // clicking confirms an action even if the camera is slightly moved.  When doing
    // actions fast, this is the case."
    //
    // The threshold above ALREADY existed, and it is the one this file argued for:
    // 4 px, tight, because a wrong answer costs the user a VIEW.  That argument
    // holds for the CONTEXT MENU and does not hold for the CONFIRM: a lost confirm
    // costs the user the whole gesture they were in the middle of, and a couple of
    // pixels of pan costs nothing at all.  Two different questions had been sharing
    // one number.  So the confirm gets its own, and it is KBOX_CLICK_PIXELS (8) —
    // the LMB side's "was this a click", reused for exactly the reason D-X7 gave
    // for reusing it there: one number decides "was this a click" in this editor.
    //
    // PLASTICITY MEASURES THE SAME THING AT THE SAME EDGE.  Its RMB tap is
    // `mouse2 -> command:finish` (default-keymap.ts:378-384) and it fires only when
    // the release is within `consummationDistanceThreshold` (4) and 200 ms of the
    // press — KeyboardEventManager.ts:12-13 and :66-85, measured as
    // `currentPosition.distanceTo(startPosition)` on raw clientX/clientY, i.e. NET
    // displacement from the press, which is exactly what s_rmbTrav* accumulates
    // (a sum of signed deltas telescopes to it).  Their orbit controls see the same
    // pointer stream INDEPENDENTLY (OrbitControls.ts:362-367), so in Plasticity the
    // camera pans during those sub-threshold pixels and the confirm still fires.
    //
    // THE TINY PAN IS ACCEPTED, NOT UNDONE, and that is the deliberate choice:
    //   * it is what Plasticity does, per the two independent streams above;
    //   * KiwiCam_PanDrag is depth- and scale-dependent, so "the inverse pan" is
    //     not simply the negated pixels and an approximate un-pan would be a NEW
    //     source of drift on a gesture the user experienced as a click;
    //   * under 8 px it is invisible, which is the whole premise of a click slop.
    // No time component is added: this layer has no press timestamp, and the pixel
    // test alone is what the report is about.
    const int KVP_RMB_CONFIRM_PIXELS = KBOX_CLICK_PIXELS;

    kgesture_t s_gesture = KG_NONE;
    int        s_gestureBtn = -1;
    int        s_lastX = 0, s_lastY = 0;

    // ── ROUND P: the ALT+MMB CLICK (kiwi_viewcube.h) ────────────────────────
    // Alt held at the MMB press arms a CLICK-OR-ORBIT discrimination on the same
    // terms the RMB has used since shakeout A: nothing rotates until the travel
    // passes the threshold, and a release under it is the click.  Latched at the
    // press and never re-read — releasing Alt half way through a drag must not
    // change what the gesture is, exactly as with s_rmbLook below.
    //
    // WHY IT IS DEFERRED RATHER THAN JUST "SNAP ON A SHORT RELEASE": with the
    // orbit running from pixel one, a 3-pixel click would still have rotated the
    // view by ~1 degree before the snap replaced it, and the snap would then be
    // measuring the nearest face view from a direction the user did not ask for.
    // Deferring costs a bare-MMB orbit nothing (Alt is not held) and costs an
    // Alt+MMB orbit four pixels of lead-in, which are re-applied in one go.
    // ── ROUND S: ...AND NOW IT IS A SWIPE, NOT A CLICK ───────────────────────
    // USER DIRECTIVE, verbatim: "The alt-MMB shortcut should be more of a swipe,
    // it's hard to actually do a standstill press of mmb."
    //
    // Round P's deferral (above) already held the orbit for the first 4 pixels;
    // past that the gesture BECAME an orbit, so a hand that moved — which is what a
    // middle-button press on most mice does — got an orbit instead of a view.  The
    // whole Alt+MMB gesture now belongs to the view: s_mmbDrag is gone and the
    // orbit is never engaged while Alt was held at the press.  BARE MMB is
    // untouched and still orbits from pixel one.
    //
    // s_mmbTravX/Y are the NET displacement from the press (a sum of signed deltas
    // telescopes to exactly that), which is what a swipe DIRECTION has to be read
    // from — an accumulated path length has no direction.  The release picks the
    // dominant axis and calls KiwiViewCube_SwipeAxisView; under the threshold it
    // falls back to round P's KiwiViewCube_StepAxisView, so both behaviours are
    // still reachable exactly as the directive asks.
    //
    // 24 px, not KVP_RMB_CLICK_PIXELS' 4: this decides between "step the ring" and
    // "go that way", and both answers are cheap to undo (swipe back), so the number
    // only has to be comfortably above the involuntary travel of a middle-button
    // press.  4 px is the involuntary travel; 24 is a deliberate flick.
    const int KVP_SWIPE_PIXELS = 24;

    bool         s_mmbAlt   = false;
    int          s_mmbTravX = 0, s_mmbTravY = 0;
    bool         s_mmbDrag  = false;   // bare MMB only: "the orbit is live"

    // KG_LOOK bookkeeping: where the press landed, what flags it carried, and how
    // far the cursor has travelled since (accumulated, not straight-line, so a
    // wandering drag can never be mistaken for a click).
    int          s_rmbPressX = 0, s_rmbPressY = 0;
    unsigned int s_rmbFlags  = 0;
    int          s_rmbTravX  = 0, s_rmbTravY = 0;
    bool         s_rmbDrag   = false;
    // KIWI-UX (shakeout E): which meaning a bare RMB DRAG has.  Latched at the
    // press, never re-read at drag time — releasing Alt half way through a drag
    // must not switch the camera from looking to trucking under the user's hand.
    bool         s_rmbLook   = false;

    // The sun uses Ctrl for two verbs distinguished at release: a stationary
    // click removes the selected helper, while a travelled drag keeps its
    // established five-degree snap.
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

    // ── KIWI-UX (ROUND AA, ITEM 9): the KG_CMD_MARQUEE release ──────────────
    // USER REPORT, verbatim: "the difference command needs to accept multiple
    // shift-clicked brushes. […]  Also support shift box select."
    //
    // THE CLICK HALF IS WHY THIS IS NOT JUST `KiwiBox_End`.  Claiming the press at
    // the top of KiwiVP_CameraButtonDown takes it away from KiwiCmd_MouseButton,
    // which is where a Shift+CLICK becomes a Click() event.  So the release
    // measures the travel with the SAME number the marquee itself uses
    // (KBOX_CLICK_PIXELS, kiwi_boxselect.h — one number decides "was this a click"
    // in this editor) and hands a click straight back to the command's ordinary
    // press path, at the PRESS pixel, with shift forced true because this rung
    // exists only for shift.  Above the threshold it is a real marquee and the
    // command gets the rect.
    //
    // KiwiBox_Cancel — never KiwiBox_End — closes the rect either way: End resolves
    // into KiwiSel(), clears it on a plain drag and re-runs the whole click grammar
    // including the face auto-enter, none of which may happen while a modal command
    // owns the viewport.  This gesture borrows the RECT, not the selection.
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

    // The classic no-drag RMB: replay the ported down+up pair.  See the RMB note
    // at the top of this file for why both halves are needed and why the fly is
    // fed dt = 0.
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

    // KIWI-UX (ROUND AA, ITEM 7): the tooltips now ENUMERATE what each mode picks,
    // because two of them exclude things and a mode that silently refuses a click
    // is a mode the user has to guess at.  The wording matches the gates:
    // kiwi_conselect.cpp ModeAllowsConstructionLines (lines) and the region arm in
    // kiwi_boxselect.cpp ClickSelect (region faces).
    const chip_t KCHIPS[5] =
    {
        { "1 Point",  "Point / vertex selection\nBrush vertices + construction points\n"
                      "Hotkey 1 (modern keymap)",                           SEL_MASK_VERTEX },
        { "2 Edge",   "Edge selection\nBrush edges + construction lines\n"
                      "Hotkey 2 (modern keymap)",                           SEL_MASK_EDGE   },
        // KIWI-UX (ROUND AM, ITEM 7a): the enumeration now names PATCHES, because
        // this round made them clickable here (kiwi_pick.cpp — a patch has no
        // faces, so the patch itself is the finest surface the mode can name) and
        // the header rule above is that a mode says what it picks.
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

    // Phase 2 wired the 1-5 keys, but ONLY in the modern keymap profile — in the
    // classic profile 1-5 are still the grid-size presets (spec §11), so the chips
    // remain the mouse route that works in both.  The keys themselves route through
    // the shared command table (KIWI_CMD_SELMODE_*), never from here.
    // ═══════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BP, ITEMS 1 + 3) — THE LATCHED-STATE BANNER
    // ═══════════════════════════════════════════════════════════════════════════
    // Both of this round's autopsies end in the same sentence: AN INVISIBLE LATCHED
    // PLANE DISTORTING PLACEMENT.  A section armed at a Z level that cut nothing was
    // clamping every pick ray in the editor; a working plane inherited from a sketch
    // drawn on a roof was placing every point 61 ft up.  Neither had ANY on-screen
    // presence, which is why both survived for rounds.
    //
    // So anything that is not the default now says so, in the loudest cheap place —
    // directly under the selection-mode chips, at the top-left where the eye already
    // goes.  Pure ImDrawList: it draws no ImGui item, so it cannot take the image's
    // hover from a marquee (the same rule the hints, the snap label and the numeric
    // HUD all follow) and the function's return value is still the cube's and the
    // chips' alone.  Nothing is drawn at all when both states are default, so the
    // ordinary viewport is unchanged.
    void DrawStateBanner( float imgMinX, float imgMinY )
    {
        char rows[2][96];
        ImU32 cols[2];
        int   n = 0;

        // ── KIWI-UX (ROUND BS): NO PLANE BANNER FOR LINE WORK ────────────────
        // USER RULING: the construction plane is out of the drawing path, so a chip
        // announcing one while a LINE tool runs would be announcing a thing that has
        // no effect — worse than silence, because the user would go looking for it.
        // The banner now appears only while a PLANAR PLACER is live (rect / circle /
        // arc / n-gon and the §16b primitives — KiwiCon_PlanePlacement, which round
        // BS narrowed to exactly those), i.e. only while the plane is doing
        // something.  An explicit plane set with [Space] still announces itself in
        // the console when it is set and again at each planar tool start.
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

        // NO cursor restore here.  A trailing SetCursorScreenPos with no item after
        // it trips ImGui::End's ErrorCheckUsingSetCursorPosToExtendParentBoundaries
        // assert (the post-image cursor sits one ItemSpacing past CursorMaxPos), and
        // validating it with a Dummy would extend the window's content extent —
        // scrollbar feedback loop risk.  Nothing after the chips reads the cursor:
        // the camera window's next call is End(), and the chips place themselves at
        // an absolute screen pos each frame anyway.
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

    // ── marquee rectangle (spec §12) ────────────────────────────────────────
    //
    // ── KIWI-UX (ROUND U): ONE CLEAN TRANSLUCENT RECT, NO DASHES ────────────
    // USER REPORT, verbatim: "Testing box select...  The preview should show a 3d
    // thing being drawn, not just lines extending from each point."
    //
    // WHAT THE USER WAS LOOKING AT, established by reading every pass that can
    // draw during a KG_MARQUEE gesture rather than by guessing:
    //   * this function, which drew the CROSSING marquee as a hand-rolled DASHED
    //     border (6 px dash, 4 px gap, one AddLine per dash) over a 10 %-alpha
    //     fill.  A row of disconnected 6 px segments with an all-but-invisible
    //     interior is exactly "lines extending from each point";
    //   * kiwi_snap.cpp's axis guides — REFUSE outright unless a click-wanting
    //     modal command is live (KiwiSnap_DrawAxisGuides' own gate), and a marquee
    //     runs with no command at all;
    //   * KiwiSnap_DrawLabel / KiwiNum_DrawHud / KiwiNum_DrawBubble — all three
    //     are inside KiwiVP_DrawCameraOverlay's `if ( KiwiCmd_Active() )`;
    //   * the hover accents — KiwiBox_Begin calls KiwiHover_Clear(), and
    //     KiwiVP_CameraMouseMove's KG_MARQUEE arm never re-picks;
    //   * the LEGACY 3D marquee quad (camwnd.cpp Ed_DrawSelectionBoxQuad, gated on
    //     `camera_fov_setup == Camera_GetRectSelection3D` plus a sel_area* mode) —
    //     UNREACHABLE from any modern flow: it is armed only by Drag_Begin, which
    //     is reached only from CamWnd_OnLButtonDown, which KiwiVP_CameraButtonDown
    //     consumes before the shell can dispatch it (imgui_shell.cpp:344-349).
    //     The one way stale state could survive is a classic-profile drag aborted
    //     mid-gesture, and that is torn down by CamWnd_AbortDrag through
    //     ImGuiShell_AbortViewportInput.  Left alone deliberately: adding a
    //     modern-side reset for it would be new code guarding a path that cannot
    //     be entered.
    // So the dashes were the only candidate that fits the description, and they
    // are gone.  BOTH modes now draw the same shape — a translucent fill with a
    // one-pixel solid border — and differ only in HUE, which is the one difference
    // that has to survive (warm = crossing, cool = containment, spec §12).  The
    // fill goes 26 -> 48 alpha so the rect reads as a surface rather than as an
    // outline, which is the "a 3d thing being drawn" half of the report.
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
        // ── KIWI-UX (ROUND X, ITEM 8): NOTHING IS DRAWN UNTIL THE DRAG IS REAL ──
        // USER REPORT, verbatim: "Sometimes when clicking, an object isn't selected
        // and it takes 2 tries.  No clue why.  This also happens with faces.  It
        // might be because I'm moving the mouse very slightly when left clicking."
        //
        // The SELECTION half of that report was a different defect entirely (the
        // ported walker cannot hit an already-selected brush — kiwi_pick.cpp
        // selUnmask_t), and it is fixed there.  What is left is the FEEDBACK: the
        // marquee rectangle appeared on the first pixel of travel, so a click with a
        // hand-wobble in it looked like a box-select even though KiwiBox_End was
        // always going to resolve it as a click (KBOX_CLICK_PIXELS, kiwi_boxselect.h).
        // Two different stories about one gesture is how a user ends up believing
        // the click was lost.
        //
        // Plasticity draws the same line in the same place and calls it
        // "consummating" the drag: `consummationDistanceThreshold = 4` pixels, in
        // clientX/clientY space (plasticity/src/components/viewport/
        // ViewportControl.ts:281-284), tested in `onPointerMove`'s `'down'` state
        // (ViewportControl.ts:149-165) — under it the pointer-up path fires a CLICK
        // (ViewportControl.ts:179-201) and their box-select never starts at all
        // (ViewportSelector.ts:43-48, reached only once the drag is consummated).
        // KIWI keeps its own 8 px rather than adopting their 4: the number already
        // decides the outcome at release, a second threshold would be a second
        // answer to one question, and 8 is the more forgiving of the two — which is
        // the direction this report asks for.
        {
            const int dx = x1 - x0;
            const int dy = y1 - y0;
            if ( ( dx > -KBOX_CLICK_PIXELS && dx < KBOX_CLICK_PIXELS )
              && ( dy > -KBOX_CLICK_PIXELS && dy < KBOX_CLICK_PIXELS ) )
                return;                      // still a CLICK — draw no marquee
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

    // ══════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BM, ITEM 1b) — THE SECTION GOES FIRST, ABOVE EVERYTHING
    // ══════════════════════════════════════════════════════════════════════════
    // Two arms, and both have to outrank every selection and command arm below:
    //   * WHILE PICKING the whole point of the click is to name the section plane,
    //     so it must not also change the selection or start a marquee.
    //   * THE HANDLE is a live grabbable thing that exists with NO command running,
    //     which is exactly the case every arm below assumes cannot happen — the
    //     lollipop and gizmo arms are both gated on KiwiCmd_Active().  Without this
    //     arm a press on the ball would fall through to the no-command click
    //     grammar and re-select whatever is behind it.
    // Shift is excluded from the handle arm because it is additive selection;
    // Ctrl keeps this view-state handle's existing meaning.
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

    // The SUN HELPER's glyph, on exactly the same terms as the section handle
    // above it: a grabbable thing that exists with NO command running, so it has
    // to outrank every arm below (all of which either assume a live command or
    // read the press as a selection click).  Shift is excluded because it adds.
    // Ctrl is resolved at release: a click removes the helper, while a travelled
    // drag retains the established snap-orbit gesture.  KiwiSun_HandleDown
    // self-gates on the helper being selected already, so the first click still
    // falls through to the ordinary grammar (kiwi_sun.h).
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

    // KIWI-UX (shakeout D, §14): THE GIZMO GOES FIRST, and only while a modal
    // command is live.  The arm below commits the active command on ANY LMB
    // press, so with the old ordering a press on a handle committed the gesture
    // before the handle test could ever run — which is why the gizmos are tested
    // here, above it, rather than down in the LMB block where the old
    // idle-selection gizmo lived.
    //
    // The KiwiCmd_Active() guard is what keeps the NO-COMMAND path completely
    // free of gizmo work: with nothing running there is no gizmo to hit (the
    // shakeout-D visibility rule, kiwi_gizmo.h), so not one projection is done.
    // KiwiGizmo_MouseDown itself re-checks the same condition, so this guard is
    // an optimisation and not a correctness dependency.
    // KIWI-UX (ROUND K): THE LOLLIPOP BALL GOES FIRST, above the gizmo, for the
    // same reason the gizmo goes above the command — the arm below would otherwise
    // read the press as "resume the drag" before the handle test could run.  The
    // two can never both be live: kiwi_gizmo.cpp's GizmoUsable() refuses outright
    // while a lollipop is wanted (kiwi_lollipop.h), so this is an ORDERING for
    // clarity rather than an arbitration.  It is owned as KG_GIZMO, which is the
    // gesture tag for "a handle is held": the release and abort arms below feed
    // both files, and each self-guards on its own grabbed flag.
    // KIWI-UX (ROUND U): …but NOT with Shift held.  Shift is ADDITIVE everywhere in
    // this editor (the marquee, the no-command click grammar, IdlePressReselect), and
    // it never means "grab a handle" — so a Shift+click that happens to land within
    // the ball's 12 px pick radius must fall through to the additive rung instead of
    // silently starting a push.  The ball sits KLOL_STEM_PIX (48 px) off the face
    // along its normal, which is close enough to a neighbouring face to matter.
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

    // ── KIWI-UX (ROUND AA, ITEM 9): A SHIFT+LMB DRAG, LENT TO THE COMMAND ────
    // USER REPORT, verbatim: "the difference command needs to accept multiple
    // shift-clicked brushes. […]  Also support shift box select."
    //
    // The marquee was UNREACHABLE while a command was live: the arm immediately
    // below hands every LMB press to KiwiCmd_MouseButton and returns, long before
    // the KiwiBox_Begin arm at the bottom of this function.  That is correct for
    // every command that existed before this round — a press during a transform
    // means "resume / park the drag", never "select something" — so the widening is
    // OPT-IN, per command, through KiwiEditorCommand::WantsMarquee.
    //
    // SHIFT IS THE WHOLE GATE, and it is the same argument the round-N widening of
    // IdlePressReselect makes one file over (kiwi_command.cpp): Shift is additive
    // everywhere in this editor — in the marquee's own grammar, in the no-command
    // click grammar, in IdlePressReselect — and it never means "resume this drag".
    // A BARE press therefore keeps meaning exactly what it meant, for every command
    // including the opted-in ones.
    //
    // BELOW the gizmo arms, deliberately: those are ordered first for their own
    // reason (a handle press must not be read as anything else) and no command that
    // opts into a marquee has a gizmo — kiwi_gizmo.cpp's GizmoUsable refuses unless
    // a transform is active.  So this is an ORDERING, not an arbitration.
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

    // Auto-entered, still-unmoved face/region commands already yield a plain or
    // Shift click through IdlePressReselect.  That virtual has no Ctrl parameter,
    // so intercept Ctrl here, cancel the record-free idle command, and route the
    // press through the one complete click grammar.  Shift+Ctrl therefore follows
    // the documented Ctrl-wins/remove rule too.
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

    // KIWI-UX Phase 2, spec §5 priority 1: an active modal command outranks selection.
    // It is offered LMB ONLY — KiwiCmd_MouseButton refuses everything else — so MMB orbit
    // and the wheel dolly below stay live mid-gesture, which is the whole point of the
    // arbitration order (§4: "camera navigation stays live during modal commands").
    //
    // DELIBERATELY ABOVE THE MASTER-TOGGLE GATE.  A modal command only exists because the
    // user explicitly started one, so this is not a modern path REPLACING a legacy one —
    // and with the toggle off the alternative would be worse than a gap: LMB would open a
    // classic Drag_Begin on top of a live gesture.  With no command running the toggle gate
    // below is reached unchanged, so legacy dispatch is still bit-for-bit what it was.
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
            KiwiCam_PanBegin( imgX, imgY );   // shakeout E: anchor the pan scale
            s_gesture = KG_PAN;
        }
        else
        {
            // ROUND P: Alt latched here (same io the shell passed shift/ctrl from —
            // see the RMB arm below for the full argument about reading it here).
            s_mmbAlt   = ImGui::GetIO().KeyAlt;
            s_mmbTravX = 0;
            s_mmbTravY = 0;
            s_mmbDrag  = !s_mmbAlt;           // a BARE MMB orbits from pixel one
            // ROUND S: an ALT press is a SWIPE for its whole life — it never
            // becomes an orbit, so the pivot latch is not taken either.
            if ( !s_mmbAlt )
                KiwiCam_OrbitBegin( imgX, imgY );
            s_gesture = KG_ORBIT;
        }
        s_gestureBtn = btn;
        return true;
    }
    if ( btn == 1 )                      // RMB — pan-or-confirm/menu, Alt = mouselook
    {
        KiwiHover_Clear();
        // Shift+RMB kept its shakeout-A meaning (truck) so the old muscle memory
        // still works; a BARE RMB drag now means the same thing (shakeout E user
        // directive), and ALT+RMB is where the mouselook went.  Alt is read here
        // rather than plumbed through KiwiVP_CameraButtonDown's signature: the
        // shell already owns the ImGui io this dispatch runs inside
        // (imgui_shell.cpp:344 passes io.KeyShift / io.KeyCtrl from the same io),
        // so reading io.KeyAlt from it is the same source, not a second one.
        //
        // AUDIT: nothing else in this layer binds Alt+RMB.  The camera's modern
        // arms are MMB (orbit), Shift+MMB (pan), RMB (this), wheel (dolly) and the
        // arrow fly; the XY window's Shift+RMB(+Alt) Drag_Begin arming
        // (xywnd.cpp:3083) is a different window and a different handler entirely.
        //
        // The gesture is STILL tagged KG_LOOK either way, because the
        // click-vs-drag discrimination — and therefore the confirm /
        // context-menu decision at the release edge — has to happen for a pan
        // press exactly as it did for a look press.  What the DRAG does is
        // s_rmbLook's business, not the gesture tag's.
        s_rmbLook = ImGui::GetIO().KeyAlt && !shift;
        // The flags the PRESS carried, built exactly as the shell's VP_Flags would
        // have (imgui_shell.cpp:226): the replay must not read them at release
        // time, when the button is already up and MK_RBUTTON has gone.
        s_rmbFlags  = MK_RBUTTON | ( ctrl ? MK_CONTROL : 0u ) | ( shift ? MK_SHIFT : 0u );
        s_rmbPressX = imgX;
        s_rmbPressY = imgY;
        s_rmbTravX  = 0;
        s_rmbTravY  = 0;
        s_rmbDrag   = false;
        s_gesture   = KG_LOOK;
        s_gestureBtn = btn;
        if ( !s_rmbLook )
            KiwiCam_PanBegin( imgX, imgY );   // shakeout E: anchor the pan scale
        return true;
    }
    if ( btn == 0 )                      // LMB — select only: marquee / click (§12, D-6)
    {
        // ── KIWI-UX (ROUND AO, ITEM Y): ALT+LMB IS THE TERRAIN-PAINT STROKE ──
        // USER REPORT, verbatim: "Advanced patch editing (Y): The circle shows up
        // but you can't paint with alt-click.  The inputs get eaten."
        //
        // WHERE THEY WERE EATEN, established by reading the dispatch rather than by
        // guessing.  The shell offers every camera press to this function FIRST
        // (imgui_shell.cpp:409) and runs the legacy CamWnd_On* handler only when it
        // returns false (imgui_shell.cpp:414).  The KiwiBox_Begin arm at the bottom
        // of this block CLAIMED every LMB press as KG_MARQUEE, unconditionally, with
        // no test of Alt at all — so CamWnd_OnLButtonDown (camwnd.cpp:4757) was never
        // called, and with it the whole ported paint chain:
        //     CamWnd_OnLButtonDown
        //       -> CamWnd_DropModelsToPlane   (camwnd.cpp:3538; nFlags == 1 is a
        //          "select" combo, so it reaches the Drag_Begin tail at :3368)
        //       -> Drag_Begin                 (drag.cpp:571) whose LABEL_34 arm at
        //          drag.cpp:650 IS the terrain-paint start: Alt held, buttons 1 or 2,
        //          mode sel_brush/sel_addpoint, sub_401D50() — then Patch_Paint_Start
        //          (pmesh.cpp:5994) and d_select_mode = sel_addpoint.
        // The ring the user sees is drawn from the SAME sub_401D50 gate
        // (camwnd.cpp:3189), which is exactly why it appeared while the stroke did
        // not: the drawing path was reachable and the input path was not.
        //
        // Nothing else was implicated, and each was checked rather than assumed:
        //   * the round-AM WM_SYSKEYDOWN routing (radiant_main.cpp:769-798) only
        //     ever looks at WM_SYSKEYDOWN/WM_SYSKEYUP.  There is no WM_SYS*BUTTON*
        //     message in Win32 — an Alt+click is a plain WM_LBUTTONDOWN — so a mouse
        //     gesture cannot enter that path, the same reason its own header gives
        //     for Alt+MMB (radiant_main.cpp:752).  Its Alt-key-UP swallow is armed
        //     only by a CONSUMED chord and is a keyboard event either way;
        //   * io.WantCaptureMouse gates the LEGACY hidden child windows
        //     (imgui_shell.cpp:1414), not this dispatch — this runs from the pump off
        //     the camera image's own hover;
        //   * Alt+RMB is mouselook and is UNTOUCHED here: it is latched in the RMB
        //     arm above (s_rmbLook) and this arm is LMB-only.
        //
        // WHY THIS RETURNS FALSE INSTEAD OF OWNING A GESTURE.  Declining the press is
        // the whole routing: the shell then runs VP_Down -> CamWnd_OnLButtonDown, and
        // because no modern gesture starts, the rest of the stroke follows by itself —
        // KiwiVP_CameraMouseMove and KiwiVP_CameraButtonUp both return false while
        // s_gesture == KG_NONE, so the shell's owning branch (imgui_shell.cpp:432/435)
        // falls through to VP_Move -> CamWnd_OnMouseMove -> CamWnd_MouseMoved
        // (camwnd.cpp:3862 Drag_MouseMoved -> drag.cpp:1424 sub_43E6F0, the paint dab)
        // and to VP_Up -> CamWnd_OnLButtonUp -> Cam_MouseUp -> Drag_MouseUp.  The
        // flags and the hwnd are the shell's own (VP_Flags, imgui_shell.cpp:263), so
        // no synthetic press is fabricated anywhere.
        //
        // ONE UNDO RECORD PER GESTURE, and it is the PORTED bracket — nothing is
        // opened or closed here.  Patch_Paint_Start does Undo_ClearRedo +
        // Undo_GeneralStart( "patch painting" ) (pmesh.cpp:5996-5997); Drag_MouseUp's
        // tail closes it exactly once through the sel_addpoint arm at drag.cpp:1693-
        // 1697 -> sub_43ECB0 -> Undo_End (pmesh.cpp:4286).  A drag that never gets its
        // release closes the same way: KiwiVP_CameraAbort returns false with no
        // gesture live, so the shell falls through to CamWnd_AbortDrag
        // (imgui_shell.cpp:476-480), which is Cam_MouseUp -> Drag_MouseUp.
        //
        // THE GATE IS THE ARMED TEST AND NOTHING MORE.  sub_401D50() is false the
        // instant the mode is "Disabled" or the radii are not outer > inner, which is
        // the default state of the panel — so with the tool not armed this arm cannot
        // fire and a bare or Alt-held LMB is the marquee it has always been.  Shift
        // and Ctrl are excluded because Drag_Begin's paint arm requires buttons == 1
        // EXACTLY (drag.cpp:651): with Shift held VP_Flags builds 5 and the ported
        // dispatcher would fall through to Drag_Setup — a brush DRAG — which is not
        // what a selection-modifier click should ever turn into.  So the Shift-add
        // and Ctrl-remove rungs keep their meaning and only bare Alt+LMB is handed over.
        //
        // THE SELECT-MODE TEST IS PART OF THE SAME "and only then".  Drag_Begin's
        // paint arm also requires sel_brush or sel_addpoint (drag.cpp:652), and its
        // fall-through for any OTHER mode is Drag_Setup (drag.cpp:676) — so without
        // this test an Alt+LMB in vertex / edge / curve-point mode would be declined
        // here and land on a MOVE drag instead of a paint stroke.  Duplicating one
        // clause of a ported condition is the lesser evil against handing the user a
        // gesture that does something else entirely; the two other clauses are NOT
        // duplicated because failing them makes Drag_Begin return having done
        // nothing (drag.cpp:655-669), which is a harmless no-op.
        const select_t selMode = g_qeglobals.d_select_mode;
        if ( ImGui::GetIO().KeyAlt && !shift && !ctrl
             && ( selMode == sel_brush || selMode == sel_addpoint )
             && sub_401D50() )
        {
            // Belt-and-braces, and the same first line every other arm here has:
            // the shell's own press-tick hover call already clears it (idle is false
            // with a button down, imgui_shell.cpp:424-425) and its owning branch
            // never re-runs hover during the drag, so this cannot un-clear anything.
            // It is here so the clear does not depend on the SHELL's ordering.
            KiwiHover_Clear();
            return false;                // → the ported chain, see above
        }

        // KIWI-UX Phase 5 (§25): DOUBLE-CLICK = select connected.  Read here, in the
        // pump, because that is where the click edge is dispatched and io's
        // click/release edges are still valid in the same tick (imgui_shell.cpp's
        // ImGuiShell_ViewportInput note).  Placed INSIDE the modern-input gate above
        // and BELOW the active-modal-command arm, so the toggle-off path and a live
        // gesture are both untouched.  The FIRST click of the pair already ran the
        // ordinary click-select, so this just re-picks and expands.
        //
        // The gesture is owned as KG_CONSUMED, NOT as a marquee: KiwiBox_End treats a
        // release within KBOX_CLICK_PIXELS of the press as a CLICK SELECT
        // (kiwi_boxselect.cpp:414), which would immediately replace the connected
        // selection with the one brush under the cursor.  Owning it with an inert
        // gesture keeps the release away from both the marquee and
        // CamWnd_OnLButtonUp, whose press it never saw.
        if ( !shift && !ctrl
             && ImGui::IsMouseDoubleClicked( ImGuiMouseButton_Left )
             && KiwiSelExt_CameraDoubleClick( imgX, imgY ) )
        {
            KiwiHover_Clear();
            s_gesture    = KG_CONSUMED;
            s_gestureBtn = btn;
            return true;
        }

        // KIWI-UX: a plain camera drag on an all-model selection defaults to the
        // ground-contact solver.  KiwiDrop_BeginAt performs the same camera pick
        // used to prove that the press landed on one of the selected models.
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

        // KIWI-UX (shakeout D): the §14 gizmo test that USED to sit here is gone.
        // It was the idle-selection gizmo's grab, and there is no idle gizmo any
        // more — the handle test now runs at the top of this function, under the
        // "a modal command is live" guard.  With no command running there is
        // nothing to hit, so this path goes straight to box select, exactly as it
        // did before shakeout A.  (The chip row needs no test here either: a chip
        // under the cursor takes the image's hover during the frame, so the shell
        // never dispatches this press at all — kiwi_viewport.h.)
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

    // A modal command previews on EVERY move, including one that belongs to a live camera
    // gesture: orbiting mid-extrude must keep the preview under the cursor.  It is fed
    // first and does NOT consume — the gesture below still gets its delta.
    if ( KiwiCmd_Active() )
        KiwiCmd_MouseMove( imgX, imgY );

    // KIWI-UX (shakeout D): a HELD ROTATE RING computes its own angle from this
    // pixel and feeds it to the command.  AFTER the command's own MouseMove, so
    // the ring's mapping is the last word for as long as it is held (the move
    // handles need nothing here — the command already mapped the cursor for them).
    if ( s_gesture == KG_GIZMO )
        KiwiGizmo_Drag( imgX, imgY );
    // ROUND BM, ITEM 1b: the section handle maps the CURSOR PIXEL, not the delta —
    // it is an absolute drag along the plane normal, rebased at the press.
    if ( s_gesture == KG_SECTION )
        KiwiSection_HandleDrag( imgX, imgY );
    // The sun glyph orbits from the CURSOR PIXEL, which it turns into its own
    // pitch/yaw accumulator (kiwi_sun.h) — it must not share this function's
    // dx/dy, whose s_lastX/s_lastY are also read by the camera arms below.
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
        // ROUND S: with Alt held nothing rotates AT ALL — the gesture is a swipe
        // and only the net travel is tracked (the direction hint reads it, and the
        // release turns it into a 90-degree view step).  Without Alt this is the
        // untouched bare-MMB orbit, live from pixel one.
        if ( s_mmbDrag )
        {
            KiwiCam_OrbitDrag( dx, dy );
        }
        else
        {
            s_mmbTravX += dx;
            s_mmbTravY += dy;
            // No repaint stamp is needed: the swipe hint is an ImDrawList overlay
            // on the compositing frame (KiwiVP_DrawCameraOverlay), which runs every
            // ImGui tick regardless of g_nUpdateBits.
        }
    }
    else if ( s_gesture == KG_MARQUEE || s_gesture == KG_CMD_MARQUEE )
        KiwiBox_Update( imgX, imgY );    // ROUND AA, ITEM 9 — one rect, two owners
    else if ( s_gesture == KG_PAN )
        KiwiCam_PanDrag( dx, dy );
    else if ( s_gesture == KG_LOOK )
    {
        // Nothing rotates until the press has travelled past the click threshold,
        // so a context click can never nudge the view.  The moment it does, the
        // travel accumulated so far is applied in one go — the view then picks up
        // exactly where the cursor is, with no lost degrees and no jump.
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
    // KG_COMMAND / KG_GIZMO: nothing more to do — the command was already fed above, and
    // holding the gesture is the whole job.
    return true;
}

bool KiwiVP_CameraButtonUp( int btn, int imgX, int imgY )
{
    if ( s_gesture == KG_NONE )
        return false;
    if ( btn != s_gestureBtn )
        return true;                     // stray release — see GESTURE OWNERSHIP

    // A no-drag RMB release has TWO meanings now (shakeout E, appendix D-2): with
    // a modal command live it CONFIRMS, and only otherwise is it the classic
    // context menu.  Both decided here, before the gesture is torn down, because
    // KiwiCmd_Confirm can commit and re-enter this layer.
    //
    // ── KIWI-UX (ROUND Z, ITEM 6): TWO SLOPS, ONE PRESS ─────────────────────
    // The NET travel is re-read here rather than trusting `s_rmbDrag`, because that
    // latch is sticky: once the pan engaged at 4 px there was no way back, and a
    // release 5 px from the press — a click, by any hand doing this at speed — lost
    // the confirm entirely.  See KVP_RMB_CONFIRM_PIXELS for the argument and the
    // Plasticity citation.
    //
    //   CONFIRM  net travel <= 8 (KVP_RMB_CONFIRM_PIXELS), even if the camera
    //            already panned by those pixels — that pan is kept, deliberately.
    //   MENU     net travel <= 4 (!s_rmbDrag, unchanged), because a context menu
    //            popping after a pan the user meant is the worse wrong answer, and
    //            nothing about the menu was reported.
    const int  rmbAbsX = ( s_rmbTravX < 0 ) ? -s_rmbTravX : s_rmbTravX;
    const int  rmbAbsY = ( s_rmbTravY < 0 ) ? -s_rmbTravY : s_rmbTravY;
    const bool rmbNearClick = ( s_gesture == KG_LOOK )
                           && rmbAbsX <= KVP_RMB_CONFIRM_PIXELS
                           && rmbAbsY <= KVP_RMB_CONFIRM_PIXELS;
    const bool rmbClick = ( s_gesture == KG_LOOK && !s_rmbDrag );
    const bool wantConfirm = rmbNearClick && ( KiwiCmd_Active() != 0 );
    // ── KIWI-UX (ROUND BG, ITEM 1): THE LEGACY CAMERA CONTEXT MENU IS GONE ──────
    // USER DIRECTIVE, verbatim: "I want this native right click menu that appears
    // sometimes when right clicking on an object GONE."  (The screenshot is the
    // face-picker popup: a material name, then "Select all" / greyed "Deselect
    // all" — CamWnd_ContextMenu, camwnd.cpp:4434.)
    //
    // The TRIGGER is fenced off here, not the menu: CamWnd_ContextMenu and its three
    // WM_COMMAND handlers stay in camwnd.cpp untouched, ported and reference-intact,
    // and are simply unreachable from this layer now.  (The other end of the same
    // directive is fenced at the ported handler itself, camwnd.cpp CamWnd_OnRButtonUp
    // — that is the arm the LEGACY dispatch used, which is why the menu "appeared
    // sometimes": a release the modern layer never claimed fell through to it.)
    //
    // A bare RMB click in the camera view therefore does nothing at all now; RMB drag
    // still trucks, Alt+RMB still mouselooks and Shift+RMB still pans, and a no-drag
    // release with a modal command live is still the CONFIRM above.  The XY and
    // texture-window menus are untouched — they are different popups and nothing was
    // reported about either.
    const bool wantMenu    = false && rmbClick && !wantConfirm;
    // ROUND S: an ALT+MMB release is ALWAYS a view change (the gesture never became
    // an orbit).  Which one is decided by the NET travel: past KVP_SWIPE_PIXELS on
    // the dominant screen axis it is a directional swipe, under it round P's
    // nearest-then-ring step.  Both are computed here, before the gesture is torn
    // down, for the same reason the confirm/menu decision is.
    const bool wantViewStep = ( s_gesture == KG_ORBIT && s_mmbAlt && !s_mmbDrag );
    int        swipeDir     = -1;
    if ( wantViewStep )
    {
        const int ax = ( s_mmbTravX < 0 ) ? -s_mmbTravX : s_mmbTravX;
        const int ay = ( s_mmbTravY < 0 ) ? -s_mmbTravY : s_mmbTravY;
        if ( ax >= KVP_SWIPE_PIXELS || ay >= KVP_SWIPE_PIXELS )
        {
            // Dominant axis; a dead-even diagonal falls to the HORIZONTAL, which is
            // the axis with four destinations rather than two.
            if ( ax >= ay ) swipeDir = ( s_mmbTravX > 0 ) ? 1 : 0;   // right / left
            else            swipeDir = ( s_mmbTravY > 0 ) ? 3 : 2;   // down  / up
        }
    }

    if ( s_gesture == KG_MARQUEE )
        KiwiBox_End( imgX, imgY );
    else if ( s_gesture == KG_CMD_MARQUEE )
        CommandMarqueeEnd( imgX, imgY );     // ROUND AA, ITEM 9 (see the helper)
    else if ( s_gesture == KG_GIZMO )
    {
        // KIWI-UX (ROUND K): KG_GIZMO now means "a handle is held", and there are
        // two kinds.  Both calls self-guard on their own grabbed flag, so exactly
        // one of them does anything and neither needs to know about the other.
        KiwiLollipop_Release();
        KiwiGizmo_Release();             // shakeout E: drag-to-PAUSE (kiwi_gizmo.h)
    }
    else if ( s_gesture == KG_SECTION )
        KiwiSection_HandleUp();          // ROUND BM: ungrab; the plane stays put
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
        KiwiCam_OrbitEnd();              // ROUND P: drop the latched orbit frame
    EndGesture();

    // ROUND P: AFTER the gesture is closed, for the same reason the menu replay is
    // — the snap re-aims the camera and KiwiCam_LookAlong drops the orbit latch, so
    // no gesture of ours may still be holding one.
    // ROUND S: ...and the swipe arm lands here too, on the same terms.
    if ( wantViewStep )
    {
        if ( swipeDir >= 0 )
            KiwiViewCube_SwipeAxisView( swipeDir );
        else
            KiwiViewCube_StepAxisView();
    }

    if ( wantConfirm )
        KiwiCmd_Confirm();

    // LAST, and only after the gesture is CLOSED: the replay pops a modal
    // TrackPopupMenu whose nested message loop runs before this returns, and no
    // gesture of ours may be live across it.
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
    if ( !over || s_gesture != KG_NONE )
    {
        KiwiHover_Clear();
        KiwiGizmo_Hover( imgX, imgY, false );
        KiwiLollipop_Hover( imgX, imgY, false );   // ROUND K
        KiwiSection_Hover( imgX, imgY, false );    // ROUND BM
        KiwiSun_Hover( imgX, imgY, false );
        return;
    }

    const ImGuiIO &io = ImGui::GetIO();
    const bool selectionAdd = io.KeyShift;

    // ROUND BM, ITEM 1b: the section handle is hoverable in BOTH arms below — it
    // exists whether or not a command is running, which is what makes it different
    // from every other handle in this file.  Shift makes the press
    // fall through to selection, so the handle must not advertise a grab then.
    KiwiSection_Hover( imgX, imgY, !selectionAdd );
    // The sun glyph is the same kind of thing and gets the same treatment
    // (kiwi_sun.h): it exists independently of any command, so it is hovered once
    // here rather than in each arm.
    KiwiSun_Hover( imgX, imgY, true );

    // KIWI-UX Phase 2, spec §5: with a modal command live, an idle move IS the command's
    // preview move (no button is down during a G/extrude drag — the gesture is modal, not
    // press-held).  A live transforming command suppresses selection hover, but an
    // unmoved auto-entered face/region command yields to selection and therefore keeps
    // the same hover preview its next click will consume.
    if ( KiwiCmd_Active() )
    {
        if ( KiwiCmd_Active()->PreemptIdle() )
            KiwiHover_Update( imgX, imgY, io.KeyCtrl );
        else
            KiwiHover_Clear();
        // KIWI-UX (shakeout D): the gizmo hover moved from the IDLE arm to this
        // one, because that is where the gizmos now live — a G / R gesture is
        // modal, no button is held, so its cursor moves arrive here.  A handle or
        // a ring therefore brightens under the cursor during the gesture, which
        // is the only time either exists.
        KiwiGizmo_Hover( imgX, imgY, true );
        // KIWI-UX (ROUND K): the lollipop's ball brightens under the cursor the
        // same way a gizmo handle does, and for the same reason — it is the one
        // thing in the viewport that can be grabbed while the gesture is parked.
        KiwiLollipop_Hover( imgX, imgY, !selectionAdd );
        KiwiCmd_MouseMove( imgX, imgY );
        return;
    }

    KiwiLollipop_Hover( imgX, imgY, false );      // ROUND K — no command, no handle
    KiwiHover_Update( imgX, imgY, io.KeyCtrl );
    // KIWI-UX (shakeout D): with NO command running there is no gizmo, so this is
    // a state clear rather than a hit test (it costs one compare — the gizmo's own
    // "is the move/rotate command active" gate refuses before any projection).
    KiwiGizmo_Hover( imgX, imgY, false );
}

bool KiwiVP_CameraAbort()
{
    if ( s_gesture == KG_NONE )
        return false;
    if ( s_gesture == KG_MARQUEE || s_gesture == KG_CMD_MARQUEE )
        KiwiBox_Cancel();                // drop the rect, change no selection.
                                         // ROUND AA, ITEM 9: the lent marquee aborts
                                         // identically — the command is told nothing,
                                         // so its tool set is left exactly as it was.
    else if ( s_gesture == KG_GIZMO )
    {
        KiwiLollipop_Abort();            // ROUND K — see the release arm above
        KiwiGizmo_Abort();               // exact restore through the command framework
    }
    else if ( s_gesture == KG_SECTION )
        KiwiSection_HandleAbort();       // ROUND BM: restore the level at the grab
    else if ( s_gesture == KG_SUN )
        KiwiSun_HandleAbort();           // nothing was written: the worldspawn already
                                         // holds the pre-drag angles
    else if ( s_gesture == KG_DROP )
        KiwiCmd_Cancel();                 // restore the move command's captured baseline
    else if ( s_gesture == KG_LOOK || s_gesture == KG_PAN )
        KiwiCam_PanEnd();                // shakeout E: drop the cached pan scale
    else if ( s_gesture == KG_ORBIT )
        KiwiCam_OrbitEnd();              // ROUND P: drop the latched orbit frame
    EndGesture();
    KiwiHover_Clear();
    return true;
}

// ─── per-tick keyboard fly (post-present) ────────────────────────────────────
// ONE call per input tick from the shell, AFTER the per-viewport dispatch, so the
// gesture state it reads is this tick's.  Both arms are decided here rather than
// in kiwi_camera.cpp because they are input ARBITRATION questions:
//
//   arm 1  — RMB mouselook live: arrows fly with Shift boost, swallowed from the
//            message path (KiwiCam_FlySwallowKey via KiwiUX_KeyFunnel).  Allowed
//            even during a modal command: §4 says camera navigation never stops.
//            (USER DIRECTIVE: arrows ONLY — WASD clashed with the modern binds.)
//   arm 2  — cursor over the camera image, ImGui wants no text input, no OTHER
//            gesture running: bare arrows fly, no RMB needed (kiwi_camera.cpp
//            explains why the modified arrows are left to the classic bindings),
//            and only unbound-from-classic in the modern keymap profile.
//
// KIWI-UX (shakeout I): arm 2 NO LONGER EXCLUDES A LIVE MODAL COMMAND.  It used to
// test `!KiwiCmd_Active()`, which made the arrow fly the ONE navigation gesture
// that stopped working mid-command — MMB orbit, RMB-drag pan and the wheel dolly
// all stayed live (they are dispatched above the command arm in
// KiwiVP_CameraButtonDown, and KiwiCmd_MouseButton refuses everything but LMB), so
// the exclusion was an inconsistency rather than a rule.  §4 says outright that
// camera navigation never stops for a command, and the user's report — "because
// the camera controls are not on right click, I can't move the camera while
// interfacing with a command" — is exactly this class of gap.
//
// SAFE, and checked rather than assumed: nothing consumes an arrow key inside a
// command.  KiwiCmd_KeyDown's ladder feeds the numeric entry (digits / '.' / '-' /
// backspace), then the command's own KeyDown, then Esc/Enter; no KiwiEditorCommand
// in the tree tests VK_LEFT/RIGHT/UP/DOWN (the only VK_ARROW references outside
// this layer are kiwi_camera.cpp's own fly, kiwi_camera.cpp:498/533-536).  And the
// fly reads the keys with GetAsyncKeyState, so it never touches the message queue
// and cannot double-fire a binding.
void KiwiVP_CameraTick( bool cursorOver )
{
    // ── KIWI-UX (ROUND AJ, ITEM 1): PATCH VERTEX MODE'S LIFECYCLE ───────────
    // Here and NOT in the draw hook: the draw runs inside Cam_Draw, which is
    // walking `active_brushes` / `selected_brushes`, and the full exit relinks
    // brushes between those two lists (D-AI21).  This tick is called once per
    // frame from imgui_shell.cpp, outside every list walk, which is the only place
    // a real exit is safe.  It self-guards when the mode is off, and its selection
    // walk is gated on Sel_Generation() so an idle frame costs one compare.
    //
    // ABOVE the modern-input gate on purpose: flipping the master toggle off must
    // not strand the editor in a mode whose only way out is the modern layer.
    KiwiPatchVerts_Update();

    // ── THE 2D VIEWS FOLLOW THE CAMERA IN REALTIME AGAIN ────────────────────
    // USER REPORT, verbatim: "I notice the camera indicator no longer updates in
    // realtime."  A camera move sets W_CAMERA only — the XY_OVERLAY bit beside it
    // is gated on m_bCamXYUpdate, which is 0 by default (prefs.cpp:46) — so
    // KiwiViewDirty_MarkFromUpdateBits never saw the 2D views and the red camera
    // marker sat still until the staggered heartbeat came round.
    //
    // ONE signature at ONE chokepoint, not a mark per handler: this tick runs
    // AFTER the whole per-viewport input dispatch (imgui_shell.cpp:762) and it
    // catches the LEGACY mutators too (CamWnd_Rotate / Rotate2 / PositionPan,
    // driven from Radiant_OnIdle between frames), which is what a per-handler mark
    // could not do without editing camwnd.cpp's ported bodies.
    //
    // KiwiViewDirty_Mark, NOT `g_nUpdateBits |= W_XY_OVERLAY`: the bit currency
    // also drives Radiant_UpdateWindows' repaint dispatch and carries W_CAMERA
    // beside it, so routing through it would re-render the camera view a second
    // time for a move it has already drawn.  The direct mark says exactly one
    // thing — "this RTT is stale" — and it is VIEW dirtiness only: no cache epoch
    // is touched, because a camera move invalidates no derived data (the 2D passes
    // do no view culling).  Cost: one XY + one Z re-render per tick WHILE the
    // camera moves; idle cost is unchanged.
    //
    // ABOVE the modern-input gate, like the patch-vertex lifecycle above it: the
    // classic input profile moves the camera too and its indicator must track.
    if ( KiwiCam_PoseChangedSinceLastTick() )
    {
        KiwiViewDirty_Mark( KIWI_DIRTYVIEW_XY );
        KiwiViewDirty_Mark( KIWI_DIRTYVIEW_Z );
    }

    if ( !KiwiUX_ModernInput() )
    {
        // Still ticked, with both arms off: that re-latches the fly's dt clock and
        // clears its "the RMB owns the arrows" latch, so flipping the master toggle
        // mid-session can never leave the key funnel swallowing arrows forever.
        KiwiCam_FlyTick( false, false );
        return;
    }
    // KIWI-UX (CLEANUP, C-38): `lookHeld`, not `wasd` — the fly has been arrows-only
    // since shakeout A, and this flag has always meant "RMB look is held".
    const bool lookHeld = ( s_gesture == KG_LOOK );
    const bool arrows   = cursorOver && !ImGuiShell_WantsKeyboard()
                       && s_gesture == KG_NONE;
    KiwiCam_FlyTick( lookHeld, arrows );
}

// ─── overlay (during the ImGui frame) ────────────────────────────────────────
namespace
{
    // ── ROUND S: the swipe DIRECTION HINT ────────────────────────────────────
    // Drawn only while an Alt+MMB swipe is live.  Four arrowheads around the
    // viewport centre; the one the current travel points at lights up, and it only
    // lights up once the travel has passed KVP_SWIPE_PIXELS — so the widget also
    // TELLS the user where the threshold is instead of leaving them to find it.
    // Pure ImDrawList, no ImGui item, so it cannot take the image's hover (the same
    // rule the snap label / numeric HUD / hints panel all follow).
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

        // dir order matches KiwiViewCube_SwipeAxisView: 0 left, 1 right, 2 up, 3 down.
        static const float DIRV[4][2] = { { -1.0f, 0.0f }, { 1.0f, 0.0f },
                                          {  0.0f, -1.0f }, { 0.0f, 1.0f } };
        ImDrawList *dl = ImGui::GetWindowDrawList();
        for ( int d = 0; d < 4; ++d )
        {
            const float ux = DIRV[d][0], uy = DIRV[d][1];
            const float px = -uy,        py = ux;          // the perpendicular
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
    // ══════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BL, ITEM 3) — THE CONSTRUCTION-FACE FILLS, IN THE OVERLAY
    // ══════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: *"There is still no light blue plane where a
    // construction face can be extruded from.  You've failed again!"*
    //
    // Six rounds of engine-route submission fixes have not produced a visible fill,
    // while every element drawn from THIS function — the chips, the snap label, the
    // value bubble, the view cube, the marquee — appears in the user's own
    // screenshots of the missing fill.  So the fill is drawn here as well, in screen
    // space, projected with the editor's one world->image helper.  The engine route
    // is kept and unchanged (kiwi_region.h); this is additive and unconditional.
    //
    // FIRST in the function on purpose: everything below is HUD, and HUD must read
    // on top of scaffolding.  It draws no ImGui item, so it cannot take the image's
    // hover from a marquee — the same rule the hints, the snap label and the numeric
    // HUD all follow, and the return value is still the cube's and the chips' alone.
    // The depth-test limitation is documented on the declaration and in
    // RADIANT_KNOWN_ISSUES.  Reached through kiwi_region.h (included above) rather
    // than through a block-scope extern: an owning header beats a re-declaration,
    // and it keeps clear of the MSVC namespace-mangling hazard kiwi_uv.cpp
    // documents (the same ruling CLEANUP B-28 applied to kiwi_numeric.cpp).
    KiwiRegion_DrawFillsOverlay( imgMinX, imgMinY, imgW, imgH );   // kiwi_region.h:377

    DrawSunRemoveCue( imgMinX, imgMinY, imgW, imgH );
    DrawMarquee( imgMinX, imgMinY );     // under the chips
    DrawSwipeHint( imgMinX, imgMinY, imgW, imgH );   // ROUND S — Alt+MMB only

    // ── KIWI-UX (ROUND Z, ITEM 5): OPEN THE SHARED BOTTOM BAND ──────────────
    // USER REPORT (screenshot): the chip strip, the texture readout and the numeric
    // HUD were drawn on top of one another along the bottom edge.  Each owned its
    // own copy of `imgMinY + imgH - boxH - 12`, so "bottom-left" and "bottom-centre"
    // were the same place for any row wide enough to reach the middle.
    //
    // The band hands out ONE vertical slot per taker, growing upward, and the ORDER
    // is the call order below.  The three calls are therefore REORDERED from what
    // this function had: the chip strip goes FIRST (lowest — it is the persistent
    // grammar and the row a user looks down for), then the numeric HUD, then the
    // texture readout.  Reordering is free for the z-order: all three are
    // ImDrawList-only and, now that they no longer overlap, nothing is on top of
    // anything.  KiwiHints_Draw's return value is void and it claims no hover, so
    // moving it above the command block changes nothing else.
    KiwiHud_BandBegin( imgMinY, imgH );

    // KIWI-UX (shakeout A): the contextual hotkey panel, bottom-left + bottom-right.
    // Pure ImDrawList, non-interactive, so it cannot take the image's hover.
    KiwiHints_Draw( imgMinX, imgMinY, imgW, imgH );

    // KIWI-UX Phase 2: the §6 snap label and the §13 numeric HUD.  Both are pure
    // ImDrawList output (no ImGui items), so neither can take the image's hover away from
    // a marquee — only the chips do that, and they still decide the return value.
    if ( KiwiCmd_Active() )
    {
        KiwiSnap_DrawLabel( KiwiCmd_LastSnap(), imgMinX, imgMinY, imgW, imgH );
        KiwiNum_DrawHud( imgMinX, imgMinY, imgW, imgH );
        // KIWI-UX (shakeout E, §13b): the value bubble pinned AT THE GEOMETRY —
        // "The distance unit should also be somewhere near the line/extrusion/
        // whatever".  Same ImDrawList-only rule as the two above, so it cannot
        // take the image's hover from a marquee either.  NOT a band taker: it is
        // pinned at the action geometry, not at the bottom edge.
        KiwiNum_DrawBubble( imgMinX, imgMinY, imgW, imgH );
    }

    // KIWI-UX Phase 6 (§26): the read-only texdef readout for the ACTIVE face.
    // Same ImDrawList-only rule as the two above, so it cannot steal the image's
    // hover either.  Called UNCONDITIONALLY — it is also this file's once-per-frame
    // tick for the camera-cursor latch Pick Texture reads.
    KiwiUv_DrawReadout( imgMinX, imgMinY, imgW, imgH );

    // KIWI-UX (shakeout A): the orientation view-cube, top-right.  This one IS
    // interactive, but with NO ImGui item (see kiwi_viewcube.h) — it resolves its
    // own click here and reports hover, so it claims the image the same way the
    // chips do and a click on a ball can never also start a marquee behind it.
    // OR-ed with the chips' answer: either one under the cursor drops the hover.
    const bool cubeHot = KiwiViewCube_Draw( imgMinX, imgMinY, imgW, imgH );
    const bool chipHot = DrawChips( imgMinX, imgMinY );
    // ROUND BP: AFTER the chips, so it sits under them and over the image.  Claims
    // no hover (ImDrawList only), so it is not in the return value.
    DrawStateBanner( imgMinX, imgMinY );
    return cubeHot || chipHot;
}

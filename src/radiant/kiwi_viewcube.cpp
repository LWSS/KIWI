#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_viewcube.cpp — the orientation widget.  See kiwi_viewcube.h.
//
// ── SHAKEOUT I: IT IS A GREY CUBE NOW ───────────────────────────────────────
// USER DIRECTIVE: "The cube in the top right is ugly, make it more like blender
// (grey)."  The BODY lost every hue it had; the colour moved to three short axis
// stubs.  The projection, the painter sort, the hit test and the view snap are
// untouched — this round is paint.  The long note is on the KVC_GREY_* block.
//
// ── SHAKEOUT D: IT IS AN ACTUAL CUBE NOW ────────────────────────────────────
// USER DIRECTIVE: "the 'blender cube' you gave me is ugly, needs to be an actual
// cube."  It was six balls on spokes; it is now eight projected corners, six
// filled quads, painter-sorted, with labels on the faces that face the viewer.
// The INTERACTION contract is unchanged — draw-list only, no ImGui items, own
// hover claim, one click = one axis-view snap through KiwiCam_LookAlong.
//
// ── THE PROJECTION ──────────────────────────────────────────────────────────
// The same projection the balls used and the same one kiwi_pick.cpp's ProjectRaw
// uses, minus the perspective divide — i.e. an ORTHOGRAPHIC view of a unit cube
// in the camera's own basis:
//     screen.x = centre.x + dot( v, vright ) * KVC_HALF
//     screen.y = centre.y - dot( v, vup    ) * KVC_HALF     (screen y is down)
//     depth    = dot( v, vpn )                              (+ = away from the eye)
// with v one of the eight (±1,±1,±1) corners.  A corner's projected distance from
// the centre is therefore at most sqrt(3) * KVC_HALF, which is what sets the
// widget footprint below (50.2 px of a 52 px half-box — the cube can never spill
// out of its own backdrop, at any orientation).
//
// ── THE PAINTER SORT ────────────────────────────────────────────────────────
// Six faces, each with an outward normal ±axis.  `dot(normal, vpn)` is positive
// when the face points AWAY from the eye, so sorting descending and drawing in
// that order paints the three back faces first and the three front faces over
// them.  With an opaque fill that IS hidden-surface removal for a convex solid —
// no depth buffer, no z-fighting, six quads.  The same dot is the FRONT/BACK test
// (< 0 = facing the viewer), which gates the labels and the click.
//
// ── THE VIEW SNAP, DERIVED RATHER THAN TABULATED ────────────────────────────
// kiwi_camera.cpp's ANGLE CONVENTION note, verified against CamWnd_BuildMatrix
// (camwnd.cpp:162-172), gives
//     vpn = ( cos(pitch) cos(yaw), cos(pitch) sin(yaw), sin(pitch) )
// with a POSITIVE angles[0] looking UP.  Clicking a face means "stand off that
// face and look back at the pivot", i.e. the new view direction is vpn = -normal,
// and inverting the relation gives  pitch = asin(vpn.z),  yaw = atan2(vpn.y,
// vpn.x).  That one derivation REPRODUCES the six-row table this file used to
// carry, exactly:
//     face    vpn         pitch   yaw
//     +X     (-1, 0, 0)     0     180
//     -X     ( 1, 0, 0)     0       0
//     +Y     ( 0,-1, 0)     0     -90
//     -Y     ( 0, 1, 0)     0      90
//     +Z     ( 0, 0,-1)   -89*    kept
//     -Z     ( 0, 0, 1)    89*    kept
// (*) sin(pitch) = -+1 wants -+90, which the camera clamps to -+89 everywhere
// (KiwiCam_OrbitDrag, KiwiCam_LookDrag) because a pole-exact pitch degenerates
// the yaw-plane forward/right basis CamWnd_BuildMatrix also derives; and at the
// pole atan2(0,0) carries no yaw, so the current one is kept.  Both fall out of
// the general helper below as the degenerate case, which is why the table is
// gone: CORNER clicks (isometric views) need the general form anyway, and two
// code paths for one relation is how they drift apart.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include <imgui/imgui.h>

#include "kiwi_viewcube.h"
#include "kiwi_camera.h"
#include "kiwi_command.h"            // ROUND N — KiwiCmd_StepGrid (the shared clamp)
#include "kiwi_grid.h"               // ROUND AJ, ITEM 5 — the grid-snap master switch
#include "kiwi_numeric.h"            // ROUND AQ, ITEM 4 — KiwiNum_EvalDisplay (the one parser)
#include "kiwi_units.h"              // ROUND N — KiwiUnits_GridSpacingInches
#include "kiwi_section.h"            // ROUND BM, ITEM 1b — the Section Analysis toggle
#include "radiant_registry.h"
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <math.h>
#include <stdio.h>                   // _snprintf — the grid readout
#include <stdlib.h>                  // ROUND R — atof, the typed grid spacing

// ── ported entry points (verified against their definitions) ────────────────
extern camera_s *Ed_Camera();          // camwnd.cpp
extern void      CamWnd_BuildMatrix(); // camwnd.cpp 0x403470
extern int       Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:112

namespace
{
    const float KVC_BOX    = 104.0f;   // widget footprint (the backdrop disc's diameter)
    const float KVC_MARGIN = 10.0f;    // inset from the image's top-right corner
    const float KVC_HALF   = 29.0f;    // cube half-size in pixels (see THE PROJECTION)
    const float KVC_CORNER = 8.0f;     // corner-click grab radius, pixels
    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BM, ITEMS 1b + 2) — THE STRIP ABOVE THE CUBE
    // ═══════════════════════════════════════════════════════════════════════
    // Two new widgets, and the user placed both by name: *"a button above the
    // cube"* (section analysis) and *"a zoom threshold bar at the top above the
    // cube"*.  So the whole cluster moves DOWN by a fixed strip and the strip holds,
    // top to bottom: the zoom bar, its "1px = ..." readout, and the section button.
    //
    // The strip height is a CONSTANT rather than a measured sum, because every other
    // y in this file is an offset chain off imgMinY + KVC_MARGIN and a measured
    // height would make the cube's position depend on the font.  The sub-offsets
    // below add up to it exactly; the label is drawn at a fixed y inside its slot.
    const float KVC_ZOOM_W    = 90.0f;   // the directive's "~90x8" bar
    const float KVC_ZOOM_H    = 8.0f;
    const float KVC_ZOOM_GAP  = 2.0f;    // bar -> label
    const float KVC_ZOOM_TEXT = 14.0f;   // the label's slot (one default-font line)
    const float KVC_SEC_W     = 104.0f;  // == KVC_BOX, so the button lines up with
    const float KVC_SEC_H     = 20.0f;   //    the cube's backdrop disc under it
    const float KVC_SEC_GAP   = 4.0f;    // label -> button
    const float KVC_STRIP_GAP = 6.0f;    // button -> cube
    const float KVC_TOP_STRIP = KVC_ZOOM_H + KVC_ZOOM_GAP + KVC_ZOOM_TEXT
                              + KVC_SEC_GAP + KVC_SEC_H + KVC_STRIP_GAP;

    // A viewport too short to hold the strip AND the cube simply does not get the
    // strip — the same rule the grid pill's own third-row guard applies, rather than
    // pushing the cube off the bottom of the image.
    float KVC_StripH( float imgH )
    {
        return ( imgH >= KVC_BOX * 1.5f + KVC_TOP_STRIP ) ? KVC_TOP_STRIP : 0.0f;
    }
    // THE one expression for "where the cube cluster starts".  Every y in this file
    // that used to read `imgMinY + KVC_MARGIN` reads this instead, so the cube, the
    // projection pill and the grid pill cannot drift apart from the strip.
    float KVC_ClusterTop( float imgMinY, float imgH )
    {
        return imgMinY + KVC_MARGIN + KVC_StripH( imgH );
    }
    // KIWI-UX (CLEANUP, C-39): the local 180/pi constant is gone — RAD2DEG
    // (q_shared.h, reachable through stdafx.h) is the tree's one spelling.

    int s_show = -1;                   // -1 = not read from the profile yet

    // ── ROUND R: the grid pill's TYPE-IN state ──────────────────────────────
    // USER DIRECTIVE: "let me type in the grid spacing manually".  Round N's pill
    // said out loud that the value was NOT clickable because "a text field here
    // would need keyboard focus, and taking the keyboard away from the viewport …
    // is a bad trade".  The directive overrules that, and the trade is affordable
    // because the field is MODAL and TINY: it exists only while the user opened it,
    // Enter and Esc both close it, and while it is open ImGui's io.WantTextInput is
    // true — which the shell's ImGuiShell_WantsKeyboard gate already turns into
    // "the hotkey table sees nothing" (the exact mechanism kiwi_palette.cpp's filter
    // field relies on).  So no key can leak into the editor while it has focus.
    bool  s_gridEdit      = false;
    bool  s_gridEditFocus = false;     // request keyboard focus on the frame it opens
    char  s_gridEditBuf[32] = { 0 };
    float s_gridEditX     = 0.0f;      // where to anchor the popup: the pill's own
    float s_gridEditY     = 0.0f;      // bottom-left, latched when it opens

    // The eight corners as sign triples, in the canonical bit order
    // (bit0 = X, bit1 = Y, bit2 = Z), so corner index and face membership are
    // both one mask test.
    inline void CornerVec( int i, float *out )
    {
        out[0] = ( i & 1 ) ? 1.0f : -1.0f;
        out[1] = ( i & 2 ) ? 1.0f : -1.0f;
        out[2] = ( i & 4 ) ? 1.0f : -1.0f;
    }

    struct face_t2
    {
        int         axis;      // 0 = X, 1 = Y, 2 = Z
        float       sign;      // +1 / -1  (the outward normal is sign * axis)
        const char *label;
    };

    // TOP / BOT rather than Z / -Z: on the two faces a mapper reaches for most,
    // the word is unambiguous where the letter needs the convention explained.
    const face_t2 KVC_FACES[6] =
    {
        { 0,  1.0f, "X"   },
        { 0, -1.0f, "-X"  },
        { 1,  1.0f, "Y"   },
        { 1, -1.0f, "-Y"  },
        { 2,  1.0f, "TOP" },
        { 2, -1.0f, "BOT" },
    };

    // §18's axis language, matching the world axes and the transform accent.
    // SHAKEOUT I: this is now used ONLY by the little protruding axis stubs — the
    // CUBE BODY is grey.  See THE GREY CUBE below.
    const float KVC_AXIS_RGB[3][3] =
    {
        { 0.84f, 0.31f, 0.32f },
        { 0.36f, 0.77f, 0.39f },
        { 0.38f, 0.54f, 0.89f },
    };

    // ── SHAKEOUT I: THE GREY CUBE ───────────────────────────────────────────
    // USER DIRECTIVE, verbatim: "The cube in the top right is ugly, make it more
    // like blender (grey)."
    //
    // It was six SATURATED faces — red/green/blue per axis at two brightnesses —
    // which made a 58-pixel widget the most colour-dense object on the screen and
    // put it in direct competition with the §18 axis language it was borrowing:
    // a red face and the red X world axis meant two different things at once.
    // Blender's navigation gizmo is a NEUTRAL body with colour on the AXES only,
    // and that division of labour is the whole point — grey says "this is chrome",
    // colour says "this is an axis".
    //
    // So: light-grey faces, a darker grey for the back faces (the painter sort
    // still needs the solid to read), a warmer light grey for the hovered one, dark
    // grey label text on the light fill, and subtle darker-grey edges.  The only
    // colour left on the widget is the three short axis stubs (below) and the
    // hovered-corner dot.
    const float KVC_GREY_FRONT = 0.62f;    // the directive's number
    const float KVC_GREY_HOVER = 0.78f;
    const float KVC_GREY_BACK  = 0.30f;    // enough to read as "the far side"

    // How far the coloured axis stubs poke out past the cube, as a multiple of the
    // half-size.  1.0 is the face; 1.32 clears it without touching the backdrop
    // disc (sqrt(3)*29 = 50.2 px of a 52 px radius is the CORNER budget, and a stub
    // runs along an AXIS, so 1.32*29 = 38.3 px is comfortably inside).
    const float KVC_STUB      = 1.32f;
    const char *KVC_STUB_TEXT[3] = { "X", "Y", "Z" };

    // ── ROUND M: the PROJECTION button ──────────────────────────────────────
    // USER DIRECTIVE: "We need an orthographic and perspective camera toggle.
    // Add that in the top right somewhere as a button in the 3d camera viewport."
    // A small pill directly under the cube's backdrop disc, sharing its right
    // margin so the two read as one cluster of view chrome.  Same interaction
    // contract as the cube itself (kiwi_viewcube.h): ImDrawList only, NO ImGui
    // item, resolves its own click, reports hover so a click on it can never also
    // start a marquee in the image behind it.
    const float KVC_PROJ_W   = 40.0f;
    const float KVC_PROJ_H   = 20.0f;
    const float KVC_PROJ_GAP = 6.0f;    // below the disc

    // ── KIWI-UX (ROUND BN, ITEM 1): the PERSPECTIVE GRID button ─────────────
    // USER DIRECTIVE, verbatim: *"When in 'P' camera mode, show another button next
    // to it that enables the ortho Grid while in 'P' mode."*  "Next to it" is taken
    // literally: same row as the P/O pill, immediately to its LEFT, which is the
    // slot the snap checkbox already occupies relative to the grid pill one row
    // down — so the cluster keeps one layout rule instead of gaining a second.
    // It exists ONLY in perspective (in ortho the lattice is unconditional and a
    // control for it would be a lie), and it rides the same left-edge clamp the
    // snap box does: a viewport too narrow simply does not get it.
    const float KVC_PGRID_W   = 20.0f;
    const float KVC_PGRID_GAP = 5.0f;

    // ── ROUND N: the GRID SPACING readout + stepper ─────────────────────────
    // USER DIRECTIVE, verbatim: "the gridsize needs to be changeable.  Put that in
    // the top right somewhere and maybe put it on pageup/pagedown if they aren't
    // taken."  Both halves ship: the keys are in kiwi_keymap.cpp (with the
    // occupancy audit), this is the top-right control.
    //
    // It sits directly under the projection pill, sharing its right margin, so the
    // three widgets read as ONE cluster of view chrome: cube, projection, grid.
    // Same interaction contract as both of them (kiwi_viewcube.h): ImDrawList only,
    // NO ImGui item, resolves its own click, reports hover so a click on a stepper
    // can never also start a marquee in the image behind it.
    //
    // Three zones in one pill: [-] on the left, the value in the middle, [+] on the
    // right.
    //
    // ROUND R: THE VALUE IS NOW CLICKABLE, and round N's argument against it —
    // "a text field here would need keyboard focus, and taking the keyboard away
    // from the viewport … is a bad trade" — is overruled by the directive ("let me
    // type in the grid spacing manually").  The trade is paid for by making the
    // field MODAL: it lives in its own small popup window (DrawGridEditPopup), it
    // only exists while the user opened it, and Enter/Esc both close it.  The pill
    // itself is still ImDrawList-only; the popup is the only ImGui ITEM in the whole
    // top-right cluster.  The [-] / [+] zones now walk a nice-number LADDER rather
    // than halving/doubling (kiwi_command.h KiwiCmd_StepGrid), which is the other
    // half of the same directive.
    const float KVC_GRID_W    = 104.0f;
    const float KVC_GRID_H    = 20.0f;
    const float KVC_GRID_STEP = 22.0f;   // width of each stepper zone
    // ── ROUND AJ, ITEM 5: the grid-SNAP checkbox, immediately left of the pill ──
    // USER DIRECTIVE, verbatim: "Add a checkbox by the grid density setting to
    // enable/disable snapping to grid."  "By" is taken literally: it shares the
    // pill's row and baseline and sits against its left edge, so the two read as
    // one control ("this much grid, and do / do not stick to it") rather than as a
    // widget that happens to be nearby.  Pure ImDrawList like everything else in
    // this cluster (kiwi_viewcube.h's no-ImGui-item contract), so it cannot take
    // the camera image's hover away from a marquee that starts elsewhere.
    const float KVC_SNAP_W   = 20.0f;
    const float KVC_SNAP_GAP = 5.0f;
    // ROUND R: the type-in popup's width.  Sized to its widest string (the hint
    // line), which is what makes a fixed-size NoResize window honest.
    const float KVC_GRID_EDIT_W = 250.0f;


    inline ImU32 Grey( float v, int alpha )
    {
        int g = (int)( v * 255.0f + 0.5f );
        if ( g < 0 )   g = 0;
        if ( g > 255 ) g = 255;
        return IM_COL32( g, g, g, alpha );
    }

    inline ImU32 AxisCol( int axis, float mul, int alpha )
    {
        const float *c = KVC_AXIS_RGB[axis];
        return IM_COL32( (int)( c[0] * mul * 255.0f ),
                         (int)( c[1] * mul * 255.0f ),
                         (int)( c[2] * mul * 255.0f ),
                         alpha );
    }

    // The four corner INDICES of the face, walked around the quad.  For outward
    // normal `sign * axis`, the in-plane basis is u = (axis+1)%3, v = (axis+2)%3
    // and the walk is (+u+v) -> (-u+v) -> (-u-v) -> (+u-v), which traces the
    // polygon in order — all AddConvexPolyFilled asks for.
    void FaceCorners( const face_t2 &f, int *out4 )
    {
        const int u = ( f.axis + 1 ) % 3;
        const int v = ( f.axis + 2 ) % 3;
        const int nb = 1 << f.axis;
        const int ub = 1 << u;
        const int vb = 1 << v;
        const int base = ( f.sign > 0.0f ) ? nb : 0;
        out4[0] = base | ub | vb;
        out4[1] = base |      vb;
        out4[2] = base;
        out4[3] = base | ub;
    }

    // Point-in-convex-quad by consistent cross-product sign.  The quad is a
    // parallelogram in screen space (an orthographic projection of a square), so
    // it is always convex however the cube is turned — degenerate (edge-on) faces
    // collapse to a line, where every cross product is ~0 and the test simply
    // fails, which is the right answer for a face with no clickable area.
    bool PointInQuad( const ImVec2 *q, float px, float py )
    {
        int pos = 0, neg = 0;
        for ( int i = 0; i < 4; ++i )
        {
            const ImVec2 &a = q[i];
            const ImVec2 &b = q[( i + 1 ) & 3];
            const float cross = ( b.x - a.x ) * ( py - a.y ) - ( b.y - a.y ) * ( px - a.x );
            if ( cross >  0.001f ) ++pos;
            if ( cross < -0.001f ) ++neg;
        }
        return !( pos && neg );
    }

    // ── ROUND N: THE SPIN RESET ─────────────────────────────────────────────
    // USER DIRECTIVE, verbatim: "whenever the blender cube is used to snap to a
    // plan, it also needs to reset the spin of the camera so everything is aligned
    // perfectly."
    //
    // Two separate things were wrong, and only one of them was obvious.
    //
    // 1. THE POLES KEPT THE OLD YAW OUTRIGHT.  A TOP / BOT click set pitch to
    //    -+89 and copied c->angles[1] through unchanged (the "degenerate" arm
    //    below), because at the pole atan2(0,0) carries no yaw.  That is true about
    //    the DIRECTION and false about the VIEW: the camera is 1 degree off the
    //    pole, so the residual yaw is exactly the "spin" the user is looking at —
    //    a plan view rolled to whatever angle they happened to be orbiting from.
    //    The fix is to keep the NEAREST QUARTER TURN instead of the raw yaw: the
    //    plan is then axis-aligned, and it is the quarter turn closest to where
    //    the user already was, so the view does not jump a side.
    //
    // 2. THE NON-POLE CASES CARRIED FLOAT RESIDUE.  atan2f(±1, 0) and
    //    atan2f(0, ±1) are exact, but atan2f(±1, ±1) * (180/pi) is 45.000004 or
    //    so, and the corner views are the ones a mapper eyeballs for alignment.
    //    Quantising the ANSWER — not the input — to the cube's own lattice makes
    //    every one of the fourteen clickable directions land on an exact constant:
    //    faces on multiples of 90, corners on multiples of 45.
    //
    // The quantum is chosen from the direction itself rather than passed in, so
    // there is still ONE entry point and the caller cannot mis-tag a click.
    float SnapAngle( float deg, float quantum )
    {
        float q = deg / quantum;
        q = ( q >= 0.0f ) ? floorf( q + 0.5f ) : ceilf( q - 0.5f );
        float out = q * quantum;
        // Keep it in (-180, 180], the same range atan2 answers in, so a snapped
        // value is directly comparable with an unsnapped one.
        while ( out >  180.0f ) out -= 360.0f;
        while ( out <= -180.0f ) out += 360.0f;
        return out;
    }

    // Point the camera along `dir` (the DESIRED vpn).  See THE VIEW SNAP above.
    void LookAlongDirection( const camera_s *c, const float *dir )
    {
        float d[3] = { dir[0], dir[1], dir[2] };
        const float len = sqrtf( Dot3( d, d ) );
        if ( !( len > 1.0e-6f ) )
            return;
        d[0] /= len; d[1] /= len; d[2] /= len;

        float z = d[2];
        if ( z >  1.0f ) z =  1.0f;
        if ( z < -1.0f ) z = -1.0f;
        float pitch = (float)RAD2DEG( asinf( z ) );
        if ( pitch >  89.0f ) pitch =  89.0f;   // the camera's own clamp
        if ( pitch < -89.0f ) pitch = -89.0f;
        // A CORNER click is 35.264 degrees by construction (asin(1/sqrt3)); a FACE
        // click is 0 or the clamped pole.  Neither is a value anyone types, so the
        // pitch is left exactly as derived — the spin the directive is about is the
        // yaw, and rounding the pitch would tilt the isometric views off the cube.
        //
        // A face's own axis, though, must be EXACTLY flat: asinf of a computed 0.0f
        // is 0.0f already, but stating it makes the "no residual fractional angles"
        // rule true by inspection rather than by luck.
        if ( fabsf( pitch ) < 1.0e-3f )
            pitch = 0.0f;

        // The yaw lattice: 45 when the direction has BOTH horizontal components
        // (a corner or an edge-on diagonal), 90 when it has at most one (a face).
        const bool diagonal = ( fabsf( d[0] ) > 1.0e-4f && fabsf( d[1] ) > 1.0e-4f );
        const float quantum = diagonal ? 45.0f : 90.0f;

        // At the pole the yaw plane carries no direction, so there is nothing to
        // derive — snap what the camera ALREADY has to the lattice (point 1 above).
        const bool degenerate = ( fabsf( d[0] ) < 1.0e-4f && fabsf( d[1] ) < 1.0e-4f );
        const float raw = degenerate ? c->angles[1]
                                     : (float)RAD2DEG( atan2f( d[1], d[0] ) );
        KiwiCam_LookAlong( pitch, SnapAngle( raw, quantum ) );
    }

    // ── ROUND P: the six FACE VIEWS, as a ring (kiwi_viewcube.h) ────────────
    // Each row is a DESIRED vpn — the direction the camera looks — using the same
    // relation the face clicks use (vpn = -normal).  The order is Plasticity's own
    // navigate command set walked as a compass: front, right, back, left, then the
    // two poles.
    struct axisView_t
    {
        float       vpn[3];
        const char *name;
    };
    const axisView_t KVC_VIEWS[6] =
    {
        { {  0.0f,  1.0f,  0.0f }, "front"  },   // stand at -Y, look toward +Y
        { { -1.0f,  0.0f,  0.0f }, "right"  },
        { {  0.0f, -1.0f,  0.0f }, "back"   },
        { {  1.0f,  0.0f,  0.0f }, "left"   },
        { {  0.0f,  0.0f, -1.0f }, "top"    },
        { {  0.0f,  0.0f,  1.0f }, "bottom" },
    };
    // How close the current view must be to a face view to count as "already on
    // it" — cos(3 degrees).  Wide enough that the round-N -+89 pole clamp (which
    // makes a TOP view 1 degree off the true pole by construction) still reads as
    // the top view, and far too tight for any orbited view to match by accident.
    const float KVC_VIEW_ALIGNED = 0.99863f;

    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BM, ITEM 2) — THE ZOOM METER
    // ═══════════════════════════════════════════════════════════════════════
    // USER DIRECTIVE, verbatim: *"Add a zoom threshold bar at the top above the
    // cube, this basically shows how far zoomed in/out you are in accordance with
    // the camera and how bad the mouse sensitivity will be."*
    //
    // THE FILL is KiwiCam_ZoomMeter's fraction: the LOG position of the camera's
    // zoom driver inside the range the wheel can actually reach, which makes the bar
    // linear in wheel notches (kiwi_camera.h states why log is the only honest
    // scale for a multiplicative control).  Empty = as zoomed in as the camera goes;
    // full = the ceiling, where the wheel stops.
    //
    // THE COLOUR RAMP answers the second half of the directive.  Sensitivity is
    // WORLD UNITS PER PIXEL, which grows with the zoom-out, so the bar ramps
    // green -> amber -> red toward the far end.  The ramp is over the SAME fraction
    // as the fill, so the colour and the length can never disagree.
    //
    // AND THE NUMBER IS PRINTED, because a colour is a hint and a number is an
    // answer: "1px = 2ft 3in" through KiwiUnits_Format, the editor's one formatter
    // (kiwi_units.h), so it reads in whatever units everything else does.
    //
    // DISPLAY ONLY in v1.  Click-to-set-zoom was considered and left out: the bar is
    // 8 px tall and sits directly under the mouse's path to the section button, so a
    // stray click would teleport the zoom — a readout that cannot be misfired is
    // worth more here than one more way to zoom.  It still reports its hover so the
    // image does not start a marquee under it.
    bool DrawZoomBar( float imgMinX, float imgMinY, float imgW, float imgH )
    {
        if ( KVC_StripH( imgH ) <= 0.0f )
            return false;                    // no room: the strip is not drawn at all

        float frac = 0.0f, wpp = 0.0f;
        if ( !KiwiCam_ZoomMeter( &frac, &wpp ) )
            return false;

        const float x1 = imgMinX + imgW - KVC_MARGIN;
        const float x0 = x1 - KVC_ZOOM_W;
        const float y0 = imgMinY + KVC_MARGIN;
        const float y1 = y0 + KVC_ZOOM_H;

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool   hot   = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows )
                          && mouse.x >= x0 && mouse.x <= x1
                          && mouse.y >= y0 && mouse.y <= y1 + KVC_ZOOM_GAP + KVC_ZOOM_TEXT;

        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                           IM_COL32( 18, 18, 22, 175 ), 2.0f );

        // green (0) -> amber (0.5) -> red (1), two linear legs so the middle is a
        // real amber rather than the muddy olive a single green->red lerp gives.
        float r, g, b;
        if ( frac < 0.5f )
        {
            const float t = frac * 2.0f;
            r = 0.36f + ( 0.98f - 0.36f ) * t;
            g = 0.80f + ( 0.75f - 0.80f ) * t;
            b = 0.36f + ( 0.22f - 0.36f ) * t;
        }
        else
        {
            const float t = ( frac - 0.5f ) * 2.0f;
            r = 0.98f + ( 0.95f - 0.98f ) * t;
            g = 0.75f + ( 0.24f - 0.75f ) * t;
            b = 0.22f + ( 0.20f - 0.22f ) * t;
        }
        const ImU32 col = IM_COL32( (int)( r * 255.0f ), (int)( g * 255.0f ),
                                    (int)( b * 255.0f ), 235 );

        // A minimum sliver so "fully zoomed in" still reads as a bar and not as an
        // empty box the user has to decode.
        float fillW = ( x1 - x0 ) * frac;
        if ( fillW < 2.0f ) fillW = 2.0f;
        dl->AddRectFilled( ImVec2( x0, y0 ), ImVec2( x0 + fillW, y1 ), col, 2.0f );
        dl->AddRect( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                     IM_COL32( 80, 84, 96, 150 ), 2.0f, 0, 1.0f );

        char num[64];
        char label[96];
        KiwiUnits_Format( num, sizeof( num ), wpp );
        _snprintf( label, sizeof( label ), "1px = %s", num );
        label[sizeof( label ) - 1] = 0;
        const ImVec2 ts = ImGui::CalcTextSize( label );
        dl->AddText( ImVec2( x1 - ts.x, y1 + KVC_ZOOM_GAP ),
                     hot ? IM_COL32( 255, 226, 110, 255 ) : IM_COL32( 190, 196, 208, 225 ),
                     label );
        return hot;
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BM, ITEM 1b) — THE SECTION ANALYSIS BUTTON
    // ═══════════════════════════════════════════════════════════════════════
    // The directive puts it here by name: *"It's a button above the cube"*.  It is a
    // three-state readout, because the feature has three states and a button that
    // only said on/off would leave the middle one — "click something to place the
    // plane" — invisible:
    //     SECTION        off
    //     PICK A PLANE   armed, waiting for the click (amber)
    //     SECTION ON     live (amber fill, so it reads as engaged at a glance)
    // Same rules as every other widget in this file: ImDrawList only, own hover, own
    // click, hover reported back.  The click routes through KiwiSection_Toggle, the
    // SAME entry point the palette row dispatches to, so the two can never diverge.
    bool DrawSectionButton( float imgMinX, float imgMinY, float imgW, float imgH )
    {
        if ( KVC_StripH( imgH ) <= 0.0f )
            return false;

        const float x1 = imgMinX + imgW - KVC_MARGIN;
        const float x0 = x1 - KVC_SEC_W;
        const float y0 = imgMinY + KVC_MARGIN + KVC_ZOOM_H + KVC_ZOOM_GAP
                       + KVC_ZOOM_TEXT + KVC_SEC_GAP;
        const float y1 = y0 + KVC_SEC_H;

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool   hot   = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows )
                          && mouse.x >= x0 && mouse.x <= x1
                          && mouse.y >= y0 && mouse.y <= y1;

        const bool on   = KiwiSection_Active();
        const bool pick = KiwiSection_Picking();

        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImU32 fill = hot ? IM_COL32( 62, 66, 78, 225 ) : IM_COL32( 18, 18, 22, 175 );
        if ( on )
            fill = hot ? IM_COL32( 150, 110, 30, 235 ) : IM_COL32( 118, 86, 22, 215 );
        else if ( pick )
            fill = hot ? IM_COL32( 96, 84, 44, 235 ) : IM_COL32( 74, 64, 34, 205 );
        dl->AddRectFilled( ImVec2( x0, y0 ), ImVec2( x1, y1 ), fill, 4.0f );
        dl->AddRect( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                     ( on || pick ) ? IM_COL32( 240, 200, 90, 200 )
                                    : IM_COL32( 80, 84, 96, 150 ), 4.0f, 0, 1.0f );

        // ── KIWI-UX (ROUND BP, ITEM 1/3): ARMED IS LOUD, AND IT CARRIES THE LEVEL ──
        // The round-BP autopsy's first correction: "the SECTION chip is visible" was
        // taken as evidence the section was armed, and it never was evidence — this
        // button is drawn UNCONDITIONALLY and read "SECTION" while OFF.  An armed
        // section that could not be told apart from a disarmed one at a glance is
        // exactly how an invisible cut clamped the user's picks for a whole session.
        // So the label now STATES the state and the number: "SECTION  Z 61 ft" while
        // live, and "SECTION (no cut)" when it is armed but the frame refused the
        // fold (a level ortho view, or the camera below the cut) — which is also the
        // state in which the pick clamp is off, so the two readouts cannot disagree.
        char label[64];
        if ( on )
        {
            char zb[32];
            KiwiUnits_Format( zb, sizeof( zb ), KiwiSection_Level() );
            _snprintf( label, sizeof( label ),
                       KiwiSection_Cutting() ? "SECTION  Z %s" : "SECTION (no cut)", zb );
        }
        else
        {
            _snprintf( label, sizeof( label ), "%s", pick ? "CLICK A HEIGHT" : "SECTION" );
        }
        label[sizeof( label ) - 1] = 0;
        const ImVec2 ts = ImGui::CalcTextSize( label );
        dl->AddText( ImVec2( ( x0 + x1 ) * 0.5f - ts.x * 0.5f,
                             ( y0 + y1 ) * 0.5f - ts.y * 0.5f ),
                     ( hot || on || pick ) ? IM_COL32( 255, 234, 150, 255 )
                                           : IM_COL32( 210, 214, 224, 235 ),
                     label );

        if ( hot )
            ImGui::SetTooltip( "Section analysis - cut the 3D view at a Z LEVEL.\n"
                               "Click, then click anything: everything above that\n"
                               "point's height disappears.  Drag the lollipop to\n"
                               "slide the level.  Click again to clear." );

        if ( hot && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
            KiwiSection_Toggle();
        return hot;
    }

    // ROUND M: the P/O pill.  Returns true when the cursor is over it (the caller
    // ORs that into the image's hover claim, exactly as it does for the cube).
    bool DrawProjButton( float imgMinX, float imgMinY, float imgW, float imgH )
    {
        const float x1 = imgMinX + imgW - KVC_MARGIN;
        const float x0 = x1 - KVC_PROJ_W;
        // KIWI-UX (ROUND BM): off KVC_ClusterTop, not off imgMinY + KVC_MARGIN — the
        // whole cluster sits below the new strip now.
        const float y0 = KVC_ClusterTop( imgMinY, imgH ) + KVC_BOX + KVC_PROJ_GAP;
        const float y1 = y0 + KVC_PROJ_H;

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool   hot   = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows )
                          && mouse.x >= x0 && mouse.x <= x1
                          && mouse.y >= y0 && mouse.y <= y1;

        const bool  ortho = KiwiCam_Ortho();
        ImDrawList *dl    = ImGui::GetWindowDrawList();
        dl->AddRectFilled( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                           hot ? IM_COL32( 62, 66, 78, 225 ) : IM_COL32( 18, 18, 22, 175 ),
                           4.0f );
        dl->AddRect( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                     IM_COL32( 80, 84, 96, 150 ), 4.0f, 0, 1.0f );

        // The label is what the camera IS, not what the click would do — the same
        // convention the mode chips use, and the one that survives being glanced at.
        const char  *label = ortho ? "ORTHO" : "PERSP";
        const ImVec2 ts    = ImGui::CalcTextSize( label );
        // Both words are wider than the pill at the default font, so draw the short
        // form when it does not fit rather than spilling over the border.
        if ( ts.x + 6.0f > KVC_PROJ_W )
        {
            label = ortho ? "O" : "P";
        }
        const ImVec2 ts2 = ImGui::CalcTextSize( label );
        dl->AddText( ImVec2( ( x0 + x1 ) * 0.5f - ts2.x * 0.5f,
                             ( y0 + y1 ) * 0.5f - ts2.y * 0.5f ),
                     hot ? IM_COL32( 255, 226, 110, 255 ) : IM_COL32( 210, 214, 224, 235 ),
                     label );

        if ( hot && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
            KiwiCam_SetOrtho( !ortho );

        // ── KIWI-UX (ROUND BN, ITEM 1): the perspective-grid box, left of the pill ──
        // Drawn from INSIDE this function rather than as a fifth top-level widget
        // because its position is defined relative to the pill and its visibility is
        // defined by the pill's own state — two reasons to keep the two together, and
        // the same call shape DrawGridButton uses for the snap box it owns.
        bool pgHot = false;
        if ( !ortho )
        {
            const float gx1 = x0 - KVC_PGRID_GAP;
            const float gx0 = gx1 - KVC_PGRID_W;
            if ( gx0 >= imgMinX + KVC_MARGIN )
            {
                const bool on   = KiwiGrid_PerspGrid();
                const bool over = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows )
                               && mouse.x >= gx0 && mouse.x <= gx1
                               && mouse.y >= y0  && mouse.y <= y1;
                pgHot = over;

                dl->AddRectFilled( ImVec2( gx0, y0 ), ImVec2( gx1, y1 ),
                                   over ? IM_COL32( 62, 66, 78, 225 )
                                        : IM_COL32( 18, 18, 22, 175 ), 4.0f );
                dl->AddRect( ImVec2( gx0, y0 ), ImVec2( gx1, y1 ),
                             IM_COL32( 80, 84, 96, 150 ), 4.0f, 0, 1.0f );

                // A 2x2 LATTICE glyph rather than a letter: the box is 20 px and the
                // thing it switches on is literally this shape.  Lit when on, dim
                // when off, which is the same on/off language the snap box uses.
                const ImU32 ink = on ? IM_COL32( 255, 226, 110, 255 )
                                     : IM_COL32( 120, 126, 138, 220 );
                const float gx  = ( gx0 + gx1 ) * 0.5f;
                const float gy  = ( y0  + y1  ) * 0.5f;
                const float r   = 5.0f;
                for ( int i = -1; i <= 1; ++i )
                {
                    const float f = (float)i * r;
                    dl->AddLine( ImVec2( gx - r, gy + f ), ImVec2( gx + r, gy + f ), ink, 1.0f );
                    dl->AddLine( ImVec2( gx + f, gy - r ), ImVec2( gx + f, gy + r ), ink, 1.0f );
                }

                if ( over && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
                    KiwiGrid_SetPerspGrid( !on );
                if ( over )
                    ImGui::SetTooltip( "Ground grid in PERSPECTIVE (off by default).\n"
                                       "Spacing is chosen at the ORBIT PIVOT's depth and the\n"
                                       "lattice stops where its own cells stop resolving, so\n"
                                       "near the horizon you get the major lines and then\n"
                                       "nothing.  Ortho is unaffected." );
            }
        }
        return hot || pgHot;
    }

    // ROUND N: the grid readout + stepper.  Returns true when the cursor is over it
    // (the caller ORs that into the image's hover claim, exactly as it does for the
    // cube and the projection pill).
    bool DrawGridButton( float imgMinX, float imgMinY, float imgW, float imgH )
    {
        const float x1 = imgMinX + imgW - KVC_MARGIN;
        const float x0 = x1 - KVC_GRID_W;
        const float y0 = KVC_ClusterTop( imgMinY, imgH ) + KVC_BOX + KVC_PROJ_GAP
                       + KVC_PROJ_H + KVC_PROJ_GAP;   // KIWI-UX (ROUND BM): the strip
        const float y1 = y0 + KVC_GRID_H;
        // The cluster is three widgets tall now and the caller's own size guard
        // (KVC_BOX * 1.5) only covers two.  A viewport too short for the third
        // simply does not get it — the grid still has [ ], PageUp/PageDown and the
        // settings block — rather than having it hang off the bottom of the image.
        if ( y1 > imgMinY + imgH - KVC_MARGIN )
            return false;

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const bool   inWin = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows );
        const bool   over  = inWin
                          && mouse.x >= x0 && mouse.x <= x1
                          && mouse.y >= y0 && mouse.y <= y1;
        const bool hotMinus = over && mouse.x <  x0 + KVC_GRID_STEP;
        const bool hotPlus  = over && mouse.x >= x1 - KVC_GRID_STEP;

        // ROUND AJ, ITEM 5: the snap checkbox's own rect, left of the pill.  It
        // rides the same left-edge clamp the type-in popup uses — a viewport too
        // narrow to hold it simply does not get it, rather than having it hang off
        // the image (the same rule the third-row height guard above applies).
        const float sx1 = x0 - KVC_SNAP_GAP;
        const float sx0 = sx1 - KVC_SNAP_W;
        const bool  haveSnapBox = ( sx0 >= imgMinX + KVC_MARGIN );
        const bool  overSnap = haveSnapBox && inWin
                            && mouse.x >= sx0 && mouse.x <= sx1
                            && mouse.y >= y0  && mouse.y <= y1;

        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                           over ? IM_COL32( 62, 66, 78, 225 ) : IM_COL32( 18, 18, 22, 175 ),
                           4.0f );
        dl->AddRect( ImVec2( x0, y0 ), ImVec2( x1, y1 ),
                     IM_COL32( 80, 84, 96, 150 ), 4.0f, 0, 1.0f );
        // The two zone dividers, so the steppers read as buttons rather than as
        // decoration on a label.
        dl->AddLine( ImVec2( x0 + KVC_GRID_STEP, y0 + 3.0f ),
                     ImVec2( x0 + KVC_GRID_STEP, y1 - 3.0f ),
                     IM_COL32( 80, 84, 96, 120 ), 1.0f );
        dl->AddLine( ImVec2( x1 - KVC_GRID_STEP, y0 + 3.0f ),
                     ImVec2( x1 - KVC_GRID_STEP, y1 - 3.0f ),
                     IM_COL32( 80, 84, 96, 120 ), 1.0f );

        const ImU32 dim = IM_COL32( 210, 214, 224, 235 );
        const ImU32 lit = IM_COL32( 255, 226, 110, 255 );

        {
            const ImVec2 ts = ImGui::CalcTextSize( "-" );
            dl->AddText( ImVec2( x0 + KVC_GRID_STEP * 0.5f - ts.x * 0.5f,
                                 ( y0 + y1 ) * 0.5f - ts.y * 0.5f ),
                         hotMinus ? lit : dim, "-" );
        }
        {
            const ImVec2 ts = ImGui::CalcTextSize( "+" );
            dl->AddText( ImVec2( x1 - KVC_GRID_STEP * 0.5f - ts.x * 0.5f,
                                 ( y0 + y1 ) * 0.5f - ts.y * 0.5f ),
                         hotPlus ? lit : dim, "+" );
        }

        // The value, in the SAME display units every other number in the editor uses
        // (§17): the spacing is held in inches, so this prints it directly rather
        // than routing a world value through KiwiUnits_Format — which would convert
        // twice.  %g keeps 0.125 and 1024 both short and never prints "10.000000".
        char buf[32];
        _snprintf( buf, sizeof( buf ), "%g in", (double)KiwiUnits_GridSpacingInches() );
        buf[sizeof( buf ) - 1] = '\0';
        const ImVec2 ts = ImGui::CalcTextSize( buf );
        dl->AddText( ImVec2( ( x0 + x1 ) * 0.5f - ts.x * 0.5f,
                             ( y0 + y1 ) * 0.5f - ts.y * 0.5f ),
                     over ? IM_COL32( 236, 240, 248, 250 ) : dim, buf );

        if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
        {
            // KiwiCmd_StepGrid, not KiwiUnits_SetGridSpacingInches: the clamp, the
            // LADDER (round R) and the console line all live in one place
            // (kiwi_command.h).
            if ( hotPlus )       KiwiCmd_StepGrid( true );
            else if ( hotMinus ) KiwiCmd_StepGrid( false );
            else if ( over )
            {
                // ROUND R: the MIDDLE zone opens the type-in.  Seeded with the
                // current spacing (not blank) so the field doubles as a readout the
                // user can edit rather than one they must retype, and anchored under
                // the pill so the popup reads as belonging to it.
                _snprintf( s_gridEditBuf, sizeof( s_gridEditBuf ), "%g",
                           (double)KiwiUnits_GridSpacingInches() );
                s_gridEditBuf[sizeof( s_gridEditBuf ) - 1] = '\0';
                s_gridEdit      = true;
                s_gridEditFocus = true;
                // RIGHT-aligned to the pill, not left: the pill lives against the
                // image's right margin and the popup is wider than it is, so
                // anchoring the left edges would hang it off the viewport.  Clamped
                // to the image's left edge for the degenerate narrow-viewport case.
                s_gridEditX     = x1 - KVC_GRID_EDIT_W;
                if ( s_gridEditX < imgMinX + KVC_MARGIN )
                    s_gridEditX = imgMinX + KVC_MARGIN;
                s_gridEditY     = y1 + 4.0f;
            }
        }

        // ── ROUND AJ, ITEM 5: the SNAP checkbox ─────────────────────────────
        // The glyph is a 2x2 LATTICE, struck through when snapping is off — a
        // tick would say "on" without saying what is on, and this cluster has no
        // room for a label and no ImGui item to hang a tooltip from.  The state
        // is also announced on the console by KiwiGrid_SetSnapEnabled, which is
        // where the one sentence of explanation lives.
        if ( haveSnapBox )
        {
            const bool on = KiwiGrid_SnapEnabled();
            dl->AddRectFilled( ImVec2( sx0, y0 ), ImVec2( sx1, y1 ),
                               overSnap ? IM_COL32( 62, 66, 78, 225 )
                                        : IM_COL32( 18, 18, 22, 175 ), 4.0f );
            dl->AddRect( ImVec2( sx0, y0 ), ImVec2( sx1, y1 ),
                         IM_COL32( 80, 84, 96, 150 ), 4.0f, 0, 1.0f );

            const ImU32 glyph = on ? IM_COL32( 255, 226, 110, 255 )
                                   : IM_COL32( 120, 124, 136, 220 );
            const float gx0 = sx0 + 5.0f, gx1 = sx1 - 5.0f;
            const float gy0 = y0  + 5.0f, gy1 = y1  - 5.0f;
            for ( int i = 1; i <= 2; ++i )
            {
                const float fx = gx0 + ( gx1 - gx0 ) * ( (float)i / 3.0f );
                const float fy = gy0 + ( gy1 - gy0 ) * ( (float)i / 3.0f );
                dl->AddLine( ImVec2( fx, gy0 ), ImVec2( fx, gy1 ), glyph, 1.0f );
                dl->AddLine( ImVec2( gx0, fy ), ImVec2( gx1, fy ), glyph, 1.0f );
            }
            if ( !on )
                dl->AddLine( ImVec2( sx0 + 3.0f, y1 - 3.0f ),
                             ImVec2( sx1 - 3.0f, y0 + 3.0f ),
                             IM_COL32( 244, 120, 110, 255 ), 2.0f );

            if ( overSnap && ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
                KiwiGrid_SetSnapEnabled( !on );
        }

        return over || overSnap;
    }

    // ROUND R: the type-in itself.  A SEPARATE ImGui WINDOW rather than ImDrawList
    // output, because this is the one piece of the cluster that genuinely needs a
    // real ImGui ITEM (an InputText owns focus, a caret and the text-input flag) —
    // the rest of kiwi_viewcube.h's "no ImGui item" contract is unchanged and still
    // holds for the cube, the projection pill and the pill's own three zones.
    // Nested Begin() inside the viewport window is the supported ImGui shape: the
    // window stack is a stack, and the popup is a top-level window of its own.
    //
    // ── KIWI-UX (ROUND Y, ITEM 6): IT CLAIMS ITS OWN RECT, NOT THE VIEWPORT ──
    // USER REPORT: "Clicks are still ignored sometimes."  This returned true for
    // as long as the popup was OPEN, and the caller ORs that into the camera
    // image's hover claim — so while the grid type-in was up, EVERY click anywhere
    // in the 3D view was discarded, not just the ones aimed at the field.  The
    // popup is a real ImGui window and ImGui already blocks the clicks that land
    // ON it (the image's hover test still respects window overlap, which round Y's
    // AllowWhenBlockedByActiveItem flag deliberately does not touch), so the claim
    // only needs to cover the popup's own rectangle.  Return true when the cursor
    // is actually over it; the rest of the viewport keeps working.
    bool DrawGridEditPopup()
    {
        if ( !s_gridEdit )
            return false;
        bool overPopup = false;

        ImGui::SetNextWindowPos( ImVec2( s_gridEditX, s_gridEditY ), ImGuiCond_Always );
        // Width fits the longest string it draws (the hint line); height 0 means
        // "auto-fit", which is what NoResize wants — AlwaysAutoResize would ignore
        // the width and let the field shrink to the value in it.
        ImGui::SetNextWindowSize( ImVec2( KVC_GRID_EDIT_W, 0.0f ), ImGuiCond_Always );
        if ( s_gridEditFocus )
            ImGui::SetNextWindowFocus();     // on OPEN only, exactly as the palette does

        bool keepOpen = true;
        if ( !ImGui::Begin( "Grid spacing##kiwigridedit", &keepOpen,
                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking |
                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize ) )
        {
            ImGui::End();
            return false;                    // collapsed: nothing of it is under the cursor
        }
        // ROUND Y, ITEM 6: the claim is this WINDOW's rect only.  ChildWindows so
        // the field inside it counts as the popup.
        overPopup = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows
                                          | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem );

        if ( s_gridEditFocus )
        {
            ImGui::SetKeyboardFocusHere();
            s_gridEditFocus = false;
        }
        ImGui::SetNextItemWidth( -1.0f );
        const bool entered = ImGui::InputText( "##kiwigridvalue", s_gridEditBuf,
                                               sizeof( s_gridEditBuf ),
                                               ImGuiInputTextFlags_EnterReturnsTrue
                                             | ImGuiInputTextFlags_AutoSelectAll );
        // KIWI-UX (ROUND AQ, ITEM 4): CharsDecimal is gone — this box takes the
        // same expression grammar the in-viewport numeric fields take now, and
        // that filter would have blocked every operator and unit letter.
        ImGui::TextDisabled( "inches — math and units ok (1/8, 6in, 1ft)  ·  "
                             "Enter applies  ·  Esc cancels" );

        // Esc BEFORE the apply arm: a cancelled edit must not also be committed.
        if ( ImGui::IsKeyPressed( ImGuiKey_Escape, false ) )
            keepOpen = false;
        else if ( entered )
        {
            // ANY positive float (kiwi_command.h KiwiCmd_SetGridSpacing) — the
            // ladder is for the ARROWS, not for what the user typed.  A value that
            // does not parse leaves the spacing alone and the field open, so a typo
            // costs a keystroke rather than a grid.
            // KIWI-UX (ROUND AQ, ITEM 4): ONE parser.  This was the tree's second
            // atof on user-typed text (the other was the numeric layer's, now the
            // expression evaluator); routing it through the same entry point is
            // what makes "it is one parser" true rather than aspirational.  A
            // value that does not evaluate leaves the spacing alone and the field
            // open, exactly as a value that did not parse used to.
            float v = 0.0f;
            if ( KiwiNum_EvalDisplay( s_gridEditBuf, &v ) && KiwiCmd_SetGridSpacing( v ) )
                keepOpen = false;
            else
                s_gridEditFocus = true;      // re-focus and let them try again
        }

        ImGui::End();
        if ( !keepOpen )
        {
            s_gridEdit      = false;
            s_gridEditFocus = false;
            return false;                    // closed this frame: claim nothing
        }
        return overPopup;                    // ROUND Y, ITEM 6 — its own rect only
    }
}

// ─── toggle ──────────────────────────────────────────────────────────────────
bool KiwiViewCube_Show()
{
    if ( s_show < 0 )
        s_show = Radiant_ProfileGetInt( "KiwiUX", "ShowViewCube", 1 ) ? 1 : 0;
    return s_show != 0;
}

void KiwiViewCube_SetShow( bool on )
{
    const int v = on ? 1 : 0;
    if ( s_show == v )
        return;
    s_show = v;
    Radiant_ProfileSetInt( "KiwiUX", "ShowViewCube", v );
}

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND Y, ITEM 4 — "IS THE CAMERA ON AN AXIS VIEW?", PUBLISHED
// ═════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: "the line tool is still way out of whack.  IT doesn't
// respect the camera angle.  When snapping to top or bottom it is IMPOSSIBLE for
// me to represent a Z direction.  Take that into account and apply it on the
// other views too (see how plasticity does it)."
//
// PLASTICITY'S ANSWER IS A LATCH.  Navigating to an axis view enters "ortho
// mode": `Viewport._navigate` sets the camera AND the construction plane in one
// act — `this.constructionPlane = to.cplane; this.transitionToOrthoMode(...)`
// (plasticity/src/components/viewport/Viewport.tsx:558-566) — with the six
// presets DEFINED as cplane targets rather than as camera targets
// (`'viewport:navigate:top': () => this._navigate(this.cplanes
// .constructionPlaneForOrientation(Orientation.posZ))`, Viewport.tsx:150-156, and
// the orientation->plane table `case Orientation.posZ: return PlaneDatabase.XY`
// at ConstructionPlaneGenerator.ts:69-78).  A view-cube face click enters the
// SAME funnel (`viewport.navigate(object.userData.type)`,
// ViewportNavigator.ts:136 -> Viewport.tsx:551-556).  Ortho mode then makes the
// plane authoritative: `else if (this._restriction === undefined && isOrtho)
// return baseConstructionPlane;` (PointPickerModel.ts:61-62) and face snaps are
// filtered out of the candidate set entirely (SnapPickerStrategy.ts:98).  It ends
// on the first orbit that moves the camera —
// `if (Math.abs(dot - 1) > 10e-6) this.transitionFromOrthoMode();`
// (Viewport.tsx:411-417).
//
// KIWI ANSWERS IT WITHOUT THE LATCH, and that is a deliberate simplification
// rather than a shortcut.  Plasticity latches on entry and un-latches on the
// first orbit, which is definitionally "the camera is still exactly on the axis
// it was navigated to" — a fact this editor can read straight off the live camera
// any time it is asked.  A stored flag would be a second copy of that fact, with
// every path that moves the camera (the cube, the swipe, the drag-orbit, the
// keyboard, a map load, a focus) obliged to keep it in step.  So: no state, one
// dot product, and KVC_VIEW_ALIGNED — cos(3 degrees), already the file's own
// "am I on this view" threshold and already wide enough for the +-89 pole clamp.
//
// outAxis is the WORLD AXIS the camera looks along (0=X, 1=Y, 2=Z), i.e. the
// normal of the plane the user is looking at; outSign is which way it looks.
bool KiwiViewCube_ViewAxis( int *outAxis, float *outSign )
{
    camera_s *c = Ed_Camera();
    if ( !c )
        return false;
    CamWnd_BuildMatrix();                        // vpn for THIS frame's angles
    for ( int i = 0; i < 6; ++i )
    {
        if ( Dot3( KVC_VIEWS[i].vpn, c->vpn ) < KVC_VIEW_ALIGNED )
            continue;
        int   axis = 2;
        float sign = 1.0f;
        for ( int k = 0; k < 3; ++k )
            if ( KVC_VIEWS[i].vpn[k] != 0.0f ) { axis = k; sign = KVC_VIEWS[i].vpn[k]; }
        if ( outAxis ) *outAxis = axis;
        if ( outSign ) *outSign = sign;
        return true;
    }
    return false;
}

const char *KiwiViewCube_ViewAxisName()
{
    camera_s *c = Ed_Camera();
    if ( !c )
        return 0;
    CamWnd_BuildMatrix();
    for ( int i = 0; i < 6; ++i )
        if ( Dot3( KVC_VIEWS[i].vpn, c->vpn ) >= KVC_VIEW_ALIGNED )
            return KVC_VIEWS[i].name;
    return 0;
}

// ─── ROUND P: Alt+MMB — step the six face views (see kiwi_viewcube.h) ────────
void KiwiViewCube_StepAxisView()
{
    camera_s *c = Ed_Camera();
    CamWnd_BuildMatrix();                        // vpn for THIS frame's angles

    // Which face view are we nearest?  One dot per row; the largest wins.
    int   best     = 0;
    float bestDot  = -2.0f;
    for ( int i = 0; i < 6; ++i )
    {
        const float d = Dot3( KVC_VIEWS[i].vpn, c->vpn );
        if ( d > bestDot )
        {
            bestDot = d;
            best    = i;
        }
    }

    // Already sitting on it -> ADVANCE; otherwise snap to the nearest, so the first
    // press of the chord never throws the view to an unrelated side.
    if ( bestDot >= KVC_VIEW_ALIGNED )
        best = ( best + 1 ) % 6;

    LookAlongDirection( c, KVC_VIEWS[best].vpn );
    Sys_Printf( "View: %s.\n", KVC_VIEWS[best].name );
}

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND U — THE VERTICAL SWIPE WRAPS.  THE POLES ARE NOT TERMINAL.
// ═════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: "fix the viewport swiping (alt - mmb) so you can wrap
// around however you like."  The console was printing
// "View: already bottom - swipe the other way to come back" — round S's vertical
// mapping was a three-rung LADDER (side -> pole, pole -> back to the side, other
// pole = refuse) and a ladder has ends.
//
// THE CYCLE IT IS NOW, walked indefinitely in either direction:
//
//     ref side  --down-->  TOP  --down-->  opposite side (yaw+180)
//               --down-->  BOTTOM  --down-->  ref side  --down--> ...
//
// and UP is that read backwards, so a swipe and its opposite always undo each
// other.  Concretely from front: front -> top -> back -> bottom -> front.  The
// HORIZONTAL ring is untouched (four side views as a compass; at a pole a
// horizontal swipe still spins the pole view in place).
//
// ── WHY THIS NEEDS TWO INTS OF STATE, and why it cannot be derived ──────────
// The missing quantity is ROLL.  "Front, having come over the top" and "back,
// reached by clicking the cube" are the SAME camera in this editor — Radiant's
// camera has pitch and yaw and no roll at all, and LookAlongDirection clamps the
// pitch to +-89 — so from the camera alone there is no way to tell whether the
// next DOWN should continue over the pole or go back the way it came.  Round S's
// refusal at the poles was that ambiguity showing up as a dead end.
//
// So the phase is REMEMBERED and then VALIDATED against the live camera on every
// swipe (ValidateVertPhase below).  The memory is only ever a tie-breaker: any
// disagreement — a cube click, an orbit, a horizontal swipe, a pole spin — resyncs
// it from what the camera actually is, so it can never send the view somewhere the
// user has no reason to expect.  Nothing else in the file reads it.
namespace
{
    // The side view (0..3) this great circle is anchored at, and where on it we
    // are: 0 = ref side, 1 = top, 2 = the opposite side, 3 = bottom.
    int s_vertRef  = -1;                  // -1 = no memory yet
    int s_vertStep = 0;

    // The view id the phase says we should be looking along.
    int VertExpected( int ref, int step )
    {
        switch ( step )
        {
        case 1:  return 4;                       // top
        case 2:  return ( ref + 2 ) & 3;         // the opposite side
        case 3:  return 5;                       // bottom
        default: return ref;
        }
    }

    // Keep the memory only when it agrees with the live camera; otherwise rebuild
    // it from the camera.  At a POLE the yaw is what survived the hop (see
    // LookAlongDirection's `degenerate` arm), so it has to be one of the circle's
    // own two side views or the memory is about a different circle entirely.
    void ValidateVertPhase( int base, int yawView )
    {
        const bool onPole = ( base == 4 || base == 5 );
        if ( s_vertRef >= 0 && s_vertRef <= 3
          && VertExpected( s_vertRef, s_vertStep ) == base
          && ( !onPole || yawView == s_vertRef || yawView == ( ( s_vertRef + 2 ) & 3 ) ) )
            return;                              // the memory still describes reality

        if ( base <= 3 )
        {
            s_vertRef  = base;
            s_vertStep = 0;
        }
        else
        {
            s_vertRef  = yawView;
            s_vertStep = ( base == 4 ) ? 1 : 3;
        }
    }
}

// ─── ROUND S: Alt+MMB SWIPE — a 90-degree step in a screen direction ─────────
// The mapping and its derivation are in kiwi_viewcube.h; this is the mechanical
// half.  Everything goes through LookAlongDirection / KiwiCam_LookAlong, the same
// pair the cube's own clicks use, so the yaw lattice, the +-89 pole clamp and the
// round-N spin reset are shared code rather than a second copy.
void KiwiViewCube_SwipeAxisView( int dir )
{
    camera_s *c = Ed_Camera();
    CamWnd_BuildMatrix();                        // vpn for THIS frame's angles

    // Where we are STARTING from: the face view nearest the current direction, so
    // a swipe out of an arbitrarily orbited view lands somewhere predictable.
    int   base    = 0;
    float bestDot = -2.0f;
    for ( int i = 0; i < 6; ++i )
    {
        const float d = Dot3( KVC_VIEWS[i].vpn, c->vpn );
        if ( d > bestDot ) { bestDot = d; base = i; }
    }
    const bool onPole = ( base == 4 || base == 5 );

    // The four horizontal face views, walked as a compass, and the yaw each one
    // sits at (LookAlongDirection derives yaw = atan2(vpn.y, vpn.x)):
    //     left(3) = 0    front(0) = 90    right(1) = 180    back(2) = -90/270
    // RING_RIGHT is the yaw -90 order; RING_LEFT is its reverse.
    //   RING_RIGHT: front(0)->left(3)->back(2)->right(1)->front(0)
    //   RING_LEFT : front(0)->right(1)->back(2)->left(3)->front(0)
    static const int RING_RIGHT[4] = { 3, 0, 1, 2 };   // indexed BY view id 0..3
    static const int RING_LEFT [4] = { 1, 2, 3, 0 };
    // Snapped yaw -> the horizontal view id, for the "come back off a pole" case.
    const float yawSnapped = SnapAngle( c->angles[1], 90.0f );
    int         yawView    = 3;                        // yaw 0 == left
    if      ( yawSnapped >   45.0f && yawSnapped <=  135.0f ) yawView = 0;   // 90  front
    else if ( yawSnapped >  135.0f || yawSnapped <= -135.0f ) yawView = 1;   // 180 right
    else if ( yawSnapped <  -45.0f )                          yawView = 2;   // -90 back

    int target = -1;
    switch ( dir )
    {
    case 0:                                            // LEFT  — yaw +90
    case 1:                                            // RIGHT — yaw -90
        if ( onPole )
        {
            // Spin the pole view in place: keep the pitch, step the yaw.  There is
            // no face-view row for "top, rotated 90", so this is the one arm that
            // goes to KiwiCam_LookAlong directly.
            const float step = ( dir == 1 ) ? -90.0f : 90.0f;
            KiwiCam_LookAlong( ( base == 4 ) ? -89.0f : 89.0f,
                               SnapAngle( yawSnapped + step, 90.0f ) );
            Sys_Printf( "View: %s (spun %s).\n", KVC_VIEWS[base].name,
                        ( dir == 1 ) ? "right" : "left" );
            return;
        }
        target = ( dir == 1 ) ? RING_RIGHT[base] : RING_LEFT[base];
        break;

    case 2:                                            // UP   — pitch +90
    default:                                           // DOWN — pitch -90
        // ── ROUND U: ONE STEP AROUND THE GREAT CIRCLE, EITHER WAY ───────
        // The whole argument (and the cycle it walks) is on ValidateVertPhase
        // above.  There is no refusal arm any more: every step of a 4-cycle has
        // a next step, in both directions, forever.
        ValidateVertPhase( base, yawView );
        s_vertStep = ( s_vertStep + ( ( dir == 3 ) ? 1 : 3 ) ) & 3;
        target     = VertExpected( s_vertRef, s_vertStep );
        break;
    }

    // A HORIZONTAL step lands on a different circle, so the vertical memory has to
    // follow it — otherwise the next vertical swipe would validate against a ref
    // that is now 90 degrees away and resync anyway, one step late.  (The pole-spin
    // arm returns above and never reaches here, and it is covered by the pole-yaw
    // clause inside ValidateVertPhase.)
    if ( dir == 0 || dir == 1 )
    {
        s_vertRef  = target;
        s_vertStep = 0;
    }

    // KIWI-UX (CLEANUP, C-39): `target` is provably 0..5, so this guard cannot fire
    // — but it INDEXES A TABLE, so it stays rather than becoming an out-of-bounds
    // read the day a caller regresses.  What changes is that it stops being silent:
    // a bail here would have meant "the view snap did nothing, for no stated
    // reason".  Same early-out, now audible.
    if ( target < 0 || target > 5 )
    {
        Sys_Printf( "View cube: refused an out-of-range view index (%i).\n", target );
        return;
    }

    LookAlongDirection( c, KVC_VIEWS[target].vpn );
    Sys_Printf( "View: %s.\n", KVC_VIEWS[target].name );
}

// ─── draw + resolve ──────────────────────────────────────────────────────────
bool KiwiViewCube_Draw( float imgMinX, float imgMinY, float imgW, float imgH )
{
    if ( imgW < KVC_BOX * 1.5f || imgH < KVC_BOX * 1.5f )
        return false;                            // too small a viewport to be useful

    // ROUND M: the projection pill is drawn FIRST and OUTSIDE the cube's own
    // toggle — hiding the orientation cube is a decluttering choice and must not
    // take the projection control away with it.  Its hover is OR-ed into the
    // return so the caller's claim covers both widgets.
    // KIWI-UX (ROUND BM, ITEMS 2 + 1b): the strip above the cube — the zoom meter
    // and the Section Analysis button.  FIRST, on the same terms as the pills: both
    // are drawn OUTSIDE KiwiViewCube_Show() because hiding the orientation cube is a
    // decluttering choice and must not take a control away with it.
    const bool zoomHot = DrawZoomBar( imgMinX, imgMinY, imgW, imgH );
    const bool secHot  = DrawSectionButton( imgMinX, imgMinY, imgW, imgH );
    const bool projHot = DrawProjButton( imgMinX, imgMinY, imgW, imgH );
    // ROUND N: the grid readout, on the same terms — the user asked for it "in the
    // top right somewhere", and hiding the orientation cube must not take a grid
    // control away with it either.
    const bool gridHot = DrawGridButton( imgMinX, imgMinY, imgW, imgH );
    // ROUND R: the grid pill's type-in popup.  Drawn right after the pill that owns
    // it and OR-ed into the hover claim on the same terms — while it is open the
    // image must not also start a marquee under the cursor.
    const bool gridEditHot = DrawGridEditPopup();

    if ( !KiwiViewCube_Show() )
        return zoomHot || secHot || projHot || gridHot || gridEditHot;

    camera_s *c = Ed_Camera();
    CamWnd_BuildMatrix();                        // vpn/vright/vup for THIS frame's angles

    const float cx = imgMinX + imgW - KVC_MARGIN - KVC_BOX * 0.5f;
    const float cy = KVC_ClusterTop( imgMinY, imgH ) + KVC_BOX * 0.5f;   // ROUND BM: the strip

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool   winHovered = ImGui::IsWindowHovered( ImGuiHoveredFlags_ChildWindows );
    const float  mdx = mouse.x - cx, mdy = mouse.y - cy;
    const bool   overWidget = winHovered
                           && ( mdx * mdx + mdy * mdy ) <= ( KVC_BOX * 0.5f ) * ( KVC_BOX * 0.5f );

    ImDrawList *dl = ImGui::GetWindowDrawList();

    // Backdrop: a soft disc so the widget reads over bright geometry.
    dl->AddCircleFilled( ImVec2( cx, cy ), KVC_BOX * 0.5f, IM_COL32( 18, 18, 22, 140 ), 32 );
    dl->AddCircle( ImVec2( cx, cy ), KVC_BOX * 0.5f, IM_COL32( 80, 84, 96, 120 ), 32, 1.0f );

    // ── project the eight corners once ──────────────────────────────────────
    ImVec2 pt[8];
    float  cdepth[8];
    for ( int i = 0; i < 8; ++i )
    {
        float v[3];
        CornerVec( i, v );
        pt[i]     = ImVec2( cx + Dot3( v, c->vright ) * KVC_HALF,
                            cy - Dot3( v, c->vup    ) * KVC_HALF );
        cdepth[i] = Dot3( v, c->vpn );
    }

    // ── order the six faces back-to-front ───────────────────────────────────
    struct laidFace_t { float depth; int idx; };
    laidFace_t order[6];
    for ( int i = 0; i < 6; ++i )
    {
        float n[3] = { 0.0f, 0.0f, 0.0f };
        n[ KVC_FACES[i].axis ] = KVC_FACES[i].sign;
        order[i].depth = Dot3( n, c->vpn );
        order[i].idx   = i;
    }
    for ( int i = 0; i < 6; ++i )                // 6 elements: insertion sort is the honest tool
        for ( int j = i + 1; j < 6; ++j )
            if ( order[j].depth > order[i].depth )
            {
                const laidFace_t t = order[i]; order[i] = order[j]; order[j] = t;
            }

    // ── the hit test, corners BEFORE faces ──────────────────────────────────
    // A corner is a small target sitting on top of three faces, so it must win
    // where they compete; and it is only clickable when it is on the viewer's
    // side of the cube (depth < 0), so a click can never snap to a view of the
    // back of the cube the user cannot see.
    int hotCorner = -1;
    int hotFace   = -1;
    if ( overWidget )
    {
        float best = KVC_CORNER;
        for ( int i = 0; i < 8; ++i )
        {
            if ( cdepth[i] >= 0.0f )
                continue;
            const float dx = mouse.x - pt[i].x, dy = mouse.y - pt[i].y;
            const float d  = sqrtf( dx * dx + dy * dy );
            if ( d <= best )
            {
                best      = d;
                hotCorner = i;
            }
        }
        if ( hotCorner < 0 )
        {
            // Front-to-back over the painter order, so the nearest face wins.
            for ( int k = 5; k >= 0 && hotFace < 0; --k )
            {
                if ( order[k].depth >= 0.0f )
                    continue;                    // back face: not clickable
                int ci[4];
                FaceCorners( KVC_FACES[ order[k].idx ], ci );
                const ImVec2 quad[4] = { pt[ci[0]], pt[ci[1]], pt[ci[2]], pt[ci[3]] };
                if ( PointInQuad( quad, mouse.x, mouse.y ) )
                    hotFace = order[k].idx;
            }
        }
    }

    // ── SHAKEOUT I: the three coloured axis stubs (Blender's, and the only
    //    colour left on the widget).  Projected through the SAME orthographic
    //    relation as the corners, so they cannot drift out of register with the
    //    cube: v = axis unit vector, screen = centre + (dot(v,vright), -dot(v,vup))
    //    * KVC_HALF * KVC_STUB, depth = dot(v,vpn).  A stub BEHIND the cube is
    //    drawn first (so the faces occlude it) and one in FRONT after the faces.
    ImVec2 stubPt[3];
    float  stubDepth[3];
    for ( int a = 0; a < 3; ++a )
    {
        float v[3] = { 0.0f, 0.0f, 0.0f };
        v[a] = 1.0f;
        stubPt[a]    = ImVec2( cx + Dot3( v, c->vright ) * KVC_HALF * KVC_STUB,
                               cy - Dot3( v, c->vup    ) * KVC_HALF * KVC_STUB );
        stubDepth[a] = Dot3( v, c->vpn );
    }
    for ( int a = 0; a < 3; ++a )
        if ( stubDepth[a] >= 0.0f )              // behind the cube
            dl->AddLine( ImVec2( cx, cy ), stubPt[a], AxisCol( a, 0.55f, 150 ), 2.0f );

    // ── draw the six faces, back to front ───────────────────────────────────
    for ( int k = 0; k < 6; ++k )
    {
        const face_t2 &f     = KVC_FACES[ order[k].idx ];
        const bool     front = ( order[k].depth < 0.0f );
        const bool     isHot = ( hotFace == order[k].idx );

        int ci[4];
        FaceCorners( f, ci );
        ImVec2 quad[4] = { pt[ci[0]], pt[ci[1]], pt[ci[2]], pt[ci[3]] };

        // SHAKEOUT I: GREY, at three levels — a back face is dim and a front face
        // light so the solid still reads on an axis-aligned view where only one
        // face is visible, and the hovered one is lighter again.  No hue anywhere
        // on the body (see THE GREY CUBE above).
        const ImU32 fill = Grey( isHot ? KVC_GREY_HOVER
                                       : ( front ? KVC_GREY_FRONT : KVC_GREY_BACK ),
                                 245 );
        dl->AddConvexPolyFilled( quad, 4, fill );

        // Edges are a DARKER grey than the fill they bound — the opposite of the
        // old scheme, which lit them.  On a light body a dark edge is what draws
        // the silhouette; a light one just fuzzes it.
        const ImU32 edge = front ? Grey( 0.34f, 190 ) : Grey( 0.20f, 130 );
        for ( int e = 0; e < 4; ++e )
            dl->AddLine( quad[e], quad[( e + 1 ) & 3], edge, 1.0f );

        if ( !front )
            continue;                            // labels on the visible side only

        const ImVec2 mid( ( quad[0].x + quad[2].x ) * 0.5f,
                          ( quad[0].y + quad[2].y ) * 0.5f );
        const ImVec2 ts = ImGui::CalcTextSize( f.label );
        // A very edge-on face has no room for its label; skip it rather than
        // spill the text over its neighbours.
        const float extent = sqrtf( ( quad[0].x - quad[2].x ) * ( quad[0].x - quad[2].x )
                                  + ( quad[0].y - quad[2].y ) * ( quad[0].y - quad[2].y ) );
        if ( extent < ts.x + 4.0f )
            continue;
        // DARK text on the light fill — the old white-on-saturated pairing does not
        // survive the grey body at all.
        dl->AddText( ImVec2( mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f ),
                     Grey( isHot ? 0.10f : 0.16f, 250 ), f.label );
    }

    // The stubs that are IN FRONT, plus their letters.  Same colours as the world
    // axes (§18) so the widget and the viewport agree on what red means.
    for ( int a = 0; a < 3; ++a )
    {
        if ( stubDepth[a] >= 0.0f )
            continue;
        dl->AddLine( ImVec2( cx, cy ), stubPt[a], AxisCol( a, 1.0f, 235 ), 2.0f );
        const ImVec2 ts = ImGui::CalcTextSize( KVC_STUB_TEXT[a] );
        dl->AddText( ImVec2( stubPt[a].x - ts.x * 0.5f, stubPt[a].y - ts.y * 0.5f ),
                     AxisCol( a, 1.0f, 245 ), KVC_STUB_TEXT[a] );
    }

    // ── the hovered corner marker ───────────────────────────────────────────
    // The corner "balls" are INVISIBLE until hovered — unchanged from shakeout D
    // and re-stated because the grey body makes it look deliberate rather than
    // missing: eight always-on dots on a neutral cube is clutter, and the corner
    // targets are discoverable by hovering them, which is what the marker is for.
    if ( hotCorner >= 0 )
        dl->AddCircleFilled( pt[hotCorner], 4.0f, IM_COL32( 255, 226, 110, 255 ), 12 );

    // (Shakeout E removed the dead "persp" READOUT that used to sit under the cube,
    //  on the grounds that "an ortho mode, if it ever ships, gets a real control,
    //  not a label."  ROUND M shipped it: DrawProjButton above is that control.)

    // ── the click ───────────────────────────────────────────────────────────
    if ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) )
    {
        if ( hotCorner >= 0 )
        {
            // The isometric views the balls could not offer: stand OFF the corner
            // and look back, which the general derivation turns into the 45-degree
            // yaw / 35.26-degree pitch pair on its own (atan2(±1,±1) = ±45 or
            // ±135; asin(∓1/sqrt3) = ∓35.26).
            float v[3];
            CornerVec( hotCorner, v );
            const float back[3] = { -v[0], -v[1], -v[2] };
            LookAlongDirection( c, back );
        }
        else if ( hotFace >= 0 )
        {
            const face_t2 &f = KVC_FACES[hotFace];
            float back[3] = { 0.0f, 0.0f, 0.0f };
            back[f.axis] = -f.sign;              // vpn = -normal
            LookAlongDirection( c, back );
            // ── ROUND Y, ITEM 4: THE CUBE SETS THE PLANE TOO ────────────────
            // This IS Plasticity's view-cube contract, not an addition to it: a
            // face click goes straight into the navigate funnel that assigns the
            // construction plane — `viewport.navigate(object.userData.type)`
            // (plasticity/src/components/viewport/ViewportNavigator.ts:136) ->
            // `this._navigate(this.cplanes.constructionPlaneForOrientation(to))`
            // (Viewport.tsx:551-556) -> `this.constructionPlane = to.cplane`
            // (Viewport.tsx:558-566), with the orientation->plane table at
            // ConstructionPlaneGenerator.ts:69-78.
            //
            // KiwiCon_AutoPlaneForTool's new rung 0 would reach the same answer at
            // the next tool start, so this is not what makes the fix work — it is
            // what makes it VISIBLE.  Setting the plane here means the console line
            // and the plane readout change AT THE CLICK, so "which plane am I on"
            // is answered before a tool is started rather than after.
            //
            // SAFE MID-GESTURE: a live drawing tool captured its plane in Begin()
            // (kiwi_construct.cpp `m_plane = KiwiCon_ActivePlane()`), so changing
            // the ACTIVE plane cannot move points already placed.
            {
                // kiwi_construct.h:606 (definition kiwi_construct.cpp:1158) — axis
                // 2 = XY, 1 = XZ, 0 = YZ, keeping the plane's current position
                // along the new normal.  Declared locally rather than by including
                // kiwi_construct.h: this file's "no ImGui item / no cross-feature
                // coupling" contract keeps its include list to what it draws.
                extern void KiwiCon_SetPlaneAxis( int axis );
                KiwiCon_SetPlaneAxis( f.axis );
            }
        }
    }

    // ROUND N: the two pills claim the image too — a click on either must never
    // also start a marquee behind them.  ROUND R adds the grid pill's type-in popup
    // on exactly the same terms.
    return overWidget || zoomHot || secHot || projHot || gridHot || gridEditHot;
}

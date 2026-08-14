#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_gizmo.cpp — RADIANT_UX_DESIGN §14: the translate gizmo (v1) and the
// shakeout-D rotate rings.  See kiwi_gizmo.h for the ImGuizmo departure, the
// one-command rule, the shakeout-D visibility inversion (gizmos are MODAL
// CHROME now) and the drag-vs-click difference.
//
// NEW code over the ported cores.  It mutates NO geometry: every drag is the
// ported-core-backed KiwiMoveCommand / KiwiRotateCommand, aimed from here and
// driven by the framework.
//
// ── SCREEN-CONSTANT SIZING ──────────────────────────────────────────────────
// Every length is a PIXEL count converted through KiwiCam_WorldPerPixel at the
// anchor, so the gizmo is the same size on screen at any distance — the only
// sizing rule that survives a dolly.  The hit test then works the other way
// round, projecting the same world points back with Pick_WorldToImage (the exact
// inverse of CameraCalcRayDir) and comparing in pixels, so what the user sees and
// what the test measures can never disagree.
//
// ── THE ROTATE SIGN, DERIVED (not guessed) ──────────────────────────────────
// A ring's swept angle is measured right-hand-positive about its own axis: for
// axis a the in-plane basis is U = world axis (a+1)%3, V = world axis (a+2)%3,
// and the angle is atan2( rel·V, rel·U ), which increases in the right-handed
// sense about +a.
//
// The ported rotate does NOT use that sign.  Select_RotateAxis (select.cpp:2337)
// builds its 3x3 from `Ed_SinCos( -deg, &s, &c )`, and the matrix is applied by
// Select_ApplyMatrix (brush.cpp:8201) through OrientationPosToWorldPos
// (draw.cpp:24), whose form is out[j] = SUM_i axis[i][j] * pos[i] + origin[j] —
// i.e. the ROW-vector convention v'[j] = SUM_i M[i][j] v[i].  Substituting the
// case-2 (Z) block, M[0][0]=c, M[1][0]=-s, M[0][1]=s, M[1][1]=c with s=-sin(deg):
//     x' = x cos(deg) + y sin(deg)
//     y' = -x sin(deg) + y cos(deg)
// which is a right-handed rotation by MINUS deg.  Cases 0 and 1 have the same
// shape.  So a positive `deg` handed to Select_RotateAxis turns the geometry the
// NEGATIVE way round the axis, and the ring must feed  deg = -sweep  for the
// geometry to follow the cursor.  That one negation is the whole mapping.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
// KIWI-UX (ROUND AK, ITEM 4) — the FILL path's types.  Same three headers, in the
// same order, that every other kiwi fill pulls in (kiwi_region.cpp:73-75,
// kiwi_hover.cpp:54, kiwi_extrude.cpp:21).
#include <gfx_d3d/r_gfx.h>          // GfxColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT,
                                    // R_AddCmdSetMaterialColor

#include "kiwi_gizmo.h"
#include "kiwi_camera.h"
#include "kiwi_command.h"
#include "kiwi_lines.h"
#include "kiwi_lollipop.h"      // ROUND K — the gizmo stands down for it
#include "kiwi_pick.h"
#include "kiwi_selection.h"
#include "kiwi_transform.h"
#include "kiwi_ux.h"
#include "radiant_registry.h"

#include <math.h>

// ── ported entry points (verified against their definitions) ────────────────
extern camera_s *Ed_Camera();          // camwnd.cpp
extern void      CamWnd_BuildMatrix(); // camwnd.cpp 0x403470
extern int       g_nUpdateBits;        // 0x25D5A74 (mainfrm.cpp)
// KIWI-UX (ROUND AK, ITEM 4): the FILL path.  Copied verbatim from
// kiwi_region.cpp:95-101, which is the same triangle draw every kiwi fill uses
// (kiwi_extrude.cpp:71, kiwi_loft.cpp:55, kiwi_split.cpp:69, kiwi_hover.cpp:78 all
// carry this identical block).  R_AddCmdSetMaterialColor comes from
// r_rendercmds.h, included above and declared __cdecl there.
extern char  Byte4PackPixelColor( float *from, GfxColor *out );          // 0x402ac0
extern void  __cdecl R_AddRenderCmdDrawTris(
                 Material *material, MaterialTechniqueType techType, short indexCount,
                 const uint16_t *indices, short vertexCount,
                 const float ( *xyzw )[4], const float ( *normal )[3], float *color,
                 const float ( *st )[2] );                               // 0x4fd1c0
// KIWI-UX (ROUND AL, ITEM 2): the ALWAYS-ON-TOP mechanism is R_AddCmdClearScreen,
// which needs no extern here — it is declared __cdecl in r_rendercmds.h:884,
// included above, and its definition is r_rendercmds.cpp:1491.  The two existing
// editor uses are camwnd.cpp:2889 (the binary's own selected-outline prelude,
// 0x4084d2) and camwnd.cpp:3000 (the port's terrain-ring re-clear).

namespace
{
    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AJ, ITEM 4) — THE PLASTICITY RESTYLE
    // ═══════════════════════════════════════════════════════════════════════
    // USER DIRECTIVE, verbatim: "I want the gizmos to look less ugly.  Make it
    // look like plasticity's gizmos."
    //
    // THIS IS A VISUAL PASS.  Every handle still means what it meant, the resolution
    // order (CENTRE -> PLANE -> AXIS) is unchanged, and no handle was added or
    // removed.  What changed is the SHAPES and the PALETTE, and the two hit zones
    // that had to follow their visuals (the plane handle and the centre; see
    // HitTest, where each is written down).
    //
    // PLASTICITY'S MOVE GIZMO, MEASURED.  Its geometry is built at unit scale and
    // multiplied by a per-sub-gizmo `relativeScale`, so everything below is
    // expressed as a fraction of the SHAFT LENGTH and then multiplied by this
    // file's own KGZ_AXIS_PIX:
    //   * the shaft is a Line2 of unit length (MiniGizmos.ts:348-349) drawn with a
    //     LineMaterial at linewidth 2 px, 3 px hovered (GizmoMaterials.ts:33,37);
    //   * the head is a cone with base radius 0.1 and height 0.2
    //     (MiniGizmos.ts:346-347: `CylinderGeometry(0, 0.1, 0.2, 12)`), i.e.
    //     0.2 and 0.1 of the shaft -> 14 px and 7 px here;
    //   * the plane handle is a FILLED SQUARE 0.2 x 0.2 centred at (0.5, 0.5)
    //     (MiniGizmos.ts:382-398) -> a 14 px square centred 35 px out on each axis;
    //   * the origin carries a WHITE SCREEN-SPACE CIRCLE — a 64-segment line loop,
    //     radius 1 at relativeScale 0.25 against the axes' 0.8 (MoveGizmo.ts:40-41,
    //     MiniGizmos.ts:62-63), billboarded to the camera
    //     (MiniGizmos.ts:106-116) — and it is the VIEW-PLANE MOVE.  That handle
    //     already exists here as KGZ_CENTER (a free move on the latched
    //     view-normal plane), so this is a restyle of it and NOT a new handle.
    // AND ITS ROTATE GIZMO: three axis-coloured FULL circles at relativeScale 0.7
    // plus a WHITE one at 0.8 (RotateGizmo.ts:40-41), the white one billboarded and
    // the three axis ones NOT (RotateGizmo.ts:129) — with the back halves hidden by
    // a giant camera-facing depth-only plane rendered first (RotateGizmo.ts:132-159),
    // which is what makes the three rings read as ONE SPHERE.
    //
    // ── pixel geometry ──────────────────────────────────────────────────────
    const float KGZ_AXIS_PIX    = 70.0f;   // arrow shaft length (unchanged)
    const float KGZ_HEAD_PIX    = 14.0f;   // arrowhead length  = 0.20 * shaft
    const float KGZ_HEAD_WIDE   = 7.0f;    // arrowhead half-width = 0.10 * shaft
    const float KGZ_PLANE_PIX   = 35.0f;   // plane-handle SQUARE centre = 0.50 * shaft
    const float KGZ_PLANE_HALF  = 7.0f;    // …and its half-size      = 0.10 * shaft
    const float KGZ_CENTER_PIX  = 10.0f;   // the white origin RING's radius
    const float KGZ_CENTER_PICK = 12.0f;   // …and its hit radius (was 8; see HitTest)
    const int   KGZ_CENTER_SEGS = 24;      // enough to read as a circle at 10 px
    const float KGZ_PICK_PIX    = 8.0f;    // hit tolerance
    const float KGZ_AXIS_MIN_T  = 0.28f;   // axis hits below this t belong to the
                                           // centre / plane handles, not the shaft

    // ── shakeout D: the rotate rings ────────────────────────────────────────
    const float KGZ_RING_PIX      = 55.0f; // ring radius, screen-constant
    const int   KGZ_RING_SEGMENTS = 48;    // one ring's segment count (see the budget)
    const float KGZ_RING_PICK_PIX = 8.0f;  // ring hit tolerance
    // ROUND AJ, ITEM 4: the white VIEW ring, at Plasticity's own 0.8 / 0.7 ratio
    // (RotateGizmo.ts:40-41) — 14% outside the axis rings, which is what closes the
    // three of them into a sphere silhouette.
    const float KGZ_VIEW_RING_MUL = 1.15f;
    const int   KGZ_VIEW_RING_SEG = 40;
    const float KGZ_DEG           = 57.29577951308232f;   // 180 / pi

    enum
    {
        KGZ_NONE = -1,
        KGZ_AXIS_X = 0, KGZ_AXIS_Y, KGZ_AXIS_Z,      // 0..2  == the axis index
        KGZ_PLANE_X,    KGZ_PLANE_Y, KGZ_PLANE_Z,    // 3..5  == plane NORMAL axis + 3
        KGZ_CENTER,                                   // 6
        // ── SHAKEOUT G: the FACE-NORMAL arrow ────────────────────────────────
        // Present only while Move is pushing faces (KiwiXform_ActivePushDir).  It
        // is the handle a push/pull actually wants: the three world arrows still
        // work and still AXIS-LOCK the push, but the default, unlocked push runs
        // along the driving face's own normal and until now had no handle of its
        // own at all — the user had to grab the CENTRE square, which reads as
        // "free move" everywhere else in the editor.  Now that shakeout G gates the
        // push on a held handle, that gap would have been the difference between
        // "face push/pull is subtle" and "face push/pull is unreachable".
        KGZ_NORMAL                                    // 7
    };
    const float KGZ_NORMAL_COL[3] = { 1.00f, 0.62f, 0.20f };   // amber — not an axis

    // ── ROUND AJ, ITEM 4: PLASTICITY'S AXIS PALETTE ─────────────────────────
    // The hexes are its theme's 600 ramp, read at GizmoMaterials.ts:111-123 and
    // default-theme.js — X #CF1124, Y #199473, Z #2563EB — and the HOVER row is the
    // 400 ramp the same materials swap to (#EF4E4E / #3EBD93 / #60A5FA,
    // MiniGizmos.ts:220-228).  They are noticeably more muted than the near-primary
    // triple that was here, which is the "less ugly" half of the directive: at 2 px
    // a fully saturated primary vibrates against a mid-grey viewport.
    //
    // THIS IS A DELIBERATE, GIZMO-LOCAL DIVERGENCE FROM §18's shared axis language.
    // The world axes (kiwi_grid.cpp) and the constraint accent
    // (kiwi_transform.cpp's KX_AXIS_COL) keep their own brighter triple: those are
    // 1 px lines seen against the whole scene and need the contrast, and they are
    // never adjacent to the gizmo's own handles.  The HUE is the same in all three
    // (red/green/blue for X/Y/Z), which is the part of §18 that carries meaning.
    //
    // ── ROUND AK, ITEM 4: ONE STEP BRIGHTER ─────────────────────────────────
    // USER DIRECTIVE, verbatim: "The new gizmo's are better, but I want them
    // slightly brighter colors and filled in (no hollow shapes)."
    //
    // "One step" is literal: the REST row moves from the 600 ramp to the 500 ramp
    // of the same three Plasticity/Refactoring-UI scales — X #E12D39, Y #27AB83,
    // Z #3B82F6.  The HOVER row stays on the 400 ramp and the HELD yellow is
    // untouched, so the three states are still one ramp step apart each and none
    // of them collapsed into another.  Round AJ's argument for leaving the
    // near-primary triple behind stands: this is a step, not a walk-back.
    const float KGZ_COL[3][3] =
    {
        { 0.882f, 0.176f, 0.224f },        // #E12D39  (was #CF1124, the 600)
        { 0.153f, 0.671f, 0.514f },        // #27AB83  (was #199473)
        { 0.231f, 0.510f, 0.965f },        // #3B82F6  (was #2563EB)
    };
    const float KGZ_COL_HOVER[3][3] =
    {
        { 0.937f, 0.306f, 0.306f },        // #EF4E4E
        { 0.243f, 0.741f, 0.576f },        // #3EBD93
        { 0.376f, 0.647f, 0.980f },        // #60A5FA
    };
    const float KGZ_HOT[3]    = { 1.00f, 0.90f, 0.30f };   // GRABBED (held)
    // Plasticity's `white` = neutral[50] #FAFAFA (GizmoMaterials.ts, default-theme.js:19).
    const float KGZ_CENTRE[3] = { 0.980f, 0.980f, 0.980f };

    int  s_hot     = KGZ_NONE;             // move handle under the cursor (draw state)
    int  s_grabbed = KGZ_NONE;             // move handle being dragged, KGZ_NONE when idle
    int  s_show    = -1;                   // -1 = not read from the profile yet

    // Rotate-ring state.  Axis index 0..2, or -1 for "none".
    int   s_ringHot   = -1;
    int   s_ringGrab  = -1;
    float s_ringPrev  = 0.0f;              // last RAW in-plane angle, degrees
    float s_ringTotal = 0.0f;              // accumulated sweep since the grab, degrees

    inline void  Copy3( const float *a, float *o ) { o[0]=a[0]; o[1]=a[1]; o[2]=a[2]; }
    inline void  Sub3( const float *a, const float *b, float *o )
    { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
    inline float Dot3( const float *a, const float *b )
    { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
    inline void  Mad3( const float *a, const float *d, float s, float *o )
    { o[0]=a[0]+d[0]*s; o[1]=a[1]+d[1]*s; o[2]=a[2]+d[2]*s; }

    // ray ∩ plane(point, normal).  A local copy of the same three lines
    // kiwi_transform.cpp's RayPlane runs: this file cannot reach into that
    // anonymous namespace, and a shared header for one dot product and one
    // divide would be more coupling than it saves.
    bool RayPlane( const ray_t &ray, const float *pt, const float *n, float *out )
    {
        const float den = Dot3( ray.dir, n );
        if ( fabsf( den ) < 1.0e-5f )
            return false;
        float rel[3];
        Sub3( pt, ray.origin, rel );
        const float t = Dot3( rel, n ) / den;
        if ( !( t > 0.0f ) || t > 1.0e6f )
            return false;
        Mad3( ray.origin, ray.dir, t, out );
        return true;
    }

    // ── the frame's translate geometry, in world units ──────────────────────
    struct gizmoGeo_t
    {
        float anchor[3];
        float axisLen;
        float headLen;
        float headWide;
        float planeOff;      // ROUND AJ: the plane SQUARE's centre offset
        float planeHalf;     // ROUND AJ: …and its half-size
        float centreRad;     // ROUND AJ: the white origin RING's radius
        // SHAKEOUT G: the face-normal arrow (see KGZ_NORMAL).
        bool  hasNormal;
        float normal[3];
    };

    // The two shared preconditions: the modern layer is on, this file's own
    // toggle is on, and the camera is real.  CamWnd_BuildMatrix has been run by
    // the caller (every caller does it — the scale below reads camera.vpn).
    bool GizmoUsable()
    {
        if ( !KiwiUX_ModernInput() || !KiwiGizmo_Show() )
            return false;
        // KIWI-UX (shakeout G): THE GIZMO STANDS DOWN WHILE THE PIVOT IS BEING
        // PLACED.  This is Plasticity's own move — `choosePivot` opens with
        // `gizmo.disable()` and closes with `gizmo.enable()`
        // (TranslateCommand.ts:316/328) — and here it is load-bearing rather than
        // cosmetic: kiwi_viewport.cpp offers an LMB press to KiwiGizmo_MouseDown
        // BEFORE KiwiCmd_MouseButton (kiwi_viewport.cpp:292 vs :311), so a gizmo
        // that stayed hit-testable would swallow the very click that places the
        // pivot and grab a handle instead.
        if ( KiwiXform_PivotPlacing() )
            return false;
        // KIWI-UX (ROUND K): THE GIZMO STANDS DOWN FOR THE LOLLIPOP.
        // USER DIRECTIVE, verbatim: "Also hide the move gizmo when extruding."
        // Refusing HERE rather than only in the draw is deliberate and is the same
        // reasoning as the pivot rung above: kiwi_viewport.cpp offers an LMB press
        // to the gizmo before it reaches the command, so a gizmo that was merely
        // invisible would still swallow the press aimed at the lollipop's ball.
        // GizmoUsable gates BuildGeo, BuildRings, KiwiGizmo_MouseDown, the hover and
        // the draw, so one refusal covers every path.
        if ( KiwiLollipop_Active() )
            return false;
        const camera_s *c = Ed_Camera();
        return c->width >= 1 && c->height >= 1;
    }

    // True when the MOVE gizmo should exist this frame.
    // SHAKEOUT D: the visibility test is now the COMMAND's identity, not the
    // selection's shape — "nothing should show up by default" (kiwi_gizmo.h).
    // ROUND L: the anchor is the command's LIVE one — the latched reference point
    // PLUS what the gesture has applied — so the handles RIDE the geometry instead
    // of staying nailed to where the drag began.  The drag's cursor mapping still
    // measures from the latched point, so nothing moves under the drag; see
    // kiwi_transform.h KiwiXform_ActivePivot for the full argument.
    bool BuildGeo( gizmoGeo_t *g )
    {
        if ( !GizmoUsable() )
            return false;
        if ( !KiwiXform_IsMoveActive() )
            return false;
        if ( !KiwiXform_ActivePivot( g->anchor ) )
            return false;

        const float wpp = KiwiCam_WorldPerPixel( g->anchor );
        g->axisLen    = KGZ_AXIS_PIX   * wpp;
        g->headLen    = KGZ_HEAD_PIX   * wpp;
        g->headWide   = KGZ_HEAD_WIDE  * wpp;
        g->planeOff   = KGZ_PLANE_PIX  * wpp;
        g->planeHalf  = KGZ_PLANE_HALF * wpp;
        g->centreRad  = KGZ_CENTER_PIX * wpp;
        // SHAKEOUT G: the face-normal arrow exists only while Move is pushing
        // faces, and it points along the DRIVE face's own outward normal.
        g->hasNormal  = KiwiXform_ActivePushDir( g->normal );
        return true;
    }

    // The normal arrow's tip.  Slightly LONGER than the world axes so that, when it
    // happens to coincide with one of them (an axis-aligned face — the common case),
    // it is the one whose shaft the cursor reaches first.
    void NormalTip( const gizmoGeo_t &g, float *out )
    {
        Mad3( g.anchor, g.normal, g.axisLen * 1.25f, out );
    }

    // ── the frame's rotate-ring geometry ────────────────────────────────────
    struct ringGeo_t
    {
        float pivot[3];
        float radius;
    };

    bool BuildRings( ringGeo_t *g )
    {
        if ( !GizmoUsable() )
            return false;
        if ( !KiwiXform_IsRotateActive() )
            return false;
        if ( !KiwiXform_ActivePivot( g->pivot ) )
            return false;
        g->radius = KGZ_RING_PIX * KiwiCam_WorldPerPixel( g->pivot );
        return g->radius > 0.0f;
    }

    // Ring point k of the ring about `axis`.  U = world axis (a+1)%3,
    // V = world axis (a+2)%3 — the right-handed in-plane basis the sign
    // derivation at the top of this file is written against.
    void RingPoint( const ringGeo_t &g, int axis, int k, float *out )
    {
        const int   i  = ( axis + 1 ) % 3;
        const int   j  = ( axis + 2 ) % 3;
        const float th = ( 6.283185307179586f * (float)k ) / (float)KGZ_RING_SEGMENTS;
        Copy3( g.pivot, out );
        out[i] += cosf( th ) * g.radius;
        out[j] += sinf( th ) * g.radius;
    }

    // The cursor's angle around the pivot IN THE RING'S PLANE, in degrees,
    // right-hand-positive about `axis`.  False when the ray is parallel to the
    // plane or lands on the pivot itself (no meaningful angle either way).
    bool RingCursorAngle( const ringGeo_t &g, int axis, int imgX, int imgY, float *outDeg )
    {
        ray_t ray;
        if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
            return false;
        float n[3] = { 0.0f, 0.0f, 0.0f };
        n[axis] = 1.0f;
        float q[3];
        if ( !RayPlane( ray, g.pivot, n, q ) )
            return false;

        const int i = ( axis + 1 ) % 3;
        const int j = ( axis + 2 ) % 3;
        float rel[3];
        Sub3( q, g.pivot, rel );
        // Guard against the degenerate centre: a hair below one thousandth of the
        // ring radius carries no reliable direction.
        const float tiny = g.radius * 0.001f;
        if ( fabsf( rel[i] ) < tiny && fabsf( rel[j] ) < tiny )
            return false;
        *outDeg = atan2f( rel[j], rel[i] ) * KGZ_DEG;
        return true;
    }

    // ── ROUND AJ, ITEM 4: the plane handle is a SQUARE, not an L ────────────
    // Plasticity's is a filled 0.2 x 0.2 plane centred at (0.5, 0.5) in shaft-length
    // units (MiniGizmos.ts:382-398); this is its outline at the same proportions.
    // The four corners come out in winding order, so the caller draws 4 segments and
    // the hit test measures 4 edges plus the 2 diagonals.
    void PlaneSquare( const gizmoGeo_t &g, int n, float q[4][3] )
    {
        const int i = ( n + 1 ) % 3;
        const int j = ( n + 2 ) % 3;
        const float si[4] = { -1.0f,  1.0f,  1.0f, -1.0f };
        const float sj[4] = { -1.0f, -1.0f,  1.0f,  1.0f };
        for ( int k = 0; k < 4; ++k )
        {
            Copy3( g.anchor, q[k] );
            q[k][i] += g.planeOff + si[k] * g.planeHalf;
            q[k][j] += g.planeOff + sj[k] * g.planeHalf;
        }
    }

    // The white ORIGIN RING, in the camera's own plane so it reads as screen-space
    // (Plasticity billboards its CircleMoveGizmo — MiniGizmos.ts:106-116).
    void CentreRingPoint( const gizmoGeo_t &g, const camera_s *c, int k, float *out )
    {
        const float th = ( 6.283185307179586f * (float)k ) / (float)KGZ_CENTER_SEGS;
        const float cs = cosf( th ) * g.centreRad;
        const float sn = sinf( th ) * g.centreRad;
        for ( int a = 0; a < 3; ++a )
            out[a] = g.anchor[a] + c->vright[a] * cs + c->vup[a] * sn;
    }

    void AxisTip( const gizmoGeo_t &g, int axis, float *out )
    {
        Copy3( g.anchor, out );
        out[axis] += g.axisLen;
    }

    // ── screen-space helpers ────────────────────────────────────────────────
    float Dist2D( float ax, float ay, float bx, float by )
    {
        const float dx = ax - bx, dy = ay - by;
        return sqrtf( dx * dx + dy * dy );
    }

    // Point→segment distance in pixels; `outT` is the clamped parameter along a→b.
    float SegDist2D( float px, float py, float ax, float ay,
                     float bx, float by, float *outT )
    {
        const float dx = bx - ax, dy = by - ay;
        const float len2 = dx * dx + dy * dy;
        float t = 0.0f;
        if ( len2 > 1.0e-6f )
        {
            t = ( ( px - ax ) * dx + ( py - ay ) * dy ) / len2;
            if ( t < 0.0f )      t = 0.0f;
            else if ( t > 1.0f ) t = 1.0f;
        }
        if ( outT )
            *outT = t;
        return Dist2D( px, py, ax + dx * t, ay + dy * t );
    }

    // ── the translate hit test ──────────────────────────────────────────────
    // Resolution order is CENTRE → PLANE → AXIS, deliberately: the three handle
    // families overlap near the anchor and this is the order of increasing reach,
    // so the smallest target always wins where they compete.  The axis arm
    // additionally refuses hits below KGZ_AXIS_MIN_T, which is the stretch of
    // shaft that runs under the plane handles.
    int HitTest( const gizmoGeo_t &g, int imgX, int imgY )
    {
        const float px = (float)imgX;
        const float py = (float)imgY;

        float ax, ay;
        if ( !Pick_WorldToImage( g.anchor, &ax, &ay ) )
            return KGZ_NONE;                    // anchor behind the eye

        // ── ROUND AJ, ITEM 4: THE CENTRE ZONE GREW, IT DID NOT MOVE ─────────
        // It was an 8 px disc and is now a 12 px one, so the white ring drawn at
        // 10 px is INSIDE its own target rather than 6 px outside it.  A strict
        // superset, and it cannot steal from anything: the axis shafts refuse hits
        // below KGZ_AXIS_MIN_T (0.28 * 70 = 19.6 px) and the plane squares now sit
        // at 35 px on each axis.  Plasticity's own ring is proportionally larger
        // (0.31 of the shaft, so 22 px here) and it can afford that because its axis
        // PICKER is a small sphere at the arrow TIP only (MoveGizmo.ts:139) rather
        // than the whole shaft; ours is the shaft, so 12 px is where the two rules
        // meet without either handle eating the other.
        if ( Dist2D( px, py, ax, ay ) <= KGZ_CENTER_PICK )
            return KGZ_CENTER;

        // ── ROUND AJ, ITEM 4: THE PLANE ZONE FOLLOWED ITS VISUAL ────────────
        // It was the two arms of an L whose corner sat 22 px out; it is now the
        // perimeter (plus both diagonals, which cover the interior of a 14 px
        // square) of the square centred 35 px out.  A hit zone HAS to be where the
        // handle is drawn — leaving it at the old corner would have made the new
        // square decorative and the grab invisible — so this one relocates.  The
        // tolerance (KGZ_PICK_PIX) and the resolution order are unchanged, and the
        // covered area is larger than the two old arms were.
        for ( int n = 0; n < 3; ++n )
        {
            float q[4][3];
            PlaneSquare( g, n, q );
            float sx[4], sy[4];
            bool  ok = true;
            for ( int k = 0; k < 4 && ok; ++k )
                ok = Pick_WorldToImage( q[k], &sx[k], &sy[k] );
            if ( !ok )
                continue;
            bool hit = false;
            for ( int k = 0; k < 4 && !hit; ++k )
            {
                const int m = ( k + 1 ) & 3;
                hit = SegDist2D( px, py, sx[k], sy[k], sx[m], sy[m], 0 ) <= KGZ_PICK_PIX;
            }
            if ( !hit )
                hit = SegDist2D( px, py, sx[0], sy[0], sx[2], sy[2], 0 ) <= KGZ_PICK_PIX
                   || SegDist2D( px, py, sx[1], sy[1], sx[3], sy[3], 0 ) <= KGZ_PICK_PIX;
            if ( hit )
                return KGZ_PLANE_X + n;
        }

        int   best     = KGZ_NONE;
        float bestDist = KGZ_PICK_PIX;

        // SHAKEOUT G: the normal arrow is tested FIRST among the shafts and wins
        // ties (`<=` below lets a later, equally close axis take it, so the normal
        // is tested first and the axes have to be STRICTLY closer to steal it).
        if ( g.hasNormal )
        {
            float tip[3];
            NormalTip( g, tip );
            float tx, ty;
            if ( Pick_WorldToImage( tip, &tx, &ty ) )
            {
                float t = 0.0f;
                const float d = SegDist2D( px, py, ax, ay, tx, ty, &t );
                if ( t >= KGZ_AXIS_MIN_T && d <= bestDist )
                {
                    bestDist = d;
                    best     = KGZ_NORMAL;
                }
            }
        }

        for ( int axis = 0; axis < 3; ++axis )
        {
            float tip[3];
            AxisTip( g, axis, tip );
            float tx, ty;
            if ( !Pick_WorldToImage( tip, &tx, &ty ) )
                continue;
            float t = 0.0f;
            const float d = SegDist2D( px, py, ax, ay, tx, ty, &t );
            if ( t < KGZ_AXIS_MIN_T )
                continue;
            if ( d <= bestDist )
            {
                bestDist = d;
                best     = KGZ_AXIS_X + axis;
            }
        }
        return best;
    }

    // ── the ring hit test ───────────────────────────────────────────────────
    // The SAME points the draw emits, projected back to pixels and measured as a
    // polyline: what the user sees and what the test measures can never disagree
    // (and a ring seen edge-on collapses to a line for both).  Nearest ring wins,
    // so overlapping rings resolve to the one actually under the cursor.
    int RingHitTest( const ringGeo_t &g, int imgX, int imgY )
    {
        const float px = (float)imgX;
        const float py = (float)imgY;

        int   best     = -1;
        float bestDist = KGZ_RING_PICK_PIX;

        for ( int axis = 0; axis < 3; ++axis )
        {
            float p0[3];
            RingPoint( g, axis, 0, p0 );
            float sx0, sy0;
            bool  have0 = Pick_WorldToImage( p0, &sx0, &sy0 );

            for ( int k = 1; k <= KGZ_RING_SEGMENTS; ++k )
            {
                float p1[3];
                RingPoint( g, axis, k % KGZ_RING_SEGMENTS, p1 );
                float sx1, sy1;
                const bool have1 = Pick_WorldToImage( p1, &sx1, &sy1 );

                if ( have0 && have1 )
                {
                    const float d = SegDist2D( px, py, sx0, sy0, sx1, sy1, 0 );
                    if ( d <= bestDist )
                    {
                        bestDist = d;
                        best     = axis;
                    }
                }
                sx0 = sx1; sy0 = sy1; have0 = have1;
            }
        }
        return best;
    }

    // ── draw helpers ────────────────────────────────────────────────────────
    // ROUND AJ, ITEM 4: THREE states, not two.  Plasticity's hover is a material
    // swap to the LIGHTER tint of the same hue (600 -> 400, MiniGizmos.ts:220-228);
    // this file additionally keeps its yellow for the HELD state, because unlike
    // Plasticity's — where the pointer is captured and the whole scene stops
    // responding — a grab here coexists with camera navigation, and "I am pointing
    // at it" and "I am holding it" have to be told apart at a glance.
    void Colour( int handle, int fallbackAxis )
    {
        if ( s_grabbed == handle )
            KiwiLines_Color( KGZ_HOT[0], KGZ_HOT[1], KGZ_HOT[2] );
        else if ( fallbackAxis < 0 )
            KiwiLines_Color( KGZ_CENTRE[0], KGZ_CENTRE[1], KGZ_CENTRE[2] );
        else if ( s_hot == handle )
            KiwiLines_Color( KGZ_COL_HOVER[fallbackAxis][0], KGZ_COL_HOVER[fallbackAxis][1],
                             KGZ_COL_HOVER[fallbackAxis][2] );
        else
            KiwiLines_Color( KGZ_COL[fallbackAxis][0], KGZ_COL[fallbackAxis][1],
                             KGZ_COL[fallbackAxis][2] );
    }

    // ROUND AK, ITEM 4: the same three-state decision as `Colour`, returning the
    // RGB rather than pushing it at the line batcher — the fill path packs its own
    // colour per vertex.  One decision, two consumers, so the fill and its outline
    // can never disagree about which state a handle is in.
    const float *HandleRgb( int handle, int fallbackAxis )
    {
        if ( s_grabbed == handle )
            return KGZ_HOT;
        if ( fallbackAxis < 0 )
            return KGZ_CENTRE;
        if ( s_hot == handle )
            return KGZ_COL_HOVER[fallbackAxis];
        return KGZ_COL[fallbackAxis];
    }

    inline bool HandleEmph( int handle )
    {
        return s_grabbed == handle || s_hot == handle;
    }

    void RingColour( int axis )
    {
        if ( s_ringGrab == axis )
            KiwiLines_Color( KGZ_HOT[0], KGZ_HOT[1], KGZ_HOT[2] );
        else if ( s_ringGrab < 0 && s_ringHot == axis )
            KiwiLines_Color( KGZ_COL_HOVER[axis][0], KGZ_COL_HOVER[axis][1],
                             KGZ_COL_HOVER[axis][2] );
        else
            KiwiLines_Color( KGZ_COL[axis][0], KGZ_COL[axis][1], KGZ_COL[axis][2] );
    }

    // ── ROUND AJ, ITEM 4: ONE HANDLE'S SEGMENTS ─────────────────────────────
    // Factored out so the HOVER OVERDRAW below can re-emit exactly the handle under
    // the cursor at a heavier line width, which is the other half of Plasticity's
    // hover (linewidth 2 -> 3, GizmoMaterials.ts:33/37).  KiwiLines_Begin takes the
    // width PER BATCH, not per segment (kiwi_lines.h), so "one element thicker"
    // costs a second batch — which is why only ONE element ever gets it.
    void EmitAxis( const gizmoGeo_t &g, const camera_s *c, int axis )
    {
        float tip[3];
        AxisTip( g, axis, tip );
        KiwiLines_Add( g.anchor, tip );

        // The head: two chevron segments PLUS the base line closing them, so it
        // reads as the small solid cone Plasticity uses (base radius 0.1, height
        // 0.2 of the shaft — MiniGizmos.ts:346-347) instead of as a bare V.  Drawn
        // in whichever off-axis plane is more side-on to the view; a chevron in the
        // near-edge-on plane would collapse to a line.
        const int i = ( axis + 1 ) % 3;
        const int j = ( axis + 2 ) % 3;
        const float wi = fabsf( c->vright[i] ) + fabsf( c->vup[i] );
        const float wj = fabsf( c->vright[j] ) + fabsf( c->vup[j] );
        const int   k  = ( wi >= wj ) ? i : j;

        float back[3];
        Copy3( tip, back );
        back[axis] -= g.headLen;
        float b1[3], b2[3];
        Copy3( back, b1 ); b1[k] += g.headWide;
        Copy3( back, b2 ); b2[k] -= g.headWide;
        KiwiLines_Add( tip, b1 );
        KiwiLines_Add( tip, b2 );
        KiwiLines_Add( b1, b2 );
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AK, ITEM 4) — THE FILLS.  NO HOLLOW SHAPES.
    // ═══════════════════════════════════════════════════════════════════════
    // USER DIRECTIVE, verbatim: "The new gizmo's are better, but I want them
    // slightly brighter colors and filled in (no hollow shapes)."
    //
    // Round AJ drew Plasticity's SOLID meshes — cone heads, a 0.2 x 0.2 filled
    // plane square, a billboarded white disc — as OUTLINES, because this file only
    // had a line batcher.  It says so in its own head/plane comments ("so it reads
    // as the small solid cone ... instead of as a bare V").  The directive is to
    // stop approximating: the shapes get real triangles.
    //
    // THE PATH IS THE ONE EVERY KIWI FILL USES: R_AddRenderCmdDrawTris on
    // g_qeglobals.d_white with TECHNIQUE_UNLIT, inside a neutral MATERIAL_COLOR
    // bracket so the PER-VERTEX colour drives the draw, and KiwiTris_OrientToEye
    // per element so nothing is lost to the white_tools back-face cull
    // (kiwi_lines.h TRAP 3).  Identical in shape to kiwi_region.cpp's region fills
    // and kiwi_split.cpp's cut quad.
    //
    // THE LINES STAY.  Every filled element keeps its outline pass on top: the
    // shafts are lines by definition, and a 2 px outline over a fill is what makes
    // a 14 px arrowhead read as a crisp silhouette rather than as a smear.  The
    // hover overdraw (pass 2) is untouched, so grab semantics and the hit tests are
    // exactly what round AJ shipped — this round adds pixels and nothing else.
    //
    // ── OPACITY, PER ELEMENT AND ON PURPOSE ─────────────────────────────────
    //   HEADS   opaque.  They are 14 px of solid colour at the end of a shaft and
    //           they must not double-blend against themselves: a closed cone drawn
    //           translucent shows its own far wall through its near one.
    //   PLANES  translucent.  The square sits over the scene at 35 px out and is a
    //           quad the user has to be able to see PAST — Plasticity's is
    //           translucent for the same reason.
    //   ORIGIN  nearly opaque.  It is 10 px across and marks the pivot, which is
    //           the one place the user wants an unambiguous answer.
    const float KGZ_FILL_A_HEAD   = 1.00f;
    const float KGZ_FILL_A_PLANE  = 0.42f;
    const float KGZ_FILL_A_CENTRE = 0.85f;
    // The hover/held states gain opacity as well as the tint the lines already
    // carry — on a FILL, "lighter colour" alone is a weak signal.
    const float KGZ_FILL_A_EMPH   = 0.72f;   // the plane square, hovered or held
    const int   KGZ_CONE_SEGS     = 10;      // base ring of one arrowhead cone

    // ── the staging buffers ─────────────────────────────────────────────────
    // ONE ELEMENT AT A TIME: FillFlush emits and resets, so these are sized by the
    // largest single element and not by the gizmo.  That is the origin DISC —
    // KGZ_CENTER_SEGS (24) rim points + 1 centre = 25 verts, 24 triangles.  The
    // cone is 1 + KGZ_CONE_SEGS + 1 = 12 verts and 2 * KGZ_CONE_SEGS = 20 tris.
    // 40 / 40 leaves headroom for both and is 1 KB of statics.
    //
    // BUDGET: at most 3 axis cones + 1 normal cone + 3 plane quads + 1 disc = 8
    // R_AddRenderCmdDrawTris per frame, 4 * 20 + 3 * 2 + 24 = 110 triangles.  The
    // rotate arm fills nothing (its rings are lines and always were).
    enum { KGZ_FILL_MAX_VERTS = 40, KGZ_FILL_MAX_TRIS = 40 };

    float    s_fXyzw  [KGZ_FILL_MAX_VERTS][4];
    float    s_fNormal[KGZ_FILL_MAX_VERTS][3];
    float    s_fSt    [KGZ_FILL_MAX_VERTS][2];
    float    s_fColor [KGZ_FILL_MAX_VERTS];
    uint16_t s_fIdx   [KGZ_FILL_MAX_TRIS * 3];
    int      s_fVerts  = 0;
    int      s_fIdxN   = 0;
    float    s_fPacked = 0.0f;
    float    s_fNrm[3] = { 0.0f, 0.0f, 1.0f };
    bool     s_fFlat   = false;                 // ROUND AM — this element took the override

    // ── KIWI-UX (ROUND AM, ITEM 4) — WHICH ELEMENTS TAKE THE FLAT OVERRIDE ──
    // USER REPORT, verbatim: "gizmo arrows are still not filled in."  Round AK
    // gave the heads real triangles and round AL fixed their normal, and what is
    // still on screen is round AJ's OUTLINE chevron — i.e. the fill is emitted and
    // does not appear, which is item 1's report on a different shape.  The shared
    // diagnosis and the measurement behind it are kiwi_lines.h TRAP 5.
    //
    // The threshold is the honest one: an element that is OPAQUE BY DESIGN cannot
    // lose anything to an override whose only risk is opacity.  The heads are 1.00
    // and the origin disc is 0.85 (the per-element opacity note above says why),
    // so both take it; the plane squares at 0.42 are meant to be seen PAST and
    // stay on the neutral bracket until the region-fill probe settles the alpha
    // question.  0.80 sits between the two with no constant near it.
    const float KGZ_FILL_FLAT_MIN = 0.80f;

    void FillBegin( const camera_s *c, const float *rgb, float alpha )
    {
        float rgba[4] = { rgb[0], rgb[1], rgb[2], alpha };
        GfxColor packed;
        Byte4PackPixelColor( rgba, &packed );
        s_fPacked = *(float *)&packed.packed;   // bit-cast, as the ported batcher does
        s_fVerts  = 0;
        s_fIdxN   = 0;
        s_fFlat   = ( alpha >= KGZ_FILL_FLAT_MIN );
        if ( s_fFlat )
            KiwiTris_FillFlatColor( rgba );     // kiwi_lines.h TRAP 5
        // ── KIWI-UX (ROUND AL, ITEM 2) — "TECHNIQUE_UNLIT DOES NOT SHADE WITH
        //    IT" WAS FALSE, AND IT IS HALF OF THE REPORT ──────────────────────
        // USER REPORT, verbatim: "the gizmos are only solid at a high zoom level.
        // Wtf?  Fix this."  TECHNIQUE_UNLIT on the "tools" techset resolves to
        // vertcol_SHADED_tools, whose vertex stage modulates the vertex colour by
        // a term computed from this array (kiwi_lines.h TRAP 4 for the decode and
        // the two rounds of evidence that pin its direction down).  With `-vpn`
        // here the fills' brightness was a function of where the camera pointed,
        // while the OUTLINES were immune — Ed_EmitLineBatch pushes a per-colour-
        // run MATERIAL_COLOR with .w == 1 (r_rendercmds.cpp:1940-1975), which
        // lerps the whole vertex term away.  "The fills vanish and the outlines
        // stay" is that asymmetry, exactly.  A constant world normal removes it.
        KiwiTris_FillNormal( s_fNrm );
        (void)c;
    }

    int FillVertex( const float *p )
    {
        if ( s_fVerts >= KGZ_FILL_MAX_VERTS )
            return -1;
        const int i = s_fVerts++;
        s_fXyzw[i][0] = p[0];
        s_fXyzw[i][1] = p[1];
        s_fXyzw[i][2] = p[2];
        s_fXyzw[i][3] = 1.0f;
        s_fNormal[i][0] = s_fNrm[0];
        s_fNormal[i][1] = s_fNrm[1];
        s_fNormal[i][2] = s_fNrm[2];
        s_fSt[i][0] = 0.0f;
        s_fSt[i][1] = 0.0f;
        s_fColor[i] = s_fPacked;
        return i;
    }

    void FillTri( int a, int b, int d )
    {
        if ( a < 0 || b < 0 || d < 0 )
            return;
        if ( s_fIdxN + 3 > KGZ_FILL_MAX_TRIS * 3 )
            return;
        s_fIdx[s_fIdxN++] = (uint16_t)a;
        s_fIdx[s_fIdxN++] = (uint16_t)b;
        s_fIdx[s_fIdxN++] = (uint16_t)d;
    }

    void FillFlush( const camera_s *c )
    {
        if ( s_fVerts < 3 || s_fIdxN < 3 )
        {
            s_fVerts = 0;
            s_fIdxN  = 0;
            if ( s_fFlat ) { KiwiTris_FillNeutral(); s_fFlat = false; }
            return;
        }
        // kiwi_lines.h TRAP 3: white_tools culls back faces, so every fan in this
        // layer is oriented at the eye per triangle before it is handed over.
        KiwiTris_OrientToEye( &s_fXyzw[0][0], 4, s_fIdx, s_fIdxN, c->origin );
        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                (short)s_fIdxN, s_fIdx, (short)s_fVerts,
                                s_fXyzw, s_fNormal, s_fColor, s_fSt );
        s_fVerts = 0;
        s_fIdxN  = 0;
        // ROUND AM: hand the pass's neutral bracket back, so the next element (and
        // every line pass after this one) starts from exactly what it did before.
        if ( s_fFlat ) { KiwiTris_FillNeutral(); s_fFlat = false; }
    }

    // A CLOSED cone: `tip`, a base ring of KGZ_CONE_SEGS points of `radius` about
    // `base` in the (u, v) plane, and a base cap fan.  Closed so the head reads as
    // a solid from behind as well as from in front — the outline pass alone always
    // did, and a fill that vanished at some angles would be worse than none.
    void FillCone( const camera_s *c, const float *tip, const float *base,
                   const float *u, const float *v, float radius )
    {
        const int iTip  = FillVertex( tip );
        const int iBase = FillVertex( base );
        int first = -1, prev = -1;
        for ( int k = 0; k < KGZ_CONE_SEGS; ++k )
        {
            const float th = ( 6.283185307179586f * (float)k ) / (float)KGZ_CONE_SEGS;
            const float cs = cosf( th ) * radius;
            const float sn = sinf( th ) * radius;
            float p[3];
            for ( int a = 0; a < 3; ++a )
                p[a] = base[a] + u[a] * cs + v[a] * sn;
            const int i = FillVertex( p );
            if ( first < 0 )
                first = i;
            if ( prev >= 0 )
            {
                FillTri( iTip,  prev, i );      // side
                FillTri( iBase, i,    prev );   // cap
            }
            prev = i;
        }
        FillTri( iTip,  prev,  first );
        FillTri( iBase, first, prev );
        FillFlush( c );
    }

    void EmitPlane( const gizmoGeo_t &g, int n )
    {
        float q[4][3];
        PlaneSquare( g, n, q );
        for ( int k = 0; k < 4; ++k )
            KiwiLines_Add( q[k], q[( k + 1 ) & 3] );
    }

    void EmitCentreRing( const gizmoGeo_t &g, const camera_s *c )
    {
        float prev[3];
        CentreRingPoint( g, c, 0, prev );
        for ( int k = 1; k <= KGZ_CENTER_SEGS; ++k )
        {
            float p[3];
            CentreRingPoint( g, c, k % KGZ_CENTER_SEGS, p );
            if ( !KiwiLines_Add( prev, p ) )
                return;
            Copy3( p, prev );
        }
    }

    void EmitNormalArrow( const gizmoGeo_t &g, const camera_s *c )
    {
        float tip[3];
        NormalTip( g, tip );
        KiwiLines_Add( g.anchor, tip );

        // side = the component of vright most perpendicular to the shaft, so the
        // chevron never collapses to a line on a shaft pointing at the camera.
        float side[3];
        const float along = Dot3( c->vright, g.normal );
        for ( int k = 0; k < 3; ++k )
            side[k] = c->vright[k] - g.normal[k] * along;
        float l = sqrtf( Dot3( side, side ) );
        if ( l < 1.0e-4f )
        {
            const float along2 = Dot3( c->vup, g.normal );
            for ( int k = 0; k < 3; ++k )
                side[k] = c->vup[k] - g.normal[k] * along2;
            l = sqrtf( Dot3( side, side ) );
        }
        if ( l <= 1.0e-4f )
            return;
        for ( int k = 0; k < 3; ++k )
            side[k] /= l;
        float back[3], b1[3], b2[3];
        Mad3( tip, g.normal, -g.headLen, back );
        Mad3( back, side,  g.headWide, b1 );
        Mad3( back, side, -g.headWide, b2 );
        KiwiLines_Add( tip, b1 );
        KiwiLines_Add( tip, b2 );
        KiwiLines_Add( b1, b2 );
    }

    // ── ROUND AK, ITEM 4: THE FILLED HALF OF EACH ELEMENT ───────────────────
    // Each of these emits ONE R_AddRenderCmdDrawTris and returns.  They run BEFORE
    // the line passes so the outlines land on top of their own fills ($line is
    // depthTest LESSEQUAL and the two are coplanar / coincident).

    // The arrowhead, as the cone round AJ's comment said it wanted to be.  The two
    // OFF-AXIS world axes are the cone's base plane by construction — no basis has
    // to be invented and none can degenerate, which is why this is simpler than the
    // face-normal case below.
    void FillAxisHead( const gizmoGeo_t &g, const camera_s *c, int axis )
    {
        float tip[3];
        AxisTip( g, axis, tip );
        float base[3];
        Copy3( tip, base );
        base[axis] -= g.headLen;

        // ── KIWI-UX (ROUND AN, ITEM 8): BILLBOARDED, NOT A CONE ─────────────
        // USER REPORT, verbatim: "the gizmos are STILL not solid.  Make them
        // solid!"  A 3D cone whose axis points near the camera presents only its
        // edge-on side triangles and an end-on cap — at typical zooms that reads
        // as a hollow ring, which is exactly the per-axis asymmetry in the
        // screenshots (one axis filled, two hollow, depending on the view).
        // Plasticity's heads are effectively screen-facing.  A SCREEN-FACING
        // TRIANGLE has no end-on presentation AT ALL — solid from every camera,
        // structurally.  The side basis is the outline chevron's own (the
        // component of vright, then vup, perpendicular to the shaft), so fill
        // and outline agree by construction.
        float side[3];
        float shaft[3] = { 0.0f, 0.0f, 0.0f };
        shaft[axis] = 1.0f;
        const float along = Dot3( c->vright, shaft );
        for ( int k = 0; k < 3; ++k )
            side[k] = c->vright[k] - shaft[k] * along;
        float l = sqrtf( Dot3( side, side ) );
        if ( l < 1.0e-4f )
        {
            const float along2 = Dot3( c->vup, shaft );
            for ( int k = 0; k < 3; ++k )
                side[k] = c->vup[k] - shaft[k] * along2;
            l = sqrtf( Dot3( side, side ) );
        }
        if ( l <= 1.0e-4f )
            return;
        for ( int k = 0; k < 3; ++k )
            side[k] /= l;

        float b1[3], b2[3];
        Mad3( base, side,  g.headWide, b1 );
        Mad3( base, side, -g.headWide, b2 );

        FillBegin( c, HandleRgb( KGZ_AXIS_X + axis, axis ), KGZ_FILL_A_HEAD );
        FillTri( FillVertex( tip ), FillVertex( b1 ), FillVertex( b2 ) );
        FillFlush( c );
    }

    // The face-normal arrow's head.  Its axis is arbitrary, so the base plane is
    // built the same way EmitNormalArrow builds its chevron side: project vright
    // (then vup) off the shaft and normalise, then the second basis vector is the
    // cross product, which is unit because the two inputs are unit and orthogonal.
    void FillNormalHead( const gizmoGeo_t &g, const camera_s *c )
    {
        float tip[3];
        NormalTip( g, tip );

        float u[3];
        const float along = Dot3( c->vright, g.normal );
        for ( int k = 0; k < 3; ++k )
            u[k] = c->vright[k] - g.normal[k] * along;
        float l = sqrtf( Dot3( u, u ) );
        if ( l < 1.0e-4f )
        {
            const float along2 = Dot3( c->vup, g.normal );
            for ( int k = 0; k < 3; ++k )
                u[k] = c->vup[k] - g.normal[k] * along2;
            l = sqrtf( Dot3( u, u ) );
        }
        if ( l <= 1.0e-4f )
            return;
        for ( int k = 0; k < 3; ++k )
            u[k] /= l;
        const float v[3] = { g.normal[1] * u[2] - g.normal[2] * u[1],
                             g.normal[2] * u[0] - g.normal[0] * u[2],
                             g.normal[0] * u[1] - g.normal[1] * u[0] };

        float base[3];
        Mad3( tip, g.normal, -g.headLen, base );

        const float *rgb = ( s_grabbed == KGZ_NORMAL || s_hot == KGZ_NORMAL )
                         ? KGZ_HOT : KGZ_NORMAL_COL;
        // ROUND AN, ITEM 8: billboarded triangle, same argument as FillAxisHead —
        // `u` above is already the screen-plane side basis this function built.
        float b1[3], b2[3];
        Mad3( base, u,  g.headWide, b1 );
        Mad3( base, u, -g.headWide, b2 );
        FillBegin( c, rgb, KGZ_FILL_A_HEAD );
        FillTri( FillVertex( tip ), FillVertex( b1 ), FillVertex( b2 ) );
        FillFlush( c );
    }

    // The plane handle: two triangles over the SAME four corners the outline and
    // the hit test already share (PlaneSquare), so the fill cannot drift from
    // either of them.
    void FillPlaneSquare( const gizmoGeo_t &g, const camera_s *c, int n )
    {
        float q[4][3];
        PlaneSquare( g, n, q );
        const int handle = KGZ_PLANE_X + n;
        FillBegin( c, HandleRgb( handle, n ),
                   HandleEmph( handle ) ? KGZ_FILL_A_EMPH : KGZ_FILL_A_PLANE );
        const int i0 = FillVertex( q[0] );
        const int i1 = FillVertex( q[1] );
        const int i2 = FillVertex( q[2] );
        const int i3 = FillVertex( q[3] );
        FillTri( i0, i1, i2 );
        FillTri( i0, i2, i3 );
        FillFlush( c );
    }

    // The origin DISC — a fan over the same billboarded rim CentreRingPoint draws,
    // so the fill is exactly the ring's interior and the ring is exactly its edge.
    void FillCentreDisc( const gizmoGeo_t &g, const camera_s *c )
    {
        FillBegin( c, HandleRgb( KGZ_CENTER, -1 ),
                   HandleEmph( KGZ_CENTER ) ? 1.0f : KGZ_FILL_A_CENTRE );
        const int iC = FillVertex( g.anchor );
        int first = -1, prev = -1;
        for ( int k = 0; k < KGZ_CENTER_SEGS; ++k )
        {
            float p[3];
            CentreRingPoint( g, c, k, p );
            const int i = FillVertex( p );
            if ( first < 0 )
                first = i;
            if ( prev >= 0 )
                FillTri( iC, prev, i );
            prev = i;
        }
        FillTri( iC, prev, first );
        FillFlush( c );
    }

    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND AL, ITEM 2) — THE GIZMO IS ALWAYS ON TOP, FULL STOP
    // ═══════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: "the gizmos are only solid at a high zoom level.
    // Wtf?  Fix this."
    //
    // A GIZMO IS A HUD OBJECT WITH WORLD COORDINATES.  It is screen-constant in
    // SIZE (KiwiCam_WorldPerPixel), so its WORLD extent grows without bound as the
    // view zooms out — at a wide ortho zoom the shaft is KGZ_AXIS_PIX * wpp, i.e.
    // ~0.12 * s_dist, hundreds of units — and it is anchored at the SELECTION's
    // pivot, which is normally inside the geometry being moved.  Everything about
    // whether it survives a depth test therefore changes with zoom, which is not a
    // property a manipulator is allowed to have.  A bigger eye-ward nudge is the
    // wrong shape of answer: it would be one more number that is right at one
    // distance.
    //
    // THE MECHANISM IS THE EDITOR'S OWN, not a material trick.  The binary's
    // selected-brush white outline opens with R_AddClearCmd(6 = depth|stencil)
    // "so the selected wireframe passes the depth test against the coplanar
    // geometry and shows THROUGH" (camwnd.cpp:2883-2889, 0x4084d2), and the port
    // already re-issues exactly that call a second time when a later pass has
    // dirtied the buffer under an overlay (camwnd.cpp:2996-3001, the terrain-paint
    // ring: "the port's filled patch pass can leave depth under the cursor, so
    // re-clear to preserve that overlay relationship").  This is the third use and
    // it is the same sentence.
    //
    // WHAT IT BUYS THAT THE EXISTING CLEAR DID NOT.  The KIWI overlay block
    // (camwnd.cpp:3007-3095) runs a long way after that prelude, and EVERY line
    // pass in it writes depth — $line is depthTest LESSEQUAL / depthWrite ON
    // (main/materials/$line refStateBits[1] = 0x0d, decoded at camwnd.cpp:2422-
    // 2432).  Hover outlines, construction lines, the marquee, the patch lattice,
    // the snap accents and the live command overlay all stamp the cleared buffer
    // before the gizmo draws, and the prelude clear itself is gated on
    // !dontDrawSelectedOutlines (a toggle at mainfrm.cpp:5121), so the gizmo's
    // "on top" was conditional on a pref it has nothing to do with.  Clearing here
    // makes it unconditional.
    //
    // COST AND BLAST RADIUS.  One RC_CLEAR_SCREEN per frame, and only on the
    // frames where a gizmo actually exists (both call sites are past their
    // Build* gate).  Nothing after the gizmo in the block reads depth except
    // KiwiLollipop_DrawWorld, and GizmoUsable() refuses outright while a lollipop
    // is wanted (kiwi_lollipop.h), so exactly one of the two ever emits.  The
    // gizmo's own internal order is unchanged and still correct: the FILLS draw
    // first and white_tools does not write depth, then the outlines draw on top
    // and stamp their own.
    void GizmoDepthClear()
    {
        static const float s_clearCol[4] = { 1.0f, 1.0f, 1.0f, 1.0f };   // unused at whichToClear 6
        R_AddCmdClearScreen( 6, s_clearCol, 1.0f, 0 );
    }

    // The whole fill pass, bracketed once.  MATERIAL_COLOR neutral so the
    // per-vertex colour drives the draw and white afterwards so no later pass
    // inherits it — the same bracket kiwi_region.cpp:1288-1290 and the ported
    // selected-face fill (camwnd.cpp 0x408106) use.
    void DrawTranslateFills( const gizmoGeo_t &g, const camera_s *c )
    {
        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white  [4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        R_AddCmdSetMaterialColor( s_neutral );

        for ( int axis = 0; axis < 3; ++axis )
            FillAxisHead( g, c, axis );
        if ( g.hasNormal )
            FillNormalHead( g, c );
        for ( int n = 0; n < 3; ++n )
            FillPlaneSquare( g, c, n );
        FillCentreDisc( g, c );

        R_AddCmdSetMaterialColor( s_white );
    }

    // ── ROUND AJ, ITEM 4: THE BACK-HALF CULL ────────────────────────────────
    // Plasticity hides the far halves of its three rotate rings with a giant
    // camera-facing plane that writes DEPTH ONLY, rendered before them
    // (RotateGizmo.ts:13/132-159: a 100000 x 100000 PlaneGeometry with
    // `opacity: 0, depthWrite: true` at `renderOrder = -1`).  That is what makes
    // three intersecting circles read as one SPHERE rather than as a tangle.
    //
    // This layer emits LINES through a batcher, not meshes through a depth buffer,
    // so the equivalent is to answer the same question per segment: is this segment
    // on the far side of the plane through the pivot facing the eye?  It is one dot
    // product, it needs no second pass and no depth state, and it is EXACT for a
    // ring centred on the pivot (which all four of them are).
    //
    // THE HIT TEST IS DELIBERATELY NOT CULLED.  RingHitTest still measures the whole
    // projected ring, so a ring's far half stays grabbable at the pixels where it
    // was always grabbable — hiding a handle's back half is a legibility decision,
    // and quietly shrinking the target with it would be a second, unasked change.
    bool RingSegmentVisible( const float *pivot, const camera_s *c,
                             const float *a, const float *b )
    {
        float mid[3];
        for ( int k = 0; k < 3; ++k )
            mid[k] = ( a[k] + b[k] ) * 0.5f - pivot[k];
        return Dot3( mid, c->vpn ) <= 0.0f;      // vpn points INTO the scene
    }

    // Hand the live rotate command the ring's sweep SINCE THE GRAB (ROUND L — it
    // used to be an absolute angle, see KiwiGizmo_MouseDown's ring arm).  The
    // negation is the ported-sign correction derived at the top of this file.
    void PushRingAngle()
    {
        KiwiXform_FeedRotateDegrees( true, -s_ringTotal );
    }
}

// ─── toggle ──────────────────────────────────────────────────────────────────
bool KiwiGizmo_Show()
{
    if ( s_show < 0 )
        s_show = Radiant_ProfileGetInt( "KiwiUX", "ShowGizmo", 1 ) ? 1 : 0;
    return s_show != 0;
}

void KiwiGizmo_SetShow( bool on )
{
    const int v = on ? 1 : 0;
    if ( s_show == v )
        return;
    s_show = v;
    Radiant_ProfileSetInt( "KiwiUX", "ShowGizmo", v );
    g_nUpdateBits |= 1;
}

// ─── hover ───────────────────────────────────────────────────────────────────
void KiwiGizmo_Hover( int imgX, int imgY, bool over )
{
    const int wasHandle = s_hot;
    const int wasRing   = s_ringHot;
    s_hot     = KGZ_NONE;
    s_ringHot = -1;

    if ( over && s_grabbed == KGZ_NONE && s_ringGrab < 0 )
    {
        CamWnd_BuildMatrix();
        gizmoGeo_t g;
        if ( BuildGeo( &g ) )
        {
            s_hot = HitTest( g, imgX, imgY );
        }
        else
        {
            ringGeo_t r;
            if ( BuildRings( &r ) )
                s_ringHot = RingHitTest( r, imgX, imgY );
        }
    }
    if ( s_hot != wasHandle || s_ringHot != wasRing )
        g_nUpdateBits |= 1;                     // repaint so the brighten shows
}

// ─── grab ────────────────────────────────────────────────────────────────────
// SHAKEOUT D: this no longer STARTS a command.  It aims the one that is already
// running, which is why the caller may skip it entirely when nothing is active.
bool KiwiGizmo_MouseDown( int imgX, int imgY )
{
    if ( !KiwiCmd_Active() )
        return false;                           // no modal gesture -> no gizmo at all

    CamWnd_BuildMatrix();

    // ── the move arrows ─────────────────────────────────────────────────────
    gizmoGeo_t g;
    if ( BuildGeo( &g ) )
    {
        const int hit = HitTest( g, imgX, imgY );
        if ( hit == KGZ_NONE )
            return false;

        // ── KIWI-UX (ROUND L): ARM THE GRAB THROUGH THE SHARED ENTRY ─────────
        // USER REPORT, verbatim: "When clicking on the gizmo after selecting a
        // shape, as soon as the gizmo is clicked it pre-calculates the mouse delta
        // and applies it.  It should ONLY move with gizmo drag, no pre existing
        // mouse offset."
        //
        // BEFORE (shakeout G, four hand-rolled steps here): the resume/rebase rung
        // ran ONLY when the gesture happened to be PAUSED — `if ( wasPaused )
        // KiwiCmd_Resume();` — so the first grab after pressing G, when the command
        // is HOT by definition, never re-latched anything before the gate opened.
        // NoteGrab's own OnConstraintChanged covered the MAPPING, but nothing
        // covered the SNAP arm, which is an ABSOLUTE mapping (`total = snapPos -
        // ref`) rather than a delta: with a snap target anywhere under the press
        // pixel the selection teleported onto it the instant the handle was taken,
        // and with a grid snap a second grab re-quantised an already-applied total.
        //
        // AFTER: KiwiCmd_HandleGrab runs the SAME five-step arm the lollipop has
        // used since round K (kiwi_command.h documents each step), and the command
        // holds its total frozen until the cursor actually leaves the grab pixel
        // (kiwi_transform.cpp's grab-freshness latch), so frame one moves NOTHING
        // whatever is under the cursor.  The constraint is aimed AFTER the arm: its
        // SetConstraint re-latches under the new constraint and Recompute then
        // measures a zero delta from it.
        if ( !KiwiCmd_HandleGrab( imgX, imgY ) )
            return false;

        if ( hit >= KGZ_AXIS_X && hit <= KGZ_AXIS_Z )
            KiwiXform_PresetMoveConstraint( KIWI_XCON_AXIS,  hit - KGZ_AXIS_X );
        else if ( hit >= KGZ_PLANE_X && hit <= KGZ_PLANE_Z )
            KiwiXform_PresetMoveConstraint( KIWI_XCON_PLANE, hit - KGZ_PLANE_X );
        else if ( hit == KGZ_NORMAL )
            // The face-normal arm: CON_FREE is what a face gesture reads as "push
            // along the drive face's own normal" (kiwi_transform.cpp
            // OnConstraintChanged), so releasing any axis lock IS aiming at it.
            KiwiXform_PresetMoveConstraint( KIWI_XCON_FREE, 0 );
        // KGZ_CENTER: leave the command's own default (free, on the latched
        // view-normal plane) — that IS the unconstrained free move.

        s_grabbed = hit;
        s_hot     = hit;
        g_nUpdateBits |= 1;
        return true;
    }

    // ── the rotate rings ────────────────────────────────────────────────────
    ringGeo_t r;
    if ( BuildRings( &r ) )
    {
        const int axis = RingHitTest( r, imgX, imgY );
        if ( axis < 0 )
            return false;

        float ang = 0.0f;
        if ( !RingCursorAngle( r, axis, imgX, imgY, &ang ) )
            return false;                       // edge-on ring: no usable mapping

        // ── KIWI-UX (ROUND L): THE RING GRAB TOOK THE ANGLE BACK TO ZERO ─────
        // BEFORE: the ring fed an ABSOLUTE angle — `deg = -s_ringTotal`, and
        // s_ringTotal was reset to 0 right here — so re-grabbing a ring after a
        // release fed 0 degrees and R's apply-from-baseline residual dutifully spun
        // the selection back to where it started.  The comment that used to sit
        // here claimed that WAS the fix; it was, in shakeout D, when a free drag
        // ran before the grab and the accumulated degrees genuinely had to be
        // rolled back.  Shakeout G removed the free drag and left this behind.
        //
        // AFTER: the ring feeds a SWEEP SINCE THE GRAB and the command latches the
        // base it adds to (KiwiRotateCommand::HandleGrab, raised by the shared arm
        // below).  Frame one therefore feeds `base + 0` == the current angle, which
        // is a delta of exactly zero, and the 5° snap is held off until the sweep
        // is non-zero so that a re-grab cannot quantise a typed angle either.
        if ( !KiwiCmd_HandleGrab( imgX, imgY ) )
            return false;

        KiwiXform_PresetRotateAxis( axis );
        s_ringGrab  = axis;
        s_ringHot   = axis;
        s_ringPrev  = ang;
        s_ringTotal = 0.0f;
        PushRingAngle();                        // base + 0 sweep = no movement
        g_nUpdateBits |= 1;
        return true;
    }

    return false;
}

// ─── the KG_GIZMO gesture's per-move feed ────────────────────────────────────
void KiwiGizmo_Drag( int imgX, int imgY )
{
    if ( s_ringGrab < 0 )
        return;                                 // move handles are fed by the command

    if ( !KiwiXform_IsRotateActive() )
    {
        s_ringGrab = -1;                        // Esc mid-drag ended it under us
        return;
    }

    CamWnd_BuildMatrix();
    ringGeo_t r;
    if ( !BuildRings( &r ) )
        return;

    float ang = 0.0f;
    if ( !RingCursorAngle( r, s_ringGrab, imgX, imgY, &ang ) )
        return;                                 // keep the last good angle

    // Unwrap across the atan2 seam so a multi-turn drag keeps counting instead of
    // snapping 360 degrees the moment it crosses ±180.
    float d = ang - s_ringPrev;
    while ( d >  180.0f ) d -= 360.0f;
    while ( d < -180.0f ) d += 360.0f;
    s_ringTotal += d;
    s_ringPrev   = ang;

    PushRingAngle();
    g_nUpdateBits |= 1;
}

// ─── the viewport's release / abort edge ─────────────────────────────────────
bool KiwiGizmo_Grabbed()
{
    return s_grabbed != KGZ_NONE || s_ringGrab >= 0;
}

void KiwiGizmo_Release()
{
    if ( !KiwiGizmo_Grabbed() )
        return;
    const bool wasRing = ( s_ringGrab >= 0 );
    s_grabbed  = KGZ_NONE;
    s_ringGrab = -1;
    KiwiCmd_HandleRelease();                       // ROUND L: the shared gate edge
    if ( wasRing )
        KiwiXform_FeedRotateDegrees( false, 0.0f );   // R stops taking ring angles
    // KIWI-UX (shakeout E): a handle release PAUSES, it no longer COMMITS.
    // "Releasing an action shouldn't commit it, it should just pause the wip
    // move.  A right click OR an enter press confirms it."  The gizmo stays
    // drawn, the preview stays where the drag left it, and grabbing a handle
    // again resumes (KiwiGizmo_MouseDown's PAUSED → HOT edge).
    if ( KiwiCmd_Active() )                     // Esc mid-drag already ended it
        KiwiCmd_Pause();
}

void KiwiGizmo_Abort()
{
    if ( !KiwiGizmo_Grabbed() )
        return;
    const bool wasRing = ( s_ringGrab >= 0 );
    s_grabbed  = KGZ_NONE;
    s_ringGrab = -1;
    KiwiCmd_HandleRelease();                       // ROUND L: the shared gate edge
    if ( wasRing )
        KiwiXform_FeedRotateDegrees( false, 0.0f );
    if ( KiwiCmd_Active() )
        KiwiCmd_Cancel();
}

// ─── Cam_Draw tail hook ──────────────────────────────────────────────────────
void KiwiGizmo_DrawWorld()
{
    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;
    CamWnd_BuildMatrix();

    gizmoGeo_t g;
    if ( BuildGeo( &g ) )
    {
        GizmoDepthClear();

        // ── ROUND AK, ITEM 4: pass 0, THE FILLS ─────────────────────────────
        // Triangles, on their own render command and their own MATERIAL_COLOR
        // bracket, BEFORE the lines so every outline lands on top of its own fill
        // (kiwi_gizmo.cpp THE FILLS).  Emits nothing the line passes do not also
        // draw, and touches no hit test.
        DrawTranslateFills( g, c );

        // ── ROUND AJ, ITEM 4: pass 1, EVERYTHING, at Plasticity's 2 px ───────
        KiwiLines_Begin( KGIZMO_MAX_SEGMENTS, 2 );

        for ( int axis = 0; axis < 3; ++axis )
        {
            Colour( KGZ_AXIS_X + axis, axis );
            EmitAxis( g, c, axis );
        }

        // SHAKEOUT G: the FACE-NORMAL arrow, present only while Move is pushing
        // faces.  Amber, deliberately not an axis colour.
        if ( g.hasNormal )
        {
            if ( s_grabbed == KGZ_NORMAL || s_hot == KGZ_NORMAL )
                KiwiLines_Color( KGZ_HOT[0], KGZ_HOT[1], KGZ_HOT[2] );
            else
                KiwiLines_Color( KGZ_NORMAL_COL[0], KGZ_NORMAL_COL[1], KGZ_NORMAL_COL[2] );
            EmitNormalArrow( g, c );
        }

        for ( int n = 0; n < 3; ++n )
        {
            Colour( KGZ_PLANE_X + n, n );
            EmitPlane( g, n );
        }

        // The WHITE ORIGIN RING — the view-plane move.  Same handle KGZ_CENTER
        // always was (a free move on the latched view-normal plane); Plasticity
        // draws that handle as exactly this, a small billboarded white circle at
        // the origin (MoveGizmo.ts:118-133).
        Colour( KGZ_CENTER, -1 );
        EmitCentreRing( g, c );

        KiwiLines_Flush();

        // ── pass 2: the HOVERED handle again, at 3 px ───────────────────────
        // Plasticity's hover is a lighter tint AND a heavier line (2 -> 3,
        // GizmoMaterials.ts:33/37).  The tint is already in pass 1; the width
        // needs its own batch because KiwiLines_Begin takes it per BATCH.  Only
        // the ONE element under the cursor is re-emitted, so this costs at most a
        // handful of segments and only while something is actually hovered.
        const int emph = ( s_grabbed != KGZ_NONE ) ? s_grabbed : s_hot;
        if ( emph != KGZ_NONE )
        {
            KiwiLines_Begin( 32, 3 );
            if ( emph >= KGZ_AXIS_X && emph <= KGZ_AXIS_Z )
            {
                Colour( emph, emph - KGZ_AXIS_X );
                EmitAxis( g, c, emph - KGZ_AXIS_X );
            }
            else if ( emph >= KGZ_PLANE_X && emph <= KGZ_PLANE_Z )
            {
                Colour( emph, emph - KGZ_PLANE_X );
                EmitPlane( g, emph - KGZ_PLANE_X );
            }
            else if ( emph == KGZ_CENTER )
            {
                // #FFFFFF, Plasticity's hover white (default-theme.js:8) against
                // the #FAFAFA rest state.
                KiwiLines_Color( 1.0f, 1.0f, 1.0f );
                EmitCentreRing( g, c );
            }
            else if ( emph == KGZ_NORMAL && g.hasNormal )
            {
                KiwiLines_Color( KGZ_HOT[0], KGZ_HOT[1], KGZ_HOT[2] );
                EmitNormalArrow( g, c );
            }
            KiwiLines_Flush();
        }
        return;                                 // the two are mutually exclusive
    }

    // ── the three rotate rings, plus the white VIEW ring ────────────────────
    ringGeo_t r;
    if ( !BuildRings( &r ) )
        return;

    GizmoDepthClear();

    KiwiLines_Begin( KGIZMO_MAX_SEGMENTS, 2 );
    for ( int axis = 0; axis < 3; ++axis )
    {
        RingColour( axis );                     // one colour run per ring (kiwi_lines.h)
        float prev[3];
        RingPoint( r, axis, 0, prev );
        for ( int k = 1; k <= KGZ_RING_SEGMENTS; ++k )
        {
            float p[3];
            RingPoint( r, axis, k % KGZ_RING_SEGMENTS, p );
            // ROUND AJ, ITEM 4: drop the far half — see RingSegmentVisible.
            if ( RingSegmentVisible( r.pivot, c, prev, p ) )
            {
                if ( !KiwiLines_Add( prev, p ) )
                    break;
            }
            Copy3( p, prev );
        }
    }

    // ── ROUND AJ, ITEM 4: THE WHITE OUTER RING ──────────────────────────────
    // Plasticity's rotate gizmo closes with a white circle 14% outside the three
    // axis rings, billboarded to the camera (RotateGizmo.ts:40-41 + the inherited
    // `shouldLookAtCamera`).  With the back halves culled above, that ring is what
    // turns three arcs into a legible SPHERE.
    //
    // IT IS NOT GRABBABLE HERE, AND THAT IS AN HONEST GAP RATHER THAN AN OVERSIGHT.
    // In Plasticity it is the VIEW-AXIS rotation, and this editor has no such
    // rotation to wire it to: the ported core is Select_RotateAxis( axis, deg, … )
    // (select.cpp:2337), which takes an axis INDEX 0/1/2, and R's whole state is
    // one `m_axis` int (kiwi_transform.cpp:3240).  An arbitrary-axis rotate is a new
    // core, not a visual pass — so the ring is drawn as the silhouette it also is,
    // and the three axis rings keep every grab they had.  Logged in
    // RADIANT_KNOWN_ISSUES round AJ.
    {
        KiwiLines_Color( KGZ_CENTRE[0], KGZ_CENTRE[1], KGZ_CENTRE[2] );
        const float rad = r.radius * KGZ_VIEW_RING_MUL;
        float prev[3];
        for ( int k = 0; k <= KGZ_VIEW_RING_SEG; ++k )
        {
            const float th = ( 6.283185307179586f * (float)( k % KGZ_VIEW_RING_SEG ) )
                           / (float)KGZ_VIEW_RING_SEG;
            const float cs = cosf( th ) * rad;
            const float sn = sinf( th ) * rad;
            float p[3];
            for ( int a = 0; a < 3; ++a )
                p[a] = r.pivot[a] + c->vright[a] * cs + c->vup[a] * sn;
            if ( k > 0 && !KiwiLines_Add( prev, p ) )
                break;
            Copy3( p, prev );
        }
    }
    KiwiLines_Flush();
}

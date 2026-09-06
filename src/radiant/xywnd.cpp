#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\Radiant\XYWnd.cpp - the 2D (XY/XZ/YZ) editor views: ortho scene setup, grid,
// brush + overlay draw, and the CXYWnd mouse / clipper / context-menu input handling.
// XY_SetupScene -> R_Ed_SetSceneParms installs the ortho VP; the draws batch into the
// renderer's line/point/tri command buffers.

#include "stdafx.h"
#include "xywnd.h"
enum { YZ = ED_VIEW_YZ, XZ = ED_VIEW_XZ, XY = ED_VIEW_XY };   // the binary's bare view names (assert strings)
#include "prefs.h"                  // g_PrefsDlg (prefData_t* — real settings)
#include "mainfrm.h"                // CXYWnd / CMainFrame (First-Light window class)
#include <gfx_d3d/r_gfx.h>          // GfxMatrix, GfxColor, GfxPointVertex
#include <gfx_d3d/r_init.h>         // dx, R_InitRendererForWindow, R_SetupRendertarget_CheckDevice
#include <gfx_d3d/r_scene.h>        // R_Ed_SetSceneParms
#include <gfx_d3d/r_rendercmds.h>   // R_AddCmd_Line3D, R_BeginFrame/R_EndFrame, R_AddCmdClearScreen
#include "radiant_rtt.h"           // P5 RTT: RTT_Begin/RTT_End, RTT_XY
#include <universal/profile.h>
#include "kiwi_walkcache.h"        // the shared prefab-walk recording
#include "kiwi_viewdirty.h"        // KiwiViewDirty_Mark — re-arm on a bailed render
#include "kiwi_camera.h"           // KiwiCam_MarkerViewpoint — the camera icon's anchor
#include "kiwi_light.h"            // selected-light radius and cone overlays
#include <math.h>
#include <vector>                   // XY_ContextMenu Layers submenu — distinct layer-name set
#include <string>                   // (std::string / std::vector; <algorithm> already in stdafx)

// Editor globals defined elsewhere in the radiant target.
extern float grid_sizes[];                 // engine_stubs.cpp  (IDB 0x6dde5c)
extern float region_mins[3];               // map.cpp           (IDB 0x739c14)
extern float region_maxs[3];               // map.cpp           (IDB 0x739d24)
extern char  Byte4PackPixelColor(float *from, GfxColor *out);  // engine_stubs.cpp (IDB 0x402ac0)

extern void Radiant_FL_Log( const char *fmt, ... );    // mainfrm.cpp (First-Light trace log)

// U-GLOBALS: THE editor camera, shell-agnostic (camwnd.cpp).  Never NULL — it replaces every
// `g_pParentWnd->m_pCamWnd->camera` reach-through this file used to make.
extern camera_s *Ed_Camera();               // camwnd.cpp

extern int   g_bCrossHairs;                // engine_stubs.cpp  (IDB 0x25d5b06) — Shift+X toggle

// g_tdp (IDB 0x23f1634) — the snapped world point under the cursor. Maintained by
// CXYWnd::OnMouseMove (0x464b10, every non-RMB move) and consumed by DrawCrosshair
// (0x4655d0) and the binary's clip-point / point-file hover proximity tests.
float g_tdp[3] = { 0.0f, 0.0f, 0.0f };

extern void Draw_PatchSelectPoints();           // brush.cpp 0x40c250 (unselected candidates, green)
extern void Draw_PatchSelectPointsSelected();   // brush.cpp 0x40c360 (selected points, light blue)

// ─── the ONE XY viewport state block (U-VP-XY) ─────────────────────────────────────
// xywndState_t + Ed_ActiveXY()'s declaration live in xywnd.h (moved there by U-GLOBALS so the
// core TUs can reach the view state without CMainFrame/CXYWnd); the instance stays here.
xywndState_t g_xywndState;

xywndState_t *Ed_ActiveXY()
{
    return &g_xywndState;
}


// XY_SetupProjectionMtx - CXYWnd::SetupProjectionMtx 0x4a7980.  Axis-aligned ortho:
// x/y map [-w/2,w/2]x[-h/2,h/2] to clip [-1,1]; z maps [0,depth] to [0,0.5] (D3D depth range).
void XY_SetupProjectionMtx(GfxMatrix *mtx, float width, float height, float depth)
{
    iassert( mtx );
    iassert( width != 0 );
    iassert( height != 0 );
    iassert( depth != 0 );

    memset(mtx, 0, sizeof(GfxMatrix));
    mtx->m[0][0] = 2.0f / width;
    mtx->m[1][1] = 2.0f / height;
    mtx->m[2][2] = 0.5f / depth;
    mtx->m[3][2] = 0.5f;
    mtx->m[3][3] = 1.0f;
}

// XY_SetupScene - CXYWnd::SetupScene 0x5064c0 + the org/axis prologue of XY_Draw 0x46ce20.
// The 3x3 view axis is a permutation/sign pattern of the world axes chosen by the view plane
// (ED_VIEW_YZ=0 / XZ=1 / XY=2); the ortho projection is sized by the live target-window dims
// divided by the view scale, depth -262144.
// KISAK: R_Ed_ProjectionWouldBeValid is the defensive twin of Cam_SetupScene's guard - the
// axis-aligned ortho VP is structurally immune, so it never trips for the 2D views.
bool XY_SetupScene(const XYViewState *v)
{
    iassert( v->scale != 0 );

    const int nDim1 = (v->viewType == ED_VIEW_YZ);     // horizontal world axis
    const int nDim2 = (v->viewType != ED_VIEW_XY) + 1; // vertical world axis
    const int nDim3 = 3 - nDim2 - nDim1;               // depth (out-of-screen) axis

    float axis[3][3];
    memset(axis, 0, sizeof(axis));
    axis[0][nDim3] =  1.0f;   // view forward = +depth world axis
    axis[1][nDim1] = -1.0f;   // view right   = -horizontal world axis
    axis[2][nDim2] =  1.0f;   // view up      = +vertical world axis

    float org[3] = { v->origin[0], v->origin[1], v->origin[2] };

    // Ortho size from the VIEW's own pixel dims (v->width/height = the RTT cell), NOT
    // dx.windows[targetWindowIndex] — under RTT the target index is a fixed host slot whose
    // swap-chain size is unrelated to this viewport's cell, which stretched the grid. The
    // view dims already equal the window dims on the legacy path, so this is correct there too.
    const float invScale = 1.0f / v->scale;
    const float projW = invScale * (float)v->width;
    const float projH = (float)v->height * invScale;

    GfxMatrix proj;
    XY_SetupProjectionMtx(&proj, projW, projH, -262144.0f);

    if ( !R_Ed_ProjectionWouldBeValid( org, (const float(*)[3])axis, &proj ) )
    {
        static int s_xyProjDropped = 0;
        s_xyProjDropped++;
        if ( ( s_xyProjDropped & ( s_xyProjDropped - 1 ) ) == 0 )
            Radiant_FL_Log( "XY_SetupScene: dropped degenerate-projection frame #%d "
                            "(viewType=%d org=%.0f,%.0f,%.0f scale=%.5g) -- no begin-view submitted",
                            s_xyProjDropped, v->viewType, org[0], org[1], org[2], v->scale );
        return false;
    }

    R_Ed_SetSceneParms(org, (const float(*)[3])axis, &proj);
    return true;
}

// ─── grid line emitter ───────────────────────────────────────────────────────
// One line = two GfxPointVertex. The view-plane axes map to the vertex xyz slots:
// horizontal -> xyz[nDim1], vertical -> xyz[nDim2], depth -> xyz[nDim3] (constant).
static const int   ED_GRID_MAX_VERTS = 8192;   // 4096 lines; adaptive step keeps the real count ~hundreds
static const float ED_GRID_DEPTH     = -131072.0f;

static inline void edGridLine(GfxPointVertex *verts, int &n, int d1, int d2, int d3,
                              float aH, float aV, float bH, float bV, unsigned int packedColor)
{
    if (n + 2 > ED_GRID_MAX_VERTS)
        return;
    GfxPointVertex *v0 = &verts[n];
    GfxPointVertex *v1 = &verts[n + 1];
    v0->xyz[d1] = aH; v0->xyz[d2] = aV; v0->xyz[d3] = ED_GRID_DEPTH;
    v1->xyz[d1] = bH; v1->xyz[d2] = bV; v1->xyz[d3] = ED_GRID_DEPTH;
    *(unsigned int *)v0->color = packedColor;
    *(unsigned int *)v1->color = packedColor;
    n += 2;
}

// Coordinate / view-name text (IDB XY_DrawGrid tail 0x4686a0).  The editor font is registered
// by R_BeginRegistrationInternal (fonts/qerfont -> g_qeglobals.d_font_list).  Text origin is a
// world position; xPixelStep/yPixelStep are the per-text-pixel world basis vectors, so glyphs
// are screen-aligned at the view scale.  The top X ruler is drawn rotated (xPixelStep along
// -nDim2), the left Y ruler horizontal.

static void XY_DrawCoordText(const XYViewState *wnd,
                             float xmin, float xmax, float ymin, float ymax,
                             int stepSize, int nDim1, int nDim2, int nDim3,
                             float w, float h)
{
    Font_s *font = (Font_s *)g_qeglobals.d_font_list;
    if (!font)
        return;

    const float inv   = 1.0f / wnd->scale;
    const float halfX = (wnd->width  & 1) ? 0.0f : 0.5f * inv;   // even dim → half-texel nudge
    const float halfY = (wnd->height & 1) ? 0.0f : 0.5f * inv;
    float *colText = g_qeglobals.d_savedinfo.colors[8];

    char  text[32];
    float a4[3], yp[3], org[3];

    // ── KIWI-UX DEVIATION (shakeout B) — RULER LABEL DENSITY CAP ─────────────
    // USER DIRECTIVE: "try to cleanup the clutter when zoomed out (deviate from
    // original in this regard)".  The ruler steps with the MAJOR grid, and the major
    // step is chosen as the next power of two >= 6.4/scale (XY_DrawGrid) — so its
    // on-screen spacing bottoms out around 6-13 px while the numbers it prints GROW
    // to 5-6 characters.  The result at far zoom is two solid smears of digits along
    // the top and left edges.  Fix: double the LABEL step (never the line step) until
    // consecutive labels are at least 40 px apart.
    // Identical at classic zoom: the stock scale 1.0 gives stepSize 64 -> 64 px, well
    // over the threshold, so the loop below does not run and the output is unchanged.
    // Anything at or above scale 0.625 is likewise untouched.
    int labelStep = stepSize;
    while ( (float)labelStep * wnd->scale < 40.0f && labelStep < ( 1 << 24 ) )
        labelStep *= 2;

    // TOP labels — X coords along the top edge (rotated: xPixelStep along -nDim2).
    a4[0]=a4[1]=a4[2]=0.0f; a4[nDim2] = -inv;
    yp[0]=yp[1]=yp[2]=0.0f; yp[nDim1] = -inv;
    const float topV = wnd->origin[nDim2] + h - inv;
    // KIWI-UX DEVIATION: `labelStep` in place of `stepSize` here and in the LEFT loop
    // below.  They are the SAME VALUE except when the cap above widened it, and the
    // start point is still xmin, so the label positions stay on the original lattice.
    for (float v = xmin; v < xmax; v += (float)labelStep)
    {
        sprintf(text, "%.0f", v);
        org[nDim1] = v + halfX;
        org[nDim2] = topV + halfY;
        org[nDim3] = 0.0f;
        R_AddCmdDrawTextAtPosition(text, font, org, a4, yp, colText);
    }

    // LEFT labels — Y coords down the left edge (xPixelStep along +nDim1).
    a4[0]=a4[1]=a4[2]=0.0f; a4[nDim1] =  inv;
    yp[0]=yp[1]=yp[2]=0.0f; yp[nDim2] = -inv;
    const float leftV = wnd->origin[nDim1] - w + inv;
    for (float v = ymin; v < ymax; v += (float)labelStep)   // KIWI-UX DEVIATION (see above)
    {
        sprintf(text, "%.0f", v);
        org[nDim1] = leftV + halfX;
        org[nDim2] = (v + inv) + halfY;
        org[nDim3] = 0.0f;
        R_AddCmdDrawTextAtPosition(text, font, org, a4, yp, colText);
    }

    // View-name hint (top-left); brighter (slot 13) when the view is active.
    const char *name = (wnd->viewType == ED_VIEW_XY) ? "XY Top"
                     : (wnd->viewType == ED_VIEW_XZ) ? "XZ Front" : "YZ Side";
    org[nDim1] = (wnd->origin[nDim1] - w + 35.0f * inv) + halfX;
    org[nDim2] = (wnd->origin[nDim2] + h - 20.0f * inv) + halfY;
    org[nDim3] = 0.0f;
    R_AddCmdDrawTextAtPosition(name, font, org, a4, yp,
        g_qeglobals.d_savedinfo.colors[wnd->active ? 13 : 8]);
}

// XY_DrawGrid - CXYWnd::XY_DrawGrid 0x4686a0: grid lines + coordinate/view-name text.
// KISAK: the minor/major MATERIAL-COLOR batching diverges from the binary's single
// per-vertex-colour buffer - the port's $line technique ignores vertex colour.
void XY_DrawGrid(const XYViewState *wnd)
{
    const int nDim1 = (wnd->viewType == ED_VIEW_YZ);
    const int nDim2 = (wnd->viewType != ED_VIEW_XY) + 1;
    const int nDim3 = 3 - nDim2 - nDim1;

    const float w = (float)(wnd->width  / 2) / wnd->scale;   // half view-width  (world units)
    const float h = (float)(wnd->height / 2) / wnd->scale;   // half view-height (world units)

    // Visible grid extents, clamped to the region and snapped to 64-unit blocks.
    float xmin = wnd->origin[nDim1] - w;
    if (region_mins[nDim1] > xmin) xmin = region_mins[nDim1];
    xmin = floorf(xmin * 0.015625f) * 64.0f;

    float xmax = wnd->origin[nDim1] + w;
    if (region_maxs[nDim1] < xmax) xmax = region_maxs[nDim1];
    xmax = ceilf(xmax * 0.015625f) * 64.0f;

    float ymin = wnd->origin[nDim2] - h;
    if (region_mins[nDim2] > ymin) ymin = region_mins[nDim2];
    ymin = floorf(ymin * 0.015625f) * 64.0f;

    float ymax = wnd->origin[nDim2] + h;
    if (region_maxs[nDim2] < ymax) ymax = region_maxs[nDim2];
    ymax = ceilf(ymax * 0.015625f) * 64.0f;

    // Major-grid step: next power of two >= 6.4/scale, but never finer than 64.
    int stepSize;
    int wantStep = (int)(6.4f / wnd->scale);
    if (wantStep >= 64)
    {
        int i;
        for (i = 1; i < wantStep; i *= 2)
            ;
        stepSize = i;
    }
    else
    {
        stepSize = 64;
    }

  if (g_qeglobals.d_showgrid)
  {
    static GfxPointVertex verts[ED_GRID_MAX_VERTS];

    float *colMinor = g_qeglobals.d_savedinfo.colors[2];
    float *colBack  = g_qeglobals.d_savedinfo.colors[1];
    float *colMajor = g_qeglobals.d_savedinfo.colors[3];

    // The $line material takes its colour from CODE_MATERIAL_COLOR, not the vertex colour,
    // so each grid colour needs its own R_AddCmdSetMaterialColor batch.
    extern void R_AddCmdSetMaterialColor( const float *rgba );   // r_rendercmds.cpp

    // Minor grid (snap-size lines) — only when zoomed in enough to be legible and
    // the minor colour differs from the background.
    // KIWI-UX (shakeout B): the "minor lines need >= 4 px of spacing" rule the
    // declutter pass called for ALREADY EXISTS here, as `minorStep * scale >= 4.0f`
    // below (plus the scale > 0.1 floor).  Extended, not duplicated — nothing to add.
    const float minorStep = grid_sizes[g_qeglobals.d_gridsize];
    const bool minorSameAsBack =
        colMinor[0] == colBack[0] && colMinor[1] == colBack[1] &&
        colMinor[2] == colBack[2] && colMinor[3] == colBack[3];
    if (wnd->scale > 0.1f && minorStep * wnd->scale >= 4.0f && !minorSameAsBack)
    {
        GfxColor col;
        Byte4PackPixelColor(colMinor, &col);
        int n = 0;
        for (float x = xmin; x < xmax; x += minorStep)
            edGridLine(verts, n, nDim1, nDim2, nDim3, x, ymin, x, ymax, col.packed);
        for (float y = ymin; y < ymax; y += minorStep)
            edGridLine(verts, n, nDim1, nDim2, nDim3, xmin, y, xmax, y, col.packed);
        if (n > 0)
        {
            R_AddCmdSetMaterialColor(colMinor);
            R_AddCmd_Line3D((short)(n / 2), 1, verts);
        }
    }

    // Major grid (block lines).
    {
        GfxColor col;
        Byte4PackPixelColor(colMajor, &col);
        int n = 0;
        for (float x = xmin; x <= xmax; x += stepSize)
            edGridLine(verts, n, nDim1, nDim2, nDim3, x, ymin, x, ymax, col.packed);
        for (float y = ymin; y <= ymax; y += stepSize)
            edGridLine(verts, n, nDim1, nDim2, nDim3, xmin, y, xmax, y, col.packed);
        if (n > 0)
        {
            R_AddCmdSetMaterialColor(colMajor);
            R_AddCmd_Line3D((short)(n / 2), 1, verts);
        }
    }

    static const float s_edWhite[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    R_AddCmdSetMaterialColor(s_edWhite);   // restore for the brush/overlay passes that follow
  } // d_showgrid

    // Coordinate labels + view-name (IDB XY_DrawGrid text block); xyShowFlags 0x20 hides them,
    // independently of the grid lines.
    if ((g_qeglobals.d_savedinfo.d_xyShowFlags & 0x20) == 0)
        XY_DrawCoordText(wnd, xmin, xmax, ymin, ymax, stepSize, nDim1, nDim2, nDim3, w, h);
}

// DrawZIcon (IDB 0x469d50) - the XY "player indicator": a translucent-blue 16x16 fill quad
// plus a box outline with a "Z", at the m_Camera marker position.
// KISAK: depth is ED_GRID_DEPTH(-131072) vs the binary's +131072 (cosmetic sign flip, kept so
// the fill stays coplanar with the port's outline - see DrawCameraIcon).
// The XY camera-icon origin (IDB m_Camera.origin0/1 @ 0x241a5a4/0x241a5a8) - written by the
// Shift+click camera-set dispatch below; NOT the live CCamWnd fly-cam origin (a separate value
// moved by WASD/Ctrl+click).  Defined here because DrawZIcon reads it.
static float m_Camera_origin0 = 0.0f;
static float m_Camera_origin1 = 0.0f;
extern void __cdecl R_AddRenderCmdDrawTris(                       // (also declared near DrawSelectionBox)
    Material *material, MaterialTechniqueType techType, short indexCount,
    const uint16_t *indices, short vertexCount,
    const float (*xyzw)[4], const float (*normal)[3], float *color,
    const float (*st)[2] );
static void DrawZIcon( const XYViewState *wnd )
{
    if ( wnd->viewType != ED_VIEW_XY )       // the marker is an XY-plane (top-view) concept
        return;
    const float cx = m_Camera_origin0;       // IDB m_Camera.origin0 (Shift+click marker), NOT the fly-cam
    const float cy = m_Camera_origin1;

    GfxColor col;
    static float s_blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };   // flt_6DE160 (camera-icon blue)
    Byte4PackPixelColor( s_blue, &col );

    // Translucent blue FILL quad (IDB 0x469d6e), drawn before the outline.
    {
        const float z = ED_GRID_DEPTH;
        float quad[4][4] = {
            { cx - 8, cy - 8, z, 1.0f },
            { cx + 8, cy - 8, z, 1.0f },
            { cx + 8, cy + 8, z, 1.0f },
            { cx - 8, cy + 8, z, 1.0f },
        };
        float normal[4][3] = { {0,0,1}, {0,0,1}, {0,0,1}, {0,0,1} };
        float st[4][2]     = { {0,0}, {1,0}, {1,1}, {0,1} };   // IDB xyzw[28..35]
        static const uint16_t fillIdx[6] = { 1, 0, 2, 2, 0, 3 };   // IDB indices[0..5]

        GfxColor fillCol;
        Byte4PackPixelColor( s_blue, &fillCol );
        fillCol.array[3] = 64;                                 // IDB: v13.array[3] = 64 (alpha)
        float colorRepl = *(float *)&fillCol.packed;
        float color[4] = { colorRepl, colorRepl, colorRepl, colorRepl };

        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT, 6, fillIdx, 4,
                                quad, normal, color, st );
    }

    GfxPointVertex verts[ 14 ];
    int n = 0;
    // 16×16 box outline.
    edGridLine( verts, n, 0, 1, 2, cx - 8, cy - 8, cx + 8, cy - 8, col.packed );
    edGridLine( verts, n, 0, 1, 2, cx + 8, cy - 8, cx + 8, cy + 8, col.packed );
    edGridLine( verts, n, 0, 1, 2, cx + 8, cy + 8, cx - 8, cy + 8, col.packed );
    edGridLine( verts, n, 0, 1, 2, cx - 8, cy + 8, cx - 8, cy - 8, col.packed );
    // the "Z" inside.
    edGridLine( verts, n, 0, 1, 2, cx - 4, cy + 4, cx + 4, cy + 4, col.packed );
    edGridLine( verts, n, 0, 1, 2, cx + 4, cy + 4, cx - 4, cy - 4, col.packed );
    edGridLine( verts, n, 0, 1, 2, cx - 4, cy - 4, cx + 4, cy - 4, col.packed );
    R_AddCmd_Line3D( (short)( n / 2 ), 1, verts );
}

// DrawCameraIcon - CXYWnd::DrawCameraIcon 0x469a40.  The 3D camera's position+facing marker in
// a 2D view: a 4-corner diamond body + a diagonal + a 2-line FOV cone along the facing angle
// (yaw for XY, pitch for XZ/YZ); drawn in all three views, in-plane axes chosen per view.
// KISAK: out-of-plane depth uses ED_GRID_DEPTH(-131072) vs the binary's +131072 (cosmetic).
// No null-guard, matching the binary - reached only post-init from OnPaint.
//
// ── KIWI-UX DEVIATION (shakeout B) — RED, FIXED SCREEN SIZE ────────────────────
// USER DIRECTIVE (verbatim): "the 2D xy view gets unusable when zoomed out too far
// and it's impossible to tell where the camera is.  Make the camera bigger, a fixed
// size with zoom, and red."
//
// The binary draws this glyph in WORLD units (16 / 8 / 48), so at scale 0.02 the whole
// marker is under two pixels across — invisible, which is precisely the complaint.
// Every extent below is multiplied by `px` = 1/scale, i.e. it is specified in SCREEN
// PIXELS and converted to world units for the ortho projection.  The SHAPE is
// unchanged: the same 4-corner diamond, the same diagonal, the same 2-line FOV wedge
// along the same facing angle.
//
// ── ROUND M: HALVED ─────────────────────────────────────────────────────────
// USER DIRECTIVE, verbatim: "The red camera indicator is too big in the 2d view,
// make it 50% smaller."  Shakeout B multiplied the binary's 16/8/48 by 1.5 to get
// 24/12/64 screen px; this round halves those to 12/6/32.  The SCREEN-CONSTANT
// property — the actual content of shakeout B's fix, and the reason the marker is
// findable at all when zoomed out — is untouched: only the three literals move.
// At 12x6 px the diamond is still four times the area it had at scale 0.02 under
// the binary's world-unit sizing, so nothing regresses toward "impossible to tell
// where the camera is".
// The colour moves from the binary's flt_6DE160 blue to red; DrawZIcon keeps the blue,
// so the two markers are now told apart at a glance instead of being the same colour.
// Drawn ON TOP of the map: the XY paint calls DrawCameraIcon AFTER XY_DrawBrushes and
// the turret highlights (xywnd.cpp:3961-3962), and the 2D views are depth-test-free
// ortho, so emission order IS paint order.  Draw ORDER is not changed by this deviation
// — the icon keeps its existing slot, just before DrawZIcon.
static void DrawCameraIcon( const XYViewState *wnd )
{
    auto &cam = *Ed_Camera();      // U-GLOBALS: was g_pParentWnd->m_pCamWnd->camera
    int   v2, v5;                  // in-plane vertical / horizontal axis indices
    float ang;
    if ( wnd->viewType == ED_VIEW_XY ) { v2 = 1; v5 = 0; ang = cam.angles[1]; }   // XY → yaw
    else if ( wnd->viewType )          { v2 = 2; v5 = 0; ang = cam.angles[0]; }   // XZ → pitch
    else                               { v2 = 2; v5 = 1; ang = cam.angles[0]; }   // YZ → pitch
    const int   d3  = 3 - v2 - v5;                                 // out-of-plane axis
    const float rad = DEG2RAD( ( ang + 45.0f ) );
    const float c   = (float)cos( rad );
    const float s   = (float)sin( rad );
    // ── KIWI-UX DEVIATION: THE ANCHOR IS THE VIEWPOINT, NOT camera.origin ─────
    // USER REPORT, verbatim: "there is a bug where the camera is warped when it
    // updates the visual position."  In the default ORTHO projection camera.origin
    // is a PSEUDO-eye that the dolly re-seats at `pivot - forward * s_dist` on
    // every notch, with s_dist as the ZOOM (1 .. 262144) — so the marker was
    // translated across this view by the zoom, along the camera's own facing, for
    // a camera that had not moved.  KiwiCam_MarkerViewpoint (kiwi_camera.h) is
    // that anchor done once; in perspective it is camera.origin unchanged.
    float eye[3];
    KiwiCam_MarkerViewpoint( eye );
    const float oh  = eye[v5];                                    // in-plane horizontal
    const float ov  = eye[v2];                                    // in-plane vertical

    // KIWI-UX DEVIATION: world units per screen pixel.  wnd->scale is guarded non-zero
    // by XY_SetupScene's iassert and clamped to [0.01, 32] by the wheel handler
    // (xywnd.cpp:3103-3105), so this cannot divide by zero.
    const float px    = 1.0f / wnd->scale;
    const float bodyH = 12.0f * px;      // ROUND M: was 24 screen px (shakeout B), binary 16 world
    const float bodyV =  6.0f * px;      // ROUND M: was 12,  binary 8
    const float wedge = 32.0f * px;      // ROUND M: was 64,  binary 48

    GfxColor col;
    static float s_camRed[4] = { 1.0f, 0.0f, 0.0f, 1.0f };        // KIWI-UX DEVIATION (was flt_6DE160 blue)
    Byte4PackPixelColor( s_camRed, &col );

    GfxPointVertex verts[16];
    int n = 0;
    // diamond body (corners: -bodyH / +bodyV / +bodyH / -bodyV) then the -h→+h diagonal
    edGridLine( verts, n, v5, v2, d3, oh - bodyH, ov,         oh,         ov + bodyV, col.packed );
    edGridLine( verts, n, v5, v2, d3, oh,         ov + bodyV, oh + bodyH, ov,         col.packed );
    edGridLine( verts, n, v5, v2, d3, oh + bodyH, ov,         oh,         ov - bodyV, col.packed );
    edGridLine( verts, n, v5, v2, d3, oh,         ov - bodyV, oh - bodyH, ov,         col.packed );
    edGridLine( verts, n, v5, v2, d3, oh - bodyH, ov,         oh + bodyH, ov,         col.packed );
    // FOV cone: two lines from the origin along the facing angle (±)
    edGridLine( verts, n, v5, v2, d3, oh + c * wedge, ov + s * wedge, oh, ov, col.packed );
    edGridLine( verts, n, v5, v2, d3, oh + s * wedge, ov - c * wedge, oh, ov, col.packed );

    // KIWI-UX DEVIATION: width 2 (the binary passes 1) — a one-pixel red wireframe was
    // still easy to lose against a dense wireframe map.  Same call, same vert layout.
    R_AddCmd_Line3D( (short)( n / 2 ), 2, verts );
}

// XY_DrawBlockGrid - CXYWnd::XY_DrawBlockGrid 0x4690f0.  View->Show->Blocks (d_xyShowFlags bit
// 0x10 SET = blocks HIDDEN): the QE4 1024-unit block subdivision (colour slot 7) plus a
// "bx,by" index label at each block centre.  Labels use +131072 depth (matches the binary);
// the lines go through edGridLine's -131072 (the cosmetic depth-sign divergence).
//
// ── KIWI-UX DEVIATION (shakeout B) — ZOOM-BASED SUPPRESSION ───────────────────
// USER DIRECTIVE (verbatim): "Also try to cleanup the clutter when zoomed out
// (deviate from original in this regard)".  The binary draws every block line and a
// "bx,by" label at every block CENTRE regardless of zoom, so once a 1024-unit block is
// a few pixels wide the whole view is a solid mat of overlapping blue text with the map
// somewhere underneath.  Two thresholds, both expressed in SCREEN PIXELS PER BLOCK
// (= 1024 * scale), both of which only ever REMOVE things at far zoom:
//     labels  need >= 48 px per block   (scale >= 0.0469)
//     lines   need >= 12 px per block   (scale >= 0.0117)
// At the classic zoom levels nothing changes: even at scale 0.25 a block is 256 px, so
// the first threshold a user meets going out is the label cut at ~21x zoom-out.  The
// existing View->Show->Blocks toggle (d_xyShowFlags 0x10) is untouched and still wins.
void XY_DrawBlockGrid(const XYViewState *wnd)
{
    // KIWI-UX DEVIATION: the whole block grid is dropped when it is pure noise.
    const float blockPx = 1024.0f * wnd->scale;
    if ( blockPx < 12.0f )
        return;

    const int nDim1 = (wnd->viewType == ED_VIEW_YZ);     // horizontal world axis (v54)
    const int nDim2 = (wnd->viewType != ED_VIEW_XY) + 1; // vertical world axis   (v7)
    const int nDim3 = 3 - nDim2 - nDim1;                 // depth (out-of-screen)  (v8)

    const float halfW = (float)(int)((double)wnd->width  * (0.5 / wnd->scale));  // packed
    const float halfH = (float)(int)((double)wnd->height * (0.5 / wnd->scale));  // v50

    // Visible block extents on each 2D axis, clamped to the region, snapped to 1024.
    float hMin = wnd->origin[nDim1] - halfW;
    if (region_mins[nDim1] > hMin) hMin = region_mins[nDim1];
    hMin = floorf(hMin * 0.0009765625f) * 1024.0f;                 // floor(x/1024)*1024

    float hMax = wnd->origin[nDim1] + halfW;
    if (region_maxs[nDim1] < hMax) hMax = region_maxs[nDim1];
    hMax = ceilf(hMax * 0.0009765625f) * 1024.0f;                  // ceil(x/1024)*1024

    float vMin = wnd->origin[nDim2] - halfH;
    if (region_mins[nDim2] > vMin) vMin = region_mins[nDim2];
    vMin = floorf(vMin * 0.0009765625f) * 1024.0f;

    float vMax = wnd->origin[nDim2] + halfH;
    if (region_maxs[nDim2] < vMax) vMax = region_maxs[nDim2];
    vMax = ceilf(vMax * 0.0009765625f) * 1024.0f;

    static GfxPointVertex verts[ED_GRID_MAX_VERTS];
    int vertCount = 0;

    GfxColor col;
    Byte4PackPixelColor(g_qeglobals.d_savedinfo.colors[7], &col);

    // Vertical block lines: x = hMin..hMax step 1024, spanning [vMin..vMax].
    for (float h = hMin; h <= hMax; h += 1024.0f)
        edGridLine(verts, vertCount, nDim1, nDim2, nDim3, h, vMin, h, vMax, col.packed);
    // Horizontal block lines: y = vMin..vMax step 1024, spanning [hMin..hMax].
    for (float v = vMin; v <= vMax; v += 1024.0f)
        edGridLine(verts, vertCount, nDim1, nDim2, nDim3, hMin, v, hMax, v, col.packed);

    if (vertCount > 0)
        R_AddCmd_Line3D((short)(vertCount / 2), 2, verts);

    // Block-index labels at each block centre (IDB text loop 0x4694da+): bx/by come from the
    // block START coord, not the +512 centre the label is POSITIONED at.  The binary rounds
    // with inline fistp (0x46950e/0x469521) - round-to-nearest, NOT the (int) truncation
    // hex-rays prints.  PRECISION TRAP: the binary stores h/1024 to a FLOAT slot, then
    // subtracts dbl_6F4178 = 0.4999999990686774 in DOUBLE before the fistp.  Written as a
    // float literal that constant collapses to exactly 0.5f, which turns every exact block
    // multiple into a round-half-to-even TIE (odd block indices came out one too low) - so
    // the subtraction must stay in double.
    Font_s *font = (Font_s *)g_qeglobals.d_font_list;
    if (!font)
        return;

    // KIWI-UX DEVIATION (see the header note): the "bx,by" labels are the worst of the
    // zoomed-out clutter — one per block, ~30 px wide whatever the zoom.  Below 48 px
    // per block they overlap each other and everything under them, so drop them and
    // keep the lines.
    if ( blockPx < 48.0f )
        return;

    const float inv   = 1.0f / wnd->scale;
    const float halfX = (wnd->width  & 1) ? 0.0f : 0.5f * inv;
    const float halfY = (wnd->height & 1) ? 0.0f : 0.5f * inv;
    float *colText = g_qeglobals.d_savedinfo.colors[7];

    float a4[3], yp[3], org[3];
    a4[0]=a4[1]=a4[2]=0.0f; a4[nDim1] =  inv;          // xPixelStep
    yp[0]=yp[1]=yp[2]=0.0f; yp[nDim2] = -inv;          // yPixelStep
    char text[32];

    for (float h = hMin; h < hMax; h += 1024.0f)
    {
        const int bx = (int)lrint((double)(h * 0.0009765625f) - 0.4999999990686774);
        for (float v = vMin; v < vMax; v += 1024.0f)
        {
            const int by = (int)lrint((double)(v * 0.0009765625f) - 0.4999999990686774);
            sprintf(text, "%i,%i", bx, by);
            org[nDim1] = (h + 512.0f) + halfX;
            org[nDim2] = (v + 512.0f) + halfY;
            org[nDim3] = 131072.0f;
            R_AddCmdDrawTextAtPosition(text, font, org, a4, yp, colText);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// XY_DrawBrushes - the brush + overlay body of CXYWnd::XY_Draw 0x46ce20: cull each brush
// against the view rect, FilterBrush it, DrawBrush its wireframe (technique 29, width 1), then
// the entity-name label and the per-classtype radius overlay; selected brushes draw on top.
// ─────────────────────────────────────────────────────────────────────────────
extern selbrush_t active_brushes;                                     // map.cpp (0x23F189C)
extern selbrush_t selected_brushes;                                   // engine_stubs (0x23F1864)
extern float      world_orient_matrix[4][3];                          // entity.cpp (identity)
extern char       FilterBrush(selbrush_t *b, int fastDrag);           // filters.cpp (0x46A1F0)
extern entity_s  *world_entity;                                       // engine_stubs.cpp (0x25D5B30)
extern char      *ValueForKey2(int e, const char *key);               // entity.cpp (0x4825C0)
extern entity_s   entityInsts;                                        // entity.cpp (0x23F1748) — entity INSTANCE list head
extern bool       HasKeyValuePair(entity_s_def *e, const char *key);  // entity.cpp (0x4838B0)
extern float      Vec3Normalize_R(float *v);                          // engine_stubs.cpp (0x40A5E0) — returns length
extern void       KiwiEntArrow_DrawXY( int viewType, float scale );   // KIWI-UX: Declare the bounded selected-entity 2D overlay.
extern void       KiwiRefImage_DrawXY( int viewType, float scale );   // KIWI (REFIMG): editor-only material quads.
extern int        R_Add3DLine(GfxPointVertex *verts, const orientation_t *orient,
                              const float *p1, const float *p2, const unsigned int *color,
                              char width, int vertCount, int maxVertCount);  // draw.cpp (0x40C110)
void DrawBrush(selbrush_t *b, const orientation_t *orient, int viewType,
               int technique, GfxColor *col, char width, int drawFlags,
               const char *layerPrefix);                                 // brush.cpp (0x47afc0)


// sub_40CD40 (0x40cd40) - the text-origin HALF-PIXEL SNAP the binary applies before emitting a
// label: nudge the two IN-PLANE components (a8 = horizontal, a4 = vertical) of `src` by
// +0.5/scale when the matching viewport dimension is EVEN; the DEPTH component (a2) is copied
// unchanged.  Register binding from the call-site disasm: out@eax, a2@edx, src@ecx, a4@esi.
static void Ed_TextHalfPixelSnap( float *out, int a2, const float *src, int a4,
                                  float scale, int width, int height, int a8 )
{
    const float vx = ( width  & 1 ) ? 0.0f : 0.5f / scale;   // horizontal (a8) half-pixel
    const float vy = ( height & 1 ) ? 0.0f : 0.5f / scale;   // vertical   (a4) half-pixel
    out[a8] = src[a8] + vx;
    out[a4] = src[a4] + vy;
    out[a2] = src[a2];                                       // depth: unchanged
}

// The label-eligibility predicate (DrawBrushEntityName's gate minus the view cull and the
// names-shown flag): non-world owner, the brush is the owner's REPRESENTATIVE def-brush
// (def->brushes.prev == IDA brushes.oprev @+0x0C), not hidden (brushFlags & 2), non-empty
// "classname".  Shared by the draw and the headless oracle so they cannot drift.
extern void Assert( const char *file, int line, int type, const char *fmt, ... ); // (also declared below)

static const char *Ed_EntityNameForBrush( selbrush_t *brush )
{
    entity_s *owner = brush ? brush->owner : nullptr;
    if ( !owner || owner == world_entity )
        return nullptr;
    entity_s_def *def = (entity_s_def *)owner->def;
    if ( !def )
        return nullptr;
    brush_t *repBrush = (brush_t *)def->brushes.prev;   // IDA brushes.oprev (+0x0C)
    if ( brush->def != repBrush || ( brush->brushFlags & 2 ) != 0 )
        return nullptr;
    const char *name = ValueForKey2( (int)(intptr_t)def, "classname" );
    return ( name && *name ) ? name : nullptr;
}

// Ed_DrawBrushEntityName - DrawBrushEntityName 0x46c880.  Draws one non-world entity's
// "classname" at its def mins + 4 on the two in-plane axes (depth 131072), but only for the
// entity's REPRESENTATIVE def-brush (def->brushes.prev, IDA brushes.oprev +0x0C) so a
// multi-brush func_* entity is labelled exactly once.  Called per-brush from BOTH XY_Draw
// loops (active 0x46d0fe and selected 0x46d2c9).  Gated on (d_xyShowFlags & 8) == 0.
// KISAK: the binary passes its caller's `color[4]` local, which in the not-selected loop is
// UNINITIALISED stack (the source's "// flickering" comment); we pass savedinfo
// COLOR_GRIDTEXT slot 8 for both loops.
static void Ed_DrawBrushEntityName( int nView, selbrush_t *b, float scale,
                                    int vWidth, int vHeight, const float *textColor )
{
    iassert( nView == XY || nView == XZ || nView == YZ );   // XYWnd.cpp:3591

    Font_s *font = (Font_s *)g_qeglobals.d_font_list;
    if ( !font )
        return;
    if ( ( g_qeglobals.d_savedinfo.d_xyShowFlags & 8 ) != 0 )   // 0x8 = View→Show→Names hidden
        return;

    // IDB 0x46c8c5: owner-consistency for ANY non-world b (before the representative gate).
    entity_s *nameOwner = b ? b->owner : nullptr;
    if ( nameOwner && nameOwner != world_entity )
        iassert( b->owner->def == b->def->owner );   // XYWnd.cpp:3602

    const char *name = Ed_EntityNameForBrush( b );          // representative-b gate
    if ( !name )
        return;

    iassert( b->owner->def == b->def->owner );   // XYWnd.cpp:3609 (0x46c8fe)

    // Axis layout — the binary's axis_1(horiz/xstep) / axis_2(vert/ystep) / axis_3(depth),
    // with the CoD4Radiant enum ED_VIEW_YZ=0 / XZ=1 / XY=2:
    //   axis_1 = (nView==0=YZ)            ; axis_2 = (nView!=2=notXY)+1 ; axis_3 = 3-a2-a1.
    //   XY(2): h=0(X) v=1(Y) d=2(Z) ; XZ(1): h=0(X) v=2(Z) d=1(Y) ; YZ(0): h=1(Y) v=2(Z) d=0(X).
    // Identical to XY_DrawBrushes' nDim1/nDim2/nDim3.
    const int axisH = ( nView == ED_VIEW_YZ );        // (nView==0)
    const int axisV = ( nView != ED_VIEW_XY ) + 1;    // (nView!=2)+1
    const int axisD = 3 - axisV - axisH;
    brush_t *bd = b->def;

    float src[3];
    src[axisH] = bd->mins[axisH] + 4.0f;
    src[axisV] = bd->mins[axisV] + 4.0f;
    src[axisD] = 131072.0f;

    float xPixelStep[3] = { 0, 0, 0 };  xPixelStep[axisH] =  1.0f / scale;
    float yPixelStep[3] = { 0, 0, 0 };  yPixelStep[axisV] = -1.0f / scale;

    // half-pixel snap into `org` (sub_40CD40): width→axisH, height→axisV, depth→axisD.
    // The binary reads g_pParentWnd->m_pXYWnd->m_nWidth/m_nHeight (the active XY window);
    // in this per-view draw the current view's own width/height are the faithful values
    // (and for the XY view they ARE m_pXYWnd's).
    float org[3];
    Ed_TextHalfPixelSnap( org, axisD, src, axisV, scale, vWidth, vHeight, axisH );

    R_AddCmdDrawTextAtPosition( name, font, org, xPixelStep, yPixelStep,
                                const_cast<float *>( textColor ) );
}

// XY_BrushColor - REDUCED Brush_GetColor2d 0x46CA10 (the brush's 2D wireframe colour).
// KISAK PORT DEBT: the full Brush_GetColor2d picks frozen-layer (brushflags&0x20)->slot24;
// !worldspawn -> func_group(17)/func_cullgroup(18)/misc_model(21)/else eclass->color +
// Brush_GetEntityLineColor; worldspawn -> weapon-clip(bf&MASK_WEAPONCLIP)->19 / unknown(bf&0x8000004)
// ->22 / bf&0x8000000->14 / BYTE1(def[1].def)->15 / else COLOR_BRUSHES(9), and copies FOUR
// floats including alpha.  This keeps only !worldspawn->eclass colour, worldspawn->9, alpha 1.
// Completing it needs sub_40A130 (colour unpack) + Brush_GetEntityLineColor 0x47aa20 + the
// stride-artifact-prone brushflags offsets.  Cosmetic - selection/geometry unaffected.
// `outRgba` (optional) hands back the same four floats this function packs, so the model-tint
// bracket can push them as MATERIAL_COLOR without unpacking the BGRA order again.
static void XY_BrushColor(selbrush_t *b, GfxColor *out, float *outRgba = nullptr)
{
    float rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    entity_s     *owner = b->owner;
    entity_s_def *eDef  = owner ? (entity_s_def *)owner->def : nullptr;
    eclass_t     *ec    = eDef ? eDef->eclass : nullptr;

    if ( ec && ec->name && _stricmp( ec->name, "worldspawn" ) != 0 )
    {
        rgba[0] = ec->color[0];
        rgba[1] = ec->color[1];
        rgba[2] = ec->color[2];
    }
    else
    {
        rgba[0] = g_qeglobals.d_savedinfo.colors[9][0];
        rgba[1] = g_qeglobals.d_savedinfo.colors[9][1];
        rgba[2] = g_qeglobals.d_savedinfo.colors[9][2];
    }
    rgba[3] = 1.0f;
    Byte4PackPixelColor( rgba, out );
    if ( outRgba )
    {
        outRgba[0] = rgba[0]; outRgba[1] = rgba[1];
        outRgba[2] = rgba[2]; outRgba[3] = rgba[3];
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  2D-VIEW RADIUS OVERLAY - Ed_DrawRadiusCircle 0x46c030 + the dispatchers 0x46c2b0 /
//  0x46c360 / 0x46c4d0 / 0x46c710, all called from CXYWnd::XY_Draw.  Dispatch keys on the
//  entity DEF eclass classtype: &1 light -> Ed_DrawLightRadiusXY (circle); &0x20 node_* ->
//  Ed_DrawNodeRadiusXY (radius + fixedNodeSafeRadius); &0xC0 trigger_radius/disk ->
//  Ed_DrawNodeRadiusXY; else Ed_DrawColoredRadiusXY (_color + radius).  Lights/nodes/colored
//  draw only for SELECTED brushes; triggers also draw unselected, plus the badplace/heading
//  targetname specials.  View-only - never serialised.
//  INSTANCE-vs-DEF: the radius/_color/height keys live on the entity DEF (owner->def, read
//  via Entity_GetFloatValueForKey @0x74), NOT the instance.
// ════════════════════════════════════════════════════════════════════════════

extern float Entity_GetFloatValueForKey( int e, const char *key );   // entity.cpp (0x4837C0)
static const char *Ed_EntFirstKey( entity_s *e, const char *key );   // defined below (epair scan)
extern void Assert( const char *file, int line, int type, const char *fmt, ... ); // (also declared below)
// script-group single-char team-colour visualization (scriptgroup.cpp): the selected-
// trigger gate + the 2D-view marker draw (sub_46B110 cluster, wired below in XY_DrawBrushes).
extern bool  ScriptGroup_BrushIsTrigger( selbrush_t *b );            // scriptgroup.cpp 0x453FD0
extern bool  PrefsDlg_ScriptTeamColorEnabled();                      // prefs.cpp/scriptgroup 0x4560F0
// (defined at the bottom of this file — relocated from scriptgroup.cpp)
extern void  ScriptGroup_DrawTeamColorViz( const char *teamColorStr, const float *viewMins,
                                           const float *viewMaxs, int axis0, int axis1 ); // 0x46B110

// Ed_DrawRadiusCircle (IDB 0x46c030) - sweep a 32-gon of `radius` around `center` in the view
// plane (amplitude axes by viewType: YZ->Y,Z  XZ->X,Z  XY->X,Y), rotating a unit vector by
// 2pi/32 each step (na=ca*c+sa*s / sa=c*sa-ca*s / ca=na; first point = center + amp2).
// Batches via R_Add3DLine -> R_AddCmd_Line3D on `channel`.
static void Ed_DrawRadiusCircle( const GfxColor *col, const float *center,
                                 int viewType, float radius, char channel )
{
    // amp1 / amp2 = the two in-plane amplitude vectors (the binary's v16..v18 / v20..v22).
    float amp1[3] = { 0.0f, 0.0f, 0.0f };
    float amp2[3] = { 0.0f, 0.0f, 0.0f };
    if ( viewType == ED_VIEW_YZ )        // *(view+288)==0
    {
        amp1[1] = radius;                // amp1 = (0,r,0)
        amp2[2] = radius;                // amp2 = (0,0,r)
    }
    else if ( viewType == ED_VIEW_XZ )   // ==1
    {
        amp1[0] = radius;                // amp1 = (r,0,0)
        amp2[2] = radius;                // amp2 = (0,0,r)
    }
    else                                 // ED_VIEW_XY / default
    {
        amp1[0] = radius;                // amp1 = (r,0,0)
        amp2[1] = radius;                // amp2 = (0,r,0)
    }

    const float step = 0.19634955f;      // 2*pi/32
    const float c = cosf( step );
    const float s = sinf( step );
    float ca = 0.0f, sa = 1.0f;          // v23 / v24 (rotating unit vector; starts at amp2)

    const orientation_t *ident = reinterpret_cast<const orientation_t *>( world_orient_matrix );
    GfxPointVertex verts[1024];
    int vertCount = 0;

    // The very first point is center + amp2 (ca=0, sa=1) — the binary pre-adds amp2 to
    // the running point before the loop (v11 = *a2 + v20, ...).
    float prev[3] = { center[0] + amp2[0], center[1] + amp2[1], center[2] + amp2[2] };

    for ( int i = 0; i < 32; ++i )
    {
        // rotate (ca,sa) by the fixed angle step
        float na = ca * c + sa * s;
        sa        = c * sa - ca * s;
        ca        = na;

        float cur[3] = { amp1[0] * ca + amp2[0] * sa + center[0],
                         amp1[1] * ca + amp2[1] * sa + center[1],
                         amp1[2] * ca + amp2[2] * sa + center[2] };

        vertCount = R_Add3DLine( verts, ident, prev, cur, (const unsigned int *)col,
                                 channel, vertCount, 1024 );
        prev[0] = cur[0]; prev[1] = cur[1]; prev[2] = cur[2];
    }
    if ( vertCount )
    {
        R_AddCmd_Line3D( (short)( vertCount / 2 ), channel, verts );
    }
}

// Ed_DrawNodeRadiusShape (IDB 0x46c4d0) - XY view -> the round radius circle; XZ/YZ -> a
// 4-line axis-aligned box of `radius` half-width on the in-plane horizontal axis
// (h = viewType != XZ) with the vertical extent given by the z-offsets zLo/zHi.
static void Ed_DrawNodeRadiusShape( int m_nViewType, const float *center, char channel,
                                    float radius, float zLo, float zHi,
                                    const GfxColor *col )
{
    iassert( m_nViewType == XY || m_nViewType == XZ || m_nViewType == YZ );   // XYWnd.cpp:3492

    if ( m_nViewType == ED_VIEW_XY )
    {
        Ed_DrawRadiusCircle( col, center, m_nViewType, radius, channel );
        return;
    }

    // box branch (XZ=1 -> horizontal axis 0; YZ=0 -> horizontal axis 1)
    const int h = ( m_nViewType != ED_VIEW_XZ );   // v9 = (*(view+288) != 1)

    float p0[3] = { center[0], center[1], center[2] };   // v19
    float p1[3] = { center[0], center[1], center[2] };   // v17
    float p2[3] = { center[0], center[1], center[2] };   // v21
    float p3[3] = { center[0], center[1], center[2] };   // v15

    p0[h] -= radius;
    p1[h] += radius;
    p2[h] += radius;
    p3[h] -= radius;

    p0[2] += zHi;   // v20 += a6
    p1[2] += zHi;   // v18 += a6
    p2[2] += zLo;   // v22 += a5
    p3[2] += zLo;   // v16 += a5

    const orientation_t *ident = reinterpret_cast<const orientation_t *>( world_orient_matrix );
    GfxPointVertex verts[8];
    int vc = 0;
    vc = R_Add3DLine( verts, ident, p0, p1, (const unsigned int *)col, channel, vc, 8 );
    vc = R_Add3DLine( verts, ident, p1, p2, (const unsigned int *)col, channel, vc, 8 );
    vc = R_Add3DLine( verts, ident, p2, p3, (const unsigned int *)col, channel, vc, 8 );
    vc = R_Add3DLine( verts, ident, p3, p0, (const unsigned int *)col, channel, vc, 8 );
    if ( vc )
    {
        R_AddCmd_Line3D( (short)( vc / 2 ), channel, verts );
    }
}

// ── Ed_DrawLightRadiusXY (IDB 0x46c2b0) — light "radius" circle (classtype&1) ────
static void Ed_DrawLightRadiusXY( selbrush_t *brush, const GfxColor *col, int viewType )
{
    brush_t  *def   = brush->def;                                   // *(b+0x14)
    entity_s *eDef  = (entity_s *)brush->owner->def;  // *(*(b+8)+8) = owner->def
    // center = midpoint of the DEF brush bbox (def+0x20 mins, +0x2C maxs)
    float center[3] = { ( def->mins[0] + def->maxs[0] ) * 0.5f,
                        ( def->mins[1] + def->maxs[1] ) * 0.5f,
                        ( def->mins[2] + def->maxs[2] ) * 0.5f };
    iassert( brush->owner->def == brush->def->owner );      // IDB 0x46c2f9 (L0)
    // the binary reads (and discards) the "light" key first, then "radius" — channel 1.
    Entity_GetFloatValueForKey( (int)(intptr_t)eDef, "light" );
    float radius = Entity_GetFloatValueForKey( (int)(intptr_t)eDef, "radius" );
    Ed_DrawRadiusCircle( col, center, viewType, radius, 1 );
}

// ── Ed_DrawColoredRadiusXY (IDB 0x46c360) — the "else" branch: _color + radius ───
// Walks the entity DEF's epairs for "_color" (3 floats), packs a GfxColor, draws the
// def-bbox-centred radius circle if "radius" != 0.
static void Ed_DrawColoredRadiusXY( selbrush_t *brush, int viewType, char channel )
{
    entity_s *eDef = (entity_s *)brush->owner->def;   // *(*(b+8)+8)

    float rgb[3] = { 0.2f, 0.5f, 0.34999999f };                    // binary defaults (v9/v10/v11)
    const char *colorString = nullptr;
    bool foundColor = false;
    for ( epair_t *ep = eDef->epairs; ep; ep = ep->next )
        if ( !_stricmp( ep->key, "_color" ) ) { colorString = ep->value; foundColor = true; break; }
    // IDB 0x46c3a8 asserts the found _color value is non-NULL (binary then sscanfs it); the
    // if(colorString) guard below still avoids sscanf(NULL) in release.
    if ( foundColor )
        iassert( colorString );   // XYWnd.cpp:3459
    if ( colorString )
    {
        float r, g, bl;
        if ( sscanf( colorString, "%f %f %f", &r, &g, &bl ) == 3 )
        {
            rgb[0] = r; rgb[1] = g; rgb[2] = bl;
        }
    }
    GfxColor col;
    Byte4PackPixelColor( rgb, &col );

    brush_t *def = brush->def;                                         // *(b+0x14)
    float center[3] = { ( def->mins[0] + def->maxs[0] ) * 0.5f,
                        ( def->mins[1] + def->maxs[1] ) * 0.5f,
                        ( def->mins[2] + def->maxs[2] ) * 0.5f };
    iassert( brush->owner->def == brush->def->owner );      // IDB 0x46c44a (L0)
    float radius = Entity_GetFloatValueForKey( (int)(intptr_t)eDef, "radius" );
    if ( radius != 0.0f )
        Ed_DrawRadiusCircle( &col, center, viewType, radius, channel );
}

// ── Ed_DrawNodeRadiusXY (IDB 0x46c710) — node_* (classtype&0x20) and trigger_radius/
//    trigger_disk (classtype&0xC0): draws the "radius" AND "fixedNodeSafeRadius" shapes.
//   Gate: (radius > 0 || safeRadius > 0) && !(brushFlags & 2).  height = trigger_disk
//   (classtype & 0x80) -> 32, else "height" > 0, else 80.  center = the DEF origin with z
//   offset by eclass.mins[2].  The safeRadius shape uses the fixed colour 0x40000000.
static void Ed_DrawNodeRadiusXY( char channel, selbrush_t *brush, int viewType, const GfxColor *col )
{
    entity_s_def *eDef = (entity_s_def *)brush->owner->def;  // *(b[2]+...) = owner->def
    iassert( brush->owner->def == brush->def->owner );      // IDB 0x46c71a (L0)
    float radius     = Entity_GetFloatValueForKey( (int)(intptr_t)eDef, "radius" );
    float safeRadius = Entity_GetFloatValueForKey( (int)(intptr_t)eDef, "fixedNodeSafeRadius" );

    // gate: at least one radius > 0 AND the instance isn't a hidden/filtered brush (brushFlags&2).
    if ( ( radius <= 0.0f && safeRadius <= 0.0f ) || ( brush->brushFlags & 2 ) != 0 )
        return;

    eclass_t *ec = eDef->eclass;
    float height;
    if ( (char)ec->classtype < 0 )      // low byte of classtype < 0  ==  classtype & 0x80 (trigger_disk)
    {
        height = 32.0f;
    }
    else
    {
        height = Entity_GetFloatValueForKey( (int)(intptr_t)eDef, "height" );
        if ( height <= 0.0f )
            height = 80.0f;
    }

    // center = entity DEF origin (+0x68) with z offset by eclass.mins[2] (eclass+0x14).
    float center[3] = { eDef->origin[0],
                        eDef->origin[1],
                        ec->mins[2] + eDef->origin[2] };

    if ( radius > 0.0f )
        Ed_DrawNodeRadiusShape( viewType, center, channel, radius, 0.0f, height, col );
    if ( safeRadius > 0.0f )
    {
        // the binary draws fixedNodeSafeRadius with a fixed colour (a4 = 0x40000000 packed).
        GfxColor safeCol;
        *(unsigned int *)&safeCol = 0x40000000u;
        Ed_DrawNodeRadiusShape( viewType, center, channel, safeRadius, 0.0f, height, &safeCol );
    }
}

// Dispatch the per-entity radius overlay for a SELECTED brush (the binary's selected
// loop in XY_Draw): light / node / trigger / colored, keyed on the DEF eclass classtype.
// `col` is the selection-highlight colour (the binary's gfx_col).
static void Ed_DrawSelectedRadius( selbrush_t *b, const GfxColor *col, int viewType, char channel )
{
    entity_s *owner = b->owner;
    if ( !owner )
        return;
    entity_s_def *eDef = (entity_s_def *)owner->def;
    eclass_t *ec = eDef ? eDef->eclass : nullptr;
    if ( !ec )
        return;
    int classtype = ec->classtype;
    if ( ( classtype & 1 ) != 0 )
        Ed_DrawLightRadiusXY( b, col, viewType );
    else if ( ( classtype & 0x20 ) != 0 )
        Ed_DrawNodeRadiusXY( 1, b, viewType, col );
    else if ( ( classtype & 0xC0 ) != 0 )
        Ed_DrawNodeRadiusXY( channel, b, viewType, col );
    else
        Ed_DrawColoredRadiusXY( b, viewType, channel );
}

static inline selbrush_t *Ed_EntFirstBrushInst( entity_s *e );   // defined below

// The prefab-walk counters, shared with the camera (brush.cpp).
extern int g_edPrefabPrefabsWalked;                     // brush.cpp:7061
extern int g_edPrefabBrushesWalked;                     // brush.cpp:7062
extern int g_edPrefabBrushesDrawn;                      // brush.cpp:7064

// The 2D view's per-entity model tint travels as a flat MATERIAL_COLOR, not a per-vertex
// stamp: the stamp forces a writable tempSkinBuf copy (skinnedVert != verts0), which
// disqualifies the surface from the geometry cache and the instance merge.  ONE
// MATERIAL_COLOR is live per RC_DRAW_EDITOR_SKINNEDCACHED, so the flush window is CUT when
// the brush colour changes with surfs queued.  SELECTED models keep the stamp.

// Bound on how often ONE pass may cut its flush window: past the cap the remaining surfs
// share the last tint, rather than risking a render-command buffer overflow.
#define KIWI_XY_TINT_MAX_CUTS 64
static int      s_xyTintCuts  = 0;
static bool     s_xyTintOpen  = false;   // a flush window has adopted a colour
static float    s_xyTintRgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

extern void *R_AddEditorSurfsCmd();      // r_ed_scene.cpp:286
extern int   Editor_PendingSurfCount();  // r_ed_scene.cpp:314

// Read by camwnd.cpp's Cam_SkinModelSEH: while non-zero, a technique-29 model arm must pass
// colorPtr = NULL.  Set ONLY around the XY view's UNSELECTED pass.
int g_edXyModelFlatTint = 0;

// Close the open flush window: push its tint as a flat MATERIAL_COLOR override (w = 1), emit
// the surf command, re-open an empty window.  Nothing queued = nothing emitted.
static void XY_FlushModelSurfs()
{
    if ( Editor_PendingSurfCount() > 0 )
    {
        if ( s_xyTintOpen )
        {
            const float tint[4] = { s_xyTintRgba[0], s_xyTintRgba[1], s_xyTintRgba[2], 1.0f };
            R_AddCmdSetMaterialColor( tint );
        }
        R_AddEditorSurfsCmd();
    }
    R_SortMaterials();      // [FLUSH DEMARC] open the next window
    s_xyTintOpen = false;
}

// Called once per top-level brush, BEFORE DrawBrush queues anything for it.
static void XY_ModelTintNote( const float *rgba )
{
    if ( !s_xyTintOpen )
    {
        s_xyTintRgba[0] = rgba[0]; s_xyTintRgba[1] = rgba[1];
        s_xyTintRgba[2] = rgba[2]; s_xyTintRgba[3] = rgba[3];
        s_xyTintOpen = true;
        return;
    }
    if ( s_xyTintRgba[0] == rgba[0] && s_xyTintRgba[1] == rgba[1]
      && s_xyTintRgba[2] == rgba[2] )
        return;                              // same colour — the window keeps growing
    if ( s_xyTintCuts >= KIWI_XY_TINT_MAX_CUTS )
        return;                              // capped — keep the tint already adopted
    if ( Editor_PendingSurfCount() > 0 )
    {
        XY_FlushModelSurfs();                // cut the window at the colour change
        ++s_xyTintCuts;
    }
    s_xyTintRgba[0] = rgba[0]; s_xyTintRgba[1] = rgba[1];
    s_xyTintRgba[2] = rgba[2]; s_xyTintRgba[3] = rgba[3];
    s_xyTintOpen = true;
}

void XY_DrawBrushes(const XYViewState *v)
{
    if ( !active_brushes.next )            // sentinel list not bootstrapped → no map loaded
        return;

    // open this view's prefab-walk window (published at the tail).
    g_edPrefabPrefabsWalked = g_edPrefabBrushesWalked = 0;
    g_edPrefabBrushesDrawn = 0;

    const int nDim1 = (v->viewType == ED_VIEW_YZ);     // horizontal world axis
    const int nDim2 = (v->viewType != ED_VIEW_XY) + 1; // vertical world axis

    // Visible view rectangle in world units (XY_Draw's v47/tdp/v48/v52).
    const float half = 0.5f / v->scale;
    const float hw   = (float)v->width  * half;
    const float hh   = (float)v->height * half;
    const float xmin = v->origin[nDim1] - hw;
    const float xmax = v->origin[nDim1] + hw;
    const float ymin = v->origin[nDim2] - hh;
    const float ymax = v->origin[nDim2] + hh;

    const orientation_t *ident = reinterpret_cast<const orientation_t *>( world_orient_matrix );

    // The QE4 label colour (savedinfo COLOR_GRIDTEXT slot 8) for the entity-name labels;
    // see Ed_DrawBrushEntityName's text-colour divergence note.
    const float *labelColor = g_qeglobals.d_savedinfo.colors[8];

    // 0x46cfe2 - XY_Draw opens the ACTIVE-brush pass with R_SortMaterials.  [FLUSH DEMARC]:
    // it performs the once-per-frontend-frame editor-scene reset (radiant_surfCount /
    // sceneSurfCount = 0) and pins sceneSurfCount_saved for this pass's flush window.  Without
    // it an interleaved camera+XY paint APPENDS the XY models onto the camera's still-queued
    // surfs, capping radiant_modelSkinnedSurfs and silently dropping model draws.
    { extern void R_SortMaterials(); R_SortMaterials(); }

    // The UNSELECTED pass is open: tech-29 model arms reached from here transport their tint
    // through MATERIAL_COLOR.  Cleared before the selected pass, which keeps the stamp.
    s_xyTintOpen  = false;
    s_xyTintCuts  = 0;
    g_edXyModelFlatTint = 1;

    // The 2D view is the second walker of the same tree: same recording, second replay.
    const bool walkReplay = KiwiWalk_BeginReplay(
        &active_brushes, ident, /*backward*/ false );

    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
    {
        brush_t *def = b->def;
        if ( !def )
            continue;
        if ( FilterBrush( b, 0 ) )         // 0 = no fast-drag filtering (g_PrefsDlg null)
            continue;

        GfxColor col;
        float    colRgba[4];
        XY_BrushColor( b, &col, colRgba );
        // the tint this brush's models want, adopted by the
        // open flush window (cutting it first if the colour changed and surfs are already
        // queued).  See the block above XY_DrawBrushes for the whole derivation.
        XY_ModelTintNote( colRgba );
        if ( walkReplay )
            KiwiWalk_TopLevel( b );        // cursor lockstep
        DrawBrush( b, ident, v->viewType, /*technique=wireframe*/ 29, &col, /*width*/ 1, /*drawFlags*/ 0,
                   /*layerPrefix (binary `zero`)*/ "" );

        // entity-name label (binary's DrawBrushEntityName at 0x46d0fe, right after DrawBrush);
        // self-gates to the owner's representative def-brush + not-hidden + names-shown.
        Ed_DrawBrushEntityName( v->viewType, b, v->scale, v->width, v->height, labelColor );

        // Radius overlay for UNSELECTED entities (binary's active loop, 0x46d647): only
        // trigger_radius/trigger_disk (classtype&0xC0) draw their radius circle here, plus
        // the "badplace"/"heading" targetname specials.  Lights/nodes/general colored-radius
        // draw only when selected (the selected loop below).  Keys are read off the DEF.
        entity_s     *owner = b->owner;
        entity_s_def *eDef  = owner ? (entity_s_def *)owner->def : nullptr;
        eclass_t     *ec    = eDef ? eDef->eclass : nullptr;
        if ( ec )
        {
            if ( ( ec->classtype & 0xC0 ) != 0 )
            {
                GfxColor tcol;
                XY_BrushColor( b, &tcol );              // Brush_GetColor2d
                Ed_DrawNodeRadiusXY( 1, b, v->viewType, &tcol );
            }
            else
            {
                // non-trigger: a "badplace" or "heading" targetname draws a colored radius
                const char *tn = Ed_EntFirstKey( owner, "targetname" );
                if ( !strcmp( tn, "badplace" ) || !strcmp( tn, "heading" ) )
                    Ed_DrawColoredRadiusXY( b, v->viewType,
                        ( g_PrefsDlg->thick_selection_lines != 0 ) + 1 );
            }
        }
    }

    if ( walkReplay )
        KiwiWalk_EndReplay();   // the selected loop below stays immediate

    // 0x46d120 - flush the ACTIVE pass's editor surfs (the ED_SURF_MODEL surfs DrawBrush ->
    // DrawModels queued above) as their own RC_DRAW_EDITOR_SKINNEDCACHED window, exactly where
    // the binary flushes after its not-selected loop.
    // 0x46d230 — the binary opens the SELECTED pass with its own R_SortMaterials, so the
    // selected surfs flush in their own [saved..count) window (not re-flushing the active
    // pass's surfs).
    // Both are XY_FlushModelSurfs now: same R_AddEditorSurfsCmd, same R_SortMaterials re-open,
    // same order and count — only the tint command is new.
    XY_FlushModelSurfs();
    // The selected pass keeps the per-vertex stamp (its arm is tens of instances, and it is
    // the arm round BY3 kept on the dynamic path on purpose).
    g_edXyModelFlatTint = 0;

    // Selected brushes drawn on top.  KISAK: the editor's $line material drives the line
    // colour from CONST_SRC_CODE_MATERIAL_COLOR, not the per-vertex colour, so the selected
    // pass switches the material colour to red; the faithful XY_Draw uses savedinfo
    // COLOR_SELBRUSHES + 3px width (per-colour/width fidelity is a later item).
    // teamColorStr = the team-colour token of the LAST selected script-trigger with a
    // non-empty ScriptColorTeamKey (binary XY_Draw's `x`, captured at 0x46d62c).
    const char *teamColorStr = nullptr;
    const bool  teamColorEnabled = PrefsDlg_ScriptTeamColorEnabled();   // sub_4560F0 (v57)

    if ( selected_brushes.next && selected_brushes.next != &selected_brushes )
    {
        static const float s_selRed[4]   = { 1.0f, 0.05f, 0.05f, 1.0f };
        static const float s_selWhite[4] = { 1.0f, 1.0f,  1.0f,  1.0f };
        R_AddCmdSetMaterialColor( s_selRed );

        GfxColor selCol;
        Byte4PackPixelColor( const_cast<float *>( s_selRed ), &selCol );
        // the binary's selected loop has no view-rect cull on the radius dispatch (a light
        // far off-screen still gets its big radius circle), so the radius overlay runs for
        // every selected brush; the wireframe still culls.  channel = thick_lines ? 2 : 1.
        const char chTrig = (char)( ( g_PrefsDlg->thick_selection_lines != 0 ) + 1 );
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        {
            brush_t *def = b->def;
            if ( !def )
                continue;
            if ( !FilterBrush( b, 0 ) )
                DrawBrush( b, ident, v->viewType, 29, &selCol, 2, 0,
                           /*layerPrefix (binary `zero`)*/ "" );

            // entity-name label (binary's DrawBrushEntityName at 0x46d2c9, in the SELECTED
            // loop — NOT cull-gated there, so a selected entity gets its label even when its
            // bbox is partly off-screen).  This is the path the old dedup-by-owner stand-in
            // dropped: selected entities now keep their classname labels.
            Ed_DrawBrushEntityName( v->viewType, b, v->scale, v->width, v->height, labelColor );

            // radius overlay (light / node / trigger / colored) — keyed on the DEF eclass
            // classtype; the binary dispatches this for EVERY selected brush after DrawBrush.
            Ed_DrawSelectedRadius( b, &selCol, v->viewType, chTrig );
            KiwiLight_DrawXY( b, v->viewType ); // KIWI-UX: Add bounded spot-cone and axis overlays.

            // script-group team-colour capture (binary XY_Draw 0x46d5ca..0x46d62c): for a
            // selected script-trigger, when the team-colour viz is enabled, remember its
            // ScriptColorTeamKey value (the LAST such trigger wins — the binary overwrites
            // `x` each iteration).  The else-branch (Ed_DrawScriptForceColor) is already
            // covered by DrawModels_Decorations for every drawn entity, so it is not
            // duplicated here.
            if ( teamColorEnabled && ScriptGroup_BrushIsTrigger( b ) )
            {
                entity_s *owner = b->owner;
                if ( owner )
                    teamColorStr = Ed_EntFirstKey( owner, g_PrefsDlg->ScriptColorTeamKey.c_str() );
            }
        }
        R_AddCmdSetMaterialColor( s_selWhite );   // restore for subsequent passes/frames
    }

    // script-group single-char team-colour visualization (binary XY_Draw 0x46d848:
    // `if (x) sub_46B110(x, &viewMins, &viewMaxs, axis0, axis1)`).  viewMins/viewMaxs are
    // the view-rect extents on the two view axes; axis0/axis1 = nDim1/nDim2 (v58/v60).
    if ( teamColorStr && teamColorStr[0] )
    {
        const float viewMins[2] = { xmin, ymin };   // &v47 = {v47, v48}
        const float viewMaxs[2] = { xmax, ymax };   // &tdp = {tdp, v52}
        ScriptGroup_DrawTeamColorViz( teamColorStr, viewMins, viewMaxs, nDim1, nDim2 );
    }

}

// ─────────────────────────────────────────────────────────────────────────────
//  CXYWnd INPUT: screen->world picking, drag, select, zoom, scroll, undo.
//  Screen->world (CXYWnd::SnapToPoint 0x467490 / XY_ToPoint 0x4676a0): the pick ray for the
//  XY top view starts high above the clicked world point and shoots straight down (0,0,-1);
//  Test_Ray (select.cpp) clips it against the brushes.  The mouse handlers drive the
//  Drag_Begin / Drag_MouseMoved / Drag_MouseUp core (drag.cpp).
extern void Drag_Begin( void *pressFunc, unsigned int buttons, int viewz,
                        int px, int py, float *xvec, float *yvec,
                        float *trace_start, float *trace_dir );        // drag.cpp
extern void Drag_MouseMoved( int x, int y, int buttons, float *origin, float *dir ); // drag.cpp
extern void Drag_MouseUp( unsigned int buttons );                     // drag.cpp
extern int  g_nPatchClickedView;                                      // engine_stubs (0x73b108)
extern int  g_nUpdateBits;                                            // engine_stubs (0x25D5A74)
extern void Undo_Undo();                                              // undo.cpp
extern void Undo_Redo();                                              // undo.cpp
extern void Map_SaveFile( const char *path, char a1, char a2 );       // map.cpp (Ctrl+S demo save)
extern void Radiant_FL_Log( const char *fmt, ... );                   // mainfrm.cpp
extern void Ed_InvalidateAllViews();                                  // mainfrm.cpp (repaint XY + Z)

extern void *R_AddEditorSurfsCmd();   // r_ed_scene.cpp 0x4FDA10

// Brush-creation chain (NewBrushDrag, IDB 0x467fa0).
extern brush_t   *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls );          // brush.cpp 0x4751e0
extern void       Brush_Create( float *mins, float *maxs, brush_t *b, eclass_t *e );// brush.cpp 0x475300
extern void       Brush_Build( brush_t *b, float *mins, float *maxs );             // brush.cpp 0x4786d0
extern void       Brush_BuildWindings( brush_t *b, int bFull );                    // brush.cpp 0x477ac0
extern void       SetupVertexSelection();                                          // brush.cpp 0x494bc0
extern void       MarkMapModified();                                               // brush.cpp 0x499bb0
extern void       Entity_LinkBrush( brush_t *b, entity_s *world_ent );             // entity.cpp 0x484fc0 (def-side)
extern selbrush_t*Brush_AddToList( brush_t *def, entity_s *owner );                // brush.cpp 0x475980 (instance)
// ── CreateEntityFromName Path B (re-class) deps ──
extern void       Undo_AddEntity_W( entity_s *a1 );                                // undo.cpp 0x45e990
extern int        Init_MaterialLayer( MaterialDef *a1, MaterialDef *a2 );          // materialdef.cpp 0x472c00
extern void       sub_476330( selbrush_t *b );                                     // brush.cpp Brush_Deselect_Helper 0x476330
extern void       Brush_Free( selbrush_t *b );                                     // brush.cpp 0x475ba0
extern eclass_t  *Entity_SetDefaultModelKey( brush_t *a1, eclass_t *a2 );          // entity.cpp 0x485510
extern void       Brush_AddToList2( selbrush_t *b );                               // brush.cpp 0x4765a0 (selection)
extern void       Com_Error( int code, const char *fmt, ... );
extern void       SetMaterial( const char *tex_name, patchMesh_material *out );    // materialdef.cpp 0x4315c0
extern char       Texture_SetTexture( const int *a1, MaterialDef *a2 );            // texwnd.cpp 0x45be50
namespace LayerMat { int GetCurrentLayer( MaterialDef *md ); }                     // materialdef.cpp 0x431b30

// Trigger_SetCurrentDefaultMaterial (IDB sub_45C1A0 0x45c1a0) — build a 64x64 default
// material from `name` and make it the current editor texture (Texture_SetTexture).
// Called for the just-created trigger/volume brush so it gets a sensible default skin.
static char Trigger_SetCurrentDefaultMaterial( const char *name )
{
    MaterialDef v3;
    memset( &v3, 0, sizeof( v3 ) );                       // 0x45c1ac
    SetMaterial( name, (patchMesh_material *)&v3 );       // 0x45c1c7
    // &v3.mat_texDef + GetCurrentLayer scales by sizeof(texdef_sub_t)=28 (IDB lea eax*7*4).
    texdef_sub_t *v1 = &v3.mat_texDef + LayerMat::GetCurrentLayer( &v3 );  // 0x45c1e0
    v1->size[0] = 64.0f;                                  // 0x45c1e4 (flt_6F4738)
    v1->size[1] = 64.0f;                                  // 0x45c1e8
    return Texture_SetTexture( 0, &v3 );                  // 0x45c1ee  a1=0 (no patch projection)
}

// ─── entity-creation chain (CreateEntityFromName, IDB 0x465cc0) ───────────────────
extern eclass_t  *Eclass_ForName( int has_brushes, const char *name );             // eclass.cpp 0x482190
extern entity_s  *Entity_Create( eclass_t *eclass );                               // entity.cpp 0x484980
extern void       EntityAssignModel( entity_s_def *a1 );                           // entity.cpp 0x485410
extern void       Undo_SetIdForEntity( entity_s_def *ent );                        // undo.cpp 0x45e9e0
extern void       SetKeyValue( entity_s_def *e, const char *key, const char *value );// entity.cpp 0x483690
extern char      *ValueForKey2( int defPtr, const char *key );                     // entity.cpp 0x4825c0
extern char      *Map_GetNextExportId( int slot );                                 // map.cpp 0x487a70
extern int        I_stricmp( const char *s0, const char *s1 );                      // q_shared
extern void       Assert( const char *file, int line, int type, const char *fmt, ... );
void              CreateEntityFromName( const char *str );                          // defined below (0x465cc0)
static void       CreateEntityFromClassname( xywndState_t *xywnd, const char *classname, int x, int y ); // below (0x466480)
extern const char *Ed_SelectedEclassName();                                          // win_ent.cpp — the eclass picked in the entity window
extern void        Ed_PostAddModelCommand();                                         // win_ent.cpp — post IDC_E_ADD_MODEL to the entity window (model picker)

// RMB context-menu hook state (consumed by CXYWnd::ContextMenu, the full 0x467100
// eclass-tree menu, ported below): set true when an RMB drag actually scrolls the view,
// so OnRButtonUp can tell a context-click (no scroll) from a scroll-release.
static bool       s_rmbScrolled = false;

// ─── XY_MouseDown dispatch helpers (clone / nudge / camera-set branches) ──────────
extern void       Select_GetMid( float *mid );                                     // select.cpp 0x48fc70
extern void       Select_Move( const float *delta, char bSnap );                   // select.cpp 0x48e9c0
extern void       Select_Brush( selbrush_t *b, char ow, char st, char ctr );       // select.cpp 0x48dcc0
extern void       Select_Deselect( int bDeselectFaces );                           // select.cpp 0x48e800
extern void       Clone_Selection( float a1 );                                     // select.cpp 0x48f0d0
extern void       ConnectEntities_R();                                             // select.cpp 0x48c530
extern void       Undo_ClearRedo();                                                // undo.cpp
extern void       Undo_GeneralStart( const char *op );                             // undo.cpp
extern void       Undo_AddBrushList( selbrush_t *list );                           // undo.cpp
extern void       Undo_EndBrushList( selbrush_t *list );                           // undo.cpp
extern void       Undo_End();                                                      // undo.cpp
extern int        Sys_Printf( const char *fmt, ... );                              // 0x499e90
extern void       MainFrm_SetStatusText( int pane, const char *text );             // mainfrm.cpp
extern bool       g_bRotateMode;                                                   // drag.cpp 0x23f16d9
extern bool       g_bScaleMode;                                                    // drag.cpp 0x23f16da
extern int        g_bPatchBendMode;                                                // 0x25d5b04
extern float      g_vRotateOrigin[3];                                              // drag.cpp 0x23f1658
extern float      g_vBendOrigin[3];                                                // 0x231f548

// byte_25D5A6A - promoted from a xywnd.cpp static to a shared global (defined in pmesh.cpp
// next to its primary reader Patch_ClickControlPoint) so the curve-point click path sees the
// "second click in the same view" flag the camera-set dispatch below sets.
extern char  g_bXYViewIsLastPatchClick;        // byte_25D5A6A (def: pmesh.cpp)

// Ed_SnapToPoint - CXYWnd::SnapToPoint 0x467490: pixel -> world point in the view plane.
// The m_nWidth*0.5f (float) vs m_nWidth/2 (int) distinction between this and SnapToGrid is
// reproduced from the binary.
// KISAK: the vertical-Y axis is FLIPPED ((h/2 - y), not the binary's (y - h/2)) consistently
// across SnapToPoint + SnapToGrid + Ed_ViewBasis - the port's mouse handlers pass raw
// top-left Y where the binary pre-flips to GL bottom-left.
static void Ed_SnapToPoint( xywndState_t *wnd, float *tdp, int x, int y )
{
    float vx = ( (float)x - (float)wnd->m_nWidth  * 0.5f ) / wnd->m_fScale;
    // VERTICAL FLIP (see the header): screen Y grows down, the view renders +vertical up.
    float vy = ( (float)wnd->m_nHeight * 0.5f - (float)y ) / wnd->m_fScale;
    if ( wnd->m_nViewType == ED_VIEW_XY )
    {
        tdp[0] = vx + wnd->m_vOrigin[0];
        tdp[1] = vy + wnd->m_vOrigin[1];
    }
    else if ( wnd->m_nViewType != ED_VIEW_YZ )   // XZ
    {
        tdp[0] = vx + wnd->m_vOrigin[0];
        tdp[2] = vy + wnd->m_vOrigin[2];
    }
    else                                         // YZ
    {
        tdp[1] = vx + wnd->m_vOrigin[1];
        tdp[2] = vy + wnd->m_vOrigin[2];
    }
}

// CXYWnd::XY_ToPoint (0x4676a0) — build the pick ray (start above + axis dir).
// Non-static: Drag_MouseUp (drag.cpp) reuses it to build the marquee box corners.
void Ed_XY_ToPoint( xywndState_t *wnd, int x, int y, float *start, float *dir )
{
    start[0] = start[1] = start[2] = 0.0f;
    Ed_SnapToPoint( wnd, start, x, y );
    dir[0] = dir[1] = dir[2] = 0.0f;
    if ( wnd->m_nViewType == ED_VIEW_XY )      { start[2] = 131072.0f; dir[2] = -1.0f; }
    else if ( wnd->m_nViewType != ED_VIEW_YZ ) { start[1] = 131072.0f; dir[1] = -1.0f; }   // XZ
    else                                       { start[0] = 131072.0f; dir[0] = -1.0f; }   // YZ
}

// View-axis world basis (screen X/Y -> world drag axes).  AxializeVector leaves a single-axis
// vector unchanged, so this carries the full 1/scale magnitude and the drag tracks the cursor
// 1:1 at any zoom.  The vertical axis is NEGATED (see Ed_SnapToPoint).
static void Ed_ViewBasis( xywndState_t *wnd, float *xvec, float *yvec )
{
    float inv = 1.0f / wnd->m_fScale;
    xvec[0] = xvec[1] = xvec[2] = 0.0f;
    yvec[0] = yvec[1] = yvec[2] = 0.0f;
    int nDim1 = ( wnd->m_nViewType == ED_VIEW_YZ ) ? 1 : 0;
    int nDim2 = ( wnd->m_nViewType != ED_VIEW_XY ) + 1;
    xvec[nDim1] =  inv;
    yvec[nDim2] = -inv;     // screen-down → world-down (matches the view's vertical axis)
}

// Press callback (Drag_Begin stores it in g_qeglobals.camera_fov_setup) - stands in for the
// binary's sub_467700; XY_Draw's marquee-overlay gate compares camera_fov_setup against its
// address, so the box only draws while a real drag is in progress.  The sub_467700 BODY (4
// box-select side-planes used ONLY by Terrain/Patch_SelectAreaPoints) stays inert: the BRUSH
// marquee uses the world box Drag_MouseUp computes from the drag rectangle.
static void Ed_PressCallback( int x1, int x2, int y1, int y2, int *out )
{ (void)x1; (void)x2; (void)y1; (void)y2; (void)out; }

// DrawSelectionBox 0x40CC50 + the box-build from XY_Draw's tail 0x46d9a1 - the live marquee as
// a translucent blue (alpha 0.25) quad on d_white/UNLIT: indices {3,0,2,2,0,1}, normals
// (0,0,1)x4, st {(0,0),(0,1),(1,1),(1,0)}, depth 131072 on both corners.  Rect normalise:
// ax=min(x_1,y_1)/bx=max, ay=min(x_2,y_2)/by=max.  The draw gate (camera_fov_setup ==
// the press callback && d_select_mode in {5,12,13,14,15}) lives in the caller.
extern void __cdecl R_AddRenderCmdDrawTris(
    Material *material, MaterialTechniqueType techType, short indexCount,
    const uint16_t *indices, short vertexCount,
    const float (*xyzw)[4], const float (*normal)[3], float *color,
    const float (*st)[2] );

// 0x40CC50 DrawSelectionBox — shared by XY_Draw (Ed_DrawSelectionBox) and the camera
// 3D-marquee tail (camwnd.cpp Cam_Draw).  Non-static so camwnd.cpp can call it.
void Ed_DrawSelectionBoxQuad( const float (*verts)[4] )
{
    static const uint16_t indices[6] = { 3, 0, 2, 2, 0, 1 };
    float normal[4][3] = { {0,0,1}, {0,0,1}, {0,0,1}, {0,0,1} };
    float st[4][2]     = { {0,0}, {0,1}, {1,1}, {1,0} };

    float c4[4] = { 0.0f, 0.0f, 1.0f, 0.25f };   // blue, 25% alpha
    GfxColor packed;
    Byte4PackPixelColor( c4, &packed );
    float colorRepl = *(float *)&packed.packed;   // the packed colour, splatted to 4 floats
    float color[4] = { colorRepl, colorRepl, colorRepl, colorRepl };

    R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT, 6, indices, 4,
                            verts, normal, color, st );
}

// Build the world box from the (normalised) drag rectangle and draw it. `wnd` supplies
// the screen→world snap + the view's depth axis (set to 131072 like the binary). The
// rect is stored as: x_1/x_2 = press (x,y); y_1/y_2 = current (x,y).
static void Ed_DrawSelectionBox( xywndState_t *wnd )
{
    // Normalise the rect so corner A = (min screen-x, min screen-y), B = (max,max).
    int ax, ay, bx, by;
    if ( g_qeglobals.drag_selectionbox_x_1 >= g_qeglobals.drag_selectionbox_y_1 )
    { ax = g_qeglobals.drag_selectionbox_y_1; bx = g_qeglobals.drag_selectionbox_x_1; }
    else
    { ax = g_qeglobals.drag_selectionbox_x_1; bx = g_qeglobals.drag_selectionbox_y_1; }
    if ( g_qeglobals.drag_selectionbox_x_2 >= g_qeglobals.drag_selectionbox_y_2 )
    { ay = g_qeglobals.drag_selectionbox_y_2; by = g_qeglobals.drag_selectionbox_x_2; }
    else
    { ay = g_qeglobals.drag_selectionbox_x_2; by = g_qeglobals.drag_selectionbox_y_2; }

    float a[3] = { 0, 0, 0 };
    float b[3] = { 0, 0, 0 };
    Ed_SnapToPoint( wnd, a, ax, ay );
    Ed_SnapToPoint( wnd, b, bx, by );

    const int vt = wnd->m_nViewType;
    // depth axis pushed out to the far plane (matches the binary's 131072 on both corners)
    if ( vt == ED_VIEW_XY )      { a[2] = 131072.0f; b[2] = 131072.0f; }
    else if ( vt != ED_VIEW_YZ ) { a[1] = 131072.0f; b[1] = 131072.0f; }  // XZ
    else                         { a[0] = 131072.0f; b[0] = 131072.0f; }  // YZ

    const int d1 = (vt == ED_VIEW_YZ);      // screen-horizontal axis
    const int d2 = (vt != ED_VIEW_XY) + 1;  // screen-vertical axis
    const int d3 = 3 - d1 - d2;              // depth axis

    // 4 corners of the rectangle in the view plane (IDA v42 layout), w=1, depth=131072.
    float quad[4][4];
    // corner 0: (a.h, a.v)
    quad[0][d1] = a[d1]; quad[0][d2] = a[d2]; quad[0][d3] = 131072.0f; quad[0][3] = 1.0f;
    // corner 1: (a.h, b.v)
    quad[1][d1] = a[d1]; quad[1][d2] = b[d2]; quad[1][d3] = 131072.0f; quad[1][3] = 1.0f;
    // corner 2: (b.h, b.v)
    quad[2][d1] = b[d1]; quad[2][d2] = b[d2]; quad[2][d3] = 131072.0f; quad[2][3] = 1.0f;
    // corner 3: (b.h, a.v)
    quad[3][d1] = b[d1]; quad[3][d2] = a[d2]; quad[3][d3] = 131072.0f; quad[3][3] = 1.0f;

    Ed_DrawSelectionBoxQuad( quad );
}

// ─── CXYWnd::SnapToGrid (IDB 0x467510) ───────────────────────────────────────
// Pixel → grid-snapped world point in the view plane. Writes only the two in-plane
// axes (the depth axis is set by the caller — e.g. NewBrushDrag's d_new_brush bounds).
static void Ed_SnapToGrid( xywndState_t *wnd, float *out, int x, int y )
{
    const float step = grid_sizes[g_qeglobals.d_gridsize];
    const float px = (float)( x - wnd->m_nWidth  / 2 ) / wnd->m_fScale;
    // VERTICAL FLIP (see Ed_SnapToPoint): screen Y grows down, the view renders the vertical
    // world axis up-screen, so flip so grid-snapping lands where the cursor visually is.
    const float py = (float)( wnd->m_nHeight / 2 - y ) / wnd->m_fScale;
    if ( wnd->m_nViewType == ED_VIEW_XY )
    {
        out[0] = (float)floor( (double)( px + wnd->m_vOrigin[0] ) / step + 0.5 ) * step;
        out[1] = (float)floor( (double)( py + wnd->m_vOrigin[1] ) / step + 0.5 ) * step;
    }
    else if ( wnd->m_nViewType != ED_VIEW_YZ )   // XZ
    {
        out[0] = (float)floor( (double)( px + wnd->m_vOrigin[0] ) / step + 0.5 ) * step;
        out[2] = (float)floor( (double)( py + wnd->m_vOrigin[2] ) / step + 0.5 ) * step;
    }
    else                                          // YZ
    {
        out[1] = (float)floor( (double)( px + wnd->m_vOrigin[1] ) / step + 0.5 ) * step;
        out[2] = (float)floor( (double)( py + wnd->m_vOrigin[2] ) / step + 0.5 ) * step;
    }
}

// ─── SnapToPoint_Chooser (IDB 0x467460) ──────────────────────────────────────
// Grid-snap unless m_bNoClamp (prefs). g_PrefsDlg NULL → 0 → grid-snap (the default).
static void Ed_SnapToPoint_Chooser( xywndState_t *wnd, float *out, int x, int y )
{
    if ( g_PrefsDlg->m_bNoClamp )
        Ed_SnapToPoint( wnd, out, x, y );
    else
        Ed_SnapToGrid( wnd, out, x, y );
}

// ─── CXYWnd::VectorCopyXY (IDB 0x46df50) ─────────────────────────────────────
// Copy the two in-plane components of `in` into `out` (leaving the depth axis).
static void Ed_VectorCopyXY( xywndState_t *wnd, float *out, const float *in )
{
    if ( wnd->m_nViewType == ED_VIEW_XY ) { out[0] = in[0]; out[1] = in[1]; }
    else if ( wnd->m_nViewType == ED_VIEW_XZ ) { out[0] = in[0]; out[2] = in[2]; }
    else { out[1] = in[1]; out[2] = in[2]; }
}

// sub_459A40 (0x459a40) = atan2(a1, a2).  DISASM: `fld a1; fld a2; call atan2` -> the MSVC x87
// _CIatan2 helper computes atan2(st1, st0) = atan2(a1, a2).  Hex-rays renders the call
// BACKWARDS as atan2(a2, a1) (a known _CIatan2 operand-reversal artifact) and mistracks st0 as
// `return a1`; copying that form swaps the XY middle-click camera yaw.
static double Ed_Atan2_459A40( float a1, float a2 ) { return atan2( (double)a1, (double)a2 ); }

// sub_48CDF0 (0x48cdf0) - save the prior select mode, store sel_brush, then the patch
// sub-mode cleanup tail: prior cycle mode -> CMainFrame::UpdatePatchToolbarButtons (a no-op
// no-toolbar stub in this build); prior addpoint mode -> Patch_FinishCurveDrag (sub_43ECB0).
// Same cleanup-tail shape as Drag_MouseUp and OnSelectionDragVertices/Edges.
extern void CMainFrame_UpdatePatchToolbarButtons(); // select.cpp (0x42AA70, no-op no-toolbar)
extern void sub_43ECB0();                            // pmesh.cpp Patch_FinishCurveDrag (0x43ECB0)
static void Ed_ResetSelectMode_48CDF0()
{
    const select_t prev = g_qeglobals.d_select_mode;
    g_qeglobals.d_select_mode = sel_brush;
    if ( prev == sel_cycle_edge_direction_quad )
        CMainFrame_UpdatePatchToolbarButtons();
    else if ( prev == sel_addpoint )
        sub_43ECB0();
}

// Ed_DragDelta - XYWnd::DragDelta 0x467e70.  Pixel delta since the press -> grid-snapped world
// delta accumulated in m_vPressdelta; `out` receives the INCREMENT, the return is "did it
// move".  The basis is the FIXED XY-plane unit basis (verbatim - NOT view-dependent and NOT
// Y-flipped): it only gates movement, the real brush bounds come from NewBrushDrag's
// view-aware SnapToGrid.  The grid snap is floorf(w/step)*step - truncation, no +0.5.
static bool Ed_DragDelta( xywndState_t *wnd, int x, int y, float *out )
{
    const int dx = x - wnd->m_nPressx;
    const int dy = y - wnd->m_nPressy;
    const float inv = 1.0f / wnd->m_fScale;
    const float xvec[3] = { inv, 0.0f, 0.0f };
    const float yvec[3] = { 0.0f, inv, 0.0f };
    const bool noclamp = ( g_PrefsDlg->m_bNoClamp != 0 );

    float work[3];
    for ( int i = 0; i < 3; ++i )
    {
        float w = yvec[i] * (float)dy + xvec[i] * (float)dx;
        if ( !noclamp )
        {
            const float step = grid_sizes[g_qeglobals.d_gridsize];
            w = (float)floorf( w / step ) * step;   // IDA truncates with floorf (no +0.5)
        }
        work[i] = w;
    }
    out[0] = work[0] - wnd->m_vPressdelta[0];
    out[1] = work[1] - wnd->m_vPressdelta[1];
    out[2] = work[2] - wnd->m_vPressdelta[2];
    wnd->m_vPressdelta[0] = work[0];
    wnd->m_vPressdelta[1] = work[1];
    wnd->m_vPressdelta[2] = work[2];
    return ( out[0] != 0.0f || out[1] != 0.0f || out[2] != 0.0f );
}

// KISAK: random_texture_stuff is the editor's "current material" template - Brush_Alloc copies
// its channel-0 MaterialDef into every face of a NEW brush.  Nothing in the port sets it yet,
// so on a fresh session it is zeroed and a new brush's channel-0 lyrMtl is NULL (trips
// MtlDef_IsValid).  Seed it lazily from the first loaded brush, else synthesise a default.
static void Ed_EnsureCurrentMaterial()
{
    MaterialDef *cur = (MaterialDef *)&g_qeglobals.random_texture_stuff[0];
    if ( cur->lyrMtl || cur->radMtl )
        return;
    for ( selbrush_t *b = active_brushes.next; b && b != &active_brushes; b = b->next )
    {
        brush_t *d = b->def;
        if ( d && d->faceCount && d->faces )
        {
            memcpy( cur, &d->faces[0].mtldef[0], sizeof( MaterialDef ) );
            return;
        }
    }
    // No brush to copy from (empty map) → a default current material (degenerate
    // headless / a real handle when the renderer is up).
    SetMaterial( "caulk", (patchMesh_material *)cur );
}

// KIWI-UX (RADIANT_UX_DESIGN §23, Phase 4): public forwarder for the static above,
// so region extrusion (kiwi_extrude.cpp) seeds a new brush's material through the
// SAME lazy path the ported creator Ed_NewBrushDrag uses instead of re-deriving
// it.  Pure forwarder — no behaviour change, and the static keeps its file scope.
void Ed_EnsureCurrentMaterial_Kiwi()
{
    Ed_EnsureCurrentMaterial();
}

// ─────────────────────────────────────────────────────────────────────────────
// Ed_NewBrushDrag - XYWnd::NewBrushDrag 0x467fa0, the brush-creation path.  Called from
// XY_MouseMoved while a plain-LMB drag is in progress and nothing was selected at press
// (m_bPress_selection == 0) in sel_brush mode.  The 2D footprint comes from the press point
// and the current point (grid-snapped per view); the depth (out-of-screen) axis comes from
// the d_new_brush_bottom/top template, with a lo>=hi guard giving one grid unit of depth.
// Nothing selected -> ALLOC/CREATE/BUILD/LINK(def+instance)/SELECT; otherwise RESIZE the
// selected brush via Brush_Build.
// NOTE Brush_Create(mins,maxs) is the DISASM-confirmed arg order - hex-rays prints the
// __fastcall args swapped as (maxs,mins).
// ─────────────────────────────────────────────────────────────────────────────
static void Ed_NewBrushDrag( xywndState_t *wnd, int x, int y )
{
    float vSize[3];
    if ( !Ed_DragDelta( wnd, x, y, vSize ) )
        return;   // drag hasn't crossed a grid cell since the last move → nothing to do

    // Depth (out-of-screen) axis: XY→Z(2), XZ→Y(1), YZ→X(0).
    const int depth = ( wnd->m_nViewType == ED_VIEW_XY ) ? 2
                                                         : ( wnd->m_nViewType != ED_VIEW_YZ );

    const bool noclamp = ( g_PrefsDlg->m_bNoClamp != 0 );

    float mins[3], maxs[3];
    if ( noclamp ) Ed_SnapToPoint( wnd, mins, wnd->m_nPressx, wnd->m_nPressy );
    else           Ed_SnapToGrid ( wnd, mins, wnd->m_nPressx, wnd->m_nPressy );

    const float step   = grid_sizes[g_qeglobals.d_gridsize];
    const float *botSrc = &g_qeglobals.d_new_brush_bottom_x;   // [x,y,z]
    const float *topSrc = &g_qeglobals.d_new_brush_top_x;      // [x,y,z]

    const float lo = (float)(int)( botSrc[depth] / step ) * step;
    mins[depth] = lo;

    if ( noclamp ) Ed_SnapToPoint( wnd, maxs, x, y );
    else           Ed_SnapToGrid ( wnd, maxs, x, y );

    float hi = (float)(int)( topSrc[depth] / step ) * step;
    maxs[depth] = hi;
    if ( lo >= hi )
        maxs[depth] = lo + step;   // never degenerate in depth — min one grid unit

    // Sort each axis (mins<=maxs); bail if any axis collapses to zero extent.
    for ( int i = 0; i < 3; ++i )
    {
        if ( maxs[i] == mins[i] )
            return;
        if ( maxs[i] < mins[i] )
        {
            const float t = mins[i]; mins[i] = maxs[i]; maxs[i] = t;
        }
    }

    if ( selected_brushes.next == &selected_brushes )
    {
        // ── nothing selected → CREATE a new brush sized to the box ──
        Ed_EnsureCurrentMaterial();   // give the new brush a valid material (see above)
        brush_t *b = Brush_Alloc( g_qeglobals.random_texture_stuff, nullptr );
        Brush_Create( mins, maxs, b, nullptr );   // (lower, upper) — disasm-confirmed arg order
        if ( !b )
            return;
        Brush_BuildWindings( b, 1 );
        if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
            SetupVertexSelection();
        MarkMapModified();
        ++b->version;
        // Link the DEF into worldspawn (def-side, refCount++), allocate the instance
        // (refCount++ again — the faithful def/instance refCount==2 model), then
        // add it to the selection list (Brush_AddToList2 self-guards double-link).
        Entity_LinkBrush( b, (entity_s *)world_entity->def );
        selbrush_t *inst = Brush_AddToList( b, world_entity );
        if ( inst->next || inst->prev )   // IDB 0x4680e2 (v13->onext/oprev = inst next/prev); restored
            Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
        Brush_AddToList2( inst );
    }
    else
    {
        // ── a brush is selected (e.g. the one just created) → RESIZE it ──
        Brush_Build( selected_brushes.next->def, mins, maxs );
    }

    char sz[96];
    _snprintf( sz, sizeof( sz ), "Size X:: %.1f  Y:: %.1f  Z:: %.1f",
               maxs[0] - mins[0], maxs[1] - mins[1], maxs[2] - mins[2] );
    MainFrm_SetStatusText( 3, sz );
    g_nUpdateBits |= 3;   // W_XY | W_CAMERA
}

// ═════════════════════════════════════════════════════════════════════════════
//  Ed_XY_MouseDown - CXYWnd::XY_MouseDown 0x467850, the full mouse-down dispatcher.  Routes a
//  press by button/modifier/select-mode into one of:
//   (1) alt+(mid|right)+ctrl  -> nudge the selection centre onto the cursor (Select_Move)
//   (2) alt+(LMB+ctrl | (mid|right)+shift) -> clone/duplicate the selection
//   (3) LMB[/+shift/+ctrl]    -> Drag_Begin: select / move / NEW-BRUSH creation
//   (4) ctrl/mid/right combos -> set the camera origin / angle / the XY camera icon
//  Drag_Begin's viewCode is XY->2 / XZ->512 / YZ->1024; the camera-angle tail uses
//  atan2(dir[a1],dir[a2])*57.29578 with a1=(vt!=XY)+1, a2=(vt==YZ), angIdx=(vt==XY).
//  KISAK: negated-yvec basis (Ed_ViewBasis, consistent with SnapToPoint); g_pParentWnd /
//  m_pCamWnd null-guards where the binary derefs directly.
// ═════════════════════════════════════════════════════════════════════════════
static void Ed_XY_MouseDown( xywndState_t *wnd, int x, int y, unsigned int buttons )
{
    // The non-left drag button: middle on a 3-button mouse, right on a 2-button mouse.
    const unsigned int v6 = ( g_PrefsDlg->m_nMouseButtons != 2 ) ? MK_MBUTTON : MK_RBUTTON;

    wnd->m_nButtonstate   = (int)buttons;
    wnd->m_nPressx        = x;
    wnd->m_nPressy        = y;
    wnd->m_vPressdelta[0] = wnd->m_vPressdelta[1] = wnd->m_vPressdelta[2] = 0.0f;

    float trace_start[3], trace_dir[3];
    Ed_XY_ToPoint( wnd, x, y, trace_start, trace_dir );

    float snap[3] = { 0.0f, 0.0f, 0.0f };
    Ed_SnapToPoint( wnd, snap, x, y );

    // Screen->world drag basis (negated yvec - see Ed_ViewBasis).  Drag_Setup axialises these
    // for the move basis; brush CREATION is independent of them (it uses SnapToGrid).
    float xvec[3], yvec[3];
    Ed_ViewBasis( wnd, xvec, yvec );

    wnd->m_bPress_selection = ( selected_brushes.next != &selected_brushes );
    GetCursorPos( &wnd->m_ptCursor );

    const bool alt = ( GetAsyncKeyState( VK_MENU ) < 0 );

    // ── (1) alt + (mid|right)+ctrl : move the selection centre onto the cursor ──────
    if ( buttons == ( v6 | MK_CONTROL ) && alt )
    {
        if ( selected_brushes.next != &selected_brushes )
        {
            Ed_ResetSelectMode_48CDF0();
            float mid[3]; Select_GetMid( mid );
            float dst[3] = { mid[0], mid[1], mid[2] };
            if ( !g_PrefsDlg->m_bNoClamp ) Ed_SnapToGrid( wnd, dst, x, y );
            else                         Ed_SnapToPoint( wnd, dst, x, y );
            float move[3] = { dst[0] - mid[0], dst[1] - mid[1], dst[2] - mid[2] };
            Select_Move( move, 0 );
            g_nUpdateBits = -1;
        }
        return;
    }

    // ── (2) alt + (LMB+ctrl | (mid|right)+shift) : clone / duplicate ────────────────
    if ( alt && ( buttons == 9 || buttons == ( v6 | MK_SHIFT ) )
         && g_qeglobals.d_select_mode != sel_curvepoint )
    {
        if ( selected_brushes.next == &selected_brushes )
            return;
        Ed_ResetSelectMode_48CDF0();
        selbrush_t *single = nullptr;
        if ( ( buttons & MK_SHIFT ) != 0 )
        {
            if ( selected_brushes.next->next != &selected_brushes )
            {
                Sys_Printf( "can only alt + shift + middle click if a single brush is selected" );
                return;
            }
            single = selected_brushes.next;
        }
        float mid[3]; Select_GetMid( mid );
        Clone_Selection( 0.0f );
        Undo_ClearRedo();
        Undo_GeneralStart( "duplicate" );
        Undo_AddBrushList( &selected_brushes );
        float dst[3] = { mid[0], mid[1], mid[2] };
        Ed_SnapToPoint_Chooser( wnd, dst, x, y );
        float move[3] = { dst[0] - mid[0], dst[1] - mid[1], dst[2] - mid[2] };
        Select_Move( move, 0 );
        if ( single )
        {
            // alt+shift+MIDDLE single-brush clone-and-connect (IDB 0x467AE8 tail).
            // single = the original brush; selected_brushes.next = the just-cloned+moved
            // copy.  Re-select exactly {original, clone} and link them via ConnectEntities_R.
            iassert( selected_brushes.next != &selected_brushes );   // XYWnd.cpp:1777
            selbrush_t *clone = selected_brushes.next;
            Select_Deselect( 1 );
            Select_Brush( single, 1, 0, 0 );
            Select_Brush( clone,  1, 0, 0 );
            ConnectEntities_R();
        }
        Undo_EndBrushList( &selected_brushes );
        Undo_End();
        g_nUpdateBits = -1;
        return;
    }

    // ── (3) the MAIN drag path: select / move / NEW-BRUSH creation via Drag_Begin ───
    if ( buttons == 1 || buttons == 5 || buttons == 9 || buttons == 13
         || ( ( buttons == 2 || buttons == 6 ) && alt ) )
    {
        int viewCode = wnd->m_nViewType;
        if ( viewCode != ED_VIEW_XY )
            viewCode = ( viewCode != ED_VIEW_YZ ) ? 512 : 1024;   // XZ→512, YZ→1024
        else
            viewCode = 2;
        g_bXYViewIsLastPatchClick = ( viewCode == g_nPatchClickedView );
        g_nPatchClickedView       = viewCode;
        Drag_Begin( (void *)&Ed_PressCallback, buttons, /*viewz*/ 0, x, y,
                    xvec, yvec, trace_start, trace_dir );
        return;
    }

    // ── (4) camera positioning / XY camera-icon placement ───────────────────────────
    // U-GLOBALS: both branches write the 3D camera through Ed_Camera() (never NULL), so the
    // shell gate and the g_pParentWnd/m_pCamWnd null-guards are gone.
    if ( wnd->m_nButtonstate == (int)( v6 | MK_CONTROL ) )
    {
        Ed_VectorCopyXY( wnd, Ed_Camera()->origin, snap );
        g_nUpdateBits |= 5;
    }
    const int nmb = g_PrefsDlg->m_nMouseButtons;
    if ( ( nmb == 3 && wnd->m_nButtonstate == 16 ) ||
         ( nmb == 2 && wnd->m_nButtonstate == 14 ) )
    {
        camera_s *cam = Ed_Camera();
        float dir[3] = { snap[0] - cam->origin[0], snap[1] - cam->origin[1],
                         snap[2] - cam->origin[2] };   // IDA VectorSubtract(snap, cam.origin, dir)
        const int a1     = ( wnd->m_nViewType != ED_VIEW_XY ) + 1;
        const int a2     = ( wnd->m_nViewType == ED_VIEW_YZ );
        const int angIdx = ( wnd->m_nViewType == ED_VIEW_XY );
        if ( dir[a1] != 0.0f || dir[a2] != 0.0f )
        {
            const double ang = Ed_Atan2_459A40( dir[a1], dir[a2] );
            g_nUpdateBits |= 0x104;
            cam->angles[angIdx] = (float)( RAD2DEG( ang ) );
        }
    }
    if ( wnd->m_nButtonstate == (int)( v6 | MK_SHIFT ) )
    {
        if ( g_bRotateMode || g_bPatchBendMode )
        {
            Ed_SnapToPoint_Chooser( wnd, snap, x, y );
            Ed_VectorCopyXY( wnd, g_vRotateOrigin, snap );
            if ( g_bPatchBendMode )
            { g_vBendOrigin[0] = snap[0]; g_vBendOrigin[1] = snap[1]; g_vBendOrigin[2] = snap[2]; }
            g_nUpdateBits |= 2;
        }
        else
        {
            Ed_SnapToPoint_Chooser( wnd, snap, x, y );
            g_nUpdateBits |= 0xC;
            if ( wnd->m_nViewType == ED_VIEW_XY )      { m_Camera_origin0 = snap[0]; m_Camera_origin1 = snap[1]; }
            else if ( wnd->m_nViewType != ED_VIEW_YZ ) { m_Camera_origin0 = snap[0]; m_Camera_origin1 = snap[2]; }
            else                                       { m_Camera_origin0 = snap[1]; m_Camera_origin1 = snap[2]; }
        }
    }
    else if ( buttons == 6 )
    {
        GetCursorPos( &wnd->m_ptCursor );
        g_nUpdateBits |= 0xC;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Ed_XY_MouseMoved - CXYWnd::XY_MouseMoved 0x468230, the full mouse-move dispatcher.
//   (A) idle (no button): iff g_bCrossHairs, hide+reshow the cursor and request an XY+camera
//       repaint so DrawCrosshair tracks the cursor (0x46824b).
//   (B) LMB-held OR alt+(RMB+shift): if a marquee toggle-state (toggle_unk03/04) is active and
//       we are NOT in a transient RECTANGLE point-edit mode (d_select_mode 12..15), re-dispatch
//       XY_MouseDown for buttons 5 / (9 & alt) / 13 (shift/ctrl box-extend), else clear the
//       toggles; with no toggle active it falls through to Drag_MouseMoved.
//   (C) sel_addpoint + RMB + alt: also the Drag_MouseMoved path (shared LABEL_41).
//   (D) camera positioning DURING the move (mirror of the XY_MouseDown camera tail) / RMB
//       view scroll.
//  KISAK: the scroll body omits the binary's ShowCursor(0) + toggle_unk02=fast_2d_view (the
//  matching ShowCursor(1)/fast-drag render path is not ported, so adding it partially would
//  strand the cursor); null-guards on the camera writes; synchronous Invalidate in place of
//  the binary's g_nUpdateBits-only deferred repaint.
// ═════════════════════════════════════════════════════════════════════════════
static void Ed_XY_MouseMoved( xywndState_t *wnd, int x, int y, unsigned int buttons )
{
    // ── (A) no button held: idle move ──
    // 0x46824b: with the crosshair on, the binary hides+reshows the cursor and requests a
    // repaint of the XY + camera views so the full-extent crosshair tracks the cursor.
    // The ShowCursor(FALSE)/ShowCursor(TRUE) pair is a net no-op on the display counter —
    // transcribed FAITHFULLY (it is what the binary does; only the repaint request matters).
    if ( !wnd->m_nButtonstate )
    {
        if ( g_bCrossHairs )
        {
            ShowCursor( FALSE );        // 0x46825e
            g_nUpdateBits |= 6;         // 0x468260
            ShowCursor( TRUE );         // 0x468269
        }
        return;
    }

    const select_t sel_mode = (select_t)g_qeglobals.d_select_mode;

    // ── NEW-BRUSH drag: plain LMB held, nothing selected at press, sel_brush mode ──
    // (XY_MouseMoved's first dispatch arm — must precede the generic LMB-drag path.)
    if ( wnd->m_nButtonstate == MK_LBUTTON && !wnd->m_bPress_selection && sel_mode == sel_brush )
    {
        Ed_NewBrushDrag( wnd, x, y );
        // == CWnd::Invalidate( FALSE ) + CWnd::UpdateWindow() on this view.
        ::InvalidateRect( wnd->m_hWnd, nullptr, FALSE );
        ::UpdateWindow( wnd->m_hWnd );
        Ed_InvalidateAllViews();   // the new/resized brush appears in XY + camera + Z
        return;
    }

    const bool alt = ( GetAsyncKeyState( VK_MENU ) < 0 );

    // ── (B) LMB-held, or alt+(RMB+shift): selection drag / marquee shift-ctrl extend ──
    bool doDrag = false;
    if ( ( wnd->m_nButtonstate & MK_LBUTTON ) != 0
         || ( wnd->m_nButtonstate == ( MK_RBUTTON | MK_SHIFT ) && alt ) )
    {
        if ( !g_qeglobals.toggle_unk03_mousedrag_state1 && !g_qeglobals.toggle_unk04_mousedrag_state2 )
        {
            doDrag = true;   // no marquee in progress → ordinary Drag_MouseMoved (LABEL_41)
        }
        else
        {
            // a marquee/area box-select drag is in progress
            const int sm = (int)sel_mode;
            if ( sm != 12 && sm != 13 && sm != 14 && sm != 15 )   // not a RECTANGLE point-edit mode
            {
                if ( buttons == 5 || ( buttons == 9 && alt ) || buttons == 13 )
                    Ed_XY_MouseDown( wnd, x, y, buttons );          // shift/ctrl box-extend
                else
                {
                    g_qeglobals.toggle_unk03_mousedrag_state1 = 0;
                    g_qeglobals.toggle_unk04_mousedrag_state2 = 0;
                }
            }
            else
            {
                g_qeglobals.toggle_unk03_mousedrag_state1 = 0;
                g_qeglobals.toggle_unk04_mousedrag_state2 = 0;
            }
            return;
        }
    }
    // ── (C) add-point mode, RMB + alt: also the Drag_MouseMoved path ─────────────────
    else if ( sel_mode == sel_addpoint && wnd->m_nButtonstate == MK_RBUTTON && alt )
    {
        doDrag = true;
    }

    if ( doDrag )   // LABEL_41: select / move the dragged geometry
    {
        float trace_start[3], trace_dir[3];
        Ed_XY_ToPoint( wnd, x, y, trace_start, trace_dir );
        Drag_MouseMoved( x, y, (int)buttons, trace_start, trace_dir );
        // Synchronous repaint so the brush tracks the cursor in real time (a queued
        // Invalidate trails behind fast drags → the "floaty" feel).
        ::InvalidateRect( wnd->m_hWnd, nullptr, FALSE );
        ::UpdateWindow( wnd->m_hWnd );
        return;
    }

    // ── (D) camera positioning during the move / view scroll ─────────────────────────
    const unsigned int v6 = ( g_PrefsDlg->m_nMouseButtons != 2 ) ? MK_MBUTTON : MK_RBUTTON;
    if ( wnd->m_nButtonstate != (int)( v6 | MK_CONTROL ) || alt )
    {
        if ( wnd->m_nButtonstate == (int)( v6 | MK_SHIFT ) )
        {
            float snap[3]; Ed_SnapToPoint_Chooser( wnd, snap, x, y );
            if ( g_bRotateMode || g_bPatchBendMode )
            {
                Ed_VectorCopyXY( wnd, g_vRotateOrigin, snap );
                if ( g_bPatchBendMode )
                { g_vBendOrigin[0] = snap[0]; g_vBendOrigin[1] = snap[1]; g_vBendOrigin[2] = snap[2]; }
                g_nUpdateBits |= 2;   // W_XY
            }
            else   // place the XY camera icon
            {
                g_nUpdateBits |= 0xC;   // W_XY_OVERLAY | W_Z
                if ( wnd->m_nViewType == ED_VIEW_XY )      { m_Camera_origin0 = snap[0]; m_Camera_origin1 = snap[1]; }
                else if ( wnd->m_nViewType != ED_VIEW_YZ ) { m_Camera_origin0 = snap[0]; m_Camera_origin1 = snap[2]; }
                else                                       { m_Camera_origin0 = snap[1]; m_Camera_origin1 = snap[2]; }
            }
        }
        else
        {
            const int nmb = g_PrefsDlg->m_nMouseButtons;
            if ( ( nmb == 3 && wnd->m_nButtonstate == MK_MBUTTON )
                 || ( nmb == 2 && wnd->m_nButtonstate == ( MK_RBUTTON | MK_SHIFT | MK_CONTROL ) ) )
            {
                // U-GLOBALS: writes the 3D camera angle through Ed_Camera() (never NULL).
                {
                    float snap[3]; Ed_SnapToPoint_Chooser( wnd, snap, x, y );
                    camera_s *cam = Ed_Camera();
                    float dir[3] = { snap[0] - cam->origin[0], snap[1] - cam->origin[1],
                                     snap[2] - cam->origin[2] };   // IDA VectorSubtract(snap, cam.origin, dir)
                    const int a1     = ( wnd->m_nViewType != ED_VIEW_XY ) + 1;
                    const int a2     = ( wnd->m_nViewType == ED_VIEW_YZ );
                    const int angIdx = ( wnd->m_nViewType == ED_VIEW_XY );
                    if ( dir[a1] != 0.0f || dir[a2] != 0.0f )
                    {
                        const double ang = Ed_Atan2_459A40( dir[a1], dir[a2] );
                        g_nUpdateBits |= 0x104;   // W_XY_OVERLAY | W_CAMERA_IFON (IDB 0x468516 = 0x104; W_CAMERA_IFON is 0x100)
                        cam->angles[angIdx] = (float)( RAD2DEG( ang ) );
                    }
                }
            }
            else if ( wnd->m_nButtonstate == MK_RBUTTON )
            {
                // view scroll (relative; cursor re-centred each move — port form, see header)
                POINT cur; GetCursorPos( &cur );
                if ( cur.x != wnd->m_ptCursor.x || cur.y != wnd->m_ptCursor.y )
                {
                    s_rmbScrolled = true;   // a real scroll → OnRButtonUp must NOT pop the menu
                    int nDim1 = ( wnd->m_nViewType == ED_VIEW_YZ ) ? 1 : 0;
                    int nDim2 = ( wnd->m_nViewType != ED_VIEW_XY ) + 1;
                    wnd->m_vOrigin[nDim1] -= (float)( cur.x - wnd->m_ptCursor.x ) / wnd->m_fScale;
                    wnd->m_vOrigin[nDim2] += (float)( cur.y - wnd->m_ptCursor.y ) / wnd->m_fScale;
                    SetCursorPos( wnd->m_ptCursor.x, wnd->m_ptCursor.y );
                    // == CWnd::Invalidate( FALSE ) on this view.
                    ::InvalidateRect( wnd->m_hWnd, nullptr, FALSE );
                    // Under RTT the InvalidateRect lands on a HIDDEN child and does nothing,
                    // so the pan must announce itself in g_nUpdateBits for the dirty gate.
                    g_nUpdateBits |= ( W_XY | W_XY_OVERLAY );
                }
            }
        }
    }
    else   // wnd->m_nButtonstate == (v6 | MK_CONTROL) && !alt → set the camera origin
    {
        // U-GLOBALS: writes the 3D camera origin through Ed_Camera() (never NULL).
        {
            float snap[3]; Ed_SnapToPoint_Chooser( wnd, snap, x, y );
            Ed_VectorCopyXY( wnd, Ed_Camera()->origin, snap );
            g_nUpdateBits |= 5;   // W_CAMERA | W_XY_OVERLAY
        }
    }
}

// =============================================================================
//  THE CLIPPER (split a brush along a clip plane) - the CXYWnd cluster.  IDB: SetClipMode
//  0x465430, DropClipPoint 0x463d70, ProduceSplitLists 0x464040, Clip 0x46dc10, FlipClip
//  0x46ddf0, DrawClipper 0x4656c0; the split primitive Brush_SplitBrushByFace (0x471960)
//  lives in brush.cpp.  Workflow: enter clip mode -> left-click drops 1-3 grid-snapped clip
//  points -> DrawClipper previews the front/back split -> Enter commits the kept side, X
//  swaps it.  Two points + the view's depth axis define the plane in a 2D view; a third makes
//  it fully 3D.  CoD divergence from GtkRadiant: the split FACE material is SYNTHESISED
//  (caulk, or nodraw_decal if the source has a decal face) via the same MaterialDef build CSG
//  uses - NOT GtkRadiant's "inherit face 0" copy.
// =============================================================================
extern selbrush_t *Brush_AddToList( brush_t *def, entity_s *owner );          // brush.cpp 0x475980
extern void        Brush_AddToList2( selbrush_t *b );                         // brush.cpp 0x4765a0
extern void        Brush_RemoveFromList( selbrush_t *b );                     // brush.cpp 0x476680
extern void        Brush_Free( selbrush_t *b );                               // brush.cpp 0x475ba0
extern void        Brush_SplitBrushByFace( brush_t *in, face_t *face,
                                           brush_t **front, brush_t **back ); // brush.cpp 0x471960
extern void        Select_Delete();                                           // select.cpp 0x48e760
extern void        SetMaterial( const char *name, patchMesh_material *out );  // materialdef.cpp 0x431520
extern qtexture_s *MaterialDef_GetLayeredMaterial( MaterialDef *m );          // materialdef.cpp 0x4314a0
namespace LayerMat { int GetCurrentLayer( MaterialDef *m ); }                 // materialdef.cpp 0x431b30
extern void        OrientationPosToWorldPos( float *out, const float *localPos,
                                             const orientation_t *orient );   // entity.cpp 0x4ba430
extern int         DrawShadedWireframe( int cullMode, face_t *face, const orientation_t *orient,
                                        GfxColor *lineColor, char width, int vertCount,
                                        int vertLimit, GfxPointVertex *verts );  // brush.cpp 0x47b5c0

// The split-preview wireframe colour (IDB flt_6DE1B0 = magenta); Byte4PackPixelColor reads 4 floats.
static const float s_clipPreview[4] = { 1.0f, 0.0f, 1.0f, 1.0f };

// CClipPoint::Reset
static void Ed_ClipPointReset( CClipPoint *p )
{
    p->m_ptClip[0] = p->m_ptClip[1] = p->m_ptClip[2] = 0.0f;
    p->m_pVec3 = nullptr;
    p->m_bSet  = false;
}

// Free a clip front/back split list (CleanList) and self-point the head.
static void Ed_FreeSplitList( selbrush_t *head )
{
    for ( selbrush_t *b = head->next; b && b != head; )
    {
        selbrush_t *next = b->next;
        Brush_Free( b );
        b = next;
    }
    head->next = head;
    head->prev = head;
}

// Brush_CopyList (IDB 0x464740) -- MOVE every brush from `from`'s display list into
// `pTo`'s display list. Faithful to the disasm (the hex-rays `onext`/`oprev` are the
// entity_brush_s 0x00/0x04 misnaming; the loads are really next@4 / prev@0 -- the
// DISPLAY list). When pTo IS selected_brushes it routes through Brush_AddToList2.
static void Brush_CopyList( selbrush_t *pTo, selbrush_t *from )
{
    for ( selbrush_t *b = from->next; b && b != from; )
    {
        selbrush_t *next = b->next;
        Brush_RemoveFromList( b );
        if ( b->next || b->prev )
            Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
        if ( pTo == &selected_brushes )
        {
            Brush_AddToList2( b );
        }
        else
        {
            b->next = pTo->next;
            pTo->next->prev = b;
            pTo->next = b;
            b->prev = pTo;
        }
        b = next;
    }
}

// CXYWnd::SetClipMode (IDB 0x465430) -- enter/leave clip mode. Entering resets the 3
// clip points and the split lists; leaving releases the moving-clip capture, frees the
// split lists, and requests a redraw. Always clears g_bRogueClipMode.
void Ed_SetClipMode( char bMode )
{
    g_bClipMode = bMode;
    g_bRogueClipMode = 0;
    if ( bMode )
    {
        Ed_ClipPointReset( &g_Clip1 );
        Ed_ClipPointReset( &g_Clip2 );
        Ed_ClipPointReset( &g_Clip3 );
        Ed_FreeSplitList( &g_brFrontSplits );
        Ed_FreeSplitList( &g_brBackSplits );
    }
    else
    {
        if ( g_pMovingClip )
        {
            ReleaseCapture();
            g_pMovingClip = nullptr;
        }
        Ed_FreeSplitList( &g_brFrontSplits );
        Ed_FreeSplitList( &g_brBackSplits );
        g_nUpdateBits |= 0x5;     // W_XY | W_CAMERA_IFON
    }
}

// CXYWnd::DropClipPoint (IDB 0x463d70) -- place the next clip point (1->2->3) at the
// grid-snapped cursor. The two in-plane axes come from the snap; the DEPTH axis (the
// axis not in this 2D view) is seeded from the new-brush template bounds (d_new_brush_top
// for points 1/2, _bottom for 3) so the clip plane spans the view depth. Dropping a 4th
// point cycles. If a clip point is being dragged (g_pMovingClip), re-snap it instead.
//
// KISAK: raw client coords throughout (the firstlight render maps world-up to screen-up); the
// IDB's Rect.bottom-y-1 flip is the other convention - double-flipping would mirror the clip.
void Ed_DropClipPoint( xywndState_t *wnd, int x, int y )
{
    if ( g_pMovingClip )
    {
        // P5 RTT: shell owns drag-capture; OS-capturing the hidden child steals the mouse from
        // the ImGui host, so the clip-point drag SetCapture is dropped here.
        if ( g_PrefsDlg->m_bNoClamp ) Ed_SnapToPoint( wnd, g_pMovingClip, x, y );
        else                          Ed_SnapToGrid ( wnd, g_pMovingClip, x, y );
        g_nUpdateBits |= 0x5;
        return;
    }

    // The depth axis for this 2D view (the axis the snap does NOT fill) = the axis NOT shown.
    // IDA 0x463d70: v9 == m_nViewType for all 3 views (XY(2)->Z=2, XZ(1)->Y=1, YZ(0)->X=0).
    int depthAxis;
    if ( wnd->m_nViewType == ED_VIEW_XY )      depthAxis = 2;   // XY view -> Z is depth
    else                                       depthAxis = ( wnd->m_nViewType != ED_VIEW_YZ );  // XZ->Y(1), YZ->X(0)

    CClipPoint *pt;
    float depthVal;
    if ( !g_Clip1.m_bSet )
    {
        pt = &g_Clip1;
        depthVal = ( &g_qeglobals.d_new_brush_top_x )[depthAxis];
    }
    else if ( !g_Clip2.m_bSet )
    {
        pt = &g_Clip2;
        depthVal = ( &g_qeglobals.d_new_brush_top_x )[depthAxis];
    }
    else if ( !g_Clip3.m_bSet )
    {
        pt = &g_Clip3;
        depthVal = ( &g_qeglobals.d_new_brush_bottom_x )[depthAxis];
    }
    else
    {
        // 4th drop -> cycle: reset clip mode (keep the rogue flag) and start over at 1.
        char rogue = g_bRogueClipMode;
        Ed_SetClipMode( 1 );
        g_bRogueClipMode = rogue;
        pt = &g_Clip1;
        depthVal = ( &g_qeglobals.d_new_brush_bottom_x )[depthAxis];
    }

    pt->m_ptClip[depthAxis] = depthVal;
    pt->m_ptScreen.x = x;
    pt->m_ptScreen.y = y;
    pt->m_bSet = true;

    if ( g_PrefsDlg->m_bNoClamp ) Ed_SnapToPoint( wnd, pt->m_ptClip, x, y );
    else                          Ed_SnapToGrid ( wnd, pt->m_ptClip, x, y );
    g_nUpdateBits |= 0x5;
}

// Clip-point hover/move detection (IDB OnMouseMove clip branch, 0x4651d5). In clip mode,
// while not dragging: if the cursor is within 3 world-units of a set clip point (both
// in-plane axes) latch it as the moving clip point and show the cross cursor. Returns
// true only if a clip point is actively being dragged (consumes the move).
static bool Ed_ClipMouseMoved( xywndState_t *wnd, int x, int y )
{
    if ( !g_bClipMode )
        return false;

    if ( g_pMovingClip && GetCapture() == wnd->m_hWnd )
    {
        if ( g_PrefsDlg->m_bNoClamp ) Ed_SnapToPoint( wnd, g_pMovingClip, x, y );
        else                          Ed_SnapToGrid ( wnd, g_pMovingClip, x, y );
        g_nUpdateBits |= 0x5;
        SetCursor( LoadCursorA( 0, IDC_CROSS ) );
        return true;
    }

    const int a0 = ( wnd->m_nViewType == ED_VIEW_YZ ) ? 1 : 0;
    const int a1 = ( wnd->m_nViewType == ED_VIEW_XY ) ? 1 : 2;

    float tdp[3] = { 0, 0, 0 };
    if ( g_PrefsDlg->m_bNoClamp ) Ed_SnapToPoint( wnd, tdp, x, y );
    else                          Ed_SnapToGrid ( wnd, tdp, x, y );

    g_pMovingClip = nullptr;
    CClipPoint *cands[3] = { &g_Clip1, &g_Clip2, &g_Clip3 };
    bool hover = false;
    for ( CClipPoint *c : cands )
    {
        if ( c->m_bSet
             && fabsf( c->m_ptClip[a0] - tdp[a0] ) < 3.0f
             && fabsf( c->m_ptClip[a1] - tdp[a1] ) < 3.0f )
        {
            g_pMovingClip = c->m_ptClip;
            hover = true;
        }
    }
    if ( hover )
        SetCursor( LoadCursorA( 0, IDC_CROSS ) );
    return false;   // hover doesn't consume the move; the normal XY_MouseMoved still runs
}

// ─────────────────────────────────────────────────────────────────────────────
// KIWI-UX (shakeout G) FORWARDER — the CLIP FACE's material synthesis, hoisted
// out of Ed_ProduceSplitLists BYTE FOR BYTE and called from there.
//
// WHY IT EXISTS.  The modern Cut (C) and Face Split (Ctrl+R) verbs
// (kiwi_split.cpp) split a brush with the SAME ported core the clipper uses,
// Brush_SplitBrushByFace (brush.cpp 0x471960), which takes a TEMPLATE face_t and
// Face_Alloc's a copy of it onto each half.  That template's `mtldef[0..2]` is
// what the two new cut faces are textured with, and a zeroed one would hand the
// halves an unrealised MaterialDef.  The synthesis is a CoD divergence with three
// channels and three different texture scales — exactly the kind of thing that
// must not exist twice.
//
// NOTHING IS CHANGED, only moved: the decal classification, the three
// SetMaterial calls and the three size writes are the original statements in the
// original order.  The PLANEPTS stay at the call site, because those are the
// clipper's own view-depth rule and have nothing to do with the material.
void Ed_BuildClipFaceMaterial_Kiwi( face_t *out, const brush_t *src )
{
    if ( !out || !src )
        return;

    // Source material classification: nodraw_decal if ANY source face is a decal, else
    // caulk. (The IDB realizes each face's MaterialDef and stops on the first decal.)
    bool anyDecal = false;
    const int layer = g_qeglobals.current_edit_layer;
    for ( int f = 0; f < src->faceCount; ++f )
    {
        MaterialDef *md = &src->faces[f].mtldef[layer];
        const char *nm = ( md->lyrMtl ) ? (const char *)md->lyrMtl
                                        : ( md->radMtl ? md->radMtl->name : nullptr );
        if ( nm && !_stricmp( nm, "nodraw_decal" ) ) { anyDecal = true; break; }
    }
    const char *clipMtl = anyDecal ? "nodraw_decal" : "caulk";

    // Channel 0 (colormap) at the current layer; scale = tex dims * 0.25.
    SetMaterial( clipMtl, (patchMesh_material *)&out->mtldef[0] );
    {
        qtexture_s *t = MaterialDef_GetLayeredMaterial( &out->mtldef[0] );
        int w = t ? t->width : 512, h = t ? t->height : 512;
        int l0 = LayerMat::GetCurrentLayer( &out->mtldef[0] );
        out->mtldef[0].mat_texDef.size[l0 * 7 + 0] = (float)w * 0.25f;
        out->mtldef[0].mat_texDef.size[l0 * 7 + 1] = (float)h * 0.25f;
    }
    // Channel 1 (lightmap_gray); scale = tex dims * 16.
    SetMaterial( "lightmap_gray", (patchMesh_material *)&out->mtldef[1] );
    {
        qtexture_s *t = MaterialDef_GetLayeredMaterial( &out->mtldef[1] );
        int w = t ? t->width : 512, h = t ? t->height : 512;
        int l1 = LayerMat::GetCurrentLayer( &out->mtldef[1] );
        out->mtldef[1].mat_texDef.size[l1 * 7 + 0] = (float)w * 16.0f;
        out->mtldef[1].mat_texDef.size[l1 * 7 + 1] = (float)h * 16.0f;
    }
    // Channel 2 (smoothing_hard); scale = tex dims * 0.25.
    SetMaterial( "smoothing_hard", (patchMesh_material *)&out->mtldef[2] );
    {
        qtexture_s *t = MaterialDef_GetLayeredMaterial( &out->mtldef[2] );
        int w = t ? t->width : 512, h = t ? t->height : 512;
        int l2 = LayerMat::GetCurrentLayer( &out->mtldef[2] );
        out->mtldef[2].mat_texDef.size[l2 * 7 + 0] = (float)w * 0.25f;
        out->mtldef[2].mat_texDef.size[l2 * 7 + 1] = (float)h * 0.25f;
    }
}

// CXYWnd::ProduceSplitLists (IDB 0x464040) -- rebuild g_brFrontSplits / g_brBackSplits
// by splitting every selected brush along the clip plane (through g_Clip1/2/3). First
// de-selects clip-incompatible brushes (patches / physics primitives / fixed-size
// entities) back into active_brushes, frees any prior split brushes, then (in clip mode,
// 2+ points placed) splits each remaining selected brush via Brush_SplitBrushByFace and
// links the front piece into g_brFrontSplits, the back into g_brBackSplits.
void Ed_ProduceSplitLists()
{
    // -- 1. Move clip-incompatible brushes out of the selection --
    bool warnedFixed = false, warnedPatch = false, warnedPhys = false;
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; )
    {
        selbrush_t *bnext = b->next;   // capture before unlink
        bool incompatible = false;
        if ( b->patch )
        {
            if ( !warnedPatch ) { warnedPatch = true; Sys_Printf( "Deselecting patches for clip operation.\n" ); }
            incompatible = true;
        }
        else if ( b->def->numberId )    // physics primitive (IDB total_size_0x58 @ 0x54)
        {
            if ( !warnedPhys ) { warnedPhys = true; Sys_Printf( "Deselecting physics primitives for clip operation.\n" ); }
            incompatible = true;
        }
        // fixed-size entity (IDB owner->def->eclass, the entity_s_def
        // path -- matches SelectFaceSth; NOT entity_s.eclass which is the def-owner path).
        else if ( *(int *)&( (entity_s_def *)b->owner->def )->eclass->fixedsize )
        {
            if ( !warnedFixed ) { warnedFixed = true; Sys_Printf( "Deselecting fixed size entities for clip operation.\n" ); }
            incompatible = true;
        }
        if ( incompatible )
        {
            // DESELECT: head-insert into active_brushes (the binary's inline splice at
            // 0x464040).  NOT Brush_AddToList2 - that re-links into selected_brushes.
            Brush_RemoveFromList( b );
            if ( b->next || b->prev )
                Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
            b->next = active_brushes.next;
            active_brushes.next->prev = b;
            active_brushes.next = b;
            b->prev = &active_brushes;
        }
        b = bnext;
    }

    // -- 2. Free any prior split brushes --
    Ed_FreeSplitList( &g_brFrontSplits );
    Ed_FreeSplitList( &g_brBackSplits );

    if ( !g_bClipMode || !g_Clip1.m_bSet || !g_Clip2.m_bSet )
        return;

    // -- 3. Split each remaining selected brush by the clip plane --
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
    {
        // Build the clip FACE: planepts from the 3 clip points + a synthesised
        // caulk/nodraw_decal material (CoD divergence). The material build matches the CSG
        // caulk path and uses SetMaterial, so the headless gate keeps the proven
        // degenerate-lyrMtl path (g_radiantFirstLightRendererReady==false).
        face_t clipFace{};

        // KIWI-UX (shakeout G): the material half of this block moved VERBATIM into
        // Ed_BuildClipFaceMaterial_Kiwi (just below Ed_SplitClip), so the modern
        // Cut / Face-Split verbs (kiwi_split.cpp) build the SAME caulk /
        // nodraw_decal clip face this clipper does instead of a second copy of it.
        // Only the planepts stay here: they are the CLIPPER's own view-depth logic.
        Ed_BuildClipFaceMaterial_Kiwi( &clipFace, i->def );

        // Plane points: clip1, clip2, and clip3 (or a view-depth synthetic third point).
        clipFace.planepts[0][0] = g_Clip1.m_ptClip[0];
        clipFace.planepts[0][1] = g_Clip1.m_ptClip[1];
        clipFace.planepts[0][2] = g_Clip1.m_ptClip[2];
        clipFace.planepts[1][0] = g_Clip2.m_ptClip[0];
        clipFace.planepts[1][1] = g_Clip2.m_ptClip[1];
        clipFace.planepts[1][2] = g_Clip2.m_ptClip[2];
        if ( g_Clip3.m_bSet )
        {
            clipFace.planepts[2][0] = g_Clip3.m_ptClip[0];
            clipFace.planepts[2][1] = g_Clip3.m_ptClip[1];
            clipFace.planepts[2][2] = g_Clip3.m_ptClip[2];
        }
        else
        {
            // Only 2 points placed -> make the third by lifting clip1 +32 along the view's
            // DEPTH axis (the axis NOT shown, so the plane is perpendicular to the 2D view).
            // IDA 0x464040: vt==2(XY)->[2]+32, vt!=0(==1,XZ)->[1]+32, else(==0,YZ)->[0]+32.
            // [BUGFIX: was XZ->[0]/YZ->[1] (swapped) — same depth-axis confusion as DropClipPoint.]
            // (was g_pParentWnd->m_pActiveXY->m_nViewType — the same singleton view, now via
            // the shell-agnostic accessor; every entry point into this function syncs the MFC
            // class members onto the state first.)
            int vt = Ed_ActiveXY()->m_nViewType;
            clipFace.planepts[2][0] = g_Clip1.m_ptClip[0];
            clipFace.planepts[2][1] = g_Clip1.m_ptClip[1];
            clipFace.planepts[2][2] = g_Clip1.m_ptClip[2];
            if ( vt == ED_VIEW_XY )      clipFace.planepts[2][2] += 32.0f;   // XY(2) depth = Z
            else if ( vt == ED_VIEW_XZ ) clipFace.planepts[2][1] += 32.0f;   // XZ(1) depth = Y
            else                          clipFace.planepts[2][0] += 32.0f;  // YZ(0) depth = X
        }

        brush_t *front = nullptr, *back = nullptr;
        Brush_SplitBrushByFace( i->def, &clipFace, &front, &back );

        if ( back )
        {
            selbrush_t *nb = Brush_AddToList( back, i->owner );
            if ( nb->next || nb->prev )                       // binary already-linked guard
                Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
            nb->next = g_brBackSplits.next;
            g_brBackSplits.next->prev = nb;
            g_brBackSplits.next = nb;
            nb->prev = &g_brBackSplits;
        }
        if ( front )
        {
            selbrush_t *nf = Brush_AddToList( front, i->owner );
            if ( nf->next || nf->prev )                       // binary already-linked guard
                Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
            nf->next = g_brFrontSplits.next;
            g_brFrontSplits.next->prev = nf;
            g_brFrontSplits.next = nf;
            nf->prev = &g_brFrontSplits;
        }
    }
}

// CXYWnd::Clip (IDB 0x46dc10) -- COMMIT the clip. Produce the split lists, choose the
// KEEP side (front or back, per g_bSwitch + m_bSwitchClip + the view type), stash the
// kept brushes in a temp list, free both split lists, delete the original selection,
// move the kept brushes into the selection, then leave clip mode + request a full redraw.
//   Keep-side (verbatim): useRawSwitch = m_bSwitchClip ? isXZ : !isXZ; v2 = useRawSwitch ?
//   g_bSwitch : !g_bSwitch; keep = (v2 == 0) ? back : front.  isXZ is a BOOLEAN, not a 3-way
//   axis pick - there is no depth-axis bug here.
void Ed_Clip( xywndState_t *wnd )
{
    if ( !g_bClipMode )
        return;

    Ed_ProduceSplitLists();

    // Pick which split list to keep (IDB 0x46dc10 verbatim):
    //   v2 = useRawSwitch ? g_bSwitch : !g_bSwitch;  keep = (v2==0) ? back : front
    //   useRawSwitch = m_bSwitchClip ? (view==XZ) : (view!=XZ)
    const bool isXZ = ( wnd->m_nViewType == ED_VIEW_XZ );    // XZ == 1
    const bool useRawSwitch = g_PrefsDlg->m_bSwitchClip ? isXZ : !isXZ;
    const int  v2 = useRawSwitch ? (int)(byte)g_bSwitch : ( g_bSwitch == 0 );
    selbrush_t *keep = ( v2 == 0 ) ? &g_brBackSplits : &g_brFrontSplits;

    if ( keep->next == keep )      // nothing produced -> leave the selection untouched
        return;

    // IDB exactly: stash the kept brushes in a temp list, free BOTH split lists (the
    // not-kept brushes get freed; the kept ones are now in temp, out of `keep`), delete
    // the original selection, then move the kept brushes into selected_brushes.
    selbrush_t temp{};
    temp.next = &temp;
    temp.prev = &temp;
    Brush_CopyList( &temp, keep );

    Ed_FreeSplitList( &g_brFrontSplits );
    Ed_FreeSplitList( &g_brBackSplits );

    Select_Delete();   // delete the original selected brushes (selected_brushes now empty)

    Brush_CopyList( &selected_brushes, &temp );

    Ed_SetClipMode( g_bRogueClipMode == 0 );
    g_bRogueClipMode = 0;
    g_nUpdateBits = -1;
}

// CXYWnd::SplitClip (IDB 0x46dd30) -- commit the clip keeping BOTH sides.  Same shape as
// Ed_Clip above but instead of picking a keep-side it moves the front list AND the back list
// into the selection, so the original brush is replaced by its two halves.  Bails (leaving
// clip mode armed) if either split list came back empty.  Wired to CMainFrame::OnSplitSelected
// (menu 32794 Selection>Clipper>Split selected / Shift+Enter).
// Faithful to the disasm: the two free-loops after the copies are no-ops because Brush_CopyList
// already emptied both lists; the SetClipMode arg is `g_bRogueClipMode == 0` (0x46DDBE cmp/jz),
// and g_bRogueClipMode is cleared afterwards.  The binary does NOT set g_nUpdateBits here
// (Ed_SetClipMode's leave path already requests W_XY|W_CAMERA_IFON).
void Ed_SplitClip()
{
    // U-GLOBALS: reached straight from CMainFrame::OnSplitSelected (mainfrm.cpp 5218), i.e. NOT
    // through a CXYWnd handler.  The transitional XYWnd_MfcSyncIn( m_pActiveXY ) that used to run
    // here is gone: m_nViewType (what Ed_ProduceSplitLists reads) now lives ONLY in g_xywndState.
    Ed_ProduceSplitLists();

    if ( g_brFrontSplits.next == &g_brFrontSplits )
        return;
    if ( g_brBackSplits.next == &g_brBackSplits )
        return;

    Select_Delete();
    Brush_CopyList( &selected_brushes, &g_brFrontSplits );
    Brush_CopyList( &selected_brushes, &g_brBackSplits );

    Ed_FreeSplitList( &g_brFrontSplits );
    Ed_FreeSplitList( &g_brBackSplits );

    Ed_SetClipMode( g_bRogueClipMode == 0 );
    g_bRogueClipMode = 0;
}

// CXYWnd::FlipClip (IDB 0x46ddf0) -- toggle which side the clip keeps.
void Ed_FlipClip()
{
    g_nUpdateBits |= 0x5;
    g_bSwitch = ( g_bSwitch == 0 );
}

// ─── CXYWnd::DrawCrosshair (IDB 0x4655d0) ────────────────────────────────────
// The Shift+X full-extent crosshair: two ±131072-unit lines crossing at g_tdp (the
// snapped world point under the cursor, maintained by CXYWnd::OnMouseMove 0x464b10),
// in the view's two in-plane axes.  The axis pair is the same one PaintSizeInfo uses:
// dim1 = (m_nViewType == YZ) @0x46561b, dim2 = (m_nViewType != XY) + 1 @0x465625.
// Colour {0.2, 0.9, 0.2, 0.8}; ONE 2-line batch at width 1.
static void Ed_DrawCrosshair( xywndState_t *wnd )
{
    float          from[4];
    GfxColor       gfx_col;
    GfxPointVertex verts[4];

    const int dim1 = ( wnd->m_nViewType == ED_VIEW_YZ );        // 0x46561b
    const int dim2 = ( wnd->m_nViewType != ED_VIEW_XY ) + 1;    // 0x465625

    from[0] = 0.2f;          // 0x4655ef
    from[1] = 0.89999998f;   // 0x4655fc
    from[2] = 0.2f;          // 0x465604
    from[3] = 0.80000001f;   // 0x465613
    Byte4PackPixelColor( from, &gfx_col );                      // 0x465627

    for ( int i = 0; i < 4; ++i )                               // 0x465632..0x46566c
    {
        verts[i].xyz[0] = g_tdp[0];
        verts[i].xyz[1] = g_tdp[1];
        verts[i].xyz[2] = g_tdp[2];
        *(GfxColor *)verts[i].color = gfx_col;                  // 0x46568b..0x465694
    }
    verts[0].xyz[dim1] = -131072.0f;                            // 0x465675
    verts[1].xyz[dim1] =  131072.0f;                            // 0x46567f
    verts[3].xyz[dim2] =  131072.0f;                            // 0x465683
    verts[2].xyz[dim2] = -131072.0f;                            // 0x465687

    R_AddCmd_Line3D( 2, 1, verts );                             // 0x465697
}

// CXYWnd::DrawClipper (IDB 0x4656c0) -- render the placed clip points (numbered square
// markers + "1"/"2"/"3" labels) and, once two points are placed, a wireframe preview of
// the kept split side. Drawn through the identity orientation (world_orient_matrix);
// colours from d_savedinfo.colors[12] (the clip-point colour).
// Split-preview keep-side v37 = (vt == XZ) ? !g_bSwitch : g_bSwitch - NO m_bSwitchClip here,
// unlike Clip.  KISAK divergences: (1) the label half-pixel centering (CXYWnd+92/+96 flags,
// unmodelled by the port's MFC-reconstructed CXYWnd) is dropped - sub-pixel; (2) the
// split-preview wireframe uses a dedicated 5450-vertex buffer where the binary reuses the
// 3-vertex clip-MARKER buffer with vertLimit=3 (0x465b05 `push 3`), a latent original bug
// that truncates the preview to ~1 line.
void Ed_DrawClipper( xywndState_t *wnd )
{
    GfxColor clipCol;
    Byte4PackPixelColor( g_qeglobals.d_savedinfo.colors[12], &clipCol );

    // The placed points as square markers (RC_DRAW_POINTS).
    GfxPointVertex pts[3];
    int n = 0;
    if ( g_Clip1.m_bSet )
    {
        OrientationPosToWorldPos( pts[n].xyz, g_Clip1.m_ptClip, (const orientation_t *)world_orient_matrix );
        *(GfxColor *)pts[n].color = clipCol; ++n;
    }
    if ( g_Clip2.m_bSet )
    {
        OrientationPosToWorldPos( pts[n].xyz, g_Clip2.m_ptClip, (const orientation_t *)world_orient_matrix );
        *(GfxColor *)pts[n].color = clipCol; ++n;
    }
    if ( g_Clip3.m_bSet )
    {
        OrientationPosToWorldPos( pts[n].xyz, g_Clip3.m_ptClip, (const orientation_t *)world_orient_matrix );
        *(GfxColor *)pts[n].color = clipCol; ++n;
    }
    if ( n )
        R_AddPointCmd_W( (short)n, 4, pts );

    // Numbered labels next to each point (screen-aligned at the view scale).
    Font_s *font = (Font_s *)g_qeglobals.d_font_list;
    const float inv = 1.0f / wnd->m_fScale;
    int nDim1 = ( wnd->m_nViewType == ED_VIEW_YZ ) ? 1 : 0;
    int nDim2 = ( wnd->m_nViewType != ED_VIEW_XY ) + 1;
    int nDim3 = ( wnd->m_nViewType == ED_VIEW_XY ) ? 2 : ( wnd->m_nViewType == ED_VIEW_XZ ? 1 : 0 );
    float xStep[3] = { 0, 0, 0 }, yStep[3] = { 0, 0, 0 };
    xStep[nDim1] = inv;
    yStep[nDim2] = -inv;
    const CClipPoint *cp[3] = { &g_Clip1, &g_Clip2, &g_Clip3 };
    const char *labels[3] = { "1", "2", "3" };
    for ( int k = 0; k < 3; ++k )
    {
        if ( !cp[k]->m_bSet ) continue;
        // org = ptClip + 2 on all 3 axes (the binary's v44[nDim1/2/3] = v47[nDim1/2/3]).
        float org[3];
        org[nDim1] = cp[k]->m_ptClip[nDim1] + 2.0f;
        org[nDim2] = cp[k]->m_ptClip[nDim2] + 2.0f;
        org[nDim3] = cp[k]->m_ptClip[nDim3] + 2.0f;
        R_AddCmdDrawTextAtPosition( labels[k], font, org, xStep, yStep,
                                    g_qeglobals.d_savedinfo.colors[12] );
    }

    // Preview the kept split side as wireframe.
    if ( g_Clip1.m_bSet && g_Clip2.m_bSet )
    {
        Ed_ProduceSplitLists();
        // IDB DrawClipper keep-side (distinct from Clip -- no m_bSwitchClip here):
        //   v37 = (view==XZ) ? (g_bSwitch==0) : g_bSwitch;  keep = v37 ? back : front.
        const int v37 = ( wnd->m_nViewType == ED_VIEW_XZ ) ? ( g_bSwitch == 0 )
                                                            : (int)(byte)g_bSwitch;
        selbrush_t *keep = v37 ? &g_brBackSplits : &g_brFrontSplits;

        GfxColor previewCol;
        Byte4PackPixelColor( const_cast<float *>( s_clipPreview ), &previewCol );

        // KISAK: dedicated 5450-vertex buffer instead of the binary's 3-vertex marker-buffer
        // reuse + vertLimit=3, which truncates the preview - see the header.
        static GfxPointVertex s_lineVerts[5450];
        int vertCount = 0;
        for ( selbrush_t *brush = keep->next; brush && brush != keep; brush = brush->next )
        {
            brush_t *d = brush->def;
            for ( int faceIndex = 0; faceIndex < d->faceCount; ++faceIndex )
            {
                iassert( brush->def->faces[faceIndex].w );   // XYWnd.cpp:1024
                vertCount = DrawShadedWireframe( -1, &d->faces[faceIndex],
                                                 (const orientation_t *)world_orient_matrix,
                                                 &previewCol, 1, vertCount, 5450, s_lineVerts );
            }
        }
        if ( vertCount )
            R_AddCmd_Line3D( (short)( vertCount / 2 ), 1, s_lineVerts );
    }
}

// Ed_DrawVertexHandles - the sel_vertex/sel_edge handle PREFIX of DrawConnectionLinks
// (0x40c9f0), which the binary calls from BOTH XY_Draw and Cam_Draw (so both call it here;
// d_points are world-space, transformed through the identity orientation).  Green points
// (flt_6DE140) for the vertices, blue edge-midpoints (flt_6DE160) for sel_edge; batched as
// RC_DRAW_POINTS, flushed at 1362 like the binary.
// KISAK REDUCTION: the full prefix also draws sub_40C4D0's red (1,0,0,0.25) overlay over
// dword_180A8E0 items, and the sel_vertex handles for the transient RECTANGLE point-edit
// modes 14/15 (not just sel_vertex).
void Ed_DrawVertexHandles()
{
    if ( g_qeglobals.d_select_mode == sel_vertex )
    {
        static const float s_vertGreen[4] = { 0.0f, 1.0f, 0.0f, 1.0f };  // flt_6DE140
        GfxColor col;
        Byte4PackPixelColor( const_cast<float *>( s_vertGreen ), &col );

        GfxPointVertex pts[1362];   // binary flushes the batch at 1361
        int n = 0;
        for ( int i = 0; i < g_qeglobals.d_numpoints; ++i )
        {
            OrientationPosToWorldPos( pts[n].xyz, g_qeglobals.d_points[i],
                                      (const orientation_t *)world_orient_matrix );
            *(GfxColor *)pts[n].color = col;
            if ( ++n == 1362 ) { R_AddPointCmd_W( (short)n, 4, pts ); n = 0; }
        }
        if ( n )
            R_AddPointCmd_W( (short)n, 4, pts );
    }
    else if ( g_qeglobals.d_select_mode == sel_edge )
    {
        static const float s_edgeBlue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };   // flt_6DE160
        GfxColor col;
        Byte4PackPixelColor( const_cast<float *>( s_edgeBlue ), &col );

        GfxPointVertex pts[1362];
        int n = 0;
        for ( int i = 0; i < g_qeglobals.d_numedges; ++i )
        {
            const float *p1 = g_qeglobals.d_points[g_qeglobals.d_edges[i].p1];
            const float *p2 = g_qeglobals.d_points[g_qeglobals.d_edges[i].p2];
            float mid[3] = { ( p1[0] + p2[0] ) * 0.5f,
                             ( p1[1] + p2[1] ) * 0.5f,
                             ( p1[2] + p2[2] ) * 0.5f };
            OrientationPosToWorldPos( pts[n].xyz, mid,
                                      (const orientation_t *)world_orient_matrix );
            *(GfxColor *)pts[n].color = col;
            if ( ++n == 1362 ) { R_AddPointCmd_W( (short)n, 4, pts ); n = 0; }
        }
        if ( n )
            R_AddPointCmd_W( (short)n, 4, pts );
    }
}

// ════════════════════════════════════════════════════════════════════════════════
//  CONNECTIONS overlay - View->Show->Connections (d_xyShowFlags bit 0x4; each function
//  SELF-GATES on it, drawing only when the bit is CLEAR).  Draws a line with chevron
//  arrowheads from each entity with a `target` epair to every entity whose `targetname`
//  matches, plus the CoD `script_linkTo`/`script_linkName` script-link variant.
//  IDB: Lines_AddLinkTo 0x46a2c0, Lines_AddLinkToScript 0x46b590, and the line-colour helper
//  Brush_GetEntityLineColor 0x47aa20 - all the tail of DrawConnectionLinks 0x40c9f0.
// ════════════════════════════════════════════════════════════════════════════════

// Brush_GetEntityLineColor (IDB 0x47aa20) lives in brush.cpp — its asserts are
// brush.cpp:4560, i.e. that IS its source file.  This TU used to carry a second
// static copy which shadowed the canonical one for the connections overlay; the two
// had drifted, so the copy is gone and both paths now share brush.cpp's body.
extern char Brush_GetEntityLineColor( float *outRgba, brush_t *brushDef,
                                      const float *inRgba );   // brush.cpp 0x47aa20

// Find an entity's first epair value for `key` (case-insensitive), or "" if absent.
// (Lines_AddLinkTo's inlined epair scan; the binary uses `zero` for the not-found case.)
static const char *Ed_EntFirstKey( entity_s *e, const char *key )
{
    for ( epair_t *ep = e->def
                        ? ((entity_s *)e->def)->epairs
                        : nullptr;
          ep; ep = ep->next )
        if ( !_stricmp( ep->key, key ) )
            return ep->value ? ep->value : "";
    return "";
}

// The head of an entity's representative (first) brush instance (brushes.ownerNext).
// Returns nullptr if the entity has no brushes (the self-pointing sentinel).
static inline selbrush_t *Ed_EntFirstBrushInst( entity_s *e )
{
    selbrush_t *first = e->brushes.ownerNext;
    return ( first == (selbrush_t *)&e->brushes ) ? nullptr : first;
}

// The eclass for an entity's line colour.  INSTANCE-vs-DEF trap: the binary reads the DEF
// eclass (*(_DWORD*)(ent+8)+0x60 = def->eclass), NOT the INSTANCE's own `eclass` field - for a
// prefab-instanced entity the instance eclass can be NULL.  entity_s_def.eclass is also at
// +0x60.  Falls back to the instance eclass / black if both are absent.
static inline const float *Ed_EntDefColor( entity_s *e )
{
    static const float kBlack[3] = { 0.0f, 0.0f, 0.0f };
    entity_s_def *def = (entity_s_def *)e->def;
    eclass_t *ec = def ? def->eclass : nullptr;
    if ( !ec ) ec = e->eclass;
    return ec ? ec->color : kBlack;
}

#define ED_LINK_MAX_ENTS  0x10000
#define ED_LINK_BATCH     1362        // GfxPointVertex flush threshold (binary: 1362)

// R_Add3DLine on channel 1 with the link batch limit (the binary's inline call shape).
static inline int Ed_AddLinkSeg( GfxPointVertex *verts, const orientation_t *orient,
                                 const float *p1, const float *p2, const GfxColor *col,
                                 int vertCount )
{
    return R_Add3DLine( verts, orient, p1, p2, (const unsigned int *)col, 1, vertCount, ED_LINK_BATCH );
}

// ── Lines_AddLinkTo (IDB 0x46a2c0) — target/targetname connection lines ─────────
// For every entity with a "target" epair whose representative brush is visible: draw
// a coloured line + chevron arrowheads to every entity whose "targetname" matches.
// If NO targetname matches, draw a single random-coloured "dangling" line to a random
// nearby point (the binary's unmatched-target indicator).
static void Lines_AddLinkTo()
{
    if ( ( g_qeglobals.d_savedinfo.d_xyShowFlags & 4 ) != 0 )
        return;                                  // connections hidden

    // Pass 1: gather every entity that HAS a targetname + a visible brush.
    static entity_s   *tnEnts[ED_LINK_MAX_ENTS];     // file-static: the IDB stack arrays
    static const char *tnVals[ED_LINK_MAX_ENTS];     //  are 256 KB each — too big for the stack
    int nTn = 0;
    for ( entity_s *e = entityInsts.next; e != &entityInsts; e = e->next )
    {
        if ( nTn == ED_LINK_MAX_ENTS )
            break;
        const char *tn = Ed_EntFirstKey( e, "targetname" );
        // The binary records the value for EVERY entity but only advances the index
        // when the targetname is non-empty AND the entity has a brush.
        if ( *tn && Ed_EntFirstBrushInst( e ) )
        {
            tnVals[nTn] = tn;
            tnEnts[nTn] = e;
            ++nTn;
        }
    }

    GfxPointVertex verts[ED_LINK_BATCH];
    const orientation_t *ident = (const orientation_t *)world_orient_matrix;
    int vertCount = 0;

    // Pass 2: for each entity with a "target", connect to the matching targetname(s).
    for ( entity_s *te = entityInsts.next; te != &entityInsts; te = te->next )
    {
        const char *target = Ed_EntFirstKey( te, "target" );
        if ( !*target )
            continue;
        selbrush_t *tb = Ed_EntFirstBrushInst( te );      // the "from" brush instance
        if ( !tb || FilterBrush( tb, 0 ) || ( tb->brushFlags & 2 ) != 0 )
            continue;

        bool anyMatch = false;
        for ( int i = 0; i < nTn; ++i )
        {
            if ( strcmp( tnVals[i], target ) )
                continue;
            anyMatch = true;
            entity_s   *me = tnEnts[i];
            selbrush_t *mb = me->brushes.ownerNext;       // matched entity's brush inst
            if ( FilterBrush( mb, 0 ) )
                continue;

            // line endpoints = bounding-box centres of the two instance brushes
            brush_t *fromDef = tb->def;
            float fromC[3] = { ( fromDef->mins[0] + fromDef->maxs[0] ) * 0.5f,
                               ( fromDef->mins[1] + fromDef->maxs[1] ) * 0.5f,
                               ( fromDef->mins[2] + fromDef->maxs[2] ) * 0.5f };
            brush_t *toDef = mb->def;
            float toC[3]   = { ( toDef->mins[0] + toDef->maxs[0] ) * 0.5f,
                               ( toDef->mins[1] + toDef->maxs[1] ) * 0.5f,
                               ( toDef->mins[2] + toDef->maxs[2] ) * 0.5f };

            // direction from→to, normalized; len = original distance (arrowhead spacing)
            float dir[3] = { fromC[0] - toC[0], fromC[1] - toC[1], fromC[2] - toC[2] };
            float len = Vec3Normalize_R( dir );

            // arrowhead wing offsets (±8 units, perpendicular in the XY plane)
            float ax = dir[0] * 8.0f, ay = dir[1] * 8.0f;
            float wing1[2] = { ax - ay, ay + ax };   // (v28,v29)
            float wing2[2] = { ax + ay, ay - ax };   // (v30,v31)

            // binary XYWnd.cpp:2881 — the matched brush instance must belong to the matched
            // entity.  tb/te are the binary's locals.
            selbrush_t *tb = mb;
            entity_s   *te = me;
            iassert( tb->owner && tb->owner == te );   // XYWnd.cpp:2881

            // line colour = matched entity's DEF eclass colour (§11 instance-vs-def: read the
            // DEF's eclass, not the instance's — see Ed_EntDefColor), overridden by the matched
            // brush's _color/actor/node_path via Brush_GetEntityLineColor (brushDef = mb->def,
            // matching the IDB's `v19 = (entity_brush_s*)v10->def`).
            const float *baseCol = Ed_EntDefColor( me );
            float rgba[4] = { baseCol[0], baseCol[1], baseCol[2], 1.0f };
            Brush_GetEntityLineColor( rgba, mb->def, rgba );
            GfxColor col;
            Byte4PackPixelColor( rgba, &col );

            // main line: matched-centre → target-centre
            vertCount = Ed_AddLinkSeg( verts, ident, toC, fromC, &col, vertCount );

            // chevron arrowheads spaced ~256 units along the line
            int chevrons = 1 - (int)( len * -0.00390625f );   // 1 + len/256
            for ( int c = 0; c < chevrons; ++c )
            {
                float t = ( (float)c + 0.5f ) * len / (float)chevrons;
                float base[3] = { t * dir[0] + toC[0],
                                  t * dir[1] + toC[1],
                                  t * dir[2] + toC[2] };
                float a1[3] = { base[0] + wing1[0], base[1] + wing1[1], base[2] };
                float a2[3] = { base[0] + wing2[0], base[1] + wing2[1], base[2] };
                vertCount = Ed_AddLinkSeg( verts, ident, base, a1, &col, vertCount );
                vertCount = Ed_AddLinkSeg( verts, ident, base, a2, &col, vertCount );
            }
        }

        if ( !anyMatch )
        {
            // unmatched target → one random-coloured line to a random nearby point
            brush_t *fromDef = tb->def;
            float fromC[3] = { ( fromDef->mins[0] + fromDef->maxs[0] ) * 0.5f,
                               ( fromDef->mins[1] + fromDef->maxs[1] ) * 0.5f,
                               ( fromDef->mins[2] + fromDef->maxs[2] ) * 0.5f };
            float rndPt[3];
            for ( int k = 0; k < 3; ++k )
                rndPt[k] = (float)rand() * 0.000030517578125f * 150.0f + fromC[k] - 75.0f;
            float rgba[4] = { (float)rand() * 0.000030517578125f,
                              (float)rand() * 0.000030517578125f,
                              (float)rand() * 0.000030517578125f, 1.0f };
            GfxColor col;
            Byte4PackPixelColor( rgba, &col );
            vertCount = Ed_AddLinkSeg( verts, ident, fromC, rndPt, &col, vertCount );
        }
    }
    if ( vertCount )
        R_AddCmd_Line3D( (short)( vertCount / 2 ), 1, verts );
}

// ── Map_ParseLinkList (IDB 0x48be20, file-static in select.cpp) ─────────────────
// Parse a space-separated list of unique integers into `out` (out[count]=-1 sentinel),
// max 30 entries.  Local copy here because the select.cpp original is file-static.
static int Ed_ParseLinkList( int *out, const char *str )
{
    int count = 0;
    bool atStart = true;
    size_t len = strlen( str );
    for ( size_t i = 0; i < len && count < 30; ++i )
    {
        if ( str[i] == ' ' ) { atStart = true; continue; }
        if ( atStart )
        {
            int v = atol( &str[i] );
            bool dup = false;
            for ( int k = 0; k < count; ++k )
                if ( out[k] == v ) { dup = true; break; }
            if ( !dup )
                out[count++] = v;
            atStart = false;
        }
    }
    out[count] = -1;
    return count;
}

// ── Lines_AddLinkToScript (IDB 0x46b590) — script_linkTo/script_linkName lines ──
// The CoD script-link variant: each entity's `script_linkTo` is a space-separated list
// of integers; draw a line (no arrowheads) to every entity whose `script_linkName`
// integer is in that list.
static void Lines_AddLinkToScript()
{
    if ( ( g_qeglobals.d_savedinfo.d_xyShowFlags & 4 ) != 0 )
        return;

    // Pass 1: gather entities with a non-zero script_linkName + a visible brush.
    static entity_s *lnEnts[ED_LINK_MAX_ENTS];
    static int       lnVals[ED_LINK_MAX_ENTS];
    int nLn = 0;
    for ( entity_s *e = entityInsts.next; e != &entityInsts; e = e->next )
    {
        if ( nLn == ED_LINK_MAX_ENTS )
            break;
        int v = atol( Ed_EntFirstKey( e, "script_linkName" ) );
        if ( v && Ed_EntFirstBrushInst( e ) )
        {
            lnVals[nLn] = v;
            lnEnts[nLn] = e;
            ++nLn;
        }
    }

    GfxPointVertex verts[ED_LINK_BATCH];
    const orientation_t *ident = (const orientation_t *)world_orient_matrix;
    int vertCount = 0;

    for ( entity_s *te = entityInsts.next; te != &entityInsts; te = te->next )
    {
        const char *linkTo = Ed_EntFirstKey( te, "script_linkTo" );
        if ( !*linkTo )
            continue;
        selbrush_t *tb = Ed_EntFirstBrushInst( te );
        if ( !tb || FilterBrush( tb, 0 ) )
            continue;

        int linkIds[32];
        int nIds = Ed_ParseLinkList( linkIds, linkTo );

        for ( int i = 0; i < nLn; ++i )
        {
            bool inList = false;
            for ( int k = 0; k < nIds; ++k )
                if ( linkIds[k] == lnVals[i] ) { inList = true; break; }
            if ( !inList )
                continue;
            selbrush_t *mb = lnEnts[i]->brushes.ownerNext;
            if ( FilterBrush( mb, 0 ) )
                continue;

            brush_t *fromDef = tb->def;
            float fromC[3] = { ( fromDef->mins[0] + fromDef->maxs[0] ) * 0.5f,
                               ( fromDef->mins[1] + fromDef->maxs[1] ) * 0.5f,
                               ( fromDef->mins[2] + fromDef->maxs[2] ) * 0.5f };
            brush_t *toDef = mb->def;
            float toC[3]   = { ( toDef->mins[0] + toDef->maxs[0] ) * 0.5f,
                               ( toDef->mins[1] + toDef->maxs[1] ) * 0.5f,
                               ( toDef->mins[2] + toDef->maxs[2] ) * 0.5f };

            // binary XYWnd.cpp:3272 — matched brush instance must belong to the matched entity.
            // tb/te are the binary's locals.
            selbrush_t *tb = mb;
            entity_s   *te = lnEnts[i];
            iassert( tb->owner && tb->owner == te );   // XYWnd.cpp:3272

            const float *baseCol = Ed_EntDefColor( lnEnts[i] );   // §11: DEF eclass colour
            float rgba[4] = { baseCol[0], baseCol[1], baseCol[2], 1.0f };
            Brush_GetEntityLineColor( rgba, mb->def, rgba );
            GfxColor col;
            Byte4PackPixelColor( rgba, &col );

            vertCount = Ed_AddLinkSeg( verts, ident, fromC, toC, &col, vertCount );
        }
    }
    if ( vertCount )
        R_AddCmd_Line3D( (short)( vertCount / 2 ), 1, verts );
}

// Ed_DrawConnectionLines - the DrawConnectionLinks (0x40c9f0) link-line tail, called from
// OnPaint after the vertex/edge handles (that function's prefix).  The binary's tail is
// exactly: Lines_AddLinkTo -> Lines_AddLinkToScript -> VehiclePath_AddNode -> Pointfile_Draw.
extern void VehiclePath_AddNode();   // vehiclepath.cpp (0x4B6710)
extern void Pointfile_Draw();        // points.cpp      (0x48AE20)
void Ed_DrawConnectionLines()
{
    Lines_AddLinkTo();
    Lines_AddLinkToScript();
    VehiclePath_AddNode();      // vehicle-path preview for the selected info_vehicle_node
    Pointfile_Draw();           // IDB DrawConnectionLinks tail — draw the .lin leak path
}



// ═════════════════════════════════════════════════════════════════════════════
//  Shell-agnostic XY-viewport handlers (U-VP-XY).  Every afx_msg body lives here as a free
//  function on plain args; the MFC CXYWnd handlers at the bottom of this file are thin
//  translations, and the raw-Win32 WndProc twin after them feeds the SAME functions with the
//  SAME conventions MFC used (client coords, the MK_* wParam flag word, LOWORD/HIWORD size).
//  The focus/capture calls and the state writes are part of the handler bodies, so both shells
//  get them identically.  `Ed_ActiveXY()` is the one XY view (see xywndState_t).
//
//  Callers declare these themselves (mainfrm.h is not this unit's to edit):
//      extern void XYWnd_OnCreate( HWND hwnd );
//      extern void XYWnd_OnSize( HWND hwnd, int cx, int cy );
//      extern void XYWnd_Paint( HWND hwnd );
//      extern void XYWnd_OnDestroy( HWND hwnd );
//      extern void XYWnd_OnLButtonDown( HWND hwnd, unsigned int nFlags, int x, int y );
//      extern void XYWnd_OnLButtonUp( unsigned int nFlags, int x, int y );
//      extern void XYWnd_OnMouseMove( unsigned int nFlags, int x, int y );
//      extern void XYWnd_OnRButtonDown( HWND hwnd, unsigned int nFlags, int x, int y );
//      extern void XYWnd_OnRButtonUp( HWND hwnd, unsigned int nFlags );
//      extern void XYWnd_OnMButtonDown( HWND hwnd, unsigned int nFlags, int x, int y );
//      extern void XYWnd_OnMButtonUp();
//      extern int  XYWnd_OnMouseWheel( HWND hwnd, short zDelta, int screenX, int screenY );
//      extern void XYWnd_OnKeyDown( unsigned int nChar, unsigned int nRepCnt, unsigned int nFlags );
// ═════════════════════════════════════════════════════════════════════════════

// CXYWnd::OnLButtonDown 0x463f70 -> OriginalButtonDown 0x464a20 -> XY_MouseDown.  The binary's
// active-XY-pane swap is moot here (single-pane) and m_ptDown is not modelled; SetFocus /
// SetCapture (inside OriginalButtonDown in the binary) + Ed_InvalidateAllViews are the port's
// conventions.  (CWnd::SetFocus/SetCapture ARE ::SetFocus(m_hWnd)/::SetCapture(m_hWnd).)
void XYWnd_OnLButtonDown( HWND hwnd, unsigned int nFlags, int x, int y )
{
    xywndState_t *wnd = Ed_ActiveXY();
    (void)hwnd;   // P5 RTT: shell owns focus + drag-capture; OS-capturing the hidden child steals the mouse.
    // Clip mode steals the left-click to drop a clip point (unless the temporary Ctrl+RMB
    // "rogue" clip is active, which keeps normal editing on LMB).
    if ( g_bClipMode && !g_bRogueClipMode )
        Ed_DropClipPoint( wnd, x, y );
    else
        Ed_XY_MouseDown( wnd, x, y, nFlags );
    Ed_InvalidateAllViews();   // selection / clip points changed -> repaint XY + Z
}

extern void SelectFaceSth( int a1, int a2, int a3 );   // select.cpp 0x48e340 (via drag.cpp extern)

void XYWnd_OnLButtonUp( unsigned int nFlags, int x, int y )
{
    xywndState_t *wnd = Ed_ActiveXY();
    // Clip mode: release any clip-point drag (IDB OnLButtonUp clip branch); a click that
    // placed a point does not run Drag_MouseUp (no drag was begun).
    if ( g_bClipMode )
    {
        if ( g_pMovingClip )
        {
            // P5 RTT: shell owns drag-capture; OS ReleaseCapture on the hidden child would
            // steal the mouse from the ImGui host, so the release is dropped here.
            g_pMovingClip = nullptr;
        }
        wnd->m_nButtonstate = 0;
        g_nUpdateBits = -1;
        Ed_InvalidateAllViews();
        return;
    }
    // RESTORED (IDB OnLButtonUp 0x464860): alt+shift LMB CLICK (no drag — the up point equals
    // the press point) → single-face select via SelectFaceSth, with the filter bits from prefs
    // (entities_off=0x200 / selectableModels=0x400 / sky_brush_off=0x800). Cast the press point
    // ray from m_nPressx/m_nPressy (the port doesn't model the binary's m_ptDown; for an LMB the
    // down/press point coincides). Off the monkey/gate path (modifier state can't be PostMessage'd).
    if ( x == wnd->m_nPressx && y == wnd->m_nPressy
         && GetKeyState( VK_MENU ) < 0 && GetKeyState( VK_SHIFT ) < 0 )
    {
        int flags = 0;
        if ( g_PrefsDlg->entities_off )        flags = 512;
        if ( g_PrefsDlg->m_bSelectableModels ) flags |= 1024;
        if ( g_PrefsDlg->sky_brush_off )       flags |= 2048;
        float start[3], dir[3];
        Ed_XY_ToPoint( wnd, wnd->m_nPressx, wnd->m_nPressy, start, dir );
        SelectFaceSth( (int)dir, (int)start, flags );
    }
    Drag_MouseUp( nFlags );        // == XY_MouseUp's Drag_MouseUp (toggle_unk02/ShowCursor reset
    wnd->m_nButtonstate = 0;       //    omitted — consistent with the documented scroll reduction)
    // P5 RTT: shell owns drag-capture; OS ReleaseCapture on the hidden child would steal the
    // mouse from the ImGui host, so the release is dropped here.
    g_nUpdateBits = -1;
    Ed_InvalidateAllViews();   // repaint XY + Z
}

// CXYWnd::OnMouseMove - REDUCED vs IDA 0x464b10.  Kept: the g_tdp cursor-point update, the
// clip-point hover/drag (Ed_ClipMouseMoved) and Ed_XY_MouseMoved.  KISAK omissions on a
// no-RButton move: the CHASE-MOUSE edge auto-scroll (m_bChaseMouse + SetTimer + m_ptDragAdj +
// the OnTimer half, which needs the unmodelled m_ptDown), the STATUS-BAR coord readout, and
// the point-edit HOVER latch (byte_23245A5 + dword_23F1648).
void XYWnd_OnMouseMove( unsigned int nFlags, int x, int y )
{
    xywndState_t *wnd = Ed_ActiveXY();
    // 0x464c1a: while the RMB is NOT down (the binary's m_bRButtonDown; the port carries the
    // same state in m_nButtonstate) the cursor's snapped world point is republished into
    // g_tdp — the anchor DrawCrosshair (0x4655d0) draws through.  Zeroed first because
    // SnapToPoint/SnapToGrid only write the two in-plane axes.
    if ( !( wnd->m_nButtonstate & MK_RBUTTON ) )
    {
        g_tdp[0] = g_tdp[1] = g_tdp[2] = 0.0f;
        Ed_SnapToPoint_Chooser( wnd, g_tdp, x, y );
    }

    // Clip mode: hover-pick / drag a placed clip point (sets cursor + g_pMovingClip). It
    // consumes the move only while a clip point is actively being dragged.
    if ( g_bClipMode && Ed_ClipMouseMoved( wnd, x, y ) )
    {
        Ed_InvalidateAllViews();
        return;
    }
    Ed_XY_MouseMoved( wnd, x, y, nFlags );
}

// CXYWnd::OnRButtonDown 0x4647b0 - single-pane reconstruction.  The binary swaps the active XY
// pane then routes the non-clip RMB through OriginalButtonDown 0x464a20 -> XY_MouseDown(...,
// MK_RBUTTON).  KISAK: (a) the active-pane swap is MOOT (single-pane); (b) OriginalButtonDown's
// SetWindowPos/BringWindowToTop Z-order raise is deliberately omitted - it destabilises the
// live D3D render panes; (c) for the default 3-button mouse XY_MouseDown(MK_RBUTTON) only sets
// m_nButtonstate + GetCursorPos, both reproduced directly; on a 2-button mouse the binary maps
// the camera origin onto Ctrl+RMB where the port arms scroll instead.
void XYWnd_OnRButtonDown( HWND hwnd, unsigned int nFlags, int x, int y )
{
    xywndState_t *wnd = Ed_ActiveXY();
    (void)hwnd;   // P5 RTT: shell owns focus + drag-capture; OS-capturing the hidden child steals the mouse.
    // Ctrl+RMB on a 3-button mouse = the temporary "rogue" clip: enter clip mode if not
    // already in it, mark it rogue (so LMB keeps normal editing), and drop a clip point
    // (IDB OnRButtonDown 0x4647b0). Otherwise arm the scroll/context-menu RMB path.
    if ( g_PrefsDlg->m_nMouseButtons == 3 && GetKeyState( VK_CONTROL ) < 0 )
    {
        if ( !g_bClipMode )
        {
            Ed_SetClipMode( 1 );
            g_bRogueClipMode = 1;
        }
        Ed_DropClipPoint( wnd, x, y );
        Ed_InvalidateAllViews();
        return;
    }
    // Faithful RMB routing (0x4647b0 → OriginalButtonDown → XY_MouseDown): the binary sends
    // EVERY RMB-down through XY_MouseDown — that is what arms Drag_Begin's Shift+RMB(+Alt)
    // marquee modes (Drag_Setup 0x47e566: buttons==6 [MK_SHIFT|MK_RBUTTON] + Alt →
    // sel_areabrush_sub(13) box-DESELECT).  The port's plain-RMB scroll/context reduction
    // stays for an UNMODIFIED RMB; a MODIFIED one routes through the real dispatch so those
    // drags arm and OnRButtonUp's Drag_MouseUp can apply them.  [audit U3, 2026-07-25]
    if ( GetKeyState( VK_SHIFT ) < 0 || GetKeyState( VK_MENU ) < 0 )
    {
        wnd->m_nPressx = x;
        wnd->m_nPressy = y;
        s_rmbScrolled = true;            // modified RMB never pops the context menu (0x4649e4 gate)
        Ed_XY_MouseDown( wnd, x, y, nFlags );
        return;
    }
    wnd->m_nButtonstate = MK_RBUTTON;
    s_rmbScrolled  = false;          // arm the context-click detector
    GetCursorPos( &wnd->m_ptCursor );
    // The binary records the client-space press point (m_ptDown) here; the RMB-create
    // dispatch (OnEntityCreate_Context 0x466730) passes it to CreateEntityFromClassname
    // so the placeholder brush drops under the cursor. m_nPressx/y ARE that client point.
    wnd->m_nPressx = x;
    wnd->m_nPressy = y;
}

// CXYWnd::ContextMenu (IDB 0x467100) - the XY right-click popup: Select / Prefab / Layers
// submenus, the Make-Detail/Structural/Clip/Coplanar + Ungroup commands, and the recursive
// eclass CREATE-entity tree.  The static items route to their existing WM_COMMAND handlers;
// TrackPopupMenu(TPM_RETURNCMD) dispatches directly.

void XYWnd_OnRButtonUp( HWND hwnd, unsigned int nFlags )
{
    xywndState_t *wnd = Ed_ActiveXY();
    // The faithful 0x464990 tail = XY_MouseUp 0x467e30: Drag_MouseUp closes/applies any armed
    // drag, toggle_unk02 resets, full repaint bits requested.  KISAK: the binary's
    // ShowCursor(1)-until-visible loop is omitted - the port's reduced RMB path never
    // ShowCursor(0)s.
    Drag_MouseUp( nFlags );                       // 0x467e35
    g_qeglobals.toggle_unk02 = 0;                 // 0x467e48
    g_nUpdateBits = -1;                           // 0x467e4d
    wnd->m_nButtonstate = 0;                      // 0x467e57
    // P5 RTT: shell owns drag-capture; OS ReleaseCapture on the hidden child would steal the
    // mouse from the ImGui host, so the ( nFlags & MK_* )==0 release (0x464a08) is dropped here.
    Ed_InvalidateAllViews();                      // port's repaint mechanism for g_nUpdateBits

    // Context menu: only on a CLICK (no scroll — s_rmbScrolled stands in for the binary's
    // point==m_ptDown test) with NO Shift/Ctrl/Alt (the 0x4649cc–0x4649e4 modifier gate,
    // previously missing).  Popped AFTER the capture release — the binary pops first
    // (0x4649e6) but literal order fails here (TrackPopupMenu×capture port quirk).
    if ( !s_rmbScrolled )
    {
        bool noAlt = GetKeyState( VK_MENU ) >= 0;                       // 0x4649cc
        if ( GetKeyState( VK_CONTROL ) < 0 )                            // 0x4649d5
            noAlt = false;
        if ( GetKeyState( VK_SHIFT ) >= 0 && noAlt )                    // 0x4649e4
        {
            (void)hwnd;               // raw shell: no context menu until the HMENU rewrite lands
        }
    }
}

void XYWnd_OnMButtonDown( HWND hwnd, unsigned int nFlags, int x, int y )
{
    // IDB CXYWnd::OnMButtonDown 0x463ff0 -> OriginalButtonDown 0x464a20 -> XY_MouseDown with
    // nFlags carrying MK_MBUTTON (0x10), which reaches Ed_XY_MouseDown's camera-angle branch
    // (m_nMouseButtons==3 && m_nButtonstate==16) and turns the 3D camera toward the clicked
    // point.  (Ed_XY_MouseDown sets m_nButtonstate itself.)
    (void)hwnd;   // P5 RTT: shell owns focus + drag-capture; OS-capturing the hidden child steals the mouse.
    Ed_XY_MouseDown( Ed_ActiveXY(), x, y, nFlags );
    Ed_InvalidateAllViews();
}

void XYWnd_OnMButtonUp()
{
    Ed_ActiveXY()->m_nButtonstate = 0;
    // P5 RTT: shell owns drag-capture; OS ReleaseCapture on the hidden child would steal the
    // mouse from the ImGui host, so the release is dropped here.
}

// KISAK PORT UX BINDING: the binary's CXYWnd has NO OnMouseWheel override (zoom is
// WM_COMMAND/key-driven via CMainFrame::OnKeyDown 0x422370), so wheel-to-zoom-around-cursor is
// a port convenience, not a reproduced handler.  The zoom-to-cursor math is the standard
// GtkRadiant form; the scale step (1.25/0.8) + clamp [0.01,32] should be cross-checked against
// the binary's zoom WM_COMMAND handler (not located).
// screenX/screenY = the WM_MOUSEWHEEL SCREEN point (MFC hands OnMouseWheel the same); returns
// TRUE(1) = handled, as the MFC override did.
int XYWnd_OnMouseWheel( HWND hwnd, short zDelta, int screenX, int screenY )
{
    xywndState_t *wnd = Ed_ActiveXY();
    // Zoom around the cursor: keep the world point under the cursor fixed. Under RTT the
    // shell passes IMAGE-RELATIVE client coords already (no ScreenToClient on the hidden
    // child — that was zeroing/garbaging the zoom anchor and breaking wheel zoom).
    (void)hwnd;
    POINT cpt; cpt.x = screenX; cpt.y = screenY;

    float before[3] = { 0, 0, 0 };
    Ed_SnapToPoint( wnd, before, cpt.x, cpt.y );

    wnd->m_fScale *= ( zDelta > 0 ) ? 1.25f : 0.8f;
    if ( wnd->m_fScale < 0.01f ) wnd->m_fScale = 0.01f;
    if ( wnd->m_fScale > 32.0f ) wnd->m_fScale = 32.0f;

    float after[3] = { 0, 0, 0 };
    Ed_SnapToPoint( wnd, after, cpt.x, cpt.y );

    int nDim1 = ( wnd->m_nViewType == ED_VIEW_YZ ) ? 1 : 0;
    int nDim2 = ( wnd->m_nViewType != ED_VIEW_XY ) + 1;
    wnd->m_vOrigin[nDim1] += before[nDim1] - after[nDim1];
    wnd->m_vOrigin[nDim2] += before[nDim2] - after[nDim2];

    // A wheel ZOOM changes this view, so it must say so for the dirty gate; the camera bit is
    // kept so the pump behaves as it did.
    g_nUpdateBits |= ( W_XY | W_XY_OVERLAY | W_CAMERA );
    return TRUE;
}

void XYWnd_OnKeyDown( unsigned int nChar, unsigned int nRepCnt, unsigned int nFlags )
{
    // CXYWnd::OnKeyDown 0x465c90 is exactly CMainFrame::OnKeyDown(g_pParentWnd, ...) - a thin
    // forward to the editor's command-map dispatch (0x422370: key+modifiers -> command map ->
    // WM_COMMAND).  Ctrl+Z/Y/S still resolve first in PreTranslateMessage / the accelerators.
    (void)nChar; (void)nRepCnt; (void)nFlags;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x487B10 Entity_NeedsExportKey - does this entity get an "export" id key?  True only for
// "actor"/"misc_turret" classnames, and only when the eclass is fixedsize.
// ─────────────────────────────────────────────────────────────────────────────
static bool Entity_NeedsExportKey( entity_s_def *e )
{
    if ( !e || !e->eclass || !*(int *)&e->eclass->fixedsize )
        return false;
    const char *cn = ValueForKey2( (int)(intptr_t)e, "classname" );
    if ( !cn ) return false;
    if ( strstr( cn, "actor" ) )       return true;
    if ( strstr( cn, "misc_turret" ) ) return true;
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x465cc0 CreateEntityFromName - create an entity of class `str`.
// PATH A (Entity_Create succeeded): EntityAssignModel -> bboxInst = v4->brushes.ownerNext ->
//   asserts 1144/1145 (binary LEVEL 1) -> Select_Deselect/Select_Brush -> the "export" key
//   (Entity_NeedsExportKey; Map_GetNextExportId returns the VA string directly, so the
//   binary's CString temp-buffer scaffolding collapses to one call) -> classtype&0x40 ->
//   radius 256 + height 128 / (classtype&0x80)==0 -> Trigger_SetCurrentDefaultMaterial
//   ("trigger"/"aitrig"/"volume") / else radius 256 -> Undo_SetIdForEntity.  The
//   misc_model/prefab/script/dyn arm is the binary's CEntityWnd_SetInspectorMode(128) +
//   PostMessageA(IDC_E_ADD_MODEL), reconstructed as Ed_PostAddModelCommand.
// PATH B (Entity_Create returned NULL): re-class the single selected fixedsize entity.
// Workflow: drag out a placeholder brush where you want the entity, then "create X" -
// Entity_Create consumes the placeholder and drops the entity there.

// CreateEntityBrush (IDB 0x466290) - drop a 32x32 grid-snapped placeholder brush at the click
// point, so a fresh point/model entity picked from the RMB menu with an empty selection has a
// box to bind to.  __usercall(int height@<eax>, int x@<ecx>, xywndState_t *wnd).  The DEPTH axis
// comes from d_new_brush_bottom_x/top_x (grid-quantised, min one grid cell).  NOTE the
// degenerate in-plane axis fixup ADDS 16.0, not +step.
static selbrush_t *CreateEntityBrush( int height, int x, xywndState_t *wnd )
{
    float mins[3], maxs[3];
    const bool noclamp = ( g_PrefsDlg->m_bNoClamp != 0 );          // 0x4662aa

    if ( noclamp ) Ed_SnapToPoint( wnd, mins, x, height );        // 0x4662b7
    else           Ed_SnapToGrid ( wnd, mins, x, height );        // 0x4662c8

    const int x2 = x + 32;                                        // 0x4662d0
    const int h2 = height + 32;                                   // 0x4662d3
    if ( noclamp ) Ed_SnapToPoint( wnd, maxs, x2, h2 );           // 0x4662e2
    else           Ed_SnapToGrid ( wnd, maxs, x2, h2 );           // 0x4662f3

    // Depth (out-of-screen) axis: XY→Z(2), XZ→Y(1)/YZ→X(0) (m_nViewType!=0).
    const int depth = ( wnd->m_nViewType == 2 ) ? 2 : ( wnd->m_nViewType != 0 );  // 0x466304

    const float step   = grid_sizes[g_qeglobals.d_gridsize];
    const float *botSrc = &g_qeglobals.d_new_brush_bottom_x;      // [x,y,z]
    const float *topSrc = &g_qeglobals.d_new_brush_top_x;         // [x,y,z]

    const float lo = (float)(int)( botSrc[depth] / step ) * step; // 0x466339
    mins[depth] = lo;
    float hi = (float)(int)( topSrc[depth] / step ) * step;       // 0x466367
    maxs[depth] = hi;
    if ( hi <= lo )                                               // 0x466376
        maxs[depth] = lo + step;                                  // 0x46637f

    for ( int i = 0; i < 3; ++i )                                 // 0x46638b
    {
        if ( maxs[i] == mins[i] )                                 // 0x46639c
            maxs[i] += 16.0f;                                     // 0x4663a4  (note: +16, not +step)
        if ( maxs[i] < mins[i] )                                  // 0x4663b7
        {
            const float t = mins[i]; mins[i] = maxs[i]; maxs[i] = t;
        }
    }

    Ed_EnsureCurrentMaterial();   // give the new brush a valid material (as Ed_NewBrushDrag)
    brush_t *b = Brush_Alloc( g_qeglobals.random_texture_stuff, nullptr );  // 0x4663e5
    Brush_Create( mins, maxs, b, nullptr );                       // 0x4663f0 (lower, upper)
    if ( !b )                                                     // 0x4663fa
        return nullptr;
    Brush_BuildWindings( b, 1 );                                  // 0x466409
    if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )  // 0x46641e
        SetupVertexSelection();
    MarkMapModified();                                            // 0x466425
    ++b->version;                                                 // 0x466430
    Entity_LinkBrush( b, (entity_s *)world_entity->def );         // 0x46643a  (def-side)
    selbrush_t *inst = Brush_AddToList( b, world_entity );        // 0x466448
    if ( inst->next || inst->prev )                               // 0x466453
        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
    Brush_AddToList2( inst );                                     // 0x46645d
    return inst;
}

// ─── CreateEntityFromClassname (IDB 0x466480) ─────────────────────────────────
// The XY-menu dynamic-id dispatch target (called from OnEntityCreate_Context 0x466730).
//   __usercall CreateEntityFromClassname(xywndState_t *xywnd@<edi>, char *classname@<esi>, int x, int y)
// Runtime refusal checks (MessageBeep + Sys_Printf, NOT a grey-out) for fixedsize-with-
// brushes and prefab/model conflicts; then Undo bracket; then if NOTHING is selected drop a
// placeholder brush at (x, clientHeight-y-1); then CreateEntityFromName(classname).
static void CreateEntityFromClassname( xywndState_t *xywnd, const char *classname, int x, int y )
{
    RECT xyrect;
    GetClientRect( xywnd->m_hWnd, &xyrect );                      // 0x46648e

    eclass_t *ec = Eclass_ForName( 0, classname );               // 0x4664a0
    const bool haveSel = ( selected_brushes.next != &selected_brushes );

    // The selected brush's owning entity eclass name (b->owner->def->eclass->name).
    // def is entity_s*; the eclass lives on the entity_s_def view (as elsewhere in this file).
    const char *selName = haveSel
        ? ( (entity_s_def *)selected_brushes.next->owner->def )->eclass->name
        : nullptr;

    if ( ec && *(int *)&ec->fixedsize )                          // fixedsize entity
    {
        // 0x4664c8: refuse ONLY when WORLD brushes are selected — the binary tests
        // `!I_stricmp(selName,"worldspawn")` (true when the selection IS worldspawn).  A
        // NON-worldspawn entity selected falls through to the re-class path (CreateEntityFromName
        // Path B).  [A prior port dropped the `!`, inverting this — it beeped on entity selection
        // and allowed the refused world-brush case.]
        if ( haveSel && !I_stricmp( selName, "worldspawn" ) )    // 0x4664c8
        {
            MessageBeep( MB_ICONHAND );                          // 0x4664d6 (0x40)
            Sys_Printf( "Can not create a fixed size entity with brushes selected\n" );
            return;                                              // 0x4664ec
        }
    }
    if ( haveSel )                                                // 0x4664f7
    {
        if ( ( !I_stricmp( selName, "misc_model" ) || !I_stricmp( selName, "script_model" ) )
             && !I_stricmp( classname, "misc_prefab" ) )         // 0x466545
        {
            MessageBeep( MB_ICONHAND );                          // 0x466553
            Sys_Printf( "Can not create a prefab with a misc_model or script_model selected\n" );
            return;                                              // 0x466569
        }
        if ( !I_stricmp( selName, "misc_prefab" )
             && ( !I_stricmp( classname, "misc_model" ) || !I_stricmp( classname, "script_model" ) ) )  // 0x4665ac
        {
            MessageBeep( MB_ICONHAND );                          // 0x4665ba
            Sys_Printf( "Can not create a misc_model or script_model with a prefab selected\n" );
            return;                                              // 0x4665d0
        }
    }

    Undo_ClearRedo();                                            // 0x4665d1
    Undo_GeneralStart( "create entity" );                        // 0x4665db
    if ( !haveSel )                                              // 0x4665ea  nothing selected → drop a box
        CreateEntityBrush( xyrect.bottom - xyrect.top - y - 1, x, xywnd );  // 0x4665fc (GL Y-flip)
    CreateEntityFromName( classname );                          // 0x466605
    Undo_End();                                                 // 0x46660d
}

void CreateEntityFromName( const char *str )
{
    if ( !I_stricmp( str, "worldspawn" ) )
    {
        MessageBoxA( GetActiveWindow(),
            "Failed to create entity.\n\nCan't create an entity with worldspawn.",
            "Radiant", 0x30u );
        return;
    }

    eclass_t *eclass = Eclass_ForName( 0, str );
    Undo_AddBrushList( &selected_brushes );          // !! hook spot (undo bracket open)

    entity_s *v4 = Entity_Create( eclass );
    if ( v4 )
    {
        // ── PATH A: a fresh entity was created (point or brush) ──
        EntityAssignModel( (entity_s_def *)v4->def );

        selbrush_t *bboxInst = v4->brushes.ownerNext;   // the entity's (bbox) brush instance
        {
            entity_s   *petNew = v4;                    // the binary's locals
            selbrush_t *b      = bboxInst;
            iassert( b != &petNew->brushes );   // XYWnd.cpp:1144
        }
        {
            selbrush_t *b = bboxInst;            // the binary's local
            iassert( b->owner->def == b->def->owner );   // XYWnd.cpp:1145
        }

        Select_Deselect( 1 );
        Select_Brush( bboxInst, 1, 1, 0 );

        if ( Entity_NeedsExportKey( (entity_s_def *)v4->def ) )
        {
            // actor/misc_turret: assign a unique "export" id.  IDA 0x466034 allocates an MFC
            // CString temp buffer for Map_GetNextExportId to fill; the port's version RETURNS
            // the VA string (slot-index convention), so that scaffolding collapses to one
            // call.  Slot 2 is free (0/1 belong to MainFrm_BrushList).
            SetKeyValue( (entity_s_def *)v4->def, "export", Map_GetNextExportId( 2 ) );
        }

        // Class-type default keys (faithful 0x465E.. tail). misc_*/script_* spawn the
        // model-selection window (PARK — P6 UI); other classes seed radius/height or a
        // trigger/volume layer key.
        if ( I_stricmp( str, "misc_model" )    && I_stricmp( str, "misc_prefab" )  &&
             I_stricmp( str, "script_model" )  && I_stricmp( str, "script_vehicle" ) &&
             I_stricmp( str, "dyn_model" ) )
        {
            int classtype = eclass->classtype;
            if ( classtype & 0x40 )
            {
                SetKeyValue( (entity_s_def *)v4->def, "radius", "256" );
                SetKeyValue( (entity_s_def *)v4->def, "height", "128" );
            }
            else if ( ( classtype & 0x80u ) == 0 )
            {
                // trigger/volume layer key (faithful 0x4661d0 dispatch): the 7 plain
                // trigger_* classes → default material "trigger"; trigger_friendlychain →
                // "aitrig"; info_volume → "volume".  Each calls Trigger_SetCurrentDefaultMaterial
                // (sub_45C1A0) → SetMaterial(name,&mtl); mtl.layer[GetCurrentLayer].size[0..1]=64;
                // Texture_SetTexture(0,&mtl) — make a 64x64 default the CURRENT editor texture for
                // the just-created trigger/volume brush.  All deps now ported (Texture_SetTexture
                // 0x45be50 → texwnd.cpp).
                if ( I_stricmp( str, "trigger_multiple" ) && I_stricmp( str, "trigger_damage" ) &&
                     I_stricmp( str, "trigger_once" )     && I_stricmp( str, "trigger_hurt" )   &&
                     I_stricmp( str, "trigger_lookat" )   && I_stricmp( str, "trigger_use" )    &&
                     I_stricmp( str, "trigger_use_touch" ) )
                {
                    if ( I_stricmp( str, "trigger_friendlychain" ) )
                    {
                        if ( !I_stricmp( str, "info_volume" ) )
                            Trigger_SetCurrentDefaultMaterial( "volume" );
                    }
                    else
                        Trigger_SetCurrentDefaultMaterial( "aitrig" );
                }
                else
                    Trigger_SetCurrentDefaultMaterial( "trigger" );
            }
            else
            {
                SetKeyValue( (entity_s_def *)v4->def, "radius", "256" );
            }
        }
        else
        {
            // misc_model / misc_prefab / script_model / script_vehicle / dyn_model: the
            // binary sets the inspector to W_ENTITY (CEntityWnd_SetInspectorMode(128))
            // then POSTS WM_COMMAND IDC_E_ADD_MODEL to the entity window, whose
            // proc pops the model/prefab file picker (→ SetKeyValue("model", name)).
            // The entity (with its bbox) is ALREADY created by Entity_Create above; the
            // picker only adds the "model" epair.  We post the same command so the
            // picker runs in the GUI; headless (no entity window) it is a clean no-op —
            // the model-less misc_model bbox stands, and the model_gate exercises the
            // picker's commit core (Ed_CommitPickedModel) directly.  (was: FATAL stub)
            // The mode switch was parked on CEntityWnd_SetInspectorMode; that shipped with
            // the inspector-mode unit, so the W_ENTITY switch is RESTORED (2026-07-31) —
            // without it the picker can pop behind a Textures/Console/Filters inspector.
            CEntityWnd_SetInspectorMode( INSPECTOR_ENTITY );   // 128
            Ed_PostAddModelCommand();
        }

        Undo_SetIdForEntity( (entity_s_def *)v4->def );
    }
    else
    {
        // PATH B: Entity_Create refused (NULL) - re-class the single selected fixedsize point
        // entity to the new fixedsize class `eclass` (IDA 0x465d54-0x465f8b).  Rebuilds the
        // bbox brush in place from the new eclass's mins/maxs, repoints owner->eclass, sets
        // "classname", relinks/reselects, then Entity_SetDefaultModelKey (0x485510).
        // The six bad-selection sub-cases (empty / >1 selected / consistency-assert / owner==
        // world / owner-class not fixedsize / new class not fixedsize) all fall through to the
        // "Failed to create entity." MessageBox (IDA loc_465F90).
        selbrush_t *v5 = selected_brushes.next;
        if ( v5 == &selected_brushes ||                          // empty selection
             v5->next != &selected_brushes )                      // more than one selected
        {
            MessageBoxA( GetActiveWindow(), "Failed to create entity.", "Radiant", 0x30u );
        }
        else
        {
            // XYWnd.cpp:1097 (level 0) — selection consistency invariant. b = the binary's local.
            selbrush_t *b = v5;
            iassert( b->owner->def == b->def->owner );   // XYWnd.cpp:1097

            entity_s *owner = v5->owner;
            entity_s_def *eDef = (entity_s_def *)owner->def;
            if ( owner == world_entity ||                                // can't re-class worldspawn
                 !*(int *)&eDef->eclass->fixedsize ||                    // current class must be fixedsize
                 !*(int *)&eclass->fixedsize )                           // new class must be fixedsize
            {
                MessageBoxA( GetActiveWindow(), "Failed to create entity.", "Radiant", 0x30u );
            }
            else
            {
                Undo_AddEntity_W( (entity_s *)eDef );

                // Recompute the bbox from the new eclass's mins/maxs, preserving the
                // old brush's origin.  IDA: delta = oldDef->mins - newEclass->mins;
                //   newMins = newEclass->mins + delta;  newMaxs = newEclass->maxs + delta.
                brush_t *oldDef = v5->def;
                float delta[3], newMins[3], newMaxs[3];
                for ( int i = 0; i < 3; ++i )
                {
                    delta[i]   = oldDef->mins[i] - eclass->mins[i];     // VectorSubtract
                    newMins[i] = eclass->mins[i] + delta[i];            // VectorAdd
                    newMaxs[i] = eclass->maxs[i] + delta[i];            // VectorAdd
                }

                // Repoint the entity to the new class + write the "classname" key.
                eDef->eclass = eclass;
                SetKeyValue( eDef, "classname", str );

                // Build the new bbox brush def from the new eclass material.
                // IDA: Init_MaterialLayer(&matdef, COERCE(0.25f)); sub_475AC0(&matdef,
                //   eclass, newMins, newMaxs) == Brush_Alloc(&matdef, eclass) +
                //   Brush_Create(newMins, newMaxs, brush, eclass).
                // matdef: the binary leaves mat_texDef uninitialized (Init_MaterialLayer
                // seeds it per-layer; Brush_Alloc memcpys all 0x24 bytes into each face).
                // memset for safety, matching the gate-proven Entity_Create bbox build.
                MaterialDef matdef;
                memset( &matdef, 0, sizeof( matdef ) );
                matdef.lyrMtl = *(LayerMaterialDef **)&eclass->material[0];  // [esi+0x34] -> var_5C
                matdef.radMtl = (qtexture_s *)eclass->textureTableOrSth;     // [esi+0x38] -> var_58
                float ss = 0.25f;                                            // flt_6F42F0 (COERCE_MATERIALDEF_)
                Init_MaterialLayer( &matdef, *(MaterialDef **)&ss );

                brush_t *nb = Brush_Alloc( &matdef, eclass );
                Brush_Create( newMins, newMaxs, nb, eclass );
                Entity_LinkBrush( nb, (entity_s *)v5->owner->def );          // def-side link into the entity

                if ( Entity_NeedsExportKey( (entity_s_def *)v5->owner->def ) )
                    SetKeyValue( (entity_s_def *)v5->owner->def, "export", Map_GetNextExportId( 2 ) );

                Brush_BuildWindings( nb, 1 );
                if ( g_qeglobals.d_select_mode == sel_vertex ||
                     g_qeglobals.d_select_mode == sel_edge )
                    SetupVertexSelection();
                MarkMapModified();
                ++nb->version;                                              // IDA 0x465edb add word [esi+4Eh],1

                // Drop the old instance, free the old def, instance the new def.
                entity_s *ownerInst = v5->owner;
                sub_476330( v5 );                                          // Brush_Deselect_Helper
                Brush_Free( v5 );
                selbrush_t *v11 = Brush_AddToList( nb, ownerInst );        // new instance for the new def

                {
                    selbrush_t *b = v11;         // the binary's local
                    iassert( b->owner->def == b->def->owner );   // XYWnd.cpp:1127
                }

                // Select_Brush_2(&active_brushes, v11): list != &selected_brushes, so the
                // else-branch head-splice into active_brushes (IDA 0x476630 else path).
                if ( v11->next || v11->prev )
                    Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
                v11->next = active_brushes.next;
                active_brushes.next->prev = v11;
                active_brushes.next = v11;
                v11->prev = &active_brushes;

                Select_Brush( v11, 1, 1, 0 );
                Entity_SetDefaultModelKey( nb, eclass );                   // 0x485510 (v9, v21)

                iassert( selected_brushes.next->def == nb && selected_brushes.prev->def == nb );   // XYWnd.cpp:1131
            }
        }
    }

    Undo_EndBrushList( &selected_brushes );          // !! hook spot (undo bracket close)
}


// CXYWnd::OnDestroy (0x463d00) — persist the window placement.  [audit U8/B9 — the
// whole handler was absent; SaveRegistryInfo has been a real port since win_qe3.cpp:336.]
// (The CWnd::OnDestroy() base chain is the SHELL's job — DefWindowProc in the raw twin.)
extern BOOL SaveRegistryInfo( const char *pszName, void *pvBuf, int lSize );   // win_qe3.cpp 0x499940
void XYWnd_OnDestroy( HWND hwnd )
{
    WINDOWPLACEMENT wndpl;
    wndpl.length = sizeof( wndpl );                          // 0x463d1a (44)
    if ( ::GetWindowPlacement( hwnd, &wndpl ) )              // 0x463d21
        SaveRegistryInfo( "Radiant::XYWindowPlace", &wndpl, 0x2C );   // 0x463d37
}

// Latch the window handle + client size (CXYWnd::OnCreate tail, after the base-class create).
void XYWnd_OnCreate( HWND hwnd )
{
    xywndState_t *wnd = Ed_ActiveXY();
    wnd->m_hWnd = hwnd;
    RECT rc; ::GetClientRect( hwnd, &rc );
    wnd->m_nWidth  = rc.right - rc.left;
    wnd->m_nHeight = rc.bottom - rc.top;
}

void XYWnd_OnSize( HWND hwnd, int cx, int cy )
{
    xywndState_t *wnd = Ed_ActiveXY();
    wnd->m_hWnd    = hwnd;
    wnd->m_nWidth  = cx;
    wnd->m_nHeight = cy;
    // Re-create this window's swap chain at the new size so Present is pixel-correct
    // (no stretch). R_Hwnd_Resize no-ops if the device isn't up yet or this hwnd isn't a
    // render window. Guard on dx.device so an OnSize during creation (pre-device) is inert.
    if ( dx.device && cx > 0 && cy > 0 )
        R_Hwnd_Resize( (HWND__ *)hwnd, cx, cy );
}

// ════════════════════════════════════════════════════════════════════════════
//  XY_Draw tail sub-draws, each wired into OnPaint at the binary's exact position
//  (see CXYWnd::XY_Draw 0x46ce20).
// ════════════════════════════════════════════════════════════════════════════

// Ed_PaintSizeInfo - PaintSizeInfo 0x46ba10, the selection size/measure overlay: two 3-segment
// measure brackets (one below the bottom edge, one right of the max edge) plus three text
// labels - the per-axis extents beside each bracket and the (h,v) world coordinate at the
// corner.
// NAMING TRAP: the binary names the corners backwards - vec3_clamp 0x4a8670 leaves the running
// MIN in `vMaxBounds`@a5 and the running MAX in `vMinBounds`@a1, so a1 = maxCorner, a5 =
// minCorner, size = a1 - a5.
// The CXYWnd "no-snap" flags at +92/+96 (tested with &1 for the half-texel label nudge) are
// not modelled by the port's class, so they default 0 = the binary's default-window case.
static void Ed_PaintSizeInfo( const float *maxCorner, xywndState_t *wnd,
                              int nDim2, int nDim1, const float *minCorner )
{
    const int   nDim3 = 3 - nDim1 - nDim2;       // v79 = 3 - a4 - a3
    const float  scale = wnd->m_fScale;          // *(a2+100)
    const float  inv   = 1.0f / scale;
    // CXYWnd "no x-snap"/"no y-snap" flags (a2+92 / a2+96): not modelled → 0 (apply nudge).
    const bool   noSnapV = false;                // (*(_BYTE*)(a2+92) & 1)
    const bool   noSnapH = false;                // (a2+96 & 1)
    const float  half    = 0.5f * inv;

    // Size vector = maxCorner - minCorner (v68).
    float size[3];
    size[0] = maxCorner[0] - minCorner[0];
    size[1] = maxCorner[1] - minCorner[1];
    size[2] = maxCorner[2] - minCorner[2];

    float *colSize = g_qeglobals.d_savedinfo.colors[20];   // size-info colour slot
    GfxColor packed;
    Byte4PackPixelColor( colSize, &packed );
    const float pc = *(float *)&packed.packed;             // packed colour, per-vertex

    // The measure brackets: SIX lines = TWELVE vertices (disasm 0x46ba83..0x46bc64 fills
    // v81 + v82[0/4] + v83[0/4/8/12] + v84[0/4] + v85[0/4/8] = 12 GfxPointVertex, then
    // R_AddCmd_Line3D(6=LINE count, ...)).  A horizontal bracket 10/scale below the bottom
    // edge and a vertical bracket 10/scale right of the max edge, each with 6/scale end ticks.
    GfxPointVertex line[12];
    int lv = 0;
    auto vert = [&]( float h, float v )
    {
        line[lv].xyz[nDim1] = h;
        line[lv].xyz[nDim2] = v;
        line[lv].xyz[nDim3] = 131072.0f;
        *(float *)line[lv].color = pc;
        ++lv;
    };
    const float hMin = minCorner[nDim1], hMax = maxCorner[nDim1];
    const float vMin = minCorner[nDim2], vMax = maxCorner[nDim2];
    // horizontal bracket: left tick, bottom rail, right tick
    vert( hMin, vMin -  6.0f * inv );  vert( hMin, vMin - 10.0f * inv );   // v81  / v82[0]
    vert( hMin, vMin - 10.0f * inv );  vert( hMax, vMin - 10.0f * inv );   // v82[4] / v83[0]
    vert( hMax, vMin - 10.0f * inv );  vert( hMax, vMin -  6.0f * inv );   // v83[4] / v83[8]
    // vertical bracket: bottom tick, right rail, top tick
    vert( hMax +  6.0f * inv, vMin );  vert( hMax + 10.0f * inv, vMin );   // v83[12] / v84[0]
    vert( hMax + 10.0f * inv, vMin );  vert( hMax + 10.0f * inv, vMax );   // v84[4] / v85[0]
    vert( hMax + 10.0f * inv, vMax );  vert( hMax +  6.0f * inv, vMax );   // v85[4] / v85[8]
    R_AddCmd_Line3D( 6, 1, line );

    Font_s *font = (Font_s *)g_qeglobals.d_font_list;
    if ( !font )
        return;

    // text step vectors (v70 = +1/scale along nDim1, v71 = -1/scale along nDim2; dbl_6F42A0=-1.0).
    float xstep[3] = { 0.0f, 0.0f, 0.0f };  xstep[nDim1] =  inv;
    float ystep[3] = { 0.0f, 0.0f, 0.0f };  ystep[nDim2] = -inv;

    // off_739EC8[axis] = per-axis size label: {"x:%.f","y:%.f","z:%.f"}.
    static const char *kAxisFmt[3] = { "x:%.f", "y:%.f", "z:%.f" };
    char  buf[64];
    float org[3];

    // Label 1 — the nDim1 (horizontal) extent, centred on the bottom edge, 24/scale below.
    {
        sprintf( buf, kAxisFmt[nDim1], size[nDim1] );
        const float midH = ( minCorner[nDim1] + maxCorner[nDim1] ) * 0.5f;
        org[nDim1] = midH + ( noSnapV ? 0.0f : half );
        org[nDim2] = ( minCorner[nDim2] - 24.0f * inv ) + ( noSnapH ? 0.0f : half );
        org[nDim3] = 131072.0f;
        R_AddCmdDrawTextAtPosition( buf, font, org, xstep, ystep, colSize );
    }
    // Label 2 — the nDim2 (vertical) extent, 16/scale right of the max edge, centred.
    {
        sprintf( buf, kAxisFmt[nDim2], size[nDim2] );
        const float midV = ( minCorner[nDim2] + maxCorner[nDim2] ) * 0.5f;
        org[nDim1] = ( maxCorner[nDim1] + 16.0f * inv ) + ( noSnapV ? 0.0f : half );
        org[nDim2] = midV + ( noSnapH ? 0.0f : half );
        org[nDim3] = 131072.0f;
        R_AddCmdDrawTextAtPosition( buf, font, org, xstep, ystep, colSize );
    }
    // Label 3 — the corner coordinate, format off_739ED4 = "(x:%.f  y:%.f)" (single, all views).
    // Args (disasm 0x46bf4d/0x46bf54): min[nDim1], max[nDim2].  Offset +4/+8 px.
    {
        sprintf( buf, "(x:%.f  y:%.f)", minCorner[nDim1], maxCorner[nDim2] );
        org[nDim1] = ( minCorner[nDim1] + 4.0f * inv ) + ( noSnapV ? 0.0f : half );
        org[nDim2] = ( maxCorner[nDim2] + 8.0f * inv ) + ( noSnapH ? 0.0f : half );
        org[nDim3] = 131072.0f;
        R_AddCmdDrawTextAtPosition( buf, font, org, xstep, ystep, colSize );
    }
}

// Ed_DrawRotateIconCenterSquare - CXYWnd::DrawRotateIcon_CenterSquare 0x469690, the
// rotate-mode pivot: a 4x4-world-unit square (two tris) in purple (0.8,0.1,0.9,0.25) at
// g_vRotateOrigin in the view plane, then a single pink (1,0.2,1,1) point at the exact pivot.
// Depth axis pushed to 131072 like the binary.  Only called when g_bRotateMode is set.
extern bool  g_bRotateMode;        // drag.cpp 0x23F16D9
extern float g_vRotateOrigin[3];   // drag.cpp 0x23F1658
static void Ed_DrawRotateIconCenterSquare( xywndState_t *wnd )
{
    const int vt = wnd->m_nViewType;
    // center (v14,v15,v16); v12/v13 = the (X,Y) corner offsets; v6/v7 = the (Y,Z) offsets.
    // Transcribed literally from 0x469690 per view.
    float cx, cy, cz;     // v14,v15,v16
    float o12, o13;       // v12 (X), v13 (Y)
    float o6,  o7;        // v6  (Y), v7  (Z)
    if ( vt == 2 )              // XY
    {
        cx = g_vRotateOrigin[0]; cy = g_vRotateOrigin[1]; cz = 131072.0f;
        o12 = 4.0f; o13 = 0.0f; o7 = 0.0f; o6 = 4.0f;
    }
    else if ( vt )             // XZ
    {
        cx = g_vRotateOrigin[0]; cy = 131072.0f; cz = g_vRotateOrigin[2];
        o12 = 4.0f; o13 = 0.0f; o6 = 0.0f; o7 = 4.0f;
    }
    else                       // YZ
    {
        cx = 131072.0f; cy = g_vRotateOrigin[1]; cz = g_vRotateOrigin[2];
        o12 = 0.0f; o13 = 4.0f; o7 = 4.0f; o6 = 0.0f;
    }

    float fillC4[4] = { 0.80000001f, 0.1f, 0.89999998f, 0.25f };
    GfxColor fillPacked;
    Byte4PackPixelColor( fillC4, &fillPacked );
    const float fc = *(float *)&fillPacked.packed;
    float color[4] = { fc, fc, fc, fc };

    float xyzw[4][4];
    float normal[4][3] = { {0,0,1}, {0,0,1}, {0,0,1}, {0,0,1} };
    float st[4][2]     = { {0,0}, {0,0}, {0,0}, {0,0} };
    // base (v9,v10,v11) = (cx-o12, cy-o13, cz-0).  Per-corner ± (v6,v7) on (Y,Z).
    const float v9  = cx - o12;
    const float v10 = cy - o13;
    const float v11 = cz - 0.0f;
    const float xc1 = cx + o12;     // v17/v2
    const float v8  = cy + o13;
    // corner 0 = ( v9 , v10-o6 , v11-o7 )
    xyzw[0][0] = v9 - 0.0f;          xyzw[0][1] = v10 - o6;       xyzw[0][2] = v11 - o7;       xyzw[0][3] = 1.0f;
    // corner 1 = ( xc1 , v8-o6 , (cz+0)-o7 )
    xyzw[1][0] = xc1 - 0.0f;         xyzw[1][1] = v8 - o6;        xyzw[1][2] = ( cz + 0.0f ) - o7; xyzw[1][3] = 1.0f;
    // corner 2 = ( xc1+0 , o6+v8 , o7+(cz+0) )
    xyzw[2][0] = xc1 + 0.0f;         xyzw[2][1] = o6 + v8;        xyzw[2][2] = o7 + ( cz + 0.0f ); xyzw[2][3] = 1.0f;
    // corner 3 = ( v9+0 , o6+v10 , o7+v11 )
    xyzw[3][0] = v9 + 0.0f;          xyzw[3][1] = o6 + v10;       xyzw[3][2] = o7 + v11;       xyzw[3][3] = 1.0f;

    static const uint16_t indices[6] = { 1, 0, 2, 2, 0, 3 };
    R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT, 6, indices, 4,
                            xyzw, normal, color, st );

    // The pink centre point.
    GfxPointVertex pt;
    pt.xyz[0] = cx; pt.xyz[1] = cy; pt.xyz[2] = cz;
    float pinkC4[4] = { 1.0f, 0.2f, 1.0f, 1.0f };
    Byte4PackPixelColor( pinkC4, (GfxColor *)pt.color );
    R_AddPointCmd_W( 1, 1, &pt );
}

// Ed_DrawTurretExportBox - sub_46AB80 0x46ab80: a 32x32-world-unit diamond (two tris) centred
// at the entity origin (XY plane, z + 0.01 - 16) in colour `col`.
static void Ed_DrawTurretExportBox( const float *p, const float *col )
{
    GfxColor packed;
    Byte4PackPixelColor( const_cast<float *>( col ), &packed );
    const float pc = *(float *)&packed.packed;

    const float cx = p[0];
    const float cy = p[1];
    // z = origin.z + 0.01 - 16 (disasm: var_24 = (z+0.01) - 16.0, shared by all 4 verts).
    const float cz = ( p[2] + 0.009999999776482582f ) - 16.0f;

    // 4 verts of the diamond (all at the same z), order is
    //  v0 (cx-16, cy, cz)  v1 (cx, cy+16, cz)  v2 (cx+16, cy, cz)  v3 (cx, cy-16, cz)
    float xyzw[4][4];
    xyzw[0][0] = cx - 16.0f; xyzw[0][1] = cy;         xyzw[0][2] = cz; xyzw[0][3] = 1.0f;
    xyzw[1][0] = cx;         xyzw[1][1] = cy + 16.0f; xyzw[1][2] = cz; xyzw[1][3] = 1.0f;
    xyzw[2][0] = cx + 16.0f; xyzw[2][1] = cy;         xyzw[2][2] = cz; xyzw[2][3] = 1.0f;
    xyzw[3][0] = cx;         xyzw[3][1] = cy - 16.0f; xyzw[3][2] = cz; xyzw[3][3] = 1.0f;

    float normal[4][3] = { {0,0,1}, {0,0,1}, {0,0,1}, {0,0,1} };
    float st[4][2]     = { {0,0}, {0,0}, {0,0}, {0,0} };
    float color[4]     = { pc, pc, pc, pc };
    static const uint16_t indices[6] = { 3, 0, 2, 2, 0, 1 };
    R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT, 6, indices, 4,
                            xyzw, normal, color, st );
}

// Ed_HighlightTurretShareAmbush - sub_46AD20 0x46ad20: walk a brush sentinel list and, for
// each brush whose entity def has an `export` key, look the id up in the two highlight bitmaps
// and draw a coloured box - both set -> flt_6DE1B0 magenta, share only -> flt_6DE130 red,
// ambush only -> flt_6DE160 blue.
static void Ed_HighlightTurretShareAmbush( selbrush_t *listHead, selbrush_t *listEnd,
                                           const byte *shareFlags,
                                           const byte *ambushFlags )
{
    static const float kShare[4] = { 1.0f, 0.0f, 0.0f, 1.0f };   // flt_6DE130 (red)
    static const float kAmbush[4]= { 0.0f, 0.0f, 1.0f, 1.0f };   // flt_6DE160 (blue)
    static const float kBoth[4]  = { 1.0f, 0.0f, 1.0f, 1.0f };   // flt_6DE1B0 (magenta)

    for ( selbrush_t *b = listHead; b != listEnd; b = b->next )
    {
        entity_s     *owner = b->owner;
        entity_s_def *def   = owner ? (entity_s_def *)owner->def : nullptr;
        if ( !def )
            continue;
        const char *val = Ed_EntFirstKey( owner, "export" );   // HasKeyValuePair + first "export" value
        if ( !val || !*val )
            continue;
        const int id = atol( val );
        if ( id <= 0 || id >= 2048 )           // bitmaps are char[2048]
            continue;
        const bool share  = shareFlags[id]  != 0;
        const bool ambush = ambushFlags[id] != 0;
        if ( !share && !ambush )
            continue;
        // origin = def->origin (entity_s.origin @ +104) -- the binary passes def+104.
        const float *c = ( share && ambush ) ? kBoth : ( share ? kShare : kAmbush );
        Ed_DrawTurretExportBox( def->origin, c );
    }
}

// Drive the turret share/ambush export-highlight (the binary's selected-loop parse at
// 0x46d426 + the twin sub_46AD20 passes at 0x46d890/0x46d8b0).  The SELECTED turrets'
// `script_turret_share` / `script_turret_ambush` keys (space-separated export ids) seed
// the two highlight bitmaps; if any id was flagged the highlight pass then draws a box on
// every brush in BOTH lists whose `export` id is flagged.  v67=share bitmap, v66=ambush.
extern bool Entity_HasEpairMatch( entity_s *e, const char *key, const char *val ); // entity.cpp 0x483930
static void Ed_DrawTurretExportHighlights()
{
    static byte shareFlags[2048];   // v67 (the binary memsets 2048 at the top of XY_Draw)
    static byte ambushFlags[2048];  // v66
    memset( shareFlags, 0, sizeof( shareFlags ) );
    memset( ambushFlags, 0, sizeof( ambushFlags ) );

    bool any = false;
    // Parse the SELECTED misc_turret entities' share/ambush key lists.
    for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
    {
        entity_s     *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        entity_s_def *def = (entity_s_def *)owner->def;
        if ( !def || !Entity_HasEpairMatch( def, "classname", "misc_turret" ) )
            continue;

        // script_turret_share -> shareFlags (v67)
        const char *shareVal = Ed_EntFirstKey( owner, "script_turret_share" );
        if ( shareVal && *shareVal )
        {
            char tmp[1024];
            strncpy( tmp, shareVal, sizeof( tmp ) - 1 );
            tmp[sizeof( tmp ) - 1] = 0;
            for ( char *tok = strtok( tmp, " " ); tok; tok = strtok( nullptr, " " ) )
            {
                int id = atol( tok );
                iassert( id > 0 );             // XYWnd.cpp:3934 "exprt > 0"
                if ( id > 0 && id < 2048 ) { shareFlags[id] = 1; any = true; }
            }
        }
        // script_turret_ambush -> ambushFlags (v66)
        const char *ambushVal = Ed_EntFirstKey( owner, "script_turret_ambush" );
        if ( ambushVal && *ambushVal )
        {
            char tmp[1024];
            strncpy( tmp, ambushVal, sizeof( tmp ) - 1 );
            tmp[sizeof( tmp ) - 1] = 0;
            for ( char *tok = strtok( tmp, " " ); tok; tok = strtok( nullptr, " " ) )
            {
                int id = atol( tok );
                iassert( id > 0 );             // XYWnd.cpp:3951 "exprt > 0"
                if ( id > 0 && id < 2048 ) { ambushFlags[id] = 1; any = true; }
            }
        }
    }

    if ( !any )                                 // binary's `if (i)` gate
        return;
    Ed_HighlightTurretShareAmbush( active_brushes.next,   &active_brushes,   shareFlags, ambushFlags );
    Ed_HighlightTurretShareAmbush( selected_brushes.next, &selected_brushes, shareFlags, ambushFlags );
}

// Drive the selection size/measure overlay (the binary's PaintSizeInfo call at 0x46d965,
// gated at 0x46d93b on `!bFixedSize && !g_bRotateMode && !g_bScaleMode && selected!=empty
// && m_bSizePaint`).  The running bounds + bFixedSize the binary accumulates in the
// selected loop are recomputed here over the selected list (the loop only feeds the size
// overlay — recomputing is identical to the in-loop accumulation).  vMaxBounds/vMinBounds
// follow the binary's swapped naming: vMaxBounds is the running MIN, vMinBounds the MAX.
static void Ed_DrawSizeInfo( xywndState_t *wnd, int nDim1, int nDim2 )
{
    if ( !selected_brushes.next || selected_brushes.next == &selected_brushes )
        return;                                      // selected list empty
    if ( g_bRotateMode || g_bScaleMode )
        return;
    if ( !g_PrefsDlg->m_bSizePaint )
        return;

    bool  bFixedSize = false;
    float vMaxBounds[3] = {  131072.0f,  131072.0f,  131072.0f };  // running MIN  (o1)
    float vMinBounds[3] = { -131072.0f, -131072.0f, -131072.0f };  // running MAX  (o2)

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s     *owner = b->owner;
        entity_s_def *eDef  = owner ? (entity_s_def *)owner->def : nullptr;
        eclass_t     *ec    = eDef ? eDef->eclass : nullptr;
        if ( ec && ec->fixedsize )
            bFixedSize = true;
        // vec3_clamp(def->mins, def->maxs, vMaxBounds(min), vMinBounds(max))
        brush_t *def = b->def;
        if ( def )
        {
            for ( int k = 0; k < 3; ++k )
            {
                if ( def->mins[k] < vMaxBounds[k] ) vMaxBounds[k] = def->mins[k];
                if ( def->maxs[k] > vMinBounds[k] ) vMinBounds[k] = def->maxs[k];
            }
        }
    }
    if ( bFixedSize )                                // binary: !bFixedSize gate
        return;
    // PaintSizeInfo(ecx=vMinBounds(=max), ebx=wnd, edi=nDim2, esi=nDim1, push vMaxBounds(=min)).
    Ed_PaintSizeInfo( vMinBounds, wnd, nDim2, nDim1, vMaxBounds );
}

// CopySelectedFaceValues (IDB 0x47d130) - XY_Draw's tail call: for each selected FACE re-run
// Brush_CheckBuildFaceVis on its owning brush so the windings used by the surface inspector /
// face-drag are current.
extern void       sub_477D70( selbrush_t *b, const float *orient );  // brush.cpp Brush_CheckBuildFaceVis
// (world_orient_matrix is declared at the top of this file.)
void CopySelectedFaceValues()
{
    const int count = g_SelectedFaces.GetSize();
    for ( int i = 0; i < count; ++i )
    {
        if ( i < 0 || i >= g_SelectedFaces.GetSize() )
        {
            Com_Error( ERR_FATAL,
                       "RADIANT: selFace array bounds check failed (CopySelectedFaceValues)" );
        }
        sub_477D70( g_SelectedFaces.GetAt( i ).brush, (const float *)world_orient_matrix );
    }
}

// CXYWnd::OnPaint - the XY_Draw pipeline (IDB 0x465b80):
//   CheckDevice -> BeginFrame -> ClearScreen -> SetupScene -> XY_DrawGrid -> XY_DrawBrushes ->
//   the overlay tail -> EndFrame -> IssueRenderCommands -> SortMaterials -> CheckTargetWindow.
// (The DC — CPaintDC / BeginPaint — belongs to the shell, not to this pipeline.)
void XYWnd_Paint( HWND hwndIn )
{
    xywndState_t *wnd = Ed_ActiveXY();

    // A WM_PAINT can arrive before the device is created (queued during CXYWnd::Create)
    // or if init failed; dx.device == NULL then. Skip the frame.
    if ( !dx.device )
        return;

    HWND__ *hwnd = (HWND__ *)hwndIn;
    if ( !R_SetupRendertarget_CheckDevice( hwnd ) )
        return;            // device not ready / not our window — skip this frame

    R_BeginFrame();
    // Point frontEndDataOut->cmds at the shared (non-view) command region. R_BeginFrame
    // leaves cmds == NULL; without this the backend's RB_CallExecuteRenderCommands sees
    // cmds==NULL and skips the command loop (frame presents an unrendered buffer).
    R_BeginSharedCmdList();
    // Clear to the XY background colour (savedinfo colour slot 1); 7 = colour+depth+stencil.
    R_AddCmdClearScreen( 7, g_qeglobals.d_savedinfo.colors[1], 1.0f, 0 );
    // The $line UNLIT pixel shader needs CONST_SRC_CODE_MATERIAL_COLOR; the bare grid draw
    // skips the per-material constant setup a scene render does, so set it white (the grid
    // lines carry their colour per-vertex).
    static const float s_edWhite[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    R_AddCmdSetMaterialColor( s_edWhite );

    XYViewState vs;
    vs.viewType  = wnd->m_nViewType;
    vs.scale     = wnd->m_fScale;
    vs.origin[0] = wnd->m_vOrigin[0];
    vs.origin[1] = wnd->m_vOrigin[1];
    vs.origin[2] = wnd->m_vOrigin[2];
    vs.width     = wnd->m_nWidth;
    vs.height    = wnd->m_nHeight;
    vs.active    = wnd->m_bActive;

    // Degenerate-projection guard (defensive — the ortho projection is provably immune, see
    // XY_SetupScene): if the inverse-VP would be non-orthogonal, no begin-view was submitted,
    // so skip ALL geometry this frame and fall through to the frame teardown (the clear stands).
    const bool sceneOk = XY_SetupScene( &vs );
    if ( sceneOk ) {
    XY_DrawGrid( &vs );        // grid lines + coordinate/view-name labels
    if ( ( g_qeglobals.d_savedinfo.d_xyShowFlags & 0x10 ) == 0 )  // View→Show→Blocks (0x10 SET = hidden)
        XY_DrawBlockGrid( &vs );   // coarse 1024-unit block grid + block-index labels
    XY_DrawBrushes( &vs );     // brushes/entity bboxes + entity-name labels
    // turret share/ambush export-highlight (IDB XY_Draw 0x46d876: the twin sub_46AD20
    // passes, gated on the selected turrets' script_turret_share/ambush key lists).
    Ed_DrawTurretExportHighlights();
    DrawCameraIcon( &vs );     // the camera position+facing marker (IDB CXYWnd::DrawCameraIcon 0x469a40)
    DrawZIcon( &vs );          // the blue camera-position marker ("player indicator", IDB 0x469d50)
    if ( g_bRotateMode )       // rotate-mode pivot marker (IDB XY_Draw 0x46db4f, DrawRotateIcon_CenterSquare 0x469690)
        Ed_DrawRotateIconCenterSquare( wnd );

    // The XY brush loop draws every entity via DrawBrush, whose DrawModels dispatch (0x47b102
    // -> Editor_InstanceAndSkinModel, tech 29 + per-vert colour) queues each model's
    // ED_SURF_MODEL surfs - including misc_models INSIDE prefab contents.  The ortho scene is
    // the active projection, so the flush draws the mesh top-down/front/side.
    // MATERIAL_COLOR is reset to the binary's NEUTRAL {0,0,0,0} before the flush
    // (R_SetMaterialColor(NULL), XY_Draw 0x46d8fa): tech 29 wireframe_shaded LERPS the texture
    // toward materialColor.rgb by materialColor.w, so w=1 would flat-override the mesh white.
    // Line batches keep their own per-brush colours (pushed just before their own DrawLines).
    {
        static const float s_flushNeutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        extern void R_AddCmdSetMaterialColor( const float *rgba );
        R_AddCmdSetMaterialColor( s_flushNeutral );
    }
    R_AddEditorSurfsCmd();   // flush the ED_SURF_MODEL surfs into THIS ortho frame
    // KIWI (REFIMG): material draw is legal here; lead the overlay tail so its wires stay above it.
    KiwiRefImage_DrawXY( wnd->m_nViewType, wnd->m_fScale );
    // Live marquee box (IDB XY_Draw 0x46d9a1): draw while a box-drag is active. The gate
    // mirrors the binary's `camera_fov_setup == sub_467700 && mode ∈ {area, 12..15}`
    // (Ed_PressCallback stands in for sub_467700 — see its note).
    {
        const select_t m = g_qeglobals.d_select_mode;
        if ( g_qeglobals.camera_fov_setup == (void *)&Ed_PressCallback
             && ( m == sel_area || m == sel_areapoint_vertex || m == sel_areabrush
               || m == sel_areabrush_sub || m == sel_areapoint_curve || m == sel_areapoint ) )
            Ed_DrawSelectionBox( wnd );
    }
    // Shift+X crosshair (IDB OnPaint 0x465bf7): drawn between XY_Draw and DrawClipper.
    if ( g_bCrossHairs )
        Ed_DrawCrosshair( wnd );
    if ( g_bClipMode )
        Ed_DrawClipper( wnd ); // clip points + numbered labels + split-preview wireframe
    Draw_PatchSelectPointsSelected();  // SELECTED curve points, light blue (binary DrawConnectionLinks prefix 0x40ca0f — before the handles)
    Ed_DrawVertexHandles();    // vertex/edge handles when in vertex-edit mode (shared with Cam_Draw)
    Draw_PatchSelectPoints();  // UNSELECTED curve-point candidates, green (binary XY_Draw 0x46d11b; self-gated on sel_curvepoint/sel_area)
    // size/measure overlay (IDB XY_Draw 0x46d965, PaintSizeInfo 0x46ba10): the selection
    // bounding-box dimensions + corner coordinate, gated on m_bSizePaint && !fixedsize &&
    // !rotate/scale && a non-empty selection.  nDim1=(viewType==YZ), nDim2=(viewType!=XY)+1.
    Ed_DrawSizeInfo( wnd, ( wnd->m_nViewType == ED_VIEW_YZ ), ( wnd->m_nViewType != ED_VIEW_XY ) + 1 );
    Ed_DrawConnectionLines();  // target/targetname + script_linkTo lines (self-gated on d_xyShowFlags&4)
    KiwiEntArrow_DrawXY( wnd->m_nViewType, wnd->m_fScale ); // KIWI-UX: Draw selected-entity facing over the 2D overlay tail.
    CopySelectedFaceValues();  // IDB XY_Draw tail 0x46db54: rebuild the selected faces' faceVis
    }   // end if (sceneOk) — degenerate-projection frame draws only the cleared background
    R_EndFrame();
    R_IssueRenderCommands( (uint)-1 );
    R_SortMaterials();
    R_CheckTargetWindow( hwnd );
}

// The dirty-flag gate (kiwi_viewdirty.h) is driven by g_nUpdateBits, which every edit /
// selection / filter / grid / layer / pan / zoom path already sets.  What it does NOT cover is
// an overlay that follows the MOUSE while no bits are set — a live marquee, a clip-point drag,
// the Shift+X crosshair — so while any of these is live the XY view is FORCE-DIRTY.
extern int drag_ok;   // drag.cpp:132 (IDB 0x23f1724)

bool XYWnd_OverlayIsLive()
{
    // Blanket default: while the 2D view owns a held mouse button, render every tick.  That
    // makes every button-driven view change right by construction instead of by enumeration.
    if ( Ed_ActiveXY()->m_nButtonstate != 0 )
        return true;
    if ( drag_ok )
        return true;
    if ( g_bCrossHairs || g_bClipMode || g_bRotateMode || g_bScaleMode )
        return true;
    const select_t m = g_qeglobals.d_select_mode;
    if ( g_qeglobals.camera_fov_setup == (void *)&Ed_PressCallback
         && ( m == sel_area || m == sel_areapoint_vertex || m == sel_areabrush
           || m == sel_areabrush_sub || m == sel_areapoint_curve || m == sel_areapoint ) )
        return true;
    return false;
}

// A SKIPPED tick still owes XYWnd_RenderToRT's two non-drawing side effects: the viewport's
// own size state (the mouse mapping reads m_nHeight) and CopySelectedFaceValues
// (XY_Draw tail 0x46db54), which keeps the surface inspector's windings current.
void XYWnd_TickSkipped( int w, int h )
{
    xywndState_t *wnd = Ed_ActiveXY();
    if ( w >= 1 && h >= 1 )
    {
        wnd->m_nWidth  = w;
        wnd->m_nHeight = h;
    }
    CopySelectedFaceValues();
}

// P5 RTT: the same XYWnd_Paint pipeline, rendering into RTT_XY's offscreen texture (for ImGui to
// sample) instead of a native window.  The window-setup (R_SetupRendertarget_CheckDevice) is
// replaced by RTT_Begin, the tail R_CheckTargetWindow is dropped, and the frame ends with RTT_End.
// The XY_Draw body is INLINE (mirrors XYWnd_Paint verbatim) and reaches the window through
// Ed_ActiveXY(), never an HWND.  `w`/`h` come from the ImGui dock cell.
void XYWnd_RenderToRT( int w, int h )
{
    PROF_SCOPED( "XY RenderToRT" );
    // The caller has already CONSUMED this view's dirty flag, so every bail below must put it
    // back or the view stays stale until the next invalidation.
    if ( !dx.device || w < 1 || h < 1 )
    {
        KiwiViewDirty_Mark( KIWI_DIRTYVIEW_XY );
        return;
    }
    xywndState_t *wnd = Ed_ActiveXY();
    // Drive the viewport's own size state from the dock-cell size (was set by XYWnd_OnSize).
    wnd->m_nWidth  = w;
    wnd->m_nHeight = h;
    {
        if ( !RTT_Begin( RTT_XY, w, h ) )   // points FRAME_BUFFER at the RT + suppresses Present
        {
            KiwiViewDirty_Mark( KIWI_DIRTYVIEW_XY );
            return;
        }
    }

    {
        R_BeginFrame();
        R_BeginSharedCmdList();
        // Clear to the XY background colour (savedinfo colour slot 1); 7 = colour+depth+stencil.
        R_AddCmdClearScreen( 7, g_qeglobals.d_savedinfo.colors[1], 1.0f, 0 );
        // The $line UNLIT pixel shader needs CONST_SRC_CODE_MATERIAL_COLOR; the bare grid draw
        // skips the per-material constant setup a scene render does, so set it white (the grid
        // lines carry their colour per-vertex).
        static const float s_edWhite[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        R_AddCmdSetMaterialColor( s_edWhite );
    }

    XYViewState vs;
    vs.viewType  = wnd->m_nViewType;
    vs.scale     = wnd->m_fScale;
    vs.origin[0] = wnd->m_vOrigin[0];
    vs.origin[1] = wnd->m_vOrigin[1];
    vs.origin[2] = wnd->m_vOrigin[2];
    vs.width     = wnd->m_nWidth;
    vs.height    = wnd->m_nHeight;
    vs.active    = wnd->m_bActive;

    // Degenerate-projection guard (defensive — the ortho projection is provably immune, see
    // XY_SetupScene): if the inverse-VP would be non-orthogonal, no begin-view was submitted,
    // so skip ALL geometry this frame and fall through to the frame teardown (the clear stands).
    const bool sceneOk = XY_SetupScene( &vs );
    if ( sceneOk ) {
    XY_DrawGrid( &vs );        // grid lines + coordinate/view-name labels
    if ( ( g_qeglobals.d_savedinfo.d_xyShowFlags & 0x10 ) == 0 )  // View→Show→Blocks (0x10 SET = hidden)
        XY_DrawBlockGrid( &vs );   // coarse 1024-unit block grid + block-index labels
    XY_DrawBrushes( &vs );     // brushes/entity bboxes + entity-name labels
    // turret share/ambush export-highlight (IDB XY_Draw 0x46d876: the twin sub_46AD20
    // passes, gated on the selected turrets' script_turret_share/ambush key lists).
    Ed_DrawTurretExportHighlights();
    DrawCameraIcon( &vs );     // the camera position+facing marker (IDB CXYWnd::DrawCameraIcon 0x469a40)
    DrawZIcon( &vs );          // the blue camera-position marker ("player indicator", IDB 0x469d50)
    if ( g_bRotateMode )       // rotate-mode pivot marker (IDB XY_Draw 0x46db4f, DrawRotateIcon_CenterSquare 0x469690)
        Ed_DrawRotateIconCenterSquare( wnd );

    // The XY brush loop draws every entity via DrawBrush, whose DrawModels dispatch (0x47b102
    // -> Editor_InstanceAndSkinModel, tech 29 + per-vert colour) queues each model's
    // ED_SURF_MODEL surfs - including misc_models INSIDE prefab contents.  The ortho scene is
    // the active projection, so the flush draws the mesh top-down/front/side.
    // MATERIAL_COLOR is reset to the binary's NEUTRAL {0,0,0,0} before the flush
    // (R_SetMaterialColor(NULL), XY_Draw 0x46d8fa): tech 29 wireframe_shaded LERPS the texture
    // toward materialColor.rgb by materialColor.w, so w=1 would flat-override the mesh white.
    // Line batches keep their own per-brush colours (pushed just before their own DrawLines).
    {
        static const float s_flushNeutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        extern void R_AddCmdSetMaterialColor( const float *rgba );
        R_AddCmdSetMaterialColor( s_flushNeutral );
    }
    R_AddEditorSurfsCmd();   // flush the ED_SURF_MODEL surfs into THIS ortho frame
    // KIWI (REFIMG): RTT twin, first in the material-capable overlay tail.
    KiwiRefImage_DrawXY( wnd->m_nViewType, wnd->m_fScale );
    // Live marquee box (IDB XY_Draw 0x46d9a1): draw while a box-drag is active. The gate
    // mirrors the binary's `camera_fov_setup == sub_467700 && mode ∈ {area, 12..15}`
    // (Ed_PressCallback stands in for sub_467700 — see its note).
    {
        const select_t m = g_qeglobals.d_select_mode;
        if ( g_qeglobals.camera_fov_setup == (void *)&Ed_PressCallback
             && ( m == sel_area || m == sel_areapoint_vertex || m == sel_areabrush
               || m == sel_areabrush_sub || m == sel_areapoint_curve || m == sel_areapoint ) )
            Ed_DrawSelectionBox( wnd );
    }
    // Shift+X crosshair (IDB OnPaint 0x465bf7): drawn between XY_Draw and DrawClipper.
    if ( g_bCrossHairs )
        Ed_DrawCrosshair( wnd );
    if ( g_bClipMode )
        Ed_DrawClipper( wnd ); // clip points + numbered labels + split-preview wireframe
    Draw_PatchSelectPointsSelected();  // SELECTED curve points, light blue (binary DrawConnectionLinks prefix 0x40ca0f — before the handles)
    Ed_DrawVertexHandles();    // vertex/edge handles when in vertex-edit mode (shared with Cam_Draw)
    Draw_PatchSelectPoints();  // UNSELECTED curve-point candidates, green (binary XY_Draw 0x46d11b; self-gated on sel_curvepoint/sel_area)
    // size/measure overlay (IDB XY_Draw 0x46d965, PaintSizeInfo 0x46ba10): the selection
    // bounding-box dimensions + corner coordinate, gated on m_bSizePaint && !fixedsize &&
    // !rotate/scale && a non-empty selection.  nDim1=(viewType==YZ), nDim2=(viewType!=XY)+1.
    Ed_DrawSizeInfo( wnd, ( wnd->m_nViewType == ED_VIEW_YZ ), ( wnd->m_nViewType != ED_VIEW_XY ) + 1 );
    Ed_DrawConnectionLines();  // target/targetname + script_linkTo lines (self-gated on d_xyShowFlags&4)
    KiwiEntArrow_DrawXY( wnd->m_nViewType, wnd->m_fScale ); // KIWI-UX: Mirror the facing overlay in the RTT XY path.
    CopySelectedFaceValues();  // IDB XY_Draw tail 0x46db54: rebuild the selected faces' faceVis
    }   // end if (sceneOk) — degenerate-projection frame draws only the cleared background
    {
        R_EndFrame();
    }
    R_IssueRenderCommands( (uint)-1 );
    {
        R_SortMaterials();
    }
    {
        RTT_End();
    }
}


// ═════════════════════════════════════════════════════════════════════════════
//  RELOCATED HOME — this function's embedded Assert() calls name THIS file as
//  their source (see the brush.cpp relocation protocol / line-uniqueness test).
// ═════════════════════════════════════════════════════════════════════════════
// deps of the moved viz cluster (previously file-local to scriptgroup.cpp):
extern int  ScriptGroup_Unreachable( const char *a1 );                 // scriptgroup.cpp 0x451170
extern void Ed_DrawScriptColorQuad( int entDef, const float *color );  // brush.cpp 0x46AE10
static const char zero[] = "";       // scriptgroup's `zero` (default team-key value)
// flt_73B098 (0x73B098) — the 7 script-colour token colours (r/b/y/c/g/p/o),
// indexed by ScriptGroup_Unreachable.  Same table as brush.cpp/camwnd.cpp.
static const float kScriptColorVizTable[7][4] = {
    { 1.0f, 0.0f, 0.0f, 1.0f },   // r — red
    { 0.0f, 0.0f, 1.0f, 1.0f },   // b — blue
    { 1.0f, 1.0f, 0.0f, 1.0f },   // y — yellow
    { 0.0f, 1.0f, 1.0f, 1.0f },   // c — cyan
    { 0.0f, 1.0f, 0.0f, 1.0f },   // g — green
    { 1.0f, 0.0f, 1.0f, 1.0f },   // p — purple
    { 1.0f, 0.4f, 0.0f, 1.0f },   // o — orange
};





// 0x46AA80  CamTokens_BrushMatchesToken (ecx=brush, arg0=token) — true if the brush
// entity's ScriptColorTeamKey value strstr-contains `token`.  Disasm-faithful: walks
// b->owner->def->epairs (selbrush+8 → entity+8 → epairs@0x74) for the team key
// (g_PrefsDlg+0x32C), defaulting to `zero`="" when absent.
static bool CamTokens_BrushMatchesToken( selbrush_t *b, const char *token )
{
    const char *teamKey = g_PrefsDlg->ScriptColorTeamKey.c_str();
    entity_s_def *def = (entity_s_def *)b->owner->def;        // [ecx+8]→[+8]
    const char *value = zero;                                 // IDB `zero` = ""
    for ( epair_t *ep = def->epairs; ep; ep = ep->next )      // [+0x74]
    {
        if ( !_stricmp( ep->key, teamKey ) ) { value = ep->value; break; }
    }
    return strstr( value, token ) != nullptr;
}

// 0x46AAE0  CamTokens_EntityGate (eax=brush) — true if the brush entity HAS the
// ScriptColorTeamKey AND its classname contains actor / node / info_volume.
// Disasm-faithful: v1 = b->owner->def; HasKeyValuePair(v1, teamKey) gate, then the
// classname epair-walk (default `zero`).
static bool CamTokens_EntityGate( selbrush_t *b )
{
    entity_s_def *def = (entity_s_def *)b->owner->def;        // *(int**)(*(int*)(a1+8)+8)
    if ( !HasKeyValuePair( def, g_PrefsDlg->ScriptColorTeamKey.c_str() ) )
        return false;
    const char *value = zero;
    for ( epair_t *ep = def->epairs; ep; ep = ep->next )
    {
        if ( !_stricmp( ep->key, "classname" ) ) { value = ep->value; break; }
    }
    return strstr( value, "actor" ) || strstr( value, "node" )
        || strstr( value, "info_volume" ) != nullptr;
}

// 0x46B110  ScriptGroup_DrawTeamColorViz (sub_46B110) — the script-group single-char
// team-colour visualization.  a1 = the team-colour token string (a selected trigger's
// ScriptColorTeamKey value); a2/a3 = the view-rect mins/maxs on the two view axes
// (XY_Draw's &v47 / &tdp); a4/a5 = the two world axis indices for the view plane (v58/v60).
//
// Copy a1 into a 1024 buffer; tokenize by " "; for each token
// store "<tok> " into a fixed 16-byte-stride array (v42) and its colour into a parallel
// 4-float-stride array (v34, token n → v34[4n+4] = kScriptColorVizTable[Unreachable(tok)]).
// Then if there is more than one token, find the first (among the first tokens-1) whose
// text contains ScriptColorKey and SWAP it (string + colour) with the LAST token — the 3
// overlapping strcpy/qmemcpy loops in the binary are exactly a token[M] ⟷ token[tokens-1]
// exchange via a 16-byte scratch (v43).  Finally, for every active then selected entity in
// view that passes !FilterBrush + CamTokens_EntityGate, draw a colour billboard
// (Ed_DrawScriptColorQuad) for each token the entity matches (CamTokens_BrushMatchesToken).
//
// MAX_COLORENTREES == 32 (the binary's `tokens < MAX_COLORENTREES` cap at XYWnd.cpp:3129).
void ScriptGroup_DrawTeamColorViz( const char *a1, const float *viewMins,
                                   const float *viewMaxs, int axis0, int axis1 )
{
    const int MAX_COLORENTREES = 32;   // camwnd idiom; scriptgroup's #define stayed behind
    char buf[1024];                          // v41 — strtok working copy of a1
    strcpy( buf, a1 );
    if ( !buf[0] )
        return;

    // token strings (16-byte stride, each "<tok> ") + parallel colours (token n at [4n+4]).
    char  tokStr[MAX_COLORENTREES][16];      // v42 (516 bytes; entry n = &v42[16*n])
    float tokCol[(MAX_COLORENTREES + 1) * 4];// v34 (132 floats; token n colour at [4n+4])
    int   tokens = 0;                        // i

    for ( char *t = strtok( buf, " " ); t; t = strtok( nullptr, " " ) )
    {
        iassert( t[0] );                     // XYWnd.cpp:3122 "token[0]"
        strcpy( tokStr[tokens], t );
        strcat( tokStr[tokens], " " );       // trailing space (binary appends asc_6D56FC)
        int ci = ScriptGroup_Unreachable( t );
        // binary indexes flt_73B098[4*ci] unconditionally; ci is -1 only on a no-match
        // (which would assert + read flt_73B098[-4]) — guard the never-valid case, matching
        // the established camwnd/brush.cpp convention.
        const float *c = ( ci >= 0 && ci <= 6 ) ? kScriptColorVizTable[ci]
                                                : kScriptColorVizTable[0];
        const int colSlot = 4 * tokens + 4;  // v34[4n+4]
        tokCol[colSlot + 0] = c[0];
        tokCol[colSlot + 1] = c[1];
        tokCol[colSlot + 2] = c[2];
        tokCol[colSlot + 3] = c[3];
        ++tokens;
        iassert( tokens < MAX_COLORENTREES );   // XYWnd.cpp:3129
    }

    // SWAP the first ScriptColorKey-matching token (among the first tokens-1) with the
    // LAST token (binary's 3 overlapping-copy loops, 0x46B268..0x46B382).
    const char *colorKey = g_PrefsDlg->ScriptColorKey.c_str();  // *(char**)(+0x330)
    const int last = tokens - 1;
    for ( int m = 0; m < last; ++m )
    {
        if ( strstr( tokStr[m], colorKey ) )
        {
            // save LAST token colour, copy MATCHED→LAST, restore saved→MATCHED (string too).
            float saveCol[4];
            saveCol[0] = tokCol[4 * last + 4 + 0];
            saveCol[1] = tokCol[4 * last + 4 + 1];
            saveCol[2] = tokCol[4 * last + 4 + 2];
            saveCol[3] = tokCol[4 * last + 4 + 3];
            char saveStr[16];
            strcpy( saveStr, tokStr[last] );

            tokCol[4 * last + 4 + 0] = tokCol[4 * m + 4 + 0];
            tokCol[4 * last + 4 + 1] = tokCol[4 * m + 4 + 1];
            tokCol[4 * last + 4 + 2] = tokCol[4 * m + 4 + 2];
            tokCol[4 * last + 4 + 3] = tokCol[4 * m + 4 + 3];
            strcpy( tokStr[last], tokStr[m] );

            tokCol[4 * m + 4 + 0] = saveCol[0];
            tokCol[4 * m + 4 + 1] = saveCol[1];
            tokCol[4 * m + 4 + 2] = saveCol[2];
            tokCol[4 * m + 4 + 3] = saveCol[3];
            strcpy( tokStr[m], saveStr );
            break;
        }
    }

    iassert( tokens > 0 );   // XYWnd.cpp:3150

    // Draw a colour billboard per matching token on every in-view active then selected
    // gated entity.  Cull: viewMaxs[0]>=def->mins[axis0] && viewMaxs[1]>=def->mins[axis1]
    //                    && viewMins[0]<=def->maxs[axis0] && viewMins[1]<=def->maxs[axis1].
    for ( int pass = 0; pass < 2; ++pass )
    {
        selbrush_t *head = pass ? &selected_brushes : &active_brushes;
        for ( selbrush_t *b = head->next; b != head; b = b->next )
        {
            brush_t *def = b->def;
            if ( !( viewMaxs[0] >= def->mins[axis0] && viewMaxs[1] >= def->mins[axis1]
                 && viewMins[0] <= def->maxs[axis0] && viewMins[1] <= def->maxs[axis1] ) )
                continue;
            if ( FilterBrush( b, 0 ) )
                continue;
            if ( !CamTokens_EntityGate( b ) )
                continue;
            for ( int n = 0; n < tokens; ++n )
            {
                if ( CamTokens_BrushMatchesToken( b, tokStr[n] ) )
                    Ed_DrawScriptColorQuad( (int)(intptr_t)b->owner->def, &tokCol[4 * n + 4] );
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  MFC shell — CXYWnd.  Each handler is a translation layer only (extract point/flags, bridge
//  the class members onto g_xywndState, call the free fn, chain the base class where it used
//  to).  U-GUARD flips this whole block off globally; the #ifndef here is the same gate.
//  XYWnd_MfcSyncIn/Out is the temporary U-GLOBALS bridge for the core TUs that still reach the
//  view state through the CXYWnd members (see its comment near the top of this file).
//  (The TU still needs U-GLOBALS before it compiles with the flag ON — Ed_XY_MouseDown /
//  Ed_XY_MouseMoved / DrawCameraIcon reach the camera through g_pParentWnd->m_pCamWnd, and
//  XY_ContextMenu / XYWnd_OnKeyDown reach CMainFrame.)
// ═════════════════════════════════════════════════════════════════════════════

// ═════════════════════════════════════════════════════════════════════════════
//  Raw-Win32 shell — the CXYWnd twin (U-VP-XY).  Same free fns, same conventions:
//    WM_CREATE      → XYWnd_OnCreate                        (CXYWnd::OnCreate tail)
//    WM_SIZE        → DefWindowProc, then XYWnd_OnSize       (MFC chains CWnd::OnSize FIRST)
//                     cx/cy = LOWORD/HIWORD(lParam), as MFC's ON_WM_SIZE thunk extracts them
//    WM_PAINT       → BeginPaint + XYWnd_Paint + EndPaint    (CPaintDC's job in the MFC shell)
//    WM_ERASEBKGND  → return 1                               (CXYWnd::OnEraseBkgnd returns TRUE)
//    WM_LBUTTONDOWN / WM_LBUTTONUP / WM_MOUSEMOVE / WM_RBUTTONDOWN / WM_RBUTTONUP /
//    WM_MBUTTONDOWN / WM_MBUTTONUP → free fn, return 0.  NONE of the CXYWnd handlers chain the
//                     base class (unlike CZWnd's move/up handlers), so MFC never reaches
//                     DefWindowProc for them either — returning 0 is the faithful twin.
//    WM_MOUSEWHEEL  → XYWnd_OnMouseWheel, return 0 when it returns TRUE (an MFC OnMouseWheel
//                     returning TRUE means "handled", i.e. no further processing); the point in
//                     lParam is in SCREEN coords for this message — passed through as such,
//                     exactly what MFC hands CXYWnd::OnMouseWheel.
//    WM_KEYDOWN     → XYWnd_OnKeyDown( nChar=wParam, nRepCnt=LOWORD(lParam), nFlags=HIWORD(lParam) )
//                     — MFC's ON_WM_KEYDOWN thunk decomposition — then return 0.
//    WM_DESTROY     → DefWindowProc, then XYWnd_OnDestroy    (MFC chains CWnd::OnDestroy FIRST)
//    x/y = (short)LOWORD/HIWORD(lParam) — client coords, exactly CPoint(lParam);
//    nFlags = wParam — the MK_* word MFC passes as UINT nFlags.
//  CAPTURE/FOCUS semantics live in the handler bodies (SetFocus/SetCapture on button-down,
//  ReleaseCapture on button-up), so both shells get them identically.
//  NOT in this twin: the RMB context menu (XY_ContextMenu is MFC CMenu — the modifier gate still
//  runs, the popup is skipped) and OnKeyDown's forward into CMainFrame; both marked U-GLOBALS.
//
//  d_hwndXY registration stays the CALLER's job in BOTH shells: mainfrm.cpp's
//  Radiant_CreateRenderWindows sets g_qeglobals.d_hwndXY from the new HWND (and creates the XY
//  window FIRST, before Camera/Z/Texture/blank-pane, because R_BeginRegistrationInternal asserts
//  and attaches the device to all five), and gfxwrapper.cpp's R_BeginRegistrationInternal later
//  calls R_InitRendererForWindow on it.  The caller must also seed the view type
//  (Ed_ActiveXY()->m_nViewType = ED_VIEW_XY) exactly as mainfrm.cpp does today.
// ═════════════════════════════════════════════════════════════════════════════

static const char *const XYWND_CLASS_NAME = "KIWIXYWnd";

LRESULT CALLBACK XYWnd_WndProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    switch ( msg )
    {
    case WM_CREATE:
        XYWnd_OnCreate( hwnd );
        return 0;

    case WM_SIZE:
    {
        LRESULT r = DefWindowProcA( hwnd, msg, wParam, lParam );
        XYWnd_OnSize( hwnd, (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return r;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint( hwnd, &ps );
        XYWnd_Paint( hwnd );
        EndPaint( hwnd, &ps );
        return 0;
    }

    case WM_LBUTTONDOWN:
        XYWnd_OnLButtonDown( hwnd, (unsigned int)wParam,
                             (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return 0;

    case WM_LBUTTONUP:
        XYWnd_OnLButtonUp( (unsigned int)wParam,
                           (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return 0;

    case WM_MOUSEMOVE:
        XYWnd_OnMouseMove( (unsigned int)wParam,
                           (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return 0;

    case WM_RBUTTONDOWN:
        XYWnd_OnRButtonDown( hwnd, (unsigned int)wParam,
                             (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return 0;

    case WM_RBUTTONUP:
        XYWnd_OnRButtonUp( hwnd, (unsigned int)wParam );
        return 0;

    case WM_MBUTTONDOWN:
        XYWnd_OnMButtonDown( hwnd, (unsigned int)wParam,
                             (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return 0;

    case WM_MBUTTONUP:
        XYWnd_OnMButtonUp();
        return 0;

    case WM_MOUSEWHEEL:
        // wParam: HIWORD = zDelta, LOWORD = the MK_* flags; lParam = the SCREEN point.
        if ( XYWnd_OnMouseWheel( hwnd, (short)HIWORD( wParam ),
                                 (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) ) )
            return 0;
        break;

    case WM_KEYDOWN:
        XYWnd_OnKeyDown( (unsigned int)wParam,
                         (unsigned int)LOWORD( lParam ), (unsigned int)HIWORD( lParam ) );
        return 0;

    case WM_DESTROY:
    {
        LRESULT r = DefWindowProcA( hwnd, msg, wParam, lParam );
        XYWnd_OnDestroy( hwnd );
        return r;
    }
    }
    return DefWindowProcA( hwnd, msg, wParam, lParam );
}

// The CXYWnd::PreCreateWindow class (CS_OWNDC + no background brush: we present via D3D, so
// the shell must not paint the client area) + the CWnd::Create style mainfrm.cpp passes.
HWND XYWnd_CreateRaw( HWND parent, int x, int y, int w, int h )
{
    static bool s_classRegistered = false;
    HINSTANCE   inst = GetModuleHandleA( nullptr );

    if ( !s_classRegistered )
    {
        WNDCLASSA wc = {};
        wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = XYWnd_WndProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursorA( nullptr, IDC_ARROW );
        wc.hbrBackground = nullptr;
        wc.lpszClassName = XYWND_CLASS_NAME;
        if ( !RegisterClassA( &wc ) )
            return nullptr;
        s_classRegistered = true;
    }

    // CREATED HIDDEN — the MFC style MINUS WS_VISIBLE.  ImGuiShell_ApplyViewportDocks
    // hides it on every pumped frame anyway (the viewport is an RTT texture), but the
    // first pumped frame is after Load_Materials, so at WS_VISIBLE it sat unpainted over
    // the frame for the whole boot material phase.  D3D9 does not require a visible window
    // (R_InitRendererForWindow / R_Hwnd_Resize only GetClientRect + CreateAdditionalSwapChain).
    return CreateWindowExA( 0, XYWND_CLASS_NAME, nullptr,
                            WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                            x, y, w, h, parent, nullptr, inst, nullptr );
}


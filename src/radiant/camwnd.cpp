#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// radiant/camwnd.cpp — the CCamWnd 3D perspective camera view (CoD4Radiant.exe).
// The world draw is IMMEDIATE-MODE (R_AddRenderCmdDrawTris) plus the editor
// surf-cache passes; the binary routes everything through DrawGeneralWorld_/DrawGeo.

#include "stdafx.h"
#include <universal/surfaceflags.h>
#include <csetjmp>                  // model-load asset-drop recovery guard (Stage B)
#include <universal/q_parse.h>      // Com_GetParseThreadInfo / negativeNumbers (collmap parse)
#include "mainfrm.h"                // CCamWnd, camera_s
#include "qe3.h"                    // g_qeglobals, selbrush_t, brush_t, face_t, MaterialDef, qtexture_s
#include "prefs.h"                  // g_PrefsDlg (camera_fov, enable_light_preview, preview_sun_aswell)
#include <gfx_d3d/r_gfx.h>          // GfxMatrix, GfxColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_init.h>         // dx, R_SetupRendertarget_CheckDevice, R_Hwnd_Resize
#include <gfx_d3d/r_scene.h>        // R_Ed_SetSceneParms
#include <gfx_d3d/r_rendercmds.h>   // R_BeginFrame/EndFrame, clear/material-color, MaterialTechniqueType
#include <gfx_d3d/r_state.h>        // CONST_SRC_CODE_SUN_POSITION/DIFFUSE/SPECULAR
#include "radiant_rtt.h"            // P5 RTT: RTT_Begin/RTT_End, RTT_CAMERA
#include "kiwi_camera.h"            // KIWI-UX (ROUND M): KiwiCam_Ortho / KiwiCam_OrthoHalfHeight
#include "kiwi_pick.h"              // KIWI-UX (ROUND AQ, ITEM 3): ray_t / Pick_RayFromImagePos
#include "kiwi_material.h"          // KIWI-UX (ROUND Y): kiwiMatDiag_t (the KiwiMatInfo readout)
#include "kiwi_skybox.h"            // KIWI-UX (ROUND AZ, ITEM 3 / BC, ITEM 3): KiwiSky_SeeThroughFace
#include "kiwi_uveditor.h"          // KIWI-UX (ROUND BN, ITEM 3): KiwiUvEd_OverlaySuppressed
#include <universal/profile.h>
#include "kiwi_shadowcache.h"       // the sun-preview cache magnitudes
#include "kiwi_lightcache.h"        // per-light caster records keyed by the shared epoch
#include "kiwi_light.h"             // KIWI Light helper policy and viewport overlay
#include "kiwi_sunshadow.h"         // shared default-on sun-preview preference/status
#include "kiwi_walkcache.h"         // the shared prefab-walk recording
#include "kiwi_surfcache.h"         // the camera's pose-invariant draw list
#include "kiwi_vec.h"               // KIWI_PI
#include "kiwi_str.h"               // KiwiStr_ContainsNoCase (see-through lightgrid volumes)
#include "kiwi_ported.h"            // KIWI: shared engine/editor externs
extern int g_svNodesWalked;         // shadowvolume.cpp:28
extern int g_svCastersDrawn;        // shadowvolume.cpp:29
extern int g_svTrisFed;             // shadowvolume.cpp:30
extern int g_svTrisKept;            // shadowvolume.cpp:31
extern int g_svBatches;             // shadowvolume.cpp:32
extern int g_svBatchKB;             // shadowvolume.cpp:33
#include <universal/com_math.h>     // AngleVectors
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// Brush_Ray (0x475fe0) headless-safe distance wrapper — select.cpp.
extern bool       Ed_BrushFloorRay( brush_t *def, const float *start,
                                     const float *dir, float *outDist );

extern int  MaterialDef_11( MaterialDef *m );                 // materialdef.cpp — layer count
extern void Draw_PatchSelectPoints();                         // brush.cpp (0x40c250) — sel-curve-point overlay
// Face_BuildLayerGeom + EdLayerGeom are declared in qe3.h.
extern void SunPrev_Setup();                                  // brush.cpp (Stage 2) — read worldspawn sun
extern int  SunPrev_Active();                                 // brush.cpp — sun key present
extern void SunPrev_FaceShade( const float *wn, float *out4 );// brush.cpp — per-face sun colour
extern void KiwiCon_DrawSelectionOverlay();                   // KIWI: kiwi_construct.cpp — selection over the region fills

// kisak's BSP sun-light parser/interpreter (r_bsp_load_obj.cpp) — the same pair the binary's
// R_SunPrev_SetSunConstants feeds the worldspawn text to.  Not header-declared for the editor.
extern "C++" char *__cdecl R_ParseSunLight( SunLightParseParams *params, char *text );
extern void __cdecl R_InterpretSunLightParseParamsIntoLights( SunLightParseParams *sunParse, GfxLight *sunLight );

// ─── camwndState_t — the 3D camera viewport's state (U-VP-CAM) ──────────────
// The camera view is a singleton (one window, one state block), so this is a file-scope
// instance the handlers at the bottom of this file read/write.  Every member below was a
// CCamWnd member in the binary; the trailing field offsets are CCamWnd's.
//   camera        = CCamWnd::camera, THE editor camera, published by Ed_Camera() below.
//   width/height  = CCamWnd::m_nWidth / m_nHeight (client size, latched in OnCreate/OnSize).
//   m_pt*         = POINT, not CPoint (CPoint IS a POINT; every use here is .x/.y only).
//   contextMenu   = CCamWnd::m_contextMenu's m_hMenu (+0x338), a bare HMENU.
//
// U-GUARD HAZARD: the binary's CMainFrame::OnDynamicLighting (0x429960, menu 32854) builds a
// SECOND CCamWnd as a floating popup (dead there too: its hwnd is never registered with the
// renderer).  With this state a singleton, that popup's WM_SIZE/WM_CREATE/mouse input would
// write the REAL camera's state, so mainfrm.cpp makes 32854 a no-op.
struct camLightPreviewRec_t   // == CCamWnd::LightPreviewRec, 56 B (the binary's 14-dword stride)
{
    selbrush_t   *inst;
    selbrush_t   *arg2;
    orientation_t orient;
};
static_assert( sizeof(camLightPreviewRec_t) == (sizeof(void *) == 8 ? 64 : 56), "camLightPreviewRec_t" );

struct camwndState_t
{
    camera_s camera = {};                 // embedded camera state (CCamWnd::camera)
    int      width  = 0;                  // m_nWidth  — client width  (px)
    int      height = 0;                  // m_nHeight — client height (px)

    // 3D-view mouse interaction state (CamWnd_DropModelsToPlane / Cam_MouseControl /
    // Cam_MouseUp / Cam_MouseMoved).
    unsigned int m_nCambuttonstate = 0;   // +0xd4  MK_ flags of the active press
    POINT  m_ptButton  = { 0, 0 };        // +0xd8  press point (flipped Y, client coords)
    bool   cam_was_not_dragged = false;   // +0xe0  set on a plain RMB press (no drag yet)
    POINT  m_ptCursor  = { 0, 0 };        // +0xe4  cursor anchor for the view-control drags
    POINT  m_ptLastCursor = { 0, 0 };     // +0xec  OnMouseMove dedup (skip no-move events)
    int    prob_some_cursor = 0;          // +0x128 accumulated cursor dx (texture rotate/shift snap)
    int    x47 = 0;                       // +0x12c accumulated cursor dy (texture rotate/shift snap)
    int    cursor_visible = 1;            // +0x130 cursor-shown flag; Cam_MouseUp restores it to 1

    // light-region preview records (Regions_ForSelected reads these): +0x134 / +0x2F4.
    camLightPreviewRec_t light_preview_arr[8] = {};
    int                  light_preview_count = 0;

    HMENU  contextMenu = nullptr;     // +0x338 the RMB face-picker popup (CMenu::m_hMenu)

    // CCamWnd::CCamWnd == the binary's Cam_Init (0x402c40): CMainFrame::CreateQEChildren
    // spawns the camera at (0, 20, 46) with yaw 0 (look +X) and draw_mode 1 (Cam_Draw maps
    // mode 1 to TECHNIQUE_UNLIT).  Static init runs long before the window is created.
    camwndState_t()
    {
        camera.angles[1] = 0.0f;          // yaw → look +X
        camera.origin[0] =  0.0f;
        camera.origin[1] = 20.0f;
        camera.origin[2] = 46.0f;
        camera.draw_mode = 1;
    }
} g_camwndState;

// ─── Ed_Camera — THE editor camera (U-VP-CAM / U-GLOBALS) ─────────
// There is exactly ONE camera, so every TU reaches it here, with no window class in the path.
// It never returns NULL.
camera_s *Ed_Camera()
{
    return &g_camwndState.camera;
}

// KIWI-UX (ROUND M): half-depth of the ORTHOGRAPHIC view volume, measured either
// side of the eye along the view axis.  131072 was the engine's own world bound
// (Brush_BuildWindings seeds its AABB at ±131072, brush.cpp:1459), so the slab
// spanned everything that can exist in front of OR behind an ortho eye.
// ── KIWI-UX (ROUND BC, ITEM 1) ──────────────────────────────────────────────
// …and that reasoning quietly conflated two different measurements.  The world
// bound is the largest coordinate a BRUSH VERTEX can hold; this slab is measured
// from the EYE, and in ortho the eye is a PSEUDO-eye parked KiwiCam_Distance()
// behind the pivot — it is not in the world and nothing is stored there.  The
// slab therefore has to cover (standoff + world extent), not (world extent), and
// sizing it at the world bound is what made the zoom-out ceiling 65536 in round
// AZ.  USER REPORT: "The zoom out limit is too restrictive.  Allow more zoom
// out."  524288 = 2^19 gives a 262144 ortho ceiling on the SAME depth/2
// derivation.
// ── KIWI-UX (CLEANUP, C-24) ─────────────────────────────────────────────────
// The number used to be written out here AND in kiwi_camera.cpp, under two
// names, coupled by nothing but "MUST equal" comments on both sides.  It is now
// defined once as KCAM_ORTHO_DEPTH_HALF in kiwi_camera.h (included at :22) and
// this local is that constant — same value, no pact to keep.
// Cost, stated: 2*524288 units of LINEAR depth over a 24-bit
// buffer is 1/16 unit per step where it used to be 1/64.  Brush coordinates are
// integers and coplanar faces tie either way, so nothing the editor draws stops
// resolving; another doubling would start to.
static const float KCAM_ORTHO_DEPTH = KCAM_ORTHO_DEPTH_HALF;   // ROUND BC: was 131072

// KIWI-UX (ROUND M): how far BEHIND the eye plane an orthographic PICK ray starts.
//
// Why it is needed at all: the ortho view volume is symmetric about the eye, so
// geometry behind the eye plane is DRAWN — unlike perspective, where it is simply
// gone.  A pick ray that started at the eye plane would therefore miss things the
// user can plainly see (the near wall of a room you have zoomed inside), and worse,
// the screen-space vertex/edge pass (kiwi_pick.cpp ProjectRaw, which has no
// behind-the-eye rejection in ortho) WOULD find them — two picks disagreeing.
//
// Why 16384 and not KCAM_ORTHO_DEPTH: the lead is pure numerical cost.  Test_Ray
// seeds its hit distance at 262144 (select.cpp:757) so anything up to that is
// legal, but the returned point is `start + dir*dist`, and float32 resolution at
// 131072 is ~0.0078 units where at 16384 it is ~0.00098.  16384 units is 1365 feet
// of lead — past any room the near-geometry case is about — for an eighth of the
// error.
static const float KCAM_ORTHO_PICK_LEAD = 16384.0f;

// 0x403470  Cam_BuildMatrix — view basis from the angles.  AngleVectors wants pitch NEGATED;
// forward/right are the yaw-plane movement basis (the binary's AngleVectors_YawPlane).
void CamWnd_BuildMatrix()
{
    camera_s &camera = g_camwndState.camera;

    float a[3] = { -camera.angles[0], camera.angles[1], camera.angles[2] };
    AngleVectors( a, camera.vpn, camera.vright, camera.vup );

    float flat[3] = { 0.0f, camera.angles[1], 0.0f };
    float up[3];
    AngleVectors( flat, camera.forward, camera.right, up );   // yaw-plane forward/right
}

// Cam_Draw's prologue: the R_SetupScene (0x506570) perspective projection + R_Ed_SetSceneParms
// with axis = { vpn, -vright, vup }.  R_SetupProjection (0x4a78e0) is inlined.
// R_Ed_ProjectionWouldBeValid has no binary counterpart — it is a call-site guard around
// the shared r_state_utils.cpp:20 inverse-VP assert.  On far-from-origin maps (blackout at world
// X ~ -175000) the float32 MatrixInverse44 loses the sign of the inverse-VP's m[3][3], so the
// assert fires on a projection the forward render handles fine.  Returns false to drop just that
// frame (the cleared background stands) instead of crashing.
bool CamWnd_SetupScene()
{
    camera_s &camera = g_camwndState.camera;


    CamWnd_BuildMatrix();

    float axis[3][3];
    axis[0][0] =  camera.vpn[0];    axis[0][1] =  camera.vpn[1];    axis[0][2] =  camera.vpn[2];
    axis[1][0] = -camera.vright[0]; axis[1][1] = -camera.vright[1]; axis[1][2] = -camera.vright[2];
    axis[2][0] =  camera.vup[0];    axis[2][1] =  camera.vup[1];    axis[2][2] =  camera.vup[2];

    const float fov  = g_PrefsDlg->camera_fov;                      // "Fov" pref (default 65)
    const int   w    = camera.width  > 0 ? camera.width  : 1;
    const int   h    = camera.height > 0 ? camera.height : 1;
    float tanY = tanf( DEG2RAD( fov ) * 0.5f ) * 0.75f;
    float tanX = tanY * (float)w / (float)h;
    // R_SetupScene: zNear = max(r_znear, 0.01).  0.01 is the CLAMP FLOOR, not the near
    // plane — using it directly makes the depth range ~400x too coarse (coplanar tool faces Z-fight).
    float zNear = (float)Dvar_GetFloat( "r_znear" );                 // default 4.0
    if ( 0.0099999998f - zNear >= 0.0f )                            // floor at 0.01
        zNear = 0.0099999998f;

    GfxMatrix proj;
    memset( &proj, 0, sizeof( proj ) );
    proj.m[0][0] = 0.99951171875f / tanX;
    proj.m[1][1] = 0.99951171875f / tanY;
    proj.m[2][2] = 0.99951172f;
    proj.m[2][3] = 1.0f;
    proj.m[3][2] = 0.99951171875f * -zNear;

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND M) — ORTHOGRAPHIC PROJECTION.  The ONE ported-code change
    //  the ortho toggle needs, and it replaces exactly the four matrix stores
    //  above; nothing else in Cam_Draw is touched.
    // ═════════════════════════════════════════════════════════════════════════
    // USER DIRECTIVE: "while at the perfect axis align plane angle, it doesn't
    // show a box as just a square ... We need an orthographic and perspective
    // camera toggle."
    //
    // WHY THIS IS SAFE, verified before it was written rather than assumed: the
    // 2D views already drive an ORTHOGRAPHIC GfxMatrix through this very entry
    // point (xywnd.cpp XY_SetupScene -> XY_SetupProjectionMtx -> the same
    // R_Ed_ProjectionWouldBeValid / R_Ed_SetSceneParms pair), so every consumer
    // downstream — R_SetupViewProjectionMatrices, the inverse-VP,
    // R_DeriveNearPlaneConstantsForView — already handles a w-free projection.
    //
    // THE MATRIX.  The row-vector convention is read straight off the
    // perspective build above: m[2][3] = 1 is what puts view-space Z into clip w,
    // so component 2 is FORWARD depth and 0/1 are horizontal/vertical.  Ortho is
    // therefore that same matrix with the w row zeroed and m[3][3] = 1:
    //     m[0][0] = C / halfWidth        (halfWidth = halfHeight * aspect)
    //     m[1][1] = C / halfHeight
    //     m[2][2] = C / (zFar - zNear)
    //     m[3][2] = C * -zNear / (zFar - zNear)
    //     m[3][3] = 1
    // with C the SAME 0.99951171875 guard-band constant the perspective path
    // carries, so the two projections frame identically rather than differing by
    // a 2047/2048 hair at the toggle.
    //
    // halfHeight is KiwiCam_OrthoHalfHeight() == distance-to-pivot * tanY, so the
    // toggle is scale-continuous AT THE PIVOT and the wheel keeps zooming (it
    // moves s_dist; see kiwi_camera.h).  The aspect uses tanX/tanY, which is
    // width/height by construction two lines up.
    //
    // DEPTH is SYMMETRIC about the eye (zNear = -KCAM_ORTHO_DEPTH, zFar =
    // +KCAM_ORTHO_DEPTH).  In an orthographic view the eye POINT is arbitrary
    // along the view axis — there is no "behind the camera" the way a frustum
    // has one — so a one-sided near plane would clip away geometry the user is
    // looking straight at after any dolly.  (ROUND BC: 1,048,576 units of LINEAR
    // depth over a 24-bit buffer is ~1/16 unit per step — was ~1/64 at the old
    // 131072 half-depth — still far finer than the perspective path's hyperbolic
    // distribution ever manages at range.  See KCAM_ORTHO_DEPTH.)
    //
    // The inverse-VP guard below still runs and still passes by construction: an
    // ortho proj has m[0..2][3] = 0 and m[3][3] = 1, the viewer matrix is affine,
    // so the product's last column is (0,0,0,1) and so is its inverse's — which
    // is exactly the (|m03|,|m13|) << m33 relation r_state_utils.cpp:21-22
    // asserts on.
    const bool kiwiOrtho = KiwiCam_Ortho();
    // The ortho arm's own numbers, hoisted so the section fold below can reach them.
    float kiwiOrthoHalfW = 0.0f, kiwiOrthoHalfH = 0.0f, kiwiOrthoDepth = 0.0f;
    if ( kiwiOrtho )
    {
        const float halfH = KiwiCam_OrthoHalfHeight();
        const float halfW = halfH * ( tanX / tanY );          // == halfH * w/h
        // ── KIWI-UX (ROUND BM, ITEM 1a): THE SLAB IS SYMMETRIC AGAIN ────────
        // Round BK made the NEAR half a variable (KiwiCam_OrthoBackDepth) so that
        // holding the forward arrow could pull it up to the eye and "cut into" a
        // building.  USER DIRECTIVE: *"Get rid of this arrow key cross section
        // feature, it's awful."*  A cutting plane welded to the eye cannot be
        // aimed, parked, or looked at from another angle, which is why it went; the
        // section is a WORLD Z LEVEL now (KIWI-UX ROUND BP: folded into this very
        // projection as an oblique near plane, immediately below), and this base
        // projection is byte-for-byte the pre-BK one.
        const float depth = KCAM_ORTHO_DEPTH;                  // zNear = -depth, zFar = +depth
        memset( &proj, 0, sizeof( proj ) );
        proj.m[0][0] = 0.99951171875f / halfW;
        proj.m[1][1] = 0.99951171875f / halfH;
        proj.m[2][2] = 0.99951171875f / ( 2.0f * depth );
        proj.m[3][2] = 0.99951171875f * 0.5f;                  // -zNear/(zFar-zNear) = 0.5
        proj.m[3][3] = 1.0f;
        kiwiOrthoHalfW = halfW;
        kiwiOrthoHalfH = halfH;
        kiwiOrthoDepth = depth;
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BP, ITEM 1b) — SECTION ANALYSIS IS AN OBLIQUE NEAR PLANE
    // ═════════════════════════════════════════════════════════════════════════
    // The D3D9 user clip plane is retired: the user's own first-light log has the
    // device ACCEPTING SetClipPlane + D3DRS_CLIPPLANEENABLE (all HRESULTs S_OK,
    // the render state reading back as D3DCLIPPLANE0, six planes advertised) and
    // cutting nothing — the known behaviour of user clip planes under a
    // programmable vertex shader.  The cut is folded into the PROJECTION instead,
    // which is the one thing on this path the user has already seen work (round
    // BK's ortho slab).  The whole derivation, in this tree's row-vector
    // convention, is on KiwiSection_ObliqueDepthColumn (kiwi_section.cpp); in one
    // line, D3D's near plane IS column 2 of the projection, so replacing column 2
    // with the section plane makes the section the near plane.
    //
    // THE VALIDITY GATE BELOW STILL SEES THE **BASE** PROJECTION, deliberately.
    // R_Ed_ProjectionWouldBeValid asks whether the float32 inverse-VP keeps the
    // ORTHO-LIKE form R_DeriveNearPlaneConstantsForView wants (|m03|,|m13| << m33)
    // — a question about the camera's world position and the base frustum, which is
    // what it was written for (a far-from-origin camera losing the sign of m33).
    // An obliquely-folded PERSPECTIVE projection violates that form by
    // construction and for a reason that is not degeneracy at all: clip.z now
    // depends on clip.x/clip.y.  Asking the guard about the folded matrix would
    // therefore drop every sectioned frame.  The near-plane constants it protects
    // are consumed only by depth-reconstruction shaders the editor never binds, and
    // r_state_utils.cpp carries a fenced guard so the derivation cannot divide by a
    // zero m33 either.  (In ORTHO the fold changes nothing about that form at all —
    // the projection's last column stays (0,0,0,1) — so this note is about the
    // perspective arm.)
    extern bool KiwiSection_ObliqueDepthColumn( const float origin[3], const float vpn[3],
                                                const float vright[3], const float vup[3],
                                                bool ortho, float guardC,
                                                float tanX, float tanY, float zNear,
                                                float halfW, float halfH, float depthHalf,
                                                float outCol[4] );              // kiwi_section.h
    extern void KiwiSection_NoteFrameCut( bool cutting );                       // kiwi_section.h

    const GfxMatrix projBase = proj;        // what the validity gate is asked about
    float      col[4];
    const bool folded = KiwiSection_ObliqueDepthColumn(
        camera.origin, camera.vpn, camera.vright, camera.vup,
        kiwiOrtho, 0.99951171875f, tanX, tanY, zNear,
        kiwiOrthoHalfW, kiwiOrthoHalfH, kiwiOrthoDepth, col );
    if ( folded )
    {
        proj.m[0][2] = col[0];
        proj.m[1][2] = col[1];
        proj.m[2][2] = col[2];
        proj.m[3][2] = col[3];
    }

    if ( !R_Ed_ProjectionWouldBeValid( camera.origin, (const float (*)[3])axis, &projBase ) )
    {
        static int s_camProjDropped = 0;
        s_camProjDropped++;
        if ( ( s_camProjDropped & ( s_camProjDropped - 1 ) ) == 0 )   // log at 1,2,4,8,... (no spam)
            Radiant_FL_Log( "Cam_SetupScene: dropped degenerate-projection frame #%d "
                            "(org=%.0f,%.0f,%.0f ang=%.2f,%.2f fov=%.1f)",
                            s_camProjDropped, camera.origin[0], camera.origin[1], camera.origin[2],
                            camera.angles[0], camera.angles[1], fov );
        // A dropped frame renders NOTHING, so it certainly does not cut — and the
        // pick clamp must agree with the frame that was actually drawn, not with the
        // one that was intended.
        KiwiSection_NoteFrameCut( false );
        return false;
    }

    // THE PICK CLAMP FOLLOWS THE RENDER, EXACTLY (round BP, item 3): the section's
    // ray clamp and its PointVisible test are gated on this answer, so a section
    // that is armed but not cutting cannot bend a single pick ray.  Set only once
    // the frame is committed, below the drop above.
    KiwiSection_NoteFrameCut( folded );

    R_Ed_SetSceneParms( camera.origin, (const float (*)[3])axis, &proj );
    return true;
}

// A face's mtldef carries a radMtl (qtexture_s) whose ->next is the engine Material* handle.
static Material *FaceMaterial( const MaterialDef *md )
{
    if ( md->radMtl )
        return md->radMtl->next;
    return nullptr;
}

// Per-face scratch (a brush face has at most MAX_POINTS_ON_WINDING verts; 64 is ample).
static const int CAM_MAXFACEVERTS = 64;

// camera.draw_mode -> MaterialTechniqueType, per Cam_Draw.
// CASE_TEXTURE must stay OFF in g_useTechnique (r_material_load_obj.cpp): enabling slot 0x1B
// makes Material_LoadTechniqueSet REQUIRE case_texture for every l_sm_* world techset, which
// binds a code image kisak never sets up (R_LoadCaseTextures unported) — the whole techset load
// then fails and the world material falls back to the "2d" default.  Cam_TechAvailable demotes
// mode 4 to UNLIT as a result.
static MaterialTechniqueType Cam_TechForDrawMode( int mode )
{
    switch ( mode )
    {
    case 0:  return TECHNIQUE_WIREFRAME_SHADED;   // wireframe (filled, wireframe state)
    case 1:  return TECHNIQUE_UNLIT;              // fullbright: colormap * MATERIAL_COLOR
    case 2:  return TECHNIQUE_FAKELIGHT_NORMAL;   // textured + normal fake-light
    case 3:  return TECHNIQUE_FAKELIGHT_VIEW;     // textured + view fake-light
    case 4:  return TECHNIQUE_CASE_TEXTURE;       // case texture
    default: return TECHNIQUE_UNLIT;              // the binary's tech_type=4 (UNLIT) default
    }
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AV, ITEMS 1+2) — "View -> Entities as..." DRIVES THE MESH TECHNIQUE.
// ═════════════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "there is a view -> entities as... -> option class that has no
// affect. The ents are always rendered as wireframe currently" / "they turn blue when not
// selected".
//
// ONE ROOT CAUSE FOR BOTH.  The six menu handlers (mainfrm.cpp:3292-3303, cmd ids
// 32909/32916/32911/32912/32913/32914 dispatched at mainfrm.cpp:5542-5547, menu items in
// res/radiant.rc:121-128) DO run and DO store the mode: g_PrefsDlg->m_nEntityShowState,
// persisted as "EntityShow" (prefs.cpp:284) and defaulted to 65552 = 0x10010 = SKINNED
// (prefs.cpp:70/:177-179).  The state is NOT uninitialised.  It was simply never read by
// anything that decides how a model DRAWS.  Before this round the only readers in the whole
// build were:
//   * brush.cpp:5379   Entity_HasRenderableModel — show == 4096 (hide all) and
//                      show & 0x100 (selected-only).  So "Bounding box", "Selected
//                      Wireframe" and "Selected Skinned" already filtered correctly.
//   * brush.cpp:6727   DrawOriginBox — the & 0x1000 BOXED bbox arm (behind RADIANT_DECOR).
//   * brush.cpp:7320   the prefab-CONTENT technique (& 1 -> 29).
//   * select.cpp:397/:636  the pick chain.
// Nothing read WIREFRAME (0x1) or SKIN_MODEL (0x10) for the xmodel MESH.  Instead the mesh
// technique came from this file's own invention, `entTech = (classtype & 0x18) ? worldTech
// : 29` — so ONLY misc_model / script_model / script_vehicle / dyn_model / misc_prefab ever
// got a skinned mesh, and EVERY other model-bearing class was hardwired to 29.
//
// WHY THAT MADE THEM BLUE, AND WHY THE BLUE ITSELF IS FAITHFUL.  The classes the user is
// looking at are the AI spawners synthesized out of main/aitype/*.gsc by Load_Defs /
// Init_ScanFiles (eclass.cpp:1676): e.g.
//     /*QUAKED actor_ally_blackkit_AR_m4grunt (0.0 0.25 1.0) (-16 -16 0) (16 16 72) ...
//     defaultmdl="body_complete_sp_sas_ct_benjamin"
// (0.0 0.25 1.0) is the ALLIES TEAM COLOUR — axis classes are (1.0 0.25 0.0), neutral
// (0.5 0.5 0.5); the synthesized weapon_* classes (eclass.cpp:1645) are (.3 .3 1).  Those
// classes carry a real character xmodel via defaultmdl= but their classtype is only 0x2
// (eclass.cpp:925-939 gives 0x8 to four names and 0x10 to misc_prefab, nothing else), so
// entTech was 29 -> SkinModelInst's `draw_meth2 != 29 ? 0 : color` arm (r_ed_scene.cpp:641)
// stamped the eclass colour over every skinned vertex -> a BLUE WIREFRAME CHARACTER.  And
// white when selected, because the tech-29 white-outline pass (:3071) stamps colorWhite.
// The COLOUR mechanism is the binary's own and is left exactly as it is: at tech 29 an
// entity wireframe is supposed to be its class/team colour.  What diverged is the
// TECHNIQUE, and fixing that removes the tint by removing the arm that applies it
// (colorPtr is NULL for any meshTech != TECHNIQUE_WIREFRAME_SHADED) rather than by touching a colour constant.
//
// THE MODE BITS, from the six handlers' immediates (mainfrm.cpp:3290-3291):
//   WIREFRAME 0x1  SKIN_MODEL 0x10  SELECTED_ONLY 0x100  BOXED 0x1000  SKINNED 0x10000
//   Bounding box       0x1000   -> Entity_HasRenderableModel returns 0, bbox only
//   Wireframe          0x10001  -> mesh at 29 (class colour)
//   Selected Wireframe 0x101    -> mesh at 29, selected entities only
//   Selected Skinned   0x110    -> mesh at the camera technique, selected only
//   Skinned            0x10010  -> mesh at the camera technique          [DEFAULT]
//   Skinned and Boxed  0x11010  -> mesh at the camera technique + DrawOriginBox
// The selected-only filtering and the hide-all case already work upstream in
// Entity_HasRenderableModel, so this helper only has to answer the mesh technique.
static int Cam_EntityMeshTech( MaterialTechniqueType cameraTech )
{
    const int show = g_PrefsDlg->m_nEntityShowState;
    if ( ( show & 0x1 ) != 0 )        // WIREFRAME / Selected Wireframe
        return 29;                    // TECHNIQUE_WIREFRAME_SHADED + the per-vertex class colour
    if ( ( show & 0x10 ) != 0 )       // SKIN_MODEL: Skinned / Selected Skinned / Skinned+Boxed
        return (int)cameraTech;
    return 29;                        // Bounding box (0x1000): no mesh reaches this anyway
}

// Direct-mapped memo in front of Cam_EditorMaterialColor's twelve strstr calls, keyed on
// the interned material NAME POINTER (qtexture_s::name, qe3.h:54) and RESET at the top of
// every CamWnd_Draw — so no entry can outlive the frame the pointer was observed in.
static const int CAM_EDCOL_MEMO = 64;
static struct { const char *name; float col[4]; bool isEditor; } s_edColMemo[CAM_EDCOL_MEMO];

// Called once per CamWnd_Draw, before any pass gathers.
static void Cam_EditorMaterialColorMemoReset()
{
    for ( int i = 0; i < CAM_EDCOL_MEMO; ++i )
        s_edColMemo[i].name = nullptr;
}

static bool Cam_EditorMaterialColorUncached( const char *name, float out[4] );

// Editor flat colour for a material name: true (+ colour) for a tool/sky material, false (+
// white) for a plain world material.  The world draw splits on this — tools/sky get UNLIT +
// flat MATERIAL_COLOR, world materials go through FAKELIGHT.
static bool Cam_EditorMaterialColor( const char *name, float out[4] )
{
    if ( !name )
    {
        out[0] = out[1] = out[2] = out[3] = 1.0f;
        return false;
    }
    // Pointer hash: the low bits of a heap pointer are the allocation's own alignment,
    // so mix in the higher ones.
    const uintptr_t p = (uintptr_t)name;
    const int slot = (int)( ( ( p >> 4 ) ^ ( p >> 12 ) ) & ( CAM_EDCOL_MEMO - 1 ) );
    if ( s_edColMemo[slot].name == name )
    {
        out[0] = s_edColMemo[slot].col[0]; out[1] = s_edColMemo[slot].col[1];
        out[2] = s_edColMemo[slot].col[2]; out[3] = s_edColMemo[slot].col[3];
        return s_edColMemo[slot].isEditor;
    }
    const bool isEditor = Cam_EditorMaterialColorUncached( name, out );
    s_edColMemo[slot].name     = name;
    s_edColMemo[slot].col[0]   = out[0]; s_edColMemo[slot].col[1] = out[1];
    s_edColMemo[slot].col[2]   = out[2]; s_edColMemo[slot].col[3] = out[3];
    s_edColMemo[slot].isEditor = isEditor;
    return isEditor;
}

// The original body, unchanged.
static bool Cam_EditorMaterialColorUncached( const char *name, float out[4] )
{
    out[0] = out[1] = out[2] = out[3] = 1.0f;     // default: white (textured world material)
    if ( !name ) return false;
    const char *n = name;
    const char *slash = strrchr( name, '/' );      // strip "wc/" / any path
    if ( slash ) n = slash + 1;
    static const struct { const char *key; float r, g, b; } tbl[] = {
        { "$opaque",   0.55f, 0.50f, 0.42f },       // caulk / no-texture fallback -> tan-grey
        { "sky",       0.45f, 0.62f, 0.92f },       // sky -> blue
        { "caulk",     0.55f, 0.50f, 0.42f },       // caulk -> tan-grey
        { "portal",    0.30f, 0.35f, 0.85f },
        { "hint",      0.88f, 0.85f, 0.20f },
        { "skip",      0.88f, 0.55f, 0.20f },
        { "nodraw",    0.42f, 0.42f, 0.42f },
        { "clip",      0.85f, 0.30f, 0.55f },
        { "trigger",   0.30f, 0.78f, 0.34f },
        { "origin",    0.78f, 0.30f, 0.78f },
        { "volume",    0.30f, 0.72f, 0.72f },        // lightgrid_volume / *_volume
        { "lightgrid", 0.30f, 0.72f, 0.72f },
    };
    for ( const auto &e : tbl )
        if ( strstr( n, e.key ) ) { out[0] = e.r; out[1] = e.g; out[2] = e.b; return true; }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND M) — DOES THIS MATERIAL WRITE DEPTH AT THIS TECHNIQUE?
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT: "There is a bug somewhere where textures are drawing over other
// boxes (wrong z order) after cutting.  See if you can find and fix that, could
// be related to how it was caulk after cutting."
//
// THE DIAGNOSIS, read out of the shipped assets rather than guessed:
//   1. A Cut/Split gives its two new faces the CAULK material — the clipper's own
//      synthesis, hoisted into Ed_BuildClipFaceMaterial_Kiwi (xywnd.cpp:2240
//      picks "caulk" unless a source face is nodraw_decal).
//   2. The world fill below routes ANY tool-class material — and
//      Cam_EditorMaterialColor's table (this file, "caulk" row) classes caulk as
//      one — down the flat-colour arm, which DRAWS IT WITH g_qeglobals.d_white
//      instead of its own material.  d_white is Material_RegisterHandle
//      ("white_tools") (gfxwrapper.cpp:77).
//   3. main/materials/white_tools has refStateBits[0] = 0x08128965: srcBlend
//      SrcAlpha(5), dstBlend InvSrcAlpha(6), blendOp Add(0x100) — an ALPHA-BLENDED
//      material — and refStateBits[1] = 0x0000000c, i.e. GFXS1_DEPTHWRITE CLEAR.
//      main/statemaps/default.sm agrees and explains why:
//          depthWrite { mtlBlendOp == Disable: Enable;  default: Disable; }
//   4. main/materials/caulk has refStateBits[0] = 0x08128812 (blendOp Disable —
//      opaque) and refStateBits[1] = 0x0000000d, i.e. GFXS1_DEPTHWRITE SET.
// So substituting white_tools for caulk silently turns a SOLID, depth-writing
// world surface into a blended one that leaves the depth buffer untouched.  It
// still looks opaque (the vertex colour is 0xFFFFFFFF), but nothing behind it is
// ever occluded — so a box further away renders straight through it.  That is
// exactly the report, and it appears "after cutting" because a cut is how an
// ordinary brush acquires caulk faces in the first place.
//
// The predicate below is the general form of "would the substitution cost this
// face its depth write".  It reads the SAME quantity the backend does —
// stateBitsTable[stateBitsEntry[techType]] is what RB_SetTessTechnique ->
// RB_BeginSurface -> Material_GetTechnique bottoms out on — using the identical
// access pattern rb_backend.cpp:1291-1295 already uses for its line probe.
static bool Cam_MaterialWritesDepth( Material *handle, MaterialTechniqueType tech )
{
    if ( !handle )
        return false;
    const Material *m = Material_FromHandle( handle );   // r_material.cpp:866 (identity + asserts)
    if ( !m || !m->stateBitsTable )
        return false;
    if ( (int)tech < 0 || (int)tech >= 34 )              // stateBitsEntry[34] (r_material.h:463)
        return false;
    const uint8_t e = m->stateBitsEntry[tech];
    if ( e == 0xFF )                                     // technique absent for this material
        return false;
    return ( m->stateBitsTable[e].loadBits[1] & GFXS1_DEPTHWRITE ) != 0;
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND O) — THE MISSING-MATERIAL CHECKERBOARD DRAWS WITH NO DEPTH
//                      AT ALL, AND THAT IS BOTH REPORTED BUGS.
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORTS, verbatim:
//   (1) "texture drawing is still buggy. (z order).  even without operations, I
//       assume this is because it was copied."   [checkerboard boxes drawing
//       through each other]
//   (2) "grid drawing still looks bad.  It needs to not draw over my shapes."
//
// THEY ARE ONE BUG.  Read out of the shipped assets, byte by byte:
//
//   1. A brush face names a material; Texture_GetHandle -> Register_WorldMaterial
//      (texwnd.cpp:239) registers "wc/<name>".  Material_Load strips the "wc/"
//      prefix (r_material_load_obj.cpp:5988-5993, g_materialTypeInfo[4]) and opens
//      "materials/<name>".  When that file does not exist, Material_Register_LoadObj
//      falls to Material_MakeDefault (r_material.cpp:568-569), which returns
//      Material_Duplicate( rgp.defaultMaterial, name ) — a per-name CLONE of
//      $default that SHARES its techniqueSet pointer (Material_Duplicate deep-copies
//      only the texture/constant/stateBits tables, r_material_load_obj.cpp:4670-4688;
//      techniqueSet comes across in the flat 0x50-byte struct copy at :4671).
//      That clone's colorMap is main/images/default.iwi — 16x16, the checkerboard
//      in the screenshots.
//
//   2. main/materials/$default, header bytes:
//         +0x34 techSetNameOffset  -> "2d"
//         +0x28 refStateBits[0]    =  0x08124812  (blendOp Disable -> OPAQUE,
//                                                  cull NONE, colorWrite RGB)
//         +0x2C refStateBits[1]    =  0x00000002  = GFXS1_DEPTHTEST_DISABLE,
//                                                  GFXS1_DEPTHWRITE *CLEAR*
//      (Compare main/materials/$default3d, byte-for-byte the same material with
//       techSet "default" and refStateBits[1] = 0x0000000d = DEPTHWRITE SET +
//       DEPTHTEST LESSEQUAL.  Same refImage "default", same refStateBits[0].)
//
//   3. main/techsets/2d.techset carries exactly ONE technique — "unlit":
//      vertcol_simple2d — and main/techniques/vertcol_simple2d.tech declares
//      stateMap "default2d".  main/statemaps/default2d.sm is unconditional:
//          depthTest  { default: Disable; }
//          depthWrite { default: Disable; }
//      Material_BuildStateBitsTable (r_material_load_obj.cpp:5748) runs every
//      refStateBit through that state map, so the per-technique loadBits the backend
//      binds (RB_SetTessTechnique <- RB_DrawTriangles_Internal, rb_backend.cpp:1423)
//      have NO depth test and NO depth write.
//
// PIXEL-LEVEL CONSEQUENCE.  A face whose material is missing neither occludes nor
// is occluded.  Every such face paints over whatever colour is already in the
// target, in SUBMISSION ORDER, and leaves the depth buffer exactly as it found it:
//   * two checkerboard boxes render "through" each other, the later one winning
//     wholesale — and a COPY is appended to active_brushes after its original, so
//     the copy always wins.  That is report (1), including why it needs no
//     "operation" to appear.
//   * the world writes no depth at all, so KiwiGrid_Draw — which round O left AFTER
//     all world/entity geometry and BEFORE the selected-outline depth clear — found
//     an empty depth buffer.  $line is depthTest LESSEQUAL / depthWrite ON
//     (main/materials/$line refStateBits[1] = 0x0d, techSet "tools" ->
//     vertcol_shaded_tools -> statemap "default", whose depthWrite rule
//     "mtlBlendOp == Disable: Enable" fires on $line's blendOp Disable), so every
//     grid segment passed and painted over the boxes.  That is report (2).
//
// ROUND S AMENDS THE LAST SENTENCE.  Round O concluded "the grid is innocent and its
// hook order is right".  The first half stands; the second does not.  The fix below
// only reaches faces whose material resolves to a $default CLONE and only when the
// asset set ships $default3d — so the grid's visibility was still a function of the
// world's material state.  ROUND S moves KiwiGrid_Draw to run BEFORE the world, where
// painter's order makes it independent of that state.  The full argument, including
// why the depth WRITE is still harmless and why $line_nodepth is not the answer, is
// on the hook itself (search "ROUND S" in CamWnd_Draw).
//
// ROUND M IS NOT AT FAULT AND IS NOT CHANGED.  Its diagnosis (caulk/nodraw were
// losing their depth write to the d_white substitution) and its fix both stand;
// they simply only covered the TOOL-material arm.  This is the WORLD arm, and a
// different mechanism: the material never loaded in the first place.
//
// THE FIX is the smallest one the shipped asset set allows: draw those faces with
// $default3d — the same checkerboard image, the same blend state, the 3D techset —
// instead of $default.  No render-state override exists on the editor command path
// (state comes only from material x technique), so the substitution IS the fix.
//
// DETECTION is by techniqueSet IDENTITY, not by Material_IsDefault: IsDefault
// (r_material.cpp:481) compares textureTable POINTERS, and Material_Duplicate
// allocates a fresh textureTable for every clone, so it answers false for exactly
// the materials we need to catch.  The techniqueSet pointer, by contrast, is shared
// by construction.  A world material that really loaded can never collide with it:
// it registers through the "wc/" prefix and so resolves techset "wc_2d", a
// different MaterialTechniqueSet object than the built-in $default's "2d".
// The depth-write test is ANDed in as a second gate so that an asset set whose
// $default does write depth leaves this code a no-op — and, more importantly, so
// that a legitimately alpha-blended world material (which correctly does not write
// depth) can never be dragged in by a future edit to the first test.
//
// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND X, ITEM 6) — THE POINTER GATE WAS THE BUG, NOT THE FIX.
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "The Z ordering of surfaces after doing an extrusion is
// still wrong. Need to fix this. This is a really big bug." + "The Z order is only
// messed up when looking from 1 direction. (see 2 pics)"
//
// Direction-dependent occlusion is the signature of a face drawn with NO DEPTH
// WRITE — painter's order decides, so one camera side happens to agree with depth
// and the other does not.  Round O found that mechanism and fixed HALF of it.
//
// The paragraph directly above is factually correct and is exactly what breaks:
// "a world material that really loaded ... resolves techset wc_2d, a DIFFERENT
// MaterialTechniqueSet object".  That sentence was written as a safety argument.
// It is also a description of the hole, because the editor's own current-material
// template is `$default` — Radiant_SeedCurrentTexdefs (mainfrm.cpp:701) seeds
// `SetMaterial("$default", &rts[0].mtl)` at boot, and nothing changes it until the
// user clicks a thumbnail.  That name registers through Register_WorldMaterial
// (texwnd.cpp:239-242) as "wc/$default", and materials/$default EXISTS, so this is
// a SUCCESSFUL LOAD, not a Material_MakeDefault clone.  Material_Load then builds
// the techset name with the type prefix — Com_sprintf(techniqueSetName, "%s%s",
// techniqueSetVertDeclPrefix, ...) at r_material_load_obj.cpp:5514, prefix "wc_"
// from g_materialTypeInfo[4] (r_material_load_obj.cpp:5266-5273) — so the techset
// is "wc_2d" while rgp.defaultMaterial's (registered unprefixed by name "$default",
// r_material.cpp:219) is "2d".  Two different interned objects, the pointer test
// returns false on the line above the depth test, and the depth test NEVER RUNS.
//
// The face keeps $default's real state: techset 2d -> vertcol_simple2d -> statemap
// default2d, depthTest Disable + depthWrite Disable (the byte-level decode is in
// the block above).  So EVERY face carrying the editor's default material draws
// with no depth at all, and the newest brush wins in submission order.  Extrusion
// makes it obvious because BuildPieceDef stamps the template onto a fresh brush and
// LandDef tail-inserts it (kiwi_extrude.cpp:275-338, brush.cpp:7664 memcpy from
// random_texture_stuff[0].mtl, Brush_AddToList2 brush.cpp:921-927) — but it is not
// an extrusion bug and the user's own "even without operations" report from round O
// was the same defect seen from the other end.
//
// THE FIX: compare the techset by NAME with the material-type prefix stripped, so
// "wc_2d" and "2d" are recognised as the same techset — which they are; the prefix
// only selects a vertex-declaration variant.  The pointer test is kept as the fast
// path (it is still exactly right for the clone case round O was built for), and
// the depth-write gate is UNCHANGED and still ANDed in, so the widened first test
// cannot drag in a material that legitimately writes depth.  What it now also
// catches is the one thing round O's comment promised could never happen.
//
// NOT DONE, deliberately: reseeding Radiant_SeedCurrentTexdefs with "$default3d"
// (what the engine's own BSP loader does, r_material_load_obj.cpp:4704).  That
// would change what gets WRITTEN INTO .map files, and map serialization is on the
// IDA-faithful side of the line.  Fixing the draw gate fixes every existing map
// too, which reseeding would not.
//
// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND Y, ITEM 1) — THE FACE MATERIAL WAS NEVER `$default`.
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "z-order bug is still here.  All I did was split a cube
// in half.  All extrusions get this on top drawing bug too. (Maybe material
// related??)" — after building round X's widened gate.
//
// ROUND X'S PREMISE WAS FALSE.  Its comment says the template "is `$default` …
// and nothing changes it until the user clicks a thumbnail".  THE BOOT CHANGES
// IT.  Radiant_ApplyStartupTextureScale (mainfrm.cpp:763-773, the OnCreate tail)
// runs Radiant_CheckTextureScale, whose tail is Texture_ResetPosition
// (mainfrm.cpp:2914).  Texture_ResetPosition ends in
// TexWnd_ApplyMaterialAtIndex( TexWnd_HitTest( 9, 9 ) ) (texwnd.cpp:2068-2073) —
// i.e. it makes THE FIRST VISIBLE THUMBNAIL the current brush texture, and says
// so in its own header comment (texwnd.cpp:2046-2048).  The browser lists
// materials alphabetically, so in this asset set the editor's template at boot is
// the first material in main/materials: **aa_default**.
//
// THE ASSET DECODE (main/materials/aa_default, MaterialRaw, r_material.h:589):
//     +0x00 nameOffset        -> "aa_default"
//     +0x09 sortKey           =  0x2B (43)   — the TRANSLUCENT/tool sort class,
//                                              byte-identical to clip / trigger /
//                                              origin / white_tools
//     +0x28 refStateBits[0]   =  0x08128965  — srcBlend SrcAlpha, dstBlend
//                                              InvSrcAlpha, blendOp ADD, cull BACK
//     +0x2C refStateBits[1]   =  0x0000000C  — GFXS1_DEPTHTEST_LESSEQUAL,
//                                              GFXS1_DEPTHWRITE **CLEAR**
//     +0x34 techSetNameOffset -> "tools"     — NOT "2d"
// main/techsets/tools.techset maps "unlit" -> vertcol_shaded_tools, whose
// main/techniques/vertcol_shaded_tools.tech declares stateMap "default", and
// main/statemaps/default.sm reads
//     depthWrite { mtlBlendOp == Disable: Enable;  default: Disable; }
// aa_default's blendOp is ADD, so the bound state is depth TEST on, depth WRITE
// OFF.  That is the exact direction-dependent signature: the brush drawn FIRST
// stamps no depth, so the brush drawn SECOND passes the test wherever they
// overlap and wins — which looks correct from the side where the newer brush is
// genuinely in front, and wrong from the other.  A split makes two brushes and an
// extrusion tail-inserts one, which is why both reproduce it every time.
//
// Round X's gate compared the techset BASE NAME against rgp.defaultMaterial's
// ("2d").  "tools" != "2d", so aa_default sailed straight through it.
//
// THE PREDICATE MOVES ONTO THE RIGHT AXIS.  Enumerating techset names one report
// at a time is the losing game the last three rounds played, so the gate is now:
//
//     the material's techset base name is an EDITOR/HUD techset ("2d" or
//     "tools")  AND  the technique the camera binds does not write depth.
//
// Both halves are checked in the shipped assets, not assumed.  A census of all
// 4355 materials in main/materials shows techsets "2d" (862) and "tools" (98) are
// used by HUD art and editor tool materials ONLY; every world surface class lives
// on l_sm_* / unlit* / effect* / sky / water / ambient_* / distortion_* techsets.
// So the predicate cannot reach a legitimately-translucent WORLD material (glass,
// foliage, water) — those keep their own material and their own blend state.
//
// WHY NOT THE SORT KEY.  "Opaque sort + no depth write" was the obvious general
// axis and it does NOT hold: aa_default declares sortKey 43, i.e. it calls itself
// translucent.  So does clip, so does trigger.  Sorting is a statement about the
// GAME's draw order, and the editor camera has no sorted pass at all — it submits
// brush faces in list order into one immediate stream.  What actually breaks the
// editor is a face that does not write depth, and what makes that safe to fix is
// knowing the material was never meant to be a world surface, which is what the
// techset says.
//
// d_white IS EXCLUDED BY NAME OF POINTER.  white_tools is itself a "tools"
// techset material with no depth write, and it is the editor's DELIBERATE
// flat-colour handle for the see-through tool volumes (round M's arm, below in
// Cam_Draw).  Substituting a checkerboard for it would turn every clip/trigger
// volume into an opaque box.  One pointer test, at the top.
//
// NOT DONE, still: reseeding what gets written into .map files (D-X4 stands).
// The boot-time template clobber is fixed SEPARATELY and at the source —
// Radiant_ApplyStartupTextureScale now restores the seeded `$default`
// (mainfrm.cpp) — so new geometry stops acquiring a tool material at all, while
// this gate keeps every map that already has one drawing correctly.

// The techset name with the vertex-declaration prefix removed.  The prefix table is
// g_materialTypeInfo (r_material_load_obj.cpp:5266-5273): "", "m_", "mc_", "w_",
// "wc_".  Tested longest-first so "mc_" is never eaten as "m_" + "c_...".
static const char *Cam_TechSetBaseName( const MaterialTechniqueSet *ts )
{
    if ( !ts || !ts->name )
        return nullptr;
    static const char *const kPrefix[] = { "mc_", "wc_", "m_", "w_" };
    for ( int i = 0; i < (int)( sizeof( kPrefix ) / sizeof( kPrefix[0] ) ); ++i )
    {
        const size_t n = strlen( kPrefix[i] );
        if ( strncmp( ts->name, kPrefix[i], n ) == 0 )
            return ts->name + n;
    }
    return ts->name;
}

// KIWI-UX (ROUND Y, ITEM 1): the EDITOR/HUD techset set.  Census of the shipped
// main/materials (4355 files): "2d" and "tools" carry HUD art and editor tool
// materials exclusively — no world surface class uses either.  $default lives on
// "2d"; aa_default, white_tools, clip, trigger, origin, caulk, nodraw and $line
// all live on "tools".  (Two of those — caulk and nodraw — DO write depth and are
// therefore never substituted; the depth gate is what separates them.)
// ── KIWI-UX (ROUND Y, ITEM 1): THE OPAQUE SORT CLASS, MEASURED ──────────────
// The obvious reading of "opaque sorts before decal" is sortKey < SORTKEY_DECAL
// (24, r_bsp_load_obj.cpp:2065).  THAT IS WRONG AND IT WAS CHECKED: a census of
// main/materials puts the shipped WORLD DECALS at sortKeys 9..12 — 434 of them,
// alpha-blended and correctly depth-write-free (ch_decal_mural, sk 12;
// ch_cliff02_decal, sk 9; every *_dec / *_decal in the set) — so a `< 24` gate
// would substitute a checkerboard for every decal in the game.
//
// The real opaque world sort is the single value 4.  The engine says so:
// Material_FinishLoadingTexdef special-cases exactly
// `material->info.sortKey == 4 && R_IsWorldMaterialType(materialType)`
// (r_material_load_obj.cpp:5458), and the census agrees — 2131 of the non-editor
// materials sit on sortKey 4 and they are the ordinary opaque world surfaces.
// Below it are only sortKey 0 and 3 (52 FX/distortion/sky materials).
//
// Arm B therefore catches exactly 13 non-editor materials in this asset set
// (particle_cloud, distortion_scale_zfeather, glow/grain overlays) — FX materials
// that declare themselves opaque and blend anyway, none of which is a brush-face
// material anyone applies from the texture browser.  That is an acceptable and
// fully enumerated blast radius for a rule that generalises the fix beyond the
// techset list.
static const int CAM_SORTKEY_OPAQUE = 4;

static bool Cam_TechSetIsEditorClass( const MaterialTechniqueSet *ts )
{
    const char *base = Cam_TechSetBaseName( ts );
    if ( !base )
        return false;
    return strcmp( base, "2d" ) == 0 || strcmp( base, "tools" ) == 0;
}

// "Would drawing a WORLD BRUSH FACE with this material break depth ordering?"
// (round O named it Cam_MaterialIsMissing when a missing material was the only
// known cause; round Y's block above is why the name is now narrower than the
// predicate).  Kept as one function because every caller — the world fill, the
// surf cache, the substitute's own validation — asks exactly this question.
static bool Cam_MaterialIsMissing( Material *handle )
{
    if ( !handle )
        return false;
    // KIWI-UX (ROUND Y, ITEM 1): the editor's OWN flat-colour handle is never
    // substituted.  white_tools is a "tools" material with no depth write, and
    // round M's tool arm picks it deliberately for the see-through volumes.
    if ( handle == g_qeglobals.d_white )
        return false;
    const Material *m = Material_FromHandle( handle );
    if ( !m )
        return false;
    // The FAST PATH round O built, still exactly right for a Material_MakeDefault
    // clone (which shares rgp.defaultMaterial's techniqueSet POINTER).
    const bool sameAsDefault = rgp.defaultMaterial
                            && m->techniqueSet == rgp.defaultMaterial->techniqueSet;
    // ARM A — an EDITOR/HUD techset.  Catches aa_default, $default and every
    // other tool/browser material, and provably cannot reach a world surface.
    // ARM B — the material DECLARES ITSELF FULLY OPAQUE.  A world material that
    // says "opaque" and then does not write depth is broken by its own
    // declaration, whatever techset it is on, so substituting is right on its own
    // terms.  See CAM_SORTKEY_OPAQUE for why the threshold is 4 and not 24.
    if ( !sameAsDefault
      && !Cam_TechSetIsEditorClass( m->techniqueSet )
      && m->info.sortKey > CAM_SORTKEY_OPAQUE )
        return false;
    return !Cam_MaterialWritesDepth( handle, TECHNIQUE_UNLIT );
}

// KIWI-UX: "this material is made to sit ON something" - the texture browser's decal
// badge.  The same two facts the substitution above reads, the other way round: a world
// material (not a $default clone, not an editor techset) that sorts after the opaque
// class and does not write depth.  Shipped world decals sit at sortKeys 9..12 and
// unlit_multiply overlays (ch_brick_wall_03_burnt) have no depth write either; used as a
// brush face's main material, whatever is behind it shows through.  Skies write depth
// (refStateBits[1] 0x0D, see the sky note in Cam_Draw) and alpha-TESTED materials sort
// opaque, so neither is badged.
bool Cam_MaterialIsOverlay( Material *handle )
{
    if ( !handle || handle == g_qeglobals.d_white )
        return false;
    const Material *m = Material_FromHandle( handle );
    if ( !m || !m->techniqueSet )
        return false;
    if ( rgp.defaultMaterial && m->techniqueSet == rgp.defaultMaterial->techniqueSet )
        return false;
    if ( Cam_TechSetIsEditorClass( m->techniqueSet ) || m->info.sortKey <= CAM_SORTKEY_OPAQUE )
        return false;
    return !Cam_MaterialWritesDepth( handle, TECHNIQUE_UNLIT );
}

// The 3D twin of $default, registered once through the same "wc/" world-material
// path every brush face uses (Material_Load strips the prefix and picks the
// "wc_default" techset, which maps "unlit" to the same textured_simple technique
// the unprefixed "default" techset does).  Returns null — leaving the caller to
// draw the broken material unchanged — if $default3d is itself absent, which the
// same missing-material test detects.
// ── KIWI-UX (ROUND Y, ITEM 1): A FALLBACK LADDER, NOT ONE NAME ──────────────
// Round X's note warned that "$default3d must exist in the asset set or nothing
// improves", and left the no-substitute case silently drawing the broken
// material.  There is no render-state override on this path — state comes only
// from material x technique (round O), and every technique in the "tools" techset
// routes through the same statemap, so forcing the depth bit is not available
// either.  What IS available is a second shipped material that is opaque and
// depth-writing.  The ladder, in order:
//   1. "$default3d" — the checkerboard twin of $default: same refImage "default",
//      same refStateBits[0], refStateBits[1] = 0x0D (DEPTHWRITE | LESSEQUAL),
//      techset "default".  Visually nearest to what the face already wore.
//   2. "caulk"      — refStateBits[0] = 0x08128812 (blendOp Disable -> opaque),
//      refStateBits[1] = 0x0D, techset "tools".  Ships in every CoD4 asset set by
//      construction (the compiler requires it), so this rung is the guarantee that
//      a world face ALWAYS ends up depth-writing.
// Both are validated with the SAME predicate, so a rung that is itself broken is
// skipped rather than trusted.
static Material *Cam_MissingMaterialSubstitute()
{
    static bool      s_tried = false;
    static Material *s_mtl   = nullptr;
    if ( !s_tried )
    {
        s_tried = true;
        static const char *const kLadder[] = { "wc/$default3d", "wc/caulk" };
        for ( int i = 0; i < (int)( sizeof( kLadder ) / sizeof( kLadder[0] ) ); ++i )
        {
            Material *cand = Material_RegisterHandle( kLadder[i], 0 );
            if ( cand && !Cam_MaterialIsMissing( cand ) )
            {
                s_mtl = cand;
                if ( i != 0 )
                    Sys_Printf( "WARNING: no usable \"$default3d\" material - depth-less faces "
                                "will draw as \"%s\" instead.\n", kLadder[i] );
                break;
            }
        }
        if ( !s_mtl )
            Sys_Printf( "WARNING: no depth-writing substitute material found - faces whose "
                        "material does not write depth will draw in submission order "
                        "(z-order will be wrong).  Run \"KiwiMatInfo\" from the command "
                        "palette on such a face for the decode.\n" );
    }
    return s_mtl;
}

// KIWI (2026-09-24, user: "special textures like mantle, ladder, etc don't draw like they
// should ... they draw as the default texture, but should be a flat color with text"):
// the substitution above is right for a material with no art of its own, but ~25 behaviour
// tool materials (mantle_on/over, ladder, kill, traverse, stopspawn, auto_adjust, ...) are
// "tools"-techset, alpha-blended (refStateBits {0x08128965, 0x0C}: SrcAlpha/InvSrcAlpha, no
// depth write) AND carry their own label art (DXT3, flat colour at alpha 85, label at 255).
// They are not in Cam_EditorMaterialColor's flat-colour table, so they reached the world arm
// and were swapped for the $default3d checkerboard.
// Their TWIN draws the material's own art with the depth write fixed: a Material_Duplicate
// (private stateBits table, shared techset/images) whose fill entries blend SrcAlpha/Zero -
// the art composited over black, i.e. the dark flat colour with the bright label, exactly
// the texture-browser tile - with depth write + LESSEQUAL.  Nothing behind shows through, so
// submission order cannot matter.  Wireframe entries are left alone.  "2d" materials
// ($default, HUD art) and $default clones keep the checkerboard.
static Material *Cam_ToolArtTwin( Material *handle )
{
    const Material *m = Material_FromHandle( handle );
    if ( !m || !m->stateBitsTable || !m->textureTable )
        return nullptr;
    if ( rgp.defaultMaterial && m->techniqueSet == rgp.defaultMaterial->techniqueSet )
        return nullptr;                                  // a Material_MakeDefault clone: no art
    const char *base = Cam_TechSetBaseName( m->techniqueSet );
    if ( !base || strcmp( base, "tools" ) != 0 )
        return nullptr;
    bool haveArt = false;
    for ( int i = 0; i < (int)m->textureCount && !haveArt; ++i )
        haveArt = m->textureTable[i].semantic == 2 && m->textureTable[i].u.image;   // TS_COLOR_MAP
    if ( !haveArt )
        return nullptr;

    // Handles live as long as the editor, so a small pointer table is the whole cache.
    static struct { Material *src, *twin; } s_twins[256];
    static int s_twinCount = 0;
    for ( int i = 0; i < s_twinCount; ++i )
        if ( s_twins[i].src == handle )
            return s_twins[i].twin;
    if ( s_twinCount >= (int)( sizeof( s_twins ) / sizeof( s_twins[0] ) ) )
        return nullptr;

    // The pointer in the name keeps it unique: Material_Duplicate on an EXISTING name
    // memcpy's the source over it, which would share (and let us patch) its state table.
    char name[160];
    _snprintf( name, sizeof( name ), "kiwi_toolart_%p_%s", (void *)handle,
               m->info.name ? m->info.name : "" );
    name[sizeof( name ) - 1] = '\0';
    Material *twin = Material_Duplicate( handle, name );
    if ( !twin || twin == handle || !twin->stateBitsTable || twin->stateBitsTable == m->stateBitsTable )
        twin = nullptr;
    else
    {
        const int wireA = twin->stateBitsEntry[TECHNIQUE_WIREFRAME_SOLID];
        const int wireB = twin->stateBitsEntry[TECHNIQUE_WIREFRAME_SHADED];
        for ( int e = 0; e < (int)twin->stateBitsCount; ++e )
        {
            if ( e == wireA || e == wireB )
                continue;
            unsigned int &b0 = twin->stateBitsTable[e].loadBits[0];
            unsigned int &b1 = twin->stateBitsTable[e].loadBits[1];
            if ( b0 & GFXS0_POLYMODE_LINE )
                continue;
            // RGB: src SRCALPHA (5), dst ZERO (1), op ADD (1).  No alpha test.
            b0 = ( b0 & ~(unsigned int)( GFXS0_BLEND_RGB_MASK | GFXS0_ATEST_MASK ) ) | GFXS0_ATEST_DISABLE
               | ( 5u << GFXS0_SRCBLEND_RGB_SHIFT ) | ( 1u << GFXS0_DSTBLEND_RGB_SHIFT )
               | ( 1u << GFXS0_BLENDOP_RGB_SHIFT );
            b1 = ( b1 & ~(unsigned int)( GFXS1_DEPTHTEST_DISABLE | GFXS1_DEPTHTEST_MASK ) )
               | GFXS1_DEPTHWRITE | GFXS1_DEPTHTEST_LESSEQUAL;
        }
    }
    s_twins[s_twinCount].src  = handle;
    s_twins[s_twinCount].twin = twin;                    // null is memoised too
    ++s_twinCount;
    return twin;
}

// The one-liner the face emitters call.  ACCEPTED COST, stated rather than hidden:
// textured_simple has no vertex.color and no MATERIAL_COLOR term, so a missing-
// material face no longer takes the selected-brush red tint (it did not take a
// useful one before either — vertcol_simple2d colours from the vertex stream, and
// the tint pass drives MATERIAL_COLOR).  Selection stays legible through the white
// wireframe outline pass and the KIWI selection accents.
static Material *Cam_DrawMaterial( Material *handle )
{
    if ( !Cam_MaterialIsMissing( handle ) )
        return handle;
    if ( Material *twin = Cam_ToolArtTwin( handle ) )    // KIWI: tool art keeps its picture
        return twin;
    Material *sub = Cam_MissingMaterialSubstitute();
    return sub ? sub : handle;
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND Y, ITEM 1) — THE PERMANENT DIAGNOSTIC
// ═════════════════════════════════════════════════════════════════════════════
// Three rounds have now guessed at what state a face is actually drawn with, and
// two of those guesses were wrong.  This is the ground truth, read out of the
// SAME fields the backend binds (Cam_MaterialWritesDepth documents the access
// pattern) and printed by "KiwiMatInfo" (kiwi_material.cpp).  It lives here
// because the predicates it reports are this file's and must not be duplicated.
bool KiwiMtl_Diagnose( Material *handle, kiwiMatDiag_t *out )
{
    if ( !out )
        return false;
    memset( out, 0, sizeof( *out ) );
    if ( !handle )
        return false;
    const Material *m = Material_FromHandle( handle );   // r_material.cpp:866
    if ( !m )
        return false;

    out->name        = m->info.name;
    out->techSet     = m->techniqueSet ? m->techniqueSet->name : nullptr;
    out->techSetBase = Cam_TechSetBaseName( m->techniqueSet );
    out->sortKey     = (int)m->info.sortKey;             // r_material.h:438

    // The technique the CAMERA binds for this face, arrived at exactly as the
    // world draw does: draw_mode -> Cam_TechForDrawMode, then demoted to UNLIT
    // when the loaded techset does not carry it (Cam_TechAvailable).
    MaterialTechniqueType tech = Cam_TechForDrawMode( Ed_Camera()->draw_mode );
    if ( tech != TECHNIQUE_UNLIT
      && ( !m->techniqueSet || !m->techniqueSet->techniques[tech] ) )
        tech = TECHNIQUE_UNLIT;
    out->cameraTech = (int)tech;

    if ( m->stateBitsTable && (int)tech >= 0 && (int)tech < 34 )
    {
        const uint8_t e = m->stateBitsEntry[tech];       // 0xFF = absent
        if ( e != 0xFF )
        {
            out->techPresent = true;
            const uint bits1 = m->stateBitsTable[e].loadBits[1];
            out->depthWrite  = ( bits1 & GFXS1_DEPTHWRITE ) != 0;
            out->depthTest   = ( bits1 & GFXS1_DEPTHTEST_DISABLE ) == 0;
            out->loadBits0   = m->stateBitsTable[e].loadBits[0];
            out->loadBits1   = bits1;
        }
    }

    out->substituted = Cam_MaterialIsMissing( handle );
    if ( out->substituted )
    {
        // Material_FromHandle asserts on a null handle (r_material.cpp:866), and the
        // ladder legitimately returns null when the asset set ships neither rung.
        Material       *subH = Cam_ToolArtTwin( handle );   // KIWI: the twin comes first
        if ( !subH )
            subH = Cam_MissingMaterialSubstitute();
        const Material *sub  = subH ? Material_FromHandle( subH ) : nullptr;
        out->substituteName = sub ? sub->info.name : "(none - z-order WILL be wrong)";
    }
    return true;
}

// Draw one CONVEX world-space polygon as a textured triangle fan with technique
// `tech`. bgra modulates it (0xFFFFFFFF = white = the world path); push displaces
// each vertex along `n` (selection overlay, to sit just in front of the coplanar
// world face).
//
// ══════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BK, ITEM 4) — THIS IS THE DONOR ROUTE, AND IT IS NOW SHARED.
// ══════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: *"You are STILL (after many tries) not rendering the
// light blue construction faces that can be extruded from (the ones that result
// from closed off construction lines).  Can you finally fix this?  It was kinda
// working the first time I asked."*
//
// Six rounds (AA, AK, AL, AM, AQ and this one) have chased the region fill's own
// submission.  Round AQ's relocation was the last difference the SUBMISSION-SIDE
// model had to give and it did not help, so this round stops iterating that route
// entirely and TRANSPLANTS the fill onto a translucent world-space polygon draw
// the user can demonstrably SEE: the ROUND-BC SKY FILM (:2916-2924 below), which
// is this very function called with a packed per-vertex RGBA at alpha 0.30.
//
// The body did not change.  It was split so that a caller with a bare point list
// — the region walker's loop — enters at EXACTLY the same place a face does, with
// the same planar 1/128 ST, the same per-vertex plane normal, the same fan and
// the same one R_AddRenderCmdDrawTris.  Cam_DrawFaceTinted is now three lines and
// its behaviour is byte-identical.
//
// TWO THINGS THE KIWI FILLS ALL DID DIFFERENTLY FROM THIS ONE, both of which the
// transplant therefore drops, and both of which are stated so the next round can
// read the experiment rather than re-run it:
//   * ST.  Every kiwi_lines-family fill writes st = (0,0) on every vertex; this
//     route writes a real planar projection.  Under a NEUTRAL material colour the
//     tools pixel shader is `sample(colorMap) * vertexColour`, so what a
//     degenerate ST samples is not something this tree can answer.
//   * THE NORMAL.  KiwiTris_FillNormal is a CONSTANT +Z (kiwi_lines.h TRAP 4);
//     this route writes the polygon's own plane normal, exactly as the binary's
//     own fill batcher does (brush.cpp Face_AddWindingToTriBatch, 0x47b86a) and
//     exactly as the one other fill confirmed visible does (kiwi_boolean.cpp
//     FillBrush).  EVERY fill in this editor that is invisible writes the
//     constant; both that are visible write a real plane normal.
void Cam_DrawWindingTinted( const float ( *pts )[3], int nv, const float *n,
                            Material *mtl, uint bgra, float push,
                            MaterialTechniqueType tech )
{
    if ( nv < 3 || nv > CAM_MAXFACEVERTS || !pts || !n )
        return;

    mtl = Cam_DrawMaterial( mtl );      // KIWI-UX (ROUND O) — see Cam_DrawMaterial

    float    xyzw[CAM_MAXFACEVERTS][4];
    float    normal[CAM_MAXFACEVERTS][3];
    float    st[CAM_MAXFACEVERTS][2];
    float    color[CAM_MAXFACEVERTS];
    uint16_t indices[3 * CAM_MAXFACEVERTS];

    // Planar texcoord: project onto the two axes least aligned with the face normal,
    // at a fixed 1/128-unit scale (the real texdef projection = follow-up).
    float an[3] = { fabsf( n[0] ), fabsf( n[1] ), fabsf( n[2] ) };
    int ai, bi;                                  // the two projection axes
    if ( an[0] >= an[1] && an[0] >= an[2] ) { ai = 1; bi = 2; }      // normal ~X
    else if ( an[1] >= an[2] )              { ai = 0; bi = 2; }      // normal ~Y
    else                                    { ai = 0; bi = 1; }      // normal ~Z
    const float texScale = 1.0f / 128.0f;

    for ( int i = 0; i < nv; ++i )
    {
        const float *p = pts[i];
        xyzw[i][0] = p[0] + n[0]*push; xyzw[i][1] = p[1] + n[1]*push;
        xyzw[i][2] = p[2] + n[2]*push; xyzw[i][3] = 1.0f;
        normal[i][0] = n[0]; normal[i][1] = n[1]; normal[i][2] = n[2];
        st[i][0] = p[ai] * texScale;
        st[i][1] = p[bi] * texScale;
        *(uint *)&color[i] = bgra;            // modulated by the technique
    }

    // Fan triangulation: (0, i, i+1).
    int ic = 0;
    for ( int i = 1; i < nv - 1; ++i )
    {
        indices[ic++] = 0;
        indices[ic++] = (uint16_t)i;
        indices[ic++] = (uint16_t)( i + 1 );
    }

    R_AddRenderCmdDrawTris( mtl, tech, (short)ic, indices, (short)nv,
                            xyzw, normal, color, st );
}

// Draw one face's winding — the original entry point, unchanged in behaviour.
static void Cam_DrawFaceTinted( const face_t *f, Material *mtl, uint bgra, float push,
                                MaterialTechniqueType tech )
{
    const winding_t *w = f->w;
    Cam_DrawWindingTinted( (const float (*)[3])w->p, w->numpoints,
                           f->plane.normal, mtl, bgra, push, tech );
}

static inline void Cam_DrawFace( const face_t *f, Material *mtl, MaterialTechniqueType tech )
{
    Cam_DrawFaceTinted( f, mtl, 0xFFFFFFFFu, 0.0f, tech );   // world path: white, no push
}

// ── editor surf-cache path ────────────────────────────────────────────────────
// The binary's Cam_Draw draws the world through the editor surface cache
// (Visuals_InitFaceVis 0x46F7A0 builds per-face GfxWorldVertex into the per-material D3D9 VB
// pool; DrawGeo emits them; R_AddEditorSurfsCmd flushes one RC_DRAW_EDITOR_SKINNEDCACHED).
// Here the DEFAULT world draw is the immediate path; RADIANT_SURFCACHE selects the cached
// one.  Both build geometry with brush.cpp Face_BuildLayerGeom, so they are bit-identical.
extern unsigned int Editor_VB_Upload( Material *material, int vertCount,
    const float *xyz, const float *tangent, const float *binormal,
    const float *normal, const float *texCoord, const float *color );
extern void  Editor_AddGeoFace( Material *handle, int techType, int sortKey,
                                int vertCount, int vbIndexAndOffs );
extern void *R_AddEditorSurfsCmd();
// How many surfs the OPEN flush window already holds.  Read either side of the entity +
// prefab pass to decide whether the window is EXACTLY the block the surf cache replayed.
extern int   Editor_PendingSurfCount();                          // r_ed_scene.cpp:441
// The surf sort bucket — 100 * info.drawSurf.fields.primarySortKey.
extern int __cdecl Editor_MaterialSortKey( Material *handle );   // r_ed_scene.cpp:190 (0x4FDBB0)

// RADIANT_SURFCACHE — operator switch: the binary's surf-cache world draw instead of immediate.
static bool Cam_SurfCacheEnabled()
{
    static const bool s = ( getenv( "RADIANT_SURFCACHE" ) != nullptr );
    return s;
}
// Render decorations (entity origin boxes, trigger-radius cylinders, script-colour tokens):
// display-only overlays, default-OFF behind RADIANT_DECOR.  NON-static — brush.cpp's DrawModels
// decoration gate shares it so the whole decoration layer flips together.
bool Radiant_DecorEnabled()
{
    static const bool s = ( getenv( "RADIANT_DECOR" ) != nullptr );
    return s;
}
// KIWI-UX: the binary gates ActiveSunLightPreviewInit on both preferences.  KIWI keeps that master
// gate but shares the persisted sun state with the View menu and lighting panels.
static bool Cam_SunPrevEnabled()
{
    return g_PrefsDlg->enable_light_preview && KiwiSunPreview_Enabled();
}

// Brush_MakeFaceVisuals (0x477C50) uploads each face's per-layer GfxWorldVertex run to the
// editor surf-cache VB pool on every faceVis rebuild.
// The headless gates run with NO D3D device, where that upload would FatalError — gate
// the GPU half on the device, mirroring the R_Ed_SetSceneParms device gate.  brush.cpp externs.
bool Radiant_FaceVisGpuReady()
{
    // KIWI-UX (ROUND AB, ITEM 1): also refuse while the device is LOST.  The upload chain
    // ends at Editor_CreateAdditionalVertexBuffer (r_ed_vertbuf.cpp:244), whose
    // CreateVertexBuffer failure is a FatalError — and CreateVertexBuffer on a lost device
    // fails by definition.  Since kiwi_devicereset.cpp arms a rebuild for EVERY brush at
    // reset time, the first post-loss draw would otherwise walk straight into it.  Refusing
    // leaves the identity faceVis (visCount = 0, vertcount = winding count) exactly as the
    // headless gates do; the version stays armed, so the rebuild happens on the first frame
    // after the device comes back.
    return dx.device != nullptr && !dx.deviceLost;
}

// KIWI-UX (ROUND AB, ITEM 1).  The two reasons Radiant_FaceVisGpuReady says no are NOT the
// same reason, and Brush_MakeFaceVisuals has to tell them apart:
//   * NO DEVICE  — a headless gate.  There will never be a device; the identity faceVis is
//     the final answer and the instance version must be synced, or every gate re-enters the
//     rebuild forever.  This is the faithful behaviour and it stays.
//   * DEVICE LOST — transient.  Syncing the version here would mark the brush "cached" while
//     its cache is empty, and nothing would ever rebuild it once the device came back: the
//     map would render permanently untextured after one monitor sleep.
// Only the second case leaves the rebuild armed.
bool Radiant_FaceVisDeviceLost()
{
    return dx.device != nullptr && dx.deviceLost;
}

// 0x406760  R_SunPrev_SetSunConstants — parse the worldspawn sun keys and push the sun
// direction/colour into CONST_SRC_CODE_SUN_POSITION/DIFFUSE/SPECULAR.  The binary round-trips
// the worldspawn through a CMemFile (Entity_WriteSelected); we build the same epair text from
// world_entity->epairs and feed the SAME parser pair, so the GfxLight is bit-identical.
// Returns 1 (+ the directional sun dir, w=0) when "sundirection" is present, else 0.
// ambientMulOut: the black-world multiply colour (0x50C470) the full-screen quad resets the
// textured world to before the SUNLIGHT_PREVIEW pass adds the sun light.
// `emit` = issue the 3 RC_SET_CUSTOM_CONSTANT commands (false = pre-compute values only).
// Called only when the edit-epoch cache rebuilds; Cam_SunPrev_Main emits the same three
// constants from the cached values.  This is the expensive half of the sun setup (an 8 KB
// epair round trip plus two parsers) and camera motion never re-runs it.
static int Cam_SunPrev_SetSunConstants( float sunDirOut[3], float ambientMulOut[3] = nullptr,
                                        float sunColorOut[3] = nullptr, bool emit = true )
{
    if ( !world_entity )
        return 0;
    entity_s_def *wd = (entity_s_def *)world_entity->def;
    if ( !wd )
        return 0;
    // Gate on the sun key, like the binary's HasKeyValuePair(def,"sundirection").
    bool hasSun = false;
    for ( epair_t *e = wd->epairs; e; e = e->next )
        if ( e->key && !_stricmp( e->key, "sundirection" ) ) { hasSun = true; break; }
    if ( !hasSun )
        return 0;

    // Worldspawn epair text as R_ParseSunLight expects it: "{\n" + "\"k\" \"v\"\n"* + "}\n".
    // 8192 matches the binary's 0x2000 read clamp.  _snprintf returns -1 on MSVC when truncated,
    // so Append clamps n rather than letting it go negative.
    char text[8192];
    int n = 0;
    const int CAP = (int)sizeof(text) - 4;
    auto Append = [&]( const char *fmt, const char *a, const char *b ) {
        if ( n >= CAP ) return;
        int w = a ? _snprintf( text + n, CAP - n, fmt, a, b ) : _snprintf( text + n, CAP - n, "%s", fmt );
        if ( w < 0 || w > CAP - n ) n = CAP; else n += w;
    };
    Append( "{\n", nullptr, nullptr );
    for ( epair_t *e = wd->epairs; e && n < CAP - 8; e = e->next )
        if ( e->key && e->value )
            Append( "\"%s\" \"%s\"\n", e->key, e->value );
    Append( "}\n", nullptr, nullptr );
    text[sizeof(text) - 1] = 0;

    SunLightParseParams params;
    memset( &params, 0, sizeof(params) );
    GfxLight light;
    memset( &light, 0, sizeof(light) );
    char *p = text;
    R_ParseSunLight( &params, p );
    R_InterpretSunLightParseParamsIntoLights( &params, &light );

    // The 3 sun code constants sunpre_*.hlsl reads (binary order/values).
    if ( emit )
    {
        R_AddCmdSetCustomShaderConstant( CONST_SRC_CODE_SUN_POSITION, light.dir[0],   light.dir[1],   light.dir[2],   0.0f );
        R_AddCmdSetCustomShaderConstant( CONST_SRC_CODE_SUN_DIFFUSE,  light.color[0], light.color[1], light.color[2], 1.0f );
        R_AddCmdSetCustomShaderConstant( CONST_SRC_CODE_SUN_SPECULAR, light.color[0], light.color[1], light.color[2], 1.0f );
    }

    sunDirOut[0] = light.dir[0];
    sunDirOut[1] = light.dir[1];
    sunDirOut[2] = light.dir[2];
    if ( sunColorOut )
    {
        sunColorOut[0] = light.color[0];
        sunColorOut[1] = light.color[1];
        sunColorOut[2] = light.color[2];
    }

    // 0x50C470  R_SunPrev_ComputeBlackWorldMultiplyColor — the colour the full-screen multiply
    // quad drops the textured world to before the SUNLIGHT_PREVIEW pass adds the sun light.
    // Both branch tests are EQUALITY (== 0), NOT a > test:
    //   if ( ambientScale == 0 ) amb = {0,0,0}
    //   ColorNormalize(ambientColor, ambientColor)  [in place, RETURNS the max]
    //   if ( returned max == 0 ) amb = {0,0,0}   <- the RETURN value, not the output
    //     (ColorNormalize writes {1,1,1} to `out` when max==0, so testing `out` here
    //      wrongly picks the scale branch for a worldspawn with no "_color")
    //   k = (sunLight - ambientScale) * diffuseFraction
    //   out[i] = diffuseColor[i]*k + amb[i]
    if ( ambientMulOut )
    {
        float amb[3] = { 0.0f, 0.0f, 0.0f };
        if ( params.ambientScale != 0.0f )
        {
            const float maxComp = ColorNormalize( params.ambientColor, params.ambientColor );
            if ( maxComp != 0.0f )
            {
                amb[0] = params.ambientScale * params.ambientColor[0];
                amb[1] = params.ambientScale * params.ambientColor[1];
                amb[2] = params.ambientScale * params.ambientColor[2];
            }
        }
        const float sunFloor = ( params.sunLight - params.ambientScale ) * params.diffuseFraction;
        ambientMulOut[0] = params.diffuseColor[0] * sunFloor + amb[0];
        ambientMulOut[1] = params.diffuseColor[1] * sunFloor + amb[1];
        ambientMulOut[2] = params.diffuseColor[2] * sunFloor + amb[2];
    }
    return 1;
}

// KIWI-UX: parsing and interpreting worldspawn sunlight is edit-dependent, not
// camera-dependent.  SetKeyValue/MarkMapModified already bump the shared walk/shadow
// epoch, so cache the exact IDB-derived constants on that epoch.  An unedited map incurs
// no per-frame epair serialization, parser work, or SunPrev_Setup re-derivation.
struct CamSunPreviewCache
{
    entity_s *world;
    unsigned epoch;
    bool initialized;
    bool active;
    bool haveConstants;
    float dir[3];
    float ambientMul[3];
    float color[3];
};

static const CamSunPreviewCache *Cam_SunPrev_CachedState()
{
    static CamSunPreviewCache cache = {};
    const unsigned epoch = KiwiWalkCache_Epoch();
    if ( !cache.initialized || cache.world != world_entity || cache.epoch != epoch )
    {
        cache.world = world_entity;
        cache.epoch = epoch;
        cache.initialized = true;
        cache.active = false;
        cache.haveConstants = false;
        cache.dir[0] = cache.dir[1] = cache.dir[2] = 0.0f;
        cache.ambientMul[0] = cache.ambientMul[1] = cache.ambientMul[2] = 0.0f;
        cache.color[0] = cache.color[1] = cache.color[2] = 1.0f;

        SunPrev_Setup();
        cache.active = SunPrev_Active() != 0;
        if ( cache.active )
        {
            cache.haveConstants = Cam_SunPrev_SetSunConstants(
                cache.dir, cache.ambientMul, cache.color, /*emit=*/false ) != 0;
        }
    }
    return &cache;
}

// Per-face editor flat colour via MATERIAL_COLOR, emitted only on CHANGE.  The dedup is
// load-bearing: one command per face overflows the editor command buffer on a big map.
static bool Cam_MaterialColorChanged( const float a[4], const float b[4] );
static void Cam_SetLastMaterialColor( float dst[4], const float src[4] );

// Surf-cache emit: per material layer build the real per-vertex geometry, upload it to the
// editor VB pool, queue the cached draw.  The pool is per-frame scratch (re-uploaded every
// Cam_Draw, flushed by R_AddEditorSurfsCmd), so no faceVis_s is persisted here.
static void Cam_DrawFaceCached( face_t *f )
{
    int layerCount = MaterialDef_11( &f->mtldef[g_qeglobals.current_edit_layer] );
    int baseSortKey = -1;                 // seeded from the first drawn layer
    for ( int L = 0; L < layerCount; ++L )
    {
        EdLayerGeom g;
        if ( !Face_BuildLayerGeom( f, (const orientation_t *)world_orient_matrix, L, &g )
             || !g.material || g.vertcount < 3 )
            continue;
        // KIWI-UX (ROUND O): same substitution as the immediate path, applied BEFORE the
        // upload so the VB pool is keyed on the material the draw will actually bind.
        Material *drawMtl = Cam_DrawMaterial( g.material );
        unsigned int handle = Editor_VB_Upload( drawMtl, g.vertcount,
            (const float *)g.xyz, (const float *)g.tangent, (const float *)g.binormal,
            (const float *)g.normal, (const float *)g.st, (const float *)g.color );
        // sortKey = Editor_MaterialSortKey(layer0) + layerIndex, so a face's layers stay
        // consecutive inside one material bucket (DrawGeo 0x47acf0, brush.cpp:6060-6062).
        if ( baseSortKey < 0 )
            baseSortKey = Editor_MaterialSortKey( drawMtl );
        Editor_AddGeoFace( drawMtl, TECHNIQUE_UNLIT, baseSortKey + L, g.vertcount, (int)handle );
    }
}

// Draw `mtl` with the requested technique if its loaded techset carries it, else UNLIT (always
// present).  Keeps the draw safe — a material that fell to the "2d" default techset would
// otherwise hit a NULL technique.
static MaterialTechniqueType Cam_TechAvailable( Material *mtl, MaterialTechniqueType want )
{
    if ( want != TECHNIQUE_UNLIT && ( !mtl->techniqueSet || !mtl->techniqueSet->techniques[want] ) )
        return TECHNIQUE_UNLIT;
    return want;
}

static bool Cam_MaterialColorChanged( const float a[4], const float b[4] )
{
    return a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3];
}

static void Cam_SetLastMaterialColor( float dst[4], const float src[4] )
{
    dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
}

// The world fill's plain arm emits the same Editor_AddGeoFace surf DrawGeo emits, from the
// persistent VB handle Brush_CheckBuildFaceVis uploaded.  STILL IMMEDIATE, and must
// stay so: the sun approximation arm, the sky see-through film, the solid-tool arm, and any
// face whose faceVis is not built.  RADIANT_WORLDGEO=0 restores the immediate arm.
static bool Cam_WorldGeoResident()
{
    static const bool s = ( getenv( "RADIANT_WORLDGEO" ) == nullptr
                            || atoi( getenv( "RADIANT_WORLDGEO" ) ) != 0 );
    return s;
}

// brush.cpp:2499 — int g_edSunBakeVertColor  (the per-vertex sun bake gate)
extern int  g_edSunBakeVertColor;

// KIWI: `ownBlend` draws the face's own (alpha-blended) material, skipping the opaque
// tool-art twin - only for passes drawn after the world flush (lightgrid volumes).
static void Cam_DrawFaceFaithfulImmediate( face_t *f, MaterialTechniqueType tech,
                                           bool ownBlend = false )
{
    int layerCount = MaterialDef_11( &f->mtldef[g_qeglobals.current_edit_layer] );
    for ( int L = 0; L < layerCount; ++L )
    {
        EdLayerGeom g;
        if ( !Face_BuildLayerGeom( f, (const orientation_t *)world_orient_matrix, L, &g )
             || !g.material || g.vertcount < 3 )
            continue;
        // KIWI-UX (ROUND O): substitute BEFORE choosing the technique — the broken
        // material's techset carries only "unlit", the substitute's carries the whole
        // set, so asking the substitute is what lets draw_mode 2/3 keep FAKELIGHT.
        Material *drawMtl = ownBlend ? g.material : Cam_DrawMaterial( g.material );
        MaterialTechniqueType useTech = Cam_TechAvailable( drawMtl, tech );

        float    xyzw[CAM_MAXFACEVERTS][4];
        uint16_t indices[3 * CAM_MAXFACEVERTS];
        for ( int i = 0; i < g.vertcount; ++i )
        {
            xyzw[i][0] = g.xyz[i][0]; xyzw[i][1] = g.xyz[i][1];
            xyzw[i][2] = g.xyz[i][2]; xyzw[i][3] = 1.0f;
        }
        int ic = 0;
        for ( int i = 1; i < g.vertcount - 1; ++i )
        {
            indices[ic++] = 0; indices[ic++] = (uint16_t)i; indices[ic++] = (uint16_t)( i + 1 );
        }
        R_AddRenderCmdDrawTris( drawMtl, useTech, (short)ic, indices,
                                (short)g.vertcount, xyzw, g.normal, (float *)g.color, g.st );
    }
}

// ── THE BX BUCKET/ORDER LAW.  DO NOT REORDER THIS.  ─────────────────────────
// The binary's world draw accumulates one editor-surf per material layer and closes with
// R_AddEditorSurfsCmd (0x4FDA10), whose first statement qsorts the range by
// Editor_SurfCompare (0x4FD9C0) — sortKey, then techType, then firstIndex.  sortKey is
// 100 * the material's own primarySortKey, so opaques (400) precede sky (500), decals
// (1200) and blends/glass (4300+).  A depth-write-free decal drawn BEFORE the opaques is
// repainted by them; that ordering is the only thing keeping it visible.  The port's two
// IMMEDIATE loops (world fill, selected-brush tint) bypassed the sort and are fixed here.
// TIE-BREAK IS SUBMISSION ORDER, deliberately: faces sharing a key keep the sequence they
// had before, which is what the caulk / sky / MATERIAL_COLOR-dedup arms rely on.
// ONE NON-MATERIAL BUCKET: the see-through SKY FILM is a translucent d_white quad that must
// composite over the whole scene, so it keys to CAM_SORTKEY_KIWI_OVERLAY, past every
// material bucket.  Every other sky face keeps its own key of 500.
struct CamSortFace
{
    face_t     *f;              // the face to draw
    // The instance + faceVis index this face's geometry already lives at on the GPU
    // (Editor_VB_Upload, brush.cpp:186).  nullptr/-1 = gathered by a pass with no instance.
    selbrush_t *inst;
    int         faceIndex;
    Material *mtl;              // FaceMaterial( &f->mtldef[layer] ) — selects the draw arm
    int       key;              // Editor_MaterialSortKey( mtl ), or CAM_SORTKEY_KIWI_OVERLAY
    int       seq;              // submission index — the stable tie-break
    bool      isSky;            // round BC: classified in the gather (see above)
    bool      skySeeThrough;    // round BC: this face draws the translucent film
    bool      uvOwns;           // selected pass: KiwiUvEd_OverlaySuppressed brush (no red tint)
};

// One bucket past every material bucket, and it can never collide: primarySortKey is a 6-bit
// field (r_gfx.h:328) and the bucket stride is 100 (r_ed_scene.cpp:192).
static const int CAM_SORTKEY_KIWI_OVERLAY = 100 * 64;

// qsort comparator, shaped exactly like Editor_SurfCompare (r_ed_scene.cpp:199-215): the
// material sort bucket first, then the stable tie-break.
static int __cdecl Cam_SortFaceCompare( const void *pa, const void *pb )
{
    const CamSortFace *a = (const CamSortFace *)pa;
    const CamSortFace *b = (const CamSortFace *)pb;
    const int result = a->key - b->key;
    if ( result )
        return result;
    return a->seq - b->seq;
}

// Append one face to a pass's gather list, computing its sort bucket (and, for the world
// pass, the round-BC sky classification the bucket depends on).
static void Cam_GatherFace( std::vector< CamSortFace > &out, face_t *f, Material *mtl,
                            const MaterialDef *md, bool classifySky, bool uvOwns,
                            selbrush_t *inst = nullptr, int faceIndex = -1 )
{
    CamSortFace e;
    e.f             = f;
    e.inst          = inst;
    e.faceIndex     = faceIndex;
    e.mtl           = mtl;
    e.seq           = (int)out.size();
    e.isSky         = false;
    e.skySeeThrough = false;
    e.uvOwns        = uvOwns;
    // Material_FromHandle asserts on a NULL handle and can answer null for one that failed to
    // register, and Editor_MaterialSortKey dereferences unconditionally (r_ed_scene.cpp:192).
    // Anything unresolved keys at 0, i.e. drawn FIRST, so the opaques can still cover it.
    const Material *mres = mtl ? Material_FromHandle( mtl ) : nullptr;
    e.key           = mres ? Editor_MaterialSortKey( mtl ) : 0;

    if ( classifySky )
    {
        const char *mn = md->radMtl ? md->radMtl->name : nullptr;
        const char *sl = mn ? strrchr( mn, '/' ) : nullptr;
        e.isSky = KiwiSky_IsSkyMaterial( md->radMtl )
               || ( mn && strstr( sl ? sl + 1 : mn, "sky" ) != nullptr );
        if ( e.isSky )
        {
            // The winding CENTROID, not a corner: a shell wall is thousands of units across and
            // one corner can be on the far side of the pivot while the face as a whole is not.
            float ctr[3] = { 0.0f, 0.0f, 0.0f };
            const int np = f->w ? f->w->numpoints : 0;
            if ( np > 0 )
            {
                for ( int pi = 0; pi < np; ++pi )
                    for ( int k = 0; k < 3; ++k )
                        ctr[k] += f->w->p[pi][k];
                const float inv = 1.0f / (float)np;
                for ( int k = 0; k < 3; ++k )
                    ctr[k] *= inv;
            }
            e.skySeeThrough = KiwiSky_SeeThroughFace( ( np > 0 ) ? ctr : nullptr );
        }
        if ( e.skySeeThrough )
            e.key = CAM_SORTKEY_KIWI_OVERLAY;
    }
    out.push_back( e );
}

// The pass tail: order the gathered faces.  Split out so every caller applies the identical
// key, and so a one-face pass costs nothing.
static void Cam_SortGatheredFaces( std::vector< CamSortFace > &faces )
{
    if ( faces.size() <= 1 )
        return;
    // KIWI (2026-10-11, profiled: 7% of a frame): keys are small integers and `seq` is the
    // gather index, so a STABLE counting sort on the key is exactly the qsort's order.
    int lo = faces[0].key, hi = faces[0].key;
    for ( const CamSortFace &e : faces )
    {
        lo = e.key < lo ? e.key : lo;
        hi = e.key > hi ? e.key : hi;
    }
    if ( hi - lo > 65536 )
    {
        qsort( faces.data(), faces.size(), sizeof( CamSortFace ), Cam_SortFaceCompare );
        return;
    }
    static std::vector< int >         s_start;
    static std::vector< CamSortFace > s_out;
    s_start.assign( (size_t)( hi - lo ) + 2, 0 );
    for ( const CamSortFace &e : faces )
        ++s_start[(size_t)( e.key - lo ) + 1];
    for ( size_t k = 1; k < s_start.size(); ++k )
        s_start[k] += s_start[k - 1];
    s_out.resize( faces.size() );
    for ( const CamSortFace &e : faces )
        s_out[(size_t)s_start[(size_t)( e.key - lo )]++] = e;
    faces.swap( s_out );
}

// World faces of the see-through tool volumes and the sky film, drawn after the main surf
// flush (they write no depth, so the resident world drawn at that flush covered them).
struct CamDeferredToolFace
{
    face_t *f;
    float   col[4];   // MATERIAL_COLOR (neutral for a volume's own art; zeros for the film)
    bool    film;     // sky see-through film instead of a flat volume
    bool    ownBlend; // KIWI: blend the material's own art (lightgrid) instead of the twin
    selbrush_t *inst = nullptr;   // KIWI: where its resident geometry lives (tool pass)
    int         faceIndex = -1;
};
static std::vector< CamDeferredToolFace > s_deferredToolFaces;

// The selected-brush pass's faces: immediate ones (no resident geometry) and resident ones,
// which wait for the pass's own flush window after the main world flush.
static std::vector< CamSortFace > s_selFaces;
static std::vector< CamSortFace > s_selResident;

// Can Cam_DrawFaceResident draw this face?  The same gates, without emitting - the
// selected-brush pass decides up front which faces wait for its own flush window.
static bool Cam_FaceResidentReady( const CamSortFace &ent )
{
    if ( !Cam_WorldGeoResident() || g_edSunBakeVertColor )
        return false;
    const selbrush_t *b = ent.inst;
    if ( !b || ent.faceIndex < 0 || !b->faces || ent.faceIndex >= b->faceCount )
        return false;
    if ( !b->def || b->version != b->def->version )
        return false;                       // stale: rebuild deferred while a drag moves it
    const faceVis_s *vis = &( (const faceVis_s *)b->faces )[ent.faceIndex];
    if ( vis->visCount <= 0 || !vis->visArray || vis->vertcount < 3 )
        return false;
    for ( int i = 0; i < vis->visCount; ++i )
        if ( vis->visArray[i].mtlHandle )
            return true;
    return false;
}

// Emit one gathered world face from its RESIDENT geometry.  false = no resident
// geometry for this face right now; the caller runs the immediate path.
static bool Cam_DrawFaceResident( const CamSortFace &ent, MaterialTechniqueType tech )
{
    if ( !Cam_WorldGeoResident() )
        return false;
    // The per-vertex sun bake writes FRAME state into VERTEX data (brush.cpp:2602),
    // so while it is on, resident vertices are the wrong vertices.
    if ( g_edSunBakeVertColor )
        return false;
    selbrush_t *b = ent.inst;
    if ( !b || ent.faceIndex < 0 || !b->faces || ent.faceIndex >= b->faceCount )
        return false;
    const faceVis_s *vis = &( (const faceVis_s *)b->faces )[ent.faceIndex];
    if ( vis->visCount <= 0 || !vis->visArray || vis->vertcount < 3 )
        return false;

    // Per-layer emit, exactly as DrawGeo does it (brush.cpp:6058-6079): the key is the FIRST
    // non-null layer's material bucket and each layer is base + i.
    int sortKey = -1;
    bool emitted = false;
    for ( int i = 0; i < vis->visCount; ++i )
    {
        Material *mtl = vis->visArray[i].mtlHandle;
        if ( !mtl )
            continue;
        Material *drawMtl = Cam_DrawMaterial( mtl );
        if ( sortKey < 0 )
            sortKey = Editor_MaterialSortKey( drawMtl );
        Editor_AddGeoFace( drawMtl, Cam_TechAvailable( drawMtl, tech ), sortKey + i,
                           vis->vertcount, vis->visArray[i].vertHandle );
        emitted = true;
    }
    return emitted;
}


// Sun-preview APPROXIMATION (used when the R_SunPrev_Main path is unavailable):
// MATERIAL_COLOR = the face's directional-sun colour, then draw the textured face UNLIT (which
// multiplies its colormap by MATERIAL_COLOR).  No shadows, no per-pixel lighting.
// RC_SET_MATERIAL_COLOR is a CRITICAL command sharing a 0x2000-byte budget (~410 commands), so
// one per face overruns it on a big map.  Two guards: dedup on the QUANTIZED shade (5-bit
// channels), and a hard per-frame cap — past the cap later faces just reuse the last shade.
static int s_sunMCEmitted = 0;                         // MATERIAL_COLORs emitted this frame
static const int SUN_MC_BUDGET = 300;                  // hard cap (< 410 critical-command ceiling)
static void Cam_DrawFaceSun( face_t *f, Material *mtl, MaterialTechniqueType tech, float lastShade[4] )
{
    float shade[4];
    SunPrev_FaceShade( f->plane.normal, shade );       // world brushes: local normal = world
    // Quantize to 5 bits/channel (1/31 steps) so near-equal facings dedup to one command.
    for ( int c = 0; c < 3; ++c )
    {
        float q = shade[c]; if ( q < 0.0f ) q = 0.0f; if ( q > 4.0f ) q = 4.0f;
        shade[c] = (float)( (int)( q * 31.0f / 4.0f + 0.5f ) ) * 4.0f / 31.0f;
    }
    shade[3] = 1.0f;
    if ( ( shade[0] != lastShade[0] || shade[1] != lastShade[1] ||
           shade[2] != lastShade[2] ) && s_sunMCEmitted < SUN_MC_BUDGET )
    {
        R_AddCmdSetMaterialColor( shade );
        lastShade[0] = shade[0]; lastShade[1] = shade[1];
        lastShade[2] = shade[2]; lastShade[3] = shade[3];
        ++s_sunMCEmitted;
    }
    Cam_DrawFace( f, mtl, tech );                       // textured face, tinted by MATERIAL_COLOR
}

// ── g_SelectedFaces (select.cpp) — the Ctrl+Shift+LMB face-selection set ──────────

// brush.cpp line-outline batcher (reused) + the deferred-line flush command.
extern int DrawShadedWireframe( int cullMode, face_t *face, const orientation_t *orient,
                                GfxColor *lineColor, char width, int vertCount,
                                int vertLimit, GfxPointVertex *verts );

// KISAK: the selected-FACE highlight.  The binary tints picked faces through DrawGeo's editor
// surf-cache; the port outlines each picked winding in magenta via DrawShadedWireframe ->
// R_AddCmd_Line3D, because the UNLIT triangle shader carries no colour for immediate tris.
static void Cam_DrawSelectedFaces()
{
    int count = g_SelectedFaces.GetSize();
    if ( count <= 0 || !g_SelectedFaces.m_pData )
        return;
    // KIWI-UX (ROUND K): USER DIRECTIVE, verbatim — "When Selecting the face of a
    // solid (brush), it still creates a pink outline on the face.  Stop doing
    // that."  THIS IS THE PINK OUTLINE.  It is the port's own selected-face
    // highlight (the binary tints picked faces through DrawGeo's editor surf-cache;
    // the port could not, so it outlines each picked winding in magenta 0xFFFF00FF
    // at width 3 — see the KISAK note above).  Shakeout G replaced face feedback
    // with a translucent FILL (kiwi_hover.cpp DrawFaceFills) precisely so a face
    // accent would stop looking like an edge one, and this pass is what kept
    // drawing the boundary anyway.
    //
    // Gated on the modern layer rather than deleted: with the master toggle OFF
    // there is no KIWI fill pass at all, and removing this outright would leave the
    // classic profile with NO selected-face feedback whatsoever.
    // Cam_DrawSelectedFaceFill (the ported FILL, immediately below) is untouched in
    // both profiles — a fill was never the complaint.
    extern bool KiwiUX_ModernInput();     // kiwi_ux.cpp
    if ( KiwiUX_ModernInput() )
        return;

    static GfxPointVertex s_hlVerts[4096];
    unsigned int magenta = 0xFFFF00FFu;              // packed line colour (the $line material
                                                     // colours from CODE_MATERIAL_COLOR, set below)
    int vc = 0;
    for ( int i = 0; i < count; ++i )
    {
        selbrush_t *b = g_SelectedFaces.GetAt( i ).brush;
        if ( !b || !b->def )
            continue;
        int idx = g_SelectedFaces.GetAt( i ).index;
        if ( (unsigned)idx >= (unsigned)b->def->faceCount )
            continue;
        face_t *f = &b->def->faces[idx];
        if ( !f->w )
            continue;
        // KIWI-UX (ROUND BN, ITEM 3): the classic profile's face outline, same rule as
        // the ported fill above — in scope, the UV editor owns the face's look.
        if ( KiwiUvEd_OverlaySuppressed( b->def, idx ) )
            continue;
        vc = DrawShadedWireframe( -1, f, (const orientation_t *)world_orient_matrix,
                                  (GfxColor *)&magenta, 3, vc, 4096, s_hlVerts );
    }
    if ( vc )
    {
        // The $line material colours from CONST_SRC_CODE_MATERIAL_COLOR, not the per-vertex
        // colour — drive the tint through R_AddCmdSetMaterialColor, then reset to white.
        static const float s_magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
        R_AddCmdSetMaterialColor( s_magenta );
        R_AddCmd_Line3D( (short)( vc / 2 ), 3, s_hlVerts );
        R_AddCmdSetMaterialColor( s_white );
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x408106  Cam_DrawSelectedFaceFill — Cam_Draw's SELECTED-FACE FILL pass, gated on
// !dontDrawSelectedTint && g_SelectedFaces.GetSize() > 0.  Every picked face's winding is
// batched (Face_AddWindingToTriBatch) into ONE white-UNLIT triangle draw carrying
// the flat d_savedinfo.colors[16] fill colour; the batcher auto-flushes at the caps.
// MATERIAL_COLOR is neutral across the pass (R_SetMaterialColor(NULL))
// so the per-vertex colour drives the fill.
// The batch buffers are function-static (the binary keeps ~100 KB of them in
// Cam_Draw's stack frame); sizes are the binary's exactly.
// ─────────────────────────────────────────────────────────────────────────────
extern void Face_AddWindingToTriBatch( face_t *face, const float *packedColor,
                                       int *indexCount, ushort *indices,
                                       int *vertCount, float ( *xyzw )[4],
                                       float ( *normal )[3], float *colorArr,
                                       float ( *st )[2] );                 // brush.cpp 0x47b780

static void Cam_DrawSelectedFaceFill()
{
    const int count = g_SelectedFaces.GetSize();
    if ( count <= 0 )
        return;

    static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    R_AddCmdSetMaterialColor( s_neutral );                    // R_SetMaterialColor(NULL)

    GfxColor gfx_col;
    Byte4PackPixelColor( g_qeglobals.d_savedinfo.colors[16], &gfx_col );

    static float          s_st[1362][2];
    static float          s_normal[1362][3];
    static float          s_xyzw[1362][4];
    static float          s_color[1362];
    static ushort s_indices[2046];

    int indexCount = 0;
    int vertCount  = 0;

    for ( int i = 0; i < count; ++i )
    {
        selface_t   selFace = g_SelectedFaces.GetAt( i );
        selbrush_t *b       = selFace.brush;
        if ( !b || !b->def )
            continue;                                         // port guard (headless/stale pick)
        const int   idx     = selFace.index;
        // CamWnd.cpp:2627
        iassert( selFace.face == &selFace.brush->faces[selFace.index] );
        brush_t *def = b->def;
        if ( (unsigned)idx >= (unsigned)def->faceCount )
            continue;                                         // port guard
        // KIWI-UX (ROUND BN, ITEM 3): the UV editor owns this face's look while it is
        // in scope — the fill is the pass that hides the texture the user is moving.
        if ( KiwiUvEd_OverlaySuppressed( def, idx ) )
            continue;
        Face_AddWindingToTriBatch( &def->faces[idx], (const float *)&gfx_col,
                                   &indexCount, s_indices, &vertCount,
                                   s_xyzw, s_normal, s_color, s_st );
    }

    if ( indexCount != 0 && vertCount != 0 )
        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                (short)indexCount, s_indices, (short)vertCount,
                                s_xyzw, s_normal, s_color, s_st );
}

// ── xmodel MESH draw ──────────────────────────────────────────────────────────
// Cam_Draw reaches misc_model meshes through DrawBrush -> DrawModels -> SetupModelInst
// (Entity_UpdateModelInst registers the xmodel in the editor model-inst buffer) + SkinModelInst
// (builds GfxModelSkinnedSurfaces, queued as ED_SURF_MODEL surfs); R_AddEditorSurfsCmd flushes.
extern void  SkinModelInst( int instanceHandle, Material *checkhandle, int techType,
                            const int *colorPtr, int drawFlags );          // r_ed_scene.cpp 0x4FE2E0

// Editor model-load asset-drop recovery guard (engine_stubs.cpp).
extern int     g_radiantAssetLoadGuard;
extern jmp_buf g_radiantAssetLoadJmp;

// Operator escape hatch: RADIANT_MODELS=0 disables the xmodel mesh draw (default ON).
// Non-static — the 2D views share this one gate.
bool Editor_ModelsEnabled()
{
    static const bool s = []{
        const char *v = getenv( "RADIANT_MODELS" );
        return !( v && v[0] == '0' && v[1] == '\0' );
    }();
    return s;
}

// Per-entity model instance + skin under SEH, so a model whose load derefs a NULL handle is
// skipped rather than crashing.  Returns 1 iff the entity has a renderable model/prefab
// (Entity_HasRenderableModel = the binary's DrawBrush gate; a bare-prefab entity
// returns 1 with no modelInst and the caller draws the prefab CONTENTS).
// Its own function because MSVC forbids setjmp and __try in one function (C2713).
extern char  Entity_HasRenderableModel( selbrush_t *b, const float *orient );  // brush.cpp 0x479610
// Non-zero only across the 2D view's UNSELECTED brush loop (XY_DrawBrushes, xywnd.cpp): the
// per-entity tint travels as a flat MATERIAL_COLOR instead of a per-vertex stamp, because
// the stamp forces a writable tempSkinBuf copy and that disqualifies the surface from the
// geometry cache and the instance merge.  The camera's tech-29 arm and the XY SELECTED pass
// never see this set, so both keep the stamp.
extern int   g_edXyModelFlatTint;                                         // xywnd.cpp:1161
static GfxColor s_sunModelFallbackColor;  // packed worldspawn sun colour for slot-24 fallback
static int Cam_SkinModelSEH( selbrush_t *b, const orientation_t *orient, int meshTech,
                             GfxColor *col, int drawFlags )
{
    int renderable = 0;
    __try
    {
        if ( Entity_HasRenderableModel( b, (const float *)orient ) )
        {
            renderable = 1;
            if ( b->owner->modelInst )
            {
                // DrawModels: SkinModelInst(inst, checkhandle, draw_meth2,
                // draw_meth2 != 29 ? 0 : color, drawFlags) — the per-vert colour override
                // only rides the WIREFRAME technique.
                const int *colorPtr = ( meshTech != TECHNIQUE_WIREFRAME_SHADED ) ? nullptr : (const int *)col;
                // KIWI-UX: SkinModelInst applies this only to materials that lack their
                // native sunlight-preview receiver and use the N.L fallback.  Native-only
                // instances remain read-only/static-cache eligible; native surfaces in a
                // mixed instance keep their authored colours.
                if ( meshTech == TECHNIQUE_SUNLIGHT_PREVIEW )
                    colorPtr = (const int *)&s_sunModelFallbackColor;
                if ( colorPtr && g_edXyModelFlatTint )
                    colorPtr = nullptr;      // constant-level tint
                SkinModelInst( b->owner->modelInst, nullptr, meshTech,
                               colorPtr, drawFlags );
            }
        }
    }
    __except( EXCEPTION_EXECUTE_HANDLER )
    {
        renderable = 0;   // a model that AV'd while loading
    }
    return renderable;
}

// Install the ERR_DROP recovery frame, then instance + skin one entity's model.  A
// CoD4-format asset the CoD3-sized parser chokes on raises Com_Error(ERR_DROP) deep in the
// load; engine_stubs.cpp longjmps back here and Model_SetModel's modelFailed flag stops that
// model retrying.  The guarded entry DrawBrush calls per fixedsize entity.
int Editor_InstanceAndSkinModel( selbrush_t *b, const orientation_t *orient, int meshTech,
                                 GfxColor *col, int drawFlags )
{
    // PARSE-STATE PRECONDITION: XModel_LoadPhysicsCollMap parses phys_collmaps/<name>.map at
    // parseInfoNum==0 (no map session during a draw).  Collmap geom holds negative floats; with
    // parseInfo[0] left at spaceDelimited==0 && negativeNumbers==0 by an earlier editor parse,
    // Com_ParseExt splits "-0.000000" into "-" + "0.000000" and every later read shifts by one.
    // Enable negativeNumbers for the load (identical result in either tokenizer mode).
    ParseThreadInfo *parse = Com_GetParseThreadInfo();
    parseInfo_t     *pi    = &parse->parseInfo[parse->parseInfoNum];
    int              savedNeg = pi->negativeNumbers;
    pi->negativeNumbers = 1;

    if ( setjmp( g_radiantAssetLoadJmp ) != 0 )
    {
        g_radiantAssetLoadGuard = 0;   // unwound from an ERR_DROP — frame reset, skip
        return 0;
    }
    ++g_radiantAssetLoadGuard;
    int skinned = Cam_SkinModelSEH( b, orient, meshTech, col, drawFlags );
    --g_radiantAssetLoadGuard;
    pi->negativeNumbers = savedNeg;
    return skinned;
}

// SunLightPreview_DrawBrushShadow's misc_model arm drives a real model load on
// entities the base pass may never have touched, so it needs the SAME bracket
// Editor_InstanceAndSkinModel installs.  Only the bracket is the port's — the arm is
// binary code.
extern char Radiant_ShadVol_ModelShadowArm( selbrush_t *sb, orientation_t *orient,
                                            const float *light );          // shadowvolume.cpp
static int Cam_ShadowModelSEH( selbrush_t *b, orientation_t *orient, const float *light )
{
    int built = 0;
    __try   { built = Radiant_ShadVol_ModelShadowArm( b, orient, light ) ? 1 : 0; }
    __except( EXCEPTION_EXECUTE_HANDLER ) { built = 0; }   // a model that AV'd while loading
    return built;
}

int Editor_ModelShadowGuarded( selbrush_t *b, orientation_t *orient, const float *light )
{
    ParseThreadInfo *parse = Com_GetParseThreadInfo();
    parseInfo_t     *pi    = &parse->parseInfo[parse->parseInfoNum];
    int              savedNeg = pi->negativeNumbers;
    pi->negativeNumbers = 1;

    if ( setjmp( g_radiantAssetLoadJmp ) != 0 )
    {
        g_radiantAssetLoadGuard = 0;   // unwound from an ERR_DROP — frame reset, skip
        return 0;
    }
    ++g_radiantAssetLoadGuard;
    int built = Cam_ShadowModelSEH( b, orient, light );
    --g_radiantAssetLoadGuard;
    pi->negativeNumbers = savedNeg;
    return built;
}

// ── LIGHT-PREVIEW GLOW SPHERE ─────────────────────────────────────────────────
// 0x4058e0  LightPreview_DrawLight2 — DrawLightsMain's fallback when the per-pixel
// GPU light path (LightPreview_DrawLight) is disabled or unavailable: a 32x16 UV-sphere
// of `radius` at the light origin, packed with the light colour, drawn additively through
// R_AddRenderCmdDrawTris($additive, TECHNIQUE_UNLIT).
// Colour per DrawLightsMain: _color (default white), saturated to the max component at the
// selected preview intensity; command 36122 switches between the key and retail's 1e7 mode.
// Topology is the binary's: 32 longitude x 16 latitude -> 15 rings of 32 (480) + 2 poles = 482,
// quad-pairs per ring + two tri pole caps.  Rebuilt from intent; vertex count and index
// pattern are exact.
extern int   Entity_GetIntValueForKey( const entity_s *e, const char *key );        // entity.cpp (0x483820)
extern void __cdecl R_SetLightShaderConstants( const float *origin, float radius,
                                               const float *color, const char *defName,
                                               const float *dir, float cosHalfFovInner,
                                               float cosHalfFovOuter, int exponent );

static int Entity_Light( const float *worldPos, const entity_s *defPtr, selbrush_t *scope,
                         const orientation_t *orient,
                         float *outDir, float *outCosInner, float *outCosOuter,
                         float *outCosHalfFov );
static int __cdecl Cam_LightGatherCached( KiwiLightCasterRecord *out,
                                          const float *origin, float radius );
static void Cam_BrushColor2d( selbrush_t *b, GfxColor *out );

static void Cam_BuildLightGlowSphere( const float center[3], float radius,
                                      const float rgb[3] )
{
    const int sideCount = 32;             // longitudinal segments (binary: 32)
    const int halfSide  = sideCount / 2;  // 16 latitude bands
    const int ringCount = halfSide - 1;   // 15 full rings of `sideCount` verts
    const int ringVerts = ringCount * sideCount;        // 480
    const int vertCount = ringVerts + 2;                // + 2 poles = 482

    // index count: ring quad band (ringCount-1 bands) ×6 + 2 pole caps ×(sideCount×3)
    // (matches the binary's two index loops: 14 bands of 32 quads + top/bottom 32 tris)
    static float    xyzw  [482][4];
    static float    normal[482][3];
    static float    color [482];
    static float    st    [482][2];       // R_AddRenderCmdDrawTris memcpy's st unconditionally
    static uint16_t indices[ (32 - 1) * 32 * 6 + 2 * 32 * 6 ];   // generous upper bound

    GfxColor packed;
    Byte4PackPixelColor( const_cast<float *>( rgb ), &packed );
    const float fcol = *(const float *)&packed;          // BGRA bit-pattern as float

    // ── ring vertices: row r ∈ [1..ringCount], col c ∈ [0..sideCount) ──
    int vc = 0;
    for ( int r = 1; r <= ringCount; ++r )
    {
        // latitude angle θ = r * (2π / sideCount)  (binary: row*0.19634954 = row*2π/32)
        const float theta = (float)r * ( (float)( 2.0 * KIWI_PI ) / (float)sideCount );   // KIWI: KIWI_PI
        const float sinT = sinf( theta ), cosT = cosf( theta );
        for ( int c = 0; c < sideCount; ++c )
        {
            const float phi = (float)c * ( (float)( 2.0 * KIWI_PI ) / (float)sideCount );   // KIWI: KIWI_PI
            const float nx = cosf( phi ) * sinT;
            const float ny = sinf( phi ) * sinT;
            const float nz = cosT;
            normal[vc][0] = nx; normal[vc][1] = ny; normal[vc][2] = nz;
            xyzw[vc][0] = nx * radius + center[0];
            xyzw[vc][1] = ny * radius + center[1];
            xyzw[vc][2] = nz * radius + center[2];
            xyzw[vc][3] = 1.0f;
            color[vc] = fcol;
            st[vc][0] = (float)c / (float)sideCount;     // polar UV (binary maps the same)
            st[vc][1] = (float)r / (float)halfSide;
            ++vc;
        }
    }
    // top pole (+Z) and bottom pole (−Z)
    const int topPole = vc, botPole = vc + 1;
    normal[topPole][0] = 0.0f; normal[topPole][1] = 0.0f; normal[topPole][2] = 1.0f;
    xyzw[topPole][0] = center[0]; xyzw[topPole][1] = center[1];
    xyzw[topPole][2] = radius + center[2]; xyzw[topPole][3] = 1.0f;
    color[topPole] = fcol; st[topPole][0] = 0.0f; st[topPole][1] = 0.0f;
    normal[botPole][0] = 0.0f; normal[botPole][1] = 0.0f; normal[botPole][2] = -1.0f;
    xyzw[botPole][0] = center[0]; xyzw[botPole][1] = center[1];
    xyzw[botPole][2] = -radius + center[2]; xyzw[botPole][3] = 1.0f;
    color[botPole] = fcol; st[botPole][0] = 0.0f; st[botPole][1] = 1.0f;

    // ── indices: 14 quad bands between adjacent rings ──
    int ic = 0;
    for ( int r = 0; r < ringCount - 1; ++r )
    {
        const int base = r * sideCount, next = ( r + 1 ) * sideCount;
        for ( int c = 0; c < sideCount; ++c )
        {
            const int c1 = ( c + 1 ) % sideCount;
            indices[ic++] = (uint16_t)( base + c  );
            indices[ic++] = (uint16_t)( base + c1 );
            indices[ic++] = (uint16_t)( next + c  );
            indices[ic++] = (uint16_t)( next + c  );
            indices[ic++] = (uint16_t)( base + c1 );
            indices[ic++] = (uint16_t)( next + c1 );
        }
    }
    // top cap: ring 0 → top pole
    for ( int c = 0; c < sideCount; ++c )
    {
        const int c1 = ( c + 1 ) % sideCount;
        indices[ic++] = (uint16_t)topPole;
        indices[ic++] = (uint16_t)( 0 + c1 );
        indices[ic++] = (uint16_t)( 0 + c  );
    }
    // bottom cap: last ring → bottom pole
    const int last = ( ringCount - 1 ) * sideCount;
    for ( int c = 0; c < sideCount; ++c )
    {
        const int c1 = ( c + 1 ) % sideCount;
        indices[ic++] = (uint16_t)botPole;
        indices[ic++] = (uint16_t)( last + c  );
        indices[ic++] = (uint16_t)( last + c1 );
    }

    // d_additive may be null if the matsys default-material init hasn't run — guard so a
    // light entity never crashes the camera before assets are registered.
    Material *addMat = g_qeglobals.d_additive;
    if ( !addMat )
        return;
    R_AddRenderCmdDrawTris( addMat, TECHNIQUE_UNLIT, (short)ic, indices,
                            (short)vertCount, xyzw, normal, color, st );
}

static entity_s_def *Region_FindTargetEntity( selbrush_t *scope, const char *name );
static bool Cam_LightPreview_CompilerSpotForDualBits(
    const float *origin, entity_s_def *ent, selbrush_t *scope,
    const orientation_t *orient, float *outDir, float *outCosInner,
    float *outCosOuter, float *outCosHalfFov );

// 0x4065e0 LightPreview_SetLightTechnique. Entity_Light supplies the exact
// spot/omni classification and cone values.
static int Cam_LightPreview_SetLightTechnique( const float *origin, float radius,
                                                const float *color, entity_s_def *ent,
                                                selbrush_t *scope,
                                                const orientation_t *orient )
{
    float dir[3] = { 0.0f, 0.0f, 0.0f };
    float cosInner = 0.0f, cosOuter = 0.0f, cosHalfFov = 0.0f;
    int cls = Entity_Light( origin, ent, scope, orient, dir,
                            &cosInner, &cosOuter, &cosHalfFov );
    // KIWI-UX: faithful Entity_Light (IDB 0x4063A0) gives bit 1 immediate omni
    // precedence.  cod4map tests primary bit 2 first, so a valid dual-bit authored
    // primary previews as the compiler's spot without changing the faithful function.
    const int spawnflags = Entity_GetIntValueForKey( ent, "spawnflags" );
    if ( ( spawnflags & 3 ) == 3
      && Cam_LightPreview_CompilerSpotForDualBits(
             origin, ent, scope, orient, dir, &cosInner, &cosOuter, &cosHalfFov ) )
        cls = 2;
    (void)cosHalfFov;

    R_AddCmdProjectionSet2D();
    const char *defName = ValueForKey2( ent, "def" );
    if ( cls == 2 )
    {
        const int exponent = Entity_GetIntValueForKey( ent, "exponent" );
        R_SetLightShaderConstants( origin, radius, color, defName, dir,
                                   cosInner, cosOuter, exponent );
        return TECHNIQUE_LIGHT_SPOT;
    }

    R_SetLightShaderConstants( origin, radius, color, defName, nullptr,
                               0.0f, 0.0f, 0 );
    return TECHNIQUE_LIGHT_OMNI;
}

// 0x406fb0 LightPreview_DrawLight. The 52-byte gather result is KIWI-cached;
// every render command and brush/shadow pass retains the binary's order.
static bool Cam_LightPreview_DrawLight( const float origin[4], float radius,
                                        const float color[3], entity_s_def *ent,
                                        selbrush_t *scope,
                                        const orientation_t *orient, bool firstLight,
                                        Material *multiplyMaterial )
{
    // KIWI-UX: the faithful IDB 0x406FB0 pass unconditionally uses both editor-only
    // materials.  Make the fallback visible and deterministic instead of submitting a
    // null/default fullscreen pass.  The kiwi helper below separately covers stencilshadow.
    const bool multiplyMissing = !multiplyMaterial
                              || ( rgp.defaultMaterial
                                && Material_IsDefault( multiplyMaterial ) );
    const bool clearMissing = !rgp.clearAlphaStencilMaterial
                           || ( rgp.defaultMaterial
                             && Material_IsDefault( rgp.clearAlphaStencilMaterial ) );
    if ( multiplyMissing || clearMissing )
    {
        static bool s_reportedMissingPreviewMaterial = false;
        if ( !s_reportedMissingPreviewMaterial )
        {
            s_reportedMissingPreviewMaterial = true;
            Sys_Printf( "Light preview: faithful per-light pass unavailable (%s missing) - "
                        "drawing glow spheres instead.\n",
                        multiplyMissing ? "white_multiply" : "rgp.clearAlphaStencilMaterial" );
        }
        return false;
    }
    if ( !KiwiLight_PerPixelPreviewReady( multiplyMaterial,
                                           rgp.clearAlphaStencilMaterial ) )
        return false;

    int casterCount = 0;
    // KIWI-UX: Reuse the per-light caster walk until its geometric key changes.
    const KiwiLightCasterRecord *casters = KiwiLightCache_Get(
        ent, origin, radius, Cam_LightGatherCached, &casterCount );
    if ( !casters )
        return false;

    if ( firstLight )
    {
        float ambientColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        entity_s_def *worldDef = world_entity ? (entity_s_def *)world_entity->def : nullptr;
        if ( worldDef && Entity_GetVec3ForKey( worldDef, ambientColor, "_color" ) )
        {
            // KIWI-UX: ColorNormalize writes white for a zero vector.  The compiler and
            // IDB branch on its returned maximum, so an explicit "0 0 0" stays black.
            const float maxComp = ColorNormalize( ambientColor, ambientColor );
            if ( maxComp != 0.0f )
            {
                const float ambient = Entity_GetFloatValueForKey(
                    worldDef, "ambient" );
                ambientColor[0] *= ambient;
                ambientColor[1] *= ambient;
                ambientColor[2] *= ambient;
            }
            else
            {
                ambientColor[0] = ambientColor[1] = ambientColor[2] = 0.0f;
            }
        }
        R_AddCmdProjectionSet2D();
        R_AddCmdDrawFullScreenColoredQuad( 0.0f, 0.0f, 1.0f, 1.0f,
                                           ambientColor, multiplyMaterial );
    }

    const int technique = Cam_LightPreview_SetLightTechnique(
        origin, radius, color, ent, scope, orient );

    extern bool Radiant_ShadVol_Begin( int frontCapPerTri, float directionalExtrudeDistance );
    extern void SunLightPreview_DrawBrushShadow( const float *light, selbrush_t *brush,
                                                  orientation_t *brushOrient );
    extern void SunLightPreview_PolyOffsetShadows();
    if ( Radiant_ShadVol_Begin( 6, 0.0f ) )     // point lights: binary extrusion unchanged
    {
        for ( int i = 0; i < casterCount; ++i )
            SunLightPreview_DrawBrushShadow(
                origin, casters[i].brush, (orientation_t *)&casters[i].orient );
    }
    SunLightPreview_PolyOffsetShadows();

    R_SortMaterials();
    for ( int i = 0; i < casterCount; ++i )
    {
        GfxColor brushColor;
        Cam_BrushColor2d( casters[i].brush, &brushColor );
        // Editor_AddMeshCmd and Editor_AddSurfCmd drop a material that lacks 21/22.
        // Never demote a light contribution to UNLIT: that would brighten unlit geometry.
        DrawBrush( casters[i].brush, &casters[i].orient, /*viewType*/ -1,
                   technique, &brushColor, /*width*/ 1, /*drawFlags*/ 0, "" );
    }
    R_AddEditorSurfsCmd();
    return true;
}

// 0x407180 DrawLightsMain. The glow sphere remains the binary's failure/disabled fallback.
static bool Cam_DrawLightsMain( selbrush_t *brush, selbrush_t *scope,
                                const orientation_t *orient,
                                bool firstLight, Material *multiplyMaterial )
{
    entity_s_def *ent = (entity_s_def *)brush->owner->def;
    iassert( brush->owner->def == brush->def->owner );

    if ( FilterBrush( brush, 0 ) )
        return false;
    const float radius = Entity_GetFloatValueForKey( ent, "radius" );
    if ( radius <= 0.0f )
        return false;

    const float localCenter[3] = {
        0.5f * ( brush->def->mins[0] + brush->def->maxs[0] ),
        0.5f * ( brush->def->mins[1] + brush->def->maxs[1] ),
        0.5f * ( brush->def->mins[2] + brush->def->maxs[2] )
    };
    float worldCenter[4];
    OrientationPosToWorldPos( worldCenter, localCenter, orient );
    worldCenter[3] = 1.0f;

    float color[3] = { 1.0f, 1.0f, 1.0f };
    const char *colorText = ValueForKey2( ent, "_color" );
    float parsed[3];
    if ( colorText
      && sscanf( colorText, "%f %f %f", &parsed[0], &parsed[1], &parsed[2] ) == 3 )
        memcpy( color, parsed, sizeof(color) );

    float intensity = Entity_GetFloatValueForKey( ent, "intensity" );
    if ( !g_qeglobals.preview_at_max_intensity )
        intensity = 10000000.0f;
    else if ( intensity <= 0.0f )
        intensity = 1.0f;

    float maxColor = color[0];
    if ( color[1] > maxColor ) maxColor = color[1];
    if ( color[2] > maxColor ) maxColor = color[2];
    float scale = intensity;
    if ( maxColor != 0.0f )
        scale /= maxColor;
    color[0] *= scale;
    color[1] *= scale;
    color[2] *= scale;

    bool drawn = false;
    if ( g_PrefsDlg->enable_light_preview )
        drawn = Cam_LightPreview_DrawLight( worldCenter, radius, color, ent, scope, orient,
                                            firstLight, multiplyMaterial );
    if ( !drawn )
        Cam_BuildLightGlowSphere( worldCenter, radius, color );
    return true;
}

// ActiveSunLightPreviewInit's pinned loop, followed by Cam_Draw's selected-light loop.
// A pinned+selected light is intentionally submitted twice, as in CoD4Radiant.
static void Cam_DrawLightPreviews( bool ambientBaseDone, Material *multiplyMaterial )
{
    camwndState_t *cam = &g_camwndState;
    if ( g_PrefsDlg->enable_light_preview )
    {
        for ( int i = 0; i < cam->light_preview_count; ++i )
        {
            camLightPreviewRec_t *rec = &cam->light_preview_arr[i];
            selbrush_t *brush = (selbrush_t *)(intptr_t)rec->inst;
            // the signed-byte gate is the brush's (brush+52), not the preview record's.
            if ( brush->brushFlags & 0x80 )
            {
                continue;
            }
            // KIWI-UX: use the same cod4map acceptance gates as the Light status panel.
            if ( KiwiLight_PreviewPrimaryOnly() && !KiwiLight_GameWillRender( brush ) )
                continue;
            if ( Cam_DrawLightsMain( brush, (selbrush_t *)(intptr_t)rec->arg2, &rec->orient,
                                     !ambientBaseDone, multiplyMaterial ) )
                ambientBaseDone = true;
        }
    }

    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes; b = b->next )
    {
        iassert( b->owner->def == b->def->owner );
        entity_s_def *ent = (entity_s_def *)b->owner->def;
        if ( !( ent->eclass->classtype & 1 ) )
            continue;
        // KIWI-UX: use the same cod4map acceptance gates as the Light status panel.
        if ( KiwiLight_PreviewPrimaryOnly() && !KiwiLight_GameWillRender( b ) )
            continue;
        if ( Cam_DrawLightsMain( b, nullptr, (const orientation_t *)world_orient_matrix,
                                 !ambientBaseDone, multiplyMaterial ) )
            ambientBaseDone = true;
    }
}

// Camera decorations drawn after the world pass: DrawTriggerRadius and
// CamWnd_Tokens, both on the immediate RC_DRAW_TRIANGLES (d_white, UNLIT) path.
extern int   ScriptGroup_Unreachable( const char *a1 );                              // scriptgroup.cpp 0x451170
extern void  Ed_DrawScriptColorQuad( const entity_s *entDef, const float *color );               // brush.cpp 0x46AE10

// The 7 token colours (r/b/y/c/g/p/o), shared with brush.cpp.
static const float kCamTokenColors[7][4] = {
    { 1.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f },
    { 1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 1.0f, 1.0f },
    { 0.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f, 1.0f, 1.0f },
    { 1.0f, 0.4f, 0.0f, 1.0f },
};

// 0x405D00  Ed_DrawTriggerCylinder - a 32-sided trigger volume: top end-cap disc (verts
// 0..31), bottom end-cap disc (32..63), side wall (64..95 top / 96..127 bottom); 128 verts,
// 372 indices, UNLIT + per-vertex colour.  center[3] is the trigger base; topZ/botZ are ring
// z-offsets above center.z.  Rebuilt from intent; vertex roles, angle step 2pi/32 and both
// index loops are exact.
static void Ed_DrawTriggerCylinder( const GfxColor *col, const float center[3],
                                    float radius, float topZ, float botZ )
{
    const int sideCount = 32;
    static float    xyzw  [128][4];
    static float    normal[128][3];
    static float    color [128];
    static uint16_t indices[372];

    const float fcol = *(const float *)&col->packed;
    const float zTop = center[2] + topZ;
    const float zBot = center[2] + botZ;
    const float kStep = (float)( 2.0 * KIWI_PI ) / (float)sideCount;     // KIWI: KIWI_PI (dbl_6F4500 = 2π/32)

    for ( int i = 0; i < sideCount; ++i )
    {
        const float ang = (float)i * kStep;
        const float c = cosf( ang ), s = sinf( ang );
        const float rx = c * radius + center[0];
        const float ry = s * radius + center[1];
        // group0 (0..31): top cap ring; normal (0,0,-1).
        xyzw[i][0]=rx; xyzw[i][1]=ry; xyzw[i][2]=zTop; xyzw[i][3]=1.0f;
        normal[i][0]=0; normal[i][1]=0; normal[i][2]=-1.0f; color[i]=fcol;
        // group1 (32..63): bottom cap ring.
        xyzw[i+32][0]=rx; xyzw[i+32][1]=ry; xyzw[i+32][2]=zBot; xyzw[i+32][3]=1.0f;
        normal[i+32][0]=0; normal[i+32][1]=0; normal[i+32][2]=1.0f; color[i+32]=fcol;
        // group2 (64..95): wall-top ring (radial normal).
        xyzw[i+64][0]=rx; xyzw[i+64][1]=ry; xyzw[i+64][2]=zTop; xyzw[i+64][3]=1.0f;
        normal[i+64][0]=c; normal[i+64][1]=s; normal[i+64][2]=0; color[i+64]=fcol;
        // group3 (96..127): wall-bottom ring.
        xyzw[i+96][0]=rx; xyzw[i+96][1]=ry; xyzw[i+96][2]=zBot; xyzw[i+96][3]=1.0f;
        normal[i+96][0]=c; normal[i+96][1]=s; normal[i+96][2]=0; color[i+96]=fcol;
    }

    // index loop 1 (i=2..31): top-cap fan (0,i-1,i) + bottom-cap fan (32,i+32,i+31).
    int ic = 0;
    for ( int i = 2; i < sideCount; ++i )
    {
        indices[ic+0]=0;            indices[ic+1]=(uint16_t)(i-1);    indices[ic+2]=(uint16_t)i;
        indices[ic+3]=32;           indices[ic+4]=(uint16_t)(i+32);   indices[ic+5]=(uint16_t)(i+31);
        ic += 6;
    }
    // index loop 2 (i=0..31): cylinder wall (v+64, v+96, n+96) + (n+96, n+64, v+64),
    // where n=(i+1)%32.
    for ( int i = 0; i < sideCount; ++i )
    {
        const int n = ( i + 1 ) % sideCount;
        indices[ic+0]=(uint16_t)(i+64); indices[ic+1]=(uint16_t)(i+96); indices[ic+2]=(uint16_t)(n+96);
        indices[ic+3]=(uint16_t)(n+96); indices[ic+4]=(uint16_t)(n+64); indices[ic+5]=(uint16_t)(i+64);
        ic += 6;
    }
    iassert( ic == 372 );

    if ( !g_qeglobals.d_white )
        return;
    static float st[128][2] = { { 0.0f, 0.0f } };          // memcpy'd unconditionally
    R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT, (short)ic, indices,
                            128, xyzw, normal, color, st );
}

// 0x46CA10  Brush_GetColor2d - eclass colour for non-worldspawn, else d_savedinfo.colors[9].
static void Cam_BrushColor2d( selbrush_t *b, GfxColor *out )
{
    float rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    entity_s     *owner = b->owner;
    entity_s_def *eDef  = owner ? (entity_s_def *)owner->def : nullptr;
    eclass_t     *ec    = eDef ? eDef->eclass : nullptr;
    if ( ec && ec->name && _stricmp( ec->name, "worldspawn" ) != 0 )
    { rgba[0] = ec->color[0]; rgba[1] = ec->color[1]; rgba[2] = ec->color[2]; }
    else
    { rgba[0] = g_qeglobals.d_savedinfo.colors[9][0];
      rgba[1] = g_qeglobals.d_savedinfo.colors[9][1];
      rgba[2] = g_qeglobals.d_savedinfo.colors[9][2]; }
    rgba[3] = 1.0f;
    Byte4PackPixelColor( rgba, out );
}

// 0x407410  DrawTriggerRadius - for a trigger entity with a "radius" or "fixedNodeSafeRadius"
// key, two stacked cylinders per volume ("radius" in the brush 2D colour @alpha 64,
// fixedNodeSafeRadius in cyan 1090453504).  Height: disk trigger (classtype&0x80) 32, else
// the "height" key, else 80.
static void Cam_DrawTriggerRadius( selbrush_t *b )
{
    iassert( b->owner->def == b->def->owner );
    entity_s_def *eDef = (entity_s_def *)b->owner->def;
    const float radius   = Entity_GetFloatValueForKey( eDef, "radius" );
    const float safeRad  = Entity_GetFloatValueForKey( eDef, "fixedNodeSafeRadius" );
    if ( !( radius > 0.0f || safeRad > 0.0f ) )
        return;
    if ( ( b->brushFlags & 2 ) != 0 )            // hidden (brushFlags bit 1)
        return;

    // height: disk trigger → 32; else "height" key (>0) else 80.
    float height;
    eclass_t *eclass = eDef->eclass;
    if ( ( eclass->classtype & 0x80 /*CLASS_TRIGGER_DISC*/ ) != 0 )
        height = 32.0f;
    else
    {
        height = Entity_GetFloatValueForKey( eDef, "height" );
        if ( !( height > 0.0f ) )
            height = 80.0f;
    }

    // center = (origin.x, origin.y, eclass.mins[2] + origin.z)
    float center[3];
    center[0] = eDef->origin[0];
    center[1] = eDef->origin[1];
    center[2] = eclass->mins[2] + eDef->origin[2];

    if ( radius > 0.0f )
    {
        GfxColor c;
        Cam_BrushColor2d( b, &c );
        c.array[3] = 64;                          // alpha 64
        Ed_DrawTriggerCylinder( &c, center, radius, height, 0.0f );
        Ed_DrawTriggerCylinder( &c, center, radius, 0.0f, height );
    }
    if ( safeRad > 0.0f )
    {
        GfxColor c;
        c.packed = 1090453504u;                   // binary literal (cyan-ish, a=64)
        Ed_DrawTriggerCylinder( &c, center, safeRad, height, 0.0f );
        Ed_DrawTriggerCylinder( &c, center, safeRad, 0.0f, height );
    }
}

// 0x4076C0  CamWnd_Tokens - the script-colour name-token overlay: parse the space-separated
// ScriptColorTeamKey value of a selected trigger, colour each token via
// ScriptGroup_Unreachable, move the ScriptColorKey-matching token to the front, then draw a
// billboard per matching token on every gated entity.
//   Cam_Token_EntityGate (0x46AAE0): HasKeyValuePair(ScriptColorTeamKey) && classname is one
//                                    of actor/node/info_volume.
//   Cam_Token_Match (0x46AA80): the entity's ScriptColorTeamKey value strstr-contains the token.
static bool Cam_Token_EntityGate( selbrush_t *b )
{
    entity_s_def *e = (entity_s_def *)b->owner->def;
    if ( !HasKeyValuePair( e, g_PrefsDlg->ScriptColorTeamKey.c_str() ) )
        return false;
    const char *classname = "";
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
        if ( !_stricmp( ep->key, "classname" ) ) { classname = ep->value; break; }
    return strstr( classname, "actor" ) || strstr( classname, "node" )
        || strstr( classname, "info_volume" ) != nullptr;
}
static bool Cam_Token_Match( selbrush_t *b, const char *token )
{
    entity_s_def *e = (entity_s_def *)b->owner->def;
    const char *teamVal = ValueForKey2( e, g_PrefsDlg->ScriptColorTeamKey.c_str() );
    return strstr( teamVal, token ) != nullptr;
}

static void Cam_DrawTokens( const char *teamKeyValue )
{
    char buf[1008];
    strncpy( buf, teamKeyValue, sizeof( buf ) - 1 );
    buf[sizeof( buf ) - 1] = 0;
    if ( !buf[0] )
        return;

    // parse the space-separated tokens; per-token colour from ScriptGroup_Unreachable.
    const int MAX_COLORENTREES = 32;
    char  tokenStr[MAX_COLORENTREES][16];     // 4-dword stride per token (+space)
    float tokenCol[MAX_COLORENTREES][4];
    int   tokens = 0;
    for ( char *t = strtok( buf, " " ); t; t = strtok( nullptr, " " ) )
    {
        iassert( t[0] );
        if ( tokens >= MAX_COLORENTREES )
            break;   // KIWI: the first 32 draw (the binary asserted on map data here)
        _snprintf( tokenStr[tokens], sizeof( tokenStr[tokens] ), "%s ", t );
        tokenStr[tokens][sizeof( tokenStr[tokens] ) - 1] = 0;   // _snprintf may not null-terminate
        // ScriptGroup_Unreachable returns 0..6 (r/b/y/c/g/p/o) or -1 (no match; the binary
        // would read colour [-1]). Guard the never-valid case (defensive divergence).
        int ci = ScriptGroup_Unreachable( t );
        if ( ci < 0 || ci > 6 )
            continue;
        const float *c = kCamTokenColors[ci];
        tokenCol[tokens][0] = c[0]; tokenCol[tokens][1] = c[1];
        tokenCol[tokens][2] = c[2]; tokenCol[tokens][3] = c[3];
        ++tokens;
    }
    if ( tokens <= 0 )
        return;   // KIWI: map data with no colour token - was an assert

    // reorder: bring the token whose text contains the ScriptColorKey to the front (the
    // binary swaps the first match into slot [i]==[last]; here move-to-front of slot 0).
    // KIWI: the last slot is tested too - the swap form's bound skipped it
    const char *colorKey = g_PrefsDlg->ScriptColorKey.c_str();
    for ( int i = 0; i < tokens; ++i )
    {
        if ( strstr( tokenStr[i], colorKey ) )
        {
            if ( i != 0 )
            {
                char  ts[16];   float tc[4];
                memcpy( ts, tokenStr[i], sizeof( ts ) );
                memcpy( tc, tokenCol[i], sizeof( tc ) );
                for ( int k = i; k > 0; --k )
                {
                    memcpy( tokenStr[k], tokenStr[k-1], sizeof( ts ) );
                    memcpy( tokenCol[k], tokenCol[k-1], sizeof( tc ) );
                }
                memcpy( tokenStr[0], ts, sizeof( ts ) );
                memcpy( tokenCol[0], tc, sizeof( tc ) );
            }
            break;
        }
    }

    // for every active + selected gated entity, draw a billboard per matching token.
    for ( int pass = 0; pass < 2; ++pass )
    {
        selbrush_t *head = pass ? &selected_brushes : &active_brushes;
        for ( selbrush_t *b = head->next; b && b != head; b = b->next )
        {
            if ( FilterBrush( b, 0 ) )
                continue;
            if ( !Cam_Token_EntityGate( b ) )
                continue;
            for ( int ti = 0; ti < tokens; ++ti )
                if ( Cam_Token_Match( b, tokenStr[ti] ) )
                {
                    Ed_DrawScriptColorQuad( b->owner->def, tokenCol[ti] );
                }
        }
    }
}

// Defined below (after Cam_Draw); the Cam_Draw tail's 3D-marquee box needs them.
static void CameraCalcRayDir( int y, float *dir, int x );
void Camera_GetRectSelection3D( int x1, int y1, int x2, int y2, float *outPlanes );

// KIWI: the binary's terrain-paint cursor ring (DrawAdvancedTerrainEditCircle, 0x441240) went
// with the AdvPatchEditDlg paint path.
extern void  R_AddCmd_Line3D( short count, char width, GfxPointVertex *verts );  // 0x4FD1A0
extern void  R_AddCmd_Line3DNoDepth( short count, char width, GfxPointVertex *verts );

// 0x406960  DrawBrush_SunPreview  — the sun-preview LIT re-add of one brush list: every brush
// with cullFlag set is DrawBrush'd at technique 26 in its Brush_GetColor2d colour.
// The retail call passes technique 26 in BOTH of DrawBrush's technique slots (draw_meth1 =
// prefab-content/geo, draw_meth2 = xmodel mesh); the port's DrawBrush reduction carries one
// `technique` filling both roles, so a single 26 is exact.
// It must go through DrawBrush, NOT a per-face loop: the technique's state (sunpre_*.tech ->
// stateMap "additive_stencil" -> depthTest EQUAL, depthWrite off, blend Add/InvDestAlpha/One)
// only lights pixels at the EXACT depth the base pass wrote, so the re-add must reuse the same
// faceVis/prefab-content/patch/model surf-cache entries.
static int Cam_DrawBrushList_SunPreview( selbrush_t *head )
{
    int drawn = 0;
    // The sun re-add walks the same prefab tree at technique 26, so it replays the shared
    // recording too.  `backward` because this walk runs through ->prev.
    const bool walkReplay = KiwiWalk_BeginReplay(
        head, (const orientation_t *)world_orient_matrix, /*backward*/ true );
    // The re-add walks the list BACKWARD (seeded from head->prev, advancing by prev), unlike
    // DrawGeneralWorld_'s forward walk.
    for ( selbrush_t *b = head->prev; b && b != head; b = b->prev )
    {
        if ( !b->cullFlag )
            continue;
        GfxColor col;
        Cam_BrushColor2d( b, &col );
        if ( walkReplay )
            KiwiWalk_TopLevel( b );
        DrawBrush( b, (const orientation_t *)world_orient_matrix, /*viewType*/ -1,
                   /*technique*/ TECHNIQUE_SUNLIGHT_PREVIEW, &col,
                   /*width*/ 1, /*drawFlags*/ 0, /*layerPrefix*/ "" );
        ++drawn;
    }
    if ( walkReplay )
        KiwiWalk_EndReplay();
    return drawn;
}

// 0x4069C0  R_SunPrev_Main - the sun-light preview sequence, at the binary's own position
// (Cam_Draw tail, after the main / patch / selected-entity surf flushes):
//   R_SunPrev_SetSunConstants(dir, ambientMul)
//   SetProjection2D
//   R_AddCmdDrawFullScreenColoredQuad(0,0,1,1, ambientMul, mat_white_multiply)
//   R_AddCmdDrawFullScreenColoredQuad(0,0,1,1, colorWhite, rgp.clearAlphaStencilMaterial)
//   shadVol_frontCapIndices = shadVol_quadsPerEdge = (dir.w != 0 ? 6 : 3)   // sun -> 3
//   SunLightPreview_BrushShadow(active) / (selected) + ..._PolyOffsetShadows
//   R_SortMaterials ; DrawBrush_SunPreview(selected) ; DrawBrush_SunPreview(active)
//   R_AddEditorSurfsCmd
// The pass re-draws the world depth-EQUAL and ADDS sun light scaled by INVERSE dest alpha
// (raw/techniques/sunpre_r0c0n0s0.tech -> statemap additive_stencil: blend Add/InvDestAlpha/
// One, depthTest Equal, depthWrite Disable, stencil OneSided Equal Keep/Keep/Keep).  The
// shadow volumes mark shadowed pixels in dest alpha + stencil; the clear_alpha_stencil quad
// zeroes both first.  ORDER IS LOAD-BEARING: everything that should darken must already be
// RASTERISED (not merely queued in the surf cache) before the multiply quad, and nothing may
// write depth or alpha between the clear quad and the re-add.

// KIWI divergence (sun-preview fix): the finite extrusion distance for the sun shadow
// volumes = the active+selected map AABB diagonal + margin.  A caster anywhere inside
// that box reaches past every possible in-bounds receiver along any sun direction, so
// shadows land exactly as before — they just STOP past the scene instead of smearing to
// infinity (the visible artifact on open/alpha model meshes like foliage).
static float Cam_SunPrev_ShadowExtrudeDistance()
{
    selbrush_t *heads[2] = { &active_brushes, &selected_brushes };
    float mins[3] = { 0.0f, 0.0f, 0.0f };
    float maxs[3] = { 0.0f, 0.0f, 0.0f };
    bool any = false;

    for ( int list = 0; list < 2; ++list )
    {
        for ( selbrush_t *b = heads[list]->next; b && b != heads[list]; b = b->next )
        {
            brush_t *def = b->def;
            if ( !def )
                continue;
            for ( int axis = 0; axis < 3; ++axis )
            {
                if ( !any || def->mins[axis] < mins[axis] ) mins[axis] = def->mins[axis];
                if ( !any || def->maxs[axis] > maxs[axis] ) maxs[axis] = def->maxs[axis];
            }
            any = true;
        }
    }

    if ( !any )
        return 1024.0f;
    const float dx = maxs[0] - mins[0];
    const float dy = maxs[1] - mins[1];
    const float dz = maxs[2] - mins[2];
    return sqrtf( dx * dx + dy * dy + dz * dz ) + 64.0f;
}

static void Cam_SunPrev_Main( bool faithfulSun, Material *sunMultiplyMat,
                              const float litSunDir[3], const float litAmbientMul[3],
                              const float litSunColor[3], bool litHaveSun )
{
    PROF_SCOPED("Cam_SunPrev_Main");

    if ( !faithfulSun )
        return;

    // R_SunPrev_Main's FIRST statement is R_SunPrev_SetSunConstants and the
    // WHOLE sequence is gated on its non-zero return: no sun key means no quads, no volumes, no
    // re-add.  Setting the constants here rather than earlier matters — emitted before the world
    // draw they re-shade every material whose technique samples them, and additive blending can
    // never bring those surfaces back up.
    if ( !litHaveSun )
        return;
    {
        // The same three RC_SET_CUSTOM_CONSTANT commands with the same values, emitted from
        // what the edit-epoch cache's `emit=false` call computed.
        // The worldspawn sun round trip is not repeated for camera-only frames.
        R_AddCmdSetCustomShaderConstant( CONST_SRC_CODE_SUN_POSITION,
                                         litSunDir[0], litSunDir[1], litSunDir[2], 0.0f );
        R_AddCmdSetCustomShaderConstant( CONST_SRC_CODE_SUN_DIFFUSE,
                                         litSunColor[0], litSunColor[1], litSunColor[2], 1.0f );
        R_AddCmdSetCustomShaderConstant( CONST_SRC_CODE_SUN_SPECULAR,
                                         litSunColor[0], litSunColor[1], litSunColor[2], 1.0f );
    }

    // KIWI-UX: XModel techsets normally own slot 26.  The rare missing-slot fallback uses
    // FAKELIGHT_NORMAL for N.L and this packed worldspawn sun colour instead of flat grey.
    float modelSunTint[4] = {
        litSunColor[0], litSunColor[1], litSunColor[2], 1.0f
    };
    Byte4PackPixelColor( modelSunTint, &s_sunModelFallbackColor );

    // The binary sets sundir.w = 0 unconditionally, so the shadVol counts are always the
    // directional 3 (its w != 0 -> 6 arm can never fire).
    float sun[4]        = { litSunDir[0], litSunDir[1], litSunDir[2], 0.0f };
    float ambientMul[3] = { litAmbientMul[0], litAmbientMul[1], litAmbientMul[2] };

    // The two full-screen quads framing the lit pass: black-world multiply
    // (mat_white_multiply * the worldspawn ambient, dropping the rasterised world to its
    // ambient/diffuse floor) then clear-stencil (zeroes dest alpha + stencil for the volumes).
    // SetProjection2D is for the quads; the 3D projection is restored immediately after.
    {
        R_AddCmdProjectionSet2D();
        if ( sunMultiplyMat )
        {
            float blackCol[4] = { ambientMul[0], ambientMul[1], ambientMul[2], 1.0f };
            R_AddCmdDrawFullScreenColoredQuad( 0.0f, 0.0f, 1.0f, 1.0f, blackCol, sunMultiplyMat );
        }
        if ( rgp.clearAlphaStencilMaterial )
        {
            static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            R_AddCmdDrawFullScreenColoredQuad( 0.0f, 0.0f, 1.0f, 1.0f, white, rgp.clearAlphaStencilMaterial );
        }
        R_AddCmdProjectionSet3D();
    }

    // The shadow volumes.  The binary ran frontCapIndices =
    // quadsPerEdge = 3: the directional (w==0) case, where every extruded vertex is the
    // same point at infinity, the back cap degenerates, and one front-cap tri per
    // silhouette tri is all that is kept.
    // KIWI divergence: sun volumes are now extruded a FINITE distance (scene AABB diagonal
    // + margin) so an open/seam edge on a model caster no longer drags a shadow wall across
    // the whole map ("model shadows extend forever").  Finite twins make the back cap and
    // full side quads real geometry, so the sun path passes 6 like point lights do.
    // BrushShadow recurses into PREFAB CONTENTS with the composed placement orientation;
    // PolyOffsetShadows emits the silhouette side quads from the edge hash and draws the volume
    // through rgp.stencilShadowMaterial (colorWrite off, depthWrite off, depthTest LESS, cull
    // NONE, stencil two-sided z-fail).  Shadowed pixels end with stencil != 0, which the
    // additive_stencil re-add (stencil EQUAL ref 0) then skips.
    extern bool Radiant_ShadVol_Begin( int frontCapPerTri, float directionalExtrudeDistance );
    extern void Radiant_ShadVol_BrushShadow( selbrush_t *listHead, const orientation_t *orient,
                                             const float *light );
    extern void SunLightPreview_PolyOffsetShadows();
    if ( Radiant_ShadVol_Begin( 6, Cam_SunPrev_ShadowExtrudeDistance() ) )  // KIWI finite extrusion
    {
        // The two BrushShadow calls cover the WHOLE map — a caster behind the camera can still
        // shadow what is in front of it.  Their tree traversal, material eligibility, and
        // XModel extraction all ride the shared edit-epoch caches.
        const orientation_t *worldOr = (const orientation_t *)world_orient_matrix;
        Radiant_ShadVol_BrushShadow( &active_brushes,   worldOr, sun );
        Radiant_ShadVol_BrushShadow( &selected_brushes, worldOr, sun );
        SunLightPreview_PolyOffsetShadows();

        // Name the live cost and the cache boundary once.  Camera movement never changes the
        // shared edit epoch, so it rebuilds none of the cached caster inputs.
        static bool s_sunCostReported = false;
        if ( !s_sunCostReported && g_svTrisFed > 0 )
        {
            s_sunCostReported = true;
            Sys_Printf( "Sun light preview ON: %d shadow casters, %d silhouette triangles, "
                        "%d volume batches.  Caster walks, material eligibility, and XModel "
                        "extraction are edit-epoch cached; camera motion invalidates none.  "
                        "View > \"Preview sun as well\" turns it off.\n",
                        g_svCastersDrawn, g_svTrisFed, g_svBatches );
        }
    }

    // The LIT re-add.  R_SortMaterials demarcates the flush so it carries
    // ONLY the tech-26 surfs; re-flushing an earlier pass would re-rasterise it on top of the
    // darkened frame.
    {
        // A SECOND FULL WORLD DRAW — DrawBrush over both lists at technique 26, so the prefab
        // walk, the layer-vis refresh and the model-surf queueing all run again.
        R_SortMaterials();
        Cam_DrawBrushList_SunPreview( &selected_brushes );
        Cam_DrawBrushList_SunPreview( &active_brushes );
        R_AddEditorSurfsCmd();
    }
}

// 0x407dc0  Cam_Draw.  `hwnd` fed only the removed terrain-paint cursor ring (it is unused);
// everything draws from g_camwndState.
void CamWnd_Draw( HWND hwnd )
{
    camwndState_t *cam    = &g_camwndState;
    camera_s      &camera = cam->camera;

    PROF_SCOPED( "CamWnd_Draw" );

    if ( !active_brushes.next )      // brush lists not bootstrapped → no map loaded
        return;

    if ( !CamWnd_SetupScene() )      // degenerate projection: skip geometry this frame, the
        return;                      // cleared background stands

    // the per-frame memo in front of Cam_EditorMaterialColor's twelve
    // strstr calls.  Reset HERE, before any pass gathers, so no entry can outlive one frame.
    Cam_EditorMaterialColorMemoReset();

    // KIWI-UX: cross-patch normal weld (pmesh.cpp) — average tessellated normals at
    // coincident patch-boundary verts so sewn terrain patches shade seamlessly, the
    // way cod4map's SmoothVertexNormalsForGroup does at compile time.  BEFORE the
    // world pass: a bumped def version makes the lazy Patch_Fill_SyncVersion rebuild
    // pick up the welded normals in this same frame.  Stamp-gated — an unedited map
    // costs one list walk.
    {
        extern void KiwiPatchWeld_Run();   // pmesh.cpp
        KiwiPatchWeld_Run();
    }

    g_svNodesWalked = g_svCastersDrawn = 0;
    g_svTrisFed     = g_svTrisKept     = 0;
    g_svBatches     = g_svBatchKB      = 0;

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BM, ITEM 1b) — THE SECTION-ANALYSIS CLIP PLANE OPENS HERE
    // ═════════════════════════════════════════════════════════════════════════
    // A D3D9 user clip plane around the WORLD passes only.  It is emitted as a
    // render command (RC_SET_CLIP_PLANE) because this draw builds a deferred
    // command buffer — a SetClipPlane call at this line would land on whatever the
    // device happened to be doing, not on these draws.  The transform contract and
    // the derivation are on GfxCmdSetClipPlane (r_rendercmds.h); the plane itself
    // and the "is a section on" gate are kiwi_section.cpp's.
    //
    // THE PAIR IS UNCONDITIONALLY MATCHED: there is no `return` anywhere between
    // this line and the closing emit before the overlay block, so a frame can never
    // leave the clip state enabled for the ImGui overlay or a 2D view.
    // KiwiSection_EmitClipBegin emits nothing at all when no section is armed.
    extern void KiwiSection_EmitClipBegin();   // kiwi_section.h
    extern void KiwiSection_EmitClipEnd();     // kiwi_section.h
    KiwiSection_EmitClipBegin();

    const int layer = g_qeglobals.current_edit_layer;

    // Two sun-preview paths: the binary's R_SunPrev_Main sequence (black-world multiply quad +
    // clear-stencil quad + stencil shadow volumes + the cached SUNLIGHT_PREVIEW(26) lit draw),
    // or, when the "white_multiply" material is missing (without the black-world reset the lit
    // pass would double-brighten), a port APPROXIMATION: each world face flat-tinted by
    // suncolor*(ambient + diffuse*N.L) through MATERIAL_COLOR under UNLIT - directional shading
    // with no shadows and no per-pixel lighting.  The game-lighting path is the
    // persisted default; View -> Preview sun as well can disable it.
    const bool sunPrev = Cam_SunPrevEnabled();
    const CamSunPreviewCache *sunState = sunPrev ? Cam_SunPrev_CachedState() : nullptr;
    const bool sunActive = sunState && sunState->active;   // sun key present in this map
    const bool useCache  = Cam_SurfCacheEnabled();

    extern Material *Radiant_GetWhiteMultiplyMaterial();   // shadowvolume.cpp
    Material *sunMultiplyMat = ( sunActive || g_PrefsDlg->enable_light_preview )
                              ? Radiant_GetWhiteMultiplyMaterial() : nullptr;
    const bool faithfulSun   = sunActive && sunMultiplyMat != nullptr;
    // On the R_SunPrev_Main path the world loop draws TEXTURED (not the N.L tint): the multiply
    // quad darkens that textured world and the cached lit pass then adds the sun on top.
    const bool sunApprox = sunActive && !faithfulSun;      // the per-face N.L MATERIAL_COLOR tint
    // Tell the shared geometry builder whether the per-vertex sun bake applies.
    // Face_BuildLayerGeom multiplies every cached face colour by suncolour*(ambient +
    // diffuse*N.L) when g_edSun.active.  That bake belongs to the APPROXIMATION path alone: on
    // the R_SunPrev_Main path it pre-darkens the world before the multiply quad, and the
    // additive tech-26 re-add can never brighten it back.
    {
        extern int g_edSunBakeVertColor;                   // brush.cpp
        const int bake = sunApprox ? 1 : 0;
        // The bake lives IN the built per-vertex colours, so a mode change leaves STALE baked
        // verts in every realized face cache; force the display rebuild (BrushList_RebuildDisplay - the same
        // def-version bump + patch re-tess every layer change makes) on a transition.
        static int s_prevBake = -1;
        if ( bake != s_prevBake )
        {
            s_prevBake = bake;
            BrushList_RebuildDisplay( &active_brushes );
            BrushList_RebuildDisplay( &selected_brushes );
            BrushList_RebuildDisplay( &filtered_brushes );
        }
        g_edSunBakeVertColor = bake;
    }
    // Sun state pre-computed here and carried to the shadow-vol / quad / lit-pass block below.
    float  s_litSunDir[3]    = { 0.0f, 0.0f, 0.0f };
    float  s_litSunColor[3]  = { 1.0f, 1.0f, 1.0f };
    float  s_litAmbientMul[3]= { 0.0f, 0.0f, 0.0f };
    bool   s_litHaveSun      = false;
    static bool s_camSunMsg  = false;   // one-shot console message naming the sun path
    if ( !s_camSunMsg )
    {
        s_camSunMsg = true;
        if ( faithfulSun )
            Sys_Printf( "Sun light preview: faithful (R_SunPrev_Main).\n" );
        else if ( sunApprox )
            Sys_Printf( "Sun light preview: approximation (per-face directional N.L shading, no "
                        "shadows) - no \"white_multiply\" material for the faithful path.\n" );
    }

    // Values only - EMISSION happens in Cam_SunPrev_Main at the binary's own position
    // (after the world draw).  The expensive parse is cached by edit epoch above.
    if ( faithfulSun && sunState && sunState->haveConstants )
    {
        memcpy( s_litSunDir,     sunState->dir,        sizeof(s_litSunDir) );
        memcpy( s_litSunColor,   sunState->color,      sizeof(s_litSunColor) );
        memcpy( s_litAmbientMul, sunState->ambientMul, sizeof(s_litAmbientMul) );
        s_litHaveSun = true;
        // NOTE: exactly THREE constants here - the binary never
        // touches CONST_SRC_CODE_ENVMAP_PARMS on this path.
    }
    // World technique from camera.draw_mode; default draw_mode 1 -> TECHNIQUE_UNLIT.
    const MaterialTechniqueType worldTech = Cam_TechForDrawMode( camera.draw_mode );

    // Cam_Draw tints SELECTED brushes (R_SetMaterialColor(colors[11]) + DrawBrush,
    // gated on !dontDrawSelectedTint) and outlines them in white (the !dontDrawSelectedOutlines
    // tech-29 loop).  Both run as immediate-mode passes below.
    float lastMC[4] = { -1.0f, -1.0f, -1.0f, -1.0f };  // emit MATERIAL_COLOR only on change
    s_sunMCEmitted = 0;                                // reset the sun-preview critical-cmd budget
    // World fill = active_brushes ONLY, matching DrawGeneralWorld_: SELECTED brushes are drawn
    // separately by the red-tint pass below, so drawing them here too would z-fight that fill.
    // ═════════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND S) — THE §17 GRID NOW DRAWS *BEFORE* THE WORLD.
    // ═════════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: "grid should never render on top of shapes".
    //
    // WHY THE OLD POSITION COULD NOT GUARANTEE THAT.  Round O established (see
    // Cam_MaterialIsMissing above, and the asset decode next to it) that a face whose
    // material failed to load draws through $default: techSet "2d" -> statemap
    // default2d.sm, whose depthTest AND depthWrite rules are both an unconditional
    // "Disable".  Such a face neither tests nor writes depth.  With the grid drawn
    // AFTER the world, "does the world hide the grid?" was therefore a question about
    // the world's material state — and for every scene that still has a missing
    // material the answer was no, whatever the grid did.  Round O's $default3d
    // substitution fixes the depth state where the substitute asset exists; it cannot
    // fix an asset set that does not ship one, and it cannot fix a legitimately
    // depth-write-free world material.
    //
    // PAINTER'S ORDER MAKES THE QUESTION GO AWAY.  Every world / entity / patch /
    // selected pass below submits AFTER this one, so a face that ignores depth simply
    // paints over the grid.  That is true for any material state whatsoever, which is
    // the property the report asks for.
    //
    // THE DEPTH STATE, CHECKED IN THE ASSETS RATHER THAN ASSUMED.  KiwiLines_* emits
    // through R_AddCmd_Line3D -> RB_DrawLines3D(depthTest=true) -> rgp.lineMaterial,
    // i.e. main/materials/$line: refStateBits[0] = 0x08128812 (blendOp field
    // (>>8)&7 == 0 = Disable), refStateBits[1] = 0x0000000d = GFXS1_DEPTHWRITE |
    // GFXS1_DEPTHTEST_LESSEQUAL, techSet "tools" -> vertcol_shaded_tools ->
    // stateMap "default".  default.sm passes depthTest through and forces depthWrite
    // Enable for blendOp == Disable, so the bound state IS depthTest LESSEQUAL +
    // depthWrite ON.  So the grid does write depth here.  It still cannot occlude
    // world geometry, in either direction:
    //   * a world pixel NEARER than the grid line passes LESSEQUAL and covers it;
    //   * a world pixel at the SAME depth (a floor brush lying on Z=0 — the common
    //     case, and the one the old order got wrong) also passes LESSEQUAL, because
    //     it is submitted LATER and LESSEQUAL admits ties.  The world wins ties now;
    //     with the grid drawn last it used to;
    //   * a world pixel strictly BEHIND a grid line is rejected — but that is correct
    //     occlusion by a line that really is in front, and it is pixel-identical to
    //     what the old post-world position already produced.
    //
    // WHY NOT R_AddCmd_Line3DNoDepth.  It exists (r_rendercmds.cpp:2042, dimension 4
    // -> RB_DrawLines3D(depthTest=false) -> rgp.lineMaterialNoDepth = the registered
    // built-in "$line_nodepth", r_material.cpp:225) and it is the WRONG tool here.
    // main/materials/$line_nodepth carries refStateBits[1] = 0x00000002 =
    // GFXS1_DEPTHTEST_DISABLE with DEPTHWRITE clear — but its refStateBits[0] is the
    // same 0x08128812 and its techSet is the same "tools", so the same default.sm
    // depthWrite rule forces the write back ON.  The bound state is therefore depth
    // test OFF, depth write ON: it would paint over everything AND stamp depth,
    // i.e. exactly backwards for a pre-world pass.  There is no shipped line material
    // with depthWrite disabled, so $line it is, and the argument above is what makes
    // that safe rather than a compromise.
    //
    // POSITION: after CamWnd_SetupScene (the view exists), before the
    // first R_SortMaterials.  The grid is an IMMEDIATE RC_DRAW_LINES command, not an
    // editor surf, so it neither opens nor disturbs the surf accumulation the
    // R_SortMaterials pairs demarcate.  It leaves MATERIAL_COLOR at its last line
    // colour (Ed_EmitLineBatch pushes one per colour run); every world arm below
    // re-emits its own (lastMC starts at {-1,-1,-1,-1}, and the cached path pushes the
    // neutral before its flush), so nothing downstream inherits a grid grey.
    {
        extern void KiwiGrid_Draw();      // kiwi_grid.cpp
        KiwiGrid_Draw();
    }

    // R_SortMaterials opens the world accumulation so the main R_AddEditorSurfsCmd
    // flush is demarcated and the later selected/white flushes do not re-draw these surfs.
    R_SortMaterials();
    // The GATHER half of DrawGeneralWorld_ -> R_AddEditorSurfsCmd: a face is APPENDED here and
    // DRAWN in the sorted loop below.  Function-static (no per-frame realloc); CamWnd_Draw is
    // not re-entrant, so one buffer is enough.
    static std::vector< CamSortFace > s_worldFaces;
    s_worldFaces.clear();
    selbrush_t *bhead = &active_brushes;
    {
    PROF_SCOPED( "world gather" );
    for ( selbrush_t *b = bhead->next; b != bhead; b = b->next )
    {
        brush_t *def = b->def;
        if ( !def )
            continue;
        // DrawGeneralWorld_ gates every world brush on FilterBrush.  Load-bearing:
        // without it the map's own lightgrid_volume worldspawn brush renders as opaque teal
        // planes z-fighting the coplanar floor.
        if ( FilterBrush( b, 0 ) )
            continue;
        // KIWI-UX: restore the binary's ONE active-world accumulation.  DrawGeneralWorld_
        // opens it at 0x407AB9, sends patches through DrawBrush at 0x407B9D (whose sole
        // instance-side patch dispatch is 0x47B018), and closes it at 0x407BB9.  The port's
        // later patch-only range made it a later painter-order range (observable at equal depth
        // or after an unsafe pass state) and kept it outside the main cache/run validation.
        if ( b->patch )
        {
            GfxColor pcol;
            Cam_BrushColor2d( b, &pcol );
            DrawBrush( b, (const orientation_t *)world_orient_matrix, /*viewType*/ -1,
                       (int)worldTech, &pcol, /*width*/ 1, /*drawFlags*/ 0,
                       /*layerPrefix*/ "" );
            continue;
        }
        // FIXEDSIZE POINT ENTITIES ARE NOT WORLD FACES.  DrawBrush routes any brush
        // whose eclass is fixedsize down DrawModels (model/prefab contents, no bbox) or, for a
        // model-less one, DrawGeo at the entity-local INVERSE orientation - never as filled
        // world faces at the world orientation.  The entity pass below handles them; drawing
        // their placeholder-bbox faces here renders an opaque box straddling the xmodel.
        {
            entity_s_def *eDef = b->owner ? (entity_s_def *)b->owner->def : nullptr;
            eclass_t     *ec   = eDef ? eDef->eclass : nullptr;
            if ( ec && *(int *)&ec->fixedsize )
                continue;
        }
        // The per-instance faceVis sync DrawBrush runs for every brush it draws
        // (brush.cpp:7438).  VERSION-GATED: a compare and a return on an unedited brush.
        if ( Cam_WorldGeoResident() )
            Brush_CheckBuildFaceVis( b, (const float *)world_orient_matrix );
        for ( int fi = 0; fi < def->faceCount; ++fi )
        {
            face_t   *f   = &def->faces[fi];
            if ( !f->w )
                continue;
            Material *mtl = FaceMaterial( &f->mtldef[layer] );
            if ( !mtl )
                continue;
            Cam_GatherFace( s_worldFaces, f, mtl, &f->mtldef[layer],
                            /*classifySky*/ !sunApprox && !useCache, /*uvOwns*/ false,
                            /*inst*/ b, /*faceIndex*/ fi );
        }
    }
    }

    // R_AddEditorSurfsCmd's qsort, on the immediate stream.  Within one key the
    // order is unchanged (Cam_SortFaceCompare's `seq` tie-break).
    {
        Cam_SortGatheredFaces( s_worldFaces );
    }

    // The SUBMIT half.  Per face-layer this rebuilds the geometry (Face_BuildLayerGeom) and
    // emits ONE RC_DRAW_TRIANGLES, i.e. one draw call per face-layer (rb_backend.cpp:1445).
    {
    PROF_SCOPED( "world submit" );
    s_deferredToolFaces.clear();
    for ( size_t wi = 0; wi < s_worldFaces.size(); ++wi )
    {
        const CamSortFace &ent = s_worldFaces[wi];
        face_t   *f   = ent.f;
        Material *mtl = ent.mtl;
        {
            if ( sunApprox )
            {
                // UNLIT * per-face directional-sun MATERIAL_COLOR, deduped via lastMC.
                Cam_DrawFaceSun( f, mtl, TECHNIQUE_UNLIT, lastMC );
            }
            // faithfulSun draws the world TEXTURED through the DEFAULT branch below; the
            // multiply quad then darkens it before the cached lit draw adds the sun.
            else if ( useCache )
                Cam_DrawFaceCached( f );                          // RADIANT_SURFCACHE
            else
            {
                // DEFAULT: split world vs tool/sky surfaces.
                //  - WORLD materials -> real texdef texcoords + world normals, drawn with the
                //    draw_mode technique where the material's techset carries it, else UNLIT.
                //  - TOOL/SKY materials -> UNLIT + flat MATERIAL_COLOR.  Tool materials have no
                //    editor colormap and the FAKELIGHT pixel shader (vertcol_shaded) ignores
                //    MATERIAL_COLOR, so a uniform FAKELIGHT would render them flat grey.
                const char *mn = f->mtldef[layer].radMtl ? f->mtldef[layer].radMtl->name : nullptr;
                // ── KIWI-UX (ROUND BC, ITEM 3): THE SKY TESTS SIT ABOVE THE SPLIT ───
                // Two facts put them here rather than eleven lines down, inside the
                // tool/sky arm:
                //   * MATERIAL_COLOR is emitted BEFORE the arm is chosen, and its .w is
                //     a FLAT-COLOUR OVERRIDE in every editor pixel shader that reads the
                //     constant:
                //         rgb = lerp( sample(colorMap) * vColor, materialColor.rgb, .w )
                //     Cam_EditorMaterialColor seeds `out[3] = 1.0f` (:614 initialises it
                //     to all-ones and only rgb is overwritten from the table), so its
                //     `{ "sky", 0.45f, 0.62f, 0.92f }` row (:621) paints every sky face
                //     solid (115,158,235) unless `isSky` is known up here.
                //   * the see-through arm needs a different DRAW, not just a different
                //     material, so it cannot be one term in the substitution below.
                //
                // WHAT THE SHIPPED ASSETS SAY, decoded rather than assumed
                // (main/materials/sky_aftermath, 116 bytes):
                //     techSetName "sky"   textureCount 1   refStateBits {0x08128812,0x0D}
                //     textureTable[0] = "colorMap", semantic 2 (TS_COLOR_MAP),
                //                       image "aftermath_ft"
                //     main/images/aftermath_ft.iwi: flags 0xC6 -> IMG_FLAG_CUBEMAP (0x04),
                //                       1024x1024 DXT1, six faces  => MAPTYPE_CUBE
                //     main/techsets/wc_sky.techset: "unlit" (and every lit/fakelight slot)
                //                       -> technique `sky`
                //     main/techniques/sky.tech: stateMap "color_only",
                //                       pixelShader 2.0 "sky.hlsl" { skyMapSampler =
                //                       material.colorMap; }, vertex.position =
                //                       code.position
                // That technique samples the CUBEMAP by a direction it computes itself
                // and declares NO materialColor — so the w == 0 emitted below is a NO-OP
                // for a genuine sky material, and is load-bearing only for a "sky"-NAMED
                // material that resolves to a lerping shader (techset "tools" ->
                // vertcol_shaded_tools).  main/statemaps/color_only.sm is depthTest
                // LessEqual, depthWrite Disable, blendFunc Disable/One/Zero: sky PAINTS
                // WITHOUT OCCLUDING, which is why a near sky wall can erase the ground
                // lattice submitted at :2627 (before the world) while the world faces
                // flushed after it still draw over the sky normally.
                //
                // THE PREDICATE IS `KiwiSky_IsSkyMaterial` OR'd with the shipped leaf-
                // name test rather than replacing it: SURF_SKY is the canonical mark
                // (kiwi_skybox.h D-AZ-A), so a `desert_backdrop` carrying it has to reach
                // this arm, and the name test stays OR'd in so nothing that rendered as
                // sky before stops doing so.
                //
                // AND THE SEE-THROUGH TEST IS PER FACE.  No camera-POSITION test can
                // answer it: KIWI's orbit rig leashes the eye s_dist behind the pivot, so
                // "eye inside the shell" is false at every working zoom, and under the
                // ortho default "pivot inside the shell" is true whenever the user is
                // working — each spelling is a constant in practice, and the second one
                // is what erased the grid.  The per-face question has the same answer
                // from either side of the shell: is this face between the eye and the
                // pivot.  kiwi_skybox.h D-BC-A.  (The selected-brush pass at :2982-2999
                // never consults it at all — it draws the face's OWN material at
                // TECHNIQUE_UNLIT — which is why a selected sky brush is always
                // textured.)
                // The predicate and the centroid rule moved into Cam_GatherFace: the see-through
                // answer decides this face's SORT BUCKET, which must be known before the order is.
                const bool isSky         = ent.isSky;
                const bool skySeeThrough = ent.skySeeThrough;
                float ecol[4];
                if ( Cam_EditorMaterialColor( mn, ecol ) || isSky )  // tool/sky/volume -> UNLIT + flat colour
                {
                    // ── KIWI-UX (ROUND BA): BOTH SKY ARMS WANT w == 0. ───────────────
                    // TEXTURED SKY (the toggle off, or a face at or beyond the pivot)
                    // wants the neutral the world arm uses, `{1,1,1,0}`: w = 0 hands the
                    // pixel to the material's own technique, which for a sky material is
                    // the cubemap-sampling `sky` technique above.  Even where a
                    // "sky"-NAMED material resolved to a lerping shader instead, w = 0
                    // shows its texture rather than the paint.
                    // SEE-THROUGH SKY wants `{0,0,0,0}` — the binary's own translucent-fill
                    // neutral (Cam_DrawSelectedFaceFill :1529, R_SetMaterialColor(NULL)) —
                    // because that arm carries its colour AND its alpha in the per-vertex
                    // colour instead, see the film below.
                    if ( isSky )
                    {
                        ecol[0] = ecol[1] = ecol[2] = skySeeThrough ? 0.0f : 1.0f;
                        ecol[3] = 0.0f;
                    }
                    // ══ KIWI-UX (ROUND BH, ITEM 4) — CAULK ONLY DREW WHEN SELECTED ══
                    // USER REPORT, verbatim: *"Caulk only draws when the object is
                    // highlighted.  Fix this."*  (Screenshots: a brush with caulk faces
                    // shows the tiled 'Caulk' editor texture while SELECTED and a flat
                    // tan surface when not.)
                    //
                    // THE ASSET, DECODED (main/materials/caulk, 174 bytes, MaterialRaw):
                    //     techSetName "tools"      refStateBits {0x08128812, 0x0000000d}
                    //     textureTable[0] = "colorMap", semantic 2 (TS_COLOR_MAP),
                    //                       image "caulk"   -> main/images/caulk.iwi,
                    //                       which IS the tiled 'Caulk' label art
                    //     textureTable[1] = "normalMap" -> "$identitynormalmap"
                    //     constantTable[0] = "colorTint" {1,1,1,1}
                    // main/techsets/wc_tools.techset maps "unlit" -> vertcol_shaded_tools,
                    // and main/techniques/vertcol_shaded_tools.tech is stateMap "default",
                    // pixelShader 2.0 "vertcol_shaded.hlsl" { colorMapSampler =
                    // material.colorMap; } with vertex.texcoord[0] = code.texcoord[0].
                    // So caulk DOES carry real editor art and the normal pass DOES bind a
                    // colormap-sampling technique.  Candidate (a) — "the tool techset has
                    // no sampling arm" — is therefore FALSE, and so is candidate (c): no
                    // substitution happens here (losesDepth is true for caulk, so `flat`
                    // is already the face's own material).
                    //
                    // THE MECHANISM IS THE FLAT-PAINT BOMB, candidate (b).  This file's
                    // Cam_EditorMaterialColor (:612) seeds `out[3] = 1.0f` and its
                    // `{ "caulk", 0.55f, 0.50f, 0.42f }` row (:622) only overwrites rgb —
                    // so MATERIAL_COLOR reaches vertcol_shaded with .w == 1 and
                    //     rgb = lerp( sample(colorMap) * vColor, matColor.rgb, 1 )
                    //         = (0.55, 0.50, 0.42)   == the reported flat tan.
                    // The SELECTED pass (:2982-3000) escapes it twice over: it emits
                    // d_savedinfo.colors[11] = {1, 0.25, 0.25, 0.25} (win_qe3.cpp:424), i.e.
                    // .w == 0.25 — mostly texture, tinted red — and it draws through
                    // Cam_DrawFaceFaithfulImmediate, which carries the face's REAL texdef
                    // ST out of Face_BuildLayerGeom instead of Cam_DrawFace's placeholder
                    // 1/128 planar projection (:1137-1145).  Both halves are needed here:
                    // .w == 0 alone would show the caulk art at a fixed world scale that
                    // ignores the texdef the Fit in ROUND BH ITEM 2 just computed.
                    //
                    // THE CLASS THIS TOUCHES IS EXACTLY THE TWO SOLID TOOL MATERIALS, and
                    // it is selected by the PROPERTY, not by a name list: `losesDepth` is
                    // already "this material writes depth at UNLIT and d_white does not",
                    // which the shipped set answers TRUE for caulk and nodraw
                    // (refStateBits[1] 0x0d) and FALSE for clip / trigger / origin / hint /
                    // skip / portal / lightgrid_volume / aa_default / white_tools (0x0c).
                    // So the round-AZ/BA see-through TOOL-VOLUME route through d_white is
                    // untouched, no statemap changes, and caulk/nodraw keep the depth write
                    // round M restored.  The only visible change is that caulk and nodraw
                    // now wear their own editor art unselected, which is what the user asked
                    // for and is desirable for both (nodraw's colorMap is the matching
                    // 'Nodraw' label art, main/images/nodraw.iwi).
                    //
                    // HOISTED, not duplicated: `losesDepth` used to be computed below,
                    // AFTER the colour emit.  The predicate has no side effects, and both
                    // the colour and the draw now need it, so it moved up here and the
                    // draw arm below reads this one.
                    const bool losesDepth = Cam_MaterialWritesDepth( mtl, TECHNIQUE_UNLIT )
                                     && !Cam_MaterialWritesDepth( g_qeglobals.d_white, TECHNIQUE_UNLIT );
                    const bool solidTool  = !isSky && losesDepth;
                    if ( solidTool )
                    {
                        // The world arm's own neutral (:2951): w == 0 hands the pixel to
                        // the material's technique, which for caulk samples caulk.iwi.
                        ecol[0] = ecol[1] = ecol[2] = 1.0f;
                        ecol[3] = 0.0f;
                    }
                    // ── KIWI-UX end ─────────────────────────────────────────────────
                    // KIWI (2026-10-13, user: "clips aren't rendering"): the see-through
                    // volumes (clip, trigger, hint...) and the sky film write no depth, and
                    // the resident world faces draw LATER, at the main surf flush - so the
                    // world painted over them.  They draw after that flush instead.
                    if ( g_qeglobals.d_white && ( skySeeThrough || ( !isSky && !losesDepth ) ) )
                    {
                        // KIWI (2026-10-13, user: clip_player "shows as pink when not
                        // highlighted"): a volume wears its own label art at the neutral
                        // w == 0, exactly as the selected pass draws it, not the flat paint.
                        static const float s_artNeutral[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
                        CamDeferredToolFace d;
                        d.f     = f;
                        d.film  = skySeeThrough;
                        // KIWI (2026-10-24, user: "make lightgrid textures transparent like
                        // they are in real cod radiant"): Cam_DrawMaterial swaps tool art for
                        // its opaque SrcAlpha/Zero twin (art over black, depth-writing), so a
                        // volume around a building hid it.  This pass runs after the world
                        // flush, so lightgrid blends its own art (DXT3 alpha ~60%) instead.
                        d.ownBlend = mn && KiwiStr_ContainsNoCase( mn, "lightgrid" );
                        memcpy( d.col, skySeeThrough ? ecol : s_artNeutral, sizeof( d.col ) );
                        d.inst      = ent.inst;
                        d.faceIndex = ent.faceIndex;
                        s_deferredToolFaces.push_back( d );
                        continue;
                    }
                    if ( Cam_MaterialColorChanged( ecol, lastMC ) )
                    {
                        R_AddCmdSetMaterialColor( ecol );
                        Cam_SetLastMaterialColor( lastMC, ecol );
                    }
                    // Tool materials draw with the FLAT WHITE material, not their own: some DO
                    // carry a colormap (light_grid_volume bakes its name into one) and sampling it
                    // at the placeholder 1/128 planar scale stretches that text over the brush.
                    // EXCEPT sky, whose colormap is real content the editor should show.
                    // ── KIWI-UX (ROUND M): ...AND EXCEPT WHEN IT WOULD COST THE DEPTH WRITE.
                    // The full diagnosis is on Cam_MaterialWritesDepth above.  In one line:
                    // white_tools is alpha-blended and therefore depthWrite-DISABLED, caulk and
                    // nodraw are opaque and depthWrite-ENABLED, and swapping the first in for the
                    // second turned every cut face into a surface that occludes nothing.
                    //
                    // The test is on the PROPERTY, not on a name list, so it keeps working if the
                    // asset set changes: substitute only when the face's own material does not
                    // write depth, or when d_white does.  In practice that leaves the see-through
                    // TOOL VOLUMES (clip / trigger / hint / skip / portal / origin /
                    // lightgrid_volume — all 0x08128965 like white_tools, i.e. already blended and
                    // already depth-write-free) going through d_white exactly as before, and takes
                    // only the two SOLID tool materials out of the substitution.
                    // ── SUPERSEDED IN PART BY ROUND BH, ITEM 4 ──────────────────────
                    // Round M's last sentence used to read: *"Those two keep their flat
                    // editor colour anyway: MATERIAL_COLOR is emitted with .w = 1 just
                    // above, and the 'tools' techset's UNLIT technique is the same
                    // vertcol_shaded_tools for caulk as for white_tools, so the colormap is
                    // lerped away regardless of which handle carries it."*  Every clause of
                    // that is still TRUE OF THE MECHANISM and it is exactly WHY caulk drew
                    // flat when it was not selected — which is the round-BH report.  So the
                    // .w for this class is now 0 (the `solidTool` arm above) and the draw is
                    // the faithful one, and the two SOLID tool materials show their own
                    // editor art.  Nothing about the DEPTH argument changed.
                    // (KIWI-UX (ROUND BH, ITEM 4): `losesDepth` is computed ABOVE now, with
                    //  the material-colour decision that shares it.  Nothing else changed.)
                    // ── KIWI-UX (ROUND BC, ITEM 3): THE SKY SEE-THROUGH ARM ──────────
                    // USER DIRECTIVE, verbatim: "allow the camera to ignore it's there so
                    // we can fly inside of the skybox and see the sky."  A sky face that
                    // stands between the eye and the orbit pivot is drawn as a translucent
                    // FILM; every other sky face stays fully textured, so the far walls
                    // are real sky behind the map and every wall is textured when you are
                    // standing in the level.  kiwi_skybox.h D-BC-A is the derivation.
                    //
                    // IT DELIBERATELY OUTRANKS `losesDepth`.  READ THIS BEFORE
                    // "SIMPLIFYING" THE EXPRESSION.  Round M added that guard because
                    // substituting d_white for an OPAQUE tool material (caulk, nodraw)
                    // "turned every cut face into a surface that occludes nothing".  A
                    // sky material is opaque, so `losesDepth` is TRUE for it — and
                    // occluding nothing is PRECISELY what this arm is for.  Folding the
                    // see-through case in under `!losesDepth` would make it a silent
                    // no-op on every sky material there is, which is why it is its own
                    // `if` AHEAD of the guard.  `losesDepth` still governs the
                    // caulk/nodraw arm alone and is untouched for every other material.
                    //
                    // AND THE FILM IS PER-VERTEX ALPHA, NOT A MATERIAL SWAP.  Routing a
                    // sky face to d_white removes the DEPTH WRITE and nothing else:
                    // `Cam_DrawFace` pins the per-vertex colour to 0xFFFFFFFF (:1153) and
                    // white_tools' blend is SrcAlpha/InvSrcAlpha, so at vertex alpha 255
                    // the blend is arithmetically OPAQUE — a solid plane that merely fails
                    // to occlude.  camwnd.cpp:667-670 writes the same thing down for the
                    // caulk case: *"It still looks opaque (the vertex colour is
                    // 0xFFFFFFFF), but nothing behind it is ever occluded"*.  The lever is
                    // the binary's own translucent-fill recipe — MATERIAL_COLOR {0,0,0,0}
                    // (set above) plus a packed per-vertex RGBA whose .a is the opacity
                    // (Cam_DrawSelectedFaceFill :1529-1533 at colors[16].a = 0.25;
                    // kiwi_hover.cpp:156-158 fills at 0.26/0.25/0.35 through the identical
                    // d_white + TECHNIQUE_UNLIT route).  0.30 sits in that measured band:
                    // enough film to read the shell boundary, thin enough to see the map
                    // through it.  NEVER w == 1 here — that is the paint, not the film.
                    if ( skySeeThrough && g_qeglobals.d_white )
                    {
                        // The sky-blue of Cam_EditorMaterialColor's table (:621) with a
                        // real alpha.
                        static const float s_skyFilm[4] = { 0.45f, 0.62f, 0.92f, 0.30f };
                        GfxColor filmCol;
                        Byte4PackPixelColor( const_cast< float * >( s_skyFilm ), &filmCol );
                        Cam_DrawFaceTinted( f, g_qeglobals.d_white,
                                            (uint)filmCol.packed, 0.0f, TECHNIQUE_UNLIT );
                    }
                    // ── KIWI-UX (ROUND BH, ITEM 4): the solid tool arm ───────────────
                    // The SAME call the selected pass makes (:2998) — the face's own
                    // material at UNLIT with the real texdef ST — so a caulk face reads
                    // identically whether or not it is selected, and the Fit that ITEM 2
                    // applies is what governs the tile size.  Cam_DrawFaceFaithfulImmediate
                    // routes through Cam_DrawMaterial/Cam_TechAvailable exactly as the
                    // world arm does, so the round-O substitution and the technique guard
                    // still apply; for caulk neither fires (it is opaque and depth-writing,
                    // so Cam_MaterialIsMissing answers false).
                    else if ( solidTool )
                    {
                        Cam_DrawFaceFaithfulImmediate( f, TECHNIQUE_UNLIT );
                    }
                    else
                    {
                        Material *flat = ( !isSky && !losesDepth && g_qeglobals.d_white )
                                         ? g_qeglobals.d_white : mtl;
                        Cam_DrawFace( f, flat, TECHNIQUE_UNLIT );
                    }
                }
                else                                            // world -> real texdef + FAKELIGHT
                {
                    // Resident geometry first: the face's vertices are already in a persistent
                    // editor VB, so draw them by NAME and only rebuild when there is no run.
                    if ( Cam_DrawFaceResident( ent, worldTech ) )
                    {
                        continue;
                    }
                    // FAKELIGHT (vertcol_shaded) LERPS by materialColor.w, so the neutral that
                    // shows the texture is w=0 (the binary's {0,0,0,0}) - a float[3] here would
                    // have its .w read OUT OF BOUNDS by R_AddCmdSetMaterialColor.
                    static const float neutral[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
                    if ( Cam_MaterialColorChanged( neutral, lastMC ) )
                    {
                        R_AddCmdSetMaterialColor( neutral );
                        Cam_SetLastMaterialColor( lastMC, neutral );
                    }
                    Cam_DrawFaceFaithfulImmediate( f, worldTech );
                }
            }
        }
    }
    }

    // The default and sun world draws set MATERIAL_COLOR per face; reset to white so the
    // selection overlay and next frame are not tinted.  The cached UNLIT path does not.
    if ( !useCache )
    {
        static const float s_white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        R_AddCmdSetMaterialColor( s_white );
    }

    // SELECTED-BRUSH RED TINT: re-draw every selected convex brush tinted by
    // d_savedinfo.colors[11].  Only the COLOUR is gated on !dontDrawSelectedTint - the brushes
    // draw either way, so a selected brush is never invisible.  UNLIT is the technique that
    // honours MATERIAL_COLOR (FAKELIGHT's vertcol_shaded ignores it).  Lights go through
    // DrawLightsMain and patches through the patch loop below, so both are skipped here.
    {
        static const float s_white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        // KIWI (2026-09-28, user: "when hovering sometimes in the uv editor, it will turn the
        // entire texture white"): MATERIAL_COLOR.w is a LERP-TO-RGB factor, not a modulate
        // (0x4fc2c0's NULL = {0,0,0,0} is the neutral), so the "tint stands down" colour
        // {1,1,1,1} painted every UV-gathered face flat white.  Untinted = {0,0,0,0}.
        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        // The second unsorted immediate loop, same fix as the world fill (the binary's flush
        // qsorts by material sort key).  The white/red tint bracket is per-FACE and emitted ON
        // CHANGE: a sorted stream can interleave brushes, and RC_SET_MATERIAL_COLOR is CRITICAL.
        const float *tintCol = g_qeglobals.dontDrawSelectedTint
                             ? s_neutral : g_qeglobals.d_savedinfo.colors[11];  // red {1,0.25,0.25}
        s_selFaces.clear();
        s_selResident.clear();
        extern bool KiwiXform_GeometryInMotion();       // kiwi_transform.h
        const bool geoInMotion = KiwiXform_GeometryInMotion();
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        {
            brush_t *def = b->def;
            if ( !def || b->patch )                         // patches → patch-wireframe loop
                continue;
            entity_s_def *eDef = b->owner ? (entity_s_def *)b->owner->def : nullptr;
            if ( eDef && eDef->eclass && ( eDef->eclass->classtype & 1 ) )   // CLASS_LIGHT → skip
                continue;
            // Fixedsize point entities are not world faces - same DrawBrush gate as the world
            // fill.  The selected-entity pass below redraws them via DrawBrush -> DrawModels.
            if ( eDef && eDef->eclass && *(int *)&eDef->eclass->fixedsize )
                continue;
            // ── KIWI-UX (ROUND BN, ITEM 3): THE TINT STANDS DOWN, THE BRUSH DOES NOT ──
            // This pass is the ONLY thing that draws a selected brush's textured surface
            // (the world pass walks `active_brushes`), so skipping a gathered brush here
            // would make it disappear rather than un-highlight it.  The COLOUR is what
            // hides the texture, so the colour is what is dropped — the same split the
            // pass's own !dontDrawSelectedTint gate already makes, applied per brush.
            const bool uvOwns = KiwiUvEd_OverlaySuppressed( def, -1 );
            // The resident geometry the world pass draws from (version-gated).  Not while a
            // G/R/S drag is changing it: a stale face takes the immediate path below, and the
            // rebuild runs once the pose has settled (KiwiXform_GeometryInMotion).
            if ( Cam_WorldGeoResident() && !geoInMotion )
                Brush_CheckBuildFaceVis( b, (const float *)world_orient_matrix );
            for ( int fi = 0; fi < def->faceCount; ++fi )
            {
                face_t *f = &def->faces[fi];
                if ( !f->w )
                    continue;
                // `classifySky` is false: a selected sky brush draws its OWN material at
                // TECHNIQUE_UNLIT, so there is no overlay bucket on this pass.
                Cam_GatherFace( s_selFaces, f, FaceMaterial( &f->mtldef[layer] ),
                                &f->mtldef[layer], /*classifySky*/ false, uvOwns, b, fi );
            }
        }
        // KIWI (2026-10-11, user: moving a whole building ran at 5 fps): faces with resident
        // geometry wait for their own tinted flush window after the main world flush (the
        // selected-entity pass's shape), merged into a few draws; this immediate loop -
        // one CPU-built RC_DRAW_TRIANGLES per face - keeps only the faces that have none.
        {
            size_t keep = 0;
            for ( size_t si = 0; si < s_selFaces.size(); ++si )
            {
                if ( Cam_FaceResidentReady( s_selFaces[si] ) )
                    s_selResident.push_back( s_selFaces[si] );
                else
                    s_selFaces[keep++] = s_selFaces[si];
            }
            s_selFaces.resize( keep );
            for ( size_t si = 0; si < s_selFaces.size(); ++si )
                s_selFaces[si].seq = (int)si;               // the sort's tie-break is the index
        }
        Cam_SortGatheredFaces( s_selFaces );
        const float *lastSelCol = nullptr;
        for ( size_t si = 0; si < s_selFaces.size(); ++si )
        {
            const CamSortFace &ent  = s_selFaces[si];
            const float       *want = ent.uvOwns ? s_neutral : tintCol;
            if ( want != lastSelCol )                       // pointer identity: both are stable
            {
                R_AddCmdSetMaterialColor( want );
                lastSelCol = want;
            }
            Cam_DrawFaceFaithfulImmediate( ent.f, TECHNIQUE_UNLIT );         // textured * red
        }
        R_AddCmdSetMaterialColor( s_white );                // reset for overlays / next frame
    }

    // ENTITY DRAW (ACTIVE list): bboxes + MODELS + PREFAB CONTENTS.  The world fill above only
    // draws convex WORLD faces, which fixedsize POINT entities do not have, so without this they
    // are invisible in the 3D view.  The binary sends every brush through
    // DrawGeneralWorld_ -> DrawBrush at the camera technique; this pass keeps plain point
    // entities (info_*, triggers, path nodes, lights) as tech-29 wireframe bboxes but gives
    // MODEL/PREFAB classes (classtype & 0x18) the camera technique, so DrawModels renders the
    // xmodel mesh and the prefab contents textured.  Identity view orientation (DrawBrush
    // derives the entity inverse itself), viewType -1 so DrawShadedWireframe draws every edge.
    // SELECTED entities get their own tinted pass after the main flush.
    // How many surfs the world fill put in the open flush window before this pass.  Zero on a
    // prefab-authored map — which is when the window and the cached block coincide.
    const int  pendingBeforeEntity = Editor_PendingSurfCount();
    bool       entReplayed         = false;
    // on a PATCHED frame the pass opens with this many entries already in
    // the flush comparator's order (the objects that did not change), so the flush merges
    // the live tail into them instead of re-sorting the map.  0 = no claim.
    int        entSortedFirst      = 0;
    int        entSortedCount      = 0;
    {
        // mp_backlot's worldspawn has no brushes: the level arrives as one misc_prefab, so this
        // loop and the DrawBrush_PrefabContents recursion under it ARE the frame.
        PROF_SCOPED( "entity + prefab pass" );
        // This pass's OUTPUT does not depend on the camera, so an unchanged frame replays it.
        // kiwi_surfcache.h has the validity contract.  The PASS KEY folds in every per-frame
        // input no epoch covers, BY VALUE — the per-vertex sun bake writes FRAME state into
        // VERTEX data (brush.cpp:2602), so a block taken with it off must not replay with it on.
        const unsigned entPassKey =
              ( (unsigned)Cam_EntityMeshTech( worldTech ) & 0xFFu )
            | ( (unsigned)( g_qeglobals.current_edit_layer & 0xFF ) << 8 )
            | ( Radiant_DecorEnabled()    ? 0x00010000u : 0u )
            | ( g_edSunBakeVertColor      ? 0x00020000u : 0u )
            | ( Cam_SunPrevEnabled()      ? 0x00040000u : 0u )
            | ( sunApprox                 ? 0x00080000u : 0u )
            | ( useCache                  ? 0x00100000u : 0u );
        // ── KIWI: GATHER THE DISPATCH LIST ONCE ──────
        // The gate below used to live inside the draw loop.  It is hoisted because the
        // per-object cache needs the SAME sequence twice — once to ask each object
        // whether it changed, once to draw the ones that did — and a gate written twice
        // is a gate that drifts.  Function-static: CamWnd_Draw is not re-entrant.
        static std::vector< selbrush_t * >      s_entBrushes;
        static std::vector< unsigned long long > s_entSigs;
        s_entBrushes.clear();
        {
            selbrush_t *head = &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                brush_t *def = b->def;
                if ( !def || b->patch )                         // patches: already in main world range
                    continue;
                entity_s     *owner = b->owner;
                entity_s_def *eDef  = owner ? (entity_s_def *)owner->def : nullptr;
                eclass_t     *ec    = eDef ? eDef->eclass : nullptr;
                if ( !ec || !*(int *)&ec->fixedsize )           // only fixedsize point entities
                    continue;

                // ── KIWI-UX (ROUND AX, ITEM 2): ENTITIES OBEY FilterBrush TOO ────
                // USER REPORT: "entities are not hidable via H, it does nothing even
                // though the eyeball changes in the UI."  The eyeball was telling the
                // truth: H (Cmd_OnHideSelected, mainfrm.cpp:5091) runs Select_Hide,
                // which sets selbrush_t.brushFlags bit 2 (select.cpp:4179-4180), and the
                // outliner's eye reads that same bit (kiwi_outliner.cpp:403-406).  What
                // never asked was THIS loop.
                //
                // FilterBrush folds the hidden bit into its answer — `(brushFlags & 5)`
                // at filters.cpp:724, bit 0 = filtered, bit 2 = HIDDEN — and it is what
                // gates the convex world pass at :2615, the ROUND-AF patch pass at :2975,
                // BOTH 2D loops (xywnd.cpp:1086/:1163) and every pick entry
                // (select.cpp:672/:774, kiwi_pick.cpp:167).  Its absence here was exactly
                // the round-AF defect repeated on the ENTITY pass, and it is why hiding
                // worked in the XY view and did nothing in the 3D view.
                //
                // A raw `brushFlags & 4` test would have been the wrong fix: it would be
                // a second spelling of "hidden" that drifts from the filter and layer
                // semantics the shared predicate already owns.  The gate also cannot go
                // inside DrawBrush — brush.cpp:7208 only tests bit 1 (layer) and its
                // `drawFlags & 1` force-draw contract is what the tint and outline passes
                // rely on — so it goes in the caller loop, which is the binary's own
                // shape (0x407af0: `!FilterBrush`).
                if ( FilterBrush( b, 0 ) )
                    continue;
                s_entBrushes.push_back( b );
            }
        }
        const int entCount = (int)s_entBrushes.size();

        // The frame in which NOTHING changed is answered first and costs nothing extra:
        // the whole block comes back presorted, with its resident index runs.
        entReplayed  = KiwiSurfCache_TryReplay( entPassKey );
        int entMode  = entReplayed ? KIWI_SURFPASS_REPLAY : KIWI_SURFPASS_LIVE;

        // Only when that was refused is it worth asking each object whether IT changed.
        // The signatures come out of the recorded walk (kiwi_walkcache.h).
        s_entSigs.clear();
        if ( !entReplayed && entCount > 0 && KiwiSurfCache_PatchWanted( entPassKey ) )
        {
            {
                PROF_SCOPED( "cam surf cache signatures" );
                if ( KiwiWalk_BeginReplay( &active_brushes,
                                           (const orientation_t *)world_orient_matrix,
                                           /*backward*/ false ) )
                {
                    s_entSigs.resize( (size_t)entCount, 0ull );
                    for ( int i = 0; i < entCount; ++i )
                    {
                        if ( !KiwiWalk_TopLevel( s_entBrushes[i] ) )
                            break;               // the recording abandoned: leave the rest 0
                        s_entSigs[i] = KiwiWalk_SubtreeSignature( s_entBrushes[i] );
                    }
                    KiwiWalk_EndReplay();
                }
            }
            if ( (int)s_entSigs.size() == entCount )
            {
                entMode = KiwiSurfCache_PassMode( entPassKey, &s_entBrushes[0],
                                                  &s_entSigs[0], entCount );
                entReplayed = ( entMode == KIWI_SURFPASS_REPLAY );
            }
        }

        // ── PATCH: replay the objects that did not change, redraw the ones that did ──
        if ( entMode == KIWI_SURFPASS_PATCH )
        {
            R_Ed_BeginLineBucket();
            if ( !KiwiSurfCache_PatchReplay( &entSortedFirst, &entSortedCount ) )
            {
                // Refused after the gate said yes: nothing was emitted, so fall through to
                // the live pass (which opens its own bucket).
                R_Ed_EndLineBucket();
                KiwiSurfCache_PatchEnd();
                entMode        = KIWI_SURFPASS_LIVE;
                entSortedFirst = 0;
                entSortedCount = 0;
            }
        }
        if ( entMode == KIWI_SURFPASS_PATCH )
        {
            {
                extern int g_drawBrushMeshTech;   // brush.cpp:6535
                g_drawBrushMeshTech = Cam_EntityMeshTech( worldTech );
                const bool walkOpened = KiwiWalk_BeginReplay(
                    &active_brushes, (const orientation_t *)world_orient_matrix,
                    /*backward*/ false );
                bool walkReplay = walkOpened;
                for ( int i = 0; i < entCount; ++i )
                {
                    selbrush_t *b = s_entBrushes[i];
                    // A cursor mismatch abandons the RECORDING, not this loop: the objects
                    // still to draw would otherwise leave holes for a frame.  DrawBrush
                    // walks its own prefab list when the recording is gone.
                    if ( walkReplay && !KiwiWalk_TopLevel( b ) )
                        walkReplay = false;
                    if ( !KiwiSurfCache_ObjectDirty( i ) )
                        continue;                 // its surfs and lines already replayed
                    GfxColor ecol;
                    Cam_BrushColor2d( b, &ecol );
                    entity_s_def *eDef = b->owner ? (entity_s_def *)b->owner->def : nullptr;
                    eclass_t     *ec   = eDef ? eDef->eclass : nullptr;
                    const int entTech  = ( ec && ( ec->classtype & 0x18 ) )
                                         ? (int)worldTech : 29;
                    DrawBrush( b, (orientation_t *)world_orient_matrix, /*viewType*/ -1,
                               entTech, &ecol, /*width*/ 1, /*drawFlags*/ 0, "" );
                }
                if ( walkOpened )
                    KiwiWalk_EndReplay();
                g_drawBrushMeshTech = -1;
            }
            R_Ed_EndLineBucket();
            KiwiSurfCache_PatchEnd();
        }

        if ( entMode == KIWI_SURFPASS_LIVE )
        {
        KiwiSurfCache_BeginRecord( entPassKey );
        // Group this pass's line stream by colour: Ed_EmitLineBatch's per-colour-run
        // RC_SET_MATERIAL_COLOR breaks R_AddLineCmd's merge, and each broken command costs a
        // full backend tess flush.  INSIDE the record bracket deliberately, so the GROUPED
        // bytes are what the surf cache captures.  Every other pass stays immediate.
        R_Ed_BeginLineBucket();
        // KIWI-UX (ROUND AV, ITEMS 1+2): publish draw_meth2 for this pass only.  `entTech`
        // below stays exactly as it was — it is draw_meth1, and it is what a model-LESS
        // point entity's placeholder bbox draws with, so leaving it at 29 keeps every
        // info_* / node_* / trigger_* box a wireframe box.  Only the MESH follows the
        // show state.  Reset to -1 after the loop: the 2D views (xywnd.cpp:4056/:4156) and
        // the white-outline pass below call the same DrawBrush and must keep their own 29.
        extern int g_drawBrushMeshTech;   // brush.cpp:6535
        g_drawBrushMeshTech = Cam_EntityMeshTech( worldTech );
        {
            // The dispatch list gathered above IS this loop's sequence — the FilterBrush /
            // fixedsize / patch gate now lives there and only there.  The PREFAB SUBTREES
            // under each brush are replayed from the recording; KiwiWalk_TopLevel advances
            // the cursor per dispatched brush, and the signature it can then read is what
            // the next frame compares this object against (kiwi_surfcache.h).
            const bool walkReplay = KiwiWalk_BeginReplay(
                &active_brushes, (const orientation_t *)world_orient_matrix,
                /*backward*/ false );
            for ( int i = 0; i < entCount; ++i )
            {
                selbrush_t   *b     = s_entBrushes[i];
                entity_s_def *eDef  = b->owner ? (entity_s_def *)b->owner->def : nullptr;
                eclass_t     *ec    = eDef ? eDef->eclass : nullptr;

                GfxColor ecol;
                Cam_BrushColor2d( b, &ecol );

                // Model/prefab classes take the camera technique (meshes/contents render lit
                // through the surf-cache); plain point entities keep the wireframe bbox.
                const int entTech = ( ec && ( ec->classtype & 0x18 /*CLASS_MODEL|CLASS_PREFAB*/ ) )
                                    ? (int)worldTech : TECHNIQUE_WIREFRAME_SHADED;
                // drawFlags 0 = draw ALL layers in one pass.  The binary runs TWO passes,
                // DrawGeneralWorld_(tech, 8=SKIP_MULTIPLY) then (tech, 4=ONLY_MULTIPLY);
                // this pass runs ONCE, so 8 would drop every additive/effect
                // prefab-content layer.  drawFlags 0 short-circuits Editor_SurfFilter
                // ((drawFlags&0xC)==0), letting all layers draw ordered by material sortKey.
                if ( walkReplay )
                    KiwiWalk_TopLevel( b );      // put the cursor on this brush's node
                DrawBrush( b, (orientation_t *)world_orient_matrix, /*viewType (no cull)*/ -1,
                           entTech, &ecol, /*width*/ 1, /*drawFlags*/ 0, /*layerPrefix*/ "" );
                // Close this object's segment.  A zero signature (no recording, or the
                // cursor drifted) makes the whole capture unpatchable — the block still
                // caches, it just cannot be edited one object at a time.
                KiwiSurfCache_RecordNode(
                    b, walkReplay ? KiwiWalk_SubtreeSignature( b ) : 0ull );
            }
            if ( walkReplay )
                KiwiWalk_EndReplay();
        }
        g_drawBrushMeshTech = -1;   // KIWI-UX (ROUND AV): draw_meth2 == draw_meth1 again
        R_Ed_EndLineBucket();        // emit the grouped lines...
        KiwiSurfCache_EndRecord();   // ...then capture them with the pass
        }
    }
    // With `pendingBeforeEntity`, this is how the flush decides whether the window it is about
    // to emit is EXACTLY the replayed block — the only case where its order is already sorted.
    const int pendingAfterEntity = Editor_PendingSurfCount();

    // Curve-point candidate markers (binary DrawGeneralWorld_ → Draw_PatchSelectPoints):
    // the GREEN UNSELECTED-candidates overlay, self-gated on sel_curvepoint/
    // sel_area.  The SELECTED set draws light blue via Draw_PatchSelectPointsSelected
    // from the DrawConnectionLinks position at the Cam_Draw tail.
    Draw_PatchSelectPoints();

    // Flush the accumulated surf cache as one RC_DRAW_EDITOR_SKINNEDCACHED: ED_SURF_MESH
    // (brush faces from the cached world fill and the entity pass's prefab contents) plus
    // ED_SURF_MODEL (xmodel meshes).  Unconditional - a no-op when nothing is queued.
    // MATERIAL_COLOR must be the binary's NEUTRAL {0,0,0,0} first.  The lit editor pixel
    // shader family (vertcol_shaded: fakelight_normal/view 24/25 and wireframe_shaded 29) does
    // not MULTIPLY by MATERIAL_COLOR, it LERPS:
    //   result.rgb = lerp( sample(colorMap)*vColor, materialColor.rgb, materialColor.w )
    // so .w is a FLAT-COLOUR OVERRIDE factor - w==1 replaces the texture entirely.  The binary
    // resets via R_SetMaterialColor(NULL) (0x4fc2c0's NULL arm = all zeros).  The port's
    // $line adaptation pushes eclass colours at w=1, so the reset here is
    // mandatory - and to zeros, NOT white.
    {
        static const float s_flushNeutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // binary R_SetMaterialColor(NULL)
        R_AddCmdSetMaterialColor( s_flushNeutral );
    }
    {
        PROF_SCOPED( "main surf flush (sort)" );
        // `presorted`: the window is byte-for-byte the block the cache replayed, already in this
        // comparator's order — claimed ONLY when the world fill put nothing ahead of the block
        // and nothing was appended after the pass.  `runsKey`: tells the backend THIS window is
        // the one it may hold resident index runs for; every other flush leaves it cleared.
        {
            const bool windowIsBlock = entReplayed
                                    && pendingBeforeEntity == 0
                                    && Editor_PendingSurfCount() == pendingAfterEntity;
            KiwiEdScene_StampMainFlush( KiwiSurfCache_BuildSerial(), windowIsBlock );
            // a patched frame is not the block, but its head IS sorted.
            // The flush honours the claim only when it names the window's own first entry,
            // so a world fill ahead of the pass just falls back to the full sort.
            KiwiEdScene_StampSortedPrefix( entSortedFirst, entSortedCount );
        }
        R_AddEditorSurfsCmd();
    }

    // The see-through tool volumes (own label art) and sky film, over the finished world (deferred in
    // the world submit), then MATERIAL_COLOR back to the flush neutral the passes below inherit.
    if ( !s_deferredToolFaces.empty() )
    {
        PROF_SCOPED( "tool volume pass" );
        // KIWI (2026-10-28, user: "ive got this map loaded in the kiwi radiant, and it's ungodly
        // laggy"): every volume face was rebuilt on the CPU (Face_BuildLayerGeom) and drawn as
        // its own RC_DRAW_TRIANGLES, ~10k draws a frame on london.map.  A plain volume (its own
        // art at the neutral colour) already has its geometry resident - the same faceVis run
        // the world pass draws by name - so those go through ONE self-bracketed flush window
        // after the main flush (the selected-brush resident pass's shape), sorted and merged
        // by the flush.  Lightgrid (own blended art), the sky film and any face without
        // resident geometry keep the immediate arm below.
        static const float s_toolArtNeutral[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
        static std::vector< CamDeferredToolFace > s_immediateTools;
        s_immediateTools.clear();
        bool windowOpen = false;
        for ( const CamDeferredToolFace &d : s_deferredToolFaces )
        {
            if ( !d.film && !d.ownBlend && d.inst
              && !memcmp( d.col, s_toolArtNeutral, sizeof( d.col ) ) )
            {
                CamSortFace s = {};
                s.f         = d.f;
                s.inst      = d.inst;
                s.faceIndex = d.faceIndex;
                if ( Cam_FaceResidentReady( s ) )
                {
                    if ( !windowOpen )
                    {
                        R_SortMaterials();
                        R_AddCmdSetMaterialColor( s_toolArtNeutral );
                        windowOpen = true;
                    }
                    if ( Cam_DrawFaceResident( s, TECHNIQUE_UNLIT ) )
                        continue;
                }
            }
            s_immediateTools.push_back( d );
        }
        if ( windowOpen )
            R_AddEditorSurfsCmd();
        s_deferredToolFaces.swap( s_immediateTools );

        // The immediate remainder: consecutive RC_DRAW_TRIANGLES share one draw only while
        // material, technique and MATERIAL_COLOR stay the same, so group them (stable: a
        // group keeps its brush order).
        std::stable_sort( s_deferredToolFaces.begin(), s_deferredToolFaces.end(),
            []( const CamDeferredToolFace &a, const CamDeferredToolFace &b )
            {
                const int c = memcmp( a.col, b.col, sizeof( a.col ) );
                if ( c )                      return c < 0;
                if ( a.film != b.film )       return a.film < b.film;
                if ( a.ownBlend != b.ownBlend ) return a.ownBlend < b.ownBlend;
                const int layer = g_qeglobals.current_edit_layer;
                return (uintptr_t)a.f->mtldef[layer].lyrMtl < (uintptr_t)b.f->mtldef[layer].lyrMtl;
            } );
        float lastToolMC[4] = { -1.0f, -1.0f, -1.0f, -1.0f };
        for ( const CamDeferredToolFace &d : s_deferredToolFaces )
        {
            if ( Cam_MaterialColorChanged( d.col, lastToolMC ) )
            {
                R_AddCmdSetMaterialColor( d.col );
                Cam_SetLastMaterialColor( lastToolMC, d.col );
            }
            if ( d.film )
            {
                // the film's colour and alpha ride in the vertex colour (see the world arm)
                static const float s_skyFilm[4] = { 0.45f, 0.62f, 0.92f, 0.30f };
                GfxColor filmCol;
                Byte4PackPixelColor( const_cast< float * >( s_skyFilm ), &filmCol );
                Cam_DrawFaceTinted( d.f, g_qeglobals.d_white, (uint)filmCol.packed, 0.0f, TECHNIQUE_UNLIT );
            }
            else
                Cam_DrawFaceFaithfulImmediate( d.f, TECHNIQUE_UNLIT, d.ownBlend );   // its own art, as when selected
        }
        static const float s_toolNeutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        R_AddCmdSetMaterialColor( s_toolNeutral );
        s_deferredToolFaces.clear();
    }

    // SELECTED-BRUSH TINT, RESIDENT HALF (KIWI 2026-10-11): the faces the tint pass above
    // deferred, from the same resident geometry the world pass draws, in self-bracketed flush
    // windows (the selected-entity pass's shape) so the flush merges them into a few draws
    // instead of one CPU-built draw per face.  Tinted window, then the UV editor's untinted
    // faces.  After the main flush: opening a window inside it would discard the world's surfs.
    if ( !s_selResident.empty() )
    {
        PROF_SCOPED( "selected brush resident pass" );
        static const float s_neutralSel[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float *tintCol = g_qeglobals.dontDrawSelectedTint
                             ? s_neutralSel : g_qeglobals.d_savedinfo.colors[11];
        for ( int pass = 0; pass < 2; ++pass )
        {
            const bool uvPass = ( pass == 1 );
            bool any = false;
            for ( size_t si = 0; si < s_selResident.size() && !any; ++si )
                any = s_selResident[si].uvOwns == uvPass;
            if ( !any )
                continue;
            R_SortMaterials();
            R_AddCmdSetMaterialColor( uvPass ? s_neutralSel : tintCol );
            for ( size_t si = 0; si < s_selResident.size(); ++si )
                if ( s_selResident[si].uvOwns == uvPass )
                    Cam_DrawFaceResident( s_selResident[si], TECHNIQUE_UNLIT );
            R_AddEditorSurfsCmd();
        }
        // Back to the main flush's neutral: the selected patch / entity passes push their
        // tint only when the pref is on and otherwise inherit this ({1,1,1,1} = flat white).
        R_AddCmdSetMaterialColor( s_neutralSel );
        s_selResident.clear();
    }

    // SELECTED PATCH fill/wireframe.  Active patches are already in the main world range above;
    // this selected-only range carries the red tint and force-draw semantics.
    // DrawGeneralWorld_ -> DrawBrush routes patch brushes to the camera
    // (viewType>2) tech-29 branch, 0x4415D0 - the FILLED per-material-layer patch draw
    // (Patch_Fill_SyncVersion instance rebuild + Editor_AddMeshCmd); DrawBrush may still pick
    // DrawPatchesWireframeGrid when the patch-wireframe pref is on.  Self-bracketed: opens with
    // R_SortMaterials and flushes its own surfs, so it neither discards nor re-flushes the main
    // pass's range.  It must NOT straddle the main accumulation - doing so discards every surf
    // the world fill queued and makes the main flush re-draw the patch range.
    {
        PROF_SCOPED( "selected patch pass" );
        const orientation_t *orient = (const orientation_t *)world_orient_matrix;

        R_SortMaterials();
        {
            selbrush_t *head = &selected_brushes;
            if ( !g_qeglobals.dontDrawSelectedTint )
                R_AddCmdSetMaterialColor( g_qeglobals.d_savedinfo.colors[11] );
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( !b->def || !b->patch )                     // DrawBrush's instance-field test
                    continue;
                // ── KIWI-UX (ROUND AF, ITEM 9): PATCHES OBEY FilterBrush TOO ────
                // USER REPORT, verbatim: "When hiding selected objects, q3 curves
                // don't hide. actually they aren't selectable at all. Fix this."
                //
                // THE HIDE HALF IS THIS LINE'S ABSENCE.  `FilterBrush` (filters.cpp
                // :724) folds the HIDDEN bit (brushFlags & 4) into the same answer as
                // every filter entry — `return (a1->brushFlags & 5) != 0;` — and it
                // gates the convex world pass 230 lines above (:2477, "FilterBrush is
                // load-bearing"), BOTH of xywnd.cpp's brush loops (:1085 active,
                // :1163 selected), and all three pick entries (select.cpp:672,
                // kiwi_pick.cpp:167, kiwi_boxselect.cpp via Pick_BrushPickable).
                // The patch pass asked NOTHING but `!b->def || !b->patch`.
                //
                // So `Select_Hide` set brushFlags |= 4 on the patch node correctly
                // (select.cpp:4168), the 2D views dropped it, every pick path dropped
                // it — and the 3D camera kept drawing it. "Hiding does nothing" and
                // "it cannot be clicked" were ONE defect seen from two windows.
                //
                // NOT A DEVIATION: the binary has no separate patch pass at all —
                // DrawGeneralWorld_ (0x407af0) runs ONE loop over the world brushes
                // gated on `!FilterBrush` and lets DrawBrush (0x47B018)
                // dispatch patches out of it.  The port split that loop in two so the
                // patch surfs could be self-bracketed (see the note above), and the
                // split dropped the gate on one side.  This restores it.
                //
                // The selected pass stays gated too: hidden and selected are not mutually
                // exclusive when visibility is changed from the outliner.
                if ( FilterBrush( b, 0 ) )
                    continue;
                GfxColor pcol;
                Cam_BrushColor2d( b, &pcol );
                DrawBrush( b, orient, /*viewType*/ -1, (int)worldTech, &pcol,
                           /*width*/ 1, /*drawFlags*/ 1, /*layerPrefix*/ "" );
            }
            if ( !g_qeglobals.dontDrawSelectedTint )
            {
                static const float s_neutralPatch[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                R_AddCmdSetMaterialColor( s_neutralPatch );
            }
        }
        R_AddEditorSurfsCmd();
    }

    // SELECTED-ENTITY textured + red tint: every selected non-light
    // non-patch brush drawn TEXTURED at tech_type, preceded by R_SetMaterialColor(colors[11])
    // (red {1,0.25,0.25,0.25}, gated on !dontDrawSelectedTint), then its OWN
    // R_AddEditorSurfsCmd so only these surfs carry the tint.  vertcol_shaded LERPs,
    // so the result is 0.75*texture + 0.25*red - the textured base under the white wireframe.
    {
        // R_SortMaterials advances sceneSurfCount_saved so the flush below carries
        // ONLY this pass's surfs.  Without it every flush re-draws from surf 0.  The binary
        // calls it before EACH pass: world, tint, white.
        R_SortMaterials();
        // KIWI-UX (ROUND AV, ITEMS 1+2): same draw_meth2 publication as the active pass.
        // A SELECTED model must skin at the same technique its unselected neighbours do —
        // this is the textured base the white tech-29 outline pass then draws on top of.
        extern int g_drawBrushMeshTech;   // brush.cpp:6535
        g_drawBrushMeshTech = Cam_EntityMeshTech( worldTech );
        // The red tint, or neutral when the pref is off (the model keeps its texture).
        if ( !g_qeglobals.dontDrawSelectedTint )
            R_AddCmdSetMaterialColor( g_qeglobals.d_savedinfo.colors[11] );   // red {1,0.25,0.25,0.25}
        for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        {
            brush_t *def = b->def;
            // The binary tests brushFlags & 0x100 here (the misc_model CYCLE-PREVIEW flag; see
            // the white-outline pass below).  The port instead splits patches into
            // their own self-bracketed pass above, so they are skipped here by `b->patch`
            // (the instance field — the binary's sole patch predicate, in DrawBrush).
            if ( !def || b->patch )
                continue;
            entity_s     *owner = b->owner;
            entity_s_def *eDef  = owner ? (entity_s_def *)owner->def : nullptr;
            eclass_t     *ec    = eDef ? eDef->eclass : nullptr;
            if ( !ec || !*(int *)&ec->fixedsize )           // fixedsize point entities (models/prefabs)
                continue;
            if ( ec->classtype & 1 )                        // skip CLASS_LIGHT (DrawLightsMain)
                continue;
            // KIWI-UX (ROUND AX, ITEM 2): the SELECTED twin of the gate on the active
            // pass above.  Needed because hidden and selected are not exclusive — the
            // outliner's eye can hide a row that stays selected, and Select_HideUnselected
            // (select.cpp:4184) leaves the selection intact by construction.  This mirrors
            // xywnd.cpp:1164, which gates its selected loop the same way.
            if ( FilterBrush( b, 0 ) )
                continue;
            const int entTech = ( ec->classtype & 0x18 /*MODEL|PREFAB*/ ) ? (int)worldTech : TECHNIQUE_WIREFRAME_SHADED;
            GfxColor ecol; Cam_BrushColor2d( b, &ecol );     // decoration/bbox colour (col arg)
            DrawBrush( b, (orientation_t *)world_orient_matrix, /*viewType*/ -1,
                       entTech, &ecol, /*width*/ 1, /*drawFlags*/ 1, /*layerPrefix*/ "" );
        }
        g_drawBrushMeshTech = -1;   // KIWI-UX (ROUND AV): before the tech-29 outline pass
        // Flush the selected surfs WHILE matColor is the red tint (separate from the
        // w=0 active flush above), then reset to neutral for the passes that follow.
        R_AddEditorSurfsCmd();
        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        R_AddCmdSetMaterialColor( s_neutral );             // R_SetMaterialColor(NULL)
    }

    // SELECTED-FACE FILL, immediately after the tinted flush and inside the same
    // !dontDrawSelectedTint gate the binary uses.
    if ( !g_qeglobals.dontDrawSelectedTint )
        Cam_DrawSelectedFaceFill();

    // ActiveSunLightPreviewInit, i.e. AFTER the world, patch and
    // selected-entity flushes and BEFORE the decoration/white-outline tail.  The
    // position is load-bearing: the multiply quad can only darken ALREADY-RASTERISED pixels and
    // the depth-EQUAL re-add can only land on depths the base pass wrote.
    if ( g_PrefsDlg->enable_light_preview )
        R_AddCmdProjectionSet2D(); // ActiveSunLightPreviewInit opens in 2D.
    Cam_SunPrev_Main( faithfulSun, sunMultiplyMat, s_litSunDir, s_litAmbientMul,
                      s_litSunColor, s_litHaveSun );
    // KIWI-UX: A completed sun pass already supplied the frame's ambient multiply.
    const bool ambientBaseDone = faithfulSun && s_litHaveSun;
    Cam_DrawLightPreviews( ambientBaseDone, sunMultiplyMat );

    // LIGHT-REGION HULL overlay, drawn right after the DrawLightsMain loop (and,
    // in the binary, after the additive world pass the port defers).  Draws the
    // hulls View→"Show Regions For Selected" published into d_lightRegionHulls; empty (and
    // therefore a no-op) until that command runs.
    {
        extern void RegionLightRelated();              // 0x406ac0 (defined below)
        RegionLightRelated();
        R_SortMaterials();                             // pass demarcation
    }

    // TRIGGER-RADIUS volumes + SCRIPT-COLOUR TOKENS.  Cam_Draw draws these in its
    // selected/active brush overlay loops after the surf flush: DrawTriggerRadius (selected-loop
    // non-light + active-loop trigger-class) and CamWnd_Tokens (gated on 0x4560F0).  Here: one
    // combined pass over both lists (they are display-only overlays), default-OFF behind
    // RADIANT_DECOR.
    if ( Radiant_DecorEnabled() )
    {

        // DrawTriggerRadius — every trigger_radius/_disk entity (classtype & 0xC0), in
        // both lists (binary: selected-loop else-branch + active-loop trigger-class).
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b && b != head; b = b->next )
            {
                if ( pass == 0 && !b->cullFlag )         // active-loop honours cullFlag (binary)
                    continue;
                entity_s *owner = b->owner;
                if ( !owner )
                    continue;
                entity_s_def *eDef = (entity_s_def *)owner->def;
                if ( !eDef || !eDef->eclass )
                    continue;
                if ( ( eDef->eclass->classtype & 0xC0 /*TRIGGER_RADIUS|TRIGGER_DISC*/ ) != 0 )
                    Cam_DrawTriggerRadius( b );
            }
        }

        // CamWnd_Tokens — the 0x4560F0 gate, then the last selected script-trigger's
        // ScriptColorTeamKey value drives the token billboards.
        const bool tokensOn = strcmp( g_PrefsDlg->ScriptGroupKey.c_str(), "token" ) != 0
            && strcmp( g_PrefsDlg->ScriptGroupKey.c_str(), g_PrefsDlg->ScriptColorTeamKey.c_str() ) == 0;
        if ( tokensOn )
        {
            extern bool ScriptGroup_BrushIsTrigger( selbrush_t *b );   // scriptgroup.cpp 0x453FD0
            const char *teamVal = nullptr;
            for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
                if ( ScriptGroup_BrushIsTrigger( b ) )
                    teamVal = ValueForKey2( b->owner->def, g_PrefsDlg->ScriptColorTeamKey.c_str() );
            if ( teamVal && teamVal[0] )
                Cam_DrawTokens( teamVal );
        }

    }

    // (KIWI-UX ROUND S: the §17 grid + axes USED to be drawn here, after every world
    //  pass.  They now run BEFORE the world — see the hook near the top of this
    //  function for the whole argument.  Nothing else moved.)

    // KIWI: EDGE OUTLINE / X-RAY view modes (kiwi_section.h, D-EDGEOUTLINE / D-XRAY).  Here,
    // after every world/entity/patch/selected pass and BEFORE the depth clear below, because
    // the visible-edge pass is depth-tested against the finished scene.  Immediate line
    // commands only (no surf accumulation is open here); no-op while both modes are off.
    {
        extern void KiwiView_DrawEdges();     // kiwi_section.cpp
        KiwiView_DrawEdges();
    }

    // SELECTED-BRUSH WHITE OUTLINE (!dontDrawSelectedOutlines), as in the binary:
    // R_AddClearCmd(6 = depth+stencil) -> DrawRegions -> pack colorWhite ->
    // each selected non-patch brush at WIREFRAME tech 29 through the SAME DrawBrush the world
    // pass uses -> ONE R_AddEditorSurfsCmd.  For a fixedsize MODEL/PREFAB that dispatches
    // SkinModelInst at draw_meth2==29, so the model's triangle MESH is queued as an
    // ED_SURF_MODEL at tech 29 and draws as a white wireframe; for a convex brush the same call
    // bottoms out in DrawGeo's tech-29 line outline.
    if ( !g_qeglobals.dontDrawSelectedOutlines )
    {

        // Clear DEPTH|STENCIL before the outline draws so the selected wireframe
        // passes the depth test against the coplanar geometry and shows THROUGH.  whichToClear=6
        // means the colour arg is unused.
        extern void R_AddCmdClearScreen( int whichToClear, const float *color, float depth, uint8_t stencil );
        R_AddCmdClearScreen( 6, colorWhite, 1.0f, 0 );
        // DrawRegions (the region-border line overlay) is not ported - it only
        // draws while a region is active, and is unrelated to the model wireframe.
        // 0x4084f0 - demarcate this WHITE pass so its flush carries ONLY the tech-29 wireframe.
        // Re-flushing the earlier textured surfs after the depth clear re-draws the UNSELECTED
        // prefab's model over a cleared depth buffer.
        R_SortMaterials();
        GfxColor whiteCol;
        Byte4PackPixelColor( const_cast<float *>( colorWhite ), &whiteCol );

        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        {
            brush_t *def = b->def;
            if ( !def )
                continue;
            // Bit 0x100 is the misc_model cycle-preview flag, not a patch test.
            if ( ( b->brushFlags & 0x100 ) != 0 )
                continue;
            // KIWI-UX (ROUND AX, ITEM 2): and the outline pass, which needs the gate MOST.
            // It passes drawFlags = 1, and drawFlags & 1 is precisely what defeats
            // DrawBrush's own visibility test (brush.cpp:7208) — so without an explicit
            // FilterBrush here a hidden selected entity would still get a white wireframe
            // drawn over a freshly cleared depth buffer, i.e. hidden geometry showing
            // THROUGH everything.  Same predicate, same reason as the two passes above.
            if ( FilterBrush( b, 0 ) )
                continue;
            // KIWI-UX (ROUND BN, ITEM 3): THE WIREMESH THE REPORT IS ABOUT.  A gathered
            // whole brush / patch loses its white tech-29 outline while the UV editor is
            // in scope — this pass draws over a freshly CLEARED depth buffer, so it is
            // the one overlay that is guaranteed to sit on top of the texture the user is
            // trying to watch.  Nothing else about the brush changes: the textured pass
            // above still drew it.
            if ( KiwiUvEd_OverlaySuppressed( def, -1 ) )
                continue;
            // KIWI-UX: selected MODELS/PREFABS show as the red tint alone — the tech-29
            // SkinModelInst white triangle mesh reads as noise on dense meshes.  Brushes
            // and plain fixedsize boxes keep the outline (it is their only selection cue).
            {
                entity_s     *owner = b->owner;
                entity_s_def *eDef  = owner ? (entity_s_def *)owner->def : nullptr;
                eclass_t     *ec    = eDef ? eDef->eclass : nullptr;
                if ( ec && *(int *)&ec->fixedsize && ( ec->classtype & 0x18 ) )   // MODEL|PREFAB
                    continue;
            }
            // KIWI Terrain Sculpt paint modes hide the selected-patch white wireframe (this
            // tech-29 pass IS the triangle grid over a selected terrain); Tab toggles it.
            {
                extern bool KiwiTerrain_HideWireframe();   // kiwi_terrain.cpp
                if ( b->patch && KiwiTerrain_HideWireframe() )
                    continue;
            }
            // drawFlags=1 (force-draw) as in the binary; viewType -1 = no 2D cull.
            DrawBrush( b, (const orientation_t *)world_orient_matrix, /*viewType*/ -1,
                       /*technique*/ TECHNIQUE_WIREFRAME_SHADED, &whiteCol, /*width*/ 1, /*drawFlags*/ 1, /*layerPrefix*/ "" );
        }
        R_AddEditorSurfsCmd();                              // flush the model wireframe surfs
    }

    // Selection overlay: highlight Ctrl+Shift+LMB-picked faces (TASK 2 / P5.7).
    Cam_DrawSelectedFaces();

    // 0x40c9f0  DrawConnectionLinks, at the Cam_Draw tail exactly as in XY_Draw.  Two parts in
    // this order: the sel_vertex/sel_edge grab handles, then the target/targetname +
    // script_linkTo connection lines.  Both are world-space and view-agnostic.
    extern void Ed_DrawVertexHandles();     // xywnd.cpp — DrawConnectionLinks handle prefix
    extern void Ed_DrawConnectionLines();   // xywnd.cpp (Lines_AddLinkTo + Lines_AddLinkToScript)
    extern void Draw_PatchSelectPointsSelected();  // brush.cpp (0x40c360) — SELECTED curve pts, light blue
    Draw_PatchSelectPointsSelected();       // DrawConnectionLinks prefix — before the handles
    Ed_DrawVertexHandles();
    Ed_DrawConnectionLines();

    // 3D-marquee selection box: with a box-drag active in the 3D view (the drag
    // callback is Camera_GetRectSelection3D and d_select_mode is a marquee/point-rect mode),
    // project the four drag-rect corners to a fixed near depth and draw the translucent blue
    // quad: rect normalise, 4 CameraCalcRayDir corners, scale = 4.001/(ray.vpn),
    // corner = ray*scale + camera.origin, w=1.
    extern void Ed_DrawSelectionBoxQuad( const float (*verts)[4] );           // xywnd.cpp 0x40CC50
    const select_t sm = g_qeglobals.d_select_mode;
    if ( g_qeglobals.camera_fov_setup == (void *)Camera_GetRectSelection3D
      && ( sm == sel_area || sm == sel_areapoint_vertex || sm == sel_areabrush
        || sm == sel_areabrush_sub || sm == sel_areapoint_curve || sm == sel_areapoint
        || sm == sel_editpoint ) )
    {
        // Normalise the drag rect: x_1/x_2 = press, y_1/y_2 = current (the binary's min/max swap).
        int a4, halfx_i, drag_y2, a1_i;
        if ( g_qeglobals.drag_selectionbox_x_1 >= g_qeglobals.drag_selectionbox_y_1 )
        { a4 = g_qeglobals.drag_selectionbox_y_1; halfx_i = g_qeglobals.drag_selectionbox_x_1; }
        else
        { a4 = g_qeglobals.drag_selectionbox_x_1; halfx_i = g_qeglobals.drag_selectionbox_y_1; }
        if ( g_qeglobals.drag_selectionbox_x_2 >= g_qeglobals.drag_selectionbox_y_2 )
        { drag_y2 = g_qeglobals.drag_selectionbox_y_2; a1_i = g_qeglobals.drag_selectionbox_x_2; }
        else
        { drag_y2 = g_qeglobals.drag_selectionbox_x_2; a1_i = g_qeglobals.drag_selectionbox_y_2; }

        const float depth = 4.000999927520752f;
        const float *o    = camera.origin;
        float rays[4][3];
        // 4 corner rays in the binary's order (drag_y2,a4)/(a1,a4)/(a1,halfx)/(drag_y2,halfx).
        CameraCalcRayDir( a4,      rays[0], drag_y2 );
        CameraCalcRayDir( a4,      rays[1], a1_i );
        CameraCalcRayDir( halfx_i, rays[2], a1_i );
        CameraCalcRayDir( halfx_i, rays[3], drag_y2 );
        float verts[4][4];
        for ( int i = 0; i < 4; ++i )
        {
            float dn = rays[i][2] * camera.vpn[2] + rays[i][1] * camera.vpn[1]
                     + rays[i][0] * camera.vpn[0];
            float s  = depth / dn;
            verts[i][0] = rays[i][0] * s + o[0];
            verts[i][1] = rays[i][1] * s + o[1];
            verts[i][2] = rays[i][2] * s + o[2];
            verts[i][3] = 1.0f;
        }
        Ed_DrawSelectionBoxQuad( verts );
    }

    // KIWI: the binary's terrain-paint cursor ring (0x408953-0x408aa5) sat here; it went with
    // the AdvPatchEditDlg paint path (Terrain Sculpt draws its own brush).
    (void)hwnd;

    // KIWI-UX (ROUND BM, ITEM 1b): …AND THE SECTION CLIP CLOSES HERE, before the
    // overlay block below.  Everything after this line is a TOOL — the hover
    // outline, the construction scaffolding, the gizmo, the lollipops, the section's
    // OWN handle — and a tool that vanished because it sat on the hidden side of the
    // cut would be unusable: the section lollipop's stem points along the plane
    // normal, i.e. INTO the clipped half, so the ball would be the first thing to go.
    KiwiSection_EmitClipEnd();

    // KIWI-UX (RADIANT_UX_DESIGN §18): hover outline + active-item accent.  LAST on purpose —
    // it rides after the selected-outline pass's depth clear, so the highlight shows through
    // geometry the same way the ported selection outline and vertex handles do.  Additive
    // only: it never re-renders selected brushes, and it is bounded to one hovered item plus
    // one active item (kiwi_hover.cpp).
    {
        extern void KiwiHover_DrawWorld();    // kiwi_hover.cpp
        KiwiHover_DrawWorld();
        // Phase 4 (§7/§8/§16): construction geometry — the region fills (a triangle
        // draw, bracketed exactly as the ported selected-face fill brackets itself),
        // then the construction lines, their point markers and, while a drawing tool
        // runs, the finite construction-plane grid patch.  BEFORE the active command's
        // overlay so a live tool's rubber band reads on top of its own scaffolding.
        // Self-budgeted (KCON_DRAW_SEGMENTS) and emits nothing when the store is
        // empty or the toggle is off.
        extern void KiwiCon_DrawWorld();      // kiwi_construct.cpp
        KiwiCon_DrawWorld();
        // KIWI-UX (ROUND AG, ITEM 6): the LIVE MARQUEE PREVIEW — what the box
        // select would take if the button came up now, outlined in the §18 hover
        // cyan.  Here, after the construction pass and before the active command's
        // overlay, for the same reason KiwiCon_DrawWorld sits where it does: a
        // marquee is scaffolding, and a live tool's rubber band reads on top of
        // it.  Self-budgeted, throttled to rect CHANGES, and emits nothing when no
        // marquee is live (kiwi_boxselect.h).
        extern void KiwiBox_DrawPreview();    // kiwi_boxselect.cpp
        KiwiBox_DrawPreview();
        // KIWI-UX (ROUND AI, ITEM 6): PATCH VERTEX MODE's control lattice and its
        // point markers.  Drawn from the mode's OWN latched patch list rather than
        // through the ported Patch_DrawControlPoints, because that one is gated on
        // the patch still being on `selected_brushes` — and the instant the user
        // clicks a control point the modern selection replaces the object item with
        // a vertex item and the patch leaves that list, so the ported draw would
        // stop exactly when the points are needed most (kiwi_patchverts.h).
        // Self-budgeted; emits nothing when the mode is off.
        extern void KiwiPatchVerts_DrawWorld();   // kiwi_patchverts.cpp
        KiwiPatchVerts_DrawWorld();
        // Phase 2 (§4/§6): the ACTIVE modal command's live overlay plus its snap marker,
        // in the same tail and under the same rules (bounded batch, additive only).
        // Emits nothing when no command is running.
        // ROUND N (§6): the SNAP ACCENTS — small dark dots at the corners and centre of
        // whatever face is under the cursor, plus (KIWI 2026-09-25) black nubs on the
        // midpoints and quarter points of every edge near the cursor, while a
        // PLACEMENT tool is live, so the tool advertises where it can land
        // before the cursor is near any of them (kiwi_snap.h).  BEFORE the command's
        // own overlay so the live rubber band and the marker read on top of the
        // guidance rather than under it.  Self-budgeted; emits nothing when no
        // click-taking command is running or the marker toggle is off.
        extern void KiwiSnap_DrawFaceAccents();   // kiwi_snap.cpp
        KiwiSnap_DrawFaceAccents();
        // ROUND P (§6): the AXIS GUIDES — dashed grey lines along world Z and the
        // working plane's own axes, through the drawing tool's LAST PLACED POINT,
        // shown while the cursor is near one of them.  This is the "make drawing a
        // vertical Z line easy, the camera angle should help" directive
        // (kiwi_snap.h).  Same slot and same rules as the accents above: its own
        // budgeted batch, nothing emitted unless a click-taking command is live,
        // the marker toggle is on and the last query actually found an axis.
        extern void KiwiSnap_DrawAxisGuides();    // kiwi_snap.cpp
        KiwiSnap_DrawAxisGuides();
        // ══════════════════════════════════════════════════════════════════
        //  KIWI-UX (ROUND AQ, ITEM 2) — THE REGION FILLS, RELOCATED.
        // ══════════════════════════════════════════════════════════════════
        // USER REPORT, verbatim: "You seriously need to fix the light blue
        // construction plane visibility!"  Fifth round on it, and no console
        // diagnostic has ever come back, so this round stops waiting for one and
        // removes the LAST difference between this fill and the one fill in this
        // editor that is CONFIRMED VISIBLE — the boolean's red operand preview.
        //
        // That preview draws from HERE: KiwiCmd_DrawWorld -> the active command's
        // DrawWorld -> kiwi_boolean.cpp FillBrush.  The region fills used to draw
        // from KiwiCon_DrawWorld, three calls earlier in this same block, and
        // round AM's audit had already equalised everything else about the two
        // submissions (same material, same TECHNIQUE_UNLIT, same 0.22 alpha, same
        // per-vertex colour under the same neutral MATERIAL_COLOR bracket, same
        // KiwiTris_OrientToEye).  Pass LOCATION was what was left, so the fills
        // move into the boolean's slot: same relative order to the selected-
        // outline depth clear and its R_SortMaterials above, same bracket
        // discipline, and inside an OPEN-then-flushed KiwiLines batch exactly as
        // the boolean's fill is (KiwiCmd_DrawWorld's own Begin/Flush pair).  The
        // empty batch costs one flush of zero vertices, which emits nothing.
        //
        // IF THE FILL APPEARS, location was the bug.  IF IT DOES NOT, there are
        // now ZERO differences between a visible fill and an invisible one, the
        // whole submission-side model is falsified, and the next round starts at
        // the renderer instead of at this layer.
        {
            extern bool KiwiCon_ShowConstruction();   // kiwi_construct.h:695
            extern void KiwiRegion_DrawFills( int highlightIndex );  // kiwi_region.h:332
            extern void KiwiLines_Begin( int maxSegments, int width );   // kiwi_lines.h:34
            extern void KiwiLines_Flush();                               // kiwi_lines.h:43
            if ( KiwiCon_ShowConstruction() )
            {
                KiwiLines_Begin( 8, 2 );
                KiwiRegion_DrawFills( -1 );
                KiwiLines_Flush();
                // KIWI: the fills wash the lines under them; the selection goes back on top.
                KiwiCon_DrawSelectionOverlay();
            }
        }
        extern void KiwiCmd_DrawWorld();      // kiwi_command.cpp
        KiwiCmd_DrawWorld();
        // Shakeout A (§14): the translate gizmo — three axis arrows, three plane
        // corners and the free-move square, screen-constant in size. LAST so its
        // handles read on top of everything else in this block; self-budgeted
        // (KGIZMO_MAX_SEGMENTS) and emits nothing while a modal command it did not
        // start is running, while its own toggle is off, or with nothing movable
        // selected.
        extern void KiwiGizmo_DrawWorld();    // kiwi_gizmo.cpp
        KiwiGizmo_DrawWorld();
        // ROUND K (§14): the Plasticity LOLLIPOP — a thin circle on the face being
        // pushed, a stem out along its (signed) normal and a ball on the end.  It
        // REPLACES the gizmo for face push/pull and both extrudes rather than
        // joining it: kiwi_gizmo.cpp's GizmoUsable() refuses outright while one is
        // wanted, so exactly one of these two ever emits (kiwi_lollipop.h).
        extern void KiwiLollipop_DrawWorld(); // kiwi_lollipop.cpp
        KiwiLollipop_DrawWorld();
        // KIWI-UX (ROUND BM, ITEM 1b): the SECTION plane's outline and its own
        // lollipop.  Beside the command lollipop deliberately — same visual
        // language, same screen-constant sizing — but it is NOT the same handle:
        // this one exists with no command running (kiwi_section.h), so the two can
        // legitimately both be on screen and neither stands the other down.
        // Self-gated (nothing while no section is armed) and self-budgeted, like
        // every other entry in this tail.
        extern void KiwiSection_DrawWorld();  // kiwi_section.cpp
        KiwiSection_DrawWorld();
        // KIWI-UX: the SUN HELPER — a warm glyph where the sun is, its direction
        // arrow, and (only while the helper is selected) the orthographic volume
        // the sun projects through the biggest brush.  Beside the section handle
        // for the same reason that one sits here: it exists with no command
        // running, and it is a screen-constant handle that must read on top of
        // the world.  Self-gated (nothing at all unless the worldspawn carries a
        // "sundirection") and self-budgeted, like every other entry in this tail.
        extern void KiwiEntArrow_DrawWorld(); // KIWI-UX: Declare the bounded selected-entity facing overlay.
        extern void KiwiSun_DrawWorld();      // kiwi_sun.cpp
        KiwiLight_DrawWorld();                // KIWI-UX: Draw bounded selected-light extents before the sun helper.
        KiwiSun_DrawWorld();
        KiwiEntArrow_DrawWorld();             // KIWI-UX: Draw facing arrows immediately after the sun helper.
        extern void KiwiCam_DrawOrbitDebug(); // kiwi_camera.cpp: View > Debug: Orbit Pivot Probe
        KiwiCam_DrawOrbitDebug();
        // KIWI-UX (ROUND AX, ITEM 6): the entity-browser DRAG GHOST — the box the
        // entity would land in, drawn while its payload hovers the camera image.
        // Self-gated (nothing while no drag is over the camera) and self-budgeted
        // (one 12-segment batch), like every other entry in this tail.  The box was
        // resolved read-only by the drop target during the previous ImGui frame; this
        // side only draws, so the picker and the camera basis are untouched here.
        extern void KiwiEntBrowser_DrawGhost();   // kiwi_entbrowser.cpp
        KiwiEntBrowser_DrawGhost();
        extern void KiwiModelBrowser_DrawGhost(); KiwiModelBrowser_DrawGhost(); // KIWI-UX
        // KIWI (2026-10-04): last compile's "fragile corner" spots, blinking like the engine's
        // out-of-bounds clear.  LAST because it draws without a depth test (kiwi_fragile.cpp).
        extern void KiwiFragile_DrawCamera(); KiwiFragile_DrawCamera();
    }



}

// The camera window's 3D mouse interaction: CamWnd_DropModelsToPlane, CameraCalcRayDir,
// Camera_GetRectSelection3D, Cam_MouseControl, Cam_MouseUp and the OnLButton handlers (the
// window itself is CamWnd_WndProc, at the bottom).  The alt+ctrl DROP-MODEL / duplicate /
// curve-point-drag sub-paths (prefs-gated, off by default) are an unported TODO in the
// dispatcher.
extern void  Drag_Begin( void *pressFunc, unsigned int buttons, int viewz,
                         int px, int py, float *xvec, float *yvec,
                         float *trace_start, float *trace_dir );          // drag.cpp 0x47E890
extern void  Drag_MouseUp( unsigned int buttons );                       // drag.cpp 0x4802A0
extern int   g_nPatchClickedView;                                        // 0x73B108
extern char  g_bXYViewIsLastPatchClick;                                  // 0x25D5A6A
// Cam_MouseMoved drag-select / texture rotate-shift dispatch deps:
extern void  Drag_MouseMoved( int x, int y, int buttons, float *origin, float *dir ); // drag.cpp 0x47FF30
extern void  Brush_ShiftTexture( float ds, float dt );                   // select.cpp 0x491F20
extern void  Brush_RotateTexture( int deg );                             // select.cpp 0x4929F0
extern void  Sys_GetCursorPos( int *x, int *y );                         // win_qe3.cpp 0x499C90 (GetCursorPos wrapper)

// 0x403b30  CameraCalcRayDir - screen (x,y) -> normalised world pick ray.
static void CameraCalcRayDir( int y, float *dir, int x )
{
    const camwndState_t *cam = &g_camwndState;
    int    height = cam->camera.height > 0 ? cam->camera.height : 1;   // KIWI: no inf ray on a 0-height cell
    double t      = tan( DEG2RAD( g_PrefsDlg->camera_fov ) * 0.5 );
    float  s      = (float)( ( t * 0.75 + t * 0.75 ) / (double)height );
    float  yf     = (float)( (double)( y - height / 2 ) * s );
    float  xf     = (float)( s * (double)( x - cam->camera.width / 2 ) );
    dir[0] = cam->camera.vup[0] * yf + cam->camera.vright[0] * xf + cam->camera.vpn[0];
    dir[1] = cam->camera.vright[1] * xf + cam->camera.vpn[1] + cam->camera.vup[1] * yf;
    dir[2] = yf * cam->camera.vup[2] + xf * cam->camera.vright[2] + cam->camera.vpn[2];
    Vec3Normalize_R( dir );
}

// KIWI-UX: public forwarder for CameraCalcRayDir (static above), so the unified pick API
// (kiwi_pick.cpp) builds its ray with the SAME math the ported picker uses instead of a
// second copy.  `y` is bottom-left origin, exactly as CameraCalcRayDir wants it (the shell
// flips with g_camwndState.height - imageY - 1).
//
// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND M) — THE ORTHOGRAPHIC PICK RAY, AND ITS INVERSE
// ═════════════════════════════════════════════════════════════════════════════
// This forwarder is KIWI code (the ported static above is untouched), so the
// ortho branch lives HERE and the binary's CameraCalcRayDir keeps its perspective
// math for its own callers (Camera_GetRectSelection3D's marquee frustum, the
// 3D-marquee quad and the terrain cursor, all inside camwnd.cpp).
//
// WRITE THE PAIR DOWN, because a picker whose forward and inverse disagree breaks
// every click and every snap silently.  Let
//     s   = ( 2 * tan(fov/2) * 0.75 ) / height          (the per-pixel scale)
//     xf  = ( x - width/2  ) * s                        (INTEGER halves — as in
//     yf  = ( y - height/2 ) * s                         CameraCalcRayDir)
//     H   = KiwiCam_OrthoHalfHeight() = dist * tan(fov/2) * 0.75
// Note H = dist * s * height/2, i.e. yf * dist is exactly the world offset from
// the view axis at the pivot plane — which is why the two projections agree there.
//
//   PERSPECTIVE                              ORTHOGRAPHIC
//     org = camera.origin                      org = camera.origin
//                                                  + vright*xf*dist + vup*yf*dist
//     dir = norm( vpn + vright*xf + vup*yf )   dir = vpn                (unit)
//
//   FORWARD  (world -> pixel), kiwi_pick.cpp ProjectRaw:
//     rel = world - camera.origin
//     z   = dot(rel,vpn)                       z   = dot(rel,vpn)   [depth only]
//     xf  = dot(rel,vright) / z                xf  = dot(rel,vright) / dist
//     yf  = dot(rel,vup)    / z                yf  = dot(rel,vup)    / dist
//     x   = xf/s + width/2                     x   = xf/s + width/2      (same)
//     y   = yf/s + height/2                    y   = yf/s + height/2     (same)
//
// So the ONLY difference is that the perspective divide by the point's own depth
// becomes a divide by the CONSTANT pivot distance — and correspondingly the ray
// gains a lateral origin offset instead of a lateral direction.  Round-tripping
// pixel -> ray -> (a point on it) -> pixel is exact in both modes.
//
// `dist` above is the ORBIT distance, but it is recovered as H / tanY rather than
// read from KiwiCam_Distance() — that way the half-height's own clamps apply
// identically here, in kiwi_pick.cpp's inverse and in the projection matrix, and
// the four consumers cannot drift apart.
void Ed_CameraCalcRayDir( int x, int y, float *dir )
{
    if ( KiwiCam_Ortho() )
    {
        const camera_s *c = Ed_Camera();
        dir[0] = c->vpn[0];
        dir[1] = c->vpn[1];
        dir[2] = c->vpn[2];          // already unit (AngleVectors), no normalise needed
        return;
    }
    CameraCalcRayDir( y, dir, x );
}

// KIWI-UX (ROUND M): the ORIGIN half of the pair above.  Perspective rays all
// start at the eye, so this is the eye; ortho rays are parallel and start on the
// image plane, so this offsets by the pixel's lateral world position.  `y` is
// bottom-left origin, same as Ed_CameraCalcRayDir.
void Ed_CameraCalcRayOrigin( int x, int y, float *org )
{
    const camera_s *c = Ed_Camera();
    org[0] = c->origin[0];
    org[1] = c->origin[1];
    org[2] = c->origin[2];
    if ( !KiwiCam_Ortho() )
        return;

    const int    height = c->height > 0 ? c->height : 1;
    const double t      = tan( DEG2RAD( g_PrefsDlg->camera_fov ) * 0.5 );
    const float  tanY   = (float)( t * 0.75 );
    if ( !( tanY > 1.0e-6f ) )
        return;                                   // degenerate FOV pref — eye ray
    const float  s   = (float)( ( t * 0.75 + t * 0.75 ) / (double)height );
    const float  yf  = (float)( y - c->height / 2 ) * s;
    const float  xf  = (float)( x - c->width  / 2 ) * s;
    // xf/yf are the PER-UNIT-DEPTH offsets, so they scale by the distance at which
    // the ortho and perspective images agree — the pivot.  `dist` is recovered from
    // the half-height (H = dist * tanY) rather than read from KiwiCam_Distance() so
    // that the half-height's own clamps apply here too; kiwi_pick.cpp's ProjectRaw
    // recovers it with the SAME expression.
    //
    // NOTE the offset is along vright/vup, both PERPENDICULAR to vpn, so this moves
    // the ray sideways WITHOUT changing its depth: the parallel rays all start on
    // the plane through the eye.  The lead below is the only depth change, and it is
    // there so an ortho pick can reach the geometry an ortho view draws behind the
    // eye plane (see KCAM_ORTHO_PICK_LEAD).
    const float  dist = KiwiCam_OrthoHalfHeight() / tanY;
    // KIWI-UX (ROUND BM, ITEM 1a): round BK clamped this lead to the eye-anchored
    // section depth.  That section is gone; the lead is the constant again.  Picks
    // are kept out of section-clipped geometry by kiwi_section.cpp instead, at the
    // one place that covers EVERY pick user (kiwi_pick.cpp's `Pick`).
    for ( int k = 0; k < 3; ++k )
        org[k] += c->vright[k] * ( xf * dist ) + c->vup[k] * ( yf * dist )
                - c->vpn[k] * KCAM_ORTHO_PICK_LEAD;
}

// 0x403c10  Camera_GetRectSelection3D - Drag_Begin's 3D marquee-frustum callback: 4 corner rays
// + the camera origin -> 4 side planes, normal = cross(ray[i], ray[i-1]).
// PLANE STRIDE IS 32 BYTES: normal{x,y,z}@0/4/8, dist as an 8-byte DOUBLE @16, type(int)=-1
// @24.  The double matters - Patch_SelectAreaPointsPass reads it back as *(double*)(plane+16).
void Camera_GetRectSelection3D( int x1, int y1, int x2, int y2, float *outPlanes )
{
    int xlo = ( x1 < x2 ) ? x1 : x2;
    int xhi = x2 + x1 - xlo;
    int ylo = ( y1 < y2 ) ? y1 : y2;
    int yhi = y2 + y1 - ylo;
    float rays[4][3];
    CameraCalcRayDir( ylo, rays[0], xlo );
    CameraCalcRayDir( yhi, rays[1], xlo );
    CameraCalcRayDir( yhi, rays[2], xhi );
    CameraCalcRayDir( ylo, rays[3], xhi );
    const float *o = Ed_Camera()->origin;
    for ( int i = 0; i < 4; ++i )
    {
        float *plane = outPlanes + 8 * i;                 // 32-byte plane stride
        Vec3Cross( rays[i], rays[( i - 1 ) & 3], plane );
        // dist (double @ +16): the float dot widened to double.
        *(double *)( plane + 4 ) = (double)( plane[0]*o[0] + plane[1]*o[1] + plane[2]*o[2] );
        *(int *)( plane + 6 ) = -1;                       // type marker @ +24
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// 0x4248a0  CCamWnd::Scroll — the mouse-wheel camera DOLLY, driven by
// CMainFrame::OnScroll (0x42b850; here Radiant_OnMouseWheel) when the wheel is over the
// camera pane and the CameraUseWheel pref is on.  Step = m_nMoveSpeed * 0.7 * amount *
// modifier, where the modifier is Shift 0.1 / Alt 1.6 / neither 0.4, and holding Ctrl
// zeroes the PITCH so the dolly stays horizontal.  Moves along -step * dir(pitch, yaw);
// OnScroll passes amount = -1 for wheel-forward, +1 for wheel-back.
// ═════════════════════════════════════════════════════════════════════════════
void CamWnd_Scroll( float amount )
{
    camwndState_t *cam = &g_camwndState;

    float factor;
    if ( GetKeyState( VK_SHIFT ) < 0 )
        factor = amount * 0.1f;
    else if ( GetKeyState( VK_MENU ) < 0 )
        factor = amount * 1.6f;
    else
        factor = amount * 0.4f;

    const float dist = (float)( (double)g_PrefsDlg->m_nMoveSpeed * 0.699999988079071
                                * (double)factor );

    const float yaw   = cam->camera.angles[1];
    // Ctrl held pins the pitch to 0 (horizontal dolly); otherwise the
    // NEGATED pitch is used.
    const float pitch = ( GetKeyState( VK_CONTROL ) < 0 ) ? 0.0f : -cam->camera.angles[0];

    // KIWI: true double deg->rad (the binary's was the float constant widened).
    const double yawR   = (double)yaw   * KIWI_DEG2RAD;
    const double pitchR = (double)pitch * KIWI_DEG2RAD;
    const float cy = (float)cos( yawR ),   sy = (float)sin( yawR );
    const float cp = (float)cos( pitchR ), sp = (float)sin( pitchR );

    g_nUpdateBits |= 1u;
    const float step = -dist;
    cam->camera.origin[0] += cp * cy * step;
    cam->camera.origin[1] += cp * sy * step;
    cam->camera.origin[2] += -sp * step;
}

// 0x403950  Cam_MouseControl - RMB-hold cursor-joystick free-look / forward-back fly.
// Reached one-shot from CamWnd_DropModelsToPlane (plain RMB, no Alt) and re-driven every idle
// by Radiant_RoutineProcessing (mainfrm.cpp), hence non-static.
void CamWnd_MouseControl( float dtime )
{
    camwndState_t *cam = &g_camwndState;

    if ( g_PrefsDlg->m_nMouseButtons == 2 )
    {
        if ( cam->m_nCambuttonstate != 6 )
            return;
    }
    else if ( !( cam->m_nCambuttonstate == 2 && ( GetAsyncKeyState( VK_MENU ) & 0x8000 ) == 0 ) )
    {
        return;
    }
    if ( g_PrefsDlg->camera_mode )
        return;
    if ( cam->camera.width < 2 || cam->camera.height < 2 )
        return;   // KIWI: a collapsed cell - the half-size divides below hit 0 (inf / NaN camera)

    float vert  = (float)( (double)( cam->m_ptButton.y - cam->camera.height / 2 ) / (double)( cam->camera.height / 2 ) );
    float horiz = (float)( (double)( cam->m_ptButton.x - cam->camera.width  / 2 ) / (double)( cam->camera.width  / 2 ) );
    float lateral = (float)( ( 1.0 - fabs( vert ) ) * horiz );
    float turn;
    if ( lateral >= 0.0f )
    {
        turn = lateral - 0.1f;
        if      ( turn < 0.0f )          turn = 0.0f;
        else if ( turn > 0.33000001f )   turn = 0.33000001f;
    }
    else
    {
        turn = lateral + 0.1f;
        if      ( turn > 0.0f )          turn = 0.0f;
        else if ( turn < -0.33000001f )  turn = -0.33000001f;
    }
    g_nUpdateBits |= 4 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
    float speed = (float)( vert * dtime * ( (double)g_PrefsDlg->m_nMoveSpeed * 6.0 ) );
    cam->camera.origin[0] += cam->camera.forward[0] * speed;
    cam->camera.origin[1] += cam->camera.forward[1] * speed;
    cam->camera.origin[2] += cam->camera.forward[2] * speed;
    cam->camera.angles[1] -= dtime * turn * 1250.0f;
}

// 0x403d30  CamWnd_DropModelsToPlane - the camera button dispatcher (the 3D analogue of
// XY_MouseDown): a selection-button combo opens a Drag_Begin pick/drag with the perspective
// ray; plain RMB without Alt runs the free-look fly.  The alt+ctrl DROP-MODEL
// (m_bDropModel/m_bOrientModel, off by default), curve-point-drag and alt+shift duplicate-drop
// branches are unported; plain select never reaches them.
void CamWnd_DropModelsToPlane( long x, long y, unsigned int nFlags )
{
    camwndState_t *cam = &g_camwndState;

    // m_ptCursor is the free-look pivot that Cam_PositionDrag / Cam_Rotate /
    // Cam_Rotate2 / Cam_PositionPan read and pin the cursor back to on each move.
    GetCursorPos( &cam->m_ptCursor );
    cam->m_nCambuttonstate = nFlags;
    cam->m_ptButton.x = x;
    cam->m_ptButton.y = y;
    int v6 = ( g_PrefsDlg->m_nMouseButtons != 2 ) ? 16 : 2;
    if ( nFlags & 2 )
        cam->cam_was_not_dragged = true;

    bool isSelect = ( nFlags == 1 || nFlags == 5 || nFlags == 9 || nFlags == 13
                   || nFlags == (unsigned)v6 || nFlags == (unsigned)( v6 | 4 )
                   || nFlags == (unsigned)( v6 | 8 ) || nFlags == (unsigned)( v6 | 0xC ) );
    if ( !isSelect )
    {
        if ( nFlags != 2 )
            return;
        if ( GetAsyncKeyState( VK_MENU ) >= 0 )      // plain RMB (no Alt) → free-look fly
        {
            CamWnd_MouseControl( g_qeglobals.g_oldtime );
            return;
        }
        // Alt+RMB falls through to a marquee drag.
    }

    bool sameView = ( g_nPatchClickedView == 1 );
    g_nPatchClickedView      = 1;
    g_bXYViewIsLastPatchClick = sameView ? 1 : 0;
    float dir[3];
    CameraCalcRayDir( (int)y, dir, (int)x );
    Drag_Begin( (void *)Camera_GetRectSelection3D, nFlags, 2, (int)x, (int)y,
                cam->camera.vright, cam->camera.vup, cam->camera.origin, dir );
}

// 0x404f70  Cam_MouseUp.
static int Cam_MouseUp( unsigned int flags )
{
    camwndState_t *cam = &g_camwndState;

    cam->m_nCambuttonstate = 0;
    Drag_MouseUp( flags );
    cam->prob_some_cursor = 0;
    cam->x47 = 0;
    cam->cursor_visible = 1;
    int r;
    do { r = ShowCursor( TRUE ); } while ( r < 0 );
    return r;
}

// UI-rework: force-teardown of an in-progress camera drag/free-look when the shell detects
// input ownership was lost ABNORMALLY — a popup from another window stole the OS mouse, so the
// WM_RBUTTONUP that would run Cam_MouseUp never arrived. Free-look repeatedly ShowCursor(FALSE)s
// and only Cam_MouseUp's ShowCursor(TRUE)-until-visible loop restores it; without this the
// cursor stays invisible app-wide. Runs the SAME restore (no context menu, no Drag side effects
// beyond the normal up). The ShowCursor loop stops at >=0, so calling this when the cursor is
// only mid-hidden brings the counter to exactly 0 — no over-increment.
void CamWnd_AbortDrag()
{
    Cam_MouseUp( 0 );
}

// ── KIWI-UX (ROUND BG, ITEM 2): "SHOULD THE CURSOR BE HIDDEN RIGHT NOW?", PER FRAME ──
// USER REPORT: "My mouse went invisible while using the UV editor."  The root cause is
// fixed at its source in imgui_shell.cpp (a legacy move handler was being fed a button
// mask for a press it never saw), but the SHAPE of the bug is what this function exists
// for: every hide in this file is a LATCH — Cam_PositionDrag / Cam_Rotate / Cam_Rotate2 /
// Cam_PositionPan and the two texture-drag arms each call ShowCursor(FALSE) on EVERY move
// and set cursor_visible = 0, and the only restore is Cam_MouseUp's
// ShowCursor(TRUE)-until-visible loop.  Any path that loses that release loses the cursor
// for the whole session, app-wide.
//
// So the shell asks this once per input tick instead: `legacyCamDragLive` is "a camera
// drag that this shell OWNS is still held".  cursor_visible is the port's own model of the
// hide (0 == at least one unmatched ShowCursor(FALSE)), so:
//   nothing hidden                      -> nothing to do
//   hidden AND a legacy camera drag live-> correct, leave it hidden
//   hidden AND no such drag             -> RESTORE, deterministically
// The loop is Cam_MouseUp's own and stops at >= 0, so it lands on exactly 0 and can never
// over-increment.  Focus loss, a lost release, a popup stealing the mouse and a drag that
// was never ours all reach the same restore, because none of them can keep the predicate
// true for more than the tick they happen on.
void CamWnd_CursorReconcile( bool legacyCamDragLive )
{
    camwndState_t *cam = &g_camwndState;
    if ( cam->cursor_visible )
        return;
    if ( legacyCamDragLive )
        return;
    cam->cursor_visible = 1;
    int r;
    do { r = ShowCursor( TRUE ); } while ( r < 0 );
}

// CCamWnd::OnDestroy persists the window placement through this.
extern BOOL SaveRegistryInfo( const char *pszName, void *pvBuf, int lSize );   // win_qe3.cpp 0x499940

// The binary's camera-control scheme: cursor-JOYSTICK fly + camera_mode view control + 3D LMB
// drag-select + alt texture rotate/shift.
//   OnRButtonDown / OnLButtonDown / OnMButtonDown (0x403220) -> the shared
//     dispatcher CamWnd_DropModelsToPlane (y flipped to a bottom-left origin).  Plain RMB (no
//     Alt, camera_mode 0) runs Cam_MouseControl once, then the WM_TIMER/OnIdle pump re-drives.
//   OnMouseMove -> Cam_MouseMoved (deduped on m_ptLastCursor).
//   OnRButtonUp -> Cam_ContextMenu -> Cam_MouseUp + ReleaseCapture.
//   The Cam_MouseControl pump is CMainFrame::RoutineProcessing (0x421a90; here
//   Radiant_RoutineProcessing).

// 0x4035f0  Cam_PositionDrag - camera_mode 1, RMB: yaw by cursor-dx, fly along the view forward
// (vpn flattened to the XY plane) by cursor-dy.  Cursor re-centred + hidden.
void CamWnd_PositionDrag()
{
    camwndState_t *cam = &g_camwndState;

    POINT pt;
    GetCursorPos( &pt );
    if ( pt.x == cam->m_ptCursor.x && pt.y == cam->m_ptCursor.y )
        return;
    int dx = pt.x - cam->m_ptCursor.x;
    int dy = pt.y - cam->m_ptCursor.y;
    cam->camera.angles[1] -= (float)( (double)g_PrefsDlg->m_nMoveSpeed / 500.0 * (double)dx );
    float fwd[3] = { cam->camera.vpn[0], cam->camera.vpn[1], 0.0f };
    Vec3Normalize_R( fwd );
    float move = (float)( (double)g_PrefsDlg->m_nMoveSpeed / -250.0 * (double)dy );
    cam->camera.origin[0] += fwd[0] * move;
    cam->camera.origin[1] += fwd[1] * move;
    cam->camera.origin[2] += fwd[2] * move;       // fwd[2]==0 → no Z drift (matches binary)
    SetCursorPos( cam->m_ptCursor.x, cam->m_ptCursor.y );
    cam->cursor_visible = 0;
    ShowCursor( FALSE );
    g_nUpdateBits |= 4 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
}

// 0x403700  Cam_Rotate - RMB+Shift+Ctrl free-look: yaw by cursor-dx, pitch by cursor-dy, both
// scaled by m_nMoveSpeed/500.  Cursor re-centred + hidden.
void CamWnd_Rotate()
{
    camwndState_t *cam = &g_camwndState;

    POINT pt;
    GetCursorPos( &pt );
    if ( pt.x == cam->m_ptCursor.x && pt.y == cam->m_ptCursor.y )
        return;
    int dx = pt.x - cam->m_ptCursor.x;
    int dy = pt.y - cam->m_ptCursor.y;
    cam->camera.angles[1] -= (float)( (double)g_PrefsDlg->m_nMoveSpeed / 500.0 * (double)dx );
    cam->camera.angles[0] -= (float)( (double)g_PrefsDlg->m_nMoveSpeed / 500.0 * (double)dy );
    SetCursorPos( cam->m_ptCursor.x, cam->m_ptCursor.y );
    cam->cursor_visible = 0;
    ShowCursor( FALSE );
    g_nUpdateBits |= 4 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
}

// 0x4037c0  Cam_Rotate2 - camera_mode 2, RMB: free-look at a fixed 0.35 deg/pixel.
void CamWnd_Rotate2()
{
    camwndState_t *cam = &g_camwndState;

    POINT pt;
    GetCursorPos( &pt );
    if ( pt.x == cam->m_ptCursor.x && pt.y == cam->m_ptCursor.y )
        return;
    int dx = pt.x - cam->m_ptCursor.x;
    int dy = pt.y - cam->m_ptCursor.y;
    cam->camera.angles[1] -= (float)( (double)dx * 0.3499999940395355 );
    cam->camera.angles[0] -= (float)( 0.3499999940395355 * (double)dy );
    SetCursorPos( cam->m_ptCursor.x, cam->m_ptCursor.y );
    cam->cursor_visible = 0;
    ShowCursor( FALSE );
    g_nUpdateBits |= 4 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
}

// 0x403870  Cam_PositionPan - RMB+Ctrl: strafe along view-right by cursor-dx, move world-Z by
// cursor-dy (m_nMoveSpeed/300).
void CamWnd_PositionPan()
{
    camwndState_t *cam = &g_camwndState;

    POINT pt;
    GetCursorPos( &pt );
    if ( pt.x == cam->m_ptCursor.x && pt.y == cam->m_ptCursor.y )
        return;
    int dx = pt.x - cam->m_ptCursor.x;
    int dy = pt.y - cam->m_ptCursor.y;
    float strafe = (float)( (double)g_PrefsDlg->m_nMoveSpeed / 300.0 * (double)dx );
    cam->camera.origin[0] += strafe * cam->camera.vright[0];
    cam->camera.origin[1] += strafe * cam->camera.vright[1];
    cam->camera.origin[2] += strafe * cam->camera.vright[2];
    cam->camera.origin[2] -= (float)( (double)g_PrefsDlg->m_nMoveSpeed / 300.0 * (double)dy );
    SetCursorPos( cam->m_ptCursor.x, cam->m_ptCursor.y );
    cam->cursor_visible = 0;
    ShowCursor( FALSE );
    g_nUpdateBits |= 4 * ( g_PrefsDlg->m_bCamXYUpdate != 0 ) + 1;
}

// 0x404fc0  Cam_MouseMoved - the central 3D-view mouse handler, button-mask gated: texture
// ROTATE (Ctrl+RMB+Alt) / texture SHIFT (RMB+Alt) / camera_mode view control (RMB combos) /
// 3D LMB drag-select / shift-ctrl LMB drop-to-plane.
// `y` is an INT (bottom-left-origin client coord), NOT a float.
extern "C" int ClampGridSize();                                          // drag.cpp (0x463a80)
static double Cam_MaxF( float a, float b ) { return ( a - b >= 0.0f ) ? a : b; } // 0x40A1D0 (max)
void CamWnd_MouseMoved( unsigned int buttons, int x, int y )
{
    camwndState_t *cam = &g_camwndState;

    cam->m_nCambuttonstate = buttons;
    if ( !buttons )
        return;
    if ( cam->m_ptButton.x != x || cam->m_ptButton.y != y )
        cam->cam_was_not_dragged = false;
    cam->m_ptButton.x = x;
    cam->m_ptButton.y = y;

    const bool altDown = ( GetAsyncKeyState( VK_MENU ) & 0x8000 ) != 0;

    if ( buttons == MK_RBUTTON )
    {
        if ( altDown )
        {
            // ── RMB+Alt → texture SHIFT (drag the texture in S/T by grid-snapped cursor delta) ──
            int cx, cy;
            Sys_GetCursorPos( &cx, &cy );
            if ( cx == cam->m_ptCursor.x && cy == cam->m_ptCursor.y )
                return;
            cam->prob_some_cursor += cx - cam->m_ptCursor.x;
            cam->x47             += cy - cam->m_ptCursor.y;
            float accX = (float)cam->prob_some_cursor;
            float gs   = grid_sizes[g_qeglobals.d_gridsize];
            float remX = (float)fmod( accX, gs );
            float accY = (float)cam->x47;
            float remY = (float)fmod( accY, gs );
            if ( remX != accX || remY != accY )
            {
                int snapX = (int)( accX - remX );
                cam->prob_some_cursor -= snapX;
                int snapY = (int)( accY - remY );
                cam->x47             -= snapY;
                Brush_ShiftTexture( (float)snapX, (float)snapY );
            }
            SetCursorPos( cam->m_ptCursor.x, cam->m_ptCursor.y );
            ShowCursor( FALSE );
            cam->cursor_visible = 0;
            return;
        }
        // RMB, no Alt → fall through to the camera_mode / drag-select dispatch.
    }
    else if ( buttons == ( MK_RBUTTON | MK_CONTROL ) && altDown )
    {
        // ── Ctrl+RMB+Alt → texture ROTATE (grid-snapped, ClampGridSize degrees) ──
        POINT pt;
        GetCursorPos( &pt );
        if ( pt.x == cam->m_ptCursor.x && pt.y == cam->m_ptCursor.y )
            return;
        cam->prob_some_cursor += pt.x - cam->m_ptCursor.x;
        cam->x47             += pt.y - cam->m_ptCursor.y;
        int accX = cam->prob_some_cursor;
        int accY = cam->x47;
        int step = (int)Cam_MaxF( 1.0f, grid_sizes[g_qeglobals.d_gridsize] );
        int remX = accX % step;
        bool snap = ( accX != remX );
        if ( !snap && ( accY % step ) != accY )      // (accX snapped) OR (accY snapped)
            snap = true;
        if ( !snap )
        {
            // no whole grid-step accumulated yet → just recentre, no rotate
            SetCursorPos( cam->m_ptCursor.x, cam->m_ptCursor.y );
            ShowCursor( FALSE );
            cam->cursor_visible = 0;
            return;
        }
        int snappedX = accX - remX;
        cam->prob_some_cursor = remX;
        int remY = accY % step;
        cam->x47 = remY;
        int snappedY = accY - remY;
        int deg = ClampGridSize();
        Brush_RotateTexture( ( snappedX / step + snappedY / step ) * deg );
        SetCursorPos( cam->m_ptCursor.x, cam->m_ptCursor.y );
        ShowCursor( FALSE );
        cam->cursor_visible = 0;
        return;
    }

    // ── camera_mode 1/2 view control, then the LMB drag-select / drop-to-plane ──
    if ( ( buttons & MK_RBUTTON ) != 0 && !altDown && g_PrefsDlg->camera_mode == 2 )
    {
        if ( buttons & MK_CONTROL ) CamWnd_PositionPan();
        else                        CamWnd_Rotate2();
        g_nUpdateBits |= W_CAMERA;
        return;
    }
    if ( buttons == MK_RBUTTON )
    {
        if ( !altDown && g_PrefsDlg->camera_mode == 1 )
        {
            CamWnd_PositionDrag();
            g_nUpdateBits |= W_CAMERA;
            return;
        }
    }
    else if ( buttons == ( MK_RBUTTON | MK_SHIFT | MK_CONTROL ) )
    {
        if ( !altDown )
        {
            CamWnd_Rotate();
            g_nUpdateBits |= W_CAMERA;
            return;
        }
    }
    else if ( buttons == ( MK_RBUTTON | MK_CONTROL ) && !altDown )
    {
        CamWnd_PositionPan();
        g_nUpdateBits |= W_CAMERA;
        return;
    }

    GetCursorPos( &cam->m_ptCursor );
    if ( ( buttons & ( MK_LBUTTON | MK_MBUTTON ) ) == 0 )
        return;

    if ( !g_qeglobals.toggle_unk03_mousedrag_state1 && !g_qeglobals.toggle_unk04_mousedrag_state2 )
    {
        // ── plain LMB/MMB drag: continue the 3D ray drag-select (the marquee gap is now closed) ──
        float dir[3];
        CameraCalcRayDir( y, dir, x );
        Drag_MouseMoved( x, y, buttons, cam->camera.origin, dir );
        if ( ( buttons & MK_LBUTTON ) != 0 && ( GetAsyncKeyState( VK_MENU ) & 0x8000 ) != 0 )
            g_nUpdateBits |= W_CAMERA;            // Alt+LMB terrain paint → camera only
        else
            g_nUpdateBits |= ( W_CAMERA | W_XY | W_Z );   // 0x0B
        return;
    }

    // ── a marquee/point-rect drag is in progress (toggle_unk03/04): re-dispatch or finalize ──
    if ( g_qeglobals.d_select_mode == sel_areabrush || g_qeglobals.d_select_mode == sel_areabrush_sub
      || g_qeglobals.d_select_mode == sel_areapoint_curve || g_qeglobals.d_select_mode == sel_areapoint )
    {
        g_qeglobals.toggle_unk03_mousedrag_state1 = 0;
        g_qeglobals.toggle_unk04_mousedrag_state2 = 0;
        return;
    }
    if ( buttons == ( MK_LBUTTON | MK_SHIFT )                          // 5
      || ( buttons == ( MK_LBUTTON | MK_CONTROL ) && !altDown )        // 9, no Alt
      || buttons == ( MK_LBUTTON | MK_SHIFT | MK_CONTROL ) )           // 0x0D
    {
        CamWnd_DropModelsToPlane( x, y, buttons );
        return;
    }
    g_qeglobals.toggle_unk03_mousedrag_state1 = 0;
    g_qeglobals.toggle_unk04_mousedrag_state2 = 0;
}

// KIWI: Cam_ContextMenu (0x404d40), the RMB brush-face picker popup, is gone - the classic
// context menu was removed (user directive) - and with it its pick list (camera_trace) and
// the three WM_COMMAND handlers that read it (0x404c20 per-face toggle, 0x404cd0 select all,
// 0x404d10 deselect all).

// 0x4034E0  Cam_ChangeFloor - View->Up Floor / Down Floor: drop or raise the camera so its
// "feet" (eye - 48) rest on the nearest brush surface above / below.  Casts a vertical ray DOWN
// from z = 131072 through every active brush.
//   `current` = distance from that high start down to the feet (131072 - (origin.z - 48)); a
//   hit distance t < current is ABOVE the feet, t > current is BELOW.
//   up   (a2!=0): best starts 0,      keep the LARGEST t still < current; best==0 means none.
//   down (a2==0): best starts 262144, keep the SMALLEST t still > current; 262144 means none.
//   On a hit: origin.z += (current - best) and g_nUpdateBits |= 0x21.  0x21 is the binary's
//   literal = W_CAMERA|W_Z_OVERLAY: UpdateWindows tests 0x28 for the Z window and 0x10 for
//   the texture window, so W_Z_OVERLAY==0x20.
// The comparison directions are the binary's, not simplified.
void CamWnd_ChangeFloor( int a2 )
{
    camwndState_t *cam = &g_camwndState;

    float start[3] = { cam->camera.origin[0], cam->camera.origin[1], 131072.0f };
    float dir[3]   = { 0.0f, 0.0f, -1.0f };
    float current  = 131072.0f - ( cam->camera.origin[2] - 48.0f );
    float best     = a2 ? 0.0f : 262144.0f;

    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
    {
        brush_t *def = b->def;
        if ( !def ) continue;
        float t;
        if ( !Ed_BrushFloorRay( def, start, dir, &t ) )
            continue;
        if ( a2 )
        {
            if ( current > (double)t && best < (double)t )
                best = t;
        }
        else if ( current < (double)t && best > (double)t )
        {
            best = t;
        }
    }

    if ( best != 0.0f && best != 262144.0f )
    {
        g_nUpdateBits |= 0x21;   // W_CAMERA|W_Z_OVERLAY
        cam->camera.origin[2] = current - best + cam->camera.origin[2];
    }
}

// Light-region "build regions for selected lights": OnShowRegionsForSelected (mainfrm.cpp) ->
// Regions_ForSelected -> Region_ForOneLight -> Region_BuildForLight ->
// { Brush_DrawSubmitFaceWindings / Patch_DrawSubmitFaceWindings } -> the primarylights_region CSG -> the
// global region-hull array.
// The `float *a3` the face submitters take points at the light descriptor
// (lightDesc_t {int cls; float p[8]}): a3[1..3] = cone centre, a3[7] = radius, a3[8] =
// cosHalfFov - i.e. a3 == (float*)&desc with cls reinterpreted at a3[0].
#include "primarylights_region.h"
extern int    Entity_GetIntValueForKey( const entity_s *e, const char *key );          // entity.cpp 0x483820
extern void   Brush_DrawSubmitFaceWindings( selbrush_t *inst, const orientation_t *orient,
                                            float *a3, rface_t **outList ); // brush.cpp 0x47B380

// d_lightRegionHulls - the global hull sink
// RegionLightRelated consumes after the per-light preview loop.
void *d_lightRegionHulls[1024];
int   d_lightRegionHullCount = 0;

// ─────────────────────────────────────────────────────────────────────────────
// 0x40c640  Region_DrawHull — draw ONE region hull winding as a flat
// DOUBLE-SIDED triangle fan in the white-UNLIT immediate path, flat-coloured with the
// caller's packed colour.  The winding's best-fit plane normal is stamped on every vertex
// and the UVs are zero.
// The 0x400-point cap is the binary's (its buffers hold exactly 1024 verts).
// ─────────────────────────────────────────────────────────────────────────────
static void Region_DrawHull( const winding_t *w, const GfxColor *col )
{
    if ( (unsigned)w->numpoints > 0x400 )
        return;

    float plane[4];
    Region_WindingPlane( plane, w );

    static float          s_xyzw[1024][4];
    static float          s_normal[1024][3];
    static float          s_st[1024][2];
    static float          s_color[1025];
    static ushort s_indices[6132];

    const int n = w->numpoints;
    for ( int i = 0; i < n; ++i )
    {
        s_color[i]     = *(const float *)&col->packed;
        s_xyzw[i][0]   = w->p[i][0];
        s_xyzw[i][1]   = w->p[i][1];
        s_xyzw[i][2]   = w->p[i][2];
        s_xyzw[i][3]   = 1.0f;
        s_normal[i][0] = plane[0];
        s_normal[i][1] = plane[1];
        s_normal[i][2] = plane[2];
        s_st[i][0]     = 0.0f;
        s_st[i][1]     = 0.0f;
    }

    // DOUBLE-sided fan: (0, k-1, k) then the reversed (k, k-1, 0).
    int idx = 0;
    for ( int k = 2; k < n; ++k )
    {
        s_indices[idx]     = 0;
        s_indices[idx + 1] = (ushort)( k - 1 );
        s_indices[idx + 2] = (ushort)k;
        s_indices[idx + 3] = (ushort)k;
        s_indices[idx + 4] = (ushort)( k - 1 );
        s_indices[idx + 5] = 0;
        idx += 6;
    }
    // Emitted even with idx == 0, as the binary does (the backend no-ops on 0 indices).
    R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT, (short)idx, s_indices,
                            (short)w->numpoints, s_xyzw, s_normal, s_color, s_st );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x406ac0  RegionLightRelated — Cam_Draw's light-REGION hull overlay: every
// hull produced by Regions_ForSelected drawn translucent orange {1, 0.75, 0, 0.25}.
// It is independent of the per-pixel light preview (LightPreview_DrawLight).
// ─────────────────────────────────────────────────────────────────────────────
void RegionLightRelated()
{

    float    rgba[4];
    GfxColor col;

    rgba[0] = 1.0f;
    rgba[1] = 0.75f;
    rgba[2] = 0.0f;
    rgba[3] = 0.25f;
    Byte4PackPixelColor( rgba, &col );

    for ( unsigned int i = 0; i < (unsigned int)d_lightRegionHullCount; ++i )
        Region_DrawHull( (const winding_t *)d_lightRegionHulls[i], &col );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x406c00  Region_ClearHulls — free every published hull winding and reset the count.
// Each hull IS a winding_t, so the shared winding-allocation counter drops by the
// hull count.
// ─────────────────────────────────────────────────────────────────────────────
void Region_ClearHulls()
{
    if ( !d_lightRegionHullCount )
        return;
    g_windingAlloc -= d_lightRegionHullCount;
    for ( int i = d_lightRegionHullCount; i > 0; )
        free( d_lightRegionHulls[--i] );
    d_lightRegionHullCount = 0;
}

// KIWI: the local copy here measured to the box's FAR side (maxs - p first), dropping big
// casters that reach into the light radius.  0x4A56B0 is mins - p, then p - maxs.
extern float PointToBoundsDistSq( const float *p, const float *mins, const float *maxs );   // pmesh.cpp 0x4A56B0

// 0x4852E0 — find the entity DEF whose "targetname" == name (top-level
// search; the prefab-scoped branch is unused for editor light region builds).
static entity_s_def *Region_FindTargetEntity( selbrush_t *scope, const char *name )
{
    entity_s *head = &entities;
    if ( scope )
        head = &scope->owner->modelClass->model->entities;
    entity_s *cur = head->next;
    while ( cur != head )
    {
        if ( Entity_HasEpairMatch( cur, "targetname", name ) )
            return (entity_s_def *)cur;
        cur = cur->next;
    }
    return 0;
}

static float Region_CosSum( float a1, float a2 )
{
    float v = ( 1.0f - a2 * a2 ) * ( 1.0f - a1 * a1 );
    return (float)( a2 * a1 - sqrtf( v ) );
}

// 0x4063A0  Entity_Light — classify a light + derive its cone.  Returns 2 (cone) / 3.
static int Entity_Light( const float *worldPos, const entity_s *defPtr, selbrush_t *scope,
                         const orientation_t *orient,
                         float *outDir, float *outCosInner, float *outCosOuter, float *outCosHalfFov )
{
    if ( ( Entity_GetIntValueForKey( defPtr, "spawnflags" ) & 1 ) != 0 )
        return 3;

    epair_t *ep = defPtr->epairs;
    const char *targetName = "";
    bool found = false;
    while ( ep )
    {
        const char *key = ep->key;
        if ( !_stricmp( key, "target" ) )
        {
            targetName = ep->value;
            if ( !targetName )
                return 3;
            found = true;
            break;
        }
        ep = ep->next;
    }
    (void)found;

    entity_s_def *tgt = Region_FindTargetEntity( scope, targetName );
    if ( !tgt )
        return 3;

    float tWorld[3];
    OrientationPosToWorldPos( tWorld, tgt->origin, orient );
    float dir[3] = { worldPos[0] - tWorld[0], worldPos[1] - tWorld[1], worldPos[2] - tWorld[2] };
    float len = Vec3Normalize_R( dir );

    float cosOuter;
    float fovOuter = Entity_GetFloatValueForKey( defPtr, "fov_outer" );
    if ( fovOuter == 0.0f )
        cosOuter = len / sqrtf( len * len + 4096.0f );
    else
        cosOuter = (float)cos( DEG2RAD( fovOuter ) * 0.5f );

    float cosInner = (float)cos( DEG2RAD( Entity_GetFloatValueForKey( defPtr, "fov_inner" ) ) * 0.5f );
    if ( cosOuter >= cosInner )
        return 3;

    outDir[0] = dir[0]; outDir[1] = dir[1]; outDir[2] = dir[2];

    float maxturn = Entity_GetFloatValueForKey( defPtr, "maxturn" );
    if ( maxturn == 0.0f )
    {
        *outCosHalfFov = cosOuter;
    }
    else
    {
        float c = (float)cos( DEG2RAD( maxturn ) );
        *outCosHalfFov = ( -cosOuter <= c ) ? Region_CosSum( cosOuter, c ) : -1.0f;
    }
    *outCosInner = cosInner;
    *outCosOuter = cosOuter;
    return 2;
}

// KIWI-UX: cod4map's dual-primary-bit precedence, isolated from faithful
// Entity_Light (IDB 0x4063A0).  Only a renderer-safe strict cone is promoted.
static bool Cam_LightPreview_CompilerSpotForDualBits(
    const float *origin, entity_s_def *ent, selbrush_t *scope,
    const orientation_t *orient, float *outDir, float *outCosInner,
    float *outCosOuter, float *outCosHalfFov )
{
    const entity_s *defPtr = ent;
    const char *targetName = ValueForKey2( defPtr, "target" );
    if ( !targetName || !*targetName )
        return false;

    entity_s_def *target = Region_FindTargetEntity( scope, targetName );
    if ( !target )
        return false;

    float targetWorld[3];
    OrientationPosToWorldPos( targetWorld, target->origin, orient );
    float dir[3] = {
        origin[0] - targetWorld[0],
        origin[1] - targetWorld[1],
        origin[2] - targetWorld[2]
    };
    const float length = Vec3Normalize_R( dir );

    const float fovOuter = Entity_GetFloatValueForKey( defPtr, "fov_outer" );
    const float cosOuter = ( fovOuter == 0.0f )
                         ? length / sqrtf( length * length + 4096.0f )
                         : (float)cos( DEG2RAD( fovOuter ) * 0.5f );
    const float cosInner = (float)cos(
        DEG2RAD( Entity_GetFloatValueForKey( defPtr, "fov_inner" ) ) * 0.5f );
    if ( cosOuter >= cosInner )
        return false;

    outDir[0] = dir[0];
    outDir[1] = dir[1];
    outDir[2] = dir[2];
    *outCosInner = cosInner;
    *outCosOuter = cosOuter;

    const float maxturn = Entity_GetFloatValueForKey( defPtr, "maxturn" );
    if ( maxturn == 0.0f )
    {
        *outCosHalfFov = cosOuter;
    }
    else
    {
        const float c = (float)cos( DEG2RAD( maxturn ) );
        *outCosHalfFov = ( -cosOuter <= c )
                       ? Region_CosSum( cosOuter, c )
                       : -1.0f;
    }
    return true;
}

// 0x47D180  recursive shadow-caster gatherer.  Each record = { orientation_t(0x30);
// pad; selbrush_t*@+48 } = 52 bytes.
static int Region_GatherShadowBrushes( const float *light, float radSq, selbrush_t *listHead,
                                       const orientation_t *orient, KiwiLightCasterRecord *out, int cap )
{
    if ( cap <= 0 )
        return 0;
    int n = 0;
    for ( selbrush_t *b = listHead->next; b != listHead; b = b->next )
    {
        if ( FilterBrush( b, 0 ) )
            continue;
        entity_s *owner = b->owner;
        if ( owner->prefab )
        {
            orientation_t childOr;
            Entity_GetOrientation( (entity_s_def *)owner->def, (orientation_t *)orient, &childOr );
            // recurse into the prefab's active brush list.  The prefab sentinel head is
            // the prefab's embedded brush-list; reached by the binary as
            // &owner->prefab->active_brushlist.
            selbrush_t *pfHead = &((prefab_s *)owner->prefab)->brushes;
            n += Region_GatherShadowBrushes( light, radSq, pfHead, &childOr, out + n, cap - n );
            if ( n == cap )
                return n;
            continue;
        }
        if ( radSq < PointToBoundsDistSq( light, b->def->mins, b->def->maxs ) )
            continue;
        out[n].orient = *orient;
        out[n].brush = b;
        n++;
        if ( n == cap )
            return n;
    }
    return n;
}

// 0x47D2A0  LightPreview_GatherShadowBrushes — active + selected lists.
static int LightPreview_GatherShadowBrushes( KiwiLightCasterRecord *out, const float *light, float radius )
{
    float radSq = radius * radius;
    int n = Region_GatherShadowBrushes( light, radSq, &active_brushes,
                                        (const orientation_t *)world_orient_matrix, out, 0x8000 );
    n += Region_GatherShadowBrushes( light, radSq, &selected_brushes,
                                     (const orientation_t *)world_orient_matrix, out + n, 0x8000 - n );
    return n;
}

// KIWI cache adapter; the payload and 0x8000 cap remain the binary gatherer's.
static int __cdecl Cam_LightGatherCached( KiwiLightCasterRecord *out,
                                          const float *origin, float radius )
{
    return LightPreview_GatherShadowBrushes( out, origin, radius );
}

// 0x406CE0 — submit the shadow-caster faces, add the light cube, run the
// merge driver, append the produced hulls to the global array.
static void Region_BuildForLight( int cls, const float *coneCenter, const float *coneDir,
                                  float radius, float cosHalfFov, int casterCount, const KiwiLightCasterRecord *recs )
{
    lightDesc_t desc;
    desc.cls  = cls;
    desc.p[0] = coneCenter[0]; desc.p[1] = coneCenter[1]; desc.p[2] = coneCenter[2];
    desc.p[3] = coneDir[0];    desc.p[4] = coneDir[1];    desc.p[5] = coneDir[2];
    desc.p[6] = radius;
    desc.p[7] = cosHalfFov;
    float *a3 = (float *)&desc;        // a3[1..3]=center, a3[7]=radius, a3[8]=cosHalfFov

    rface_t *faceList = 0;
    for ( int i = 0; i < casterCount; i++ )
    {
        Brush_DrawSubmitFaceWindings( recs[i].brush, &recs[i].orient, a3, &faceList );
    }
    Region_AddBoundsFaces( &faceList, a3 );   // light-cube faces (reads a3[+28] = radius)

    void *hulls[35];
    unsigned int count = (unsigned int)Region_BuildHulls( &faceList, &desc, hulls );

    Region_FreeList( faceList );

    if ( count > 8 )
        MessageBoxA( 0, va( "Cannot have more than %i regions for a light", 8 ),
                     "Too many regions", MB_ICONERROR );

    for ( unsigned int i = 0; i < count; i++ )
    {
        unsigned int idx = d_lightRegionHullCount;
        if ( idx < 0x400 ) { d_lightRegionHulls[idx] = hulls[i]; d_lightRegionHullCount = idx + 1; }
    }
}

// 0x406E00 — for one light brush, build its region if radius>0 & spawnflags&3.
static void Region_ForOneLight( selbrush_t *inst, selbrush_t *scope,
                                const orientation_t *orient )
{
    const entity_s *defPtr = inst->owner->def;
    float radius = Entity_GetFloatValueForKey( defPtr, "radius" );
    if ( radius <= 0.0f )
        return;

    brush_t *def = inst->def;
    float boxMid[3], coneCenter[3];
    boxMid[0] = ( def->mins[0] + def->maxs[0] ) * 0.5f;
    boxMid[1] = ( def->mins[1] + def->maxs[1] ) * 0.5f;
    boxMid[2] = 0.5f * ( def->mins[2] + def->maxs[2] );
    OrientationPosToWorldPos( coneCenter, boxMid, orient );

    if ( ( Entity_GetIntValueForKey( defPtr, "spawnflags" ) & 3 ) == 0 )
        return;

    KiwiLightCasterRecord *recs = (KiwiLightCasterRecord *)operator new(
        0x8000u * sizeof(KiwiLightCasterRecord) );   // throws on failure, never NULL
    int casterCount = LightPreview_GatherShadowBrushes( recs, coneCenter, radius );

    float dir[3], cosInner, cosOuter, cosHalfFov;
    int cls = Entity_Light( coneCenter, defPtr, scope, orient, dir,
                            &cosInner, &cosOuter, &cosHalfFov );
    Region_BuildForLight( cls, coneCenter, dir, radius, cosHalfFov, casterCount, recs );

    operator delete( recs );   // KIWI: was free() on operator new memory
}

// 0x406200  CCamWnd_AddLightPreview - append { inst, arg2, orient } to the camera's
// light_preview_arr[] (LightPreviewRec, 56-byte stride); no-op if `inst` is already present.
// When the 8-slot ring is full it drops the OLDEST record first (FIFO shift, count=7).
// The binary returned the cam pointer, which no caller reads, so this returns void.
void CamWnd_AddLightPreview( selbrush_t *inst, selbrush_t *arg2, const orientation_t *orient )
{
    camwndState_t *cam = &g_camwndState;

    int count = cam->light_preview_count;
    // Already present? (linear search on inst)
    if ( count > 0 )
    {
        for ( int i = 0; i < count; ++i )
            if ( (selbrush_t *)(intptr_t)cam->light_preview_arr[i].inst == inst )
                return;                      // already queued — no-op
    }
    // FIFO evict the oldest when the 8-slot ring is full.
    if ( cam->light_preview_count == 8 )
    {
        cam->light_preview_count = 7;
        for ( int i = 0; i < cam->light_preview_count; ++i )
            cam->light_preview_arr[i] = cam->light_preview_arr[i + 1];
    }
    int slot = cam->light_preview_count;
    cam->light_preview_arr[slot].inst  = inst;
    cam->light_preview_arr[slot].arg2  = arg2;
    memcpy( &cam->light_preview_arr[slot].orient, orient, 0x30u );
    ++cam->light_preview_count;
}

// The `light_preview_count = 0` write; callers: mainfrm.cpp Cmd_OnClearPreviewList, map.cpp
// Prefab_NextLevel.
void CamWnd_ClearLightPreviews()
{
    g_camwndState.light_preview_count = 0;
}

// 0x4062d0  CCamWnd light-preview-record removal, called from Brush_Free when a brush instance
// dies: linear search by inst, then shift the tail down (56-byte LightPreviewRec stride) and
// decrement.  Returns the removed index, or light_preview_count when not present.
int CamWnd_RemoveLightPreview( selbrush_t *removed )
{
    camwndState_t *cam = &g_camwndState;

    int count = cam->light_preview_count;
    int idx = 0;
    if ( count <= 0 )
        return 0;

    // find the record whose inst == removed
    while ( (selbrush_t *)(intptr_t)cam->light_preview_arr[idx].inst != removed )
    {
        if ( ++idx >= count )
            return idx;                       // not found
    }

    cam->light_preview_count = count - 1;
    // shift the tail down over the removed slot
    for ( int i = idx; i < count - 1; ++i )
        cam->light_preview_arr[i] = cam->light_preview_arr[i + 1];

    return idx;
}

// 0x406F10  Regions_ForSelected - clear the old hulls, then build regions for every selected
// light brush and every camera light-preview record.
void CamWnd_RegionsForSelected()
{
    camwndState_t *cam = &g_camwndState;

    Region_ClearHulls();

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        eclass_t *eclass = ((entity_s_def *)owner->def)->eclass;
        if ( ( eclass->classtype & 1 ) != 0 )        // light eclass bit
            Region_ForOneLight( b, nullptr, (const orientation_t *)world_orient_matrix );
    }
    for ( int i = 0; i < cam->light_preview_count; i++ )
    {
        camLightPreviewRec_t *r = &cam->light_preview_arr[i];
        selbrush_t *brush = (selbrush_t *)(intptr_t)r->inst;
        // the signed-byte gate is the brush's (brush+52), not the preview record's.
        if ( (brush->brushFlags & 0x80) == 0 )
            Region_ForOneLight( brush, (selbrush_t *)(intptr_t)r->arg2, &r->orient );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Camera-viewport handlers (U-VP-CAM).  Every afx_msg body lives here as a free function on
//  plain args; the raw-Win32 WndProc at the end of this file and the ImGui shell feed them with
//  the conventions MFC used (client coords, the MK_* wParam flag word, LOWORD/HIWORD size).
//  The flipped Y (height - y - 1) is part of the handler bodies; capture/focus belong to the
//  shell.
//
//  Callers declare these themselves:
//      extern camera_s *Ed_Camera();                                   // THE editor camera
//      extern void CamWnd_OnCreate( HWND hwnd );
//      extern void CamWnd_OnSize( HWND hwnd, int cx, int cy );
//      extern void CamWnd_Paint( HWND hwnd );
//      extern void CamWnd_OnDestroy( HWND hwnd );
//      extern void CamWnd_OnKeyDown( unsigned int nChar, unsigned int nRepCnt, unsigned int nFlags );
//      extern void CamWnd_OnLButtonDown( HWND hwnd, unsigned int nFlags, int x, int y );
//      extern void CamWnd_OnLButtonUp( unsigned int nFlags );
//      extern void CamWnd_OnRButtonDown( HWND hwnd, unsigned int nFlags, int x, int y );
//      extern void CamWnd_OnRButtonUp( HWND hwnd, unsigned int nFlags, int x, int y );
//      extern void CamWnd_OnMouseMove( HWND hwnd, unsigned int nFlags, int x, int y );
//  ...plus the non-handler entry points:
//      extern void CamWnd_BuildMatrix();
//      extern void CamWnd_Draw( HWND hwnd );
//      extern void CamWnd_ChangeFloor( int a2 );
//      extern void CamWnd_MouseControl( float dtime );
//      extern void CamWnd_MouseMoved( unsigned int buttons, int x, int y );
//      extern void CamWnd_Scroll( float amount );
//      extern void CamWnd_RegionsForSelected();
//      extern void CamWnd_AddLightPreview( selbrush_t *inst, selbrush_t *arg2, const orientation_t *o );
//      extern int  CamWnd_RemoveLightPreview( selbrush_t *removed );
//      extern void CamWnd_ClearLightPreviews();
// ═════════════════════════════════════════════════════════════════════════════

// Latch the client size (CCamWnd::OnCreate tail, after the base-class create).
void CamWnd_OnCreate( HWND hwnd )
{
    RECT rc; ::GetClientRect( hwnd, &rc );
    g_camwndState.width  = rc.right - rc.left;
    g_camwndState.height = rc.bottom - rc.top;
    g_camwndState.camera.width  = g_camwndState.width;
    g_camwndState.camera.height = g_camwndState.height;
}

void CamWnd_OnSize( HWND hwnd, int cx, int cy )
{
    g_camwndState.width  = cx;
    g_camwndState.height = cy;
    g_camwndState.camera.width  = cx;
    g_camwndState.camera.height = cy;
    // Re-create this window's swap chain at the new size (pixel-correct, no stretch).
    if ( dx.device && cx > 0 && cy > 0 )
        R_Hwnd_Resize( (HWND__ *)hwnd, cx, cy );
}

// KIWI (2026-09-18, user: "an ON/OFF option to turn the background a strobing pink/green (or
// whatever colors the engine uses for this would be a bonus!!) so I can see leaks in terrain
// easier").  View > Leak Finder Background.  The CADENCE is the engine's own: r_clear "blink"
// (gfx_d3d/r_utils.cpp R_ClearScreen) swaps r_clearColor / r_clearColor2 on
// `Sys_Milliseconds() & 0x200`, i.e. every 512 ms.  The COLOURS are not the engine's: its
// dev defaults are sky blue (0.5 0.75 1) and orange (1 0.5 0) - picked to stand out inside a
// lit level, and both read as sky / dirt over open terrain - so this uses magenta / green,
// neither of which any terrain material resembles.  While it is on the camera must keep
// drawing, or the strobe freezes on whichever colour the last repaint had.
static const float *Cam_ClearColor()
{
    extern bool KiwiUX_LeakBackground();                   // kiwi_ux.cpp
    if ( !KiwiUX_LeakBackground() )
        return g_qeglobals.d_savedinfo.colors[4];          // COLOR_CAMERABACK
    static const float s_magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };
    static const float s_green[4]   = { 0.0f, 1.0f, 0.0f, 1.0f };
    g_nUpdateBits |= W_CAMERA;                             // keep the frames coming
    return ( ::GetTickCount() & 0x200 ) == 0 ? s_magenta : s_green;
}

// The CCamWnd::OnPaint pipeline — the DC (CPaintDC / BeginPaint) belongs to the shell, not here.
void CamWnd_Paint( HWND hwnd )
{
    if ( !dx.device )
        return;

    // Keep camera.width/height in step with this window (a WM_SIZE may not have arrived yet).
    g_camwndState.camera.width  = g_camwndState.width;
    g_camwndState.camera.height = g_camwndState.height;

    // A window the renderer never registered (in the binary, the floating Dynamic-Lighting
    // popup: R_MAX_WINDOWS=5 is full at startup) renders nothing.  Skip cleanly so
    // R_SetupRendertarget_CheckDevice's invalid-hwnd assert never fires for it.
    if ( !R_IsRegisteredRenderWindow( (HWND__ *)hwnd ) )
        return;
    if ( !R_SetupRendertarget_CheckDevice( (HWND__ *)hwnd ) )
        return;

    R_BeginFrame();
    R_BeginSharedCmdList();
    R_AddCmdClearScreen( 7, Cam_ClearColor(), 1.0f, 0 );   // COLOR_CAMERABACK, or the leak strobe
    static const float s_white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    R_AddCmdSetMaterialColor( s_white );

    CamWnd_Draw( hwnd );

    R_EndFrame();
    R_IssueRenderCommands( (uint)-1 );
    R_SortMaterials();
    R_CheckTargetWindow( (HWND__ *)hwnd );
}

// P5 RTT: the same CamWnd_Paint pipeline, but rendering into RTT_CAMERA's offscreen texture
// (for ImGui to sample) instead of a native window.  The window-setup (R_IsRegisteredRenderWindow
// + R_SetupRendertarget_CheckDevice) is replaced by RTT_Begin, the tail R_CheckTargetWindow is
// dropped, and the frame ends with RTT_End.  `w`/`h` come from the ImGui dock cell.
void CamWnd_RenderToRT( int w, int h )
{
    PROF_SCOPED( "Camera RenderToRT" );
    if ( !dx.device || w < 1 || h < 1 )
        return;
    // Drive the viewport's own size state from the dock-cell size (was set by CamWnd_OnSize).
    g_camwndState.width  = w;   g_camwndState.height = h;
    g_camwndState.camera.width = w;   g_camwndState.camera.height = h;
    {
        if ( !RTT_Begin( RTT_CAMERA, w, h ) )   // points FRAME_BUFFER at the RT + suppresses Present
            return;
    }

    {
        R_BeginFrame();
        R_BeginSharedCmdList();
        R_AddCmdClearScreen( 7, Cam_ClearColor(), 1.0f, 0 );   // COLOR_CAMERABACK, or the leak strobe
        static const float s_white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        R_AddCmdSetMaterialColor( s_white );
    }

    // CamWnd_Draw's hwnd fed only the removed terrain-paint cursor ring; it is not the render
    // target.
    CamWnd_Draw( g_qeglobals.d_hwndCamera );

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

// 0x402f10  CCamWnd::OnDestroy tail - persist the window placement.
void CamWnd_OnDestroy( HWND hwnd )
{
    WINDOWPLACEMENT wndpl;
    wndpl.length = sizeof( wndpl );                          // 44
    if ( ::GetWindowPlacement( hwnd, &wndpl ) )
        SaveRegistryInfo( "Radiant::CameraWindowPlace", &wndpl, 0x2C );
}

// 0x402f60  CCamWnd::OnKeyDown - in the binary a thin forward to CMainFrame::OnKeyDown; a
// no-op here.  Camera movement in the binary is purely the RMB cursor-joystick fly plus the
// command-map camera-nudge WM_COMMANDs; there are NO built-in WASD/arrow fly keys.
void CamWnd_OnKeyDown( unsigned int nChar, unsigned int nRepCnt, unsigned int nFlags )
{
    (void)nChar; (void)nRepCnt; (void)nFlags;
}

// 0x403160  CCamWnd::OnLButtonDown.  The 3D pick uses a bottom-left origin (flip Y), as in
// the binary.
void CamWnd_OnLButtonDown( HWND hwnd, unsigned int nFlags, int x, int y )
{
    (void)hwnd;   // P5 RTT: hidden child — flip against viewport size state; shell owns capture/focus.
    CamWnd_DropModelsToPlane( x, g_camwndState.height - y - 1, nFlags );
}

// 0x4031d0  CCamWnd::OnLButtonUp.
void CamWnd_OnLButtonUp( unsigned int nFlags )
{
    Cam_MouseUp( nFlags );
    // P5 RTT: shell owns drag-capture; OS ReleaseCapture on the hidden child would steal the
    // mouse from the ImGui host, so the ( nFlags & MK_* )==0 release is dropped here.
}

// 0x4032b0  CCamWnd::OnRButtonDown — the shared button dispatcher (LMB/MMB/RMB all route
// here), bottom-left origin (flip Y).
void CamWnd_OnRButtonDown( HWND hwnd, unsigned int nFlags, int x, int y )
{
    (void)hwnd;   // P5 RTT: hidden child — flip against viewport size state; shell owns capture/focus.
    CamWnd_DropModelsToPlane( x, g_camwndState.height - y - 1, nFlags );
}

// 0x403310  CCamWnd::OnRButtonUp (the binary: ContextMenu -> Cam_MouseUp -> ReleaseCapture).
void CamWnd_OnRButtonUp( HWND hwnd, unsigned int nFlags, int x, int y )
{
    (void)hwnd;   // P5 RTT: hidden child — flip against viewport size state; shell owns capture.
    Cam_MouseUp( nFlags );
    // P5 RTT: OS ReleaseCapture on the hidden child would steal the mouse from the ImGui host;
    // the shell owns drag-capture, so the release is dropped here.
    // KIWI-UX (ROUND BG, ITEM 1) — USER DIRECTIVE: "I want this native right click menu that
    // appears sometimes when right clicking on an object GONE."  This call was the popup's
    // last live trigger (the modern layer's replay is fenced off in kiwi_viewport.cpp, and
    // this arm is the one the LEGACY shell dispatch reached — imgui_shell.cpp VP_Up — which
    // is exactly why it appeared only "sometimes").  KIWI: the popup builder itself is now
    // deleted too.  Cam_MouseUp above still runs, so the cursor/drag teardown is unchanged.
    (void)x; (void)y;
    // The binary does NOT chain to CWnd::OnRButtonUp (which would DefWindowProc ->
    // WM_CONTEXTMENU); omit it to match.
}

// 0x403100  CCamWnd::OnMouseMove.
void CamWnd_OnMouseMove( HWND hwnd, unsigned int nFlags, int x, int y )
{
    (void)hwnd;   // P5 RTT: hidden child — flip against viewport size state.
    // Dedup: the binary skips Cam_MouseMoved when the cursor hasn't moved (m_ptLastCursor).
    if ( g_camwndState.m_ptLastCursor.x != x || g_camwndState.m_ptLastCursor.y != y )
        CamWnd_MouseMoved( nFlags, x, g_camwndState.height - y - 1 );
    g_camwndState.m_ptLastCursor.x = x;
    g_camwndState.m_ptLastCursor.y = y;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Raw-Win32 shell — the CCamWnd twin (U-VP-CAM), with the binary's MFC conventions:
//    (ImGui first)  → ImGuiShell_HandleMessage, consumed → return 0
//    WM_CREATE     → CamWnd_OnCreate                    (CCamWnd::OnCreate tail)
//    WM_SIZE       → DefWindowProc, then CamWnd_OnSize   (MFC chains CWnd::OnSize FIRST)
//                    cx/cy = LOWORD/HIWORD(lParam), as MFC's ON_WM_SIZE thunk extracts them
//    WM_PAINT      → BeginPaint + CamWnd_Paint + EndPaint (CPaintDC's job in the binary)
//    WM_ERASEBKGND → return 1                           (CCamWnd::OnEraseBkgnd returns TRUE)
//    WM_DESTROY    → DefWindowProc, then CamWnd_OnDestroy (MFC chains CWnd::OnDestroy FIRST,
//                    then saves the placement)
//    WM_KEYDOWN    → CamWnd_OnKeyDown, return 0         (the handler does not call Default())
//                    nRepCnt = LOWORD(lParam), nFlags = HIWORD(lParam), as MFC's thunk splits it
//    WM_LBUTTONDOWN/WM_RBUTTONDOWN → CamWnd_On?ButtonDown, return 0 (the binary's handlers do
//                    NOT chain the base class — SetFocus + SetCapture happen inside them)
//    WM_LBUTTONUP  → CamWnd_OnLButtonUp, THEN DefWindowProc (the MFC handler tail-calls
//                    CWnd::OnLButtonUp, which is Default() → DefWindowProc)
//    WM_RBUTTONUP  → CamWnd_OnRButtonUp, return 0       (the binary deliberately does NOT chain:
//                    DefWindowProc would post WM_CONTEXTMENU on top of its own popup)
//    WM_MOUSEMOVE  → CamWnd_OnMouseMove, return 0       (this handler does not chain either)
//    (KIWI: no WM_COMMAND entry - the face-picker popup that posted 0x8CA0..0x8CB5 is gone)
//    x/y = (short)LOWORD/HIWORD(lParam) — client coords, exactly CPoint(lParam);
//    nFlags = wParam — the MK_* word MFC passes as UINT nFlags.
//  No wheel entry: CCamWnd's message map has none — the wheel dolly arrives at the MAIN FRAME
//  (Radiant_OnMouseWheel → CamWnd_Scroll).
//  No WM_CHAR entry either: the map has ON_WM_KEYDOWN only.
//
//  d_hwndCamera registration is the CALLER's job: radiant_main.cpp sets
//  g_qeglobals.d_hwndCamera from CamWnd_CreateRaw's HWND, and gfxwrapper.cpp's
//  R_BeginRegistrationInternal later calls R_InitRendererForWindow on it.
// ═════════════════════════════════════════════════════════════════════════════

static const char *const CAMWND_CLASS_NAME = "KIWICamWnd";

LRESULT CALLBACK CamWnd_WndProc( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    extern bool ImGuiShell_HandleMessage( HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam );   // imgui_shell.cpp
    if ( ImGuiShell_HandleMessage( hwnd, msg, wParam, lParam ) )
        return 0;

    switch ( msg )
    {
    case WM_CREATE:
        CamWnd_OnCreate( hwnd );
        return 0;

    case WM_SIZE:
    {
        LRESULT r = DefWindowProcA( hwnd, msg, wParam, lParam );
        CamWnd_OnSize( hwnd, (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return r;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint( hwnd, &ps );
        CamWnd_Paint( hwnd );
        EndPaint( hwnd, &ps );
        return 0;
    }

    case WM_DESTROY:
    {
        LRESULT r = DefWindowProcA( hwnd, msg, wParam, lParam );
        CamWnd_OnDestroy( hwnd );
        return r;
    }

    case WM_KEYDOWN:
        CamWnd_OnKeyDown( (unsigned int)wParam,
                          (unsigned int)LOWORD( lParam ), (unsigned int)HIWORD( lParam ) );
        return 0;

    case WM_LBUTTONDOWN:
        CamWnd_OnLButtonDown( hwnd, (unsigned int)wParam,
                              (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return 0;

    case WM_RBUTTONDOWN:
        CamWnd_OnRButtonDown( hwnd, (unsigned int)wParam,
                              (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return 0;

    case WM_LBUTTONUP:
        CamWnd_OnLButtonUp( (unsigned int)wParam );
        break;      // → DefWindowProc, like CWnd::OnLButtonUp

    case WM_RBUTTONUP:
        CamWnd_OnRButtonUp( hwnd, (unsigned int)wParam,
                            (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return 0;   // the binary does NOT chain the base class

    case WM_MOUSEMOVE:
        CamWnd_OnMouseMove( hwnd, (unsigned int)wParam,
                            (int)(short)LOWORD( lParam ), (int)(short)HIWORD( lParam ) );
        return 0;
    }
    return DefWindowProcA( hwnd, msg, wParam, lParam );
}

// The CCamWnd::PreCreateWindow class (CS_OWNDC + no background brush: we present via D3D, so
// the shell must not paint the client area) + the CWnd::Create style CreateQEChildren passes
// (WS_CHILD | WS_VISIBLE, child id AFX_IDW_PANE_FIRST+1 — the dock host positions it, so the
// raw creator takes the rect directly instead).
HWND CamWnd_CreateRaw( HWND parent, int x, int y, int w, int h )
{
    static bool s_classRegistered = false;
    HINSTANCE   inst = GetModuleHandleA( nullptr );

    if ( !s_classRegistered )
    {
        WNDCLASSA wc = {};
        wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = CamWnd_WndProc;
        wc.hInstance     = inst;
        wc.hCursor       = LoadCursorA( nullptr, IDC_ARROW );
        wc.hbrBackground = nullptr;
        wc.lpszClassName = CAMWND_CLASS_NAME;
        if ( !RegisterClassA( &wc ) )
            return nullptr;
        s_classRegistered = true;
    }

    // CREATED HIDDEN — see the same note in XYWnd_CreateRaw (xywnd.cpp).
    return CreateWindowExA( 0, CAMWND_CLASS_NAME, nullptr,
                            WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                            x, y, w, h, parent, nullptr, inst, nullptr );
}

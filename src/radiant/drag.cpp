#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\drag.cpp - the mouse press/drag/release state machine.
// Ported from CoD4Radiant (IW3xRadiant.i64); GtkRadiant 1.6 for naming only.
//   0x47e3a0  Drag_Setup       mouse-down setup (what a press starts)
//   0x47e810  Drag_FaceAlign   copy the texture projection between two selected faces
//   0x463a80  ClampGridSize    grid index -> rotation-snap step
//   0x47e890  Drag_Begin       mouse-down dispatcher (select / drag / texture pick-apply)
//   0x47f0c0  MoveSelection    translate / scale / rotate / vertex / terrain
//   0x47ff30  Drag_MouseMoved  pixel delta -> grid-snapped world move
//   0x4802a0  Drag_MouseUp     marquee box-select + undo close
// The screen->world drag basis (g_drag_xvec/yvec) is AXIALISED to UNIT vectors, so the
// move is computed in PIXEL units and then grid-snapped; m_fScale is applied only in
// rotate.  ALL THREE xvec components must land in g_drag_xvec[]: splitting the z into a
// separate local leaves AxializeVector reading an uninitialised component.

#include "stdafx.h"
#include "qe3.h"
#include "prefs.h"       // g_PrefsDlg (prefData_t* — real settings)
#include "mainfrm.h"     // CMainFrame / CXYWnd (m_pActiveXY->m_fScale in rotate/bend path)
#include "xywnd.h"       // xywndState_t / Ed_ActiveXY (U-GLOBALS)
#include "kiwi_ported.h" // KIWI: shared engine/editor externs
#include <math.h>
#include <stdlib.h>

// ─── forward declarations ─────────────────────────────────────────────────────
// undo.cpp
extern void    Undo_Start( const char *op );

// select.cpp
extern void    Select_ClickTrace( float *trace_dir, float *trace_start, int contents );

// select.cpp
extern void    SelectVertexByRay( float *origin, float *dir );
extern void    SelectCurvePointByRay( float *origin, float *dir, unsigned int buttons );
extern void    Select_Edge( float *dir, float *origin );
// facePtr is the face's address (&brush->def->faces[idx]), NOT a raw index.
extern void Brush_SelectFaceForDragging( brush_t *def, face_t *facePtr, int shear );
extern void    Brush_SideSelect( float *origin, brush_t *def, float *dir, int control );
extern void    Patch_RedispersePreDrag();   // pmesh.cpp (0x43D7A0)

// pmesh.cpp / engine_stubs.cpp
extern void    Patch_TurnEdge( float *origin, float *dir );
extern void    Patch_UpdateSelected( float *move, const float *total );
extern int     Patch_DragScale( float *delta, void *patchDef, float *move );
extern int     OnlyPatchesSelected();

// brush.cpp
extern int     Brush_MoveVertex( vec3_t delta, brush_t *brush, vec3_t move_points, vec3_t end, bool exactEnd );

// select.cpp - selection movers used by MoveSelection
extern void    Select_Scale( float sx, float sy, float sz );
extern void    Select_ApplyMatrix( float *matrix, selbrush_t *b, int bSnap, float deg, char bSwap );

// mainfrm / status
extern void        MainFrm_SetStatusText( int pane, const char *text );

// qcommon / com_math
extern void    Com_PrintMessage( const char *fmt, ... );

// abs32: integer absolute value.
static inline int abs32( int x ) { return x < 0 ? -x : x; }

// patch_t (the patch instance node; .def @0, .selected @6) is now the shared struct in
// qe3.h — selbrush_t.patch is a patch_t*.

// The editor's "point-editing" select sub-modes (sel_editpoint=10 and its sub-states
// 11..15) form the contiguous range [10,15]. None equal sel_brush(0), so the normal drag
// path is unaffected.
static inline bool Drag_IsPointEditMode( int m ) { return m >= sel_editpoint && m <= 15; }

// Test_Ray (select.cpp): casts a pick ray against the brush lists, filling an edTrace_t
// (the real 88-byte trace_t layout, qe3.h).
extern void    Test_Ray( float *start, float *dir, int contents,
                         edTrace_t *t, int num_traces );

// g_PrefsDlg (0x73c704) — the editor preference singleton (prefs.h declares it as
// a real prefData_t*, non-NULL with the binary defaults from static-init onward).

// g_nPatchClickedView (0x73b108)
extern int  g_nPatchClickedView;   // engine_stubs.cpp

// ─── drag globals ─────────────────────────────────────────────────────────────
// pressdelta (3-float accumulated drag offset).
float  g_pressdelta[3];          // 0x23f16f8
// press xvec (the axialised xaxis vector).
float  g_drag_xvec[3];           // 0x23f1718
// press yvec (the axialised yaxis vector).
float  g_drag_yvec[3];           // 0x23f170c
// integer press X pixel.
int    g_pressx_real;            // 0x23f1708
// integer press Y pixel.
int    g_pressy_real;            // 0x23f1704
// 0x23f1720 is NOT a separate variable — it is g_drag_xvec[2]. The real press-start
// vector is g_vPressStart[] (declared below). Drag_Setup writes the xvec basis's z into
// g_drag_xvec[2] so AxializeVector(g_drag_xvec) sees all three components.
// drag_first flag.
int    drag_first;               // 0x23f16e0
// drag_ok flag.
int    drag_ok;                  // 0x23f1724

// vPressStart (3 floats, used in MoveSelection).
float  g_vPressStart[3];         // 0x23f16ec

bool   g_bRotateMode;            // 0x23f16d9
bool   g_bScaleMode;             // 0x23f16da
// low 3 bits = X/Y/Z lock
char   g_nScaleHow;              // 0x23f16dc

// SCALE_X/Y/Z bit flags (g_nScaleHow & 1/2/4)
#define SCALE_X  1
#define SCALE_Y  2
#define SCALE_Z  4

// rotation accumulator for rotate mode
float  g_vRotation[3];           // 0x23f164c
// center of rotation
float  g_vRotateOrigin[3];       // 0x23f1658

// selected_brushes_next (0x23f1868) is the 'next' pointer INSIDE the selected_brushes
// sentinel node: access it as selected_brushes.next everywhere.  selected_brushes itself
// is defined in engine_stubs.cpp.

// Helper to get the face index of a faceVis_s* within a selbrush_t's face array.
static inline int drag_face_index( selbrush_t *b, faceVis_s *face )
{
    return (int)( face - b->faces );          // typed pointer subtraction = element index
}

// ─── 0x47e2a0  AxializeVector ────────────────────────────────────────────────
// Collapses a vector in place to its dominant axis unit vector (same as GtkRadiant).
// The binary's >=-negation tie-breaking picks the same axis as the strict-> chain here.
static void AxializeVector( float *v )
{
    float a[3];
    int   i;

    // Early out if two components are already zero
    if ( !v[0] && !v[1] ) return;
    if ( !v[1] && !v[2] ) return;
    if ( !v[0] && !v[2] ) return;

    for ( i = 0; i < 3; i++ )
        a[i] = (float)fabs( (double)v[i] );

    if ( a[0] > a[1] && a[0] > a[2] )
        i = 0;
    else if ( a[1] > a[0] && a[1] > a[2] )
        i = 1;
    else
        i = 2;

    float o = v[i];
    v[0] = v[1] = v[2] = 0.0f;
    v[i] = ( o < 0.0f ) ? -1.0f : 1.0f;
}

// ─── 0x47e3a0  Drag_Setup ────────────────────────────────────────────────────
// Initialises a drag operation given the current mouse position and selection state.
// Called by Drag_Begin when MK_LBUTTON is pressed (non-area-select path).
//   trace_dir, trace_start = the pick ray
//   press_origin           = the view's screen-X world basis (Drag_Begin's xvec)
//   x, y                   = pixel coords of press
//   buttons                = MK_ button flags
//   xyvec                  = the view's screen-Y world basis (Drag_Begin's yvec)
// The port adds `if(hitBrush)` null-guards in the buttons 9/0xD face paths, where the
// binary derefs the hit brush unconditionally - a latent null-deref reachable via
// alt-edge-drag with no ray hit.
void Drag_Setup( float *trace_dir, float *trace_start, float *press_origin,
                 int x, int y, unsigned int buttons, float *xyvec )
{
    edTrace_t t;

    // pressdelta = { 0, 0, 0 }
    g_pressdelta[0] = 0.0f;
    g_pressdelta[1] = 0.0f;
    g_pressdelta[2] = 0.0f;

    // Store the screen X-axis world basis.
    // All three components go into g_drag_xvec so AxializeVector below sees them.
    g_drag_xvec[0] = press_origin[0];
    g_drag_xvec[1] = press_origin[1];
    g_drag_xvec[2] = press_origin[2];

    g_pressx_real = x;
    drag_first    = 1;
    g_pressy_real = y;

    // Axialise the xvec
    AxializeVector( g_drag_xvec );

    // Copy and axialise the yvec
    g_drag_yvec[0] = xyvec[0];
    g_drag_yvec[1] = xyvec[1];
    g_drag_yvec[2] = xyvec[2];
    AxializeVector( g_drag_yvec );

    // ── sel_curvepoint mode ───────────────────────────────────────────────────
    if ( g_qeglobals.d_select_mode == sel_curvepoint )
    {
        if ( GetAsyncKeyState( VK_MENU ) < 0 )  // VK_MENU = 18 = Alt key
        {
            drag_ok = g_qeglobals.d_num_move_points > 0;
        }
        else
        {
            SelectCurvePointByRay( trace_start, trace_dir, buttons );
            if ( g_qeglobals.d_num_move_points || g_qeglobals.d_select_mode == sel_area )
            {
                drag_ok = 1;
                // Store selection rect
                g_qeglobals.drag_selectionbox_x_1 = x;
                g_qeglobals.drag_selectionbox_x_2 = y;
                g_qeglobals.drag_selectionbox_y_1 = x;
                g_qeglobals.drag_selectionbox_y_2 = y;
            }
        }
        g_nUpdateBits = -1;
        Undo_ClearRedo();
        Undo_GeneralStart( "drag curve point" );
        Undo_AddBrushList( &selected_brushes );
        Patch_RedispersePreDrag();   // snapshot live texCoords → savedTexCoord (Patch_SaveLayerMtl)
        return;
    }

    // ── sel_cycle_edge_direction_quad mode ──────────────────────────────────
    if ( g_qeglobals.d_select_mode == sel_cycle_edge_direction_quad )
    {
        Patch_TurnEdge( trace_start, trace_dir );
        return;
    }

    // ── reset move points and terrain points ─────────────────────────────────
    g_qeglobals.d_num_move_points = 0;
    g_qeglobals.d_numterrapoints  = 0;

    // ── area select mode (sel_areatall equivalent) ────────────────────────────
    // Alt with LMB+Shift (5, gated on the 2D view and no rotate/scale mode) or
    // RMB+Shift (6) starts a marquee box select.  Each path reads the Alt key once.
    const bool boxGate   = g_nPatchClickedView != 1 && !g_bRotateMode && !g_bScaleMode;
    const bool boxSelect = ( ( buttons == 5 && boxGate ) || buttons == 6 )
                           ? GetAsyncKeyState( VK_MENU ) < 0 : false;
    if ( boxSelect )
    {
        // 0x47e57b: d_select_mode = (buttons & 2 | MK_CONTROL | MK_MBUTTON) >> 1, i.e.
        // (buttons & 2) ? 0x1A>>1 = 13 (box DESELECT) : 0x18>>1 = 12 (box ADD).
        g_qeglobals.d_select_mode = (select_t)(( (buttons & 2) | (MK_CONTROL | MK_MBUTTON) ) >> 1);
        g_qeglobals.drag_selectionbox_x_1 = x;
        g_qeglobals.drag_selectionbox_x_2 = y;
        g_qeglobals.drag_selectionbox_y_1 = x;
        g_qeglobals.drag_selectionbox_y_2 = y;
        drag_ok       = 1;
        g_nUpdateBits = -1;
        return;
    }

    // ── no selection? start a 'create brush' undo and return ─────────────────
    if ( selected_brushes.next == &selected_brushes )
    {
        Undo_ClearRedo();
        Undo_GeneralStart( "create brush" );
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"No selection to drag\n" );
        return;
    }

    // ── sel_vertex mode ───────────────────────────────────────────────────────
    if ( g_qeglobals.d_select_mode == sel_vertex )
    {
        SelectVertexByRay( trace_start, trace_dir );
        if ( g_qeglobals.d_num_move_points )
        {
            drag_ok = 1;
            Undo_Start( "drag vertex" );
            Undo_AddBrushList( &selected_brushes );
            return;
        }
    }

    // ── sel_edge mode ─────────────────────────────────────────────────────────
    if ( g_qeglobals.d_select_mode == sel_edge )
    {
        Select_Edge( trace_dir, trace_start );
        if ( g_qeglobals.d_num_move_points )
        {
            drag_ok = 1;
            Undo_Start( "drag edge" );
            Undo_AddBrushList( &selected_brushes );
            return;
        }
    }

    // ── ray test: direct hit on selection or Alt+drag face-extrude ───────────
    memset( &t, 0, sizeof(t) );
    Test_Ray( trace_start, trace_dir, 257, &t, 1 );
    // m_bALTEdge (ALTEdgeDrag, default 1): Alt+drag face-extrude. Inert at rest (the
    // AND is false unless Alt is physically held), so default behaviour is unchanged.
    if ( t.selected || ( g_PrefsDlg->m_bALTEdge && GetAsyncKeyState( VK_MENU ) < 0 ) )
    {
        drag_ok = 1;
        Undo_Start( "drag selection" );
        Undo_AddBrushList( &selected_brushes );

        selbrush_t *hitBrush = (selbrush_t *)t.hit.brush;
        faceVis_s  *hitFace  = (faceVis_s  *)t.hit.face;

        if ( buttons == 9 )  // MK_LBUTTON|MK_CONTROL = 9
        {
            // Shear dragging: select single face
            if ( hitBrush )
            {
                iassert( t.hit.brush->version == t.hit.brush->def->version );   // drag.cpp:203
                iassert( t.hit.face >= &t.hit.brush->faces[0] && t.hit.face < &t.hit.brush->faces[t.hit.brush->faceCount] );   // drag.cpp:204
                int fIdx = drag_face_index( hitBrush, hitFace );
                Brush_SelectFaceForDragging( hitBrush->def,
                    &hitBrush->def->faces[fIdx], 1 );
            }
        }
        else if ( buttons == (MK_LBUTTON | MK_SHIFT | MK_CONTROL) )  // 0xD
        {
            // Sticky drag: select all faces on the hit brush
            if ( hitBrush && hitBrush->faceCount )
            {
                for ( int f = 0; (unsigned)f < (unsigned)hitBrush->faceCount; f++ )
                    Brush_SelectFaceForDragging( hitBrush->def, &hitBrush->def->faces[f], 0 );
            }
        }
        return;
    }

    // ── side-hit check (not vertex/edge mode) ─────────────────────────────────
    if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
    {
        return;  // no side-hit in vertex/edge mode
    }

    // single vs multiple selected brushes
    if ( selected_brushes.next->next == &selected_brushes )
    {
        // Single brush selected
        if ( (buttons & MK_CONTROL) != 0 )
        {
            Brush_SideSelect( trace_start, selected_brushes.next->def, trace_dir, 1 );
        }
        else
        {
            Brush_SideSelect( trace_start, selected_brushes.next->def, trace_dir, 0 );
        }
    }
    else
    {
        // Multiple brushes selected — iterate
        for ( selbrush_t *sb = selected_brushes.next; sb != &selected_brushes; sb = sb->next )
        {
            if ( buttons & MK_CONTROL )
            {
                Brush_SideSelect( trace_start, sb->def, trace_dir, 1 );
            }
            else
            {
                Brush_SideSelect( trace_start, sb->def, trace_dir, 0 );
            }
        }
    }

    drag_ok = 1;
    Undo_Start( "side stretch" );
    Undo_AddBrushList( &selected_brushes );
}

// ─── 0x47e810  Drag_FaceAlign ────────────────────────────────────────────────
// Aligns the texture of the second selected face to the first selected face.
// Called from Drag_Begin when exactly 2 faces are selected after an Alt + texture-button
// press.
//
// NOTE: g_SelectedFaces lives in select.cpp (model in qe3.h).

// brush.cpp 0x476a30; the destination brush def is the middle arg.
extern brush_t *Brush_ApplyTextureProjection( face_t *srcFace, brush_t *b, face_t *dstFace );

// count != 2 -> Assert 270; count < 2 -> SelectedFaces_BoundsAbort (CRT range-check abort,
// noreturn).  The port's explicit `return` after the abort is harmless; the binary falls
// through into dead code.
void Drag_FaceAlign()
{
    int count = g_SelectedFaces.GetSize();
    iassert( g_SelectedFaces.GetCount() == 2 );   // drag.cpp:270

    if ( count < 2 )
    {
        SelectedFaces_BoundsAbort();
        return;
    }

    // Apply the texture projection from g_SelectedFaces.GetAt( 0 ) to g_SelectedFaces.GetAt( 1 )
    Brush_ApplyTextureProjection(
        &g_SelectedFaces.GetAt( 0 ).brush->def->faces[ g_SelectedFaces.GetAt( 0 ).index ],
             g_SelectedFaces.GetAt( 1 ).brush->def,
        &g_SelectedFaces.GetAt( 1 ).brush->def->faces[ g_SelectedFaces.GetAt( 1 ).index ]
    );
}

// ─── 0x463a80  ClampGridSize ─────────────────────────────────────────────────
// The angle/grid snap table (the name is misleading — it maps the grid index to a
// rotation-snap step). Used by Drag_MouseMoved / MoveSelection in rotate mode.
// 0/1->1, 2->2, 3->5, 4->15, 5->30, 6/7->45, default->90.
extern "C" int ClampGridSize();
int ClampGridSize()
{
    switch ( g_qeglobals.d_gridsize )
    {
        case 0: case 1: return 1;
        case 2:         return 2;
        case 3:         return 5;
        case 4:         return 15;
        case 5:         return 30;
        case 6: case 7: return 45;
        default:        return 90;
    }
}

// ─── 0x47e890  Drag_Begin (the mouse-down dispatcher) ─────────────────────────
// Decides, from the button/modifier combination and the select mode, what a press starts:
// a group/face select (Select_ClickTrace), a brush drag (Drag_Setup), or a texture
// pick/apply.  Contents-flag prefs: entities_off 0x200 / models 0x400 / sky 0x800
// / viewz 2 -> 0x1000, 0 -> 4.  The middle/right-button tail has five sub-branches by
// nMouseButton + modifier bits (pick / apply-brush / apply-face / "set face but leave
// info" / face-align).  The brush-apply branches route to Brush_SetAllFacesMaterial.
//
// See CXYWnd::XY_MouseDown (0x467850) for the call. Params:
//   pressFunc   = press/snap callback ptr (stored in g_qeglobals.camera_fov_setup)
//   buttons     = MK_ button flags
//   viewz       = view depth-axis selector (XY view passes 0)
//   px, py      = press pixel coords
//   xvec, yvec  = the view's screen-X / screen-Y world basis (1/scale magnitude)
//   trace_start, trace_dir = the picking ray
//
extern int  g_nPatchClickedView;                // 0x73b108

// --- middle/right-button texture pick/apply tail deps ---
// (Test_Ray + g_SelectedFaces + Drag_FaceAlign already declared above.)
extern void UpdatePatchInspector();                                   // patchdialog.cpp 0x436db0
namespace SurfaceInspector { void UpdateSurfaceDialog(); }            // 0x458590 (surfacedlg.cpp)
// brush.cpp ports used by the apply branches:
//   Brush_SetFaceTexdefSize == Face_SetMaterial (0x476740): write {lyrMtl,radMtl} into face->mtldef[layer]
extern void Brush_SetFaceTexdefSize( const patchMesh_material *material, face_t *f, brush_t *b ); // 0x476740
extern void Brush_PropagateFaceTexdef( brush_t *def, const face_t *srcFace );   // brush.cpp 0x4766f0
// per-BRUSH apply (set every face to the current material; patch: Patch_ApplyMaterial projection)
extern void Brush_SetAllFacesMaterial( brush_t *def, MaterialDef *material, char planarProject,
                                       float useStoredST, char materialOnly );   // brush.cpp 0x476ed0
// texture-pick deps:
//   Texture_SetTexture (0x45be50, texwnd.cpp): writes the picked MaterialDef into
//       random_texture_stuff[layer], realizes it, applies it (Brush_SetTexture) and
//       scrolls the browser to it.
//   Face_CurrentTexdef (0x45d320, select.cpp): the material's current-layer texdef.
//   Radiant_PatchGetTexdef (0x44b620, brush.cpp): patch-texdef extraction.  Always returns
//       0 (the binary has no success path); the texdef WRITE is the observable.
extern char Texture_SetTexture( const int *patchGrid, MaterialDef *material ); // texwnd.cpp 0x45be50
extern char Radiant_PatchGetTexdef( patchMesh_t *patch, texdef_sub_t *texdef ); // brush.cpp 0x44b620

void Drag_Begin( void *pressFunc, unsigned int buttons, int viewz,
                 int px, int py, float *xvec, float *yvec,
                 float *trace_start, float *trace_dir )
{
    int contents = 0;

    g_pressdelta[0] = g_pressdelta[1] = g_pressdelta[2] = 0.0f;
    g_qeglobals.camera_fov_setup = pressFunc;
    g_vPressStart[0] = g_vPressStart[1] = g_vPressStart[2] = 0.0f;
    drag_ok    = 0;
    drag_first = 1;

    if ( g_PrefsDlg->entities_off )         contents  = 0x200;   // entities_off (default 0)
    if ( g_PrefsDlg->m_bSelectableModels )  contents |= 0x400;   // ModelSelection (default 0)
    if ( g_PrefsDlg->sky_brush_off )        contents |= 0x800;   // sky_brush_off (default 0)
    if ( viewz == 2 )       contents |= 0x1000;
    else if ( viewz == 0 )  contents |= 4;

    iassert( (buttons & MK_ALT) == 0 );   // drag.cpp:323 (MK_ALT=0x20, oleidl.h)

    const select_t mode = g_qeglobals.d_select_mode;

    // The binary's goto web, kept (labels named): shift_alt = Shift+Alt click,
    // ctrl_click = a click without Alt, click_trace = the click pick, button_drag = the
    // plain button drag, start_drag = Drag_Setup.
    if ( buttons == ( MK_LBUTTON | MK_SHIFT ) )            // 5 = select group
    {
        if ( ( GetAsyncKeyState( VK_MENU ) & 0x8000 ) == 0 )
            goto ctrl_click;
        goto shift_alt;
    }
    if ( buttons == ( MK_RBUTTON | MK_SHIFT ) )            // 6
    {
        if ( ( GetAsyncKeyState( VK_MENU ) & 0x8000 ) == 0 )
            goto button_drag;
    shift_alt:
        if ( mode != sel_curvepoint )
        {
            if ( g_nPatchClickedView == 1 )
            {
            click_trace:
                Select_ClickTrace( trace_dir, trace_start, contents );
                return;
            }
            goto start_drag;
        }
    }
    if ( buttons == 13 )                                  // LMB|SHIFT|CTRL
    {
        if ( GetAsyncKeyState( VK_MENU ) < 0 && mode != sel_curvepoint )
        {
            Select_ClickTrace( trace_dir, trace_start, contents | 0x40 );
            return;
        }
        if ( ( GetAsyncKeyState( VK_MENU ) & 0x8000 ) == 0 && mode != sel_curvepoint )
        {
            Select_ClickTrace( trace_dir, trace_start, 520 );
            return;
        }
        goto button_drag;
    }
    if ( buttons != 5 )                                   // LMB|SHIFT
        goto button_drag;
ctrl_click:
    if ( ( GetAsyncKeyState( VK_MENU ) & 0x8000 ) == 0 && mode != sel_curvepoint )
    {
        if ( g_PrefsDlg->enable_light_preview )   // light/sun preview pick (default 1)
            contents |= 0x4000;
        goto click_trace;
    }
button_drag:
    // KIWI: the binary's Alt + LMB/RMB terrain-paint start sat here (AdvPatchEditDlg);
    // Terrain Sculpt replaced it and the paint path is gone.
    if ( ( buttons & MK_LBUTTON ) != 0 )
    {
    start_drag:
        // press_origin = xvec, xyvec = yvec (see Drag_Setup's params).
        Drag_Setup( trace_dir, trace_start, xvec, px, py, buttons, yvec );
        return;
    }

    // ── middle/right-button texture pick / apply / face-align ────────────────
    // nMouseButton selects the texture-edit button (16 for 3-button mice, 2 for
    // 2-button).  Five sub-branches by modifier bits: texture pick (Texture_SetTexture /
    // Radiant_PatchGetTexdef), brush / face apply (Brush_SetAllFacesMaterial /
    // Face_SetMaterial / Brush_PropagateFaceTexdef) and face-align (Drag_FaceAlign).
    // Not reachable from a plain left-click drag.
    const int nMouseButton = ( g_PrefsDlg->m_nMouseButtons != MK_RBUTTON ) ? 16 : 2;
    const bool altDown = ( GetAsyncKeyState( VK_MENU ) & 0x8000 ) != 0;
    const int layer = g_qeglobals.current_edit_layer;

    edTrace_t t;

    // ── BRANCH 1: nMouseButton, no Alt → pick the hit face's texture ──
    if ( buttons == (unsigned)nMouseButton && !altDown )
    {
        Test_Ray( trace_start, trace_dir, 512, &t, 1 );
        if ( !t.hit.face )
        {
            Sys_Printf( "Did not select a texture\n" );
            return;
        }
        selbrush_t *brush = t.hit.brush;
        brush_t    *def   = brush->def;
        // Remember the hit brush's vertical extent (bottom/top) as the new-brush template.
        g_qeglobals.d_new_brush_bottom_x = def->mins[0];
        g_qeglobals.d_new_brush_bottom_y = def->mins[1];
        g_qeglobals.d_new_brush_bottom_z = def->mins[2];
        g_qeglobals.d_new_brush_top_x = def->maxs[0];
        g_qeglobals.d_new_brush_top_y = def->maxs[1];
        g_qeglobals.d_new_brush_top_z = def->maxs[2];

        iassert( t.hit.face >= &t.hit.brush->faces[0] && t.hit.face < &t.hit.brush->faces[t.hit.brush->faceCount] );   // drag.cpp:413
        iassert( t.hit.brush->version == t.hit.brush->def->version );   // drag.cpp:414

        int faceIdx = (int)( t.hit.face - brush->faces );   // faceVis_s stride 12
        face_t *face = &def->faces[ faceIdx ];
        if ( brush->patch )
        {
            texdef_sub_t *td = Face_CurrentTexdef( &face->mtldef[ layer ] );
            Radiant_PatchGetTexdef( def->patch, td );   // return discarded — see brush.cpp
        }
        // patch-consistency assert
        patch_t *pinst = brush->patch;
        // drag.cpp:422. The short-circuit keeps it deref-safe for both
        // consistent states (plain brush: first clause true; patch brush: patch != NULL);
        // the inconsistent patch==NULL && def->patch!=NULL state derefs NULL exactly as
        // the original source would have.
        iassert( (t.hit.brush->patch == NULL && t.hit.brush->def->patch == NULL) || (t.hit.brush->patch->def == t.hit.brush->def->patch) );
        // The patch def (its ST grid) for a patch pick, else 0: a brush-face pick copies no
        // projection.
        const int *patchDef = pinst ? (const int *)pinst->def : nullptr;
        Texture_SetTexture( patchDef, &face->mtldef[ layer ] );
        SurfaceInspector::UpdateSurfaceDialog();
        UpdatePatchInspector();
        return;
    }

    // ── BRANCH 2: nMouseButton|8, no Alt → apply current material to whole brush ──
    if ( buttons == (unsigned)( nMouseButton | 8 ) && !altDown )
    {
        Test_Ray( trace_start, trace_dir, 0, &t, 1 );
        if ( !t.hit.brush )
        {
            Sys_Printf( "Didn't hit a brush\n" );
            return;
        }
        // eclass is read via owner->def (entity_s_def), NOT owner->eclass directly
        // (the instance-vs-def pattern).
        if ( ( (entity_s_def *)t.hit.brush->owner->def )->eclass->fixedsize )
        {
            Sys_Printf( "Can't change an entity texture\n" );
            return;
        }
        Brush_SetAllFacesMaterial( t.hit.brush->def, (MaterialDef *)&g_qeglobals.random_texture_stuff[ layer ], 1, 1.0f, 0 );
        g_nUpdateBits = -1;
        return;
    }

    // ── BRANCH 3: nMouseButton|0xC, no Alt → apply current material to the face ──
    if ( buttons == (unsigned)( nMouseButton | 0xC ) && !altDown )
    {
        Test_Ray( trace_start, trace_dir, 0, &t, 1 );
        selbrush_t *brush = t.hit.brush;
        if ( !brush )
        {
            Sys_Printf( "Didn't hit a brush\n" );
            return;
        }
        if ( ( (entity_s_def *)brush->owner->def )->eclass->fixedsize )
        {
            Sys_Printf( "Can't change an entity texture\n" );
            return;
        }
        brush_t *def = brush->def;
        if ( brush->patch )   // patch INSTANCE present ⇒ whole-brush apply
        {
            Brush_SetAllFacesMaterial( def, (MaterialDef *)&g_qeglobals.random_texture_stuff[ layer ], 1, 0.0f, 0 );
            g_nUpdateBits = -1;
            return;
        }
        iassert( t.hit.face >= &t.hit.brush->faces[0] && t.hit.face < &t.hit.brush->faces[t.hit.brush->faceCount] );   // drag.cpp:473
        iassert( t.hit.brush->version == t.hit.brush->def->version );   // drag.cpp:474
        int faceIdx = (int)( t.hit.face - brush->faces );
        face_t *face = &def->faces[ faceIdx ];
        // copy the whole current-layer MaterialDef (36 bytes) onto the picked face.
        memcpy( &face->mtldef[ layer ], &g_qeglobals.random_texture_stuff[ layer ].mtl, sizeof(MaterialDef) );
        if ( def->patch )
            Brush_PropagateFaceTexdef( def, face );   // propagate to all faces (patch case)
        else
            ++def->version;
        Brush_RebuildAfterEdit( def );
        g_nUpdateBits = -1;
        return;
    }

    // ── BRANCH 4: nMouseButton|4, no Alt → set brush face texture but leave info ──
    if ( buttons == (unsigned)( nMouseButton | 4 ) && !altDown )
    {
        Sys_Printf( "Set brush face texture but leave info\n" );
        Test_Ray( trace_start, trace_dir, 0, &t, 1 );
        selbrush_t *brush = t.hit.brush;
        if ( !brush )
        {
            Sys_Printf( "Didn't hit a brush\n" );
            return;
        }
        if ( ( (entity_s_def *)brush->owner->def )->eclass->fixedsize )
        {
            Sys_Printf( "Can't select an entity brush face\n" );
            return;
        }
        brush_t *def = brush->def;
        if ( brush->patch )   // patch instance present ⇒ write the patch's texture/lightmap/smoothing pair
        {
            patchMesh_t *pd = brush->patch->def;
            patchMesh_material *slot = (&pd->texture) + layer;
            slot->lyrMtl = g_qeglobals.random_texture_stuff[ layer ].mtl.lyrMtl;
            slot->radMtl = g_qeglobals.random_texture_stuff[ layer ].mtl.radMtl;
            ++pd->version;
            UpdatePatchInspector();                        // refreshes the patch dialog if open
            g_nUpdateBits = -1;
            return;
        }
        iassert( t.hit.face >= &t.hit.brush->faces[0] && t.hit.face < &t.hit.brush->faces[t.hit.brush->faceCount] );   // drag.cpp:504
        iassert( t.hit.brush->version == t.hit.brush->def->version );   // drag.cpp:505
        int faceIdx = (int)( t.hit.face - brush->faces );
        face_t *face = &def->faces[ faceIdx ];
        // Face_SetMaterial(&random_texture_stuff[layer], face, def): write the {lyrMtl,radMtl}
        // pair into face->mtldef[layer]; ++def->version.
        Brush_SetFaceTexdefSize( (const patchMesh_material *)&g_qeglobals.random_texture_stuff[ layer ], face, def );
        g_nUpdateBits = -1;
        return;
    }

    // ── BRANCH 5: nMouseButton + Alt → align texture between two selected faces ──
    if ( buttons == (unsigned)nMouseButton && altDown )
    {
        Select_Deselect( 0 );
        int n = g_SelectedFaces.GetSize();
        if ( n > 1 )
            g_SelectedFaces.RemoveAt( 0, n - 1 );   // keep only the last selected face
        Select_ClickTrace( trace_dir, trace_start, 520 );
        if ( g_SelectedFaces.GetSize() == 2 )
            Drag_FaceAlign();                   // decomposes with texturevecs_02_exact
        return;
    }
}

// ─── 0x4a5930  ProjectPointOntoRay (misnamed "Vec3Normalize" in the binary) ──
// NOT a normalize: projects `point` onto the ray (origin, dir) and writes the foot of
// the perpendicular into `out`.
//   v = point - origin;  t = dot(dir, v);  out = origin + dir*t
// `dir` must be unit length (the binary asserts Vec3IsNormalized at com_math.cpp:1357).
static void Drag_ProjectPointOntoRay( float *out, const float *origin,
                                      const float *dir, const float *point )
{
    iassert( Vec3IsNormalized( dir ) );

    float dx = point[0] - origin[0];
    float dy = point[1] - origin[1];
    float dz = point[2] - origin[2];
    float t  = dir[0] * dx + dir[1] * dy + dir[2] * dz;
    out[0] = dir[0] * t + origin[0];
    out[1] = dir[1] * t + origin[1];
    out[2] = t * dir[2] + origin[2];
}

// ─── 0x4a5860  RayPointDistSq (terrain distance² helper) ──────────────────────
// Returns the squared perpendicular distance of `point` from the ray (origin, dir).
// `dir` must be unit length (the binary asserts Vec3IsNormalized at com_math.cpp:1344).
//   d = point - origin;  proj = dot(dir, d);  perp = d - dir*proj;  return |perp|²
// The math negates proj then re-adds it, in that exact order, to preserve float32 rounding.
static float Drag_RayPointDistSq( const float *point, const float *origin,
                                  const float *dir )
{
    iassert( Vec3IsNormalized( dir ) );

    float dx      = point[0] - origin[0];
    float dy      = point[1] - origin[1];
    float dz      = point[2] - origin[2];
    float proj    = dir[2] * dz + dir[0] * dx + dir[1] * dy;
    float negProj = -proj;
    float px      = dx + dir[0] * negProj;
    float py      = dy + dir[1] * negProj;
    float pz      = negProj * dir[2] + dz;
    return pz * pz + py * py + px * px;
}

// ─── 0x47f0c0  MoveSelection (apply a world-space move to the selection) ──────
// Called by Drag_MouseMoved with the snapped world delta.  Dispatches by mode:
//   rotate - grid-snap (move/grid * ClampGridSize) + sub-unit 0.1, axis and
//     angle from m_pActiveXY->m_nViewType, g_vRotation accumulate + status text, then the
//     free-rotate (Select_RotateAxis -> Select_ApplyMatrix per selected brush).  (KIWI: the
//     binary's patch-bend sub-branches went with bend mode.)
//   scale - axis locks from scale-how; factor move[1]<0 -> 0.9 / >0 -> 1.1
//   translate - move-points add, per-brush BuildWindings + the maxs<=mins / >262144 validity
//     test + Patch_DragScale; a blocked move is undone
//   sel_curvepoint (Patch_UpdateSelected) / sel_vertex (Brush_MoveVertex accumulate)
//   terrain points - the sculpt path (d_terrapoints), see below
// Asserts drag.cpp:711/800/747/748.  The status text goes to the decoupled
// MainFrm_SetStatusText (the binary uses get_m_strStatus + UpdateStatusText).

// KIWI: the per-brush check both MoveSelection drags open-coded (point drag, terrain-point
// drag).  Rebuild every selected brush after its points moved; return the first that went
// degenerate (max <= min, or > 262144, on an axis) or whose patch refused the scale
// (Patch_DragScale) - nullptr when every brush took the move.
static selbrush_t *Selection_RebuildCheckMove( float *move )
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        brush_t *def = b->def;
        float preExt[3] = { def->maxs[0] - def->mins[0],
                            def->maxs[1] - def->mins[1],
                            def->maxs[2] - def->mins[2] };
        Brush_RebuildAfterEdit( def );

        int okAxes = 0;
        for ( ; okAxes < 3; ++okAxes )
        {
            if ( def->maxs[okAxes] <= def->mins[okAxes] ) break;
            if ( def->maxs[okAxes] - def->mins[okAxes] > 262144.0f ) break;
        }
        if ( okAxes != 3 )
            return b;

        if ( b->patch )
        {
            float center[3] = {
                ( def->maxs[0] - def->mins[0] ) - preExt[0],
                ( def->maxs[1] - def->mins[1] ) - preExt[1],
                ( def->maxs[2] - def->mins[2] ) - preExt[2] };
            iassert( b->def->patch == b->patch->def );   // drag.cpp:800
            if ( !Patch_DragScale( center, b->patch->def, move ) )
                return b;
        }
    }
    return nullptr;
}

// The undo half: rebuild every selected brush after the move was taken back.
static void Selection_RebuildAll()
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        Brush_RebuildAfterEdit( b->def );
}

// `total` is the move since the press (the binary's fourth argument; Nudge passes its step).
void MoveSelection( float *origin, float *dir, float *move, float *total )
{
    char statusBuf[160];

    if ( move[0] == 0.0f && move[1] == 0.0f && move[2] == 0.0f )
        return;

    if ( g_bRotateMode )
        goto ROTATE_OR_BEND;

    // Scale-how axis lock: zero the locked component(s) of `move`.
    {
        char how = g_nScaleHow;
        if ( how & SCALE_X ) move[0] = 0.0f;
        if ( how & SCALE_Y ) move[1] = 0.0f;
        if ( how & SCALE_Z ) move[2] = 0.0f;

        // (IDA 0x47f1a8 checked patch-bend here, before scale - KIWI: bend mode is gone.)
        if ( g_bScaleMode )
        {
            // factor from the sign of move[1]: 0.9/1.1/1.0.
            float f = 1.0f;
            if      ( move[1] < 0.0f ) f = 0.89999998f;
            else if ( move[1] > 0.0f ) f = 1.1f;
            float sx = ( how & SCALE_X ) ? 1.0f : f;
            float sy = ( how & SCALE_Y ) ? 1.0f : f;
            float sz = ( how & SCALE_Z ) ? 1.0f : f;
            Select_Scale( sx, sy, sz );
            g_nUpdateBits = -1;
            return;
        }
    }

    // ── translation ─────────────────────────────────────────────────────────
    {
        float dist[3];
        dist[0] = g_pressdelta[0] - g_vPressStart[0];
        dist[1] = g_pressdelta[1] - g_vPressStart[1];
        dist[2] = g_pressdelta[2] - g_vPressStart[2];
        _snprintf( statusBuf, sizeof( statusBuf ),
                   "Distance x: %.1f  y: %.1f  z: %.1f", dist[0], dist[1], dist[2] );
        MainFrm_SetStatusText( 3, statusBuf );
    }

    {
        char pointDrag = 0;   // move points (or sel_area) are being dragged
        if ( g_qeglobals.d_num_move_points == 0 && g_qeglobals.d_select_mode != sel_area )
            goto ON_BRUSH_MOVE;
        pointDrag = 1;

        if ( g_qeglobals.d_select_mode == sel_area )
        {
            Sys_Printf( "MoveSelection for sel_area: should never happen, tell a coder\n" );
            return;
        }
        if ( g_qeglobals.d_select_mode == sel_curvepoint )
        {
            Patch_UpdateSelected( move, total );
            return;
        }
        if ( g_qeglobals.d_select_mode != sel_vertex )
        {
            // ── normal brush / move-point drag ───────────────────────────────
            // (nested scope so the early `goto ON_BRUSH_MOVE` above does not jump
            //  over these initialisers — they are out of scope at the label.)
            {
            for ( int ii = 0; ii < g_qeglobals.d_num_move_points; ++ii )
            {
                drawVert_t *mp = g_qeglobals.d_move_points[ii];
                mp->xyz[0] += move[0]; mp->xyz[1] += move[1]; mp->xyz[2] += move[2];
            }

            if ( Selection_RebuildCheckMove( move ) )
            {
                Com_PrintMessage( "MoveSelection: Attempted drag or nudge blocked because "
                                  "it would invalidate or destroy a brush.\n" );
                // Undo the move-point translation and rebuild.
                for ( int jj = 0; jj < g_qeglobals.d_num_move_points; ++jj )
                {
                    drawVert_t *mp = g_qeglobals.d_move_points[jj];
                    mp->xyz[0] -= move[0]; mp->xyz[1] -= move[1]; mp->xyz[2] -= move[2];
                }
                Selection_RebuildAll();
            }
            }  // end nested scope (ii)

        ON_BRUSH_MOVE:
            if ( g_qeglobals.d_numterrapoints )
            {
                // --- terrain-point drag (sculpt) ---
                // d_terrapoints[] are the side-face planept vec3's collected by
                // Brush_SideSelect (numberId==2 brush).
                // d_select_translate_unk = the dragged plane's normal (set by Brush_SideSelect).
                // Algorithm: centroid of the cluster → project onto the click ray → find the
                // nearest cluster point → scale the WHOLE cluster about the centroid by the
                // dragged radius ratio (capped at 5×); rebuild the selected brushes; if any
                // brush goes degenerate, undo the scale (×1/ratio) and rebuild.
                float centroid[3] = { 0.0f, 0.0f, 0.0f };
                for ( int i = 0; i < g_qeglobals.d_numterrapoints; ++i )
                {
                    const float *p = g_qeglobals.d_terrapoints[i];
                    centroid[0] = p[0] + centroid[0];
                    centroid[1] = p[1] + centroid[1];
                    centroid[2] = p[2] + centroid[2];
                }
                float inv = 1.0f / (double)g_qeglobals.d_numterrapoints;
                centroid[0] = inv * centroid[0];
                centroid[1] = centroid[1] * inv;
                centroid[2] = inv * centroid[2];

                iassert( origin );   // drag.cpp:747
                iassert( dir );   // drag.cpp:748

                // project the centroid onto the click ray (foot of perpendicular).
                float end[3];
                Drag_ProjectPointOntoRay( end, origin, dir, centroid );

                // find the cluster point nearest the projected point.
                int best = 0;
                {
                    const float *p0 = g_qeglobals.d_terrapoints[0];
                    float d0[3] = { p0[0] - end[0], p0[1] - end[1], p0[2] - end[2] };
                    float bestDist = d0[2] * d0[2] + d0[0] * d0[0] + d0[1] * d0[1];
                    for ( int i = 0; i < g_qeglobals.d_numterrapoints; ++i )
                    {
                        const float *p = g_qeglobals.d_terrapoints[i];
                        float dd[3] = { p[0] - end[0], p[1] - end[1], p[2] - end[2] };
                        float dist = dd[2] * dd[2] + dd[0] * dd[0] + dd[1] * dd[1];
                        if ( bestDist > dist )
                        {
                            bestDist = dist;
                            best = i;
                        }
                    }
                }

                // current radius of the nearest point from the ray (origin=centroid, dir=normal).
                float startRadius = (float)sqrt(
                    Drag_RayPointDistSq( g_qeglobals.d_terrapoints[best],
                                         centroid, g_qeglobals.d_select_translate_unk ) );
                if ( startRadius > 0.0f )
                {
                    // radius after applying the drag `move` to the nearest point.
                    const float *pb = g_qeglobals.d_terrapoints[best];
                    float moved[3] = { pb[0] + move[0], pb[1] + move[1], pb[2] + move[2] };
                    float endRadius = (float)sqrt(
                        Drag_RayPointDistSq( moved, centroid, g_qeglobals.d_select_translate_unk ) );
                    float ratio = ( endRadius - startRadius ) / startRadius + 1.0f;
                    if ( ratio > 0.0f )
                    {
                        if ( ratio > 5.0f )
                            ratio = 5.0f;

                        // scale the whole cluster about the centroid by `ratio`.
                        for ( int i = 0; i < g_qeglobals.d_numterrapoints; ++i )
                        {
                            float *pt = g_qeglobals.d_terrapoints[i];
                            float p[3] = { pt[0] - centroid[0],
                                           pt[1] - centroid[1],
                                           pt[2] - centroid[2] };
                            p[0] = p[0] * ratio;
                            p[1] = p[1] * ratio;
                            p[2] = p[2] * ratio;
                            pt[0] = p[0] + centroid[0];
                            pt[1] = p[1] + centroid[1];
                            pt[2] = p[2] + centroid[2];
                        }

                        // rebuild the selected brushes; undo the scale if any went degenerate.
                        if ( Selection_RebuildCheckMove( move ) )
                        {
                            Com_PrintMessage( "MoveSelection: Attempted drag or nudge blocked because "
                                              "it would invalidate or destroy a brush.\n" );
                            // undo: scale the cluster back about the centroid by 1/ratio.
                            float invRatio = 1.0f / ratio;
                            for ( int i = 0; i < g_qeglobals.d_numterrapoints; ++i )
                            {
                                float *pt = g_qeglobals.d_terrapoints[i];
                                float p[3] = { pt[0] - centroid[0],
                                               pt[1] - centroid[1],
                                               pt[2] - centroid[2] };
                                p[0] = p[0] * invRatio;
                                p[1] = p[1] * invRatio;
                                p[2] = p[2] * invRatio;
                                pt[0] = p[0] + centroid[0];
                                pt[1] = p[1] + centroid[1];
                                pt[2] = p[2] + centroid[2];
                            }
                            Selection_RebuildAll();
                        }
                    }
                }
            }
            else if ( !pointDrag )
            {
                Select_Move( move, 0 );      // ← the brush-drag demo path
            }
            return;
        }

        // ── sel_vertex: per-brush vertex move ────────────────────────────────
        {
            float end[3] = { 0, 0, 0 };
            int allOk = 1;
            for ( selbrush_t *sb = selected_brushes.next; sb != &selected_brushes; sb = sb->next )
                allOk &= Brush_MoveVertex( move, sb->def,
                                           g_qeglobals.d_move_points[0]->xyz, end, false );
            if ( allOk )
            {
                drawVert_t *mp0 = g_qeglobals.d_move_points[0];
                mp0->xyz[0] = end[0]; mp0->xyz[1] = end[1]; mp0->xyz[2] = end[2];
            }
        }
        return;
    }

ROTATE_OR_BEND:
    // ── Rotate (g_bRotateMode) drag ───────────────────────────────────────────────
    // Reached from `goto ROTATE_OR_BEND` at the top (g_bRotateMode; the binary's
    // patch-bend mode jumped here too).  The CString status machinery is replaced by
    // MainFrm_SetStatusText.
    {
        // (1) Grid-snap each move component to the angle/grid step (ClampGridSize),
        //     a float32 round-trip: move[i] = (move[i]/grid)*step.
        for ( int i = 0; i < 3; ++i )
        {
            if ( move[i] != 0.0f )
            {
                move[i] = move[i] / grid_sizes[g_qeglobals.d_gridsize];
                move[i] = move[i] * (float)ClampGridSize();   // integer multiply
            }
        }
        // (2) Sub-unit grid → extra 0.1 scale (runs when grid<=1).  The constant is
        //     0.10000000149011612 (= (double)0.1f); multiply in double then truncate to
        //     float32, as the binary's 80-bit x87 multiply did.
        if ( grid_sizes[g_qeglobals.d_gridsize] <= 1.0f )
        {
            for ( int j = 0; j < 3; ++j )
                if ( move[j] != 0.0f )
                    move[j] = (float)( (double)move[j] * 0.10000000149011612 );
        }

        // (3) Pick the rotation axis + the signed angle from the active XY view type.
        //     m_nViewType: 2=XY(default), 1=XZ, 0=YZ.
        //       rotAxisAngle (-move[1] XY, +move[2] XZ, -move[2] YZ) is the rotation amount;
        //       rotationDelta (the same component, unnegated) accumulates into g_vRotation.
        int   axisIdx       = 0;
        float rotAxisAngle  = -move[2];
        float rotationDelta =  move[2];
        xywndState_t *activeXY = Ed_ActiveXY();
        if ( activeXY->m_nViewType == 2 )        // XY (top) → rotate about Z
        {
            rotAxisAngle  = -move[1];
            rotationDelta =  move[1];
            axisIdx       =  2;
        }
        else if ( activeXY->m_nViewType == 1 )   // XZ (front) → rotate about Y
        {
            rotAxisAngle  =  move[2];
            rotationDelta =  move[2];
            axisIdx       =  1;
        }
        // else YZ (side) → axisIdx 0 (rotate about X), rotAxisAngle = -move[2].

        // (4) Accumulate the rotation total + status text.
        g_vRotation[axisIdx] += rotationDelta;
        {
            char statusRot[160];
            _snprintf( statusRot, sizeof( statusRot ),
                       "%s x:: %.1f  y:: %.1f  z:: %.1f",
                       "Rotation", g_vRotation[0], g_vRotation[1], g_vRotation[2] );
            MainFrm_SetStatusText( 2, statusRot );
        }

        // (5) Apply the rotation. rot_around[0] = pivot origin (g_vRotateOrigin),
        //     rot_around[1..3] = the 3x3 built by Select_RotateAxis. The buffer is
        //     [4][3] (12 floats) per the established caller pattern (mainfrm.cpp).
        // (The binary's patch-bend sub-branch - Patch_SelectBendNormal at 2*angle, then
        // Patch_SelectBendAxis at angle - went with bend mode.)
        float rot_around[4][3];
        if ( rotAxisAngle != 0.0f )
        {
            // Free rotate: rotate every selected brush about the pivot by rotAxisAngle.
            rot_around[0][0] = g_vRotateOrigin[0];
            rot_around[0][1] = g_vRotateOrigin[1];
            rot_around[0][2] = g_vRotateOrigin[2];
            Select_RotateAxis( axisIdx, rotAxisAngle, (float (*)[4][3])rot_around );
            for ( selbrush_t *n = selected_brushes.next; n != &selected_brushes; n = n->next )
                Select_ApplyMatrix( rot_around[0], n, 0, rotAxisAngle, 0 );
        }
    }
    return;
}

// ─── 0x47ff30  Drag_MouseMoved (a mouse-move during a press) ─────────────────
// Drag_IsPointEditMode(m) = (m >= sel_editpoint(10) && m <= 15) equals the binary's
// explicit exclusion chains (modes 10..15).
// Converts the pixel delta since the press into a grid-snapped world move and
// hands it to MoveSelection. x/y are the CURRENT pixel coords (CXYWnd passes
// raw pixels — see XY_MouseMoved 0x468230); origin/dir are the pick ray (the terrain-point
// drag and the addpoint path).  The move basis g_drag_xvec/yvec is the AXIALISED (unit)
// view basis, so the move is in pixel units and then grid-snapped; m_fScale is applied
// only in rotate mode.
void Drag_MouseMoved( int x, int y, int buttons, float *origin, float *dir )
{
    if ( !buttons )
    {
        drag_ok = 0;
        g_qeglobals.toggle_unk03_mousedrag_state1 = 0;
        g_qeglobals.toggle_unk04_mousedrag_state2 = 0;
        return;
    }

    const select_t d_select_mode = g_qeglobals.d_select_mode;
    unsigned int btn = buttons;

    // Shift held: constrain the drag to the dominant pixel axis.
    if ( buttons == MK_SHIFT && !Drag_IsPointEditMode( g_qeglobals.d_select_mode ) )
    {
        drag_first = 0;
        btn = MK_SHIFT;
        if ( abs32( x - g_pressx_real ) <= abs32( y - g_pressy_real ) )
            x = g_pressx_real;
        else
            y = g_pressy_real;
    }

    if ( g_qeglobals.toggle_unk03_mousedrag_state1 || g_qeglobals.toggle_unk04_mousedrag_state2 )
    {
        if ( g_qeglobals.d_select_mode != sel_curvepoint
             && !Drag_IsPointEditMode( g_qeglobals.d_select_mode ) )
        {
            switch ( btn )
            {
                case MK_LBUTTON | MK_SHIFT:                       return;
                case MK_LBUTTON | MK_CONTROL:
                    if ( GetAsyncKeyState( VK_MENU ) < 0 )        return;
                    break;
                case MK_LBUTTON | MK_SHIFT | MK_CONTROL:         return;
            }
        }
        g_qeglobals.toggle_unk03_mousedrag_state1 = 0;
        g_qeglobals.toggle_unk04_mousedrag_state2 = 0;
        return;
    }

    if ( !drag_ok )
        return;

    // Shift held with another button: the same dominant-axis lock.
    if ( ( btn & MK_SHIFT ) != 0 && !Drag_IsPointEditMode( g_qeglobals.d_select_mode ) )
    {
        btn = buttons;
        drag_first = 0;
        if ( abs32( x - g_pressx_real ) <= abs32( y - g_pressy_real ) )
            x = g_pressx_real;
        else
            y = g_pressy_real;
    }

    // ── the drag: pixel delta → snapped world move → MoveSelection ───────────
    const bool rotate = g_bRotateMode;
    float dx = (float)( x - g_pressx_real );
    float dy = (float)( y - g_pressy_real );
    float drag_vec[3];

    for ( int i = 0; i < 3; ++i )
    {
        float w = g_drag_xvec[i] * dx + g_drag_yvec[i] * dy;
        drag_vec[i] = w;

        if ( rotate )
        {
            w *= Ed_ActiveXY()->m_fScale;                 // scale only in rotate
            drag_vec[i] = w;
            int c = ClampGridSize();
            if ( c == 1 )      drag_vec[i] = w * 0.5f;
            else if ( c == 2 ) drag_vec[i] = w * 0.75f;
        }

        if ( selected_brushes.next != &selected_brushes
             && selected_brushes.next->patch && rotate && ClampGridSize() == 1 )
        {
            drag_vec[i] *= 0.5f;
        }
        else if ( !g_PrefsDlg->m_bNoClamp )   // m_bNoClamp → grid snap when off (default 0)
        {
            float gs = grid_sizes[g_qeglobals.d_gridsize];
            drag_vec[i] = (float)floor( 0.5 + drag_vec[i] / gs ) * gs;
        }
    }

    float move[3];
    move[0] = drag_vec[0] - g_pressdelta[0];
    move[1] = drag_vec[1] - g_pressdelta[1];
    move[2] = drag_vec[2] - g_pressdelta[2];
    g_pressdelta[0] = drag_vec[0];
    g_pressdelta[1] = drag_vec[1];
    g_pressdelta[2] = drag_vec[2];

    if ( d_select_mode == sel_area || Drag_IsPointEditMode( d_select_mode ) )
    {
        g_qeglobals.drag_selectionbox_y_1 = x;
        g_qeglobals.drag_selectionbox_y_2 = y;
    }
    else
    {
        MoveSelection( origin, dir, move, drag_vec );
    }
}

// ── marquee box-select wiring (select.cpp / xywnd.cpp) ───────────────────────
extern void Select_FlipFilteredBrushes( const float *boxMins, const float *boxMaxs, char bActiveList );
extern void Ed_XY_ToPoint( xywndState_t *wnd, int x, int y, float *start, float *dir );

// ── sel_area control-point rect-select (pmesh.cpp 0x448460 / 0x448620) ────────
extern void Terrain_SelectAreaPoints( const void *planes, char select );
extern void Patch_SelectAreaPoints( const void *planes, char select );

// ─── 0x4718d0  Select_RemoveEmptyFaces (post vertex/edge-drag cleanup) ────────
// For every selected brush: drop any face whose winding went null during the drag
// (Brush_RemoveFace shifts the array down, so don't advance the index after a
// removal), then either bump the def version (>=4 faces survive → still a valid
// brush) or free the brush outright (<4 faces → collapsed). Called by Drag_MouseUp
// when the closed drag was a vertex/edge edit (sel_vertex/sel_edge). It reads the
// DEF faceCount (the authoritative count).  The binary's two indices (RemoveFace arg +
// bound, winding test) always stay equal, so the single running index `i` is exact.
static void Select_RemoveEmptyFaces()
{
    selbrush_t *sb = selected_brushes.next;
    while ( sb != &selected_brushes )
    {
        selbrush_t *next = sb->next;
        brush_t    *def  = sb->def;

        // Remove null-winding faces. `i` steps on a kept face and holds on a removed one.
        for ( unsigned int i = 0; (int)i < def->faceCount; )
        {
            if ( def->faces[i].w )
                ++i;
            else
                Brush_RemoveFace( def, i );   // shifts down → re-test the same index
        }

        if ( sb->def->faceCount >= 4 )
            ++sb->def->version;
        else
            Brush_Free( sb );

        sb = next;
    }
}

// ─── 0x4802a0  Drag_MouseUp (end of a press) ─────────────────────────────────
// The Select_FlipFilteredBrushes flag is NOT inverted: sel_areabrush_sub(13) -> flip(0);
// 12 -> flip(1).
// Resets the drag toggles, performs the MARQUEE box-select if one was active, and
// closes the undo record opened by Drag_Setup/Drag_Begin (Undo_End/Undo_EndBrushList
// self-guard when no record is open, e.g. a shift+click select).
//
// Selection modes set by Drag_Setup's box-drag branch:
//   sel_areabrush     (12)  Alt+Shift+LMB drag → box ADD-select   (active → selected)
//   sel_areabrush_sub (13)  Alt+Shift+RMB drag → box DESELECT     (selected → active),
//                            or a single click-select via Select_ClickTrace if barely moved.
// The world box is built from the drag rectangle (press corner = x_1,x_2; current
// corner = y_1,y_2) via XY_ToPoint (0x480484..0x48057b).
//
// sel_area (5) is the 3D camera-view point rect-select (terrain/patch CONTROL POINTS):
// build the view frustum from the drag rect via the press-callback (g_qeglobals.camera_fov_setup,
// installed = Camera_GetRectSelection3D by Drag_Begin for a camera-view area drag), then add the
// covered control points to the move-point sets (Terrain/Patch_SelectAreaPoints).  After it the
// binary switches the tool to curve-point mode (4) and re-reads d_select_mode for the
// UpdatePatchToolbarButtons (mode 9) / Patch_FinishCurveDrag (mode 8) tail — both inert when the
// entry mode was sel_area (5); the port keeps only the mode-9 test.  Editpoint siblings
// 11/14/15 = point edit.
void Drag_MouseUp( unsigned int buttons )
{
    g_qeglobals.toggle_unk03_mousedrag_state1 = 0;
    g_qeglobals.toggle_unk04_mousedrag_state2 = 0;
    drag_ok = 0;

    // status-bar pane 0 = "drag completed." (SendMessageA + d_hwndStatus, as in Drag_Setup).
    SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0, (LPARAM)"drag completed." );

    const select_t mode = g_qeglobals.d_select_mode;

    // ── sel_area (5): camera-view rect-select of terrain/patch CONTROL POINTS ──
    // Build the 4-plane view frustum from the drag rect via the
    // installed press callback (g_qeglobals.camera_fov_setup == Camera_GetRectSelection3D),
    // then collect the covered control points.  `(buttons>>3)&1` (the cleaned MK flag) =
    // the ADD-vs-fresh select arg.
    if ( mode == sel_area )
    {
        if ( g_qeglobals.camera_fov_setup )
        {
            int planeBuf[32];   // 4 planes × 32 bytes
            ( (void (__cdecl *)( int, int, int, int, int * ))g_qeglobals.camera_fov_setup )(
                g_qeglobals.drag_selectionbox_x_1, g_qeglobals.drag_selectionbox_x_2,
                g_qeglobals.drag_selectionbox_y_1, g_qeglobals.drag_selectionbox_y_2, planeBuf );

            char select = (char)( ( buttons >> 3 ) & 1 );
            Terrain_SelectAreaPoints( planeBuf, select );
            Patch_SelectAreaPoints( planeBuf, select );

            const select_t prev = g_qeglobals.d_select_mode; // re-read (unchanged by the above)
            g_qeglobals.d_select_mode = sel_curvepoint;      // switch to curve-point edit (4)
            if ( prev == sel_cycle_edge_direction_quad )     // 9
            {
                CMainFrame_UpdatePatchToolbarButtons();
            }
            g_nUpdateBits = -1;
        }
        // (mode now sel_curvepoint(4), so the marquee branch below is skipped and the
        //  shared translate/undo-close tail runs.)
    }
    // ── sel_areabrush / sel_areabrush_sub (12/13): the BRUSH marquee box-select ─
    // The whole marquee/click dispatch is gated on g_nPatchClickedView != W_CAMERA
    // (a camera-view press skips straight to the undo-close).  Drag_Setup only ARMS
    // modes 12/13 when g_nPatchClickedView != 1, so the two are mutually exclusive in
    // practice; the guard is kept anyway.
    else if ( ( mode == sel_areabrush || mode == sel_areabrush_sub )
              && g_nPatchClickedView != W_CAMERA )
    {
        xywndState_t *xy = Ed_ActiveXY();   // never NULL

        // mode 13 (box deselect) with a sub-pixel drag = a click → single face/brush
        // pick via Select_ClickTrace.
        bool didClick = false;
        if ( mode == sel_areabrush_sub )
        {
            float dist = sqrtf( g_pressdelta[0]*g_pressdelta[0]
                              + g_pressdelta[1]*g_pressdelta[1]
                              + g_pressdelta[2]*g_pressdelta[2] );
            if ( dist < 1.0f )
            {
                float start[3] = { 0.0f, 0.0f, 0.0f };
                float dir[3];
                Ed_XY_ToPoint( xy, g_qeglobals.drag_selectionbox_x_1,
                                   g_qeglobals.drag_selectionbox_x_2, start, dir );
                // Alt+LMB(==4) selects a single FACE (flag 4); else the whole brush (64).
                int flag = 64;
                if ( buttons == 4 && GetAsyncKeyState( VK_MENU ) < 0
                     && mode != sel_curvepoint && mode != sel_terrainpoint
                     && mode != sel_terraintexture )
                    flag = 4;
                Select_ClickTrace( dir, start, flag );
                g_qeglobals.d_select_mode = sel_brush;
                g_nUpdateBits = -1;
                didClick = true;
            }
        }

        if ( !didClick )
        {
            // Build the world box from the two snapped corners (only the .start of
            // XY_ToPoint is used; the depth axis carries the 131072 ray offset which
            // is harmless for a "Tall" two-axis bounds test).
            float c1[3] = { 0.0f, 0.0f, 0.0f };   // press corner (x_1, x_2)
            float c2[3] = { 0.0f, 0.0f, 0.0f };   // current corner (y_1, y_2)
            float dir[3];
            Ed_XY_ToPoint( xy, g_qeglobals.drag_selectionbox_x_1,
                               g_qeglobals.drag_selectionbox_x_2, c1, dir );
            Ed_XY_ToPoint( xy, g_qeglobals.drag_selectionbox_y_1,
                               g_qeglobals.drag_selectionbox_y_2, c2, dir );
            float boxMins[3], boxMaxs[3];
            for ( int i = 0; i < 3; ++i )
            {
                boxMins[i] = ( c1[i] <= c2[i] ) ? c1[i] : c2[i];
                boxMaxs[i] = ( c1[i] >= c2[i] ) ? c1[i] : c2[i];
            }
            // mode 12 → walk active list (ADD); mode 13 → walk selected list (DESELECT).
            Select_FlipFilteredBrushes( boxMins, boxMaxs, mode == sel_areabrush ? 1 : 0 );
            g_qeglobals.d_select_mode = sel_brush;
            g_nUpdateBits = -1;
        }
    }
    // ── point-edit rectangle siblings 14/15: close the rect back to sel_vertex ──
    // Same g_nPatchClickedView != W_CAMERA gate as the marquee arm above.
    else if ( ( mode == sel_areapoint_curve || mode == sel_areapoint )
              && g_nPatchClickedView != W_CAMERA )
    {
        g_qeglobals.d_select_mode = sel_vertex;   // mode = 1
        g_nUpdateBits = -1;
    }

    // ── pending nudge-translate, then close the undo record ────────────────────
    // If a translate was queued (Select_Move w/ snap), apply it and clear it.
    // d_select_translate is written by the keyboard-nudge path; it is {0,0,0} until that
    // lands, so this is currently a no-op.
    if ( g_qeglobals.d_select_translate[0] != 0.0f
      || g_qeglobals.d_select_translate[1] != 0.0f
      || g_qeglobals.d_select_translate[2] != 0.0f )
    {
        Select_Move( g_qeglobals.d_select_translate, 1 );   // KIWI: snap = 1 (IDA shows the 1 as W_CAMERA)
        g_qeglobals.d_select_translate[0] = 0.0f;
        g_qeglobals.d_select_translate[1] = 0.0f;
        g_qeglobals.d_select_translate[2] = 0.0f;
        g_nUpdateBits |= W_CAMERA;
    }

    // The binary cleared status pane 3 here (get_m_strStatus + CMainFrame::UpdateStatusText).
    // The port decoupled the MFC m_strStatus mechanism (see Sys_UpdateStatusBar,
    // win_qe3.cpp) and drives status panes via SendMessageA instead, so this cosmetic
    // clear is omitted.

    // A vertex/edge drag can collapse a face to a null winding — Select_RemoveEmptyFaces
    // drops empties / frees sub-4-face brushes before closing the undo record.
    if ( g_qeglobals.d_select_mode == sel_edge || g_qeglobals.d_select_mode == sel_vertex )
        Select_RemoveEmptyFaces();
    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

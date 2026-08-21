#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ═════════════════════════════════════════════════════════════════════════════════════
//  kiwi_uveditor.cpp — KIWI-UX (ROUND BD): THE UV EDITOR WINDOW.
// ═════════════════════════════════════════════════════════════════════════════════════
//
//  Copyright (C) 2010 Kristian Duske
//
//  Portions of this file are derived from TrenchBroom
//  (https://github.com/TrenchBroom/TrenchBroom): UvViewHelper.cpp (snapDelta, zoom fit),
//  UvOffsetTool.cpp (vertex-to-grid offset snap), UvScaleTool.cpp (origin-to-handle ratio
//  + vertex snap), UvRotateTool.cpp (grab-offset angle, the four-phase edge-angle snap and
//  its 150/pow(d,0.8) threshold), UvShearTool.cpp (the shear factors, the edge-slope snap
//  and the #1350 near-axis guard), UvOriginTool.cpp (origin snap candidates),
//  UvCameraTool.cpp (cursor-anchored ×1.1 wheel zoom) and UvEditor.cpp (the toolbar).
//  ROUND BI removed the two derivations that hung the SCALE and SHEAR handles on the UV
//  GRID LINES (UvViewHelper::pickUvGrid) and the rotate RING (UvRotateTool::pick) — see
//  D-BI-A.  The gesture MATH those tools carried is still TB's and is still credited here.
//
//  TrenchBroom is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  TrenchBroom is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
// ═════════════════════════════════════════════════════════════════════════════════════
// READ kiwi_uveditor.h FIRST.  It carries the whole design: why this is a 2D UV canvas
// and not TrenchBroom's world-space ortho view (D-BD-A), how one 2×3 affine reaches a
// texdef through Face_MoveTexture + texturevecs_02 (D-BD-B), why OFFSET bypasses that and
// is exact (D-BD-C), why the background tiles by hand (D-BD-D), why this file owns no D3D
// object (D-BD-E), why a patch brush contributes its patch and not its box faces (D-BD-F),
// the undo shape (D-BD-G) and what v1 does not do (D-BD-H).  This file is the mechanical
// half.
//
// KIWI-UX (ROUND BI): the GESTURE LAYER above all of that was replaced on a user verdict —
// the controls came off the grid lines and onto the shapes.  D-BI-A..F in the header carry
// that argument (what was rejected and the measurement that condemned it, the transform
// box, first-class sub-shape selection with a click cycle and a marquee, the pivot that
// follows the target set, the grid demoted to furniture, and the snap sets captured at the
// press).  The WRITE path below D-BD-B / D-BG-A is untouched by that round.
// ═════════════════════════════════════════════════════════════════════════════════════

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_uveditor.h"
#include "kiwi_command.h"
#include "kiwi_selection.h"
#include "kiwi_uv.h"                 // KiwiUv_FaceTexdef — THE per-face texdef accessor
#include "kiwi_windows.h"

// Include order mirrors kiwi_skybox.cpp:33-39, the other KIWI TU that walks a Material's
// textureTable down to its GfxImage and hands the result to ImGui.
#include <gfx_d3d/r_material.h>      // Material / MaterialTextureDef / textureTable
#include <gfx_d3d/r_gfx.h>           // GfxImage / GfxTexture / MAPTYPE_2D
#include <d3d9.h>                    // IDirect3DTexture9 (the ImTextureID we hand over)

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>          // ROUND BG: std::sort, for the fold-out's edge weld
#include <vector>

// ── ported / cross-file entry points (each verified against its definition) ─────────
extern int         Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:118   int Sys_Printf(const char*,...)
extern int         g_nUpdateBits;                                            // engine_stubs.cpp:773  int g_nUpdateBits
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1358  bool Radiant_RegisterCommand(const char*,byte,byte,int)
extern ImGuiID     ImGuiShell_DockRoot();                                    // imgui_shell.cpp:796   ImGuiID ImGuiShell_DockRoot()

// The forward transform and its inverse.  Declarations copied VERBATIM from brush.cpp:
// 1736-1737 and brush.cpp:1748-1749 — the byte-pointer `int` parameters are the binary's
// usercall convention and are not to be "tidied" (texturevecs.cpp:94-101 / :195-221).
extern void Face_MoveTexture( int surfDef, const float *normal, int outVecs,
                               int uvBase, float sizeX, float sizeY );       // texturevecs.cpp:101  void Face_MoveTexture(int,const float*,int,int,float,float)
extern void texturevecs_02( int surfDef, int uvVecs, float v5, int normal,
                             float dist, int arg6, int arg7, int arg8 );     // texturevecs.cpp:219  void texturevecs_02(int,int,float,int,float,int,int,int)

extern void        TexMatToFakeTexCoords( MaterialDef *def, texdef_sub_t *texDef );  // materialdef.cpp:377  void TexMatToFakeTexCoords(MaterialDef*,texdef_sub_t*)
extern void        Brush_BuildWindings( brush_t *b, int bFull );             // brush.cpp:1434    void Brush_BuildWindings(brush_t*,int)
extern void        SetupVertexSelection();                                   // select.cpp:4617   void SetupVertexSelection()
extern void        MarkMapModified();                                        // win_qe3.cpp:195   void MarkMapModified(void)
extern void        sub_477D70( selbrush_t *b, const float *mat );            // brush.cpp:205     void sub_477D70(selbrush_t*,const float*)
extern float       world_orient_matrix[4][3];                                // entity.cpp:312    float world_orient_matrix[4][3]

extern qtexture_s *MaterialDef_GetLayeredMaterial( MaterialDef *def );       // materialdef.cpp:168  qtexture_s *MaterialDef_GetLayeredMaterial(MaterialDef*)
extern LayerMaterialDef *Materialdef_GetName( MaterialDef *mtlDef );         // materialdef.cpp:159  LayerMaterialDef *Materialdef_GetName(MaterialDef*)
extern qtexture_s *Texture_GetHandle( const char *name );                    // texwnd.cpp:314    qtexture_s *Texture_GetHandle(const char*)

// The patch side.  Patch_ShiftTexture( p, 0, 0 ) is used as the REBUILD: it adds zero to
// every control ST and then runs the exact tail this file needs (bDirty for layer 1, free
// curveDef, Patch_GenericMesh2, ++version — pmesh.cpp:3241-3246).  Writing that tail out
// again here would be a second, quietly divergent spelling of a ported sequence.
extern void        Patch_ShiftTexture( patchMesh_t *p, float s, float t );   // pmesh.cpp:3216    void Patch_ShiftTexture(patchMesh_t*,float,float)
extern void        Patch_NaturalizeSelected( bool unk, bool cap, float x, float y ); // pmesh.cpp:2742  void Patch_NaturalizeSelected(bool,bool,float,float)
extern void        Select_SetTexture( float *out );                          // select.cpp:1179   void Select_SetTexture(float*)

// The two sentinel lists come from qe3.h:1053-1054; g_SelectedFaces from qe3.h:263.

namespace
{
    // ═══════════════════════════════════════════════════════════════════════════════
    //  constants that are not tuning (tuning lives in kiwi_uveditor.h)
    // ═══════════════════════════════════════════════════════════════════════════════
    const float KUVE_EPS        = 1.0e-6f;
    const int   KUVE_MAX_WINDING = 64;   // EdLayerGeom's own cap (qe3.h:216) — brush faces
                                         // are convex, so 64 points is ample.
    const float KUVE_PI         = 3.14159265358979323846f;

    // Colours.  Deliberately a fixed light/dark PAIR rather than a luma sample of the
    // bound texture: sampling would mean locking and reading a GPU surface every frame
    // (D-BD-E says this file owns nothing), and TB's own adaptive rule is a hard binary
    // (UvView.fragsh: luma < 0.5 ? 0.9-gray : 0.1-gray) whose two outputs are exactly
    // these two.  The background is dimmed instead, which makes the light arm always the
    // right one.
    const ImU32 KUVE_COL_BG        = IM_COL32(  24,  24,  28, 255 );
    const ImU32 KUVE_COL_TILE_TINT = IM_COL32( 168, 168, 168, 255 );  // multiply tint: dim
    const ImU32 KUVE_COL_GRID_MAJ  = IM_COL32( 230, 230, 230, 150 );
    const ImU32 KUVE_COL_GRID_MIN  = IM_COL32( 230, 230, 230,  98 );  // 150 * 0.65 (TB)
    const ImU32 KUVE_COL_FACE      = IM_COL32( 120, 200, 255, 190 );
    const ImU32 KUVE_COL_ACTIVE    = IM_COL32( 255, 210,  90, 255 );
    const ImU32 KUVE_COL_PATCH     = IM_COL32( 150, 255, 150, 190 );
    const ImU32 KUVE_COL_HANDLE    = IM_COL32( 247, 230,  59, 255 );  // TB HandleColor
    const ImU32 KUVE_COL_HANDLE_HI = IM_COL32( 255,  40,  40, 255 );  // TB SelectedHandleColor
    // KIWI-UX (ROUND BJ, ITEM 6): the snap accent.  The 3D pass paints its marker
    // near-black (kiwi_snap.cpp KSNAP_COL_MARK) because it draws over a bright
    // viewport; this canvas is a dimmed texture on a near-black plate, so the same
    // glyph is drawn bright.  The GLYPH is what carries the language, not the hue.
    const ImU32 KUVE_COL_SNAP      = IM_COL32( 120, 255, 235, 240 );
    const ImU32 KUVE_COL_UAXIS     = IM_COL32( 255,  61,   0, 178 );  // TB (1.0,0.24,0.0,0.7)
    const ImU32 KUVE_COL_VAXIS     = IM_COL32(  74, 148,   0, 178 );  // TB (0.29,0.58,0.0,0.7)

    // ═══════════════════════════════════════════════════════════════════════════════
    //  the 2×3 affine (D-BD-B).  Row-major: x' = m[0]x + m[1]y + m[2].
    // ═══════════════════════════════════════════════════════════════════════════════
    struct uvXform_t { float m[6]; };

    uvXform_t XfIdentity()
    {
        uvXform_t x = { { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f } };
        return x;
    }

    uvXform_t XfTranslate( float dx, float dy )
    {
        uvXform_t x = { { 1.0f, 0.0f, dx, 0.0f, 1.0f, dy } };
        return x;
    }

    // T(origin) · L · T(-origin) — every rotate / scale / skew in this file pins the
    // origin handle, which is TB's rule (SURVEY_TRENCHBROOM §7 item 7) reached here by
    // construction instead of by a compensating second write.
    uvXform_t XfAboutOrigin( float l00, float l01, float l10, float l11,
                             float ox, float oy )
    {
        uvXform_t x;
        x.m[0] = l00; x.m[1] = l01; x.m[2] = ox - ( l00 * ox + l01 * oy );
        x.m[3] = l10; x.m[4] = l11; x.m[5] = oy - ( l10 * ox + l11 * oy );
        return x;
    }

    void XfApply( const uvXform_t &x, float sIn, float tIn, float *sOut, float *tOut )
    {
        const float s = x.m[0] * sIn + x.m[1] * tIn + x.m[2];
        const float t = x.m[3] * sIn + x.m[4] * tIn + x.m[5];
        *sOut = s;
        *tOut = t;
    }

    bool XfIsIdentity( const uvXform_t &x )
    {
        return fabsf( x.m[0] - 1.0f ) < KUVE_EPS && fabsf( x.m[1] ) < KUVE_EPS
            && fabsf( x.m[2] )        < KUVE_EPS && fabsf( x.m[3] ) < KUVE_EPS
            && fabsf( x.m[4] - 1.0f ) < KUVE_EPS && fabsf( x.m[5] ) < KUVE_EPS;
    }

    // ── KIWI-UX (ROUND BG): the algebra the fold-out needs (D-BG-A) ────────────────
    // Composition, `a AFTER b`: (a∘b)(x) = a(b(x)).  Row-major 2×3, translation in [2]/[5].
    uvXform_t XfMul( const uvXform_t &a, const uvXform_t &b )
    {
        uvXform_t r;
        r.m[0] = a.m[0] * b.m[0] + a.m[1] * b.m[3];
        r.m[1] = a.m[0] * b.m[1] + a.m[1] * b.m[4];
        r.m[2] = a.m[0] * b.m[2] + a.m[1] * b.m[5] + a.m[2];
        r.m[3] = a.m[3] * b.m[0] + a.m[4] * b.m[3];
        r.m[4] = a.m[3] * b.m[1] + a.m[4] * b.m[4];
        r.m[5] = a.m[3] * b.m[2] + a.m[4] * b.m[5] + a.m[5];
        return r;
    }

    // A display transform is always invertible by construction (its linear part is
    // orthonormal, det ±1), but this is the general 2×3 inverse and it guards anyway:
    // a singular argument would otherwise divide by zero and poison every texdef the
    // conjugation touches.  Singular ⇒ identity, i.e. "no display transform".
    uvXform_t XfInverse( const uvXform_t &x )
    {
        const float det = x.m[0] * x.m[4] - x.m[1] * x.m[3];
        if ( fabsf( det ) < KUVE_EPS )
            return XfIdentity();
        const float inv = 1.0f / det;
        uvXform_t r;
        r.m[0] =  x.m[4] * inv;
        r.m[1] = -x.m[1] * inv;
        r.m[3] = -x.m[3] * inv;
        r.m[4] =  x.m[0] * inv;
        r.m[2] = -( r.m[0] * x.m[2] + r.m[1] * x.m[5] );
        r.m[5] = -( r.m[3] * x.m[2] + r.m[4] * x.m[5] );
        return r;
    }

    // The LINEAR part only — a delta is a vector, not a point (D-BG-A: the offset case
    // of the conjugation is D⁻¹'s linear part applied to the displayed delta).
    void XfApplyVec( const uvXform_t &x, float sIn, float tIn, float *sOut, float *tOut )
    {
        const float s = x.m[0] * sIn + x.m[1] * tIn;
        const float t = x.m[3] * sIn + x.m[4] * tIn;
        *sOut = s;
        *tOut = t;
    }

    // THE CONJUGATION, D-BG-A: a gesture affine `A` expressed in DISPLAYED space becomes
    // the TRUE-frame affine that may reach a texdef.  Identity D ⇒ A unchanged, which is
    // why the active face costs nothing and why BD's behaviour is bit-identical whenever
    // the chain is off or the selection is a single face.
    uvXform_t XfConjugate( const uvXform_t &A, const uvXform_t &D )
    {
        if ( XfIsIdentity( D ) )
            return A;
        return XfMul( XfInverse( D ), XfMul( A, D ) );
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  the gathered selection
    // ═══════════════════════════════════════════════════════════════════════════════
    struct uvBrush_t
    {
        selbrush_t  *node;    // the INSTANCE node (may be null for a g_SelectedFaces row
                              // whose brush we could not resolve — never dereferenced then)
        brush_t     *def;
        patchMesh_t *pm;      // non-null == this row IS a patch (D-BD-F)
    };
    struct uvFace_t
    {
        int brushIdx;
        int faceIndex;
    };

    std::vector<uvBrush_t> s_brushes;
    std::vector<uvFace_t>  s_faces;
    std::vector<int>       s_patches;    // indices into s_brushes
    int                    s_activeFace = -1;   // index into s_faces, or -1

    // ── KIWI-UX (ROUND BG), D-BG-A: the DISPLAY transforms, parallel to s_faces /
    //    s_patches.  Identity means "drawn at its true ST", which is BD's behaviour and
    //    is what every entry holds while the chain is off. ──────────────────────────
    std::vector<uvXform_t> s_faceD;
    std::vector<uvXform_t> s_patchD;
    // ...and the CACHE of the last solve, replayed onto the above every frame (Gather()
    // resets them).  Index-parallel, so it is only valid while the STRUCTURE signature —
    // which rows were gathered, in which order — is unchanged.
    std::vector<uvXform_t> s_chainD;
    std::vector<uvXform_t> s_chainPD;
    bool                   s_chain      = true;   // toolbar toggle, session-only (no registry)
    // ROUND BJ, ITEM 7: the texdef half of the old signature is GONE.  The layout is
    // latched on the STRUCTURE alone — see the note in DrawCanvas.
    unsigned               s_chainStructSig = 0;  // (gathered rows, order, anchor, toggle)
    bool                   s_chainBuilt = false;
    int                    s_chainFolded = 0;     // how many faces the fold actually placed
    int                    s_chainShelved = 0;    // ...and how many had to be shelved

    // ── KIWI-UX (ROUND BG), D-BG-B: the UV-EDITOR-LOCAL target sub-selection.  These are
    //    parallel to s_faces / s_patches and are re-derived every gather; `s_targetAll`
    //    is the BD state (everything gathered is a target) and is what an empty-canvas
    //    click restores.  NOTHING here touches the 3D selection. ────────────────────
    std::vector<char>      s_faceTgt;
    std::vector<char>      s_patchTgt;
    bool                   s_targetAll = true;
    // Kept across gathers so a target set survives a repaint: keyed the same way the
    // gesture snapshot is (def + faceIndex / patchMesh_t*), and re-matched, never
    // dereferenced.
    struct uvTgtKey_t { brush_t *def; int faceIndex; patchMesh_t *pm; };
    std::vector<uvTgtKey_t> s_targetKeys;
    // ── KIWI-UX (ROUND BJ, ITEM 2): the selection-change stamp (D-BJ-D) ────────────
    // FALSE means "this gather may derive the default target set from the EXPLICIT 3D
    // face selection".  It is set the moment that derivation runs (once) and cleared
    // only by a Sel_Generation change, so a canvas-local sub-selection the user built
    // survives every unrelated re-gather — the gather runs EVERY FRAME — while a real
    // 3D selection change (including a shift-click that adds a face) re-derives.
    bool                   s_tgtSeeded = false;

    // ── KIWI-UX (ROUND BN, ITEM 3): the SCOPE latch (kiwi_uveditor.h "IN SCOPE") ───
    // Written once per frame by KiwiUvEd_Draw — the only place the window's own ImGui
    // focus/hover is askable — and read by the 3D overlay passes through
    // KiwiUvEd_InScope / KiwiUvEd_OverlaySuppressed.  False whenever the window is
    // closed, collapsed or clipped away, so a stale gather can never suppress anything.
    bool                   s_inScope   = false;

    // ── the gesture-start snapshot (D-BD-B point 2: every live frame rebuilds from
    //    HERE, never from the previous frame's output) ────────────────────────────────
    struct uvSnapFace_t
    {
        brush_t      *def;
        int           faceIndex;
        texdef_sub_t  td;
    };
    struct uvSnapPatch_t
    {
        patchMesh_t *pm;
        float        st[16][16][2];
    };
    std::vector<uvSnapFace_t>  s_snapFaces;
    std::vector<uvSnapPatch_t> s_snapPatches;

    // ── view state ─────────────────────────────────────────────────────────────────
    float    s_panU = 0.0f, s_panV = 0.0f;   // UV coord at the canvas top-left
    float    s_zoom = 128.0f;                // canvas pixels per texture repeat
    bool     s_zoomValid = false;            // TB UvViewHelper::m_zoomValid (:79-83)
    float    s_originU = 0.0f, s_originV = 0.0f;
    int      s_subX = 1, s_subY = 1;         // TB subDivisions, range 1..16
    unsigned s_selGen = 0;                   // Sel_Generation() the view was framed for
    bool     s_haveGen = false;

    // ── gesture state ──────────────────────────────────────────────────────────────
    // KIWI-UX (ROUND BI): UVG_OFFSET is UVG_MOVE (it is a body drag on the shapes now, not
    // the ladder's fallback), and UVG_MARQUEE is the rubber-band that replaced that
    // fallback (D-BI-C).  UVG_SHEAR keeps its name because its arithmetic is unchanged.
    enum uvGesture_t { UVG_NONE = 0, UVG_ROTATE, UVG_ORIGIN, UVG_SCALE, UVG_SHEAR,
                       UVG_MOVE, UVG_MARQUEE, UVG_PAN };

    // KIWI-UX (ROUND BI): the eight handles of the transform box (D-BI-B).  The order is
    // load-bearing — 0..3 are the CORNERS (which also carry the rotate annulus) and 4..7
    // are the EDGE MIDPOINTS (which also carry the Alt-skew), so `h < 4` is "is a corner".
    enum uvHandle_t { UVH_NONE = -1,
                      UVH_NW = 0, UVH_NE, UVH_SE, UVH_SW,
                      UVH_N,      UVH_E,  UVH_S,  UVH_W };

    uvGesture_t s_gesture   = UVG_NONE;
    int         s_gButton   = -1;            // which ImGui mouse button owns it
    bool        s_undoOpen  = false;
    bool        s_covered   = false;
    float       s_gStartU = 0.0f, s_gStartV = 0.0f;   // cursor UV at press
    float       s_gCurU   = 0.0f, s_gCurV   = 0.0f;   // ...and at the latest update
    float       s_gHandleU = 0.0f, s_gHandleV = 0.0f; // the grabbed handle, in UV
    bool        s_gAxis[2] = { false, false };        // which axes the handle armed
    int         s_gHandle  = UVH_NONE;                // which handle (ROUND BI)
    bool        s_gOnShape = false;                   // the press landed on a shape body
    bool        s_gShift   = false;                   // Shift AS OF THE PRESS: a marquee's
                                                      // "extend" is decided when the gesture
                                                      // starts, not by whether the key is
                                                      // still down when the mouse comes up
    float       s_gOriginU = 0.0f, s_gOriginV = 0.0f; // origin at press (origin drag)
    // ── KIWI-UX (ROUND BJ, ITEM 5): the SCALE ANCHOR (D-BJ-E) ─────────────────────
    // The point a scale holds FIXED.  Round BI scaled about the pivot, so grabbing the
    // right edge moved the left edge out by the same amount; the standard 2D-editor
    // rule is that the OPPOSITE side (or corner) is nailed down and only the dragged
    // side moves.  Set at the press from the DEFLATED box, alongside s_gHandle*; the
    // pivot keeps rotate, skew, Rot 90 / Flip and the numeric ops.
    float       s_gAnchorU = 0.0f, s_gAnchorV = 0.0f;
    float       s_gStartAngle = 0.0f;                 // TB computeInitialAngle grab offset
    char        s_status[192] = { 0 };

    // ── KIWI-UX (ROUND BI): the pivot's "user placed" latch (D-BI-D) ───────────────
    // While this is false the pivot is re-derived from the target set's displayed bbox
    // centre every idle frame.  A pivot DRAG that actually moved it latches it true, and
    // the latch clears on the selection change that re-frames the view.
    bool        s_originUser = false;

    // ── KIWI-UX (ROUND BI): the stacked-shape click cycle (D-BI-C) ─────────────────
    float       s_cycleU = 0.0f, s_cycleV = 0.0f;
    int         s_cycleNext  = 0;
    bool        s_cycleValid = false;

    // ── KIWI-UX (ROUND BO, ITEM 2): THE DEFERRED COLLAPSE ─────────────────────────
    // Armed by a press on a shape that is ALREADY one of several targets, consumed by
    // GestureEnd.  A press there can mean two different things and the difference is
    // only knowable at the release: DRAG = move the whole target set (which is the
    // re-reported bug), CLICK = collapse the set to the pressed shape.  So the press
    // commits to neither, and this carries the click's answer across the gesture.
    // Cleared by every gesture start and by every teardown, so it can never fire for
    // a gesture it was not armed by.
    bool        s_collapseArmed = false;
    int         s_collapseFace  = -1;
    int         s_collapsePatch = -1;
    int         s_collapseNext  = 0;    // the cycle index the collapse hands on

    // ── KIWI-UX (ROUND BI): the gesture-start snap candidate sets (D-BI-F) ─────────
    struct uvPt_t   { float u, v; };
    struct uvEdge_t { float u0, v0, u1, v1; };
    std::vector<uvPt_t>   s_gTgtPts;     // the DRAGGED shapes' displayed outline points
    std::vector<uvPt_t>   s_gOtherPts;   // ...and everything else's, as snap targets
    std::vector<uvEdge_t> s_gTgtEdges;   // the dragged outlines' edges (angle / slope snaps)

    // ── the canvas rect for this frame (screen pixels) ─────────────────────────────
    ImVec2 s_c0( 0.0f, 0.0f );
    ImVec2 s_cs( 0.0f, 0.0f );

    // ═══════════════════════════════════════════════════════════════════════════════
    //  small helpers
    // ═══════════════════════════════════════════════════════════════════════════════
    float ClampF( float v, float lo, float hi ) { return v < lo ? lo : ( v > hi ? hi : v ); }
    int   ClampI( int v, int lo, int hi )       { return v < lo ? lo : ( v > hi ? hi : v ); }

    // random_texture_stuff is [3] (qe3.h:939) while current_edit_layer runs 0..3, so the
    // template read is clamped to the array — the same guard texwnd.cpp's own users need.
    int TemplateLayer()
    {
        return ClampI( g_qeglobals.current_edit_layer, 0, 2 );
    }

    bool MtlDefUsable( const MaterialDef *md )
    {
        // The MtlDef_IsValid invariant (materialdef.cpp:53) is an L0 assert inside both
        // MaterialDef_GetLayeredMaterial and Materialdef_GetName.  This window runs every
        // frame over whatever is selected, so it TESTS the invariant instead of tripping
        // it — the same reasoning kiwi_uv.cpp:185-189 states for the readout.
        return md && ( ( md->lyrMtl != 0 ) + ( md->radMtl != 0 ) ) == 1;
    }

    // The patch's per-layer material pair, as a MaterialDef.  `&p->texture + layer` is
    // patchMesh_material pointer arithmetic (8 bytes/layer → texture / lightmap /
    // smoothing) and MaterialDef's first two fields are the same pair — the expression is
    // copied from Patch_RotateTexture (pmesh.cpp:3259).
    MaterialDef *PatchMtlDef( patchMesh_t *p )
    {
        if ( !p )
            return nullptr;
        return (MaterialDef *)( &p->texture + ClampI( g_qeglobals.current_edit_layer, 0, 2 ) );
    }

    void MaterialSize( MaterialDef *md, float *w, float *h )
    {
        *w = 512.0f;
        *h = 512.0f;
        if ( !MtlDefUsable( md ) )
            return;
        qtexture_s *q = MaterialDef_GetLayeredMaterial( md );
        if ( !q )
            return;
        if ( q->width  > 0 ) *w = (float)q->width;
        if ( q->height > 0 ) *h = (float)q->height;
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  GATHER — every selected face and every selected patch, once per frame
    // ═══════════════════════════════════════════════════════════════════════════════
    // Both sources are walked LIVE (the sentinel lists and the CArray), so nothing here
    // is a stored pointer and Sel_BrushLive is not the right tool: a node reached by
    // walking `selected_brushes` is live by construction.  The only stored pointers this
    // file keeps are the gesture SNAPSHOT keys, and those are re-matched against a fresh
    // gather every frame (SnapshotStillMatches) rather than dereferenced blind.
    int FindBrushRow( selbrush_t *node, brush_t *def )
    {
        for ( size_t i = 0; i < s_brushes.size(); ++i )
            if ( s_brushes[i].def == def && s_brushes[i].node == node )
                return (int)i;
        return -1;
    }

    bool HaveFace( brush_t *def, int faceIndex )
    {
        for ( size_t i = 0; i < s_faces.size(); ++i )
            if ( s_brushes[s_faces[i].brushIdx].def == def && s_faces[i].faceIndex == faceIndex )
                return true;
        return false;
    }

    void StoreTargetKeys();       // ROUND BJ, ITEM 2 — defined below, used by Gather
    void TargetAll();             // ROUND BN, ITEM 4 — likewise (the re-derivation)
    void TargetSingle( int face, int patch );   // ROUND BO, ITEM 2 — the deferred
                                                // collapse, fired from GestureEnd

    void Gather()
    {
        // ── KIWI-UX (ROUND BJ, ITEM 2): the selection-change stamp ─────────────────
        // Read against `s_selGen`, which DrawCanvas updates LATER in this same frame,
        // so a 3D selection change is seen by the gather that first observes it and the
        // re-derivation below lands on the frame it belongs to.  Every other frame
        // leaves the stamp set and the user's canvas-local target set alone.
        if ( !s_haveGen || Sel_Generation() != s_selGen )
            s_tgtSeeded = false;

        s_brushes.clear();
        s_faces.clear();
        s_patches.clear();
        s_activeFace = -1;

        // 1. whole-selected brushes.  A PATCH brush contributes its patch and NOT its box
        //    faces — D-BD-F.
        for ( selbrush_t *b = selected_brushes.next;
              b && b != &selected_brushes;
              b = b->next )
        {
            brush_t *def = b->def;
            if ( !def )
                continue;
            uvBrush_t row;
            row.node = b;
            row.def  = def;
            row.pm   = ( b->patch && def->patch ) ? def->patch : nullptr;
            const int idx = (int)s_brushes.size();
            s_brushes.push_back( row );

            if ( row.pm )
            {
                if ( (int)s_patches.size() < KUVE_MAX_PATCHES )
                    s_patches.push_back( idx );
                continue;
            }
            if ( !def->faces )
                continue;
            for ( int fi = 0; fi < def->faceCount; ++fi )
            {
                if ( (int)s_faces.size() >= KUVE_MAX_FACES )
                    break;
                uvFace_t f;
                f.brushIdx  = idx;
                f.faceIndex = fi;
                s_faces.push_back( f );
            }
        }

        // 2. the face selection.  Face-selected brushes are NOT on `selected_brushes`
        //    (kiwi_selection.h DESIGN NOTE 2), so this cannot duplicate the pass above —
        //    the HaveFace guard is belt for a selection state that grew a third writer.
        const int nf = g_SelectedFaces.GetSize();
        for ( int i = 0; i < nf; ++i )
        {
            selface_t  &sf   = g_SelectedFaces.GetAt( i );
            selbrush_t *node = sf.brush;
            brush_t    *def  = node ? node->def : nullptr;
            if ( !def || !def->faces || node->patch )
                continue;
            if ( sf.index < 0 || sf.index >= def->faceCount )
                continue;
            if ( HaveFace( def, sf.index ) )
                continue;
            if ( (int)s_faces.size() >= KUVE_MAX_FACES )
                break;
            int idx = FindBrushRow( node, def );
            if ( idx < 0 )
            {
                uvBrush_t row;
                row.node = node;
                row.def  = def;
                row.pm   = nullptr;
                idx = (int)s_brushes.size();
                s_brushes.push_back( row );
            }
            uvFace_t f;
            f.brushIdx  = idx;
            f.faceIndex = sf.index;
            s_faces.push_back( f );
        }

        // 3. THE ACTIVE FACE — the one whose material paints the background and whose
        //    vertices/edges every snap reads.  KiwiSel().active when it names a face we
        //    gathered, else the first gathered face.
        const sel_item_t &act = KiwiSel().active;
        if ( act.kind == SEL_FACE && act.brush && act.brush->def )
        {
            for ( size_t i = 0; i < s_faces.size(); ++i )
            {
                if ( s_brushes[s_faces[i].brushIdx].def == act.brush->def
                     && s_faces[i].faceIndex == act.faceIndex )
                {
                    s_activeFace = (int)i;
                    break;
                }
            }
        }
        if ( s_activeFace < 0 && !s_faces.empty() )
            s_activeFace = 0;

        // ── KIWI-UX (ROUND BG): the two parallel arrays the rest of the round reads.
        //    Both are rebuilt from scratch here, every frame, for the same reason the
        //    gather itself is: nothing in this file may hold an index across a frame.
        s_faceD.assign( s_faces.size(), XfIdentity() );
        s_patchD.assign( s_patches.size(), XfIdentity() );
        s_faceTgt.assign( s_faces.size(), 1 );
        s_patchTgt.assign( s_patches.size(), 1 );
        if ( !s_targetAll )
        {
            // Re-match the stored keys against THIS gather.  A key that no longer names a
            // gathered row simply drops out; if nothing matches at all the sub-selection
            // has become meaningless and we fall back to "all" rather than to "nothing"
            // (a canvas where no gesture does anything is a bug report waiting to happen).
            int hits = 0;
            for ( size_t i = 0; i < s_faces.size(); ++i )
            {
                brush_t *def = s_brushes[s_faces[i].brushIdx].def;
                char     on  = 0;
                for ( size_t k = 0; k < s_targetKeys.size(); ++k )
                    if ( !s_targetKeys[k].pm && s_targetKeys[k].def == def
                         && s_targetKeys[k].faceIndex == s_faces[i].faceIndex )
                    {
                        on = 1;
                        break;
                    }
                s_faceTgt[i] = on;
                hits += on;
            }
            for ( size_t i = 0; i < s_patches.size(); ++i )
            {
                patchMesh_t *pm = s_brushes[s_patches[i]].pm;
                char         on = 0;
                for ( size_t k = 0; k < s_targetKeys.size(); ++k )
                    if ( s_targetKeys[k].pm && s_targetKeys[k].pm == pm )
                    {
                        on = 1;
                        break;
                    }
                s_patchTgt[i] = on;
                hits += on;
            }
            if ( !hits )
            {
                s_targetAll = true;
                s_targetKeys.clear();
                s_faceTgt.assign( s_faces.size(), 1 );
                s_patchTgt.assign( s_patches.size(), 1 );
            }
        }

        // ── KIWI-UX (ROUND BJ, ITEM 2): THE TARGET SET STARTS AS THE 3D FACE PICK ──
        // USER REPORT, verbatim: *"When faces are shift-selected in 3D, they also need
        // to be shift-selected in the UV editor by default."*
        //
        // Round BI's default was "everything gathered is a target".  For a PURE face
        // selection that already answers the report — the gathered rows ARE the picked
        // faces, so all of them are targeted, and ITEM 3 is what makes that VISIBLE
        // (they all draw gold now instead of one of them doing so).  Where it was wrong
        // is a MIXED selection: whole brushes contribute all their faces, so the faces
        // the user actually shift-picked were buried among them and the first canvas
        // gesture moved rows that were never picked in 3D.
        //
        // So: when the 3D selection carries EXPLICIT faces (g_SelectedFaces), those
        // faces — and only those — are the initial target set.  A whole-brush-only
        // selection keeps "all", unchanged.  The stamp above is what makes this a
        // DEFAULT and not a per-frame override.
        // ── KIWI-UX (ROUND BN, ITEM 4): …AND THE RE-DERIVATION IS UNCONDITIONAL ──
        // USER RE-REPORT, verbatim: *"When selecting multiple faces, it still needs
        // to select multiple in the UV editor.  I shouldn't have to shift-click them
        // in the UV editor to move them all at once after having shift-clicked them
        // in 3D."*
        //
        // THE DEFECT IS THE `hits < nShapes` GUARD BELOW, and the stamp is innocent.
        // Round BJ declined to seed when the explicit 3D faces cover EVERY gathered
        // shape, reasoning "leave the state as plain `all` rather than as a
        // sub-selection that happens to cover everything".  That reasoning silently
        // assumes the state IS "all" — and after ANY canvas press it is not: a press
        // on a shape runs TargetSingle (:2890, `s_targetAll = false`) and stores its
        // keys, and the RE-MATCH block immediately above (:572-612) has just
        // re-applied those stale keys to the NEW gather.  Nothing between the stamp
        // (:471, which only clears `s_tgtSeeded`) and here ever clears `s_targetAll`.
        //
        // So the user's actual flow — shift-click faces in 3D, drag one in the
        // canvas (which is what makes the canvas useful at all), shift-click a
        // fourth face in 3D — re-gathered four faces, re-matched the ONE canvas key,
        // hit the `4 < 4` guard, declined, and drew one gold shape.  Every later 3D
        // shift-click was ignored for the rest of the session, which is exactly
        // "I shouldn't have to shift-click them in the UV editor".
        //
        // THE RULE, stated once and implemented below: ANY change to the 3D
        // selection re-derives the target set from `g_SelectedFaces` IMMEDIATELY —
        // add, remove or replace.  A canvas-local sub-selection survives only
        // UNRELATED re-gathers, i.e. frames in which `Sel_Generation()` did not
        // move.  The three arms are the three answers, and the middle one is round
        // BJ's, unchanged:
        //     explicit faces cover EVERY gathered shape  -> plain "all" (TargetAll,
        //         stated rather than assumed — this is the arm that was missing);
        //     explicit faces cover SOME of them          -> that sub-selection;
        //     no explicit faces at all (whole brushes)   -> plain "all", because the
        //         old keys belong to a selection the user has just replaced.
        if ( !s_tgtSeeded )
        {
            s_tgtSeeded = true;
            const int nExplicit = g_SelectedFaces.GetSize();
            const int nShapes   = (int)( s_faces.size() + s_patches.size() );
            if ( nExplicit <= 0 && nShapes > 0 )
            {
                // A whole-brush-only selection: "keeps all" was always the intent
                // (round BJ), and TargetAll is what actually makes it true.
                TargetAll();
            }
            else if ( nExplicit > 0 && nShapes > 0 )
            {
                std::vector<char> ft( s_faces.size(), 0 );
                int hits = 0;
                for ( size_t i = 0; i < s_faces.size(); ++i )
                {
                    brush_t *def = s_brushes[s_faces[i].brushIdx].def;
                    for ( int k = 0; k < nExplicit; ++k )
                    {
                        const selface_t &sf = g_SelectedFaces.GetAt( k );
                        if ( sf.brush && sf.brush->def == def
                             && sf.index == s_faces[i].faceIndex )
                        {
                            ft[i] = 1;
                            ++hits;
                            break;
                        }
                    }
                }
                // `hits == nShapes` means the explicit faces ARE everything on the
                // canvas: the state becomes plain "all" rather than a sub-selection
                // that happens to cover everything (Probe's empty-space arm reads
                // s_targetAll, D-BI-C, and a redundant sub-selection would change it).
                // KIWI-UX (ROUND BN, ITEM 4): SET, not assumed — see above.
                if ( hits > 0 && hits < nShapes )
                {
                    s_faceTgt = ft;
                    s_patchTgt.assign( s_patches.size(), 0 );
                    s_targetAll = false;
                    StoreTargetKeys();
                }
                else if ( hits > 0 )
                {
                    TargetAll();
                }
            }
        }
    }

    bool AnythingSelected()
    {
        return !s_faces.empty() || !s_patches.empty();
    }

    // ── KIWI-UX (ROUND BG), D-BG-B ────────────────────────────────────────────────
    bool FaceIsTarget ( size_t i ) { return i < s_faceTgt.size()  && s_faceTgt[i]  != 0; }
    bool PatchIsTarget( size_t i ) { return i < s_patchTgt.size() && s_patchTgt[i] != 0; }

    int TargetCount()
    {
        int n = 0;
        for ( size_t i = 0; i < s_faceTgt.size();  ++i ) n += ( s_faceTgt[i]  != 0 );
        for ( size_t i = 0; i < s_patchTgt.size(); ++i ) n += ( s_patchTgt[i] != 0 );
        return n;
    }

    // Freeze the live s_faceTgt / s_patchTgt into the durable key list.  Called by every
    // path that CHANGES the target set, never by the gather (which only reads the keys).
    void StoreTargetKeys()
    {
        s_targetKeys.clear();
        for ( size_t i = 0; i < s_faces.size(); ++i )
            if ( s_faceTgt[i] )
            {
                uvTgtKey_t k;
                k.def       = s_brushes[s_faces[i].brushIdx].def;
                k.faceIndex = s_faces[i].faceIndex;
                k.pm        = nullptr;
                s_targetKeys.push_back( k );
            }
        for ( size_t i = 0; i < s_patches.size(); ++i )
            if ( s_patchTgt[i] )
            {
                uvTgtKey_t k;
                k.def       = nullptr;
                k.faceIndex = -1;
                k.pm        = s_brushes[s_patches[i]].pm;
                s_targetKeys.push_back( k );
            }
    }

    void TargetAll()
    {
        s_targetAll = true;
        s_targetKeys.clear();
        s_faceTgt.assign( s_faces.size(), 1 );
        s_patchTgt.assign( s_patches.size(), 1 );
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  PER-FACE UV READ (the SURVEY_KIWI recipe, verbatim)
    // ═══════════════════════════════════════════════════════════════════════════════
    //   td = &f->mtldef[L].mat_texDef + LayerMat::GetCurrentLayer(...)   [KiwiUv_FaceTexdef]
    //   Face_MoveTexture( td, f->plane.normal, texMat, &td->shift[0], td->rotate, td->crossterm )
    //   s = dot(row0, p) + texMat[3];  t = dot(row1, p) + texMat[7]      [brush.cpp:2618-2619]
    face_t *FaceOf( const uvFace_t &f )
    {
        brush_t *def = s_brushes[f.brushIdx].def;
        if ( !def || !def->faces || f.faceIndex < 0 || f.faceIndex >= def->faceCount )
            return nullptr;
        return &def->faces[f.faceIndex];
    }

    void BuildTexMat( const texdef_sub_t *td, const float *normal, float *outMat8 )
    {
        Face_MoveTexture( (int)(intptr_t)td, normal, (int)(intptr_t)outMat8,
                          (int)(intptr_t)&td->shift[0], td->rotate, td->crossterm );
    }

    void StFromMat( const float *m, const float *p, float *s, float *t )
    {
        *s = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
        *t = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    }

    // The face's winding in ST.  Returns the point count (0 when the face has no winding
    // or no usable texdef on the current layer).  `tdOverride` non-null reads the winding
    // through a DIFFERENT texdef than the face is currently carrying — that is how a live
    // gesture measures its snaps against the GESTURE-START projection instead of against
    // the frame it is in the middle of writing.
    int FaceStPoints( const uvFace_t &f, float out[KUVE_MAX_WINDING][2],
                      const texdef_sub_t *tdOverride )
    {
        face_t *fd = FaceOf( f );
        if ( !fd || !fd->w )
            return 0;
        MaterialDef  *md = nullptr;
        texdef_sub_t *td = KiwiUv_FaceTexdef( s_brushes[f.brushIdx].def, f.faceIndex, &md );
        if ( !td )
            return 0;
        const texdef_sub_t *use = tdOverride ? tdOverride : td;

        float mat[8];
        BuildTexMat( use, fd->plane.normal, mat );

        int n = fd->w->numpoints;
        if ( n < 1 )
            return 0;
        if ( n > KUVE_MAX_WINDING )
            n = KUVE_MAX_WINDING;
        for ( int i = 0; i < n; ++i )
            StFromMat( mat, fd->w->p[i], &out[i][0], &out[i][1] );
        return n;
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BG) — "PAPER ANGELS": THE DISPLAY TRANSFORMS (D-BG-A)
    // ═══════════════════════════════════════════════════════════════════════════════
    // Everything in this block is DISPLAY ONLY.  Not one line of it may reach a texdef
    // or a control ST; the single point where D crosses into the write path is
    // XfConjugate, at apply time, and that is the whole design.

    // The face winding in DISPLAYED ST — i.e. through D_f.  Same signature as
    // FaceStPoints so the drawing / hit-testing sites read the same shape.
    int FaceStPointsDisp( size_t fi, float out[KUVE_MAX_WINDING][2],
                          const texdef_sub_t *tdOverride )
    {
        const int n = FaceStPoints( s_faces[fi], out, tdOverride );
        if ( fi < s_faceD.size() && !XfIsIdentity( s_faceD[fi] ) )
            for ( int i = 0; i < n; ++i )
                XfApply( s_faceD[fi], out[i][0], out[i][1], &out[i][0], &out[i][1] );
        return n;
    }

    void PatchStDisp( size_t pi, int i, int j, float *s, float *t )
    {
        patchMesh_t *p = s_brushes[s_patches[pi]].pm;
        const int layer = ClampI( g_qeglobals.current_edit_layer, 0, 2 );
        float u = p->ctrl[i][j].texCoord.st[2 * layer];
        float v = p->ctrl[i][j].texCoord.st[2 * layer + 1];
        if ( pi < s_patchD.size() && !XfIsIdentity( s_patchD[pi] ) )
            XfApply( s_patchD[pi], u, v, &u, &v );
        *s = u;
        *t = v;
    }

    // The patch's DISPLAYED outline: the control net's outer ring, walked once round.
    // (A Bezier patch lies inside its control hull, and the ring is the cheap honest
    // boundary — the same thing DrawWireframes already draws the user.)  Returns the
    // point count; 0 for a degenerate patch.
    int PatchRing( size_t pi, float ring[KUVE_MAX_WINDING][2] )
    {
        patchMesh_t *p = s_brushes[s_patches[pi]].pm;
        if ( !p )
            return 0;
        const int w = ClampI( p->width, 0, 16 ), h = ClampI( p->height, 0, 16 );
        if ( w < 2 || h < 2 )
            return 0;
        int n = 0;
        for ( int i = 0;     i < w  && n < KUVE_MAX_WINDING; ++i )
            { PatchStDisp( pi, i,     0,     &ring[n][0], &ring[n][1] ); ++n; }
        for ( int j = 1;     j < h  && n < KUVE_MAX_WINDING; ++j )
            { PatchStDisp( pi, w - 1, j,     &ring[n][0], &ring[n][1] ); ++n; }
        for ( int i = w - 2; i >= 0 && n < KUVE_MAX_WINDING; --i )
            { PatchStDisp( pi, i,     h - 1, &ring[n][0], &ring[n][1] ); ++n; }
        for ( int j = h - 2; j >= 1 && n < KUVE_MAX_WINDING; --j )
            { PatchStDisp( pi, 0,     j,     &ring[n][0], &ring[n][1] ); ++n; }
        return n;
    }

    // ── the rebuild trigger ────────────────────────────────────────────────────────
    // KIWI-UX (ROUND BJ, ITEM 7): this is now the WHOLE trigger.  Round BG also hashed
    // every gathered texdef (ChainSignature / HashF, DELETED with this round — see the
    // note in DrawCanvas), which made the fold-out re-solve itself from the new STs the
    // moment any gesture released and visibly reshuffled the arrangement.  A display
    // layout is not a function of the texdefs, so it is not keyed on them.
    unsigned HashMix( unsigned h, unsigned v )
    {
        h ^= v;
        h *= 16777619u;            // FNV-1a
        return h;
    }

    // Which rows were gathered, in which order, which one is the fold ANCHOR, and
    // whether the chain is on at all.  While THIS is unchanged the cached D array is
    // index-parallel to the live one and is replayed onto it every frame.
    unsigned ChainStructSig()
    {
        unsigned h = 2166136261u;
        h = HashMix( h, (unsigned)s_faces.size() );
        h = HashMix( h, (unsigned)s_patches.size() );
        h = HashMix( h, (unsigned)( s_activeFace + 1 ) );
        h = HashMix( h, s_chain ? 1u : 0u );
        for ( size_t i = 0; i < s_faces.size(); ++i )
        {
            h = HashMix( h, (unsigned)(uintptr_t)s_brushes[s_faces[i].brushIdx].def );
            h = HashMix( h, (unsigned)s_faces[i].faceIndex );
        }
        for ( size_t i = 0; i < s_patches.size(); ++i )
            h = HashMix( h, (unsigned)(uintptr_t)s_brushes[s_patches[i]].pm );
        return h;
    }

    // ── the build ──────────────────────────────────────────────────────────────────
    const int KUVE_CHAIN_MAX_PTS = 32;      // per-face winding cap for the fold only

    struct chainFace_t
    {
        int   n;
        float st[KUVE_CHAIN_MAX_PTS][2];    // the face's OWN ST frame
        float wp[KUVE_CHAIN_MAX_PTS][3];    // the same points in world space
        float ctr[2];                       // ST centroid, own frame
    };
    struct chainVert_t { long long k[3]; int face; int idx; };
    struct chainEdge_t { int va, vb; int face; int i0, i1; };   // va<=vb weld ids
    struct chainAdj_t  { int fa, fb; int a0, a1, b0, b1; };     // matching vertex indices

    std::vector<chainFace_t> s_cf;
    std::vector<chainVert_t> s_cv;
    std::vector<chainEdge_t> s_ce;
    std::vector<chainAdj_t>  s_ca;

    long long WeldQ( float v )
    {
        return (long long)floorf( v / KUVE_WELD_EPS + 0.5f );
    }

    bool VertLess( const chainVert_t &a, const chainVert_t &b )
    {
        if ( a.k[0] != b.k[0] ) return a.k[0] < b.k[0];
        if ( a.k[1] != b.k[1] ) return a.k[1] < b.k[1];
        return a.k[2] < b.k[2];
    }
    bool VertSame( const chainVert_t &a, const chainVert_t &b )
    {
        return a.k[0] == b.k[0] && a.k[1] == b.k[1] && a.k[2] == b.k[2];
    }
    bool EdgeLess( const chainEdge_t &a, const chainEdge_t &b )
    {
        if ( a.va != b.va ) return a.va < b.va;
        return a.vb < b.vb;
    }

    // The rigid map placing neighbour edge (b0,b1) onto the anchor's DISPLAYED edge
    // (A0,A1): rotate dir(b1-b0) onto dir(A1-A0), then translate b0 onto A0.  Lengths
    // are NOT matched — a rigid transform cannot, and the two faces may carry different
    // texture scales (D-BG-A states the compromise).
    uvXform_t FoldRigid( const float *A0, const float *A1, const float *b0, const float *b1 )
    {
        const float ax = A1[0] - A0[0], ay = A1[1] - A0[1];
        const float bx = b1[0] - b0[0], by = b1[1] - b0[1];
        const float la = sqrtf( ax * ax + ay * ay );
        const float lb = sqrtf( bx * bx + by * by );
        if ( la < KUVE_EPS || lb < KUVE_EPS )
            return XfTranslate( A0[0] - b0[0], A0[1] - b0[1] );
        const float ux = ax / la, uy = ay / la;
        const float vx = bx / lb, vy = by / lb;
        // R takes v onto u: cos = v·u, sin = v×u.
        const float c = vx * ux + vy * uy;
        const float s = vx * uy - vy * ux;
        uvXform_t r;
        r.m[0] = c;  r.m[1] = -s;  r.m[2] = A0[0] - ( c * b0[0] - s * b0[1] );
        r.m[3] = s;  r.m[4] =  c;  r.m[5] = A0[1] - ( s * b0[0] + c * b0[1] );
        return r;
    }

    // Reflection across the line through P with direction (dx,dy) — the FOLD itself.
    uvXform_t FoldMirror( const float *P, float dx, float dy )
    {
        const float l = sqrtf( dx * dx + dy * dy );
        if ( l < KUVE_EPS )
            return XfIdentity();
        const float ux = dx / l, uy = dy / l;
        const float a = ux * ux - uy * uy;     // cos 2phi
        const float b = 2.0f * ux * uy;        // sin 2phi
        return XfAboutOrigin( a, b, b, -a, P[0], P[1] );
    }

    float SideOf( const float *A0, const float *A1, const float *p )
    {
        return ( A1[0] - A0[0] ) * ( p[1] - A0[1] ) - ( A1[1] - A0[1] ) * ( p[0] - A0[0] );
    }

    void BuildChain()
    {
        s_chainFolded  = 0;
        s_chainShelved = 0;
        if ( !s_chain )
            return;
        const int nf = (int)s_faces.size();
        if ( nf > KUVE_CHAIN_MAX_FACES )
            return;                                   // D-BG-A: too many to be a diagram

        // 1. gather the per-face geometry ONCE (world + own-frame ST + ST centroid).
        s_cf.assign( (size_t)( nf > 0 ? nf : 0 ), chainFace_t() );
        for ( int i = 0; i < nf; ++i )
        {
            chainFace_t &cf = s_cf[i];
            cf.n = 0;
            cf.ctr[0] = cf.ctr[1] = 0.0f;
            face_t *fd = FaceOf( s_faces[i] );
            if ( !fd || !fd->w )
                continue;
            float st[KUVE_MAX_WINDING][2];
            int   n = FaceStPoints( s_faces[i], st, nullptr );
            if ( n > KUVE_CHAIN_MAX_PTS )
                n = KUVE_CHAIN_MAX_PTS;
            cf.n = n;
            for ( int k = 0; k < n; ++k )
            {
                cf.st[k][0] = st[k][0];
                cf.st[k][1] = st[k][1];
                cf.wp[k][0] = fd->w->p[k][0];
                cf.wp[k][1] = fd->w->p[k][1];
                cf.wp[k][2] = fd->w->p[k][2];
                cf.ctr[0]  += st[k][0];
                cf.ctr[1]  += st[k][1];
            }
            if ( n > 0 )
            {
                cf.ctr[0] /= (float)n;
                cf.ctr[1] /= (float)n;
            }
        }

        // 2. weld the world vertices (quantised buckets) and build the edge list.
        s_cv.clear();
        for ( int i = 0; i < nf; ++i )
            for ( int k = 0; k < s_cf[i].n; ++k )
            {
                chainVert_t v;
                v.k[0] = WeldQ( s_cf[i].wp[k][0] );
                v.k[1] = WeldQ( s_cf[i].wp[k][1] );
                v.k[2] = WeldQ( s_cf[i].wp[k][2] );
                v.face = i;
                v.idx  = k;
                s_cv.push_back( v );
            }
        if ( s_cv.empty() )
            return;
        std::sort( s_cv.begin(), s_cv.end(), VertLess );

        // weldId[face][idx] — flattened, because the per-face point counts vary.
        std::vector<int> weldBase( (size_t)nf, 0 );
        int total = 0;
        for ( int i = 0; i < nf; ++i ) { weldBase[i] = total; total += s_cf[i].n; }
        std::vector<int> weldId( (size_t)( total > 0 ? total : 1 ), -1 );
        int nextId = 0;
        for ( size_t i = 0; i < s_cv.size(); )
        {
            size_t j = i;
            while ( j < s_cv.size() && VertSame( s_cv[j], s_cv[i] ) )
            {
                weldId[weldBase[s_cv[j].face] + s_cv[j].idx] = nextId;
                ++j;
            }
            ++nextId;
            i = j;
        }

        s_ce.clear();
        for ( int i = 0; i < nf && (int)s_ce.size() < KUVE_CHAIN_MAX_EDGES; ++i )
        {
            const int n = s_cf[i].n;
            for ( int k = 0; k < n; ++k )
            {
                const int k2 = ( k + 1 ) % n;
                chainEdge_t e;
                e.face = i;
                e.i0   = k;
                e.i1   = k2;
                const int a = weldId[weldBase[i] + k];
                const int b = weldId[weldBase[i] + k2];
                if ( a < 0 || b < 0 || a == b )
                    continue;
                e.va = ( a < b ) ? a : b;
                e.vb = ( a < b ) ? b : a;
                s_ce.push_back( e );
            }
        }
        std::sort( s_ce.begin(), s_ce.end(), EdgeLess );

        // 3. adjacency: equal (va,vb) runs, every cross-face pair inside one.
        s_ca.clear();
        for ( size_t i = 0; i < s_ce.size(); )
        {
            size_t j = i;
            while ( j < s_ce.size() && s_ce[j].va == s_ce[i].va && s_ce[j].vb == s_ce[i].vb )
                ++j;
            for ( size_t a = i; a < j; ++a )
                for ( size_t b = a + 1; b < j; ++b )
                {
                    if ( s_ce[a].face == s_ce[b].face )
                        continue;
                    chainAdj_t ad;
                    ad.fa = s_ce[a].face;
                    ad.fb = s_ce[b].face;
                    ad.a0 = s_ce[a].i0;
                    ad.a1 = s_ce[a].i1;
                    // Match the ENDPOINTS, not just the edge: b's i0 may be a's i1.
                    const int wa0 = weldId[weldBase[s_ce[a].face] + s_ce[a].i0];
                    const int wb0 = weldId[weldBase[s_ce[b].face] + s_ce[b].i0];
                    if ( wb0 == wa0 ) { ad.b0 = s_ce[b].i0; ad.b1 = s_ce[b].i1; }
                    else              { ad.b0 = s_ce[b].i1; ad.b1 = s_ce[b].i0; }
                    s_ca.push_back( ad );
                }
            i = j;
        }

        // 4. BFS from the active face; D stays identity there, always.
        std::vector<char> placed( (size_t)nf, 0 );
        std::vector<int>  queue;
        const int anchor = ( s_activeFace >= 0 && s_activeFace < nf ) ? s_activeFace : 0;
        if ( nf < 1 )
            return;
        placed[anchor]   = 1;
        s_faceD[anchor]  = XfIdentity();
        queue.push_back( anchor );
        s_chainFolded = 1;

        for ( size_t qi = 0; qi < queue.size(); ++qi )
        {
            const int a = queue[qi];
            for ( size_t r = 0; r < s_ca.size(); ++r )
            {
                const chainAdj_t &ad = s_ca[r];
                int fa = ad.fa, fb = ad.fb, a0 = ad.a0, a1 = ad.a1, b0 = ad.b0, b1 = ad.b1;
                if ( fb == a && !placed[fa] ) { fb = fa; fa = a;
                                                const int t0 = a0, t1 = a1;
                                                a0 = b0; a1 = b1; b0 = t0; b1 = t1; }
                if ( fa != a || placed[fb] )
                    continue;
                if ( s_cf[fa].n < 3 || s_cf[fb].n < 3 )
                    continue;

                float A0[2], A1[2];
                XfApply( s_faceD[fa], s_cf[fa].st[a0][0], s_cf[fa].st[a0][1], &A0[0], &A0[1] );
                XfApply( s_faceD[fa], s_cf[fa].st[a1][0], s_cf[fa].st[a1][1], &A1[0], &A1[1] );

                uvXform_t D = FoldRigid( A0, A1, s_cf[fb].st[b0], s_cf[fb].st[b1] );

                // Which side is the anchor's body on, and which side did the neighbour
                // land on?  Same side ⇒ it overlaps the anchor ⇒ fold it across.
                float ca[2], cb[2];
                XfApply( s_faceD[fa], s_cf[fa].ctr[0], s_cf[fa].ctr[1], &ca[0], &ca[1] );
                XfApply( D,           s_cf[fb].ctr[0], s_cf[fb].ctr[1], &cb[0], &cb[1] );
                const float sa = SideOf( A0, A1, ca );
                const float sb = SideOf( A0, A1, cb );
                if ( ( sa > 0.0f ) == ( sb > 0.0f ) )
                    D = XfMul( FoldMirror( A0, A1[0] - A0[0], A1[1] - A0[1] ), D );

                s_faceD[fb] = D;
                placed[fb]  = 1;
                ++s_chainFolded;
                queue.push_back( fb );
            }
        }

        // 5. the shelf: everything the fold could not reach goes in a row to the right of
        //    the occupied box.  Trivial shelf packing — D-BG-A says so and means it.
        float occ[4] = { 1.0e30f, 1.0e30f, -1.0e30f, -1.0e30f };
        bool  haveOcc = false;
        for ( int i = 0; i < nf; ++i )
        {
            if ( !placed[i] )
                continue;
            for ( int k = 0; k < s_cf[i].n; ++k )
            {
                float u, v;
                XfApply( s_faceD[i], s_cf[i].st[k][0], s_cf[i].st[k][1], &u, &v );
                if ( u < occ[0] ) occ[0] = u;
                if ( v < occ[1] ) occ[1] = v;
                if ( u > occ[2] ) occ[2] = u;
                if ( v > occ[3] ) occ[3] = v;
                haveOcc = true;
            }
        }
        if ( !haveOcc )
        {
            occ[0] = occ[1] = 0.0f;
            occ[2] = occ[3] = 1.0f;
        }
        float shelfX = occ[2] + KUVE_SHELF_GAP;

        for ( int i = 0; i < nf; ++i )
        {
            if ( placed[i] || s_cf[i].n < 2 )
                continue;
            float mn[2] = { 1.0e30f, 1.0e30f }, mx[2] = { -1.0e30f, -1.0e30f };
            for ( int k = 0; k < s_cf[i].n; ++k )
                for ( int c = 0; c < 2; ++c )
                {
                    if ( s_cf[i].st[k][c] < mn[c] ) mn[c] = s_cf[i].st[k][c];
                    if ( s_cf[i].st[k][c] > mx[c] ) mx[c] = s_cf[i].st[k][c];
                }
            s_faceD[i] = XfTranslate( shelfX - mn[0], occ[1] - mn[1] );
            shelfX    += ( mx[0] - mn[0] ) + KUVE_SHELF_GAP;
            ++s_chainShelved;
        }

        // 6. patches: their ST footprints rarely stack, so they are left alone UNLESS one
        //    sits almost entirely inside the occupied box (>80% of its own area), which is
        //    the case the user cannot read.  Those get the same shelf.
        const int layer = ClampI( g_qeglobals.current_edit_layer, 0, 2 );
        for ( size_t pi = 0; pi < s_patches.size(); ++pi )
        {
            patchMesh_t *p = s_brushes[s_patches[pi]].pm;
            if ( !p )
                continue;
            const int w = ClampI( p->width, 0, 16 ), h = ClampI( p->height, 0, 16 );
            if ( w < 1 || h < 1 )
                continue;
            float mn[2] = { 1.0e30f, 1.0e30f }, mx[2] = { -1.0e30f, -1.0e30f };
            for ( int i = 0; i < w; ++i )
                for ( int j = 0; j < h; ++j )
                {
                    const float st[2] = { p->ctrl[i][j].texCoord.st[2 * layer],
                                          p->ctrl[i][j].texCoord.st[2 * layer + 1] };
                    for ( int c = 0; c < 2; ++c )
                    {
                        if ( st[c] < mn[c] ) mn[c] = st[c];
                        if ( st[c] > mx[c] ) mx[c] = st[c];
                    }
                }
            const float aw = mx[0] - mn[0], ah = mx[1] - mn[1];
            if ( aw <= 0.0f || ah <= 0.0f )
                continue;
            const float ow = ( ( mx[0] < occ[2] ? mx[0] : occ[2] )
                             - ( mn[0] > occ[0] ? mn[0] : occ[0] ) );
            const float oh = ( ( mx[1] < occ[3] ? mx[1] : occ[3] )
                             - ( mn[1] > occ[1] ? mn[1] : occ[1] ) );
            const float cover = ( ow > 0.0f && oh > 0.0f ) ? ( ow * oh ) / ( aw * ah ) : 0.0f;
            if ( cover <= 0.8f )
                continue;
            s_patchD[pi] = XfTranslate( shelfX - mn[0], occ[1] - mn[1] );
            shelfX      += aw + KUVE_SHELF_GAP;
            ++s_chainShelved;
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  THE WRITE PATH
    // ═══════════════════════════════════════════════════════════════════════════════
    // The tail every ported texdef writer runs, per brush.  Verbatim the sequence
    // Brush_ShiftTexture's face pass uses (select.cpp:3158-3163): rebuild windings, keep
    // a vertex/edge selection consistent, mark the map, bump the def version, then
    // re-sync the instance faceVis (sub_477D70 is version-gated, so this is also what
    // keeps `selbrush->version == def->version` — the invariant select.cpp:2471 asserts).
    void TouchBrush( selbrush_t *node, brush_t *def )
    {
        if ( !def )
            return;
        Brush_BuildWindings( def, 1 );
        if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
            SetupVertexSelection();
        MarkMapModified();
        ++def->version;
        if ( node )
            sub_477D70( node, (const float *)world_orient_matrix );
    }

    // D-BD-B: matrix surgery on the GESTURE-START texdef, then the binary's own inverse.
    void TransformFace( face_t *fd, const texdef_sub_t *td0, const uvXform_t &A,
                        texdef_sub_t *out )
    {
        // A SINGULAR linear part collapses the two texture axes onto one line, and
        // texturevecs_02 then divides by a zero row length (Phase E, `1/|row0|`) and
        // poisons the texdef with an infinity that no later gesture can undo.  Skew is
        // the reachable case: its matrix is [[1,f1],[f0,1]], whose determinant is
        // 1 − f0·f1.  Refuse the frame instead — the caller re-derives from the snapshot
        // next frame anyway, so the gesture simply stops moving at the degenerate point.
        const float det = A.m[0] * A.m[4] - A.m[1] * A.m[3];
        if ( fabsf( det ) < 1.0e-6f )
        {
            *out = *td0;
            return;
        }

        float m0[8];
        BuildTexMat( td0, fd->plane.normal, m0 );

        float m[8];
        for ( int k = 0; k < 3; ++k )
        {
            m[k]     = A.m[0] * m0[k] + A.m[1] * m0[4 + k];
            m[4 + k] = A.m[3] * m0[k] + A.m[4] * m0[4 + k];
        }
        m[3] = A.m[0] * m0[3] + A.m[1] * m0[7] + A.m[2];
        m[7] = A.m[3] * m0[3] + A.m[4] * m0[7] + A.m[5];

        // texturevecs_02 CONSUMES `m` as scratch (it orthogonalises the rows in place —
        // a no-op for us, see D-BD-B) and writes the four texdef fields byref.  The arg
        // shape is copied from Face_TexLock_Reproject's own call (brush.cpp:2978-2980):
        // out+0 size, out+8 shift, out+16 rotate, out+20 crossterm.  The third argument
        // is the hex-rays x87 phantom and is ignored by the port.
        texturevecs_02( (int)(intptr_t)&out->size[0], (int)(intptr_t)m,
                        fd->plane.normal[2],
                        (int)(intptr_t)fd->plane.normal, fd->plane.dist,
                        (int)(intptr_t)&out->shift[0],
                        (int)(intptr_t)&out->rotate,
                        (int)(intptr_t)&out->crossterm );
    }

    // D-BD-C: the EXACT route for a pure translation.  m3 == -shift[0]/sx, so a UV
    // translation by d is shift = shift0 - d*size0, with the binary's own zero-size
    // substitution (texturevecs.cpp:108-109).
    void OffsetFace( const texdef_sub_t *td0, float dU, float dV, texdef_sub_t *out )
    {
        float sx = td0->size[0]; if ( sx == 0.0f ) sx = 128.0f;
        float sy = td0->size[1]; if ( sy == 0.0f ) sy = 128.0f;
        out->size[0]  = td0->size[0];
        out->size[1]  = td0->size[1];
        out->rotate   = td0->rotate;
        out->crossterm = td0->crossterm;
        out->shift[0] = td0->shift[0] - dU * sx;
        out->shift[1] = td0->shift[1] - dV * sy;
    }

    // Patch control STs.  `st[2*layer]` / `[2*layer+1]` is the SAME slot
    // Patch_ShiftTexture writes (pmesh.cpp:3232-3238); `texCoord` is st/lightmap/smoothing
    // contiguous (qedefs.h:159-165) so the stride-2 walk reaches all three channels.
    void TransformPatch( patchMesh_t *p, float src[16][16][2], const uvXform_t &A )
    {
        const int layer = ClampI( g_qeglobals.current_edit_layer, 0, 2 );
        const int w = ClampI( p->width,  0, 16 );
        const int h = ClampI( p->height, 0, 16 );
        for ( int i = 0; i < w; ++i )
            for ( int j = 0; j < h; ++j )
            {
                float ns, nt;
                XfApply( A, src[i][j][0], src[i][j][1], &ns, &nt );
                p->ctrl[i][j].texCoord.st[2 * layer]     = ns;
                p->ctrl[i][j].texCoord.st[2 * layer + 1] = nt;
            }
        // The REBUILD, borrowed rather than re-spelled: a zero shift adds nothing and then
        // runs pmesh.cpp:3241-3246 (bDirty for layer 1, free curveDef, Patch_GenericMesh2,
        // ++version).  Note this file transforms the WHOLE control grid even in
        // sel_curvepoint mode — the canvas draws the whole grid, so it edits the whole
        // grid; Patch_ShiftTexture's d_move_points filter only gates ST writes, and ours
        // are already done.
        Patch_ShiftTexture( p, 0.0f, 0.0f );
    }

    void SnapshotPatch( patchMesh_t *p, float dst[16][16][2] )
    {
        const int layer = ClampI( g_qeglobals.current_edit_layer, 0, 2 );
        memset( dst, 0, sizeof( float ) * 16 * 16 * 2 );
        const int w = ClampI( p->width,  0, 16 );
        const int h = ClampI( p->height, 0, 16 );
        for ( int i = 0; i < w; ++i )
            for ( int j = 0; j < h; ++j )
            {
                dst[i][j][0] = p->ctrl[i][j].texCoord.st[2 * layer];
                dst[i][j][1] = p->ctrl[i][j].texCoord.st[2 * layer + 1];
            }
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  UNDO (D-BD-G)
    // ═══════════════════════════════════════════════════════════════════════════════
    void UndoOpen( const char *op )      // `op` MUST be a literal — the record stores it
    {
        if ( s_undoOpen )
            return;
        // ── KIWI-UX (ROUND BH, ITEM 3): END A PARKED FACE GESTURE FIRST ────────
        // USER REPORT, verbatim: *"Make it so texture applications dont require a
        // right-click/enter to confirm, they are just an operation that goes
        // through when you click the new texture (same with UVs)."*
        //
        // THIS WINDOW ALREADY COMMITTED PER GESTURE — every write path here is
        // UndoOpen -> mutate -> RebuildAll -> UndoCommit (:2123 for the drag end,
        // :2680 for ApplyAffine, :2690 for ApplyField), and KiwiCmd_UndoCommit
        // closes the record on the spot.  Nothing here was ever deferred to a
        // confirm.  What DID take UV edits back is the same thing that took
        // texture applies back: in Face mode a face click auto-enters
        // KIWI_CMD_MOVE and parks it, and that gesture's per-face baseline is the
        // whole MaterialDef BLOCK (kiwi_transform.cpp:1663) — a texdef lives
        // INSIDE that block — which RestoreAll (:1573) and every push frame
        // (:2638) memcpy back over the face.  So an edit made here survived until
        // the parked gesture was cancelled, and RMB / Enter (its Commit) was the
        // only exit that kept it.  kiwi_uv.cpp has the full chain.
        //
        // At UndoOpen rather than at each caller: this is the ONE place every
        // mutating path in this file passes through, and it runs BEFORE the first
        // write.  The return value is deliberately NOT acted on here — a refusal
        // (a live gesture that has applied something and does not opt into the
        // round-Z swap protocol) leaves this window behaving exactly as it did
        // before this round, bracket and all, plus one console line naming the
        // command that is in the way.  Refusing the edit outright would be a new
        // way for the UV window to do nothing, which is not what was asked for.
        KiwiUv_EndGestureBeforeApply( "UV editor" );
        KiwiCmd_UndoBegin( op );
        s_undoOpen = true;
        if ( !s_covered )
        {
            // KiwiCmd_UndoBegin cloned `selected_brushes`; a brush named only by a FACE is
            // not on that list (kiwi_selection.h DESIGN NOTE 2).  Cover every gathered row
            // — Undo_AddBrush self-dedupes (undo.cpp:485), so the whole-selected ones cost
            // nothing extra.  BEFORE the first mutation, which is the whole point.
            for ( size_t i = 0; i < s_brushes.size(); ++i )
                KiwiCmd_UndoCoverBrush( s_brushes[i].node );
            s_covered = true;
        }
    }

    void UndoCommit()
    {
        if ( !s_undoOpen )
            return;
        KiwiCmd_UndoCommit();
        s_undoOpen = false;
        s_covered  = false;
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  CANVAS TRANSFORM
    // ═══════════════════════════════════════════════════════════════════════════════
    // +T is DOWN on the canvas, matching the image v axis, so a tile drawn at
    // (u..u+1, v..v+1) shows the texture the right way up.
    ImVec2 UvToPx( float u, float v )
    {
        return ImVec2( s_c0.x + ( u - s_panU ) * s_zoom,
                       s_c0.y + ( v - s_panV ) * s_zoom );
    }

    void PxToUv( const ImVec2 &p, float *u, float *v )
    {
        *u = s_panU + ( p.x - s_c0.x ) / s_zoom;
        *v = s_panV + ( p.y - s_c0.y ) / s_zoom;
    }

    float StripeU() { return 1.0f / (float)ClampI( s_subX, 1, 16 ); }
    float StripeV() { return 1.0f / (float)ClampI( s_subY, 1, 16 ); }

    // The UV rect the canvas currently shows.  FALSE means "not a rect any integer loop
    // may be built from": both the tile fill and the grid convert these to `int` line
    // indices, and a face whose STs run to 1e9 (a size[] of a few thousandths, which the
    // Surface Inspector will happily type) would make `(int)floorf(u0)` undefined and the
    // loop bound meaningless.  One guard, both consumers.
    const float KUVE_UV_SANE = 1.0e6f;
    bool VisibleUvRect( float *u0, float *v0, float *u1, float *v1 )
    {
        PxToUv( s_c0, u0, v0 );
        PxToUv( ImVec2( s_c0.x + s_cs.x, s_c0.y + s_cs.y ), u1, v1 );
        return fabsf( *u0 ) < KUVE_UV_SANE && fabsf( *u1 ) < KUVE_UV_SANE
            && fabsf( *v0 ) < KUVE_UV_SANE && fabsf( *v1 ) < KUVE_UV_SANE;
    }

    // TB resetZoom (UvViewHelper.cpp:316-348): margins clamp(10%, 2..40) px, fit, and
    // re-run on every viewport change until a valid zoom can be computed — the widget can
    // be size-0 the first frame a selection exists.
    void FrameActive()
    {
        // KIWI-UX (ROUND BI): the ORIGIN is no longer seeded from here.  It follows the
        // TARGET SET's displayed bbox centre every idle frame (D-BI-D, AutoPivot), so this
        // function is back to what its name says: the zoom fit over the whole DISPLAYED
        // spread, which is what the fold-out decides.
        float mn[2] = {  1.0e30f,  1.0e30f };
        float mx[2] = { -1.0e30f, -1.0e30f };
        bool  any   = false;

        float st[KUVE_MAX_WINDING][2];
        for ( size_t f = 0; f < s_faces.size(); ++f )
        {
            const int n = FaceStPointsDisp( f, st, nullptr );
            for ( int i = 0; i < n; ++i )
                for ( int k = 0; k < 2; ++k )
                {
                    if ( st[i][k] < mn[k] ) mn[k] = st[i][k];
                    if ( st[i][k] > mx[k] ) mx[k] = st[i][k];
                    any = true;
                }
        }
        for ( size_t pi = 0; pi < s_patches.size(); ++pi )
        {
            patchMesh_t *p = s_brushes[s_patches[pi]].pm;
            if ( !p )
                continue;
            const int w = ClampI( p->width, 0, 16 ), h = ClampI( p->height, 0, 16 );
            for ( int i = 0; i < w; ++i )
                for ( int j = 0; j < h; ++j )
                {
                    float d[2];
                    PatchStDisp( pi, i, j, &d[0], &d[1] );
                    for ( int k = 0; k < 2; ++k )
                    {
                        if ( d[k] < mn[k] ) mn[k] = d[k];
                        if ( d[k] > mx[k] ) mx[k] = d[k];
                    }
                    any = true;
                }
        }
        if ( !any )
        {
            mn[0] = mn[1] = 0.0f;
            mx[0] = mx[1] = 1.0f;
        }

        float bw = mx[0] - mn[0];
        float bh = mx[1] - mn[1];
        if ( bw < 1.0e-4f ) bw = 1.0f;
        if ( bh < 1.0e-4f ) bh = 1.0f;

        if ( s_cs.x <= 20.0f || s_cs.y <= 20.0f )
            return;                          // TB's size-0 guard: try again next frame

        const float mgX = ClampF( s_cs.x * 0.1f, 2.0f, 40.0f );
        const float mgY = ClampF( s_cs.y * 0.1f, 2.0f, 40.0f );
        const float availW = s_cs.x - 2.0f * mgX;
        const float availH = s_cs.y - 2.0f * mgY;

        float z = KUVE_ZOOM_MAX;
        if ( availW / bw < z ) z = availW / bw;
        if ( availH / bh < z ) z = availH / bh;
        s_zoom = ClampF( z, KUVE_ZOOM_MIN, KUVE_ZOOM_MAX );

        // Centre the box.
        s_panU = 0.5f * ( mn[0] + mx[0] ) - 0.5f * s_cs.x / s_zoom;
        s_panV = 0.5f * ( mn[1] + mx[1] ) - 0.5f * s_cs.y / s_zoom;
        s_zoomValid = true;
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  THE BACKGROUND (D-BD-D)
    // ═══════════════════════════════════════════════════════════════════════════════
    // The ACTIVE material's colormap, as an ImGui texture, or null.  This is the round-AZ
    // MAPTYPE_2D arm (kiwi_skybox.cpp:451-496) with the CUBE arm deliberately absent —
    // see D-BD-E.  The guards are the same three and each is load-bearing:
    //   textureTable null   — an unloaded / default material
    //   semantic != 2       — TS_COLOR_MAP; the table is hash-sorted, so [0] is a coin flip
    //   delayLoadPixels     — GfxTexture is a UNION (r_gfx.h:203-210); before upload the
    //                         live arm is loadDef, and handing THAT to ImGui is a crash
    IDirect3DTexture9 *ActiveColorMap( MaterialDef *md, bool *outNot2D )
    {
        if ( outNot2D )
            *outNot2D = false;
        if ( !MtlDefUsable( md ) )
            return nullptr;
        qtexture_s *q = MaterialDef_GetLayeredMaterial( md );
        if ( !q )
            return nullptr;
        if ( !q->next && q->name )
            Texture_GetHandle( q->name );        // lazy registration (texwnd.cpp:320)
        Material *mtl = q->next;
        if ( !mtl || !mtl->textureTable )
            return nullptr;
        for ( int i = 0; i < (int)mtl->textureCount; ++i )
        {
            if ( mtl->textureTable[i].semantic != 2 )        // TS_COLOR_MAP
                continue;
            GfxImage *img = mtl->textureTable[i].u.image;
            if ( !img || img->delayLoadPixels )
                continue;
            if ( img->mapType == MAPTYPE_2D && img->texture.map )
                return img->texture.map;
            if ( outNot2D )
                *outNot2D = true;                // a cubemap / volume: nothing to tile
        }
        return nullptr;
    }

    void DrawBackground( ImDrawList *dl, IDirect3DTexture9 *tex )
    {
        dl->AddRectFilled( s_c0, ImVec2( s_c0.x + s_cs.x, s_c0.y + s_cs.y ), KUVE_COL_BG );
        if ( !tex )
            return;

        float u0, v0, u1, v1;
        if ( !VisibleUvRect( &u0, &v0, &u1, &v1 ) )
            return;

        const int i0 = (int)floorf( u0 ), i1 = (int)floorf( u1 );
        const int j0 = (int)floorf( v0 ), j1 = (int)floorf( v1 );
        const double nx = (double)i1 - (double)i0 + 1.0;
        const double ny = (double)j1 - (double)j0 + 1.0;
        if ( nx * ny > (double)KUVE_MAX_TILES )
            return;                              // past the cap: the flat plate stands

        for ( int j = j0; j <= j1; ++j )
            for ( int i = i0; i <= i1; ++i )
                dl->AddImage( (ImTextureID)(intptr_t)tex,
                              UvToPx( (float)i, (float)j ),
                              UvToPx( (float)i + 1.0f, (float)j + 1.0f ),
                              ImVec2( 0.0f, 0.0f ), ImVec2( 1.0f, 1.0f ),
                              KUVE_COL_TILE_TINT );
    }

    void DrawGrid( ImDrawList *dl )
    {
        float u0, v0, u1, v1;
        if ( !VisibleUvRect( &u0, &v0, &u1, &v1 ) )
            return;

        const float sx = StripeU(), sy = StripeV();
        const int   nU = (int)( ( u1 - u0 ) / sx ) + 2;
        const int   nV = (int)( ( v1 - v0 ) / sy ) + 2;
        if ( nU > 0 && nU < 2048 )
        {
            const int k0 = (int)floorf( u0 / sx );
            for ( int k = k0; k <= k0 + nU; ++k )
            {
                const float u = (float)k * sx;
                const bool  maj = ( ( k % ClampI( s_subX, 1, 16 ) ) == 0 );
                dl->AddLine( UvToPx( u, v0 ), UvToPx( u, v1 ),
                             maj ? KUVE_COL_GRID_MAJ : KUVE_COL_GRID_MIN,
                             maj ? 1.5f : 1.0f );
            }
        }
        if ( nV > 0 && nV < 2048 )
        {
            const int k0 = (int)floorf( v0 / sy );
            for ( int k = k0; k <= k0 + nV; ++k )
            {
                const float v = (float)k * sy;
                const bool  maj = ( ( k % ClampI( s_subY, 1, 16 ) ) == 0 );
                dl->AddLine( UvToPx( u0, v ), UvToPx( u1, v ),
                             maj ? KUVE_COL_GRID_MAJ : KUVE_COL_GRID_MIN,
                             maj ? 1.5f : 1.0f );
            }
        }
    }

    // KIWI-UX (ROUND BG, ITEM 6): a non-target shape keeps its hue and loses its alpha —
    // dimming the COLOUR would make the two shape families (blue faces, green patches)
    // converge on the same grey and lose the one thing the canvas uses colour for.
    ImU32 DimIf( ImU32 col, bool target )
    {
        if ( target )
            return col;
        const unsigned a = ( col >> IM_COL32_A_SHIFT ) & 0xFFu;
        const unsigned d = (unsigned)( (float)a * KUVE_DIM_ALPHA );
        return ( col & ~( 0xFFu << IM_COL32_A_SHIFT ) ) | ( d << IM_COL32_A_SHIFT );
    }

    void DrawWireframes( ImDrawList *dl )
    {
        float st[KUVE_MAX_WINDING][2];
        ImVec2 pts[KUVE_MAX_WINDING];

        for ( size_t i = 0; i < s_faces.size(); ++i )
        {
            // ROUND BG: through D_f (D-BG-A).  With the chain off, or on a single face,
            // D is identity and this is BD's loop unchanged.
            const int n = FaceStPointsDisp( i, st, nullptr );
            if ( n < 2 )
                continue;
            for ( int k = 0; k < n; ++k )
                pts[k] = UvToPx( st[k][0], st[k][1] );
            // ── KIWI-UX (ROUND BJ, ITEM 3): EVERY TARGET IS GOLD ──────────────
            // USER REPORT, verbatim: *"Everything selected in the UV editor should
            // be yellow (not just 1 chunk)."*  BG gave the gold outline to the
            // ACTIVE face and nothing else, and dimmed by target — so a five-face
            // target set drew one gold shape and four blue ones and read as "one of
            // these is selected".  The gold IS the target treatment now, and the
            // active face gets no outline of its own: it survives only as the
            // background-material source and the chain-fold anchor (D-BJ-D).
            const bool tgt = FaceIsTarget( i );
            dl->AddPolyline( pts, n, DimIf( tgt ? KUVE_COL_ACTIVE : KUVE_COL_FACE, tgt ),
                             ImDrawFlags_Closed, tgt ? 2.5f : 1.5f );
        }

        for ( size_t pi = 0; pi < s_patches.size(); ++pi )
        {
            patchMesh_t *p = s_brushes[s_patches[pi]].pm;
            if ( !p )
                continue;
            // ROUND BJ, ITEM 3: a targeted patch is gold too.  It costs the green
            // family tell while it is targeted, which is the right trade: the grid
            // net already says "patch" at a glance, and "which of these will my
            // drag move" is the question the canvas has to answer first.
            const bool ptgt = PatchIsTarget( pi );
            const ImU32 col = DimIf( ptgt ? KUVE_COL_ACTIVE : KUVE_COL_PATCH, ptgt );
            const int w = ClampI( p->width, 0, 16 ), h = ClampI( p->height, 0, 16 );
            for ( int i = 0; i < w; ++i )
            {
                for ( int j = 0; j < h; ++j )
                {
                    float u, v;
                    PatchStDisp( pi, i, j, &u, &v );
                    pts[j] = UvToPx( u, v );
                }
                if ( h >= 2 )
                    dl->AddPolyline( pts, h, col, 0, 1.5f );
            }
            for ( int j = 0; j < h; ++j )
            {
                for ( int i = 0; i < w; ++i )
                {
                    float u, v;
                    PatchStDisp( pi, i, j, &u, &v );
                    pts[i] = UvToPx( u, v );
                }
                if ( w >= 2 )
                    dl->AddPolyline( pts, w, col, 0, 1.5f );
            }
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  HIT TESTS — KIWI-UX (ROUND BI): THE TRANSFORM BOX, NOT THE GRID (D-BI-B)
    // ═══════════════════════════════════════════════════════════════════════════════
    // DELETED with this round, not disabled: TB's `pickUvGrid` port (GridPickPx / PickGrid),
    // the rotate RING pick, and the origin's two infinite-axis-LINE picks.  D-BI-A has the
    // measurement that condemned them; what is left is a pivot DISC and eight box handles,
    // all of which sit ON the shapes the gesture will move.
    //
    // The pivot: TB UvOriginTool::pick (:328-374) reduced to its circle arm.  The PICK
    // radius (8 px) is deliberately larger than the radius it is DRAWN at (5 px, TB's) —
    // a 5 px disc is a 10 px target and that was the original complaint.
    bool PickPivot( float u, float v )
    {
        const float du = ( u - s_originU ) * s_zoom;
        const float dv = ( v - s_originV ) * s_zoom;
        return sqrtf( du * du + dv * dv ) <= KUVE_ORIGIN_PICK_RAD;
    }

    // The transform box, in canvas PIXELS, already inflated.  `valid` is false when the
    // target set has nothing with a drawable outline in it.
    struct uvBox_t
    {
        bool  valid;
        float x0, y0, x1, y1;
    };
    uvBox_t s_box = { false, 0.0f, 0.0f, 0.0f, 0.0f };

    // The bbox of every TARGETED shape's DISPLAYED outline, in UV.  Patches contribute
    // their whole control net rather than the ring: an interior control point can lie
    // outside the ring, and a box that a shape pokes out of is a lie.
    bool TargetBoxUv( float *mn, float *mx )
    {
        bool any = false;
        mn[0] = mn[1] =  1.0e30f;
        mx[0] = mx[1] = -1.0e30f;

        float st[KUVE_MAX_WINDING][2];
        for ( size_t i = 0; i < s_faces.size(); ++i )
        {
            if ( !FaceIsTarget( i ) )
                continue;
            const int n = FaceStPointsDisp( i, st, nullptr );
            for ( int k = 0; k < n; ++k )
                for ( int c = 0; c < 2; ++c )
                {
                    if ( st[k][c] < mn[c] ) mn[c] = st[k][c];
                    if ( st[k][c] > mx[c] ) mx[c] = st[k][c];
                    any = true;
                }
        }
        for ( size_t pi = 0; pi < s_patches.size(); ++pi )
        {
            if ( !PatchIsTarget( pi ) )
                continue;
            patchMesh_t *p = s_brushes[s_patches[pi]].pm;
            if ( !p )
                continue;
            const int w = ClampI( p->width, 0, 16 ), h = ClampI( p->height, 0, 16 );
            for ( int i = 0; i < w; ++i )
                for ( int j = 0; j < h; ++j )
                {
                    float d[2];
                    PatchStDisp( pi, i, j, &d[0], &d[1] );
                    for ( int c = 0; c < 2; ++c )
                    {
                        if ( d[c] < mn[c] ) mn[c] = d[c];
                        if ( d[c] > mx[c] ) mx[c] = d[c];
                    }
                    any = true;
                }
        }
        return any;
    }

    uvBox_t ComputeBox()
    {
        uvBox_t b = { false, 0.0f, 0.0f, 0.0f, 0.0f };
        float mn[2], mx[2];
        if ( !TargetBoxUv( mn, mx ) )
            return b;
        const ImVec2 a = UvToPx( mn[0], mn[1] );   // +T is DOWN, so this is the TOP-LEFT
        const ImVec2 c = UvToPx( mx[0], mx[1] );
        b.valid = true;
        b.x0 = a.x - KUVE_BOX_INFLATE_PX;
        b.y0 = a.y - KUVE_BOX_INFLATE_PX;
        b.x1 = c.x + KUVE_BOX_INFLATE_PX;
        b.y1 = c.y + KUVE_BOX_INFLATE_PX;
        return b;
    }

    // The box WITHOUT its inflation — the shapes' own bbox.  Scale and skew measure from
    // this one: the inflation exists so the handles do not sit on top of the outline, and
    // a factor derived from an inflated reference would make the drag's grabbed point miss
    // the cursor by `inflation × (k − 1)`, which is the whole "shapes move with the cursor"
    // law (D-BG-D) failing by a few pixels at every large factor.  Only ever called on a
    // box that passed BoxHasHandles, which is >= 24 px, i.e. wider than the 12 px it loses.
    uvBox_t DeflateBox( const uvBox_t &b )
    {
        uvBox_t r = b;
        r.x0 += KUVE_BOX_INFLATE_PX;
        r.y0 += KUVE_BOX_INFLATE_PX;
        r.x1 -= KUVE_BOX_INFLATE_PX;
        r.y1 -= KUVE_BOX_INFLATE_PX;
        if ( r.x1 < r.x0 || r.y1 < r.y0 )
            return b;
        return r;
    }

    // ── KIWI-UX (ROUND BI): the pivot follows the target set (D-BI-D) ───────────────
    // Re-derived every idle frame from the SAME displayed bbox the box is drawn from, so a
    // retarget moves the pivot with it and Rot 90 / Flip turn the target set about its own
    // centre.  A pivot the user has dragged latches `s_originUser` and stops following
    // until the 3D selection changes (which is where the latch clears).
    void AutoPivot()
    {
        if ( s_originUser || s_gesture != UVG_NONE )
            return;
        float mn[2], mx[2];
        if ( !TargetBoxUv( mn, mx ) )
            return;
        s_originU = 0.5f * ( mn[0] + mx[0] );
        s_originV = 0.5f * ( mn[1] + mx[1] );
    }

    // Handle centres, in canvas pixels.  Order matches uvHandle_t: 4 corners, then the 4
    // edge midpoints, starting at the top and going clockwise in both groups.
    void HandlePx( const uvBox_t &b, int h, float *x, float *y )
    {
        const float cx = 0.5f * ( b.x0 + b.x1 );
        const float cy = 0.5f * ( b.y0 + b.y1 );
        switch ( h )
        {
        case UVH_NW: *x = b.x0; *y = b.y0; break;
        case UVH_NE: *x = b.x1; *y = b.y0; break;
        case UVH_SE: *x = b.x1; *y = b.y1; break;
        case UVH_SW: *x = b.x0; *y = b.y1; break;
        case UVH_N:  *x = cx;   *y = b.y0; break;
        case UVH_E:  *x = b.x1; *y = cy;   break;
        case UVH_S:  *x = cx;   *y = b.y1; break;
        default:     *x = b.x0; *y = cy;   break;    // UVH_W
        }
    }

    // ── KIWI-UX (ROUND BJ, ITEM 5): the handle across the box from `h` ──────────────
    // Corners: NW<->SE, NE<->SW — which is `h ^ 2` over the 0..3 corner order.  Edge
    // mids: N<->S, E<->W — the same half-turn over the 4..7 order.  That handle's
    // position is the scale ANCHOR, so the side the user grabbed is the only one that
    // moves.  (For an edge-mid the anchor's OTHER coordinate is the box centre and is
    // never read: a single-axis scale leaves the other axis at factor 1, and
    // XfAboutOrigin's translation term for that axis is then exactly zero.)
    int OppositeHandle( int h )
    {
        if ( h >= UVH_NW && h <= UVH_SW )
            return h ^ 2;
        if ( h >= UVH_N && h <= UVH_W )
            return UVH_N + ( ( h - UVH_N + 2 ) & 3 );
        return UVH_NONE;
    }

    // Which axes a handle scales.  Corners take both; N/S take T only; E/W take S only.
    void HandleAxes( int h, bool *axis )
    {
        axis[0] = ( h == UVH_NW || h == UVH_NE || h == UVH_SE || h == UVH_SW
                    || h == UVH_E || h == UVH_W );
        axis[1] = ( h == UVH_NW || h == UVH_NE || h == UVH_SE || h == UVH_SW
                    || h == UVH_N || h == UVH_S );
    }

    bool InBox( const uvBox_t &b, float mx, float my )
    {
        return b.valid && mx >= b.x0 && mx <= b.x1 && my >= b.y0 && my <= b.y1;
    }

    // A box smaller than KUVE_BOX_MIN_PX on either axis carries NO handles at all: eight
    // 14 px pick squares on a 20 px box would cover it completely and the BODY DRAG — the
    // gesture the user is here for — would become unreachable.  Below that size the box is
    // a plain outline, everything inside it drags, and the way to the handles is the wheel.
    // Both the pick and the draw ask this one function, so they cannot disagree.
    bool BoxHasHandles( const uvBox_t &b )
    {
        return b.valid && ( b.x1 - b.x0 ) >= KUVE_BOX_MIN_PX
                       && ( b.y1 - b.y0 ) >= KUVE_BOX_MIN_PX;
    }

    // Square pick on the eight handles; then, for the CORNERS only, the rotate annulus —
    // which must also be OUTSIDE the box, so that it can never take a press that belongs
    // to the body (D-BI-B).  Returns UVH_NONE for a miss.
    int PickHandle( const uvBox_t &b, float mx, float my, bool *outRotate )
    {
        *outRotate = false;
        if ( !BoxHasHandles( b ) )
            return UVH_NONE;
        for ( int h = 0; h < 8; ++h )
        {
            float hx, hy;
            HandlePx( b, h, &hx, &hy );
            if ( fabsf( mx - hx ) <= KUVE_HANDLE_PICK_PX
                 && fabsf( my - hy ) <= KUVE_HANDLE_PICK_PX )
                return h;
        }
        if ( InBox( b, mx, my ) )
            return UVH_NONE;
        for ( int h = 0; h < 4; ++h )
        {
            float hx, hy;
            HandlePx( b, h, &hx, &hy );
            const float d = sqrtf( ( mx - hx ) * ( mx - hx ) + ( my - hy ) * ( my - hy ) );
            if ( d <= KUVE_HANDLE_PICK_PX + KUVE_ROTATE_ZONE_PX )
            {
                *outRotate = true;
                return h;
            }
        }
        return UVH_NONE;
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BG, ITEM 6) — SHAPE HIT-TESTING, IN DISPLAYED SPACE (D-BG-B)
    // ═══════════════════════════════════════════════════════════════════════════════
    bool PointInPoly( const float pt[2], const float poly[KUVE_MAX_WINDING][2], int n )
    {
        bool in = false;
        for ( int i = 0, j = n - 1; i < n; j = i++ )
        {
            if ( ( ( poly[i][1] > pt[1] ) != ( poly[j][1] > pt[1] ) )
                 && ( pt[0] < ( poly[j][0] - poly[i][0] ) * ( pt[1] - poly[i][1] )
                              / ( poly[j][1] - poly[i][1] ) + poly[i][0] ) )
                in = !in;
        }
        return in;
    }

    // Distance from pt to the polygon's boundary, in canvas PIXELS.  The enlarged
    // tolerance from item 3a applies here too: a thin sliver of a face is unclickable by
    // area alone, and its outline is the thing the user is aiming at anyway.
    float PolyEdgeDistPx( const float pt[2], const float poly[KUVE_MAX_WINDING][2], int n )
    {
        float best = 1.0e30f;
        for ( int i = 0, j = n - 1; i < n; j = i++ )
        {
            const float ax = poly[j][0], ay = poly[j][1];
            const float bx = poly[i][0], by = poly[i][1];
            const float ex = bx - ax, ey = by - ay;
            const float len2 = ex * ex + ey * ey;
            float t = 0.0f;
            if ( len2 > KUVE_EPS )
                t = ClampF( ( ( pt[0] - ax ) * ex + ( pt[1] - ay ) * ey ) / len2, 0.0f, 1.0f );
            const float dx = ( ax + ex * t - pt[0] ) * s_zoom;
            const float dy = ( ay + ey * t - pt[1] ) * s_zoom;
            const float d  = sqrtf( dx * dx + dy * dy );
            if ( d < best )
                best = d;
        }
        return best;
    }

    // ── KIWI-UX (ROUND BI): EVERY shape under the cursor, TOPMOST FIRST (D-BI-C) ──────
    // BG's PickShape returned ONE shape and ranked with a strict `<` over a rank that is
    // 0.0 for every shape CONTAINING the point — so of N stacked shapes only the
    // lowest-indexed one was ever reachable, with no way down the stack.  That is half of
    // the "still cannot select each sub-shape" report (D-BI-A).  This returns the whole
    // stack in reverse DRAW order, which is what "topmost" means on this canvas:
    // DrawWireframes draws faces in index order and then patches, so patches are over
    // faces and a later row is over an earlier one.
    struct uvShapeRef_t { int face, patch; };

    int CollectShapesAt( float u, float v, uvShapeRef_t *out, int maxOut )
    {
        int n = 0;
        const float pt[2] = { u, v };
        float poly[KUVE_MAX_WINDING][2];

        for ( int pi = (int)s_patches.size() - 1; pi >= 0 && n < maxOut; --pi )
        {
            const int np = PatchRing( (size_t)pi, poly );
            if ( np < 3 )
                continue;
            if ( !PointInPoly( pt, poly, np )
                 && PolyEdgeDistPx( pt, poly, np ) > KUVE_SHAPE_EDGE_PX )
                continue;
            out[n].face  = -1;
            out[n].patch = pi;
            ++n;
        }
        for ( int i = (int)s_faces.size() - 1; i >= 0 && n < maxOut; --i )
        {
            const int nf = FaceStPointsDisp( (size_t)i, poly, nullptr );
            if ( nf < 3 )
                continue;
            if ( !PointInPoly( pt, poly, nf )
                 && PolyEdgeDistPx( pt, poly, nf ) > KUVE_SHAPE_EDGE_PX )
                continue;
            out[n].face  = i;
            out[n].patch = -1;
            ++n;
        }
        return n;
    }

    // Is this press within KUVE_CYCLE_PX of the previous one — i.e. the same spot, clicked
    // again?  That and nothing else is what advances the stack cycle.
    bool CycleIsRepeat( float u, float v )
    {
        return s_cycleValid
            && fabsf( u - s_cycleU ) * s_zoom <= KUVE_CYCLE_PX
            && fabsf( v - s_cycleV ) * s_zoom <= KUVE_CYCLE_PX;
    }

    // Which entry of that stack the NEXT press takes.  Pure — the press updates the cycle
    // state itself, so the hover probe can ask the same question without moving anything.
    int CycleIndexFor( float u, float v, int n )
    {
        if ( n < 1 )
            return 0;
        if ( CycleIsRepeat( u, v ) )
            return ( ( s_cycleNext % n ) + n ) % n;
        return 0;
    }

    // ── KIWI-UX (ROUND BI): the marquee's shape test (D-BI-C) ────────────────────────
    // "Intersects" is the honest test for a rubber band over OUTLINES: a vertex inside the
    // band, an edge crossing it, or the band sitting entirely inside the outline.  Segment
    // vs. axis-aligned rect is Liang-Barsky, which needs no square roots and no cases.
    bool SegHitsRect( float ax, float ay, float bx, float by,
                      const float r0[2], const float r1[2] )
    {
        float t0 = 0.0f, t1 = 1.0f;
        const float d[2] = { bx - ax, by - ay };
        const float p[2] = { ax, ay };
        for ( int c = 0; c < 2; ++c )
        {
            const float lo = ( c == 0 ) ? r0[0] : r0[1];
            const float hi = ( c == 0 ) ? r1[0] : r1[1];
            if ( fabsf( d[c] ) < KUVE_EPS )
            {
                if ( p[c] < lo || p[c] > hi )
                    return false;
                continue;
            }
            float e0 = ( lo - p[c] ) / d[c];
            float e1 = ( hi - p[c] ) / d[c];
            if ( e0 > e1 ) { const float t = e0; e0 = e1; e1 = t; }
            if ( e0 > t0 ) t0 = e0;
            if ( e1 < t1 ) t1 = e1;
            if ( t0 > t1 )
                return false;
        }
        return true;
    }

    bool PolyHitsRect( const float poly[KUVE_MAX_WINDING][2], int n,
                       const float r0[2], const float r1[2] )
    {
        for ( int i = 0; i < n; ++i )
            if ( poly[i][0] >= r0[0] && poly[i][0] <= r1[0]
                 && poly[i][1] >= r0[1] && poly[i][1] <= r1[1] )
                return true;
        for ( int i = 0, j = n - 1; i < n; j = i++ )
            if ( SegHitsRect( poly[j][0], poly[j][1], poly[i][0], poly[i][1], r0, r1 ) )
                return true;
        const float c[2] = { 0.5f * ( r0[0] + r1[0] ), 0.5f * ( r0[1] + r1[1] ) };
        return PointInPoly( c, poly, n );
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BG, ITEM 3a) — ONE PROBE, RUN BY BOTH THE HOVER AND THE PRESS
    // ═══════════════════════════════════════════════════════════════════════════════
    // BD ran the ladder inside GestureBegin and a PARTIAL copy of it inside DrawOverlays,
    // which is how a highlight and a press can disagree.  There is now exactly one
    // function that answers "what would a press here grab?", and both callers use it —
    // so the hover feedback is correct BY CONSTRUCTION rather than by maintenance.
    struct uvProbe_t
    {
        uvGesture_t what;        // what a press here would BEGIN (UVG_NONE == nothing)
        bool        axis[2];     // for SCALE / SHEAR
        int         handle;      // uvHandle_t, or UVH_NONE
        bool        rotateZone;  // the press is in a corner's rotate annulus
        bool        shearRefused;
        int         shapeFace, shapePatch;   // the shape a press would TARGET (D-BI-C)
        bool        shapeIsTarget;           // ...and whether it already is one
        bool        inBox;
    };

    void ProbeClear( uvProbe_t *p )
    {
        p->what    = UVG_NONE;
        p->axis[0] = p->axis[1] = false;
        p->handle  = UVH_NONE;
        p->rotateZone   = false;
        p->shearRefused = false;
        p->shapeFace    = p->shapePatch = -1;
        p->shapeIsTarget = false;
        p->inBox   = false;
    }

    // ── THE LADDER, ROUND BI (D-BI-B/C) ─────────────────────────────────────────────
    // Pivot -> box handles -> the SHAPE under the cursor -> the box body -> marquee.  The
    // shapes now sit ABOVE everything except the two handle families that are drawn ON
    // them, which is the whole correction: BG put three canvas-global objects (grid lines,
    // the origin's infinite axis lines, the rotate ring) above the shape hit-test, and at
    // a fit zoom those covered most of the canvas (D-BI-A).  Ctrl is snapping-only now.
    uvProbe_t Probe( float u, float v, bool altDown )
    {
        uvProbe_t p;
        ProbeClear( &p );
        if ( !AnythingSelected() )
            return p;

        const ImVec2 px = UvToPx( u, v );
        p.inBox = InBox( s_box, px.x, px.y );

        // 1. THE PIVOT — view state only (TB UvOriginTool.cpp:120-125).  It is first
        //    because it is the smallest target on the canvas and it sits inside the box.
        if ( PickPivot( u, v ) )
        {
            p.what    = UVG_ORIGIN;
            p.axis[0] = p.axis[1] = true;     // the disc is the only pivot grab now
            return p;
        }

        // 2. THE BOX HANDLES — scale, rotate (corner annulus), skew (Alt + edge mid).
        bool rot = false;
        const int h = PickHandle( s_box, px.x, px.y, &rot );
        if ( h != UVH_NONE )
        {
            p.handle     = h;
            p.rotateZone = rot;
            HandleAxes( h, p.axis );
            if ( rot )
            {
                p.what = UVG_ROTATE;
                return p;
            }
            if ( altDown && h >= UVH_N )      // skew lives on the EDGE MIDPOINTS only
            {
                // TB issue #1350 (UvShearTool.cpp:284-289): the shear factor divides by the
                // handle's offset from the pivot along the OTHER axis, so a handle sitting
                // on the pivot's axis is a division by ~zero.  Refuse rather than explode.
                const uvBox_t raw = DeflateBox( s_box );
                float hx, hy;
                HandlePx( raw, h, &hx, &hy );
                float hu, hv;
                PxToUv( ImVec2( hx, hy ), &hu, &hv );
                const float offU = fabsf( hu - s_originU ) * s_zoom;
                const float offV = fabsf( hv - s_originV ) * s_zoom;
                if ( ( p.axis[0] && offU < KUVE_SHEAR_MIN_PX )
                     || ( p.axis[1] && offV < KUVE_SHEAR_MIN_PX ) )
                {
                    p.shearRefused = true;
                    return p;
                }
                p.what = UVG_SHEAR;
                return p;
            }
            p.what = UVG_SCALE;               // Alt on a CORNER is still a scale
            return p;
        }

        // 3. THE SHAPES — a body press moves, and targets in the same press.  The entry of
        //    the stack it resolves to is the cycle's (D-BI-C); the hover asks for it the
        //    same way the press does, so the highlight cannot name a different shape.
        uvShapeRef_t stack[64];
        const int    n = CollectShapesAt( u, v, stack, 64 );
        if ( n > 0 )
        {
            const int k = CycleIndexFor( u, v, n );
            p.shapeFace  = stack[k].face;
            p.shapePatch = stack[k].patch;
            p.shapeIsTarget = ( p.shapeFace >= 0 ) ? FaceIsTarget( (size_t)p.shapeFace )
                                                   : PatchIsTarget( (size_t)p.shapePatch );
            p.what = UVG_MOVE;
            return p;
        }

        // 4/5. EMPTY SPACE.  Inside the box it is the body of a real target set, so it
        //      drags; with no sub-selection there is no such object and it rubber-bands
        //      (D-BI-C states the reconciliation and why it is not a mode).
        p.what = ( p.inBox && !s_targetAll ) ? UVG_MOVE : UVG_MARQUEE;
        return p;
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  SNAPPING (all of it TB's, adapted to repeat units)
    // ═══════════════════════════════════════════════════════════════════════════════
    // ═══════════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BJ, ITEM 6) — SNAP FEEDBACK (D-BJ-F)
    // ═══════════════════════════════════════════════════════════════════════════════
    // USER REPORT, verbatim: *"The snapping isn't very intuitive."*
    //
    // The MECHANISM was already right — D-BI-F snaps the thing the user is holding
    // (the target set's own displayed vertices on a move; the dragged handle on a
    // scale) against the grid, whole texels and the OTHER shapes' vertices, 8 px
    // absorb, Ctrl off.  What was missing was any way to tell that it had happened:
    // an 8 px absorb with no accent reads as the cursor being ignored.  So every
    // engaged snap now leaves a MARK — the same glyph the 3D side uses (kiwi_snap.cpp
    // EmitDotAndRing: a filled dot with a SEPARATED ring, Plasticity's language), plus
    // the short tick ALONG the locked axis that a KIWI edge/midpoint snap carries, so
    // "it locked onto THIS line" reads without a legend.  The colour is bright rather
    // than the 3D pass's near-black (KSNAP_COL_MARK): this canvas is dark.
    enum { KUVE_SNAPK_GRID = 0, KUVE_SNAPK_VERT = 1 };

    struct uvSnapAcc_t { float u, v; int axis; };   // axis 0 = S locked -> VERTICAL tick
    const int KUVE_SNAP_ACC_MAX = 8;
    std::vector<uvSnapAcc_t> s_snapAcc;
    unsigned                 s_snapKinds = 0;       // bit per KUVE_SNAPK_*
    bool                     s_snapAngle = false;   // the rotate arm's edge-angle lock

    void SnapClear()
    {
        s_snapAcc.clear();
        s_snapKinds = 0;
        s_snapAngle = false;
    }

    void SnapNote( float u, float v, int axis, int kind )
    {
        s_snapKinds |= ( 1u << (unsigned)kind );
        if ( (int)s_snapAcc.size() >= KUVE_SNAP_ACC_MAX )
            return;
        uvSnapAcc_t a;
        a.u = u; a.v = v; a.axis = axis;
        s_snapAcc.push_back( a );
    }

    // One clause appended to the status readout, so the canvas says what it locked to.
    const char *SnapLabel()
    {
        if ( s_snapAngle )
            return "   [snapped: edge angle]";
        const bool g = ( s_snapKinds & ( 1u << KUVE_SNAPK_GRID ) ) != 0;
        const bool v = ( s_snapKinds & ( 1u << KUVE_SNAPK_VERT ) ) != 0;
        if ( g && v ) return "   [snapped: grid + vertex]";
        if ( g )      return "   [snapped: grid]";
        if ( v )      return "   [snapped: vertex]";
        return "";
    }

    // TB UvViewHelper::snapDelta (:198-209): per axis, |distance| < 8 px ⇒ absorb onto the
    // target, else ROUND the delta.  TB rounds to whole TEXELS because its UV coords are
    // texels; ours are REPEATS, so the same rule is `round(delta·texSize)/texSize`.
    // ROUND BJ, ITEM 6: `outAbsorbed` reports the ABSORB arm — the one that is felt as
    // magnetism (the texel rounding is sub-pixel at any sane zoom and is not marked).
    float SnapDeltaAxis( float delta, float distance, float texSize,
                         bool *outAbsorbed = nullptr )
    {
        if ( outAbsorbed )
            *outAbsorbed = false;
        if ( fabsf( distance ) * s_zoom < KUVE_SNAP_PX )
        {
            if ( outAbsorbed )
                *outAbsorbed = true;
            return delta + distance;
        }
        if ( texSize < 1.0f )
            texSize = 1.0f;
        return floorf( delta * texSize + 0.5f ) / texSize;
    }

    // ── KIWI-UX (ROUND BI): ONE CANDIDATE SET, SHARED BY MOVE AND SCALE (D-BI-F) ─────
    // The signed distance from `value` to the nearest snap candidate on `axis`: the grid
    // lines of the current subdivision, and every displayed vertex of the shapes that are
    // NOT being dragged.  (Whole TEXELS are the fallback quantum inside SnapDeltaAxis, TB's
    // own rule.)  BG read these from the ACTIVE face, which need not be in the target set
    // at all now — and a shape cannot usefully snap to itself.
    float AxisStripe( int axis ) { return axis ? StripeV() : StripeU(); }

    float SnapCandidateDist( float value, int axis, int *outKind = nullptr )
    {
        const float st = AxisStripe( axis );
        float best = floorf( value / st + 0.5f ) * st - value;
        int   kind = KUVE_SNAPK_GRID;
        for ( size_t i = 0; i < s_gOtherPts.size(); ++i )
        {
            const float c = axis ? s_gOtherPts[i].v : s_gOtherPts[i].u;
            const float d = c - value;
            if ( fabsf( d ) < fabsf( best ) )
            {
                best = d;
                kind = KUVE_SNAPK_VERT;
            }
        }
        if ( outKind )
            *outKind = kind;
        return best;
    }

    // TB UvOffsetTool::snapDelta (:56-75) with the vertex set widened as above: the
    // componentwise abs-min distance from every DRAGGED vertex, in its hypothetical
    // post-drag position, to the nearest candidate.
    // ROUND BJ, ITEM 6: it also remembers WHICH dragged vertex won each axis, so the
    // accent can be drawn on the vertex that actually stuck rather than at the cursor.
    void SnapMoveDelta( float dU, float dV, float texW, float texH,
                        float *outU, float *outV )
    {
        float best[2]     = { 1.0e30f, 1.0e30f };
        int   bestIdx[2]  = { -1, -1 };
        int   bestKind[2] = { KUVE_SNAPK_GRID, KUVE_SNAPK_GRID };
        for ( size_t i = 0; i < s_gTgtPts.size(); ++i )
        {
            const float p[2] = { s_gTgtPts[i].u + dU, s_gTgtPts[i].v + dV };
            for ( int c = 0; c < 2; ++c )
            {
                int kind = KUVE_SNAPK_GRID;
                const float d = SnapCandidateDist( p[c], c, &kind );
                if ( fabsf( d ) < fabsf( best[c] ) )
                {
                    best[c]     = d;
                    bestIdx[c]  = (int)i;
                    bestKind[c] = kind;
                }
            }
        }
        if ( s_gTgtPts.empty() )
            best[0] = best[1] = 0.0f;
        bool absorbed[2] = { false, false };
        *outU = SnapDeltaAxis( dU, best[0], texW, &absorbed[0] );
        *outV = SnapDeltaAxis( dV, best[1], texH, &absorbed[1] );
        for ( int c = 0; c < 2; ++c )
            if ( absorbed[c] && bestIdx[c] >= 0 )
                SnapNote( s_gTgtPts[bestIdx[c]].u + *outU,
                          s_gTgtPts[bestIdx[c]].v + *outV, c, bestKind[c] );
    }

    // TB UvScaleTool::snap (:103-127): pull the moved handle onto a candidate when it is
    // within 8 px on that axis.
    float SnapHandleAxis( float value, int axis,
                          bool *outHit = nullptr, int *outKind = nullptr )
    {
        if ( outHit )
            *outHit = false;
        int kind = KUVE_SNAPK_GRID;
        const float d = SnapCandidateDist( value, axis, &kind );
        if ( fabsf( d ) * s_zoom <= KUVE_SNAP_PX )
        {
            if ( outHit )  *outHit  = true;
            if ( outKind ) *outKind = kind;
            return value + d;
        }
        return value;
    }

    float NormDeg( float a )
    {
        while ( a >= 180.0f ) a -= 360.0f;
        while ( a < -180.0f ) a += 360.0f;
        return a;
    }

    // TB UvRotateTool::snapAngle (:72-108), re-expressed for a DELTA angle.  TB snaps the
    // ABSOLUTE texdef rotation against every face-edge angle at all four 90° phases; our
    // gesture carries a delta θ, so the candidates are the θ that make an edge square:
    // θ = −edgeAngle + 90k.  Threshold is TB's own trial-and-error 150/pow(d,0.8), with
    // the distance already in PIXELS (a 2D canvas has no world-unit-per-pixel step, so
    // TB's extra /zoom is gone — see D-BD-A).
    // ROUND BI: the edges are the TARGET SET's displayed outlines, captured at the press
    // (D-BI-F) — the shapes the rotation is actually turning.
    float SnapAngle( float theta, float distPx )
    {
        if ( s_gTgtEdges.size() < 1 )
            return theta;
        float best = 0.0f;
        float bestDelta = 1.0e30f;
        for ( size_t i = 0; i < s_gTgtEdges.size(); ++i )
        {
            const float ex = s_gTgtEdges[i].u1 - s_gTgtEdges[i].u0;
            const float ey = s_gTgtEdges[i].v1 - s_gTgtEdges[i].v0;
            if ( fabsf( ex ) < KUVE_EPS && fabsf( ey ) < KUVE_EPS )
                continue;
            const float edge = atan2f( ey, ex ) * 180.0f / KUVE_PI;
            for ( int k = 0; k < 4; ++k )
            {
                const float cand = theta + NormDeg( -edge + 90.0f * (float)k - theta );
                const float d    = fabsf( cand - theta );
                if ( d < bestDelta )
                {
                    bestDelta = d;
                    best      = cand;
                }
            }
        }
        if ( distPx < 1.0f )
            distPx = 1.0f;
        const float threshold = 150.0f / powf( distPx, 0.8f );
        return ( bestDelta < threshold ) ? best : theta;
    }

    // TB UvShearTool::snapShearFactors (:85-112): the candidate factors are the ones that
    // make an edge axis-aligned, i.e. −v[1−axis]/v[axis].  Sign-independent of the drag, so
    // the formula is TB's verbatim even though our factor sign is flipped (D-BD-A).
    // Threshold 10/|orthogonalOffset|, in pixels.  ROUND BI: same captured edge set.
    float SnapShear( float factor, float orthoOffsetUv, int axis )
    {
        float best = 0.0f;
        float bestDelta = 1.0e30f;
        for ( size_t i = 0; i < s_gTgtEdges.size(); ++i )
        {
            const float v[2] = { s_gTgtEdges[i].u1 - s_gTgtEdges[i].u0,
                                 s_gTgtEdges[i].v1 - s_gTgtEdges[i].v0 };
            if ( fabsf( v[axis] ) < KUVE_EPS )
                continue;
            const float cand = -v[1 - axis] / v[axis];
            const float d    = fabsf( cand - factor );
            if ( d < bestDelta ) { bestDelta = d; best = cand; }
        }
        float px = fabsf( orthoOffsetUv ) * s_zoom;
        if ( px < 1.0f )
            px = 1.0f;
        return ( bestDelta < 10.0f / px ) ? best : factor;
    }

    // TB UvOriginTool::snapDelta (:97-157) with its own KNOWN BUG fixed.  TB takes a
    // componentwise abs_min over vertex COORDINATES, which snaps to a point that is on no
    // vertex and no edge (its comment at :127-129 says so).  This snaps to the nearest
    // candidate POINT in 2D and only when that point is inside the 8 px radius.
    void SnapOrigin( float *u, float *v )
    {
        float bestU = *u, bestV = *v;
        float bestD = KUVE_SNAP_PX;
        const float cu = *u, cv = *v;

        const struct { float u, v; } grid = { floorf( cu / StripeU() + 0.5f ) * StripeU(),
                                              floorf( cv / StripeV() + 0.5f ) * StripeV() };
        {
            const float du = ( grid.u - cu ) * s_zoom, dv = ( grid.v - cv ) * s_zoom;
            const float d  = sqrtf( du * du + dv * dv );
            if ( d < bestD ) { bestD = d; bestU = grid.u; bestV = grid.v; }
        }

        // ── KIWI-UX (ROUND BJ, ITEM 6): the pivot snaps to the SHAPES ON SCREEN ──
        // BD/BG read these from the ACTIVE face's own ST frame, which since round BI
        // need not be a targeted shape and, with the chain on, is not even drawn where
        // its STs say (D-BG-A).  So a pivot dropped "on that corner" landed somewhere
        // else.  The candidates are the same DISPLAYED-space point sets every other
        // gesture uses (D-BI-F), captured at the press — both halves, because the pivot
        // belongs to no shape and every vertex on the canvas is a fair target for it.
        const std::vector<uvPt_t> *sets[2] = { &s_gTgtPts, &s_gOtherPts };
        float ctr[2] = { 0.0f, 0.0f };
        int   nctr = 0;
        for ( int s = 0; s < 2; ++s )
            for ( size_t i = 0; i < sets[s]->size(); ++i )
            {
                const uvPt_t &p = (*sets[s])[i];
                const float du = ( p.u - cu ) * s_zoom, dv = ( p.v - cv ) * s_zoom;
                const float d  = sqrtf( du * du + dv * dv );
                if ( d < bestD ) { bestD = d; bestU = p.u; bestV = p.v; }
                if ( s == 0 ) { ctr[0] += p.u; ctr[1] += p.v; ++nctr; }
            }
        // ...plus the TARGET SET's own centroid, which is the pivot placement people
        // actually reach for and which no vertex would offer.
        if ( nctr > 0 )
        {
            ctr[0] /= (float)nctr;
            ctr[1] /= (float)nctr;
            const float du = ( ctr[0] - cu ) * s_zoom, dv = ( ctr[1] - cv ) * s_zoom;
            const float d  = sqrtf( du * du + dv * dv );
            if ( d < bestD ) { bestD = d; bestU = ctr[0]; bestV = ctr[1]; }
        }
        *u = bestU;
        *v = bestV;
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  GESTURE LIFECYCLE
    // ═══════════════════════════════════════════════════════════════════════════════
    void SnapshotSelection()
    {
        s_snapFaces.clear();
        s_snapPatches.clear();
        for ( size_t i = 0; i < s_faces.size(); ++i )
        {
            MaterialDef  *md = nullptr;
            texdef_sub_t *td = KiwiUv_FaceTexdef( s_brushes[s_faces[i].brushIdx].def,
                                                  s_faces[i].faceIndex, &md );
            uvSnapFace_t sf;
            sf.def       = s_brushes[s_faces[i].brushIdx].def;
            sf.faceIndex = s_faces[i].faceIndex;
            if ( td )
                sf.td = *td;
            else
                memset( &sf.td, 0, sizeof( sf.td ) );
            s_snapFaces.push_back( sf );
        }
        for ( size_t i = 0; i < s_patches.size(); ++i )
        {
            patchMesh_t *p = s_brushes[s_patches[i]].pm;
            if ( !p )
                continue;
            uvSnapPatch_t sp;
            sp.pm = p;
            SnapshotPatch( p, sp.st );
            s_snapPatches.push_back( sp );
        }
    }

    // ── KIWI-UX (ROUND BI): the snap candidate sets, captured ONCE at the press ──────
    // D-BI-F: during the drag the STs these would be read from are exactly what the drag
    // is rewriting, so reading them live would make every snap chase its own tail.  Both
    // sets are DISPLAYED-space (the frame the gesture is expressed in) and both are capped.
    void CaptureSnapSets()
    {
        s_gTgtPts.clear();
        s_gOtherPts.clear();
        s_gTgtEdges.clear();
        SnapClear();                         // ROUND BJ, ITEM 6 - the accents die with it

        float poly[KUVE_MAX_WINDING][2];
        for ( size_t i = 0; i < s_faces.size(); ++i )
        {
            const int n = FaceStPointsDisp( i, poly, nullptr );
            if ( n < 2 )
                continue;
            const bool tgt = FaceIsTarget( i );
            for ( int k = 0; k < n; ++k )
            {
                uvPt_t p;
                p.u = poly[k][0];
                p.v = poly[k][1];
                if ( tgt )
                {
                    if ( (int)s_gTgtPts.size() < KUVE_SNAP_MAX_PTS )
                        s_gTgtPts.push_back( p );
                }
                else if ( (int)s_gOtherPts.size() < KUVE_SNAP_MAX_PTS )
                    s_gOtherPts.push_back( p );
            }
            if ( !tgt )
                continue;
            for ( int k = 0; k < n && (int)s_gTgtEdges.size() < KUVE_SNAP_MAX_PTS; ++k )
            {
                const int  j = ( k + 1 ) % n;
                uvEdge_t   e;
                e.u0 = poly[k][0]; e.v0 = poly[k][1];
                e.u1 = poly[j][0]; e.v1 = poly[j][1];
                s_gTgtEdges.push_back( e );
            }
        }
        for ( size_t pi = 0; pi < s_patches.size(); ++pi )
        {
            const int n = PatchRing( pi, poly );
            if ( n < 2 )
                continue;
            const bool tgt = PatchIsTarget( pi );
            for ( int k = 0; k < n; ++k )
            {
                uvPt_t p;
                p.u = poly[k][0];
                p.v = poly[k][1];
                if ( tgt )
                {
                    if ( (int)s_gTgtPts.size() < KUVE_SNAP_MAX_PTS )
                        s_gTgtPts.push_back( p );
                }
                else if ( (int)s_gOtherPts.size() < KUVE_SNAP_MAX_PTS )
                    s_gOtherPts.push_back( p );
            }
            if ( !tgt )
                continue;
            for ( int k = 0; k < n && (int)s_gTgtEdges.size() < KUVE_SNAP_MAX_PTS; ++k )
            {
                const int  j = ( k + 1 ) % n;
                uvEdge_t   e;
                e.u0 = poly[k][0]; e.v0 = poly[k][1];
                e.u1 = poly[j][0]; e.v1 = poly[j][1];
                s_gTgtEdges.push_back( e );
            }
        }
    }

    // The snapshot is keyed on (def, faceIndex) / patchMesh_t*, and the gather runs FRESH
    // every frame, so a selection that changed under the drag is DETECTED here rather than
    // dereferenced.  Returns false when the gesture can no longer be applied.
    bool SnapshotStillMatches()
    {
        if ( s_snapFaces.size() != s_faces.size() )
            return false;
        for ( size_t i = 0; i < s_faces.size(); ++i )
            if ( s_snapFaces[i].def != s_brushes[s_faces[i].brushIdx].def
                 || s_snapFaces[i].faceIndex != s_faces[i].faceIndex )
                return false;
        if ( s_snapPatches.size() != s_patches.size() )
            return false;
        for ( size_t i = 0; i < s_patches.size(); ++i )
            if ( s_snapPatches[i].pm != s_brushes[s_patches[i]].pm )
                return false;
        return true;
    }

    // Write the whole selection from the snapshot through `A`.  `offsetDelta` non-null
    // selects the EXACT translation route for faces (D-BD-C); patches always go through
    // the affine, which for a translation is the same arithmetic.
    void ApplySelection( const uvXform_t &A, const float *offsetDelta, const char *undoName )
    {
        if ( !SnapshotStillMatches() )
            return;
        // An identity transform is skipped ONLY while nothing has been written yet — a
        // bracket around a no-op is what kiwi_command.h forbids.  Once the gesture HAS
        // mutated, identity must still be applied: it is the frame where the user dragged
        // back to where they started, and the write puts the snapshot back.  (Returning
        // early there would strand the previous frame's value on the selection.)
        if ( XfIsIdentity( A ) && !s_undoOpen )
            return;

        UndoOpen( undoName );

        for ( size_t i = 0; i < s_faces.size(); ++i )
        {
            if ( !FaceIsTarget( i ) )        // ROUND BG, item 6: the apply loop is the
                continue;                    // ONLY thing per-shape targeting changes
            face_t *fd = FaceOf( s_faces[i] );
            if ( !fd )
                continue;
            MaterialDef  *md = nullptr;
            texdef_sub_t *td = KiwiUv_FaceTexdef( s_brushes[s_faces[i].brushIdx].def,
                                                  s_faces[i].faceIndex, &md );
            if ( !td || !md )
                continue;
            texdef_sub_t out = s_snapFaces[i].td;
            // ROUND BG, D-BG-A: `A` is expressed in DISPLAYED space, so it is conjugated
            // by this face's display transform before it may touch a texdef.  Identity D
            // (the active face, or the chain off) short-circuits to BD's exact arithmetic.
            const uvXform_t D = ( i < s_faceD.size() ) ? s_faceD[i] : XfIdentity();
            if ( offsetDelta )
            {
                // The vector case of the same conjugation: D⁻¹'s LINEAR part on the delta.
                float dU = offsetDelta[0], dV = offsetDelta[1];
                if ( !XfIsIdentity( D ) )
                    XfApplyVec( XfInverse( D ), offsetDelta[0], offsetDelta[1], &dU, &dV );
                OffsetFace( &s_snapFaces[i].td, dU, dV, &out );
            }
            else
            {
                TransformFace( fd, &s_snapFaces[i].td, XfConjugate( A, D ), &out );
            }
            td->size[0]   = out.size[0];
            td->size[1]   = out.size[1];
            td->shift[0]  = out.shift[0];
            td->shift[1]  = out.shift[1];
            td->rotate    = out.rotate;
            td->crossterm = out.crossterm;
            TexMatToFakeTexCoords( md, td );     // every ported writer's tail
        }

        for ( size_t i = 0; i < s_snapPatches.size(); ++i )
        {
            if ( !PatchIsTarget( i ) )
                continue;
            const uvXform_t D = ( i < s_patchD.size() ) ? s_patchD[i] : XfIdentity();
            TransformPatch( s_snapPatches[i].pm, s_snapPatches[i].st, XfConjugate( A, D ) );
        }

        // ONE rebuild per brush, not one per face: Brush_ShiftTexture's face pass rebuilds
        // per face because it walks faces, but the sequence is per-brush work and a drag
        // over a 40-brush selection would otherwise pay for it six times over.  PATCH rows
        // get it too — Brush_ShiftTexture's own brush loop is unconditional (select.cpp:
        // 3095-3136), and Patch_ShiftTexture's rebuild covers the mesh, not the brush.
        for ( size_t i = 0; i < s_brushes.size(); ++i )
            TouchBrush( s_brushes[i].node, s_brushes[i].def );

        g_nUpdateBits = -1;
    }

    // Put the selection back exactly where the gesture found it.  EXACT (it is a stored
    // copy), unlike kiwi_uv.cpp's inverse-delta leg, which its own header calls out as
    // non-bit-exact (kiwi_uv.h:83-86).
    void RestoreSnapshot()
    {
        if ( !SnapshotStillMatches() )
            return;
        for ( size_t i = 0; i < s_faces.size(); ++i )
        {
            MaterialDef  *md = nullptr;
            texdef_sub_t *td = KiwiUv_FaceTexdef( s_brushes[s_faces[i].brushIdx].def,
                                                  s_faces[i].faceIndex, &md );
            if ( !td || !md )
                continue;
            *td = s_snapFaces[i].td;
            TexMatToFakeTexCoords( md, td );
        }
        for ( size_t i = 0; i < s_snapPatches.size(); ++i )
            TransformPatch( s_snapPatches[i].pm, s_snapPatches[i].st, XfIdentity() );
        for ( size_t i = 0; i < s_brushes.size(); ++i )
            TouchBrush( s_brushes[i].node, s_brushes[i].def );
        g_nUpdateBits = -1;
    }

    // ── KIWI-UX (ROUND BI): the marquee's payload (D-BI-C) ──────────────────────────
    // Every shape whose DISPLAYED outline intersects the band becomes the target set;
    // Shift extends the existing one.  A band that catches nothing restores "all" — the
    // same way out an empty click gives, because a target set of nothing is a canvas where
    // no gesture does anything.
    void ApplyMarquee( float u0, float v0, float u1, float v1, bool extend )
    {
        float r0[2] = { u0 < u1 ? u0 : u1, v0 < v1 ? v0 : v1 };
        float r1[2] = { u0 < u1 ? u1 : u0, v0 < v1 ? v1 : v0 };

        std::vector<char> ft( s_faces.size(),   0 );
        std::vector<char> pt( s_patches.size(), 0 );
        if ( extend && !s_targetAll )
        {
            for ( size_t i = 0; i < ft.size() && i < s_faceTgt.size();  ++i ) ft[i] = s_faceTgt[i];
            for ( size_t i = 0; i < pt.size() && i < s_patchTgt.size(); ++i ) pt[i] = s_patchTgt[i];
        }

        int hits = 0;
        float poly[KUVE_MAX_WINDING][2];
        for ( size_t i = 0; i < s_faces.size(); ++i )
        {
            const int n = FaceStPointsDisp( i, poly, nullptr );
            if ( n >= 2 && PolyHitsRect( poly, n, r0, r1 ) )
            {
                ft[i] = 1;
                ++hits;
            }
        }
        for ( size_t pi = 0; pi < s_patches.size(); ++pi )
        {
            const int n = PatchRing( pi, poly );
            if ( n >= 2 && PolyHitsRect( poly, n, r0, r1 ) )
            {
                pt[pi] = 1;
                ++hits;
            }
        }

        int total = 0;
        for ( size_t i = 0; i < ft.size(); ++i ) total += ft[i];
        for ( size_t i = 0; i < pt.size(); ++i ) total += pt[i];
        if ( !hits && !extend )
        {
            TargetAll();
            _snprintf( s_status, sizeof( s_status ), "targeting all shapes" );
        }
        else if ( total < 1 )
        {
            TargetAll();
            _snprintf( s_status, sizeof( s_status ), "targeting all shapes" );
        }
        else
        {
            s_targetAll = false;
            s_faceTgt   = ft;
            s_patchTgt  = pt;
            StoreTargetKeys();
            _snprintf( s_status, sizeof( s_status ), "targeting %d shape%s",
                       total, total == 1 ? "" : "s" );
        }
        s_status[sizeof( s_status ) - 1] = '\0';
        s_cycleValid = false;                 // a marquee is not a point in the click cycle
    }

    void GestureEnd()
    {
        // ── KIWI-UX (ROUND BI): the two releases that mean something ────────────────
        if ( s_gesture == UVG_MARQUEE )
        {
            const float dx = ( s_gCurU - s_gStartU ) * s_zoom;
            const float dy = ( s_gCurV - s_gStartV ) * s_zoom;
            const bool  extend = s_gShift;
            if ( fabsf( dx ) < KUVE_MARQUEE_MIN_PX && fabsf( dy ) < KUVE_MARQUEE_MIN_PX )
            {
                // A click, not a band: the way back to "all" (D-BI-C).
                if ( !extend )
                {
                    if ( !s_targetAll )
                        _snprintf( s_status, sizeof( s_status ), "targeting all shapes" );
                    TargetAll();
                }
            }
            else
                ApplyMarquee( s_gStartU, s_gStartV, s_gCurU, s_gCurV, extend );
        }
        else if ( s_gesture == UVG_MOVE && !s_gOnShape )
        {
            // A press on the box's empty interior that never moved is the same "restore
            // all" click — it is the only empty space a sub-selection leaves reachable.
            const float dx = ( s_gCurU - s_gStartU ) * s_zoom;
            const float dy = ( s_gCurV - s_gStartV ) * s_zoom;
            if ( fabsf( dx ) < KUVE_MARQUEE_MIN_PX && fabsf( dy ) < KUVE_MARQUEE_MIN_PX
                 && !s_gShift )
            {
                if ( !s_targetAll )
                    _snprintf( s_status, sizeof( s_status ), "targeting all shapes" );
                TargetAll();
            }
        }
        else if ( s_gesture == UVG_MOVE && s_gOnShape && s_collapseArmed )
        {
            // ══════════════════════════════════════════════════════════════════
            //  KIWI-UX (ROUND BO, ITEM 2) — THE DEFERRED COLLAPSE FIRES HERE
            // ══════════════════════════════════════════════════════════════════
            // The press kept the whole target set (see the arm in GestureBegin) so
            // the DRAG could move it as a unit.  If the cursor never left the click
            // box then it was not a drag at all, it was a CLICK on one of several
            // targets — and the universal convention for that is "select just this
            // one, on the release".  Same threshold as the marquee and empty-box
            // arms above, so the canvas has one definition of a drag.
            //
            // Shift is excluded: a Shift press never reaches this arm (it returns
            // from GestureBegin with the gesture cleared), and testing it here keeps
            // the arm honest if that ever changes.
            const float dx = ( s_gCurU - s_gStartU ) * s_zoom;
            const float dy = ( s_gCurV - s_gStartV ) * s_zoom;
            if ( fabsf( dx ) < KUVE_MARQUEE_MIN_PX && fabsf( dy ) < KUVE_MARQUEE_MIN_PX
                 && !s_gShift )
            {
                TargetSingle( s_collapseFace, s_collapsePatch );
                s_cycleNext  = s_collapseNext;   // the next click here steps the stack
                s_cycleValid = true;
                AutoPivot();                     // D-BI-D: the pivot follows the set
                _snprintf( s_status, sizeof( s_status ), "targeting 1 shape" );
                s_status[sizeof( s_status ) - 1] = '\0';
            }
        }
        else if ( s_gesture == UVG_ORIGIN )
        {
            // D-BI-D: a pivot the user actually MOVED stops following the target set.
            if ( fabsf( s_originU - s_gOriginU ) * s_zoom > 0.5f
                 || fabsf( s_originV - s_gOriginV ) * s_zoom > 0.5f )
                s_originUser = true;
        }

        UndoCommit();
        s_collapseArmed = false;      // KIWI-UX (ROUND BO, ITEM 2)
        s_gesture = UVG_NONE;
        s_gButton = -1;
        s_gHandle = UVH_NONE;
        s_snapFaces.clear();
        s_snapPatches.clear();
        s_gTgtPts.clear();
        s_gOtherPts.clear();
        s_gTgtEdges.clear();
        SnapClear();                         // ROUND BJ, ITEM 6 - the accents die with it
    }

    void GestureCancel()
    {
        if ( s_gesture == UVG_NONE )
            return;
        if ( s_gesture == UVG_ORIGIN )
        {
            s_originU = s_gOriginU;              // view state only — nothing was mutated
            s_originV = s_gOriginV;
        }
        else if ( s_gesture != UVG_PAN && s_gesture != UVG_MARQUEE )
        {                                        // ROUND BI: a marquee mutates nothing
            RestoreSnapshot();
            // Belt AND braces, the kiwi_uv.cpp:345-353 shape: the restore above is exact,
            // and KiwiCmd_UndoCancel then rolls the bracket back so the cancelled gesture
            // leaves nothing in either direction (kiwi_command.cpp:2338-2361).
            if ( s_undoOpen )
            {
                KiwiCmd_UndoCancel();
                s_undoOpen = false;
                s_covered  = false;
            }
        }
        s_collapseArmed = false;      // KIWI-UX (ROUND BO, ITEM 2)
        s_gesture = UVG_NONE;
        s_gButton = -1;
        s_gHandle = UVH_NONE;
        s_snapFaces.clear();
        s_snapPatches.clear();
        s_gTgtPts.clear();
        s_gOtherPts.clear();
        s_gTgtEdges.clear();
        SnapClear();                         // ROUND BJ, ITEM 6 - the accents die with it
        _snprintf( s_status, sizeof( s_status ), "cancelled" );
        s_status[sizeof( s_status ) - 1] = '\0';
        g_nUpdateBits = -1;
    }

    // Drop a live gesture WITHOUT re-reading the selection.  Used on the paths where the
    // window did not run Gather() this frame (closed, or collapsed): `s_brushes` there is
    // last frame's gather, so its `brush_t*` are stored pointers and RestoreSnapshot would
    // dereference them.  The undo bracket is the authoritative restore anyway — this is
    // exactly the belt half of GestureCancel with the braces left off, and it is the ONE
    // thing that must not be skipped, because an open bracket may never survive a frame.
    void GestureAbandon()
    {
        if ( s_gesture == UVG_NONE )
            return;
        if ( s_undoOpen )
        {
            KiwiCmd_UndoCancel();
            s_undoOpen = false;
            s_covered  = false;
        }
        if ( s_gesture == UVG_ORIGIN )
        {
            s_originU = s_gOriginU;
            s_originV = s_gOriginV;
        }
        s_collapseArmed = false;      // KIWI-UX (ROUND BO, ITEM 2)
        s_gesture = UVG_NONE;
        s_gButton = -1;
        s_gHandle = UVH_NONE;
        s_snapFaces.clear();
        s_snapPatches.clear();
        s_gTgtPts.clear();
        s_gOtherPts.clear();
        s_gTgtEdges.clear();
        SnapClear();                         // ROUND BJ, ITEM 6 - the accents die with it
        g_nUpdateBits = -1;
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BI): THE PRESS.  ONE Probe() ANSWER, ACTED ON (D-BI-B/C)
    // ═══════════════════════════════════════════════════════════════════════════════
    // TB's ToolChain order (Rotate ring → Origin → Scale/Shear on a grid line → Offset)
    // is GONE with the tools that were on the grid.  What remains is the standard 2D
    // editor's: pivot → the target set's own handles → the shapes → the box body →
    // marquee, all of it resolved by the same probe the hover ran this frame.
    // Ctrl is snapping-only; it starts no gesture of its own.

    // Make ONE shape the whole target set (D-BI-C).  Never touches the 3D selection.
    void TargetSingle( int face, int patch )
    {
        s_targetAll = false;
        s_faceTgt.assign( s_faces.size(), 0 );
        s_patchTgt.assign( s_patches.size(), 0 );
        if ( face >= 0 && (size_t)face < s_faceTgt.size() )
            s_faceTgt[face] = 1;
        else if ( patch >= 0 && (size_t)patch < s_patchTgt.size() )
            s_patchTgt[patch] = 1;
        StoreTargetKeys();
    }

    void GestureBegin( int button, float u, float v )
    {
        ImGuiIO &io = ImGui::GetIO();

        // KIWI-UX (ROUND BO, ITEM 2): a new gesture never inherits the last one's
        // armed collapse.  FIRST thing, above every early return, so the pan arm
        // and the "not the left button" refusal disarm it too.
        s_collapseArmed = false;

        if ( button == ImGuiMouseButton_Right || button == ImGuiMouseButton_Middle )
        {
            s_gesture = UVG_PAN;
            s_gButton = button;
            s_gStartU = s_gCurU = u;
            s_gStartV = s_gCurV = v;
            return;
        }
        if ( button != ImGuiMouseButton_Left || !AnythingSelected() )
            return;

        // D-BG-E, kept and now load-bearing: the SAME probe the hover ran this frame, so
        // what lit up under the cursor is exactly what this press grabs.
        const uvProbe_t pr = Probe( u, v, io.KeyAlt );

        s_gStartU  = s_gCurU = u;
        s_gStartV  = s_gCurV = v;
        s_gOriginU = s_originU;
        s_gOriginV = s_originV;
        s_gAxis[0] = pr.axis[0];
        s_gAxis[1] = pr.axis[1];
        s_gHandle  = pr.handle;
        s_gOnShape = ( pr.shapeFace >= 0 || pr.shapePatch >= 0 );
        s_gShift   = io.KeyShift;
        s_gButton  = button;
        s_gHandleU = u;
        s_gHandleV = v;
        s_gAnchorU = s_originU;          // ROUND BJ, ITEM 5 — pivot unless a scale
        s_gAnchorV = s_originV;          //                    overwrites it below
        if ( pr.handle != UVH_NONE && s_box.valid )
        {
            // Scale and skew measure from the HANDLE, not from the press point: the press
            // may be up to KUVE_HANDLE_PICK_PX off centre and that offset would otherwise
            // be baked into the very first frame's factor.  From the DEFLATED box, so the
            // point that tracks the cursor is on the shapes rather than on the inflation.
            const uvBox_t raw = DeflateBox( s_box );
            float hx, hy;
            HandlePx( raw, pr.handle, &hx, &hy );
            PxToUv( ImVec2( hx, hy ), &s_gHandleU, &s_gHandleV );
            // ROUND BJ, ITEM 5: …and the opposite handle is the anchor a SCALE holds
            // fixed.  SKEW keeps the pivot (D-BJ-E), so this is only read by UVG_SCALE.
            const int opp = OppositeHandle( pr.handle );
            if ( opp != UVH_NONE )
            {
                float ax, ay;
                HandlePx( raw, opp, &ax, &ay );
                PxToUv( ImVec2( ax, ay ), &s_gAnchorU, &s_gAnchorV );
            }
        }

        if ( pr.shearRefused )
        {
            s_gesture  = UVG_NONE;
            s_gButton  = -1;
            s_gHandle  = UVH_NONE;
            s_gAxis[0] = s_gAxis[1] = false;
            _snprintf( s_status, sizeof( s_status ),
                       "skew refused: that handle is level with the pivot (move the pivot)" );
            s_status[sizeof( s_status ) - 1] = '\0';
            return;
        }

        switch ( pr.what )
        {
        case UVG_ORIGIN:
            s_gesture = UVG_ORIGIN;          // view state only, never persisted
            // ROUND BJ, ITEM 6: the pivot snaps against the same captured displayed
            // point sets every other gesture uses (SnapOrigin) — so it has to capture
            // them.  No SnapshotSelection: nothing is mutated by a pivot drag.
            CaptureSnapSets();
            return;
        case UVG_ROTATE:
            s_gesture     = UVG_ROTATE;
            s_gStartAngle = atan2f( v - s_originV, u - s_originU ) * 180.0f / KUVE_PI;
            SnapshotSelection();
            CaptureSnapSets();
            return;
        case UVG_SCALE:
        case UVG_SHEAR:
            s_gesture = pr.what;
            SnapshotSelection();
            CaptureSnapSets();
            return;
        case UVG_MARQUEE:
            s_gesture = UVG_MARQUEE;         // nothing is mutated until the release
            return;
        default:
            break;                           // UVG_MOVE
        }

        // ── MOVE, AND WITH IT THE TARGETING (D-BI-C) ────────────────────────────────
        // A press on a shape targets it and drags it in ONE press (BG's rule).  What is
        // new: repeated presses at the same spot walk DOWN the stack under the cursor,
        // and a press on a shape that is already one of SEVERAL targets moves the whole
        // set instead of collapsing it — the collapse is the next press at that spot,
        // which starts the cycle at the topmost shape (s_cycleNext = 0 below).
        if ( s_gOnShape )
        {
            uvShapeRef_t stack[64];
            const int    n      = CollectShapesAt( u, v, stack, 64 );
            const int    k      = CycleIndexFor( u, v, n );
            const bool   repeat = CycleIsRepeat( u, v );

            if ( io.KeyShift )
            {
                // Shift toggles membership and starts nothing — a toggle that also dragged
                // would move the shape the user was only trying to add.  Out of "all" it
                // means "all EXCEPT this one", the rule every shift-toggle in this editor
                // follows (kiwi_boxselect ClickSelect).
                if ( s_targetAll )
                {
                    s_targetAll = false;
                    s_faceTgt.assign( s_faces.size(), 1 );
                    s_patchTgt.assign( s_patches.size(), 1 );
                }
                if ( pr.shapeFace >= 0 )
                    s_faceTgt[pr.shapeFace] = s_faceTgt[pr.shapeFace] ? 0 : 1;
                else
                    s_patchTgt[pr.shapePatch] = s_patchTgt[pr.shapePatch] ? 0 : 1;
                if ( TargetCount() < 1 )
                    TargetAll();
                else
                    StoreTargetKeys();
                s_cycleU     = u;
                s_cycleV     = v;
                s_cycleNext  = ( n > 0 ) ? ( k + 1 ) % n : 0;
                s_cycleValid = true;
                s_gesture    = UVG_NONE;
                s_gButton    = -1;
                _snprintf( s_status, sizeof( s_status ), "targeting %d shape%s",
                           TargetCount(), TargetCount() == 1 ? "" : "s" );
                s_status[sizeof( s_status ) - 1] = '\0';
                return;
            }

            // ══════════════════════════════════════════════════════════════════
            //  KIWI-UX (ROUND BO, ITEM 2) — THE `!s_targetAll` TERM WAS THE BUG
            // ══════════════════════════════════════════════════════════════════
            // USER RE-REPORT, verbatim: *"When selecting multiple faces, it still
            // does not truly act like multiple are selected in the UV editor.  They
            // are all highlighted, but I can't click-drag them as a cohesive unit
            // unless I shift-click them again in the UV editor."*
            //
            // AND THE `unless I shift-click them again` HALF IS THE WHOLE
            // DIAGNOSIS.  Round BN's seed does the right thing for the reported
            // flow and then names the state badly: when the explicit 3D faces cover
            // EVERY gathered shape it calls `TargetAll()` (:709), which sets
            // `s_targetAll = true`.  That is a REPRESENTATION of "all three are
            // targeted" — `s_faceTgt` is all 1s (:761-762), `TargetCount()` is 3 and
            // `pr.shapeIsTarget` is true — but this predicate treated it as "there
            // is no target set", so `keepSet` was false, `TargetSingle` ran, and the
            // gold three collapsed to one on the press.  Shift-clicking in the
            // canvas is the ONE path that turns "all" into an explicit set of the
            // same shapes (:3013-3018), after which `!s_targetAll` is true and the
            // drag worked — which is exactly, and only, what the user found.
            //
            // THE QUESTION THIS ARM ASKS has nothing to do with how the set is
            // stored: "is the shape under the press already targeted, and is there
            // more than one target?"  `pr.shapeIsTarget && TargetCount() > 1`
            // answers it in both representations, so the `!s_targetAll` term is
            // deleted rather than patched.
            const bool keepSet = ( !repeat && pr.shapeIsTarget && TargetCount() > 1 );
            if ( keepSet )
            {
                // ── THE DEFERRED COLLAPSE (the file-explorer nuance) ───────────
                // Pressing an already-targeted shape must not decide anything yet:
                // a DRAG means "move the whole set" and a CLICK means "collapse to
                // this one", and which it is cannot be known until the release.  So
                // the collapse is ARMED here and executed in GestureEnd only if the
                // cursor never left the click box.  This is the same press/release
                // split the marquee and the empty-box arms already use (:2762,
                // :2781), with the same threshold, so the canvas has one answer to
                // "did that count as a drag?"
                s_cycleNext      = 0;
                s_collapseArmed  = true;
                s_collapseFace   = pr.shapeFace;
                s_collapsePatch  = pr.shapePatch;
                s_collapseNext   = ( n > 0 ) ? ( k + 1 ) % n : 0;
                _snprintf( s_status, sizeof( s_status ), "moving %d targeted shape%s",
                           TargetCount(), TargetCount() == 1 ? "" : "s" );
                s_status[sizeof( s_status ) - 1] = '\0';
            }
            else
            {
                TargetSingle( pr.shapeFace, pr.shapePatch );
                s_cycleNext = ( n > 0 ) ? ( k + 1 ) % n : 0;
                _snprintf( s_status, sizeof( s_status ), "targeting 1 of %d shape%s here",
                           n, n == 1 ? "" : "s" );
                s_status[sizeof( s_status ) - 1] = '\0';
            }
            s_cycleU     = u;
            s_cycleV     = v;
            s_cycleValid = true;
        }

        // The retarget above may have changed the target set, and the pivot follows it
        // (D-BI-D) — re-derive it NOW rather than on the next frame, so a rotate or a
        // scale begun straight after this press pivots on what is actually targeted.
        // (AutoPivot is a no-op once the gesture below is live, and while the latch is on.)
        AutoPivot();

        s_gesture = UVG_MOVE;
        SnapshotSelection();
        CaptureSnapSets();               // AFTER the retarget: it partitions on the set
    }

    void GestureUpdate( float u, float v )
    {
        ImGuiIO &io = ImGui::GetIO();
        // ── KIWI-UX (ROUND BO, ITEM 3): THE CANVAS FOLLOWS THE TRANSFORM RULE ──
        // Every gesture on this canvas — move, pivot, rotate, scale, shear — is a
        // TRANSFORM: it moves UVs that already exist.  So it is RAW by default and
        // Ctrl engages the whole snap family (grid, whole texels, the other shapes'
        // vertices).  This is the same inversion the 3D side takes, written here
        // rather than through KiwiCmd_SnapEngaged because the canvas runs inside the
        // ImGui frame with no active KIWI command to ask — io.KeyCtrl IS the frame's
        // own authority for that (imgui_shell.cpp passes the same io to the
        // viewport arms).  ONE line, one meaning, and the header's "Ctrl is
        // snapping-only" note still holds: it starts no gesture of its own.
        const bool snap = io.KeyCtrl;
        s_gCurU = u;
        s_gCurV = v;

        switch ( s_gesture )
        {
        case UVG_PAN:
            // The cursor holds the UV point it grabbed: pan by the difference between the
            // UV now under the cursor and the one that was there at the press.  That is
            // TB UvCameraTool's unproject-old / unproject-new / moveBy(delta) (:44-55)
            // with the unprojects already done by the caller.
            s_panU += s_gStartU - u;
            s_panV += s_gStartV - v;
            break;
        case UVG_MARQUEE:
            // Nothing is mutated until the release (ApplyMarquee, from GestureEnd); the
            // band itself is drawn from s_gStart* / s_gCur* by the overlays.
            _snprintf( s_status, sizeof( s_status ), "marquee  %+.3f x %+.3f repeats",
                       u - s_gStartU, v - s_gStartV );
            break;
        case UVG_ORIGIN:
        {
            // ROUND BI: the pivot's axis LINES are gone, so a pivot drag is always free in
            // both axes (D-BI-D).  Its snap candidates are unchanged (SnapOrigin).
            float nu = s_gOriginU + ( u - s_gStartU );
            float nv = s_gOriginV + ( v - s_gStartV );
            SnapClear();                     // ROUND BJ, ITEM 6
            if ( snap )
            {
                const float ru = nu, rv = nv;
                SnapOrigin( &nu, &nv );
                if ( fabsf( nu - ru ) > 1.0e-6f || fabsf( nv - rv ) > 1.0e-6f )
                {
                    // A 2D point snap: mark BOTH axes so the accent reads as a cross
                    // through the point rather than as a single line lock.
                    SnapNote( nu, nv, 0, KUVE_SNAPK_VERT );
                    SnapNote( nu, nv, 1, KUVE_SNAPK_VERT );
                }
            }
            s_originU = nu;
            s_originV = nv;
            _snprintf( s_status, sizeof( s_status ), "pivot  %.4f, %.4f%s",
                       nu, nv, SnapLabel() );
            break;
        }
        case UVG_MOVE:
        {
            float dU = u - s_gStartU;
            float dV = v - s_gStartV;
            SnapClear();                     // ROUND BJ, ITEM 6
            if ( snap )
            {
                MaterialDef *md = nullptr;
                if ( s_activeFace >= 0 )
                    KiwiUv_FaceTexdef( s_brushes[s_faces[s_activeFace].brushIdx].def,
                                       s_faces[s_activeFace].faceIndex, &md );
                float tw, th;
                MaterialSize( md, &tw, &th );
                float su, sv;
                SnapMoveDelta( dU, dV, tw, th, &su, &sv );
                dU = su;
                dV = sv;
            }
            const float d[2] = { dU, dV };
            ApplySelection( XfTranslate( dU, dV ), d, "uv move" );
            _snprintf( s_status, sizeof( s_status ), "move  %+.4f, %+.4f repeats%s",
                       dU, dV, SnapLabel() );
            break;
        }
        case UVG_ROTATE:
        {
            const float cur = atan2f( v - s_originV, u - s_originU ) * 180.0f / KUVE_PI;
            float theta = NormDeg( cur - s_gStartAngle );
            const float du = ( u - s_originU ) * s_zoom, dv = ( v - s_originV ) * s_zoom;
            SnapClear();                     // ROUND BJ, ITEM 6
            if ( snap )
            {
                const float raw = theta;
                theta = SnapAngle( theta, sqrtf( du * du + dv * dv ) );
                // The rotate snap is an ANGLE, not a point, so it earns the readout
                // clause and no dot: a mark on the canvas would name a place that had
                // nothing to do with what locked.
                s_snapAngle = ( fabsf( theta - raw ) > 1.0e-4f );
            }
            const float r = theta * KUVE_PI / 180.0f;
            const float c = cosf( r ), s = sinf( r );
            ApplySelection( XfAboutOrigin( c, -s, s, c, s_originU, s_originV ),
                            nullptr, "uv rotate" );
            _snprintf( s_status, sizeof( s_status ), "rotate  %+.2f deg%s",
                       theta, SnapLabel() );
            break;
        }
        case UVG_SCALE:
        {
            // TB UvScaleTool (:196-254): the factor is the ratio of anchor-to-handle
            // distances, the handle having been moved by the drag delta.  ROUND BI: the
            // handle is a BOX handle rather than a grid line (D-BI-B) — same arithmetic,
            // and the corner case gains Shift = uniform.
            // ── KIWI-UX (ROUND BJ, ITEM 5): THE ANCHOR IS THE OPPOSITE SIDE ─────
            // USER REPORT, verbatim: *"When dragging the sides, dont expand both
            // sides, just expand the side that is being dragged."*  BI derived the
            // factor from the PIVOT, which sits at the box centre by default — so a
            // right-edge drag scaled about the centre and the left edge moved out by
            // the same amount.  `o` is the ANCHOR now: the handle across the box,
            // captured at the press (D-BJ-E).  The arithmetic is otherwise TB's
            // untouched, and with the anchor at the pivot (which is what the pivot
            // handle still is for rotate / skew / Rot 90 / Flip) it is bit-identical
            // to BI's.
            float k[2] = { 1.0f, 1.0f };
            const float o[2]  = { s_gAnchorU, s_gAnchorV };
            const float h0[2] = { s_gHandleU, s_gHandleV };
            float       h1[2] = { s_gHandleU + ( u - s_gStartU ),
                                  s_gHandleV + ( v - s_gStartV ) };
            SnapClear();
            if ( snap )
            {
                // ROUND BJ, ITEM 6: the thing the user is HOLDING is what snaps —
                // the dragged handle, per armed axis — and it says so on the canvas.
                bool hit[2]  = { false, false };
                int  kind[2] = { KUVE_SNAPK_GRID, KUVE_SNAPK_GRID };
                for ( int i = 0; i < 2; ++i )
                    if ( s_gAxis[i] )
                        h1[i] = SnapHandleAxis( h1[i], i, &hit[i], &kind[i] );
                // Noted AFTER both axes, so a corner's first accent is not placed at
                // the second axis's pre-snap value.
                for ( int i = 0; i < 2; ++i )
                    if ( hit[i] )
                        SnapNote( h1[0], h1[1], i, kind[i] );
            }
            const bool uniform = ( s_gHandle >= UVH_NW && s_gHandle <= UVH_SW )
                                 && io.KeyShift;
            if ( uniform )
            {
                // Aspect-locked: the least-squares scalar, i.e. the drag's component ALONG
                // the anchor-to-corner diagonal decides one factor for both axes.
                const float ox = h0[0] - o[0], oy = h0[1] - o[1];
                const float len2 = ox * ox + oy * oy;
                if ( len2 > KUVE_EPS )
                {
                    const float val = ( ( h1[0] - o[0] ) * ox + ( h1[1] - o[1] ) * oy ) / len2;
                    if ( fabsf( val ) > KUVE_EPS )
                        k[0] = k[1] = val;
                }
            }
            else
            {
                for ( int i = 0; i < 2; ++i )
                {
                    if ( !s_gAxis[i] )
                        continue;
                    const float den = h0[i] - o[i];
                    if ( fabsf( den ) < KUVE_EPS )
                        continue;
                    const float val = ( h1[i] - o[i] ) / den;
                    if ( fabsf( val ) < KUVE_EPS )       // TB: `value != 0.0f`
                        continue;
                    k[i] = val;
                }
            }
            ApplySelection( XfAboutOrigin( k[0], 0.0f, 0.0f, k[1], o[0], o[1] ),
                            nullptr, "uv scale" );
            _snprintf( s_status, sizeof( s_status ), "scale  x%.4f, x%.4f%s%s",
                       k[0], k[1], uniform ? "  (uniform)" : "", SnapLabel() );
            break;
        }
        case UVG_SHEAR:
        {
            // TB UvShearTool (:159-208): factors {−dy/x0, −dx/y0}, recomputed from the
            // DRAG START every frame (TB rolls the transaction back to get that; we get it
            // for free because every frame rebuilds from the snapshot).  The sign is
            // flipped from TB's — D-BD-A: our wireframe follows the cursor.  ROUND BI: the
            // reference offset is the grabbed EDGE-MID HANDLE's offset from the pivot, not
            // the press point's, so the shear is exact from the first frame.
            const float x0 = s_gHandleU - s_originU;
            const float y0 = s_gHandleV - s_originV;
            const float dx = u - s_gStartU;
            const float dy = v - s_gStartV;
            float f0 = 0.0f, f1 = 0.0f;         // f0: t += f0·s   f1: s += f1·t
            if ( s_gAxis[0] && fabsf( x0 ) > KUVE_EPS ) f0 =  dy / x0;
            if ( s_gAxis[1] && fabsf( y0 ) > KUVE_EPS ) f1 =  dx / y0;
            if ( snap )
            {
                if ( s_gAxis[0] ) f0 = SnapShear( f0, x0, 0 );
                if ( s_gAxis[1] ) f1 = SnapShear( f1, y0, 1 );
            }
            ApplySelection( XfAboutOrigin( 1.0f, f1, f0, 1.0f, s_originU, s_originV ),
                            nullptr, "uv skew" );
            _snprintf( s_status, sizeof( s_status ), "skew  %+.4f, %+.4f", f0, f1 );
            break;
        }
        default:
            break;
        }
        s_status[sizeof( s_status ) - 1] = '\0';
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  OVERLAYS
    // ═══════════════════════════════════════════════════════════════════════════════
    // KIWI-UX (ROUND BG, ITEM 3a / D-BG-E): the overlays now read the SAME probe the press
    // will read, so the highlight and the grab can no longer disagree.  `pr` is the probe
    // for this frame's cursor (UVG_NONE-what when nothing is hovered or a drag is live).
    // (The cursor UV that BD passed in is gone: every hit test it used to re-run here is
    // now inside the probe, which is the point of D-BG-E.)
    void DrawOverlays( ImDrawList *dl, bool hovered, const uvProbe_t &pr )
    {
        const ImVec2 o = UvToPx( s_originU, s_originV );
        const bool   idle = ( s_gesture == UVG_NONE ) && hovered;

        // 1. THE SHAPE UNDER THE CURSOR announces itself — a press there targets AND drags
        //    it (D-BI-C), so it has to be visible before the press.  With the click cycle
        //    this is also the readout of WHICH of the stacked shapes is next.
        if ( idle && pr.what == UVG_MOVE && ( pr.shapeFace >= 0 || pr.shapePatch >= 0 ) )
        {
            float st[KUVE_MAX_WINDING][2];
            ImVec2 pts[KUVE_MAX_WINDING];
            int n = 0;
            if ( pr.shapeFace >= 0 )
                n = FaceStPointsDisp( (size_t)pr.shapeFace, st, nullptr );
            else
                n = PatchRing( (size_t)pr.shapePatch, st );
            if ( n >= 3 )
            {
                for ( int k = 0; k < n; ++k )
                    pts[k] = UvToPx( st[k][0], st[k][1] );
                dl->AddPolyline( pts, n, IM_COL32( 255, 255, 255, 140 ),
                                 ImDrawFlags_Closed, 3.5f );
            }
        }

        // 2. THE TRANSFORM BOX AND ITS EIGHT HANDLES (D-BI-B).  Nothing here highlights
        //    that a press would not grab: every hot test reads the SAME probe the press
        //    reads, and the box the probe hit-tested is the box being drawn (s_box).
        if ( s_box.valid )
        {
            dl->AddRect( ImVec2( s_box.x0, s_box.y0 ), ImVec2( s_box.x1, s_box.y1 ),
                         IM_COL32( 247, 230, 59, 110 ), 0.0f, 0, 1.0f );
            const int nHandles = BoxHasHandles( s_box ) ? 8 : 0;
            for ( int h = 0; h < nHandles; ++h )
            {
                float hx, hy;
                HandlePx( s_box, h, &hx, &hy );
                const bool hot = ( idle && pr.handle == h && !pr.rotateZone )
                                 || ( s_gesture != UVG_NONE && s_gHandle == h );
                dl->AddRectFilled( ImVec2( hx - KUVE_HANDLE_PX, hy - KUVE_HANDLE_PX ),
                                   ImVec2( hx + KUVE_HANDLE_PX, hy + KUVE_HANDLE_PX ),
                                   hot ? KUVE_COL_HANDLE_HI : KUVE_COL_HANDLE );
                dl->AddRect( ImVec2( hx - KUVE_HANDLE_PX, hy - KUVE_HANDLE_PX ),
                             ImVec2( hx + KUVE_HANDLE_PX, hy + KUVE_HANDLE_PX ),
                             IM_COL32( 30, 30, 30, 200 ) );
            }
            // The rotate annulus has no permanent furniture — it would be four arcs of
            // clutter around a box the user is trying to read.  It draws its quarter arc
            // only while the cursor is in it (or a rotate is live from that corner).
            const int rotCorner = ( idle && pr.rotateZone ) ? pr.handle
                                : ( s_gesture == UVG_ROTATE && s_gHandle >= UVH_NW
                                    && s_gHandle <= UVH_SW ) ? s_gHandle : UVH_NONE;
            if ( rotCorner != UVH_NONE )
            {
                const float a0[4] = { 1.0f * KUVE_PI, 1.5f * KUVE_PI,
                                      0.0f,           0.5f * KUVE_PI };
                float hx, hy;
                HandlePx( s_box, rotCorner, &hx, &hy );
                const float r = KUVE_HANDLE_PICK_PX + KUVE_ROTATE_ZONE_PX * 0.5f;
                dl->PathArcTo( ImVec2( hx, hy ), r, a0[rotCorner],
                               a0[rotCorner] + 0.5f * KUVE_PI, 12 );
                dl->PathStroke( KUVE_COL_HANDLE_HI, 0, 2.5f );
            }
        }

        // 3. THE MARQUEE (D-BI-C).
        if ( s_gesture == UVG_MARQUEE )
        {
            const ImVec2 a = UvToPx( s_gStartU, s_gStartV );
            const ImVec2 b = UvToPx( s_gCurU,   s_gCurV );
            const ImVec2 r0( a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y );
            const ImVec2 r1( a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y );
            dl->AddRectFilled( r0, r1, IM_COL32( 255, 255, 255, 30 ) );
            dl->AddRect( r0, r1, IM_COL32( 255, 255, 255, 190 ), 0.0f, 0, 1.0f );
        }

        // ── 3b. KIWI-UX (ROUND BJ, ITEM 6): THE SNAP ACCENTS (D-BJ-F) ──────────
        // Drawn only while a gesture is live and only for an ENGAGED absorb.  Per
        // accent: a short TICK along the line the motion locked onto (a locked S
        // means a vertical line, and vice versa), then the 3D pass's dot-and-
        // separated-ring on the point itself.  Under the pivot on purpose — the
        // pivot is a handle you can grab and must stay readable over an accent.
        if ( s_gesture != UVG_NONE )
        {
            for ( size_t i = 0; i < s_snapAcc.size(); ++i )
            {
                const ImVec2 p = UvToPx( s_snapAcc[i].u, s_snapAcc[i].v );
                const float  t = 11.0f;
                if ( s_snapAcc[i].axis == 0 )
                    dl->AddLine( ImVec2( p.x, p.y - t ), ImVec2( p.x, p.y + t ),
                                 KUVE_COL_SNAP, 1.5f );
                else
                    dl->AddLine( ImVec2( p.x - t, p.y ), ImVec2( p.x + t, p.y ),
                                 KUVE_COL_SNAP, 1.5f );
                dl->AddCircleFilled( p, 2.5f, KUVE_COL_SNAP, 10 );
                dl->AddCircle( p, 6.0f, KUVE_COL_SNAP, 16, 1.5f );
            }
        }

        // 4. THE PIVOT: a circle and a SMALL CROSS.  Its two infinite axis LINES are gone
        //    (D-BI-D) — they were two full-canvas bands of picks sitting on the shapes.
        //    The cross keeps their one useful job, naming which way S and T run.
        const bool pivotHot = ( s_gesture == UVG_ORIGIN )
                            || ( idle && pr.what == UVG_ORIGIN );
        dl->AddLine( ImVec2( o.x - KUVE_ORIGIN_CROSS_PX, o.y ),
                     ImVec2( o.x + KUVE_ORIGIN_CROSS_PX, o.y ), KUVE_COL_UAXIS, 1.5f );
        dl->AddLine( ImVec2( o.x, o.y - KUVE_ORIGIN_CROSS_PX ),
                     ImVec2( o.x, o.y + KUVE_ORIGIN_CROSS_PX ), KUVE_COL_VAXIS, 1.5f );
        dl->AddCircleFilled( o, KUVE_ORIGIN_RADIUS_PX,
                             pivotHot ? KUVE_COL_HANDLE_HI : KUVE_COL_HANDLE, 16 );
    }

    // The ImGui mouse cursor for what the probe found.  Recomputed FROM SCRATCH every
    // frame and never latched — ImGui resets g.MouseCursor to Arrow in NewFrame
    // (imgui.cpp:6012) and the win32 backend only calls ::SetCursor when the value
    // CHANGES (imgui_impl_win32.cpp:534-538), so "stop asking" is the same as "arrow".
    // That shape is the standing regression check for the round-BG cursor fix.
    void ApplyHoverCursor( const uvProbe_t &pr, bool hovered )
    {
        if ( !hovered || s_gesture != UVG_NONE )
            return;
        switch ( pr.what )
        {
        case UVG_ROTATE:
            // ImGui 1.92's cursor set has no ROTATE shape (imgui.h ImGuiMouseCursor_ enum
            // is Arrow / TextInput / ResizeAll / NS / EW / NESW / NWSE / Hand / NotAllowed),
            // so the pivot and the rotate annulus share Hand.  They never overlap — the
            // annulus is outside the box and the pivot disc is inside it.
            ImGui::SetMouseCursor( ImGuiMouseCursor_Hand );
            break;
        case UVG_ORIGIN:
            ImGui::SetMouseCursor( ImGuiMouseCursor_Hand );
            break;
        case UVG_SCALE:
        case UVG_SHEAR:
            // Corners take the DIAGONAL cursors, and which diagonal depends on the corner:
            // NW/SE lie on the NWSE diagonal, NE/SW on the NESW one.
            if ( pr.handle == UVH_NW || pr.handle == UVH_SE )
                ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeNWSE );
            else if ( pr.handle == UVH_NE || pr.handle == UVH_SW )
                ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeNESW );
            else
                ImGui::SetMouseCursor( pr.axis[0] ? ImGuiMouseCursor_ResizeEW
                                                  : ImGuiMouseCursor_ResizeNS );
            break;
        case UVG_MOVE:
            ImGui::SetMouseCursor( ImGuiMouseCursor_ResizeAll );
            break;
        default:
            if ( pr.shearRefused )
                ImGui::SetMouseCursor( ImGuiMouseCursor_NotAllowed );
            break;      // UVG_MARQUEE keeps the arrow
        }
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  TOOLBAR
    // ═══════════════════════════════════════════════════════════════════════════════
    texdef_sub_t *ActiveTexdef( MaterialDef **outMd )
    {
        if ( s_activeFace < 0 )
            return nullptr;
        return KiwiUv_FaceTexdef( s_brushes[s_faces[s_activeFace].brushIdx].def,
                                  s_faces[s_activeFace].faceIndex, outMd );
    }

    // One immediate texdef edit over every TARGETED FACE, in one undo bracket.  `field`
    // says which member the callback writes; patches are untouched (D-BD-H).
    // KIWI-UX (ROUND BG, ITEM 4): UVF_FLIPU / UVF_FLIPV / UVF_ROT90CW / UVF_ROT90CCW are
    // GONE from this enum.  They wrote the texdef fields directly, which pivots on the
    // TEXTURE-SPACE ORIGIN — the "flies around really far off the centre point" report.
    // All four are now ApplyAffineImmediate calls pivoted on the origin handle (D-BG-C).
    enum uvField_t { UVF_SHIFT0 = 0, UVF_SHIFT1, UVF_SIZE0, UVF_SIZE1, UVF_ROTATE,
                     UVF_CROSSTERM, UVF_RESET };

    // The per-face write WITHOUT bracket management, so a verb that has to touch patches
    // as well can put both halves inside ONE undo record.  (Rot 90 on a mixed selection
    // did exactly that wrongly at first: the face half through ApplyField and the patch
    // half through a second call that ALSO rewrote the faces, turning them 180°.)
    void ApplyFieldInner( uvField_t field, float value )
    {
        const float sample = g_qeglobals.random_texture_stuff[TemplateLayer()].sampleSize;

        for ( size_t i = 0; i < s_faces.size(); ++i )
        {
            if ( !FaceIsTarget( i ) )        // ROUND BG, item 6
                continue;
            MaterialDef  *md = nullptr;
            texdef_sub_t *td = KiwiUv_FaceTexdef( s_brushes[s_faces[i].brushIdx].def,
                                                  s_faces[i].faceIndex, &md );
            if ( !td || !md )
                continue;
            switch ( field )
            {
            case UVF_SHIFT0:    td->shift[0]  = value; break;
            case UVF_SHIFT1:    td->shift[1]  = value; break;
            case UVF_SIZE0:     td->size[0]   = value; break;
            case UVF_SIZE1:     td->size[1]   = value; break;
            case UVF_ROTATE:    td->rotate    = value; break;
            case UVF_CROSSTERM: td->crossterm = value; break;
            case UVF_RESET:
            {
                // TexWnd_BuildClickedMaterialDef's own default (texwnd.cpp:674-697):
                // size = material w/h × the layer's sample size, everything else zero.
                float w, h;
                MaterialSize( md, &w, &h );
                td->size[0]   = w * sample;
                td->size[1]   = h * sample;
                td->shift[0]  = 0.0f;
                td->shift[1]  = 0.0f;
                td->rotate    = 0.0f;
                td->crossterm = 0.0f;
                break;
            }
            }
            TexMatToFakeTexCoords( md, td );
        }
    }

    void RebuildAll()
    {
        for ( size_t i = 0; i < s_brushes.size(); ++i )
            TouchBrush( s_brushes[i].node, s_brushes[i].def );
        g_nUpdateBits = -1;
    }

    // ── KIWI-UX (ROUND BG, ITEM 4 / D-BG-C): ONE-SHOT AFFINE, THE GESTURE PATH ────────
    // Rot 90 CW/CCW and Flip U/V are now this, and nothing else.  It is deliberately the
    // SAME arithmetic ApplySelection runs — displayed-space affine `A`, conjugated per
    // shape by its display transform, faces through Face_MoveTexture + texturevecs_02 and
    // patches through their control STs — with the one difference that a toolbar verb has
    // no gesture snapshot, so each shape's CURRENT state is its own td0.  Both halves live
    // inside ONE undo bracket, which is the shape BD's Rot 90 already had and had to.
    void ApplyAffineImmediate( const uvXform_t &A, const char *undoName )
    {
        if ( !AnythingSelected() || XfIsIdentity( A ) )
            return;
        UndoOpen( undoName );

        for ( size_t i = 0; i < s_faces.size(); ++i )
        {
            if ( !FaceIsTarget( i ) )
                continue;
            face_t *fd = FaceOf( s_faces[i] );
            if ( !fd )
                continue;
            MaterialDef  *md = nullptr;
            texdef_sub_t *td = KiwiUv_FaceTexdef( s_brushes[s_faces[i].brushIdx].def,
                                                  s_faces[i].faceIndex, &md );
            if ( !td || !md )
                continue;
            const uvXform_t D   = ( i < s_faceD.size() ) ? s_faceD[i] : XfIdentity();
            const texdef_sub_t td0 = *td;
            texdef_sub_t out = td0;
            TransformFace( fd, &td0, XfConjugate( A, D ), &out );
            td->size[0]   = out.size[0];
            td->size[1]   = out.size[1];
            td->shift[0]  = out.shift[0];
            td->shift[1]  = out.shift[1];
            td->rotate    = out.rotate;
            td->crossterm = out.crossterm;
            TexMatToFakeTexCoords( md, td );
        }
        for ( size_t i = 0; i < s_patches.size(); ++i )
        {
            if ( !PatchIsTarget( i ) )
                continue;
            patchMesh_t *p = s_brushes[s_patches[i]].pm;
            if ( !p )
                continue;
            const uvXform_t D = ( i < s_patchD.size() ) ? s_patchD[i] : XfIdentity();
            float st[16][16][2];
            SnapshotPatch( p, st );
            TransformPatch( p, st, XfConjugate( A, D ) );
        }

        RebuildAll();
        UndoCommit();
    }

    void ApplyField( uvField_t field, float value, const char *undoName )
    {
        if ( s_faces.empty() )
            return;
        UndoOpen( undoName );
        ApplyFieldInner( field, value );
        RebuildAll();
        UndoCommit();
    }

    void DrawToolbar()
    {
        const bool canFace  = !s_faces.empty();
        const bool canAny   = AnythingSelected();
        const bool hasPatch = !s_patches.empty();

        ImGui::BeginDisabled( !canAny );
        if ( ImGui::Button( "Reset" ) )
        {
            if ( canFace )
                ApplyField( UVF_RESET, 0.0f, "uv reset" );
            if ( hasPatch )
            {
                // Patch_NaturalizeSelected opens its OWN bracket (pmesh.cpp:2755-2757), so
                // it cannot join ours — a mixed selection's Reset is two Ctrl+Z and the
                // console says so rather than leaving the user to find out.  This is
                // Cmd_OnPatchNaturalize's own pair (mainfrm.cpp:3895).
                float x[2] = { 0.0f, 0.0f };
                Select_SetTexture( x );
                Patch_NaturalizeSelected( 0, 0, x[0], x[1] );
                Sys_Printf( "UV editor: patches re-naturalised under their own undo "
                            "record%s.\n",
                            canFace ? " (so this Reset is two undo steps)" : "" );
            }
            g_nUpdateBits = -1;
        }
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Faces: shift 0, rotate 0, skew 0, size = material w/h x the\n"
                               "layer sample size - the same default a texture-browser click\n"
                               "applies.  Patches: re-naturalise (its own undo record).\n"
                               "The face half honours the canvas target set; the patch half\n"
                               "cannot - Patch_NaturalizeSelected takes the 3D selection." );

        // ── KIWI-UX (ROUND BG, ITEM 4 + ITEM 5) ────────────────────────────────────
        // All four verbs are ONE affine about the ORIGIN HANDLE, applied through the
        // gesture path to faces and patches alike (D-BG-C).  The signs are chosen in
        // CANVAS space, where +T is DOWN: [[0,-1],[1,0]] takes screen-right to screen-DOWN,
        // i.e. the outline turns CLOCKWISE, which is what a button labelled "CW" has to do
        // under D-BG-D.  (BD's patch arm used [[0,1],[-1,0]] for "CW", which is the other
        // way round; its face arm did not pivot here at all.)
        ImGui::SameLine();
        ImGui::BeginDisabled( !canAny );
        if ( ImGui::Button( "Flip U" ) )
            ApplyAffineImmediate(
                XfAboutOrigin( -1.0f, 0.0f, 0.0f, 1.0f, s_originU, s_originV ), "uv flip u" );
        ImGui::SameLine();
        if ( ImGui::Button( "Flip V" ) )
            ApplyAffineImmediate(
                XfAboutOrigin( 1.0f, 0.0f, 0.0f, -1.0f, s_originU, s_originV ), "uv flip v" );
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled( !canAny );
        if ( ImGui::Button( "Rot 90 CW" ) )
            ApplyAffineImmediate(
                XfAboutOrigin( 0.0f, -1.0f, 1.0f, 0.0f, s_originU, s_originV ), "uv rotate 90" );
        ImGui::SameLine();
        if ( ImGui::Button( "Rot 90 CCW" ) )
            ApplyAffineImmediate(
                XfAboutOrigin( 0.0f, 1.0f, -1.0f, 0.0f, s_originU, s_originV ), "uv rotate 90" );
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Faces AND patches turn about the yellow PIVOT, through the same\n"
                               "affine path a canvas gesture uses - so the shape turns in place\n"
                               "instead of swinging around the texture origin.  The pivot sits\n"
                               "at the centre of whatever is targeted until you drag it, so by\n"
                               "default these turn the target set about its own centre." );

        // ── KIWI-UX (ROUND BG, ITEM 3b): the fold-out toggle (D-BG-A) ──────────────
        ImGui::SameLine();
        if ( ImGui::Checkbox( "Chain", &s_chain ) )
            s_chainBuilt = false;                 // rebuild (or drop) on the next frame
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Fold multi-face selections out like paper angels: faces that\n"
                               "share a world edge are unfolded along it so they stop stacking\n"
                               "on top of each other.  DISPLAY ONLY - the STs never move.\n"
                               "Off = true ST positions (use it to check real alignment)." );

        // ── KIWI-UX (ROUND BJ, ITEM 7): RE-SOLVING THE LAYOUT IS A DECISION ────────
        // The fold is latched now (see DrawCanvas) — it is NOT re-solved because a
        // texdef changed, which is what made a group rotate reshuffle on release.  So
        // the way to re-solve it has to be something the user asks for.
        ImGui::SameLine();
        ImGui::BeginDisabled( !s_chain || s_faces.size() < 2 );
        if ( ImGui::Button( "Re-fold" ) )
        {
            s_chainBuilt = false;
            _snprintf( s_status, sizeof( s_status ), "re-folding the chain" );
            s_status[sizeof( s_status ) - 1] = '\0';
        }
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Re-solve the fold-out from the CURRENT texture coordinates.\n"
                               "The layout is otherwise held fixed for as long as the same\n"
                               "faces are selected, so editing UVs (a group rotate above all)\n"
                               "never re-arranges the shapes underneath you." );

        ImGui::SameLine();
        ImGui::SetNextItemWidth( 70.0f );
        if ( ImGui::InputInt( "grid X", &s_subX, 1, 1 ) )
            s_subX = ClampI( s_subX, 1, 16 );
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 70.0f );
        if ( ImGui::InputInt( "grid Y", &s_subY, 1, 1 ) )
            s_subY = ClampI( s_subY, 1, 16 );

        // ── the numeric row (FACE ONLY — D-BD-H) ───────────────────────────────────
        MaterialDef  *md = nullptr;
        texdef_sub_t *td = ActiveTexdef( &md );
        (void)md;                     // the fields want the texdef, not the material
        static float  f[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        static bool   editing[6] = { false, false, false, false, false, false };
        if ( td )
        {
            for ( int i = 0; i < 6; ++i )
                if ( !editing[i] )
                {
                    switch ( i )
                    {
                    case 0: f[0] = td->shift[0];  break;
                    case 1: f[1] = td->shift[1];  break;
                    case 2: f[2] = td->size[0];   break;
                    case 3: f[3] = td->size[1];   break;
                    case 4: f[4] = td->rotate;    break;
                    case 5: f[5] = td->crossterm; break;
                    }
                }
        }

        static const char *labels[6] = { "shift x", "shift y", "size x", "size y",
                                         "rotate",  "skew" };
        static const uvField_t fields[6] = { UVF_SHIFT0, UVF_SHIFT1, UVF_SIZE0,
                                             UVF_SIZE1, UVF_ROTATE, UVF_CROSSTERM };
        ImGui::BeginDisabled( td == nullptr );
        for ( int i = 0; i < 6; ++i )
        {
            if ( i )
                ImGui::SameLine();
            ImGui::PushID( i );
            ImGui::SetNextItemWidth( 84.0f );
            const bool enter = ImGui::InputFloat( labels[i], &f[i], 0.0f, 0.0f, "%.3f",
                                                  ImGuiInputTextFlags_EnterReturnsTrue );
            editing[i] = ImGui::IsItemActive();
            if ( enter || ImGui::IsItemDeactivatedAfterEdit() )
                ApplyField( fields[i], f[i], "uv numeric" );
            ImGui::PopID();
        }
        ImGui::EndDisabled();
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Numeric fields are FACE-ONLY: a patch has no texdef - it\n"
                               "carries per-control-point ST directly.  The five gesture\n"
                               "verbs on the canvas DO apply to patches.\n"
                               "These write the RAW texdef values, so \"rotate\" turns about\n"
                               "the face's own texture origin - the toolbar's Rot 90 / Flip\n"
                               "buttons pivot on the yellow ORIGIN handle instead." );
    }

    // ═══════════════════════════════════════════════════════════════════════════════
    //  THE CANVAS
    // ═══════════════════════════════════════════════════════════════════════════════
    void DrawCanvas()
    {
        ImGuiIO &io = ImGui::GetIO();

        s_c0 = ImGui::GetCursorScreenPos();
        s_cs = ImGui::GetContentRegionAvail();
        if ( s_cs.x < 32.0f ) s_cs.x = 32.0f;
        if ( s_cs.y < 32.0f ) s_cs.y = 32.0f;

        ImGui::InvisibleButton( "##uvpad", s_cs,
                                ImGuiButtonFlags_MouseButtonLeft
                              | ImGuiButtonFlags_MouseButtonRight
                              | ImGuiButtonFlags_MouseButtonMiddle );
        const bool hovered = ImGui::IsItemHovered();

        // Re-frame when the selection changed, and keep re-trying the fit while the widget
        // has no real size yet (TB cameraViewportChanged, UvViewHelper.cpp:73-83).
        const unsigned gen = Sel_Generation();
        if ( !s_haveGen || gen != s_selGen )
        {
            s_haveGen   = true;
            s_selGen    = gen;
            s_zoomValid = false;
            // KIWI-UX (ROUND BI): the pivot's "user placed" latch lives for exactly one
            // selection (D-BI-D), and the click cycle's anchor means nothing across one.
            s_originUser = false;
            s_cycleValid = false;
            if ( s_gesture != UVG_NONE )
                GestureCancel();
        }

        // ── KIWI-UX (ROUND BG, ITEM 3b): (re)solve the fold-out (D-BG-A) ───────────
        // Gather() resets every D to identity each frame, so the layout has to be either
        // rebuilt or REPLAYED FROM THE CACHE here.  The cache is index-parallel and is
        // only valid while the STRUCTURE signature (which rows were gathered, in which
        // order) is unchanged.
        //
        // ── KIWI-UX (ROUND BJ, ITEM 7): THE LAYOUT IS LATCHED, NOT RE-SOLVED ──────
        // USER REPORT, verbatim: *"When rotating them all as a group, dont scramble
        // them, keep them in their orientation."*
        //
        // THE MECHANISM WAS THIS BLOCK.  Round BG's rebuild condition was
        // `!s_chainBuilt || ChainSignature() != s_chainSig`, and ChainSignature hashed
        // every gathered face's size / shift / rotate / crossterm and every patch's
        // ST — i.e. the very values a gesture writes.  The gesture itself was safe (the
        // `s_gesture == UVG_NONE` clause froze the layout mid-drag), but the instant the
        // mouse came UP the signature had moved and the whole fold-out was re-solved
        // FROM THE NEW STs: a new BFS, new FoldRigid rotations, possibly different
        // mirror decisions (SideOf flips as the ensemble turns) and a shelf laid out
        // from a moved occupied box.  A group rotate therefore reshuffled on release,
        // and so did every other write — a single-shape move snapped its shape back
        // into the chain, and a numeric field edit re-solved the lot.
        //
        // THE FIX: D_f is a DISPLAY LAYOUT, not a function of the texdefs.  It is
        // solved once per (gathered set, active face = fold anchor, Chain toggle) —
        // which is exactly what ChainStructSig already hashes — and NEVER because a
        // texdef changed.  The conjugation (D-BG-A) then does the rest for free: a
        // displayed outline is D_f·st_f and a gesture writes A_true = D_f⁻¹·A·D_f, so
        // the displayed outline moves by exactly A whatever D_f is.  The arrangement is
        // therefore preserved indefinitely, and a group rotate rigidly turns the whole
        // displayed ensemble about the pivot.
        //
        // THE TARGET SET is deliberately NOT part of the key.  BuildChain never reads
        // it (the anchor is the active face), so keying on it would add re-solve points
        // — i.e. scramble points — for no change in the answer.  RE-FOLD is a toolbar
        // button instead, because "re-solve the layout" is now a decision and not a
        // side effect.  AUTO-FRAME is unaffected: it runs off `s_zoomValid`, which the
        // Sel_Generation change clears — that is the VIEW, not the layout.
        {
            const unsigned ssig = ChainStructSig();
            if ( ssig != s_chainStructSig )
                s_chainBuilt = false;
            if ( s_gesture == UVG_NONE && !s_chainBuilt )
            {
                BuildChain();
                s_chainD         = s_faceD;
                s_chainPD        = s_patchD;
                s_chainStructSig = ssig;
                s_chainBuilt     = true;
            }
            else if ( s_chainBuilt
                      && s_chainD.size()  == s_faceD.size()
                      && s_chainPD.size() == s_patchD.size() )
            {
                s_faceD  = s_chainD;
                s_patchD = s_chainPD;
            }
        }

        // AFTER the chain solve (ROUND BG): the fit frames the DISPLAYED spread, and the
        // fold-out is what decides where that is.  Framing first would centre the view on
        // the active face and leave its folded-out neighbours off-screen.
        if ( !s_zoomValid )
            FrameActive();

        // ── KIWI-UX (ROUND BI): the pivot, then the box, BEFORE any input is resolved ──
        // Probe() hit-tests s_box, so the box the press is measured against has to be this
        // frame's — computed from the same displayed outlines the wireframes will draw.
        AutoPivot();
        s_box = ComputeBox();

        float cu = 0.0f, cv = 0.0f;
        PxToUv( io.MousePos, &cu, &cv );

        // ── wheel zoom, cursor-anchored (TB UvCameraTool::mouseScroll :80-107) ──────
        if ( hovered && io.MouseWheel != 0.0f && s_gesture == UVG_NONE )
        {
            const float before[2] = { cu, cv };
            const float z = ( io.MouseWheel > 0.0f ) ? KUVE_ZOOM_STEP : 1.0f / KUVE_ZOOM_STEP;
            s_zoom = ClampF( s_zoom * z, KUVE_ZOOM_MIN, KUVE_ZOOM_MAX );
            float after[2];
            PxToUv( io.MousePos, &after[0], &after[1] );
            s_panU += before[0] - after[0];
            s_panV += before[1] - after[1];
            PxToUv( io.MousePos, &cu, &cv );
        }

        // ── gesture start / update / end ────────────────────────────────────────────
        if ( s_gesture == UVG_NONE && hovered )
        {
            // KIWI-UX (ROUND BI): Esc with no gesture live clears the sub-selection back
            // to all — the keyboard way out of a target set, next to the empty click.
            // (The KIWI key funnel can claim Escape before ImGui sees it; the empty click
            // and the empty marquee are the paths that never depend on that.)
            if ( !s_targetAll && ImGui::IsKeyPressed( ImGuiKey_Escape, false ) )
            {
                TargetAll();
                s_cycleValid = false;
                _snprintf( s_status, sizeof( s_status ), "targeting all shapes" );
                s_status[sizeof( s_status ) - 1] = '\0';
            }
            for ( int b = 0; b < 3; ++b )
                if ( ImGui::IsItemClicked( b ) )
                {
                    GestureBegin( b, cu, cv );
                    break;
                }
        }
        else if ( s_gesture != UVG_NONE )
        {
            // App focus loss cancels the drag — TB does the same on window deactivate
            // (UvView.cpp:191-199).  Esc cancels too; note that the KIWI key funnel can
            // claim Escape before ImGui sees it, so the mouse release is still the
            // primary end of a gesture.
            if ( io.AppFocusLost || ImGui::IsKeyPressed( ImGuiKey_Escape, false ) )
                GestureCancel();
            else if ( s_gButton >= 0 && !io.MouseDown[s_gButton] )
            {
                GestureUpdate( cu, cv );
                GestureEnd();
            }
            else
                GestureUpdate( cu, cv );
        }

        // ── draw ────────────────────────────────────────────────────────────────────
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->PushClipRect( s_c0, ImVec2( s_c0.x + s_cs.x, s_c0.y + s_cs.y ), true );

        MaterialDef *bgMd = nullptr;
        if ( s_activeFace >= 0 )
            ActiveTexdef( &bgMd );
        else if ( !s_patches.empty() )
            bgMd = PatchMtlDef( s_brushes[s_patches[0]].pm );

        bool not2D = false;
        DrawBackground( dl, ActiveColorMap( bgMd, &not2D ) );
        DrawGrid( dl );
        DrawWireframes( dl );

        // ── KIWI-UX (ROUND BG, ITEM 3a / D-BG-E): ONE probe, both consumers ────────
        // Run AFTER the wireframes (it hit-tests displayed shapes) and BEFORE the
        // overlays (which draw its answer).  Recomputed from scratch every frame — see
        // ApplyHoverCursor for why that shape is also the cursor's regression check.
        // ROUND BI: the box is recomputed here too, because a live gesture has just
        // rewritten the STs it is derived from and the overlays must draw the box that
        // is around the shapes NOW, not the one the press was measured against.
        s_box = ComputeBox();
        uvProbe_t pr;
        ProbeClear( &pr );
        if ( hovered && s_gesture == UVG_NONE )
            pr = Probe( cu, cv, io.KeyAlt );
        ApplyHoverCursor( pr, hovered );

        if ( AnythingSelected() )
            DrawOverlays( dl, hovered, pr );

        // ── the readout, bottom-left of the canvas ─────────────────────────────────
        char line[320];
        const char *name = ( MtlDefUsable( bgMd ) )
                         ? (const char *)Materialdef_GetName( bgMd ) : nullptr;
        // ROUND BG, item 6: the readout NAMES the target count, because a canvas where
        // only some of the visible shapes will move has to say so.
        const int nShapes = (int)( s_faces.size() + s_patches.size() );
        char tgt[64];
        tgt[0] = '\0';
        if ( !s_targetAll && nShapes > 0 )
            _snprintf( tgt, sizeof( tgt ), "   editing %d of %d", TargetCount(), nShapes );
        tgt[sizeof( tgt ) - 1] = '\0';
        // ...and what the fold-out actually did, for the same reason: a shape that is not
        // where its STs say it is has to admit it.
        char chain[64];
        chain[0] = '\0';
        if ( s_chain && s_faces.size() > 1 )
        {
            if ( (int)s_faces.size() > KUVE_CHAIN_MAX_FACES )
                _snprintf( chain, sizeof( chain ), "   chain off (>%d faces)",
                           KUVE_CHAIN_MAX_FACES );
            else
                _snprintf( chain, sizeof( chain ), "   chain %d folded, %d shelved",
                           s_chainFolded, s_chainShelved );
        }
        chain[sizeof( chain ) - 1] = '\0';
        _snprintf( line, sizeof( line ),
                   "%s   %d face%s, %d patch%s%s%s   %.0f px/repeat   u %.3f  v %.3f%s",
                   name ? name : "(no material)",
                   (int)s_faces.size(), s_faces.size() == 1 ? "" : "s",
                   (int)s_patches.size(), s_patches.size() == 1 ? "" : "es",
                   tgt, chain, s_zoom, cu, cv,
                   not2D ? "   [colormap is not a 2D image - no background]" : "" );
        line[sizeof( line ) - 1] = '\0';
        dl->AddText( ImVec2( s_c0.x + 6.0f, s_c0.y + s_cs.y - 34.0f ),
                     IM_COL32( 225, 225, 225, 210 ), line );
        if ( s_status[0] )
            dl->AddText( ImVec2( s_c0.x + 6.0f, s_c0.y + s_cs.y - 18.0f ),
                         IM_COL32( 255, 220, 140, 230 ), s_status );

        dl->PopClipRect();
    }
}   // anonymous namespace

// ═════════════════════════════════════════════════════════════════════════════════════
//  the dock window
// ═════════════════════════════════════════════════════════════════════════════════════
void KiwiUvEd_Draw()
{
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_UVEDITOR );
    if ( !open || !*open )
    {
        // Closed with a drag live (the ✕ box is reachable mid-gesture): the gesture has to
        // end somewhere, and an open undo bracket must never survive a frame.  ABANDON
        // rather than cancel — nothing gathered this frame, so there is no live selection
        // to restore through (see GestureAbandon).
        GestureAbandon();
        s_inScope = false;                   // ROUND BN, ITEM 3: closed is out of scope
        return;                              // closed: no Begin, no End, no cost
    }

    if ( KiwiWindows_JustOpened( KIWI_WIN_UVEDITOR ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );

    const bool visible = ImGui::Begin( KiwiWindows_Title( KIWI_WIN_UVEDITOR ), open );
    if ( visible )
    {
        Gather();

        // ── KIWI-UX (ROUND BN, ITEM 3): THE SCOPE LATCH ─────────────────────────
        // Computed HERE, once, because this is the only place in the frame where the
        // window's own ImGui focus/hover state is askable at all — Cam_Draw, which is
        // what reads the answer, does not run inside this window's scope.  The whole
        // rule (and why one frame of latency is the right trade) is in kiwi_uveditor.h
        // "IN SCOPE".  RootAndChildWindows on both predicates because the canvas and
        // the toolbar are CHILD windows: without it, hovering the canvas — i.e. the
        // one thing the user is certain to be doing — would read as NOT hovering the
        // UV editor.
        s_inScope = ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows )
                 || ImGui::IsWindowHovered( ImGuiHoveredFlags_RootAndChildWindows )
                 || s_gesture != UVG_NONE;

        if ( !AnythingSelected() )
        {
            // KIWI-UX (ROUND BI): the help text is the interaction model, so it is
            // rewritten with it.  Nothing in here is on a grid line any more.
            ImGui::TextWrapped(
                "Nothing selected.  Select brush faces (or whole brushes, or patches) and "
                "their UV wireframes appear here over the active material, tiled.\n\n"
                "Drag a shape to MOVE it - the outline follows the cursor.  The targeted "
                "shapes (drawn in GOLD) share one transform box: corner handles scale "
                "about the OPPOSITE corner (Shift = uniform), edge handles scale one axis "
                "about the opposite edge, just outside a corner rotates, Alt + an edge "
                "handle skews, and the yellow dot is the pivot for rotate / skew / the "
                "Rot 90 and Flip buttons (drag it to move it).\n\n"
                "Click a shape to edit only that one; click the same spot again to step "
                "down through shapes stacked there; Shift+click adds or removes; drag on "
                "empty canvas to rubber-band; click empty canvas (or Esc) for all of them "
                // KIWI-UX (ROUND BO, ITEM 3): Ctrl ENGAGES snapping here now.
                "again.  Right/middle drag pans, the wheel zooms, HOLD CTRL to snap "
                "(grid, whole texels and the other shapes' vertices - drags are free "
                "without it), Esc cancels a live drag.\n\n"
                "Multiple faces are FOLDED OUT along the world edges they share, so they "
                "stop stacking on top of each other (\"Chain\" in the toolbar turns that "
                "off).  The grid is a guide and a snap target only - nothing is dragged "
                "by it." );
            if ( s_gesture != UVG_NONE )
                GestureCancel();
        }
        else
        {
            const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + 12.0f;
            ImGui::BeginChild( "##uvcanvas", ImVec2( 0.0f, -footer ),
                               ImGuiChildFlags_Borders,
                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
                             | ImGuiWindowFlags_NoMove );
            DrawCanvas();
            ImGui::EndChild();
            DrawToolbar();
        }
    }
    else
    {
        GestureAbandon();                    // collapsed / clipped away mid-drag
        s_inScope = false;                   // ROUND BN, ITEM 3: and out of scope with it
    }
    ImGui::End();
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BN, ITEM 3) — the two questions the 3D overlay passes ask
// ═════════════════════════════════════════════════════════════════════════════════════
// Both are pure reads of state this file already keeps per frame; the whole argument for
// what they suppress (and for what they deliberately do not) is on the declarations in
// kiwi_uveditor.h.
bool KiwiUvEd_InScope()
{
    return s_inScope;
}

bool KiwiUvEd_OverlaySuppressed( const brush_t *def, int faceIndex )
{
    if ( !s_inScope || !def )
        return false;
    if ( faceIndex < 0 )
    {
        // The WHOLE-object question.  s_brushes carries one row per gathered def
        // whether it arrived as a whole brush, as a patch or as the owner of a picked
        // face — and for the face case the brush is not on `selected_brushes` at all
        // (kiwi_selection.h DESIGN NOTE 2), so the brush-level passes never see it.
        for ( size_t i = 0; i < s_brushes.size(); ++i )
            if ( s_brushes[i].def == def )
                return true;
        return false;
    }
    return HaveFace( const_cast< brush_t * >( def ), faceIndex );
}

// ═════════════════════════════════════════════════════════════════════════════════════
//  §15 palette
// ═════════════════════════════════════════════════════════════════════════════════════
void KiwiUvEd_RegisterCommands()
{
    // Unbound, searchable — the same deal every §9 window entry gets
    // (kiwi_windows.cpp:296-301).  The TOGGLE dispatches through
    // KiwiWindows_DispatchInstant, which owns every §9 flag; this row only names it.
    Radiant_RegisterCommand( "KiwiWindowUvEditor", 0, 0, KIWI_CMD_WINDOW_UVEDITOR );
}

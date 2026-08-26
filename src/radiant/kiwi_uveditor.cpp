#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Copyright (C) 2010 Kristian Duske
//
// Portions of this file are derived from TrenchBroom
// (https://github.com/TrenchBroom/TrenchBroom): UvViewHelper.cpp (snapDelta, zoom fit),
// UvOffsetTool.cpp (offset snap), UvScaleTool.cpp (handle ratio and vertex snap),
// UvRotateTool.cpp (grab angle and edge-angle snap), UvShearTool.cpp (shear, edge-slope
// snap, and issue #1350 guard), UvOriginTool.cpp, UvCameraTool.cpp, and UvEditor.cpp.
//
// TrenchBroom is free software: you can redistribute it and/or modify it under the terms
// of the GNU General Public License as published by the Free Software Foundation, either
// version 3 of the License, or (at your option) any later version.
//
// TrenchBroom is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// kiwi_uveditor.h documents the coordinate, affine, display-layout, and undo contracts.

#include "stdafx.h"
#include "qe3.h"
#include <imgui/imgui.h>

#include "kiwi_uveditor.h"
#include "kiwi_command.h"
#include "kiwi_selection.h"
#include "kiwi_hover.h"
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
#include <algorithm>          // std::sort for the fold-out's edge weld
#include <vector>

// Ported/cross-file entry points, verified against their definitions.
extern int         Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:118   int Sys_Printf(const char*,...)
extern int         g_nUpdateBits;                                            // engine_stubs.cpp:773  int g_nUpdateBits
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1358  bool Radiant_RegisterCommand(const char*,byte,byte,int)
extern ImGuiID     ImGuiShell_DockRoot();                                    // imgui_shell.cpp:796   ImGuiID ImGuiShell_DockRoot()

// Copied from brush.cpp:1736-1749. The pointer-as-int parameters preserve the binary's
// usercall convention (texturevecs.cpp:94-101, 195-221).
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

// Patch_ShiftTexture(p, 0, 0) supplies the ported rebuild tail after direct ST writes:
// dirty layer 1, free curveDef, Patch_GenericMesh2, and increment version (pmesh.cpp:3241-3246).
extern void        Patch_ShiftTexture( patchMesh_t *p, float s, float t );   // pmesh.cpp:3216    void Patch_ShiftTexture(patchMesh_t*,float,float)
extern void        Patch_NaturalizeSelected( bool unk, bool cap, float x, float y ); // pmesh.cpp:2742  void Patch_NaturalizeSelected(bool,bool,float,float)
extern void        Select_SetTexture( float *out );                          // select.cpp:1179   void Select_SetTexture(float*)

namespace
{
    // Non-tuning constants; tuning lives in kiwi_uveditor.h.
    const float KUVE_EPS        = 1.0e-6f;
    const int   KUVE_MAX_WINDING = 64;   // EdLayerGeom's own cap (qe3.h:216) — brush faces
                                         // are convex, so 64 points is ample.
    const float KUVE_PI         = 3.14159265358979323846f;

    // Sampling texture luma would lock a GPU surface every frame. The dim background makes
    // this fixed light palette readable without owning or reading back a D3D resource.
    const ImU32 KUVE_COL_BG        = IM_COL32(  24,  24,  28, 255 );
    const ImU32 KUVE_COL_TILE_TINT = IM_COL32( 168, 168, 168, 255 );  // multiply tint: dim
    const ImU32 KUVE_COL_GRID_MAJ  = IM_COL32( 230, 230, 230, 150 );
    const ImU32 KUVE_COL_GRID_MIN  = IM_COL32( 230, 230, 230,  98 );  // 150 * 0.65 (TB)
    const ImU32 KUVE_COL_FACE      = IM_COL32( 120, 200, 255, 190 );
    const ImU32 KUVE_COL_ACTIVE    = IM_COL32( 255, 210,  90, 255 );
    const ImU32 KUVE_COL_PATCH     = IM_COL32( 150, 255, 150, 190 );
    const ImU32 KUVE_COL_HANDLE    = IM_COL32( 247, 230,  59, 255 );  // TB HandleColor
    const ImU32 KUVE_COL_HANDLE_HI = IM_COL32( 255,  40,  40, 255 );  // TB SelectedHandleColor
    // The 3D snap glyph is near-black; this canvas uses the same glyph in a visible hue.
    const ImU32 KUVE_COL_SNAP      = IM_COL32( 120, 255, 235, 240 );
    const ImU32 KUVE_COL_UAXIS     = IM_COL32( 255,  61,   0, 178 );  // TB (1.0,0.24,0.0,0.7)
    const ImU32 KUVE_COL_VAXIS     = IM_COL32(  74, 148,   0, 178 );  // TB (0.29,0.58,0.0,0.7)

    // Row-major 2x3 affine: x' = m[0]x + m[1]y + m[2].
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

    // T(origin) * L * T(-origin): rotate/skew and pivoted toolbar transforms pin the pivot.
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

    // Composition, `a AFTER b`: (a o b)(x) = a(b(x)); translation is in [2]/[5].
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

    // Display transforms are orthonormal, but guard the general inverse: singular means
    // identity rather than dividing by zero and poisoning every conjugated texdef.
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

    // A delta is a vector: apply only D^-1's linear part for displayed-space offsets.
    void XfApplyVec( const uvXform_t &x, float sIn, float tIn, float *sOut, float *tOut )
    {
        const float s = x.m[0] * sIn + x.m[1] * tIn;
        const float t = x.m[3] * sIn + x.m[4] * tIn;
        *sOut = s;
        *tOut = t;
    }

    // Convert a displayed-space gesture to true ST. Identity D leaves A bit-identical.
    uvXform_t XfConjugate( const uvXform_t &A, const uvXform_t &D )
    {
        if ( XfIsIdentity( D ) )
            return A;
        return XfMul( XfInverse( D ), XfMul( A, D ) );
    }

    // Gathered selection.
    struct uvBrush_t
    {
        selbrush_t  *node;    // instance node; a null unresolved face row is never dereferenced
        brush_t     *def;
        patchMesh_t *pm;      // non-null means this row is a patch
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

    // Display transforms parallel s_faces/s_patches; identity means true ST position.
    std::vector<uvXform_t> s_faceD;
    std::vector<uvXform_t> s_patchD;
    // Gather resets D each frame, so replay this index-parallel cache while row structure matches.
    std::vector<uvXform_t> s_chainD;
    std::vector<uvXform_t> s_chainPD;
    bool                   s_chain      = true;   // toolbar toggle, session-only (no registry)
    unsigned               s_chainStructSig = 0;  // (gathered rows, order, anchor, toggle)
    bool                   s_chainBuilt = false;
    int                    s_chainFolded = 0;     // how many faces the fold actually placed
    int                    s_chainShelved = 0;    // ...and how many had to be shelved

    // Window-local targets parallel s_faces/s_patches; no state here changes 3D selection.
    std::vector<char>      s_faceTgt;
    std::vector<char>      s_patchTgt;
    bool                   s_targetAll = true;
    bool                   s_targetNone = false;
    // Durable target keys are re-matched after each gather and never dereferenced.
    struct uvTgtKey_t { brush_t *def; int faceIndex; patchMesh_t *pm; };
    std::vector<uvTgtKey_t> s_targetKeys;
    // Cleared only by Sel_Generation changes: local targets survive ordinary frame gathers,
    // while a real 3D selection change re-seeds them from explicit selected faces.
    bool                   s_tgtSeeded = false;

    // Written where the window's ImGui focus/hover is available; false when not visible.
    bool                   s_inScope   = false;

    // Every live frame rebuilds from this gesture-start snapshot.
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

    // View state.
    float    s_panU = 0.0f, s_panV = 0.0f;   // UV coord at the canvas top-left
    float    s_zoom = 128.0f;                // canvas pixels per texture repeat
    bool     s_zoomValid = false;            // TB UvViewHelper::m_zoomValid (:79-83)
    float    s_originU = 0.0f, s_originV = 0.0f;
    int      s_subX = 1, s_subY = 1;         // TB subDivisions, range 1..16
    unsigned s_selGen = 0;                   // Sel_Generation() the view was framed for
    bool     s_haveGen = false;

    // Gesture state.
    enum uvGesture_t { UVG_NONE = 0, UVG_ROTATE, UVG_ORIGIN, UVG_SCALE, UVG_SHEAR,
                       UVG_MOVE, UVG_MARQUEE, UVG_PAN };

    // Load-bearing order: 0..3 are corners; 4..7 are edge midpoints.
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
    int         s_gHandle  = UVH_NONE;
    bool        s_gOnShape = false;                   // the press landed on a shape body
    bool        s_gShift   = false;                   // modifier snapshot from the press
    bool        s_gCtrl    = false;
    float       s_gOriginU = 0.0f, s_gOriginV = 0.0f; // origin at press (origin drag)
    // Scale pins the opposite handle from the deflated box; other transforms use the pivot.
    float       s_gAnchorU = 0.0f, s_gAnchorV = 0.0f;
    float       s_gStartAngle = 0.0f;                 // TB computeInitialAngle grab offset
    char        s_status[192] = { 0 };

    // Until dragged, the pivot follows the displayed target bbox center; selection clears the latch.
    bool        s_originUser = false;

    // Stacked-shape click cycle.
    float       s_cycleU = 0.0f, s_cycleV = 0.0f;
    int         s_cycleNext  = 0;
    bool        s_cycleValid = false;

    // A press on one of several targets defers click-to-collapse until release; a drag moves all.
    bool        s_collapseArmed = false;
    int         s_collapseFace  = -1;
    int         s_collapsePatch = -1;
    int         s_collapseNext  = 0;    // the cycle index the collapse hands on
    bool        s_removeArmed   = false;
    int         s_removeFace    = -1;
    int         s_removePatch   = -1;

    // Gesture-start snap candidates in displayed space.
    struct uvPt_t   { float u, v; };
    struct uvEdge_t { float u0, v0, u1, v1; };
    std::vector<uvPt_t>   s_gTgtPts;     // the DRAGGED shapes' displayed outline points
    std::vector<uvPt_t>   s_gOtherPts;   // ...and everything else's, as snap targets
    std::vector<uvEdge_t> s_gTgtEdges;   // the dragged outlines' edges (angle / slope snaps)

    // Canvas rect for this frame, in screen pixels.
    ImVec2 s_c0( 0.0f, 0.0f );
    ImVec2 s_cs( 0.0f, 0.0f );

    // Small helpers.
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
        // Avoid the MtlDef_IsValid L0 assertions in per-frame material accessors.
        return md && ( ( md->lyrMtl != 0 ) + ( md->radMtl != 0 ) ) == 1;
    }

    // MaterialDef shares the patchMesh_material pair layout; copied from
    // Patch_RotateTexture (pmesh.cpp:3259).
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

    // Gather every selected face and patch once per frame. Live lists need no liveness
    // check; stored snapshot/target pointers are only re-matched against a fresh gather.
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

    void StoreTargetKeys();
    void TargetAll();
    void TargetSingle( int face, int patch );

    void Gather()
    {
        // DrawCanvas updates s_selGen later, so this gather sees and seeds a selection change.
        if ( !s_haveGen || Sel_Generation() != s_selGen )
            s_tgtSeeded = false;

        s_brushes.clear();
        s_faces.clear();
        s_patches.clear();
        s_activeFace = -1;

        // Whole-selected patch brushes contribute their patch, not derived box faces.
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

        // Face-selected brushes are absent from selected_brushes (kiwi_selection.h).
        // HaveFace also protects against any future overlapping selection source.
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

        // The active face supplies the background, texel size, and chain anchor.
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

        // Rebuild index-parallel display and target arrays every frame.
        s_faceD.assign( s_faces.size(), XfIdentity() );
        s_patchD.assign( s_patches.size(), XfIdentity() );
        s_faceTgt.assign( s_faces.size(), 1 );
        s_patchTgt.assign( s_patches.size(), 1 );
        if ( !s_targetAll )
        {
            // Re-match the stored keys against THIS gather.  A key that no longer names a
            // gathered row simply drops out.  An explicit Ctrl-removed empty target set is
            // preserved; stale non-empty keys still fall back to all.
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
                if ( s_targetNone )
                {
                    s_faceTgt.assign( s_faces.size(), 0 );
                    s_patchTgt.assign( s_patches.size(), 0 );
                }
                else
                {
                    s_targetAll = true;
                    s_targetKeys.clear();
                    s_faceTgt.assign( s_faces.size(), 1 );
                    s_patchTgt.assign( s_patches.size(), 1 );
                }
            }
        }

        // Any 3D selection change replaces stale local keys: explicit faces seed that
        // subset; explicit faces covering every shape and whole-brush-only selections use
        // plain "all". Local targeting survives only gathers with the same generation.
        if ( !s_tgtSeeded )
        {
            s_tgtSeeded = true;
            const int nExplicit = g_SelectedFaces.GetSize();
            const int nShapes   = (int)( s_faces.size() + s_patches.size() );
            if ( nExplicit <= 0 && nShapes > 0 )
            {
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
                // Avoid a redundant subset covering all; Probe treats s_targetAll as a mode.
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
        s_targetNone = !s_targetAll && s_targetKeys.empty();
    }

    void TargetAll()
    {
        s_targetAll = true;
        s_targetNone = false;
        s_targetKeys.clear();
        s_faceTgt.assign( s_faces.size(), 1 );
        s_patchTgt.assign( s_patches.size(), 1 );
    }

    // Per-face UV read, matching the ported write path.
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

    // tdOverride reads ST from a gesture-start texdef rather than the frame being written.
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

    // Display transforms. Only XfConjugate may carry D into the texdef/control-ST write path.

    // Face winding in displayed ST; drawing and hit-testing share this path.
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

    // Displayed control-hull ring; a Bezier patch lies inside this inexpensive boundary.
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

    // Chain rebuild key: display layout depends on row structure, not mutable texdefs/ST.
    unsigned HashMix( unsigned h, unsigned v )
    {
        h ^= v;
        h *= 16777619u;            // FNV-1a
        return h;
    }

    // Cache stays index-parallel while row order, anchor, and Chain state match.
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

    // Chain build.
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

    // Rotate neighbor edge (b0,b1) to the anchor's displayed direction and pin b0 to A0.
    // Lengths deliberately remain unmatched because D is rigid and UV scales may differ.
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

    // Reflection across the line through P in direction (dx,dy).
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
            return;                                   // too many faces for a useful diagram

        // Gather world points, own-frame ST, and ST centroids once.
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

        // Weld world vertices into quantized buckets and build edges.
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

        // Equal welded-edge runs produce every cross-face adjacency.
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
                    // Match endpoints: b's i0 may correspond to a's i1.
                    const int wa0 = weldId[weldBase[s_ce[a].face] + s_ce[a].i0];
                    const int wb0 = weldId[weldBase[s_ce[b].face] + s_ce[b].i0];
                    if ( wb0 == wa0 ) { ad.b0 = s_ce[b].i0; ad.b1 = s_ce[b].i1; }
                    else              { ad.b0 = s_ce[b].i1; ad.b1 = s_ce[b].i0; }
                    s_ca.push_back( ad );
                }
            i = j;
        }

        // BFS from the active face, whose D remains identity.
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

                // Same-side centroids overlap, so reflect the neighbor across the edge.
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

        // Shelf unreachable faces to the right of the occupied box.
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

        // Leave patches at true ST unless >80% of their bbox overlaps the occupied box.
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

    // Ported texdef-writer tail from Brush_ShiftTexture (select.cpp:3158-3163): rebuild
    // windings/vertex selection, mark modified, bump the def version, then sync faceVis.
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

    // Transform the gesture-start texdef matrix, then use the binary's inverse decomposition.
    void TransformFace( face_t *fd, const texdef_sub_t *td0, const uvXform_t &A,
                        texdef_sub_t *out )
    {
        // A singular affine collapses a texture axis; texturevecs_02 would divide by its
        // zero row length. Refuse that frame and let the next rebuild use the snapshot.
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

        // texturevecs_02 consumes m as scratch and writes size/shift/rotate/crossterm byref.
        // Call shape matches Face_TexLock_Reproject (brush.cpp:2978-2980); arg 3 is the
        // ignored Hex-Rays x87 phantom.
        texturevecs_02( (int)(intptr_t)&out->size[0], (int)(intptr_t)m,
                        fd->plane.normal[2],
                        (int)(intptr_t)fd->plane.normal, fd->plane.dist,
                        (int)(intptr_t)&out->shift[0],
                        (int)(intptr_t)&out->rotate,
                        (int)(intptr_t)&out->crossterm );
    }

    // Exact translation: m3=-shift[0]/sx, so shift=shift0-d*size0, with the port's
    // zero-size substitution (texturevecs.cpp:108-109).
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

    // Patch_ShiftTexture uses these same `[2*layer + {0,1}]` slots (pmesh.cpp:3232-3238);
    // texCoord stores the three channels contiguously (qedefs.h:159-165).
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
        // Zero shift invokes only the ported rebuild tail (pmesh.cpp:3241-3246). This editor
        // intentionally transforms the whole displayed control grid even in curve-point mode.
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

    // Undo.
    void UndoOpen( const char *op )      // `op` MUST be a literal — the record stores it
    {
        if ( s_undoOpen )
            return;
        // A parked face-move baseline contains the whole MaterialDef and can overwrite this
        // edit. End it at the one point every mutating path crosses, before the first write.
        // A refusal is reported by KiwiUv_EndGestureBeforeApply but does not block this edit.
        KiwiUv_EndGestureBeforeApply( "UV editor" );
        KiwiCmd_UndoBegin( op );
        s_undoOpen = true;
        if ( !s_covered )
        {
            // Face-selected brushes are absent from selected_brushes. Cover every gathered
            // row before mutation; Undo_AddBrush self-deduplicates (undo.cpp:485).
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

    // Canvas transform.
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

    // Reject UV bounds that cannot safely become integer tile/grid loop indices; tiny typed
    // texture sizes can otherwise drive ST near 1e9 and make `(int)floorf` undefined.
    const float KUVE_UV_SANE = 1.0e6f;
    bool VisibleUvRect( float *u0, float *v0, float *u1, float *v1 )
    {
        PxToUv( s_c0, u0, v0 );
        PxToUv( ImVec2( s_c0.x + s_cs.x, s_c0.y + s_cs.y ), u1, v1 );
        return fabsf( *u0 ) < KUVE_UV_SANE && fabsf( *u1 ) < KUVE_UV_SANE
            && fabsf( *v0 ) < KUVE_UV_SANE && fabsf( *v1 ) < KUVE_UV_SANE;
    }

    // TB resetZoom (UvViewHelper.cpp:316-348): fit with clamped 10% margins and retry a
    // size-zero first frame. Fit the complete displayed spread; AutoPivot owns the pivot.
    void FrameActive()
    {
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

        s_panU = 0.5f * ( mn[0] + mx[0] ) - 0.5f * s_cs.x / s_zoom;
        s_panV = 0.5f * ( mn[1] + mx[1] ) - 0.5f * s_cs.y / s_zoom;
        s_zoomValid = true;
    }

    // Active material colormap, borrowing the MAPTYPE_2D path from kiwi_skybox.cpp:451-496.
    // The three guards are load-bearing:
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

    // Preserve face/patch hue while dimming non-target alpha.
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
            const int n = FaceStPointsDisp( i, st, nullptr );
            if ( n < 2 )
                continue;
            for ( int k = 0; k < n; ++k )
                pts[k] = UvToPx( st[k][0], st[k][1] );
            // Gold denotes targets; the active face only supplies material and fold anchor.
            const bool tgt = FaceIsTarget( i );
            dl->AddPolyline( pts, n, DimIf( tgt ? KUVE_COL_ACTIVE : KUVE_COL_FACE, tgt ),
                             ImDrawFlags_Closed, tgt ? 2.5f : 1.5f );
        }

        for ( size_t pi = 0; pi < s_patches.size(); ++pi )
        {
            patchMesh_t *p = s_brushes[s_patches[pi]].pm;
            if ( !p )
                continue;
            // Targeted patches are gold; their control net still distinguishes the family.
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

    // Hit tests use the pivot disc and transform-box handles, never grid lines. The pivot
    // follows TB UvOriginTool::pick (:328-374), with a pick radius larger than its drawing.
    bool PickPivot( float u, float v )
    {
        const float du = ( u - s_originU ) * s_zoom;
        const float dv = ( v - s_originV ) * s_zoom;
        return sqrtf( du * du + dv * dv ) <= KUVE_ORIGIN_PICK_RAD;
    }

    // Inflated transform box in canvas pixels; invalid without a drawable target outline.
    struct uvBox_t
    {
        bool  valid;
        float x0, y0, x1, y1;
    };
    uvBox_t s_box = { false, 0.0f, 0.0f, 0.0f, 0.0f };

    // Displayed target bbox in UV. Patches use the whole net because interior controls can
    // extend beyond the outer ring.
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

    // Scale/skew use the uninflated shape bbox; using the drawn outset would offset the
    // grabbed point by `inflation * (factor - 1)`.
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

    // Follow the displayed target bbox until the user places the pivot; selection clears it.
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

    // Canvas-pixel handle centers in uvHandle_t order.
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

    // Opposite handle supplies the fixed scale anchor; an edge scale ignores its other axis.
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

    // Hide handles on tiny boxes so their pick squares do not consume the body drag. Pick
    // and draw share this predicate.
    bool BoxHasHandles( const uvBox_t &b )
    {
        return b.valid && ( b.x1 - b.x0 ) >= KUVE_BOX_MIN_PX
                       && ( b.y1 - b.y0 ) >= KUVE_BOX_MIN_PX;
    }

    // Test handle squares first, then corner rotate annuli only outside the box body.
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

    // Shape hit-testing in displayed space.
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

    // Pixel distance to the boundary lets thin face slivers be picked by their outline.
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

    // Return all hits in reverse draw order: patches over faces, later rows over earlier.
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

    // Only a repeat press within KUVE_CYCLE_PX advances the stack cycle.
    bool CycleIsRepeat( float u, float v )
    {
        return s_cycleValid
            && fabsf( u - s_cycleU ) * s_zoom <= KUVE_CYCLE_PX
            && fabsf( v - s_cycleV ) * s_zoom <= KUVE_CYCLE_PX;
    }

    // Pure lookup so hover can preview the same cycle entry without changing state.
    int CycleIndexFor( float u, float v, int n )
    {
        if ( n < 1 )
            return 0;
        if ( CycleIsRepeat( u, v ) )
            return ( ( s_cycleNext % n ) + n ) % n;
        return 0;
    }

    // Marquee intersection includes vertices, crossing edges, or a band inside the outline.
    // Segment-vs-AABB uses Liang-Barsky.
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

    // One probe supplies both hover feedback and press behavior.
    struct uvProbe_t
    {
        uvGesture_t what;        // what a press here would BEGIN (UVG_NONE == nothing)
        bool        axis[2];     // for SCALE / SHEAR
        int         handle;      // uvHandle_t, or UVH_NONE
        bool        rotateZone;  // the press is in a corner's rotate annulus
        bool        shearRefused;
        int         shapeFace, shapePatch;   // shape a press would target
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

    // Pick order: pivot, box handles, shape, box body, marquee. Ctrl-click removes;
    // Ctrl-drag retains transform snapping.
    uvProbe_t Probe( float u, float v, bool altDown )
    {
        uvProbe_t p;
        ProbeClear( &p );
        if ( !AnythingSelected() )
            return p;

        const ImVec2 px = UvToPx( u, v );
        p.inBox = InBox( s_box, px.x, px.y );

        // Pivot is first because it is the smallest target and lies inside the box.
        if ( PickPivot( u, v ) )
        {
            p.what    = UVG_ORIGIN;
            p.axis[0] = p.axis[1] = true;     // the disc is the only pivot grab now
            return p;
        }

        // Box handles: scale, corner-annulus rotate, or Alt+edge-mid skew.
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
                // TB issue #1350: shear divides by the handle's other-axis pivot offset;
                // refuse a near-zero divisor (UvShearTool.cpp:284-289).
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

        // A shape body targets and moves; hover previews the same stack-cycle entry.
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

        // Empty space inside an explicit target box drags it; otherwise it marquees.
        p.what = ( p.inBox && !s_targetAll ) ? UVG_MOVE : UVG_MARQUEE;
        return p;
    }

    // TrenchBroom snapping adapted to repeat units. Point/axis absorbs draw KIWI's dot/ring
    // glyph plus a tick along the locked axis; rotate reports its angle lock in the status.
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

    // Status clause naming the active snap.
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

    // TB UvViewHelper::snapDelta (:198-209): absorb inside 8 px; otherwise round the
    // repeat-space delta to whole texels. Only the visible absorb is reported/marked.
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

    // Signed distance to the nearest subdivision line or displayed vertex not being dragged.
    // Whole texels remain SnapDeltaAxis's fallback quantum.
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

    // TB UvOffsetTool::snapDelta (:56-75): componentwise nearest candidate over every
    // moved vertex; retain the winning vertex so the accent marks what actually snapped.
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

    // TB UvScaleTool::snap (:103-127): absorb the moved handle within 8 px per axis.
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

    // TB UvRotateTool::snapAngle (:72-108), expressed for delta theta: candidates making
    // a captured displayed edge square are `-edgeAngle + 90k`. Distance is already pixels,
    // so the threshold is 150/pow(d,0.8) without TB's world-space `/zoom`.
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

    // TB UvShearTool::snapShearFactors (:85-112): `-v[1-axis]/v[axis]` makes an edge
    // axis-aligned. The formula is sign-independent even though this canvas flips drag sign.
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

    // TB UvOriginTool::snapDelta (:97-157) combines coordinate minima from different
    // vertices; use the nearest actual 2D candidate point within the 8 px radius instead.
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

        // The pivot belongs to no shape, so both captured displayed-space point sets apply.
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
        // Also offer the target centroid, which no vertex necessarily represents.
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

    // Gesture lifecycle.
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

    // Capture capped displayed-space snap sets once; live reads would chase rewritten ST.
    void CaptureSnapSets()
    {
        s_gTgtPts.clear();
        s_gOtherPts.clear();
        s_gTgtEdges.clear();
        SnapClear();

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

    // Re-match snapshot keys against the fresh gather before dereferencing them.
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

    // Write targets from the snapshot. offsetDelta selects the exact face-translation path;
    // a patch translation uses the equivalent affine arithmetic.
    void ApplySelection( const uvXform_t &A, const float *offsetDelta, const char *undoName )
    {
        if ( !SnapshotStillMatches() )
            return;
        // Skip initial identity to avoid an empty undo record. After any write, identity
        // must restore the snapshot when the cursor returns to its start.
        if ( XfIsIdentity( A ) && !s_undoOpen )
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
            texdef_sub_t out = s_snapFaces[i].td;
            // A is displayed-space; conjugate it into this face's true ST frame.
            const uvXform_t D = ( i < s_faceD.size() ) ? s_faceD[i] : XfIdentity();
            if ( offsetDelta )
            {
                // Translation uses only D^-1's linear part.
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
            TexMatToFakeTexCoords( md, td );     // ported texdef-writer tail
        }

        for ( size_t i = 0; i < s_snapPatches.size(); ++i )
        {
            if ( !PatchIsTarget( i ) )
                continue;
            const uvXform_t D = ( i < s_patchD.size() ) ? s_patchD[i] : XfIdentity();
            TransformPatch( s_snapPatches[i].pm, s_snapPatches[i].st, XfConjugate( A, D ) );
        }

        // Rebuild once per brush rather than once per face. Patch_ShiftTexture rebuilds the
        // mesh, not its owning brush (select.cpp:3095-3136).
        for ( size_t i = 0; i < s_brushes.size(); ++i )
            TouchBrush( s_brushes[i].node, s_brushes[i].def );

        g_nUpdateBits = -1;
    }

    // Restore the exact stored snapshot, not an inverse delta (kiwi_uv.h:83-86).
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

    // Marquee grammar: plain replaces, Shift adds, Ctrl removes displayed-outline hits.
    void ApplyMarquee( float u0, float v0, float u1, float v1,
                       bool extend, bool remove )
    {
        float r0[2] = { u0 < u1 ? u0 : u1, v0 < v1 ? v0 : v1 };
        float r1[2] = { u0 < u1 ? u1 : u0, v0 < v1 ? v1 : v0 };

        std::vector<char> ft( s_faces.size(),   0 );
        std::vector<char> pt( s_patches.size(), 0 );
        if ( ( extend || remove ) && s_targetAll )
        {
            ft.assign( s_faces.size(), 1 );
            pt.assign( s_patches.size(), 1 );
        }
        else if ( extend || remove )
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
                ft[i] = remove ? 0 : 1;
                ++hits;
            }
        }
        for ( size_t pi = 0; pi < s_patches.size(); ++pi )
        {
            const int n = PatchRing( pi, poly );
            if ( n >= 2 && PolyHitsRect( poly, n, r0, r1 ) )
            {
                pt[pi] = remove ? 0 : 1;
                ++hits;
            }
        }

        int total = 0;
        for ( size_t i = 0; i < ft.size(); ++i ) total += ft[i];
        for ( size_t i = 0; i < pt.size(); ++i ) total += pt[i];
        if ( !hits && !extend && !remove )
        {
            TargetAll();
            _snprintf( s_status, sizeof( s_status ), "targeting all shapes" );
        }
        else if ( !hits )
        {
            return;                                  // additive/removal miss
        }
        else if ( total == (int)( s_faces.size() + s_patches.size() ) )
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
        s_cycleValid = false;
    }

    void RemoveTarget( int face, int patch )
    {
        if ( s_targetAll )
        {
            s_targetAll = false;
            s_faceTgt.assign( s_faces.size(), 1 );
            s_patchTgt.assign( s_patches.size(), 1 );
        }

        bool changed = false;
        if ( face >= 0 && (size_t)face < s_faceTgt.size() && s_faceTgt[face] )
        {
            s_faceTgt[face] = 0;
            changed = true;
        }
        else if ( patch >= 0 && (size_t)patch < s_patchTgt.size() && s_patchTgt[patch] )
        {
            s_patchTgt[patch] = 0;
            changed = true;
        }
        if ( !changed )
            return;

        StoreTargetKeys();
        AutoPivot();
        _snprintf( s_status, sizeof( s_status ), "targeting %d shape%s",
                   TargetCount(), TargetCount() == 1 ? "" : "s" );
        s_status[sizeof( s_status ) - 1] = '\0';
        s_cycleValid = false;
    }

    void GestureEnd()
    {
        if ( s_gesture == UVG_MARQUEE )
        {
            const float dx = ( s_gCurU - s_gStartU ) * s_zoom;
            const float dy = ( s_gCurV - s_gStartV ) * s_zoom;
            const bool  extend = s_gShift && !s_gCtrl;
            const bool  remove = s_gCtrl;
            if ( fabsf( dx ) < KUVE_MARQUEE_MIN_PX && fabsf( dy ) < KUVE_MARQUEE_MIN_PX )
            {
                // A click rather than a band restores all targets.
                if ( !extend && !remove )
                {
                    if ( !s_targetAll )
                        _snprintf( s_status, sizeof( s_status ), "targeting all shapes" );
                    TargetAll();
                }
            }
            else
                ApplyMarquee( s_gStartU, s_gStartV, s_gCurU, s_gCurV,
                              extend, remove );
        }
        else if ( s_gesture == UVG_MOVE && !s_gOnShape )
        {
            // A click in an explicit target box's empty interior also restores all.
            const float dx = ( s_gCurU - s_gStartU ) * s_zoom;
            const float dy = ( s_gCurV - s_gStartV ) * s_zoom;
            if ( fabsf( dx ) < KUVE_MARQUEE_MIN_PX && fabsf( dy ) < KUVE_MARQUEE_MIN_PX
                 && !s_gShift && !s_gCtrl )
            {
                if ( !s_targetAll )
                    _snprintf( s_status, sizeof( s_status ), "targeting all shapes" );
                TargetAll();
            }
        }
        else if ( s_gesture == UVG_MOVE && s_gOnShape && s_removeArmed )
        {
            const float dx = ( s_gCurU - s_gStartU ) * s_zoom;
            const float dy = ( s_gCurV - s_gStartV ) * s_zoom;
            if ( fabsf( dx ) < KUVE_MARQUEE_MIN_PX && fabsf( dy ) < KUVE_MARQUEE_MIN_PX )
            {
                RestoreSnapshot();
                if ( s_undoOpen )
                {
                    KiwiCmd_UndoCancel();
                    s_undoOpen = false;
                    s_covered  = false;
                }
                RemoveTarget( s_removeFace, s_removePatch );
            }
        }
        else if ( s_gesture == UVG_MOVE && s_gOnShape && s_collapseArmed )
        {
            // Below the shared drag threshold, collapse the multi-target set on release.
            const float dx = ( s_gCurU - s_gStartU ) * s_zoom;
            const float dy = ( s_gCurV - s_gStartV ) * s_zoom;
            if ( fabsf( dx ) < KUVE_MARQUEE_MIN_PX && fabsf( dy ) < KUVE_MARQUEE_MIN_PX
                 && !s_gShift )
            {
                TargetSingle( s_collapseFace, s_collapsePatch );
                s_cycleNext  = s_collapseNext;   // the next click here steps the stack
                s_cycleValid = true;
                AutoPivot();                     // pivot follows the target set
                _snprintf( s_status, sizeof( s_status ), "targeting 1 shape" );
                s_status[sizeof( s_status ) - 1] = '\0';
            }
        }
        else if ( s_gesture == UVG_ORIGIN )
        {
            // A moved pivot stops following the target set.
            if ( fabsf( s_originU - s_gOriginU ) * s_zoom > 0.5f
                 || fabsf( s_originV - s_gOriginV ) * s_zoom > 0.5f )
                s_originUser = true;
        }

        UndoCommit();
        s_collapseArmed = false;
        s_removeArmed   = false;
        s_gesture = UVG_NONE;
        s_gButton = -1;
        s_gHandle = UVH_NONE;
        s_snapFaces.clear();
        s_snapPatches.clear();
        s_gTgtPts.clear();
        s_gOtherPts.clear();
        s_gTgtEdges.clear();
        SnapClear();
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
        {
            RestoreSnapshot();
            // Exact restore plus undo cancellation matches kiwi_uv.cpp:345-353.
            if ( s_undoOpen )
            {
                KiwiCmd_UndoCancel();
                s_undoOpen = false;
                s_covered  = false;
            }
        }
        s_collapseArmed = false;
        s_removeArmed   = false;
        s_gesture = UVG_NONE;
        s_gButton = -1;
        s_gHandle = UVH_NONE;
        s_snapFaces.clear();
        s_snapPatches.clear();
        s_gTgtPts.clear();
        s_gOtherPts.clear();
        s_gTgtEdges.clear();
        SnapClear();
        _snprintf( s_status, sizeof( s_status ), "cancelled" );
        s_status[sizeof( s_status ) - 1] = '\0';
        g_nUpdateBits = -1;
    }

    // Closed/collapsed paths have no fresh gather, so do not dereference stored brush keys.
    // The undo record is the authoritative restore and must not survive the frame.
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
        s_collapseArmed = false;
        s_removeArmed   = false;
        s_gesture = UVG_NONE;
        s_gButton = -1;
        s_gHandle = UVH_NONE;
        s_snapFaces.clear();
        s_snapPatches.clear();
        s_gTgtPts.clear();
        s_gOtherPts.clear();
        s_gTgtEdges.clear();
        SnapClear();
        g_nUpdateBits = -1;
    }

    // Press lifecycle uses the same Probe result as hover.

    // Make one shape the local target set without changing 3D selection.
    void TargetSingle( int face, int patch )
    {
        s_targetAll = false;
        s_targetNone = false;
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

        // Clear deferred click actions before every early return.
        s_collapseArmed = false;
        s_removeArmed   = false;

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

        // The same probe drives hover and press.
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
        s_gCtrl    = io.KeyCtrl;
        s_gButton  = button;
        s_gHandleU = u;
        s_gHandleV = v;
        s_gAnchorU = s_originU;
        s_gAnchorV = s_originV;
        if ( pr.handle != UVH_NONE && s_box.valid )
        {
            // Measure from the uninflated handle center, not an off-center press point.
            const uvBox_t raw = DeflateBox( s_box );
            float hx, hy;
            HandlePx( raw, pr.handle, &hx, &hy );
            PxToUv( ImVec2( hx, hy ), &s_gHandleU, &s_gHandleV );
            // Scale pins the opposite handle; skew keeps the pivot.
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
            // Pivot snapping needs displayed point sets but no texdef snapshot.
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
            s_gesture = UVG_MARQUEE;
            return;
        default:
            break;                           // UVG_MOVE
        }

        // Shape press targets and drags; repeats cycle stacked hits. Pressing one of several
        // targets moves the set and defers click-to-collapse until release.
        if ( s_gOnShape )
        {
            uvShapeRef_t stack[64];
            const int    n      = CollectShapesAt( u, v, stack, 64 );
            const int    k      = CycleIndexFor( u, v, n );
            const bool   repeat = CycleIsRepeat( u, v );

            if ( io.KeyCtrl )
            {
                if ( !pr.shapeIsTarget )
                {
                    s_gesture = UVG_NONE;
                    s_gButton = -1;
                    return;
                }
                // Stationary Ctrl release removes; Ctrl-drag snap-moves.
                s_removeArmed = true;
                s_removeFace  = pr.shapeFace;
                s_removePatch = pr.shapePatch;
                _snprintf( s_status, sizeof( s_status ),
                           "Ctrl-click removes; Ctrl-drag snap-moves %d shape%s",
                           TargetCount(), TargetCount() == 1 ? "" : "s" );
                s_status[sizeof( s_status ) - 1] = '\0';
                s_cycleU     = u;
                s_cycleV     = v;
                s_cycleNext  = ( n > 0 ) ? ( k + 1 ) % n : 0;
                s_cycleValid = true;
                s_gesture = UVG_MOVE;
                SnapshotSelection();
                CaptureSnapSets();
                return;
            }
            else if ( io.KeyShift )
            {
                // Shift is add-only; an existing member is a no-op.
                if ( !pr.shapeIsTarget )
                {
                    s_targetAll  = false;
                    s_targetNone = false;
                    if ( pr.shapeFace >= 0 )
                        s_faceTgt[pr.shapeFace] = 1;
                    else
                        s_patchTgt[pr.shapePatch] = 1;
                    StoreTargetKeys();
                }
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

            // Membership/count, not s_targetAll's storage mode, decides whether to keep a set.
            const bool keepSet = ( !repeat && pr.shapeIsTarget && TargetCount() > 1 );
            if ( keepSet )
            {
                // Defer collapse until release; a drag must retain and move the whole set.
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

        // Retarget before the gesture goes live so an automatic pivot follows immediately.
        AutoPivot();

        s_gesture = UVG_MOVE;
        SnapshotSelection();
        CaptureSnapSets();               // partition after retargeting
    }

    void GestureUpdate( float u, float v )
    {
        ImGuiIO &io = ImGui::GetIO();
        // Canvas transforms are raw by default; Ctrl engages snapping from ImGui frame state.
        // Ctrl-click remains removal until motion crosses the click threshold.
        const bool snap = io.KeyCtrl;
        s_gCurU = u;
        s_gCurV = v;

        switch ( s_gesture )
        {
        case UVG_PAN:
            // Keep the grabbed UV under the cursor (TB UvCameraTool.cpp:44-55).
            s_panU += s_gStartU - u;
            s_panV += s_gStartV - v;
            break;
        case UVG_MARQUEE:
            _snprintf( s_status, sizeof( s_status ), "marquee  %+.3f x %+.3f repeats",
                       u - s_gStartU, v - s_gStartV );
            break;
        case UVG_ORIGIN:
        {
            // Pivot drag is free in both S and T.
            float nu = s_gOriginU + ( u - s_gStartU );
            float nv = s_gOriginV + ( v - s_gStartV );
            SnapClear();
            if ( snap )
            {
                const float ru = nu, rv = nv;
                SnapOrigin( &nu, &nv );
                if ( fabsf( nu - ru ) > 1.0e-6f || fabsf( nv - rv ) > 1.0e-6f )
                {
                    // Mark both axes so a 2D point snap reads as a cross.
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
            SnapClear();
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
            SnapClear();
            if ( snap )
            {
                const float raw = theta;
                theta = SnapAngle( theta, sqrtf( du * du + dv * dv ) );
                // Angle snaps use only the readout; a point marker would be misleading.
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
            // TB UvScaleTool (:196-254): scale is moved-handle distance divided by its
            // start distance. Here the opposite box handle is the fixed anchor; Shift on
            // corners uses one least-squares factor for both axes.
            float k[2] = { 1.0f, 1.0f };
            const float o[2]  = { s_gAnchorU, s_gAnchorV };
            const float h0[2] = { s_gHandleU, s_gHandleV };
            float       h1[2] = { s_gHandleU + ( u - s_gStartU ),
                                  s_gHandleV + ( v - s_gStartV ) };
            SnapClear();
            if ( snap )
            {
                // Snap the dragged handle on each armed axis.
                bool hit[2]  = { false, false };
                int  kind[2] = { KUVE_SNAPK_GRID, KUVE_SNAPK_GRID };
                for ( int i = 0; i < 2; ++i )
                    if ( s_gAxis[i] )
                        h1[i] = SnapHandleAxis( h1[i], i, &hit[i], &kind[i] );
                // Record accents after both axes so a corner uses the final 2D point.
                for ( int i = 0; i < 2; ++i )
                    if ( hit[i] )
                        SnapNote( h1[0], h1[1], i, kind[i] );
            }
            const bool uniform = ( s_gHandle >= UVH_NW && s_gHandle <= UVH_SW )
                                 && io.KeyShift;
            if ( uniform )
            {
                // Least-squares scalar: the diagonal component decides both axes.
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
            // TB UvShearTool (:159-208), rebuilt from the gesture start each frame. The
            // sign is flipped so the wireframe follows the cursor; reference the edge-mid
            // handle rather than an off-center press point.
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

    // Overlays consume the same frame probe as press handling.
    void DrawOverlays( ImDrawList *dl, bool hovered, const uvProbe_t &pr )
    {
        const ImVec2 o = UvToPx( s_originU, s_originV );
        const bool   idle = ( s_gesture == UVG_NONE ) && hovered;

        // Highlight the shape/cycle entry the next press will target and drag.
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
                ImU32 hoverCol = IM_COL32( 255, 255, 255, 140 );
                if ( ImGui::GetIO().KeyCtrl && pr.shapeIsTarget )
                {
                    float rgb[3];
                    KiwiHover_PreviewColor( true, rgb );
                    hoverCol = ImGui::ColorConvertFloat4ToU32(
                        ImVec4( rgb[0], rgb[1], rgb[2], 0.92f ) );
                }
                dl->AddPolyline( pts, n, hoverCol,
                                 ImDrawFlags_Closed, 3.5f );
            }
        }

        // Draw the same box and hot handle that Probe tested.
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
            // Draw a rotate arc only while its corner annulus is hot/live.
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

        // Marquee.
        if ( s_gesture == UVG_MARQUEE )
        {
            const ImVec2 a = UvToPx( s_gStartU, s_gStartV );
            const ImVec2 b = UvToPx( s_gCurU,   s_gCurV );
            const ImVec2 r0( a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y );
            const ImVec2 r1( a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y );
            ImU32 fill = IM_COL32( 255, 255, 255, 30 );
            ImU32 edge = IM_COL32( 255, 255, 255, 190 );
            if ( s_gCtrl )
            {
                float rgb[3];
                KiwiHover_PreviewColor( true, rgb );
                fill = ImGui::ColorConvertFloat4ToU32(
                    ImVec4( rgb[0], rgb[1], rgb[2], 30.0f / 255.0f ) );
                edge = ImGui::ColorConvertFloat4ToU32(
                    ImVec4( rgb[0], rgb[1], rgb[2], 190.0f / 255.0f ) );
            }
            dl->AddRectFilled( r0, r1, fill );
            dl->AddRect( r0, r1, edge, 0.0f, 0, 1.0f );
        }

        // Engaged absorbs draw a locked-axis tick plus dot/ring beneath the pivot.
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

        // Pivot cross names the S/T directions without full-canvas pickable axis lines.
        const bool pivotHot = ( s_gesture == UVG_ORIGIN )
                            || ( idle && pr.what == UVG_ORIGIN );
        dl->AddLine( ImVec2( o.x - KUVE_ORIGIN_CROSS_PX, o.y ),
                     ImVec2( o.x + KUVE_ORIGIN_CROSS_PX, o.y ), KUVE_COL_UAXIS, 1.5f );
        dl->AddLine( ImVec2( o.x, o.y - KUVE_ORIGIN_CROSS_PX ),
                     ImVec2( o.x, o.y + KUVE_ORIGIN_CROSS_PX ), KUVE_COL_VAXIS, 1.5f );
        dl->AddCircleFilled( o, KUVE_ORIGIN_RADIUS_PX,
                             pivotHot ? KUVE_COL_HANDLE_HI : KUVE_COL_HANDLE, 16 );
    }

    // Recompute every frame: ImGui resets to Arrow in NewFrame, while the Win32 backend
    // calls SetCursor only when the requested value changes (imgui.cpp:6012,
    // imgui_impl_win32.cpp:534-538).
    void ApplyHoverCursor( const uvProbe_t &pr, bool hovered )
    {
        if ( !hovered || s_gesture != UVG_NONE )
            return;
        switch ( pr.what )
        {
        case UVG_ROTATE:
            // ImGui 1.92 has no rotate cursor, so rotate and pivot use Hand.
            ImGui::SetMouseCursor( ImGuiMouseCursor_Hand );
            break;
        case UVG_ORIGIN:
            ImGui::SetMouseCursor( ImGuiMouseCursor_Hand );
            break;
        case UVG_SCALE:
        case UVG_SHEAR:
            // Match each corner to its diagonal resize cursor.
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

    // Toolbar.
    texdef_sub_t *ActiveTexdef( MaterialDef **outMd )
    {
        if ( s_activeFace < 0 )
            return nullptr;
        return KiwiUv_FaceTexdef( s_brushes[s_faces[s_activeFace].brushIdx].def,
                                  s_faces[s_activeFace].faceIndex, outMd );
    }

    // Immediate numeric texdef edits affect targeted faces only. Flip/Rot 90 use the
    // pivoted affine path instead of raw fields, which would pivot at texture-space (0,0).
    enum uvField_t { UVF_SHIFT0 = 0, UVF_SHIFT1, UVF_SIZE0, UVF_SIZE1, UVF_ROTATE,
                     UVF_CROSSTERM, UVF_RESET };

    // Per-face write without bracket management, allowing mixed operations one undo record.
    void ApplyFieldInner( uvField_t field, float value )
    {
        const float sample = g_qeglobals.random_texture_stuff[TemplateLayer()].sampleSize;

        for ( size_t i = 0; i < s_faces.size(); ++i )
        {
            if ( !FaceIsTarget( i ) )
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
                // TexWnd_BuildClickedMaterialDef default (texwnd.cpp:674-697).
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

    // One-shot transforms use the gesture arithmetic: displayed A, conjugated per shape;
    // faces use Face_MoveTexture/texturevecs_02 and patches use control ST. With no gesture
    // snapshot, each current shape state is td0. Faces and patches share one undo record.
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
                // Patch_NaturalizeSelected owns its bracket, so mixed Reset is two undo
                // steps (pmesh.cpp:2755-2757; mainfrm.cpp:3895).
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

        // Canvas +T is down, so [[0,-1],[1,0]] maps right to down: clockwise on screen.
        // Faces and patches use the same affine about the displayed pivot.
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

        ImGui::SameLine();
        if ( ImGui::Checkbox( "Chain", &s_chain ) )
            s_chainBuilt = false;                 // rebuild (or drop) on the next frame
        if ( ImGui::IsItemHovered() )
            ImGui::SetTooltip( "Fold multi-face selections out like paper angels: faces that\n"
                               "share a world edge are unfolded along it so they stop stacking\n"
                               "on top of each other.  DISPLAY ONLY - the STs never move.\n"
                               "Off = true ST positions (use it to check real alignment)." );

        // Fold layout is latched; only an explicit request re-solves current ST.
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

        // Numeric row: face texdefs only.
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

    // Canvas.
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

        // Re-frame selection changes; retry while the widget has no usable size
        // (TB UvViewHelper.cpp:73-83).
        const unsigned gen = Sel_Generation();
        if ( !s_haveGen || gen != s_selGen )
        {
            s_haveGen   = true;
            s_selGen    = gen;
            s_zoomValid = false;
            // User pivot placement and click-cycle state live for one 3D selection.
            s_originUser = false;
            s_cycleValid = false;
            if ( s_gesture != UVG_NONE )
                GestureCancel();
        }

        // Gather resets D to identity, so replay the cached index-parallel layout while
        // structure matches. D is display layout, not a function of mutable texdefs/ST or
        // the target subset: those must not trigger a post-edit rearrangement. Conjugation
        // guarantees displayed motion follows A. Re-fold explicitly invalidates the cache.
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

        // Frame after chain placement so the complete displayed spread is visible.
        if ( !s_zoomValid )
            FrameActive();

        // Compute this frame's pivot and box before Probe hit-testing.
        AutoPivot();
        s_box = ComputeBox();

        float cu = 0.0f, cv = 0.0f;
        PxToUv( io.MousePos, &cu, &cv );

        // Cursor-anchored wheel zoom (TB UvCameraTool::mouseScroll :80-107).
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

        // Gesture start/update/end.
        if ( s_gesture == UVG_NONE && hovered )
        {
            // Escape clears local targets when ImGui receives it; empty click remains the
            // path independent of the KIWI key funnel.
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
            // Focus loss cancels as in TB UvView.cpp:191-199; mouse release is primary
            // because the KIWI key funnel may consume Escape first.
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

        // Draw.
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

        // Recompute the box after live ST writes, then run the shared hover/press probe
        // before drawing overlays.
        s_box = ComputeBox();
        uvProbe_t pr;
        ProbeClear( &pr );
        if ( hovered && s_gesture == UVG_NONE )
            pr = Probe( cu, cv, io.KeyAlt );
        ApplyHoverCursor( pr, hovered );

        if ( AnythingSelected() )
            DrawOverlays( dl, hovered, pr );

        // Bottom-left readout.
        char line[320];
        const char *name = ( MtlDefUsable( bgMd ) )
                         ? (const char *)Materialdef_GetName( bgMd ) : nullptr;
        const int nShapes = (int)( s_faces.size() + s_patches.size() );
        char tgt[64];
        tgt[0] = '\0';
        if ( !s_targetAll && nShapes > 0 )
            _snprintf( tgt, sizeof( tgt ), "   editing %d of %d", TargetCount(), nShapes );
        tgt[sizeof( tgt ) - 1] = '\0';
        // Report display-only folding because chained positions differ from true ST.
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

// Dock window.
void KiwiUvEd_Draw()
{
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_UVEDITOR );
    if ( !open || !*open )
    {
        // No fresh gather exists after a mid-drag close, so abandon via undo rather than
        // dereferencing the previous frame's selection.
        GestureAbandon();
        s_inScope = false;
        return;                              // closed: no Begin, no End, no cost
    }

    if ( KiwiWindows_JustOpened( KIWI_WIN_UVEDITOR ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );

    const bool visible = ImGui::Begin( KiwiWindows_Title( KIWI_WIN_UVEDITOR ), open );
    if ( visible )
    {
        Gather();

        // Latch here because Cam_Draw cannot query this window's ImGui state. Include child
        // windows so the canvas and toolbar count as focused/hovered.
        s_inScope = ImGui::IsWindowFocused( ImGuiFocusedFlags_RootAndChildWindows )
                 || ImGui::IsWindowHovered( ImGuiHoveredFlags_RootAndChildWindows )
                 || s_gesture != UVG_NONE;

        if ( !AnythingSelected() )
        {
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
                "down through shapes stacked there; Shift+click adds; Ctrl+click removes; "
                "drag on "
                "empty canvas to rubber-band; click empty canvas (or Esc) for all of them "
                "again.  Right/middle drag pans, the wheel zooms, HOLD CTRL while dragging "
                "to snap "
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
        GestureAbandon();                    // collapsed/clipped away mid-drag
        s_inScope = false;
    }
    ImGui::End();
}

// Pure per-frame reads used by 3D overlay passes; see the header's scope contract.
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
        // s_brushes covers whole brushes, patches, and owners of face-only selections.
        for ( size_t i = 0; i < s_brushes.size(); ++i )
            if ( s_brushes[i].def == def )
                return true;
        return false;
    }
    return HaveFace( const_cast< brush_t * >( def ), faceIndex );
}

// Command palette.
void KiwiUvEd_RegisterCommands()
{
    // Unbound/searchable; KiwiWindows_DispatchInstant owns the toggle state.
    Radiant_RegisterCommand( "KiwiWindowUvEditor", 0, 0, KIWI_CMD_WINDOW_UVEDITOR );
}

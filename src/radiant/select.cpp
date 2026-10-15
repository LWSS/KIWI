#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\select.cpp - selection state: the pick-ray chain, the brush/face
// selection lists, selection transforms, hide/show, and the vertex/edge edit handles.
// Ported from CoD4Radiant (IW3xRadiant.i64); GtkRadiant 1.6 for naming only.

#include "stdafx.h"
#include <universal/assertive.h>    // iassert (USE_ASSERTS always on; same handler as Assert)
#include "qe3.h"
#include "mainfrm.h"   // CMainFrame / CXYWnd::m_nViewType (region-select view axis)
#include "kiwi_lightcache.h"
#include "xywnd.h"     // xywndState_t / Ed_ActiveXY (U-GLOBALS)
#include "prefs.h"     // CPrefsDlg / g_PrefsDlg (Test_Ray pick-chain m_nEntityShowState/...)
#include <gfx_d3d/r_xsurface.h>  // XModel, Editor_ExtractXModelGeo (Model_RayTest model ray-pick)
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <vector>     // Test_Ray cycle-pick (contents&0x40) brush gather (IDB CPtrArray)
#include "kiwi_fmt.h" // KIWI: KiwiFmt_Vec3Exact ("angles" that read back exactly)
#include "kiwi_angles.h" // KIWI: double "angles" composition (RotateFixedSize, DoFlip)
#include "kiwi_floatsnap.h" // KIWI: KiwiRound_Fistp
#include "kiwi_ported.h" // KIWI: shared engine/editor externs

// ─── CRT import thunk (j__free_0 = free) ─────────────────────────────────────
static inline void j__free_0( void *p ) { free( p ); }

// ─── editor helpers from q_shared.cpp ────────────────────────────────────────
extern void  I_strncpyz( char *dest, const char *src, int destsize );   // q_shared.cpp (0x4B9290)

// KIWI-UX (ROUND AZ, ITEM 3): THE sky predicate — kiwi_skybox.h D-AZ-A.  Declared
// here rather than by including the header, because this is the file's ONLY use of
// it and select.cpp includes no kiwi_* header today.
extern bool KiwiSky_IsSkyMaterial( const qtexture_s *q );   // kiwi_skybox.cpp  bool KiwiSky_IsSkyMaterial(const qtexture_s*)

// ─── forward declarations ──────────────────────────────────────────────────────
// brush.cpp
// Made non-static in brush.cpp so select.cpp can call them.
extern void Brush_Deselect_Helper( selbrush_t *b );
extern void Brush_Select_Helper( selbrush_t *b );

// entity.cpp
extern void        Entity_RebuildBounds( entity_s *e );


// patch (pmesh.cpp) + brush face helpers (brush.cpp)
extern void        Patch_Deselect();
extern void        Patch_ShiftTexture( patchMesh_t *p, float s, float t );
extern curvePatchDef_t *Patch_FlipTexture( patchMesh_t *def, int axis );    // pmesh.cpp 0x445e30
extern void        Patch_ScaleTexture( patchMesh_t *p, float s, float t );
extern void        Patch_RotateTexture( patchMesh_t *p, float deg );
extern void        Patch_Scale( patchMesh_t *p, const float *mid, const float *scale, char doRebuild );
extern void        Patch_ApplyMatrix( const orientation_t *orient, patchMesh_t *p, char snap ); // pmesh.cpp 0x441E70
extern void        Brush_FitTextureFaces( brush_t *def, float vTimes, float uTimes );
extern void        Texture_Fit( face_t *f, float vTimes, float uTimes, int selectionBounds );
extern void        Vis_Free( int count, faceVis_s *f, selbrush_t *brushPtr );   // brush.cpp 0x4702b0
extern void        Brush_SetAllFacesSampleSize( brush_t *b, int sampleSize );
extern void        Brush_SetAllFacesMaterial( brush_t *def, MaterialDef *material, char planarProject,
                                              float useStoredST, char materialOnly );
extern void        Brush_SetAllFacesTexdef( brush_t *b, const texdef_sub_t *texdef );

// brush.cpp texture helpers
extern brush_t    *Brush_SetFaceTexdef( const texdef_sub_t *texDef, face_t *f, brush_t *b );
extern void        Face_SetSampleSize( face_t *f, brush_t *b, int sampleSize );

// materialdef.cpp
extern void        TexMatToFakeTexCoords( MaterialDef *def, texdef_sub_t *texDef );
namespace LayerMat { int GetCurrentLayer( MaterialDef *def ); }

// selection bounds (defined below)
void Select_GetBounds( float *mins, float *maxs );
void Select_GetMid( float *mid );


// engine_stubs.cpp globals
extern void       *zero;                       // empty-string sentinel (engine_stubs.cpp)

// --- Test_Ray pick chain deps ---
// Brush_Ray (0x475fe0): convex-clip the instance against the ray; returns the entry
// faceVis_s* (&b->faces[entryIdx]) on a hit, NULL otherwise. *outDist = entry distance
// along dir, *outNormal = entry face normal. Defined below (uses the real instance
// faceVis array Brush_CheckBuildFaceVis builds), NOT a separate struct.
extern faceVis_s  *Brush_Ray( selbrush_t *b, const float *dir, const float *start,
                              const orientation_t *orient, float *outDist, float *outNormal );
extern char        Patch_RayTest( const float *org, const float *dir, patch_t *pm,
                             float *outDist, int *outCol, int *outRow,
                             byte *outColor, float *outPlane );        // pmesh.cpp 0x44acc0
extern char        Ray_TriangleHit( const float *vB, const float *vA, const float *dir,
                                     const float *base, const float *org,
                                     float *outT, float *outU, float *outV );   // pmesh.cpp 0x44ab10
extern int         PlaneFromPoints_Real( float *out, const float *A, const float *B,
                                         const float *C );                      // pmesh.cpp 0x4a9950
extern void        Entity_GetOrientationMatrix( entity_s *ent, float (*axis)[3] ); // entity.cpp 0x482940
extern void        VectorRotateByAxis( float *out, const float *axisMatrix, const float *dir ); // draw.cpp 0x4ba6b0
// The pick chain transforms the ray WORLD->LOCAL, the INVERSE of
// OrientationPosToWorldPos - two distinct functions.
extern void        OrientationWorldPosToLocalPos( float *out, const float *pos,
                                                  const orientation_t *orient ); // draw.cpp 0x4ba610
extern void        OrientationConcatenate( const orientation_t *orFirst,
                                           const orientation_t *orSecond,
                                           orientation_t *out );                // engine_stubs.cpp 0x4ba7d0
extern char        Filter_IsFaceMaterialHidden( Material *faceMtl );            // filters.cpp 0x46fcf0
void               Select_Deselect( int bDeselectFaces );                      // 0x48e800 (below)

// KIWI-UX: the typed selection core's legacy-change notification (kiwi_selection.cpp).
// Hooked at the funnels that bypass Brush_AddToList2 / Brush_RemoveFromList (the two
// direct list splices) and at every g_SelectedFaces size change.  O(1).
extern void        Sel_InvalidateFromLegacy();
// KIWI-UX (shakeout D): the same notification PLUS "this was a wholesale deselect",
// used only by Select_Deselect.  See the call site at the tail of that function.
extern void        Sel_NoteLegacyDeselect();
// g_PrefsDlg (prefData_t*) comes from prefs.h; Editor_ExtractXModelGeo from r_xsurface.h.

// prefab_s (qe3.h): the recursed prefab brush list passed to BrushList_Trace is the
// EMBEDDED `brushes` sentinel — the {active_brushlist(prev), active_brushlist_next(next)}
// pair is a selbrush_t list head, exactly like the &active_brushes / &selected_brushes
// sentinels.
static inline selbrush_t *Prefab_BrushListSentinel( void *prefab )
{
    return &((prefab_s *)prefab)->brushes;
}

// W_CAMERA bitmask (g_nUpdateBits flag for camera redraw)
#ifndef W_CAMERA
#define W_CAMERA 0x01
#endif

// undo_s is defined in qe3.h (included above via stdafx.h).

// patch_t (the patch instance node; .def @0, .selected @6) is the shared struct in qe3.h;
// selbrush_t.patch is a patch_t*.

// selface_t is defined in qe3.h (moved so drag.cpp can use it).

// ── helper macros ────────────────────────────────────────────────────────────

// Cast selbrush_t.faces (void*) to faceVis_s[] and index it.
#define SEL_FACES(b)      ((faceVis_s *)(b)->faces)

// String zero — engine_stubs.cpp points `zero` at an empty string (the binary's
// empty-CString sentinel).  For plain-C usage we cast to (const char*) and guard
// against null.
static inline const char *zero_str() {
    return zero ? (const char *)zero : "";
}

// ── select.cpp globals ───────────────────────────────────────────────────────
// g_SelectedFaces (CArray<selface_t,selface_t&>, 0x73C70C) — the face-selection
// storage; model in qe3.h.

// ── undo helpers ─────────────────────────────────────────────────────────────
// The "adding brushes after entity" guard tests entitylist.next, NOT the embedded
// brush head - Undo only links entities via entity_s.next.
static void Undo_TryAddBrush( brush_t *bDef )
{
    if ( g_lastundo )
    {
        if ( g_lastundo->entitylist.next != &g_lastundo->entitylist )
            Sys_Printf( "Undo_AddBrush: WARNING adding brushes after entity.\n" );
        entity_s *owner = (entity_s *)(intptr_t)bDef->owner;
        if ( *(int *)&owner->eclass->fixedsize )
            Undo_AddEntity( (entity_s *)owner );
        Undo_AddBrush( (entity_brush_s *)bDef );
    }
    else
    {
        Sys_Printf( "Undo_AddBrush: no last undo.\n" );
    }
}

// UNDO_LINK_BRUSH — record undo id in brush_t.ownerPrev and entity.epairEdits.
static void Undo_LinkBrush( brush_t *bDef )
{
    if ( g_lastundo && !g_lastundo->done )
    {
        bDef->ownerPrev = (entity_s *)(intptr_t)g_lastundo->id;
        entity_s *owner = (entity_s *)(intptr_t)bDef->owner;
        if ( *(int *)&owner->eclass->fixedsize )
            owner->epairEdits = g_lastundo->id;
    }
}

// ── mode reset helper ─────────────────────────────────────────────────────────
// CMainFrame::UpdatePatchToolbarButtons (0x42aa70) - the binary refreshed the patch-edit
// toolbar check-states (0x8072 patch-bend, 0x8068 redisperse, 0x8174 lock, 0x8173 unlock,
// 0x8175 cycle-edge) and the three Curve-menu check marks (0x8174/0x8173/0x8175).
// The symbol stays because ~20 core call sites across brush/drag/pmesh/select/xywnd/mainfrm
// reach it; the body is empty, since the ImGui shell renders the patch-mode toggles
// straight from bLockPatchVerts / d_select_mode.
void CMainFrame_UpdatePatchToolbarButtons()
{
}

static void ResetSelectMode()
{
    select_t prev = g_qeglobals.d_select_mode;
    g_qeglobals.d_select_mode = sel_brush;
    if ( prev == sel_cycle_edge_direction_quad )
        CMainFrame_UpdatePatchToolbarButtons();
}

// ── Select_Brush_2 — links brush into list head ───────────────────────────────
// 0x476630
static void Select_Brush_2( selbrush_t *list, selbrush_t *b )
{
    if ( b->next || b->prev )
        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
    if ( list == &selected_brushes )
    {
        Brush_AddToList2( b );
    }
    else
    {
        b->next = list->next;
        list->next->prev = b;
        list->next = b;
        b->prev = list;
    }
}


// ═════════════════════════════════════════════════════════════════════════════
//  THE Test_Ray PICK CHAIN (brush + patch + prefab + model ray-pick):
//    Brush_Ray   (0x475fe0)  convex-brush plane clip -> entry faceVis_s*
//    Model_RayTest  (0x48ce60)  XModel per-triangle ray-pick
//    Brush_TraceTest  (0x48d240)  per-brush inner trace: brush -> patch -> prefab -> model
//    BrushList_Trace  (0x48d460)  brush-list walker (keeps nearest, dedup by ~0.5 dist)
//    Test_Ray    (0x48d7c0)  top entry (incl. the contents&0x40 cycle-pick branch)
//  edTrace_t (qe3.h) is the trace record: hit{brush, face, index}, xx2, xx3, xx4 (an
//  orientation copy), dist, selected, normal.  The faceVis identity t->hit.face ==
//  &brush->faces[idx] holds because Brush_Ray returns into the real instance faceVis array
//  (built by Brush_CheckBuildFaceVis).
//  BrushList_Trace's camera/XY frustum cull is not ported - it is a pure optimization (a
//  brush is processed unless culled by BOTH views), so never culling is safe.
// ═════════════════════════════════════════════════════════════════════════════

// Forward declarations — the chain is mutually recursive (Brush_TraceTest → BrushList_Trace →
// Brush_TraceTest for prefab recursion; Test_Ray → BrushList_Trace; BrushList_Trace → Model_RayTest).
static char Model_RayTest( float *outDist, float *outNormal, const float *org,
                           const float *dir, selbrush_t *brush );
static char Brush_TraceTest( const float *start, const float *dir, int contents,
                             selbrush_t *inst, const orientation_t *orient, edTrace_t *trace );
static void BrushList_Trace( const float *start, const float *dir, int contents,
                             selbrush_t *list, const orientation_t *orient, edTrace_t *traces,
                             int maxTraces );

// ── Brush_Ray (0x475fe0) — ray-vs-convex-brush, returns the entry faceVis_s* ──────
//  Builds the instance faceVis (Brush_CheckBuildFaceVis), then clips the ray segment
//  [start, start+262144*dir] against the b->faceCount planes (b->def->faces[i].plane).
//  On entry returns &b->faces[entryFaceIdx] and *outDist = the entry distance along dir;
//  on miss returns NULL with *outDist = 0.  The patch/model/prefab fallback (no instance
//  faces or no entry face) returns &b->faces[0] (non-NULL, dist 0) with normal (0,0,1) - a
//  pretend-hit so the caller's finer test (Patch_RayTest / prefab recursion / model triangle
//  pick) decides; plain brushes miss.
faceVis_s *Brush_Ray( selbrush_t *b, const float *dir, const float *start,
                      const orientation_t *orient, float *outDist, float *outNormal )
{
    // far endpoint = start + 262144*dir
    float endX = dir[0] * 262144.0f + start[0];
    float endY = dir[1] * 262144.0f + start[1];
    float endZ = dir[2] * 262144.0f + start[2];

    Brush_CheckBuildFaceVis( b, (const float *)orient );

    double entryDist = 0.0;
    faceVis_s *hitFace = NULL;
    int    i         = 0;         // loop counter against b->faceCount

    if ( b->faceCount )
    {
        // near point (p1) clipped forward, far point (p2) clipped back, as planes pass.
        float p1x = start[0], p1y = start[1], p1z = start[2];
        float p2x = endX,     p2y = endY,     p2z = endZ;
        for ( ;; )
        {
            const plane_t &pl = b->def->faces[i].plane;
            float d1 = pl.normal[1]*p1y + pl.normal[0]*p1x + pl.normal[2]*p1z - pl.dist;
            float d2 = pl.normal[1]*p2y + p2x*pl.normal[0] + pl.normal[2]*p2z - pl.dist;
            if ( d1 >= 0.0f && d2 >= 0.0f )                     // both in front → outside (convex)
            {
                *outDist = 0.0f;
                return nullptr;
            }
            if ( d1 <= 0.0f && d2 <= 0.0f )                     // both behind → plane doesn't clip
            {
                // entryDist unchanged
            }
            else
            {
                float frac = d1 / ( d1 - d2 );
                if ( d1 <= 0.0f )                               // exit: clip the far endpoint
                {
                    p2x = ( p2x - p1x ) * frac + p1x;
                    p2y = ( p2y - p1y ) * frac + p1y;
                    p2z = frac * ( p2z - p1z ) + p1z;
                    // entryDist unchanged
                }
                else                                            // entry: record face/normal, clip near
                {
                    if ( outNormal )
                    {
                        outNormal[0] = pl.normal[0];
                        outNormal[1] = pl.normal[1];
                        outNormal[2] = pl.normal[2];
                    }
                    hitFace = &b->faces[i];
                    p1x = p1x + ( p2x - p1x ) * frac;
                    p1y = p1y + ( p2y - p1y ) * frac;
                    p1z = frac * ( p2z - p1z ) + p1z;
                }
            }
            ++i;
            if ( (unsigned)i >= (unsigned)b->faceCount )
                break;
        }
        entryDist = 0.0;   // always 0 in this path
        if ( hitFace )     // an entry face was recorded
        {
            // (entry point - start) dotted with dir
            *outDist = dir[1] * ( p1y - start[1] )
                     + dir[0] * ( p1x - start[0] )
                     + dir[2] * ( p1z - start[2] );
            return hitFace;
        }
        // hitFace == 0: fall through to the no-entry-face fallback.
    }

    // no instance faces, or no entry face recorded.
    *outDist = (float)entryDist;
    // PATCH instances - like prefabs / shown models below - get the pretend-hit so the
    // caller's finer test (Patch_RayTest) decides.
    if ( !b->patch )
    {
        entity_s *owner = b->owner;
        // owner==NULL only via Ed_BrushFloorRay's transient headless instance (the
        // binary always passes real owned instances) — treat as a plain-brush miss.
        if ( !owner
          || ( !owner->prefab
            && ( !owner->modelInst || ( g_PrefsDlg->m_nEntityShowState & 0x10000 ) == 0 ) ) )  // ENTITY_SKINNED
            return nullptr;
    }
    if ( outNormal )
    {
        outNormal[0] = (float)entryDist;
        outNormal[1] = (float)entryDist;
        outNormal[2] = 1.0f;
    }
    return b->faces;                 // == &b->faces[0]
}

// Ed_BrushFloorRay - headless distance wrapper over Brush_Ray for the
// Cam_ChangeFloor / z.cpp floor-march callers (which hold only a def).
bool Ed_BrushFloorRay( brush_t *def, const float *start, const float *dir, float *outDist )
{
    // Transient instance header so Brush_Ray (instance-based) can clip the def faces.
    // owner/patch stay NULL so the no-entry fallback returns NULL, not a bogus hit.
    selbrush_t tmp;
    memset( &tmp, 0, sizeof( tmp ) );
    tmp.def       = def;
    tmp.version   = (short)( def->version ^ 1 );   // force a faceVis (re)build
    float normal[3];
    faceVis_s *f = Brush_Ray( &tmp, dir, start, (const orientation_t *)world_orient_matrix,
                              outDist, normal );
    // Brush_CheckBuildFaceVis may have built real faceVis (incl. surf-cache VB handles), so tear it
    // down with Vis_Free rather than free() or the floor-march leaks a VB per face.
    if ( tmp.faces )
        Vis_Free( tmp.faceCount, tmp.faces, &tmp );
    return f != nullptr;
}

// ── Model_RayTest (0x48ce60) — XModel PMESH per-triangle ray-pick ────────────────────
//  Extract the brush's model geometry (Editor_ExtractXModelGeo), transform the ray into
//  the model's local frame (inverse rotation by "angles", scaled by 1/modelscale), then
//  ray-test every extracted triangle (Ray_TriangleHit).  Keeps the nearest hit; returns
//  1 on a hit, 0 on miss (and 1 when the model has no geometry).
//  Args: outDist, outNormal, org(start), dir, brush.
static char Model_RayTest( float *outDist, float *outNormal, const float *org,
                        const float *dir, selbrush_t *brush )
{
    // KIWI: resident geometry + model-local BVH avoid extraction and a full
    // triangle scan for each cursor ray, including recursive prefab children.
    extern int KiwiDrop_TraceModelCached( selbrush_t *, const float *, const float *, float *, float * );
    const int cached = KiwiDrop_TraceModelCached( brush, org, dir, outDist, outNormal );
    if ( cached >= 0 ) return (char)cached;
    static byte vbuf[196612];   // extracted vertex bytes (0x4000 verts * 12)
    static byte ibuf[131072];   // extracted tri indices (0x10000)

    entity_s *owner = brush->owner;
    entity_s *ownerDef = (entity_s *)owner->def;
    iassert( ownerDef->modelClass );                    // select.cpp:64 brush->owner->def->modelClass
    entitymodel_t *mc = (entitymodel_t *)ownerDef->modelClass;
    iassert( mc->model );                               // select.cpp:65 ->model
    iassert( mc->model->handle );                       // select.cpp:66 ->model->handle

    int triCount = Editor_ExtractXModelGeo( (XModel *)(intptr_t)mc->model->handle,
                                            (float *)vbuf, 0x4000,
                                            (uint16_t *)ibuf, 0x10000 );
    if ( !triCount )
        return 1;

    float angles[3];
    if ( !Entity_GetVec3ForKey( (entity_s_def *)ownerDef, angles, "angles" ) )
    {
        angles[0] = 0.0f; angles[1] = 0.0f; angles[2] = 0.0f;
    }
    float axis[3][3];
    AnglesToAxis( angles, axis );

    // localOrg = axis · (org - entityOrigin) — WORLD→LOCAL, the INVERSE of
    // OrientationPosToWorldPos.
    float localOrg[3];
    orientation_t modelOr;                              // {origin, axis}
    modelOr.origin[0] = ownerDef->origin[0];
    modelOr.origin[1] = ownerDef->origin[1];
    modelOr.origin[2] = ownerDef->origin[2];
    modelOr.axis[0][0] = axis[0][0]; modelOr.axis[0][1] = axis[0][1]; modelOr.axis[0][2] = axis[0][2];
    modelOr.axis[1][0] = axis[1][0]; modelOr.axis[1][1] = axis[1][1]; modelOr.axis[1][2] = axis[1][2];
    modelOr.axis[2][0] = axis[2][0]; modelOr.axis[2][1] = axis[2][1]; modelOr.axis[2][2] = axis[2][2];
    OrientationWorldPosToLocalPos( localOrg, org, &modelOr );

    // modelscale epair (default 1.0); when > 0, divide the local origin by it.
    float scale = 1.0f;
    // zero_str(), NOT (const char*)zero: it guards a NULL `zero`, and atof(NULL) crashes.
    const char *mscale = zero_str();
    for ( epair_t *ep = (epair_t *)ownerDef->epairs; ep; ep = (epair_t *)ep->next )
    {
        if ( !_stricmp( ep->key, "modelscale" ) )
        {
            mscale = ep->value ? ep->value : zero_str();
            break;
        }
    }
    float invScale = 1.0f;                              // divides the local origin
    scale = (float)atof( mscale );
    if ( scale <= 0.0f )
    {
        scale = 1.0f;
    }
    else
    {
        invScale = 1.0f / scale;
        localOrg[0] *= invScale;
        localOrg[1] *= invScale;
        localOrg[2] *= invScale;
    }

    // localDir = dir rotated into model space.  VectorRotateByAxis reads the rotation from
    // axisMatrix[3..11], i.e. it expects the ORIENTATION block (origin[0..2] THEN
    // axis[3..11]) - pass &modelOr, NOT the bare `axis` array (offset by 3 floats).
    float localDir[3];
    VectorRotateByAxis( localDir, (const float *)&modelOr, dir );

    float best = 3.4028235e38f;
    float planeV[4];
    int   idx = 0;
    if ( triCount > 0 )
    {
        const uint16_t *i16 = (const uint16_t *)ibuf;
        do
        {
            int a = i16[idx];
            int c = i16[idx + 1];
            int bIdx = i16[idx + 2];                    // (interleaved triplets)
            float t;                                    // outU/outV passed NULL
            const float *vA = (const float *)&vbuf[12 * a];
            const float *vB = (const float *)&vbuf[12 * c];
            const float *vC = (const float *)&vbuf[12 * bIdx];
            if ( Ray_TriangleHit( vA, vB, localDir, vC, localOrg, &t, nullptr, nullptr ) )
            {
                t *= scale;
                if ( t < best )
                {
                    if ( outNormal )
                    {
                        // (out, vC, vB, vA) in the index order above.
                        PlaneFromPoints_Real( planeV, vC, vB, vA );
                        outNormal[0] = planeV[0];
                        outNormal[1] = planeV[1];
                        outNormal[2] = planeV[2];
                    }
                    best = t;
                }
            }
            idx += 3;
        } while ( idx < triCount );
    }

    if ( best == 3.4028235e38f )                        // no hit
        return 0;
    *outDist = best;
    return 1;
}

// ── Brush_TraceTest (0x48d240) — per-brush inner trace ─────────────────────────────────
//  Ray-test ONE brush instance: convex brush (Brush_Ray) -> patch (Patch_RayTest) -> prefab
//  (recurse via BrushList_Trace through the prefab's local orientation) -> model (Model_RayTest).
//  Fills `trace` with the hit; clears trace->hit.brush/face on miss.
static char Brush_TraceTest( const float *start, const float *dir, int contents,
                             selbrush_t *inst, const orientation_t *orient, edTrace_t *trace )
{
    float *normal = trace->normal;
    float *p_dist = &trace->dist;

    faceVis_s *face = Brush_Ray( inst, dir, start, orient, &trace->dist, trace->normal );
    trace->hit.face = face;
    if ( !face )
    {
        trace->hit.brush = nullptr;
        return 0;
    }
    trace->hit.brush = inst;
    trace->hit.leafBrush = inst;
    trace->leafFace = face;
    trace->orientation = *orient;
    trace->prefabOwner = NULL;
    trace->_pad[0] = 0;
    trace->selected = 0;

    patch_t *patch = inst->patch;
    if ( patch )
    {
        if ( ( contents & 0x100 ) != 0 )               // patch-pick disabled this pass
            return 1;
        char hit = Patch_RayTest( start, dir, patch, p_dist, nullptr, nullptr, nullptr, normal );
        if ( !hit )
        {
            trace->hit.brush = nullptr;
            trace->hit.face  = nullptr;
        }
        return hit;
    }

    entity_s *owner = inst->owner;
    if ( owner->prefab )
    {
        orientation_t orFirst;
        orientation_t out;
        float localDir[3];
        float localStart[3];
        Entity_GetOrientationMatrix( (entity_s *)owner->def, &orFirst.origin );   // fills {origin, axis}
        VectorRotateByAxis( localDir, (const float *)&orFirst, dir );
        OrientationWorldPosToLocalPos( localStart, start, &orFirst );
        OrientationConcatenate( &orFirst, orient, &out );

        edTrace_t inner;
        inner.dist = 262144.0f;
        BrushList_Trace( localStart, localDir, contents,
                         Prefab_BrushListSentinel( owner->prefab ),
                         &out, &inner, 1 );
        char result;
        if ( inner.dist == 262144.0f )                  // nothing hit in the prefab
        {
            trace->hit.face  = nullptr;
            trace->hit.brush = nullptr;
            result = 0;
        }
        else
        {
            trace->hit.leafBrush = inner.hit.leafBrush;
            trace->leafFace = inner.leafFace;
            trace->selected = inner.selected;
            trace->orientation = inner.orientation;
            *p_dist = inner.dist;
            trace->_pad[0] = inner._pad[0];
            OrientationDirToWorldDir( normal, &out, inner.normal );   // map the prefab normal back out
            trace->prefabOwner = inner.prefabOwner;
            result = 1;
            if ( !inner.prefabOwner )
            {
                trace->prefabOwner = inst->owner;
            }
        }
        return result;
    }
    else if ( owner->modelInst )
    {
        // model pick: only when "show models" (0x10000) is on and (contents&4 unset OR
        // the eclass is not an actor — actors are excluded from the &4 world pass).
        if ( ( g_PrefsDlg->m_nEntityShowState & 0x10000 ) != 0
          && ( ( contents & 4 ) == 0
            || _strnicmp( ((entity_s *)owner->def)->eclass->name, "actor", 5 ) != 0 ) )
        {
            // KIWI-UX divergence from 0x48d43e: the binary treats Model_RayTest's
            // per-triangle test as authoritative — a miss CLEARS the hit, making
            // skinny models (grass, poles, fences) nearly unclickable.  Here the
            // triangle pass only REFINES dist/normal on a mesh hit (it writes
            // nothing on a miss), and the Brush_Ray bounds hit above stands
            // regardless, so the whole model bbox is the selection zone.  The
            // drop trace is unaffected: it passes contents|0x200, which excludes
            // fixedsize entities in the walker before this branch.
            Model_RayTest( p_dist, normal, start, dir, inst );
            return 1;
        }
    }
    return (char)(intptr_t)face;
}

// KIWI: expose native recursive prefab tracing to the model-mode picker.
bool KiwiSelect_TracePrefab( selbrush_t *node, const float *start,
                             const float *dir, edTrace_t *trace )
{
    if ( !node || !node->owner || !node->owner->prefab || !trace )
        return false;
    *trace = edTrace_t{};
    trace->dist = 262144.0f;
    return Brush_TraceTest( start, dir, 0, node,
        (const orientation_t *)world_orient_matrix, trace ) != 0
        && trace->hit.brush != nullptr;
}

// ── BrushList_Trace (0x48d460) — brush-list walker ─────────────────────────────────────
//  Iterate a brush list (`list` is the sentinel head), ray-test each pickable brush via
//  Brush_TraceTest, and keep the nearest hit into the `traces` array (maxTraces long).
//  The camera/XY frustum cull is skipped headless (see the chain header).
static void BrushList_Trace( const float *start, const float *dir, int contents,
                             selbrush_t *list, const orientation_t *orient, edTrace_t *traces,
                             int maxTraces )
{
    for ( selbrush_t *sbn = list->next; sbn != list; sbn = sbn->next )
    {
        // contents&4 (world pass): skip prefab/model entity brushes (owner != world &
        // owner is a prefab class) — those are picked via the prefab/model recursion.
        if ( ( contents & 4 ) != 0 )
        {
            entity_s *owner = sbn->owner;
            if ( owner != world_entity
              && ( ((entity_s *)owner->def)->eclass->classtype & 0x10 ) != 0 )   // CLASS_PREFAB
            {
                continue;
            }
        }
        if ( FilterBrush( sbn, 0 ) )
        {
            continue;
        }
        int brushflags = sbn->brushFlags;
        if ( ( brushflags & BRUSHFLAG_SELECTED ) != 0 )
        {
            continue;
        }
        if ( ( brushflags & 0x20 ) != 0 && ( contents & 0x1000 ) == 0 )
        {
            continue;
        }

        iassert( sbn->def->owner == (entity_s *)sbn->owner->def );   // select.cpp:233 brush->def->owner==brush->owner->def

        eclass_t *eclass = ((entity_s *)sbn->owner->def)->eclass;
        // hidden/fixedsize/model gate: pickable unless a point/model entity the
        // current pass excludes.
        bool ok = ( eclass->classtype & 0x10 ) != 0
               || ( ( ( contents & 0x200 ) == 0 || !*(int *)&eclass->fixedsize )
                 && ( ( contents & 0x400 ) == 0 || !Model_SetModel( sbn, (const float *)orient ) ) );
        if ( !ok )
        {
            continue;
        }
        if ( sbn->patch && !( *(int *)&g_PrefsDlg->m_bSelectCurves && ( contents & 0x10 ) == 0 ) )
        {
            continue;
        }

        // sky-brush disable: when SkyBrushOff is set, skip faces whose base
        // material name contains "sky_".
        if ( g_PrefsDlg->sky_brush_off )
        {
            MaterialDef *skyMd = &sbn->def->faces->mtldef[g_qeglobals.current_edit_layer];
            LayerMaterialDef *nm = Materialdef_GetName( skyMd );
            // ── KIWI-UX (ROUND AZ, ITEM 3): WIDENED, NOT REPLACED ────────────────
            // The `"sky_"` substring is the binary's own test and stays exactly as it
            // was.  What is OR'd in is the SURF_SKY predicate (kiwi_skybox.h D-AZ-A),
            // so this preference finally means what its menu item says — "disable
            // selection of sky" — for a sky material that simply is not named `sky_*`.
            //
            // THIS IS THE ONLY PLACE THE EXCLUSION CAN LIVE, and that is worth stating
            // because the obvious alternative is wrong: `Pick`'s area arm keeps ONE
            // nearest hit (kiwi_pick.cpp:606-607), so rejecting a sky hit after
            // `Test_Ray` returns would mean "nothing is under the cursor", not "select
            // what is behind it".  Only this walker's `continue` moves on to the next
            // brush, which is what click-THROUGH means.
            if ( strstr( (const char *)nm, "sky_" ) || KiwiSky_IsSkyMaterial( skyMd->radMtl ) )
                continue;
        }

        edTrace_t hit;
        Brush_TraceTest( start, dir, contents, sbn, orient, &hit );
        if ( !hit.hit.face )
            continue;
        // face-texture-filter test: skip if the hit face's material is filtered out.
        // (CPU mode: visArray is NULL — derive the Material* from the def face's layer.)
        Material *faceMtl = nullptr;
        if ( hit.hit.face->visArray )
            faceMtl = hit.hit.face->visArray->mtlHandle;
        if ( Filter_IsFaceMaterialHidden( faceMtl ) )
            continue;

        float dist = hit.dist;
        // de-dup: walk forward in the output trace array, stopping at the first slot
        // whose dist differs from this hit by > 0.5 (so a coincident multi-list hit
        // isn't recorded twice). Then keep it if it's strictly nearer than that slot.
        edTrace_t *slot = traces;
        int k = 0;
        while ( k < maxTraces - 1 )
        {
            if ( fabs( dist - slot->dist ) > 0.5f )
                break;
            ++k;
            ++slot;
        }
        if ( dist > 0.0f && slot->dist > dist )
        {
            memcpy( slot, &hit, sizeof( edTrace_t ) );
            slot->selected = ( list == &selected_brushes );
            if ( ( contents & 0x1000 ) != 0 && ( sbn->brushFlags & 0x20 ) != 0 )
                slot->_pad[0] = 1;
        }
    }
}

// ── Test_Ray (0x48d7c0) — the top pick-ray entry ──────────────────────────────────
//  Trace `dir` from `start` through up to num_traces nearest brushes (the array t).
//  contents bit 0x40 = cycle pick: deselect, gather every hit brush under the cursor and
//  pick the one BEFORE the current selection.  Otherwise walk the active + selected lists
//  (a set bit SUPPRESSES that list) and then the &4 world-priority retry.
void Test_Ray( float *start, float *dir, int contents, edTrace_t *t, int num_traces )
{
    memset( t, 0, sizeof( edTrace_t ) * num_traces );
    for ( int k = 0; k < num_traces; ++k )
        t[k].dist = 262144.0f;

    int x_contents = contents;                               // the &1/&2/&4 mask below
    if ( ( contents & 0x40 ) != 0 )
    {
        // ── cycle-pick branch (contents & 0x40) ──
        std::vector<selbrush_t *> hits;
        // pToSelect = the currently-selected brush (or NULL): used to pick the one before it.
        selbrush_t *pToSelect = ( &selected_brushes != selected_brushes.next )
                              ? selected_brushes.next : nullptr;
        Select_Deselect( 1 );

        int passFlags = contents & 0x1000;
        for ( selbrush_t *brush = active_brushes.next;
              brush != &active_brushes;
              brush = brush->next )
        {
            if ( FilterBrush( brush, 0 ) )
                continue;
            int bf = brush->brushFlags;
            if ( ( bf & BRUSHFLAG_SELECTED ) != 0 )
                continue;
            if ( ( bf & 0x20 ) != 0 )
                continue;
            if ( brush->patch && !*(int *)&g_PrefsDlg->m_bSelectCurves )
                continue;

            edTrace_t hit;
            Brush_TraceTest( start, dir, passFlags, brush,
                        (const orientation_t *)world_orient_matrix, &hit );
            if ( !hit.hit.face )
                continue;
            // In CPU mode (visArray NULL) this filter is inert; gather all.
            if ( Filter_IsFaceMaterialHidden( hit.hit.face->visArray ? hit.hit.face->visArray->mtlHandle : nullptr ) )
                continue;
            hits.push_back( brush );
        }

        selbrush_t *winner = pToSelect;
        if ( !hits.empty() )
        {
            int x = 0;
            int nSize = (int)hits.size();
            for ( ; x < nSize; ++x )
                if ( hits[x] == pToSelect )
                    break;
            if ( x >= nSize )                                // current selection not among hits → first
            {
                winner = hits[0];
            }
            else
            {
                int prevIdx = ( x <= 0 ) ? ( nSize - 1 ) : ( x - 1 );   // the brush BEFORE the selected one
                winner = hits[prevIdx];
            }
        }

        if ( winner )
        {
            iassert( num_traces == 1 );                      // select.cpp:352 "maxTraces == 1"
            t->normal[0] = 0.0f;
            t->normal[1] = 0.0f;
            t->normal[2] = 1.0f;
            float dist;
            faceVis_s *face = Brush_Ray( winner, dir, start,
                                         (const orientation_t *)world_orient_matrix,
                                         &dist, t->normal );
            t->dist     = dist;
            t->hit.brush    = winner;
            t->hit.face     = face;
            t->selected = 0;
            return;
        }
        // cycle-pick found nothing -> FALL THROUGH to the ordinary two-list walk,
        // and with the cycle-pass mask (contents & 0x1000), not the original contents.  That
        // retry is the only path that can still reach a flag-0x20 brush (BrushList_Trace admits
        // one when contents&0x1000; the gather loop above rejects it unconditionally).
        x_contents = passFlags;
    }

    // ── normal walk: active list, selected list, then the &4 world-priority retry ──
    if ( ( x_contents & 1 ) == 0 )
        BrushList_Trace( start, dir, x_contents, &active_brushes,
                    (const orientation_t *)world_orient_matrix, t, num_traces );
    if ( ( x_contents & 2 ) == 0 )
        BrushList_Trace( start, dir, x_contents, &selected_brushes,
                    (const orientation_t *)world_orient_matrix, t, num_traces );
    if ( ( x_contents & 4 ) != 0 && !t->hit.brush )
    {
        edTrace_t retry;
        Test_Ray( start, dir, x_contents - 4, &retry, 1 );
        memcpy( t, &retry, sizeof( edTrace_t ) );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48DAA0  Trace_AllDirectionsIfFailed - drop-to-floor ray with a jitter fallback.
//  Trace `dir` from `cam_origin`; if nothing was hit, retry from four origins jittered
//  +/-1 on X and Y (so a ray that slips between two world brushes still finds the floor).
// ═════════════════════════════════════════════════════════════════════════════
edTrace_t *Trace_AllDirectionsIfFailed( float *cam_origin, edTrace_t *trace_result,
                                        float *dir, int contents )
{
    Test_Ray( cam_origin, dir, contents, trace_result, 1 );
    if ( !trace_result->hit.brush )
    {
        static const float jit[4][2] = { { 1.0f, 1.0f }, { 1.0f, -1.0f },
                                         { -1.0f, 1.0f }, { -1.0f, -1.0f } };
        for ( int k = 0; k < 4 && !trace_result->hit.brush; ++k )
        {
            float o[3] = { cam_origin[0] + jit[k][0],
                           cam_origin[1] + jit[k][1],
                           cam_origin[2] };
            Test_Ray( o, dir, contents, trace_result, 1 );
        }
    }
    return trace_result;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48DCC0  Select_Brush - move brushes between the active and selected lists.
//  The MFC bStatus status-text and the center_grid PositionView are dropped.
// ═════════════════════════════════════════════════════════════════════════════
void Select_Brush( selbrush_t *brush, char some_overwrite, char bStatus, char center_grid_on_selection )
{
    iassert( (g_SelectedFaces.GetSize() == 0) || brush->patch );   // select.cpp:426
    entity_s *e = brush->owner;
    iassert( e );

    if ( e == world_entity || some_overwrite != 1 )
    {
        Brush_RemoveFromList( brush );
        Brush_AddToList2( brush );
    }
    else
    {
        selbrush_t *ab = active_brushes.next;
        if ( ab != &active_brushes )
        {
            do
            {
                selbrush_t *abNext = ab->next;
                if ( ab->owner == e )
                {
                    Brush_RemoveFromList( ab );
                    Brush_AddToList2( ab );
                }
                ab = abNext;
            }
            while ( ab != &active_brushes );
        }
    }

    // MFC status bar and grid centering: skipped.
    (void)bStatus;
    (void)center_grid_on_selection;
}

// ═════════════════════════════════════════════════════════════════════════════
//  g_SelectedFaces CArray backbone (0x73C70C).
//  m_pData = the selface_t[] data; m_nSize = the live count; m_nMaxSize = the
//  allocated capacity.  SetSize models MFC CArray::SetSize (grow-by =
//  clamp(count/8, 4, 1024)).
// ═════════════════════════════════════════════════════════════════════════════
SelectedFaceArray g_SelectedFaces;

void SelectedFaceArray::SetSize( int n )        // 0x494610
{
    Sel_InvalidateFromLegacy();   // KIWI-UX (every face-selection size change, incl. ::Add)
    if ( n < 0 ) { SelectedFaces_BoundsAbort(); return; }
    if ( n == 0 )
    {
        if ( m_pData ) { j__free_0( m_pData ); m_pData = nullptr; }
        m_nMaxSize = 0;
        m_nSize    = 0;
        return;
    }
    if ( !m_pData )                              // first allocation
    {
        int cap = ( n <= m_nMaxSize ) ? m_nMaxSize : n;
        m_pData = (selface_t *)malloc( sizeof( selface_t ) * cap );
        memset( m_pData, 0, sizeof( selface_t ) * cap );
        m_nMaxSize = cap;
        m_nSize    = n;
        return;
    }
    if ( n > m_nMaxSize )                         // grow capacity
    {
        // grow-by = m_nSize/8 (MFC CArray), not capacity/8.
        int growby = m_nSize / 8;
        if ( growby < 4 )       growby = 4;
        else if ( growby > 1024 ) growby = 1024;
        int newcap = m_nMaxSize + growby;
        if ( n >= newcap ) newcap = n;
        selface_t *nf = (selface_t *)malloc( sizeof( selface_t ) * newcap );
        memcpy( nf, m_pData, sizeof( selface_t ) * m_nSize );
        memset( nf + m_nSize, 0, sizeof( selface_t ) * ( newcap - m_nSize ) );
        j__free_0( m_pData );
        m_pData    = nf;
        m_nMaxSize = newcap;
    }
    else if ( n > m_nSize )                       // grow size within capacity
    {
        memset( m_pData + m_nSize, 0, sizeof( selface_t ) * ( n - m_nSize ) );
    }
    m_nSize = n;
}

// CArray::Add (0x4947A0): SetSize(GetSize()+1) + element write.
int SelectedFaceArray::Add( const selface_t &f )
{
    int idx = m_nSize;
    if ( idx < 0 ) SelectedFaces_BoundsAbort();
    SetSize( idx + 1 );
    m_pData[idx] = f;
    return idx;
}

// CArray::RemoveAt (0x480670): drop `count` entries starting at index i.
void SelectedFaceArray::RemoveAt( int i, int count )
{
    Sel_InvalidateFromLegacy();   // KIWI-UX (the one face removal that does not call SetSize)
    int end = i + count;
    if ( i < 0 || count < 0 || end > m_nSize || end < i || end < count )
        SelectedFaces_BoundsAbort();
    if ( m_nSize != end )
        memmove( &m_pData[i], &m_pData[end], sizeof( selface_t ) * ( m_nSize - end ) );
    m_nSize -= count;
}

// Insert a non-patch brush at the head of active_brushes (the cluster's
// "demote a brush back to the active list" idiom; &active==&selected never holds).
static void Brush_ToActiveHead( selbrush_t *b )
{
    Brush_RemoveFromList( b );
    if ( b->next || b->prev )
        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
    b->next             = active_brushes.next;
    active_brushes.next->prev = b;
    active_brushes.next = b;
    b->prev             = &active_brushes;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48E050  Brush_DeselectFaces - remove ALL of a brush's faces from g_SelectedFaces.
//  (Patch brushes: move the brush back to the active list instead.)
// ═════════════════════════════════════════════════════════════════════════════
void Brush_DeselectFaces( selbrush_t *b )
{
    if ( b->patch )
    {
        Brush_ToActiveHead( b );
        return;
    }
    for ( unsigned fi = 0; fi < (unsigned)b->faceCount; ++fi )
    {
        faceVis_s *target = &SEL_FACES( b )[fi];
        int sz = g_SelectedFaces.GetSize();
        int k = 0;
        while ( k < sz && g_SelectedFaces.GetAt( k ).face != target )
            ++k;
        if ( k < sz )
            g_SelectedFaces.RemoveAt( k, 1 );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48DEC0  Brush_SelectFaces - ADD all of a brush's (winding-bearing) faces to
//  g_SelectedFaces, skipping any already present.  (Patch brushes: select-as-brush.)
//  Brush_CheckBuildFaceVis runs once at the top; the version compare is 16-bit.
//  The binary's tautological assert (select.cpp:505) is omitted.
// ═════════════════════════════════════════════════════════════════════════════
void Brush_SelectFaces( selbrush_t *brush )
{
    Brush_CheckBuildFaceVis( brush, (const float *)world_orient_matrix );
    if ( brush->patch )
    {
        Brush_RemoveFromList( brush );
        if ( brush->next || brush->prev )
            Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
        Brush_AddToList2( brush );
        return;
    }
    if ( !brush->faceCount )
        return;
    for ( unsigned fi = 0; fi < (unsigned)brush->faceCount; ++fi )
    {
        faceVis_s *target = &SEL_FACES( brush )[fi];
        int sz = g_SelectedFaces.GetSize();
        int i = 0;
        while ( i < sz && g_SelectedFaces.GetAt( i ).face != target )
            ++i;
        if ( i < sz )                         // already selected
        {
            iassert( brush == g_SelectedFaces.GetAt( i ).brush );   // select.cpp:494
        }
        else                                  // add it
        {
            selface_t selFace;
            selFace.brush = brush;
            selFace.face  = target;
            selFace.index = (int)fi;
            iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:506
            g_SelectedFaces.Add( selFace );
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48E170  Select_BrushesToFaces - convert the current whole-brush selection into a face
//  selection: for every selected (non-patch) brush, clear any of its faces already
//  recorded, add all winding-bearing faces, then demote the brush to the active list.
// ═════════════════════════════════════════════════════════════════════════════
void Select_BrushesToFaces()
{
    extern bool  PatchDialog_IsOpen();           // patchdialog.cpp
    extern void  g_PatchDialog_GetPatchInfo();
    if ( selected_brushes.next == &selected_brushes )
        return;

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( b->patch )
            continue;
        Brush_DeselectFaces( b );
        for ( unsigned i = 0; i < (unsigned)b->faceCount; ++i )
        {
            // capture the record, rebuild faceVis PER-FACE (idempotent, version-gated),
            // then assert it didn't move (570) and the instance/def versions are synced
            // (571).
            selface_t selFace;
            selFace.brush = b;
            selFace.face  = &SEL_FACES( b )[i];
            selFace.index = (int)i;
            Brush_CheckBuildFaceVis( b, (const float *)world_orient_matrix );
            iassert( selFace.face == &selFace.brush->faces[selFace.index] );        // select.cpp:570
            iassert( selFace.brush->version == selFace.brush->def->version );       // select.cpp:571
            if ( !b->def->faces[i].w )                   // skip faces with no winding
                continue;
            g_SelectedFaces.Add( selFace );
        }
    }
    // Demote every non-patch selected brush back to the active list.
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; )
    {
        selbrush_t *next = b->next;
        if ( !b->patch )
            Brush_ToActiveHead( b );
        b = next;
    }
    // re-bind the Patch Inspector to the new selection.
    if ( PatchDialog_IsOpen() )
        g_PatchDialog_GetPatchInfo();
}

// Select_SetTexture (0x456D70) - the default texture-repeat scale (x,y) for
// Patch->Naturalize.  The binary's inspector-OPEN branch also multiplied the default by
// the surface inspector's IDC_SURFACE_INSP_TEX_REP_X/Y edits, which this inspector does not
// have; only the default-scale slot (random_texture_stuff[layer].sampleSize) is read.
void Select_SetTexture( float *out )
{
    const int layer = g_qeglobals.current_edit_layer;
    const float scale = g_qeglobals.random_texture_stuff[layer].sampleSize;
    out[0] = scale;
    out[1] = scale;
}

// Select_Invert (0x493F10) - Selection->Invert (Ctrl+I, cmd 33101).  Swaps the active and
// selected display lists wholesale, then re-runs the per-brush bookkeeping
// (Brush_Deselect_Helper on the now-active set, Brush_Select_Helper on the now-selected)
// and the patch instance's selected byte.  Empty-list cases reset the sentinel to itself.
void Select_Invert()
{
    Sys_Printf( "inverting selection...\n" );
    selbrush_t *oldActiveHead = active_brushes.next;
    selbrush_t *oldActiveTail = active_brushes.prev;

    if ( selected_brushes.next == &selected_brushes )      // selected list empty → active empties
    {
        active_brushes.next = &active_brushes;
        active_brushes.prev = &active_brushes;
    }
    else                                                   // active list ← old selected list
    {
        active_brushes.next        = selected_brushes.next;
        active_brushes.prev        = selected_brushes.prev;
        selected_brushes.next->prev = &active_brushes;
        active_brushes.prev->next   = &active_brushes;
    }

    if ( oldActiveHead == &active_brushes )                // old active list empty → selected empties
    {
        selected_brushes.next = &selected_brushes;
        selected_brushes.prev = &selected_brushes;
    }
    else                                                   // selected list ← old active list
    {
        selected_brushes.prev      = oldActiveTail;
        selected_brushes.next      = oldActiveHead;
        oldActiveHead->prev        = &selected_brushes;
        selected_brushes.prev->next = &selected_brushes;
    }

    for ( selbrush_t *i = active_brushes.next; i != &active_brushes; i = i->next )
    {
        Brush_Deselect_Helper( i );
        if ( i->patch )
            i->patch->selected = 0;                        // per-instance selected flag
    }
    for ( selbrush_t *j = selected_brushes.next; j != &selected_brushes; j = j->next )
    {
        Brush_Select_Helper( j );
        if ( j->patch )
            j->patch->selected = 1;
    }
    g_nUpdateBits = -1;
    Sys_Printf( "done.\n" );

    Sel_InvalidateFromLegacy();   // KIWI-UX (both list heads swapped directly)
}


// ═════════════════════════════════════════════════════════════════════════════
//  0x48DC60  Deselect_Brush — move a brush from the selected list back to active.
// ═════════════════════════════════════════════════════════════════════════════
void Deselect_Brush( selbrush_t *b )
{
    Brush_RemoveFromList( b );
    if ( b->next || b->prev )
        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
    // Insert at the head of active_brushes.
    b->next = active_brushes.next;
    active_brushes.next->prev = b;
    active_brushes.next = b;
    b->prev = &active_brushes;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48E340  Select_ClickTrace - pick a brush/face under the ray and select/deselect.
//    contents&8 (Ctrl+Shift+LMB, 520): single-FACE select/toggle into g_SelectedFaces.
//    else, with faces already selected: brush<->faceset toggle (Brush_SelectFaces/Brush_DeselectFaces).
//    else: whole-brush select/deselect; Alt = the whole entity group.
// ═════════════════════════════════════════════════════════════════════════════

void Select_ClickTrace( float *trace_dir, float *trace_start, int contents )
{
    edTrace_t t;
    Test_Ray( trace_start, trace_dir, contents, &t, 1 );
    selbrush_t *brush = t.hit.brush;
    if ( !brush )
        return;
    // on the Z-view (contents & 0x1000, set by Drag_Begin when viewz==2) reject the pick
    // when brush->brushFlags & 0x20 OR the trace's _pad[0] byte is set (BrushList_Trace sets
    // it when a 0x20 brush was admitted by the 0x1000 pass - which for a prefab hit is an
    // INNER brush, not this one).
    if ( ( contents & 0x1000 ) && ( ( brush->brushFlags & 0x20 ) || t._pad[0] ) )
        return;

    // 0x4000 = light-preview face pick.  The binary diverted to the per-light preview
    // (0x406330) only when the ray hit a LIGHT entity, else fell through to the normal
    // select.  The port does not populate the light-pick target (t.hit.index), so it always
    // falls through: clicking a light bbox simply selects it.
    (void)contents;

    // Reset transient edit modes (d_select_mode back to sel_brush first).
    select_t prevMode = g_qeglobals.d_select_mode;
    g_qeglobals.d_select_mode = sel_brush;
    if ( prevMode == sel_cycle_edge_direction_quad )
        CMainFrame_UpdatePatchToolbarButtons();

    // ── single-FACE selection (Ctrl+Shift+LMB) ────────────────────────────────
    if ( ( contents & 8 ) != 0 && !brush->patch )
    {
        faceVis_s *face = t.hit.face;
        iassert( t.hit.face );   // select.cpp:631

        // Fixed-size entities (bbox brushes) have no editable faces — ignore.
        entity_s_def *od = (entity_s_def *)brush->owner->def;
        if ( *(int *)&od->eclass->fixedsize )
            return;

        Select_BrushesToFaces();   // fold any whole-brush selection into face selection first

        int sz = g_SelectedFaces.GetSize();
        int hit = -1;
        for ( int i = 0; i < sz; ++i )
            if ( g_SelectedFaces.GetAt( i ).face == face ) { hit = i; break; }

        if ( hit >= 0 )
        {
            // already selected → toggle off
            if ( !g_qeglobals.toggle_unk03_mousedrag_state1 )
            {
                g_SelectedFaces.RemoveAt( hit, 1 );
                g_qeglobals.toggle_unk04_mousedrag_state2 = 1;
                g_nUpdateBits = -1;
            }
            return;
        }

        // not selected → add it
        if ( g_qeglobals.toggle_unk04_mousedrag_state2 )
            return;
        selface_t selFace;
        selFace.brush = t.hit.brush;
        selFace.face  = face;
        selFace.index = (int)( face - brush->faces );
        iassert( selFace.index >= 0 && selFace.index < t.hit.brush->faceCount );    // select.cpp:664
        iassert( selFace.brush->version == selFace.brush->def->version );           // select.cpp:665
        g_SelectedFaces.Add( selFace );
        g_qeglobals.toggle_unk03_mousedrag_state1 = 1;
        g_nUpdateBits = -1;
        return;
    }

    // ── no face selection active (or a patch hit) → whole-brush select/deselect ──
    if ( !g_SelectedFaces.GetSize() || brush->patch )
    {
        if ( t.selected )
        {
            if ( !g_qeglobals.toggle_unk03_mousedrag_state1 )
            {
                entity_s *owner = brush->owner;
                if ( owner && owner != world_entity && GetAsyncKeyState( VK_MENU ) < 0 )
                {
                    for ( selbrush_t *v = selected_brushes.next; v != &selected_brushes; )
                    {
                        selbrush_t *next = v->next;
                        if ( v->owner == owner ) Deselect_Brush( v );
                        v = next;
                    }
                }
                else
                {
                    Deselect_Brush( brush );
                }
                {
                    extern bool  PatchDialog_IsOpen();           // patchdialog.cpp
                    extern void  g_PatchDialog_GetPatchInfo();
                    if ( PatchDialog_IsOpen() )               // rebind Patch Inspector after deselect
                        g_PatchDialog_GetPatchInfo();
                }
                g_qeglobals.toggle_unk04_mousedrag_state2 = 1;
                g_nUpdateBits = -1;
            }
            return;
        }
        if ( g_qeglobals.toggle_unk04_mousedrag_state2 )
            return;
        bool alt = GetKeyState( VK_MENU ) < 0;
        Select_Brush( brush, alt, 1, 0 );
        g_qeglobals.toggle_unk03_mousedrag_state1 = 1;
        g_nUpdateBits = -1;
        return;
    }

    // ── faces already selected, hit a non-patch brush → brush↔faceset toggle ─────
    entity_s *grpOwner = brush->owner;
    bool alt = ( grpOwner && grpOwner != world_entity && GetAsyncKeyState( VK_MENU ) < 0 );

    int sz  = g_SelectedFaces.GetSize();
    int hit = -1;
    for ( int i = 0; i < sz; ++i )
        if ( (face_t *)t.hit.face == (face_t *)g_SelectedFaces.GetAt( i ).face ) { hit = i; break; }

    if ( hit >= 0 )
    {
        // some of the brush's faces are already selected → remove them (toggle off)
        if ( !g_qeglobals.toggle_unk03_mousedrag_state1 )
        {
            if ( alt && active_brushes.next != &active_brushes )
            {
                for ( selbrush_t *v = active_brushes.next; v != &active_brushes; )
                {
                    selbrush_t *next = v->next;
                    if ( v->owner == grpOwner ) Brush_DeselectFaces( v );
                    v = next;
                }
            }
            else if ( !alt )
            {
                Brush_DeselectFaces( brush );
            }
            g_qeglobals.toggle_unk04_mousedrag_state2 = 1;
            g_nUpdateBits = -1;
        }
        return;
    }

    // not selected → add the brush's faces (toggle on)
    if ( g_qeglobals.toggle_unk04_mousedrag_state2 )
        return;
    if ( alt )
    {
        for ( selbrush_t *v = active_brushes.next; v != &active_brushes; )
        {
            selbrush_t *next = v->next;
            if ( v->owner == grpOwner ) Brush_SelectFaces( v );
            v = next;
        }
    }
    else
    {
        Brush_SelectFaces( brush );
    }
    g_qeglobals.toggle_unk03_mousedrag_state1 = 1;
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48E800  Select_Deselect - bDeselectFaces also clears the face selection.
//  Head-node aliasing (selected.prev->next), 3-axis maxs>mins new-brush bounds template.
// ═════════════════════════════════════════════════════════════════════════════
void Select_Deselect( int bDeselectFaces )
{
    Patch_Deselect();

    for ( selbrush_t *i = selected_brushes.next;
          i != &selected_brushes;
          i = i->next )
    {
        i->xx6 = 0;
        Brush_Deselect_Helper( i );
    }
    ++g_qeglobals.d_workcount;

    selbrush_t *first = selected_brushes.next;
    g_qeglobals.d_select_count             = 0;
    g_qeglobals.d_select_info.numPatches   = 0;
    g_qeglobals.d_select_info.numBrushes   = 0;
    g_qeglobals.d_select_info.numFixedSize = 0;
    g_qeglobals.d_num_move_points          = 0;

    if ( bDeselectFaces )
    {
        g_SelectedFaces.SetSize( 0 );    // inline CArray RemoveAll (free + cap=0 + size=0)
    }

    if ( first != &selected_brushes )
    {
        ResetSelectMode();

        // Copy last-selected brush's bounds as the new-brush template.
        if ( first->def->maxs[0] > (double)first->def->mins[0] )
        {
            g_qeglobals.d_new_brush_bottom_x = first->def->mins[0];
            g_qeglobals.d_new_brush_top_x    = first->def->maxs[0];
        }
        if ( first->def->maxs[1] > (double)first->def->mins[1] )
        {
            g_qeglobals.d_new_brush_bottom_y = first->def->mins[1];
            g_qeglobals.d_new_brush_top_y    = first->def->maxs[1];
        }
        if ( first->def->maxs[2] > (double)first->def->mins[2] )
        {
            g_qeglobals.d_new_brush_bottom_z = first->def->mins[2];
            g_qeglobals.d_new_brush_top_z    = first->def->maxs[2];
        }

        // Splice selected_brushes list into front of active_brushes.
        selected_brushes.next->prev = &active_brushes;
        selected_brushes.prev->next = active_brushes.next;
        active_brushes.next->prev   = selected_brushes.prev;
        active_brushes.next         = selected_brushes.next;
        selected_brushes.next       = &selected_brushes;
        selected_brushes.prev       = &selected_brushes;

        UpdateSelection( 0xFFFFFFFF, 0 );
    }
    g_nUpdateBits = 0xFFFFFFFF;

    // KIWI-UX (shakeout D): the selected→active splice bypasses the brush.cpp
    // funnels, AND this is the one legacy path that means "drop everything" —
    // which the typed layer can no longer infer from an empty selected_brushes,
    // because since shakeout D an edge/vertex-only selection legitimately leaves
    // that list empty (kiwi_selection.cpp, the carry rule).  So it is said
    // explicitly here instead.  Sel_NoteLegacyDeselect self-suppresses while
    // Sel_SyncToLegacy is driving this function itself.
    Sel_NoteLegacyDeselect();     // KIWI-UX (implies Sel_InvalidateFromLegacy)
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48E760  Select_Delete - free every selected brush, then free any owner entity left
//  with no brushes (worldspawn excepted).  Used by Map_RegionBrush / Map_RegionTallBrush
//  (delete the defining brush after caging the region) and by Edit->Delete.  The transient
//  face/patch-edit modes are reset first.
// ═════════════════════════════════════════════════════════════════════════════

void Select_Delete()
{
    g_SelectedFaces.SetSize( 0 );        // inline CArray RemoveAll (free + cap=0 + size=0)
    select_t prevMode      = g_qeglobals.d_select_mode;
    g_qeglobals.d_select_mode = sel_brush;
    if ( prevMode == sel_cycle_edge_direction_quad )
        CMainFrame_UpdatePatchToolbarButtons();

    g_qeglobals.d_num_move_points = 0;
    while ( selected_brushes.next != &selected_brushes )
    {
        selbrush_t *sbn   = selected_brushes.next;
        entity_s   *owner = sbn->owner;
        Brush_Free( sbn );   // unlinks sbn from both the display list and its owner list
        // If the owner entity has no brushes left (and isn't worldspawn), free it.
        if ( (void *)owner->brushes.ownerNext == (void *)&owner->brushes
             && owner != world_entity )
            Entity_Free( owner );
    }
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48EE10  Select_BrushByLayer
// ═════════════════════════════════════════════════════════════════════════════
void Select_BrushByLayer( char *layer_str )
{
    Select_Deselect( 1 );

    selbrush_t *brush = active_brushes.next;
    if ( brush != &active_brushes )
    {
        do
        {
            selbrush_t *brushNext = brush->next;
            if ( !FilterBrush( brush, 0 ) )
            {
                int bf = brush->brushFlags;
                if ( (bf & 2) == 0 && (bf & 0x20) == 0 )
                {
                    iassert( brush->owner );
                    iassert( brush->owner->def == brush->def->owner );

                    if ( !strncmp( brush->def->parent_layer_string, layer_str, strlen(layer_str) )
                         || !strcmp( layer_str, "The Map" ) )
                    {
                        Brush_RemoveFromList( brush );
                        Brush_AddToList2( brush );
                    }
                }
            }
            brush = brushNext;
        }
        while ( brush != &active_brushes );
    }
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  REGION / MARQUEE SELECTION  (Select_*Tall / Select_Touching_R / Select_Inside_R
//  + Select_FlipFilteredBrushes marquee box-build)
//
//  The four "Select ... Tall / Touching / Inside" menu commands take the SINGLE selected
//  brush as a selection BOX (QE_SingleBrush), grab its def mins/maxs, DELETE that brush,
//  then walk active_brushes and (re)select every brush satisfying the box test.  "Tall"
//  tests only the two SCREEN axes of the active XY view; Touching/Inside test all three.
//  Select_FlipFilteredBrushes is the live MARQUEE drag-box select (Drag_MouseUp): it takes
//  a precomputed world box + a flag selecting which list to walk (Alt+Shift+LMB box-ADD
//  mode 12 / Alt+Shift+MMB box-DESELECT mode 13).
//  View-axis selection (identical in all five):
//    v_a = (m_nViewType == 0)        // YZ view -> axis 1 (Y), else axis 0 (X)
//    v_b = (m_nViewType != 2) + 1    // XY view -> axis 1 (Y), else axis 2 (Z)
//  giving the two on-screen axes: XY {0,1}, XZ {0,2}, YZ {1,2}.
// ═════════════════════════════════════════════════════════════════════════════

// Active XY view's two on-screen axes (the depth axis is dropped for "Tall").
// Ed_ActiveXY() is never NULL (the state block's own default is ED_VIEW_XY == 2).
static inline int Region_ViewAxisA() {
    int vt = Ed_ActiveXY()->m_nViewType;
    return (vt == 0);                    // YZ→1, XZ/XY→0
}
static inline int Region_ViewAxisB() {
    int vt = Ed_ActiveXY()->m_nViewType;
    return (vt != 2) + 1;                // XY→1, XZ/YZ→2
}

// Common prologue for the four menu cores:
// snapshot the single selected brush's box, reset the transient select mode, delete it.
static bool Region_BeginFromSingleBrush( float boxMins[3], float boxMaxs[3] )
{
    if ( !QE_SingleBrush() )
        return false;

    ResetSelectMode();   // sel_cycle→UpdatePatchToolbar; mode→sel_brush

    brush_t *def = selected_brushes.next->def;
    boxMins[0] = def->mins[0];  boxMins[1] = def->mins[1];  boxMins[2] = def->mins[2];
    boxMaxs[0] = def->maxs[0];  boxMaxs[1] = def->maxs[1];  boxMaxs[2] = def->maxs[2];
    Select_Delete();           // free the brush that defined the box
    return true;
}

// ── 0x490170  Select_CompleteTall ────────────────────────────────────────────
// Select every brush COMPLETELY CONTAINED by the box on the two screen axes.
void Select_CompleteTall()
{
    float mins[3], maxs[3];
    if ( !Region_BeginFromSingleBrush( mins, maxs ) )
        return;

    const int a = Region_ViewAxisA();
    const int b = Region_ViewAxisB();

    for ( selbrush_t *sb = active_brushes.next; sb != &active_brushes; )
    {
        brush_t    *def  = sb->def;
        selbrush_t *next = sb->next;
        // containment: box.maxs >= brush.maxs && box.mins <= brush.mins on both axes
        if ( maxs[a] >= def->maxs[a] && mins[a] <= def->mins[a]
          && maxs[b] >= def->maxs[b] && mins[b] <= def->mins[b]
          && !FilterBrush( sb, 0 ) )
        {
            int bf = sb->brushFlags;
            if ( (bf & 2) == 0 && (bf & 0x20) == 0 )
            {
                Brush_RemoveFromList( sb );
                Select_Brush_2( &selected_brushes, sb );
            }
        }
        sb = next;
    }
    g_nUpdateBits = -1;
}

// ── 0x4903D0  Select_PartialTall ─────────────────────────────────────────────
// Select every brush that OVERLAPS the box on the two screen axes.
void Select_PartialTall()
{
    float mins[3], maxs[3];
    if ( !Region_BeginFromSingleBrush( mins, maxs ) )
        return;

    const int a = Region_ViewAxisA();
    const int b = Region_ViewAxisB();

    for ( selbrush_t *sb = active_brushes.next; sb != &active_brushes; )
    {
        brush_t    *def  = sb->def;
        selbrush_t *next = sb->next;
        // overlap: box.maxs >= brush.mins && box.mins <= brush.maxs on both axes
        if ( maxs[a] >= def->mins[a] && mins[a] <= def->maxs[a]
          && maxs[b] >= def->mins[b] && mins[b] <= def->maxs[b]
          && !FilterBrush( sb, 0 ) )
        {
            int bf = sb->brushFlags;
            if ( (bf & 2) == 0 && (bf & 0x20) == 0 )
            {
                Brush_RemoveFromList( sb );
                Select_Brush_2( &selected_brushes, sb );
            }
        }
        sb = next;
    }
    g_nUpdateBits = -1;
}

// ── 0x490520  Select_Touching_R ──────────────────────────────────────────────
// Select every brush whose bounds touch the box on ALL THREE axes (with a +1 unit
// epsilon — "touching" includes brushes that just abut the box face).
void Select_Touching_R()
{
    float mins[3], maxs[3];
    if ( !Region_BeginFromSingleBrush( mins, maxs ) )
        return;

    for ( selbrush_t *sb = active_brushes.next; sb != &active_brushes; )
    {
        selbrush_t *next = sb->next;
        brush_t    *def  = sb->def;
        int i = 0;
        for ( ; i < 3; ++i )
        {
            if ( maxs[i] + 1.0f < def->mins[i] ) break;   // box.maxs+1 < brush.mins → miss
            if ( mins[i] - 1.0f > def->maxs[i] ) break;   // box.mins-1 > brush.maxs → miss
        }
        if ( i == 3 && !FilterBrush( sb, 0 ) )
        {
            int bf = sb->brushFlags;
            if ( (bf & 2) == 0 && (bf & 0x20) == 0 )
            {
                Brush_RemoveFromList( sb );
                if ( sb->next || sb->prev )
                    Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
                Brush_AddToList2( sb );
            }
        }
        sb = next;
    }
    g_nUpdateBits = -1;
}

// ── 0x490650  Select_Inside_R ────────────────────────────────────────────────
// Select every brush fully CONTAINED by the box on ALL THREE axes (no epsilon).
void Select_Inside_R()
{
    float mins[3], maxs[3];
    if ( !Region_BeginFromSingleBrush( mins, maxs ) )
        return;

    for ( selbrush_t *sb = active_brushes.next; sb != &active_brushes; )
    {
        selbrush_t *next = sb->next;
        brush_t    *def  = sb->def;
        int i = 0;
        for ( ; i < 3; ++i )
        {
            if ( maxs[i] < def->maxs[i] ) break;   // box.maxs < brush.maxs → not contained
            if ( mins[i] > def->mins[i] ) break;   // box.mins > brush.mins → not contained
        }
        if ( i == 3 && !FilterBrush( sb, 0 ) )
        {
            int bf = sb->brushFlags;
            if ( (bf & 2) == 0 && (bf & 0x20) == 0 )
            {
                Brush_RemoveFromList( sb );
                if ( sb->next || sb->prev )
                    Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
                Brush_AddToList2( sb );
            }
        }
        sb = next;
    }
    g_nUpdateBits = -1;
}

// ── 0x4902C0  Select_FlipFilteredBrushes (marquee drag box-select) ────────────
//  Called from Drag_MouseUp for the live marquee. The box [boxMins,boxMaxs] is
//  pre-built in world space from the drag rectangle. bActiveList selects which
//  list to scan and where matches go:
//    bActiveList = 1 (mode 12, box ADD)     → walk active_brushes,   move matches → selected
//    bActiveList = 0 (mode 13, box DESELECT) → walk selected_brushes, move matches → active
//  The bounds test is CONTAINMENT on the active XY view's two screen axes (Tall),
//  identical to Select_CompleteTall.
void Select_FlipFilteredBrushes( const float *boxMins, const float *boxMaxs, char bActiveList )
{
    const int a = Region_ViewAxisA();
    const int b = Region_ViewAxisB();

    selbrush_t *head = bActiveList ? &active_brushes : &selected_brushes;
    selbrush_t *dst  = bActiveList ? &selected_brushes : &active_brushes;

    for ( selbrush_t *sb = head->next; sb != head; )
    {
        brush_t    *def  = sb->def;
        selbrush_t *next = sb->next;
        if ( boxMaxs[a] >= def->maxs[a] && boxMins[a] <= def->mins[a]
          && boxMaxs[b] >= def->maxs[b] && boxMins[b] <= def->mins[b]
          && !FilterBrush( sb, 0 ) )
        {
            int bf = sb->brushFlags;
            if ( (bf & 2) == 0 && (bf & 0x20) == 0 )
            {
                Brush_RemoveFromList( sb );
                Select_Brush_2( dst, sb );
            }
        }
        sb = next;
    }
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48F170  Brush_SetTexture - the def version is 16-bit; patch brushes fan the copy out
//  to every face.
// ═════════════════════════════════════════════════════════════════════════════
void Brush_SetTexture( MaterialDef *material, char planarProject )
{
    int  faceCount = g_SelectedFaces.GetSize();
    const char *opName;

    if ( faceCount )
        opName = "set face textures";
    else
    {
        if ( selected_brushes.next == &selected_brushes )
            return;
        opName = "set brush textures";
    }

    Undo_ClearRedo();
    Undo_GeneralStart( opName );

    if ( faceCount > 0 )
    {
        for ( int i = 0; i < faceCount; ++i )
        {
            selface_t  &selFace = g_SelectedFaces.GetAt( i );   // CArray::GetAt (bounds ENSURE)
            selbrush_t *b     = selFace.brush;
            int         index = selFace.index;
            brush_t    *b_def = b->def;

            Undo_TryAddBrush( b_def );

            iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:1114
            iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:1115
            iassert( selFace.brush->faceCount == selFace.brush->def->faceCount );   // select.cpp:1116

            // Copy MaterialDef into the face's current layer slot.
            MaterialDef *dstSlot = &b_def->faces[index].mtldef[g_qeglobals.current_edit_layer];
            qmemcpy( dstSlot, material, sizeof(MaterialDef) );

            if ( b_def->patch )
            {
                for ( unsigned j = 0; j < (unsigned)b_def->faceCount; ++j )
                {
                    qmemcpy( (char *)&b_def->faces[j].mtldef[g_qeglobals.current_edit_layer],
                             dstSlot,
                             sizeof(MaterialDef) );
                }
            }

            ++b_def->version;
            Brush_BuildWindings( b->def, 0 );
            if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
                SetupVertexSelection();
            MarkMapModified();
            ++b->def->version;
            Brush_UpdateSpecialMaterialFlag( b->def );
            Undo_LinkBrush( b->def );
            Brush_CheckBuildFaceVis( b, (const float *)world_orient_matrix );
            iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:1125
            iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:1126
        }
    }

    // Per-brush pass (non-fixedsize brushes only).
    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes;
          b = b->next )
    {
        entity_s_def *def = (entity_s_def *)b->owner->def;
        if ( !*(int *)&def->eclass->fixedsize )
        {
            Undo_TryAddBrush( b->def );
            Brush_SetAllFacesMaterial( b->def, material, planarProject, 0.0f, 1 );
            Undo_LinkBrush( b->def );
        }
    }

    Undo_End();
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48F4F0  Brush_SetTextureMapping - applies `texdef` via Brush_SetFaceTexdef.
// ═════════════════════════════════════════════════════════════════════════════
void Brush_SetTextureMapping( texdef_sub_t *texdef )
{
    int faceCount = g_SelectedFaces.GetSize();
    const char *opName;

    if ( faceCount )
        opName = "set face texture mapping";
    else
    {
        if ( selected_brushes.next == &selected_brushes )
            return;
        opName = "set brush texture mapping";
    }

    Undo_ClearRedo();
    Undo_GeneralStart( opName );

    for ( int i = 0; i < faceCount; ++i )
    {
        selface_t  &selFace = g_SelectedFaces.GetAt( i );   // CArray::GetAt (bounds ENSURE)
        selbrush_t *b     = selFace.brush;
        int         index = selFace.index;
        brush_t    *def   = b->def;

        Undo_TryAddBrush( def );

        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:1161
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:1162
        iassert( selFace.brush->faceCount == selFace.brush->def->faceCount );   // select.cpp:1163

        Brush_SetFaceTexdef( texdef, &def->faces[index], def );

        Brush_RebuildAfterEdit( def );
        Brush_UpdateSpecialMaterialFlag( def );
        Undo_LinkBrush( def );
        Brush_CheckBuildFaceVis( b, (const float *)world_orient_matrix );
        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:1172
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:1173
    }

    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes;
          b = b->next )
    {
        entity_s_def *def = (entity_s_def *)b->owner->def;
        if ( !*(int *)&def->eclass->fixedsize )
        {
            Undo_TryAddBrush( b->def );
            Brush_SetAllFacesTexdef( b->def, texdef );
            Undo_LinkBrush( b->def );
        }
    }

    Undo_End();
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48F800  Brush_SetSampleSize
// ═════════════════════════════════════════════════════════════════════════════
void Brush_SetSampleSize( int size )
{
    int faceCount = g_SelectedFaces.GetSize();
    const char *opName;

    if ( faceCount )
        opName = "set face samplesize";
    else
    {
        if ( selected_brushes.next == &selected_brushes )
            return;
        opName = "set brush samplesize";
    }

    Undo_ClearRedo();
    Undo_GeneralStart( opName );

    for ( int i = 0; i < faceCount; ++i )
    {
        selface_t  &selFace = g_SelectedFaces.GetAt( i );   // CArray::GetAt (bounds ENSURE)
        selbrush_t *b     = selFace.brush;
        int         index = selFace.index;
        brush_t    *def   = b->def;

        Undo_TryAddBrush( def );

        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:1208
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:1209
        iassert( selFace.brush->faceCount == selFace.brush->def->faceCount );   // select.cpp:1210

        Face_SetSampleSize( &b->def->faces[index], b->def, size );
        Brush_RebuildAfterEdit( b->def );
        Brush_UpdateSpecialMaterialFlag( b->def );
        Undo_LinkBrush( b->def );
        Brush_CheckBuildFaceVis( b, (const float *)world_orient_matrix );
        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:1219
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:1220
    }

    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes;
          b = b->next )
    {
        entity_s_def *def = (entity_s_def *)b->owner->def;
        if ( !*(int *)&def->eclass->fixedsize )
        {
            Undo_TryAddBrush( b->def );
            Brush_SetAllFacesSampleSize( b->def, size );
            Undo_LinkBrush( b->def );
        }
    }

    Undo_End();
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48FDC0  Select_Scale
// ═════════════════════════════════════════════════════════════════════════════
void Select_Scale( float sx, float sy, float sz )
{
    float mid[3];
    Select_GetMid( mid );

    selbrush_t *b = selected_brushes.next;
    if ( b == &selected_brushes )
        return;

    do
    {
        patch_t *patch = (patch_t *)b->patch;
        if ( patch )
        {
            float scale[3] = { sx, sy, sz };
            iassert( b->patch->def == b->def->patch );   // select.cpp:1351
            Patch_Scale( b->def->patch, mid, scale, 1 );
        }
        else
        {
            if ( b->faceCount )
            {
                for ( int fi = 0; fi < b->faceCount; ++fi )
                {
                    float *planepts = &b->def->faces[fi].planepts[0][0];
                    for ( int vi = 0; vi < 3; ++vi )
                    {
                        float *pt = planepts + vi * 3;
                        pt[0] = ( pt[0] - mid[0] ) * sx + mid[0];
                        pt[1] = ( pt[1] - mid[1] ) * sy + mid[1];
                        pt[2] = ( pt[2] - mid[2] ) * sz + mid[2];
                    }
                }
            }
            Brush_BuildWindings( b->def, 0 );
            if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
                SetupVertexSelection();
            MarkMapModified();
            ++b->def->version;
        }
        b = b->next;
    }
    while ( b != &selected_brushes );
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48FB10  Select_GetBounds - AABB enclosing every selected brush def.  Init to the
//  editor's +/-131072 sentinel box, then union each brush's [mins,maxs].
// ═════════════════════════════════════════════════════════════════════════════
void Select_GetBounds( float *mins, float *maxs )
{
    mins[0] = mins[1] = mins[2] =  131072.0f;
    maxs[0] = maxs[1] = maxs[2] = -131072.0f;
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        const float *bmin = b->def->mins;
        const float *bmax = b->def->maxs;
        for ( int i = 0; i < 3; ++i )
        {
            if ( bmin[i] < mins[i] ) mins[i] = bmin[i];
            if ( bmax[i] > maxs[i] ) maxs[i] = bmax[i];
        }
    }
}

// ─── Select_GetTrueMid (0x48FC20) — centre of the selection bounding box ──────────
// Used by CXYWnd::SetRotateMode to seat the mouse-rotation pivot (g_vRotateOrigin).
void Select_GetTrueMid( float *center )
{
    float mins[3], maxs[3];
    Select_GetBounds( mins, maxs );
    center[0] = ( maxs[0] + mins[0] ) * 0.5f;
    center[1] = ( maxs[1] + mins[1] ) * 0.5f;
    center[2] = ( maxs[2] + mins[2] ) * 0.5f;
}

// ─── Select_GetFitBounds (0x48FB70) — bounds of the current selection for Texture_Fit
//     (selectionBounds != 0, "fit texture across the whole selection") ───────────────
// If individual FACES are selected, expand [mins,maxs] (already seeded +/-131072 by the
// caller) by every selected face's winding points; otherwise fall back to the selected
// BRUSHES' bbox (Select_GetBounds).  Reached via Brush_FitTexture( ..., 1 ) (Fit All).
// BINARY QUIRK, kept: numpoints is read from the brush's FACE 0 winding while the points
// iterated belong to the selected face (selFace[i].index); the null guards are the port's.
void Select_GetFitBounds( float *minsOut, float *maxsOut )
{
    float *mins = (float *)(intptr_t)minsOut;
    float *maxs = (float *)(intptr_t)maxsOut;

    const int count = g_SelectedFaces.GetSize();
    if ( !count )
    {
        Select_GetBounds( mins, maxs );
        return;
    }

    for ( int i = 0; i < count; ++i )
    {
        face_t *faces = g_SelectedFaces.GetAt( i ).brush->def->faces;
        if ( !faces[0].w )
            continue;
        const int numpoints = faces[0].w->numpoints;             // face 0's count (binary quirk)
        if ( !numpoints )
            continue;
        winding_t *w = faces[g_SelectedFaces.GetAt( i ).index].w;   // selected face's winding
        if ( !w )
            continue;
        for ( int n = 0; n < numpoints; ++n )
        {
            const float *p = w->p[n];
            if ( p[0] < mins[0] ) mins[0] = p[0];
            if ( p[0] > maxs[0] ) maxs[0] = p[0];
            if ( p[1] < mins[1] ) mins[1] = p[1];
            if ( p[1] > maxs[1] ) maxs[1] = p[1];
            if ( p[2] < mins[2] ) mins[2] = p[2];
            if ( p[2] > maxs[2] ) maxs[2] = p[2];
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48E9C0  Select_Move - move every selected brush by `delta`, then refresh the status
//  bar with the new selection origin.  Called by MoveSelection (drag.cpp) and XY_MouseDown.
//  The status text goes to MainFrm_SetStatusText.
// ═════════════════════════════════════════════════════════════════════════════
extern void      MainFrm_SetStatusText( int pane, const char *text );       // mainfrm.cpp

void Select_Move( const float *delta, char bSnap )
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        Brush_Move( delta, b->def, bSnap );

    float bmins[3], bmaxs[3];
    Select_GetBounds( bmins, bmaxs );
    char buf[128];
    // X comes from the mins buffer but Y/Z from the maxs buffer - a mixed-corner readout
    // in the original CoD3 source, kept.
    _snprintf( buf, sizeof( buf ), "Origin X:: %.1f  Y:: %.1f  Z:: %.1f",
               bmins[0], bmaxs[1], bmaxs[2] );
    MainFrm_SetStatusText( 2, buf );
}

// ═════════════════════════════════════════════════════════════════════════════
//  SELECTION TRANSFORMS - rotate / flip / arbitrary-matrix apply.
//  The Flip/Rotate menu commands (CMainFrame::OnBrushFlipx/y/z + OnBrushRotatex/y/z) and
//  the rotate-mode keyboard nudge (NudgeSelection) all build a rotation/mirror matrix into
//  a `float rot_around[4][3]` ORIENTATION block - row 0 = the pivot origin (Select_GetMid),
//  rows 1..3 = the 3x3 axis matrix - then push every selected brush through
//  Select_ApplyMatrix_SelectedBrushes -> Select_ApplyMatrix, which transforms each face's
//  3 planepts about the pivot and rebuilds windings.
//  The matrix layout IS an orientation_t, so Select_FlipAxis/Select_RotateAxis declare it
//  as one `float rot_around[4][3]`.  Select_ApplyMatrix's `mid` arg IS that block; it
//  casts to orientation_t* and calls OrientationPosToWorldPos.
//  The texture / lightmap lock (0x470570 / 0x4706F0, lockFlags { m_bRotateLock,
//  m_bLightmapLock, 1 }) is live - MaterialDef_04 is > 0 for real materials.  KIWI does it
//  exactly (kiwi_texlock.h: Face_TexLockAffineBegin / End).
// ═════════════════════════════════════════════════════════════════════════════

// ── deps for the transform core ──────────────────────────────────────────────
// com_math.h supplies MatrixMultiply / AnglesToAxis / AxisToAngles / Vec3RotateTranspose /
// MatrixTranspose with their exact compiled (mat3x3&) signatures - hand-declaring mangles
// wrong.
#include <universal/com_math.h>

// OrientationPosToWorldPos is DEFINED in draw.cpp (q_shared 0x4BA430), not com_math.h.

extern brush_t  *Brush_Clone( brush_t *def );                               // brush.cpp (0x475d20)
extern char      g_nScaleHow;                                               // drag.cpp (0x23F16DC)
#include "prefs.h"                                                          // g_PrefsDlg (m_bNoClamp ...)

// 0x48FC70  Select_GetMid — selection-bounds midpoint. Grid-snapped (floor toward the
// grid) unless the NoClamp pref is set. Used by rotate/flip/scale as the pivot.
// FLOAT floorf here.
void Select_GetMid( float *mid )
{
    float bmins[3], bmaxs[3];
    Select_GetBounds( bmins, bmaxs );
    if ( g_PrefsDlg->m_bNoClamp )
    {
        mid[0] = ( bmaxs[0] + bmins[0] ) * 0.5f;
        mid[1] = ( bmaxs[1] + bmins[1] ) * 0.5f;
        mid[2] = ( bmaxs[2] + bmins[2] ) * 0.5f;
    }
    else
    {
        for ( int i = 0; i < 3; ++i )
        {
            float gs = grid_sizes[g_qeglobals.d_gridsize];
            mid[i] = floorf( ( bmins[i] + bmaxs[i] ) * 0.5f / gs ) * gs;
        }
    }
}

// SinCosDeg (0x4AABC0) is texturevecs.cpp's Ed_SinCos.
extern void Ed_SinCos( float deg, float *s, float *c );

// Select_ApplyMatrix (0x47CDE0) lives in brush.cpp (its asserts are brush.cpp:5598/5608).
extern void Select_ApplyMatrix( float *mat, selbrush_t *b, int bSnap, float deg,
                                char bSwap );   // brush.cpp 0x47CDE0

// 0x48FD10  Select_ApplyMatrix_SelectedBrushes — apply `mat` to every selected brush.
void Select_ApplyMatrix_SelectedBrushes( int bSnap, float *mat, float deg, char bSwap )
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        Select_ApplyMatrix( mat, b, bSnap, deg, bSwap );
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x47CFD0  Brush_DropPivot — the drop-to-floor scatter PIVOT (OnDropSelected's model
//  scatter rotates the entity about it).  The binary used the bbox mid snapped DOWN to the
//  grid unless NoClamp.
//  KIWI: the entity's own origin - a random yaw about the grid-floored mid swung the model
//  around a point up to a grid cell away (~90 units at grid 64); now it turns in place.
// ═════════════════════════════════════════════════════════════════════════════
static void Brush_DropPivot( float *mid, selbrush_t *b )
{
    const entity_s_def *def = (const entity_s_def *)b->owner->def;
    mid[0] = def->origin[0];
    mid[1] = def->origin[1];
    mid[2] = def->origin[2];
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x47CBA0  Brush_RotateAboutPivot — rotate one brush `deg` degrees about its pivot on
//  `axis` (0=X/pitch, 1=Y/roll, 2=Z/yaw). Builds the orientation block (pivot +
//  per-axis 3x3) and hands it to Select_ApplyMatrix. deg==0 is a no-op. The binary
//  negates the angle before SinCosDeg + ApplyMatrix. Used by OnDropSelected's
//  random model scatter (3 calls, axes 0/1/2).
// ═════════════════════════════════════════════════════════════════════════════
void Brush_RotateAboutPivot( selbrush_t *b, int axis, float deg )
{
    if ( deg == 0.0f )
        return;

    float m[12];                 // orientation_t: m[0..2]=pivot, m[3..11]=3x3 (row-major)
    Brush_DropPivot( m, b );          // pivot = the entity origin

    float rot = -deg;
    float s, c;
    Ed_SinCos( rot, &s, &c );

    m[3] = 1.0f; m[4]  = 0.0f; m[5]  = 0.0f;   // identity 3x3
    m[6] = 0.0f; m[7]  = 1.0f; m[8]  = 0.0f;
    m[9] = 0.0f; m[10] = 0.0f; m[11] = 1.0f;

    switch ( axis )
    {
    case 0:  m[7] = c;  m[10] = -s;  m[8] =  s;  m[11] = c;  break;   // about X
    case 1:  m[3] = c;  m[9]  =  s;  m[5] = -s;  m[11] = c;  break;   // about Y
    case 2:  m[3] = c;  m[6]  = -s;  m[4] =  s;  m[7]  = c;  break;   // about Z
    }

    Select_ApplyMatrix( m, b, 1, rot, 0 );
    g_nUpdateBits = -1;
}

//  0x47CC90  Select_RotateFixedSize — rotate a fixed-size entity about the pivot.
//  Rotates its origin around the pivot (Vec3RotateTranspose by the 3x3) and composes
//  the rotation onto its `angles` key. mid_point = the orientation block (mid_point[0]
//  = pivot origin, mid_point[1] = the 3x3). `rot` is the eclass (unused; kept for sig).
// ═════════════════════════════════════════════════════════════════════════════
void Select_RotateFixedSize( selbrush_t *sb, float (*mid_point)[3], const float * /*rot*/ )
{
    entity_s_def *def = (entity_s_def *)sb->owner->def;

    float moveDelta[3];
    moveDelta[0] = def->origin[0] - mid_point[0][0];
    moveDelta[1] = def->origin[1] - mid_point[0][1];
    moveDelta[2] = def->origin[2] - mid_point[0][2];
    float distSq = moveDelta[0] * moveDelta[0] + moveDelta[1] * moveDelta[1]
                 + moveDelta[2] * moveDelta[2];
    // mid_point[1..3] is the 3x3 axis of the orientation block (mid_point[0] = pivot).
    // KIWI (2026-10-05, precision): in double.  Ed_SinCos' float noise (cos 90 = -4.4e-8) is
    // cleaned off the matrix, the origin lands on pivot + rel * rotM in one rounding (not
    // origin + (rotated - rel)), and the angles compose from their own text (kiwi_angles.h) -
    // the float AnglesToAxis / AxisToAngles left noise the exact writer then printed.
    double turn[3][3];
    for ( int a = 0; a < 3; ++a )
        for ( int b = 0; b < 3; ++b )
        {
            const double m = mid_point[1 + a][b];
            const double r = floor( m + 0.5 );                 // 0 / +-1: a quarter turn's entries
            turn[a][b] = ( fabs( m - r ) < 1.0e-7 && fabs( r ) <= 1.0 ) ? r + 0.0 : m;
        }
    if ( distSq != 0.0f )
    {
        // rotated offset of the origin about the pivot
        double want[3];
        float  move[3];
        for ( int j = 0; j < 3; ++j )
            want[j] = (double)mid_point[0][j] + moveDelta[0] * turn[0][j] + moveDelta[1] * turn[1][j]
                    + moveDelta[2] * turn[2][j];
        for ( int j = 0; j < 3; ++j )
            move[j] = (float)( want[j] - def->origin[j] );
        Brush_Move( move, sb->def, 1 );
        for ( int j = 0; j < 3; ++j )
            def->origin[j] = (float)want[j];
    }

    // newAxis = curAxis * rotM.  NOT commutative.
    double base[3] = { 0.0, 0.0, 0.0 }, turned[3];
    const char *text = ValueForKey2( def, "angles" );
    if ( !text || sscanf( text, "%lf %lf %lf", &base[0], &base[1], &base[2] ) != 3 )
        base[0] = base[1] = base[2] = 0.0;
    KiwiAng_Turn( base, turn, turned );
    const float angles[3] = { (float)turned[0], (float)turned[1], (float)turned[2] };
    char v[160];
    SetKeyValue( def, "angles", KiwiFmt_Vec3Exact( v, sizeof( v ), angles, 0 ) );   // bare, as %g

    Brush_RebuildAfterEdit( sb->def );
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48FF40  Select_RotateAxis — build the rotation orientation block.
//  rot_around[0] = pivot origin (filled by the caller via Select_GetMid).
//  rot_around[1..3] = the 3x3 rotation matrix about `axis` by `deg` degrees.
//  When the lone selection is a fixed-size entity, the rotation axis is re-derived
//  from the entity's local frame + the scale-how flags (so prefabs rotate in their
//  own space); `fixedsize` then enables the orientation re-base at the tail.
// ═════════════════════════════════════════════════════════════════════════════
void Select_RotateAxis( int axis, float deg, float (*rot_around)[4][3] )
{
    selbrush_t *first = selected_brushes.next;
    bool fixedsize = false;
    float localAxis[3][3];   // the entity's own frame, for the fixed-size re-base

    iassert( selected_brushes.next != &selected_brushes );

    // Single fixed-size entity selected → rotate about its own axis.
    if ( first->next == &selected_brushes )
    {
        entity_s_def *def = (entity_s_def *)first->owner->def;
        if ( *(int *)&def->eclass->fixedsize )
        {
            float ang[3];
            if ( !Entity_GetVec3ForKey( def, ang, "angles" ) )
                ang[0] = ang[1] = ang[2] = 0.0f;
            AnglesToAxis( ang, localAxis );
            switch ( g_nScaleHow & 7 )
            {
                case 6: axis = 0; fixedsize = true; break;
                case 5: axis = 1; fixedsize = true; break;
                case 3: axis = 2; fixedsize = true; break;
                default: break;   // not a single-axis scale-how → ordinary axis rotate
            }
        }
    }

    float s, c;
    Ed_SinCos( -deg, &s, &c );

    // Identity-init the 3x3 (rows 1..3 of rot_around).
    float (*m)[3] = &(*rot_around)[1];   // m[0..2] = the 3x3
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f;

    switch ( axis )
    {
        case 0:   // rotate about X
            m[1][1] = c;  m[2][1] = -s;
            m[1][2] = s;  m[2][2] = c;
            break;
        case 1:   // rotate about Y
            m[0][0] = c;  m[2][0] = s;
            m[0][2] = -s; m[2][2] = c;
            break;
        case 2:   // rotate about Z
            m[0][0] = c;  m[1][0] = -s;
            m[0][1] = s;  m[1][1] = c;
            break;
    }

    if ( fixedsize )
    {
        // Re-base the rotation into the entity's local frame with the binary's
        // LEFT-association + single-temp operand sequence:
        //   lt = localAxisᵀ;  tmp = localAxisᵀ · M;  M = (localAxisᵀ · M) · localAxis
        // (algebraically == localAxisᵀ·M·localAxis; the FP op order is kept).
        mat3x3 &M = *reinterpret_cast<mat3x3 *>( m );   // rows 1..3 of rot_around
        float lt[3][3], tmp[3][3];
        MatrixTranspose( localAxis, lt );    // lt  = localAxisᵀ
        MatrixMultiply( lt, M, tmp );        // tmp = localAxisᵀ · M
        MatrixMultiply( tmp, localAxis, M ); // M   = (localAxisᵀ · M) · localAxis
    }
}
// the iassert at the top is select.cpp:1389.

// ═════════════════════════════════════════════════════════════════════════════
//  0x48FD50  Select_FlipAxis — mirror the selection across the `axis` plane through
//  the pivot. Builds an identity 3x3 with the chosen axis negated, then applies it
//  with bSwap=1 (winding reverse so reflected faces stay outward-facing).
// ═════════════════════════════════════════════════════════════════════════════
// KIWI (2026-10-05, reversibility): the fixed-size entities of a selection, as points.
static bool Select_FixedSizeOwner( const selbrush_t *b )
{
    const entity_s_def *ent = b->owner ? (const entity_s_def *)b->owner->def : nullptr;
    return ent && ent->eclass && *(int *)&ent->eclass->fixedsize;
}

void Select_FlipAxisAbout( int axis, const float pivot[3], bool fixedSizeOrigins );

void Select_FlipAxis( int axis )
{
    float rot_around[4][3];
    // KIWI: the true centre.  The binary's Select_GetMid floors it to the legacy grid
    // (16), so a mirror threw the selection up to a grid step sideways.
    // KIWI (2026-10-05, reversibility): a model counts as its origin, not its box - DoFlip
    // turns a model 180 instead of mirroring it, so its box moves to the far side of the
    // origin, and a second mirror pivoted somewhere else (mirror twice walked the selection).
    float mins[3] = {  131072.0f,  131072.0f,  131072.0f };
    float maxs[3] = { -131072.0f, -131072.0f, -131072.0f };
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        const bool fixed = Select_FixedSizeOwner( b );
        const float *lo = fixed ? ( (const entity_s_def *)b->owner->def )->origin : b->def->mins;
        const float *hi = fixed ? ( (const entity_s_def *)b->owner->def )->origin : b->def->maxs;
        for ( int i = 0; i < 3; ++i )
        {
            if ( lo[i] < mins[i] ) mins[i] = lo[i];
            if ( hi[i] > maxs[i] ) maxs[i] = hi[i];
        }
    }
    for ( int i = 0; i < 3; ++i )
        rot_around[0][i] = ( maxs[i] + mins[i] ) * 0.5f;

    Select_FlipAxisAbout( axis, rot_around[0], true );
}

// KIWI (2026-10-12): Select_FlipAxis' reflection about a caller's pivot (the Mirror tool,
// kiwi_dupe.cpp, picks its own and places models itself: fixedSizeOrigins false).
void Select_FlipAxisAbout( int axis, const float pivot[3], bool fixedSizeOrigins )
{
    float rot_around[4][3];
    for ( int i = 0; i < 3; ++i )
        rot_around[0][i] = pivot[i];

    // identity 3x3, then negate the chosen axis (a reflection)
    float (*m)[3] = &rot_around[1];
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f;
    m[axis][axis] = -1.0f;

    Select_ApplyMatrix_SelectedBrushes( 1, rot_around[0], 0.0f, 1 );

    // KIWI (2026-10-05): Select_ApplyMatrix leaves a fixed-size entity where it is on a
    // mirror (deg 0), so a lamp mirrored with its wall stayed on the old side.  Its origin
    // mirrors with the selection; DoFlip still turns the model.
    for ( selbrush_t *b = selected_brushes.next; fixedSizeOrigins && b != &selected_brushes; b = b->next )
    {
        if ( !Select_FixedSizeOwner( b ) )
            continue;
        float delta[3] = { 0.0f, 0.0f, 0.0f };
        delta[axis] = 2.0f * ( rot_around[0][axis] - ( (const entity_s_def *)b->owner->def )->origin[axis] );
        if ( delta[axis] != 0.0f )
            Brush_Move( delta, b->def, 0 );
    }
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x424F30  DoFlip — Brush→Flip→{X,Y,Z} core (called by CMainFrame::OnBrushFlipx/y/z).
//  Undo-brackets the selection, mirrors it (Select_FlipAxis), and for every selected
//  fixed-size entity ALSO flips its `angles` key by 180° on the mirror axis (so a
//  mirrored prefab/model faces the reflected direction), rebuilding windings.
// ═════════════════════════════════════════════════════════════════════════════
void DoFlip( int axis, const char *opName )
{
    Undo_ClearRedo();
    Undo_GeneralStart( opName );
    Undo_AddBrushList( &selected_brushes );
    Select_FlipAxis( axis );

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s_def *def = (entity_s_def *)b->owner->def;
        if ( !*(int *)&def->eclass->fixedsize )
            continue;

        // Read the entity's `angles` epair (default 0 0 0), flip the mirror axis by 180,
        // normalize, write it back (the binary: sscanf "%f %f %f" + AngleNormalize360).
        // KIWI (2026-10-05, precision): the text in double and a flip that is its own inverse
        // (KiwiAng_Flip180), written exact - AngleNormalize360 in float sent "-90" to 90 and
        // back to 270, and 12.3456 to 192.345596 and back to 12.345596.
        double angD[3] = { 0.0, 0.0, 0.0 };
        const char *val = ValueForKey2( def, "angles" );   // "" when missing: zeroes below
        if ( sscanf( val, "%lf %lf %lf", &angD[0], &angD[1], &angD[2] ) != 3 )
            angD[0] = angD[1] = angD[2] = 0.0;
        angD[axis] = KiwiAng_Flip180( angD[axis] );
        const float ang[3] = { (float)angD[0], (float)angD[1], (float)angD[2] };
        char angText[160];
        SetKeyValue( def, "angles", KiwiFmt_Vec3Exact( angText, sizeof( angText ), ang, 0 ) );

        Brush_RebuildAfterEdit( b->def );
    }

    Undo_EndBrushList( &selected_brushes );
    Undo_End();
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48F0D0  Clone_Selection — duplicate every selected brush into its owner entity,
//  then select the duplicates.
//
//  The binary clones via the OLE clipboard (CXYWnd::Copy -> Entity_WriteSelected_R ->
//  CMemFile -> CXYWnd::Paste -> Map_ImportBuffer with full target/targetname auto-remap),
//  and follows the paste with NudgeSelection(2)+(3).  KIWI (2026-09-09) restored that
//  route through the in-app clipboard (RadiantClipboard_CloneSelection): entities,
//  patches and worldspawn brushes all clone.  Only the cosmetic grid offset is not
//  reproduced (`gridSize`, unused for that reason) — KiwiCmd_AfterPaste enters a
//  paused Move on the copies instead.
// ═════════════════════════════════════════════════════════════════════════════
void Clone_Selection( float /*gridSize*/ )
{
    if ( selected_brushes.next == &selected_brushes )
        return;                               // nothing selected: no undo record either

    // KIWI (2026-09-09): the binary's route, minus the OLE mirror and the cosmetic
    // nudge — Copy (Entity_WriteSelected_R) then Paste (Map_ImportBuffer), which owns the
    // whole bracket: Select_Deselect(1), Undo_ClearRedo / Undo_GeneralStart("import
    // buffer"), the target/targetname/script_link auto-renumber, MarkMapModified,
    // Undo_EndBrushList(selected) / Undo_End, and leaves the copies selected.  That is
    // the ONLY clone that is right for entities: a misc_model, trigger or light must
    // come back as a NEW entity with its own epairs.  The previous in-memory subset
    // deep-copied brush DEFs into their EXISTING owner entity (a cloned misc_model was a
    // second proxy brush inside the same entity — invisible and never saved) and skipped
    // fixed-size entities and patches altogether.  The user's in-app clipboard is stashed
    // and restored around the round trip (RadiantClipboard_CloneSelection).
    extern void RadiantClipboard_CloneSelection();   // entity.cpp
    RadiantClipboard_CloneSelection();
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x490A70  Select_ByClass - forward ->next walk; brushFlags &2/&0x20 gate;
//  assert-then-deref (no null guard).
// ═════════════════════════════════════════════════════════════════════════════
void Select_ByClass( const char *key )
{
    if ( selected_brushes.next == &selected_brushes )
        return;
    if ( selected_brushes.next->next != &selected_brushes )
        return;

    selbrush_t *brush = selected_brushes.next;
    entity_s *owner = brush->owner;
    if ( owner == world_entity )
        return;

    iassert( brush->owner->def == brush->def->owner );   // 1776

    char *value = ValueForKey2( owner->def, key );
    Sys_Printf( "Selecting %s %s\n", key, value );

    Select_Deselect( 1 );

    brush = active_brushes.next;
    if ( brush != &active_brushes )
    {
        do
        {
            selbrush_t *brushNext = brush->next;
            if ( !FilterBrush( brush, 0 ) )
            {
                int bf = brush->brushFlags;
                if ( (bf & 2) == 0 && (bf & 0x20) == 0 )
                {
                    iassert( brush->owner );   // 1793
                    if ( brush->owner != world_entity )
                    {
                        iassert( brush->owner->def == brush->def->owner );   // 1796
                        if ( Entity_HasEpairMatch( (entity_s *)brush->owner->def, key, value ) )
                        {
                            Brush_RemoveFromList( brush );
                            Select_Brush_2( &selected_brushes, brush );
                        }
                    }
                }
            }
            brush = brushNext;
        }
        while ( brush != &active_brushes );
    }
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x490C00  Select_ByKeyValue  — Selection→Select by Key/Value (menu/cmd 33133).
//
//  The binary constructs CKeyValueSelectDlg (IDD 0xBC, ctor 0x416760) which pre-
//  fills its key/value CString members from the entity-window key/value edit fields,
//  DoModals it, and — if OK — runs the active_brushes match-walk below.  The dialog's
//  two CString members (key/value) and two flags (key_substr/value_substr) drive an
//  8-way matcher dispatch (exact/substring × key/value × present/absent).
//
//  PORT: the deterministic match-walk is factored into Select_ByKeyValue_Core so it can
//  be driven without a message pump; the public Select_ByKeyValue() toggles the ImGui
//  panel, whose Select button calls the core (via KeyValueSelect_Apply).
//
//  The active-list walk captures `saved=b->next` BEFORE relinking, then advances b=saved.
// ═════════════════════════════════════════════════════════════════════════════

// The eight epair matchers (entity.cpp).  HasKeyValuePair / Entity_HasEpairMatch are
// already extern'd above; the six substring variants are declared here.
extern bool Entity_HasKeySubstr( entity_s_def *e, const char *key );               // 0x483900
extern bool Entity_HasValue( entity_s_def *e, const char *val );                   // 0x483AC0
extern bool Entity_HasValueSubstr( entity_s_def *e, const char *val );             // 0x483B10
extern bool Entity_HasKeySubstrValue( entity_s_def *e, const char *key, const char *val );       // 0x4839C0
extern bool Entity_HasKeyValueSubstr( entity_s_def *e, const char *key, const char *val );       // 0x483A20
extern bool Entity_HasKeySubstrValueSubstr( entity_s_def *e, const char *key, const char *val ); // 0x483A80

// ── the deterministic match-walk (Select_ByKeyValue's body after DoModal returns OK) ──
void Select_ByKeyValue_Core( const char *key, const char *value, bool keySubstr, bool valueSubstr )
{
    const bool hasKey = ( key   && key[0]   != 0 );
    const bool hasVal = ( value && value[0] != 0 );

    if ( hasKey )
    {
        if ( !hasVal )
        {
            if ( keySubstr )
                Sys_Printf( "Selecting entities that have a substring key of '%s'\n", key );
            else
                Sys_Printf( "Selecting entities that have the key '%s'\n", key );
        }
        else if ( keySubstr )
        {
            if ( valueSubstr )
                Sys_Printf( "Selecting entities that have a substring key of '%s' with a substring value of '%s'\n", key, value );
            else
                Sys_Printf( "Selecting entities that have a substring key of '%s' with the value '%s'\n", key, value );
        }
        else if ( valueSubstr )
            Sys_Printf( "Selecting entities that have the key '%s' with a substring value of '%s'\n", key, value );
        else
            Sys_Printf( "Selecting entities that have the key '%s' with the value '%s'\n", key, value );
    }
    else
    {
        if ( !hasVal )
            return;                                   // nothing entered → no-op
        if ( valueSubstr )
            Sys_Printf( "Selecting entities that have a key substring value of '%s'\n", value );
        else
            Sys_Printf( "Selecting entities that have a key value of '%s'\n", value );
    }

    Select_Deselect( 1 );

    selbrush_t *brush = active_brushes.next;
    while ( brush != &active_brushes )
    {
        selbrush_t *saved = brush->next;                  // captured pre-relink
        if ( !FilterBrush( brush, 0 )
          && ( brush->brushFlags & 2 ) == 0
          && ( brush->brushFlags & 0x20 ) == 0 )
        {
            entity_s *owner = brush->owner;
            if ( owner != world_entity )
            {
                iassert( brush->owner->def == brush->def->owner );   // select.cpp:1882

                entity_s_def *def = (entity_s_def *)owner->def;
                bool match;
                if ( hasKey )
                {
                    if ( hasVal )
                    {
                        if ( valueSubstr )
                        {
                            if ( keySubstr )
                                match = Entity_HasKeySubstrValueSubstr( def, key, value );
                            else
                                match = Entity_HasKeyValueSubstr( def, key, value );
                        }
                        else if ( keySubstr )
                            match = Entity_HasKeySubstrValue( def, key, value );
                        else
                            match = Entity_HasEpairMatch( def, key, value );               // exact
                    }
                    else if ( keySubstr )
                        match = Entity_HasKeySubstr( def, key );
                    else
                        match = HasKeyValuePair( def, key );
                }
                else if ( valueSubstr )
                    match = Entity_HasValueSubstr( def, value );
                else
                    match = Entity_HasValue( def, value );

                if ( match )
                {
                    Brush_RemoveFromList( brush );
                    Select_Brush_2( &selected_brushes, brush );   // Brush_AddToList2 into selected list
                }
            }
        }
        brush = saved;
    }

    g_nUpdateBits = -1;
}

// UI-independent action behind the Key/Value panel's Select button.
void KeyValueSelect_Apply( const char *key, const char *value, bool keySubstr, bool valueSubstr )
{
    Select_ByKeyValue_Core( key, value, keySubstr, valueSubstr );
    g_nUpdateBits |= 1;
}

// ── KeyValueSelect_Apply above is called by imgui_panel_kvselect.cpp. ─────────────────

void Select_ByKeyValue()
{
    // KIWI: the MFC popup is gone; open/close the ImGui panel (it calls KeyValueSelect_Apply)
    extern void ImGuiPanel_KVSelect_Toggle();   // imgui_panel_kvselect.cpp
    ImGuiPanel_KVSelect_Toggle();
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x490EC0  Select_Connected  (1407 bytes)
//  Walks target/targetname and script_linkTo/script_linkName epairs.
// ═════════════════════════════════════════════════════════════════════════════
void Select_Connected()
{
    if ( selected_brushes.next == &selected_brushes )
        return;

    ResetSelectMode();

    LinkList_t links;   // Map_ParseLinkList parse buffer (qe3.h)

    char added;
    do
    {
        added = 0;
        selbrush_t *brush = selected_brushes.next;
        if ( brush == &selected_brushes )
            break;

        do
        {
            entity_s *owner = brush->owner;
            if ( owner != world_entity )
            {
                iassert( brush->def->owner == brush->owner->def );   // 1969

                entity_s_def *def = (entity_s_def *)brush->owner->def;

                // 1. targetname → select entities that target this one.
                if ( HasKeyValuePair( def, "targetname" ) )
                {
                    const char *value = ValueForKey2( def, "targetname" );
                    selbrush_t *ab = active_brushes.next;
                    while ( ab != &active_brushes )
                    {
                        selbrush_t *abNext = ab->next;
                        if ( ab->owner != world_entity
                             && !FilterBrush( ab, 0 )
                             && (ab->brushFlags & 2) == 0
                             && (ab->brushFlags & 0x20) == 0
                             && Entity_HasEpairMatch( (entity_s *)ab->owner->def, "target", value ) )
                        {
                            Brush_RemoveFromList( ab );
                            Brush_AddToList2( ab );
                            added = 1;
                        }
                        ab = abNext;
                    }
                }

                // 2. script_linkTo → select entities whose script_linkName is in the list.
                if ( HasKeyValuePair( def, "script_linkTo" ) )
                {
                    Map_ParseLinkList( &links, ValueForKey2( def, "script_linkTo" ) );
                    int count = links.size;

                    selbrush_t *ab = active_brushes.next;
                    while ( ab != &active_brushes )
                    {
                        selbrush_t *abNext = ab->next;
                        if ( ab->owner != world_entity && !FilterBrush( ab, 0 )
                             && (ab->brushFlags & 2) == 0 && (ab->brushFlags & 0x20) == 0 )
                        {
                            const char *linkName = ValueForKey2( (entity_s *)ab->owner->def, "script_linkName" );
                            if ( *linkName )
                            {
                                int linkId = (int)atol( linkName );
                                for ( int si = 0; si < count; ++si )
                                {
                                    if ( linkId == links.id[si] )
                                    {
                                        Brush_RemoveFromList( ab );
                                        Brush_AddToList2( ab );
                                        added = 1;
                                        break;
                                    }
                                }
                            }
                        }
                        ab = abNext;
                    }
                }

                // 3. script_linkName → select entities whose script_linkTo contains this id.
                if ( HasKeyValuePair( def, "script_linkName" ) )
                {
                    int myId = (int)atol( ValueForKey2( def, "script_linkName" ) );

                    selbrush_t *ab = active_brushes.next;
                    while ( ab != &active_brushes )
                    {
                        selbrush_t *abNext = ab->next;
                        if ( ab->owner != world_entity && !FilterBrush( ab, 0 )
                             && (ab->brushFlags & 2) == 0 && (ab->brushFlags & 0x20) == 0 )
                        {
                            const char *linkTo = ValueForKey2( (entity_s *)ab->owner->def, "script_linkTo" );
                            if ( *linkTo )
                            {
                                Map_ParseLinkList( &links, linkTo );
                                int count2 = links.size;
                                for ( int si = 0; si < count2; ++si )
                                {
                                    if ( myId == links.id[si] )
                                    {
                                        Brush_RemoveFromList( ab );
                                        Brush_AddToList2( ab );
                                        added = 1;
                                        break;
                                    }
                                }
                            }
                        }
                        ab = abNext;
                    }
                }

                // 4. target → select entities with matching targetname.
                if ( HasKeyValuePair( def, "target" ) )
                {
                    const char *target = ValueForKey2( def, "target" );
                    selbrush_t *ab = active_brushes.next;
                    while ( ab != &active_brushes )
                    {
                        selbrush_t *abNext = ab->next;
                        if ( ab->owner != world_entity && !FilterBrush( ab, 0 )
                             && (ab->brushFlags & 2) == 0 && (ab->brushFlags & 0x20) == 0
                             && Entity_HasEpairMatch( (entity_s *)ab->owner->def, "targetname", target ) )
                        {
                            Brush_RemoveFromList( ab );
                            Brush_AddToList2( ab );
                        }
                        ab = abNext;
                    }
                }
            }
            brush = brush->next;
        }
        while ( brush != &selected_brushes );
    }
    while ( added );

    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x491440  SelectTargettedEntity  (840 bytes)
// Two 512-stride dedup loops; brushFlags gate; restart-from-head on a match.
// KIWI: the binary strcpy'd every distinct target into its 16-slot stack buffer and checked
// targets < 16 only after the loop, so 17+ distinct targets overran the stack.  It stops at 16.
// ═════════════════════════════════════════════════════════════════════════════
void SelectTargettedEntity()
{
    if ( selected_brushes.next == &selected_brushes )
        return;

    ResetSelectMode();

    enum { MAX_TARGETS = 16 };
    int         targets = 0;
    bool        dropped = false;
    char        targetNames[MAX_TARGETS][512];
    selbrush_t *brush = selected_brushes.next;

    while ( brush != &selected_brushes )
    {
        entity_s *owner = brush->owner;
        if ( owner != world_entity )
        {
            iassert( brush->def->owner == brush->owner->def );   // 2148

            entity_s_def *def = (entity_s_def *)brush->owner->def;
            if ( HasKeyValuePair( def, "target" ) )
            {
                const char *value = ValueForKey2( def, "target" );
                int found = 0;
                for ( int k = 0; k < targets; ++k )
                    if ( !strcmp( targetNames[k], value ) ) { found = 1; break; }
                if ( !found )
                {
                    if ( targets < MAX_TARGETS )
                        I_strncpyz( targetNames[targets++], value, sizeof( targetNames[0] ) );
                    else
                        dropped = true;
                }
            }
        }
        brush = brush->next;
    }
    if ( dropped )
        Sys_Printf( "Select targetted: more than %i distinct targets; using the first %i\n",
                    MAX_TARGETS, MAX_TARGETS );

    brush = active_brushes.next;
    while ( brush != &active_brushes )
    {
        selbrush_t *brushNext = brush->next;
        if ( brush->owner != world_entity && !FilterBrush( brush, 0 ) )
        {
            int bf = brush->brushFlags;
            if ( (bf & 2) == 0 && (bf & 0x20) == 0 )
            {
                iassert( brush->def->owner == brush->owner->def );   // 2189

                entity_s_def *def = (entity_s_def *)brush->owner->def;
                if ( HasKeyValuePair( def, "targetname" ) )
                {
                    const char *name = ValueForKey2( def, "targetname" );
                    for ( int k = 0; k < targets; ++k )
                    {
                        if ( !strcmp( targetNames[k], name ) )
                        {
                            Brush_RemoveFromList( brush );
                            Brush_AddToList2( brush );
                            // Restart the outer loop.
                            brush = active_brushes.next;
                            goto NEXT_ITER;
                        }
                    }
                }
            }
        }
        brush = brushNext;
        NEXT_ITER:;
    }

    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x491790  Select_ChangeBrushType  (253 bytes)
//  The def->contents RMW lives INSIDE the faceCount-gated face loop and the patch
//  contents write targets i->patch->def->contents.
// ═════════════════════════════════════════════════════════════════════════════
void Select_ChangeBrushType( int contents, int clearBits )
{
    int mask = ~clearBits;
    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes;
          b = b->next )
    {
        for ( int fi = 0; fi < b->faceCount; ++fi )
        {
            b->def->faces[fi].contents &= mask;
            b->def->faces[fi].contents |= contents;
            // def->contents RMW is INSIDE the face loop — redundant per face (idempotent) but
            // SKIPPED entirely for faceCount==0 (patch) brushes, whose authoritative contents
            // live in patchMesh_t and which the binary leaves untouched here.
            b->def->contents &= mask;
            b->def->contents |= contents;
        }

        patch_t *patch = (patch_t *)b->patch;
        if ( patch )
        {
            iassert( b->patch->def == b->def->patch );   // select.cpp:2237
            patch->def->contents &= mask;   // b->patch->def->contents (instance patch)
            patch->def->contents |= contents;
        }
    }
    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes;
          b = b->next )
    {
        b->xx6 = 0;
    }
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x491890  Select_ChangeBrushToolflags  (200 bytes)
//  The patch flags write targets b->patch->def->flags (the instance patch).
// ═════════════════════════════════════════════════════════════════════════════
void Select_ChangeBrushToolflags( int toolFlags, int clearBits )
{
    int mask = ~clearBits;
    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes;
          b = b->next )
    {
        for ( int fi = 0; fi < b->faceCount; ++fi )
        {
            b->def->faces[fi].toolflags &= mask;
            b->def->faces[fi].toolflags |= toolFlags;
        }
        patch_t *patch = (patch_t *)b->patch;
        if ( patch )
        {
            iassert( b->patch->def == b->def->patch );   // select.cpp:2265
            patch->def->flags &= mask;   // b->patch->def->flags (instance patch)
            patch->def->flags |= toolFlags;
        }
    }
    g_nUpdateBits = -1;
}

// A face material's current-layer texdef (the texture ops below; drag.cpp's texture pick).
texdef_sub_t *Face_CurrentTexdef( MaterialDef *md )
{
    return &md->mat_texDef + LayerMat::GetCurrentLayer( md );
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x491F20  Brush_ShiftTexture  (850 bytes)
//  Non-patch: (int)s is a TRUNCATE.  Patch: s*0.001f.  The base shift comes from the
//  current-layer texdef slot.  The version asserts compare instance vs def (both 16-bit).
// ═════════════════════════════════════════════════════════════════════════════
void Brush_ShiftTexture( float s, float t )
{
    unsigned faceCount = (unsigned)g_SelectedFaces.GetSize();

    if ( selected_brushes.next == &selected_brushes && !faceCount )
        return;

    selbrush_t *b = selected_brushes.next;
    if ( b != &selected_brushes )
    {
        do
        {
            float ds, dt;
            if ( b->patch )
            {
                ds = s * 0.001f;
                dt = t * 0.001f;
            }
            else
            {
                ds = (float)(int)s;
                dt = (float)(int)t;
            }

            for ( int fi = 0; fi < b->faceCount; ++fi )
            {
                MaterialDef  *md = &b->def->faces[fi].mtldef[g_qeglobals.current_edit_layer];
                texdef_sub_t *td = Face_CurrentTexdef( md );
                // the base shift is read from the SAME (current-layer) texdef_sub_t slot, not
                // from sub-layer 0.
                td->shift[0] = td->shift[0] + ds;
                td->shift[1] = td->shift[1] + dt;
                TexMatToFakeTexCoords( md, td );
            }

            Brush_RebuildAfterEdit( b->def );

            patch_t *patch = b->patch;
            if ( patch )
            {
                iassert( b->patch->def == b->def->patch );   // select.cpp:2460
                Patch_ShiftTexture( patch->def, ds, dt );   // the patch DEF, not the instance
            }

            b = b->next;
        }
        while ( b != &selected_brushes );
    }

    for ( unsigned i = 0; i < faceCount; ++i )
    {
        selface_t  &selFace = g_SelectedFaces.GetAt( i );   // CArray::GetAt (bounds ENSURE)
        int         index = selFace.index;
        selbrush_t *fb    = selFace.brush;

        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:2470
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:2471

        MaterialDef  *md = &fb->def->faces[index].mtldef[g_qeglobals.current_edit_layer];
        texdef_sub_t *td = Face_CurrentTexdef( md );
        td->shift[0] = td->shift[0] + s;
        td->shift[1] = td->shift[1] + t;
        TexMatToFakeTexCoords( md, td );

        Brush_RebuildAfterEdit( fb->def );
        Brush_CheckBuildFaceVis( fb, (const float *)world_orient_matrix );

        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:2482
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:2483
    }
    g_nUpdateBits |= 1u;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x492280  Brush_FlipTexture  (967 bytes)
//  size[axis] = -size[axis], shift[axis] = 1-shift[axis]; Patch_FlipTexture takes the patch DEF.
// ═════════════════════════════════════════════════════════════════════════════
void Brush_FlipTexture( int axis )
{
    if ( selected_brushes.next == &selected_brushes
         && !g_SelectedFaces.GetSize() )
        return;

    iassert( axis == 0 || axis == 1 );

    Undo_ClearRedo();
    Undo_GeneralStart( axis ? "flip texture y" : "flip texture x" );
    Undo_AddBrushList( &selected_brushes );

    selbrush_t *b = selected_brushes.next;
    while ( b != &selected_brushes )
    {
        for ( int fi = 0; fi < b->faceCount; ++fi )
        {
            MaterialDef  *md = &b->def->faces[fi].mtldef[g_qeglobals.current_edit_layer];
            texdef_sub_t *td = Face_CurrentTexdef( md );
            td->size[axis]  = -td->size[axis];
            td->shift[axis] = 1.0f - td->shift[axis];
            TexMatToFakeTexCoords( md, td );
        }
        Brush_RebuildAfterEdit( b->def );

        patch_t *patch = (patch_t *)b->patch;
        if ( patch )
        {
            // pass the patch DEF (b->patch->def), NOT the instance, after asserting the
            // instance/def link.
            iassert( b->patch->def == b->def->patch );   // select.cpp:2524
            Patch_FlipTexture( b->patch->def, axis );
        }

        b = b->next;
    }

    Undo_EndBrushList( &selected_brushes );

    unsigned faceCount = (unsigned)g_SelectedFaces.GetSize();
    for ( unsigned i = 0; i < faceCount; ++i )
    {
        selface_t  &selFace = g_SelectedFaces.GetAt( i );   // CArray::GetAt (bounds ENSURE)
        selbrush_t *fb    = selFace.brush;
        int         index = selFace.index;

        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:2534
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:2535

        Undo_TryAddBrush( fb->def );

        MaterialDef  *md = &fb->def->faces[index].mtldef[g_qeglobals.current_edit_layer];
        texdef_sub_t *td = Face_CurrentTexdef( md );
        td->size[axis]  = -td->size[axis];
        td->shift[axis] = 1.0f - td->shift[axis];
        TexMatToFakeTexCoords( md, td );

        Brush_RebuildAfterEdit( fb->def );
        Undo_LinkBrush( fb->def );
        Brush_CheckBuildFaceVis( fb, (const float *)world_orient_matrix );

        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:2549
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:2550
    }

    Undo_End();
    g_nUpdateBits |= 1u;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x492650  Brush_ScaleTexture  (918 bytes)
//  (float)ds == (double)ds for the small int ds here (no precision divergence);
//  Patch_ScaleTexture takes the patch DEF.
// ═════════════════════════════════════════════════════════════════════════════
void Brush_ScaleTexture( int ds, int dt )
{
    if ( selected_brushes.next == &selected_brushes
         && !g_SelectedFaces.GetSize() )
        return;

    Undo_ClearRedo();
    Undo_GeneralStart( "scale texture" );
    Undo_AddBrushList( &selected_brushes );

    selbrush_t *b = selected_brushes.next;
    while ( b != &selected_brushes )
    {
        for ( int fi = 0; fi < b->faceCount; ++fi )
        {
            MaterialDef  *md = &b->def->faces[fi].mtldef[g_qeglobals.current_edit_layer];
            texdef_sub_t *td = Face_CurrentTexdef( md );
            td->size[0]      = (float)ds + td->size[0];
            td->size[1]      = (float)dt + td->size[1];
            TexMatToFakeTexCoords( md, td );
        }
        Brush_RebuildAfterEdit( b->def );

        patch_t *patch = b->patch;
        if ( patch )
        {
            iassert( b->patch->def == b->def->patch );   // select.cpp:2589
            Patch_ScaleTexture( patch->def, (float)ds, (float)dt );   // the patch DEF
        }

        b = b->next;
    }

    Undo_EndBrushList( &selected_brushes );

    unsigned faceCount = (unsigned)g_SelectedFaces.GetSize();
    for ( unsigned i = 0; i < faceCount; ++i )
    {
        selface_t  &selFace = g_SelectedFaces.GetAt( i );   // CArray::GetAt (bounds ENSURE)
        int         index = selFace.index;
        selbrush_t *fb    = selFace.brush;

        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:2599
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:2600

        Undo_TryAddBrush( fb->def );

        MaterialDef  *md = &fb->def->faces[index].mtldef[g_qeglobals.current_edit_layer];
        texdef_sub_t *td = Face_CurrentTexdef( md );
        td->size[0]      = (float)ds + td->size[0];
        td->size[1]      = (float)dt + td->size[1];
        TexMatToFakeTexCoords( md, td );

        Brush_RebuildAfterEdit( fb->def );
        Undo_LinkBrush( fb->def );
        Brush_CheckBuildFaceVis( fb, (const float *)world_orient_matrix );

        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:2613
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:2614
    }

    Undo_End();
    g_nUpdateBits |= 1u;
}

// The binary's new texture angle: (fistp)( sum + 2^-30 ) % 360, whole degrees.  KIWI (2026-10-05,
// reversibility): an angle that is not whole within noise keeps its fraction (fmod, the same
// sign) - a rotate / mirror texture lock leaves e.g. 37.21, which +90 then -90 rounded to 37.
static float Ed_TexRotateSum( float sum )
{
    const double whole = (double)KiwiRound_Fistp( sum );
    if ( fabs( (double)sum - whole ) <= 1.0e-4 )
        return (float)( (int)whole % 360 );
    return (float)fmod( (double)sum, 360.0 );
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x4929F0  Brush_RotateTexture  (1008 bytes)
//  New angle via Ed_TexRotateSum; Patch_RotateTexture takes the patch DEF.
void Brush_RotateTexture( int deg )
{
    unsigned faceCount = (unsigned)g_SelectedFaces.GetSize();

    if ( selected_brushes.next == &selected_brushes && !faceCount )
        return;

    Undo_ClearRedo();
    Undo_GeneralStart( "rotate texture" );
    Undo_AddBrushList( &selected_brushes );

    selbrush_t *b = selected_brushes.next;
    while ( b != &selected_brushes )
    {
        for ( int fi = 0; fi < b->faceCount; ++fi )
        {
            MaterialDef  *md  = &b->def->faces[fi].mtldef[g_qeglobals.current_edit_layer];
            texdef_sub_t *td  = Face_CurrentTexdef( md );
            float         sum = (float)deg + td->rotate;
            td->rotate = Ed_TexRotateSum( sum );   // KIWI: reversible (see Ed_TexRotateSum)
            TexMatToFakeTexCoords( md, td );
        }
        Brush_RebuildAfterEdit( b->def );

        patch_t *patch = b->patch;
        if ( patch )
        {
            iassert( b->patch->def == b->def->patch );   // select.cpp:2652
            Patch_RotateTexture( patch->def, (float)deg );   // the patch DEF
        }

        b = b->next;
    }

    Undo_EndBrushList( &selected_brushes );

    for ( unsigned i = 0; i < faceCount; ++i )
    {
        selface_t  &selFace = g_SelectedFaces.GetAt( i );   // CArray::GetAt (bounds ENSURE)
        int         index = selFace.index;
        selbrush_t *fb    = selFace.brush;

        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:2661
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:2662

        Undo_TryAddBrush( fb->def );

        MaterialDef  *md  = &fb->def->faces[index].mtldef[g_qeglobals.current_edit_layer];
        texdef_sub_t *td  = Face_CurrentTexdef( md );
        float         sum = (float)deg + td->rotate;
        td->rotate = Ed_TexRotateSum( sum );    // KIWI: reversible (see Ed_TexRotateSum)
        TexMatToFakeTexCoords( md, td );

        Brush_RebuildAfterEdit( fb->def );
        Undo_LinkBrush( fb->def );
        Brush_CheckBuildFaceVis( fb, (const float *)world_orient_matrix );

        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:2675
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:2676
    }

    Undo_End();
    g_nUpdateBits |= W_CAMERA;
}

// ─── prefab_s (qe3.h) alias.  entity_s.prefab points here; the prefab's instanced brush
//     list lives at active_brushlist (tail) / active_brushlist_next (head).
typedef prefab_s prefab_s_select;

// ═════════════════════════════════════════════════════════════════════════════
//  0x493210  Prefab_HasMaterial  — recursive prefab/patch texture-name search.
//
//  Walks b's owning entity's PREFAB brush list (b->owner->prefab : prefab_s, whose
//  brush sentinel is &active_brushlist with the first node in active_brushlist_next).  For each
//  unfiltered brush: a patch → compare the patch's per-layer material name; a nested
//  prefab → recurse; otherwise → loop the def faces comparing the per-layer material
//  name.  Returns 1 the moment a name matches `name` (caller selects the whole top-level
//  brush), 0 if nothing in the subtree matches.  Match uses Materialdef_GetName
//  (lyrMtl ? (char*)lyrMtl : radMtl->name) + _stricmp.
// ═════════════════════════════════════════════════════════════════════════════
int Prefab_HasMaterial( selbrush_t *b, const char *name )
{
    prefab_s_select *prefab = (prefab_s_select *)b->owner->prefab;

    selbrush_t *sentinel = (selbrush_t *)&prefab->active_brushlist;
    selbrush_t *pfb = prefab->active_brushlist_next;
    if ( pfb == sentinel )
        return 0;

    while ( 1 )
    {
        if ( !FilterBrush( pfb, 0 ) )
        {
            patch_t *patch = pfb->patch;
            if ( patch )
            {
                // patch path
                iassert( pfb->patch->def == pfb->def->patch );   // select.cpp:2799
                MaterialDef *md =
                    (MaterialDef *)( &pfb->patch->def->texture + g_qeglobals.current_edit_layer );
                // (the MaterialDef.cpp:85 assert lives in Materialdef_GetName)
                const char *nm = (const char *)Materialdef_GetName( md );
                if ( !_stricmp( name, nm ) )
                    return 1;
            }
            else if ( pfb->owner->prefab )
            {
                // nested prefab → recurse
                if ( Prefab_HasMaterial( pfb, name ) )
                    return 1;
            }
            else
            {
                // face loop
                brush_t *def = pfb->def;
                if ( def->faceCount )
                {
                    unsigned int i = 0;
                    while ( 1 )
                    {
                        MaterialDef *md = &def->faces[i].mtldef[g_qeglobals.current_edit_layer];
                        // (the MaterialDef.cpp:85 assert lives in Materialdef_GetName)
                        const char *nm = (const char *)Materialdef_GetName( md );
                        if ( !_stricmp( name, nm ) )
                            return 1;
                        def = pfb->def;            // reload
                        if ( ++i >= (unsigned int)def->faceCount )
                            break;               // → next brush
                    }
                }
            }
        }
        pfb = pfb->next;
        if ( pfb == sentinel )
            return 0;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x4934F0  Select_ByTexture  — select every active brush/patch whose current-layer
//  material matches the "source" material name.
//
//  Source name: if a face is selected, it's that face's per-layer material
//  (selFace[0].brush->def->faces[index].mtldef[layer]); otherwise the texture window's
//  current-layer template (g_qeglobals.random_texture_stuff[layer].mtl).  Then walk
//  active_brushes: filtered / brushFlags &2 / &0x20 skipped; a patch compares its
//  per-layer material name (match → Brush_RemoveFromList + Select_Brush_2 to selected);
//  a prefab owner (when includePrefabs) recurses via Prefab_HasMaterial (match → select the whole
//  brush); a normal brush loops its faces (match → Brush_RemoveFromList + Brush_AddToList2).
//
//  A patch match uses Select_Brush_2(&selected_brushes, ...); a face match uses
//  Brush_AddToList2 - both as in the binary.
// ═════════════════════════════════════════════════════════════════════════════
void Select_ByTexture( int includePrefabs )
{
    MaterialDef *src;

    if ( g_SelectedFaces.GetSize() )
    {
        selface_t  &selFace = g_SelectedFaces.GetAt( 0 );
        int         index = selFace.index;
        selbrush_t *brush = selFace.brush;
        iassert( selFace.face == &selFace.brush->faces[selFace.index] );    // select.cpp:2842
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:2843
        src = &brush->def->faces[index].mtldef[g_qeglobals.current_edit_layer];
    }
    else
    {
        src = &g_qeglobals.random_texture_stuff[g_qeglobals.current_edit_layer].mtl;
    }

    const char *Name = (const char *)Materialdef_GetName( src );
    Select_Deselect( 1 );

    selbrush_t *b = active_brushes.next;
    while ( b != &active_brushes )
    {
        selbrush_t *bNext = b->next;
        if ( !FilterBrush( b, 0 ) )
        {
            int bf = b->brushFlags;
            if ( (bf & 2) == 0 && (bf & 0x20) == 0 )
            {
                patch_t *patch = b->patch;
                if ( patch )
                {
                    // patch path
                    iassert( b->patch->def == b->def->patch );   // select.cpp:2861
                    MaterialDef *md =
                        (MaterialDef *)( &b->patch->def->texture + g_qeglobals.current_edit_layer );
                    const char *nm = (const char *)Materialdef_GetName( md );
                    if ( !_stricmp( Name, nm ) )
                    {
                        Brush_RemoveFromList( b );
                        Select_Brush_2( &selected_brushes, b );
                    }
                }
                else if ( b->owner->prefab && includePrefabs )
                {
                    // prefab owner → recurse; match selects the whole top-level brush
                    if ( Prefab_HasMaterial( b, Name ) )
                    {
                        Brush_RemoveFromList( b );
                        Select_Brush_2( &selected_brushes, b );
                    }
                }
                else
                {
                    // face loop
                    brush_t *def = b->def;
                    if ( def->faceCount )
                    {
                        unsigned int i = 0;
                        while ( 1 )
                        {
                            MaterialDef *md = &def->faces[i].mtldef[g_qeglobals.current_edit_layer];
                            // (the MaterialDef.cpp:85 assert lives in Materialdef_GetName)
                            const char *nm = (const char *)Materialdef_GetName( md );
                            if ( !_stricmp( Name, nm ) )
                            {
                                Brush_RemoveFromList( b );
                                if ( b->next || b->prev )
                                    Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
                                Brush_AddToList2( b );
                                break;           // → next brush
                            }
                            if ( ++i >= (unsigned int)b->def->faceCount )
                                break;           // → next brush
                        }
                    }
                }
            }
        }
        b = bNext;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x493830  Select_ByClassSimilar  (368 bytes)
//  Selects all brushes sharing the same entity class as any
//  currently-selected brush.  Iterates selected list in reverse (prev chain).
//  The _stricmp deref is unconditional (no owner guard).
// ═════════════════════════════════════════════════════════════════════════════
void Select_ByClassSimilar()
{
    // Start at selected_brushes.PREV (the list TAIL), then walk ->prev to the head.
    // Starting at .next would break after the first node (head->prev == sentinel) and
    // match only the head brush's class.
    selbrush_t *brush = selected_brushes.prev;

    if ( brush == &selected_brushes )
    {
        g_nUpdateBits = -1;
        return;
    }

    // Iterate selected list in reverse.
    while ( 1 )
    {
        entity_s *owner = brush->owner;
        if ( owner != world_entity )
        {
            iassert( brush->owner->def == brush->def->owner );   // 2909
            const char *name = ((entity_s_def *)owner->def)->eclass->name;
            selbrush_t *b    = active_brushes.next;

            while ( b != &active_brushes )
            {
                selbrush_t *bnext = b->next;
                if ( !FilterBrush( b, 0 ) )
                {
                    int bf = b->brushFlags;
                    if ( (bf & 2) == 0 && (bf & 0x20) == 0 )
                    {
                        iassert( b->owner );                          // 2923
                        iassert( b->owner->def == b->def->owner );    // 2924
                        if ( !_stricmp( ((entity_s_def *)b->owner->def)->eclass->name, name ) )
                        {
                            Brush_RemoveFromList( b );
                            Brush_AddToList2( b );
                        }
                    }
                }
                b = bnext;
            }
        }

        if ( brush->prev == &selected_brushes )
            break;
        brush = brush->prev;
    }
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x4939E0  Brush_FitTexture  (721 bytes)
// ═════════════════════════════════════════════════════════════════════════════
void Brush_FitTexture( float x, float y, int acrossSelection )
{
    int faceCount = g_SelectedFaces.GetSize();

    if ( selected_brushes.next == &selected_brushes && !faceCount )
        return;

    Undo_ClearRedo();
    Undo_GeneralStart( "Fit texture" );
    Undo_AddBrushList( &selected_brushes );

    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes;
          b = b->next )
    {
        Brush_FitTextureFaces( b->def, y, x );
        Brush_RebuildAfterEdit( b->def );
    }

    for ( int i = 0; i < faceCount; ++i )
    {
        selface_t  &selFace = g_SelectedFaces.GetAt( i );   // CArray::GetAt (bounds ENSURE)
        int         index = selFace.index;
        selbrush_t *b     = selFace.brush;

        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:2973
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:2974

        Undo_TryAddBrush( b->def );
        Texture_Fit( &b->def->faces[index], y, x, acrossSelection );
        Brush_RebuildAfterEdit( b->def );
        Undo_LinkBrush( b->def );
        Brush_CheckBuildFaceVis( b, (const float *)world_orient_matrix );

        iassert( selFace.face == &selFace.brush->faces[selFace.index] );   // select.cpp:2982
        iassert( selFace.brush->version == selFace.brush->def->version );   // select.cpp:2983
    }

    Undo_EndBrushList( &selected_brushes );
    Undo_End();
    g_nUpdateBits |= 1u;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x494210  SelectNext  (326 bytes)
//  The owner->def deref is unconditional (assert, then deref).
// ═════════════════════════════════════════════════════════════════════════════
void SelectNext()
{
    if ( g_qeglobals.d_select_count != 1 )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Must have 1 entity selected!" );
        MessageBeep( 0x40u );
        return;
    }

    iassert( g_qeglobals.d_select_order[0] );                 // 3195
    iassert( g_qeglobals.d_select_order[0]->owner );          // 3196
    iassert( g_qeglobals.d_select_order[0]->owner->def );     // 3197

    // d_select_order[0]->owner is the selbrush's entity INSTANCE; ->def is the DEF the
    // epair lookups read (assert 3197 guards it non-null).
    entity_s_def *def = (entity_s_def *)g_qeglobals.d_select_order[0]->owner->def;
    if ( !HasKeyValuePair( def, "target" ) )
        return;

    char *target = ValueForKey2( def, "target" );
    selbrush_t *b = active_brushes.next;
    while ( b != &active_brushes )
    {
        iassert( b->owner );   // 3205 (asserts, then derefs UNCONDITIONALLY)
        if ( Entity_HasEpairMatch( (entity_s *)b->owner->def, "targetname", target ) )
        {
            Select_Deselect( 1 );
            Select_Brush( b, 1, 1, 0 );
            return;
        }
        b = b->next;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x494360  SelectPrev  (326 bytes)
//  Mirror of SelectNext (the same unconditional deref).
// ═════════════════════════════════════════════════════════════════════════════
void SelectPrev()
{
    if ( g_qeglobals.d_select_count != 1 )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Must have 1 entity selected!" );
        MessageBeep( 0x40u );
        return;
    }

    iassert( g_qeglobals.d_select_order[0] );                 // 3230
    iassert( g_qeglobals.d_select_order[0]->owner );          // 3231
    iassert( g_qeglobals.d_select_order[0]->owner->def );     // 3232

    // d_select_order[0]->owner is the entity INSTANCE; ->def is the DEF the epair lookups
    // read (assert 3232 guards it non-null).
    entity_s_def *def = (entity_s_def *)g_qeglobals.d_select_order[0]->owner->def;
    if ( !HasKeyValuePair( def, "targetname" ) )
        return;

    char *targetName = ValueForKey2( def, "targetname" );
    selbrush_t *b = active_brushes.next;
    while ( b != &active_brushes )
    {
        iassert( b->owner );   // 3240 (asserts, then derefs UNCONDITIONALLY)
        if ( Entity_HasEpairMatch( (entity_s *)b->owner->def, "target", targetName ) )
        {
            Select_Deselect( 1 );
            Select_Brush( b, 1, 1, 0 );
            return;
        }
        b = b->next;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48C530  ConnectEntities_R — link the two selected entities (target/targetname).
//
//  Selection→Connect Entities (and the alt+shift+MMB clone-and-connect tail in
//  CXYWnd::XY_MouseDown).  Requires exactly two selected brushes from two different
//  non-world entities.  The FIRST (d_select_order[0]) gets "target" = name; the SECOND
//  (d_select_order[1]) gets "targetname" = name.  The name is the source's existing
//  "target", else the dest's existing "targetname", else a fresh "auto%i" computed by
//  scanning every entity DEF's "targetname" for the max "auto<N>" suffix.  If the source
//  is a light (eclass classtype bit 0), default exponent/fov_inner/fov_outer epairs are
//  seeded when absent.  Finally the second brush is re-selected (the first deselected
//  unless Prefs→linking_keeps_selection).
//
//  atol(targetname+4) skips the "auto" prefix (4 chars) to read the numeric suffix (a
//  quirk kept).  entities list = the entity-DEF sentinel; d_select_order[i]->owner->def
//  are the two entity defs.
// ═════════════════════════════════════════════════════════════════════════════

void ConnectEntities_R()
{
    if ( g_qeglobals.d_select_count != 2 )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Must have two brushes selected." );
        Sys_Printf( "Must have two brushes selected.\n" );
        MessageBeep( 0x40u );
        return;
    }

    entity_s *owner0 = g_qeglobals.d_select_order[0]->owner;
    entity_s *owner1 = g_qeglobals.d_select_order[1]->owner;
    if ( owner0 == world_entity || owner1 == world_entity )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Can't connect to the world." );
        Sys_Printf( "Can't connect to the world.\n" );
        MessageBeep( 0x40u );
        return;
    }

    entity_s_def *dest = (entity_s_def *)owner1->def;   // gets "targetname"
    entity_s_def *src  = (entity_s_def *)owner0->def;   // gets "target"
    if ( src == dest )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Brushes are from same entity." );
        MessageBeep( 0x40u );
        return;
    }

    char name[1028];
    char *existing = ValueForKey2( src, "target" );
    if ( ( existing && *existing )
      || ( ( existing = ValueForKey2( dest, "targetname" ) ) != nullptr && *existing ) )
    {
        // Reuse an existing link name (source's target, else dest's targetname).
        strncpy( name, existing, 1024 );
        name[1024] = 0;
    }
    else
    {
        // Generate a fresh "auto<N>" name: N = max existing "auto<num>" suffix + 1.
        int maxNum = 0;
        for ( entity_s *e = entities.next; e != &entities; e = e->next )
        {
            char *tn = ValueForKey2( e, "targetname" );
            if ( tn && strlen( tn ) >= 4 )   // KIWI: a 1-3 char name read past its end
            {
                int n = atol( tn + 4 );   // skip the "auto" prefix
                if ( n > maxNum )
                    maxNum = n;
            }
        }
        sprintf( name, "auto%i", maxNum + 1 );
    }

    SetKeyValue( src, "target", name );
    if ( ( src->eclass->classtype & 1 ) != 0 )   // light: seed default cone epairs
    {
        if ( !HasKeyValuePair( src, "exponent" ) )
            SetKeyValue( src, "exponent", "1" );
        if ( !HasKeyValuePair( src, "fov_inner" ) )
            SetKeyValue( src, "fov_inner", "60" );
        if ( !HasKeyValuePair( src, "fov_outer" ) )
            SetKeyValue( src, "fov_outer", "90" );
    }
    SetKeyValue( dest, "targetname", name );
    g_nUpdateBits |= 3;

    if ( !g_PrefsDlg->linking_keeps_selection )
        Select_Deselect( 1 );
    Select_Brush( (selbrush_t *)g_qeglobals.d_select_order[1], 1, 1, 0 );
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48EB30  Select_InsertMidpointEntity — the NON-PATCH arm of
//  Patch→Insert→Add Terrain Row/Column (CMainFrame::OnAddTerrainRowColumn 0x42B080,
//  cmd 33153 / Shift+Ctrl+A).  With exactly TWO selected fixed-size entities of the
//  SAME eclass that are already linked target↔targetname, it CLONES the source entity
//  onto the midpoint of the pair and re-links the chain src → clone → dest, leaving the
//  clone selected.  (That is how a terrain/path row is grown one node at a time when no
//  patch is selected; the patch arm is Patch_InsertRemoveFromVertPair.)
//
//  Four reject paths (count != 2 / world entity / different eclass / not fixedsize),
//  each with its status-bar + console string and MessageBeep(0x40).  `eclass->fixedsize`
//  is read as a bool here (the binary read a DWORD; the same result).
//
//  When NEITHER link direction matches, the binary dereferenced two NULL brush pointers
//  (an original crash).  The port returns instead.
// ═════════════════════════════════════════════════════════════════════════════
extern void Sys_Status( const char *psz );            // win_qe3.cpp 0x499B90

// 0x409F20 — midpoint of two vec3 (also file-local in brush.cpp).
static void Select_BoundsMidpoint( const float *a, const float *b, float *out )
{
    out[0] = ( a[0] + b[0] ) * 0.5f;
    out[1] = ( a[1] + b[1] ) * 0.5f;
    out[2] = 0.5f * ( a[2] + b[2] );
}

void Select_InsertMidpointEntity()
{
    if ( g_qeglobals.d_select_count != 2 )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Must have two brushes selected." );
        Sys_Printf( "Must have two brushes selected.\n" );
        MessageBeep( 0x40u );
        return;
    }

    entity_s *owner0 = g_qeglobals.d_select_order[0]->owner;
    entity_s *owner1 = g_qeglobals.d_select_order[1]->owner;
    if ( owner0 == world_entity || owner1 == world_entity )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Can't connect to the world." );
        Sys_Printf( "Can't connect to the world.\n" );
        MessageBeep( 0x40u );
        return;
    }

    eclass_t *eclass = ( (entity_s_def *)owner0->def )->eclass;
    if ( eclass != ( (entity_s_def *)owner1->def )->eclass )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Brushes are not the same type." );
        Sys_Printf( "Brushes are not the same type.\n" );
        MessageBeep( 0x40u );
        return;
    }
    if ( !eclass->fixedsize )
    {
        Sys_Status( "Brushes must be fixed size entities." );
        Sys_Printf( "Brushes must be fixed size entities.\n" );
        MessageBeep( 0x40u );
        return;
    }

    // Which of the two selected brushes owns the "target" end of the link?
    selbrush_t *first  = selected_brushes.next;
    selbrush_t *second = selected_brushes.next->next;
    selbrush_t *src  = nullptr;   // the entity that gets cloned (link SOURCE)
    selbrush_t *dest = nullptr;   // the far end of the link
    {
        char *tn = ValueForKey2( second->owner->def, "targetname" );
        if ( !strcmp( ValueForKey2( first->owner->def, "target" ), tn ) )
        {
            src  = first;
            dest = second;
        }
        else
        {
            char *tg = ValueForKey2( second->owner->def, "target" );
            if ( !strcmp( ValueForKey2( first->owner->def, "targetname" ), tg ) )
            {
                dest = first;
                src  = second;
            }
        }
    }
    if ( !src || !dest )
    {
        // See the note above - the binary walks off a NULL here.
        Sys_Printf( "The two selected entities are not linked to each other.\n" );
        return;
    }

    ResetSelectMode();                                       // 0x48CDF0

    const float *origin = ( (entity_s_def *)src->owner->def )->origin;
    float mid[3];
    Select_BoundsMidpoint( origin, ( (entity_s_def *)dest->owner->def )->origin, mid );
    const float srcOrg[3] = { origin[0], origin[1], origin[2] };

    Select_Deselect( 1 );
    Brush_RemoveFromList( src );
    Select_Brush_2( &selected_brushes, src );
    Clone_Selection( 0.0f );

    const float move[3] = { mid[0] - srcOrg[0], mid[1] - srcOrg[1], mid[2] - srcOrg[2] };
    Select_Move( move, 0 );

    selbrush_t *clone = selected_brushes.next;

    Select_Deselect( 1 );
    Select_Brush( src,   0, 0, 0 );
    Select_Brush( clone, 0, 0, 0 );
    ConnectEntities_R();

    Select_Deselect( 1 );
    Select_Brush( clone, 0, 0, 0 );
    Select_Brush( dest,  0, 0, 0 );
    ConnectEntities_R();

    Select_Deselect( 1 );
    Select_Brush( clone, 0, 0, 0 );
    g_nUpdateBits = -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48C3D0  SetViewToEntity — snap the 3D camera onto the single selected entity
//  and aim it at that entity's "target".  Selection→"Set view to entity" (F6).
//
//  Requires exactly one selected brush from a non-world entity.  Reads the entity's
//  "target" key; walks the entity-DEF list for the entity whose "targetname" matches;
//  moves the camera to the source origin and points it along (dest - src) via
//  vectoangles (pitch is negated to the camera's convention).
//  entities list = entity-DEF sentinel; d_select_order[0]->owner->def is the selected
//  entity's def.
// ═════════════════════════════════════════════════════════════════════════════
extern void Radiant_VecToAngles( float *ang, const float *vec );   // engine_stubs.cpp 0x4A5020

void SetViewToEntity()
{
    if ( g_qeglobals.d_select_count != 1 )
        return;

    entity_s *owner = g_qeglobals.d_select_order[0]->owner;
    if ( owner == world_entity )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Can't connect to the world." );
        Sys_Printf( "Can't connect to the world.\n" );
        MessageBeep( 0x40u );
        return;
    }

    entity_s_def *src = (entity_s_def *)owner->def;
    char *target = ValueForKey2( src, "target" );
    if ( !target || !*target )
        return;

    for ( entity_s *e = entities.next; e != &entities; e = e->next )
    {
        if ( !strcmp( target, ValueForKey2( e, "targetname" ) ) )
        {
            camera_s *cam = Ed_Camera();
            cam->origin[0] = src->origin[0];
            cam->origin[1] = src->origin[1];
            cam->origin[2] = src->origin[2];

            float vec[3];
            vec[0] = e->origin[0] - src->origin[0];
            vec[1] = e->origin[1] - src->origin[1];
            vec[2] = e->origin[2] - src->origin[2];

            float ang[3];
            Radiant_VecToAngles( ang, vec );
            g_nUpdateBits |= 3u;
            cam->angles[0] = ang[0];
            cam->angles[1] = ang[1];
            cam->angles[2] = ang[2];
            cam->angles[0] = cam->angles[0] * -1.0f;
            return;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  HIDE / SHOW workflow.  Each brush node carries a hidden bit (brushFlags & 4) plus a
//  "hide depth" counter (xx5) so Show-Last-Hidden can peel one hide level at a time.
//    Select_Hide           0x493D50 — hide the SELECTED brushes (deepen depth on all)
//    Select_HideUnselected 0x493DD0 — hide the ACTIVE (unselected) brushes
//    ShowHidden            0x493E50 — clear all hidden bits + depth
//    ShowLastHidden        0x493EA0 — peel one hide level; unhide when depth hits 0
// ═════════════════════════════════════════════════════════════════════════════
void Select_Hide()
{
    if ( selected_brushes.next == &selected_brushes )
        return;

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        if ( b->xx5 )
        {
            b->brushFlags |= 4u;
            b->xx5 = b->xx5 + 1;
        }
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
        if ( b->xx5 )
        {
            b->brushFlags |= 4u;
            b->xx5 = b->xx5 + 1;
        }
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        b->brushFlags |= 4u;
        b->xx5 = 1;
    }
    g_nUpdateBits = -1;
    KiwiLightCache_VisibilityChanged(); // KIWI-UX: Hidden casters must leave cached per-light lists.
}

void Select_HideUnselected()
{
    if ( selected_brushes.next == &selected_brushes )
        return;

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        if ( b->xx5 )
        {
            b->brushFlags |= 4u;
            b->xx5 = b->xx5 + 1;
        }
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
        if ( b->xx5 )
        {
            b->brushFlags |= 4u;
            b->xx5 = b->xx5 + 1;
        }
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
    {
        b->brushFlags |= 4u;
        b->xx5 = 1;
    }
    g_nUpdateBits = -1;
    KiwiLightCache_VisibilityChanged(); // KIWI-UX: Isolating changes the faithful caster set.
}

// 0x48EF40  Select_HideUnselected2_unused — "hide by classname" (Shift+Alt+Ctrl+H).
// Same first two depth-deepening passes as Select_HideUnselected, but the third pass only
// hides the ACTIVE brushes whose owning entity has the SAME eclass as the first selected
// brush's.  The name carries "_unused" because no MENU item reaches it — the keyboard
// table does (HideByClassname, cmd 32925).
void Select_HideUnselected2_unused()
{
    if ( selected_brushes.next == &selected_brushes )
        return;

    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        if ( b->xx5 )
        {
            b->brushFlags |= 4u;
            b->xx5 = b->xx5 + 1;
        }
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
        if ( b->xx5 )
        {
            b->brushFlags |= 4u;
            b->xx5 = b->xx5 + 1;
        }
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
    {
        entity_s *mine  = (entity_s *)selected_brushes.next->owner->def;
        entity_s *theirs = (entity_s *)b->owner->def;
        if ( theirs->eclass == mine->eclass && !b->xx5 )
        {
            b->brushFlags |= 4u;
            b->xx5 = 1;
        }
    }
    g_nUpdateBits = -1;
    KiwiLightCache_VisibilityChanged(); // KIWI-UX: Class hide changes the faithful caster set.
}

void ShowHidden()
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        b->brushFlags &= ~4u;
        b->xx5 = 0;
    }
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
    {
        b->brushFlags &= ~4u;
        b->xx5 = 0;
    }
    g_nUpdateBits = -1;
    KiwiLightCache_VisibilityChanged(); // KIWI-UX: Newly shown casters must re-enter per-light lists.
}

void ShowLastHidden()
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        if ( b->xx5 )
        {
            int depth = b->xx5;
            b->xx5 = depth - 1;
            if ( depth == 1 )
                b->brushFlags &= ~4u;
        }
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
        if ( b->xx5 )
        {
            int depth = b->xx5;
            b->xx5 = depth - 1;
            if ( depth == 1 )
                b->brushFlags &= ~4u;
        }
    g_nUpdateBits = -1;
    KiwiLightCache_VisibilityChanged(); // KIWI-UX: Restored hide depth changes cached caster visibility.
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x48C7B0  LinkSelected — Selection→Link Selected (33211, Ctrl+Shift+K).
//  Requires exactly two selected brushes from two DIFFERENT non-world entities and
//  Script_Links their DEFs (script_linkname / script_linkto epairs — core in qe3.cpp).
//  Deselects (unless Prefs→linking_keeps_selection) and re-selects the second brush.
// ═════════════════════════════════════════════════════════════════════════════
extern void Script_Link( entity_s_def *a, entity_s_def *b );   // qe3.cpp 0x48BF10

void LinkSelected()
{
    if ( g_qeglobals.d_select_count != 2 )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Must have two entities selected." );
        Sys_Printf( "Must have two entities selected.\n" );
        MessageBeep( 0x40u );
        return;
    }

    entity_s *owner0 = g_qeglobals.d_select_order[0]->owner;
    entity_s *owner1 = g_qeglobals.d_select_order[1]->owner;
    if ( owner0 == world_entity || owner1 == world_entity )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Can't connect to the world." );
        Sys_Printf( "Can't connect to the world.\n" );
        MessageBeep( 0x40u );
        return;
    }

    entity_s_def *a = (entity_s_def *)owner0->def;
    entity_s_def *b = (entity_s_def *)owner1->def;
    if ( a == b )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Brushes are from same entity." );
        MessageBeep( 0x40u );
        return;
    }

    Script_Link( a, b );
    g_nUpdateBits |= 3u;
    if ( !g_PrefsDlg->linking_keeps_selection )
        Select_Deselect( 1 );
    Select_Brush( (selbrush_t *)g_qeglobals.d_select_order[1], 1, 1, 0 );
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x47A260  Get_DistanceBetweenEnts — Misc→Get Distance (33178, Alt+F1).
//  Walks the selected brushes; between each consecutive pair of non-world entities
//  prints the 2D (XY) distance between their origins (only origin[0]/origin[1];
//  origin[2] is not read).
// ═════════════════════════════════════════════════════════════════════════════
void Get_DistanceBetweenEnts()
{
    bool first = true;
    float prevX = 0.0f, prevY = 0.0f;
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        if ( !b->owner || b->owner == world_entity )
            continue;
        entity_s *e = b->def->owner;
        float x = e->origin[0];
        float y = e->origin[1];
        if ( first )
        {
            first = false;
        }
        else
        {
            float dx = x - prevX;
            float dy = y - prevY;
            float dist = (float)sqrt( (double)( dy * dy + dx * dx ) );
            Sys_Printf( "Distance between entities: %f\n", dist );
        }
        prevX = x;
        prevY = y;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x429570  NudgeSelection — keyboard nudge of the selection (Alt+arrows, dir 0..3).
//  Three modes (checked in order):
//    * g_bRotateMode : accumulate a rotation about the view axis by ±amt, show it in
//      the status bar, and rotate every selected brush (Select_RotateAxis + the whole-
//      selection apply).  Direction 2/3 (right/down) flips the sign; XY view rotates
//      about Z (axis 2), XZ about Y (axis 1, sign flipped), YZ about X (axis 0).
//    * g_bScaleMode  : scale the selection by 1.1 / 0.9 (dir up/right vs down/left),
//      masked per axis by g_nScaleHow (X=1/Y=2/Z=4).
//    * default       : plain translate, axis picked from the active XY view type + the
//      nudge direction (the binary tail-calls CMainFrame::Nudge for this).
//  dir enum: 0=Left, 1=Up, 2=Right, 3=Down.  Status text goes to
//  MainFrm_SetStatusText(2, va(...)).
//
//  NudgeSelection_Apply(dir, amt) is the whole body: the binary's CMainFrame* argument fed
//  only the final ->Nudge(nDim, amt) call, and CMainFrame::Nudge touches no
//  CMainFrame state (MoveSelection for curve/terrain point selections, else Select_Move),
//  so its two-branch body is inlined here.  mainfrm.cpp's command table dispatches the four
//  Alt+arrow nudges (32847..32850, the binary's 0x429510..0x429550 handlers) straight here.
// ═════════════════════════════════════════════════════════════════════════════
extern float g_vRotation[3];        // drag.cpp 0x23F164C
extern float g_vRotateOrigin[3];    // drag.cpp 0x23F1658
extern bool  g_bScaleMode;          // drag.cpp 0x23F16DA

void NudgeSelection_Apply( int dir, float amt )
{
    if ( g_bRotateMode )
    {
        int axis = 0;
        if ( Ed_ActiveXY()->m_nViewType == 2 )        // XY (top) → about Z
        {
            axis = 2;
        }
        else if ( Ed_ActiveXY()->m_nViewType == 1 )   // XZ (front) → about Y
        {
            axis = 1;
            amt = -amt;
        }
        if ( dir == 2 || dir == 3 )   // right / down flip the sign
            amt = -amt;

        float rotAxisAngle = -amt;
        g_vRotation[axis] += amt;
        MainFrm_SetStatusText( 2, va( "Rotation x:: %.1f  y:: %.1f  z:: %.1f",
                                      g_vRotation[0], g_vRotation[1], g_vRotation[2] ) );
        if ( rotAxisAngle != 0.0f )
        {
            float rot_around[4][3];
            rot_around[0][0] = g_vRotateOrigin[0];
            rot_around[0][1] = g_vRotateOrigin[1];
            rot_around[0][2] = g_vRotateOrigin[2];
            Select_RotateAxis( axis, rotAxisAngle, (float (*)[4][3])rot_around );
            Select_ApplyMatrix_SelectedBrushes( 0, rot_around[0], rotAxisAngle, 0 );
        }
        g_nUpdateBits = -1;
        return;
    }

    if ( g_bScaleMode )
    {
        if ( dir == 0 || dir == 3 )   // left / down shrink
            amt = -amt;
        float f = ( amt <= 0.0f ) ? 0.89999998f : 1.1f;
        float sx = ( g_nScaleHow & 1 ) ? f : 1.0f;
        float sy = ( g_nScaleHow & 2 ) ? f : 1.0f;
        float sz = ( g_nScaleHow & 4 ) ? f : 1.0f;
        Select_Scale( sx, sy, sz );
        g_nUpdateBits = -1;
        return;
    }

    // Plain translate: pick the axis from the view type + nudge direction.
    int nDim;
    int vt = Ed_ActiveXY()->m_nViewType;
    switch ( dir )
    {
    case 0:  nDim = ( vt == 0 );          amt = -amt; break;   // Left
    case 1:  nDim = ( vt != 2 ) + 1;                  break;   // Up
    case 2:  nDim = ( vt == 0 );                      break;   // Right
    default: nDim = ( vt != 2 ) + 1;      amt = -amt; break;   // Down
    }

    // CMainFrame::Nudge (0x4298B0) — no CMainFrame state is read.
    {
        extern void MoveSelection( float *origin, float *dir, float *move, float *total );  // drag.cpp
        extern void Patch_RedispersePreDrag();                                // pmesh.cpp 0x43D7A0
        float a[3] = { 0.0f, 0.0f, 0.0f };
        a[nDim] = amt;
        if ( ( g_qeglobals.d_select_mode == sel_curvepoint
            || g_qeglobals.d_select_mode == sel_terrainpoint )
          && g_qeglobals.d_num_move_points > 0 )
        {
            // The binary passes the nudge as the total too.  KIWI: re-snapshot first, so a
            // texture-lock-off nudge follows from the S/T as they are now, not from the last
            // drag's start.
            if ( g_qeglobals.d_select_mode == sel_curvepoint )
                Patch_RedispersePreDrag();
            MoveSelection( 0, 0, a, a );
            g_nUpdateBits = -1;
        }
        else
        {
            Select_Move( a, 1 );
            g_nUpdateBits = -1;
        }
    }
}


// ═════════════════════════════════════════════════════════════════════════════
//  0x485B70  Select_AllByKeyValue — the "same target(name)" command core (36121/36123).
//  Reads the FIRST selected entity that has key `key`, then writes that key's value onto
//  EVERY selected entity's DEF (so a group shares one target / targetname).
// ═════════════════════════════════════════════════════════════════════════════
void Select_AllByKeyValue( const char *key )
{
    const char *value = "";
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s_def *def = (entity_s_def *)b->owner->def;
        if ( HasKeyValuePair( def, key ) )
        {
            char *v = ValueForKey2( def, key );
            value = ( v && *v ) ? v : "";
            break;
        }
    }
    if ( value[0] )
    {
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            SetKeyValue( (entity_s_def *)b->owner->def, key, value );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  VERTEX-EDIT MODE — the point/edge handle subsystem.
//  Builds the global d_points/d_edges handle lists from the selected brushes'
//  faces, picks a vertex by ray for dragging, and feeds the drag path
//  (Drag_Setup → SelectVertexByRay → MoveSelection → Brush_MoveVertex).
// ═════════════════════════════════════════════════════════════════════════════

extern winding_t *Brush_MakeFaceWinding( face_t *f, brush_t *def );   // brush.cpp 0x471260

// MAX_POINTS_ON_WINDING is #define'd locally in winding.cpp (1024); mirror it here
// for the MakeFace point-index scratch (a single face winding never exceeds it).
#ifndef MAX_POINTS_ON_WINDING
#define MAX_POINTS_ON_WINDING 1024
#endif

// ─── FindPoint (0x494a10) ────────────────────────────────────────────────────
// The dedup epsilon is compared against a DOUBLE 0.1, NOT float 0.1f
// (== 0.100000001490116, a wider window).
// Return the index of `pt` in g_qeglobals.d_points[], appending it (capped at
// 2047) if not already present (0.1 tolerance per-axis).
static int FindPoint( const float *pt )
{
    for ( int i = 0; i < g_qeglobals.d_numpoints; ++i )
    {
        int axis = 0;
        for ( ; axis < 3; ++axis )
            // the magnitude rounds to float, then compares against DOUBLE 0.1 (see above).
            if ( (float)fabs( pt[axis] - g_qeglobals.d_points[i][axis] ) > 0.1 )
                break;
        if ( axis == 3 )
            return i;          // already in the list
    }

    int idx = g_qeglobals.d_numpoints;
    g_qeglobals.d_points[idx][0] = pt[0];
    g_qeglobals.d_points[idx][1] = pt[1];
    g_qeglobals.d_points[idx][2] = pt[2];
    if ( g_qeglobals.d_numpoints < 2047 )
        ++g_qeglobals.d_numpoints;
    return g_qeglobals.d_numpoints - 1;
}

// ─── FindEdge (0x494ac0) ─────────────────────────────────────────────────────
// 511 cap.
// Register the edge (p1,p2) of face `f`. If the REVERSE edge (p1==.p2, p2==.p1 —
// the binary tests d_edges[i].p1==p2 && .p2==p1) already exists, record `f` as its
// second face and return that index; else append a new edge (capped at 511).
static int FindEdge( face_t *f, int p1, int p2 )
{
    for ( int i = 0; i < g_qeglobals.d_numedges; ++i )
    {
        if ( g_qeglobals.d_edges[i].p1 == p2 && g_qeglobals.d_edges[i].p2 == p1 )
        {
            g_qeglobals.d_edges[i].f2 = f;
            return i;
        }
    }

    int idx = g_qeglobals.d_numedges;
    g_qeglobals.d_edges[idx].p1 = p1;
    g_qeglobals.d_edges[idx].p2 = p2;
    g_qeglobals.d_edges[idx].f1 = f;
    if ( g_qeglobals.d_numedges < 511 )
        ++g_qeglobals.d_numedges;
    return g_qeglobals.d_numedges - 1;
}

// ─── MakeFace (0x494b30) ─────────────────────────────────────────────────────
// Rebuild face `f`'s winding (Brush_MakeFaceWinding), then register each corner
// point (FindPoint) and each edge (FindEdge) into the global handle lists. The
// temporary winding is freed afterward (the binary's --g_windingAlloc; free()).
static void MakeFace( brush_t *def, face_t *f )
{
    winding_t *w = Brush_MakeFaceWinding( f, def );
    if ( !w )
        return;

    int ptIdx[MAX_POINTS_ON_WINDING];
    int n = w->numpoints;
    if ( n > MAX_POINTS_ON_WINDING )
        n = MAX_POINTS_ON_WINDING;
    for ( int i = 0; i < n; ++i )
        ptIdx[i] = FindPoint( w->p[i] );
    for ( int i = 0; i < n; ++i )
        FindEdge( f, ptIdx[i], ptIdx[( i + 1 ) % n] );

    --g_windingAlloc;
    free( w );
}

// ─── SetupVertexSelection (0x494bc0) ─────────────────────────────────────────
// (Re)build the point/edge handle lists from every face of every selected brush.
// Called when entering vertex/edge mode and after each drag rebuilds a winding.
void SetupVertexSelection()
{
    g_qeglobals.d_numpoints = 0;
    g_qeglobals.d_numedges  = 0;
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        // the binary loops b->faceCount (the INSTANCE count, a cached copy of
        // def->faceCount set by Brush_MakeFaceVisuals on the 3D draw), which is 0 right
        // after a load or headless.  The DEF count is authoritative, so iterate
        // that - identical when valid, and also correct before the first camera draw.
        if ( !b->def )
            continue;
        for ( int i = 0; i < b->def->faceCount; ++i )
            MakeFace( b->def, &b->def->faces[i] );
    }
}

// ─── SelectVertexByRay (0x494fd0) ────────────────────────────────────────────
// Pick the d_points[] handle closest to the ray (start,dir) within 8 units and
// push it as the single drag move-point. The dragged vertex's drawVert_t* is the
// d_points[] slot reinterpreted (drawVert_t.xyz is at offset 0), as in the binary.
// MoveSelection reads d_move_points[0]->xyz.
void SelectVertexByRay( float *origin, float *dir )
{
    const float *start = (const float *)(intptr_t)origin;
    const float *dirv  = (const float *)(intptr_t)dir;

    float best     = 8.0f;
    int   bestIdx  = -1;

    if ( g_qeglobals.d_numpoints <= 0 )
    {
        Sys_Printf( "Click didn't hit a vertex\n" );
        return;
    }

    for ( int i = 0; i < g_qeglobals.d_numpoints; ++i )
    {
        const float *p = g_qeglobals.d_points[i];
        // project p onto the ray, then measure the perpendicular distance.
        float rel[3] = { p[0] - start[0], p[1] - start[1], p[2] - start[2] };
        float t = dirv[0] * rel[0] + dirv[1] * rel[1] + dirv[2] * rel[2];
        float proj[3] = { start[0] + dirv[0] * t,
                          start[1] + dirv[1] * t,
                          start[2] + dirv[2] * t };
        float d[3] = { p[0] - proj[0], p[1] - proj[1], p[2] - proj[2] };
        float dist = (float)sqrt( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] );
        if ( best > dist )
        {
            best    = dist;
            bestIdx = i;
        }
    }

    if ( bestIdx == -1 )
    {
        Sys_Printf( "Click didn't hit a vertex\n" );
        return;
    }

    Sys_Printf( "hit vertex\n" );
    g_qeglobals.d_move_points[g_qeglobals.d_num_move_points++] =
        (drawVert_t *)&g_qeglobals.d_points[bestIdx];
}

// ─── SelectCurvePointByRay (0x495150) ────────────────────────────────────────
// The patch sibling of SelectVertexByRay (the curve-point pick for dragging).
// Called by Drag_Setup when a non-Alt press starts in sel_curvepoint mode.
//   * SHIFT held (buttons & MK_SHIFT/4): ray-pick the nearest control-point handle in
//     g_qeglobals.d_points[] (filled by Patch_EditPatch) within (28 / g_zoomLevel)
//     units, then hand it to Patch_ClickControlPoint which queues the
//     matching control point(s) as drag move-points.  Drag_Setup then arms drag_ok
//     and MoveSelection → Patch_UpdateSelected bends the patch.
//   * SHIFT not held (or the ray missed every handle): fall through to the MISS path —
//     switch to sel_area so the press rubber-bands a control-point selection box
//     instead of dragging (the mode switch is kept; the box-point rect-select itself is
//     deferred).
// `origin`/`dir` are the pick-ray vectors; `buttons` is the mouse-button/modifier mask.
extern float g_zoomLevel;                              // 0x25D5A90 (mainfrm.cpp)
extern bool  g_bScaleMode;                             // 0x23F16DA (drag.cpp)
extern "C" void Patch_ClickControlPoint_C( const float *cp, char bMultiAppend, char bColRowSelect ); // pmesh.cpp

void SelectCurvePointByRay( float *origin, float *dir, unsigned int buttons )
{
    const float *start = (const float *)(intptr_t)origin;
    const float *dirv  = (const float *)(intptr_t)dir;

    int bestIdx = -1;

    if ( ( buttons & 4 ) != 0 )         // MK_SHIFT — do the ray-pick
    {
        // Pick radius scales with the zoom (a fixed pixel radius in world units).
        float best = 28.0f;
        if ( g_zoomLevel > 0.0f )
            best = 28.0f / g_zoomLevel;

        for ( int i = 0; i < g_qeglobals.d_numpoints; ++i )
        {
            const float *p = g_qeglobals.d_points[i];
            // project p onto the ray, measure perpendicular distance.
            float rel[3] = { p[0] - start[0], p[1] - start[1], p[2] - start[2] };
            float t = dirv[0] * rel[0] + dirv[1] * rel[1] + dirv[2] * rel[2];
            float proj[3] = { start[0] + dirv[0] * t,
                              start[1] + dirv[1] * t,
                              start[2] + dirv[2] * t };
            float d[3] = { p[0] - proj[0], p[1] - proj[1], p[2] - proj[2] };
            float dist = (float)sqrt( d[0] * d[0] + d[1] * d[1] + d[2] * d[2] );
            if ( best >= dist )
            {
                best    = dist;
                bestIdx = i;
            }
        }

        if ( bestIdx != -1 )
        {
            // hand the clicked handle to Patch_ClickControlPoint: append=(buttons&8), colRow=(buttons&4).
            Patch_ClickControlPoint_C( g_qeglobals.d_points[bestIdx],
                                       (char)( ( buttons & 8 ) != 0 ),
                                       (char)( ( buttons & 4 ) != 0 ) );
            return;
        }
    }

    // ── MISS / no-shift: switch to sel_area (rubber-band point select) ──────────
    if ( !g_bRotateMode && !g_bScaleMode )
    {
        select_t prev = g_qeglobals.d_select_mode;
        g_qeglobals.d_select_mode = sel_area;
        if ( prev == sel_cycle_edge_direction_quad )
            CMainFrame_UpdatePatchToolbarButtons();
    }
}

// ─── AddPlanept (0x477250) ───────────────────────────────────────────────────
// Append a point POINTER to g_qeglobals.d_move_points[] (cap 4096), de-duped
// against the existing terrain points (d_terrapoints) and the move-points already
// queued. Returns 1 if newly added, 0 if a duplicate / list full. SelectFaceEdge
// uses it to register the two edge-endpoint planepts as the drag move-points; the
// dedup is what stops the SHARED endpoint of the two faces being added twice.
static int AddPlanept( float *pt )
{
    if ( g_qeglobals.d_num_move_points == 4096 )
        return 0;
    for ( int i = 0; i < g_qeglobals.d_numterrapoints; ++i )
        if ( g_qeglobals.d_terrapoints[i] == pt )
            return 0;
    for ( int i = 0; i < g_qeglobals.d_num_move_points; ++i )
        if ( (float *)g_qeglobals.d_move_points[i] == pt )
            return 0;
    g_qeglobals.d_move_points[g_qeglobals.d_num_move_points++] = (drawVert_t *)pt;
    return 1;
}

// ─── ClipLineToFace (0x477150) ───────────────────────────────────────────────
// Clip the segment (farPt, nearPt) to the FRONT side of face f's plane. Returns 0 when
// both endpoints are in front (segment fully outside the half-space), 1 otherwise (fully
// behind/inside, or after clipping the front endpoint to the plane). plane.dist is read
// as float.
static char ClipLineToFace( face_t *f, float *farPt, float *nearPt )
{
    float nearDot  = f->plane.normal[1] * nearPt[1] + nearPt[0] * f->plane.normal[0] + f->plane.normal[2] * nearPt[2];
    float nearDist = nearDot - f->plane.dist;
    float farDot   = f->plane.normal[1] * farPt[1] + farPt[0] * f->plane.normal[0] + f->plane.normal[2] * farPt[2];
    float farDist  = farDot - f->plane.dist;
    if ( nearDist >= 0.0f && farDist >= 0.0f )
        return 0;
    if ( nearDist <= 0.0f && farDist <= 0.0f )
        return 1;
    float  frac = nearDist / ( nearDist - farDist );
    float *clip = ( nearDist <= 0.0f ) ? farPt : nearPt;
    clip[0] = ( farPt[0] - nearPt[0] ) * frac + nearPt[0];
    clip[1] = ( farPt[1] - nearPt[1] ) * frac + nearPt[1];
    clip[2] = frac * ( farPt[2] - nearPt[2] ) + nearPt[2];
    return 1;
}

// ─── Brush_SelectFaceForDragging (0x477330) ──────────────────────────────────
// Add the face's 3 planepts to the move-point set, then (recursively) every face in
// the OTHER selected brushes whose planepts are coplanar with this face's plane — so
// dragging a shared wall drags all the brushes that share it.
// The SHEAR branch (shear set, non-terrain: rebuild each adjacent face's winding, find the edge coplanar with the dragged plane, AddPlanept its
// endpoints + insert rotated planepts) is deferred - it is the Ctrl+LMB face-SHEAR
// gesture, not the plain side-stretch.  TERRAIN handling lives in Brush_SideSelect.
void Brush_SelectFaceForDragging( brush_t *def, face_t *dragFace, int shear )
{
    if ( *(int *)&def->owner->eclass->fixedsize )   // fixed-size entity → no side-drag
        return;

    int added = 0;
    added += AddPlanept( dragFace->planepts[0] );
    added += AddPlanept( dragFace->planepts[1] );
    added += AddPlanept( dragFace->planepts[2] );
    if ( !added )
        return;

    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
    {
        brush_t *other = i->def;
        if ( other == def )
            continue;
        for ( unsigned fi = 0; fi < (unsigned)other->faceCount; ++fi )
        {
            face_t *cf = &other->faces[fi];
            int k = 0;
            do {
                float d = cf->planepts[k][1] * dragFace->plane.normal[1]
                        + cf->planepts[k][0] * dragFace->plane.normal[0]
                        + cf->planepts[k][2] * dragFace->plane.normal[2];
                if ( fabs( d - dragFace->plane.dist ) > 0.1 )   // tolerance 0.1
                    break;
                ++k;
            } while ( k < 3 );
            if ( k == 3 ) {
                Brush_SelectFaceForDragging( other, cf, shear );
                break;
            }
        }
    }
    // SHEAR branch (if shear && !terrain) deferred — see header.
    (void)shear;
}

// ─── Brush_SideSelect (0x4777d0) ─────────────────────────────────────────────
// The click missed the brush surface, so grab one or more side planes for dragging.
// For each face: extend the ray, clip it to the brush interior (every OTHER face),
// and if it still exits through this face, either (a) for a numberId==2 brush past
// face index 2, collect every side face's planept vec3's as draggable "terrain"
// points (then RETURN), or (b) hand the face to Brush_SelectFaceForDragging.
// The TERRAIN branch:
//   * def->numberId==2 AND the exiting face index >= 2 triggers the collector.
//   * For each face >= 2 of a non-fixedsize brush, the 3 planept vec3's
//     (face_t.planepts[0..2]) are appended to
//     g_qeglobals.d_terrapoints[] (cap 4096), de-duped against the existing
//     terrapoints AND d_move_points (raw pointer compare).
//   * d_select_translate_unk = def->faces[0].plane.normal (the dragged plane normal).
//   The collector RETURNS once it fires (does NOT fall through to
//   Brush_SelectFaceForDragging).  MoveSelection's terrain-drag branch consumes
//   d_terrapoints/d_numterrapoints/d_select_translate_unk.
void Brush_SideSelect( float *trace_start, brush_t *def, float *trace_dir, int shear )
{
    int facecount = def->faceCount;
    for ( int fi = 0; fi < facecount; ++fi )
    {
        float nearPt[3] = { trace_start[0], trace_start[1], trace_start[2] };
        float farPt[3]  = { trace_dir[0] * 262144.0f + trace_start[0],
                            trace_dir[1] * 262144.0f + trace_start[1],
                            262144.0f * trace_dir[2] + trace_start[2] };
        for ( int fj = 0; fj < def->faceCount; ++fj )                               // clip to the brush interior
            if ( fj != fi )
                ClipLineToFace( &def->faces[fj], farPt, nearPt );
        int k = 0;                                                                  // did the near point move?
        for ( ; k < 3; ++k )
        {
            float d = nearPt[k] - trace_start[k];
            if ( d * d > 0.00000100000011116208 )
                break;
        }
        if ( k >= 3 )
            continue;                                                               // ray never reached this face
        if ( !ClipLineToFace( &def->faces[fi], farPt, nearPt ) )                    // exits through this face → grab it
        {
            // numberId==2 (terrain-style) brush AND the exiting face is index >= 2:
            // collect every side face's planepts as draggable terrain points, then bail.
            if ( def->numberId == 2 && (unsigned)fi >= 2 )
            {
                if ( (unsigned)def->faceCount > 2u )                                // facecount<=2 skips collection
                {
                    int d_numterrapoints = g_qeglobals.d_numterrapoints;
                    int d_num_move_points = g_qeglobals.d_num_move_points;
                    for ( int fk = 2; fk < def->faceCount; ++fk )                   // faces 2..N
                    {
                        // candidate terrain verts = the face's 3 planept vec3's.
                        float *tv = def->faces[fk].planepts[0];
                        if ( !*(int *)&def->owner->eclass->fixedsize )              // skip fixed-size (bbox) brushes
                        {
                            for ( int n = 3; n; --n )
                            {
                                if ( d_numterrapoints != 4096 )
                                {
                                    bool dup = false;
                                    for ( int t = 0; t < d_numterrapoints; ++t )
                                        if ( g_qeglobals.d_terrapoints[t] == tv ) { dup = true; break; }
                                    if ( !dup )
                                    {
                                        for ( int m = 0; m < d_num_move_points; ++m )
                                            if ( (float *)g_qeglobals.d_move_points[m] == tv ) { dup = true; break; }
                                    }
                                    if ( !dup )
                                    {
                                        g_qeglobals.d_terrapoints[d_numterrapoints] = tv;
                                        d_num_move_points = g_qeglobals.d_num_move_points;
                                        d_numterrapoints = ++g_qeglobals.d_numterrapoints;
                                    }
                                }
                                tv += 3;                                            // next planept (vec3)
                            }
                        }
                    }
                }
                // d_select_translate_unk = face[0]'s plane normal.
                g_qeglobals.d_select_translate_unk[0] = def->faces[0].plane.normal[0];
                g_qeglobals.d_select_translate_unk[1] = def->faces[0].plane.normal[1];
                g_qeglobals.d_select_translate_unk[2] = def->faces[0].plane.normal[2];
                return;
            }
            Brush_SelectFaceForDragging( def, &def->faces[fi], shear );
        }
    }
}

// ─── SelectFaceEdge (0x494c30) ───────────────────────────────────────────────
// For face `f` of brush `def`, find the directed edge (p1→p2, indices into
// g_qeglobals.d_points[]) in the face's winding, then REWRITE the face's three
// defining planepts to the three consecutive winding points spanning that edge
// (grid-snapped) and queue the first two (the edge endpoints) as drag move-points
// via AddPlanept. The face plane is preserved (the 3 points are co-planar winding
// verts), so when MoveSelection later translates those planepts + rebuilds the
// windings, the brush reshapes along the dragged edge.
static void SelectFaceEdge( brush_t *def, face_t *f, int p1, int p2 )
{
    winding_t *w = Brush_MakeFaceWinding( f, def );
    if ( !w )
        return;

    int n = w->numpoints;
    // 129 slots, as in the binary; a single brush-face winding never approaches that.
    int idx[129];
    if ( n > 129 )
        n = 129;
    for ( int i = 0; i < n; ++i )
        idx[i] = FindPoint( w->p[i] );

    // search for the consecutive index pair (p1,p2) in the winding's point list.
    int j = 0;
    if ( n > 0 )
    {
        for ( ; j < n; ++j )
            if ( idx[j] == p1 && idx[( j + 1 ) % n] == p2 )
                break;
    }
    if ( n <= 0 || j >= n )
    {
        Sys_Printf( "SelectFaceEdge: failed\n" );
        --g_windingAlloc;
        free( w );
        return;
    }

    // rewrite the face's three planepts to the three winding points spanning the
    // edge (planepts[2] = the next point, keeping the plane well-defined).
    int i0 = idx[j];
    int i1 = idx[( j + 1 ) % n];
    int i2 = idx[( j + 2 ) % n];
    for ( int k = 0; k < 3; ++k )
    {
        f->planepts[0][k] = g_qeglobals.d_points[i0][k];
        f->planepts[1][k] = g_qeglobals.d_points[i1][k];
        f->planepts[2][k] = g_qeglobals.d_points[i2][k];
    }

    // grid-snap all 9 planept coords: floor(v/grid + 0.5) * grid.
    {
        float grid = grid_sizes[g_qeglobals.d_gridsize];
        float *fp  = &f->planepts[0][0];
        for ( int p = 0; p < 3; ++p )
            for ( int k = 0; k < 3; ++k )
            {
                *fp = (float)( floorf( *fp / grid + 0.5f ) * grid );
                ++fp;
            }
    }

    // queue the two edge endpoints (planepts[0], planepts[1]) as drag move-points.
    AddPlanept( &f->planepts[0][0] );
    AddPlanept( &f->planepts[1][0] );

    --g_windingAlloc;
    free( w );
}

// ─── Select_Edge (0x494dc0) ──────────────────────────────────────────────────
// Midpoint-closest-to-ray pick (best>dist, 8u init); per-brush SelectFaceEdge for both
// edge directions.
// Pick the d_edges[] edge whose midpoint is closest to the ray (within 8 units),
// then for every selected brush register the two faces sharing that edge via
// SelectFaceEdge — once for each direction (f1: p1→p2, f2: p2→p1). The result is
// the edge's two endpoints queued as drag move-points (in each face's planepts).
// Drag_Setup calls Select_Edge(trace_dir, trace_start).
void Select_Edge( float *dir, float *origin )
{
    const float *dirv  = (const float *)(intptr_t)dir;
    const float *start = (const float *)(intptr_t)origin;

    float best    = 8.0f;
    int   bestIdx = -1;

    for ( int i = 0; i < g_qeglobals.d_numedges; ++i )
    {
        const pedge_t *e = &g_qeglobals.d_edges[i];
        // edge midpoint.
        float mid[3];
        mid[0] = ( g_qeglobals.d_points[e->p2][0] + g_qeglobals.d_points[e->p1][0] ) * 0.5f;
        mid[1] = ( g_qeglobals.d_points[e->p2][1] + g_qeglobals.d_points[e->p1][1] ) * 0.5f;
        mid[2] = 0.5f * ( g_qeglobals.d_points[e->p2][2] + g_qeglobals.d_points[e->p1][2] );
        // perpendicular distance from the midpoint to the ray.
        float rel[3] = { mid[0] - start[0], mid[1] - start[1], mid[2] - start[2] };
        float t = dirv[1] * rel[1] + dirv[0] * rel[0] + dirv[2] * rel[2];
        float proj[3] = { dirv[0] * t + start[0],
                          dirv[1] * t + start[1],
                          t * dirv[2] + start[2] };
        float d[3] = { mid[0] - proj[0], mid[1] - proj[1], mid[2] - proj[2] };
        float dist = (float)sqrt( d[2] * d[2] + d[1] * d[1] + d[0] * d[0] );
        if ( best > dist )
        {
            best    = dist;
            bestIdx = i;
        }
    }

    if ( bestIdx == -1 )
    {
        Sys_Printf( "Click didn't hit an edge\n" );
        return;
    }

    Sys_Printf( "hit edge\n" );
    const pedge_t *e = &g_qeglobals.d_edges[bestIdx];
    g_qeglobals.d_num_move_points = 0;
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        // the binary calls SelectFaceEdge unconditionally for BOTH faces with no
        // NULL guard, so it AVs in Brush_MakeFaceWinding(NULL) when an edge has only one
        // face registered - which FindEdge leaves whenever the reverse direction never
        // registered (a face whose winding failed to build).  Such an edge's midpoint IS
        // drawn as a handle, so a click near it can pick it.  Skip the NULL side.
        if ( e->f1 )
            SelectFaceEdge( b->def, e->f1, e->p1, e->p2 );
        if ( e->f2 )
            SelectFaceEdge( b->def, e->f2, e->p2, e->p1 );
    }
}



// deps of Select_Ungroup:
extern selbrush_t *Entity_LinkBrush_0_extern( entity_s *e, entity_brush_s *b );   // entity.cpp
// ─────────────────────────────────────────────────────────────────────────────
// 0x490780  Select_Ungroup  (select.cpp:1719 asserts)
// Ungroups selected non-worldspawn brush entities by relinking brushes to worldspawn.
// The rebuild includes the vertex/edge handles (sel_vertex/sel_edge), else they dangle
// after ungroup.
// ─────────────────────────────────────────────────────────────────────────────
LRESULT Select_Ungroup()
{
    if ( selected_brushes.next == &selected_brushes )
        return (LRESULT)Sys_Printf( "No grouped entities selected.\n" );

    int numselectedgroups = 0;

    // We need to iterate selected_brushes but it changes as we relink; snapshot owner list.
    selbrush_t *sb  = selected_brushes.next;
    entity_s   *last = nullptr;

    while ( sb != &selected_brushes )
    {
        iassert( sb->owner );   // select.cpp:1719
        iassert( sb->def );   // select.cpp:1720
        // the instance's owner and its def's owner agree (the def's owner is the entity
        // DEF, reached via the instance's def).
        iassert( sb->owner->def == sb->def->owner );   // select.cpp:1721

        entity_s *e      = sb->owner;
        selbrush_t *next = sb->next;

        if ( e != world_entity && !*(int *)&((entity_s_def *)e->def)->eclass->fixedsize )
        {
            // Walk this entity's brush instances, relinking each to worldspawn.
            selbrush_t *b = e->brushes.ownerNext;
            while ( b != &e->brushes )
            {
                selbrush_t *biNext = b->ownerNext;
                iassert( b->def->owner == b->owner->def );   // select.cpp:1730
                bool wasSelected = ( ( b->brushFlags & 0x80 ) != 0 );

                if ( wasSelected )
                    Brush_Deselect_Helper( b );

                // Unlink brush def from old entity
                brush_t *bDef = b->def;
                Entity_UnlinkBrush( bDef );

                // Relink into worldspawn def
                entity_s_def *worldDef = (entity_s_def *)world_entity->def;
                Entity_LinkBrush( bDef, (entity_s *)worldDef );

                // Relink brush instance into worldspawn's instance list
                Entity_LinkBrush_0_extern( world_entity, b );

                Brush_RebuildAfterEdit( bDef );   // vertex handles too: else they dangle after ungroup

                if ( wasSelected )
                    Brush_Select_Helper( b );

                // re-assert after the relink (now both point at the worldspawn DEF).
                iassert( b->def->owner == b->owner->def );   // select.cpp:1746

                b = biNext;
            }

            iassert( e->brushes.ownerNext == &e->brushes );   // select.cpp:1748

            Entity_Free( e );
            ++numselectedgroups;
        }

        sb = next;
    }

    if ( numselectedgroups <= 0 )
        return (LRESULT)Sys_Printf( "No grouped entities selected.\n" );

    const char *suffix = ( numselectedgroups == 1 ) ? "y" : "ies";
    LRESULT result     = (LRESULT)Sys_Printf( "Ungrouped %d entit%s.\n", numselectedgroups, suffix );
    g_nUpdateBits      = -1;
    return result;
}

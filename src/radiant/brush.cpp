#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\radiant\brush.cpp
// Ported from CoD4Radiant.exe (IW3xRadiant.i64).  GtkRadiant 1.6 radiant/brush.cpp was
// used only for algorithm names.

#include "stdafx.h"
#include "winding.h"
#include "prefs.h"                  // g_PrefsDlg (prefData_t* — texture/lightmap lock)
#include "linearmapping.h"          // LinearMapping_Setup/Apply (texture-lock reproject)
#include <gfx_d3d/r_rendercmds.h>   // R_AddCmd_Line3D, GfxPointVertex, GfxColor
#include <universal/assertive.h>    // iassert (USE_ASSERTS always on; same handler as Assert)
#include <string>                   // SelectedAssociated's matchValue
#include "kiwi_shadowcache.h"       // KiwiShadowCache_Invalidate
#include "kiwi_walkcache.h"         // the shared prefab-walk recording
#include "kiwi_matconvert.h"        // KIWI-UX: solid-red Shift+L face diagnostic
#include "kiwi_ux.h"                // KIWI: unified facing-arrow visibility
#include "kiwi_fmt.h"               // KIWI: exact key values
#include "kiwi_floatsnap.h"         // KIWI: noise-only half-grid snap
#include "kiwi_texlock.h"           // KIWI: exact texture lock (translate / rotate / flip)
#include "kiwi_angles.h"            // KIWI: KiwiAng_ToAxis (physics cylinder frame)
#include "kiwi_ported.h"            // KIWI: shared engine/editor externs
#include <unordered_map>
#include <vector>

// faceVisuals_s / faceVis_s and their static_asserts live in qe3.h (select.cpp uses them).

// orientation_t is defined in fxprimitives.h (included via r_image.h → xanim.h → snd_public.h).
// Layout: float origin[3](0x00) + float axis[3][3](0x0C) = 48 bytes.
static_assert(sizeof(orientation_t) == 48, "orientation_t (fxprimitives.h != IDB)");

// ─── helpers from qe3.cpp / engine_stubs.cpp ────────────────────────────────
extern char *AllocMaterialString( const char *src );

// ─── CRT import thunk ───────────────────────────────────────────────────────
// 0x583a8d j__free_0 — thin forwarder to free.
static inline void j__free_0( void *p ) { free( p ); }

// ─── globals ─────────────────────────────────────────────────────────────────
extern char  g_activeLayer_string[];   // engine_stubs.cpp

// ─── world entity and UI notification stubs ─────────────────────────────────
namespace SurfaceInspector { void UpdateSurfaceDialog(); }

// ─── MFC parent window (CMainFrame) ─────────────────────────────────────────
#include "mainfrm.h"
#include "xywnd.h"                   // xywndState_t / Ed_ActiveXY
extern int CamWnd_RemoveLightPreview( selbrush_t *removed );   // camwnd.cpp (0x4062d0)

// ─── patch dialog (patchdialog.cpp) ─────────────────────────────────────────
// g_PatchDialog_GetPatchInfo = CPatchDialog::GetPatchInfo (0x436ba0).
// PatchDialog_IsOpen is the binary's `if (CWnd_PatchDialog.m_hWnd)` gate; false in the
// no-MFC shell, so the guarded refresh is skipped as with the inspector closed.
extern bool  PatchDialog_IsOpen();
extern void  g_PatchDialog_GetPatchInfo();

// ─── Entity_RebuildBounds (0x485390, entity.cpp) ────────────────────────────
extern void Entity_RebuildBounds( entity_s *e );

// colorWhite[4] is static const in q_shared.h (included via stdafx.h → qe3.h → qedefs.h)

// ─── brush geometry helpers ──────────────────────────────────────────────────
// The CM_* brush-winding pipeline + Brush_MakeFacePlanes / VectorMaxValues are now
// implemented in this file (see the "brush winding generation" block below).
extern unsigned int Brush_SnapPlanepts( brush_t *b );   // engine_stubs (bFull-only edit path)

// kisak engine math (IntersectPlanes / SnapPointToIntersectingPlanes / Vec3Cross).
#include <universal/com_math.h>
extern int Face_MakePlane( face_t *face );              // brush.cpp

// ─── patch helpers (patchsys.cpp / patch.cpp) ───────────────────────────────
extern void        Patch_Delete( patchMesh_t *patch );
extern patch_t    *Patch_AllocInstance( patchMesh_t *def );
extern curvePatchDef_t *Patch_GenericMesh2( patchMesh_t *p, int layer, int *colMap, int *rowMap ); // pmesh.cpp 0x438b60
extern void        Patch_FreeInstance( patch_t *patch );
extern void        Patch_Fill_ResetVisuals( patch_t *patch );
extern patchMesh_t *Patch_Duplicate( patchMesh_t *from );

// ─── vis helpers ─────────────────────────────────────────────────────────────
extern void Visuals_VisArray( faceVis_s *vis );
extern int  Visuals_InitFaceVis( faceVis_s *vis, face_t *faceDef,
                                  const orientation_t *orient );
bool        Visuals_RewriteFaceVis( faceVis_s *vis, face_t *faceDef,
                                    const orientation_t *orient );   // KIWI: in-place refill
// Vis_Free + PlanePts_Alloc are defined further below; Brush_MakeFaceVisuals uses them
// before their definitions, so forward-declare.
void  Vis_Free( int count, faceVis_s *f, selbrush_t *brushPtr );
void *PlanePts_Alloc( int count );
// Entity_GetOrientationInverse (entity.cpp, 0x482B50)
extern void Entity_GetOrientationInverse( entity_s_def *entityDef, orientation_t *orient,
                                           orientation_t *outOrient );

// prefab_s is defined in qe3.h; only its instanced-brush list sentinel is accessed here
// (the prefab-render path).

// ═════════════════════════════════════════════════════════════════════════════
//  0x477c50 Brush_MakeFaceVisuals + Brush_CheckBuildFaceVis (0x477d70)
//  — the per-instance faceVis builder.  ONE lifecycle:
//  Brush_CheckBuildFaceVis calls Brush_MakeFaceVisuals on a stale version, which
//  (re)allocates faceVis_s[faceCount] and per face runs Visuals_VisArray (free the old
//  visArray) then Visuals_InitFaceVis (build the per-layer GfxWorldVertex run, upload
//  it to the editor VB pool, record {Material*,vbHandle}).
//  This is the persistent surf-cache the filled/textured camera draw consumes.
//  The GPU half is skipped when no D3D device is up (Radiant_FaceVisGpuReady
//  == false — the headless gates).  The faceVis then stays the identity array
//  (visCount=0, vertcount=winding count), which is all any SELECTION path reads.
// ─────────────────────────────────────────────────────────────────────────────

// Renderer-ready predicate for the GPU faceVis build.  True once the D3D9 device the
// editor surf-cache VB pool (Editor_VB_Upload, r_ed_vertbuf.cpp) writes through is up.
// Defined in camwnd.cpp (it owns the gfx_d3d includes); declared here.
extern bool Radiant_FaceVisGpuReady();

static void Brush_MakeFaceVisuals( selbrush_t *b, const float *orientArg )
{
    iassert( b );                                  // brush.cpp:3441
    iassert( b->version != b->def->version );      // brush.cpp:3442

    // Reallocate b->faces only when the face count changes.  Vis_Free releases each
    // old face's visArray (Visuals_VisArray) before freeing; PlanePts_Alloc =
    // operator new(12*count) + memset 0 over faceVis_s[].
    if ( b->faceCount != b->def->faceCount )
    {
        if ( b->faces )
            Vis_Free( b->faceCount, b->faces, b );
        b->faceCount = b->def->faceCount;
        b->faces = (faceVis_s *)PlanePts_Alloc( b->faceCount > 0 ? b->faceCount : 1 );
    }

    // For a fixedsize entity (model/light) the binary inverts the entity orientation
    // so the per-face geometry is built in the entity's local frame.
    // eclass lives on the entity DEF, NOT the instance — read it through owner->def.
    // Ed_BrushFloorRay
    // passes a transient instance with owner==NULL (the floor-march wrapper); guard it.
    const orientation_t *orient = (const orientation_t *)orientArg;
    char localOrientBuf[48];                        // orientation_t is fwd-declared here
    orientation_t *localOrient = (orientation_t *)localOrientBuf;
    entity_s_def *eDef = b->owner ? (entity_s_def *)b->owner->def : nullptr;
    if ( eDef && eDef->eclass && eDef->eclass->fixedsize )
    {
        Entity_GetOrientationInverse( eDef, (orientation_t *)orientArg, localOrient );
        orient = localOrient;
    }

    // Per-face: free any stale visArray, then build the surf-cache visuals.  The GPU
    // build is the binary's default; skip it (identity array only) when no device.
    // Also skip it when owner==NULL: that is ONLY Ed_BrushFloorRay's TRANSIENT stack
    // instance (select.cpp), which forces a rebuild just to clip a ray against
    // def->faces[i].plane and never reads the GPU visArray.  Building it there would
    // alloc+free editor VB-pool slots on every floor ray, recycling slots out from under
    // in-flight geometry surfs.  The binary never GPU-builds an owner-less brush (its
    // floor-march passes real, already-built instances).
    const bool gpu = Radiant_FaceVisGpuReady() && ( b->owner != nullptr );
    for ( int i = 0; i < b->faceCount; ++i )
    {
        faceVis_s *fv = &b->faces[i];
        // KIWI: same materials and vertex count = refill the face's runs in place (see
        // Visuals_RewriteFaceVis); anything else takes the ported rebuild below.
        if ( gpu && Visuals_RewriteFaceVis( fv, &b->def->faces[i], orient ) )
            continue;
        Visuals_VisArray( fv );                     // free old visArray
        if ( gpu )
            Visuals_InitFaceVis( fv, &b->def->faces[i], orient );   // build + upload
        else
            fv->vertcount = b->def->faces[i].w ? b->def->faces[i].w->numpoints : 0;
    }

    // KIWI-UX (ROUND AB, ITEM 1): a rebuild that was skipped because the DEVICE IS LOST has
    // not actually cached anything, so it must not claim the version — leave it armed and let
    // the first frame after recovery do the real build.  Every other case (device present, or
    // headless with no device at all) syncs exactly as the binary does.  See camwnd.cpp
    // Radiant_FaceVisDeviceLost.
    extern bool Radiant_FaceVisDeviceLost();          // camwnd.cpp
    if ( !Radiant_FaceVisDeviceLost() )
        b->version = b->def->version;
}

// 0x477d70 Brush_CheckBuildFaceVis — rebuild the instance faceVis when the def
// geometry version has advanced past the cached one.
void Brush_CheckBuildFaceVis( selbrush_t *b, const float *orient )
{
    iassert( b );        // brush.cpp:3484
    iassert( b->def );   // brush.cpp:3485
    if ( b->version != b->def->version )
    {
        Brush_MakeFaceVisuals( b, orient );
        // KIWI-UX (ROUND AB, ITEM 1): the invariant holds except while the device is LOST,
        // where the rebuild deliberately stays armed (see Brush_MakeFaceVisuals' tail).
        extern bool Radiant_FaceVisDeviceLost();    // camwnd.cpp
        if ( !Radiant_FaceVisDeviceLost() )
        {
            iassert( b->version == b->def->version );   // brush.cpp:3491 (level 0, post-rebuild invariant)
        }
    }
}

// Entity_LinkBrush / Entity_UnlinkBrush ─ combined (0x475730)
static void Entity_LinkBrush( entity_s *entity, selbrush_t *b );

// Entity_UnlinkBrush wrapper (0x485020): unlinks the brush def from its entity's def-list.
// Brush_Free uses this when refCount drops to 1 (still referenced by its owner).
// A thin forwarder; the body lives in entity.cpp.
static void Entity_UnlinkBrush_def( brush_t *def )
{
    Entity_UnlinkBrush( def );
}

// ─── Vis_Free forward (needed by Brush_Free, Brush_InvalidateVis) ────────────
extern void Vis_Free( int count, faceVis_s *f, selbrush_t *brushPtr );

// ─── PlanePts_Alloc forward ──────────────────────────────────────────────────
extern void *PlanePts_Alloc( int count );

// ────────────────────────────────────────────────────────────────────────────
// Face_Alloc_R  (0x470140) — allocates count zeroed face entries
// ────────────────────────────────────────────────────────────────────────────
static face_t *Face_Alloc_R( int count )
{
    iassert( count > 0 );
    if ( count <= 0 || (size_t)count > SIZE_MAX / sizeof(face_t) )
    {
        Com_Error( ERR_FATAL, "Face_Alloc_R: invalid face count %i", count );
        return NULL;
    }
    size_t bytes = (size_t)count * sizeof(face_t);
    face_t *v = (face_t *)operator new( bytes );
    memset( v, 0, bytes );
    return v;
}

// ────────────────────────────────────────────────────────────────────────────
// Face_Free  (0x4701a0) — frees windings in face array then frees the array
// ────────────────────────────────────────────────────────────────────────────
static void Face_Free( int count, face_t *f )
{
    iassert( f );
    iassert( count >= 0 );
    for ( int i = 0; i < count; ++i )
    {
        winding_t *w = f[i].w;
        if ( w )
        {
            --g_windingAlloc;
            free( w );
            f[i].w = nullptr;
        }
    }
    j__free_0( f );
}

// ────────────────────────────────────────────────────────────────────────────
// PlanePts_Alloc  (0x470250) — allocates count*12 zero bytes (faceVis planePts)
// ────────────────────────────────────────────────────────────────────────────
void *PlanePts_Alloc( int count )
{
    iassert( count > 0 );
    if ( count <= 0 || (size_t)count > SIZE_MAX / sizeof(faceVis_s) )
    {
        Com_Error( ERR_FATAL, "PlanePts_Alloc: invalid face count %i", count );
        return NULL;
    }
    size_t bytes = (size_t)count * sizeof(faceVis_s);
    void *p = operator new( bytes );
    memset( p, 0, bytes );
    return p;
}

// ────────────────────────────────────────────────────────────────────────────
// Vis_Free  (0x4702b0) — calls Visuals_VisArray on each slot then frees array
// ────────────────────────────────────────────────────────────────────────────
void Vis_Free( int count, faceVis_s *f, selbrush_t *b )
{
    iassert( b );            // brush.cpp:337
    iassert( f );            // brush.cpp:338
    iassert( count >= 0 );   // brush.cpp:339
    for ( int i = 0; i < count; ++i )
    {
        Visuals_VisArray( &f[i] );
    }
    j__free_0( f );
}

// ────────────────────────────────────────────────────────────────────────────
// Face_Alloc  (0x471500) — appends a copy of *src to b's face array
// Returns pointer to the newly appended face entry.
// ────────────────────────────────────────────────────────────────────────────
face_t *Face_Alloc( brush_t *b, face_t *f )
{
    iassert( b );             // brush.cpp:829
    iassert( f );             // brush.cpp:830

    face_t *faceList = Face_Alloc_R( b->faceCount + 1 );
    iassert( faceList );   // brush.cpp:833

    // copy existing faces (INCLUDING their winding pointers) to faceList
    if ( b->faceCount )
    {
        memcpy( faceList, b->faces, sizeof(face_t) * b->faceCount );
    }

    // append new face (clone its winding)
    face_t *dst = faceList + b->faceCount;
    memcpy( dst, f, sizeof(face_t) );
    dst->w = f->w ? Winding_Clone( f->w ) : nullptr;

    // The existing faces' windings are now OWNED by faceList (the pointers were
    // memcpy'd over). NULL them in the OLD array so the Face_Free below does NOT
    // free them out from under faceList (Brush_MoveVertex reads the windings after
    // Face_Alloc).
    if ( b->faces )
    {
        for ( int i = 0; i < b->faceCount; ++i )
            b->faces[i].w = nullptr;
        Face_Free( b->faceCount, b->faces );
    }

    ++b->faceCount;
    b->faces = faceList;
    return faceList + b->faceCount - 1;
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_RemoveFace  (0x471640)
// Removes face at faceIndex from b's face array; shifts remaining faces down.
// KIWI: void - the binary returned a byte offset on one path and the index on the other, and
// no caller read either.
// ────────────────────────────────────────────────────────────────────────────
void Brush_RemoveFace( brush_t *b, unsigned int faceIndex )
{
    iassert( b );   // brush.cpp:852
    iassert( faceIndex >= 0 && faceIndex < b->faceCount );   // brush.cpp:853

    face_t *f = b->faces + faceIndex;
    iassert( f );   // brush.cpp:83 (inlined accessor null-check; never fires)

    winding_t *w = f->w;
    if ( w )
    {
        --g_windingAlloc;
        free( w );
        f->w = nullptr;
    }

    --b->faceCount;
    for ( unsigned int i = faceIndex; i < (unsigned int)b->faceCount; ++i )
    {
        b->faces[i] = b->faces[i + 1];
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_FaceIndexCmp  (0x471760) — face-index comparator for sorting
// Compares plane.dist of face[fi1] vs face[fi2]; returns -1/0/1
// ────────────────────────────────────────────────────────────────────────────
int Brush_FaceIndexCmp( unsigned int faceIndex2, brush_t *b, unsigned int faceIndex )
{
    iassert( faceIndex >= 0 && faceIndex < b->faceCount );     // brush.cpp:883
    iassert( faceIndex2 >= 0 && faceIndex2 < b->faceCount );   // brush.cpp:884

    double dist1 = (double)b->faces[faceIndex2].plane.dist;
    double dist2 = (double)b->faces[faceIndex].plane.dist;

    if ( dist1 < dist2 )
        return 1;
    if ( dist1 > dist2 )
        return -1;

    // equal dist: compare normal components: fi1>fi2 -> -1, fi1<fi2 -> +1.
    float *n1 = b->faces[faceIndex2].plane.normal;
    float *n2 = b->faces[faceIndex].plane.normal;
    for ( int c = 0; c < 3; ++c )
    {
        if ( n1[c] > (double)n2[c] ) return -1;
        if ( n1[c] < (double)n2[c] ) return  1;
    }

    // equal normals: compare unk03 (axis alignment): fi1>fi2 -> -1, fi1<fi2 -> +1.
    int axis1 = b->faces[faceIndex2].unk03;
    int axis2 = b->faces[faceIndex].unk03;
    if ( axis1 > axis2 ) return -1;
    if ( axis1 < axis2 ) return  1;
    return 0;
}

// ────────────────────────────────────────────────────────────────────────────
// Face_InitMaterialChannel  (0x472C90) — initialises one material channel
// channel: 0=$default, 1=lightmap_gray, 2=smoothing_hard
// ────────────────────────────────────────────────────────────────────────────
int Face_InitMaterialChannel( unsigned int textureChannel, face_t *faceDef,
                                      float src )
{
    static const char *tex_names[3] = { "$default", "lightmap_gray", "smoothing_hard" };
    iassert( faceDef );   // brush.cpp:1383
    vassert( (textureChannel >= 0 && textureChannel < QER_TEX_CHAN_COUNT),
             "(textureChannel) = %i", (int)textureChannel );   // brush.cpp:1384
    patchMesh_material *slot = (patchMesh_material *)&faceDef->mtldef[textureChannel];
    SetMaterial( tex_names[textureChannel], slot );
    return Init_MaterialLayer( (MaterialDef *)slot, src );
}

// ────────────────────────────────────────────────────────────────────────────
// Face_SetDefaultMaterials  (0x472D30)
// Sets material channels 1 and 2 from the d_savedinfo texture table.
// Channel 0 ($default) is skipped.
// ────────────────────────────────────────────────────────────────────────────
static void Face_SetDefaultMaterials( face_t *faceDef )
{
    iassert( faceDef );   // brush.cpp:1397
    // The d_savedinfo material channel table (g_qeglobals.random_texture_stuff[]).
    // Channel 0 is skipped (only lightmap and smoothing are defaulted here).
    for ( unsigned int ch = 1; ch < 3; ++ch )
    {
        Face_InitMaterialChannel( ch, faceDef, g_qeglobals.random_texture_stuff[ch].sampleSize );
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_SetDefaultMaterials  (0x472D90)
// ────────────────────────────────────────────────────────────────────────────
static void Brush_SetDefaultMaterials( brush_t *b )
{
    iassert( b );   // brush.cpp:1413
    for ( int i = 0; i < b->faceCount; ++i )
        Face_SetDefaultMaterials( &b->faces[i] );
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_Alloc  (0x4751e0) — allocates and zero-initialises a brush_t
// planeptsSrc = the MaterialDef block copied into each face's mtldef[0]
// ecls        = eclass_t* (or nullptr)
// ────────────────────────────────────────────────────────────────────────────
brush_t *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls )
{
    // Pack colorWhite into GfxColor
    GfxColor col;
    if ( ecls )
    {
        Byte4PackPixelColor( ecls->color, &col );
    }
    else
    {
        Byte4PackPixelColor( const_cast<float*>(colorWhite), &col );
    }

    brush_t *b = (brush_t *)operator new( sizeof(brush_t) );
    memset( b, 0, sizeof(brush_t) );
    iassert( b );   // brush.cpp:2354

    if ( b->parent_layer_string )
        j__free_0( b->parent_layer_string );

    size_t layerLen = strlen( g_activeLayer_string );
    void *layerCopy = operator new( layerLen + 1 );
    memcpy( layerCopy, g_activeLayer_string, layerLen + 1 );
    b->parent_layer_string = (char *)layerCopy;

    b->faceCount = 6;
    if ( ecls && (ecls->classtype & 1) != 0 )
        b->faceCount = 8;

    b->faces = Face_Alloc_R( b->faceCount );

    for ( int i = 0; i < b->faceCount; ++i )
    {
        face_t *f = &b->faces[i];
        // copy planepts block (36 bytes) to f->mtldef[0] area
        memcpy( &f->mtldef[0], planeptsSrc, sizeof(MaterialDef) );
        // clear certain flags then set packed color
        *(int *)&f->contents &= 0xF7FFDF7B;
        f->packedColor = col.packed;
    }
    Brush_SetDefaultMaterials( b );
    return b;
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_Create  (0x475300) — fills brush_t face planepts from mins/maxs
// Standard 6-face box; 8-face cylinder if eclass has classtype bit 0.
// ────────────────────────────────────────────────────────────────────────────
void Brush_Create( float *mins, float *maxs, brush_t *b, eclass_t *ecls )
{
    for ( int i = 0; i < 3; ++i )
    {
        if ( mins[i] > maxs[i] )
            Com_Error( ERR_FATAL, "Brush_InitSolid: backwards" );
    }

    face_t *faces = b->faces;

    if ( ecls && (ecls->classtype & 1) != 0 )
    {
        // 8-sided cylinder: 4 top + 4 bottom faces
        float cx = (mins[0] + maxs[0]) * 0.5f;
        float cy = (mins[1] + maxs[1]) * 0.5f;
        float cz = (mins[2] + maxs[2]) * 0.5f;

        // Corner vertices on the top plane at cz
        float vx[4] = { mins[0], mins[0], maxs[0], maxs[0] };
        float vy[4] = { mins[1], maxs[1], maxs[1], mins[1] };

        for ( int i = 0; i < 4; ++i )
        {
            face_t *f = &faces[i];
            f->planepts[0][0] = cx; f->planepts[0][1] = cy; f->planepts[0][2] = maxs[2];
            f->planepts[1][0] = vx[i];   f->planepts[1][1] = vy[i];   f->planepts[1][2] = cz;
            int ni = (i + 1) & 3;
            f->planepts[2][0] = vx[ni];  f->planepts[2][1] = vy[ni];  f->planepts[2][2] = cz;
        }
        for ( int i = 0; i < 4; ++i )
        {
            face_t *f = &faces[4 + i];
            f->planepts[0][0] = cx; f->planepts[0][1] = cy; f->planepts[0][2] = mins[2];
            int ni = (i + 1) & 3;
            f->planepts[1][0] = vx[ni];  f->planepts[1][1] = vy[ni];  f->planepts[1][2] = cz;
            f->planepts[2][0] = vx[i];   f->planepts[2][1] = vy[i];   f->planepts[2][2] = cz;
        }
    }
    else
    {
        // Standard 6-face box:
        // face 0: -Z (z = mins.z)
        faces[0].planepts[0][0] = maxs[0]; faces[0].planepts[0][1] = maxs[1]; faces[0].planepts[0][2] = mins[2];
        faces[0].planepts[1][0] = mins[0]; faces[0].planepts[1][1] = maxs[1]; faces[0].planepts[1][2] = mins[2];
        faces[0].planepts[2][0] = mins[0]; faces[0].planepts[2][1] = mins[1]; faces[0].planepts[2][2] = mins[2];
        // face 1: +Z (z = maxs.z)
        faces[1].planepts[0][0] = mins[0]; faces[1].planepts[0][1] = mins[1]; faces[1].planepts[0][2] = maxs[2];
        faces[1].planepts[1][0] = mins[0]; faces[1].planepts[1][1] = maxs[1]; faces[1].planepts[1][2] = maxs[2];
        faces[1].planepts[2][0] = maxs[0]; faces[1].planepts[2][1] = maxs[1]; faces[1].planepts[2][2] = maxs[2];
        // face 2
        faces[2].planepts[0][0] = mins[0]; faces[2].planepts[0][1] = mins[1]; faces[2].planepts[0][2] = maxs[2];
        faces[2].planepts[1][0] = maxs[0]; faces[2].planepts[1][1] = mins[1]; faces[2].planepts[1][2] = maxs[2];
        faces[2].planepts[2][0] = maxs[0]; faces[2].planepts[2][1] = mins[1]; faces[2].planepts[2][2] = mins[2];
        // face 3: +X (x = maxs.x).
        faces[3].planepts[0][0] = maxs[0]; faces[3].planepts[0][1] = mins[1]; faces[3].planepts[0][2] = maxs[2];
        faces[3].planepts[1][0] = maxs[0]; faces[3].planepts[1][1] = maxs[1]; faces[3].planepts[1][2] = maxs[2];
        faces[3].planepts[2][0] = maxs[0]; faces[3].planepts[2][1] = maxs[1]; faces[3].planepts[2][2] = mins[2];
        // face 4: +Y (y = maxs.y).
        faces[4].planepts[0][0] = maxs[0]; faces[4].planepts[0][1] = maxs[1]; faces[4].planepts[0][2] = maxs[2];
        faces[4].planepts[1][0] = mins[0]; faces[4].planepts[1][1] = maxs[1]; faces[4].planepts[1][2] = maxs[2];
        faces[4].planepts[2][0] = mins[0]; faces[4].planepts[2][1] = maxs[1]; faces[4].planepts[2][2] = mins[2];
        // face 5: -X (x = mins.x).
        faces[5].planepts[0][0] = mins[0]; faces[5].planepts[0][1] = maxs[1]; faces[5].planepts[0][2] = maxs[2];
        faces[5].planepts[1][0] = mins[0]; faces[5].planepts[1][1] = mins[1]; faces[5].planepts[1][2] = maxs[2];
        faces[5].planepts[2][0] = mins[0]; faces[5].planepts[2][1] = mins[1]; faces[5].planepts[2][2] = mins[2];
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Entity_LinkBrush  (0x475730) — combined link/unlink of selbrush_t into entity's
// brush list (ownerNext/ownerPrev chain). Pass entity=nullptr to unlink only.
// ────────────────────────────────────────────────────────────────────────────
static void Entity_LinkBrush( entity_s *entity, selbrush_t *b )
{
    iassert( b );   // brush.cpp:2319

    if ( b->owner )
    {
        iassert( b->ownerNext );   // brush.cpp:2323
        iassert( b->ownerPrev );   // brush.cpp:2324
        b->ownerNext->ownerPrev = b->ownerPrev;
        b->ownerPrev->ownerNext = b->ownerNext;
        b->ownerNext = nullptr;
        b->ownerPrev = nullptr;
        b->owner     = nullptr;
    }
    else
    {
        iassert( !b->ownerNext );   // brush.cpp:2334
        iassert( !b->ownerPrev );   // brush.cpp:2335
    }

    iassert( b->owner == NULL );   // brush.cpp:2338
    iassert( b->ownerNext == NULL );   // brush.cpp:2339
    iassert( b->ownerPrev == NULL );   // brush.cpp:2340

    if ( entity )
    {
        b->owner     = entity;
        b->ownerNext = entity->brushes.ownerNext;
        entity->brushes.ownerNext->ownerPrev = b;
        entity->brushes.ownerNext = b;
        b->ownerPrev = (selbrush_t *)&entity->brushes;
    }
}

// Entity_LinkBrush_0_extern — external bridge to the file-static Entity_LinkBrush
// above, so entity.cpp can relink a selbrush_t INSTANCE's owner-chain into a new owner
// instance (Select_Ungroup's instance relink; Entity_Create's brush-entity / merge
// reparent paths).  The binary returned the entity; the value is unused at every call
// site, so this returns the instance.
selbrush_t *Entity_LinkBrush_0_extern( entity_s *e, entity_brush_s *b )
{
    Entity_LinkBrush( e, (selbrush_t *)b );
    return (selbrush_t *)b;
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_SetLayerString  (0x4758A0) — replaces b->parent_layer_string
// ────────────────────────────────────────────────────────────────────────────
void Brush_SetLayerString( brush_t *b, const char *str )
{
    iassert( b );   // brush.cpp:2354
    if ( b->parent_layer_string )
        j__free_0( b->parent_layer_string );
    b->parent_layer_string = AllocMaterialString( str );
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_SetInstanceLayerString  (0x4758E0) — sets the def's layer string and
// clears the instance's xx7 flag.
// ────────────────────────────────────────────────────────────────────────────
void Brush_SetInstanceLayerString( selbrush_t *b, const char *str )
{
    iassert( b );   // brush.cpp:2365
    // The binary inlines Brush_SetLayerString(b->def, str) here — its brush.cpp:2354
    // "b" assert is that inlined def-null check.
    Brush_SetLayerString( b->def, str );
    b->xx7 = 0;
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_AddToList  (0x475980) — allocates a new selbrush_t INSTANCE for the
// given brush_t DEF, increments def->refCount, sets version/patch, and links
// the instance into owner's entity brush list.
// Returns the new selbrush_t instance.
// ────────────────────────────────────────────────────────────────────────────
selbrush_t *Brush_AddToList( brush_t *def, entity_s *owner )
{
    // The ONE allocator for a selbrush_t, so it is the exact signal that the recorded
    // caster walk has changed shape.
    KiwiWalkCache_MarkStructural();
    KiwiShadowCache_Invalidate();
    iassert( def );     // brush.cpp:2384
    iassert( owner );   // brush.cpp:2385
    iassert( def->owner == owner->def );   // brush.cpp:2386

    selbrush_t *b = (selbrush_t *)operator new( sizeof(selbrush_t) );
    memset( b, 0, sizeof(selbrush_t) );
    b->def = (brush_t *)def;
    b->version = (short)(def->version - 1);
    b->cullFlag = true;
    ++def->refCount;

    if ( def->patch )
    {
        patch_t *patchInst = Patch_AllocInstance( def->patch );
        b->patch = patchInst;
        iassert( b->patch->def == b->def->patch );   // brush.cpp:2397
    }

    Entity_LinkBrush( owner, b );

    iassert( b->owner == owner );   // brush.cpp:2401
    iassert( b->owner->def == b->def->owner );   // brush.cpp:2402

    return b;
}

static void PhysParsed_Forget( const brush_t *b );

// ────────────────────────────────────────────────────────────────────────────
// Brush_Free_R  (0x475af0) — frees a brush_t DEF (must have refCount == 0)
// ────────────────────────────────────────────────────────────────────────────
void Brush_Free_R( brush_t *def )
{
    iassert( def );   // brush.cpp:2422
    vassert( (def->refCount == 0), "(def->refCount) = %i", def->refCount );   // brush.cpp:2423

    PhysParsed_Forget( def );   // KIWI
    extern void KiwiVis_DefFreed( const brush_t *def );   // kiwi_visibility.h
    KiwiVis_DefFreed( def );    // KIWI: a red-eye lock must not land on a new brush here
    if ( def->patch )
        Patch_Delete( def->patch );
    if ( def->faces )
        Face_Free( def->faceCount, def->faces );
    if ( def->parent_layer_string )
        j__free_0( def->parent_layer_string );
    iassert( def->onext == NULL );   // brush.cpp:2434
    operator delete( def );
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_Clone  (0x475d20) — deep-copies a brush_t DEF into a new brush_t DEF.
// If the source has a patch, duplicates it (returns the patch's symbiont instance).
// It reads the def's OWN patch / owner / numberId / faceCount, and copies numberId,
// NOT version.
// ────────────────────────────────────────────────────────────────────────────
brush_t *Brush_Clone( brush_t *def )
{
    if ( def->patch )
    {
        patchMesh_t *dup = Patch_Duplicate( (patchMesh_t *)(intptr_t)def->patch );
        Entity_UnlinkBrush( (brush_t *)dup->pSymbiot );
        return (brush_t *)dup->pSymbiot;
    }

    brush_t *b = (brush_t *)operator new( sizeof(brush_t) );
    memset( b, 0, sizeof(brush_t) );
    iassert( b );   // brush.cpp:2354

    if ( b->parent_layer_string )
        j__free_0( b->parent_layer_string );

    size_t layerLen = strlen( g_activeLayer_string );
    void *layerCopy = operator new( layerLen + 1 );
    memcpy( layerCopy, g_activeLayer_string, layerLen + 1 );
    b->parent_layer_string = (char *)layerCopy;

    b->owner    = def->owner;
    b->numberId = def->numberId;
    b->faceCount = def->faceCount;
    b->faces = Face_Alloc_R( b->faceCount );

    for ( int i = 0; i < b->faceCount; ++i )
    {
        face_t *dst  = &b->faces[i];
        face_t *srcc = &def->faces[i];
        // copy the mtldef[0..2] block
        memcpy( &dst->mtldef[0], &srcc->mtldef[0], 3 * sizeof(MaterialDef) );
        // copy mtldef[3]
        dst->mtldef[3] = srcc->mtldef[3];
        // copy packed color and flags verbatim
        dst->packedColor = srcc->packedColor;
        dst->contents    = srcc->contents;
        dst->toolflags   = srcc->toolflags;
        // copy planepts
        memcpy( dst->planepts, srcc->planepts, 0x24u );
    }
    return b;
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_Deselect_Helper  (0x476330) — clears BRUSHFLAG_SELECTED and updates
// selection counters. Called from Brush_RemoveFromList.
// ────────────────────────────────────────────────────────────────────────────
void Brush_Deselect_Helper( selbrush_t *b )
{
    iassert( b->brushFlags & BRUSHFLAG_SELECTED );   // brush.cpp:2676

    b->brushFlags &= ~0x80u;

    // eclass / modelClass / version live on the entity DEF, NOT the instance (which has
    // no eclass field); read them all via owner->def.  Reading them off the instance
    // walks past its allocation and faults when the adjacent heap block is freed — the
    // delete-path UAF. (entity_s_def == entity_s typedef.)
    entity_s *eDef = (entity_s *)b->owner->def;

    if ( b->brushFlags & 0x100 )
    {
        b->brushFlags &= ~0x100u;
        eDef->modelClass = nullptr;
        ++*(unsigned __int16 *)&eDef->version_prob_wrong;   // the def's 16-bit model version, NOT version
        b->def->unk01 = 0;
        Entity_RebuildBounds( eDef );
    }

    --g_qeglobals.d_select_count;

    if ( b->owner == world_entity )
    {
        --g_qeglobals.d_select_info.numBrushesAndPatches;
        if ( b->patch )
            --g_qeglobals.d_select_info.numBrushes;
        else
            --g_qeglobals.d_select_info.numPatches;
    }
    else
    {
        entity_s *ent = b->owner;
        if ( eDef->eclass->fixedsize )
        {
            --g_qeglobals.d_select_info.numFixedSize;
        }
        else if ( b->patch )
        {
            --g_qeglobals.d_select_info.numBrushes;
        }
        else
        {
            --g_qeglobals.d_select_info.numPatches;
        }

        // check if any other selected brush in same entity still has SELECTED flag
        int found = 0;
        for ( selbrush_t *v = selected_brushes.next;
              v != &selected_brushes; v = v->next )
        {
            if ( v->owner == ent && (v->brushFlags & 0x80) != 0 )
            {
                found = 1;
                break;
            }
        }
        if ( !found )
        {
            ent->mapLayer = (char *)((intptr_t)ent->mapLayer & ~0x80);
            --g_qeglobals.d_select_info.numEntWithFlag;
        }
    }

    iassert( g_qeglobals.d_select_count == (g_qeglobals.d_select_info.numBrushes
             + g_qeglobals.d_select_info.numPatches
             + g_qeglobals.d_select_info.numFixedSize) );   // brush.cpp:2720
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_Select_Helper  (0x476470) — sets BRUSHFLAG_SELECTED and updates counters
// Called from Brush_AddToList2 BEFORE linking into selected_brushes.
// ────────────────────────────────────────────────────────────────────────────
void Brush_Select_Helper( selbrush_t *b )
{
    iassert( !(b->brushFlags & BRUSHFLAG_SELECTED) );   // brush.cpp:2726

    b->brushFlags |= 0x80u;

    // eclass / modelClass / version live on the entity DEF (via owner->def),
    // NOT the instance — see the Brush_Deselect_Helper note above.
    entity_s *eDef = (entity_s *)b->owner->def;

    if ( g_qeglobals.w_cyclePreviewMode )
    {
        eclass_t *cls = eDef->eclass;
        if ( cls && cls->cycleModelName[(ushort)g_qeglobals.w_cyclePreviewMode] )
        {
            b->brushFlags |= 0x100u;
            eDef->modelClass = nullptr;
            ++*(unsigned __int16 *)&eDef->version_prob_wrong;   // the def's 16-bit model version, NOT version
            b->def->unk01 = 0;
        }
    }

    ++g_qeglobals.d_select_count;

    if ( b->owner == world_entity )
    {
        ++g_qeglobals.d_select_info.numBrushesAndPatches;
        if ( b->patch )
            ++g_qeglobals.d_select_info.numBrushes;
        else
            ++g_qeglobals.d_select_info.numPatches;
    }
    else
    {
        entity_s *ent = b->owner;
        if ( eDef->eclass->fixedsize )
        {
            ++g_qeglobals.d_select_info.numFixedSize;
        }
        else if ( b->patch )
        {
            ++g_qeglobals.d_select_info.numBrushes;
        }
        else
        {
            ++g_qeglobals.d_select_info.numPatches;
        }

        if ( ((intptr_t)ent->mapLayer & 0x80) == 0 )
        {
            ent->mapLayer = (char *)((intptr_t)ent->mapLayer | 0x80);
            ++g_qeglobals.d_select_info.numEntWithFlag;
        }
    }

    iassert( g_qeglobals.d_select_count == (g_qeglobals.d_select_info.numBrushes
             + g_qeglobals.d_select_info.numPatches
             + g_qeglobals.d_select_info.numFixedSize) );   // brush.cpp:2758
}

// KIWI-UX: the typed selection core's legacy-change notification (kiwi_selection.cpp).
// O(1) — these two funnels run per-brush inside bulk loops.
extern void Sel_InvalidateFromLegacy();

// ────────────────────────────────────────────────────────────────────────────
// Brush_AddToList2  (0x4765A0) — links an existing selbrush_t into the
// selected_brushes display list (tail-insertion, circular doubly-linked).
// ────────────────────────────────────────────────────────────────────────────
void Brush_AddToList2( selbrush_t *b )
{
    // selecting a brush MOVES it between the two display lists
    // the sun preview walks, so the recorded walk no longer describes them.
    KiwiWalkCache_MarkStructural();
    KiwiShadowCache_Invalidate();
    if ( b->next || b->prev )
        Com_Error( ERR_FATAL, "Brush_AddToList: allready linked" );

    if ( g_qeglobals.d_select_count < 2 )
        g_qeglobals.d_select_order[g_qeglobals.d_select_count] =
            (entity_brush_s *)b;

    Brush_Select_Helper( b );

    // tail-insertion into selected_brushes circular list
    // selected_brushes.prev = current tail; list iterates via .next
    selbrush_t *tail = selected_brushes.prev;
    b->prev = tail;
    tail->next = b;
    selected_brushes.prev = b;
    b->next = &selected_brushes;

    b->brushFlags &= ~0x1Fu;
    b->xx6 = 0;
    b->xx7 = 0;

    if ( b->patch )
        b->patch->selected = 1;

    UpdateSelection( -1, 0 );
    SurfaceInspector::UpdateSurfaceDialog();
    if ( PatchDialog_IsOpen() )
        g_PatchDialog_GetPatchInfo();

    Sel_InvalidateFromLegacy();   // KIWI-UX
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_RemoveFromList  (0x476680) — unlinks a selbrush_t from the display list
// ────────────────────────────────────────────────────────────────────────────
void Brush_RemoveFromList( selbrush_t *b )
{
    KiwiWalkCache_MarkStructural();   // see Brush_AddToList
    KiwiShadowCache_Invalidate();
    selbrush_t *next = b->next;
    selbrush_t *prev = b->prev;
    if ( !next || !prev )
        Com_Error( ERR_FATAL, "Brush_RemoveFromList: not linked" );

    int wasSelected = (b->brushFlags >> 7) & 1;
    next->prev = prev;
    prev->next = next;
    b->prev = nullptr;
    b->next = nullptr;

    if ( wasSelected )
    {
        Brush_Deselect_Helper( b );
        if ( b->patch )
            b->patch->selected = 0;
        UpdateSelection( -1, 0 );
        SurfaceInspector::UpdateSurfaceDialog();
        if ( PatchDialog_IsOpen() )
            g_PatchDialog_GetPatchInfo();

        Sel_InvalidateFromLegacy();   // KIWI-UX
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_Free  (0x475ba0) — full cleanup of a selbrush_t instance
// ────────────────────────────────────────────────────────────────────────────
void Brush_Free( selbrush_t *b )
{
    // Brush_RemoveFromList below invalidates only when the brush is still linked, and a
    // recording that outlived one of its brushes is a DANGLING POINTER.  Bump unconditionally.
    KiwiWalkCache_MarkStructural();
    KiwiShadowCache_Invalidate();
    // The head drops this brush's cached light-preview record from the camera window (the
    // binary only when the cam window existed; the record array lives in the
    // shell-agnostic camera state, so no guard is needed).
    CamWnd_RemoveLightPreview( b );

    if ( b->next )
        Brush_RemoveFromList( b );

    if ( b->patch )
        Patch_FreeInstance( b->patch );

    if ( b->faces )
        Vis_Free( b->faceCount, b->faces, b );

    Entity_LinkBrush( nullptr, b );

    iassert( b->def );   // brush.cpp:2459

    brush_t *def = b->def;
    if ( def->owner && def->onext && def->refCount == 2 )
        Entity_UnlinkBrush_def( def );

    vassert( (b->def->refCount > 0), "(b->def->refCount) = %i", b->def->refCount );   // brush.cpp:2464

    if ( --b->def->refCount == 0 )
        Brush_Free_R( b->def );

    operator delete( b );
}

// ═════════════════════════════════════════════════════════════════════════════
//  Brush winding generation — the CoD "CM" half-space pipeline.
//
//  CoD4Radiant builds a brush's face windings the collision-model way, NOT Q3R's
//  plane-chop way: enumerate every triple of face planes, keep the intersection points
//  that lie inside all other half-spaces (CM_AddSimpleBrushPoint), then per face
//  gift-wrap the points lying on that plane into a convex polygon
//  (CM_BuildBrushWindingForSide).
//
//  winding_t = { int numpoints; float p[][3]; } allocated 12*N+4 bytes; g_windingAlloc
//  tracks the live count. Plane format for the kisak math is {nx,ny,nz,dist}.
// ═════════════════════════════════════════════════════════════════════════════

// These editor CM_* names collide with kisak's collision-model CM_* in qcommon.h
// (homonyms with different signatures — the editor brush.cpp set vs the engine cm_*.cpp
// set). Wrap the editor pipeline in a namespace so both coexist. Brush_BuildWindings
// qualifies its calls with `edwind::`.
namespace edwind {

// 0x470a90  CM_PointInList — is `pt` already in xyzList (within 0.1²·3 per-axis)?
static int CM_PointInList( const float *xyzList, int count, const float *pt )
{
    for ( int i = 0; i < count; ++i )
    {
        const float *p = &xyzList[3 * i];
        float dx = p[0] - pt[0], dy = p[1] - pt[1], dz = p[2] - pt[2];
        if ( dx * dx <= 0.01f && dy * dy <= 0.01f && dz * dz <= 0.01f )
            return 1;
    }
    return 0;
}

// 0x470af0  CM_GetXyzList — collect (deduped) intersection points that lie on face
// `faceIdx` into xyzList; returns the count.
// Capped at 1024 points (the binary warned, then overran).
static int CM_GetXyzList( int ptCount, int faceIdx, const BrushPt_t *brushPts, float *xyzList )
{
    int count = 0;
    for ( int i = 0; i < ptCount; ++i )
    {
        const BrushPt_t *bp = &brushPts[i];
        if ( ( faceIdx == bp->sideIndex[0] || faceIdx == bp->sideIndex[1] || faceIdx == bp->sideIndex[2] )
             && !CM_PointInList( xyzList, count, bp->xyz ) )
        {
            if ( count >= 1024 )
            {
                Com_PrintMessage( "Winding point limit (%i) exceeded on brush face", 1024 );
                break;
            }
            xyzList[3 * count + 0] = bp->xyz[0];
            xyzList[3 * count + 1] = bp->xyz[1];
            xyzList[3 * count + 2] = bp->xyz[2];
            ++count;
        }
    }
    return count;
}

// 0x470b70  CM_PickProjectionAxes — the two non-dominant axes of `normal`.
static void CM_PickProjectionAxes( const float *normal, int *axis1, int *axis2 )
{
    int dom = ( fabsf( normal[1] ) > fabsf( normal[0] ) );   // 0 or 1
    if ( fabsf( normal[2] ) > fabsf( normal[dom] ) )
        dom = 2;
    *axis1 = ( ( dom & 1 ) == 0 );      // dom0->1 dom1->0 dom2->1
    *axis2 = ( ~dom ) & 2;              // dom0->2 dom1->2 dom2->0
}

// 0x470c20  CM_AddColinearExteriorPointToWindingProjected — when the new point is
// (nearly) colinear with the best edge, extend that edge by replacing its further
// endpoint rather than inserting a new vertex.
static void CM_AddColinearExteriorPointToWindingProjected(
        winding_t *w, const float *pt, int i, int j, int index0, int index1 )
{
    float *pts = &w->p[0][0];

    iassert( w->pts[index0][i] != w->pts[index1][i] || w->pts[index0][j] != w->pts[index1][j] );   // brush.cpp:572

    float di = pts[3 * index1 + i] - pts[3 * index0 + i];
    float dj = pts[3 * index1 + j] - pts[3 * index0 + j];
    int   axis;
    float delta;
    if ( fabsf( dj ) > fabsf( di ) ) { delta = dj; axis = j; }
    else                             { delta = di; axis = i; }

    float p0 = pts[3 * index0 + axis];
    float p1 = pts[3 * index1 + axis];
    int   replaceIdx;
    if ( delta <= 0.0f )
    {
        iassert( w->pts[index0][axis] > w->pts[index1][axis] );   // brush.cpp:597
        if ( p0 < pt[axis] )            replaceIdx = index0;
        else if ( p1 <= pt[axis] )      return;
        else                            replaceIdx = index1;
    }
    else
    {
        iassert( w->pts[index0][axis] < w->pts[index1][axis] );   // brush.cpp:589
        if ( p0 <= pt[axis] )
        {
            if ( p1 >= pt[axis] )       return;
            replaceIdx = index1;
        }
        else                            replaceIdx = index0;
    }
    pts[3 * replaceIdx + 0] = pt[0];
    pts[3 * replaceIdx + 1] = pt[1];
    pts[3 * replaceIdx + 2] = pt[2];
}

// 0x470df0  CM_AddExteriorPointToWindingProjected — incremental convex-hull insert
// of `pt` into the (projected) winding: find the edge `pt` is most exterior to and
// insert/extend there.
// The insert is an overlap-safe backward shift.
static void CM_AddExteriorPointToWindingProjected( const float *pt, winding_t *w, int axisU, int axisV )
{
    int n = w->numpoints;
    if ( n == 0 )
        return;

    float *pts = &w->p[0][0];
    int   bestIndex = -1;
    float minArea   = 3.4028235e38f;
    int   prev      = n - 1;
    for ( int index = 0; index < n; ++index )
    {
        const float *cur = &pts[3 * index];
        const float *prv = &pts[3 * prev];
        float area = ( cur[axisV] - pt[axisV] ) * prv[axisU]
                   + ( prv[axisV] - cur[axisV] ) * pt[axisU]
                   + ( pt[axisV]  - prv[axisV] ) * cur[axisU];
        if ( area < minArea ) { minArea = area; bestIndex = index; }
        prev = index;
    }

    if ( minArea < -0.001f )
    {
        for ( int k = n; k > bestIndex; --k )
        {
            pts[3 * k + 0] = pts[3 * ( k - 1 ) + 0];
            pts[3 * k + 1] = pts[3 * ( k - 1 ) + 1];
            pts[3 * k + 2] = pts[3 * ( k - 1 ) + 2];
        }
        pts[3 * bestIndex + 0] = pt[0];
        pts[3 * bestIndex + 1] = pt[1];
        pts[3 * bestIndex + 2] = pt[2];
        ++w->numpoints;
    }
    else if ( minArea <= 0.001f )
    {
        CM_AddColinearExteriorPointToWindingProjected(
            w, pt, axisU, axisV, ( n + bestIndex - 1 ) % n, bestIndex );
    }
}

// 0x470f20  CM_RepresentativeTriangleFromWinding — the winding triangle of largest
// area projected along `normal`; outputs its 3 vertex indices, returns the area.
static float CM_RepresentativeTriangleFromWinding(
        const winding_t *w, const float *normal, int *outI, int *outJ, int *outK )
{
    float best = 0.0f;
    *outI = 0; *outJ = 1; *outK = 2;
    const float *pts = &w->p[0][0];
    int n = w->numpoints;
    for ( int k = 2; k < n; ++k )
        for ( int j = 1; j < k; ++j )
            for ( int i = 0; i < j; ++i )
            {
                float e_kj[3] = { pts[3*k+0]-pts[3*j+0], pts[3*k+1]-pts[3*j+1], pts[3*k+2]-pts[3*j+2] };
                float e_ij[3] = { pts[3*i+0]-pts[3*j+0], pts[3*i+1]-pts[3*j+1], pts[3*i+2]-pts[3*j+2] };
                float cr[3];
                Vec3Cross( e_ij, e_kj, cr );
                float a = fabsf( normal[0]*cr[0] + normal[1]*cr[1] + normal[2]*cr[2] );
                if ( best < a ) { best = a; *outI = i; *outJ = j; *outK = k; }
            }
    return best;
}

// 0x4d7dc0  CM_ReverseWinding — a freshly-allocated winding with reversed point order.
static winding_t *CM_ReverseWinding( const winding_t *w )
{
    int n = w->numpoints;
    ++g_windingAlloc;
    winding_t *out = (winding_t *)malloc( 12 * n + 4 );
    if ( !out )
        Com_PrintMessage( "out of memory: winding_t\n" );
    const float *src = &w->p[0][0];
    float       *dst = &out->p[0][0];
    for ( int i = 0; i < n; ++i )
    {
        const float *s = &src[3 * ( n - 1 - i )];
        dst[3*i+0] = s[0]; dst[3*i+1] = s[1]; dst[3*i+2] = s[2];
    }
    out->numpoints = n;
    return out;
}

// 0x471090  CM_BuildBrushWindingForSide — convex winding for one face from the brush's
// intersection points, oriented to agree with the face plane normal.
// orientation chain wn = (p[kk]-p[ii]) x (p[jj]-p[ii]).
winding_t *CM_BuildBrushWindingForSide( int faceIdx, BrushPt_t *brushPts, brush_t *def, int ptCount )
{
    static float xyzList[1024 * 3];      // static keeps the frame small
    int xyzCount = CM_GetXyzList( ptCount, faceIdx, brushPts, xyzList );
    if ( xyzCount < 3 )
        return nullptr;

    face_t *face = &def->faces[faceIdx];
    int projAxis1, projAxis2;
    CM_PickProjectionAxes( face->plane.normal, &projAxis1, &projAxis2 );

    ++g_windingAlloc;
    winding_t *w = (winding_t *)malloc( 12 * xyzCount + 4 );
    if ( !w )
        Com_PrintMessage( "out of memory: winding_t\n" );
    float *pts = &w->p[0][0];
    pts[0]=xyzList[0]; pts[1]=xyzList[1]; pts[2]=xyzList[2];
    pts[3]=xyzList[3]; pts[4]=xyzList[4]; pts[5]=xyzList[5];
    w->numpoints = 2;

    for ( int m = 0; m < xyzCount - 2; ++m )
        CM_AddExteriorPointToWindingProjected( &xyzList[3 * ( m + 2 )], w, projAxis1, projAxis2 );

    int ii, jj, kk;
    // edwind:: qualified to suppress ADL — winding_t is global, so an unqualified call
    // would also find kisak's collision-model homonym (qcommon.h) and be ambiguous.
    float area = edwind::CM_RepresentativeTriangleFromWinding( w, face->plane.normal, &ii, &jj, &kk );
    if ( area < 0.001f )
    {
        --g_windingAlloc;
        free( w );
        return nullptr;
    }

    // Orientation: winding normal = (p[kk]-p[ii]) × (p[jj]-p[ii]) (PlaneFromPoints_Real
    // convention). Reverse the winding if it opposes the face plane normal. The cross
    // need not be normalised — only the sign of the dot matters.
    float *P  = &w->p[0][0];
    float ca[3] = { P[3*kk+0]-P[3*ii+0], P[3*kk+1]-P[3*ii+1], P[3*kk+2]-P[3*ii+2] };
    float ba[3] = { P[3*jj+0]-P[3*ii+0], P[3*jj+1]-P[3*ii+1], P[3*jj+2]-P[3*ii+2] };
    float wn[3];
    Vec3Cross( ca, ba, wn );
    float dot = face->plane.normal[0]*wn[0] + face->plane.normal[1]*wn[1] + face->plane.normal[2]*wn[2];
    if ( dot >= 0.0f )
        return w;

    winding_t *rev = edwind::CM_ReverseWinding( w );   // edwind:: — see note above
    --g_windingAlloc;
    free( w );
    return rev;
}

// 0x4779e0  CM_AddSimpleBrushPoint — record `xyz` (the intersection of the 3 planes in
// `sideIndices`) iff it lies inside every other face half-space.
static int CM_AddSimpleBrushPoint( brush_t *def, const int *sideIndices, const float *xyz,
                                   int ptCount, int numsides, BrushPt_t *brushPts )
{
    int faceCount = def->faceCount;
    for ( int i = 0; i < faceCount; ++i )
    {
        if ( i != sideIndices[0] && i != sideIndices[1] && i != sideIndices[2] )
        {
            const plane_t *pl = &def->faces[i].plane;
            float d = pl->normal[0]*xyz[0] + pl->normal[1]*xyz[1] + pl->normal[2]*xyz[2] - pl->dist;
            if ( d > 0.1f )
                return ptCount;             // outside this half-space — reject
        }
    }
    if ( ptCount == numsides )
    {
        Com_PrintMessage( "More than %i pts from plane intersections on %i-sided brush\n",
                          ptCount, faceCount );
        return ptCount;
    }
    brushPts[ptCount].xyz[0] = xyz[0];
    brushPts[ptCount].xyz[1] = xyz[1];
    brushPts[ptCount].xyz[2] = xyz[2];
    brushPts[ptCount].sideIndex[0] = sideIndices[0];
    brushPts[ptCount].sideIndex[1] = sideIndices[1];
    brushPts[ptCount].sideIndex[2] = sideIndices[2];
    return ptCount + 1;
}

// 0x470880  CM_ForEachBrushPlaneIntersection — fill brushPts[] with the brush's
// half-space vertices (every inside triple-plane intersection); returns the count.
// The binary snapped with SnapPointToIntersectingPlanes (0.25f / 0.01f).
// KIWI: the corners in double, from each face's plane points, rounded to float once.  The
// float plane (normal + dist) of a steep face 30000 out is ~0.003 off its plane points, so
// every corner drawn, snapped to or copied into a new plane carried that: a vertex G out and
// back left slivers there even with exact plane points.  Same intersection, same 0.25 / 0.001
// snap (IntersectPlanes / SnapPointToIntersectingPlanes, Banker's rounding), in double.
static void CM_FacePlaneDouble( const face_t *f, double *pl )
{
    double e1[3], e2[3];
    for ( int k = 0; k < 3; ++k )
    {
        e1[k] = (double)f->planepts[0][k] - f->planepts[1][k];   // as Face_MakePlane
        e2[k] = (double)f->planepts[2][k] - f->planepts[1][k];
    }
    pl[0] = e1[1] * e2[2] - e1[2] * e2[1];
    pl[1] = e1[2] * e2[0] - e1[0] * e2[2];
    pl[2] = e1[0] * e2[1] - e1[1] * e2[0];
    const double len = sqrt( pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2] );
    if ( len > 0.0 )
    {
        pl[0] /= len;
        pl[1] /= len;
        pl[2] /= len;
    }
    pl[3] = pl[0] * f->planepts[1][0] + pl[1] * f->planepts[1][1] + pl[2] * f->planepts[1][2];
}

static bool CM_IntersectPlanesDouble( const double *a, const double *b, const double *c, double *xyz )
{
    const double bc[3] = { b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0] };
    const double ca[3] = { c[1] * a[2] - c[2] * a[1], c[2] * a[0] - c[0] * a[2], c[0] * a[1] - c[1] * a[0] };
    const double ab[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    const double det = a[0] * bc[0] + a[1] * bc[1] + a[2] * bc[2];
    if ( fabs( det ) < EQUAL_EPSILON )
        return false;
    for ( int k = 0; k < 3; ++k )
        xyz[k] = ( a[3] * bc[k] + b[3] * ca[k] + c[3] * ab[k] ) / det;
    return true;
}

static void CM_SnapPointDouble( const double *const *planes, double *xyz, double snapGrid, double snapEpsilon )
{
    double snapped[3];
    for ( int axis = 0; axis < 3; ++axis )
    {
        const double rounded = nearbyint( xyz[axis] / snapGrid ) * snapGrid;
        snapped[axis] = fabs( rounded - xyz[axis] ) < snapEpsilon ? rounded : xyz[axis];
    }
    if ( snapped[0] == xyz[0] && snapped[1] == xyz[1] && snapped[2] == xyz[2] )
        return;
    double maxSnapError = 0.0, maxBaseError = snapEpsilon;
    for ( int p = 0; p < 3; ++p )
    {
        const double *pl = planes[p];
        maxSnapError = fmax( maxSnapError, fabs( pl[0] * snapped[0] + pl[1] * snapped[1] + pl[2] * snapped[2] - pl[3] ) );
        maxBaseError = fmax( maxBaseError, fabs( pl[0] * xyz[0] + pl[1] * xyz[1] + pl[2] * xyz[2] - pl[3] ) );
    }
    if ( maxBaseError > maxSnapError )
    {
        xyz[0] = snapped[0];
        xyz[1] = snapped[1];
        xyz[2] = snapped[2];
    }
}

int CM_ForEachBrushPlaneIntersection( brush_t *def, BrushPt_t *brushPts )
{
    int faceCount = def->faceCount;
    if ( faceCount == 2 )
        return 0;

    static std::vector<double> dplanes;   // single-threaded, as Brush_BuildWindings' brushPts
    dplanes.resize( (size_t)faceCount * 4 );
    for ( int i = 0; i < faceCount; ++i )
        CM_FacePlaneDouble( &def->faces[i], &dplanes[(size_t)i * 4] );

    int ptCount = 0;
    for ( int i = 0; i + 2 < faceCount; ++i )
    {
        for ( int j = i + 1; j + 1 < faceCount; ++j )
        {
            for ( int k = j + 1; k < faceCount; ++k )
            {
                const double *planes[3] = { &dplanes[(size_t)i * 4], &dplanes[(size_t)j * 4], &dplanes[(size_t)k * 4] };
                double dxyz[3];
                if ( CM_IntersectPlanesDouble( planes[0], planes[1], planes[2], dxyz ) )
                {
                    // KIWI (2026-10-05): snap epsilon 0.001, not the binary's 0.01.  At 0.01 a
                    // rotated trim's corner at 2576.508 was drawn, bounded and vertex-snapped
                    // at 2576.5: Mirror / R pivots drifted per use, and tools that copy
                    // winding points into new planes (extrude, bevel, edge move) built faces
                    // tilted against their neighbours.  cod4map (BrushToPoints) uses the same 0.001.
                    CM_SnapPointDouble( planes, dxyz, 0.25, 0.001 );
                    const float xyz[3] = { (float)dxyz[0], (float)dxyz[1], (float)dxyz[2] };
                    int sideIdx[3] = { i, j, k };
                    ptCount = CM_AddSimpleBrushPoint( def, sideIdx, xyz, ptCount, 1024, brushPts );
                }
            }
        }
    }
    return ptCount;
}

// 0x470a50  Brush_MakeFacePlanes — recompute every face plane from its planepts.
void Brush_MakeFacePlanes( brush_t *b )
{
    for ( int i = 0; i < b->faceCount; ++i )
        Face_MakePlane( &b->faces[i] );
}

// 0x431690  CM_CheckBrushContents — true iff the two base mtldefs' lyrMtl/radMtl handle
// pairs are identical (base-material homogeneity test). Affects def->xx1 only, not
// geometry.
bool CM_CheckBrushContents( const MaterialDef *a, const MaterialDef *b )
{
    return a->lyrMtl == b->lyrMtl && a->radMtl == b->radMtl;
}

// 0x4a8240  VectorMaxValues — expand [mins,maxs] to include point `pt`.
void VectorMaxValues( const float *pt, float *mins, float *maxs )
{
    for ( int i = 0; i < 3; ++i )
    {
        if ( mins[i] > pt[i] ) mins[i] = pt[i];
        if ( maxs[i] < pt[i] ) maxs[i] = pt[i];
    }
}

} // namespace edwind

// ─────────────────────────────────────────────────────────────────────────────
// 0x47ABE0  Brush_AccumulateWorldBounds
// Expand [mins,maxs] to enclose every winding point of brush DEF `b`, transformed
// to world space by orientation `orient`.  Used by Entity_InitPrefabInst to compute
// the placed prefab's overall bbox from each instanced brush's def windings.
// ─────────────────────────────────────────────────────────────────────────────
void Brush_AccumulateWorldBounds( const orientation_t *orient, const brush_t *b, float *mins, float *maxs )
{
    iassert( b );      // brush.cpp:4618
    iassert( orient ); // brush.cpp:4619 (the binary's "or")
    iassert( mins );   // brush.cpp:4620
    iassert( maxs );   // brush.cpp:4621

    for ( int f = 0; f < b->faceCount; ++f )
    {
        winding_t *w = b->faces[f].w;
        if ( !w )
        {
            continue;
        }
        for ( int i = 0; i < w->numpoints; ++i )
        {
            float worldPt[3];
            OrientationPosToWorldPos( worldPt, w->p[i], orient );
            edwind::VectorMaxValues( worldPt, mins, maxs );
        }
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_BuildWindings  (0x477AC0) — rebuilds all face windings for brush def
// bFull != 0: snap planepts first
// ────────────────────────────────────────────────────────────────────────────
void Brush_BuildWindings( brush_t *def, int bFull )
{

    if ( bFull )
        Brush_SnapPlanepts( def );

    edwind::Brush_MakeFacePlanes( def );

    // The brush's half-space vertices (capacity 1024 matches MAX_POINTS_ON_WINDING / the
    // CM_* caps).
    static BrushPt_t brushPts[1024];
    int ptCount = edwind::CM_ForEachBrushPlaneIntersection( def, brushPts );

    if ( ptCount < 4 )
        Com_PrintMessage( "Brush_BuildWindings: Encountered degenerate brush.\n" );

    def->contents = -1;
    def->xx1      = def->faceCount ? def->faces->mtldef : NULL;

    for ( int fi = 0; fi < def->faceCount; ++fi )
    {
        face_t *face = &def->faces[fi];   // brush.cpp:83 "f": dead inlined-accessor null-check — elided

        winding_t *w = face->w;
        if ( w )
        {
            --g_windingAlloc;
            free( w );
            face->w = nullptr;
        }
        iassert( face->w == NULL );   // brush.cpp:3422

        face->w = edwind::CM_BuildBrushWindingForSide( fi, brushPts, def, ptCount );

        qtexture_s *lyrMtl = MaterialDef_GetLayeredMaterial( face->mtldef );
        // contents ANDs the layered material's in_use (not its name field).
        int in_use = lyrMtl ? lyrMtl->in_use : 0;

        def->contents &= in_use;

        // CM_CheckBrushContents compares the base layer's lyrMtl/radMtl handle pair
        // (&mtldef[0]), NOT face->contents.  xx1 itself was set to &faces[0].mtldef
        // above, so both operands must address the mtldef block.
        MaterialDef *xx1 = def->xx1;
        if ( xx1 && !edwind::CM_CheckBrushContents( face->mtldef, xx1 ) )
        {
            def->xx1 = 0;
        }
    }

    // compute AABB from intersection points
    def->mins[0] = def->mins[1] = def->mins[2] =  131072.0f;
    def->maxs[0] = def->maxs[1] = def->maxs[2] = -131072.0f;

    for ( int i = 0; i < ptCount; ++i )
        edwind::VectorMaxValues( brushPts[i].xyz, def->mins, def->maxs );
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_BuildFaceVis / Brush_EnsureFaceVis are Brush_MakeFaceVisuals /
// Brush_CheckBuildFaceVis at the top of this file (one faceVis lifecycle).
// ────────────────────────────────────────────────────────────────────────────

// ────────────────────────────────────────────────────────────────────────────
// Brush_InvalidateVis  (0x478340) — frees faceVis cache and forces rebuild
// Returns b->def.
// ────────────────────────────────────────────────────────────────────────────
brush_t *Brush_InvalidateVis( selbrush_t *b )
{
    iassert( b );        // brush.cpp:3596
    iassert( b->def );   // brush.cpp:3597

    if ( b->faces )
    {
        j__free_0( b->faces );
        b->faces = nullptr;
        b->faceCount = 0;
    }

    if ( b->patch )
        Patch_Fill_ResetVisuals( b->patch );

    b->version = (short)(b->def->version - 1);
    return b->def;
}

// ────────────────────────────────────────────────────────────────────────────
// Brush_Build  (0x4786D0) — full rebuild: create face planepts + windings
// ────────────────────────────────────────────────────────────────────────────
void Brush_Build( brush_t *b, float *mins, float *maxs )
{
    iassert( b );                  // brush.cpp:3698
    iassert( b->owner );           // brush.cpp:3699
    iassert( b->owner->eclass );   // brush.cpp:3700

    // Brush_Create( mins, maxs, ... ) — the lower bound first; swapping them trips
    // Brush_InitSolid's "backwards" FATAL.
    Brush_Create( mins, maxs, b, b->owner->eclass );
    Brush_BuildWindings( b, 1 );

    if ( g_qeglobals.d_select_mode == sel_vertex ||
         g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();

    ++b->version;
}

// KIWI: the tail every def edit open-codes (the binary inlines it): windings, vertex
// handles, the modified flag, the version stamp.
void Brush_RebuildAfterEdit( brush_t *def )
{
    Brush_BuildWindings( def, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++def->version;
}

// ════════════════════════════════════════════════════════════════════════════
//  Brush_RebuildBrush  (0x438760) — rebuilds a PATCH SYMBIONT's 6-face bounding box
//  brush from new mins/maxs after the control grid changed (insert/remove row/col).
//  Called by Patch_Rebuild (pmesh.cpp).
//   1. Round each axis' mins/maxs (floor(2x+0.5)*0.5 = round to nearest 0.5; the
//      operands are already grid-snapped, so this lands on integers).  An axis that
//      collapses to a point is expanded by ±4 — Patch_CalcBounds' degenerate guard.
//   2. PRESERVE the old face[0]'s material block (3 MaterialDef layers = 0x6C bytes)
//      and apply it to every new face; contents/toolflags come from the patch, or from
//      the old face[0] when there is no patch.
//   3. Free the old faces, alloc 6 fresh ones, write the box, Brush_BuildWindings.
//  The binary inlined the box winding from 4 vertical edges; this reuses Brush_Create's
//  box layout — the resulting axis-aligned box is identical, and the symbiont bbox is
//  never serialized (Brush_Write dispatches patches to Patch_Write), so only its
//  bounds/windings matter.
void Brush_RebuildBrush( brush_t *b, float *mins, float *maxs )
{
    // 1. round + expand-degenerate (floor(0.5 + x + x)*0.5 per axis)
    // KIWI (2026-10-05, precision gauntlet): noise-only, as Brush_SnapPlanepts.  These bounds
    // are every patch's selection bounds - Select_GetTrueMid, the R / S / Mirror pivot - and
    // rounding them to 0.5 put an off-grid patch's pivot up to 0.25 off: 4 x R 90 drifted the
    // control points 0.25, R X 90 / -90 0.56.
    double scale = 0.0;                    // the box's magnitude (kiwi_floatsnap.h)
    for ( int i = 0; i < 3; ++i )
    {
        if ( fabs( (double)mins[i] ) > scale ) scale = fabs( (double)mins[i] );
        if ( fabs( (double)maxs[i] ) > scale ) scale = fabs( (double)maxs[i] );
    }
    for ( int i = 0; i < 3; ++i )
    {
        float lo = KiwiSnap_NoiseHalf( mins[i], scale );
        float hi = KiwiSnap_NoiseHalf( maxs[i], scale );
        mins[i] = lo;
        maxs[i] = hi;
        if ( (int)lo == (int)hi )
        {
            mins[i] = lo - 4.0f;
            maxs[i] = hi + 4.0f;
        }
    }

    face_t *oldFaces = b->faces;
    if ( !oldFaces )
        return;                              // no faces → nothing to rebuild

    // 2. snapshot the old material block (3 layers = 0x6C bytes) + contents/flags
    char savedMtl[0x6C];
    memcpy( savedMtl, oldFaces->mtldef, sizeof( savedMtl ) );

    int contents, toolflags;
    if ( b->patch )
    {
        contents  = b->patch->contents;
        toolflags = b->patch->flags;
    }
    else
    {
        contents  = oldFaces->contents;
        toolflags = oldFaces->toolflags;
    }

    Face_Free( b->faceCount, oldFaces );
    b->faces = nullptr;

    // backwards guard
    for ( int i = 0; i < 3; ++i )
    {
        if ( mins[i] > (double)maxs[i] )
            Com_Error( ERR_FATAL, "Brush_RebuildBrush: backwards" );
    }

    // 3. alloc 6 fresh faces, restore material + contents/flags, build the box
    b->faceCount   = 6;
    b->faces = Face_Alloc_R( 6 );
    for ( int i = 0; i < 6; ++i )
    {
        memcpy( b->faces[i].mtldef, savedMtl, sizeof( savedMtl ) );
        b->faces[i].contents  = contents;
        b->faces[i].toolflags = toolflags;
    }
    Brush_Create( mins, maxs, b, nullptr );  // writes the 6 box-face planepts (eclass=null → box)

    Brush_BuildWindings( b, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex ||
         g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++b->version;
}

// ─────────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────────
// CXYWnd::OnSelectionAddToActiveLayer  (0x466930) — context-menu command "Add
// selection to active layer" (command ID 0x88B9 = 35001, an ON_COMMAND on the CXYWnd
// map).  Walks selected_brushes; per instance replaces def->parent_layer_string with
// g_activeLayer_string and zeroes the instance's xx7 faceVis flag.
// The binary gates the loop on this->contextmenu_sub_hwnd; the port routes the command
// through CMainFrame and has no such field, so that (no-op) gate is dropped.  Asserts
// brush.cpp:2365 "b" (null instance) and 2354 "b" (null def).
// ─────────────────────────────────────────────────────────────────────────────
void CXYWnd_OnSelectionAddToActiveLayer()
{
    for ( selbrush_t *i = selected_brushes.next; i != &selected_brushes; i = i->next )
    {
        // (the binary inlines Brush_SetInstanceLayerString here; the helper carries the
        // brush.cpp:2365/2354 checks itself)
        Brush_SetInstanceLayerString( i, g_activeLayer_string );   // free old + dup new + xx7=0
    }
}

// qtexture_s is defined in qe3.h.

// ─── R_Ed_FreeVertices (r_ed_vertbuf.cpp): releases a vertex buffer / material handle
extern char R_Ed_FreeVertices( Material *material, int vertCount, int vertHandle );

// ─── Face_FullClone (0x4703a0): deep copies one face entry ─────────────────────
// Undo_AddBrush clones the dragged brush def, and the clone copies every face through
// here: copy the face fields, then DEEP-CLONE the winding rather than alias it.
// unk02 is intentionally NOT copied (left zeroed by Face_Alloc_R).
extern winding_t *Winding_Clone( winding_t *w );   // winding.cpp (0x4d7d60)
void Face_FullClone( const face_t *src, face_t *dst )
{
    memcpy( dst->mtldef, src->mtldef, 4 * sizeof(MaterialDef) );
    dst->packedColor = src->packedColor;
    dst->contents = src->contents;
    dst->toolflags = src->toolflags;
    memcpy( dst->planepts, src->planepts, 3 * sizeof(vec3_t) );
    dst->plane = src->plane;
    dst->unk03 = src->unk03;
    dst->unk04 = src->unk04;
    dst->w = src->w ? Winding_Clone( src->w ) : NULL;
}

// ─── forward declarations for the parse sub-functions ──────────────────────────
static brush_t *Brush_ParsePhysicsBox( const char **text );
static brush_t *Brush_ParsePhysCylinder( const char **text );
// Write sub-functions (WriteWriter_t defined near Brush_Write further below)
// — forward declared there as static, prototypes repeated after typedef.

// materialdef.cpp helpers
extern texdef_sub_t *MaterialDef_FindTexdef( MaterialDef *mtlDef, int layerHandle );
namespace LayerMat {
    extern int GetCurrentLayer( MaterialDef *def );
}

// surface/texture helpers
extern void sub_4A47D0( float *outS, const float *inVecs );
extern void Face_GetLayerColor( MaterialDef *mtlDef, const face_t *faceDef, int visIdx, int *outData );
extern int  sub_51D1D0( int material, int vertCount, int positions, int sVec,
                         int tVec, float dummy, int normalInfo, int uvData );

// math helpers
extern void MatrixInverse44( const void *src, void *dst );
extern void Face_TexPlaneGradient( float *normalOut, const float *planes, float *gradOut );

// ─── parsing (real engine parser — universal/q_parse.cpp) ───────────────────
// Brushes parse through the engine's q_parse.cpp, NOT a Radiant-local parser.
// parseInfo_t / ParseThreadInfo / Com_Parse / Com_ParseExt / Com_ParseOnLine /
// Com_MatchToken / Com_ParseFloat / Com_UngetToken / Com_SetSpaceDelimited all live
// there. The parse global g_parse is q_parse.cpp's `ParseThreadInfo g_parse[4]`; this
// file touches it only via the API.
#include <universal/q_parse.h>

extern void   Error( const char *fmt, ... );

// Patch_Write (pmesh.cpp, 0x4458f0) declared near Brush_Write where WriteWriter_t exists.
extern int  *contents;   // content table for MapLoad_ParseBrush_Content
extern int  *toolflags;  // toolflags table

// Map_ParseEntityLayerKey (layers.cpp, 0x42F8A0) - reads a `<key> "<value>"` line with a default.
extern void Map_ParseEntityLayerKey( const char **text, const char *defVal, char *outBuf, const char *key );
extern int  Map_ParseContentsLine( const char **text );
extern int  Map_ParseToolFlagsLine( const char **text );
extern char *Mtl_NormalizeName( char *name );
extern float Float_CleanLowBits( float f, int threshold );  // snap low 12 mantissa bits

// brush geometry helpers
// Brush_MakeSided is defined further down in this file (void, char snap).
// 0x4A59C0 (com_math.cpp:1399) is the engine's ClosestApproachOfTwoLines — the
// radiant binary carried its own copy; this calls the engine fn (com_math.cpp).
extern void __cdecl ClosestApproachOfTwoLines( const float *p1, const float *dir1,
                                               const float *p2, const float *dir2,
                                               float *s, float *t );

// Patch_ParseMesh (0x444ac0) — parse a mesh/curve block into a patchMesh_t + symbiont
// brush (pmesh.cpp).
extern brush_t *Patch_ParseMesh( const char **text, int version, int isMesh );

// ════════════════════════════════════════════════════════════════════════════
//  4. Visuals_VisArray  (0x46f590)
//  Releases all render handles in the faceVis array and frees the array.
// ════════════════════════════════════════════════════════════════════════════
void Visuals_VisArray( faceVis_s *f )
{
    if ( f->visCount )
    {
        iassert( f->visArray != NULL );   // brush.cpp:67
        for ( int i = 0; i < f->visCount; ++i )
        {
            faceVisuals_s *visuals = &f->visArray[i];
            iassert( visuals->mtlHandle );    // brush.cpp:71
            iassert( visuals->vertHandle );   // brush.cpp:72
            R_Ed_FreeVertices( visuals->mtlHandle, f->vertcount, visuals->vertHandle );
        }
        j__free_0( f->visArray );
        f->visArray  = nullptr;
        f->visCount  = 0;
    }
    else if ( f->visArray )
    {
        iassert( f->visArray == NULL );   // brush.cpp:63
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  The editor surface-cache faceVis builder.
//
//  Visuals_InitFaceVis (0x46F7A0) builds, per face per material layer, a
//  GfxWorldVertex run in the per-material D3D9 VB pool (Editor_VB_Upload) and records
//  {Material*, vbHandle} in faceVis_s.visArray[layer].  The per-vertex math (world
//  position/normal, tangent/binormal, texdef texcoords, per-face colour) is factored
//  into Face_BuildLayerGeom so the camera surf-cache draw (Cam_DrawFaceCached) and the
//  immediate draw (Cam_DrawFace) emit bit-identical geometry.
//
//  Chain:
//    Ed_Normal_Calc (0x459A60) + Ed_baseaxis — TextureAxisFromPlane.
//    Ed_texturevecs (0x459BA0)               — base S/T axis vectors + s/t indices.
//    Face_MoveTexture (0x45A1C0)             — scale/shift/rotate → 2x4 tex matrix.
//    sub_4A47D0                              — Vec3Normalize (out-of-place).
//    Ed_SinCos (0x4AABC0)                    — degrees → sin/cos, exact 0/90/180/270.
//    MaterialDef_FindTexdef (0x45D360)         — find the layer's texdef block by key.
//    Face_GetLayerColor (0x46F6C0)                   — per-layer face colour (colorTint·entity, else face colour).
//    Ed_PackColor (0x46F410)                 — float[4] → packed RGBA bytes.
//    Ed_Material_GetColorTint (0x51AD50)     — Material "colorTint" constant lookup.
// ════════════════════════════════════════════════════════════════════════════

// materialdef.cpp accessors (real; radMtl ⇒ single layer in the GUI).
extern int   MaterialDef_04( MaterialDef *m );
extern int   MaterialDef_11( MaterialDef *m );
extern int   MaterialDef_13( int visIndex, MaterialDef *m );
extern Material *MaterialDef_14( unsigned int visIndex, MaterialDef *m );

// r_ed_vertbuf.cpp — upload one layer's verts; returns the packed VB handle.
extern unsigned int Editor_VB_Upload( Material *material, int vertCount,
    const float *xyz, const float *tangent, const float *binormal,
    const float *normal, const float *texCoord, const float *color );
// KIWI: refill a live run of the same material and vertex count in place.
extern void Editor_VB_Rewrite( unsigned int handle, int vertCount,
    const float *xyz, const float *tangent, const float *binormal,
    const float *normal, const float *texCoord, const float *color );

// renderer string hash (r_utils.cpp) — for the material constant lookup.
extern unsigned int R_HashString( const char *string );

// ── texturevecs cluster — lives in texturevecs.cpp (the binary's common/texturevecs.cpp
//    TU; its asserts name that file).  Externs for the brush.cpp users: ──
extern void Ed_Normal_Calc( const float *normal, float *xv, float *yv );   // 0x459A60

// ════════════════════════════════════════════════════════════════════════════
//  Patch texture-PICK cluster (Drag_Begin middle-button, when the hit brush is a
//  patch).  Five functions:
//
//    Ed_Normal_Calc        (0x459A60)  — TextureAxisFromPlane (already above; reused).
//    Ed_Patch_AffineInverse(0x43CEE0)  — 3x3 inverse of [[u_i, v_i, 1]] (Cramer).
//    Ed_Patch_PlanarTest   (0x4382D0)  — average per-quad normals over the curveDef
//                                        tessellated mesh; verify every CONTROL point
//                                        lies within ±1 of that plane.  out = {N[3],dist}.
//    Ed_Patch_PickSpan     (0x4380B0)  — pick the first of 8 candidate (col,row) index
//                                        triples whose 3 control points are non-collinear.
//    Ed_Patch_GetTexdef    (0x44B620)  — the whole pick: build the plane basis, project
//                                        3 control pts to (u,v)+(s,t), solve the affine
//                                        (u,v)->(s,t), recover size/shift/rotate, then a
//                                        built-in verification loop over ALL control pts.
//
//  Ed_Patch_GetTexdef has NO success path — every exit returns 0.  The texdef is the
//  SIDE EFFECT: size[2]/
//  shift[2]/rotate are written iff the affine is valid AND all control points reproduce
//  within tol AND the recovered rotate rounds to an integer degree within 0.001.  The
//  consumer (Drag_Begin, drag.cpp) discards the return value.
//
//  FP constants:
//    0.001 (float)       (Ed_Patch_PlanarTest normalize-length guard;
//                         Ed_Patch_GetTexdef rotate roundtrip-check tolerance)
//    0.001 (double)      (verify-loop per-texel tolerance)
//    57.2957763671875    (radians -> degrees, 180/pi)
//    0.5                 (floorf(x+0.5) rotate-rounding offset)
//    -1.0                (Ed_Patch_PlanarTest dot-vs-(-1) planar tolerance, ±1 band)
// ════════════════════════════════════════════════════════════════════════════

// 0x43CEE0  Ed_Patch_AffineInverse — inverse of M = [[xs[i], ys[i], 1]] (i=0..2),
// Cramer's rule.  xs = the 3 u's, ys = the 3 v's (in the tex-plane basis).  Returns 0
// (degenerate) if |det| < 0.001, else fills inv[9] with the inverse and returns 1.
// det band ±0.001.  Non-static: pmesh.cpp's Patch_TexFollow solves with it too.
char Ed_Patch_AffineInverse( const float *xs, const float *ys, double *inv )
{
    double dy12 = ys[1] - ys[2];
    double dx21 = xs[2] - xs[1];
    double x1y2 = xs[1] * ys[2];
    double x2y1 = xs[2] * ys[1];
    double det  = xs[0] * dy12 + ys[0] * dx21 + x1y2 - x2y1;
    if ( det > -0.001000000047497451 && det < 0.001000000047497451 )
        return 0;
    double invDet = 1.0 / det;
    inv[0] = dy12 * invDet;
    inv[1] = ( ys[2] - ys[0] ) * invDet;
    inv[2] = ( ys[0] - ys[1] ) * invDet;
    inv[3] = dx21 * invDet;
    inv[4] = ( xs[0] - xs[2] ) * invDet;
    inv[5] = ( xs[1] - xs[0] ) * invDet;
    inv[6] = ( x1y2 - x2y1 ) * invDet;
    inv[7] = ( xs[2] * ys[0] - xs[0] * ys[2] ) * invDet;
    inv[8] = invDet * ( xs[0] * ys[1] - xs[1] * ys[0] );
    return 1;
}

// 0x4382D0  Ed_Patch_PlanarTest — average the per-quad face normals over the patch's
// tessellated render mesh (curveDef), normalize that to the patch plane normal, then
// verify the (width-1)×(height-1) CONTROL-point sub-grid sits within ±1 of that plane.
// out = {normal[3], dist}.  Returns 1 if planar, else 0.
// Two cross products per quad, len>0.001 gate before accumulate; plane dist =
// dot(centroidSum,N)/sampleCount; ±1 plane band.
char Ed_Patch_PlanarTest( patchMesh_t *patch, void *outBlk )  // out: float N[3]@0, double dist@16 (non-static: Patch_ApplyMaterial in pmesh.cpp calls it)
{
    float *out = (float *)outBlk;
    curvePatchDef_t *cd = patch->curveDef;
    curveVert_t     *verts = cd->verts;

    float nx = 0.0f, ny = 0.0f, nz = 0.0f;           // running plane-normal sum
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;           // running centroid (source-vertex) sum
    int   samples = 0;

    for ( int row = 0; row < cd->height - 1; ++row )
    {
        int width = cd->width;
        for ( int col = 0; col < cd->width - 1; ++col )
        {
            verts = cd->verts;
            // ── cross 1: edges from vert[i00] toward +column and +row ──
            int   i00 = col + row * width;
            float edgeA[3], edgeB[3], cr[3];
            edgeA[0] = verts[i00 + 1].xyz[0] - verts[i00].xyz[0];
            edgeA[1] = verts[i00 + 1].xyz[1] - verts[i00].xyz[1];
            edgeA[2] = verts[i00 + 1].xyz[2] - verts[i00].xyz[2];
            int i01  = col + width * ( row + 1 );
            edgeB[0] = verts[i01].xyz[0] - verts[i00].xyz[0];
            edgeB[1] = verts[i01].xyz[1] - verts[i00].xyz[1];
            edgeB[2] = verts[i01].xyz[2] - verts[i00].xyz[2];
            Vec3Cross( edgeA, edgeB, cr );
            if ( Vec3Normalize_R( cr ) > 0.001f )
            {
                nx += cr[0]; ny += cr[1]; nz += cr[2];
                float *c = cd->verts[col + row * cd->width].xyz;
                ++samples;
                cx += c[0]; cy += c[1]; cz += c[2];
            }
            // ── cross 2: edges from vert[i01b] (next row) toward -column and the diagonal ──
            verts    = cd->verts;
            int i01b = col + cd->width * ( row + 1 );
            edgeA[0] = verts[i01b].xyz[0] - verts[i01b + 1].xyz[0];
            edgeA[1] = verts[i01b].xyz[1] - verts[i01b + 1].xyz[1];
            edgeA[2] = verts[i01b].xyz[2] - verts[i01b + 1].xyz[2];
            int i10  = row * cd->width + col + 1;
            edgeB[0] = verts[i10].xyz[0] - verts[i01b + 1].xyz[0];
            edgeB[1] = verts[i10].xyz[1] - verts[i01b + 1].xyz[1];
            edgeB[2] = verts[i10].xyz[2] - verts[i01b + 1].xyz[2];
            Vec3Cross( edgeA, edgeB, cr );
            if ( Vec3Normalize_R( cr ) > 0.001f )
            {
                nx += cr[0]; ny += cr[1]; nz += cr[2];
                float *c = cd->verts[( row + 1 ) * cd->width + 1 + col].xyz;
                ++samples;
                cx += c[0]; cy += c[1]; cz += c[2];
            }
        }
    }

    float nrm[3] = { nx, ny, nz };
    if ( Vec3Normalize_R( nrm ) < 0.001f )                        // degenerate average normal
        return 0;
    nx = nrm[0]; ny = nrm[1]; nz = nrm[2];

    // plane dist = average of dot(centroid, N) over the samples.
    float dist = (float)( ( cz * nz + cx * nx + cy * ny ) / (double)samples );

    int width  = patch->width;
    int height = patch->height;
    if ( width - 1 >= 0 && width != 1 )
    {
        // verify the (width-1)×(height-1) control-point sub-grid lies within ±1 of the plane.
        for ( int col = 0; col < width - 1; ++col )
        {
            for ( int row = 0; row < height - 1; ++row )
            {
                const float *p = patch->ctrl[col][row].xyz;
                float d = p[0] * nx + p[1] * ny + p[2] * nz - dist;
                if ( d > 1.0f || d < -1.0f )
                    return 0;
            }
        }
    }

    out[0] = nx; out[1] = ny; out[2] = nz;
    *(double *)( (char *)outBlk + 16 ) = (double)dist;   // dist is stored as a double
    return 1;
}

// 0x4380B0  Ed_Patch_PickSpan — pick the first of 8 candidate control-point index
// triples whose 3 points are non-collinear (cross-product length² > 0.001).  Each
// candidate is 3 (col,row) pairs spanning the patch corners/edges; the candidates are
// seeded from the grid extents W1=width-1, H1=height-1, W2=width-2, H2=height-2.
// outCols[3] / outRows[3] receive the winning triple (control index = col*16 + row).
// Returns 1 on success, 0 if all 8 candidates are degenerate.
static char Ed_Patch_PickSpan( patchMesh_t *patch, int *outCols, int *outRows )
{
    const int W1 = patch->width  - 1;
    const int H1 = patch->height - 1;
    const int W2 = patch->width  - 2;
    const int H2 = patch->height - 2;

    // 8 candidates, each { col[3], row[3] } (index = col*16 + row).
    const int colTbl[8][3] = {
        { W1, 0,  0  },   // 0
        { 0,  0,  W1 },   // 1
        { 0,  W1, W1 },   // 2
        { W1, W1, 0  },   // 3
        { 1,  0,  0  },   // 4
        { 0,  0,  1  },   // 5
        { W2, W1, W1 },   // 6
        { W1, W1, W2 },   // 7
    };
    const int rowTbl[8][3] = {
        { 0,  0,  H1 },   // 0
        { 0,  H1, H1 },   // 1
        { H1, H1, 0  },   // 2
        { H1, 0,  0  },   // 3
        { 0,  0,  1  },   // 4
        { H2, H1, H1 },   // 5
        { H1, H1, H2 },   // 6
        { 1,  0,  0  },   // 7
    };

    for ( int i = 0; i < 8; ++i )
    {
        outCols[0] = colTbl[i][0]; outRows[0] = rowTbl[i][0];
        outCols[1] = colTbl[i][1]; outRows[1] = rowTbl[i][1];
        outCols[2] = colTbl[i][2]; outRows[2] = rowTbl[i][2];

        // three control points (index = col*16 + row), edges e1 = p1-p0, e2 = p2-p0.
        const float *p0 = patch->ctrl[ outCols[0] ][ outRows[0] ].xyz;
        const float *p1 = patch->ctrl[ outCols[1] ][ outRows[1] ].xyz;
        const float *p2 = patch->ctrl[ outCols[2] ][ outRows[2] ].xyz;
        float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
        float cr[3];
        Vec3Cross( e1, e2, cr );
        float lenSq = cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2];
        if ( lenSq > 0.001f )            // non-collinear: this candidate wins
            return 1;
    }
    return 0;
}

// 0x44B620  Ed_Patch_GetTexdef — extract a planar texdef (size/shift/rotate) from a
// patch's control-grid texCoords.  The patch analog of texturevecs_02; the middle-button
// patch texture-PICK (grab a patch's texture into the current texdef).  Side-effects
// `texdef` (size[2], shift[2], rotate) when ALL the patch's control points reproduce
// within the per-texel tolerance.
//
// IMPORTANT: this function ALWAYS returns 0 — every exit (including the success tail)
// returns 0.  The texdef WRITE is the observable; the consumer Drag_Begin discards the
// return value.
//
// Chain: Ed_Patch_PlanarTest -> Ed_Patch_PickSpan -> Ed_Normal_Calc (basis) -> project 3
// control pts to (u,v)+(s,t) -> Ed_Patch_AffineInverse -> recover the 2x3 affine (u,v)->(s,t)
// -> degenerate guard -> texel size from MaterialDef_GetLayeredMaterial (512 fallback) ->
// per-control-point verification loop -> size=1/|coeff|, shift=const term, rotate=atan2*180/pi
// with a floorf(x+0.5) integer-degree roundtrip check.
static char Ed_Patch_GetTexdef( patchMesh_t *patch, texdef_sub_t *texdef )
{
    // ── Phase A: planar test (writes plane normal N[3] @ planeBlk[0..2]) ──
    // planeBlk doubles as the 2x3 affine-matrix store after the multiply below.
    double planeBlk[8];                         // [0..2]=N (as floats via the cast), then reused
    if ( !Ed_Patch_PlanarTest( patch, planeBlk ) )
        return 0;

    // ── Phase B: pick 3 non-collinear control points spanning the patch ──
    int cols[3], rows[3];
    if ( !Ed_Patch_PickSpan( patch, cols, rows ) )
        return 0;

    // ── Phase C: build the tex-plane basis (xv,yv) from N (TextureAxisFromPlane) ──
    float xv[3], yv[3];
    Ed_Normal_Calc( (const float *)planeBlk, xv, yv );   // N is the first 3 floats of planeBlk

    const int layer = g_qeglobals.current_edit_layer;

    // ── Phase D: project the 3 chosen control points to (u,v) tex-plane coords + (s,t). ──
    int   idx0 = cols[0] * 16 + rows[0];
    int   idx1 = cols[1] * 16 + rows[1];
    int   idx2 = cols[2] * 16 + rows[2];
    const float *xyz0 = patch->ctrl[0][idx0].xyz;
    const float *xyz1 = patch->ctrl[0][idx1].xyz;
    const float *xyz2 = patch->ctrl[0][idx2].xyz;

    float u[3], v[3];                 // u = xyz·xv, v = xyz·yv
    u[0] = xyz0[0]*xv[0] + xyz0[1]*xv[1] + xyz0[2]*xv[2];
    v[0] = xyz0[0]*yv[0] + xyz0[1]*yv[1] + xyz0[2]*yv[2];
    u[1] = xyz1[0]*xv[0] + xyz1[1]*xv[1] + xyz1[2]*xv[2];
    v[1] = xyz1[0]*yv[0] + xyz1[1]*yv[1] + xyz1[2]*yv[2];
    u[2] = xyz2[0]*xv[0] + xyz2[1]*xv[1] + xyz2[2]*xv[2];
    v[2] = xyz2[0]*yv[0] + xyz2[1]*yv[1] + xyz2[2]*yv[2];

    // st coords: ctrl[idx].texCoord.st[2*layer + {0,1}].
    auto ctrlST = [&]( int idx, int comp ) -> float {
        return patch->ctrl[0][idx].texCoord.st[2 * layer + comp];
    };
    float s0 = ctrlST( idx0, 0 ), t0 = ctrlST( idx0, 1 );
    float s1 = ctrlST( idx1, 0 ), t1 = ctrlST( idx1, 1 );
    float s2 = ctrlST( idx2, 0 ), t2 = ctrlST( idx2, 1 );

    // ── Phase E: invert M = [[u_i, v_i, 1]] (Cramer) ──
    double inv[9];
    if ( !Ed_Patch_AffineInverse( u, v, inv ) )
        return 0;

    // ── Phase F: recover the 2x3 affine (u,v)->(s,t): rowS·(u,v,1)=s, rowT·(u,v,1)=t.
    //    rowS = {a_su, a_sv, a_sc}, rowT = {a_tu, a_tv, a_tc}  (double arithmetic). ──
    double rowS_u = (double)s0*inv[0] + (double)s1*inv[1] + (double)s2*inv[2];
    double rowS_v = (double)s2*inv[5] + (double)s1*inv[4] + (double)s0*inv[3];
    double rowS_c = (double)s0*inv[6] + (double)s1*inv[7] + (double)s2*inv[8];
    double rowT_u = inv[0]*(double)t0 + inv[1]*(double)t1 + inv[2]*(double)t2;
    double rowT_v = inv[5]*(double)t2 + inv[4]*(double)t1 + (double)t0*inv[3];
    double rowT_c = inv[6]*(double)t0 + inv[7]*(double)t1 + (double)t2*inv[8];

    // ── Phase G: degenerate guard — each row's (u,v) coefficients must not both be 0. ──
    if ( 0.0 == rowS_u && 0.0 == rowS_v ) return 0;
    if ( 0.0 == rowT_u && 0.0 == rowT_v ) return 0;

    // ── Phase H: texel size from the layered material (512 fallback). ──
    int texelW, texelH;
    qtexture_s *lm = MaterialDef_GetLayeredMaterial(
        (MaterialDef *)( (char *)&patch->texture + (size_t)layer * sizeof( patchMesh_material ) ) );
    if ( lm ) { texelW = lm->width; texelH = lm->height; }
    else      { texelW = 512;       texelH = 512; }

    // ── Phase I: verification loop — every control point must reproduce its st within tol. ──
    //    s_rec = rowS_u*u_pt + rowS_v*v_pt + rowS_c ;  |s_rec - s|*texelW must be <= 0.001.
    //    t_rec = rowT_u*u_pt + rowT_v*v_pt + rowT_c ;  |t_rec - t|*texelH must be <= 0.001.
    for ( int col = 0; col < patch->width; ++col )
    {
        for ( int row = 0; row < patch->height; ++row )
        {
            const float *xyz = patch->ctrl[col][row].xyz;
            double u_pt = (double)xyz[0]*xv[0] + (double)xyz[1]*xv[1] + (double)xyz[2]*xv[2];
            double v_pt = (double)xyz[0]*yv[0] + (double)xyz[1]*yv[1] + (double)xyz[2]*yv[2];
            float  s_act = patch->ctrl[col][row].texCoord.st[2 * layer + 0];
            float  t_act = patch->ctrl[col][row].texCoord.st[2 * layer + 1];
            double errS = rowS_u*u_pt + rowS_v*v_pt + rowS_c - (double)s_act;
            if ( errS < 0.0 ) errS = -errS;
            if ( errS * (double)texelW > 0.001000000047497451 )
                return 0;
            double errT = rowT_u*u_pt + rowT_v*v_pt + rowT_c - (double)t_act;
            if ( errT < 0.0 ) errT = -errT;
            if ( errT * (double)texelH > 0.001000000047497451 )
                return 0;
        }
    }

    // ── Phase J: size[i] = 1/|(coeff_u,coeff_v)|, shift[i] = const term, for rows S(0) and T(1). ──
    {
        double rows2[2][3] = { { rowS_u, rowS_v, rowS_c }, { rowT_u, rowT_v, rowT_c } };
        for ( int i = 0; i < 2; ++i )
        {
            texdef->shift[i] = (float)rows2[i][2];                              // const term -> shift
            double len = sqrt( rows2[i][0]*rows2[i][0] + rows2[i][1]*rows2[i][1] );
            if ( 0.0 == len )
                return 0;
            texdef->size[i] = (float)( 1.0 / len );
            // (the binary also normalizes the in-place (u,v) coeffs to (cos,sin); only the
            //  T row's normalized direction is used below, so re-normalize there directly.)
        }
    }

    // ── Phase K: rotate = atan2(rowT_v_norm, rowT_u_norm) * 180/pi, with a floorf(x+0.5)
    //    integer-degree roundtrip check (|round - raw| < 0.001 else fail). ──
    {
        // The size phase normalized rowT's (u,v) by lenT; the binary then took
        // atan2(y=rowT_u, x=rowT_v).  Since lenT>0 the normalization cancels:
        // atan2(rowT_u/lenT, rowT_v/lenT) == atan2(rowT_u, rowT_v).
        float  rotRaw = (float)( RAD2DEG( atan2( rowT_u, rowT_v ) ) );
        texdef->rotate = rotRaw;                                                // raw store
        float  rounded = floorf( rotRaw + 0.5f );
        float  diff = rounded - rotRaw;
        if ( diff < 0.0f ) diff = -diff;
        if ( diff >= 0.001f )
            return 0;
        texdef->rotate = rounded;
    }
    return 0;   // no success path (every exit returns 0)
}

// Drag_Begin (drag.cpp) middle-button PATCH texture-pick entry point (Ed_Patch_GetTexdef).
// Return value is intentionally discarded by the caller; the texdef write is the
// observable.
char Radiant_PatchGetTexdef( patchMesh_t *patch, texdef_sub_t *texdef )
{
    return Ed_Patch_GetTexdef( patch, texdef );
}

// 0x45D360  MaterialDef_FindTexdef — find the texdef block whose vis key matches
// `layerHandle` within the MaterialDef. Returns the texdef block, or NULL.
texdef_sub_t *MaterialDef_FindTexdef( MaterialDef *mtlDef, int layerHandle )
{
    int layerCount = MaterialDef_04( mtlDef );
    iassert( layerCount );   // TexWnd.cpp:1646
    for ( int layer = 0; layer < layerCount; ++layer )
    {
        texdef_sub_t *texdef = &mtlDef->mat_texDef + layer;
        int key;
        memcpy( &key, &texdef->sample_size, sizeof(int) );
        if ( key == layerHandle )
        {
            return texdef;
        }
    }
    return NULL;
}

// 0x51AD50  Material "colorTint" constant lookup — fill out[4], return 1 if found.
// constantCount is a BYTE.
static char Ed_Material_GetColorTint( Material *mtl, const char *name, float *out )
{
    unsigned int hash = R_HashString( name );
    if ( !mtl->constantCount )
        return 0;
    int index = 0;
    MaterialConstantDef *c = mtl->constantTable;
    while ( c->nameHash != hash )
    {
        if ( (unsigned)++index >= (unsigned)(byte)mtl->constantCount )
            return 0;
        ++c;
    }
    const float *lit = mtl->constantTable[index].literal;
    out[0] = lit[0]; out[1] = lit[1]; out[2] = lit[2]; out[3] = lit[3];
    return 1;
}

// 0x46F410  Ed_PackColor — pack float[4] colour → 4 bytes (out[2]=R, out[1]=G, out[0]=B,
// out[3]=A; BGRA store order).  ROUNDS, does not truncate.
static int Ed_ClampColorByte( float f )
{
    int v = (int)KiwiRound_Fistp( (double)f * 255.0 );   // KIWI: the float bias was lost (x.5 went to even)
    if ( v >= 255 ) return 255;
    if ( v <= 0 )   return 0;
    return v;
}
static void Ed_PackColor( const float *rgba, byte *out )
{
    out[2] = (byte)Ed_ClampColorByte( rgba[0] );
    out[1] = (byte)Ed_ClampColorByte( rgba[1] );
    out[0] = (byte)Ed_ClampColorByte( rgba[2] );
    out[3] = (byte)Ed_ClampColorByte( rgba[3] );
}

// 0x46F6C0  per-layer face colour: colorTint·entityColor if the material defines a
// "colorTint" constant, else the per-face packed colour (default white).
void Face_GetLayerColor( MaterialDef *mtlDef, const face_t *faceDef, int visIdx, int *outData )
{
    const byte *color = (const byte *)&faceDef->packedColor;
    Material      *mtl = MaterialDef_14( visIdx, mtlDef );
    iassert( mtl );   // MaterialDef.cpp:336
    float tint[4];
    if ( Ed_Material_GetColorTint( mtl, "colorTint", tint ) )
    {
        const double k = 0.003921568859368563;   // 1/255
        float tinted[4];
        tinted[0] = tint[0] * (float)( (double)color[2] * k );
        tinted[1] = tint[1] * (float)( (double)color[1] * k );
        tinted[2] = tint[2] * (float)( (double)color[0] * k );
        tinted[3] = tint[3] * (float)( k * (double)color[3] );
        Ed_PackColor( tinted, (byte *)outData );
    }
    else
    {
        *outData = faceDef->packedColor;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  TEXTURE FIT  (Surface Inspector "Fit" button → Brush_FitTexture).
//
//  Texture_Fit — fit one face's texture so the texture spans the face
//  `uTimes` times horizontally and `vTimes` times vertically (both clamped >= 1).  The
//  math is GtkRadiant's Face_FitTexture lineage, keeping the binary's float ordering:
//    1. Bounding box of the face winding points (or, with selectionBounds, the whole
//       selection's via Select_GetFitBounds - a Surface-Inspector mode OnFit does not use).
//    2. Ed_Normal_Calc picks the face's base S/T axes (xv,yv) from its plane normal.
//    3. Project the 8 box corners onto (xv,yv) — only the 4 (min/max u)×(min/max v)
//       combinations matter — rotate each by the texdef's stored `rotate`, and take
//       the rotated u/v extents.
//    4. size0 = -uExtent/uTimes, size1 = -vExtent/vTimes (texels-per-repeat, the editor's
//       inverted-scale convention); shift snapped into [0,extent) via floorf.
//  The texdef written is &md->mat_texDef + GetCurrentLayer(md) where md is the
//  layer's MaterialDef (f->mtldef[current_edit_layer]) — the SAME slot the Surface
//  Inspector edits (Surf_ApplyTexdefRaw) and the camera reads (Face_MoveTexture),
//  so a Fit re-projects live + round-trips through the .map writer.
// ════════════════════════════════════════════════════════════════════════════

// 0x48FB70 — fill [mins,maxs] from the whole current selection (Texture_Fit's
// selectionBounds mode).  OnFit / Brush_FitTexture never ask for it, so this is a
// tripwire, not a silent no-op.  (Distinct from Select_GetBounds 0x48FB10.)
extern void Select_GetFitBounds( float *minsOut, float *maxsOut );

// 0x47C590  Texture_Fit
void Texture_Fit( face_t *f, float vTimes, float uTimes, int selectionBounds )
{
    if ( vTimes < 1.0f ) vTimes = 1.0f;
    if ( uTimes < 1.0f ) uTimes = 1.0f;

    // Bounding box of the face's winding points (±131072 = 2^17 init sentinel).
    float mins[3] = {  131072.0f,  131072.0f,  131072.0f };
    float maxs[3] = { -131072.0f, -131072.0f, -131072.0f };
    if ( selectionBounds )
    {
        Select_GetFitBounds( mins, maxs );
    }
    else
    {
        winding_t *w = f->w;
        if ( !w )
            return;
        for ( int i = 0; i < w->numpoints; ++i )
            edwind::VectorMaxValues( w->p[i], mins, maxs );   // defined in namespace edwind
    }

    // The layer MaterialDef + its current sub-layer texdef block (the edited slot).
    MaterialDef  *md = &f->mtldef[g_qeglobals.current_edit_layer];
    int           curLayer = LayerMat::GetCurrentLayer( md );
    float        *texdef = &md->mat_texDef.size[7 * curLayer];   // [0]=size0 [1]=size1 [2]=shift0 [3]=shift1
    float         rotate = md->mat_texDef.size[7 * curLayer + 4];  // block CL's rotate

    double rad  = DEG2RAD( (double)rotate );
    float  cosv = (float)cos( rad );
    float  sinv = (float)sin( rad );

    // Base S/T axes for the face plane (xv,yv vec3).
    float xv[3], yv[3];
    Ed_Normal_Calc( f->plane.normal, xv, yv );

    // The four candidate (u,v) corners.  Dot products keep the binary's component order.
    float dotXvMins = xv[0]*mins[0] + xv[1]*mins[1] + xv[2]*mins[2];
    float dotYvMins = mins[0]*yv[0] + mins[1]*yv[1] + yv[2]*mins[2];
    float dotXvMaxs = xv[2]*maxs[2] + xv[0]*maxs[0] + xv[1]*maxs[1];
    float dotYvMaxs = maxs[0]*yv[0] + yv[1]*maxs[1] + maxs[2]*yv[2];

    float c0u = dotXvMins, c0v = dotYvMins;
    float c1u = dotXvMaxs, c1v = dotYvMins;
    float c2u = dotXvMins, c2v = dotYvMaxs;
    float c3u = dotXvMaxs, c3v = dotYvMaxs;

    // Rotate each corner and track the u/v extents.
    //   minU,minV start at +131072 ; maxU,maxV start at -131072.
    float minU =  131072.0f, minV =  131072.0f;
    float maxU = -131072.0f, maxV = -131072.0f;

    // C0
    float r0u = c0u*cosv - c0v*sinv;
    float r0v = c0u*sinv + c0v*cosv;
    if ( r0u < 131072.0f ) minU = r0u;
    if ( r0v < 131072.0f ) minV = r0v;
    // C1
    float r1u = c1u*cosv - c1v*sinv;
    float r1v = c1u*sinv + c1v*cosv;
    if ( r1u > -131072.0f ) maxU = r1u;
    if ( minV > r1v ) minV = r1v;
    // C2
    float r2u = c2u*cosv - c2v*sinv;
    float r2v = c2u*sinv + c2v*cosv;
    if ( minU > r2u ) minU = r2u;
    if ( r2v > -131072.0f ) maxV = r2v;
    // C3
    float r3u = c3u*cosv - c3v*sinv;
    float r3v = c3u*sinv + c3v*cosv;
    if ( maxU < r3u ) maxU = r3u;
    if ( maxV < r3v ) maxV = r3v;

    float uExtent = maxU - minU;
    float vExtent = maxV - minV;

    if ( uExtent == 0.0f || vExtent == 0.0f )
    {
        Sys_Printf( "Cannot fit texture - scale would be 0\n" );
        return;
    }

    texdef[0] = -uExtent / uTimes;              // size0
    texdef[1] = -vExtent / vTimes;              // size1
    float scaleW = texdef[0] * uTimes;          // = -uExtent
    texdef[2] = minU - floorf( minU / scaleW ) * scaleW;   // shift0
    float scaleH = texdef[1] * vTimes;          // = -vExtent
    texdef[3] = minV - floorf( minV / scaleH ) * scaleH;   // shift1
}

// 0x447600 — Patch_FitTexturing: fit one texture tile across a PATCH (st = normalized
// grid position).  Reached from Brush_FitTextureFaces when def->patch != 0 (the Surface Inspector
// "Fit" path on a selected patch).  Defined in pmesh.cpp.
extern void Patch_FitTexturing( patchMesh_t *patch );

// 0x47C950 — fit every face of a brush def (Brush_FitTexture's brush-pass).  Patch
// brushes route to Patch_FitTexturing; solid brushes Texture_Fit each face.
void Brush_FitTextureFaces( brush_t *def, float vTimes, float uTimes )
{
    if ( def->patch )
    {
        Patch_FitTexturing( def->patch );
        return;
    }
    for ( int i = 0; i < def->faceCount; ++i )
        Texture_Fit( &def->faces[i], vTimes, uTimes, 0 );
}

// ════════════════════════════════════════════════════════════════════════════
//  SUN-LIGHT PREVIEW (directional sunlight in the camera).
//
//  The binary's chain is R_SunPrev_Main (0x4069C0): R_SunPrev_SetSunConstants sets the
//  SUN_POSITION/DIFFUSE/SPECULAR shader constants from the worldspawn sun keys, two
//  full-screen quads black the world + clear alpha/stencil, SunLightPreview_BrushShadow
//  (0x47B310) extrudes stencil shadow volumes, then the world is re-drawn SUN-LIT via
//  R_AddEditorSurfsCmd.
//
//  g_edSun (not in the binary) is an APPROXIMATION that bakes the Lambert term
//  (ambient + diffuse*N.L)*suncolor into the per-vertex colour in Face_BuildLayerGeom,
//  from the worldspawn sun keys read by SunPrev_Setup.  No stencil shadows.  It is only
//  active when Cam_Draw selects it (g_edSun.active), never on the R_SunPrev_Main path.
// ════════════════════════════════════════════════════════════════════════════

struct EdSunState { int active; float dir[3]; float color[3]; float ambient; float diffuse; };
EdSunState g_edSun = { 0, { 0, 0, -1 }, { 1, 1, 1 }, 0.15f, 0.85f };

// Set once per camera frame by Cam_Draw (camwnd.cpp): 1 only when the APPROXIMATION
// sun preview is the active path, 0 on the R_SunPrev_Main path (and 0 with the
// preview off).  Gates the per-vertex sun bake in Face_BuildLayerGeom — see the comment
// there; on the R_SunPrev_Main path the sun must come ONLY from the tech-26 additive re-add.
int g_edSunBakeVertColor = 0;

void SunPrev_Setup()
{
    g_edSun.active = 0;
    if ( !world_entity )
        return;
    entity_s_def *wd = (entity_s_def *)world_entity->def;
    if ( !wd )
        return;
    float ang[3] = { 0, 0, 0 };
    if ( !Entity_GetVec3ForKey( wd, ang, "sundirection" ) )
        return;                                         // no sun key → preview off
    float fwd[3], right[3], up[3];
    AngleVectors( ang, fwd, right, up );                // sun travel direction
    g_edSun.dir[0] = fwd[0]; g_edSun.dir[1] = fwd[1]; g_edSun.dir[2] = fwd[2];
    float col[3] = { 1, 1, 1 };
    Entity_GetVec3ForKey( wd, col, "suncolor" );
    g_edSun.color[0] = col[0]; g_edSun.color[1] = col[1]; g_edSun.color[2] = col[2];
    float sunlight = Entity_GetFloatValueForKey( wd, "sunlight" );
    if ( sunlight <= 0.0f ) sunlight = 1.0f;
    float diffuseFrac = Entity_GetFloatValueForKey( wd, "diffusefraction" );
    if ( diffuseFrac <= 0.0f ) diffuseFrac = 0.5f;
    float ambient = Entity_GetFloatValueForKey( wd, "ambient" );
    if ( ambient < 0.05f ) ambient = 0.15f;
    g_edSun.ambient = ambient;
    g_edSun.diffuse = sunlight * diffuseFrac;
    g_edSun.active  = 1;
}

// Modulate a packed face colour (Ed_PackColor: byte0=B, byte1=G, byte2=R, byte3=A)
// by the directional-sun Lambert term against the world normal `wn`.
static unsigned int SunPrev_ShadeColor( unsigned int packed, const float *wn )
{
    float ndl = -( wn[0]*g_edSun.dir[0] + wn[1]*g_edSun.dir[1] + wn[2]*g_edSun.dir[2] );
    if ( ndl < 0.0f ) ndl = 0.0f;
    float lit = g_edSun.ambient + g_edSun.diffuse * ndl;
    if ( lit > 1.5f ) lit = 1.5f;
    byte *c = (byte *)&packed;
    float b = (float)c[0] * ( g_edSun.color[2] * lit );   // B
    float gg = (float)c[1] * ( g_edSun.color[1] * lit );  // G
    float r = (float)c[2] * ( g_edSun.color[0] * lit );   // R
    int bi = (int)b; if ( bi > 255 ) bi = 255; if ( bi < 0 ) bi = 0;
    int gi = (int)gg; if ( gi > 255 ) gi = 255; if ( gi < 0 ) gi = 0;
    int ri = (int)r; if ( ri > 255 ) ri = 255; if ( ri < 0 ) ri = 0;
    c[0] = (byte)bi; c[1] = (byte)gi; c[2] = (byte)ri;
    return packed;
}

int SunPrev_Active() { return g_edSun.active; }

// The editor brush technique (UNLIT) tints from CONST_SRC_CODE_MATERIAL_COLOR (a
// per-draw uniform), NOT per-vertex colour — so the visible directional-sun term is
// applied per face via R_AddCmdSetMaterialColor in the camera draw. Returns the
// face's sun colour {r,g,b,1} = suncolor·(ambient + diffuse·N·L).
void SunPrev_FaceShade( const float *wn, float *out4 )
{
    float ndl = -( wn[0]*g_edSun.dir[0] + wn[1]*g_edSun.dir[1] + wn[2]*g_edSun.dir[2] );
    if ( ndl < 0.0f ) ndl = 0.0f;
    float lit = g_edSun.ambient + g_edSun.diffuse * ndl;
    out4[0] = g_edSun.color[0] * lit;
    out4[1] = g_edSun.color[1] * lit;
    out4[2] = g_edSun.color[2] * lit;
    out4[3] = 1.0f;
}

// Face_BuildLayerGeom — Visuals_InitFaceVis' inner body for ONE material layer.
// Shared by the surf-cache draw and the immediate draw so they emit bit-identical
// geometry. Returns false when the face has no winding or the layer texdef is missing
// (defensive; emit nothing).  Editor_VB_Upload arg order is (pos,tangent,binormal,
// normal,st,color).  EdLayerGeom caps the winding at 64 verts vs the binary's 1024
// (safe: brush faces are convex).
bool Face_BuildLayerGeom( face_t *faceDef, const orientation_t *orient,
                          int layer, EdLayerGeom *g )
{
    MaterialDef *mtldef = &faceDef->mtldef[g_qeglobals.current_edit_layer];
    winding_t   *w      = faceDef->w;
    if ( !w )
        return false;
    int nv = w->numpoints;
    if ( nv < 1 || nv > 64 )
        return false;
    g->vertcount = nv;

    // world-space face normal (computed once per face; same for every layer).
    float wn[3];
    OrientationDirToWorldDir( wn, orient, faceDef->plane.normal );

    int   visVal = MaterialDef_13( layer, mtldef );
    texdef_sub_t *texdef = MaterialDef_FindTexdef( mtldef, visVal );
    if ( !texdef )
        return false;
    float texMat[8];
    texdef_sub_t *td = (texdef_sub_t *)(intptr_t)texdef;
    Face_MoveTexture( texdef->size, faceDef->plane.normal, texMat, texdef->shift,
                      td->rotate, td->crossterm );

    float tang[3];   sub_4A47D0( (float *)tang, (float *)texMat );  // tangent = normalize(row0)
    float binorm[3]; Vec3Cross( wn, tang, binorm );          // binormal = N × T

    unsigned int col = 0;
    Face_GetLayerColor( mtldef, faceDef, layer, (int *)&col );
    // APPROXIMATION ONLY: bake the directional sun into the per-vertex colour.
    // The binary's Visuals_InitFaceVis has NO such term — it is the fallback for the sun
    // preview when the R_SunPrev_Main path is unavailable.  `g_edSun.active` alone is not
    // the gate: SunPrev_Setup() sets it for ANY map with a "sundirection" key once the sun
    // preview pref is on, and on the R_SunPrev_Main path a bake multiplies every cached
    // face by suncolour·(ambient + diffuse·N·L) — a face pointing away from the sun bakes
    // down to `ambient` BEFORE the multiply quad and the additive re-add run, which
    // additive blending cannot undo (near-black facades).
    // g_edSunBakeVertColor is set per frame by Cam_Draw: 1 only on the approximation path.
    if ( g_edSun.active && g_edSunBakeVertColor )
        col = SunPrev_ShadeColor( col, wn );

    for ( int i = 0; i < nv; ++i )
    {
        const float *p = w->p[i];
        OrientationPosToWorldPos( g->xyz[i], p, orient );
        g->normal[i][0]   = wn[0];     g->normal[i][1]   = wn[1];     g->normal[i][2]   = wn[2];
        g->tangent[i][0]  = tang[0];   g->tangent[i][1]  = tang[1];   g->tangent[i][2]  = tang[2];
        g->binormal[i][0] = binorm[0]; g->binormal[i][1] = binorm[1]; g->binormal[i][2] = binorm[2];
        g->st[i][0] = texMat[0]*p[0] + texMat[1]*p[1] + texMat[2]*p[2] + texMat[3];
        g->st[i][1] = texMat[4]*p[0] + texMat[5]*p[1] + texMat[6]*p[2] + texMat[7];
        g->color[i] = col;
    }
    g->material = MaterialDef_14( layer, mtldef );
    KiwiMatConvert_ApplyFaceLightmapDiagnostic( faceDef, g ); // KIWI-UX: non-lit base material replaces the checker with red
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
//  Brush_RealizeFaceMaterials — not in the binary (the stock editor never makes
//  brush-face materials degenerate).
//
//  Prefab-content brush faces parse with the DEGENERATE SetMaterial shim (lyrMtl =
//  name@0, radMtl = NULL, layerCount 0) because the port forces that path while
//  g_radiantLoadingPrefab != 0 (materialdef.cpp), to dodge the CoD4-water material-asset
//  AV in the CoD3-sized kisak engine.  A degenerate mtldef makes Visuals_InitFaceVis'
//  MaterialDef_11 return 0, so the face builds NO visuals and the wall renders as a
//  white void.
//
//  MUST run at LOAD time (Eclass_RealizeModel's post-parse brush walk), never inside a
//  draw: realizing 100+ wall materials mid-frame stalls the first frame hard enough that
//  the MFC window never pumps a paint.  Requires the renderer to be up
//  (g_radiantFirstLightRendererReady, set in CMainFrame::OnCreate before the map load).
//  Materialdef_Realize is idempotent, and only mtldef[current_edit_layer] — exactly the
//  layer Visuals_InitFaceVis reads — is realized.
// ════════════════════════════════════════════════════════════════════════════
void Brush_RealizeFaceMaterials( brush_t *def )
{
    if ( !def )
        return;
    extern bool Materialdef_Realize( MaterialDef *md );   // materialdef.cpp
    for ( int i = 0; i < def->faceCount; ++i )
        Materialdef_Realize( &def->faces[i].mtldef[g_qeglobals.current_edit_layer] );
}

// ════════════════════════════════════════════════════════════════════════════
//  0x46F7A0  Visuals_InitFaceVis — build the per-face, per-layer surface cache.
//  Allocates face->visArray[layerCount] = {Material*, vbHandle} and uploads each
//  layer's GfxWorldVertex run to the editor VB pool. Uses the shared
//  Face_BuildLayerGeom for the per-vertex math. Reached via the brush faceVis
//  lifecycle (Brush_MakeFaceVisuals). Returns the layer count.
// ════════════════════════════════════════════════════════════════════════════
int Visuals_InitFaceVis( faceVis_s *face, face_t *def, const orientation_t *orient )
{
    iassert( face );                     // brush.cpp:130
    iassert( face->visCount == 0 );      // brush.cpp:131
    iassert( face->visArray == NULL );   // brush.cpp:132
    iassert( def );                      // brush.cpp:133

    winding_t *w = def->w;
    if ( !w )
        return 0;

    MaterialDef *mtldef = &def->mtldef[g_qeglobals.current_edit_layer];
    int layerCount = MaterialDef_11( mtldef );
    if ( !layerCount )
        return 0;

    face->visCount = layerCount;
    face->visArray = (faceVisuals_s *)operator new( (size_t)layerCount * sizeof(faceVisuals_s) );
    if ( !face->visArray )
        Error( "Out of memory allocating face visuals" );
    face->vertcount = w->numpoints;

    iassert( w->ptCount <= MAX_POINTS_ON_WINDING );   // brush.cpp:161

    for ( int L = 0; L < layerCount; ++L )
    {
        EdLayerGeom g;
        if ( !Face_BuildLayerGeom( def, orient, L, &g ) || !g.material )
        {
            face->visArray[L].mtlHandle  = nullptr;
            face->visArray[L].vertHandle = 0;
            continue;
        }
        face->visArray[L].mtlHandle  = g.material;
        face->visArray[L].vertHandle = (int)Editor_VB_Upload( g.material, g.vertcount,
            (const float *)g.xyz, (const float *)g.tangent, (const float *)g.binormal,
            (const float *)g.normal, (const float *)g.st, (const float *)g.color );
    }
    return layerCount;
}

// KIWI (2026-10-27, user: "it's still laggy while dragging and using tools. Nasty hitches
// too"): a live sample of a drag put 19% of the main thread in Brush_MakeFaceVisuals - every
// moved face freed its VB runs and allocated new ones every frame.  When a face still has
// the same layer count, the same material per layer and the same vertex count (every face
// of a move, and most edits), its existing runs are refilled in place instead.  Every layer
// is built first; nothing is written unless all of them fit, so any mismatch falls back
// to the ported free + Visuals_InitFaceVis path untouched.  True = rewritten.
const int KVIS_REWRITE_MAX_LAYERS = 8;

bool Visuals_RewriteFaceVis( faceVis_s *face, face_t *def, const orientation_t *orient )
{
    winding_t *w = def->w;
    if ( !face->visCount || !face->visArray || !w || face->vertcount != w->numpoints
      || face->visCount > KVIS_REWRITE_MAX_LAYERS )
        return false;
    MaterialDef *mtldef = &def->mtldef[g_qeglobals.current_edit_layer];
    if ( MaterialDef_11( mtldef ) != face->visCount )
        return false;

    static EdLayerGeom s_geom[KVIS_REWRITE_MAX_LAYERS];   // ~4 KB each; main thread only
    for ( int L = 0; L < face->visCount; ++L )
    {
        const faceVisuals_s &old = face->visArray[L];
        EdLayerGeom &g = s_geom[L];
        if ( !old.mtlHandle || !old.vertHandle
          || !Face_BuildLayerGeom( def, orient, L, &g ) || g.material != old.mtlHandle
          || g.vertcount != face->vertcount )
            return false;
    }
    for ( int L = 0; L < face->visCount; ++L )
    {
        const EdLayerGeom &g = s_geom[L];
        Editor_VB_Rewrite( (unsigned int)face->visArray[L].vertHandle, g.vertcount,
            (const float *)g.xyz, (const float *)g.tangent, (const float *)g.binormal,
            (const float *)g.normal, (const float *)g.st, (const float *)g.color );
    }
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
//  6. Brush_SetFaceTexdefSize  (0x476740)  —  really Face_SetMaterial.
//  Writes the {lyrMtl, radMtl} POINTER PAIR into face->mtldef[layer] (NOT size[0..1]),
//  then ++b->version.  Callers (csg.cpp, drag.cpp, findtexture.cpp) pass a
//  patchMesh_material {lyrMtl,radMtl} pair.
// ════════════════════════════════════════════════════════════════════════════
void Brush_SetFaceTexdefSize( const patchMesh_material *material, face_t *f, brush_t *b )
{
    iassert( b );   // brush.cpp:2872
    iassert( f );   // brush.cpp:2873
    iassert( f >= &b->faces[0] && f < &b->faces[b->faceCount] );   // brush.cpp:2874

    MaterialDef *md = &f->mtldef[ g_qeglobals.current_edit_layer ];
    md->lyrMtl = material->lyrMtl;
    md->radMtl = material->radMtl;
    ++b->version;
}

// ════════════════════════════════════════════════════════════════════════════
//  6b. Brush_PropagateFaceTexdef (0x4766f0) — copy the current-layer
//  MaterialDef from the picked SOURCE face into EVERY face of the brush def, then
//  bump the def's version.  Reached from Drag_Begin's middle/right "apply face
//  texture but leave info" branch when the hit brush has a patch (def->patch != 0):
//  the caller already wrote random_texture_stuff[layer] into the picked face's
//  mtldef[layer], then passes that same face address here so the new MaterialDef
//  is propagated to ALL faces (keeping the patch + faces consistent).
// ════════════════════════════════════════════════════════════════════════════
void Brush_PropagateFaceTexdef( brush_t *def, const face_t *srcFace )
{
    const int layer = g_qeglobals.current_edit_layer;
    const MaterialDef *s = &srcFace->mtldef[ layer ];
    for ( int i = 0; i < def->faceCount; ++i )
        memcpy( &def->faces[i].mtldef[ layer ], s, sizeof(MaterialDef) );
    ++def->version;
}

// forward declare TexMatToFakeTexCoords (0x472df0)
extern void TexMatToFakeTexCoords( MaterialDef *mtlDef, texdef_sub_t *texDef );

// ════════════════════════════════════════════════════════════════════════════
//  7. Brush_SetFaceTexdef  (0x4767e0)
//  Copies a texdef_sub_t into the current-layer MaterialDef of a face.
// ════════════════════════════════════════════════════════════════════════════
brush_t *Brush_SetFaceTexdef( const texdef_sub_t *texDef, face_t *f, brush_t *b )
{
    iassert( b );        // brush.cpp:2885
    iassert( f );        // brush.cpp:2886
    iassert( f >= &b->faces[0] && f < &b->faces[b->faceCount] );   // brush.cpp:2887
    iassert( texDef );   // brush.cpp:2888

    MaterialDef *md = &f->mtldef[ g_qeglobals.current_edit_layer ];
    memcpy( &md->mat_texDef, texDef, sizeof(md->mat_texDef) );
    TexMatToFakeTexCoords( md, &md->mat_texDef );
    ++b->version;
    return b;
}

// ════════════════════════════════════════════════════════════════════════════
//  8. Face_SetSampleSize (0x4768b0)
//  Sets the texture sample size for a face (or all faces if brush has a patch).
// ════════════════════════════════════════════════════════════════════════════
// Brush_SetAllFacesSampleSize (0x477080): whole-brush sample size setter (defined just below).
extern void Brush_SetAllFacesSampleSize( brush_t *b, int sampleSize );

void Face_SetSampleSize( face_t *f, brush_t *b, int sampleSize )
{
    iassert( b );   // brush.cpp:2902
    iassert( f );   // brush.cpp:2903
    iassert( f >= &b->faces[0] && f < &b->faces[b->faceCount] );   // brush.cpp:2904

    if ( b->patch )
    {
        Brush_SetAllFacesSampleSize( b, sampleSize );
    }
    else
    {
        // mtldef[1] — the lightmap material def.
        MaterialDef *md = &f->mtldef[1];
        qtexture_s *lyrMtl = MaterialDef_GetLayeredMaterial( md );
        int width  = lyrMtl ? lyrMtl->width  : 512;
        int height = lyrMtl ? lyrMtl->height : 512;
        float *size = &md->mat_texDef.size[ 7 * LayerMat::GetCurrentLayer( md ) ];
        size[0] = (float)(sampleSize * width);
        size[1] = (float)(sampleSize * height);
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  Brush_SetAllFacesMaterial (0x476ED0) — the per-BRUSH "set all face textures" apply.
//  Reached from Brush_SetTexture's brush pass (select.cpp) when whole BRUSHES (not faces)
//  are selected and a material is clicked, and from the drag.cpp camera apply-material
//  paths:
//   1) Copy `material` into EVERY face's mtldef[current_edit_layer].  For a patch brush
//      the binary then fans face i's mtldef[layer] out to ALL faces' mtldef[layer]
//      (a redundant inner copy); bumps ->version inside the per-face loop.
//   2) Brush_BuildWindings; SetupVertexSelection() if vertex/edge mode; MarkMapModified();
//      ++version ("tex wont update visually without this").
//   3) Patch brush: if materialOnly → stamp the patch's per-layer lyrMtl/radMtl + bump
//      patch version + refresh patch inspector (if open) + return; else → Patch_ApplyMaterial
//      (drag-texture-onto-a-patch projection: planarProject / useStoredST are its flags).
//   4) Brush_UpdateSpecialMaterialFlag.
// ════════════════════════════════════════════════════════════════════════════
extern char Patch_ApplyMaterial( MaterialDef *material, patchMesh_t *patch, char planarProject,
                                 float useStoredST );                            // pmesh.cpp (0x4428f0)
void Brush_UpdateSpecialMaterialFlag( brush_t *def );                          // fwd (defined below 0x47b940)

void Brush_SetAllFacesMaterial( brush_t *def, MaterialDef *material, char planarProject, float useStoredST,
                                char materialOnly )
{
    const int layer = g_qeglobals.current_edit_layer;

    for ( unsigned int i = 0; i < (unsigned int)def->faceCount; ++i )
    {
        face_t *f = &def->faces[i];
        qmemcpy( &f->mtldef[layer], material, sizeof( MaterialDef ) );

        if ( def->patch )                                                       // fan-out (patch only)
        {
            for ( unsigned int j = 0; j < (unsigned int)def->faceCount; ++j )
                qmemcpy( (char *)&def->faces->mtldef[layer] + j * sizeof( face_t ),
                         &f->mtldef[layer], sizeof( MaterialDef ) );
        }
        ++def->version;
    }

    Brush_BuildWindings( def, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();

    patchMesh_t *patch = def->patch;
    ++def->version;                                                           // tex wont update visually without this
    if ( patch )
    {
        if ( materialOnly )
        {
            patchMesh_material *pmDst = &patch->texture + layer;
            pmDst->lyrMtl = ( (patchMesh_material *)material )->lyrMtl;
            pmDst->radMtl = ( (patchMesh_material *)material )->radMtl;
            ++patch->version;
            if ( PatchDialog_IsOpen() )
            {
                g_PatchDialog_GetPatchInfo();
                Brush_UpdateSpecialMaterialFlag( def );
                return;
            }
        }
        else
        {
            Patch_ApplyMaterial( material, patch, planarProject, useStoredST );
        }
    }
    Brush_UpdateSpecialMaterialFlag( def );
}

// ════════════════════════════════════════════════════════════════════════════
//  8b. Brush_SetAllFacesSampleSize (0x477080) — whole-brush sample size setter.
//  Sets the lightmap-channel texdef size of every face to (sampleSize * material w/h),
//  rebuilds the windings, bumps the version, and for a patch brush delegates the patch
//  refinement to Patch_SetSampleSize.  Reached from Brush_SetSampleSize's brush pass and from
//  Face_SetSampleSize's patch branch.
// ════════════════════════════════════════════════════════════════════════════
extern void Patch_SetSampleSize( patchMesh_t *patch, int sampleSize );   // pmesh.cpp 0x442B00 (patch sample-size core)

void Brush_SetAllFacesSampleSize( brush_t *b, int sampleSize )
{
    for ( int i = 0; i < b->faceCount; ++i )
    {
        MaterialDef *md = &b->faces[i].mtldef[1];   // lightmap channel
        qtexture_s *lyrMtl = MaterialDef_GetLayeredMaterial( md );
        int width  = lyrMtl ? lyrMtl->width  : 512;
        int height = lyrMtl ? lyrMtl->height : 512;
        float *size = &md->mat_texDef.size[ 7 * LayerMat::GetCurrentLayer( md ) ];
        size[0] = (float)(sampleSize * width);
        size[1] = (float)(sampleSize * height);
    }
    Brush_BuildWindings( b, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++b->version;
    if ( b->patch )
        Patch_SetSampleSize( b->patch, sampleSize );   // patch sample-size core (texCoord resample deferred)
}

// ════════════════════════════════════════════════════════════════════════════
//  8c. Brush_SetAllFacesTexdef (0x477020) — apply one texdef to EVERY face of a brush
//  (Brush_SetFaceTexdef), then rebuild windings + bump version.  Reached from
//  Brush_SetTextureMapping's brush pass ("set brush texture mapping").
// ════════════════════════════════════════════════════════════════════════════
void Brush_SetAllFacesTexdef( brush_t *b, const texdef_sub_t *texdef )
{
    for ( int i = 0; i < b->faceCount; ++i )
        Brush_SetFaceTexdef( texdef, &b->faces[i], b );
    Brush_BuildWindings( b, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++b->version;
}

// ════════════════════════════════════════════════════════════════════════════
//  9. Brush_ApplyTextureProjection  (0x476a30)
//  Copies texture projection from srcFace to dstFace.
// ════════════════════════════════════════════════════════════════════════════
// Callers pass (srcFace, brush, dstFace) — note the second arg is the brush, not a face.
brush_t *Brush_ApplyTextureProjection( face_t *srcFace, brush_t *b, face_t *dstFace )
{
    face_t *src = (face_t *)(intptr_t)srcFace;
    face_t *dst = (face_t *)(intptr_t)dstFace;

    int layer = g_qeglobals.current_edit_layer;

    texdef_sub_t *srcTd = &src->mtldef[layer].mat_texDef;
    texdef_sub_t *dstTd = &dst->mtldef[layer].mat_texDef;

    float srcCrossterm = srcTd->crossterm;
    float srcRotate = srcTd->rotate;
    float *src_normal = src->plane.normal;

    {
        float texVec[8];    // Face_MoveTexture output, later matrix-transform output
        float inVec[8];     // master/slave plane pair, then the two stride-2 input vectors
        float tvGrad[3];    // Face_SolveTexGradient out, later Vec3Cross(dst) out
        float tvAxis[3];    // shared rotation axis
        float crossSrc[3];  // Vec3Cross(src) out

        Face_MoveTexture( (float *)srcTd, src_normal,
                          (float *)texVec, (float *)srcTd->shift,
                          srcRotate, srcCrossterm );

        float *dst_normal = dst->plane.normal;

        // dot product of normals
        float dot = src->plane.normal[2] * dst->plane.normal[2]
                  + src->plane.normal[1] * dst->plane.normal[1]
                  + src->plane.normal[0] * dst->plane.normal[0];

        if ( dot > 0.9990000128746033 || dot < -0.9989999999525025 )
        {
            // Parallel planes: direct copy of texdef values.  KIWI: exact decomposition.
            double tm[8];
            for ( int k = 0; k < 8; ++k )
                tm[k] = texVec[k];
            texturevecs_02_exact( dstTd->size, tm, dst_normal, dst->plane.dist,
                                  dstTd->shift, &dstTd->rotate, &dstTd->crossterm );
        }
        else
        {
            // Non-parallel: build rotation matrix. inVec:
            // [0..3] master plane (normal, -dist), [4..7] slave plane.
            inVec[0] = *src_normal;
            inVec[1] = src->plane.normal[1];
            inVec[2] = src->plane.normal[2];
            inVec[4] = *dst_normal;
            inVec[5] = dst->plane.normal[1];
            inVec[6] = dst->plane.normal[2];
            inVec[3] = -src->plane.dist;
            inVec[7] = -dst->plane.dist;

            Face_TexPlaneGradient( tvAxis, inVec, tvGrad );

            float masterDot = src->plane.normal[2] * tvGrad[2]
                            + src->plane.normal[1] * tvGrad[1]
                            + tvGrad[0] * src->plane.normal[0];
            float distFromMasterPlane = (float)(masterDot - (double)src->plane.dist);

            float slaveDot = tvGrad[2] * dst->plane.normal[2]
                           + tvGrad[1] * dst->plane.normal[1]
                           + tvGrad[0] * dst->plane.normal[0];
            float distFromSlavePlane = (float)(slaveDot - (double)dst->plane.dist);

            vassert( (fabsf( distFromMasterPlane ) < 0.01f), "(distFromMasterPlane) = %g", (double)distFromMasterPlane );   // brush.cpp:2992
            vassert( (fabsf( distFromSlavePlane ) < 0.01f),  "(distFromSlavePlane) = %g",  (double)distFromSlavePlane );    // brush.cpp:2993

            // Build 4x4 matrix basis (64-byte GfxMatrix)
            float basis[16], basisInv[16];
            memset( basis, 0, sizeof(basis) );
            basis[0]  = tvGrad[0];
            basis[1]  = tvGrad[1];
            basis[2]  = tvGrad[2];
            basis[3]  = 1.0f;

            // texVec = Face_MoveTexture output:
            //   [0..2] S-vector, [3] S-shift, [4..6] T-vector, [7] T-shift
            float sDot = tvGrad[2] * texVec[2] + texVec[1] * tvGrad[1] + texVec[0] * tvGrad[0];
            inVec[0] = sDot + texVec[3];
            float tDot = tvGrad[0] * texVec[4] + tvGrad[1] * texVec[5] + texVec[6] * tvGrad[2];
            inVec[1] = tDot + texVec[7];

            basis[4]  = tvAxis[0];
            basis[5]  = tvAxis[1];
            basis[6]  = tvAxis[2];

            inVec[2] = tvAxis[0] * texVec[0] + tvAxis[1] * texVec[1] + tvAxis[2] * texVec[2];
            inVec[3] = texVec[5] * tvAxis[1] + texVec[4] * tvAxis[0] + texVec[6] * tvAxis[2];

            basis[8]  = *dst_normal;
            basis[9]  = dst->plane.normal[1];
            basis[10] = dst->plane.normal[2];

            inVec[4] = texVec[0] * src->plane.normal[0] + texVec[1] * src->plane.normal[1] + texVec[2] * src->plane.normal[2];
            inVec[5] = texVec[5] * src->plane.normal[1] + texVec[4] * src->plane.normal[0] + src->plane.normal[2] * texVec[6];

            Vec3Cross( dst_normal, tvAxis, tvGrad );
            Vec3Normalize_R( tvGrad );
            Vec3Cross( src_normal, tvAxis, crossSrc );
            Vec3Normalize_R( crossSrc );

            basis[12] = tvGrad[0];
            basis[13] = tvGrad[1];
            basis[14] = tvGrad[2];

            inVec[6] = crossSrc[1] * texVec[1] + crossSrc[0] * texVec[0] + crossSrc[2] * texVec[2];
            inVec[7] = crossSrc[1] * texVec[5] + crossSrc[0] * texVec[4] + crossSrc[2] * texVec[6];

            MatrixInverse44( basis, basisInv );

            // Apply the inverse matrix basisInv (row-major 4x4) to two input 4-vectors,
            // writing the 8 transformed tex-vec floats texVec[0..7]: 2 (outer, in =
            // &inVec[0] then &inVec[1]) x 4 (row), each
            //   out[row] = basisInv[row][0]*in[0] + basisInv[row][1]*in[2]
            //            + basisInv[row][2]*in[4] + basisInv[row][3]*in[6]
            // (in indexed stride-2; out[0..3]=texVec[0..3] then out[4..7]=texVec[4..7]).
            for ( int o = 0; o < 2; ++o )
            {
                const float *in = &inVec[o];
                for ( int r = 0; r < 4; ++r )
                    texVec[4 * o + r] = basisInv[4 * r + 0] * in[0]
                                      + basisInv[4 * r + 1] * in[2]
                                      + basisInv[4 * r + 2] * in[4]
                                      + basisInv[4 * r + 3] * in[6];
            }

            double tm[8];                  // KIWI: exact decomposition
            for ( int k = 0; k < 8; ++k )
                tm[k] = texVec[k];
            texturevecs_02_exact( dstTd->size, tm, dst_normal, dst->plane.dist,
                                  dstTd->shift, &dstTd->rotate, &dstTd->crossterm );
        }
    }

    ++b->version;
    return b;
}

// ════════════════════════════════════════════════════════════════════════════
//  1. Face_InitTextureChannel is Face_InitMaterialChannel above.
// ════════════════════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════════════════════
//  2+3. Face_InitMaterialChannels / Brush_InitMaterialChannels are
//  Face_SetDefaultMaterials / Brush_SetDefaultMaterials above.
// ════════════════════════════════════════════════════════════════════════════

// ════════════════════════════════════════════════════════════════════════════
//  10. Face_ParseSurfDef  (0x472ec0)
//  Parses one surface definition from the text stream for a face.
// ════════════════════════════════════════════════════════════════════════════
// The channel-2 path also clears unk3 (the texdef's int [6]).
static int Face_ParseSurfDef( const char **text, MaterialDef *surfDef, int version, int channel )
{
    iassert( surfDef );   // brush.cpp:1444

    patchMesh_material *material = (patchMesh_material *)surfDef;
    const char *name;
    char nameBuf[1028];

    if ( channel == 2 )
    {
        if ( version >= 4 )
        {
            Map_ParseEntityLayerKey( text, "smoothing_hard", nameBuf, "smoothing" );
            name = nameBuf;
        }
        else
        {
            name = "smoothing_hard";
        }
    }
    else
    {
        parseInfo_t *tok;
        if ( version >= 4 )
        {
            Com_SetSpaceDelimited( 1 );
            tok = Com_ParseOnLine( text );
            Com_SetSpaceDelimited( 0 );
        }
        else
        {
            tok = Com_ParseOnLine( text );
        }
        char *token = tok->token;
        if ( strchr( tok->token, 32 ) )
        {
            Error( "Material name with a space in it! '%s'\n", token );
            return 0;
        }
        if ( version )
            name = token;
        else
            name = Mtl_NormalizeName( token );
    }

    SetMaterial( name, material );

    iassert( material->radMtl || material->lyrMtl );   // brush.cpp:1478

    int curLayer = LayerMat::GetCurrentLayer( surfDef );
    // texdef = this layer's texdef_sub_t block (&surfDef->mat_texDef + curLayer).
    float *texdef = (float *)(&surfDef->mat_texDef + curLayer);

    if ( channel == 2 )
    {
        // smoothing channel: fixed defaults
        texdef[0] = 0.25f;
        // unk3 is int [6] ([5] is the float zeroed just below), as in the else-branch.
        ((int *)texdef)[6] = 0;  // unk3 at +24
        texdef[1] = 0.25f;
        texdef[2] = 0.0f;
        texdef[3] = 0.0f;
        texdef[4] = 0.0f;
        texdef[5] = 0.0f;
    }
    else
    {
        parseInfo_t *tok = Com_ParseOnLine( text );
        if ( version >= 4 )
        {
            texdef[0] = (float)atof( tok->token );
            tok = Com_ParseOnLine( text );
            texdef[1] = (float)atof( tok->token );
            tok = Com_ParseOnLine( text );
            texdef[2] = (float)atof( tok->token );
            tok = Com_ParseOnLine( text );
            texdef[3] = (float)atof( tok->token );
            tok = Com_ParseOnLine( text );
            texdef[4] = (float)atof( tok->token );
            tok = Com_ParseOnLine( text );
            texdef[5] = (float)atof( tok->token );
        }
        else
        {
            texdef[2] = (float)atol( tok->token );
            tok = Com_ParseOnLine( text );
            texdef[3] = (float)atol( tok->token );
            tok = Com_ParseOnLine( text );
            texdef[4] = (float)atol( tok->token );
            tok = Com_ParseOnLine( text );
            texdef[0] = (float)atof( tok->token );
            tok = Com_ParseOnLine( text );
            float size1 = (float)atof( tok->token );
            texdef[5] = 0.0f;
            texdef[0] = Float_CleanLowBits( texdef[0], 4 );   // FP-noise scrub
            texdef[1] = Float_CleanLowBits( size1, 4 );
        }

        ((int *)texdef)[6] = 0;   // unk3 at +24

        if ( texdef[0] == 0.0f )
        {
            float def = channel ? 16.0f : 0.25f;
            texdef[0] = def;
        }
    }

    if ( texdef[1] == 0.0f )
    {
        float def = channel ? 16.0f : 0.25f;
        texdef[1] = def;
    }

    TexMatToFakeTexCoords( surfDef, (texdef_sub_t *)texdef );
    return 1;
}

// KIWI: the numbers a physics brush was parsed from, with its plane points as placed.  The
// writer re-emits them while the plane points are untouched: an axis cannot be recovered
// from float corners 100 units out to float precision, so every re-save moved it an ulp.
struct physParsed_t
{
    std::vector<float> planepts;
    float              vals[15];
};
static std::unordered_map<const brush_t *, physParsed_t> s_physParsed;

static void PhysParsed_Planepts( const brush_t *b, std::vector<float> &out )
{
    out.clear();
    for ( int fi = 0; b->faces && fi < b->faceCount; ++fi )
        out.insert( out.end(), &b->faces[fi].planepts[0][0], &b->faces[fi].planepts[0][0] + 9 );
}

static void PhysParsed_Forget( const brush_t *b )
{
    s_physParsed.erase( b );
}

static void PhysParsed_Store( const brush_t *b, const float *vals, int n )
{
    physParsed_t &rec = s_physParsed[b];
    PhysParsed_Planepts( b, rec.planepts );
    memcpy( rec.vals, vals, n * sizeof( float ) );
}

// The parsed numbers, or null once the brush was moved / edited (or never parsed).
static const float *PhysParsed_Find( const brush_t *b )
{
    auto it = s_physParsed.find( b );
    if ( it == s_physParsed.end() )
        return nullptr;
    std::vector<float> now;
    PhysParsed_Planepts( b, now );
    return now == it->second.planepts ? it->second.vals : nullptr;
}

// KIWI: place a physics brush built about the origin - local x / y / z become axis[0..2]
// (rows), then the centre - in double.  The binary turned the axis into angles and rotated
// the plane points by them in float, so a saved box never read back onto its own axis.
// A left-handed axis would turn the faces inside out: their plane points are re-wound.
static void PhysBrush_Place( brush_t *b, const double ( *axis )[3], const float *centre )
{
    const double det = axis[0][0] * ( axis[1][1] * axis[2][2] - axis[1][2] * axis[2][1] )
                     - axis[0][1] * ( axis[1][0] * axis[2][2] - axis[1][2] * axis[2][0] )
                     + axis[0][2] * ( axis[1][0] * axis[2][1] - axis[1][1] * axis[2][0] );
    for ( int fi = 0; fi < b->faceCount; ++fi )
    {
        face_t *f = &b->faces[fi];
        for ( int p = 0; p < 3; ++p )
        {
            const double l[3] = { f->planepts[p][0], f->planepts[p][1], f->planepts[p][2] };
            for ( int k = 0; k < 3; ++k )
                f->planepts[p][k] = (float)( centre[k] + l[0] * axis[0][k] + l[1] * axis[1][k] + l[2] * axis[2][k] );
        }
        if ( det < 0.0 )
        {
            for ( int k = 0; k < 3; ++k )
            {
                const float t = f->planepts[0][k];
                f->planepts[0][k] = f->planepts[2][k];
                f->planepts[2][k] = t;
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  12. Brush_ParsePhysicsBox  (0x473660)
// ════════════════════════════════════════════════════════════════════════════
// size = (double)wh * 0.25.
static brush_t *Brush_ParsePhysicsBox( const char **text )
{
    if ( Com_Parse( text )->token[0] != '{' )
        return nullptr;

    float axis[9];
    axis[0] = (float)Com_ParseFloat( text );
    parseInfo_t *t = Com_ParseOnLine( text );
    axis[1] = (float)atof( t->token );
    t = Com_ParseOnLine( text ); axis[2] = (float)atof( t->token );
    axis[3] = (float)Com_ParseFloat( text );
    t = Com_ParseOnLine( text ); axis[4] = (float)atof( t->token );
    t = Com_ParseOnLine( text ); axis[5] = (float)atof( t->token );
    axis[6] = (float)Com_ParseFloat( text );
    t = Com_ParseOnLine( text ); axis[7] = (float)atof( t->token );
    t = Com_ParseOnLine( text ); axis[8] = (float)atof( t->token );

    // center[3] = box center; half[3] = box half-extents.
    float center[3], half[3];
    t = Com_ParseOnLine( text ); center[0] = (float)atof( t->token );
    t = Com_ParseOnLine( text ); center[1] = (float)atof( t->token );
    t = Com_ParseOnLine( text ); center[2] = (float)atof( t->token );
    t = Com_ParseOnLine( text ); half[0]   = (float)atof( t->token );
    t = Com_ParseOnLine( text ); half[1]   = (float)atof( t->token );
    t = Com_ParseOnLine( text ); half[2]   = (float)atof( t->token );
    Com_MatchToken( text, "}", 0 );

    MaterialDef material;
    memset( &material.mat_texDef, 0, sizeof(material.mat_texDef) );

    // KIWI: built about the origin, then placed by PhysBrush_Place (below).
    float mins[3], maxs[3];
    for ( int k = 0; k < 3; ++k )
    {
        mins[k] = -half[k];
        maxs[k] = half[k];
    }

    material.lyrMtl = g_qeglobals.random_texture_stuff[0].mtl.lyrMtl;
    material.radMtl = g_qeglobals.random_texture_stuff[0].mtl.radMtl;

    qtexture_s *lm = MaterialDef_GetLayeredMaterial( &material );
    int w = lm ? lm->width  : 512;
    int h = lm ? lm->height : 512;

    texdef_sub_t *texSlot = &material.mat_texDef + LayerMat::GetCurrentLayer( &material );
    texSlot->size[0] = (double)w * 0.25;
    texSlot->size[1] = (double)h * 0.25;

    brush_t *b = Brush_Alloc( &material, nullptr );
    Brush_Create( mins, maxs, b, nullptr );
    b->numberId = 1;
    Brush_BuildWindings( b, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex ||
         g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++b->version;

    // KIWI: replaces the binary's AxisToAngles + face-planepts rotation (0x4738df..).
    const double frame[3][3] = { { axis[0], axis[1], axis[2] },
                                 { axis[3], axis[4], axis[5] },
                                 { axis[6], axis[7], axis[8] } };
    PhysBrush_Place( b, frame, center );
    Brush_BuildWindings( b, 1 );
    {
        const float vals[15] = { axis[0], axis[1], axis[2], axis[3], axis[4], axis[5], axis[6], axis[7],
                                 axis[8], center[0], center[1], center[2], half[0], half[1], half[2] };
        PhysParsed_Store( b, vals, 15 );
    }
    if ( g_qeglobals.d_select_mode == sel_vertex ||
         g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++b->version;
    return b;
}

// ════════════════════════════════════════════════════════════════════════════
//  Brush primitives — reshape the SELECTED brush into an N-sided cylinder
//  (Brush_MakeSided), a cone (Brush_MakeSidedCone) or a sphere
//  (Brush_MakeSidedSphere).  All three operate IN-PLACE on the existing brush
//  DEF: they free the old face array and re-allocate it with the new face count,
//  then rebuild the windings.  This is the CoD divergence from GtkRadiant (which
//  Brush_Free's the brush and Brush_Alloc's a fresh one + prepends faces) — the
//  CoD binary keeps the same brush_t and writes the faces into an array in the
//  order [top, bottom, side0, side1, …].  The planept rotation math matches
//  GtkRadiant 1.6; the constants (min-width, conditional snap, in-place layout) are
//  the binary's.
// ════════════════════════════════════════════════════════════════════════════

// Sphere_Point (0x4AA390) — fill out[3] for one sphere planept: radius·cos²,
// radius·cos·sin, radius·sin of `ang` (NOT a textbook spherical point — the `cos²`
// term is real), single precision.
static void Sphere_Point( float *out, float radius, float ang )
{
    float c = cosf( ang );
    float s = sinf( ang );
    out[0] = c * radius * c;
    out[1] = c * ( s * radius );
    out[2] = radius * s;
}

// ─── Brush_MakeSided (0x4731E0) ───────────────────────────────────────────────
// def = the brush DEF (selected_brushes.next->def), sides = side count,
// axis = the cylinder axis (0=X,1=Y,2=Z) from the active XY-view type,
// snap = snap-to-integer flag (floor(x+0.5) on each side-face point).
void Brush_MakeSided( brush_t *def, unsigned int sides, int axis, char snap )
{
    if ( sides < 3 )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Bad sides number" );
        return;
    }
    if ( sides >= 1020 )                            // MAX_POINTS_ON_WINDING - 4
    {
        Sys_Printf( "too many sides.\n" );
        return;
    }

    float mins[3], maxs[3], mid[3];
    Vec3Copy( def->mins, mins );
    Vec3Copy( def->maxs, maxs );

    // Centre of the brush; widest non-axis half-extent (min 1.0 — the CoD
    // divergence: GtkRadiant + the cone/sphere clamp to 8.0, makeSided to 1.0).
    float width = 1.0f;
    for ( int i = 0; i < 3; ++i )
    {
        mid[i] = ( maxs[i] + mins[i] ) * 0.5f;
        if ( i == axis )
            continue;
        float half = ( maxs[i] - mins[i] ) * 0.5f;
        if ( width < half )
            width = half;
    }

    // Re-allocate the face array in place: sides + a top and a bottom cap.
    Face_Free( def->faceCount, def->faces );
    def->faceCount  = (int)sides + 2;
    def->faces = Face_Alloc_R( def->faceCount );

    GfxColor white;
    Byte4PackPixelColor( const_cast<float *>( colorWhite ), &white );

    const int a1ax = ( axis + 1 ) % 3;
    const int a2ax = ( axis + 2 ) % 3;

    // face[0] — top cap (axis = maxs), winding wound to face +axis.
    face_t *top = &def->faces[0];
    memcpy( top->mtldef, &g_qeglobals.random_texture_stuff[0].mtl, sizeof( MaterialDef ) );
    top->field_0xE4 = white.packed;
    top->planepts[0][a1ax] = maxs[a1ax]; top->planepts[0][a2ax] = maxs[a2ax]; top->planepts[0][axis] = maxs[axis];
    top->planepts[1][a1ax] = maxs[a1ax]; top->planepts[1][a2ax] = mins[a2ax]; top->planepts[1][axis] = maxs[axis];
    top->planepts[2][a1ax] = mins[a1ax]; top->planepts[2][a2ax] = mins[a2ax]; top->planepts[2][axis] = maxs[axis];

    // face[1] — bottom cap (axis = mins).
    face_t *bot = &def->faces[1];
    memcpy( bot->mtldef, &g_qeglobals.random_texture_stuff[0].mtl, sizeof( MaterialDef ) );
    bot->field_0xE4 = white.packed;
    bot->planepts[0][a1ax] = mins[a1ax]; bot->planepts[0][a2ax] = mins[a2ax]; bot->planepts[0][axis] = mins[axis];
    bot->planepts[1][a1ax] = maxs[a1ax]; bot->planepts[1][a2ax] = mins[a2ax]; bot->planepts[1][axis] = mins[axis];
    bot->planepts[2][a1ax] = maxs[a1ax]; bot->planepts[2][a2ax] = maxs[a2ax]; bot->planepts[2][axis] = mins[axis];

    const double dtheta = 6.283185482025146 / (double)sides;
    for ( unsigned int i = 0; i < sides; ++i )
    {
        face_t *f = &def->faces[2 + i];
        memcpy( f->mtldef, &g_qeglobals.random_texture_stuff[0].mtl, sizeof( MaterialDef ) );
        f->field_0xE4 = white.packed;

        float ang = (float)( (double)i * dtheta );
        float cv  = cosf( ang );
        float sv  = sinf( ang );

        f->planepts[0][a1ax] = cv * width + mid[a1ax];
        f->planepts[0][a2ax] = sv * width + mid[a2ax];
        if ( snap )
        {
            f->planepts[0][a1ax] = floorf( f->planepts[0][a1ax] + 0.5f );
            f->planepts[0][a2ax] = floorf( f->planepts[0][a2ax] + 0.5f );
        }
        f->planepts[0][axis] = mins[axis];

        f->planepts[1][a1ax] = f->planepts[0][a1ax];
        f->planepts[1][a2ax] = f->planepts[0][a2ax];
        f->planepts[1][axis] = maxs[axis];

        f->planepts[2][a1ax] = f->planepts[0][a1ax] - sv * width;
        f->planepts[2][a2ax] = cv * width + f->planepts[0][a2ax];
        if ( snap )
        {
            f->planepts[2][a1ax] = floorf( f->planepts[2][a1ax] + 0.5f );
            f->planepts[2][a2ax] = floorf( f->planepts[2][a2ax] + 0.5f );
        }
        f->planepts[2][axis] = maxs[axis];
    }

    Brush_SetDefaultMaterials( def );
    Brush_BuildWindings( def, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex ||
         g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++def->version;
    g_nUpdateBits = -1;
}

// ─── Brush_MakePhysBox (0x47C180) / Brush_MakePhysCylinder (0x47C310) ─────────
//  Physics→Box (36120) / Physics→Cylinder (36113).  Both REPLACE the single selected
//  brush with a freshly-allocated brush whose numberId tags it as
//  a physics primitive — 1 = box, 2 = cylinder — using the current MaterialDef from
//  g_qeglobals.random_texture_stuff[0] with the layer texture size seeded at width/4,
//  height/4 (the same seed the physics map-PARSE path uses, Brush_ParsePhysicsBox /
//  Brush_ParsePhysCylinder above).  The box keeps the source brush's AABB; the cylinder
//  squares the XY footprint to the LARGER of the two extents about the AABB centre and
//  then runs Brush_MakeSided_Prolog(16, 0) to cut the 16-sided barrel.
//  Keeps the binary's dead `&active_brushes == &selected_brushes` branch.
// ─────────────────────────────────────────────────────────────────────────────
void Brush_MakeSided_Prolog( unsigned int sides, char snap );  // below (0x4735E0)

// The shared tail of both handlers: seed a MaterialDef from the current texture, allocate
// + create the brush over [mins,maxs], tag it, link it into the source brush's entity,
// splice it into active_brushes, delete the old selection and select the new brush.
static selbrush_t *Brush_MakePhysPrimitive( float *mins, float *maxs,
                                            entity_s *owner, int physKind )
{
    MaterialDef mtl;
    // The binary's box and cylinder paths both clear the same trailing texdef block;
    // the lyrMtl/radMtl pair is written immediately after.
    memset( &mtl.mat_texDef, 0, sizeof( mtl.mat_texDef ) );
    mtl.lyrMtl = g_qeglobals.random_texture_stuff[0].mtl.lyrMtl;
    mtl.radMtl = g_qeglobals.random_texture_stuff[0].mtl.radMtl;

    qtexture_s *lm = MaterialDef_GetLayeredMaterial( &mtl );
    int w = lm ? lm->width  : 512;
    int h = lm ? lm->height : 512;
    texdef_sub_t *texSlot = &mtl.mat_texDef + LayerMat::GetCurrentLayer( &mtl );
    texSlot->size[0] = (double)w * 0.25;
    texSlot->size[1] = 0.25 * (double)h;

    brush_t *def = Brush_Alloc( &mtl, nullptr );
    Brush_Create( mins, maxs, def, nullptr );
    def->numberId = physKind;                       // 1 = box, 2 = cylinder

    entity_s *ownerDef = (entity_s *)owner->def;
    if ( ownerDef )
    {
        Entity_LinkBrush( ownerDef, (selbrush_t *)def );
        Brush_BuildWindings( def, 1 );
        if ( g_qeglobals.d_select_mode == sel_vertex ||
             g_qeglobals.d_select_mode == sel_edge )
            SetupVertexSelection();
        MarkMapModified();
        ++def->version;
    }

    selbrush_t *inst = Brush_AddToList( def, owner );
    if ( inst->next || inst->prev )
        Com_Error( ERR_FATAL, "Brush_AddToList: already linked" );
    if ( &active_brushes == &selected_brushes )     // never true; matches the binary
    {
        Brush_AddToList2( inst );
    }
    else
    {
        inst->next = active_brushes.next;
        active_brushes.next->prev = inst;
        active_brushes.next = inst;
        inst->prev = &active_brushes;
    }

    Select_Delete();
    Select_Brush( inst, 1, 1, 0 );
    return inst;
}

void Brush_MakePhysBox()          // 0x47C180
{
    if ( !QE_SingleBrush() )
        return;

    brush_t  *def   = selected_brushes.next->def;
    entity_s *owner = selected_brushes.next->owner;
    Brush_MakePhysPrimitive( def->mins, def->maxs, owner, 1 );
    g_nUpdateBits = -1;
}

void Brush_MakePhysCylinder()     // 0x47C310
{
    if ( !QE_SingleBrush() )
        return;

    brush_t  *def   = selected_brushes.next->def;
    entity_s *owner = selected_brushes.next->owner;

    // Square the XY footprint about the AABB centre using the LARGER extent.
    float cx = ( def->mins[0] + def->maxs[0] ) * 0.5f;
    float cy = ( def->mins[1] + def->maxs[1] ) * 0.5f;
    float ex = def->maxs[0] - def->mins[0];
    float ey = def->maxs[1] - def->mins[1];
    float side = ( ex - ey < 0.0f ) ? ey : ex;
    float half = 0.5f * side;

    float mins[3] = { cx - half, cy - half, def->mins[2] };
    float maxs[3] = { cx + half, cy + half, def->maxs[2] };

    Brush_MakePhysPrimitive( mins, maxs, owner, 2 );
    Brush_MakeSided_Prolog( 0x10u, 0 );
    g_nUpdateBits = -1;
}

// ─── Brush_MakeSided_Prolog (0x4735E0) ────────────────────────────────────────
// Picks the cylinder axis from the active XY-view orientation, then calls
// Brush_MakeSided on the single selected brush.  m_nViewType: 0=YZ→axis 0,
// 1=XZ→axis 1, 2=XY→axis 2 (no active view → axis 2).
// With an active XY view the binary re-inits axis=0, so an invalid viewType>=3 gives
// axis 0 (dead path).
void Brush_MakeSided_Prolog( unsigned int sides, char snap )
{
    if ( !QE_SingleBrush() )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Must have a single brush selected" );
        return;
    }

    int axis = 2;
    // Ed_ActiveXY() is never NULL, so the "no active view → axis 2" arm is gone.
    switch ( Ed_ActiveXY()->m_nViewType )
    {
    case 0: axis = 0; break;
    case 1: axis = 1; break;
    case 2: axis = 2; break;
    default: axis = 0; break;   // re-inits to 0 when an XY view exists
    }
    Brush_MakeSided( selected_brushes.next->def, sides, axis, snap );
}

// ─── Brush_MakeSidedCone (0x47BC10) ───────────────────────────────────────────
// Reshape the single selected brush into an N-sided cone (apex at +Z / maxs[2],
// base ring at mins[2]).  Axis is fixed to Z.  Always snaps to integer.
// cosf/sinf differ sub-ULP from the binary's (float)cos/sin, washed out by
// the floorf to integer planepts.
void Brush_MakeSidedCone( int sides )
{
    if ( sides < 3 )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Bad sideCount number" );
        return;
    }
    if ( !QE_SingleBrush() )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Must have a single brush selected" );
        return;
    }

    brush_t *def = selected_brushes.next->def;
    float mins[3], maxs[3], mid[2];
    Vec3Copy( def->mins, mins );
    Vec3Copy( def->maxs, maxs );

    float width = 8.0f;
    for ( int i = 0; i < 2; ++i )
    {
        mid[i] = ( maxs[i] + mins[i] ) * 0.5f;
        float ext = maxs[i] - mins[i];
        if ( width < ext )
            width = ext;
    }
    width = 0.5f * width;

    Face_Free( def->faceCount, def->faces );
    def->faceCount  = sides + 1;
    def->faces = Face_Alloc_R( def->faceCount );

    // face[0] — the base triangle (the bottom is closed by the side faces meeting
    // the base ring; the cap is a single plane through three base corners).
    face_t *base = &def->faces[0];
    memcpy( base->mtldef, &g_qeglobals.random_texture_stuff[0].mtl, sizeof( MaterialDef ) );
    base->planepts[0][0] = mins[0]; base->planepts[0][1] = mins[1]; base->planepts[0][2] = mins[2];
    base->planepts[1][0] = maxs[0]; base->planepts[1][1] = mins[1]; base->planepts[1][2] = mins[2];
    base->planepts[2][0] = maxs[0]; base->planepts[2][1] = maxs[1]; base->planepts[2][2] = mins[2];

    const double dtheta = 6.283185482025146 / (double)sides;
    for ( int i = 0; i < sides; ++i )
    {
        face_t *f = &def->faces[1 + i];
        memcpy( f->mtldef, &g_qeglobals.random_texture_stuff[0].mtl, sizeof( MaterialDef ) );

        float ang = (float)( (double)i * dtheta );
        float cv  = cosf( ang );
        float sv  = sinf( ang );

        f->planepts[0][0] = floorf( cv * width + mid[0] + 0.5f );
        f->planepts[0][1] = floorf( sv * width + mid[1] + 0.5f );
        f->planepts[0][2] = mins[2];

        f->planepts[1][0] = mid[0];      // apex
        f->planepts[1][1] = mid[1];
        f->planepts[1][2] = maxs[2];

        f->planepts[2][0] = floorf( f->planepts[0][0] - sv * width + 0.5f );
        f->planepts[2][1] = floorf( cv * width + f->planepts[0][1] + 0.5f );
        f->planepts[2][2] = maxs[2];
    }

    Brush_SetDefaultMaterials( def );
    Brush_BuildWindings( def, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex ||
         g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++def->version;
    g_nUpdateBits = -1;
}

// ─── Brush_MakeSidedSphere (0x47BE90) ─────────────────────────────────────────
// Reshape the single selected brush into a sphere built from sides×sides faces
// (sides bands × sides segments).  Each face's three planepts are spherical
// points (Sphere_Point) offset by the brush centre.  Needs sides >= 4.
void Brush_MakeSidedSphere( int sides )
{
    if ( sides < 4 )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Bad sideCount number" );
        return;
    }
    if ( !QE_SingleBrush() )
    {
        SendMessageA( g_qeglobals.d_hwndStatus, WM_USER | WM_CREATE, 0,
                      (LPARAM)"Must have a single brush selected" );
        return;
    }

    brush_t *def = selected_brushes.next->def;
    float mins[3], maxs[3], mid[3];
    Vec3Copy( def->mins, mins );
    Vec3Copy( def->maxs, maxs );

    float radius = 8.0f;
    for ( int i = 0; i < 2; ++i )
    {
        mid[i] = ( maxs[i] + mins[i] ) * 0.5f;
        float ext = maxs[i] - mins[i];
        if ( radius < ext )
            radius = ext;
    }
    radius = 0.5f * radius;
    mid[2] = ( maxs[2] + mins[2] ) * 0.5f;   // mid = the brush centre

    Face_Free( def->faceCount, def->faces );
    def->faceCount  = sides * sides;
    def->faces = Face_Alloc_R( def->faceCount );

    const double dphi = 6.283185482025146 / (double)sides;
    face_t *f = def->faces;

    // Body bands: outer i in [0, sides), inner j runs (sides-1) times.  IMPORTANT
    // (a CoD quirk kept as-is, NOT a port bug): the inner loop does NOT vary the
    // angle — it recomputes the same face (ang0 = i·dphi for planepts[0]/[1], ang1 =
    // ang0+dphi for planepts[2]) (sides-1) times.
    for ( int i = 0; i < sides; ++i )
    {
        float ang0 = (float)( (double)i * dphi );
        float ang1 = (float)( (double)dphi + (double)ang0 );
        for ( int j = 0; j < sides - 1; ++j )
        {
            memcpy( f->mtldef, &g_qeglobals.random_texture_stuff[0].mtl, sizeof( MaterialDef ) );

            Sphere_Point( f->planepts[0], radius, ang0 );
            Sphere_Point( f->planepts[1], radius, ang0 );
            Sphere_Point( f->planepts[2], radius, ang1 );
            for ( int k = 0; k < 3; ++k )
            {
                f->planepts[k][0] += mid[0];
                f->planepts[k][1] += mid[1];
                f->planepts[k][2] += mid[2];
            }
            ++f;
        }
    }
    // Cap band: one face per segment (the tail loop, sides faces).
    for ( int i = 0; i < sides; ++i )
    {
        memcpy( f->mtldef, &g_qeglobals.random_texture_stuff[0].mtl, sizeof( MaterialDef ) );
        float ang0 = (float)( (double)i * dphi );
        float ang1 = (float)( ang0 + (double)dphi );
        Sphere_Point( f->planepts[0], radius, ang0 );
        Sphere_Point( f->planepts[1], radius, ang1 );
        Sphere_Point( f->planepts[2], radius, ang1 );
        for ( int k = 0; k < 3; ++k )
        {
            f->planepts[k][0] += mid[0];
            f->planepts[k][1] += mid[1];
            f->planepts[k][2] += mid[2];
        }
        ++f;
    }

    Brush_SetDefaultMaterials( def );
    Brush_BuildWindings( def, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex ||
         g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++def->version;
    g_nUpdateBits = -1;
}

// ════════════════════════════════════════════════════════════════════════════
//  13. Brush_ParsePhysCylinder  (0x473950)
// ════════════════════════════════════════════════════════════════════════════
// MakeSided(16); vectoangles reorder {roll,pitch,yaw}.
static brush_t *Brush_ParsePhysCylinder( const char **text )
{
    if ( Com_Parse( text )->token[0] != '{' )
        return nullptr;

    float vec[3];
    vec[0] = (float)Com_ParseFloat( text );
    parseInfo_t *t = Com_ParseOnLine( text );
    vec[1] = (float)atof( t->token );
    t = Com_ParseOnLine( text ); vec[2] = (float)atof( t->token );

    // center[3] = cylinder center; length along X axis, radius on Y/Z.
    float center[3];
    t = Com_ParseOnLine( text ); center[0] = (float)atof( t->token );
    t = Com_ParseOnLine( text ); center[1] = (float)atof( t->token );
    t = Com_ParseOnLine( text ); center[2] = (float)atof( t->token );
    t = Com_ParseOnLine( text ); float length = (float)atof( t->token );
    t = Com_ParseOnLine( text ); float radius = (float)atof( t->token );
    Com_MatchToken( text, "}", 0 );

    double halfLen = length * 0.5;
    MaterialDef material;
    memset( &material.mat_texDef, 0, sizeof(material.mat_texDef) );

    // KIWI: built about the origin, then placed by PhysBrush_Place (below).
    float mins[3], maxs[3];
    mins[0] = -(float)halfLen;
    mins[1] = -radius;
    mins[2] = -radius;
    maxs[0] = (float)halfLen;
    maxs[1] = radius;
    maxs[2] = radius;

    material.lyrMtl = g_qeglobals.random_texture_stuff[0].mtl.lyrMtl;
    material.radMtl = g_qeglobals.random_texture_stuff[0].mtl.radMtl;

    qtexture_s *lm = MaterialDef_GetLayeredMaterial( &material );
    int w = lm ? lm->width  : 512;
    int h = lm ? lm->height : 512;

    texdef_sub_t *texSlot = &material.mat_texDef + LayerMat::GetCurrentLayer( &material );
    texSlot->size[0] = (double)w * 0.25;
    texSlot->size[1] = (double)h * 0.25;

    brush_t *b = Brush_Alloc( &material, nullptr );
    Brush_Create( mins, maxs, b, nullptr );
    b->numberId = 2;
    Brush_BuildWindings( b, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex ||
         g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++b->version;

    Brush_MakeSided( b, 16u, 0, 0 );

    // Orient the 16-sided cylinder along the parsed axis: vectoangles -> (pitch, yaw, 0) and
    // its frame.  KIWI: that frame in double (KiwiAng_ToAxis), placed like the box.
    double ang[3] = { 0.0, 0.0, 0.0 };
    if ( vec[0] == 0.0f && vec[1] == 0.0f )
        ang[0] = vec[2] > 0.0f ? 270.0 : 90.0;           // Radiant_VecToAngles' straight up / down
    else
    {
        ang[0] = -atan2( (double)vec[2], sqrt( (double)vec[0] * vec[0] + (double)vec[1] * vec[1] ) ) * KIWI_RAD2DEG;
        ang[1] = atan2( (double)vec[1], (double)vec[0] ) * KIWI_RAD2DEG;
    }
    double frame[3][3];
    KiwiAng_ToAxis( ang, frame );
    PhysBrush_Place( b, frame, center );

    Brush_BuildWindings( b, 1 );
    {
        const float vals[8] = { vec[0], vec[1], vec[2], center[0], center[1], center[2], length, radius };
        PhysParsed_Store( b, vals, 8 );
    }
    if ( g_qeglobals.d_select_mode == sel_vertex ||
         g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++b->version;
    return b;
}

// ════════════════════════════════════════════════════════════════════════════
//  11. Brush_Parse  (0x473bd0)
//  Main brush text parser. Parses one brush block from the map text.
// ════════════════════════════════════════════════════════════════════════════
// ParseEntity (entity.cpp, 0x483e70) calls this as Brush_Parse(text, version).
// version<4 reads radMtl->size[0]/[1] as INT, not float.
brush_t *Brush_Parse( const char **text, int version )
{
    // File-static rather than a 512-face stack buffer (non-reentrant — map parse is
    // serial).
    static face_t s_faceStack[512];

    ++g_qeglobals.d_parsed_brushes;

    // First token: "patchDef5"/"curve"+"mesh"/"patchTerrainDef3" | physics_* | brush faces.
    parseInfo_t *tok5 = Com_Parse( text );
    const char *first = tok5->token;
    int patchCmp;
    const char *meshKeyword;
    if ( version )
    {
        patchCmp    = _stricmp( tok5->token, "curve" );
        meshKeyword = "mesh";
    }
    else
    {
        patchCmp    = _stricmp( tok5->token, "patchDef5" );
        meshKeyword = "patchTerrainDef3";
    }
    bool isPatch = (patchCmp == 0);
    bool isMesh  = (_stricmp( first, meshKeyword ) == 0);

    if ( isPatch || isMesh )
    {
        // PATCH / MESH brush — parse the real mesh/curve block (pmesh.cpp).
        // isMesh = the token matched the terrain variant ("mesh" for v>=1 /
        // "patchTerrainDef3" for v0); Patch_ParseMesh sets PATCH_TERRAIN for it.
        // Patch_ParseMesh consumes through the mesh-block close '}'; Brush_Parse then
        // reads the brush's outer '}'.
        brush_t *patchBrush = Patch_ParseMesh( text, version, isMesh ? 1 : 0 );
        parseInfo_t *closer = Com_Parse( text );   // the brush's outer '}'
        if ( !patchBrush || !closer->token[0] || strcmp( closer->token, "}" ) )
        {
            Error( "parsing patch/brush" );
            return nullptr;
        }
        return patchBrush;
    }

    if ( !_stricmp( first, "physics_cylinder" ) )
    {
        brush_t *cyl = Brush_ParsePhysCylinder( text );
        parseInfo_t *closer = Com_Parse( text );
        if ( !cyl || !closer->token[0] || strcmp( closer->token, "}" ) )
        {
            Error( "parsing physics cylinder" );
            return nullptr;
        }
        return cyl;
    }

    if ( !_stricmp( first, "physics_box" ) )
    {
        brush_t *box = Brush_ParsePhysicsBox( text );
        parseInfo_t *closer = Com_Parse( text );
        if ( !box || !closer->token[0] || strcmp( closer->token, "}" ) )
        {
            Error( "parsing physics box" );
            return nullptr;
        }
        return box;
    }

    Com_UngetToken();

    char layerStr[1028];
    int  contents = 0, toolFlags = 0;
    if ( version >= 4 )
    {
        Map_ParseEntityLayerKey( text, "000_Global", layerStr, "layer" );
        contents  = Map_ParseContentsLine( text );
        toolFlags = Map_ParseToolFlagsLine( text );
    }
    else
    {
        strcpy( layerStr, "000_Global" );
    }

    int faceCount = 0;
    face_t *nextFace = s_faceStack;

    while ( true )
    {
        if ( Com_Parse( text )->token[0] == '}' )
        {
            // Done — build the brush
            brush_t *nb = (brush_t *)operator new( sizeof(brush_t) );
            memset( nb, 0, sizeof(brush_t) );
            nb->faceCount = faceCount;
            nb->faces     = Face_Alloc_R( faceCount );
            if ( nb->parent_layer_string )                 // always null after the memset
                j__free_0( nb->parent_layer_string );      // (binary's inlined SetLayerString head)
            size_t ll = strlen( layerStr );
            void *lc = operator new( ll + 1 );
            memcpy( lc, layerStr, ll + 1 );
            nb->parent_layer_string = (char *)lc;
            memcpy( nb->faces, s_faceStack, sizeof(face_t) * faceCount );
            return nb;
        }

        Com_UngetToken();   // unget the token we just tested for '}'

        if ( faceCount == 512 )
        {
            Error( "too many faces in brush" );
            return nullptr;
        }

        face_t *facePtr = nextFace;
        ++nextFace;
        int nextCount = faceCount + 1;
        facePtr->w = NULL;

        // Read 3 sets of planepts: "( x y z )"
        for ( int i = 0; i < 3; ++i )
        {
            parseInfo_t *ptok = i ? Com_ParseOnLine( text ) : Com_Parse( text );
            if ( ptok->token[0] != '(' )
            {
                Error( "missing '(' parsing brush" );
                return nullptr;
            }
            float *coords = facePtr->planepts[i];
            for ( int j = 0; j < 3; ++j )
            {
                parseInfo_t *num = Com_ParseOnLine( text );
                coords[j] = (float)atof( num->token );
            }
            parseInfo_t *ptok2 = Com_ParseOnLine( text );
            if ( ptok2->token[0] != ')' )
            {
                Error( "missing ')' parsing brush" );
                return nullptr;
            }
        }

        int fmt = version;
        if ( version < 4 )
        {
            Byte4PackPixelColor( const_cast<float*>(colorWhite),
                                 (GfxColor *)&facePtr->packedColor );
            if ( Face_ParseSurfDef( text, &facePtr->mtldef[0], version, 0 ) )
            {
                face_t *f = facePtr;                    // the binary's local
                iassert( f->surfDef[0].mtlDef.radMtl );            // brush.cpp:1925
                iassert( f->surfDef[0].mtlDef.radMtl->size[0] );   // brush.cpp:1926
                iassert( f->surfDef[0].mtlDef.radMtl->size[1] );   // brush.cpp:1927

                int curLayer2 = LayerMat::GetCurrentLayer( &facePtr->mtldef[0] );
                // NOTE the texdef_sub_t stride from mtldef[0].mat_texDef, not the
                // MaterialDef stride.
                texdef_sub_t *tdl =
                    &facePtr->mtldef[0].mat_texDef + curLayer2;
                float size0 = tdl->size[0];
                tdl->shift[0] = -size0 * tdl->shift[0];
                tdl->shift[1] = -tdl->size[1] * tdl->shift[1];
                // radMtl's width/height are ints, NOT a float reinterpret.
                qtexture_s *rm = facePtr->mtldef[0].radMtl;
                tdl->size[0] = (float)( (double)rm->width  * tdl->size[0] );
                tdl->size[1] = (float)( (double)rm->height * tdl->size[1] );
                tdl->sample_size = 0.0f;   // int-0 store in the binary; same bits
                fmt = version;
            }
            else
            {
                return nullptr;
            }
        }

        // face contents/flags words
        if ( fmt > 3 )
        {
            facePtr->contents = contents;
        }
        else
        {
            parseInfo_t *tok = Com_ParseOnLine( text );
            facePtr->contents = (int)atol( tok->token );
        }

        bool doSampleSize;   // sample size + lightmap + Init_MaterialLayer
        if ( fmt == 3 )
        {
            parseInfo_t *tok = Com_ParseOnLine( text );
            facePtr->toolflags = (int)atol( tok->token );
            doSampleSize = true;
        }
        else
        {
            facePtr->toolflags = toolFlags;
            if ( fmt <= 1 )
                Com_ParseOnLine( text );
            if ( !fmt )
            {
                Com_ParseOnLine( text );
                doSampleSize = true;
            }
            else
            {
                doSampleSize = ( fmt < 4 );   // true for fmt == 1, 2
            }
        }

        if ( doSampleSize )
        {
            parseInfo_t *tok = Com_ParseOnLine( text );
            int sampleSize = (int)atol( tok->token );
            if ( sampleSize <= 0 ) sampleSize = 16;

            Face_SetDefaultMaterials( facePtr );
            SetMaterial( "lightmap_gray", (patchMesh_material *)&facePtr->mtldef[1] );
            // Init_MaterialLayer takes the sample size as a float.
            float sampleSizeF = (float)sampleSize;
            Init_MaterialLayer( &facePtr->mtldef[1], sampleSizeF );
        }
        else
        {
            // version >= 4: read up to 3 surface defs (radiosity / lightmap / smoothing)
            int chanIdx = 0;
            MaterialDef *mdPtr = facePtr->mtldef;
            while ( Face_ParseSurfDef( text, mdPtr, version, chanIdx ) )
            {
                ++chanIdx;
                ++mdPtr;
                if ( chanIdx >= 3 )
                    break;
            }
            if ( chanIdx < 3 )   // a surf def failed to parse before reaching 3 → error
                return nullptr;
        }

        faceCount = nextCount;
    }
}

// ════════════════════════════════════════════════════════════════════════════
//  14. Brush_Write  (0x474e90)
//  Writes a brush to the map output stream.
// ════════════════════════════════════════════════════════════════════════════
// The writer is a triple-pointer handle whose ctx is the handle itself.
typedef int WriteFunc_t( void *ctx, const char *fmt, ... );
typedef WriteFunc_t **WriteWriter_t;
// Convenience: WRITE(w, fmt, ...) = (**w)(w, fmt, ...)
#define WRITE(w, ...) ((**w)((w), __VA_ARGS__))

// Write sub-function forward declarations (defined as static below)
static int Brush_WritePhysicsBox( brush_t *b, WriteWriter_t writer );
static int Brush_WritePhysCylinder( brush_t *b, WriteWriter_t writer );

// MapLoad helpers needed by Brush_Write
extern int  Patch_Write( WriteWriter_t writer, patchMesh_t *patch );   // pmesh.cpp 0x4458f0
extern void MapLoad_ParseBrush_Layer( WriteWriter_t writer, const char *layerStr );
extern void MapLoad_ParseBrush_Content( const char *name, WriteWriter_t writer,
                                         int val, void *table );
extern int *contents_table;
extern int *toolflags_table;

// VC7.1 / pre-UCRT printf vs modern UCRT differ in TWO ways that break byte-exact
// round-trips of stock .map files (written by the VC7.1 CoD4Radiant):
//   (1) EXPONENT WIDTH — VC7.1 emits a MINIMUM 3-digit exponent ("7.1e-005"); UCRT
//       emits 2 ("7.1e-05") per C99.
//   (2) HALF ROUNDING — VC7.1's "%.8g" rounds ties AWAY from zero; UCRT rounds ties
//       to EVEN. A float that lands exactly on an 8th-significant-figure half breaks:
//       e.g. the float 2845.15625 ⇒ "-2845.1563" (VC7.1) vs "-2845.1562" (UCRT).
// Fmt8g reproduces VC7.1: detect the exact tie, round the magnitude up, then pad the
// exponent. Non-tie values are byte-identical to plain "%.8g", so only the handful of
// exact-half coordinates change. (Non-static — pmesh.cpp's patch writer shares it.)
const char *Fmt8g( char *buf, double v )
{
    sprintf( buf, "%.8g", v );

    if ( v != 0.0 )
    {
        double av = ( v < 0.0 ) ? -v : v;
        char   ebuf[32];
        sprintf( ebuf, "%.7e", av );                 // reliable base-10 exponent ('e' present
        char  *ec = strchr( ebuf, 'e' );             //  only for finite values — guards inf/nan)
        if ( ec )
        {
            int e        = atoi( ec + 1 );
            int decimals = 7 - e;                     // %g fixed-notation decimals for 8 sig figs
            if ( decimals >= 0 && decimals <= 18 )
            {
                double scale  = pow( 10.0, (double)decimals );
                double scaled = av * scale;
                double fl     = floor( scaled );
                if ( scaled - fl == 0.5 )             // exact tie at the 8th significant figure
                {
                    double awayMag = ( fl + 1.0 ) / scale;
                    sprintf( buf, "%.8g", ( v < 0.0 ) ? -awayMag : awayMag );
                }
            }
        }
    }

    // KIWI (2026-10-05, load/save gauntlet): 8 significant digits do not hold every float
    // (1000.00006 saved as 1000.0001, the next float up), so a save + reload moved plane
    // points and patch vertices by an ulp.  When 8 digits do not read back as the same float,
    // write 9 - which always do.  Values that survive 8 digits (every stock file's) are
    // written exactly as before.
    if ( (float)strtod( buf, nullptr ) != (float)v )
        sprintf( buf, "%.9g", v );

    char *e = strchr( buf, 'e' );
    if ( e && ( e[1] == '+' || e[1] == '-' ) )
    {
        int  expval = atoi( e + 1 );                 // signed exponent
        char sign   = ( expval < 0 ) ? '-' : '+';
        int  mag    = ( expval < 0 ) ? -expval : expval;
        sprintf( e + 1, "%c%03d", sign, mag );       // e-05 -> e-005 ; e-100 stays e-100
    }
    return buf;
}

// Fmt8g(%.8g) over 9 planepts + 6 texdef floats.
int Brush_Write( WriteWriter_t writer, brush_t *brush )
{
    if ( brush->patch )
        return Patch_Write( writer, brush->patch );

    int tot = brush->numberId;
    if ( tot == 2 )
        return Brush_WritePhysCylinder( brush, writer );
    if ( tot == 1 )
        return Brush_WritePhysicsBox( brush, writer );

    // Normal brush
    WRITE( writer,"{\n" );
    // Every active brush should carry a layer.  KIWI: a NULL one is written as 000_Global
    // (no layer line) instead of crashing the save.
    if ( brush->parent_layer_string && strcmp( brush->parent_layer_string, "000_Global" ) )
    {
        MapLoad_ParseBrush_Layer( writer, brush->parent_layer_string );
    }

    MapLoad_ParseBrush_Content( "contents",  writer,
                                 brush->faces->contents,  contents_table );
    MapLoad_ParseBrush_Content( "toolFlags",  writer,
                                 brush->faces->toolflags, toolflags_table );

    for ( int fi = 0; fi < brush->faceCount; ++fi )
    {
        face_t *f = &brush->faces[fi];
        float *pp = f->planepts[0];
        for ( int pt = 0; pt < 3; ++pt )
        {
            WRITE( writer," (" );
            for ( int c = 0; c < 3; ++c )
            {
                char nb[40];
                WRITE( writer," %s", Fmt8g( nb, *pp++ ) );
            }
            WRITE( writer," )" );
        }
        for ( int ch = 0; ch < 3; ++ch )
        {
            MaterialDef *md = &f->mtldef[ch];
            // the binary inlines Materialdef_GetName here (MaterialDef.cpp:85 lives in it)
            const char *name    = (const char *)Materialdef_GetName( md );
            char       *mtlName = (char *)name;

            float *texdef = &md->mat_texDef.size[ 7 * LayerMat::GetCurrentLayer( md ) ];
            iassert( mtlName[0] );

            if ( ch == 2 )
            {
                if ( strcmp( name, "smoothing_hard" ) )
                    WRITE( writer," smoothing %s", name );
            }
            else
            {
                char nb[6][40];
                WRITE( writer," %s %s %s %s %s %s %s",
                            mtlName,
                            Fmt8g( nb[0], texdef[0] ), Fmt8g( nb[1], texdef[1] ), Fmt8g( nb[2], texdef[2] ),
                            Fmt8g( nb[3], texdef[3] ), Fmt8g( nb[4], texdef[4] ), Fmt8g( nb[5], texdef[5] ) );
            }
        }
        WRITE( writer,"\n" );
    }
    return WRITE( writer,"}\n" );
}

// ════════════════════════════════════════════════════════════════════════════
//  15. Brush_WritePhysicsBox  (0x4742c0)
// ════════════════════════════════════════════════════════════════════════════
// The binary's axis[3][3] box frame is assembled alongside the scalar math so the
// brush.cpp:1995/2001/2007/2008/2009 asserts keep their Vec3Dot/I_fabs conditions.
static int Brush_WritePhysicsBox( brush_t *b, WriteWriter_t file )
{
    iassert( b );      // brush.cpp:1988
    iassert( file );   // brush.cpp:1989
    iassert( b->faceCount == 6 );   // brush.cpp:1990

    face_t *faces = b->faces;
    float cx = (b->mins[0] + b->maxs[0]) * 0.5f;
    float cy = (b->mins[1] + b->maxs[1]) * 0.5f;
    float cz = (b->mins[2] + b->maxs[2]) * 0.5f;

    // axis[2] = faces[1].plane.normal
    float axis[3][3];                     // the binary's box frame (assert strings)
    float a2x = faces[1].plane.normal[0];
    float a2y = faces[1].plane.normal[1];
    float a2z = faces[1].plane.normal[2];
    axis[2][0] = a2x; axis[2][1] = a2y; axis[2][2] = a2z;

    iassert( Vec3Dot( axis[2], b->faces[0].plane.normal ) < -0.999f );   // brush.cpp:1995

    // Compute half-extents along axis[2] direction
    float d0 = faces[1].planepts[0][0]*a2x + faces[1].planepts[0][1]*a2y + faces[1].planepts[0][2]*a2z;
    float d1 = faces[0].planepts[0][0]*a2x + faces[0].planepts[0][1]*a2y + faces[0].planepts[0][2]*a2z;
    float halfZ = fabsf( d0 - d1 ) * 0.5f;

    // axis[1] = faces[4].plane.normal
    float a1x = faces[4].plane.normal[0];
    float a1y = faces[4].plane.normal[1];
    float a1z = faces[4].plane.normal[2];
    axis[1][0] = a1x; axis[1][1] = a1y; axis[1][2] = a1z;

    iassert( Vec3Dot( axis[1], b->faces[2].plane.normal ) < -0.999f );   // brush.cpp:2001

    float e0 = faces[4].planepts[0][0]*a1x + faces[4].planepts[0][1]*a1y + faces[4].planepts[0][2]*a1z;
    float e1 = faces[2].planepts[0][0]*a1x + faces[2].planepts[0][1]*a1y + faces[2].planepts[0][2]*a1z;
    float halfY = 0.5f * fabsf( e0 - e1 );

    // axis[0] = cross(axis[1], axis[2]).  KIWI: the port passed &a1x / &a2x - separate
    // locals, not arrays - so the box's X axis and half size were stack garbage.
    float out[3];
    Vec3Cross( axis[1], axis[2], out );
    axis[0][0] = out[0]; axis[0][1] = out[1]; axis[0][2] = out[2];

    iassert( I_fabs( Vec3Dot( axis[0], b->faces[3].plane.normal ) ) > 0.999f );   // brush.cpp:2007
    iassert( I_fabs( Vec3Dot( axis[0], b->faces[5].plane.normal ) ) > 0.999f );   // brush.cpp:2008
    iassert( Vec3Dot( b->faces[3].plane.normal, b->faces[5].plane.normal ) < -0.999f );   // brush.cpp:2009

    float f3d = faces[3].planepts[0][0]*out[0] + faces[3].planepts[0][1]*out[1] + faces[3].planepts[0][2]*out[2];
    float f5d = faces[5].planepts[0][0]*out[0] + faces[5].planepts[0][1]*out[1] + faces[5].planepts[0][2]*out[2];
    float halfX = fabsf( f3d - f5d ) * 0.5f;

    // KIWI: exact text (%f kept 6 decimals: a 30 deg axis read back 1e-7 off); centre /
    // half sizes within float noise of the half grid snap onto it, as the cylinder.
    const double scale = fabs( cx ) + fabs( cy ) + fabs( cz ) + halfX + halfY + halfZ;
    float vals[15] = { out[0], out[1], out[2], a1x, a1y, a1z, a2x, a2y, a2z,
                       KiwiSnap_NoiseHalf( cx, scale ), KiwiSnap_NoiseHalf( cy, scale ),
                       KiwiSnap_NoiseHalf( cz, scale ), KiwiSnap_NoiseHalf( halfX, scale ),
                       KiwiSnap_NoiseHalf( halfY, scale ), KiwiSnap_NoiseHalf( halfZ, scale ) };
    if ( const float *parsed = PhysParsed_Find( b ) )
        memcpy( vals, parsed, sizeof( vals ) );            // untouched since the parse
    char line[15 * 32 + 16] = "     ";
    for ( int i = 0; i < 15; ++i )
    {
        char num[32];
        strcat( line, " " );
        strcat( line, KiwiFmt_Exact( num, sizeof( num ), vals[i] + 0.0f, 0 ) );
    }
    WRITE( file,"  {\n    physics_box\n    {\n" );
    WRITE( file,"%s\n", line );
    return WRITE( file,"    }\n  }\n" );
}

// ════════════════════════════════════════════════════════════════════════════
//  16. Brush_WritePhysCylinder  (0x474800)
//  Writes a physics_cylinder brush.  Recovers the cylinder axis/center/length/radius
//  from the N-sided prism's faces: faces[0]/faces[1] are the two end caps, faces[2..]
//  the side faces.  The radius divisor is (faceCount - 2) (the side-face count), not
//  faceCount.
// ════════════════════════════════════════════════════════════════════════════
static int Brush_WritePhysCylinder( brush_t *b, WriteWriter_t file )
{
    iassert( b );      // brush.cpp:2038
    iassert( file );   // brush.cpp:2039
    iassert( b->faceCount > 3 );   // brush.cpp:2040

    face_t *faces     = b->faces;
    int     faceCount = b->faceCount;

    // KIWI: the binary took the axis from the cap's float normal and the centre from float
    // pairwise side-face line intersections, so every save + reopen moved both (7th digit).
    // Brush_MakeSided's side faces each run planepts[0] (bottom) -> planepts[1] (top) along
    // the axis, tangent to the circle at planepts[0]: averaged over all of them in double,
    // the axis, length, centre and radius come back to the float the parse placed.
    double run[3] = { 0.0, 0.0, 0.0 }, mid[3] = { 0.0, 0.0, 0.0 };
    const int sides = faceCount - 2;
    for ( int f = 2; f < faceCount; ++f )
    {
        const float *p0 = faces[f].planepts[0], *p1 = faces[f].planepts[1];
        for ( int k = 0; k < 3; ++k )
        {
            run[k] += (double)p1[k] - p0[k];
            mid[k] += 0.5 * ( (double)p0[k] + p1[k] );
        }
    }
    const double runLen = sqrt( run[0] * run[0] + run[1] * run[1] + run[2] * run[2] );
    double axisD[3], centre[3];
    for ( int k = 0; k < 3; ++k )
    {
        axisD[k]  = runLen > 0.0 ? run[k] / runLen : faces[0].plane.normal[k];
        centre[k] = mid[k] / sides;
    }
    const double length = runLen / sides;

    // Radius = mean distance from the centre to each side-face plane (double normals).
    double radiusSum = 0.0;
    for ( int f = 2; f < faceCount; ++f )
    {
        const float *p0 = faces[f].planepts[0], *p1 = faces[f].planepts[1], *p2 = faces[f].planepts[2];
        const double u[3] = { (double)p1[0] - p0[0], (double)p1[1] - p0[1], (double)p1[2] - p0[2] };
        const double v[3] = { (double)p2[0] - p0[0], (double)p2[1] - p0[1], (double)p2[2] - p0[2] };
        double n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        const double nl = sqrt( n[0] * n[0] + n[1] * n[1] + n[2] * n[2] );
        if ( nl > 0.0 )
            radiusSum += fabs( ( n[0] * ( p0[0] - centre[0] ) + n[1] * ( p0[1] - centre[1] )
                               + n[2] * ( p0[2] - centre[2] ) ) / nl );
    }
    const double radius = radiusSum / sides;

    // KIWI: exact text, as the box; centre / sizes within float noise of the half grid
    // snap onto it (the placement rounds at the coordinates' magnitude, ~1 ulp).
    const double scale = fabs( centre[0] ) + fabs( centre[1] ) + fabs( centre[2] ) + length + radius;
    float vals[8] = { (float)axisD[0], (float)axisD[1], (float)axisD[2],
                      KiwiSnap_NoiseHalf( (float)centre[0], scale ),
                      KiwiSnap_NoiseHalf( (float)centre[1], scale ),
                      KiwiSnap_NoiseHalf( (float)centre[2], scale ),
                      KiwiSnap_NoiseHalf( (float)length, scale ),
                      KiwiSnap_NoiseHalf( (float)radius, scale ) };
    if ( const float *parsed = PhysParsed_Find( b ) )
        memcpy( vals, parsed, sizeof( vals ) );            // untouched since the parse
    char line[8 * 32 + 16] = "     ";
    for ( int i = 0; i < 8; ++i )
    {
        char num[32];
        strcat( line, " " );
        strcat( line, KiwiFmt_Exact( num, sizeof( num ), vals[i] + 0.0f, 0 ) );
    }
    WRITE( file,"  {\n    physics_cylinder\n    {\n" );
    WRITE( file,"%s\n", line );
    return WRITE( file,"    }\n  }\n" );
}

// ════════════════════════════════════════════════════════════════════════════
//  Geometry (vertex/face topology): Brush_Convex / Brush_SplitBrushByFace /
//  Brush_MoveVertex, plus the brush.cpp deps Face_MakePlane /
//  Brush_RemoveWindinglessFaces / Brush_RemoveDuplicateFaces.
// ════════════════════════════════════════════════════════════════════════════

extern unsigned int Brush_ApplyEntityColor( brush_t *b );   // 0x475110 (defined below, brush.cpp:2167)

// ─── Face_MakePlane  (0x470470) ─────────────────────────────────────────────
// Recomputes face->plane from its 3 planepts, normalises, sets face->unk03 (the
// major-axis index 0..3) and returns it.
// plane.unk is unused; its readers reconstitute (double)dist.
// The no-normal warning uses a VectorCompare epsilon, not an exact ==0.0f
// (diagnostic only).
int Face_MakePlane( face_t *face )
{
    float e1[3], e2[3];
    e1[0] = face->planepts[0][0] - face->planepts[1][0];
    e1[1] = face->planepts[0][1] - face->planepts[1][1];
    e1[2] = face->planepts[0][2] - face->planepts[1][2];
    e2[0] = face->planepts[2][0] - face->planepts[1][0];
    e2[1] = face->planepts[2][1] - face->planepts[1][1];
    e2[2] = face->planepts[2][2] - face->planepts[1][2];
    Vec3Cross( e1, e2, face->plane.normal );
    // VectorCompare(face->plane.normal, vec3_origin) — an EPSILON test (component squared
    // > ~1e-6 => not degenerate), NOT an exact ==0 check.
    bool degenerate = true;
    for ( int i = 0; i < 3; ++i )
        if ( (double)face->plane.normal[i] * (double)face->plane.normal[i] > 0.00000100000011116208 )
        { degenerate = false; break; }
    if ( degenerate )
        printf( "WARNING: brush plane with no normal\n" );
    Vec3Normalize_R( face->plane.normal );
    // dist is a float store.
    face->plane.dist = face->planepts[1][1] * face->plane.normal[1]
                     + face->plane.normal[0] * face->planepts[1][0]
                     + face->planepts[1][2] * face->plane.normal[2];
    int axis;
    if ( face->plane.normal[0] == 1.0f )
        axis = 0;
    else if ( face->plane.normal[1] == 1.0f )
        axis = 1;
    else
    {
        axis = 2;
        if ( face->plane.normal[2] != 1.0f )
            axis = 3;
    }
    face->unk03 = axis;
    return axis;
}

// ─── Brush_RemoveWindinglessFaces  (0x471720) ───────────────────────────────────
// Removes faces with a null winding. Brush_RemoveFace shifts the array down, so
// the index is not advanced on removal; faceCount is re-read each iteration.
void Brush_RemoveWindinglessFaces( brush_t *b )
{
    unsigned int i = 0;
    while ( i < (unsigned int)b->faceCount )
    {
        if ( b->faces[i].w )
            ++i;
        else
            Brush_RemoveFace( b, i );
    }
}

// ─── Brush_RemoveDuplicateFaces  (0x471850) ───────────────────────────────────
// Removes coincident faces: Brush_FaceIndexCmp(j,b,i)==0 means face[j] has the
// same plane as face[i] (a duplicate) → drop it.  The outer i advances every iteration
// (separate j local); faceCount is compared unsigned.
bool Brush_RemoveDuplicateFaces( brush_t *b )
{
    bool changed = false;
    for ( unsigned int i = 0; i + 1 < (unsigned int)b->faceCount; ++i )
    {
        unsigned int j = i + 1;
        while ( j < (unsigned int)b->faceCount )
        {
            if ( Brush_FaceIndexCmp( j, b, i ) )   // nonzero = differ → keep
            {
                ++j;
            }
            else
            {
                Brush_RemoveFace( b, j );          // identical plane → drop dup
                changed = true;
            }
        }
    }
    return changed;
}

// ─── Brush_Convex  (0x471B60) ───────────────────────────────────────────────
// True if no ordered pair of face windings is concave w.r.t. the other's plane.
// Winding_PlanesConcave takes (w_i, w_j, normal_j, normal_i, dist_i, dist_j), in that
// order.
bool Brush_Convex( brush_t *b )
{
    unsigned int faceCount = (unsigned int)b->faceCount;
    if ( !faceCount )
        return true;
    for ( unsigned int i = 0; i < faceCount; ++i )
    {
        winding_t *wi = b->faces[i].w;
        if ( !wi )
            continue;
        for ( unsigned int j = 0; j < faceCount; ++j )
        {
            if ( i == j )
                continue;
            winding_t *wj = b->faces[j].w;
            if ( !wj )
                continue;
            if ( Winding_PlanesConcave( wi, wj,
                                        b->faces[j].plane.normal,
                                        b->faces[i].plane.normal,
                                        b->faces[i].plane.dist,
                                        b->faces[j].plane.dist ) )
                return false;
        }
    }
    return true;
}

// ─── Brush_SplitBrushByFace  (0x471960) ─────────────────────
// Clones `in` twice and clips each by `face`: clone 1 keeps the face as-is → *back;
// clone 2 gets the face reversed (planepts[0]<->[1], flipping the plane) → *front.
// A clone survives only if it still has >= 4 faces after dropping empty/coincident
// ones, else it is freed and the out-pointer nulled.
//
// The binary inlines entity.cpp's def-side Entity_LinkBrush (0x484FC0) here — a
// DIFFERENT function from this file's static instance-side Entity_LinkBrush (the
// kiwi_ported.h overload).  clone->owner = the ENTITY, not its def (setting it to
// the def crashed the clipper).
static void Split_LinkCloneToEntity( brush_t *clone, entity_s *ownerEntity )
{
    Entity_LinkBrush( clone, ownerEntity );
}

void Brush_SplitBrushByFace( brush_t *in, face_t *face, brush_t **front, brush_t **back )
{
    // ── back half: clone clipped by face as-is ──────────────────────────────
    brush_t *b = Brush_Clone( in );          // Brush_Clone takes the DEF directly
    iassert( b );   // brush.cpp:2354
    if ( b->parent_layer_string )
        j__free_0( b->parent_layer_string );
    b->parent_layer_string = AllocMaterialString( in->parent_layer_string );
    face_t *nf = Face_Alloc( b, face );
    Byte4PackPixelColor( const_cast<float*>(colorWhite), (GfxColor *)&nf->packedColor );
    Brush_BuildWindings( b, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++b->version;
    Brush_RemoveWindinglessFaces( b );
    Brush_RemoveDuplicateFaces( b );
    if ( (unsigned int)b->faceCount >= 4u )
    {
        Split_LinkCloneToEntity( b, in->owner );
        *back = b;
    }
    else
    {
        Brush_Free_R( b );
        *back = nullptr;
    }

    // ── front half: clone clipped by the reversed face ──────────────────────
    b = Brush_Clone( in );          // reuse `b` (the brush.cpp:2354 "b" assert)
    iassert( b );   // brush.cpp:2354
    if ( b->parent_layer_string )
        j__free_0( b->parent_layer_string );
    b->parent_layer_string = AllocMaterialString( in->parent_layer_string );
    face_t *nf2 = Face_Alloc( b, face );
    // Reverse the new face: swap planepts[0] <-> planepts[1] (flips the plane normal).
    float t0 = nf2->planepts[0][0], t1 = nf2->planepts[0][1], t2 = nf2->planepts[0][2];
    nf2->planepts[0][0] = nf2->planepts[1][0];
    nf2->planepts[0][1] = nf2->planepts[1][1];
    nf2->planepts[0][2] = nf2->planepts[1][2];
    nf2->planepts[1][0] = t0;
    nf2->planepts[1][1] = t1;
    nf2->planepts[1][2] = t2;
    Byte4PackPixelColor( const_cast<float*>(colorWhite), (GfxColor *)&nf2->packedColor );
    Brush_BuildWindings( b, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++b->version;
    Brush_RemoveWindinglessFaces( b );
    Brush_RemoveDuplicateFaces( b );
    if ( (unsigned int)b->faceCount >= 4u )
    {
        Split_LinkCloneToEntity( b, in->owner );
        *front = b;
    }
    else
    {
        Brush_Free_R( b );
        *front = nullptr;
    }
}

// ─── Brush_MakeFaceWinding  (0x471260)  ──────────────────────────────────────
// Build the polygon for face `f` of brush `def` by starting from the face plane's
// base winding (clipped to the brush AABB ±1) and clipping it behind every OTHER
// face plane of the brush. Returns the winding (caller owns it) or NULL if the
// face is a duplicate plane / degenerate ("unused plane").
//
// This is the per-face winding builder SetupVertexSelection uses (via MakeFace) to
// recover the brush's corner vertices. The map-load/edit path's Brush_BuildWindings
// computes the same convex face polygon through CM_BuildBrushWindingForSide; this
// is the polylib path the vertex-selection code uses directly.
//
// The face plane.dist is read as float.  The binary clips in place with the polylib
// ClipWindingEpsilon (0x4d83b0); here the split Winding_Clip keeps the BACK
// (inside-the-brush) half and frees the input each pass.
extern winding_t *Winding_BaseForPlane( vec3_t maxs, vec3_t mins, plane_t *plane );  // winding.cpp 0x4d7bc0
extern int        Winding_Clip( winding_t *in, vec3_t normal, double dist, float epsilon,
                                winding_t **front, winding_t **back );               // winding.cpp 0x462860 (returns side code; ignored here)
extern void       Winding_Free( winding_t *w );                                      // winding.cpp

// Split Winding_Clip + 2 Winding_Free is equivalent to the in-place negated-plane clip
// (identical g_windingAlloc delta); constants 0.1/0.999/0.01.
winding_t *Brush_MakeFaceWinding( face_t *f, brush_t *def )
{
    // Base winding from the face plane, clipped to the brush AABB expanded ±1.
    vec3_t boundsMin = { def->mins[0] - 1.0f, def->mins[1] - 1.0f, def->mins[2] - 1.0f };
    vec3_t boundsMax = { def->maxs[0] + 1.0f, def->maxs[1] + 1.0f, def->maxs[2] + 1.0f };
    plane_t fplane;
    fplane.normal[0] = f->plane.normal[0];
    fplane.normal[1] = f->plane.normal[1];
    fplane.normal[2] = f->plane.normal[2];
    fplane.type      = f->plane.type;
    fplane.dist      = f->plane.dist;
    fplane.unk       = f->plane.unk;

    winding_t *w = Winding_BaseForPlane( boundsMax, boundsMin, &fplane );

    bool samePlaneSeen = false;
    for ( int i = 0; i < def->faceCount; ++i )
    {
        if ( !w )
            break;
        face_t *of = &def->faces[i];
        if ( of == f )
        {
            samePlaneSeen = true;
            continue;
        }
        // coplanar with the face? (dot of normals near 1 and dist near-equal)
        float ndot = f->plane.normal[0] * of->plane.normal[0]
                   + f->plane.normal[1] * of->plane.normal[1]
                   + f->plane.normal[2] * of->plane.normal[2];
        if ( ndot <= 0.99900001f || fabs( (double)f->plane.dist - (double)of->plane.dist ) >= 0.0099999998f )
        {
            // not coplanar — clip the running winding behind this face plane (keep
            // the BACK half = inside the brush), discard the front half.
            vec3_t n = { of->plane.normal[0], of->plane.normal[1], of->plane.normal[2] };
            winding_t *front = nullptr, *back = nullptr;
            Winding_Clip( w, n, of->plane.dist, 0.1f, &front, &back );
            if ( front ) Winding_Free( front );
            Winding_Free( w );
            w = back;
            if ( !w )
                return nullptr;
        }
        else if ( samePlaneSeen )
        {
            // a second face on the SAME plane as `f` — this face is the duplicate.
            Winding_Free( w );
            return nullptr;
        }
    }

    if ( !w )
    {
        Com_PrintMessage( "unused plane\n" );
        return nullptr;
    }
    if ( (unsigned int)w->numpoints < 3 )
    {
        Winding_Free( w );
        Com_PrintMessage( "unused plane\n" );
        return nullptr;
    }
    return w;
}

// ─── Brush_MoveVertex  (0x471C30) ────────────────────────
// Drags the brush vertex at `move_points` by `delta`, grid-snaps it to `end`,
// re-triangulates the faces sharing the vertex (collapse/split as needed), clamps
// the segment start->end against neighbour planes, and accepts the move only if
// the brush stays convex (else reverts and returns 0). Returns 1 if applied.
//
// There is no bSnap arg — the binary always grid-snaps.
//
// plane.dist is read/written as float; the winding template is 0x28 =
// {int numpoints; float p[3][3]}; the binary's movefacepoints[faceCount+64] write is
// the adjacent originalFaceIndex[faceCount-1] = originalFaceIndex[fi];
// Plane_FromPoints / Winding_PlanesConcave arg orders are the binary's (the helpers
// reorder their params — see their notes).
// KIWI: exactEnd (KIWI vertex G) uses `end` as given - no legacy-grid snap, any step size.
static winding_t *MV_NewWindingFromTemplate( face_t *f, const void *tmpl28 )
{
    iassert( f );   // brush.cpp:83 "f" — inlined winding-alloc face null-check
    if ( f->w ) { --g_windingAlloc; free( f->w ); f->w = nullptr; }
    ++g_windingAlloc;
    winding_t *w = (winding_t *)malloc( 0x28u );
    if ( !w )
        Com_PrintMessage( "out of memory: winding_t\n" );
    w->numpoints = 0;
    memcpy( w, tmpl28, 0x28u );
    f->w = w;
    return w;
}

// KIWI: a point within float noise (4 ulps) of one of the brush's plane points IS that point.
// Windings are float clip results, 1-2 ulps off the authored corners 30000 out; a face
// rebuilt through them tilted (1e-3 on a 2-unit end cap) and cut a sliver off its neighbour.
bool Brush_ExactCorner( const brush_t *b, float *p )
{
    double m = fabs( (double)p[0] );
    m = fmax( m, fabs( (double)p[1] ) );
    m = fmax( m, fabs( (double)p[2] ) );
    const double tol = 2.0 * KiwiSnap_TwoUlps( m );
    for ( int fi = 0; b->faces && fi < b->faceCount; ++fi )
        for ( int k = 0; k < 3; ++k )
        {
            const float *q = b->faces[fi].planepts[k];
            if ( fabs( (double)p[0] - q[0] ) <= tol && fabs( (double)p[1] - q[1] ) <= tol
                 && fabs( (double)p[2] - q[2] ) <= tol )
            {
                p[0] = q[0];
                p[1] = q[1];
                p[2] = q[2];
                return true;
            }
        }
    return false;
}

// KIWI: `end` stays on face f's plane (double, from its plane points) to float noise - the
// vertex slides along f, so f keeps its plane.  Noise only (2 ulps of the larger of the
// plane points and `end`): a 0.001 floor counted an end 100+ ulps off the plane near the
// origin as sliding, the corner landed on the projection, and out and back did not return.
static bool MV_Slides( const face_t *f, const float *end )
{
    double u[3], v[3], n[3], m = 0.0;
    for ( int k = 0; k < 3; ++k )
    {
        u[k] = (double)f->planepts[1][k] - f->planepts[0][k];
        v[k] = (double)f->planepts[2][k] - f->planepts[0][k];
        for ( int p = 0; p < 3; ++p )
            m = fmax( m, fabs( (double)f->planepts[p][k] ) );
        m = fmax( m, fabs( (double)end[k] ) );
    }
    n[0] = u[1] * v[2] - u[2] * v[1];
    n[1] = u[2] * v[0] - u[0] * v[2];
    n[2] = u[0] * v[1] - u[1] * v[0];
    const double len = sqrt( n[0] * n[0] + n[1] * n[1] + n[2] * n[2] );
    if ( len == 0.0 )
        return false;
    const double s = ( n[0] * ( end[0] - (double)f->planepts[0][0] ) + n[1] * ( end[1] - (double)f->planepts[0][1] )
                     + n[2] * ( end[2] - (double)f->planepts[0][2] ) ) / len;
    return fabs( s ) <= fmax( 1e-5, KiwiSnap_TwoUlps( m ) );
}

// KIWI: the point t along start -> end, if it is a real step (a crease clamp that rounds back
// onto `start` would merge and re-split the same pair forever).
static bool MV_CreaseStep( const float *start, const float *end, double t, float *at )
{
    if ( !( t > 0.0 && t < 1.0 ) )
        return false;
    for ( int k = 0; k < 3; ++k )
        at[k] = (float)( start[k] + ( (double)end[k] - start[k] ) * t );
    return fabsf( at[0] - start[0] ) + fabsf( at[1] - start[1] ) + fabsf( at[2] - start[2] ) > 1e-4f;
}

int Brush_MoveVertex( vec3_t delta, brush_t *b, vec3_t move_points, vec3_t end, bool exactEnd )
{
    struct WTmpl { int numpoints; float p[3][3]; };   // 0x28 winding template
    WTmpl tmpl;
    tmpl.numpoints = 3;

    float start[3];
    int   result = 1;
    // KIWI: were [64]/[66]/[64] stack arrays; splitting a 32-gon cap overran them (crash).
    std::vector<int> movefaces;
    std::vector<int> movefacepoints;   // movefacepoints (paired: movefaces[k] <-> movefacepoints[k+1])
    std::vector<int> originalFaceIndex;
    // A pass adds at most (points - 2) faces per face it splits, and Phase 2a's loop visits
    // those new triangles too: size for the worst case.
    auto reserveFaces = [&]()
    {
        size_t points = 0;
        for ( int fi = 0; fi < b->faceCount; ++fi )
            if ( winding_t *w = b->faces[fi].w )
                points += (size_t)w->numpoints;
        const size_t cap = (size_t)b->faceCount + 2 * points + 2;
        if ( movefaces.size() < cap )
        {
            movefaces.resize( cap );
            movefacepoints.resize( cap + 2 );
            originalFaceIndex.resize( cap + 1 );
        }
    };
    unsigned int nummovefaces;
    int   settled;
    int   slidefaces[64], slidepoints[64];   // KIWI: faces the vertex slides along (MV_Slides)
    unsigned int numslide;
    int   flipA = -1, flipB = -1;        // KIWI: the move-face pair whose crease the clamp flips
    bool  creaseOff = false;             // KIWI: no more crease clamps (a merge failed / 16 passes)
    int   passes = 0;

    start[0] = move_points[0];
    start[1] = move_points[1];
    start[2] = move_points[2];

    end[0] = move_points[0] + delta[0];
    end[1] = delta[1] + move_points[1];
    end[2] = delta[2] + move_points[2];

    // KIWI: KIWI vertex G (kiwi_transform.cpp) has already snapped through its own grid;
    // this legacy-grid (16) snap dragged every typed vertex move onto a 16 lattice.
    if ( !exactEnd )
    for ( unsigned int axis = 0; axis < 3; ++axis )   // grid-snap end
    {
        int gs = g_qeglobals.d_gridsize;
        double snapped = floor( end[axis] / grid_sizes[gs] + 0.5 );
        end[axis] = (float)( snapped * grid_sizes[gs] );
    }

    // KIWI: the exact (KIWI-driven) path moves by any amount.  A 0.3 floor refused every
    // small step - typing "1.25" applies 1, 1.2, 1.25 as steps, and 0.2 / 0.05 were dropped.
    if ( Point_Equal( start, end, exactEnd ? 0.001f : 0.30000001f ) )
        return 0;

    // Phase 1: originalFaceIndex[i]=i; abort if `end` lands on an existing vertex.
    reserveFaces();
    if ( b->faceCount )
    {
        for ( unsigned int fi = 0; fi < (unsigned int)b->faceCount; ++fi )
        {
            originalFaceIndex[fi] = fi;
            winding_t *w = b->faces[fi].w;
            if ( w )
            {
                for ( unsigned int k = 0; k < (unsigned int)w->numpoints; ++k )
                {
                    // KIWI: on the exact path the moving vertex itself is not "an existing
                    // vertex" - a step under 0.3 landed within 0.3 of its own start and aborted.
                    if ( exactEnd && Point_Equal( w->p[k], start, 0.2f ) )
                        continue;
                    if ( Point_Equal( w->p[k], end, 0.30000001f ) )
                    {
                        end[0] = move_points[0];
                        end[1] = move_points[1];
                        end[2] = move_points[2];
                        return 0;
                    }
                }
            }
        }
    }

    // KIWI: work on the corners as authored (Brush_ExactCorner), so the faces rebuilt below
    // meet their unchanged neighbours exactly.
    Brush_ExactCorner( b, start );
    for ( int fi = 0; fi < b->faceCount; ++fi )
        if ( winding_t *w = b->faces[fi].w )
            for ( int k = 0; k < w->numpoints; ++k )
                Brush_ExactCorner( b, w->p[k] );
    // KIWI: and without the points the winding builder lets through up to 0.1 outside another
    // face (CM_AddSimpleBrushPoint, as cod4map draws them): on a shallow crease a fan triangle
    // borrowed its neighbour's far corner, looked 4-sided, and was collapsed instead of tilted
    // - its plane stranded at the old height.  A face keeps them if it would drop under 3.
    {
        std::vector<double> pl( (size_t)b->faceCount * 4 );
        for ( int fi = 0; fi < b->faceCount; ++fi )
            edwind::CM_FacePlaneDouble( &b->faces[fi], &pl[(size_t)fi * 4] );
        for ( int fi = 0; fi < b->faceCount; ++fi )
        {
            winding_t *w = b->faces[fi].w;
            if ( !w || w->numpoints <= 3 )
                continue;
            std::vector<int> keep;
            for ( int k = 0; k < w->numpoints; ++k )
            {
                const float *p = w->p[k];
                const double tol = fmax( 0.002, 2.0 * KiwiSnap_TwoUlps( fmax( fabs( (double)p[0] ), fmax( fabs( (double)p[1] ), fabs( (double)p[2] ) ) ) ) );
                bool inside = true;
                for ( int o = 0; o < b->faceCount && inside; ++o )
                    if ( o != fi )
                        inside = pl[o * 4] * p[0] + pl[o * 4 + 1] * p[1] + pl[o * 4 + 2] * p[2] - pl[o * 4 + 3] <= tol;
                if ( inside )
                    keep.push_back( k );
            }
            if ( keep.size() < 3 || (int)keep.size() == w->numpoints )
                continue;
            for ( size_t n = 0; n < keep.size(); ++n )
                memmove( w->p[n], w->p[keep[n]], sizeof( w->p[0] ) );
            w->numpoints = (int)keep.size();
        }
    }

    do  // main loop until `settled`
    {
        nummovefaces = 0;
        numslide     = 0;
        reserveFaces();
        if ( ++passes > 16 )
            creaseOff = true;   // KIWI: the binary's clamps alone from here (they settle)

        // Phase 2a: find/build the faces that contain `start`, splitting/collapsing as needed.
        for ( unsigned int fi = 0; fi < (unsigned int)b->faceCount; ++fi )
        {
            face_t   *fp = &b->faces[fi];
            winding_t *w = fp->w;
            if ( !w || !w->numpoints )
                continue;

            unsigned int np  = (unsigned int)w->numpoints;
            unsigned int pti = 0;
            for ( ; pti < np; ++pti )
                if ( Point_Equal( w->p[pti], start, 0.2f ) )
                    break;
            if ( pti >= np )
                continue;   // this face doesn't touch `start`

            // KIWI: a face the vertex slides along keeps its plane; Phase 2c only moves the
            // point in its winding.  Collapsed into a triangle and rebuilt, it tilted by the
            // float noise, then neither merged back nor clamped sanely 30000 out.
            if ( numslide < 64 && MV_Slides( fp, end ) )
            {
                slidefaces[numslide]  = fi;
                slidepoints[numslide] = pti;
                ++numslide;
                continue;
            }

            if ( np > 3 )
            {
                float ndote = fp->plane.normal[1] * end[1]
                            + end[0] * fp->plane.normal[0]
                            + fp->plane.normal[2] * end[2];
                float behind = ndote - fp->plane.dist;   // float dist
                if ( behind <= 0.1f )
                {
                    // COLLAPSE: drop the vertex from this winding, splice in a triangle face.
                    tmpl.p[0][0] = w->p[(np + pti - 1) % np][0];
                    tmpl.p[0][1] = w->p[(np + pti - 1) % np][1];
                    tmpl.p[0][2] = w->p[(np + pti - 1) % np][2];
                    tmpl.p[1][0] = w->p[pti][0];
                    tmpl.p[1][1] = w->p[pti][1];
                    tmpl.p[1][2] = w->p[pti][2];
                    tmpl.p[2][0] = w->p[(pti + 1) % np][0];
                    tmpl.p[2][1] = w->p[(pti + 1) % np][1];
                    tmpl.p[2][2] = w->p[(pti + 1) % np][2];
                    bcassert( pti, np );   // inlined Winding.cpp:72 RemovePoint check
                    unsigned int newCount = (unsigned int)--w->numpoints;
                    if ( pti < newCount )
                        memcpy( w->p[pti], w->p[pti + 1], 12 * ( newCount - pti ) );
                    face_t *newface = Face_Alloc( b, fp );
                    iassert( newface == &b->faces[b->faceCount - 1] );   // brush.cpp:1146
                    originalFaceIndex[b->faceCount - 1] = originalFaceIndex[fi];
                    MV_NewWindingFromTemplate( newface, &tmpl );
                    iassert( newface == &b->faces[b->faceCount - 1] );   // brush.cpp:1152
                    movefaces[nummovefaces] = b->faceCount - 1;
                    movefacepoints[nummovefaces + 1] = 1;   // dragged vertex is tmpl.p[1]
                    ++nummovefaces;
                    // movefacepoints is offset by one (the binary's [n - 1] is [n] here).
                    iassert( Point_Equal( start,
                             b->faces[movefaces[nummovefaces - 1]].w->p[movefacepoints[nummovefaces]], 0.2f ) );   // :1189
                }
                else
                {
                    // SPLIT: fan out one triangle face per spanned edge, then re-triangulate
                    // the original face. `pti` is the dragged vertex; np unchanged here.
                    if ( pti < np + pti - 3 )
                    {
                        for ( unsigned int e = pti + 2; e - 2 < (unsigned int)w->numpoints + pti - 3; ++e )
                        {
                            unsigned int n2 = (unsigned int)w->numpoints;
                            tmpl.p[0][0] = w->p[pti][0];
                            tmpl.p[0][1] = w->p[pti][1];
                            tmpl.p[0][2] = w->p[pti][2];
                            tmpl.p[1][0] = w->p[(e - 1) % n2][0];
                            tmpl.p[1][1] = w->p[(e - 1) % n2][1];
                            tmpl.p[1][2] = w->p[(e - 1) % n2][2];
                            tmpl.p[2][0] = w->p[e % n2][0];
                            tmpl.p[2][1] = w->p[e % n2][1];
                            tmpl.p[2][2] = w->p[e % n2][2];
                            face_t *newface = Face_Alloc( b, fp );
                            fp = &b->faces[fi];   // re-fetch (Face_Alloc may realloc)
                            iassert( newface == &b->faces[b->faceCount - 1] );   // brush.cpp:1180
                            originalFaceIndex[b->faceCount - 1] = originalFaceIndex[fi];
                            MV_NewWindingFromTemplate( newface, &tmpl );
                            iassert( newface == &b->faces[b->faceCount - 1] );   // brush.cpp:1186
                            movefaces[nummovefaces] = b->faceCount - 1;
                            movefacepoints[nummovefaces + 1] = 0;   // dragged vertex is tmpl.p[0]
                            ++nummovefaces;
                            iassert( Point_Equal( start,
                                     b->faces[movefaces[nummovefaces - 1]].w->p[movefacepoints[nummovefaces]], 0.2f ) );   // :1155
                        }
                    }
                    // re-triangulate the original face (its winding becomes the last triangle)
                    unsigned int n3 = (unsigned int)w->numpoints;
                    tmpl.p[0][0] = w->p[(n3 + pti - 2) % n3][0];
                    tmpl.p[0][1] = w->p[(n3 + pti - 2) % n3][1];
                    tmpl.p[0][2] = w->p[(n3 + pti - 2) % n3][2];
                    tmpl.p[1][0] = w->p[(n3 + pti - 1) % n3][0];
                    tmpl.p[1][1] = w->p[(n3 + pti - 1) % n3][1];
                    tmpl.p[1][2] = w->p[(n3 + pti - 1) % n3][2];
                    tmpl.p[2][0] = w->p[pti][0];
                    tmpl.p[2][1] = w->p[pti][1];
                    tmpl.p[2][2] = w->p[pti][2];
                    MV_NewWindingFromTemplate( fp, &tmpl );
                    movefaces[nummovefaces] = fi;
                    movefacepoints[nummovefaces + 1] = 2;   // dragged vertex is tmpl.p[2]
                    ++nummovefaces;
                    iassert( Point_Equal( start,
                             b->faces[movefaces[nummovefaces - 1]].w->p[movefacepoints[nummovefaces]], 0.2f ) );   // :1166
                }
            }
            else
            {
                // numpoints <= 3: record this face as a move-face directly.
                movefacepoints[nummovefaces + 1] = pti;
                movefaces[nummovefaces] = fi;
                ++nummovefaces;
            }
        }

        // Phase 2b: clamp start->end against every non-move face plane; track the
        // smallest crossing fraction into (clampX,clampY,clampZ).
        float clampX = end[0], clampY = end[1], clampZ = end[2];
        float smallestFrac = 1.0f;
        settled = 1;
        for ( unsigned int f = 0; f < (unsigned int)b->faceCount; ++f )
        {
            if ( !nummovefaces )
                continue;
            // find the move-face slot whose originalFaceIndex maps to f
            unsigned int slot = 0;
            while ( (unsigned int)originalFaceIndex[movefaces[slot]] != f )
                if ( ++slot >= nummovefaces )
                    goto next_clamp_face;

            {
                char planeBuf[32];                       // plane storage (double[4])
                plane_t *pl = (plane_t *)planeBuf;
                // find a move-face whose movefaces[j]==f (a direct neighbour edge)
                unsigned int j = 0;
                while ( f != (unsigned int)movefaces[j] )
                {
                    if ( ++j >= nummovefaces )
                    {
                        // no direct neighbour: use the original face plane verbatim
                        memcpy( planeBuf, &b->faces[originalFaceIndex[f]].plane, sizeof(planeBuf) );
                        goto do_clamp;
                    }
                }
                {
                    int pj      = movefacepoints[j + 1];
                    winding_t *wj = b->faces[movefaces[j]].w;
                    unsigned int nj = (unsigned int)wj->numpoints;
                    float *a = wj->p[(unsigned int)(pj + 1) % nj];
                    tmpl.p[0][0] = a[0]; tmpl.p[0][1] = a[1]; tmpl.p[0][2] = a[2];
                    tmpl.p[1][0] = wj->p[(pj + 2) % nj][0];
                    tmpl.p[1][1] = wj->p[(pj + 2) % nj][1];
                    tmpl.p[1][2] = wj->p[(pj + 2) % nj][2];
                    int ps      = movefacepoints[slot + 1];
                    winding_t *ws = b->faces[movefaces[slot]].w;
                    float *c = ws->p[(unsigned int)(ps + 1) % (unsigned int)ws->numpoints];
                    tmpl.p[2][0] = c[0]; tmpl.p[2][1] = c[1]; tmpl.p[2][2] = c[2];
                    // Plane_FromPoints(p1,p2,p3,plane) gets the points as (tmpl.p[2],tmpl.p[1],tmpl.p[0])
                    if ( !Plane_FromPoints( tmpl.p[2], tmpl.p[1], tmpl.p[0], pl ) )
                    {
                        unsigned int alt = (unsigned int)(ps + 2) % (unsigned int)ws->numpoints;
                        tmpl.p[2][0] = ws->p[alt][0];
                        tmpl.p[2][1] = ws->p[alt][1];
                        tmpl.p[2][2] = ws->p[alt][2];
                        if ( !Plane_FromPoints( tmpl.p[2], tmpl.p[1], tmpl.p[0], pl ) )
                            goto next_clamp_face;
                    }
                }
do_clamp:
                {
                    float startDist = pl->normal[0] * start[0] + pl->normal[1] * start[1]
                                    + pl->normal[2] * start[2] - pl->dist;
                    float endDist   = pl->normal[0] * end[0] + pl->normal[1] * end[1]
                                    + pl->normal[2] * end[2] - pl->dist;
                    if ( startDist < 0.0099999998f && endDist < 0.0099999998f )
                        goto next_clamp_face;   // both behind → no clamp from this plane
                    if ( startDist <= -0.0099999998f || endDist <= -0.0099999998f )
                    {
                        float denom = startDist - endDist;
                        if ( fabs( denom ) >= 0.001f )
                        {
                            float frac = startDist / denom;
                            settled = 0;
                            if ( smallestFrac > (double)frac )
                            {
                                clampX = ( end[0] - start[0] ) * frac + start[0];
                                clampY = start[1] + ( end[1] - start[1] ) * frac;
                                clampZ = start[2] + ( end[2] - start[2] ) * frac;
                                smallestFrac = startDist / denom;
                            }
                        }
                    }
                }
            }
next_clamp_face:;
        }

        // KIWI: the crease between two move-faces flips when the vertex passes the plane of
        // their three other corners - tetra( P, Q, R, V(t) ) changes sign, linear in t.  Clamp
        // there; once accepted, the two become one face, and the next pass collapses or
        // splits it on the diagonal that stays convex.  The clamps above only see faces split
        // in this call, so an already-split roof went straight through (Brush_Convex allows
        // 0.2) and the rebuild cut the neighbouring corners down to the vertex.
        flipA = flipB = -1;
        for ( unsigned int ia = 0; ia < nummovefaces && !creaseOff; ++ia )
        {
            winding_t *wa = b->faces[movefaces[ia]].w;
            const unsigned int na = (unsigned int)wa->numpoints, pa = (unsigned int)movefacepoints[ia + 1];
            for ( unsigned int ib = 0; ib < nummovefaces; ++ib )
            {
                winding_t *wb = b->faces[movefaces[ib]].w;
                const unsigned int nb = (unsigned int)wb->numpoints, pb = (unsigned int)movefacepoints[ib + 1];
                if ( ib == ia || !Point_Equal( wa->p[( pa + 1 ) % na], wb->p[( pb + nb - 1 ) % nb], 0.01f ) )
                    continue;           // not the pair sharing the edge V -> Q
                const float *P = wa->p[( pa + na - 1 ) % na], *Q = wa->p[( pa + 1 ) % na], *R = wb->p[( pb + 1 ) % nb];
                double pq[3], rq[3], s[3], e[3];
                for ( int c = 0; c < 3; ++c )
                {
                    pq[c] = (double)P[c] - Q[c];
                    rq[c] = (double)R[c] - Q[c];
                    s[c]  = (double)start[c] - Q[c];
                    e[c]  = (double)end[c] - Q[c];
                }
                const double x[3] = { pq[1] * rq[2] - pq[2] * rq[1], pq[2] * rq[0] - pq[0] * rq[2], pq[0] * rq[1] - pq[1] * rq[0] };
                const double vol0 = x[0] * s[0] + x[1] * s[1] + x[2] * s[2];
                const double vol1 = x[0] * e[0] + x[1] * e[1] + x[2] * e[2];
                const double tiny = 1e-9 * sqrt( x[0] * x[0] + x[1] * x[1] + x[2] * x[2] ) * sqrt( s[0] * s[0] + s[1] * s[1] + s[2] * s[2] );
                if ( !( vol0 * vol1 < 0.0 ) || fabs( vol0 ) <= tiny )
                    continue;           // no flip on this move (or the pair is coplanar already)
                const double t = vol0 / ( vol0 - vol1 );
                float at[3];
                if ( t < smallestFrac && MV_CreaseStep( start, end, t, at ) )
                {
                    smallestFrac = (float)t;
                    clampX  = at[0];
                    clampY  = at[1];
                    clampZ  = at[2];
                    flipA   = movefaces[ia];
                    flipB   = movefaces[ib];
                    settled = 0;
                }
            }
        }
        // ... and the crease on a move-face triangle's far edge, against the face across it
        // (a valley vertex rising through it): V crosses that face's plane.  The face keeps
        // its plane points; the triangle merges into it.
        for ( unsigned int ia = 0; ia < nummovefaces && !creaseOff; ++ia )
        {
            winding_t *wt = b->faces[movefaces[ia]].w;
            if ( wt->numpoints != 3 )
                continue;
            const unsigned int pt = (unsigned int)movefacepoints[ia + 1];
            float *n1 = wt->p[( pt + 1 ) % 3], *n2 = wt->p[( pt + 2 ) % 3];
            for ( int f = 0; f < b->faceCount; ++f )
            {
                winding_t *wf = b->faces[f].w;
                bool isMove = false;
                for ( unsigned int m = 0; m < nummovefaces && !isMove; ++m )
                    isMove = movefaces[m] == f;
                if ( isMove || !wf )
                    continue;
                bool across = false;
                for ( int k = 0; k < wf->numpoints && !across; ++k )
                    across = Point_Equal( wf->p[k], n2, 0.01f ) && Point_Equal( wf->p[( k + 1 ) % wf->numpoints], n1, 0.01f );
                if ( !across )
                    continue;
                double pl[4];
                edwind::CM_FacePlaneDouble( &b->faces[f], pl );
                const double s0 = pl[0] * start[0] + pl[1] * start[1] + pl[2] * start[2] - pl[3];
                const double s1 = pl[0] * end[0] + pl[1] * end[1] + pl[2] * end[2] - pl[3];
                if ( !( s0 < -1e-6 && s1 > 0.0 ) )
                    continue;           // the vertex stays behind it
                const double t = s0 / ( s0 - s1 );
                float at[3];
                if ( t < smallestFrac && MV_CreaseStep( start, end, t, at ) )
                {
                    smallestFrac = (float)t;
                    clampX  = at[0];
                    clampY  = at[1];
                    clampZ  = at[2];
                    flipA   = f;
                    flipB   = movefaces[ia];
                    settled = 0;
                }
            }
        }

        // Phase 2c: write the clamped position into each move-face's dragged vertex,
        // rebuild planepts + plane; flag degenerate normals.
        for ( unsigned int i = 0; i < nummovefaces; ++i )
        {
            face_t   *fp = &b->faces[movefaces[i]];
            winding_t *w = fp->w;
            float    *vp = w->p[movefacepoints[i + 1]];
            iassert( Point_Equal( vp, start, 0.2f ) );   // :1267
            vp[0] = clampX;
            vp[1] = clampY;
            vp[2] = clampZ;
            fp->planepts[0][0] = w->p[0][0]; fp->planepts[0][1] = w->p[0][1]; fp->planepts[0][2] = w->p[0][2];
            fp->planepts[1][0] = w->p[1][0]; fp->planepts[1][1] = w->p[1][1]; fp->planepts[1][2] = w->p[1][2];
            fp->planepts[2][0] = w->p[2][0]; fp->planepts[2][1] = w->p[2][1]; fp->planepts[2][2] = w->p[2][2];
            Face_MakePlane( fp );
            float nlen = (float)sqrt( fp->plane.normal[0] * fp->plane.normal[0]
                                    + fp->plane.normal[1] * fp->plane.normal[1]
                                    + fp->plane.normal[2] * fp->plane.normal[2] );
            if ( nlen < 0.1f )
                result = 0;
        }
        for ( unsigned int i = 0; i < numslide; ++i )   // KIWI: plane kept, point moved
        {
            float *vp = b->faces[slidefaces[i]].w->p[slidepoints[i]];
            vp[0] = clampX;
            vp[1] = clampY;
            vp[2] = clampZ;
        }

        // Phase 2d: accept if still convex, else revert every move-face to `start`.
        if ( result && Brush_Convex( b ) )
        {
            start[0] = clampX;
            start[1] = clampY;
            start[2] = clampZ;
        }
        else
        {
            for ( unsigned int j = 0; j < nummovefaces; ++j )
            {
                face_t   *fp = &b->faces[movefaces[j]];
                winding_t *w = fp->w;
                float    *vp = w->p[movefacepoints[j + 1]];
                vp[0] = start[0];
                vp[1] = start[1];
                vp[2] = start[2];
                fp->planepts[0][0] = w->p[0][0]; fp->planepts[0][1] = w->p[0][1]; fp->planepts[0][2] = w->p[0][2];
                fp->planepts[1][0] = w->p[1][0]; fp->planepts[1][1] = w->p[1][1]; fp->planepts[1][2] = w->p[1][2];
                fp->planepts[2][0] = w->p[2][0]; fp->planepts[2][1] = w->p[2][1]; fp->planepts[2][2] = w->p[2][2];
                Face_MakePlane( fp );
            }
            for ( unsigned int j = 0; j < numslide; ++j )   // KIWI
            {
                float *vp = b->faces[slidefaces[j]].w->p[slidepoints[j]];
                vp[0] = start[0];
                vp[1] = start[1];
                vp[2] = start[2];
            }
            end[0] = start[0];
            end[1] = start[1];
            end[2] = start[2];
            result  = 0;
            settled = 1;
        }

        // KIWI: the crease clamp left the pair flat - make it one face (the quad P V R Q).
        if ( result && flipA >= 0 )
        {
            face_t    *fa     = &b->faces[flipA];
            winding_t *merged = Winding_TryMerge( fa->w, b->faces[flipB].w, fa->plane.normal, 0 );
            if ( merged )
            {
                --g_windingAlloc;
                free( fa->w );
                fa->w = merged;
                Brush_RemoveFace( b, (unsigned int)flipB );
                const unsigned int fc = (unsigned int)b->faceCount;
                for ( unsigned int s = (unsigned int)flipB; s < fc; ++s )
                {
                    const int nv = originalFaceIndex[s + 1];
                    originalFaceIndex[s] = ( nv >= flipB ) ? nv - 1 : nv;
                }
            }
            else
                creaseOff = true;   // left as two faces; never clamp at this point again
        }

        // Phase 2e: merge coplanar adjacent faces; compact originalFaceIndex.
        for ( unsigned int faceIndex = 0; faceIndex < (unsigned int)b->faceCount; ++faceIndex )
            iassert( originalFaceIndex[faceIndex] <= faceIndex );

        {
            unsigned int i = 0, k = 0;
            while ( i < (unsigned int)b->faceCount )
            {
                unsigned int orig = (unsigned int)originalFaceIndex[i];
                if ( orig == i )
                {
                    ++i;
                    ++k;
                    continue;
                }
                face_t *fa = &b->faces[orig];
                if ( Plane_Equal( &fa->plane, &b->faces[k].plane, 0 ) )
                {
                    winding_t *merged = Winding_TryMerge( b->faces[k].w, fa->w,
                                                          fa->plane.normal, 0 );
                    if ( merged )
                    {
                        if ( fa->w ) { --g_windingAlloc; free( fa->w ); fa->w = nullptr; }
                        fa->w = merged;
                        Brush_RemoveFace( b, i );
                        if ( i >= (unsigned int)b->faceCount )
                            break;
                        unsigned int fc = (unsigned int)b->faceCount;
                        for ( unsigned int s = i; s < fc; ++s )
                        {
                            unsigned int nv = (unsigned int)originalFaceIndex[s + 1];
                            originalFaceIndex[s] = nv;
                            if ( nv >= i )
                                originalFaceIndex[s] = nv - 1;
                        }
                    }
                    else
                    {
                        ++i;
                        ++k;
                    }
                }
                else
                {
                    ++i;
                    ++k;
                }
            }
        }
    }
    while ( !settled );

    // KIWI: a refused move still had its windings snapped and filtered (the pre-pass above);
    // draw it again as cod4map will.
    if ( !result )
        Brush_BuildWindings( b, 0 );
    ++b->version;
    return result;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Model_SetModel  (0x478780, brush.cpp:3713) — PREFAB-LOAD-ON-MAP-OPEN.
//
//  Called by the GUI map load for every misc_prefab entity (Map_LoadFromFile ->
//  Map_LoadEntities post-process loop, gated on eclass->classtype & 0x10 =
//  CLASS_PREFAB).  Resolves the entity's "model" key into a loaded prefab .map and
//  INSTANCES that prefab's brushes into the live editor scene.
//
//  CONTROL FLOW (brush_t.modelFailed is the BYTE overlaying the low byte of
//  brush_t.unk01, so it is accessed through a byte pointer):
//    1. Copy the caller's 12-float orientation matrix into a local (so the cyclePreview
//       ground-drop branch can perturb it without touching the caller's matrix).
//    2. VERSION-and-already-built gate: instance version == def version AND the instance
//       already has a modelInst or prefab -> return (asserting modelFailed is clear).
//    3. Otherwise rebuild: bail if modelFailed is already set; set modelFailed=1;
//       Entity_FreePrefab + RemoveModelInstFromBuf to tear down any stale instance.
//    4. Resolve modelClass: entityDef->modelClass if set; else Eclass_hasModel(eclass)
//       lazily loads an eclass-level model; else resolve the model NAME (the misc_prefab
//       "model" epair via ValueForKey2) and load it via Eclass_ForModel (-> Eclass_LoadModel
//       -> Prefab_Load -> Map_LoadEntities).
//    5. On success: Entity_RebuildBounds(entityDef) + SetupModelInst(matrix, instance).
//       SetupModelInst routes classtype&0x10 prefabs to Entity_InitPrefabInst and
//       everything else to Entity_UpdateModelInst; clears modelFailed if an instance now
//       exists.
//
//  The cyclePreview ground-drop branch (instance brushFlags & 0x100 ->
//  Trace_AllDirectionsIfFailed) only runs under the model-cycling UI toggle
//  (w_cyclePreviewMode), never during a map load.
// ─────────────────────────────────────────────────────────────────────────────
extern "C++" {
    extern bool  Eclass_hasModel( eclass_t *e );                  // eclass.cpp 0x481740
    extern models_t *Eclass_ForModel( const char *name, entity_s *ent ); // eclass.cpp 0x482300
    extern void  Entity_FreePrefab( entity_s *e );                // entity.cpp 0x4825F0
    extern void  RemoveModelInstFromBuf( int inst );              // engine_stubs 0x4FDCE0
    extern void  SetupModelInst( float *ident_mtx, entity_s *e ); // entity.cpp 0x485890
}

// modelFailed is a byte (brush_t.unk01's low byte); Model_SetModel sets, clears and tests it
// at byte width.
static inline byte *Brush_ModelFailedByte( brush_t *def )
{
    return &def->modelFailed;
}

// VecMultiplyAdd (0x40A5A0) — vec3 multiply-add: out = a + scale*b.  (VectorMA is a
// deliberate compile-error in this tree, so the binary's helper is defined here for its sole
// radiant caller, the Model_SetModel cyclePreview ground-drop.)
static void VecMultiplyAdd( const float *a, float scale, const float *b, float *out )
{
    out[0] = a[0] + scale * b[0];
    out[1] = a[1] + scale * b[1];
    out[2] = a[2] + scale * b[2];
}

// drop-to-floor ray (select.cpp 0x48DAA0; edTrace_t is defined in qe3.h).
extern edTrace_t *Trace_AllDirectionsIfFailed( float *cam_origin, edTrace_t *trace_result,
                                               float *dir, int contents );

bool Model_SetModel( entity_brush_s *b, const float *orientMatrix )
{
    // 1. Local copy of the caller's orientation matrix (3x4 = 12 floats).
    float mtx[12];
    for ( int i = 0; i < 12; ++i )
        mtx[i] = orientMatrix[i];

    entity_s *inst = b->owner;   // entity instance

    // 2. Version-and-already-built gate.  inst->version (short) vs the def's 16-bit
    //    model version; plus modelInst / prefab.
    {
        entity_s_def *instDef = (entity_s_def *)inst->def;
        if ( (short)inst->version == (short)instDef->version_prob_wrong
             && ( inst->modelInst != 0 || inst->prefab != nullptr ) )
        {
            // Up to date.
            iassert( !b->def->modelFailed );   // brush.cpp:3723
            return true;
        }
    }

    // 3. Needs rebuild.  Bail if a previous attempt already failed.
    if ( *Brush_ModelFailedByte( b->def ) )
        return false;
    *Brush_ModelFailedByte( b->def ) = 1;

    Entity_FreePrefab( inst );
    if ( inst->modelInst )
    {
        RemoveModelInstFromBuf( inst->modelInst );
        inst->modelInst = 0;
    }

    // ownerDef = b->def->owner (the def's owning entity DEF).
    iassert( b->owner->def == b->def->owner );
    entity_s_def *ownerDef = (entity_s_def *)b->def->owner;

    // 4. Resolve the model eclass node `e`:
    //    e = ownerDef->modelClass; if (e) goto instance;
    //    e = eclass;   // ← eclass IS the modelClass node
    //    if (!Eclass_hasModel(eclass)) { ... resolve "model" key -> e = Eclass_ForModel(...) }
    // CRITICAL: when Eclass_hasModel(eclass) is TRUE, e is the ECLASS itself — leaving e
    // NULL means a misc_model whose eclass resolved its model never instances.
    entitymodel_t *e = (entitymodel_t *) (models_t *)ownerDef->modelClass;
    const bool resolving = ( e == nullptr );   // KIWI: first resolve -> prefab proxy boxes refresh
    if ( !e )
    {
        eclass_t *eclass = ownerDef->eclass;
        bool eclassHasModel = Eclass_hasModel( eclass );
        e = (entitymodel_t *)eclass;                          // modelClass = ownerDef->eclass
        if ( !eclassHasModel )
        {
            // eclass has no built-in model node → resolve a per-entity model name.
            // The dword at &fixedsize (bool + 3 pad bytes) gates non-modelled
            // classes out (misc_prefab is fixedsize → non-zero → proceeds).
            if ( !*(int *)&eclass->fixedsize )
                return *Brush_ModelFailedByte( b->def ) == 0;

            const char *modelName;
            if ( ( b->brushFlags & 0x100 ) != 0 )
            {
                // ── XMODEL cyclePreview ground-drop.  The
                //    model-cycle UI (w_cyclePreviewMode) sets brushFlags&0x100: pick the cycle
                //    model name from the eclass's default_model_name[] char* array, trace
                //    straight down from the entity origin, and if the floor is hit, drop the
                //    orientation matrix's origin onto it (VecMultiplyAdd by the hit distance).
                modelName = eclass->cycleModelName[(ushort)g_qeglobals.w_cyclePreviewMode];
                float dropDir[3] = { 0.0f, 0.0f, -1.0f };
                edTrace_t tr;
                Trace_AllDirectionsIfFailed( ownerDef->origin, &tr, dropDir, 4608 );
                if ( tr.hit.brush )
                    VecMultiplyAdd( orientMatrix, tr.dist, dropDir, mtx );   // mtx.origin += dist*dir (to floor)
            }
            else
            {
                modelName = ValueForKey2( ownerDef, "model" );
            }

            // brush.cpp:3763 — LEVEL 1 (binary asserts modelName != NULL post-resolve).
            iassert( modelName );   // brush.cpp:3763
            // KIWI: NULL is "no model" too - the binary dereferenced it in release
            if ( !modelName || !*modelName )
                return *Brush_ModelFailedByte( b->def ) == 0;

            e = Eclass_ForModel( modelName, ownerDef );
        }
    }

    // 5. Instance the resolved model.
    if ( e )
    {
        iassert( ownerDef->modelClass == e );
        if ( resolving )
        {
            extern unsigned g_modelResolveSerial;   // entity.cpp: Prefab_RefreshProxyBounds
            ++g_modelResolveSerial;
        }

        Entity_RebuildBounds( ownerDef );
        SetupModelInst( mtx, b->owner );

        // If an instance now exists, clear modelFailed.
        if ( inst->modelInst != 0 || inst->prefab != nullptr )
            *Brush_ModelFailedByte( b->def ) = 0;

        // One-shot load trace (fires once per misc_prefab — the version gate at the
        // top short-circuits every subsequent call).  Counts the brush INSTANCES
        // linked into this prefab's active list, isolating prefab-loaded geometry
        // from any inline world brushes.  Consistent with the First-Light load trace.
        if ( inst->prefab )
        {
            prefab_s *pf = (prefab_s *)inst->prefab;
            int nBrush = 0;
            for ( selbrush_t *pb = pf->active_brushlist_next;
                  pb && pb != (selbrush_t *)&pf->active_brushlist;
                  pb = pb->next )
                ++nBrush;
            Radiant_FL_Log( "Model_SetModel: prefab '%s' instanced %d brush(es)",
                            ValueForKey2( ownerDef, "model" ), nBrush );
        }
    }

    return *Brush_ModelFailedByte( b->def ) == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Brush_IsInSelectedList (0x478a20) — is brush instance `b` in the selected_brushes
// display list?  A plain circular-list membership walk.
// ─────────────────────────────────────────────────────────────────────────────
static char Brush_IsInSelectedList( selbrush_t *b )
{
    for ( selbrush_t *v = selected_brushes.next; v != &selected_brushes; v = v->next )
        if ( v == b )
            return 1;
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Entity_HasRenderableModel (0x479610) — the model-presence gate DrawBrush + the
// sun-preview model shadow (SunLightPreview_DrawBrushShadow) use before instancing a
// model.  For a non-prefab (classtype & 0x10 == 0) entity, honours the View->Entities-As
// show-state (m_nEntityShowState 4096 = hide-all, 0x100 = selected-only via
// Brush_IsInSelectedList); then Model_SetModel and returns 1 iff a model/prefab instanced
// (else asserts unless modelFailed).
// KIWI: takes the brush INSTANCE (the IDB typed it as the def; every caller passes an instance).
// ─────────────────────────────────────────────────────────────────────────────
char Entity_HasRenderableModel( selbrush_t *b, const float *orient )
{
    entity_s_def *eDef = (entity_s_def *)b->owner->def;
    if ( ( eDef->eclass->classtype & 0x10 ) == 0 )         // not a prefab class
    {
        int show = g_PrefsDlg->m_nEntityShowState;
        if ( show == 4096 || ( ( show & 0x100 ) != 0 && !Brush_IsInSelectedList( b ) ) )
            return 0;                                      // hidden by the show-state filter
    }
    if ( !Model_SetModel( b, orient ) )
    {
        iassert( b->def->modelFailed );   // brush.cpp:4037
        return 0;
    }
    return 1;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x47a1b0  DisassociateEntities  (brush.cpp:4329)
// For each selected brush whose owner entity is not worldspawn and has the
// ScriptGroupKey epair, delete that key and re-check the entity's model/color.
// ─────────────────────────────────────────────────────────────────────────────

// Helper defined in scriptgroup.cpp.
extern void ScriptGroup_Type();                                                    // scriptgroup.cpp 0x451200

void DisassociateEntities()
{
    for ( selbrush_t *b = selected_brushes.next;
          b != &selected_brushes;
          b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;

        iassert( b->owner->def == b->def->owner );

        entity_s_def *def = (entity_s_def *)b->owner->def;
        const char *ScriptGroupKey = g_PrefsDlg->ScriptGroupKey.c_str();
        if ( HasKeyValuePair( def, ScriptGroupKey ) )
        {
            DeleteKey( &def->epairs, ScriptGroupKey );
            Checkkey_Model( def, ScriptGroupKey );
            Checkkey_Color( def, ScriptGroupKey );
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x47a330  SelectedAssociated  (brush.cpp:4393)
// Grow the selection to every active brush whose owner entity shares the same
// ScriptGroupKey value as a currently-selected entity; repeat until a pass adds
// nothing.  First resets the edit mode to brush-select (closing a patch point-edit
// drag).  If ScriptGroupKey == ScriptColorTeamKey, defers to ScriptGroup_Type.
// The binary built an MFC CString for the compared value; here it is a std::string.
// ─────────────────────────────────────────────────────────────────────────────
void SelectedAssociated()
{
    std::string matchValue;   // the compared value

    if ( selected_brushes.next == &selected_brushes )
        return;

    select_t prevMode = g_qeglobals.d_select_mode;
    g_qeglobals.d_select_mode = sel_brush;
    if ( prevMode == sel_cycle_edge_direction_quad )
        CMainFrame_UpdatePatchToolbarButtons();

    if ( !strcmp( g_PrefsDlg->ScriptGroupKey.c_str(), g_PrefsDlg->ScriptColorTeamKey.c_str() ) )
    {
        ScriptGroup_Type();
        return;
    }

    bool added;
    do
    {
        added = false;
        for ( selbrush_t *brush = selected_brushes.next; brush != &selected_brushes; brush = brush->next )
        {
            iassert( brush->def->owner == brush->owner->def );

            entity_s *owner = brush->def->owner;
            const char *ScriptGroupKey = g_PrefsDlg->ScriptGroupKey.c_str();
            if ( !HasKeyValuePair( owner, ScriptGroupKey ) )
                continue;

            // The entity's ScriptGroupKey value (empty string when the key is absent or
            // its value is null).
            const char *value = "";
            for ( epair_t *ep = owner->epairs; ep; ep = ep->next )
            {
                if ( !_stricmp( ep->key, ScriptGroupKey ) )
                {
                    if ( ep->value )
                        value = ep->value;
                    break;
                }
            }
            matchValue = value;

            // Pull every active brush whose owner shares this value into the selection.
            // matchNext is captured BEFORE Brush_RemoveFromList/Brush_AddToList2 relink
            // `match` out of active_brushes (the binary saves &match->next->prev first).
            for ( selbrush_t *match = active_brushes.next; match != &active_brushes; )
            {
                selbrush_t *matchNext = match->next;
                entity_s *matchOwner = match->owner;
                if ( matchOwner && matchOwner != world_entity
                     && !FilterBrush( match, 0 )
                     && ( match->brushFlags & 0x20 ) == 0 )
                {
                    iassert( match->def->owner == match->owner->def );

                    if ( Entity_HasEpairMatch( (entity_s *)match->owner->def,
                                               g_PrefsDlg->ScriptGroupKey.c_str(), matchValue.c_str() ) )
                    {
                        Brush_RemoveFromList( match );
                        Brush_AddToList2( match );
                        added = true;
                    }
                }
                match = matchNext;
            }
        }
    } while ( added );

    g_nUpdateBits = -1;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x47a600  OverbrightShift  (brush.cpp:4444)
// Per selected fixed-size entity: a "light" nudges "overbrightShift" by delta (clamped
// 0..1); anything else scales "fixedNodeSafeRadius" down by delta*320 (clamped >= 0).
// ─────────────────────────────────────────────────────────────────────────────
void OverbrightShift( float delta )
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        iassert( b->owner->def == b->def->owner );

        entity_s_def *def = (entity_s_def *)b->owner->def;
        eclass_t *eclass = def->eclass;
        if ( !*(int *)&eclass->fixedsize )
            continue;

        char buf[32];
        if ( !_stricmp( eclass->name, "light" ) )
        {
            float v = Entity_GetFloatValueForKey( def, "overbrightShift" ) + delta;
            if ( v > 1.0f )        v = 1.0f;
            else if ( v < 0.0f )   v = 0.0f;
            KiwiFmt_Exact( buf, sizeof( buf ), v );   // KIWI: %f kept 6 decimals
            SetKeyValue( def, "overbrightShift", buf );
        }
        else
        {
            float v = Entity_GetFloatValueForKey( def, "fixedNodeSafeRadius" );
            if ( v == 0.0f )   v = 256.0f;
            v = v - delta * 320.0f;
            if ( v < 0.0f )   v = 0.0f;
            KiwiFmt_Exact( buf, sizeof( buf ), v );   // KIWI: %f kept 6 decimals
            SetKeyValue( def, "fixedNodeSafeRadius", buf );
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x47a790  Light_ScaleSelected  (brush.cpp:4484)
// Per selected fixed-size entity: a "light" scales its "light" value by scale (default
// 300 if 0); anything else scales "radius" by scale (default 256 if 0).  Both clamped >= 0.
// ─────────────────────────────────────────────────────────────────────────────
void Light_ScaleSelected( float scale )
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        iassert( b->owner->def == b->def->owner );

        entity_s_def *def = (entity_s_def *)b->owner->def;
        eclass_t *eclass = def->eclass;
        if ( !*(int *)&eclass->fixedsize )
            continue;

        char buf[32];
        if ( !_stricmp( eclass->name, "light" ) )
        {
            float v = Entity_GetFloatValueForKey( def, "light" );
            if ( v == 0.0f )   v = 300.0f;
            v = v * scale;
            if ( v < 0.0f )   v = 0.0f;
            KiwiFmt_Exact( buf, sizeof( buf ), v );   // KIWI: %f kept 6 decimals
            SetKeyValue( def, "light", buf );
        }
        else
        {
            float v = Entity_GetFloatValueForKey( def, "radius" );
            if ( v == 0.0f )   v = 256.0f;
            v = v * scale;
            if ( v < 0.0f )   v = 0.0f;
            KiwiFmt_Exact( buf, sizeof( buf ), v );   // KIWI: %f kept 6 decimals
            SetKeyValue( def, "radius", buf );
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x47a910  Light_Height  (brush.cpp:4523)
// Per selected fixed-size entity whose eclass classtype bit 0x40 is set, scale the
// "height" key by scale (default 128 if 0), clamped >= 0.
// ─────────────────────────────────────────────────────────────────────────────
void Light_Height( float scale )
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        iassert( b->owner->def == b->def->owner );

        entity_s_def *def = (entity_s_def *)b->owner->def;
        eclass_t *eclass = def->eclass;
        if ( !*(int *)&eclass->fixedsize )
            continue;
        if ( ( eclass->classtype & 0x40 ) == 0 )
            continue;

        float v = Entity_GetFloatValueForKey( def, "height" );
        if ( v == 0.0f )   v = 128.0f;
        v = v * scale;
        if ( v < 0.0f )   v = 0.0f;
        char buf[32];
        KiwiFmt_Exact( buf, sizeof( buf ), v );       // KIWI: %f kept 6 decimals
        SetKeyValue( def, "height", buf );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  0x47B380  Brush_DrawSubmitFaceWindings — light-region face-winding submit.
//  `inst` is a selbrush_t INSTANCE.  `light` = the light descriptor (lightDesc_t, byte
//  offsets): light+0=class, light+4=cone center, light+16=cone dir, light+28=radius,
//  light+32=cosHalfFov.  `outList` = the region face-list head.
// ═════════════════════════════════════════════════════════════════════════════
#include "primarylights_region.h"
#include <universal/com_math.h>           // CullBoxFromConicSectionOfSphere
extern void  VectorRotateByAxis( float *out, const float *axisMatrix, const float *dir ); // draw.cpp 0x4ba6b0
extern int   MaterialDef_06( qtexture_s *radMtl );                         // materialdef.cpp 0x4318A0
extern bool  MaterialDef_10_LayeredMatHandle( MaterialDef *mtlDef );       // materialdef.cpp 0x431A60
extern char  MtlDef_IsFaceFiltered( MaterialDef *mtlDef );                 // mayaexport.cpp 0x46FBE0
extern void Patch_DrawSubmitFaceWindings( patch_t *patchInst, const orientation_t *orient, float *desc, rface_t **outList );

// Region_BoundsMidpoint (0x409F20) — midpoint of two vec3.
static void Region_BoundsMidpoint( const float *a, const float *b, float *out )
{
    out[0] = ( a[0] + b[0] ) * 0.5f;
    out[1] = ( a[1] + b[1] ) * 0.5f;
    out[2] = 0.5f * ( a[2] + b[2] );
}

// Region_WorldPosToLocal (0x4BA610) — world→local: out[i] = orient.axis[i] · (pos - origin).
// (Distinct from OrientationPosToWorldPos, which is local→world.)
static void Region_WorldPosToLocal( const float *pos, float *out, const orientation_t *orient )
{
    float d[3];
    d[0] = pos[0] - orient->origin[0];
    d[1] = pos[1] - orient->origin[1];
    d[2] = pos[2] - orient->origin[2];
    out[0] = orient->axis[0][0] * d[0] + orient->axis[0][1] * d[1] + orient->axis[0][2] * d[2];
    out[1] = orient->axis[1][0] * d[0] + orient->axis[1][1] * d[1] + orient->axis[1][2] * d[2];
    out[2] = orient->axis[2][0] * d[0] + orient->axis[2][1] * d[1] + orient->axis[2][2] * d[2];
}

void Brush_DrawSubmitFaceWindings( selbrush_t *inst, const orientation_t *orient,
                                   float *light, rface_t **outList )
{
    if ( inst->owner->prefab )
    {
        iassert( !inst->owner->prefab );          // brush.cpp:4831 "!brush->owner->prefab"
    }

    // patch brush → route to the pmesh winding submitter
    if ( inst->patch )
    {
        Patch_DrawSubmitFaceWindings( inst->patch, orient, light, outList );
        return;
    }

    // fixed-size (model/light) entities have no submittable faces
    entity_s *owner = inst->owner;
    eclass_t *eclass = ((entity_s_def *)owner->def)->eclass;
    if ( *(int *)&eclass->fixedsize )
        return;

    // cone cull: a spot light (class 2) with cosHalfFov > 0 tests the brush's def AABB
    // against the light's conic section and skips the brush when culled.
    if ( *(int *)light == 2 && light[8] > 0.0f )   // light+32 = cosHalfFov (= light[8])
    {
        brush_t *def = inst->def;
        float boxMid[3], boxHalf[3], coneCtr[3], coneDir[3];
        Region_BoundsMidpoint( def->mins, def->maxs, boxMid );
        boxHalf[0] = boxMid[0] - def->mins[0];
        boxHalf[1] = boxMid[1] - def->mins[1];
        boxHalf[2] = boxMid[2] - def->mins[2];
        Region_WorldPosToLocal( &light[1], coneCtr, orient );     // light+4 = cone center
        VectorRotateByAxis( coneDir, (const float *)orient, &light[4] );  // light+16 = cone dir
        if ( CullBoxFromConicSectionOfSphere( coneCtr, coneDir, light[8], light[7], boxMid, boxHalf ) )
            return;                                 // ( ..., cosHalfFov, radius, ... ) - as the binary
    }

    winding_t *worldW = Winding_Alloc( 1024 );
    const int layer = g_qeglobals.current_edit_layer;
    for ( int fi = 0; fi < inst->faceCount; fi++ )
    {
        face_t *face = &inst->def->faces[fi];
        winding_t *fw = face->w;
        if ( !fw || (unsigned int)fw->numpoints > 0x400u )
            continue;
        if ( MtlDef_IsFaceFiltered( &face->mtldef[layer] ) )
            continue;

        MaterialDef *md = &face->mtldef[layer];
        int exterior;
        if ( MaterialDef_10_LayeredMatHandle( md ) )
        {
            exterior = 1;
        }
        else
        {
            dword_181F51C = 0x2000;
            MaterialDef_02( md, MaterialDef_06 );
            if ( dword_181F51C )
                continue;                            // not a drawn (shadow) material
            exterior = 0;
        }

        // transform the face winding into the light's orientation, then submit.
        worldW->numpoints = fw->numpoints;
        for ( int i = 0; i < fw->numpoints; i++ )
            OrientationPosToWorldPos( worldW->p[i], fw->p[i], orient );
        Region_AddWindingFace( worldW, outList, light, exterior );
    }

    --g_windingAlloc;
    free( worldW );
}

// ═════════════════════════════════════════════════════════════════════════════
//  XY brush draw — the 2D wireframe line path.
//
//  Pipeline: CXYWnd::XY_Draw -> DrawBrush (per brush) -> DrawGeo -> DrawShadedWireframe
//  (per visible face) -> R_Add3DLine batches winding edges -> R_AddCmd_Line3D.
//
//  Filled/textured faces draw through the per-face faceVis cache (Visuals_InitFaceVis) +
//  the editor vertex-buffer pipeline, gated on faceVis::visCount = the material's layer
//  count.  The TECHNIQUE_WIREFRAME_SHADED branch gates on face->w (built by
//  Brush_BuildWindings at load) instead of visCount (see DrawGeo).
// ═════════════════════════════════════════════════════════════════════════════

// ─────────────────────────────────────────────────────────────────────────────
// 0x47b940 / 0x47b900  Brush_UpdateSpecialMaterialFlag (+ _Recurse) — set the brush DEF's
//   "special material" 2D back-face-cull flag (def->unk01 HIBYTE, read by DrawGeo's
//   cullMode).  For a MODEL entity (eclass.fixedsize) it recurses through the
//   model's content brush tree; for a brush entity it clears the flag, then (when prefs
//   draw_toggle is on) sets it iff ANY face's current-layer material carries the
//   MaterialDef_06 flag (the realize callback ANDs each layer's qtexture unk_flags2 low word
//   into dword_181F51C, seeded to 2 — a non-zero survivor means "special").  Patch brushes
//   (def->patch) are skipped entirely.
// ─────────────────────────────────────────────────────────────────────────────
extern int  MaterialDef_06( qtexture_s *radMtl );                        // materialdef.cpp (0x4318A0)

void Brush_UpdateSpecialMaterialFlag( brush_t *def );                    // fwd (mutually recursive)

// 0x47b900 — walk a model's content brush tree, refreshing each brush's special-material
// flag.  `node` is the model's entity list head; each entity's brush list uses the
// def/brushes head overlay.
static void Brush_UpdateSpecialMaterialFlag_Recurse( entity_s *node )
{
    for ( entity_s *ent = node->next; ent != node; ent = ent->next )
    {
        for ( brush_t *b = (brush_t *)ent->brushes.prev; b != (brush_t *)&ent->def; b = b->onext )
            Brush_UpdateSpecialMaterialFlag( b );
    }
}

void Brush_UpdateSpecialMaterialFlag( brush_t *def )
{
    if ( def->patch )
        return;

    entity_s *owner = def->owner;
    if ( *(int *)&owner->eclass->fixedsize )                 // model/light fixedsize entity
    {
        entitymodel_t *mc = (entitymodel_t *)owner->modelClass;
        if ( !mc )
            return;
        modelnode_t *model = mc->model;
        iassert( model );                                    // "b->owner->modelClass->model" (L0)
        if ( model->entities.next )
        {
            Brush_UpdateSpecialMaterialFlag_Recurse( &model->entities );
        }
        return;
    }

    // brush entity: clear the flag, then set it if any face's material is "special".
    def->unk01 &= 0x00FF;                          // clear the special-material byte
    if ( !g_PrefsDlg->draw_toggle || def->faceCount <= 0 )
        return;

    const int layer = g_qeglobals.current_edit_layer;
    for ( int i = 0; i < def->faceCount; ++i )
    {
        MaterialDef *md = &def->faces[i].mtldef[layer];
        dword_181F51C = 2;
        MaterialDef_02( md, MaterialDef_06 );
        if ( dword_181F51C )                                 // a "special" flag survived
        {
            def->unk01 = (def->unk01 & 0x00FF) | 0x0100;
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x47b5c0  DrawShadedWireframe  (brush.cpp:4888)
// Append one face's closed-winding edges (each point transformed to world space
// through `orient`) into a caller-owned GfxPointVertex batch, with 2D back-face
// culling: cullMode 0=YZ/1=XZ/2=XY drops faces whose world normal points away along
// that view axis; cullMode -1 (special-material brushes) skips the cull. Returns the
// updated vertex count.
// ─────────────────────────────────────────────────────────────────────────────
int DrawShadedWireframe( int cullMode, face_t *face, const orientation_t *orient,
                         GfxColor *lineColor, char width, int vertCount,
                         int vertLimit, GfxPointVertex *verts )
{
    iassert( face );
    iassert( face->w );
    iassert( lineColor );
    iassert( verts );
    vassert( (vertCount >= 0 && vertCount <= vertLimit), "(vertCount) = %i", vertCount );

    if ( cullMode >= 0 && cullMode <= 2 )
    {
        float wdir[3];
        OrientationDirToWorldDir( wdir, orient, face->plane.normal );
        if ( wdir[cullMode] <= 0.0f )
            return vertCount;             // back-facing in this 2D view — skip
    }

    winding_t *w = face->w;
    if ( w->numpoints == 0 )
        return vertCount;

    // Closed outline: first edge wraps last->first, then point[i-1]->point[i].
    const float *cur = &w->p[0][0];
    int prevIdx = w->numpoints - 1;
    for ( int i = 0; i < w->numpoints; ++i )
    {
        vertCount = R_Add3DLine( verts, orient, &w->p[prevIdx][0], cur,
                                 (const unsigned int *)lineColor, width, vertCount, vertLimit );
        prevIdx = i;
        cur += 3;
    }
    return vertCount;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x47b780  Face_AddWindingToTriBatch — append ONE face's winding to a
// caller-owned white-UNLIT triangle batch as a fan, auto-flushing the batch through
// R_AddRenderCmdDrawTris(d_white, TECHNIQUE_UNLIT, ...) when the next winding would
// overrun the caller's 0x552-vertex / 0x7FB-index caps.
//   • per vertex: xyzw = winding point + w=1, normal = the FACE PLANE normal,
//     colour = the caller's packed colour word (bit-cast into the float colour array),
//     st = {0,0}.
//   • indices: a one-sided fan (base, base+i-1, base+i) for i = 2 .. numpoints-1.
// The ONLY caller is CCamWnd::Cam_Draw's selected-FACE fill pass.
// `packedColor` is a pointer to a GfxColor read as a float — the binary stores the raw
// dword into the float colour array (a bit-cast, not a value cast).
// ─────────────────────────────────────────────────────────────────────────────
void Face_AddWindingToTriBatch( face_t *face, const float *packedColor,
                                int *indexCount, ushort *indices,
                                int *vertCount, float ( *xyzw )[4],
                                float ( *normal )[3], float *colorArr,
                                float ( *st )[2] )
{
    winding_t *w = face->w;
    if ( !w )
        return;

    // flush when the append would overflow either cap.
    if ( w->numpoints + *vertCount > 0x552
      || *indexCount + 3 * w->numpoints - 6 > 0x7FB )
    {
        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                (short)*indexCount, indices, (short)*vertCount,
                                xyzw, normal, colorArr, st );
        *indexCount = 0;
        *vertCount  = 0;
    }

    // vertex append (positions + face-plane normal + flat colour + zero UVs).
    for ( int i = 0; i < w->numpoints; ++i )
    {
        const int v = *vertCount + i;
        xyzw[v][0] = w->p[i][0];
        xyzw[v][1] = w->p[i][1];
        xyzw[v][2] = w->p[i][2];
        xyzw[v][3] = 1.0f;
        normal[v][0] = face->plane.normal[0];
        normal[v][1] = face->plane.normal[1];
        normal[v][2] = face->plane.normal[2];
        colorArr[v]  = *packedColor;
        st[v][0] = 0.0f;
        st[v][1] = 0.0f;
    }

    // one-sided triangle fan off the winding's first point.
    if ( w->numpoints > 2 )
    {
        int ic = *indexCount;
        for ( int i = 2; i < w->numpoints; ++i )
        {
            indices[ic]     = (ushort)( *vertCount );
            indices[ic + 1] = (ushort)( *vertCount + i - 1 );
            indices[ic + 2] = (ushort)( *vertCount + i );
            ic += 3;
        }
        *indexCount = ic;
    }
    *vertCount += w->numpoints;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x47acf0  DrawGeo  (brush.cpp:4652) — wireframe + FILLED branches.
// Per winding-bearing face (with per-face visuals built by Brush_CheckBuildFaceVis):
//   • technique 29 (WIREFRAME_SHADED): batch the face outline (DrawShadedWireframe).
//   • mtlOverride set: emit ONE surf with the override material (Editor_AddGeoFace,
//     + Editor_AddGeoBackFace for special-material brushes).  This is the arm the
//     binary's tint/sun passes use (DrawBrush's 4th param).
//   • else: emit each material LAYER's visual {mtlHandle, vertHandle} at sortKey
//     Editor_MaterialSortKey(firstLayerMaterial)+layerIndex — the filled/textured surf-cache draw
//     the camera consumes via R_AddEditorSurfsCmd → RC_DRAW_EDITOR_SKINNEDCACHED.
// Gates (binary order): face->w → vis->visCount → !MtlDef_IsFaceFiltered →
// MaterialDef_15_Drawflag_Multiply.  ONE documented divergence: the WIREFRAME branch
// draws even when visCount==0 — the port's headless/no-device faceVis is the identity
// array (visCount=0, see Brush_MakeFaceVisuals' Radiant_FaceVisGpuReady gate), and
// gating the lines on it would blank every 2D view in the device-less selftests.
// ─────────────────────────────────────────────────────────────────────────────
extern int  Editor_MaterialSortKey( Material *handle );                       // r_ed_scene.cpp 0x4FDBB0
extern void Editor_AddGeoFace( Material *handle, int techType, int sortKey,
                               int vertCount, int vbIndexAndOffs );           // r_ed_scene.cpp 0x4FEEF0
extern void Editor_AddGeoBackFace( Material *handle, int techType, int sortKey,
                                   int vertCount, int vbIndexAndOffs );       // r_ed_scene.cpp 0x4FEF50
extern bool MaterialDef_15_Drawflag_Multiply( int drawFlags, MaterialDef *m );// materialdef.cpp 0x431E90

// True if `mtl` is the CoD4 "$opaque" clip/tool editor material (basename "$opaque",
// after stripping any "wc/" path).  See the CoD3↔CoD4 techset note at the FILLED-branch
// call site: this material has no lit technique in the CoD4 binary but does in kisak's
// CoD3-loaded form, so it must not draw as an opaque lit surface in the camera.
static bool Material_IsToolOpaque( Material *mtl )
{
    if ( !mtl )
        return false;
    const Material *m = Material_FromHandle( mtl );   // resolve handle → struct (as Editor_AddMeshCmd)
    if ( !m || !m->info.name )
        return false;
    const char *slash = strrchr( m->info.name, '/' );
    const char *base  = slash ? slash + 1 : m->info.name;
    return strcmp( base, "$opaque" ) == 0;
}

void DrawGeo( GfxColor *col, Material *mtlOverride, selbrush_t *b,
              const orientation_t *orient, int viewType, int technique,
              char width, int drawFlags )
{
    iassert( b->owner->def == b->def->owner );

    brush_t *def = b->def;
    if ( def->faceCount <= 0 )
        return;

    // A function-local static keeps the ~85 KB batch off the stack; the editor is
    // single-threaded and DrawGeo is non-reentrant.
    static GfxPointVertex s_lineVerts[5450];
    int vertCount = 0;

    // HIBYTE(def->unk01) marks "special material" brushes: suppress 2D back-face
    // culling (cullMode -1) on the wireframe, and ALSO emit the back-facing fan on
    // the filled branches.
    const bool specialMtl = ( ( (unsigned __int16)def->unk01 >> 8 ) & 0xFF ) != 0;
    const int  cullMode   = specialMtl ? -1 : viewType;

    for ( int facenum = 0; facenum < def->faceCount; ++facenum )
    {
        face_t *face = &def->faces[facenum];
        if ( !face->w )
            continue;
        faceVis_s *vis = b->faces ? &( (faceVis_s *)b->faces )[facenum] : nullptr;

        // per-face material filter gates BOTH branches.
        MaterialDef *mtldef = &face->mtldef[g_qeglobals.current_edit_layer];
        if ( MtlDef_IsFaceFiltered( mtldef ) || !MaterialDef_15_Drawflag_Multiply( drawFlags, mtldef ) )
            continue;

        if ( technique == TECHNIQUE_WIREFRAME_SHADED )
        {
            // (binary also gates the wireframe on vis->visCount — see the header
            // note on the headless identity-faceVis divergence.)
            vertCount = DrawShadedWireframe( cullMode, face, orient, col, width,
                                             vertCount, 5450, s_lineVerts );
            continue;
        }

        // FILLED branches need the GPU visuals.
        if ( !vis || !vis->visCount || !vis->visArray )
            continue;

        if ( mtlOverride )                              // single override surf
        {
            int sortKey = Editor_MaterialSortKey( mtlOverride );
            Editor_AddGeoFace( mtlOverride, technique, sortKey, vis->vertcount,
                               vis->visArray->vertHandle );
            if ( specialMtl )
                Editor_AddGeoBackFace( mtlOverride, technique, sortKey, vis->vertcount,
                                       vis->visArray->vertHandle );
        }
        else                                            // per-layer visuals
        {
            // Binary: base = Editor_MaterialSortKey(visArray[0].handle), then base+i per layer.
            // Defensive: a layer whose GPU build failed stores mtlHandle=NULL
            // (Visuals_InitFaceVis) — the binary has no such case; skip those entries
            // (and seed the base key from the first non-null layer).
            int sortKey = -1;
            for ( int i = 0; i < vis->visCount; ++i )
            {
                Material *mtl = vis->visArray[i].mtlHandle;
                if ( !mtl )
                    continue;
                // CoD3↔CoD4 TECHSET DIVERGENCE: the CoD4 clip/tool editor material
                // "$opaque" has NO lit (FAKELIGHT_NORMAL/VIEW, tech 24/25) technique — the
                // binary's Editor_AddMeshCmd technique gate (techniques[techType+1]) skips it,
                // so a prefab's clip-hull faces never draw as opaque lit surfaces.  kisak's
                // CoD3-loaded "$opaque" DOES carry a fakelight technique + a 1×1 white colorMap,
                // so the SAME face flushed white-opaque over the model (BRUSHPROBE: wc/$opaque,
                // opaque loadBits, fakelight_normal(24) = the white shell around the ch46e).
                // Reproduce the binary's technique-gate RESULT: don't emit the $opaque tool
                // material at a LIT camera technique.  Tech 29 (wireframe, 2D) is unaffected;
                // non-tool world/model materials (real colorMap) are unaffected.
                if ( ( technique == TECHNIQUE_FAKELIGHT_NORMAL || technique == TECHNIQUE_FAKELIGHT_VIEW ) && Material_IsToolOpaque( mtl ) )
                    continue;
                if ( sortKey < 0 )
                    sortKey = Editor_MaterialSortKey( mtl );
                Editor_AddGeoFace( mtl, technique, sortKey + i, vis->vertcount,
                                   vis->visArray[i].vertHandle );
                if ( specialMtl )
                    Editor_AddGeoBackFace( mtl, technique, sortKey + i, vis->vertcount,
                                           vis->visArray[i].vertHandle );
            }
        }
    }

    if ( vertCount )
        R_AddCmd_Line3D( (short)( vertCount / 2 ), width, s_lineVerts );
}

// ─── patch control-point overlay (defined just below DrawPatchesWireframeGrid) ──
// DrawPatchesWireframeGrid tails with Patch_DrawControlPoints, so it's forward-declared.
void  Patch_DrawControlPoints( patch_t *patchInstance, const orientation_t *orient );

// ═════════════════════════════════════════════════════════════════════════════
// 0x441620  DrawPatchesWireframeGrid  (PMESH.CPP:4137) — PATCH wireframe.
//
// Draws a curve patch's tessellated render mesh (patchMesh_t.curveDef, built by
// Patch_GenericMesh2) as a wireframe grid of 3D lines — the patch analogue of
// DrawGeo, and the path DrawBrush takes for a patch brush in a 2D (XY/XZ/YZ) view
// with technique 29 (TECHNIQUE_WIREFRAME_SHADED).  Each tessellated quad cell emits
// its top/left edges (shared edges drawn once via the col==0 / row==0 guards) plus
// the two far edges and one diagonal; the lines batch through R_Add3DLine →
// R_AddCmd_Line3D, exactly like the brush wireframe.
//
// Adaptations:
//  * The binary first calls Patch_Fill_SyncVersion (sync the patch INSTANCE's visuals
//    to the def version).  That instance rebuild builds the FILLED draw's per-layer
//    visuals (MaterialDef_11 layer counts) which the WIREFRAME does not read — so it is
//    skipped; instead the def's curveDef is ensured (Patch_ParseMesh already builds it
//    at load; this is the safety net for a never-tessellated patch).
//  * MaterialDef_15_Drawflag_Multiply (the draw-flag gate at the top) returns 1 for
//    the line path (drawFlags has no DRAWFLAG_ONLY/SKIP_MULTIPLY bits), so it is a
//    passthrough here — matching the degenerate-materialdef shim already used by the
//    brush wireframe.
//  * Patch_DrawControlPoints (the control-POINT marker overlay, called at the tail)
//    only draws in patch point-edit modes; in a normal view it early-returns.
//  The diagonal direction matches the binary: terrain patches honour the control
//  vertex's `turned_edge` flag (terrain render mesh == control grid, so the index is
//  in range); bezier patches always take the v01→v10 diagonal (type != 64).
// ═════════════════════════════════════════════════════════════════════════════
void DrawPatchesWireframeGrid( patch_t *patchInstance, GfxColor *col,
                               const orientation_t *orient, char width, int drawFlags )
{
    if ( !patchInstance )
        return;
    patchMesh_t *def = patchInstance->def;                 // instance.def == patch DEF
    if ( !def )
        return;

    // Ensure the render mesh exists (built at load; rebuild defensively if absent).
    if ( !def->curveDef )
    {
        def->curveDef = Patch_GenericMesh2( def, g_qeglobals.current_edit_layer, 0, 0 );
        ++def->version;
        if ( !def->curveDef )
            return;
    }

    curvePatchDef_t *mesh = def->curveDef;
    int   mw   = mesh->width;
    int   mh   = mesh->height;
    if ( mw <= 0 || mh <= 0 )
        return;
    curveVert_t *verts = mesh->verts;

    const bool terrain = ( *(int *)&def->type == 64 );      // PATCH_TERRAIN

    // Function-local static line batch, as DrawGeo (single-threaded, non-reentrant).
    static GfxPointVertex s_patchVerts[1362];
    const int kLimit = 1362;
    const unsigned int *lineCol = (const unsigned int *)col;
    int vc = 0;

    for ( int row = 0; row + 1 < mh; ++row )
    {
        for ( int col2 = 0; col2 + 1 < mw; ++col2 )
        {
            const float *v00 = verts[col2     +   row       * mw].xyz;   // [col][row]
            const float *v10 = verts[col2 + 1 +   row       * mw].xyz;   // [col+1][row]
            const float *v01 = verts[col2     + ( row + 1 ) * mw].xyz;   // [col][row+1]
            const float *v11 = verts[col2 + 1 + ( row + 1 ) * mw].xyz;   // [col+1][row+1]

            // shared edges drawn once: left edge only for the first column, top edge
            // only for the first row.
            if ( col2 == 0 )
                vc = R_Add3DLine( s_patchVerts, orient, v00, v01, lineCol, width, vc, kLimit );
            if ( row == 0 )
                vc = R_Add3DLine( s_patchVerts, orient, v00, v10, lineCol, width, vc, kLimit );
            // far edges of this cell.
            vc = R_Add3DLine( s_patchVerts, orient, v11, v10, lineCol, width, vc, kLimit );
            vc = R_Add3DLine( s_patchVerts, orient, v11, v01, lineCol, width, vc, kLimit );
            // diagonal: terrain honours the control vertex's turned_edge flag
            // (terrain render mesh == control grid, so [col][row] is in range);
            // bezier always takes the v01→v10 diagonal.
            bool turned = terrain && ( def->ctrl[col2][row].turned_edge & 1 );
            if ( turned )
                vc = R_Add3DLine( s_patchVerts, orient, v00, v11, lineCol, width, vc, kLimit );
            else
                vc = R_Add3DLine( s_patchVerts, orient, v01, v10, lineCol, width, vc, kLimit );
        }
    }

    if ( vc )
    {
        R_AddCmd_Line3D( (short)( vc / 2 ), width, s_patchVerts );
    }

    // Control-POINT marker overlay: the binary's DrawPatchesWireframeGrid tails with
    // Patch_DrawControlPoints to draw this patch's control-point grid as point markers
    // when a patch-point edit mode is active (display subset).
    Patch_DrawControlPoints( patchInstance, orient );
}

// ═════════════════════════════════════════════════════════════════════════════
// 0x440750  Patch_DrawControlPoints  (PMESH.CPP) — per-patch
// CONTROL-POINT MARKER overlay.  Called at the tail of DrawPatchesWireframeGrid /
// DrawPatches / 0x4415D0 (all the per-patch draws).  In a patch point-edit mode
// (d_select_mode == sel_curvepoint|sel_area, or bend/redisperse modes) it walks the
// patch's 16×16 control grid and emits each control point as a coloured 3D point
// marker (R_AddPointCmd_W, size 6) so the user can see and pick the grid handles.
//
// SCOPE: the STEADY-STATE display branch (the binary's
// `!(g_bPatchBendMode || g_qeglobals_redispersePatchVerts)` tail) — the
// plain sel_curvepoint/sel_area marker draw.  The redisperse-mode branch is not ported
// (KIWI: the bend-mode one went with bend mode).
//
// COLOURS:
//   red     (1,0,0)  — a control vertex flagged turned_edge&2 (a "marked" CP)
//   blue    (0,0,1)  — an even-parity (row|col even) interior control point
//   magenta (1,0,1)  — an odd-parity control point (the checkerboard)
//   blue    (0,0,1)  — a SELECTED control point (in d_move_points), drawn
//                      on top in a 2nd reverse-order pass.
// SELECTION TEST: Patch_FindSelectedMovePoint(ctrl) → index into
// g_qeglobals.d_move_points[] (the currently-selected control points, matched by xyz)
// or -1.  >=0 ⇒ deferred to the on-top "selected" pass; <0 ⇒ drawn now with the
// checkerboard colour.
//
// The instance's "draw this patch" byte (instance.selected, set by Brush_Build,
// brush.cpp:991) gates the draw.
// ═════════════════════════════════════════════════════════════════════════════
// Patch_FindSelectedMovePoint (0x43c2c0): index of `ctrl` in d_move_points[] (selected
// set), else -1.  Each d_move_points[i] is matched by xyz within a tiny epsilon (1e-6
// per axis).
static int Patch_FindSelectedMovePoint( const drawVert_t *ctrl )
{
    for ( int i = 0; i < g_qeglobals.d_num_move_points; ++i )
    {
        const drawVert_t *mp = g_qeglobals.d_move_points[i];
        int matched = 0;
        for ( ; matched < 3; ++matched )
        {
            float d = ctrl->xyz[matched] - mp->xyz[matched];
            if ( d * d > 1.00000011e-06f )
                break;
        }
        if ( matched >= 3 )
            return i;
    }
    return -1;
}

void Patch_DrawControlPoints( patch_t *patchInstance, const orientation_t *orient )
{
    if ( !patchInstance )
        return;

    // instance.selected (the per-patch "draw" flag) must be set, and a patch point-edit
    // mode must be active (the binary's top guard).
    if ( patchInstance->selected == 0 )
        return;

    if ( g_qeglobals.d_select_mode != sel_curvepoint &&
         g_qeglobals.d_select_mode != sel_area &&
         !g_qeglobals_redispersePatchVerts )
        return;

    patchMesh_t *def = patchInstance->def;               // instance.def == patch DEF
    if ( !def )
        return;

    // The redisperse-mode branch is not ported (KIWI: bend mode, the other, is gone):
    // render nothing in it (matches the pre-unit behaviour for that mode).
    if ( g_qeglobals_redispersePatchVerts )
        return;

    // Colours, packed once, reused per point.
    static const float s_red[4]     = { 1.0f, 0.0f, 0.0f, 1.0f };   // turned_edge&2
    static const float s_blue[4]    = { 0.0f, 0.0f, 1.0f, 1.0f };   // even-parity
    static const float s_selBlue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };   // selected
    static const float s_magenta[4] = { 1.0f, 0.0f, 1.0f, 1.0f };   // odd-parity
    GfxColor cRed, cBlue, cSelBlue, cMagenta;
    Byte4PackPixelColor( const_cast<float *>( s_red ),     &cRed );
    Byte4PackPixelColor( const_cast<float *>( s_blue ),    &cBlue );
    Byte4PackPixelColor( const_cast<float *>( s_magenta ), &cMagenta );
    Byte4PackPixelColor( const_cast<float *>( s_selBlue ), &cSelBlue );

    // Batch points (flush at 1361); DrawGeo's static-buffer pattern (single-threaded,
    // non-reentrant — the editor draws one window at a time).
    static GfxPointVertex s_pts[1362];
    int n = 0;

    // Selected control points are deferred to a 2nd reverse-order pass.
    const drawVert_t *selStack[258];
    int selCount = 0;

    const int rows = def->width;     // control grid width
    const int cols = def->height;    // control grid height

    for ( int i = 0; i < rows; ++i )
    {
        for ( int j = 0; j < cols; ++j )
        {
            const drawVert_t *cp = &def->ctrl[i][j];

            if ( Patch_FindSelectedMovePoint( cp ) >= 0 )
            {
                // selected → on-top pass
                if ( selCount < 258 )
                    selStack[selCount++] = cp;
                continue;
            }

            // not selected → checkerboard colour now.
            GfxColor *col;
            if ( ( cp->turned_edge & 2 ) != 0 )
                col = &cRed;
            else if ( ( ( i | j ) & 1 ) != 0 )
                col = &cMagenta;
            else
                col = &cBlue;

            OrientationPosToWorldPos( s_pts[n].xyz, cp->xyz, orient );
            *(GfxColor *)s_pts[n].color = *col;
            if ( ++n == 1361 ) { R_AddPointCmd_W( (short)n, 6, s_pts ); n = 0; }
        }
    }

    // 2nd pass: the selected control points, in REVERSE order, in the selected colour.
    for ( int s = selCount - 1; s >= 0; --s )
    {
        OrientationPosToWorldPos( s_pts[n].xyz, selStack[s]->xyz, orient );
        *(GfxColor *)s_pts[n].color = cSelBlue;
        if ( ++n == 1361 ) { R_AddPointCmd_W( (short)n, 6, s_pts ); n = 0; }
    }

    if ( n )
        R_AddPointCmd_W( (short)n, 6, s_pts );
}

// ═════════════════════════════════════════════════════════════════════════════
// 0x47D060  BrushList_RebuildDisplay — brush-list DISPLAY REBUILD.  For every instance on the given
// display list: bump its DEF's version word (forces the faceVis/winding rebuild on the
// next draw), and for a PATCH brush re-tessellate the patch DEF at the CURRENT EDIT
// LAYER (free old curveDef → Patch_GenericMesh2 → ++def->version → assign).  If the
// instance's owner entity is a PREFAB, recurse into the prefab's own brush list
// (its embedded selbrush sentinel), then bump the owner DEF's 16-bit model version word
// (the version_prob_wrong idiom).  31 call sites: layer switch / relayer / script-group
// / layered-material / prefab enter-leave.
void BrushList_RebuildDisplay( selbrush_t *listHead )
{

    selbrush_t *sentinel = (selbrush_t *)(intptr_t)listHead;
    for ( selbrush_t *i = sentinel->next; i != sentinel; i = i->next )
    {
        ++i->def->version;
        if ( i->patch )
        {
            patchMesh_t *pm = i->patch->def;
            if ( pm->curveDef )
                free( pm->curveDef );
            curvePatchDef_t *cd =
                Patch_GenericMesh2( pm, g_qeglobals.current_edit_layer, 0, 0 );
            ++pm->version;
            pm->curveDef = cd;
        }
        entity_s *owner = i->owner;
        if ( owner )
        {
            if ( owner->prefab )
                BrushList_RebuildDisplay( &((prefab_s *)owner->prefab)->brushes );
            entity_s *eDef = (entity_s *)owner->def;
            ++*(unsigned __int16 *)&eDef->version_prob_wrong; // the def's 16-bit model version
        }
    }
}

// 0x40c250  Draw_PatchSelectPoints  (CamWnd.cpp) — the curve-point CANDIDATE overlay,
// called once per view from BOTH XY_Draw and DrawGeneralWorld_.
// When a patch point-edit mode is active (sel_curvepoint|sel_area) it walks
// g_qeglobals.patch_verts_array01 (the displayed candidate-point list, vec3 world
// coords, count patch_verts_array01_count) and draws each point that is NOT in the
// selected set (Patch_FindSelectedArrayPoint returns -1) as a GREEN marker
// ({0,0.7,0,1}).  The >=0 (selected) points are SKIPPED here; they are drawn LIGHT
// BLUE by the inverse twin Draw_PatchSelectPointsSelected (below, called from the
// DrawConnectionLinks prefix).
//
// The arrays live in qeglobals_t (patch_verts_array01 starts at the
// `unkown_pmesh_float1` field; each entry is a vec3 / 12 bytes).  The points are
// already in WORLD space, so they go through the IDENTITY orientation
// (world_orient_matrix), as in the binary.
// ═════════════════════════════════════════════════════════════════════════════
// Patch_FindSelectedArrayPoint (0x43c1c0): index of world point `pt` in patch_verts_array02 (the SELECTED
// curve points, vec3 stride), within 0.1u (dist² < 0.01), else -1.
// NON-STATIC: also called by Patch_SelectAreaPointsPass (pmesh.cpp, 0x4484c0) — the
// sel_area rect-select path; see the extern decl there.
int Patch_FindSelectedArrayPoint( const float *pt )
{
    if ( g_qeglobals.patch_verts_array02_count <= 0 )
        return -1;
    // array02's base is &unkown_pmesh_float2 (the field 8 bytes BEFORE the qe3.h
    // `patch_verts_array02` symbol), +12 per element — mirroring array01's base
    // (= unkown_pmesh_float1).
    const float *p = &g_qeglobals.unkown_pmesh_float2;
    for ( int i = 0; i < g_qeglobals.patch_verts_array02_count; ++i, p += 3 )
    {
        float dx = p[0] - pt[0];
        float dy = p[1] - pt[1];
        float dz = p[2] - pt[2];
        if ( dx * dx + dy * dy + dz * dz < 0.01f )
            return i;
    }
    return -1;
}

void Draw_PatchSelectPoints()
{
    if ( g_qeglobals.d_select_mode != sel_curvepoint &&
         g_qeglobals.d_select_mode != sel_area )
        return;
    if ( g_qeglobals.patch_verts_array01_count <= 0 )
        return;

    static const float s_green[4] = { 0.0f, 0.7f, 0.0f, 1.0f };
    GfxColor col;
    Byte4PackPixelColor( const_cast<float *>( s_green ), &col );

    static GfxPointVertex s_pts[1362];
    int n = 0;

    // patch_verts_array01 begins at the `unkown_pmesh_float1` field (3-float stride).
    const float *pt = &g_qeglobals.unkown_pmesh_float1;
    for ( int i = 0; i < g_qeglobals.patch_verts_array01_count; ++i, pt += 3 )
    {
        if ( Patch_FindSelectedArrayPoint( pt ) >= 0 )  // selected: skip — those
            continue;                                   // draw via the Selected twin
        OrientationPosToWorldPos( s_pts[n].xyz, pt,
                                  (const orientation_t *)world_orient_matrix );
        *(GfxColor *)s_pts[n].color = col;
        if ( ++n == 1362 ) { R_AddPointCmd_W( (short)n, 6, s_pts ); n = 0; }
    }

    if ( n )
        R_AddPointCmd_W( (short)n, 6, s_pts );
}

// 0x40c360  Draw_PatchSelectPointsSelected — the inverse twin of
// Draw_PatchSelectPoints (identical body except colour + gate): draws each
// patch_verts_array01 point that IS in the selected set (Patch_FindSelectedArrayPoint
// >= 0) as a LIGHT-BLUE marker ({0.5,0.5,1,1}).
// Same self-gate (sel_curvepoint|sel_area), same batching (flush at 1362,
// R_AddPointCmd_W technique 6, identity orientation).  The binary calls it from the
// DrawConnectionLinks (0x40c9f0) prefix — i.e. from BOTH XY_Draw and Cam_Draw tails —
// right before the sel_vertex/sel_edge handle draw.
void Draw_PatchSelectPointsSelected()
{
    if ( g_qeglobals.d_select_mode != sel_curvepoint &&
         g_qeglobals.d_select_mode != sel_area )
        return;
    if ( g_qeglobals.patch_verts_array01_count <= 0 )
        return;

    static const float s_selBlue[4] = { 0.5f, 0.5f, 1.0f, 1.0f };
    GfxColor col;
    Byte4PackPixelColor( const_cast<float *>( s_selBlue ), &col );

    static GfxPointVertex s_pts[1362];
    int n = 0;

    const float *pt = &g_qeglobals.unkown_pmesh_float1;
    for ( int i = 0; i < g_qeglobals.patch_verts_array01_count; ++i, pt += 3 )
    {
        if ( Patch_FindSelectedArrayPoint( pt ) < 0 )   // unselected: skip
            continue;
        OrientationPosToWorldPos( s_pts[n].xyz, pt,
                                  (const orientation_t *)world_orient_matrix );
        *(GfxColor *)s_pts[n].color = col;
        if ( ++n == 1362 ) { R_AddPointCmd_W( (short)n, 6, s_pts ); n = 0; }
    }

    if ( n )
        R_AddPointCmd_W( (short)n, 6, s_pts );
}


// ─────────────────────────────────────────────────────────────────────────────
//  DrawBrush_PrefabContents — the prefab-render slice of DrawModels (0x4796F0) +
//  0x478B10, reduced to the line path.
//
//  When a misc_prefab entity's bbox brush is drawn, the binary calls DrawModels ->
//  0x478B10, which iterates the entity's prefab_s->active_brushlist (the brush
//  INSTANCES of the referenced .map, built by Entity_InitPrefabInst) and recursively
//  DrawBrush()es each through the entity's FORWARD orientation (Entity_GetOrientation,
//  NOT the inverse the bbox uses).  Those prefab brushes are plain worldspawn-class
//  brushes, so the recursion bottoms out in DrawGeo.
//
//  Skipped vs 0x478B10: the spawnflags drawFlags|=2 "ghost" toggle (read, but the
//  line path ignores it).  Filtered prefab brushes (FilterBrush) ARE
//  honoured.
// ─────────────────────────────────────────────────────────────────────────────

// Forward decl (DrawBrush_PrefabContents recurses into DrawBrush, defined just below).
void DrawBrush( selbrush_t *b, const orientation_t *orient, int viewType,
                int technique, GfxColor *col, char width, int drawFlags,
                const char *layerPrefix );

// ─────────────────────────────────────────────────────────────────────────────
// 0x479d50  DrawAngles  (brush.cpp:4229) — the entity ANGLE ARROW overlay.
//
// The View→Show→Angles toggle (d_xyShowFlags bit 0x2; SET = arrows HIDDEN) draws a
// small arrowhead (a forward stick + two barbs) from a fixedsize point-entity origin
// in the direction of its "angles" key, in BOTH the XY views and the camera (DrawBrush
// is shared by both).
//
// drawType is the view-axis index 0..2 for the XY/camera passes; drawType == -1 is the
// binary's special "use the code-default grey, then white for the barbs" rendering
// (selected/active pass) — the editor's XY/camera passes pass the view-axis (!= -1), so
// the passed colour is used, but the -1 branch is kept.
//
// Reuses the line batcher already proven for the connections overlay (R_Add3DLine →
// R_AddCmd_Line3D) and the engine math helpers (AnglesToAxis / OrientationConcatenate).
// ─────────────────────────────────────────────────────────────────────────────
extern void   OrientationConcatenate( const orientation_t *orFirst,
                                      const orientation_t *orSecond,
                                      orientation_t *out );                          // engine_stubs.cpp 0x4ba7d0

// The code-default arrow colour (grey) for the drawType == -1 pass.
static const float kAngleArrowDefault[4] = { 0.5f, 0.5f, 0.5f, 1.0f };

// The selbrush_t whose owner entity DrawAngles reads (the binary passed it as a register
// arg; this single-threaded-editor global keeps DrawAngles' signature).
selbrush_t *g_drawAnglesBrush = nullptr;

// ── KIWI-UX (ROUND AV, ITEMS 1+2): draw_meth2 — the xmodel MESH technique ────────────
// The binary's DrawBrush (0x47afc0) takes TWO techniques: draw_meth1 (geometry / prefab
// contents / the placeholder bbox) and draw_meth2 (the xmodel MESH — SkinModelInst at
// 0x479735 reads draw_meth2 for BOTH its techType and its `draw_meth2 != 29 ? 0 : color`
// per-vertex colour arm).  This port collapsed them into one `technique` parameter, so
// every caller forced the mesh to whatever the geometry wanted.  That is what made
// View->Entities-as... dead: the show state could not reach the mesh.
// Published the way g_drawAnglesBrush publishes DrawAngles' ECX argument.  -1 means
// "draw_meth2 == draw_meth1", which is what every caller except the camera's two entity
// passes wants (the 2D views and the selected-white-outline pass genuinely do want 29).
int g_drawBrushMeshTech = -1;

void DrawAngles( int drawType, const orientation_t *orient, GfxColor *col )
{
    // KIWI: the master switch also gates legacy arrows, including forced selected
    // passes and recursive prefab children. Keep the native secondary gates below.
    if ( !KiwiUX_ShowFacingArrows() )
        return;
    // `b` is the selbrush_t whose owner entity supplies the angles (DrawBrush publishes
    // it in g_drawAnglesBrush).
    selbrush_t *b = g_drawAnglesBrush;
    if ( !b )
        return;

    entity_s *owner = b->owner;
    if ( owner == world_entity )
        return;                                           // worldspawn has no angle arrow

    iassert( b->owner->def == b->def->owner );

    entity_s_def *eDef = (entity_s_def *)owner->def;
    if ( !eDef || !HasKeyValuePair( eDef, "angles" ) )
        return;                                           // no "angles" key → no arrow

    eclass_t *eclass = eDef->eclass;
    if ( !eclass )
        return;
    // Drawn for fixedsize entities, or the representative def-brush of a brush entity,
    // and never for prefab classes (classtype & 0x10).  The brush-entity gate compares
    // `b->def (brush_t*)` against the entity DEF's embedded def-brush list head's first
    // pointer, a brush_t* (the def list, NOT the instance list `selbrush_t.ownerPrev`),
    // read raw.
    brush_t *repBrush = (brush_t *)eDef->brushes.prev;
    bool isRep = ( b->def == repBrush );
    if ( ( !*(int *)&eclass->fixedsize && !isRep ) || ( eclass->classtype & 0x10 ) != 0 )
        return;

    // angles → orientation axis
    float angles[3] = { 0.0f, 0.0f, 0.0f };
    if ( !Entity_GetVec3ForKey( eDef, angles, "angles" ) )
    { angles[0] = angles[1] = angles[2] = 0.0f; }

    orientation_t entOrient;
    AnglesToAxis( angles, entOrient.axis );

    // origin = centre of the eclass bbox + the entity's world origin
    entOrient.origin[0] = ( eclass->mins[0] + eclass->maxs[0] ) * 0.5f + eDef->origin[0];
    entOrient.origin[1] = ( eclass->mins[1] + eclass->maxs[1] ) * 0.5f + eDef->origin[1];
    entOrient.origin[2] = ( eclass->mins[2] + eclass->maxs[2] ) * 0.5f + eDef->origin[2];

    orientation_t worldOrient;
    OrientationConcatenate( &entOrient, orient, &worldOrient );

    // arrowhead scale = half the brush's X extent
    const float scale = ( b->def->maxs[0] - b->def->mins[0] ) * 0.5f;
    const float base[3] = { 0.0f, 0.0f, 0.0f };
    const float tip[3]  = { 1.62399995f * scale, 0.0f, 0.0f };
    const float wing1[3]= { 1.27799999f * scale,  0.34599998f * scale, 0.0f };
    const float wing2[3]= { 1.27799999f * scale, -0.34599998f * scale, 0.0f };

    GfxPointVertex verts[6];
    GfxColor arrowCol;
    if ( drawType == -1 )
        Byte4PackPixelColor( const_cast<float *>( kAngleArrowDefault ), &arrowCol );
    else
        arrowCol.packed = col->packed;

    int vc = R_Add3DLine( verts, &worldOrient, base, tip, (const unsigned int *)&arrowCol, 2, 0, 6 );
    vc      = R_Add3DLine( verts, &worldOrient, tip, wing1, (const unsigned int *)&arrowCol, 2, vc, 6 );
    if ( drawType == -1 )
        Byte4PackPixelColor( const_cast<float *>( colorWhite ), &arrowCol );
    vc      = R_Add3DLine( verts, &worldOrient, tip, wing2, (const unsigned int *)&arrowCol, 2, vc, 6 );
    if ( vc )
    {
        R_AddCmd_Line3D( (short)( vc / 2 ), 2, verts );
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  RENDER DECORATIONS — the entity ORIGIN BOX + the ScriptGroup colour billboards.
//  These are the decoration half of DrawModels (0x4796F0): the MESH half (SkinModelInst)
//  is the separate Editor_DrawModelsPass / Cam_DrawModels pass; this block holds the
//  per-entity origin-box wireframe (DrawOriginBox) + the script-colour name billboards
//  (Ed_DrawScriptColorQuad / Ed_DrawScriptForceColor) that the binary's DrawModels
//  draws after the mesh, then a DrawModels_Decorations() wrapper that reproduces the
//  binary's DrawModels colour/extent logic and is called from DrawBrush's fixedsize
//  branch (where the binary calls DrawModels). DrawLightsMain's glow-sphere lives in
//  camwnd.cpp (Cam_DrawLightPreviewSpheres).
// ═════════════════════════════════════════════════════════════════════════════
extern int   ScriptGroup_Unreachable( const char *a1 );              // scriptgroup.cpp 0x451170
extern bool  ScriptGroup_BrushIsTrigger( selbrush_t *b );            // scriptgroup.cpp 0x453FD0

// 0x479690 — the ScriptGroup colour-quad gate: the entity carries the
// ScriptGroupKey AND that key isn't the ScriptColorTeamKey (the colour-team path owns
// that one).
static bool sub_479690_HasScriptGroupKey( selbrush_t *b )
{
    entity_s_def *eDef = (entity_s_def *)b->owner->def;
    const char *sgKey = g_PrefsDlg->ScriptGroupKey.c_str();
    if ( !HasKeyValuePair( eDef, sgKey ) )
        return false;
    return strcmp( sgKey, g_PrefsDlg->ScriptColorTeamKey.c_str() ) != 0;
}

// 0x73B098 — the 7 script-colour token colours (r/b/y/c/g/p/o), indexed by
// ScriptGroup_Unreachable. Shared by the camera token billboards +
// Ed_DrawScriptForceColor / Ed_DrawScriptColorQuad.
static const float kScriptTokenColors[7][4] = {
    { 1.0f, 0.0f, 0.0f, 1.0f },   // r — red
    { 0.0f, 0.0f, 1.0f, 1.0f },   // b — blue
    { 1.0f, 1.0f, 0.0f, 1.0f },   // y — yellow
    { 0.0f, 1.0f, 1.0f, 1.0f },   // c — cyan
    { 0.0f, 1.0f, 0.0f, 1.0f },   // g — green
    { 1.0f, 0.0f, 1.0f, 1.0f },   // p — purple
    { 1.0f, 0.4f, 0.0f, 1.0f },   // o — orange
};

// 0x739DC8 — the 16 ScriptGroup colour-quad colours (atol(spawnflags)&0xF).
static const float kScriptGroupQuadColors[16][4] = {
    { 1.0f, 0.0f,  0.0f, 1.0f }, { 0.0f, 0.0f,  1.0f, 1.0f },
    { 1.0f, 1.0f,  0.0f, 1.0f }, { 0.0f, 1.0f,  1.0f, 1.0f },
    { 0.0f, 1.0f,  0.0f, 1.0f }, { 1.0f, 0.0f,  1.0f, 1.0f },
    { 0.8f, 0.3f,  0.0f, 1.0f }, { 1.0f, 0.75f, 0.75f,1.0f },
    { 0.3f, 0.6f,  1.0f, 1.0f }, { 0.6f, 1.0f,  0.6f, 1.0f },
    { 0.4f, 0.0f,  0.0f, 1.0f }, { 0.0f, 0.0f,  0.5f, 1.0f },
    { 0.0f, 0.5f,  0.0f, 1.0f }, { 0.0f, 0.5f,  0.5f, 1.0f },
    { 0.5f, 0.0f,  0.5f, 1.0f }, { 0.65f,0.65f, 0.65f,1.0f },
};

// ─────────────────────────────────────────────────────────────────────────────
// 0x478E30  DrawOriginBox  (brush.cpp:3938) — the entity ORIGIN-BOX wireframe.
//
// Draws (a) three axis lines through `origin` spanning mins..maxs, (b) the 12 edges of
// the mins..maxs box, and (c) — when View→Entities-as-bounding-box is on
// (m_nEntityShowState & 0x1000) and the entity is NOT a disk trigger (eclass nShowFlags
// & 0x10) — the 12 edges of the eclass MODEL bbox (b->def mins/maxs), coloured by
// the eclass colour. Batches through R_Add3DLine → R_AddCmd_Line3D, exactly the proven
// origin-box line path (same drawer the angle arrow / connections overlays use).
//
// The m_nEntityShowState&0x1000 eclass-model-bbox gate tests eclass nShowFlags & 0x10
// (CLASS-as-bbox), reading b->def's mins / maxs; the disk-trigger skip is that same 0x10
// bit on the eclass model node (NOT the 0x80 disk bit — that one drives
// DrawTriggerRadius).  ONE GfxPointVertex[256] batch + one R_AddCmd_Line3D flush at the
// end.
// ─────────────────────────────────────────────────────────────────────────────
static void DrawOriginBox( const float *mins, const float *origin, char width,
                           const orientation_t *orient, selbrush_t *b,
                           const float *maxs, const float *lineColor )
{
    GfxColor col;
    Byte4PackPixelColor( const_cast<float *>( lineColor ), &col );
    const unsigned int *cp = (const unsigned int *)&col;

    // Function-local static line batch: the editor is single-threaded and this is
    // non-reentrant.
    static GfxPointVertex s_box[256];
    const int kLimit = 256;
    int vc = 0;

    // (a) three axis lines through `origin`, spanning the mins..maxs extent on each axis.
    float p1[3], p2[3];
    p1[0] = mins[0]; p1[1] = origin[1]; p1[2] = origin[2];
    p2[0] = maxs[0]; p2[1] = origin[1]; p2[2] = origin[2];
    vc = R_Add3DLine( s_box, orient, p1, p2, cp, width, vc, kLimit );
    p1[0] = origin[0]; p1[1] = mins[1]; p1[2] = origin[2];
    p2[0] = origin[0]; p2[1] = maxs[1]; p2[2] = origin[2];
    vc = R_Add3DLine( s_box, orient, p1, p2, cp, width, vc, kLimit );
    p1[0] = origin[0]; p1[1] = origin[1]; p1[2] = mins[2];
    p2[0] = origin[0]; p2[1] = origin[1]; p2[2] = maxs[2];
    vc = R_Add3DLine( s_box, orient, p1, p2, cp, width, vc, kLimit );

    // (b) the 12 edges of the mins..maxs box. The 8 corners:
    //   c000=mins, c100=(max.x,min.y,min.z), c010=(min.x,max.y,min.z),
    //   c001=(min.x,min.y,max.z), c110=(max.x,max.y,min.z), c101=(max.x,min.y,max.z),
    //   c011=(min.x,max.y,max.z), c111=max.
    const float c000[3] = { mins[0], mins[1], mins[2] };
    const float c100[3] = { maxs[0], mins[1], mins[2] };
    const float c010[3] = { mins[0], maxs[1], mins[2] };
    const float c001[3] = { mins[0], mins[1], maxs[2] };
    const float c110[3] = { maxs[0], maxs[1], mins[2] };
    const float c101[3] = { maxs[0], mins[1], maxs[2] };
    const float c011[3] = { mins[0], maxs[1], maxs[2] };
    const float c111[3] = { maxs[0], maxs[1], maxs[2] };
    // 12 edges in the binary's emission order:
    vc = R_Add3DLine( s_box, orient, c000, c100, cp, width, vc, kLimit );
    vc = R_Add3DLine( s_box, orient, c000, c010, cp, width, vc, kLimit );
    vc = R_Add3DLine( s_box, orient, c000, c001, cp, width, vc, kLimit );
    vc = R_Add3DLine( s_box, orient, c100, c110, cp, width, vc, kLimit );
    vc = R_Add3DLine( s_box, orient, c100, c101, cp, width, vc, kLimit );
    vc = R_Add3DLine( s_box, orient, c010, c110, cp, width, vc, kLimit );
    vc = R_Add3DLine( s_box, orient, c010, c011, cp, width, vc, kLimit );
    vc = R_Add3DLine( s_box, orient, c001, c101, cp, width, vc, kLimit );
    vc = R_Add3DLine( s_box, orient, c001, c011, cp, width, vc, kLimit );
    vc = R_Add3DLine( s_box, orient, c110, c111, cp, width, vc, kLimit );
    vc = R_Add3DLine( s_box, orient, c101, c111, cp, width, vc, kLimit );
    vc = R_Add3DLine( s_box, orient, c011, c111, cp, width, vc, kLimit );

    iassert( b->owner->def == b->def->owner );

    // (c) the eclass MODEL bbox (12 more edges) when View→Entities-as-bounding-box is on.
    if ( ( g_PrefsDlg->m_nEntityShowState & 0x1000 ) != 0 )
    {
        // gate on the eclass-model-node flag bit 0x10 — draw unless that bit is set.
        eclass_t *eclass = ( (entity_s_def *)b->owner->def )->eclass;
        if ( ( eclass->nShowFlags & 0x10 ) == 0 )
        {
            Byte4PackPixelColor( eclass->color, &col );
            // model bbox = the brush DEF's mins/maxs.
            brush_t *bdef = b->def;
            const float *mmins = bdef->mins;
            const float *mmaxs = bdef->maxs;
            const float m000[3] = { mmins[0], mmins[1], mmins[2] };
            const float m100[3] = { mmaxs[0], mmins[1], mmins[2] };
            const float m010[3] = { mmins[0], mmaxs[1], mmins[2] };
            const float m001[3] = { mmins[0], mmins[1], mmaxs[2] };
            const float m110[3] = { mmaxs[0], mmaxs[1], mmins[2] };
            const float m101[3] = { mmaxs[0], mmins[1], mmaxs[2] };
            const float m011[3] = { mmins[0], mmaxs[1], mmaxs[2] };
            const float m111[3] = { mmaxs[0], mmaxs[1], mmaxs[2] };
            vc = R_Add3DLine( s_box, orient, m000, m100, cp, width, vc, kLimit );
            vc = R_Add3DLine( s_box, orient, m000, m010, cp, width, vc, kLimit );
            vc = R_Add3DLine( s_box, orient, m000, m001, cp, width, vc, kLimit );
            vc = R_Add3DLine( s_box, orient, m100, m110, cp, width, vc, kLimit );
            vc = R_Add3DLine( s_box, orient, m100, m101, cp, width, vc, kLimit );
            vc = R_Add3DLine( s_box, orient, m010, m110, cp, width, vc, kLimit );
            vc = R_Add3DLine( s_box, orient, m010, m011, cp, width, vc, kLimit );
            vc = R_Add3DLine( s_box, orient, m001, m101, cp, width, vc, kLimit );
            vc = R_Add3DLine( s_box, orient, m001, m011, cp, width, vc, kLimit );
            vc = R_Add3DLine( s_box, orient, m110, m111, cp, width, vc, kLimit );
            vc = R_Add3DLine( s_box, orient, m101, m111, cp, width, vc, kLimit );
            vc = R_Add3DLine( s_box, orient, m011, m111, cp, width, vc, kLimit );
        }
    }

    if ( vc )
        R_AddCmd_Line3D( (short)( vc / 2 ), width, s_box );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x46AE10  Ed_DrawScriptColorQuad — a flat-colour billboard quad over an
// entity, placed just above the brush-instance bbox top. Drawn UNLIT with d_white as a
// per-vertex-coloured diamond (4 verts, 2 tris). Shared by Ed_DrawScriptForceColor
// (script_forcecolor) and CamWnd_Tokens (camera script-colour tokens; that copy lives in
// camwnd.cpp).
//   entDef — the entity DEF; reads its eclass bbox + origin.
//   color  — float[4] RGBA.
// classname strstr("node") raises the quad by +32; the quad is a diamond in
// the XY plane at the bbox top z (+0.01).  One R_AddRenderCmdDrawTris.
// ─────────────────────────────────────────────────────────────────────────────
void Ed_DrawScriptColorQuad( const entity_s *entDef, const float *color )
{
    GfxColor col;
    Byte4PackPixelColor( const_cast<float *>( color ), &col );

    // The diamond is built from the eclass bbox (mins[0..2] / maxs[0..1]) relative to the
    // entity origin.
    const entity_s_def *e = entDef;
    eclass_t     *eclass = e->eclass;
    const float minx = eclass->mins[0], miny = eclass->mins[1], minz = eclass->mins[2];
    const float maxx = eclass->maxs[0], maxy = eclass->maxs[1];
    // classname → raise the quad for "node" entities.
    const char *classname = "";
    for ( epair_t *ep = e->epairs; ep; ep = ep->next )
        if ( !_stricmp( ep->key, "classname" ) ) { classname = ep->value; break; }

    float ox = e->origin[0], oy = e->origin[1], oz = e->origin[2];
    if ( strstr( classname, "node" ) )
        oz += 32.0f;
    oz += 0.01f;
    const float qz = minz + oz;        // diamond plane z = eclass mins[2] + origin[2] (+offset)

    // diamond: 4 verts at the bbox mid-edges, in the XY plane (binary xyzw order).
    float xyzw[4][4];
    xyzw[0][0] = minx + ox;                   xyzw[0][1] = ( maxy + miny ) * 0.5f + oy; xyzw[0][2] = qz; xyzw[0][3] = 1.0f;
    xyzw[1][0] = ( minx + maxx ) * 0.5f + ox; xyzw[1][1] = maxy + oy;                   xyzw[1][2] = qz; xyzw[1][3] = 1.0f;
    xyzw[2][0] = maxx + ox;                   xyzw[2][1] = ( maxy + miny ) * 0.5f + oy; xyzw[2][2] = qz; xyzw[2][3] = 1.0f;
    xyzw[3][0] = ( minx + maxx ) * 0.5f + ox; xyzw[3][1] = miny + oy;                   xyzw[3][2] = qz; xyzw[3][3] = 1.0f;

    float normal[4][3] = { {0,0,1},{0,0,1},{0,0,1},{0,0,1} };
    float st[4][2]     = { {0,0},{0,0},{0,0},{0,0} };
    const float fcol   = *(const float *)&col.packed;
    float color4[4]    = { fcol, fcol, fcol, fcol };
    static const uint16_t indices[6] = { 3, 0, 2, 2, 0, 1 };

    if ( !g_qeglobals.d_white )
        return;
    R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT, 6, indices,
                            4, xyzw, normal, color4, st );
}

// 0x453EF0  Ed_DrawScriptForceColor — if the entity carries a
// "script_forcecolor" key, draw its colour billboard over the entity.
static void Ed_DrawScriptForceColor( selbrush_t *b )
{
    entity_s *owner = b->owner;
    if ( !owner || owner == world_entity )
        return;
    iassert( b->owner->def == b->def->owner );
    const entity_s *eDef = owner->def;
    char *fc = ValueForKey2( eDef, "script_forcecolor" );
    if ( fc && fc[0] )
    {
        int ci = ScriptGroup_Unreachable( fc );   // 0..6, or -1 on no match (defensive guard)
        if ( ci >= 0 && ci <= 6 )
            Ed_DrawScriptColorQuad( eDef, kScriptTokenColors[ci] );
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x47AA20  Brush_GetEntityLineColor  (brush.cpp:4538) — the entity LINE colour picker.
// brushDef = the entity's representative brush DEF; outRgba receives r,g,b,a; inRgba
// supplies the default alpha. worldspawn→0 (no override); "_color" epair→that colour;
// classname "actor*"→eclass.color (spawnflags!=0 forces g=0.8); classtype&0x20 +
// "node_path*" + targetname!="auto"→green. This is the SOLE definition. NULL
// owner/eclass guards are a defensive divergence on the never-run path. Non-static so
// xywnd.cpp's connections overlay and z.cpp's Z_Draw filled-column pass both reuse this
// body.
// ─────────────────────────────────────────────────────────────────────────────
char Brush_GetEntityLineColor( float *outRgba, brush_t *brushDef, const float *inRgba )
{
    if ( !brushDef ) return 0;
    entity_s *owner = brushDef->owner;
    if ( !owner ) return 0;
    eclass_t *ec = owner->eclass;
    if ( !ec || !ec->name ) return 0;

    if ( !_stricmp( ec->name, "worldspawn" ) )
        return 0;

    const char *colorString = nullptr;
    for ( epair_t *ep = owner->epairs; ep; ep = ep->next )
    {
        if ( !_stricmp( ep->key, "_color" ) )
        {
            colorString = ep->value;
            iassert( colorString );   // brush.cpp:4560
            break;
        }
    }
    if ( !colorString )
        colorString = "";                // empty string

    float r = 0.0f, g = 0.0f, b = 0.0f;
    if ( sscanf( colorString, "%f %f %f", &r, &g, &b ) == 3 )
    {
        outRgba[0] = r; outRgba[1] = g; outRgba[2] = b; outRgba[3] = inRgba[3];
        return 1;
    }

    if ( strncmp( ec->name, "actor", 5 ) != 0 )
    {
        // classtype&0x20 + node_path* + targetname!="auto" → green path line.
        if ( ( ec->classtype & 0x20 ) != 0 && !strncmp( ec->name, "node_path", 9 ) )
        {
            if ( HasKeyValuePair( (entity_s_def *)owner, "targetname" ) )
            {
                char *tn = ValueForKey2( owner, "targetname" );
                if ( strncmp( tn, "auto", 4 ) != 0 )
                {
                    outRgba[0] = 0.0f; outRgba[1] = 1.0f; outRgba[2] = 0.0f; outRgba[3] = 1.0f;
                    return 1;
                }
            }
        }
        return 0;
    }

    // actor*: eclass.color; spawnflags!=0 forces g=0.8 (keeps r/b/a from eclass).
    // Alpha is the eclass slot right after color[0..2] (eclass_t::unk), NOT a constant:
    // the binary reads it as a field in BOTH branches.
    char *sf = ValueForKey2( owner, "spawnflags" );
    bool sfZero = ( atol( sf ) == 0 );
    outRgba[0] = ec->color[0];
    if ( sfZero )
    {
        outRgba[1] = ec->color[1];
        outRgba[2] = ec->color[2];
        outRgba[3] = ec->unk;
    }
    else
    {
        outRgba[1] = 0.80000001f;
        outRgba[2] = ec->color[2];
        outRgba[3] = ec->unk;
    }
    return 1;
}

// DrawModels decoration tail (0x4796F0): the origin-box + ScriptGroup colour overlays the
// binary draws after the model mesh. Reproduces DrawModels' per-class colour/extent logic
// (CLASS_MODEL → small ±model_origin_size black box; prefab → ±prefab_origin_size,
// eclass·0.8; brush-entity → eclass mins/maxs + Brush_GetEntityLineColor), the
// ScriptGroup colour quad (the 0x479690 gate), the script_forcecolor billboard, and the
// DrawOriginBox call. The MESH itself is the separate Editor_DrawModelsPass.
static void DrawModels_Decorations( selbrush_t *b, const orientation_t *orient,
                                    char width, const GfxColor *brushCol )
{
    entity_s     *owner  = b->owner;
    entity_s_def *eDef   = (entity_s_def *)owner->def;
    eclass_t     *eclass = eDef->eclass;

    float lineCol[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    float mins[3], maxs[3];

    if ( ( eclass->classtype & 0x8 /*CLASS_MODEL*/ ) != 0 )
    {
        lineCol[0] = 0.0f; lineCol[1] = 0.0f; lineCol[2] = 1.0f; lineCol[3] = 1.0f;
        const float s = (float)(int)g_PrefsDlg->model_origin_size;
        mins[0] = mins[1] = mins[2] = -s;
        maxs[0] = maxs[1] = maxs[2] =  s;
    }
    else if ( owner->prefab )
    {
        // prefab (non-model): eclass colour ·0.8, ±prefab_origin_size box.
        // (toggle_unk05 / hidden-model recolour branch is the rare random-colour path; the
        // common branch is eclass·0.8.)
        lineCol[0] = eclass->color[0] * 0.8f;
        lineCol[1] = eclass->color[1] * 0.8f;
        lineCol[2] = eclass->color[2] * 0.8f;
        lineCol[3] = 1.0f;
        const float s = (float)(int)g_PrefsDlg->prefab_origin_size;
        mins[0] = mins[1] = mins[2] = -s;
        maxs[0] = maxs[1] = maxs[2] =  s;
    }
    else
    {
        lineCol[0] = eclass->color[0] * 0.8f;
        lineCol[1] = eclass->color[1] * 0.8f;
        lineCol[2] = eclass->color[2] * 0.8f;
        lineCol[3] = 1.0f;
        mins[0] = eclass->mins[0]; mins[1] = eclass->mins[1]; mins[2] = eclass->mins[2];
        maxs[0] = eclass->maxs[0]; maxs[1] = eclass->maxs[1]; maxs[2] = eclass->maxs[2];
        // Brush_GetEntityLineColor on the brush DEF, default alpha from lineCol[3] (==1.0).
        // Overrides lineCol in place for _color / actor / node_path.
        Brush_GetEntityLineColor( lineCol, b->def, lineCol );
    }

    float origin[3] = { eDef->origin[0], eDef->origin[1], eDef->origin[2] };
    mins[0] += origin[0]; mins[1] += origin[1]; mins[2] += origin[2];
    maxs[0] += origin[0]; maxs[1] += origin[1]; maxs[2] += origin[2];

    // ScriptGroup colour quad — when the entity has the ScriptGroupKey (and it isn't the
    // colour-team key), draw a colour quad keyed off atol(ScriptGroupKey value)&0xF.
    if ( sub_479690_HasScriptGroupKey( b ) )
    {
        const char *sgKey = g_PrefsDlg->ScriptGroupKey.c_str();
        const char *val = "";
        for ( epair_t *ep = eDef->epairs; ep; ep = ep->next )
            if ( !_stricmp( ep->key, sgKey ) ) { val = ep->value; break; }
        int idx = atol( val ) & 0xF;
        GfxColor qc;
        Byte4PackPixelColor( const_cast<float *>( kScriptGroupQuadColors[idx] ), &qc );
        const float fcol = *(const float *)&qc.packed;
        float xyzw[4][4], normal[4][3], st[4][2], color4[4];
        const float halfY = ( maxs[1] + mins[1] ) * 0.5f;
        const float halfX = ( mins[0] + maxs[0] ) * 0.5f;
        xyzw[0][0] = mins[0] - 8.0f; xyzw[0][1] = halfY;        xyzw[0][2] = mins[2]; xyzw[0][3] = 1.0f;
        xyzw[1][0] = halfX;          xyzw[1][1] = maxs[1] + 8.0f;xyzw[1][2] = mins[2]; xyzw[1][3] = 1.0f;
        xyzw[2][0] = maxs[0] + 8.0f; xyzw[2][1] = halfY;        xyzw[2][2] = mins[2]; xyzw[2][3] = 1.0f;
        xyzw[3][0] = halfX;          xyzw[3][1] = mins[1] - 8.0f;xyzw[3][2] = mins[2]; xyzw[3][3] = 1.0f;
        for ( int i = 0; i < 4; ++i ) { normal[i][0]=0; normal[i][1]=0; normal[i][2]=1; st[i][0]=0; st[i][1]=0; color4[i]=fcol; }
        static const uint16_t qidx[6] = { 3, 0, 2, 2, 0, 1 };
        if ( g_qeglobals.d_white )
            R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT, 6, qidx, 4, xyzw, normal, color4, st );
    }

    // script_forcecolor billboard — unless this brush is a script trigger.
    if ( !ScriptGroup_BrushIsTrigger( b ) )
        Ed_DrawScriptForceColor( b );

    DrawOriginBox( mins, origin, width, orient, b, maxs, lineCol );
    (void)brushCol;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x478A50  PrefabContent_IsClipBrush — "is this brush a CLIP brush?" (any face material
// named "clip*").  Used by DrawBrush_PrefabContents' draw gate to SKIP a prefab's clip-
// hull brushes when the prefab is drawn with drawFlags bit 1 set (the entity carries a
// non-zero "spawnflags"):
//   if (patch) return strncmp(Materialdef_GetName(&patch->texture), "clip", 4)==0
//   else for each of def->faceCount faces: MtlDef_IsValid(mtldef);
//        if strncmp(Materialdef_GetName(&face->mtldef[0]), "clip", 4)==0 return 1
//        (ANY face named clip -> 1)  return 0
// NOTE: the binary indexes mtldef[0], NOT current_edit_layer — the clip
// classification is layer-independent.
// ─────────────────────────────────────────────────────────────────────────────

// NOT static: KiwiWalk_DrawChildren applies the same gate and it must be the SAME function,
// not a second spelling of it.
bool PrefabContent_IsClipBrush( selbrush_t *b )
{
    brush_t *def = b->def;
    if ( def->patch )
    {
        const char *nm = (const char *)Materialdef_GetName(
            (MaterialDef *)&def->patch->texture );
        return nm && strncmp( nm, "clip", 4 ) == 0;
    }
    for ( int i = 0; i < def->faceCount; ++i )
    {
        MaterialDef *md = &def->faces[i].mtldef[0];                    // mtldef[0]
        iassert( ( ( md->lyrMtl != nullptr ) + ( md->radMtl != nullptr ) ) == 1 );  // MtlDef_IsValid
        const char *nm = (const char *)Materialdef_GetName( md );
        if ( nm && strncmp( nm, "clip", 4 ) == 0 )
            return true;
    }
    return false;
}


// The prefab layer-prefix cache.  The string is `layerPrefix ++ strlwr(model) ++ "/"`, and
// neither input changes per frame, so it is derived ONCE per (entity, parent prefix) and kept
// until a SetKeyValue or a map load.  The inline build below stays as the fallback.
// Keyed on the entity DEF pointer, with the parent prefix verified by STRCMP, not by pointer.
static unsigned s_prefabPrefixEpoch = 1;

// Called from SetKeyValue (entity.cpp) — any key set can be the "model" key, and
// a key set is a user-scale event, so the cheap conservative bump is the right one.
void KiwiPrefabPrefix_Invalidate()
{
    ++s_prefabPrefixEpoch;
}

#define KIWI_PP_SLOTS 0x8000     // 32,768 slots x 16 B = 512 KB; ~9x mp_backlot's prefabs

struct KiwiPrefixSlot
{
    const void *key;
    unsigned    epoch;
    char       *parent;
    char       *child;
};
static KiwiPrefixSlot s_prefixTable[KIWI_PP_SLOTS];

// Called from Map_NewMap.  The table must be EMPTIED, not merely aged: after a map load the
// entity DEF pointers are recycled memory, and stale slots are only reclaimed when reclaimed.
void KiwiPrefabPrefix_Shutdown()
{
    for ( int i = 0; i < KIWI_PP_SLOTS; ++i )
    {
        free( s_prefixTable[i].parent );
        free( s_prefixTable[i].child );
        s_prefixTable[i].parent = nullptr;
        s_prefixTable[i].child  = nullptr;
        s_prefixTable[i].key    = nullptr;
        s_prefixTable[i].epoch  = 0;
    }
    ++s_prefabPrefixEpoch;
}

static const char *PrefabContent_ChildPrefix( entity_s_def *eDef, const char *layerPrefix )
{
    if ( !layerPrefix )
        layerPrefix = "";
    unsigned h    = (unsigned)( (uintptr_t)eDef >> 4 ) * 2654435761u;
    int      slot = (int)( h & ( KIWI_PP_SLOTS - 1 ) );
    for ( int probe = 0; probe < 16; ++probe, slot = ( slot + 1 ) & ( KIWI_PP_SLOTS - 1 ) )
    {
        KiwiPrefixSlot *e = &s_prefixTable[slot];
        if ( e->key == eDef && e->epoch == s_prefabPrefixEpoch && e->parent && e->child
             && strcmp( e->parent, layerPrefix ) == 0 )
            return e->child;
        // A LIVE entry for a different entity: keep probing, or the chain breaks.
        if ( e->key && e->key != eDef && e->epoch == s_prefabPrefixEpoch )
            continue;
        // Free, stale, or ours-with-a-different-parent: (re)build in this slot.
        const char *model = ValueForKey2( eDef, "model" );
        if ( !model )
            model = "";
        const size_t pl = strlen( layerPrefix );
        const size_t ml = strlen( model );
        char *child  = (char *)malloc( pl + ml + 2 );
        char *parent = (char *)malloc( pl + 1 );
        if ( !child || !parent )
        {
            free( child );
            free( parent );
            return nullptr;                       // caller runs the inline build
        }
        memcpy( parent, layerPrefix, pl + 1 );
        memcpy( child, layerPrefix, pl );
        memcpy( child + pl, model, ml + 1 );
        _strlwr( child + pl );                    // on the model half only
        child[pl + ml]     = '/';
        child[pl + ml + 1] = 0;
        free( e->parent );
        free( e->child );
        e->key    = eDef;
        e->epoch  = s_prefabPrefixEpoch;
        e->parent = parent;
        e->child  = child;
        return child;
    }
    return nullptr;
}

static void DrawBrush_PrefabContents( selbrush_t *bboxBrush, entity_s_def *eDef,
                                      const orientation_t *orient, int viewType,
                                      int technique, GfxColor *col, char width,
                                      int drawFlags, const char *layerPrefix )
{
    entity_s *owner = bboxBrush->owner;
    prefab_s *pf    = (prefab_s *)owner->prefab;
    if ( !pf )
        return;

    // Layer-key prefix for the prefab's content brushes:
    // childPrefix = layerPrefix ++ strlwr(<"model" epair value>) ++ "/" (a missing key
    // reads as ""), served from the cache above; the inline build stays as the
    // out-of-memory fallback and as the readable statement of what the bytes are.
    const char *childPrefix = PrefabContent_ChildPrefix( eDef, layerPrefix );
    char childPrefixBuf[1028];
    if ( !childPrefix )
    {
        char modelLc[1024];
        strcpy( childPrefixBuf, layerPrefix );
        strcpy( modelLc, ValueForKey2( eDef, "model" ) );
        _strlwr( modelLc );
        strcat( childPrefixBuf, modelLc );
        strcat( childPrefixBuf, "/" );
        childPrefix = childPrefixBuf;
    }

    // Forward orientation of the placed prefab (Entity_GetOrientation on the entity DEF
    // through the caller's matrix).
    // A replay hands back the RECORDED orientation, and also decides who walks the content
    // list below: the recording's contiguous child array, or the prefab's linked list.
    orientation_t        prefabOrient;
    const orientation_t *useOrient = nullptr;   // set by exactly one of the two arms below
    const bool replay = KiwiWalk_PrefabEnter( bboxBrush, &useOrient );
    if ( !replay )
    {
        Entity_GetOrientation( eDef, (orientation_t *)orient, &prefabOrient );
        useOrient = &prefabOrient;
    }

    // The XY view is what tells this loop and KiwiWalk_DrawChildren "this is the XY path"
    // for FilterBrush's fast-2D-drag argument (a SEMANTIC gate, not a view test).
    // Ed_ActiveXY() IS the one XY view; `viewType < 0` is the camera.
    xywndState_t *drawXY = ( viewType >= 0 ) ? Ed_ActiveXY() : nullptr;

    // Iterate the prefab's instanced brush list (active_brushlist_next .. sentinel
    // &active_brushlist, linked via selbrush_t.next) and draw each.
    selbrush_t *sentinel = (selbrush_t *)&pf->active_brushlist;

    // The replay arm: same gate, same order, same DrawBrush — only the child SEQUENCE comes
    // out of the recording.  KiwiWalk_DrawChildren is this loop's twin: edit both or neither.
    if ( replay )
    {
        KiwiWalk_DrawChildren( useOrient, viewType, technique, col, width, drawFlags,
                               childPrefix, drawXY );
        KiwiWalk_PrefabLeave();
        return;
    }

    for ( selbrush_t *pb = pf->active_brushlist_next;
          pb && pb != sentinel;
          pb = pb->next )
    {
        // The binary's draw gate: draw UNLESS FilterBrush hides it, OR (drawFlags bit 1
        // set AND it's a clip brush).  A misc_prefab placed with a non-zero "spawnflags"
        // sets bit 1 → its clip-hull brushes are NOT drawn (without the gate the vehicle
        // prefab's clip hull drew as an opaque wc/$opaque white shell over the model).
        // NB: FilterBrush honours the per-filter isShown state, which the binary seeds
        // from GetProfileIntA("Filters",name,1) — DEFAULT 1 (SHOWN) for a fresh registry.
        // The stock CoD4Radiant.exe has NO "Filters" registry subkey, so every filter is
        // shown and all tool volumes (Hint/Portal/Clip/LightGridVolume/Caulk) render.  The
        // port reads its OWN app registry (HKCU\...\KIWI-Radiant\Filters); a session that
        // toggled filters there leaves them unchecked → those volumes hide.  That matches
        // the binary given the same registry — to reproduce the stock default, clear that
        // Filters subkey.
        // FilterBrush's 2nd arg is the fast-2D-drag flag, TRUE only while a drag is active
        // AND fast_2d_view_dragging is on AND this is the XY path.
        {
            const char fastDrag = ( g_qeglobals.toggle_unk02
                                    && g_PrefsDlg->fast_2d_view_dragging
                                    && drawXY != nullptr ) ? 1 : 0;
            if ( FilterBrush( pb, fastDrag ) )
                continue;
        }
        if ( ( drawFlags & 2 ) != 0 && PrefabContent_IsClipBrush( pb ) )
            continue;
        DrawBrush( pb, &prefabOrient, viewType, technique, col, width, drawFlags,
                   childPrefix );
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x47afc0  DrawBrush  (brush.cpp:4729) — the FULL dispatch: layer vis refresh →
// patch branch → Brush_CheckBuildFaceVis (feeds DrawGeo's filled branches)
// → fixedsize entity: DrawAngles + the DrawModels slice (Entity_HasRenderableModel →
// xmodel MESH via SkinModelInst + prefab CONTENTS via DrawBrush_PrefabContents, then
// return) → else DrawGeo on the (entity-local for fixedsize) geometry.
// Not done vs the binary: the g_PrefsDlg technique/line-width toggles.
// ─────────────────────────────────────────────────────────────────────────────
void DrawBrush( selbrush_t *b, const orientation_t *orient, int viewType,
                int technique, GfxColor *col, char width, int drawFlags,
                const char *layerPrefix )
{
    // FIRST: refresh this instance's layer-derived hide/freeze flags from the
    // layer map (lazy, g_layerCount_maybe-generation-cached).
    extern void Brush_UpdateLayerVis( selbrush_t *b, const char *layerPrefix );  // layers.cpp 0x418990
    Brush_UpdateLayerVis( b, layerPrefix );

    // Drawn unless hidden-by-layer (brushFlags bit 1) and not force-drawn (drawFlags bit 0).
    if ( ( drawFlags & 1 ) == 0 && ( b->brushFlags & 2 ) != 0 )
        return;

    // No per-brush MATERIAL_COLOR push — the binary's DrawBrush never pushes one:
    //   (1) for a SELECTED MODEL drawn at tech 29 a white MATERIAL_COLOR (w=1) would
    //       FLAT-OVERRIDE the vertcol_shaded mesh to solid white.  The model wireframe is
    //       coloured via the PER-VERTEX stamp (SkinModelInst, colorPtr=white).
    //   (2) the LINE path does not need it: DrawShadedWireframe / the wireframe builders
    //       write the brush colour into each GfxPointVertex.color, and R_AddCmd_Line3D
    //       carries that per-vertex colour to the $line UNLIT draw (RB_DrawLines3D sets tess
    //       vert color = GfxPointVertex.color, no MATERIAL_COLOR).

    // PATCH brushes — the binary's DrawBrush patch dispatch:
    //   draw_meth2 == 29 && viewType <= 2  → DrawPatchesWireframeGrid  (2D views, wireframe)
    //   draw_meth2 == 29 && viewType  > 2  → DrawPatchCameraFilled
    //   else (draw_meth2 != 29, e.g. the world draw at tech 24) → DrawPatches:
    //        the FILLED per-layer surf-cache draw (Patch_Fill_Emit →
    //        Editor_AddMeshCmd) UNLESS g_PrefsDlg->patch_wireframe is set, in which case it SKIPS
    //        the filled top-face and draws DrawPatchesWireframeGrid with colors[23].
    //
    // The 3D world pass arrives with viewType=-1 and a non-wireframe technique, so it
    // takes DrawPatches and emits through the editor surf cache.
    if ( b->patch )
    {
        if ( technique == TECHNIQUE_WIREFRAME_SHADED )
        {
            // view types 0..2 use the 2D grid; the camera uses DrawPatchCameraFilled.
            if ( (unsigned)viewType <= 2 )
            {
                // 2D views (wireframe technique): the tessellated grid, entity colour.
                DrawPatchesWireframeGrid( b->patch, col, orient, width, drawFlags );
            }
            else
            {
                // 3D camera: d_white at TECHNIQUE_WIREFRAME_SOLID, both windings.
                extern bool DrawPatchCameraFilled( patch_t *inst, const orientation_t *orient,
                                                   int drawFlags );   // pmesh.cpp 0x4415d0
                DrawPatchCameraFilled( b->patch, orient, drawFlags );
            }
        }
        else
        {
            extern bool DrawPatches( patch_t *inst, const orientation_t *orient,
                                     int techType, int drawFlags ); // pmesh.cpp 0x4414E0
            if ( !DrawPatches( b->patch, orient, technique, drawFlags ) )
            {
                // Defensive asset-gap fallback; stock assumes the instance visuals exist.
                GfxColor pcol;
                Byte4PackPixelColor( g_qeglobals.d_savedinfo.colors[23], &pcol );   // COLOR_PATCH
                DrawPatchesWireframeGrid( b->patch, &pcol, orient, width, drawFlags );
            }
        }
        return;
    }

    // Brush_CheckBuildFaceVis: (re)build this instance's per-face visuals
    // (Visuals_InitFaceVis → world-space GfxWorldVertex runs uploaded to the editor VB
    // pool) when the def version advanced.  This is what DrawGeo's FILLED branches and
    // the selection paths consume.
    // KIWI (2026-10-11, user: moving a whole building lagged): a WIREFRAME draw reads only the
    // windings (DrawGeo's tech-29 branch), yet the white selection outline and every 2D view
    // rebuilt each moved brush's GPU vertices here, per frame of a drag.  The filled passes
    // (and Brush_Ray) build them when they read them.
    if ( technique != TECHNIQUE_WIREFRAME_SHADED )
        Brush_CheckBuildFaceVis( b, (const float *)orient );

    iassert( b->owner->def == b->def->owner );

    entity_s_def *eDef   = (entity_s_def *)b->owner->def;
    eclass_t     *eclass = eDef->eclass;

    const orientation_t *useOrient = orient;
    orientation_t        localOrient;
    if ( eclass && *(int *)&eclass->fixedsize )
    {
        // Entity ANGLE ARROW overlay (View→Show→Angles, d_xyShowFlags bit 0x2 SET = hidden).
        // Gate: (drawFlags&1) || ((d_xyShowFlags&2)==0 && (classtype&2)!=0), and owner!=0.
        // `b` reaches DrawAngles via g_drawAnglesBrush.
        if ( ( ( drawFlags & 1 ) != 0
            || ( ( g_qeglobals.d_savedinfo.d_xyShowFlags & 2 ) == 0 && ( eclass->classtype & 2 ) != 0 ) )
            && b->owner )
        {
            extern selbrush_t *g_drawAnglesBrush;
            g_drawAnglesBrush = b;
            DrawAngles( viewType, orient, col );
        }
        // The DrawModels dispatch: when the entity has a renderable model or prefab
        // (Entity_HasRenderableModel = show-state filter + Model_SetModel), draw it via
        // the DrawModels slice and RETURN — the bbox below is only for entities with no
        // model.  The instance+skin runs guarded (ERR_DROP + SEH, camwnd.cpp
        // Editor_InstanceAndSkinModel) so a CoD4-format asset the CoD3 loader rejects
        // skips that model instead of crashing.  RADIANT_MODELS=0 escape hatch disables
        // the xmodel MESH; prefab classes always dispatch (their contents must keep
        // drawing).
        extern bool Editor_ModelsEnabled();                                    // camwnd.cpp
        extern int  Editor_InstanceAndSkinModel( selbrush_t *b, const orientation_t *orient,
                                                 int meshTech, GfxColor *col, int drawFlags ); // camwnd.cpp
        const bool prefabClass = ( eclass->classtype & 0x10 /* CLASS_PREFAB */ ) != 0;
        // ── KIWI-UX (ROUND AV, ITEMS 1+2) ────────────────────────────────────────────
        // The MESH takes draw_meth2, not draw_meth1.  g_drawBrushMeshTech (see its
        // definition above) carries the camera's View->Entities-as choice; -1 = "same as
        // draw_meth1", which keeps the 2D views and the selected-white-outline pass at
        // their own 29 exactly as before.
        extern int g_drawBrushMeshTech;
        const int meshTech = ( g_drawBrushMeshTech >= 0 ) ? g_drawBrushMeshTech : technique;
        if ( ( prefabClass || Editor_ModelsEnabled() )
             && !( b->def->unk01 & 0xFF )               // low byte = modelFailed
             && Editor_InstanceAndSkinModel( b, orient, meshTech, col, drawFlags ) )
        {
            // DrawModels: prefab CONTENTS.  spawnflags != 0 → drawFlags |= 2
            // (the MaterialDef_15_Drawflag_Multiply skip bit).
            if ( b->owner->prefab )
            {
                int contentFlags = drawFlags;
                // -1 = no replay open: read the epair, as before.  Only SetKeyValue can change
                // this answer, and SetKeyValue drops the recording (entity.cpp:212).
                const int spawnBit = KiwiWalk_SpawnflagsBit( b );
                const bool spawnSet = ( spawnBit >= 0 )
                                    ? ( spawnBit != 0 )
                                    : ( atol( ValueForKey2( eDef, "spawnflags" ) ) != 0 );
                if ( spawnSet )
                    contentFlags |= 2;
                // draw_meth1 role: View→Entities-as-wireframe (show-state bit 0)
                // forces the content GEOMETRY to 29; the xmodel mesh keeps `technique`
                // (the draw_meth2 role — SkinModelInst uses draw_meth2).
                int contentTech = ( g_PrefsDlg->m_nEntityShowState & 1 ) ? TECHNIQUE_WIREFRAME_SHADED : technique;
                DrawBrush_PrefabContents( b, eDef, orient, viewType, contentTech, col,
                                          width, contentFlags, layerPrefix );
            }
            // DrawModels' decoration tail (origin box / ScriptGroup quad) — default-off.
            // KIWI-UX (ROUND AV, ITEM 1): ...EXCEPT under View->Entities-as->"Skinned and
            // Boxed" (m_nEntityShowState & 0x1000).  The BOX half of that mode IS this
            // tail: DrawOriginBox's third arm (:6739) is the only code in the build gated
            // on that bit, and a model that drew RETURNS here before the placeholder bbox,
            // so with the decoration layer off the "and Boxed" half was inert.  The binary
            // reaches DrawOriginBox unconditionally from DrawModels (0x4796f0).
            extern bool Radiant_DecorEnabled();   // camwnd.cpp
            if ( Radiant_DecorEnabled() || ( g_PrefsDlg->m_nEntityShowState & 0x1000 ) != 0 )
                DrawModels_Decorations( b, orient, width, col );
            return;                              // model/prefab drawn — no bbox
        }

        // DrawModels decoration tail — the entity ORIGIN BOX + ScriptGroup colour
        // billboards.  The binary draws these inside DrawModels (reached for fixedsize
        // entities via Entity_HasRenderableModel); here they ride the FORWARD orientation
        // (`orient`, not the inverse the placeholder bbox uses below), matching the binary's
        // DrawModels which takes `ident_mtx` straight through to DrawOriginBox.  Drawn for
        // ALL fixedsize point entities (model / brush-rep / non-instanced-prefab) — the
        // mesh itself is the separate Editor_DrawModelsPass.
        // DEFAULT-OFF (RADIANT_DECOR), shared with the camwnd decoration layer — see
        // camwnd.cpp Radiant_DecorEnabled(); keeps the default render path unchanged.
        extern bool Radiant_DecorEnabled();   // camwnd.cpp
        if ( Radiant_DecorEnabled() )
            DrawModels_Decorations( b, orient, width, col );

        Entity_GetOrientationInverse( eDef, (orientation_t *)orient, &localOrient );
        useOrient = &localOrient;
    }

    DrawGeo( col, nullptr, b, useOrient, viewType, technique, width, drawFlags );
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x475cb0  Brush_MemorySize
// Returns the heap size of a brush_t def (struct + patch + faces including each face's
// winding).
// ─────────────────────────────────────────────────────────────────────────────
// The first _msize is def->patch, NOT parent_layer_string.
size_t Brush_MemorySize( brush_t *def )
{
    size_t total = sizeof(brush_t);
    if ( def->patch )
        total += _msize( def->patch );
    if ( def->faceCount )
    {
        for ( int i = 0; i < def->faceCount; ++i )
        {
            face_t *f = &def->faces[i];
            void *faceStr = f->w;
            if ( faceStr )
                total += _msize( faceStr ) + sizeof(face_t);
            else
                total += sizeof(face_t);
        }
    }
    return total;
}

// ─────────────────────────────────────────────────────────────────────────────
// 0x475e80  Brush_FullClone
// Deep clone of a brush DEF.  KIWI: the IDB typed the argument
// entity_brush_s* (an instance) and indexed it in 56-byte steps; it always was a def.
// ─────────────────────────────────────────────────────────────────────────────
brush_t *Brush_FullClone( brush_t *src )
{
    // The argument IS the def, so src->patch directly.
    patchMesh_t *pPatch = (patchMesh_t *)(intptr_t)src->patch;

    if ( pPatch )
    {
        patchMesh_t *dup = Patch_Duplicate( pPatch );
        Entity_UnlinkBrush( (brush_t_with_custom_def *)dup->pSymbiot );
        brush_t_with_custom_def *ps = (brush_t_with_custom_def *)dup->pSymbiot;
        ps->owner = (entity_s *)(intptr_t)src->owner;
        Brush_BuildWindings( (brush_t *)ps, 1 );
        if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
            SetupVertexSelection();
        MarkMapModified();
        ++ps->version;
        return ps;
    }
    else
    {
        brush_t_with_custom_def *b = (brush_t_with_custom_def *)::operator new( sizeof(brush_t_with_custom_def) );
        memset( b, 0, sizeof(brush_t_with_custom_def) );
        iassert( b );

        // parent_layer_string: the freshly zeroed alloc has none, so no free is needed.
        // Copy from g_activeLayer_string (same as Brush_Clone)
        size_t layerLen = strlen( g_activeLayer_string );
        void *layerCopy = ::operator new( layerLen + 1 );
        memcpy( layerCopy, g_activeLayer_string, layerLen + 1 );
        b->parent_layer_string = (char *)layerCopy;

        b->owner = (entity_s *)(intptr_t)src->owner;

        // Copies numberId, NOT version: the clone keeps version 0 from the memset.
        b->numberId  = src->numberId;
        b->faceCount = src->faceCount;

        b->mins[0] = src->mins[0];
        b->mins[1] = src->mins[1];
        b->mins[2] = src->mins[2];
        b->maxs[0] = src->maxs[0];
        b->maxs[1] = src->maxs[1];
        b->maxs[2] = src->maxs[2];

        // Allocate + deep-clone faces
        b->faces = Face_Alloc_R( src->faceCount );
        for ( int i = 0; i < src->faceCount; ++i )
        {
            Face_FullClone( &src->faces[i], &b->faces[i] );
        }

        return b;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  Texture lock that follows the face's 3 plane points - the binary's Face_TexLock_Save /
//  Face_TexLock_Reproject (0x470570 / 0x4706F0), now used by KIWI Match Face only: S/T at
//  the plane points before they move, then the 3x3 LU solve (linearmapping.cpp) over the
//  moved points recovers each world texture row.  KIWI: in double and decomposed by
//  texturevecs_02_exact - the float pair's 0.001 / 0.005 deg snaps moved real texdefs.
// ═════════════════════════════════════════════════════════════════════════════

// KIWI: the layers every texture-lock pass walks - one cap for all of them.
static int TexLock_Layers( const MaterialDef *mtl )
{
    const int layers = MaterialDef_04( (MaterialDef *)mtl );
    iassert( layers <= KTEXLOCK_LAYERS );
    return layers < KTEXLOCK_LAYERS ? layers : KTEXLOCK_LAYERS;
}

// 0x470570: S/T at planepts[0..2] for every channel / layer.
void Face_TexLockPointsSave( face_t *face, double ( *save )[KTEXLOCK_LAYERS][6] )
{
    for ( int ch = 0; ch < 3; ++ch )
    {
        MaterialDef *mtl = &face->mtldef[ch];
        const int layers = TexLock_Layers( mtl );
        for ( int L = 0; L < layers; ++L )
        {
            const texdef_sub_t *td = &mtl->mat_texDef + L;
            float tm[8];
            Face_MoveTexture( td->size, face->plane.normal, tm, td->shift, td->rotate, td->crossterm );
            for ( int p = 0; p < 3; ++p )
            {
                const float *pt = face->planepts[p];
                save[ch][L][2 * p]     = (double)tm[0] * pt[0] + (double)tm[1] * pt[1] + (double)tm[2] * pt[2] + tm[3];
                save[ch][L][2 * p + 1] = (double)tm[4] * pt[0] + (double)tm[5] * pt[1] + (double)tm[6] * pt[2] + tm[7];
            }
        }
    }
}

// 0x4706F0: Face_MakePlane, then per locked channel / layer solve the S and T rows over
// the moved plane points (Setup's (normal, p2, p0, p1) order) and decompose them.
void Face_TexLockPointsReproject( face_t *face, double ( *save )[KTEXLOCK_LAYERS][6],
                                  const byte *lockFlags )
{
    Face_MakePlane( face );

    LinearMapping lm;
    if ( !LinearMapping_Setup( &lm, face->plane.normal,
                               face->planepts[2], face->planepts[0], face->planepts[1] ) )
        return;

    for ( int ch = 0; ch < 3; ++ch )
    {
        if ( !lockFlags[ch] )
            continue;
        MaterialDef *mtl = &face->mtldef[ch];
        const int layers = TexLock_Layers( mtl );
        for ( int L = 0; L < layers; ++L )
        {
            const double *sv = save[ch][L];
            double texMat[8];
            LinearMapping_Apply( &lm, texMat,     sv[0], sv[2], sv[4] );   // S at p0, p1, p2
            LinearMapping_Apply( &lm, texMat + 4, sv[1], sv[3], sv[5] );   // T
            texdef_sub_t *td = &mtl->mat_texDef + L;
            texturevecs_02_exact( td->size, texMat, face->plane.normal, face->plane.dist,
                                  td->shift, &td->rotate, &td->crossterm );
        }
    }
}

// KIWI-UX (RADIANT_UX_DESIGN §23, Phase 4): public forwarder for the IN-PLACE
// face-array resize the ported primitive Brush_MakeSided (0x4731E0) performs, so
// region extrusion (kiwi_extrude.cpp) can build an N+2-faced prism.
//
// WHY IT IS NEEDED: Brush_Alloc always allocates SIX faces (eight for a
// classtype-bit-0 eclass) and Brush_Create writes a box into them.  A prism over
// an n-gon needs n + 2 faces, which for a triangle is FIVE — Face_Alloc can only
// GROW the array, so there is no way to reach it through the existing exports.
//
// The body is Brush_MakeSided's own sequence, lifted verbatim from 0x4731E0's
// re-allocation prologue (brush.cpp:3413-3420 + its per-face MaterialDef stamp
// and the Brush_SetDefaultMaterials tail).  Nothing new is invented; the ported
// primitive's face-array swap is simply made reachable.  The caller writes the
// planepts afterwards and rebuilds.
void Ed_BrushSetFaceCount( brush_t *def, int faceCount )
{
    iassert( def );
    if ( !def || faceCount < 4 || faceCount >= 1020 )   // 1020 = MAX_POINTS_ON_WINDING - 4
        return;

    if ( def->faces )
        Face_Free( def->faceCount, def->faces );
    def->faceCount = faceCount;
    def->faces     = Face_Alloc_R( faceCount );

    GfxColor white;
    Byte4PackPixelColor( const_cast<float *>( colorWhite ), &white );
    for ( int i = 0; i < faceCount; ++i )
    {
        face_t *f = &def->faces[i];
        memcpy( f->mtldef, &g_qeglobals.random_texture_stuff[0].mtl, sizeof( MaterialDef ) );
        f->field_0xE4 = white.packed;
    }
    Brush_SetDefaultMaterials( def );
}


// ─────────────────────────────────────────────────────────────────────────────
//  0x47ba40  Brush_Move  (the leaf brush-translate)
//
//  Translates one brush definition by `move` (vec3) and rebuilds it. Called for
//  every selected brush by Select_Move (select.cpp). Returns the owning entity.
//
//  Texture/lightmap LOCK: the binary builds a 3-byte lockFlags { texLock=
//  m_bTextureLock!=0, lightmapLock=m_bLightmapLock!=0, const1=1 } ONCE, then per face
//  that has a winding (face->w):
//      Face_TexLock_Save(saveBuf, face)        — snapshot the WORLD S/T
//                                                at the 3 plane points (pre-move)
//      planepts[0..2] += move                  — the actual translate (inline)
//      Face_TexLock_Reproject(face, saveBuf, &lockFlags)
//                                    — Face_MakePlane(face) + a per-channel texture
//                                      reproject GATED on the lock bytes AND
//                                      MaterialDef_04(layer) > 0, via the LU solver.
//  Brush_BuildWindings (called right after, as in the binary) recomputes every face
//  plane + winding — so the per-face Face_MakePlane inside Reproject is redundant
//  for geometry; geometry is unaffected, only the texdef-lock behaviour is added.
//  Default m_bTextureLock=1, so brush moves reproject the texture by default
//  (as the binary); .map texdefs change on move iff the lock is on.
//
//  KIWI (float round trip): the Save / Reproject pair is replaced by Face_TexLockTranslate,
//  the same lock done exactly.  The LU re-solve (float S/T in the hundreds, then
//  texturevecs_02's snapping) turned / sheared the texture slightly on every nudge, array
//  copy or paste-place far from the origin, and there-and-back did not return.
// ─────────────────────────────────────────────────────────────────────────────
extern curvePatchDef_t *Patch_Move( const float *move, patchMesh_t *p );   // 0x441dd0 (pmesh.cpp)

// KIWI: texture lock for a pure translation - shift by size x (row . d) per locked channel /
// layer (Face_TexLock_Save's walk); size / rotate / crossterm untouched.  Call it BEFORE the
// planepts move (the normal it projects with is the face's own).
void Face_TexLockTranslate( face_t *f, const float *d, const byte *lockFlags )
{
    for ( int ch = 0; ch < 3; ++ch )
    {
        if ( !lockFlags[ch] )
            continue;
        MaterialDef *mtl = &f->mtldef[ch];
        const int layers = TexLock_Layers( mtl );
        for ( int L = 0; L < layers; ++L )
        {
            texdef_sub_t *td = &mtl->mat_texDef + L;    // the contiguous layer walk
            float tm[8];
            Face_MoveTexture( td->size, f->plane.normal, tm, td->shift, td->rotate, td->crossterm );
            const float sx = td->size[0] != 0.0f ? td->size[0] : 128.0f;   // Face_MoveTexture's default
            const float sy = td->size[1] != 0.0f ? td->size[1] : 128.0f;
            td->shift[0] += sx * ( tm[0] * d[0] + tm[1] * d[1] + tm[2] * d[2] );
            td->shift[1] += sy * ( tm[4] * d[0] + tm[5] * d[1] + tm[6] * d[2] );
        }
    }
}

// KIWI (2026-10-05, IDA 0x47CDE0): the rotate / flip texture lock Select_ApplyMatrix applies
// (the port had dropped it as a no-op), with the rows turned in double.  For x' = o + (x - o) T,
// S'(x') = S(x) gives row' = row T and offset' = offset + row.o - row'.o; texturevecs_02_exact
// (the binary's decomposition in double, snapping only float noise) then writes the texdef
// against the moved face - not Save / Reproject's float LU solve from S/T at the plane
// points, which tilted textures far from the origin (Face_TexLockTranslate's reason).
// Begin: BEFORE the planepts move, with the face's pre-move normal.  End: after they moved.
void Face_TexLockAffineBegin( face_t *f, const float *normal, const double ( *turn )[3],
                              const double *origin, const byte *lockFlags, double ( *rows )[KTEXLOCK_LAYERS][8] )
{
    for ( int ch = 0; ch < 3; ++ch )
    {
        if ( !lockFlags[ch] )
            continue;
        MaterialDef *mtl = &f->mtldef[ch];
        const int layers = TexLock_Layers( mtl );
        for ( int L = 0; L < layers; ++L )
        {
            const texdef_sub_t *td = &mtl->mat_texDef + L;
            float tm[8];
            Face_MoveTexture( td->size, normal, tm, td->shift, td->rotate, td->crossterm );
            for ( int row = 0; row < 2; ++row )
            {
                const float *r   = tm + row * 4;
                double      *out = rows[ch][L] + row * 4;
                double ro = 0.0, rno = 0.0;
                for ( int j = 0; j < 3; ++j )
                {
                    out[j] = r[0] * turn[0][j] + r[1] * turn[1][j] + r[2] * turn[2][j];
                    ro  += r[j] * origin[j];
                    rno += out[j] * origin[j];
                }
                out[3] = r[3] + ro - rno;
            }
        }
    }
}

void Face_TexLockAffineEnd( face_t *f, double ( *rows )[KTEXLOCK_LAYERS][8], const byte *lockFlags )
{
    Face_MakePlane( f );
    for ( int ch = 0; ch < 3; ++ch )
    {
        if ( !lockFlags[ch] )
            continue;
        MaterialDef *mtl = &f->mtldef[ch];
        const int layers = TexLock_Layers( mtl );
        for ( int L = 0; L < layers; ++L )
        {
            texdef_sub_t *td = &mtl->mat_texDef + L;
            texturevecs_02_exact( td->size, rows[ch][L], f->plane.normal, f->plane.dist,
                                  td->shift, &td->rotate, &td->crossterm );
        }
    }
}

entity_s *Brush_Move( const float *move, brush_t *def, char snap )
{
    // Texture/lightmap lock flags {texLock, lightmapLock, 1}, built once before the face
    // loop, as the binary.
    byte lockFlags[3];
    lockFlags[0] = (byte)( g_PrefsDlg->m_bTextureLock  != 0 );   // texLock
    lockFlags[1] = (byte)( g_PrefsDlg->m_bLightmapLock != 0 );   // lightmapLock
    lockFlags[2] = 1;                                                     // const1

    // Translate every face.  The binary gates on face->w and wraps it in
    // Face_TexLock_Save / Face_TexLock_Reproject.  KIWI: every face
    // (a windingless one stayed behind and could cut the brush at its new spot; KIWI G
    // moves them all), locked exactly (Face_TexLockTranslate above).
    for ( int i = 0; i < def->faceCount; ++i )
    {
        face_t *f = &def->faces[i];

        Face_TexLockTranslate( f, move, lockFlags );

        for ( int p = 0; p < 3; ++p )
        {
            f->planepts[p][0] += move[0];
            f->planepts[p][1] += move[1];
            f->planepts[p][2] += move[2];
        }
    }

    Brush_BuildWindings( def, snap );          // recomputes planes (Face_MakePlane) + windings
    if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++def->version;

    if ( def->patch )
        Patch_Move( move, def->patch );

    entity_s *ent = def->owner;
    if ( ent->eclass && ent->eclass->fixedsize )
    {
        ent->origin[0] += move[0];
        ent->origin[1] += move[1];
        ent->origin[2] += move[2];
        // the entity's 16-bit render-version counter.
        ent->version_prob_wrong = (ent->version_prob_wrong & ~0xFFFF)
                                | ((ent->version_prob_wrong + 1u) & 0xFFFF);
    }
    return ent;
}

// ── Brush_SnapToGrid (0x4783D0) — snap a brush's geometry to the grid ─────────
//   Reached by Patch_SnapVertToGrid (pmesh.cpp) for the non-patch brushes of a mixed
//   selection.  For a fixedsize entity, snaps the owner's ORIGIN and moves the brush by
//   the delta (Brush_Move).  Otherwise clones the brush, snaps every face's plane points
//   to the grid, rebuilds; if the snapped brush collapsed below 1 unit on any axis, swaps
//   the original faces back and warns.
extern brush_t *Brush_Clone( brush_t *def );                       // 0x475D20
extern void     Brush_RemoveWindinglessFaces( brush_t *b );            // 0x471720
void Brush_SnapToGrid( brush_t *b )
{
    Brush_RemoveWindinglessFaces( b );
    if ( b->faceCount < 4 )
    {
        Error( "Brush_SnapToGrid: Removing brush with no faces\n" );
        Brush_Free_R( b );
        return;
    }

    entity_s *owner = b->owner;
    if ( owner && owner->eclass && owner->eclass->fixedsize )
    {
        // snap the owner's origin, move the brush by the resulting delta.
        float snapped[4];
        for ( int k = 0; k < 3; ++k )
            snapped[k + 1] = (float)floor( owner->origin[k] / grid_sizes[g_qeglobals.d_gridsize] + 0.5 )
                           * grid_sizes[g_qeglobals.d_gridsize];
        float move_delta[3];
        move_delta[0] = snapped[1] - owner->origin[0];
        move_delta[1] = snapped[2] - owner->origin[1];
        move_delta[2] = snapped[3] - owner->origin[2];
        Brush_Move( move_delta, b, 1 );
        return;
    }

    // non-fixedsize: clone (to restore if the snap collapses the brush), snap each
    // face's 3 plane points to the grid, rebuild.
    brush_t *clone = Brush_Clone( b );
    for ( int fi = 0; fi < b->faceCount; ++fi )
    {
        face_t *f = &b->faces[fi];
        winding_t *w = f->w;
        for ( int p = 0; p < 3; ++p )
            for ( int k = 0; k < 3; ++k )
                f->planepts[p][k] = (float)floor( w->p[p][k] / grid_sizes[g_qeglobals.d_gridsize] + 0.5 )
                                  * grid_sizes[g_qeglobals.d_gridsize];
    }
    Brush_BuildWindings( b, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++b->version;

    // reject the snap if any axis of the resulting bbox is < 1 unit.
    int okAxes = 0;
    for ( int k = 0; k < 3; ++k )
    {
        if ( b->maxs[k] - b->mins[k] < 1.0f )
            break;
        ++okAxes;
    }
    if ( okAxes >= 3 )
    {
        Brush_Free_R( clone );
        return;
    }

    Sys_Printf( "WARNING: Snapped brush is smaller than 1 unit, restoring original brush\n" );
    face_t *tmp        = b->faces;
    b->faces           = clone->faces;
    clone->faces       = tmp;
    Brush_BuildWindings( b, 1 );
    if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++b->version;
    Brush_Free_R( clone );
}

// ═════════════════════════════════════════════════════════════════════════════
//  Functions whose embedded asserts name cod3src\Radiant\brush.cpp, i.e. brush.cpp
//  is their original source file, in the order of the brush.cpp line their asserts
//  cite.  Functions elsewhere that carry a brush.cpp assert from an INLINED brush.cpp
//  helper stay where they are — the assert rode in with the inlined callee and
//  does not mean the enclosing function came from brush.cpp.
// ═════════════════════════════════════════════════════════════════════════════

// The maya object counters are defined in mayaexport.cpp; ExportTo3D below bumps them
// for the progress readout.
extern int s_mayaObjCount;
extern int s_mayaObjTotal;

// Callees of the bodies below that live in other TUs.
extern void Patch_ApplyMatrix( const orientation_t *orient, patchMesh_t *p, char snap );  // pmesh.cpp 0x441E70

// ── brush.cpp:241 ──
// ════════════════════════════════════════════════════════════════════════════
//  0x46FDA0  ExportTo3D_CreatePolyFacet — emit ONE brush face winding as a
//  `polyCreateFacet -p x y z ...` (points REVERSED) + optional per-vertex polyEditUV.
//
//  `def` is the brush DEF face_t*, `face` the per-instance faceVis_s*.  Returns 1 if a
//  facet was emitted, 0 if skipped (no winding, or filtered).
// ════════════════════════════════════════════════════════════════════════════
static char ExportTo3D_CreatePolyFacet( faceVis_s *face, face_t *def,
                                        const orientation_t *orient, FILE *f,
                                        char emitUVs, float scale )
{
    iassert( face );   // brush.cpp:241
    iassert( def );    // brush.cpp:242

    winding_t *w = def->w;
    if ( !w )
        return 0;

    // world-space face normal (scratch; the binary fills per-vertex normal arrays it
    // never emits — only the positions + UVs reach the .mel).
    float worldNormal[3];
    OrientationDirToWorldDir( worldNormal, orient, def->plane.normal );

    // texdef + texture matrix (layer 0 — the export base layer; layerHandle=0 as in the binary).
    MaterialDef *mtldef = &def->mtldef[0];
    texdef_sub_t *texdef = MaterialDef_FindTexdef( mtldef, 0 );

    // Headless-vs-GUI material-realize invariant: the binary derefs `texdef`
    // UNCONDITIONALLY (Face_MoveTexture + the texCoord matrix), relying on the GUI's
    // material-realize pass having populated the layer (so texdef != 0).  A LAYERED
    // material whose `lyrMtl->layerCount == 0` (not realized) makes
    // MaterialDef_FindTexdef return 0; the binary would then deref a NULL texdef
    // (it never does in the GUI).  We GUARD it: with no realized layer, emit the GEOMETRY
    // (the whole point of the export — positions always exist) with identity UVs and skip
    // the colour probe (Face_GetLayerColor → MaterialDef_14 would also assert on the empty layer).
    bool haveTex = ( texdef != 0 );
    float texMat[8];
    if ( haveTex )
    {
        texdef_sub_t *tdp = (texdef_sub_t *)(intptr_t)texdef;
        Face_MoveTexture( texdef->size, def->plane.normal, texMat, texdef->shift,
                          tdp->rotate, tdp->crossterm );
    }
    else
    {
        texMat[0] = texMat[1] = texMat[2] = texMat[3] = 0.0f;
        texMat[4] = texMat[5] = texMat[6] = texMat[7] = 0.0f;
    }

    iassert( w->ptCount <= MAX_POINTS_ON_WINDING );   // brush.cpp:250

    if ( MtlDef_IsFaceFiltered( mtldef ) )
        return 0;

    fprintf( f, "\t\t$strPolyInfo = `polyCreateFacet -ch off -tx 1 -s 1" );

    if ( haveTex )                        // colour probe also needs a realized layer (MaterialDef_14)
    {
        unsigned int packedColor = 0;    // Face_GetLayerColor result (unused by the -p emit)
        Face_GetLayerColor( mtldef, def, 0, (int *)&packedColor );
    }

    // texcoords are buffered then emitted in the SAME reverse order as the -p list.
    float texCoord[1024][2];

    // Emit points in REVERSE (numpoints-1 .. 0), matching the binary's facet winding.
    for ( int ptIndex = w->numpoints - 1; ptIndex >= 0; --ptIndex )
    {
        const float *p = w->p[ptIndex];        // x=p[0] y=p[1] z=p[2]

        texCoord[ptIndex][0] = texMat[0] * p[0] + texMat[1] * p[1] + texMat[2] * p[2] + texMat[3];
        texCoord[ptIndex][1] = texMat[4] * p[0] + texMat[5] * p[1] + texMat[6] * p[2] + texMat[7];

        iassert( !IS_NAN(texCoord[ptIndex][0]) );   // brush.cpp:265
        iassert( !IS_NAN(texCoord[ptIndex][1]) );   // brush.cpp:266

        fprintf( f, " -p %f %f %f", scale * p[0], p[1] * scale, scale * p[2] );
    }

    fprintf( f, "`;\r\n" );

    if ( emitUVs )
    {
        for ( int ptIndex = w->numpoints - 1; ptIndex >= 0; --ptIndex )
            fprintf( f, "\t\tpolyEditUV -r false -u %f -v %f ($strPolyInfo[0]).map[%d];\r\n",
                     texCoord[ptIndex][0], texCoord[ptIndex][1], ptIndex );
    }
    return 1;
}

// ── brush.cpp:2167 ──
// ─────────────────────────────────────────────────────────────────────────────
// 0x475110  Brush_ApplyEntityColor  (brush.cpp:2167 assert)
// Sets the GfxColor of all faces in a brush from the owning entity's _color or eclass color.
// Called after Entity_LinkBrush to paint the brush in the editor.
// ─────────────────────────────────────────────────────────────────────────────
// _color / eclass-color / default branches, Byte4PackPixelColor(src,dest), the face-color
// loop, then a 16-bit version ++.
unsigned int Brush_ApplyEntityColor( brush_t *b )
{
    iassert( b );   // brush.cpp:2167

    entity_s_def *entDef = (entity_s_def *)b->owner;

    // Entity_GetVec3ForKey writes rgba[0..2].  ONE float[4], never three loose locals:
    // MSVC may reorder separate locals and the write runs off.
    float rgba[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    eclass_t *eclass = nullptr;
    if ( entDef && ( eclass = entDef->eclass ) != nullptr && *(int *)&eclass->fixedsize )
    {
        if ( HasKeyValuePair( entDef, "_color" ) )
        {
            Entity_GetVec3ForKey( entDef, rgba, "_color" );
        }
        else
        {
            rgba[0] = eclass->color[0];
            rgba[1] = eclass->color[1];
            rgba[2] = eclass->color[2];
            rgba[3] = eclass->unk;
        }
    }

    GfxColor color;
    Byte4PackPixelColor( rgba, &color );

    unsigned int result = 0;
    int faceCount = b->faceCount;
    if ( faceCount )
    {
        do
        {
            memcpy( &b->faces[result].packedColor, &color, sizeof(GfxColor) );
            ++result;
        }
        while ( result < (unsigned)faceCount );
    }
    ++b->version;
    return result;
}

// ── brush.cpp:3499 ──
// ════════════════════════════════════════════════════════════════════════════
//  0x477E00  ExportTo3D — emit ONE brush as a `{ ... }` MEL block: a polyCreateFacet
//  per (filtered-in) face, then polyUnite/polyMergeVertex/parent.  Point (fixedsize)
//  entities additionally get four small marker facets + move/rotate from origin/angles.
// ════════════════════════════════════════════════════════════════════════════
char ExportTo3D( selbrush_t *b, const orientation_t *orient, FILE *f,
                        char groupAsBrush, char emitUVs, float scale )
{
    iassert( b );   // brush.cpp:3499
    iassert( b->def );   // brush.cpp:3500

    char filtered = FilterBrush( b, 0 );
    if ( filtered )
        return filtered;                 // filtered-out brush -> emit nothing (returns nonzero)

    // sync the per-instance faceVis array to the def's face count (rebuild if stale).
    if ( b->faceCount != b->def->faceCount )
    {
        if ( b->faces )
            Vis_Free( b->faceCount, b->faces, b );
        b->faceCount = b->def->faceCount;
        b->faces     = (faceVis_s *)PlanePts_Alloc( b->faceCount );
    }

    fprintf( f, "\t{\r\n" );
    fprintf( f, "\t\tstring $strPolyInfo[];\r\n" );
    if ( groupAsBrush )
    {
        fprintf( f, "\t\tstring $strPolyList[];\r\n" );
        fprintf( f, "\t\tstring $strBrush[];\r\n\r\n" );
    }

    face_t    *defFaces = b->def->faces;
    faceVis_s *visFaces = (faceVis_s *)b->faces;
    int        polyIdx  = 0;
    for ( int i = 0; (unsigned)i < (unsigned)b->faceCount; ++i )
    {
        if ( ExportTo3D_CreatePolyFacet( &visFaces[i], &defFaces[i], orient, f, emitUVs, scale ) )
        {
            if ( groupAsBrush )
                fprintf( f, "\t\t$strPolyList[%d] = $strPolyInfo[0];\r\n", polyIdx );
            ++polyIdx;
        }
    }

    entity_s_def *ownerDef = (entity_s_def *)b->owner->def;   // owner entity def
    if ( *(int *)&ownerDef->eclass->fixedsize )       // point entity: emit 4 marker facets
    {
        const float *o = ownerDef->origin;
        // +X / +Z(8) / -Y(8) marker
        fprintf( f, "\t\t$strPolyInfo = `polyCreateFacet -ch off -tx 1 -s 1" );
        fprintf( f, " -p %f %f %f", o[0] + 32.0, o[1],       o[2] );
        fprintf( f, " -p %f %f %f", o[0],        o[1],       o[2] + 8.0 );
        fprintf( f, " -p %f %f %f", o[0],        o[1] - 8.0, o[2] );
        fprintf( f, "`;\r\n" );
        fprintf( f, "\t$strPolyList[size($strPolyList)] = $strPolyInfo[0];\r\n" );
        // +X / +Y(8) / +Z(8) marker
        fprintf( f, "\t\t$strPolyInfo = `polyCreateFacet -ch off -tx 1 -s 1" );
        fprintf( f, " -p %f %f %f", o[0] + 32.0, o[1],       o[2] );
        fprintf( f, " -p %f %f %f", o[0],        o[1] + 8.0, o[2] );
        fprintf( f, " -p %f %f %f", o[0],        o[1],       o[2] + 8.0 );
        fprintf( f, "`;\r\n" );
        fprintf( f, "\t$strPolyList[size($strPolyList)] = $strPolyInfo[0];\r\n" );
        // +X / -Z(8) / +Y(8) marker
        fprintf( f, "\t\t$strPolyInfo = `polyCreateFacet -ch off -tx 1 -s 1" );
        fprintf( f, " -p %f %f %f", o[0] + 32.0, o[1],       o[2] );
        fprintf( f, " -p %f %f %f", o[0],        o[1],       o[2] - 8.0 );
        fprintf( f, " -p %f %f %f", o[0],        o[1] + 8.0, o[2] );
        fprintf( f, "`;\r\n" );
        fprintf( f, "\t$strPolyList[size($strPolyList)] = $strPolyInfo[0];\r\n" );
        // +X / -Y(8) / -Z(8) marker
        fprintf( f, "\t\t$strPolyInfo = `polyCreateFacet -ch off -tx 1 -s 1" );
        fprintf( f, " -p %f %f %f", o[0] + 32.0, o[1],       o[2] );
        fprintf( f, " -p %f %f %f", o[0],        o[1] - 8.0, o[2] );
        fprintf( f, " -p %f %f %f", o[0],        o[1],       o[2] - 8.0 );
        fprintf( f, "`;\r\n" );
        fprintf( f, "\t$strPolyList[size($strPolyList)] = $strPolyInfo[0];\r\n" );
    }

    if ( groupAsBrush )
    {
        fprintf( f, "\t\tif (size ($strPolyList) > 1)\r\n" );
        fprintf( f, "\t\t\t$strBrush = `polyUnite -ch 0 ($strPolyList)`;\r\n" );
        fprintf( f, "\t\telse\r\n" );
        fprintf( f, "\t\t\t$strBrush[0] = $strPolyList[0];\r\n" );
        fprintf( f, "\t\tpolyMergeVertex -d 0.01 -ch 0 $strBrush[0];\r\n" );
        fprintf( f, "\t\tparent $strBrush[0] $strGroups[$iCurGroup];\r\n" );

        if ( *(int *)&ownerDef->eclass->fixedsize )
        {
            float ang[3];
            if ( !Entity_GetVec3ForKey( ownerDef, ang, "angles" ) )
            {
                ang[0] = 0.0f; ang[1] = 0.0f; ang[2] = 0.0f;
            }
            const float *o = ownerDef->origin;
            fprintf( f, "\t\tmove -x %f -y %f -z %f (($strBrush[0]) + \".scalePivot\") "
                        "(($strBrush[0]) + \".rotatePivot\");\r\n", o[0], o[1], o[2] );
            if ( HasKeyValuePair( ownerDef, "angles" ) )
                fprintf( f, "\t\trotate -os %f %f %f ($strBrush[0]);\r\n", ang[2], ang[0], ang[1] );
        }
    }
    else
    {
        fprintf( f, "\t\tparent $strPolyList $strGroups[$iCurGroup];\r\n" );
    }

    ++s_mayaObjCount;
    fprintf( f, "\t}\r\n" );
    fprintf( f, "\tprogressWindow -edit -s 1;\r\n" );
    return (char)fprintf( f, "\tprogressWindow -edit -st \"%d of %d objects\";\r\n",
                          s_mayaObjCount, s_mayaObjTotal );
}

// ── brush.cpp:4293 ──
// 0x479FF0  ScriptGroup_AssignNextNumber — assign the next free group NUMBER (under
// g_PrefsDlg->ScriptGroupKey) to every selected entity:
//   * guard: only run when ScriptGroupKey != ScriptColorTeamKey (the colour-team key
//     uses the colour-token path instead; a vehicle/group key is always != the team key)
//   * pass 1: walk active_brushes, max = -1; for each non-world entity carrying the key,
//     atol(value) and keep the max (skip empty values)
//   * pass 2: itoa(max+1) and SetKeyValue it onto every selected non-world entity
// (The asserts carry the binary's literal brush.cpp file/line — AssignNextNumber was
//  inlined from a brush.cpp context; type 0 → log+continue, type 1 → would-be-fatal but
//  ValueForKey2 returns the "" global, never null, so the *v guard handles it.)
void ScriptGroup_AssignNextNumber()
{
    const char *groupKey = g_PrefsDlg->ScriptGroupKey.c_str();
    if ( !strcmp( groupKey, g_PrefsDlg->ScriptColorTeamKey.c_str() ) )
        return;   // ScriptGroupKey == ScriptColorTeamKey → the colour-token path owns it.

    // PASS 1 — find the highest existing number under groupKey across active brushes.
    int maxNum = -1;
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        entity_s_def *def = (entity_s_def *)owner->def;
        // b->def->owner is dereferenced UNCONDITIONALLY (no `b->def &&` guard), as in the
        // sibling iterators.
        iassert( b->owner->def == b->def->owner );   // brush.cpp:4293

        if ( !HasKeyValuePair( def, groupKey ) )
            continue;
        const char *group = ValueForKey2( def, groupKey );   // the binary's local
        iassert( group );   // brush.cpp:4298
        if ( group && *group )
        {
            int n = atol( group );
            if ( maxNum < n )
                maxNum = n;
        }
    }

    // PASS 2 — assign maxNum+1 to every selected non-world entity.
    char next[16];
    _itoa( maxNum + 1, next, 10 );
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
    {
        entity_s *owner = b->owner;
        if ( !owner || owner == world_entity )
            continue;
        entity_s_def *def = (entity_s_def *)owner->def;
        // b->def->owner is dereferenced UNCONDITIONALLY.
        iassert( b->owner->def == b->def->owner );   // brush.cpp:4313
        SetKeyValue( def, groupKey, next );
    }
}

// KIWI: the surface inspector's mtldef[3] save / revert pair (PostDoSurface02 0x47CA50 /
// _Patch 0x47CAF0) is gone; the ImGui inspector applies each edit live.

// ── brush.cpp:5598 ──
// ═════════════════════════════════════════════════════════════════════════════
//  0x47CDE0  Select_ApplyMatrix — transform one brush by the orientation block.
//  `mat` is the orientation_t (origin + 3x3). bSnap = grid-snap on rebuild.
//  deg != 0 routes fixed-size (prefab/model) entities to Select_RotateFixedSize.
//  bSwap flips each face's planept winding order (planept[0]<->planept[2]) — used
//  by the mirror (Select_FlipAxis) so reflected faces keep outward-facing normals.
// ═════════════════════════════════════════════════════════════════════════════
// The loop bound uses def->faceCount (the instance-vs-def adaptation below).  KIWI: texture
// lock is Face_TexLockAffineBegin/End (kiwi_texlock.h), not the binary's float reproject.
extern void Select_RotateFixedSize( selbrush_t *sb, float (*mid_point)[3], const float *rot );  // select.cpp 0x47CC90

void Select_ApplyMatrix( float *mat, selbrush_t *b, int bSnap, float deg, char bSwap )
{
    if ( b->patch )
    {
        // Patch arm: transform the control points by the orientation block (rotate/flip).
        iassert( b->def->patch == b->patch->def );   // brush.cpp:5598
        Patch_ApplyMatrix( (const orientation_t *)mat, b->def->patch, (char)bSnap );
        return;
    }

    entity_s_def *ownerDef = (entity_s_def *)b->owner->def;
    iassert( b->owner->def == b->def->owner );   // brush.cpp:5608

    eclass_t *eclass = ownerDef->eclass;
    if ( *(int *)&eclass->fixedsize )
    {
        // Fixed-size entity (prefab / model bbox): rotate its origin + angles, no
        // planept transform. Only meaningful for a real rotation (deg != 0); a pure
        // mirror leaves the bbox where it is (matches the binary).
        if ( deg != 0.0f )
            Select_RotateFixedSize( b, (float (*)[3])mat, (const float *)eclass );
        return;
    }

    // instance-vs-def: the binary loops b->faceCount (the INSTANCE's cached count, set by
    // Brush_MakeFaceVisuals on the camera draw, so 0 headless / before the first 3D draw).
    // Loop the DEF count (authoritative, identical when valid) so the transform also
    // applies headless.  Same adaptation as SetupVertexSelection.
    const byte lockFlags[3] = { (byte)( g_PrefsDlg->m_bRotateLock != 0 ),
                                (byte)( g_PrefsDlg->m_bLightmapLock != 0 ), 1 };
    double turn[3][3], origin[3];
    for ( int a = 0; a < 3; ++a )
    {
        origin[a] = mat[a];
        for ( int c = 0; c < 3; ++c )
            turn[a][c] = mat[3 + a * 3 + c];          // the orientation's axis rows
    }
    double rows[3][KTEXLOCK_LAYERS][8];
    if ( b->def->faceCount )
    {
        for ( int fi = 0; fi < b->def->faceCount; ++fi )
        {
            face_t *f = &b->def->faces[fi];
            if ( bSwap )
            {
                // swap planepts[0] <-> planepts[2] (reverse winding for the mirror)
                float t0 = f->planepts[0][0], t1 = f->planepts[0][1], t2 = f->planepts[0][2];
                f->planepts[0][0] = f->planepts[2][0];
                f->planepts[0][1] = f->planepts[2][1];
                f->planepts[0][2] = f->planepts[2][2];
                f->planepts[2][0] = t0;
                f->planepts[2][1] = t1;
                f->planepts[2][2] = t2;
            }
            // KIWI (IDA 0x47cf23 / 0x47cf66): the binary brackets this with Face_TexLock_Save /
            // Face_TexLock_Reproject, lockFlags { m_bRotateLock, m_bLightmapLock, 1 } - the
            // port had dropped them as no-ops.  Done exactly (kiwi_texlock.h).
            Face_TexLockAffineBegin( f, f->plane.normal, turn, origin, lockFlags, rows );
            for ( int pi = 0; pi < 3; ++pi )
            {
                float rel[3];
                rel[0] = f->planepts[pi][0] - mat[0];   // VectorSubtract(pt - origin)
                rel[1] = f->planepts[pi][1] - mat[1];
                rel[2] = f->planepts[pi][2] - mat[2];
                OrientationPosToWorldPos( f->planepts[pi], rel,
                                          reinterpret_cast<const orientation_t *>( mat ) );
            }
            Face_TexLockAffineEnd( f, rows, lockFlags );
        }
    }

    Brush_BuildWindings( b->def, bSnap );
    if ( g_qeglobals.d_select_mode == sel_vertex || g_qeglobals.d_select_mode == sel_edge )
        SetupVertexSelection();
    MarkMapModified();
    ++b->def->version;
}


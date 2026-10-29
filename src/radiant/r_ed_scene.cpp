#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// cod3src\src\gfx_d3d\r_ed_scene.cpp
// Editor-only scene rendering: the per-frame surface cache the editor draw paths accumulate
// brush/model surfaces into, flushed as one RC_DRAW_EDITOR_SKINNEDCACHED command.  No kisak
// equivalent; adapted to kisak's backend API (tess == binary tess_r).

#include "stdafx.h"
#include <gfx_d3d/r_init.h>        // rg, rgp, frontEndFrameCount, dx, needSortMaterials
#include <gfx_d3d/r_material.h>    // Material_Sort, Material_FromHandle
#include <gfx_d3d/r_scene.h>       // R_ClearScene
#include <gfx_d3d/rb_backend.h>    // tess, gfxCmdBufContext, GfxCmdBufState, frontEndDataOut
#include <gfx_d3d/rb_state.h>      // gfxCmdBufState, gfxCmdBufSourceState
#include <gfx_d3d/rb_shade.h>      // RB_BeginSurface, RB_EndTessSurface
#include <gfx_d3d/r_shade.h>       // R_SetupPass*, R_UpdateVertexDecl, R_SetIndexData, R_SetVertexData
#include <gfx_d3d/r_state.h>       // R_ChangeStreamSource, R_DrawIndexedPrimitive, R_*Viewport
#include <gfx_d3d/r_utils.h>       // R_GetActiveWorldMatrix, R_MatrixIdentity44, R_Set3D
#include <gfx_d3d/r_debug.h>       // R_WarnOncePerFrame
#include <gfx_d3d/r_dobj_skin.h>   // GfxModelSkinnedSurface, GfxModelSurfaceInfo
#include <gfx_d3d/r_buffers.h>     // gfxBuf (dynamicVertexBuffer)
#include <gfx_d3d/r_light.h>       // R_RegisterLightDef_LoadObj
#include <gfx_d3d/r_rendercmds.h>  // RC_SetLightColor / clear-alpha fullscreen quad
#include <gfx_d3d/r_xsurface.h>    // XSurfaceGetNumVerts/Tris
#include <xanim/xmodel.h>          // XModel, XSurface, XModelBad, XModelGetBounds/Surfaces
#include <universal/com_math.h>    // AxisToQuat, QuatToAxis, mat3x3
#include <universal/profile.h>
#include "kiwi_modelcache.h"      // KiwiModelCache_Get / KiwiModelGeo
#include "kiwi_instcache.h"       // KiwiInstCache_* / KiwiInstGeo
#include "kiwi_light.h"           // Missing additive-technique policy
#include "kiwi_ported.h"          // KIWI: shared engine/editor externs
#include "kiwi_gpu_d3d9.h"        // KIWI: the retained scene's static index buffers (ED_SURF_BATCH)
#include "kiwi_edmodel.h"         // KIWI: model instances for the retained scene
#include <vector>
#include <algorithm>              // std::sort / std::inplace_merge (the mixed-window flush)
#include <vector>                 // KIWI (PERF): the main-flush sort-order cache
#include <unordered_map>
#include <stdlib.h>
#include <utility>                // std::pair (record + owning object, sorted together)

void FatalError(int code, const char *fmt, ...);                                // 0x49a9e0

// r_ed_vertbuf.cpp — handle → (vb, firstIndex)
void Editor_GetVertexBufferAndIndex(unsigned int handle, IDirect3DVertexBuffer9 **vb, uint16_t *firstIndex);

// ── surface-cache types (editorMesh_s / editorSurf_s) ────────────────────────
// KIWI: ED_SURF_BATCH is a retained-scene batch (kiwi_scene.h) - many faces in one static
// index buffer, drawn in one call.
enum EDITOR_SURF_TYPE { ED_SURF_MESH = 0, ED_SURF_MODEL = 1, ED_SURF_BATCH = 2 };

struct editorMesh_s {              // 24 bytes
    const Material *material;      // +0
    int       techType;           // +4
    int       sortKey;            // +8  (primary sort key)
    int       handle;             // +C  (LOWORD=firstIndex, HIWORD=vb buffer)
    uint16_t  vertCount;          // +10
    uint16_t  indexCount;         // +12
    const uint16_t *indexTable;         // +14 (edFaceIndices / edBackFaceIndices)
};

// editorSurf_sub — the ED_SURF_MODEL surf record (vs editorMesh_s, the brush-face one).
struct editorSurf_sub {            // 20 bytes
    Material               *material;   // +0
    int                     techType;   // +4
    int                     sortKey;    // +8
    union {                             // +C  the built skinned surface (verts+indices)
        GfxModelSkinnedSurface *skinnedSurf;
        GfxModelSkinnedSurface *surf;   //     (the binary's name — assert strings)
    };
    GfxScaledPlacement     *placement;  // +10 the model instance's world transform
};

// KIWI: the ED_SURF_BATCH record.  Its first three fields are editorMesh_s's, which is what
// Editor_SurfCompare's primary keys read.
struct editorBatch_s {
    const Material           *material;
    int                       techType;
    int                       sortKey;
    const KiwiGpuIndexBuffer *ib;
    int                       firstIndex;
    int                       triCount;
    unsigned                  vbPage;     // editor VB page, 1-based (the handle's HIWORD); 0 = `vb`
    int                       maxVert;    // highest vertex the batch references
    const KiwiGpuVertexBuffer *vb;        // a static buffer of its own (vbPage 0)...
    int                       vertDecl;   // ...its MaterialVertexDeclType...
    int                       stride;     // ...and vertex size
};

struct editorSurf_s {              // 8 bytes
    EDITOR_SURF_TYPE type;        // +0
    void            *mesh_or_surfSub; // +4  (editorMesh_s* for MESH, editorSurf_sub* for MODEL,
                                      //      editorBatch_s* for BATCH)
};

#define ED_SCENE_MAX_MESHES  0x40000
#define ED_SCENE_MAX_SURFS   0x44000
#define ED_SCENE_MAX_MODELSURFS 0x4000
#define ED_SCENE_MAX_BATCHES 0x10000

static struct {                        // the binary's edSceneGlobals block (assert strings)
    editorMesh_s sceneMeshes[ED_SCENE_MAX_MESHES];
    editorSurf_s sceneSurfs[ED_SCENE_MAX_SURFS];
    int          sceneMeshCount;
    int          sceneSurfCount;
    int          sceneSurfCount_saved;
} edSceneGlobals;
static editorBatch_s s_edBatches[ED_SCENE_MAX_BATCHES];   // KIWI: this frame's ED_SURF_BATCH records
static int           s_edBatchCount;
static edSurfRecording_t *s_edSurfRecording = nullptr;  // KIWI: Editor_SetSurfRecording

void Editor_SetSurfRecording(edSurfRecording_t *rec)
{
    s_edSurfRecording = rec;
}
static editorSurf_sub radiant_surfs[ED_SCENE_MAX_MODELSURFS];
static GfxModelSkinnedSurface radiant_modelSkinnedSurfs[ED_SCENE_MAX_MODELSURFS]; // built surfs
static int radiant_surfCount;          // model-surf counter
static int radiant_modelSurfPos;       // stands in for the binary's frontEndDataOut->surfPos
static int edScene_lastFrameCount;     // per-frame reset guard

// 0x4FED50  R_SetLightShaderConstants — build the exact editor GfxLight payload, clear
// alpha/stencil before its volume pass, then defer installation of the light
// constants until the command stream reaches this point.
void __cdecl R_SetLightShaderConstants(
    const float *origin, float radius, const float *color, const char *defName,
    const float *dir, float cosHalfFovInner, float cosHalfFovOuter, int exponent)
{
    vassert(radius > 0.0f, "(radius) = %g", radius);   // r_ed_scene.cpp:782

    GfxLight light;
    memset(&light, 0, sizeof(light));
    light.type = dir ? 2 : 3; // GFX_LIGHT_TYPE_SPOT / GFX_LIGHT_TYPE_OMNI
    light.color[0] = color[0];
    light.color[1] = color[1];
    light.color[2] = color[2];
    if ( dir )
    {
        light.dir[0] = dir[0];
        light.dir[1] = dir[1];
        light.dir[2] = dir[2];
        light.cosHalfFovOuter = cosHalfFovOuter;
        light.cosHalfFovInner = cosHalfFovInner;
        light.exponent = exponent;
    }
    else
    {
        light.exponent = 1;
    }
    light.origin[0] = origin[0];
    light.origin[1] = origin[1];
    light.origin[2] = origin[2];
    light.radius = radius;
    // KIWI-UX: retail IDB 0x4FED50 uses rgp.dlightDef for an omitted name.  Both CoD4
    // compilers instead default that authored light to light_point_linear, so the mutable
    // editor preview registers the compiler default while named defs keep retail loading.
    light.def = ( defName && *defName )
              ? R_RegisterLightDef_LoadObj( defName )
              : R_RegisterLightDef( "light_point_linear" );

    R_AddCmdDrawFullScreenColoredQuad(0.0f, 0.0f, 1.0f, 1.0f,
                                      colorWhite, rgp.clearAlphaStencilMaterial);
    RC_SetLightColor(&light);
}

// Triangle-fan index tables: triangle t = { t+1, t+2, 0 }
// front, { t+2, t+1, 0 } back.  Sized for the editor's max face (3*(N-2) <= 0xBA, N <= 64).
#define ED_FACE_MAX_INDICES 186
static uint16_t edFaceIndices[ED_FACE_MAX_INDICES];
static uint16_t edBackFaceIndices[ED_FACE_MAX_INDICES];
static bool     s_edFaceIndicesInit;

static void Editor_InitFaceIndices()
{
    for (int t = 0; t * 3 < ED_FACE_MAX_INDICES; ++t) {
        edFaceIndices[3 * t + 0]     = (uint16_t)(t + 1);
        edFaceIndices[3 * t + 1]     = (uint16_t)(t + 2);
        edFaceIndices[3 * t + 2]     = 0;
        edBackFaceIndices[3 * t + 0] = (uint16_t)(t + 2);
        edBackFaceIndices[3 * t + 1] = (uint16_t)(t + 1);
        edBackFaceIndices[3 * t + 2] = 0;
    }
    s_edFaceIndicesInit = true;
}

// KIWI-UX: camera fill techniques are the editor's opaque surface path.  Wireframe,
// sunlight-preview and shadow-cookie techniques are intentional overlays/contributions and
// retain their authored depth policy.  A primary sort bucket past 4 is sky/decal/blend work,
// where disabling depth writes is also intentional.
static bool Editor_IsOpaqueFillPass(const Material *material, int techType, uint stateBits0)
{
    if (!material || material->info.drawSurf.fields.primarySortKey > 4)
        return false;
    if ((stateBits0 & GFXS0_BLENDOP_RGB_MASK) != 0)
        return false;
    // KIWI (2026-10-29): a pass that writes no colour is a stencil/depth pass (the light
    // preview's retained shadow volumes, stencilshadow at UNLIT), not a fill.
    if ((stateBits0 & GFXS0_COLORWRITE_MASK) == 0)
        return false;
    return techType == TECHNIQUE_UNLIT
        || techType == TECHNIQUE_FAKELIGHT_NORMAL
        || techType == TECHNIQUE_FAKELIGHT_VIEW
        || techType == TECHNIQUE_CASE_TEXTURE;
}

static bool s_edUnsafeOpaqueDepthReported;

// Submission-time diagnostic requested for this failure class.  It inspects the same
// stateBitsEntry + pass index that R_SetupPass reads.
static void Editor_CheckSubmittedOpaqueDepth(const Material *material, int techType)
{
    if (s_edUnsafeOpaqueDepthReported || !material || techType < 0 || techType >= TECHNIQUE_COUNT)
        return;
    if (!material->stateBitsTable)
        return;
    const MaterialTechnique *technique = material->techniqueSet
                                       ? material->techniqueSet->techniques[techType] : nullptr;
    if (!technique)
        return;
    const int first = material->stateBitsEntry[techType];
    if (first < 0 || first >= material->stateBitsCount)
        return;
    for (int passIndex = 0; passIndex < technique->passCount; ++passIndex)
    {
        const int stateIndex = first + passIndex;
        if (stateIndex >= material->stateBitsCount)
            return;
        const GfxStateBits &bits = material->stateBitsTable[stateIndex];
        if (!Editor_IsOpaqueFillPass(material, techType, bits.loadBits[0]))
            continue;
        const uint depth = bits.loadBits[1];
        if ((depth & GFXS1_DEPTHWRITE) == 0
         || (depth & GFXS1_DEPTHTEST_DISABLE) != 0
         || (depth & GFXS1_DEPTHTEST_MASK) != GFXS1_DEPTHTEST_LESSEQUAL)
        {
            s_edUnsafeOpaqueDepthReported = true;
            Sys_Printf("Editor opaque surface submitted with unsafe depth state: material \"%s\", "
                       "tech %i, pass %i, stateBits {0x%08X,0x%08X}; forcing depthWrite + "
                       "LESSEQUAL.\n",
                       material->info.name ? material->info.name : "(null)", techType, passIndex,
                       bits.loadBits[0], bits.loadBits[1]);
            return;
        }
    }
}

// ── front-end accumulation ────────────────────────────────────────────────────

// 0x4FDA50  Editor_AddMeshCmd — append one cached mesh + its surf entry.
void __cdecl Editor_AddMeshCmd(Material *handle, int techType, int sortKey,
                               int vertCount, int vbIndexAndOffs, int indexCount, const uint16_t *indexTable)
{
    iassert(techType >= TECHNIQUE_DEPTH_PREPASS);         // (level 0)
    const Material *material = Material_FromHandle(handle);
    iassert( material );   // r_ed_scene.cpp:224

    // The binary's technique set reserves slot 0 (techniques[techType+1]); kisak's
    // MaterialTechniqueSet is indexed directly by techType (r_material.cpp:753).
    if (techType < TECHNIQUE_COUNT && !material->techniqueSet->techniques[techType])
        return;

    if (s_edSurfRecording) {                    // KIWI: Editor_SetSurfRecording
        edRecMesh_t m;
        m.material   = handle;
        m.techType   = techType;
        m.sortKey    = sortKey;
        m.vbHandle   = (unsigned)vbIndexAndOffs;
        m.vertCount  = vertCount;
        m.indices    = indexTable;
        m.indexCount = indexCount;
        s_edSurfRecording->meshes.push_back(m);
        return;
    }

    if (edSceneGlobals.sceneMeshCount == ED_SCENE_MAX_MESHES) {
        R_WarnOncePerFrame((GfxWarningType)34, ED_SCENE_MAX_MESHES);
        return;
    }

    Editor_CheckSubmittedOpaqueDepth(material, techType);

    editorMesh_s *mesh = &edSceneGlobals.sceneMeshes[edSceneGlobals.sceneMeshCount++];
    mesh->handle      = vbIndexAndOffs;
    mesh->material    = material;
    mesh->techType    = techType;
    mesh->sortKey     = sortKey;
    mesh->vertCount = (uint16_t)vertCount;
    iassert(mesh->vertCount == vertCount);                // r_ed_scene.cpp:241, after the store
    mesh->indexCount  = (uint16_t)indexCount;
    iassert(mesh->indexCount == indexCount);              // (level 0), after the store
    mesh->indexTable  = indexTable;

        iassert(edSceneGlobals.sceneSurfCount < ARRAY_COUNT( edSceneGlobals.sceneSurfs ));   // r_ed_scene.cpp:246
    editorSurf_s *surf = &edSceneGlobals.sceneSurfs[edSceneGlobals.sceneSurfCount++];
    surf->mesh_or_surfSub = mesh;
    surf->type = ED_SURF_MESH;
}

// 0x4FEEF0  Editor_AddGeoFace — emit a front-facing fan (edFaceIndices) for one face.
//
// KIWI-UX — THE ASSERT HAS TO RETURN.  In the binary's dev build the Assert above the
// emit is a hard stop, so control never reaches Editor_AddMeshCmd with a fan that does
// not fit; in this port Assert is NON-FATAL, so it fell straight through and emitted
// `indexCount = 3 * vertCount - 6` anyway.  For vertCount < 3 that is NEGATIVE (-6 / -3),
// and Editor_AddMeshCmd stores it into a uint16 (:131-134) — so the surf claims ~65530
// indices over a 186-entry table and every consumer walks ~131 KB past `edFaceIndices`.
// For vertCount > 64 it walks past it by a smaller, equally arbitrary amount.  Either way
// the values that come back are not indices into this face's vertex run, and the triangles
// they build reach whatever else lives in the shared vertex buffer.  Dropping the face is
// the same class of correction as the "MESH surf with no resolvable VB MUST be dropped"
// rule below (:1991-1995), and it is what the dev build's stop achieves.
void __cdecl Editor_AddGeoFace(Material *handle, int techType, int sortKey, int vertCount, int vbIndexAndOffs)
{
    if (!s_edFaceIndicesInit)
        Editor_InitFaceIndices();
    const bool fits = vertCount >= 3 && (unsigned)(3 * vertCount - 6) <= ED_FACE_MAX_INDICES;
    vassert(fits, "(vertCount) = %i", vertCount);   // r_ed_scene.cpp:197
    if (!fits)
        return;
    Editor_AddMeshCmd(handle, techType, sortKey, vertCount, vbIndexAndOffs, 3 * vertCount - 6, edFaceIndices);
}

// 0x4FEF50  Editor_AddGeoBackFace — emit a back-facing fan (edBackFaceIndices) for one face.
// The same non-fatal-Assert fall-through, and the same correction — see Editor_AddGeoFace.
void __cdecl Editor_AddGeoBackFace(Material *handle, int techType, int sortKey, int vertCount, int vbIndexAndOffs)
{
    if (!s_edFaceIndicesInit)
        Editor_InitFaceIndices();
    const bool fits = vertCount >= 3 && (unsigned)(3 * vertCount - 6) <= ED_FACE_MAX_INDICES;
    vassert(fits, "(vertCount) = %i", vertCount);   // r_ed_scene.cpp:204
    if (!fits)
        return;
    Editor_AddMeshCmd(handle, techType, sortKey, vertCount, vbIndexAndOffs, 3 * vertCount - 6, edBackFaceIndices);
}

// KIWI: a new ED_SURF_BATCH record with the same technique gate and depth diagnostic as
// Editor_AddMeshCmd; the caller says where its vertices are.  nullptr = refused.
static editorBatch_s *Editor_NewBatch(Material *handle, int techType, int sortKey,
                                      const KiwiGpuIndexBuffer *ib, int firstIndex, int triCount,
                                      int maxVert)
{
    if (!ib || triCount <= 0)
        return nullptr;
    const Material *material = Material_FromHandle(handle);
    if (!material)
        return nullptr;
    if (techType < TECHNIQUE_COUNT && !material->techniqueSet->techniques[techType])
        return nullptr;
    if (s_edBatchCount == ED_SCENE_MAX_BATCHES
     || edSceneGlobals.sceneSurfCount == ARRAY_COUNT(edSceneGlobals.sceneSurfs)) {
        R_WarnOncePerFrame((GfxWarningType)34, ED_SCENE_MAX_BATCHES);
        return nullptr;
    }

    Editor_CheckSubmittedOpaqueDepth(material, techType);

    editorBatch_s *batch = &s_edBatches[s_edBatchCount++];
    batch->material   = material;
    batch->techType   = techType;
    batch->sortKey    = sortKey;
    batch->ib         = ib;
    batch->firstIndex = firstIndex;
    batch->triCount   = triCount;
    batch->maxVert    = maxVert;

    editorSurf_s *surf = &edSceneGlobals.sceneSurfs[edSceneGlobals.sceneSurfCount++];
    surf->mesh_or_surfSub = batch;
    surf->type = ED_SURF_BATCH;
    return batch;
}

// KIWI: one retained-scene batch (kiwi_scene.h): `triCount` triangles of the static buffer
// `ib` from `firstIndex`, indexing the editor VB page `vbPage`.
void Editor_AddBatchCmd(Material *handle, int techType, int sortKey, unsigned vbPage,
                        const KiwiGpuIndexBuffer *ib, int firstIndex, int triCount, int maxVert)
{
    if (!vbPage)
        return;
    editorBatch_s *batch = Editor_NewBatch(handle, techType, sortKey, ib, firstIndex, triCount, maxVert);
    if (!batch)
        return;
    batch->vbPage   = vbPage;
    batch->vb       = nullptr;
    batch->vertDecl = VERTDECL_WORLD;
    batch->stride   = sizeof(GfxWorldVertex);
}

// KIWI: a batch over a static vertex buffer of its own instead of an editor VB page - the
// light preview's retained shadow volumes (GfxVertex at VERTDECL_GENERIC, exactly as
// RB_DrawTriangles_Internal builds them in tess) and the scene's world-space model runs
// (GfxPackedVertex at VERTDECL_PACKED, as the merged-instance draw holds them).
void Editor_AddVertexBatchCmd(Material *handle, int techType, int sortKey,
                              const KiwiGpuVertexBuffer *vb, int vertDecl, int stride,
                              const KiwiGpuIndexBuffer *ib, int firstIndex, int triCount,
                              int maxVert)
{
    if (!vb)
        return;
    editorBatch_s *batch = Editor_NewBatch(handle, techType, sortKey, ib, firstIndex, triCount, maxVert);
    if (!batch)
        return;
    batch->vbPage   = 0;
    batch->vb       = vb;
    batch->vertDecl = vertDecl;
    batch->stride   = stride;
}

// 0x4FDBB0  Editor_MaterialSortKey — the sortKey DrawGeo (0x47acf0) passes to Editor_AddGeoFace: 100 x
// the material's primary sort bucket.  Per-layer visuals of one face get consecutive keys
// (base+layerIndex), so layer order survives.
int __cdecl Editor_MaterialSortKey(Material *handle)
{
    return 100 * (int)Material_FromHandle(handle)->info.drawSurf.fields.primarySortKey;
}

// 0x4FD9C0  Editor_SurfCompare — surf sort comparator.  editorMesh_s and editorSurf_sub share their
// first three fields (material@0, techType@4, sortKey@8), so the primary keys work on both.
// EVERY KEY IS LOAD-BEARING ORDERING, do not simplify:
//   1/2 sortKey, techType — the material sort bucket (opaques 400, sky 500, decals 1200,
//       blends 4300+).  Layering correctness depends on these two and nothing else.
//   3   surf TYPE — models(0) before meshes(1); also what keeps this a valid STRICT WEAK
//       ORDERING, since a MESH with handle 0 could otherwise tie with models and make the
//       per-type tie-breaks intransitive (UB in the sort).
//   4   per-type tie-break — what lets the backend batch runs into one draw.
//   5   record ADDRESS = submission order; neither sort is stable.
static int __cdecl Editor_SurfCompare(const void *pa, const void *pb)
{
    const editorSurf_s *a = (const editorSurf_s *)pa;
    const editorSurf_s *b = (const editorSurf_s *)pb;
    const editorMesh_s *ma = (const editorMesh_s *)a->mesh_or_surfSub;
    const editorMesh_s *mb = (const editorMesh_s *)b->mesh_or_surfSub;
    int result = ma->sortKey - mb->sortKey;
    if (!result)
        result = ma->techType - mb->techType;
    if (!result)
        result = (int)a->type - (int)b->type;
    if (result)
        return result;

    // 4a/4b — group same-material, same-geometry model surfs.  Compared as uintptr_t
    // rather than subtracted: with LAA a >2GB span would wrap the returned int.
    if (a->type == ED_SURF_MODEL) {
        const editorSurf_sub *sa = (const editorSurf_sub *)a->mesh_or_surfSub;
        const editorSurf_sub *sb = (const editorSurf_sub *)b->mesh_or_surfSub;
        uintptr_t va = (uintptr_t)sa->material, vb = (uintptr_t)sb->material;
        if (va != vb)
            return (va < vb) ? -1 : 1;
        va = (uintptr_t)(sa->skinnedSurf ? sa->skinnedSurf->xsurf : nullptr);
        vb = (uintptr_t)(sb->skinnedSurf ? sb->skinnedSurf->xsurf : nullptr);
        if (va != vb)
            return (va < vb) ? -1 : 1;
    }
    else if (a->type == ED_SURF_BATCH) {
        // KIWI: material, then VB page - consecutive batches then share their binds.
        const editorBatch_s *ba = (const editorBatch_s *)a->mesh_or_surfSub;
        const editorBatch_s *bb = (const editorBatch_s *)b->mesh_or_surfSub;
        uintptr_t va = (uintptr_t)ba->material, vb = (uintptr_t)bb->material;
        if (va != vb)
            return (va < vb) ? -1 : 1;
        if (ba->vbPage != bb->vbPage)
            return (ba->vbPage < bb->vbPage) ? -1 : 1;
    }
    else {
        // material FIRST, then vb + offset.  One vb holds interleaved 256-vert pools of many
        // materials (r_ed_vertbuf.cpp:282), so buffer-first cuts the batches on every surf.
        uintptr_t va = (uintptr_t)ma->material, vb = (uintptr_t)mb->material;
        if (va != vb)
            return (va < vb) ? -1 : 1;
        if (ma->handle != mb->handle)
            return ((unsigned)ma->handle < (unsigned)mb->handle) ? -1 : 1;
    }

    // 5 — submission order, so equal keys stay deterministic.
    {
        uintptr_t va = (uintptr_t)a->mesh_or_surfSub, vb = (uintptr_t)b->mesh_or_surfSub;
        if (va != vb)
            return (va < vb) ? -1 : 1;
    }
    return 0;
}

#ifdef KISAK_RADIANT
// The `less` adaptor std::sort wants.  The comparator above is a TOTAL order (it ends on
// the unique record address), which is why no stability guarantee is needed.
struct Editor_SurfLess
{
    bool operator()(const editorSurf_s &a, const editorSurf_s &b) const
    {
        return Editor_SurfCompare(&a, &b) < 0;
    }
};
#endif

// 0x4FD300  Editor_AddCmd_DrawSkinnedCached — emit the backend command.
struct GfxCmdEditorSkinnedCached { GfxCmdHeader header; int index; int amount; };

void *__cdecl Editor_AddCmd_DrawSkinnedCached(int index, int amount)
{
    GfxCmdEditorSkinnedCached *cmd =
        (GfxCmdEditorSkinnedCached *)R_GetCommandBuffer(RC_DRAW_EDITOR_SKINNEDCACHED, sizeof(GfxCmdEditorSkinnedCached));
    if (cmd) {
        cmd->index  = index;
        cmd->amount = amount;
    }
    return cmd;
}

// One-shot, set just before a flush whose window opens with entries that are ALREADY in
// comparator order, and consumed by R_AddEditorSurfsCmd.  0 = no claim.  Absolute, so a
// fill ahead of the claimed entries cannot make it a lie: it is only honoured when it
// names a prefix that starts at the window's own first entry.
static int  s_edPendingSortedFirst = 0;
static int  s_edPendingSortedCount = 0;

// KIWI (2026-10-29): the open window's first `count` entries are the retained scene's
// batches, emitted in comparator order (kiwi_scene.cpp sorts each window's batches by
// Editor_SurfCompare's keys) - the flush sorts only what follows them and merges.
void KiwiEdScene_StampSortedHead(int count)
{
    s_edPendingSortedFirst = edSceneGlobals.sceneSurfCount_saved;
    s_edPendingSortedCount = count;
}

// 0x4FDA10  R_AddEditorSurfsCmd — sort the surfs added since the last flush and emit
// one RC_DRAW_EDITOR_SKINNEDCACHED for them.
void *__cdecl R_AddEditorSurfsCmd()
{
    PROF_SCOPED( "R_AddEditorSurfsCmd (sort)" );
    int first = edSceneGlobals.sceneSurfCount_saved;
    int count = edSceneGlobals.sceneSurfCount - first;
    // Consumed here whatever happens below, so an empty window cannot leave one armed.
    const int  sortedFirst = s_edPendingSortedFirst;
    const int  sortedCount = s_edPendingSortedCount;
    s_edPendingSortedFirst = 0;
    s_edPendingSortedCount = 0;
    if (count) {
#ifdef KISAK_RADIANT
        // SORTED HEAD + LIVE TAIL: [ entries already in comparator order - the retained
        // scene's batches ][ the rest ].
        // Sorting only the tail and merging is O(n) with a scratch buffer instead of
        // O(n log n) over the whole window.  Only claimed when the prefix really is the head
        // of THIS window.
        bool headSorted = sortedCount > 0 && sortedFirst == first && sortedCount <= count;
        if (headSorted && !std::is_sorted(&edSceneGlobals.sceneSurfs[first],
                                          &edSceneGlobals.sceneSurfs[first + sortedCount],
                                          Editor_SurfLess())) {
            // A claim is checked, not trusted: the full sort below is always correct.
            static bool s_reported = false;
            if (!s_reported) {
                s_reported = true;
                Sys_Printf("R_AddEditorSurfsCmd: a sorted-head claim of %d entries was not sorted.\n",
                           sortedCount);
            }
            headSorted = false;
        }
        if (headSorted) {
            std::sort(&edSceneGlobals.sceneSurfs[first + sortedCount],
                      &edSceneGlobals.sceneSurfs[first] + count, Editor_SurfLess());
            std::inplace_merge(&edSceneGlobals.sceneSurfs[first],
                               &edSceneGlobals.sceneSurfs[first + sortedCount],
                               &edSceneGlobals.sceneSurfs[first] + count, Editor_SurfLess());
            return Editor_AddCmd_DrawSkinnedCached(first, count);
        }
        std::sort(&edSceneGlobals.sceneSurfs[first],
                  &edSceneGlobals.sceneSurfs[first] + count, Editor_SurfLess());
#else
        qsort(&edSceneGlobals.sceneSurfs[first], count, sizeof(editorSurf_s), Editor_SurfCompare);
#endif
        return Editor_AddCmd_DrawSkinnedCached(first, count);
    }
    return (void *)first;
}

// How many surfs the OPEN flush window holds.  Read-only; the boundaries stay
// R_SortMaterials' business.
int Editor_PendingSurfCount()
{
    return edSceneGlobals.sceneSurfCount - edSceneGlobals.sceneSurfCount_saved;
}

// ── the xmodel mesh path (ED_SURF_MODEL) ──────────────────────────────────────
// DrawBrush → DrawModels → SkinModelInst → AddModelSurfBuf + Editor_AddSurfCmd, then
// RB_DrawEditorSkinnedCached_Sub.  Does NOT use the per-material VB pool.

// model_inst (44 bytes) — origin/quat/scale alias GfxScaledPlacement at +0, so an inst
// pointer casts straight to a placement.
struct model_inst {                // 44 bytes
    float    angles[4];            // +0   quat (GfxPlacement.quat)
    float    origin[3];            // +16  (GfxPlacement.origin)
    float    modelscale;           // +28  (GfxScaledPlacement.scale)
    int      colorOverride;        // +32  pad_0x0020 — per-vert color (or -1)
    XModel  *model;                // +36
    int      inuse;                // +40
};
static_assert(sizeof(model_inst) == (sizeof(void *) == 8 ? 56 : 44), "model_inst native storage");
static_assert(offsetof(model_inst, angles) == 0, "model_inst placement quaternion");
static_assert(offsetof(model_inst, origin) == offsetof(GfxScaledPlacement, base) + offsetof(GfxPlacement, origin), "model_inst placement origin");
static_assert(offsetof(model_inst, modelscale) == offsetof(GfxScaledPlacement, scale), "model_inst placement scale");

#define ED_MAP_MAX_MODELINST 65536
struct EdMapGlobals {              // edMapGlobals
    int        modelInstMax;       // highest-used slot + 1 (NOT a fixed capacity)
    model_inst modelInst[ED_MAP_MAX_MODELINST + 16];
};
static EdMapGlobals edMapGlobals;
// KIWI (2026-10-29): bumped by the three placement writers below, so the retained scene's
// scan sees a re-posed, re-used or freed instance without reading its placement.
static unsigned s_modelInstVersion[ED_MAP_MAX_MODELINST + 16];

unsigned Editor_ModelInstVersion(int instanceHandle)
{
    if (instanceHandle <= 0 || instanceHandle > ED_MAP_MAX_MODELINST + 16)
        return 0;
    return s_modelInstVersion[instanceHandle - 1];
}

// 0x4FDBE0  AddModelToModelInstBuff — claim a free slot, return its 1-based handle.
int __cdecl AddModelToModelInstBuff(XModel *model, float *axis, float scale)
{
    iassert(model);                                       // (level 0)
    if (XModelBad(model))
        return 0;

    int idx = 0;
    while (idx < edMapGlobals.modelInstMax && edMapGlobals.modelInst[idx].inuse)
        ++idx;

    if (idx == edMapGlobals.modelInstMax) {
        if (edMapGlobals.modelInstMax == 65536)
            FatalError(0, "too many models in map");
        if (edMapGlobals.modelInstMax == 65520)
            MessageBoxA(GetActiveWindow(),
                "WARNING!\nThis map is dangerously close to the editor's internal model limit.\n"
                "Adding any more models may cause the editor to exit without saving.\n", "Warning", 0x30u);
        ++edMapGlobals.modelInstMax;
    }

    model_inst *v8 = &edMapGlobals.modelInst[idx];
    // DIRTY SIGNAL 1 of 3 (kiwi_instcache.h): a reused slot means whatever the instance
    // cache holds against this address is about to stop existing.
    KiwiInstCache_InvalidateInstance((const GfxScaledPlacement *)v8);
    ++s_modelInstVersion[idx];
    memset(v8, 0, sizeof(model_inst));
    v8->inuse      = 1;            // the binary set only the low byte
    v8->model      = model;
    v8->origin[0]  = axis[0];
    v8->origin[1]  = axis[1];
    v8->origin[2]  = axis[2];
    AxisToQuat((vec3_t *)(axis + 3), v8->angles);
    v8->modelscale = scale;
    return idx + 1;
}

// 0x4FDD80  ModelInstUpdate — re-pose an existing instance.
void __cdecl ModelInstUpdate(int instanceHandle, float (*axis)[3], float scale)
{
    iassert(instanceHandle > 0 && instanceHandle <= edMapGlobals.modelInstMax);
    model_inst *v3 = &edMapGlobals.modelInst[instanceHandle - 1];
    iassert(edMapGlobals.modelInst[instanceHandle - 1].inuse);   // r_ed_scene.cpp:330
    // DIRTY SIGNAL 2 of 3 — the editor's move/rotate/scale funnel (every entity edit path
    // ends in Entity_UpdateModelInst, entity.cpp, this function's only caller).
    KiwiInstCache_InvalidateInstance((const GfxScaledPlacement *)v3);
    ++s_modelInstVersion[instanceHandle - 1];
    v3->origin[0] = axis[0][0];
    v3->origin[1] = axis[0][1];
    v3->origin[2] = axis[0][2];
    AxisToQuat((vec3_t *)&axis[1][0], v3->angles);
    v3->modelscale = scale;
}

// 0x4FDCE0  RemoveModelInstFromBuf — free a slot, shrink modelInstMax past trailing frees.
void __cdecl RemoveModelInstFromBuf(int instanceHandle)
{
    iassert(instanceHandle > 0 && instanceHandle <= edMapGlobals.modelInstMax);
    iassert(edMapGlobals.modelInst[instanceHandle - 1].inuse);   // r_ed_scene.cpp:314
    // DIRTY SIGNAL 3 of 3 — leave the pre-transformed pool now, not when the slot is reused.
    KiwiInstCache_InvalidateInstance(
        (const GfxScaledPlacement *)&edMapGlobals.modelInst[instanceHandle - 1]);
    ++s_modelInstVersion[instanceHandle - 1];
    edMapGlobals.modelInst[instanceHandle - 1].inuse = 0;
    edMapGlobals.modelInst[instanceHandle - 1].model = 0;
    int n = edMapGlobals.modelInstMax;
    while (n > 0 && !edMapGlobals.modelInst[n - 1].inuse)
        --n;
    edMapGlobals.modelInstMax = n;
}

// 0x4FDE10  Entity_GetModelInstBounds — local-space mins/maxs of an instance's
// model (xmodel bounds rotated by the instance quat, scaled by modelscale).
void __cdecl Entity_GetModelInstBounds(int instanceHandle, float *out_mins, float *out_maxs)
{
    iassert(instanceHandle > 0 && instanceHandle <= edMapGlobals.modelInstMax);
    model_inst *mi = &edMapGlobals.modelInst[instanceHandle - 1];
    iassert(edMapGlobals.modelInst[instanceHandle - 1].inuse);   // r_ed_scene.cpp:351

    float mins[3], maxs[3];
    XModelGetBounds(mi->model, mins, maxs);
    mat3x3 axis;
    QuatToAxis(mi->angles, axis);
    // Rotate the local AABB by axis into an axis-aligned [mins,maxs] pair.
    float rmin[3], rmax[3];
    for (int i = 0; i < 3; ++i) { rmin[i] = 1e30f; rmax[i] = -1e30f; }
    for (int c = 0; c < 8; ++c) {
        float p[3] = { (c & 1) ? maxs[0] : mins[0], (c & 2) ? maxs[1] : mins[1], (c & 4) ? maxs[2] : mins[2] };
        float r[3];
        for (int i = 0; i < 3; ++i)
            r[i] = axis[0][i] * p[0] + axis[1][i] * p[1] + axis[2][i] * p[2];
        for (int i = 0; i < 3; ++i) { if (r[i] < rmin[i]) rmin[i] = r[i]; if (r[i] > rmax[i]) rmax[i] = r[i]; }
    }
    for (int i = 0; i < 3; ++i) {
        // The binary's rotate helper seeds its min/max WITH origin and subtracts it back out;
        // rmin/rmax here are already origin-free.
        out_mins[i] = mi->modelscale * rmin[i] + mi->origin[i];
        out_maxs[i] = mi->modelscale * rmax[i] + mi->origin[i];
    }
}

// ── model-surf builder (the ED_SURF_MODEL queue) ──────────────────────────────

// 0x4FE0D0  AddSurfTempSkinBuf — copy verts0 into frontEndDataOut->tempSkinBuf and point
// skinnedVert at that WRITABLE COPY, so SkinModelInst's colour stamp never writes
// the shared asset verts0.  skinnedCachedOffset = -1; on overflow the binary drops the model.
void __cdecl Z_VirtualCommit(void *ptr, int size);    // com_memory.cpp (0x4AC210)

// KIWI: two divergences from the binary, whose per-frame decommit an editor frame cannot
// afford.  The arithmetic, 32-byte stride, 0x5000000 cap and drop-on-overflow are unchanged.
//   (A) NO COPY WHEN NOTHING WRITES — `needWritable` is threaded down from SkinModelInst
//       rather than inferred, so a future caller that stamps gets the copy back.
//   (B) COMMIT TO A HIGH-WATER MARK.  MUST be invalidated by Radiant_TempSkin_Invalidate()
//       from R_ShutdownTempSkinBuf: a fresh Z_VirtualReserve can hand back the SAME address
//       fully decommitted, and a stale high-water there is an access violation.

// Per-frame counters, published+zeroed by R_SortMaterials' once-per-frame arm.
static int s_edSkinSurfs   = 0;   // model surfaces prepared
static int s_edSkinBytes   = 0;   // bytes memcpy'd into tempSkinBuf (0 = change (A) took them all)
static int s_edSkinCommits = 0;   // Z_VirtualCommit calls actually issued

static int s_edModelInsts        = 0;   // SkinModelInst calls (model INSTANCES queued)
static int s_edModelSurfsCached  = 0;   // model surfs drawn straight from the static pool
static int s_edModelSurfsDynamic = 0;   // model surfs still paying a per-frame upload
static int s_edModelUploadBytes  = 0;   // bytes those dynamic surfs pushed through R_SetVertexData

static int s_edDrawCalls      = 0;
static int s_edMergedDraws    = 0;
static int s_edMergedInsts    = 0;
static int s_edPassSetups     = 0;
static int s_edPassSetupsSkip = 0;
static int s_edVsConstUploads = 0;
static int s_edBeginSurfaces  = 0;
static int s_edStreamSwitches = 0;

static int s_edRunIndexLocks  = 0;   // index-buffer locks the merged draws take (1 per draw)
static int s_edRunIndexRuns   = 0;   // instance index arrays those draws concatenate

static int s_edMeshTessDraws = 0;   // tess batches — each one is an index UPLOAD
static int s_edMeshSurfs     = 0;   // mesh surfs seen by the flushes this frame

// Two slots: GfxBackEndData alternates and each half owns its own tempSkinBuf reservation.
static uint8_t *s_edSkinCommitBuf[2];
static unsigned s_edSkinCommitEnd[2];

// Called from R_ShutdownTempSkinBuf (r_buffers.cpp) after the reservations are freed.
void Radiant_TempSkin_Invalidate()
{
    s_edSkinCommitBuf[0] = s_edSkinCommitBuf[1] = nullptr;
    s_edSkinCommitEnd[0] = s_edSkinCommitEnd[1] = 0;
}

static void Editor_TempSkinCommit(uint8_t *base, unsigned endOffset, uint8_t *from, unsigned size)
{
    int slot = -1;
    for (int i = 0; i < 2; ++i)
        if (s_edSkinCommitBuf[i] == base) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < 2; ++i)
            if (!s_edSkinCommitBuf[i]) { s_edSkinCommitBuf[i] = base; s_edSkinCommitEnd[i] = 0; slot = i; break; }
    if (slot < 0) {
        // More than two live reservations — fall back to the binary's own behaviour.
        Z_VirtualCommit(from, (int)size);
        ++s_edSkinCommits;
        return;
    }
    if (endOffset > s_edSkinCommitEnd[slot]) {
        uint8_t *newFrom = base + s_edSkinCommitEnd[slot];
        Z_VirtualCommit(newFrom, (int)(endOffset - s_edSkinCommitEnd[slot]));
        s_edSkinCommitEnd[slot] = endOffset;
        ++s_edSkinCommits;
    }
}

static bool AddSurfTempSkinBuf(XSurface *xsurf, GfxModelSkinnedSurface *out, XModel *model,
                               bool needWritable)
{
    iassert(model);
    iassert(xsurf);
    out->skinnedCachedOffset = -1;            // RIGID/UNCACHED — draw from the temp copy
    out->xsurf               = xsurf;
    if (!xsurf->verts0)
        return false;

    ++s_edSkinSurfs;
    // KIWI change (A): nothing will write these verts, so hand out the asset's own run.
    if (!needWritable) {
        out->skinnedVert = (GfxPackedVertex *)xsurf->verts0;
        return true;
    }

    const int numVerts  = XSurfaceGetNumVerts(xsurf);
    const unsigned vsize = 32u * (unsigned)numVerts;   // 32 bytes per vert
    GfxBackEndData *fe = frontEndDataOut;
    if (!fe || !fe->tempSkinBuf)
        return false;                                        // the binary asserted; no buffer -> drop
    if (vsize + (unsigned)fe->tempSkinPos > 0x5000000u) {
        return false;                                        // drop this surf/model
    }
    uint8_t *copy = fe->tempSkinBuf + fe->tempSkinPos;
    fe->tempSkinPos += (long)vsize;
    // KIWI change (B): the binary's Z_VirtualCommit (0x4fe1ac), deduped against the
    // high-water mark.  Identical page state.
    Editor_TempSkinCommit(fe->tempSkinBuf, (unsigned)fe->tempSkinPos, copy, vsize);
    s_edSkinBytes += (int)vsize;
    memcpy(copy, xsurf->verts0, vsize);                      // copy the base verts
    out->skinnedVert = (GfxPackedVertex *)copy;              // skinnedVert = the copy
    return true;
}

// xmodel_utils helpers kisak lacks (CoD4Radiant-only) — trivial LOD-0 accessors.
//  GetXmodelNumSurfs (0x4CBD80)      = lodInfo[lod].numsurfs
//  GetXmodelMaterialHandle (0x4CBDE0)= &materialHandles[lodInfo[lod].surfIndex]
static inline int GetXmodelNumSurfs(const XModel *m, int lod) { return (uint16_t)m->lodInfo[lod].numsurfs; }
static inline Material **GetXmodelMaterialHandle(XModel *m, int lod) { return &m->materialHandles[(uint16_t)m->lodInfo[lod].surfIndex]; }

// 0x4FE1D0  AddModelSurfBuf — build one GfxModelSkinnedSurface per LOD-0 surface of the
// xmodel; nullptr on overflow or a failed surf.
// kisak's GfxBackEndData has no surfsBuffer/surfPos, so the editor keeps its own array
// + cursor.  The cursor advances by the FULL surface count of every model (NOT by the number
// queued — Editor_AddSurfCmd can drop some), so a queued record is never overwritten.
static GfxModelSkinnedSurface *AddModelSurfBuf(XModel *xmodel, bool needWritable)
{
    if (XModelBad(xmodel))
        return 0;
    XModelNumBones(xmodel);                   // (bone count not needed for rigid)

    XSurface *surfBase = nullptr;
    int surfaceCount = XModelGetSurfaces(xmodel, &surfBase, 0);
    iassert(surfaceCount);

    const int firstSurfSlot = radiant_modelSurfPos;
    if (firstSurfSlot + surfaceCount > ED_SCENE_MAX_MODELSURFS)   // binary: surfsBuffer cap
        return 0;
    GfxModelSkinnedSurface *dst = &radiant_modelSkinnedSurfs[firstSurfSlot];
    for (int i = 0; i < surfaceCount; ++i) {
        if (!AddSurfTempSkinBuf(&surfBase[i], &dst[i], xmodel, needWritable))
            return 0;                         // drop the model, cursor unmoved
    }
    radiant_modelSurfPos = firstSurfSlot + surfaceCount;
    return dst;
}

// 0x4FDF40  Editor_SurfFilter — multiply/skip-multiply draw-flag filter.  The binary keys "effect"
// on editorToolFlags & 0x70 == 0x70, then masks 8 for opaque / 4 for effect.
static bool Editor_SurfFilter(int drawFlags, const Material *material)
{
    if ((drawFlags & 0xC) == 0)
        return true;
    iassert(!(drawFlags & DRAWFLAG_ONLY_MULTIPLY) || !(drawFlags & DRAWFLAG_SKIP_MULTIPLY));   // r_ed_scene.cpp:496
    const unsigned flags  = material ? material->editorToolFlags : 0;   // no material -> opaque
    const bool     opaque = (flags & 0x70) != 0x70;
    return (drawFlags & (opaque ? 8 : 4)) != 0;
}

// 0x4FDFA0  Editor_AddSurfCmd — queue one ED_SURF_MODEL surf (a built skinned surface +
// the instance placement) into the scene surf list.
void __cdecl Editor_AddSurfCmd(int drawFlags, Material *material, model_inst *inst,
                               GfxModelSkinnedSurface *surf, int techType)
{
    iassert(surf);
    iassert(material);

    // Gate: queue ONLY when the material carries the requested technique (or
    // techType >= TECHNIQUE_COUNT), else DROP it.  kisak indexes techniques[techType] with no +1.
    if (techType >= TECHNIQUE_COUNT || material->techniqueSet->techniques[techType]) {
        if (s_edSurfRecording) {                // KIWI: Editor_SetSurfRecording
            if (!Editor_SurfFilter(drawFlags, material))
                return;
            const XSurface *xsurf = surf->xsurf;
            edRecModel_t m;
            m.surf.material   = material;
            m.surf.techType   = techType;
            m.surf.sortKey    = 0x64 * Material_FromHandle(material)->info.drawSurf.fields.primarySortKey;
            m.surf.xsurf      = xsurf;
            m.surf.vertCount  = xsurf ? XSurfaceGetNumVerts(xsurf) : 0;
            m.surf.indices    = xsurf ? xsurf->triIndices : nullptr;
            m.surf.indexCount = xsurf ? 3 * XSurfaceGetNumTris(xsurf) : 0;
            m.instance        = (int)(inst - edMapGlobals.modelInst) + 1;
            m.stamped         = !xsurf || surf->skinnedVert != (GfxPackedVertex *)xsurf->verts0;
            s_edSurfRecording->models.push_back(m);
            return;
        }
        if (radiant_surfCount == ED_SCENE_MAX_MODELSURFS) {
            R_WarnOncePerFrame((GfxWarningType)35, ED_SCENE_MAX_MODELSURFS);
        } else if (Editor_SurfFilter(drawFlags, material)) {
            Editor_CheckSubmittedOpaqueDepth(material, techType);
            editorSurf_sub *v5 = &radiant_surfs[radiant_surfCount++];
            v5->material  = material;
            v5->techType  = techType;
            // The binary computed 0x64 * ((drawSurf >> 29) & 0xFFF); kisak's packed layout differs.
            v5->sortKey   = 0x64 * Material_FromHandle(material)->info.drawSurf.fields.primarySortKey;
            v5->skinnedSurf = surf;
            v5->placement = (GfxScaledPlacement *)inst;   // model_inst aliases GfxScaledPlacement
                iassert(edSceneGlobals.sceneSurfCount < ARRAY_COUNT( edSceneGlobals.sceneSurfs ));   // r_ed_scene.cpp:536
            editorSurf_s *v6 = &edSceneGlobals.sceneSurfs[edSceneGlobals.sceneSurfCount++];
            v6->mesh_or_surfSub = v5;
            v6->type = ED_SURF_MODEL;
        }
    }
}

// ── per-surf technique fallback + the model/material report ───────────────────
// Editor_AddSurfCmd's gate silently DROPS a surf whose material lacks the requested
// technique, so a ladder demotes first: requested -> UNLIT (4) -> WIREFRAME_SHADED (29).
// A TOGGLE, not a frame-scoped one-shot: R_SortMaterials' reset runs at the TOP of Cam_Draw
// (camwnd.cpp:2667), so a one-shot cleared there would clear itself before it printed.
int g_kiwiModelInfoDump = 0;

namespace {

// Reported (model, material) pairs; asset name POINTERS are stable, so identity comparison
// is enough.  When the table fills, both outputs go quiet.
struct kiwiSurfNote_t { const char *model; const char *material; bool warned; bool dumped; };
kiwiSurfNote_t s_kiwiNotes[256];
int            s_kiwiNoteCount = 0;

// Find-or-add.  Null when the table is full, which both callers read as "stay quiet".
kiwiSurfNote_t *KiwiNoteFind(const char *model, const char *material)
{
    for (int i = 0; i < s_kiwiNoteCount; ++i)
        if (s_kiwiNotes[i].model == model && s_kiwiNotes[i].material == material)
            return &s_kiwiNotes[i];
    if (s_kiwiNoteCount == (int)ARRAY_COUNT(s_kiwiNotes))
        return 0;
    kiwiSurfNote_t *n = &s_kiwiNotes[s_kiwiNoteCount++];
    n->model    = model;
    n->material = material;
    n->warned   = false;
    n->dumped   = false;
    return n;
}

bool KiwiWarnOnce(const char *model, const char *material)
{
    kiwiSurfNote_t *n = KiwiNoteFind(model, material);
    if (!n || n->warned)
        return false;
    n->warned = true;
    return true;
}

// The same presence test Editor_AddSurfCmd's gate makes, so the ladder can never pick a rung
// the gate would then drop.
bool KiwiTechPresent(const Material *m, int tech)
{
    if (tech >= 34)
        return true;
    return m && m->techniqueSet && m->techniqueSet->techniques[tech] != 0;
}

// The loader injects this stencil-aware technique into an absent sun slot when the
// techset has FAKELIGHT_NORMAL.  If the small technique asset itself is unavailable,
// the direct slot-24 fallback below still needs the same sun-colour vertex stamp.
bool KiwiModelNeedsSunFallbackColor(Material *handle)
{
    const Material *m = handle ? Material_FromHandle(handle) : 0;
    if (!m || !m->techniqueSet)
        return false;
    MaterialTechnique *sun = m->techniqueSet->techniques[TECHNIQUE_SUNLIGHT_PREVIEW];
    if (sun)
        return sun->name && !_stricmp(sun->name, "kiwi_sun_fakelight");
    return m->techniqueSet->techniques[TECHNIQUE_FAKELIGHT_NORMAL] != 0;
}

// The colorMap texdef — semantic 2, the value R_OverrideImage switches on (r_shade.cpp:326).
const GfxImage *KiwiColorMap(const Material *m)
{
    if (!m || !m->textureTable)
        return 0;
    for (int i = 0; i < (int)m->textureCount; ++i)
        if (m->textureTable[i].semantic == 2)
            return m->textureTable[i].u.image;
    return 0;
}

const char *KiwiStr(const char *s) { return (s && *s) ? s : "?"; }

// Material_IsDefault dereferences rgp.defaultMaterial behind an iassert that is empty in
// release (r_material.cpp:481-490), so the null case is guarded HERE.
bool KiwiIsDefaultMaterial(const Material *m)
{
    return m && rgp.defaultMaterial && Material_IsDefault(m);
}

bool KiwiDumpOnce(const char *model, const char *material)
{
    kiwiSurfNote_t *n = KiwiNoteFind(model, material);
    if (!n || n->dumped)
        return false;
    n->dumped = true;
    return true;
}

} // namespace

// Resolve the technique one model surface is queued at, reporting anything worth reporting.
static int Editor_ModelSurfTech(const XModel *xmodel, Material *handle, int surfIndex, int techType)
{
    const Material *m = handle ? Material_FromHandle(handle) : 0;
    if (!m)
        return techType;

    const char *mdlName = KiwiStr(xmodel ? xmodel->name : 0);
    const char *mtlName = KiwiStr(m->info.name);
    const char *tsName  = KiwiStr(m->techniqueSet ? m->techniqueSet->name : 0);

    int use = techType;
    if (!KiwiTechPresent(m, techType)) {
        if (KiwiLight_KeepMissingTechnique(techType)) return techType; // KIWI-UX: Keep additive light passes out of the UNLIT fallback ladder so the existing queue gate skips them.
        if (techType == TECHNIQUE_SUNLIGHT_PREVIEW) {
            // KIWI-UX: a missing sun contribution must never demote to flat UNLIT grey.
            // Prefer the requested N.L fallback; if even that is absent, leave 26 in place
            // so Editor_AddSurfCmd drops only the sun contribution and preserves the
            // already-rasterised textured ambient base.
            use = KiwiTechPresent(m, TECHNIQUE_FAKELIGHT_NORMAL)
                ? TECHNIQUE_FAKELIGHT_NORMAL : -1;
        } else {
            static const int kLadder[2] = { 4 /*TECHNIQUE_UNLIT*/, 29 /*TECHNIQUE_WIREFRAME_SHADED*/ };
            use = -1;
            for (int i = 0; i < 2; ++i) {
                if (kLadder[i] != techType && KiwiTechPresent(m, kLadder[i])) { use = kLadder[i]; break; }
            }
        }
        if (KiwiWarnOnce(mdlName, mtlName)) {
            if (use < 0)
                Sys_Printf("KIWI model \"%s\" surf %i: material \"%s\" [techset \"%s\"] has no "
                           "technique %i and no fallback - the surface is DROPPED (invisible).\n",
                           mdlName, surfIndex, mtlName, tsName, techType);
            else
                Sys_Printf("KIWI model \"%s\" surf %i: material \"%s\" [techset \"%s\"] has no "
                           "technique %i - drawing at %i instead.\n",
                           mdlName, surfIndex, mtlName, tsName, techType, use);
        }
        if (use < 0)
            use = techType;              // let Editor_AddSurfCmd's own gate drop it
    }
    else if (KiwiIsDefaultMaterial(m) && KiwiWarnOnce(mdlName, mtlName)) {
        // A $default clone (r_material.cpp:566-569): this line and the CHECKERBOARD are one event.
        Sys_Printf("KIWI model \"%s\" surf %i: material \"%s\" IS THE $default FALLBACK "
                   "(the asset was not found) - it draws as the 16x16 checkerboard.\n",
                   mdlName, surfIndex, mtlName);
    }

    if (g_kiwiModelInfoDump && KiwiDumpOnce(mdlName, mtlName)) {
        const GfxImage *cm = KiwiColorMap(m);   // only walked when the dump asks for it
        Sys_Printf("  %-34s surf %-2i  \"%s\"\n", mdlName, surfIndex, mtlName);
        Sys_Printf("      techset \"%s\"   tech %i%s   sortKey %i\n",
                   tsName, use, KiwiTechPresent(m, use) ? "" : " (ABSENT - will be dropped)",
                   (int)m->info.sortKey);
        if (cm)
            Sys_Printf("      colorMap \"%s\"  %ix%i%s\n", KiwiStr(cm->name),
                       (int)cm->width, (int)cm->height,
                       KiwiIsDefaultMaterial(m) ? "   ($default clone - ASSET NOT FOUND)" : "");
        else
            Sys_Printf("      colorMap (none declared)\n");
    }
    return use;
}

// "KiwiModelInfo" — TOGGLE the dump (see g_kiwiModelInfoDump for why it is not a one-shot).
void KiwiEdScene_ArmModelInfoDump()
{
    if (g_kiwiModelInfoDump) {
        g_kiwiModelInfoDump = 0;
        Sys_Printf("--- KiwiModelInfo: OFF ---\n");
        return;
    }
    g_kiwiModelInfoDump = 1;
    s_kiwiNoteCount     = 0;             // a fresh arm re-lists (and re-warns) everything
    Sys_Printf("--- KiwiModelInfo: ON.  Every model surface prints as it is drawn; each\n"
               "    (model, material) pair prints ONCE.  Run it again to stop. ---\n");
}

// 0x4FE2E0  SkinModelInst — build + queue every surface of a placed model instance.
// instanceHandle is 1-based into edMapGlobals.modelInst[]; checkhandle overrides the material.
void __cdecl SkinModelInst(int instanceHandle, Material *checkhandle, int techType,
                           const int *colorPtr, int drawFlags)
{
    iassert(instanceHandle > 0 && instanceHandle <= edMapGlobals.modelInstMax);
    model_inst *mi = &edMapGlobals.modelInst[instanceHandle - 1];
    iassert(edMapGlobals.modelInst[instanceHandle - 1].inuse);   // r_ed_scene.cpp:619

    ++s_edModelInsts;

    unsigned numsurfs    = (unsigned)GetXmodelNumSurfs(mi->model, 0);
    Material **modelMaterial = GetXmodelMaterialHandle(mi->model, 0);
    iassert(modelMaterial);

    // The wireframe stamp writes every surface.  The sun stamp is narrower: only
    // XModel surfaces using the injected/direct fakelight fallback need a writable copy.
    // A native-only instance stays in the static model cache; a mixed instance is copied
    // once, but only its fallback surfaces are stamped below.
    bool needWritable = colorPtr && techType == TECHNIQUE_WIREFRAME_SHADED;
    if (colorPtr && techType == TECHNIQUE_SUNLIGHT_PREVIEW) {
        for (unsigned i = 0; i < numsurfs && !needWritable; ++i) {
            Material *useMat = checkhandle
                             ? (Material *)Material_FromHandle(checkhandle)
                             : modelMaterial[i];
            needWritable = KiwiModelNeedsSunFallbackColor(useMat);
        }
    }
    GfxModelSkinnedSurface *skinned = AddModelSurfBuf(mi->model, needWritable);
    if (!skinned)
        return;

    for (unsigned i = 0; i < numsurfs; ++i) {
        // KIWI: shadow-map proxy surfaces (techset "mc_shadowcaster") are invisible in
        // every game colour pass; drawing them here rendered the coarse proxy volume
        // as a dark blob over foliage models.  Same filter as the geometry extraction.
        extern bool Editor_XModelSurfIsShadowProxy(XModel *model, int lod0SurfIndex);   // r_xsurface.cpp
        if (Editor_XModelSurfIsShadowProxy(mi->model, (int)i))
            continue;
        GfxModelSkinnedSurface *skinnedSurf = &skinned[i];   // the binary's local (assert strings)
        Material *material = modelMaterial[i];
        iassert(material);
        Material *useMat = checkhandle ? (Material *)Material_FromHandle(checkhandle) : material;
        // KIWI-UX: native tech-26 model materials retain their authored vertex colour.
        // Only wireframe or the fakelight sun fallback receives the supplied stamp.
        const bool stampColor = colorPtr
            && ( techType == TECHNIQUE_WIREFRAME_SHADED
              || ( techType == TECHNIQUE_SUNLIGHT_PREVIEW
                && KiwiModelNeedsSunFallbackColor(useMat) ) );
        if (stampColor) {
            // Record the override AND write *colorPtr over each vert's
            // colour (@+0x10, stride 0x20).  Only ever the tempSkinBuf COPY, never verts0.
            mi->colorOverride = *colorPtr;
            iassert(skinnedSurf->skinnedCachedOffset != RIGID_SKINNED_CACHE_OFFSET);   // r_ed_scene.cpp:639
            iassert(skinnedSurf->skinnedCachedOffset != HIDDEN_SURFACE_OFFSET);        // r_ed_scene.cpp:640
            const int nv  = XSurfaceGetNumVerts(skinned[i].xsurf);
            uint8_t  *base = (uint8_t *)skinned[i].skinnedVert;
            for (int vi = 0; vi < nv; ++vi)
                *(uint *)(base + 32 * vi + 0x10) = (uint)*colorPtr;
        } else {
            mi->colorOverride = -1;
        }
        // KIWI: DEMOTE rather than vanish; returns techType unchanged in the normal case.
        const int surfTech = Editor_ModelSurfTech(mi->model, useMat, (int)i, techType);
        Editor_AddSurfCmd(drawFlags, useMat, mi, &skinned[i], surfTech);
        iassert(skinnedSurf->skinnedCachedOffset != RIGID_SKINNED_CACHE_OFFSET);   // r_ed_scene.cpp:656
        iassert(skinnedSurf->skinnedCachedOffset != HIDDEN_SURFACE_OFFSET);        // r_ed_scene.cpp:657
    }
}

// ── KIWI (2026-10-29): a model instance for the retained scene (kiwi_edmodel.h) ─────────
// SkinModelInst's selection without its per-frame buffers: an unstamped surface draws the
// asset's own verts0 (AddSurfTempSkinBuf), so the scene can hold them in world space.
static model_inst *Editor_LiveModelInst(int instanceHandle)
{
    if (instanceHandle <= 0 || instanceHandle > edMapGlobals.modelInstMax)
        return nullptr;
    model_inst *mi = &edMapGlobals.modelInst[instanceHandle - 1];
    return mi->inuse && mi->model ? mi : nullptr;
}

int Editor_ModelInstSurfaces(int instanceHandle, int techType, edModelSurf_t *out, int maxOut)
{
    model_inst *mi = Editor_LiveModelInst(instanceHandle);
    if (!mi || XModelBad(mi->model))
        return 0;
    XSurface *surfBase = nullptr;
    const int surfaceCount = XModelGetSurfaces(mi->model, &surfBase, 0);
    const int numsurfs     = GetXmodelNumSurfs(mi->model, 0);
    Material **modelMaterial = GetXmodelMaterialHandle(mi->model, 0);
    if (!surfBase || !modelMaterial || numsurfs > surfaceCount)
        return 0;
    for (int i = 0; i < surfaceCount; ++i)
        if (!surfBase[i].verts0)
            return 0;                         // AddModelSurfBuf drops the whole model
    extern bool Editor_XModelSurfIsShadowProxy(XModel *model, int lod0SurfIndex);   // r_xsurface.cpp
    int n = 0;
    for (int i = 0; i < numsurfs && n < maxOut; ++i) {
        if (Editor_XModelSurfIsShadowProxy(mi->model, i))
            continue;
        Material *material = modelMaterial[i];
        if (!material)
            continue;
        const int surfTech = Editor_ModelSurfTech(mi->model, material, i, techType);
        // Editor_AddSurfCmd's gate (drawFlags 0 passes Editor_SurfFilter).
        if (surfTech < TECHNIQUE_COUNT && !material->techniqueSet->techniques[surfTech])
            continue;
        const XSurface *xsurf = &surfBase[i];
        edModelSurf_t &s = out[n++];
        s.material   = material;
        s.techType   = surfTech;
        s.sortKey    = Editor_MaterialSortKey(material);
        s.xsurf      = xsurf;
        s.vertCount  = XSurfaceGetNumVerts(xsurf);
        s.indices    = xsurf->triIndices;
        s.indexCount = 3 * XSurfaceGetNumTris(xsurf);
    }
    return n;
}

bool Editor_ModelInstPlacement(int instanceHandle, float out[8])
{
    const model_inst *mi = Editor_LiveModelInst(instanceHandle);
    if (!mi)
        return false;
    memcpy(out, mi->angles, 4 * sizeof(float));
    memcpy(out + 4, mi->origin, 3 * sizeof(float));
    out[7] = mi->modelscale;
    return true;
}

extern void __cdecl SetupTransformUnitVec(const float4 *mtx, int (*fixedMtx)[3]);
extern PackedUnitVec __cdecl LocalTransformUnitVec(PackedUnitVec in, const int (*fixedMtx)[3]);

// The merged-instance transform (kiwi_instcache.cpp IC_Transform): positions by rotation *
// scale + the world origin, normals and tangents by rotation alone.
void Editor_ModelInstTransform(int instanceHandle, const XSurface *xsurf, void *outVerts)
{
    const model_inst *mi = Editor_LiveModelInst(instanceHandle);
    if (!mi || !xsurf || !xsurf->verts0)
        return;
    mat3x3 axis;
    UnitQuatToAxis(mi->angles, axis);
    float4 normAxis[4];
    float4 useAxis[4];
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
            normAxis[r].v[k] = axis[r][k];
            useAxis[r].v[k]  = mi->modelscale * axis[r][k];
        }
        normAxis[r].v[3] = 0.0f;
        useAxis[r].v[3]  = 0.0f;
    }
    normAxis[3].v[0] = normAxis[3].v[1] = normAxis[3].v[2] = normAxis[3].v[3] = 0.0f;
    useAxis[3].v[0] = mi->origin[0];
    useAxis[3].v[1] = mi->origin[1];
    useAxis[3].v[2] = mi->origin[2];
    useAxis[3].v[3] = 0.0f;
    int fixedNormAxis[3][3];
    SetupTransformUnitVec(normAxis, fixedNormAxis);

    const GfxPackedVertex *src = xsurf->verts0;
    GfxPackedVertex       *out = (GfxPackedVertex *)outVerts;
    const int vertCount = XSurfaceGetNumVerts(xsurf);
    for (int v = 0; v < vertCount; ++v) {
        R_TransformSkelMat(src[v].xyz, (const DObjSkelMat *)useAxis, out[v].xyz);
        out[v].binormalSign    = src[v].binormalSign;
        out[v].color.packed    = src[v].color.packed;
        out[v].texCoord.packed = src[v].texCoord.packed;
        out[v].normal          = LocalTransformUnitVec(src[v].normal, fixedNormAxis);
        out[v].tangent         = LocalTransformUnitVec(src[v].tangent, fixedNormAxis);
    }
}

int Editor_ModelVertexStride() { return (int)sizeof(GfxPackedVertex); }
int Editor_ModelVertexDecl()   { return VERTDECL_PACKED; }

// ── backend draw (mesh branch only) ───────────────────────────────────────────

// 0x4FE500  Editor_DrawIndexedPrimitive — the editor's OWN untracked indexed draw.  It must
// NOT use the game's R_DrawIndexedPrimitive, whose RB_TrackDrawPrimCall asserts on
// g_primStats — which no editor frame ever sets (int3 0xC0000409).
static void Editor_DrawIndexedPrimitive(GfxCmdBufPrimState *state, const GfxDrawPrimArgs *args)
{
    IDirect3DDevice9 *device = state->device;
    iassert(device);

    HRESULT hr = device->DrawIndexedPrimitive(
        D3DPT_TRIANGLELIST, 0, 0, args->vertexCount, args->baseIndex, args->triCount);
    if (hr < 0) {
        ++g_disableRendering;
        FatalError(0, "r_ed_scene: DrawIndexedPrimitive( %i verts, %i tris ) failed: %s\n",
            args->vertexCount, args->triCount, R_ErrorDescription(hr));
    }
}

// KIWI-UX: sorting is only a batching decision for opaque editor fills.  R_SetupPass
// faithfully installs the material pass at IDB 0x53C520-0x53C590; this editor-only guard
// then repairs an unsafe opaque asset to the camera invariant before any primitive is drawn.
// Intentional wireframe/sky/blend passes are rejected by Editor_IsOpaqueFillPass.
static void Editor_ForceOpaqueFillDepth()
{
    const Material *material = gfxCmdBufState.material;
    const uint stateBits0 = gfxCmdBufState.refStateBits[0];
    if (!Editor_IsOpaqueFillPass(material, gfxCmdBufState.techType, stateBits0))
        return;

    const uint oldDepth = gfxCmdBufState.refStateBits[1];
    const uint newDepth = (oldDepth & ~(GFXS1_DEPTHWRITE
                                     | GFXS1_DEPTHTEST_DISABLE
                                     | GFXS1_DEPTHTEST_MASK))
                        | GFXS1_DEPTHWRITE | GFXS1_DEPTHTEST_LESSEQUAL;
    if (newDepth == oldDepth)
        return;

    uint corrected[2] = { stateBits0, newDepth };
    R_SetState(&gfxCmdBufState, corrected);
}

// 0x4FE5A0  R_DrawTessTechnique_Brushes — run the bound technique's passes over the
// accumulated tess indices, through the editor's own untracked Editor_DrawIndexedPrimitive.
static void R_DrawTessTechnique_Brushes(const GfxDrawPrimArgs *args)
{
    iassert(dx.d3d9 && dx.device);
    iassert(gfxCmdBufState.material);   // r_ed_scene.cpp:555
    const MaterialTechnique *technique = gfxCmdBufState.technique;
    iassert(technique);

    for (uint passIndex = 0; passIndex < technique->passCount; ++passIndex) {
        R_SetupPass(gfxCmdBufContext, passIndex);
        Editor_ForceOpaqueFillDepth();
        R_UpdateVertexDecl(&gfxCmdBufState);
        R_SetupPassCriticalPixelShaderArgs(gfxCmdBufContext);
        R_SetupPassPerObjectArgs(gfxCmdBufContext);
        R_SetupPassPerPrimArgs(gfxCmdBufContext);

        Editor_DrawIndexedPrimitive(&gfxCmdBufState.prim, args);
    }
}

// Defined below: the brush-mesh tess draw can drive R_SetupPass behind the model path's back.
static void Editor_InvalidatePassKey();

// 0x4FE690  RB_DrawTessSurface (editor-local; distinct from kisak's parameterless one).
// Draws the accumulated indices over the bound world stream for verts [minVert, maxVert].
static void RB_DrawEditorTessSurface(uint16_t minVert, uint16_t maxVert)
{
    GfxDrawPrimArgs args;
    tess.finishedFilling = 1;
    if (gfxCmdBufSourceState.viewportIsDirty) {
        GfxViewport vp;
        R_GetViewport(&gfxCmdBufSourceState, &vp);
        R_SetViewport(&gfxCmdBufState, &vp);
        R_UpdateViewport(&gfxCmdBufSourceState, &vp);
    }
    // MinVertexIndex is hardwired to 0, so vertexCount must span all referenced verts
    // [0, maxVert].  (The binary used minVert/range + an indexCount field kisak lacks.)
    args.baseIndex   = R_SetIndexData(&gfxCmdBufState.prim, (uint8_t *)tess.indices, tess.indexCount / 3);
    args.vertexCount = maxVert + 1;          // NumVertices (verts span [0, maxVert])
    args.triCount    = tess.indexCount / 3;  // PrimitiveCount (triangles)
    // Runs its OWN R_SetupPass loop, so the pass key can no longer vouch for
    // gfxCmdBufState.pass.  Stale BEFORE the call, so an early return cannot leave a lie.
    Editor_InvalidatePassKey();
    R_DrawTessTechnique_Brushes(&args);
    ++s_edMeshTessDraws;   // one index UPLOAD, counted at the source
    tess.indexCount = 0;
    tess.finishedFilling = 0;
}

// Reset the active world matrix to eye-relative identity (world verts are in world space;
// the camera-relative offset lives in eyeOffset).  The active matrix is matrix[0].
static void Editor_SetEyeRelativeWorldMatrix()
{
    GfxCmdBufSourceState *src = R_GetActiveWorldMatrix(&gfxCmdBufSourceState);
    float (*m)[4] = src->matrices.matrix[0].m;
    R_MatrixIdentity44(m);
    m[3][0] -= gfxCmdBufSourceState.eyeOffset[0];
    m[3][1] -= gfxCmdBufSourceState.eyeOffset[1];
    m[3][2] -= gfxCmdBufSourceState.eyeOffset[2];
    // KIWI: belongs HERE, on the line that clobbers matrix[0], NOT in the model arm — an
    // unconditional reset per model surf kills that arm's guard and rebuilds per surface.
    gfxCmdBufSourceState.objectPlacement = 0;
}

// Not in the binary: re-apply every `def cN, x,y,z,w` immediate in a vertex shader's
// bytecode — the editor _dtex shaders decompress packed UBYTE4 texcoords from def constants
// (c8..c12) that live there, not in any engine constant.  THE PARSE IS CACHED; THE ISSUE IS
// NOT: R_SetupPass' args write the same constant file, so they must be re-issued per pass.
struct EdVsDefConst { uint reg; float v[4]; };
struct EdVsDefEntry
{
    const MaterialVertexShader *vs;
    int                         count;      // -1 = too many defs to cache: parse live
    EdVsDefConst                defs[16];
};
static EdVsDefEntry  s_edVsDefs[64];
static int           s_edVsDefCount;
static EdVsDefEntry *s_edVsDefMru;

static void Editor_ScanVsDefConstants(const MaterialVertexShader *vs, EdVsDefEntry *out);

// No invalidation hook, deliberately: the key is the SHADER ASSET pointer and the value comes
// purely from its bytecode, so nothing can make an entry wrong.  Bounded at 64 shaders.
static const EdVsDefEntry *Editor_GetVsDefConstants(const MaterialVertexShader *vs)
{
    if (s_edVsDefMru && s_edVsDefMru->vs == vs)
        return s_edVsDefMru;
    for (int i = 0; i < s_edVsDefCount; ++i) {
        if (s_edVsDefs[i].vs == vs) {
            s_edVsDefMru = &s_edVsDefs[i];
            return s_edVsDefMru;
        }
    }
    if (s_edVsDefCount == (int)ARRAY_COUNT(s_edVsDefs))
        return nullptr;                       // table full -> caller parses live
    EdVsDefEntry *e = &s_edVsDefs[s_edVsDefCount++];
    e->vs = vs;
    Editor_ScanVsDefConstants(vs, e);
    s_edVsDefMru = e;
    return e;
}

// The original walk, now called at most once per shader (live when the cache cannot hold it).
static void Editor_ForceVsDefConstants(IDirect3DDevice9 *dev, const MaterialVertexShader *vs)
{
    if (!vs || !vs->prog.loadDef.program)
        return;
    const EdVsDefEntry *cached = Editor_GetVsDefConstants(vs);
    if (cached && cached->count >= 0) {
        for (int i = 0; i < cached->count; ++i)
            dev->SetVertexShaderConstantF(cached->defs[i].reg, cached->defs[i].v, 1);
        s_edVsConstUploads += cached->count;
        return;
    }
    const uint *tok = (const uint *)vs->prog.loadDef.program;
    unsigned n = vs->prog.loadDef.programSize;   // dwords
    unsigned i = 1;                              // skip version token
    while (i < n) {
        uint t = tok[i];
        if (t == 0x0000FFFF)                     // end token
            break;
        if ((t & 0xFFFF) == 0xFFFE) {            // comment (CTAB etc): length in bits 16..30
            i += ((t >> 16) & 0x7FFF) + 1;
            continue;
        }
        uint op = t & 0xFFFF;
        if (op == 0x51) {                        // D3DSIO_DEF: dst tok + 4 raw float dwords
            uint dst = tok[i + 1] & 0x7FF;
            dev->SetVertexShaderConstantF(dst, (const float *)&tok[i + 2], 1);
            ++s_edVsConstUploads;
            i += 6;
            continue;
        }
        if (op == 0x30) { i += 6; continue; }    // D3DSIO_DEFI: dst + 4 ints
        if (op == 0x2F) { i += 3; continue; }    // D3DSIO_DEFB: dst + 1 bool
        ++i;                                     // other instruction: skip its param tokens
        while (i < n && (tok[i] & 0x80000000))
            ++i;
    }
}

// The same walk, recording instead of issuing.  The two token decodes MUST agree.
static void Editor_ScanVsDefConstants(const MaterialVertexShader *vs, EdVsDefEntry *out)
{
    out->count = 0;
    if (!vs || !vs->prog.loadDef.program)
        return;
    const uint *tok = (const uint *)vs->prog.loadDef.program;
    unsigned n = vs->prog.loadDef.programSize;   // dwords
    unsigned i = 1;                              // skip version token
    while (i < n) {
        uint t = tok[i];
        if (t == 0x0000FFFF)
            break;
        if ((t & 0xFFFF) == 0xFFFE) {
            i += ((t >> 16) & 0x7FFF) + 1;
            continue;
        }
        uint op = t & 0xFFFF;
        if (op == 0x51) {
            if (out->count == (int)ARRAY_COUNT(out->defs)) {
                out->count = -1;                 // more defs than the cache holds: parse live
                return;
            }
            EdVsDefConst *d = &out->defs[out->count++];
            d->reg = tok[i + 1] & 0x7FF;
            memcpy(d->v, &tok[i + 2], sizeof(d->v));
            i += 6;
            continue;
        }
        if (op == 0x30) { i += 6; continue; }
        if (op == 0x2F) { i += 3; continue; }
        ++i;
        while (i < n && (tok[i] & 0x80000000))
            ++i;
    }
}

// ── the pass-setup key ────────────────────────────────────────────────────────
// Only R_SetupPassPerObjectArgs depends on the surface; the rest is a function of (material,
// technique, passIndex, vertDeclType), so it runs once per run.  Skipping the def constants
// with it is safe: they are compiler-allocated literals disjoint from the CTAB-bound uniforms
// (r_shade.cpp:376-377).  MUST be invalidated wherever something else drives R_SetupPass.
struct EdPassKey
{
    const Material          *material;
    const MaterialTechnique *technique;
    uint                     passIndex;
    int                      vertDeclType;
    IDirect3DDevice9        *device;
};
static EdPassKey s_edPassKey;
static bool      s_edPassKeyValid;

static void Editor_InvalidatePassKey()
{
    s_edPassKeyValid = false;
}

// Make pass `pass` current.  True when the FULL setup ran — the caller's signal to re-issue
// the def constants AFTER the per-object/per-prim args (the original order).
static bool Editor_BeginModelPass(uint pass)
{
    const MaterialTechnique *technique = gfxCmdBufState.technique;
    IDirect3DDevice9        *device    = gfxCmdBufState.prim.device;
    if (s_edPassKeyValid &&
        s_edPassKey.material     == gfxCmdBufState.material &&
        s_edPassKey.technique    == technique &&
        s_edPassKey.passIndex    == pass &&
        s_edPassKey.vertDeclType == gfxCmdBufState.prim.vertDeclType &&
        s_edPassKey.device       == device)
    {
        ++s_edPassSetupsSkip;
        return false;
    }
    R_SetupPass(gfxCmdBufContext, pass);
    Editor_ForceOpaqueFillDepth();
    R_UpdateVertexDecl(&gfxCmdBufState);
    R_SetupPassCriticalPixelShaderArgs(gfxCmdBufContext);
    ++s_edPassSetups;
    s_edPassKey.material     = gfxCmdBufState.material;
    s_edPassKey.technique    = technique;
    s_edPassKey.passIndex    = pass;
    s_edPassKey.vertDeclType = gfxCmdBufState.prim.vertDeclType;
    s_edPassKey.device       = device;
    s_edPassKeyValid         = true;
    return true;
}

// 0x53AA30  R_DrawXModelSkinnedUncached_2 — the EDITOR's xmodel skinned draw
// (VERTDECL_PACKED, stride 32).  The binary uses R_DrawIndexedPrimitive_R (0x538990,
// untracked), which kisak lacks; Editor_DrawIndexedPrimitive emits the identical call.
// 0x53AA30 has NO R_CheckVertexDataOverflow — that is the GAME variant only.
static void Editor_DrawXModelSkinnedUncached(XSurface *xsurf, GfxPackedVertex *skinnedVert)
{
    iassert(xsurf);                    // rb_shade.cpp:383-386
    iassert(skinnedVert);
    iassert(tess.indexCount == 0);
    iassert(tess.vertexCount == 0);

    // The binary copies the IB via R_CheckTris(xsurf, tess.indices, 0), a plain memcpy of
    // triIndices; kisak has none, and reading triIndices directly is byte-identical.

    // `skinnedVert == xsurf->verts0` is exactly the no-stamp case — verts read-only for the
    // session, so the pool can hold a MANAGED copy (kiwi_modelcache.h).  Tested on the DATA.
    KiwiModelGeo geo = {};
    const bool rigidAlias = ( skinnedVert == (GfxPackedVertex *)xsurf->verts0 );
    const bool cached     = rigidAlias && KiwiModelCache_Get( xsurf, &geo );
    if (cached)
        ++s_edModelSurfsCached;
    else
        ++s_edModelSurfsDynamic;

    if (gfxCmdBufSourceState.viewportIsDirty) {
        GfxViewport vp;
        R_GetViewport(&gfxCmdBufSourceState, &vp);
        R_SetViewport(&gfxCmdBufState, &vp);
        R_UpdateViewport(&gfxCmdBufSourceState, &vp);
    }

    GfxDrawPrimArgs args;
    args.vertexCount = XSurfaceGetNumVerts(xsurf);
    args.triCount    = XSurfaceGetNumTris(xsurf);

    vassert(gfxCmdBufState.prim.vertDeclType == VERTDECL_PACKED, "(gfxCmdBufState.prim.vertDeclType) = %i",
            gfxCmdBufState.prim.vertDeclType);   // rb_shade.cpp:405

    // Both arms leave prim.indexBuffer correct, so a later R_SetIndexData rebinds the
    // dynamic IB by its own rule (r_shade.cpp:69-70).
    if (cached) {
        if (gfxCmdBufState.prim.indexBuffer != geo.ib)
            R_ChangeIndices(&gfxCmdBufState.prim, geo.ib);
        args.baseIndex = (int)geo.startIndex;
    } else {
        args.baseIndex = R_SetIndexData(&gfxCmdBufState.prim, (uint8_t *)xsurf->triIndices, args.triCount);
    }

    iassert(gfxCmdBufState.technique);   // rb_shade.cpp:409

    // Cached: the pool chunk at this surface's byte offset (r_draw_staticmodel.cpp:215-218).
    // MinVertexIndex/BaseVertexIndex stay 0, so 0-based triIndices still address own verts.
    uint vertexOffset;
    IDirect3DVertexBuffer9 *vb;
    if (cached) {
        vertexOffset = geo.vertexOffsetBytes;
        vb           = geo.vb;
    } else {
        vertexOffset = (uint)R_SetVertexData(&gfxCmdBufState, skinnedVert, args.vertexCount, 32);
        vb           = gfxBuf.dynamicVertexBuffer->buffer;
        s_edModelUploadBytes += 32 * args.vertexCount;
    }
    iassert(vb);   // rb_shade.cpp:412
    if (gfxCmdBufState.prim.streams[0].vb != vb || gfxCmdBufState.prim.streams[0].offset != vertexOffset ||
        gfxCmdBufState.prim.streams[0].stride != 32) {
        R_ChangeStreamSource(&gfxCmdBufState.prim, 0, vb, vertexOffset, 32);
        ++s_edStreamSwitches;
    }
    if (gfxCmdBufState.prim.streams[1].vb || gfxCmdBufState.prim.streams[1].offset || gfxCmdBufState.prim.streams[1].stride)
        R_ChangeStreamSource(&gfxCmdBufState.prim, 1, 0, 0, 0);

    for (uint pass = 0; pass < gfxCmdBufState.technique->passCount; ++pass) {
        // The invariant half, behind the pass key.  Def constants still go out AFTER the
        // per-object args when the full setup ran — the original order.
        const bool fullSetup = Editor_BeginModelPass(pass);
        R_SetupPassPerObjectArgs(gfxCmdBufContext);
        R_SetupPassPerPrimArgs(gfxCmdBufContext);
        if (fullSetup) {
            IDirect3DDevice9 *dev = gfxCmdBufState.prim.device;
            const MaterialPass *p = &gfxCmdBufState.technique->passArray[pass];
            if (dev)
                Editor_ForceVsDefConstants(dev, p->vertexShader);
        }
        Editor_DrawIndexedPrimitive(&gfxCmdBufState.prim, &args);   // editor-untracked draw
        ++s_edDrawCalls;
    }
}

// ── many instances of one surface, one draw call ──────────────────────────────
// The editor's port of R_DrawStaticModelsCachedDrawSurf (r_draw_staticmodel.cpp:255-282):
// the instances' vertices are already in world space (kiwi_instcache.cpp), so their index
// runs CONCATENATE under the eye-relative world matrix — no object placement at all.  A run
// is a maximal span of CONSECUTIVE sorted entries, so it never crosses a sortKey bucket.
// ED_MERGE_MAX_TRIS keeps 3 x triCount inside the dynamic IB (r_buffers.cpp:211).
#define ED_MERGE_MAX_INSTANCES 1024
#define ED_MERGE_MAX_TRIS      60000
static KiwiInstGeo s_edMergeGeo[ED_MERGE_MAX_INSTANCES];
// Gathered once so the whole concatenation takes ONE index-buffer lock (R_SetIndexDataRuns).
// File scope, like s_edMergeGeo: single-threaded and non-reentrant, so 8 KB off the stack.
static const uint16_t *s_edRunPtrs[ED_MERGE_MAX_INSTANCES];
static int             s_edRunTris[ED_MERGE_MAX_INSTANCES];

// Kill switch behind the "KiwiInstBatch" command (kiwi_command.h:KIWI_CMD_INSTBATCH): OFF
// puts every model surf back on the per-instance path, so a regression is an A/B.
static int s_edInstBatching = 1;

void KiwiEdScene_ToggleInstBatching()
{
    s_edInstBatching = !s_edInstBatching;
    Sys_Printf("--- KIWI instance batching: %s ---\n", s_edInstBatching ? "ON" : "OFF (round BY3 per-instance path)");
}

static int Editor_DrawMergedInstanceRun(const editorSurf_s *surfs, int first, int avail)
{
    PROF_SCOPED( "merged instance run" );   // KIWI 2026-09-10: was invisible in Tracy
    if (!s_edInstBatching)
        return 0;
    if (!gfxCmdBufState.technique)
        return 0;                       // no bound technique -> nothing this path can do
    const editorSurf_sub *lead = (const editorSurf_sub *)surfs[first].mesh_or_surfSub;
    if (!lead->skinnedSurf || !lead->skinnedSurf->xsurf || !lead->placement)
        return 0;
    const XSurface *xsurf = lead->skinnedSurf->xsurf;
    // Tested on the DATA, not a flag: a stamped copy is not what the instance cache holds.
    if (lead->skinnedSurf->skinnedVert != (GfxPackedVertex *)xsurf->verts0)
        return 0;
    if (!KiwiInstCache_Get(lead->placement, xsurf, &s_edMergeGeo[0]))
        return 0;

    int      count    = 1;
    unsigned totalTris = s_edMergeGeo[0].triCount;
    while (count < avail && count < ED_MERGE_MAX_INSTANCES) {
        const editorSurf_s *s = &surfs[first + count];
        if (s->type != ED_SURF_MODEL)
            break;
        const editorSurf_sub *sub = (const editorSurf_sub *)s->mesh_or_surfSub;
        if (sub->material != lead->material || sub->techType != lead->techType)
            break;
        if (!sub->skinnedSurf || sub->skinnedSurf->xsurf != xsurf || !sub->placement)
            break;
        if (sub->skinnedSurf->skinnedVert != (GfxPackedVertex *)xsurf->verts0)
            break;
        // geo[0].triCount, not geo[count]'s: a run shares ONE xsurf so the counts are equal,
        // and the test must happen BEFORE the lookup that would fill geo[count].
        if (totalTris + s_edMergeGeo[0].triCount > ED_MERGE_MAX_TRIS)
            break;
        if (!KiwiInstCache_Get(sub->placement, xsurf, &s_edMergeGeo[count]))
            break;
        if (s_edMergeGeo[count].page != s_edMergeGeo[0].page)
            break;
        totalTris += s_edMergeGeo[count].triCount;
        ++count;
    }

    // World-space verts need the eye-relative world matrix, not an object placement — this
    // restore is what makes a merged run legal directly after a per-instance one.
    if (gfxCmdBufSourceState.objectPlacement)
        Editor_SetEyeRelativeWorldMatrix();

    if (gfxCmdBufSourceState.viewportIsDirty) {
        GfxViewport vp;
        R_GetViewport(&gfxCmdBufSourceState, &vp);
        R_SetViewport(&gfxCmdBufState, &vp);
        R_UpdateViewport(&gfxCmdBufSourceState, &vp);
    }

    // The page binds at offset 0 and the draw declares its FULL window, as the game's binder
    // does (r_draw_staticmodel.cpp:215-218) — legal because the page holds 65,536 vertices.
    IDirect3DVertexBuffer9 *vb = s_edMergeGeo[0].vb;
    if (gfxCmdBufState.prim.streams[0].vb != vb || gfxCmdBufState.prim.streams[0].offset ||
        gfxCmdBufState.prim.streams[0].stride != 32) {
        R_ChangeStreamSource(&gfxCmdBufState.prim, 0, vb, 0, 32);
        ++s_edStreamSwitches;
    }
    if (gfxCmdBufState.prim.streams[1].vb || gfxCmdBufState.prim.streams[1].offset || gfxCmdBufState.prim.streams[1].stride)
        R_ChangeStreamSource(&gfxCmdBufState.prim, 1, 0, 0, 0);

    GfxDrawPrimArgs args;
    args.vertexCount = 0x10000;
    args.triCount    = (int)totalTris;
    // ONE LOCK PER RUN: R_SetIndexDataRuns takes the whole span at once (r_shade.cpp), and a
    // failed lock draws nothing rather than garbage.
    {
        for (int k = 0; k < count; ++k) {
            s_edRunPtrs[k] = (const uint16_t *)s_edMergeGeo[k].indices;
            s_edRunTris[k] = (int)s_edMergeGeo[k].triCount;
        }
        args.baseIndex = R_SetIndexDataRuns(&gfxCmdBufState.prim, s_edRunPtrs, s_edRunTris, count);
        if (args.baseIndex < 0) {
            static bool s_reported = false;
            if (!s_reported) {
                s_reported = true;
                Sys_Printf("KIWI merged instance draw: index buffer lock failed - "
                           "%d instances skipped this frame.\n", count);
            }
            return count;
        }
        s_edRunIndexLocks += 1;
        s_edRunIndexRuns  += count;
    }

    for (uint pass = 0; pass < gfxCmdBufState.technique->passCount; ++pass) {
        const bool fullSetup = Editor_BeginModelPass(pass);
        R_SetupPassPerObjectArgs(gfxCmdBufContext);
        R_SetupPassPerPrimArgs(gfxCmdBufContext);
        if (fullSetup) {
            IDirect3DDevice9 *dev = gfxCmdBufState.prim.device;
            const MaterialPass *p = &gfxCmdBufState.technique->passArray[pass];
            if (dev)
                Editor_ForceVsDefConstants(dev, p->vertexShader);
        }
        Editor_DrawIndexedPrimitive(&gfxCmdBufState.prim, &args);
        ++s_edDrawCalls;
    }
    ++s_edMergedDraws;
    s_edMergedInsts    += count;
    s_edModelSurfsCached += count;      // these surfs never reach the per-surf arm
    return count;
}

// ── KIWI-UX — THE MESH-SURF INDEX INVARIANT ─────────────────────────────────
// Every consumer of an editorMesh_s rebases its index table onto the surf's own
// vertex run: `firstIndex + indexTable[k]`, as the tess path in
// RB_DrawEditorSkinnedCached_Sub does.  That is only a rebase if every value in
// indexTable[0 .. indexCount) is already inside [0, vertCount) — the ONE invariant
// the whole editor mesh pipeline rests on, and the one thing nothing checks.
//
// WHAT A VIOLATION LOOKS LIKE ON SCREEN, which is why this matters.  An index at or
// beyond vertCount rebases to a slot BEYOND the surf's run, and the editor packs
// every surf of one material into shared 64 K vertex buffers (r_ed_vertbuf.cpp:38,
// carved per material by Editor_VB_AllocFromPools) — so the offending triangle takes
// one corner from a completely unrelated surface and draws as a thin sliver running
// from this object to wherever that surface happens to sit.  It does not crash, it
// does not warn, and D3D cannot reject it: the tess draw declares
// MinVertexIndex/NumVertices as [minVert, maxVert] over the WHOLE batch, so a stray
// index is still inside the declared range and draws whatever it lands on.
//
// AND IT IS FAR MORE VISIBLE IN LIGHTMAP MODE, without being caused by it.  With
// Material_SetMode 0 each surface uploads under its OWN material (brush.cpp:6078,
// pmesh.cpp:10227), so a material's pool holds only that material's surfaces and a
// stray index lands on a neighbouring surface of the same object — a wrong-looking
// triangle nobody notices.  With Material_SetMode 1 EVERY surface in the map uploads
// under `lightmap_gray`, one pool, so the same stray index lands on whatever else in
// the MAP occupies that slot.  Same bad data, same bad triangle; only its far end
// moves, from "next door" to "across the level".
//
// COST.  One extra pass over a table the tess copy loop walks anyway: one compare per
// index, against a loop that already loads, adds, truncates and stores.
static const int  KIWI_MESHIDX_MAX_REPORTS = 8;
static int        s_kiwiMeshIdxReports = 0;
static const void *s_kiwiMeshIdxLastMtl = nullptr;

// Returns the first k in [0, indexCount) whose index escapes the surf's vertex run,
// or -1 when the surf is sound.  A surf with no table or no indices is sound (the
// callers drop those on their own terms).
static int Editor_MeshSurfBadIndex(const editorMesh_s *m)
{
    const uint16_t *itab = (const uint16_t *)m->indexTable;
    const int ic = (int)m->indexCount;
    const int vc = (int)m->vertCount;
    if (!itab || ic <= 0)
        return -1;
    for (int k = 0; k < ic; ++k)
        if ((int)itab[k] >= vc)
            return k;
    return -1;
}

// Name the producer once per material, capped — the point is to identify what emitted
// the surf, not to fill the console every frame it is submitted.
static void Editor_ReportMeshSurfBadIndex(const editorMesh_s *m, int badK)
{
    if (s_kiwiMeshIdxReports >= KIWI_MESHIDX_MAX_REPORTS || m->material == s_kiwiMeshIdxLastMtl)
        return;
    s_kiwiMeshIdxLastMtl = m->material;
    ++s_kiwiMeshIdxReports;
    const uint16_t *itab = (const uint16_t *)m->indexTable;
    Sys_Printf("Editor mesh surf DROPPED - index out of its own vertex run: material \"%s\", "
               "tech %i, vertCount %i, indexCount %i, indexTable[%i] = %i.  It would have drawn "
               "a stray triangle into unrelated geometry.\n",
               KiwiStr(m->material ? m->material->info.name : 0),
               m->techType, (int)m->vertCount, (int)m->indexCount, badK,
               itab ? (int)itab[badK] : -1);
}

// KIWI: one retained-scene batch - its static index buffer over its VB page, one draw.
static void Editor_DrawBatch( const editorBatch_s *batch )
{
    IDirect3DVertexBuffer9 *vb = nullptr;
    if ( batch->vb )
        vb = KiwiGpu_D3D9VertexBuffer( batch->vb );
    else
    {
        uint16_t firstVert = 0;
        Editor_GetVertexBufferAndIndex( batch->vbPage << 16, &vb, &firstVert );
    }
    IDirect3DIndexBuffer9 *ib = KiwiGpu_D3D9IndexBuffer( batch->ib );
    if ( !vb || !ib || batch->triCount <= 0 )
        return;
    // Runs its OWN R_SetupPass loop, so the pass key can no longer vouch for
    // gfxCmdBufState.pass — stale BEFORE the call, as RB_DrawEditorTessSurface does it.
    Editor_InvalidatePassKey();
    RB_BeginSurface( batch->material, (MaterialTechniqueType)batch->techType );
    ++s_edBeginSurfaces;
    gfxCmdBufState.prim.vertDeclType = (MaterialVertexDeclType)batch->vertDecl;
    if ( gfxCmdBufSourceState.objectPlacement )
        Editor_SetEyeRelativeWorldMatrix();
    if ( gfxCmdBufState.prim.streams[0].vb != vb || gfxCmdBufState.prim.streams[0].offset ||
         gfxCmdBufState.prim.streams[0].stride != (uint)batch->stride ) {
        R_ChangeStreamSource( &gfxCmdBufState.prim, 0, vb, 0, batch->stride );
        ++s_edStreamSwitches;
    }
    if ( gfxCmdBufState.prim.streams[1].vb || gfxCmdBufState.prim.streams[1].offset ||
         gfxCmdBufState.prim.streams[1].stride )
        R_ChangeStreamSource( &gfxCmdBufState.prim, 1, 0, 0, 0 );
    if ( gfxCmdBufState.prim.indexBuffer != ib )
        R_ChangeIndices( &gfxCmdBufState.prim, ib );
    if ( gfxCmdBufSourceState.viewportIsDirty ) {
        GfxViewport vp;
        R_GetViewport( &gfxCmdBufSourceState, &vp );
        R_SetViewport( &gfxCmdBufState, &vp );
        R_UpdateViewport( &gfxCmdBufSourceState, &vp );
    }
    GfxDrawPrimArgs args;
    args.baseIndex   = batch->firstIndex;
    args.vertexCount = batch->maxVert + 1;   // verts span [0, maxVert], as every editor draw declares
    args.triCount    = batch->triCount;
    if ( batch->vertDecl != VERTDECL_PACKED )
    {
        R_DrawTessTechnique_Brushes( &args );
        return;
    }
    // Model runs: the merged-instance draw's pass loop, whose _dtex shaders read their packed
    // texcoord decode from def constants that must be re-issued per pass.
    for ( uint pass = 0; pass < gfxCmdBufState.technique->passCount; ++pass )
    {
        Editor_BeginModelPass( pass );
        R_SetupPassPerObjectArgs( gfxCmdBufContext );
        R_SetupPassPerPrimArgs( gfxCmdBufContext );
        if ( IDirect3DDevice9 *dev = gfxCmdBufState.prim.device )
            Editor_ForceVsDefConstants( dev, gfxCmdBufState.technique->passArray[pass].vertexShader );
        Editor_DrawIndexedPrimitive( &gfxCmdBufState.prim, &args );
    }
}

// 0x4FE750  RB_DrawEditorSkinnedCached_Sub — batch consecutive ED_SURF_MESH surfs sharing
// (vb, material, techType) into tess; ED_SURF_MODEL surfs draw via the uncached xmodel path
// with the instance's object placement (VERTDECL_PACKED, no VB pool).
static void RB_DrawEditorSkinnedCached_Sub(int index, int amount)
{
    PROF_SCOPED( "RB_DrawEditorSkinnedCached" );
    // A flush starts with NO claim about the current pass — other render commands may have
    // run since the last one.
    Editor_InvalidatePassKey();
    if (tess.indexCount)
        RB_EndTessSurface();
    R_Set3D(&gfxCmdBufSourceState);

    uint16_t minVert = 0xFFFF;
    int      maxVert = 0;
    IDirect3DVertexBuffer9 *boundVb = 0;
    bool     haveBatch = false;
    // After a retained batch the next surf ALWAYS starts a new tess batch: the batch-start
    // test cuts on `vb != boundVb`, and a retained batch never sets boundVb, so without this
    // a surf matching the batch's material would draw with the batch's vertex decl.
    bool     cutNext = false;

    for (int i = 0; i < amount; ++i) {
        editorSurf_s *edSurf = &edSceneGlobals.sceneSurfs[index + i];

        // KIWI: a retained batch draws from its own buffers in one draw and no upload; it ends
        // any open tess batch.
        if (edSurf->type == ED_SURF_BATCH) {
            if (tess.indexCount) {
                RB_DrawEditorTessSurface(minVert, (uint16_t)maxVert);
                minVert = 0xFFFF;
                maxVert = 0;
                boundVb = 0;
            }
            Editor_DrawBatch((const editorBatch_s *)edSurf->mesh_or_surfSub);
            cutNext = true;
            continue;
        }

        editorMesh_s           *mesh = nullptr;   // set for ED_SURF_MESH
        editorSurf_sub         *modelSurf = nullptr; // set for ED_SURF_MODEL
        GfxModelSkinnedSurface *skinnedSurf = nullptr;
        IDirect3DVertexBuffer9 *vb = 0;
        uint16_t firstIndex = 0;
        const Material *material;
        int techType, indexCount;

        if (edSurf->type == ED_SURF_MESH) {
            mesh = (editorMesh_s *)edSurf->mesh_or_surfSub;
            Editor_GetVertexBufferAndIndex(mesh->handle, &vb, &firstIndex);
            // A MESH surf with no resolvable VB MUST be dropped, not merely not-drawn:
            // falling through still copies its indices into tess, and the final flush is
            // `if (haveBatch && boundVb)` — so tess.indexCount would ESCAPE with g_primStats
            // 0 and the next RC_SET_MATERIAL_COLOR would deref NULL (rb_shade.cpp:202).
            if (!vb) {
                continue;
            }
            // KIWI-UX — the index invariant (Editor_MeshSurfBadIndex): this path rebases the
            // surf's table onto its own
            // vertex run below, which is only a rebase while every value is inside
            // [0, vertCount).  Dropped for the same reason a VB-less surf is, and BEFORE
            // the batch is opened so nothing of it reaches `tess`.
            {
                const int badK = Editor_MeshSurfBadIndex(mesh);
                if (badK >= 0) {
                    Editor_ReportMeshSurfBadIndex(mesh, badK);
                    continue;
                }
            }
            material   = mesh->material;
            techType   = mesh->techType;
            indexCount = (uint16_t)mesh->indexCount;
            ++s_edMeshSurfs;
        } else {
            vassert((edSurf->type == ED_SURF_MODEL), "(edSurf->type) = %i", edSurf->type);   // r_ed_scene.cpp:786
            modelSurf   = (editorSurf_sub *)edSurf->mesh_or_surfSub;
            vb          = 0;             // model surfs do NOT use the VB pool
            firstIndex  = 0;
            skinnedSurf = modelSurf->skinnedSurf;
            material    = modelSurf->material;
            techType    = modelSurf->techType;
            iassert(skinnedSurf->skinnedCachedOffset != RIGID_SKINNED_CACHE_OFFSET);   // r_ed_scene.cpp:798
            iassert(skinnedSurf->skinnedCachedOffset != HIDDEN_SURFACE_OFFSET);        // r_ed_scene.cpp:799
            indexCount  = 3 * XSurfaceGetNumTris(skinnedSurf->xsurf);
        }

        // Flush the current mesh batch when source/material/technique changes or tess would
        // overflow.  A model edSurf (vb==0) always breaks the batch.
        if (cutNext || vb != boundVb || material != gfxCmdBufState.material ||
            techType != gfxCmdBufState.techType || indexCount + tess.indexCount > 0x7FC0)
        {
            cutNext = false;
            // Gate on the PREVIOUS edSurf's VB — 0 after a MODEL (matrix[0] holds that
            // model's placement), non-zero after a MESH.
            if (boundVb) {
                RB_DrawEditorTessSurface(minVert, (uint16_t)maxVert);
                minVert = 0xFFFF;
                maxVert = 0;
            } else {
                Editor_SetEyeRelativeWorldMatrix();   // model-dirtied or first — restore world xform
            }
            RB_BeginSurface(material, (MaterialTechniqueType)techType);
            ++s_edBeginSurfaces;
            if (vb) {
                gfxCmdBufState.prim.vertDeclType = VERTDECL_WORLD;
                if (gfxCmdBufState.prim.streams[0].vb != vb || gfxCmdBufState.prim.streams[0].offset ||
                    gfxCmdBufState.prim.streams[0].stride != sizeof(GfxWorldVertex))
                    R_ChangeStreamSource(&gfxCmdBufState.prim, 0, vb, 0, sizeof(GfxWorldVertex));
                if (gfxCmdBufState.prim.streams[1].vb || gfxCmdBufState.prim.streams[1].offset ||
                    gfxCmdBufState.prim.streams[1].stride)
                    R_ChangeStreamSource(&gfxCmdBufState.prim, 1, 0, 0, 0);
            } else {
                gfxCmdBufState.prim.vertDeclType = VERTDECL_PACKED;   // model path
            }
            boundVb = vb;
            haveBatch = true;
        }

        if (mesh) {
            iassert( !modelSurf );   // r_ed_scene.cpp:836
            if ((uint16_t)firstIndex < minVert)
                minVert = firstIndex;
            if (maxVert < mesh->vertCount + firstIndex - 1)
                maxVert = mesh->vertCount + firstIndex - 1;
            const uint16_t *itab = (const uint16_t *)mesh->indexTable;
            for (int k = 0; k < (int)(uint16_t)mesh->indexCount; ++k)
                tess.indices[tess.indexCount + k] = (uint16_t)(firstIndex + itab[k]);
            tess.indexCount += (uint16_t)mesh->indexCount;
        } else {
            iassert( modelSurf );   // r_ed_scene.cpp:848
            iassert( modelSurf->surf );   // r_ed_scene.cpp:849
            iassert( tess.indexCount == 0 );   // r_ed_scene.cpp:850
            iassert( tess.vertexCount == 0 );   // r_ed_scene.cpp:851
            iassert(skinnedSurf->skinnedCachedOffset != RIGID_SKINNED_CACHE_OFFSET);   // r_ed_scene.cpp:855
            iassert(skinnedSurf->skinnedCachedOffset != HIDDEN_SURFACE_OFFSET);        // r_ed_scene.cpp:856
            // Try the merged pre-transformed run first.  Zero means "not available" and the
            // surf falls through to the per-instance path — a slower frame, never a wrong one.
            {
                const int merged = Editor_DrawMergedInstanceRun(
                    &edSceneGlobals.sceneSurfs[index], i, amount - i);
                if (merged > 0) {
                    i += merged - 1;      // the loop's own ++i consumes the last one
                    continue;
                }
            }
            if (gfxCmdBufSourceState.objectPlacement != modelSurf->placement)
                R_ChangeObjectPlacement(&gfxCmdBufSourceState, modelSurf->placement);
            Editor_DrawXModelSkinnedUncached(skinnedSurf->xsurf, skinnedSurf->skinnedVert);
            // KIWI: the binary's `objectPlacement = 0` lives in Editor_SetEyeRelativeWorldMatrix.
        }
    }

    if (haveBatch && boundVb)         // only a mesh batch needs a final tess flush
        RB_DrawEditorTessSurface(minVert, (uint16_t)maxVert);

    gfxCmdBufState.prim.vertDeclType = VERTDECL_GENERIC;
    Editor_SetEyeRelativeWorldMatrix();
}

// 0x533880  RB_DrawEditorSkinnedCached — RC_DRAW_EDITOR_SKINNEDCACHED handler.
void __cdecl RB_DrawEditorSkinnedCachedCmd(GfxRenderCommandExecState *execState)
{
    const GfxCmdEditorSkinnedCached *cmd = (const GfxCmdEditorSkinnedCached *)execState->cmd;
    // Differs from the binary: clear the persisted material first.  Sub's batch-start
    // condition compares material/techType, which Sub leaves set while resetting
    // vertDeclType to GENERIC; back-to-back flushes sharing both would skip RB_BeginSurface
    // and draw with the stale decl.
    gfxCmdBufState.material = 0;
    RB_DrawEditorSkinnedCached_Sub(cmd->index, cmd->amount);
    execState->cmd = (const char *)execState->cmd + cmd->header.byteCount;
}

// ── per-frame reset ───────────────────────────────────────────────────────────
// 0x4FD910  R_SortMaterials — sort newly-registered materials and, once per front-end frame,
// reset the editor scene accumulation for the next view.
void __cdecl R_SortMaterials()
{

    bool inFrame = rg.inFrame;
    rg.inFrame = 1;

    if (rgp.needSortMaterials) {
        Material_Sort();
        rgp.needSortMaterials = 0;
    }

    if (edScene_lastFrameCount != (int)rg.frontEndFrameCount) {
        // The one place that runs exactly once per front-end frame.
        s_edSkinSurfs = s_edSkinBytes = s_edSkinCommits = 0;
        s_edModelInsts = s_edModelSurfsCached = s_edModelSurfsDynamic = 0;
        s_edModelUploadBytes = 0;
        s_edDrawCalls = s_edMergedDraws = s_edMergedInsts = 0;
        s_edRunIndexLocks = s_edRunIndexRuns = 0;
        s_edPassSetups = s_edPassSetupsSkip = s_edVsConstUploads = 0;
        s_edBeginSurfaces = s_edStreamSwitches = 0;
        s_edMeshSurfs = s_edMeshTessDraws = 0;
        // The per-frame transform budget (kiwi_instcache.h), reset AFTER the plots read it.
        KiwiInstCache_BeginFrame();
        frontEndDataOut->viewInfo[frontEndDataOut->viewInfoCount].cmds = 0;
        R_ClearScene(0);
        edScene_lastFrameCount        = rg.frontEndFrameCount;
        edSceneGlobals.sceneMeshCount = 0;
        radiant_surfCount             = 0;
        radiant_modelSurfPos          = 0;   // binary: surfPos resets with tempSkinPos
        s_edBatchCount                = 0;
        edSceneGlobals.sceneSurfCount = 0;
    }

    edSceneGlobals.sceneSurfCount_saved = edSceneGlobals.sceneSurfCount;
    iassert(rg.inFrame);
    rg.inFrame = inFrame;
}

#pragma once
// A model instance's mesh as the retained scene keeps it (r_ed_scene.cpp implements these):
// the surfaces SkinModelInst would queue for one instance at a technique, and their vertices
// in world space, so a cell can batch them like brush faces.

#include <stdint.h>
#include <vector>

struct Material;
struct XSurface;

struct edModelSurf_t
{
    Material       *material;     // after the technique demotion SkinModelInst applies
    int             techType;
    int             sortKey;
    const XSurface *xsurf;        // identity of the vertex run (Editor_ModelInstTransform)
    int             vertCount;
    const uint16_t *indices;      // run-relative triangle list
    int             indexCount;
};

// The surfaces `instanceHandle` draws at `techType` with no colour stamp, in SkinModelInst's
// order; returns how many were written (at most maxOut), 0 = draws nothing.
int  Editor_ModelInstSurfaces( int instanceHandle, int techType, edModelSurf_t *out, int maxOut );

// Moves whenever the instance's slot is claimed, re-posed or freed.
unsigned Editor_ModelInstVersion( int instanceHandle );

// The instance's placement: quaternion, origin, scale (8 floats).  False = no live instance.
bool Editor_ModelInstPlacement( int instanceHandle, float out[8] );

// Write `xsurf`'s vertices transformed by the instance's placement: vertCount packed
// vertices (32 bytes each) into `out`, exactly as the merged-instance draw holds them.
void Editor_ModelInstTransform( int instanceHandle, const XSurface *xsurf, void *out );

// The packed (model) vertex layout's size and declaration, for a batch over those vertices.
int  Editor_ModelVertexStride();
int  Editor_ModelVertexDecl();

// ── Recording what a draw queues into the editor flush ───────────────────────
// While a recording is set, Editor_AddMeshCmd and Editor_AddSurfCmd append to it (after
// their own gates) instead of queueing - how the retained scene keeps a prefab's contents,
// drawn once through DrawBrush.  A model surface with a colour stamp is flagged: its vertices
// are a per-frame copy the scene cannot keep.
struct edRecMesh_t
{
    Material       *material;
    int             techType;
    int             sortKey;
    unsigned        vbHandle;
    int             vertCount;
    const uint16_t *indices;      // run-relative
    int             indexCount;
};
struct edRecModel_t
{
    edModelSurf_t   surf;         // material, technique, sort key, vertices, indices
    int             instance;
    bool            stamped;
};
struct edSurfRecording_t
{
    std::vector<edRecMesh_t>  meshes;
    std::vector<edRecModel_t> models;
};
void Editor_SetSurfRecording( edSurfRecording_t *rec );   // nullptr = stop

// The open window's first `count` entries are sorted (the retained scene's batches).
void KiwiEdScene_StampSortedHead( int count );

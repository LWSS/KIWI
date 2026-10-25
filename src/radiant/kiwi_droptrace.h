#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif

// Shared by model-drag placement and both browsers: Test_Ray handles visible
// brushes/patches, then resident XModels get a bounded mesh pass.

struct ray_t;
struct selbrush_t;
struct XModel;

struct kiwiDropHit_t
{
    float point[3];
    float normal[3];
    float dist;
};

bool KiwiDrop_BoundsValid( const float mins[3], const float maxs[3] );
bool KiwiDrop_IsModelEntity( const selbrush_t *node );

// -1: unavailable, use native fallback; 0: mesh miss; 1: nearest mesh hit.
// Input ray and output normal use the entity's parent coordinate system.
int KiwiDrop_TraceModelCached( selbrush_t *node, const float *start, const float *dir,
                               float *outDist, float *outNormal );
void KiwiDrop_ClearModelTrees();
bool KiwiDrop_ValidateModelTree( selbrush_t *node ); // deterministic test-mode parity check

// Uses resident state only; a missing XModel falls back to eclass bounds and null outModel.
bool KiwiDrop_GetModelInfo( selbrush_t *node,
                            float mins[3], float maxs[3],
                            float angles[3], float *scale, float origin[3],
                            XModel **outModel = 0 );

// Rotates/scales the eight local bounds corners about the model origin.  The
// returned extents and optional corners are origin-relative world-axis vectors.
bool KiwiDrop_TransformBounds( const float mins[3], const float maxs[3],
                               const float angles[3], float scale,
                               float outMins[3], float outMaxs[3],
                               float ( *outCorners )[3] = 0 );

// Nearest Test_Ray surface or one of the 64 nearest model-AABB candidates.
// Only live drags omit selected models; browser drops may land on them.
bool KiwiDrop_Trace( const ray_t &ray, bool excludeSelectedModels,
                     kiwiDropHit_t *outHit );

// KIWI (2026-10-03, user: "there should be snapping points for each vertex on each xmodel
// near the cursor"): mesh vertices of visible resident xmodels within `reachPx` of the
// camera-image pixel (curX, curY), nearest first, at most maxOut.  Vertices the model itself
// hides from the eye (its far side) are dropped; occlusion by OTHER geometry is the caller's
// gate.  Coincident vertices (UV seams) appear once.  excludeSelected = live transforms.
// One gather per frame (cursor, camera, exclusion) serves every reach up to 64 px; a model
// with more than 0x40000 LOD-0 vertices, or past that budget in one gather, is skipped.
struct kiwiModelVert_t
{
    float       pos[3];      // world
    float       px, py;      // camera-image pixels
    float       distPx;      // to (curX, curY)
    selbrush_t *node;
};
int KiwiDrop_ModelVertsNear( float curX, float curY, float reachPx, bool excludeSelected,
                             kiwiModelVert_t *out, int maxOut );

// KIWI (2026-10-22, Plasticity 2026.2 "mesh snapping highlights the snapped mesh face with a
// partial wireframe showing the triangle"): the world triangle of `node`'s mesh that owns the
// snapped vertex `pos` (UV-seam copies included) and that the cursor is on - the one whose
// screen projection holds (curX, curY), else the nearest on screen - among the triangles the
// model itself does not hide from the eye.  False when none qualifies.
bool KiwiDrop_ModelVertTriangle( selbrush_t *node, const float pos[3], float curX, float curY,
                                 float outTri[3][3] );

// KIWI (2026-09-09): the pick-side model test — the node whose MESH the ray hits first
// within `maxDist` (the occluding brush hit, or FLT_MAX), or false.  Same bounded
// candidate gather as KiwiDrop_Trace; the ray must already be section-clamped.
bool KiwiDrop_PickModelMesh( const float start[3], const float dir[3], float maxDist,
                             bool excludeSelectedModels, selbrush_t **outNode,
                             float *outDist, float outNormal[3] );

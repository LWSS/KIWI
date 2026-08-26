#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Classic brushes are plane-defined: edit planepts, rebuild derived windings, then
// validate. Rejected per-frame edits restore an in-memory plane/control-point baseline
// without touching the gesture-level undo record or accumulating incremental drift.
//
// KiwiValid_CheckBrush runs after KiwiValid_Rebuild; the first failure wins:
//   V1  fewer than four retained half-spaces, or no face array
//   V2  plane normal not approximately unit length after Face_MakePlane
//   V3  null, shorter-than-three-point, or overflowed winding
//   V4  winding area below KVALID_MIN_FACE_AREA
//   V5  same-facing planes within the dot and distance tolerances
//   V6  opposed planes whose n·p <= d slab is thinner than KVALID_MIN_THICKNESS
//   V7  non-increasing, oversized, or out-of-map rebuilt bounds
// Full convexity is derived by Brush_BuildWindings; tiny-edge rejection would exclude
// legitimate detail brushes, so neither is a separate check.
//
// V8 is separate because an unbounded solid can still have a finite set of
// triple-plane intersections and finite-looking bounds. It grows a probe box and calls
// Brush_MakeFaceWinding (brush.cpp 0x471260); a point on that box means a face is unbounded.
// Only half-space removal needs the O(faceCount²) closure test.

#include <vector>

struct brush_t;
struct patchMesh_t;

// Loose broken-geometry thresholds; distances and areas are in world units.
#define KVALID_MIN_FACE_AREA   0.1f       // square world units
#define KVALID_PLANE_DOT       0.999f     // |cos| above this = parallel planes
#define KVALID_PLANE_DIST      0.01f      // world units, duplicate-plane test
#define KVALID_MIN_THICKNESS   0.01f      // world units, opposed-plane slab
#define KVALID_MAX_SPAN        65536.0f   // map bound
#define KVALID_MAX_COORD       131072.0f  // the editor's own sentinel box

// Patch_GenericMesh refuses a control-grid width outside 3..15 (pmesh.cpp:1550).
// This builder limit is distinct from patchMesh_t::ctrl's 16x16 storage extent.
#define KPATCH_MIN_WIDTH       3
#define KPATCH_MAX_WIDTH       15

// The large margin separates closed faces from the probe; TOUCH recognizes box clips.
#define KVALID_CLOSE_MARGIN    4096.0f    // world units the probe box adds per side
#define KVALID_CLOSE_TOUCH     1.0f       // world units, "this point is ON the box"

// Exactly one geometry baseline is populated: patches store ctrl xyz in
// col*height+row order; classic brushes store face-order planepts. Patch_Rebuild
// replaces a patch's bounding-brush faces, so those planepts are never captured.
struct kiwiBaseBrush_t
{
    brush_t           *def       = nullptr;
    int                faceCount = 0;
    int                patchW    = 0;
    int                patchH    = 0;
    std::vector<float> planepts;
    std::vector<float> ctrl;
};

// Capture planepts or patch ctrl xyz. Empty brushes are allowed; null arguments fail.
bool KiwiValid_Snapshot( brush_t *def, kiwiBaseBrush_t *out );

// Copy the baseline without rebuilding, avoiding a redundant per-frame rebuild.
// Returns false if the face count or patch dimensions no longer match.
bool KiwiValid_RestoreOnly( const kiwiBaseBrush_t &base );

// RestoreOnly plus the appropriate brush or patch rebuild.
bool KiwiValid_Restore( const kiwiBaseBrush_t &base );

// Rebuild planes, windings, and bounds with Brush_BuildWindings(def, 0). bFull 1
// would apply the legacy power-of-two snap after the modern layer's own snapping.
// Also marks the map modified and increments def->version.
void KiwiValid_Rebuild( brush_t *def );

// Full V1..V7 gate after KiwiValid_Rebuild. `outWhy` is untouched on success.
bool KiwiValid_CheckBrush( brush_t *def, const char **outWhy = 0 );

// V1..V6 exclude `ignoreFace`, with V1 counted after exclusion. Intended for a
// trial where that face duplicates a survivor: excluding the redundant half-space
// models the post-removal solid while keeping V5 strict. Duplicate planes do not
// affect rebuilt bounds, so V7 remains unchanged. A negative index excludes nothing.
bool KiwiValid_CheckBrushIgnoringFace( brush_t *def, int ignoreFace,
                                       const char **outWhy = 0 );

// V8 closure test for half-space removal, after V1..V7 and KiwiValid_Rebuild.
// Costs O(faceCount²) winding clips; def->[mins,maxs] are restored before return.
bool KiwiValid_BrushCloses( brush_t *def, const char **outWhy = 0 );

// V7 alone for patches: their control grid is not plane-defined, while Patch_Rebuild
// writes meaningful tessellated bounds through Brush_RebuildBrush.
bool KiwiValid_CheckBounds( brush_t *def, const char **outWhy = 0 );

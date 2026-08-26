#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Chamfer geometry shared with kiwi_patchfillet.cpp, plus inset and remove-face
// commands. KIWI_CMD_BEVEL_EDGE forwards to the fillet command, which starts in
// chamfer mode and lets D toggle the patch fillet.
//
// Geometry is regenerated from baseline plane points, rebuilt, and rejected by
// the §19 validity gate before commit.
//
// Chamfer frame
// One physical edge has a SEL_EDGE item on each adjacent face; callers deduplicate
// unordered segments with the ported 0.1-world-unit point tolerance.
// For outward unit normals n1/n2 and edge endpoints e0/e1:
//
//     n  = normalise( n1 + n2 )            the angle bisector, pointing OUTWARD
//     u  = normalise( e1 - e0 )            the edge direction
//     v  = cross( n, u )                   the third basis vector
//     c  = midpoint(e0,e1) - n * d         the plane's anchor point
//
// u is re-orthogonalised because endpoints and normals are floats. With
// v = cross(n,u), cross(u,v) = n, so the frame has the required handedness.
// d is perpendicular world-space depth inward along -n; the validity gate rejects
// zero-area and over-deep cuts.
//
// Face_MakePlane at 0x470470 computes cross(p0-p1, p2-p1). Writing
//
//     planepts[0] = c + u * s      planepts[1] = c      planepts[2] = c + v * s
//
// gives s²*n, the required outward normal. Spread s is clamped around edge length
// so the three points retain §19 V2 headroom.
//
// Face_Alloc at 0x471500 copies the material-bearing source face and reallocates
// def->faces; callers must not retain face pointers across it. Brush_RemoveFace at
// 0x471640 shifts whole face records. Appended faces are therefore removed from
// the tail in reverse order before each preview is regenerated and validated.
// Drag and numeric depths use raw world-space floats; no grid rounding is applied.
//
// Inset clone
// A convex half-space brush cannot represent a mesh-style border ring. Inset
// instead clones the brush, moves each edge-adjacent face inward along its own
// outward normal, and leaves the target plane and original brush unchanged.
// Adjacent means two winding points match within the ported 0.1-unit tolerance;
// corner-only contact does not count.
// The clone remains unlinked during preview, is linked only on commit, and can be
// freed safely on rejection. Drag depth is start radius minus current radius in
// the target plane; numeric input is converted to world units by the numeric layer.

class  KiwiEditorCommand;
struct brush_t;                   // qe3.h:474 (the 88-byte brush DEFINITION)
struct selbrush_t;                // qe3.h:429 (the 56-byte brush INSTANCE / list node)

#define KBEV_MIN_DIST     0.25f    // world units — below this the gesture is a no-op
#define KBEV_MIN_SPREAD   16.0f    // planept spread floor (§19 V2 headroom)
#define KBEV_MAX_SPREAD   1024.0f  // ceiling preserves precision on long edges
#define KBEV_MAX_EDGES    64
#define KBEV_MAX_FACES    64       // inset target cap

// Shared chamfer helpers keep the patch arc and appended notch on the same frame.
// `srcFace` follows kiwi_material.h R5: prefer an inheritable adjacent face, then
// the brush's dominant inheritable face, then adj[0].
// The bias convention rotates the normal in the +v sense:
//
//     n(bias) = n·cos(bias) + v·sin(bias)
//
// Bias zero is symmetric. The caller resolves positive toward adj[1] and keeps
// its magnitude below the half-angle returned by KiwiBevel_BiasLimit.
struct kiwiBevelEdge_t
{
    selbrush_t *node;
    brush_t    *def;
    float       e0[3], e1[3];   // the world edge, baseline
    float       mid[3];         // its midpoint
    float       n[3];           // outward bisector (unit)
    float       u[3];           // edge direction (unit, ⟂ n)
    float       v[3];           // cross(n,u) (unit)
    float       spread;         // planept spread, clamped to [MIN,MAX]_SPREAD
    int         srcFace;        // material source face (kiwi_material.h R5)
    int         adj[2];         // the two faces sharing this edge
    float       bias;           // chamfer tilt about the edge, radians (0 = bisector)
};

// Fill everything but node/def/e0/e1, which the caller supplies.  False = this is
// not a manifold two-face corner, or the frame collapses (exactly opposed faces,
// zero-length edge) — either way there is no well-defined chamfer.  `bias` is
// always seeded to 0 — the caller sets it afterwards.
bool KiwiBevel_MakeFrame( kiwiBevelEdge_t *e );

// The half-angle between the bisector and either adjacent face normal, in
// radians: |bias| must stay strictly below it.  Returns 0 when the frame is
// degenerate, which the caller reads as "no bias is available here".
float KiwiBevel_BiasLimit( const kiwiBevelEdge_t &e );

// The chamfer plane's outward normal at this frame's `bias`.  Exported because
// the patch fillet needs the SAME normal the appended face will carry.
void  KiwiBevel_BiasedNormal( const kiwiBevelEdge_t &e, float out[3] );

// Append ONE face carrying the chamfer plane at perpendicular depth `dist`,
// measured from the edge inward along -n(bias).  Returns the new face's index, or
// -1.  Face_Alloc REALLOCATES def->faces, so no caller may hold a face pointer
// across this call.  The caller owns the rebuild and the §19 gate.
int  KiwiBevel_AppendFace( brush_t *def, const kiwiBevelEdge_t &e, float dist );

void KiwiBevel_RegisterCommands();
KiwiEditorCommand *KiwiBevel_CommandForId( int commandId );

// §3 canExecute predicates for the palette metadata rows.
bool KiwiBevel_CanBevel();     // at least one live SEL_EDGE item
bool KiwiBevel_CanInset();     // at least one live SEL_FACE item on a non-patch

// Remove face / restore edge
// Delete with exactly one live brush face selected removes that half-space and
// rebuilds, re-extending its neighbours. Other selections use classic Delete.
// This is deliberately general because brushes store no bevel-origin metadata.
// Fewer than four remaining faces, invalid geometry, or an unbounded result is
// refused. Rejection re-appends the saved whole face; its index may change, so the
// typed selection is rebuilt. Selected fillet patches share the same undo record.
bool KiwiBevel_CanRemoveFace();          // exactly one live SEL_FACE item on a brush
bool KiwiBevel_RemoveFaceRestoreEdge();  // true = it ran (and printed its own line)

// The "Modeling" block's bevel/inset buttons (drawn by KiwiCsg_MenuItems' caller).
void KiwiBevel_MenuItems();

#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Fillet Edge is one modal tool: it starts as a flat chamfer, and D toggles a
// tangent patch fillet without changing the current solid cut. Chamfer bias is
// signed degrees about the edge; fillet mode refuses it because circular
// tangency to both faces fixes the arc on the bisector.
//
// For outward face normals n1/n2, n = normalize(n1+n2), phi = acos(n1.n2),
// and k = n.n1 = cos(phi/2). A radius r uses axis A = E - n*r/k and chamfer
// depth d = r*(1-k^2)/k; its tangent rails are A+r*n1 and A+r*n2.
//
// Each non-rational quadratic span uses the two circle endpoints and their
// tangent intersection at r/cos(alpha/2). Limiting spans to KPF_SPAN_DEG keeps
// the radial error r*(1-cos(alpha/2))^2/(2*cos(alpha/2)) small.
//
// Wedges outside KPF_MIN_WEDGE_DEG..KPF_MAX_WEDGE_DEG are refused: near-flat
// corners collapse the strip, while spikes make the tangent length consume the brush.
//
// The solid is rebuilt live from its baseline. Patches land only at commit because
// patch creation allocates and links a symbiont brush; doing that per mouse move
// would require undo-backed unlinking.
//
// Edge selections are absent from selected_brushes, so touched solids are covered
// explicitly. Deselect before landing patches so the undo tail sees only the
// newly created patch brushes; one undo then removes patches and restores solids.
//
// Bare B dispatches here when brush edges are selected; otherwise it remains the
// construction-curve fillet command.

class  KiwiEditorCommand;
struct brush_t;                   // qe3.h:474 (the 88-byte brush DEFINITION)

// Tuning.
#define KPF_MIN_RADIUS      0.25f   // world units — below this the gesture is a no-op
#define KPF_MAX_EDGES       16      // gesture cap: one large patch per edge
#define KPF_SPAN_DEG        30.0f   // target arc angle per bezier span (see the bound)
#define KPF_MAX_SPANS       7       // 2*7+1 = 15 columns, inside the 16 the format allows
#define KPF_MIN_WEDGE_DEG   5.0f    // interior angle floor — below this it is a spike
#define KPF_MAX_WEDGE_DEG   170.0f  // …and ceiling — above this there is no corner
#define KPF_PATCH_ROWS      3       // along the edge; the cross-section is constant, so
                                    // three rows (both ends + the exact midpoint) is not
                                    // an approximation, it is the minimum odd grid

void KiwiPatchFillet_RegisterCommands();
KiwiEditorCommand *KiwiPatchFillet_CommandForId( int commandId );

// Enabled by at least one live SEL_EDGE item on a non-patch brush.
bool KiwiPatchFillet_CanFillet();

// Contextual B returns KIWI_CMD_FILLET_EDGE when `commandId` is
// KIWI_CMD_FILLET_CURVE and the selection is brush edges; otherwise returns
// `commandId` unchanged.  Total, side-effect free, and safe to call on every id.
int  KiwiPatchFillet_ContextB( int commandId );
// Landed fillets store no source-brush id; association is re-derived from current
// geometry so it remains valid after map round trips and external edits.
//
// A plane move carries only complete patch rows lying on that face and
// re-interpolates interior rows. Partial cross-sections are refused because they
// require re-solving the radius; rotate, vertex, free-form, and scale edits are
// intentionally outside this model.
//
// The caller must already own the undo bracket. planeDistBefore is the face's old
// distance, travel is signed along planeN, and outSkipped counts refused patches.

int KiwiFillet_CarryOnPlaneMove( const brush_t *def, int movedFace,
                                 const float planeN[3], float planeDistBefore,
                                 float travel, int *outSkipped );

// KIWI (2026-11-01): a patch textured as a continuation of the brush face it meets, so curve and
// brush read as one surface.  A flat patch takes the face that is coplanar with it, facing the
// same way and touching it, and gets that face's own planar mapping (same texdef, same
// projection: no seam).  A curved patch takes the face that holds a whole end column and runs on
// from it - the curve's start tangent lies in the face, as an arch's leaves its side wall - and
// gets that face's mapping wrapped round the curve by arc length, as a fillet's is.  Layer 0
// only; the caller owns undo and rebuilds.  Null / false: no such face.
struct face_t;
struct patchMesh_t;
const face_t *KiwiPatch_FindSourceFace( const patchMesh_t *p, brush_t *const *brushes, int count );
bool          KiwiPatch_TextureFromFace( patchMesh_t *p, const face_t *src );

// KIWI (2026-11-01, user: "match the ceiling to both sides of the arch ... match the left side
// wall, and for the right side, scroll the brush texture down Z and get it to match that way"):
// after a curve was wrapped from `src`, the brush faces its OTHER end runs into - the far face
// (as KiwiPatch_FindSourceFace picks one) and every face among `brushes` that is coplanar with it
// and carries the same mapping, i.e. that whole wall - and the scroll in repeats (s, t; whole
// repeats dropped) that lands their mapping on the curve's there.  An unfolded wrap meets the far
// wall upside down; KiwiPatch_MatchTexture folds it first.  0: no far face, or no single scroll
// matches (its texture runs at another size or angle).
struct kpatchFaceRef_t
{
    brush_t *brush;
    int      face;
};
int  KiwiPatch_FarFaces( const patchMesh_t *p, const face_t *src, brush_t *const *brushes, int count,
                         kpatchFaceRef_t *out, int max, float scroll[2] );
// Scrolls one face's layer-0 texture by `scroll` repeats (its texdef shift) and rebuilds the
// brush's windings.  The caller owns undo.
void KiwiPatch_ScrollFace( brush_t *b, int face, const float scroll[2] );

// The whole match: KiwiPatch_TextureFromFace, and for a curve that comes down onto a far wall
// among `brushes` (an arch between two sides) the wrap folds at the crown instead - each half
// runs up from its own side the right way up - and the far wall + its scroll come back as
// KiwiPatch_FarFaces gives them, for the caller to cover in undo and apply.  -1: no match at all.
int  KiwiPatch_MatchTexture( patchMesh_t *p, const face_t *src, brush_t *const *brushes, int count,
                             kpatchFaceRef_t *out, int max, float scroll[2] );

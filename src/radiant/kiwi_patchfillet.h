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

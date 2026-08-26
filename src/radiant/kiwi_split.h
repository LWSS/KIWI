#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Cut splits selected brushes with a plane picked from a construction segment,
// brush edge, or brush face. Ctrl+R splits the selected brush along a live line
// on its selected face; a convex half-space brush cannot hold a divided face.
//
// Cut is two-stage: the click locks the source plane, then RMB/Enter commits the
// preview. A face supplies its plane directly. For a segment or edge in world space:
//   u = normalize(p1-p0), away = normalize(vpn-u*(vpn·u)), n = normalize(u×away).
// camera_s::vpn points into the scene, so `away` sweeps from the camera; a source
// parallel to vpn is rejected. Orbiting never re-derives the locked plane.
//
// Both commands wrap Brush_SplitBrushByFace in brush.cpp at 0x471960. Returned
// defs are linked to the source owner but to no display list. The new face inherits
// from the best source face, falling back to classic caulk for all-tool brushes.
// §19 rejects invalid landed halves before map, selection, or undo state changes.
//
// Undo follows CSG_MakeHollow: clone selected sources, land all halves, then free
// sources so an owner never transiently has zero brushes. Undo_EndBrushList at
// 0x45E870 stamps the landed halves; callers, not the splitter, free source instances.
//
// Esc returns preview to source picking, then cancels. Construction picking uses
// the shared 10-pixel segment tolerance independent of selection mode.
//
// Face Split's U is perpendicular to the longest face edge; V is parallel to it.
// The cursor ray is intersected with the face plane. An on-plane geometry snap wins
// except SNAP_FACE (the unsnapped surface hit); otherwise the world grid is used.
// Its sole numeric field is a world-length offset from the low slide-axis edge.
// Keep exactly one field so kiwi_command.cpp routes Tab to the U/V switch; changing
// axes must re-seat the cut because the numeric reference edge changes.

class  KiwiEditorCommand;
struct brush_t;                   // qe3.h:474 (the 88-byte brush DEFINITION)
struct selbrush_t;                // qe3.h:429 (the 56-byte brush INSTANCE / list node)

// Lands two selected halves; the caller frees `node` after success inside undo.
// Failure leaves the map unchanged and names the reason through `why`.
bool KiwiSplit_BrushByPlane( selbrush_t *node,
                             const float p0[3], const float p1[3], const float p2[3],
                             selbrush_t **outA, selbrush_t **outB, const char **why );

// Def-level split for cascades; returned defs are owner-linked but not displayed.
// Front is {n·p >= d}, back is {n·p <= d}; one NULL means the plane did not divide.
// Both halves are §19-gated. False allocates nothing and never changes `def`.
bool KiwiSplit_DefByPlane( brush_t *def,
                           const float p0[3], const float p1[3], const float p2[3],
                           brush_t **outFront, brush_t **outBack, const char **why );

// Subtract needs per-half results: front pieces may be landed, while back is the
// running intersection and is discarded after the final tool plane.
enum kiwiSplitHalf_t
{
    KSPLIT_HALF_NONE = 0,   // the plane did not divide: nothing lies on this side
    KSPLIT_HALF_OK,         // a §19-valid def is returned and is the caller's
    KSPLIT_HALF_SLIVER      // a def existed, failed §19 and HAS BEEN FREED
};

// §19 always gates front because it may be landed. A refused back may continue as
// an unlanded intermediate only when its bounds remain a real subset with at least
// this much thickness on every world axis; outBackRefused makes that visible.
#define KSPLIT_CARRY_EXTENT   1.0f      // world units, per axis

// False allocates nothing. On true, state and pointer report each half independently;
// `why` names the first §19 refusal (front before back).
bool KiwiSplit_DefByPlaneCarve( brush_t *def,
                                const float p0[3], const float p1[3], const float p2[3],
                                brush_t **outFront, brush_t **outBack,
                                kiwiSplitHalf_t *outFrontState,
                                kiwiSplitHalf_t *outBackState,
                                bool *outBackRefused,
                                const char **why );

// Entity_UnlinkBrush + Brush_Free_R for a returned def that will not be landed.
void KiwiSplit_FreeUnlandedDef( brush_t *def );

// True when the plane through p0/p1/p2 strictly straddles the brush's bounds —
// the cheap pre-test both verbs use to decide which brushes a cut applies to.
bool KiwiSplit_PlaneCrossesBrush( const brush_t *def,
                                  const float p0[3], const float p1[3], const float p2[3] );

void KiwiSplit_RegisterCommands();
KiwiEditorCommand *KiwiSplit_CommandForId( int commandId );

bool KiwiSplit_CanCut();          // §3 palette predicate: >= 1 splittable brush
bool KiwiSplit_CanSplitFace();    // §3 palette predicate: exactly one face selected

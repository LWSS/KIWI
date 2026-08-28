#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Modal transforms: G acts on the dominant selection kind (objects > faces >
// edges > vertices); R and S act on whole-object selections only.
//
// Apply every frame from the gesture baseline. Incremental ported mutators receive
// only the residual from the total already applied. Undo opens at the first real
// mutation; face-selected brushes must be covered explicitly before mutation.
//
// Move invariant: m_ref stays fixed while the live reference is m_ref + total
// (or m_ref + pushDir * scalar for faces). Rotate keeps a latched pivot/axis and
// applies degree residuals. Scale applies factor ratios so it does not compound.
// Face pushes retain each face's baseline plane and use one scalar; edge moves keep
// the retained third point fixed; patch vertices use baseline control points while
// brush vertices feed residuals to Brush_MoveVertex (brush.cpp 0x471C30).
//
// Free movement uses ray/plane intersection with the camera normal latched at
// Begin; axis locks use closest-point-on-ray and plane locks use ray/axis-normal
// plane intersection. Move and rotate mapping requires a held handle/ring; typed
// values bypass that gate. A new grab must reproduce the current pose exactly and
// stays frozen until the cursor leaves the press pixel.
//
// Grid snapping is absolute (`snap(ref + total) - ref`); named geometry targets
// remain exact. G numeric input is a world-unit distance (free input uses the
// ground-plane mouse direction, falling back to +X), R is degrees, and S is a
// factor. S intentionally remains a free horizontal-pixel drag until it has handles.
// Invalid results may not commit; the validity/rollback details live with each path.
#include "kiwi_selection.h"          // sel_kind_t (the gizmo handoff below)

class KiwiEditorCommand;
struct ray_t;

// Drop local XModel bounds onto traced scene geometry. X/Y alone snap; the
// origin seats directly on the hit surface (bbox clipping into the ground is
// deliberate — CoD model origins are authored at ground contact).
bool KiwiDrop_ComputePlacement( const ray_t &ray,
                                const float modelMins[3], const float modelMaxs[3],
                                const float angles[3], float scale,
                                float outOrigin[3],
                                float outWorldMins[3] = 0,
                                float outWorldMaxs[3] = 0,
                                const float *supportCorners = 0,
                                int supportCornerCount = 0 );

// Start the Move drop variant from an already-selected model-only selection.
bool KiwiDrop_BeginAt( int imgX, int imgY );
bool KiwiDrop_Active();

// Called by KiwiCmd_RegisterCommands, the single registration point.
void KiwiXform_RegisterCommands();

// Command object for a transform id, or NULL.
KiwiEditorCommand *KiwiXform_CommandForId( int commandId );

// Palette availability.
bool KiwiXform_CanMove();      // any movable item is selected
bool KiwiXform_CanRotate();    // at least one whole OBJECT is selected (v1 scope)
bool KiwiXform_CanScale();     // ditto

// Gizmos feed the live command object. Presets are guarded no-ops unless the
// matching transform is active and route through its constraint-rebase path.
#define KIWI_XCON_FREE   0
#define KIWI_XCON_AXIS   1
#define KIWI_XCON_PLANE  2

bool KiwiXform_DominantKind( sel_kind_t *out );
void KiwiXform_PresetMoveConstraint( int con, int axis );

// ActivePivot is the move's live anchor or the rotate's latched pivot; never
// re-derive it from selection bounds. Rotate feeds are sweeps since the current
// grab, added to the angle latched by HandleGrab.
bool KiwiXform_IsMoveActive();
bool KiwiXform_IsRotateActive();
bool KiwiXform_ActivePivot( float *out3 );
void KiwiXform_PresetRotateAxis( int axis );
void KiwiXform_FeedRotateDegrees( bool active, float degrees );

// Live drive-face outward normal for the face-push handle.
bool KiwiXform_ActivePushDir( float *out3 );

// V places one session-only pivot inside the live Move/Rotate gesture. It survives
// commits but expires when the selection signature changes and is never persisted.
bool KiwiXform_PivotOverride( float *out3 );   // false = no session pivot in force
bool KiwiXform_PivotPlacing();                 // true while V-placement is live

// Snap candidates are useful only while placing a pivot or aiming a move handle.
bool KiwiXform_WantsSnapDots();

// One-shot face push shared with negative extrude. Depth is brush thickness along
// the face's outward normal; crossing it deletes the brush. `undoOp` must be a
// string literal because Undo_GeneralStart retains the pointer. Rejection restores
// plane and texture baselines and cancels the record.
// This epsilon is the shared push-versus-delete tolerance for push and extrude.
#define KXPUSH_EPS 1.0e-4f

enum kiwiFacePush_t
{
    KXPUSH_FAILED  = 0,   // nothing changed; *outWhy says why (never NULL)
    KXPUSH_PUSHED  = 1,   // the face moved; the brush survives
    KXPUSH_DELETED = 2,   // the push annihilated the brush, and it is gone
};

int KiwiXform_FacePushDepth( selbrush_t *node, int faceIndex, float *outDepth );
int KiwiXform_PushFaceOnce( selbrush_t *node, int faceIndex, float dist,
                            const char *undoOp, const char **outWhy );

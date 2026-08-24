#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_transform.h — RADIANT_UX_DESIGN §13 (G/R/S + axis locks + numeric entry),
// §20 (face push/pull, the flagship op), §21 (edge move), §22 (vertex move).
//
// Three modal commands, registered in the reserved MODAL range (kiwi_command.h):
//     KIWI_CMD_MOVE   34031   G
//     KIWI_CMD_ROTATE 34032   R
//     KIWI_CMD_SCALE  34033   S
//
// ── G IS CONTEXT-AWARE (spec §13's table) ───────────────────────────────────
// One command id, four behaviours, chosen from the DOMINANT kind in the current
// selection (objects > faces > edges > verts — a mixed selection transforms the
// dominant kind and the HUD says which):
//
//   SEL_OBJECT  whole-selection move   → the ported Select_Move (select.cpp
//               0x48E9C0), which already handles brushes, patches, entities and
//               fixed-size entities.  Nothing is re-implemented.
//   SEL_FACE    push/pull along the face's OWN normal (§20).  Multi-face moves
//               each face along its own normal by the same scalar, which is the
//               Plasticity behaviour §20 asks for.
//   SEL_EDGE    edge move (§21): re-solve the plane of every face adjacent to the
//               edge from the moved edge + a retained third point.
//   SEL_VERTEX  vertex move (§22): brush verts go through the PORTED solver
//               Brush_MoveVertex (brush.cpp 0x471C30); patch control points are
//               written from the baseline and re-tessellated by Patch_Rebuild.
//
// R and S are whole-selection only in v1 (spec §13's table marks face-rotate
// "later" and edge/vertex rotate/scale "— v1").
//
// ── THE THREE RULES THIS FILE IS BUILT ON ───────────────────────────────────
//
// 1. APPLY FROM BASELINE, never accumulate blindly.  Begin() snapshots what the
//    gesture will change (planepts / control points / a scalar of "how much has
//    been applied"); every MouseMove computes the TOTAL from the gesture start and
//    either re-derives the geometry from the baseline outright (faces, edges,
//    patch points) or pushes the residual `total - applied` through the legacy
//    mutator (objects, rotate, scale, brush verts).  A drag that wanders out and
//    comes back therefore lands EXACTLY where it started, and a snapped or typed
//    value is applied as an exact correction rather than as one more increment.
//
// 2. ONE GESTURE = ONE UNDO RECORD.  The bracket opens at the FIRST ACTUAL
//    MUTATION, not in Begin() — a gesture the user starts and escapes without
//    moving anything must leave no undo record at all (kiwi_command.h says the
//    same thing from the other side).  KiwiCmd_UndoCommit / UndoCancel are run by
//    the framework in KiwiCmd_Commit / KiwiCmd_Cancel.
//    A FACE selection needs extra care: face-selected brushes are NOT on
//    `selected_brushes` (kiwi_selection.h DESIGN NOTE 2), so
//    KiwiCmd_UndoBegin's Undo_AddBrushList would cover nothing — this file adds
//    each touched brush with Undo_AddBrush BEFORE mutating it, exactly as
//    kiwi_command.h's protocol note requires.
//
// 3. REUSE THE PORTED CORES.  Select_Move / Select_GetMid / Select_RotateAxis /
//    Select_ApplyMatrix_SelectedBrushes / Select_Scale / Brush_BuildWindings /
//    Brush_MoveVertex / Patch_Rebuild and the texture-lock pair
//    Face_TexLock_Save / Face_TexLock_Reproject do the actual work.  The new code
//    is the INPUT MAPPING, the constraint state and the validity gate.
//
// ── CURSOR MAPPING (Blender-style, spec §13) ────────────────────────────────
//   unconstrained  ray ∩ the plane through the gesture-start reference point
//                  whose normal is the camera vpn.  The normal is LATCHED at
//                  Begin (deliberate: camera navigation stays live during a modal
//                  command per §4, and a live vpn would swing the movement plane —
//                  and the geometry with it — every time the user orbits).
//   axis lock      the closest point on the axis line through the reference point
//                  to the cursor ray.
//   plane lock     ray ∩ the axis-normal plane through the reference point.
//   face push/pull the closest point on the face-NORMAL line through the face
//                  centre to the cursor ray, reduced to a signed scalar.
//
//   …AND ALL FOUR OF THOSE ARE GATED ON A HELD GIZMO HANDLE SINCE SHAKEOUT G.
//   USER REPORT, verbatim: "When selecting an object and pressing G (move), it
//   moves with the mouse.  It should only move with the gizmo."  G no longer maps
//   bare cursor travel to anything: the mappings above run ONLY while a move
//   handle is held (KiwiEditorCommand::HandleGrab, raised by the shared arm in
//   kiwi_command.cpp), and the total is otherwise LATCHED.  Typed values are the
//   other source and are unaffected.  The full before/after is in
//   KiwiMoveCommand::Recompute.  ROUND L adds one more rung to the same rule: for
//   the first frames of a grab, until the cursor leaves the pixel it was pressed
//   at, not even a snap target moves anything (KiwiMoveCommand::NoteGrab).
//
// ── ROUND P: GRID SNAPPING IS ABSOLUTE, GEOMETRY SNAPPING IS NOT ────────────
// USER DIRECTIVE, verbatim: "when moving something offgrid, the snaps no longer
// are aligned to the grid.  Fix this, the grid snaps should only be for the grid,
// no offset, custom offsets are done each time."
//
// The grid arm used to quantise the DELTA, which preserved whatever offset the
// object already had for ever.  It now snaps the MOVED REFERENCE POINT to an
// absolute grid position (`total = snap(ref + total) - ref`), so one grid-snapped
// move puts the thing ON the grid.  The face push/pull's scalar does the same
// whenever the push direction is a world axis — it snaps the moved PLANE's
// position along that axis — and keeps quantising the distance on a slanted
// normal, where there is no single axis to be on the grid of.  Both bodies carry
// the full argument.  GEOMETRY snaps (vertex / edge / midpoint / endpoint /
// intersection / face centre) are UNCHANGED: those name an exact target and were
// never a grid question.
//
//   S IS DELIBERATELY UNCHANGED and still free-drags (1 + dx * 0.005).  The
//   directive named Move and Rotate; scale has no handle set of its own yet
//   (kiwi_gizmo.cpp draws arrows for Move and rings for Rotate and nothing for
//   S), so gating it would leave S reachable only by typing a factor — i.e. it
//   would remove a working tool rather than fix a broken one.  Logged in
//   RADIANT_KNOWN_ISSUES as the scale-handle debt.
//   R              NOTHING.  SHAKEOUT G removed R's free-drag mapping outright
//                  (it was 0.5° per horizontal pixel, live from the moment R was
//                  pressed): the angle is LATCHED and changes only for a HELD
//                  ROTATE RING or a TYPED value.  User report, verbatim: "The
//                  rotate tool is bugged.  It needs to not do any rotation at all
//                  unless the gizmo is being dragged."  The full before/after is
//                  in KiwiRotateCommand::Recompute.
//   S              1 + dx * 0.005, clamped to > 0.01.
//
// Locking mid-gesture never jumps: the delta already accumulated is carried into
// the new constraint and then projected onto it (see SetConstraint in the .cpp).
// A second press of the same axis releases the lock (spec §13's "second press
// cycles world→local" is a LATER phase; v1 releases instead, and says so).
//
// ── NUMERIC ENTRY (spec §13, grammar v1 = one scalar) ───────────────────────
//   G   distance.  Requires a DIRECTION to be meaningful, so:
//         * axis-locked      → along that axis
//         * plane-locked     → along the in-plane component of the current mouse
//                              direction
//         * face push/pull   → along the push direction (the face normal, or the
//                              locked axis)
//         * unconstrained    → along the current mouse direction PROJECTED ONTO
//                              THE GROUND PLANE (Z=0) and normalised.  This is the
//                              documented rule for a bare scalar with no
//                              constraint: it is predictable (the HUD shows the
//                              direction's dominant axis), it never produces a
//                              vertical surprise, and typing an axis key first is
//                              always available for anything else.  With no mouse
//                              movement yet it falls back to +X.
//   R   degrees, exactly.      S   factor, exactly.
//   The numeric layer hands commands RAW WORLD UNITS (Units_FromDisplay already
//   applied — kiwi_numeric.h).  Degrees and factors are NOT lengths, so those two
//   run the value back through Units_ToDisplay to recover exactly what was typed.
//
// ── VALIDITY (spec §19) ─────────────────────────────────────────────────────
// Every apply ends with KiwiValid_Rebuild + KiwiValid_CheckBrush (kiwi_validity.h
// lists the checks).  A rejected apply restores the baseline, rebuilds again and
// raises an INVALID flag: the HUD turns red, and Commit while invalid takes the
// CANCEL path (baseline restore + undo rollback) instead of keeping the edit.
// Rejected geometry is never left live, not even for one frame.
// ─────────────────────────────────────────────────────────────────────────────

#include "kiwi_selection.h"          // sel_kind_t (the gizmo handoff below)

class KiwiEditorCommand;
struct ray_t;

// Model-drop placement is shared by an in-map drag and both browser ghosts.
// `modelMins` / `modelMaxs` are local XModel bounds.  The helper rotates and
// scales all eight corners, traces visible brushes, tessellated patches and existing
// model meshes, then falls back to global Z=0 for a downward miss.  It snaps only
// the hit point's X/Y and lifts along +Z until every support corner clears the hit
// tangent plane.  `supportCorners` optionally supplies group corners relative to
// the placed origin; browser drops use the transformed model bounds themselves.
#define KDROP_FLOAT 0.75f
bool KiwiDrop_ComputePlacement( const ray_t &ray,
                                const float modelMins[3], const float modelMaxs[3],
                                const float angles[3], float scale,
                                float outOrigin[3],
                                float outWorldMins[3] = 0,
                                float outWorldMaxs[3] = 0,
                                const float *supportCorners = 0,
                                int supportCornerCount = 0 );

// Plain camera LMB on an already-selected, model-only selection starts the drop
// variant of Move.  The viewport owns its press/release cycle; Esc remains the
// command framework's ordinary cancel path.
bool KiwiDrop_BeginAt( int imgX, int imgY );
bool KiwiDrop_Active();

// Registers the three rows into the shared `g_radiantCommands` table.  Called by
// KiwiCmd_RegisterCommands (kiwi_command.cpp) so there is still exactly one
// registration point.
void KiwiXform_RegisterCommands();

// The command object for a KIWI_CMD_MOVE / _ROTATE / _SCALE id, or NULL.
// Called by kiwi_command.cpp's CommandForId.
KiwiEditorCommand *KiwiXform_CommandForId( int commandId );

// §3 canExecute predicates for the palette metadata rows.
bool KiwiXform_CanMove();      // any movable item is selected
bool KiwiXform_CanRotate();    // at least one whole OBJECT is selected (v1 scope)
bool KiwiXform_CanScale();     // ditto

// ─── §14 GIZMO HANDOFF (shakeout A) ─────────────────────────────────────────
// The move gizmo is not a second transform: it is a second way to START the SAME
// KIWI_CMD_MOVE command (spec §13 "both the gizmo and G/R/S feed the SAME command
// object").  Three minimal entry points make that possible without exposing the
// command's internals:
//
//   KiwiXform_DominantKind    — the kind G would act on RIGHT NOW (objects >
//                               faces > edges > verts), so the gizmo can decide
//                               whether to appear at all.  Liveness-checked.
//   KiwiXform_PresetMoveConstraint — apply a handle's constraint to the LIVE move
//                               command.  Call IMMEDIATELY after a successful
//                               KiwiCmd_Start( KIWI_CMD_MOVE ) and never at any
//                               other time: it is a no-op unless the move command
//                               is the active one.  Routes through the command's
//                               own SetConstraint, so the "locking never jumps"
//                               re-latch (kiwi_transform.h CURSOR MAPPING) applies
//                               to a gizmo grab exactly as it does to pressing X.
#define KIWI_XCON_FREE   0
#define KIWI_XCON_AXIS   1
#define KIWI_XCON_PLANE  2

bool KiwiXform_DominantKind( sel_kind_t *out );
void KiwiXform_PresetMoveConstraint( int con, int axis );

// ─── SHAKEOUT D: the gizmos are now MODAL CHROME ────────────────────────────
// USER DIRECTIVE: "The Move gizmo should show up when pressing G, nothing should
// show up by default" and "the Rotate key should spawn a rotate gizmo".  The
// gizmo is therefore no longer an idle-selection affordance that STARTS a
// command — it is the live command's own handle set, drawn only while that
// command owns the viewport.  kiwi_gizmo.cpp needs exactly four things to do
// that, and they are deliberately COMMAND-IDENTITY questions rather than
// selection questions:
//
//   KiwiXform_IsMoveActive / IsRotateActive
//       Which of the two commands is the active one.  Identity comparison
//       against the file-static command objects (KiwiCmd_Active() == &s_move),
//       which is the same test KiwiXform_PresetMoveConstraint already used to
//       guard itself — exposing the PREDICATE rather than the pointers keeps the
//       command objects private and makes the gizmo unable to call into them
//       except through the two preset entries below.
//
//   KiwiXform_ActivePivot
//       Where the gizmo DRAWS AND HIT-TESTS: KiwiMoveCommand's LIVE anchor
//       (m_ref plus whatever the gesture has applied) or KiwiRotateCommand's
//       m_pivot.  KIWI-UX (CLEANUP, A-17): this is the ONLY anchor the gizmo may
//       use — a point re-derived from the live selection is a different thing
//       again (it would follow a selection change, not this gesture's own
//       applied delta).
//
//       ── ROUND L: THE ANCHOR RIDES ────────────────────────────────────────
//       USER REPORT, verbatim: "The pivot point is broken when moving a box
//       because the gizmo doesn't move with the object.  Fix this.  I should be
//       able to snap corners together easily by placing a pivot and moving it."
//       Shakeout D deliberately anchored on the LATCHED m_ref alone, reasoning
//       that a translate gizmo which crawls with its geometry runs away from the
//       cursor.  That reasoning holds only when the draw anchor and the MAPPING
//       anchor are the same value.  They are now separated: MapCursor still
//       measures from the latched m_ref (so the drag mapping cannot move under
//       the drag), and only the drawn/hit-tested anchor rides.  The arrows
//       therefore travel WITH the cursor, the handles stay on the object, and a
//       second grab starts from where the object now is.
//       A committed OBJECT move additionally carries the SESSION PIVOT with it
//       (KiwiMoveCommand::PivotRide), so a pivot placed on a corner is still on
//       that corner afterwards — which is what makes corner-to-corner snapping
//       repeatable rather than a one-shot.
//
//   KiwiXform_PresetRotateAxis
//       R's counterpart to KiwiXform_PresetMoveConstraint: aim the live rotate
//       at the grabbed ring's axis, through the command's OWN SetConstraint (so
//       the "re-aiming undoes what the old axis applied" rule in
//       KiwiRotateCommand::SetConstraint applies to a ring grab too).
//
//   KiwiXform_FeedRotateDegrees
//       The ring drag's SWEEP SINCE THE GRAB, in degrees, measured by the gizmo as
//       the cursor's angular travel about the pivot IN THE RING'S PLANE.  ROUND L
//       changed it from an ABSOLUTE angle to a sweep: the gizmo zeroes its
//       accumulator on every grab, so an absolute feed made the second grab of a
//       ring feed 0 and the apply-from-baseline residual span the selection back to
//       where it started.  The command latches the base the sweep is added to on
//       the grab edge (KiwiRotateCommand::HandleGrab).  This is the LEAST
//       INVASIVE entry into R that exists: R's Recompute already derives one
//       scalar `deg` and then runs it through the numeric override and the 5°
//       snap before ApplyDelta, so a ring simply REPLACES the source of that
//       scalar (horizontal pixels) and inherits snapping, the HUD, the undo
//       bracket and the apply-from-baseline residual untouched.  `active` false
//       hands the mapping back to the pixel rule.
bool KiwiXform_IsMoveActive();
bool KiwiXform_IsRotateActive();
bool KiwiXform_ActivePivot( float *out3 );
void KiwiXform_PresetRotateAxis( int axis );
void KiwiXform_FeedRotateDegrees( bool active, float degrees );

// ─── SHAKEOUT G: THE MOVE GRAB GATE ─────────────────────────────────────────
// USER REPORT, verbatim: "When selecting an object and pressing G (move), it
// moves with the mouse.  It should only move with the gizmo."
//
// The rotate half of that directive was answered by a RING that feeds a scalar
// (KiwiXform_FeedRotateDegrees above) — R never had a "handle position" to map
// against.  Move is the opposite shape: its four cursor mappings are already
// exactly right, they were just running whether or not anything was held.  So the
// fix is a GATE rather than a new mapping: while it is shut, Recompute leaves the
// total exactly where it is.
//
// ROUND L: THE GATE HAS NO ENTRY POINT OF ITS OWN ANY MORE.  It is raised through
// KiwiEditorCommand::HandleGrab, by the ONE arm every handle in the editor shares
// (kiwi_command.h THE ONE HANDLE-GRAB ENTRY) — the gizmo's arrows, its plane
// corners, its centre square, the rotate rings and the lollipop's ball all take
// the same five steps in the same order, which is the only way they can be
// guaranteed not to drift apart again.  Opening the gate re-latches the mapping
// start AND freezes the total until the cursor leaves the grab pixel, so no snap
// target, grid step or idle cursor offset can move anything on frame one.

// The live move's face-push direction, i.e. the drive face's own outward normal
// — the gizmo draws a FOURTH arrow along it while Move is in face context, which
// is the handle a push/pull actually wants (the three world arrows are still
// there and still axis-lock the push).  False when Move is not active or is not
// pushing faces.
bool KiwiXform_ActivePushDir( float *out3 );

// ─── SHAKEOUT G: THE MOVABLE PIVOT (V) ──────────────────────────────────────
// USER DIRECTIVE, verbatim: "For most operations, add an option to change the
// point of pivot.  For example in rotate mode press (V) and it lets you move the
// pivot with the mouse (supports snapping) and can be off the solid (origin,
// anywhere in 3D).  Save the pivot spot in memory (not disk) for reuse soon
// after."
//
// ── WHAT PLASTICITY DOES (read, not assumed) ────────────────────────────────
// `v` is bound PER GIZMO CONTEXT, never globally: default-keymap.ts:147
// `"v": "keyboard:move:pivot"` inside `[command="move"]`, :179
// `"v": "keyboard:rotate:pivot"`, :159 scale, :120 extrude.  The handler is
// TranslateCommand.ts:296-312 `onKeyPress`, whose 'pivot' arm FINISHES the
// running command and re-enqueues a fresh one with `choosePivot = true`; the
// placement itself is TranslateCommand.ts:314-330 `choosePivot`, which
// DISABLES the gizmo, runs a full PointPicker (i.e. the whole snap stack), copies
// the picked point into `gizmo.position` live, and on resolve writes it to BOTH
// `gizmo.pivot` and `factory.pivot` before re-enabling the gizmo.  MoveGizmo.ts:35
// keeps `pivot` as a SEPARATE vector from `position` (`position = pivot + delta`,
// :65/:83); RotateGizmo.ts:29 makes them the same thing (`get pivot() { return
// this.position }`) — which is exactly the KIWI split too (Move's m_ref is a
// constraint anchor, Rotate's m_pivot IS the rotation centre).
//
// TWO DELIBERATE DEVIATIONS:
//   1. V DOES NOT RESTART THE COMMAND.  Plasticity has to (its own comment,
//      TranslateCommand.ts:297: "both pivot & freestyle rely on snap points, which
//      are only updated on commit"); KIWI's SnapManager reads the live brush lists
//      every query, so the placement runs INSIDE the gesture and the wip edit
//      survives it.  Restarting would throw away a half-finished move for nothing.
//   2. THE PIVOT PERSISTS.  Plasticity's is per-command — `choosePivot` is false
//      on every fresh command, so the next one falls back to the bbox centroid
//      (TranslateCommand.ts:326-329).  The directive explicitly asks for reuse, so
//      KIWI keeps ONE session pivot in memory.
//
// RESET RULE, chosen and stated: the pivot is dropped when the SELECTION SIGNATURE
// changes — the item count plus a mix of every item's (instance pointer, kind,
// face/edge/vert index), plus the construction-selection count.  NOT
// Sel_Generation(), which bumps on every sync even when the same things end up
// selected (Sel_SyncToLegacy alone bumps it), and which would therefore throw the
// pivot away between the two commands the user wants to share it.  Committing a
// move leaves the signature alone (the same items are still selected, they just
// moved), so "reuse soon after" works; clicking a different brush changes it and
// the pivot goes.  NEVER PERSISTED: no ini key, no profile entry, gone on exit.
bool KiwiXform_PivotOverride( float *out3 );   // false = no session pivot in force
bool KiwiXform_PivotPlacing();                 // true while V-placement is live

// ── KIWI-UX (ROUND BK, ITEM 6b): should the SNAP CANDIDATE DOTS be drawn? ────
// USER DIRECTIVE, verbatim: *"when hunting for a pivot point, the obvious spots
// (centers, corners, points, midways, etc.) need to have a black dot to show where
// they are."*  True while a transform is placing its pivot (V) or is holding a
// handle — the two states where the user is aiming at a point rather than looking
// at a result.  Asked by KiwiSnap_DrawFaceAccents (kiwi_snap.cpp), which owns the
// enumeration; this only widens its gate, and the reasoning for keeping that gate
// otherwise tight is on the definition.
bool KiwiXform_WantsSnapDots();

// ─── ROUND Q: THE FACE PUSH AS A ONE-SHOT (un-extrude to destroy) ───────────
// USER DIRECTIVE, verbatim: "Make it so that I can un-extrude faces entirely to
// DESTROY them.  This is allowed in plasticity."
//
// THE DIAGNOSIS.  The push-through DELETE state (shakeout G) was never broken —
// it is reached from the lollipop exactly as designed, because a face CLICK in
// mode 3 auto-enters KIWI_CMD_MOVE (kiwi_boxselect.cpp) and the ball drives that
// command's RecomputeFace, whose delete arm is tested every frame and BEFORE the
// §19 invalid arm.  What was blocked is the other face verb: **E**.
// KiwiExtrudeFaceCommand grows a new body and REFUSED a negative distance
// outright — `m_invalid = ( d < KEXT_MIN_DIST )` per frame plus a Commit that
// printed "distance must be positive" and did nothing.  Pull the lollipop back
// through the brush under E and the HUD went red and confirm was a no-op; that
// is the "un-extrude does not destroy" the directive is about.
//
// THE WIRING.  Negative E now MEANS push/pull: the source face is carved inward,
// and once the carve annihilates the solid it is the DELETE.  Rather than give
// kiwi_extrude.cpp its own copy of the delete rule, the §19 gate, the texture-
// lock bracket and the Select_Delete ordering — a second implementation of four
// subtle things — the push is exported from here as a one-shot and E calls it.
// One delete rule in the editor.
//
// KiwiXform_FacePushDepth reports the same threshold the interactive push uses:
// the brush's thickness along that face's outward normal, i.e. how far the face
// has to travel INWARD before the half-space intersection is empty.  0 = the
// question could not be asked (dead node, no such face, no winding).
//
// KiwiXform_PushFaceOnce applies `dist` along the face's baseline outward normal
// (negative = inward) in ONE undo record, opened with KiwiCmd_UndoBegin and left
// OPEN for the framework's KiwiCmd_UndoCommit to close — the same bracket shape
// every other commit in this layer uses.  `undoOp` must be a STRING LITERAL
// (Undo_GeneralStart stores the pointer).  A rejected push restores the face and
// CANCELS the record, so a refusal leaves nothing on the stack.
// KIWI-UX (CLEANUP, A-26): THE SLACK ON THE PUSH-THROUGH TEST, EXPORTED.
// `depth > KXPUSH_EPS && dist <= -depth + KXPUSH_EPS` is the comparison that
// decides PUSH versus DELETE.  kiwi_extrude.cpp's HUD has to predict that answer
// before E commits, so it ran the same comparison with its own copy of the
// number (KEXT_PUSH_EPS) — and kiwi_extrude.cpp:1865 states outright that "the
// two comparisons are the same comparison", which was true only for as long as
// two unrelated literals happened to agree.  If they drifted, the HUD would
// promise a carve and the helper would delete the brush.  One number now, read
// by both; the value is unchanged.  kiwi_transform.cpp's file-local KX_EPS is
// defined FROM this, so the file still has exactly one tolerance.
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

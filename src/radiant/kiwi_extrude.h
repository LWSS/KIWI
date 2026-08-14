#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_extrude.h — RADIANT_UX_DESIGN §23: Region extrusion → ordinary brushes.
//
// ONE modal command (KIWI_CMD_EXTRUDE_REGION, 34039).  Invoked with the cursor
// over a region — or with exactly one region in the store, in which case that is
// the one.  The gesture drags along the region plane's NORMAL (the same
// closest-point-on-the-normal-line mapping face push/pull uses, kiwi_transform.h
// "CURSOR MAPPING") or takes an exact typed distance; commit lands brushes.
//
// ── THE OUTPUT IS ORDINARY BRUSH DATA ───────────────────────────────────────
// No new format, no new lifecycle.  The creation sequence is transcribed from
// the ported creator Ed_NewBrushDrag (xywnd.cpp 0x467fa0), in its order:
//
//   Ed_EnsureCurrentMaterial_Kiwi()                      // xywnd.cpp forwarder
//   def  = Brush_Alloc( g_qeglobals.random_texture_stuff, nullptr )
//   Ed_BrushSetFaceCount( def, n )                       // brush.cpp forwarder
//   ...write planepts...
//   Brush_BuildWindings( def, 0 )                        // via KiwiValid_Rebuild
//   Entity_LinkBrush( def, (entity_s *)world_entity->def )
//   inst = Brush_AddToList( def, world_entity )
//   Brush_AddToList2( inst )                             // → selected_brushes
//
// Two documented divergences from that creator, both forced:
//   * Brush_Alloc always allocates SIX faces and Brush_Create writes a box into
//     them.  A prism over an n-gon needs n + 2, which for a triangle is FIVE.
//     Ed_BrushSetFaceCount is a // KIWI-UX forwarder in brush.cpp that performs
//     the SAME in-place face-array swap the ported Brush_MakeSided (0x4731E0)
//     performs for exactly this reason — Face_Free, Face_Alloc_R(n), re-stamp the
//     current MaterialDef and the white packed colour, Brush_SetDefaultMaterials.
//     Nothing new is invented; the ported primitive's own sequence is exposed.
//   * the rebuild is bFull 0, not Ed_NewBrushDrag's bFull 1.  bFull 1 runs
//     Brush_SnapPlanepts, i.e. the LEGACY power-of-two grid, which would quantise
//     a circle profile into a polygon the user never drew.  kiwi_validity.h
//     already fixes bFull 0 as the modern layer's one spelling of the rebuild.
//
// ── UNDO (verified in undo.cpp, not assumed) ────────────────────────────────
// Undo_EndBrushList (0x45E870) stamps `def->ownerPrev = undo->id` on every brush
// in the list it is handed.  Undo_Undo's Phase 1 then REMOVES every live brush
// carrying that stamp, and its Phase 4 re-links the clones Undo_AddBrush saved.
// So a brush that was stamped but never cloned simply disappears — which is
// exactly "undo a creation".  The requirement that falls out of that pair:
// everything Undo_AddBrushList cloned at BEGIN must still be in the list at
// COMMIT, or its clone would be restored while the live original stayed and the
// brush would double.  This command therefore does
//
//     Select_Deselect(1)  →  KiwiCmd_UndoBegin("extrude region")  →  create
//
// in that order, at Commit time (the gesture mutates nothing before then, so per
// kiwi_command.h rule 2 no bracket may exist before then either).  The bracket
// opens over an EMPTY selection, the new brushes land selected, and
// KiwiCmd_UndoCommit's Undo_EndBrushList stamps precisely them.
//
// ── THE GEOMETRY ────────────────────────────────────────────────────────────
// A convex profile becomes ONE brush: one side plane per profile edge (through
// the edge, parallel to the extrusion axis) plus two cap planes.  A concave
// profile is ear-clipped and Hertel-Mehlhorn-merged into convex pieces
// (kiwi_region.h) and becomes one brush per piece, all landing selected together.
//
// Every plane is written as THREE PLANEPTS, never as a normal+distance: face_t
// has no independent plane field the editor trusts — Face_MakePlane recomputes
// `plane` from planepts on every rebuild, and .map serialization writes the
// planepts.  The three points are chosen well-spread (a side face uses the two
// edge ends at the two cap heights; a cap uses three profile vertices picked for
// maximum spread) so the cross product never collapses.  Winding order is chosen
// so Face_MakePlane's normal — cross(p0 - p1, p2 - p1) — comes out OUTWARD.
//
// ── REJECTION IS FREE, BECAUSE VALIDATION HAPPENS BEFORE LINKING ────────────
// Brush_BuildWindings and the §19 checks read only planepts / faces / windings —
// never def->owner — so every piece is built and gated while it is still an
// UNLINKED def.  A rejection therefore frees the defs with Brush_Free_R (whose
// refCount == 0 / no owner-chain precondition an unlinked def satisfies by
// construction) and touches nothing else: not the map, not the selection, and
// not the undo stack.  The alternative — land, gate, unwind — leaves an empty
// undo record for the user to step over.
//
// Rejections, each with a console message and no geometry:
//   * |distance| below KEXT_MIN_DIST
//   * profile with more than KEXT_MAX_PROFILE vertices (v1 cap)
//   * a decomposition that produces more than KEXT_MAX_PIECES pieces
//   * a self-intersecting or all-collinear profile
//   * any piece that fails KiwiValid_CheckBrush after the rebuild
// ─────────────────────────────────────────────────────────────────────────────

class  KiwiEditorCommand;
struct kconPlane_t;               // kiwi_construct.h
struct brush_t;                   // qe3.h:474 (the 88-byte brush DEFINITION)
struct selbrush_t;                // qe3.h:429 (the 56-byte brush INSTANCE / list node)

#define KEXT_MIN_DIST      0.5f   // world units — below this the gesture is a no-op

// ── KIWI-UX (ROUND X, ITEM 4a): THE SELF-SNAP BAND ──────────────────────────
// USER DIRECTIVE, verbatim: "When extruding, dont allow snapping to self, it's
// just an annoyance fix."
//
// An extrusion is a ONE-AXIS gesture measured from a start plane, and everything
// the extrusion is made OF lies on that plane: the source face and its winding
// corners, the source region's own boundary lines and their endpoints, every
// midpoint of both.  Those are the snap candidates nearest the cursor for the
// whole first part of the drag, and each of them resolves to a scalar of ZERO —
// so the depth is glued to "no extrusion at all" exactly where the user is trying
// to leave it.  (It is also, by itself, the whole of ITEM 9's "releasing the mouse
// snaps back to the starting value": zero IS the starting value.)
//
// A geometry snap whose projection onto the extrusion axis lands inside this band
// is REFUSED, and the gesture keeps its cursor-mapped distance instead.  Nothing
// is lost: the command already rejects |d| < KEXT_MIN_DIST as a no-op, so the
// refused snaps could never have produced a legal result anyway.  The band is
// deliberately WIDER than KEXT_MIN_DIST — a snap that is merely *near* zero is
// just as sticky as one that is exactly zero, and the user has the numeric field
// and Ctrl (snapping off) for the rare deliberate hair's-breadth push.
//
// It is a rule about the SOURCE PLANE, not about a list of objects: it needs no
// exclusion set, it cannot go stale mid-gesture, and it catches construction
// geometry (which the pick layer's PICKF_EXCLUDE_SELECTED cannot see at all) on
// exactly the same terms as brush geometry.
#define KEXT_SELF_SNAP_BAND 2.0f  // world units either side of the start plane
// ROUND AG, ITEM 2: KREG_MAX_LOOP comes from here, and the include is what makes
// the macro below safe to expand in any TU rather than only in one whose include
// order happens to be right.
#include "kiwi_region.h"

// ROUND AG, ITEM 2: these TRACK KREG_MAX_LOOP and must keep tracking it.  The
// invariant the round is about is "extrudable if and only if filled", and a
// profile cap below the region's loop cap breaks it BY CONSTRUCTION — the fill
// would appear and E would then refuse it.  KREG_MAX_LOOP went 64 -> 128 because
// a derived cell is bounded by parts of SEVERAL objects (kiwi_region.h THE LOOP
// CAP IS NOT THE TESSELLATION CAP), so these follow.
//
// KEXT_MAX_PIECES follows for the same reason at one remove: Hertel-Mehlhorn on
// an n-gon starts from n-2 triangles and only merges, so a 128-vertex concave
// profile can need up to 126 pieces and a cap of 64 would refuse it after the
// fill had already promised it would work.  128 BRUSHES from one extrude is a
// lot; it is also what the geometry asks for, it is bounded, and the refusal
// past it is loud.
#define KEXT_MAX_PROFILE   KREG_MAX_LOOP   // profile vertex cap (== the region cap)
#define KEXT_MAX_PIECES    128    // convex pieces one concave region may produce

void KiwiExtrude_RegisterCommands();
KiwiEditorCommand *KiwiExtrude_CommandForId( int commandId );
bool KiwiExtrude_CanExecute();          // §3 palette predicate: at least one region

// ── SHAKEOUT G: E — EXTRUDE FACE (KIWI_CMD_EXTRUDE_FACE, 34058) ─────────────
// USER DIRECTIVE (owed from an earlier round), verbatim: "extrude (E) […] new
// body from extrusion".
//
// A NEW PRISM BRUSH grown off a face winding along that face's normal.  The
// original brush is left COMPLETELY UNTOUCHED — not rebuilt, not re-selected, not
// in the undo bracket — which is the whole difference between this and G's face
// push/pull (§20, which moves the face's plane and reshapes the solid).
//
// It reuses KiwiExtrude_BuildPrismDef / KiwiExtrude_LandDef below verbatim: a
// face winding IS a convex plane profile, so the only new work is turning the
// face into (plane, CCW plane-space loop) and testing the winding's sense.
//
// ═══════════════════════════════════════════════════════════════════════════
//  ROUND Q — E GOES BOTH WAYS: UN-EXTRUDE, AND UN-EXTRUDE TO DESTROY
// ═══════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: "Make it so that I can un-extrude faces entirely to
// DESTROY them.  This is allowed in plasticity."
//
// WHAT BLOCKED IT.  Shakeout G shipped the push-through DELETE on the §20 face
// push/pull and it works — a face click in mode 3 auto-enters KIWI_CMD_MOVE
// (kiwi_boxselect.cpp), the lollipop drives that command, and its RecomputeFace
// tests the delete arm every frame ABOVE the §19 invalid arm.  E was the hole.
// This command grew a body and refused a negative distance OUTRIGHT: it set
// `m_invalid = ( d < KEXT_MIN_DIST )` on every frame, so pulling the lollipop
// back through the brush painted the HUD red, and its Commit printed "distance
// must be positive" and did nothing.  Under E there was no way to un-extrude
// anything, let alone destroy it — which is exactly the report.
//
// WHAT E MEANS NOW.  Three outcomes on one drag, decided by the sign, recomputed
// every frame with no latching so dragging back and forth crosses freely:
//
//   OUT (d >= +KEXT_MIN_DIST)   GROW    — a NEW BODY.  Unchanged, original verb.
//   IN  (d <= -KEXT_MIN_DIST)   CARVE   — the SOURCE face is pushed inward.  This
//                                         IS §20's push/pull, delegated to it.
//   IN, past the brush's own thickness along that normal
//                               DESTROY — the carve annihilates the solid, so the
//                                         brush is removed.
//
// "Past the brush's own thickness" is KiwiXform_FacePushDepth (kiwi_transform.h
// §Q) — the SAME threshold the interactive push uses, read from the same
// function, measured ONCE at Begin from untouched geometry.
//
// THE WIRING, and why it is a delegation and not a reimplementation.  The commit
// calls KiwiXform_PushFaceOnce, which is the interactive push's own code factored
// out: the delete rule, the §19 gate, the Brush_Move texture-lock bracket and the
// Select_Deselect → head → Undo_AddEntity_W → Select_Brush → Select_Delete
// removal ordering all stay in ONE place.  This file contributes the distance.
//
// THE COMMAND STILL MUTATES NOTHING UNTIL COMMIT, negative arm included — that is
// this command's standing contract and the negative arm keeps it.  So the drag
// draws the picture instead: the source winding translated to where the plane
// will land (amber) while carving, and every winding of the doomed brush in the
// delete red once the carve goes through.  The HUD says it in words as well, and
// says which of the three it is BEFORE the confirm, every frame.
//
// A negative distance is NOT "invalid": HudInvalid() drives the red numeric HUD
// and the framework's "commit takes the cancel path", which is precisely the
// behaviour being removed here.  Only |d| < KEXT_MIN_DIST is invalid now.
//
// ── ROUND AP, ITEM 1: THIS SURVIVES THE AUTO-CARVE WITHDRAWAL ──────────────
// USER DIRECTIVE, verbatim: "when extruding, dont automatically bool diff the
// solid.  It can be done by the user with a boolean after."  That withdrew round
// AF item 2 — the REGION command's push-in cave, which probed for solid under the
// drag and turned the prism into a KiwiBool_DifferenceByDef tool against every
// brush it met.  The three outcomes above are a DIFFERENT feature and are kept,
// on three grounds that the region cave failed on all three:
//   * it is not a boolean.  KiwiXform_PushFaceOnce moves ONE face plane of ONE
//     brush — the same edit the lollipop performs by hand;
//   * it touches only the brush whose face the user grabbed, never a bystander;
//   * its trigger is the SIGN OF THE DRAG, which the user is steering directly.
//     The cave's trigger was a hidden point probe, so the same drag could mean
//     two different things depending on geometry the user was not looking at.
// If the un-extrude is ever unwanted, it is a separate directive about a separate
// gesture; nothing in the AP report reaches it.
//
// E IS A CONTEXT VERB: KiwiExtrude_CommandForId returns the FACE command when a
// face is selected or hovered and the §23 REGION command otherwise, so one key
// covers both — see the note on that function.  The result lands selected and
// enters a PAUSED Move (KiwiCmd_StartDeferred), the same handoff paste and clone
// perform, so a new body can be placed with the gizmo without pressing anything.
//
// The face E acts on: the ACTIVE selected face, else the first selected face,
// else the HOVERED one.  False when there is none.
bool KiwiExtrudeFace_Pick( selbrush_t **outNode, int *outFace );
bool KiwiExtrudeFace_CanExecute();      // §3 palette predicate: a face is available

// ── §16b (shakeout C): the prism writer, exported ────────────────────────────
// The §16b Box primitive (kiwi_primitive.cpp) is a convex prism over a 4-gon —
// which is exactly what this file already builds, with winding rules that were
// worked out rather than guessed (see the ORIENTATION note in the .cpp).  A
// second copy of that math in kiwi_primitive.cpp would be a second place for it
// to drift, so the two entry points are exported instead.
//
//   `loopUV` is `n` CCW plane-space points (2 floats each);
//   `lo`/`hi` are the cap offsets along plane.normal, hi > lo;
//   the returned def is UNLINKED and UNBUILT — the caller runs KiwiValid_Rebuild
//   + KiwiValid_CheckBrush on it and then either Brush_Free_R's it (a rejection
//   costs nothing, because nothing was linked) or lands it.
// Returns NULL with *why set on a degenerate profile.
brush_t *KiwiExtrude_BuildPrismDef( const kconPlane_t &plane, const float *loopUV, int n,
                                    float lo, float hi, const char **why );

// The ported Ed_NewBrushDrag tail, in its order: Entity_LinkBrush →
// Brush_AddToList → Brush_AddToList2 (i.e. the new brush lands SELECTED).
selbrush_t *KiwiExtrude_LandDef( brush_t *def );

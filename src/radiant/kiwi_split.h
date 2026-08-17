#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_split.h — SHAKEOUT G: the ONE two-halves splitter, and the two verbs
// built on it — CUT (C) and FACE SPLIT (Ctrl+R).
//
// USER DIRECTIVES, verbatim:
//   "Add a 'cut' feature (C) that requires at least 1 selected solid (brush) and
//    then a line is selected to cut along.  The solid(Brush) is split into 2
//    exactly where the line indicates.  There is a preview before the final
//    commit with a giant red plane swept across from the line to the solid (In
//    the direction AWAY from the camera) (From plasticity)"
//   "When a face is selected, allow pressing of Ctrl-R to 'split' the face in 2.
//    Pressing [tab] with this action changes the Direction from U to V"
//
// ── WHAT PLASTICITY ACTUALLY DOES (read, not assumed) ───────────────────────
// CUT.  plasticity/src/commands/boolean/CutCommand.ts + CutFactory.ts.
//   * inputs: 1..N SOLIDS (CutCommand.ts:30, `SelectionMode.Solid`) plus curve /
//     face cutters (CutCommand.ts:56).
//   * the cutting surface is the cutter curve EXTRUDED along a placement's Z
//     axis — `const Z = vec2vec(placement.GetAxisZ(), 1);` then a bounding-cube
//     length (CutFactory.ts:112-116).  The placement is the VIEWPORT'S
//     CONSTRUCTION PLANE first (CutCommand.ts:13 `cut.constructionPlane =
//     this.editor.activeViewport?.constructionPlane;`, precedence at
//     CutFactory.ts:278-280).
//   * the preview is a RED, 10%-opaque, DOUBLE-SIDED extruded surface
//     (CutFactory.ts:122 `const material = { surface: surface_red };`,
//     :333-343 `mesh_red.color.setHex(0xff0000); mesh_red.opacity = 0.1;`).
//   * the result is TWO solids (`__tests__/commands/Cut.test.ts:49`,
//     `expect(result.length).toBe(2);`).
//   * bound to a bare `c` (src/startup/default-keymap.ts:257).
//
//   ONE DELIBERATE DEVIATION, and it is the user's own wording: the sweep
//   direction here is AWAY FROM THE CAMERA, not the construction plane's normal.
//   KIWI's construction plane is a §16 concept the user does not have to have set
//   up before pressing C, and "swept away from the camera" is what the directive
//   asks for and what the picture shows.  The two agree in the common case
//   anyway — a line drawn on the active construction plane with the camera
//   looking at it sweeps the same way.
//
// FACE SPLIT.  Plasticity HAS a split factory and it is DEAD CODE from the UI:
// `SplitFactory` (CutFactory.ts:176) wraps `c3d.ActionSolid.SplitSolid_async`
// (:200) and is reachable only through `CutAndSplitFactory` (:207-240), which
// `CutCommand` never instantiates.  Its behaviour is also NOT ours: the test at
// __tests__/commands/Cut.test.ts:178-191 shows a 6-face box becoming a 7-face
// box — ONE solid with one face divided.  The user asked for the brush to be
// "split in 2", and a classic brush is a convex half-space intersection that
// cannot carry a divided face, so KIWI's Ctrl+R splits the BRUSH along the plane
// through the split line — which is why it shares Cut's machinery outright.
//
// ── THE SHARED SPLITTER ─────────────────────────────────────────────────────
// Both verbs reduce to "cut brush B by the plane through p0/p1/p2", and that is
// ONE function, KiwiSplit_BrushByPlane.  It is a thin, gated wrapper over the
// PORTED core the classic clipper uses:
//
//   Brush_SplitBrushByFace( def, &face, &front, &back )      brush.cpp 0x471960
//
// which clones the def twice, Face_Allocs the template face onto one clone and
// its REVERSED twin onto the other, rebuilds both, drops empty/coincident faces
// and hands back only halves that still have >= 4 faces (NULL otherwise).  Both
// halves come back already Entity_LinkBrush'd to the source's owner entity, and
// NOT on any display list — exactly what Ed_ProduceSplitLists (xywnd.cpp) and
// CSG_MakeHollow (csg.cpp) each then link themselves.
//
// ── THE TEMPLATE FACE'S MATERIAL (ROUND T CHANGED THIS) ─────────────────────
// It USED TO BE the caulk / nodraw_decal synthesis the clipper performs, through
// the // KIWI-UX forwarder Ed_BuildClipFaceMaterial_Kiwi (xywnd.cpp).  USER
// DIRECTIVE: "Make it so the texture is just inherited from the parent brush that
// are being operated on.  The caulk texture is not usable."
//
// So the KIWI verbs now seed it with kiwi_material.h's rules: the source brush's
// largest INHERITABLE face whose plane is most perpendicular to the cut (R2),
// falling back to the classic caulk synthesis only when the brush has no
// inheritable face at all (R3 — a caulk block still cuts into caulk).  The
// CLASSIC CLIPPER is untouched and still caulks, which is the binary's behaviour.
// A zeroed template would hand both halves an unrealised MaterialDef either way,
// which is why there is always a seed.
//
// WHAT THIS ADDS OVER THE CORE, and why each piece is here:
//   * a §19 gate on BOTH halves before either is linked.  Brush_SplitBrushByFace
//     already rejects a half with fewer than 4 faces, but not a sliver, a
//     zero-area face or a crossed pair — and a half that fails must not reach the
//     map, so the whole split is refused and the source brush left untouched.
//   * REJECTION IS FREE, on the kiwi_extrude.h terms: the halves are gated while
//     they are unlinked defs, so a refusal frees them with Entity_UnlinkBrush +
//     Brush_Free_R (the same pair CSG_MakeHollow uses on the piece it discards,
//     csg.cpp:365-369) and touches neither the map, the selection nor undo.
//   * the source instance is freed by the CALLER, not here, and only after both
//     halves are linked — see the undo note below.
//
// ── UNDO: ONE RECORD FOR "REMOVE ONE, ADD TWO" ──────────────────────────────
// The shape is CSG_MakeHollow's, whose wrapper (mainfrm.cpp Cmd_OnSelectionMake-
// hollow, :2300-2306) does exactly this — free the originals, add the pieces, one
// record:
//
//   Undo_ClearRedo()  →  Undo_GeneralStart(op)  →  Undo_AddBrushList(&selected_brushes)
//   …split: land both halves (Brush_AddToList + Brush_AddToList2 = selected),
//           THEN Brush_Free the source instance…
//   Undo_EndBrushList(&selected_brushes)  →  Undo_End()
//
// i.e. KiwiCmd_UndoBegin / KiwiCmd_UndoCommit verbatim (kiwi_command.h).  Why it
// is correct, read out of undo.cpp rather than assumed:
//   * the HEAD's Undo_AddBrushList CLONES every selected brush — including every
//     brush this verb is about to consume.  Undo_Undo's Phase 4 re-links those
//     clones, which IS "the original comes back".
//   * the TAIL's Undo_EndBrushList (0x45E870) stamps `def->ownerPrev = undo->id`
//     on everything in the list AT THAT MOMENT — the two halves.  Undo_Undo's
//     Phase 1 removes every live brush carrying the stamp, which IS "the halves
//     go away".
//   * ORDER: land the halves BEFORE freeing the source, so the owner entity never
//     transiently drops to zero brushes (the halves are linked to the SAME owner,
//     Brush_SplitBrushByFace's Split_LinkCloneToEntity( b, in->owner )).  This is
//     also why the source is freed with Brush_Free (the instance) and not through
//     Select_Delete: Select_Delete additionally frees an owner entity that ran out
//     of brushes, which by construction cannot happen here.
//   * kiwi_extrude.h's rule — "everything the head cloned must still be in the
//     list at commit or the brush doubles" — is satisfied in the OTHER direction:
//     what the head cloned is gone from the map, so restoring the clone restores
//     it exactly once.
//
// ── ROUND L: THE TWO-STAGE CUT ──────────────────────────────────────────────
// USER DIRECTIVE, verbatim: "The cut workflow is clunky.  It should be: Select a
// solid, press C, then the selection expects a line to be selected.  Also the way
// it cuts needs to be decided at line click-time and not update as the camera
// moves."
//
// Both halves of that were wrong in shakeout G, and for the same underlying
// reason — the command had no STAGES, so everything had to be true before C was
// pressed and everything stayed live afterwards:
//
//   WAS                                     IS
//   C refuses unless a construction line    C needs >= 1 splittable brush and
//   is ALREADY selected (KiwiSplit_CanCut   nothing else.  It refuses only when
//   ran CutLine over the conselect list).   the map has no construction segment
//                                           at all, because "click a line" is
//                                           unanswerable from inside a gesture.
//   ---------------------------------------------------------------------------
//   the plane is re-derived from the LIVE   STAGE 1 "pick a line": the segment
//   camera on every hot frame, so orbiting  under the cursor highlights (10 px,
//   to inspect the preview silently         KCON_LINE_PIXELS — the shared
//   re-aims the cut.                        construction clickbox) and CLICKING it
//                                           LOCKS the plane THEN AND THERE, from
//                                           that line and the camera's vpn at that
//                                           instant.  Nothing re-derives it after.
//                                           STAGE 2 "preview": the red sweep quad
//                                           and the outlines, RMB / Enter cuts.
//
//   Esc walks BACK one stage (preview -> pick a line) and cancels from stage 1,
//   which is the ladder the drawing tools already teach.
//
// The line is picked by a LOCAL segment scan rather than through
// KiwiConSel_PickAt, for the reason Match Face re-casts its own ray: that entry
// point answers at the granularity of the CURRENT SELECTION MODE and names no
// segment at all in Object / Face / All mode.  The tolerance is the shared
// constant, so the clickbox is identical.
//
// ── THE CUT PLANE (C) ───────────────────────────────────────────────────────
// Inputs: >= 1 selected BRUSH and the construction SEGMENT clicked in stage 1.
// From the segment's two world endpoints p0, p1:
//
//     u    = normalise( p1 - p0 )                     the line direction
//     away = normalise( vpn - u * (vpn . u) )         the camera's forward,
//                                                     projected PERPENDICULAR to
//                                                     the line
//     n    = normalise( cross( u, away ) )            the cutting plane's normal
//
// so the plane CONTAINS the line and sweeps along `away`, i.e. straight into the
// screen — "swept across from the line to the solid, in the direction AWAY from
// the camera".  `vpn` is camera_s.vpn, which points INTO the scene (kiwi_hover.cpp
// nudges by -vpn to move a point toward the eye), so no sign flip is needed.
// DEGENERATE CASE: a line pointing at the camera makes `away` collapse; the
// command refuses to start and says so, because there is no such plane.
//
// THE PLANE IS LOCKED AT THE CLICK AND NEVER RE-DERIVED (ROUND L).  Shakeout G
// re-aimed it from the live camera on every hot frame and froze it only when the
// gesture paused; the directive is explicit that this is the wrong trade — an
// orbit meant to inspect the preview silently re-aimed the cut.  AimFromCamera is
// now called from exactly ONE place, KiwiCutCommand::Click, and everything it
// writes is the gesture's plane from then on.  To aim differently: Esc back to
// stage 1, orbit, click the line again.
//
// ── THE FACE SPLIT LINE (Ctrl+R) ────────────────────────────────────────────
// With exactly one FACE selected: the split line lies IN the face plane, runs
// along U (perpendicular to the face's LONGEST EDGE; TAB switches to V, the
// in-plane perpendicular) and SLIDES along the perpendicular of whichever of the
// two it currently is.  The cutting plane is that line swept along the FACE
// NORMAL, so the cut is square to the face.
//
// ── ROUND S: THE LINE IS LIVE, AND IT SPLITS THE BRUSH ──────────────────────
// USER DIRECTIVE, verbatim: "You misunderstood the split command (ctrl-R).  It
// only splits the face, not the whole brush (although that might be a tech
// limitation?).  Also it needs to have a live update when hovering the mouse
// around to where it's going to split, it's not always just 50/50, let me
// customize it."
//
// THE FIRST HALF IS A REAL TECH LIMITATION AND IS NOW SAID OUT LOUD.  A Radiant
// brush is a CONVEX SOLID defined as the intersection of its faces' half-spaces
// (Brush_BuildWindings, brush.cpp:1459 — every winding is derived by clipping the
// face's plane against every OTHER face's plane).  A face has no independent
// existence to divide: "two faces where there was one" is only expressible as two
// coplanar faces belonging to two different convex solids, which IS the brush
// split.  There is no representation in which the solid stays one brush and the
// face becomes two.  So the command splits the BRUSH, and the HUD and the console
// line say so in those words rather than leaving the user to infer it.
//
// THE SECOND HALF IS THE WORK.  The line no longer sits at the centroid: it
// tracks the cursor for the whole gesture.
//
//   THE SLIDE AXIS.  offDir = normalise( cross( faceNormal, lineDir ) ) — in the
//   face plane, perpendicular to the line, so moving along it is exactly "where
//   the cut lands".  The face's own winding is projected onto it once per derive
//   to give the SPAN [lo, hi]; the cut position is one scalar `t` in that span.
//
//   THE CURSOR MAPPING.  ray ∩ THE FACE PLANE (not the snap layer's own fallback
//   plane — the cut must stay on the face even when the cursor leaves it), then
//   t = dot( hit, offDir ), clamped into (lo, hi) with a one-unit guard at each
//   end so a commit can never ask for an empty half.
//
//   SNAP.  The framework's snap result is used AS IT COMES (kiwi_snap.h), on the
//   same rule kiwi_primitive.cpp uses: a GEOMETRY snap (vertex / endpoint /
//   midpoint / intersection / edge / face centre) wins outright when it lies ON the
//   face plane, because that is a target the user aimed at on purpose; otherwise
//   the plane hit is passed through KiwiGrid_Snap — the §17 WORLD-ANCHORED lattice,
//   so the cut lands on the same grid every other placement does.
//     SNAP_FACE is EXCLUDED from that first rank even though KiwiSnap_IsGeometry
//     answers true for it.  Arm 6 is the ported Test_Ray surface hit, UNSNAPPED,
//     and it fires on the very face being split — so taking it would mean the cut
//     could never grid-snap, because the grid arm would never be reached.
//   Ctrl (the §6 snap suppression) needs no code here: the snap layer answers
//   SNAP_NONE, which is not a geometry type, and the raw plane hit is used.
//
//   NUMERIC ENTRY, AND ITS REFERENCE.  ONE field, "offset", in inches.  It is
//   measured from the face edge at the LOW end of the slide axis — i.e. from
//   `lo`, the minimum projection of the face winding onto offDir — so 0 is that
//   edge and the HUD prints the full span next to it ("offset 32 / 128") so the
//   number always has its scale attached.  Typing flips the line to the typed
//   position and the cursor stops driving it until the field is cleared, which is
//   the same grammar every other numeric command in the editor uses.
//
//   RMB / Enter commits at the CURRENT position; Esc cancels.  Tab re-derives the
//   basis and re-seats the line at the centroid (the old 50/50), because the
//   offset's reference edge changes with the axis and carrying the number across
//   would silently mean something else.
//
// TAB ROUTING.  kiwi_command.cpp's key ladder feeds VK_TAB to the numeric layer
// FIRST (KiwiNum_Key consumes it unconditionally for field cycling), so a command
// could never see it.  Shakeout G adds ONE rung above that: when the numeric layer
// has AT MOST ONE field there is nothing to cycle, so Tab is offered to the active
// command before the numeric layer gets it (kiwi_command.cpp:1089 —
// `vk == 0x09 && KiwiNum_FieldCount() <= 1`).  ROUND S gives this command exactly
// ONE field, which is still inside that rung: Tab keeps flipping U/V and the
// offset field is still typeable.  Declaring a SECOND field would silently take
// Tab away, so the field table must stay at one.
// ─────────────────────────────────────────────────────────────────────────────

class  KiwiEditorCommand;
struct brush_t;                   // qe3.h:474 (the 88-byte brush DEFINITION)
struct selbrush_t;                // qe3.h:429 (the 56-byte brush INSTANCE / list node)

// ── the shared splitter ──────────────────────────────────────────────────────
// Split `node` by the plane through p0/p1/p2 and LAND both halves selected.  The
// SOURCE INSTANCE IS NOT FREED — the caller does that, after this returns true,
// inside its own undo bracket (see the undo note above).
//
// False (with *why set to a static string) and NOTHING changed when:
//   * the node is a patch, a fixed-size entity's brush or has no def,
//   * the plane misses the brush (one half comes back NULL),
//   * either half fails the §19 gate.
bool KiwiSplit_BrushByPlane( selbrush_t *node,
                             const float p0[3], const float p1[3], const float p2[3],
                             selbrush_t **outA, selbrush_t **outB, const char **why );

// ── ROUND L: the same split, ONE LEVEL DOWN — no landing, no instance ────────
// The N-plane subtract in kiwi_boolean.cpp splits a brush repeatedly, keeping one
// half and re-splitting the other, and only the final pieces may ever reach the
// map.  So the LANDING moved out of the function above rather than the splitting
// being written a second time; KiwiSplit_BrushByPlane is now this plus "both
// halves or nothing" plus the landing.
//
// On `true`: *outFront is the { n·p >= d } half and *outBack the { n·p <= d } half
// of the plane through p0/p1/p2 (`n` = the plane's own outward normal, i.e. the
// one Face_MakePlane derives from those three points, in that order).  At most ONE
// is NULL, and a NULL one means the plane did not divide the brush — everything is
// on the other side.  Both are §19-gated, already Entity_LinkBrush'd to the source
// def's owner ENTITY, and on NO display list: the caller either lands them
// (Brush_AddToList + Brush_AddToList2) or frees them with KiwiSplit_FreeUnlandedDef.
//
// On `false` NOTHING is allocated — a §19 failure on EITHER half frees BOTH — so a
// caller's cleanup never has to unpick a partial result.  `def` itself is never
// touched: the ported core clones twice and reads the original.
bool KiwiSplit_DefByPlane( brush_t *def,
                           const float p0[3], const float p1[3], const float p2[3],
                           brush_t **outFront, brush_t **outBack, const char **why );

// ═════════════════════════════════════════════════════════════════════════════
//  WHY A SUBTRACT NEEDS PER-HALF REPORTING (history: round AO, item 3)
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT that opened it, verbatim: "With this new update, the boolean tool
// got worse and i can't diff an arch into a square pyramid anymore.  Try to make
// it more robust."
//
// WHY THE ALL-OR-NOTHING CONTRACT ABOVE IS THE WRONG ONE FOR A SUBTRACT.  The
// N-plane subtract walks the tool's faces and, at each one, keeps the OUTSIDE
// half as a surviving piece and re-splits the INSIDE half.  Those two halves do
// NOT have the same standing:
//
//   * the FRONT (outside) half is a PIECE THAT WILL BE LANDED.  If it comes back
//     as a sliver — V3 "face collapsed", V4 "zero-area face", V6 "planes crossed"
//     — the right answer is to DROP IT, because it is a fragment of essentially
//     zero volume that the mapper never asked for and cannot select afterwards.
//   * the BACK (inside) half is the REMAINDER, and the remainder is DISCARDED at
//     the end of the subtract by definition — it IS target ∩ tool.  A sliver
//     remainder means "there is nothing meaningful left to remove", which is a
//     STOP, not an error.
//
// Neither of those is a reason to abandon the target, yet `KiwiSplit_DefByPlane`
// reports both as one indivisible `false` and frees both halves, so a single
// sliver anywhere in a cascade threw the WHOLE carve away.  That is precisely the
// arch-into-a-pyramid case: a pyramid has an apex and four sloped faces, an arch
// is a many-planed tool (or a many-brush cascade), and intermediate pieces near
// the apex and along the sloped edges routinely land under KVALID_MIN_FACE_AREA.
//
// KIWI-UX (CLEANUP, A-19): the entry point that serves this argument is
// `KiwiSplit_DefByPlaneCarve` below — round AR's, which the subtract has called
// since.  Round AO's own by-parts entry was published for the same reason but
// has no external caller left, so it is now the file-static `SplitDefByPlaneBody`
// that `KiwiSplit_DefByPlane` and `KiwiSplit_DefByPlaneCarve` both front.
enum kiwiSplitHalf_t
{
    KSPLIT_HALF_NONE = 0,   // the plane did not divide: nothing lies on this side
    KSPLIT_HALF_OK,         // a §19-valid def is returned and is the caller's
    KSPLIT_HALF_SLIVER      // a def existed, failed §19 and HAS BEEN FREED
};

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AR, ITEM 2) — §19 IS A GATE ON WHAT GETS LANDED
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: *"why does this bool diff fail? … The only different is
// I joined the polyline on the 2nd one.  When i bool it into the pyramid it
// fails!"*
//
// THE DEFECT ROUND AO AND ROUND AQ BOTH WALKED PAST.  Round AO's by-parts split
// applied the FULL §19 gate to BOTH halves, and kiwi_boolean.cpp's subtract then
// treats a refused BACK half as "the two solids miss" and abandons the whole
// difference.  But the back half of a subtract step is the RUNNING INTERSECTION —
// `target ∩ (inside tool planes 0..f)` — and kiwi_boolean.cpp's own contract says
// it is DISCARDED at the end of the loop and *never landed for even one frame*.
//
// §19 exists to keep invalid brushes out of the MAP (kiwi_validity.h opening
// note).  Half of its checks are statements about a brush's PRESENTATION rather
// than about its volume: V4 rejects a face whose winding fell below 0.1 square
// units, V5 rejects two planes that became parallel-and-coincident, V3 rejects a
// face whose winding got clipped away entirely.  A running intersection against a
// MANY-PLANED tool — an extruded arch is 15-plus side planes, most of them
// near-tangent to their neighbours — collects exactly those three shapes as a
// matter of course, WITH A COMPLETELY HEALTHY VOLUME.  Refusing it is refusing an
// intermediate on the grounds that it would look wrong if it were landed, which it
// never is.
//
// SO: this entry point gates the FRONT half exactly as before — the front IS
// landed and every §19 refusal of it still drops it as a sliver — and lets a
// refused BACK half THROUGH, provided it is not a genuine graze.  "Genuine graze"
// is measured where a volume claim can honestly be made, on the half's own
// BOUNDS: every extent at or above KSPLIT_CARRY_EXTENT is a solid with real
// thickness in all three directions, and anything thinner is exactly the sliver
// round AO argued should stop the cascade.  Below that bound the behaviour is
// byte-for-byte round AO's.
//
// `*outBackRefused` is true when the back half came through on this rule, so the
// caller can COUNT and REPORT it — a partial hole must never be silent.  The back
// half is then the caller's to free exactly as an OK one is.
//
// 1.0 world unit rather than KVALID_MIN_THICKNESS (0.01): the question here is
// not "is this numerically degenerate" (§19 already answered that, and said yes)
// but "did the mapper mean to remove this much material".  A cut under one inch
// thick in some direction is a graze by any reading a mapper would recognise, and
// it is two orders of magnitude clear of the gate that produced the refusal.
#define KSPLIT_CARRY_EXTENT   1.0f      // world units, per axis

// Returns false — and allocates nothing — only when the split could not happen at
// all: a degenerate cut plane, or the ported core producing neither half.  On
// true, each half's state is reported in *outFrontState / *outBackState and the
// matching out-pointer is non-NULL only for KSPLIT_HALF_OK.  `why` names the §19
// check that rejected the FIRST sliver half (front before back), so a caller that
// wants to report a drop has the same string KiwiSplit_DefByPlane would give it.
bool KiwiSplit_DefByPlaneCarve( brush_t *def,
                                const float p0[3], const float p1[3], const float p2[3],
                                brush_t **outFront, brush_t **outBack,
                                kiwiSplitHalf_t *outFrontState,
                                kiwiSplitHalf_t *outBackState,
                                bool *outBackRefused,
                                const char **why );

// Entity_UnlinkBrush + Brush_Free_R, the pair csg.cpp:365-369 uses on a piece it
// throws away.  For a def from KiwiSplit_DefByPlane that is NOT going to be landed.
void KiwiSplit_FreeUnlandedDef( brush_t *def );

// True when the plane through p0/p1/p2 strictly straddles the brush's bounds —
// the cheap pre-test both verbs use to decide which brushes a cut applies to.
bool KiwiSplit_PlaneCrossesBrush( const brush_t *def,
                                  const float p0[3], const float p1[3], const float p2[3] );

// ── commands ─────────────────────────────────────────────────────────────────
void KiwiSplit_RegisterCommands();
KiwiEditorCommand *KiwiSplit_CommandForId( int commandId );

bool KiwiSplit_CanCut();          // §3 palette predicate: >= 1 brush AND 1 line
bool KiwiSplit_CanSplitFace();    // §3 palette predicate: exactly one face selected

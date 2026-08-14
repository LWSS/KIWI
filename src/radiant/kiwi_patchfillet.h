#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_patchfillet.h — ROUND Q: PSEUDO-FILLETS OUT OF Q3 BEZIER PATCHES.
//
//     KIWI_CMD_FILLET_EDGE  34063   MODAL "Fillet Edge (patch)"   — modern key B
//                                   when brush EDGES are selected
//     KIWI_CMD_BEVEL_EDGE   34040   the SAME command, reached by its chamfer name
//                                   (kiwi_bevel.h forwards the id — ROUND T)
//
// ═════════════════════════════════════════════════════════════════════════════
//  ROUND T — ONE TOOL, TWO MODES, AND AN ANGLE
// ═════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: "The default bevel mode should be chamfer, make it a
// Curve(Fillet) if (D) is pressed during the operation of the tool.  Also allow
// angle adjustments on both of them.  Once a chamfer/bevel is made, allow (DEL)
// key to remove the chamfer and restore the original edge/corner."
//
// ── THE MERGE ───────────────────────────────────────────────────────────────
// §25's Bevel Edge (kiwi_bevel.cpp) and this command ran the SAME live chamfer —
// KiwiBevel_MakeFrame + KiwiBevel_AppendFace per frame from the baseline, §19
// gate, one undo bracket opened at the first mutation — and differed only in what
// the confirm landed.  Two commands cannot be "one tool with a mode key", so the
// chamfer half folded in here.  The command now carries `m_curve`:
//
//     m_curve == false   CHAMFER (the default, every time the gesture starts).
//                        The drag scalar IS the chamfer depth.  The confirm lands
//                        the solid as it already stands, i.e. nothing extra.
//     m_curve == true    FILLET.  The drag scalar is the arc RADIUS and the depth
//                        is derived, d = r(1-k²)/k, exactly as round Q did.  The
//                        confirm additionally lands one patch per edge.
//
// D toggles, mid-gesture, and converts the scalar through that same identity so
// the SOLID does not jump across the toggle; the typed value is cleared, because
// a number typed as a depth would silently become a radius.
//
// ── THE ANGLE: PARAMETRISATION, STATED ──────────────────────────────────────
// A second numeric field, "bias", Tab-reachable, in DEGREES, signed:
//
//     the chamfer plane's normal = the bisector n ROTATED ABOUT THE EDGE by the
//     bias, positive meaning "toward the second adjacent face"
//         n(bias) = n·cos(bias) + v·sin(bias)      (kiwi_bevel.h; u·n == 0, so
//                                                   Rodrigues collapses to this)
//
// so 0° is the symmetric bisector — every pre-round-T chamfer, bit for bit — and
// the usable range is (-φ/2, +φ/2) where φ/2 = acos(n·n1) is the half-angle
// between the bisector and either face normal.  It is CLAMPED PER EDGE to that
// limit minus a 2° guard band, because the limit differs corner by corner and a
// gesture spanning a square corner and a shallow one should bias each as far as
// that corner legally allows.  The HUD prints the APPLIED angle and says
// "(clamped)" when it differs from the request.
//
// WHY DEGREES-ABOUT-THE-EDGE RATHER THAN A 0..1 BLEND between the two face
// planes: the blend's midpoint is not the bisector for a non-square corner (the
// normals are not equally spaced in angle from a normalised sum), so "0.5" would
// mean something different at every edge.  An angle about the edge means the same
// thing everywhere and is the number a mapper can reason about.
//
// ── AND WHY THE FILLET REFUSES IT ───────────────────────────────────────────
// The fillet arc must be TANGENT TO BOTH FACES.  That condition places its axis
// on the bisector at r/k (THE AXIS, below) — there is no freedom left to spend on
// a bias, and the tangent solution for a tilted plane is an ELLIPSE, which the
// non-rational biquadratic construction below is not derived for.  So in fillet
// mode the bias field is not offered and a bias in force when D is pressed is
// DROPPED with a console line saying why.  Refusing out loud beats accepting and
// approximating.
//
// ── AND THE RESTORE ─────────────────────────────────────────────────────────
// DEL with exactly one brush FACE selected removes it and lets the neighbours
// re-extend, which recreates the edge.  It is a general face-removal verb rather
// than a "was this made by a bevel" guess, it is §19-validated, and it lives in
// kiwi_bevel.cpp with the rest of the chamfer machinery — see kiwi_bevel.h
// REMOVE FACE (RESTORE EDGE) for the whole rule and the funnel rung.
//
// USER DIRECTIVE, verbatim: "I want to have psuedo-fillets using the quake3
// curves, is that possible?  Try to add that".
//
// YES, and this is the thing classic Radiant's Curve→Bevel has always been: a
// quarter-cylinder bezier patch tucked into a chamfered corner.  What Radiant
// never did was DERIVE it from the corner — its Bevel builds a patch over the
// selected brush's BOUNDING BOX and deletes the brush.  This command does the
// modelling operation instead: chamfer the real edge, then fill the notch with
// the arc that is tangent to both original faces.
//
// ═════════════════════════════════════════════════════════════════════════════
//  WHAT A FILLET IS, AND WHY THE CHAMFER COMES FIRST
// ═════════════════════════════════════════════════════════════════════════════
// A solid fillet of radius r at a convex edge is a CYLINDER of radius r, axis
// parallel to the edge, tangent to both adjacent faces, with everything outside
// it removed.  A plane-defined brush cannot BE a cylinder — so the solid keeps
// the flat chamfer (one extra half-space, which is all §25's Bevel Edge ever
// was), and the curved surface is a separate PATCH laid over it.  Hence
// "pseudo-fillet": the collision hull is the chamfer, the visible surface is the
// arc.  That is exactly the trade every q3-lineage map has always made, and it
// is stated here rather than implied.
//
// ── THE GEOMETRY, DERIVED (not guessed) ─────────────────────────────────────
// One edge, two adjacent faces with OUTWARD unit normals n1, n2 (kiwi_bevel.h's
// frame: n = normalise(n1+n2) is the outward bisector, u the edge direction,
// v = n × u).  Write
//
//     c = n1·n2                        φ = acos(c)   the angle between the
//                                                    OUTWARD NORMALS
//     θ = π - φ                        the INTERIOR WEDGE ANGLE of the solid
//     k = n·n1 = cos(φ/2) = sin(θ/2)   (and k² = (1+c)/2)
//
// For a square corner φ = θ = 90° and k = √½.
//
// THE AXIS.  The fillet cylinder's axis A satisfies n_i·(A - E) = -r for both
// faces (distance r inside each), where E is any point of the edge.  Writing
// A - E = a·n1 + b·n2 and solving the symmetric 2×2 gives a = b = -r/(1+c), so
//
//     A = E - r·(n1 + n2)/(1 + c) = E - n·r/k
//
// (using |n1+n2| = √(2+2c) = 2k).  It is r/k INWARD along the bisector, which is
// the textbook fillet offset and is where the √½ in a 90° corner comes from.
//
// THE TANGENT POINTS.  T1 = A + r·n1 and T2 = A + r·n2 — each is the foot of the
// perpendicular from A to its plane, so it is ON that face.  Their distance from
// the edge is the classic tangent length
//
//     |T1 - E| = r·√((1-c)/(1+c)) = r·tan(φ/2) = r/tan(θ/2)
//
// (r for a square corner).  Their depth BELOW the edge along the bisector is
// n·(E - T1) = r·(1-k²)/k — and THAT is the chamfer depth this command asks
// kiwi_bevel.h's machinery for:
//
//     d = r·(1 - k²)/k                 [ = r·√½ for a square corner ]
//
// With the chamfer cut at exactly that depth, the chamfer's two boundary lines
// ARE the fillet's two tangent lines.  Nothing is fitted; the two constructions
// meet by derivation.
//
// The arc's own closest approach to the original edge is r·(1-k)/k, which is
// LESS than d — so the arc bulges out of the chamfer plane, back toward the
// corner, filling exactly the notch the chamfer removed.  It never dips below
// the chamfer and never pokes through either original face.
//
// ── THE CONTROL POINTS ──────────────────────────────────────────────────────
// A q3 patch is a NON-RATIONAL biquadratic bezier, so it cannot represent a
// circular arc exactly (that needs the rational form with middle weight
// w = cos(half-arc)).  The standard construction is used: for an arc span the
// three control points are (start point, INTERSECTION OF THE TWO END TANGENTS,
// end point).  For a circle the tangents at ψ0 and ψ1 meet on the bisector at
// radius r/cos(α/2), α = ψ1 - ψ0.  So with basis
//
//     a = n1,  b = normalise(n2 - c·n1)      (both ⟂ u; m(ψ) = cos ψ·a + sin ψ·b
//                                             sweeps n1 → n2 as ψ: 0 → φ)
//
//     EVEN column 2i    Q_i  = A + r·m(i·α)                     i = 0..spans
//     ODD  column 2i+1  M_i  = A + (r/cos(α/2))·m((i+½)·α)      i = 0..spans-1
//
// Q_0 = T1 and Q_spans = T2 — the ends land on the chamfer's boundary lines, as
// required.  AND AT ONE SPAN THIS IS THE TEXTBOOK FIGURE: α = φ, so the single
// middle control point is A + n·r/k = **E, the original edge position** — which
// is what every q3 bevel patch has at its middle column.  The multi-span form is
// that same construction refined, not a different one.
//
// ── THE ERROR BOUND, WITH A NUMBER ──────────────────────────────────────────
// For one span of arc angle α the non-rational quadratic passes through both
// ends with the correct tangents and deviates most at its midpoint.  Evaluating
// B(½) = (Q_i + 2M_i + Q_{i+1})/4 against the true arc midpoint gives a radial
// error of
//
//     ε = r·(1 - cos(α/2))² / (2·cos(α/2))
//
// and the approximation lies OUTSIDE the true arc (fractionally further from the
// axis, i.e. toward the corner).  Numbers for a 90° corner, radius r:
//
//     1 span  (width 3)   α = 90°   ε = 0.0607·r     ← visibly under-round
//     2 spans (width 5)   α = 45°   ε = 0.0031·r
//     3 spans (width 7)   α = 30°   ε = 0.00060·r
//     4 spans (width 9)   α = 22.5° ε = 0.00019·r
//
// A single span is 6% of the radius off, which on a 64-unit fillet is nearly 4
// units — that is why this command does NOT ship the three-column figure by
// default.  It subdivides to KPF_SPAN_DEG per span (30°), capped by the patch
// format's 16-column ceiling, so a square corner gets width 7 and 0.06% error
// and a shallow one gets fewer spans because it needs fewer.
//
// ── GENERAL ANGLES, AND WHAT IS REFUSED ─────────────────────────────────────
// Nothing above assumed a square corner: φ is measured per edge and the whole
// construction is written in terms of k = sin(θ/2).  Two ends are refused rather
// than approximated, both with a console message:
//
//   * θ > KPF_MAX_WEDGE_DEG (170°) — a nearly flat edge.  d → 0 and the fillet
//     degenerates into a strip of nothing; there is no corner to round.
//   * θ < KPF_MIN_WEDGE_DEG (5°)   — a spike.  The tangent length r/tan(θ/2)
//     exceeds 22·r, so any usable radius eats the whole brush.  (§19 would reject
//     the chamfer anyway; refusing here says WHY instead of "invalid geometry".)
//
// Everything between is handled exactly, including obtuse and acute wedges.
//
// ═════════════════════════════════════════════════════════════════════════════
//  THE GESTURE, AND ONE DELIBERATE DEVIATION FROM THE BASELINE DISCIPLINE
// ═════════════════════════════════════════════════════════════════════════════
// Drag (or type) the RADIUS.  Every frame, from the BASELINE brush:
//
//   1. remove the chamfer face this command added last frame (reverse order);
//   2. per edge, compute k, refuse the degenerate wedges, derive d = r(1-k²)/k;
//   3. KiwiBevel_AppendFace at that depth — the SAME appender §25's Bevel Edge
//      uses (kiwi_bevel.h ROUND Q), so there is one chamfer implementation;
//   4. KiwiValid_Rebuild + KiwiValid_CheckBrush.  Fail → back to step 1's state
//      and raise INVALID.  Live geometry is never left rejected, not for a frame.
//
// So the SOLID is fully live and fully baseline-disciplined, exactly as §25's
// bevel is — this command IS that command plus a surface.
//
// THE PATCH IS NOT.  It is drawn as a polyline preview during the drag (the
// bezier EVALUATED, so the preview is the patch and not a sketch of it) and only
// created at commit.  This is a deviation from "re-place the patch from baseline
// each frame" and it is deliberate:
//
//   * a patchMesh_t is 20 556 bytes and its creation is not a value assignment —
//     AddBrushForPatch allocates a symbiont brush and LINKS IT INTO THE ENTITY
//     DEF LIST (pmesh.cpp:840), Brush_AddToList allocates the instance and links
//     that (brush.cpp:656), and Patch_GenericMesh2 mallocs a fresh tessellated
//     mesh (pmesh.cpp:720).  Doing that per mouse-move means linking and
//     unlinking a world brush tens of times a second;
//   * unlinking it again on the next frame — or on Esc — has to walk the same
//     funnels backwards, and the ONE safe way to remove a linked brush in this
//     editor is the undo record, which is exactly the thing a per-frame preview
//     must not be touching;
//   * and it buys nothing.  The patch's shape is a pure function of the chamfer,
//     the chamfer IS live, and the preview draws the identical curve.
//
// The cost is stated honestly: Esc after a commit is an undo, not a cancel — but
// that is true of every command here.  Esc DURING the drag rolls the chamfer back
// and creates nothing, because nothing was created.
//
// ═════════════════════════════════════════════════════════════════════════════
//  UNDO: THE BRUSH CHANGE AND THE PATCH IN ONE RECORD
// ═════════════════════════════════════════════════════════════════════════════
// Read out of undo.cpp, not assumed.  undo.cpp contains no mention of patches at
// all — patch awareness lives one level down, in the cloner:
//
//   * Undo_AddBrush (undo.cpp:494) calls BrushDef_FullClone → Brush_FullClone_
//     sub475E80 (brush.cpp:7361), whose FIRST branch is `if (def->patch)` →
//     Patch_Duplicate (pmesh.cpp:2236), a memcpy of the whole 20 556-byte struct
//     including the 16×16 control grid, plus a fresh symbiont.  So a MODIFIED
//     patch is deep-copied by the ordinary brush path with nothing extra.
//   * A CREATED brush is covered by the head/tail pair instead:
//     KiwiCmd_UndoBegin's Undo_AddBrushList(&selected_brushes) at the head and
//     KiwiCmd_UndoCommit's Undo_EndBrushList(&selected_brushes) at the tail,
//     which stamps `def->ownerPrev = record id` on everything on the list
//     (undo.cpp:576) — the "remove me on undo" mark.  The patch lands on
//     selected_brushes via Brush_AddToList2, so the tail finds it.
//
// This command therefore needs ONE bracket around both halves, and gets it:
//   HEAD   KiwiCmd_UndoBegin("fillet edge"), opened at the FIRST chamfer
//          mutation (kiwi_transform.h rule 2), plus one Undo_AddBrush per
//          touched brush BY HAND — an EDGE selection puts nothing on
//          selected_brushes (kiwi_selection.h DESIGN NOTE 2), so the head's
//          Undo_AddBrushList covers none of the brushes being chamfered.
//   BODY   the chamfer faces, then at commit the patch creation + landing.
//   TAIL   the framework's KiwiCmd_UndoCommit.
// One Ctrl+Z removes the patch AND un-chamfers the brush.
//
// ORDERING TRAP, and how it is avoided: the chamfered brushes must NOT be on
// selected_brushes when the tail runs, or Undo_EndBrushList would stamp them
// "added by this record" and undo would DELETE them.  An edge selection never
// puts them there, and the commit calls Select_Deselect(1) before landing the
// patch anyway — the same belt kiwi_transform.cpp's CommitDelete wears.
//
// ═════════════════════════════════════════════════════════════════════════════
//  THE B KEY: CONTEXT-AWARE, NOT SHIFT+B
// ═════════════════════════════════════════════════════════════════════════════
// B is already Fillet Corners (KIWI_CMD_FILLET_CURVE, round J) on a construction
// chain.  This round makes B a CONTEXT VERB instead of taking a second chord:
//
//     B with brush EDGES selected        → Fillet Edge (patch)   [this file]
//     B otherwise                        → Fillet Corners        [kiwi_fillet]
//
// Chosen over Shift+B for four reasons, in order of weight:
//   1. It is ONE CONCEPT.  Both round a corner at a dragged radius; a user who
//      learned B on a curve has already learned B on a solid.  J is the existing
//      precedent for exactly this (kiwi_join.h: faces → CSG_Merge, lines →
//      JoinLines, one key, arbitrated by selection).
//   2. It is MORE faithful to Plasticity, not less.  Plasticity's `b` is
//      `command:fillet-solid` (default-keymap.ts:264) — the SOLID fillet.  Round J
//      took B for the curve fillet only because KIWI had no solid fillet to give
//      it (kiwi_fillet.h says so in as many words).  Now it does, and B on a
//      solid edge means what it means in Plasticity.
//   3. NO NEW KEYMAP DISPLACEMENT.  Shift+B (0x42 mods 1) is free, but round J
//      already paid for bare B by moving SameTargetname 36121 to Shift+Alt+B; a
//      context arm costs nothing more and leaves the audit table untouched.
//   4. The two selections cannot be confused: KCONSEL items and SEL_EDGE items
//      live in different stores (kiwi_conselect.h), so "brush edges selected" is
//      an exact question with an exact answer.
//
// Precedence when BOTH are non-empty: brush edges win.  Same order J uses and for
// the same reason — a brush-side selection is what every other command in the
// editor reads first, so B agrees with the rest of the editor rather than
// inventing a second precedence.
//
// The redirect is ONE line in KiwiCmd_DispatchInner, applied before the modal
// branch, so every route to the command — key, palette, menu, Repeat Last —
// arbitrates identically.  KIWI_CMD_FILLET_EDGE is also registered in its own
// right, so the palette can run it directly and unambiguously.
// ─────────────────────────────────────────────────────────────────────────────

class  KiwiEditorCommand;
struct brush_t;                   // qe3.h:474 (the 88-byte brush DEFINITION)

// Tuning.
#define KPF_MIN_RADIUS      0.25f   // world units — below this the gesture is a no-op
#define KPF_MAX_EDGES       16      // v1 cap: one patch per edge, and patches are big
#define KPF_SPAN_DEG        30.0f   // target arc angle per bezier span (see the bound)
#define KPF_MAX_SPANS       7       // 2*7+1 = 15 columns, inside the 16 the format allows
#define KPF_MIN_WEDGE_DEG   5.0f    // interior angle floor — below this it is a spike
#define KPF_MAX_WEDGE_DEG   170.0f  // …and ceiling — above this there is no corner
#define KPF_PATCH_ROWS      3       // along the edge; the cross-section is constant, so
                                    // three rows (both ends + the exact midpoint) is not
                                    // an approximation, it is the minimum odd grid

void KiwiPatchFillet_RegisterCommands();
KiwiEditorCommand *KiwiPatchFillet_CommandForId( int commandId );

// §3 canExecute predicate: at least one live SEL_EDGE item on a non-patch brush.
bool KiwiPatchFillet_CanFillet();

// THE B DISPATCH.  Returns KIWI_CMD_FILLET_EDGE when `commandId` is
// KIWI_CMD_FILLET_CURVE and the selection is brush edges; otherwise returns
// `commandId` unchanged.  Total, side-effect free, and safe to call on every id.
int  KiwiPatchFillet_ContextB( int commandId );

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AO, ITEM 2 — THE FILLETS FOLLOW THE SURFACE
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "When extending a surface that has fillets, the fillets
// also need to stretch with the surface."
//
// ── WHY THERE IS NO STORED LINK, AND WHY THE ASSOCIATION IS GEOMETRIC ────────
// A fillet lands three INDEPENDENT patch entities (the arc and two end caps) and
// records nothing about the brush it was cut into: `filletUnit_t` is a
// gesture-lifetime struct that dies with the command.  Two designs were possible
// and the geometric one is taken, for one decisive reason:
//
//   * a PERSISTED id (a sidecar line, or a spare field on the patch) is only as
//     good as the round trip.  A .map is a text file that other tools and other
//     Radiants write; a brush ordinal or a minted id survives neither an edit
//     outside KIWI nor a re-order, and a STALE id is worse than none because it
//     names the wrong brush with full confidence.
//   * the GEOMETRIC association is re-derived from the geometry that is actually
//     in front of the editor, every time, so it works identically on a map that
//     was just loaded, on one hand-edited outside KIWI, and on one built five
//     minutes ago.  A fillet's own construction is what makes it decidable: the
//     patch is laid ON the brush's planes (round AM's RefitRails put the two rail
//     columns exactly on the chamfer plane and on the two adjacent planes), so
//     "does this patch sit on the face that just moved" is a plane test and a
//     point-in-winding test, not a guess.
//
// ── WHAT IT CARRIES, AND WHAT IT WILL NOT ───────────────────────────────────
// The rule is ONE rule: a control point that lies ON the moved face's plane AND
// INSIDE that face's winding moves with the plane, and any interior row between
// two end rows is then re-interpolated so the fillet's "both ends and the exact
// midpoint" grid stays true (KPF_PATCH_ROWS).  That single rule covers both of
// the gestures the report is about:
//
//   * pushing the wall's END face lengthens the filleted edge — the arc patch's
//     far ROW is on that plane, so it translates and the arc stretches; the end
//     CAP patch lies wholly on that plane, so it translates whole and stays
//     sealed to the end it caps.
//   * moving the whole brush along an axis moves every plane, so every control
//     point is on a moved plane and the fillet rides along.
//
// IT REFUSES, LOUDLY, when the moved plane cuts the patch's CROSS-SECTION rather
// than its length — i.e. when the on-plane control points do not form complete
// ROWS.  That is the chamfer-plane push, and carrying it correctly means
// re-solving the RADIUS against the new chamfer plane (round AM's RefitRails),
// which cannot be done from a landed patch alone without reconstructing the
// gesture's `filletUnit_t`.  Moving the rails without re-solving the radius would
// shear the arc and open the very seam round AM closed, so the fillet is left
// alone and the console says to re-run the bevel.  `outSkipped` counts those.
//
// NOT CARRIED AT ALL, by design and stated rather than discovered: ROTATE, VERTEX
// edits, free-form (unconstrained) drags and SCALE.  All four change the brush's
// planes in ways that are not a single plane translation, so the one rule above
// does not describe them and a second rule would be a second, unproven fillet
// model.
//
// CONTRACT: the CALLER owns the undo bracket and must already have one open —
// this covers each patch it touches with `Undo_AddBrush` inside that bracket, so
// the whole gesture stays ONE undo record.  `planeDistBefore` is the moved face's
// plane distance BEFORE the push, `travel` the signed distance it moved along
// `planeN`.  Returns the number of patches moved.
int KiwiFillet_CarryOnPlaneMove( const brush_t *def, int movedFace,
                                 const float planeN[3], float planeDistBefore,
                                 float travel, int *outSkipped );

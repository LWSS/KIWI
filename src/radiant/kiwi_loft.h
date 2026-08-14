#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_loft.h — ROUND AF, ITEM 6: LOFT (L).  RADIANT_UX_DESIGN §60.
//
// USER DIRECTIVE, verbatim: "You select 1 face then press L and select the other
// face.  (Another way is to select 2 faces and then press L and the operation
// starts)  It creates a new bridge between the 2 faces like a loft in plasticity.
// It can be used on faces from other solids(Brushes) as well.  Make it robust and
// try to solve most issues including curvature.  It might need a density setting
// before a final confirm.  G-continuity options like in plasticity would also be
// incredible."
//
// ═════════════════════════════════════════════════════════════════════════════
//  WHAT PLASTICITY ACTUALLY DOES, AND WHAT OF IT CAN CROSS OVER
// ═════════════════════════════════════════════════════════════════════════════
// Read before designing, and it moved the design twice:
//
//   * `LoftCommand.ts:12` takes its sections from the SELECTION, not a picker —
//     `const curves = [...this.editor.selection.selected.curves];` — and the only
//     thing it PICKS is an optional SPINE (`LoftCommand.ts:23-32`, an ObjectPicker
//     with min 1 / max 1 / `SelectionMode.Curve`).  Its sections are CURVES.  It
//     cannot loft between two faces at all.
//   * `LoftFactory.ts:6-11` is the whole parameter set:
//         thickness1, thickness2, thickness, closed
//     There is **no density, no degree and NO G-continuity option**.  `closed` is
//     not a continuity control either — it is set from the spine
//     (`LoftFactory.ts:26`, `if (this._spine.model!.IsClosed()) this.closed = true`).
//   * the actual surface is one kernel call —
//     `c3d.ActionSolid.LoftedSolid( placements, contours, spine ?? null, params,
//     [], names, ns )` (`LoftFactory.ts:76`) — i.e. the continuity and the
//     tessellation are C3D's, not the application's.  Plasticity has no code that
//     could be ported here; it has a NURBS kernel where KIWI has half-spaces.
//   * each section is planarised first (`curve3d2curve2d`, `LoftFactory.ts:40-41`)
//     and a section that will not planarise raises
//     `ValidationError("Curve cannot be converted to planar")`.  That rule IS
//     adopted, verbatim in spirit: **planar profiles only.**
//   * `"l": "command:loft"` — `default-keymap.ts:270`, bare L inside the
//     `body:not([gizmo])` scope.  That IS adopted (see the vk 0x4C audit in
//     kiwi_keymap.h).
//   * its preview is the ordinary GeometryFactory live-update loop
//     (`LoftCommand.ts:19-21` / `:34`), re-running the factory on every parameter
//     change.  Adopted in shape: every field edit re-tessellates and the preview
//     is the RESULT, not a schematic.
//
// So: the ENTRY GRAMMAR is the user's (faces, not curves), the PARAMETERS are
// KIWI's own (Plasticity has neither), and the only two things taken across
// unchanged are the key and the planar-section rule.  Everything below is
// therefore stated as KIWI's design, with Plasticity cited where it actually
// informed a decision rather than decorating one.
//
// ═════════════════════════════════════════════════════════════════════════════
//  THE GRAMMAR
// ═════════════════════════════════════════════════════════════════════════════
//   2+ faces selected  ->  L starts LIVE on the first two, immediately.
//   1  face  selected  ->  L is HOT and prompts for the second: click ANY brush
//                          face, on any solid, including another face of the same
//                          solid.
//   0  faces selected  ->  L refuses and says which two states it accepts.
//
//   D          RULED <-> TANGENT
//   Tab        density <-> tension
//   digits     the focused field
//   RMB/Enter  apply     Esc  back a stage / cancel
//
// ═════════════════════════════════════════════════════════════════════════════
//  THE GEOMETRY, AND THE ONE HONEST APPROXIMATION IN IT
// ═════════════════════════════════════════════════════════════════════════════
// A classic brush is an INTERSECTION OF HALF-SPACES.  A loft between two arbitrary
// planar profiles is, in general, a surface with non-planar quads — and a
// non-planar quad is not something a brush face can be.  This is the whole
// difficulty and it does not go away by being ignored, so it is stated up front:
//
//   ── THE PIPELINE ─────────────────────────────────────────────────────────
//   1. RINGS.  Ring A and ring B are the two source faces' WINDINGS, in world
//      space.  A brush face winding is convex by construction (it is itself a
//      half-space intersection), so both profiles are convex for free — which is
//      why this command never needs the region layer's ear clip.
//   2. SENSE.  Both rings are rewound CCW about the loft axis d = normalize(cB -
//      cA), so a side quad's winding rule is one expression instead of two
//      mirrored ones.
//   3. COUNTS.  The SHORTER ring's longest edges are split until the counts match
//      — the source corners of both profiles survive, and only collinear points
//      are added.  (Resampling both rings uniformly would have been two lines and
//      would round off every corner of both.)
//   4. CORRESPONDENCE.  The cyclic offset k that minimises
//          sum_i | perp_d( A[i] - cA ) - perp_d( B[(i+k)%n] - cB ) |²
//      wins — i.e. nearest match after both rings are centred and the axial
//      component is projected out.  That is "no twisting" made measurable, and it
//      is O(n²) over n <= 64.
//   5. STATIONS.  N+1 rings along the path.  RULED interpolates linearly;
//      TANGENT is a cubic Hermite per corresponding vertex, leaving A along A's
//      outward normal and arriving at B along B's, with tangent magnitude
//      `tension * |cB - cA|`.
//   6. PLANARISE.  **Each intermediate station ring is projected onto its own
//      Newell best-fit plane.**  This is the approximation, and it is deliberate:
//      it is what makes station i's top cap and station i+1's bottom cap THE SAME
//      PLANE, which is what makes the segments mate with no gap and no overlap.
//      The projection is exactly zero at both ends (the source windings are
//      already planar) and shrinks with density, because a shorter span twists
//      less.
//   7. SEGMENTS.  Segment i is a brush over ring[i] and ring[i+1]:
//        * two cap planes — the two stations' own fit planes, outward;
//        * one side plane per corresponding edge pair, whose normal is the
//          Newell normal of the (possibly non-planar) quad, PUSHED OUT until all
//          four of its points are inside.
//      Pushing out rather than through means the brush CONTAINS the hull of its
//      2n ring points, so no vertex of either ring is ever clipped off.  The
//      inflation is bounded by the quad's non-planarity and, again, goes to zero
//      as density rises.
//   8. GATE.  Every segment goes through KiwiValid_Rebuild + KiwiValid_CheckBrush
//      (§19 V1..V7) while it is still UNLINKED, so a rejection frees defs and
//      touches nothing — the same all-or-nothing rule kiwi_extrude.h states.
//
// ── WHAT THAT BUYS, STATED AS LIMITS RATHER THAN AS FEATURES ────────────────
//   * PLANAR PROFILES ONLY.  Brush face windings always are.  (Plasticity's own
//     rule, LoftFactory.ts:40-41.)
//   * EVERY SEGMENT IS CONVEX, because a brush is.  Two profiles whose
//     correspondence would require a concave bridge are refused by §19, not
//     silently mangled.
//   * G0 (RULED) IS EXACT.  G1 (TANGENT) is exact at the two ENDS — the first and
//     last station ARE the source windings and the first/last path tangents ARE
//     the source normals — and piecewise-linear in between, which is what a
//     faceted medium can offer.  Raising the density is what buys smoothness.
//   * G2 IS NOT ATTAINABLE IN BRUSHES AT ALL and is not attempted.  A curvature-
//     continuous bridge needs a curved medium; in this editor that medium is the
//     q3 PATCH (pmesh.cpp), which kiwi_patchfillet.cpp already lays into a
//     chamfer.  A patch loft is the honest future work and is recorded as such in
//     RADIANT_KNOWN_ISSUES rather than half-shipped here.
//   * THE SOURCE BRUSHES ARE NOT TOUCHED.  A loft BRIDGES; it does not weld.  The
//     two interior end caps stay where they are, and removing them (for a
//     continuous tunnel) is the user's own Remove Face — see the note in
//     RADIANT_KNOWN_ISSUES round AF for why that is not done automatically.
// ─────────────────────────────────────────────────────────────────────────────

class KiwiEditorCommand;

// v1 caps.  The ring cap is KEXT_MAX_PROFILE / KCON_SEGS_MAX, i.e. the same 64
// every other profile in this layer is bounded by; the segment cap keeps the
// worst case at 32 brushes of 66 faces, which is a lot of brush and is meant to
// be the number a mapper has to think about before exceeding.
#define KLOFT_MAX_RING     64
#define KLOFT_MIN_SEGS      1
// ROUND AN (user directive): "Make the topend something like 128".  32 was the
// think-before-exceeding number; 128 is the user's asked-for ceiling and the
// preview/undo machinery is count-bounded either way (worst case 128 brushes of
// 66 faces — a deliberate choice on their part via the slider or a typed count).
#define KLOFT_MAX_SEGS    128
#define KLOFT_DEF_SEGS_RULED    1     // a straight bridge needs exactly one solid
#define KLOFT_DEF_SEGS_TANGENT  8     // …a curved one needs enough to read as curved
#define KLOFT_MIN_SPAN      1.0f      // world units between the two face centroids
#define KLOFT_MIN_TENSION   0.05f
#define KLOFT_MAX_TENSION   4.0f
#define KLOFT_DEF_TENSION   0.5f      // "tangent magnitudes ~ half the span"

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AO, ITEM 1 — THE ROBUSTNESS ROUND, AND WHY PERPENDICULAR FACES FAILED
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "The loft tool doesn't work in a lot of cases.  I want
// it to be more robust.  First, dont make the preview blue until it will
// actually work (misleading! and a time waster!).  2nd, I want the option to
// make the loft a curve.  Here is a picture of a loft that I would like to work.
// It's a curved wall.  This type of loft will not work no matter what.  Needs
// more options available.  Also the curve needs to be the same thickness and
// match textures to the 2 pieces."
//
// The picture is two wall pieces meeting at about 90 degrees with their small
// square END faces selected, and a straight blue ruled preview bridging them.
//
// ── THE REFUSAL, NAMED: THE CORRESPONDENCE IS MEASURED IN THE WRONG PLANE ────
// Step 4 above compares the two rings after "the axial component is projected
// out", i.e. both rings are flattened along ONE direction — the straight axis
// d = normalize(cB - cA).  That is the right comparison when the two faces are
// roughly PARALLEL and facing each other, because then both rings already lie in
// planes perpendicular to d and the projection is nearly an isometry.
//
// IT IS THE WRONG COMPARISON THE MOMENT THE FACES ARE NOT PARALLEL.  Ring A lies
// in a plane at angle alpha to d and ring B in a plane at angle alpha on the
// OTHER side.  Projecting both along d therefore squashes each ring by cos(alpha)
// — but along two DIFFERENT in-plane directions, 2*alpha apart.  At the pictured
// 90 degrees, a SQUARE end face on A projects to a rectangle squashed one way and
// B's square projects to a rectangle squashed the ORTHOGONAL way, so the cyclic
// offset that minimises the sum of squares is being chosen between two shapes
// whose aspect ratios are inverted.  It is routinely ONE VERTEX OFF, which is a
// 90-degree TWIST of the correspondence — and a 90-degree twisted square bridge
// has side quads that cross, which §19 rejects as "planes crossed" / "face
// collapsed".  THE TWIST IS DECIDED BEFORE ANY STATION MATH RUNS, which is
// exactly why the report says it "will not work no matter what": neither RULED
// nor TANGENT nor any density can rescue a wrong correspondence.
//
// ── AND THE SECOND HALF: THE VERTEX BLEND PINCHES ───────────────────────────
// Even with the right correspondence, blending corresponding vertices in WORLD
// space between two non-parallel planes shrinks the section.  For the pictured
// wall — A's section spanning +-w along X, B's spanning +-w along Y — the
// midpoint ring spans +-w/2 along each, i.e. a width of w*sqrt(2)/2 ~= 0.71 w.
// That is precisely "the curve needs to be the same thickness".
//
// ── THE CURE FOR BOTH: A ROTATION-MINIMISING FRAME ──────────────────────────
// Stop comparing and blending in world space; do both in a FRAME that is carried
// from A to B with no twist about the section normal.
//
//   * Frame at A: (nA, uA, vA) with uA any deterministic tangent and vA = nA x uA.
//   * TRANSPORT to B by the MINIMAL ROTATION R taking nA to nB — Rodrigues about
//     w = normalize(nA x nB) through theta = acos(nA.nB):
//         R x = x cos(theta) + (w x x) sin(theta) + w (w.x)(1 - cos(theta))
//     uB = R uA, vB = nB x uB.  A minimal rotation has no component about the
//     normal itself, so it introduces zero twist — that IS the defining property
//     of the rotation-minimising frame, evaluated in closed form here because the
//     path has exactly two ends rather than a sampled spine.
//   * CORRESPONDENCE is then the cyclic offset minimising the squared distance
//     between A's (x,y) coordinates in (uA,vA) and B's in (uB,vB).  No squash, no
//     aspect inversion, and for parallel faces R is the identity so the answer is
//     bit-for-bit what step 4 gave before.
//   * STATIONS are built the same way: at parameter t the frame is R(t) (the same
//     Rodrigues formula at angle t*theta), the origin is the path point, and the
//     ring is the 2D blend of A's and B's coordinates mapped THROUGH that frame.
//     Two consequences, both of them the report's asks:
//       - if A and B are congruent the section is congruent at EVERY station, so
//         the wall keeps its thickness;
//       - every station is PLANAR BY CONSTRUCTION, so step 6's projection is
//         exact rather than an approximation and cannot introduce a twist.
//     With nA == nB this reduces algebraically to the old world-space lerp, so
//     the parallel case — every loft that already worked — is unchanged.
//
// ── THE PREVIEW NOW RUNS THE COMMIT ─────────────────────────────────────────
// Steps 7 and 8 (build every segment, gate it) used to run only at COMMIT, so the
// preview was blue whenever STATIONS existed even when the commit would refuse.
// Rebuild now performs the whole build as a DRY RUN — the "rejection is free"
// property kiwi_extrude.h states and kiwi_boolean.cpp's KiwiBool_WouldCarve
// already relies on — frees every def, and colours the bridge BAD-red unless the
// commit would succeed.  The TANGENT->RULED retry is run in the dry pass too, so
// when it would kick in the preview shows the RULED bridge that will ACTUALLY be
// built rather than the tangent one that will not.
//
// ── THE TWIST STEPPER ───────────────────────────────────────────────────────
// The automatic correspondence is now measured in the right frame, but a profile
// with rotational symmetry (a square, a regular n-gon) has several offsets with
// near-equal cost and the mapper may want a different one.  `Twist` steps the
// correspondence by whole vertices, applied ON TOP of the computed offset, and it
// rescues a refused loft in one keypress when the refusal is a correspondence.
//
// ── AND THE THIRD BRIDGE MODE: CURVE (PATCHES) ──────────────────────────────
// "I want the option to make the loft a curve."  RULED and TANGENT build BRUSHES
// and are untouched.  CURVE builds q3 BEZIER PATCHES over the same stations:
// every corresponding profile EDGE sweeps to one patch strip, so a rectangular
// wall section becomes four strips (two large sides plus top and bottom) — the
// tube, with the two ends left open because the source faces are already there.
//
//   * CONTROL GRID.  width = 2*spans + 1 columns along the path, height = 3 rows
//     across the edge.  EVEN columns sit on the stations; ODD columns are the
//     ARC HANDLE for that span — the intersection of the two stations' path
//     tangents (ROUND AT, ITEM 3a corrects what the END stations' tangents are;
//     read the round-AT block below with this one), which is
//     kiwi_patchfillet.cpp's own ArcPoint rule generalised
//     (there the tangents meet at r/cos(a/2); here they are solved as the closest
//     approach of two lines and fall back to the midpoint when they are parallel,
//     which is what makes a straight span come out straight).  The three ROWS are
//     the edge's two rails and their exact midpoint, which is exact for a
//     straight edge because three evenly spaced collinear controls describe the
//     segment.
//   * THICKNESS IS FREE.  Both sheets of the wall are the same profile swept from
//     the same stations, so they stay parallel by construction — there is no
//     separate offset surface to keep in step.
//   * TEXTURES.  Each strip inherits from the face of brush A that is ADJACENT to
//     the profile edge it continues — i.e. the wall's own side, top or bottom —
//     so the swept surface carries on the material of the surface it grows out
//     of.  Alignment follows round AN's directive for the fillet's swept surface:
//     `Patch_KiwiCapAlign` (the Surface Inspector's CAP), because a loft strip IS
//     the fillet arc's shape — a swept quadratic — and not a flat end cap.
//   * NO COLLISION.  Patches are render surfaces; the standard caulk hint is
//     printed on every creation, exactly as the patch cylinder prints it.
//   * SPAN CAP.  width = 2*spans + 1 must stay inside the format's control grid
//     (Patch_GenericMesh refuses a width outside 3..15, pmesh.cpp:1550), so CURVE
//     clamps the density to KLOFT_MAX_CURVE_SEGS.  The HUD says so.
// ─────────────────────────────────────────────────────────────────────────────

// CURVE mode's density ceiling: 2*7 + 1 == 15 columns, the widest control grid
// the patch format accepts (pmesh.cpp:1550).  Nothing about the SHAPE wants more
// — a bezier span is already curved — so this is a format bound, not a taste one.
#define KLOFT_MAX_CURVE_SEGS    7
#define KLOFT_DEF_SEGS_CURVE    2     // one span per 45 degrees of a right-angle bend
#define KLOFT_CURVE_ROWS        3     // the edge's two rails + their midpoint

// The three bridge modes.  RULED and TANGENT are round AF's, unchanged; CURVE is
// round AO's patch medium and shares TANGENT's station path.
enum kiwiLoftBridge_t
{
    KLOFT_BRIDGE_RULED = 0,
    KLOFT_BRIDGE_TANGENT,
    KLOFT_BRIDGE_CURVE,
    KLOFT_BRIDGE_COUNT
};

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AT, ITEM 3 — WHY CURVE MODE WAS A POLYLINE, AND THE THREE THINGS THAT
//  FIX IT
// ═════════════════════════════════════════════════════════════════════════════
// USER REPORT, verbatim: "For the lofting curve option - it does not curve.  It
// only acts like a brush.  You need to make it a perfect curve and Add G0/G1/G2
// like in plasticity for start and end.  Also when doing a curve like this, have
// an option for 2-sided faces (Texture on BOTH sides).  Currently in some
// scenarios it only shows 1 side of the walls texture at a time."
//
// ── (a) THE FLATNESS, NAMED ──────────────────────────────────────────────────
// The control grid was right and the handle solve was right; the TANGENTS fed to
// it were not.  `SpanTangent` took the CENTRAL DIFFERENCE of the neighbouring
// stations' rail points and clamped to a ONE-SIDED difference at the two end
// stations — so at station 0 the tangent is exactly `normalize(P1 - P0)`, i.e.
// the span's own chord.  `ArcHandle` then intersects a line that LIES ON the
// chord with the other tangent line: the meeting point is on the chord, the
// quadratic degenerates to a straight segment, and the "arc" is a chord.  The
// same happens at the last span from the other side.
//
// THAT IS EVERY SPAN AT THE DEFAULT DENSITY.  `KLOFT_DEF_SEGS_CURVE` is 2, so
// spans 0 and 1 ARE the first and the last: both flat, meeting at an angle at the
// middle station.  A polyline of flat quads — "it only acts like a brush", and
// the picture is a chain of planar facets.  (At density 3+ the interior spans did
// bend, which is why this was never obvious.)
//
// THE END TANGENTS WERE ALREADY COMPUTED AND WERE THROWN AWAY.  `BuildStations`
// derives `outA` / `outB` from the two source faces' own plane normals — the
// direction the Hermite path leaves A and arrives at B — and they are LOCALS.
// They are now stored on the command and are what the end stations' tangents come
// from, which is the definition of G1 and is what the mode was documented to do.
//
// ── (b) CONTINUITY, PER END, AND EXACTLY WHAT EACH ONE MEANS ────────────────
//   G0  the end span leaves along its own CHORD.  This is the OLD behaviour,
//       kept and named rather than deleted: a crease where the sweep meets the
//       source face is sometimes what a mapper wants (a hard corner), and it is
//       the one setting that is exact for a straight run.
//   G1  the end span leaves along the SOURCE FACE'S NORMAL.  Tangent-continuous
//       with the wall it grows out of — no crease.  This is the DEFAULT and it is
//       exact: the first control column pair is colinear with the face normal, so
//       the surface's start direction IS the face's normal.
//   G2  "arc-fit", and the name is the honest one.  A quadratic Bezier span (the
//       patch format's 3 columns) has ONE interior control point, so its second
//       derivative is a single constant vector: there is no freedom left to match
//       a neighbouring curvature after the tangent has been matched.  What G2
//       does instead is force the end span to be a CIRCULAR ARC — the handle goes
//       at the tangent intersection a circle would put it at, distance
//       `chord / (2 cos(angle between the end tangent and the chord))` along the
//       end tangent.  That is `kiwi_patchfillet.cpp`'s `ArcPoint` rule
//       (`r / cos(alpha/2)`, kiwi_patchfillet.cpp:217-234) written for a path
//       whose radius is not known in closed form: for a quarter turn both give the
//       same point.  Constant curvature across the span is what makes two equal
//       spans agree on curvature at the station between them; it is NOT a
//       curvature match against the source face (that face is a PLANE, curvature
//       zero, and matching it would mean the sweep leaves dead straight — which is
//       G0 with a G1 direction, and is not what anyone means by asking for G2).
//       This limit is stated in RADIANT_KNOWN_ISSUES rather than papered over.
//   PER END, because Plasticity exposes per-end continuity on the commands that
//   have two ends and because the two ends of a loft genuinely differ (a wall
//   growing out of a flat face at one end and into a free-standing edge at the
//   other).  Two panel rows, START and END.
//
// ── (c) TWO-SIDED ───────────────────────────────────────────────────────────
// A q3 patch is a SHEET with one facing: `Curve_ComputeNormals` (pmesh.cpp:502)
// takes cross(dCol, dRow), so the surface is visible from one side and invisible
// from the other — which is the report's "only shows 1 side of the walls texture
// at a time".  The editor's own answer to that is the double-sided patch idiom: a
// second patch over the same control points with the row order REVERSED, which is
// what `patchInvert2` (pmesh.cpp:2232, the Curve->Negative primitive) does to a
// selection.  That primitive is `static` and takes the SELECTION, so it cannot be
// called from here; the in-tree precedent for orienting a patch being BUILT is
// kiwi_patchfillet.cpp's `u.rowFlip` (kiwi_patchfillet.cpp:870, honoured at
// :1437-1443) — swap the two row ends as the control points are written.  Two
// sided therefore writes the SAME strip twice, the second with its rows mirrored.
// DEFAULT OFF: it doubles the patch count, and a wall the player only ever sees
// from one side does not need it.
//
// The three settings persist in the profile ini (section KLOFT_SECTION) because a
// mapper who wants two-sided walls wants them for the whole session, and the loft
// command's `Reset()` runs on every `Begin()`.
#define KLOFT_SECTION        "KiwiLoft"
// The end tangent must lean toward the far end of the span by at least this
// cosine for the arc fit to mean anything; below it the arc is degenerate (the
// tangent points sideways or backwards) and the span falls back to the tangent
// intersection, then to the chord midpoint, exactly as before.
#define KLOFT_ARC_MIN_COS    0.05f
// Preview subdivision per span for the CURVE rails: enough to read as a curve, no
// more (the preview is a line batch and LineBudget pays for every segment).
#define KLOFT_CURVE_PREVIEW  6

enum kiwiLoftCont_t
{
    KLOFT_CONT_G0 = 0,
    KLOFT_CONT_G1,
    KLOFT_CONT_G2,
    KLOFT_CONT_COUNT
};

void KiwiLoft_RegisterCommands();
KiwiEditorCommand *KiwiLoft_CommandForId( int commandId );

// §3 palette predicate: at least ONE usable brush face is selected (one is
// enough — the command's own second stage picks the other).
bool KiwiLoft_CanExecute();

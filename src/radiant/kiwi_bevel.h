#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_bevel.h — RADIANT_UX_DESIGN §25's first two modeling ops:
//
//     KIWI_CMD_BEVEL_EDGE  34040   MODAL "Bevel Edge"   — add ONE plane per edge
//     KIWI_CMD_INSET_FACE  34041   MODAL "Inset Face"   — the clone compound
//     KIWI_CMD_REMOVE_FACE 34113   INSTANT "Remove Face (restore edge)"  [ROUND T]
//
// ROUND T NOTE — WHERE THE BEVEL COMMAND WENT.  The modal drag for
// KIWI_CMD_BEVEL_EDGE no longer lives in kiwi_bevel.cpp: the §25 chamfer and the
// round-Q patch fillet were the same drag with different commits, and the user
// directive "the default bevel mode should be chamfer, make it a Curve(Fillet) if
// (D) is pressed during the operation of the tool" asks for ONE tool, so they
// merged into kiwi_patchfillet.cpp's command and KiwiBevel_CommandForId forwards
// the id to it.  Everything else in this file — the frame, the appender, the
// inset, the exports below — is unchanged and is what both modes run on.
//
// Both follow kiwi_transform.h's three rules verbatim (apply from baseline, one
// gesture = one undo record opened at the FIRST mutation, reuse the ported cores)
// and kiwi_validity.h's §19 gate (rebuild → check → reject, HUD red, commit while
// invalid takes the cancel path).
//
// ═════════════════════════════════════════════════════════════════════════════
//  BEVEL / CHAMFER  (§25 "bevel/chamfer edge (add a plane)")
// ═════════════════════════════════════════════════════════════════════════════
// A chamfer on a plane-defined brush is exactly ONE extra half-space, which is
// why §25 parenthesises it that way: no topology has to be invented, the winding
// solver produces the new face for free.
//
// ── SELECTION ───────────────────────────────────────────────────────────────
// SEL_EDGE items.  One physical brush edge is TWO (faceIndex, edgeIndex) items
// (kiwi_selection.h DESIGN NOTE 3), so the same unordered-segment dedup
// kiwi_transform.cpp's BeginEdges uses is applied here at the same 0.1-unit
// tolerance the ported FindPoint / SetupVertexSelection dedup uses.  Unlike the
// edge MOVE, two bevels that share a face are NOT ill-posed — each writes its own
// NEW face and never touches an existing one — so there is no clash rule here.
//
// ── THE PLANE ───────────────────────────────────────────────────────────────
// For an edge shared by exactly two faces with baseline outward unit normals
// n1, n2:
//
//     n  = normalise( n1 + n2 )            the angle bisector, pointing OUTWARD
//     u  = normalise( e1 - e0 )            the edge direction
//     v  = cross( n, u )                   the third basis vector
//     c  = midpoint(e0,e1) - n * d         the plane's anchor point
//
// u ⟂ n exactly: the edge lies in both faces, so (e1-e0)·n1 = (e1-e0)·n2 = 0 and
// therefore (e1-e0)·(n1+n2) = 0.  (The code still re-orthogonalises u against n
// before normalising, because the two windings' endpoints are matched at a 0.1
// tolerance and n1/n2 come from floats.)  n, u, v are consequently an orthonormal
// right-handed triple with cross(u, v) = cross(u, cross(n,u)) = n(u·u) - u(u·n) = n.
//
// d is the PERPENDICULAR DEPTH of the cut, measured from the edge inward along
// -n.  d = 0 puts the plane exactly through the edge (it touches the solid in a
// line, so the new face has zero area and §19's V3/V4 reject it), and a large d
// eventually swallows a neighbouring face (V3 again).  Both ends are therefore
// caught by the existing gate rather than by hand-written limits.
//
// ── WINDING ORDER, PROVED NOT GUESSED ───────────────────────────────────────
// Face_MakePlane (brush.cpp 0x470470) computes  normal = cross(p0 - p1, p2 - p1)
// and then normalises.  Writing
//
//     planepts[0] = c + u * s      planepts[1] = c      planepts[2] = c + v * s
//
// gives  cross(p0 - p1, p2 - p1) = cross(u*s, v*s) = s² * cross(u, v) = s² * n,
// i.e. exactly +n after normalisation — OUTWARD, which is the convention the
// whole brush model is built on (interior at n·p <= dist).  `s` is the spread,
// clamped to [KBEV_MIN_SPREAD, KBEV_MAX_SPREAD] around the edge length so the
// three points are never near-collinear (§19's V2).
//
// ── HOW THE FACE IS ADDED ───────────────────────────────────────────────────
// `Face_Alloc( def, src )` (brush.cpp 0x471500) is the ported grow-by-one: it
// allocates faceCount+1 entries, memcpy's the existing faces (winding pointers
// included, NULLing them in the old array so Face_Free does not double-free),
// appends a COPY of `src` with a cloned winding, and returns the new entry.
// Passing one of the edge's adjacent faces as `src` is what gives the bevel face
// its MATERIAL for free — the whole 4-layer MaterialDef block, the contents, the
// toolflags and the packed colour all come across in that memcpy, which is
// exactly what §26 ("preserve per-face material through every new op") asks for.
//
// The chosen alternative was `Ed_BrushSetFaceCount` + re-copy (what kiwi_extrude
// does).  It is REJECTED here: that helper frees the whole face array and
// re-stamps every face with the CURRENT material, so keeping the brush's existing
// materials would mean copying 232-byte face records back over it by hand.
// Face_Alloc already does precisely the right thing and is the ported spelling
// of "grow the face array", so it is used.  FLAGGED because it means this file
// holds NO pointer into def->faces across a Face_Alloc call — the array moves.
//
// The inverse is `Brush_RemoveFace( def, index )` (brush.cpp 0x471640), which
// frees the face's winding and shifts the array down.  Bevel faces are always
// appended at the TAIL and removed in reverse order, so the indices are exact.
//
// ── PER-FRAME SHAPE (apply from baseline, kiwi_transform.h rule 1) ──────────
//     1. remove every bevel face added last frame (reverse order)   → the brush
//        is byte-identically the original again: this file never writes an
//        EXISTING face's planepts, so "baseline" for the untouched faces is the
//        brush itself, and the appended faces are gone.
//     2. compute the planes for the current d;
//     3. Face_Alloc one face per edge, write its three planepts;
//     4. KiwiValid_Rebuild + KiwiValid_CheckBrush.
//        FAIL → go back to step 1's state and raise INVALID.  Live geometry is
//        never left in a rejected state, not for one frame.
// The cost is 2·K face-array reallocations per frame for K bevelled edges, which
// for a drag on a handful of edges is a few kilobytes of churn — deliberately
// traded for a rollback that is exact by construction.
//
// Multiple brushes are handled by iterating them independently; several edges of
// the SAME brush all get their planes in one pass (step 3 loops all units before
// step 4 rebuilds), which is what "all bevel faces added together from baseline"
// means.
//
// ── DRAG / NUMERIC ──────────────────────────────────────────────────────────
// Drag = the closest point on the bisector line through the driving edge's
// midpoint (the same RayAxis solve face push/pull and region extrude use),
// reduced to the signed depth d.  Numeric = an exact depth in display units
// (inches), handed over already converted by the numeric layer.
//
// ── ROUND Q: SMOOTH BY DEFAULT, ZERO SNAPPING ───────────────────────────────
// USER DIRECTIVE: "much finer in detail, smooth by default with 0 snapping".
// d is the RAW closest-point scalar in full float precision and NOTHING rounds
// it — there is no grid quantisation on this gesture at any spacing, and CTRL
// (the snap suppressor, kiwi_snap.h) therefore changes nothing about it because
// there is nothing left to suppress.  Numeric entry is still exact.
//
// It used to round d to KiwiUnits_GridSpacingWorld() whenever the frame's snap
// query came back non-geometry — i.e. on SNAP_GRID, which is the fallback arm
// and so was on almost every frame.  That, and not the drag math, was the whole
// of the reported coarseness: the chamfer could only take whole-grid depths.
//
// A GEOMETRY snap survives and is the one remaining arm: it does not quantise,
// it names an exact target ("chamfer up to that vertex"), which is the opposite
// of the defect.  The same removal is applied to INSET below, for the same
// reason and to keep the two siblings behaving alike.
//
// ═════════════════════════════════════════════════════════════════════════════
//  INSET FACE  — and an honest statement of what it is
// ═════════════════════════════════════════════════════════════════════════════
// **A CLASSIC-BRUSH INSET IS NOT A MESH INSET.**  On a polygon mesh, "inset face"
// inserts a RING of new vertices inside the face and rebuilds it as a border quad
// strip plus a smaller centre face.  A convex plane-defined brush cannot express
// that: it has no per-face vertex list to insert into, its faces are DERIVED from
// the half-space intersection, and a border ring would make the solid non-convex.
// There is no way to write it as a plane edit, so it is not written as one.
//
// What ships instead is the compound a brush modeller actually uses, and what
// Plasticity's inset degrades to on a solid: **Inset (clone)**.
//
//     * CLONE the brush (the ported Brush_Clone, brush.cpp 0x475D20);
//     * on the clone, translate every face ADJACENT to the target face inward
//       along its OWN outward normal by d  (planepts -= n_g * d);
//     * leave the target face and every non-adjacent face alone.
//
// The result is a smaller brush sharing the target face's plane, centred inside
// the original's cross-section — i.e. exactly the brush you then push/pull to get
// the extruded-inset look.  The ORIGINAL BRUSH IS NEVER TOUCHED.
//
// "Adjacent" = shares an EDGE with the target face, i.e. its winding contains at
// least two of the target winding's points (0.1-unit match, the ported dedup
// tolerance).  Faces that merely touch at a corner are not adjacent and are left
// alone, which is what keeps the inset planar-uniform.
//
// The clone is built UNLINKED and stays unlinked for the whole gesture: it is
// drawn as a wireframe preview by this command's DrawWorld and only lands at
// commit, through the same creation sequence kiwi_extrude.h documents
// (Select_Deselect(1) → KiwiCmd_UndoBegin → Entity_LinkBrush → Brush_AddToList →
// Brush_AddToList2).  A rejected or cancelled gesture calls Brush_Free_R on it,
// whose refCount==0 / no-owner-chain precondition an unlinked clone satisfies by
// construction — so nothing reaches the map, the selection or the undo stack.
//
// Drag maps the cursor's distance from the face centre IN THE FACE PLANE:
// d = (radius at gesture start) - (radius now), so pulling the cursor toward the
// centre grows the inset.  Numeric is an exact depth in inches.
// ─────────────────────────────────────────────────────────────────────────────

class  KiwiEditorCommand;
struct brush_t;                   // qe3.h:474 (the 88-byte brush DEFINITION)
struct selbrush_t;                // qe3.h:429 (the 56-byte brush INSTANCE / list node)

#define KBEV_MIN_DIST     0.25f    // world units — below this the gesture is a no-op
#define KBEV_MIN_SPREAD   16.0f    // planept spread floor (§19 V2 headroom)
#define KBEV_MAX_SPREAD   1024.0f  // …and ceiling, so a huge edge keeps float precision
#define KBEV_MAX_EDGES    64       // v1 cap on bevelled edges in one gesture
#define KBEV_MAX_FACES    64       // v1 cap on inset target faces in one gesture

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND Q — THE CHAMFER FRAME AND THE CHAMFER FACE, EXPORTED
// ═════════════════════════════════════════════════════════════════════════════
// kiwi_patchfillet.cpp's "Fillet Edge (patch)" is a chamfer PLUS a bezier patch
// laid into the notch the chamfer cut, so it needs exactly the two things this
// file already does: the orthonormal bisector frame for one brush edge, and the
// appended half-space that realises the chamfer.  Both are lifted out here
// rather than re-derived over there — a second copy of the frame derivation (and
// of the winding-order proof above it) is the duplicate-function drift this
// codebase keeps paying for.
//
// The frame is the one this header derives at the top: n the OUTWARD bisector, u
// the edge direction, v = n × u, with cross(u, v) == n exactly.  `adj` names the
// two adjacent faces in the order they were found.
//
// ── ROUND T: `srcFace` IS NO LONGER JUST adj[0] ─────────────────────────────
// USER REPORT, verbatim: "Fillets are still not inheriting a texture.  They're
// being created invisible!"  `srcFace` was hardcoded to `adj[0]` — an ARBITRARY
// one of the two faces at the corner — and the chamfer, and then the patch laid
// over it, inherited whatever that face happened to wear.  After one Cut, half
// the edges in a map have a CAULK face for a neighbour, so the chamfer came out
// caulk and the patch was created carrying a tool material: present, selectable
// and drawing nothing.  It now runs through kiwi_material.h R5 — the first
// INHERITABLE face of {adj[0], adj[1]}, else the brush's dominant one, else
// adj[0] exactly as before.  See kiwi_material.h for the whole chain.
//
// ── ROUND T: `bias` — THE CHAMFER ANGLE ─────────────────────────────────────
// USER DIRECTIVE, verbatim: "Also allow angle adjustments on both of them."
// The chamfer plane's normal is the bisector n ROTATED ABOUT THE EDGE by `bias`
// radians, in the +v sense:
//
//     n(bias) = n·cos(bias) + v·sin(bias)          (u·n == 0, so Rodrigues
//                                                   collapses to this)
//
// with bias = 0 the symmetric bisector, i.e. every pre-round-T chamfer exactly.
// The sign is resolved by the caller so that POSITIVE means "toward adj[1]"; the
// magnitude must stay strictly inside the half-angle between n and either face
// normal or the chamfer plane stops separating the two faces, which is what
// KiwiBevel_BiasLimit reports.
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
    int         srcFace;        // ROUND T: the INHERITABLE face (kiwi_material.h R5)
    int         adj[2];         // the two faces sharing this edge
    float       bias;           // ROUND T: chamfer tilt about the edge, radians (0 = bisector)
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

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND T — REMOVE FACE (RESTORE EDGE)
// ═════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: "Once a chamfer/bevel is made, allow (DEL) key to
// remove the chamfer and restore the original edge/corner."
//
// THE RULE, stated once and advertised in the hint strip:
//
//     DELETE with EXACTLY ONE BRUSH FACE selected  →  remove that face from the
//                                                     brush and rebuild, which
//                                                     re-extends its neighbours
//                                                     and recreates the edge.
//     DELETE with anything else selected           →  the classic delete,
//                                                     untouched.
//
// It is not a "was this face made by a bevel" test, because there is no such
// fact stored anywhere and every heuristic for guessing it (face count > 6, an
// obtuse two-neighbour winding, …) is a guess that will be wrong on somebody's
// map.  Removing a face from a convex brush IS the restore operation — a chamfer
// is one extra half-space and nothing else — so the honest verb is the general
// one, named for what it does.
//
// SAFETY: the removal is validated by §19 (KiwiValid_CheckBrush) on the REBUILT
// brush BEFORE the record closes.  A face whose removal leaves fewer than four
// half-spaces, an unbounded solid or a degenerate one is REFUSED with a console
// line and the brush is restored byte-for-byte from its planepts snapshot;
// nothing falls through to the classic delete, because "I could not do the thing
// you asked, so I deleted your brush instead" is not an acceptable answer.
//
// A FILLET is a chamfer plus a patch, so the verb also removes any PATCH the
// selection carries alongside — see kiwi_bevel.cpp for the pairing rule.
bool KiwiBevel_CanRemoveFace();          // exactly one live SEL_FACE item on a brush
bool KiwiBevel_RemoveFaceRestoreEdge();  // true = it ran (and printed its own line)

// The "Modeling" block's bevel/inset buttons (drawn by KiwiCsg_MenuItems' caller).
void KiwiBevel_MenuItems();

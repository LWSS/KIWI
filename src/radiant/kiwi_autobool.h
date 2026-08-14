#pragma once
// kiwi_autobool.h — KIWI-UX ROUND AB, ITEM 3: AUTO BOOL.
//
// USER REQUEST, verbatim: *"You should add an 'auto bool' that attempts to
// consolidate a large group of brushes all at once to reduce brush-count. Might be
// a bad idea, but we can try it."*
//
// ROUND AN EXTENSION, verbatim: *"Make it try combining all the solids at once
// along with more combinations."*  See "THE THREE PHASES" below.
//
// ── WHAT IT IS ─────────────────────────────────────────────────────────────
// Take the current brush selection and greedily merge every PAIR whose union is
// exactly one convex brush, repeating until no pair is left that can merge; then
// offer each TOUCHING CLUSTER of three or more survivors to the core all at once;
// then sweep the pairs again from the other end.  One undo record for the whole
// run, all three phases.  Strictly opt-in: it is registered UNBOUND and reached only
// by name from the command palette or by the CSG panel button.  It is never
// triggered by anything else, ever.
//
// ── THE THREE PHASES (ROUND AN) ────────────────────────────────────────────
//  A. PAIRWISE, FORWARD — the original round AB cascade, unchanged.  Cheap, needs no
//     guard (a pair either forms one convex brush or it does not), does the bulk of
//     the work, and leaves the cluster phase a smaller and sparser graph.
//  B. CLUSTERS — build the touching graph of what phase A could not take (one node
//     per surviving brush, an edge wherever the epsilon-padded bounds test says two
//     of them abut) and hand each connected component of >= 3 members to
//     `Brush_MergeList` WHOLE.  This reaches what pairs structurally cannot: a plus
//     of five boxes has no two members whose union is convex, but all five together
//     are one brush.  GUARDED — see below; it is the only phase that can be wrong.
//  C. PAIRWISE, REVERSED — the same cascade walked from the other end.  The greedy
//     result is order-dependent, so a reversed sweep rescues pairs the forward one
//     had already spoiled, plus whatever a rejected cluster still had in it.
// Each phase prints its own count, and the cluster phase prints how many components
// it tried, merged, and threw out on the volume guard.
//
// ── THE CLUSTER GUARD: TOTAL VOLUME CONSERVATION ───────────────────────────
// `Brush_MergeList` classifies a face INNER whenever ANY other brush in the set
// carries a flipped-equal plane, WITHOUT checking that the two faces geometrically
// touch (the round AB finding, restated in the last limit below).  For a pair that
// is the intended meaning of "shared face".  For a SET of three or more it is a
// live hazard: two members on the same wall plane but a room apart cancel each
// other's walls, both faces vanish from the outer set, and the "merged" brush is a
// hull that has SWALLOWED the empty space between them.  Winding_PlanesConcave
// cannot catch it — the faces it would have rejected on are the ones that got
// classified away.
//
// So phase B proves every merge before keeping it.  A legitimate union of solids
// meeting only at shared faces has exactly the sum of its parts' volumes (the shared
// faces are measure zero); a hull that swallowed a void is strictly bigger.  The
// implementation sums the members' winding volumes (divergence theorem, one fan
// triangle at a time, taken relative to the cluster centre and accumulated in
// double), measures the merged brush the same way, and accepts only if the two agree
// within 0.1% — a tolerance that sits in the wide empty band between float noise
// (~1e-6 relative) and a swallowed room (tens of percent).  On rejection the merged
// brush is freed and every member is linked straight back, unmodified: the originals
// are deliberately NOT freed until after the guard passes, which is what makes the
// restore trivial.  An unmeasurable brush (a missing or degenerate face winding)
// counts as a rejection — "I cannot verify this" reads as "then do not do it".
//
// WHAT THE GUARD CANNOT CATCH, stated plainly:
//  * a volume-NEUTRAL misclassification — a hull that lost as much volume somewhere
//    as it gained elsewhere.  Geometrically contrived and not something a map can
//    reach by accident, but the check is a scalar and a scalar cannot see shape.
//  * it produces FALSE NEGATIVES on OVERLAPPING inputs.  Two brushes that intersect
//    are double-counted in the "before" sum, so a perfectly correct merge of them
//    measures short and is refused.  That is the safe direction to be wrong in, and
//    the pairwise phases (which are not guarded) still handle those.
//  * it says nothing about MATERIALS, contents or layer — those follow whatever
//    `Brush_MergeList` picked, exactly as a manual CSG Merge would.
//
// ── WHAT DECIDES A MERGE — NOT THIS FILE ───────────────────────────────────
// `Brush_MergeList` (csg.cpp:409, 0x47D600) is the ported core, and it is the ONLY
// thing here that looks at geometry.  Its rule: classify each face as INNER (some
// other brush in the set carries a flipped-equal plane) or OUTER, reject the set if
// any pair of OUTER faces is `Winding_PlanesConcave` (csg.cpp:478-487,
// winding.cpp:246), then build one brush from the outer planes, skipping duplicates.
// It returns nullptr when the union would not be convex.  Materials come from
// `Face_Alloc( newBrush, face1 )` (csg.cpp:541) per surviving outer face — i.e.
// exactly what a manual Selection -> CSG -> Merge already gives you.  NOTHING about
// that is reimplemented, second-guessed or tuned here.
//
// The surrounding surgery is `CSG_Merge`'s (csg.cpp:572-649) applied to a PAIR
// rather than to the whole selection: validate, unlink both into a NULL-terminated
// merge list threaded through their own `.next`, call `Brush_MergeList`, then either
// `Brush_Free` both and `Brush_AddToList2` the result, or `Brush_AddToList2` both
// originals back.  `CSG_Merge` itself cannot be used pairwise — it takes no
// arguments and merges everything on `selected_brushes` (its contract, and the
// reason kiwi_join.cpp and kiwi_boolean.cpp drive it by rewriting the selection) —
// and it also prints five lines per attempt, which over hundreds of attempts would
// bury the console.
//
// ── THE LIMITS, STATED HONESTLY ────────────────────────────────────────────
//  * CONVEX UNION ONLY.  This is not a simplifier.  It never deletes a face, never
//    moves a plane, never approximates.  If a pair or a cluster does not form one
//    convex solid it is refused and every member is left exactly as it was.  The
//    result is always bounded by a subset of the inputs' own planes — which is why
//    the pairwise phases need no volume check and why, when the cluster phase's
//    check DOES fire, it is reporting a `Brush_MergeList` misclassification and not
//    a rounding difference.
//  * STILL GREEDY, SO STILL ORDER-DEPENDENT.  Merging A+B first can make A+B+C
//    impossible where merging B+C first would have allowed it.  A different selection
//    order can give a different (never a WRONG, only a less thorough) result.  Round
//    AN's reverse sweep narrows that gap without closing it: there is still no
//    search and no backtracking, because a version that is easy to reason about
//    beats one that is clever and surprising.
//  * THE CLUSTER PHASE MAKES ONE PASS, not a fixed point.  Components are disjoint by
//    construction, so merging one cannot make another viable; and a component the
//    core or the guard refused is not re-attempted in sub-clusters (that would be a
//    search over the subset lattice).  Whatever such a component still has in it is
//    left to phase C.
//  * IT DOES NOT MERGE ACROSS ENTITIES, and it skips patches and fixed-size entity
//    brushes.  Those are `CSG_Merge`'s own refusals (csg.cpp:589-603) and they are
//    reproduced rather than relaxed.
//  * THE MERGED BRUSH TAKES `g_activeLayer_string` and the merge-list head's owner
//    (csg.cpp:548-555, inside `Brush_MergeList`).  That is the existing Merge
//    behaviour; over a cascade it simply applies more than once.
//  * `Brush_MergeList` classifies a face INNER whenever ANY other brush in the set
//    carries a flipped-equal plane, without checking that the two faces geometrically
//    touch.  For a PAIR that is the intended meaning of "shared face", so phases A and
//    C rely on it as-is.  For a SET it is a real hazard, which is the whole reason the
//    cluster phase (1) only ever offers ONE CONNECTED COMPONENT at a time, never the
//    raw selection, and (2) proves the result by volume before keeping it.
//
// ── PREFILTER ──────────────────────────────────────────────────────────────
// Pairs are gated on an epsilon-padded bounds test before any plane work, and the
// cluster phase builds its touching graph out of that same test, because the
// interesting neighbours ABUT rather than overlap and a strict test would reject
// every one of them.  The epsilon is the ported `Select_Touching_R` test's
// (select.cpp:1678-1679), reused through kiwi_selext.h.  Note what this makes the
// graph: bounds-touching, not surface-touching, so a component can be slightly
// larger than the true adjacency — which costs a refused merge at worst, never a
// wrong one, since the core and the guard both still have to agree.
//
// See RADIANT_UX_DESIGN.md §58.3.

// Palette predicate: >= 2 usable, same-entity, non-patch brushes selected.
bool KiwiAutoBool_CanExecute();

// The instant-command hook.  Returns false for any id that is not Auto Bool.
bool KiwiAutoBool_DispatchInstant( unsigned int commandId );

// Registration (called from KiwiCmd_RegisterCommands).
void KiwiAutoBool_RegisterCommands();

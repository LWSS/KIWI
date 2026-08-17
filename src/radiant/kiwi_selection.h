#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_selection.h — RADIANT_UX_DESIGN §1: the typed selection core + the legacy
// adapter.  NEW code (sanctioned divergence); it sits OVER the ported cores and
// never changes them.
//
// The ~50 ported operation cores consume the legacy selection globals
// (`selected_brushes`, `g_SelectedFaces`, `QE_SingleBrush()`).  This header adds a
// typed selection model on top and defines exactly two crossings:
//
//   Sel_SyncToLegacy()      selection_t  →  legacy globals   (so old ops run on a
//                                           selection the new layer built)
//   Sel_RebuildFromLegacy() legacy globals →  selection_t    (so the new layer sees
//                                           selections old code made)
//
// ── DESIGN NOTE 1: the object pointer is selbrush_t*, not brush_t* ────────────
// The spec sketch writes `brush_t *brush`.  In CoD4Radiant a brush is TWO objects
// (see the qe3.h ruling): the 56-byte INSTANCE node `selbrush_t` and the 88-byte
// DEFINITION `brush_t`, and MANY instances may share one def (def->refCount).
// Every legacy selection global addresses the INSTANCE:
//   * `selected_brushes` / `active_brushes` are selbrush_t sentinel lists,
//   * `selface_t.brush` (g_SelectedFaces) is a selbrush_t*,
//   * `edTrace_t.hit.brush` (Test_Ray) is a selbrush_t*,
//   * the per-instance patch flag is `selbrush_t.patch->selected`.
// A brush_t* cannot name WHICH instance was clicked, so sel_item_t stores the
// instance.  The definition is always `item.brush->def`; faces live on the def
// (`def->faces[faceIndex]`), the per-instance visibility face array on the node
// (`item.brush->faces[faceIndex]`, a faceVis_s built by Brush_CheckBuildFaceVis).
//
// ── DESIGN NOTE 2: face selection in THIS port ───────────────────────────────
// Faces are NOT a `selFace[]` array of face_t*.  They live in `g_SelectedFaces`
// (qe3.h), the port of the binary's MFC CArray<selface_t> at 0x73C70C, whose
// element is `selface_t { selbrush_t *brush; faceVis_s *face; int index; }`.
// `index` is the face's ordinal inside the brush def; `face` is
// `&brush->faces[index]` (the INSTANCE faceVis array).  sel_item_t therefore
// carries only the ordinal — the faceVis_s pointer is re-derived on sync, after
// Brush_CheckBuildFaceVis, because the instance array is rebuilt on version bumps
// and a cached pointer would dangle.
//
// ── DESIGN NOTE 3: edge / vertex indexing ────────────────────────────────────
// A brush edge is (faceIndex, edgeIndex) where edgeIndex e names the winding edge
// from `w->p[e]` to `w->p[(e + 1) % w->numpoints]` of `def->faces[faceIndex].w`.
// A brush vertex is (faceIndex, vertIndex) naming `w->p[vertIndex]` of that same
// winding.  This is the cheapest faithful indexing: it needs no extra structure,
// it is stable for as long as the winding is (rebuilt by Brush_BuildWindings, the
// same lifetime the legacy d_points/d_edges handles have), and it maps 1:1 onto
// what the drawer and Test_Ray already walk.
//   Consequence, deliberate: one world corner shared by 3 faces is 3 distinct
// (faceIndex, vertIndex) pairs.  Picking resolves the ambiguity deterministically
// (lowest faceIndex wins — kiwi_pick.cpp); a future vertex-move command must fan
// a vertex item back out to its coincident partners, exactly as the ported
// SetupVertexSelection / FindPoint dedup already does at 0.1 units.
// PATCHES: a patch item is a control point — faceIndex = -1 and
// vertIndex = col * patch->height + row into `def->patch->ctrl[col][row]`, the
// same linear order Patch_EditPatch (pmesh.cpp) pushes into d_points[].
// ─────────────────────────────────────────────────────────────────────────────

#include <vector>

struct selbrush_t;
struct patchMesh_t;

// ── selection kinds + the 1–5 mode-filter mask ───────────────────────────────
enum sel_kind_t
{
    SEL_VERTEX = 0,
    SEL_EDGE   = 1,
    SEL_FACE   = 2,
    SEL_OBJECT = 3,
    SEL_KIND_COUNT
};

typedef unsigned int sel_mask_t;

#define SEL_KIND_BIT( k )   ( 1u << (unsigned)( k ) )
#define SEL_MASK_VERTEX     SEL_KIND_BIT( SEL_VERTEX )
#define SEL_MASK_EDGE       SEL_KIND_BIT( SEL_EDGE )
#define SEL_MASK_FACE       SEL_KIND_BIT( SEL_FACE )
#define SEL_MASK_OBJECT     SEL_KIND_BIT( SEL_OBJECT )
#define SEL_MASK_EVERYTHING ( SEL_MASK_VERTEX | SEL_MASK_EDGE | SEL_MASK_FACE | SEL_MASK_OBJECT )

// ── one selected thing ───────────────────────────────────────────────────────
// `brush` NULL == the null item (an unset `active`, a missed pick).  Unused
// indices are -1 so a memcmp-free equality test stays exact.
struct sel_item_t
{
    sel_kind_t  kind      = SEL_OBJECT;
    selbrush_t *brush     = nullptr;   // the INSTANCE node (see DESIGN NOTE 1)
    int         faceIndex = -1;        // SEL_FACE / SEL_EDGE / SEL_VERTEX (-1 = patch ctrl pt)
    int         edgeIndex = -1;        // SEL_EDGE   — winding edge p[e] → p[e+1]
    int         vertIndex = -1;        // SEL_VERTEX — winding vert, or patch ctrl-point ordinal
};

struct selection_t
{
    std::vector<sel_item_t> items;
    sel_item_t              active;   // last-clicked; the "active item" ops key off
};

// ── item helpers ─────────────────────────────────────────────────────────────
inline bool Sel_ItemValid( const sel_item_t &a )        { return a.brush != nullptr; }
bool        Sel_ItemEqual( const sel_item_t &a, const sel_item_t &b );

// ── LIVENESS (the guard every cached selbrush_t* must pass before a deref) ────
// Delete, undo and map close free brush nodes with no notification this layer
// sees, so ANY selbrush_t* cached across a frame boundary is suspect
// (RADIANT_KNOWN_ISSUES "UX overhaul").  The test is pure POINTER COMPARISON
// against the two display lists — it never dereferences the candidate, so it is
// safe to call on freed memory.  Phase 1b's hover pass introduced this rule;
// Phase 3's transform commands cache brush pointers for a whole gesture and use
// exactly the same guard, so the check lives here rather than in two copies.
// KNOWN LIMIT (unchanged): a freed node whose address is reused by a new
// selbrush_t passes.  Cosmetic for hover; for a transform the gesture is
// abandoned on the next selection generation change anyway.
bool Sel_BrushLive( const selbrush_t *b );

// ── KIWI-UX (CLEANUP, A-14): the patch control-grid bound, named ─────────────
// `patchMesh_t::ctrl` is declared `drawVert_t ctrl[16][16]` (qe3.h:749), so a
// width or height outside 1..16 means the def is not a usable patch and indexing
// it would walk off the array.  The bare literal 16 was spelled at two sites
// (kiwi_validity.cpp's baseline capture, kiwi_transform.cpp's VertexPos) with no
// name and no derivation; both now ask this.  It lives HERE rather than in
// kiwi_validity.h because DESIGN NOTE 3 above already owns the
// ctrl[col][row] indexing scheme, and because every asker already includes this
// header (kiwi_validity.cpp is the one that did not — one added include).
#define KIWI_PATCH_MAX_DIM 16
bool Patch_DimsSane( const patchMesh_t *pm );

sel_item_t  Sel_MakeObject( selbrush_t *b );
sel_item_t  Sel_MakeFace  ( selbrush_t *b, int faceIndex );
sel_item_t  Sel_MakeEdge  ( selbrush_t *b, int faceIndex, int edgeIndex );
sel_item_t  Sel_MakeVertex( selbrush_t *b, int faceIndex, int vertIndex );
sel_item_t  Sel_MakePatchPoint( selbrush_t *b, int ctrlIndex );

// ── KIWI-UX (CLEANUP, A-13 / C-49): resolving an item to world geometry ──────
// ONE spelling each of the two questions that were written out four times apiece
// with four different guard sets (kiwi_hover, kiwi_snap, kiwi_conselect,
// kiwi_transform for the edge; kiwi_hover, kiwi_transform, kiwi_selconv,
// kiwi_boxselect for the vertex).  Both carry the UNION of the guards those
// copies had, so no site loses a check:
//   * the def, its face array and faceIndex against def->faceCount;
//   * the winding, and numpoints inside 1..MAX_POINTS_ON_WINDING (kiwi_conselect
//     and kiwi_selconv had this bound; the others did not);
//   * Patch_DimsSane on the control grid, and col/row inside it;
//   * the edge / vertex ordinal against numpoints.
//
// `checkLive` is the ONE guard that is not free: Sel_BrushLive walks the display
// lists, so a per-frame loop over the selection pays O(sel * brushes) for it —
// the same cost DominantKind's own checkLive parameter exists to let a caller
// decline (kiwi_transform.cpp).  It defaults to the SAFE answer; the loop sites
// that already test liveness themselves, or that never held the pointer across a
// frame, pass false explicitly and say why at the call site.
//
// Sel_EdgeEnds additionally refuses an item whose kind is not SEL_EDGE (the
// kiwi_conselect copy did).  Sel_ItemWorldPos does NOT test kind — none of its
// four copies did, and its callers select the arm by kind before calling.
bool Sel_EdgeEnds   ( const sel_item_t &it, float outA[3], float outB[3],
                      bool checkLive = true );
bool Sel_ItemWorldPos( const sel_item_t &it, float out[3], bool checkLive = true );

// ── the global selection + the pick-time mode mask ───────────────────────────
// KiwiSel() is THE accessor; it guarantees any pending legacy change has been
// folded in before the caller sees the vector (see Sel_RebuildFromLegacy below).
selection_t &KiwiSel();
sel_mask_t   KiwiSel_GetModeMask();
void         KiwiSel_SetModeMask( sel_mask_t mask );

// ── selection_t mutation (does NOT sync — call Sel_SyncToLegacy when done) ───
void Sel_Clear   ( selection_t &sel );
bool Sel_Contains( const selection_t &sel, const sel_item_t &item );
bool Sel_Add     ( selection_t &sel, const sel_item_t &item );   // true if newly added
bool Sel_Remove  ( selection_t &sel, const sel_item_t &item );   // true if it was present
bool Sel_Toggle  ( selection_t &sel, const sel_item_t &item );   // true if now selected

// ── the two crossings ────────────────────────────────────────────────────────
// Push KiwiSel() into the legacy globals so the ported ops (hide, texture apply,
// clipper, CSG, …) operate on it.  Drives legacy code through its OWN entry
// points (Select_Deselect / Select_Brush / g_SelectedFaces) — it never hand-edits
// the sentinel lists.  Re-entrancy safe: the rebuild hooks the legacy calls fire
// are suppressed for the duration.
void Sel_SyncToLegacy();

// Reconstruct KiwiSel() from the legacy globals.  Called by the // KIWI-UX hooks
// at the legacy selection funnels (see kiwi_selection.cpp for the hook list).
void Sel_RebuildFromLegacy();

// O(1) hook body: mark the legacy state changed.  The rebuild runs before the
// next observation of KiwiSel() — see the DEVIATION note in kiwi_selection.cpp.
void Sel_InvalidateFromLegacy();

// The same, plus "and that change was a WHOLESALE DESELECT".  Hooked at exactly
// one site: Select_Deselect (select.cpp), the legacy funnel that splices the
// whole selected list back onto active_brushes.
//
// WHY IT EXISTS (shakeout D).  SEL_EDGE / SEL_VERTEX items have no legacy
// representation at all, so the rebuild CARRIES them across a legacy change.
// Until shakeout D the carry condition was "the owning brush is still on
// selected_brushes", which only worked because Sel_SyncToLegacy PROMOTED those
// owners into the legacy selection.  The user directive "when selecting an edge,
// the entire solid should not get selected as well" removed that promotion, so an
// edge-only selection now syncs to an EMPTY legacy state — indistinguishable from
// a deselect if the rebuild tried to INFER intent from the list.  It no longer
// infers: a deselect says so here, and everything else carries.
void Sel_NoteLegacyDeselect();

// Bumped on every change to KiwiSel() from either direction; UI/render layers can
// use it to skip work.  Never reset.
unsigned Sel_Generation();

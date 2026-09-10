#pragma once
#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Typed selection over the legacy selected_brushes and g_SelectedFaces stores.
// Sel_SyncToLegacy pushes typed state; Sel_RebuildFromLegacy adopts legacy state.
//
// Items store selbrush_t instances because brush_t definitions can be shared.
// Legacy faces at 0x73C70C store instance faceVis pointers, so typed faces keep
// only the definition-face ordinal and re-derive faceVis after Brush_CheckBuildFaceVis.
//
// Brush edges and vertices index definition windings by face plus edge/vertex.
// Indices last only until Brush_BuildWindings rebuilds the winding. Shared world
// corners remain distinct per face; picking chooses the lowest faceIndex, while
// moves fan out at the ported SetupVertexSelection/FindPoint tolerance of 0.1 units.
// Patch controls use faceIndex = -1 and vertIndex = col * height + row, matching
// Patch_EditPatch's d_points order.

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
    selbrush_t *brush     = nullptr;   // instance node; definitions may be shared
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

// Cached brush nodes can be freed or replaced. This compares pointers against the
// two display lists before dereference; a reused address can still pass until a
// selection-generation change invalidates the gesture.
bool Sel_BrushLive( const selbrush_t *b );

// patchMesh_t::ctrl is 16x16; reject dimensions that would index past it.
#define KIWI_PATCH_MAX_DIM 16
bool Patch_DimsSane( const patchMesh_t *pm );

sel_item_t  Sel_MakeObject( selbrush_t *b );
sel_item_t  Sel_MakeFace  ( selbrush_t *b, int faceIndex );
sel_item_t  Sel_MakeEdge  ( selbrush_t *b, int faceIndex, int edgeIndex );
sel_item_t  Sel_MakeVertex( selbrush_t *b, int faceIndex, int vertIndex );
sel_item_t  Sel_MakePatchPoint( selbrush_t *b, int ctrlIndex );

// Resolve bounds-checked winding or patch geometry. checkLive defaults safe but
// costs a display-list walk; callers with transient validated pointers may skip it.
// Sel_EdgeEnds validates kind; Sel_ItemWorldPos relies on its caller's kind branch.
bool Sel_EdgeEnds   ( const sel_item_t &it, float outA[3], float outB[3],
                      bool checkLive = true );
bool Sel_ItemWorldPos( const sel_item_t &it, float out[3], bool checkLive = true );

// ── the global selection + the pick-time mode mask ───────────────────────────
// KiwiSel() is THE accessor; it guarantees any pending legacy change has been
// folded in before the caller sees the vector (see Sel_RebuildFromLegacy below).
selection_t &KiwiSel();
sel_mask_t   KiwiSel_GetModeMask();
void         KiwiSel_SetModeMask( sel_mask_t mask );
// KIWI (2026-09-09): the "models only" sub-mode of Object mode.  While set, click and
// marquee picks in Object mode name model entities only (brushes and patches occlude
// but never select).  Entered by a double tap of 4 or the [4 Object] chip dropdown;
// a plain 4 (or any other mode key) leaves it.
bool         KiwiSel_ModelsOnly();
void         KiwiSel_SetModelsOnly( bool on );

// ── selection_t mutation (does NOT sync — call Sel_SyncToLegacy when done) ───
// Click/marquee grammar: plain replaces, Shift adds, Ctrl removes.  Ctrl wins
// Shift+Ctrl; it is not a toggle, and removing an absent item is a no-op.
void Sel_Clear   ( selection_t &sel );
bool Sel_Contains( const selection_t &sel, const sel_item_t &item );
bool Sel_Add     ( selection_t &sel, const sel_item_t &item );   // true if newly added
bool Sel_Remove  ( selection_t &sel, const sel_item_t &item );   // true if it was present
bool Sel_Toggle  ( selection_t &sel, const sel_item_t &item );   // true if now selected

// ── the two crossings ────────────────────────────────────────────────────────
// Push through legacy entry points so their invariants remain intact; suppress
// the rebuild notifications those entry points fire during synchronization.
void Sel_SyncToLegacy();

// Reconstruct KiwiSel() from the legacy globals after an invalidation.
void Sel_RebuildFromLegacy();

// O(1) hook body; rebuild before the next KiwiSel() observation.
void Sel_InvalidateFromLegacy();

// Mark Select_Deselect explicitly so rebuild drops fine-component items; an empty
// selected_brushes list is otherwise valid for edge/vertex-only selection.
void Sel_NoteLegacyDeselect();

// Bumped on every change to KiwiSel() from either direction; UI/render layers can
// use it to skip work.  Never reset.
unsigned Sel_Generation();

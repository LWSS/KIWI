#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_selection.cpp — RADIANT_UX_DESIGN §1 implementation.  See kiwi_selection.h
// for the model and the three design notes (instance-vs-def pointer, how face
// selection is really stored in this port, edge/vertex indexing).
//
// ── LEGACY HOOK SITES (all marked `// KIWI-UX` at the call site) ─────────────
// Every legacy path that can change the selection passes through one of these:
//   brush.cpp  Brush_AddToList2      — the ONLY link into selected_brushes
//   brush.cpp  Brush_RemoveFromList  — the ONLY unlink from a display list
//   select.cpp Select_Deselect       — splices selected→active directly (bypasses
//                                      the pair above)
//   select.cpp Select_Invert         — swaps both list heads directly (ditto)
//   select.cpp SelectedFaceArray::SetSize   — every face-selection size change
//                                      (::Add funnels through it)
//   select.cpp SelectedFaceArray::RemoveAt  — the one face removal that does not
//   undo.cpp   Undo_Undo / Undo_Redo  — wholesale list rebuilds; the tail hook is
//                                      belt-and-braces over the funnels above
//
// ── DEVIATION FROM THE SPEC (flagged) ────────────────────────────────────────
// §1 says the rebuild is "called at defined sync points — never lazily".  Two of
// the funnels above (Brush_AddToList2 / Brush_RemoveFromList) are per-BRUSH and
// run inside bulk loops (Select_ByClass, Select_Connected, Select_Invert,
// Map_LoadFile, …), so an eager O(selection) rebuild inside them is O(N²).  The
// hook body is therefore an O(1) invalidation and the rebuild is forced by
// KiwiSel(), the single accessor.  This is observationally identical to the eager
// form — nothing between a hook and the next KiwiSel() can read selection_t —
// while staying linear.  Sel_RebuildFromLegacy() remains callable directly for
// any site that wants the eager guarantee.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include <universal/assertive.h>
#include "qe3.h"
#include "kiwi_selection.h"

#include <string.h>

// ── legacy entry points (verified against their definitions) ────────────────
extern float world_orient_matrix[4][3];                          // entity.cpp(0x6DE290)
// KIWI-UX (CLEANUP, C-55): no local `extern selbrush_t active_brushes;` — qe3.h
// (included above) declares both display-list sentinels.

extern void Select_Deselect( int bAlsoFreeFaces );               // select.cpp 0x48E800
extern void Select_Brush( selbrush_t *brush, char some_overwrite,
                          char bStatus, char center_grid_on_selection ); // select.cpp 0x48DCC0
extern void sub_477D70( selbrush_t *b, const float *mat );       // brush.cpp  Brush_CheckBuildFaceVis
// KIWI-UX (CLEANUP, C-40): the ONE per-brush hide writer, used to re-assert a hide
// that Brush_AddToList2's `brushFlags &= ~0x1Fu` clears — see Sel_SyncToLegacy.
extern void KiwiVis_SetHidden( selbrush_t *b, bool hidden );     // kiwi_visibility.h:141 / kiwi_visibility.cpp:289
// (SetupVertexSelection, select.cpp 0x494BC0, is NO LONGER called from here —
//  see the note in Sel_SyncToLegacy pass 1.)

namespace
{
    selection_t s_selection;
    sel_mask_t  s_modeMask   = SEL_MASK_EVERYTHING;   // "5 Everything" is the start mode
    unsigned    s_generation = 1;

    // Re-entrancy + laziness state.  s_syncing suppresses the rebuild hooks while
    // Sel_SyncToLegacy drives legacy code (Select_Deselect / Select_Brush /
    // g_SelectedFaces all fire them); s_rebuilding is belt-and-braces.
    bool s_syncing    = false;
    bool s_rebuilding = false;
    bool s_dirty      = true;    // first KiwiSel() adopts whatever legacy state exists
    // KIWI-UX (shakeout D): set by Sel_NoteLegacyDeselect, consumed by the next
    // rebuild.  "The pending legacy change was a wholesale deselect, so do NOT
    // carry the edge/vertex items over it."  See kiwi_selection.h.
    bool s_deselected = false;

    // The instance's brush definition, or NULL when the node is not usable.
    brush_t *DefOf( const selbrush_t *b )
    {
        return b ? b->def : nullptr;
    }

    // A brush winding, or NULL. `faceIndex` is bounds-checked against the DEF's
    // face count (the instance's faceCount is a cache that is 0 until the first
    // camera draw — SetupVertexSelection documents the same trap).
    winding_t *WindingOf( const selbrush_t *b, int faceIndex )
    {
        brush_t *def = DefOf( b );
        if ( !def || !def->faces )
            return nullptr;
        if ( faceIndex < 0 || faceIndex >= def->faceCount )
            return nullptr;
        return def->faces[faceIndex].w;
    }

    // Patch DEF behind an instance node (b->patch->def == b->def->patch).
    patchMesh_t *PatchOf( const selbrush_t *b )
    {
        brush_t *def = DefOf( b );
        return ( b && b->patch && def ) ? def->patch : nullptr;
    }

    // Is this item still addressable in the live map data?
    // CRASH FIX (user report, 0xDDDDDDDD deref in WindingOf): the node must be
    // LIVE before ANY deref.  A cancelled/invalid edge move runs Undo_Undo, which
    // FREES the live selbrush nodes and relinks clones; the carried edge/vertex
    // items in Sel_RebuildFromLegacy then hold freed pointers, and this function
    // was the first deref on the rebuild path.  Sel_BrushLive walks the two
    // display lists comparing POINTERS only, so the check itself is safe against
    // freed memory.  Guarding here (not just at the carried loop) closes the same
    // hole for every other ItemResolves caller (Sel_SyncToLegacy passes 1 and 2).
    bool ItemResolves( const sel_item_t &it )
    {
        if ( !Sel_BrushLive( it.brush ) )
            return false;
        switch ( it.kind )
        {
        case SEL_OBJECT:
            return DefOf( it.brush ) != nullptr;

        case SEL_FACE:
        {
            brush_t *def = DefOf( it.brush );
            return def && it.faceIndex >= 0 && it.faceIndex < def->faceCount;
        }

        case SEL_EDGE:
        {
            winding_t *w = WindingOf( it.brush, it.faceIndex );
            return w && it.edgeIndex >= 0 && it.edgeIndex < w->numpoints;
        }

        case SEL_VERTEX:
            if ( it.faceIndex < 0 )                        // patch control point
            {
                patchMesh_t *p = PatchOf( it.brush );
                return p && it.vertIndex >= 0 && it.vertIndex < p->width * p->height;
            }
            else
            {
                winding_t *w = WindingOf( it.brush, it.faceIndex );
                return w && it.vertIndex >= 0 && it.vertIndex < w->numpoints;
            }

        default:
            return false;
        }
    }
}

// ─── item helpers ────────────────────────────────────────────────────────────
bool Sel_ItemEqual( const sel_item_t &a, const sel_item_t &b )
{
    return a.kind      == b.kind
        && a.brush     == b.brush
        && a.faceIndex == b.faceIndex
        && a.edgeIndex == b.edgeIndex
        && a.vertIndex == b.vertIndex;
}

// See the header: pointer comparison only, so this is safe on a freed node.
bool Sel_BrushLive( const selbrush_t *b )
{
    if ( !b )
        return false;
    for ( selbrush_t *i = active_brushes.next; i && i != &active_brushes; i = i->next )
        if ( i == b )
            return true;
    for ( selbrush_t *i = selected_brushes.next; i && i != &selected_brushes; i = i->next )
        if ( i == b )
            return true;
    return false;
}

sel_item_t Sel_MakeObject( selbrush_t *b )
{
    sel_item_t it;
    it.kind  = SEL_OBJECT;
    it.brush = b;
    return it;
}

sel_item_t Sel_MakeFace( selbrush_t *b, int faceIndex )
{
    sel_item_t it;
    it.kind      = SEL_FACE;
    it.brush     = b;
    it.faceIndex = faceIndex;
    return it;
}

sel_item_t Sel_MakeEdge( selbrush_t *b, int faceIndex, int edgeIndex )
{
    sel_item_t it;
    it.kind      = SEL_EDGE;
    it.brush     = b;
    it.faceIndex = faceIndex;
    it.edgeIndex = edgeIndex;
    return it;
}

sel_item_t Sel_MakeVertex( selbrush_t *b, int faceIndex, int vertIndex )
{
    sel_item_t it;
    it.kind      = SEL_VERTEX;
    it.brush     = b;
    it.faceIndex = faceIndex;
    it.vertIndex = vertIndex;
    return it;
}

sel_item_t Sel_MakePatchPoint( selbrush_t *b, int ctrlIndex )
{
    return Sel_MakeVertex( b, -1, ctrlIndex );
}

// ─── KIWI-UX (CLEANUP, A-14 / A-13 / C-49) — item → world geometry ───────────
// The three questions the layer used to answer in nine private copies.  See
// kiwi_selection.h for what each guard is and which copy it came from.
bool Patch_DimsSane( const patchMesh_t *pm )
{
    return pm && pm->width > 0 && pm->height > 0
        && pm->width <= KIWI_PATCH_MAX_DIM && pm->height <= KIWI_PATCH_MAX_DIM;
}

namespace
{
    // The winding behind (brush, faceIndex), with the numpoints bound the strict
    // copies carried.  `minPoints` is 2 for an edge, 1 for a vertex — the only
    // difference between the two resolutions' winding guards.
    const winding_t *StrictWinding( const sel_item_t &it, int minPoints )
    {
        if ( !it.brush || !it.brush->def )
            return nullptr;
        const brush_t *def = it.brush->def;
        if ( !def->faces || it.faceIndex < 0 || it.faceIndex >= def->faceCount )
            return nullptr;
        const winding_t *w = def->faces[it.faceIndex].w;
        if ( !w || w->numpoints < minPoints || w->numpoints > MAX_POINTS_ON_WINDING )
            return nullptr;
        return w;
    }
}

bool Sel_EdgeEnds( const sel_item_t &it, float outA[3], float outB[3], bool checkLive )
{
    if ( it.kind != SEL_EDGE )
        return false;
    if ( checkLive && !Sel_BrushLive( it.brush ) )
        return false;
    const winding_t *w = StrictWinding( it, 2 );
    if ( !w )
        return false;
    if ( it.edgeIndex < 0 || it.edgeIndex >= w->numpoints )
        return false;
    const int j = ( it.edgeIndex + 1 ) % w->numpoints;
    for ( int k = 0; k < 3; ++k )
    {
        outA[k] = w->p[it.edgeIndex][k];
        outB[k] = w->p[j][k];
    }
    return true;
}

bool Sel_ItemWorldPos( const sel_item_t &it, float out[3], bool checkLive )
{
    if ( checkLive && !Sel_BrushLive( it.brush ) )
        return false;
    if ( !it.brush || !it.brush->def )
        return false;
    if ( it.faceIndex < 0 )                          // patch control point
    {
        const patchMesh_t *pm = it.brush->def->patch;
        if ( !Patch_DimsSane( pm ) )
            return false;
        const int col = it.vertIndex / pm->height;
        const int row = it.vertIndex % pm->height;
        if ( col < 0 || col >= pm->width || row < 0 || row >= pm->height )
            return false;
        out[0] = pm->ctrl[col][row].xyz[0];
        out[1] = pm->ctrl[col][row].xyz[1];
        out[2] = pm->ctrl[col][row].xyz[2];
        return true;
    }
    const winding_t *w = StrictWinding( it, 1 );
    if ( !w || it.vertIndex < 0 || it.vertIndex >= w->numpoints )
        return false;
    out[0] = w->p[it.vertIndex][0];
    out[1] = w->p[it.vertIndex][1];
    out[2] = w->p[it.vertIndex][2];
    return true;
}

// ─── globals ─────────────────────────────────────────────────────────────────
selection_t &KiwiSel()
{
    if ( s_dirty && !s_syncing )
        Sel_RebuildFromLegacy();
    return s_selection;
}

sel_mask_t KiwiSel_GetModeMask()
{
    return s_modeMask;
}

void KiwiSel_SetModeMask( sel_mask_t mask )
{
    mask &= SEL_MASK_EVERYTHING;
    if ( !mask )
        mask = SEL_MASK_EVERYTHING;      // an empty filter would make picking dead
    if ( mask == s_modeMask )
        return;
    s_modeMask = mask;
    ++s_generation;
}

unsigned Sel_Generation()
{
    return s_generation;
}

// ─── selection_t mutation ────────────────────────────────────────────────────
void Sel_Clear( selection_t &sel )
{
    if ( sel.items.empty() && !Sel_ItemValid( sel.active ) )
        return;
    sel.items.clear();
    sel.active = sel_item_t();
    ++s_generation;
}

bool Sel_Contains( const selection_t &sel, const sel_item_t &item )
{
    for ( size_t i = 0; i < sel.items.size(); ++i )
        if ( Sel_ItemEqual( sel.items[i], item ) )
            return true;
    return false;
}

bool Sel_Add( selection_t &sel, const sel_item_t &item )
{
    if ( !Sel_ItemValid( item ) )
        return false;
    sel.active = item;
    if ( Sel_Contains( sel, item ) )
    {
        ++s_generation;                  // active moved even though the set did not
        return false;
    }
    sel.items.push_back( item );
    ++s_generation;
    return true;
}

bool Sel_Remove( selection_t &sel, const sel_item_t &item )
{
    for ( size_t i = 0; i < sel.items.size(); ++i )
    {
        if ( !Sel_ItemEqual( sel.items[i], item ) )
            continue;
        sel.items.erase( sel.items.begin() + (int)i );
        if ( Sel_ItemEqual( sel.active, item ) )
            sel.active = sel.items.empty() ? sel_item_t() : sel.items.back();
        ++s_generation;
        return true;
    }
    return false;
}

bool Sel_Toggle( selection_t &sel, const sel_item_t &item )
{
    if ( Sel_Remove( sel, item ) )
        return false;
    Sel_Add( sel, item );
    return true;
}

// ─── selection_t → legacy ────────────────────────────────────────────────────
// Order matters: Select_Brush asserts `g_SelectedFaces.GetSize() == 0 || patch`
// (select.cpp:426), so the whole-brush selection MUST be pushed before any face
// selection.  Select_Deselect(1) first, so every brush is back on active_brushes
// (Brush_RemoveFromList requires a linked node) and the face array is empty.
void Sel_SyncToLegacy()
{
    if ( s_syncing )
        return;
    s_syncing = true;

    Select_Deselect( 1 );

    // ── pass 1: whole-brush selection ────────────────────────────────────────
    // ONLY SEL_OBJECT selects its brush.
    //
    // SHAKEOUT D — THE PROMOTION IS GONE.  USER DIRECTIVE: "When selecting an
    // edge, the entire solid (Brush in this case) should not get selected as
    // well, only the edge/s."  SEL_VERTEX / SEL_EDGE used to promote their owner
    // brush into `selected_brushes` so the ported vertex-edit handles
    // (d_points / d_edges, rebuilt by SetupVertexSelection from the SELECTED
    // brushes) had something to work from — which also made the whole brush
    // light up red, which is what the user is objecting to.  SEL_FACE never
    // promoted (a face selection and a brush selection are mutually exclusive in
    // the legacy model — sub_48E170 converts one into the other), and now the
    // fine kinds behave the same way.
    //
    // WHAT THAT COSTS, checked consumer by consumer before cutting it:
    //   * kiwi_transform's BeginEdges / BeginVerts read KiwiSel().items DIRECTLY
    //     and cover their brushes with UndoCoverBrush in OpenUndoForBrushes
    //     (kiwi_transform.cpp) — they never depended on Undo_AddBrushList seeing
    //     the brush, exactly as the FACE path already did not.
    //   * kiwi_bevel's edge path is the same shape (its own undo cover, items
    //     read directly).
    //   * SetupVertexSelection is no longer called from here at all: it walks
    //     `selected_brushes` (select.cpp:4595) and would now always produce an
    //     EMPTY handle list.  Its only consumers are the legacy handle draw and
    //     the legacy SelectVertexByRay / MoveSelection drag, none of which the
    //     modern layer uses — the fine kinds are drawn by the shakeout-D accent
    //     pass in kiwi_hover.cpp instead.
    //   * KiwiXform_CanRotate / CanScale test `selected_brushes` (they are
    //     whole-object ops), so R and S now correctly REFUSE on a pure
    //     edge/vertex selection instead of silently rotating the whole brush.
    //   * Sel_RebuildFromLegacy's carry rule had to change with it — see there.
    std::vector<selbrush_t *> brushes;

    for ( size_t i = 0; i < s_selection.items.size(); ++i )
    {
        const sel_item_t &it = s_selection.items[i];
        if ( it.kind != SEL_OBJECT || !ItemResolves( it ) )
            continue;

        bool dup = false;
        for ( size_t k = 0; k < brushes.size() && !dup; ++k )
            dup = ( brushes[k] == it.brush );
        if ( !dup )
            brushes.push_back( it.brush );
    }

    // ── KIWI-UX (CLEANUP, C-40): SELECTING A BRUSH MUST NOT UN-HIDE IT ────────
    // Select_Brush -> Brush_AddToList2 ends with `b->brushFlags &= ~0x1Fu`
    // (brush.cpp:940), which clears bits 0..4 — and the hide bit is VALUE 4
    // (bit index 2: select.cpp:4168 `|= 4u`, KVIS_HIDDEN_BIT), inside that mask.  It does
    // not clear `xx5`, so the brush landed bit-clear/depth-1, a pair
    // KiwiVis_SetHidden never produces, and nothing re-asserted it afterwards:
    // one outliner click on a hidden brush destroyed the hide that
    // KiwiVis_SidecarLoadApply had just restored.  The ported mask is FAITHFUL
    // and is not touched; the state is re-applied here instead, through the one
    // writer, AFTER the sync (before it, and Brush_AddToList2 would stomp it).
    // Only the brushes being ADDED can lose it — Select_Deselect / the deselect
    // helper leave brushFlags bit 4 alone.
    std::vector<char> wasHidden( brushes.size(), 0 );
    for ( size_t i = 0; i < brushes.size(); ++i )
        wasHidden[i] = ( ( (unsigned)brushes[i]->brushFlags & 4u ) != 0 ) ? 1 : 0;

    for ( size_t i = 0; i < brushes.size(); ++i )
        Select_Brush( brushes[i], 0, 0, 0 );   // 0 = this brush only, never the entity group

    for ( size_t i = 0; i < brushes.size(); ++i )
        if ( wasHidden[i] )
            KiwiVis_SetHidden( brushes[i], true );

    // ── pass 2: face selection ───────────────────────────────────────────────
    // The selface_t the legacy ops read wants the INSTANCE faceVis_s pointer, so
    // rebuild the instance's face array first (idempotent + version-gated) and
    // derive `face` from it — never cache that pointer in sel_item_t.
    for ( size_t i = 0; i < s_selection.items.size(); ++i )
    {
        const sel_item_t &it = s_selection.items[i];
        if ( it.kind != SEL_FACE || !ItemResolves( it ) )
            continue;
        if ( it.brush->patch )                 // patches have no selectable faces
            continue;

        sub_477D70( it.brush, (const float *)world_orient_matrix );
        if ( !it.brush->faces || it.faceIndex >= it.brush->faceCount )
            continue;

        faceVis_s *fv = &it.brush->faces[it.faceIndex];
        int  sz    = g_SelectedFaces.GetSize();
        bool found = false;
        for ( int k = 0; k < sz && !found; ++k )
            found = ( g_SelectedFaces.GetAt( k ).face == fv );
        if ( found )
            continue;

        selface_t sf;
        sf.brush = it.brush;
        sf.face  = fv;
        sf.index = it.faceIndex;
        g_SelectedFaces.Add( sf );
    }

    s_syncing    = false;
    s_dirty      = false;       // selection_t is the truth; do not re-adopt legacy
    s_deselected = false;       // …and our own Select_Deselect above was not a user deselect
    ++s_generation;
}

// ─── legacy → selection_t ────────────────────────────────────────────────────
// SEL_OBJECT comes from the selected_brushes sentinel list, SEL_FACE from
// g_SelectedFaces.  SEL_VERTEX/SEL_EDGE have no legacy source (d_move_points
// holds raw float* into the deduped d_points scratch, which cannot be mapped back
// to a (brush, face, vert) triple), so surviving vertex/edge items are CARRIED
// OVER when their owning brush is still selected and their indices still resolve.
void Sel_RebuildFromLegacy()
{
    if ( s_syncing || s_rebuilding )
        return;
    s_rebuilding = true;

    const sel_item_t prevActive = s_selection.active;

    std::vector<sel_item_t> carried;
    for ( size_t i = 0; i < s_selection.items.size(); ++i )
    {
        const sel_item_t &it = s_selection.items[i];
        if ( ( it.kind == SEL_VERTEX || it.kind == SEL_EDGE ) && ItemResolves( it ) )
            carried.push_back( it );
    }

    s_selection.items.clear();

    // Sentinel walk: init from .next, advance via ->next (never mix in ->prev).
    for ( selbrush_t *b = selected_brushes.next; b && b != &selected_brushes; b = b->next )
        s_selection.items.push_back( Sel_MakeObject( b ) );

    const int nFaces = g_SelectedFaces.GetSize();
    for ( int i = 0; i < nFaces; ++i )
    {
        const selface_t &sf = g_SelectedFaces.GetAt( i );
        if ( !sf.brush )
            continue;
        s_selection.items.push_back( Sel_MakeFace( sf.brush, sf.index ) );
    }

    // Carry the new-model-only kinds.
    //
    // SHAKEOUT D: the condition WAS "the owning brush is still on
    // selected_brushes", which only ever held because pass 1 of the sync PROMOTED
    // those owners.  With the promotion removed (see there) an edge-only
    // selection leaves the legacy lists empty, and that test would have thrown
    // the selection away on the very next legacy change.  The rule is now
    // "the item still resolves" — ItemResolves already carries the Sel_BrushLive
    // guard, so a freed node can never be carried — and the ONE case that must
    // still drop them, an explicit user deselect, announces itself through
    // Sel_NoteLegacyDeselect rather than being inferred from an empty list.
    if ( !s_deselected )
        for ( size_t i = 0; i < carried.size(); ++i )
            s_selection.items.push_back( carried[i] );

    // Keep `active` if it survived; otherwise fall back to the last item.
    s_selection.active = sel_item_t();
    if ( Sel_ItemValid( prevActive ) )
    {
        for ( size_t i = 0; i < s_selection.items.size(); ++i )
            if ( Sel_ItemEqual( s_selection.items[i], prevActive ) )
            {
                s_selection.active = prevActive;
                break;
            }
    }
    if ( !Sel_ItemValid( s_selection.active ) && !s_selection.items.empty() )
        s_selection.active = s_selection.items.back();

    s_dirty      = false;
    s_deselected = false;       // consumed
    s_rebuilding = false;
    ++s_generation;
}

void Sel_InvalidateFromLegacy()
{
    if ( s_syncing )
        return;                 // our own legacy drive — selection_t already knows
    s_dirty = true;
}

void Sel_NoteLegacyDeselect()
{
    if ( s_syncing )
        return;                 // Sel_SyncToLegacy's own Select_Deselect(1)
    s_dirty      = true;
    s_deselected = true;
}

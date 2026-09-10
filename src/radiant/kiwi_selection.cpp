#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Typed selection and legacy-selection adapter; see kiwi_selection.h for indexing.
// Legacy list, face-array, and undo hooks only invalidate: eager rebuilds from the
// per-brush hooks would make bulk selection O(N²). KiwiSel() rebuilds before use.

#include "stdafx.h"
#include <universal/assertive.h>
#include "qe3.h"
#include "kiwi_refimage.h"
#include "kiwi_selection.h"

#include <string.h>

// ── legacy entry points (verified against their definitions) ────────────────
extern float world_orient_matrix[4][3];                          // entity.cpp(0x6DE290)

extern void Select_Deselect( int bAlsoFreeFaces );               // select.cpp 0x48E800
extern void Select_Brush( selbrush_t *brush, char some_overwrite,
                          char bStatus, char center_grid_on_selection ); // select.cpp 0x48DCC0
extern void sub_477D70( selbrush_t *b, const float *mat );       // brush.cpp  Brush_CheckBuildFaceVis
// Re-applies hidden state that Brush_AddToList2 clears; see Sel_SyncToLegacy.
extern void KiwiVis_SetHidden( selbrush_t *b, bool hidden );     // kiwi_visibility.h:141 / kiwi_visibility.cpp:289

namespace
{
    selection_t s_selection;
    sel_mask_t  s_modeMask   = SEL_MASK_EVERYTHING;   // "5 Everything" is the start mode
    bool        s_modelsOnly = false;                 // Object mode restricted to models (KIWI 2026-09-09)
    unsigned    s_generation = 1;

    // Suppress notifications driven by our own sync and guard rebuild recursion.
    bool s_syncing    = false;
    bool s_rebuilding = false;
    bool s_dirty      = true;    // first KiwiSel() adopts whatever legacy state exists
    // Explicit legacy deselects suppress edge/vertex carry on the next rebuild.
    bool s_deselected = false;

    // The instance's brush definition, or NULL when the node is not usable.
    brush_t *DefOf( const selbrush_t *b )
    {
        return b ? b->def : nullptr;
    }

    // Use the definition's face count; the instance cache can remain zero until draw.
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

    // Undo may replace cached nodes, so liveness must be checked before any dereference.
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

// ─── item → world geometry ───────────────────────────────────────────────────
bool Patch_DimsSane( const patchMesh_t *pm )
{
    return pm && pm->width > 0 && pm->height > 0
        && pm->width <= KIWI_PATCH_MAX_DIM && pm->height <= KIWI_PATCH_MAX_DIM;
}

namespace
{
    // minPoints distinguishes edge (2) from vertex (1) resolution.
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
    if ( mask != SEL_MASK_OBJECT && s_modelsOnly )
    {
        s_modelsOnly = false;            // the sub-mode belongs to Object mode alone
        ++s_generation;
    }
    if ( mask == s_modeMask )
        return;
    s_modeMask = mask;
    ++s_generation;
}

bool KiwiSel_ModelsOnly()
{
    return s_modelsOnly;
}

void KiwiSel_SetModelsOnly( bool on )
{
    if ( s_modelsOnly == on )
        return;
    s_modelsOnly = on;
    ++s_generation;
}

unsigned Sel_Generation()
{
    return s_generation;
}

// ─── selection_t mutation ────────────────────────────────────────────────────
void Sel_Clear( selection_t &sel )
{
    // KIWI (REFIMG2): every typed full-clear is also a reference-image clear.
    KiwiRefImage_Select( -1 );
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
// Select_Brush requires an empty face array, so whole brushes precede faces.
// Select_Deselect(1) also relinks brushes before Select_Brush removes them.
void Sel_SyncToLegacy()
{
    if ( s_syncing )
        return;
    s_syncing = true;

    Select_Deselect( 1 );

    // ── pass 1: whole-brush selection ────────────────────────────────────────
    // Only objects enter selected_brushes; promoting fine-component owners would
    // render and operate on them as whole brushes. Fine transforms consume KiwiSel().
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

    // Brush_AddToList2 clears bits 0..4, including hidden value 4. Restore hidden
    // state through the canonical writer after Select_Brush has cleared the mask.
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
// Objects and faces have legacy sources. Edge/vertex identity does not round-trip,
// so live fine-component items carry unless an explicit deselect drops them.
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

    // Empty legacy lists are valid for fine selection; only explicit deselect drops it.
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
    // KIWI (REFIMG2): legacy XY/Esc deselects enter through this one adapter.
    KiwiRefImage_Select( -1 );
    s_dirty      = true;
    s_deselected = true;
}

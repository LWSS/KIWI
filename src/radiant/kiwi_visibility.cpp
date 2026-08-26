#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// KIWI hide/isolate extensions. Visibility writes only brushFlags bit 2 and xx5.

#include "stdafx.h"
#include "qe3.h"
// Patch-gate diagnostics read preferences, filter lists, and selection mode.
#include "prefs.h"          // g_PrefsDlg->m_bSelectCurves (prefs.h:86)

#include "kiwi_command.h"
#include "kiwi_selection.h" // KiwiSel_GetModeMask / SEL_MASK_FACE
#include "kiwi_undo.h"      // KiwiUndo_NoteVisibilityRecord
#include "kiwi_visibility.h"
#include "kiwi_lightcache.h"

#include <string.h>         // _strnicmp
#include <vector>
#include <algorithm>        // std::sort / std::lower_bound
#include <utility>          // std::pair

// 0x25d5a74 g_nUpdateBits; 0x23f1864 selected_brushes; 0x23f189c active_brushes.
// 0x23F17A0 entities; keep its declaration at file scope for external linkage.
extern int      Sys_Printf( const char *fmt, ... );
extern int      g_nUpdateBits;
extern bool     Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );
extern entity_s entities;

namespace
{
    // Matches Select_Hide and ShowHidden's brushFlags bit.
    const unsigned KVIS_HIDDEN_BIT = 4u;

    inline bool Hidden( const selbrush_t *b )
    {
        return ( (unsigned)b->brushFlags & KVIS_HIDDEN_BIT ) != 0;
    }

    // Keep the selected and active list passes on one implementation.
    void InvertOne( selbrush_t *b, int *nowHidden, int *nowShown )
    {
        if ( ( (unsigned)b->brushFlags & KVIS_HIDDEN_BIT ) != 0 )
        {
            // Match ShowHidden's per-brush body.
            b->brushFlags &= ~(int)KVIS_HIDDEN_BIT;
            b->xx5         = 0;
            ++( *nowShown );
        }
        else
        {
            // Select_Hide also resets hidden brushes to depth 1.
            b->brushFlags |= (int)KVIS_HIDDEN_BIT;
            b->xx5         = 1;
            ++( *nowHidden );
        }
    }

    // Visibility undo mirrors the construction snapshot domain.
    struct visEntry_t
    {
        brush_t *def;       // stable across instance recreation, but not unique
        bool     hidden;
        int      depth;     // selbrush_t::xx5
    };
    typedef std::vector<visEntry_t> visSnap_t;

    // Match the construction store's bounded undo depth.
    const int KVIS_UNDO_DEPTH = 32;

    std::vector<visSnap_t> s_visUndo;
    std::vector<visSnap_t> s_visRedo;

    void Snapshot( visSnap_t *out )
    {
        out->clear();
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        {
            visEntry_t e;
            e.def    = b->def;
            e.hidden = Hidden( b );
            e.depth  = b->xx5;
            out->push_back( e );
        }
        for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
        {
            visEntry_t e;
            e.def    = b->def;
            e.hidden = Hidden( b );
            e.depth  = b->xx5;
            out->push_back( e );
        }
    }

    // Restore only bit 2 and xx5; other brushFlags bits belong to filters/layers.
    void Restore( const visSnap_t &snap )
    {
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass ? &active_brushes : &selected_brushes;
            for ( selbrush_t *b = head->next; b != head; b = b->next )
            {
                for ( size_t i = 0; i < snap.size(); ++i )
                {
                    if ( snap[i].def != b->def )
                        continue;
                    if ( snap[i].hidden )
                        b->brushFlags |= (int)KVIS_HIDDEN_BIT;
                    else
                        b->brushFlags &= ~(int)KVIS_HIDDEN_BIT;
                    b->xx5 = snap[i].depth;
                    break;
                }
            }
        }
        KiwiLightCache_VisibilityChanged();
        g_nUpdateBits = -1;
    }

    bool SameSnap( const visSnap_t &a, const visSnap_t &b )
    {
        if ( a.size() != b.size() )
            return false;
        for ( size_t i = 0; i < a.size(); ++i )
            if ( a[i].def != b[i].def || a[i].hidden != b[i].hidden
              || a[i].depth != b[i].depth )
                return false;
        return true;
    }
}

// Compare pre/post snapshots so no-op gestures add no undo ticket. Committing at
// the edit preserves journal order; later pushes and stack reads flush a missed Commit.
namespace
{
    visSnap_t   s_visPending;
    const char *s_visPendingLabel = 0;
    bool        s_visPendingHave  = false;

    // Commit only captures that differ from the current visibility state.
    void FlushPendingVisRecord()
    {
        if ( !s_visPendingHave )
            return;
        s_visPendingHave = false;

        visSnap_t now;
        Snapshot( &now );
        if ( SameSnap( s_visPending, now ) )
        {
            s_visPending.clear();
            return;                       // no visibility change
        }

        s_visUndo.push_back( visSnap_t() );
        s_visUndo.back().swap( s_visPending );
        s_visPending.clear();
        if ( (int)s_visUndo.size() > KVIS_UNDO_DEPTH )
            s_visUndo.erase( s_visUndo.begin() );

        s_visRedo.clear();
        KiwiUndo_NoteVisibilityRecord( s_visPendingLabel ? s_visPendingLabel : "hide" );
    }
}

// Capture before the gesture; pair with KiwiVis_UndoCommit after its last write.
void KiwiVis_UndoPush( const char *label )
{
    FlushPendingVisRecord();              // finish a caller's outstanding capture

    Snapshot( &s_visPending );
    s_visPendingLabel = label;            // caller guarantees static lifetime
    s_visPendingHave  = true;
}

// Idempotent; discard the capture when the gesture changed nothing.
void KiwiVis_UndoCommit()
{
    FlushPendingVisRecord();
}

bool KiwiVis_UndoPop()
{
    FlushPendingVisRecord();
    if ( s_visUndo.empty() )
        return false;

    // Preserve the current state for redo before restoring undo.
    s_visRedo.push_back( visSnap_t() );
    Snapshot( &s_visRedo.back() );
    if ( (int)s_visRedo.size() > KVIS_UNDO_DEPTH )
        s_visRedo.erase( s_visRedo.begin() );

    Restore( s_visUndo.back() );
    s_visUndo.pop_back();
    return true;
}

bool KiwiVis_RedoPop()
{
    FlushPendingVisRecord();              // finish any outstanding capture
    if ( s_visRedo.empty() )
        return false;

    s_visUndo.push_back( visSnap_t() );
    Snapshot( &s_visUndo.back() );
    if ( (int)s_visUndo.size() > KVIS_UNDO_DEPTH )
        s_visUndo.erase( s_visUndo.begin() );

    Restore( s_visRedo.back() );
    s_visRedo.pop_back();
    return true;
}

void KiwiVis_ClearRedo() { s_visRedo.clear(); }

void KiwiVis_UndoReset()
{
    // A pending capture belongs to the discarded map; do not commit it.
    s_visPendingHave = false;
    s_visPending.clear();
    s_visUndo.clear();
    s_visRedo.clear();
}

// Match Select_Hide/ShowHidden: write only the hidden bit and depth.
void KiwiVis_SetHidden( selbrush_t *b, bool hidden )
{
    if ( !b )
        return;
    if ( hidden )
    {
        b->brushFlags |= (int)KVIS_HIDDEN_BIT;
        b->xx5 = 1;
    }
    else
    {
        b->brushFlags &= ~(int)KVIS_HIDDEN_BIT;
        b->xx5 = 0;
    }
    KiwiLightCache_VisibilityChanged();
    g_nUpdateBits = -1;
}

// KIWI-owned fourth hide command: invert hidden.
void KiwiVis_InvertHidden()
{
    int nowHidden = 0, nowShown = 0;

    // One undo record covers the whole gesture.
    KiwiVis_UndoPush( "invert hidden" );

    // Match Select_Hide's selected-then-active order.
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        InvertOne( b, &nowHidden, &nowShown );
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
        InvertOne( b, &nowHidden, &nowShown );

    KiwiLightCache_VisibilityChanged();
    KiwiVis_UndoCommit();      // compare post-state and commit if changed

    // Match the ported hide handlers' invalidation.
    g_nUpdateBits = -1;
    Sys_Printf( "Invert hidden: %i now hidden, %i shown.\n", nowHidden, nowShown );
}

// Palette predicates.
bool KiwiVis_CanHide()
{
    // Hide and isolate both no-op on an empty selection.
    return selected_brushes.next != &selected_brushes;
}

bool KiwiVis_HasHidden()
{
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        if ( Hidden( b ) )
            return true;
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
        if ( Hidden( b ) )
            return true;
    return false;
}

bool KiwiVis_CanInvert()
{
    return selected_brushes.next != &selected_brushes
        || active_brushes.next   != &active_brushes;
}

// Hidden-solid sidecar persistence.
namespace
{
    // Cap hostile sidecars far above legitimate map sizes.
    const int KVIS_SIDECAR_MAX_HIDDEN = 262144;

    // Definition pointers are not unique across prefab instances; scan each equal run.
    typedef std::pair<brush_t *, selbrush_t *> defInst_t;

    std::vector<brush_t *> s_saveOrder;        // the enumeration, ordinal == index
    std::vector<int>       s_hiddenOrdinals;   // which of those are hidden (save side)
    std::vector<int>       s_pendingHidden;    // ordinals read back from a sidecar
    int                    s_pendingTotal = -1;// its `hiddenbrushtotal`, -1 = absent

    // Sort live instances by definition for per-ordinal binary search during save.
    void BuildDefIndex( std::vector<defInst_t> *out )
    {
        out->clear();
        // Traverse selected then active; sorting makes this order immaterial.
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass ? &active_brushes : &selected_brushes;
            for ( selbrush_t *b = head->next; b != head; b = b->next )
                out->push_back( defInst_t( b->def, b ) );
        }
        std::sort( out->begin(), out->end() );
    }

    // Must mirror Map_SaveFile's entity and definition order; ordinals depend on it.
    void BuildSaveOrder( std::vector<brush_t *> *out )
    {
        out->clear();
        // Mirror Map_SaveFile's entity loop and write gate.
        for ( entity_s *eIter = entities.next; eIter != &entities; eIter = eIter->next )
        {
            entity_s_def *eDef = (entity_s_def *)eIter;
            // Map_SaveFile dereferences eclass at 0x486f59; avoid another crash path here.
            if ( !eDef->eclass )
                continue;
            const bool hasDefBrushes = ( (void *)eDef->brushes.prev != (void *)&eDef->def );
            const bool isWorld       = ( strcmp( eDef->eclass->name, "worldspawn" ) == 0 );
            if ( !hasDefBrushes && !isWorld )
                continue;                       // omitted by Map_SaveFile

            // `brushes.prev`/`onext` is insertion order, matching file order.
            // Deliberate divergence: include each fixed-size bbox although it is not
            // written; ParseEntity recreates one per entity in the same order.
            brush_t *sentinel = (brush_t *)&eDef->def;
            for ( brush_t *b = (brush_t *)eDef->brushes.prev; b != sentinel; b = b->onext )
                out->push_back( b );
        }
    }

    // Return the first equal definition, or idx.size() when no instance is live.
    size_t DefRunBegin( const std::vector<defInst_t> &idx, brush_t *def )
    {
        const defInst_t key( def, (selbrush_t *)0 );   // null second key starts the run
        return (size_t)( std::lower_bound( idx.begin(), idx.end(), key ) - idx.begin() );
    }
}

int KiwiVis_SidecarBuild()
{
    BuildSaveOrder( &s_saveOrder );
    s_hiddenOrdinals.clear();

    std::vector<defInst_t> idx;
    BuildDefIndex( &idx );

    for ( size_t i = 0; i < s_saveOrder.size(); ++i )
    {
        brush_t *def = s_saveOrder[i];
        // One ordinal represents every instance of a definition; preserve a hide if
        // any instance is hidden.
        for ( size_t k = DefRunBegin( idx, def ); k < idx.size() && idx[k].first == def; ++k )
        {
            if ( Hidden( idx[k].second ) )
            {
                s_hiddenOrdinals.push_back( (int)i );
                break;
            }
        }
    }
    return (int)s_hiddenOrdinals.size();
}

int KiwiVis_SidecarTotal()
{
    return (int)s_saveOrder.size();
}

int KiwiVis_SidecarOrdinal( int i )
{
    if ( i < 0 || i >= (int)s_hiddenOrdinals.size() )
        return -1;
    return s_hiddenOrdinals[i];
}

void KiwiVis_SidecarLoadBegin()
{
    s_pendingHidden.clear();
    s_pendingTotal = -1;
}

void KiwiVis_SidecarLoadTotal( int total )
{
    s_pendingTotal = total;
}

void KiwiVis_SidecarLoadNote( int ordinal )
{
    if ( ordinal < 0 )
        return;                                          // malformed line is non-fatal
    if ( (int)s_pendingHidden.size() >= KVIS_SIDECAR_MAX_HIDDEN )
        return;
    s_pendingHidden.push_back( ordinal );
}

void KiwiVis_SidecarLoadApply()
{
    if ( s_pendingHidden.empty() )
    {
        s_pendingTotal = -1;
        return;                                          // normal case: nothing hidden
    }

    // Re-run the exact save-side enumeration against live instances.
    BuildSaveOrder( &s_saveOrder );
    const int total = (int)s_saveOrder.size();

    // A total mismatch proves the ordinals stale, so drop the whole set. Equal-count
    // edits or reordering remain an unavoidable limitation of positional identity.
    if ( s_pendingTotal >= 0 && s_pendingTotal != total )
    {
        Sys_Printf( "WARNING: sidecar hidden-brush list is stale (%i brushes recorded, %i in the map) "
                    "— hidden state not restored.\n", s_pendingTotal, total );
        s_pendingHidden.clear();
        s_pendingTotal = -1;
        return;
    }

    std::vector<defInst_t> idx;
    BuildDefIndex( &idx );

    int applied = 0;
    int skipped = 0;
    for ( size_t i = 0; i < s_pendingHidden.size(); ++i )
    {
        const int o = s_pendingHidden[i];
        if ( o < 0 || o >= total )
        {
            ++skipped;                                   // ignore invalid ordinal
            continue;
        }
        brush_t *def   = s_saveOrder[o];
        bool     found = false;
        for ( size_t k = DefRunBegin( idx, def ); k < idx.size() && idx[k].first == def; ++k )
        {
            // Use the shared hidden-bit/depth writer.
            KiwiVis_SetHidden( idx[k].second, true );
            found = true;
        }
        if ( found ) ++applied;
        else         ++skipped;                          // definition has no live instance
    }

    s_pendingHidden.clear();
    s_pendingTotal = -1;

    // Loaded visibility is initial state, not an undoable user gesture.
    g_nUpdateBits = -1;
    if ( applied )
        Sys_Printf( "Restored %i hidden brush(es) from the sidecar.\n", applied );
    if ( skipped )
        Sys_Printf( "WARNING: %i hidden-brush record(s) did not resolve and were ignored "
                    "(Unhide All clears any wrong hide).\n", skipped );
}

// Read-only diagnostics for silent patch-selection gates.
namespace
{
    bool AnyPatchInMap()
    {
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            if ( b->patch )
                return true;
        for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
            if ( b->patch )
                return true;
        return false;
    }

    // Filter conditions have no accessor, so infer curve/terrain gates from the
    // displayed filter name. This diagnostic is advisory, not exhaustive.
    const char *HiddenCurveFilter()
    {
        filter_entry_s *heads[4] = {
            g_qeglobals.d_filterGlobals_geometryFilters,
            g_qeglobals.d_filterGlobals_triggerFilters,
            g_qeglobals.d_filterGlobals_entityFilters,
            g_qeglobals.d_filterGlobals_otherFilters,
        };
        for ( int h = 0; h < 4; ++h )
            for ( filter_entry_s *f = heads[h]; f; f = f->next_filter )
            {
                if ( f->isShown || !f->name )
                    continue;
                // _stricmp has no substring form; scan both spellings in place.
                for ( const char *s = f->name; *s; ++s )
                    if ( ( ( s[0] == 'c' || s[0] == 'C' ) && !_strnicmp( s, "curve",   5 ) )
                      || ( ( s[0] == 't' || s[0] == 'T' ) && !_strnicmp( s, "terrain", 7 ) ) )
                        return f->name;
            }
        return 0;
    }

    bool s_reportedPatchGates = false;
}

void KiwiVis_ReportPatchGates( bool force )
{
    if ( !force )
    {
        if ( s_reportedPatchGates || !AnyPatchInMap() )
            return;
        s_reportedPatchGates = true;
    }

    const bool  curvesOff = ( g_PrefsDlg && g_PrefsDlg->m_bSelectCurves == 0 );
    const char *filtered  = HiddenCurveFilter();
    const bool  faceMode  = ( KiwiSel_GetModeMask() == SEL_MASK_FACE );

    if ( !curvesOff && !filtered && !faceMode )
    {
        if ( force )
            Sys_Printf( "Patches: nothing is blocking patch selection — 'Don't select "
                        "curves' is OFF, no curve/terrain filter is hidden, and the "
                        "selection mode resolves whole objects.  A patch that still "
                        "will not pick has no curveDef (pmesh.cpp PMESH_51 needs one).\n" );
        return;
    }

    Sys_Printf( "Patches: q3 curves are currently HARD TO SELECT —%s%s%s\n",
                curvesOff ? "  'Don't select curves' is ON (toolbar / command 32852)."
                          : "",
                filtered  ? "  a filter is hiding them (untick it in Filters)."
                          : "",
                faceMode  ? "  selection mode 3 (Face) has no face on a patch — press "
                            "4 for Object or 5 for Everything."
                          : "" );
    if ( filtered )
        Sys_Printf( "Patches: the filter is \"%s\".\n", filtered );
}

// Commands.
void KiwiVis_RegisterCommands()
{
    // Classic leaves this unbound; the modern profile maps Ctrl+H.
    Radiant_RegisterCommand( "KiwiInvertHidden", 0, 0, KIWI_CMD_HIDE_INVERT );
}

bool KiwiVis_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned)KIWI_CMD_HIDE_INVERT )
        return false;
    KiwiVis_InvertHidden();
    // Forced mode also prints the patch-gate all-clear diagnostic.
    KiwiVis_ReportPatchGates( true );
    return true;
}

#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_visibility.cpp — ROUND J: the hide / isolate family's KIWI half.  See
// kiwi_visibility.h for what Plasticity binds, which three quarters of the family
// were already ported, and why Invert Hidden takes no undo bracket.
//
// NEW code.  It touches exactly the two selbrush_t fields the ported hide cores
// touch (`brushFlags` bit 2 and the hide depth `xx5`) and nothing else — no
// geometry, no entity, no list surgery.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
// ROUND AF, ITEM 9 — the patch-gate diagnostic reads three pieces of state it does
// not own: the "Don't select curves" pref, the filter category lists (both through
// qe3.h's globals) and the current selection mode.
#include "prefs.h"          // g_PrefsDlg->m_bSelectCurves (prefs.h:86)

#include "kiwi_command.h"
#include "kiwi_selection.h" // KiwiSel_GetModeMask / SEL_MASK_FACE
#include "kiwi_undo.h"      // ROUND AG, ITEM 7 — KiwiUndo_NoteVisibilityRecord
#include "kiwi_visibility.h"

#include <string.h>         // _strnicmp
#include <vector>
#include <algorithm>        // ROUND AO — std::sort / std::lower_bound over the def index
#include <utility>          // ROUND AO — std::pair

// ── ported entry points (each verified against its DEFINITION) ─────────────
//   win_qe3.cpp:112        int  Sys_Printf( const char *fmt, ... )
//   engine_stubs.cpp:693   int  g_nUpdateBits = 0;   // 0x25d5a74
//   qe3.h:1054             selbrush_t selected_brushes;   // 0x23f1864
//   qe3.h:1053             selbrush_t active_brushes;     // 0x23f189c
//   mainfrm.cpp:1251       bool Radiant_RegisterCommand( const char *name, byte vk,
//                                                        byte mods, int commandId )
//   entity.cpp:282         entity_s entities{};   // 0x23F17A0 — the entity-DEF list
//                          sentinel.  ROUND AO walks it to reproduce Map_SaveFile's
//                          enumeration; declared at FILE SCOPE (never inside the
//                          anonymous namespace — that is the round-AI link failure).
extern int      Sys_Printf( const char *fmt, ... );
extern int      g_nUpdateBits;
extern bool     Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );
extern entity_s entities;

namespace
{
    // The hidden bit, spelled exactly as the three ported cores spell it
    // (select.cpp:4168 `b->brushFlags |= 4u`, :4249 `b->brushFlags &= ~4u`).
    const unsigned KVIS_HIDDEN_BIT = 4u;

    inline bool Hidden( const selbrush_t *b )
    {
        return ( (unsigned)b->brushFlags & KVIS_HIDDEN_BIT ) != 0;
    }

    // Flip one brush's visibility.  Split out so the two list walks below cannot
    // drift apart — which is exactly the failure the ported cores' three near-copy
    // passes invite.
    void InvertOne( selbrush_t *b, int *nowHidden, int *nowShown )
    {
        if ( ( (unsigned)b->brushFlags & KVIS_HIDDEN_BIT ) != 0 )
        {
            // Exactly ShowHidden's per-brush body (select.cpp:4249-4250).
            b->brushFlags &= ~(int)KVIS_HIDDEN_BIT;
            b->xx5         = 0;
            ++( *nowShown );
        }
        else
        {
            // Exactly Select_Hide's THIRD pass (select.cpp:4179-4180), including
            // the unconditional depth 1 — see the "flattens the depth" note in
            // kiwi_visibility.h.
            b->brushFlags |= (int)KVIS_HIDDEN_BIT;
            b->xx5         = 1;
            ++( *nowHidden );
        }
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  ROUND AG, ITEM 7 — the hide snapshot store (kiwi_visibility.h THE STORE)
    // ═════════════════════════════════════════════════════════════════════════
    // Shaped one-for-one on kiwi_construct.cpp's construction store
    // (KiwiCon_UndoPush :1082 / UndoPop :1096 / RedoPop :1121 / ClearRedo :1136),
    // because that is the domain in this codebase that already solved "journal a
    // non-geometry state change" and a second shape would drift from it.
    struct visEntry_t
    {
        brush_t *def;       // the STABLE key — see kiwi_undo.h KUNDO_VISIBILITY
        bool     hidden;
        int      depth;     // selbrush_t::xx5
    };
    typedef std::vector<visEntry_t> visSnap_t;

    // The same depth the construction store uses (KCON_UNDO_DEPTH is 32); a hide
    // snapshot is three words per brush, so even a 4000-brush map is ~48 KB per
    // record and 1.5 MB for a full stack.  That is the same order as one
    // construction snapshot and it is bounded.
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

    // Write a snapshot back.  ONLY bit 2 and xx5 — kiwi_visibility.h WHAT IT
    // RESTORES says why the rest of brushFlags is untouchable here.
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

// ─── ROUND AG, ITEM 7: the hide/unhide undo domain ───────────────────────────
void KiwiVis_UndoPush( const char *label )
{
    visSnap_t snap;
    Snapshot( &snap );

    // A gesture that would restore to exactly what the newest record already
    // holds is not a step: pressing H twice with nothing left to hide must not
    // cost two Ctrl+Z presses.  (The construction store does not need this
    // because its pushes are all at the head of a real edit; the hide family's
    // are at the head of a call that very often changes nothing.)
    if ( !s_visUndo.empty() && SameSnap( s_visUndo.back(), snap ) )
        return;

    s_visUndo.push_back( visSnap_t() );
    s_visUndo.back().swap( snap );
    if ( (int)s_visUndo.size() > KVIS_UNDO_DEPTH )
        s_visUndo.erase( s_visUndo.begin() );

    s_visRedo.clear();
    KiwiUndo_NoteVisibilityRecord( label ? label : "hide" );
}

bool KiwiVis_UndoPop()
{
    if ( s_visUndo.empty() )
        return false;

    // Push the CURRENT state as the redo, exactly as KiwiCon_UndoPop does.
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
    s_visUndo.clear();
    s_visRedo.clear();
}

// ─── ROUND AG, ITEM 7: the ONE per-brush writer ──────────────────────────────
// The same two fields the ported family writes: Select_Hide's third pass
// (select.cpp:4179-4180) sets the bit and depth 1, ShowHidden (select.cpp:
// 4249-4250) clears both.  kiwi_outliner.cpp had a private copy of this; it now
// calls here, so there is one spelling of "hidden" for the eye and the H key.
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
    g_nUpdateBits = -1;
}

// ─── the KIWI-owned fourth member: INVERT HIDDEN ─────────────────────────────
void KiwiVis_InvertHidden()
{
    int nowHidden = 0, nowShown = 0;

    // ROUND AG, ITEM 7 — ONE record for the whole invert, taken before the first
    // write.  Ctrl+H being its own inverse is no longer a reason not to journal
    // it: the user asked for hide to be on the timeline, and a verb that is
    // undoable by pressing it again is still a verb Ctrl+Z should reach.
    KiwiVis_UndoPush( "invert hidden" );

    // BOTH display lists, in the ported order (selected first, then active —
    // Select_Hide's own order, select.cpp:4165/4171).
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        InvertOne( b, &nowHidden, &nowShown );
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
        InvertOne( b, &nowHidden, &nowShown );

    // The ported hide handlers' own invalidation (select.cpp:4182 / 4207 / 4257).
    g_nUpdateBits = -1;
    Sys_Printf( "Invert hidden: %i now hidden, %i shown.\n", nowHidden, nowShown );
}

// ─── §3 palette predicates ───────────────────────────────────────────────────
bool KiwiVis_CanHide()
{
    // Both ported cores bail immediately on an empty selected list
    // (select.cpp:4162, :4187), so the row would be a no-op without this.
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

int KiwiVis_HiddenCount()
{
    int n = 0;
    for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        if ( Hidden( b ) )
            ++n;
    for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
        if ( Hidden( b ) )
            ++n;
    return n;
}

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AO — HIDDEN SOLIDS ACROSS SAVE / LOAD
// ═════════════════════════════════════════════════════════════════════════════
// The design, the ordinal space and the honest limits are written out in
// kiwi_visibility.h (ROUND AO).  This half is the two walks and nothing else.
namespace
{
    // A hand-edited sidecar is a text file; this is the only place that can stop a
    // million `hiddenbrush` lines from becoming a million-entry vector.  Sized well
    // above any real map (CoD4 maps run tens of thousands of brushes) so it can
    // only ever be a bound on abuse, never on a legitimate file.
    const int KVIS_SIDECAR_MAX_HIDDEN = 262144;

    // (def, instance) — the def is the key, and it is NOT unique: an instanced
    // prefab can present the same def more than once, so lookups walk the whole
    // run of equal keys rather than taking the first.
    typedef std::pair<brush_t *, selbrush_t *> defInst_t;

    std::vector<brush_t *> s_saveOrder;        // the enumeration, ordinal == index
    std::vector<int>       s_hiddenOrdinals;   // which of those are hidden (save side)
    std::vector<int>       s_pendingHidden;    // ordinals read back from a sidecar
    int                    s_pendingTotal = -1;// its `hiddenbrushtotal`, -1 = absent

    // Every live brush INSTANCE, keyed on its def, sorted so the per-ordinal lookup
    // below is a binary search.  The undo store's Restore() (above) does the same
    // job with a nested scan; it can afford to because it runs once per Ctrl+Z on a
    // snapshot the same size as the list.  This one runs once per SAVE over every
    // brush in the map, and O(n^2) on a 30k-brush map is a visible stall.
    void BuildDefIndex( std::vector<defInst_t> *out )
    {
        out->clear();
        // Same two lists, in the same order, as every other walk in this file
        // (Select_Hide's order — select.cpp:4165/4171).  The order is irrelevant
        // once sorted; it is kept so the walk reads like its neighbours.
        for ( int pass = 0; pass < 2; ++pass )
        {
            selbrush_t *head = pass ? &active_brushes : &selected_brushes;
            for ( selbrush_t *b = head->next; b != head; b = b->next )
                out->push_back( defInst_t( b->def, b ) );
        }
        std::sort( out->begin(), out->end() );
    }

    // THE ENUMERATION.  A verbatim copy of Map_SaveFile's walk — see the citations
    // in kiwi_visibility.h.  If either loop below ever stops matching its source,
    // the ordinals stop meaning anything and the `hiddenbrushtotal` guard is what
    // stops that being visible to the user as randomly hidden geometry.
    void BuildSaveOrder( std::vector<brush_t *> *out )
    {
        out->clear();
        // map.cpp:693 — the entity loop, and :697-700 its write gate.
        for ( entity_s *eIter = entities.next; eIter != &entities; eIter = eIter->next )
        {
            entity_s_def *eDef = (entity_s_def *)eIter;
            // map.cpp:698 derefs eclass unconditionally (faithful to 0x486f59).  We
            // are not the save path and must never be the thing that crashes it, so
            // a null eclass is skipped: it contributes no brushes to the file either
            // way, so skipping it cannot shift an ordinal.
            if ( !eDef->eclass )
                continue;
            const bool hasDefBrushes = ( (void *)eDef->brushes.prev != (void *)&eDef->def );
            const bool isWorld       = ( strcmp( eDef->eclass->name, "worldspawn" ) == 0 );
            if ( !hasDefBrushes && !isWorld )
                continue;                       // not written at all → contributes nothing

            // map.cpp:1515-1517 — the per-entity DEF-list walk.  `brushes.prev` is
            // the OLDEST link and `onext` runs forward (Entity_LinkBrush,
            // entity.cpp:449-452), so this is insertion order == file order.
            //
            // DELIBERATE DIVERGENCE, the only one: MapFile_WriteEntity:1511 gates
            // this loop on `!eclass->fixedsize`, i.e. a light / misc_model writes no
            // brush block.  We count its bbox brush anyway, because ParseEntity's
            // fixed-size tail (entity.cpp:1250) recreates exactly one per entity in
            // the same entity order — so both sides still agree, and hiding a model
            // becomes persistable instead of being silently dropped.
            brush_t *sentinel = (brush_t *)&eDef->def;
            for ( brush_t *b = (brush_t *)eDef->brushes.prev; b != sentinel; b = b->onext )
                out->push_back( b );
        }
    }

    // First index of `def`'s run in the sorted index — callers walk forward while
    // `idx[k].first == def`.  Returns idx.size() when the def has no live instance
    // (a def-list brush that was never instanced — legal, and simply has no
    // visibility to read or to write).
    size_t DefRunBegin( const std::vector<defInst_t> &idx, brush_t *def )
    {
        const defInst_t key( def, (selbrush_t *)0 );   // second == 0 sorts below any real instance
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
        // ANY live instance hidden marks the ordinal.  A def with several instances
        // (an instanced prefab) has one ordinal and can only be given one answer;
        // "hidden if any is" is the reading that cannot lose a user's hide.
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
        return;                                          // malformed line — ignored, never fatal
    if ( (int)s_pendingHidden.size() >= KVIS_SIDECAR_MAX_HIDDEN )
        return;
    s_pendingHidden.push_back( ordinal );
}

void KiwiVis_SidecarLoadApply()
{
    if ( s_pendingHidden.empty() )
    {
        s_pendingTotal = -1;
        return;                                          // the normal case: nothing was hidden
    }

    // Re-run the enumeration against the map that is now live.  Same function the
    // save side used, which is the whole point.
    BuildSaveOrder( &s_saveOrder );
    const int total = (int)s_saveOrder.size();

    // THE GUARD.  A count that does not match means the two walks are looking at
    // different maps — a .map edited outside KIWI, a stale sidecar, a save/load
    // divergence.  Drop the entire set rather than apply a shifted one: hiding the
    // wrong brushes is a confusing bug report, hiding none is a no-op the user can
    // see is a no-op.
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
            ++skipped;                                   // out of range — ignored, not an error
            continue;
        }
        brush_t *def   = s_saveOrder[o];
        bool     found = false;
        for ( size_t k = DefRunBegin( idx, def ); k < idx.size() && idx[k].first == def; ++k )
        {
            // The SAME accessor Hide / Unhide All and the outliner eye use, so the
            // bit and the hide depth are written exactly once, in one spelling.
            KiwiVis_SetHidden( idx[k].second, true );
            found = true;
        }
        if ( found ) ++applied;
        else         ++skipped;                          // def with no live instance
    }

    s_pendingHidden.clear();
    s_pendingTotal = -1;

    // NO KiwiVis_UndoPush here.  A map load has just reset both undo stores
    // (KiwiCon_LoadSidecar → KiwiUndo_Reset, kiwi_construct.cpp:4451), and restored
    // state is the map's starting state, not a step the user took.
    g_nUpdateBits = -1;
    if ( applied )
        Sys_Printf( "Restored %i hidden brush(es) from the sidecar.\n", applied );
    if ( skipped )
        Sys_Printf( "WARNING: %i hidden-brush record(s) did not resolve and were ignored "
                    "(Unhide All clears any wrong hide).\n", skipped );
}

// ═════════════════════════════════════════════════════════════════════════════
//  ROUND AF, ITEM 9 — THE FOUR SILENT PATCH GATES, NAMED
// ═════════════════════════════════════════════════════════════════════════════
// See kiwi_visibility.h for the report this answers and for what each gate is.
// This file only READS state — it changes nothing, which is why it is safe to
// hang off the H key.
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

    // A filter ENTRY whose name mentions curves / terrain and which is currently
    // hidden.  Matched BY NAME, deliberately: what actually gates the patch is the
    // node keyword inside the entry's condition tree (filters.cpp:507-509 tests
    // `!strcmp( node, "curve" )` / `"terrain"`), and that tree is a private parse
    // product with no accessor.  The shipped RadiantFilters.txt names those entries
    // for what they filter, the Filters panel shows the user that same name, and a
    // diagnostic that names the row the user has to un-tick is worth more than one
    // that is provably exhaustive over a structure they cannot see.
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
                // _stricmp has no substring form; lowercase-scan by hand rather than
                // pull in a case-folding helper for two keywords.
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

// ─── commands ────────────────────────────────────────────────────────────────
void KiwiVis_RegisterCommands()
{
    // Unbound here — this is the CLASSIC-profile row.  kiwi_keymap.cpp puts it on
    // Ctrl+H in the modern profile (vk 0x48 mods 4, which the default table leaves
    // free; the audit is in kiwi_keymap.h).
    Radiant_RegisterCommand( "KiwiInvertHidden", 0, 0, KIWI_CMD_HIDE_INVERT );
}

bool KiwiVis_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned)KIWI_CMD_HIDE_INVERT )
        return false;
    KiwiVis_InvertHidden();
    // ROUND AF, ITEM 9: Ctrl+H is the family's "my visibility is weird" verb, so it
    // is where the ALL-CLEAR line earns its place — see kiwi_visibility.h.
    KiwiVis_ReportPatchGates( true );
    return true;
}

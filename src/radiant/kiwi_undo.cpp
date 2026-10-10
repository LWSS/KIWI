#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Unified undo/redo journal. Tickets preserve cross-domain order while each
// domain retains its own snapshots and restore implementation.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_undo.h"
#include "kiwi_construct.h"
#include "kiwi_visibility.h"    // visibility undo domain
#include "kiwi_refimage.h"      // KIWI (REFIMG): fourth whole-store domain
#include "kiwi_selection.h"     // face-granular selection restore
#include "kiwi_command.h"       // KiwiCmd_Active restore guard
#include "kiwi_ported.h"

#include <vector>
#include <unordered_map>
#include <algorithm>
#include <math.h>

// Ported undo entry points.
extern void  Undo_Undo();                          // undo.cpp:736  void Undo_Undo()
extern void  Undo_Redo();                          // undo.cpp:1019 void Undo_Redo()
extern bool  Undo_RedoAvailable();                 // undo.cpp:99   bool Undo_RedoAvailable()
extern void  Undo_SetMaxSize( int size );          // undo.cpp:276  (clears the legacy stack)
extern int   Undo_GetMaxSize();                    // undo.cpp:287
extern void  Undo_SetMaxMemorySize( int size );    // undo.cpp:295  (clears the legacy stack)
extern int   Undo_GetMaxMemorySize();              // undo.cpp:307

namespace
{
    // Every store's capacity together.  Global trimming is safe only when every domain
    // reports its own evictions before this limit is exceeded.
    int MaxTickets()
    {
        return Undo_GetMaxSize() + 3 * KUNDO_DOMAIN_DEPTH;
    }

    // Pointer identity does not survive legacy restore, so selection keys use a
    // snapshot-local brush ordinal and geometry values. See kiwi_undo.h.
    struct kundoSelKey_t
    {
        int   kind;                 // sel_kind_t, stored as an int
        int   ordinal;              // index into the snapshot's brush list
        int   faceIndex, edgeIndex, vertIndex;
        float mins[3], maxs[3];     // owning definition fingerprint
        int   faceCount;            // owning definition face count
        float normal[3], dist;      // selected face plane, when faceIndex >= 0
        bool  isActive;             // this entry was KiwiSel().active
    };

    const float KUNDO_FP_EPS = 0.05f;    // world units; a brush bound is grid-quantised
    const float KUNDO_PL_EPS = 0.001f;   // plane normal components are unit-length

    struct ticket_t
    {
        kundoDomain_t domain;
        const char   *label;    // a literal / static — see kiwi_undo.h
        // Selection before the record (restored by undo) and after it (by redo).
        // hasSel is false only for a record whose open was never armed.
        std::vector<kundoSelKey_t> sel;
        std::vector<kundoSelKey_t> after;
        bool                       hasSel;
    };

    std::vector<ticket_t> s_undo;    // oldest .. newest
    std::vector<ticket_t> s_redo;    // oldest .. newest (back() = the next redo)

    // A depth counter keeps nested cancel unwinds suppressed until the outer end.
    int  s_suppress = 0;

    // Undo_Redo closes a legacy record internally; suppress its generated ticket
    // and redo clear while forwarding an existing ticket.
    bool s_replay = false;

    void DropRedo()
    {
        s_redo.clear();
        KiwiCon_ClearRedo();            // domain-local redo follows the journal
        KiwiVis_ClearRedo();            // same rule for visibility
        KiwiRefImage_ClearRedo();       // same rule for reference images
    }

    // Snapshot armed by the current bracket and consumed by its ticket.
    std::vector<kundoSelKey_t> s_pendingSel;
    bool                       s_pendingArmed  = false;
    bool                       s_pendingLegacy = false;   // armed by Undo_GeneralStart

    // The live selection as value keys (see kundoSelKey_t).
    void CaptureSelection( std::vector<kundoSelKey_t> &out )
    {
        out.clear();
        const selection_t &sel = KiwiSel();

        // Deduplicated brush order supplies snapshot-local ordinals.
        std::unordered_map<const selbrush_t *, int> order;
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            const sel_item_t &it = sel.items[i];
            if ( !it.brush || !it.brush->def )
                continue;
            const int ord = (int)order.emplace( it.brush, (int)order.size() ).first->second;
            const brush_t *def = it.brush->def;

            kundoSelKey_t key;
            key.kind      = (int)it.kind;
            key.ordinal   = ord;
            key.faceIndex = it.faceIndex;
            key.edgeIndex = it.edgeIndex;
            key.vertIndex = it.vertIndex;
            key.faceCount = def->faceCount;
            for ( int c = 0; c < 3; ++c )
            {
                key.mins[c]   = def->mins[c];
                key.maxs[c]   = def->maxs[c];
                key.normal[c] = 0.0f;
            }
            key.dist = 0.0f;
            if ( it.faceIndex >= 0 && def->faces && it.faceIndex < def->faceCount )
            {
                const face_t &f = def->faces[it.faceIndex];
                for ( int c = 0; c < 3; ++c )
                    key.normal[c] = f.plane.normal[c];
                key.dist = (float)f.plane.dist;
            }
            key.isActive = Sel_ItemEqual( it, sel.active );
            out.push_back( key );
        }
    }

    void Append( kundoDomain_t domain, const char *label )
    {
        // Consume before suppression so a cancelled bracket cannot leak its
        // snapshot into the next record.
        std::vector<kundoSelKey_t> sel;
        const bool hasSel = s_pendingArmed;
        if ( s_pendingArmed )
        {
            sel.swap( s_pendingSel );
            s_pendingArmed  = false;
            s_pendingLegacy = false;
        }

        if ( s_suppress > 0 || s_replay )
            return;
        ticket_t t;
        t.domain = domain;
        t.label  = label ? label : "edit";
        t.sel.swap( sel );
        t.hasSel = hasSel && domain == KUNDO_LEGACY;
        if ( t.hasSel )
            CaptureSelection( t.after );            // Undo_End: the record's AFTER state
        s_undo.push_back( t );
        if ( (int)s_undo.size() > MaxTickets() )
            s_undo.erase( s_undo.begin() );
        DropRedo();                     // new work destroys the redo (undo.cpp's own rule)
    }

    // Remove the oldest matching ticket for an explicitly reported eviction.
    bool DropOldest( std::vector<ticket_t> &v, kundoDomain_t domain )
    {
        for ( size_t i = 0; i < v.size(); ++i )
        {
            if ( v[i].domain != domain )
                continue;
            v.erase( v.begin() + i );
            return true;
        }
        return false;
    }

    const char *DomainName( kundoDomain_t d )
    {
        return ( d == KUNDO_LEGACY )     ? "brush"
             : ( d == KUNDO_VISIBILITY ) ? "hide"
             : ( d == KUNDO_REFIMAGE )   ? "reference image"
                                         : "construction";
    }

    void DropAll( std::vector<ticket_t> &v, kundoDomain_t domain )
    {
        for ( size_t i = v.size(); i-- > 0; )
            if ( v[i].domain == domain )
                v.erase( v.begin() + i );
    }

    // Value-keyed face-selection restore.
    bool FingerprintMatches( const brush_t *def, const kundoSelKey_t &k )
    {
        if ( !def || def->faceCount != k.faceCount )
            return false;
        for ( int c = 0; c < 3; ++c )
        {
            if ( fabsf( def->mins[c] - k.mins[c] ) > KUNDO_FP_EPS ) return false;
            if ( fabsf( def->maxs[c] - k.maxs[c] ) > KUNDO_FP_EPS ) return false;
        }
        return true;
    }

    bool PlaneMatches( const brush_t *def, const kundoSelKey_t &k )
    {
        if ( k.faceIndex < 0 )
            return true;                       // not a face-indexed kind
        if ( !def->faces || k.faceIndex >= def->faceCount )
            return false;
        const face_t &f = def->faces[k.faceIndex];
        for ( int c = 0; c < 3; ++c )
            if ( fabsf( f.plane.normal[c] - k.normal[c] ) > KUNDO_PL_EPS )
                return false;
        return fabsf( (float)f.plane.dist - k.dist ) <= KUNDO_FP_EPS;
    }

    // Legacy undo selects restored brushes; untouched live brushes remain on the
    // active list, so matching must walk both lists.
    void CollectLiveBrushes( std::vector<selbrush_t *> &out )
    {
        for ( selbrush_t *b = selected_brushes.next;
              b && b != &selected_brushes; b = b->next )
            if ( b->def )
                out.push_back( b );
        for ( selbrush_t *b = active_brushes.next;
              b && b != &active_brushes; b = b->next )
            if ( b->def )
                out.push_back( b );
    }

    // The snapshot replaces whatever legacy undo/redo selected (it selects every brush
    // it recreates - for a Delete, the whole owning entity).
    void RestoreSelection( const ticket_t &t, const std::vector<kundoSelKey_t> &keys )
    {
        if ( !t.hasSel )
            return;                         // no snapshot: legacy selection stands
        if ( KiwiCmd_Active() )
            return;

        std::vector<selbrush_t *> live;
        CollectLiveBrushes( live );

        // First-unused matching keeps coincident brush ordinals from collapsing
        // onto one live instance.
        int maxOrd = -1;
        for ( size_t i = 0; i < keys.size(); ++i )
            if ( keys[i].ordinal > maxOrd )
                maxOrd = keys[i].ordinal;

        // Live brushes by mins[0]; a fingerprint can only match inside +-KUNDO_FP_EPS.
        std::vector<std::pair<float, size_t>> byX;
        byX.reserve( live.size() );
        for ( size_t i = 0; i < live.size(); ++i )
            byX.push_back( std::make_pair( live[i]->def->mins[0], i ) );
        std::sort( byX.begin(), byX.end() );

        std::vector<selbrush_t *> byOrd( (size_t)( maxOrd + 1 ), (selbrush_t *)0 );
        std::vector<char>         used( live.size(), 0 );
        std::vector<const kundoSelKey_t *> proto( (size_t)( maxOrd + 1 ), (const kundoSelKey_t *)0 );
        for ( size_t i = 0; i < keys.size(); ++i )   // every key of an ordinal has the same fingerprint
            if ( keys[i].ordinal >= 0 && !proto[(size_t)keys[i].ordinal] )
                proto[(size_t)keys[i].ordinal] = &keys[i];
        for ( int ord = 0; ord <= maxOrd; ++ord )
        {
            const kundoSelKey_t *p = proto[(size_t)ord];
            if ( !p )
                continue;
            auto it = std::lower_bound( byX.begin(), byX.end(),
                                        std::make_pair( p->mins[0] - KUNDO_FP_EPS, (size_t)0 ) );
            for ( ; it != byX.end() && it->first <= p->mins[0] + KUNDO_FP_EPS; ++it )
            {
                const size_t i = it->second;
                if ( used[i] || !FingerprintMatches( live[i]->def, *p ) )
                    continue;
                byOrd[(size_t)ord] = live[i];
                used[i] = 1;
                break;
            }
        }

        selection_t rebuilt;
        sel_item_t  activeItem;          // assign after the loop: Sel_Add moves active
        for ( size_t i = 0; i < keys.size(); ++i )
        {
            const kundoSelKey_t &k = keys[i];
            if ( k.ordinal < 0 || (size_t)k.ordinal >= byOrd.size() )
                continue;
            selbrush_t *node = byOrd[(size_t)k.ordinal];
            if ( !node || !PlaneMatches( node->def, k ) )
                continue;

            sel_item_t it;
            switch ( k.kind )
            {
            case SEL_FACE:   it = Sel_MakeFace  ( node, k.faceIndex ); break;
            case SEL_EDGE:   it = Sel_MakeEdge  ( node, k.faceIndex, k.edgeIndex ); break;
            case SEL_VERTEX: it = ( k.faceIndex < 0 )
                                  ? Sel_MakePatchPoint( node, k.vertIndex )
                                  : Sel_MakeVertex( node, k.faceIndex, k.vertIndex ); break;
            default:         it = Sel_MakeObject( node ); break;
            }
            Sel_Add( rebuilt, it );
            if ( k.isActive )
                activeItem = it;
        }
        rebuilt.active = Sel_ItemValid( activeItem ) ? activeItem : rebuilt.active;

        selection_t &sel = KiwiSel();
        Sel_Clear( sel );
        for ( size_t i = 0; i < rebuilt.items.size(); ++i )
            Sel_Add( sel, rebuilt.items[i] );
        if ( Sel_ItemValid( rebuilt.active ) )
            sel.active = rebuilt.active;
        Sel_SyncToLegacy();     // also clears legacy undo's whole-brush selection
    }
}

// Record hooks.
void KiwiUndo_NoteLegacyRecord( const char *operation )
{
    Append( KUNDO_LEGACY, operation );
}

void KiwiUndo_NoteConstructionRecord( const char *operation )
{
    Append( KUNDO_CONSTRUCTION, operation );
}

// Hidden state is instance-side and therefore needs a non-legacy domain.
void KiwiUndo_NoteVisibilityRecord( const char *operation )
{
    Append( KUNDO_VISIBILITY, operation );
}

void KiwiUndo_NoteRefImageRecord( const char *operation )
{
    Append( KUNDO_REFIMAGE, operation );
}

// Consistency hooks.
void KiwiUndo_NoteLegacyEvicted()
{
    // Same-domain records and tickets have identical order.
    DropOldest( s_undo, KUNDO_LEGACY );
}

void KiwiUndo_NoteLegacyCleared()
{
    DropAll( s_undo, KUNDO_LEGACY );
    DropAll( s_redo, KUNDO_LEGACY );
}

void KiwiUndo_NoteLegacyRedoCleared()
{
    // Cancels genuinely clear redo; only Undo_Redo's internal clear is suppressed.
    if ( s_replay )
        return;
    DropRedo();
}

// Arm before the first KIWI gesture mutation; a newer bracket replaces an
// unconsumed snapshot.
void KiwiUndo_ArmSelectionSnapshot()
{
    CaptureSelection( s_pendingSel );
    s_pendingArmed  = true;
    s_pendingLegacy = false;
}

// A KIWI bracket arms before its first mutation and then opens the record; that
// arm is the truer BEFORE.  A legacy arm left by a record that never closed is stale.
void KiwiUndo_ArmLegacySnapshot()
{
    if ( s_pendingArmed && !s_pendingLegacy )
        return;
    CaptureSelection( s_pendingSel );
    s_pendingArmed  = true;
    s_pendingLegacy = true;
}

// Suppression.
void KiwiUndo_SuppressBegin() { ++s_suppress; }
void KiwiUndo_SuppressEnd()   { if ( s_suppress > 0 ) --s_suppress; }

// Reset.
void KiwiUndo_Reset()
{
    s_undo.clear();
    s_redo.clear();
    s_pendingSel.clear();               // discard the map's pending bracket
    s_pendingArmed   = false;
    s_pendingLegacy  = false;
    KiwiVis_UndoReset();               // visibility history belongs to the map
    KiwiRefImage_UndoReset();           // reference-image history belongs to the map
}

// KIWI: the ported stack kept 64 records in at most 2 MB of snapshots, and the "Undo
// Levels" preference was loaded but never applied.  A terrain patch snapshot is ~20 KB, so
// one wide sculpt stroke filled the whole 2 MB and evicted every older step.
void KiwiUndo_ApplyLimits( int levels )
{
    if ( levels < 1 )
        levels = 1;
    if ( Undo_GetMaxSize() != levels )
        Undo_SetMaxSize( levels );
    if ( Undo_GetMaxMemorySize() != KUNDO_LEGACY_MEMORY )
        Undo_SetMaxMemorySize( KUNDO_LEGACY_MEMORY );
}

// Depths and labels.
int KiwiUndo_UndoDepth() { return (int)s_undo.size(); }
int KiwiUndo_RedoDepth() { return (int)s_redo.size(); }

const char *KiwiUndo_UndoLabel()
{
    return s_undo.empty() ? 0 : s_undo.back().label;
}

// Ctrl+Z.
bool KiwiUndo_Undo()
{
    // A settled reference-image edit (panel widget, armed drag) must be a ticket
    // before the newest ticket is chosen, or Ctrl+Z pops the record beneath it.
    KiwiRefImage_FlushPending();
    // Discard stale tickets until an authoritative domain store can honor one.
    while ( !s_undo.empty() )
    {
        const ticket_t t = s_undo.back();
        s_undo.pop_back();

        bool done = false;
        if ( t.domain == KUNDO_LEGACY )
        {
            if ( g_lastundo )                       // undo.cpp:738's own precondition
            {
                Undo_Undo();                        // mints the legacy REDO record
                done = true;
            }
        }
        else if ( t.domain == KUNDO_VISIBILITY )    // instance-side hidden snapshots
        {
            done = KiwiVis_UndoPop();               // pushes the hide redo snapshot
        }
        else if ( t.domain == KUNDO_REFIMAGE )
        {
            done = KiwiRefImage_UndoPop();
        }
        else
        {
            done = KiwiCon_UndoPop();               // pushes the construction redo snapshot
        }

        if ( !done )
        {
            Sys_Printf( "Undo: dropped a stale %s step.\n", DomainName( t.domain ) );
            continue;                               // try the next ticket down
        }

        // Legacy restore selects every recreated brush; put back the BEFORE selection.
        if ( t.domain == KUNDO_LEGACY )
            RestoreSelection( t, t.sel );

        s_redo.push_back( t );
        Sys_Printf( "Undo: %s (%i left).\n", t.label, (int)s_undo.size() );
        return true;
    }
    return false;                                   // caller falls through to the classic path
}

// Ctrl+Y / Ctrl+Shift+Z.
bool KiwiUndo_Redo()
{
    KiwiRefImage_FlushPending();            // same rule as undo; a new record drops redo
    while ( !s_redo.empty() )
    {
        const ticket_t t = s_redo.back();
        s_redo.pop_back();

        bool done = false;
        // Undo_Redo's internal close and redo clear are replay, not new work.
        s_replay = true;
        if ( t.domain == KUNDO_LEGACY )
        {
            if ( Undo_RedoAvailable() )             // undo.cpp:99 — g_lastredo != NULL
            {
                Undo_Redo();
                done = true;
            }
        }
        else if ( t.domain == KUNDO_VISIBILITY )    // instance-side hidden snapshots
        {
            done = KiwiVis_RedoPop();
        }
        else if ( t.domain == KUNDO_REFIMAGE )
        {
            done = KiwiRefImage_RedoPop();
        }
        else
        {
            done = KiwiCon_RedoPop();
        }
        s_replay = false;

        if ( !done )
        {
            Sys_Printf( "Redo: dropped a stale %s step.\n", DomainName( t.domain ) );
            continue;
        }

        // Same for redo, with the AFTER selection.
        if ( t.domain == KUNDO_LEGACY )
            RestoreSelection( t, t.after );

        s_undo.push_back( t );
        Sys_Printf( "Redo: %s (%i left).\n", t.label, (int)s_redo.size() );
        return true;
    }
    return false;
}

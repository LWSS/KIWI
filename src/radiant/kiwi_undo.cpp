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
#include "kiwi_selection.h"     // face-granular selection restore
#include "kiwi_command.h"       // KiwiCmd_Active restore guard

#include <vector>
#include <math.h>

// Ported undo entry points.
extern int   Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:118 int Sys_Printf(const char*,...)
extern void  Undo_Undo();                          // undo.cpp:736  void Undo_Undo()
extern void  Undo_Redo();                          // undo.cpp:1019 void Undo_Redo()
extern bool  Undo_RedoAvailable();                 // undo.cpp:99   bool Undo_RedoAvailable()
extern undo_s *g_lastundo;                         // undo.cpp:82   undo_s *g_lastundo (0x23F162C)

namespace
{
    // Default domain capacities total 64 + 32 + 32. Global trimming is safe only
    // when every domain reports its own evictions before this limit is exceeded.
    const int KUNDO_MAX_TICKETS = 128;

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
        // Empty unless a KIWI undo bracket armed a selection snapshot.
        std::vector<kundoSelKey_t> sel;
        bool                       selHasFace;
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
    }

    // Snapshot armed by the current KIWI bracket and consumed by its ticket.
    std::vector<kundoSelKey_t> s_pendingSel;
    bool                       s_pendingHasFace = false;
    bool                       s_pendingArmed   = false;

    void Append( kundoDomain_t domain, const char *label )
    {
        // Consume before suppression so a cancelled bracket cannot leak its
        // snapshot into the next record.
        std::vector<kundoSelKey_t> sel;
        bool hasFace = false;
        if ( s_pendingArmed )
        {
            sel.swap( s_pendingSel );
            hasFace        = s_pendingHasFace;
            s_pendingArmed = false;
            s_pendingHasFace = false;
        }

        if ( s_suppress > 0 || s_replay )
            return;
        ticket_t t;
        t.domain = domain;
        t.label  = label ? label : "edit";
        t.sel.swap( sel );
        t.selHasFace = hasFace;
        s_undo.push_back( t );
        if ( (int)s_undo.size() > KUNDO_MAX_TICKETS )
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

    void RestoreSelection( const ticket_t &t )
    {
        // Restore only face-bearing snapshots and never under a live modal command.
        if ( !t.selHasFace || t.sel.empty() )
            return;
        if ( KiwiCmd_Active() )
            return;

        std::vector<selbrush_t *> live;
        CollectLiveBrushes( live );
        if ( live.empty() )
            return;

        // First-unused matching keeps coincident brush ordinals from collapsing
        // onto one live instance.
        int maxOrd = -1;
        for ( size_t i = 0; i < t.sel.size(); ++i )
            if ( t.sel[i].ordinal > maxOrd )
                maxOrd = t.sel[i].ordinal;
        if ( maxOrd < 0 )
            return;

        std::vector<selbrush_t *> byOrd( (size_t)( maxOrd + 1 ), (selbrush_t *)0 );
        std::vector<char>         used( live.size(), 0 );
        for ( int ord = 0; ord <= maxOrd; ++ord )
        {
            // Every key for an ordinal carries the same brush fingerprint.
            const kundoSelKey_t *proto = 0;
            for ( size_t i = 0; i < t.sel.size() && !proto; ++i )
                if ( t.sel[i].ordinal == ord )
                    proto = &t.sel[i];
            if ( !proto )
                continue;
            for ( size_t i = 0; i < live.size(); ++i )
            {
                if ( used[i] || !FingerprintMatches( live[i]->def, *proto ) )
                    continue;
                byOrd[(size_t)ord] = live[i];
                used[i] = 1;
                break;
            }
        }

        selection_t rebuilt;
        sel_item_t  activeItem;          // assign after the loop: Sel_Add moves active
        int faces = 0;
        for ( size_t i = 0; i < t.sel.size(); ++i )
        {
            const kundoSelKey_t &k = t.sel[i];
            if ( k.ordinal < 0 || (size_t)k.ordinal >= byOrd.size() )
                continue;
            selbrush_t *node = byOrd[(size_t)k.ordinal];
            if ( !node || !PlaneMatches( node->def, k ) )
                continue;

            sel_item_t it;
            switch ( k.kind )
            {
            case SEL_FACE:   it = Sel_MakeFace  ( node, k.faceIndex ); ++faces; break;
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

        // If no face resolves, retain the selection produced by legacy undo.
        if ( !faces )
            return;

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
    s_pendingSel.clear();
    s_pendingHasFace = false;
    s_pendingArmed   = true;

    const selection_t &sel = KiwiSel();

    // Deduplicated brush order supplies snapshot-local ordinals.
    std::vector<selbrush_t *> order;
    for ( size_t i = 0; i < sel.items.size(); ++i )
    {
        selbrush_t *b = sel.items[i].brush;
        if ( !b || !b->def )
            continue;
        bool dup = false;
        for ( size_t k = 0; k < order.size() && !dup; ++k )
            dup = ( order[k] == b );
        if ( !dup )
            order.push_back( b );
    }

    for ( size_t i = 0; i < sel.items.size(); ++i )
    {
        const sel_item_t &it = sel.items[i];
        if ( !it.brush || !it.brush->def )
            continue;
        const brush_t *def = it.brush->def;

        int ord = -1;
        for ( size_t k = 0; k < order.size(); ++k )
            if ( order[k] == it.brush ) { ord = (int)k; break; }
        if ( ord < 0 )
            continue;

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

        if ( it.kind == SEL_FACE )
            s_pendingHasFace = true;
        s_pendingSel.push_back( key );
    }
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
    s_pendingHasFace = false;
    s_pendingArmed   = false;
    KiwiVis_UndoReset();                // visibility history belongs to the map
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
        else
        {
            done = KiwiCon_UndoPop();               // pushes the construction redo snapshot
        }

        if ( !done )
        {
            Sys_Printf( "Undo: dropped a stale %s step.\n", DomainName( t.domain ) );
            continue;                               // try the next ticket down
        }

        // Legacy restore selects covered brushes as whole objects; restore the
        // saved face selection when this KIWI ticket carries one.
        if ( t.domain == KUNDO_LEGACY )
            RestoreSelection( t );

        s_redo.push_back( t );
        Sys_Printf( "Undo: %s (%i left).\n", t.label, (int)s_undo.size() );
        return true;
    }
    return false;                                   // caller falls through to the classic path
}

// Ctrl+Y / Ctrl+Shift+Z.
bool KiwiUndo_Redo()
{
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

        // Reapply face selection after legacy redo when its value keys still match.
        if ( t.domain == KUNDO_LEGACY )
            RestoreSelection( t );

        s_undo.push_back( t );
        Sys_Printf( "Redo: %s (%i left).\n", t.label, (int)s_redo.size() );
        return true;
    }
    return false;
}

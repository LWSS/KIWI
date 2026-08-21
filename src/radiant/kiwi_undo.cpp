#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_undo.cpp — the unified undo/redo journal.  See kiwi_undo.h for the
// directive, the ticket model, the eviction-sync mechanism and the redo rules.
//
// NEW code.  It owns two vectors of tickets and forwards to the two domains'
// existing entry points; it copies no geometry and reimplements no restore.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_undo.h"
#include "kiwi_construct.h"
#include "kiwi_visibility.h"    // ROUND AG, ITEM 7 - the third domain
#include "kiwi_selection.h"     // ROUND BJ, ITEM 4 - the face-granular restore
#include "kiwi_command.h"       // ...and KiwiCmd_Active(), the one guard it needs

#include <vector>
#include <math.h>

// ── ported entry points (verified against their definitions) ────────────────
extern int   Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:118 int Sys_Printf(const char*,...)
extern void  Undo_Undo();                          // undo.cpp:736  void Undo_Undo()
extern void  Undo_Redo();                          // undo.cpp:1019 void Undo_Redo()
extern bool  Undo_RedoAvailable();                 // undo.cpp:99   bool Undo_RedoAvailable()
extern undo_s *g_lastundo;                         // undo.cpp:82   undo_s *g_lastundo (0x23F162C)

namespace
{
    // The journal's own cap.  Deliberately larger than BOTH domain caps
    // (g_undoMaxSize 64 + KCON_UNDO_DEPTH 32 = 96): the journal must never be the
    // thing that loses a step, so it only trims when both domains together could
    // not possibly still be holding that many records.
    const int KUNDO_MAX_TICKETS = 128;

    // ── KIWI-UX (ROUND BJ, ITEM 4): the value-keyed selection snapshot ──────
    // Every field is a VALUE.  `ordinal` names the owning brush inside the
    // snapshot's own brush list (deduped, in KiwiSel() order); `mins`/`maxs`/
    // `faceCount` are that brush def's fingerprint and `normal`/`dist` the plane
    // of `faceIndex`, both of which a texdef-only edit leaves untouched and the
    // legacy undo restores bit-for-bit from its clone.  See kiwi_undo.h.
    struct kundoSelKey_t
    {
        int   kind;                 // sel_kind_t, stored as an int
        int   ordinal;              // index into the snapshot's brush list
        int   faceIndex, edgeIndex, vertIndex;
        float mins[3], maxs[3];     // the OWNING DEF's fingerprint…
        int   faceCount;            // …and the rest of it
        float normal[3], dist;      // the face plane (faceIndex >= 0 only)
        bool  isActive;             // this entry was KiwiSel().active
    };

    const float KUNDO_FP_EPS = 0.05f;    // world units; a brush bound is grid-quantised
    const float KUNDO_PL_EPS = 0.001f;   // plane normal components are unit-length

    struct ticket_t
    {
        kundoDomain_t domain;
        const char   *label;    // a literal / static — see kiwi_undo.h
        // ROUND BJ, ITEM 4.  Empty on every ticket a ported record mints.
        std::vector<kundoSelKey_t> sel;
        bool                       selHasFace;
    };

    std::vector<ticket_t> s_undo;    // oldest .. newest
    std::vector<ticket_t> s_redo;    // oldest .. newest (back() = the next redo)

    // Appends are deaf while a cancel is unwinding its own bracket (kiwi_undo.h
    // CANCEL SUPPRESSION).  A DEPTH, not a bool: nested cancels must not re-arm
    // the hooks halfway out.
    int  s_suppress = 0;

    // True only while KiwiUndo_Redo is forwarding a ticket.  Stops the record
    // hooks from minting a NEW ticket (and from clearing the redo stack) for work
    // that is a REPLAY of one — Undo_Redo internally runs Undo_GeneralStart +
    // Undo_End, so without this a single Ctrl+Y would wipe the rest of the redo.
    bool s_replay = false;

    void DropRedo()
    {
        s_redo.clear();
        KiwiCon_ClearRedo();            // the construction store's own redo goes with it
        KiwiVis_ClearRedo();            // ROUND AG, ITEM 7 - and the hide store's
    }

    // ── ROUND BJ, ITEM 4: the armed snapshot, waiting for the next ticket ───
    std::vector<kundoSelKey_t> s_pendingSel;
    bool                       s_pendingHasFace = false;
    bool                       s_pendingArmed   = false;

    void Append( kundoDomain_t domain, const char *label )
    {
        // ROUND BJ, ITEM 4: the arm is consumed HERE, before the suppress test, so
        // a CANCELLED gesture (whose Undo_End is deaf) cannot leave a stale
        // snapshot to be stapled onto somebody else's record later.
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

    // Drop the OLDEST ticket naming `domain` from `v`.  Returns false when there
    // was none — which is the "journal is ahead of the domain" case and is
    // survivable, so it only ever costs a console line.
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

    // ROUND AG, ITEM 7: three domains, so the two-way ternary in the "stale step"
    // messages became a lie for the third.  One table instead.
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

    // ═══════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BJ, ITEM 4) — the value snapshot and its restore
    // ═══════════════════════════════════════════════════════════════════════
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

    // Every live instance, both display lists, in list order.  Phase 4 of Undo_Undo
    // pushes the restored brushes onto `selected_brushes`; anything the record did
    // not cover is still wherever it was, so both lists are walked.
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
        // Narrow by design — kiwi_undo.h SCOPE says why both of these decline.
        if ( !t.selHasFace || t.sel.empty() )
            return;
        if ( KiwiCmd_Active() )
            return;

        std::vector<selbrush_t *> live;
        CollectLiveBrushes( live );
        if ( live.empty() )
            return;

        // Map each snapshot ORDINAL onto a live instance, first-unused-wins over
        // list order.  Two coincident brushes share a fingerprint, so "unused"
        // is what keeps two ordinals from collapsing onto one brush.
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
            // The first key naming this ordinal carries the fingerprint (they all
            // carry the same one — it is the brush's, not the item's).
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
        sel_item_t  activeItem;          // set AFTER the loop: Sel_Add moves `active`
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

        // Nothing resolved as a FACE means the geometry moved out from under the
        // snapshot (a KIWI op that adds or removes brushes, for instance).  Leave
        // the ported undo's own selection alone rather than install a worse one.
        if ( !faces )
            return;

        selection_t &sel = KiwiSel();
        Sel_Clear( sel );
        for ( size_t i = 0; i < rebuilt.items.size(); ++i )
            Sel_Add( sel, rebuilt.items[i] );
        if ( Sel_ItemValid( rebuilt.active ) )
            sel.active = rebuilt.active;
        Sel_SyncToLegacy();     // opens with Select_Deselect(1) — the whole-brush
                                // selection Phase 4 left behind goes with it
    }
}

// ─── the record hooks ────────────────────────────────────────────────────────
void KiwiUndo_NoteLegacyRecord( const char *operation )
{
    Append( KUNDO_LEGACY, operation );
}

void KiwiUndo_NoteConstructionRecord( const char *operation )
{
    Append( KUNDO_CONSTRUCTION, operation );
}

// ROUND AG, ITEM 7 - the hide/unhide domain.  See kiwi_visibility.h for why the
// hidden bit needed a domain of its own rather than a legacy record.
void KiwiUndo_NoteVisibilityRecord( const char *operation )
{
    Append( KUNDO_VISIBILITY, operation );
}

// ─── the consistency hooks ───────────────────────────────────────────────────
void KiwiUndo_NoteLegacyEvicted()
{
    // The OLDEST legacy record just died (undo.cpp:316 Undo_FreeFirstUndo), so the
    // OLDEST LEGACY ticket has to die with it — the order of same-domain tickets
    // and the order of that domain's own records are the same order by
    // construction, so "oldest LEGACY ticket" names exactly that record.
    DropOldest( s_undo, KUNDO_LEGACY );
}

void KiwiUndo_NoteLegacyCleared()
{
    DropAll( s_undo, KUNDO_LEGACY );
    DropAll( s_redo, KUNDO_LEGACY );
}

void KiwiUndo_NoteLegacyRedoCleared()
{
    // NOT gated on s_suppress: a cancel really does clear the legacy redo list, so
    // the journal's redo stack has to follow it even mid-cancel.  It IS gated on
    // s_replay, because Undo_Redo's own internals must not eat the rest of the
    // stack they are walking.
    if ( s_replay )
        return;
    DropRedo();
}

// ─── KIWI-UX (ROUND BJ, ITEM 4): arm the selection snapshot ──────────────────
// Called from KiwiCmd_UndoBegin, i.e. once per KIWI gesture, before the first
// mutation.  Re-arming before the previous arm was consumed simply replaces it:
// the newer bracket is the one the next ticket belongs to.
void KiwiUndo_ArmSelectionSnapshot()
{
    s_pendingSel.clear();
    s_pendingHasFace = false;
    s_pendingArmed   = true;

    const selection_t &sel = KiwiSel();

    // The brush list the ordinals index, deduped in KiwiSel() order.
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

// ─── suppression ─────────────────────────────────────────────────────────────
void KiwiUndo_SuppressBegin() { ++s_suppress; }
void KiwiUndo_SuppressEnd()   { if ( s_suppress > 0 ) --s_suppress; }

// ─── reset ───────────────────────────────────────────────────────────────────
void KiwiUndo_Reset()
{
    s_undo.clear();
    s_redo.clear();
    s_pendingSel.clear();               // ROUND BJ, ITEM 4 — the arm dies with the map
    s_pendingHasFace = false;
    s_pendingArmed   = false;
    KiwiVis_UndoReset();                // ROUND AG, ITEM 7
}

// ─── depths / labels ─────────────────────────────────────────────────────────
int KiwiUndo_UndoDepth() { return (int)s_undo.size(); }
int KiwiUndo_RedoDepth() { return (int)s_redo.size(); }

const char *KiwiUndo_UndoLabel()
{
    return s_undo.empty() ? 0 : s_undo.back().label;
}

// ─── Ctrl+Z ──────────────────────────────────────────────────────────────────
bool KiwiUndo_Undo()
{
    // The self-heal loop (kiwi_undo.h BELT AND BRACES): a ticket whose domain
    // cannot actually honour it is DISCARDED rather than forwarded, and the next
    // one is tried.  In a synchronised journal this loop runs exactly once.
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
        else if ( t.domain == KUNDO_VISIBILITY )    // ROUND AG, ITEM 7
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

        // KIWI-UX (ROUND BJ, ITEM 4): the legacy restore re-selects every brush it
        // re-created as a whole OBJECT and empties g_SelectedFaces outright, so a
        // face-scoped KIWI record has to put its own selection back.  No-op for
        // every ticket that carries no face snapshot — which is all of them until
        // a KIWI gesture opens a bracket.
        if ( t.domain == KUNDO_LEGACY )
            RestoreSelection( t );

        s_redo.push_back( t );
        Sys_Printf( "Undo: %s (%i left).\n", t.label, (int)s_undo.size() );
        return true;
    }
    return false;                                   // caller falls through to the classic path
}

// ─── Ctrl+Y / Ctrl+Shift+Z ───────────────────────────────────────────────────
bool KiwiUndo_Redo()
{
    while ( !s_redo.empty() )
    {
        const ticket_t t = s_redo.back();
        s_redo.pop_back();

        bool done = false;
        // s_replay is held across the forward ONLY: Undo_Redo runs a whole
        // Undo_GeneralStart / Undo_End bracket internally, and neither the ticket
        // it would mint nor the redo-clear it would trigger is real new work.
        s_replay = true;
        if ( t.domain == KUNDO_LEGACY )
        {
            if ( Undo_RedoAvailable() )             // undo.cpp:99 — g_lastredo != NULL
            {
                Undo_Redo();
                done = true;
            }
        }
        else if ( t.domain == KUNDO_VISIBILITY )    // ROUND AG, ITEM 7
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

        // ROUND BJ, ITEM 4: the same restore on the way back.  Undo_Redo re-runs the
        // record's own re-create path and therefore re-selects the same whole
        // brushes; and a face-scoped gesture never changed the selection, so the
        // state that was right at gesture begin is right at both ends of it.
        if ( t.domain == KUNDO_LEGACY )
            RestoreSelection( t );

        s_undo.push_back( t );
        Sys_Printf( "Redo: %s (%i left).\n", t.label, (int)s_redo.size() );
        return true;
    }
    return false;
}

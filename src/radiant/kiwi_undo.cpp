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

#include <vector>

// ── ported entry points (verified against their definitions) ────────────────
extern int   Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:112 int Sys_Printf(const char*,...)
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

    struct ticket_t
    {
        kundoDomain_t domain;
        const char   *label;    // a literal / static — see kiwi_undo.h
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

    void Append( kundoDomain_t domain, const char *label )
    {
        if ( s_suppress > 0 || s_replay )
            return;
        ticket_t t;
        t.domain = domain;
        t.label  = label ? label : "edit";
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

// ─── suppression ─────────────────────────────────────────────────────────────
void KiwiUndo_SuppressBegin() { ++s_suppress; }
void KiwiUndo_SuppressEnd()   { if ( s_suppress > 0 ) --s_suppress; }

// ─── reset ───────────────────────────────────────────────────────────────────
void KiwiUndo_Reset()
{
    s_undo.clear();
    s_redo.clear();
    KiwiVis_UndoReset();                // ROUND AG, ITEM 7
}

// ─── depths / labels ─────────────────────────────────────────────────────────
int KiwiUndo_UndoDepth() { return (int)s_undo.size(); }
int KiwiUndo_RedoDepth() { return (int)s_redo.size(); }

const char *KiwiUndo_UndoLabel()
{
    return s_undo.empty() ? 0 : s_undo.back().label;
}

const char *KiwiUndo_RedoLabel()
{
    return s_redo.empty() ? 0 : s_redo.back().label;
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

        s_undo.push_back( t );
        Sys_Printf( "Redo: %s (%i left).\n", t.label, (int)s_redo.size() );
        return true;
    }
    return false;
}

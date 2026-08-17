#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_conclip.cpp — KIWI-UX ROUND AR, ITEM 1 implementation.  See kiwi_conclip.h
// for why the payload is process-local rather than on the OS clipboard, what a
// pasted object resets, and why a MIXED paste refuses the auto-Move.
//
// NEW code over the KIWI layers only.  It touches NO ported state: the store is
// KIWI's own (kiwi_construct.cpp) and so is the parallel selection
// (kiwi_conselect.cpp).  The one ported thing used here is Sys_Printf.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_conclip.h"
#include "kiwi_conselect.h"
#include "kiwi_construct.h"

#include <vector>

// ── ported entry points (verified against their definitions) ────────────────
extern int Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:112  int Sys_Printf(const char*,...)
extern int g_nUpdateBits;                        // engine_stubs.cpp:773  int g_nUpdateBits = 0  (0x25D5A74)

namespace
{
    // The payload.  Whole objects, by value — a kconObject_t owns its own vectors
    // and its own std::string, so a copy is self-contained and nothing here can
    // dangle when the store is rearranged underneath it.  That is the whole reason
    // the clipboard stores OBJECTS and not the store INDICES the selection uses:
    // indices shift on every KiwiCon_RemoveAt (kiwi_conselect.h), so a clipboard
    // made of them would paste the wrong lines after a delete.
    std::vector<kconObject_t> s_clip;

    // ROUND AR, ITEM 1: the one-shot "this paste landed N lines" latch.  See
    // kiwi_conclip.h for why KiwiCmd_AfterPaste cannot ask the live selection
    // instead (it is the tail on CLONE too).
    int s_justPasted = 0;
}

int KiwiConClip_TakeJustPasted()
{
    const int n = s_justPasted;
    s_justPasted = 0;
    return n;
}

int KiwiConClip_Copy()
{
    // WHOLE OBJECTS, whatever granularity the items carry.  A KCONSEL_POINT or
    // KCONSEL_SEGMENT item names a part of an object that has no independent
    // existence in the store — there is no such thing as "half a polyline" to put
    // on a clipboard — and G's construction arm already reads a part-selection as
    // its whole object (kiwi_transform.cpp's v1 note).  One rule, both verbs.
    // De-duplicated BY STORE INDEX — three selected segments of one polyline name
    // one object three times, and the clipboard must hold it once.  Index is the
    // right key here and nowhere else: it is only ever used inside this loop, while
    // the store is not being mutated.
    std::vector<int>          objs;
    std::vector<kconObject_t> made;
    const int items = KiwiConSel_Count();
    for ( int i = 0; i < items; ++i )
    {
        const kconSelItem_t *it = KiwiConSel_At( i );
        if ( !it )
            continue;
        const kconObject_t *o = KiwiCon_At( it->object );
        if ( !o )
            continue;
        bool dup = false;
        for ( size_t k = 0; k < objs.size() && !dup; ++k )
            dup = ( objs[k] == it->object );
        if ( dup )
            continue;
        objs.push_back( it->object );
        made.push_back( *o );
    }

    if ( made.empty() )
    {
        // ── THE TWO CLIPBOARDS MUST AGREE ABOUT WHAT WAS COPIED ─────────────
        // They are separate stores (kiwi_conclip.h says why), and separate stores
        // can DISAGREE: copy lines, then copy a brush, and a later Ctrl+V would
        // paste the brush AND the lines from the earlier copy — a mixed paste the
        // user never asked for, which then refuses the auto-Move on top.
        //
        // The rule that keeps them honest is the obvious one: **a Copy that took
        // solids and no lines means the clipboard now holds solids only.**  So this
        // half is emptied — but ONLY when the ported Copy actually had something to
        // take.  `selected_brushes` is exactly what XYWnd_CopyClip serialises, so
        // this asks the same question the ported half just answered.
        //
        // A Copy with NOTHING selected at all still leaves BOTH clipboards alone,
        // which is the rule KiwiCmd_ClipCut applies brush-side for the same reason:
        // an accidental Ctrl+C must not destroy what you were about to paste.
        if ( selected_brushes.next != &selected_brushes )
            s_clip.clear();
        return 0;
    }

    s_clip.swap( made );
    Sys_Printf( "Copy: %i construction object(s) copied.\n", (int)s_clip.size() );
    return (int)s_clip.size();
}

int KiwiConClip_Paste()
{
    if ( s_clip.empty() )
        return 0;                    // silent: an ordinary brush paste says nothing

    // ONE snapshot for the whole paste (kiwi_construct.h ruling 2), taken BEFORE
    // the first Add so a single construction Ctrl+Z removes every pasted object.
    KiwiCon_UndoPush();

    std::vector<int> landed;
    for ( size_t i = 0; i < s_clip.size(); ++i )
    {
        kconObject_t o = s_clip[i];
        o.hidden = false;            // a paste is new scaffolding — kiwi_conclip.h
        o.group  = -1;               // …and it is ungrouped
        const int idx = KiwiCon_Add( o );   // normalises the points and refits the plane
        if ( idx >= 0 )
            landed.push_back( idx );
    }

    if ( landed.empty() )
    {
        // KiwiCon_Add refused every one of them (over the point budget, or under
        // two points).  It has already said so per object; this is the summary, and
        // the undo push is left in place — it is a no-op snapshot, which is exactly
        // what an undo of "nothing happened" should restore.
        Sys_Printf( "Paste: no construction object could be added.\n" );
        return 0;
    }

    // The pasted set IS the selection, and nothing else is — the same contract the
    // brush paste has (Map_ImportBuffer opens with Select_Deselect(1)), and what
    // makes the Move that follows unambiguous.  Safe against the store's index
    // shifting: KiwiCon_Add only ever APPENDS, so nothing already in the store was
    // renumbered and these indices are current.
    KiwiConSel_SelectObjects( &landed[0], (int)landed.size() );

    s_justPasted = (int)landed.size();      // read once by KiwiCmd_AfterPaste
    Sys_Printf( "Paste: %i construction object(s) pasted.\n", (int)landed.size() );
    g_nUpdateBits = -1;
    return (int)landed.size();
}

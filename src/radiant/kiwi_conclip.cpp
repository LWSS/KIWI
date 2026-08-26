#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Construction copy/paste uses KIWI's store and parallel selection.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_conclip.h"
#include "kiwi_conselect.h"
#include "kiwi_construct.h"

#include <vector>

// Legacy symbols.
extern int Sys_Printf( const char *fmt, ... );   // win_qe3.cpp:118  int Sys_Printf(const char*,...)
extern int g_nUpdateBits;                        // engine_stubs.cpp:773  int g_nUpdateBits = 0  (0x25D5A74)

namespace
{
    // Store values, not selection indices: removals renumber the indices.
    std::vector<kconObject_t> s_clip;

    // The one-shot count distinguishes paste output from a stale live selection.
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
    // Point/segment items copy their whole object. Deduplicate by store index,
    // which cannot change during this read-only loop.
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
        // Brush-only Copy clears stale construction data to prevent a mixed paste.
        // This branch treats no legacy brush selection as an empty Copy.
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

    // Snapshot before the batch so one construction Ctrl+Z removes all landed objects.
    KiwiCon_UndoPush();

    std::vector<int> landed;
    for ( size_t i = 0; i < s_clip.size(); ++i )
    {
        kconObject_t o = s_clip[i];
        o.hidden = false;            // pasted scaffolding must be visible
        o.group  = -1;               // the clipboard can outlive its source group table
        const int idx = KiwiCon_Add( o );   // normalises the points and refits the plane
        if ( idx >= 0 )
            landed.push_back( idx );
    }

    if ( landed.empty() )
    {
        // KiwiCon_Add already reported each rejection. The pre-paste undo record remains.
        Sys_Printf( "Paste: no construction object could be added.\n" );
        return 0;
    }

    // Replace selection with the pasted set. KiwiCon_Add only appends, so landed
    // indices remain valid throughout this batch.
    KiwiConSel_SelectObjects( &landed[0], (int)landed.size() );

    s_justPasted = (int)landed.size();      // read once by KiwiCmd_AfterPaste
    Sys_Printf( "Paste: %i construction object(s) pasted.\n", (int)landed.size() );
    g_nUpdateBits = -1;
    return (int)landed.size();
}

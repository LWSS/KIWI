#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_focus.cpp — ROUND J: "/" frames the selection.  See kiwi_focus.h for the
// Plasticity source, the `/` vs `space` distinction, and what gets framed.
//
// NEW code.  It reads bounds and moves the camera; it mutates no map data, no
// selection and no construction geometry.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_camera.h"
#include "kiwi_command.h"
#include "kiwi_conselect.h"
#include "kiwi_construct.h"
#include "kiwi_focus.h"

// ── ported entry points (each verified against its DEFINITION) ─────────────
//   win_qe3.cpp:112        int  Sys_Printf( const char *fmt, ... )
//   select.cpp:2056        void Select_GetBounds( float *mins, float *maxs )
//   qe3.h:1054             selbrush_t selected_brushes;   // 0x23f1864
//   qe3.h:1053             selbrush_t active_brushes;     // 0x23f189c
//   qe3.h:484/485          brush_t::mins / ::maxs  (vec3_t @0x20 / @0x2C)
//   mainfrm.cpp:1251       bool Radiant_RegisterCommand( const char *name, byte vk,
//                                                        byte mods, int commandId )
extern int  Sys_Printf( const char *fmt, ... );
extern void Select_GetBounds( float *mins, float *maxs );
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

namespace
{
    // The hidden bit — the same one kiwi_visibility.cpp and the three ported hide
    // cores use (select.cpp:4168).
    const unsigned KFOC_HIDDEN_BIT = 4u;

    struct box_t
    {
        float lo[3];
        float hi[3];
        bool  any = false;

        void Add( const float *p )
        {
            if ( !any )
            {
                for ( int k = 0; k < 3; ++k ) { lo[k] = p[k]; hi[k] = p[k]; }
                any = true;
                return;
            }
            for ( int k = 0; k < 3; ++k )
            {
                if ( p[k] < lo[k] ) lo[k] = p[k];
                if ( p[k] > hi[k] ) hi[k] = p[k];
            }
        }
        void AddBox( const float *bmin, const float *bmax ) { Add( bmin ); Add( bmax ); }
    };

    void AddObjectVerts( box_t *box, const kconObject_t &o )
    {
        const int n = KiwiCon_VertCount( o );
        for ( int i = 0; i < n; ++i )
        {
            float w[3];
            if ( KiwiCon_VertWorld( o, i, w ) )
                box->Add( w );
        }
    }

    // The SELECTED construction objects (any granularity — a selected point or
    // segment still names an object, and framing the whole object is the same
    // "give me the context" answer the brush path gives for a selected face).
    void AddConstructionSelection( box_t *box )
    {
        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            if ( !KiwiConSel_ObjectSelected( i ) )
                continue;
            const kconObject_t *o = KiwiCon_At( i );
            if ( o )
                AddObjectVerts( box, *o );
        }
    }

    // Plasticity's `visibleObjects` fallback: everything that is on screen.
    void AddVisibleWorld( box_t *box )
    {
        for ( selbrush_t *b = active_brushes.next; b != &active_brushes; b = b->next )
            if ( b->def && ( (unsigned)b->brushFlags & KFOC_HIDDEN_BIT ) == 0 )
                box->AddBox( b->def->mins, b->def->maxs );
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            if ( b->def && ( (unsigned)b->brushFlags & KFOC_HIDDEN_BIT ) == 0 )
                box->AddBox( b->def->mins, b->def->maxs );

        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            // ROUND U: this function is literally "everything that is on screen",
            // and a hidden construction object is not.  The brush arms above
            // already exclude their own hidden bit; this is the same rule.
            if ( o && !o->hidden )
                AddObjectVerts( box, *o );
        }
    }
}

// ─── the shared "what would / frame" query ───────────────────────────────────
bool KiwiFocus_SelectionBounds( float mins[3], float maxs[3] )
{
    box_t box;

    // 1. the LEGACY brush selection, through the ported core.  Select_GetBounds
    //    seeds an INVERTED +-131072 sentinel box and unions each selected brush
    //    into it (select.cpp:2058-2069), so with an empty selection it comes back
    //    inverted — which is exactly the "nothing here" test, and is why the list
    //    is checked rather than the box.
    if ( selected_brushes.next != &selected_brushes )
    {
        float bmin[3], bmax[3];
        Select_GetBounds( bmin, bmax );
        if ( bmin[0] <= bmax[0] && bmin[1] <= bmax[1] && bmin[2] <= bmax[2] )
            box.AddBox( bmin, bmax );
    }

    // 2. the KIWI construction selection, which never enters selection_t.
    AddConstructionSelection( &box );

    // 3. nothing named → the whole visible world.
    if ( !box.any )
        AddVisibleWorld( &box );

    if ( !box.any )
        return false;

    for ( int k = 0; k < 3; ++k )
    {
        mins[k] = box.lo[k];
        maxs[k] = box.hi[k];
    }
    return true;
}

bool KiwiFocus_CanFocus()
{
    // DELIBERATELY NOT `KiwiFocus_SelectionBounds(...)`.  The palette calls every
    // row's canExecute EVERY FRAME it is open (kiwi_palette.cpp:114), and the
    // no-selection path of SelectionBounds walks both brush lists AND tessellates
    // every construction circle/arc with cosf/sinf per vertex.  The cheap test
    // below gives the SAME answer — the box can only be empty when there is
    // nothing at all — for three pointer compares.
    return selected_brushes.next != &selected_brushes
        || active_brushes.next   != &active_brushes
        || KiwiCon_Count() > 0;
}

// ─── commands ────────────────────────────────────────────────────────────────
void KiwiFocus_RegisterCommands()
{
    // Unbound here — the CLASSIC-profile row.  kiwi_keymap.cpp puts it on the bare
    // "/" (vk 0xBF, mods 0) in the modern profile, which is Plasticity's own key
    // and is free in the default table; the audit is in kiwi_keymap.h.
    Radiant_RegisterCommand( "KiwiFocusSelection", 0, 0, KIWI_CMD_FOCUS_SELECTION );
}

bool KiwiFocus_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned)KIWI_CMD_FOCUS_SELECTION )
        return false;

    float mins[3], maxs[3];
    if ( !KiwiFocus_SelectionBounds( mins, maxs ) )
    {
        Sys_Printf( "Focus: nothing to look at.\n" );
        return true;
    }
    KiwiCam_FrameBounds( mins, maxs );
    return true;
}

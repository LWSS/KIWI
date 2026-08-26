#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Camera focus commands. They move the view and may set the construction plane;
// they do not mutate map geometry or selection.

#include "stdafx.h"
#include "qe3.h"

#include "kiwi_camera.h"
#include "kiwi_command.h"
#include "kiwi_conselect.h"
#include "kiwi_construct.h"
#include "kiwi_focus.h"
#include "kiwi_hover.h"                     // Face hover fallback.
#include "kiwi_selection.h"                 // Face-selection validity.
#include "kiwi_skybox.h"                    // Sky-shell detection.

#include <math.h>                           // Face aim angles.

// Ported entry points, verified against their definitions.
//   win_qe3.cpp:112        int  Sys_Printf( const char *fmt, ... )
//   select.cpp:2073        void Select_GetBounds( float *mins, float *maxs )
//   qe3.h:1054             selbrush_t selected_brushes;   // 0x23f1864
//   qe3.h:1053             selbrush_t active_brushes;     // 0x23f189c
//   mainfrm.cpp:1340       bool Radiant_RegisterCommand( const char *name, byte vk,
//                                                        byte mods, int commandId )
extern int  Sys_Printf( const char *fmt, ... );
extern void Select_GetBounds( float *mins, float *maxs );
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

namespace
{
    // Shared with kiwi_visibility.cpp and the ported hide core (select.cpp:4185).
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

    // Any selected sub-element names its whole construction object, matching the
    // brush path's context framing for selected faces.
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

    // Skip sky shells in fallback framing because their enclosing bounds place the
    // camera outside the shell. Explicitly selected sky is still framed.
    // If every visible brush is sky, retain it rather than return an empty box.
    bool AddWorldBrushes( box_t *box, bool skipSky )
    {
        bool added = false;
        for ( int list = 0; list < 2; ++list )
        {
            selbrush_t *head = list ? &selected_brushes : &active_brushes;
            for ( selbrush_t *b = head->next; b != head; b = b->next )
            {
                if ( !b->def || ( (unsigned)b->brushFlags & KFOC_HIDDEN_BIT ) != 0 )
                    continue;
                if ( skipSky && KiwiSky_IsSkyBrush( b->def ) )
                    continue;
                box->AddBox( b->def->mins, b->def->maxs );
                added = true;
            }
        }
        return added;
    }

    // Plasticity's visibleObjects fallback: everything on screen.
    void AddVisibleWorld( box_t *box )
    {
        if ( !AddWorldBrushes( box, true ) )
            AddWorldBrushes( box, false );      // every visible brush was sky

        const int count = KiwiCon_Count();
        for ( int i = 0; i < count; ++i )
        {
            const kconObject_t *o = KiwiCon_At( i );
            // Hidden construction objects are not part of the visible fallback.
            if ( o && !o->hidden )
                AddObjectVerts( box, *o );
        }
    }
}

bool KiwiFocus_SelectionBounds( float mins[3], float maxs[3] )
{
    box_t box;

    // Select_GetBounds uses an inverted +-131072 sentinel when no selected brush
    // contributes bounds; check the list first (select.cpp:2075-2086).
    if ( selected_brushes.next != &selected_brushes )
    {
        float bmin[3], bmax[3];
        Select_GetBounds( bmin, bmax );
        if ( bmin[0] <= bmax[0] && bmin[1] <= bmax[1] && bmin[2] <= bmax[2] )
            box.AddBox( bmin, bmax );
    }

    // Construction selection never enters the legacy selection_t.
    AddConstructionSelection( &box );

    // Nothing named: frame the whole visible world.
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
    // Palette predicates run every frame; avoid the bounds walk and construction
    // tessellation performed by KiwiFocus_SelectionBounds (kiwi_palette.cpp:114).
    return selected_brushes.next != &selected_brushes
        || active_brushes.next   != &active_brushes
        || KiwiCon_Count() > 0;
}

// Space follows Plasticity's navigate-to-selection split from "/" focus: aim at
// the face and install its construction plane without changing projection.
// The modern profile moves classic Clone from Space to Shift+Space.
namespace
{
    // Prefer the active selected face, then another selected face, then hover.
    // Active-first makes multi-face selection use the most recently clicked face.
    bool FaceUnderVerb( selbrush_t **outBrush, int *outFace )
    {
        const selection_t &sel = KiwiSel();
        if ( sel.active.kind == SEL_FACE && Sel_ItemValid( sel.active ) )
        {
            *outBrush = sel.active.brush;
            *outFace  = sel.active.faceIndex;
            return true;
        }
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            if ( sel.items[i].kind == SEL_FACE && Sel_ItemValid( sel.items[i] ) )
            {
                *outBrush = sel.items[i].brush;
                *outFace  = sel.items[i].faceIndex;
                return true;
            }
        }
        // Hover is the fallback, not the primary selection source.
        const pick_result_t &hov = KiwiHover_Get();
        if ( hov.valid && hov.item.kind == SEL_FACE && Sel_ItemValid( hov.item ) )
        {
            *outBrush = hov.item.brush;
            *outFace  = hov.item.faceIndex;
            return true;
        }
        return false;
    }
}

void KiwiFocus_RegisterCommands()
{
    // Register unbound classic rows; the modern profile assigns "/" and Space.
    Radiant_RegisterCommand( "KiwiFocusSelection", 0, 0, KIWI_CMD_FOCUS_SELECTION );
    Radiant_RegisterCommand( "KiwiViewFace", 0, 0, KIWI_CMD_VIEW_FACE );
}

bool KiwiFocus_CanViewFace()
{
    selbrush_t *b = nullptr;
    int         f = -1;
    return FaceUnderVerb( &b, &f );
}

bool KiwiFocus_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId == (unsigned)KIWI_CMD_FOCUS_SELECTION )
    {
        float mins[3], maxs[3];
        if ( !KiwiFocus_SelectionBounds( mins, maxs ) )
        {
            Sys_Printf( "Focus: nothing to look at.\n" );
            return true;
        }
        KiwiCam_FrameBounds( mins, maxs );
        return true;
    }

    if ( cmdId != (unsigned)KIWI_CMD_VIEW_FACE )
        return false;

    selbrush_t *inst = nullptr;
    int         fi   = -1;
    if ( !FaceUnderVerb( &inst, &fi ) )
    {
        // With no selected or hovered face, Space clears an explicit plane.
        // Esc remains selection-only; a construction plane is not a selection.
        if ( KiwiCon_PlaneIsExplicit() )
        {
            KiwiCon_ClearPlaneToDefault();
            return true;
        }
        Sys_Printf( "View Face: no face is selected or under the cursor.  Switch to "
                    "FACE mode (3), click a face, then press Space — that looks at "
                    "the face head-on AND makes it the drawing plane.\n" );
        return true;
    }
    brush_t *def = inst->def;
    if ( !def || !def->faces || fi < 0 || fi >= def->faceCount )
    {
        Sys_Printf( "View Face: that face went away.\n" );
        return true;
    }
    const winding_t *w = def->faces[fi].w;
    if ( !w || w->numpoints < 3 )
    {
        Sys_Printf( "View Face: that face has no winding to look at.\n" );
        return true;
    }

    // Feed the face winding AABB through the same framing fit as "/"; this keeps
    // its standoff, margin, and radius floor consistent.
    box_t box;
    for ( int i = 0; i < w->numpoints; ++i )
        box.Add( w->p[i] );
    if ( !box.any )
        return true;

    // Order is load-bearing: LookAlong changes direction about the current pivot;
    // FrameBounds then retargets and dollies while preserving that direction.
    // Camera convention is vpn=(cos p cos y, cos p sin y, sin p); d is -normal.
    const float *n = def->faces[fi].plane.normal;
    const float  nl = sqrtf( n[0]*n[0] + n[1]*n[1] + n[2]*n[2] );
    if ( !( nl > 1.0e-6f ) )
    {
        Sys_Printf( "View Face: that face has a degenerate normal.\n" );
        return true;
    }
    const float d[3] = { -n[0] / nl, -n[1] / nl, -n[2] / nl };
    float dz = d[2];
    if ( dz >  1.0f ) dz =  1.0f;
    if ( dz < -1.0f ) dz = -1.0f;
    // asin/atan2 return radians; 180/pi converts to the camera's degree convention.
    const float pitch = (float)( asin( (double)dz )      * 57.295779513082320876 );
    const float yaw   = (float)( atan2( (double)d[1], (double)d[0] ) * 57.295779513082320876 );

    KiwiCam_LookAlong( pitch, yaw );
    KiwiCam_FrameBounds( box.lo, box.hi );

    // Space combines the camera move with Plasticity's construction-plane update.
    // The shared helper uses the winding centre, normal, and longest-edge hint;
    // marking it explicit prevents tool starts from replacing it.
    {
        if ( KiwiCon_SetPlaneFromFace( inst, fi, false ) )
        {
            KiwiCon_MarkPlaneExplicit( "a face (Space)" );
            // Only planar shape tools consume this plane; line tools follow cursor hits.
            Sys_Printf( "View Face: looking at the face head-on, and it is now the "
                        "plane for the SHAPE tools (rect / circle / arc / n-gon) "
                        "(normal %.2f %.2f %.2f).  Lines always follow the cursor.  "
                        "Space over empty space clears it.\n",
                        n[0] / nl, n[1] / nl, n[2] / nl );
            return true;
        }
    }

    Sys_Printf( "View Face: looking at the face head-on (normal %.2f %.2f %.2f).\n",
                n[0] / nl, n[1] / nl, n[2] / nl );
    return true;
}

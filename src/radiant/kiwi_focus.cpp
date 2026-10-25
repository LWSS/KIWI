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
#include "kiwi_ported.h"
#include "kiwi_region.h"                    // Space on construction faces.
#include "kiwi_selection.h"                 // Face-selection validity.
#include "kiwi_skybox.h"                    // Sky-shell detection.
#include "kiwi_vec.h"                       // Dot3 / Sub3.

#include <vector>

#include <math.h>                           // Face aim angles.

// Ported entry points, verified against their definitions.
//   select.cpp:2073        void Select_GetBounds( float *mins, float *maxs )
extern void Select_GetBounds( float *mins, float *maxs );

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
    // What Space aims at: a brush face or a construction region (KIWI 2026-09-28, user:
    // "allow aligning with construction faces (for when its impossible to click behind
    // them, allow spacebar to work)").
    struct faceTarget_t
    {
        selbrush_t *brush  = nullptr;
        int         face   = -1;
        int         region = -1;            // KiwiRegion_All index when >= 0
    };

    bool BrushFaceUsable( const selbrush_t *b, int f )
    {
        if ( !b || !b->def || b->def->patch || !b->def->faces || f < 0 || f >= b->def->faceCount )
            return false;
        const winding_t *w = b->def->faces[f].w;
        return w && w->numpoints >= 3;
    }

    // KIWI (2026-09-28, user: "if I have an object selected, and I press space, currently it
    // fails to lock the camera ... find the closest face to the camera and lock to that plane
    // instead"): among the selected brushes, the camera-facing face whose centre is nearest
    // the eye; a camera inside every brush takes the nearest face of any facing.
    bool NearestSelectedFace( selbrush_t **outBrush, int *outFace )
    {
        float eye[3];
        KiwiCam_MarkerViewpoint( eye );
        float bestFront = 1.0e30f, bestAny = 1.0e30f;
        selbrush_t *frontB = nullptr, *anyB = nullptr;
        int         frontF = -1,      anyF = -1;
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
        {
            if ( !b->def || ( (unsigned)b->brushFlags & KFOC_HIDDEN_BIT ) != 0 )
                continue;
            for ( int f = 0; b->def->faces && f < b->def->faceCount; ++f )
            {
                if ( !BrushFaceUsable( b, f ) )
                    continue;
                const winding_t *w = b->def->faces[f].w;
                float c[3] = { 0.0f, 0.0f, 0.0f };
                for ( int i = 0; i < w->numpoints; ++i )
                    for ( int k = 0; k < 3; ++k )
                        c[k] += w->p[i][k];
                for ( int k = 0; k < 3; ++k )
                    c[k] /= (float)w->numpoints;
                float toEye[3];
                Sub3( eye, c, toEye );
                const float d = Len3( toEye );
                if ( d < bestAny ) { bestAny = d; anyB = b; anyF = f; }
                if ( Dot3( b->def->faces[f].plane.normal, toEye ) > 0.0f && d < bestFront )
                {
                    bestFront = d; frontB = b; frontF = f;
                }
            }
        }
        *outBrush = frontB ? frontB : anyB;
        *outFace  = frontB ? frontF : anyF;
        return *outBrush != nullptr;
    }

    // The object click Space aims from (KiwiFocus_NoteObjectClick).
    struct objectClick_t
    {
        selbrush_t *brush     = nullptr;
        float       point[3]  = { 0.0f, 0.0f, 0.0f };
        float       normal[3] = { 0.0f, 0.0f, 1.0f };
        bool        haveNormal = false;
    };
    objectClick_t s_click;

    bool BrushSelected( const selbrush_t *b )
    {
        for ( const selbrush_t *s = selected_brushes.next; s != &selected_brushes; s = s->next )
            if ( s == b )
                return true;
        return false;
    }

    // The clicked brush's face under the recorded hit: the face whose plane holds the point
    // (nearest plane, normal agreeing with the hit normal when the pick supplied one).
    bool ClickedFace( selbrush_t **outBrush, int *outFace )
    {
        selbrush_t *b = s_click.brush;
        if ( !b || !Sel_BrushLive( b ) || !BrushSelected( b ) || !b->def || b->def->patch
          || !b->def->faces )
            return false;
        float best = 1.0e30f;
        int   bestF = -1;
        for ( int f = 0; f < b->def->faceCount; ++f )
        {
            if ( !BrushFaceUsable( b, f ) )
                continue;
            const plane_t &pl = b->def->faces[f].plane;
            if ( s_click.haveNormal && Dot3( pl.normal, s_click.normal ) < 0.5f )
                continue;
            const float d = fabsf( Dot3( pl.normal, s_click.point ) - pl.dist );
            if ( d < best ) { best = d; bestF = f; }
        }
        if ( bestF < 0 || best > 2.0f )     // the brush moved away from the click
            return false;
        *outBrush = b;
        *outFace  = bestF;
        return true;
    }

    bool SelectedBrushHasFaces()
    {
        for ( selbrush_t *b = selected_brushes.next; b != &selected_brushes; b = b->next )
            if ( b->def && !b->def->patch && b->def->faces && b->def->faceCount > 0 )
                return true;
        return false;
    }

    bool RegionUsable( int r )
    {
        return r >= 0 && r < (int)KiwiRegion_All().size() && KiwiRegion_All()[r].pts.size() >= 6;
    }

    // Selected before hovered, faces before regions of the same standing: the active
    // selected face, another selected face, a selected region, the hovered face or region,
    // then (cheap test only when `resolveNearest` is false) the selected objects' nearest face.
    bool FaceUnderVerb( faceTarget_t *out, bool resolveNearest )
    {
        const selection_t &sel = KiwiSel();
        if ( sel.active.kind == SEL_FACE && Sel_ItemValid( sel.active ) )
        {
            out->brush = sel.active.brush;
            out->face  = sel.active.faceIndex;
            return true;
        }
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            if ( sel.items[i].kind == SEL_FACE && Sel_ItemValid( sel.items[i] ) )
            {
                out->brush = sel.items[i].brush;
                out->face  = sel.items[i].faceIndex;
                return true;
            }
        }
        const int selRegion = KiwiRegion_SelectedIndex();
        if ( RegionUsable( selRegion ) )
        {
            out->region = selRegion;
            return true;
        }
        // Hover is the fallback, not the primary selection source.
        const pick_result_t &hov = KiwiHover_Get();
        if ( hov.valid && hov.item.kind == SEL_FACE && Sel_ItemValid( hov.item ) )
        {
            out->brush = hov.item.brush;
            out->face  = hov.item.faceIndex;
            return true;
        }
        const int hovRegion = KiwiHover_RegionIndex();
        if ( RegionUsable( hovRegion ) )
        {
            out->region = hovRegion;
            return true;
        }
        if ( !resolveNearest )
            return SelectedBrushHasFaces();
        // The face the selecting click landed on, else the nearest camera-facing face.
        if ( ClickedFace( &out->brush, &out->face ) )
            return true;
        return NearestSelectedFace( &out->brush, &out->face );
    }

    // pitch / yaw looking along `d` (unit), in the camera's degree convention
    // (vpn = (cos p cos y, cos p sin y, sin p)).
    void LookDir( const float d[3] )
    {
        float dz = d[2];
        if ( dz >  1.0f ) dz =  1.0f;
        if ( dz < -1.0f ) dz = -1.0f;
        const float pitch = (float)( asin( (double)dz )      * KIWI_RAD2DEG );
        const float yaw   = (float)( atan2( (double)d[1], (double)d[0] ) * KIWI_RAD2DEG );
        KiwiCam_LookAlong( pitch, yaw );
    }

    // A construction region: look at it from the side the camera is on, frame its loop and
    // make its plane the drawing plane (the normal turned toward the camera, v flipped with
    // it so u x v stays the normal).
    void ViewRegion( int index )
    {
        const kregion_t &r = KiwiRegion_All()[index];
        kconPlane_t p = r.plane;
        float eye[3], rel[3];
        KiwiCam_MarkerViewpoint( eye );
        Sub3( eye, p.origin, rel );
        if ( Dot3( p.normal, rel ) < 0.0f )
            for ( int k = 0; k < 3; ++k )
            {
                p.normal[k] = -p.normal[k];
                p.v[k]      = -p.v[k];
            }
        box_t box;
        const int n = (int)( r.pts.size() / 2 );
        for ( int i = 0; i < n; ++i )
        {
            float w[3];
            KiwiCon_PlaneToWorld( r.plane, &r.pts[(size_t)i * 2], w );
            box.Add( w );
        }
        if ( !box.any )
            return;
        const float d[3] = { -p.normal[0], -p.normal[1], -p.normal[2] };
        LookDir( d );
        KiwiCam_FrameBounds( box.lo, box.hi );
        {
            // KIWI: in perspective, stop the eye in front of whatever is between it and the face.
            const float c[3] = { ( box.lo[0] + box.hi[0] ) * 0.5f, ( box.lo[1] + box.hi[1] ) * 0.5f,
                                 ( box.lo[2] + box.hi[2] ) * 0.5f };
            KiwiCam_PullInFrontOf( c, p.normal );
        }
        KiwiCon_SetActivePlane( p );
        KiwiCon_MarkPlaneExplicit( "a construction face (Space)" );
        Sys_Printf( "View Face: looking at the construction face head-on; it is now the plane "
                    "for the SHAPE tools (normal %.2f %.2f %.2f).  Space over empty space "
                    "clears it.\n", p.normal[0], p.normal[1], p.normal[2] );
    }
}

void KiwiFocus_NoteObjectClick( selbrush_t *brush, const float point[3], const float *normal )
{
    s_click = objectClick_t();
    if ( !brush || !point )
        return;
    s_click.brush = brush;
    Copy3( point, s_click.point );
    if ( normal )
    {
        Copy3( normal, s_click.normal );
        s_click.haveNormal = true;
    }
}

bool KiwiFocus_ClickedOrNearestFace( selbrush_t **outBrush, int *outFace )
{
    if ( !outBrush || !outFace )
        return false;
    if ( ClickedFace( outBrush, outFace ) )
        return true;
    return NearestSelectedFace( outBrush, outFace );
}

void KiwiFocus_RegisterCommands()
{
    // Register unbound classic rows; the modern profile assigns "/" and Space.
    Radiant_RegisterCommand( "KiwiFocusSelection", 0, 0, KIWI_CMD_FOCUS_SELECTION );
    Radiant_RegisterCommand( "KiwiViewFace", 0, 0, KIWI_CMD_VIEW_FACE );
}

bool KiwiFocus_CanViewFace()
{
    faceTarget_t t;
    return FaceUnderVerb( &t, false );       // per-frame palette predicate: no face walk
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

    faceTarget_t target;
    if ( !FaceUnderVerb( &target, true ) )
    {
        // With no selected or hovered face, Space clears an explicit plane.
        // Esc remains selection-only; a construction plane is not a selection.
        if ( KiwiCon_PlaneIsExplicit() )
        {
            KiwiCon_ClearPlaneToDefault();
            return true;
        }
        Sys_Printf( "View Face: nothing to look at - select or hover a face, a "
                    "construction face, or an object (its face nearest the camera), then "
                    "press Space.\n" );
        return true;
    }
    if ( target.region >= 0 )
    {
        ViewRegion( target.region );
        return true;
    }
    selbrush_t *inst = target.brush;
    const int   fi   = target.face;
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
    LookDir( d );
    KiwiCam_FrameBounds( box.lo, box.hi );
    {
        // KIWI (2026-10-22): in perspective, trace out from the face's centre and stop the
        // eye in front of anything nearer than the framed distance (a room's far wall).
        float centre[3] = { 0.0f, 0.0f, 0.0f };
        for ( int i = 0; i < w->numpoints; ++i )
            for ( int k = 0; k < 3; ++k )
                centre[k] += w->p[i][k] / (float)w->numpoints;
        KiwiCam_PullInFrontOf( centre, n );
    }

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

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
#include "kiwi_hover.h"                     // ROUND BK — KiwiHover_Get (the face fallback)
#include "kiwi_selection.h"                 // ROUND BK — KiwiSel / Sel_ItemValid
#include "kiwi_skybox.h"                    // ROUND BB — KiwiSky_IsSkyBrush

#include <math.h>                           // ROUND BK — asin / atan2 (the aim)

// ── ported entry points (each verified against its DEFINITION) ─────────────
//   win_qe3.cpp:112        int  Sys_Printf( const char *fmt, ... )
//   select.cpp:2073        void Select_GetBounds( float *mins, float *maxs )
//   qe3.h:1054             selbrush_t selected_brushes;   // 0x23f1864
//   qe3.h:1053             selbrush_t active_brushes;     // 0x23f189c
//   qe3.h:484/485          brush_t::mins / ::maxs  (vec3_t @0x20 / @0x2C)
//   mainfrm.cpp:1340       bool Radiant_RegisterCommand( const char *name, byte vk,
//                                                        byte mods, int commandId )
extern int  Sys_Printf( const char *fmt, ... );
extern void Select_GetBounds( float *mins, float *maxs );
extern bool Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId );

namespace
{
    // The hidden bit — the same one kiwi_visibility.cpp and the three ported hide
    // cores use (select.cpp:4185).
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

    // ═════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BB, ITEM 1) — FRAME-ALL DOES NOT FRAME THE SKY SHELL
    // ═════════════════════════════════════════════════════════════════════════
    // USER REPORT (round BA shipped): *"skybox still doesn't show, even when
    // manuvering the camera into the hull"*, with the user's own diagnosis
    // *"i think with plasticity style controls, it's impossible to fly into this
    // skybox cube"*.  This function is one of the two halves of why.
    //
    // A SKY SHELL IS, BY CONSTRUCTION, THE BIGGEST THING IN THE MAP — it is the
    // map bounds plus a margin plus a wall thickness (kiwi_skybox.cpp CreateShell).
    // Framing it therefore sets KiwiCam_FrameBounds' `radius` from the SHELL, and
    // the distance it solves for is the distance at which the whole shell fits on
    // screen — i.e. a camera that is OUTSIDE the shell by definition, every time
    // the user presses "/".  Since the eye is rigidly leashed s_dist behind the
    // pivot (kiwi_camera.cpp KiwiCam_Translate), that is a hole the arrow keys
    // cannot climb out of.
    //
    // ONLY THE FALLBACK ARM SKIPS IT, and that is the same rule the hidden bit
    // already follows two lines down: an EXPLICIT selection is framed whatever it
    // contains, because the user named it.  Select a sky brush and press "/" and it
    // is framed exactly as before — this arm is only reached with nothing selected.
    //
    // AND IT IS NOT ALLOWED TO EMPTY THE BOX.  A map that is nothing but sky
    // brushes (a shell built into an empty map — a documented use of the verb) would
    // otherwise make "/" print *"nothing to look at"*, which is a worse answer than
    // framing the shell.  So the skip is a first pass and the sky is added back when
    // it was everything there was.
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

    // Plasticity's `visibleObjects` fallback: everything that is on screen.
    void AddVisibleWorld( box_t *box )
    {
        if ( !AddWorldBrushes( box, true ) )
            AddWorldBrushes( box, false );      // the map WAS only sky — frame that

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
    //    into it (select.cpp:2075-2086), so with an empty selection it comes back
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

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND BK, ITEM 7) — SPACE: THE CAMERA GOES IN FRONT OF THE FACE
// ═════════════════════════════════════════════════════════════════════════════
// USER DIRECTIVE, verbatim: *"Pressing [Space] on a face should mimic what
// Plasticity does.  It moves the camera in front of that face (similar to /)."*
//
// This is the verb kiwi_focus.h's own header says was NOT shipped in round J —
// Plasticity's `viewport:navigate:selection` on space (default-keymap.ts:231),
// which REORIENTS the camera onto the selection's plane, as opposed to `/`'s
// `viewport:focus`, which re-targets and dollies without ever turning.  Round J
// declined it for one reason, quoted from that header: *"mapping it onto the
// SPACE key would collide head-on with classic Radiant's Clone (33001)"*.  The
// user has now asked for the key by name, so the collision is RESOLVED rather
// than avoided — see kiwi_keymap.cpp, which moves Clone to Shift+Space in the
// MODERN profile only.
//
// WHAT IT IS NOT.  Plasticity's version also swaps the active CONSTRUCTION PLANE
// and can flip the projection.  KIWI has five explicit construction-plane commands
// (KIWI_CMD_CPLANE_*) and a projection pill, and silently reaching into either
// from a view key would be a surprise; so this moves the camera and nothing else.
// That is also the half the directive describes ("moves the camera in front of
// that face").
namespace
{
    // The face this verb acts on: the SELECTED one (the active item first, so a
    // multi-face selection uses the one the user clicked last), falling back to
    // the one under the cursor.  False = there is no face to look at.
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
        // HOVER is the fallback, not the primary: "pressing space ON a face" reads
        // as the thing under the cursor, and in mode 3 a face is usually both.
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

// ─── commands ────────────────────────────────────────────────────────────────
void KiwiFocus_RegisterCommands()
{
    // Unbound here — the CLASSIC-profile row.  kiwi_keymap.cpp puts it on the bare
    // "/" (vk 0xBF, mods 0) in the modern profile, which is Plasticity's own key
    // and is free in the default table; the audit is in kiwi_keymap.h.
    Radiant_RegisterCommand( "KiwiFocusSelection", 0, 0, KIWI_CMD_FOCUS_SELECTION );
    // ROUND BK, ITEM 7 — same rule: unbound here, Space in the modern profile.
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
        // ── KIWI-UX (ROUND BP, ITEM 3): SPACE ON NOTHING CLEARS THE PLANE ────
        // The ruling asks for one key to establish a working plane and for a way
        // back to the default, and to "pick the least surprising, state it".  This
        // is it: SPACE ON A FACE sets the plane (and flies the camera), SPACE ON
        // NOTHING clears it.  No new binding, no modifier, and the discoverability
        // was already here — this is the message the user hits when they press
        // Space with nothing under the cursor, so it is the message that teaches
        // the clear.  (Esc is deliberately NOT it: Esc means "drop the selection"
        // everywhere in this editor and a working plane is not a selection — the
        // same argument kiwi_section.h makes for the section toggle.)
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

    // The face's own AABB — what `/` would frame if a face had a bounding box —
    // so the standoff comes out of the SAME KiwiCam_FrameBounds fit round J
    // derived, margin and radius floor included.  No second framing rule.
    box_t box;
    for ( int i = 0; i < w->numpoints; ++i )
        box.Add( w->p[i] );
    if ( !box.any )
        return true;

    // AIM FIRST, FRAME SECOND, and that order is load-bearing: KiwiCam_LookAlong
    // re-aims about the CURRENT pivot (it does not re-target), and
    // KiwiCam_FrameBounds preserves the DIRECTION while it re-targets and dollies.
    // Composed in that order the two existing public mutators are exactly "look
    // along -normal at the face, from far enough away to see it".
    //
    // THE ANGLES.  kiwi_camera.cpp's ANGLE CONVENTION note gives
    // `vpn = ( cos p cos y, cos p sin y, sin p )`, so for a wanted view direction
    // d (== -normal, i.e. looking INTO the face):
    //     pitch = asin( d.z ),  yaw = atan2( d.y, d.x )
    // LookAlong bounds the pitch at +-90, so a FLOOR or CEILING face is viewed
    // from EXACTLY overhead — the same pole-exact snap the view cube's top/bottom
    // take.  The yaw the pole keeps is this face's own atan2, which at a pole is
    // atan2(0,0) = 0; the view is square to the face either way.
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
    const float pitch = (float)( asin( (double)dz )      * 57.295779513082320876 );
    const float yaw   = (float)( atan2( (double)d[1], (double)d[0] ) * 57.295779513082320876 );

    KiwiCam_LookAlong( pitch, yaw );
    KiwiCam_FrameBounds( box.lo, box.hi );

    // ══════════════════════════════════════════════════════════════════════════
    //  KIWI-UX (ROUND BP, ITEM 3) — …AND IT SETS THE WORKING PLANE
    // ══════════════════════════════════════════════════════════════════════════
    // USER RULING, verbatim: *"construction planes should only be made with
    // [space], usually, the current behavior is too strict.  That's what it is."*
    //
    // ONE KEY, BOTH EFFECTS, which is the Plasticity shape this verb was already
    // copying: their navigate-to-selection sets `this.constructionPlane = to.cplane`
    // in the same act as the camera move (Viewport.tsx:551-556, quoted at
    // kiwi_viewcube.cpp:1443).  Round BK took the camera half and left the plane
    // half to a ladder of inferences; the ladder is what produced the reported
    // teleports, so the plane half moves here where the user asked for it.
    //
    // The plane is built at the face's WINDING CENTRE with its own normal and its
    // longest edge as the u hint — the identical construction
    // KiwiCon_SetPlaneFromSelectedFace makes, reached through the same public
    // entry point so the two can never drift.  Marked EXPLICIT, so nothing at a
    // tool start may replace it and the PLANE chip states it for as long as it
    // stands.
    {
        if ( KiwiCon_SetPlaneFromFace( inst, fi, false ) )
        {
            KiwiCon_MarkPlaneExplicit( "a face (Space)" );
            // KIWI-UX (ROUND BS): say WHICH tools it is a plane for.  Since this
            // round the line, polyline and spline tools place at the snap, else the
            // ray's surface hit, else the ground, and do not read a plane at all —
            // so promising "the drawing plane" to a user about to draw a line would
            // be promising something that no longer happens.  The camera flight, and
            // the plane for the PLANAR shapes, are both unchanged.
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

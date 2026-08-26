#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Patch-vertex mode; see kiwi_patchverts.h for the V binding and lifecycle.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                 // camera_s

#include "kiwi_patchverts.h"
#include "kiwi_camera.h"             // KiwiCam_WorldPerPixel (screen-constant markers)
#include "kiwi_hover.h"              // shared hover pick and accent
#include "kiwi_lines.h"
#include "kiwi_pick.h"               // pick_result_t
#include "kiwi_selection.h"

#include <math.h>
#include <stdio.h>
#include <vector>

// Ed_Camera always returns &g_camwndState.camera (camwnd.cpp:159).
extern camera_s  *Ed_Camera();                       // camwnd.cpp:161
extern void       CamWnd_BuildMatrix();              // camwnd.cpp 0x403470
extern int        g_nUpdateBits;                     // mainfrm.cpp
extern selbrush_t selected_brushes;                  // brush.cpp
extern int        Sys_Printf( const char *fmt, ... );  // win_qe3.cpp

namespace
{
    // Bound multi-patch overlays; 6000 segments covers the common one/two-patch case.
    enum { KPV_MAX_PATCHES  = 16 };
    enum { KPV_MAX_SEGMENTS = 6000 };
    const float KPV_MARKER_PX = 4.0f;

    // Dim scaffolding, bright points, and the warm active-selection accent.
    const float KPV_LATTICE[3]  = { 0.30f, 0.55f, 0.70f };
    const float KPV_POINT[3]    = { 0.45f, 0.85f, 1.00f };
    const float KPV_SELECTED[3] = { 1.00f, 0.80f, 0.25f };
    // Hover is larger as a second cue beyond its shared cyan accent.
    const float KPV_HOVER[3]    = { 0.35f, 0.95f, 1.00f };
    const float KPV_HOVER_PX    = 6.5f;

    bool                      s_active   = false;
    sel_mask_t                s_prevMask = SEL_MASK_OBJECT;
    std::vector<selbrush_t *> s_patches;
    char                      s_status[192] = { 0 };
    // Gate lifecycle selection walks on actual changes.
    unsigned                  s_selGen   = 0;

    patchMesh_t *PatchOf( selbrush_t *b )
    {
        if ( !Sel_BrushLive( b ) || !b->patch || !b->def )
            return 0;
        // Ported patch consumers use the DEF spelling; pmesh.cpp:3851 asserts agreement.
        patchMesh_t *pm = b->def->patch;
        if ( !pm || pm->width <= 0 || pm->height <= 0 || pm->width > 16 || pm->height > 16 )
            return 0;
        return pm;
    }

    // Include each live patch named by the current selection.
    void GatherPatches( std::vector<selbrush_t *> &out )
    {
        out.clear();
        // Report patches omitted by the hard cap once per mode entry.
        int overflow = 0;
        const selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            selbrush_t *b = sel.items[i].brush;
            if ( !PatchOf( b ) )
                continue;
            bool dup = false;
            for ( size_t k = 0; k < out.size() && !dup; ++k )
                dup = ( out[k] == b );
            if ( dup )
                continue;
            if ( (int)out.size() >= KPV_MAX_PATCHES )
            {
                ++overflow;                   // count omissions for the entry warning
                continue;
            }
            out.push_back( b );
        }
        // Also scan the legacy selected-brush list, deduplicating against KiwiSel().
        for ( selbrush_t *b = selected_brushes.next;
              b && b != &selected_brushes; b = b->next )
        {
            if ( !PatchOf( b ) )
                continue;
            bool dup = false;
            for ( size_t k = 0; k < out.size() && !dup; ++k )
                dup = ( out[k] == b );
            if ( dup )
                continue;
            if ( (int)out.size() >= KPV_MAX_PATCHES )
            {
                ++overflow;                   // count omissions for the entry warning
                continue;
            }
            out.push_back( b );
        }

        if ( overflow )
            Sys_Printf( "Patch vertex mode: %i selected patch(es) beyond the "
                        "KPV_MAX_PATCHES limit of %i were left out — their control "
                        "points are not editable in this session.\n",
                        overflow, (int)KPV_MAX_PATCHES );
    }

    // Nudge 0.25 world units toward the eye to avoid depth-fighting the patch surface.
    void AddNudged( const camera_s *c, const float *a, const float *b )
    {
        const float n = 0.25f;
        float na[3], nb[3];
        for ( int k = 0; k < 3; ++k )
        {
            na[k] = a[k] - c->vpn[k] * n;
            nb[k] = b[k] - c->vpn[k] * n;
        }
        KiwiLines_Add( na, nb );
    }

    // A screen-constant box marker, exactly kiwi_hover.cpp's EmitPointMarker.
    void EmitMarkerPx( const camera_s *c, const float *p, float px )
    {
        const float h = KiwiCam_WorldPerPixel( p ) * px;
        float corner[4][3];
        const float sx[4] = { -1.0f,  1.0f,  1.0f, -1.0f };
        const float sy[4] = { -1.0f, -1.0f,  1.0f,  1.0f };
        for ( int i = 0; i < 4; ++i )
            for ( int k = 0; k < 3; ++k )
                corner[i][k] = p[k] + c->vright[k] * ( sx[i] * h )
                                    + c->vup[k]    * ( sy[i] * h );
        for ( int i = 0; i < 4; ++i )
            AddNudged( c, corner[i], corner[( i + 1 ) & 3] );
    }

    void EmitMarker( const camera_s *c, const float *p )
    {
        EmitMarkerPx( c, p, KPV_MARKER_PX );
    }

    bool PointSelected( selbrush_t *b, int ctrlIndex )
    {
        const selection_t &sel = KiwiSel();
        for ( size_t i = 0; i < sel.items.size(); ++i )
        {
            const sel_item_t &it = sel.items[i];
            // Patch points use faceIndex = -1 and vertIndex = col * height + row.
            if ( it.kind == SEL_VERTEX && it.brush == b
              && it.faceIndex < 0 && it.vertIndex == ctrlIndex )
                return true;
        }
        return false;
    }

    // Share the cursor pick so hover and click target the same control point.
    bool PointHovered( selbrush_t *b, int ctrlIndex )
    {
        const pick_result_t &h = KiwiHover_Get();
        return h.valid && h.item.kind == SEL_VERTEX && h.item.brush == b
            && h.item.faceIndex < 0 && h.item.vertIndex == ctrlIndex;
    }

    void UpdateStatus()
    {
        if ( !s_active )
        {
            s_status[0] = '\0';
            return;
        }
        int points = 0;
        for ( size_t i = 0; i < s_patches.size(); ++i )
            if ( patchMesh_t *pm = PatchOf( s_patches[i] ) )
                points += pm->width * pm->height;
        int sel = 0;
        const selection_t &s = KiwiSel();
        for ( size_t i = 0; i < s.items.size(); ++i )
            if ( s.items[i].kind == SEL_VERTEX && s.items[i].faceIndex < 0 )
                ++sel;
        _snprintf( s_status, sizeof( s_status ),
                   "patch vertex mode  %i point(s) on %i patch(es)  %i selected  ·  "
                   "click a point (Shift adds, Ctrl removes, drag a box takes several), then G "
                   "or the gizmo  ·  V / Esc: leave",
                   points, (int)s_patches.size(), sel );
        s_status[sizeof( s_status ) - 1] = '\0';
    }

    // Do not restore a user-chosen mask or reselect over another selection.
    // `quiet` is for Cam_Draw, where selection mutation would relink lists mid-walk.
    void ExitInternal( bool restoreMask, bool reselect, const char *why, bool quiet = false )
    {
        if ( !s_active )
            return;
        s_active = false;

        if ( restoreMask )
            KiwiSel_SetModeMask( s_prevMask );

        if ( reselect && !quiet )
        {
            // Toggle-off returns the latched patches as whole-object selection.
            selection_t &sel = KiwiSel();
            Sel_Clear( sel );
            for ( size_t i = 0; i < s_patches.size(); ++i )
                if ( PatchOf( s_patches[i] ) )
                    Sel_Add( sel, Sel_MakeObject( s_patches[i] ) );
            Sel_SyncToLegacy();
        }

        s_patches.clear();
        s_status[0] = '\0';
        s_selGen    = 0;          // reset the lifecycle gate
        if ( quiet )
            return;               // Cam_Draw-safe teardown stops here
        Sys_Printf( "Patch vertex mode: off%s%s.\n", why ? " — " : "", why ? why : "" );
        g_nUpdateBits = -1;
    }
}

bool KiwiPatchVerts_OwnsPatch( const selbrush_t *b )
{
    if ( !s_active || !b )
        return false;
    for ( size_t i = 0; i < s_patches.size(); ++i )
        if ( s_patches[i] == b )
            return true;
    return false;
}

bool KiwiPatchVerts_Active()
{
    return s_active;
}

const char *KiwiPatchVerts_Status()
{
    return ( s_active && s_status[0] ) ? s_status : 0;
}

void KiwiPatchVerts_Exit()
{
    // Restore the previous mask and return the latched patches as objects.
    ExitInternal( true, true, 0 );
}

bool KiwiPatchVerts_HandleEscape()
{
    if ( !s_active )
        return false;
    // Escape leaves patches selected as objects; a second Escape deselects them.
    ExitInternal( true, true, "Esc" );
    return true;
}

void KiwiPatchVerts_Update()
{
    if ( !s_active )
        return;

    // A new mode is the user's choice; leave without restoring the old mask.
    if ( KiwiSel_GetModeMask() != SEL_MASK_VERTEX )
    {
        ExitInternal( false, false, "the selection mode changed" );
        return;
    }

    // Leave when no latched patch remains live.
    bool anyLive = false;
    for ( size_t i = 0; i < s_patches.size() && !anyLive; ++i )
        anyLive = ( PatchOf( s_patches[i] ) != 0 );
    if ( !anyLive )
    {
        ExitInternal( true, false, "the patch is gone" );
        return;
    }

    // KiwiSel() may rebuild and bump the generation, so read the generation second.
    const selection_t &sel = KiwiSel();
    const unsigned     gen = Sel_Generation();
    if ( gen == s_selGen )
        return;
    s_selGen = gen;

    // Empty selection only drops selected points; it does not leave the mode.
    for ( size_t i = 0; i < sel.items.size(); ++i )
    {
        const sel_item_t &it = sel.items[i];
        if ( KiwiPatchVerts_OwnsPatch( it.brush ) )
            continue;

        // The vertex mask is global: adopt a picked point's live patch when possible.
        // Any other unowned selection item exits the mode.
        if ( it.kind == SEL_VERTEX && it.faceIndex < 0 && PatchOf( it.brush )
          && (int)s_patches.size() < KPV_MAX_PATCHES )
        {
            s_patches.push_back( it.brush );
            Sys_Printf( "Patch vertex mode: now editing %i patch(es).\n",
                        (int)s_patches.size() );
            g_nUpdateBits = -1;
            continue;
        }

        ExitInternal( true, false, "the selection moved off the patch" );
        return;
    }
}

bool KiwiPatchVerts_ToggleForSelection()
{
    if ( s_active )
    {
        KiwiPatchVerts_Exit();
        return true;
    }

    std::vector<selbrush_t *> found;
    GatherPatches( found );
    if ( found.empty() )
        return false;                  // no patch — the classic 33005 path owns V

    s_patches  = found;
    s_prevMask = KiwiSel_GetModeMask();
    s_active   = true;

    // SEL_MASK_VERTEX routes picking and marquee to patch points; kiwi_transform's
    // vertex path supplies gizmo movement, snap, cancel restore, and undo.
    KiwiSel_SetModeMask( SEL_MASK_VERTEX );

    // Drop object items so the first click selects a point; s_patches keeps draw scope.
    selection_t &sel = KiwiSel();
    Sel_Clear( sel );
    Sel_SyncToLegacy();
    // Adopt the clear's generation so Update does not reprocess our own entry.
    s_selGen = Sel_Generation();

    UpdateStatus();
    Sys_Printf( "Patch vertex mode: %i patch(es).  Click a control point and drag "
                "the move gizmo (or press G).  SHIFT-CLICK adds points, CTRL-CLICK "
                "removes one, and a DRAGGED "
                "BOX takes every point inside it — the gizmo then moves them all "
                "together, as one undo record.  V or Esc leaves.\n",
                (int)s_patches.size() );
    g_nUpdateBits = -1;
    return true;
}

void KiwiPatchVerts_DrawWorld()
{
    if ( !s_active )
        return;

    // If every patch is gone, quietly restore the mask and drop the latch.
    // Cam_Draw is walking brush lists here, so selection sync must not relink them.
    bool anyLive = false;
    for ( size_t i = 0; i < s_patches.size() && !anyLive; ++i )
        anyLive = ( PatchOf( s_patches[i] ) != 0 );
    if ( !anyLive )
    {
        ExitInternal( true, false, 0, true );
        return;
    }

    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;
    CamWnd_BuildMatrix();

    UpdateStatus();

    // One color keeps the control lattice in a single material-color run.
    KiwiLines_Begin( KPV_MAX_SEGMENTS, 1 );
    KiwiLines_Color( KPV_LATTICE[0], KPV_LATTICE[1], KPV_LATTICE[2] );
    for ( size_t i = 0; i < s_patches.size(); ++i )
    {
        patchMesh_t *pm = PatchOf( s_patches[i] );
        if ( !pm )
            continue;
        for ( int col = 0; col < pm->width; ++col )
            for ( int row = 0; row < pm->height; ++row )
            {
                if ( KiwiLines_Remaining() <= 0 )
                    break;
                if ( col + 1 < pm->width )
                    AddNudged( c, pm->ctrl[col][row].xyz, pm->ctrl[col + 1][row].xyz );
                if ( row + 1 < pm->height )
                    AddNudged( c, pm->ctrl[col][row].xyz, pm->ctrl[col][row + 1].xyz );
            }
    }
    KiwiLines_Flush();

    // Draw plain, selected, then hover markers so later accents win overlaps.
    // Hover is also larger, providing a cue beyond color.
    KiwiLines_Begin( KPV_MAX_SEGMENTS, 2 );
    for ( int pass = 0; pass < 3; ++pass )
    {
        if ( pass == 0 )      KiwiLines_Color( KPV_POINT[0],    KPV_POINT[1],    KPV_POINT[2] );
        else if ( pass == 1 ) KiwiLines_Color( KPV_SELECTED[0], KPV_SELECTED[1], KPV_SELECTED[2] );
        else if ( KiwiHover_RemovePreview() )
            KiwiLines_Color( KPV_SELECTED[0], KPV_SELECTED[1], KPV_SELECTED[2] );
        else
            KiwiLines_Color( KPV_HOVER[0], KPV_HOVER[1], KPV_HOVER[2] );

        for ( size_t i = 0; i < s_patches.size(); ++i )
        {
            selbrush_t  *b  = s_patches[i];
            patchMesh_t *pm = PatchOf( b );
            if ( !pm )
                continue;
            for ( int col = 0; col < pm->width; ++col )
                for ( int row = 0; row < pm->height; ++row )
                {
                    if ( KiwiLines_Remaining() <= 0 )
                        break;
                    // Match kiwi_pick's col * height + row index, including non-square grids.
                    const int idx = col * pm->height + row;
                    const bool hovered = PointHovered( b, idx );
                    if ( pass == 2 )
                    {
                        if ( !hovered )
                            continue;
                        EmitMarkerPx( c, pm->ctrl[col][row].xyz, KPV_HOVER_PX );
                        continue;
                    }
                    if ( hovered )
                        continue;                       // the hover run owns it
                    if ( PointSelected( b, idx ) != ( pass == 1 ) )
                        continue;
                    EmitMarker( c, pm->ctrl[col][row].xyz );
                }
        }
    }
    KiwiLines_Flush();
}

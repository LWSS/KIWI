#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_patchverts.cpp — RADIANT_UX_DESIGN §62.6.  See kiwi_patchverts.h for the
// whole design note, the V-binding rule and the three kills it works around.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"                 // camera_s

#include "kiwi_patchverts.h"
#include "kiwi_camera.h"             // KiwiCam_WorldPerPixel (screen-constant markers)
#include "kiwi_hover.h"              // ROUND AJ, ITEM 1 — the hover accent
#include "kiwi_lines.h"
#include "kiwi_pick.h"               // ROUND AJ, ITEM 1 — pick_result_t (KiwiHover_Get)
#include "kiwi_selection.h"

#include <math.h>
#include <stdio.h>
#include <vector>

// ── ported entry points (verified against their definitions) ────────────────
// KIWI-UX (CLEANUP, B-6): Ed_Camera returns &g_camwndState.camera and NEVER
// returns NULL (camwnd.cpp:159, and the contract note at :148-155 says so).  The
// deref at :422 is therefore correct as written; a `!c` guard would be dead code.
extern camera_s  *Ed_Camera();                       // camwnd.cpp:161
extern void       CamWnd_BuildMatrix();              // camwnd.cpp 0x403470
extern int        g_nUpdateBits;                     // mainfrm.cpp
extern selbrush_t selected_brushes;                  // brush.cpp
extern int        Sys_Printf( const char *fmt, ... );  // win_qe3.cpp

namespace
{
    // ── budget ───────────────────────────────────────────────────────────────
    // A 16x16 control grid is 15*16 + 16*15 = 480 lattice segments plus 256
    // markers at 6 segments each = 2016 — i.e. ONE fully populated patch is
    // already 2496 segments.  The mode is normally scoped to one or two patches,
    // so the cap is generous rather than tight, but it exists because the mode can
    // be entered on a multi-patch selection.
    enum { KPV_MAX_PATCHES  = 16 };
    enum { KPV_MAX_SEGMENTS = 6000 };
    const float KPV_MARKER_PX = 4.0f;

    // §18's language: the lattice is the scaffolding (dim), an unselected control
    // point is the same dim colour, and a SELECTED one takes the warm active
    // accent kiwi_hover.cpp spends on "this is the thing you are moving".
    const float KPV_LATTICE[3]  = { 0.30f, 0.55f, 0.70f };
    const float KPV_POINT[3]    = { 0.45f, 0.85f, 1.00f };
    const float KPV_SELECTED[3] = { 1.00f, 0.80f, 0.25f };
    // ROUND AJ, ITEM 1: §18's hover cyan — the same "this is the thing you are
    // pointing at" colour kiwi_hover.cpp and the marquee preview already use.  The
    // hovered marker is also drawn LARGER, because at 4 px a colour change alone is
    // not a hover accent (round AI, item 4's own finding: one channel is not two).
    const float KPV_HOVER[3]    = { 0.35f, 0.95f, 1.00f };
    const float KPV_HOVER_PX    = 6.5f;

    bool                      s_active   = false;
    sel_mask_t                s_prevMask = SEL_MASK_OBJECT;
    std::vector<selbrush_t *> s_patches;
    char                      s_status[192] = { 0 };
    // ROUND AJ, ITEM 1: the lifecycle's cheap gate — the selection walk runs only
    // when the selection actually changed.
    unsigned                  s_selGen   = 0;

    patchMesh_t *PatchOf( selbrush_t *b )
    {
        if ( !Sel_BrushLive( b ) || !b->patch || !b->def )
            return 0;
        // pmesh.cpp:3851 asserts the two spellings agree; read the DEF's, which is
        // the one every ported patch consumer uses.
        patchMesh_t *pm = b->def->patch;
        if ( !pm || pm->width <= 0 || pm->height <= 0 || pm->width > 16 || pm->height > 16 )
            return 0;
        return pm;
    }

    // Every patch named by the CURRENT selection, whether it is there as a whole
    // object or as one of its own control points — so re-entering the mode after a
    // point has been selected still finds the patch.
    void GatherPatches( std::vector<selbrush_t *> &out )
    {
        out.clear();
        // KIWI-UX (CLEANUP, B-38): the cap is announced.  A 17th selected patch was
        // dropped with no console line and no HUD note, and the status line then
        // read "N point(s) on 16 patch(es)" for a 20-patch selection — while the
        // ADOPTION path below prints when it widens the set, so the two halves of
        // one cap disagreed about whether the user is told.  Counted, then printed
        // ONCE at the end: GatherPatches is only reached from
        // KiwiPatchVerts_ToggleForSelection, i.e. once per Begin, never per frame.
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
                ++overflow;                   // KIWI-UX (CLEANUP, B-38)
                continue;
            }
            out.push_back( b );
        }
        // The legacy list too: a patch selected through a ported path (a menu
        // command, the XY pane) is on `selected_brushes` and may not have reached
        // KiwiSel() yet this frame.
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
                ++overflow;                   // KIWI-UX (CLEANUP, B-38)
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

    // The nudge every accent in this layer uses — 0.25 world units toward the eye,
    // so a lattice line lying ON the patch surface is not decided pixel-by-pixel
    // against that surface's depth (kiwi_hover.cpp's AddNudged, same constant).
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
            // kiwi_pick.h's indexing: a PATCH vertex is faceIndex = -1 with
            // vertIndex = col * height + row.
            if ( it.kind == SEL_VERTEX && it.brush == b
              && it.faceIndex < 0 && it.vertIndex == ctrlIndex )
                return true;
        }
        return false;
    }

    // ROUND AJ, ITEM 1: is this control point the one under the cursor?  Read off
    // the SHARED hover pick (kiwi_hover.cpp runs Pick() at the cursor every frame
    // under the live mode mask, which in this mode is SEL_MASK_VERTEX), so the
    // accent and the click can never disagree about which point is being aimed at.
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
        // ROUND AJ, ITEM 1: the line NAMES THE MULTI-POINT GRAMMAR.  Shift-add and
        // the marquee both worked before this round (kiwi_boxselect.cpp's click
        // grammar is Sel_Add under Shift and its SEL_VERTEX rect arm collects patch
        // control points) — nothing in the mode ever said so, and a grammar nobody
        // is told about is a grammar that does not exist.
        _snprintf( s_status, sizeof( s_status ),
                   "patch vertex mode  %i point(s) on %i patch(es)  %i selected  ·  "
                   "click a point (Shift adds, drag a box takes several), then G "
                   "or the gizmo  ·  V / Esc: leave",
                   points, (int)s_patches.size(), sel );
        s_status[sizeof( s_status ) - 1] = '\0';
    }

    // ── ROUND AJ, ITEM 1: the ONE exit, with its two independent questions ───
    // `restoreMask` — false only when the USER has just chosen a different mask
    // (putting the old one back would undo their keystroke).
    // `reselect`    — false when something else now owns the selection (the user
    // clicked another object, or the patches are gone); handing the patches back
    // as objects there would fight the selection that caused the exit.
    //
    // ── KIWI-UX (CLEANUP, B-33): AND ITS THIRD, `quiet` ─────────────────────
    // KiwiPatchVerts_DrawWorld used to inline a partial second teardown rather
    // than call this, because it runs INSIDE Cam_Draw, which is walking the two
    // brush sentinel lists — and this function's reselect arm ends in
    // Sel_SyncToLegacy -> Select_Deselect, which RELINKS brushes between them.
    // Re-entering that from the draw is the classic list-mutated-under-the-walker
    // crash.  The reasoning was right; the shape was not: the copy already missed
    // s_selGen, so the mode's state had two teardowns and one of them was already
    // wrong.  `quiet` skips the reselect arm ENTIRELY (not just Sel_SyncToLegacy —
    // Sel_Clear / Sel_Add are equally off-limits there) and the console line,
    // which is exactly what the draw path was hand-writing.
    void ExitInternal( bool restoreMask, bool reselect, const char *why, bool quiet = false )
    {
        if ( !s_active )
            return;
        s_active = false;

        if ( restoreMask )
            KiwiSel_SetModeMask( s_prevMask );

        if ( reselect && !quiet )
        {
            // Leaving the user with an empty selection after they press V would be
            // the wrong end of "toggle off": they were editing that patch and still
            // are, just not at point granularity any more.
            selection_t &sel = KiwiSel();
            Sel_Clear( sel );
            for ( size_t i = 0; i < s_patches.size(); ++i )
                if ( PatchOf( s_patches[i] ) )
                    Sel_Add( sel, Sel_MakeObject( s_patches[i] ) );
            Sel_SyncToLegacy();
        }

        s_patches.clear();
        s_status[0] = '\0';
        s_selGen    = 0;          // KIWI-UX (CLEANUP, B-33): the field the copy missed
        if ( quiet )
            return;               // no console line, no repaint request — see above
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
    // The deliberate exit (V again, a delete, a map load): restore the mask the
    // user was working in AND hand the patches back as whole objects.
    ExitInternal( true, true, 0 );
}

bool KiwiPatchVerts_HandleEscape()
{
    if ( !s_active )
        return false;
    // ONE LEVEL OUT, NOT A DESELECT.  Escape leaves the mode with the patches
    // selected as objects; a second Escape then reaches the ordinary 33002
    // UnSelectSelection (mainfrm.cpp:1144) and drops them.  Consuming the key is
    // what makes that two-step readable — an Escape that both left the mode and
    // cleared the selection would look like one press did two unrelated things.
    ExitInternal( true, true, "Esc" );
    return true;
}

void KiwiPatchVerts_Update()
{
    if ( !s_active )
        return;

    // ── TRIGGER 1: THE MODE MASK CHANGED ────────────────────────────────────
    // The mode OWNS SEL_MASK_VERTEX for as long as it is live (it is the one state
    // change that makes the whole thing work — see ToggleForSelection).  Anything
    // that moves the mask off it — 1..5, Ctrl+1..4, a command's own mask — is the
    // user choosing a granularity, so the mode stands down and does NOT put the
    // old mask back over the top of the one they just picked.
    if ( KiwiSel_GetModeMask() != SEL_MASK_VERTEX )
    {
        ExitInternal( false, false, "the selection mode changed" );
        return;
    }

    // ── TRIGGER 2: NOTHING LEFT TO EDIT ─────────────────────────────────────
    bool anyLive = false;
    for ( size_t i = 0; i < s_patches.size() && !anyLive; ++i )
        anyLive = ( PatchOf( s_patches[i] ) != 0 );
    if ( !anyLive )
    {
        ExitInternal( true, false, "the patch is gone" );
        return;
    }

    // ── TRIGGER 3: THE SELECTION MOVED OFF THE PATCH ────────────────────────
    // KiwiSel() first (it can rebuild from the legacy lists, which bumps the
    // generation), THEN read the generation, or the gate would latch a number the
    // rebuild is about to invalidate.
    const selection_t &sel = KiwiSel();
    const unsigned     gen = Sel_Generation();
    if ( gen == s_selGen )
        return;
    s_selGen = gen;

    // An EMPTY selection is NOT an exit: that is the "click empty space drops the
    // point selection" case, and the patch is still the thing being edited.
    for ( size_t i = 0; i < sel.items.size(); ++i )
    {
        const sel_item_t &it = sel.items[i];
        if ( KiwiPatchVerts_OwnsPatch( it.brush ) )
            continue;

        // ── ADOPTION, and it replaces round AI's "scoped at entry" limit ────
        // The pick mask is GLOBAL, so a control point on a patch the mode did not
        // latch is perfectly clickable — and until now the lattice simply did not
        // draw for it, which read as "that patch's points do not work".  A control
        // point of ANY live patch is by definition a statement that the user is
        // still doing patch vertex work, so the mode widens to include it instead
        // of standing down.  Anything else in the selection — an object, a face,
        // a brush vertex — is a statement that they are not, and exits.
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

    // ── THE ONE STATE CHANGE THAT MAKES IT WORK ─────────────────────────────
    // A pick-time mask, exactly as spec §1 requires ("modes are a pick-time mask,
    // never a post-conversion of an object hit").  With SEL_MASK_VERTEX in force,
    // kiwi_pick.cpp's screen-space scan answers with the nearest CONTROL POINT
    // within PICK_VERT_PIXELS (kiwi_pick.cpp:336-359) instead of the patch, the
    // marquee collects control points (kiwi_boxselect.cpp's SEL_VERTEX arm), and
    // everything downstream — the gizmo, G, the grid snap, the undo bracket, the
    // baseline restore on cancel — is the machinery kiwi_transform.cpp already
    // runs for a vertex selection, with its patch arm already written
    // (kiwi_transform.cpp:1912-1931 / :2735-2781).
    KiwiSel_SetModeMask( SEL_MASK_VERTEX );

    // Start from a CLEAN point selection.  The patch is dropped from the selection
    // here (it stays in s_patches, which is what the draw reads), so the first
    // click selects a point rather than adding one to a whole-object selection —
    // and so the ported selected-patch wireframe stops fighting this file's own
    // control-grid draw for the same pixels.
    selection_t &sel = KiwiSel();
    Sel_Clear( sel );
    Sel_SyncToLegacy();
    // ROUND AJ, ITEM 1: adopt the generation the clear just produced, so the very
    // first KiwiPatchVerts_Update does not read our own entry as "the selection
    // changed" and walk it for nothing.
    s_selGen = Sel_Generation();

    UpdateStatus();
    Sys_Printf( "Patch vertex mode: %i patch(es).  Click a control point and drag "
                "the move gizmo (or press G).  SHIFT-CLICK adds points and a DRAGGED "
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

    // Every patch gone (deleted, or a map load) — leave rather than draw nothing
    // forever with a stale mask in force.
    //
    // A *QUIET* exit, and that matters: this runs inside Cam_Draw, which is walking
    // `active_brushes` / `selected_brushes` right now.  The full
    // KiwiPatchVerts_Exit ends in Sel_SyncToLegacy → Select_Deselect, which RELINKS
    // brushes between those two sentinel lists — re-entering that from inside the
    // draw is the classic list-mutated-under-the-walker crash.  Restoring the mask
    // and dropping the latch is all that is needed here; there is nothing left to
    // re-select anyway, which is why we are exiting.
    //
    // KIWI-UX (CLEANUP, B-33): this WAS a hand-inlined second teardown that had
    // already drifted (it never cleared s_selGen).  ExitInternal's `quiet` flag
    // now expresses exactly the same restriction, so the mode has ONE teardown.
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

    // ── PASS 1: the control LATTICE, dim ────────────────────────────────────
    // One colour for the whole pass so Ed_EmitLineBatch emits exactly one
    // SetMaterialColor + DrawLines pair for it (kiwi_lines.h "Colour runs").
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

    // ── PASS 2: the point MARKERS, at handle weight ─────────────────────────
    // THREE colour runs inside one batch — every plain marker, then every selected
    // one, then the single HOVERED one — rather than one run per marker.
    //
    // ROUND AJ, ITEM 1 (b): the hover run is what makes an 8 px pick target
    // aimable.  It is drawn LAST so it wins the pixels where it overlaps either of
    // the other two, and LARGER (KPV_HOVER_PX) because at handle size a colour
    // swap alone is not a second channel.
    KiwiLines_Begin( KPV_MAX_SEGMENTS, 2 );
    for ( int pass = 0; pass < 3; ++pass )
    {
        if ( pass == 0 )      KiwiLines_Color( KPV_POINT[0],    KPV_POINT[1],    KPV_POINT[2] );
        else if ( pass == 1 ) KiwiLines_Color( KPV_SELECTED[0], KPV_SELECTED[1], KPV_SELECTED[2] );
        else                  KiwiLines_Color( KPV_HOVER[0],    KPV_HOVER[1],    KPV_HOVER[2] );

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
                    // kiwi_pick.h's index: col * height + row.  It MUST match, or
                    // the marker highlight and the pick would disagree about which
                    // point is which on a non-square grid.
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

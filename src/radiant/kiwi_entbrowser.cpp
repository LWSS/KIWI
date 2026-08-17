#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_entbrowser.cpp — ROUND AU implementation.  See kiwi_entbrowser.h for the
// user directive, the preview scoping, the drop mechanics and the ruling that the
// creation path is the PORTED one.
//
// NEW code over the KIWI layers plus the ported cores, each called exactly the
// way its existing caller calls it:
//
//   ENUMERATE      EclassList_Gather( rows )                  win_ent.cpp:169
//                  (FillClassList 0x496800's own g_eclass walk)
//   PLACE          Test_Ray( origin, dir, contents, t, n )    select.cpp:770
//                  called as Cam_ContextMenu calls it (camwnd.cpp:4273)
//   THE BOX        Ed_EnsureCurrentMaterial_Kiwi + Brush_Alloc + Brush_Create +
//                  Brush_BuildWindings + KiwiExtrude_LandDef — the same five the
//                  ported CreateEntityBrush (xywnd.cpp:3306-3325) runs, and the
//                  same five kiwi_primitive.cpp's AllocBoxDef/land pair runs
//   CREATE         Undo_ClearRedo / Undo_GeneralStart( "create entity" ) /
//                  CreateEntityFromName / Undo_End — CreateEntityFromClassname's
//                  own bracket, verbatim (xywnd.cpp:3374-3386)
//   THE TAIL       KiwiCmd_AfterPaste()                       kiwi_command.cpp:1193
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"

#include <imgui/imgui.h>

#include "kiwi_entbrowser.h"
#include "kiwi_entthumb.h"          // KIWI-UX (ROUND AV, ITEM 3): the real 3D tile preview
#include "kiwi_command.h"
#include "kiwi_construct.h"         // KiwiCon_ActivePlane / KiwiCon_RayPlane
#include "kiwi_grid.h"              // KiwiGrid_Snap
#include "kiwi_lines.h"             // KIWI-UX (ROUND AX, ITEM 6): the drag ghost's 12 edges
#include "kiwi_pick.h"              // Pick_RayFromImagePos / Pick_CameraContents
#include "kiwi_str.h"                // KIWI-UX (CLEANUP, C-66): KiwiStr_ContainsNoCase
#include "kiwi_windows.h"

#include <stdio.h>
#include <string.h>
#include <vector>

// ── ported entry points (each verified against its definition) ──────────────
extern int         Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:112   int Sys_Printf(const char*,...)
extern int         g_nUpdateBits;                                            // engine_stubs.cpp:773  int g_nUpdateBits
extern eclass_t   *Eclass_ForName( int has_brushes, const char *name );      // eclass.cpp:1090   eclass_t *Eclass_ForName(int,const char*)
extern brush_t    *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls );   // brush.cpp:463     brush_t *Brush_Alloc(const void*,eclass_t*)
extern void        Brush_Create( float *mins, float *maxs, brush_t *b, eclass_t *ecls ); // brush.cpp:508  void Brush_Create(float*,float*,brush_t*,eclass_t*)
extern void        Brush_BuildWindings( brush_t *def, int bFull );           // brush.cpp:1418    void Brush_BuildWindings(brush_t*,int)
extern void        Select_Deselect( int a1 );                                // select.cpp:1445   void Select_Deselect(int)
extern void        CreateEntityFromName( const char *str );                  // xywnd.cpp:3390    void CreateEntityFromName(const char*)
extern void        Test_Ray( float *start, float *dir, int contents,
                             edTrace_t *t, int num_traces );                 // select.cpp:770    void Test_Ray(float*,float*,int,edTrace_t*,int)
extern void        Undo_ClearRedo();                                         // undo.cpp:176      void Undo_ClearRedo()
extern void        Undo_GeneralStart( const char *operation );               // undo.cpp:367      void Undo_GeneralStart(const char*)
extern void        Undo_End();                                               // undo.cpp:686      void Undo_End()
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1340  bool Radiant_RegisterCommand(const char*,byte,byte,int)
// xywnd.cpp:1571 // KIWI-UX forwarder for the static Ed_EnsureCurrentMaterial
// (the same one kiwi_extrude.cpp / kiwi_primitive.cpp use).
extern void        Ed_EnsureCurrentMaterial_Kiwi();                          // xywnd.cpp:1571    void Ed_EnsureCurrentMaterial_Kiwi()
// kiwi_extrude.cpp:2328 — the ported land triple (Entity_LinkBrush ->
// Brush_AddToList -> Brush_AddToList2), exported so nothing re-spells it.
extern selbrush_t *KiwiExtrude_LandDef( brush_t *def );                      // kiwi_extrude.cpp:2349  selbrush_t *KiwiExtrude_LandDef(brush_t*)
// imgui_shell.cpp:739 — the live dockspace id, for the JustOpened re-dock latch
// (the same accessor kiwi_outliner.cpp:118 declares).
extern ImGuiID     ImGuiShell_DockRoot();                                    // imgui_shell.cpp:789   ImGuiID ImGuiShell_DockRoot()

// win_ent.cpp's eclass row.  MUST MATCH win_ent.cpp:161 and
// imgui_panel_entity.cpp:38 verbatim (shared-header consolidation pending — the
// same note both of those carry).
struct eclassRow_t
{
    const char *name;
    eclass_t   *eclass;
};
extern void EclassList_Gather( std::vector<eclassRow_t> &rows );             // win_ent.cpp:169   void EclassList_Gather(std::vector<eclassRow_t>&)

namespace
{
    // ── tile metrics ────────────────────────────────────────────────────────
    const float KENTB_TILE_W    = 84.0f;    // the 3D cell
    const float KENTB_TILE_H    = 64.0f;
    const float KENTB_PAD       = 6.0f;     // isometric inset inside the cell
    const int   KENTB_TRACES    = 20;       // Cam_ContextMenu's own depth (camwnd.cpp:4196)
    const float KENTB_BOX_SIDE  = 64.0f;    // placeholder cube for a BRUSH eclass

    // Filter kinds.  "Point" == eclass_t.fixedsize (a real bbox); "Brush" == a
    // class that takes the selection's brushes (func_*, trigger_*, …).
    enum entKind_t { KENTB_KIND_ALL = 0, KENTB_KIND_POINT, KENTB_KIND_BRUSH };

    char       s_filter[64]  = { 0 };
    int        s_kind        = KENTB_KIND_ALL;

    // ── THE PENDING DROP ────────────────────────────────────────────────────
    // Written by the drop handler INSIDE the ImGui frame, consumed by the
    // deferred KIWI_CMD_ENT_DROP arm after the present.  The PIXEL is recorded,
    // not the world point: the ray must be cast from where the user let go, and
    // it must be cast with the camera state the pump sees, not a frame earlier.
    bool  s_pendHave = false;
    char  s_pendClass[64] = { 0 };
    int   s_pendX = 0, s_pendY = 0;

    // ── KIWI-UX (ROUND AX, ITEM 6) — THE DRAG GHOST ─────────────────────────
    // The RESOLVED BOX, not the pixel, and that asymmetry with s_pend* above is
    // deliberate.  The drop resolves post-present, where re-deriving the camera basis
    // is free; the ghost is drawn from inside CamWnd_Draw's overlay tail, where
    // Pick_RayFromImagePos' CamWnd_BuildMatrix (kiwi_pick.cpp:493) has no business
    // running.  So the ladder runs once per frame in the ImGui frame that owns the
    // drag, and the camera only draws twelve latched line segments.
    bool  s_ghostHave = false;
    float s_ghostMins[3] = { 0.0f, 0.0f, 0.0f };
    float s_ghostMaxs[3] = { 0.0f, 0.0f, 0.0f };
    float s_ghostCol[3]  = { 1.0f, 1.0f, 1.0f };

    // ── grouping ────────────────────────────────────────────────────────────
    // Group key = the classname up to the FIRST underscore; no underscore ->
    // "(other)".  See kiwi_entbrowser.h for why this is KIWI's own rule and not
    // a mirror of the (stubbed) RMB tree.
    void GroupKey( const char *name, char *out, size_t outSz )
    {
        out[0] = '\0';
        if ( !name || !*name )
        {
            strncpy( out, "(other)", outSz - 1 );
            out[outSz - 1] = '\0';
            return;
        }
        const char *us = strchr( name, '_' );
        if ( !us || us == name )
        {
            strncpy( out, "(other)", outSz - 1 );
            out[outSz - 1] = '\0';
            return;
        }
        size_t n = (size_t)( us - name );
        if ( n > outSz - 1 )
            n = outSz - 1;
        memcpy( out, name, n );
        out[n] = '\0';
    }

    // ASCII case-insensitive substring, done by hand for the same reason
    // imgui_panel_entity.cpp:107-127 does it by hand — no extra headers.
    // KIWI-UX (CLEANUP, C-66): this body IS the one that moved to kiwi_str.h as
    // KiwiStr_ContainsNoCase — it was the copy with the right empty-needle answer
    // and the right ASCII fold, and two other files disagreed with it.

    bool RowVisible( const eclassRow_t &r )
    {
        if ( !r.eclass )
            return false;
        const bool point = ( *(int *)&r.eclass->fixedsize != 0 );
        if ( s_kind == KENTB_KIND_POINT && !point )
            return false;
        if ( s_kind == KENTB_KIND_BRUSH && point )
            return false;
        return KiwiStr_ContainsNoCase( r.name, s_filter );
    }

    // ── the isometric box ───────────────────────────────────────────────────
    // A real projection, not an icon: the eight corners of the eclass's own
    // [mins,maxs] are projected with the classic 2:1 isometric basis
    //     sx = ( x - y ) * cos30 ,  sy = ( x + y ) * sin30 - z
    // then the projected 2D bounds are fitted into the tile.  Three faces are
    // filled (top / front-left / front-right) at three brightnesses of the
    // eclass colour, and all twelve edges are stroked.  Nothing here touches the
    // renderer: it is ImDrawList triangles inside the panel's own window.
    void ProjectIso( const float p[3], float *ox, float *oy )
    {
        const float c30 = 0.86602540f, s30 = 0.5f;
        *ox = ( p[0] - p[1] ) * c30;
        *oy = ( p[0] + p[1] ) * s30 - p[2];
    }

    ImU32 EclassCol( const eclass_t *ec, float mul, float alpha )
    {
        float r = 0.7f, g = 0.7f, b = 0.7f;
        if ( ec )
        {
            r = ec->color[0];
            g = ec->color[1];
            b = ec->color[2];
        }
        // A class whose QUAKED block gave no colour parses as pure white
        // (eclass.cpp:810) — leave it, white reads fine on the dark theme.
        r *= mul; g *= mul; b *= mul;
        if ( r > 1.0f ) r = 1.0f;
        if ( g > 1.0f ) g = 1.0f;
        if ( b > 1.0f ) b = 1.0f;
        return ImGui::GetColorU32( ImVec4( r, g, b, alpha ) );
    }

    void DrawIsoBox( ImDrawList *dl, const ImVec2 &cellMin, const ImVec2 &cellMax,
                     const eclass_t *ec, bool point, bool hovered )
    {
        float bmin[3], bmax[3];
        if ( point )
        {
            for ( int k = 0; k < 3; ++k )
            {
                bmin[k] = ec->mins[k];
                bmax[k] = ec->maxs[k];
            }
        }
        else
        {
            // A brush class has NO bbox (mins/maxs stay zero — only the QUAKED
            // size block sets them, eclass.cpp:836), so the tile shows the unit
            // cube that stands for "this class takes a box".
            for ( int k = 0; k < 3; ++k )
            {
                bmin[k] = -0.5f;
                bmax[k] =  0.5f;
            }
        }
        // A degenerate axis (a QUAKED block with a flat box) would divide by zero
        // in the fit below; give it a sliver so the projection still has area.
        for ( int k = 0; k < 3; ++k )
            if ( bmax[k] - bmin[k] < 0.001f )
            {
                bmin[k] -= 0.5f;
                bmax[k] += 0.5f;
            }

        // The eight corners, in the canonical order the face table below indexes:
        // bit 0 = x, bit 1 = y, bit 2 = z (0 = min, 1 = max).
        // KIWI-UX (CLEANUP, BoxEdges): one expansion, kiwi_lines.h.
        float world[KIWI_BOX_CORNERS][3];
        KiwiBox_Corners( bmin, bmax, world );

        float px[8], py[8];
        float lox = 1e30f, loy = 1e30f, hix = -1e30f, hiy = -1e30f;
        for ( int i = 0; i < 8; ++i )
        {
            ProjectIso( world[i], &px[i], &py[i] );
            if ( px[i] < lox ) lox = px[i];
            if ( px[i] > hix ) hix = px[i];
            if ( py[i] < loy ) loy = py[i];
            if ( py[i] > hiy ) hiy = py[i];
        }

        const float availW = ( cellMax.x - cellMin.x ) - KENTB_PAD * 2.0f;
        const float availH = ( cellMax.y - cellMin.y ) - KENTB_PAD * 2.0f;
        const float spanX  = ( hix - lox ) > 0.001f ? ( hix - lox ) : 1.0f;
        const float spanY  = ( hiy - loy ) > 0.001f ? ( hiy - loy ) : 1.0f;
        float scale = availW / spanX;
        if ( availH / spanY < scale )
            scale = availH / spanY;
        const float cx = ( cellMin.x + cellMax.x ) * 0.5f;
        const float cy = ( cellMin.y + cellMax.y ) * 0.5f;
        const float mx = ( lox + hix ) * 0.5f;
        const float my = ( loy + hiy ) * 0.5f;

        ImVec2 s[8];
        for ( int i = 0; i < 8; ++i )
            s[i] = ImVec2( cx + ( px[i] - mx ) * scale,
                           cy + ( py[i] - my ) * scale );

        const float boost = hovered ? 1.35f : 1.0f;
        if ( point )
        {
            // TOP (z max): 4,5,7,6 — the winding that comes out convex under the
            // projection above.  Then the two front faces.
            dl->AddQuadFilled( s[4], s[5], s[7], s[6], EclassCol( ec, 1.15f * boost, 0.85f ) );
            dl->AddQuadFilled( s[0], s[1], s[5], s[4], EclassCol( ec, 0.80f * boost, 0.85f ) );  // y min
            dl->AddQuadFilled( s[1], s[3], s[7], s[5], EclassCol( ec, 0.55f * boost, 0.85f ) );  // x max
        }
        // KIWI-UX (CLEANUP, BoxEdges): one edge table, kiwi_lines.h.
        const ImU32 edge = EclassCol( ec, point ? ( 1.4f * boost ) : ( 1.1f * boost ), 1.0f );
        for ( int e = 0; e < KIWI_BOX_EDGES; ++e )
            dl->AddLine( s[KIWI_BOX_EDGE[e][0]], s[KIWI_BOX_EDGE[e][1]], edge, 1.0f );
    }

    // KIWI-UX (ROUND AX, ITEM 4).  The AC130 thermal family, by MODEL name suffix.
    // Both stock thermal models end in "_ac130" (body_complete_sp_sas_ct_ac130 and
    // body_complete_sp_spetsnaz_boris_sp_ac130) and nothing else in raw/xmodel does.
    // See the call site for why this is a hint on the tooltip and not a render decision.
    bool ThermalModelName( const char *mdl )
    {
        if ( !mdl )
            return false;
        const size_t n = strlen( mdl );
        return n >= 6 && _stricmp( mdl + n - 6, "_ac130" ) == 0;
    }

    // ── the tooltip ─────────────────────────────────────────────────────────
    void TileTooltip( const eclassRow_t &r, bool point )
    {
        if ( !ImGui::BeginTooltip() )
            return;
        ImGui::TextUnformatted( r.name ? r.name : "(unnamed)" );
        ImGui::Separator();
        if ( point )
            ImGui::Text( "point entity   size %g %g %g",
                         r.eclass->maxs[0] - r.eclass->mins[0],
                         r.eclass->maxs[1] - r.eclass->mins[1],
                         r.eclass->maxs[2] - r.eclass->mins[2] );
        else
            ImGui::TextUnformatted( "brush entity   (drops a 64-unit box)" );
        // qe3.h:604 — the five preview-model slots; [0] is "defaultmdl=" from the
        // QUAKED attributes (eclass.cpp:915).  Named here because it is exactly
        // the set a future thumbnail pass would render.
        if ( r.eclass->default_model_name && *r.eclass->default_model_name )
        {
            ImGui::Text( "model: %s", r.eclass->default_model_name );
            // ── KIWI-UX (ROUND AX, ITEM 4) — THE THERMAL NOTE ────────────────
            // USER REPORT: "some thumbnails still white", pointing at the
            // actor_enemy_* run.  Six of the 72 actor_enemy_* classes
            // (actor_enemy_ac130_*) resolve to body_complete_sp_spetsnaz_boris_sp_ac130,
            // and Eclass_InsertAlphabetized sorts them FIRST in that group, so they are
            // literally the first tiles of the block.  Round AW measured why they are
            // white: their materials carry the techset literally named `unlit`
            // (technique vertcol_simple_fog_dtex, the FLAT member of the family),
            // declare only a colorMap, and that colorMap is the AC130 thermal texture
            // whose first DXT1 block decodes to (255,242,255).  §74 item 2.
            //
            // The test is on the resolved MODEL name, not the class name.  The naming
            // convention holds across stock content — every class with "ac130" in its
            // name uses a thermal model and no other class does — but nothing enforces
            // it, and a mod's aitype/*.gsc can name a class anything it likes.  What
            // makes a model draw white is its material's techset; the model name is the
            // closest thing to that available without loading the asset, and
            // KiwiModelInfo (round AW) is the readout that settles it for certain.
            if ( ThermalModelName( r.eclass->default_model_name ) )
                ImGui::TextDisabled( "AC130 thermal model - near-white is the shipped asset" );
        }
        if ( r.eclass->comments && *r.eclass->comments )
        {
            ImGui::Separator();
            ImGui::PushTextWrapPos( ImGui::GetFontSize() * 24.0f );
            ImGui::TextUnformatted( r.eclass->comments );
            ImGui::PopTextWrapPos();
        }
        ImGui::TextDisabled( "drag into the 3D view to place" );
        ImGui::EndTooltip();
    }

    // ── one tile ────────────────────────────────────────────────────────────
    void DrawTile( const eclassRow_t &r, int uid )
    {
        const bool point = ( *(int *)&r.eclass->fixedsize != 0 );
        const float labelH = ImGui::GetTextLineHeight();
        const ImVec2 cellSz( KENTB_TILE_W, KENTB_TILE_H + labelH + 2.0f );

        ImGui::PushID( uid );
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        // A REAL item id, deliberately: BeginDragDropSource's common path wants
        // g.ActiveId == the source item (imgui.cpp:15804), which an
        // InvisibleButton gives for free — no SourceAllowNullID, no rect-derived
        // id, and the hover/active states come out right.
        ImGui::InvisibleButton( "##tile", cellSz );
        const bool hovered = ImGui::IsItemHovered();
        const bool active  = ImGui::IsItemActive();

        // ── the drag SOURCE, FIRST ──────────────────────────────────────────
        // Before anything else is submitted: BeginDragDropSource reads
        // g.LastItemData (imgui.cpp:15800), and g.LastItemData is per-CONTEXT, not
        // per-window — a tooltip opened in between would leave the tooltip's last
        // text as "the item" and the drag would never start.  The visuals below
        // are ImDrawList only and submit no items, so they are safe after it.
        if ( ImGui::BeginDragDropSource( ImGuiDragDropFlags_SourceNoHoldToOpenOthers ) )
        {
            char payload[64];
            payload[0] = '\0';
            if ( r.name )
            {
                strncpy( payload, r.name, sizeof( payload ) - 1 );
                payload[sizeof( payload ) - 1] = '\0';
            }
            ImGui::SetDragDropPayload( KENTB_PAYLOAD, payload, sizeof( payload ) );
            ImGui::TextUnformatted( payload );

            // ── KIWI-UX (ROUND AX, ITEM 6a) — SHOW WHAT IS BEING DRAGGED ────
            // USER REPORT: "there's no preview until the drag is completed".  The
            // cheap half of the answer is that the thing under the cursor should look
            // like the thing on the tile.  Same drawer, same source of truth: the
            // cached 3D thumbnail when there is one, otherwise the tile's isometric
            // bbox.  Dummy reserves the rect so the tooltip sizes itself; the visual
            // is ImDrawList into the tooltip's own window, so it costs no item.
            //
            // mayRequest is FALSE here on purpose.  A drag tooltip must never enqueue
            // a model load: the load is a multi-frame synchronous stall (round AX
            // item 3) and stalling the frame that is tracking the cursor is the one
            // place it would be felt most.
            {
                const ImVec2 gp = ImGui::GetCursorScreenPos();
                ImGui::Dummy( ImVec2( KENTB_TILE_W, KENTB_TILE_H ) );
                const ImVec2 gmax( gp.x + KENTB_TILE_W, gp.y + KENTB_TILE_H );
                ImDrawList *gdl = ImGui::GetWindowDrawList();
                IDirect3DTexture9 *gtex = KiwiEntThumb_Get( r.eclass, false );
                if ( gtex )
                {
                    const float gside = ( KENTB_TILE_H < KENTB_TILE_W ? KENTB_TILE_H : KENTB_TILE_W );
                    const float gcx   = ( gp.x + gmax.x ) * 0.5f;
                    const float gcy   = ( gp.y + gmax.y ) * 0.5f;
                    gdl->AddImage( (ImTextureID)(intptr_t)gtex,
                                   ImVec2( gcx - gside * 0.5f, gcy - gside * 0.5f ),
                                   ImVec2( gcx + gside * 0.5f, gcy + gside * 0.5f ) );
                }
                else
                {
                    DrawIsoBox( gdl, gp, gmax, r.eclass, point, true );
                }
            }

            ImGui::TextDisabled( "drop in the 3D view" );
            ImGui::EndDragDropSource();
        }

        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 boxMin( p0.x, p0.y );
        const ImVec2 boxMax( p0.x + KENTB_TILE_W, p0.y + KENTB_TILE_H );

        if ( hovered || active )
            dl->AddRectFilled( p0, ImVec2( p0.x + cellSz.x, p0.y + cellSz.y ),
                               ImGui::GetColorU32( ImGuiCol_FrameBgHovered ), 3.0f );

        // ── KIWI-UX (ROUND AV, ITEM 3): the REAL 3D preview, when there is one ──
        // Round AU's isometric bbox is not replaced, it is the FALLBACK — and it
        // stays the answer for every class with no class-level model, which is most
        // of them.  KiwiEntThumb_Get is a map lookup plus (at most once per frame,
        // across the whole grid) a request record; the render happens in the frame
        // loop's viewport pass, one tile per tick.  Null covers "not rendered yet",
        // "no model" and "load failed" alike, which is exactly the three cases that
        // should all draw a bbox.
        //
        // NO ImDrawList CALLBACK, on purpose.  imgui_shell.cpp:786 + :654 has to bracket
        // its viewport image with an ALPHABLENDENABLE-off callback because the RT's
        // alpha is not guaranteed opaque; a callback SPLITS the draw list, which in a
        // grid of tiles is the one thing not to do.  The thumbnail's alpha was forced
        // to 0xFF during the CPU copy instead (kiwi_entthumb.cpp CopyOutThumb), so a
        // plain blended AddImage is already opaque.
        //
        // SQUARE, CENTRED.  The RT is 128x128 and the tile box is 84x64; stretching
        // one into the other would squash every model horizontally, so the image is
        // drawn at 64x64 centred in the box and the bbox-tile geometry is untouched.
        // KIWI-UX (ROUND AX, ITEM 3): the second argument is "this tile is on screen".
        // There is no ImGuiListClipper here — every tile of every OPEN group is submitted
        // — so without it the load queue ran in list order and a class scrolled far out of
        // view was registered before the row the operator is looking at.  IsRectVisible
        // tests the current window's clip rect, which for this child IS the scroll view.
        IDirect3DTexture9 *thumb =
            KiwiEntThumb_Get( r.eclass,
                              ImGui::IsRectVisible( p0, ImVec2( p0.x + cellSz.x, p0.y + cellSz.y ) ) );
        if ( thumb )
        {
            const float side = ( KENTB_TILE_H < KENTB_TILE_W ? KENTB_TILE_H : KENTB_TILE_W );
            const float cx   = ( boxMin.x + boxMax.x ) * 0.5f;
            const float cy   = ( boxMin.y + boxMax.y ) * 0.5f;
            dl->AddImage( (ImTextureID)(intptr_t)thumb,
                          ImVec2( cx - side * 0.5f, cy - side * 0.5f ),
                          ImVec2( cx + side * 0.5f, cy + side * 0.5f ) );
        }
        else
        {
            DrawIsoBox( dl, boxMin, boxMax, r.eclass, point, hovered );
        }

        // A class that names a model gets a corner badge.  ROUND AV: only while the
        // model is NOT being shown — once the preview is up, the badge is both
        // redundant and sitting on top of the thing it was standing in for.
        if ( !thumb && r.eclass->default_model_name && *r.eclass->default_model_name )
            dl->AddText( ImVec2( boxMax.x - 12.0f, boxMin.y + 1.0f ),
                         ImGui::GetColorU32( ImGuiCol_TextDisabled ), "M" );
        if ( !point )
            dl->AddText( ImVec2( boxMin.x + 2.0f, boxMin.y + 1.0f ),
                         ImGui::GetColorU32( ImGuiCol_TextDisabled ), "B" );

        // The name, clipped to the tile.
        const char *name = r.name ? r.name : "(unnamed)";
        dl->PushClipRect( ImVec2( p0.x, boxMax.y ),
                          ImVec2( p0.x + cellSz.x, p0.y + cellSz.y ), true );
        // The cast is not decoration: the ternary's type is the unnamed ImGuiCol_
        // enum, and GetColorU32 is overloaded on ImGuiCol (int) and ImU32
        // (unsigned) — naming the parameter type keeps the pick unambiguous.
        const ImGuiCol nameStyle = hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled;
        dl->AddText( ImVec2( p0.x + 2.0f, boxMax.y + 1.0f ),
                     ImGui::GetColorU32( nameStyle ), name );
        dl->PopClipRect();

        // The tooltip is LAST and never while a button is held — a drag already
        // has its own preview and two floating panels chasing the cursor is the
        // kind of thing the round-AI options panel was moved out of the image to
        // avoid.
        if ( hovered && !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            TileTooltip( r, point );

        ImGui::PopID();
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  PLACEMENT — everything below runs POST-PRESENT (the deferred command).
    // ═════════════════════════════════════════════════════════════════════════

    // Where a drop at (imgX,imgY) puts the entity's BOUNDING BOX, in world units.
    // `ec` may be a brush class, in which case the box is a KENTB_BOX_SIDE cube.
    // Returns false only when the camera viewport has no ray to give.
    bool ResolveDropBox( const eclass_t *ec, int imgX, int imgY,
                         float outMins[3], float outMaxs[3] )
    {
        ray_t ray;
        if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
            return false;

        const bool point = ( ec && *(int *)&ec->fixedsize != 0 );
        float bmin[3], bmax[3];
        for ( int k = 0; k < 3; ++k )
        {
            bmin[k] = point ? ec->mins[k] : -KENTB_BOX_SIDE * 0.5f;
            bmax[k] = point ? ec->maxs[k] :  KENTB_BOX_SIDE * 0.5f;
        }

        // ── rung 1: a real surface under the drop pixel ─────────────────────
        // Test_Ray called the way Cam_ContextMenu calls it (camwnd.cpp:4273),
        // with the camera-viewport contents mask the pick layer already computes.
        float hit[3];
        float normal[3] = { 0.0f, 0.0f, 1.0f };
        bool  haveHit   = false;
        {
            // KIWI-UX (CLEANUP, C-73): a PLAIN local.  It was a function-local
            // `static` that was memset on every call, so the `static` bought nothing
            // but a hidden global shared between the two callers (the ImGui-frame
            // ghost preview and the post-present drop).  edTrace_t is 88 bytes
            // (camwnd.cpp:4194-4195) x KENTB_TRACES 20 = 1760 bytes of stack, which
            // is what Cam_ContextMenu's own array costs and is nothing on a 1 MB
            // stack.
            edTrace_t traces[KENTB_TRACES];
            memset( traces, 0, sizeof( traces ) );
            Test_Ray( ray.origin, ray.dir, Pick_CameraContents(), traces, KENTB_TRACES );
            if ( traces[0].hit.brush )
            {
                for ( int k = 0; k < 3; ++k )
                {
                    hit[k]    = ray.origin[k] + ray.dir[k] * traces[0].dist;
                    normal[k] = traces[0].normal[k];
                }
                haveHit = true;
            }
        }

        // ── rung 2: the ACTIVE WORKING PLANE ────────────────────────────────
        // Read only.  A drop must never MOVE the plane the user is drawing on —
        // that is round AT's D-AT6/D-AT7 lesson applied to a new caller.
        if ( !haveHit )
        {
            if ( KiwiCon_RayPlane( KiwiCon_ActivePlane(), ray, hit ) )
            {
                const kconPlane_t &pl = KiwiCon_ActivePlane();
                for ( int k = 0; k < 3; ++k )
                    normal[k] = pl.normal[k];
                haveHit = true;
            }
        }

        // ── rung 3: a sane distance down the ray ────────────────────────────
        if ( !haveHit )
        {
            for ( int k = 0; k < 3; ++k )
            {
                hit[k]    = ray.origin[k] + ray.dir[k] * KENTB_FALLBACK_DIST;
                normal[k] = 0.0f;
            }
            normal[2] = 1.0f;
        }

        // The bbox is centred on the hit in the two axes across the surface and
        // pushed OFF it along the dominant normal axis, so it rests ON what was
        // dropped onto rather than half inside it.
        int axis = 2;
        {
            float best = -1.0f;
            for ( int k = 0; k < 3; ++k )
            {
                const float a = normal[k] < 0.0f ? -normal[k] : normal[k];
                if ( a > best ) { best = a; axis = k; }
            }
        }

        float origin[3];
        for ( int k = 0; k < 3; ++k )
            origin[k] = hit[k] - ( bmin[k] + bmax[k] ) * 0.5f;
        if ( normal[axis] >= 0.0f )
            origin[axis] = hit[axis] - bmin[axis];      // sitting on a floor-ish face
        else
            origin[axis] = hit[axis] - bmax[axis];      // hanging under a ceiling-ish one

        // Grid-quantised.  KiwiGrid_Snap copies through when the round-AJ snap
        // master switch is off, which is the same "leave it alone" answer every
        // other caller already handles (kiwi_grid.h:541).
        float snapped[3];
        if ( !KiwiGrid_Snap( origin, snapped ) )
            for ( int k = 0; k < 3; ++k )
                snapped[k] = origin[k];

        for ( int k = 0; k < 3; ++k )
        {
            outMins[k] = snapped[k] + bmin[k];
            outMaxs[k] = snapped[k] + bmax[k];
        }
        return true;
    }

    // The placeholder world brush the ported creator binds to.  Same five calls
    // CreateEntityBrush runs (xywnd.cpp:3306-3325) and the same land triple
    // kiwi_extrude/kiwi_primitive use; only the bounds come from here.
    bool DropPlaceholder( const float mins[3], const float maxs[3] )
    {
        float lo[3], hi[3];
        for ( int k = 0; k < 3; ++k )
        {
            lo[k] = mins[k];
            hi[k] = maxs[k];
            // Brush_Create Com_Error()s on a backwards or zero box
            // (brush.cpp:507-513), so the guard is here — kiwi_primitive.cpp's
            // AllocBoxDef makes the same argument.
            if ( hi[k] - lo[k] < 1.0f )
                hi[k] = lo[k] + 1.0f;
        }

        Ed_EnsureCurrentMaterial_Kiwi();
        brush_t *def = Brush_Alloc( g_qeglobals.random_texture_stuff, nullptr );
        if ( !def )
        {
            Sys_Printf( "Entity browser: brush allocation failed.\n" );
            return false;
        }
        Brush_Create( lo, hi, def, nullptr );
        Brush_BuildWindings( def, 1 );
        KiwiExtrude_LandDef( def );      // link + instance + onto selected_brushes
        return true;
    }

    // The whole deferred act.  ONE undo record (kiwi_entbrowser.h THE CREATION
    // PATH IS THE PORTED ONE).
    void PerformDrop( const char *classname, int imgX, int imgY )
    {
        if ( !classname || !*classname )
        {
            // KIWI-UX (CLEANUP, C-68): the drop record arrived empty (the deferred
            // WM_COMMAND ran with no pending classname), so nothing is placed.  Same
            // early-out, now audible.
            Sys_Printf( "Entity browser: the drop carried no classname - "
                        "nothing placed.\n" );
            return;
        }

        // A live modal gesture cannot have geometry appear underneath it.  Same
        // arbitration KIWI_CMD_REMOVE_FACE uses (kiwi_command.cpp): an unmoved,
        // auto-entered gesture is provably record-free and is cancelled; anything
        // that has actually applied something is left alone and the drop is
        // refused with a line rather than silently.
        if ( KiwiEditorCommand *live = KiwiCmd_Active() )
        {
            if ( live->PreemptIdle() )
            {
                KiwiCmd_Cancel();
            }
            else
            {
                Sys_Printf( "Entity browser: finish or cancel \"%s\" before dropping "
                            "an entity.\n", live->Name() );
                return;
            }
        }

        eclass_t *ec = Eclass_ForName( 0, classname );
        float mins[3], maxs[3];
        if ( !ResolveDropBox( ec, imgX, imgY, mins, maxs ) )
        {
            Sys_Printf( "Entity browser: the 3D view has no size yet — nothing placed.\n" );
            return;
        }

        // CREATE-ONLY.  Entity_Create MERGES the selection into an existing
        // non-world entity instead of creating a new one (entity.cpp:1633-1666),
        // so the selection is cleared first — and BEFORE the bracket opens, so
        // the record clones nothing that is about to be dropped (§23's ordering,
        // as kiwi_primitive.cpp:1173).
        Select_Deselect( 1 );

        // CreateEntityFromClassname's own bracket, verbatim (xywnd.cpp:3374-3386).
        Undo_ClearRedo();
        Undo_GeneralStart( "create entity" );
        if ( !DropPlaceholder( mins, maxs ) )
        {
            Undo_End();
            return;
        }
        CreateEntityFromName( classname );
        Undo_End();

        g_nUpdateBits = -1;
        Sys_Printf( "Placed %s at %g %g %g.\n", classname, mins[0], mins[1], mins[2] );

        // ── KIWI-UX (CLEANUP, C-20): SAY SO WHEN THE MODEL KEY IS STILL MISSING ──
        // CreateEntityFromName's model-class tail (xywnd.cpp:3435-3490) ends in
        // Ed_PostAddModelCommand, which posts WM_COMMAND to g_qeglobals.d_hwndEntity
        // — and this shell keeps that HWND permanently NULL by design
        // (radiant_main.cpp:419-420: CEntityWnd is the MFC inspector the ImGui panels
        // replace).  So the post is a GUARANTEED no-op: the entity lands with its
        // bbox and NO "model" epair, and nothing on screen said why.  There is no
        // "pick a model for this entity" entry point in the shell to call instead —
        // the picker's commit core (Ed_CommitPickedModel, win_ent.cpp:890) exists but
        // has no shell-side chooser in front of it, and popping a modal
        // GetOpenFileNameA from here would run it inside the deferred post-present
        // act.  So this reports honestly rather than pretending; setting the key from
        // the Entity panel is the working route.  These are the same five classes
        // xywnd.cpp:3435-3437 tests.
        if ( !I_stricmp( classname, "misc_model" )   || !I_stricmp( classname, "misc_prefab" ) ||
             !I_stricmp( classname, "script_model" ) || !I_stricmp( classname, "script_vehicle" ) ||
             !I_stricmp( classname, "dyn_model" ) )
        {
            Sys_Printf( "  ...with NO model set — this shell has no model picker on the "
                        "create path.  Set the \"model\" key on it in the Entity panel.\n" );
        }

        // The paste precedent: the new entity is already selected
        // (CreateEntityFromName's Select_Deselect + Select_Brush, xywnd.cpp:3419),
        // so this only puts it under a PAUSED move gizmo.  No-op in the classic
        // keymap profile and no-op if nothing landed (kiwi_command.cpp:1193).
        KiwiCmd_AfterPaste();
    }
}

// ═════════════════════════════════════════════════════════════════════════════
//  THE PANEL
// ═════════════════════════════════════════════════════════════════════════════
void KiwiEntBrowser_Draw()
{
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_ENTITIES );
    if ( !open || !*open )
        return;                              // closed: no Begin, no End, no cost

    if ( KiwiWindows_JustOpened( KIWI_WIN_ENTITIES ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );

    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_ENTITIES ), open ) )
    {
        // ── the header: the same search idiom the Textures panel uses ───────
        // (imgui_shell.cpp:751's InputTextWithHint, round AE.)
        ImGui::SetNextItemWidth( 150.0f );
        ImGui::InputTextWithHint( "##entsearch", "search", s_filter, sizeof( s_filter ) );
        if ( s_filter[0] )
        {
            ImGui::SameLine();
            if ( ImGui::SmallButton( "x##entsearchclr" ) )
                s_filter[0] = '\0';
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth( 90.0f );
        {
            const char *kinds[] = { "All", "Point", "Brush" };
            ImGui::Combo( "##entkind", &s_kind, kinds, 3 );
        }
        ImGui::Separator();

        std::vector<eclassRow_t> rows;
        EclassList_Gather( rows );

        ImGui::BeginChild( "##enttiles", ImVec2( 0.0f, 0.0f ), 0,
                           ImGuiWindowFlags_HorizontalScrollbar );

        // ── BUCKET FIRST, THEN DRAW ─────────────────────────────────────────
        // Not a run-length walk over the alphabetised list: g_eclass is sorted by
        // NAME (Eclass_InsertAlphabetized), so a prefix-less class — "light",
        // "origin", "worldspawn" — falls between two real prefixes and the
        // "(other)" bucket would be emitted several times, each header carrying the
        // SAME ImGui id and therefore the same open/closed state.  One pass to
        // bucket, first-seen group order, one header each.
        struct entGroup_t
        {
            char             key[32];
            std::vector<int> rows;
        };
        std::vector<entGroup_t> groups;
        int shown = 0;
        for ( size_t i = 0; i < rows.size(); ++i )
        {
            if ( !RowVisible( rows[i] ) )
                continue;
            char key[32];
            GroupKey( rows[i].name, key, sizeof( key ) );
            size_t g = 0;
            for ( ; g < groups.size(); ++g )
                if ( strcmp( groups[g].key, key ) == 0 )
                    break;
            if ( g == groups.size() )
            {
                entGroup_t ng;
                strncpy( ng.key, key, sizeof( ng.key ) - 1 );
                ng.key[sizeof( ng.key ) - 1] = '\0';
                groups.push_back( ng );
            }
            groups[g].rows.push_back( (int)i );
            ++shown;
        }

        const float cellW = KENTB_TILE_W + ImGui::GetStyle().ItemSpacing.x;
        for ( size_t g = 0; g < groups.size(); ++g )
        {
            // A search narrows to a handful of classes; forcing every surviving
            // group open is what makes the box usable as a jump.
            if ( s_filter[0] )
                ImGui::SetNextItemOpen( true, ImGuiCond_Always );
            // "label###id": the count is part of the LABEL and not of the id, so
            // changing the filter (which changes the count) cannot reset the
            // header's open state.
            char header[96];
            _snprintf( header, sizeof( header ), "%s (%i)###entgrp_%s",
                       groups[g].key, (int)groups[g].rows.size(), groups[g].key );
            header[sizeof( header ) - 1] = '\0';
            if ( !ImGui::CollapsingHeader( header, ImGuiTreeNodeFlags_DefaultOpen ) )
                continue;

            // Read per group: the child's avail width is stable, but reading it
            // here keeps the wrap correct after a resize.
            const float avail = ImGui::GetContentRegionAvail().x;
            int perRow = (int)( avail / cellW );
            if ( perRow < 1 )
                perRow = 1;

            int col = 0;
            for ( size_t k = 0; k < groups[g].rows.size(); ++k )
            {
                if ( col > 0 )
                    ImGui::SameLine();
                const int idx = groups[g].rows[k];
                DrawTile( rows[idx], idx );
                if ( ++col >= perRow )
                    col = 0;
            }
        }

        if ( shown == 0 )
        {
            if ( rows.empty() )
                ImGui::TextDisabled( "No entity definitions loaded.\n"
                                     "(the .def source directory is set in Preferences)" );
            else
                ImGui::TextDisabled( "No entity class matches the filter." );
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

// ═════════════════════════════════════════════════════════════════════════════
//  THE DROP TARGET (called from imgui_shell.cpp, right after the camera Image)
// ═════════════════════════════════════════════════════════════════════════════
bool KiwiEntBrowser_CameraDropTarget( float imgMinX, float imgMinY )
{
    // ── KIWI-UX (ROUND AX, ITEM 6) — THE GHOST'S LIFETIME ────────────────────
    // USER REPORT: "When dragging out the entity, there's no preview until the drag is
    // completed, not very intuitive. Should be a box at least."
    // Two clears, and both are needed.  A drag that ends anywhere else, or that leaves
    // the camera image, must not leave a box hanging in the viewport — and this function
    // is the ONLY per-frame hook the feature has, so the "no longer dragging" clear has
    // to happen BEFORE the BeginDragDropTarget early-out, not after it.
    // (compile fix: public-API spelling — GetDragDropPayload() is NULL when no
    // drag is live, imgui.h:1020; IsDragDropActive is imgui_internal.h-only.)
    if ( ImGui::GetDragDropPayload() == nullptr )
        s_ghostHave = false;

    if ( !ImGui::BeginDragDropTarget() )
    {
        s_ghostHave = false;         // dragging, but not over the camera image
        return false;
    }

    bool took = false;
    // ACCEPT-BEFORE-DELIVERY.  Without this flag the payload is invisible until the
    // mouse is RELEASED, which is precisely the complaint: nothing to see during the
    // drag.  With it, `p` is non-null on every hovering frame and `p->Delivery`
    // separates "previewing" from "dropped".  AcceptNoDrawDefaultRect because the ghost
    // IS the feedback; ImGui's default yellow rect around a whole 3D viewport is noise.
    const ImGuiPayload *p = ImGui::AcceptDragDropPayload(
        KENTB_PAYLOAD,
        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect );

    // ── the PREVIEW arm — read-only, every hovering frame ────────────────────
    // It runs the SAME ladder the drop runs (ResolveDropBox: Test_Ray -> the working
    // plane -> the fallback distance -> grid snap), which is a pure
    // (eclass, pixel) -> (mins,maxs) function: it creates nothing, selects nothing, and
    // moves no working plane.  Running the real ladder rather than an approximation is
    // the whole point — a preview that disagrees with the drop is worse than none.
    //
    // The BOX is latched, not the pixel, and the camera draws the latched box.  The
    // ladder calls Pick_RayFromImagePos, which calls CamWnd_BuildMatrix
    // (kiwi_pick.cpp:493) — recomputing the camera basis from inside CamWnd_Draw's own
    // overlay tail is not something to do for a decoration.  Consequence: the ghost is
    // one tick behind the cursor, because ImGuiShell_RenderViewportsToRT (which draws
    // the camera) runs BEFORE the compositing ImGui frame this code is in
    // (radiant_main.cpp:942 vs :945).  At the pump's 60 Hz that is 16 ms of trail.
    if ( p && !p->Delivery && p->Data && p->DataSize > 0 )
    {
        char name[64];
        name[0] = '\0';
        const int n = ( p->DataSize < (int)sizeof( name ) ) ? p->DataSize : (int)sizeof( name );
        memcpy( name, p->Data, (size_t)n );
        name[sizeof( name ) - 1] = '\0';

        s_ghostHave = false;
        // Argument order is (has_brushes, name) — eclass.cpp:1090; PerformDrop:632
        // makes the identical call, and a preview that resolved a DIFFERENT class than
        // the drop would be the worst possible preview.
        const eclass_t *ec = name[0] ? Eclass_ForName( 0, name ) : nullptr;
        if ( ec )
        {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            if ( ResolveDropBox( ec, (int)( mp.x - imgMinX ), (int)( mp.y - imgMinY ),
                                 s_ghostMins, s_ghostMaxs ) )
            {
                s_ghostCol[0] = ec->color[0];
                s_ghostCol[1] = ec->color[1];
                s_ghostCol[2] = ec->color[2];
                s_ghostHave   = true;
            }
        }
    }

    if ( p && p->Delivery && p->Data && p->DataSize > 0 )
    {
        s_ghostHave = false;         // the drop owns it from here
        char name[64];
        name[0] = '\0';
        const int n = ( p->DataSize < (int)sizeof( name ) ) ? p->DataSize
                                                            : (int)sizeof( name );
        memcpy( name, p->Data, (size_t)n );
        name[sizeof( name ) - 1] = '\0';

        if ( name[0] )
        {
            // The DROP PIXEL, image-relative and top-left origin — the space
            // kiwi_pick.h's whole coordinate contract is written in.
            const ImVec2 mp = ImGui::GetIO().MousePos;
            s_pendX = (int)( mp.x - imgMinX );
            s_pendY = (int)( mp.y - imgMinY );
            strncpy( s_pendClass, name, sizeof( s_pendClass ) - 1 );
            s_pendClass[sizeof( s_pendClass ) - 1] = '\0';
            s_pendHave = true;
            took = true;

            // DEFERRED — see kiwi_entbrowser.h AND WHY THE CREATION IS DEFERRED.
            // Same PostMessage route kiwi_palette.cpp:151 documents; ids fit
            // LOWORD.
            ::PostMessageA( g_qeglobals.d_hwndMain, WM_COMMAND,
                            (WPARAM)(unsigned int)KIWI_CMD_ENT_DROP, 0 );
        }
    }
    ImGui::EndDragDropTarget();
    return took;
}

// ═════════════════════════════════════════════════════════════════════════════
//  KIWI-UX (ROUND AX, ITEM 6) — THE 3D GHOST
// ═════════════════════════════════════════════════════════════════════════════
// Called from CamWnd_Draw's overlay tail (camwnd.cpp), beside KiwiHover_DrawWorld,
// KiwiCon_DrawWorld and the rest.  It self-gates on the latch and self-budgets, which
// is the contract every entry in that list keeps.
//
// TWELVE SEGMENTS, ONE BATCH, ONE COLOUR — the shape kiwi_dupe.cpp:862's DrawBox uses,
// with the same bit-coded corner order (bit0 = x, bit1 = y, bit2 = z) and the same edge
// table.  That is the identical topology kiwi_entbrowser.cpp's DrawIsoBox draws in the
// tile, so the tooltip preview and the world ghost are literally the same box.
//
// COLOUR IS THE ECLASS COLOUR, BRIGHTENED — not a fixed accent.  The tile already
// identifies a class by its QUAKED colour, and the ghost is the same object mid-flight.
// Alpha is not available here (kiwi_lines.h TRAP 2: alpha is a lerp weight, not
// transparency), so "ghost" is expressed as a lighter tint, never as fading.
void KiwiEntBrowser_DrawGhost()
{
    if ( !s_ghostHave )
        return;

    // KIWI-UX (CLEANUP, BoxEdges): the corners and the edge table are shared with
    // the tile above and with kiwi_dupe.cpp's preview — kiwi_lines.h.
    float v[KIWI_BOX_CORNERS][3];
    KiwiBox_Corners( s_ghostMins, s_ghostMaxs, v );

    float r = s_ghostCol[0] * 1.35f + 0.25f;
    float g = s_ghostCol[1] * 1.35f + 0.25f;
    float b = s_ghostCol[2] * 1.35f + 0.25f;
    if ( r > 1.0f ) r = 1.0f;
    if ( g > 1.0f ) g = 1.0f;
    if ( b > 1.0f ) b = 1.0f;

    KiwiLines_Begin( KIWI_BOX_EDGES, 2 );
    KiwiLines_Color( r, g, b );
    for ( int e = 0; e < KIWI_BOX_EDGES; ++e )
        if ( !KiwiLines_Add( v[KIWI_BOX_EDGE[e][0]], v[KIWI_BOX_EDGE[e][1]] ) )
            break;
    KiwiLines_Flush();
}

// ═════════════════════════════════════════════════════════════════════════════
//  §3 registration + dispatch
// ═════════════════════════════════════════════════════════════════════════════
void KiwiEntBrowser_RegisterCommands()
{
    // The window toggle is registered (unbound) so it is searchable in the §15
    // palette and remappable from radiant.ini — the same deal every other §9
    // window entry gets (kiwi_windows.cpp:271-276).  KIWI_CMD_ENT_DROP is
    // DELIBERATELY NOT registered: it is an internal continuation of a gesture,
    // not a verb anybody can invoke, and a palette row that means "re-place the
    // last thing you dragged" would be a trap.
    Radiant_RegisterCommand( "KiwiWindowEntities", 0, 0, KIWI_CMD_WINDOW_ENTITIES );
}

bool KiwiEntBrowser_DispatchInstant( unsigned int cmdId )
{
    if ( cmdId != (unsigned int)KIWI_CMD_ENT_DROP )
        return false;
    if ( !s_pendHave )
        return true;                 // ours, already consumed (a duplicate post)
    s_pendHave = false;

    char cls[64];
    strncpy( cls, s_pendClass, sizeof( cls ) - 1 );
    cls[sizeof( cls ) - 1] = '\0';
    s_pendClass[0] = '\0';

    PerformDrop( cls, s_pendX, s_pendY );
    return true;
}

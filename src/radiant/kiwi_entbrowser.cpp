#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Uses the ported EclassList_Gather (FillClassList 0x496800), Test_Ray,
// brush-construction/landing, and CreateEntityFromName undo paths.
// KiwiCmd_AfterPaste supplies the established post-create tail.

#include "stdafx.h"
#include "qe3.h"

#include <imgui/imgui.h>

#include "kiwi_entbrowser.h"
#include "kiwi_entthumb.h"          // 3D tile preview
#include "kiwi_command.h"
#include "kiwi_construct.h"         // KiwiCon_ActivePlane / KiwiCon_RayPlane
#include "kiwi_fmt.h"
#include "kiwi_grid.h"              // KiwiGrid_Snap
#include "kiwi_lines.h"             // drag ghost edges
#include "kiwi_pick.h"              // Pick_RayFromImagePos / Pick_CameraContents
#include "kiwi_str.h"                // case-insensitive filtering
#include "kiwi_transform.h"
#include "kiwi_windows.h"

#include <stdio.h>
#include <string.h>
#include <vector>

// Ported entry points; signatures checked against their definitions.
extern int         Sys_Printf( const char *fmt, ... );                       // win_qe3.cpp:118
extern int         g_nUpdateBits;                                            // engine_stubs.cpp:773
extern eclass_t   *Eclass_ForName( int has_brushes, const char *name );      // eclass.cpp:1096
extern brush_t    *Brush_Alloc( const void *planeptsSrc, eclass_t *ecls );   // brush.cpp:465
extern void        Brush_Create( float *mins, float *maxs, brush_t *b, eclass_t *ecls ); // brush.cpp:510
extern void        Brush_BuildWindings( brush_t *def, int bFull );           // brush.cpp:1434
extern void        Select_Deselect( int a1 );                                // select.cpp:1444
extern void        CreateEntityFromName( const char *str );                  // xywnd.cpp:3429
extern void        Test_Ray( float *start, float *dir, int contents,
                             edTrace_t *t, int num_traces );                 // select.cpp:770
extern void        Undo_ClearRedo();                                         // undo.cpp:176
extern void        Undo_GeneralStart( const char *operation );               // undo.cpp:367
extern void        Undo_End();                                               // undo.cpp:686
extern bool        Radiant_RegisterCommand( const char *name, byte vk, byte mods, int commandId ); // mainfrm.cpp:1358
// Forwarder for static Ed_EnsureCurrentMaterial (xywnd.cpp:1595).
extern void        Ed_EnsureCurrentMaterial_Kiwi();                          // xywnd.cpp:1605
// Reuses the ported link/instance/selection landing sequence.
extern selbrush_t *KiwiExtrude_LandDef( brush_t *def );                      // kiwi_extrude.cpp:2360
// Live dockspace id used by the just-opened re-dock latch.
extern ImGuiID     ImGuiShell_DockRoot();                                    // imgui_shell.cpp:796

// Must match the eclass row in win_ent.cpp and imgui_panel_entity.cpp until the
// shared-header consolidation happens.
struct eclassRow_t
{
    const char *name;
    eclass_t   *eclass;
};
extern void EclassList_Gather( std::vector<eclassRow_t> &rows );             // win_ent.cpp:159

namespace
{
    // Tile metrics.
    const float KENTB_TILE_W    = 84.0f;    // the 3D cell
    const float KENTB_TILE_H    = 64.0f;
    const float KENTB_PAD       = 6.0f;     // isometric inset inside the cell
    const int   KENTB_TRACES    = 20;       // Cam_ContextMenu's own depth (camwnd.cpp:4347)
    const float KENTB_BOX_SIDE  = 64.0f;    // placeholder cube for a BRUSH eclass

    // Filter kinds.  "Point" == eclass_t.fixedsize (a real bbox); "Brush" == a
    // class that takes the selection's brushes (func_*, trigger_*, …).
    enum entKind_t { KENTB_KIND_ALL = 0, KENTB_KIND_POINT, KENTB_KIND_BRUSH };

    char       s_filter[64]  = { 0 };
    int        s_kind        = KENTB_KIND_ALL;

    // Written in the ImGui frame and consumed after present. Latch the last valid
    // box so a parallel/upward model release stays stable.
    bool  s_pendHave = false;
    char  s_pendClass[64] = { 0 };
    int   s_pendX = 0, s_pendY = 0;
    bool  s_pendPlacement = false;
    float s_pendMins[3] = { 0.0f, 0.0f, 0.0f };
    float s_pendMaxs[3] = { 0.0f, 0.0f, 0.0f };

    // Resolve the ghost in the ImGui frame because Pick_RayFromImagePos rebuilds
    // the camera basis; CamWnd_Draw receives only the latched box edges.
    bool  s_ghostHave = false;
    char  s_ghostClass[64] = { 0 };
    float s_ghostMins[3] = { 0.0f, 0.0f, 0.0f };
    float s_ghostMaxs[3] = { 0.0f, 0.0f, 0.0f };
    float s_ghostCol[3]  = { 1.0f, 1.0f, 1.0f };

    // Group by the classname prefix before the first underscore; otherwise
    // use "(other)".
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

    // Project the eclass bounds with a 2:1 isometric basis and fit the result
    // inside the tile using only ImDrawList geometry.
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
        // QUAKED blocks without a colour parse as white (eclass.cpp:853).
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
            // Brush classes have no bbox, so a unit cube represents "takes a box"
            // without implying a placement size.
            for ( int k = 0; k < 3; ++k )
            {
                bmin[k] = -0.5f;
                bmax[k] =  0.5f;
            }
        }
        // Give flat QUAKED axes area so the fit cannot divide by zero.
        for ( int k = 0; k < 3; ++k )
            if ( bmax[k] - bmin[k] < 0.001f )
            {
                bmin[k] -= 0.5f;
                bmax[k] += 0.5f;
            }

        // Shared corner order: bit 0=x, bit 1=y, bit 2=z; 0=min, 1=max.
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
            // Top winding 4,5,7,6 stays convex under this projection.
            dl->AddQuadFilled( s[4], s[5], s[7], s[6], EclassCol( ec, 1.15f * boost, 0.85f ) );
            dl->AddQuadFilled( s[0], s[1], s[5], s[4], EclassCol( ec, 0.80f * boost, 0.85f ) );  // y min
            dl->AddQuadFilled( s[1], s[3], s[7], s[5], EclassCol( ec, 0.55f * boost, 0.85f ) );  // x max
        }
        const ImU32 edge = EclassCol( ec, point ? ( 1.4f * boost ) : ( 1.1f * boost ), 1.0f );
        for ( int e = 0; e < KIWI_BOX_EDGES; ++e )
            dl->AddLine( s[KIWI_BOX_EDGE[e][0]], s[KIWI_BOX_EDGE[e][1]], edge, 1.0f );
    }

    // Stock AC130 thermal models share the "_ac130" suffix; use it only to
    // explain their near-white shipped previews, never to change rendering.
    bool ThermalModelName( const char *mdl )
    {
        if ( !mdl )
            return false;
        const size_t n = strlen( mdl );
        return n >= 6 && _stricmp( mdl + n - 6, "_ac130" ) == 0;
    }

    // Tile tooltip.
    void TileTooltip( const eclassRow_t &r, bool point )
    {
        if ( !ImGui::BeginTooltip() )
            return;
        ImGui::TextUnformatted( r.name ? r.name : "(unnamed)" );
        ImGui::Separator();
        if ( point )
        {
            char x[32], y[32], z[32];
            ImGui::Text( "point entity   size %s %s %s",
                         KiwiFmt_Num( x, sizeof( x ), r.eclass->maxs[0] - r.eclass->mins[0] ),
                         KiwiFmt_Num( y, sizeof( y ), r.eclass->maxs[1] - r.eclass->mins[1] ),
                         KiwiFmt_Num( z, sizeof( z ), r.eclass->maxs[2] - r.eclass->mins[2] ) );
        }
        else
            ImGui::TextUnformatted( "brush entity   (drops a 64-unit box)" );
        // default_model_name is QUAKED's "defaultmdl=" preview slot
        // (qe3.h:604, eclass.cpp:958).
        if ( r.eclass->default_model_name && *r.eclass->default_model_name )
        {
            ImGui::Text( "model: %s", r.eclass->default_model_name );
            // Test the resolved model rather than classname because mods can rename
            // classes. KiwiModelInfo can confirm an unfamiliar material/techset.
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

    // One tile.
    void DrawTile( const eclassRow_t &r, int uid )
    {
        const bool point = ( *(int *)&r.eclass->fixedsize != 0 );
        const float labelH = ImGui::GetTextLineHeight();
        const ImVec2 cellSz( KENTB_TILE_W, KENTB_TILE_H + labelH + 2.0f );

        ImGui::PushID( uid );
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        // InvisibleButton supplies the real ActiveId required by
        // BeginDragDropSource's common path (imgui.cpp:15804).
        ImGui::InvisibleButton( "##tile", cellSz );
        const bool hovered = ImGui::IsItemHovered();
        const bool active  = ImGui::IsItemActive();

        // Call the drag source before any tooltip/item because it consumes the
        // context-wide LastItemData (imgui.cpp:15800).
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

            // Reuse the cached thumbnail or bbox in the drag tooltip. mayRequest=false
            // avoids a synchronous multi-frame model load while tracking the cursor;
            // Dummy sizes the tooltip without replacing LastItemData.
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

        // No-model, pending, and failed thumbnails retain the bbox fallback. Alpha is
        // forced opaque during CPU copy, avoiding a draw-list callback per tile.
        // Keep the 128x128 texture square and request loads only for visible tiles so
        // offscreen list order cannot starve the rows in the scroll viewport.
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

        // The model badge disappears once the thumbnail supplies the same signal.
        if ( !thumb && r.eclass->default_model_name && *r.eclass->default_model_name )
            dl->AddText( ImVec2( boxMax.x - 12.0f, boxMin.y + 1.0f ),
                         ImGui::GetColorU32( ImGuiCol_TextDisabled ), "M" );
        if ( !point )
            dl->AddText( ImVec2( boxMin.x + 2.0f, boxMin.y + 1.0f ),
                         ImGui::GetColorU32( ImGuiCol_TextDisabled ), "B" );

        const char *name = r.name ? r.name : "(unnamed)";
        dl->PushClipRect( ImVec2( p0.x, boxMax.y ),
                          ImVec2( p0.x + cellSz.x, p0.y + cellSz.y ), true );
        // The named ImGuiCol keeps GetColorU32's int/ImU32 overload unambiguous.
        const ImGuiCol nameStyle = hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled;
        dl->AddText( ImVec2( p0.x + 2.0f, boxMax.y + 1.0f ),
                     ImGui::GetColorU32( nameStyle ), name );
        dl->PopClipRect();

        // A drag already has a preview, so suppress the ordinary tooltip while held.
        if ( hovered && !ImGui::IsMouseDown( ImGuiMouseButton_Left ) )
            TileTooltip( r, point );

        ImGui::PopID();
    }

    // Placement runs post-present through the deferred command.

    // Resolve an image-relative pixel to world-space bounds. Brush classes use a
    // KENTB_BOX_SIDE cube; model classes can fail on a parallel/upward no-hit ray,
    // in which case the drag target retains its last valid box.
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

        // Model classes share the model-browser placement solver; creation still
        // receives only the resolved box.
        if ( point && ( ec->classtype & 0x8 ) != 0 )
        {
            const float angles[3] = { 0.0f, 0.0f, 0.0f };
            float origin[3];
            return KiwiDrop_ComputePlacement( ray, bmin, bmax, angles, 1.0f,
                                              origin, outMins, outMaxs );
        }

        // Prefer a real surface using Cam_ContextMenu's Test_Ray convention.
        float hit[3];
        float normal[3] = { 0.0f, 0.0f, 1.0f };
        bool  haveHit   = false;
        {
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

        // Otherwise intersect the active working plane without moving it.
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

        // Last resort: KENTB_FALLBACK_DIST world units down the ray.
        if ( !haveHit )
        {
            for ( int k = 0; k < 3; ++k )
            {
                hit[k]    = ray.origin[k] + ray.dir[k] * KENTB_FALLBACK_DIST;
                normal[k] = 0.0f;
            }
            normal[2] = 1.0f;
        }

        // Center across the surface and offset on its dominant normal so the box
        // rests on the face instead of halfway inside it.
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

        // Disabled grid snapping uses KiwiGrid_Snap's copy-through contract.
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

    // Build the placeholder through CreateEntityBrush's ported five-call path;
    // only its bounds originate here.
    bool DropPlaceholder( const float mins[3], const float maxs[3] )
    {
        float lo[3], hi[3];
        for ( int k = 0; k < 3; ++k )
        {
            lo[k] = mins[k];
            hi[k] = maxs[k];
            // Brush_Create errors on backwards or zero boxes (brush.cpp:507-513).
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

    // The deferred placement remains one undo record.
    void PerformDrop( const char *classname, int imgX, int imgY,
                      const float *latchedMins, const float *latchedMaxs )
    {
        if ( !classname || !*classname )
        {
            Sys_Printf( "Entity browser: the drop carried no classname - "
                        "nothing placed.\n" );
            return;
        }

        // Preempt a live command only while it is provably record-free; otherwise
        // refuse to place geometry underneath an active gesture.
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
        if ( latchedMins && latchedMaxs )
        {
            for ( int k = 0; k < 3; ++k )
            {
                mins[k] = latchedMins[k];
                maxs[k] = latchedMaxs[k];
            }
        }
        else if ( !ResolveDropBox( ec, imgX, imgY, mins, maxs ) )
        {
            Sys_Printf( "Entity browser: the drop ray has no valid placement - nothing placed.\n" );
            return;
        }

        // Entity_Create merges into a selected non-world entity, so deselect before
        // opening the undo bracket; the record must not clone discarded selection
        // state (entity.cpp:1633-1666).
        Select_Deselect( 1 );

        // CreateEntityFromClassname's own bracket, verbatim (xywnd.cpp:3375-3387).
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

        // The native add-model post targets d_hwndEntity, which this shell leaves null,
        // so these classes arrive without a "model" key. The separate Models browser
        // handles model-backed creation; this instance can be edited in Entity.
        if ( !I_stricmp( classname, "misc_model" )   || !I_stricmp( classname, "misc_prefab" ) ||
             !I_stricmp( classname, "script_model" ) || !I_stricmp( classname, "script_vehicle" ) ||
             !I_stricmp( classname, "dyn_model" ) )
        {
            Sys_Printf( "  ...with NO model set — this shell has no model picker on the "
                        "create path.  Set the \"model\" key on it in the Entity panel.\n" );
        }

        // The new entity is already selected, so this only starts the configured
        // paused move tail; the classic profile leaves it alone.
        KiwiCmd_AfterPaste();
    }
}

// Entity browser panel.
void KiwiEntBrowser_Draw()
{
    bool *open = KiwiWindows_OpenPtr( KIWI_WIN_ENTITIES );
    if ( !open || !*open )
        return;                              // closed: no Begin, no End, no cost

    if ( KiwiWindows_JustOpened( KIWI_WIN_ENTITIES ) )
        ImGui::SetNextWindowDockID( ImGuiShell_DockRoot(), ImGuiCond_Always );

    if ( ImGui::Begin( KiwiWindows_Title( KIWI_WIN_ENTITIES ), open ) )
    {
        // Match the Textures panel's search control.
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

        // Bucket before drawing: alphabetic order interleaves prefix-less classes,
        // and repeated "(other)" headers would share one ImGui id. Preserve
        // first-seen group order and emit each header once.
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
            // Open filtered groups so search results act as a jump list.
            if ( s_filter[0] )
                ImGui::SetNextItemOpen( true, ImGuiCond_Always );
            // Keep the changing count outside the stable ### id so filtering does
            // not reset the header state.
            char header[96];
            _snprintf( header, sizeof( header ), "%s (%i)###entgrp_%s",
                       groups[g].key, (int)groups[g].rows.size(), groups[g].key );
            header[sizeof( header ) - 1] = '\0';
            if ( !ImGui::CollapsingHeader( header, ImGuiTreeNodeFlags_DefaultOpen ) )
                continue;

            // Re-read available width so group rows rewrap after resize.
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

// Camera drop target; called immediately after the camera Image.
bool KiwiEntBrowser_CameraDropTarget( float imgMinX, float imgMinY )
{
    // Clear before BeginDragDropTarget: this is the only per-frame hook, so a drag
    // ending elsewhere or leaving the image must not leave a stale box.
    // GetDragDropPayload is the public active-drag test (imgui.h:1020).
    if ( ImGui::GetDragDropPayload() == nullptr )
    {
        s_ghostHave = false;
        s_ghostClass[0] = '\0';
    }

    if ( !ImGui::BeginDragDropTarget() )
    {
        s_ghostHave = false;         // dragging, but not over the camera image
        s_ghostClass[0] = '\0';
        return false;
    }

    bool took = false;
    // AcceptBeforeDelivery exposes the payload during hover; Delivery distinguishes
    // preview from drop. The world ghost replaces ImGui's viewport-sized target rect.
    const ImGuiPayload *p = ImGui::AcceptDragDropPayload(
        KENTB_PAYLOAD,
        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect );
    if ( !p )
    {
        s_ghostHave = false;
        s_ghostClass[0] = '\0';
    }

    // Preview uses the drop's pure resolver, so it never mutates selection or the plane.
    // Latch the box here because picking rebuilds the camera basis; render ordering
    // makes the viewport ghost one tick behind the cursor (about 16 ms at 60 Hz).
    if ( p && !p->Delivery && p->Data && p->DataSize > 0 )
    {
        char name[64];
        name[0] = '\0';
        const int n = ( p->DataSize < (int)sizeof( name ) ) ? p->DataSize : (int)sizeof( name );
        memcpy( name, p->Data, (size_t)n );
        name[sizeof( name ) - 1] = '\0';

        if ( _stricmp( s_ghostClass, name ) != 0 )
        {
            s_ghostHave = false;
            strncpy( s_ghostClass, name, sizeof( s_ghostClass ) - 1 );
            s_ghostClass[sizeof( s_ghostClass ) - 1] = '\0';
        }
        // Eclass_ForName order is (has_brushes, name); preview and drop must resolve
        // the same class.
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
        char name[64];
        name[0] = '\0';
        const int n = ( p->DataSize < (int)sizeof( name ) ) ? p->DataSize
                                                            : (int)sizeof( name );
        memcpy( name, p->Data, (size_t)n );
        name[sizeof( name ) - 1] = '\0';

        if ( name[0] )
        {
            // Drop pixel is image-relative with a top-left origin (kiwi_pick.h).
            const ImVec2 mp = ImGui::GetIO().MousePos;
            s_pendX = (int)( mp.x - imgMinX );
            s_pendY = (int)( mp.y - imgMinY );

            if ( _stricmp( s_ghostClass, name ) != 0 )
            {
                s_ghostHave = false;
                strncpy( s_ghostClass, name, sizeof( s_ghostClass ) - 1 );
                s_ghostClass[sizeof( s_ghostClass ) - 1] = '\0';
            }
            const eclass_t *ec = Eclass_ForName( 0, name );
            if ( ec )
            {
                float mins[3], maxs[3];
                if ( ResolveDropBox( ec, s_pendX, s_pendY, mins, maxs ) )
                {
                    for ( int k = 0; k < 3; ++k )
                    {
                        s_ghostMins[k] = mins[k];
                        s_ghostMaxs[k] = maxs[k];
                    }
                    s_ghostHave = true;
                }
            }
            s_pendPlacement = s_ghostHave;
            if ( s_pendPlacement )
                for ( int k = 0; k < 3; ++k )
                {
                    s_pendMins[k] = s_ghostMins[k];
                    s_pendMaxs[k] = s_ghostMaxs[k];
                }
            strncpy( s_pendClass, name, sizeof( s_pendClass ) - 1 );
            s_pendClass[sizeof( s_pendClass ) - 1] = '\0';
            s_pendHave = true;
            took = true;
            s_ghostHave = false;         // the deferred drop owns the latched box
            s_ghostClass[0] = '\0';

            // Defer through the established PostMessage command route; ids fit LOWORD.
            ::PostMessageA( g_qeglobals.d_hwndMain, WM_COMMAND,
                            (WPARAM)(unsigned int)KIWI_CMD_ENT_DROP, 0 );
        }
        else
        {
            s_ghostHave = false;
            s_ghostClass[0] = '\0';
        }
    }
    ImGui::EndDragDropTarget();
    return took;
}

// CamWnd_Draw overlay: self-gated 12-edge box using the shared corner topology.
// Brighten the eclass colour because kiwi_lines alpha is a lerp weight, not
// transparency.
void KiwiEntBrowser_DrawGhost()
{
    if ( !s_ghostHave )
        return;

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

// Command registration and dispatch.
void KiwiEntBrowser_RegisterCommands()
{
    // Register the remappable window toggle, but not KIWI_CMD_ENT_DROP: the latter
    // is an internal gesture continuation, not a user-invokable command.
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

    const bool havePlacement = s_pendPlacement;
    float mins[3] = { s_pendMins[0], s_pendMins[1], s_pendMins[2] };
    float maxs[3] = { s_pendMaxs[0], s_pendMaxs[1], s_pendMaxs[2] };
    s_pendPlacement = false;

    PerformDrop( cls, s_pendX, s_pendY,
                 havePlacement ? mins : 0, havePlacement ? maxs : 0 );
    return true;
}

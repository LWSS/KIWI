#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// Hover and active accents have a bounded line budget; patches degrade to their
// bounding boxes rather than exhausting it with a control net.
// Lines are nudged toward the viewer to avoid z-fighting the depth-buffered face.
// Fills use 0.5 world units because depth loss is more visible across an area.
// Face accents are convex triangle fans, not edge-like line-only highlights.
// Their material bracket mirrors the selected-face pass at camwnd.cpp 0x408106:
// neutral MATERIAL_COLOR uses per-vertex color, then white restores later passes.

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "prefs.h"          // g_PrefsDlg (camera_fov — the screen-scaled marker)
#include <gfx_d3d/r_gfx.h>          // GfxColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT
#include "kiwi_camera.h"    // KiwiCam_WorldPerPixel (the shared screen-scale)
#include "kiwi_conselect.h"
#include "kiwi_construct.h"
#include "kiwi_grass.h"
#include "kiwi_hover.h"
#include "kiwi_lines.h"
#include "kiwi_region.h"
#include "kiwi_selection.h"
#include "kiwi_sun.h"
#include "kiwi_ux.h"
#include "kiwi_uveditor.h"  // KiwiUvEd_OverlaySuppressed
#include "kiwi_vec.h"

#include <math.h>
#include <string.h>
#include <stdint.h>

// Ported entry points.
extern camera_s   *Ed_Camera();          // camwnd.cpp
extern void        CamWnd_BuildMatrix(); // camwnd.cpp 0x403470
extern char  Byte4PackPixelColor( float *from, GfxColor *out );          // 0x402ac0
extern int   Sys_Printf( const char *fmt, ... );                        // win_qe3.cpp:118
extern int   g_nUpdateBits;                                             // engine_stubs.cpp:773
extern void  __cdecl R_AddRenderCmdDrawTris(
                 Material *material, MaterialTechniqueType techType, short indexCount,
                 const uint16_t *indices, short vertexCount,
                 const float ( *xyzw )[4], const float ( *normal )[3], float *color,
                 const float ( *st )[2] );                               // 0x4fd1c0

namespace
{
    enum { KHOVER_MAX_SEGMENTS = 220 };

    // Selected accents walk newest-first and thick-before-thin, so this separate
    // cap drops the oldest face borders before point/edge feedback.
    enum { KSEL_MAX_SEGMENTS = 300 };

    // Delete, undo, and map close can stale cached brush pointers; require list
    // membership through the shared guard before every dereference.
    inline bool BrushLive( const selbrush_t *b ) { return Sel_BrushLive( b ); }

    const float KHOVER_NUDGE = 0.25f;      // world units toward the eye (see the note above)
    const float KHOVER_COL[3] = { 0.35f, 0.95f, 1.00f };   // light cyan
    const float KACTIVE_COL[3] = { 1.00f, 0.80f, 0.25f };  // warm accent
    // Selected uses the active hue at lower intensity to preserve state ordering.
    const float KSELECTED_COL[3] = { 0.72f, 0.56f, 0.16f };

    // Face fills retain the line hues; hover matches selected opacity and active
    // is deliberately stronger.
    const float KHOVER_FILL_NUDGE = 0.50f;                            // larger area nudge; see above
    // The ported selected-face fill uses alpha 0.25 (win_qe3.cpp:429), so hover
    // remains comparable.
    const float KFILL_HOVER   [4] = { 0.35f, 0.95f, 1.00f, 0.26f };   // hover      — light cyan
    const float KFILL_SELECTED[4] = { 0.72f, 0.56f, 0.16f, 0.25f };   // selected   — amber, dim
    const float KFILL_ACTIVE  [4] = { 1.00f, 0.80f, 0.25f, 0.35f };   // active     — amber, bright

    // Oversized windings fall back to an outline, bounding the static fan buffers.
    enum { KHOVER_FILL_MAX_PTS = 64 };
    // Each face is one draw command, so all fill users share a per-frame cap.
    enum { KHOVER_FILL_MAX_FACES = 64 };

    int s_fillsThisFrame = 0;            // reset by KiwiHover_DrawWorld

    // The caller owns the MATERIAL_COLOR bracket to avoid two extra commands per face.
    void EmitFaceFill( const camera_s *c, winding_t *w, const float rgba[4] )
    {
        if ( !w || w->numpoints < 3 || w->numpoints > KHOVER_FILL_MAX_PTS )
            return;
        if ( s_fillsThisFrame >= KHOVER_FILL_MAX_FACES )
            return;
        ++s_fillsThisFrame;

        static float    s_xyzw  [KHOVER_FILL_MAX_PTS][4];
        static float    s_normal[KHOVER_FILL_MAX_PTS][3];
        static float    s_st    [KHOVER_FILL_MAX_PTS][2];
        static float    s_color [KHOVER_FILL_MAX_PTS];
        static uint16_t s_idx   [( KHOVER_FILL_MAX_PTS - 2 ) * 3];

        float col[4] = { rgba[0], rgba[1], rgba[2], rgba[3] };
        GfxColor packed;
        Byte4PackPixelColor( col, &packed );
        const float packedAsFloat = *(float *)&packed.packed;   // bit-cast, as the ported batcher does

        const int n = w->numpoints;
        for ( int i = 0; i < n; ++i )
        {
            s_xyzw[i][0] = w->p[i][0] - c->vpn[0] * KHOVER_FILL_NUDGE;
            s_xyzw[i][1] = w->p[i][1] - c->vpn[1] * KHOVER_FILL_NUDGE;
            s_xyzw[i][2] = w->p[i][2] - c->vpn[2] * KHOVER_FILL_NUDGE;
            s_xyzw[i][3] = 1.0f;
            // TECHNIQUE_UNLIT still shades from normals; constant world +Z avoids
            // camera-pitch-dependent tint (kiwi_lines.h TRAP 4).
            KiwiTris_FillNormal( s_normal[i] );
            s_st[i][0] = 0.0f;
            s_st[i][1] = 0.0f;
            s_color[i] = packedAsFloat;
        }

        // A fan from vertex 0 is exact because brush-face windings are convex.
        int k = 0;
        for ( int i = 1; i + 1 < n; ++i )
        {
            s_idx[k++] = 0;
            s_idx[k++] = (uint16_t)i;
            s_idx[k++] = (uint16_t)( i + 1 );
        }

        // Brush windings face outward; orient each triangle to the eye because
        // white_tools backface-culls (kiwi_lines.h TRAP 3).
        KiwiTris_OrientToEye( &s_xyzw[0][0], 4, s_idx, k, c->origin );

        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                (short)k, s_idx, (short)n,
                                s_xyzw, s_normal, s_color, s_st );
    }

    pick_result_t s_hover;
    kconSelItem_t s_conHover;
    bool          s_conHoverValid = false;
    int           s_regionHover   = -1;
    bool          s_removePreview = false;


    void Nudge( const camera_s *c, const float *in, float *out )
    {
        out[0] = in[0] - c->vpn[0] * KHOVER_NUDGE;
        out[1] = in[1] - c->vpn[1] * KHOVER_NUDGE;
        out[2] = in[2] - c->vpn[2] * KHOVER_NUDGE;
    }

    void AddNudged( const camera_s *c, const float *a, const float *b )
    {
        float na[3], nb[3];
        Nudge( c, a, na );
        Nudge( c, b, nb );
        KiwiLines_Add( na, nb );
    }

    winding_t *WindingOf( const sel_item_t &it )
    {
        if ( !it.brush || !it.brush->def )
            return nullptr;
        brush_t *def = it.brush->def;
        if ( !def->faces || it.faceIndex < 0 || it.faceIndex >= def->faceCount )
            return nullptr;
        return def->faces[it.faceIndex].w;
    }

    // The UV editor owns selected overlays per face; other kinds use object scope,
    // which also suppresses accents for a gathered patch.
    bool UvEditorOwns( const sel_item_t &it )
    {
        if ( !it.brush || !it.brush->def )
            return false;
        return KiwiUvEd_OverlaySuppressed( it.brush->def,
                                           ( it.kind == SEL_FACE ) ? it.faceIndex : -1 );
    }

    // Callers liveness-gate before EmitItem, so shared selection resolvers skip a
    // duplicate display-list walk.

    void EmitWinding( const camera_s *c, winding_t *w )
    {
        if ( !w || w->numpoints < 2 || w->numpoints > MAX_POINTS_ON_WINDING )
            return;
        int prev = w->numpoints - 1;
        for ( int i = 0; i < w->numpoints; ++i )
        {
            if ( KiwiLines_Remaining() <= 0 )
                return;
            AddNudged( c, w->p[prev], w->p[i] );
            prev = i;
        }
    }

    void EmitBox( const camera_s *c, const float *mins, const float *maxs )
    {
        static const int edges[12][2] =
        {
            { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },     // z = mins
            { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },     // z = maxs
            { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
        };
        float pts[8][3];
        for ( int i = 0; i < 8; ++i )
        {
            pts[i][0] = ( i & 1 ) ? maxs[0] : mins[0];
            pts[i][1] = ( i & 2 ) ? maxs[1] : mins[1];
            pts[i][2] = ( i & 4 ) ? maxs[2] : mins[2];
        }
        for ( int e = 0; e < 12; ++e )
            AddNudged( c, pts[edges[e][0]], pts[edges[e][1]] );
    }

    // Square-plus-diagonals marker in camera basis. KiwiCam_WorldPerPixel returns
    // world units per screen pixel, keeping its image size constant with depth.
    void EmitPointMarker( const camera_s *c, const float *p, float pixels )
    {
        const float h = KiwiCam_WorldPerPixel( p ) * pixels;
        float corner[4][3];
        const float sx[4] = { -1.0f,  1.0f,  1.0f, -1.0f };
        const float sy[4] = { -1.0f, -1.0f,  1.0f,  1.0f };
        for ( int i = 0; i < 4; ++i )
            for ( int k = 0; k < 3; ++k )
                corner[i][k] = p[k] + c->vright[k] * ( sx[i] * h )
                                    + c->vup[k]    * ( sy[i] * h );
        for ( int i = 0; i < 4; ++i )
            AddNudged( c, corner[i], corner[( i + 1 ) & 3] );
        AddNudged( c, corner[0], corner[2] );
        AddNudged( c, corner[1], corner[3] );
    }

    bool ConItemSelected( const kconSelItem_t &item )
    {
        for ( int i = 0; i < KiwiConSel_Count(); ++i )
        {
            const kconSelItem_t *at = KiwiConSel_At( i );
            if ( at && at->object == item.object && at->kind == item.kind
              && at->index == item.index )
                return true;
        }
        return false;
    }

    void EmitConstructionHover( const camera_s *c )
    {
        if ( !s_conHoverValid )
            return;
        const kconObject_t *o = KiwiCon_At( s_conHover.object );
        if ( !o )
            return;

        if ( s_conHover.kind == KCONSEL_POINT )
        {
            float p[3];
            if ( KiwiCon_AnchorWorld( *o, s_conHover.index, p ) )
                EmitPointMarker( c, p, 5.0f );
            return;
        }
        if ( s_conHover.kind == KCONSEL_SEGMENT )
        {
            float a[3], b[3];
            if ( KiwiCon_SegmentWorld( *o, s_conHover.index, a, b ) )
                AddNudged( c, a, b );
            return;
        }
        const int count = KiwiCon_SegmentCount( *o );
        for ( int i = 0; i < count && KiwiLines_Remaining() > 0; ++i )
        {
            float a[3], b[3];
            if ( KiwiCon_SegmentWorld( *o, i, a, b ) )
                AddNudged( c, a, b );
        }
    }

    void EmitRegionHover( const camera_s *c )
    {
        const std::vector<kregion_t> &regions = KiwiRegion_All();
        if ( s_regionHover < 0 || s_regionHover >= (int)regions.size() )
            return;
        const kregion_t &r = regions[s_regionHover];
        const int count = (int)( r.pts.size() / 2 );
        if ( count < 2 )
            return;
        float prev[3];
        KiwiCon_PlaneToWorld( r.plane, &r.pts[( count - 1 ) * 2], prev );
        for ( int i = 0; i < count && KiwiLines_Remaining() > 0; ++i )
        {
            float cur[3];
            KiwiCon_PlaneToWorld( r.plane, &r.pts[i * 2], cur );
            AddNudged( c, prev, cur );
            memcpy( prev, cur, sizeof( prev ) );
        }
    }

    void DrawSpecialHover( const camera_s *c )
    {
        if ( !s_conHoverValid && s_regionHover < 0 )
            return;
        KiwiLines_Begin( KHOVER_MAX_SEGMENTS, s_regionHover >= 0 ? 1 : 2 );
        const float *col = s_removePreview ? KACTIVE_COL : KHOVER_COL;
        KiwiLines_Color( col[0], col[1], col[2] );
        if ( s_conHoverValid )
            EmitConstructionHover( c );
        else
            EmitRegionHover( c );
        KiwiLines_Flush();
    }

    void EmitObject( const camera_s *c, selbrush_t *b )
    {
        brush_t *def = b ? b->def : nullptr;
        if ( !def )
            return;
        if ( b->patch )
        {
            // A 16x16 control net is 480 segments on its own — degrade to the def's
            // bounding box so a hovered patch can never blow the budget.
            EmitBox( c, def->mins, def->maxs );
            return;
        }
        if ( !def->faces || def->faceCount <= 0 )
            return;
        for ( int f = 0; f < def->faceCount; ++f )
        {
            if ( KiwiLines_Remaining() <= 0 )
                return;
            EmitWinding( c, def->faces[f].w );
        }
    }

    // Emit into the caller's thin/thick batch; width belongs to Begin/Flush.
    void EmitItem( const camera_s *c, const sel_item_t &it, bool thin )
    {
        switch ( it.kind )
        {
        case SEL_OBJECT:
            if ( thin )
                EmitObject( c, it.brush );
            break;
        case SEL_FACE:
            // A face combines its translucent fill with a width-1 closed border;
            // an edge remains one width-2 segment, so the accents stay distinct.
            if ( thin )
            {
                winding_t *fw = WindingOf( it );
                if ( fw && fw->numpoints >= 2 )
                    for ( int i = 0, j = fw->numpoints - 1; i < fw->numpoints; j = i++ )
                        AddNudged( c, fw->p[j], fw->p[i] );
            }
            break;
        case SEL_EDGE:
            if ( !thin )
            {
                float a[3], b[3];
                if ( Sel_EdgeEnds( it, a, b, false ) )
                    AddNudged( c, a, b );
            }
            break;
        case SEL_VERTEX:
            if ( !thin )
            {
                float p[3];
                if ( Sel_ItemWorldPos( it, p, false ) )
                    EmitPointMarker( c, p, 5.0f );
            }
            break;
        default:
            break;
        }
    }

    bool IsThinKind( const sel_item_t &it )
    {
        return it.kind == SEL_OBJECT || it.kind == SEL_FACE;
    }

    // Fine selections do not populate legacy selected_brushes, so they need their
    // own accents. Objects retain the ported outline and are not redrawn here.
    void DrawSelectedAccents( const camera_s *c )
    {
        const selection_t &sel = KiwiSel();
        if ( sel.items.empty() )
            return;
        const sel_item_t active = KiwiSel().active;
        // When hover rendering is disabled, this pass also draws the active item.
        const bool activeDrawnElsewhere = KiwiUX_ShowHover();

        int budget = KSEL_MAX_SEGMENTS;
        for ( int pass = 0; pass < 2 && budget > 0; ++pass )
        {
            const bool thin = ( pass == 1 );        // THICK first (see the cap note)
            KiwiLines_Begin( budget, thin ? 1 : 2 );
            KiwiLines_Color( KSELECTED_COL[0], KSELECTED_COL[1], KSELECTED_COL[2] );

            for ( int i = (int)sel.items.size() - 1; i >= 0; --i )
            {
                const sel_item_t &it = sel.items[i];
                if ( it.kind == SEL_OBJECT )
                    continue;
                if ( IsThinKind( it ) != thin )
                    continue;
                if ( !BrushLive( it.brush ) )
                    continue;                       // never deref a freed node
                if ( KiwiLines_Remaining() <= 0 )
                    break;

                const bool isActive = Sel_ItemEqual( it, active );
                if ( isActive && activeDrawnElsewhere )
                    continue;                       // already drawn, brighter
                if ( UvEditorOwns( it ) )
                    continue;                       // UV editor owns this overlay
                KiwiLines_Color( isActive ? KACTIVE_COL[0] : KSELECTED_COL[0],
                                 isActive ? KACTIVE_COL[1] : KSELECTED_COL[1],
                                 isActive ? KACTIVE_COL[2] : KSELECTED_COL[2] );
                EmitItem( c, it, thin );
            }

            budget = KiwiLines_Remaining();          // what this pass left over
            KiwiLines_Flush();
        }
    }

    // Selected, active, then hovered faces share one MATERIAL_COLOR bracket in
    // overlay order. Selection fills remain visible when hover is disabled.
    void DrawFaceFills( const camera_s *c )
    {
        const selection_t &sel    = KiwiSel();
        const sel_item_t   active = sel.active;
        const bool activeIsFace   = ( active.kind == SEL_FACE ) && BrushLive( active.brush );

        const pick_result_t &hov = KiwiHover_Get();
        const bool hoverIsFace = KiwiUX_ShowHover() && hov.valid
                              && hov.item.kind == SEL_FACE && BrushLive( hov.item.brush );

        bool any = activeIsFace || hoverIsFace;
        for ( size_t i = 0; i < sel.items.size() && !any; ++i )
            any = ( sel.items[i].kind == SEL_FACE );
        if ( !any )
            return;

        // Match camwnd.cpp 0x408106: neutral lets vertex color drive the draw;
        // restore white so later passes do not inherit the bracket.
        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
        R_AddCmdSetMaterialColor( s_neutral );

        // Newest-first makes the fill cap drop the oldest faces.
        for ( int i = (int)sel.items.size() - 1; i >= 0; --i )
        {
            const sel_item_t &it = sel.items[i];
            if ( it.kind != SEL_FACE || !BrushLive( it.brush ) )
                continue;
            if ( activeIsFace && Sel_ItemEqual( it, active ) )
                continue;                        // drawn brighter, below
            if ( UvEditorOwns( it ) )
                continue;                        // UV editor owns this overlay
            EmitFaceFill( c, WindingOf( it ), KFILL_SELECTED );
        }
        // Selection yields to the UV canvas; hover remains as the next-click preview.
        // Ctrl-hover uses the existing warm remove/warning palette.
        if ( activeIsFace && !UvEditorOwns( active ) )
            EmitFaceFill( c, WindingOf( active ), KFILL_ACTIVE );
        if ( hoverIsFace )
            EmitFaceFill( c, WindingOf( hov.item ),
                          s_removePreview ? KFILL_ACTIVE : KFILL_HOVER );

        R_AddCmdSetMaterialColor( s_white );
    }

    // Bound row hover so a large entity cannot create hundreds of draw commands.
    enum { KHOVER_OUT_MAX = 96 };

    selbrush_t *s_outBrush[KHOVER_OUT_MAX];
    int         s_outCount    = 0;
    bool        s_outOverflow = false;
    int         s_outCon      = -1;
    int         s_outConGroup = 0;

    // Row hover uses shared cyan with stronger alpha for deliberate search feedback.
    const float KFILL_OUTLINER[4] = { 0.35f, 0.95f, 1.00f, 0.34f };

    // Selected brushes have tint plus wireframe at camwnd.cpp 0x4084f0; row hover
    // likewise adds a bounded width-2 outline over its fill.
    enum { KHOVER_OUT_SEGMENTS = 1200 };

    // Fill every brush face because an outline alone looks like ordinary selection.
    void EmitBrushFill( const camera_s *c, selbrush_t *b )
    {
        if ( !BrushLive( b ) )
            return;
        brush_t *def = b->def;
        if ( !def || !def->faces || def->faceCount <= 0 )
            return;
        // A patch def is its symbiont bounding brush (AddBrushForPatch), so patch
        // hover degrades to its box rather than an unbounded control net.
        for ( int f = 0; f < def->faceCount; ++f )
            EmitFaceFill( c, def->faces[f].w, KFILL_OUTLINER );
    }

    void DrawOutlinerHover( const camera_s *c )
    {
        if ( s_outCount <= 0 )
            return;

        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
        R_AddCmdSetMaterialColor( s_neutral );
        for ( int i = 0; i < s_outCount; ++i )
            EmitBrushFill( c, s_outBrush[i] );      // Sel_BrushLive-guarded inside
        R_AddCmdSetMaterialColor( s_white );

        // Close the fill bracket before drawing the outline over the wash. Its
        // separate budget safely drops the tail of oversized groups.
        KiwiLines_Begin( KHOVER_OUT_SEGMENTS, 2 );
        KiwiLines_Color( KHOVER_COL[0], KHOVER_COL[1], KHOVER_COL[2] );
        for ( int i = 0; i < s_outCount; ++i )
        {
            if ( KiwiLines_Remaining() <= 0 )
                break;
            if ( !BrushLive( s_outBrush[i] ) )
                continue;
            EmitObject( c, s_outBrush[i] );
        }
        KiwiLines_Flush();
    }
}

// Outliner hover latch.
void KiwiHover_OutlinerClear()
{
    s_outCount    = 0;
    s_outOverflow = false;
    s_outCon      = -1;
    s_outConGroup = 0;
}

void KiwiHover_OutlinerBrush( selbrush_t *b )
{
    if ( !b )
        return;
    if ( s_outCount >= KHOVER_OUT_MAX )
    {
        if ( !s_outOverflow )
        {
            s_outOverflow = true;
            Sys_Printf( "Outliner: that row owns more than %i brushes — the 3D "
                        "highlight shows the first %i.\n",
                        (int)KHOVER_OUT_MAX, (int)KHOVER_OUT_MAX );
        }
        return;
    }
    // Publish without dereferencing; draw-time liveness guards cover deletion
    // between the outliner and camera passes.
    s_outBrush[s_outCount++] = b;
    g_nUpdateBits |= 1;                 // the highlight has to cause a repaint
}

void KiwiHover_OutlinerEntity( entity_s *e )
{
    if ( !e )
        return;
    for ( selbrush_t *b = e->brushes.ownerNext; b && b != &e->brushes; b = b->ownerNext )
        KiwiHover_OutlinerBrush( b );
}

void KiwiHover_OutlinerCon( int conIndex )
{
    s_outCon = conIndex;
    g_nUpdateBits |= 1;
}

void KiwiHover_OutlinerConGroup( int conGroup )
{
    s_outConGroup = conGroup;
    g_nUpdateBits |= 1;
}

int KiwiHover_OutlinerConIndex()   { return s_outCon; }
int KiwiHover_OutlinerConGroupId() { return s_outConGroup; }

// Camera hover state.
void KiwiHover_Update( int imgX, int imgY, bool ctrl )
{
    s_hover          = pick_result_t();
    s_conHoverValid = false;
    s_regionHover   = -1;
    s_removePreview = false;
    if ( !KiwiUX_ShowHover() )
        return;

    // The sun glyph owns its pixel before any world ray, exactly as ClickSelect.
    if ( KiwiSun_GlyphHit( imgX, imgY ) )
    {
        s_removePreview = ctrl && KiwiSun_Selected();
        return;
    }

    ray_t ray;
    if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
        return;

    const sel_mask_t mask = KiwiSel_GetModeMask();
    pick_result_t hit = Pick( ray, mask );

    // Construction targets use the same point/line-over-area arbitration as the
    // click path.  Clearing the brush result when construction wins is important:
    // hover is a promise about the one item the ensuing click will affect.
    kconSelItem_t conItem;
    float         conDist = 0.0f;
    if ( KiwiConSel_PickAt( imgX, imgY, &conItem, &conDist ) )
    {
        const bool brushPointish = hit.valid
                                && ( hit.item.kind == SEL_VERTEX
                                  || hit.item.kind == SEL_EDGE );
        const bool conWins = !hit.valid || !brushPointish
                          || conDist < hit.screenDist;
        if ( conWins )
        {
            s_conHover       = conItem;
            s_conHoverValid  = true;
            s_removePreview  = ctrl && ConItemSelected( conItem );
            return;
        }
    }

    // Construction regions are area targets and yield to aimed-at points/lines or
    // a nearer brush face, mirroring ClickSelect byte for byte.
    if ( mask != SEL_MASK_OBJECT )
    {
        const int reg = KiwiRegion_PickAt( ray );
        if ( reg >= 0 )
        {
            const bool brushPointish = hit.valid
                                    && ( hit.item.kind == SEL_VERTEX
                                      || hit.item.kind == SEL_EDGE );
            bool regionWins = !brushPointish;
            float regDist = 0.0f;
            if ( regionWins && hit.valid
              && KiwiRegion_HitDistance( ray, reg, &regDist ) )
            {
                const float dx = hit.point[0] - ray.origin[0];
                const float dy = hit.point[1] - ray.origin[1];
                const float dz = hit.point[2] - ray.origin[2];
                if ( sqrtf( dx * dx + dy * dy + dz * dz ) < regDist )
                    regionWins = false;
            }
            if ( regionWins )
            {
                s_regionHover   = reg;
                s_removePreview = ctrl && KiwiRegion_IsSelected( reg );
                return;
            }
        }
    }

    s_hover = hit;
    if ( ctrl && hit.valid && Sel_ItemValid( hit.item ) )
        s_removePreview = Sel_Contains( KiwiSel(), hit.item );
}

void KiwiHover_Clear()
{
    s_hover          = pick_result_t();
    s_conHoverValid = false;
    s_regionHover   = -1;
    s_removePreview = false;
}

const pick_result_t &KiwiHover_Get()
{
    return s_hover;
}

bool KiwiHover_RemovePreview()
{
    return s_removePreview;
}

void KiwiHover_PreviewColor( bool remove, float outRgb[3] )
{
    if ( !outRgb )
        return;
    const float *col = remove ? KACTIVE_COL : KHOVER_COL;
    outRgb[0] = col[0];
    outRgb[1] = col[1];
    outRgb[2] = col[2];
}

// ─── Cam_Draw tail hook ──────────────────────────────────────────────────────
void KiwiHover_DrawWorld()
{
    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;
    CamWnd_BuildMatrix();

    // Grass Scatter is another cursor-driven camera accent.  It owns a separate,
    // exact-size line batch so the hover budget cannot truncate its AOE ring.
    KiwiGrass_DrawWorld();

    // Fills precede line accents so borders, edges, and vertices remain on top.
    // Every brush-backed emit is liveness-gated before dereference.
    if ( s_hover.valid && !BrushLive( s_hover.item.brush ) )
        KiwiHover_Clear();
    s_fillsThisFrame = 0;
    DrawFaceFills( c );
    // Outliner hover shares the fill cap but is independent of viewport-hover
    // visibility because the pointer is over a list row, not the camera image.
    DrawOutlinerHover( c );

    // Fine-selection accents remain visible when hover is disabled and run first
    // so active/hover feedback can overlay them.
    DrawSelectedAccents( c );

    if ( !KiwiUX_ShowHover() )
        return;

    DrawSpecialHover( c );

    // Drop a stale hover before dereference; a stale active is skipped this frame.
    if ( s_hover.valid && !BrushLive( s_hover.item.brush ) )
        KiwiHover_Clear();

    // Only fine active items need an accent; objects retain the ported outline.
    const sel_item_t active = KiwiSel().active;
    const bool activeFine   = Sel_ItemValid( active ) && active.kind != SEL_OBJECT
                           && BrushLive( active.brush );
    const bool hoverValid   = s_hover.valid && Sel_ItemValid( s_hover.item );

    // Object/face borders are thin; edge and vertex accents are thick.
    for ( int pass = 0; pass < 2; ++pass )
    {
        const bool thin = ( pass == 0 );
        KiwiLines_Begin( KHOVER_MAX_SEGMENTS, thin ? 1 : 2 );

        if ( activeFine && IsThinKind( active ) == thin )
        {
            KiwiLines_Color( KACTIVE_COL[0], KACTIVE_COL[1], KACTIVE_COL[2] );
            EmitItem( c, active, thin );
        }
        // Hover draws last for Shift visibility; Ctrl uses the warm removal warning.
        if ( hoverValid && IsThinKind( s_hover.item ) == thin )
        {
            const float *col = s_removePreview ? KACTIVE_COL : KHOVER_COL;
            KiwiLines_Color( col[0], col[1], col[2] );
            EmitItem( c, s_hover.item, thin );
        }

        KiwiLines_Flush();
    }
}

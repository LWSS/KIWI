#ifndef KISAK_RADIANT
#error this file is only for Radiant!
#endif
// ─────────────────────────────────────────────────────────────────────────────
// kiwi_hover.cpp — RADIANT_UX_DESIGN §18 implementation.  See kiwi_hover.h.
//
// ── WHY THE OUTLINE IS NUDGED TOWARD THE VIEWER ──────────────────────────────
// The hover outline traces the winding of a face that is ALREADY in the depth
// buffer, so a verbatim emit z-fights with it.  The ported terrain-paint ring
// solves the same problem the same way (sub_43ED50's -0.125 * camera.vpn nudge,
// documented at DrawAdvancedTerrainEditCircle) — this pass reuses that mechanism
// rather than inventing a no-depth emit.
//
// ── BUDGET ───────────────────────────────────────────────────────────────────
// ONE hovered item + ONE active item.  A brush object costs faceCount * winding
// points; KHOVER_MAX_SEGMENTS caps that, and a hovered PATCH object degrades to
// its bounding box (a 16x16 control net would be 480 segments on its own).
//
// ── SHAKEOUT G: A FACE IS A FILL, NOT AN OUTLINE ─────────────────────────────
// USER DIRECTIVE, verbatim: "When selecting a face on a brush, don't highlight
// the edges […] it's just the face."
//
// A face accent used to be EmitWinding — the face's own boundary, in the same
// line batch an EDGE accent uses — so "one face selected" and "the four edges
// around it selected" drew identically, and the user could not tell which one
// they had.  A face now draws as a TRANSLUCENT FILL over its winding instead:
// unmistakably an area, and unmistakably not an edge.
//
// The fill is a triangle FAN over the winding, emitted through the same
// R_AddRenderCmdDrawTris + MATERIAL_COLOR bracket kiwi_region.cpp's region fills
// use (kiwi_region.cpp:661-666 / :736-741, itself transcribed from the ported
// selected-face fill at camwnd.cpp 0x408106) — neutral MATERIAL_COLOR so the
// PER-VERTEX colour drives the draw, white again afterwards so no later pass
// inherits it.  A brush face is convex by construction (Brush_BuildWindings only
// ever emits the convex hull of the half-space intersection — kiwi_validity.h),
// so a fan from vertex 0 is exact; no triangulator is needed.
//
// The fan vertices carry the SAME nudge-toward-the-eye the outline pass has
// always used, at twice the distance (KHOVER_FILL_NUDGE 0.5 vs KHOVER_NUDGE
// 0.25).  They are coplanar with a face that is already in the depth buffer, so
// they z-fight it verbatim; the outline only ever had a one-pixel-wide sliver to
// lose to that, while a fill loses AREA, and the error term at grazing angles
// scales with how much of the polygon is far from the nudge's own reference —
// i.e. with the polygon, not with the line.  0.5 world units is still far under
// one grid cell at every modern spacing, so the fill never visibly floats.
// ─────────────────────────────────────────────────────────────────────────────

#include "stdafx.h"
#include "qe3.h"
#include "mainfrm.h"        // camera_s
#include "prefs.h"          // g_PrefsDlg (camera_fov — the screen-scaled marker)
#include <gfx_d3d/r_gfx.h>          // GfxColor
#include <gfx_d3d/r_material.h>     // Material
#include <gfx_d3d/r_rendercmds.h>   // MaterialTechniqueType, TECHNIQUE_UNLIT
#include "kiwi_camera.h"    // KiwiCam_WorldPerPixel (the shared screen-scale)
#include "kiwi_hover.h"
#include "kiwi_lines.h"
#include "kiwi_selection.h"
#include "kiwi_ux.h"
#include "kiwi_uveditor.h"  // KIWI-UX (ROUND BN, ITEM 3): KiwiUvEd_OverlaySuppressed
#include "kiwi_vec.h"     // KIWI-UX (CLEANUP, A-15): the one spelling of Dot3/Sub3/...

#include <math.h>
#include <string.h>
#include <stdint.h>

// ── ported entry points (verified against their definitions) ────────────────
extern camera_s   *Ed_Camera();          // camwnd.cpp
extern void        CamWnd_BuildMatrix(); // camwnd.cpp 0x403470
// (active_brushes is no longer referenced here: the liveness walk that needed it
//  moved to Sel_BrushLive in kiwi_selection.cpp — see BrushLive below.)
// SHAKEOUT G — the face-fill pass.  Same two entry points kiwi_region.cpp uses
// (kiwi_region.cpp:78 / :80-84); R_AddCmdSetMaterialColor comes from
// r_rendercmds.h, where it is declared __cdecl.
extern char  Byte4PackPixelColor( float *from, GfxColor *out );          // 0x402ac0
// ROUND AG, ITEM 3 — the outliner hover target needs a console line for its cap
// and a repaint request when it changes.
extern int   Sys_Printf( const char *fmt, ... );                        // win_qe3.cpp:112
extern int   g_nUpdateBits;                                             // engine_stubs.cpp:773
extern void  __cdecl R_AddRenderCmdDrawTris(
                 Material *material, MaterialTechniqueType techType, short indexCount,
                 const uint16_t *indices, short vertexCount,
                 const float ( *xyzw )[4], const float ( *normal )[3], float *color,
                 const float ( *st )[2] );                               // 0x4fd1c0

namespace
{
    enum { KHOVER_MAX_SEGMENTS = 220 };

    // ── shakeout D: the SELECTED-ITEM accent budget ─────────────────────────
    // Its own cap, on top of the hover/active pair's, because it is a different
    // question: how much of a MULTI-item fine selection may be drawn.
    //
    // THE DEGRADE RULE, stated once: the items are walked NEWEST-FIRST (the
    // selection vector is append-ordered, so index N-1 is the most recent
    // click), thick pass before thin, and emission simply stops when the cap is
    // reached.  So the segments that get dropped are always the OLDEST items in
    // the selection, and the edge/vertex accents — the ones the user is looking
    // at, and the only feedback a fine selection now has — outrank the face
    // outlines.  A face is up to MAX_POINTS_ON_WINDING segments on its own, so
    // 300 is roughly "40 edges plus a dozen faces", well past any selection a
    // person makes by hand and a fifth of the grid pass's own ceiling.
    enum { KSEL_MAX_SEGMENTS = 300 };

    // A cached selbrush_t* can go stale between the pick and the draw: delete,
    // undo and map close free brush nodes with no notification this layer sees
    // (RADIANT_KNOWN_ISSUES "UX overhaul").  Before any deref, require the node
    // to still be LINKED in one of the two display lists.
    // KIWI-UX (Phase 3): this WAS a private copy here; Phase 3's transform commands
    // need the identical guard for the brush pointers they cache across a gesture,
    // so the body moved to Sel_BrushLive (kiwi_selection.h) and this is now a thin
    // alias — one implementation, no drift.
    inline bool BrushLive( const selbrush_t *b ) { return Sel_BrushLive( b ); }

    const float KHOVER_NUDGE = 0.25f;      // world units toward the eye (see the note above)
    const float KHOVER_COL[3] = { 0.35f, 0.95f, 1.00f };   // light cyan
    const float KACTIVE_COL[3] = { 1.00f, 0.80f, 0.25f };  // warm accent
    // Shakeout D: the SELECTED-but-not-active accent.  The same hue as the active
    // one, dimmer, so "selected" and "active" read as one language with a clear
    // ordering rather than as two unrelated colours.
    const float KSELECTED_COL[3] = { 0.72f, 0.56f, 0.16f };

    // ── SHAKEOUT G: the FACE FILL pass (see the header note) ────────────────
    // The three states keep the EXACT hues the line accents used, so the change
    // is "a face is now an area" and not "faces changed colour".  The alphas are
    // the whole ordering: hover reads as a light wash, a selected face as a
    // definite tint, the active one as the strongest of the three.
    const float KHOVER_FILL_NUDGE = 0.50f;                            // see the header note
    // ── KIWI-UX (ROUND AI, ITEM 4): HOVER IS AS LOUD AS SELECTED ────────────
    // USER DIRECTIVE, verbatim: "The hover highlighting is too weak.  Should be
    // the same as when a brush is selected.  Fix it."
    //
    // WHAT "SELECTED" ACTUALLY MEASURES, so the target is a number and not a
    // feeling.  A selected BRUSH in the camera is TWO channels, not one
    // (camwnd.cpp:2755-2856):
    //   1. a MATERIAL_COLOR tint of `d_savedinfo.colors[11]` = {1, .25, .25, .25}
    //      (win_qe3.cpp:424) over the whole TEXTURED surface — vertcol_shaded
    //      lerps, so the result is 0.75*texture + 0.25*red; and
    //   2. a full WHITE WIREFRAME on top of it (the 0x4084f0 pass).
    // The ported selected-FACE fill is the same 0.25 as a straight alpha
    // (colors[16] = {1, .25, .25, .25}, win_qe3.cpp:429, drawn by
    // Cam_DrawSelectedFaceFill through the identical d_white/TECHNIQUE_UNLIT
    // route EmitFaceFill uses — so the alphas here and that 0.25 are directly
    // comparable quantities, not two different scales).
    //
    // THE HOVER WAS BELOW IT ON BOTH CHANNELS: 0.18 of wash against 0.25, and —
    // the bigger half — the OUTLINER hover (round AG) drew a fill and NO outline
    // at all, so it was missing an entire one of the two channels a selection
    // has.  Alpha alone could never have closed that; the alpha was only the
    // visible symptom.
    //
    // NOTHING ELSE DIMS IT.  Audited the whole path: Byte4PackPixelColor is a
    // straight float->byte pack (no scale), `s_color[i]` is the bit-cast packed
    // word the ported batcher also uses, the draw is d_white + TECHNIQUE_UNLIT
    // with no second colour term, and the MATERIAL_COLOR bracket around the pass
    // is the NEUTRAL {0,0,0,0} that hands the draw to the per-vertex colour.
    // There is no fade, no multiplier and no second alpha anywhere in it.
    const float KFILL_HOVER   [4] = { 0.35f, 0.95f, 1.00f, 0.26f };   // hover      — light cyan
    const float KFILL_SELECTED[4] = { 0.72f, 0.56f, 0.16f, 0.25f };   // selected   — amber, dim
    const float KFILL_ACTIVE  [4] = { 1.00f, 0.80f, 0.25f, 0.35f };   // active     — amber, bright

    // Windings above this are not filled — they fall back to the outline emit.
    // MAX_POINTS_ON_WINDING is 1024 (qe3.h:302) and R_AddRenderCmdDrawTris takes a
    // SHORT vertex count, so an unbounded fan would need a 1024-vertex static
    // buffer for a case brush faces never produce (a face of a convex solid is a
    // handful of vertices; only a patch's tessellation ever gets large, and
    // patches have no plane faces at all).
    enum { KHOVER_FILL_MAX_PTS = 64 };
    // …and a cap on how many faces one frame may fill, because each one is its own
    // draw command.  A hand-made face selection never approaches it; a Ctrl+3
    // conversion of a big multi-brush selection would.
    enum { KHOVER_FILL_MAX_FACES = 64 };

    int s_fillsThisFrame = 0;            // reset by KiwiHover_DrawWorld

    // ONE face, filled.  `rgba` is straight colour; the caller owns the
    // MATERIAL_COLOR bracket (opening it per face would be two extra commands per
    // face for a value that never changes inside the pass).
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
            // The nudge, at the fill distance: straight back along the view
            // normal, exactly as Nudge() does it for the outline.
            s_xyzw[i][0] = w->p[i][0] - c->vpn[0] * KHOVER_FILL_NUDGE;
            s_xyzw[i][1] = w->p[i][1] - c->vpn[1] * KHOVER_FILL_NUDGE;
            s_xyzw[i][2] = w->p[i][2] - c->vpn[2] * KHOVER_FILL_NUDGE;
            s_xyzw[i][3] = 1.0f;
            // KIWI-UX (ROUND AL, ITEM 1): the normal is a CONSTANT (world +Z),
            // not the view normal and not the face's.  The sentence that used to
            // sit here — "the fan is a screen-facing decal over the face,
            // TECHNIQUE_UNLIT ignores it" — is the claim round AL disproved:
            // TECHNIQUE_UNLIT resolves to vertcol_SHADED_tools and its vertex
            // stage modulates the vertex colour by a term computed from this
            // array, so `-vpn` made this tint's brightness a function of camera
            // pitch exactly as it did the region fill's.  kiwi_lines.h TRAP 4
            // carries the derivation and the binary's precedent.
            KiwiTris_FillNormal( s_normal[i] );
            s_st[i][0] = 0.0f;
            s_st[i][1] = 0.0f;
            s_color[i] = packedAsFloat;
        }

        // Fan from vertex 0 — exact for a convex winding, which every brush face
        // is by construction (kiwi_validity.h).
        int k = 0;
        for ( int i = 1; i + 1 < n; ++i )
        {
            s_idx[k++] = 0;
            s_idx[k++] = (uint16_t)i;
            s_idx[k++] = (uint16_t)( i + 1 );
        }

        // KIWI-UX (ROUND AA, ITEM 2): the fan order comes from the winding, which
        // Winding_BaseForPlane wound against the face's OUTWARD normal — so a face
        // fill on a face you are looking at from the inside (a hollowed room, a
        // see-through tool volume, a selected face you then orbited behind) was
        // culled away entirely.  kiwi_lines.h TRAP 3.
        KiwiTris_OrientToEye( &s_xyzw[0][0], 4, s_idx, k, c->origin );

        R_AddRenderCmdDrawTris( g_qeglobals.d_white, TECHNIQUE_UNLIT,
                                (short)k, s_idx, (short)n,
                                s_xyzw, s_normal, s_color, s_st );
    }

    pick_result_t s_hover;


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

    // ── KIWI-UX (ROUND BN, ITEM 3): "is the UV editor holding this item" ───────
    // One predicate for both KIWI overlay passes (the translucent face FILLS and
    // the wire ACCENTS), so they can never disagree about which shapes are hidden.
    // The whole scope rule is on kiwi_uveditor.h "IN SCOPE"; here it is only asked.
    // A SEL_FACE asks the per-face question; anything else asks the whole-object
    // one, which is what makes a gathered patch's accents stand down too.
    bool UvEditorOwns( const sel_item_t &it )
    {
        if ( !it.brush || !it.brush->def )
            return false;
        return KiwiUvEd_OverlaySuppressed( it.brush->def,
                                           ( it.kind == SEL_FACE ) ? it.faceIndex : -1 );
    }

    // KIWI-UX (CLEANUP, A-13 / C-49): the local EdgeEnds / VertexPos were two of
    // the four copies each; both resolutions now live in kiwi_selection.h.  The
    // call sites below pass checkLive = false because this file's draw loops
    // ALREADY gate on BrushLive before EmitItem (:514, and activeFine/hoverValid
    // for the other two entries) — asking again would be a second display-list
    // walk per selected item per frame.

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

    // A small screen-scaled marker in the camera's own basis: a square plus its
    // two diagonals (6 segments), so it reads at any orientation.
    //
    // KiwiCam_WorldPerPixel (kiwi_camera.h) is world units per screen pixel at the
    // depth of the point — the same per-pixel scale CameraCalcRayDir builds its ray
    // from, so a marker sized with it is constant on screen at any distance, and
    // the §14 gizmo and the truck-pan size themselves with the identical constant.
    // KIWI-UX (CLEANUP, C-39): called directly; the local `WorldPerPixel( c, ... )`
    // alias that kept shakeout A's call sites unchanged had one caller left and
    // discarded its own first parameter.
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

    // Draw one item at the given colour.  `width` picks the batch this belongs to,
    // so callers group by width (the batcher carries one width per Begin/Flush).
    void EmitItem( const camera_s *c, const sel_item_t &it, bool thin )
    {
        switch ( it.kind )
        {
        case SEL_OBJECT:
            if ( thin )
                EmitObject( c, it.brush );
            break;
        case SEL_FACE:
            // SHAKEOUT G: a face draws as a translucent FILL (DrawFaceFills), not
            // as its winding outline — see the header note.  Nothing was emitted
            // into the LINE batches for a face, which is the whole point of the
            // user directive: a face accent must not look like an edge one.
            //
            // ── KIWI-UX (ROUND AM, ITEM 2) — …AND THE BORDER COMES BACK ─────
            // USER REPORT, verbatim: "Need hover highlighting when hovering a
            // face in mode 3."  THE WIRING WAS ALREADY THERE and this is the
            // finding: KiwiHover_Update picks with the LIVE mode mask (:707), so
            // in mode 3 (SEL_MASK_FACE) the hover result IS a SEL_FACE on every
            // brush face, DrawFaceFills' `hoverIsFace` accepts it, and
            // EmitFaceFill emits the cyan tint for it — in mode 3 exactly as in
            // mode 5.  Nothing re-gated it.  What is missing is that the tint is
            // the face's ONLY channel, and this round found that channel is the
            // one that does not reach the screen (items 1, 2 and 4 are one
            // defect; kiwi_lines.h TRAP 5).
            //
            // So the face gets a SECOND channel, and it is the one round AI
            // already prescribed when the outliner hover read as too weak: "a
            // selected brush is a tint AND a white wireframe; this pass had only
            // the tint, which is the whole reason it read as weaker than a
            // selection no matter what the alpha said" (DrawOutlinerHover).  A
            // face is now a tint AND its border.
            //
            // SHAKEOUT G's DIRECTIVE IS NOT REOPENED, because the thing it banned
            // was a face reading like an EDGE.  An edge accent is ONE segment at
            // width 2 (IsThinKind puts SEL_EDGE in the thick batch); this is the
            // face's whole CLOSED border at width 1, which is what a face looks
            // like and what no edge can look like.  The two are in different
            // batches at different widths and cannot be confused.
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

    // ── shakeout D: draw EVERY selected fine item ───────────────────────────
    // WHY THIS EXISTS.  USER DIRECTIVE: "When selecting an edge, the entire
    // solid (Brush in this case) should not get selected as well, only the
    // edge/s."  Honouring that (kiwi_selection.cpp, the promotion removal) took
    // away the only feedback an edge or vertex selection had — the ported
    // tech-29 white brush outline, which the owner brush got because it was
    // promoted onto `selected_brushes`.  Without a replacement, an edge-only
    // selection would look like NOTHING is selected.  So the fine items draw
    // their own accent here.
    //
    // The ACTIVE item is deliberately skipped: the pass above already draws it,
    // brighter, and drawing it twice would just double the colour-run count.
    // SEL_OBJECT is skipped too — a selected brush still gets the ported
    // outline, and §18 says add accents, never re-render selected brushes.
    void DrawSelectedAccents( const camera_s *c )
    {
        const selection_t &sel = KiwiSel();
        if ( sel.items.empty() )
            return;
        const sel_item_t active = KiwiSel().active;
        // The hover pass below draws the active item itself — but only when the
        // hover toggle is ON.  With it off this pass is the only pass there is,
        // so it takes the active item over (at the ACTIVE colour, so the
        // "active is brighter" language survives the toggle).
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
                    continue;                       // ROUND BN, ITEM 3: in scope
                KiwiLines_Color( isActive ? KACTIVE_COL[0] : KSELECTED_COL[0],
                                 isActive ? KACTIVE_COL[1] : KSELECTED_COL[1],
                                 isActive ? KACTIVE_COL[2] : KSELECTED_COL[2] );
                EmitItem( c, it, thin );
            }

            budget = KiwiLines_Remaining();          // what this pass left over
            KiwiLines_Flush();
        }
    }

    // ── SHAKEOUT G: every face accent this frame, as translucent fills ──────
    // ONE pass for all three states, because they share one MATERIAL_COLOR
    // bracket and one draw-order question: selected faces first, then the active
    // one, then the hovered one, so where two coincide the stronger colour is the
    // one that ends up on top.
    //
    // This is deliberately NOT under the hover toggle as a whole: "Show hover
    // highlight" is a preference about the thing under the cursor, and since
    // shakeout D it is the only thing drawing a fine SELECTION.  Only the HOVER
    // face inside it is gated (the same split DrawSelectedAccents already makes).
    void DrawFaceFills( const camera_s *c )
    {
        const selection_t &sel    = KiwiSel();
        const sel_item_t   active = sel.active;
        const bool activeIsFace   = ( active.kind == SEL_FACE ) && BrushLive( active.brush );

        const pick_result_t &hov = KiwiHover_Get();
        const bool hoverIsFace = KiwiUX_ShowHover() && hov.valid
                              && hov.item.kind == SEL_FACE && BrushLive( hov.item.brush )
                              && !( activeIsFace && Sel_ItemEqual( hov.item, active ) );

        bool any = activeIsFace || hoverIsFace;
        for ( size_t i = 0; i < sel.items.size() && !any; ++i )
            any = ( sel.items[i].kind == SEL_FACE );
        if ( !any )
            return;

        // The ported selected-face fill's own bracket (camwnd.cpp 0x408106, and
        // kiwi_region.cpp:661-666 says the same): neutral MATERIAL_COLOR so the
        // per-vertex colour drives the draw, white again at the end so no later
        // pass inherits it.
        static const float s_neutral[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static const float s_white[4]   = { 1.0f, 1.0f, 1.0f, 1.0f };
        R_AddCmdSetMaterialColor( s_neutral );

        // Newest-first, the same degrade order DrawSelectedAccents uses: when the
        // face cap bites, the faces that lose their fill are the oldest ones.
        for ( int i = (int)sel.items.size() - 1; i >= 0; --i )
        {
            const sel_item_t &it = sel.items[i];
            if ( it.kind != SEL_FACE || !BrushLive( it.brush ) )
                continue;
            if ( activeIsFace && Sel_ItemEqual( it, active ) )
                continue;                        // drawn brighter, below
            if ( UvEditorOwns( it ) )
                continue;                        // ROUND BN, ITEM 3: in scope
            EmitFaceFill( c, WindingOf( it ), KFILL_SELECTED );
        }
        // The ACTIVE face and the HOVERED one take the same rule: the active face is
        // the UV canvas's background material, so leaving its fill on would tint the
        // very surface the user opened the editor to look at.
        if ( activeIsFace && !UvEditorOwns( active ) )
            EmitFaceFill( c, WindingOf( active ), KFILL_ACTIVE );
        if ( hoverIsFace && !UvEditorOwns( hov.item ) )
            EmitFaceFill( c, WindingOf( hov.item ), KFILL_HOVER );

        R_AddCmdSetMaterialColor( s_white );
    }

    // ═════════════════════════════════════════════════════════════════════════
    //  ROUND AG, ITEM 3 — the outliner's hover target (kiwi_hover.h)
    // ═════════════════════════════════════════════════════════════════════════
    // Bounded so one entity row cannot turn into hundreds of draw commands; see
    // the header for why the cap exists and what happens past it.
    enum { KHOVER_OUT_MAX = 96 };

    selbrush_t *s_outBrush[KHOVER_OUT_MAX];
    int         s_outCount    = 0;
    bool        s_outOverflow = false;
    int         s_outCon      = -1;
    int         s_outConGroup = 0;

    // The outliner hover reuses the RAYCAST hover's hue at a slightly stronger
    // alpha.  Stronger because a list hover is a deliberate search — the user is
    // looking for the thing, so it has to be findable across a busy scene — and
    // the SAME hue because §18 spends cyan on "the thing you are pointing at" and
    // pointing at a row is pointing at the object.
    // ROUND AI, ITEM 4: 0.26 -> 0.34, one step ABOVE the ported selection tint's
    // 0.25 rather than one step below it, for the same "deliberate search" reason.
    const float KFILL_OUTLINER[4] = { 0.35f, 0.95f, 1.00f, 0.34f };

    // ROUND AI, ITEM 4: the WIREFRAME channel the outliner hover never had.  A
    // selected brush is a tint AND a white wireframe; this pass had only the tint,
    // which is the whole reason it read as weaker than a selection no matter what
    // the alpha said.  Width 2 = the same weight DrawSelectedAccents gives its
    // "grab me" pass, so a hovered row is as findable across a busy scene as a
    // selection is.
    enum { KHOVER_OUT_SEGMENTS = 1200 };

    // Every face of one brush, filled.  Reuses the whole-brush walk EmitObject
    // does for the outline, but through the fill emitter — the directive is "the
    // hover-fill language", and an outline alone is what a SELECTED brush already
    // looks like.
    void EmitBrushFill( const camera_s *c, selbrush_t *b )
    {
        if ( !BrushLive( b ) )
            return;
        brush_t *def = b->def;
        if ( !def || !def->faces || def->faceCount <= 0 )
            return;
        // A PATCH's def is its symbiont BOUNDING BRUSH (pmesh.cpp
        // AddBrushForPatch), so this fills the patch's box — the same degrade
        // EmitObject already makes for a hovered patch, and for the same reason:
        // a 16x16 control net is not a highlight, it is a budget accident.
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

        // ── ROUND AI, ITEM 4: …AND THE WIREFRAME ────────────────────────────
        // AFTER the fill (and after its bracket is closed) so the outline lands ON
        // TOP of the wash rather than under it — the same ordering DrawFaceFills
        // and the hover line passes already keep.  Its own KiwiLines batch, its own
        // budget, and EmitObject is Remaining()-gated per face, so a hovered
        // 96-brush group degrades by dropping the tail of its outlines instead of
        // overflowing anything.
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

// ─── ROUND AG, ITEM 3: the outliner's hover target ───────────────────────────
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
    // NOT liveness-guarded here on purpose: a row can be published and the brush
    // deleted before the camera pass runs, so the guard that matters is the one
    // at DRAW time (EmitBrushFill).  See kiwi_hover.h THE CONTRACT.
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

// ─── hover state ─────────────────────────────────────────────────────────────
void KiwiHover_Update( int imgX, int imgY )
{
    if ( !KiwiUX_ShowHover() )
    {
        s_hover = pick_result_t();
        return;
    }
    ray_t ray;
    if ( !Pick_RayFromImagePos( imgX, imgY, &ray ) )
    {
        s_hover = pick_result_t();
        return;
    }
    s_hover = Pick( ray, KiwiSel_GetModeMask() );
}

void KiwiHover_Clear()
{
    s_hover = pick_result_t();
}

const pick_result_t &KiwiHover_Get()
{
    return s_hover;
}

// ─── Cam_Draw tail hook ──────────────────────────────────────────────────────
void KiwiHover_DrawWorld()
{
    camera_s *c = Ed_Camera();
    if ( c->width < 1 || c->height < 1 )
        return;
    CamWnd_BuildMatrix();

    // KIWI-UX (shakeout G): the FACE FILLS run FIRST, before either line pass, so
    // the edge / vertex accents and the hover outline land on top of them rather
    // than under them.  A stale hover is dropped below; a stale FILL cannot
    // survive the frame either, because every emit is BrushLive-gated.
    if ( s_hover.valid && !BrushLive( s_hover.item.brush ) )
        KiwiHover_Clear();
    s_fillsThisFrame = 0;
    DrawFaceFills( c );
    // ROUND AG, ITEM 3: the OUTLINER's hover, in the same pass and sharing the
    // same per-frame fill cap (s_fillsThisFrame) — so a hovered 96-brush group
    // degrades against the same ceiling every other fill answers to instead of
    // opening a second, unaccounted budget.  NOT under KiwiUX_ShowHover(): that
    // toggle is about the thing under the CURSOR in the viewport, and a list row
    // the user is deliberately pointing at is a different question.
    DrawOutlinerHover( c );

    // KIWI-UX (shakeout D): the SELECTION accent is NOT under the hover toggle.
    // "Show hover highlight" is a preference about the thing under the cursor;
    // since the promotion removal it is the only thing drawing an edge/vertex
    // selection at all, and turning hover off must not make a fine selection
    // invisible.  It runs first here, so the hover / active pair below still
    // draws over it.
    DrawSelectedAccents( c );

    if ( !KiwiUX_ShowHover() )
        return;

    // Liveness gate BEFORE any brush deref (see BrushLive above).  The stale
    // hover is dropped for good; a stale active is skipped for the frame (the
    // selection rebuild will replace it).
    if ( s_hover.valid && !BrushLive( s_hover.item.brush ) )
        KiwiHover_Clear();

    // The ACTIVE item, but only when it is FINER than a whole object: an active
    // object is already the tech-29 white outline the ported pass draws, and §18
    // says add accents, never re-render selected brushes.
    const sel_item_t active = KiwiSel().active;
    const bool activeFine   = Sel_ItemValid( active ) && active.kind != SEL_OBJECT
                           && BrushLive( active.brush );
    const bool hoverValid   = s_hover.valid && Sel_ItemValid( s_hover.item );
    const bool hoverIsActive = hoverValid && activeFine && Sel_ItemEqual( s_hover.item, active );

    // Two batches, one per line width.  Outlines (object/face) are thin; the edge
    // highlight and the vertex marker are thick so they read as "grab me".
    for ( int pass = 0; pass < 2; ++pass )
    {
        const bool thin = ( pass == 0 );
        KiwiLines_Begin( KHOVER_MAX_SEGMENTS, thin ? 1 : 2 );

        if ( activeFine && IsThinKind( active ) == thin )
        {
            KiwiLines_Color( KACTIVE_COL[0], KACTIVE_COL[1], KACTIVE_COL[2] );
            EmitItem( c, active, thin );
        }
        // Hover last so it wins where the two coincide (unless they ARE the same
        // item, in which case the active accent already said everything).
        if ( hoverValid && !hoverIsActive && IsThinKind( s_hover.item ) == thin )
        {
            KiwiLines_Color( KHOVER_COL[0], KHOVER_COL[1], KHOVER_COL[2] );
            EmitItem( c, s_hover.item, thin );
        }

        KiwiLines_Flush();
    }
}
